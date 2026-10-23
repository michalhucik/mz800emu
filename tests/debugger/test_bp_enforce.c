/*
 * test_bp_enforce.c — testy enforcement smyčky pro smart BP V1 (D.2)
 *
 * Pokrytí:
 *   - breakpoints_enforce: skip_count, condition, hit_count, action
 *     continue/stop, MSG_BREAKPOINT_HIT dispatch
 *   - breakpoints_enforce_pc_exec: integrace PC dispatch
 *   - breakpoints_enforce_mem_r/w: range match + ctx Address/Value
 *   - breakpoints_enforce_iorq_r/w: low-byte port match
 *   - breakpoints_enforce_irq: edge-raise sémantika (test úrovně
 *     wrap kolem hooku v interrupt.c má static prev - testujeme jen
 *     enforce_irq)
 *   - breakpoints_enforce_global: condition-only + edge_triggered
 *   - bptmap V2: register/unregister/get_list + per_type_active flagy
 *   - self_id: action disable_self vypne BP
 *
 * Test mockuje MSG dispatcher pro ověření odeslání DBGAPI_MSG_BREAKPOINT_HIT
 * s payload {addr, id}.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "debugger/bp_expr.h"
#include "debugger/bp_action.h"
#include "debugger/bp_vars.h"
#include "debugger/debugger.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_msg.h"
#include "debugger/bp_zone.h"
#include "emulator/emulator.h"
#include "hw-generic/memory/memext.h"
#include "debugger/run_control/run_control.h"
#include "mzarch/mzarch_config.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdg.h"
#endif

#include <string.h>
#include <glib.h>


/* ========================================================================= */
/*  Mock MSG dispatcher                                                      */
/* ========================================================================= */


/* Záznam posledního přijatého MSG. */
typedef struct {
    int call_count;             /* kolikrát byl dispatcher volaný */
    en_DBGAPI_MSG last_msg;
    int last_id;
    uint16_t last_addr;
} mock_msg_state_t;

static mock_msg_state_t s_mock_state;


/* Test mock: zachytí MSG, vyplní s_mock_state, uvolní data. */
static void mock_msg_dispatcher ( en_DBGAPI_MSG msg,
                                   st_DBGAPI_MSG_DATA *data,
                                   void *user_data ) {
    (void) user_data;
    s_mock_state.call_count++;
    s_mock_state.last_msg = msg;
    if ( data ) {
        s_mock_state.last_id = data->id;
        s_mock_state.last_addr = data->addr;
        g_free ( data );
    };
}


static void mock_msg_reset ( void ) {
    memset ( &s_mock_state, 0, sizeof ( s_mock_state ) );
}


/* ========================================================================= */
/*  setUp / tearDown                                                         */
/* ========================================================================= */


void setUp ( void ) {
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    bp_vars_clear_storage ( );
    g_emulator.paused = false;
    mock_msg_reset ( );
    dbgapi_emu_register_msg_dispatcher ( mock_msg_dispatcher, NULL );
}


void tearDown ( void ) {
    dbgapi_emu_register_msg_dispatcher ( NULL, NULL );
}


/* ========================================================================= */
/*  Helper: vytvoří BP daného typu                                            */
/* ========================================================================= */


static int make_bp ( en_BPT_TYPE type, uint16_t addr ) {
    int id = breakpoints_add ( addr, "T", -1 );
    breakpoints_set_type ( id, type );
    return id;
}


/* ========================================================================= */
/*  bptmap V2 - per-typ tabulky                                              */
/* ========================================================================= */


/**
 * Registrace BP non-PC typu zaktivuje per_type_active a any_active flagy.
 */
void test_bptmap_register_activates_flags ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    /* set_type už triggernul resync (bp byl effective enabled). */
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );
    TEST_ASSERT_TRUE ( g_bptmap.any_active );

    /* Disable -> unregister. */
    breakpoints_set_enabled ( id, false );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );
    TEST_ASSERT_FALSE ( g_bptmap.any_active );
}


/**
 * Změna typu BP přesune ID mezi per-typ listy.
 */
void test_bptmap_set_type_resyncs_lists ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_R, 0x4000 );
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_R] );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );

    breakpoints_set_type ( id, BPT_TYPE_MEM_W );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_R] );
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );

    /* PC_EXEC - žádný per-typ list, ale legacy bpmap[]. */
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );
    TEST_ASSERT_FALSE ( g_bptmap.any_active );
    TEST_ASSERT_EQUAL_INT ( id, g_bptmap.bpmap[0x4000] );
}


/* ========================================================================= */
/*  PC_EXEC enforce + skip/hit_count                                          */
/* ========================================================================= */


/**
 * BP bez condition + bez action = klasický stop, MSG odeslán.
 */
void test_pc_exec_basic_stop ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );

    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 1, s_mock_state.call_count );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_MSG_BREAKPOINT_HIT, s_mock_state.last_msg );
    TEST_ASSERT_EQUAL_INT ( id, s_mock_state.last_id );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, s_mock_state.last_addr );

    /* hits inkrementoval. */
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 1, bpt->hits );
}


/**
 * PC_EXEC mimo addr = no-op.
 */
void test_pc_exec_no_match ( void ) {
    make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_enforce_pc_exec ( 0x9999 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 0, s_mock_state.call_count );
}


/**
 * skip_count: prvních N hitů přeskočí, pak fire.
 */
void test_skip_count ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_skip_count ( id, 2 );

    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    /* 3. hit fire. */
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 1, s_mock_state.call_count );

    /* Skip dekrementuje hits ne, počítá se jen efektivní hit. */
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 1, bpt->hits );
}


/**
 * hit_count: fire jen na N. hitu.
 */
void test_hit_count ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_hit_count ( id, 3 );

    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    /* 3. hit fire. */
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 1, s_mock_state.call_count );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 3, bpt->hits );
}


/* ========================================================================= */
/*  Condition + Action                                                       */
/* ========================================================================= */


/**
 * Condition `Address == 0x1234` matchne PC_EXEC.
 */
void test_condition_match ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_expr ( id, "Address == 0x1234" );
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


/**
 * Condition false → no fire.
 */
void test_condition_false_no_fire ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_expr ( id, "0" );
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 0, s_mock_state.call_count );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT64 ( 0, bpt->hits );
}


/**
 * Action `$x += 1` neblokuje (= continue), $x se inkrementuje.
 */
