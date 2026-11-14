/**
 * @file   test_mzf.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Unit testy knihovny mzf (MZF souborový formát Sharp MZ).
 *
 * Testuje: velikosti struktur, konverzi endianity, čtení/zápis hlavičky
 * a těla přes memory handler, konverzi jmen souborů (Sharp MZ ASCII
 * <-> ASCII), tovární funkci, validaci hlavičky i souboru, lifecycle
 * API (load/save/free), chybové kódy, pretty-print dump, regresní
 * testy opravených bugů (off-by-one, dvojitá konverze, OOB přístup).
 *
 * Linkuje se s TEST_ALL_OBJS (vyžaduje generic_driver, memory_driver,
 * endianity, sharpmz_ascii).
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

#include <string.h>
#include <stdlib.h>

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

/** @brief Handler pro memory-based MZF operace */
static st_HANDLER s_handler;
/** @brief Příznak, zda je handler otevřen */
static int s_handler_open = 0;


/** @brief Příprava před každým testem — vynuluje handler */
void setUp ( void ) {
    memset ( &s_handler, 0, sizeof ( s_handler ) );
    s_handler_open = 0;
}


/** @brief Úklid po každém testu — zavře handler, pokud je otevřen */
void tearDown ( void ) {
    if ( s_handler_open ) {
        generic_driver_close ( &s_handler );
        s_handler_open = 0;
    }
}


/* ========================================================================
 * Pomocné funkce
 * ======================================================================== */

/**
 * @brief Otevře paměťový handler s dostatečnou velikostí pro MZF operace.
 *
 * Používá realloc driver pro automatické zvětšování.
 *
 * @param initial_size Počáteční velikost paměťového bufferu v bajtech
 * @return Ukazatel na handler, nebo NULL při chybě
 */
static st_HANDLER* open_memory_handler ( uint32_t initial_size ) {
    st_HANDLER *h = generic_driver_open_memory ( &s_handler, &g_memory_driver_realloc, initial_size );
    if ( h != NULL ) s_handler_open = 1;
    return h;
}


/**
 * @brief Vytvoří testovací MZF hlavičku s rozumnými hodnotami.
 *
 * @param hdr Výstupní buffer pro hlavičku
 */
static void create_test_header ( st_MZF_HEADER *hdr ) {
    memset ( hdr, 0, sizeof ( st_MZF_HEADER ) );
    hdr->ftype = MZF_FTYPE_OBJ;
    hdr->fsize = 0x1000;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
    memset ( hdr->fname.name, MZF_FNAME_TERMINATOR, MZF_FILE_NAME_LENGTH );
    hdr->fname.name[0] = 'T';
    hdr->fname.name[1] = 'E';
    hdr->fname.name[2] = 'S';
    hdr->fname.name[3] = 'T';
    hdr->fname.terminator = MZF_FNAME_TERMINATOR;
}


/**
 * @brief Vytvoří kompletní MZF v memory handleru (hlavička + tělo vyplněné vzorem).
 *
 * @param body_size Velikost těla MZF souboru v bajtech
 * @return Ukazatel na handler, nebo NULL při chybě
 */
static st_HANDLER* create_test_mzf_in_memory ( uint16_t body_size ) {
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );
    hdr.fsize = body_size;

    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE + body_size );
    if ( !h ) return NULL;

    if ( EXIT_SUCCESS != mzf_write_header ( h, &hdr ) ) return NULL;

    if ( body_size > 0 ) {
        uint8_t *body = (uint8_t*) malloc ( body_size );
        if ( !body ) return NULL;
        for ( uint16_t i = 0; i < body_size; i++ ) {
            body[i] = (uint8_t) ( i & 0xFF );
        }
        int rc = mzf_write_body ( h, body, body_size );
        free ( body );
        if ( EXIT_SUCCESS != rc ) return NULL;
    }

    return h;
}


/* ========================================================================
 * SMOKE testy — velikosti struktur a základní invarianty
 * ======================================================================== */

/** @brief Testuje velikost st_MZF_HEADER — musí být přesně 128 bajtů (MZF specifikace) */
static void test_header_size ( void ) {
    TEST_ASSERT_EQUAL_UINT ( MZF_HEADER_SIZE, sizeof ( st_MZF_HEADER ) );
    TEST_ASSERT_EQUAL_UINT ( 128, sizeof ( st_MZF_HEADER ) );
}


