/*
 * test_bp_persist_bc.c - BC (Backwards Compatibility) load testy V1.5
 *
 * Pokrytí:
 *   - V1 minimal .bpt fixture: ověření že chybějící V1.5 fields dostanou
 *     defaultní hodnoty (no crash, no error, sane defaults)
 *   - V1.5 full .bpt fixture: ověření že všechna nová pole (match modes,
 *     port mode, IRQ filter, IRQ_SIG, HWE trigger, vars sekce s comment
 *     a persist_value) se loadnou správně round-trip
 *
 * Fixtures: tests/debugger/fixtures/bpt/v1_minimal.bpt + v1_5_full.bpt
 *           (cesty pres MZTEST_FIXTURES_DIR compile-time define)
 *
 * Strategie: copy fixture do cfg-dir tmpfile, load, ověřit obsah.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/breakpoints.h"
#include "debugger/bp_event.h"
#include "debugger/bp_vars.h"
#include "main.h"  /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>


#ifndef MZTEST_FIXTURES_DIR
#error "MZTEST_FIXTURES_DIR must be defined (= absolutni cesta k tests/debugger/fixtures)"
#endif


/* ========================================================================= */
/*  setUp / tearDown                                                          */
/* ========================================================================= */


void setUp ( void ) {
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    bp_vars_clear_storage ( );
}


void tearDown ( void ) {
    /* nothing */
}


/* ========================================================================= */
/*  Helper: copy fixture file z repo do cfg-dir tmpfile a vrátí jméno        */
/*  k pouziti v breakpoints_load_from_filepath (resolver je relativni        */
/*  k cfg dir).                                                              */
/* ========================================================================= */


static char* copy_fixture_to_cfgdir ( const char *fixture_subpath,
                                       const char *tmpfile_name ) {
    char src_path[ 1024 ];
    snprintf ( src_path, sizeof ( src_path ), "%s/%s",
               MZTEST_FIXTURES_DIR, fixture_subpath );

    char *dst_resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths, tmpfile_name );
    if ( !dst_resolved ) return NULL;

    /* Načíst obsah fixture do bufferu. */
    FILE *fsrc = fopen ( src_path, "rb" );
    if ( !fsrc ) {
        fprintf ( stderr, "FIXTURE missing: %s\n", src_path );
        g_free ( dst_resolved );
        return NULL;
    };
    fseek ( fsrc, 0, SEEK_END );
    long sz = ftell ( fsrc );
    fseek ( fsrc, 0, SEEK_SET );

    char *buf = (char *) malloc ( (size_t) sz + 1 );
    if ( !buf ) { fclose ( fsrc ); g_free ( dst_resolved ); return NULL; };
    size_t read_bytes = fread ( buf, 1, (size_t) sz, fsrc );
    fclose ( fsrc );
    if ( read_bytes != (size_t) sz ) {
        fprintf ( stderr, "FIXTURE short read: %s\n", src_path );
        free ( buf );
        g_free ( dst_resolved );
        return NULL;
    };
    buf[ sz ] = '\0';

    /* Zapsat do cfg-dir cílové cesty. */
    FILE *fdst = fopen ( dst_resolved, "wb" );
    if ( !fdst ) {
        fprintf ( stderr, "Cannot write tmp: %s\n", dst_resolved );
        free ( buf );
        g_free ( dst_resolved );
        return NULL;
    };
    fwrite ( buf, 1, (size_t) sz, fdst );
    fclose ( fdst );

    free ( buf );
    return dst_resolved;  /* caller g_free + remove */
}


/* ========================================================================= */
/*  V1 minimal fixture: BC fallback test                                     */
/* ========================================================================= */


/**
 * V1 fixture: jen základní fields (id/name/addr/type/enabled/skip/hit/expr/
 * action/edge_triggered). Ověřuje že load nepadne a nové V1.5 fields
 * dostanou sane defaults.
 */
