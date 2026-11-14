/**
 * @file   test_wav.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Testy knihovny wav (RIFF WAVE souborový formát).
 *
 * Testuje: velikosti binárních struktur, parsování hlavičky (validní/nevalidní),
 *          chunk scanning (přeskakování neznámých chunků), čtení vzorků
 *          (normalizovaná float hodnota včetně 32-bit IEEE float WAV, 1-bit
 *          kvantizace), zápis WAV souborů,
 *          roundtrip (zápis → čtení), polaritu, vícekanálové WAV, chybové kódy,
 *          robustnost (NULL vstupy, poškozená data, prázdné WAV).
 *
 * Linkuje se s TEST_ALL_OBJS (vyžaduje generic_driver, memory_driver, endianity).
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: Test čtení 32-bit IEEE float mono WAV (wav 2.1.0).
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

#include "libs/wav/wav.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/endianity/endianity.h"


/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

/** @brief Sdílený handler pro testy */
static st_HANDLER s_handler;
/** @brief Příznak, zda je handler otevřený (pro tearDown) */
static int s_handler_open = 0;


/** @brief Inicializace před každým testem — reset handleru */
void setUp ( void ) {
    memset ( &s_handler, 0, sizeof ( s_handler ) );
    s_handler_open = 0;
}


/** @brief Úklid po každém testu — zavření handleru pokud je otevřený */
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
 * @brief Otevře paměťový handler s automatickým zvětšováním.
 *
 * @param initial_size Počáteční velikost bufferu v bajtech
 * @return Ukazatel na handler, nebo NULL při chybě
 */
static st_HANDLER* open_memory_handler ( uint32_t initial_size ) {
    st_HANDLER *h = generic_driver_open_memory ( &s_handler, &g_memory_driver_realloc, initial_size );
    if ( h != NULL ) s_handler_open = 1;
    return h;
}


/**
 * @brief Vytvoří minimální validní 8-bit mono PCM WAV v memory handleru.
 *
 * @param num_samples  Počet vzorků
 * @param sample_rate  Vzorkovací frekvence (Hz)
 * @param sample_value Hodnota každého vzorku (0-255, 8-bit unsigned)
 * @return Handler, nebo NULL při chybě
 */
static st_HANDLER* create_test_wav_8bit_mono ( uint32_t num_samples, uint32_t sample_rate, uint8_t sample_value ) {

    uint32_t data_size = num_samples;
    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + data_size;

    st_HANDLER *h = open_memory_handler ( total_size );
    if ( !h ) return NULL;

    /* vzorková data */
    uint8_t *samples = (uint8_t*) malloc ( data_size );
    if ( !samples ) return NULL;
    memset ( samples, sample_value, data_size );

    en_WAV_ERROR err = wav_write ( h, sample_rate, 1, 8, samples, data_size );
    free ( samples );

    if ( err != WAV_OK ) return NULL;
    return h;
}


/**
 * @brief Vytvoří 16-bit mono PCM WAV v memory handleru.
 *
 * @param samples     Pole int16_t vzorků (v host byte-order)
 * @param num_samples Počet vzorků
 * @param sample_rate Vzorkovací frekvence (Hz)
 * @return Handler, nebo NULL při chybě
 */
static st_HANDLER* create_test_wav_16bit_mono ( const int16_t *samples, uint32_t num_samples, uint32_t sample_rate ) {

    uint32_t data_size = num_samples * 2;

    /* konverze do little-endian */
    int16_t *le_samples = (int16_t*) malloc ( data_size );
    if ( !le_samples ) return NULL;
    for ( uint32_t i = 0; i < num_samples; i++ ) {
        le_samples[i] = (int16_t) endianity_bswap16_LE ( (uint16_t) samples[i] );
    }

    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + data_size;

    st_HANDLER *h = open_memory_handler ( total_size );
    if ( !h ) { free ( le_samples ); return NULL; }

    en_WAV_ERROR err = wav_write ( h, sample_rate, 1, 16, le_samples, data_size );
    free ( le_samples );

    if ( err != WAV_OK ) return NULL;
    return h;
}


