/**
 * @file   test_dsk.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Unit testy knihovny dsk (Extended CPC DSK diskový formát).
 *
 * Testuje: inline konverze (encode/decode sector/track size), výpočty offsetů,
 *          vytváření obrazů (header, stopy, sektory), čtení/zápis sektorů,
 *          sector mapy (normal, interlaced, custom), identifikace formátu,
 *          validace (check_dsk), modifikace (change_track, add_tracks, shrink),
 *          geometrie (dsk_get_geometry), iterace (for_each_track/sector),
 *          logovací callback, regresní testy opravených bugů,
 *          délka dat sektoru v Sector Info (vytvoření, diagnostika, repair).
 *
 * Test vyžaduje emulátorový kontext (memory_driver_realloc).
 * Linkuje se s TEST_ALL_OBJS + Unity.
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

#include "libs/dsk/dsk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"


/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

/** Handler a jeho stav pro memory-based DSK operace */
static st_HANDLER s_handler;
/** Příznak, zda je handler otevřený (pro tearDown úklid) */
static int s_handler_open = 0;

/** @brief Inicializace před každým testem — reset handleru */
void setUp ( void ) {
    memset ( &s_handler, 0, sizeof ( s_handler ) );
    s_handler_open = 0;
}

/** @brief Úklid po každém testu — zavření handleru a reset logu */
void tearDown ( void ) {
    if ( s_handler_open ) {
        generic_driver_close ( &s_handler );
        s_handler_open = 0;
    }
    /* Reset logovacího callbacku */
    dsk_tools_set_log_cb ( NULL, NULL );
}


/* ========================================================================
 * Pomocné funkce
 * ======================================================================== */

/**
 * @brief Otevře paměťový handler s dostatečnou velikostí pro DSK operace.
 *
 * Používá realloc driver pro automatické zvětšování.
 *
 * @param initial_size Počáteční velikost alokovaného bufferu v bajtech.
 * @return Ukazatel na otevřený handler, nebo NULL při chybě.
 */
static st_HANDLER* open_memory_handler ( uint32_t initial_size ) {
    st_HANDLER *h = generic_driver_open_memory ( &s_handler, &g_memory_driver_realloc, initial_size );
    if ( h != NULL ) s_handler_open = 1;
    return h;
}


/**
 * @brief Vytvoří jednoduchý MZ-BASIC DSK obraz (1 pravidlo, 16x256B sektory).
 *
 * @param tracks Počet stop.
 * @param sides  Počet stran (1 nebo 2).
 * @return Ukazatel na handler s vytvořeným obrazem, nebo NULL při chybě.
 */
static st_HANDLER* create_mzbasic_image ( uint8_t tracks, uint8_t sides ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    if ( h == NULL ) return NULL;

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->tracks = tracks;
    desc->sides = sides;
    desc->count_rules = 1;

    dsk_tools_assign_description ( desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) return NULL;
    return h;
}


/**
 * @brief Vytvoří CP/M DSK obraz (3 pravidla: 9x512 + 16x256 boot + 9x512).
 *
 * @param tracks Počet stop.
 * @param sides  Počet stran (1 nebo 2).
 * @return Ukazatel na handler s vytvořeným obrazem, nebo NULL při chybě.
 */
static st_HANDLER* create_cpm_image ( uint8_t tracks, uint8_t sides ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    if ( h == NULL ) return NULL;

    size_t desc_size = dsk_tools_compute_description_size ( 3 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->tracks = tracks;
    desc->sides = sides;
    desc->count_rules = 3;

    dsk_tools_assign_description ( desc, 0, 0, 9, DSK_SECTOR_SIZE_512, DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );
    dsk_tools_assign_description ( desc, 1, 1, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    dsk_tools_assign_description ( desc, 2, 2, 9, DSK_SECTOR_SIZE_512, DSK_SEC_ORDER_INTERLACED_LEC, NULL, 0xE5 );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) return NULL;
    return h;
}


/** Logovací buffer pro zachycení zpráv z dsk_tools */
static char s_log_buffer[2048];
/** Počet zachycených logovacích zpráv */
static int s_log_count;
/** Poslední zachycená úroveň logování */
static int s_last_log_level;

/**
 * @brief Logovací callback pro testování — ukládá zprávy do s_log_buffer.
 *
 * @param level     Úroveň logové zprávy.
 * @param msg       Text logové zprávy.
 * @param user_data Uživatelská data (nepoužito).
 */
static void test_log_cb ( int level, const char *msg, void *user_data ) {
    (void) user_data;
    s_last_log_level = level;
    s_log_count++;
    /* Přidat zprávu do bufferu */
    size_t current_len = strlen ( s_log_buffer );
    size_t remaining = sizeof ( s_log_buffer ) - current_len;
    if ( remaining > 1 ) {
        snprintf ( s_log_buffer + current_len, remaining, "%s\n", msg );
    }
}

/** @brief Resetuje logovací stav (buffer, počítadlo, úroveň) */
static void reset_log ( void ) {
    s_log_buffer[0] = '\0';
    s_log_count = 0;
    s_last_log_level = -1;
}


/* ========================================================================
 * SMOKE TESTY — inline konverze
 * ======================================================================== */

/** @brief Testuje dekódování všech platných velikostí sektorů */
void test_decode_sector_size_all ( void ) {
    TEST_ASSERT_EQUAL_UINT16 ( 128, dsk_decode_sector_size ( DSK_SECTOR_SIZE_128 ) );
    TEST_ASSERT_EQUAL_UINT16 ( 256, dsk_decode_sector_size ( DSK_SECTOR_SIZE_256 ) );
    TEST_ASSERT_EQUAL_UINT16 ( 512, dsk_decode_sector_size ( DSK_SECTOR_SIZE_512 ) );
    TEST_ASSERT_EQUAL_UINT16 ( 1024, dsk_decode_sector_size ( DSK_SECTOR_SIZE_1024 ) );
}

/** @brief Testuje kódování velikostí sektorů včetně neplatných hodnot */
void test_encode_sector_size_roundtrip ( void ) {
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_128, dsk_encode_sector_size ( 128 ) );
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_256, dsk_encode_sector_size ( 256 ) );
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_512, dsk_encode_sector_size ( 512 ) );
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_1024, dsk_encode_sector_size ( 1024 ) );
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_INVALID, dsk_encode_sector_size ( 333 ) );
    TEST_ASSERT_EQUAL_INT ( DSK_SECTOR_SIZE_INVALID, dsk_encode_sector_size ( 0 ) );
}

