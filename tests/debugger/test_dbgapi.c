/*
 * test_dbgapi.c — testy CMDRQ dispatch handlerů v dbgapi
 *
 * Pre-fáze D.0.5.A (smart breakpoints V1) implementuje 23 CMDRQ
 * handlerů jako forwarders k existujícím emu funkcím. Tento test
 * volá `dbgapi_emu_dispatch()` přímo s vyplněným slotem (mock dispatch,
 * bez fronty + UI vlákna) a ověřuje:
 *   - rq->success je true po úspěšném zpracování
 *   - result_ptr má očekávanou hodnotu (= forwarder zavolal správnou
 *     funkci se správnými argumenty a její výstup je propsaný zpět)
 *   - vedlejší efekt na globálním stavu (= např. emulator paused flag)
 *
 * Test používá plný emulator core (mztest_init() + mzarch_platform_fn_init).
 * Forwarders volají reálné emu funkce, které manipulují globální stav.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_regions.h"
#include "debugger/replay_dbg.h"
#include "debugger/coverage/coverage_hook.h"
#include "debugger/trace/ramlog.h"
#include "state_hash.h"
#include "state_rev.h"
#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "debugger/debugger.h"
#include "emulator/emulator.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#include "libs/cpu-z80/z80.h"

#include <string.h>


/* ========================================================================= */
/*  setUp / tearDown                                                         */
/* ========================================================================= */


/**
 * @brief Příprava před každým testem - vyčistí BP, resetuje emu stav.
 *
 * Není volán dbgapi_init() záměrně - testy nepracují s frontou,
 * volají dbgapi_emu_dispatch() přímo s lokálním slotem.
 */
void setUp ( void ) {
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    g_emulator.paused = false;
    g_debugger.skip_bp_at_pc = -1;
}


void tearDown ( void ) {
}


/* ========================================================================= */
/*  Pomocné funkce                                                            */
/* ========================================================================= */


/**
 * @brief Vyplní slot CMDRQ a zavolá `dbgapi_emu_dispatch()`.
 *
 * @param cmd Příkaz včetně případných flagů (BLOCKING)
 * @param data_ptr Vstupní data nebo NULL
 * @param result_ptr Buffer pro výstup nebo NULL
 * @return true pokud handler nastavil rq->success na true
 */
static bool call_dispatch ( en_DBGAPI_CMD cmd, void *data_ptr, void *result_ptr ) {
    st_DBGAPI_CMDRQ rq;
    memset ( &rq, 0, sizeof (rq) );
    rq.cmd = cmd;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = data_ptr;
    rq.result_ptr = result_ptr;
    rq.success = false;
    dbgapi_emu_dispatch ( &rq );
    return rq.success;
}


/* ========================================================================= */
/*  Testy řízení emulace - CMD_NONE (sanity check, předem implementováno)    */
/* ========================================================================= */


void test_cmd_none_success ( void ) {
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_NONE, NULL, NULL ) );
}


/* ========================================================================= */
/*  CMD_IS_DEBUGGER_ACTIVE - dotaz na stav debug okna                          */
/* ========================================================================= */


void test_cmd_is_debugger_active_initial ( void ) {
    bool active = true;
    bool ok = call_dispatch ( DBGAPI_CMD_IS_DEBUGGER_ACTIVE, NULL, &active );
    TEST_ASSERT_TRUE ( ok );
    /* g_debugger.active je 0 v test setupu (mztest_init nezavolá debugger UI). */
    TEST_ASSERT_FALSE ( active );
}


void test_cmd_is_debugger_active_when_set ( void ) {
    g_debugger.active = 1;
    bool active = false;
    bool ok = call_dispatch ( DBGAPI_CMD_IS_DEBUGGER_ACTIVE, NULL, &active );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( active );
    g_debugger.active = 0;
}


void test_cmd_is_debugger_active_null_result_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_IS_DEBUGGER_ACTIVE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_DEBUGGER_ACTIVATE                                                     */
/* ========================================================================= */


void test_cmd_debugger_activate_sets_flag ( void ) {
    g_debugger.active = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_DEBUGGER_ACTIVATE, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_ACTIVE );
    g_debugger.active = 0;
}


/* ========================================================================= */
/*  CMD_DEBUGGER_DEACTIVATE                                                   */
/* ========================================================================= */


void test_cmd_debugger_deactivate_clears_flag ( void ) {
    g_debugger.active = 1;
    bool ok = call_dispatch ( DBGAPI_CMD_DEBUGGER_DEACTIVATE, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_ACTIVE );
}


/* ========================================================================= */
/*  CMD_DEBUGGER_ACTIVATE / _DEACTIVATE - přepočet CPU callbacků              */
/* ========================================================================= */


/**
 * @brief Vrátí true, pokud CPU právě používá logging (pomalé) callbacky.
 *
 * Swap provádí mzarch_platform_fn_debugger_state_changed(); čtecí callback
 * paměti je jeho spolehlivý indikátor (vzor test_mhmap_window_queue.c).
 */
static bool cpu_on_logging_callbacks ( void ) {
    return g_mzarch_main.cpu->mread_cb == memory_read_with_logging_cb;
}


/**
 * @brief MCP debugger_activate / _deactivate přepne CPU callbacky.
 *
 * Regrese: handlery dřív jen zapsaly g_debugger.active bez přepočtu
 * callbacků. Při cpuhist v režimu WITH_WINDOW (a ničem jiném, co by
 * pomalé callbacky vyžadovalo) pak po aktivaci CPU zůstalo na rychlých
 * callbackách a historie nezaznamenávala; po deaktivaci naopak zůstaly
 * pomalé callbacky.
 */
void test_cmd_debugger_activate_deactivate_swaps_callbacks ( void ) {
    en_DEBUGGER_CPUHIST_MODE saved_cpuhist = g_debugger.cpuhist_mode;
    en_DEBUGGER_MHMAP_MODE saved_mhmap = g_debugger.mhmap_mode;

    g_debugger.active = 0;
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW;
    g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_OFF;
    mzarch_platform_fn_debugger_state_changed ( false );
    /* Předpoklad: bez aktivního debuggeru nic pomalé callbacky nepotřebuje. */
    TEST_ASSERT_FALSE ( cpu_on_logging_callbacks ( ) );

    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_DEBUGGER_ACTIVATE, NULL, NULL ) );
    bool logging_after_activate = cpu_on_logging_callbacks ( );

    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_DEBUGGER_DEACTIVATE, NULL, NULL ) );
    bool logging_after_deactivate = cpu_on_logging_callbacks ( );

    /* Úklid před asserty, aby selhání nezanechalo stav pro další testy. */
    g_debugger.active = 0;
    g_debugger.cpuhist_mode = saved_cpuhist;
    g_debugger.mhmap_mode = saved_mhmap;
    mzarch_platform_fn_debugger_state_changed ( false );

    TEST_ASSERT_TRUE ( logging_after_activate );
    TEST_ASSERT_FALSE ( logging_after_deactivate );
}


/* ========================================================================= */
/*  CMD_PAUSE - forward na emulator_pause(true)                              */
/* ========================================================================= */


