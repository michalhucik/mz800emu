/**
 * @file   screenshot_ui.cpp
 * @brief  Screenshot z GUI: menu Tools -> Screenshot, zkratky Alt+F, Alt+Shift+F
 *         a Alt+G, dialog nastavení a okno Screenshot Control.
 *
 * Viz screenshot_ui.h. Zdrojem obrazu je surface, který okno emulátoru
 * zobrazuje (UI vlákno ho samo plní), takže screenshot je přesně to, co
 * uživatel vidí. Oblast a měřítko bere z g_screenshot_settings.
 *
 * Okno Screenshot Control (`###ScreenshotRemote`) je obdoba dálkového
 * ovládání nahrávání videa: nastavení (oblast, měřítko, složka), poslední
 * screenshot, stav série snímků a tlačítka s viditelnými zkratkami.
 * Viditelnost okna se ukládá do INI (sekce `[SCREENSHOT_UI]`, klíč
 * `window`, vzor videorec_remote_window.cpp včetně cache pro headless).
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
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/screenshot/screenshot_ui.h"
#include "iface-video/sdlapp/sdlapp_imgui_video.h"
#include "baseui/baseui_filechooser.h"
#include "emulator/screenshot/screenshot_capture.h"
#include "emulator/screenshot/screenshot_config.h"
#include "emulator/screenshot/screenshot_series.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

#include "i18n.h"

/* Implementace v snapshot/snapshot_notification.cpp. */
extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title);

/** @brief Zobrazovaný popisek zkratky uložení screenshotu. */
#define SCREENSHOT_SHORTCUT_LABEL "Alt+F"
/** @brief Zobrazovaný popisek zkratky série snímků. */
#define SCREENSHOT_SERIES_SHORTCUT_LABEL "Alt+Shift+F"
/** @brief Zobrazovaný popisek zkratky okna Screenshot Control. */
#define SCREENSHOT_WINDOW_SHORTCUT_LABEL "Alt+G"

/** @brief Cesta posledního screenshotu z GUI ("" = žádný v tomto běhu). */
static char s_last_path[1200] = "";
/** @brief Rozměr posledního screenshotu. */
static unsigned s_last_w = 0, s_last_h = 0;
/** @brief Poslední pokus o screenshot selhal (s_last_error platí). */
static bool s_last_failed = false;
/** @brief Popis chyby posledního pokusu (přeložený nebo anglicky z jádra). */
static char s_last_error[256] = "";


/*******************************************************************************
 *
 *                  Uložení
 *
 ******************************************************************************/

/**
 * @brief Uloží PNG zobrazovaného obrazu do souboru podle g_screenshot_settings.
 *
 * @param path     Cílová cesta.
 * @param width    Výstup: šířka obrázku.
 * @param height   Výstup: výška obrázku.
 * @param err      Buffer na anglický popis chyby (jádro) nebo přeložený text.
 * @param err_size Velikost `err`.
 * @return true = soubor zapsán.
 */
static bool ss_ui_save_to(const char *path, unsigned *width, unsigned *height, char *err, size_t err_size)
{
    SDL_Surface *surface = iface_sdl3_video_get_screen_surface();
    if (!surface || surface->format != SDL_PIXELFORMAT_INDEX8) {
        g_strlcpy(err, _("The emulator screen is not available"), err_size);
        return false;
    }

    st_SCREENSHOT_OPTS opts;
    screenshot_settings_to_opts(&g_screenshot_settings, &opts);

    st_SCREENSHOT_PNG png;
    SDL_LockSurface(surface);
    bool ok = screenshot_encode_png((const uint8_t *)surface->pixels, (size_t)surface->pitch, &opts, &png, err, err_size);
    SDL_UnlockSurface(surface);
    if (!ok) return false;

    GError *werr = NULL;
    ok = g_file_set_contents(path, (const gchar *)png.png, (gssize)png.png_len, &werr);
    g_free(png.png);
    if (!ok) {
        g_strlcpy(err, werr ? werr->message : "write error", err_size);
        if (werr) g_error_free(werr);
        return false;
    }
    *width = png.width;
    *height = png.height;
    return true;
}

