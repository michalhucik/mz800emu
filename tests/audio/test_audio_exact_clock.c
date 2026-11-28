/**
 * @file   test_audio_exact_clock.c
 * @brief  Převod snímků na skutečný čas podle krystalu ve zvukové cestě (iface_audio.h).
 *
 * Emulátor běží celý počet taktů na snímek (VIDEO_SCREEN_TICKS); skutečný
 * snímek trvá VIDEO_SCREEN_TICKS / krystal sekund (PAL 17 734 475 Hz:
 * 50,036326 snímků/s, NTSC 14 318 180 Hz: 59,922743 snímků/s). Testuje se:
 * - akumulátor délky bloku SDL cesty (iface_audio_block_clock_step()):
 *   za 354 432 snímků PAL vydá přesně floor(44100 * 354432 * 354432 /
 *   17734475) vzorků, za 238 944 snímků NTSC floor(44100 * 238944 * 238944 /
 *   14318180), bloky mají jen dolní nebo horní celou část průměru,
 * - sestavení platformy (iface_audio_block_clock_next(),
 *   IFACE_AUDIO_BLOCK_SAMPLES_MAX) odpovídá obecné funkci s jejími konstantami,
 * - termíny tempa bez zvukového zařízení (iface_audio_frames_to_us(),
 *   iface_audio_frames_to_real_us(), které volá iface_audio_timer_sync()):
 *   přesně floor(k * takty * 1e6 / krystal) i pro velká k a za 1000 s
 *   50 036 (PAL) nebo 59 922 (NTSC) snímků,
 * - souvislá fáze vzorků (iface_audio_block_timing_for()): po N blocích je
 *   bod vzorku PSG i hranice vzorku v buňkách CTC0 přesně funkcí průběžného
 *   indexu n (čas `n * krystal / 44100`), bez skoků na hranicích bloků,
 *   PAL i NTSC, při 100 % i při vlastní rychlosti,
 * - zesílení převzorkování CTC0 (iface_audio_ctc0_sdl_gain_for()) je
 *   u každého vzorku stejné (krajní buňky vážené podílem překryvu)
 *   a s násobitelem mixu dává 1,49 / 4,
 * - bod vzorku PSG (iface_audio_block_point()) i pro nejvyšší přípustnou
 *   délku logu IFACE_AUDIO_SPAN_TICKS_MAX (dříve přetečení na 0), shoda se
 *   128bitovým výpočtem a s dřívějším zápisem všude, kde nepřetékal; buňka
 *   záporné hrany (iface_audio_block_cell()) na mezi kontraktu.
 *
 * Testy se sestavují pro MZ-800; obecné funkce se proto zkouší i s konstantami NTSC.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <stdint.h>
#include <stdio.h>

#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "iface/iface_audio.h"

/** @brief Krystal PAL [Hz]. */
#define PAL_HZ 17734475ull
/** @brief Takty snímku PAL (1136 x 312). */
#define PAL_TICKS 354432ull
/** @brief Krystal NTSC [Hz]. */
#define NTSC_HZ 14318180ull
/** @brief Takty snímku NTSC (912 x 262). */
#define NTSC_TICKS 238944ull
/** @brief Vzorkovací kmitočet SDL cesty. */
#define RATE 44100ull

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Projde `frames` bloků obecného akumulátoru a ověří součet a meze bloků.
 * @param ticks Takty snímku.
 * @param hz    Krystal.
 * @param frames Počet snímků.
 * @param lo    Dolní celá část průměrného bloku.
 */
static void check_block_clock(uint64_t ticks, uint64_t hz, uint64_t frames, size_t lo)
{
    st_IFACE_AUDIO_BLOCK_CLOCK c = { 0 };
    uint64_t sum = 0;
    uint64_t n_lo = 0, n_hi = 0;
    for (uint64_t f = 0; f < frames; f++) {
        size_t n = iface_audio_block_clock_step(&c, RATE, ticks, hz);
        if (n == lo) n_lo++;
        else if (n == lo + 1) n_hi++;
        else TEST_FAIL_MESSAGE("block length is neither floor nor ceil of the mean");
        TEST_ASSERT_TRUE(c.rem < hz);
        sum += n;
        /* po každém bloku: součet = floor(f * rate * ticks / hz) (žádný drift) */
        if ((f & 0xFFFu) == 0) {
            unsigned __int128 exact = (unsigned __int128)(f + 1) * RATE * ticks / hz;
            TEST_ASSERT_EQUAL_UINT64((uint64_t)exact, sum);
        }
    }
    unsigned __int128 exact = (unsigned __int128)frames * RATE * ticks / hz;
    TEST_ASSERT_EQUAL_UINT64((uint64_t)exact, sum);
    printf("block clock %llu Hz: %llu frames -> %llu samples (%llu x %zu, %llu x %zu)\n",
           (unsigned long long)hz, (unsigned long long)frames, (unsigned long long)sum,
           (unsigned long long)n_lo, lo, (unsigned long long)n_hi, lo + 1);
}

