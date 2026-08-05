#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "version_check/version_check.h"

#include "emulator/mzarch/mzcommon_config.h"
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "ui-imgui/mcp_activity/mcp_tools_menu.h"
#endif

extern "C"
{
    void imgui_menu_tools(void);
} // extern "C"

void imgui_menu_tools(void)
{
    // Sekce Speed
    if (ImGui::BeginMenu(_L("Tools")))
    {
        if (ImGui::MenuItem(_L("DSK Images..."), NULL, false, true))
        {
            g_gui->showDskCreateWindow = true;
        };

        if (ImGui::BeginMenu(_L("Version Check")))
        {
            if (ImGui::MenuItem(_L("Make Online Version Check")))
            {
                version_check_run_test(true);
            };

            if (ImGui::MenuItem(_L("Version Check Setup")))
            {
                g_gui->showVersionCheckSetupWindow = true;
            };

            ImGui::EndMenu();
        };

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
        // V0.A.5: MCP TCP Server start/stop + status
        imgui_menu_mcp_tools_render();
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
        // MCP Activity okno (přesunuto z Debugger menu). Real-time log
        // MCP akcí. Gating MCP_SERVER (NE jen TCP) - okno funguje i bez
        // TCP serveru (pipe transport). Per-session, bez globální zkratky.
        if (ImGui::MenuItem(_L("MCP Activity##menu_mcp_activity"), NULL,
                            g_gui->showMcpActivityWindow))
        {
            g_gui->showMcpActivityWindow = !g_gui->showMcpActivityWindow;
        };
#endif

        ImGui::EndMenu();
    };
}
