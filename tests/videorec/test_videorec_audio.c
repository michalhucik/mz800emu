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

/* M_PI není součást ISO C; glibc ho při _POSIX_C_SOURCE bez _XOPEN_SOURCE
 * nenabízí (stejné řešení jako src/iface/iface_audio.c). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    /* střídavá vazba CTC0 0,85 s, výstupní propust 460 us, přechod 10 ms */
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0 - exp(-1.0 / (0.85 * 44100.0)), c.ctc_ac_k);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0 - exp(-1.0 / (460e-6 * 44100.0)), c.out_lp_k);
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, 1.0f / 441.0f, c.out_xfade_step);
    /* střídavá vazba výstupu je po inicializaci vypnutá a kanály PSG se parkují */
    TEST_ASSERT_TRUE(c.out_ac_k == 0.0);
    TEST_ASSERT_TRUE(c.psg_park);
    videorec_audio_coef_set_output_coupling(&c, 44100, true);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0 - exp(-1.0 / (0.57 * 44100.0)), c.out_ac_k);
    TEST_ASSERT_FALSE(c.psg_park);
    videorec_audio_coef_set_output_coupling(&c, 44100, false);
    TEST_ASSERT_TRUE(c.out_ac_k == 0.0 && c.psg_park);
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
    /* časové konstanty střídavé vazby a výstupní propusti nezávisí na frekvenci */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, exp(-1.0 / 0.85), pow(1.0 - c.ctc_ac_k, 48000.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, exp(-1.0 / 0.46), pow(1.0 - c.out_lp_k, 48.0));
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

/**
 * Nový renderer při 44 100 Hz s řetězcem SDL cesty.
 *
 * Testy tvaru filtrů (střídavé vazby, propust, IIR) pracují s úrovněmi až do
 * plného rozsahu, proto má výstupní stupeň vypnuté dorovnání hlasitosti
 * a omezovač (násobitele 1, koleno 1 = prostý ořez na [-1, 1]). Dorovnání
 * a omezovač ověřují test_soft_limit_properties(), test_output_stage_makeup_gain()
 * a test_render_makeup_and_limiter() s výchozím nastavením.
 */
static void init44(uint8_t v0, uint8_t v1)
{
    videorec_audio_free(&A);
    uint8_t vals[2] = { v0, v1 };
    videorec_audio_init(&A, CLK44, RATE44, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, 0, true);
    videorec_audio_coef_set_output_level(&A.coef, 1.0f, 1.0f, 1.0f);
}

/*
 * Renderer s výchozím nastavením (jak ho používá emulátor): stálá úroveň kanálu PSG 0,2 dá po ustálení
 * IIR 0,2 krát +4 dB, se zapnutou propustí 0,2 krát +8 dB; úroveň 1,0 (nad rozsahem po dorovnání) se
 * měkce stlačí pod plný rozsah a neořízne; koeficient s kolenem 1 dává dřívější tvrdý ořez.
 * Čte se vzorek 1500 (34 ms): IIR i propust jsou ustálené a parkování stálé úrovně (po 45 ms) ještě nezačalo.
 */
static void test_render_makeup_and_limiter(void)
{
    static float lv[2][VIDEOREC_AUDIO_LEVELS];
    memset(lv, 0, sizeof(lv));
    lv[1][15] = 0.2f;
    lv[1][14] = 1.0f;
    static int16_t out[2000 * 2];
    uint8_t vals[2] = { 0, 15 };
    for (int pass = 0; pass < 2; pass++) {
        videorec_audio_free(&A);
        videorec_audio_init(&A, CLK44, RATE44, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])lv, vals, 0, true);
        videorec_audio_set_output_filter(&A, pass == 1, true);
        TEST_ASSERT_EQUAL_size_t(2000, videorec_audio_render(&A, 2000ull * 400, out, 2000));
        const double want = 0.2 * pow(10.0, (pass ? VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB : VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB) / 20.0) * 32767.0;
        TEST_ASSERT_INT16_WITHIN(3, (int16_t)lrint(want), out[1500 * 2]);
        TEST_ASSERT_EQUAL_INT16(out[1500 * 2], out[1500 * 2 + 1]);
    }

    vals[1] = 14;
    videorec_audio_free(&A);
    videorec_audio_init(&A, CLK44, RATE44, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])lv, vals, 0, true);
    TEST_ASSERT_EQUAL_size_t(2000, videorec_audio_render(&A, 2000ull * 400, out, 2000));
    const double lim = 0.7 + 0.3 * tanh((pow(10.0, 4.0 / 20.0) - 0.7) / 0.3);
    TEST_ASSERT_INT16_WITHIN(3, (int16_t)lrint(lim * 32767.0), out[1500 * 2]);
    TEST_ASSERT_TRUE(out[1500 * 2] < 32767);
    for (int i = 1; i <= 1500; i++) TEST_ASSERT_TRUE(out[i * 2] >= out[(i - 1) * 2]); /* náběh IIR zůstává monotónní */

    videorec_audio_free(&A);
    videorec_audio_init(&A, CLK44, RATE44, 2, (const float (*)[VIDEOREC_AUDIO_LEVELS])lv, vals, 0, true);
    videorec_audio_coef_set_output_level(&A.coef, 2.0f, 2.0f, 1.0f);
    TEST_ASSERT_EQUAL_size_t(2000, videorec_audio_render(&A, 2000ull * 400, out, 2000));
    TEST_ASSERT_EQUAL_INT16(32767, out[1500 * 2]);
}

