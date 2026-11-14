/**
 * @file snapshot_load_dialog.cpp
 * @brief Dialog pro načtení snapshotu — ImGuiFileDialog s náhledem
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <filesystem>
#include <glib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3_image/SDL_image.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/snapshot/snapshot_request_ui.h"
#include "ui-imgui/cmt/imgui_cmt.h"

extern "C"
{
#include "snapshot/snapshot.h"
}

/* Deklarace z menu_snapshot.cpp */
extern "C" bool snapshot_ui_is_load_requested(void);
extern "C" void snapshot_ui_clear_load_request(void);
extern "C" bool snapshot_ui_was_running_before_pause(void);

/* Kešovaný náhled */
static std::string s_cached_preview_path;
static GLuint s_preview_texture = 0;
static int s_preview_width = 0;
static int s_preview_height = 0;
static st_SNAPSHOT_METADATA s_cached_metadata;
static bool s_metadata_valid = false;

static bool s_file_dialog_open = false;

/* Forward deklarace pro konverzi SDL_Surface na GL texturu */
extern "C" GLuint SDL_SurfaceToTexture(SDL_Surface *surface);


/**
 * @brief Obnoví běh po zavření dialogu bez načtení (Cancel) podle
 *        `load_resume_mode`, stejně jako po úspěšném načtení.
 *
 * Výsledek skutečného načtení obsluhuje imgui_snapshot_request_results().
 */
static void snapshot_load_restore_emulation(void)
{
    int mode = g_snapshot_settings.load_resume_mode;
    switch (mode) {
        case SNAPSHOT_RESUME_ALWAYS_RUN:
            dbg_ui_run();
            break;
        case SNAPSHOT_RESUME_ALWAYS_PAUSE:
            /* Nechat v pauze */
            break;
        case SNAPSHOT_RESUME_PREVIOUS:
        default:
            if (snapshot_ui_was_running_before_pause())
                dbg_ui_run();
            break;
    }
}


static void snapshot_load_clear_preview(void)
{
    if (s_preview_texture) {
        glDeleteTextures(1, &s_preview_texture);
        s_preview_texture = 0;
    }
    s_cached_preview_path.clear();
    s_metadata_valid = false;
    s_preview_width = 0;
    s_preview_height = 0;
}


static void snapshot_load_update_preview(const std::string &fullPath)
{
    /* Pokud je to stejný soubor, nechat keš */
    if (fullPath == s_cached_preview_path)
        return;

    snapshot_load_clear_preview();
    s_cached_preview_path = fullPath;

    /* Načíst metadata */
    memset(&s_cached_metadata, 0, sizeof(s_cached_metadata));
    en_SNAPSHOT_RESULT res = snapshot_read_metadata(fullPath.c_str(), &s_cached_metadata);
    s_metadata_valid = (res == SNAPSHOT_OK);

    /* Načíst screenshot */
    uint8_t *png_data = NULL;
    size_t png_size = 0;
    res = snapshot_extract_screenshot(fullPath.c_str(), &png_data, &png_size);
    if (res == SNAPSHOT_OK && png_data) {
        SDL_IOStream *io = SDL_IOFromConstMem(png_data, png_size);
        if (io) {
            SDL_Surface *surface = IMG_Load_IO(io, true);
            if (surface) {
                s_preview_width = surface->w;
                s_preview_height = surface->h;
                s_preview_texture = SDL_SurfaceToTexture(surface);
                SDL_DestroySurface(surface);
            }
        }
        g_free(png_data);
    }
}


/**
 * Custom pane callback pro ImGuiFileDialog — zobrazí náhled snapshotu
 */