/**
 * @brief Uloží screenshot a výsledek ohlásí notifikací.
 * @param path Cílová cesta.
 */
static void ss_ui_save_and_notify(const char *path)
{
    unsigned w = 0, h = 0;
    char err[256] = "";
    bool ok = ss_ui_save_to(path, &w, &h, err, sizeof(err));
    g_strlcpy(s_last_path, path, sizeof(s_last_path));
    s_last_failed = !ok;
    s_last_w = w;
    s_last_h = h;
    g_strlcpy(s_last_error, ok ? "" : err, sizeof(s_last_error));
    if (ok) {
        char *msg = g_strdup_printf(_("Screenshot saved (%u x %u): %s"), w, h, path);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf(_("Cannot save screenshot to '%s': %s"), path, err);
        snapshot_notification_show_ex(msg, true, _("Screenshot Error"));
        g_free(msg);
    }
}

void screenshot_ui_save_quick(void)
{
    char dir[1024];
    char path[1200];
    screenshot_resolve_output_dir(dir, sizeof(dir));
    screenshot_make_filename(dir, path, sizeof(path));
    ss_ui_save_and_notify(path);
}

/** @brief Callback dialogu Save As - uloží screenshot na zvolenou cestu. */
static void ss_ui_save_as_cb(baseui_fchooser_t *fch)
{
    if (fch->state == BASEUI_FCHOOSER_STATE_CLOSED_OK && fch->selected_filePathName) {
        ss_ui_save_and_notify(fch->selected_filePathName);
    }
    baseui_filechooser_destroy(fch);
}

void screenshot_ui_save_as(void)
{
    char dir[1024];
    char path[1200];
    screenshot_resolve_output_dir(dir, sizeof(dir));
    screenshot_make_filename(dir, path, sizeof(path));
    char *name = g_path_get_basename(path);
    baseui_fchooser_t *fch = baseui_filechooser_save_file(BASEUI_FCHOOSER_CAT_SCREENSHOT, _("Save Screenshot"),
                                                          ".png", NULL, name, NULL, ss_ui_save_as_cb, NULL);
    g_free(name);
    if (!fch) fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
}

/* Doxygen viz screenshot_ui.h. */
void screenshot_ui_open_output_folder(void)
{
    char dir[1024];
    bool ok = screenshot_resolve_output_dir(dir, sizeof(dir));
    char *abs = g_canonicalize_filename(dir, NULL);
    char *uri = ok ? g_filename_to_uri(abs, NULL, NULL) : NULL;
    if (!uri || !SDL_OpenURL(uri)) {
        char *msg = g_strdup_printf(_("Cannot open folder: %s"), abs);
        snapshot_notification_show_ex(msg, true, _("Screenshot Error"));
        g_free(msg);
    }
    g_free(uri);
    g_free(abs);
}


/*******************************************************************************
 *
 *                  Menu a zkratka
 *
 ******************************************************************************/

void imgui_screenshot_menu(void)
{
    if (!ImGui::BeginMenu(_L("Screenshot"))) return;
    if (ImGui::MenuItem(_L("Save Screenshot"), SCREENSHOT_SHORTCUT_LABEL)) {
        screenshot_ui_save_quick();
    }
    if (ImGui::MenuItem(_L("Save Screenshot As..."))) {
        screenshot_ui_save_as();
    }
    st_SCREENSHOT_SERIES_STATUS st;
    screenshot_series_get_status(&st);
    if (ImGui::MenuItem(_L("Record Changed Frames"), SCREENSHOT_SERIES_SHORTCUT_LABEL, st.active)) {
        screenshot_ui_toggle_series();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Saves a PNG of every emulated frame that differs from the previous one "
                                  "(area and scale from Settings) until you stop it."));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(_L("Screenshot Control..."), SCREENSHOT_WINDOW_SHORTCUT_LABEL, g_gui->showScreenshotWindow)) {
        g_gui->showScreenshotWindow = !g_gui->showScreenshotWindow;
    }
    if (ImGui::MenuItem(_L("Settings..."))) {
        g_gui->showScreenshotSetupWindow = true;
    }
    if (ImGui::MenuItem(_L("Open Output Folder"))) {
        screenshot_ui_open_output_folder();
    }
    ImGui::EndMenu();
}