/*
 * Střídavá vazba CTC0: skok na stálou úroveň projde celý a doznívá s časovou
 * konstantou 0,85 s (za 0,85 s na 1/e, tolerance 10 %); úroveň, která trvá
 * od začátku (ustálená vazba), nedává nic.
 */
static void test_ctc0_ac_constant_decays(void)
{
    enum { N = 52920 }; /* 1,2 s */
    static int16_t out[N * 2];
    init44(0, 0);
    videorec_audio_event(&A, 0, 15, 0);
    TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, out, N));
    TEST_ASSERT_TRUE(out[0] > 0 && out[0] < 3000);                 /* low-pass a anti-glitch: 0,4 * 0,2 = 0,08 */
    TEST_ASSERT_INT16_WITHIN(150, 32767, out[60 * 2]);             /* skok prošel celý */
    double e = 32767.0 * exp(-1.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * e, e, (double)out[37485 * 2]); /* 0,85 s = 37 485 vzorků */
    TEST_ASSERT_TRUE(out[(N - 1) * 2] < out[37485 * 2]);           /* doznívá dál */
    TEST_ASSERT_EQUAL_INT16(out[37485 * 2], out[37485 * 2 + 1]);   /* L == R */

    init44(15, 0);
    TEST_ASSERT_EQUAL_size_t(2000, videorec_audio_render(&A, 2000ull * 400, out, 2000));
    for (int i = 0; i < 2000; i += 100) TEST_ASSERT_INT16_WITHIN(1, 0, out[i * 2]);
}

/*
 * Střídavá vazba CTC0: obdélník (perioda 200 vzorků) je souměrný kolem nuly.
 * S ustálenou střední hodnotou 0,5 hned, od nuly po několika časových konstantách.
 */
static void test_ctc0_ac_square_symmetric(void)
{
    enum { N = 220500 }; /* 5 s */
    static int16_t out[N * 2];
    init44(0, 0);
    videorec_audio_set_ctc0_mean(&A, 0.5);
    for (uint64_t k = 1; k < 100; k++) videorec_audio_event(&A, 0, (k & 1) ? 15 : 0, k * 100 * 400);
    TEST_ASSERT_EQUAL_size_t(9900, videorec_audio_render(&A, 9900ull * 400, out, 9900));
    int mx = -32768, mn = 32767;
    for (int i = 9000; i < 9900; i++) {
        if (out[i * 2] > mx) mx = out[i * 2];
        if (out[i * 2] < mn) mn = out[i * 2];
    }
    TEST_ASSERT_INT_WITHIN(200, 16384, mx);
    TEST_ASSERT_INT_WITHIN(200, -16384, mn);

    init44(0, 0);
    for (uint64_t k = 1; k < N / 100; k++) videorec_audio_event(&A, 0, (k & 1) ? 15 : 0, k * 100 * 400);
    TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, out, N));
    /* na začátku jednostranný (0 až 1), po 5 s souměrný */
    mx = -32768; mn = 32767;
    for (int i = 200; i < 1000; i++) {
        if (out[i * 2] > mx) mx = out[i * 2];
        if (out[i * 2] < mn) mn = out[i * 2];
    }
    TEST_ASSERT_TRUE(mx > 31000 && mn > -1500);
    mx = -32768; mn = 32767;
    double mean = 0;
    for (int i = N - 2000; i < N; i++) {
        if (out[i * 2] > mx) mx = out[i * 2];
        if (out[i * 2] < mn) mn = out[i * 2];
        mean += out[i * 2];
    }
    TEST_ASSERT_INT_WITHIN(300, 16384, mx);
    TEST_ASSERT_INT_WITHIN(300, -16384, mn);
    TEST_ASSERT_DOUBLE_WITHIN(150.0, 0.0, mean / 2000.0);
}