void test_cmd_pause_sets_paused_flag ( void ) {
    g_emulator.paused = false;
    bool ok = call_dispatch ( DBGAPI_CMD_PAUSE, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( EMULATOR_TEST_PAUSED );
}


void test_cmd_pause_idempotent ( void ) {
    g_emulator.paused = true;
    bool ok = call_dispatch ( DBGAPI_CMD_PAUSE, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( EMULATOR_TEST_PAUSED );
}


/* ========================================================================= */
/*  CMD_FORCE_PAUSE - forward na emulator_pause(true) bez výjimek            */
/* ========================================================================= */


void test_cmd_force_pause_sets_paused ( void ) {
    g_emulator.paused = false;
    bool ok = call_dispatch ( DBGAPI_CMD_FORCE_PAUSE, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( EMULATOR_TEST_PAUSED );
}


/* ========================================================================= */
/*  CMD_RUN - forward na emulator_pause(false)                                */
/* ========================================================================= */


void test_cmd_run_clears_paused_flag ( void ) {
    g_emulator.paused = true;
    bool ok = call_dispatch ( DBGAPI_CMD_RUN, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_FALSE ( EMULATOR_TEST_PAUSED );
}


/* ========================================================================= */
/*  CMD_IS_RUNNING - vrátí !EMULATOR_TEST_PAUSED                              */
/* ========================================================================= */


void test_cmd_is_running_when_paused ( void ) {
    g_emulator.paused = true;
    bool running = true;
    bool ok = call_dispatch ( DBGAPI_CMD_IS_RUNNING, NULL, &running );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_FALSE ( running );
}


void test_cmd_is_running_when_running ( void ) {
    g_emulator.paused = false;
    bool running = false;
    bool ok = call_dispatch ( DBGAPI_CMD_IS_RUNNING, NULL, &running );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( running );
}


void test_cmd_is_running_null_result_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_IS_RUNNING, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_STEP_INTO - forward na debugger_step_call(1)                          */
/* ========================================================================= */


void test_cmd_step_into_in_pause_sets_flag ( void ) {
    /* V paused stavu nastaví debugger_step_call(1) → step_call flag. */
    g_emulator.paused = true;
    g_debugger.step_call = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_STEP_INTO, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_STEP_CALL );
    g_debugger.step_call = 0;
}


/* ========================================================================= */
/*  CMD_STEP_OVER - replika logiky dbg_iconbar.cpp                            */
/* ========================================================================= */


void test_cmd_step_over_when_running_pauses ( void ) {
    /* Pokud emu běží, jen pozastaví (= UX z dbg_iconbar). */
    g_emulator.paused = false;
    bool ok = call_dispatch ( DBGAPI_CMD_STEP_OVER, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( EMULATOR_TEST_PAUSED );
}


void test_cmd_step_over_when_paused_returns_success ( void ) {
    /* V paused stavu provede step over (degraduje na step into pokud
     * PC neukazuje na CALL/RST/DJNZ/blokovou instrukci). Test ověří
     * jen, že dispatch vrátí success - faktická step logika vyžaduje
     * běžící emulační smyčku. */
    g_emulator.paused = true;
    g_debugger.step_call = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_STEP_OVER, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    g_debugger.step_call = 0;
}


/* ========================================================================= */
/*  CMD_RUN_TO - dočasný BP na cílové adrese + run                            */
/* ========================================================================= */


void test_cmd_run_to_null_data_fails ( void ) {
    g_emulator.paused = true;
    bool ok = call_dispatch ( DBGAPI_CMD_RUN_TO, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_run_to_when_paused_sets_temporary_bp ( void ) {
    g_emulator.paused = true;
    bptmap_reset_temporary_event ( );
    uint16_t target = 0x4321;
    bool ok = call_dispatch ( DBGAPI_CMD_RUN_TO, &target, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_INT ( 0x4321, g_bptmap.temporary_bpt_addr );
    /* po run_to_temporary_breakpoint je paused = false */
    TEST_ASSERT_FALSE ( EMULATOR_TEST_PAUSED );
    bptmap_reset_temporary_event ( );
    g_emulator.paused = false;
}


void test_cmd_run_to_when_running_pauses ( void ) {
    g_emulator.paused = false;
    uint16_t target = 0x4321;
    bool ok = call_dispatch ( DBGAPI_CMD_RUN_TO, &target, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( EMULATOR_TEST_PAUSED );
}


/* ========================================================================= */
/*  CMD_RESET - asynchronní platform reset                                    */
/* ========================================================================= */


void test_cmd_reset_sets_request_flag ( void ) {
    /* mzarch_platform_fn_reset_request() nastaví reset_request flag
     * pod mutexem. Test ověří flag, ne faktický reset (= ten provede
     * mzarch_main loop, který v testu neběží). */
    g_mzarch_main.reset_request = false;
    bool ok = call_dispatch ( DBGAPI_CMD_RESET, NULL, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( g_mzarch_main.reset_request );
    g_mzarch_main.reset_request = false;
}


/* ========================================================================= */
/*  CMD_GET_REG - z80_get_reg(reg_id)                                         */
/* ========================================================================= */


void test_cmd_get_reg_pc ( void ) {
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_PC, 0xABCD );
    uint8_t reg_id = (uint8_t)Z80_REG_PC;
    uint16_t value = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_GET_REG, &reg_id, &value );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0xABCD, value );
}


void test_cmd_get_reg_sp ( void ) {
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_SP, 0x1234 );
    uint8_t reg_id = (uint8_t)Z80_REG_SP;
    uint16_t value = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_GET_REG, &reg_id, &value );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, value );
}


void test_cmd_get_reg_invalid_id_fails ( void ) {
    uint8_t reg_id = 99;
    uint16_t value = 0;
    bool ok = call_dispatch ( DBGAPI_CMD_GET_REG, &reg_id, &value );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_get_reg_null_args_fail ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_GET_REG, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_SET_REG - z80_set_reg(reg_id, value)                                  */
/* ========================================================================= */


void test_cmd_set_reg_pc ( void ) {
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_PC, 0x0000 );
    st_DBGAPI_REG_PARAM p = { (uint8_t)Z80_REG_PC, 0x5678 };
    bool ok = call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5678,
                              z80_get_reg ( g_mzarch_main.cpu, Z80_REG_PC ) );
}


void test_cmd_set_reg_ir_writes_full_pair ( void ) {
    /* IR = celý pár I:R včetně bitu 7 R (jako LD I,A + LD R,A), v obou
     * směrech bitu 7. */
    g_mzarch_main.cpu->i = 0xAA;
    g_mzarch_main.cpu->r = 0x00;
    st_DBGAPI_REG_PARAM p = { (uint8_t)Z80_REG_IR, 0x55BB };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x55, g_mzarch_main.cpu->i );
    TEST_ASSERT_EQUAL_HEX8 ( 0xBB, g_mzarch_main.cpu->r );

    st_DBGAPI_REG_PARAM p2 = { (uint8_t)Z80_REG_IR, 0x1234 };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_REG, &p2, NULL ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, z80_get_reg ( g_mzarch_main.cpu, Z80_REG_IR ) );
}


void test_cmd_set_cpu_flags_r_masks ( void ) {
    /* UM_R (okno CPU): jen dolních 7 bitů, bit 7 zachová.
     * UM_R_FULL (MCP): všech 8 bitů jako LD R,A. */
    st_DBGAPI_CPU_FLAGS f;
    memset ( &f, 0, sizeof ( f ) );
    g_mzarch_main.cpu->r = 0x80;
    f.update_mask = DBGAPI_CPU_FLAGS_UM_R;
    f.r_reg = 0x3B;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_CPU_FLAGS, &f, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0xBB, g_mzarch_main.cpu->r );

    f.update_mask = DBGAPI_CPU_FLAGS_UM_R_FULL;
    f.r_reg = 0x3B;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_CPU_FLAGS, &f, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x3B, g_mzarch_main.cpu->r );

    f.r_reg = 0xC4;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_CPU_FLAGS, &f, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0xC4, g_mzarch_main.cpu->r );
}


void test_cmd_set_reg_invalid_id_fails ( void ) {
    st_DBGAPI_REG_PARAM p = { 99, 0x1234 };
    bool ok = call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_set_reg_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_SET_REG, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_GET_ALL_REGS - dump všech registrů                                    */
/* ========================================================================= */


void test_cmd_get_all_regs ( void ) {
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_PC, 0x1234 );
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_SP, 0xFFFE );
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, 0x4444 );
    uint16_t regs[ DBGAPI_REG_COUNT ] = { 0 };
    bool ok = call_dispatch ( DBGAPI_CMD_GET_ALL_REGS, NULL, regs );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4444, regs[ Z80_REG_HL ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0xFFFE, regs[ Z80_REG_SP ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, regs[ Z80_REG_PC ] );
}


void test_cmd_get_all_regs_null_result_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_GET_ALL_REGS, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_MEM_READ - blok paměti přes banking                                   */
/* ========================================================================= */


void test_cmd_mem_read_block ( void ) {
    /* ROM monitor MZ-800 začíná na 0x0000 (= mapováno po resetu).
     * Čtení 4 bajtů musí dát něco jiného než 0xFF (= validní ROM). */
    uint8_t buf[ 4 ] = { 0xAA, 0xAA, 0xAA, 0xAA };
    st_DBGAPI_MEM_PARAM p = { 0x0000, 4, buf };
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_READ, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    /* Aspoň jeden bajt z 4 musí být != 0xAA (= něco se přečetlo). */
    bool nonzero = ( buf[ 0 ] != 0xAA ) || ( buf[ 1 ] != 0xAA )
                || ( buf[ 2 ] != 0xAA ) || ( buf[ 3 ] != 0xAA );
    TEST_ASSERT_TRUE ( nonzero );
}


void test_cmd_mem_read_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_READ, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_mem_read_null_buf_fails ( void ) {
    st_DBGAPI_MEM_PARAM p = { 0x0000, 4, NULL };
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_READ, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_MEM_WRITE - zápis bloku přes banking                                  */
/* ========================================================================= */


void test_cmd_mem_write_then_read_back ( void ) {
    /* Adresa 0xC000 je default RAM (= mimo ROM 0x0000-0x0FFF + 0xE000+).
     * Zapsat 4 bajty, přečíst zpět, ověřit. */
    uint8_t pattern[ 4 ] = { 0xDE, 0xAD, 0xBE, 0xEF };
    st_DBGAPI_MEM_PARAM wp = { 0xC000, 4, pattern };
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL );
    TEST_ASSERT_TRUE ( ok );

    uint8_t readback[ 4 ] = { 0 };
    st_DBGAPI_MEM_PARAM rp = { 0xC000, 4, readback };
    ok = call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( pattern, readback, 4 );
}


void test_cmd_mem_write_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_mem_write_null_buf_fails ( void ) {
    st_DBGAPI_MEM_PARAM p = { 0xC000, 4, NULL };
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_BP_ADD - přidání execution BP                                         */
/* ========================================================================= */


void test_cmd_bp_add_returns_id ( void ) {
    st_DBGAPI_BP_PARAM p = { 0x1234, 0 };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_ADD, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_GREATER_THAN_INT ( 0, p.id );
    /* Ověř že BP existuje. */
    TEST_ASSERT_NOT_NULL ( breakpoints_find_by_addr ( 0x1234 ) );
}


void test_cmd_bp_add_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_ADD, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_BP_REMOVE - odstranění BP podle ID                                    */
/* ========================================================================= */


void test_cmd_bp_remove_existing ( void ) {
    int id = breakpoints_add_auto ( 0x2000, NULL, -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    st_DBGAPI_BP_PARAM p = { 0x2000, id };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_REMOVE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_NULL ( breakpoints_find_by_addr ( 0x2000 ) );
}


void test_cmd_bp_remove_nonexistent_fails ( void ) {
    st_DBGAPI_BP_PARAM p = { 0x9999, 999 };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_REMOVE, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_remove_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_REMOVE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_BP_LIST - výpis BP                                                    */
/* ========================================================================= */


void test_cmd_bp_list_empty ( void ) {
    /* Caller alokuje s flexibilním polem - tady stačí 4 sloty. */
    uint8_t buf[ sizeof(st_DBGAPI_BP_LIST_RESULT) + 4 * sizeof(((st_DBGAPI_BP_LIST_RESULT*)0)->bp[0]) ];
    st_DBGAPI_BP_LIST_RESULT *r = (st_DBGAPI_BP_LIST_RESULT *)buf;
    r->max_count = 4;
    r->count = 99;
    bool ok = call_dispatch ( DBGAPI_CMD_BP_LIST, NULL, r );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_INT ( 0, r->count );
}


void test_cmd_bp_list_two_bps ( void ) {
    breakpoints_add_auto ( 0x1111, NULL, -1 );
    breakpoints_add_auto ( 0x2222, NULL, -1 );

    uint8_t buf[ sizeof(st_DBGAPI_BP_LIST_RESULT) + 4 * sizeof(((st_DBGAPI_BP_LIST_RESULT*)0)->bp[0]) ];
    st_DBGAPI_BP_LIST_RESULT *r = (st_DBGAPI_BP_LIST_RESULT *)buf;
    r->max_count = 4;
    r->count = 0;

    bool ok = call_dispatch ( DBGAPI_CMD_BP_LIST, NULL, r );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_INT ( 2, r->count );
    /* Adresy musí být obě, pořadí dle GArray indexu. */
    bool found1 = false, found2 = false;
    for (int i = 0; i < r->count; i++)
    {
        if ( r->bp[ i ].addr == 0x1111 ) found1 = true;
        if ( r->bp[ i ].addr == 0x2222 ) found2 = true;
    };
    TEST_ASSERT_TRUE ( found1 );
    TEST_ASSERT_TRUE ( found2 );
}


void test_cmd_bp_list_null_result_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_LIST, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_BP_UPDATE / CMD_BP_SET_ENABLED / CMD_BP_SET_PARENT /                 */
/*  CMD_BP_CREATE_WITH_INIT - V1.7+ CRUD migrace                              */
/* ========================================================================= */

void test_cmd_bp_update_full_struct_roundtrip ( void ) {
    int id = breakpoints_add_auto ( 0x3000, NULL, -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );

    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = id;
    p.update_mask = DBGAPI_BP_UM_ENABLED
                  | DBGAPI_BP_UM_AUTO_NAME
                  | DBGAPI_BP_UM_NAME
                  | DBGAPI_BP_UM_COLORS
                  | DBGAPI_BP_UM_TYPE
                  | DBGAPI_BP_UM_ADDR
                  | DBGAPI_BP_UM_ADDR_END
                  | DBGAPI_BP_UM_HIT_COUNT
                  | DBGAPI_BP_UM_SKIP_COUNT
                  | DBGAPI_BP_UM_EDGE_TRIGGERED
                  | DBGAPI_BP_UM_EXPR
                  | DBGAPI_BP_UM_ACTION;
    p.enabled = false;
    p.auto_name = false;
    p.name = "test_label";
    p.bg_rgb = 0x112233;
    p.fg_rgb = 0xAABBCC;
    p.type = BPT_TYPE_MEM_W;
    p.addr = 0x3000;
    p.addr_end = 0x3FFF;
    p.hit_count = 5;
    p.skip_count = 2;
    p.edge_triggered = true;
    p.expr = "A == 0x42";
    p.action = "log \"hit\"; continue";

    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPT *bp = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bp );
    TEST_ASSERT_FALSE ( bp->enabled );
    TEST_ASSERT_EQUAL_STRING ( "test_label", bp->name );
    TEST_ASSERT_EQUAL_HEX32 ( 0x112233, bp->bg_rgb );
    TEST_ASSERT_EQUAL_HEX32 ( 0xAABBCC, bp->fg_rgb );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_MEM_W, bp->type );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3000, bp->addr );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3FFF, bp->addr_end );
    TEST_ASSERT_EQUAL_UINT32 ( 5, bp->hit_count );
    TEST_ASSERT_EQUAL_UINT32 ( 2, bp->skip_count );
    TEST_ASSERT_TRUE ( bp->edge_triggered );
    TEST_ASSERT_EQUAL_STRING ( "A == 0x42", bp->expr );
    TEST_ASSERT_EQUAL_STRING ( "log \"hit\"; continue", bp->action );
}


