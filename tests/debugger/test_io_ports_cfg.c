/*
 * test_io_ports_cfg.c - Cfg roundtrip test pro IO_PORTS_PANEL sekci.
 *
 * Sprint 2 Fáze 4.4. Verifikuje cfgmodule pattern modulu IO_PORTS_PANEL
 * (= klíče collapse_<chip>, history_capacity, history_auto_follow,
 * tracking_active). history_capacity dnes registruje jádro
 * (io_history_register_persistence, vazbu na ring ověřuje
 * test_io_history_cfg.c), ostatní io_window_register_persistence.
 *
 * Test strategie:
 *  - Vytvoří cfgroot s docasnym INI souborem
 *  - Zaregistruje stejný set klíčů jako io_window (= simulace, target
 *    proměnné jsou lokální, ne g_io_ui - propojení s g_io_ui ověří
 *    UI smoke test)
 *  - Ověří default values
 *  - Ověří save/load roundtrip (= save -> read INI -> reset -> propagate ->
 *    target value matches)
 *  - Ověří BC fallback (= chybějící klíč v INI -> default value)
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "baseui/baseui_tools.h"

/* Konstanty z io_history.h */
#define IO_PORTS_HISTORY_DEFAULT_CAP   10000
#define IO_PORTS_HISTORY_MIN_CAP        1000
#define IO_PORTS_HISTORY_MAX_CAP       50000


/* ========================================================================
 * setUp / tearDown
 * ======================================================================== */

static char *test_ini_file = NULL;


void setUp ( void )
{
    /* CWD-relative INI path - mztest_init nastavuje work_dir. */
    if ( test_ini_file == NULL ) {
        test_ini_file = strdup ( "test_io_ports_cfg.ini" );
    }
}


void tearDown ( void )
{
    if ( test_ini_file != NULL ) {
        char *locale_path = baseui_tools_file_name_locale_from_utf8 ( test_ini_file );
        if ( locale_path != NULL ) {
            remove ( locale_path );
            baseui_tools_mem_free ( locale_path );
        }
    }
}


/* ========================================================================
 * Helpery - simulace io_window_register_persistence pattern
 * ======================================================================== */

/**
 * @brief Stav simulujici g_io_ui pole používané register_persistence.
 */
typedef struct {
    int collapse_gdg;
    int collapse_ppi8255;
    int history_capacity;
    int auto_follow;
    int tracking_active;
    int detail_panel_height;  /* V1.5.D fix #1 */
} sim_io_state_t;


/**
 * @brief Zaregistruje stejné klíče jako io_window_register_persistence.
 */
static void register_io_keys ( st_CFGMODULE *cmod, sim_io_state_t *st )
{
    st_CFGELEMENT *elm;

    elm = cfgmodule_register_new_element ( cmod, (char *)"collapse_gdg",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, &st->collapse_gdg, &st->collapse_gdg );

    elm = cfgmodule_register_new_element ( cmod, (char *)"collapse_8255_ppi",
                                            CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, &st->collapse_ppi8255, &st->collapse_ppi8255 );

    elm = cfgmodule_register_new_element ( cmod, (char *)"history_capacity",
                                            CFGENTYPE_UNSIGNED,
                                            IO_PORTS_HISTORY_DEFAULT_CAP,
                                            IO_PORTS_HISTORY_MIN_CAP,
                                            IO_PORTS_HISTORY_MAX_CAP );
    cfgelement_set_handlers ( elm, &st->history_capacity, &st->history_capacity );

    elm = cfgmodule_register_new_element ( cmod, (char *)"history_auto_follow",
                                            CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, &st->auto_follow, &st->auto_follow );

    elm = cfgmodule_register_new_element ( cmod, (char *)"tracking_active",
                                            CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, &st->tracking_active, &st->tracking_active );

    /* V1.5.D fix #1: Selected event detail panel height. */
    elm = cfgmodule_register_new_element ( cmod, (char *)"detail_panel_height",
                                            CFGENTYPE_UNSIGNED, 160, 60, 400 );
    cfgelement_set_handlers ( elm, &st->detail_panel_height,
                              &st->detail_panel_height );
}


/* ========================================================================
 * Defaults
 * ======================================================================== */