/* ================================================================
 * Výstupní propust (volba [AUDIO] output_filter)
 * ================================================================ */

/*
 * Výstupní stupeň: váha 0 = součet po filtrech kanálů (dřívější filtry) krát dorovnání pro vypnutou propust,
 * pod kolenem omezovače přesně lineární; součet nad rozsahem se měkce stlačí pod 1,0 (dřív tvrdý ořez na 1,0).
 * Propust přitom běží.
 */
static void test_output_stage_gain_zero_is_legacy(void)
{
    st_VIDEOREC_AUDIO_COEF c;
    videorec_audio_coef_init(&c, 44100);
    double lp = 0.0, ac = 0.0;
    TEST_ASSERT_TRUE(videorec_audio_output_stage(&c, &ac, &lp, 0.3f, 0.9f, 0.0f) == 0.3f * c.out_makeup_off);
    float top = videorec_audio_output_stage(&c, &ac, &lp, 1.7f, 0.9f, 0.0f);
    TEST_ASSERT_TRUE(top > VIDEOREC_AUDIO_OUT_KNEE && top <= 1.0f);
    TEST_ASSERT_TRUE(videorec_audio_output_stage(&c, &ac, &lp, -0.4f, 0.9f, 0.0f) == -0.4f * c.out_makeup_off);
    TEST_ASSERT_TRUE(lp > 0.0 && lp < 0.9); /* propust běží i při váze 0 */
    TEST_ASSERT_TRUE(ac == 0.0);            /* bez střídavé vazby výstupu se její stav nemění */
}

/*
 * Výstupní propust: stejnosměrné zesílení propusti 1 (výstup = vstup krát dorovnání pro zapnutou propust),
 * -3 dB na 346 Hz, -20,3 dB na 3,46 kHz. Amplitudy jsou volené pod kolenem omezovače.
 */
static void test_output_stage_lowpass_response(void)
{
    st_VIDEOREC_AUDIO_COEF c;
    videorec_audio_coef_init(&c, 44100);
    double lp = 0.0, ac = 0.0;
    float y = 0;
    for (int i = 0; i < 44100; i++) y = videorec_audio_output_stage(&c, &ac, &lp, 0.0f, 0.2f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.2f * c.out_makeup_on, y);

    const double freq[3] = { 34.6, 346.0, 3460.0 };
    const double want_db[3] = { -0.04, -3.01, -20.04 };
    for (int f = 0; f < 3; f++) {
        lp = 0.0;
        double peak = 0;
        for (int i = 0; i < 44100; i++) {
            float x = (float)(0.2 * sin(2.0 * M_PI * freq[f] * i / 44100.0));
            y = videorec_audio_output_stage(&c, &ac, &lp, 0.0f, x, 1.0f);
            if (i > 22050 && fabs(y) > peak) peak = fabs(y);
        }
        TEST_ASSERT_DOUBLE_WITHIN(0.5, want_db[f], 20.0 * log10(peak / (0.2 * (double)c.out_makeup_on)));
    }
}

/*
 * Měkký omezovač: do kolena přesně lineární (bitově), lichý, neklesající, nikdy nad 1,0 ani pro
 * libovolně velký vstup, nad kolenem stlačuje a v koleni navazuje směrnicí 1 (bez zlomu).
 */