/**
 * @brief Zapíše surová data (bajt po bajtu) do memory handleru.
 *
 * Používá se pro vytváření poškozených/speciálních WAV souborů.
 *
 * @param h      Otevřený handler
 * @param offset Offset pro zápis
 * @param data   Data k zápisu
 * @param size   Velikost dat v bajtech
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
 */
static int write_raw_bytes ( st_HANDLER *h, uint32_t offset, const void *data, uint32_t size ) {
    return generic_driver_write ( h, offset, (void*) data, size );
}


/**
 * @brief Vytvoří WAV s extra chunkem mezi fmt a data (pro test chunk scanningu).
 *
 * Manuálně skládá RIFF strukturu: RIFF + fmt + LIST(dummy) + data.
 *
 * @return Handler s WAV daty, nebo NULL při chybě
 */
static st_HANDLER* create_wav_with_extra_chunk ( void ) {

    /* parametry */
    uint32_t sample_rate = 44100;
    uint16_t bits_per_sample = 8;
    uint16_t channels = 1;
    uint32_t num_samples = 4;

    /* dummy LIST chunk — 8 bajtů hlavička + 10 bajtů dat = 18 bajtů */
    uint8_t list_chunk[] = { 'L','I','S','T',  10,0,0,0,  'I','N','F','O', 0,0,0,0,0,0 };
    uint32_t list_chunk_size = sizeof ( list_chunk );

    uint32_t fmt_chunk_total = sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 );
    uint32_t data_chunk_total = sizeof ( st_WAV_CHUNK_HEADER ) + num_samples;
    uint32_t riff_size = fmt_chunk_total + list_chunk_size + data_chunk_total;
    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER ) + riff_size;

    st_HANDLER *h = open_memory_handler ( total_size );
    if ( !h ) return NULL;

    uint32_t pos = 0;

    /* RIFF hlavička */
    st_WAV_RIFF_HEADER riff;
    memcpy ( riff.riff_tag, "RIFF", 4 );
    riff.overall_size = endianity_bswap32_LE ( riff_size );
    memcpy ( riff.wave_tag, "WAVE", 4 );
    write_raw_bytes ( h, pos, &riff, sizeof ( riff ) );
    pos += sizeof ( riff );

    /* fmt chunk */
    st_WAV_CHUNK_HEADER fmt_hdr;
    memcpy ( fmt_hdr.chunk_tag, "fmt ", 4 );
    fmt_hdr.chunk_size = endianity_bswap32_LE ( sizeof ( st_WAV_FMT16 ) );
    write_raw_bytes ( h, pos, &fmt_hdr, sizeof ( fmt_hdr ) );
    pos += sizeof ( fmt_hdr );

    st_WAV_FMT16 fmt;
    fmt.format_code = endianity_bswap16_LE ( WAVE_FORMAT_CODE_PCM );
    fmt.channels = endianity_bswap16_LE ( channels );
    fmt.sample_rate = endianity_bswap32_LE ( sample_rate );
    fmt.bytes_per_sec = endianity_bswap32_LE ( sample_rate * channels * ( bits_per_sample / 8 ) );
    fmt.block_size = endianity_bswap16_LE ( channels * ( bits_per_sample / 8 ) );
    fmt.bits_per_sample = endianity_bswap16_LE ( bits_per_sample );
    write_raw_bytes ( h, pos, &fmt, sizeof ( fmt ) );
    pos += sizeof ( fmt );

    /* LIST chunk (neznámý chunk — musí být přeskočen) */
    write_raw_bytes ( h, pos, list_chunk, list_chunk_size );
    pos += list_chunk_size;

    /* data chunk */
    st_WAV_CHUNK_HEADER data_hdr;
    memcpy ( data_hdr.chunk_tag, "data", 4 );
    data_hdr.chunk_size = endianity_bswap32_LE ( num_samples );
    write_raw_bytes ( h, pos, &data_hdr, sizeof ( data_hdr ) );
    pos += sizeof ( data_hdr );

    /* vzorková data: 0, 128, 200, 64 */
    uint8_t samples[] = { 0, 128, 200, 64 };
    write_raw_bytes ( h, pos, samples, num_samples );

    return h;
}