void test_action_continue ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_action ( id, "$x += 1" );

    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "x" ) );
    TEST_ASSERT_EQUAL_INT ( 0, s_mock_state.call_count );

    /* Druhý hit. */
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "x" ) );
}


/**
 * Action disable_self vypne BP po prvním hitu (= self_id propaguje).
 */
void test_action_disable_self ( void ) {
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_action ( id, "disable_self" );

    breakpoints_enforce_pc_exec ( 0x1234 );
    /* Action neblokuje (= disable_self je neutrální stmt) - continue. */
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* BP je teď disabled. */
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_FALSE ( bpt->enabled );

    /* Druhý hit už nic nedělá. */
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_UINT64 ( 1, bpt->hits );  /* zůstal 1 */
}


/* ========================================================================= */
/*  MEM_R / MEM_W                                                            */
/* ========================================================================= */


/**
 * MEM_W single-point trigger + ctx Value naplnění.
 */
/* Fix C: NEED_DEBUG_CALLBACKS musí být true i jen kvůli existenci MEM/IORQ
 * breakpointu (jinak logging callback není nainstalován a BP tiše nestřílí,
 * když není otevřené žádné debug okno / recording). */
void test_need_callbacks_includes_mem_iorq_bp ( void ) {
    /* Reset: žádný recording aktivní. */
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_OFF;
    g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_OFF;
    g_iorqlog_active = 0;
    g_io_window_tracking_active = 0;
    g_eventlog_active = 0;
    memset ( g_bptmap.per_type_active, 0, sizeof ( g_bptmap.per_type_active ) );

    /* Bez callback-dispatchovaného BP: nevyžaduje debug callbacky. */
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );

    /* Každý ze čtyř callback-dispatchovaných typů musí makro zapnout. */
    g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] = true;
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );
    g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] = false;

    g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] = true;
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );
    g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] = false;

    g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_R ] = true;
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );
    g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_R ] = false;

    g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_W ] = true;
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_NEED_DEBUG_CALLBACKS );
    g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_W ] = false;
}


/* Feature D: MEM_W BP v offset módu matchuje offset v PEHU bance bez ohledu
 * na to, do kterého CPU okna je banka namapovaná. Bank check zůstává na
 * zone (bp_zone_is_active_at). */
void test_enforce_mmext_bank_offset ( void ) {
    /* PEHU: bank 9 na stránce 2 (map[2]=18 dolní, map[3]=19 horní). */
    g_memext.connection = MEMEXT_CONNECTION_YES;
    g_memext.type = MEMEXT_TYPE_PEHU;
    g_memext.map[ 2 ] = 18;
    g_memext.map[ 3 ] = 19;

    int id = make_bp ( BPT_TYPE_MEM_W, 0x0463 );   /* addr = offset v bance */
    breakpoints_set_zone ( id, BP_ZONE_MMEXT_BANK );
    breakpoints_set_bank_id ( id, 9 );
    st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, 0 );
    bpt->bp_addr_space = BP_ADDR_SPACE_BANK_OFFSET;

    /* Write do bank 9 offset 0x463 (CPU addr 0x2463) -> fire (stop). */
    g_emulator.paused = false;
    breakpoints_enforce_mem_w ( 0x2463, 0xAA );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Jiný offset (CPU 0x2464 = offset 0x464) -> no fire. */
    g_emulator.paused = false;
    breakpoints_enforce_mem_w ( 0x2464, 0xAA );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Správný offset, ale na stránce je teď jiná bank (10) -> no fire. */
    g_memext.map[ 2 ] = 20;
    g_memext.map[ 3 ] = 21;
    g_emulator.paused = false;
    breakpoints_enforce_mem_w ( 0x2463, 0xAA );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/* Bug B / Divnost 2: MEM_W (a jakýkoliv non-PC_EXEC) BP NESMÍ vystřelit na
 * EXEC své adresy. Repro create-with-init flow: PC_EXEC default -> set_type
 * MEM_W -> set_addr. set_addr dřív bezpodmínečně přidal BP do bpmap[] (jako
 * PC_EXEC), takže exec na té adrese ho spustil přes enforce_pc_exec. */
/* CPU historie v režimu WITH_WINDOW_OR_BP: k dispozici když je okno otevřené
 * NEBO je aspoň jeden BP enabled. Režim WITH_WINDOW na BP nereaguje. OFF se
 * respektuje. */
void test_cpuhist_active_when_enabled_bp ( void ) {
    g_debugger.active = 0;   /* okno zavřené -> TEST_DEBUGGER_ACTIVE false */

    /* WITH_WINDOW_OR_BP, bez BP: neaktivní. */
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP;
    breakpoints_clear_all ( );
    breakpoints_sync_bptmap ( );
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_CPUHIST_ACTIVE );

    /* WITH_WINDOW_OR_BP, s enabled BP: aktivní i bez okna. */
    int id = breakpoints_add ( 0x4000, "T", -1 );
    breakpoints_sync_bptmap ( );
    TEST_ASSERT_TRUE ( TEST_DEBUGGER_CPUHIST_ACTIVE );

    /* Režim WITH_WINDOW se stejným enabled BP: NEaktivní (BP nereaguje). */
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW;
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_CPUHIST_ACTIVE );

    /* Zpět WITH_WINDOW_OR_BP, disabled BP: neaktivní. */
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP;
    breakpoints_set_enabled ( id, false );
    breakpoints_sync_bptmap ( );
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_CPUHIST_ACTIVE );

    /* OFF + enabled BP: respektuj OFF (BP historii nepřebíjí). */
    breakpoints_set_enabled ( id, true );
    breakpoints_sync_bptmap ( );
    g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_OFF;
    TEST_ASSERT_FALSE ( TEST_DEBUGGER_CPUHIST_ACTIVE );
}


void test_mem_w_bp_does_not_fire_on_exec ( void ) {
    int id = breakpoints_add ( 0x4000, "T", -1 );  /* PC_EXEC -> bpmap[0x4000] */
    breakpoints_set_type ( id, BPT_TYPE_MEM_W );    /* resync -> bpmap cleared */
    breakpoints_set_addr ( id, 0x4000 );            /* nesmí znovu zapsat bpmap[] */

    /* Exec na 0x4000 NESMÍ spustit MEM_W BP. */
    g_emulator.paused = false;
    breakpoints_enforce_pc_exec ( 0x4000 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Ale zápis na 0x4000 spustit MUSÍ (BP funguje). */
    g_emulator.paused = false;
    breakpoints_enforce_mem_w ( 0x4000, 0x11 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


void test_mem_w_single_point ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_action ( id, "$last_v = Value" );

    breakpoints_enforce_mem_w ( 0x4000, 0xAB );
    TEST_ASSERT_FALSE ( g_emulator.paused );  /* action = continue */
    TEST_ASSERT_EQUAL_INT32 ( 0xAB, bp_var_get ( "last_v" ) );

    /* Mimo addr - no fire. */
    bp_vars_clear_storage ( );
    breakpoints_enforce_mem_w ( 0x4001, 0x55 );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "last_v" ) );
}