void test_bc_load_v1_minimal ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_minimal.bpt",
                                               "test_bc_v1.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_load_from_filepath ( resolved );

    TEST_ASSERT_EQUAL_INT ( 2, (int) breakpoints_count ( ) );

    st_BPT *bp1 = breakpoints_find_by_addr ( 5000 );
    TEST_ASSERT_NOT_NULL ( bp1 );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_PC_EXEC, bp1->type );
    /* V1.5 fields - defaults. */
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_SINGLE, bp1->addr_match_mode );
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_SINGLE, bp1->port_match_mode );
    TEST_ASSERT_EQUAL_INT ( BP_PORT_8BIT,    bp1->port_mode );
    TEST_ASSERT_EQUAL_INT ( BP_ZONE_CPU_VIEW, bp1->zone );
    /* IRQ filter defaults: vše im modes ON, žádné im2 vector/isr filter. */
    /* (= default po add - settery nechávají jako neutralní). */

    st_BPT *bp2 = breakpoints_find_by_addr ( 16384 );
    TEST_ASSERT_NOT_NULL ( bp2 );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_MEM_W, bp2->type );
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_SINGLE, bp2->addr_match_mode );

    /* Vars sekce missing v V1 fixture → 0 vars. */
    TEST_ASSERT_EQUAL_INT ( 0, (int) bp_vars_count ( ) );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  V1.5 full fixture: všechna pole load                                     */
/* ========================================================================= */


/**
 * V1.5 fixture: všechna nová pole + 6 BP různých typů + vars sekce.
 */
void test_bc_load_v1_5_full ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_5_full.bpt",
                                               "test_bc_v15.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_load_from_filepath ( resolved );

    TEST_ASSERT_EQUAL_INT ( 6, (int) breakpoints_count ( ) );

    /* PC range. */
    st_BPT *pc = breakpoints_find_by_addr ( 16384 );
    TEST_ASSERT_NOT_NULL ( pc );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_PC_EXEC, pc->type );
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_RANGE, pc->addr_match_mode );
    TEST_ASSERT_EQUAL_HEX16 ( 16639, pc->addr_end );

    /* IORQ 16bit. */
    st_BPT *iorq = breakpoints_find_by_addr ( 0 /* port stored in port field, not addr */ );
    /* find_by_addr možná nehledá podle port - hledejme podle id. */
    iorq = breakpoints_find_by_id ( 2 );
    TEST_ASSERT_NOT_NULL ( iorq );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_IORQ_W, iorq->type );
    TEST_ASSERT_EQUAL_INT ( BP_PORT_16BIT, iorq->port_mode );
    TEST_ASSERT_EQUAL_HEX16 ( 65486, iorq->port );

    /* IRQ filter. */
    st_BPT *irq = breakpoints_find_by_id ( 3 );
    TEST_ASSERT_NOT_NULL ( irq );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_IRQ, irq->type );
    TEST_ASSERT_TRUE  ( irq->im0_enabled );
    TEST_ASSERT_FALSE ( irq->im1_enabled );
    TEST_ASSERT_TRUE  ( irq->im2_enabled );
    TEST_ASSERT_EQUAL_HEX8 ( 165, irq->im0_rst_mask );
    TEST_ASSERT_TRUE  ( irq->im2_vector_enabled );
    TEST_ASSERT_EQUAL_HEX16 ( 65040, irq->im2_vector_addr );
    TEST_ASSERT_TRUE  ( irq->im2_isr_enabled );
    TEST_ASSERT_EQUAL_HEX16 ( 4660, irq->im2_isr_addr );

    /* IRQ_SIG mask. */
    st_BPT *sig = breakpoints_find_by_id ( 4 );
    TEST_ASSERT_NOT_NULL ( sig );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_IRQ_SIG, sig->type );
    TEST_ASSERT_EQUAL_HEX8 ( 13, sig->irq_sig_source_mask );

    /* HWE raster:192. */
    st_BPT *ras = breakpoints_find_by_id ( 5 );
    TEST_ASSERT_NOT_NULL ( ras );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_HW_EVENT, ras->type );
    TEST_ASSERT_EQUAL_STRING ( "raster:192", ras->event_name );
    TEST_ASSERT_EQUAL_INT ( BP_EVENT_GDG_RASTER, ras->parsed_event );
    TEST_ASSERT_EQUAL ( 192, ras->event_param );
    TEST_ASSERT_EQUAL_INT ( BP_EVT_TRIG_RISING, ras->event_trigger );

    /* HWE vsync falling. */
    st_BPT *vsync = breakpoints_find_by_id ( 6 );
    TEST_ASSERT_NOT_NULL ( vsync );
    TEST_ASSERT_EQUAL_STRING ( "vsync", vsync->event_name );
    TEST_ASSERT_EQUAL_INT ( BP_EVT_TRIG_FALLING, vsync->event_trigger );

    /* Vars sekce. */
    TEST_ASSERT_EQUAL_INT ( 3, (int) bp_vars_count ( ) );
    TEST_ASSERT_EQUAL_INT32  ( 100, bp_var_get ( "score" ) );
    TEST_ASSERT_EQUAL_STRING ( "hra body", bp_var_get_comment ( "score" ) );
    TEST_ASSERT_TRUE  ( bp_var_get_persist ( "score" ) );

    TEST_ASSERT_EQUAL_INT32 ( 3, bp_var_get ( "lives" ) );
    TEST_ASSERT_NULL ( bp_var_get_comment ( "lives" ) );

    /* runtime: persist=false → value reset 0 i kdyby v souboru byla. */
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_var_get ( "runtime" ) );
    TEST_ASSERT_FALSE ( bp_var_get_persist ( "runtime" ) );
    TEST_ASSERT_EQUAL_STRING ( "ephemeral runtime counter",
                                bp_var_get_comment ( "runtime" ) );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  V1 -> V1.5 round-trip: load V1 fixture, save, load znovu, verify         */
