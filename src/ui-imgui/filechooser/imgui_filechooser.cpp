#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <glib.h>

#include <filesystem>
#include <string>
#include <algorithm>

// Lokalizace
#include "i18n.h"

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "baseui/baseui_filechooser.h"
#include "res/CustomFont.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

extern "C"
{
    void imgui_file_chooser_window(void);
    void imgui_filechooser_new(baseui_fchooser_t *fch);
    void imgui_filechooser_settings_init(void);
};

// Cache pro persistované šířky panelů FileDialogu
static struct {
    float placesPaneWidth = 200.0f;
    bool placesPaneShown = false;
    float sidePaneWidth = 350.0f;
    bool loaded = false;
} s_igfd_panel_state;

// --- ImGuiSettingsHandler callbacky ---

static void IGFD_ClearAllFn(ImGuiContext *, ImGuiSettingsHandler *)
{
    s_igfd_panel_state.placesPaneWidth = 200.0f;
    s_igfd_panel_state.placesPaneShown = false;
    s_igfd_panel_state.sidePaneWidth = 350.0f;
    s_igfd_panel_state.loaded = false;
}

static void *IGFD_ReadOpenFn(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
    if (strcmp(name, "Panels") == 0)
        return &s_igfd_panel_state;
    return nullptr;
}

static void IGFD_ReadLineFn(ImGuiContext *, ImGuiSettingsHandler *, void *entry, const char *line)
{
    if (entry != &s_igfd_panel_state)
        return;

    float fval;
    int ival;

    if (sscanf(line, "PlacesPaneWidth=%f", &fval) == 1) {
        s_igfd_panel_state.placesPaneWidth = fval;
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "PlacesPaneShown=%d", &ival) == 1) {
        s_igfd_panel_state.placesPaneShown = (ival != 0);
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "SidePaneWidth=%f", &fval) == 1) {
        /* Defenzivní clamp - nesmyslně nízká hodnota = ignorovat, vrátit
         * default. Ochrana pro případ, že se v dříve uloženém .ini ocitla
         * 0 (regrese, viz IGFD_WriteAllFn). */
        if (fval < 50.0f) fval = 350.0f;
        s_igfd_panel_state.sidePaneWidth = fval;
        s_igfd_panel_state.loaded = true;
    }
}

static void IGFD_ApplyAllFn(ImGuiContext *, ImGuiSettingsHandler *)
{
    if (!s_igfd_panel_state.loaded)
        return;

    auto *dlg = ImGuiFileDialog::Instance();
    dlg->SetPlacesPaneWidth(s_igfd_panel_state.placesPaneWidth);
    dlg->SetPlacesPaneShown(s_igfd_panel_state.placesPaneShown);
    // sidePaneWidth se aplikuje při otevření dialogu přes config
}

static void IGFD_WriteAllFn(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    auto *dlg = ImGuiFileDialog::Instance();

    /* Pozor na sidePaneWidth: GetSidePaneWidth() vrací aktuální hodnotu
     * z config posledně otevřeného dialogu. Pokud byl posledně otevřen
     * dialog BEZ sidePane (např. CDL meta.json picker), IGFD vynuluje
     * sidePaneWidth na 0 (configureDialog řádek "if (sidePane==null)
     * sidePaneWidth = 0"). Pak by se 0 zapsala do .ini a další otevření
     * dialogu se sidePane (MZF preview) by mělo neviditelný panel.
     *
     * Proto persistujeme z cached state @ref s_igfd_panel_state, který
     * se updatuje JEN při zavření dialogu, který sidePane má. */
    float cached_sidepane_w = s_igfd_panel_state.sidePaneWidth;
    if (cached_sidepane_w < 50.0f) cached_sidepane_w = 350.0f;  /* defenzivní clamp */

    buf->appendf("[%s][Panels]\n", handler->TypeName);
    buf->appendf("PlacesPaneWidth=%.1f\n", dlg->GetPlacesPaneWidth());
    buf->appendf("PlacesPaneShown=%d\n", dlg->IsPlacesPaneShown() ? 1 : 0);
    buf->appendf("SidePaneWidth=%.1f\n", cached_sidepane_w);
    buf->append("\n");
}

void imgui_filechooser_settings_init(void)
{
    ImGuiSettingsHandler handler;
    handler.TypeName = "FileDialog";
    handler.TypeHash = ImHashStr("FileDialog");
    handler.ClearAllFn = IGFD_ClearAllFn;
    handler.ReadOpenFn = IGFD_ReadOpenFn;
    handler.ReadLineFn = IGFD_ReadLineFn;
    handler.ApplyAllFn = IGFD_ApplyAllFn;
    handler.WriteAllFn = IGFD_WriteAllFn;
    ImGui::AddSettingsHandler(&handler);
}

bool canValidateDialog = true;