/**
 * MEM_R range match (addr <= hit <= addr_end).
 *
 * V1.5.E - RANGE musí být explicit přes set_addr_match_mode (default
 * je SINGLE). set_addr_range jen nastaví addr+addr_end fieldy, ne mode.
 */
void test_mem_r_range ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_R, 0x4000 );
    breakpoints_set_addr_range ( id, 0x4000, 0x40FF );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_action ( id, "$hits += 1" );

    breakpoints_enforce_mem_r ( 0x4000, 0x00 );
    breakpoints_enforce_mem_r ( 0x4080, 0x00 );
    breakpoints_enforce_mem_r ( 0x40FF, 0x00 );
    breakpoints_enforce_mem_r ( 0x4100, 0x00 );  /* mimo */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "hits" ) );
}


/**
 * Více MEM_R BP - každý se vyhodnotí samostatně.
 */
void test_mem_r_multi_bp ( void ) {
    int b1 = breakpoints_add ( 0x4000, "B1", -1 );
    breakpoints_set_type ( b1, BPT_TYPE_MEM_R );
    breakpoints_set_action ( b1, "$a += 1" );

    int b2 = breakpoints_add ( 0x4000, "B2", -1 );
    breakpoints_set_type ( b2, BPT_TYPE_MEM_R );
    breakpoints_set_action ( b2, "$b += 1" );

    breakpoints_enforce_mem_r ( 0x4000, 0x00 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "a" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "b" ) );
}


/* ========================================================================= */
/*  IORQ_R / IORQ_W                                                          */
/* ========================================================================= */


/**
 * IORQ_W match na low-byte port.
 */
void test_iorq_w_low_byte_match ( void ) {
    int id = breakpoints_add ( 0xCE, "IOW", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IORQ_W );
    breakpoints_set_port ( id, 0x00CE );
    breakpoints_set_action ( id, "$cnt += 1" );

    /* Match low byte = 0xCE; high byte (= B reg) ignored. */
    breakpoints_enforce_iorq_w ( 0x12CE, 0x42 );
    breakpoints_enforce_iorq_w ( 0x00CE, 0x10 );
    breakpoints_enforce_iorq_w ( 0x00CF, 0x99 );  /* miss */

    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
}


/**
 * IORQ_R + condition Value == X.
 */
void test_iorq_r_condition ( void ) {
    int id = breakpoints_add ( 0xE0, "IOR", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IORQ_R );
    breakpoints_set_port ( id, 0xE0 );
    breakpoints_set_expr ( id, "Value == 0xFF" );

    breakpoints_enforce_iorq_r ( 0xE0, 0x00 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    breakpoints_enforce_iorq_r ( 0xE0, 0xFF );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


/* ========================================================================= */
/*  IRQ                                                                      */
/* ========================================================================= */


/**
 * IRQ enforce vždy match když im_mode je v enabled set
 * (V1.5.A8: 5-arg signature - raised, im_mode, vector, isr, int_vector_byte).
 */
void test_irq_fires ( void ) {
    int id = breakpoints_add ( 0x0000, "IRQ", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ );
    /* Default - všechny IM povoleny (bez filtrů). */

    /* IM 1, raised=1, žádný IM 2 ctx. */
    breakpoints_enforce_irq ( 0x01, 1, 0, 0x0038, 0xFF );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( id, s_mock_state.last_id );
}


/* ========================================================================= */
/*  GLOBAL                                                                   */
/* ========================================================================= */


/**
 * GLOBAL bez expr = vždy fire.
 */
void test_global_unconditional ( void ) {
    int id = breakpoints_add ( 0x0000, "G", -1 );
    breakpoints_set_type ( id, BPT_TYPE_GLOBAL );
    breakpoints_set_action ( id, "$g += 1" );

    breakpoints_enforce_global ( );
    breakpoints_enforce_global ( );
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "g" ) );
    (void) id;
}


/**
 * Háček brány GLOBAL (zastavení na čase) drží per_type_active[GLOBAL]
 * i po odebrání posledního GLOBAL BP a po breakpoints_clear_all().
 */
void test_global_hook_keeps_gate ( void ) {
    bptmap_set_global_hook ( true );
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
    TEST_ASSERT_TRUE ( g_bptmap.any_active );
    int id = make_bp ( BPT_TYPE_GLOBAL, 0x0000 );
    breakpoints_remove ( id );
    TEST_ASSERT_TRUE_MESSAGE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ],
                               "odebrání posledního GLOBAL BP bránu s háčkem nevypne" );
    breakpoints_clear_all ( );
    TEST_ASSERT_TRUE_MESSAGE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ],
                               "clear_all bránu s háčkem nevypne" );
    bptmap_set_global_hook ( false );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
    id = make_bp ( BPT_TYPE_GLOBAL, 0x0000 );
    bptmap_set_global_hook ( true );
    bptmap_set_global_hook ( false );
    TEST_ASSERT_TRUE_MESSAGE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ],
                               "vypnutí háčku nechá bránu uživatelskému GLOBAL BP" );
    breakpoints_remove ( id );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
}


/**
 * Zastavení na čase: před cílem enforce_global nezastaví, na cíli pauza
 * s důvodem RUN_UNTIL_TICKS a zrušené zastavení i háček brány.
 */
void test_stop_at_ticks ( void ) {
    uint64_t now = gdg_get_total_ticks ( );
    run_control_stop_at_arm ( now + 100, EMU_PAUSE_REASON_RUN_UNTIL_TICKS );
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
    breakpoints_enforce_global ( );
    TEST_ASSERT_FALSE_MESSAGE ( g_emulator.paused, "před cílem běží dál" );
    g_gdg.total_elapsed.ticks += 100;
    breakpoints_enforce_global ( );
    TEST_ASSERT_TRUE_MESSAGE ( g_emulator.paused, "na cíli pauza" );
    TEST_ASSERT_EQUAL_INT ( EMU_PAUSE_REASON_RUN_UNTIL_TICKS, g_emulator.pause_reason );
    TEST_ASSERT_EQUAL_UINT ( 0, g_debugger.stop_at_active );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
    emulator_pause ( false );
}


