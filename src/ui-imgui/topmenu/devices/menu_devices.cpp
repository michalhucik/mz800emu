#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "mzarch/mzarch_config.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#if MZARCH == 700
#include "ui-imgui/mz700/devbus/menu_devbus.h"
#endif

extern "C"
{
    void imgui_menu_devices(void);
}

void imgui_menu_devices(void)
{
    // Sekce Devices
    if (ImGui::BeginMenu(_L("HW & Devices")))
    {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /*
         * Bus Map (plán D6 bus-model) - kdo vlastní který port a mapovanou
         * adresu (čipy a periferie desky, na MZ-700 i zařízení devbus),
         * výstupní stupeň, kolize, aktivita. Okno žije v debuggeru, položka
         * je tady, protože je to přehled "co je kde" mezi zařízeními.
         * Všechny platformy, bez zkratky.
         */
        if (ImGui::MenuItem(_L("Bus Map"), NULL, g_gui->showBusMapWindow))
        {
            g_gui->showBusMapWindow = !g_gui->showBusMapWindow;
        };
        ImGui::Separator();
#endif
        imgui_menu_cmt();
        imgui_menu_fdcontroller();

        imgui_menu_ramdisk();
        imgui_menu_qdisk();
        imgui_menu_unicard();
        imgui_menu_ide8();
        imgui_menu_memext();
        imgui_menu_rom();

#if (MZARCH == 800) || (MZARCH == 1500)
        // Tiskárna (capture + plotter) existuje jen na architekturách se Z80 PIO
        imgui_menu_printer();
#endif

        imgui_menu_mz800_dip_switch();

#if MZARCH == 700
        // Volitelná zařízení devbus (bez registrovaného zařízení se nezobrazí nic)
        imgui_menu_devbus();
#endif

#if MZARCH == 800
        ImGui::Separator();
        imgui_menu_mz800_hw_compat();
#endif

        ImGui::EndMenu();
    };
}