/* ========================================================================= */


/**
 * Load V1 minimal fixture, save jako V1.5, load zpět - musí mít V1.5
 * defaults persistované jako explicit stringy (= upgrade migration).
 */
void test_bc_v1_to_v1_5_round_trip ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_minimal.bpt",
                                               "test_bc_upgrade.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    /* Phase 1: load V1. */
    breakpoints_load_from_filepath ( resolved );
    TEST_ASSERT_EQUAL_INT ( 2, (int) breakpoints_count ( ) );

    /* Phase 2: save (= dostane V1.5 schema). */
    breakpoints_save_to_filepath ( resolved );

    /* Phase 3: wipe + load - musí matchovat. */
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    breakpoints_load_from_filepath ( resolved );

    TEST_ASSERT_EQUAL_INT ( 2, (int) breakpoints_count ( ) );
    st_BPT *bp1 = breakpoints_find_by_addr ( 5000 );
    TEST_ASSERT_NOT_NULL ( bp1 );
    TEST_ASSERT_EQUAL_INT ( BPT_TYPE_PC_EXEC, bp1->type );
    TEST_ASSERT_EQUAL_INT ( BP_MATCH_SINGLE, bp1->addr_match_mode );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  V1.5 - Group safety: cycle detection + dangling parent                   */
/* ========================================================================= */


/**
 * Fixture obsahuje 4 problematic groups:
 *   - id=1, parent=2 + id=2, parent=1  → 2-cycle
 *   - id=3, parent=3                   → self-loop
 *   - id=4, parent=9999                → dangling parent
 *
 * Po loadu validate_group_parents všechny problematické groups unparent
 * (parent=-1) a vypíše warning na stderr.
 */
