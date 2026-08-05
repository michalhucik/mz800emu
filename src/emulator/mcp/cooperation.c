/**
 * @file   cooperation.c
 * @brief  Implementace cooperation hint state (mutant mcp-server V1.A.1).
 *
 * Detaily v cooperation.h. Tento soubor je obalen guardem
 * MZ800EMU_CFG_MCP_SERVER_ENABLED - při buildu s NO_MCP=1 zůstane prázdný.
 *
 * @par Licence:
 * GPL-3.0-or-later.
 */

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzcommon_config.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include "cooperation.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <glib.h>


/* ====================================================================== */
/* Globální stav                                                           */
/* ====================================================================== */

/**
 * @brief Globální cooperation hint slot.
 *
 * Inicializován do FREE / NULL / 0. cooperation_hint_init musí být zavolán
 * z transport bootstrap (= main_pipe / tcp_server) před přijetím prvního
 * klientského requestu - reset zaručí čistý stav i kdyby předchozí
 * session zanechala alokovaný until string (relevantní pro test runner,
 * který může init volat opakovaně).
 */
st_COOPERATION_HINT_STATE g_cooperation_hint = {
    .mode      = COOPERATION_HINT_FREE,
    .until     = NULL,
    .set_at_us = 0,
};


/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

void cooperation_hint_init(void)
{
    /* Idempotentní reset - uvolnit případný starý heap string. */
    if (g_cooperation_hint.until != NULL)
    {
        g_free(g_cooperation_hint.until);
        g_cooperation_hint.until = NULL;
    }
    g_cooperation_hint.mode      = COOPERATION_HINT_FREE;
    g_cooperation_hint.set_at_us = 0;
}


void cooperation_hint_set(en_COOPERATION_HINT mode, const char *until)
{
    /* Uvolnit starý until před přepsáním (= prevence leaku, AI klient
     * může hint volat opakovaně). */
    if (g_cooperation_hint.until != NULL)
    {
        g_free(g_cooperation_hint.until);
        g_cooperation_hint.until = NULL;
    }

    g_cooperation_hint.mode = mode;
    if (until != NULL && until[0] != '\0')
    {
        g_cooperation_hint.until = g_strdup(until);
    }
    g_cooperation_hint.set_at_us = (uint64_t)g_get_monotonic_time();
}


void cooperation_hint_clear(void)
{
    cooperation_hint_set(COOPERATION_HINT_FREE, NULL);
}


const char *cooperation_hint_mode_to_str(en_COOPERATION_HINT mode)
{
    switch (mode)
    {
        case COOPERATION_HINT_FREE:        return "free";
        case COOPERATION_HINT_READ_ONLY:   return "read_only";
        case COOPERATION_HINT_PAUSED_ONLY: return "paused_only";
        default:                           return "unknown";
    }
}


bool cooperation_hint_mode_from_str(const char *s, en_COOPERATION_HINT *out_mode)
{
    if (s == NULL)
    {
        return false;
    }
    en_COOPERATION_HINT m;
    if (strcmp(s, "free") == 0)
    {
        m = COOPERATION_HINT_FREE;
    }
    else if (strcmp(s, "read_only") == 0)
    {
        m = COOPERATION_HINT_READ_ONLY;
    }
    else if (strcmp(s, "paused_only") == 0)
    {
        m = COOPERATION_HINT_PAUSED_ONLY;
    }
    else
    {
        return false;
    }
    if (out_mode != NULL)
    {
        *out_mode = m;
    }
    return true;
}

#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */
