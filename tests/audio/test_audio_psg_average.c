/**
 * @file   test_audio_psg_average.c
 * @brief  Průměr výchylky PSG přes interval výstupního vzorku (iface_audio_resampler_psg_average()).
 *
 * SDL cesta dřív brala z logu PSG jeden bod na výstupní vzorek; harmonické
 * obdélníku nad 22 kHz se tím překládaly do basového pásma (MZ-1500, dělič
 * 28: falešný tón 154,69 Hz o 94,9 % amplitudy nosné). Nově vzorek =
 * integrál výchylky přes přesný interval vzorku / jeho délka, se stejnými
 * hranicemi jako renderer (celé takty na souvislé ose, iface_audio_block_ticks_for()).
 *
 * Test staví globální časovou osu hodnot kanálu (náhodné změny, husté
 * i řídké, změny přesně na hranicích vzorků a snímků, úseky nulové délky),
 * krájí ji na logy snímků (log začíná `off` taktů před začátkem snímku jako
 * po posledním kroku PSG) a každý vzorek srovná s integrálem po taktech přes
 * jeho interval na globální ose; první vzorek bloku se kusem z konce
 * předchozího snímku (přenos `carry`). Shoda je bitová: součty výchylek
 * (float) krát celé takty jsou v double přesné, takže nezávisí na pořadí.
 * PAL i NTSC, 100 %, 50 % a 200 % (délka snímku logu), blok po bloku
 * navazující i nenavazující log (contiguous false: kus před začátkem
 * snímku má výchylku začátku snímku).
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "iface/iface_audio.h"
#include "iface/iface_audio_resampler.h"

/** @brief Vzorkovací kmitočet SDL cesty. */
#define RATE 44100ull
/** @brief Nejvíc snímků scénáře. */
#define FRAMES 8u
/** @brief Nejdelší snímek logu [takty] (200 % PAL). */
#define SPAN_MAX (2u * 354432u)
/** @brief Délka globální osy [takty]. */
#define AXIS_MAX (FRAMES * SPAN_MAX + 4096u)
/** @brief Nejvíc událostí logu snímku. */
#define EVENTS_MAX 80000u

/** @brief Hodnota kanálu v každém taktu globální osy. */
static uint8_t s_axis[AXIS_MAX];
/** @brief Výchylky hodnot logu 0 až 31 (různé řády, jako tabulka SN76489). */
static float s_vol[32];
/** @brief Události logu snímku. */
static st_AUDIO_LOG_EVENT s_ev[EVENTS_MAX];

void setUp(void) {}
void tearDown(void) {}

/** @brief Deterministický generátor (xorshift64). */
static uint64_t rnd(uint64_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 7;
    *st ^= *st << 17;
    return *st;
}

/**
 * @brief Naplní globální osu hodnot.
 * @param mode 0 = řídké změny (tón ~ 1 kHz), 1 = husté (každých 1 až 40 taktů),
 *             2 = žádná změna, 3 = změny přesně na hranicích vzorků (`ticks`) a snímků.
 * @param len  Délka osy.
 * @param span Délka snímku (režim 3).
 * @param st   Stav generátoru.
 */
static void fill_axis(int mode, size_t len, uint64_t span, uint64_t *st)
{
    uint8_t v = 15;
    size_t t = 0;
    while (t < len) {
        size_t run;
        switch (mode) {
            case 0: run = 800u + rnd(st) % 400u; break;
            case 1: run = 1u + rnd(st) % 40u; break;
            case 2: run = len; break;
            default: run = 1u + rnd(st) % 900u; break;
        }
        for (size_t k = 0; k < run && t < len; k++) s_axis[t++] = v;
        v = (uint8_t)(rnd(st) % 32u);
    }
    if (mode == 3) {
        /* změny na každé hranici snímku a na celých taktech kolem nich */
        for (size_t f = 1; f * span < len; f++) {
            for (size_t k = 0; k < 2 && f * span + k < len; k++) s_axis[f * span + k] = (uint8_t)((f * 7u + k) % 32u);
        }
    }
}

/**
 * @brief Log snímku z globální osy: skutečné úseky od taktu `from` do `valid`, pak držená hodnota do `to`.
 *
 * Jako zvukový log PSG: skutečné hodnoty jen do posledního kroku PSG
 * (`valid`), za ním do konce snímku drží poslední hodnotu.
 * @return Log (ukazuje do s_ev).
 */
