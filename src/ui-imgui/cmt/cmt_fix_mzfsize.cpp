#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <string>

// Lokalizace
#include "i18n.h"

#include "ui-imgui/filechooser/res/CustomFont.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "baseui/baseui_tools.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_cmt_fix_mzfsize_popup_activate(const char *filename, int valid_size, int file_size);
    void imgui_cmt_fix_mzfsize_popup(bool *p_open);
};

typedef struct ui_cmt_mzfsize_t
{
    char *filename;
    int valid_size;
    int file_size;
} ui_cmt_mzfsize_t;

ui_cmt_mzfsize_t g_ui_cmt_mzfsize = {NULL, 0, 0};

/**
 * @brief Zámek g_ui_cmt_mzfsize.
 *
 * Popup aktivuje jádro kazety z emu vlákna (otevření pásky, načtení
 * souboru CMT hackem uvnitř instrukce), kreslí ho UI vlákno z kopie.
 */
static GMutex s_mzfsize_lock;

/**
 * @brief Otevře popup s varováním o větším souboru MZF (hook jádra kazety).
 *
 * @param filename Cesta k souboru (zkopíruje se).
 * @param valid_size Velikost podle hlavičky MZF.
 * @param file_size Skutečná velikost souboru.
 * @note Libovolné vlákno (typicky emu vlákno); údaje chrání s_mzfsize_lock.
 */
void imgui_cmt_fix_mzfsize_popup_activate(const char *filename, int valid_size, int file_size)
{
    g_print("MZF file size check: %s (%d) > %d\n", filename, file_size, valid_size);

    g_mutex_lock(&s_mzfsize_lock);
    g_free((gpointer)g_ui_cmt_mzfsize.filename);
    g_ui_cmt_mzfsize.filename = g_strdup(filename);
    g_ui_cmt_mzfsize.valid_size = valid_size;
    g_ui_cmt_mzfsize.file_size = file_size;
    g_mutex_unlock(&s_mzfsize_lock);
    g_gui->showCmtFixMzfsizeWindow = true;
}

void imgui_cmt_fix_mzfsize_saveas_cb(baseui_fchooser_t *fch)
{
    ui_cmt_mzfsize_t *srcFile = (ui_cmt_mzfsize_t *)fch->user_data;

    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        g_free(srcFile->filename);
        g_free(srcFile);
        return;
    };

    char *new_filepath = fch->selected_filePathName;
    if (new_filepath)
    {
        // ziskame priponu souboru
        const char *fileext = baseui_filechooser_get_extension_ptr(new_filepath);
        if (fileext != NULL)
        {
            if ((0 != strcasecmp(fileext, ".mzf")) && (0 != strcasecmp(fileext, ".m12")) && (0 != strcasecmp(fileext, ".mzt")))
            {
                fileext = NULL;
            };
        };

        if (fileext == NULL)
        {
            GString *gs = g_string_new(0);
            int i = 0;

            FILE *tst_fh = NULL;

            do
            {
                if (i > 100)
                    break;
                g_string_printf(gs, "%s", new_filepath);
                if (i != 0)
                {
                    g_string_append_printf(gs, "_%d", i);
                };
                i++;
                g_string_append(gs, ".mzf");
                tst_fh = baseui_tools_file_open(gs->str, "rb+");
            } while (tst_fh);

            if (tst_fh)
            {
                g_string_free(gs, TRUE);
                baseui_tools_file_close(tst_fh);
                fprintf(stderr, "%s(%d) - ERROR: Can't save to file: %s.mzf\n", __func__, __LINE__, new_filepath);
                free(new_filepath);
                new_filepath = NULL;
            }
            else
            {
                free(new_filepath);
                new_filepath = g_string_free(gs, FALSE);
            };
        };

        if (new_filepath)
        {
            FILE *fh_src = baseui_tools_file_open(srcFile->filename, "rb");
            if (fh_src)
            {
                uint8_t *buffer = g_new(uint8_t, srcFile->valid_size);
                if ((unsigned)srcFile->valid_size == baseui_tools_file_read(buffer, 1, srcFile->valid_size, fh_src))
                {
                    FILE *fh_dst = baseui_tools_file_open(new_filepath, "wb");
                    if (fh_dst)
                    {
                        if ((unsigned)srcFile->valid_size != baseui_tools_file_write(buffer, 1, srcFile->valid_size, fh_dst))
                        {
                            fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
                        }
                        else
                        {
                            g_print("Fixed MZF file saved as: %s\n", new_filepath);
                        };
                        baseui_tools_file_close(fh_dst);
                    }
                    else
                    {
                        fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
                    };
                }
                else
                {
                    fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
                };

                baseui_tools_file_close(fh_src);
                free(buffer);
            }
            else
            {
                fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
            };
        };
    };

    baseui_filechooser_destroy(fch);
    g_free(srcFile->filename);
    g_free(srcFile);
}

