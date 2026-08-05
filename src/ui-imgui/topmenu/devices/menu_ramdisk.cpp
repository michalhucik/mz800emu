#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"
#include "emulator/mzarch/mzhal.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"

#include "emulator/hw-generic/ramdisk/ramdisk.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_menu_ramdisk(void);
}

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
        ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, g_ramdisk.std.size, filename);
    };

    baseui_filechooser_destroy(fch);
}

void imgui_ramdisk_std_open_file(void)
{
    baseui_fchooser_t *fch;

    if (RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_SRAM))
    {
        const char *title = _("Select SRAM disk image file to open");
        fch = baseui_filechooser_open_rw_file(title, ".dat, .*", NULL, NULL, g_ramdisk.std.filepath, imgui_ramdisk_std_open_file_cb, NULL);
    }
    else
    {
        const char *title = _("Select ROM disk image file to open");
        fch = baseui_filechooser_open_file(title, ".dat, .*", NULL, NULL, g_ramdisk.std.filepath, imgui_ramdisk_std_open_file_cb, NULL);
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
            RAMDISK_DISCONNECT_ALL();
        };

        bool std_is_dissabled = (RAMDISK_TEST_PEZIK_E8_CONNECTED);

        if (std_is_dissabled)
        {
            ImGui::BeginDisabled();
        };

        if (MenuRadioItem(_("MR-1R18 Compatible"), RAMDISK_TEST_STD_CONNECTED))
        {
            RAMDISK_DISCONNECT_ALL();
            ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, g_ramdisk.std.size, g_ramdisk.std.filepath);
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

/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
        if (g_mzhal.arch != 1500
            && MenuRadioItem(_("Pezik 0xE8 - 0xEF"), RAMDISK_TEST_PEZIK_E8_CONNECTED))
        {
            RAMDISK_DISCONNECT_ALL();
            ramdisk_pezik_connect(RAMDISK_PEZIK_E8);
        };

        if (pezik_is_dissabled)
        {
            ImGui::EndDisabled();
        };

        if (MenuRadioItem(_("Pezik 0x68 - 0x6F"), RAMDISK_TEST_PEZIK_68_CONNECTED))
        {
            RAMDISK_DISCONNECT_ALL();
            ramdisk_pezik_connect(RAMDISK_PEZIK_68);
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Pezik Settings..."), NULL, false, true))
        {
            g_gui->showPezikSettingsWindow = true;
        };

        ImGui::SeparatorText(_("MR-1R18 Options"));

        if (ImGui::BeginMenu(_L("Size"), RAMDISK_TEST_STD_CONNECTED))
        {
            if (MenuRadioItem(_("64 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_64)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, RAMDISK_SIZE_64, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("256 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_256)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, RAMDISK_SIZE_256, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("512 KB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_512)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, RAMDISK_SIZE_512, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("1 MB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_1M)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, RAMDISK_SIZE_1M, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("16 MB"), RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_16M)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, g_ramdisk.std.type, RAMDISK_SIZE_16M, g_ramdisk.std.filepath);
            };

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(_L("Type"), RAMDISK_TEST_STD_CONNECTED))
        {
            if (MenuRadioItem(_("Unbacked RAM Disk"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_STD)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, RAMDISK_TYPE_STD, g_ramdisk.std.size, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("SRAM Emulation"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_SRAM)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, RAMDISK_TYPE_SRAM, g_ramdisk.std.size, g_ramdisk.std.filepath);
            };

            if (MenuRadioItem(_("ROM Disk"), RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_ROM)))
            {
                ramdisk_std_init(RAMDISK_CONNECTED, RAMDISK_TYPE_ROM, g_ramdisk.std.size, g_ramdisk.std.filepath);
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
