/**
 * @file   videorec_remote_window.cpp
 * @brief  Okno dálkového ovládání nahrávání videa (`###VideorecRemote`) a persistence jeho viditelnosti.
 *
 * Plovoucí okno s tlačítky Start/Stop, Pause/Resume a Add Marker (s volitelným
 * popiskem), u každého tlačítka je viditelně vypsaná klávesová zkratka - text
 * pochází z tabulky zkratek ve videorec_menu.cpp (videorec_ui_shortcut_label()),
 * takže se nemůže rozejít s obsluhou zkratek ani s menu. Dále ukazuje stav
 * nahrávání, čas a počet snímků videa, velikost souborů, aktuální segment,
 * časovou základnu (skutečnou, v realtime i činnost vzorkovače), poslední
 * událost, přepínač časové základny emulační čas / podle reality (s viditelnou
 * zkratkou Alt+U, platí hned i během nahrávání), rychlý přepínač režimu
 * retake a tlačítka Open Output Folder a Settings...
 *
 * Stav se čte jen přes videorec_get_status() (zveřejněná kopie pod listovým
 * zámkem), takže okno nikdy nečeká na emu vlákno. Akce volají stejné funkce
 * jako zkratky a menu (videorec_ui_*()).
 *
 * Viditelnost se ukládá do INI (sekce `[VIDEOREC_UI]`, klíč `remote_window`);
 * vzor dbg_stack_panel.cpp (propagate/save callbacky do g_gui) doplněný o
 * cache pro headless běh, kde g_gui je jen nulový stub.
 *
 * @par Vlákna
 * Vše volá UI (hlavní) vlákno; persistence i z inicializace videa (také
 * hlavní vlákno, před první ImGui frame).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <float.h>
#include <string.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/videorec/videorec_menu.h"

// Lokalizace
#include "i18n.h"

#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

/** Rozpracovaný popisek markeru (pole v okně); po vložení markeru se vymaže. */
static char s_marker_label[128] = "";

/** Viditelnost okna načtená z INI (cache do vzniku g_gui, a v headless trvale). */
static bool s_cfg_remote_open = false;
/** Cache byla aplikovaná do g_gui (GUI režim) - od té doby je zdrojem pravdy g_gui. */
static bool s_cfg_applied = false;
/** Modul `[VIDEOREC_UI]` je zaregistrovaný. */
static bool s_cfg_registered = false;

/*******************************************************************************
 *
 *                  Persistence viditelnosti
 *
 ******************************************************************************/

/**
 * @brief INI -> cache viditelnosti (případně rovnou do g_gui, pokud už byla aplikovaná).
 * @param e    Prvek `remote_window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void vr_cfg_propagate_remote_window(void *e, void *data)
{
    (void)data;
    s_cfg_remote_open = cfgelement_get_bool_value((st_CFGELEMENT *)e) ? true : false;
    if (s_cfg_applied && g_gui) g_gui->showVideorecRemoteWindow = s_cfg_remote_open;
}

/**
 * @brief Viditelnost -> INI: z g_gui po aplikaci cache, jinak hodnota cache (headless ji nepřepíše).
 * @param e    Prvek `remote_window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void vr_cfg_save_remote_window(void *e, void *data)
{
    (void)data;
    bool open = (s_cfg_applied && g_gui) ? g_gui->showVideorecRemoteWindow : s_cfg_remote_open;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, open ? 1 : 0);
}

void imgui_videorec_remote_cfg_init(void)
{
    if (s_cfg_registered) return;
    s_cfg_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)"VIDEOREC_UI");
    if (!cmod) return; /* duplicitní modul - okno pojede bez persistence */

    CFGELM *elm = cfgmodule_register_new_element(cmod, (char *)"remote_window", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, vr_cfg_propagate_remote_window, NULL);
    cfgelement_set_save_cb(elm, vr_cfg_save_remote_window, NULL);

    /* Globální propagace se v emulátoru nevolá - modul si sekci načte sám
     * (vzor videorec_config_init(), dbg_stack_panel_cfg_init()). */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void imgui_videorec_remote_apply_persisted(void)
{
    if (!g_gui) return;
    g_gui->showVideorecRemoteWindow = s_cfg_remote_open;
    s_cfg_applied = true;
}