/* ========================================================================
 * SMOKE testy — velikosti struktur a základní invarianty
 * ======================================================================== */

/** @brief Testuje velikosti binárních struktur pro korektní mapování na WAV data */
static void test_struct_sizes ( void ) {
    TEST_ASSERT_EQUAL_UINT ( 12, sizeof ( st_WAV_RIFF_HEADER ) );
    TEST_ASSERT_EQUAL_UINT ( 8, sizeof ( st_WAV_CHUNK_HEADER ) );
    TEST_ASSERT_EQUAL_UINT ( 16, sizeof ( st_WAV_FMT16 ) );
}


/** @brief Testuje, že wav_error_string vrací neprázdné řetězce pro všechny chybové kódy */
static void test_error_strings ( void ) {
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_OK ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_IO ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_BAD_RIFF ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_NO_FMT_CHUNK ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_NO_DATA_CHUNK ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_UNSUPPORTED_CODEC ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_UNSUPPORTED_BPS ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_CORRUPT_HEADER ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_ALLOC ) );
    TEST_ASSERT_NOT_NULL ( wav_error_string ( WAV_ERROR_INVALID_PARAM ) );
    /* neznámý kód nesmí vrátit NULL */
    TEST_ASSERT_NOT_NULL ( wav_error_string ( (en_WAV_ERROR) 99 ) );
}


/** @brief Testuje, že WAV_MAX_BITS_PER_SAMPLE je minimálně 64 */
static void test_max_bps_constant ( void ) {
    TEST_ASSERT_GREATER_OR_EQUAL ( 64, WAV_MAX_BITS_PER_SAMPLE );
}


/* ========================================================================
 * Testy parsování hlavičky
 * ======================================================================== */

/** @brief Testuje parsování validního 8-bit mono WAV — všechna pole se správně vyplní */
static void test_parse_valid_8bit_mono ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 100, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    TEST_ASSERT_EQUAL_UINT16 ( WAVE_FORMAT_CODE_PCM, sh->format_code );
    TEST_ASSERT_EQUAL_UINT16 ( 1, sh->channels );
    TEST_ASSERT_EQUAL_UINT32 ( 44100, sh->sample_rate );
    TEST_ASSERT_EQUAL_UINT16 ( 8, sh->bits_per_sample );
    TEST_ASSERT_EQUAL_UINT16 ( 1, sh->block_size );
    TEST_ASSERT_EQUAL_UINT32 ( 100, sh->blocks );
    TEST_ASSERT_EQUAL_UINT32 ( 100, sh->data_size );

    /* délka záznamu: 100 vzorků / 44100 Hz ≈ 0.00227 s */
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 100.0 / 44100.0, sh->count_sec );

    wav_simple_header_destroy ( sh );
}


/** @brief Testuje parsování s NULL handlerem — vrátí chybu, necrashne */
static void test_parse_null_handler ( void ) {
    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( NULL, &err );
    TEST_ASSERT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, err );
}


/** @brief Testuje parsování s NULL err parametrem — necrashne */
static void test_parse_null_err ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 10, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, NULL );
    TEST_ASSERT_NOT_NULL ( sh );

    wav_simple_header_destroy ( sh );
}


/** @brief Testuje wav_simple_header_destroy s NULL — no-op, necrashne */
static void test_destroy_null ( void ) {
    wav_simple_header_destroy ( NULL );
}


/** @brief Testuje detekci nevalidní RIFF hlavičky — náhodná data místo RIFF */
static void test_parse_bad_riff ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = open_memory_handler ( 64 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t garbage[64];
    memset ( garbage, 0xAB, sizeof ( garbage ) );
    write_raw_bytes ( h, 0, garbage, sizeof ( garbage ) );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_BAD_RIFF, err );
}


