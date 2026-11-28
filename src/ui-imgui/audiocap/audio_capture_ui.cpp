/**
 * @file   audio_capture_ui.cpp
 * @brief  Zachycení zvuku z GUI (viz audio_capture_ui.h).
 *
 * Okno Audio Capture Control (`###AudioCaptureRemote`, Alt+Shift+Q) je
 * obdoba dálkového ovládání nahrávání videa: stav, délka, složka, soubory
 * k zápisu, počty zapsaných řádků a vzorků, poslední výsledek a tlačítka
 * s viditelnými zkratkami. Viditelnost se ukládá do INI (sekce
 * `[AUDIOCAP_UI]`, klíč `window`, vzor videorec_remote_window.cpp včetně
 * cache pro headless).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <SDL3/SDL.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "mzarch/mzarch_config.h"
#include "gdg/video.h"
#include "hw-generic/gdg/gdgclk.h"
#include "ui-imgui/audiocap/audio_capture_ui.h"
#include "emulator/audio_capture.h"
#include "emulator/screenshot/screenshot_series.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include "i18n.h"

/* Implementace v snapshot/snapshot_notification.cpp. */
extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title);

/** @brief Zobrazovaný popisek zkratky startu / stopu. */
#define AUDIO_CAPTURE_SHORTCUT_LABEL "Alt+Q"
/** @brief Zobrazovaný popisek zkratky okna Audio Capture Control. */
#define AUDIO_CAPTURE_WINDOW_SHORTCUT_LABEL "Alt+Shift+Q"

/** @brief GUI spustilo zachycení a ještě neohlásilo jeho konec. */
static bool s_pending_report = false;
/** @brief Dialog nastavení je otevřený. */
static bool s_setup_open = false;


/** @brief Délka zachycení ve skutečných sekundách (snímky podle krystalu, PAL 50,036 za sekundu). */
static double ac_ui_seconds(uint64_t frames)
{
    return (double) frames / GDGCLK_REAL_FPS;
}


/** @brief Otevře výstupní složku zachycení v systémovém prohlížeči souborů. */
static void ac_ui_open_output_folder(void)
{
    char dir[AUDIO_CAPTURE_PATH_MAX];
    audio_capture_resolve_output_dir(dir, sizeof(dir));
    char *abs = g_canonicalize_filename(dir, NULL);
    char *uri = g_filename_to_uri(abs, NULL, NULL);
    if (!uri || !SDL_OpenURL(uri)) {
        char *msg = g_strdup_printf(_("Cannot open folder: %s"), abs);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    }
    g_free(uri);
    g_free(abs);
}


void audio_capture_ui_toggle(void)
{
    if (audio_capture_stop()) return; /* běželo - konec ohlásí imgui_audio_capture_poll() */

    st_AUDIO_CAPTURE_PARAMS p;
    audio_capture_params_from_settings(&p);
    char err[AUDIO_CAPTURE_TEXT_MAX] = "";
    if (audio_capture_start(&p, err, sizeof(err))) {
        s_pending_report = true;
    } else {
        char *msg = g_strdup_printf(_("Cannot start the audio capture: %s"), err);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    }
}


void imgui_audio_capture_menu(void)
{
    if (!ImGui::BeginMenu(_L("Audio Capture"))) return;
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (ImGui::MenuItem(_L("Record Audio Capture"), AUDIO_CAPTURE_SHORTCUT_LABEL, st.active || st.pending)) {
        audio_capture_ui_toggle();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Records what the sound hardware does into a new folder until you stop it:\n"
                                  "changes of the channel levels, PSG writes, the exact sound (reference.wav)\n"
                                  "and the sound you hear (sdl.wav). For finding sound bugs."));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(_L("Audio Capture Control..."), AUDIO_CAPTURE_WINDOW_SHORTCUT_LABEL,
                        g_gui->showAudioCaptureWindow)) {
        g_gui->showAudioCaptureWindow = !g_gui->showAudioCaptureWindow;
    }
    if (ImGui::MenuItem(_L("Settings..."))) {
        s_setup_open = true;
    }
    if (ImGui::MenuItem(_L("Open Output Folder"))) {
        ac_ui_open_output_folder();
    }
    ImGui::EndMenu();
}