/** @brief Testuje velikost st_MZF_FILENAME — musí být přesně 17 bajtů (16 + terminátor) */
static void test_filename_size ( void ) {
    TEST_ASSERT_EQUAL_UINT ( MZF_FILE_NAME_LENGTH + 1, sizeof ( st_MZF_FILENAME ) );
    TEST_ASSERT_EQUAL_UINT ( 17, sizeof ( st_MZF_FILENAME ) );
}


/** @brief Testuje správné hodnoty konstant MZF_FTYPE_* */
static void test_ftype_constants ( void ) {
    TEST_ASSERT_EQUAL_UINT8 ( 0x01, MZF_FTYPE_OBJ );
    TEST_ASSERT_EQUAL_UINT8 ( 0x02, MZF_FTYPE_BTX );
    TEST_ASSERT_EQUAL_UINT8 ( 0x03, MZF_FTYPE_BSD );
    TEST_ASSERT_EQUAL_UINT8 ( 0x04, MZF_FTYPE_BRD );
    TEST_ASSERT_EQUAL_UINT8 ( 0x05, MZF_FTYPE_RB );
}


/*
 * Konverze endianity — na LE systému beze změny, na BE systému swap.
 * Testuje symetrii (dvojité volání vrátí původní hodnoty).
 */
static void test_header_items_correction_symmetry ( void ) {
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );

    uint16_t orig_fsize = hdr.fsize;
    uint16_t orig_fstrt = hdr.fstrt;
    uint16_t orig_fexec = hdr.fexec;

    /* Dvojité volání = identita */
    mzf_header_items_correction ( &hdr );
    mzf_header_items_correction ( &hdr );

    TEST_ASSERT_EQUAL_UINT16 ( orig_fsize, hdr.fsize );
    TEST_ASSERT_EQUAL_UINT16 ( orig_fstrt, hdr.fstrt );
    TEST_ASSERT_EQUAL_UINT16 ( orig_fexec, hdr.fexec );
}


/*
 * Chybové kódy — mzf_error_string vrací neprázdné řetězce pro všechny kódy
 */
static void test_error_string ( void ) {
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_OK ) );
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_ERROR_IO ) );
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_ERROR_INVALID_HEADER ) );
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_ERROR_NO_FNAME_TERMINATOR ) );
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_ERROR_SIZE_MISMATCH ) );
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( MZF_ERROR_ALLOC ) );
    /* Neznámý kód nesmí vrátit NULL */
    TEST_ASSERT_NOT_NULL ( mzf_error_string ( (en_MZF_ERROR) 99 ) );
}


/* ========================================================================
 * Regresní testy — opravené bugy
 * ======================================================================== */

/*
 * Regrese: mzf_tools_get_fname_length vracela pozici za terminátorem
 * místo skutečné délky. Terminator na pozici 0 → délka musí být 0, ne 1.
 */
static void test_regression_get_fname_length_off_by_one ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    /* Terminátor na pozici 0 → délka = 0 */
    hdr.fname.name[0] = MZF_FNAME_TERMINATOR;
    TEST_ASSERT_EQUAL_UINT8 ( 0, mzf_tools_get_fname_length ( &hdr ) );

    /* Terminátor na pozici 5 → délka = 5 */
    memset ( hdr.fname.name, 'A', MZF_FILE_NAME_LENGTH );
    hdr.fname.name[5] = MZF_FNAME_TERMINATOR;
    TEST_ASSERT_EQUAL_UINT8 ( 5, mzf_tools_get_fname_length ( &hdr ) );

    /* Terminátor na poslední pozici (15) → délka = 15 */
    memset ( hdr.fname.name, 'A', MZF_FILE_NAME_LENGTH );
    hdr.fname.name[15] = MZF_FNAME_TERMINATOR;
    TEST_ASSERT_EQUAL_UINT8 ( 15, mzf_tools_get_fname_length ( &hdr ) );
}


/*
 * Regrese: mzf_tools_set_fname zapisovala mimo hranice pole fname.name[16]
 * (formálně UB). Po opravě: fname.name se vyplní jen do indexu 15,
 * fname.terminator se nastaví explicitně.
 */