void test_cmd_bp_update_partial_mask ( void ) {
    /* Vytvoříme BP se známou hodnotou, pak UPDATE jen jednoho pole - */
    /* ostatní musí zůstat nedotčené. */
    int id = breakpoints_add_auto ( 0x4000, "original_name", -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    breakpoints_set_addr_end ( id, 0x40FF );
    breakpoints_set_hit_count ( id, 10 );

    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = id;
    p.update_mask = DBGAPI_BP_UM_HIT_COUNT;  /* jen hit_count */
    p.hit_count = 99;
    /* Šum v ostatních polích - musí být ignorováno: */
    p.addr_end = 0xDEAD;
    p.name = "ignored";

    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPT *bp = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT32 ( 99, bp->hit_count );        /* změněno */
    TEST_ASSERT_EQUAL_HEX16 ( 0x40FF, bp->addr_end );      /* zachováno */
    TEST_ASSERT_EQUAL_STRING ( "original_name", bp->name ); /* zachováno */
}


void test_cmd_bp_update_zero_mask_is_noop ( void ) {
    int id = breakpoints_add_auto ( 0x5000, "keep", -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );

    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = id;
    p.update_mask = 0;

    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPT *bp = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_STRING ( "keep", bp->name );
}


void test_cmd_bp_update_nonexistent_fails ( void ) {
    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = 9999;
    p.update_mask = DBGAPI_BP_UM_ENABLED;
    p.enabled = false;
    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_update_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_update_match_modes_and_irq_fields ( void ) {
    int id = breakpoints_add_auto ( 0x6000, NULL, -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );

    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = id;
    p.update_mask = DBGAPI_BP_UM_ADDR_MATCH_MODE
                  | DBGAPI_BP_UM_ADDR_MASK
                  | DBGAPI_BP_UM_PORT_MODE
                  | DBGAPI_BP_UM_SP_MODE
                  | DBGAPI_BP_UM_SP_UPPER
                  | DBGAPI_BP_UM_IM2_VECTOR_FILTER
                  | DBGAPI_BP_UM_IM2_ISR_FILTER
                  | DBGAPI_BP_UM_IM0_ENABLED
                  | DBGAPI_BP_UM_IM1_ENABLED
                  | DBGAPI_BP_UM_IM2_ENABLED
                  | DBGAPI_BP_UM_IM0_RST_MASK
                  | DBGAPI_BP_UM_IRQ_SIG_SOURCE_MASK;
    p.addr_match_mode = BP_MATCH_MASK;
    p.addr_mask = 0xF000;
    p.port_mode = BP_PORT_16BIT;
    p.sp_mode = BP_SP_WINDOW;
    p.sp_upper = 0xFFFE;
    p.im2_vector_enabled = true;
    p.im2_vector_addr = 0x10AA;
    p.im2_isr_enabled = true;
    p.im2_isr_addr = 0x2000;
    p.im0_enabled = false;
    p.im1_enabled = true;
    p.im2_enabled = false;
    p.im0_rst_mask = 0x80;
    p.irq_sig_source_mask = 0x05;

    bool ok = call_dispatch ( DBGAPI_CMD_BP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPT *bp = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_MASK, bp->addr_match_mode );
    TEST_ASSERT_EQUAL_HEX16 ( 0xF000, bp->addr_mask );
    TEST_ASSERT_EQUAL_INT ( BP_PORT_16BIT, bp->port_mode );
    TEST_ASSERT_EQUAL_INT ( BP_SP_WINDOW, bp->sp_mode );
    TEST_ASSERT_EQUAL_HEX16 ( 0xFFFE, bp->sp_upper );
    TEST_ASSERT_TRUE ( bp->im2_vector_enabled );
    TEST_ASSERT_EQUAL_HEX16 ( 0x10AA, bp->im2_vector_addr );
    TEST_ASSERT_TRUE ( bp->im2_isr_enabled );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, bp->im2_isr_addr );
    TEST_ASSERT_FALSE ( bp->im0_enabled );
    TEST_ASSERT_TRUE ( bp->im1_enabled );
    TEST_ASSERT_FALSE ( bp->im2_enabled );
    TEST_ASSERT_EQUAL_HEX8 ( 0x80, bp->im0_rst_mask );
    TEST_ASSERT_EQUAL_HEX8 ( 0x05, bp->irq_sig_source_mask );
}


void test_cmd_bp_set_enabled_toggle ( void ) {
    int id = breakpoints_add_auto ( 0x7000, NULL, -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    TEST_ASSERT_TRUE ( breakpoints_find_by_id ( id )->enabled );

    st_DBGAPI_BP_SET_ENABLED_PARAM p = { id, false };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_SET_ENABLED, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_FALSE ( breakpoints_find_by_id ( id )->enabled );

    p.enabled = true;
    ok = call_dispatch ( DBGAPI_CMD_BP_SET_ENABLED, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_TRUE ( breakpoints_find_by_id ( id )->enabled );
}


void test_cmd_bp_set_enabled_nonexistent_fails ( void ) {
    st_DBGAPI_BP_SET_ENABLED_PARAM p = { 9999, false };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_SET_ENABLED, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_set_enabled_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_SET_ENABLED, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_set_parent_reparent_and_clear ( void ) {
    int gid = breakpoints_group_add ( "grp_test", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid );

    int id = breakpoints_add_auto ( 0x8000, NULL, -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    TEST_ASSERT_EQUAL_INT ( -1, breakpoints_find_by_id ( id )->parent );

    st_DBGAPI_BP_SET_PARENT_PARAM p = { id, gid };
    bool ok = call_dispatch ( DBGAPI_CMD_BP_SET_PARENT, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_INT ( gid, breakpoints_find_by_id ( id )->parent );

    /* Clear parent (-1) */
    p.parent_id = -1;
    ok = call_dispatch ( DBGAPI_CMD_BP_SET_PARENT, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_INT ( -1, breakpoints_find_by_id ( id )->parent );
}


void test_cmd_bp_set_parent_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_SET_PARENT, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_create_with_init_sp_threshold ( void ) {
    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = -1;  /* CREATE invariant */
    p.update_mask = DBGAPI_BP_UM_ADDR
                  | DBGAPI_BP_UM_TYPE
                  | DBGAPI_BP_UM_SP_THRESHOLD
                  | DBGAPI_BP_UM_AUTO_NAME
                  | DBGAPI_BP_UM_NAME;
    p.addr = 0x0000;
    p.type = BPT_TYPE_SP_THRESHOLD;
    p.sp_threshold = 0xFE82;
    p.auto_name = false;
    p.name = "sp_quick";

    bool ok = call_dispatch ( DBGAPI_CMD_BP_CREATE_WITH_INIT, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_GREATER_THAN_INT ( 0, p.id );

    st_BPT *bp = breakpoints_find_by_id ( p.id );
    TEST_ASSERT_NOT_NULL ( bp );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_SP_THRESHOLD, bp->type );
    TEST_ASSERT_EQUAL_HEX16 ( 0xFE82, bp->sp_threshold );
    TEST_ASSERT_EQUAL_STRING ( "sp_quick", bp->name );
}


void test_cmd_bp_create_with_init_wrong_id_fails ( void ) {
    st_DBGAPI_BP_UPDATE_PARAM p = { 0 };
    p.id = 5;  /* MUSÍ být -1 pro CREATE */
    bool ok = call_dispatch ( DBGAPI_CMD_BP_CREATE_WITH_INIT, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bp_create_with_init_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BP_CREATE_WITH_INIT, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_BPGRP_ADD / _REMOVE / _UPDATE - V1.7+ groups CRUD migrace             */
/* ========================================================================= */

void test_cmd_bpgrp_add_returns_id ( void ) {
    st_DBGAPI_BPGRP_ADD_PARAM p = { "grp1", -1, -1 };
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_ADD, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, p.id );
    TEST_ASSERT_NOT_NULL ( breakpoints_group_find_by_id ( p.id ) );
}


void test_cmd_bpgrp_add_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_ADD, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bpgrp_remove_existing ( void ) {
    int gid = breakpoints_group_add ( "to_remove", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid );
    st_DBGAPI_BPGRP_REMOVE_PARAM p = { gid };
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_REMOVE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_NULL ( breakpoints_group_find_by_id ( gid ) );
}


void test_cmd_bpgrp_remove_nonexistent_fails ( void ) {
    st_DBGAPI_BPGRP_REMOVE_PARAM p = { 9999 };
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_REMOVE, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bpgrp_remove_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_REMOVE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bpgrp_update_full_roundtrip ( void ) {
    int gid = breakpoints_group_add ( "orig", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid );
    int gid_parent = breakpoints_group_add ( "parent_grp", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid_parent );

    st_DBGAPI_BPGRP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = gid;
    p.update_mask = DBGAPI_BPGRP_UM_ENABLED
                  | DBGAPI_BPGRP_UM_NAME
                  | DBGAPI_BPGRP_UM_COLORS
                  | DBGAPI_BPGRP_UM_PARENT;
    p.enabled = false;
    p.name = "renamed";
    p.bg_rgb = 0xAB1234;
    p.fg_rgb = 0xCD5678;
    p.parent = gid_parent;

    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPTGROUP *grp = breakpoints_group_find_by_id ( gid );
    TEST_ASSERT_NOT_NULL ( grp );
    TEST_ASSERT_FALSE ( grp->enabled );
    TEST_ASSERT_EQUAL_STRING ( "renamed", grp->name );
    TEST_ASSERT_EQUAL_HEX32 ( 0xAB1234, grp->bg_rgb );
    TEST_ASSERT_EQUAL_HEX32 ( 0xCD5678, grp->fg_rgb );
    TEST_ASSERT_EQUAL_INT ( gid_parent, grp->parent );
}


void test_cmd_bpgrp_update_partial_mask ( void ) {
    int gid = breakpoints_group_add ( "keep_name", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid );
    breakpoints_group_set_colors ( gid, 0x111111, 0x222222 );

    st_DBGAPI_BPGRP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = gid;
    p.update_mask = DBGAPI_BPGRP_UM_ENABLED;  /* jen enabled */
    p.enabled = false;
    /* Šum: name a colors v p, ale bity nejsou v mask -> nesmí přepsat */
    p.name = "should_be_ignored";
    p.bg_rgb = 0xFFFFFF;

    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );

    st_BPTGROUP *grp = breakpoints_group_find_by_id ( gid );
    TEST_ASSERT_FALSE ( grp->enabled );                  /* změněno */
    TEST_ASSERT_EQUAL_STRING ( "keep_name", grp->name ); /* zachováno */
    TEST_ASSERT_EQUAL_HEX32 ( 0x111111, grp->bg_rgb );   /* zachováno */
}


void test_cmd_bpgrp_update_zero_mask_is_noop ( void ) {
    int gid = breakpoints_group_add ( "noop", -1 );
    TEST_ASSERT_GREATER_OR_EQUAL_INT ( 0, gid );

    st_DBGAPI_BPGRP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = gid;
    p.update_mask = 0;

    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_UPDATE, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_STRING ( "noop", breakpoints_group_find_by_id ( gid )->name );
}


void test_cmd_bpgrp_update_nonexistent_fails ( void ) {
    st_DBGAPI_BPGRP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = 9999;
    p.update_mask = DBGAPI_BPGRP_UM_ENABLED;
    p.enabled = false;
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_UPDATE, &p, NULL );
    TEST_ASSERT_FALSE ( ok );
}


void test_cmd_bpgrp_update_null_data_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_BPGRP_UPDATE, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_DASM - disassembly N instrukcí                                        */
/* ========================================================================= */


void test_cmd_dasm_block ( void ) {
    /* Zapsat NOP (0x00), NOP, JR -2 (0x18 0xFE) na 0xC100. */
    uint8_t code[ 4 ] = { 0x00, 0x00, 0x18, 0xFE };
    st_DBGAPI_MEM_PARAM wp = { 0xC100, 4, code };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );

    st_DBGAPI_DASM_PARAM dp = { 0xC100, 3 };
    st_DBGAPI_DASM_RESULT out[ 3 ];
    memset ( out, 0, sizeof(out) );
    bool ok = call_dispatch ( DBGAPI_CMD_DASM, &dp, out );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC100, out[ 0 ].addr );
    TEST_ASSERT_EQUAL_INT ( 1, out[ 0 ].num_bytes );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, out[ 0 ].bytes[ 0 ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC101, out[ 1 ].addr );
    TEST_ASSERT_EQUAL_HEX16 ( 0xC102, out[ 2 ].addr );
    TEST_ASSERT_EQUAL_INT ( 2, out[ 2 ].num_bytes );
}


void test_cmd_dasm_null_args_fail ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_DASM, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* ========================================================================= */
/*  CMD_HISTORY_GET - snímek g_debugger_history.row                            */
/* ========================================================================= */


void test_cmd_history_get_copies_buffer ( void ) {
    /* Patnout testovací hodnoty do history bufferu. */
    g_debugger_history.row[ 0 ].addr = 0xABCD;
    g_debugger_history.row[ 0 ].byte[ 0 ] = 0x12;
    g_debugger_history.row[ 5 ].addr = 0x1234;

    st_DEBUGGER_HISTORY_ROW out[ DEBUGGER_HISTORY_LENGTH ];
    memset ( out, 0, sizeof(out) );
    bool ok = call_dispatch ( DBGAPI_CMD_HISTORY_GET, NULL, out );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_HEX16 ( 0xABCD, out[ 0 ].addr );
    TEST_ASSERT_EQUAL_HEX8 ( 0x12, out[ 0 ].byte[ 0 ] );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, out[ 5 ].addr );
}


void test_cmd_history_get_null_result_fails ( void ) {
    bool ok = call_dispatch ( DBGAPI_CMD_HISTORY_GET, NULL, NULL );
    TEST_ASSERT_FALSE ( ok );
}


/* Pozice ringu se čte ve stejném příkazu jako řádky (data_ptr = unsigned*),
 * ne až po návratu mimo emu vlákno (MCP history_get). */
void test_cmd_history_get_returns_position ( void ) {
    g_debugger_history.position = 7;
    st_DEBUGGER_HISTORY_ROW out[ DEBUGGER_HISTORY_LENGTH ];
    unsigned position = 0xFFFFFFFFu;
    bool ok = call_dispatch ( DBGAPI_CMD_HISTORY_GET, &position, out );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_UINT ( 7, position );
}


/* ========================================================================= */
/*  CMD_MEM_WRITE_CHECKED - zamítnutí kontrolou regionu                      */
/* ========================================================================= */


/**
 * @brief Zamítnutý zápis (ROM) skončí neúspěchem příkazu, ne úspěchem.
 *
 * Dřív handler při zamítnutí vracel rq->success = true; Activity pak
 * zamítnutý mem_write ukázala jako provedený a záznam vstupů ho
 * zaznamenal. Test namapuje dolní ROM (0000h-0FFFh) a mapování po sobě
 * vrátí.
 */
void test_cmd_mem_write_checked_rejected_fails ( void ) {
    const uint8_t saved_map = g_memory.map;
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_0000;
    memory_reconnect_ram ( );
    TEST_ASSERT_EQUAL_INT ( MEMMAP_KIND_ROM_LOW, memmap_query ( 0x0 ) );

    uint8_t data[2] = { 0x12, 0x34 };
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.addr = 0x0100;
    p.length = 2;
    p.data = data;
    p.success = 1;
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL );

    g_memory.map = saved_map;
    memory_reconnect_ram ( );

    TEST_ASSERT_FALSE ( ok );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p.success );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0100, p.first_failed_addr );
    TEST_ASSERT_EQUAL_UINT8 ( MEMMAP_KIND_ROM_LOW, p.first_failed_kind );
}


/**
 * @brief Povolený zápis (RAM) skončí úspěchem příkazu i zápisu.
 */
void test_cmd_mem_write_checked_ram_succeeds ( void ) {
    int page = -1;
    for ( int p = 1; p < 16; p++ ) {
        if ( memmap_query ( (uint8_t) p ) == MEMMAP_KIND_RAM ) {
            page = p;
            break;
        };
    };
    if ( page < 0 ) TEST_IGNORE_MESSAGE ( "no RAM page mapped" );

    uint16_t addr = (uint16_t) ( ( page << 12 ) + 0x100 );
    uint8_t data[2] = { 0x5A, 0xA5 };
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.addr = addr;
    p.length = 2;
    p.data = data;
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p.success );
}


