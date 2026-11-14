/**
 * @file   test_mztape.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Unit testy knihovny mztape (konverze MZF na CMT streamy).
 *
 * Testuje: lifecycle (create/destroy), checksums, vstream/bitstream generování,
 * compute_pulses, speed array, format blocks, alokátor callback,
 * error callback, regresní testy opravených bugů.
 *
 * Linkuje se s TEST_ALL_OBJS (vyžaduje generic_driver, memory_driver,
 * mzf, cmt_stream, endianity, cmtspeed).
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
#include <math.h>

#include "libs/mztape/mztape.h"
#include "libs/mzf/mzf.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"


/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

static st_HANDLER s_handler;
static int s_handler_open = 0;


/** @brief Inicializace před každým testem — vynuluje handler a příznak otevření */
void setUp ( void ) {
    memset ( &s_handler, 0, sizeof ( s_handler ) );
    s_handler_open = 0;
}


/** @brief Úklid po každém testu — zavře handler, resetuje alokátor a error callback */
void tearDown ( void ) {
    if ( s_handler_open ) {
        generic_driver_close ( &s_handler );
        s_handler_open = 0;
    }
    /* reset alokátoru a error callbacku na výchozí */
    mztape_set_allocator ( NULL );
    mztape_set_error_callback ( NULL );
}


/* ========================================================================
 * Pomocné funkce
 * ======================================================================== */

/**
 * @brief Otevře paměťový handler s realokačním driverem.
 * @param initial_size počáteční velikost bufferu v bajtech
 * @return ukazatel na handler, nebo NULL při chybě
 */
static st_HANDLER* open_memory_handler ( uint32_t initial_size ) {
    st_HANDLER *h = generic_driver_open_memory ( &s_handler, &g_memory_driver_realloc, initial_size );
    if ( h != NULL ) s_handler_open = 1;
    return h;
}


/**
 * @brief Vytvoří kompletní MZF v memory handleru (hlavička + tělo) pro mztape testy.
 * @param body_size velikost těla MZF v bajtech
 * @return ukazatel na handler, nebo NULL při chybě
 */