/**
 * Pauza z jiného důvodu (uživatel, breakpoint) zastavení na čase zruší.
 */
void test_stop_at_cleared_by_pause ( void ) {
    run_control_stop_at_arm ( gdg_get_total_ticks ( ) + 1000, EMU_PAUSE_REASON_RUN_UNTIL_TICKS );
    emulator_pause ( true );
    TEST_ASSERT_EQUAL_UINT ( 0, g_debugger.stop_at_active );
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[ BPTMAP_IDX_GLOBAL ] );
    emulator_pause ( false );
}


/**
 * GLOBAL edge_triggered - fire jen na přechod 0->1 condition.
 */
void test_global_edge_triggered ( void ) {
    int id = breakpoints_add ( 0x0000, "G", -1 );
    breakpoints_set_type ( id, BPT_TYPE_GLOBAL );
    breakpoints_set_edge_triggered ( id, true );
    breakpoints_set_expr ( id, "$gate" );  /* fire pokud $gate != 0 */
    breakpoints_set_action ( id, "$cnt += 1" );

    /* $gate = 0 → no fire. */
    bp_var_set ( "gate", 0 );
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "cnt" ) );

    /* $gate = 1 → edge 0->1 → fire. */
    bp_var_set ( "gate", 1 );
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* $gate stále 1 → edge ne, žádný fire. */
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* $gate = 0 → reset edge. */
    bp_var_set ( "gate", 0 );
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* $gate = 1 znovu → další edge → fire. */
    bp_var_set ( "gate", 1 );
    breakpoints_enforce_global ( );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );

    (void) id;
}


/* ========================================================================= */
/*  Disabled BP                                                              */
/* ========================================================================= */


/**
 * Disabled BP nefiruje ani v hot path.
 */
void test_disabled_bp_skipped ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_enabled ( id, false );

    breakpoints_enforce_mem_w ( 0x4000, 0x00 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 0, s_mock_state.call_count );

    /* Per_type_active je false → hot loop by ani nedispatchnul. */
    TEST_ASSERT_FALSE ( g_bptmap.per_type_active[BPTMAP_IDX_MEM_W] );
}


/* ========================================================================= */
/*  V1.5.E - Match modes (PC_EXEC RANGE / MEM RANGE+MASK / IORQ RANGE+MASK)  */
/* ========================================================================= */


/**
 * PC_EXEC RANGE - sync_bptmap registruje všechny addr v rozsahu do bpmap[].
 * Po set_addr_match_mode(RANGE) musí test explicit zavolat sync_bptmap()
 * (= settery match_mode + addr_end nesync, intent: batch update).
 */