static void test_regression_set_fname_no_oob ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0xFF, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "AB" );

    /* Znaky na správných pozicích */
    TEST_ASSERT_NOT_EQUAL ( 0xFF, hdr.fname.name[0] );
    TEST_ASSERT_NOT_EQUAL ( 0xFF, hdr.fname.name[1] );

    /* Zbytek name[] vyplněn terminátory */
    for ( int i = 2; i < MZF_FILE_NAME_LENGTH; i++ ) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE ( MZF_FNAME_TERMINATOR, hdr.fname.name[i],
                                          "Zbývající pozice musí být terminátor" );
    }

    /* Explicitní terminátor struktury */
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FNAME_TERMINATOR, hdr.fname.terminator );
}


/* ========================================================================
 * Testy jména souboru (fname)
 * ======================================================================== */

/*
 * set_fname + get_fname roundtrip — krátké jméno
 */
static void test_fname_roundtrip_short ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "HELLO" );

    char result[MZF_FILE_NAME_LENGTH + 1];
    mzf_tools_get_fname ( &hdr, result );
    TEST_ASSERT_EQUAL_STRING ( "HELLO", result );
}


/*
 * set_fname s plným 16-znakovým jménem
 */
static void test_fname_full_length ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "0123456789ABCDEF" );

    char result[MZF_FILE_NAME_LENGTH + 1];
    mzf_tools_get_fname ( &hdr, result );
    TEST_ASSERT_EQUAL_STRING ( "0123456789ABCDEF", result );
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FILE_NAME_LENGTH, mzf_tools_get_fname_length ( &hdr ) );
}


/*
 * set_fname s příliš dlouhým jménem — musí se oříznout na 16 znaků
 */
static void test_fname_truncation ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "THIS_IS_A_VERY_LONG_FILENAME" );

    TEST_ASSERT_EQUAL_UINT8 ( MZF_FILE_NAME_LENGTH, mzf_tools_get_fname_length ( &hdr ) );

    char result[MZF_FILE_NAME_LENGTH + 1];
    mzf_tools_get_fname ( &hdr, result );
    TEST_ASSERT_EQUAL_STRING ( "THIS_IS_A_VERY_L", result );
}


/*
 * set_fname s prázdným řetězcem → délka = 0
 */
static void test_fname_empty ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );

    mzf_tools_set_fname ( &hdr, "" );

    TEST_ASSERT_EQUAL_UINT8 ( 0, mzf_tools_get_fname_length ( &hdr ) );

    char result[MZF_FILE_NAME_LENGTH + 1];
    mzf_tools_get_fname ( &hdr, result );
    TEST_ASSERT_EQUAL_STRING ( "", result );
}


/*
 * get_fname_length bez terminátoru → vrací MZF_FILE_NAME_LENGTH
 */
static void test_fname_length_no_terminator ( void ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memset ( hdr.fname.name, 'X', MZF_FILE_NAME_LENGTH );

    TEST_ASSERT_EQUAL_UINT8 ( MZF_FILE_NAME_LENGTH, mzf_tools_get_fname_length ( &hdr ) );
}


/* ========================================================================
 * Testy tovární funkce (create_mzfhdr)
 * ======================================================================== */

/*
 * Základní vytvoření hlavičky — všechna pole nastavena správně
 */
static void test_create_mzfhdr_basic ( void ) {
    uint8_t fname[] = "TEST";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr ( MZF_FTYPE_OBJ, 0x1000, 0x1200, 0x1200,
                                                     fname, 4, NULL );
    TEST_ASSERT_NOT_NULL ( hdr );
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FTYPE_OBJ, hdr->ftype );
    TEST_ASSERT_EQUAL_UINT16 ( 0x1000, hdr->fsize );
    TEST_ASSERT_EQUAL_UINT16 ( 0x1200, hdr->fstrt );
    TEST_ASSERT_EQUAL_UINT16 ( 0x1200, hdr->fexec );

    /* Jméno zkopírováno, zbytek terminátory */
    TEST_ASSERT_EQUAL_UINT8 ( 'T', hdr->fname.name[0] );
    TEST_ASSERT_EQUAL_UINT8 ( 'E', hdr->fname.name[1] );
    TEST_ASSERT_EQUAL_UINT8 ( 'S', hdr->fname.name[2] );
    TEST_ASSERT_EQUAL_UINT8 ( 'T', hdr->fname.name[3] );
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FNAME_TERMINATOR, hdr->fname.name[4] );
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FNAME_TERMINATOR, hdr->fname.terminator );

    free ( hdr );
}