/**
 * @brief Ověří, že celý prostor 0000h-FFFFh obsahuje bajt @p value.
 * @param value Očekávaný bajt.
 * @param what Popis kroku do zprávy testu.
 */
static void assert_full_64k ( uint8_t value, const char *what ) {
    uint8_t *buf = g_malloc ( 0x10000 );
    st_DBGAPI_MEM_PARAM rp = { 0x0000, 0x10000, buf };
    TEST_ASSERT_TRUE_MESSAGE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ), what );
    uint32_t bad = 0x10000;
    for ( uint32_t i = 0; i < 0x10000; i++ ) {
        if ( buf[ i ] != value ) {
            bad = i;
            break;
        };
    };
    g_free ( buf );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 0x10000, bad, what );
}


/**
 * @brief Zápis celých 64 KiB (addr 0, délka 65536) přes MEM_WRITE_CHECKED,
 *        MEM_WRITE_FORCE i přes přehrání zaznamenaného zápisu (N61).
 *
 * Délka se dřív předávala v uint16_t a 65536 přeteklo na 0: MEM_WRITE_CHECKED
 * skončil chybou regionu na 0000h, MEM_WRITE_FORCE "uspěl" bez zápisu
 * a přehrání záznamu zapsalo 0 bajtů. Test přepne mapování na samou RAM
 * (map = 0, bez ROM, CG-ROM, VRAM a mapovaných portů), po sobě vrátí
 * mapování i obsah paměti.
 */
