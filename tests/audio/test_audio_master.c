/**
 * @file   test_audio_master.c
 * @brief  Hlavní hlasitost a ztlumení poslechu na výstupu mixu (iface_audio.c).
 *
 * Hlavní hlasitost (iface_audio_set_master_gain()) a ztlumení
 * (iface_audio_set_mute()) násobí hotový smíchaný blok těsně před zvukovým
 * zařízením (iface_audio_apply_master()), za odběrem pro video záznam a
 * zachycení zvuku. Testuje se:
 * - výchozí stav (1,0 a nevypnuto) nechá blok bitově shodný,
 * - ztlumení dá ticho a hlavní hlasitost ani hlasitosti kanálů nezmění,
 * - zisk 0,5 dá poloviční amplitudu,
 * - změna je plynulý přechod přes jeden blok, další blok má stálý násobitel,
 * - meze nastavení (oříznutí na 0 až 1, NaN),
 * - hlasitosti kanálů (gain[], okno Audio Mixer) se nastavením nemění.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <math.h>
#include <string.h>

#include "audio.h"
#include "iface/iface_audio.h"

/** @brief Počet stereo snímků testovacího bloku (jako blok SDL cesty při 50 snímcích/s). */
#define BLOCK_FRAMES 882
/** @brief Počet vzorků testovacího bloku (stereo). */
#define BLOCK_SAMPLES (BLOCK_FRAMES * 2)

/** @brief Vzorový blok: různé hodnoty obou znamének včetně krajních. */
static float s_ref[BLOCK_SAMPLES];
/** @brief Pracovní blok, na který se hlasitost uplatňuje. */
static float s_buf[BLOCK_SAMPLES];

/** @brief Naplní pracovní blok vzorovými hodnotami. */
static void load_block(void)
{
    memcpy(s_buf, s_ref, sizeof(s_buf));
}

void setUp(void)
{
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        s_ref[i] = (float)sin((double)i * 0.37) * ((i % 7 == 0) ? 1.0f : 0.73f);
    }
    s_ref[0] = 1.0f;
    s_ref[1] = -1.0f;
    s_ref[2] = 0.0f;
    /* výchozí stav a ustálený naposledy uplatněný násobitel */
    iface_audio_set_master_gain(1.0f);
    iface_audio_set_mute(false);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
}

void tearDown(void)
{
    iface_audio_set_master_gain(1.0f);
    iface_audio_set_mute(false);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
}

/* Výchozí hodnoty po iface_audio_init(): 1,0 a nevypnuto; blok zůstane bitově shodný. */
void test_master_default_is_bit_identical(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 1.0f);
    TEST_ASSERT_FALSE(iface_audio_get_mute());
    for (int n = 0; n < 3; n++) {
        load_block();
        iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(s_buf, s_ref, sizeof(s_buf)), "the default master volume must not change the block");
    }
    /* neplatné argumenty nic nedělají */
    iface_audio_apply_master(NULL, BLOCK_FRAMES, 2);
    iface_audio_apply_master(s_buf, 0, 2);
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 0);
    TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(s_buf)));
}

/* První blok po startu (nižší hlasitost z INI) a po iface_audio_master_restart(): rovnou cílová hlasitost, žádný přechod z plné. */
void test_master_first_block_has_no_fade(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    iface_audio_set_mute(true);
    iface_audio_master_restart();
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE_MESSAGE(s_buf[i] == 0.0f, "muted from the first sample");

    iface_audio_set_mute(false);
    iface_audio_set_master_gain(0.5f);
    iface_audio_master_restart();
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE_MESSAGE(s_buf[i] == s_ref[i] * 0.5f, "the volume from the first sample");

    /* další změna už zase přechází plynule */
    iface_audio_set_master_gain(1.0f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    TEST_ASSERT_TRUE(fabsf(s_buf[0]) < fabsf(s_ref[0]));
    TEST_ASSERT_TRUE(s_buf[BLOCK_SAMPLES - 1] == s_ref[BLOCK_SAMPLES - 1]);
}

/* Ztlumení: první blok je plynulý přechod k tichu, další bloky jsou ticho; zrušení vrátí původní hlasitost. */
void test_master_mute_gives_silence(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    iface_audio_set_master_gain(0.8f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    iface_audio_set_mute(true);
    TEST_ASSERT_TRUE(iface_audio_get_mute());
    TEST_ASSERT_TRUE_MESSAGE(iface_audio_get_master_gain() == 0.8f, "mute keeps the master volume");

    /* přechod: obálka neroste a poslední snímek je ticho */
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    TEST_ASSERT_TRUE(s_buf[BLOCK_SAMPLES - 1] == 0.0f && s_buf[BLOCK_SAMPLES - 2] == 0.0f);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(s_buf[0]) > 0.5f, "the block fades out, it does not cut");
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        TEST_ASSERT_TRUE(fabsf(s_buf[i]) <= fabsf(s_ref[i]) * 0.8f + 1e-6f);
    }

    for (int n = 0; n < 2; n++) {
        load_block();
        iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
        for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE_MESSAGE(s_buf[i] == 0.0f, "mute gives silence");
    }

    /* zrušení ztlumení: po přechodu zpět původní hlasitost 0,8 */
    iface_audio_set_mute(false);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] == s_ref[i] * 0.8f);
}

/* Zisk 0,5: po přechodu má každý vzorek přesně poloviční amplitudu. */
void test_master_half_gain(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    iface_audio_set_master_gain(0.5f);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 0.5f);
    /* přechod z 1,0 na 0,5: násobitel klesá lineárně, poslední snímek už má 0,5 */
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    TEST_ASSERT_TRUE(s_buf[BLOCK_SAMPLES - 1] == s_ref[BLOCK_SAMPLES - 1] * 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, s_buf[0]);
    const int mid = (BLOCK_FRAMES / 2) * 2;
    if (fabsf(s_ref[mid]) > 0.1f) {
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.75f, s_buf[mid] / s_ref[mid]);
    }

    for (int n = 0; n < 2; n++) {
        load_block();
        iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
        for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE_MESSAGE(s_buf[i] == s_ref[i] * 0.5f, "gain 0.5 halves the amplitude");
    }

    /* mono blok (jeden kanál) se násobí stejně */
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_SAMPLES, 1);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] == s_ref[i] * 0.5f);
}

/* Meze nastavení a nezávislost na hlasitostech kanálů. */
void test_master_limits_and_channel_gains(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    float gains[AUDIO_SRC_CHANNELS_COUNT];
    memcpy(gains, g_iface_audio.gain, sizeof(gains));

    iface_audio_set_master_gain(-0.5f);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 0.0f);
    iface_audio_set_master_gain(3.0f);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 1.0f);
    iface_audio_set_master_gain(NAN);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 1.0f);
    iface_audio_set_master_gain(0.25f);
    iface_audio_set_mute(true);
    iface_audio_set_mute(false);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 0.25f);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(gains, g_iface_audio.gain, sizeof(gains)), "the master volume does not touch the channel gains");

    /* hlasitost 0: ticho i bez ztlumení */
    iface_audio_set_master_gain(0.0f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] == 0.0f);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_master_default_is_bit_identical);
    RUN_TEST(test_master_mute_gives_silence);
    RUN_TEST(test_master_half_gain);
    RUN_TEST(test_master_limits_and_channel_gains);
    RUN_TEST(test_master_first_block_has_no_fade);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