void imgui_audio_capture_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyAlt || io.KeyCtrl || !ImGui::IsKeyPressed(ImGuiKey_Q, false)) return;
    if (io.KeyShift) {
        g_gui->showAudioCaptureWindow = !g_gui->showAudioCaptureWindow;
    } else {
        audio_capture_ui_toggle();
    }
}


void imgui_audio_capture_poll(void)
{
    if (!s_pending_report) return;
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (st.active || st.pending) return;
    s_pending_report = false;
    if (st.last_error[0]) {
        char *msg = g_strdup_printf(_("Audio capture stopped after %.1f s: %s"), ac_ui_seconds(st.frames), st.last_error);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    } else if (strcmp(st.stop_reason, "Stopped") != 0 && strcmp(st.stop_reason, "Emulator exit") != 0) {
        /* Konec bez stisku stop: reset nebo snapshot (skok času), pojistka. */
        char *msg = g_strdup_printf(_("Audio capture ended (%s): %.1f s saved to %s"),
                                    st.stop_reason, ac_ui_seconds(st.frames), st.dir);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf(_("Audio capture saved: %.1f s to %s"), ac_ui_seconds(st.frames), st.dir);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    }
}


void imgui_audio_capture_overlay(void)
{
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (!st.active && !st.pending) return;

    char text[128];
    if (st.active) {
        g_snprintf(text, sizeof(text), _("AUDIO %.1f s"), ac_ui_seconds(st.frames));
    } else {
        g_snprintf(text, sizeof(text), "%s", _("AUDIO starting"));
    }

    ImVec2 img_min = ImGui::GetItemRectMin();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float pad = 6.0f;
    const float margin = 10.0f;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.4f;
    float w = pad + 2.0f * r + pad + ts.x + pad;
    float h = ts.y + 2.0f * pad;
    /* Pod indikátorem série snímků, pokud běží (stejná výška boxu). */
    st_SCREENSHOT_SERIES_STATUS ss;
    screenshot_series_get_status(&ss);
    float y0 = img_min.y + margin + (ss.active ? h + margin * 0.5f : 0.0f);
    ImVec2 box_min(img_min.x + margin, y0);
    ImVec2 box_max(img_min.x + margin + w, y0 + h);
    dl->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    /* Symbol: tři svislé pruhy (úrovně zvuku). */
    ImVec2 c(box_min.x + pad + r, box_min.y + h * 0.5f);
    ImU32 col = st.active ? IM_COL32(120, 230, 120, 255) : IM_COL32(200, 200, 200, 255);
    float bw = r * 0.5f;
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.3f), ImVec2(c.x - r + bw, c.y + r), col);
    dl->AddRectFilled(ImVec2(c.x - bw * 0.5f, c.y - r), ImVec2(c.x + bw * 0.5f, c.y + r), col);
    dl->AddRectFilled(ImVec2(c.x + r - bw, c.y - r * 0.6f), ImVec2(c.x + r, c.y + r), col);
    dl->AddText(ImVec2(c.x + r + pad, box_min.y + pad), IM_COL32(255, 255, 255, 255), text);
}