static st_HANDLER* create_test_mzf_in_memory ( uint16_t body_size ) {
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    hdr.ftype = 0x01; /* OBJ */
    hdr.fsize = body_size;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    memset ( hdr.fname.name, 0x0D, MZF_FILE_NAME_LENGTH );
    hdr.fname.name[0] = 'T';
    hdr.fname.name[1] = 'S';
    hdr.fname.name[2] = 'T';
    hdr.fname.terminator = 0x0D;

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
 * SMOKE testy
 * ======================================================================== */

/** @brief Testuje vytvoření a zničení MZF tapového objektu */
static void test_mztapemzf_create_destroy ( void ) {
    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );
    TEST_ASSERT_EQUAL_UINT16 ( 64, mztmzf->size );
    TEST_ASSERT_NOT_NULL ( mztmzf->body );

    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje destroy s NULL parametrem (nesmí crashnout) */
static void test_mztapemzf_destroy_null ( void ) {
    mztape_mztmzf_destroy ( NULL );
}


/* ========================================================================
 * UNIT testy
 * ======================================================================== */

/** @brief Testuje správnost header a body checksumů v MZF tapovém objektu */
static void test_mztapemzf_checksums ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 32 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    /* checksums musí být nenulové pro nenulová data */
    TEST_ASSERT_NOT_EQUAL ( 0, mztmzf->chkh );
    TEST_ASSERT_NOT_EQUAL ( 0, mztmzf->chkb );

    /* header checksum nesmí přesáhnout 128*8 = 1024 (počet bitů v hlavičce) */
    TEST_ASSERT_TRUE ( mztmzf->chkh <= 128 * 8 );

    /* body checksum nesmí přesáhnout body_size*8 */
    TEST_ASSERT_TRUE ( mztmzf->chkb <= mztmzf->size * 8 );

    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje vytvoření vstreamu z testovacího MZF */
static void test_create_vstream ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    st_CMT_VSTREAM *vstream = mztape_create_cmt_vstream_from_mztmzf (
        mztmzf, MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_1_1, 44100 );
    TEST_ASSERT_NOT_NULL ( vstream );

    /* vstream musí mít nenulový počet vzorků */
    TEST_ASSERT_TRUE ( cmt_vstream_get_count_scans ( vstream ) > 0 );

    cmt_vstream_destroy ( vstream );
    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje vytvoření bitstreamu přes vstream konverzi */
static void test_create_bitstream ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    st_CMT_STREAM *stream = mztape_create_stream_from_mztapemzf (
        mztmzf, CMTSPEED_1_1, CMT_STREAM_TYPE_BITSTREAM,
        MZTAPE_FORMATSET_MZ800_SANE, 44100 );
    TEST_ASSERT_NOT_NULL ( stream );

    TEST_ASSERT_NOT_NULL ( stream->str.bitstream );

    cmt_stream_destroy ( stream );
    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje wrapper funkci pro vytvoření obou typů streamů (vstream i bitstream) */
static void test_create_stream_wrapper ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 32 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    /* vstream varianta */
    st_CMT_STREAM *vs = mztape_create_stream_from_mztapemzf (
        mztmzf, CMTSPEED_1_1, CMT_STREAM_TYPE_VSTREAM,
        MZTAPE_FORMATSET_MZ800_SANE, 44100 );
    TEST_ASSERT_NOT_NULL ( vs );
    TEST_ASSERT_NOT_NULL ( vs->str.vstream );
    cmt_stream_destroy ( vs );

    /* bitstream varianta */
    st_CMT_STREAM *bs = mztape_create_stream_from_mztapemzf (
        mztmzf, CMTSPEED_1_1, CMT_STREAM_TYPE_BITSTREAM,
        MZTAPE_FORMATSET_MZ800_SANE, 44100 );
    TEST_ASSERT_NOT_NULL ( bs );
    TEST_ASSERT_NOT_NULL ( bs->str.bitstream );
    cmt_stream_destroy ( bs );

    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje compute_pulses — ověření počtu pulzů pro známý MZF a porovnání SANE vs. plný formát */
static void test_compute_pulses ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    uint64_t long_pulses = 0, short_pulses = 0;
    mztape_compute_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, &long_pulses, &short_pulses );

    /* musí být nenulový počet obou typů pulzů */
    TEST_ASSERT_TRUE ( long_pulses > 0 );
    TEST_ASSERT_TRUE ( short_pulses > 0 );

    /* SANE formát má kratší GAP → menší celkový počet pulzů než plný formát */
    uint64_t long_full = 0, short_full = 0;
    mztape_compute_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800, &long_full, &short_full );
    TEST_ASSERT_TRUE ( ( long_full + short_full ) > ( long_pulses + short_pulses ) );

    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief Testuje, že pole g_mztape_speed[] je správně ukončeno terminátorem CMTSPEED_NONE */
static void test_speed_array_terminated ( void ) {
    int i = 0;
    while ( g_mztape_speed[i] != CMTSPEED_NONE ) {
        TEST_ASSERT_TRUE ( cmtspeed_is_valid ( g_mztape_speed[i] ) );
        i++;
        TEST_ASSERT_TRUE_MESSAGE ( i < 100, "g_mztape_speed[] neobsahuje terminátor" );
    }
    /* musí obsahovat alespoň jednu platnou rychlost */
    TEST_ASSERT_TRUE ( i > 0 );
}


/* ========================================================================
 * Testy alokátoru a error callbacku
 * ======================================================================== */

/* počítadla pro vlastní alokátor */
static int s_alloc_count = 0;
static int s_alloc0_count = 0;
static int s_free_count = 0;

/** @brief Testovací alokátor — počítá volání a deleguje na malloc */
static void* test_alloc ( size_t size ) { s_alloc_count++; return malloc ( size ); }
/** @brief Testovací alokátor — počítá volání a deleguje na calloc */
static void* test_alloc0 ( size_t size ) { s_alloc0_count++; return calloc ( 1, size ); }
/** @brief Testovací dealokátor — počítá volání a deleguje na free */
static void  test_free ( void *ptr ) { s_free_count++; free ( ptr ); }


