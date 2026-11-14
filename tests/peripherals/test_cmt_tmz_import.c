/**
 * @file test_cmt_tmz_import.c
 * @brief Testy importu pásky do TMZ v paměti (cmt_tmz_import.h).
 *
 * Import převádí soubor nebo buffer pásky na st_TZX_FILE bez závislosti
 * na globálním stavu emulátoru. Testuje se:
 *   - rozpoznání zdroje podle přípony souboru a výchozí volby,
 *   - odmítnutí neznámého obsahu, neznámé přípony a chybějícího souboru
 *     (NULL a text chyby v reportu), úspěšný import ze souboru,
 *   - sestavení bloku 0x46 MZ800EMU-CMT a odmítnutí neplatných parametrů,
 *   - MZF: jeden blok, hlavička a tělo bajtově beze změny, hrany velikosti
 *     (delší soubor, kratší soubor, tělo 0 B), výchozí rychlost z voleb,
 *   - MZT: blok na každý MZF, rychlost z hlaviček CMTSPEED UniCMT, export
 *     MZT UniCMT bajtově shodný se vstupem, varování a chyby,
 *   - signál bloku přehraný playerem při GDGCLK_BASE proti dnešnímu mztape
 *     (stejné běhy, pauza jen mezi bloky, poslední blok bez pauzy),
 *   - TAP: blok 0x11 s časováním zxtape na každý blok TAP, pauzy jako
 *     dřívější rozšíření TAP, poškozený konec; signál bloku proti zxtape,
 *   - WAV: prahování na 1 bit jako dřívější rozšíření WAV, jeden blok 0x18 CSW,
 *     velký WAV 96 kHz stereo float,
 *   - TMZ/TZX: načtení souboru uloženého tzx_save(),
 *   - rychlost bloků z cmttmz_import_buffer_speeds() a
 *     cmttmz_import_file_speeds() (MZF DEFAULT, MZT DEFAULT / FIXED
 *     s pulzy z hlavičky CMTSPEED, TAP FIXED 1:1, TMZ NONE; blok 0x46
 *     MZ800EMU-CMT z TMZ FIXED s rychlostí a pauzou z PROFILE_ARGS),
 *   - cmttmz_build_tap_block() v rychlostech zxtape a 2:1 proti
 *     zxtape_create_stream_from_tapblock() (délky pulzů na 1 vzorek).
 *
 * Testovací data se generují v testu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/gdg/gdgclk.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_mzt.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/tmz/tmz_player.h"
#include "libs/zxtape/zxtape.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/*
 * Přípona rozhoduje o zdroji bez ohledu na velikost písmen; neznámá
 * přípona, soubor bez přípony a NULL selžou.
 */
void test_source_from_filename(void)
{
    en_CMTTMZ_SOURCE src = CMTTMZ_SOURCE_WAV;

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("a.MZF", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZF, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("b.m12", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZF, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("c.Tmz", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_TMZ, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("d.tzx", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_TMZ, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("e.wave", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_WAV, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("dir.x/f.mzt", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZT, src);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_source_from_filename("g.TAP", &src));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_TAP, src);

    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_source_from_filename("f.dsk", &src));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_source_from_filename("g", &src));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_source_from_filename("dir.mzf/g", &src));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_source_from_filename("h.", &src));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_source_from_filename(NULL, &src));
}

/*
 * Výchozí volby importu.
 */
void test_import_opts_defaults(void)
{
    st_CMTTMZ_IMPORT_OPTS opts;
    memset(&opts, 0xA5, sizeof(opts));
    cmttmz_import_opts_init(&opts);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, opts.default_speed);
    TEST_ASSERT_EQUAL_INT(CMT_STREAM_POLARITY_NORMAL, opts.wav_polarity);
    TEST_ASSERT_EQUAL_UINT16(1000, opts.pause_ms);
}

/*
 * 16 nulových bajtů jako TMZ: NULL a vyplněná chyba.
 */
void test_import_buffer_unknown_content(void)
{
    uint8_t zeros[16];
    memset(zeros, 0, sizeof(zeros));
    st_CMTTMZ_IMPORT_REPORT rep;
    memset(&rep, 0, sizeof(rep));

    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_TMZ, zeros, sizeof(zeros), NULL, &rep);
    TEST_ASSERT_NULL(f);
    TEST_ASSERT_TRUE(rep.error[0] != '\0');
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_TMZ, rep.source);

    /* bez reportu jen NULL */
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_TMZ, zeros, sizeof(zeros), NULL, NULL));
}

/*
 * Blok 0x46 z hlavičky a těla; neplatné parametry vrátí NULL.
 */
void test_build_mzf_block_args(void)
{
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = MZF_FTYPE_OBJ;
    memset(hdr.fname.name, 0x0D, sizeof(hdr.fname.name));
    memcpy(hdr.fname.name, "B1", 2);
    hdr.fname.terminator = 0x0D;
    hdr.fsize = 4;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    const uint8_t body[4] = {1, 2, 3, 4};

    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_1_1, NULL, 1000);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_HEX8(TMZ_BLOCK_ID_MZ_FILE, b->id);
    tmz_block_free(b);

    b = cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_NONE, NULL, 0);
    TEST_ASSERT_NOT_NULL(b);
    tmz_block_free(b);

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 139));
    b = cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_CUSTOM, &p, 1000);
    TEST_ASSERT_NOT_NULL(b);
    tmz_block_free(b);

    /* vlastní rychlost bez pulzů, neplatná rychlost, délka těla různá od fsize, NULL */
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_CUSTOM, NULL, 1000));
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_COUNT, NULL, 1000));
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(&hdr, body, 3, CMTSPEED_1_1, NULL, 1000));
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(&hdr, NULL, 4, CMTSPEED_1_1, NULL, 1000));
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(NULL, body, 4, CMTSPEED_1_1, NULL, 1000));

    /* pulz pod 0,5 µs se na celé µs nezaokrouhlí */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 0.25));
    TEST_ASSERT_NULL(cmttmz_build_mzf_block(&hdr, body, 4, CMTSPEED_CUSTOM, &p, 1000));
}

/*
 * Neznámá přípona a neexistující soubor: NULL a text chyby.
 */