// Callback pro pravý panel (custom pane)
void MZ_InfoPane(const char *vFilter, IGFD::UserDatas vUserDatas, bool *vCantContinue)
{
    (void)vFilter;
    (void)vUserDatas;
    (void)vCantContinue;
    // Pokud nechceme povolit vyber aktualniho souboru, tak zde nastavime false
    // if (vCantContinue)
    //     *vCantContinue = canValidateDialog;

    // Získáme aktuálně vybraný soubor v dialogu
    std::string selectedFile = ImGuiFileDialog::Instance()->GetCurrentFileName();
    std::string fullPath = ImGuiFileDialog::Instance()->GetCurrentPath() + "/" + selectedFile;

    if (selectedFile.empty() || selectedFile == ".." || selectedFile.size() <= 4 || (!std::filesystem::exists(fullPath)))
    {
        // g_print("Bad file: '%s'\n", selectedFile.c_str());
        ImGui::Dummy(ImVec2(1, 1));
        return;
    };

    std::string extension = selectedFile.substr(selectedFile.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    // Ověříme, zda má soubor příponu .mzf
    if (extension == ".mzf")
    {
        // g_print("MZF File: '%s'\n", selectedFile.c_str());
        ImGui::TextColored(ImVec4(1, 1, 0, 1), ICON_MZGLYPH_CASETTE " ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), _("MZF File Info"));

        ImGui::Separator();

        try
        {
            auto fileSize = std::filesystem::file_size(fullPath);
            ImGui::Text("Size: %" PRIu64 " bytes", (uint64_t)fileSize);

            ImGui::Text("\n");
            ImGui::SeparatorText("MZF Header");

            // Otevřeme soubor
            FILE *file = fopen(fullPath.c_str(), "rb");
            if (!file)
            {
                fprintf(stderr, "%s(%d): Failed to open file: %s\n", __FILE__, __LINE__, fullPath.c_str());
                return;
            };

            // Cely soubor nacteme do pameti
            uint8_t *buffer = (uint8_t *)malloc(fileSize);
            if (!buffer)
            {
                fprintf(stderr, "%s(%d): Failed to allocate memory for file buffer\n", __FILE__, __LINE__);
                fclose(file);
                return;
            };

            size_t bytesRead = fread(buffer, 1, fileSize, file);
            if (bytesRead != fileSize)
            {
                fprintf(stderr, "%s(%d): Failed to read file: %s\n", __FILE__, __LINE__, fullPath.c_str());
                free(buffer);
                fclose(file);
                return;
            };

            fclose(file);

            st_MZF_HEADER *mzfhdr = (st_MZF_HEADER *)buffer;
            char fname[MZF_FNAME_FULL_LENGTH];
            mzf_tools_get_fname(mzfhdr, fname);

            if (ImGui::BeginTable("FileInfoTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // NAME
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("NAME:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", fname);

                // TYPE
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("TYPE:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%02x", mzfhdr->ftype);

                // SIZE
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("SIZE:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fsize);

                // STRT
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("STRT:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fstrt);

                // EXEC
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("EXEC:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fexec);

                ImGui::EndTable();
            };
            free(buffer);
        }
        catch (...)
        {
            ImGui::Text("Size: Unknown");
        };
    }
    else
    {
        // g_print("OTHER File: '%s'\n", selectedFile.c_str());
        ImGui::Dummy(ImVec2(1, 1)); // ImGui neumožňuje prázdné panely, proto vložíme neviditelný element
    }
}

void imgui_file_chooser_window(void)
{
    // ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.0f, 1.0f)); // Oranžová při hoveru
    ImGui::SetNextWindowSize(ImVec2(1200, 768), ImGuiCond_FirstUseEver);

    /* Detekce změny šířky Devices panelu (placesPaneWidth) během dialog
     * session - splitter drag mění m_PlacesPaneWidth přímo bez ImGui dirty
     * markeru, takže auto-save .ini neuložil změněnou hodnotu. Per frame
     * porovnáme s cached state a při změně markneme dirty + updatujeme
     * cache. Stejně tak pro placesPaneShown a sidePaneWidth. */
    auto *dlg_persist = ImGuiFileDialog::Instance();
    {
        float pw = dlg_persist->GetPlacesPaneWidth();
        bool ps = dlg_persist->IsPlacesPaneShown();
        float sw = dlg_persist->GetSidePaneWidth();
        bool changed = false;
        if (pw != s_igfd_panel_state.placesPaneWidth)
        {
            s_igfd_panel_state.placesPaneWidth = pw;
            changed = true;
        };
        if (ps != s_igfd_panel_state.placesPaneShown)
        {
            s_igfd_panel_state.placesPaneShown = ps;
            changed = true;
        };
        /* sidePaneWidth: GetSidePaneWidth vrací 0 pokud aktuální dialog
         * nepoužívá sidePane - takový update do cache nepouštíme. */
        if (sw >= 50.0f && sw != s_igfd_panel_state.sidePaneWidth)
        {
            s_igfd_panel_state.sidePaneWidth = sw;
            changed = true;
        };
        if (changed)
            ImGui::MarkIniSettingsDirty();
    }

    if (ImGuiFileDialog::Instance()->Display("ChooseFileDlgKey"))
    {
        baseui_fchooser_t *fch = (baseui_fchooser_t *)ImGuiFileDialog::Instance()->GetUserDatas();
        if (fch == NULL)
        {
            fprintf(stderr, "%s(%d): Invalid filechooser data\n", __FILE__, __LINE__);
            s_igfd_panel_state.placesPaneWidth = ImGuiFileDialog::Instance()->GetPlacesPaneWidth();
            s_igfd_panel_state.placesPaneShown = ImGuiFileDialog::Instance()->IsPlacesPaneShown();
            s_igfd_panel_state.sidePaneWidth = ImGuiFileDialog::Instance()->GetSidePaneWidth();
            ImGui::MarkIniSettingsDirty();
            ImGuiFileDialog::Instance()->Close();
            // ImGui::PopStyleColor(); // Vrácení zpět na původní barvu
            return;
        };

        g_mutex_lock(&fch->mutex);

        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();
            std::string filePath = ImGuiFileDialog::Instance()->GetCurrentPath();
            std::string currentFilter = ImGuiFileDialog::Instance()->GetCurrentFilter();
            // action
            fch->selected_filePathName = g_strdup(filePathName.c_str());
            fch->selected_path = g_strdup(filePath.c_str());
            fch->selected_filter = g_strdup(currentFilter.c_str());
            fch->state = BASEUI_FCHOOSER_STATE_CLOSED_OK;
        }
        else
        {
            fch->state = BASEUI_FCHOOSER_STATE_CLOSED_CANCEL;
        }

        // Aktualizace cache šířek panelů před zavřením
        s_igfd_panel_state.placesPaneWidth = ImGuiFileDialog::Instance()->GetPlacesPaneWidth();
        s_igfd_panel_state.placesPaneShown = ImGuiFileDialog::Instance()->IsPlacesPaneShown();
        s_igfd_panel_state.sidePaneWidth = ImGuiFileDialog::Instance()->GetSidePaneWidth();
        ImGui::MarkIniSettingsDirty();

        // close
        ImGuiFileDialog::Instance()->Close();

        BaseuiFchooserCb cb = fch->cb;
        g_cond_signal(&fch->cond);
        g_mutex_unlock(&fch->mutex);

        if (cb)
            cb(fch);
    }
    // ImGui::PopStyleColor(); // Vrácení zpět na původní barvu
}

void imgui_filechooser_new(baseui_fchooser_t *fch)
{
    if (fch == NULL)
        return;

    g_mutex_lock(&fch->mutex);

    fch->selected_filePathName = NULL;
    fch->selected_path = NULL;
    fch->selected_filter = NULL;

    // Pokud jiz exisuje bezici dialog, tak vratime error
    if (ImGuiFileDialog::Instance()->IsOpened("ChooseFileDlgKey"))
    {
        fprintf(stderr, "%s(%d): Filechooser dialog is already opened\n", __FILE__, __LINE__);
        fch->state = BASEUI_FCHOOSER_STATE_CLOSED_ERROR;

        g_cond_signal(&fch->cond);
        g_mutex_unlock(&fch->mutex);

        if (fch->cb)
            fch->cb(fch);
        return;
    };

    fch->state = BASEUI_FCHOOSER_STATE_OPENED;

    const char *default_title = _("Untitled File Chooser");
    const char *default_filter = ".*";
    const char *default_path = ".";

    const char *use_title = (fch->title != NULL) ? fch->title : default_title;
    const char *use_filter = NULL;

    IGFD::FileDialogConfig config;

    if (fch->filePathName != NULL)
    {
        config.filePathName = fch->filePathName;
    }
    else
    {
        config.path = (fch->path != NULL) ? fch->path : default_path;
        config.fileName = (fch->fileName != NULL) ? fch->fileName : NULL;
    };

    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles | ImGuiFileDialogFlags_ShowDevicesButton | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
    switch (fch->type)
    {
    case BASEUI_FCHOOSER_TYPE_OPEN_FILE:
        config.flags |= ImGuiFileDialogFlags_ReadOnlyFileNameField;
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    case BASEUI_FCHOOSER_TYPE_SAVE_FILE:
        config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    case BASEUI_FCHOOSER_TYPE_RW_FILE:
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    default:
        break;
    }

    // Použití custom panelu pro .mzf soubory
    config.sidePane = std::bind(&MZ_InfoPane, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    config.sidePaneWidth = s_igfd_panel_state.sidePaneWidth;
    config.userDatas = fch;

    ImVec4 mzfColor = ImVec4(0.0f, 1.0f, 1.0f, 0.9f);
    const char *mzfIcon = ICON_MZGLYPH_CASETTE;

    ImVec4 dskColor = ImVec4(0.0f, 1.0f, 1.0f, 0.9f);
    const char *dskIcon = ICON_MZGLYPH_FLOPPY_DISC;

    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".mzf", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".MZF", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".m12", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".M12", mzfColor, mzfIcon);

    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".dsk", dskColor, dskIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".DSK", dskColor, dskIcon);

    // const char *filters = ".*, .mzf";
    const char *filters = use_filter;
    ImGuiFileDialog::Instance()->OpenDialog("ChooseFileDlgKey", use_title, filters, config);

    g_cond_signal(&fch->cond);
    g_mutex_unlock(&fch->mutex);
}