/*
 * Vytvoření s NULL komentářem — komentář musí být vynulován
 */
static void test_create_mzfhdr_null_cmnt ( void ) {
    uint8_t fname[] = "X";
    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr ( MZF_FTYPE_BTX, 100, 0, 0,
                                                     fname, 1, NULL );
    TEST_ASSERT_NOT_NULL ( hdr );

    /* Komentář musí být samé nuly */
    uint8_t zeros[MZF_CMNT_LENGTH];
    memset ( zeros, 0, sizeof ( zeros ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( zeros, hdr->cmnt, MZF_CMNT_LENGTH );

    free ( hdr );
}


/*
 * Vytvoření s explicitním komentářem — musí se zkopírovat
 */
static void test_create_mzfhdr_with_cmnt ( void ) {
    uint8_t fname[] = "X";
    uint8_t cmnt[MZF_CMNT_LENGTH];
    memset ( cmnt, 0xAB, sizeof ( cmnt ) );

    st_MZF_HEADER *hdr = mzf_tools_create_mzfhdr ( MZF_FTYPE_OBJ, 100, 0, 0,
                                                     fname, 1, cmnt );
    TEST_ASSERT_NOT_NULL ( hdr );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( cmnt, hdr->cmnt, MZF_CMNT_LENGTH );

    free ( hdr );
}


/* ========================================================================
 * Testy I/O hlavičky přes memory handler
 * ======================================================================== */

/*
 * Roundtrip: zápis hlavičky → čtení hlavičky — všechna pole se zachovají
 */
static void test_header_read_write_roundtrip ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZF_HEADER orig;
    create_test_header ( &orig );
    orig.fsize = 0x2345;
    orig.fstrt = 0xABCD;
    orig.fexec = 0x4567;

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_header ( h, &orig ) );

    st_MZF_HEADER loaded;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_read_header ( h, &loaded ) );

    TEST_ASSERT_EQUAL_UINT8 ( orig.ftype, loaded.ftype );
    TEST_ASSERT_EQUAL_UINT16 ( orig.fsize, loaded.fsize );
    TEST_ASSERT_EQUAL_UINT16 ( orig.fstrt, loaded.fstrt );
    TEST_ASSERT_EQUAL_UINT16 ( orig.fexec, loaded.fexec );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( orig.fname.name, loaded.fname.name, MZF_FILE_NAME_LENGTH );
    TEST_ASSERT_EQUAL_UINT8 ( MZF_FNAME_TERMINATOR, loaded.fname.terminator );
}


/*
 * Roundtrip těla: zápis → čtení — data se zachovají
 */
static void test_body_read_write_roundtrip ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint16_t body_size = 256;
    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE + body_size );
    TEST_ASSERT_NOT_NULL ( h );

    /* Zápis hlavičky (aby tělo začínalo na správném offsetu) */
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );
    hdr.fsize = body_size;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_header ( h, &hdr ) );

    /* Zápis těla s testovacím vzorem */
    uint8_t orig_body[256];
    for ( int i = 0; i < 256; i++ ) orig_body[i] = (uint8_t) i;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_body ( h, orig_body, body_size ) );

    /* Čtení těla */
    uint8_t loaded_body[256];
    memset ( loaded_body, 0, sizeof ( loaded_body ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_read_body ( h, loaded_body, body_size ) );

    TEST_ASSERT_EQUAL_UINT8_ARRAY ( orig_body, loaded_body, body_size );
}


/*
 * Test terminátoru jména přes handler — validní hlavička
 */
static void test_fname_terminator_via_handler ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 16 );
    TEST_ASSERT_NOT_NULL ( h );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_header_test_fname_terminator ( h ) );
}


/*
 * Zápis/čtení hlavičky na nenulovém offsetu
 */
static void test_header_on_offset ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint32_t offset = 64;
    st_HANDLER *h = open_memory_handler ( offset + MZF_HEADER_SIZE );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZF_HEADER orig;
    create_test_header ( &orig );
    orig.fsize = 0xBEEF;

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_header_on_offset ( h, offset, &orig ) );

    st_MZF_HEADER loaded;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_read_header_on_offset ( h, offset, &loaded ) );

    TEST_ASSERT_EQUAL_UINT16 ( orig.fsize, loaded.fsize );
    TEST_ASSERT_EQUAL_UINT8 ( orig.ftype, loaded.ftype );
}