/** @brief Testuje vlastní alokátor — ověření, že se alloc/alloc0/free volají */
static void test_allocator_custom ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_MZTAPE_ALLOCATOR custom = { test_alloc, test_alloc0, test_free };
    mztape_set_allocator ( &custom );

    s_alloc_count = 0;
    s_alloc0_count = 0;
    s_free_count = 0;

    st_HANDLER *h = create_test_mzf_in_memory ( 16 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    /* alloc0 pro strukturu, alloc pro body */
    TEST_ASSERT_TRUE ( s_alloc0_count > 0 );
    TEST_ASSERT_TRUE ( s_alloc_count > 0 );

    mztape_mztmzf_destroy ( mztmzf );

    /* free pro body + strukturu */
    TEST_ASSERT_TRUE ( s_free_count >= 2 );

    mztape_set_allocator ( NULL ); /* reset */
}


/* počítadlo pro error callback */
static int s_error_count = 0;

/** @brief Testovací error callback — počítá volání */
static void test_error_cb ( const char *func, int line, const char *fmt, ... ) {
    (void) func; (void) line; (void) fmt;
    s_error_count++;
}


/** @brief Selhávající alokátor — vždy vrací NULL */
static void* test_alloc_fail ( size_t size ) { (void) size; return NULL; }
/** @brief Selhávající alokátor — vždy vrací NULL */
static void* test_alloc0_fail ( size_t size ) { (void) size; return NULL; }
/** @brief Prázdný dealokátor — nic nedělá */
static void  test_free_noop ( void *ptr ) { (void) ptr; }


/** @brief Testuje vlastní error callback — ověření, že se volá při selhání alokace */
static void test_error_callback ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    mztape_set_error_callback ( test_error_cb );
    s_error_count = 0;

    /* selhávající alokátor → alloc0 vrátí NULL → error callback musí být zavolán */
    st_MZTAPE_ALLOCATOR fail_alloc = { test_alloc_fail, test_alloc0_fail, test_free_noop };
    mztape_set_allocator ( &fail_alloc );

    st_HANDLER *h = open_memory_handler ( 128 + 64 );
    TEST_ASSERT_NOT_NULL ( h );

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NULL ( mztmzf ); /* musí selhat — alloc0 vrátil NULL */
    TEST_ASSERT_TRUE ( s_error_count > 0 ); /* error callback musí být zavolán */

    mztape_set_allocator ( NULL ); /* reset */
    mztape_set_error_callback ( NULL ); /* reset */
}


/* ========================================================================
 * Regresní testy
 * ======================================================================== */

/** @brief Regresní test: destruktor musí uvolnit body (dříve memory leak) */
static void test_destroy_frees_body ( void ) {
    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    /* vlastní alokátor pro počítání free volání */
    st_MZTAPE_ALLOCATOR custom = { test_alloc, test_alloc0, test_free };
    mztape_set_allocator ( &custom );
    s_free_count = 0;

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );
    TEST_ASSERT_NOT_NULL ( mztmzf->body );

    mztape_mztmzf_destroy ( mztmzf );

    /* musí proběhnout minimálně 2 free: body + struktura */
    TEST_ASSERT_TRUE_MESSAGE ( s_free_count >= 2,
        "Destruktor musí uvolnit body i strukturu (regrese: memory leak)" );

    mztape_set_allocator ( NULL );
}


/**
 * @brief Regresní test: chybová cesta nesmí způsobit use-after-free.
 *
 * Dříve mztape_create_mztapemzf() při chybě čtení body volal:
 *   mztape_mztmzf_destroy(mztmzf);  // uvolní strukturu
 *   baseui_tools_mem_free(mztmzf->body);  // use-after-free!
 *
 * Po opravě se body uvolňuje uvnitř destroy.
 */
