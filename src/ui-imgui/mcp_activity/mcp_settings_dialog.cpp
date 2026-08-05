/*
 * File:   mcp_settings_dialog.cpp
 *
 * GUI dialog pro konfiguraci MCP backendu (V0.B.4).
 *
 * Implementace pravidel z INSTRUKCE V0.B.4:
 *  - Pri otevreni se z g_mcp_config nakopiruje lokální editační kopie.
 *  - Save: zapíše s_local zpět do g_mcp_config, zavolá cfgroot_save,
 *    pokud TCP server běží, restartuje ho s novými hodnotami.
 *  - Cancel / zavření křížkem: zahodí dočasný stav.
 *  - Bind != LOCALHOST a Profile != WILD generují varovný banner.
 *
 * Pattern (ImGui::Begin + NoCollapse + AlwaysAutoResize) je převzat
 * z mz800new snapshot_setup_dialog.cpp pro konzistenci s ostatními
 * konfiguračními okny v projektu.
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
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include "i18n.h"
#include "mcp_settings_dialog.h"

#include "emulator/mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
extern "C" {
#include "emulator/mcp/mcp_config.h"
#include "emulator/mcp/tcp_server.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
}
#endif


#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED

/**
 * @brief Lokální editační kopie konfigurace.
 *
 * Při fresh open (s_initialized == false) se z g_mcp_config nakopíruje
 * aktuální stav. Save kopíruje zpět; Cancel jen resetuje s_initialized.
 */
static st_MCP_CONFIG s_local;
static bool s_initialized = false;


/* Položky combo boxů. Texty jsou UI-facing v angličtině. N_() je
 * značkovací makro - registruje string do .pot pro extrakci xgettextem,
 * ale nepřekládá za běhu. Runtime překlad zajistí _() / _L() na callsite
 * (viz _(s_bind_items[idx]) v renderu). Bez N_() by xgettext stringy ve
 * statickém poli neviděl a combo by se nikdy nepřeložilo.
 *
 * Konvence tokenů: kanonické identifikátory (IP adresy, názvy profilů
 * wild/confined/sandboxed/observer, log úrovně OFF/ERROR/WARNING/INFO/
 * DEBUG) zůstávají v originále i v překladu; lokalizuje se jen popisná
 * část v závorce. */
static const char *const s_bind_items[] = {
    N_("127.0.0.1 (localhost only)"),
    N_("0.0.0.0 (all interfaces - RISKY)")
};

static const char *const s_profile_items[] = {
    N_("wild (no restrictions, dev default)"),
    N_("confined (workspace write-only) [V1.A]"),
    N_("sandboxed (whitelist + auth) [V1.A]"),
    N_("observer (read-only) [V1.A]")
};

/**
 * @brief Položky combo Log Level - musí odpovídat pořadí
 * en_MCP_WRAPPER_LOG_LEVEL.
 */
static const char *const s_wrapper_log_level_items[] = {
    N_("OFF (no log file)"),
    N_("ERROR"),
    N_("WARNING"),
    N_("INFO"),
    N_("DEBUG")
};

/**
 * @brief Položky combo Rotation Kind - musí odpovídat pořadí
 * en_MCP_WRAPPER_LOG_ROTATE_KIND.
 */
static const char *const s_wrapper_log_rotate_items[] = {
    N_("none (single append-only file)"),
    N_("size (rotate at max bytes)"),
    N_("time (rotate on time interval)")
};

/**
 * @brief Lokální editační bufery pro InputText polí.
 *
 * Sync s `s_local.wrapper_log_path` / `s_local.wrapper_log_rotate_when`
 * při fresh open dialogu. Save kopíruje zpět přes g_strdup.
 *
 * 1024 znaků pro cestu (Windows MAX_PATH = 260, ale s long path
 * podporou klidně víc). 64 pro when token (S/M/H/D/midnight/W0-W6).
 */
static char s_wrapper_log_path_buf[1024];
static char s_wrapper_log_when_buf[64];


/**
 * @brief Aplikuje editační kopii do globální konfigurace + persistuje.
 *
 * Volá se z Save tlačítka. Pokud TCP server právě běží, restartuje
 * ho s novými hodnotami (= uživatel okamžitě uvidí změnu v Tools menu).
 */
