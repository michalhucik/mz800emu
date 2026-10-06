/**
 * @file   test_videorec_rt.c
 * @brief  Standalone testy režimu "podle reality": hodiny vzorkovače, převzorkování a korekce driftu.
 *
 * Simulace běží ve virtuálním čase (us): producent (SDL callback) přidává
 * bloky 882 vzorků 44,1 kHz v termínech podle hodin "zařízení" (s volitelným
 * driftem a jitterem), spotřebitel (vzorkovač) odebírá 960 vzorků 48 kHz
 * každý snímek (20 ms při 50 fps) monotónních hodin.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "emulator/videorec/videorec_rt.h"

#define IN_RATE 44100u
#define OUT_RATE 48000u
#define CHUNK 882u   /**< Blok SDL cesty (20 ms při 44,1 kHz). */
#define OUT_TICK 960u /**< Výstup jednoho ticku při 48 kHz. */
#define PERIOD 20000 /**< Perioda ticku [us]. */

static st_VIDEOREC_RT_AUDIO RA;

void setUp(void)
{
    videorec_rt_audio_init(&RA, IN_RATE, OUT_RATE, VIDEOREC_RT_AUDIO_TARGET_MS, VIDEOREC_RT_AUDIO_CAPACITY_MS);
    videorec_rt_audio_reset(&RA, true, 0);
}

void tearDown(void)
{
    videorec_rt_audio_free(&RA);
}

/* ================================================================
 * Hodiny vzorkovače
 * ================================================================ */

/* První volání založí řadu a vydá tick 0; další ticky přesně po periodě. */
static void test_clock_first_tick_and_period(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    TEST_ASSERT_EQUAL_INT64(0, videorec_rt_clock_next_deadline(&c));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 1000000));
    TEST_ASSERT_EQUAL_INT64(1000000 + PERIOD, videorec_rt_clock_next_deadline(&c));
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 1000000 + PERIOD - 1));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 1000000 + PERIOD));
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 1000000 + PERIOD + 10));
}

/* Zpoždění do max_catchup se dohání; termíny zůstávají absolutní (bez driftu). */
static void test_clock_catchup_without_drift(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    (void)videorec_rt_clock_due(&c, 0);
    TEST_ASSERT_EQUAL_UINT(3, videorec_rt_clock_due(&c, 3 * PERIOD + 5));
    /* 10 000 ticků s pozdním probuzením o 7 ms: celkem přesně tolik ticků, kolik termínů uběhlo */
    uint64_t total = 4;
    for (int k = 4; k < 10000; k++) total += videorec_rt_clock_due(&c, (int64_t)k * PERIOD + 7000);
    TEST_ASSERT_EQUAL_UINT64(10000, total);
    TEST_ASSERT_EQUAL_UINT64(0, c.rebased);
}

/* Zpoždění nad max_catchup: nová řada od aktuálního času, vydá se 1 tick. */
static void test_clock_rebase_on_large_lag(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    (void)videorec_rt_clock_due(&c, 0);
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 100 * PERIOD));
    TEST_ASSERT_EQUAL_UINT64(1, c.rebased);
    TEST_ASSERT_TRUE(c.lost_ticks >= 50);
    TEST_ASSERT_EQUAL_INT64(101 * PERIOD, videorec_rt_clock_next_deadline(&c));
}

/* 60 snímků/s (MZ-1500, MZ-700 NTSC): perioda 16 666,67 us bez zaokrouhlení - za 1 s přesně
 * 60 ticků, za 1 h přesně 216 000 (celočíselná perioda 16 666 us by dala o 13 ticků víc). */
static void test_clock_fractional_period_60fps(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init_fps(&c, 60, 1, 60);
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 0));
    TEST_ASSERT_EQUAL_INT64(16667, videorec_rt_clock_next_deadline(&c)); /* ceil(1e6 / 60) */
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 16666));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 16667));
    TEST_ASSERT_EQUAL_INT64(33334, videorec_rt_clock_next_deadline(&c)); /* ceil(2e6 / 60) */
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 33334));
    /* tick 3 přesně v 50 000 us */
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 49999));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 50000));
    uint64_t total = 4;
    for (int64_t t = 50000 + 1000; t <= 3600LL * 1000000; t += 1000) total += videorec_rt_clock_due(&c, t);
    TEST_ASSERT_EQUAL_UINT64(216001, total); /* ticky 0 .. 216000 včetně (t = 3600 s) */
    TEST_ASSERT_EQUAL_UINT64(0, c.rebased);
}