/** @brief Testuje kódování a dekódování velikosti stopy (roundtrip) */
void test_encode_decode_track_size ( void ) {
    /* 16 sektorů × 256 B = 4096 B dat + 256 B hlavička = 4352 B → tsize = 17 */
    uint8_t tsize = dsk_encode_track_size ( 16, DSK_SECTOR_SIZE_256 );
    TEST_ASSERT_EQUAL_UINT8 ( 17, tsize );
    TEST_ASSERT_EQUAL_UINT16 ( 17 * 256, dsk_decode_track_size ( tsize ) );

    /* 0 sektorů → tsize = 0 */
    TEST_ASSERT_EQUAL_UINT8 ( 0, dsk_encode_track_size ( 0, DSK_SECTOR_SIZE_512 ) );
}

/** @brief Testuje výpočet velikosti DSK popisu pro různý počet pravidel */
void test_compute_description_size ( void ) {
    size_t s1 = dsk_tools_compute_description_size ( 1 );
    size_t s3 = dsk_tools_compute_description_size ( 3 );
    TEST_ASSERT_EQUAL ( sizeof ( st_DSK_DESCRIPTION ) + sizeof ( st_DSK_DESCRIPTION_RULE ), s1 );
    TEST_ASSERT_EQUAL ( sizeof ( st_DSK_DESCRIPTION ) + 3 * sizeof ( st_DSK_DESCRIPTION_RULE ), s3 );
    TEST_ASSERT_TRUE ( s3 > s1 );
}

/** @brief Testuje chybové zprávy DSK pro různé chybové kódy */
void test_error_message ( void ) {
    st_HANDLER h;
    st_DRIVER d;
    memset ( &h, 0, sizeof ( h ) );
    memset ( &d, 0, sizeof ( d ) );
    h.driver = &d;

    h.err = (en_HANDLER_ERROR) DSK_ERROR_NONE;
    TEST_ASSERT_NOT_NULL ( dsk_error_message ( &h, &d ) );

    h.err = (en_HANDLER_ERROR) DSK_ERROR_TRACK_NOT_FOUND;
    const char *msg = dsk_error_message ( &h, &d );
    TEST_ASSERT_NOT_NULL ( msg );
    TEST_ASSERT_TRUE ( strlen ( msg ) > 0 );

    h.err = (en_HANDLER_ERROR) DSK_ERROR_UNKNOWN;
    TEST_ASSERT_NOT_NULL ( dsk_error_message ( &h, &d ) );
}


/* ========================================================================
 * TESTY — vytváření obrazů
 * ======================================================================== */

/** @brief Testuje vytvoření jednoduchého MZ-BASIC DSK obrazu */
void test_create_simple_mzbasic_image ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 40, 2 );
    TEST_ASSERT_NOT_NULL_MESSAGE ( h, "Nelze vytvořit MZ-BASIC obraz" );

    /* Ověřit fileinfo */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_check_dsk_fileinfo ( h ) );

    /* Ověřit image info */
    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 40, iinfo.tracks );
    TEST_ASSERT_EQUAL_UINT8 ( 2, iinfo.sides );
}

/** @brief Testuje přítomnost správné magické sekvence v hlavičce DSK obrazu */
void test_create_image_header_magic ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Přečíst prvních 34 bajtů a ověřit magii */
    uint8_t magic[DSK_FILEINFO_FIELD_LENGTH];
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, 0, magic, DSK_FILEINFO_FIELD_LENGTH ) );
    TEST_ASSERT_EQUAL_MEMORY ( DSK_DEFAULT_FILEINFO, magic, DSK_FILEINFO_FIELD_LENGTH );
}

/** @brief Testuje selhání vytvoření obrazu s 0 stopami */
void test_create_image_no_tracks_fails ( void ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    TEST_ASSERT_NOT_NULL ( h );

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->tracks = 0; /* 0 stop */
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description ( desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_create_image ( h, desc ) );
    free ( desc );
}

/** @brief Testuje selhání vytvoření obrazu s 0 pravidly */
void test_create_image_no_rules_fails ( void ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    TEST_ASSERT_NOT_NULL ( h );

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    memset ( desc, 0, desc_size );
    desc->tracks = 10;
    desc->sides = 1;
    desc->count_rules = 0; /* 0 pravidel */

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_create_image ( h, desc ) );
    free ( desc );
}

/** @brief Testuje vytvoření CP/M DSK obrazu se 3 pravidly a ověření track info */
void test_create_cpm_image ( void ) {
    st_HANDLER *h = create_cpm_image ( 40, 2 );
    TEST_ASSERT_NOT_NULL_MESSAGE ( h, "Nelze vytvořit CP/M obraz" );

    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 40, iinfo.tracks );
    TEST_ASSERT_EQUAL_UINT8 ( 2, iinfo.sides );

    /* Stopa 0: 9×512B, stopa 1: 16×256B (boot), stopa 2: 9×512B */
    st_DSK_SHORT_TRACK_INFO tinfo;

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_track_info ( h, &iinfo, 0, &tinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 9, tinfo.sectors );
    TEST_ASSERT_EQUAL_UINT8 ( DSK_SECTOR_SIZE_512, tinfo.ssize );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_track_info ( h, &iinfo, 1, &tinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 16, tinfo.sectors );
    TEST_ASSERT_EQUAL_UINT8 ( DSK_SECTOR_SIZE_256, tinfo.ssize );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_track_info ( h, &iinfo, 2, &tinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 9, tinfo.sectors );
    TEST_ASSERT_EQUAL_UINT8 ( DSK_SECTOR_SIZE_512, tinfo.ssize );
}