/* ========================================================================
 * Testy validačního API
 * ======================================================================== */

/*
 * Validní hlavička — mzf_header_validate vrátí MZF_OK
 */
static void test_validate_header_ok ( void ) {
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );
    TEST_ASSERT_EQUAL_INT ( MZF_OK, mzf_header_validate ( &hdr ) );
}


/*
 * Hlavička bez terminátoru — vrátí MZF_ERROR_NO_FNAME_TERMINATOR
 */
static void test_validate_header_no_terminator ( void ) {
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );

    /* Vyplnit celé fname pole ne-terminátory */
    memset ( &hdr.fname, 'A', sizeof ( hdr.fname ) );

    TEST_ASSERT_EQUAL_INT ( MZF_ERROR_NO_FNAME_TERMINATOR, mzf_header_validate ( &hdr ) );
}


/*
 * NULL hlavička — vrátí MZF_ERROR_INVALID_HEADER
 */
static void test_validate_header_null ( void ) {
    TEST_ASSERT_EQUAL_INT ( MZF_ERROR_INVALID_HEADER, mzf_header_validate ( NULL ) );
}


/*
 * Validní MZF soubor přes handler — mzf_file_validate vrátí MZF_OK
 */
static void test_validate_file_ok ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint16_t body_size = 64;
    st_HANDLER *h = create_test_mzf_in_memory ( body_size );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( MZF_OK, mzf_file_validate ( h ) );
}


/*
 * MZF soubor s nesouhlasnou velikostí — vrátí MZF_ERROR_SIZE_MISMATCH
 */
static void test_validate_file_size_mismatch ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Vytvoříme MZF s fsize=1000, ale tělo bude kratší */
    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE + 16 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZF_HEADER hdr;
    create_test_header ( &hdr );
    hdr.fsize = 1000; /* deklaruje 1000 bajtů, ale handler má jen 16 navíc */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_header ( h, &hdr ) );

    TEST_ASSERT_EQUAL_INT ( MZF_ERROR_SIZE_MISMATCH, mzf_file_validate ( h ) );
}


/* ========================================================================
 * Testy lifecycle API (load / save / free)
 * ======================================================================== */

/*
 * Roundtrip: save → load — všechna data se zachovají
 */
static void test_lifecycle_save_load_roundtrip ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Příprava st_MZF */
    st_MZF orig;
    create_test_header ( &orig.header );
    orig.header.fsize = 32;
    orig.body_size = 32;
    uint8_t body_data[32];
    for ( int i = 0; i < 32; i++ ) body_data[i] = (uint8_t) ( i * 3 );
    orig.body = body_data;

    /* Uložení do memory handleru */
    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE + 32 );
    TEST_ASSERT_NOT_NULL ( h );

    en_MZF_ERROR err = mzf_save ( h, &orig );
    TEST_ASSERT_EQUAL_INT ( MZF_OK, err );

    /* Načtení zpět */
    st_MZF *loaded = mzf_load ( h, &err );
    TEST_ASSERT_NOT_NULL ( loaded );
    TEST_ASSERT_EQUAL_INT ( MZF_OK, err );

    /* Porovnání */
    TEST_ASSERT_EQUAL_UINT8 ( orig.header.ftype, loaded->header.ftype );
    TEST_ASSERT_EQUAL_UINT16 ( orig.header.fsize, loaded->header.fsize );
    TEST_ASSERT_EQUAL_UINT16 ( orig.header.fstrt, loaded->header.fstrt );
    TEST_ASSERT_EQUAL_UINT16 ( orig.header.fexec, loaded->header.fexec );
    TEST_ASSERT_NOT_NULL ( loaded->body );
    TEST_ASSERT_EQUAL_UINT32 ( 32, loaded->body_size );
    TEST_ASSERT_EQUAL_UINT8_ARRAY ( body_data, loaded->body, 32 );

    mzf_free ( loaded );
}


/*
 * mzf_load s nulovým fsize — body musí být NULL, body_size = 0
 */