/** Akumulátor PAL: za 354 432 snímků přesně 312 382 073 vzorků, bloky 881 a 882. */
void test_block_clock_pal_no_drift(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_UINT64(312382073ull, (uint64_t)((unsigned __int128)PAL_TICKS * RATE * PAL_TICKS / PAL_HZ));
    check_block_clock(PAL_TICKS, PAL_HZ, PAL_TICKS, 881);
}

/** Akumulátor NTSC: za 238 944 snímků přesně 175 850 266 vzorků, bloky 735 a 736. */
void test_block_clock_ntsc_no_drift(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_UINT64(175850266ull, (uint64_t)((unsigned __int128)NTSC_TICKS * RATE * NTSC_TICKS / NTSC_HZ));
    check_block_clock(NTSC_TICKS, NTSC_HZ, NTSC_TICKS, 735);
}

/** Sestavení platformy: iface_audio_block_clock_next() = obecná funkce s konstantami platformy. */
void test_block_clock_platform(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_UINT64(PAL_HZ, GDGCLK_REAL_TICKS_PER_SEC);
    TEST_ASSERT_EQUAL_UINT64(PAL_TICKS, (uint64_t)VIDEO_SCREEN_TICKS);
    TEST_ASSERT_EQUAL_size_t(882, IFACE_AUDIO_BLOCK_SAMPLES_MAX);
    TEST_ASSERT_EQUAL_size_t(11076, IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME);
    st_IFACE_AUDIO_BLOCK_CLOCK a = { 0 }, b = { 0 };
    for (int f = 0; f < 5000; f++) {
        size_t n = iface_audio_block_clock_next(&a);
        TEST_ASSERT_EQUAL_size_t(iface_audio_block_clock_step(&b, RATE, PAL_TICKS, PAL_HZ), n);
        TEST_ASSERT_TRUE(n <= IFACE_AUDIO_BLOCK_SAMPLES_MAX);
    }
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 50.036326, GDGCLK_REAL_FPS);
}

/**
 * @brief Ověří obecný převod snímků na us proti 128bitovému výpočtu a počet snímků za 1000 s.
 * @param ticks Takty snímku.
 * @param hz    Krystal.
 * @param per_1000s Očekávaný počet celých snímků za 1000 s.
 */
static void check_frames_to_us(uint64_t ticks, uint64_t hz, uint64_t per_1000s)
{
    static const uint64_t ks[] = { 0, 1, 2, 49, 50, 59, 60, 1000, 709379, 715909, 3579545,
                                   17734475, 14318180, 17734476, 123456789ull, 4000000000ull,
                                   99999999999ull };
    for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        unsigned __int128 exact = (unsigned __int128)ks[i] * ticks * 1000000u / hz;
        TEST_ASSERT_EQUAL_INT64((int64_t)exact, iface_audio_frames_to_us(ks[i], ticks, hz));
    }
    /* termíny rostou o celou periodu nebo o 1 us víc, nikdy neklesají */
    int64_t prev = 0;
    const int64_t period = (int64_t)(ticks * 1000000u / hz);
    for (uint64_t k = 1; k < 20000; k++) {
        int64_t t = iface_audio_frames_to_us(k, ticks, hz);
        TEST_ASSERT_TRUE(t - prev == period || t - prev == period + 1);
        prev = t;
    }
    TEST_ASSERT_TRUE(iface_audio_frames_to_us(per_1000s, ticks, hz) <= 1000000000ll);
    TEST_ASSERT_TRUE(iface_audio_frames_to_us(per_1000s + 1, ticks, hz) > 1000000000ll);
}

/** Tempo bez zařízení PAL: snímek 19 985,48 us, 50 036 snímků za 1000 s (50,036 snímků/s). */
void test_timer_deadlines_pal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT64(19985, iface_audio_frames_to_us(1, PAL_TICKS, PAL_HZ));
    TEST_ASSERT_EQUAL_INT64(999274, iface_audio_frames_to_us(50, PAL_TICKS, PAL_HZ)); /* 999 274,013 us */
    /* 709 379 snímků = 14 177,28 s přesně (krystal / 25) */
    TEST_ASSERT_EQUAL_INT64(14177280000ll, iface_audio_frames_to_us(709379, PAL_TICKS, PAL_HZ));
    check_frames_to_us(PAL_TICKS, PAL_HZ, 50036);
    /* platforma sestavení (MZ-800) */
    for (uint64_t k = 0; k < 3000; k += 7) {
        TEST_ASSERT_EQUAL_INT64(iface_audio_frames_to_us(k, PAL_TICKS, PAL_HZ), iface_audio_frames_to_real_us(k));
    }
}

/** Tempo bez zařízení NTSC: snímek 16 688,15 us, 59 922 snímků za 1000 s (59,923 snímků/s). */
void test_timer_deadlines_ntsc(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT64(16688, iface_audio_frames_to_us(1, NTSC_TICKS, NTSC_HZ));
    check_frames_to_us(NTSC_TICKS, NTSC_HZ, 59922);
}

/**
 * @brief Souvislá fáze: časy vzorků N bloků za sebou proti průběžnému indexu.
 *
 * Bloky z akumulátoru (iface_audio_block_clock_step()), časování každého
 * bloku z iface_audio_block_timing_for() s fází `lead` = zbytek akumulátoru
 * před krokem. Snímek k začíná v taktu `k * span` logu (při 100 % `span` =
 * takty snímku, při vlastní rychlosti délka synchronizační události), log
 * začíná `offset` taktů před snímkem. Pro průběžný index vzorku n (od začátku)
 * musí platit přesně:
 * - bod PSG = `k * span + offset + ceil((n + 1) * krystal * span / (44100 *
 *   takty)) - k * span - 1`, tj. poslední takt intervalu vzorku n, jehož
 *   začátek je `n * krystal / 44100` jmenovitých taktů (při 100 %),
 * - poloha hranice vzorku v proudu buněk CTC0 = `n * krystal * buňky -
 *   k * buňky * 44100 * takty` (poslední hranice bloku k = první hranice
 *   bloku k + 1 posunutá o snímek), buňka hranice
 *   `floor(n * krystal * buňky / (44100 * takty)) - k * buňky`,
 * - rozdíl sousedních bodů PSG (i přes hranici bloku) je jen dolní nebo
 *   horní celá část `krystal * span / (44100 * takty)` (žádný skok),
 * - počet přenesených buněk CTC0 nepřekročí horní celou část buněk na vzorek.
 *
 * @param ticks  Takty jmenovitého snímku.
 * @param hz     Krystal.
 * @param cells  Buňky proudu CTC0 na snímek.
 * @param span   Délka snímku v logu [takty].
 * @param offset Začátek snímku v logu [takty].
 * @param blocks Počet bloků.
 */