void test_import_file_errors(void)
{
    st_CMTTMZ_IMPORT_REPORT rep;
    memset(&rep, 0, sizeof(rep));
    TEST_ASSERT_NULL(cmttmz_import_file("nothing.dsk", NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("Unsupported tape file extension", rep.error);

    memset(&rep, 0, sizeof(rep));
    TEST_ASSERT_NULL(cmttmz_import_file("mztest_cmt_tmz_import_missing_file.mzf", NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("Can't open file mztest_cmt_tmz_import_missing_file.mzf", rep.error);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZF, rep.source);
}

/** @brief Pulzy UniCMT 1x v µs (soubor 1xspeed.mzf, báze hw/25-unicmt.md). */
static const uint16_t s_pulses_1x[4] = {470, 494, 240, 278};
/** @brief Pulzy UniCMT 2x v µs (soubor 2xspeed.mzf, báze hw/25-unicmt.md). */
static const uint16_t s_pulses_2x[4] = {235, 247, 120, 139};
/** @brief Pulzy UniCMT 3x v µs (soubor 3xspeed.mzf, báze hw/25-unicmt.md). */
static const uint16_t s_pulses_3x[4] = {156, 164, 80, 92};

/** @brief Pracovní buffer pro sestavení testovacích MZF a MZT. */
static uint8_t s_buf[4096];

/**
 * @brief Zapíše do bufferu MZF (hlavička + tělo).
 *
 * Hlavička má bajt 11h 00h (jako zápis ROM SAVE) a komentář vyplněný
 * vzorkem, aby bylo vidět, že se přenáší celá beze změny.
 *
 * @param dst Cíl (aspoň 128 + body_len bajtů).
 * @param name Jméno souboru (nejvýš 16 znaků).
 * @param fsize Velikost těla zapsaná v hlavičce.
 * @param body_len Skutečný počet bajtů těla za hlavičkou.
 * @param seed Semínko vzorku těla.
 * @return Počet zapsaných bajtů.
 */
static uint32_t put_mzf(uint8_t *dst, const char *name, uint16_t fsize, uint32_t body_len, uint8_t seed)
{
    memset(dst, 0, 128);
    dst[0] = MZF_FTYPE_OBJ;
    memset(&dst[1], 0x0D, 16);
    memcpy(&dst[1], name, strlen(name));
    dst[0x11] = 0x00;
    dst[0x12] = (uint8_t)(fsize & 0xFF);
    dst[0x13] = (uint8_t)(fsize >> 8);
    dst[0x14] = 0x00; /* fstrt = 1200h */
    dst[0x15] = 0x12;
    dst[0x16] = 0x34; /* fexec = 1234h */
    dst[0x17] = 0x12;
    for (int i = 0x18; i < 128; i++) dst[i] = (uint8_t)(i ^ 0x5A);
    for (uint32_t i = 0; i < body_len; i++) dst[128 + i] = (uint8_t)((i * 37 + seed) & 0xFF);
    return 128 + body_len;
}

/**
 * @brief Zapíše do bufferu hlavičku CMTSPEED (tmz_mzt_build_cmtspeed, bajtově jako soubory UniCMT).
 * @param dst Cíl (aspoň 128 bajtů).
 * @param us Pulzy LONG H, LONG L, SHORT H, SHORT L v µs.
 * @return 128.
 */
static uint32_t put_cmtspeed(uint8_t *dst, const uint16_t us[4])
{
    tmz_mzt_build_cmtspeed(us, dst);
    return 128;
}

/**
 * @brief Ověří, že blok nese MZF shodné se vstupem (hlavička bajt po bajtu, tělo).
 * @param block Blok 0x46.
 * @param raw MZF ve vstupních datech (hlavička v pořadí souboru + tělo).
 * @param body_size Očekávaná délka těla.
 */
static void assert_block_mzf(const st_TZX_BLOCK *block, const uint8_t *raw, uint32_t body_size)
{
    TEST_ASSERT_EQUAL_HEX8(TMZ_BLOCK_ID_MZ_FILE, block->id);
    en_TZX_ERROR err = TZX_OK;
    st_MZF *mzf = tmz_block_to_mzf(block, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(mzf, tzx_error_string(err));
    st_MZF_HEADER hdr = mzf->header;
    mzf_header_swap_endianity(&hdr);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, (const uint8_t *)&hdr, 128);
    TEST_ASSERT_EQUAL_UINT32(body_size, mzf->body_size);
    if (body_size) TEST_ASSERT_EQUAL_HEX8_ARRAY(raw + 128, mzf->body, body_size);
    mzf_free(mzf);
}

/**
 * @brief Ověří rychlost bloku pro export MZT (pulzy UniCMT v µs).
 * @param block Blok 0x46.
 * @param us Očekávané pulzy LONG H, LONG L, SHORT H, SHORT L.
 */
static void assert_block_pulses(const st_TZX_BLOCK *block, const uint16_t us[4])
{
    st_TMZ_MZT_BLOCK_INFO info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL_INT(1, tmz_mzt_block_info(block, &info));
    TEST_ASSERT_EQUAL_INT(1, info.rom_structure);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(us, info.pulses_us, 4);
    tmz_mzt_block_info_free(&info);
}

/*
 * MZF 300 B -> jeden blok 0x46 -> tmz_block_to_mzf() vrátí hlavičku i tělo bajtově shodné.
 */
void test_mzf_roundtrip(void)
{
    uint32_t size = put_mzf(s_buf, "ROUNDTRIP", 300, 300, 11);
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_EQUAL_STRING("", rep.error);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZF, rep.source);
    TEST_ASSERT_TRUE(f->is_tmz);
    TEST_ASSERT_TRUE(tmz_header_is_tmz(&f->header));
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    assert_block_mzf(&f->blocks[0], s_buf, 300);
    assert_block_pulses(&f->blocks[0], s_pulses_1x);
    tzx_free(f);
}

/*
 * Soubor o 10 B delší: varování, tělo fsize bajtů. O 1 B kratší: NULL a chyba.
 * fsize 0: blok jen s hlavičkou. Data kratší než hlavička: chyba.
 */
void test_mzf_size_edges(void)
{
    st_CMTTMZ_IMPORT_REPORT rep;

    uint32_t size = put_mzf(s_buf, "LONGER", 300, 310, 3);
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("MZF file is longer than header size, extra data ignored", rep.warning);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    assert_block_mzf(&f->blocks[0], s_buf, 300);
    tzx_free(f);

    size = put_mzf(s_buf, "SHORTER", 300, 299, 3);
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("MZF body is shorter than header size", rep.error);

    size = put_mzf(s_buf, "EMPTY", 0, 0, 0);
    f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    assert_block_mzf(&f->blocks[0], s_buf, 0);
    tzx_free(f);

    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, 127, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("MZF file is shorter than MZF header", rep.error);
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, NULL, 0, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("MZF file is shorter than MZF header", rep.error);
}

/*
 * Výchozí rychlost CMTSPEED_CUSTOM: blok MZF s vlastními pulzy z opts;
 * poměr 3:1: blok s rychlostí 3:1.
 */
void test_mzf_default_speed_from_opts(void)
{
    uint32_t size = put_mzf(s_buf, "CUSTOM", 40, 40, 7);
    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.default_speed = CMTSPEED_CUSTOM;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&opts.custom_pulses, s_pulses_3x[0], s_pulses_3x[1],
                                                             s_pulses_3x[2], s_pulses_3x[3]));
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, &opts, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    assert_block_pulses(&f->blocks[0], s_pulses_3x);
    tzx_free(f);

    /* neplatná výchozí rychlost: chyba sestavení bloku */
    opts.default_speed = CMTSPEED_COUNT;
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, s_buf, size, &opts, &rep));
    TEST_ASSERT_EQUAL_STRING_LEN("Can't create MZF tape block: ", rep.error, strlen("Can't create MZF tape block: "));
}

/*
 * MZT: tři MZF, mezi druhým a třetím CMTSPEED 2x -> 3 bloky; export
 * TMZ_MZT_UNICMT vrátí bajty shodné se vstupem.
 */
void test_mzt_unicmt_roundtrip(void)
{
    uint32_t off_a = 0;
    uint32_t size = put_mzf(s_buf, "PART A", 100, 100, 1);
    uint32_t off_b = size;
    size += put_mzf(s_buf + size, "PART B", 200, 200, 2);
    size += put_cmtspeed(s_buf + size, s_pulses_2x);
    uint32_t off_c = size;
    size += put_mzf(s_buf + size, "PART C", 50, 50, 3);

    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZT, rep.source);
    TEST_ASSERT_TRUE(tmz_header_is_tmz(&f->header));
    TEST_ASSERT_EQUAL_UINT32(3, f->block_count);
    assert_block_mzf(&f->blocks[0], s_buf + off_a, 100);
    assert_block_mzf(&f->blocks[1], s_buf + off_b, 200);
    assert_block_mzf(&f->blocks[2], s_buf + off_c, 50);
    assert_block_pulses(&f->blocks[0], s_pulses_1x);
    assert_block_pulses(&f->blocks[1], s_pulses_1x);
    assert_block_pulses(&f->blocks[2], s_pulses_2x);

    uint8_t *out = NULL;
    uint32_t out_size = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, tmz_mzt_export(f, TMZ_MZT_UNICMT, &out, &out_size, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(size, out_size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_buf, out, size);
    free(out);
    tzx_free(f);
}

/*
 * CMTSPEED na prvním místě platí, ale přidá poznámku; CMTSPEED s nulovým
 * pulzem se zahodí s varováním; zbytek kratší než hlavička se ignoruje.
 */
void test_mzt_markers_warnings_and_tail(void)
{
    static const uint16_t zero_pulse[4] = {0, 494, 240, 278};
    uint32_t size = put_cmtspeed(s_buf, s_pulses_3x);
    size += put_mzf(s_buf + size, "FIRST", 20, 20, 4);
    uint32_t off_zero = size;
    size += put_cmtspeed(s_buf + size, zero_pulse);
    size += put_mzf(s_buf + size, "SECOND", 30, 30, 5);
    memset(s_buf + size, 0, 50); /* zbytek po přenosu přes CP/M */
    size += 50;

    char expect[256];
    snprintf(expect, sizeof(expect),
             "note: CMTSPEED header is the first block; the real UniCMT does not play such a tape; "
             "warning: CMTSPEED header at offset 0x%x has a zero pulse length, ignored",
             (unsigned)off_zero);

    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING(expect, rep.warning);
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    /* úvodní CMTSPEED platí (jako dosavadní rozšíření MZT), nulová hlavička rychlost nemění */
    assert_block_pulses(&f->blocks[0], s_pulses_3x);
    assert_block_pulses(&f->blocks[1], s_pulses_3x);
    tzx_free(f);
}

/*
 * MZT bez přehratelného bloku, s tělem přes konec dat a prázdné.
 */
void test_mzt_errors(void)
{
    st_CMTTMZ_IMPORT_REPORT rep;

    uint32_t size = put_cmtspeed(s_buf, s_pulses_2x);
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("No playable block in MZT file", rep.error);

    size = put_mzf(s_buf, "CUT", 100, 99, 1);
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("Can't parse MZT file", rep.error);

    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, NULL, 0, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("Can't parse MZT file", rep.error);
}

/**
 * @brief Výsledek porovnání běhů dvou vstreamů.
 */
typedef struct st_RUNS_CMP {
    uint64_t runs_ref;      /**< počet běhů reference (mztape) */
    uint64_t runs_got;      /**< počet běhů přehraného bloku */
    uint64_t first_diff;    /**< index první neshody (UINT64_MAX = žádná) */
    uint64_t last_ref;      /**< délka posledního běhu reference */
    uint64_t last_got;      /**< délka posledního běhu bloku */
    int last_ref_value;     /**< úroveň posledního běhu reference */
    int last_got_value;     /**< úroveň posledního běhu bloku */
} st_RUNS_CMP;