static void test_soft_limit_properties(void)
{
    const float knee = VIDEOREC_AUDIO_OUT_KNEE;
    TEST_ASSERT_TRUE(knee > 0.0f && knee < 1.0f);

    /* lineární část: přesně vstup */
    for (int i = -1000; i <= 1000; i++) {
        float x = knee * (float)i / 1000.0f;
        TEST_ASSERT_TRUE(videorec_audio_soft_limit(x) == x);
    }
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(knee) == knee);
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(-knee) == -knee);
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(0.0f) == 0.0f);

    /* neklesající, lichý, v [-1, 1], nad kolenem pod vstupem */
    float prev = 0.0f;
    for (int i = 1; i <= 40000; i++) {
        float x = (float)i / 10000.0f;
        float y = videorec_audio_soft_limit(x);
        TEST_ASSERT_TRUE(y >= prev);
        TEST_ASSERT_TRUE(y <= 1.0f);
        TEST_ASSERT_TRUE(y <= x);
        TEST_ASSERT_TRUE(videorec_audio_soft_limit(-x) == -y);
        if (x > knee + 0.01f) TEST_ASSERT_TRUE(y < x);
        prev = y;
    }
    /* do dvojnásobku plného rozsahu ostře roste (žádná rovná střecha jako u tvrdého ořezu) */
    prev = videorec_audio_soft_limit(knee);
    for (int i = 1; i <= 130; i++) {
        float y = videorec_audio_soft_limit(knee + (float)i / 100.0f);
        TEST_ASSERT_TRUE(y > prev);
        prev = y;
    }
    /* libovolně velký vstup zůstává v rozsahu */
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(100.0f) <= 1.0f && videorec_audio_soft_limit(100.0f) > 0.999f);
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(-1.0e30f) >= -1.0f && videorec_audio_soft_limit(-1.0e30f) < -0.999f);
    TEST_ASSERT_TRUE(videorec_audio_soft_limit(INFINITY) == 1.0f);

    /* známé hodnoty: knee + (1 - knee) * tanh((x - knee) / (1 - knee)) */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)(0.7 + 0.3 * tanh(1.0)), videorec_audio_soft_limit(1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)(0.7 + 0.3 * tanh(0.5 / 0.3)), videorec_audio_soft_limit(1.2f));

    /* spojitá derivace v koleni: směrnice těsně pod i nad kolenem je 1 a nad kolenem plynule klesá */
    const float h = 1.0e-3f;
    float below = (videorec_audio_soft_limit(knee) - videorec_audio_soft_limit(knee - h)) / h;
    float above = (videorec_audio_soft_limit(knee + h) - videorec_audio_soft_limit(knee)) / h;
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, below);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, above);
    float slope_prev = above;
    for (int i = 1; i <= 20; i++) {
        float x = knee + 0.05f * (float)i;
        float slope = (videorec_audio_soft_limit(x + h) - videorec_audio_soft_limit(x)) / h;
        TEST_ASSERT_TRUE(slope < slope_prev + 2e-3f);
        TEST_ASSERT_TRUE(slope >= 0.0f);
        slope_prev = slope;
    }

    /* obecné koleno */
    TEST_ASSERT_TRUE(videorec_audio_soft_limit_knee(0.5f, 0.5f) == 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)(0.5 + 0.5 * tanh(1.0)), videorec_audio_soft_limit_knee(1.0f, 0.5f));
}

/*
 * Dorovnání hlasitosti: +4 dB s vypnutou propustí (a na strojích bez ní), +8 dB se zapnutou; při
 * přechodu se násobitel mění lineárně s vahou propusti; pod kolenem je výstup přesně úměrný vstupu
 * (poměry mezi zdroji se nemění).
 */