void imgui_screenshot_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyAlt || io.KeyCtrl) return;
    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        g_gui->showScreenshotWindow = !g_gui->showScreenshotWindow;
        return;
    }
    if (!ImGui::IsKeyPressed(ImGuiKey_F, false)) return;
    if (io.KeyShift) {
        screenshot_ui_toggle_series();
    } else {
        screenshot_ui_save_quick();
    }
}


/*******************************************************************************
 *
 *                  Série snímků
 *
 ******************************************************************************/

/** @brief UI spustilo sérii a ještě neohlásilo její dokončení. */
static bool s_series_pending_report = false;

void screenshot_ui_toggle_series(void)
{
    if (screenshot_series_stop()) return; /* běžela - dokončení ohlásí imgui_screenshot_series_poll() */

    st_SCREENSHOT_SERIES_PARAMS p;
    screenshot_series_params_init(&p);
    screenshot_settings_to_opts(&g_screenshot_settings, &p.opts);
    p.only_changed = g_screenshot_settings.series_only_changed;
    char err[256] = "";
    if (screenshot_series_start(&p, err, sizeof(err))) {
        s_series_pending_report = true;
    } else {
        char *msg = g_strdup_printf(_("Cannot start the frame series: %s"), err);
        snapshot_notification_show_ex(msg, true, _("Screenshot Error"));
        g_free(msg);
    }
}

void imgui_screenshot_series_poll(void)
{
    if (!s_series_pending_report) return;
    st_SCREENSHOT_SERIES_STATUS st;
    screenshot_series_get_status(&st);
    if (st.active || st.writing) return;
    s_series_pending_report = false;
    if (st.last_error[0]) {
        char *msg = g_strdup_printf(_("Frame series stopped after %u files: %s"), (unsigned)st.files, st.last_error);
        snapshot_notification_show_ex(msg, true, _("Screenshot Error"));
        g_free(msg);
    } else {
        char *msg = g_strdup_printf(_("Frame series saved: %u files of %u frames to %s"),
                                    (unsigned)st.files, (unsigned)st.frames_seen, st.dir);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    }
}

void imgui_screenshot_series_overlay(void)
{
    st_SCREENSHOT_SERIES_STATUS st;
    screenshot_series_get_status(&st);
    if (!st.active) return;

    ImVec2 img_min = ImGui::GetItemRectMin();
    char text[128];
    g_snprintf(text, sizeof(text), _("FRAMES %u / %u"), (unsigned)st.files, (unsigned)st.frames_seen);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float pad = 6.0f;
    const float margin = 10.0f;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.4f;
    float w = pad + 2.0f * r + pad + ts.x + pad;
    float h = ts.y + 2.0f * pad;
    ImVec2 box_min(img_min.x + margin, img_min.y + margin);
    ImVec2 box_max(img_min.x + margin + w, img_min.y + margin + h);
    dl->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    /* Symbol série: dva překrývající se čtverečky (snímky). */
    ImVec2 c(box_min.x + pad + r, box_min.y + h * 0.5f);
    dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), IM_COL32(80, 200, 255, 255));
    dl->AddRectFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r, c.y + r), IM_COL32(80, 200, 255, 255));
    dl->AddText(ImVec2(c.x + r + pad, box_min.y + pad), IM_COL32(255, 255, 255, 255), text);
}


/*******************************************************************************
 *
 *                  Dialog nastavení
 *
 ******************************************************************************/

/** @brief Upravovaná kopie nastavení (aplikuje se po OK). */
static st_SCREENSHOT_SETTINGS s_settings;
/** @brief Kopie je načtená z g_screenshot_settings (do zavření dialogu). */
static bool s_initialized = false;
/** @brief Je otevřený výběr adresáře. */
static bool s_dir_chooser_open = false;

/** @brief Položky měřítka v dialogu: 0 = native, 1..4 = okno. */
static const char *const s_scale_labels[] = {
    N_("Native (computer dots, 320 or 640 wide)"),
    N_("1x"),
    N_("2x (window at 100 %)"),
    N_("3x"),
    N_("4x"),
};

/**
 * @brief Text s rozměrem obrázku pro aktuální volby v dialogu.
 * @param buf  Výstup.
 * @param size Velikost `buf`.
 */
