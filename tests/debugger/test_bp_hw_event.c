/*
 * test_bp_hw_event.c - testy HW event vocabulary + dispatch (D.3)
 *
 * Pokrytí:
 *   - bp_event_to_string / bp_event_from_string konverze
 *   - bp_event_from_string parametrizovaný formát "raster:N"
 *   - bp_event_fire -> breakpoints_enforce_hw_event dispatch
 *   - parsed_event cache po set_event_name
 *   - g_bp_event_active bitmap aktualizace
 *   - condition + action s HW eventem
 *   - raster:N parametrizovaný match
 *   - persist event_name přes save/load round-trip
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "debugger/bp_event.h"
#include "debugger/bp_expr.h"
#include "debugger/bp_action.h"
#include "debugger/bp_vars.h"
#include "debugger/debugger.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_msg.h"
#include "emulator/emulator.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>


/* ========================================================================= */
/*  Mock MSG dispatcher                                                       */
/* ========================================================================= */


typedef struct {
    int call_count;
    en_DBGAPI_MSG last_msg;
    int last_id;
    uint16_t last_addr;
} mock_msg_state_t;

static mock_msg_state_t s_mock_state;


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
/*  setUp / tearDown                                                          */
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
/*  Helper                                                                    */
/* ========================================================================= */


static int make_hw_event_bp ( const char *event_name ) {
    int id = breakpoints_add ( 0, "T", -1 );
    breakpoints_set_type ( id, BPT_TYPE_HW_EVENT );
    breakpoints_set_event_name ( id, event_name );
    return id;
}


/* ========================================================================= */
/*  Konverze enum <-> string                                                  */
/* ========================================================================= */


/**
 * vsync = jednoduchý GDG event, párový převod.
 */
void test_event_string_vsync_roundtrip ( void ) {
    TEST_ASSERT_EQUAL_STRING ( "vsync", bp_event_to_string ( BP_EVENT_GDG_VSYNC ) );
    en_BP_EVENT ev = BP_EVENT_NONE;
    int32_t param = 42;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "vsync", &ev, &param ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_VSYNC, ev );
    TEST_ASSERT_EQUAL ( 0, param );
}


/**
 * ctc:zc0 - název s dvojtečkou, ale bez parametru.
 */
void test_event_string_ctc_zc0_with_colon ( void ) {
    en_BP_EVENT ev = BP_EVENT_NONE;
    int32_t param = -1;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "ctc:zc0", &ev, &param ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_CTC_ZC0, ev );
    TEST_ASSERT_EQUAL ( 0, param );
}


/**
 * raster:192 - parametrizovaný event, N do event_param.
 */
void test_event_string_raster_with_param ( void ) {
    en_BP_EVENT ev = BP_EVENT_NONE;
    int32_t param = 0;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "raster:192", &ev, &param ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_RASTER, ev );
    TEST_ASSERT_EQUAL ( 192, param );

    /* Edge cases. */
    TEST_ASSERT_TRUE ( bp_event_from_string ( "raster:0", &ev, &param ) );
    TEST_ASSERT_EQUAL ( 0, param );

    TEST_ASSERT_TRUE ( bp_event_from_string ( "raster:65535", &ev, &param ) );
    TEST_ASSERT_EQUAL ( 65535, param );
}


/**
 * Neznámé jméno = false.
 */
void test_event_string_unknown ( void ) {
    en_BP_EVENT ev = BP_EVENT_NONE;
    TEST_ASSERT_FALSE ( bp_event_from_string ( "fake_event", &ev, NULL ) );
    TEST_ASSERT_FALSE ( bp_event_from_string ( "raster:abc", &ev, NULL ) );
    TEST_ASSERT_FALSE ( bp_event_from_string ( "raster:", &ev, NULL ) );
    TEST_ASSERT_FALSE ( bp_event_from_string ( ":42", &ev, NULL ) );
}


/**
 * Empty string = BP_EVENT_NONE.
 */
void test_event_string_empty ( void ) {
    en_BP_EVENT ev = BP_EVENT_GDG_VSYNC;
    int32_t param = 5;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "", &ev, &param ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_NONE, ev );
    TEST_ASSERT_EQUAL ( 0, param );
}


/* ========================================================================= */
/*  set_event_name -> parsed_event cache + active bitmap                     */
/* ========================================================================= */