void test_pc_exec_range_hit ( void ) {
    int id = breakpoints_add ( 0x4000, "PCR", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_addr_range ( id, 0x4000, 0x40FF );
    breakpoints_sync_bptmap ( );  /* re-register celý range do bpmap[] */

    breakpoints_enforce_pc_exec ( 0x4000 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_pc_exec ( 0x4080 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_pc_exec ( 0x40FF );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


void test_pc_exec_range_miss ( void ) {
    int id = breakpoints_add ( 0x4000, "PCR", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_addr_range ( id, 0x4000, 0x40FF );
    breakpoints_sync_bptmap ( );

    breakpoints_enforce_pc_exec ( 0x3FFF );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    breakpoints_enforce_pc_exec ( 0x4100 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * PC_EXEC MASK V1.6+ TODO 4.3: plny support pres
 * BPTMAP_IDX_PC_EXEC_NONSINGLE per-type list (= sparse mask nelze
 * enumerovat v O(1) bpmap[], iteruje per-instruction).
 *
 * V1.5.E mela degraded fallback (= jen ref addr fire); V1.6+ MASK
 * fire na vsech matching addrs.
 *
 * Test enforce path: enforce_pc_exec (= bpmap[] cesta) sama o sobe NE-
 * fire (= MASK BP neni v bpmap), enforce_pc_exec_nonsingle volana
 * z hot loopu sekundarne fire dle bp_match16.
 */
void test_pc_exec_mask_full_support ( void ) {
    int id = breakpoints_add ( 0x4123, "PCM", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_MASK );
    breakpoints_set_addr_mask ( id, 0xFF00 );  /* match 0x41** */
    breakpoints_sync_bptmap ( );

    /* MASK BP neni registrovan v bpmap[] (= sparse, nelze enumerate).
     * Ref addr 0x4123 - MASK match (0x4123 & 0xFF00 == 0x4100 == 0x4123 & 0xFF00). */
    breakpoints_enforce_pc_exec_nonsingle ( 0x4123 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Jina addr v rozsahu masky (= 0x41xx). */
    g_emulator.paused = false;
    breakpoints_enforce_pc_exec_nonsingle ( 0x4100 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_pc_exec_nonsingle ( 0x41FF );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Mimo masku (high byte != 0x41). */
    g_emulator.paused = false;
    breakpoints_enforce_pc_exec_nonsingle ( 0x4099 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_pc_exec_nonsingle ( 0x4200 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


void test_mem_w_range ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_addr_range ( id, 0x4000, 0x40FF );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_mem_w ( 0x4000, 0x00 );
    breakpoints_enforce_mem_w ( 0x4080, 0x00 );
    breakpoints_enforce_mem_w ( 0x40FF, 0x00 );
    breakpoints_enforce_mem_w ( 0x4100, 0x00 );  /* miss */
    breakpoints_enforce_mem_w ( 0x3FFF, 0x00 );  /* miss */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
}


void test_mem_w_mask ( void ) {
    /* Mask 0xFF00 - match všech 0x40** addr. */
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_MASK );
    breakpoints_set_addr_mask ( id, 0xFF00 );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_mem_w ( 0x4000, 0x00 );
    breakpoints_enforce_mem_w ( 0x4042, 0x00 );
    breakpoints_enforce_mem_w ( 0x40FF, 0x00 );
    breakpoints_enforce_mem_w ( 0x5000, 0x00 );  /* miss - jiný high byte */
    breakpoints_enforce_mem_w ( 0x4100, 0x00 );  /* miss - 0x41xx */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
}


void test_mem_r_mask ( void ) {
    /* Mask 0x00FF - match všech *XX kde XX = 0x42 (low byte). */
    int id = make_bp ( BPT_TYPE_MEM_R, 0x4042 );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_MASK );
    breakpoints_set_addr_mask ( id, 0x00FF );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_mem_r ( 0x4042, 0x00 );
    breakpoints_enforce_mem_r ( 0x9942, 0x00 );  /* match low */
    breakpoints_enforce_mem_r ( 0x0042, 0x00 );  /* match low */
    breakpoints_enforce_mem_r ( 0x4043, 0x00 );  /* miss */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
}


/* ========================================================================= */
/*  V1.5.A7 - IORQ port mode 8/16-bit                                        */
/* ========================================================================= */


/**
 * Default 8-bit mode - low byte match, high byte ignored.
 */
void test_iorq_8bit_ignores_high_byte ( void ) {
    int id = breakpoints_add ( 0xCE, "IO8", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IORQ_W );
    breakpoints_set_port ( id, 0xCE );
    breakpoints_set_port_mode ( id, BP_PORT_8BIT );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_iorq_w ( 0x00CE, 0x00 );
    breakpoints_enforce_iorq_w ( 0x12CE, 0x00 );  /* high byte ignored */
    breakpoints_enforce_iorq_w ( 0xFFCE, 0x00 );
    breakpoints_enforce_iorq_w ( 0x00CF, 0x00 );  /* miss */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
}


/**
 * 16-bit mode - full BC match.
 */
void test_iorq_16bit_full_bc_match ( void ) {
    int id = breakpoints_add ( 0xFFCE, "IO16", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IORQ_W );
    breakpoints_set_port ( id, 0xFFCE );
    breakpoints_set_port_mode ( id, BP_PORT_16BIT );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_iorq_w ( 0xFFCE, 0x00 );  /* full match */
    breakpoints_enforce_iorq_w ( 0x00CE, 0x00 );  /* miss - high byte */
    breakpoints_enforce_iorq_w ( 0xFFCF, 0x00 );  /* miss - low byte */

    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
}


/**
 * IORQ_R + RANGE mode na low byte (8-bit).
 */
void test_iorq_8bit_range ( void ) {
    int id = breakpoints_add ( 0xC0, "IOR", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IORQ_R );
    breakpoints_set_port ( id, 0xC0 );
    breakpoints_set_port_end ( id, 0xCF );
    breakpoints_set_port_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_port_mode ( id, BP_PORT_8BIT );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_iorq_r ( 0xC0, 0x00 );
    breakpoints_enforce_iorq_r ( 0xC8, 0x00 );
    breakpoints_enforce_iorq_r ( 0xCF, 0x00 );
    breakpoints_enforce_iorq_r ( 0xD0, 0x00 );  /* miss */
    breakpoints_enforce_iorq_r ( 0xBF, 0x00 );  /* miss */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
}


/* ========================================================================= */
/*  V1.5.A8 + A8.5 - IRQ filter (IM mode + RST + vector + ISR)               */
/* ========================================================================= */


/**
 * Helper: nastav defaultní IRQ BP s podporou všech IM, žádné filtry.
 */
static int make_irq_bp_no_filter ( void ) {
    int id = breakpoints_add ( 0x0000, "IRQ", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ );
    breakpoints_set_im_enabled ( id, 0, true );
    breakpoints_set_im_enabled ( id, 1, true );
    breakpoints_set_im_enabled ( id, 2, true );
    return id;
}


/**
 * IM mode discriminator - jen IM 1 enabled.
 */
void test_irq_im1_only_fires_in_im1 ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 2, false );

    /* IM 0 - no fire. */
    breakpoints_enforce_irq ( 0x01, 0, 0, 0, 0xC7 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* IM 1 - fire. */
    breakpoints_enforce_irq ( 0x01, 1, 0, 0x0038, 0 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* IM 2 - reset paused, no fire. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM 0 RST mask - jen RST 08 (0xCF) match.
 */
void test_irq_im0_rst_mask_subset ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im_enabled ( id, 2, false );
    /* RST 08 = bit 1 (RST 00=bit 0, 08=1, 10=2, 18=3, 20=4, 28=5, 30=6, 38=7). */
    breakpoints_set_im0_rst_mask ( id, 1 << 1 );

    /* RST 00 (0xC7) - mask doesn't include - no fire. */
    breakpoints_enforce_irq ( 0x01, 0, 0, 0x0000, 0xC7 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* RST 08 (0xCF) - match. */
    breakpoints_enforce_irq ( 0x01, 0, 0, 0x0008, 0xCF );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* RST 38 (0xFF) - no match. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 0, 0, 0x0038, 0xFF );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM 0 RST mask = 0 (= match-all RST opcodů).
 */
void test_irq_im0_rst_mask_zero_matches_all ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im_enabled ( id, 2, false );
    breakpoints_set_im0_rst_mask ( id, 0 );  /* match-all */

    breakpoints_enforce_irq ( 0x01, 0, 0, 0x0000, 0xC7 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 0, 0, 0x0038, 0xFF );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


/**
 * IM 2 vector filter - exact match (low bit ignored).
 */
void test_irq_im2_vector_match ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_vector_filter ( id, true, 0xFE10 );

    /* Match. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Low bit ignored - vector_addr & 0xFE. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE11, 0x1234, 0x11 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Different vector - no match. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE20, 0x5678, 0x20 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM 2 ISR filter - exact PC match po dispatchi.
 */
void test_irq_im2_isr_match ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_isr_filter ( id, true, 0x1234 );

    /* Match ISR addr. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Jiná ISR addr - no match. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x5678, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM 2 vector + ISR oba enabled - musí matchovat oba.
 */
void test_irq_im2_vector_and_isr_combined ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_vector_filter ( id, true, 0xFE10 );
    breakpoints_set_im2_isr_filter   ( id, true, 0x1234 );

    /* Oba match. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Jen vector match, ISR ne. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x9999, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Jen ISR match, vector ne. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE20, 0x1234, 0x20 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/* ========================================================================= */
/*  V1.6+ TODO 4.4 - IM2 vector / ISR Match modes (RANGE / MASK)             */
/* ========================================================================= */


/**
 * IM2 vector RANGE mode - fire pro vsechny vector_addr v intervalu.
 * Vector_addr je AND-ovan s 0xFFFE pred porovnanim (= bit 0 ignorovan).
 */
void test_irq_im2_vector_range_mode ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_vector_filter ( id, true, 0xFE10 );
    breakpoints_set_im2_vector_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_im2_vector_addr_end ( id, 0xFE20 );

    /* Vector na dolnim okraji. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Vector uvnitr intervalu. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE18, 0x1234, 0x18 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Vector na hornim okraji. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE20, 0x1234, 0x20 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Vector mimo - nizsi. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE08, 0x1234, 0x08 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Vector mimo - vyssi. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE30, 0x1234, 0x30 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM2 vector MASK mode - fire jen pro vector_addr s match-bits dle masky.
 * Maska 0xFF00 = high byte musi byt 0xFE.
 */
void test_irq_im2_vector_mask_mode ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_vector_filter ( id, true, 0xFE00 );
    breakpoints_set_im2_vector_match_mode ( id, BP_MATCH_MASK );
    breakpoints_set_im2_vector_mask ( id, 0xFF00 );

    /* High byte = 0xFE (match). */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFEFE, 0x1234, 0xFE );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* High byte != 0xFE (no match). */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFD10, 0x1234, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM2 ISR RANGE mode - fire pro vsechny isr_addr v intervalu.
 */
void test_irq_im2_isr_range_mode ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_isr_filter ( id, true, 0x1000 );
    breakpoints_set_im2_isr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_im2_isr_addr_end ( id, 0x1FFF );

    /* ISR uvnitr intervalu. */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1500, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* ISR na hranicich. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1000, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1FFF, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Mimo. */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x2000, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * IM2 ISR MASK mode - fire pro isr_addr matching maska bits.
 * Maska 0x000F = low nibble musi byt 0x4 (= ISR endpoints with .4 nibble).
 */
void test_irq_im2_isr_mask_mode ( void ) {
    int id = make_irq_bp_no_filter ( );
    breakpoints_set_im_enabled ( id, 0, false );
    breakpoints_set_im_enabled ( id, 1, false );
    breakpoints_set_im2_isr_filter ( id, true, 0x0004 );
    breakpoints_set_im2_isr_match_mode ( id, BP_MATCH_MASK );
    breakpoints_set_im2_isr_mask ( id, 0x000F );

    /* Low nibble = 4 (match). */
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1234, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0xABC4, 0x10 );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Low nibble != 4 (no match). */
    g_emulator.paused = false;
    breakpoints_enforce_irq ( 0x01, 2, 0xFE10, 0x1235, 0x10 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/* ========================================================================= */
/*  V1.5.A8.5 - IRQ_SIG pre-dispatch                                         */
/* ========================================================================= */


#include "mzarch/interrupt.h"  /* MZARCH_INTERRUPT_CTC2 / FDC / PIOZ80 */


/**
 * IRQ_SIG fires na rising edge CTC2 sourcu.
 */
void test_irq_sig_ctc2_rising ( void ) {
    int id = breakpoints_add ( 0x0000, "SIG", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ_SIG );
    breakpoints_set_irq_sig_source_mask ( id, BP_IRQ_SIG_CTC2 );
    breakpoints_set_action ( id, "$sigs += 1" );

    /* Rising: prev=0, now=CTC2. */
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_CTC2, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "sigs" ) );

    /* Sustained = no edge, no fire. */
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_CTC2, MZARCH_INTERRUPT_CTC2 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "sigs" ) );

    /* Falling = no fire. */
    breakpoints_enforce_irq_sig ( 0, MZARCH_INTERRUPT_CTC2 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "sigs" ) );

    /* Re-rise = fire. */
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_CTC2, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "sigs" ) );
    (void) id;
}


/**
 * IRQ_SIG fires na FDC source.
 */
void test_irq_sig_fdc_rising ( void ) {
    int id = breakpoints_add ( 0x0000, "SIG", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ_SIG );
    breakpoints_set_irq_sig_source_mask ( id, BP_IRQ_SIG_FDC );
    breakpoints_set_action ( id, "$sigs += 1" );

    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_FDC, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "sigs" ) );
    (void) id;
}


/**
 * Source mask filtruje nematchující sources.
 */
void test_irq_sig_source_mask_filters ( void ) {
    int id = breakpoints_add ( 0x0000, "SIG", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ_SIG );
    breakpoints_set_irq_sig_source_mask ( id, BP_IRQ_SIG_CTC2 );  /* jen CTC2 */
    breakpoints_set_action ( id, "$sigs += 1" );

    /* FDC edge - no match. */
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_FDC, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "sigs" ) );

    /* CTC2 edge - match. */
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_CTC2, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "sigs" ) );
    (void) id;
}


