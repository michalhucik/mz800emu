/**
 * @file test_cmt_unicmt.c
 * @brief Testy vlastních délek pulzů Virtual CMT a hlaviček CMTSPEED (UniCMT) v MZT.
 *
 * Hlavička CMTSPEED zařízení UniCMT je 128bajtový MZF blok bez těla
 * (typ 00h, jméno "CMTSPEED" + 0Dh, na offsetu 30h 4x uint16 LE délky
 * pulzů v µs), který mění rychlost dalších dílů pásky. Testuje se:
 *   - index MZT: hlavička se do seznamu bloků nedostane, další bloky mají
 *     pevnou rychlost CMTSPEED_CUSTOM s délkami z hlavičky až do další
 *     hlavičky, bloky před první hlavičkou výchozí rychlost,
 *   - hlavička na prvním místě, neplatná hlavička (nulová délka), MZT jen
 *     s hlavičkami,
 *   - otevřený blok přehrává pulzy z hlavičky (počet taktů GDG, Bd),
 *   - per-blok vlastní pulzy (cmt_tape_set_block_pulses) a efektivní
 *     pulzy bloků (cmt_tape_get_block_pulses),
 *   - výchozí vlastní pulzy (cmt_change_custom_pulses) přegenerují
 *     vložený blok s výchozí rychlostí.
 *
 * Testovací MZF/MZT se generují do dočasného adresáře (g_get_tmp_dir).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/gdg/gdgclk.h"

/** @brief Délka těla testovacích MZF v bajtech. */
#define TEST_BODY_SIZE 64

/** @brief Dočasný soubor pásky (vytváří test, maže tearDown). */
static char *s_tape_path = NULL;

/** @brief Výchozí rychlost a vlastní pulzy před testem (obnovuje tearDown). */
static en_CMTSPEED s_saved_speed;
static st_MZTAPE_PULSES_LENGTH s_saved_pulses;

/**
 * @brief Přidá do bufferu MZF blok (typ 01h, tělo TEST_BODY_SIZE bajtů).
 *
 * @param buf Cílový buffer.
 * @param name Jméno souboru (max 16 znaků).
 */
static void append_mzf(GByteArray *buf, const char *name)
{
    uint8_t hdr[128];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x01;
    memset(&hdr[1], 0x0D, 17);
    memcpy(&hdr[1], name, strlen(name));
    hdr[0x12] = TEST_BODY_SIZE;
    hdr[0x13] = 0x00;
    hdr[0x14] = 0x00; /* fstrt = 1200h */
    hdr[0x15] = 0x12;
    hdr[0x16] = 0x00; /* fexec = 1200h */
    hdr[0x17] = 0x12;
    g_byte_array_append(buf, hdr, sizeof(hdr));
    for (int i = 0; i < TEST_BODY_SIZE; i++) {
        uint8_t b = (uint8_t)((i * 37 + 11) & 0xFF);
        g_byte_array_append(buf, &b, 1);
    }
}

/**
 * @brief Přidá do bufferu hlavičku CMTSPEED (jako soubory 1x/2x/3xspeed.mzf UniCMT).
 *
 * @param buf Cílový buffer.
 * @param lh Délka high dlouhého pulzu (µs).
 * @param ll Délka low dlouhého pulzu (µs).
 * @param sh Délka high krátkého pulzu (µs).
 * @param sl Délka low krátkého pulzu (µs).
 */
static void append_cmtspeed(GByteArray *buf, uint16_t lh, uint16_t ll, uint16_t sh, uint16_t sl)
{
    uint8_t hdr[128];
    const uint16_t us[4] = {lh, ll, sh, sl};
    memset(hdr, 0, sizeof(hdr));
    memcpy(&hdr[0x01], "CMTSPEED\r", 9);
    hdr[0x20] = 0x01;
    for (int i = 0; i < 4; i++) {
        hdr[0x30 + i * 2] = (uint8_t)(us[i] & 0xFF);
        hdr[0x31 + i * 2] = (uint8_t)(us[i] >> 8);
    }
    g_byte_array_append(buf, hdr, sizeof(hdr));
}

/**
 * @brief Zapíše buffer jako pásku (.mzt nebo .mzf) a vrátí výsledek cmt_open_file_by_extension.
 *
 * @param buf Data pásky (funkce ho uvolní).
 * @param ext Přípona souboru ("mzt" / "mzf").
 * @return Návratová hodnota cmt_open_file_by_extension().
 */