static void test_lifecycle_load_empty_body ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 0 );
    TEST_ASSERT_NOT_NULL ( h );

    en_MZF_ERROR err;
    st_MZF *mzf = mzf_load ( h, &err );
    TEST_ASSERT_NOT_NULL ( mzf );
    TEST_ASSERT_EQUAL_INT ( MZF_OK, err );
    TEST_ASSERT_NULL ( mzf->body );
    TEST_ASSERT_EQUAL_UINT32 ( 0, mzf->body_size );

    mzf_free ( mzf );
}


/*
 * mzf_free s NULL — nesmí crashnout
 */
static void test_lifecycle_free_null ( void ) {
    mzf_free ( NULL );
    /* Pokud se sem dostaneme, test prošel */
}


/*
 * mzf_save s NULL — vrátí chybu
 */
static void test_lifecycle_save_null ( void ) {
    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE );
    TEST_ASSERT_NOT_NULL ( h );
    TEST_ASSERT_EQUAL_INT ( MZF_ERROR_INVALID_HEADER, mzf_save ( h, NULL ) );
}


/*
 * mzf_load s err == NULL — nesmí crashnout
 */
static void test_lifecycle_load_null_err ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 8 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZF *mzf = mzf_load ( h, NULL );
    TEST_ASSERT_NOT_NULL ( mzf );

    mzf_free ( mzf );
}


/* ========================================================================
 * Testy pretty-print dump
 * ======================================================================== */

/*
 * mzf_tools_dump_header — nesmí crashnout, musí vypsat něco na fp
 */
static void test_dump_header ( void ) {
    st_MZF_HEADER hdr;
    create_test_header ( &hdr );

    /* Výstup do /dev/null (jen test, že nekrachuje) */
    FILE *fp = fopen ( "/dev/null", "w" );
    if ( fp ) {
        mzf_tools_dump_header ( &hdr, fp );
        fclose ( fp );
    }

    /* NULL argumenty — nesmí crashnout */
    mzf_tools_dump_header ( NULL, stderr );
    mzf_tools_dump_header ( &hdr, NULL );
}


/* ========================================================================
 * main
 * ======================================================================== */

int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ();

    UNITY_BEGIN ();

    /* Smoke testy — velikosti a invarianty */
    RUN_TEST ( test_header_size );
    RUN_TEST ( test_filename_size );
    RUN_TEST ( test_ftype_constants );
    RUN_TEST ( test_header_items_correction_symmetry );
    RUN_TEST ( test_error_string );

    /* Regresní testy — opravené bugy */
    RUN_TEST ( test_regression_get_fname_length_off_by_one );
    RUN_TEST ( test_regression_set_fname_no_oob );

    /* Fname operace */
    RUN_TEST ( test_fname_roundtrip_short );
    RUN_TEST ( test_fname_full_length );
    RUN_TEST ( test_fname_truncation );
    RUN_TEST ( test_fname_empty );
    RUN_TEST ( test_fname_length_no_terminator );

    /* Tovární funkce */
    RUN_TEST ( test_create_mzfhdr_basic );
    RUN_TEST ( test_create_mzfhdr_null_cmnt );
    RUN_TEST ( test_create_mzfhdr_with_cmnt );

    /* I/O hlavičky a těla přes memory handler */
    RUN_TEST ( test_header_read_write_roundtrip );
    RUN_TEST ( test_body_read_write_roundtrip );
    RUN_TEST ( test_fname_terminator_via_handler );
    RUN_TEST ( test_header_on_offset );

    /* Validační API */
    RUN_TEST ( test_validate_header_ok );
    RUN_TEST ( test_validate_header_no_terminator );
    RUN_TEST ( test_validate_header_null );
    RUN_TEST ( test_validate_file_ok );
    RUN_TEST ( test_validate_file_size_mismatch );

    /* Lifecycle API */
    RUN_TEST ( test_lifecycle_save_load_roundtrip );
    RUN_TEST ( test_lifecycle_load_empty_body );
    RUN_TEST ( test_lifecycle_free_null );
    RUN_TEST ( test_lifecycle_save_null );
    RUN_TEST ( test_lifecycle_load_null_err );

    /* Pretty-print */
    RUN_TEST ( test_dump_header );

    int result = UNITY_END ();
    mztest_teardown ();
    return result;
}