static void apply_and_save(void)
{
    /* Před přepsáním globálu uvolnit jeho heap stringy. s_local drží
     * vlastní heap kopie (g_strdup'nuté při fresh open + edit) - po
     * tomto přiřazení vlastnictví přechází na g_mcp_config. */
    g_free(g_mcp_config.action_log_path);
    g_free(g_mcp_config.wrapper_log_path);
    g_free(g_mcp_config.wrapper_log_rotate_when);

    g_mcp_config = s_local;

    /* s_local stringy jsou teď ve vlastnictví g_mcp_config; pro jistotu
     * vynulujeme lokální pointery, aby je další fresh open nedouble-freenul. */
    s_local.action_log_path = NULL;
    s_local.wrapper_log_path = NULL;
    s_local.wrapper_log_rotate_when = NULL;

    /* Persist do INI souboru. cfgroot_save iteruje moduly a volá
     * cfgmodule_save -> per-element save_cb -> cfgelement_set_*_value
     * z aktuální g_mcp_config -> zápis na disk. */
    cfgroot_save(g_cfgmain);

    /* Pokud TCP server běží, graceful restart s novými hodnotami.
     * Pokud server neběží, nic - příští Start už vezme novou config. */
    if (g_mcp_tcp_server && mcp_tcp_server_is_running(g_mcp_tcp_server))
    {
        mcp_tcp_server_stop(g_mcp_tcp_server);
        const char *bind = mcp_config_bind_addr_str(g_mcp_config.bind_addr);
        en_MCP_TCP_START_RESULT r = mcp_tcp_server_start(
            g_mcp_tcp_server, g_mcp_config.tcp_port, bind);
        if (r != MCP_TCP_START_OK)
        {
            fprintf(stderr,
                "[MCP] settings: TCP restart on %s:%d failed (rc=%d)\n",
                bind, g_mcp_config.tcp_port, (int)r);
        }
    }
}


