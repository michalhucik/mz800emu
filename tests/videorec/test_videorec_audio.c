/**
 * @file   test_videorec_audio.c
 * @brief  Standalone testy audio rendereru pro video záznam.
 *
 * Pokrývá box filtr a časování v surovém režimu (bez filtrů), koeficienty
 * řetězce SDL cesty (přesně při 44 100 Hz, stejná zlomová frekvence při
 * 48 000 Hz), parkování konstantní úrovně, IIR PSG, korekci zesílení CTC0
 * a uložení / obnovení stavu rendereru (plynulý retake), rozložení kanálů
 * do stereo L/R (mono a stereo rozložení SDL cesty) a celočíselný počet
 * vzorků na snímek při 50 i 60 snímcích/s. Paritu s funkcemi SDL cesty
 * ověřuje test_videorec_audio_sdl_parity.c.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/videorec/videorec_audio.h"

/* clk = 48000 * 400 => přesně 400 taktů na vzorek */
#define CLK 19200000ull
#define RATE 48000u
/* clk = 44100 * 400 => přesně 400 taktů na vzorek při 44 100 Hz */
#define CLK44 17640000ull
#define RATE44 44100u

static float LV[2][VIDEOREC_AUDIO_LEVELS];
static st_VIDEOREC_AUDIO A;

void setUp(void)
{
    memset(LV, 0, sizeof(LV));
    for (int v = 0; v < VIDEOREC_AUDIO_LEVELS; v++) { LV[0][v] = v ? 1.0f : 0.0f; LV[1][v] = v / 15.0f * 0.5f; }
    uint8_t vals[2] = { 0, 0 };
    videorec_audio_init(&A, CLK, RATE, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 1000, false);
}
void tearDown(void) { videorec_audio_free(&A); }

/* ================================================================
 * Surový režim (bez filtrů): box filtr a časování
 * ================================================================ */

static void test_silence(void)
{
    int16_t out[8];
    TEST_ASSERT_EQUAL_size_t(4, videorec_audio_render(&A, 1000 + 4 * 400, out, 4));
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_INT16(0, out[i]);
}

static void test_half_sample_box_filter(void)
{
    videorec_audio_event(&A, 0, 15, 1000 + 200);   /* uprostřed vzorku 0 */
    int16_t out[4];
    TEST_ASSERT_EQUAL_size_t(2, videorec_audio_render(&A, 1000 + 800, out, 2));
    TEST_ASSERT_INT16_WITHIN(1, 16384, out[0]);    /* 0.5 */
    TEST_ASSERT_EQUAL_INT16(out[0], out[1]);       /* L == R */
    TEST_ASSERT_EQUAL_INT16(32767, out[2]);        /* celý vzorek 1.0 */
}

static void test_horizon_limits_output(void)
{
    int16_t out[20];
    TEST_ASSERT_EQUAL_size_t(2, videorec_audio_render(&A, 1000 + 2 * 400 + 399, out, 10));
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 1000 + 3 * 400, out, 10));
}

static void test_channels_sum_and_clip(void)
{
    videorec_audio_event(&A, 0, 15, 1000);
    videorec_audio_event(&A, 1, 15, 1000);         /* 1.0 + 0.5 => ořez výstupu na 1.0 */
    int16_t out[2];
    videorec_audio_render(&A, 1000 + 400, out, 1);
    TEST_ASSERT_EQUAL_INT16(32767, out[0]);
}

static void test_event_before_origin_applies_at_origin(void)
{
    videorec_audio_event(&A, 1, 15, 10);           /* před origin */
    int16_t out[2];
    videorec_audio_render(&A, 1000 + 400, out, 1);
    TEST_ASSERT_INT16_WITHIN(1, 16384, out[0]);
}

static void test_rebase_drops_queue_and_sets_values(void)
{
    videorec_audio_event(&A, 0, 15, 5000);
    uint8_t vals[2] = { 0, 15 };
    videorec_audio_rebase(&A, 100, vals);
    int16_t out[2];
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 100 + 400, out, 1));
    TEST_ASSERT_INT16_WITHIN(1, 16384, out[0]);    /* jen kanál 1 z vals */
}

