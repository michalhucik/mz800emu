/**
 * @file   videorec_setup_dialog.cpp
 * @brief  Dialog nastavení video záznamu (okno `###VideorecSetup`).
 *
 * Vzor snapshot_setup_dialog.cpp: při otevření se zkopíruje
 * g_videorec_settings do lokální kopie, OK ji aplikuje
 * (videorec_ui_settings_apply()), Cancel nebo zavření křížkem ji zahodí. Do
 * INI se nastavení dostane save callbacky modulu `[VIDEOREC]`
 * (videorec_config.c) při ukládání konfigurace. Změny platí od dalšího startu
 * nahrávání (běžící nahrávka si parametry zkopírovala při startu) - kromě
 * časové základny, která se po OK přepne hned (jen když na ni uživatel
 * v dialogu sáhl přepínačem nebo předvolbou; do té doby kopie sleduje
 * přepnutí za běhu přes Alt+U a OK ho nepřepíše).
 *
 * Sekce "Time base and real-time mode": předvolby (Gameplay showcase,
 * Live / tutorial), časová základna a všechny klíče režimu podle reality
 * z INI sekce `[VIDEOREC]` (realtime_pause, realtime_pause_cap_s,
 * realtime_speed, realtime_turbo_audio, record_debugger_steps, state_marks,
 * auto_markers).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/videorec/videorec_menu.h"

// Lokalizace
#include "i18n.h"

/** Lokální kopie nastavení pro editaci (aplikuje se po OK). */
static st_VIDEOREC_SETTINGS s_settings;
/** Lokální kopie je platná (dialog je otevřený a zkopíroval nastavení). */
static bool s_initialized = false;
/** Je otevřený výběr adresáře (IGFD). */
static bool s_dir_chooser_open = false;

/** Volby vzorkovací frekvence [Hz]. */
static const unsigned s_audio_rates[] = { 44100, 48000 };

/** Volitelné výchozí přechody (bez VIDEOREC_TRANS_NONE - ten patří jen prvnímu segmentu). */
static const en_VIDEOREC_TRANSITION s_transitions[] = {
    VIDEOREC_TRANS_CUT,
    VIDEOREC_TRANS_FADE,
    VIDEOREC_TRANS_CROSSFADE,
    VIDEOREC_TRANS_CARD,
};

/** Popisky přechodů (stejné pořadí jako s_transitions), anglické msgid. */
static const char *const s_transition_labels[] = {
    N_("Cut"),
    N_("Fade through black"),
    N_("Crossfade"),
    N_("Title card"),
};

/**
 * Uživatel v dialogu sáhl na časovou základnu (přepínač nebo předvolba).
 * Do té doby kopie sleduje aktuální hodnotu (videorec_ui_setup_sync_timebase()),
 * OK ji pak nemění (videorec_ui_settings_apply()).
 */
static bool s_timebase_touched = false;

/** Popisky `realtime_pause` (index = en_VIDEOREC_RT_PAUSE), anglické msgid. */
static const char *const s_rt_pause_labels[] = {
    N_("Skip (do not record the pause)"),
    N_("Frozen picture and silence"),
    N_("Frozen picture, at most the limit below"),
};

/** Popisky `realtime_speed` (index = en_VIDEOREC_RT_SPEED), anglické msgid. */
static const char *const s_rt_speed_labels[] = {
    N_("Record as seen (faster or slower)"),
    N_("Switch to emulated time"),
};

/** Popisky `realtime_turbo_audio` (index = en_VIDEOREC_TURBO_AUDIO), anglické msgid. */
static const char *const s_turbo_audio_labels[] = {
    N_("As heard"),
    N_("Silence"),
    N_("Attenuated (-12 dB)"),
};
static_assert(G_N_ELEMENTS(s_rt_pause_labels) == VIDEOREC_RT_PAUSE_FREEZE_CAPPED + 1, "realtime_pause labels out of sync");
static_assert(G_N_ELEMENTS(s_rt_speed_labels) == VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST + 1, "realtime_speed labels out of sync");
static_assert(G_N_ELEMENTS(s_turbo_audio_labels) == VIDEOREC_TURBO_AUDIO_ATTENUATE + 1, "turbo audio labels out of sync");