/* 50 snímků/s přes videorec_rt_clock_init_fps() = stejné termíny jako perioda 20 000 us. */
static void test_clock_fps_50_equals_period(void)
{
    st_VIDEOREC_RT_CLOCK a, b;
    videorec_rt_clock_init_fps(&a, 50, 1, 50);
    videorec_rt_clock_init(&b, PERIOD, 50);
    for (int64_t t = 0; t < 2000000; t += 777) {
        TEST_ASSERT_EQUAL_UINT(videorec_rt_clock_due(&b, t), videorec_rt_clock_due(&a, t));
        TEST_ASSERT_EQUAL_INT64(videorec_rt_clock_next_deadline(&b), videorec_rt_clock_next_deadline(&a));
    }
}

/* Zpoždění obrazu je dané v ms (VIDEOREC_RT_VIDEO_DELAY_MS = 40) a na ticky se
 * zaokrouhluje podle fps: 50 i 60 snímků/s = 2 ticky (60: 2,4 -> 2), nejméně 1 tick. */
static void test_video_delay_ticks_from_ms(void)
{
    TEST_ASSERT_EQUAL_UINT(40u, VIDEOREC_RT_VIDEO_DELAY_MS);
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(50, 1));
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(60, 1));
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(60000, 1001)); /* 59,94 */
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(25, 1));      /* 1,0 */
    TEST_ASSERT_EQUAL_UINT(4u, videorec_rt_video_delay_ticks(100, 1));
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(1, 1));       /* 0,04 -> min. 1 */
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(0, 0));       /* 0 se nahradí 1 (jako u hodin) -> min. 1 */
}

/* Blok SDL cesty = VIDEOREC_RT_SDL_RATE / fps: 882 při 50, 735 při 60 snímcích/s. */
static void test_sdl_chunk_per_fps(void)
{
    TEST_ASSERT_EQUAL_UINT(882u, videorec_rt_sdl_chunk(50, 1));
    TEST_ASSERT_EQUAL_UINT(735u, videorec_rt_sdl_chunk(60, 1));
    TEST_ASSERT_EQUAL_UINT(882u, VIDEOREC_RT_SDL_CHUNK_MAX);
    TEST_ASSERT_TRUE(videorec_rt_sdl_chunk(60, 1) <= VIDEOREC_RT_SDL_CHUNK_MAX);
}

/* ================================================================
 * Zvukový buffer - pomocné funkce simulace
 * ================================================================ */

/** @brief Výsledek simulace. */
typedef struct {
    uint64_t out_frames;   /**< Vydaných výstupních vzorků celkem. */
    size_t min_fill;       /**< Minimální naplnění po prvním naplnění. */
    size_t max_fill;       /**< Maximální naplnění po prvním naplnění. */
    double sum_fill;       /**< Součet naplnění (pro průměr). */
    unsigned n_fill;       /**< Počet měření naplnění. */
    double sum_corr;       /**< Součet korekce v poslední třetině simulace. */
    double min_corr;       /**< Minimální korekce v poslední třetině. */
    double max_corr;       /**< Maximální korekce v poslední třetině. */
    unsigned n_corr;       /**< Počet měření korekce. */
    size_t late_min_fill;  /**< Minimální naplnění v poslední třetině. */
    size_t late_max_fill;  /**< Maximální naplnění v poslední třetině. */
} st_SIM;

/** Producent: konstantní hodnota (pro testy driftu na obsahu nezáleží). */
static void push_chunk(float v)
{
    float buf[CHUNK * 2];
    for (unsigned i = 0; i < CHUNK * 2; i++) buf[i] = v;
    videorec_rt_audio_push(&RA, buf, CHUNK);
}