void test_bc_load_groups_cycle_detection ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_5_groups_cycle.bpt",
                                               "test_bc_groups.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_load_from_filepath ( resolved );

    /* Všechny 4 groups loadnuly, ale parent reset. */
    TEST_ASSERT_EQUAL_INT ( 4, (int) breakpoints_group_count ( ) );

    /* 2-cycle: jeden z {id=1, id=2} bude unparent (= validate jde po
     * groups in order, první detekuje cyklus a unparent sám sebe).
     * Druhý už pak má funkční chain (= unparent root). */
    st_BPTGROUP *g1 = breakpoints_group_find_by_id ( 1 );
    st_BPTGROUP *g2 = breakpoints_group_find_by_id ( 2 );
    TEST_ASSERT_NOT_NULL ( g1 );
    TEST_ASSERT_NOT_NULL ( g2 );
    /* Aspoň jeden z těch dvou má parent=-1 (cyklus rozbit). */
    TEST_ASSERT_TRUE ( g1->parent == -1 || g2->parent == -1 );

    /* Self-loop: id=3 musí být unparent. */
    st_BPTGROUP *g3 = breakpoints_group_find_by_id ( 3 );
    TEST_ASSERT_NOT_NULL ( g3 );
    TEST_ASSERT_EQUAL_INT ( -1, g3->parent );

    /* Dangling parent: id=4 musí být unparent. */
    st_BPTGROUP *g4 = breakpoints_group_find_by_id ( 4 );
    TEST_ASSERT_NOT_NULL ( g4 );
    TEST_ASSERT_EQUAL_INT ( -1, g4->parent );

    remove ( resolved );
    g_free ( resolved );
}


/**
 * Sanity test: po load + cycle resolution + save + reload zůstávají
 * všechny groups unparent (= validation je idempotent).
 */
void test_bc_groups_cycle_idempotent_after_save_reload ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_5_groups_cycle.bpt",
                                               "test_bc_idempotent.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_load_from_filepath ( resolved );

    /* Save (= dostane sanitized state). */
    breakpoints_save_to_filepath ( resolved );

    /* Wipe + load znovu - cykly už nebudou. */
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    breakpoints_load_from_filepath ( resolved );

    /* Self-loop a dangle vždy unparent. */
    st_BPTGROUP *g3 = breakpoints_group_find_by_id ( 3 );
    TEST_ASSERT_NOT_NULL ( g3 );
    TEST_ASSERT_EQUAL_INT ( -1, g3->parent );

    st_BPTGROUP *g4 = breakpoints_group_find_by_id ( 4 );
    TEST_ASSERT_NOT_NULL ( g4 );
    TEST_ASSERT_EQUAL_INT ( -1, g4->parent );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  V1.6+ TODO 4.7 - schema_version BC: fixtures bez klíče loadnou OK         */
/* ========================================================================= */


/**
 * Existing V1 / V1.5 fixtures byly zapsány PŘED zavedením schema_version
 * klíče (= TODO 4.7). Loader tedy musí akceptovat missing klíč jako BC
 * fallback (= info log, ne chyba).
 *
 * Test reuse v1_minimal.bpt fixture - load nesmí zhavarovat ani odmítnout
 * data; BPT count + obsah musí zůstat shodný.
 */
void test_bc_schema_version_missing_in_fixture ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_minimal.bpt",
                                               "test_bc_schver_v1.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    /* Load nesmí selhat ani vrátit prázdný state (= BC fallback funguje). */
    breakpoints_load_from_filepath ( resolved );

    TEST_ASSERT_EQUAL_INT ( 2, (int) breakpoints_count ( ) );
    /* BPs byly úspěšně načteny - schema_version missing jen způsobil
     * info log "assuming V1.0-pre-versioning". */

    remove ( resolved );
    g_free ( resolved );
}


/**
 * V1.5 full fixture rovněž bez schema_version klíče - load BC.
 */
