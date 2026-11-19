/**
 * @file   test_audio_ctc0_ac.c
 * @brief  Střídavá vazba CTC0 vedená emulátorem v emulovaném čase (audio.c).
 *
 * Na MZ-800 je CTC0 (i echo kazety, které jde stejným kanálem) do zvukového
 * výstupu vázaný střídavě: obdélník je souměrný kolem klidu a stálá úroveň
 * se ke klidu vrací s časovou konstantou 0,85 s (AUDIO_CTC0_AC_TAU_MS).
 * Emulátor vede střední hodnotu kanálu (audio_ctc0_ac_mean()); výstup vazby je
 * úroveň minus střední hodnota. Testuje se:
 * - stálá úroveň klesne za 0,85 s na 1/e (tolerance 10 %),
 * - obdélník je souměrný kolem nuly (střední hodnota 0,5),
 * - echo kazety dál mění kanál CTC0 zvukového logu a prochází stejnou vazbou,
 * - nový zvukový log nese stav vazby pro SDL cestu,
 * - střídavá vazba celého výstupu (AUDIO_OUT_AC_TAU_MS, MZ-800 a MZ-1500):
 *   střední výchylka kanálu PSG a druhý stupeň pro CTC0.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <math.h>
#include <string.h>

#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/psg/psg.h"
#include "iface/iface_audio.h"

/** @brief Časová konstanta střídavé vazby v ticích GDG. */
#define TAU_TICKS ((uint64_t)GDGCLK_BASE * AUDIO_CTC0_AC_TAU_MS / 1000u)

/** @brief Čas, od kterého test posílá události (za vším, co log dosud viděl). */
static uint64_t s_t0;

void setUp(void)
{
    /* Čistý log a ustálená vazba v čase za dosavadními událostmi. */
    s_t0 = g_audio.log->last_psg_timestamp + 10u * (uint64_t)GDGCLK_BASE;
    g_audio.ctc0_output = 0;
    g_audio.ctc0_div2 = 0;
    g_audio.cmt_echo = 0;
    audio_reset_log(s_t0);
}

void tearDown(void) { }

/** @brief Hodnota kanálu CTC0 ve zvukovém logu. */
static int ctc0_log_value(void)
{
    return g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)]->last_value;
}

/** @brief Výstup střídavé vazby (úroveň minus střední hodnota) v čase @p t. */
static double ac_output(uint64_t t)
{
    double mean = audio_ctc0_ac_mean(t);
    return g_audio.ctc0_ac.level - mean;
}

/* Po resetu logu je vazba ustálená: stálá úroveň nedává nic. */
void test_ctc0_ac_settled_after_reset(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, ac_output(s_t0 + TAU_TICKS));

    g_audio.ctc0_output = 1;
    audio_reset_log(s_t0 + 2 * TAU_TICKS);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, g_audio.ctc0_ac.level);
    /* log po resetu nese úroveň kanálu hned, bez čekání na další hranu */
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, ac_output(s_t0 + 3 * TAU_TICKS));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, g_audio.log->ctc0_ac_mean);
}

/* Stálá úroveň CTC0 klesne za 0,85 s na 1/e (+-10 %). */
void test_ctc0_ac_constant_level_decays(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    audio_ctc0_changed(true, s_t0 + 1000);
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0, ac_output(s_t0 + 1000));              /* skok projde celý */
    double e = exp(-1.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * e, e, ac_output(s_t0 + 1000 + TAU_TICKS)); /* 0,85 s */
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, e, ac_output(s_t0 + 1000 + TAU_TICKS));    /* model je přesný */
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.0, ac_output(s_t0 + 1000 + 10 * TAU_TICKS));

    /* návrat do nuly dá stejně velký skok dolů a doznívá stejně */
    uint64_t t1 = s_t0 + 1000 + 10 * TAU_TICKS;
    audio_ctc0_changed(false, t1);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -1.0, ac_output(t1));
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * e, -e, ac_output(t1 + TAU_TICKS));

    /* čas před posledním dopočtem stav nemění */
    double m = g_audio.ctc0_ac.mean;
    TEST_ASSERT_DOUBLE_WITHIN(1e-15, m, audio_ctc0_ac_mean(t1));
}