/* ========================================================================
 * TESTY — čtení/zápis sektorů
 * ======================================================================== */

/** @brief Testuje zápis a zpětné čtení sektoru (roundtrip) */
void test_read_write_sector_roundtrip ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Zapsat data do sektoru 1 na stopě 0 */
    uint8_t write_buf[256];
    memset ( write_buf, 0, sizeof ( write_buf ) );
    write_buf[0] = 0xDE;
    write_buf[1] = 0xAD;
    write_buf[255] = 0x42;

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_write_sector ( h, 0, 1, write_buf ) );

    /* Přečíst zpět */
    uint8_t read_buf[256];
    memset ( read_buf, 0, sizeof ( read_buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_sector ( h, 0, 1, read_buf ) );

    TEST_ASSERT_EQUAL_MEMORY ( write_buf, read_buf, 256 );
}

/** @brief Testuje selhání čtení sektoru z neplatné stopy */
void test_read_sector_invalid_track ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t buf[256];
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_read_sector ( h, 99, 1, buf ) );
}

/** @brief Testuje selhání čtení neexistujícího sektoru */
void test_read_sector_invalid_sector ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t buf[256];
    /* Sektor 99 neexistuje (máme 1-16) */
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_read_sector ( h, 0, 99, buf ) );
}

/** @brief Testuje čtení/zápis sektoru s cachovaným image/track info */
void test_rw_sector_with_cached_info ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Načíst image/track info a použít pro cachovaný přístup */
    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );

    st_DSK_SHORT_TRACK_INFO tinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_track_info ( h, &iinfo, 0, &tinfo ) );

    uint8_t write_buf[256];
    memset ( write_buf, 0xAB, sizeof ( write_buf ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_rw_sector ( h, DSK_RWOP_WRITE, &iinfo, &tinfo, 0, 1, write_buf ) );

    uint8_t read_buf[256];
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_rw_sector ( h, DSK_RWOP_READ, &iinfo, &tinfo, 0, 1, read_buf ) );
    TEST_ASSERT_EQUAL_MEMORY ( write_buf, read_buf, 256 );
}


/* ========================================================================
 * TESTY — sector mapy
 * ======================================================================== */

/** @brief Testuje vytvoření sector mapy v normálním (sekvenčním) pořadí */
void test_make_sector_map_normal ( void ) {
    uint8_t map[16];
    dsk_tools_make_sector_map ( 16, DSK_SEC_ORDER_NORMAL, map );

    /* Normal: 1, 2, 3, ..., 16 */
    uint8_t i;
    for ( i = 0; i < 16; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( i + 1, map[i] );
    }
}

/** @brief Testuje vytvoření prokládané sector mapy (interlaced LEC) */
void test_make_sector_map_interlaced ( void ) {
    uint8_t map[9];
    dsk_tools_make_sector_map ( 9, DSK_SEC_ORDER_INTERLACED_LEC, map );

    /* Interlaced (2 skupiny prokládané): liché, pak sudé */
    /* Očekáváme: 1, 6, 2, 7, 3, 8, 4, 9, 5 */
    TEST_ASSERT_EQUAL_UINT8 ( 1, map[0] );
    TEST_ASSERT_EQUAL_UINT8 ( 6, map[1] );
    /* Ověříme alespoň že všechna čísla 1-9 jsou přítomná */
    uint8_t found[10] = {0};
    uint8_t i;
    for ( i = 0; i < 9; i++ ) {
        TEST_ASSERT_TRUE ( map[i] >= 1 && map[i] <= 9 );
        found[map[i]] = 1;
    }
    for ( i = 1; i <= 9; i++ ) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 1, found[i], "Chybí sektor v interlaced mapě" );
    }
}

/** @brief Testuje vytvoření prokládané sector mapy pro HD (18 sektorů) */
void test_make_sector_map_interlaced_hd ( void ) {
    uint8_t map[18];
    dsk_tools_make_sector_map ( 18, DSK_SEC_ORDER_INTERLACED_LEC_HD, map );

    /* Ověříme že všechna čísla 1-18 jsou přítomná */
    uint8_t found[19] = {0};
    uint8_t i;
    for ( i = 0; i < 18; i++ ) {
        TEST_ASSERT_TRUE ( map[i] >= 1 && map[i] <= 18 );
        found[map[i]] = 1;
    }
    for ( i = 1; i <= 18; i++ ) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 1, found[i], "Chybí sektor v HD interlaced mapě" );
    }
}

/** @brief Testuje fallback CUSTOM sector mapy na normální pořadí */
void test_make_sector_map_custom_fallback ( void ) {
    /* CUSTOM bez mapy → má se chovat jako NORMAL */
    uint8_t map[8];
    dsk_tools_make_sector_map ( 8, DSK_SEC_ORDER_CUSTOM, map );

    uint8_t i;
    for ( i = 0; i < 8; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( i + 1, map[i] );
    }
}


/* ========================================================================
 * TESTY — identifikace formátu
 * ======================================================================== */

/** @brief Testuje identifikaci formátu MZ-BASIC obrazu */
void test_identformat_mzbasic ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 40, 2 );
    TEST_ASSERT_NOT_NULL ( h );

    en_DSK_TOOLS_IDENTFORMAT fmt;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_identformat ( h, &fmt ) );
    TEST_ASSERT_EQUAL_INT ( DSK_TOOLS_IDENTFORMAT_MZBASIC, fmt );
}

/** @brief Testuje identifikaci formátu MZ-CP/M obrazu */
void test_identformat_mzcpm ( void ) {
    st_HANDLER *h = create_cpm_image ( 40, 2 );
    TEST_ASSERT_NOT_NULL ( h );

    en_DSK_TOOLS_IDENTFORMAT fmt;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_identformat ( h, &fmt ) );
    TEST_ASSERT_EQUAL_INT ( DSK_TOOLS_IDENTFORMAT_MZCPM, fmt );
}