static void check_continuous_phase(uint64_t ticks, uint64_t hz, uint64_t cells, uint64_t span, uint64_t offset,
                                   unsigned blocks)
{
    static uint64_t points[1024];
    static int64_t edges[1025];
    st_IFACE_AUDIO_BLOCK_CLOCK c = { 0 };
    uint64_t n0 = 0;                      /* průběžný index prvního vzorku bloku */
    int64_t prev_last_edge = 0;           /* poloha poslední hranice předchozího bloku (jeho souřadnice) */
    uint64_t prev_point_abs = 0;          /* poslední bod PSG předchozího bloku (absolutně, bez offset) */
    const uint64_t den = RATE * ticks;
    const uint64_t step_lo = (uint64_t)(((unsigned __int128)hz * span) / den);
    const int64_t carry_max = (int64_t)((hz * cells + den - 1u) / den);
    unsigned jumps = 0;
    for (unsigned k = 0; k < blocks; k++) {
        const uint64_t lead = c.rem;
        const size_t n = iface_audio_block_clock_step(&c, RATE, ticks, hz);
        TEST_ASSERT_TRUE(n < 1024);
        iface_audio_block_timing_for(lead, n, offset, span, RATE, ticks, hz, cells, points, edges);
        /* CTC0: navazující polohy hranic, přenos nejvýš carry_max buněk */
        const int64_t frame_pos = (int64_t)(cells * den);
        if (k > 0) {
            TEST_ASSERT_EQUAL_INT64(prev_last_edge - frame_pos, edges[0]);
        }
        const int64_t c0 = iface_audio_block_cell(edges[0] / (int64_t)cells, cells, RATE, ticks);
        const int64_t cn = edges[n] / (int64_t)den;
        TEST_ASSERT_TRUE(edges[0] <= 0 && c0 >= -carry_max);
        TEST_ASSERT_TRUE(edges[n] > 0 && edges[n] <= frame_pos);
        TEST_ASSERT_TRUE((int64_t)cells - cn <= carry_max);
        for (size_t i = 0; i <= n; i++) {
            unsigned __int128 abs_pos = (unsigned __int128)(n0 + i) * hz * cells;
            TEST_ASSERT_EQUAL_INT64((int64_t)abs_pos - (int64_t)k * frame_pos, edges[i]);
            if (i > 0) {
                TEST_ASSERT_EQUAL_INT64((int64_t)(hz * cells), edges[i] - edges[i - 1]);
            }
        }
        /* PSG: bod vzorku = poslední takt jeho intervalu na souvislé ose */
        for (size_t i = 0; i < n; i++) {
            unsigned __int128 e = (unsigned __int128)(n0 + i + 1) * hz * span;
            uint64_t want = (uint64_t)((e + den - 1u) / den) - 1u;  /* absolutní takt */
            uint64_t got = (uint64_t)k * span + points[i] - offset;
            TEST_ASSERT_EQUAL_UINT64(want, got);
            TEST_ASSERT_TRUE(points[i] >= offset && points[i] < offset + span);
            if (k > 0 || i > 0) {
                const uint64_t d = got - prev_point_abs;
                if (d != step_lo && d != step_lo + 1u) jumps++;
            }
            prev_point_abs = got;
        }
        prev_last_edge = edges[n];
        n0 += n;
    }
    TEST_ASSERT_EQUAL_UINT(0, jumps);
    printf("continuous phase %llu Hz, span %llu, offset %llu: %u blocks, %llu samples, PSG step %llu or %llu ticks\n",
           (unsigned long long)hz, (unsigned long long)span, (unsigned long long)offset, blocks,
           (unsigned long long)n0, (unsigned long long)step_lo, (unsigned long long)step_lo + 1u);
}

/** Souvislá fáze PAL (MZ-800, MZ-700 PAL): 100 %, s posunem logu a při vlastní rychlosti 50, 37 a 200 %. */
void test_continuous_phase_pal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_continuous_phase(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, PAL_TICKS, 0, 20000);
    check_continuous_phase(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, PAL_TICKS, 64, 2000);
    check_continuous_phase(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, PAL_TICKS * 50u / 100u, 0, 2000);
    check_continuous_phase(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, PAL_TICKS * 37u / 100u, 16, 2000);
    check_continuous_phase(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, PAL_TICKS * 200u / 100u, 0, 2000);
}

/** Souvislá fáze NTSC (MZ-1500, MZ-700 NTSC): 100 %, s posunem logu a při vlastní rychlosti 50 a 400 %. */
void test_continuous_phase_ntsc(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_continuous_phase(NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u, NTSC_TICKS, 0, 20000);
    check_continuous_phase(NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u, NTSC_TICKS, 32, 2000);
    check_continuous_phase(NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u, NTSC_TICKS * 50u / 100u, 0, 2000);
    check_continuous_phase(NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u, NTSC_TICKS * 400u / 100u, 0, 2000);
}

/**
 * @brief Zesílení převzorkování CTC0 ze vzorce = zesílení každého vzorku skutečného dělení.
 *
 * Vzorek trvale v jedničce dá vážený součet = délka intervalu (krajní buňky
 * podílem překryvu), zesílení `délka / (délka + d)`; má být u všech vzorků
 * všech bloků stejné a rovné iface_audio_ctc0_sdl_gain_for().
 * @param ticks Takty snímku. @param hz Krystal. @param cells Buňky na snímek. @param blocks Počet bloků.
 */
