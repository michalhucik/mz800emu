/**
 * @file   videorec_menu.cpp
 * @brief  UI video záznamu: podmenu Tools -> Video Recording, akce zkratek
 *         (tabulka s_shortcuts: Alt+O, Alt+Shift+O, Alt+L, Alt+Shift+L, Alt+U),
 *         přepínač časové základny, předvolby nastavení, REC indikátor s režimem,
 *         notifikace událostí a sdílená logika okna dálkového ovládání.
 *
 * Viz videorec_menu.h. Jádro (videorec.c) je bez i18n: texty událostí
 * skládá tato vrstva z druhu události a hodnot (videorec_get_event()), takže
 * se překládá šablona (např. "Retake: rewound to %s"), ne celý text.
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/videorec/videorec_menu.h"

// Lokalizace
#include "i18n.h"

#include "emulator/emulator.h"
#include "iface/iface_audio.h"

/* Implementace v snapshot/snapshot_notification.cpp. */
extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title);
/* Implementace v snapshot/snapshot_notification.cpp: otevřené chybové okno. */
extern "C" bool snapshot_notification_error_active(void);

/**
 * Anglické texty chyb jádra bez proměnných částí - jen pro extrakci do .pot,
 * aby je šlo v notifikaci přeložit přes `_()`. Texty s cestou ("Cannot create
 * video file: <cesta>") se zobrazí anglicky.
 */
static const char *const s_core_error_msgids[] = {
    N_("Video recording is not initialized"),
    N_("Video recording is already running"),
    N_("Video encoder error"),
    N_("Cannot create video encoder"),
    N_("Video file write error"),
    N_("Video file write error (disk full?)"),
    N_("Video file truncate error"),
    N_("Cannot finalize the video file"),
    N_("Cannot write the cuts file (.cuts.json)"),
};

/**
 * Start přijat jádrem, ale ještě nezpracován. Maže se událostí STARTED nebo
 * FAILED, a také když jádro start už nečeká a nenahrává se
 * (imgui_videorec_poll_events()) - jinak by po zrušení čekajícího startu
 * mimo UI nebo po ztrátě události (přetečení kruhového bufferu) další Alt+O
 * nahrávání zastavovalo místo spouštělo.
 */
static bool s_start_pending = false;
/** Pořadové číslo naposledy zpracované události jádra. */
static uint32_t s_last_event_seq = 0;
/** Číslo posledního markeru v session `s_marker_session`. */
static unsigned s_marker_count = 0;
/** Session, ke které patří `s_marker_count`. */
static uint64_t s_marker_session = 0;
/** Text poslední události (přeložený, jako v notifikaci); "" = zatím žádná. */
static char s_last_event[1200] = "";
/** Poslední událost je chyba. */
static bool s_last_event_error = false;

/**
 * @brief Tabulka klávesových zkratek video záznamu (jediný zdroj pravdy).
 *
 * Řádek = en_VIDEOREC_SHORTCUT (stejné pořadí). Popisek musí odpovídat
 * klávese a Shiftu ve tvaru "Alt+[Shift+]<ImGui::GetKeyName()>" - hlídá to
 * test ui-functional (test_shortcut_labels_match_keys).
 *
 * Alt+U (časová základna) zvolena jako písmeno volné v celém UI: globální
 * zkratky (global_shortcuts.cpp) i okna debuggeru Alt+U ani Alt+Shift+U
 * nepoužívají; Alt+R koliduje s NVIDIA overlay, Alt+T je benchmark MAX SPEED.
 */
static const struct {
    ImGuiKey key;      /**< Klávesa (s Alt). */
    bool shift;        /**< Vyžaduje Shift (bez Shiftu zkratka nesmí reagovat a naopak). */
    const char *label; /**< Text zkratky pro menu a okno (nepřekládá se). */
} s_shortcuts[VIDEOREC_SHORTCUT_COUNT] = {
    { ImGuiKey_O, false, "Alt+O" },       /* VIDEOREC_SHORTCUT_TOGGLE_RECORDING */
    { ImGuiKey_O, true, "Alt+Shift+O" },  /* VIDEOREC_SHORTCUT_TOGGLE_PAUSE */
    { ImGuiKey_L, false, "Alt+L" },       /* VIDEOREC_SHORTCUT_ADD_MARKER */
    { ImGuiKey_L, true, "Alt+Shift+L" },  /* VIDEOREC_SHORTCUT_REMOTE_WINDOW */
    { ImGuiKey_U, false, "Alt+U" },       /* VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE */
};