/** @brief Testuje detekci nepodporovaného formátového kódu (A-law 0x0006 místo PCM; IEEE float čte knihovna od wav 2.1.0) */
static void test_parse_unsupported_codec ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Vytvoříme validní WAV a pak přepíšeme format_code */
    st_HANDLER *h = create_test_wav_8bit_mono ( 10, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    /* format_code je na offsetu: RIFF(12) + chunk_hdr(8) + 0 = 20 */
    uint16_t unsupported_code = endianity_bswap16_LE ( 0x0006 ); /* WAVE_FORMAT_ALAW */
    write_raw_bytes ( h, 20, &unsupported_code, 2 );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_UNSUPPORTED_CODEC, err );
}


/* ========================================================================
 * Testy chunk scanningu — přeskakování neznámých chunků
 * ======================================================================== */

/*
 * WAV s extra chunkem (LIST) mezi fmt a data — musí se správně přeskočit.
 */
static void test_chunk_scanning_extra_chunk ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_wav_with_extra_chunk ();
    TEST_ASSERT_NOT_NULL ( h );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL_MESSAGE ( sh, "chunk scanning musí přeskočit LIST chunk" );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    TEST_ASSERT_EQUAL_UINT32 ( 4, sh->blocks );
    TEST_ASSERT_EQUAL_UINT16 ( 8, sh->bits_per_sample );
    TEST_ASSERT_EQUAL_UINT32 ( 44100, sh->sample_rate );

    wav_simple_header_destroy ( sh );
}


/*
 * Čtení vzorků z WAV s extra chunkem — data musí být správná.
 */
static void test_chunk_scanning_read_samples ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_wav_with_extra_chunk ();
    TEST_ASSERT_NOT_NULL ( h );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    /* kontrola hodnot vzorků: 0, 128, 200, 64 */
    double val;

    /* vzorek 0: raw=0, normalized = (0-128)/128 = -1.0 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 0, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, -1.0, val );

    /* vzorek 1: raw=128, normalized = (128-128)/128 = 0.0 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 1, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, 0.0, val );

    /* vzorek 2: raw=200, normalized = (200-128)/128 ≈ 0.5625 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 2, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, 0.5625, val );

    /* vzorek 3: raw=64, normalized = (64-128)/128 = -0.5 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 3, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, -0.5, val );

    wav_simple_header_destroy ( sh );
}


/* ========================================================================
 * Testy čtení vzorků — normalizovaná float hodnota
 * ======================================================================== */

/*
 * 8-bit PCM: krajní hodnoty (0 → -1.0, 128 → 0.0, 255 → ~+1.0).
 */
static void test_sample_float_8bit_range ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Vytvoříme WAV se 3 vzorky: 0, 128, 255 */
    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + 3;

    st_HANDLER *h = open_memory_handler ( total_size );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t samples[] = { 0, 128, 255 };
    en_WAV_ERROR err = wav_write ( h, 44100, 1, 8, samples, 3 );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    double val;

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 0, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, -1.0, val );

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 1, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, 0.0, val );

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 2, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.02, 1.0, val );

    wav_simple_header_destroy ( sh );
}


/*
 * 16-bit PCM: kladné a záporné vzorky.
 */
static void test_sample_float_16bit ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    int16_t samples[] = { 0, 16384, -16384, 32767, -32768 };
    st_HANDLER *h = create_test_wav_16bit_mono ( samples, 5, 44100 );
    TEST_ASSERT_NOT_NULL ( h );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    double val;

    /* 0 → 0.0 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 0, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 0.0, val );

    /* 16384 → ~0.5 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 1, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 0.5, val );

    /* -16384 → ~-0.5 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 2, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, -0.5, val );

    /* 32767 → ~+1.0 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 3, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 1.0, val );

    /* -32768 → -1.0 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 4, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, -1.0, val );

    wav_simple_header_destroy ( sh );
}


/*
 * 32-bit IEEE float mono WAV (format_code 3) - hlavička se přijme a vzorky
 * se vrátí beze změny jako normalizované hodnoty.
 */