static void check_ctc0_gain(uint64_t ticks, uint64_t hz, uint64_t cells, unsigned blocks)
{
    static int64_t edges[1025];
    st_IFACE_AUDIO_BLOCK_CLOCK c = { 0 };
    const double g = iface_audio_ctc0_sdl_gain_for(RATE, ticks, hz, cells);
    const double d = (double)(RATE * ticks);
    double mn = 1.0, mx = 0.0;
    uint64_t count = 0;
    for (unsigned k = 0; k < blocks; k++) {
        const uint64_t lead = c.rem;
        const size_t n = iface_audio_block_clock_step(&c, RATE, ticks, hz);
        iface_audio_block_timing_for(lead, n, 0, ticks, RATE, ticks, hz, cells, NULL, edges);
        for (size_t i = 0; i < n; i++) {
            const double len = (double)(edges[i + 1] - edges[i]);
            const double u = len / (len + d);
            mn = (u < mn) ? u : mn;
            mx = (u > mx) ? u : mx;
            count++;
        }
    }
    printf("CTC0 gain %llu Hz: formula %.7f, per sample %.7f .. %.7f over %llu samples\n", (unsigned long long)hz, g,
           mn, mx, (unsigned long long)count);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, g, mn);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, g, mx);
}

/**
 * Násobitel CTC0 v mixu: součin se zesílením převzorkování je 1,49 / 4
 * (nezávisí na délce bloku); zesílení ze vzorce odpovídá skutečnému dělení na buňky.
 */
void test_ctc0_mix_level_continuous(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const float want = AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT;
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, want, iface_audio_ctc0_sdl_gain() * iface_audio_ctc0_mix_level());
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.926291, iface_audio_ctc0_sdl_gain_for(RATE, PAL_TICKS, PAL_HZ, PAL_TICKS / 32u));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.910283, iface_audio_ctc0_sdl_gain_for(RATE, NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u));
    check_ctc0_gain(PAL_TICKS, PAL_HZ, PAL_TICKS / 32u, 20000);
    check_ctc0_gain(NTSC_TICKS, NTSC_HZ, NTSC_TICKS / 32u, 20000);
    /* sestavení platformy: přenos buněk */
    TEST_ASSERT_EQUAL_size_t(13, IFACE_AUDIO_CTC0_CARRY_MAX);
}

/** @brief Deterministický generátor pro náhodné vstupy (xorshift64). */
static uint64_t xs64(uint64_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 7;
    *st ^= *st << 17;
    return *st;
}

/** @brief Dřívější zápis iface_audio_block_point() (04ee7d22): `ceil` přičtením `den - 1`. */
static uint64_t old_block_point(int64_t end_edge, uint64_t span_ticks, uint64_t rate, uint64_t frame_ticks)
{
    const uint64_t den = rate * frame_ticks;
    uint64_t e = (end_edge > 0) ? (uint64_t)end_edge : 1u;
    if (e > den)
        e = den;
    const uint64_t p = (e * span_ticks + den - 1u) / den;
    return (p > 0u) ? (p - 1u) : 0u;
}

/**
 * @brief Bod vzorku PSG na mezi kontraktu a shoda s dřívějším zápisem.
 *
 * Pro `span_ticks = IFACE_AUDIO_SPAN_TICKS_MAX` (pro dané `rate * frame_ticks`)
 * a koncovou hranu snímku musí vyjít `span_ticks - 1` (dřívější zápis
 * přetekl a vracel 0: PAL 1 180 179 882, NTSC 1 750 592 256). Obecně se
 * výsledek srovná s `ceil(e * span / den) - 1` ve 128 bitech a všude, kde
 * dřívější zápis nepřetekl, i s ním (bitově stejný výpočet).
 * @param ticks Takty snímku.
 * @param want_max Očekávaný výsledek pro mez (span - 1).
 */