void test_bc_schema_version_missing_in_v1_5_fixture ( void ) {
    char *resolved = copy_fixture_to_cfgdir ( "bpt/v1_5_full.bpt",
                                               "test_bc_schver_v15.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_load_from_filepath ( resolved );

    TEST_ASSERT_EQUAL_INT ( 6, (int) breakpoints_count ( ) );
    TEST_ASSERT_EQUAL_INT ( 3, (int) bp_vars_count ( ) );

    remove ( resolved );
    g_free ( resolved );
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */


/**
 * V1.D.2.C - origin field round-trip přes save/load.
 *
 * Vytvoří BP, nastaví různé cmd_origin, save, wipe, load - origin
 * fields se musí obnovit z JSON `origin` klíče.
 */
void test_bc_origin_field_roundtrip ( void ) {
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;

    int id_a = breakpoints_add ( 0x4000, "bp_user",     -1 );
    int id_b = breakpoints_add ( 0x5000, "bp_mcp",      -1 );
    int id_c = breakpoints_add ( 0x6000, "bp_internal", -1 );
    TEST_ASSERT_TRUE ( id_a > 0 );
    TEST_ASSERT_TRUE ( id_b > 0 );
    TEST_ASSERT_TRUE ( id_c > 0 );

    st_BPT *bp_a = breakpoints_find_by_id ( id_a );
    st_BPT *bp_b = breakpoints_find_by_id ( id_b );
    st_BPT *bp_c = breakpoints_find_by_id ( id_c );
    TEST_ASSERT_NOT_NULL ( bp_a );
    TEST_ASSERT_NOT_NULL ( bp_b );
    TEST_ASSERT_NOT_NULL ( bp_c );

    bp_a->cmd_origin = DBGAPI_CMD_ORIGIN_USER;
    bp_b->cmd_origin = DBGAPI_CMD_ORIGIN_MCP;
    bp_c->cmd_origin = DBGAPI_CMD_ORIGIN_INTERNAL;

    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths,
                                                 "test_bp_origin_rt.bpt" );
    TEST_ASSERT_NOT_NULL ( resolved );

    breakpoints_save_to_filepath ( resolved );

    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    TEST_ASSERT_EQUAL_INT ( 0, (int) breakpoints_count ( ) );

    breakpoints_load_from_filepath ( resolved );
    TEST_ASSERT_EQUAL_INT ( 3, (int) breakpoints_count ( ) );

    /* Po reload najdi BP podle addr (= ID se může změnit) a ověř origin. */
    st_BPT *r_a = breakpoints_find_by_addr ( 0x4000 );
    st_BPT *r_b = breakpoints_find_by_addr ( 0x5000 );
    st_BPT *r_c = breakpoints_find_by_addr ( 0x6000 );
    TEST_ASSERT_NOT_NULL ( r_a );
    TEST_ASSERT_NOT_NULL ( r_b );
    TEST_ASSERT_NOT_NULL ( r_c );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_USER,     r_a->cmd_origin );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_MCP,      r_b->cmd_origin );
    TEST_ASSERT_EQUAL_INT ( DBGAPI_CMD_ORIGIN_INTERNAL, r_c->cmd_origin );

    remove ( resolved );
    g_free ( resolved );
}


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_bc_load_v1_minimal );
    RUN_TEST ( test_bc_load_v1_5_full );
    RUN_TEST ( test_bc_v1_to_v1_5_round_trip );

    /* V1.5 group safety */
    RUN_TEST ( test_bc_load_groups_cycle_detection );
    RUN_TEST ( test_bc_groups_cycle_idempotent_after_save_reload );

    /* V1.6+ TODO 4.7 - schema_version BC fallback */
    RUN_TEST ( test_bc_schema_version_missing_in_fixture );
    RUN_TEST ( test_bc_schema_version_missing_in_v1_5_fixture );

    /* V1.D.2.C - cmd_origin field persist */
    RUN_TEST ( test_bc_origin_field_roundtrip );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
