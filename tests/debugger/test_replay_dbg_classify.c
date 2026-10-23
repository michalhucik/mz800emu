/**
 * @file test_replay_dbg_classify.c
 * @brief Tabulka vlastností příkazů dbgapi (dbgapi_cmd_props.c): každý
 *        příkaz zařazený, třídy záznamu vstupů beze změny (N48, M18).
 *
 * Nezařazený příkaz by záznam vstupů propustil (ALLOW) a revize stavu ho
 * brala jako měnící. Test prochází celý enum en_DBGAPI_CMD
 * (0..DBGAPI_CMD_COUNT-1) a ověřuje:
 *
 *  - každý příkaz má druh (ne DBGAPI_CMD_KIND_UNSET),
 *  - třída záznamu jiná než ALLOW znamená druh MUTATES,
 *  - třídy záznamu vstupů jsou stejné jako před převodem na tabulku
 *    (seznamy RECORD, INPUT, FORBIDDEN níže, ostatní ALLOW).
 *
 * Kdo přidá příkaz, musí mu dát řádek v tabulce; změna třídy záznamu
 * vstupů musí změnit i seznamy níže.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <stdio.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_cmd_props.h"
#include "debugger/replay_dbg.h"


/** @brief Příkazy, které se zapíší jako událost `dbg`. */
static const en_DBGAPI_CMD s_record[] = {
    DBGAPI_CMD_MEM_WRITE,
    DBGAPI_CMD_MEM_WRITE_CHECKED,
    DBGAPI_CMD_MEM_WRITE_FORCE,
    DBGAPI_CMD_REGIONS_WRITE,
    DBGAPI_CMD_SET_REG,
    DBGAPI_CMD_SET_CPU_FLAGS,
    DBGAPI_CMD_IO_WRITE,
    DBGAPI_CMD_IRQ_INJECT,
    DBGAPI_CMD_NMI_INJECT,
    DBGAPI_CMD_CMT_TRANSPORT,
    DBGAPI_CMD_CMT_RECORD,
    DBGAPI_CMD_CMT_TAPE_SEEK,
};

/** @brief Příkazy vstupů (zachytí je vzorek vstupů). */
static const en_DBGAPI_CMD s_input[] = {
    DBGAPI_CMD_INPUT_PRESS_KEY,
    DBGAPI_CMD_INPUT_RELEASE_KEY,
    DBGAPI_CMD_INPUT_RELEASE_ALL,
    DBGAPI_CMD_INPUT_JOY_SET,
    DBGAPI_CMD_INPUT_JOY_CLEAR,
    DBGAPI_CMD_HID_SCRIPT_START,
    DBGAPI_CMD_HID_SCRIPT_CANCEL,
};

/** @brief Příkazy, které záznam vstupů odmítne. */
static const en_DBGAPI_CMD s_forbidden[] = {
    DBGAPI_CMD_SNAPSHOT_LOAD_FILE,
    DBGAPI_CMD_SNAPSHOT_LOAD_BUFFER,
    DBGAPI_CMD_MEDIA_LOAD_MZF,
    DBGAPI_CMD_MEDIA_LOAD_BINARY,
    DBGAPI_CMD_MEDIA_INSERT,
    DBGAPI_CMD_MEDIA_EJECT,
    DBGAPI_CMD_MEDIA_RUN_MZF,
    DBGAPI_CMD_SETTINGS_SET,
    DBGAPI_CMD_PLATFORM_SET,
    DBGAPI_CMD_PERIPH_ATTACH,
    DBGAPI_CMD_PERIPH_DETACH,
    DBGAPI_CMD_SET_PIOZ80_INTERRUPT_VECTOR,
    DBGAPI_CMD_IO_READ,
    DBGAPI_CMD_CMT_HACK_SET,
    DBGAPI_CMD_CMT_SET_PROPERTY,
    DBGAPI_CMD_CMT_OPEN,
    DBGAPI_CMD_CMT_TAPE_BLOCK_SPEED,
    DBGAPI_CMD_MEMMAP_SET,
    DBGAPI_CMD_FREEZE_ADD,
};


void setUp ( void ) {
}


void tearDown ( void ) {
}


/** @brief Je příkaz v seznamu? */
static bool in_list ( const en_DBGAPI_CMD *list, size_t n, en_DBGAPI_CMD cmd ) {
    for ( size_t i = 0; i < n; i++ ) {
        if ( list[ i ] == cmd ) return true;
    };
    return false;
}