/**
 * @brief Combo nad výčtem s popisky (index = hodnota výčtu).
 * @param id     Stabilní ImGui ID (např. "##videorec_rt_pause").
 * @param labels Pole anglických msgid.
 * @param count  Počet položek.
 * @param value  Hodnota (vstup/výstup); mimo rozsah se zobrazí jako 0.
 * @return true při změně hodnoty.
 */
static bool vr_enum_combo(const char *id, const char *const *labels, int count, int *value)
{
    int cur = (*value >= 0 && *value < count) ? *value : 0;
    bool changed = false;
    if (ImGui::BeginCombo(id, _(labels[cur]))) {
        for (int i = 0; i < count; i++) {
            bool selected = (cur == i);
            if (ImGui::Selectable(_L(labels[i]), selected)) {
                *value = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

/**
 * @brief Sekce "Real-time mode" dialogu: předvolby, časová základna a nastavení režimu podle reality.
 *
 * Edituje lokální kopii s_settings (aplikuje se až po OK). Předvolba nastaví
 * jen své klíče, ostatní hodnoty zůstávají upravitelné.
 */
static void vr_realtime_section(void)
{
    ImGui::SeparatorText(_("Time base and real-time mode"));

    /* Předvolby */
    ImGui::Text("%s:", _("Preset"));
    for (int p = 0; p < VIDEOREC_UI_PRESET_COUNT; p++) {
        ImGui::SameLine();
        char label[128];
        g_snprintf(label, sizeof(label), "%s###videorec_preset_%d", _(videorec_ui_preset_msgid((en_VIDEOREC_UI_PRESET)p)), p);
        if (ImGui::Button(label)) {
            videorec_ui_apply_preset(&s_settings, (en_VIDEOREC_UI_PRESET)p);
            s_timebase_touched = true;
        }
    }
    const char *match = NULL;
    for (int p = 0; p < VIDEOREC_UI_PRESET_COUNT; p++) {
        if (videorec_ui_preset_matches(&s_settings, (en_VIDEOREC_UI_PRESET)p)) match = videorec_ui_preset_msgid((en_VIDEOREC_UI_PRESET)p);
    }
    if (match) {
        ImGui::TextDisabled(_("Current settings: %s"), _(match));
    } else {
        ImGui::TextDisabled("%s", _("Current settings: custom"));
    }
    if (videorec_ui_preset_matches(&s_settings, VIDEOREC_UI_PRESET_LIVE)) {
        ImGui::TextDisabled("%s", _("Export with --state-overlay icons to show pause, speed and real time in the video."));
    }
    ImGui::Spacing();

    /* Časová základna */
    ImGui::Text("%s:", _("Time base"));
    int tb = (s_settings.timebase == VIDEOREC_TIMEBASE_REALTIME) ? 1 : 0;
    if (ImGui::RadioButton(_L("Emulated time"), tb == 0)) {
        s_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
        s_timebase_touched = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(_L("Real time"), tb == 1)) {
        s_settings.timebase = VIDEOREC_TIMEBASE_REALTIME;
        s_timebase_touched = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE));
    ImGui::TextDisabled("%s", _("Time base switches on OK, also while recording."));
    ImGui::Spacing();

    /* Chování v režimu podle reality */
    ImGui::Text("%s:", _("Emulation paused (real time)"));
    int v = (int)s_settings.realtime_pause;
    if (vr_enum_combo("##videorec_rt_pause", s_rt_pause_labels, (int)G_N_ELEMENTS(s_rt_pause_labels), &v)) {
        s_settings.realtime_pause = (en_VIDEOREC_RT_PAUSE)v;
    }
    ImGui::Text("%s:", _("Frozen pause limit [s]"));
    int cap = (int)s_settings.realtime_pause_cap_s;
    if (cap < (int)VIDEOREC_RT_PAUSE_CAP_MIN_S || cap > (int)VIDEOREC_RT_PAUSE_CAP_MAX_S) cap = (int)VIDEOREC_RT_PAUSE_CAP_DEFAULT_S;
    ImGui::BeginDisabled(s_settings.realtime_pause != VIDEOREC_RT_PAUSE_FREEZE_CAPPED);
    if (ImGui::SliderInt("##videorec_rt_cap", &cap, (int)VIDEOREC_RT_PAUSE_CAP_MIN_S, (int)VIDEOREC_RT_PAUSE_CAP_MAX_S)) {
        s_settings.realtime_pause_cap_s = (unsigned)cap;
    }
    ImGui::EndDisabled();

    ImGui::Text("%s:", _("Speed other than normal (real time)"));
    v = (int)s_settings.realtime_speed;
    if (vr_enum_combo("##videorec_rt_speed", s_rt_speed_labels, (int)G_N_ELEMENTS(s_rt_speed_labels), &v)) {
        s_settings.realtime_speed = (en_VIDEOREC_RT_SPEED)v;
    }
    ImGui::Text("%s:", _("Sound when faster than normal (real time)"));
    v = (int)s_settings.realtime_turbo_audio;
    ImGui::BeginDisabled(s_settings.realtime_speed != VIDEOREC_RT_SPEED_AS_SEEN);
    if (vr_enum_combo("##videorec_rt_turbo", s_turbo_audio_labels, (int)G_N_ELEMENTS(s_turbo_audio_labels), &v)) {
        s_settings.realtime_turbo_audio = (en_VIDEOREC_TURBO_AUDIO)v;
    }
    ImGui::EndDisabled();

    ImGui::Checkbox(_L("Record debugger steps (real time)"), &s_settings.record_debugger_steps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Checked: stepping in the debugger is recorded as a frozen picture. "
                                  "Unchecked: stepping is left out of the recording."));
    }
    ImGui::Spacing();

    /* Značky a markery (obě základny) */
    bool marks = (s_settings.state_marks == VIDEOREC_STATE_MARKS_SIDECAR);
    if (ImGui::Checkbox(_L("Save state marks for export"), &marks)) {
        s_settings.state_marks = marks ? VIDEOREC_STATE_MARKS_SIDECAR : VIDEOREC_STATE_MARKS_NONE;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Writes pause, speed and time base changes into the cuts file; "
                                  "the export can show them as icons in the video."));
    }
    ImGui::Checkbox(_L("Automatic markers"), &s_settings.auto_markers);
    if (ImGui::IsItemHovered()) {
        /* Odpovídá lepidlu: "Reset" v obou základnách, Speed / Snapshot loaded / Pause (jen pauza
         * uživatele) jen v efektivní realtime (vr_observe_speed_reset(), vr_observe_pause(), vr_rt_snapshot()). */
        ImGui::SetTooltip("%s", _("Adds a marker (chapter) on reset; in real time also on a speed change, "
                                  "a snapshot load and a pause made by the user."));
    }
}