static void check_block_point_limit(uint64_t ticks, uint64_t want_max)
{
    const uint64_t den = RATE * ticks;
    const uint64_t span_max = UINT64_MAX / den;
    TEST_ASSERT_EQUAL_UINT64(want_max + 1u, span_max);
    TEST_ASSERT_EQUAL_UINT64(want_max, iface_audio_block_point((int64_t)den, span_max, RATE, ticks));
    TEST_ASSERT_EQUAL_UINT64(0, iface_audio_block_point(1, span_max, RATE, ticks));
    TEST_ASSERT_EQUAL_UINT64(0, iface_audio_block_point((int64_t)den, 0, RATE, ticks));
    TEST_ASSERT_EQUAL_UINT64(0, iface_audio_block_point(-5, 1, RATE, ticks));
    uint64_t st = 0x243F6A8885A308D3ull ^ ticks;
    unsigned compared_old = 0;
    for (unsigned k = 0; k < 200000; k++) {
        const int64_t e = (int64_t)(xs64(&st) % (den + 2u)) - 1;
        uint64_t span;
        switch (k % 4u) {
            case 0: span = 1u + xs64(&st) % ticks; break;           /* běžný snímek */
            case 1: span = 1u + xs64(&st) % (ticks * 40u); break;   /* vlastní rychlost do 4000 % */
            case 2: span = span_max - xs64(&st) % 1000u; break;     /* u meze */
            default: span = 1u + xs64(&st) % span_max; break;       /* celý rozsah */
        }
        uint64_t ec = (e > 0) ? (uint64_t)e : 1u;
        if (ec > den)
            ec = den;
        const unsigned __int128 x = (unsigned __int128)ec * span;
        const uint64_t want = (uint64_t)((x + den - 1u) / den) - 1u;
        const uint64_t got = iface_audio_block_point(e, span, RATE, ticks);
        if (got != want) {
            printf("block point %llu ticks: e %lld span %llu got %llu want %llu\n", (unsigned long long)ticks,
                   (long long)e, (unsigned long long)span, (unsigned long long)got, (unsigned long long)want);
        }
        TEST_ASSERT_EQUAL_UINT64(want, got);
        if (x + den - 1u <= UINT64_MAX) {
            TEST_ASSERT_EQUAL_UINT64(old_block_point(e, span, RATE, ticks), got);
            compared_old++;
        }
    }
    printf("block point %llu ticks: limit span %llu -> %llu, %u random inputs equal to the former formula\n",
           (unsigned long long)ticks, (unsigned long long)span_max, (unsigned long long)want_max, compared_old);
    TEST_ASSERT_TRUE(compared_old > 100000);
}

/** Bod vzorku PSG pro nejvyšší přípustnou délku logu: PAL. */
void test_block_point_limit_pal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_block_point_limit(PAL_TICKS, 1180179882ull);
    TEST_ASSERT_EQUAL_UINT64(1180179883ull, (uint64_t)IFACE_AUDIO_SPAN_TICKS_MAX);
}

/** Bod vzorku PSG pro nejvyšší přípustnou délku logu: NTSC. */
void test_block_point_limit_ntsc(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_block_point_limit(NTSC_TICKS, 1750592256ull);
}

/**
 * Buňka záporné hrany (iface_audio_block_cell()): `floor(edge * cells / den)`
 * ve 128 bitech pro náhodné hrany i pro `|edge| * cells` na mezi uint64_t.
 */
void test_block_cell_floor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint64_t norms[][2] = { { PAL_TICKS, PAL_TICKS / 32u }, { NTSC_TICKS, NTSC_TICKS / 32u } };
    uint64_t st = 0x13198A2E03707344ull;
    for (unsigned n = 0; n < 2; n++) {
        const uint64_t ticks = norms[n][0], cells = norms[n][1], den = RATE * ticks;
        for (unsigned k = 0; k < 100000; k++) {
            const uint64_t mag = (k & 1u) ? 1u + xs64(&st) % (2u * PAL_HZ) : 1u + xs64(&st) % (UINT64_MAX / cells);
            const int64_t edge = (k & 2u) ? (int64_t)(mag & 0x7FFFFFFFFFFFFFFFull) : -(int64_t)(mag & 0x7FFFFFFFFFFFFFFFull);
            const unsigned __int128 m = (unsigned __int128)(uint64_t)(edge < 0 ? -edge : edge) * cells;
            const int64_t want = (edge >= 0) ? (int64_t)(m / den) : -(int64_t)((m + den - 1u) / den);
            TEST_ASSERT_EQUAL_INT64(want, iface_audio_block_cell(edge, cells, RATE, ticks));
        }
        /* |edge| * cells přesně na mezi uint64_t */
        const int64_t edge = -(int64_t)(UINT64_MAX / cells); /* dřívější zápis přidal den - 1 a přetekl */
        const unsigned __int128 m = (unsigned __int128)(uint64_t)(-edge) * cells;
        TEST_ASSERT_EQUAL_INT64(-(int64_t)((m + den - 1u) / den), iface_audio_block_cell(edge, cells, RATE, ticks));
    }
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_block_clock_pal_no_drift);
    RUN_TEST(test_block_clock_ntsc_no_drift);
    RUN_TEST(test_block_clock_platform);
    RUN_TEST(test_timer_deadlines_pal);
    RUN_TEST(test_timer_deadlines_ntsc);
    RUN_TEST(test_continuous_phase_pal);
    RUN_TEST(test_continuous_phase_ntsc);
    RUN_TEST(test_ctc0_mix_level_continuous);
    RUN_TEST(test_block_point_limit_pal);
    RUN_TEST(test_block_point_limit_ntsc);
    RUN_TEST(test_block_cell_floor);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