void test_io_cfg_defaults ( void )
{
    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
    register_io_keys ( m, &st );

    cfgmodule_propagate ( m );

    /* Defaults per Sprint 2 spec. */
    TEST_ASSERT_EQUAL_INT ( 0, st.collapse_gdg );       /* expanded */
    TEST_ASSERT_EQUAL_INT ( 0, st.collapse_ppi8255 );   /* expanded */
    TEST_ASSERT_EQUAL_INT ( IO_PORTS_HISTORY_DEFAULT_CAP, st.history_capacity );
    TEST_ASSERT_EQUAL_INT ( 1, st.auto_follow );
    TEST_ASSERT_EQUAL_INT ( 1, st.tracking_active );    /* default ON, gated na otevřené okno */
    /* V1.5.D fix #1: detail panel default 160 px. */
    TEST_ASSERT_EQUAL_INT ( 160, st.detail_panel_height );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * Roundtrip: set -> save -> reset -> propagate -> verify
 * ======================================================================== */

void test_io_cfg_roundtrip_collapse ( void )
{
    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    /* Krok 1: register + set value + save. */
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        st.collapse_gdg = 1;
        st.collapse_ppi8255 = 1;

        FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
        TEST_ASSERT_NOT_NULL ( fp );
        r->ini_fp = fp;
        cfgmodule_save ( m );
        fclose ( fp );
        r->ini_fp = NULL;

        cfgroot_destroy ( r );
    }

    /* Krok 2: re-register fresh state, parse INI, propagate. */
    memset ( &st, 0, sizeof ( st ) );
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        cfgmodule_parse ( m );
        cfgmodule_propagate ( m );

        TEST_ASSERT_EQUAL_INT ( 1, st.collapse_gdg );
        TEST_ASSERT_EQUAL_INT ( 1, st.collapse_ppi8255 );

        cfgroot_destroy ( r );
    }
}


void test_io_cfg_roundtrip_capacity ( void )
{
    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    /* Save 25000 */
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        st.history_capacity = 25000;

        FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
        TEST_ASSERT_NOT_NULL ( fp );
        r->ini_fp = fp;
        cfgmodule_save ( m );
        fclose ( fp );
        r->ini_fp = NULL;

        cfgroot_destroy ( r );
    }

    /* Load + verify */
    memset ( &st, 0, sizeof ( st ) );
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        cfgmodule_parse ( m );
        cfgmodule_propagate ( m );

        TEST_ASSERT_EQUAL_INT ( 25000, st.history_capacity );

        cfgroot_destroy ( r );
    }
}


void test_io_cfg_roundtrip_tracking ( void )
{
    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    /* Set tracking_active = 1 + auto_follow = 0, save, reload, verify. */
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        st.tracking_active = 1;
        st.auto_follow = 0;

        FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
        r->ini_fp = fp;
        cfgmodule_save ( m );
        fclose ( fp );
        r->ini_fp = NULL;
        cfgroot_destroy ( r );
    }

    memset ( &st, 0, sizeof ( st ) );
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        cfgmodule_parse ( m );
        cfgmodule_propagate ( m );

        TEST_ASSERT_EQUAL_INT ( 1, st.tracking_active );
        TEST_ASSERT_EQUAL_INT ( 0, st.auto_follow );

        cfgroot_destroy ( r );
    }
}


/* ========================================================================
 * BC fallback - chybějící klíč v INI musí dát default
 * ======================================================================== */

void test_io_cfg_bc_fallback_missing_key ( void )
{
    /* Vytvoř INI s jen 1 klíčem - history_capacity, ostatní default.
     * Cfgmodule UNSIGNED parser parsuje vždy hex (kompatibilní se save
     * formátem 0x%02x), takže 5000 (= 0x1388) píšeme jako 0x1388. */
    FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
    TEST_ASSERT_NOT_NULL ( fp );
    fputs ( "[IO_PORTS_PANEL]\nhistory_capacity = 0x1388\n", fp );
    fclose ( fp );

    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
    register_io_keys ( m, &st );

    cfgmodule_parse ( m );
    cfgmodule_propagate ( m );

    /* history_capacity z INI */
    TEST_ASSERT_EQUAL_INT ( 5000, st.history_capacity );
    /* ostatní defaults */
    TEST_ASSERT_EQUAL_INT ( 0, st.collapse_gdg );
    TEST_ASSERT_EQUAL_INT ( 1, st.tracking_active );    /* default ON */
    TEST_ASSERT_EQUAL_INT ( 1, st.auto_follow );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * V1.5.D fix #1: detail_panel_height roundtrip
 * ======================================================================== */

void test_io_cfg_roundtrip_detail_panel_height ( void )
{
    sim_io_state_t st;
    memset ( &st, 0, sizeof ( st ) );

    /* Save 240 */
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        st.detail_panel_height = 240;

        FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
        TEST_ASSERT_NOT_NULL ( fp );
        r->ini_fp = fp;
        cfgmodule_save ( m );
        fclose ( fp );
        r->ini_fp = NULL;

        cfgroot_destroy ( r );
    }

    /* Load + verify */
    memset ( &st, 0, sizeof ( st ) );
    {
        st_CFGROOT *r = cfgroot_new ( test_ini_file );
        st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
        register_io_keys ( m, &st );

        cfgmodule_parse ( m );
        cfgmodule_propagate ( m );

        TEST_ASSERT_EQUAL_INT ( 240, st.detail_panel_height );

        cfgroot_destroy ( r );
    }
}


/* ========================================================================
 * Main
 * ======================================================================== */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_io_cfg_defaults );
    RUN_TEST ( test_io_cfg_roundtrip_collapse );
    RUN_TEST ( test_io_cfg_roundtrip_capacity );
    RUN_TEST ( test_io_cfg_roundtrip_tracking );
    RUN_TEST ( test_io_cfg_bc_fallback_missing_key );
    RUN_TEST ( test_io_cfg_roundtrip_detail_panel_height );

    return UNITY_END ( );
}