/**
 * @brief Porovná běhy (úroveň, délka ve vzorcích) dvou vstreamů kromě posledního.
 *
 * Poslední běh se jen zaznamená: u přehraného bloku do něj padá pauza za
 * blokem (úroveň LOW navazuje na poslední L reference).
 *
 * @param ref Reference (mztape).
 * @param got Přehraný blok.
 * @param[out] cmp Výsledek.
 */
static void runs_compare(st_CMT_VSTREAM *ref, st_CMT_VSTREAM *got, st_RUNS_CMP *cmp)
{
    memset(cmp, 0, sizeof(*cmp));
    cmp->first_diff = UINT64_MAX;
    cmt_vstream_read_reset(ref);
    cmt_vstream_read_reset(got);
    uint64_t sa = 0, sb = 0;
    int va = 0, vb = 0;
    int ea = cmt_vstream_read_pulse(ref, &sa, &va);
    int eb = cmt_vstream_read_pulse(got, &sb, &vb);
    while (ea == EXIT_SUCCESS || eb == EXIT_SUCCESS) {
        uint64_t pa = sa, pb = sb;
        int pva = va, pvb = vb;
        int had_a = (ea == EXIT_SUCCESS), had_b = (eb == EXIT_SUCCESS);
        if (had_a) cmp->runs_ref++;
        if (had_b) cmp->runs_got++;
        if (had_a) ea = cmt_vstream_read_pulse(ref, &sa, &va);
        if (had_b) eb = cmt_vstream_read_pulse(got, &sb, &vb);
        int last_a = had_a && ea != EXIT_SUCCESS;
        int last_b = had_b && eb != EXIT_SUCCESS;
        if (last_a) {
            cmp->last_ref = pa;
            cmp->last_ref_value = pva;
        }
        if (last_b) {
            cmp->last_got = pb;
            cmp->last_got_value = pvb;
        }
        /* neshoda mimo poslední běh kteréhokoli streamu */
        if (cmp->first_diff == UINT64_MAX && !last_a && !last_b
            && (!had_a || !had_b || pa != pb || pva != pvb)) {
            cmp->first_diff = cmp->runs_ref - 1;
        }
    }
    cmt_vstream_read_reset(ref);
    cmt_vstream_read_reset(got);
}

/**
 * @brief Přehraje blok z importu a porovná ho s dnešním signálem mztape.
 *
 * Reference je cesta dosavadního rozšíření MZF: st_MZTAPE_MZF z paměťového
 * handleru (mztape_create_mztapemzf) a vstream mztape formátem
 * MZTAPE_FORMATSET_MZ800_SANE při GDGCLK_BASE. Testy běží s jádrem MZ-800
 * (mz_test_core_mz800), GDGCLK_BASE je tedy 17 721 600 Hz = časová základna
 * profilu MZ800EMU-CMT.
 *
 * @param block Blok 0x46 z importu.
 * @param raw MZF bloku (hlavička + tělo). @param size Délka MZF.
 * @param speed Rychlost reference (poměr nebo CMTSPEED_CUSTOM).
 * @param pulses Vlastní pulzy reference (při CMTSPEED_CUSTOM).
 * @param pause_ms Očekávaná pauza za blokem v ms.
 * @param label Popis do výpisu.
 */
static void assert_block_signal(const st_TZX_BLOCK *block, const uint8_t *raw, uint32_t size, en_CMTSPEED speed,
                                const st_MZTAPE_PULSES_LENGTH *pulses, uint16_t pause_ms, const char *label)
{
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = GDGCLK_BASE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *got = tmz_player_play_block(block, &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, tmz_player_error_string(perr));
    TEST_ASSERT_EQUAL_INT(CMT_STREAM_TYPE_VSTREAM, got->stream_type);

    st_HANDLER handler;
    st_HANDLER *h = generic_driver_open_memory(&handler, &g_memory_driver_static, size);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, generic_driver_write(h, 0, (void *)raw, size));
    st_MZTAPE_MZF *m = mztape_create_mztapemzf(h, 0);
    TEST_ASSERT_NOT_NULL(m);
    st_CMT_VSTREAM *ref;
    if (cmtspeed_is_custom(speed)) {
        ref = mztape_create_cmt_vstream_from_mztmzf_pulses(m, MZTAPE_FORMATSET_MZ800_SANE, pulses, GDGCLK_BASE);
    } else {
        ref = mztape_create_cmt_vstream_from_mztmzf(m, MZTAPE_FORMATSET_MZ800_SANE, speed, GDGCLK_BASE);
    }
    TEST_ASSERT_NOT_NULL(ref);

    st_RUNS_CMP cmp;
    runs_compare(ref, got->str.vstream, &cmp);
    uint64_t pause_ticks = (uint64_t)pause_ms * GDGCLK_BASE / 1000;
    if (mztest_verbose) {
        printf("%s: mztape %llu runs, block %llu runs, last run mztape %llu/%d, block %llu/%d, pause %llu ticks\n",
               label, (unsigned long long)cmp.runs_ref, (unsigned long long)cmp.runs_got,
               (unsigned long long)cmp.last_ref, cmp.last_ref_value, (unsigned long long)cmp.last_got,
               cmp.last_got_value, (unsigned long long)pause_ticks);
    }

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(UINT64_MAX, cmp.first_diff, label);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(cmp.runs_ref, cmp.runs_got, label);
    /* poslední běh: L reference, u bloku prodloužené o pauzu za blokem (LOW) */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmp.last_ref_value, label);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmp.last_got_value, label);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(cmp.last_ref + pause_ticks, cmp.last_got, label);

    cmt_vstream_destroy(ref);
    mztape_mztmzf_destroy(m);
    generic_driver_close(h);
    cmt_stream_destroy(got);
}

/**
 * @brief Importuje MZF a ověří signál jeho jediného bloku proti mztape (pauza 0).
 * @param raw MZF. @param size Délka MZF.
 * @param opts Volby importu (rychlost reference = opts->default_speed).
 * @param label Popis do výpisu.
 */
static void assert_mzf_signal(const uint8_t *raw, uint32_t size, const st_CMTTMZ_IMPORT_OPTS *opts, const char *label)
{
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZF, raw, size, opts, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    /* jediný blok = poslední blok pásky: bez pauzy, signál shodný s mztape celý */
    assert_block_signal(&f->blocks[0], raw, size, opts->default_speed, &opts->custom_pulses, 0, label);
    tzx_free(f);
}

/*
 * Blok z MZF přehraný playerem při GDGCLK_BASE má stejné běhy jako
 * dnešní mztape (MZ800_SANE); jediný blok je bez pauzy, takže je shodný
 * celý. 1:1, navíc 3:1, vlastní pulzy UniCMT 2x a tělo 0 B.
 */
void test_mzf_signal_matches_mztape(void)
{
    uint32_t size = put_mzf(s_buf, "SIGNAL", 300, 300, 9);
    st_CMTTMZ_IMPORT_OPTS opts;

    cmttmz_import_opts_init(&opts);
    assert_mzf_signal(s_buf, size, &opts, "speed 1:1");

    opts.default_speed = CMTSPEED_3_1;
    assert_mzf_signal(s_buf, size, &opts, "speed 3:1");

    opts.default_speed = CMTSPEED_CUSTOM;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&opts.custom_pulses, s_pulses_2x[0], s_pulses_2x[1],
                                                             s_pulses_2x[2], s_pulses_2x[3]));
    assert_mzf_signal(s_buf, size, &opts, "custom pulses 2x");

    /* tělo 0 B (hlavička + prázdné tělo) */
    size = put_mzf(s_buf, "SIGNAL0", 0, 0, 0);
    cmttmz_import_opts_init(&opts);
    assert_mzf_signal(s_buf, size, &opts, "speed 1:1, empty body");
}

/*
 * MZT ze dvou MZF: první blok má za sebou pauzu opts->pause_ms (LOW),
 * poslední blok pauzu 0; signál obou jinak shodný s mztape.
 */
void test_mzt_pause_only_between_blocks(void)
{
    uint32_t size = put_mzf(s_buf, "FIRST", 40, 40, 1);
    uint32_t off_b = size;
    size += put_mzf(s_buf + size, "LAST", 30, 30, 2);

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.pause_ms = 700;
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, &opts, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    assert_block_signal(&f->blocks[0], s_buf, off_b, CMTSPEED_1_1, NULL, 700, "MZT block 0");
    assert_block_signal(&f->blocks[1], s_buf + off_b, size - off_b, CMTSPEED_1_1, NULL, 0, "MZT last block");
    tzx_free(f);
}

/*
 * Úspěšný import ze souboru: MZF v dočasném souboru -> 1 blok shodný se vstupem.
 */
void test_import_file_mzf(void)
{
    uint32_t size = put_mzf(s_buf, "FROM FILE", 77, 77, 5);
    gchar *name = g_strdup_printf("mztest_cmt_tmz_import_%u.mzf", (unsigned)g_random_int());
    gchar *path = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)s_buf, size, NULL));

    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_file(path, NULL, &rep);
    g_remove(path);
    g_free(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SOURCE_MZF, rep.source);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    assert_block_mzf(&f->blocks[0], s_buf, 77);
    tzx_free(f);
}

/*
 * Mnoho varování: report zůstane ukončený nulou a nekončí oddělovačem.
 */