/** @brief Regresní test BUG-3: identformat_from_tracks_rules s NULL vrátí UNKNOWN */
void test_identformat_null_tracks_rules ( void ) {
    /* Regresní test BUG-3: identformat_from_tracks_rules s NULL → UNKNOWN */
    en_DSK_TOOLS_IDENTFORMAT fmt = dsk_tools_identformat_from_tracks_rules ( NULL );
    TEST_ASSERT_EQUAL_INT ( DSK_TOOLS_IDENTFORMAT_UNKNOWN, fmt );
}


/* ========================================================================
 * TESTY — validace (check_dsk)
 * ======================================================================== */

/** @brief Testuje validaci (check_dsk) na platném DSK obrazu */
void test_check_dsk_valid_image ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Nastavit logovací callback */
    reset_log ();
    dsk_tools_set_log_cb ( test_log_cb, NULL );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_check_dsk ( h, 1, 0 ) );
    TEST_ASSERT_TRUE ( s_log_count > 0 );  /* měly se logovat zprávy */
}

/** @brief Testuje selhání validace fileinfo na poškozených datech */
void test_check_dsk_fileinfo_invalid ( void ) {
    st_HANDLER *h = open_memory_handler ( 512 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Zapsat nesmyslná data */
    uint8_t garbage[256];
    memset ( garbage, 0xAA, sizeof ( garbage ) );
    generic_driver_write ( h, 0, garbage, sizeof ( garbage ) );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_check_dsk_fileinfo ( h ) );
}

/** @brief Testuje čtení pole creator z DSK hlavičky */
void test_get_dsk_creator ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t creator[DSK_CREATOR_FIELD_LENGTH + 1];
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_get_dsk_creator ( h, creator ) );

    /* Creator by měl začínat "DSKLib" */
    creator[DSK_CREATOR_FIELD_LENGTH] = '\0';
    TEST_ASSERT_TRUE ( memcmp ( creator, "DSKLib", 6 ) == 0 );
}


/* ========================================================================
 * TESTY — modifikace (shrink, tracks rules)
 * ======================================================================== */

/** @brief Testuje zmenšení DSK obrazu (shrink) z 20 na 10 stop */
void test_shrink_image ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_mzbasic_image ( 20, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 20, iinfo.tracks );

    /* Zmenšit na 10 stop */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_shrink_image ( h, &iinfo, 10 ) );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_UINT8 ( 10, iinfo.tracks );
}

/** @brief Testuje selhání shrink na 0 stop */
void test_shrink_image_zero_tracks_fails ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_shrink_image ( h, NULL, 0 ) );
}

/** @brief Regresní test BUG-2: shrink s truncate_cb=NULL nesmí crashnout */
void test_shrink_image_null_truncate_cb ( void ) {
    /* Regresní test BUG-2: truncate_cb = NULL nesmí crashnout */
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Dočasně nastavit truncate_cb na NULL */
    st_DRIVER *d = (st_DRIVER *) h->driver;
    generic_driver_truncate_cb orig_truncate = d->truncate_cb;
    d->truncate_cb = NULL;

    /* Musí vrátit FAILURE, ale nesmí crashnout */
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_shrink_image ( h, NULL, 5 ) );

    /* Obnovit */
    d->truncate_cb = orig_truncate;
}

/** @brief Testuje získání pravidel stop (tracks rules) z CP/M obrazu */
void test_get_tracks_rules ( void ) {
    st_HANDLER *h = create_cpm_image ( 40, 2 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_TOOLS_TRACKS_RULES_INFO *rules = dsk_tools_get_tracks_rules ( h );
    TEST_ASSERT_NOT_NULL ( rules );
    TEST_ASSERT_EQUAL_UINT8 ( 80, rules->total_tracks ); /* 40 × 2 */
    TEST_ASSERT_EQUAL_UINT8 ( 2, rules->sides );
    TEST_ASSERT_TRUE ( rules->count_rules >= 3 );  /* min 3 pravidla pro CP/M */

    /* Stopa 0: 9 sektorů */
    st_DSK_TOOLS_TRACK_RULE_INFO *r = dsk_tools_get_rule_for_track ( rules, 0 );
    TEST_ASSERT_NOT_NULL ( r );
    TEST_ASSERT_EQUAL_UINT8 ( 9, r->sectors );

    dsk_tools_destroy_track_rules ( rules );
}


/* ========================================================================
 * TESTY — geometrie
 * ======================================================================== */

/** @brief Testuje získání geometrie DSK obrazu (stopy, strany, velikost dat) */
void test_get_geometry ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_GEOMETRY geom;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_get_geometry ( h, &geom ) );
    TEST_ASSERT_EQUAL_UINT8 ( 10, geom.tracks );
    TEST_ASSERT_EQUAL_UINT8 ( 1, geom.sides );
    TEST_ASSERT_EQUAL_UINT8 ( 10, geom.total_tracks );
    TEST_ASSERT_TRUE ( geom.total_data_bytes > 0 );
    TEST_ASSERT_TRUE ( geom.image_size > sizeof ( st_DSK_HEADER ) );

    /* Data = 10 stop × 16 sektorů × 256 B = 40960 B */
    TEST_ASSERT_EQUAL_UINT32 ( 10 * 16 * 256, geom.total_data_bytes );
}

/** @brief Testuje selhání dsk_get_geometry s NULL výstupním parametrem */
void test_get_geometry_null_param ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_get_geometry ( h, NULL ) );
}


/* ========================================================================
 * TESTY — iterace
 * ======================================================================== */

/** Počítadlo pro track callback */
static int s_track_count;

/**
 * @brief Callback pro počítání stop při iteraci.
 *
 * @param h         Handler DSK obrazu.
 * @param abstrack  Absolutní číslo stopy.
 * @param tinfo     Informace o stopě.
 * @param user_data Ukazatel na počítadlo (int*).
 * @return 0 pro pokračování iterace.
 */