static st_AUDIO_SOURCE_LOG make_log(uint64_t from, uint64_t valid, uint64_t to)
{
    st_AUDIO_SOURCE_LOG log;
    unsigned n = 0;
    uint64_t t = from;
    while (t < valid && n < EVENTS_MAX) {
        uint64_t e = t + 1;
        while (e < valid && s_axis[e] == s_axis[t]) e++;
        s_ev[n].value = (AUDIO_BUF_t)s_axis[t];
        s_ev[n].count_ticks = (uint32_t)(e - t);
        n++;
        t = e;
    }
    if (n > 0) s_ev[n - 1].count_ticks += (uint32_t)(to - valid);   /* držená hodnota do konce snímku */
    /* úsek nulové délky uprostřed (zápis bez změny času) nesmí nic přidat */
    if (n > 3 && n < EVENTS_MAX) {
        memmove(&s_ev[2], &s_ev[1], (n - 1) * sizeof(s_ev[0]));
        s_ev[1].count_ticks = 0;
        s_ev[1].value = 31;
        n++;
    }
    log.last_timestamp = to - from;
    log.last_value = s_ev[n - 1].value;
    log.samples = s_ev;
    log.samples_count = n;
    return log;
}

/** @brief Integrál výchylky po taktech přes `[a, b)` globální osy (skutečné hodnoty). */
static double brute(uint64_t a, uint64_t b)
{
    double s = 0.0;
    for (uint64_t t = a; t < b; t++) s += (double)s_vol[s_axis[t]];
    return s;
}

/** @brief Totéž, ale od taktu `valid` výš držená hodnota taktu `valid - 1` (co log snímku zná). */
static double brute_held(uint64_t a, uint64_t b, uint64_t valid)
{
    double s = 0.0;
    for (uint64_t t = a; t < b; t++) s += (double)s_vol[s_axis[(t < valid) ? t : valid - 1]];
    return s;
}

/**
 * @brief Projde FRAMES bloků jedné normy a rychlosti, srovná každý vzorek s integrálem na globální ose.
 *
 * Snímek k má na konci držený úsek délky h_k (0 až 79 taktů, jako konec za
 * posledním krokem PSG); log snímku k + 1 začíná posledním krokem PSG, tedy
 * h_k taktů před začátkem snímku. Vzorek, který do drženého úseku svého
 * snímku zasahuje, počítá drženou hodnotou; přenesený kus prvního vzorku
 * dalšího bloku musí mít skutečné hodnoty (z dalšího logu).
 * @param ticks_frame Takty jmenovitého snímku.
 * @param crystal     Krystal [Hz].
 * @param pct         Rychlost [%] (délka snímku logu = pct * takty / 100).
 * @param hold        0 = bez drženého konce, 1 = h_k = (32 k + 32) mod 80, 2 = náhodně 1 až 79.
 * @param mode        Režim osy (fill_axis()).
 * @param breaks      Každý třetí blok jako nenavazující log.
 * @return Počet porovnaných vzorků.
 */
static unsigned run_case(uint64_t ticks_frame, uint64_t crystal, unsigned pct, int hold, int mode, bool breaks)
{
    static int64_t ticks[1024];
    static float avg[1024];
    static uint8_t endv[1024];
    const uint64_t span = ticks_frame * pct / 100u;
    uint64_t st = 0x9E3779B97F4A7C15ull ^ (ticks_frame * 31u + pct * 7u + (uint64_t)hold * 131u + (uint64_t)mode * 1000003u);
    const uint64_t base = 2048u; /* globální takt začátku snímku 0 (místo pro kus před ním) */
    fill_axis(mode, base + FRAMES * span + 2048u, span, &st);
    st_IFACE_AUDIO_BLOCK_CLOCK clk = { 0 };
    st_IFACE_AUDIO_PSG_CARRY carry;
    memset(&carry, 0, sizeof(carry));
    uint64_t prev_tail_start = 0; /* globální začátek kusu za poslední hranicí předchozího bloku */
    uint64_t off = (hold == 0) ? 0u : 17u; /* log snímku 0 začíná 17 taktů před ním */
    unsigned compared = 0;
    for (unsigned f = 0; f < FRAMES; f++) {
        const uint64_t lead = clk.rem;
        const size_t n = iface_audio_block_clock_step(&clk, RATE, ticks_frame, crystal);
        iface_audio_block_ticks_for(lead, n, span, RATE, ticks_frame, crystal, ticks);
        const uint64_t fs = base + f * span; /* globální začátek snímku */
        const uint64_t h = (hold == 0) ? 0u : (hold == 1) ? (32u * f + 32u) % 80u : 1u + rnd(&st) % 79u;
        const uint64_t valid = fs + span - h;
        st_AUDIO_SOURCE_LOG log = make_log(fs - off, valid, fs + span);
        const bool contiguous = (f > 0) && !(breaks && (f % 3u) == 2u);
        iface_audio_resampler_psg_average(&log, off, span, (hold == 0) ? UINT64_MAX : valid - (fs - off), ticks, n,
                                          contiguous, s_vol, &carry, avg, endv);
        TEST_ASSERT_TRUE(ticks[0] <= 0 && ticks[n] <= (int64_t)span);
        for (size_t i = 0; i < n; i++) {
            double sum;
            uint64_t len;
            const uint64_t b = fs + (uint64_t)ticks[i + 1];
            if (i > 0) {
                const uint64_t a = fs + (uint64_t)ticks[i];
                sum = brute_held(a, b, valid);
                len = b - a;
            } else if (contiguous) {
                /* kus za poslední hranicí předchozího snímku (skutečné hodnoty) + začátek tohoto */
                sum = brute(prev_tail_start, fs) + brute_held(fs, b, valid);
                len = b - prev_tail_start;
            } else {
                /* nová časová osa: kus před začátkem snímku má výchylku začátku snímku */
                const uint64_t lead_len = (ticks[0] < 0) ? (uint64_t)(-ticks[0]) : 0u;
                sum = (double)s_vol[s_axis[fs]] * (double)lead_len + brute_held(fs, b, valid);
                len = lead_len + (b - fs);
            }
            const float want = (len > 0) ? (float)(sum / (double)len) : s_vol[s_axis[b - 1]];
            if (avg[i] != want) {
                printf("MISMATCH %llu ticks %u %% hold %d mode %d frame %u sample %zu: got %.9g want %.9g\n",
                       (unsigned long long)ticks_frame, pct, hold, mode, f, i, (double)avg[i], (double)want);
            }
            TEST_ASSERT_TRUE(avg[i] == want);
            TEST_ASSERT_EQUAL_UINT8(s_axis[((b - 1) < valid) ? b - 1 : valid - 1], endv[i]);
            compared++;
        }
        prev_tail_start = fs + (uint64_t)ticks[n];
        off = h; /* další log začíná posledním krokem PSG tohoto snímku */
    }
    return compared;
}