/**
 * set_event_name aktualizuje parsed_event a globální active bitmap.
 */
void test_set_event_name_updates_cache_and_bitmap ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_VSYNC, bpt->parsed_event );
    TEST_ASSERT_EQUAL ( 0, bpt->event_param );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );
}


/**
 * set_event_name s "raster:100" naplní event_param.
 */
void test_set_event_name_raster_param ( void ) {
    int id = make_hw_event_bp ( "raster:100" );
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_RASTER, bpt->parsed_event );
    TEST_ASSERT_EQUAL ( 100, bpt->event_param );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_GDG_RASTER ] );
}


/**
 * Disabled BP nepřispívá do active bitmap.
 */
void test_disabled_bp_clears_bitmap ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );
    breakpoints_set_enabled ( id, false );
    /* set_enabled spustí sync_bptmap → recompute_event_active. */
    TEST_ASSERT_FALSE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );
    breakpoints_set_enabled ( id, true );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );
}


/* ========================================================================= */
/*  Dispatch: bp_event_fire -> hit                                            */
/* ========================================================================= */


/**
 * Vsync BP s prázdnou action = stop. Rising edge (0->1) spustí pause + MSG.
 *
 * V1.5 HWE: signal events vyžadují edge - default trigger RISING.
 */
void test_dispatch_vsync_basic_hit ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    /* Rising edge: prev=0 (default state cache), curr=1. */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 1, bpt->hits );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL ( 1, s_mock_state.call_count );
    TEST_ASSERT_EQUAL ( DBGAPI_MSG_BREAKPOINT_HIT, s_mock_state.last_msg );
    TEST_ASSERT_EQUAL ( id, s_mock_state.last_id );
}


/**
 * raster:192 fire na row 191 = no hit, row 192 = hit.
 */
void test_dispatch_raster_param_match ( void ) {
    int id = make_hw_event_bp ( "raster:192" );

    /* Row 191 - no match. */
    bp_event_fire ( BP_EVENT_GDG_RASTER, 191 );
    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 0, bpt->hits );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Row 192 - match. */
    bp_event_fire ( BP_EVENT_GDG_RASTER, 192 );
    bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 1, bpt->hits );
    TEST_ASSERT_TRUE ( g_emulator.paused );
}


/**
 * BP s condition (Frame > 5) + action continue. V V1 Frame = 0 (TODO),
 * tedy condition false → no hit, no pause.
 *
 * Vyhodnocení Frame > 5: Frame = 0, vrátí false. Pro V1 OK.
 */
void test_dispatch_condition_false_no_hit ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_expr ( id, "Frame > 5" );
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 0, bpt->hits );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * Action `continue` -> 100 fire = 100 hits, žádný pause.
 *
 * V1.5 HWE: pro 100 hits střídáme rising edges (0->1, 0->1, ...).
 * Každé fire(1) po fire(0) = jeden rising edge.
 */
void test_dispatch_action_continue_no_pause ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_action ( id, "continue" );

    int i;
    for ( i = 0; i < 100; i++ ) {
        bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* falling / no-op */
        bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* rising = hit */
    }

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 100, bpt->hits );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL ( 0, s_mock_state.call_count );
}


/**
 * Fire eventu pro který není BP = no-op.
 */
void test_dispatch_no_bp_no_op ( void ) {
    /* Nezakládám žádný BP. Fire = nesmí hodit. */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL ( 0, s_mock_state.call_count );
}


/**
 * Fire eventu E1 nesmí hit BP s eventem E2.
 */
void test_dispatch_event_filter ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    bp_event_fire ( BP_EVENT_GDG_HSYNC, 0 ); /* hsync, ne vsync */

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL ( 0, bpt->hits );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * BPTMAP_IDX_HW_EVENT - registrovaný BP se zahrne do per-typ listu.
 */
void test_bptmap_register_hw_event ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    GArray *list = bptmap_get_list ( BPTMAP_IDX_HW_EVENT );
    TEST_ASSERT_NOT_NULL ( list );
    TEST_ASSERT_EQUAL ( 1, list->len );
    TEST_ASSERT_EQUAL ( id, g_array_index ( list, int, 0 ) );
    TEST_ASSERT_TRUE ( g_bptmap.per_type_active[ BPTMAP_IDX_HW_EVENT ] );
    TEST_ASSERT_TRUE ( g_bptmap.any_active );
}


