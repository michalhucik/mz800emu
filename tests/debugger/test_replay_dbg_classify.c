/**
 * @file test_replay_dbg_classify.c
 * @brief Každý příkaz dbgapi má výslovnou třídu záznamu vstupů (N48).
 *
 * replay_dbg_classify() vrací pro nevyjmenovaný příkaz výchozí třídu
 * REPLAY_DBG_ALLOW: nový příkaz, který mění stav stroje, by tak prošel
 * záznamem vstupů bez povšimnutí a přehrání by se rozjelo. Test prochází
 * celý enum en_DBGAPI_CMD (0..DBGAPI_CMD_COUNT-1) a pro každý příkaz s třídou
 * ALLOW vyžaduje, aby byl v seznamu prověřených příkazů níže. Kdo přidá
 * příkaz, musí ho zařadit: buď do replay_dbg_classify() (RECORD, INPUT,
 * FORBIDDEN), nebo sem, pokud stav stroje nemění.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <stdio.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/replay_dbg.h"


/**
 * @brief Příkazy, které stav stroje nemění (nebo ho mění jiným, zachyceným
 *        kanálem) a smí mít v záznamu vstupů výchozí třídu ALLOW.
 *
 * Seznam je prověřený ručně proti obsluhám v dbgapi.c (observe-cas, M10).
 */