void test_warning_overflow(void)
{
    static const uint16_t zero_pulse[4] = {0, 494, 240, 278};
    uint32_t size = put_mzf(s_buf, "A", 10, 10, 1);
    for (int i = 0; i < 12; i++) size += put_cmtspeed(s_buf + size, zero_pulse);
    size += put_mzf(s_buf + size, "B", 10, 10, 2);

    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_MZT, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    size_t len = strlen(rep.warning);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(rep.warning));
    TEST_ASSERT_FALSE(len >= 2 && rep.warning[len - 2] == ';' && rep.warning[len - 1] == ' ');
    TEST_ASSERT_FALSE(rep.warning[len - 1] == ';');
    tzx_free(f);

    /*
     * Hranice: za 253 znaky se oddělovač "; " s aspoň jedním znakem nevejde
     * (253 + 2 + 1 = 256), další varování se nepřidá vůbec. Dřívější pravidlo
     * by přidalo samotný oddělovač (253) nebo jeden znak bez oddělovače (254).
     */
    for (size_t fill = 253; fill <= 255; fill++) {
        memset(&rep, 0, sizeof(rep));
        memset(rep.warning, 'w', fill);
        cmttmz_report_add_warning(&rep, "next");
        TEST_ASSERT_EQUAL_size_t(fill, strlen(rep.warning));
        TEST_ASSERT_EQUAL_CHAR('w', rep.warning[fill - 1]);
    }
    /* 252 znaků: oddělovač se vejde, z dalšího varování zbude 1 znak */
    memset(&rep, 0, sizeof(rep));
    memset(rep.warning, 'w', 252);
    cmttmz_report_add_warning(&rep, "next");
    TEST_ASSERT_EQUAL_size_t(255, strlen(rep.warning));
    TEST_ASSERT_EQUAL_STRING("; n", &rep.warning[252]);
    /* prázdný report: varování celé, druhé s oddělovačem */
    memset(&rep, 0, sizeof(rep));
    cmttmz_report_add_warning(&rep, "first");
    cmttmz_report_add_warning(&rep, "second");
    TEST_ASSERT_EQUAL_STRING("first; second", rep.warning);
}

/* ================================================================
 * TAP, WAV, TMZ/TZX
 * ================================================================ */

/**
 * @brief Zapíše do bufferu blok TAP ([2 B délka LE][flag][data][checksum]).
 * @param dst Cíl (aspoň 2 + len bajtů).
 * @param flag Flag bloku (00h hlavička, FFh data).
 * @param len Délka bloku v TAP (flag + data + checksum, aspoň 2).
 * @param seed Semínko vzorku dat.
 * @return Počet zapsaných bajtů.
 */
static uint32_t put_tap_block(uint8_t *dst, uint8_t flag, uint16_t len, uint8_t seed)
{
    dst[0] = (uint8_t)(len & 0xFF);
    dst[1] = (uint8_t)(len >> 8);
    dst[2] = flag;
    uint8_t chk = flag;
    for (uint16_t i = 1; i + 1 < len; i++) {
        dst[2 + i] = (uint8_t)((i * 29 + seed) & 0xFF);
        chk ^= dst[2 + i];
    }
    dst[2 + len - 1] = chk;
    return 2u + len;
}

/**
 * @brief Ověří blok 0x11: časování zxtape, pauza a data shodná s blokem TAP.
 * @param block Blok. @param tap_block Blok TAP včetně 2 B délky.
 * @param pause_ms Očekávaná pauza.
 */
static void assert_tap_block(const st_TZX_BLOCK *block, const uint8_t *tap_block, uint16_t pause_ms)
{
    TEST_ASSERT_EQUAL_HEX8(TZX_BLOCK_ID_TURBO_SPEED, block->id);
    st_TZX_TURBO_SPEED ts;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_parse_turbo_speed(block->data, block->length, &ts));
    uint16_t len = (uint16_t)(tap_block[0] | (tap_block[1] << 8));
    TEST_ASSERT_EQUAL_UINT32(0x12u + len, block->length);
    /* konstanty zxtape (zxtape.c:156-179) x 3 500 000 zaokrouhleno */
    TEST_ASSERT_EQUAL_UINT16(2143, ts.pilot_pulse); /* 612,395 us */
    TEST_ASSERT_EQUAL_UINT16(652, ts.sync1_pulse);  /* 186,161 us */
    TEST_ASSERT_EQUAL_UINT16(508, ts.sync2_pulse);  /* 145,070 us */
    TEST_ASSERT_EQUAL_UINT16(847, ts.zero_pulse);   /* 242,140 us */
    TEST_ASSERT_EQUAL_UINT16(1695, ts.one_pulse);   /* 484,335 us */
    TEST_ASSERT_EQUAL_UINT16(tap_block[2] < 0x80 ? 8064 : 3220, ts.pilot_count);
    TEST_ASSERT_EQUAL_UINT8(8, ts.used_bits);
    TEST_ASSERT_EQUAL_UINT16(pause_ms, ts.pause_ms);
    TEST_ASSERT_EQUAL_UINT32(len, ts.data_length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tap_block + 2, ts.data, len);
}

/*
 * TAP: hlavička 19 B a data 10 B -> 2 bloky 0x11 se shodnými daty (pauza
 * za hlavičkou 350 ms jako dřívější rozšíření TAP, poslední blok 0); useknutý třetí
 * blok -> 2 bloky a varování.
 */
void test_tap_import(void)
{
    uint32_t off_b = put_tap_block(s_buf, 0x00, 19, 1);
    uint32_t size = off_b + put_tap_block(s_buf + off_b, 0xFF, 10, 2);

    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, size, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_TRUE(tmz_header_is_tmz(&f->header));
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    assert_tap_block(&f->blocks[0], s_buf, 350);
    assert_tap_block(&f->blocks[1], s_buf + off_b, 0);
    tzx_free(f);

    /* data, hlavička, data: pauza za daty 1000 ms, za hlavičkou 350 ms, poslední 0 */
    uint32_t off_c = size;
    size = off_c + put_tap_block(s_buf + off_c, 0x00, 19, 5);
    uint32_t off_d = size;
    size = off_d + put_tap_block(s_buf + off_d, 0xFF, 10, 6);
    f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf + off_b, size - off_b, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_UINT32(3, f->block_count);
    assert_tap_block(&f->blocks[0], s_buf + off_b, 1000);
    assert_tap_block(&f->blocks[1], s_buf + off_c, 350);
    assert_tap_block(&f->blocks[2], s_buf + off_d, 0);
    tzx_free(f);
    size = off_c;

    /* třetí blok useknutý: délka 50, k dispozici jen 20 bajtů */
    uint32_t cut = size + put_tap_block(s_buf + size, 0xFF, 50, 3);
    cut -= 30;
    f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, cut, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("TAP file has a damaged end, incomplete block ignored", rep.warning);
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    assert_tap_block(&f->blocks[0], s_buf, 350);
    assert_tap_block(&f->blocks[1], s_buf + off_b, 0);
    tzx_free(f);

    /* jediný bajt délky navíc je také poškozený konec */
    f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, size + 1, NULL, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("TAP file has a damaged end, incomplete block ignored", rep.warning);
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    tzx_free(f);

    /* žádný celý blok */
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, 10, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("No playable block in TAP file", rep.error);
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, NULL, 0, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("No playable block in TAP file", rep.error);
}

/**
 * @brief Běhy vstreamu (bez běhů nulové délky).
 *
 * @invariant n <= cap. @par Vlastnictví: len a val uvolní runs_free().
 */
typedef struct st_RUNS {
    uint64_t *len;   /**< délky běhů ve vzorcích */
    int *val;        /**< úrovně běhů */
    uint32_t n;      /**< počet běhů */
    uint32_t cap;    /**< kapacita */
} st_RUNS;

/**
 * @brief Přečte běhy vstreamu, běhy délky 0 vynechá.
 * @param v Vstream. @param[out] r Běhy (uvolnit runs_free()).
 */
static void runs_collect(st_CMT_VSTREAM *v, st_RUNS *r)
{
    memset(r, 0, sizeof(*r));
    cmt_vstream_read_reset(v);
    uint64_t s = 0;
    int val = 0;
    while (cmt_vstream_read_pulse(v, &s, &val) == EXIT_SUCCESS) {
        if (s == 0) continue;
        if (r->n == r->cap) {
            r->cap = r->cap ? r->cap * 2 : 4096;
            r->len = (uint64_t *)realloc(r->len, r->cap * sizeof(uint64_t));
            r->val = (int *)realloc(r->val, r->cap * sizeof(int));
            TEST_ASSERT_NOT_NULL(r->len);
            TEST_ASSERT_NOT_NULL(r->val);
        }
        r->len[r->n] = s;
        r->val[r->n] = val;
        r->n++;
    }
    cmt_vstream_read_reset(v);
}

/** @brief Uvolní běhy. @param r Běhy. */
static void runs_free(st_RUNS *r)
{
    free(r->len);
    free(r->val);
    memset(r, 0, sizeof(*r));
}