static void test_output_stage_makeup_gain(void)
{
    st_VIDEOREC_AUDIO_COEF c;
    videorec_audio_coef_init(&c, 44100);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5849f, c.out_makeup_off);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5119f, c.out_makeup_on);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB, 20.0 * log10((double)c.out_makeup_off));
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB, 20.0 * log10((double)c.out_makeup_on));
    /* dorovnání nezávisí na vzorkovací frekvenci */
    st_VIDEOREC_AUDIO_COEF c48;
    videorec_audio_coef_init(&c48, 48000);
    TEST_ASSERT_TRUE(c48.out_makeup_off == c.out_makeup_off && c48.out_makeup_on == c.out_makeup_on);

    /* vypnutá propust: linearita pod kolenem, poměr dvou úrovní přesně zachovaný */
    double lp = 0.0, ac = 0.0;
    float a1 = videorec_audio_output_stage(&c, &ac, &lp, 0.25f, 0.0f, 0.0f);
    float a2 = videorec_audio_output_stage(&c, &ac, &lp, 0.25f * 1.49f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(a1 == 0.25f * c.out_makeup_off);
    TEST_ASSERT_TRUE(a2 < VIDEOREC_AUDIO_OUT_KNEE);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.49f, a2 / a1);

    /* zapnutá propust, ustálená stálá úroveň */
    lp = 0.1;
    float y = videorec_audio_output_stage(&c, &ac, &lp, 0.0f, 0.1f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f * c.out_makeup_on, y);

    /* uprostřed přechodu: průměr obou cest krát průměr obou násobitelů */
    lp = 0.1;
    y = videorec_audio_output_stage(&c, &ac, &lp, 0.2f, 0.1f, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f * 0.5f * (c.out_makeup_off + c.out_makeup_on), y);

    /* nejhorší případ (CTC0 a všechny kanály PSG naplno, součet 1,37): stlačí se, neořízne */
    lp = 0.0;
    float worst = videorec_audio_output_stage(&c, &ac, &lp, 1.37f, 0.0f, 0.0f);
    float more = videorec_audio_output_stage(&c, &ac, &lp, 1.5f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(worst > VIDEOREC_AUDIO_OUT_KNEE && worst <= 1.0f);
    TEST_ASSERT_TRUE(more >= worst && more <= 1.0f);
    TEST_ASSERT_TRUE(videorec_audio_output_stage(&c, &ac, &lp, -1.37f, 0.0f, 0.0f) == -worst);
}

/** Tón 440 Hz na kanálu PSG (úroveň 0,5) v intervalu [from, to). */
static void feed_psg_tone(st_VIDEOREC_AUDIO *a, uint64_t from, uint64_t to)
{
    const uint64_t half = 20045; /* 17,64 MHz / 880 */
    for (uint64_t t = (from + half - 1) / half * half; t < to; t += half) videorec_audio_event(a, 1, ((t / half) & 1) ? 15 : 0, t);
}

/*
 * Přepnutí výstupní propusti za běhu: plynulý přechod 10 ms bez skoku a po něm
 * přesně stejný výstup jako renderer, který měl propust zapnutou od začátku
 * (propust běží stále, i když je vypnutá).
 */
static void test_output_filter_switch_without_click(void)
{
    enum { N1 = 4410, N2 = 4410 };
    static int16_t on[(N1 + N2) * 2], sw[(N1 + N2) * 2], off[(N1 + N2) * 2];
    const uint64_t END = (uint64_t)(N1 + N2) * 400;

    init44(0, 0);
    videorec_audio_set_output_filter(&A, true, true);
    feed_psg_tone(&A, 0, END);
    TEST_ASSERT_EQUAL_size_t(N1 + N2, videorec_audio_render(&A, END, on, N1 + N2));

    init44(0, 0);
    feed_psg_tone(&A, 0, END);
    TEST_ASSERT_EQUAL_size_t(N1 + N2, videorec_audio_render(&A, END, off, N1 + N2));

    init44(0, 0);
    feed_psg_tone(&A, 0, END);
    TEST_ASSERT_EQUAL_size_t(N1, videorec_audio_render(&A, (uint64_t)N1 * 400, sw, N1));
    videorec_audio_set_output_filter(&A, true, false);
    TEST_ASSERT_EQUAL_size_t(N2, videorec_audio_render(&A, END, sw + N1 * 2, N2));

    /* před přepnutím přesně výstup bez propusti */
    for (int i = 0; i < N1; i++) TEST_ASSERT_EQUAL_INT16(off[i * 2], sw[i * 2]);
    /* největší krok mezi vzorky při přechodu nepřekročí největší krok tónu bez propusti */
    int max_off = 0, max_sw = 0;
    for (int i = 1; i < N1; i++) {
        int d = abs(off[i * 2] - off[(i - 1) * 2]);
        if (d > max_off) max_off = d;
    }
    for (int i = N1; i < N1 + 500; i++) {
        int d = abs(sw[i * 2] - sw[(i - 1) * 2]);
        if (d > max_sw) max_sw = d;
    }
    TEST_ASSERT_TRUE(max_sw <= max_off);
    /* po 10 ms (441 vzorků) přesně výstup s propustí od začátku */
    for (int i = N1 + 442; i < N1 + N2; i++) TEST_ASSERT_INT16_WITHIN(1, on[i * 2], sw[i * 2]);
    /* propust tón 440 Hz zeslabí a zaoblí: jiný výstup než bez ní */
    int diff = 0;
    for (int i = N1 + 442; i < N1 + N2; i++) if (abs(on[i * 2] - off[i * 2]) > 500) diff++;
    TEST_ASSERT_TRUE(diff > 1000);

    /* a zpět: po vypnutí a 10 ms přesně výstup bez propusti */
    init44(0, 0);
    videorec_audio_set_output_filter(&A, true, true);
    feed_psg_tone(&A, 0, END);
    videorec_audio_render(&A, (uint64_t)N1 * 400, sw, N1);
    videorec_audio_set_output_filter(&A, false, false);
    videorec_audio_render(&A, END, sw + N1 * 2, N2);
    for (int i = N1 + 442; i < N1 + N2; i++) TEST_ASSERT_EQUAL_INT16(off[i * 2], sw[i * 2]);
}