static const en_DBGAPI_CMD s_allow_reviewed[] = {
    /* Čtení stavu bez vedlejších efektů (včetně uložení snapshotu a exportů). */
    DBGAPI_CMD_IS_DEBUGGER_ACTIVE,
    DBGAPI_CMD_IS_RUNNING,
    DBGAPI_CMD_GET_REG,
    DBGAPI_CMD_GET_ALL_REGS,
    DBGAPI_CMD_MEM_READ,
    DBGAPI_CMD_BP_LIST,
    DBGAPI_CMD_DASM,
    DBGAPI_CMD_HISTORY_GET,
    DBGAPI_CMD_GET_CPU_FLAGS,
    DBGAPI_CMD_GET_IM2_VECTOR,
    DBGAPI_CMD_GET_RASTER_POS,
    DBGAPI_CMD_GET_LAST_INSTR,
    DBGAPI_CMD_GET_CPU_PANEL_BATCH,
    DBGAPI_CMD_STACK_DUMP,
    DBGAPI_CMD_STACK_REGIONS_LIST,
    DBGAPI_CMD_STACK_HISTORY_GET,
    DBGAPI_CMD_GET_CALLSTACK,
    DBGAPI_CMD_GET_PROFILER,
    DBGAPI_CMD_PROFILER_EXPORT,
    DBGAPI_CMD_SNAPSHOT_SAVE_FILE,
    DBGAPI_CMD_SNAPSHOT_SAVE_BUFFER,
    DBGAPI_CMD_SYMBOL_LOOKUP,
    DBGAPI_CMD_SYMBOL_LIST,
    DBGAPI_CMD_WATCH_LIST,
    DBGAPI_CMD_WATCH_EVAL,
    DBGAPI_CMD_CDL_EXPORT,
    DBGAPI_CMD_MEDIA_STATE,
    DBGAPI_CMD_SETTINGS_GET,
    DBGAPI_CMD_GET_CPU_IM2_VECTOR,
    DBGAPI_CMD_GET_CPU_INTERRUPT_BUS,
    DBGAPI_CMD_GET_MEMORY_MAP,
    DBGAPI_CMD_GET_MEMEXT_INFO,
    DBGAPI_CMD_BP_VARS_LIST,
    DBGAPI_CMD_BOOKMARKS_LIST,
    DBGAPI_CMD_GET_PERIPH_I8255,
    DBGAPI_CMD_GET_PERIPH_I8253,
    DBGAPI_CMD_GET_PERIPH_Z80_PIO,
    DBGAPI_CMD_GET_PERIPH_SN76489,
    DBGAPI_CMD_GET_PERIPH_AY3_8910,
    DBGAPI_CMD_GET_PERIPH_BEEPER,
    DBGAPI_CMD_GET_PERIPH_GDG,
    DBGAPI_CMD_GET_PERIPH_WD1793,
    DBGAPI_CMD_GET_PERIPH_CMT,
    DBGAPI_CMD_GET_PERIPH_QD,
    DBGAPI_CMD_GET_INPUT_KEYBOARD_STATE,
    DBGAPI_CMD_GET_INPUT_KEYBOARD_MATRIX_INFO,
    DBGAPI_CMD_GET_INPUT_JOYSTICK_STATE,
    DBGAPI_CMD_GET_FRAME_FRAMEBUFFER_INFO,
    DBGAPI_CMD_GET_FRAME_SCREENSHOT_RAW,
    DBGAPI_CMD_GET_FRAME_SCREENSHOT_PNG,
    DBGAPI_CMD_GET_VIDEO_TEXT_DUMP,
    DBGAPI_CMD_GET_WATCH_SNAPSHOT,
    DBGAPI_CMD_REGIONS_ENUMERATE,
    DBGAPI_CMD_REGIONS_READ,
    DBGAPI_CMD_GET_SPEED,
    DBGAPI_CMD_CMT_TAPE_LIST,
    DBGAPI_CMD_TRACE_SAVE,
    DBGAPI_CMD_TRACE_SNAPSHOT,
    DBGAPI_CMD_TRACE_RESOLVE,
    DBGAPI_CMD_MEMSEARCH_SAMPLE,
    DBGAPI_CMD_FREEZE_LIST,
    DBGAPI_CMD_FRAME_HASH,
    DBGAPI_CMD_BP_TEST_EVAL,
    DBGAPI_CMD_STATE_OBSERVE,
    /* Konfigurace a nástroje debuggeru (BP, watch, symboly, trace, profilery, CDL, ...). */
    DBGAPI_CMD_DEBUGGER_ACTIVATE,
    DBGAPI_CMD_DEBUGGER_DEACTIVATE,
    DBGAPI_CMD_BP_ADD,
    DBGAPI_CMD_BP_REMOVE,
    DBGAPI_CMD_BP_UPDATE,
    DBGAPI_CMD_BP_SET_ENABLED,
    DBGAPI_CMD_BP_SET_PARENT,
    DBGAPI_CMD_BP_CREATE_WITH_INIT,
    DBGAPI_CMD_BPGRP_ADD,
    DBGAPI_CMD_BPGRP_REMOVE,
    DBGAPI_CMD_BPGRP_UPDATE,
    DBGAPI_CMD_SET_USER_CYCLE_ORIGIN,
    DBGAPI_CMD_STACK_REGIONS_ADD,
    DBGAPI_CMD_STACK_REGIONS_REMOVE,
    DBGAPI_CMD_STACK_REGIONS_RESET_WATERMARK,
    DBGAPI_CMD_STACK_REGIONS_EDIT,
    DBGAPI_CMD_STACK_HISTORY_ENABLE,
    DBGAPI_CMD_STACK_HISTORY_RESET,
    DBGAPI_CMD_EVENTLOG_START,
    DBGAPI_CMD_EVENTLOG_STOP,
    DBGAPI_CMD_EVENTLOG_CLEAR,
    DBGAPI_CMD_EVENTLOG_SET_CAPACITY,
    DBGAPI_CMD_EVENTLOG_SET_MASK,
    DBGAPI_CMD_EVENTLOG_GET_EVENT,
    DBGAPI_CMD_PROFILER_SET_ACTIVE,
    DBGAPI_CMD_PROFILER_RESET,
    DBGAPI_CMD_SYMBOL_ADD,
    DBGAPI_CMD_SYMBOL_REMOVE,
    DBGAPI_CMD_WATCH_ADD,
    DBGAPI_CMD_WATCH_REMOVE,
    DBGAPI_CMD_CDL_START,
    DBGAPI_CMD_CDL_STOP,
    DBGAPI_CMD_CDL_RESET,
    DBGAPI_CMD_BOOKMARK_ADD,
    DBGAPI_CMD_BOOKMARK_REMOVE,
    DBGAPI_CMD_TRACE_START,
    DBGAPI_CMD_TRACE_STOP,
    DBGAPI_CMD_TRACE_RESET,
    DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE,
    DBGAPI_CMD_VIDEOREC,
    DBGAPI_CMD_IO_HISTORY_SET_CAPACITY,
    DBGAPI_CMD_IO_HISTORY_CLEAR,
    DBGAPI_CMD_IO_ACTIVITY_RESET,
    DBGAPI_CMD_BP_CLEAR_ALL,
    DBGAPI_CMD_BP_LOAD_FILE,
    DBGAPI_CMD_MHMAP_RESET_REGION,
    DBGAPI_CMD_MHMAP_MERGE,
    DBGAPI_CMD_EVENTLOG_SET_MODE,
    DBGAPI_CMD_EVENTLOG_IMPORT_FILE,
    DBGAPI_CMD_BP_RESET_HITS,
    DBGAPI_CMD_CALLSTACK_SET_ACTIVE,
    DBGAPI_CMD_CALLSTACK_RESET,
    DBGAPI_CMD_IO_ACTIVITY_RESET_PORT,
    DBGAPI_CMD_EVENTLOG_TRIGGER_SET,
    DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES,
    DBGAPI_CMD_SCREENSHOT_SERIES,
    DBGAPI_CMD_PROFILER_PC,
    DBGAPI_CMD_AUDIO_CAPTURE,
    DBGAPI_CMD_RAMLOG,
    DBGAPI_CMD_COVERAGE,
    DBGAPI_CMD_BPLOG,
    /* Řízení běhu (běh, kroky, pauza, rychlost hostitele). */
    DBGAPI_CMD_NONE,
    DBGAPI_CMD_PAUSE,
    DBGAPI_CMD_FORCE_PAUSE,
    DBGAPI_CMD_RUN,
    DBGAPI_CMD_STEP_INTO,
    DBGAPI_CMD_STEP_OVER,
    DBGAPI_CMD_RUN_TO,
    DBGAPI_CMD_STEP_OUT,
    DBGAPI_CMD_SET_SPEED,
    DBGAPI_CMD_RUN_FRAMES,
    DBGAPI_CMD_RUN_DEADLINE,
    /* Mění stav jiným, zachyceným kanálem nebo jen zobrazení. */
    DBGAPI_CMD_RESET, /* reset jde přes replay_divert_reset_request(), zaznamená se jako reset */
    DBGAPI_CMD_FREEZE_REMOVE, /* jen odebere položku; záznam s aktivním freeze nejde spustit */
    DBGAPI_CMD_SCREEN_REFRESH, /* přepíše jen framebuffer (obraz), stav stroje ne */
};