/**
 * @brief Největší rozdíl délek běhů v úseku a průměry obou streamů.
 * @param a Běhy A. @param b Běhy B. @param from Začátek. @param count Počet.
 * @param[out] avg_a Průměr A. @param[out] avg_b Průměr B.
 * @return Největší |A - B| ve vzorcích.
 */
static uint64_t runs_max_diff(const st_RUNS *a, const st_RUNS *b, uint32_t from, uint32_t count, double *avg_a,
                              double *avg_b)
{
    uint64_t md = 0, sa = 0, sb = 0;
    for (uint32_t i = from; i < from + count; i++) {
        uint64_t d = (a->len[i] > b->len[i]) ? a->len[i] - b->len[i] : b->len[i] - a->len[i];
        if (d > md) md = d;
        sa += a->len[i];
        sb += b->len[i];
    }
    *avg_a = count ? (double)sa / count : 0.0;
    *avg_b = count ? (double)sb / count : 0.0;
    return md;
}

/*
 * Blok 0x11 z importu TAP přehraný playerem při 44 100 Hz proti dnešnímu
 * zxtape_create_stream_from_tapblock ( ..., CMTSPEED_1_1, 44100 ):
 * pilot, oba sync, krátké a dlouhé půlvlny dat shodné do 1 vzorku,
 * celková délka do 0,2 %. Strukturní rozdíly se ověřují přesně: zxtape
 * má navíc závěrečný běh L délky půlvlny pilotu (zxtape.c:600), blok 0x11
 * jako poslední blok pásky nemá nic. Úrovně test záměrně neověřuje: signál
 * bloku 0x11 má opačnou úroveň než zxtape (player začíná blok TZX úrovní
 * TZX_LEVEL_AFTER_PAUSE, první pulz HIGH, zxtape první půlvlnou LOW);
 * je to přijatá odchylka, dopad na zavaděče ZX na MZ je [neověřeno].
 */
void test_tap_timing_vs_zxtape(void)
{
    static const struct {
        uint8_t flag;
        uint16_t len;
    } cases[2] = {{0x00, 19}, {0xFF, 10}};
    for (int c = 0; c < 2; c++) {
        uint32_t size = put_tap_block(s_buf, cases[c].flag, cases[c].len, 7);
        st_CMTTMZ_IMPORT_REPORT rep;
        st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, size, NULL, &rep);
        TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);

        st_TMZ_PLAYER_CONFIG cfg;
        tmz_player_config_init(&cfg);
        cfg.sample_rate = 44100;
        cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
        en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
        st_CMT_STREAM *got = tmz_player_play_block(&f->blocks[0], &cfg, &perr);
        TEST_ASSERT_NOT_NULL_MESSAGE(got, tmz_player_error_string(perr));
        st_CMT_STREAM *ref = zxtape_create_stream_from_tapblock((en_ZXTAPE_BLOCK_FLAG)cases[c].flag, s_buf + 2,
                                                                cases[c].len, CMTSPEED_1_1, 44100,
                                                                CMT_STREAM_TYPE_VSTREAM);
        TEST_ASSERT_NOT_NULL(ref);

        st_RUNS rg, rr;
        runs_collect(got->str.vstream, &rg);
        runs_collect(ref->str.vstream, &rr);
        uint32_t pilot = 2u * (cases[c].flag ? ZXTAPE_DATA_PILOT_LENGTH : ZXTAPE_HEADER_PILOT_LENGTH);
        uint32_t data = (uint32_t)cases[c].len * 16u;
        uint32_t body = pilot + 2 + data;
        /* struktura: blok 0x11 přesně pilot + 2 sync + data, zxtape navíc 1 závěrečný běh */
        TEST_ASSERT_EQUAL_UINT32(body, rg.n);
        TEST_ASSERT_EQUAL_UINT32(body + 1, rr.n);

        double ag, ar;
        uint64_t d_pilot = runs_max_diff(&rg, &rr, 0, pilot, &ag, &ar);
        double pilot_g = ag, pilot_r = ar;
        uint64_t d_sync1 = runs_max_diff(&rg, &rr, pilot, 1, &ag, &ar);
        double s1_g = ag, s1_r = ar;
        uint64_t d_sync2 = runs_max_diff(&rg, &rr, pilot + 1, 1, &ag, &ar);
        double s2_g = ag, s2_r = ar;
        uint64_t d_short = 0, d_long = 0, n_short = 0, n_long = 0, sum_sg = 0, sum_sr = 0, sum_lg = 0, sum_lr = 0;
        for (uint32_t i = pilot + 2; i < body; i++) {
            /* krátká / dlouhá podle reference (práh 3/4 pilotu) */
            int is_long = (rr.len[i] * 4 > rr.len[0] * 3);
            uint64_t d = (rg.len[i] > rr.len[i]) ? rg.len[i] - rr.len[i] : rr.len[i] - rg.len[i];
            if (is_long) {
                if (d > d_long) d_long = d;
                n_long++;
                sum_lg += rg.len[i];
                sum_lr += rr.len[i];
            } else {
                if (d > d_short) d_short = d;
                n_short++;
                sum_sg += rg.len[i];
                sum_sr += rr.len[i];
            }
        }
        uint64_t tot_g = 0, tot_r = 0;
        for (uint32_t i = 0; i < rg.n; i++) tot_g += rg.len[i];
        for (uint32_t i = 0; i < rr.n; i++) tot_r += rr.len[i];
        double tot_diff = fabs((double)tot_g - (double)tot_r) / (double)tot_r;

        if (mztest_verbose) {
            double us = 1e6 / 44100.0;
            printf("%s block: pilot %u halves 0x11 %.2f us / zxtape %.2f us (max diff %llu), "
                   "sync1 %.2f / %.2f us (%llu), sync2 %.2f / %.2f us (%llu), short %llu x %.2f / %.2f us (%llu), "
                   "long %llu x %.2f / %.2f us (%llu), total %llu / %llu samples (%.4f %%), "
                   "zxtape tail %llu samples level %d, first level 0x11 %d zxtape %d\n",
                   cases[c].flag ? "data" : "header", pilot, pilot_g * us, pilot_r * us, (unsigned long long)d_pilot,
                   s1_g * us, s1_r * us, (unsigned long long)d_sync1, s2_g * us, s2_r * us,
                   (unsigned long long)d_sync2, (unsigned long long)n_short,
                   n_short ? (double)sum_sg / n_short * us : 0.0, n_short ? (double)sum_sr / n_short * us : 0.0,
                   (unsigned long long)d_short, (unsigned long long)n_long,
                   n_long ? (double)sum_lg / n_long * us : 0.0, n_long ? (double)sum_lr / n_long * us : 0.0,
                   (unsigned long long)d_long, (unsigned long long)tot_g, (unsigned long long)tot_r,
                   tot_diff * 100.0, (unsigned long long)rr.len[rr.n - 1], rr.val[rr.n - 1], rg.val[0], rr.val[0]);
        }

        TEST_ASSERT_TRUE_MESSAGE(d_pilot <= 1, "pilot");
        TEST_ASSERT_TRUE_MESSAGE(d_sync1 <= 1, "sync1");
        TEST_ASSERT_TRUE_MESSAGE(d_sync2 <= 1, "sync2");
        TEST_ASSERT_TRUE_MESSAGE(d_short <= 1, "short half-wave");
        TEST_ASSERT_TRUE_MESSAGE(d_long <= 1, "long half-wave");
        TEST_ASSERT_TRUE_MESSAGE(tot_diff <= 0.002, "total duration");
        /* závěrečný běh zxtape: L délky půlvlny pilotu (zxtape.c:600) */
        TEST_ASSERT_EQUAL_UINT64(rr.len[0], rr.len[rr.n - 1]);

        runs_free(&rg);
        runs_free(&rr);
        cmt_stream_destroy(got);
        cmt_stream_destroy(ref);
        tzx_free(f);
    }
}

/**
 * @brief Sestaví WAV v paměti (RIFF, fmt 16 B, data); data vynulovaná.
 * @param format_code 1 = PCM, 3 = IEEE float.
 * @param channels Počet kanálů. @param rate Vzorkovací frekvence.
 * @param bits Bitů na vzorek. @param frames Počet rámců.
 * @param[out] out_size Délka WAV.
 * @return Buffer (uvolnit free()), nebo NULL.
 */