/**
 * Simulace: producent s hodinami zařízení posunutými o `drift` (relativně,
 * kladný = zařízení rychlejší), bloky v dávkách po `burst` (jitter), spotřebitel
 * každý snímek (20 ms při 50 fps) monotónních hodin.
 */
static st_SIM simulate(double seconds, double drift, unsigned burst)
{
    st_SIM s;
    memset(&s, 0, sizeof(s));
    s.min_fill = (size_t)-1;
    s.late_min_fill = (size_t)-1;
    s.min_corr = 1.0;
    s.max_corr = -1.0;
    int16_t out[OUT_TICK * 2];
    double prod_period = (double)PERIOD * burst / (1.0 + drift);
    double next_prod = 0.0;
    int64_t end = (int64_t)(seconds * 1e6);
    bool primed = false;
    for (int64_t t = 0; t < end; t += PERIOD) {
        /* producent: všechny dávky s termínem <= t (zařízení si říká o data po dávkách) */
        while (next_prod <= (double)t) {
            for (unsigned b = 0; b < burst; b++) push_chunk(0.5f);
            next_prod += prod_period;
        }
        /* naplnění před odběrem (to regulátor drží na cíli) */
        size_t f = videorec_rt_audio_fill(&RA);
        if (primed) {
            if (f < s.min_fill) s.min_fill = f;
            if (f > s.max_fill) s.max_fill = f;
            s.sum_fill += (double)f;
            s.n_fill++;
        }
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
        s.out_frames += OUT_TICK;
        if (!RA.starved) primed = true;
        if (t >= end * 2 / 3) {
            double c = RA.stats.corr;
            s.sum_corr += c;
            s.n_corr++;
            if (c < s.min_corr) s.min_corr = c;
            if (c > s.max_corr) s.max_corr = c;
            if (f < s.late_min_fill) s.late_min_fill = f;
            if (f > s.late_max_fill) s.late_max_fill = f;
        }
    }
    return s;
}

/* ================================================================
 * Zvukový buffer - testy
 * ================================================================ */

/* Bez driftu: po naplnění žádné podtečení ani přetečení, naplnění kolem cíle. */
static void test_audio_steady_no_drift(void)
{
    st_SIM s = simulate(60.0, 0.0, 1);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(60 * 50 * OUT_TICK, s.out_frames);
    TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
    TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
    double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
    double avg = s.sum_fill / s.n_fill;
    TEST_ASSERT_DOUBLE_WITHIN(CHUNK, target, avg);
    /* ticho jen při počátečním plnění (cíl 3 bloky = 2 ticky čekání) */
    TEST_ASSERT_TRUE(st.silence <= 3 * OUT_TICK);
}

/**
 * Drift hodin zařízení +-0,2 % (řádově víc než reálná zařízení): regulátor ho
 * dorovná, naplnění zůstane omezené kolem cíle a nedojde k podtečení ani
 * přetečení - zvuk proti obrazu nedriftuje.
 */
static void test_audio_drift_corrected(void)
{
    const double drifts[] = { +0.002, -0.002 };
    for (unsigned i = 0; i < 2; i++) {
        videorec_rt_audio_reset(&RA, true, 0);
        st_SIM s = simulate(300.0, drifts[i], 1);
        st_VIDEOREC_RT_AUDIO_STATS st;
        videorec_rt_audio_get_stats(&RA, &st);
        TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
        TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
        /* průměrná korekce v ustáleném stavu odpovídá driftu */
        double mean_corr = s.sum_corr / s.n_corr;
        printf("drift %+.4f: mean corr %+.5f (min %+.5f, max %+.5f), late fill %u..%u, max fill %u\n", drifts[i],
               mean_corr, s.min_corr, s.max_corr, (unsigned)s.late_min_fill, (unsigned)s.late_max_fill,
               (unsigned)s.max_fill);
        TEST_ASSERT_DOUBLE_WITHIN(0.0002, drifts[i], mean_corr);
        /* kolísání korekce (výška tónu) v ustáleném stavu nejvýš 0,3 % špička-špička */
        TEST_ASSERT_TRUE(s.max_corr - s.min_corr < 0.003);
        double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
        /* P regulátor má trvalou odchylku = drift * in_rate * TC (882 vzorků = 20 ms při 0,2 %);
         * naplnění zůstává omezené - zvuk proti obrazu nedriftuje */
        TEST_ASSERT_DOUBLE_WITHIN(2.0 * CHUNK, target, st.fill_ema);
        TEST_ASSERT_TRUE(s.late_max_fill < target + 3 * CHUNK);
        TEST_ASSERT_TRUE(s.late_min_fill > CHUNK + 2);
    }
}