static int count_tracks_cb ( st_HANDLER *h, uint8_t abstrack, const st_DSK_SHORT_TRACK_INFO *tinfo, void *user_data ) {
    (void) h;
    (void) abstrack;
    (void) tinfo;
    int *count = (int *) user_data;
    (*count)++;
    return 0;
}

/** @brief Testuje iteraci přes všechny stopy jednostranného obrazu */
void test_for_each_track ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    s_track_count = 0;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_for_each_track ( h, count_tracks_cb, &s_track_count ) );
    TEST_ASSERT_EQUAL_INT ( 10, s_track_count );
}

/** @brief Testuje iteraci přes stopy oboustranného obrazu (tracks x 2) */
void test_for_each_track_double_sided ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 2 );
    TEST_ASSERT_NOT_NULL ( h );

    s_track_count = 0;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_for_each_track ( h, count_tracks_cb, &s_track_count ) );
    TEST_ASSERT_EQUAL_INT ( 20, s_track_count );  /* 10 × 2 */
}

/** Počítadlo pro sector callback */
static int s_sector_count;
/** Celkový součet velikostí sektorů v bajtech */
static uint32_t s_total_sector_bytes;

/**
 * @brief Callback pro počítání sektorů a součet jejich velikostí.
 *
 * @param h              Handler DSK obrazu.
 * @param abstrack       Absolutní číslo stopy.
 * @param sector_idx     Index sektoru v rámci stopy.
 * @param sector_id      ID sektoru.
 * @param sector_offset  Offset sektoru v obrazu.
 * @param sector_size    Velikost sektoru v bajtech.
 * @param user_data      Uživatelská data (nepoužito).
 * @return 0 pro pokračování iterace.
 */
static int count_sectors_cb ( st_HANDLER *h, uint8_t abstrack, uint8_t sector_idx, uint8_t sector_id, uint32_t sector_offset, uint16_t sector_size, void *user_data ) {
    (void) h;
    (void) abstrack;
    (void) sector_idx;
    (void) sector_id;
    (void) sector_offset;
    (void) user_data;
    s_sector_count++;
    s_total_sector_bytes += sector_size;
    return 0;
}

/** @brief Testuje iteraci přes všechny sektory jedné stopy */
void test_for_each_sector ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 10, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    s_sector_count = 0;
    s_total_sector_bytes = 0;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_for_each_sector ( h, 0, count_sectors_cb, NULL ) );
    TEST_ASSERT_EQUAL_INT ( 16, s_sector_count );
    TEST_ASSERT_EQUAL_UINT32 ( 16 * 256, s_total_sector_bytes );
}

/** @brief Testuje selhání iterace sektorů na neplatné stopě */
void test_for_each_sector_invalid_track ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_for_each_sector ( h, 99, count_sectors_cb, NULL ) );
}

/**
 * @brief Callback, který zastaví iteraci po 3. sektoru (vrátí nenulovou hodnotu).
 *
 * @param h              Handler DSK obrazu.
 * @param abstrack       Absolutní číslo stopy.
 * @param sector_idx     Index sektoru v rámci stopy.
 * @param sector_id      ID sektoru.
 * @param sector_offset  Offset sektoru v obrazu.
 * @param sector_size    Velikost sektoru v bajtech.
 * @param user_data      Ukazatel na počítadlo (int*).
 * @return 42 po 3. sektoru, jinak 0.
 */
static int stop_after_3_cb ( st_HANDLER *h, uint8_t abstrack, uint8_t sector_idx, uint8_t sector_id, uint32_t sector_offset, uint16_t sector_size, void *user_data ) {
    (void) h;
    (void) abstrack;
    (void) sector_id;
    (void) sector_offset;
    (void) sector_size;
    int *count = (int *) user_data;
    (*count)++;
    if ( sector_idx >= 2 ) return 42;  /* zastavit po 3. sektoru */
    return 0;
}

/** @brief Testuje předčasné zastavení iterace sektorů callbackem */
void test_for_each_sector_early_stop ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    int count = 0;
    int ret = dsk_for_each_sector ( h, 0, stop_after_3_cb, &count );
    TEST_ASSERT_EQUAL_INT ( 42, ret );  /* návratová hodnota z callbacku */
    TEST_ASSERT_EQUAL_INT ( 3, count );  /* callback zavolán 3× */
}


/* ========================================================================
 * TESTY — logovací callback
 * ======================================================================== */

/** @brief Testuje volání logovacího callbacku při check_dsk */
void test_log_callback ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    reset_log ();
    dsk_tools_set_log_cb ( test_log_cb, NULL );

    /* check_dsk s print_info=1 by měl generovat logové zprávy */
    dsk_tools_check_dsk ( h, 1, 0 );
    TEST_ASSERT_TRUE ( s_log_count > 0 );
    TEST_ASSERT_TRUE ( strlen ( s_log_buffer ) > 0 );
}

/** @brief Testuje, že bez logovacího callbacku nedojde ke crash */
void test_log_callback_null_is_silent ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Bez callbacku — nesmí crashnout */
    dsk_tools_set_log_cb ( NULL, NULL );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_check_dsk ( h, 1, 0 ) );
}


/* ========================================================================
 * TESTY — input validace
 * ======================================================================== */

/** @brief Testuje výpočet offsetu stopy s NULL polem velikostí */
void test_compute_track_offset_null_tsizes ( void ) {
    /* S NULL tsizes vrátí offset hlavičky (sizeof(st_DSK_HEADER)) */
    uint32_t offset = dsk_compute_track_offset ( 5, NULL );
    TEST_ASSERT_EQUAL_UINT32 ( sizeof ( st_DSK_HEADER ), offset );
}

/** @brief Testuje výpočet offsetu sektoru s NULL track info */
void test_compute_sector_offset_null_tinfo ( void ) {
    int32_t offset = dsk_compute_sector_offset ( 1, NULL );
    TEST_ASSERT_EQUAL_INT32 ( -1, offset );
}

