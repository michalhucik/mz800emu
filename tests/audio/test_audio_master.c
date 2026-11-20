/**
 * @file   test_audio_master.c
 * @brief  Hlavní hlasitost a ztlumení poslechu na výstupu mixu a indikace úrovní (iface_audio.c).
 *
 * Hlavní hlasitost (iface_audio_set_master_gain()) a ztlumení
 * (iface_audio_set_mute()) násobí hotový smíchaný blok těsně před zvukovým
 * zařízením (iface_audio_apply_master()), za odběrem pro video záznam a
 * zachycení zvuku. Testuje se:
 * - výchozí stav (1,0 a nevypnuto) nechá blok bitově shodný,
 * - ztlumení dá ticho a hlavní hlasitost ani hlasitosti kanálů nezmění,
 * - zisk 0,5 dá poloviční amplitudu,
 * - změna je plynulý přechod přes jeden blok, další blok má stálý násobitel,
 * - meze nastavení (oříznutí na 0 až 2, NaN),
 * - hlasitosti kanálů (gain[], okno Audio Mixer) se nastavením nemění,
 * - hlasitost nad 100 % zesiluje a špičky měkce omezí (žádný vzorek mimo [-1, 1]),
 * - rozklad hlasitosti do INI klíčů volume_master a volume_master_boost
 *   (starší INI bez druhého klíče znamená totéž co dřív),
 * - indikace úrovní (iface_audio_mix_block(), iface_audio_level_follow()):
 *   kanál naplno ukáže 1,0, stálá úroveň a ticho 0, doběh je plynulý.
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
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == AUDIO_MASTER_GAIN_MAX);
    TEST_ASSERT_TRUE(AUDIO_MASTER_GAIN_MAX == 2.0f);
    iface_audio_set_master_gain(1.5f);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 1.5f);
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

/*
 * Hlasitost 200 %: po přechodu má vzorek pod polovinou kolena přesně dvojnásobnou amplitudu, větší
 * vzorky se měkce stlačí a žádný neopustí [-1, 1]; pořadí velikostí se zachová. Návrat na 100 % dá po
 * přechodu zase bitově shodný blok.
 */
void test_master_boost_soft_limits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    iface_audio_set_master_gain(2.0f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    /* i během přechodu zůstává blok v rozsahu */
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] >= -1.0f && s_buf[i] <= 1.0f);

    for (int n = 0; n < 2; n++) {
        load_block();
        iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
        int linear = 0, limited = 0;
        for (int i = 0; i < BLOCK_SAMPLES; i++) {
            const float x = s_ref[i];
            TEST_ASSERT_TRUE_MESSAGE(s_buf[i] >= -1.0f && s_buf[i] <= 1.0f, "boosted block stays in range");
            if (fabsf(x) * 2.0f <= AUDIO_OUT_LIMIT_KNEE) {
                TEST_ASSERT_TRUE_MESSAGE(s_buf[i] == x * 2.0f, "below the knee the boost is exactly linear");
                linear++;
            } else {
                TEST_ASSERT_TRUE_MESSAGE(fabsf(s_buf[i]) > AUDIO_OUT_LIMIT_KNEE - 1e-6f, "above the knee the sample is compressed, not cut down");
                TEST_ASSERT_TRUE(fabsf(s_buf[i]) <= fabsf(x) * 2.0f);
                TEST_ASSERT_TRUE((s_buf[i] > 0.0f) == (x > 0.0f));
                limited++;
            }
        }
        TEST_ASSERT_TRUE(linear > 100 && limited > 100);
        /* plný vzorek (1,0 a -1,0) se stlačí těsně pod plný rozsah, ne ořízne na menší hodnotu */
        TEST_ASSERT_TRUE(s_buf[0] > 0.99f && s_buf[0] <= 1.0f);
        TEST_ASSERT_TRUE(s_buf[1] < -0.99f && s_buf[1] >= -1.0f);
    }

    /* 150 %: vzorek 0,4 dá 0,6 (pod kolenem, přesně) */
    iface_audio_set_master_gain(1.5f);
    iface_audio_master_restart();
    for (int i = 0; i < BLOCK_SAMPLES; i++) s_buf[i] = 0.4f;
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] == 0.4f * 1.5f);

    /* zpět na 100 %: přechodový blok ještě omezovačem projde (v rozsahu), další je bitově shodný */
    iface_audio_set_master_gain(1.0f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] >= -1.0f && s_buf[i] <= 1.0f);
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(s_buf, s_ref, sizeof(s_buf)), "100 % is bit identical again");

    /* ztlumení při 200 %: ticho, hlasitost zůstává */
    iface_audio_set_master_gain(2.0f);
    iface_audio_set_mute(true);
    iface_audio_master_restart();
    load_block();
    iface_audio_apply_master(s_buf, BLOCK_FRAMES, 2);
    for (int i = 0; i < BLOCK_SAMPLES; i++) TEST_ASSERT_TRUE(s_buf[i] == 0.0f);
    TEST_ASSERT_TRUE(iface_audio_get_master_gain() == 2.0f);
}