/* Obdélník CTC0 (440 Hz, 5 s) je souměrný kolem nuly: střední hodnota 0,5. */
void test_ctc0_ac_square_symmetric(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const uint64_t half = (uint64_t)GDGCLK_BASE / 880u;
    uint64_t t = s_t0 + 1000;
    bool level = false;
    while (t < s_t0 + 5u * (uint64_t)GDGCLK_BASE) {
        level = !level;
        audio_ctc0_changed(level, t);
        t += half;
    }
    /* po 5,9 časových konstantách: horní půlvlna +0,5, dolní -0,5 */
    double mean = audio_ctc0_ac_mean(t);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.5, mean);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, level ? 0.5 : -0.5, ac_output(t));
}

/* Echo kazety dál hraje: mění kanál CTC0 zvukového logu (XOR s CTC0) a jde stejnou vazbou. */
void test_cmt_echo_plays_through_ac_coupling(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint64_t t = s_t0 + 1000;
    audio_cmt_echo_changed(true, t);
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, g_audio.ctc0_ac.level);
    audio_cmt_echo_changed(false, t + 5000);
    TEST_ASSERT_EQUAL_INT(0, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, g_audio.ctc0_ac.level);
    /* události echa jsou ve zvukovém logu jako hrany CTC0 */
    TEST_ASSERT_EQUAL_UINT(3, g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)]->samples_count);

    /* s CTC0 v jedničce je echo obrácené (XOR), výstup CTC0 se nemění */
    audio_ctc0_changed(true, t + 10000);
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    audio_cmt_echo_changed(true, t + 15000);
    TEST_ASSERT_EQUAL_INT(0, ctc0_log_value());
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_output);
    audio_cmt_echo_changed(false, t + 20000);
    audio_ctc0_changed(false, t + 25000);

    /* signál pásky 1200 Hz po dobu 5 s: souměrný kolem nuly jako tón CTC0 */
    const uint64_t half = (uint64_t)GDGCLK_BASE / 2400u;
    bool level = false;
    t += 30000;
    while (t < s_t0 + 5u * (uint64_t)GDGCLK_BASE) {
        level = !level;
        audio_cmt_echo_changed(level, t);
        t += half;
    }
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.5, audio_ctc0_ac_mean(t));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, level ? 0.5 : -0.5, ac_output(t));
    audio_cmt_echo_changed(false, t);
}

/* Nový zvukový log nese střední hodnotu CTC0 pro SDL cestu. */
void test_ctc0_ac_mean_carried_by_log(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, g_audio.log->ctc0_ac_mean);
    audio_ctc0_changed(true, s_t0 + 1000);
    uint64_t frame_end = s_t0 + 1000 + TAU_TICKS;
    audiolog_finish_20ms_frame(frame_end);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0 - exp(-1.0), g_audio.log->ctc0_ac_mean);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, g_audio.ctc0_ac.mean, g_audio.log->ctc0_ac_mean);
}

/* Výchylky hodnot zvukového logu jsou stejné jako tabulka SDL cesty při hlasitosti 100 %. */
void test_audio_level_amplitude_matches_sdl_table(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    iface_audio_set_src_volume(1, 100);
    /* hodnoty kanálů PSG (0 až 30); hodnota 31 patří jen kanálu CTC0 */
    for (unsigned v = 0; v < AUDIO_LEVEL_CTC0_DIV2; v++) {
        TEST_ASSERT_DOUBLE_WITHIN(1e-6, (double)g_iface_audio.SN76489_volume_value[1][v], audio_level_amplitude(v));
    }
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, (double)g_iface_audio.SN76489_volume_value[1][AUDIO_LEVEL_CTC0_DIV2]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, audio_level_amplitude(AUDIO_LEVEL_CTC0_DIV2));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, audio_level_amplitude(AUDIO_LEVEL_VALUES_COUNT));
}

#if AUDIO_HAVE_OUTPUT_COUPLING
/** @brief Časová konstanta střídavé vazby výstupu v ticích GDG. */
#define OUT_TAU_TICKS ((uint64_t)GDGCLK_BASE * AUDIO_OUT_AC_TAU_MS / 1000u)