void imgui_audio_capture_setup_dialog(void)
{
    static bool s_initialized = false;
    static bool s_dir_chooser_open = false;
    static st_AUDIO_CAPTURE_SETTINGS s_settings;
    if (!s_setup_open) {
        s_initialized = false;
        return;
    }
    if (!s_initialized) {
        s_settings = g_audio_capture_settings;
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    char title[128];
    g_snprintf(title, sizeof(title), "%s###AudioCaptureSetup", _("Audio Capture Settings"));

    if (ImGui::Begin(title, &s_setup_open, flags)) {
        ImGui::Text("%s:", _("Files to write"));
        ImGui::Checkbox(_L("events.csv - changes of the channel levels"), &s_settings.events);
        ImGui::Checkbox(_L("psg_writes.csv - PSG writes, decoded"), &s_settings.psg_writes);
        ImGui::Checkbox(_L("reference.wav - exact sound in emulated time"), &s_settings.reference_wav);
        ImGui::Checkbox(_L("sdl.wav - sound of the audio output (what you hear)"), &s_settings.sdl_wav);
        ImGui::Spacing();

        /* Výstupní složka */
        ImGui::Text("%s:", _("Output folder"));
        float btn_width = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##audio_capture_dir", _("(default: audio_captures folder next to the emulator)"),
                                 s_settings.output_dir, sizeof(s_settings.output_dir));
        ImGui::SameLine();
        if (ImGui::Button("...##audio_capture_dir_browse", ImVec2(btn_width, 0))) {
            IGFD::FileDialogConfig config;
            config.path = s_settings.output_dir[0] ? s_settings.output_dir : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_AUDIO_CAPTURE, config);
            ImGuiFileDialog::Instance()->OpenDialog("AudioCaptureDirChooser", _("Select Directory"), nullptr, config);
            s_dir_chooser_open = true;
        }
        ImGui::TextDisabled("%s", _("Each capture creates a new folder in the output folder."));
        ImGui::TextDisabled(_("Record Audio Capture (%s) starts and stops the capture."), AUDIO_CAPTURE_SHORTCUT_LABEL);
        ImGui::Separator();
        ImGui::Spacing();

        bool any = s_settings.events || s_settings.psg_writes || s_settings.reference_wav || s_settings.sdl_wav;
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float offset = (ImGui::GetContentRegionAvail().x - total_btn_width) * 0.5f;
        if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        ImGui::BeginDisabled(!any);
        if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
            g_audio_capture_settings = s_settings;
            s_setup_open = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_setup_open = false;
        }
    }
    ImGui::End();
    if (!s_setup_open) s_initialized = false;

    /* Výběr adresáře (modální, nad dialogem) */
    if (s_dir_chooser_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("AudioCaptureDirChooser")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                g_strlcpy(s_settings.output_dir, dir.c_str(), sizeof(s_settings.output_dir));
            }
            ImGuiFileDialog::Instance()->Close();
            s_dir_chooser_open = false;
        }
    }
}


/*******************************************************************************
 *
 *                  Okno Audio Capture Control
 *
 ******************************************************************************/

/** @brief Viditelnost okna načtená z INI (cache do vzniku g_gui, a v headless trvale). */
static bool s_cfg_window_open = false;
/** @brief Cache byla aplikovaná do g_gui - od té doby je zdrojem pravdy g_gui. */
static bool s_cfg_window_applied = false;
/** @brief Modul `[AUDIOCAP_UI]` je zaregistrovaný. */
static bool s_cfg_window_registered = false;

/**
 * @brief INI -> cache viditelnosti okna (případně rovnou do g_gui po aplikaci).
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void ac_cfg_propagate_window(void *e, void *data)
{
    (void)data;
    s_cfg_window_open = cfgelement_get_bool_value((st_CFGELEMENT *)e) ? true : false;
    if (s_cfg_window_applied && g_gui) g_gui->showAudioCaptureWindow = s_cfg_window_open;
}

/**
 * @brief Viditelnost okna -> INI: z g_gui po aplikaci cache, jinak hodnota cache.
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void ac_cfg_save_window(void *e, void *data)
{
    (void)data;
    bool open = (s_cfg_window_applied && g_gui) ? g_gui->showAudioCaptureWindow : s_cfg_window_open;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, open ? 1 : 0);
}

void imgui_audio_capture_window_cfg_init(void)
{
    if (s_cfg_window_registered) return;
    s_cfg_window_registered = true;
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)"AUDIOCAP_UI");
    if (!cmod) return; /* duplicitní modul - okno pojede bez persistence */
    CFGELM *elm = cfgmodule_register_new_element(cmod, (char *)"window", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, ac_cfg_propagate_window, NULL);
    cfgelement_set_save_cb(elm, ac_cfg_save_window, NULL);
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void imgui_audio_capture_window_apply_persisted(void)
{
    if (!g_gui) return;
    g_gui->showAudioCaptureWindow = s_cfg_window_open;
    s_cfg_window_applied = true;
}