void imgui_cmt_fix_mzfsize_popup(bool *p_open)
{
    /* Kopie údajů: activate je smí z emu vlákna kdykoli přepsat. */
    g_mutex_lock(&s_mzfsize_lock);
    ui_cmt_mzfsize_t cur = g_ui_cmt_mzfsize;
    cur.filename = g_strdup(g_ui_cmt_mzfsize.filename ? g_ui_cmt_mzfsize.filename : "");
    g_mutex_unlock(&s_mzfsize_lock);

    ImGui::OpenPopup(_L("MZF Size Warning"));

    if (ImGui::BeginPopupModal(_L("MZF Size Warning"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::BeginTable("MZFInfoTable", 3, ImGuiTableFlags_SizingFixedFit);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Colon", ImGuiTableColumnFlags_WidthFixed, 10.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text(_("MZF file"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text(":");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", cur.filename);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text(_("Exceeded bytes"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text(":");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", cur.file_size - cur.valid_size);

        ImGui::EndTable();

        ImGui::Text(" ");
        ImGui::Separator();
        ImGui::Text(" ");

        ImGui::TextWrapped(_("This MZF file is a bit bigger than it should be."));
        ImGui::Text(" ");
        ImGui::TextWrapped(_("This could arise, for example, when the MZF file is exported from cp/m."));
        ImGui::TextWrapped(_("You can ignore it, or truncate the original file."));
        ImGui::TextWrapped(_("Is recommended to use the Save as, for create truncated copy of this file."));

        ImGui::Text(" ");
        ImGui::Separator();
        ImGui::Text(" ");

        bool check_enabled = CMT_TEST_MZFSIZE_CHECK_ENABLED;
        if (ImGui::Checkbox(_L("MZF size check enabled"), &check_enabled))
        {
            cmt_mzfsize_check_set((CMT_TEST_MZF_SIZE_CHECK_ENABLED) ? CMT_MZFSIZE_CHECK_DISABLED : CMT_MZFSIZE_CHECK_ENABLED);
        };

        ImGui::Text(" ");

        // Tlačítka
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float button_width = 100;
        float total_width = button_width * 3 + spacing * 2;

        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - total_width) * 0.5f);

        if (ImGui::Button(_L("Ignore"), ImVec2(button_width, 0)))
        {
            // Akce ignorování
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Truncate"), ImVec2(button_width, 0)))
        {
            // Akce zkrácení
            *p_open = false;
            ImGui::CloseCurrentPopup();

            if (0 != baseui_tools_file_truncate(cur.filename, cur.valid_size))
            {
                fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
            }
            else
            {
                g_print("MZF file truncated.\n");
            };
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Save As"), ImVec2(button_width, 0)))
        {
            // Akce uložení jako
            *p_open = false;
            ImGui::CloseCurrentPopup();

            ui_cmt_mzfsize_t *srcFile = g_new0(ui_cmt_mzfsize_t, 1);
            srcFile->filename = g_strdup(cur.filename);
            srcFile->valid_size = cur.valid_size;
            srcFile->file_size = cur.file_size;

            const char *title = _("Save MZF file as");
            baseui_fchooser_t *fch = baseui_filechooser_open_rw_file(BASEUI_FCHOOSER_CAT_MZF, title, ".mzf, .m12, .mzt", NULL, NULL, "newfile.mzf", imgui_cmt_fix_mzfsize_saveas_cb, srcFile);
            if (!fch)
            {
                fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
            };
        }

        ImGui::EndPopup();
    };
    g_free(cur.filename);
}