/* Vazba výstupu: střední výchylka stálého kanálu PSG doběhne za 0,57 s na 1 - 1/e, po 5 s je ustálená. */
void test_out_ac_psg_constant_level(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* kanál 0: dělič 001h a útlum 0 = stálá hodnota 30 (výchylka 0,49), bez zápisů do PSG */
    memset(&g_psg_module, 0, sizeof(g_psg_module));
    psg_init(false);
    g_psg.channel[PSG_CHANNEL_0].tone.divider = 1;
    g_psg.channel[PSG_CHANNEL_0].attn = PSG_OUT_MAX;
    audio_log_fill_psg(s_t0 + PSG_DIVIDER);
    TEST_ASSERT_EQUAL_INT(30, g_audio.log->src[AUDIO_SOURCE_ID_SHIFT_PSG0]->last_value);

    double mean[AUDIO_SRC_CHANNELS_COUNT];
    audio_out_ac_means(s_t0 + OUT_TAU_TICKS, mean);
    double amp = audio_level_amplitude(30);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, amp);
    /* výstup vazby = výchylka - střední hodnota: za 0,57 s na 1/e (+-10 %) */
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * amp * exp(-1.0), amp * exp(-1.0), amp - mean[1]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.0, mean[2]);
    audio_out_ac_means(s_t0 + 5u * (uint64_t)GDGCLK_BASE, mean);
    TEST_ASSERT_TRUE(amp - mean[1] < 0.001);                 /* po 5 s pod 0,1 % rozsahu */

    /* ustálený stav po resetu logu: stálá úroveň nedá žádný skok */
    audio_reset_log(s_t0 + 6u * (uint64_t)GDGCLK_BASE);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, amp, g_audio.log->out_ac_mean[1]);
    g_psg.channel[PSG_CHANNEL_0].attn = PSG_OUT_OFF;
}

/* Vazba výstupu: druhý stupeň pro CTC0 odpovídá číselnému řešení dvou propustí za sebou. */
void test_out_ac_ctc0_second_stage(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const double tau1 = AUDIO_CTC0_AC_TAU_MS / 1000.0, tau2 = AUDIO_OUT_AC_TAU_MS / 1000.0;
    const double dt = 1e-5;
    double m1 = 0.0, m2 = 0.0; /* číselně: úroveň 1 od času 0, pak 0 od 0,3 s */
    uint64_t t = s_t0 + 1000;
    double mean[AUDIO_SRC_CHANNELS_COUNT];

    audio_ctc0_changed(true, t);
    for (int i = 0; i < 30000; i++) { m2 += ((1.0 - m1) - m2) * dt / tau2; m1 += (1.0 - m1) * dt / tau1; }
    audio_out_ac_means(t + (uint64_t)(0.3 * GDGCLK_BASE), mean);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, m2, mean[0]);
    TEST_ASSERT_TRUE(mean[0] > 0.2);

    audio_ctc0_changed(false, t + (uint64_t)(0.3 * GDGCLK_BASE));
    for (int i = 0; i < 50000; i++) { m2 += ((0.0 - m1) - m2) * dt / tau2; m1 += (0.0 - m1) * dt / tau1; }
    audio_out_ac_means(t + (uint64_t)(0.8 * GDGCLK_BASE), mean);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, m2, mean[0]);

    /* tón 440 Hz: výstup vazby CTC0 je souměrný, jeho střední hodnota jde k nule */
    const uint64_t half = (uint64_t)GDGCLK_BASE / 880u;
    bool level = false;
    t += (uint64_t)GDGCLK_BASE;
    uint64_t end = t + 6u * (uint64_t)GDGCLK_BASE;
    while (t < end) {
        level = !level;
        audio_ctc0_changed(level, t);
        t += half;
    }
    audio_out_ac_means(t, mean);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, mean[0]);
    audio_ctc0_changed(false, t);
}
#endif /* AUDIO_HAVE_OUTPUT_COUPLING */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_ctc0_ac_settled_after_reset);
    RUN_TEST(test_ctc0_ac_constant_level_decays);
    RUN_TEST(test_ctc0_ac_square_symmetric);
    RUN_TEST(test_cmt_echo_plays_through_ac_coupling);
    RUN_TEST(test_ctc0_ac_mean_carried_by_log);
    RUN_TEST(test_audio_level_amplitude_matches_sdl_table);
#if AUDIO_HAVE_OUTPUT_COUPLING
    RUN_TEST(test_out_ac_psg_constant_level);
    RUN_TEST(test_out_ac_ctc0_second_stage);
#endif
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