/**
 * No edge (raised_now == raised_prev) = no fire.
 */
void test_irq_sig_no_edge_no_fire ( void ) {
    int id = breakpoints_add ( 0x0000, "SIG", -1 );
    breakpoints_set_type ( id, BPT_TYPE_IRQ_SIG );
    breakpoints_set_irq_sig_source_mask ( id, 0xFF );  /* match all */
    breakpoints_set_action ( id, "$sigs += 1" );

    breakpoints_enforce_irq_sig ( 0, 0 );
    breakpoints_enforce_irq_sig ( MZARCH_INTERRUPT_CTC2, MZARCH_INTERRUPT_CTC2 );

    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "sigs" ) );
    (void) id;
}


/* ========================================================================= */
/*  V1.5.E - SP_THRESHOLD WINDOW mode                                        */
/* ========================================================================= */


/**
 * WINDOW: fires při EXIT z okna (= "stack alarm" use case).
 *
 * Sémantika: SP byl uvnitř <sp_threshold..sp_upper>, teď je venku.
 * Edge-triggered = nezpamuje pri sustained outside.
 */
void test_sp_window_exit ( void ) {
    int id = breakpoints_add ( 0x0000, "SPW", -1 );
    breakpoints_set_type ( id, BPT_TYPE_SP_THRESHOLD );
    breakpoints_set_sp_mode ( id, BP_SP_WINDOW );
    breakpoints_set_sp_threshold ( id, 0xF000 );  /* lower bound */
    breakpoints_set_sp_upper ( id, 0xF1FF );      /* upper bound */
    breakpoints_set_action ( id, "$exits += 1" );

    /* SP byl uvnitř (0xF180), teď je venku (0xF200) → exit fire. */
    breakpoints_enforce_sp_threshold ( 0xF180, 0xF200 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "exits" ) );
    (void) id;
}


/**
 * WINDOW: SP zůstane uvnitř → no fire (= ani enter, ani inside-move).
 */