static void test_sample_float_ieee32_mono ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    const float samples[] = { 0.5f, -0.5f, 0.0f, 1.0f };
    const uint32_t num_samples = sizeof ( samples ) / sizeof ( samples[0] );
    const uint32_t data_size = num_samples * 4;
    const uint32_t fmt_size = sizeof ( st_WAV_FMT16 );
    const uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                              + sizeof ( st_WAV_CHUNK_HEADER ) + fmt_size
                              + sizeof ( st_WAV_CHUNK_HEADER ) + data_size;

    st_HANDLER *h = open_memory_handler ( total_size );
    TEST_ASSERT_NOT_NULL ( h );

    /* RIFF + fmt (IEEE float, 1 kanál, 44100 Hz, 32 bit) + data, ručně v LE */
    st_WAV_RIFF_HEADER riff;
    memcpy ( riff.riff_tag, "RIFF", 4 );
    riff.overall_size = endianity_bswap32_LE ( total_size - 8 );
    memcpy ( riff.wave_tag, "WAVE", 4 );

    st_WAV_CHUNK_HEADER fmt_hdr;
    memcpy ( fmt_hdr.chunk_tag, "fmt ", 4 );
    fmt_hdr.chunk_size = endianity_bswap32_LE ( fmt_size );

    st_WAV_FMT16 fmt;
    fmt.format_code = endianity_bswap16_LE ( WAVE_FORMAT_CODE_IEEE_FLOAT );
    fmt.channels = endianity_bswap16_LE ( 1 );
    fmt.sample_rate = endianity_bswap32_LE ( 44100 );
    fmt.bytes_per_sec = endianity_bswap32_LE ( 44100 * 4 );
    fmt.block_size = endianity_bswap16_LE ( 4 );
    fmt.bits_per_sample = endianity_bswap16_LE ( 32 );

    st_WAV_CHUNK_HEADER data_hdr;
    memcpy ( data_hdr.chunk_tag, "data", 4 );
    data_hdr.chunk_size = endianity_bswap32_LE ( data_size );

    uint32_t offset = 0;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, write_raw_bytes ( h, offset, &riff, sizeof ( riff ) ) );
    offset += sizeof ( riff );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, write_raw_bytes ( h, offset, &fmt_hdr, sizeof ( fmt_hdr ) ) );
    offset += sizeof ( fmt_hdr );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, write_raw_bytes ( h, offset, &fmt, sizeof ( fmt ) ) );
    offset += sizeof ( fmt );
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, write_raw_bytes ( h, offset, &data_hdr, sizeof ( data_hdr ) ) );
    offset += sizeof ( data_hdr );
    for ( uint32_t i = 0; i < num_samples; i++ ) {
        uint32_t u;
        memcpy ( &u, &samples[i], sizeof ( u ) );
        u = endianity_bswap32_LE ( u );
        TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, write_raw_bytes ( h, offset, &u, sizeof ( u ) ) );
        offset += sizeof ( u );
    }
    TEST_ASSERT_EQUAL_UINT32 ( total_size, offset );

    en_WAV_ERROR err = WAV_ERROR_IO;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );
    TEST_ASSERT_EQUAL_UINT16 ( WAVE_FORMAT_CODE_IEEE_FLOAT, sh->format_code );
    TEST_ASSERT_EQUAL_UINT16 ( WAVE_FORMAT_CODE_IEEE_FLOAT, sh->sample_format );
    TEST_ASSERT_EQUAL_UINT16 ( 32, sh->bits_per_sample );
    TEST_ASSERT_EQUAL_UINT32 ( num_samples, sh->blocks );

    for ( uint32_t i = 0; i < num_samples; i++ ) {
        double val = -99.0;
        TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, i, 0, &val ) );
        TEST_ASSERT_FLOAT_WITHIN ( 1e-6, (double) samples[i], val );
    }

    wav_simple_header_destroy ( sh );
}