static int open_tape(GByteArray *buf, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_unicmt_%u.%s", (unsigned)g_random_int(), ext);
    g_free(s_tape_path);
    s_tape_path = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    TEST_ASSERT_TRUE(g_file_set_contents(s_tape_path, (const gchar *)buf->data, buf->len, NULL));
    g_byte_array_free(buf, TRUE);
    return cmt_open_file_by_extension(s_tape_path);
}

/** @brief Ověří délky pulzů v µs (tolerance 1e-6 µs). */
static void assert_pulses_us(const st_MZTAPE_PULSES_LENGTH *p, double lh, double ll, double sh, double sl)
{
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, lh / 1e6, p->long_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, ll / 1e6, p->long_pulse.low);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, sh / 1e6, p->short_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, sl / 1e6, p->short_pulse.low);
}

/**
 * @brief Ověří první krátký pulz otevřeného bloku (začátek dlouhého GAPu) v taktech GDG.
 *
 * @param sh_us Délka high krátkého pulzu (µs).
 * @param sl_us Délka low krátkého pulzu (µs).
 */
static void assert_block_first_pulse(double sh_us, double sl_us)
{
    st_CMT_STREAM *stream = g_cmt.ext->block->stream;
    TEST_ASSERT_EQUAL_INT(CMT_STREAM_TYPE_VSTREAM, stream->stream_type);
    st_CMT_VSTREAM *v = stream->str.vstream;
    TEST_ASSERT_EQUAL_UINT32(GDGCLK_BASE, cmt_vstream_get_rate(v));
    cmt_vstream_read_reset(v);
    uint64_t samples;
    int value;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_vstream_read_pulse(v, &samples, &value));
    TEST_ASSERT_EQUAL_INT(1, value);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)round(sh_us * 1e-6 * GDGCLK_BASE), samples);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_vstream_read_pulse(v, &samples, &value));
    TEST_ASSERT_EQUAL_INT(0, value);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)round(sl_us * 1e-6 * GDGCLK_BASE), samples);
    cmt_vstream_read_reset(v);
}

void setUp(void)
{
    s_saved_speed = g_cmt.mz_cmtspeed;
    s_saved_pulses = g_cmt.mz_custom_pulses;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
}

void tearDown(void)
{
    cmt_eject();
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.mz_custom_pulses = s_saved_pulses;
    if (s_tape_path) {
        g_remove(s_tape_path);
        g_free(s_tape_path);
        s_tape_path = NULL;
    }
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/*
 * A + 3xspeed + B + 1xspeed + C: hlavičky zmizí, B má pulzy 3x, C pulzy 1x,
 * A výchozí rychlost.
 */
void test_mzt_markers_set_speed_of_following_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    GByteArray *buf = g_byte_array_new();
    append_mzf(buf, "PART A");
    append_cmtspeed(buf, 156, 164, 80, 92);
    append_mzf(buf, "PART B");
    append_cmtspeed(buf, 470, 494, 240, 278);
    append_mzf(buf, "PART C");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, open_tape(buf, "mzt"));

    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_NOT_NULL(container);
    TEST_ASSERT_EQUAL_INT(3, cmtext_container_get_count_blocks(container));
    TEST_ASSERT_EQUAL_STRING("PART A", cmtext_container_get_block_fname(container, 0));
    TEST_ASSERT_EQUAL_STRING("PART B", cmtext_container_get_block_fname(container, 1));
    TEST_ASSERT_EQUAL_STRING("PART C", cmtext_container_get_block_fname(container, 2));

    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(container, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(container, 1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(container, 2));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(container, 2));

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(1, &p));
    assert_pulses_us(&p, 156, 164, 80, 92);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(2, &p));
    assert_pulses_us(&p, 470, 494, 240, 278);

    /* blok A má výchozí rychlost 1:1 = délky Intercopy */
    st_MZTAPE_PULSES_LENGTH nominal;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_get_speed_pulses(CMTTMZ_FORMATSET, CMTSPEED_1_1, &nominal));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(0, &p));
    TEST_ASSERT_EQUAL_MEMORY(&nominal, &p, sizeof(p));

    /* otevřený blok A hraje 1:1, blok B pulzy 3x z hlavičky */
    TEST_ASSERT_EQUAL_UINT16(1200, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, g_cmt.ext->container->cb_next_block());
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    assert_block_first_pulse(80, 92);
    /* ekvivalent 3,0257:1 vůči 1200 Bd */
    TEST_ASSERT_EQUAL_UINT16(3631, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));

    /* blok B nemá výchozí rychlost - změna výchozí rychlosti ho nepřegeneruje */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    assert_block_first_pulse(80, 92);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, g_cmt.ext->container->cb_next_block());
    assert_block_first_pulse(240, 278);
}

