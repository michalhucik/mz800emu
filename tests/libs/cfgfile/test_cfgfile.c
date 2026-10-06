/**
 * @file   test_cfgfile.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Testy knihovny cfgfile (INI konfiguracni system)
 *
 * Testuje hierarchii root/module/element, vsechny typy elementu
 * (KEYWORD, BOOL, TEXT, UNSIGNED, FLOAT), propagate/save cyklus,
 * handler/pointer binding, cfgelement_bind(), INI parsovani a zapis,
 * by_name wrappery, reset, chybove stavy a memory management.
 *
 * Standalone test — cfgfile pouziva baseui_tools_file_open pro I/O,
 * proto se linkuje s TEST_ALL_OBJS.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mztest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <glib.h>

#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "baseui/baseui_tools.h"


/* ========================================================================
 * setUp / tearDown — volany pred/po kazdem testu
 * ======================================================================== */

/** @brief Docasny INI soubor pro testy parsovani a zapisu — cesta se konstruuje za behu */
static char *test_ini_file = NULL;


/**
 * @brief Inicializace pred kazdym testem (Unity setUp)
 */
void setUp ( void ) {
}


/**
 * @brief Uklid po kazdem testu — smaze docasny INI soubor
 */
void tearDown ( void ) {
    /* Po kazdem testu smazeme docasny INI soubor */
    if ( test_ini_file != NULL ) {
        /* Pouzijeme locale konverzi pro spravne smazani na vsech platformach */
        char *locale_path = baseui_tools_file_name_locale_from_utf8 ( test_ini_file );
        if ( locale_path != NULL ) {
            remove ( locale_path );
            baseui_tools_mem_free ( locale_path );
        };
    };
}


/* ========================================================================
 * Pomocne funkce
 * ======================================================================== */

/**
 * @brief Vytvori testovaci INI soubor s danym obsahem
 * @param content Textovy obsah INI souboru
 */
static void create_test_ini ( const char *content ) {
    FILE *fp = baseui_tools_file_open ( test_ini_file, "w" );
    TEST_ASSERT_NOT_NULL_MESSAGE ( fp, "Nelze vytvorit testovaci INI soubor" );
    fputs ( content, fp );
    fclose ( fp );
}


/**
 * @brief Otevre docasny INI soubor pro zapis (save)
 *
 * Nutne pred volanim cfgelement_save mimo cfgroot_save.
 *
 * @param r Ukazatel na cfgroot strukturu
 */
static void open_ini_for_save ( st_CFGROOT *r ) {
    r->ini_fp = baseui_tools_file_open ( test_ini_file, "w" );
    TEST_ASSERT_NOT_NULL_MESSAGE ( r->ini_fp, "Nelze otevrit docasny INI soubor pro save" );
}


/**
 * @brief Zavre docasny INI soubor po zapisu
 * @param r Ukazatel na cfgroot strukturu
 */
static void close_ini_after_save ( st_CFGROOT *r ) {
    if ( r->ini_fp != NULL ) {
        fclose ( r->ini_fp );
        r->ini_fp = NULL;
    };
}


/* ========================================================================
 * SMOKE TESTY — zakladni operace
 * ======================================================================== */

/** @brief Testuje vytvoreni a destrukci prazdneho cfgroot */
void test_root_new_destroy ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    TEST_ASSERT_NOT_NULL ( r );
    TEST_ASSERT_EQUAL_STRING ( test_ini_file, r->filename );
    TEST_ASSERT_EQUAL_INT ( 0, r->modules_count );
    TEST_ASSERT_NULL ( r->modules );
    cfgroot_destroy ( r );
}