static void test_create_error_no_use_after_free ( void ) {
    /* Vytvoříme handler s hlavičkou, ale bez dostatečného těla */
    st_MZF_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    hdr.ftype = 0x01;
    hdr.fsize = 1000; /* deklaruje 1000 B těla */
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    memset ( hdr.fname.name, 0x0D, MZF_FILE_NAME_LENGTH );
    hdr.fname.terminator = 0x0D;

    st_HANDLER *h = open_memory_handler ( MZF_HEADER_SIZE + 8 ); /* jen 8 B těla */
    TEST_ASSERT_NOT_NULL ( h );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mzf_write_header ( h, &hdr ) );

    /* Zapíšeme jen 8 bajtů těla — čtení 1000 B selže */
    uint8_t body[8] = {0};
    generic_driver_write ( h, MZF_HEADER_SIZE, body, 8 );

    /* Vlastní alokátor pro sledování */
    st_MZTAPE_ALLOCATOR custom = { test_alloc, test_alloc0, test_free };
    mztape_set_allocator ( &custom );
    mztape_set_error_callback ( test_error_cb );
    s_free_count = 0;
    s_error_count = 0;

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );

    /*
     * Výsledek závisí na implementaci memory driveru — pokud driver
     * vrátí úspěch i pro krátké čtení, mztmzf bude vytvořen.
     * V obou případech nesmí dojít k use-after-free.
     */
    if ( mztmzf ) {
        mztape_mztmzf_destroy ( mztmzf );
    }
    /* Pokud test doběhl sem, use-after-free nenastal */

    mztape_set_allocator ( NULL );
    mztape_set_error_callback ( NULL );
}


/* ========================================================================
 * main
 * ======================================================================== */

/* ========================================================================
 * Vlastní délky pulzů a hlavička CMTSPEED (UniCMT)
 * ======================================================================== */

/** @brief Takt GDG MZ-800 (Hz) - frekvence vstreamu MZF v emulátoru. */
#define TEST_GDG_RATE 17721600

/** @brief Porovná délky pulzů s hodnotami v µs (tolerance 1e-6 µs). */
static void assert_pulses_us ( const st_MZTAPE_PULSES_LENGTH *p, double lh, double ll, double sh, double sl ) {
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, lh / 1e6, p->long_pulse.high );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, ll / 1e6, p->long_pulse.low );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, ( lh + ll ) / 1e6, p->long_pulse.total );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, sh / 1e6, p->short_pulse.high );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, sl / 1e6, p->short_pulse.low );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-12, ( sh + sl ) / 1e6, p->short_pulse.total );
}


/** @brief mztape_pulses_set_us: převod µs -> s, meze, při chybě beze změny. */
static void test_pulses_set_us ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 156, 164, 80, 92 ) );
    assert_pulses_us ( &p, 156, 164, 80, 92 );

    /* desetinné hodnoty projdou */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 470.5, 494.25, 240.125, 278.0 ) );
    assert_pulses_us ( &p, 470.5, 494.25, 240.125, 278.0 );

    /* neplatné hodnoty - *p se nemění */
    st_MZTAPE_PULSES_LENGTH saved = p;
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_pulses_set_us ( &p, 0, 164, 80, 92 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_pulses_set_us ( &p, 156, -1, 80, 92 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_pulses_set_us ( &p, 156, 164, NAN, 92 ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_pulses_set_us ( &p, 156, 164, 80, MZTAPE_PULSE_US_MAX + 1 ) );
    TEST_ASSERT_EQUAL_MEMORY ( &saved, &p, sizeof ( p ) );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, MZTAPE_PULSE_US_MAX, 1, 1, 1 ) );
}


/** @brief mztape_get_speed_pulses: konstanty Intercopy / cmt.com vydělené poměrem. */
static void test_get_speed_pulses ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_1_1, &p ) );
    assert_pulses_us ( &p, 470.330, 494.308, 245.802, 278.204 );

    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_3_1, &p ) );
    assert_pulses_us ( &p, 470.330 / 3, 494.308 / 3, 245.802 / 3, 278.204 / 3 );

    /* 2:1 CP/M má jiný tvar pulzu (cmt.com), ne jen jiný poměr */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_2_1_CPM, &p ) );
    assert_pulses_us ( &p, 524.796 / 2, 488.665 / 2, 304.762 / 2, 262.935 / 2 );

    st_MZTAPE_PULSES_LENGTH saved = p;
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_CUSTOM, &p ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_NONE, &p ) );
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, mztape_get_speed_pulses ( MZTAPE_FORMATSET_COUNT, CMTSPEED_1_1, &p ) );
    TEST_ASSERT_EQUAL_MEMORY ( &saved, &p, sizeof ( p ) );
}