/**
 * @brief Volání DBGAPI_CMD_COVERAGE s danou operací.
 * @param op    Operace.
 * @param level Úroveň (START).
 * @param snap  Cíl snímku (SNAPSHOT), jinak NULL.
 * @param st    Výstup stavu po operaci.
 * @return rq->success.
 */
static bool coverage_cmd ( en_DBGAPI_COVERAGE_OP op, en_COVERAGE_LEVEL level,
                           st_COVERAGE_SNAPSHOT *snap, st_COVERAGE_STATUS *st ) {
    st_DBGAPI_COVERAGE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = op;
    p.cfg.level = level;
    p.cfg.key = COVERAGE_KEY_OPCODE;
    p.cfg.capacity = 64;
    p.reset = true;
    p.snap = snap;
    bool ok = call_dispatch ( DBGAPI_CMD_COVERAGE, &p, NULL );
    if ( st ) *st = p.out_status;
    return ok;
}


/**
 * @brief DBGAPI_CMD_COVERAGE a háček po instrukci nad skutečnou pamětí a CPU.
 *
 * START branch zapne bit masky háčků bez logovacích callbacků, krok JR NZ
 * (bajty z paměti po kroku) dá výsledek podle F. START detail vynutí
 * logovací callbacky; krok bere bajty ze záznamu fetch, buňka je RAM (nebo
 * banka MemExt) na adrese kroku; chybějící bajt v záznamu =
 * fetch_fallback. STOP vrátí
 * masku i callbacky.
 */
void test_cmd_coverage_hook ( void ) {
    z80_t *cpu = g_mzarch_main.cpu;
    z80_mread_cb cb_before = cpu->mread_cb;
    static const uint8_t jrnz[] = { 0x20, 0x10 };
    st_DBGAPI_MEM_PARAM mp = { .addr = 0x3000, .len = 2, .buf = (uint8_t *) jrnz };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_FORCE, &mp, NULL ) );

    st_COVERAGE_STATUS st;
    TEST_ASSERT_TRUE ( coverage_cmd ( DBGAPI_COVERAGE_OP_START, COVERAGE_LEVEL_BRANCH, NULL, &st ) );
    TEST_ASSERT_TRUE ( st.active );
    TEST_ASSERT_TRUE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_COVERAGE );
    TEST_ASSERT_EQUAL_INT ( 0, g_coverage_need_fetch );
    TEST_ASSERT_TRUE ( cpu->mread_cb == cb_before );

    cpu->af.l = 0x00;   /* NZ -> skok */
    st_MZARCH_INSN_INFO insn = { .pc = 0x3000, .next_pc = 0x3012, .tstates = 12, .wait = 0 };
    coverage_hook_on_instruction ( &insn );

    st_COVERAGE_SNAPSHOT *snap = g_new0 ( st_COVERAGE_SNAPSHOT, 1 );
    TEST_ASSERT_TRUE ( coverage_cmd ( DBGAPI_COVERAGE_OP_SNAPSHOT, COVERAGE_LEVEL_BRANCH, snap, &st ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, snap->exec[ 0x3000 >> 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, snap->taken[ 0x3000 >> 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, snap->not_taken[ 0x3000 >> 3 ] );
    TEST_ASSERT_NULL ( snap->units );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.branches_taken_only );

    /* detail: bajty ze záznamu fetch (v paměti jiný obsah, rozhoduje fetch) */
    TEST_ASSERT_TRUE ( coverage_cmd ( DBGAPI_COVERAGE_OP_START, COVERAGE_LEVEL_DETAIL, NULL, &st ) );
    TEST_ASSERT_EQUAL_INT ( 1, g_coverage_need_fetch );
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );
    TEST_ASSERT_TRUE ( cpu->mread_cb != cb_before );
    g_debugger_insn_fetch.addr = 0x3000;
    g_debugger_insn_fetch.count = 2;
    g_debugger_insn_fetch.byte[ 0 ] = 0x28;   /* JR Z */
    g_debugger_insn_fetch.byte[ 1 ] = 0x10;
    cpu->af.l = 0x00;   /* NZ -> JR Z propadne */
    insn.next_pc = 0x3002;
    coverage_hook_on_instruction ( &insn );
    g_debugger_insn_fetch.count = 1;          /* operand chybí -> fallback */
    coverage_hook_on_instruction ( &insn );

    TEST_ASSERT_TRUE ( coverage_cmd ( DBGAPI_COVERAGE_OP_SNAPSHOT, COVERAGE_LEVEL_DETAIL, snap, &st ) );
    TEST_ASSERT_EQUAL_UINT32 ( 1, snap->n_units );
    TEST_ASSERT_NOT_NULL ( snap->units );
    TEST_ASSERT_EQUAL_HEX8 ( 0x28, snap->units[ 0 ].key[ 0 ] );
    TEST_ASSERT_EQUAL_UINT32 ( 2, snap->units[ 0 ].not_taken );
    /* Stránka 3 čte z hlavní RAM, nebo s připojeným MemExt z jeho banky 0. */
    uint8_t space = COVERAGE_CELL_SPACE ( snap->units[ 0 ].cell );
    TEST_ASSERT_TRUE ( space == RAMLOG_SPACE_RAM || space == RAMLOG_SPACE_MEMEXT );
    TEST_ASSERT_EQUAL_HEX32 ( 0x3000, COVERAGE_CELL_OFFSET ( snap->units[ 0 ].cell ) );
    TEST_ASSERT_TRUE ( snap->units[ 0 ].flags & COVERAGE_F_FETCH_FALLBACK );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.fetch_fallback );
    TEST_ASSERT_FALSE ( st.complete );
    coverage_snapshot_clear ( snap );

    /* Snímek bez jednotek (okna GUI): bitmapy ano, tabulka ne. */
    {
        st_DBGAPI_COVERAGE_PARAM sp;
        memset ( &sp, 0, sizeof ( sp ) );
        sp.op = DBGAPI_COVERAGE_OP_SNAPSHOT;
        sp.snap = snap;
        sp.snap_without_units = true;
        TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_COVERAGE, &sp, NULL ) );
        TEST_ASSERT_NULL ( snap->units );
        TEST_ASSERT_EQUAL_UINT32 ( 0, snap->n_units );
        TEST_ASSERT_EQUAL_UINT32 ( 1, snap->status.units );
        TEST_ASSERT_EQUAL_HEX8 ( 0x01, snap->exec[ 0x3000 >> 3 ] );
    }
    g_free ( snap );

    st_COVERAGE_STATUS mk;
    st_DBGAPI_COVERAGE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = DBGAPI_COVERAGE_OP_MARK;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_COVERAGE, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT32 ( 1, p.out_epoch );

    TEST_ASSERT_TRUE ( coverage_cmd ( DBGAPI_COVERAGE_OP_STOP, COVERAGE_LEVEL_DETAIL, NULL, &mk ) );
    TEST_ASSERT_FALSE ( mk.active );
    TEST_ASSERT_TRUE ( mk.has_data );
    TEST_ASSERT_FALSE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_COVERAGE );
    TEST_ASSERT_EQUAL_INT ( 0, g_coverage_need_fetch );
    TEST_ASSERT_TRUE ( cpu->mread_cb == cb_before );
    coverage_free ( );
}


void test_cmd_mem_write_full_64k ( void ) {
    const uint8_t saved_map = g_memory.map;
    g_memory.map = 0;
    memory_reconnect_ram ( );
    bool all_ram = true;
    for ( int pg = 0; pg < 16; pg++ ) {
        if ( memmap_query ( (uint8_t) pg ) != MEMMAP_KIND_RAM ) all_ram = false;
    };
    if ( !all_ram ) {
        g_memory.map = saved_map;
        memory_reconnect_ram ( );
        TEST_IGNORE_MESSAGE ( "map 0 is not all RAM" );
    };

    uint8_t *saved = g_malloc ( 0x10000 );
    uint8_t *pattern = g_malloc ( 0x10000 );
    st_DBGAPI_MEM_PARAM sp = { 0x0000, 0x10000, saved };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &sp, NULL ) );

    /* MEM_WRITE_CHECKED: kontrola regionu přes všech 16 stránek projde. */
    memset ( pattern, 0x5A, 0x10000 );
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM cp;
    memset ( &cp, 0, sizeof ( cp ) );
    cp.addr = 0x0000;
    cp.length = 0x10000;
    cp.data = pattern;
    bool ok_checked = call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &cp, NULL );
    uint8_t checked_success = cp.success;
    assert_full_64k ( 0x5A, "mem_write_checked" );

    /* MEM_WRITE_FORCE. */
    memset ( pattern, 0xA5, 0x10000 );
    st_DBGAPI_MEM_PARAM fp = { 0x0000, 0x10000, pattern };
    bool ok_force = call_dispatch ( DBGAPI_CMD_MEM_WRITE_FORCE, &fp, NULL );
    assert_full_64k ( 0xA5, "mem_write_force" );

    /* Záznam a přehrání zápisu 64 KiB. */
    memset ( pattern, 0x3C, 0x10000 );
    st_DBGAPI_MEM_PARAM rp = { 0x0000, 0x10000, pattern };
    char *args = replay_dbg_args_to_json ( DBGAPI_CMD_MEM_WRITE_FORCE, &rp );
    TEST_ASSERT_NOT_NULL ( args );
    char *err = NULL;
    bool ok_replay = replay_dbg_execute ( "mem_write_force", args, &err );
    g_free ( args );
    assert_full_64k ( 0x3C, "replay mem_write_force" );

    st_DBGAPI_MEM_PARAM wp = { 0x0000, 0x10000, saved };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );
    g_free ( saved );
    g_free ( pattern );
    g_memory.map = saved_map;
    memory_reconnect_ram ( );

    TEST_ASSERT_TRUE ( ok_checked );
    TEST_ASSERT_EQUAL_UINT8 ( 1, checked_success );
    TEST_ASSERT_TRUE ( ok_force );
    TEST_ASSERT_TRUE_MESSAGE ( ok_replay, err ? err : "" );
    g_free ( err );
}


/* ========================================================================= */
/*  CMD_STATE_OBSERVE - registry a okna paměti z jednoho okamžiku (M10)       */
/* ========================================================================= */


/**
 * @brief Najde stránku 4 KiB s RAM (1..15), jinak test přeskočí.
 * @return Adresa 100h bajtů za začátkem stránky.
 */
