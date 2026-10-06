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
#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "debugger/debugger.h"
#include "emulator/emulator.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"
#include "hw-generic/memory/memory.h"
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


void test_cmd_set_reg_ir_special_handling ( void ) {
    /* IR speciální handling: zapíše jen dolní bajt R (& 0x7F),
     * I (vysoký bajt) a R bit 7 zachová. */
    g_mzarch_main.cpu->i = 0xAA;
    g_mzarch_main.cpu->r = 0x80; /* bit 7 nastaven */
    st_DBGAPI_REG_PARAM p = { (uint8_t)Z80_REG_IR, 0x55BB };
    bool ok = call_dispatch ( DBGAPI_CMD_SET_REG, &p, NULL );
    TEST_ASSERT_TRUE ( ok );
    /* I se nemění, R bit 7 zachován + dolních 7 bitů z value (0xBB & 0x7F = 0x3B). */
    TEST_ASSERT_EQUAL_HEX8 ( 0xAA, g_mzarch_main.cpu->i );
    TEST_ASSERT_EQUAL_HEX8 ( 0x80 | 0x3B, g_mzarch_main.cpu->r );
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
    RUN_TEST ( test_cmd_set_reg_ir_special_handling );
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

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