/** @brief Testuje registraci modulu, duplicitni registraci a case-insensitive vyhledani */
void test_module_register_and_find ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );

    st_CFGMODULE *m1 = cfgroot_register_new_module ( r, "MODULE_A" );
    TEST_ASSERT_NOT_NULL ( m1 );
    TEST_ASSERT_EQUAL_INT ( 1, r->modules_count );

    st_CFGMODULE *m2 = cfgroot_register_new_module ( r, "MODULE_B" );
    TEST_ASSERT_NOT_NULL ( m2 );
    TEST_ASSERT_EQUAL_INT ( 2, r->modules_count );

    /* Duplicitni registrace — musi vratit NULL */
    st_CFGMODULE *m_dup = cfgroot_register_new_module ( r, "MODULE_A" );
    TEST_ASSERT_NULL ( m_dup );
    TEST_ASSERT_EQUAL_INT ( 2, r->modules_count );

    /* Vyhledani (case-insensitive) */
    TEST_ASSERT_EQUAL_PTR ( m1, cfgroot_get_module_by_name ( r, "module_a" ) );
    TEST_ASSERT_EQUAL_PTR ( m2, cfgroot_get_module_by_name ( r, "MODULE_B" ) );
    TEST_ASSERT_NULL ( cfgroot_get_module_by_name ( r, "NEEXISTUJE" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje registraci elementu, duplicitni registraci a case-insensitive vyhledani */
void test_element_register_and_find ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e_bool = cfgmodule_register_new_element ( m, "flag", CFGENTYPE_BOOL, 1 );
    TEST_ASSERT_NOT_NULL ( e_bool );

    st_CFGELEMENT *e_text = cfgmodule_register_new_element ( m, "name", CFGENTYPE_TEXT, "default" );
    TEST_ASSERT_NOT_NULL ( e_text );

    /* Duplicitni registrace — NULL */
    st_CFGELEMENT *e_dup = cfgmodule_register_new_element ( m, "flag", CFGENTYPE_BOOL, 0 );
    TEST_ASSERT_NULL ( e_dup );

    /* Vyhledani (case-insensitive) */
    TEST_ASSERT_EQUAL_PTR ( e_bool, cfgmodule_get_element_by_name ( m, "FLAG" ) );
    TEST_ASSERT_EQUAL_PTR ( e_text, cfgmodule_get_element_by_name ( m, "name" ) );
    TEST_ASSERT_NULL ( cfgmodule_get_element_by_name ( m, "NEEXISTUJE" ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY TYPU — BOOL
 * ======================================================================== */

/** @brief Testuje BOOL element — vychozi hodnota, nastaveni, normalizace na 0/1, reset */
void test_element_bool ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "enabled", CFGENTYPE_BOOL, 1 );
    TEST_ASSERT_NOT_NULL ( e );

    /* Vychozi hodnota */
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_value ( e ) );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_default_value ( e ) );

    /* Nastaveni hodnoty */
    cfgelement_set_bool_value ( e, 0 );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_get_bool_value ( e ) );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_default_value ( e ) );

    /* Normalizace na 0/1 — nenulova hodnota se musi prevest na 1 */
    cfgelement_set_bool_value ( e, 42 );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_value ( e ) );

    /* Reset na default */
    cfgelement_reset ( e );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY TYPU — TEXT
 * ======================================================================== */

/** @brief Testuje TEXT element — vychozi hodnota, nastaveni, reset */
void test_element_text ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "path", CFGENTYPE_TEXT, "/default/path" );
    TEST_ASSERT_NOT_NULL ( e );

    TEST_ASSERT_EQUAL_STRING ( "/default/path", cfgelement_get_text_value ( e ) );
    TEST_ASSERT_EQUAL_STRING ( "/default/path", cfgelement_get_text_default_value ( e ) );

    cfgelement_set_text_value ( e, "/new/path" );
    TEST_ASSERT_EQUAL_STRING ( "/new/path", cfgelement_get_text_value ( e ) );
    TEST_ASSERT_EQUAL_STRING ( "/default/path", cfgelement_get_text_default_value ( e ) );

    cfgelement_reset ( e );
    TEST_ASSERT_EQUAL_STRING ( "/default/path", cfgelement_get_text_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY TYPU — UNSIGNED
 * ======================================================================== */

/** @brief Testuje UNSIGNED element — vychozi hodnota, nastaveni, min/max validace, reset */
void test_element_unsigned ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "value", CFGENTYPE_UNSIGNED, 0x42, 0, 0xFF );
    TEST_ASSERT_NOT_NULL ( e );

    TEST_ASSERT_EQUAL_UINT ( 0x42, cfgelement_get_unsigned_value ( e ) );
    TEST_ASSERT_EQUAL_UINT ( 0x42, cfgelement_get_unsigned_default_value ( e ) );

    cfgelement_set_unsigned_value ( e, 0xAB );
    TEST_ASSERT_EQUAL_UINT ( 0xAB, cfgelement_get_unsigned_value ( e ) );

    /* Min/max validace */
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_property_test_unsigned_value ( e->ppu, 0x00 ) );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_property_test_unsigned_value ( e->ppu, 0xFF ) );
    TEST_ASSERT_EQUAL_INT ( -1, cfgelement_property_test_unsigned_value ( e->ppu, 0x100 ) );

    cfgelement_reset ( e );
    TEST_ASSERT_EQUAL_UINT ( 0x42, cfgelement_get_unsigned_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY TYPU — KEYWORD
 * ======================================================================== */

/** @brief Testuje KEYWORD element — vychozi hodnota, nastaveni, vyhledani keyword->hodnota, reset */
void test_element_keyword ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "mode", CFGENTYPE_KEYWORD, 0,
        0, "normal",
        1, "turbo",
        2, "slow",
        -1 );
    TEST_ASSERT_NOT_NULL ( e );

    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_get_keyword_value ( e ) );
    TEST_ASSERT_EQUAL_STRING ( "normal", cfgelement_get_keyword_by_value ( e ) );

    cfgelement_set_keyword_value ( e, 1 );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_keyword_value ( e ) );
    TEST_ASSERT_EQUAL_STRING ( "turbo", cfgelement_get_keyword_by_value ( e ) );

    /* Vyhledani keyword -> hodnota (case-insensitive) */
    TEST_ASSERT_EQUAL_INT ( 2, cfgelement_property_get_value_by_keyword ( e->pkw, "SLOW" ) );
    TEST_ASSERT_EQUAL_INT ( -1, cfgelement_property_get_value_by_keyword ( e->pkw, "neexistuje" ) );

    cfgelement_reset ( e );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_get_keyword_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY TYPU — FLOAT (novy typ)
 * ======================================================================== */