/* Jitter: zařízení si bere data po dvou blocích každých 40 ms - stále bez podtečení. */
static void test_audio_bursty_producer(void)
{
    (void)simulate(30.0, 0.0, 2);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
    TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
}

/* Výpadek producenta (pauza zařízení): ticho, pak znovu naplnění na cíl a pokračování. */
static void test_audio_starve_and_reprime(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 10; k++) {
        push_chunk(0.5f);
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    }
    TEST_ASSERT_FALSE(RA.starved);
    /* výpadek */
    for (int k = 0; k < 5; k++) videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(RA.starved);
    TEST_ASSERT_EQUAL_INT16(0, out[0]);
    TEST_ASSERT_EQUAL_INT16(0, out[OUT_TICK * 2 - 1]);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_TRUE(st.underruns >= 1);
    /* dva bloky nestačí na cíl (3 bloky) - pořád ticho a nic se nespotřebuje */
    push_chunk(0.5f);
    push_chunk(0.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(0, out[OUT_TICK]);
    TEST_ASSERT_EQUAL_UINT(2 * CHUNK, videorec_rt_audio_fill(&RA));
    /* třetí blok = cíl: data */
    push_chunk(0.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_FALSE(RA.starved);
    TEST_ASSERT_INT16_WITHIN(2, 16384, out[OUT_TICK]);
}

/* Zesílení: 0 = ticho při zachované spotřebě, 0,25 = útlum; NULL výstup jen spotřebuje. */
static void test_audio_gain_and_discard(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 4; k++) push_chunk(0.8f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 0.25f);
    TEST_ASSERT_INT16_WITHIN(2, (int16_t)lrintf(0.2f * 32767.0f), out[100]);
    size_t f1 = videorec_rt_audio_fill(&RA);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 0.0f);
    TEST_ASSERT_EQUAL_INT16(0, out[100]);
    size_t f2 = videorec_rt_audio_fill(&RA);
    TEST_ASSERT_TRUE(f2 < f1); /* spotřebováno */
    videorec_rt_audio_pull(&RA, NULL, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) < f2);
}

/* Ořez: |gain * vzorek| > 1 se ořízne na int16 rozsah. */
static void test_audio_clip(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 3; k++) push_chunk(1.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(32767, out[10]);
    for (int k = 0; k < 3; k++) push_chunk(-1.5f);
    videorec_rt_audio_reset(&RA, true, 0);
    for (int k = 0; k < 3; k++) push_chunk(-1.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(-32767, out[10]);
}

/* Zakázaný buffer zahazuje data; wants_data jen pod cílem + blok. */
static void test_audio_disabled_and_wants_data(void)
{
    videorec_rt_audio_reset(&RA, false, 0);
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_data(&RA, CHUNK));
    push_chunk(0.5f);
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_audio_fill(&RA));
    videorec_rt_audio_reset(&RA, true, 0);
    unsigned pushes = 0;
    while (videorec_rt_audio_wants_data(&RA, CHUNK)) {
        push_chunk(0.5f);
        pushes++;
    }
    /* cíl 3 bloky + 1 blok rezervy */
    TEST_ASSERT_EQUAL_UINT(RA.target / CHUNK + 1, pushes);
    TEST_ASSERT_EQUAL_UINT(RA.target + CHUNK, videorec_rt_audio_fill(&RA));
}