/** @brief mztape_pulses_get_ratio: orientační poměr vůči 1:1. */
static void test_pulses_get_ratio ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_1_1, &p ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 1e-9, 1.0, mztape_pulses_get_ratio ( MZTAPE_FORMATSET_MZ800_SANE, &p ) );

    /* UniCMT 3x: (964,638 + 524,006) / (156 + 164 + 80 + 92) = 3,0257 */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 156, 164, 80, 92 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.0001, 3.0257, mztape_pulses_get_ratio ( MZTAPE_FORMATSET_MZ800_SANE, &p ) );

    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, mztape_pulses_get_ratio ( MZTAPE_FORMATSET_COUNT, &p ) );
    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, mztape_pulses_get_ratio ( MZTAPE_FORMATSET_MZ800_SANE, NULL ) );
}


/**
 * @brief Sestaví hlavičku CMTSPEED jako soubor 3xspeed.mzf z USB disku UniCMT FW 0.5.
 *
 * Bajty podle hexdumpu v bázi (hw/25-unicmt.md, kap. 2.1): typ 00h, jméno
 * "CMTSPEED" + 0Dh, délka/zaváděcí/startovací adresa 0, bajt 20h = 01h,
 * na 30h 4x uint16 LE 156, 164, 80, 92.
 *
 * @param[out] hdr Cílový buffer 128 bajtů.
 */
static void make_cmtspeed_3x ( uint8_t *hdr ) {
    static const char name[] = "CMTSPEED\r";
    static const uint16_t us[4] = { 156, 164, 80, 92 };
    memset ( hdr, 0, 128 );
    memcpy ( &hdr[0x01], name, sizeof ( name ) - 1 );
    hdr[0x20] = 0x01;
    for ( int i = 0; i < 4; i++ ) {
        hdr[0x30 + i * 2] = (uint8_t) ( us[i] & 0xFF );
        hdr[0x31 + i * 2] = (uint8_t) ( us[i] >> 8 );
    }
}


/** @brief mztape_unicmt_speed_marker: rozpoznání hlavičky CMTSPEED. */
static void test_unicmt_speed_marker ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint8_t hdr[128];
    st_MZTAPE_PULSES_LENGTH p;

    make_cmtspeed_3x ( hdr );
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_VALID, mztape_unicmt_speed_marker ( hdr, &p ) );
    assert_pulses_us ( &p, 156, 164, 80, 92 );
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_VALID, mztape_unicmt_speed_marker ( hdr, NULL ) );

    /* bajt 20h se nekontroluje */
    hdr[0x20] = 0x00;
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_VALID, mztape_unicmt_speed_marker ( hdr, NULL ) );

    /* běžné MZF bloky: jiný typ, jiné jméno, nenulová délka těla */
    make_cmtspeed_3x ( hdr );
    hdr[0x00] = 0x01;
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_NONE, mztape_unicmt_speed_marker ( hdr, NULL ) );
    make_cmtspeed_3x ( hdr );
    hdr[0x09] = ' ';  /* "CMTSPEED " bez 0Dh za jménem */
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_NONE, mztape_unicmt_speed_marker ( hdr, NULL ) );
    make_cmtspeed_3x ( hdr );
    hdr[0x02] = 'X';
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_NONE, mztape_unicmt_speed_marker ( hdr, NULL ) );
    make_cmtspeed_3x ( hdr );
    hdr[0x13] = 0x01;
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_NONE, mztape_unicmt_speed_marker ( hdr, NULL ) );

    /* nulová délka pulzu = neplatná hlavička, *p beze změny */
    make_cmtspeed_3x ( hdr );
    hdr[0x34] = 0x00;
    hdr[0x35] = 0x00;
    st_MZTAPE_PULSES_LENGTH saved = p;
    TEST_ASSERT_EQUAL_INT ( MZTAPE_UNICMT_MARKER_INVALID, mztape_unicmt_speed_marker ( hdr, &p ) );
    TEST_ASSERT_EQUAL_MEMORY ( &saved, &p, sizeof ( p ) );
}