/* ========================================================================= */
/*  Persist - JSON round-trip event_name                                      */
/* ========================================================================= */


/**
 * Save BP s event="vsync" do tmp souboru, clear, load, ověř že
 * event_name = "vsync" + parsed_event = BP_EVENT_GDG_VSYNC.
 */
void test_persist_event_name_roundtrip ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_action ( id, "continue" );

    /* Tmp file v env-friendly umístění. */
    gchar *tmpfile = g_build_filename ( g_get_tmp_dir ( ), "test_bp_hw_event.bpt", NULL );
    breakpoints_save_to_filepath ( tmpfile );

    breakpoints_clear_all ( );
    TEST_ASSERT_FALSE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );

    breakpoints_load_from_filepath ( tmpfile );
    TEST_ASSERT_EQUAL ( 1, g_breakpoints.breakpoints->len );

    st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, 0 );
    TEST_ASSERT_EQUAL_STRING ( "vsync", bpt->event_name );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_VSYNC, bpt->parsed_event );
    TEST_ASSERT_EQUAL ( 0, bpt->event_param );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );

    g_unlink ( tmpfile );
    g_free ( tmpfile );
}


/**
 * raster:192 round-trip - event_param se odvozuje z event_name po loadu.
 */
void test_persist_raster_param_roundtrip ( void ) {
    int id = make_hw_event_bp ( "raster:192" );
    breakpoints_set_action ( id, "continue" );

    gchar *tmpfile = g_build_filename ( g_get_tmp_dir ( ), "test_bp_raster.bpt", NULL );
    breakpoints_save_to_filepath ( tmpfile );
    breakpoints_clear_all ( );
    breakpoints_load_from_filepath ( tmpfile );

    st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, 0 );
    TEST_ASSERT_EQUAL_STRING ( "raster:192", bpt->event_name );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_RASTER, bpt->parsed_event );
    TEST_ASSERT_EQUAL ( 192, bpt->event_param );

    g_unlink ( tmpfile );
    g_free ( tmpfile );
}


/* ========================================================================= */
/*  V1.5 HWE - Event vocabulary (28 events, 4 kindy)                         */
/* ========================================================================= */


/**
 * Round-trip ověření všech 31 known eventů (= bez NONE/COUNT sentinelů).
 *
 * V1.5: 28 events.
 * V1.6+ TODO 4.6: +3 (CPU_IFF1_CHANGE, CPU_IFF2_CHANGE, CMT_STATE_CHANGE).
 *
 * Každý event musí mít stabilní string name + parsovat zpět na stejné enum.
 */
void test_event_vocabulary_31_roundtrip ( void ) {
    en_BP_EVENT e;
    for ( e = (en_BP_EVENT) ( BP_EVENT_NONE + 1 ); e < BP_EVENT_COUNT;
          e = (en_BP_EVENT) ( e + 1 ) ) {
        const char *name = bp_event_to_string ( e );
        TEST_ASSERT_NOT_NULL ( name );
        TEST_ASSERT_TRUE ( name[0] != '\0' );

        en_BP_EVENT parsed = BP_EVENT_NONE;
        int32_t param = -1;
        TEST_ASSERT_TRUE ( bp_event_from_string ( name, &parsed, &param ) );
        TEST_ASSERT_EQUAL ( e, parsed );
    }
}


/**
 * SIGNAL kind subset (17 events) - vrací BP_EVT_KIND_SIGNAL.
 */
void test_event_kind_signal_subset ( void ) {
    en_BP_EVENT signals[] = {
        BP_EVENT_GDG_VSYNC, BP_EVENT_GDG_HSYNC, BP_EVENT_GDG_VBLN, BP_EVENT_GDG_HBLN,
        BP_EVENT_CTC_ZC0, BP_EVENT_CTC_ZC1, BP_EVENT_CTC_ZC2,
        BP_EVENT_IRQ_CTC2, BP_EVENT_IRQ_PIOZ80_A, BP_EVENT_IRQ_PIOZ80_B, BP_EVENT_IRQ_FDC,
        BP_EVENT_TEMPO, BP_EVENT_CURSOR,
        BP_EVENT_CMT_IN, BP_EVENT_CMT_OUT, BP_EVENT_CMT_MSTATE, BP_EVENT_CMT_MOTOR,
    };
    size_t i;
    for ( i = 0; i < sizeof ( signals ) / sizeof ( signals[0] ); i++ ) {
        TEST_ASSERT_EQUAL ( BP_EVT_KIND_SIGNAL, bp_event_get_kind ( signals[i] ) );
    }
}