void setUp ( void ) {
}


void tearDown ( void ) {
}


/** @brief Je příkaz v seznamu prověřených ALLOW? */
static bool allow_reviewed ( en_DBGAPI_CMD cmd ) {
    for ( size_t i = 0; i < G_N_ELEMENTS ( s_allow_reviewed ); i++ ) {
        if ( s_allow_reviewed[ i ] == cmd ) return true;
    };
    return false;
}


/**
 * @brief Každý příkaz s třídou ALLOW je prověřený, žádný prověřený nemá
 *        jinou třídu (jinak je seznam zastaralý).
 */
void test_every_command_has_explicit_class ( void ) {
    for ( int c = 0; c < (int) DBGAPI_CMD_COUNT; c++ ) {
        en_DBGAPI_CMD cmd = (en_DBGAPI_CMD) c;
        en_REPLAY_DBG_CLASS k = replay_dbg_classify ( cmd );
        char msg[ 200 ];
        if ( k == REPLAY_DBG_ALLOW ) {
            g_snprintf ( msg, sizeof ( msg ),
                         "command %d (%s) has the default class ALLOW: classify it in "
                         "replay_dbg_classify() or add it to s_allow_reviewed",
                         c, dbgapi_cmd_to_str ( cmd ) );
            TEST_ASSERT_TRUE_MESSAGE ( allow_reviewed ( cmd ), msg );
        } else {
            g_snprintf ( msg, sizeof ( msg ),
                         "command %d (%s) is classified but also listed in s_allow_reviewed",
                         c, dbgapi_cmd_to_str ( cmd ) );
            TEST_ASSERT_FALSE_MESSAGE ( allow_reviewed ( cmd ), msg );
        };
    };
}


/** @brief Seznam nemá duplicity ani hodnoty mimo enum. */
void test_reviewed_list_is_clean ( void ) {
    for ( size_t i = 0; i < G_N_ELEMENTS ( s_allow_reviewed ); i++ ) {
        TEST_ASSERT_TRUE ( (int) s_allow_reviewed[ i ] < (int) DBGAPI_CMD_COUNT );
        for ( size_t j = i + 1; j < G_N_ELEMENTS ( s_allow_reviewed ); j++ ) {
            TEST_ASSERT_NOT_EQUAL ( s_allow_reviewed[ i ], s_allow_reviewed[ j ] );
        };
    };
}


/** @brief Příklady tříd: zápisy RECORD, vstupy INPUT, čtení portu FORBIDDEN. */
void test_known_classes ( void ) {
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_RECORD, replay_dbg_classify ( DBGAPI_CMD_MEM_WRITE_CHECKED ) );
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_RECORD, replay_dbg_classify ( DBGAPI_CMD_SET_REG ) );
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_INPUT, replay_dbg_classify ( DBGAPI_CMD_INPUT_PRESS_KEY ) );
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_FORBIDDEN, replay_dbg_classify ( DBGAPI_CMD_IO_READ ) );
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_ALLOW, replay_dbg_classify ( DBGAPI_CMD_STATE_OBSERVE ) );
}


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_every_command_has_explicit_class );
    RUN_TEST ( test_reviewed_list_is_clean );
    RUN_TEST ( test_known_classes );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