/**
 * @brief Přečte celý vstream a ověří, že je bajtově shodný s druhým.
 */
static void assert_vstreams_equal ( st_CMT_VSTREAM *a, st_CMT_VSTREAM *b ) {
    TEST_ASSERT_EQUAL_UINT32 ( a->size, b->size );
    TEST_ASSERT_EQUAL_UINT64 ( a->scans, b->scans );
    TEST_ASSERT_EQUAL_MEMORY ( a->data, b->data, a->size );
}


/**
 * @brief Vstream z poměru a vstream z délek pulzů toho poměru jsou shodné.
 *
 * Délky z mztape_get_speed_pulses() tedy přesně popisují, co emulátor pro
 * daný poměr přehrává (GUI je nabízí jako výchozí bod vlastních pulzů).
 */
static void test_vstream_pulses_match_ratio ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 64 );
    TEST_ASSERT_NOT_NULL ( h );
    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    const uint32_t rates[] = { 44100, 48000, TEST_GDG_RATE };
    for ( size_t r = 0; r < sizeof ( rates ) / sizeof ( rates[0] ); r++ ) {
        for ( int i = 0; g_mztape_speed[i] != CMTSPEED_NONE; i++ ) {
            st_MZTAPE_PULSES_LENGTH p;
            TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_get_speed_pulses ( MZTAPE_FORMATSET_MZ800_SANE, g_mztape_speed[i], &p ) );
            st_CMT_VSTREAM *a = mztape_create_cmt_vstream_from_mztmzf ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, g_mztape_speed[i], rates[r] );
            st_CMT_VSTREAM *b = mztape_create_cmt_vstream_from_mztmzf_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, &p, rates[r] );
            TEST_ASSERT_NOT_NULL ( a );
            TEST_ASSERT_NOT_NULL ( b );
            assert_vstreams_equal ( a, b );
            cmt_vstream_destroy ( a );
            cmt_vstream_destroy ( b );
        }
    }

    /* CUSTOM není poměr - ratio cesta ho odmítne */
    TEST_ASSERT_NULL ( mztape_create_cmt_vstream_from_mztmzf ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, CMTSPEED_CUSTOM, 44100 ) );

    mztape_mztmzf_destroy ( mztmzf );
}


/**
 * @brief Vlastní délky pulzů: počty vzorků high/low = round(délka * rate).
 *
 * UniCMT 3x na taktu GDG MZ-800: krátký 80 / 92 µs = 1418 / 1630 taktů,
 * dlouhý 156 / 164 µs = 2765 / 2906 taktů. Záznam začíná dlouhým GAPem
 * (MZTAPE_LGAP_LENGTH_SANE krátkých pulzů), za ním dlouhý tapemark
 * (40 dlouhých).
 */