/** Popisky režimů retake (index = en_VIDEOREC_RETAKE), anglické msgid. */
static const char *const s_retake_modes[] = {
    N_("Off"),
    N_("Discard frames (seamless)"),
    N_("Keep as cut with transition"),
};
static_assert(G_N_ELEMENTS(s_retake_modes) == VIDEOREC_UI_RETAKE_MODES, "retake mode labels out of sync");

/*******************************************************************************
 *
 *                  Čistá logika
 *
 ******************************************************************************/

st_VIDEOREC_UI_MENU_MODEL videorec_ui_menu_model(en_VIDEOREC_STATE state, bool start_pending)
{
    st_VIDEOREC_UI_MENU_MODEL m;
    memset(&m, 0, sizeof(m));
    bool active = (state != VIDEOREC_STATE_IDLE);
    m.start_stop_label = (active || start_pending) ? N_("Stop Recording") : N_("Start Recording");
    m.start_stop_enabled = true;
    m.pause_enabled = active;
    m.pause_checked = m.pause_enabled && (state == VIDEOREC_STATE_PAUSED);
    m.marker_enabled = m.pause_enabled;
    return m;
}

void videorec_ui_format_time(uint64_t frames, unsigned fps, char *buf, size_t size)
{
    uint64_t sec = frames / (fps ? fps : 1u);
    g_snprintf(buf, size, "%02u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
}

st_VIDEOREC_UI_REMOTE_MODEL videorec_ui_remote_model(const st_VIDEOREC_STATUS *st, bool start_pending)
{
    st_VIDEOREC_UI_REMOTE_MODEL m;
    memset(&m, 0, sizeof(m));
    en_VIDEOREC_STATE state = st->state;
    m.actions = videorec_ui_menu_model(state, start_pending);
    m.pause_label = (state == VIDEOREC_STATE_PAUSED) ? N_("Resume Recording") : N_("Pause Recording");
    switch (state) {
    case VIDEOREC_STATE_RECORDING:
        m.state_label = N_("REC");
        m.state_rgba = IM_COL32(230, 40, 40, 255);
        break;
    case VIDEOREC_STATE_PAUSED:
        m.state_label = N_("PAUSED");
        m.state_rgba = IM_COL32(255, 190, 40, 255);
        break;
    default:
        m.state_label = start_pending ? N_("STARTING") : N_("IDLE");
        m.state_rgba = start_pending ? IM_COL32(255, 140, 0, 255) : IM_COL32(150, 150, 150, 255);
        break;
    }
    return m;
}

const char *videorec_ui_shortcut_label(en_VIDEOREC_SHORTCUT id)
{
    if ((int)id < 0 || id >= VIDEOREC_SHORTCUT_COUNT) return "";
    return s_shortcuts[id].label;
}

void videorec_ui_shortcut_keys(en_VIDEOREC_SHORTCUT id, int *imgui_key, bool *shift)
{
    bool ok = ((int)id >= 0 && id < VIDEOREC_SHORTCUT_COUNT);
    *imgui_key = ok ? (int)s_shortcuts[id].key : 0;
    *shift = ok ? s_shortcuts[id].shift : false;
}

const char *videorec_ui_retake_mode_msgid(int mode)
{
    if (mode < 0 || mode >= (int)G_N_ELEMENTS(s_retake_modes)) mode = VIDEOREC_RETAKE_DISCARD;
    return s_retake_modes[mode];
}

const char *videorec_ui_timebase_msgid(en_VIDEOREC_TIMEBASE tb)
{
    return (tb == VIDEOREC_TIMEBASE_REALTIME) ? N_("Real time") : N_("Emulated time");
}

const char *videorec_ui_timebase_status_msgid(const st_VIDEOREC_STATUS *st, bool active)
{
    if (!active) return videorec_ui_timebase_msgid(st->timebase);
    if (st->timebase_effective == VIDEOREC_TIMEBASE_REALTIME) {
        switch (st->rt_activity) {
        case VIDEOREC_RT_ACTIVITY_LIVE:
            return N_("Real time (live)");
        case VIDEOREC_RT_ACTIVITY_FROZEN:
            return N_("Real time (frozen picture)");
        case VIDEOREC_RT_ACTIVITY_SKIPPING:
            return N_("Real time (not writing)");
        case VIDEOREC_RT_ACTIVITY_OFF:
            break;
        }
        return N_("Real time");
    }
    /* Požadovaný realtime zatím neplatí: přepínání (nejvýš jeden tick) nebo
     * emulated_when_fast při rychlosti != 100 %. */
    if (st->timebase == VIDEOREC_TIMEBASE_REALTIME) return N_("Emulated time (real time requested)");
    return N_("Emulated time");
}

void videorec_ui_overlay_text(const st_VIDEOREC_STATUS *st, char *buf, size_t size)
{
    char t[16];
    videorec_ui_format_time(st->frames, videorec_get_fps(), t, sizeof(t));
    const char *mode = (st->timebase_effective == VIDEOREC_TIMEBASE_REALTIME) ? _("real-time") : _("emulated");
    /* U+00B7 (střední tečka) je v rozsahu fontu UI (0x0020-0x024F). */
    g_snprintf(buf, size, "%s %s \xC2\xB7 %s", (st->state == VIDEOREC_STATE_PAUSED) ? _("PAUSE") : _("REC"), t, mode);
}

/** Popisky předvoleb (index = en_VIDEOREC_UI_PRESET), anglické msgid. */
static const char *const s_preset_names[VIDEOREC_UI_PRESET_COUNT] = {
    N_("Gameplay showcase"),
    N_("Live / tutorial"),
};

/**
 * @brief Hodnoty klíčů, které předvolba nastavuje (index = en_VIDEOREC_UI_PRESET).
 *
 * Podle tabulky V1.3: Gameplay showcase = emulated + skip + sidecar,
 * Live / tutorial = realtime + freeze_capped + sidecar.
 */
static const struct {
    en_VIDEOREC_TIMEBASE timebase;      /**< INI `timebase`. */
    en_VIDEOREC_RT_PAUSE realtime_pause; /**< INI `realtime_pause`. */
    en_VIDEOREC_STATE_MARKS state_marks; /**< INI `state_marks`. */
} s_presets[VIDEOREC_UI_PRESET_COUNT] = {
    { VIDEOREC_TIMEBASE_EMULATED, VIDEOREC_RT_PAUSE_SKIP, VIDEOREC_STATE_MARKS_SIDECAR },
    { VIDEOREC_TIMEBASE_REALTIME, VIDEOREC_RT_PAUSE_FREEZE_CAPPED, VIDEOREC_STATE_MARKS_SIDECAR },
};

void videorec_ui_apply_preset(st_VIDEOREC_SETTINGS *s, en_VIDEOREC_UI_PRESET p)
{
    if ((int)p < 0 || p >= VIDEOREC_UI_PRESET_COUNT) return;
    s->timebase = s_presets[p].timebase;
    s->realtime_pause = s_presets[p].realtime_pause;
    s->state_marks = s_presets[p].state_marks;
}

bool videorec_ui_preset_matches(const st_VIDEOREC_SETTINGS *s, en_VIDEOREC_UI_PRESET p)
{
    if ((int)p < 0 || p >= VIDEOREC_UI_PRESET_COUNT) return false;
    return s->timebase == s_presets[p].timebase && s->realtime_pause == s_presets[p].realtime_pause &&
           s->state_marks == s_presets[p].state_marks;
}

const char *videorec_ui_preset_msgid(en_VIDEOREC_UI_PRESET p)
{
    if ((int)p < 0 || p >= VIDEOREC_UI_PRESET_COUNT) return "";
    return s_preset_names[p];
}

bool videorec_ui_is_start_pending(void)
{
    return s_start_pending;
}

const char *videorec_ui_last_event(bool *is_error)
{
    if (is_error) *is_error = s_last_event_error;
    return s_last_event;
}

/**
 * @brief Zapamatuje text poslední události pro okno dálkového ovládání.
 * @param text     Přeložený text (zkopíruje se, zkrátí na velikost bufferu).
 * @param is_error Událost je chyba.
 */
static void vr_set_last_event(const char *text, bool is_error)
{
    g_strlcpy(s_last_event, text, sizeof(s_last_event));
    s_last_event_error = is_error;
}

/**
 * @brief Přeloží anglický text chyby jádra, pokud je mezi známými msgid.
 * @param text Anglický text.
 * @return Přeložený text, nebo `text` beze změny.
 */
static const char *vr_translate_core_text(const char *text)
{
    for (size_t i = 0; i < G_N_ELEMENTS(s_core_error_msgids); i++) {
        if (strcmp(text, s_core_error_msgids[i]) == 0) return _(s_core_error_msgids[i]);
    }
    return text;
}

/**
 * @brief Zobrazí chybovou notifikaci "Recording failed: <chyba>" a zapamatuje ji jako poslední událost.
 * @param error Anglický text chyby z jádra.
 */
static void vr_notify_failed(const char *error)
{
    char *msg = g_strdup_printf(_("Recording failed: %s"), vr_translate_core_text(error));
    snapshot_notification_show_ex(msg, true, _("Video Recording Error"));
    vr_set_last_event(msg, true);
    g_free(msg);
}

/*******************************************************************************
 *
 *                  Akce (menu + zkratky)
 *
 ******************************************************************************/

void videorec_ui_toggle_recording(void)
{
    if (videorec_get_state() != VIDEOREC_STATE_IDLE || s_start_pending) {
        videorec_request_stop(); /* čekající start zruší */
        s_start_pending = false;
        return;
    }

    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    iface_audio_build_videorec_levels(s.level, VIDEOREC_AUDIO_MAX_CHANNELS);
    if (!videorec_request_start(&s)) {
        vr_notify_failed(videorec_get_last_error());
        return;
    }
    s_start_pending = true;
    if (EMULATOR_TEST_PAUSED) {
        snapshot_notification_show_ex(_("Recording will start when emulation resumes"), false, NULL);
    }
}

void videorec_ui_toggle_pause(void)
{
    videorec_request_pause_toggle();
}

void videorec_ui_add_marker(void)
{
    videorec_ui_add_marker_label(NULL);
}

void videorec_ui_add_marker_label(const char *label)
{
    if (videorec_get_state() == VIDEOREC_STATE_IDLE) return;
    uint64_t session = videorec_get_session_id();
    if (session != s_marker_session) {
        s_marker_session = session;
        s_marker_count = 0;
    }
    s_marker_count++;
    if (label && label[0]) {
        videorec_request_marker(label);
        return;
    }
    /* Výchozí popisek jde do sidecaru (strojově čtený soubor) - záměrně anglicky, bez překladu. */
    char def[32];
    g_snprintf(def, sizeof(def), "Marker %u", s_marker_count);
    videorec_request_marker(def);
}

void videorec_ui_set_timebase(en_VIDEOREC_TIMEBASE tb)
{
    videorec_request_timebase(tb);
    char *msg = g_strdup_printf(_("Recording time base: %s"), _(videorec_ui_timebase_msgid(tb)));
    vr_set_last_event(msg, false);
    if (!snapshot_notification_error_active()) snapshot_notification_show_ex(msg, false, NULL);
    g_free(msg);
}

void videorec_ui_toggle_timebase(void)
{
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    videorec_ui_set_timebase((st.timebase == VIDEOREC_TIMEBASE_REALTIME) ? VIDEOREC_TIMEBASE_EMULATED
                                                                          : VIDEOREC_TIMEBASE_REALTIME);
}

void videorec_ui_settings_apply(const st_VIDEOREC_SETTINGS *edited, bool timebase_touched)
{
    en_VIDEOREC_TIMEBASE current = g_videorec_settings.timebase;
    g_videorec_settings = *edited;
    g_videorec_settings.timebase = current;
    if (timebase_touched && edited->timebase != current) videorec_ui_set_timebase(edited->timebase);
}

void videorec_ui_setup_sync_timebase(st_VIDEOREC_SETTINGS *edited, bool timebase_touched)
{
    if (!timebase_touched) edited->timebase = g_videorec_settings.timebase;
}

void imgui_videorec_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyAlt) return;
    for (int i = 0; i < VIDEOREC_SHORTCUT_COUNT; i++) {
        if (io.KeyShift != s_shortcuts[i].shift || !ImGui::IsKeyPressed(s_shortcuts[i].key, false)) continue;
        switch ((en_VIDEOREC_SHORTCUT)i) {
        case VIDEOREC_SHORTCUT_TOGGLE_RECORDING:
            videorec_ui_toggle_recording();
            break;
        case VIDEOREC_SHORTCUT_TOGGLE_PAUSE:
            videorec_ui_toggle_pause();
            break;
        case VIDEOREC_SHORTCUT_ADD_MARKER:
            videorec_ui_add_marker();
            break;
        case VIDEOREC_SHORTCUT_REMOTE_WINDOW:
            g_gui->showVideorecRemoteWindow = !g_gui->showVideorecRemoteWindow;
            break;
        case VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE:
            videorec_ui_toggle_timebase();
            break;
        case VIDEOREC_SHORTCUT_COUNT:
            break;
        }
    }
}