/*
 * Čtení mimo rozsah — vrátí WAV_ERROR_INVALID_PARAM.
 */
static void test_sample_float_out_of_range ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 10, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    double val;
    /* pozice 10 je mimo rozsah (0-9) */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_get_sample_float ( h, sh, 10, 0, &val ) );

    /* kanál 1 u mono WAV je mimo rozsah */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_get_sample_float ( h, sh, 0, 1, &val ) );

    wav_simple_header_destroy ( sh );
}


/*
 * NULL parametry — necrashne, vrátí WAV_ERROR_INVALID_PARAM.
 */
static void test_sample_float_null_params ( void ) {
    double val;
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_get_sample_float ( NULL, NULL, 0, 0, &val ) );
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_get_sample_float ( NULL, NULL, 0, 0, NULL ) );
}


/* ========================================================================
 * Testy 1-bit kvantizace
 * ======================================================================== */

/*
 * Normální polarita: záporné vzorky → 1, kladné/nulové → 0.
 */
static void test_bit_value_normal_polarity ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* 8-bit: hodnota 0 → normalizace = -1.0 → bit = 1 (záporný)
     *        hodnota 128 → normalizace = 0.0 → bit = 0 (nulový)
     *        hodnota 255 → normalizace ≈ +1.0 → bit = 0 (kladný) */
    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + 3;

    st_HANDLER *h = open_memory_handler ( total_size );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t samples[] = { 0, 128, 255 };
    wav_write ( h, 44100, 1, 8, samples, 3 );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    int bit;
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 0, WAV_POLARITY_NORMAL, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 1, bit ); /* záporný → 1 */

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 1, WAV_POLARITY_NORMAL, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 0, bit ); /* nulový → 0 */

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 2, WAV_POLARITY_NORMAL, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 0, bit ); /* kladný → 0 */

    wav_simple_header_destroy ( sh );
}


/*
 * Invertovaná polarita: kladné vzorky → 1, záporné/nulové → 0.
 */
static void test_bit_value_inverted_polarity ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    uint32_t total_size = sizeof ( st_WAV_RIFF_HEADER )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                        + sizeof ( st_WAV_CHUNK_HEADER ) + 3;

    st_HANDLER *h = open_memory_handler ( total_size );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t samples[] = { 0, 128, 255 };
    wav_write ( h, 44100, 1, 8, samples, 3 );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    int bit;
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 0, WAV_POLARITY_INVERTED, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 0, bit ); /* záporný → 0 */

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 1, WAV_POLARITY_INVERTED, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 0, bit ); /* nulový → 0 */

    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_bit_value_of_sample ( h, sh, 2, WAV_POLARITY_INVERTED, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 1, bit ); /* kladný → 1 */

    wav_simple_header_destroy ( sh );
}


/*
 * NULL bit_value — vrátí WAV_ERROR_INVALID_PARAM.
 */
static void test_bit_value_null_output ( void ) {
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM,
        wav_get_bit_value_of_sample ( NULL, NULL, 0, WAV_POLARITY_NORMAL, NULL ) );
}


/* ========================================================================
 * Testy zápisu WAV
 * ======================================================================== */

/*
 * Zápis 8-bit mono WAV — roundtrip: zápis → parsování → čtení vzorku.
 */
static void test_write_8bit_roundtrip ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = open_memory_handler ( 1024 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t samples[] = { 0, 64, 128, 192, 255 };
    en_WAV_ERROR err = wav_write ( h, 22050, 1, 8, samples, 5 );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    TEST_ASSERT_EQUAL_UINT16 ( WAVE_FORMAT_CODE_PCM, sh->format_code );
    TEST_ASSERT_EQUAL_UINT16 ( 1, sh->channels );
    TEST_ASSERT_EQUAL_UINT32 ( 22050, sh->sample_rate );
    TEST_ASSERT_EQUAL_UINT16 ( 8, sh->bits_per_sample );
    TEST_ASSERT_EQUAL_UINT32 ( 5, sh->blocks );

    /* kontrola vzorků */
    double val;
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 2, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.01, 0.0, val ); /* 128 → 0.0 */

    wav_simple_header_destroy ( sh );
}