static void test_exact_samples_per_frame_mz800(void)
{
    /* MZ-800: clk = 354432 * 50, snímek = 354432 taktů => 960 vzorků při 48 kHz, 882 při 44,1 kHz */
    videorec_audio_free(&A);
    uint8_t vals[2] = { 0, 0 };
    videorec_audio_init(&A, 354432ull * 50, 48000, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, false);
    static int16_t out[2000 * 2];
    TEST_ASSERT_EQUAL_size_t(960, videorec_audio_render(&A, 354432, out, 2000));
    TEST_ASSERT_EQUAL_size_t(960, videorec_audio_render(&A, 2 * 354432, out, 2000));
    videorec_audio_free(&A);
    videorec_audio_init(&A, 354432ull * 50, 44100, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, true);
    TEST_ASSERT_EQUAL_size_t(882, videorec_audio_render(&A, 354432, out, 2000));
}

static void test_exact_samples_per_frame_60fps(void)
{
    /* MZ-1500 / MZ-700 NTSC: clk = 238944 * 60, snímek = 238944 taktů => 800 vzorků při 48 kHz, 735 při 44,1 kHz */
    videorec_audio_free(&A);
    uint8_t vals[2] = { 0, 0 };
    videorec_audio_init(&A, 238944ull * 60, 48000, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, false);
    static int16_t out[2000 * 2];
    for (int f = 1; f <= 120; f++) TEST_ASSERT_EQUAL_size_t(800, videorec_audio_render(&A, (uint64_t)f * 238944, out, 2000));
    videorec_audio_free(&A);
    videorec_audio_init(&A, 238944ull * 60, 44100, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, true);
    for (int f = 1; f <= 120; f++) TEST_ASSERT_EQUAL_size_t(735, videorec_audio_render(&A, (uint64_t)f * 238944, out, 2000));
}

/* ================================================================
 * Stereo (rozložení kanálů jako SDL cesta, iface_audio.c)
 * ================================================================ */

/** Inicializuje A s 9 kanály (CTC0 + 2x4 PSG) v surovém režimu; kanál c drží hodnotu 15 s úrovní lv[c]. */
static void init9(const float lv[9])
{
    videorec_audio_free(&A);
    static float L9[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    memset(L9, 0, sizeof(L9));
    for (int c = 0; c < 9; c++) L9[c][15] = lv[c];
    videorec_audio_init(&A, CLK, RATE, 9, (const float (*)[VIDEOREC_AUDIO_LEVELS])L9, NULL, 0, false);
    for (unsigned c = 0; c < 9; c++) videorec_audio_event(&A, c, 15, 0);
}

/* Bez videorec_audio_set_stereo() jdou všechny kanály do obou stran (dřívější chování). */
static void test_default_all_channels_both_sides(void)
{
    const float lv[9] = { 0.1f, 0.05f, 0, 0, 0, 0.2f, 0, 0, 0 };
    init9(lv);
    int16_t out[2];
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 400, out, 1));
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.35 * 32767), out[0]);
    TEST_ASSERT_EQUAL_INT16(out[0], out[1]);
    TEST_ASSERT_FALSE(videorec_audio_is_stereo(&A));
}

/* Stereo: L = CTC0 + PSG0 (kanály 1-4), R = CTC0 + PSG1 (kanály 5-8), iface_audio_mix_channels_stereo(). */
static void test_stereo_routing(void)
{
    const float lv[9] = { 0.1f, 0.05f, 0.01f, 0, 0, 0.2f, 0, 0, 0.02f };
    init9(lv);
    videorec_audio_set_stereo(&A, true);
    TEST_ASSERT_TRUE(videorec_audio_is_stereo(&A));
    int16_t out[2];
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 400, out, 1));
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.16 * 32767), out[0]);
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.32 * 32767), out[1]);
}

/* Mono rozložení SDL cesty: CTC0 + PSG0 do obou stran, PSG1 se nemixuje (iface_audio_mix_channels_with_gain()). */
static void test_mono_layout_mutes_psg1(void)
{
    const float lv[9] = { 0.1f, 0.05f, 0, 0, 0, 0.2f, 0, 0, 0 };
    init9(lv);
    videorec_audio_set_stereo(&A, false);
    int16_t out[2];
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 400, out, 1));
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.15 * 32767), out[0]);
    TEST_ASSERT_EQUAL_INT16(out[0], out[1]);
}

/* Ořez se dělá pro každou stranu zvlášť (přetečení L neovlivní R). */
static void test_stereo_clip_per_side(void)
{
    const float lv[9] = { 0.5f, 0.6f, 0, 0, 0, 0.1f, 0, 0, 0 };
    init9(lv);
    videorec_audio_set_stereo(&A, true);
    int16_t out[2];
    videorec_audio_render(&A, 400, out, 1);
    TEST_ASSERT_EQUAL_INT16(32767, out[0]);
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.6 * 32767), out[1]);
}