void test_event_kind_change_subset ( void ) {
    en_BP_EVENT changes[] = {
        BP_EVENT_GDG_MODE_CHANGE, BP_EVENT_GDG_PALETTE_CHANGE,
        BP_EVENT_GDG_PALGRP_CHANGE, BP_EVENT_GDG_BORDER_CHANGE,
        BP_EVENT_GDG_CKSW_CHANGE,
        /* V1.6+ TODO 4.6: CMT_STATE_CHANGE pridan jako CHANGE kind. */
        BP_EVENT_CMT_STATE_CHANGE,
    };
    size_t i;
    for ( i = 0; i < sizeof ( changes ) / sizeof ( changes[0] ); i++ ) {
        TEST_ASSERT_EQUAL ( BP_EVT_KIND_CHANGE, bp_event_get_kind ( changes[i] ) );
    }
}


void test_event_kind_point_param ( void ) {
    /* Jediný point param event v V1.5: raster:N. */
    TEST_ASSERT_EQUAL ( BP_EVT_KIND_POINT_PARAM,
                         bp_event_get_kind ( BP_EVENT_GDG_RASTER ) );
}


void test_event_kind_cpu_subset ( void ) {
    en_BP_EVENT cpu_evs[] = {
        BP_EVENT_CPU_NMI, BP_EVENT_CPU_DI, BP_EVENT_CPU_IM_CHANGE,
        BP_EVENT_CPU_IFF_CHANGE,
        /* V1.6+ TODO 4.6: separate IFF1/IFF2 events. */
        BP_EVENT_CPU_IFF1_CHANGE, BP_EVENT_CPU_IFF2_CHANGE,
        BP_EVENT_CPU_HALT, BP_EVENT_CPU_RESET,
    };
    size_t i;
    for ( i = 0; i < sizeof ( cpu_evs ) / sizeof ( cpu_evs[0] ); i++ ) {
        TEST_ASSERT_EQUAL ( BP_EVT_KIND_POINT_NOPARAM,
                             bp_event_get_kind ( cpu_evs[i] ) );
    }
}


/* ========================================================================= */
/*  V1.5 HWE - Parametrized raster:N                                         */
/* ========================================================================= */


void test_event_raster_param_serialize ( void ) {
    /* bp_event_to_string vrací "raster" bez ":N" - param je samostatně. */
    TEST_ASSERT_EQUAL_STRING ( "raster", bp_event_to_string ( BP_EVENT_GDG_RASTER ) );
}


void test_event_raster_param_parse_invalid ( void ) {
    en_BP_EVENT e = BP_EVENT_NONE;
    int32_t p = -1;
    /* "raster:abc" = invalid param. */
    TEST_ASSERT_FALSE ( bp_event_from_string ( "raster:abc", &e, &p ) );
}


void test_event_raster_param_range ( void ) {
    en_BP_EVENT e;
    int32_t p;

    /* 0 (= top row). */
    e = BP_EVENT_NONE; p = -1;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "raster:0", &e, &p ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_RASTER, e );
    TEST_ASSERT_EQUAL ( 0, p );

    /* 311 (= bottom row PAL). */
    e = BP_EVENT_NONE; p = -1;
    TEST_ASSERT_TRUE ( bp_event_from_string ( "raster:311", &e, &p ) );
    TEST_ASSERT_EQUAL ( BP_EVENT_GDG_RASTER, e );
    TEST_ASSERT_EQUAL ( 311, p );
}


/* ========================================================================= */
/*  V1.5 HWE - Trigger conditions                                             */
/* ========================================================================= */


