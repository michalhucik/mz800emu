/**
 * @file cmt_fix_mzfsize.cpp
 * @brief Popup s varováním o souboru MZF delším, než udává jeho hlavička.
 *
 * Jádro kazety (kontrola velikosti MZF) popup aktivuje přes
 * imgui_cmt_fix_mzfsize_popup_activate(); UI vlákno ho kreslí a nabízí
 * ignorovat, zkrátit původní soubor nebo uložit zkrácenou kopii. S volbou
 * automatické opravy (g_cmt.mzfsize_autofix) UI vlákno soubor zkrátí bez
 * dotazu a popup se neukáže.
 */
#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/cmt/imgui_cmt.h"
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
#include "ui-imgui/cmt/cmt_view.h"

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
 * @brief Stabilní ID popupu MZF Size Warning (část za "###" se nepřekládá).
 *
 * Otevření (imgui_modal_open_when_free()) i BeginPopupModal() s přeloženým
 * titulkem dají totéž ID v každém jazyce.
 */
#define CMT_MZFSIZE_POPUP_ID "###cmt_mzf_size_warning"

/**
 * @brief Zámek g_ui_cmt_mzfsize.
 *
 * Popup aktivuje jádro kazety z emu vlákna (otevření pásky, načtení
 * souboru CMT hackem uvnitř instrukce), kreslí ho UI vlákno z kopie.
 */
static GMutex s_mzfsize_lock;

/**
 * @brief U aktuálního souboru je rozhodnuto ptát se popupem (pod s_mzfsize_lock).
 *
 * Nová aktivace příznak nuluje; první kreslení po ní rozhodne: s volbou
 * automatické opravy soubor zkrátí, jinak (nebo když zkrácení selže)
 * příznak nastaví a ukáže se popup. Zaškrtnutí "Fix automatically next
 * time" v otevřeném popupu tak platí až pro další soubor.
 */
static bool s_mzfsize_ask = false;

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
    s_mzfsize_ask = false;
    g_mutex_unlock(&s_mzfsize_lock);
    g_gui->showCmtFixMzfsizeWindow = true;
}

/**
 * @brief Callback dialogu "Save MZF file as" - uloží MZF zkrácený na platnou délku.
 *
 * Pokud uživatel dialog potvrdil, zkopíruje z původního souboru prvních
 * valid_size bajtů (velikost podle hlavičky MZF) do zvoleného souboru.
 * Cílová cesta bez přípony .mzf, .m12 nebo .mzt dostane příponu .mzf;
 * existuje-li takový soubor, zkouší se jména s příponou _1 .. _100.
 * Chyby čtení, zápisu a alokace se jen vypíší na stderr; výsledek se
 * volajícímu nevrací.
 *
 * @param fch Dialog výběru souboru; fch->user_data je ui_cmt_mzfsize_t
 *            alokovaný přes g_new0 (filename přes g_strdup).
 *
 * Vlastnictví: funkce přebírá fch i user_data. Na všech cestách volá
 * baseui_filechooser_destroy(fch) a uvolní srcFile->filename i srcFile
 * (g_free); po návratu jsou fch i user_data neplatné. Pracovní buffer
 * (g_new) uvolní g_free.
 *
 * Cestu fch->selected_filePathName (g_strdup v imgui_filechooser.cpp)
 * funkce převezme (v fch ji vynuluje, aby ji baseui_filechooser_destroy()
 * neuvolnila podruhé) a výslednou cestu (převzatou, nebo novou
 * z g_string_free(gs, FALSE) s doplněnou příponou) uvolní sama přes g_free.
 * Dříve se ve větvi bez platné přípony převzatá cesta uvolnila free()
 * a pak znovu v baseui_filechooser_destroy() (dvojí uvolnění) a nová
 * cesta se neuvolnila vůbec. Soubor otevřený při zkoušení, zda jméno
 * existuje, se hned zavře (dříve zůstával otevřený, až 100 FILE*).
 */
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

    /* převzetí cesty: destroy ji pak neuvolní (vzor imgui_cmt_tape_save_as_cb) */
    char *new_filepath = fch->selected_filePathName;
    fch->selected_filePathName = NULL;
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
            bool name_free = false;

            /* první neexistující jméno <cesta>.mzf, <cesta>_1.mzf .. <cesta>_100.mzf;
             * otevřený zkušební soubor se hned zavře (dříve zůstal otevřený) */
            for (int i = 0; i <= 100; i++)
            {
                g_string_printf(gs, "%s", new_filepath);
                if (i != 0)
                {
                    g_string_append_printf(gs, "_%d", i);
                };
                g_string_append(gs, ".mzf");
                FILE *tst_fh = baseui_tools_file_open(gs->str, "rb+");
                if (!tst_fh)
                {
                    name_free = true;
                    break;
                };
                baseui_tools_file_close(tst_fh);
            };

            if (!name_free)
            {
                g_string_free(gs, TRUE);
                fprintf(stderr, "%s(%d) - ERROR: Can't save to file: %s.mzf\n", __func__, __LINE__, new_filepath);
                g_free(new_filepath);
                new_filepath = NULL;
            }
            else
            {
                g_free(new_filepath);
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
                g_free(buffer);
            }
            else
            {
                fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
            };
        };
    };

    g_free(new_filepath);
    baseui_filechooser_destroy(fch);
    g_free(srcFile->filename);
    g_free(srcFile);
}