/* Přepnutí rozložení platí od dalšího vyrenderovaného vzorku. */
static void test_stereo_switch_between_renders(void)
{
    const float lv[9] = { 0.1f, 0.05f, 0, 0, 0, 0.2f, 0, 0, 0 };
    init9(lv);
    videorec_audio_set_stereo(&A, true);
    int16_t out[4];
    videorec_audio_render(&A, 400, out, 1);
    videorec_audio_set_stereo(&A, false);
    videorec_audio_render(&A, 800, out + 2, 1);
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.30 * 32767), out[1]);
    TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrint(0.15 * 32767), out[3]);
    TEST_ASSERT_FALSE(videorec_audio_is_stereo(&A));
}

/* Jediný kanál (MZ-700, jen CTC0): stereo rozložení nic nemění, L == R. */
static void test_single_channel_mono(void)
{
    videorec_audio_free(&A);
    videorec_audio_init(&A, CLK, RATE, 1, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, NULL, 0, false);
    videorec_audio_set_stereo(&A, false);
    videorec_audio_event(&A, 0, 15, 0);
    int16_t out[2];
    videorec_audio_render(&A, 400, out, 1);
    TEST_ASSERT_EQUAL_INT16(32767, out[0]);
    TEST_ASSERT_EQUAL_INT16(32767, out[1]);
}

/* ================================================================
 * Koeficienty řetězce SDL cesty
 * ================================================================ */

/* Při 44 100 Hz musí být koeficienty bit po bitu ty z iface_audio_resampler.c / iface_audio.c. */
static void test_coef_44100_exact_sdl(void)
{
    st_VIDEOREC_AUDIO_COEF c;
    videorec_audio_coef_init(&c, 44100);
    TEST_ASSERT_EQUAL_UINT(1980, c.park_samples); /* (44100 / 1000) * 45, iface_audio.c:224 */
    TEST_ASSERT_EQUAL_UINT(880, c.park_fade);     /* (44100 / 1000) * 20, iface_audio.c:226 */
    TEST_ASSERT_TRUE(c.ctc_lp_alpha == 0.4f);     /* iface_audio_resampler.c:82 */
    TEST_ASSERT_TRUE(c.ctc_ag_factor == 0.2f);    /* iface_audio_resampler.c:160 */
    TEST_ASSERT_TRUE(c.psg_iir_div == 6.0f);      /* iface_audio_resampler.c:384 */
}

/* Při 48 000 Hz stejná zlomová frekvence: (1 - a48)^48000 == (1 - a44)^44100 (útlum za 1 s). */
static void test_coef_48000_equal_cutoff(void)
{
    st_VIDEOREC_AUDIO_COEF c;
    videorec_audio_coef_init(&c, 48000);
    TEST_ASSERT_EQUAL_UINT(2160, c.park_samples); /* 45 ms */
    TEST_ASSERT_EQUAL_UINT(960, c.park_fade);     /* 20 ms */
    double k = 44100.0 / 48000.0;
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0 - pow(0.6, k), c.ctc_lp_alpha);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0 - pow(0.8, k), c.ctc_ag_factor);
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, 1.0 / (1.0 - pow(5.0 / 6.0, k)), c.psg_iir_div);
    /* Útlum za 1 ms (pól jednopólového filtru) je při obou frekvencích stejný. */
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, pow(0.6, 44.1), pow(1.0 - c.ctc_lp_alpha, 48.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, pow(5.0 / 6.0, 44.1), pow(1.0 - 1.0 / c.psg_iir_div, 48.0));
}

/* Korekce zesílení CTC0: střední hodnota podílu nenulových / (délka bloku + 1) jako SDL cesta. */
static void test_sdl_ctc0_gain(void)
{
    /* poměr přesně 2: bloky délky 2 => 2/3, poslední blok oříznutý na délku 1 => 1/2 */
    double expect = (881.0 * (2.0 / 3.0) + 0.5) / 882.0;
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, expect, videorec_audio_sdl_ctc0_gain(1764, 882));
    /* MZ-800: 17 721 600 / 32 / 50 = 11076 vzorků CTC0 na snímek, 882 výstupních (poměr cca 12,56) */
    float g = videorec_audio_sdl_ctc0_gain(11076, 882);
    TEST_ASSERT_TRUE(g > 12.0f / 13.0f - 0.01f && g < 13.0f / 14.0f + 0.01f);
}

