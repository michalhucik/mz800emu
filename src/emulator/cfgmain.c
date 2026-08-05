/*
 * File:   cfgmain.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 15. září 2015, 13:54
 *
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <glib.h>

#include "cfgmain.h"
#include "build_revision/build_revision.h"
#include "baseui/baseui.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "emulator/mzarch/mzarch_platform.h"
#include "emulator/mzarch/mzcommon_config.h"
#include "snapshot/snapshot_config.h"
#include "i18n_lang.h"
#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "mcp/mcp_config.h"
#endif

extern SdlApp *g_sdlapp;

struct st_CFGROOT *g_cfgmain;
bool g_cfgmain_ini_file_exists = false;
const char *g_cfgmail_filename = NULL;

const char *g_build_date = __DATE__; // Formát: "MMM DD YYYY"
const char *g_build_time = __TIME__; // Formát: "HH:MM:SS"

const char *g_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const char *cfgmain_get_build_date(void)
{

    char month_str[4];
    int day, year, month = 0;

    sscanf(g_build_date, "%s %d %d", month_str, &day, &year);

    for (int i = 0; i < 12; i++)
    {
        if (strcmp(month_str, g_months[i]) == 0)
        {
            month = i + 1;
            break;
        };
    };

    static char build_date[] = "0000-00-00";
    snprintf(build_date, sizeof(build_date), "%04d-%02d-%02d", year, month, day);

    return build_date;
}

const char *cfgmain_get_build_datetime(void)
{

    char month_str[4];
    int day, year, month = 0;

    sscanf(g_build_date, "%s %d %d", month_str, &day, &year);

    for (int i = 0; i < 12; i++)
    {
        if (strcmp(month_str, g_months[i]) == 0)
        {
            month = i + 1;
            break;
        };
    };

    static char build_datetime[] = "0000-00-00 00:00:00";
    snprintf(build_datetime, sizeof(build_datetime), "%04d-%02d-%02d %s", year, month, day, g_build_time);

    return build_datetime;
}

char *cfgmain_create_timestamp(void)
{
    static char timestamp[] = "0000-00-00 00:00:00";
    time_t t;
    struct tm *tmp;

    t = time(NULL);
    tmp = localtime(&t);

    if (tmp == NULL)
    {
        fprintf(stderr, "%s():%d - localtime error: %s\n", __func__, __LINE__, strerror(errno));
        return timestamp;
    };

    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tmp) == 0)
    {
        fprintf(stderr, "%s():%d - strftime error: %s\n", __func__, __LINE__, strerror(errno));
        return timestamp;
    };
    return timestamp;
}

void cfgmain_savecfg(void *m, void *data)
{
    (void)data;

    GString *str = g_string_new(NULL);
    g_string_printf(str, "%s Emulator", g_mzarch_full_name);
    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "This_is_Config_File_for"), str->str);
    g_string_free(str, TRUE);

    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "config_version"), "1.0");
    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "emulator_version"), CFGMAIN_EMULATOR_VERSION_TEXT);
    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "emulator_build_time"), cfgmain_get_build_datetime());
    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "config_creation_timestamp"), cfgmain_create_timestamp());
    cfgelement_set_text_value(cfgmodule_get_element_by_name((CFGMOD *)m, "config_creation_platform"), CFGMAIN_PLATFORM);
}

void cfgmain_init(void)
{
    /* CLI option --config přebíjí default (cfg-dir/mz<arch>emu.ini). */
    const char *opt_config = sdlapp_option_value("--config");
    if (opt_config) {
        g_cfgmail_filename = g_strdup(opt_config);
    } else {
        GString *str = g_string_new(NULL);
        g_string_printf(str, "%semu.ini", g_mzarch_platform_name);
        char *rel = g_string_free(str, FALSE);
        g_cfgmail_filename = sdlapp_paths_resolve_cfg(g_sdlapp->paths, rel);
        g_free(rel);
    }

    g_cfgmain = cfgroot_new(g_cfgmail_filename);

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "CFGMAIN");

    cfgmodule_set_save_cb(cmod, &cfgmain_savecfg, NULL);

    cfgmodule_register_new_element(cmod, "This_is_Config_File_for", CFGENTYPE_TEXT, "");
    cfgmodule_register_new_element(cmod, "config_version", CFGENTYPE_TEXT, "");
    cfgmodule_register_new_element(cmod, "emulator_version", CFGENTYPE_TEXT, "");
    cfgmodule_register_new_element(cmod, "emulator_build_time", CFGENTYPE_TEXT, "");
    cfgmodule_register_new_element(cmod, "config_creation_timestamp", CFGENTYPE_TEXT, "");
    cfgmodule_register_new_element(cmod, "config_creation_platform", CFGENTYPE_TEXT, "");

    if (EXIT_SUCCESS == cfgmodule_parse(cmod))
    {
        g_cfgmain_ini_file_exists = true;
        printf("INFO: restore configuration from %s\n", g_cfgmail_filename);
    }
    else
    {
        printf("INFO: create new configuration file %s\n", g_cfgmail_filename);
    };

    /* Registrace snapshot konfigurace */
    snapshot_config_init();

    /* Registrace jazykové konfigurace */
    i18n_lang_config_init();

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    /* Registrace MCP konfigurace (V0.B.4 - sekce [MCP] s 4 klíči:
     * tcp_port, bind_address, profile, auto_start_tcp). Vyžaduje
     * MCP TCP backend (NO_MCP_TCP=0). */
    mcp_config_init();
#endif
}

void cfgmain_exit(void)
{
    /* CLI option --no-save-ini potlačí finální save .ini souboru. Použití pro
     * dočasné session - např. otestovat nastavení bez persistence. */
    if (!sdlapp_option_present("--no-save-ini"))
    {
        cfgroot_save(g_cfgmain);
    }
    cfgroot_destroy(g_cfgmain);
    if (g_cfgmail_filename)
    {
        g_free((char *)g_cfgmail_filename);
        g_cfgmail_filename = NULL;
    };
}

uint32_t cfgmain_get_version_uint32(void)
{

    static uint32_t version = 0;
    if (version)
        return version;

    char *src = CFGMAIN_EMULATOR_VERSION_NUM_STRING;

    int i;
    int src_len = strlen(CFGMAIN_EMULATOR_VERSION_NUM_STRING);
    int start = 0;
    int byte = 3;

    for (i = 0; i <= src_len; i++)
    {
        if ((src[i] == '.') || (src[i] == 0x00))
        {
            if (byte < 0)
            {
                fprintf(stderr, "%s():%d - Bad version string '%s'\n", __func__, __LINE__, CFGMAIN_EMULATOR_VERSION_NUM_STRING);
                version = 0;
                return 0;
            };
            int len = i - start;
            char *buff = (char *)baseui_tools_mem_alloc0(len + 1);
            strncpy(buff, &src[start], len);
            start = i + 1;
            uint32_t value = atoi(buff);
            baseui_tools_mem_free(buff);
            version += (value & 0xff) * (1 << (byte * 8));
            byte--;
        };
    };

    return version;
}