/*
 * INI: klíč volume_master znamená totéž co před rozšířením rozsahu (0 až 4294967295 = 0 až 100 %),
 * klíč volume_master_boost přidává procenta nad 100 %; starší INI bez něj (výchozí 0) dá stejnou
 * hlasitost jako dřív a hodnoty do 100 % se ukládají stejně jako dřív.
 */
void test_master_ini_split(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* starší INI: jen volume_master */
    TEST_ASSERT_TRUE(audio_master_gain_from_cfg(0xFFFFFFFFu, 0) == 1.0f);
    TEST_ASSERT_TRUE(audio_master_gain_from_cfg(0u, 0) == 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, audio_master_gain_from_cfg(0x80000000u, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.6f, audio_master_gain_from_cfg(2576980378u, 0));

    /* nad 100 % */
    TEST_ASSERT_TRUE(audio_master_gain_from_cfg(0xFFFFFFFFu, 50) == 1.5f);
    TEST_ASSERT_TRUE(audio_master_gain_from_cfg(0xFFFFFFFFu, 100) == 2.0f);
    TEST_ASSERT_TRUE_MESSAGE(audio_master_gain_from_cfg(0xFFFFFFFFu, 500) == 2.0f, "boost above 100 is clamped");
    TEST_ASSERT_TRUE_MESSAGE(audio_master_gain_from_cfg(0x80000000u, 100) <= 2.0f, "the sum never exceeds the maximum");

    uint32_t vol = 1, boost = 1;
    audio_master_gain_to_cfg(1.0f, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, vol);
    TEST_ASSERT_EQUAL_UINT32(0, boost);
    audio_master_gain_to_cfg(0.0f, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32(0, vol);
    TEST_ASSERT_EQUAL_UINT32(0, boost);
    audio_master_gain_to_cfg(0.5f, &vol, &boost);
    TEST_ASSERT_UINT32_WITHIN(256, 0x80000000u, vol);
    TEST_ASSERT_EQUAL_UINT32(0, boost);
    audio_master_gain_to_cfg(1.5f, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFFFFFFFu, vol, "above 100 % an older emulator reads 100 %");
    TEST_ASSERT_EQUAL_UINT32(50, boost);
    audio_master_gain_to_cfg(2.0f, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, vol);
    TEST_ASSERT_EQUAL_UINT32(100, boost);
    audio_master_gain_to_cfg(7.0f, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32(100, boost);
    audio_master_gain_to_cfg(NAN, &vol, &boost);
    TEST_ASSERT_EQUAL_UINT32(0, vol);
    TEST_ASSERT_EQUAL_UINT32(0, boost);
    /* NULL výstupy se nezapisují */
    audio_master_gain_to_cfg(1.2f, NULL, NULL);

    /* každé celé procento 0 až 200 se uloží a přečte zpět stejné */
    for (int pct = 0; pct <= 200; pct++) {
        const float gain = (float)pct / 100.0f;
        audio_master_gain_to_cfg(gain, &vol, &boost);
        const float back = audio_master_gain_from_cfg(vol, boost);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, gain, back);
        TEST_ASSERT_EQUAL_INT(pct, (int)floorf(back * 100.0f + 0.5f));
        if (pct <= 100) TEST_ASSERT_EQUAL_UINT32(0, boost);
    }
}

/** @brief Počet vzorků bloku mixu pro testy indikace (blok SDL cesty při 50 snímcích/s). */
#define MIX_FRAMES 882

/** @brief Kanály po filtrech pro test mixu. */
static float s_mix_ch[AUDIO_SRC_CHANNELS_COUNT][MIX_FRAMES];
/** @brief Kanály před filtry pro test mixu. */
static float s_mix_raw[AUDIO_SRC_CHANNELS_COUNT][MIX_FRAMES];
/** @brief Výstup mixu (L, R). */
static float s_mix_out[MIX_FRAMES * 2];

/** @brief Vynuluje vstupy mixu. */
static void mix_clear(void)
{
    memset(s_mix_ch, 0, sizeof(s_mix_ch));
    memset(s_mix_raw, 0, sizeof(s_mix_raw));
}

/**
 * @brief Naplní kanál obdélníkem lo/hi s půlperiodou `half` vzorků (stejně po filtrech i před nimi).
 */
static void mix_square(int ch, float lo, float hi, int half)
{
    for (int i = 0; i < MIX_FRAMES; i++) {
        const float v = ((i / half) & 1) ? hi : lo;
        s_mix_ch[ch][i] = v;
        s_mix_raw[ch][i] = v;
    }
}

/** @brief Smíchá jeden blok z testovacích kanálů (mono rozložení). */
static void mix_run(void)
{
    float *ch[AUDIO_SRC_CHANNELS_COUNT], *raw[AUDIO_SRC_CHANNELS_COUNT];
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        ch[i] = s_mix_ch[i];
        raw[i] = s_mix_raw[i];
    }
    iface_audio_mix_block(ch, raw, s_mix_out, MIX_FRAMES, false);
}

/* Krok indikace: okamžitý náběh, exponenciální doběh s časovou konstantou 300 ms, pod prahem nula. */
void test_level_follow(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_TRUE(iface_audio_level_follow(0.0f, 1.0f, MIX_FRAMES) == 1.0f);
    TEST_ASSERT_TRUE(iface_audio_level_follow(0.2f, 0.9f, MIX_FRAMES) == 0.9f);
    TEST_ASSERT_TRUE(iface_audio_level_follow(0.0f, 0.0f, MIX_FRAMES) == 0.0f);
    /* doběh: jeden blok 20 ms = exp(-20 / 300) */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, (float)exp(-20.0 / (double)IFACE_AUDIO_LEVEL_RELEASE_MS), iface_audio_level_follow(1.0f, 0.0f, MIX_FRAMES));
    /* nižší špička doběh nezkrátí, vyšší ho přebije */
    TEST_ASSERT_TRUE(iface_audio_level_follow(1.0f, 0.5f, MIX_FRAMES) > 0.9f);
    /* meze: nad 1 a NaN */
    TEST_ASSERT_TRUE(iface_audio_level_follow(0.0f, 3.0f, MIX_FRAMES) == 1.0f);
    TEST_ASSERT_TRUE(iface_audio_level_follow(0.0f, NAN, MIX_FRAMES) == 0.0f);
    TEST_ASSERT_TRUE(iface_audio_level_follow(0.0f, -1.0f, MIX_FRAMES) == 0.0f);
    /* doběh k nule: monotónně, za 300 ms na 1/e, do 3 s přesně 0 */
    float v = 1.0f;
    for (int n = 1; n <= 150; n++) {
        const float next = iface_audio_level_follow(v, 0.0f, MIX_FRAMES);
        TEST_ASSERT_TRUE(next <= v);
        v = next;
        if (n == 15) TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.3679f, v);
    }
    TEST_ASSERT_TRUE(v == 0.0f);
}

