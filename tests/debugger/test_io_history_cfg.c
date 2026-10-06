/**
 * @file test_io_history_cfg.c
 * @brief Kapacita ringu a maska zaznamenávaných portů historie I/O
 *        z konfigurace se projeví po initu a přežijí změnu kapacity.
 *
 * Regresní test: klíč `[IO_PORTS_PANEL] history_capacity` dřív registrovalo
 * okno I/O Ports do UI proměnné (g_io_ui.history_capacity), debugger_init()
 * ale alokoval ring vždy s IO_HISTORY_DEFAULT_CAPACITY - uložená kapacita
 * se po startu ukázala jen v comboboxu, ring měl 10000. Nově klíč patří
 * jádru (io_history_register_persistence, g_io_history_cfg_capacity)
 * a debugger_init() volá io_history_init_from_cfg().
 *
 * Ověřuje se stejná sekvence jako v debugger_init() (registrace do modulu
 * IO_PORTS_PANEL, cfgmodule_parse, cfgmodule_propagate,
 * io_history_init_from_cfg) nad dočasným INI:
 *  - kapacita z INI se projeví v g_io_history.capacity,
 *  - chybějící klíč = IO_HISTORY_DEFAULT_CAPACITY,
 *  - změna kapacity (io_history_set_capacity, handler příkazu z okna) se
 *    při uložení konfigurace zapíše a po "restartu" platí,
 *  - maska zaznamenávaných portů (`record_mask`) z INI platí po initu i po
 *    změně kapacity a uložená maska po "restartu" platí. Dřív klíč
 *    registrovalo okno I/O Ports a io_history_init() (start i každá změna
 *    kapacity) masku přepsal na "vše zaznamenávat".
 *
 * Nepokrývá: samotné volání debugger_init() (headless test ho nevolá -
 * inicializuje celý debugger vč. UI persistence).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <stdio.h>
#include <string.h>

#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "baseui/baseui_tools.h"
#include "debugger/io_history.h"


/** @brief Dočasný INI soubor (relativně k pracovnímu adresáři testu). */
static const char *c_test_ini = "test_io_history_cfg.ini";


/**
 * @brief Smaže dočasný INI soubor (pokud existuje).
 */
static void remove_ini ( void )
{
    char *locale_path = baseui_tools_file_name_locale_from_utf8 ( c_test_ini );
    if ( locale_path != NULL ) {
        remove ( locale_path );
        baseui_tools_mem_free ( locale_path );
    }
}


/**
 * @brief Zapíše @p text do dočasného INI.
 * @param text Obsah souboru.
 */
static void write_ini ( const char *text )
{
    FILE *fp = baseui_tools_file_open ( c_test_ini, "w" );
    TEST_ASSERT_NOT_NULL ( fp );
    fputs ( text, fp );
    fclose ( fp );
}


/**
 * @brief Start jako v debugger_init(): registrace, parse, propagate a init
 *        ringu z konfigurace.
 *
 * g_io_history_cfg_capacity se před tím nastaví na nesmyslnou hodnotu, aby
 * test poznal, že ji propagate skutečně přepsal.
 */
static void startup_from_ini ( void )
{
    g_io_history_cfg_capacity = 0;
    st_CFGROOT *r = cfgroot_new ( c_test_ini );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
    io_history_register_persistence ( m );
    cfgmodule_parse ( m );
    cfgmodule_propagate ( m );
    cfgroot_destroy ( r );

    io_history_init_from_cfg ( );
}


void setUp ( void )
{
    remove_ini ( );
    io_history_destroy ( );
    io_history_record_enable_all ( );
}


void tearDown ( void )
{
    io_history_destroy ( );
    io_history_record_enable_all ( );
    g_io_history_cfg_capacity = IO_HISTORY_DEFAULT_CAPACITY;
    remove_ini ( );
}


/**
 * @brief Uloží modul IO_PORTS_PANEL (jen klíče jádra) do dočasného INI.
 *
 * Napodobuje uložení konfigurace při ukončení emulátoru.
 */
static void save_ini ( void )
{
    st_CFGROOT *r = cfgroot_new ( c_test_ini );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "IO_PORTS_PANEL" );
    io_history_register_persistence ( m );
    FILE *fp = baseui_tools_file_open ( c_test_ini, "w" );
    TEST_ASSERT_NOT_NULL ( fp );
    r->ini_fp = fp;
    cfgmodule_save ( m );
    fclose ( fp );
    r->ini_fp = NULL;
    cfgroot_destroy ( r );
}


/**
 * @brief Kapacita z INI (25000) se po initu projeví v ringu.
 *
 * UNSIGNED parser cfgmodule čte hex (formát save 0x%x): 25000 = 0x61A8.
 */