void test_sp_window_inside_no_fire ( void ) {
    int id = breakpoints_add ( 0x0000, "SPW", -1 );
    breakpoints_set_type ( id, BPT_TYPE_SP_THRESHOLD );
    breakpoints_set_sp_mode ( id, BP_SP_WINDOW );
    breakpoints_set_sp_threshold ( id, 0xF000 );
    breakpoints_set_sp_upper ( id, 0xF1FF );
    breakpoints_set_action ( id, "$exits += 1" );

    /* Pohyb uvnitř okna - žádný fire. */
    breakpoints_enforce_sp_threshold ( 0xF180, 0xF100 );
    breakpoints_enforce_sp_threshold ( 0xF100, 0xF080 );
    breakpoints_enforce_sp_threshold ( 0xF080, 0xF1F0 );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "exits" ) );

    /* Vstup zvenčí (was_outside, is_inside) - taky no fire (jen exit). */
    breakpoints_enforce_sp_threshold ( 0xF200, 0xF180 );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "exits" ) );
    (void) id;
}


/**
 * WINDOW: sustained outside → no retrigger.
 */
void test_sp_window_sustained_outside_no_retrigger ( void ) {
    int id = breakpoints_add ( 0x0000, "SPW", -1 );
    breakpoints_set_type ( id, BPT_TYPE_SP_THRESHOLD );
    breakpoints_set_sp_mode ( id, BP_SP_WINDOW );
    breakpoints_set_sp_threshold ( id, 0xF000 );
    breakpoints_set_sp_upper ( id, 0xF1FF );
    breakpoints_set_action ( id, "$exits += 1" );

    /* Exit edge - fire. */
    breakpoints_enforce_sp_threshold ( 0xF180, 0xF200 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "exits" ) );

    /* Sustained outside - no retrigger. */
    breakpoints_enforce_sp_threshold ( 0xF200, 0xF300 );
    breakpoints_enforce_sp_threshold ( 0xF300, 0xE000 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "exits" ) );
    (void) id;
}


/**
 * Legacy SP_SINGLE: edge-crossing dolů.
 */
void test_sp_single_edge_crossing ( void ) {
    int id = breakpoints_add ( 0x0000, "SP1", -1 );
    breakpoints_set_type ( id, BPT_TYPE_SP_THRESHOLD );
    breakpoints_set_sp_mode ( id, BP_SP_SINGLE );
    breakpoints_set_sp_threshold ( id, 0xF000 );
    breakpoints_set_action ( id, "$crosses += 1" );

    /* Přechod F100 -> EFFF (= z >= threshold do < threshold). */
    breakpoints_enforce_sp_threshold ( 0xF100, 0xEFFF );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "crosses" ) );

    /* Sustained pod threshold - no retrigger. */
    breakpoints_enforce_sp_threshold ( 0xEFFF, 0xEFEF );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "crosses" ) );

    /* Návrat nahoru (no fire) + opětovný edge dolů. */
    breakpoints_enforce_sp_threshold ( 0xEFEF, 0xF100 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "crosses" ) );
    breakpoints_enforce_sp_threshold ( 0xF100, 0xEFFF );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "crosses" ) );
    (void) id;
}


/* ========================================================================= */
/*  Condition na Value (= MEM_W value filter via expression)                 */
/* ========================================================================= */


/**
 * MEM_W + condition Value == 0xFF - filter.
 */
void test_mem_w_value_condition_match ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_expr ( id, "Value == 0xFF" );
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_mem_w ( 0x4000, 0x00 );
    breakpoints_enforce_mem_w ( 0x4000, 0xFF );
    breakpoints_enforce_mem_w ( 0x4000, 0xAB );

    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * Match mode + value condition kombinace.
 */