static void test_vstream_custom_pulse_samples ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 16 );
    TEST_ASSERT_NOT_NULL ( h );
    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 156, 164, 80, 92 ) );
    st_CMT_VSTREAM *v = mztape_create_cmt_vstream_from_mztmzf_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, &p, TEST_GDG_RATE );
    TEST_ASSERT_NOT_NULL ( v );

    cmt_vstream_read_reset ( v );
    uint64_t samples;
    int value;
    for ( int i = 0; i < MZTAPE_LGAP_LENGTH_SANE; i++ ) {
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, cmt_vstream_read_pulse ( v, &samples, &value ) );
        TEST_ASSERT_EQUAL_INT ( 1, value );
        TEST_ASSERT_EQUAL_UINT64 ( 1418, samples );
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, cmt_vstream_read_pulse ( v, &samples, &value ) );
        TEST_ASSERT_EQUAL_INT ( 0, value );
        TEST_ASSERT_EQUAL_UINT64 ( 1630, samples );
    }
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, cmt_vstream_read_pulse ( v, &samples, &value ) );
    TEST_ASSERT_EQUAL_INT ( 1, value );
    TEST_ASSERT_EQUAL_UINT64 ( 2765, samples );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, cmt_vstream_read_pulse ( v, &samples, &value ) );
    TEST_ASSERT_EQUAL_INT ( 0, value );
    TEST_ASSERT_EQUAL_UINT64 ( 2906, samples );

    /* celkový počet vzorků = dlouhé * (2765 + 2906) + krátké * (1418 + 1630) */
    uint64_t long_pulses, short_pulses;
    mztape_compute_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, &long_pulses, &short_pulses );
    TEST_ASSERT_EQUAL_UINT64 ( long_pulses * ( 2765 + 2906 ) + short_pulses * ( 1418 + 1630 ), cmt_vstream_get_count_scans ( v ) );

    cmt_vstream_destroy ( v );

    /* část pulzu kratší než půl vzorku -> chyba (při 44,1 kHz je vzorek 22,7 µs) */
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 156, 164, 80, 10 ) );
    TEST_ASSERT_NULL ( mztape_create_cmt_vstream_from_mztmzf_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, &p, 44100 ) );
    TEST_ASSERT_NULL ( mztape_create_cmt_vstream_from_mztmzf_pulses ( mztmzf, MZTAPE_FORMATSET_MZ800_SANE, NULL, 44100 ) );

    mztape_mztmzf_destroy ( mztmzf );
}


/** @brief mztape_create_stream_from_mztapemzf_pulses: vstream i bitstream. */
static void test_stream_from_pulses ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_mzf_in_memory ( 16 );
    TEST_ASSERT_NOT_NULL ( h );
    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, 0 );
    TEST_ASSERT_NOT_NULL ( mztmzf );

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, mztape_pulses_set_us ( &p, 470, 494, 240, 278 ) );

    st_CMT_STREAM *s = mztape_create_stream_from_mztapemzf_pulses ( mztmzf, &p, CMT_STREAM_TYPE_VSTREAM, MZTAPE_FORMATSET_MZ800_SANE, TEST_GDG_RATE );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQUAL_INT ( CMT_STREAM_TYPE_VSTREAM, s->stream_type );
    cmt_stream_destroy ( s );

    s = mztape_create_stream_from_mztapemzf_pulses ( mztmzf, &p, CMT_STREAM_TYPE_BITSTREAM, MZTAPE_FORMATSET_MZ800_SANE, 44100 );
    TEST_ASSERT_NOT_NULL ( s );
    TEST_ASSERT_EQUAL_INT ( CMT_STREAM_TYPE_BITSTREAM, s->stream_type );
    cmt_stream_destroy ( s );

    mztape_mztmzf_destroy ( mztmzf );
}


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ();

    UNITY_BEGIN ();

    /* Smoke testy */
    RUN_TEST ( test_mztapemzf_create_destroy );
    RUN_TEST ( test_mztapemzf_destroy_null );

    /* Unit testy */
    RUN_TEST ( test_mztapemzf_checksums );
    RUN_TEST ( test_create_vstream );
    RUN_TEST ( test_create_bitstream );
    RUN_TEST ( test_create_stream_wrapper );
    RUN_TEST ( test_compute_pulses );
    RUN_TEST ( test_speed_array_terminated );
    RUN_TEST ( test_allocator_custom );
    RUN_TEST ( test_error_callback );

    /* Regresní testy */
    RUN_TEST ( test_destroy_frees_body );
    RUN_TEST ( test_create_error_no_use_after_free );
    RUN_TEST ( test_pulses_set_us );
    RUN_TEST ( test_get_speed_pulses );
    RUN_TEST ( test_pulses_get_ratio );
    RUN_TEST ( test_unicmt_speed_marker );
    RUN_TEST ( test_vstream_pulses_match_ratio );
    RUN_TEST ( test_vstream_custom_pulse_samples );
    RUN_TEST ( test_stream_from_pulses );

    int result = UNITY_END ();
    mztest_teardown ();
    return result;
}
