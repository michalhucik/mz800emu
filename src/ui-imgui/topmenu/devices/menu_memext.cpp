#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"
#include "ui-imgui/memext/memext_content.h"
#include "emulator/hw-generic/memory/memext.h"
#include "emulator/hw-generic/memory/memext_request.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_menu_memext(void);
}

/**
 * @brief Callback výběru souboru FLASH Luftner: načtení zařadí do emu vlákna.
 * @param fch Dialog (zničí se zde).
 */
void imgui_memext_luftner_select_flash_filepath_cb(baseui_fchooser_t *fch)
{
    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    char *filename = fch->selected_filePathName;
    if (filename)
    {
        memext_request_set_flash_filepath(filename);
    };

    baseui_filechooser_destroy(fch);
}

void imgui_memext_luftner_select_flash_filepath(void)
{
    baseui_fchooser_t *fch;

    const char *title = _("Select FLASH image file to open");
    fch = baseui_filechooser_open_file(BASEUI_FCHOOSER_CAT_RAMDISK, title, ".dat, .*", NULL, NULL, MEMEXT_LUFTNER_GET_FLASH_FILEPATH(), imgui_memext_luftner_select_flash_filepath_cb, NULL);

    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

void imgui_menu_memext(void)
{
    if (ImGui::BeginMenu(_L("MemExt")))
    {

        if (MenuRadioItem(_("Not Connected"), !MEMEXT_TEST_CONNECTED))
        {
            memext_request_set_connection(MEMEXT_REQUEST_DISCONNECT);
        };

        if (MenuRadioItem(_("Model - 'Peroutka & Hučík'"), MEMEXT_TEST_CONNECTED_PEHU))
        {
            memext_request_set_connection(MEMEXT_TYPE_PEHU);
        };

        if (MenuRadioItem(_("Model - 'Luftner'"), MEMEXT_TEST_CONNECTED_LUFTNER))
        {
            memext_request_set_connection(MEMEXT_TYPE_LUFTNER);
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("RAM Initialization on Boot")))
        {

            if (MenuRadioItem(_("Filled of Zeros"), MEMEXT_TEST_INIT_FILL_NULL))
            {
                MEMEXT_SET_INIT_FILL(MEMEXT_INIT_MEM_NULL);
            };

            if (MenuRadioItem(_("Filled of Random Values"), MEMEXT_TEST_INIT_FILL_RANDOM))
            {
                MEMEXT_SET_INIT_FILL(MEMEXT_INIT_MEM_RANDOM);
            };

            if (MenuRadioItem(_("Filled as Sharp DRAM"), MEMEXT_TEST_INIT_FILL_SHARP))
            {
                MEMEXT_SET_INIT_FILL(MEMEXT_INIT_MEM_SHARP);
            };

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Luftner Forced Auto Init"), NULL, MEMEXT_TEST_LUFTNER_AUTO_INIT, MEMEXT_TEST_CONNECTED_LUFTNER))
        {
            MEMEXT_SET_INIT_LUFTNER((!MEMEXT_TEST_LUFTNER_AUTO_INIT) ? MEMEXT_INIT_LUFTNER_RESET : MEMEXT_INIT_LUFTNER_NONE);
        };

        if (ImGui::BeginMenu(_L("Luftner FLASH Image"), MEMEXT_TEST_CONNECTED_LUFTNER))
        {
            GString *str = g_string_new(_("File: "));

            const char *basename = g_path_get_basename(MEMEXT_LUFTNER_GET_FLASH_FILEPATH());
            // pokud ma vice jak 20 znaku, tak zkratime a na konec pridame "..." - aby se veslo do menu
            if (strlen(basename) > 20)
            {
                g_string_append_printf(str, "%s...", g_utf8_substring(basename, 0, 20));
            }
            else
            {
                g_string_append(str, basename);
            };
            g_free((gpointer)basename);

            ImGui::MenuItem(str->str, NULL, false, false);

            if (ImGui::MenuItem(_L("Select FLASH Autoload File..."), NULL, false))
            {
                imgui_memext_luftner_select_flash_filepath();
            };

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Memory Map Settings..."), NULL, false, MEMEXT_TEST_CONNECTED))
        {
            g_gui->showMemextMapWindow = true;
        };

        if (ImGui::BeginMenu(_L("Memory Content"), MEMEXT_TEST_CONNECTED))
        {
            if (ImGui::MenuItem(_L("Load MemExt RAM From File...")))
            {
                imgui_memext_open_load(MEMEXT_CONTENT_RAM);
            };

            if (ImGui::MenuItem(_L("Save MemExt RAM Into File...")))
            {
                imgui_memext_open_save(MEMEXT_CONTENT_RAM);
            };

            ImGui::Separator();

            if (ImGui::MenuItem(_L("Load MemExt FLASH From File..."), NULL, false, MEMEXT_TEST_CONNECTED_LUFTNER))
            {
                imgui_memext_open_load(MEMEXT_CONTENT_FLASH);
            };

            if (ImGui::MenuItem(_L("Save MemExt FLASH Into File..."), NULL, false, MEMEXT_TEST_CONNECTED_LUFTNER))
            {
                imgui_memext_open_save(MEMEXT_CONTENT_FLASH);
            };

            ImGui::EndMenu();
        };

        ImGui::EndMenu();
    };
}