/** @brief Testuje FLOAT element — vychozi hodnota, nastaveni, min/max validace, reset */
void test_element_float ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "volume", CFGENTYPE_FLOAT, 0.5, 0.0, 1.0 );
    TEST_ASSERT_NOT_NULL ( e );

    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgelement_get_float_value ( e ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgelement_get_float_default_value ( e ) );

    cfgelement_set_float_value ( e, 0.75f );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.75f, cfgelement_get_float_value ( e ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgelement_get_float_default_value ( e ) );

    /* Min/max validace */
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_property_test_float_value ( e->ppf, 0.0f ) );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_property_test_float_value ( e->ppf, 1.0f ) );
    TEST_ASSERT_EQUAL_INT ( -1, cfgelement_property_test_float_value ( e->ppf, -0.1f ) );
    TEST_ASSERT_EQUAL_INT ( -1, cfgelement_property_test_float_value ( e->ppf, 1.1f ) );

    cfgelement_reset ( e );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgelement_get_float_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje presnost FLOAT elementu — male a velke hodnoty */
void test_element_float_precision ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    /* Test s malou hodnotou */
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "gain", CFGENTYPE_FLOAT, 0.001, 0.0, 100.0 );
    TEST_ASSERT_FLOAT_WITHIN ( 0.0001f, 0.001f, cfgelement_get_float_value ( e ) );

    /* Test s velkou hodnotou */
    cfgelement_set_float_value ( e, 99.999f );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 99.999f, cfgelement_get_float_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY BIND — sjednocena vazba na promennou
 * ======================================================================== */

/** @brief Testuje bind BOOL elementu — propagate a save smer */
void test_bind_bool ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "flag", CFGENTYPE_BOOL, 1 );

    int app_var = 0;
    cfgelement_bind ( e, &app_var );

    /* Propagate: element -> aplikace */
    cfgmodule_propagate ( m );
    TEST_ASSERT_EQUAL_INT ( 1, app_var );

    /* Zmena v aplikaci a save: aplikace -> element */
    app_var = 0;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_get_bool_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje bind UNSIGNED elementu — propagate a save smer */
void test_bind_unsigned ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "addr", CFGENTYPE_UNSIGNED, 0xFF, 0, 0xFFFF );

    unsigned app_var = 0;
    cfgelement_bind ( e, &app_var );

    cfgmodule_propagate ( m );
    TEST_ASSERT_EQUAL_UINT ( 0xFF, app_var );

    app_var = 0x1234;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_UINT ( 0x1234, cfgelement_get_unsigned_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje bind FLOAT elementu — propagate a save smer */
void test_bind_float ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "vol", CFGENTYPE_FLOAT, 0.5, 0.0, 1.0 );

    float app_var = 0.0f;
    cfgelement_bind ( e, &app_var );

    cfgmodule_propagate ( m );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, app_var );

    app_var = 0.8f;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.8f, cfgelement_get_float_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje bind TEXT elementu — propagate a save smer */
void test_bind_text ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "path", CFGENTYPE_TEXT, "/default" );

    char *app_var = NULL;
    cfgelement_bind ( e, &app_var );

    cfgmodule_propagate ( m );
    TEST_ASSERT_NOT_NULL ( app_var );
    TEST_ASSERT_EQUAL_STRING ( "/default", app_var );
    free ( app_var );
    app_var = NULL;

    /* Save smer: aplikace -> element */
    app_var = strdup ( "/changed" );
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_STRING ( "/changed", cfgelement_get_text_value ( e ) );
    free ( app_var );

    cfgroot_destroy ( r );
}


/** @brief Testuje bind KEYWORD elementu — propagate a save smer */
void test_bind_keyword ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "mode", CFGENTYPE_KEYWORD, 0,
        0, "off",
        1, "on",
        -1 );

    int app_var = -1;
    cfgelement_bind ( e, &app_var );

    cfgmodule_propagate ( m );
    TEST_ASSERT_EQUAL_INT ( 0, app_var );

    app_var = 1;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_keyword_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje cfgelement_bind_separate jen se save ukazatelem (bez propagate handleru) */