/* Hlavička jako první blok se v emulátoru použije (HW ji nesnese, nenapodobujeme). */
void test_mzt_marker_first_block_applies(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    GByteArray *buf = g_byte_array_new();
    append_cmtspeed(buf, 235, 247, 120, 139);
    append_mzf(buf, "ONLY");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, open_tape(buf, "mzt"));

    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(container));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(container, 0));
    assert_block_first_pulse(120, 139);
}

/* Hlavička s nulovou délkou pulzu se zahodí a rychlost nemění. */
void test_mzt_invalid_marker_ignored(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    GByteArray *buf = g_byte_array_new();
    append_mzf(buf, "PART A");
    append_cmtspeed(buf, 156, 164, 0, 92);
    append_mzf(buf, "PART B");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, open_tape(buf, "mzt"));

    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(container));
    TEST_ASSERT_EQUAL_STRING("PART B", cmtext_container_get_block_fname(container, 1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 1));
}

/* MZT jen s hlavičkami CMTSPEED nemá co přehrát - otevření selže. */
void test_mzt_only_markers_fails(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    GByteArray *buf = g_byte_array_new();
    append_cmtspeed(buf, 156, 164, 80, 92);
    append_cmtspeed(buf, 470, 494, 240, 278);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, open_tape(buf, "mzt"));
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
}

/* Per-blok vlastní pulzy: blok přejde na SET + CUSTOM, chyby nic nemění. */
void test_block_pulses_set_and_get(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 176, 185, 90, 104));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_pulses(0, &p)); /* bez pásky */
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_get_block_pulses(0, &p));

    GByteArray *buf = g_byte_array_new();
    append_mzf(buf, "PART A");
    append_mzf(buf, "PART B");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, open_tape(buf, "mzt"));
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);

    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_pulses(2, &p));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_pulses(-1, &p));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 1));

    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_pulses(1, &p));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(container, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(container, 1));

    st_MZTAPE_PULSES_LENGTH got;
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(1, &got));
    assert_pulses_us(&got, 176, 185, 90, 104);
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_get_block_pulses(2, &got));

    /* projeví se při otevření bloku */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, g_cmt.ext->container->cb_next_block());
    assert_block_first_pulse(90, 104);

    /* zpět na poměr přes cmt_tape_set_block_cmt_speed; CUSTOM tudy nejde (chybí délky) */
    TEST_ASSERT_EQUAL_INT(-2, cmt_tape_set_block_cmt_speed(1, CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    st_MZTAPE_PULSES_LENGTH r3;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_get_speed_pulses(CMTTMZ_FORMATSET, CMTSPEED_3_1, &r3));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(1, &got));
    TEST_ASSERT_EQUAL_MEMORY(&r3, &got, sizeof(got));
}

/*
 * Výchozí vlastní pulzy: vložené samostatné MZF (výchozí rychlost) se při
 * STOP přegeneruje; efektivní pulzy bloku odpovídají.
 */
void test_default_custom_pulses_regenerate_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    GByteArray *buf = g_byte_array_new();
    append_mzf(buf, "SINGLE");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, open_tape(buf, "mzf"));
    TEST_ASSERT_EQUAL_UINT16(1200, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));

    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 156, 164, 80, 92));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_custom_pulses(&p));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, g_cmt.mz_cmtspeed);
    assert_pulses_us(&g_cmt.mz_custom_pulses, 156, 164, 80, 92);
    assert_block_first_pulse(80, 92);

    st_MZTAPE_PULSES_LENGTH got;
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_get_block_pulses(0, &got));
    assert_pulses_us(&got, 156, 164, 80, 92);
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_get_block_pulses(1, &got));

    /* jiné vlastní pulzy při již vlastní rychlosti se taky projeví */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 139));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_custom_pulses(&p));
    assert_block_first_pulse(120, 139);

    /* návrat na poměr; vlastní pulzy zůstanou uložené */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_1_1));
    TEST_ASSERT_EQUAL_UINT16(1200, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    assert_pulses_us(&g_cmt.mz_custom_pulses, 235, 247, 120, 139);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_CUSTOM));
    assert_block_first_pulse(120, 139);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_mzt_markers_set_speed_of_following_blocks);
    RUN_TEST(test_mzt_marker_first_block_applies);
    RUN_TEST(test_mzt_invalid_marker_ignored);
    RUN_TEST(test_mzt_only_markers_fails);
    RUN_TEST(test_block_pulses_set_and_get);
    RUN_TEST(test_default_custom_pulses_regenerate_block);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