/*******************************************************************************
 *
 *                  Menu
 *
 ******************************************************************************/

void videorec_ui_open_output_folder(void)
{
    char dir[1024];
    bool ok = videorec_resolve_output_dir(dir, sizeof(dir));
    char *abs = g_canonicalize_filename(dir, NULL);
    char *uri = ok ? g_filename_to_uri(abs, NULL, NULL) : NULL;
    if (!uri || !SDL_OpenURL(uri)) {
        char *msg = g_strdup_printf(_("Cannot open folder: %s"), abs);
        snapshot_notification_show_ex(msg, true, _("Video Recording Error"));
        g_free(msg);
    }
    g_free(uri);
    g_free(abs);
}

void imgui_videorec_menu(void)
{
    if (!ImGui::BeginMenu(_L("Video Recording"))) return;

    st_VIDEOREC_UI_MENU_MODEL m = videorec_ui_menu_model(videorec_get_state(), s_start_pending);

    if (ImGui::MenuItem(_L(m.start_stop_label), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_RECORDING), false, m.start_stop_enabled)) {
        videorec_ui_toggle_recording();
    }

    if (ImGui::MenuItem(_L("Pause Recording"), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_PAUSE), m.pause_checked, m.pause_enabled)) {
        videorec_ui_toggle_pause();
    }

    if (ImGui::MenuItem(_L("Add Marker"), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_ADD_MARKER), false, m.marker_enabled)) {
        videorec_ui_add_marker();
    }

    /* Časová základna: přepíná se kdykoli (bez nahrávání = pro příští start). */
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    if (ImGui::MenuItem(_L("Record in Real Time"), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE),
                        st.timebase == VIDEOREC_TIMEBASE_REALTIME)) {
        videorec_ui_toggle_timebase();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Checked: the recording follows real time (what was on the screen and in the "
                                  "speakers). Unchecked: emulated time (one emulated frame = one video frame)."));
    }

    ImGui::Separator();

    if (ImGui::MenuItem(_L("Settings..."))) {
        g_gui->showVideorecSetupWindow = true;
    }

    if (ImGui::MenuItem(_L("Open Output Folder"))) {
        videorec_ui_open_output_folder();
    }

    if (ImGui::MenuItem(_L("Remote Control..."), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_REMOTE_WINDOW),
                        g_gui->showVideorecRemoteWindow)) {
        g_gui->showVideorecRemoteWindow = !g_gui->showVideorecRemoteWindow;
    }

    ImGui::EndMenu();
}