/* Přetečení kapacity: nejstarší data se zahodí, naplnění nepřekročí kapacitu. */
static void test_audio_capacity_overflow(void)
{
    for (int k = 0; k < 100; k++) push_chunk(0.5f); /* 2 s > kapacita 1 s */
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) <= RA.cap);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_TRUE(st.overflow_drops > 0);
    /* první pull zahodí přebytek nad cíl (overrun) a pokračuje plynule */
    int16_t out[OUT_TICK * 2];
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(1, st.overruns);
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) <= RA.target);
}

/**
 * Kvalita převzorkování 44,1 -> 48 kHz: sinus 1 kHz má po převzorkování
 * frekvenci 1 kHz (průchody nulou) a amplitudu v toleranci lineární interpolace.
 */
static void test_audio_resample_sine(void)
{
    const double f = 1000.0;
    float buf[CHUNK * 2];
    uint64_t n_in = 0;
    int16_t out[OUT_TICK * 2];
    unsigned crossings = 0;
    int16_t prev = 0, maxv = 0;
    bool have_prev = false;
    uint64_t counted = 0;
    for (int tick = 0; tick < 200; tick++) {
        for (unsigned i = 0; i < CHUNK; i++, n_in++) {
            float v = (float)(0.5 * sin(2.0 * G_PI * f * (double)n_in / IN_RATE));
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        videorec_rt_audio_push(&RA, buf, CHUNK);
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
        if (tick < 20) continue; /* ustálení */
        for (unsigned i = 0; i < OUT_TICK; i++) {
            int16_t v = out[2 * i];
            TEST_ASSERT_EQUAL_INT16(v, out[2 * i + 1]);
            if (v > maxv) maxv = v;
            if (have_prev && prev < 0 && v >= 0) crossings++;
            prev = v;
            have_prev = true;
            counted++;
        }
    }
    double secs = (double)counted / OUT_RATE;
    TEST_ASSERT_DOUBLE_WITHIN(0.005 * f, f, crossings / secs);
    /* amplituda 0,5 * 32767; lineární interpolace (< 0,3 %) a vzorkování vrcholu při 48 kHz (< 0,3 %) */
    TEST_ASSERT_INT_WITHIN(150, 16384, maxv);
}

/* Při stejné vstupní a výstupní frekvenci a bez driftu je výstup přesná kopie vstupu. */
static void test_audio_identity_44k(void)
{
    videorec_rt_audio_free(&RA);
    videorec_rt_audio_init(&RA, IN_RATE, IN_RATE, VIDEOREC_RT_AUDIO_TARGET_MS, VIDEOREC_RT_AUDIO_CAPACITY_MS);
    videorec_rt_audio_reset(&RA, true, 0);
    float buf[CHUNK * 2];
    int16_t out[CHUNK * 2];
    uint64_t n = 0;
    for (unsigned i = 0; i < CHUNK; i++) buf[2 * i] = buf[2 * i + 1] = (float)i / CHUNK;
    for (int k = 0; k < 3; k++) videorec_rt_audio_push(&RA, buf, CHUNK); /* = cíl */
    videorec_rt_audio_pull(&RA, out, CHUNK, 1.0f);
    for (unsigned i = 0; i < CHUNK; i++, n++) {
        TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrintf((float)i / CHUNK * 32767.0f), out[2 * i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clock_first_tick_and_period);
    RUN_TEST(test_clock_catchup_without_drift);
    RUN_TEST(test_clock_rebase_on_large_lag);
    RUN_TEST(test_clock_fractional_period_60fps);
    RUN_TEST(test_clock_fps_50_equals_period);
    RUN_TEST(test_video_delay_ticks_from_ms);
    RUN_TEST(test_sdl_chunk_per_fps);
    RUN_TEST(test_audio_steady_no_drift);
    RUN_TEST(test_audio_drift_corrected);
    RUN_TEST(test_audio_bursty_producer);
    RUN_TEST(test_audio_starve_and_reprime);
    RUN_TEST(test_audio_gain_and_discard);
    RUN_TEST(test_audio_clip);
    RUN_TEST(test_audio_disabled_and_wants_data);
    RUN_TEST(test_audio_capacity_overflow);
    RUN_TEST(test_audio_resample_sine);
    RUN_TEST(test_audio_identity_44k);
    return UNITY_END();
}