/** @brief Všechny kombinace jedné normy. */
static void check_norm(uint64_t ticks_frame, uint64_t crystal)
{
    static const unsigned speeds[] = { 100, 50, 200 };
    unsigned total = 0;
    for (unsigned s = 0; s < 3; s++) {
        for (int hold = 0; hold < 3; hold++) {
            for (int mode = 0; mode < 4; mode++) {
                total += run_case(ticks_frame, crystal, speeds[s], hold, mode, false);
                total += run_case(ticks_frame, crystal, speeds[s], hold, mode, true);
            }
        }
    }
    printf("PSG average %llu ticks: %u samples equal to the tick-by-tick integral\n",
           (unsigned long long)ticks_frame, total);
}

/** PAL (MZ-800, krystal 17 734 475 Hz, 354 432 taktů). */
void test_psg_average_pal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_norm(354432u, 17734475u);
}

/** NTSC (MZ-1500, krystal 14 318 180 Hz, 238 944 taktů). */
void test_psg_average_ntsc(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_norm(238944u, 14318180u);
}

/**
 * Stálá hodnota: každý vzorek je přesně výchylka hodnoty (bez chyby dělení),
 * takže úroveň a stejnosměrná složka nízkých tónů se průměrováním nemění.
 */
void test_psg_average_constant_exact(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static int64_t ticks[1024];
    static float avg[1024];
    st_IFACE_AUDIO_BLOCK_CLOCK clk = { 0 };
    st_IFACE_AUDIO_PSG_CARRY carry;
    memset(&carry, 0, sizeof(carry));
    for (unsigned v = 0; v < 32; v++) {
        st_AUDIO_LOG_EVENT ev = { 354432u, (AUDIO_BUF_t)v };
        st_AUDIO_SOURCE_LOG log = { 354432u, (AUDIO_BUF_t)v, &ev, 1 };
        const uint64_t lead = clk.rem;
        const size_t n = iface_audio_block_clock_step(&clk, RATE, 354432u, 17734475u);
        iface_audio_block_ticks_for(lead, n, 354432u, RATE, 354432u, 17734475u, ticks);
        iface_audio_resampler_psg_average(&log, 0, 354432u, UINT64_MAX, ticks, n, v > 0, s_vol, &carry, avg, NULL);
        for (size_t i = 1; i < n; i++) TEST_ASSERT_TRUE(avg[i] == s_vol[v]);
    }
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    for (unsigned v = 0; v < 32; v++) s_vol[v] = (v == 0) ? 0.0f : 0.8f / (float)(1u << (v % 8u)) + 0.0013f * (float)v;

    UNITY_BEGIN();
    RUN_TEST(test_psg_average_pal);
    RUN_TEST(test_psg_average_ntsc);
    RUN_TEST(test_psg_average_constant_exact);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