static uint8_t *make_wav(uint16_t format_code, uint16_t channels, uint32_t rate, uint16_t bits, uint32_t frames,
                         uint32_t *out_size)
{
    uint32_t block = (uint32_t)channels * bits / 8;
    uint32_t data_size = block * frames;
    uint32_t size = 44 + data_size;
    uint8_t *w = (uint8_t *)calloc(1, size);
    if (!w) return NULL;
    memcpy(w, "RIFF", 4);
    uint32_t riff = size - 8;
    memcpy(w + 4, &riff, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(w + 16, &fmt_size, 4);
    memcpy(w + 20, &format_code, 2);
    memcpy(w + 22, &channels, 2);
    memcpy(w + 24, &rate, 4);
    uint32_t bps = rate * block;
    memcpy(w + 28, &bps, 4);
    uint16_t block16 = (uint16_t)block;
    memcpy(w + 32, &block16, 2);
    memcpy(w + 34, &bits, 2);
    memcpy(w + 36, "data", 4);
    memcpy(w + 40, &data_size, 4);
    *out_size = size;
    return w;
}

/**
 * @brief Zapíše vzorek do WAV z make_wav() (PCM 8 bit nebo float 32 bit).
 * @param w WAV. @param format_code 1 = PCM 8 bit, 3 = float 32 bit.
 * @param channels Počet kanálů. @param frame Rámec. @param ch Kanál.
 * @param v Hodnota -1.0 až 1.0.
 */
static void wav_put(uint8_t *w, uint16_t format_code, uint16_t channels, uint32_t frame, uint16_t ch, double v)
{
    if (format_code == 1) {
        w[44 + (size_t)frame * channels + ch] = (uint8_t)(128 + (int)(v * 127.0));
    } else {
        float f = (float)v;
        memcpy(w + 44 + ((size_t)frame * channels + ch) * 4, &f, 4);
    }
}

/**
 * @brief Přečte pulzy bloku 0x18 (RLE).
 * @param block Blok 0x18. @param[out] rate Frekvence.
 * @param[out] pulses Pole pulzů (nejvýš max). @param max Kapacita pole.
 * @return Počet pulzů v bloku (pulse_count); ověří, že RLE obsahuje právě tolik pulzů.
 */
static uint32_t csw_pulses(const st_TZX_BLOCK *block, uint32_t *rate, uint32_t *pulses, uint32_t max)
{
    TEST_ASSERT_EQUAL_HEX8(TZX_BLOCK_ID_CSW_RECORDING, block->id);
    st_TZX_CSW_RECORDING csw;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_parse_csw_recording(block->data, block->length, &csw));
    TEST_ASSERT_EQUAL_HEX8(TZX_CSW_COMPRESSION_RLE, csw.compression_type);
    *rate = csw.sample_rate;
    uint32_t n = 0, i = 0;
    while (i < csw.data_length) {
        uint32_t p = csw.data[i++];
        if (p == 0) {
            TEST_ASSERT_TRUE(i + 4 <= csw.data_length);
            memcpy(&p, csw.data + i, 4);
            i += 4;
        }
        if (n < max) pulses[n] = p;
        n++;
    }
    TEST_ASSERT_EQUAL_UINT32(csw.pulse_count, n);
    return n;
}

/**
 * @brief Obdélník 100 period po 20 vzorcích (10 + 10) do mono WAV 44 100 Hz.
 * @param format_code 1 = PCM 8 bit, 3 = float 32 bit.
 * @param first Hodnota první půlperiody (druhá má opačné znaménko).
 * @param[out] size Délka WAV.
 * @return WAV (uvolnit free()).
 */
static uint8_t *make_square_wav(uint16_t format_code, double first, uint32_t *size)
{
    uint8_t *w = make_wav(format_code, 1, 44100, format_code == 1 ? 8 : 32, 2000, size);
    TEST_ASSERT_NOT_NULL(w);
    for (uint32_t i = 0; i < 2000; i++) wav_put(w, format_code, 1, i, 0, ((i / 10) % 2) ? -first : first);
    return w;
}

/**
 * @brief Ověří import obdélníku: 200 pulzů po 10 vzorcích, při začátku HIGH s pulzem LOW 1 vzorek navíc.
 * @param w WAV. @param size Délka. @param polarity Polarita prahování.
 * @param starts_high 1 = signál po prahování začíná HIGH.
 */
static void assert_square_import(const uint8_t *w, uint32_t size, en_CMT_STREAM_POLARITY polarity, int starts_high)
{
    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.wav_polarity = polarity;
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, w, size, &opts, &rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_STRING("", rep.warning);
    TEST_ASSERT_TRUE(tmz_header_is_tmz(&f->header));
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    uint32_t rate = 0;
    static uint32_t p[256];
    uint32_t n = csw_pulses(&f->blocks[0], &rate, p, 256);
    TEST_ASSERT_EQUAL_UINT32(44100, rate);
    TEST_ASSERT_EQUAL_UINT32(starts_high ? 201 : 200, n);
    uint32_t k = 0;
    if (starts_high) TEST_ASSERT_EQUAL_UINT32(1, p[k++]);
    for (; k < n; k++) TEST_ASSERT_EQUAL_UINT32(10, p[k]);
    tzx_free(f);
}

/*
 * WAV PCM 8 bit a float 32 bit, obdélník 100 period po 20 vzorcích:
 * jeden blok 0x18 s frekvencí souboru. Prahování jako dřívější rozšíření WAV
 * (wav_get_bit_value_of_sample): normální polarita = záporný vzorek HIGH.
 */
void test_wav_square(void)
{
    uint16_t formats[2] = {1, 3};
    for (int k = 0; k < 2; k++) {
        uint32_t size = 0;
        /* kladná první půlperioda: při normální polaritě LOW */
        uint8_t *w = make_square_wav(formats[k], 0.5, &size);
        assert_square_import(w, size, CMT_STREAM_POLARITY_NORMAL, 0);
        assert_square_import(w, size, CMT_STREAM_POLARITY_INVERTED, 1);
        free(w);
        /* záporná první půlperioda: při normální polaritě HIGH */
        w = make_square_wav(formats[k], -0.5, &size);
        assert_square_import(w, size, CMT_STREAM_POLARITY_NORMAL, 1);
        free(w);
    }
}

/*
 * WAV bez hrany a poškozený WAV.
 */
void test_wav_errors(void)
{
    uint32_t size = 0;
    uint8_t *w = make_wav(3, 1, 44100, 32, 1000, &size); /* ticho 0.0 */
    TEST_ASSERT_NOT_NULL(w);
    st_CMTTMZ_IMPORT_REPORT rep;
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, w, size, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("WAV contains no signal", rep.error);
    free(w);

    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, s_buf, 16, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING_LEN("Can't read WAV file: ", rep.error, strlen("Can't read WAV file: "));

    /* počet kanálů 0 (block_size zůstává 4, jinak by hlavičku odmítla už knihovna) */
    w = make_square_wav(3, 0.5, &size);
    w[22] = 0;
    w[23] = 0;
    TEST_ASSERT_NULL(cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, w, size, NULL, &rep));
    TEST_ASSERT_EQUAL_STRING("WAV header is corrupted", rep.error);
    free(w);
}

/*
 * Blok 0x18 z WAV, který začíná HIGH, přehraný playerem při frekvenci
 * WAV: první běh je LOW délky 1 vzorek (předřazený pulz), druhý HIGH.
 */
void test_wav_block_plays_low_first(void)
{
    uint32_t size = 0;
    uint8_t *w = make_square_wav(1, -0.5, &size); /* záporná první půlperioda = HIGH */
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, w, size, NULL, &rep);
    free(w);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);

    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = 44100;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *s = tmz_player_play_block(&f->blocks[0], &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, tmz_player_error_string(perr));
    TEST_ASSERT_EQUAL_INT(CMT_STREAM_TYPE_VSTREAM, s->stream_type);
    st_CMT_VSTREAM *v = s->str.vstream;
    TEST_ASSERT_EQUAL_UINT32(44100, cmt_vstream_get_rate(v));
    cmt_vstream_read_reset(v);
    uint64_t samples = 0;
    int value = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_vstream_read_pulse(v, &samples, &value));
    TEST_ASSERT_EQUAL_INT(0, value);
    TEST_ASSERT_EQUAL_UINT64(1, samples);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_vstream_read_pulse(v, &samples, &value));
    TEST_ASSERT_EQUAL_INT(1, value);
    TEST_ASSERT_EQUAL_UINT64(10, samples);
    cmt_stream_destroy(s);
    tzx_free(f);
}

/** @brief Délka velkého WAV v sekundách (10 minut). */
#define BIG_WAV_SECONDS 600

/*
 * Velký WAV: 96 kHz, stereo, float, 10 minut ticha
 * s jedním burstem 1000 period: import doběhne a blok má pod 16 KB dat.
 */
void test_wav_big(void)
{
    const uint32_t rate = 96000;
    const uint32_t frames = rate * BIG_WAV_SECONDS;
    uint32_t size = 0;
    uint8_t *w = make_wav(3, 2, rate, 32, frames, &size);
    TEST_ASSERT_NOT_NULL(w);
    uint32_t start = rate * 30;
    for (uint32_t i = 0; i < 1000 * 20; i++) {
        double v = ((i / 10) % 2) ? 0.5 : -0.5; /* nejdřív záporná = HIGH */
        wav_put(w, 3, 2, start + i, 0, v);
        wav_put(w, 3, 2, start + i, 1, -v);
    }

    gint64 t0 = g_get_monotonic_time();
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_WAV, w, size, NULL, &rep);
    gint64 t1 = g_get_monotonic_time();
    free(w);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    TEST_ASSERT_TRUE(f->blocks[0].length < 16 * 1024);
    uint32_t r = 0;
    static uint32_t p[4];
    /* ticho LOW, 1000 x (HIGH 10, LOW 10), poslední LOW splyne s tichem */
    TEST_ASSERT_EQUAL_UINT32(2001, csw_pulses(&f->blocks[0], &r, p, 4));
    TEST_ASSERT_EQUAL_UINT32(rate, r);
    TEST_ASSERT_EQUAL_UINT32(start, p[0]);
    if (mztest_verbose) {
        printf("big WAV: %u frames, %u B, import %.3f s, block %u B\n", frames, size, (t1 - t0) / 1e6,
               f->blocks[0].length);
    }
    tzx_free(f);
}