/*******************************************************************************
 *
 *                  REC indikátor
 *
 ******************************************************************************/

void imgui_videorec_overlay(void)
{
    st_VIDEOREC_STATUS status;
    videorec_get_status(&status);
    en_VIDEOREC_STATE st = status.state;
    if (st == VIDEOREC_STATE_IDLE) return;

    ImVec2 img_min = ImGui::GetItemRectMin();
    ImVec2 img_max = ImGui::GetItemRectMax();

    char text[128];
    videorec_ui_overlay_text(&status, text, sizeof(text));

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float pad = 6.0f;
    const float margin = 10.0f;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.4f;
    float w = pad + 2.0f * r + pad + ts.x + pad;
    float h = ts.y + 2.0f * pad;
    ImVec2 box_min(img_max.x - margin - w, img_min.y + margin);
    ImVec2 box_max(img_max.x - margin, img_min.y + margin + h);

    dl->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    ImVec2 c(box_min.x + pad + r, box_min.y + h * 0.5f);
    if (st == VIDEOREC_STATE_PAUSED) {
        /* pauza: dva svislé pruhy */
        float bw = r * 0.6f;
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r + bw, c.y + r), IM_COL32(255, 190, 40, 255));
        dl->AddRectFilled(ImVec2(c.x + r - bw, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(255, 190, 40, 255));
    } else {
        dl->AddCircleFilled(c, r, IM_COL32(230, 30, 30, 255));
    }
    dl->AddText(ImVec2(c.x + r + pad, box_min.y + pad), IM_COL32(255, 255, 255, 255), text);
}