/* Úroveň nízkých kmitočtů se zapnutím propusti nemění: tón 55 Hz má stejný rozkmit (do 2 %). */
static void test_output_filter_keeps_low_frequency_level(void)
{
    enum { N = 22050 };
    static int16_t on[N * 2], off[N * 2];
    const uint64_t half = 160364; /* 17,64 MHz / 110 */
    for (int pass = 0; pass < 2; pass++) {
        init44(0, 0);
        videorec_audio_set_output_filter(&A, pass == 1, true);
        for (uint64_t t = half; t < (uint64_t)N * 400; t += half) videorec_audio_event(&A, 1, ((t / half) & 1) ? 15 : 0, t);
        TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, pass ? on : off, N));
    }
    int mx_on = 0, mx_off = 0;
    for (int i = N / 2; i < N; i++) {
        if (on[i * 2] > mx_on) mx_on = on[i * 2];
        if (off[i * 2] > mx_off) mx_off = off[i * 2];
    }
    TEST_ASSERT_TRUE(mx_off > 16000);
    TEST_ASSERT_INT_WITHIN(mx_off / 50, mx_off, mx_on);
}

/* ================================================================
 * Střídavá vazba celého výstupu (MZ-800, MZ-1500) místo parkování PSG
 * ================================================================ */

/*
 * Stálá úroveň kanálu PSG: s vazbou výstupu se neparkuje (po 45 ms tu ještě je),
 * klesne za 0,57 s na 1/e (+-10 %) a po 5 s je pod 0,1 % plného rozsahu.
 */
static void test_output_coupling_constant_level_decays(void)
{
    enum { N = 229320 }; /* 5,2 s */
    static int16_t out[N * 2];
    init44(0, 0);
    videorec_audio_set_output_coupling(&A, true);
    videorec_audio_event(&A, 1, 15, 0); /* úroveň 0,5 */
    TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, out, N));
    TEST_ASSERT_INT16_WITHIN(150, 16384, out[60 * 2]);                 /* skok prošel celý */
    TEST_ASSERT_TRUE(out[3100 * 2] > 13000);                           /* 70 ms: žádné parkování */
    double e = 16383.5 * exp(-1.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * e, e, (double)out[25137 * 2]);    /* 0,57 s = 25 137 vzorků */
    for (int i = 220500; i < N; i += 441) TEST_ASSERT_TRUE(abs(out[i * 2]) < 33); /* po 5 s pod 0,1 % rozsahu */
    TEST_ASSERT_EQUAL_INT16(out[25137 * 2], out[25137 * 2 + 1]);       /* L == R */

    /* stejně se zapnutou výstupní propustí */
    init44(0, 0);
    videorec_audio_set_output_coupling(&A, true);
    videorec_audio_set_output_filter(&A, true, true);
    videorec_audio_event(&A, 1, 15, 0);
    TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, out, N));
    TEST_ASSERT_DOUBLE_WITHIN(0.10 * e, e, (double)out[25137 * 2]);
    for (int i = 220500; i < N; i += 441) TEST_ASSERT_TRUE(abs(out[i * 2]) < 33);

    /* bez vazby (výchozí stav rendereru) se stálá úroveň po 45 ms zaparkuje jako dřív */
    init44(0, 0);
    videorec_audio_event(&A, 1, 15, 0);
    TEST_ASSERT_EQUAL_size_t(4000, videorec_audio_render(&A, 4000ull * 400, out, 4000));
    TEST_ASSERT_INT16_WITHIN(50, 16384, out[1900 * 2]);
    TEST_ASSERT_INT16_WITHIN(1, 0, out[3100 * 2]);
}