/* ================================================================
 * Řetězec SDL cesty v rendereru
 * ================================================================ */

/** Nový renderer při 44 100 Hz s řetězcem SDL cesty. */
static void init44(uint8_t v0, uint8_t v1)
{
    videorec_audio_free(&A);
    uint8_t vals[2] = { v0, v1 };
    videorec_audio_init(&A, CLK44, RATE44, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, true);
}

/* CTC0 trvale 15: low-pass + anti-glitch dosáhnou 1,0, po 45 ms parkování útlum k 0 během cca 20 ms. */
static void test_parking_ctc0_constant(void)
{
    init44(15, 0);
    static int16_t out[8820 * 2];
    TEST_ASSERT_EQUAL_size_t(8820, videorec_audio_render(&A, 8820ull * 400, out, 8820));
    TEST_ASSERT_TRUE(out[0] > 0 && out[0] < 3000);       /* 0,4 * 0,2 = 0,08 */
    TEST_ASSERT_INT16_WITHIN(50, 32767, out[1900 * 2]);   /* ustáleno, ještě nezaparkováno */
    TEST_ASSERT_TRUE(out[2400 * 2] > 3000 && out[2400 * 2] < 30000); /* uprostřed útlumu */
    TEST_ASSERT_INT16_WITHIN(1, 0, out[3100 * 2]);        /* po útlumu ticho */
    TEST_ASSERT_INT16_WITHIN(1, 0, out[8819 * 2]);
}

/* Měnící se hodnota parkování resetuje: obdélník CTC0 (perioda 200 vzorků) zůstane slyšet. */
static void test_parking_not_triggered_by_square(void)
{
    init44(0, 0);
    for (uint64_t k = 1; k < 100; k++) videorec_audio_event(&A, 0, (k & 1) ? 15 : 0, k * 100 * 400);
    static int16_t out[9900 * 2];
    TEST_ASSERT_EQUAL_size_t(9900, videorec_audio_render(&A, 9900ull * 400, out, 9900));
    int16_t mx = 0;
    for (int i = 9000; i < 9900; i++) if (out[i * 2] > mx) mx = out[i * 2];
    TEST_ASSERT_INT16_WITHIN(100, 32767, mx);
}

/* PSG: IIR sample += (x - sample) / 6 => y[k] = x * (1 - (5/6)^(k+1)). */
static void test_psg_iir_step(void)
{
    init44(0, 0);
    videorec_audio_event(&A, 1, 15, 0); /* úroveň 0,5 od začátku */
    int16_t out[5 * 2];
    TEST_ASSERT_EQUAL_size_t(5, videorec_audio_render(&A, 5 * 400, out, 5));
    for (int k = 0; k < 5; k++) {
        double y = 0.5 * (1.0 - pow(5.0 / 6.0, k + 1));
        TEST_ASSERT_INT16_WITHIN(2, (int16_t)lrint(y * 32767.0), out[k * 2]);
    }
}

/* Rebase (šev) nemění stav filtrů: výstup pokračuje plynule z dosavadního stavu. */
static void test_rebase_keeps_filter_state(void)
{
    init44(15, 0);
    static int16_t out[200 * 2];
    videorec_audio_render(&A, 200 * 400, out, 200);
    int16_t last = out[199 * 2];
    uint8_t vals[2] = { 0, 0 };
    videorec_audio_rebase(&A, 1000000, vals);
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&A, 1000000 + 400, out, 1));
    TEST_ASSERT_TRUE(last > 32000);
    TEST_ASSERT_TRUE(out[0] > 20000); /* anti-glitch 0,2 => cca 0,92 * 1,0, žádný skok k 0 */
}

/** Deterministický zdroj událostí: CTC0 obdélník perioda 3 ms, PSG perioda 2,27 ms (takty jako u CLK44). */
static void feed(st_VIDEOREC_AUDIO *a, uint64_t from, uint64_t to)
{
    const uint64_t pc = 52920 / 2, pp = 40000 / 2;
    for (uint64_t t = (from + pc - 1) / pc * pc; t < to; t += pc) videorec_audio_event(a, 0, ((t / pc) & 1) ? 15 : 0, t);
    for (uint64_t t = (from + pp - 1) / pp * pp; t < to; t += pp) videorec_audio_event(a, 1, ((t / pp) & 1) ? 12 : 0, t);
}