/*
 * Indikace úrovní z mixu: obdélník naplno na kanálu s hlasitostí 100 % ukáže 1,0 (dřív střední
 * absolutní hodnota, tedy polovinu), hlasitost kanálu ji úměrně sníží, stálá úroveň a ticho dají 0,
 * po utichnutí indikace plynule doběhne. Celková úroveň je špička výstupu za omezovačem.
 */
void test_levels_show_peak(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    float gains[AUDIO_SRC_CHANNELS_COUNT];
    memcpy(gains, g_iface_audio.gain, sizeof(gains));
    const bool saved_filter = iface_audio_output_filter_enabled();
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = 1.0f;
    iface_audio_set_output_filter(false);

    /* ticho */
    iface_audio_mix_restart();
    mix_clear();
    mix_run();
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) TEST_ASSERT_TRUE(g_iface_audio.level[i] == 0.0f);
    TEST_ASSERT_TRUE(g_iface_audio.total_level == 0.0f);
    for (int i = 0; i < MIX_FRAMES * 2; i++) TEST_ASSERT_TRUE(s_mix_out[i] == 0.0f);

    /* CTC0: obdélník souměrný kolem nuly s rozkmitem středního zesílení převzorkování (jako po střídavé vazbě) */
    const float ctc0_unit = AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT / iface_audio_ctc0_mix_level();
    TEST_ASSERT_TRUE(ctc0_unit > 0.8f && ctc0_unit < 1.0f);
    iface_audio_mix_restart();
    mix_clear();
    mix_square(0, -0.5f * ctc0_unit, 0.5f * ctc0_unit, 50);
    mix_run();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 1.0f, g_iface_audio.level[0], "a full CTC0 square wave shows full scale");
    for (int i = 1; i < AUDIO_SRC_CHANNELS_COUNT; i++) TEST_ASSERT_TRUE(g_iface_audio.level[i] == 0.0f);
    /* celková úroveň = špička výstupu: polovina rozkmitu CTC0 (1,49 / 4 / 2) krát dorovnání, pod kolenem */
    float peak = 0.0f;
    for (int i = 0; i < MIX_FRAMES * 2; i++) {
        if (fabsf(s_mix_out[i]) > peak) peak = fabsf(s_mix_out[i]);
    }
    TEST_ASSERT_TRUE(g_iface_audio.total_level == peak);
    TEST_ASSERT_TRUE(peak > 0.1f && peak < AUDIO_OUT_LIMIT_KNEE);
    /* mono rozložení: obě strany stejné */
    for (int i = 0; i < MIX_FRAMES; i++) TEST_ASSERT_TRUE(s_mix_out[i * 2] == s_mix_out[i * 2 + 1]);

