/**
 * @file   state_rev.c
 * @brief  Revize stavu stroje (viz state_rev.h).
 */

#include <glib.h>

#include "state_rev.h"


uint64_t g_state_rev = 1;
uint32_t g_state_rev_src = 0;
uint32_t g_state_rev_src_override = 0;
uint32_t g_state_rev_reset_src = 0;

/** @brief Naposledy ohlášená revize (state_rev_take_unannounced()). */
static uint64_t s_announced_rev = 1;


void state_rev_init(void)
{
    g_state_rev = (uint64_t)g_random_int_range(1, (1 << 20) + 1) << 32;
    state_rev_mark_announced();
}


void state_rev_mark_announced(void)
{
    s_announced_rev = g_state_rev;
    g_state_rev_src = 0;
    /* Reset vyžádaný před během (nebo odmítnutý záznamem) už zdroj nenese. */
    g_state_rev_reset_src = 0;
}


uint32_t state_rev_take_unannounced(uint64_t *rev)
{
    if (rev) *rev = g_state_rev;
    if (g_state_rev == s_announced_rev) return 0;
    uint32_t src = g_state_rev_src;
    state_rev_mark_announced();
    return src;
}


const char *state_rev_src_name(uint32_t src)
{
    switch (src)
    {
        case STATE_REV_SRC_RUN: return "run";
        case STATE_REV_SRC_MCP: return "mcp";
        case STATE_REV_SRC_DEBUGGER: return "debugger";
        case STATE_REV_SRC_RESET: return "reset";
        case STATE_REV_SRC_SNAPSHOT: return "snapshot";
        case STATE_REV_SRC_DEVICE: return "device";
        case STATE_REV_SRC_REPLAY: return "replay";
        case STATE_REV_SRC_INPUT: return "input";
        case STATE_REV_SRC_TAPE: return "tape";
        default: return "unknown";
    };
}