static uint16_t observe_ram_addr ( void ) {
    for ( int p = 1; p < 16; p++ ) {
        if ( memmap_query ( (uint8_t) p ) == MEMMAP_KIND_RAM ) {
            return (uint16_t) ( ( p << 12 ) + 0x100 );
        };
    };
    TEST_IGNORE_MESSAGE ( "no RAM page mapped" );
    return 0;
}


/**
 * @brief Okno CPU i okno regionu RAM vrátí zapsaná data, registry a čas
 *        odpovídají stavu, hash části cpu se rovná state_hash_part().
 */
void test_cmd_state_observe_windows_regs_hash ( void ) {
    uint16_t addr = observe_ram_addr ( );
    uint8_t pattern[ 4 ] = { 0x11, 0x22, 0x33, 0x44 };
    st_DBGAPI_MEM_PARAM wp = { addr, 4, pattern };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, 0xBEEF );
    g_emulator.paused = true;

    uint8_t cpu_buf[ 4 ] = { 0 };
    uint8_t reg_buf[ 4 ] = { 0 };
    st_DBGAPI_OBSERVE_WINDOW w[ 2 ];
    memset ( w, 0, sizeof ( w ) );
    w[ 0 ].start = addr;
    w[ 0 ].len = 4;
    w[ 0 ].buf = cpu_buf;
    w[ 1 ].by_region = 1;
    w[ 1 ].region_kind = REGION_KIND_RAM;
    w[ 1 ].sub_id = 0;
    w[ 1 ].start = addr;
    w[ 1 ].len = 4;
    w[ 1 ].buf = reg_buf;

    st_DBGAPI_STATE_OBSERVE_PARAM *p = g_new0 ( st_DBGAPI_STATE_OBSERVE_PARAM, 1 );
    p->windows = w;
    p->window_count = 2;
    p->hash_mask = 1u << STATE_HASH_CPU;
    p->want_memory_map = 1;
    p->which = DBGAPI_CPU_PANEL_WANT_RASTER;
    bool ok = call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL );
    g_emulator.paused = false;

    TEST_ASSERT_TRUE ( ok );
    TEST_ASSERT_EQUAL_UINT8 ( DBGAPI_OBSERVE_WINDOW_OK, w[ 0 ].status );
    TEST_ASSERT_EQUAL_UINT8 ( DBGAPI_OBSERVE_WINDOW_OK, w[ 1 ].status );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( pattern, cpu_buf, 4 );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( pattern, reg_buf, 4 );
    TEST_ASSERT_EQUAL_UINT32 ( 0x10000, w[ 1 ].region_size );
    TEST_ASSERT_EQUAL_HEX16 ( 0xBEEF, p->panel.regs[ Z80_REG_HL ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p->panel.raster_valid );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p->panel.im2_valid );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p->running );
    TEST_ASSERT_EQUAL_UINT32 ( g_mzarch_main.cpu->total_cycles, p->total_cycles );

    char want[ STATE_HASH_HEX_LEN + 1 ];
    TEST_ASSERT_TRUE ( state_hash_part ( STATE_HASH_CPU, want ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p->hash_ok[ STATE_HASH_CPU ] );
    TEST_ASSERT_EQUAL_STRING ( want, p->hash_hex[ STATE_HASH_CPU ] );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p->hash_ok[ STATE_HASH_RAM ] );

    TEST_ASSERT_EQUAL_INT ( 16, p->memory_map.slot_count );
    for ( uint8_t page = 0; page < 16; page++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) memmap_query ( page ), p->page_kind[ page ] );
    };
    g_free ( p );
}


/**
 * @brief Nedostupný region a okno přes konec regionu: příkaz uspěje,
 *        okno má status a vynulovaný buffer.
 */
void test_cmd_state_observe_unavailable_and_out_of_range ( void ) {
    uint8_t gone[ 8 ];
    uint8_t over[ 8 ];
    memset ( gone, 0xEE, sizeof ( gone ) );
    memset ( over, 0xEE, sizeof ( over ) );
    st_DBGAPI_OBSERVE_WINDOW w[ 2 ];
    memset ( w, 0, sizeof ( w ) );
    w[ 0 ].by_region = 1;
    w[ 0 ].region_kind = REGION_KIND_RAMDISK_PEZIK;
    w[ 0 ].sub_id = 200;
    w[ 0 ].len = 8;
    w[ 0 ].buf = gone;
    w[ 1 ].by_region = 1;
    w[ 1 ].region_kind = REGION_KIND_RAM;
    w[ 1 ].start = 0xFFFC;
    w[ 1 ].len = 8;
    w[ 1 ].buf = over;

    st_DBGAPI_STATE_OBSERVE_PARAM *p = g_new0 ( st_DBGAPI_STATE_OBSERVE_PARAM, 1 );
    p->windows = w;
    p->window_count = 2;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( DBGAPI_OBSERVE_WINDOW_UNAVAILABLE, w[ 0 ].status );
    TEST_ASSERT_EQUAL_UINT8 ( DBGAPI_OBSERVE_WINDOW_OUT_OF_RANGE, w[ 1 ].status );
    TEST_ASSERT_EQUAL_UINT32 ( 0x10000, w[ 1 ].region_size );
    for ( int i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_HEX8 ( 0, gone[ i ] );
        TEST_ASSERT_EQUAL_HEX8 ( 0, over[ i ] );
    };
    g_free ( p );
}


/**
 * @brief Neplatné parametry: příkaz selže a okna nečte.
 */
void test_cmd_state_observe_limits ( void ) {
    static uint8_t big[ 0x10000 ];
    uint8_t one[ 4 ];
    st_DBGAPI_OBSERVE_WINDOW w[ 17 ];
    st_DBGAPI_STATE_OBSERVE_PARAM *p = g_new0 ( st_DBGAPI_STATE_OBSERVE_PARAM, 1 );

    /* 17 oken. */
    memset ( w, 0, sizeof ( w ) );
    for ( int i = 0; i < 17; i++ ) {
        w[ i ].len = 1;
        w[ i ].buf = one;
    };
    p->windows = w;
    p->window_count = 17;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );

    /* Součet nad 64 KiB. */
    memset ( w, 0, sizeof ( w ) );
    w[ 0 ].len = 0x10000;
    w[ 0 ].buf = big;
    w[ 1 ].len = 1;
    w[ 1 ].buf = one;
    p->window_count = 2;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );

    /* CPU okno přes 0FFFFh, nulová délka, chybějící buffer. */
    memset ( w, 0, sizeof ( w ) );
    w[ 0 ].start = 0xFFFF;
    w[ 0 ].len = 2;
    w[ 0 ].buf = one;
    p->window_count = 1;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );
    w[ 0 ].start = 0;
    w[ 0 ].len = 0;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );
    w[ 0 ].len = 1;
    w[ 0 ].buf = NULL;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );

    /* Okna chybí, ale počet nenulový; NULL parametr. */
    p->windows = NULL;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, NULL, NULL ) );

    /* Bez oken je to platné čtení jen registrů a času. */
    p->window_count = 0;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_STATE_OBSERVE, p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p->panel.regs_valid );
    g_free ( p );
}


/* ========================================================================= */
/*  Podmíněné zápisy (observe-cas M10, V1)                                    */
/* ========================================================================= */


/**
 * @brief SHA-256 bloku (GLib), binárně.
 * @param data Data.
 * @param len Délka.
 * @param[out] out 32 bajtů.
 */
static void cond_sha256 ( const uint8_t *data, size_t len, uint8_t out[ 32 ] ) {
    GChecksum *cs = g_checksum_new ( G_CHECKSUM_SHA256 );
    g_checksum_update ( cs, data, (gssize) len );
    gsize dlen = 32;
    g_checksum_get_digest ( cs, out, &dlen );
    g_checksum_free ( cs );
}


/**
 * @brief Zapíše do RAM výchozí obsah a vrátí jeho hash.
 * @param addr Adresa (RAM).
 * @param data 4 bajty.
 * @param[out] hash SHA-256 zapsaných bajtů.
 */
static void cond_prepare ( uint16_t addr, const uint8_t data[ 4 ], uint8_t hash[ 32 ] ) {
    st_DBGAPI_MEM_PARAM wp = { addr, 4, (uint8_t *) data };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );
    cond_sha256 ( data, 4, hash );
}


/**
 * @brief MEM_WRITE_CHECKED: správný hash zapíše a vrátí before/after,
 *        neplatný hash nic nezapíše a skončí neúspěchem příkazu.
 */
void test_cond_mem_write_checked ( void ) {
    uint16_t addr = observe_ram_addr ( );
    const uint8_t old_data[ 4 ] = { 1, 2, 3, 4 };
    uint8_t new_data[ 4 ] = { 9, 8, 7, 6 };
    uint8_t h_old[ 32 ], h_new[ 32 ];
    cond_prepare ( addr, old_data, h_old );
    cond_sha256 ( new_data, 4, h_new );

    st_DBGAPI_WRITE_COND c;
    memset ( &c, 0, sizeof ( c ) );
    c.has_expected = 1;
    memcpy ( c.expected, h_old, 32 );
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.addr = addr;
    p.length = 4;
    p.data = new_data;
    p.cond = &c;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0, c.precondition_failed );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.hashes_valid );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.written );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_old, c.before, 32 );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_new, c.after, 32 );

    /* Druhý zápis se stejným (už starým) hashem: odmítnout, nic nezapsat. */
    uint8_t other[ 4 ] = { 0xAA, 0xAA, 0xAA, 0xAA };
    p.data = other;
    p.success = 1;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.precondition_failed );
    TEST_ASSERT_EQUAL_UINT8 ( 0, c.written );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p.success );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_new, c.before, 32 );
    uint8_t now[ 4 ];
    st_DBGAPI_MEM_PARAM rp = { addr, 4, now };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( new_data, now, 4 );

    /* Jen want_hashes: zapíše, vrátí oba hashe. */
    memset ( &c, 0, sizeof ( c ) );
    c.want_hashes = 1;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.written );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_new, c.before, 32 );
}


/**
 * @brief MEM_WRITE_CHECKED do ROM s neplatným hashem: má přednost chyba
 *        regionu, podmínka se nevyhodnotí.
 */