/**
 * @brief Sestaví soubor se třemi bloky (0x30 text, 0x10, 0x20) a uloží ho do paměti.
 * @param tmz 1 = signatura TapeMZ!, 0 = ZXTape!.
 * @param[out] file Sestavený soubor (uvolnit tzx_free()).
 * @param[out] h Paměťový handler s uloženým souborem (zavřít generic_driver_close()).
 */
static void make_saved_tape(int tmz, st_TZX_FILE **file, st_HANDLER *h)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    if (tmz) {
        tmz_header_init(&f->header);
        f->is_tmz = true;
    } else {
        tzx_header_init(&f->header);
    }
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description("B3 test", &b));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));

    uint32_t tl = put_tap_block(s_buf, 0xFF, 10, 4);
    b.id = TZX_BLOCK_ID_STANDARD_SPEED;
    b.length = 4 + (tl - 2);
    b.data = (uint8_t *)malloc(b.length);
    TEST_ASSERT_NOT_NULL(b.data);
    b.data[0] = 0xE8; /* 1000 ms */
    b.data[1] = 0x03;
    memcpy(b.data + 2, s_buf, tl);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));

    b.id = TZX_BLOCK_ID_PAUSE;
    b.length = 2;
    b.data = (uint8_t *)malloc(2);
    TEST_ASSERT_NOT_NULL(b.data);
    b.data[0] = 0xF4; /* 500 ms */
    b.data[1] = 0x01;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));

    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(h, f));
    *file = f;
}

/**
 * @brief Tichý error callback knihovny tzx pro testy očekávaných chyb.
 * @param func Funkce. @param line Řádek. @param fmt Formát (ignoruje se).
 */
static void silent_tzx_error(const char *func, int line, const char *fmt, ...)
{
    (void)func;
    (void)line;
    (void)fmt;
}

/*
 * TMZ a TZX uložené tzx_save() se třemi bloky -> načtení vrátí tři bloky
 * se shodnými ID a daty; hlavička souboru zůstává (TZX "ZXTape!").
 */
void test_tmz_load(void)
{
    for (int tmz = 1; tmz >= 0; tmz--) {
        st_TZX_FILE *src = NULL;
        st_HANDLER h;
        make_saved_tape(tmz, &src, &h);

        st_CMTTMZ_IMPORT_REPORT rep;
        st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_TMZ, h.spec.memspec.ptr, (uint32_t)h.spec.memspec.size,
                                              NULL, &rep);
        TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
        TEST_ASSERT_EQUAL_INT(tmz ? 1 : 0, f->is_tmz ? 1 : 0);
        TEST_ASSERT_EQUAL_HEX8_ARRAY((const uint8_t *)&src->header, (const uint8_t *)&f->header, sizeof(f->header));
        TEST_ASSERT_EQUAL_UINT32(3, f->block_count);
        for (uint32_t i = 0; i < 3; i++) {
            TEST_ASSERT_EQUAL_HEX8(src->blocks[i].id, f->blocks[i].id);
            TEST_ASSERT_EQUAL_UINT32(src->blocks[i].length, f->blocks[i].length);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(src->blocks[i].data, f->blocks[i].data, f->blocks[i].length);
        }
        tzx_free(f);
        tzx_free(src);
        generic_driver_close(&h);
    }

    /* useknutý soubor: chyba knihovny textem tzx_error_string() */
    st_TZX_FILE *src = NULL;
    st_HANDLER h;
    make_saved_tape(1, &src, &h);
    st_CMTTMZ_IMPORT_REPORT rep;
    tzx_set_error_callback(silent_tzx_error); /* očekávaná chyba, bez výpisu na stderr */
    st_TZX_FILE *bad = cmttmz_import_buffer(CMTTMZ_SOURCE_TMZ, h.spec.memspec.ptr, (uint32_t)h.spec.memspec.size - 1,
                                            NULL, &rep);
    tzx_set_error_callback(NULL);
    TEST_ASSERT_NULL(bad);
    TEST_ASSERT_EQUAL_STRING(tzx_error_string(TZX_ERROR_UNEXPECTED_EOF), rep.error);
    tzx_free(src);
    generic_driver_close(&h);
}

/*
 * Rychlost bloků z importu:
 *   MZF -> 1 záznam DEFAULT s opts->default_speed, pauza 0,
 *   MZT A + CMTSPEED + B + C -> A DEFAULT (pauza 1000), B a C FIXED CUSTOM
 *   s pulzy z hlavičky (B pauza 1000, C poslední 0),
 *   TAP hlavička + data -> FIXED 1:1, pauzy 350 a 0,
 *   TMZ -> NONE u všech bloků (soubor ze souboru přes _file_speeds),
 *   speeds = NULL se chová jako cmttmz_import_buffer().
 */
void test_import_speeds(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_CMTTMZ_IMPORT_OPTS opts;
    cmttmz_import_opts_init(&opts);
    opts.default_speed = CMTSPEED_3_1;
    st_CMTTMZ_IMPORT_REPORT rep;
    st_CMTTMZ_BLOCK_SPEED *sp = NULL;

    /* MZF */
    uint32_t size = put_mzf(s_buf, "SPEED MZF", 32, 32, 1);
    st_TZX_FILE *f = cmttmz_import_buffer_speeds(CMTTMZ_SOURCE_MZF, s_buf, size, &opts, &rep, &sp);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQUAL_UINT32(1, f->block_count);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp[0].origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, sp[0].speed);
    TEST_ASSERT_EQUAL_UINT16(0, sp[0].pause_ms);
    free(sp);
    tzx_free(f);

    /* MZT: A, CMTSPEED 3x, B, C */
    const uint16_t us3[4] = {156, 164, 80, 92};
    size = put_mzf(s_buf, "A", 16, 16, 2);
    size += put_cmtspeed(s_buf + size, us3);
    size += put_mzf(s_buf + size, "B", 16, 16, 3);
    size += put_mzf(s_buf + size, "C", 16, 16, 4);
    opts.default_speed = CMTSPEED_1_1;
    f = cmttmz_import_buffer_speeds(CMTTMZ_SOURCE_MZT, s_buf, size, &opts, &rep, &sp);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQUAL_UINT32(3, f->block_count);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp[0].origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, sp[0].speed);
    TEST_ASSERT_EQUAL_UINT16(1000, sp[0].pause_ms);
    for (int i = 1; i <= 2; i++) {
        TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp[i].origin);
        TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, sp[i].speed);
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 156e-6, sp[i].pulses.long_pulse.high);
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 164e-6, sp[i].pulses.long_pulse.low);
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 80e-6, sp[i].pulses.short_pulse.high);
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 92e-6, sp[i].pulses.short_pulse.low);
    }
    TEST_ASSERT_EQUAL_UINT16(1000, sp[1].pause_ms);
    TEST_ASSERT_EQUAL_UINT16(0, sp[2].pause_ms);
    free(sp);
    tzx_free(f);

    /* TAP: hlavička 19 B + data */
    size = put_tap_block(s_buf, 0x00, 19, 5);
    size += put_tap_block(s_buf + size, 0xFF, 12, 6);
    f = cmttmz_import_buffer_speeds(CMTTMZ_SOURCE_TAP, s_buf, size, &opts, &rep, &sp);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQUAL_UINT32(2, f->block_count);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp[i].origin);
        TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, sp[i].speed);
    }
    TEST_ASSERT_EQUAL_UINT16(350, sp[0].pause_ms);
    TEST_ASSERT_EQUAL_UINT16(0, sp[1].pause_ms);
    free(sp);
    tzx_free(f);

    /* TMZ ze souboru */
    st_TZX_FILE *src = NULL;
    st_HANDLER h;
    make_saved_tape(1, &src, &h);
    gchar *path = g_build_filename(g_get_tmp_dir(), "mztest_cmt_tmz_import_speeds.tmz", NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    f = cmttmz_import_file_speeds(path, &opts, &rep, &sp);
    g_remove(path);
    g_free(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQUAL_UINT32(src->block_count, f->block_count);
    for (uint32_t i = 0; i < f->block_count; i++) {
        TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_NONE, sp[i].origin);
        TEST_ASSERT_EQUAL_INT(CMTSPEED_NONE, sp[i].speed);
        TEST_ASSERT_EQUAL_UINT16(0, sp[i].pause_ms);
    }
    free(sp);
    tzx_free(f);
    tzx_free(src);

    /* chyba: pole se nevytvoří */
    sp = (st_CMTTMZ_BLOCK_SPEED *)&opts; /* nesmí zůstat */
    TEST_ASSERT_NULL(cmttmz_import_buffer_speeds(CMTTMZ_SOURCE_TAP, NULL, 0, &opts, &rep, &sp));
    TEST_ASSERT_NULL(sp);
}