/**
 * @brief Vykreslí modální popup "MZF Size Warning" (MZF větší než podle hlavičky).
 *
 * Nabízí Ignore, Truncate (zkrátí původní soubor na valid_size) a Save As
 * (dialog pro uložení zkrácené kopie; zpracuje ho
 * imgui_cmt_fix_mzfsize_saveas_cb()). Údaje bere z kopie
 * g_ui_cmt_mzfsize pořízené pod s_mzfsize_lock.
 *
 * S volbou automatické opravy (g_cmt.mzfsize_autofix, zaškrtávátko
 * "Fix automatically next time" v popupu a položka menu CMT Options) se
 * popup neukáže: soubor se hned zkrátí na valid_size, výsledek ohlásí
 * řádek zpráv okna Virtual CMT (cmt_view_post(), informace) a výpis na
 * stdout. Když zkrácení selže, ukáže se popup jako bez volby.
 *
 * @param p_open Příznak zobrazení okna; tlačítka ho nastaví na false.
 *
 * Vlastnictví: pro Save As alokuje ui_cmt_mzfsize_t (g_new0, filename
 * g_strdup) a předá ho dialogu jako user_data; uvolní ho callback, a když
 * dialog vůbec nevznikne (NULL), uvolní ho tato funkce.
 * @pre UI vlákno uvnitř ImGui snímku.
 */
void imgui_cmt_fix_mzfsize_popup(bool *p_open)
{
    /* Kopie údajů: activate je smí z emu vlákna kdykoli přepsat. */
    g_mutex_lock(&s_mzfsize_lock);
    ui_cmt_mzfsize_t cur = g_ui_cmt_mzfsize;
    cur.filename = g_strdup(g_ui_cmt_mzfsize.filename ? g_ui_cmt_mzfsize.filename : "");
    bool ask = s_mzfsize_ask;
    s_mzfsize_ask = true;
    g_mutex_unlock(&s_mzfsize_lock);

    /* automatická oprava (rozhoduje první kreslení po aktivaci): zkrátit bez
     * dotazu; popup jen když zkrácení selže */
    if (!ask && g_cmt.mzfsize_autofix)
    {
        if (0 == baseui_tools_file_truncate(cur.filename, cur.valid_size))
        {
            g_print("MZF file truncated automatically: %s (%d -> %d)\n", cur.filename, cur.file_size, cur.valid_size);
            char *base = g_path_get_basename(cur.filename);
            char *msg = g_strdup_printf(_("MZF file %s was truncated by %d bytes to the size in its header."), base,
                                        cur.file_size - cur.valid_size);
            cmt_view_post(msg, CMT_VIEW_MSG_INFO);
            g_free(msg);
            g_free(base);
            *p_open = false;
            g_free(cur.filename);
            return;
        };
        fprintf(stderr, "%s(%d) - Error: %s\n", __func__, __LINE__, baseui_tools_strerror());
    };

    /* jen při volnu: jinak by OpenPopup() zavřel jiný otevřený modál (imgui_modal.h);
     * ID nezávislé na překladu titulku ("###") */
    imgui_modal_open_when_free(CMT_MZFSIZE_POPUP_ID);

    char title[160];
    snprintf(title, sizeof(title), "%s%s", _("MZF Size Warning"), CMT_MZFSIZE_POPUP_ID);
    /* uprostřed okna Virtual CMT a v jeho viewportu */
    imgui_cmt_window_modal_next_pos();
    if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
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

        /* předvolba: příště zkrátit bez dotazu (jen se zapnutou kontrolou) */
        ImGui::BeginDisabled(!check_enabled);
        bool autofix = g_cmt.mzfsize_autofix != 0;
        if (ImGui::Checkbox(_L("Fix automatically next time"), &autofix))
        {
            cmt_mzfsize_autofix_set(autofix ? 1 : 0);
        };
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", _("An MZF file longer than its header says is truncated\n"
                                      "to the right size without asking."));
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
                /* dialog nevznikl a callback se nezavolá: srcFile uvolnit tady */
                fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
                g_free(srcFile->filename);
                g_free(srcFile);
            };
        }

        ImGui::EndPopup();
    };
    g_free(cur.filename);
}
