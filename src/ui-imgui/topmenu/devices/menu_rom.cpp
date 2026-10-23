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
#include "emulator/emulator.h"
#include "emulator/hw-generic/memory/rom.h"
#include "emulator/hw-generic/memory/rom_request.h"

extern "C"
{
    void imgui_menu_rom(void);
}

void imgui_menu_rom(void)
{
    if (ImGui::BeginMenu(_L("ROM")))
    {
        st_ROM_CONFIG_ROW *row;
        for (row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++)
        {
            if (!row->enabled) continue;

            /* Přeskočit development ROM v normálním módu */
            if ((row->flags & ROM_CONFIG_FLAG_DEVELOPMENT) && !EMULATOR_TEST_DEVELOPMENT_MODE) continue;

            if (row->type == ROM_CONFIG_TYPE_SECTION)
            {
                if (row->description)
                    ImGui::SeparatorText(_(row->description));
                else
                    ImGui::Separator();
                continue;
            }

            if (row->type == ROM_CONFIG_TYPE_ROM)
            {
                if (MenuRadioItem(_(row->description), ROM_TEST_CURRENT(row)))
                {
                    rom_request_select(row);
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Settings for User Defined..."), NULL, false, true))
        {
            g_gui->showRomSettingsWindow = true;
        }

        ImGui::EndMenu();
    };
}