/** @brief Testuje selhání čtení image info s NULL výstupem */
void test_read_short_image_info_null_output ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_read_short_image_info ( h, NULL ) );
}

/** @brief Testuje selhání čtení/zápisu sektoru s NULL bufferem */
void test_rw_sector_null_buffer ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_rw_sector ( h, DSK_RWOP_READ, NULL, NULL, 0, 1, NULL ) );
}

/** @brief Testuje, že assign_description s NULL popisem nezpůsobí crash */
void test_assign_description_null ( void ) {
    /* Nesmí crashnout s NULL desc */
    dsk_tools_assign_description ( NULL, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF );
    /* Pokud sem dojdeme, test prošel */
}

/** @brief Testuje, že make_sector_map s NULL mapou nezpůsobí crash */
void test_make_sector_map_null ( void ) {
    /* Nesmí crashnout s NULL sector_map */
    dsk_tools_make_sector_map ( 8, DSK_SEC_ORDER_NORMAL, NULL );
}

/** @brief Testuje selhání vytvoření obrazu s NULL parametry */
void test_create_image_null_params ( void ) {
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, dsk_tools_create_image ( NULL, NULL ) );
}


/* ========================================================================
 * TESTY — per-sector size v short_track_info
 * ======================================================================== */

/** @brief Testuje, že short_track_info obsahuje správné per-sector velikosti */
void test_short_track_info_has_sector_sizes ( void ) {
    st_HANDLER *h = create_mzbasic_image ( 5, 1 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_SHORT_TRACK_INFO tinfo;
    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_track_info ( h, &iinfo, 0, &tinfo ) );

    /* V MZ-BASIC obrazu všechny sektory mají stejnou velikost (256B = DSK_SECTOR_SIZE_256) */
    uint8_t i;
    for ( i = 0; i < tinfo.sectors; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( DSK_SECTOR_SIZE_256, tinfo.sector_sizes[i] );
    }
}


/* ========================================================================
 * TESTY — délka dat sektoru v Sector Info (offset 6-7)
 *
 * Extended CPC DSK vyžaduje u každého sektoru skutečnou délku dat.
 * Knihovna dsk_tools < 3.4.0 pole nechávala nulové. Testy převzaty
 * z projektu mzdisk (tests/libs/test_dsk_tools.c).
 * ======================================================================== */

/**
 * @brief Vytvoří DSK obraz s jedním pravidlem geometrie pro všechny stopy.
 *
 * @param tracks  Počet stop na stranu.
 * @param sides   Počet stran (1 nebo 2).
 * @param sectors Počet sektorů na stopě.
 * @param ssize   Kódovaná velikost sektoru.
 * @param filler  Bajt, kterým se vyplní data sektorů.
 * @return Ukazatel na handler s vytvořeným obrazem, nebo NULL při chybě.
 *         Handler zavírá tearDown().
 */
static st_HANDLER* create_uniform_image ( uint8_t tracks, uint8_t sides, uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint8_t filler ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    if ( h == NULL ) return NULL;

    size_t desc_size = dsk_tools_compute_description_size ( 1 );
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *) malloc ( desc_size );
    if ( desc == NULL ) return NULL;
    memset ( desc, 0, desc_size );
    desc->tracks = tracks;
    desc->sides = sides;
    desc->count_rules = 1;

    dsk_tools_assign_description ( desc, 0, 0, sectors, ssize, DSK_SEC_ORDER_NORMAL, NULL, filler );

    int ret = dsk_tools_create_image ( h, desc );
    free ( desc );

    if ( ret != EXIT_SUCCESS ) return NULL;
    return h;
}


/**
 * @brief Vynuluje pole délky dat u všech sektorů jedné stopy.
 *
 * Napodobuje obraz vytvořený knihovnou dsk_tools < 3.4.0.
 *
 * @param h        Handler s DSK obrazem.
 * @param abstrack Absolutní číslo stopy.
 * @return EXIT_SUCCESS / EXIT_FAILURE
 */
static int zero_track_data_lengths ( st_HANDLER *h, uint8_t abstrack ) {
    st_DSK_SHORT_IMAGE_INFO iinfo;
    if ( EXIT_SUCCESS != dsk_read_short_image_info ( h, &iinfo ) ) return EXIT_FAILURE;
    uint32_t offset = dsk_compute_track_offset ( abstrack, iinfo.tsize );

    st_DSK_TRACK_INFO trk;
    if ( EXIT_SUCCESS != dsk_read_on_offset ( h, offset, &trk, sizeof ( trk ) ) ) return EXIT_FAILURE;
    int i;
    for ( i = 0; i < trk.sectors; i++ ) {
        dsk_sector_info_set_data_length ( &trk.sinfo[i], 0 );
    }
    return dsk_write_on_offset ( h, offset, &trk, sizeof ( trk ) );
}


/**
 * @brief Ověří délku dat u všech sektorů nově vytvořeného obrazu.
 *
 * Vytvoří oboustranný obraz se dvěma stopami na stranu a zkontroluje
 * pole délky dat přes API i jeho uložení v souboru (little-endian).
 *
 * @param sectors Počet sektorů na stopě.
 * @param ssize   Kódovaná velikost sektoru.
 * @param bytes   Očekávaná délka dat sektoru v bajtech.
 */
static void check_created_data_length ( uint8_t sectors, en_DSK_SECTOR_SIZE ssize, uint16_t bytes ) {
    st_HANDLER *h = create_uniform_image ( 2, 2, sectors, ssize, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );

    int t, i;
    for ( t = 0; t < 4; t++ ) {
        st_DSK_TRACK_HEADER_INFO info;
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_read_track_header_info ( h, (uint8_t) t, &info ) );
        for ( i = 0; i < sectors; i++ ) {
            TEST_ASSERT_EQUAL_UINT16 ( bytes, dsk_sector_info_get_data_length ( &info.sinfo[i] ) );
        }
    }

    /* Uložení je little-endian: nižší bajt na offsetu 6, vyšší na 7 */
    st_DSK_TRACK_INFO trk;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
    const uint8_t *raw = (const uint8_t *) &trk.sinfo[0];
    TEST_ASSERT_EQUAL_UINT8 ( bytes & 0xff, raw[6] );
    TEST_ASSERT_EQUAL_UINT8 ( bytes >> 8, raw[7] );
}

