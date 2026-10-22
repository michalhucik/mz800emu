#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"

#include "emulator/hw-generic/ramdisk/ramdisk.h"
#include "emulator/hw-generic/ramdisk/ramdisk_request.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_menu_ramdisk(void);
}

/**
 * @brief Callback výběru souboru MZ-1R18: novou konfiguraci zařadí do emu vlákna.
 * @param fch Dialog (zničí se zde).
 */
void imgui_ramdisk_std_open_file_cb(baseui_fchooser_t *fch)
{
    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    char *filename = fch->selected_filePathName;
    if (filename)
    {
        ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, filename, false);
    };

    baseui_filechooser_destroy(fch);
}

void imgui_ramdisk_std_open_file(void)
{
    baseui_fchooser_t *fch;

    if (RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_SRAM))
    {
        const char *title = _("Select SRAM disk image file to open");
        fch = baseui_filechooser_open_rw_file(BASEUI_FCHOOSER_CAT_RAMDISK, title, ".dat, .*", NULL, NULL, g_ramdisk.std.filepath, imgui_ramdisk_std_open_file_cb, NULL);
    }
    else
    {
        const char *title = _("Select ROM disk image file to open");
        fch = baseui_filechooser_open_file(BASEUI_FCHOOSER_CAT_RAMDISK, title, ".dat, .*", NULL, NULL, g_ramdisk.std.filepath, imgui_ramdisk_std_open_file_cb, NULL);
    };

    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

void imgui_menu_ramdisk(void)
{
    if (ImGui::BeginMenu(_L("Memory Disk")))
    {
        if (MenuRadioItem(_("Not Connected"), RAMDISK_TEST_NOT_CONNECTED))
        {
            ramdisk_request_disconnect_all();
        };

        bool std_is_dissabled = (RAMDISK_TEST_PEZIK_E8_CONNECTED);

        if (std_is_dissabled)
        {
            ImGui::BeginDisabled();
        };

        if (MenuRadioItem(_("MZ-1R18 Compatible"), RAMDISK_TEST_STD_CONNECTED))
        {
            ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, NULL, true);
        };

        if (std_is_dissabled)
        {
            ImGui::EndDisabled();
        };

        bool pezik_is_dissabled = (RAMDISK_TEST_STD_CONNECTED);

        if (pezik_is_dissabled)
        {
            ImGui::BeginDisabled();
        };

#if MZARCH != 1500
        if (MenuRadioItem(_("Pezik 0xE8 - 0xEF"), RAMDISK_TEST_PEZIK_E8_CONNECTED))
        {
            ramdisk_request_pezik_connect(RAMDISK_PEZIK_E8);
        };
#endif

        if (pezik_is_dissabled)
        {
            ImGui::EndDisabled();
        };

        if (MenuRadioItem(_("Pezik 0x68 - 0x6F"), RAMDISK_TEST_PEZIK_68_CONNECTED))
        {
            ramdisk_request_pezik_connect(RAMDISK_PEZIK_68);
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Pezik Settings..."), NULL, false, true))
        {
            g_gui->showPezikSettingsWindow = true;
        };

        ImGui::SeparatorText(_("MZ-1R18 Options"));

        if (ImGui::BeginMenu(_L("Size"), RAMDISK_TEST_STD_CONNECTED))
        {
            if (MenuRadioItem(_("64 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_64)))
            {
                ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_64, NULL, false);
            };

            if (MenuRadioItem(_("256 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_256)))
            {
                ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_256, NULL, false);
            };

            if (MenuRadioItem(_("512 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_512)))
            {
                ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_512, NULL, false);
            };

            if (MenuRadioItem(_("1 MB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_1M)))
            {
                ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_1M, NULL, false);
            };

            if (MenuRadioItem(_("16 MB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_16M)))
            {
                ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_16M, NULL, false);
            };

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(_L("Type"), RAMDISK_TEST_STD_CONNECTED))
        {
            if (MenuRadioItem(_("Unbacked RAM Disk"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_STD)))
            {
                ramdisk_request_std_set(RAMDISK_TYPE_STD, RAMDISK_REQUEST_KEEP, NULL, false);
            };

            if (MenuRadioItem(_("SRAM Emulation"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_SRAM)))
            {
                ramdisk_request_std_set(RAMDISK_TYPE_SRAM, RAMDISK_REQUEST_KEEP, NULL, false);
            };

            if (MenuRadioItem(_("ROM Disk"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_ROM)))
            {
                ramdisk_request_std_set(RAMDISK_TYPE_ROM, RAMDISK_REQUEST_KEEP, NULL, false);
            };

            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(_L("RAM Disk File..."), NULL, false, RAMDISK_TEST_STD_CONNECTED))
        {
            imgui_ramdisk_std_open_file();
        };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* memory-disk-state: debugger okno se stavem vsech ramdisku. */
        ImGui::Separator();
        if (ImGui::MenuItem(_L("Memory Disk State (debugger)..."), NULL,
                            g_gui->showRamdiskStateWindow, true))
        {
            g_gui->showRamdiskStateWindow = !g_gui->showRamdiskStateWindow;
        };
#endif

        ImGui::EndMenu();
    };
}