#if HAVE_PSG >= 1
    /* kanál PSG naplno (0 až 1): plná výchylka, i když střední absolutní hodnota je polovina */
    iface_audio_mix_restart();
    mix_clear();
    mix_square(1, 0.0f, 1.0f, 50);
    mix_run();
    TEST_ASSERT_TRUE_MESSAGE(g_iface_audio.level[1] == 1.0f, "a full PSG channel shows full scale");
    TEST_ASSERT_TRUE(g_iface_audio.level[0] == 0.0f && g_iface_audio.level[2] == 0.0f);
    TEST_ASSERT_TRUE(g_iface_audio.total_level > 0.0f && g_iface_audio.total_level < AUDIO_OUT_LIMIT_KNEE);

    /* hlasitost kanálu 50 %: poloviční výchylka; útlum kanálu (rozkmit 0,1) úměrně méně */
    iface_audio_mix_restart();
    g_iface_audio.gain[1] = 0.5f;
    mix_square(2, 0.0f, 0.1f, 50);
    mix_run();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, g_iface_audio.level[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, g_iface_audio.level[2]);
    g_iface_audio.gain[1] = 1.0f;

    /* stálá úroveň (není slyšet): 0 */
    iface_audio_mix_restart();
    mix_clear();
    for (int i = 0; i < MIX_FRAMES; i++) s_mix_ch[3][i] = s_mix_raw[3][i] = 1.0f;
    mix_run();
    TEST_ASSERT_TRUE(g_iface_audio.level[3] == 0.0f);

    /* doběh po utichnutí: plynule k nule */
    iface_audio_mix_restart();
    mix_clear();
    mix_square(1, 0.0f, 1.0f, 50);
    mix_run();
    mix_clear();
    float prev = g_iface_audio.level[1];
    float prev_total = g_iface_audio.total_level;
    mix_run();
    TEST_ASSERT_TRUE_MESSAGE(g_iface_audio.level[1] > 0.9f && g_iface_audio.level[1] < 1.0f, "the level decays, it does not drop at once");
    for (int n = 0; n < 200; n++) {
        mix_run();
        TEST_ASSERT_TRUE(g_iface_audio.level[1] <= prev);
        prev = g_iface_audio.level[1];
    }
    TEST_ASSERT_TRUE(g_iface_audio.level[1] == 0.0f);
    TEST_ASSERT_TRUE(g_iface_audio.total_level < prev_total);

    /* nejhorší případ: CTC0 a všechny kanály PSG naplno ve fázi - výstup se stlačí, neořízne */
    iface_audio_mix_restart();
    mix_clear();
    mix_square(0, -0.5f * ctc0_unit, 0.5f * ctc0_unit, 50);
    for (int ch = 1; ch <= PSG_CHANNELS_COUNT; ch++) mix_square(ch, -0.5f, 0.5f, 50);
    mix_run();
    peak = 0.0f;
    int at_one = 0;
    for (int i = 0; i < MIX_FRAMES * 2; i++) {
        TEST_ASSERT_TRUE(s_mix_out[i] >= -1.0f && s_mix_out[i] <= 1.0f);
        if (fabsf(s_mix_out[i]) > peak) peak = fabsf(s_mix_out[i]);
        if (fabsf(s_mix_out[i]) >= 1.0f) at_one++;
    }
    TEST_ASSERT_TRUE_MESSAGE(peak > AUDIO_OUT_LIMIT_KNEE && peak < 1.0f, "the worst case is compressed below full scale");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, at_one, "no sample is clipped to full scale");
    TEST_ASSERT_TRUE(g_iface_audio.total_level == peak);