static void ss_ui_size_text(char *buf, size_t size)
{
    st_SCREENSHOT_GEOMETRY g;
    screenshot_geometry_current(&g);
    unsigned w = (s_settings.area == SCREENSHOT_AREA_CANVAS) ? g.canvas_w : g.fb_width;
    unsigned h = (s_settings.area == SCREENSHOT_AREA_CANVAS) ? g.canvas_h : g.fb_height;
    if (s_settings.scale_mode == SCREENSHOT_SCALE_NATIVE) {
        g_snprintf(buf, size, _("Image size: %u x %u or %u x %u (640-dot modes)"), w / 2, h, w, h);
    } else {
        unsigned k = s_settings.window_scale;
        g_snprintf(buf, size, _("Image size: %u x %u"), w * k / 2, h * k);
    }
}

void imgui_screenshot_setup_dialog(void)
{
    if (!g_gui->showScreenshotSetupWindow) {
        s_initialized = false;
        return;
    }
    if (!s_initialized) {
        s_settings = g_screenshot_settings;
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    char title[128];
    g_snprintf(title, sizeof(title), "%s###ScreenshotSetup", _("Screenshot Settings"));

    if (ImGui::Begin(title, &g_gui->showScreenshotSetupWindow, flags)) {

        /* Oblast */
        ImGui::Text("%s:", _("Area"));
        if (ImGui::RadioButton(_L("Whole frame with border"), s_settings.area == SCREENSHOT_AREA_FULL)) {
            s_settings.area = SCREENSHOT_AREA_FULL;
        }
        if (ImGui::RadioButton(_L("Picture without border"), s_settings.area == SCREENSHOT_AREA_CANVAS)) {
            s_settings.area = SCREENSHOT_AREA_CANVAS;
        }
        ImGui::Spacing();

        /* Měřítko */
        ImGui::Text("%s:", _("Scale"));
        int cur = (s_settings.scale_mode == SCREENSHOT_SCALE_NATIVE) ? 0 : (int)s_settings.window_scale;
        if (cur < 0 || cur > SCREENSHOT_WINDOW_SCALE_MAX) cur = 2;
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("##screenshot_scale", _(s_scale_labels[cur]))) {
            for (int i = 0; i <= SCREENSHOT_WINDOW_SCALE_MAX; i++) {
                bool selected = (cur == i);
                if (ImGui::Selectable(_L(s_scale_labels[i]), selected)) {
                    if (i == 0) {
                        s_settings.scale_mode = SCREENSHOT_SCALE_NATIVE;
                        s_settings.window_scale = 1;
                    } else {
                        s_settings.scale_mode = SCREENSHOT_SCALE_WINDOW;
                        s_settings.window_scale = (unsigned)i;
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        char size_text[128];
        ss_ui_size_text(size_text, sizeof(size_text));
        ImGui::TextDisabled("%s", size_text);
        if (s_settings.scale_mode == SCREENSHOT_SCALE_WINDOW && s_settings.window_scale == 1) {
            ImGui::TextDisabled("%s", _("1x drops every second column in the 640-dot modes."));
        }
        ImGui::Spacing();

        /* Výstupní adresář */
        ImGui::Text("%s:", _("Output folder"));
        float btn_width = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##screenshot_dir", _("(default: screenshots folder next to the emulator)"),
                                 s_settings.output_dir, sizeof(s_settings.output_dir));
        ImGui::SameLine();
        if (ImGui::Button("...##screenshot_dir_browse", ImVec2(btn_width, 0))) {
            IGFD::FileDialogConfig config;
            config.path = s_settings.output_dir[0] ? s_settings.output_dir : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_SCREENSHOT, config);
            ImGuiFileDialog::Instance()->OpenDialog("ScreenshotDirChooser", _("Select Directory"), nullptr, config);
            s_dir_chooser_open = true;
        }

        ImGui::Spacing();
        ImGui::Checkbox(_L("Frame series: only changed frames"), &s_settings.series_only_changed);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _("Checked: Record Changed Frames saves only frames that differ from the last "
                                      "saved one. Unchecked: every emulated frame."));
        }

        ImGui::Spacing();
        ImGui::TextDisabled(_("Save Screenshot (%s) saves the picture shown in the window."), SCREENSHOT_SHORTCUT_LABEL);
        ImGui::TextDisabled(_("Record Changed Frames (%s) saves emulated frames into a new folder."),
                            SCREENSHOT_SERIES_SHORTCUT_LABEL);
        ImGui::Separator();
        ImGui::Spacing();

        /* OK / Cancel na střed */
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float offset = (ImGui::GetContentRegionAvail().x - total_btn_width) * 0.5f;
        if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
            g_screenshot_settings = s_settings;
            g_gui->showScreenshotSetupWindow = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            g_gui->showScreenshotSetupWindow = false;
        }
    }
    ImGui::End();

    /* Zavření (OK, Cancel, křížek) - příště znovu zkopírovat aktuální nastavení. */
    if (!g_gui->showScreenshotSetupWindow) s_initialized = false;

    /* Výběr adresáře (modální, nad dialogem) */
    if (s_dir_chooser_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("ScreenshotDirChooser")) {
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
 *                  Okno Screenshot Control
 *
 ******************************************************************************/

/** @brief Viditelnost okna načtená z INI (cache do vzniku g_gui, a v headless trvale). */
static bool s_cfg_window_open = false;
/** @brief Cache byla aplikovaná do g_gui - od té doby je zdrojem pravdy g_gui. */
static bool s_cfg_window_applied = false;
/** @brief Modul `[SCREENSHOT_UI]` je zaregistrovaný. */
static bool s_cfg_window_registered = false;

/**
 * @brief INI -> cache viditelnosti okna (případně rovnou do g_gui po aplikaci).
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void ss_cfg_propagate_window(void *e, void *data)
{
    (void)data;
    s_cfg_window_open = cfgelement_get_bool_value((st_CFGELEMENT *)e) ? true : false;
    if (s_cfg_window_applied && g_gui) g_gui->showScreenshotWindow = s_cfg_window_open;
}

/**
 * @brief Viditelnost okna -> INI: z g_gui po aplikaci cache, jinak hodnota cache.
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void ss_cfg_save_window(void *e, void *data)
{
    (void)data;
    bool open = (s_cfg_window_applied && g_gui) ? g_gui->showScreenshotWindow : s_cfg_window_open;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, open ? 1 : 0);
}

void imgui_screenshot_window_cfg_init(void)
{
    if (s_cfg_window_registered) return;
    s_cfg_window_registered = true;
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)"SCREENSHOT_UI");
    if (!cmod) return; /* duplicitní modul - okno pojede bez persistence */
    CFGELM *elm = cfgmodule_register_new_element(cmod, (char *)"window", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, ss_cfg_propagate_window, NULL);
    cfgelement_set_save_cb(elm, ss_cfg_save_window, NULL);
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void imgui_screenshot_window_apply_persisted(void)
{
    if (!g_gui) return;
    g_gui->showScreenshotWindow = s_cfg_window_open;
    s_cfg_window_applied = true;
}

/**
 * @brief Řádek tabulky stavu: popisek a hodnota (zalomená na šířku okna).
 * @param label Přeložený popisek.
 * @param value Hodnota (UTF-8).
 * @param error true = hodnota je chyba (červeně).
 */
static void ss_status_row(const char *label, const char *value, bool error = false)
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

/**
 * @brief Tlačítko akce na novém řádku tabulky akcí, zkratka viditelně ve druhém sloupci.
 * @param label Popisek s ImGui ID.
 * @param width Šířka tlačítka.
 * @param hint  Text zkratky nebo NULL.
 * @return true = stisknuto.
 */
static bool ss_action(const char *label, float width, const char *hint)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    if (hint) {
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(hint);
    }
    return pressed;
}