/*
 * Zápis 16-bit stereo WAV — roundtrip.
 */
static void test_write_16bit_stereo_roundtrip ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = open_memory_handler ( 1024 );
    TEST_ASSERT_NOT_NULL ( h );

    /* 2 rámce (stereo): L0 R0 L1 R1 */
    int16_t samples[] = { 16384, -16384, -32768, 32767 };
    /* konverze do LE */
    for ( int i = 0; i < 4; i++ ) {
        samples[i] = (int16_t) endianity_bswap16_LE ( (uint16_t) samples[i] );
    }

    en_WAV_ERROR err = wav_write ( h, 48000, 2, 16, samples, sizeof ( samples ) );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );

    TEST_ASSERT_EQUAL_UINT16 ( 2, sh->channels );
    TEST_ASSERT_EQUAL_UINT32 ( 48000, sh->sample_rate );
    TEST_ASSERT_EQUAL_UINT16 ( 16, sh->bits_per_sample );
    TEST_ASSERT_EQUAL_UINT16 ( 4, sh->block_size ); /* 2 kanály * 2 bajty */
    TEST_ASSERT_EQUAL_UINT32 ( 2, sh->blocks );    /* 2 rámce */

    /* čtení kanálu 0 (levý) rámce 0 → 16384 → ~0.5 */
    double val;
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 0, 0, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 0.5, val );

    /* čtení kanálu 1 (pravý) rámce 0 → -16384 → ~-0.5 */
    TEST_ASSERT_EQUAL_INT ( WAV_OK, wav_get_sample_float ( h, sh, 0, 1, &val ) );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, -0.5, val );

    wav_simple_header_destroy ( sh );
}


/*
 * Zápis s neplatnými parametry — vrátí chybu.
 */
static void test_write_invalid_params ( void ) {
    st_HANDLER *h = open_memory_handler ( 1024 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t data[10] = {0};

    /* NULL handler */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_write ( NULL, 44100, 1, 8, data, 10 ) );

    /* NULL data */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_write ( h, 44100, 1, 8, NULL, 10 ) );

    /* 0 kanálů */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_write ( h, 44100, 0, 8, data, 10 ) );

    /* 0 sample rate */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_INVALID_PARAM, wav_write ( h, 0, 1, 8, data, 10 ) );

    /* nepodporovaná bitová hloubka (32) */
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_UNSUPPORTED_BPS, wav_write ( h, 44100, 1, 32, data, 10 ) );
}


/* ========================================================================
 * Testy zpětně kompatibilního API
 * ======================================================================== */

/*
 * wav_simple_header_new_from_handler_compat — wrapper bez chybového kódu.
 */
static void test_compat_header ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 50, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler_compat ( h );
    TEST_ASSERT_NOT_NULL ( sh );
    TEST_ASSERT_EQUAL_UINT32 ( 50, sh->blocks );

    wav_simple_header_destroy ( sh );
}


/*
 * wav_get_bit_value_of_sample_compat — wrapper s EXIT_SUCCESS/EXIT_FAILURE.
 */
static void test_compat_bit_value ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 10, 44100, 0 );
    TEST_ASSERT_NOT_NULL ( h );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler_compat ( h );
    TEST_ASSERT_NOT_NULL ( sh );

    int bit;
    TEST_ASSERT_EQUAL_INT ( EXIT_SUCCESS, wav_get_bit_value_of_sample_compat ( h, sh, 0, WAV_POLARITY_NORMAL, &bit ) );
    TEST_ASSERT_EQUAL_INT ( 1, bit ); /* 0 → -1.0 → záporný → 1 */

    /* mimo rozsah → EXIT_FAILURE */
    TEST_ASSERT_EQUAL_INT ( EXIT_FAILURE, wav_get_bit_value_of_sample_compat ( h, sh, 99, WAV_POLARITY_NORMAL, &bit ) );

    wav_simple_header_destroy ( sh );
}