/** @brief Očekávaná třída záznamu vstupů podle seznamů. */
static en_REPLAY_DBG_CLASS expected_class ( en_DBGAPI_CMD cmd ) {
    if ( in_list ( s_record, G_N_ELEMENTS ( s_record ), cmd ) ) return REPLAY_DBG_RECORD;
    if ( in_list ( s_input, G_N_ELEMENTS ( s_input ), cmd ) ) return REPLAY_DBG_INPUT;
    if ( in_list ( s_forbidden, G_N_ELEMENTS ( s_forbidden ), cmd ) ) return REPLAY_DBG_FORBIDDEN;
    return REPLAY_DBG_ALLOW;
}


/** @brief Každý příkaz enumu má druh. */
void test_every_command_has_kind ( void ) {
    for ( int c = 0; c < (int) DBGAPI_CMD_COUNT; c++ ) {
        en_DBGAPI_CMD cmd = (en_DBGAPI_CMD) c;
        char msg[ 200 ];
        g_snprintf ( msg, sizeof ( msg ),
                     "command %d (%s) has no kind: add its row to dbgapi_cmd_props.c",
                     c, dbgapi_cmd_to_str ( cmd ) );
        TEST_ASSERT_NOT_EQUAL_MESSAGE ( DBGAPI_CMD_KIND_UNSET, dbgapi_cmd_props ( cmd )->kind, msg );
    };
}


/** @brief Třída záznamu jiná než ALLOW = druh MUTATES. */
void test_replay_class_implies_mutates ( void ) {
    for ( int c = 0; c < (int) DBGAPI_CMD_COUNT; c++ ) {
        en_DBGAPI_CMD cmd = (en_DBGAPI_CMD) c;
        if ( replay_dbg_classify ( cmd ) == REPLAY_DBG_ALLOW ) continue;
        char msg[ 200 ];
        g_snprintf ( msg, sizeof ( msg ), "command %d (%s) is recorded or refused by the replay but not MUTATES",
                     c, dbgapi_cmd_to_str ( cmd ) );
        TEST_ASSERT_EQUAL_INT_MESSAGE ( DBGAPI_CMD_KIND_MUTATES, dbgapi_cmd_props ( cmd )->kind, msg );
        TEST_ASSERT_TRUE ( dbgapi_cmd_mutates ( cmd ) );
    };
}


/** @brief Třídy záznamu vstupů beze změny proti seznamům. */
void test_replay_classes_unchanged ( void ) {
    for ( int c = 0; c < (int) DBGAPI_CMD_COUNT; c++ ) {
        en_DBGAPI_CMD cmd = (en_DBGAPI_CMD) c;
        char msg[ 200 ];
        g_snprintf ( msg, sizeof ( msg ), "command %d (%s): replay class changed", c, dbgapi_cmd_to_str ( cmd ) );
        TEST_ASSERT_EQUAL_INT_MESSAGE ( expected_class ( cmd ), replay_dbg_classify ( cmd ), msg );
    };
}


/** @brief Příklady druhů a hodnota mimo enum. */
void test_known_kinds ( void ) {
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_READ, dbgapi_cmd_props ( DBGAPI_CMD_MEM_READ )->kind );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_READ, dbgapi_cmd_props ( DBGAPI_CMD_STATE_OBSERVE )->kind );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_READ, dbgapi_cmd_props ( DBGAPI_CMD_WATCH_EVAL )->kind );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_DEBUG_CONFIG, dbgapi_cmd_props ( DBGAPI_CMD_BP_ADD )->kind );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_RUN_CONTROL, dbgapi_cmd_props ( DBGAPI_CMD_PAUSE )->kind );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_MUTATES, dbgapi_cmd_props ( DBGAPI_CMD_RESET )->kind );
    TEST_ASSERT_EQUAL_INT ( REPLAY_DBG_ALLOW, replay_dbg_classify ( DBGAPI_CMD_RESET ) );
    TEST_ASSERT_TRUE ( dbgapi_cmd_mutates ( DBGAPI_CMD_MEM_WRITE ) );
    TEST_ASSERT_FALSE ( dbgapi_cmd_mutates ( DBGAPI_CMD_PAUSE ) );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_KIND_UNSET, dbgapi_cmd_props ( DBGAPI_CMD_COUNT )->kind );
    TEST_ASSERT_TRUE ( dbgapi_cmd_mutates ( DBGAPI_CMD_COUNT ) );
}


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_every_command_has_kind );
    RUN_TEST ( test_replay_class_implies_mutates );
    RUN_TEST ( test_replay_classes_unchanged );
    RUN_TEST ( test_known_kinds );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