void test_bind_separate_save_only ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "val", CFGENTYPE_UNSIGNED, 10, 0, 100 );

    unsigned save_var = 42;
    cfgelement_bind_separate ( e, NULL, &save_var );

    /* Propagate nema handler — promenna se nezmeni */
    unsigned before = save_var;
    cfgmodule_propagate ( m );
    TEST_ASSERT_EQUAL_UINT ( before, save_var );

    /* Save: z promenne do elementu */
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_UINT ( 42, cfgelement_get_unsigned_value ( e ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY BY_NAME WRAPPERY
 * ======================================================================== */

/** @brief Testuje by_name wrappery pro vsechny typy — aktualni a vychozi hodnoty */
void test_by_name_wrappers ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    cfgmodule_register_new_element ( m, "b", CFGENTYPE_BOOL, 1 );
    cfgmodule_register_new_element ( m, "t", CFGENTYPE_TEXT, "hello" );
    cfgmodule_register_new_element ( m, "u", CFGENTYPE_UNSIGNED, 0xAB, 0, 0xFF );
    cfgmodule_register_new_element ( m, "k", CFGENTYPE_KEYWORD, 2,
        1, "one",
        2, "two",
        3, "three",
        -1 );
    cfgmodule_register_new_element ( m, "f", CFGENTYPE_FLOAT, 3.14, 0.0, 100.0 );

    /* Aktualni hodnoty pres by_name */
    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_bool_value_by_name ( m, "b" ) );
    TEST_ASSERT_EQUAL_STRING ( "hello", cfgmodule_get_element_text_value_by_name ( m, "t" ) );
    TEST_ASSERT_EQUAL_UINT ( 0xAB, cfgmodule_get_element_unsigned_value_by_name ( m, "u" ) );
    TEST_ASSERT_EQUAL_INT ( 2, cfgmodule_get_element_keyword_value_by_name ( m, "k" ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01f, 3.14f, cfgmodule_get_element_float_value_by_name ( m, "f" ) );

    /* Vychozi hodnoty pres by_name */
    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_bool_default_value_by_name ( m, "b" ) );
    TEST_ASSERT_EQUAL_STRING ( "hello", cfgmodule_get_element_text_default_value_by_name ( m, "t" ) );
    TEST_ASSERT_EQUAL_UINT ( 0xAB, cfgmodule_get_element_unsigned_default_value_by_name ( m, "u" ) );
    TEST_ASSERT_EQUAL_INT ( 2, cfgmodule_get_element_keyword_default_value_by_name ( m, "k" ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01f, 3.14f, cfgmodule_get_element_float_default_value_by_name ( m, "f" ) );

    /* Keyword by value/default */
    TEST_ASSERT_EQUAL_STRING ( "two", cfgmodule_get_element_keyword_by_value_by_name ( m, "k" ) );
    TEST_ASSERT_EQUAL_STRING ( "two", cfgmodule_get_element_keyword_by_default_value_by_name ( m, "k" ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY INI PARSE/SAVE — roundtrip
 * ======================================================================== */

/** @brief Testuje parsovani BOOL hodnoty z INI souboru */
void test_ini_parse_bool ( void ) {
    create_test_ini (
        "[TEST]\n"
        "enabled = 0\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "enabled", CFGENTYPE_BOOL, 1 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_INT ( 0, cfgmodule_get_element_bool_value_by_name ( m, "enabled" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani TEXT hodnoty z INI souboru */
void test_ini_parse_text ( void ) {
    create_test_ini (
        "[PATHS]\n"
        "datadir = /usr/share/mz800\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "PATHS" );
    cfgmodule_register_new_element ( m, "datadir", CFGENTYPE_TEXT, "" );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_STRING ( "/usr/share/mz800", cfgmodule_get_element_text_value_by_name ( m, "datadir" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani UNSIGNED hodnoty v hex formatu (0xFF) z INI souboru */
void test_ini_parse_unsigned_hex ( void ) {
    create_test_ini (
        "[TEST]\n"
        "addr = 0xFF\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "addr", CFGENTYPE_UNSIGNED, 0, 0, 0xFFFF );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT ( 0xFF, cfgmodule_get_element_unsigned_value_by_name ( m, "addr" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani UNSIGNED hodnoty v hash formatu (#A0) z INI souboru */
void test_ini_parse_unsigned_hash ( void ) {
    create_test_ini (
        "[TEST]\n"
        "color = #A0\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "color", CFGENTYPE_UNSIGNED, 0, 0, 0xFF );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_UINT ( 0xA0, cfgmodule_get_element_unsigned_value_by_name ( m, "color" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani KEYWORD hodnoty z INI souboru */
void test_ini_parse_keyword ( void ) {
    create_test_ini (
        "[DISPLAY]\n"
        "mode = turbo\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "DISPLAY" );
    cfgmodule_register_new_element ( m, "mode", CFGENTYPE_KEYWORD, 0,
        0, "normal",
        1, "turbo",
        -1 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_keyword_value_by_name ( m, "mode" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani FLOAT hodnoty z INI souboru */
void test_ini_parse_float ( void ) {
    create_test_ini (
        "[AUDIO]\n"
        "volume = 0.75\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "AUDIO" );
    cfgmodule_register_new_element ( m, "volume", CFGENTYPE_FLOAT, 0.5, 0.0, 1.0 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.75f, cfgmodule_get_element_float_value_by_name ( m, "volume" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani FLOAT hodnoty ve vedecke notaci (1e-3) z INI souboru */
void test_ini_parse_float_scientific ( void ) {
    create_test_ini (
        "[TEST]\n"
        "small = 1e-3\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "small", CFGENTYPE_FLOAT, 0.0, 0.0, 1.0 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_FLOAT_WITHIN ( 0.0001f, 0.001f, cfgmodule_get_element_float_value_by_name ( m, "small" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani z neexistujiciho INI souboru — hodnoty zustanou na vychozich */
void test_ini_parse_nonexistent_file ( void ) {
    char *nonexistent = g_build_filename ( g_get_tmp_dir (), "neexistujici_soubor_cfgfile_test.ini", NULL );
    st_CFGROOT *r = cfgroot_new ( nonexistent );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "val", CFGENTYPE_BOOL, 1 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, ret );

    /* Hodnota zustane na vychozi */
    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_bool_value_by_name ( m, "val" ) );

    cfgroot_destroy ( r );
    g_free ( nonexistent );
}


/** @brief Testuje case-insensitive parsovani nazvu modulu v INI souboru */
void test_ini_parse_module_case_insensitive ( void ) {
    create_test_ini (
        "[test]\n"
        "val = 0\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "val", CFGENTYPE_BOOL, 1 );

    int ret = cfgmodule_parse ( m );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );
    TEST_ASSERT_EQUAL_INT ( 0, cfgmodule_get_element_bool_value_by_name ( m, "val" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani vice modulu z jednoho INI souboru */
void test_ini_parse_multiple_modules ( void ) {
    create_test_ini (
        "[MODULE_A]\n"
        "x = 1\n"
        "\n"
        "[MODULE_B]\n"
        "y = 0\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *ma = cfgroot_register_new_module ( r, "MODULE_A" );
    cfgmodule_register_new_element ( ma, "x", CFGENTYPE_BOOL, 0 );

    st_CFGMODULE *mb = cfgroot_register_new_module ( r, "MODULE_B" );
    cfgmodule_register_new_element ( mb, "y", CFGENTYPE_BOOL, 1 );

    cfgmodule_parse ( ma );
    cfgmodule_parse ( mb );

    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_bool_value_by_name ( ma, "x" ) );
    TEST_ASSERT_EQUAL_INT ( 0, cfgmodule_get_element_bool_value_by_name ( mb, "y" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani UNSIGNED hodnoty mimo povoleny rozsah — pouzije se default */
void test_ini_parse_unsigned_out_of_range ( void ) {
    create_test_ini (
        "[TEST]\n"
        "val = 0xFF\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    /* Rozsah 0-100 — 0xFF je mimo */
    cfgmodule_register_new_element ( m, "val", CFGENTYPE_UNSIGNED, 0x42, 0, 100 );

    cfgmodule_parse ( m );

    /* Hodnota zustane na default, protoze INI hodnota je mimo rozsah */
    TEST_ASSERT_EQUAL_UINT ( 0x42, cfgmodule_get_element_unsigned_value_by_name ( m, "val" ) );

    cfgroot_destroy ( r );
}


/** @brief Testuje parsovani FLOAT hodnoty mimo povoleny rozsah — pouzije se default */
void test_ini_parse_float_out_of_range ( void ) {
    create_test_ini (
        "[TEST]\n"
        "vol = 1.5\n"
    );

    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    cfgmodule_register_new_element ( m, "vol", CFGENTYPE_FLOAT, 0.5, 0.0, 1.0 );

    cfgmodule_parse ( m );

    /* Hodnota zustane na default, protoze 1.5 je mimo rozsah */
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgmodule_get_element_float_value_by_name ( m, "vol" ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY SAVE/LOAD ROUNDTRIP
 * ======================================================================== */

/** @brief Testuje kompletni save/load roundtrip — ulozeni a zpetne nacteni vsech typu */
void test_ini_save_load_roundtrip ( void ) {
    /* Faze 1: vytvoreni a ulozeni */
    st_CFGROOT *r1 = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m1 = cfgroot_register_new_module ( r1, "SETTINGS" );

    cfgmodule_register_new_element ( m1, "enabled", CFGENTYPE_BOOL, 0 );
    cfgmodule_register_new_element ( m1, "path", CFGENTYPE_TEXT, "" );
    cfgmodule_register_new_element ( m1, "color", CFGENTYPE_UNSIGNED, 0, 0, 0xFFFF );
    cfgmodule_register_new_element ( m1, "speed", CFGENTYPE_KEYWORD, 0,
        0, "normal",
        1, "fast",
        -1 );
    cfgmodule_register_new_element ( m1, "volume", CFGENTYPE_FLOAT, 0.0, 0.0, 1.0 );

    /* Nastavime netrivialni hodnoty */
    st_CFGELEMENT *e;
    e = cfgmodule_get_element_by_name ( m1, "enabled" );
    cfgelement_set_bool_value ( e, 1 );
    e = cfgmodule_get_element_by_name ( m1, "path" );
    cfgelement_set_text_value ( e, "/home/test/data" );
    e = cfgmodule_get_element_by_name ( m1, "color" );
    cfgelement_set_unsigned_value ( e, 0xABCD );
    e = cfgmodule_get_element_by_name ( m1, "speed" );
    cfgelement_set_keyword_value ( e, 1 );
    e = cfgmodule_get_element_by_name ( m1, "volume" );
    cfgelement_set_float_value ( e, 0.75f );

    cfgroot_save ( r1 );
    cfgroot_destroy ( r1 );

    /* Faze 2: nacteni a overeni */
    st_CFGROOT *r2 = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m2 = cfgroot_register_new_module ( r2, "SETTINGS" );

    cfgmodule_register_new_element ( m2, "enabled", CFGENTYPE_BOOL, 0 );
    cfgmodule_register_new_element ( m2, "path", CFGENTYPE_TEXT, "" );
    cfgmodule_register_new_element ( m2, "color", CFGENTYPE_UNSIGNED, 0, 0, 0xFFFF );
    cfgmodule_register_new_element ( m2, "speed", CFGENTYPE_KEYWORD, 0,
        0, "normal",
        1, "fast",
        -1 );
    cfgmodule_register_new_element ( m2, "volume", CFGENTYPE_FLOAT, 0.0, 0.0, 1.0 );

    int ret = cfgmodule_parse ( m2 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, ret );

    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_bool_value_by_name ( m2, "enabled" ) );
    TEST_ASSERT_EQUAL_STRING ( "/home/test/data", cfgmodule_get_element_text_value_by_name ( m2, "path" ) );
    TEST_ASSERT_EQUAL_UINT ( 0xABCD, cfgmodule_get_element_unsigned_value_by_name ( m2, "color" ) );
    TEST_ASSERT_EQUAL_INT ( 1, cfgmodule_get_element_keyword_value_by_name ( m2, "speed" ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.75f, cfgmodule_get_element_float_value_by_name ( m2, "volume" ) );

    cfgroot_destroy ( r2 );
}


/* ========================================================================
 * TESTY PROPAGATE CALLBACK
 * ======================================================================== */

/** @brief Pocitadlo volani propagate callbacku pro regresni test */
static int g_propagate_cb_called = 0;

/** @brief Data prijata propagate callbackem pro regresni test */
static void *g_propagate_cb_received_data = NULL;

/**
 * @brief Testovaci propagate callback — zaznamenava volani a prijata data
 * @param m Ukazatel na modul (nepouzito)
 * @param data Uzivatelska data predana callbacku
 */
static void test_propagate_cb ( void *m, void *data ) {
    (void) m;
    g_propagate_cb_called = 1;
    g_propagate_cb_received_data = data;
}

/** @brief Testuje regresni bug #1 — propagate_cb musi dostat propagate_data, ne save_data */
void test_module_propagate_cb_data ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    int propagate_data = 42;
    int save_data = 99;

    cfgmodule_set_propagate_cb ( m, test_propagate_cb, &propagate_data );
    cfgmodule_set_save_cb ( m, NULL, &save_data );

    g_propagate_cb_called = 0;
    g_propagate_cb_received_data = NULL;

    cfgmodule_propagate ( m );

    TEST_ASSERT_EQUAL_INT ( 1, g_propagate_cb_called );
    /* Musi dostat propagate_data, NE save_data */
    TEST_ASSERT_EQUAL_PTR ( &propagate_data, g_propagate_cb_received_data );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TESTY PROPAGATE/SAVE S HANDLERY
 * ======================================================================== */

/** @brief Testuje propagate a save pres set_handlers (stary zpusob vazby) */
void test_handler_propagate_and_save ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "val", CFGENTYPE_UNSIGNED, 0x42, 0, 0xFF );

    unsigned app_var = 0;
    cfgelement_set_handlers ( e, &app_var, &app_var );

    /* Propagate */
    cfgelement_propagate ( e );
    TEST_ASSERT_EQUAL_UINT ( 0x42, app_var );

    /* Save */
    app_var = 0xAB;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_UINT ( 0xAB, cfgelement_get_unsigned_value ( e ) );

    cfgroot_destroy ( r );
}


/**
 * @brief Připnutá INI hodnota: save zapisuje hodnotu před přepsáním,
 *        dokud proměnná drží hodnotu jen pro běh.
 */
void test_pin_save_value_keeps_ini ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "conn", CFGENTYPE_BOOL, 1 );

    int app_var = 1;
    cfgelement_set_handlers ( e, &app_var, &app_var );

    /* Snapshot přepíše proměnnou jen pro běh. */
    app_var = 0;
    cfgelement_pin_save_value ( e, 1 );

    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_value ( e ) );
    TEST_ASSERT_EQUAL_INT ( 0, app_var );

    cfgroot_destroy ( r );
}


/** @brief Změna proměnné po připnutí (uživatel v menu) se do INI uloží. */
void test_pin_save_value_user_change_wins ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "size", CFGENTYPE_UNSIGNED, 1, 0, 10 );

    unsigned app_var = 1;
    cfgelement_set_handlers ( e, &app_var, &app_var );

    app_var = 5;                        /* snapshot */
    cfgelement_pin_save_value ( e, 1 );
    app_var = 7;                        /* uživatel */

    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_UINT ( 7, cfgelement_get_unsigned_value ( e ) );

    /* Připnutí je zrušené: návrat na 5 se už uloží. */
    app_var = 5;
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_UINT ( 5, cfgelement_get_unsigned_value ( e ) );

    cfgroot_destroy ( r );
}


/** @brief Druhý snapshot po prvním zachová původní INI hodnotu. */
void test_pin_save_value_repeated_keeps_original ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );
    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "type", CFGENTYPE_KEYWORD, 0,
                                                        0, "A", 1, "B", 2, "C", -1 );

    int app_var = 0;
    cfgelement_set_handlers ( e, &app_var, &app_var );

    app_var = 1;                        /* snapshot 1 */
    cfgelement_pin_save_value ( e, 0 );
    int before = app_var;
    app_var = 2;                        /* snapshot 2 */
    cfgelement_pin_save_value ( e, ( unsigned ) before );

    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_INT ( 0, cfgelement_get_keyword_value ( e ) );

    /* Snapshot se stejnou hodnotou jako INI připnutí zruší. */
    before = app_var;
    app_var = 0;
    cfgelement_pin_save_value ( e, ( unsigned ) before );
    TEST_ASSERT_EQUAL_INT ( 0, e->save_pin_active );

    cfgroot_destroy ( r );
}


/** @brief Testuje propagate a save pres set_pointers pro TEXT element */
void test_pointer_propagate_and_save ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *e = cfgmodule_register_new_element ( m, "dir", CFGENTYPE_TEXT, "/default" );

    char *app_var = NULL;
    cfgelement_set_pointers ( e, &app_var, &app_var );

    /* Propagate */
    cfgelement_propagate ( e );
    TEST_ASSERT_NOT_NULL ( app_var );
    TEST_ASSERT_EQUAL_STRING ( "/default", app_var );
    free ( app_var );

    /* Save */
    app_var = strdup ( "/custom" );
    open_ini_for_save ( r );
    cfgelement_save ( e );
    close_ini_after_save ( r );
    TEST_ASSERT_EQUAL_STRING ( "/custom", cfgelement_get_text_value ( e ) );
    free ( app_var );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TEST CFGROOT RESET
 * ======================================================================== */

/** @brief Testuje cfgroot_reset — vsechny elementy se vrati na vychozi hodnoty */
void test_root_reset ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );
    st_CFGMODULE *m = cfgroot_register_new_module ( r, "TEST" );

    st_CFGELEMENT *eb = cfgmodule_register_new_element ( m, "flag", CFGENTYPE_BOOL, 1 );
    st_CFGELEMENT *eu = cfgmodule_register_new_element ( m, "addr", CFGENTYPE_UNSIGNED, 0x10, 0, 0xFF );
    st_CFGELEMENT *ef = cfgmodule_register_new_element ( m, "vol", CFGENTYPE_FLOAT, 0.5, 0.0, 1.0 );

    cfgelement_set_bool_value ( eb, 0 );
    cfgelement_set_unsigned_value ( eu, 0xFF );
    cfgelement_set_float_value ( ef, 1.0f );

    cfgroot_reset ( r );

    TEST_ASSERT_EQUAL_INT ( 1, cfgelement_get_bool_value ( eb ) );
    TEST_ASSERT_EQUAL_UINT ( 0x10, cfgelement_get_unsigned_value ( eu ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001f, 0.5f, cfgelement_get_float_value ( ef ) );

    cfgroot_destroy ( r );
}


/* ========================================================================
 * TEST DESTROY — spravne uvolneni pameti (nesmí crashnout)
 * ======================================================================== */

/** @brief Testuje destrukci slozite hierarchie — pokryva fix memory leaku (bug #3, #4, #5) */
void test_destroy_complex ( void ) {
    st_CFGROOT *r = cfgroot_new ( test_ini_file );

    st_CFGMODULE *m1 = cfgroot_register_new_module ( r, "MOD1" );
    cfgmodule_register_new_element ( m1, "a", CFGENTYPE_BOOL, 0 );
    cfgmodule_register_new_element ( m1, "b", CFGENTYPE_TEXT, "text" );
    cfgmodule_register_new_element ( m1, "c", CFGENTYPE_UNSIGNED, 0, 0, 100 );
    cfgmodule_register_new_element ( m1, "d", CFGENTYPE_KEYWORD, 0,
        0, "zero",
        1, "one",
        -1 );
    cfgmodule_register_new_element ( m1, "e", CFGENTYPE_FLOAT, 0.0, -100.0, 100.0 );

    st_CFGMODULE *m2 = cfgroot_register_new_module ( r, "MOD2" );
    cfgmodule_register_new_element ( m2, "x", CFGENTYPE_TEXT, "world" );
    cfgmodule_register_new_element ( m2, "y", CFGENTYPE_FLOAT, 1.0, 0.0, 10.0 );

    /* Musi probhnout bez crashe — pokryva fix memory leaku (bug #3, #4, #5) */
    cfgroot_destroy ( r );
}


/** @brief Testuje destrukci NULL ukazatele — nesmi crashnout */
void test_destroy_null ( void ) {
    /* Destrukce NULL nesmí crashnout */
    cfgroot_destroy ( NULL );
}


/* ========================================================================
 * MAIN
 * ======================================================================== */

/**
 * @brief Hlavni funkce testoveho modulu cfgfile
 * @param argc Pocet argumentu prikazove radky
 * @param argv Argumenty prikazove radky
 * @return Navratovy kod Unity (0 = vsechny testy prosly)
 */
int main ( int argc, char *argv[] ) {

    mztest_parse_args ( argc, argv );
    mztest_init ();

    /* Konstrukce cesty k docasnemu INI souboru — platformne nezavisla */
    test_ini_file = g_build_filename ( g_get_tmp_dir (), "test_cfgfile.ini", NULL );

    UNITY_BEGIN();

    /* Smoke */
    RUN_TEST ( test_root_new_destroy );
    RUN_TEST ( test_module_register_and_find );
    RUN_TEST ( test_element_register_and_find );

    /* Typy */
    RUN_TEST ( test_element_bool );
    RUN_TEST ( test_element_text );
    RUN_TEST ( test_element_unsigned );
    RUN_TEST ( test_element_keyword );
    RUN_TEST ( test_element_float );
    RUN_TEST ( test_element_float_precision );

    /* Bind */
    RUN_TEST ( test_bind_bool );
    RUN_TEST ( test_bind_unsigned );
    RUN_TEST ( test_bind_float );
    RUN_TEST ( test_bind_text );
    RUN_TEST ( test_bind_keyword );
    RUN_TEST ( test_bind_separate_save_only );

    /* By_name wrappery */
    RUN_TEST ( test_by_name_wrappers );

    /* INI parsovani */
    RUN_TEST ( test_ini_parse_bool );
    RUN_TEST ( test_ini_parse_text );
    RUN_TEST ( test_ini_parse_unsigned_hex );
    RUN_TEST ( test_ini_parse_unsigned_hash );
    RUN_TEST ( test_ini_parse_keyword );
    RUN_TEST ( test_ini_parse_float );
    RUN_TEST ( test_ini_parse_float_scientific );
    RUN_TEST ( test_ini_parse_nonexistent_file );
    RUN_TEST ( test_ini_parse_module_case_insensitive );
    RUN_TEST ( test_ini_parse_multiple_modules );
    RUN_TEST ( test_ini_parse_unsigned_out_of_range );
    RUN_TEST ( test_ini_parse_float_out_of_range );

    /* Save/load roundtrip */
    RUN_TEST ( test_ini_save_load_roundtrip );

    /* Regresni testy bugov */
    RUN_TEST ( test_module_propagate_cb_data );

    /* Propagate/save handlery */
    RUN_TEST ( test_handler_propagate_and_save );
    RUN_TEST ( test_pointer_propagate_and_save );
    RUN_TEST ( test_pin_save_value_keeps_ini );
    RUN_TEST ( test_pin_save_value_user_change_wins );
    RUN_TEST ( test_pin_save_value_repeated_keeps_original );

    /* Reset */
    RUN_TEST ( test_root_reset );

    /* Destroy / memory management */
    RUN_TEST ( test_destroy_complex );
    RUN_TEST ( test_destroy_null );

    int result = UNITY_END();

    g_free ( test_ini_file );
    test_ini_file = NULL;

    return result;
}