/*******************************************************************************
 *
 *                  Notifikace událostí
 *
 ******************************************************************************/

void imgui_videorec_poll_events(void)
{
    uint32_t seq = videorec_get_event_seq();
    bool error_shown = false;

    while (s_last_event_seq != seq) {
        s_last_event_seq++;
        st_VIDEOREC_EVENT e;
        if (!videorec_get_event(s_last_event_seq, &e)) continue; /* přepsaná (příliš stará) událost */

        char *msg = NULL;
        char t[16];
        switch (e.kind) {
        case VIDEOREC_EVENT_STARTED:
            s_start_pending = false;
            msg = g_strdup(_("Recording started"));
            break;
        case VIDEOREC_EVENT_SAVED:
            msg = g_strdup_printf(_("Recording saved: %s"), e.path);
            break;
        case VIDEOREC_EVENT_FAILED:
            s_start_pending = false;
            vr_notify_failed(e.text);
            error_shown = true;
            break;
        case VIDEOREC_EVENT_RETAKE:
            videorec_ui_format_time(e.frame, videorec_get_fps(), t, sizeof(t));
            msg = g_strdup_printf(_("Retake: rewound to %s"), t);
            break;
        case VIDEOREC_EVENT_SEAM:
            msg = g_strdup(_("Recording seam (snapshot loaded)"));
            break;
        case VIDEOREC_EVENT_NONE:
            break;
        }
        if (msg) vr_set_last_event(msg, false);
        /* Chybové okno nepřepisovat běžnou notifikací - ani ze stejné dávky,
         * ani dokud ho uživatel nezavřel. */
        if (msg && !error_shown && !snapshot_notification_error_active()) {
            snapshot_notification_show_ex(msg, false, NULL);
        }
        g_free(msg);
    }

    /* Čekající start zanikl bez události STARTED (zrušen mimo UI, nebo událost
     * přepsaná): pořadí čtení (nejdřív "čeká start", pak stav) zaručí, že se
     * nesmaže start, který se mezi oběma čteními právě zpracoval. */
    if (s_start_pending && !videorec_is_start_pending() && videorec_get_state() == VIDEOREC_STATE_IDLE) {
        s_start_pending = false;
    }
}