extern "C" void imgui_mcp_settings_dialog(void)
{
    if (!g_gui->showMcpSettingsWindow)
    {
        /* Příště fresh open - znovu nakopírovat aktuální g_mcp_config. */
        s_initialized = false;
        return;
    }

    /* Fresh open -> kopie aktuální konfigurace do edit bufferu.
     * Heap stringy klonujeme přes g_strdup, aby s_local měl vlastní
     * kopie (apply_and_save je předá zpět g_mcp_config; Cancel je
     * uvolní). InputText buffer naplníme z aktuální hodnoty. */
    if (!s_initialized)
    {
        s_local = g_mcp_config;
        if (g_mcp_config.action_log_path) {
            s_local.action_log_path = g_strdup(g_mcp_config.action_log_path);
        }
        if (g_mcp_config.wrapper_log_path) {
            s_local.wrapper_log_path = g_strdup(g_mcp_config.wrapper_log_path);
            g_strlcpy(s_wrapper_log_path_buf,
                      g_mcp_config.wrapper_log_path,
                      sizeof(s_wrapper_log_path_buf));
        } else {
            s_wrapper_log_path_buf[0] = '\0';
        }
        if (g_mcp_config.wrapper_log_rotate_when) {
            s_local.wrapper_log_rotate_when =
                g_strdup(g_mcp_config.wrapper_log_rotate_when);
            g_strlcpy(s_wrapper_log_when_buf,
                      g_mcp_config.wrapper_log_rotate_when,
                      sizeof(s_wrapper_log_when_buf));
        } else {
            g_strlcpy(s_wrapper_log_when_buf, "midnight",
                      sizeof(s_wrapper_log_when_buf));
        }
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse;

    const char *title = _L("MCP Server Settings###mcp_settings_window");
    auto_layout_first_use_portrait(title, 520.0f, 380.0f);

    if (ImGui::Begin(title, &g_gui->showMcpSettingsWindow, flags))
    {
        /* TCP port. InputInt s rozsahem 1024..65535 (privilegované
         * porty < 1024 vyžadují root na Linuxu, nemá smysl je nabízet
         * pro user-mode emu). */
        ImGui::Text("%s:", _("TCP Port"));
        ImGui::SetNextItemWidth(180.0f);
        int port = s_local.tcp_port;
        if (ImGui::InputInt("##mcp_port", &port, 1, 100))
        {
            if (port < 1024) port = 1024;
            if (port > 65535) port = 65535;
            s_local.tcp_port = port;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(1024 - 65535)");

        ImGui::Spacing();

        /* Bind address - dva pevné výběry. */
        ImGui::Text("%s:", _("Bind Address"));
        int bind_idx = (int)s_local.bind_addr;
        if (bind_idx < 0 || bind_idx >= (int)MCP_BIND__COUNT) bind_idx = 0;
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("##mcp_bind", _(s_bind_items[bind_idx])))
        {
            for (int i = 0; i < (int)MCP_BIND__COUNT; i++)
            {
                bool selected = (bind_idx == i);
                if (ImGui::Selectable(_L(s_bind_items[i]), selected))
                {
                    s_local.bind_addr = (en_MCP_BIND_ADDR)i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (s_local.bind_addr == MCP_BIND_ALL_INTERFACES)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                "%s",
                _("WARNING: 0.0.0.0 exposes the server to any network "
                  "client. V0 has no authentication."));
        }

        ImGui::Spacing();

        /* Security profile - jen persistuje, plné enforcement v V1.A. */
        ImGui::Text("%s:", _("Security Profile"));
        int prof_idx = (int)s_local.profile;
        if (prof_idx < 0 || prof_idx >= (int)MCP_PROFILE__COUNT) prof_idx = 0;
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("##mcp_profile", _(s_profile_items[prof_idx])))
        {
            for (int i = 0; i < (int)MCP_PROFILE__COUNT; i++)
            {
                bool selected = (prof_idx == i);
                if (ImGui::Selectable(_L(s_profile_items[i]), selected))
                {
                    s_local.profile = (en_MCP_PROFILE)i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (s_local.profile != MCP_PROFILE_WILD)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                "%s",
                _("Note: profile enforcement (file whitelist, capability "
                  "gating, tool restrictions) lands in V1.A. V0 only "
                  "persists the selected value."));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Auto-start checkbox. */
        bool auto_start = s_local.auto_start_tcp;
        if (ImGui::Checkbox(_L("Auto-start TCP server on emulator startup##mcp_auto_start"),
                            &auto_start))
        {
            s_local.auto_start_tcp = auto_start;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- MCP Wrapper Log sekce ----------------------------------- */
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                           "%s", _("MCP Wrapper Log (Python mcp_server.py)"));
        ImGui::TextDisabled("%s", _("Changes apply on next mcp_server.py startup."));

        ImGui::Spacing();

        /* Log Level combo. */
        ImGui::Text("%s:", _("Log Level"));
        int level_idx = (int)s_local.wrapper_log_level;
        if (level_idx < 0 || level_idx >= (int)MCP_WRAPPER_LOG_LEVEL__COUNT) {
            level_idx = 0;
        }
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("##mcp_wrapper_log_level",
                              _(s_wrapper_log_level_items[level_idx])))
        {
            for (int i = 0; i < (int)MCP_WRAPPER_LOG_LEVEL__COUNT; i++)
            {
                bool selected = (level_idx == i);
                if (ImGui::Selectable(_L(s_wrapper_log_level_items[i]), selected))
                {
                    s_local.wrapper_log_level = (en_MCP_WRAPPER_LOG_LEVEL)i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        bool wrapper_log_disabled =
            (s_local.wrapper_log_level == MCP_WRAPPER_LOG_LEVEL_OFF);

        /* Zbývající widgety jsou disabled pokud log == OFF. */
        if (wrapper_log_disabled) ImGui::BeginDisabled();

        /* Log Path. */
        ImGui::Text("%s:", _("Log Path"));
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::InputText("##mcp_wrapper_log_path",
                             s_wrapper_log_path_buf,
                             sizeof(s_wrapper_log_path_buf)))
        {
            g_free(s_local.wrapper_log_path);
            s_local.wrapper_log_path =
                (s_wrapper_log_path_buf[0] != '\0')
                    ? g_strdup(s_wrapper_log_path_buf)
                    : NULL;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Empty = default mcp-server/mcp_server.log. "
                  "Absolute path recommended."));
        }

        /* Rotation Kind combo. */
        ImGui::Text("%s:", _("Rotation"));
        int kind_idx = (int)s_local.wrapper_log_rotate_kind;
        if (kind_idx < 0 || kind_idx >= (int)MCP_WRAPPER_LOG_ROTATE_KIND__COUNT) {
            kind_idx = 0;
        }
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("##mcp_wrapper_log_rotate_kind",
                              _(s_wrapper_log_rotate_items[kind_idx])))
        {
            for (int i = 0; i < (int)MCP_WRAPPER_LOG_ROTATE_KIND__COUNT; i++)
            {
                bool selected = (kind_idx == i);
                if (ImGui::Selectable(_L(s_wrapper_log_rotate_items[i]), selected))
                {
                    s_local.wrapper_log_rotate_kind =
                        (en_MCP_WRAPPER_LOG_ROTATE_KIND)i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        /* Size MB - jen pokud kind == size. */
        bool size_disabled =
            (s_local.wrapper_log_rotate_kind != MCP_WRAPPER_LOG_ROTATE_SIZE);
        if (size_disabled) ImGui::BeginDisabled();
        ImGui::Text("%s:", _("Max Size (MB)"));
        ImGui::SetNextItemWidth(180.0f);
        int size_mb = s_local.wrapper_log_rotate_size_mb;
        if (ImGui::InputInt("##mcp_wrapper_log_size_mb", &size_mb, 1, 10))
        {
            if (size_mb < 1) size_mb = 1;
            if (size_mb > 10240) size_mb = 10240;
            s_local.wrapper_log_rotate_size_mb = size_mb;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(1 - 10240)");
        if (size_disabled) ImGui::EndDisabled();

        /* When token - jen pokud kind == time. */
        bool when_disabled =
            (s_local.wrapper_log_rotate_kind != MCP_WRAPPER_LOG_ROTATE_TIME);
        if (when_disabled) ImGui::BeginDisabled();
        ImGui::Text("%s:", _("Time When"));
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::InputText("##mcp_wrapper_log_when",
                             s_wrapper_log_when_buf,
                             sizeof(s_wrapper_log_when_buf)))
        {
            g_free(s_local.wrapper_log_rotate_when);
            s_local.wrapper_log_rotate_when =
                (s_wrapper_log_when_buf[0] != '\0')
                    ? g_strdup(s_wrapper_log_when_buf)
                    : NULL;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                _("Python TimedRotatingFileHandler 'when' token: "
                  "S (seconds), M (minutes), H (hours), D (days), "
                  "midnight, W0-W6 (weekday)."));
        }
        if (when_disabled) ImGui::EndDisabled();

        /* Keep files - relevantní pro size i time. */
        bool keep_disabled =
            (s_local.wrapper_log_rotate_kind == MCP_WRAPPER_LOG_ROTATE_NONE);
        if (keep_disabled) ImGui::BeginDisabled();
        ImGui::Text("%s:", _("Keep Files"));
        ImGui::SetNextItemWidth(180.0f);
        int keep = s_local.wrapper_log_rotate_keep;
        if (ImGui::InputInt("##mcp_wrapper_log_keep", &keep, 1, 5))
        {
            if (keep < 0) keep = 0;
            if (keep > 1000) keep = 1000;
            s_local.wrapper_log_rotate_keep = keep;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(0 - 1000)");
        if (keep_disabled) ImGui::EndDisabled();

        if (wrapper_log_disabled) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tlačítka Save / Cancel zarovnaná na střed. */
        const float btn_w = 120.0f;
        float total_w = btn_w * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_w) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button(_L("Save##mcp_save"), ImVec2(btn_w, 0)))
        {
            apply_and_save();
            s_initialized = false;
            g_gui->showMcpSettingsWindow = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel##mcp_cancel"), ImVec2(btn_w, 0)))
        {
            /* Uvolnit heap stringy lokální kopie - apply_and_save je
             * převede do g_mcp_config jen při Save. */
            g_free(s_local.action_log_path);
            g_free(s_local.wrapper_log_path);
            g_free(s_local.wrapper_log_rotate_when);
            s_local.action_log_path = NULL;
            s_local.wrapper_log_path = NULL;
            s_local.wrapper_log_rotate_when = NULL;
            s_initialized = false;
            g_gui->showMcpSettingsWindow = false;
        }
    }
    ImGui::End();

    /* Zavření křížkem -> zahodit dočasný stav včetně heap kopií. */
    if (!g_gui->showMcpSettingsWindow)
    {
        if (s_initialized) {
            g_free(s_local.action_log_path);
            g_free(s_local.wrapper_log_path);
            g_free(s_local.wrapper_log_rotate_when);
            s_local.action_log_path = NULL;
            s_local.wrapper_log_path = NULL;
            s_local.wrapper_log_rotate_when = NULL;
        }
        s_initialized = false;
    }
}

#else /* !MZ800EMU_CFG_MCP_TCP_ENABLED */

extern "C" void imgui_mcp_settings_dialog(void)
{
    /* MCP TCP backend vypnut (NO_MCP_TCP=1 nebo NO_MCP=1) - no-op. */
}

#endif /* MZ800EMU_CFG_MCP_TCP_ENABLED */