#endif /* HAVE_PSG >= 1 */

    iface_audio_mix_restart();
    iface_audio_set_output_filter(saved_filter);
    memcpy(g_iface_audio.gain, gains, sizeof(gains));
}

/*
 * Poměry zdrojů a dorovnání hlasitosti v mixu: tón CTC0 má 1,49 násobek výchylky kanálu PSG naplno
 * (pod kolenem), s vypnutou propustí je výstup součet kanálů krát +4 dB, se zapnutou stálá složka
 * odezní a ustálený rozkmit pomalého obdélníku je krát +8 dB.
 */
void test_mix_ratios_and_makeup(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    float gains[AUDIO_SRC_CHANNELS_COUNT];
    memcpy(gains, g_iface_audio.gain, sizeof(gains));
    const bool saved_filter = iface_audio_output_filter_enabled();
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = 1.0f;
    const float ctc0_unit = AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT / iface_audio_ctc0_mix_level();
    const float off = powf(10.0f, (float)AUDIO_OUT_MAKEUP_FILTER_OFF_DB / 20.0f);

    /* CTC0 samotný, propust vypnutá: rozkmit = 1,49 / 4 krát dorovnání */
    iface_audio_set_output_filter(false);
    iface_audio_mix_restart();
    mix_clear();
    mix_square(0, -0.5f * ctc0_unit, 0.5f * ctc0_unit, 50);
    mix_run();
    float mn = 1.0f, mx = -1.0f;
    for (int i = 0; i < MIX_FRAMES; i++) {
        if (s_mix_out[i * 2] < mn) mn = s_mix_out[i * 2];
        if (s_mix_out[i * 2] > mx) mx = s_mix_out[i * 2];
    }
    const float ctc0_swing = mx - mn;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT * off, ctc0_swing);

#if HAVE_PSG >= 1
    /* kanál PSG naplno souměrný kolem nuly: rozkmit 1 / 4 krát dorovnání; poměr k CTC0 je 1,49 */
    iface_audio_mix_restart();
    mix_clear();
    mix_square(1, -0.5f, 0.5f, 50);
    mix_run();
    mn = 1.0f;
    mx = -1.0f;
    for (int i = 0; i < MIX_FRAMES; i++) {
        if (s_mix_out[i * 2] < mn) mn = s_mix_out[i * 2];
        if (s_mix_out[i * 2] > mx) mx = s_mix_out[i * 2];
    }
    const float psg_swing = mx - mn;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, off / (float)PSG_CHANNELS_COUNT, psg_swing);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, AUDIO_CTC0_TO_PSG_CHANNEL_RATIO, ctc0_swing / psg_swing, "the makeup gain keeps the CTC0 to PSG ratio");
#endif

#if AUDIO_HAVE_OUTPUT_FILTER
    /* propust zapnutá: pomalý obdélník (půlperioda 441 vzorků = 10 ms, 22 časových konstant) se ustálí na rozkmitu krát +8 dB */
    const float on = powf(10.0f, (float)AUDIO_OUT_MAKEUP_FILTER_ON_DB / 20.0f);
    iface_audio_set_output_filter(true);
    iface_audio_mix_restart();
    mix_clear();
    mix_square(0, -0.5f * ctc0_unit, 0.5f * ctc0_unit, 441);
    for (int n = 0; n < 3; n++) mix_run();
    mn = 1.0f;
    mx = -1.0f;
    for (int i = 0; i < MIX_FRAMES; i++) {
        if (s_mix_out[i * 2] < mn) mn = s_mix_out[i * 2];
        if (s_mix_out[i * 2] > mx) mx = s_mix_out[i * 2];
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT * on, mx - mn);
    TEST_ASSERT_TRUE(mx < AUDIO_OUT_LIMIT_KNEE);
#endif

    iface_audio_mix_restart();
    iface_audio_set_output_filter(saved_filter);
    memcpy(g_iface_audio.gain, gains, sizeof(gains));
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
    RUN_TEST(test_master_boost_soft_limits);
    RUN_TEST(test_master_ini_split);
    RUN_TEST(test_level_follow);
    RUN_TEST(test_levels_show_peak);
    RUN_TEST(test_mix_ratios_and_makeup);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