/** Hodnota kanálů zdroje feed() v čase t (před událostí v t). */
static void feed_values(uint64_t t, uint8_t v[2])
{
    const uint64_t pc = 52920 / 2, pp = 40000 / 2;
    v[0] = (t == 0) ? 0 : ((((t - 1) / pc) & 1) ? 15 : 0);
    v[1] = (t == 0) ? 0 : ((((t - 1) / pp) & 1) ? 12 : 0);
}

/*
 * Plynulý retake na úrovni rendereru: stav uložený v bodě T a obnovený po
 * rebase na T dá přesně stejný výstup jako nepřerušený běh. Bez obnovy
 * (dnešní záložní chování) pokračují filtry ze zahozené budoucnosti.
 */
static void test_state_restore_continuity(void)
{
    const uint64_t T = 3000ull * 400, END = 4000ull * 400;
    static int16_t ref[4000 * 2], out[4000 * 2];

    /* Nepřerušená reference. */
    init44(0, 0);
    feed(&A, 0, END);
    TEST_ASSERT_EQUAL_size_t(4000, videorec_audio_render(&A, END, ref, 4000));

    /* Běh do T, uložit stav, "zahozená budoucnost" do T + 1531 vzorků, retake na T. */
    init44(0, 0);
    feed(&A, 0, T);
    TEST_ASSERT_EQUAL_size_t(3000, videorec_audio_render(&A, T, out, 3000));
    st_VIDEOREC_AUDIO_STATE st;
    videorec_audio_get_state(&A, &st);
    uint8_t v[2];
    feed_values(T, v);
    TEST_ASSERT_EQUAL_UINT8(v[0], st.value[0]);
    TEST_ASSERT_EQUAL_UINT8(v[1], st.value[1]);
    feed(&A, T, T + 1531ull * 400 + 7);
    videorec_audio_event(&A, 0, 15, T + 1531ull * 400 + 7); /* nevyrenderovaná událost budoucnosti */
    videorec_audio_render(&A, T + 1531ull * 400, out, 1531); /* budoucnost končí v jiné fázi CTC0 než bod T */

    /* bez obnovy stavu: skok proti referenci */
    st_VIDEOREC_AUDIO B;
    memcpy(&B, &A, sizeof(B));
    memset(B.q, 0, sizeof(B.q));
    uint8_t zero[2] = { 0, 0 };
    videorec_audio_rebase(&B, T, zero);
    feed(&B, T, END);
    int16_t first_fallback[2];
    TEST_ASSERT_EQUAL_size_t(1, videorec_audio_render(&B, T + 400, first_fallback, 1));
    videorec_audio_free(&B);

    /* s obnovou stavu: přesně reference */
    videorec_audio_rebase(&A, T, zero);
    videorec_audio_set_state(&A, &st);
    feed(&A, T, END);
    TEST_ASSERT_EQUAL_size_t(1000, videorec_audio_render(&A, END, out, 1000));
    for (int i = 0; i < 1000; i++) TEST_ASSERT_EQUAL_INT16(ref[(3000 + i) * 2], out[i * 2]);
    TEST_ASSERT_TRUE_MESSAGE(abs(first_fallback[0] - ref[3000 * 2]) > 1000, "fallback should differ (negative control)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_silence);
    RUN_TEST(test_half_sample_box_filter);
    RUN_TEST(test_horizon_limits_output);
    RUN_TEST(test_channels_sum_and_clip);
    RUN_TEST(test_event_before_origin_applies_at_origin);
    RUN_TEST(test_rebase_drops_queue_and_sets_values);
    RUN_TEST(test_exact_samples_per_frame_mz800);
    RUN_TEST(test_exact_samples_per_frame_60fps);
    RUN_TEST(test_default_all_channels_both_sides);
    RUN_TEST(test_stereo_routing);
    RUN_TEST(test_mono_layout_mutes_psg1);
    RUN_TEST(test_stereo_clip_per_side);
    RUN_TEST(test_stereo_switch_between_renders);
    RUN_TEST(test_single_channel_mono);
    RUN_TEST(test_coef_44100_exact_sdl);
    RUN_TEST(test_coef_48000_equal_cutoff);
    RUN_TEST(test_sdl_ctc0_gain);
    RUN_TEST(test_parking_ctc0_constant);
    RUN_TEST(test_parking_not_triggered_by_square);
    RUN_TEST(test_psg_iir_step);
    RUN_TEST(test_rebase_keeps_filter_state);
    RUN_TEST(test_state_restore_continuity);
    return UNITY_END();
}