/*******************************************************************************
 *
 *                  Okno
 *
 ******************************************************************************/

/**
 * @brief Řádek tabulky stavu: popisek v prvním sloupci, hodnota (text) ve druhém.
 * @param label Přeložený popisek.
 * @param value Hodnota (UTF-8).
 */
static void vr_status_row(const char *label, const char *value)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value);
}

/**
 * @brief Vykreslí viditelný text klávesové zkratky do druhého sloupce tabulky akcí.
 *
 * Text je mimo případný BeginDisabled tlačítka - zkratka je čitelná i tehdy,
 * když akce právě není dostupná.
 *
 * @param id Zkratka.
 */
static void vr_shortcut_hint(en_VIDEOREC_SHORTCUT id)
{
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(videorec_ui_shortcut_label(id));
}

void imgui_videorec_remote_window(void)
{
    if (!g_gui->showVideorecRemoteWindow) return;

    char title[128];
    g_snprintf(title, sizeof(title), "%s###VideorecRemote", _("Recording Remote Control"));
    /* První zobrazení: levý horní roh hlavního okna (souřadnice jsou absolutní -
     * při multi-viewport by pevná pozice skončila mimo hlavní okno). */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 40.0f, vp->WorkPos.y + 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &g_gui->showVideorecRemoteWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    st_VIDEOREC_UI_REMOTE_MODEL m = videorec_ui_remote_model(&st, videorec_ui_is_start_pending());
    bool active = (st.state != VIDEOREC_STATE_IDLE);

    /* Stav */
    if (ImGui::BeginTable("##vr_rc_status", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("State"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(m.state_rgba), "%s", _(m.state_label));

        char buf[128];
        videorec_ui_format_time(active ? st.frames : 0, videorec_get_fps(), buf, sizeof(buf));
        vr_status_row(_("Time"), buf);

        g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT, active ? st.frames : (uint64_t)0);
        vr_status_row(_("Video frames"), buf);

        if (active && st.bytes > 0) {
            char *sz = g_format_size(st.bytes);
            if (st.parts > 1) {
                g_snprintf(buf, sizeof(buf), _("%s (%u files)"), sz, st.parts);
            } else {
                g_strlcpy(buf, sz, sizeof(buf));
            }
            g_free(sz);
        } else {
            g_strlcpy(buf, "-", sizeof(buf));
        }
        vr_status_row(_("File size"), buf);

        if (active) {
            if (st.segment_open) {
                g_snprintf(buf, sizeof(buf), "%u", st.segment);
            } else {
                g_snprintf(buf, sizeof(buf), _("%u (paused)"), st.segment);
            }
        } else {
            g_strlcpy(buf, "-", sizeof(buf));
        }
        vr_status_row(_("Segment"), buf);

        vr_status_row(_("Time base"), _(videorec_ui_timebase_status_msgid(&st, active)));

        bool ev_error = false;
        const char *ev = videorec_ui_last_event(&ev_error);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("Last event"));
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
        if (!ev[0]) {
            ImGui::TextUnformatted("-");
        } else if (ev_error) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", ev);
        } else {
            ImGui::TextUnformatted(ev);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTable();
    }

    /* Akce: tlačítko + viditelná zkratka (ze sdílené tabulky zkratek). */
    ImGui::SeparatorText(_("Controls"));
    const ImGuiStyle &style = ImGui::GetStyle();
    const char *const widest[] = { m.actions.start_stop_label, N_("Start Recording"), N_("Stop Recording"),
                                   N_("Pause Recording"), N_("Resume Recording"), N_("Add Marker") };
    float btn_w = 0.0f;
    for (size_t i = 0; i < G_N_ELEMENTS(widest); i++) {
        float w = ImGui::CalcTextSize(_(widest[i])).x;
        if (w > btn_w) btn_w = w;
    }
    btn_w += style.FramePadding.x * 2.0f;

    if (ImGui::BeginTable("##vr_rc_actions", 2, ImGuiTableFlags_SizingFixedFit)) {
        char label[128];

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        g_snprintf(label, sizeof(label), "%s###vr_rc_start", _(m.actions.start_stop_label));
        ImGui::BeginDisabled(!m.actions.start_stop_enabled);
        if (ImGui::Button(label, ImVec2(btn_w, 0.0f))) videorec_ui_toggle_recording();
        ImGui::EndDisabled();
        vr_shortcut_hint(VIDEOREC_SHORTCUT_TOGGLE_RECORDING);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        g_snprintf(label, sizeof(label), "%s###vr_rc_pause", _(m.pause_label));
        ImGui::BeginDisabled(!m.actions.pause_enabled);
        if (ImGui::Button(label, ImVec2(btn_w, 0.0f))) videorec_ui_toggle_pause();
        ImGui::EndDisabled();
        vr_shortcut_hint(VIDEOREC_SHORTCUT_TOGGLE_PAUSE);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginDisabled(!m.actions.marker_enabled);
        bool add = ImGui::Button(_L("Add Marker"), ImVec2(btn_w, 0.0f));
        ImGui::EndDisabled();
        vr_shortcut_hint(VIDEOREC_SHORTCUT_ADD_MARKER);
        ImGui::EndTable();

        ImGui::BeginDisabled(!m.actions.marker_enabled);
        /* Celá šířka okna, aby se vešla nápověda (popisek "Marker N" pro prázdné pole). */
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##vr_rc_marker_label", _("Marker label (empty = Marker N)"), s_marker_label,
                                     sizeof(s_marker_label), ImGuiInputTextFlags_EnterReturnsTrue)) {
            add = true;
        }
        ImGui::EndDisabled();
        if (add && m.actions.marker_enabled) {
            videorec_ui_add_marker_label(s_marker_label);
            s_marker_label[0] = '\0';
        }
    }

    /* Časová základna: přepíná se hned, i během nahrávání (hranice segmentu);
     * bez nahrávání platí pro příští start. Zkratka viditelně vedle přepínače. */
    ImGui::SeparatorText(_("Time base"));
    {
        bool rt = (st.timebase == VIDEOREC_TIMEBASE_REALTIME);
        char label[128];
        g_snprintf(label, sizeof(label), "%s###vr_rc_tb_emu", _(videorec_ui_timebase_msgid(VIDEOREC_TIMEBASE_EMULATED)));
        if (ImGui::RadioButton(label, !rt) && rt) videorec_ui_set_timebase(VIDEOREC_TIMEBASE_EMULATED);
        ImGui::SameLine();
        g_snprintf(label, sizeof(label), "%s###vr_rc_tb_rt", _(videorec_ui_timebase_msgid(VIDEOREC_TIMEBASE_REALTIME)));
        if (ImGui::RadioButton(label, rt) && !rt) videorec_ui_set_timebase(VIDEOREC_TIMEBASE_REALTIME);
        ImGui::SameLine();
        ImGui::TextUnformatted(videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE));
        ImGui::TextDisabled("%s", _("Switches immediately, also while recording (starts a new segment)."));
    }

    /* Rychlé přepnutí režimu retake (zapisuje přímo g_videorec_settings, ukládá se do INI). */
    ImGui::SeparatorText(_("Retake mode"));
    int rm = (int)g_videorec_settings.retake_mode;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo("##vr_rc_retake", _(videorec_ui_retake_mode_msgid(rm)))) {
        for (int i = 0; i < VIDEOREC_UI_RETAKE_MODES; i++) {
            bool selected = (rm == i);
            if (ImGui::Selectable(_L(videorec_ui_retake_mode_msgid(i)), selected)) {
                g_videorec_settings.retake_mode = (en_VIDEOREC_RETAKE)i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%s", _("Applies from the next recording."));
    if (active && st.retake_mode != g_videorec_settings.retake_mode) {
        ImGui::TextDisabled(_("Current recording: %s"), _(videorec_ui_retake_mode_msgid((int)st.retake_mode)));
    }

    ImGui::Separator();
    if (ImGui::Button(_L("Open Output Folder"))) videorec_ui_open_output_folder();
    ImGui::SameLine();
    if (ImGui::Button(_L("Settings..."))) g_gui->showVideorecSetupWindow = true;

    ImGui::Separator();
    ImGui::TextDisabled(_("Show or hide this window: %s"), videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_REMOTE_WINDOW));

    ImGui::End();
}