void test_event_trigger_to_string_all ( void ) {
    TEST_ASSERT_EQUAL_STRING ( "rising",  bp_event_trigger_to_string ( BP_EVT_TRIG_RISING ) );
    TEST_ASSERT_EQUAL_STRING ( "falling", bp_event_trigger_to_string ( BP_EVT_TRIG_FALLING ) );
    TEST_ASSERT_EQUAL_STRING ( "changed", bp_event_trigger_to_string ( BP_EVT_TRIG_CHANGED ) );
    TEST_ASSERT_EQUAL_STRING ( "low",     bp_event_trigger_to_string ( BP_EVT_TRIG_LOW ) );
    TEST_ASSERT_EQUAL_STRING ( "high",    bp_event_trigger_to_string ( BP_EVT_TRIG_HIGH ) );
}


void test_event_trigger_from_string_all ( void ) {
    en_BP_EVENT_TRIGGER t;
    TEST_ASSERT_TRUE ( bp_event_trigger_from_string ( "rising", &t ) );
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_RISING, t );
    TEST_ASSERT_TRUE ( bp_event_trigger_from_string ( "falling", &t ) );
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_FALLING, t );
    TEST_ASSERT_TRUE ( bp_event_trigger_from_string ( "changed", &t ) );
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_CHANGED, t );
    TEST_ASSERT_TRUE ( bp_event_trigger_from_string ( "low", &t ) );
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_LOW, t );
    TEST_ASSERT_TRUE ( bp_event_trigger_from_string ( "high", &t ) );
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_HIGH, t );
}


void test_event_trigger_from_string_invalid ( void ) {
    en_BP_EVENT_TRIGGER t = BP_EVT_TRIG_RISING;
    TEST_ASSERT_FALSE ( bp_event_trigger_from_string ( NULL, &t ) );
    TEST_ASSERT_FALSE ( bp_event_trigger_from_string ( "FOO", &t ) );
    TEST_ASSERT_FALSE ( bp_event_trigger_from_string ( "Rising", &t ) );  /* case-sensitive */
    TEST_ASSERT_EQUAL ( BP_EVT_TRIG_RISING, t );  /* unchanged */
}


/* ========================================================================= */
/*  V1.5 HWE - Edge detection per trigger condition                          */
/* ========================================================================= */


/**
 * RISING: prev=0, curr=1 → fire. Sustained curr=1 → no re-fire.
 */
void test_trigger_rising_edge ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_RISING );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* prev=0, curr=0 - no edge */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* rising = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* sustained = no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* falling = no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* rising znovu = fire */

    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * FALLING: prev=1, curr=0 → fire.
 */
void test_trigger_falling_edge ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_FALLING );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* prev=0, curr=1 - rising, no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* falling = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* rising = no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* falling = fire */

    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * CHANGED: prev != curr → fire (= obě hrany).
 */
void test_trigger_changed_both_edges ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_CHANGED );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* changed (0->1) */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* changed (1->0) */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* changed */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* sustained = no fire */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * LOW: curr == 0 → fire (level, ne edge).
 */
void test_trigger_low_level ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_LOW );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* low = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* still low = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* high = no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* low = fire */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * HIGH: curr == 1 → fire.
 */
void test_trigger_high_level ( void ) {
    int id = make_hw_event_bp ( "vsync" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_HIGH );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* high = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* still high = fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 0 );  /* low = no fire */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );  /* high = fire */

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  V1.5 HWE - Change events (implicit "happened")                           */
/* ========================================================================= */