/** @brief Nový obraz se sektory 128 B má vyplněnou délku dat */
void test_create_sets_data_length_128 ( void ) {
    check_created_data_length ( 9, DSK_SECTOR_SIZE_128, 128 );
}

/** @brief Nový obraz se sektory 256 B má vyplněnou délku dat */
void test_create_sets_data_length_256 ( void ) {
    check_created_data_length ( 16, DSK_SECTOR_SIZE_256, 256 );
}

/** @brief Nový obraz se sektory 512 B má vyplněnou délku dat */
void test_create_sets_data_length_512 ( void ) {
    check_created_data_length ( 9, DSK_SECTOR_SIZE_512, 512 );
}

/** @brief Nový obraz se sektory 1024 B má vyplněnou délku dat */
void test_create_sets_data_length_1024 ( void ) {
    check_created_data_length ( 5, DSK_SECTOR_SIZE_1024, 1024 );
}


/** @brief dsk_tools_change_track zapíše délku dat podle nové velikosti sektoru */
void test_change_track_sets_data_length ( void ) {
    st_HANDLER *h = create_uniform_image ( 4, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t map[16];
    int i;
    for ( i = 0; i < 16; i++ ) map[i] = (uint8_t) ( i + 1 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_change_track ( h, NULL, 1, 16, DSK_SECTOR_SIZE_256, map, 0xFF ) );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_read_track_header_info ( h, 1, &info ) );
    for ( i = 0; i < 16; i++ ) {
        TEST_ASSERT_EQUAL_UINT16 ( 256, dsk_sector_info_get_data_length ( &info.sinfo[i] ) );
    }

    /* Sousední stopa zůstává 512 B */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_read_track_header_info ( h, 2, &info ) );
    TEST_ASSERT_EQUAL_UINT16 ( 512, dsk_sector_info_get_data_length ( &info.sinfo[0] ) );
}


/** @brief Diagnostika nově vytvořeného obrazu nehlásí žádný problém */
void test_diag_new_image_is_clean ( void ) {
    st_HANDLER *h = create_uniform_image ( 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_IMAGE_OK, diag->image_flags );
    TEST_ASSERT_EQUAL_INT ( 0, dsk_tools_diag_has_repairable_errors ( diag ) );
    dsk_tools_destroy_diag_result ( diag );
}


/**
 * @brief Nulová délka dat sektoru je detekována jako opravitelná chyba.
 *
 * Nesmí být fatální a nesmí nastavit souhrnný flag TRACK_ERRORS.
 */
void test_diag_detects_zero_data_length ( void ) {
    st_HANDLER *h = create_uniform_image ( 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, zero_track_data_lengths ( h, 3 ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_IMAGE_ZERO_DATALEN, diag->image_flags );
    TEST_ASSERT_EQUAL_INT ( 1, diag->zero_datalen_tracks );
    int t;
    for ( t = 0; t < diag->count_tracks; t++ ) {
        TEST_ASSERT_EQUAL_HEX16 ( ( t == 3 ) ? DSK_DIAG_TRACK_ZERO_DATALEN : DSK_DIAG_TRACK_OK, diag->tracks[t].flags );
    }
    TEST_ASSERT_EQUAL_INT ( 1, dsk_tools_diag_has_repairable_errors ( diag ) );
    TEST_ASSERT_EQUAL_INT ( 0, dsk_tools_diag_has_fatal_errors ( diag ) );
    dsk_tools_destroy_diag_result ( diag );
}


/**
 * @brief Repair doplní nulové délky dat a nic jiného nezmění.
 *
 * Po opravě musí být obraz bajt po bajtu shodný s kopií pořízenou před
 * vynulováním délek (včetně vlastního FDC statusu, ID sektoru a dat).
 */
void test_repair_fills_zero_data_length ( void ) {
    st_HANDLER *h = create_uniform_image ( 5, 2, 9, DSK_SECTOR_SIZE_512, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );

    /* Vlastní FDC status, ID sektoru a data, aby bylo co poškodit */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_set_sector_fdc_status ( h, 3, 2, 0x40, 0x20 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_set_sector_id ( h, 3, 4, 77 ) );
    uint8_t marker[512];
    memset ( marker, 0xAB, sizeof ( marker ) );
    st_DSK_SHORT_IMAGE_INFO iinfo;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_short_image_info ( h, &iinfo ) );
    uint32_t t3 = dsk_compute_track_offset ( 3, iinfo.tsize );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_write_on_offset ( h, t3 + sizeof ( st_DSK_TRACK_INFO ), marker, sizeof ( marker ) ) );

    /* Referenční kopie správného obrazu */
    uint32_t size = (uint32_t) h->spec.memspec.size;
    uint8_t *reference = (uint8_t *) malloc ( size );
    TEST_ASSERT_NOT_NULL ( reference );
    memcpy ( reference, h->spec.memspec.ptr, size );

    /* Poškození: nulové délky na dvou stopách */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, zero_track_data_lengths ( h, 0 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, zero_track_data_lengths ( h, 3 ) );
    int damaged = ( 0 != memcmp ( reference, h->spec.memspec.ptr, size ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    int diag_ok = ( diag != NULL ) && ( diag->zero_datalen_tracks == 2 );
    int repair_ret = ( diag != NULL ) ? dsk_tools_repair ( h, diag ) : EXIT_FAILURE;
    dsk_tools_destroy_diag_result ( diag );

    /* Po opravě je obraz bajt po bajtu shodný s referencí */
    int same_size = ( size == (uint32_t) h->spec.memspec.size );
    int same = same_size && ( 0 == memcmp ( reference, h->spec.memspec.ptr, size ) );
    free ( reference );

    TEST_ASSERT_TRUE ( damaged );
    TEST_ASSERT_TRUE ( diag_ok );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, repair_ret );
    TEST_ASSERT_TRUE ( same_size );
    TEST_ASSERT_TRUE ( same );

    diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_IMAGE_OK, diag->image_flags );
    dsk_tools_destroy_diag_result ( diag );
}