/**
 * @brief Řádek tabulky stavu: popisek a hodnota (zalomená na šířku okna).
 * @param label Přeložený popisek.
 * @param value Hodnota (UTF-8).
 * @param error true = hodnota je chyba (červeně).
 */
static void ac_status_row(const char *label, const char *value, bool error = false)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24.0f);
    if (error) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", value);
    } else {
        ImGui::TextUnformatted(value);
    }
    ImGui::PopTextWrapPos();
}

void imgui_audio_capture_window(void)
{
    if (!g_gui->showAudioCaptureWindow) return;

    char title[128];
    g_snprintf(title, sizeof(title), "%s###AudioCaptureRemote", _("Audio Capture Control"));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 100.0f, vp->WorkPos.y + 100.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &g_gui->showAudioCaptureWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    bool running = st.active || st.pending;

    if (ImGui::BeginTable("##ac_rc_status", 2, ImGuiTableFlags_SizingFixedFit)) {
        char buf[AUDIO_CAPTURE_PATH_MAX + 64];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("State"));
        ImGui::TableSetColumnIndex(1);
        if (st.active) {
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.0f), "%s", _("Capturing"));
        } else if (st.pending) {
            ImGui::TextUnformatted(_("Starting"));
        } else {
            ImGui::TextUnformatted(_("Off"));
        }

        if (running || st.dir[0]) {
            g_snprintf(buf, sizeof(buf), "%.1f s", ac_ui_seconds(st.frames));
            ac_status_row(running ? _("Time") : _("Last length"), buf);
            ac_status_row(_("Folder"), st.dir[0] ? st.dir : "-");
            g_snprintf(buf, sizeof(buf), _("%llu level changes, %llu PSG writes"), (unsigned long long)st.events,
                       (unsigned long long)st.psg_writes);
            ac_status_row(_("Recorded"), buf);
        }
        if (!running && st.last_error[0]) {
            ac_status_row(_("Last result"), st.last_error, true);
        } else if (!running && st.stop_reason[0]) {
            ac_status_row(_("Last result"), st.stop_reason);
        }

        GString *files = g_string_new(NULL);
        if (g_audio_capture_settings.events) g_string_append(files, "events.csv ");
        if (g_audio_capture_settings.psg_writes) g_string_append(files, "psg_writes.csv ");
        if (g_audio_capture_settings.reference_wav) g_string_append(files, "reference.wav ");
        if (g_audio_capture_settings.sdl_wav) g_string_append(files, "sdl.wav");
        ac_status_row(_("Files"), files->len ? files->str : "-");
        g_string_free(files, TRUE);

        char dir[AUDIO_CAPTURE_PATH_MAX];
        audio_capture_resolve_output_dir(dir, sizeof(dir));
        ac_status_row(_("Output folder"), dir);
        ImGui::EndTable();
    }

    ImGui::SeparatorText(_("Controls"));
    const ImGuiStyle &style = ImGui::GetStyle();
    float btn_w = ImGui::CalcTextSize(_("Start Audio Capture")).x;
    float w2 = ImGui::CalcTextSize(_("Stop Audio Capture")).x;
    if (w2 > btn_w) btn_w = w2;
    btn_w += style.FramePadding.x * 2.0f;
    char label[128];
    g_snprintf(label, sizeof(label), "%s###ac_rc_toggle", running ? _("Stop Audio Capture") : _("Start Audio Capture"));
    if (ImGui::Button(label, ImVec2(btn_w, 0.0f))) audio_capture_ui_toggle();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(AUDIO_CAPTURE_SHORTCUT_LABEL);
    ImGui::TextDisabled("%s", _("Each capture creates a new folder in the output folder."));

    ImGui::Separator();
    if (ImGui::Button(_L("Open Output Folder"))) ac_ui_open_output_folder();
    ImGui::SameLine();
    if (ImGui::Button(_L("Settings..."))) s_setup_open = true;

    ImGui::Separator();
    ImGui::TextDisabled(_("Show or hide this window: %s"), AUDIO_CAPTURE_WINDOW_SHORTCUT_LABEL);
    ImGui::End();
}