void test_change_event_mode_change_fires ( void ) {
    int id = make_hw_event_bp ( "mode_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_MODE_CHANGE, 0x42 );
    bp_event_fire ( BP_EVENT_GDG_MODE_CHANGE, 0x88 );

    /* CHANGE kind = každé fire = hit (no edge logic). */
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


void test_change_event_palette_change ( void ) {
    int id = make_hw_event_bp ( "palette_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_PALETTE_CHANGE, 0xFF );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
    (void) id;
}


void test_change_event_border_change ( void ) {
    int id = make_hw_event_bp ( "border_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_GDG_BORDER_CHANGE, 0x00 );
    bp_event_fire ( BP_EVENT_GDG_BORDER_CHANGE, 0x07 );
    bp_event_fire ( BP_EVENT_GDG_BORDER_CHANGE, 0x07 );  /* CHANGE doesn't dedupe */
    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  V1.5 HWE - CPU events (point/state, žádný selector)                      */
/* ========================================================================= */


void test_cpu_event_nmi_fires ( void ) {
    int id = make_hw_event_bp ( "nmi" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CPU_NMI, 0 );
    bp_event_fire ( BP_EVENT_CPU_NMI, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


void test_cpu_event_halt_fires ( void ) {
    int id = make_hw_event_bp ( "halt" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CPU_HALT, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
    (void) id;
}


void test_cpu_event_reset_fires ( void ) {
    int id = make_hw_event_bp ( "reset" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CPU_RESET, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  V1.5 HWE - g_bp_event_active fast-skip bitmap                            */
/* ========================================================================= */


/**
 * Fire bez registrovaného BP - bitmap je false → no-op.
 */
void test_fast_skip_inactive_event ( void ) {
    /* Žádný BP - bitmap[VSYNC] = false. */
    TEST_ASSERT_FALSE ( g_bp_event_active[ BP_EVENT_GDG_VSYNC ] );

    /* Fire bez BP = no crash, no side effect (= early return). */
    bp_event_fire ( BP_EVENT_GDG_VSYNC, 1 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_INT ( 0, s_mock_state.call_count );
}


/**
 * Po registrace BP - bitmap je true; po unregister - false.
 */
void test_fast_skip_bitmap_lifecycle ( void ) {
    int id = make_hw_event_bp ( "halt" );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_CPU_HALT ] );

    /* Disable BP - bitmap reset. */
    breakpoints_set_enabled ( id, false );
    TEST_ASSERT_FALSE ( g_bp_event_active[ BP_EVENT_CPU_HALT ] );

    /* Re-enable - bitmap zpět. */
    breakpoints_set_enabled ( id, true );
    TEST_ASSERT_TRUE ( g_bp_event_active[ BP_EVENT_CPU_HALT ] );
}


/* ========================================================================= */
/*  V1.5 fáze 2.2 / 2.3 - HWE polling -> edge refactor verify                 */
/* ========================================================================= */


/**
 * V1.5 fáze 2.3: bp_event_fire(BP_EVENT_CMT_IN, X) opakovaně se stejnou
 * hodnotou nesmí způsobit duplicate hit. Edge detection v
 * breakpoints_enforce_hw_event eliminuje stable level fire.
 *
 * Tento test ověřuje že CMT_IN s RISING trigger fire jen na skutečné
 * 0->1 přechod, ne na stable level. Dříve byl polling fire v PIO 8255
 * PortC read + legacy fire v cmt_play/cmt_eject = duplicate hit risk.
 */
void test_cmt_in_no_duplicate_on_stable_level ( void ) {
    int id = make_hw_event_bp ( "cmt:in" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_RISING );
    breakpoints_set_action ( id, "$cnt += 1" );

    /* Stable 0 - žádný edge. */
    bp_event_fire ( BP_EVENT_CMT_IN, 0 );
    bp_event_fire ( BP_EVENT_CMT_IN, 0 );
    bp_event_fire ( BP_EVENT_CMT_IN, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "cnt" ) );

    /* Rising edge - jeden fire. */
    bp_event_fire ( BP_EVENT_CMT_IN, 1 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* Stable 1 - žádný další fire. */
    bp_event_fire ( BP_EVENT_CMT_IN, 1 );
    bp_event_fire ( BP_EVENT_CMT_IN, 1 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* Falling edge - žádný fire (RISING trigger). */
    bp_event_fire ( BP_EVENT_CMT_IN, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* Druhý rising edge - druhý fire. */
    bp_event_fire ( BP_EVENT_CMT_IN, 1 );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );

    (void) id;
}


/**
 * V1.5 fáze 2.2: BP_EVENT_CURSOR edge fire pri změně cursor blink state.
 * Cursor state = (g_mzarch_main.cursor_timer / 25) & 1, fire nyní z
 * mz800_gdg_event.c po každém screen_done.
 *
 * Tento test ověřuje že edge detection v enforce vrstvě správně rozliší
 * stable vs change u CURSOR eventu (= analogicky cmt:in test, jen pro
 * cursor signal).
 */
void test_cursor_edge_detection ( void ) {
    int id = make_hw_event_bp ( "cursor" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_CHANGED );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CURSOR, 0 );  /* prev=0, curr=0, no change */
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "cnt" ) );

    bp_event_fire ( BP_EVENT_CURSOR, 1 );  /* change 0->1 */
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* Simulace 24 frames se stable cursor = 0 (= před edge na frame 25). */
    for ( int i = 0; i < 24; i++ ) {
        bp_event_fire ( BP_EVENT_CURSOR, 1 );
    }
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );

    /* Frame 25 - cursor flip 1->0 = další change. */
    bp_event_fire ( BP_EVENT_CURSOR, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );

    (void) id;
}


/**
 * V1.5 fáze 2.2: CMT_MSTATE edge detection ověření. Reálný fire z cmt.c
 * (non-CMTHACK) nebo pio8255_write (CMTHACK), zde testujeme jen že
 * enforce vrstva správně edge detekuje (= analogie cursor/cmt:in).
 */
void test_cmt_mstate_edge_detection ( void ) {
    int id = make_hw_event_bp ( "cmt:mstate" );
    breakpoints_set_event_trigger ( id, BP_EVT_TRIG_RISING );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CMT_MSTATE, 0 );  /* stable 0 */
    bp_event_fire ( BP_EVENT_CMT_MSTATE, 1 );  /* rising = fire */
    bp_event_fire ( BP_EVENT_CMT_MSTATE, 1 );  /* stable 1 */
    bp_event_fire ( BP_EVENT_CMT_MSTATE, 0 );  /* falling = no fire */
    bp_event_fire ( BP_EVENT_CMT_MSTATE, 1 );  /* rising znovu = fire */

    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  V1.6+ TODO 4.6 - HW_EVENT vocabulary expand                              */
/* ========================================================================= */


/**
 * V1.6+ 4.6: BP_EVENT_CPU_IFF1_CHANGE fire + filter test.
 *
 * Verifikuje:
 * - parse "cpu:iff1_change" -> BP_EVENT_CPU_IFF1_CHANGE
 * - kind = POINT_NOPARAM (CPU event)
 * - fire pri value 0/1 = vzdy hit (= bez edge filteringu, point event)
 */
void test_cpu_iff1_change_event_fires ( void ) {
    int id = make_hw_event_bp ( "cpu:iff1_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CPU_IFF1_CHANGE, 1 );
    bp_event_fire ( BP_EVENT_CPU_IFF1_CHANGE, 0 );
    TEST_ASSERT_EQUAL_INT32 ( 2, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * V1.6+ 4.6: BP_EVENT_CPU_IFF2_CHANGE fire + filter test.
 *
 * Filter ze separate fire pro IFF1 vs IFF2 - BP zaregistrovan jen na
 * IFF2 nesmi reagovat na IFF1 fire.
 */
void test_cpu_iff2_change_event_fires_isolated ( void ) {
    int id = make_hw_event_bp ( "cpu:iff2_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    /* Fire jen IFF1 - BP na IFF2 nesmi reagovat. */
    if ( g_bp_event_active[ BP_EVENT_CPU_IFF1_CHANGE ] ) {
        bp_event_fire ( BP_EVENT_CPU_IFF1_CHANGE, 1 );
    }
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "cnt" ) );

    /* Fire IFF2 - BP musi zareagovat. */
    bp_event_fire ( BP_EVENT_CPU_IFF2_CHANGE, 1 );
    TEST_ASSERT_EQUAL_INT32 ( 1, bp_var_get ( "cnt" ) );
    (void) id;
}


/**
 * V1.6+ 4.6: BP_EVENT_CMT_STATE_CHANGE fire test.
 *
 * Verifikuje:
 * - parse "cmt:state_change" -> BP_EVENT_CMT_STATE_CHANGE
 * - kind = CHANGE
 * - value je en_BP_CMT_STATE (STOP=0/PLAY=1/RECORD=2/PAUSED=3)
 * - kazde fire = hit (CHANGE kind nezahazuje stable level)
 */
void test_cmt_state_change_event_fires ( void ) {
    int id = make_hw_event_bp ( "cmt:state_change" );
    breakpoints_set_action ( id, "$cnt += 1" );

    bp_event_fire ( BP_EVENT_CMT_STATE_CHANGE, BP_CMT_STATE_STOP );
    bp_event_fire ( BP_EVENT_CMT_STATE_CHANGE, BP_CMT_STATE_PLAY );
    bp_event_fire ( BP_EVENT_CMT_STATE_CHANGE, BP_CMT_STATE_PAUSED );
    bp_event_fire ( BP_EVENT_CMT_STATE_CHANGE, BP_CMT_STATE_PLAY );

    /* CHANGE kind = vsechny 4 fires = hits. */
    TEST_ASSERT_EQUAL_INT32 ( 4, bp_var_get ( "cnt" ) );
    (void) id;
}


/* ========================================================================= */
/*  Test runner                                                               */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* String konverze */
    RUN_TEST ( test_event_string_vsync_roundtrip );
    RUN_TEST ( test_event_string_ctc_zc0_with_colon );
    RUN_TEST ( test_event_string_raster_with_param );
    RUN_TEST ( test_event_string_unknown );
    RUN_TEST ( test_event_string_empty );

    /* set_event_name + cache + bitmap */
    RUN_TEST ( test_set_event_name_updates_cache_and_bitmap );
    RUN_TEST ( test_set_event_name_raster_param );
    RUN_TEST ( test_disabled_bp_clears_bitmap );

    /* Dispatch */
    RUN_TEST ( test_dispatch_vsync_basic_hit );
    RUN_TEST ( test_dispatch_raster_param_match );
    RUN_TEST ( test_dispatch_condition_false_no_hit );
    RUN_TEST ( test_dispatch_action_continue_no_pause );
    RUN_TEST ( test_dispatch_no_bp_no_op );
    RUN_TEST ( test_dispatch_event_filter );
    RUN_TEST ( test_bptmap_register_hw_event );

    /* Persist */
    RUN_TEST ( test_persist_event_name_roundtrip );
    RUN_TEST ( test_persist_raster_param_roundtrip );

    /* V1.5 HWE - Vocabulary 28 events */
    RUN_TEST ( test_event_vocabulary_31_roundtrip );
    RUN_TEST ( test_event_kind_signal_subset );
    RUN_TEST ( test_event_kind_change_subset );
    RUN_TEST ( test_event_kind_point_param );
    RUN_TEST ( test_event_kind_cpu_subset );

    /* V1.5 HWE - Parametrized raster:N */
    RUN_TEST ( test_event_raster_param_serialize );
    RUN_TEST ( test_event_raster_param_parse_invalid );
    RUN_TEST ( test_event_raster_param_range );

    /* V1.5 HWE - Trigger conditions */
    RUN_TEST ( test_event_trigger_to_string_all );
    RUN_TEST ( test_event_trigger_from_string_all );
    RUN_TEST ( test_event_trigger_from_string_invalid );

    /* V1.5 HWE - Edge detection */
    RUN_TEST ( test_trigger_rising_edge );
    RUN_TEST ( test_trigger_falling_edge );
    RUN_TEST ( test_trigger_changed_both_edges );
    RUN_TEST ( test_trigger_low_level );
    RUN_TEST ( test_trigger_high_level );

    /* V1.5 HWE - Change events */
    RUN_TEST ( test_change_event_mode_change_fires );
    RUN_TEST ( test_change_event_palette_change );
    RUN_TEST ( test_change_event_border_change );

    /* V1.5 HWE - CPU events */
    RUN_TEST ( test_cpu_event_nmi_fires );
    RUN_TEST ( test_cpu_event_halt_fires );
    RUN_TEST ( test_cpu_event_reset_fires );

    /* V1.5 HWE - Fast-skip bitmap */
    RUN_TEST ( test_fast_skip_inactive_event );
    RUN_TEST ( test_fast_skip_bitmap_lifecycle );

    /* V1.5 fáze 2.2 / 2.3: HWE polling -> edge refactor verify */
    RUN_TEST ( test_cmt_in_no_duplicate_on_stable_level );
    RUN_TEST ( test_cursor_edge_detection );
    RUN_TEST ( test_cmt_mstate_edge_detection );

    /* V1.6+ TODO 4.6: HW_EVENT vocabulary expand (3 nove eventy) */
    RUN_TEST ( test_cpu_iff1_change_event_fires );
    RUN_TEST ( test_cpu_iff2_change_event_fires_isolated );
    RUN_TEST ( test_cmt_state_change_event_fires );

    return UNITY_END ( );
}