/**
 * @brief Nenulová délka dat se při opravě nepřepisuje.
 *
 * Délka odlišná od velikosti dané polem N není diagnostikou hlášena
 * a repair se jí nedotkne, ani když na téže stopě opravuje jiný sektor.
 */
void test_repair_keeps_nonzero_data_length ( void ) {
    st_HANDLER *h = create_uniform_image ( 2, 1, 9, DSK_SECTOR_SIZE_512, 0xE5 );
    TEST_ASSERT_NOT_NULL ( h );

    st_DSK_TRACK_INFO trk;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
    dsk_sector_info_set_data_length ( &trk.sinfo[1], 300 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_write_on_offset ( h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );

    st_DSK_DIAG_RESULT *diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQUAL_HEX16 ( DSK_DIAG_IMAGE_OK, diag->image_flags );
    dsk_tools_destroy_diag_result ( diag );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_read_on_offset ( h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );
    dsk_sector_info_set_data_length ( &trk.sinfo[5], 0 );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_write_on_offset ( h, sizeof ( st_DSK_HEADER ), &trk, sizeof ( trk ) ) );

    diag = dsk_tools_diagnose ( h );
    TEST_ASSERT_NOT_NULL ( diag );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_repair ( h, diag ) );
    dsk_tools_destroy_diag_result ( diag );

    st_DSK_TRACK_HEADER_INFO info;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, dsk_tools_read_track_header_info ( h, 0, &info ) );
    TEST_ASSERT_EQUAL_UINT16 ( 300, dsk_sector_info_get_data_length ( &info.sinfo[1] ) );
    TEST_ASSERT_EQUAL_UINT16 ( 512, dsk_sector_info_get_data_length ( &info.sinfo[5] ) );
}


/* ========================================================================
 * main
 * ======================================================================== */

int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ();

    UNITY_BEGIN ();

    /* Smoke testy — inline konverze */
    RUN_TEST ( test_decode_sector_size_all );
    RUN_TEST ( test_encode_sector_size_roundtrip );
    RUN_TEST ( test_encode_decode_track_size );
    RUN_TEST ( test_compute_description_size );
    RUN_TEST ( test_error_message );

    /* Vytváření obrazů */
    RUN_TEST ( test_create_simple_mzbasic_image );
    RUN_TEST ( test_create_image_header_magic );
    RUN_TEST ( test_create_image_no_tracks_fails );
    RUN_TEST ( test_create_image_no_rules_fails );
    RUN_TEST ( test_create_cpm_image );

    /* Čtení/zápis sektorů */
    RUN_TEST ( test_read_write_sector_roundtrip );
    RUN_TEST ( test_read_sector_invalid_track );
    RUN_TEST ( test_read_sector_invalid_sector );
    RUN_TEST ( test_rw_sector_with_cached_info );

    /* Sector mapy */
    RUN_TEST ( test_make_sector_map_normal );
    RUN_TEST ( test_make_sector_map_interlaced );
    RUN_TEST ( test_make_sector_map_interlaced_hd );
    RUN_TEST ( test_make_sector_map_custom_fallback );

    /* Identifikace formátu */
    RUN_TEST ( test_identformat_mzbasic );
    RUN_TEST ( test_identformat_mzcpm );
    RUN_TEST ( test_identformat_null_tracks_rules );

    /* Validace */
    RUN_TEST ( test_check_dsk_valid_image );
    RUN_TEST ( test_check_dsk_fileinfo_invalid );
    RUN_TEST ( test_get_dsk_creator );

    /* Modifikace */
    RUN_TEST ( test_shrink_image );
    RUN_TEST ( test_shrink_image_zero_tracks_fails );
    RUN_TEST ( test_shrink_image_null_truncate_cb );
    RUN_TEST ( test_get_tracks_rules );

    /* Geometrie */
    RUN_TEST ( test_get_geometry );
    RUN_TEST ( test_get_geometry_null_param );

    /* Iterace */
    RUN_TEST ( test_for_each_track );
    RUN_TEST ( test_for_each_track_double_sided );
    RUN_TEST ( test_for_each_sector );
    RUN_TEST ( test_for_each_sector_invalid_track );
    RUN_TEST ( test_for_each_sector_early_stop );

    /* Logovací callback */
    RUN_TEST ( test_log_callback );
    RUN_TEST ( test_log_callback_null_is_silent );

    /* Input validace */
    RUN_TEST ( test_compute_track_offset_null_tsizes );
    RUN_TEST ( test_compute_sector_offset_null_tinfo );
    RUN_TEST ( test_read_short_image_info_null_output );
    RUN_TEST ( test_rw_sector_null_buffer );
    RUN_TEST ( test_assign_description_null );
    RUN_TEST ( test_make_sector_map_null );
    RUN_TEST ( test_create_image_null_params );

    /* Per-sector size */
    RUN_TEST ( test_short_track_info_has_sector_sizes );

    /* Délka dat sektoru v Sector Info */
    RUN_TEST ( test_create_sets_data_length_128 );
    RUN_TEST ( test_create_sets_data_length_256 );
    RUN_TEST ( test_create_sets_data_length_512 );
    RUN_TEST ( test_create_sets_data_length_1024 );
    RUN_TEST ( test_change_track_sets_data_length );
    RUN_TEST ( test_diag_new_image_is_clean );
    RUN_TEST ( test_diag_detects_zero_data_length );
    RUN_TEST ( test_repair_fills_zero_data_length );
    RUN_TEST ( test_repair_keeps_nonzero_data_length );

    int result = UNITY_END ();
    mztest_teardown ();
    return result;
}