/* ========================================================================
 * Testy robustnosti
 * ======================================================================== */

/*
 * Prázdné WAV (0 vzorků) — parsuje se správně, blocks = 0.
 */
static void test_empty_wav ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = open_memory_handler ( 256 );
    TEST_ASSERT_NOT_NULL ( h );

    uint8_t dummy = 0;
    en_WAV_ERROR err = wav_write ( h, 44100, 1, 8, &dummy, 0 );
    TEST_ASSERT_EQUAL_INT ( WAV_OK, err );

    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NOT_NULL ( sh );
    TEST_ASSERT_EQUAL_UINT32 ( 0, sh->blocks );
    TEST_ASSERT_FLOAT_WITHIN ( 0.001, 0.0, sh->count_sec );

    wav_simple_header_destroy ( sh );
}


/*
 * block_size = 0 v hlavičce — musí vrátit WAV_ERROR_CORRUPT_HEADER.
 */
static void test_corrupt_zero_block_size ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    st_HANDLER *h = create_test_wav_8bit_mono ( 10, 44100, 128 );
    TEST_ASSERT_NOT_NULL ( h );

    /* block_size je na offsetu: RIFF(12) + chunk_hdr(8) + format_code(2) + channels(2) +
     * sample_rate(4) + bytes_per_sec(4) = offset 32 */
    uint16_t zero = 0;
    write_raw_bytes ( h, 32, &zero, 2 );

    en_WAV_ERROR err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &err );
    TEST_ASSERT_NULL ( sh );
    TEST_ASSERT_EQUAL_INT ( WAV_ERROR_CORRUPT_HEADER, err );
}


/* ========================================================================
 * main
 * ======================================================================== */

int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ();

    UNITY_BEGIN ();

    /* Smoke testy — velikosti a invarianty */
    RUN_TEST ( test_struct_sizes );
    RUN_TEST ( test_error_strings );
    RUN_TEST ( test_max_bps_constant );

    /* Parsování hlavičky */
    RUN_TEST ( test_parse_valid_8bit_mono );
    RUN_TEST ( test_parse_null_handler );
    RUN_TEST ( test_parse_null_err );
    RUN_TEST ( test_destroy_null );
    RUN_TEST ( test_parse_bad_riff );
    RUN_TEST ( test_parse_unsupported_codec );

    /* Chunk scanning */
    RUN_TEST ( test_chunk_scanning_extra_chunk );
    RUN_TEST ( test_chunk_scanning_read_samples );

    /* Čtení vzorků — float */
    RUN_TEST ( test_sample_float_8bit_range );
    RUN_TEST ( test_sample_float_16bit );
    RUN_TEST ( test_sample_float_ieee32_mono );
    RUN_TEST ( test_sample_float_out_of_range );
    RUN_TEST ( test_sample_float_null_params );

    /* 1-bit kvantizace */
    RUN_TEST ( test_bit_value_normal_polarity );
    RUN_TEST ( test_bit_value_inverted_polarity );
    RUN_TEST ( test_bit_value_null_output );

    /* Zápis WAV */
    RUN_TEST ( test_write_8bit_roundtrip );
    RUN_TEST ( test_write_16bit_stereo_roundtrip );
    RUN_TEST ( test_write_invalid_params );

    /* Zpětná kompatibilita */
    RUN_TEST ( test_compat_header );
    RUN_TEST ( test_compat_bit_value );

    /* Robustnost */
    RUN_TEST ( test_empty_wav );
    RUN_TEST ( test_corrupt_zero_block_size );

    int result = UNITY_END ();
    mztest_teardown ();
    return result;
}