void test_mem_w_range_with_value_filter ( void ) {
    int id = make_bp ( BPT_TYPE_MEM_W, 0x4000 );
    breakpoints_set_addr_match_mode ( id, BP_MATCH_RANGE );
    breakpoints_set_addr_range ( id, 0x4000, 0x40FF );
    breakpoints_set_expr ( id, "Value & 0x80" );  /* high bit set */
    breakpoints_set_action ( id, "$cnt += 1" );

    breakpoints_enforce_mem_w ( 0x4042, 0x80 );  /* in range, high bit */
    breakpoints_enforce_mem_w ( 0x4099, 0x7F );  /* in range, no high bit */
    breakpoints_enforce_mem_w ( 0x4000, 0xFF );  /* in range, high bit */
    breakpoints_enforce_mem_w ( 0x5000, 0xFF );  /* mimo range */

    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  0019 KUS 1 - Vrstva 1: control-plane drain na hranici BP akce            */
/* ========================================================================= */


/* g_dbgapi_cmdrq_queue + dbgapi_init/destroy + dequeue/dispatch/complete jsou
 * deklarovány v debugger/dbgapi_emu.h (už includováno výše). */


/**
 * @brief White-box helper: vloží řídicí příkaz do CMDRQ fronty bez čekání.
 *
 * Replikuje enqueue část UI submitu (dbgapi.c), ale BEZ blokujícího čekání
 * na odpověď (= v single-threaded testu nikdo frontu nedrainuje, sync submit
 * by deadlockoval / vytekl timeoutem a slot vyčistil). Vloží PENDING slot na
 * pozici tail a posune tail; drain ho pak dequeue/dispatch/complete.
 *
 * @param cmd Příkaz (en_DBGAPI_CMD).
 */
static void test_enqueue_cmd_no_wait ( en_DBGAPI_CMD cmd ) {
    int next_tail = ( g_dbgapi_cmdrq_queue.tail + 1 ) % DBGAPI_CMDRQ_QUEUE_SIZE;
    st_DBGAPI_CMDRQ *slot = &g_dbgapi_cmdrq_queue.cmdrq[ g_dbgapi_cmdrq_queue.tail ];
    g_dbgapi_cmdrq_queue.tail = next_tail;
    slot->cmd = cmd;
    slot->cmd_origin = DBGAPI_CMD_ORIGIN_TEST;
    slot->cmd_state = DBGAPI_CMDSTATE_PENDING;
    slot->data_ptr = NULL;
    slot->result_ptr = NULL;
    slot->success = false;
}


/**
 * Drain obslouží pending PAUSE i bez per-frame bodu (mzarch.c) - holý drain.
 */
void test_drain_handles_pending_pause ( void ) {
    dbgapi_init ( &g_dbgapi_cmdrq_queue );

    g_emulator.paused = false;
    test_enqueue_cmd_no_wait ( DBGAPI_CMD_PAUSE );

    /* Fronta má pending příkaz. */
    TEST_ASSERT_TRUE ( dbgapi_emu_has_pending ( &g_dbgapi_cmdrq_queue ) );

    breakpoints_drain_control_plane_at_bp_boundary ( );

    /* PAUSE se obsloužila -> emu je v pauze, fronta prázdná. */
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_FALSE ( dbgapi_emu_has_pending ( &g_dbgapi_cmdrq_queue ) );

    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/**
 * Pending PAUSE submitnutý během běžící BP akce (= continue) se obslouží
 * na hranici akce, NE až per-frame. Tím je control-plane responzivní i na
 * horké BP smyčce (akceptační kritérium 0019 vrstva 1).
 */
void test_pending_pause_handled_at_bp_action_boundary ( void ) {
    dbgapi_init ( &g_dbgapi_cmdrq_queue );

    g_emulator.paused = false;

    /* BP s akcí, která vrací continue (= $x += 1, neblokuje). */
    int id = make_bp ( BPT_TYPE_PC_EXEC, 0x1234 );
    breakpoints_set_action ( id, "$x += 1" );

    /* Simulace: control-plane PAUSE dorazila do fronty během akce (= UI/MCP
     * klik na horké smyčce). Drain v enforce ji obslouží po dokončení akce. */
    test_enqueue_cmd_no_wait ( DBGAPI_CMD_PAUSE );

    breakpoints_enforce_pc_exec ( 0x1234 );

    /* Akce proběhla (continue) A pending PAUSE byla obsloužena na hranici. */
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "x" ) );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_FALSE ( dbgapi_emu_has_pending ( &g_dbgapi_cmdrq_queue ) );

    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/**
 * Prázdná fronta = drain je no-op (žádná změna paused).
 */
void test_drain_empty_queue_is_noop ( void ) {
    dbgapi_init ( &g_dbgapi_cmdrq_queue );

    g_emulator.paused = false;
    TEST_ASSERT_FALSE ( dbgapi_emu_has_pending ( &g_dbgapi_cmdrq_queue ) );

    breakpoints_drain_control_plane_at_bp_boundary ( );

    TEST_ASSERT_FALSE ( g_emulator.paused );

    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* bptmap V2 */
    RUN_TEST ( test_bptmap_register_activates_flags );
    RUN_TEST ( test_bptmap_set_type_resyncs_lists );

    /* PC_EXEC */
    RUN_TEST ( test_pc_exec_basic_stop );
    RUN_TEST ( test_pc_exec_no_match );
    RUN_TEST ( test_skip_count );
    RUN_TEST ( test_hit_count );

    /* Condition + Action */
    RUN_TEST ( test_condition_match );
    RUN_TEST ( test_condition_false_no_fire );
    RUN_TEST ( test_action_continue );
    RUN_TEST ( test_action_disable_self );

    /* Fix C - callback gating */
    RUN_TEST ( test_need_callbacks_includes_mem_iorq_bp );

    /* CPU history gating */
    RUN_TEST ( test_cpuhist_active_when_enabled_bp );

    /* MEM_R/W */
    RUN_TEST ( test_mem_w_bp_does_not_fire_on_exec );
    RUN_TEST ( test_mem_w_single_point );
    RUN_TEST ( test_enforce_mmext_bank_offset );
    RUN_TEST ( test_mem_r_range );
    RUN_TEST ( test_mem_r_multi_bp );

    /* IORQ */
    RUN_TEST ( test_iorq_w_low_byte_match );
    RUN_TEST ( test_iorq_r_condition );

    /* IRQ */
    RUN_TEST ( test_irq_fires );

    /* GLOBAL */
    RUN_TEST ( test_global_unconditional );
    RUN_TEST ( test_global_edge_triggered );

    /* Disabled */
    RUN_TEST ( test_disabled_bp_skipped );

    /* V1.5.E - Match modes */
    RUN_TEST ( test_pc_exec_range_hit );
    RUN_TEST ( test_pc_exec_range_miss );
    RUN_TEST ( test_pc_exec_mask_full_support );
    RUN_TEST ( test_mem_w_range );
    RUN_TEST ( test_mem_w_mask );
    RUN_TEST ( test_mem_r_mask );

    /* V1.5.A7 - IORQ port mode */
    RUN_TEST ( test_iorq_8bit_ignores_high_byte );
    RUN_TEST ( test_iorq_16bit_full_bc_match );
    RUN_TEST ( test_iorq_8bit_range );

    /* V1.5.A8 + A8.5 - IRQ filter */
    RUN_TEST ( test_irq_im1_only_fires_in_im1 );
    RUN_TEST ( test_irq_im0_rst_mask_subset );
    RUN_TEST ( test_irq_im0_rst_mask_zero_matches_all );
    RUN_TEST ( test_irq_im2_vector_match );
    RUN_TEST ( test_irq_im2_isr_match );
    RUN_TEST ( test_irq_im2_vector_and_isr_combined );

    /* V1.6+ TODO 4.4: IM2 vector / ISR Match modes (RANGE / MASK) */
    RUN_TEST ( test_irq_im2_vector_range_mode );
    RUN_TEST ( test_irq_im2_vector_mask_mode );
    RUN_TEST ( test_irq_im2_isr_range_mode );
    RUN_TEST ( test_irq_im2_isr_mask_mode );

    /* V1.5.A8.5 - IRQ_SIG */
    RUN_TEST ( test_irq_sig_ctc2_rising );
    RUN_TEST ( test_irq_sig_fdc_rising );
    RUN_TEST ( test_irq_sig_source_mask_filters );
    RUN_TEST ( test_irq_sig_no_edge_no_fire );

    /* V1.5.E - SP_THRESHOLD */
    RUN_TEST ( test_sp_window_exit );
    RUN_TEST ( test_sp_window_inside_no_fire );
    RUN_TEST ( test_sp_window_sustained_outside_no_retrigger );
    RUN_TEST ( test_sp_single_edge_crossing );

    /* MEM_W value filter */
    RUN_TEST ( test_mem_w_value_condition_match );
    RUN_TEST ( test_mem_w_range_with_value_filter );

    /* 0019 KUS 1 - Vrstva 1: control-plane drain na hranici BP akce */
    RUN_TEST ( test_drain_handles_pending_pause );
    RUN_TEST ( test_pending_pause_handled_at_bp_action_boundary );
    RUN_TEST ( test_drain_empty_queue_is_noop );
    RUN_TEST ( test_global_hook_keeps_gate );
    RUN_TEST ( test_stop_at_ticks );
    RUN_TEST ( test_stop_at_cleared_by_pause );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