void test_cond_mem_write_checked_region_first ( void ) {
    const uint8_t saved_map = g_memory.map;
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_0000;
    memory_reconnect_ram ( );
    uint8_t data[ 2 ] = { 0x12, 0x34 };
    st_DBGAPI_WRITE_COND c;
    memset ( &c, 0, sizeof ( c ) );
    c.has_expected = 1;
    memset ( c.expected, 0xEE, 32 );
    st_DBGAPI_MEM_WRITE_CHECKED_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.addr = 0x0100;
    p.length = 2;
    p.data = data;
    p.cond = &c;
    bool ok = call_dispatch ( DBGAPI_CMD_MEM_WRITE_CHECKED, &p, NULL );
    g_memory.map = saved_map;
    memory_reconnect_ram ( );
    TEST_ASSERT_FALSE ( ok );
    TEST_ASSERT_EQUAL_UINT8 ( MEMMAP_KIND_ROM_LOW, p.first_failed_kind );
    TEST_ASSERT_EQUAL_UINT8 ( 0, c.precondition_failed );
}


/**
 * @brief Souběh s člověkem: mezi čtením agenta a podmíněným zápisem MCP
 *        přepíše tentýž rozsah zápis z GUI (původ USER) -> odmítnuto;
 *        zápis GUI jinam -> podmíněný zápis projde.
 */
void test_cond_mem_write_force_user_race ( void ) {
    uint16_t addr = observe_ram_addr ( );
    const uint8_t base[ 4 ] = { 0x10, 0x20, 0x30, 0x40 };
    uint8_t h_read[ 32 ];
    cond_prepare ( addr, base, h_read );

    /* Člověk v Memory Browseru přepíše druhý bajt. */
    uint8_t human = 0x77;
    st_DBGAPI_MEM_PARAM hp = { (uint16_t) ( addr + 1 ), 1, &human };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &hp, NULL ) );

    uint8_t agent[ 4 ] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    st_DBGAPI_WRITE_COND c;
    memset ( &c, 0, sizeof ( c ) );
    c.has_expected = 1;
    memcpy ( c.expected, h_read, 32 );
    st_DBGAPI_MEM_PARAM ap = { addr, 4, agent, &c };
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_FORCE, &ap, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.precondition_failed );
    uint8_t now[ 4 ];
    st_DBGAPI_MEM_PARAM rp = { addr, 4, now };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x77, now[ 1 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x10, now[ 0 ] );

    /* Člověk změní bajt mimo rozsah: podmínka platí, zápis projde. */
    cond_prepare ( addr, base, h_read );
    st_DBGAPI_MEM_PARAM hp2 = { (uint16_t) ( addr + 8 ), 1, &human };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &hp2, NULL ) );
    memset ( &c, 0, sizeof ( c ) );
    c.has_expected = 1;
    memcpy ( c.expected, h_read, 32 );
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE_FORCE, &ap, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0, c.precondition_failed );
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( agent, now, 4 );
}


/**
 * @brief REGIONS_WRITE (RAM): podmínka nad bajty regionu, před i po zápisu.
 */
void test_cond_regions_write ( void ) {
    uint16_t addr = observe_ram_addr ( );
    const uint8_t base[ 4 ] = { 5, 6, 7, 8 };
    uint8_t h_old[ 32 ], h_new[ 32 ];
    cond_prepare ( addr, base, h_old );
    st_REGION_DESC d;
    dbgapi_regions_refresh ( );
    TEST_ASSERT_TRUE ( dbgapi_regions_find_kind ( REGION_KIND_RAM, 0, &d ) );

    uint8_t data[ 4 ] = { 0xC1, 0xC2, 0xC3, 0xC4 };
    cond_sha256 ( data, 4, h_new );
    st_DBGAPI_WRITE_COND c;
    memset ( &c, 0, sizeof ( c ) );
    c.has_expected = 1;
    memset ( c.expected, 0x00, 32 );
    st_DBGAPI_REGIONS_WRITE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.region_id = d.id;
    p.offset = addr;
    p.data = data;
    p.len = 4;
    p.cond = &c;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_REGIONS_WRITE, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.precondition_failed );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_old, c.before, 32 );

    memcpy ( c.expected, h_old, 32 );
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_REGIONS_WRITE, &p, NULL ) );
    TEST_ASSERT_EQUAL_INT ( 4, p.out_count );
    TEST_ASSERT_EQUAL_UINT8 ( 0, c.precondition_failed );
    TEST_ASSERT_EQUAL_UINT8 ( 1, c.written );
    TEST_ASSERT_EQUAL_HEX8_ARRAY ( h_new, c.after, 32 );
}


/**
 * @brief SET_REG s expected: CAS na registr, previous vždy.
 */
void test_cond_set_reg ( void ) {
    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, 0x1234 );
    st_DBGAPI_REG_PARAM p = { (uint8_t) Z80_REG_HL, 0x5678, 1, 0x1111, 0, 0 };
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p.precondition_failed );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, p.previous );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, z80_get_reg ( g_mzarch_main.cpu, Z80_REG_HL ) );

    p.expected = 0x1234;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p.precondition_failed );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5678, z80_get_reg ( g_mzarch_main.cpu, Z80_REG_HL ) );

    /* Bez podmínky: zapíše a vrátí previous. */
    st_DBGAPI_REG_PARAM q = { (uint8_t) Z80_REG_HL, 0x0001 };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_REG, &q, NULL ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5678, q.previous );
}


/**
 * @brief Revize stavu (Z3): zápis ji zvedne, čtení a pauza v pauze ne,
 *        nesplněná podmínka zápisu ne.
 */
void test_state_rev_bumped_by_mutating_commands ( void ) {
    g_emulator.paused = true;
    uint8_t buf[ 2 ] = { 0 };
    st_DBGAPI_MEM_PARAM rp = { 0xC000, 2, buf };
    uint64_t r0 = g_state_rev;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ) );
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_PAUSE, NULL, NULL ) );
    TEST_ASSERT_EQUAL_UINT64_MESSAGE ( r0, g_state_rev, "čtení a pauza v pauze revizi nezvednou" );

    uint8_t data[ 2 ] = { 0x12, 0x34 };
    st_DBGAPI_MEM_PARAM wp = { 0xC000, 2, data };
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );
    TEST_ASSERT_EQUAL_UINT64_MESSAGE ( r0 + 1, g_state_rev, "zápis paměti zvedne revizi" );

    z80_set_reg ( g_mzarch_main.cpu, Z80_REG_HL, 0x1234 );
    st_DBGAPI_REG_PARAM p = { (uint8_t) Z80_REG_HL, 0x5678, 1, 0x1111, 0, 0 };
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT64_MESSAGE ( r0 + 1, g_state_rev, "nesplněná podmínka revizi nezvedne" );
    g_emulator.paused = false;
}


/** @brief Jako call_dispatch(), s volbami revize stavu ve slotu. */
/**
 * @brief Zdroje zvednutí revize: příkaz MCP klienta = `mcp`, jiný původ =
 *        `debugger`; ohlášení zdroje vynuluje, nahrazení zdroje po příkazu
 *        zmizí.
 */
void test_state_rev_sources ( void ) {
    g_emulator.paused = true;
    uint8_t data[ 1 ] = { 0x55 };
    st_DBGAPI_MEM_PARAM wp = { 0xC000, 1, data };
    state_rev_mark_announced ( );
    TEST_ASSERT_EQUAL_UINT32 ( 0, state_rev_take_unannounced ( NULL ) );

    st_DBGAPI_CMDRQ rq;
    memset ( &rq, 0, sizeof (rq) );
    rq.cmd = DBGAPI_CMD_MEM_WRITE;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.cmd_origin = DBGAPI_CMD_ORIGIN_MCP;
    rq.data_ptr = &wp;
    dbgapi_emu_dispatch ( &rq );
    TEST_ASSERT_TRUE ( rq.success );
    uint64_t rev = 0;
    TEST_ASSERT_EQUAL_UINT32 ( STATE_REV_SRC_MCP, state_rev_take_unannounced ( &rev ) );
    TEST_ASSERT_EQUAL_UINT64 ( g_state_rev, rev );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 0, state_rev_take_unannounced ( NULL ), "ohlášená revize nemá zdroje" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 0, g_state_rev_src_override, "nahrazení zdroje platí jen během příkazu" );

    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_WRITE, &wp, NULL ) );
    TEST_ASSERT_EQUAL_UINT32 ( STATE_REV_SRC_DEBUGGER, state_rev_take_unannounced ( NULL ) );
    TEST_ASSERT_EQUAL_STRING ( "device", state_rev_src_name ( STATE_REV_SRC_DEVICE ) );
    TEST_ASSERT_EQUAL_STRING ( "unknown", state_rev_src_name ( 1u << STATE_REV_SRC_COUNT_BITS ) );
    g_emulator.paused = false;
}


static bool call_dispatch_opts ( en_DBGAPI_CMD cmd, void *data_ptr, st_DBGAPI_SUBMIT_OPTS *opts ) {
    st_DBGAPI_CMDRQ rq;
    memset ( &rq, 0, sizeof (rq) );
    rq.cmd = cmd;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = data_ptr;
    rq.opts = opts;
    dbgapi_emu_dispatch ( &rq );
    return rq.success;
}


/**
 * @brief Podmínky revize stavu ve slotu: EXPECT_EQUAL zapíše jen při
 *        shodné revizi v pauze, SKIP_IF_EQUAL při shodě příkaz přeskočí;
 *        výstupy revize a běhu jsou vyplněné.
 */
