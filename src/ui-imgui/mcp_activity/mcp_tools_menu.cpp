/*
 * File:   mcp_tools_menu.cpp
 *
 * GUI integration MCP TCP serveru do Tools menu (V0.A.5).
 *
 * Renderuje submenu "MCP TCP Server" v top-menu Tools. Submenu poskytuje
 * runtime toggle (Start / Stop) listeneru + read-only status (port,
 * počet klientů).
 *
 * Globální handle `g_mcp_tcp_server` se inicializuje lazy při prvním
 * kliknutí na Start (pokud nebyl už alokován v `main` cestě
 * `--mcp-tcp-port=N`). Persistence stavu mezi spuštěními emu žádná -
 * vždy ruční Start.
 *
 * Port collision detekce: pokud `_start` vrátí `PORT_IN_USE`, log se
 * zaznamená na stderr a v ImGui zobrazíme red text v menu (= dočasná
 * V0 cesta, V1+ modal popup).
 *
 * Při buildu s `NO_MCP_TCP=1` (resp. cascade z `NO_MCP=1`) je tělo
 * funkce no-op.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"

#include "libs/imgui/imgui.h"

#include "i18n.h"
#include "mcp_tools_menu.h"

#include "emulator/mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "emulator/mcp/tcp_server.h"
#include "emulator/mcp/mcp_config.h"
#include "ui-imgui/mcp_activity/mcp_settings_dialog.h"
#include "ui-imgui/bootstrap/myimgui.h"
#endif


extern "C" {

void imgui_menu_mcp_tools_render(void)
{
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED

    /* Flag pro perzistentní indikaci port collision od posledního Start
     * pokusu. Vyresetuje se při příštím úspěšném startu nebo při Stop. */
    static bool s_last_start_port_in_use = false;
    static bool s_last_start_other_error = false;

    if (ImGui::BeginMenu(_L("MCP TCP Server##menu_mcp_tcp")))
    {
        bool running = g_mcp_tcp_server &&
                       mcp_tcp_server_is_running(g_mcp_tcp_server);

        if (!running)
        {
            if (ImGui::MenuItem(_L("Start##mcp_tcp_start")))
            {
                if (!g_mcp_tcp_server)
                {
                    g_mcp_tcp_server = mcp_tcp_server_create();
                }
                if (g_mcp_tcp_server)
                {
                    /* V0.B.4: použij INI / GUI konfigurované hodnoty
                     * (port + bind) místo hardcoded defaultu. */
                    const char *bind = mcp_config_bind_addr_str(
                        g_mcp_config.bind_addr);
                    en_MCP_TCP_START_RESULT r = mcp_tcp_server_start(
                        g_mcp_tcp_server, g_mcp_config.tcp_port, bind);
                    s_last_start_port_in_use = (r == MCP_TCP_START_PORT_IN_USE);
                    s_last_start_other_error = (r != MCP_TCP_START_OK &&
                                                r != MCP_TCP_START_PORT_IN_USE);
                }
                else
                {
                    s_last_start_other_error = true;
                }
            }

            if (s_last_start_port_in_use)
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                    "%s: %d",
                    _("Last start failed: port already in use"),
                    g_mcp_config.tcp_port);
            }
            else if (s_last_start_other_error)
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                    "%s",
                    _("Last start failed (see console log)"));
            }
        }
        else
        {
            if (ImGui::MenuItem(_L("Stop##mcp_tcp_stop")))
            {
                mcp_tcp_server_stop(g_mcp_tcp_server);
                s_last_start_port_in_use = false;
                s_last_start_other_error = false;
            }
            ImGui::Separator();
            ImGui::Text("%s: %s:%d",
                _("Listening on"),
                mcp_config_bind_addr_str(g_mcp_config.bind_addr),
                mcp_tcp_server_get_port(g_mcp_tcp_server));
            ImGui::Text("%s: %d",
                _("Clients"),
                mcp_tcp_server_get_client_count(g_mcp_tcp_server));
        }

        /* V0.B.4: Settings... otevírá konfigurační dialog. */
        ImGui::Separator();
        if (ImGui::MenuItem(_L("Settings...##mcp_tcp_settings")))
        {
            g_gui->showMcpSettingsWindow = true;
        }

        ImGui::EndMenu();
    }
#endif /* MZ800EMU_CFG_MCP_TCP_ENABLED */
}

} /* extern "C" */