static void Snapshot_InfoPane(const char *vFilter, IGFD::UserDatas vUserDatas, bool *vCantContinue)
{
    (void)vFilter;
    (void)vUserDatas;
    (void)vCantContinue;

    std::string selectedFile = ImGuiFileDialog::Instance()->GetCurrentFileName();
    std::string currentPath = ImGuiFileDialog::Instance()->GetCurrentPath();

    if (selectedFile.empty() || selectedFile == "..")
        return;

    std::string fullPath = currentPath + "/" + selectedFile;

    /* Ověřit existenci souboru */
    if (!std::filesystem::exists(fullPath))
        return;

    /* Ověřit příponu .mzs */
    if (fullPath.size() < 4)
        return;
    std::string ext = fullPath.substr(fullPath.size() - 4);
    if (ext != ".mzs" && ext != ".MZS")
        return;

    /* Aktualizovat náhled (kešuje se) */
    snapshot_load_update_preview(fullPath);

    /* Zobrazit náhled */
    ImGui::TextColored(ImVec4(1, 1, 0, 1), _("Snapshot Preview"));
    ImGui::Separator();

    /* Screenshot */
    if (s_preview_texture) {
        float aspect = (float)s_preview_width / (float)s_preview_height;
        float pane_width = ImGui::GetContentRegionAvail().x;
        float img_width = pane_width;
        float img_height = img_width / aspect;

        /* Omezit výšku */
        if (img_height > 200.0f) {
            img_height = 200.0f;
            img_width = img_height * aspect;
        }

        ImGui::Image((ImTextureID)(intptr_t)s_preview_texture,
                      ImVec2(img_width, img_height));
    } else {
        ImGui::TextDisabled(_("No screenshot available"));
    }

    ImGui::Spacing();

    /* Metadata */
    if (s_metadata_valid) {
        const char *arch_name = "?";
        switch (s_cached_metadata.architecture) {
            case 700: arch_name = "MZ-700"; break;
            case 800: arch_name = "MZ-800"; break;
            case 1500: arch_name = "MZ-1500"; break;
        }

        ImGui::Text("%s: %s", _("Architecture"), arch_name);
        ImGui::Text("%s: %s", _("Created"), s_cached_metadata.created);
        ImGui::Text("%s: %s", _("Emulator"), s_cached_metadata.emulator_version);

        ImGui::Separator();

        if (s_cached_metadata.description[0]) {
            ImGui::Text("%s:", _("Description"));
            ImGui::TextWrapped("%s", s_cached_metadata.description);
        }

        ImGui::Separator();

        ImGui::Text("%s: v%u", _("Format"), s_cached_metadata.format_version);

        /* Velikost souboru */
        try {
            auto fileSize = std::filesystem::file_size(fullPath);
            if (fileSize < 1024)
                ImGui::Text("%s: %llu B", _("Size"), (unsigned long long)fileSize);
            else if (fileSize < 1024 * 1024)
                ImGui::Text("%s: %.1f KB", _("Size"), fileSize / 1024.0);
            else
                ImGui::Text("%s: %.1f MB", _("Size"), fileSize / (1024.0 * 1024.0));
        } catch (...) {}
    } else {
        ImGui::TextDisabled(_("Cannot read snapshot metadata"));
    }
}


static void snapshot_load_open_file_dialog(void)
{
    snapshot_load_clear_preview();

    IGFD::FileDialogConfig config;
    config.path = g_snapshot_settings.default_directory;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton |
                   ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering |
                   ImGuiFileDialogFlags_ReadOnlyFileNameField;
    config.sidePane = Snapshot_InfoPane;
    config.sidePaneWidth = 350.0f;

    imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_SNAPSHOT, config);
    ImGuiFileDialog::Instance()->OpenDialog(
        "SnapshotLoadDialog", _("Load Snapshot"), ".mzs", config);
    s_file_dialog_open = true;
}


/**
 * @brief Načtení snapshotu zrušené dotazem Virtual CMT po výběru souboru: obnoví běh jako Cancel dialogu.
 * @pre UI vlákno.
 */
extern "C" void snapshot_ui_load_cancelled(void)
{
    snapshot_load_restore_emulation();
}


extern "C" void imgui_snapshot_load_dialog(void)

{
    /* Fáze 1: Otevření souborového dialogu po požadavku z menu */
    if (snapshot_ui_is_load_requested()) {
        snapshot_ui_clear_load_request();
        snapshot_load_open_file_dialog();
    }

    /* Fáze 2: Zpracování souborového dialogu */
    if (s_file_dialog_open) {
        ImGui::SetNextWindowSize(ImVec2(1200, 768), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("SnapshotLoadDialog")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();

                /* Načtení vyřídí emu vlákno v pauze; oznámení a obnovení
                 * běhu dělá imgui_snapshot_request_results(). Stav pásky se
                 * kontroluje znovu (mezi dotazem a výběrem souboru se mohl
                 * zhoršit); zrušený dotaz = snapshot_ui_load_cancelled(). */
                imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, path.c_str());
            } else {
                /* Cancel — obnovit předchozí stav */
                snapshot_load_restore_emulation();
            }
            ImGuiFileDialog::Instance()->Close();
            s_file_dialog_open = false;
            snapshot_load_clear_preview();
        }
    }
}