/** Rozkmit (max - min) levé strany v úseku [from, to). */
static int swing16(const int16_t *x, int from, int to)
{
    int mx = -32768, mn = 32767;
    for (int i = from; i < to; i++) {
        if (x[i * 2] > mx) mx = x[i * 2];
        if (x[i * 2] < mn) mn = x[i * 2];
    }
    return mx - mn;
}

/* Běžné tóny od 100 Hz výš vazba výstupu nezmění o víc než 0,1 dB (rozkmit), jen je posune kolem nuly. */
static void test_output_coupling_keeps_tones(void)
{
    enum { N = 132300 }; /* 3 s */
    static int16_t on[N * 2], off[N * 2];
    const uint64_t half[3] = { 88200, 20045, 8820 }; /* 100 Hz, 440 Hz, 1 kHz při 17,64 MHz */
    for (int f = 0; f < 3; f++) {
        for (int pass = 0; pass < 2; pass++) {
            init44(0, 0);
            videorec_audio_set_output_coupling(&A, pass == 1);
            for (uint64_t t = half[f]; t < (uint64_t)N * 400; t += half[f]) videorec_audio_event(&A, 1, ((t / half[f]) & 1) ? 15 : 0, t);
            TEST_ASSERT_EQUAL_size_t(N, videorec_audio_render(&A, (uint64_t)N * 400, pass ? on : off, N));
        }
        int s_on = swing16(on, N - 8820, N), s_off = swing16(off, N - 8820, N);
        TEST_ASSERT_TRUE(s_off > 15000);
        TEST_ASSERT_DOUBLE_WITHIN(0.1, 0.0, 20.0 * log10((double)s_on / (double)s_off));
        /* bez vazby jednostranný (0 až úroveň), s vazbou po 3 s souměrný kolem nuly */
        double m_on = 0, m_off = 0;
        for (int i = N - 8820; i < N; i++) { m_on += on[i * 2]; m_off += off[i * 2]; }
        TEST_ASSERT_TRUE(m_off / 8820.0 > 7000.0);
        TEST_ASSERT_DOUBLE_WITHIN(150.0, 0.0, m_on / 8820.0);
    }
}

/* Stav vazby výstupu jde nastavit: ustálená střední hodnota = stálá úroveň nedá nic (začátek nahrávky bez posunu). */
static void test_output_coupling_seeded_mean(void)
{
    static int16_t out[2000 * 2];
    init44(0, 15); /* PSG trvale na úrovni 0,5 */
    videorec_audio_set_output_coupling(&A, true);
    videorec_audio_set_output_mean(&A, 0.5, 0.5);
    A.flt[1].lp = 0.5f; /* IIR kanálu ustálený na úrovni (jako po dlouhém běhu) */
    TEST_ASSERT_EQUAL_size_t(2000, videorec_audio_render(&A, 2000ull * 400, out, 2000));
    for (int i = 0; i < 2000; i += 50) TEST_ASSERT_INT16_WITHIN(2, 0, out[i * 2]);

    /* stav je součástí uloženého stavu rendereru */
    st_VIDEOREC_AUDIO_STATE st;
    videorec_audio_get_state(&A, &st);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, st.out_ac[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, st.out_ac[1]);
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
    init44(0, 0);
    videorec_audio_event(&A, 0, 15, 0); /* skok CTC0 na 1 (ustálená úroveň by přes střídavou vazbu nedala nic) */
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
    RUN_TEST(test_ctc0_ac_constant_decays);
    RUN_TEST(test_ctc0_ac_square_symmetric);
    RUN_TEST(test_output_stage_gain_zero_is_legacy);
    RUN_TEST(test_output_stage_lowpass_response);
    RUN_TEST(test_soft_limit_properties);
    RUN_TEST(test_output_stage_makeup_gain);
    RUN_TEST(test_render_makeup_and_limiter);
    RUN_TEST(test_output_filter_switch_without_click);
    RUN_TEST(test_output_filter_keeps_low_frequency_level);
    RUN_TEST(test_output_coupling_constant_level_decays);
    RUN_TEST(test_output_coupling_keeps_tones);
    RUN_TEST(test_output_coupling_seeded_mean);
    RUN_TEST(test_psg_iir_step);
    RUN_TEST(test_rebase_keeps_filter_state);
    RUN_TEST(test_state_restore_continuity);
    return UNITY_END();
}