/**
 * @brief Index přechodu v s_transitions.
 * @param t Přechod.
 * @return Index, nebo index FADE pro přechod mimo seznam.
 */
static int vr_transition_index(en_VIDEOREC_TRANSITION t)
{
    for (int i = 0; i < (int)G_N_ELEMENTS(s_transitions); i++) {
        if (s_transitions[i] == t) return i;
    }
    return 1; /* FADE */
}

void imgui_videorec_setup_dialog(void)
{
    if (!g_gui->showVideorecSetupWindow) {
        s_initialized = false;
        return;
    }

    if (!s_initialized) {
        s_settings = g_videorec_settings;
        s_timebase_touched = false;
        s_initialized = true;
    }
    /* Nedotčená časová základna sleduje přepnutí za běhu (Alt+U, okno, MCP). */
    videorec_ui_setup_sync_timebase(&s_settings, s_timebase_touched);

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    char title[128];
    g_snprintf(title, sizeof(title), "%s###VideorecSetup", _("Video Recording Settings"));

    if (ImGui::Begin(title, &g_gui->showVideorecSetupWindow, flags)) {

        /* Výstupní adresář */
        ImGui::Text("%s:", _("Output folder"));
        float btn_width = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##videorec_dir", _("(default: videos folder next to the emulator)"),
                                 s_settings.output_dir, sizeof(s_settings.output_dir));
        ImGui::SameLine();
        if (ImGui::Button("...##videorec_dir_browse", ImVec2(btn_width, 0))) {
            IGFD::FileDialogConfig config;
            config.path = s_settings.output_dir[0] ? s_settings.output_dir : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            ImGuiFileDialog::Instance()->OpenDialog("VideorecDirChooser", _("Select Directory"), nullptr, config);
            s_dir_chooser_open = true;
        }
        ImGui::Spacing();

        /* Zvuk */
        ImGui::Text("%s:", _("Audio sample rate"));
        char rate_preview[32];
        g_snprintf(rate_preview, sizeof(rate_preview), "%u Hz", s_settings.audio_rate == 44100 ? 44100u : 48000u);
        if (ImGui::BeginCombo("##videorec_rate", rate_preview)) {
            for (size_t i = 0; i < G_N_ELEMENTS(s_audio_rates); i++) {
                char item[32];
                g_snprintf(item, sizeof(item), "%u Hz", s_audio_rates[i]);
                bool selected = (s_settings.audio_rate == s_audio_rates[i]);
                if (ImGui::Selectable(item, selected)) s_settings.audio_rate = s_audio_rates[i];
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();

        /* Snapshoty během nahrávání */
        ImGui::SeparatorText(_("Snapshot loaded while recording"));
        ImGui::Text("%s:", _("Retake mode"));
        int rm = (int)s_settings.retake_mode;
        if (rm < 0 || rm >= VIDEOREC_UI_RETAKE_MODES) rm = VIDEOREC_RETAKE_DISCARD;
        if (ImGui::BeginCombo("##videorec_retake", _(videorec_ui_retake_mode_msgid(rm)))) {
            for (int i = 0; i < VIDEOREC_UI_RETAKE_MODES; i++) {
                bool selected = (rm == i);
                if (ImGui::Selectable(_L(videorec_ui_retake_mode_msgid(i)), selected)) s_settings.retake_mode = (en_VIDEOREC_RETAKE)i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();

        /* Přechody (zapisují se do sidecaru, aplikuje je export) */
        ImGui::SeparatorText(_("Transitions"));
        ImGui::Text("%s:", _("Default transition"));
        int ti = vr_transition_index(s_settings.default_transition);
        if (ImGui::BeginCombo("##videorec_transition", _(s_transition_labels[ti]))) {
            for (int i = 0; i < (int)G_N_ELEMENTS(s_transitions); i++) {
                bool selected = (ti == i);
                if (ImGui::Selectable(_L(s_transition_labels[i]), selected)) s_settings.default_transition = s_transitions[i];
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Text("%s:", _("Transition length [ms]"));
        int ms = (int)s_settings.transition_ms;
        if (ImGui::SliderInt("##videorec_transition_ms", &ms, 0, 5000)) s_settings.transition_ms = (unsigned)ms;

        ImGui::Spacing();
        vr_realtime_section();

        ImGui::Spacing();
        ImGui::TextDisabled("%s", _("Changes apply to the next recording (the time base switches immediately)."));
        ImGui::Separator();
        ImGui::Spacing();

        /* OK / Cancel na střed */
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float offset = (ImGui::GetContentRegionAvail().x - total_btn_width) * 0.5f;
        if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
            videorec_ui_settings_apply(&s_settings, s_timebase_touched);
            g_gui->showVideorecSetupWindow = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            g_gui->showVideorecSetupWindow = false;
        }
    }
    ImGui::End();

    /* Zavření (OK, Cancel, křížek) - příště znovu zkopírovat aktuální nastavení. */
    if (!g_gui->showVideorecSetupWindow) s_initialized = false;

    /* Výběr adresáře (modální, nad dialogem) */
    if (s_dir_chooser_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("VideorecDirChooser")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                g_strlcpy(s_settings.output_dir, dir.c_str(), sizeof(s_settings.output_dir));
            }
            ImGuiFileDialog::Instance()->Close();
            s_dir_chooser_open = false;
        }
    }
}