void test_capacity_from_ini_applied_to_ring ( void )
{
    write_ini ( "[IO_PORTS_PANEL]\nhistory_capacity = 0x61A8\n" );
    startup_from_ini ( );

    TEST_ASSERT_EQUAL_UINT ( 25000u, g_io_history_cfg_capacity );
    TEST_ASSERT_EQUAL_UINT ( 25000u, (unsigned) g_io_history.capacity );
    TEST_ASSERT_NOT_NULL ( g_io_history.events );
}


/**
 * @brief Bez klíče v INI má ring výchozí kapacitu.
 */
void test_missing_key_gives_default ( void )
{
    write_ini ( "[IO_PORTS_PANEL]\nhistory_auto_follow = 1\n" );
    startup_from_ini ( );

    TEST_ASSERT_EQUAL_UINT ( IO_HISTORY_DEFAULT_CAPACITY,
                             (unsigned) g_io_history.capacity );
}


/**
 * @brief Změna kapacity za běhu se uloží a po dalším startu platí.
 */
void test_set_capacity_persists_across_restart ( void )
{
    write_ini ( "[IO_PORTS_PANEL]\n" );
    startup_from_ini ( );
    TEST_ASSERT_EQUAL_UINT ( IO_HISTORY_DEFAULT_CAPACITY,
                             (unsigned) g_io_history.capacity );

    /* Emu vlákno vykoná příkaz okna (DBGAPI_CMD_IO_HISTORY_SET_CAPACITY). */
    io_history_set_capacity ( 5000 );
    TEST_ASSERT_EQUAL_UINT ( 5000u, g_io_history_cfg_capacity );

    /* Uložení konfigurace při ukončení. */
    save_ini ( );

    /* "Restart". */
    io_history_destroy ( );
    g_io_history_cfg_capacity = IO_HISTORY_DEFAULT_CAPACITY;
    startup_from_ini ( );
    TEST_ASSERT_EQUAL_UINT ( 5000u, (unsigned) g_io_history.capacity );
}


/**
 * @brief Maska nastavená před initem ringu (= po cfg propagate) přežije
 *        init z konfigurace i změnu kapacity.
 *
 * Nezávislé na tom, kdo masku z INI naplní: io_history_init() ani
 * io_history_set_capacity() ji nesmí přepsat.
 */
void test_record_mask_survives_init_and_capacity_change ( void )
{
    g_io_history_record_enabled[ 0x10 ] = 0u;

    io_history_init_from_cfg ( );
    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0x10 ] );

    io_history_set_capacity ( 5000 );
    TEST_ASSERT_EQUAL_UINT ( 5000u, (unsigned) g_io_history.capacity );
    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0x10 ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ 0x11 ] );
}


/**
 * @brief Maska z INI platí po startu i po změně kapacity.
 *
 * Port 10h = bajt 2 masky, bit 0: znaky 4-5 = "FE" (vypnut jen port 10h).
 */
void test_record_mask_from_ini_applied ( void )
{
    write_ini ( "[IO_PORTS_PANEL]\nrecord_mask = "
                "FFFFFEFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n" );
    startup_from_ini ( );

    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0x10 ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ 0x00 ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ 0x11 ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ 0xFF ] );

    io_history_set_capacity ( 20000 );
    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0x10 ] );
}


/**
 * @brief Bez klíče v INI se zaznamenávají všechny porty.
 */
void test_record_mask_missing_key_records_all ( void )
{
    g_io_history_record_enabled[ 0x42 ] = 0u;
    write_ini ( "[IO_PORTS_PANEL]\n" );
    startup_from_ini ( );

    for ( unsigned i = 0; i < IO_HISTORY_RECORD_MAP_SIZE; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ i ] );
    }
}


/**
 * @brief Změněná maska se uloží a po "restartu" platí.
 */
void test_record_mask_persists_across_restart ( void )
{
    write_ini ( "[IO_PORTS_PANEL]\n" );
    startup_from_ini ( );

    /* Uživatel v okně vypne port 42h a FEh. */
    g_io_history_record_enabled[ 0x42 ] = 0u;
    g_io_history_record_enabled[ 0xFE ] = 0u;
    save_ini ( );

    /* "Restart" - maska v paměti zpět na výchozí stav. */
    io_history_destroy ( );
    io_history_record_enable_all ( );
    startup_from_ini ( );

    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0x42 ] );
    TEST_ASSERT_EQUAL_UINT8 ( 0u, g_io_history_record_enabled[ 0xFE ] );
    TEST_ASSERT_EQUAL_UINT8 ( 1u, g_io_history_record_enabled[ 0x43 ] );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_capacity_from_ini_applied_to_ring );
    RUN_TEST ( test_missing_key_gives_default );
    RUN_TEST ( test_set_capacity_persists_across_restart );
    RUN_TEST ( test_record_mask_survives_init_and_capacity_change );
    RUN_TEST ( test_record_mask_from_ini_applied );
    RUN_TEST ( test_record_mask_missing_key_records_all );
    RUN_TEST ( test_record_mask_persists_across_restart );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
