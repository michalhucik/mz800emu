#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>
#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "mzarch/mzarch_platform_functions.h"
// #include "iface/iface_video.h"
#include "topmenu.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

// #include "iface/iface_joy.h"
// #endif

extern "C"
{
    void imgui_topmenu_handler(void);
    void imgui_topmenu_body(void);
} // extern "C"

void imgui_topmenu_body(void)
{
    imgui_menu_devices();
    imgui_menu_interface();
    imgui_menu_speed();
    imgui_menu_emulator();
    imgui_menu_snapshot();
    ImGui::Separator();

    if ((ImGui::MenuItem(_L("Pause Emulation"), "Alt+P", EMULATOR_TEST_PAUSED)) || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_P))
    {
        dbg_ui_pause_toggle();
    };

    if ((ImGui::MenuItem(_L("Reset"), "F12")) || ImGui::Shortcut(ImGuiKey_F12))
    {
        mzarch_platform_fn_reset_request();
    }

    ImGui::Separator();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    imgui_menu_debugger(DBG_MENU_CALLER_TOPMENU);
    ImGui::Separator();
#endif

    imgui_menu_tools();

    ImGui::Separator();

    if (ImGui::MenuItem(_L("About...")))
    {
        g_gui->showAboutWindow = true;
    };

    ImGui::EndPopup();
}

void imgui_topmenu_handler(void) // pouzivame pokud je emulator zobrazen jako pozadi
{
    if (ImGui::BeginPopupContextVoid("EmulatorPopupMenu", ImGuiPopupFlags_MouseButtonRight))
    {
        imgui_topmenu_body();
    }
}