/**
 * @brief Porovná blok 0x11 přehraný při 44,1 kHz se zxtape v dané rychlosti.
 *
 * Kontroluje strukturu (pilot + 2 sync + 16 půlvln na bajt, zxtape navíc
 * závěrečný běh) a délky půlvln pilotu, sync a bitů na 1 vzorek.
 *
 * @param block Blok 0x11. @param tap Data bloku TAP (flag + data + checksum).
 * @param len Délka dat. @param speed Rychlost zxtape reference.
 */
static void assert_tap_block_vs_zxtape(const st_TZX_BLOCK *block, const uint8_t *tap, uint16_t len, en_CMTSPEED speed)
{
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = 44100;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *got = tmz_player_play_block(block, &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, tmz_player_error_string(perr));
    st_CMT_STREAM *ref = zxtape_create_stream_from_tapblock((en_ZXTAPE_BLOCK_FLAG)tap[0], (uint8_t *)tap, len, speed,
                                                            44100, CMT_STREAM_TYPE_VSTREAM);
    TEST_ASSERT_NOT_NULL(ref);

    st_RUNS rg, rr;
    runs_collect(got->str.vstream, &rg);
    runs_collect(ref->str.vstream, &rr);
    uint32_t pilot = 2u * (tap[0] ? ZXTAPE_DATA_PILOT_LENGTH : ZXTAPE_HEADER_PILOT_LENGTH);
    uint32_t body = pilot + 2 + (uint32_t)len * 16u;
    TEST_ASSERT_EQUAL_UINT32(body, rg.n);
    TEST_ASSERT_EQUAL_UINT32(body + 1, rr.n);
    double ag, ar;
    TEST_ASSERT_TRUE_MESSAGE(runs_max_diff(&rg, &rr, 0, pilot, &ag, &ar) <= 1, "pilot");
    TEST_ASSERT_TRUE_MESSAGE(runs_max_diff(&rg, &rr, pilot, 1, &ag, &ar) <= 1, "sync1");
    TEST_ASSERT_TRUE_MESSAGE(runs_max_diff(&rg, &rr, pilot + 1, 1, &ag, &ar) <= 1, "sync2");
    TEST_ASSERT_TRUE_MESSAGE(runs_max_diff(&rg, &rr, pilot + 2, body - pilot - 2, &ag, &ar) <= 1, "data half-waves");
    runs_free(&rg);
    runs_free(&rr);
    cmt_stream_destroy(got);
    cmt_stream_destroy(ref);
}

/*
 * cmttmz_build_tap_block(): rychlosti zxtape (g_zxtape_speed[]) a 2:1 proti
 * zxtape_create_stream_from_tapblock() ve stejné rychlosti; 1:1 dá bajtově
 * stejný blok jako import TAP; chybné parametry NULL.
 */
void test_tap_block_speed_vs_zxtape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const struct {
        uint8_t flag;
        uint16_t len;
    } cases[2] = {{0x00, 19}, {0xFF, 10}};
    for (int c = 0; c < 2; c++) {
        uint32_t size = put_tap_block(s_buf, cases[c].flag, cases[c].len, 9);
        const uint8_t *tap = s_buf + 2;
        en_CMTSPEED speeds[8];
        int n = 0;
        for (int i = 0; g_zxtape_speed[i] != CMTSPEED_NONE; i++) speeds[n++] = g_zxtape_speed[i];
        speeds[n++] = CMTSPEED_2_1;
        for (int i = 0; i < n; i++) {
            st_TZX_BLOCK *b = cmttmz_build_tap_block(tap, cases[c].len, speeds[i], 0);
            TEST_ASSERT_NOT_NULL(b);
            TEST_ASSERT_EQUAL_HEX8(TZX_BLOCK_ID_TURBO_SPEED, b->id);
            assert_tap_block_vs_zxtape(b, tap, cases[c].len, speeds[i]);
            tmz_block_free(b);
        }
        /* 1:1 a CMTSPEED_NONE = blok z importu TAP */
        st_CMTTMZ_IMPORT_REPORT rep;
        st_TZX_FILE *f = cmttmz_import_buffer(CMTTMZ_SOURCE_TAP, s_buf, size, NULL, &rep);
        TEST_ASSERT_NOT_NULL_MESSAGE(f, rep.error);
        st_TZX_BLOCK *b = cmttmz_build_tap_block(tap, cases[c].len, CMTSPEED_NONE, 0);
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_EQUAL_UINT32(f->blocks[0].length, b->length);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(f->blocks[0].data, b->data, b->length);
        tmz_block_free(b);
        tzx_free(f);
    }
    TEST_ASSERT_NULL(cmttmz_build_tap_block(NULL, 10, CMTSPEED_1_1, 0));
    TEST_ASSERT_NULL(cmttmz_build_tap_block(s_buf, 0, CMTSPEED_1_1, 0));
    TEST_ASSERT_NULL(cmttmz_build_tap_block(s_buf, 10, CMTSPEED_CUSTOM, 0));
}

/**
 * @brief Uloží soubor do paměti (tzx_save) a importuje ho jako TMZ s rychlostmi bloků.
 * @param f Soubor (funkce ho uvolní). @param[out] speeds Rychlosti (uvolnit free()).
 * @return Importovaný soubor (uvolnit tzx_free()).
 */
static st_TZX_FILE *roundtrip_tmz_speeds(st_TZX_FILE *f, st_CMTTMZ_BLOCK_SPEED **speeds)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    st_CMTTMZ_IMPORT_REPORT rep;
    st_TZX_FILE *g = cmttmz_import_buffer_speeds(CMTTMZ_SOURCE_TMZ, h.spec.memspec.ptr, (uint32_t)h.spec.memspec.size,
                                                 NULL, &rep, speeds);
    TEST_ASSERT_NOT_NULL_MESSAGE(g, rep.error);
    TEST_ASSERT_NOT_NULL(*speeds);
    generic_driver_close(&h);
    tzx_free(f);
    return g;
}

/*
 * TMZ: blok 0x46 MZ800EMU-CMT 3:1 s pauzou 500 ms -> FIXED 3:1, pauza 500;
 * MZ800EMU-CMT s vlastními pulzy -> FIXED CUSTOM s pulzy; ROM-800-NOMINAL
 * a text 0x30 -> NONE.
 */
void test_import_speeds_tmz_emu_profile(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    put_mzf(s_buf, "EMU", 16, 16, 3);
    st_MZF_HEADER hdr;
    memcpy(&hdr, s_buf, sizeof(hdr));
    mzf_header_swap_endianity(&hdr);
    const uint8_t *body = s_buf + 128;

    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;

    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 16, CMTSPEED_3_1, NULL, 500);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 139));
    b = cmttmz_build_mzf_block(&hdr, body, 16, CMTSPEED_CUSTOM, &p, 1000);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);

    en_TMZ_PROFILE_ERROR perr = TMZ_PROFILE_OK;
    b = tmz_profile_build_block("ROM-800-NOMINAL", NULL, &hdr, body, 16, &perr);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);

    st_TZX_BLOCK t;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description("text", &t));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &t));

    st_CMTTMZ_BLOCK_SPEED *sp = NULL;
    st_TZX_FILE *g = roundtrip_tmz_speeds(f, &sp);
    TEST_ASSERT_EQUAL_UINT32(4, g->block_count);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp[0].origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, sp[0].speed);
    TEST_ASSERT_EQUAL_UINT16(500, sp[0].pause_ms);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp[1].origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, sp[1].speed);
    TEST_ASSERT_EQUAL_UINT16(1000, sp[1].pause_ms);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 235e-6, sp[1].pulses.long_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 139e-6, sp[1].pulses.short_pulse.low);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_NONE, sp[2].origin);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_NONE, sp[3].origin);
    free(sp);
    tzx_free(g);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_source_from_filename);
    RUN_TEST(test_import_opts_defaults);
    RUN_TEST(test_import_buffer_unknown_content);
    RUN_TEST(test_build_mzf_block_args);
    RUN_TEST(test_import_file_errors);
    RUN_TEST(test_mzf_roundtrip);
    RUN_TEST(test_mzf_size_edges);
    RUN_TEST(test_mzf_default_speed_from_opts);
    RUN_TEST(test_mzt_unicmt_roundtrip);
    RUN_TEST(test_mzt_markers_warnings_and_tail);
    RUN_TEST(test_mzt_errors);
    RUN_TEST(test_mzf_signal_matches_mztape);
    RUN_TEST(test_mzt_pause_only_between_blocks);
    RUN_TEST(test_import_file_mzf);
    RUN_TEST(test_warning_overflow);
    RUN_TEST(test_tap_import);
    RUN_TEST(test_tap_timing_vs_zxtape);
    RUN_TEST(test_wav_square);
    RUN_TEST(test_wav_errors);
    RUN_TEST(test_wav_block_plays_low_first);
    RUN_TEST(test_wav_big);
    RUN_TEST(test_tmz_load);
    RUN_TEST(test_import_speeds);
    RUN_TEST(test_tap_block_speed_vs_zxtape);
    RUN_TEST(test_import_speeds_tmz_emu_profile);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