void test_state_rev_conditions ( void ) {
    g_emulator.paused = true;
    uint8_t data[ 1 ] = { 0x5A };
    st_DBGAPI_MEM_PARAM wp = { 0xC100, 1, data };
    uint8_t back[ 1 ] = { 0 };
    st_DBGAPI_MEM_PARAM rp = { 0xC100, 1, back };

    st_DBGAPI_SUBMIT_OPTS o;
    memset ( &o, 0, sizeof ( o ) );
    o.rev_cond = DBGAPI_REV_COND_EXPECT_EQUAL;
    o.rev_value = g_state_rev;
    uint64_t r0 = g_state_rev;
    TEST_ASSERT_TRUE ( call_dispatch_opts ( DBGAPI_CMD_MEM_WRITE, &wp, &o ) );
    TEST_ASSERT_FALSE ( o.rev_failed );
    TEST_ASSERT_EQUAL_UINT64 ( r0 + 1, o.state_rev_after );
    TEST_ASSERT_FALSE ( o.running_after );

    /* Stará revize: nic se nezapíše, revize beze změny. */
    data[ 0 ] = 0xA5;
    o.rev_value = r0;
    TEST_ASSERT_FALSE ( call_dispatch_opts ( DBGAPI_CMD_MEM_WRITE, &wp, &o ) );
    TEST_ASSERT_TRUE ( o.rev_failed );
    TEST_ASSERT_EQUAL_UINT64 ( r0 + 1, g_state_rev );
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_MEM_READ, &rp, NULL ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x5A, back[ 0 ] );

    /* Za běhu EXPECT_EQUAL nikdy neplatí. */
    g_emulator.paused = false;
    o.rev_value = g_state_rev;
    TEST_ASSERT_FALSE ( call_dispatch_opts ( DBGAPI_CMD_MEM_WRITE, &wp, &o ) );
    TEST_ASSERT_TRUE ( o.rev_failed );
    TEST_ASSERT_TRUE ( o.running_after );
    g_emulator.paused = true;

    /* SKIP_IF_EQUAL: shoda = přeskočit (buffer nezměněn), rozdíl = provést. */
    memset ( &o, 0, sizeof ( o ) );
    o.rev_cond = DBGAPI_REV_COND_SKIP_IF_EQUAL;
    o.rev_value = g_state_rev;
    back[ 0 ] = 0;
    TEST_ASSERT_TRUE ( call_dispatch_opts ( DBGAPI_CMD_MEM_READ, &rp, &o ) );
    TEST_ASSERT_TRUE ( o.rev_skipped );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, back[ 0 ] );
    o.rev_value = g_state_rev - 1;
    TEST_ASSERT_TRUE ( call_dispatch_opts ( DBGAPI_CMD_MEM_READ, &rp, &o ) );
    TEST_ASSERT_FALSE ( o.rev_skipped );
    TEST_ASSERT_EQUAL_HEX8 ( 0x5A, back[ 0 ] );
    g_emulator.paused = false;
}


/**
 * @brief SET_CPU_FLAGS s expect_mask: při neshodě nic nezapsat, prev_* vždy.
 */
void test_cond_set_cpu_flags ( void ) {
    z80_t *cpu = g_mzarch_main.cpu;
    cpu->iff1 = 1;
    cpu->im = 1;
    cpu->r = 0x85;
    st_DBGAPI_CPU_FLAGS p;
    memset ( &p, 0, sizeof ( p ) );
    p.update_mask = DBGAPI_CPU_FLAGS_UM_IFF1 | DBGAPI_CPU_FLAGS_UM_R_FULL;
    p.iff1 = 0;
    p.r_reg = 0x10;
    p.expect_mask = DBGAPI_CPU_FLAGS_UM_IFF1 | DBGAPI_CPU_FLAGS_UM_R_FULL;
    p.exp_iff1 = 1;
    p.exp_r = 0x05;
    TEST_ASSERT_FALSE ( call_dispatch ( DBGAPI_CMD_SET_CPU_FLAGS, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p.precondition_failed );
    TEST_ASSERT_EQUAL_UINT8 ( 0x85, p.prev_r );
    TEST_ASSERT_TRUE ( cpu->iff1 );
    TEST_ASSERT_EQUAL_HEX8 ( 0x85, cpu->r );

    p.exp_r = 0x85;
    TEST_ASSERT_TRUE ( call_dispatch ( DBGAPI_CMD_SET_CPU_FLAGS, &p, NULL ) );
    TEST_ASSERT_EQUAL_UINT8 ( 0, p.precondition_failed );
    TEST_ASSERT_EQUAL_UINT8 ( 1, p.prev_iff1 );
    TEST_ASSERT_FALSE ( cpu->iff1 );
    TEST_ASSERT_EQUAL_HEX8 ( 0x10, cpu->r );
}


/* ========================================================================= */
/*  MAIN                                                                      */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_cmd_none_success );

    RUN_TEST ( test_cmd_is_debugger_active_initial );
    RUN_TEST ( test_cmd_is_debugger_active_when_set );
    RUN_TEST ( test_cmd_is_debugger_active_null_result_fails );

    RUN_TEST ( test_cmd_debugger_activate_sets_flag );
    RUN_TEST ( test_cmd_debugger_deactivate_clears_flag );
    RUN_TEST ( test_cmd_debugger_activate_deactivate_swaps_callbacks );

    RUN_TEST ( test_cmd_pause_sets_paused_flag );
    RUN_TEST ( test_cmd_pause_idempotent );

    RUN_TEST ( test_cmd_force_pause_sets_paused );

    RUN_TEST ( test_cmd_run_clears_paused_flag );

    RUN_TEST ( test_cmd_is_running_when_paused );
    RUN_TEST ( test_cmd_is_running_when_running );
    RUN_TEST ( test_cmd_is_running_null_result_fails );

    RUN_TEST ( test_cmd_step_into_in_pause_sets_flag );

    RUN_TEST ( test_cmd_step_over_when_running_pauses );
    RUN_TEST ( test_cmd_step_over_when_paused_returns_success );

    RUN_TEST ( test_cmd_run_to_null_data_fails );
    RUN_TEST ( test_cmd_run_to_when_paused_sets_temporary_bp );
    RUN_TEST ( test_cmd_run_to_when_running_pauses );

    RUN_TEST ( test_cmd_reset_sets_request_flag );

    RUN_TEST ( test_cmd_get_reg_pc );
    RUN_TEST ( test_cmd_get_reg_sp );
    RUN_TEST ( test_cmd_get_reg_invalid_id_fails );
    RUN_TEST ( test_cmd_get_reg_null_args_fail );

    RUN_TEST ( test_cmd_set_reg_pc );
    RUN_TEST ( test_cmd_set_reg_ir_writes_full_pair );
    RUN_TEST ( test_cmd_set_cpu_flags_r_masks );
    RUN_TEST ( test_cmd_set_reg_invalid_id_fails );
    RUN_TEST ( test_cmd_set_reg_null_data_fails );

    RUN_TEST ( test_cmd_get_all_regs );
    RUN_TEST ( test_cmd_get_all_regs_null_result_fails );

    RUN_TEST ( test_cmd_mem_read_block );
    RUN_TEST ( test_cmd_mem_read_null_data_fails );
    RUN_TEST ( test_cmd_mem_read_null_buf_fails );

    RUN_TEST ( test_cmd_mem_write_then_read_back );
    RUN_TEST ( test_cmd_mem_write_null_data_fails );
    RUN_TEST ( test_cmd_mem_write_null_buf_fails );

    RUN_TEST ( test_cmd_bp_add_returns_id );
    RUN_TEST ( test_cmd_bp_add_null_data_fails );

    RUN_TEST ( test_cmd_bp_remove_existing );
    RUN_TEST ( test_cmd_bp_remove_nonexistent_fails );
    RUN_TEST ( test_cmd_bp_remove_null_data_fails );

    RUN_TEST ( test_cmd_bp_list_empty );
    RUN_TEST ( test_cmd_bp_list_two_bps );
    RUN_TEST ( test_cmd_bp_list_null_result_fails );

    RUN_TEST ( test_cmd_bp_update_full_struct_roundtrip );
    RUN_TEST ( test_cmd_bp_update_partial_mask );
    RUN_TEST ( test_cmd_bp_update_zero_mask_is_noop );
    RUN_TEST ( test_cmd_bp_update_nonexistent_fails );
    RUN_TEST ( test_cmd_bp_update_null_data_fails );
    RUN_TEST ( test_cmd_bp_update_match_modes_and_irq_fields );
    RUN_TEST ( test_cmd_bp_set_enabled_toggle );
    RUN_TEST ( test_cmd_bp_set_enabled_nonexistent_fails );
    RUN_TEST ( test_cmd_bp_set_enabled_null_data_fails );
    RUN_TEST ( test_cmd_bp_set_parent_reparent_and_clear );
    RUN_TEST ( test_cmd_bp_set_parent_null_data_fails );
    RUN_TEST ( test_cmd_bp_create_with_init_sp_threshold );
    RUN_TEST ( test_cmd_bp_create_with_init_wrong_id_fails );
    RUN_TEST ( test_cmd_bp_create_with_init_null_data_fails );

    RUN_TEST ( test_cmd_bpgrp_add_returns_id );
    RUN_TEST ( test_cmd_bpgrp_add_null_data_fails );
    RUN_TEST ( test_cmd_bpgrp_remove_existing );
    RUN_TEST ( test_cmd_bpgrp_remove_nonexistent_fails );
    RUN_TEST ( test_cmd_bpgrp_remove_null_data_fails );
    RUN_TEST ( test_cmd_bpgrp_update_full_roundtrip );
    RUN_TEST ( test_cmd_bpgrp_update_partial_mask );
    RUN_TEST ( test_cmd_bpgrp_update_zero_mask_is_noop );
    RUN_TEST ( test_cmd_bpgrp_update_nonexistent_fails );
    RUN_TEST ( test_cmd_bpgrp_update_null_data_fails );

    RUN_TEST ( test_cmd_dasm_block );
    RUN_TEST ( test_cmd_dasm_null_args_fail );

    RUN_TEST ( test_cmd_history_get_copies_buffer );
    RUN_TEST ( test_cmd_history_get_null_result_fails );
    RUN_TEST ( test_cmd_history_get_returns_position );

    RUN_TEST ( test_cmd_mem_write_checked_rejected_fails );
    RUN_TEST ( test_cmd_mem_write_full_64k );
    RUN_TEST ( test_cmd_coverage_hook );
    RUN_TEST ( test_cmd_mem_write_checked_ram_succeeds );

    RUN_TEST ( test_cmd_state_observe_windows_regs_hash );
    RUN_TEST ( test_cmd_state_observe_unavailable_and_out_of_range );
    RUN_TEST ( test_cmd_state_observe_limits );

    RUN_TEST ( test_cond_mem_write_checked );
    RUN_TEST ( test_cond_mem_write_checked_region_first );
    RUN_TEST ( test_cond_mem_write_force_user_race );
    RUN_TEST ( test_cond_regions_write );
    RUN_TEST ( test_cond_set_reg );
    RUN_TEST ( test_cond_set_cpu_flags );
    RUN_TEST ( test_state_rev_bumped_by_mutating_commands );
    RUN_TEST ( test_state_rev_sources );
    RUN_TEST ( test_state_rev_conditions );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