void imgui_screenshot_window(void)
{
    if (!g_gui->showScreenshotWindow) return;

    char title[128];
    g_snprintf(title, sizeof(title), "%s###ScreenshotRemote", _("Screenshot Control"));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 80.0f, vp->WorkPos.y + 80.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &g_gui->showScreenshotWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    st_SCREENSHOT_SERIES_STATUS st;
    screenshot_series_get_status(&st);

    if (ImGui::BeginTable("##ss_rc_status", 2, ImGuiTableFlags_SizingFixedFit)) {
        char buf[1400];
        ss_status_row(_("Area"), g_screenshot_settings.area == SCREENSHOT_AREA_CANVAS ? _("Picture without border")
                                                                                      : _("Whole frame with border"));
        int cur = (g_screenshot_settings.scale_mode == SCREENSHOT_SCALE_NATIVE) ? 0 : (int)g_screenshot_settings.window_scale;
        if (cur < 0 || cur > SCREENSHOT_WINDOW_SCALE_MAX) cur = 2;
        ss_status_row(_("Scale"), _(s_scale_labels[cur]));
        char dir[1024];
        screenshot_resolve_output_dir(dir, sizeof(dir));
        ss_status_row(_("Output folder"), dir);

        if (!s_last_path[0]) {
            ss_status_row(_("Last screenshot"), "-");
        } else if (s_last_failed) {
            ss_status_row(_("Last screenshot"), s_last_error, true);
        } else {
            char *base = g_path_get_basename(s_last_path);
            g_snprintf(buf, sizeof(buf), "%s (%u x %u)", base, s_last_w, s_last_h);
            g_free(base);
            ss_status_row(_("Last screenshot"), buf);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s_last_path);
        }

        if (st.active) {
            g_snprintf(buf, sizeof(buf), _("Recording: %u files of %u frames"), (unsigned)st.files,
                       (unsigned)st.frames_seen);
            ss_status_row(_("Frame series"), buf);
        } else if (st.writing) {
            g_snprintf(buf, sizeof(buf), _("Writing: %u files"), (unsigned)st.files);
            ss_status_row(_("Frame series"), buf);
        } else if (st.dir[0]) {
            if (st.last_error[0]) {
                g_snprintf(buf, sizeof(buf), _("Stopped after %u files: %s"), (unsigned)st.files, st.last_error);
                ss_status_row(_("Frame series"), buf, true);
            } else {
                g_snprintf(buf, sizeof(buf), _("Last: %u files of %u frames"), (unsigned)st.files,
                           (unsigned)st.frames_seen);
                ss_status_row(_("Frame series"), buf);
            }
        } else {
            ss_status_row(_("Frame series"), _("off"));
        }
        if (st.dir[0]) {
            ss_status_row(_("Series folder"), st.dir);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(_("Controls"));
    const ImGuiStyle &style = ImGui::GetStyle();
    const char *const widest[] = { N_("Save Screenshot"), N_("Save Screenshot As..."), N_("Start Frame Series"),
                                   N_("Stop Frame Series") };
    float btn_w = 0.0f;
    for (size_t i = 0; i < G_N_ELEMENTS(widest); i++) {
        float w = ImGui::CalcTextSize(_(widest[i])).x;
        if (w > btn_w) btn_w = w;
    }
    btn_w += style.FramePadding.x * 2.0f;
    if (ImGui::BeginTable("##ss_rc_actions", 2, ImGuiTableFlags_SizingFixedFit)) {
        if (ss_action(_L("Save Screenshot"), btn_w, SCREENSHOT_SHORTCUT_LABEL)) screenshot_ui_save_quick();
        if (ss_action(_L("Save Screenshot As..."), btn_w, NULL)) screenshot_ui_save_as();
        char label[128];
        g_snprintf(label, sizeof(label), "%s###ss_rc_series", st.active ? _("Stop Frame Series") : _("Start Frame Series"));
        ImGui::BeginDisabled(!st.active && st.writing);
        if (ss_action(label, btn_w, SCREENSHOT_SERIES_SHORTCUT_LABEL)) screenshot_ui_toggle_series();
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", _("The frame series saves a PNG of every changed emulated frame into a new folder."));

    ImGui::Separator();
    if (ImGui::Button(_L("Open Output Folder"))) screenshot_ui_open_output_folder();
    ImGui::SameLine();
    if (ImGui::Button(_L("Settings..."))) g_gui->showScreenshotSetupWindow = true;

    ImGui::Separator();
    ImGui::TextDisabled(_("Show or hide this window: %s"), SCREENSHOT_WINDOW_SHORTCUT_LABEL);
    ImGui::End();
}
