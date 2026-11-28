/**
 * @file   test_audio_ctc0_resample_equiv.c
 * @brief  Převod CTC0 SDL cesty: optimalizovaná smyčka je bitově shodná s vážením každé buňky.
 *
 * iface_audio_resampler_ctc0_run() počítá vnitřní buňky intervalu vzorku
 * jen čítači (běžné a AUDIO_LEVEL_CTC0_DIV2 zvlášť) a krajní buňky váží
 * jednotlivě. Referenční implementace v tomto testu je doslovná kopie
 * dřívější smyčky (exact-clock `04ee7d22`, iface_audio_resampler.c), která
 * vážila každou buňku zvlášť; liší se jen tím, že stav a konstanty normy
 * dostává parametrem. Obě dostanou stejné posloupnosti bloků a výstup
 * i mezivýstup před low-passem (`raw_out`) se porovnají bitově (memcmp).
 * Bitová shoda je správná mez: celočíselné součty vah nezávisí na pořadí
 * sčítání a výrazy v plovoucí čárce jsou v obou cestách stejné.
 *
 * Vstupy (každý jako souvislá řada bloků, PAL i NTSC): ticho, tóny různých
 * period, DIV2 (stálá, klíčovaná a smíšená s plnou výchylkou), klíčování
 * tónu, náhodné hodnoty buněk, změny přesně na přelomu bloku, přenesené
 * buňky (navazující log), nespojitý log (contiguous = false i nesouhlasný
 * počet přenesených buněk), buňky ze zvukového logu při vlastní rychlosti
 * (délka snímku logu 50 % a 200 %) a ořezávané hranice (hrana mimo snímek,
 * klesající hrana).
 *
 * Část "public" porovnává přímo produkční iface_audio_resampler_output_stream_ctc0()
 * (statický stav modulu, konstanty sestavení MZ-800) - ta existuje i ve
 * starších revizích, takže tentýž test jde přeložit i proti nim s
 * -DCTC0_EQUIV_PUBLIC_ONLY (bez volání iface_audio_resampler_ctc0_run());
 * reference pak musí souhlasit i s původní smyčkou.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "iface/iface_audio.h"
#include "iface/iface_audio_resampler.h"
#include "videorec/videorec_audio.h"

/** @brief Vzorkovací kmitočet SDL cesty. */
#define RATE 44100ull
/** @brief Nejvíc buněk snímku (PAL 11 076). */
#define CELLS_MAX 11076u
/** @brief Nejvíc vzorků bloku (PAL 882). */
#define BLOCK_MAX 882u
/** @brief Kapacita přenosu v referenci. */
#define REF_CARRY_LIMIT 16u

/** @brief Konstanty normy. */
typedef struct
{
    const char *name;     /**< Název do výpisu. */
    uint64_t crystal;     /**< Takty za skutečnou sekundu. */
    uint64_t ticks;       /**< Takty snímku. */
    uint64_t cells;       /**< Buňky CTC0 na snímek (takty / 32). */
} st_NORM;

/** @brief PAL (MZ-800, MZ-700 PAL). */
static const st_NORM NORM_PAL = { "PAL", 17734475ull, 354432ull, 354432ull / 32u };
/** @brief NTSC (MZ-1500, MZ-700 NTSC). */
static const st_NORM NORM_NTSC = { "NTSC", 14318180ull, 238944ull, 238944ull / 32u };

/** @brief Stav referenční implementace (statické proměnné původní smyčky). */
typedef struct
{
    st_VIDEOREC_AUDIO_CHSTATE state; /**< s_ctc0_state. */
    st_VIDEOREC_AUDIO_COEF coef;     /**< s_ctc0_coef. */
    bool coef_ready;                 /**< s_ctc0_coef_ready. */
    uint8_t carry[REF_CARRY_LIMIT];  /**< s_ctc0_carry. */
    size_t carry_count;              /**< s_ctc0_carry_count. */
} st_REF;

/** @brief Buňka polohy, kopie ctc0_cell_of() z 04ee7d22 s jednotkou parametrem. */
static inline int64_t ref_cell_of(int64_t pos, int64_t d)
{
    return (pos >= 0) ? (pos / d) : -((-pos + d - 1) / d);
}

/**
 * @brief Doslovná kopie iface_audio_resampler_output_stream_ctc0() z 04ee7d22.
 *
 * Jediné rozdíly: stav v `s`, jednotka buňky `d` a mez přenosu `carry_max`
 * parametrem, výstup do pole volajícího.
 */
static void ref_output_stream_ctc0(st_REF *s, const uint8_t *cells, size_t cells_count, const int64_t *edge_pos,
                                   size_t output_samples_count, bool contiguous, int64_t d, size_t carry_max,
                                   float *output_samples, float *raw_out)
{
    if (!s->coef_ready)
    {
        videorec_audio_coef_init(&s->coef, IFACE_AUDIO_SAMPLE_RATE);
        s->coef_ready = true;
    };

    const int64_t lo = -(int64_t)carry_max * d;
    const int64_t hi = (int64_t)cells_count * d;

    int64_t a = edge_pos[0];
    if (a > 0)
        a = 0;
    if (a < lo)
        a = lo;
    const size_t need = (size_t)(-ref_cell_of(a, d));
    if (!contiguous || (need != s->carry_count))
    {
        memset(s->carry, cells[0], need);
        s->carry_count = need;
    };

    for (size_t i = 0; i < output_samples_count; i++)
    {
        int64_t b = edge_pos[i + 1];
        if (b < a)
            b = a;
        if (b > hi)
            b = hi;
        const int64_t ja = ref_cell_of(a, d);
        const int64_t jb = ref_cell_of(b, d);
        const int64_t fa = a - ja * d;
        const int64_t fb = b - jb * d;

        int64_t ones_w = 0;
        int64_t div2_w = 0;
        for (int64_t j = ja; j <= jb; j++)
        {
            int64_t w = d;
            if (j == ja)
                w -= fa;
            if (j == jb)
                w -= d - fb;
            if (w <= 0 || j >= (int64_t)cells_count)
                continue;
            const uint8_t v = (j < 0) ? s->carry[(int64_t)s->carry_count + j] : cells[j];
            if (v)
            {
                if (v == AUDIO_LEVEL_CTC0_DIV2)
                    div2_w += w;
                else
                    ones_w += w;
            };
        };

        const double div = (double)(b - a) + (double)d;
        float resampled = (float)(((double)ones_w + (double)AUDIO_CTC0_DIV2_FACTOR * (double)div2_w) / div);
        float unit = (float)((double)(b - a) / div);

        float raw = 0.0f;
        output_samples[i] = videorec_audio_chain_ctc0(&s->state, &s->coef, resampled, unit, &raw);
        if (raw_out)
            raw_out[i] = raw;
        a = b;
    };

    int64_t start = ref_cell_of(a, d);
    if (start < 0)
        start = 0;
    size_t tail = cells_count - (size_t)start;
    if (tail > carry_max)
        tail = carry_max;
    memcpy(s->carry, cells + (cells_count - tail), tail);
    s->carry_count = tail;
}

/** @brief Deterministický generátor (xorshift64). */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;

/** @brief Další pseudonáhodné číslo. */
static uint64_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

/** @brief Druhy vstupu (scénáře). */
typedef enum
{
    SC_SILENCE = 0,  /**< Ticho. */
    SC_TONE,         /**< Obdélník 0 / 15 s periodou `p` buněk. */
    SC_DIV2,         /**< Stálá úroveň DIV2. */
    SC_DIV2_KEYED,   /**< DIV2 klíčovaná s periodou `p`. */
    SC_MIXED,        /**< Střídání 15 a 31 s periodou `p`. */
    SC_KEYED_TONE,   /**< Tón klíčovaný po blocích (každý třetí blok ticho). */
    SC_RANDOM,       /**< Náhodné hodnoty 0, 15, 31, 7, 20. */
    SC_RANDOM_RUNS,  /**< Náhodné úseky stálé hodnoty. */
    SC_EDGE,         /**< Změna přesně na prvních / posledních buňkách bloku. */
    SC_LOG_SPEED,    /**< Buňky ze zvukového logu při vlastní rychlosti (iface_audio_resampler_ctc0_cells()). */
    SC_COUNT         /**< Počet scénářů. */
} en_SC;

/**
 * @brief Naplní buňky bloku `blk` scénáře.
 * @param sc    Scénář.
 * @param p     Perioda (buňky) nebo parametr scénáře.
 * @param blk   Pořadí bloku ve scénáři.
 * @param cells Výstup.
 * @param n     Počet buněk.
 * @param ticks Takty snímku normy (SC_LOG_SPEED).
 */
static void fill_cells(en_SC sc, unsigned p, unsigned blk, uint8_t *cells, size_t n, uint64_t ticks)
{
    const uint64_t base = (uint64_t)blk * n;
    switch (sc)
    {
        case SC_SILENCE:
            memset(cells, 0, n);
            break;
        case SC_TONE:
            for (size_t j = 0; j < n; j++)
                cells[j] = (((base + j) / p) & 1u) ? 15 : 0;
            break;
        case SC_DIV2:
            memset(cells, AUDIO_LEVEL_CTC0_DIV2, n);
            break;
        case SC_DIV2_KEYED:
            for (size_t j = 0; j < n; j++)
                cells[j] = (((base + j) / p) & 1u) ? AUDIO_LEVEL_CTC0_DIV2 : 0;
            break;
        case SC_MIXED:
            for (size_t j = 0; j < n; j++)
                cells[j] = (((base + j) / p) % 3u == 0) ? AUDIO_LEVEL_CTC0_DIV2 : ((((base + j) / p) % 3u == 1) ? 15 : 0);
            break;
        case SC_KEYED_TONE:
            for (size_t j = 0; j < n; j++)
                cells[j] = ((blk % 3u) != 2u && (((base + j) / p) & 1u)) ? 15 : 0;
            break;
        case SC_RANDOM:
        {
            static const uint8_t vals[] = { 0, 15, AUDIO_LEVEL_CTC0_DIV2, 7, 20 };
            for (size_t j = 0; j < n; j++)
                cells[j] = vals[rnd() % G_N_ELEMENTS(vals)];
            break;
        }
        case SC_RANDOM_RUNS:
        {
            static const uint8_t vals[] = { 0, 15, AUDIO_LEVEL_CTC0_DIV2 };
            size_t j = 0;
            while (j < n)
            {
                size_t run = 1 + (size_t)(rnd() % (p ? p : 1));
                uint8_t v = vals[rnd() % G_N_ELEMENTS(vals)];
                for (size_t k = 0; k < run && j < n; k++)
                    cells[j++] = v;
            };
            break;
        }
        case SC_EDGE:
            /* Změny na krajních buňkách: první p buněk jedna hodnota, posledních p jiná. */
            memset(cells, (blk & 1u) ? 15 : 0, n);
            for (size_t j = 0; j < p && j < n; j++)
                cells[j] = (blk & 2u) ? AUDIO_LEVEL_CTC0_DIV2 : 15;
            for (size_t j = 0; j < p && j < n; j++)
                cells[n - 1 - j] = (blk & 1u) ? 0 : 15;
            break;
        case SC_LOG_SPEED:
        {
            /* Log se změnami po `p` taktech; délka snímku logu 50 % nebo 200 % snímku. */
            const uint64_t span = (blk & 1u) ? ticks * 2u : ticks / 2u;
            static st_AUDIO_LOG_EVENT ev[4096];
            unsigned cnt = 0;
            uint64_t t = 0;
            while (t < span && cnt < G_N_ELEMENTS(ev))
            {
                uint32_t len = (uint32_t)(p + rnd() % (p + 1u));
                ev[cnt].count_ticks = len;
                ev[cnt].value = (AUDIO_BUF_t)((cnt & 1u) ? 15 : ((rnd() & 3u) ? 0 : AUDIO_LEVEL_CTC0_DIV2));
                t += len;
                cnt++;
            };
            st_AUDIO_SOURCE_LOG src = { span, ev[cnt - 1].value, ev, cnt };
            iface_audio_resampler_ctc0_cells(&src, 0, span, cells, n);
            break;
        }
        default:
            memset(cells, 0, n);
            break;
    };
}

/** @brief Způsob, jak se pro blok mění hranice a návaznost. */
typedef enum
{
    ED_NORMAL = 0, /**< Hranice z akumulátoru, navazující log. */
    ED_BREAKS,     /**< Každý pátý blok contiguous = false. */
    ED_MISMATCH,   /**< Každý čtvrtý blok náhodný zbytek akumulátoru (nesouhlasný přenos). */
    ED_CLAMP,      /**< Ořezávané hranice: hrana za koncem snímku, klesající, první mimo meze. */
    ED_COUNT       /**< Počet způsobů. */
} en_ED;

/** @brief Výsledek porovnání. */
typedef struct
{
    unsigned blocks;     /**< Porovnané bloky. */
    unsigned samples;    /**< Porovnané vzorky. */
    unsigned mismatches; /**< Bloky s rozdílem. */
} st_CMP;

/** @brief Spočte mez přenosu normy (IFACE_AUDIO_CTC0_CARRY_MAX pro dané konstanty). */
static size_t norm_carry_max(const st_NORM *nm)
{
    const uint64_t den = RATE * nm->ticks;
    return (size_t)((nm->crystal * nm->cells + den - 1u) / den);
}

/**
 * @brief Připraví hranice bloku podle způsobu `ed`.
 * @return Počet vzorků bloku.
 */
static size_t make_edges(const st_NORM *nm, st_IFACE_AUDIO_BLOCK_CLOCK *clk, en_ED ed, unsigned blk, int64_t *edges,
                         bool *contiguous)
{
    uint64_t lead = clk->rem;
    if (ed == ED_MISMATCH && (blk % 4u) == 3u)
    {
        clk->rem = rnd() % nm->crystal;
        lead = clk->rem;
    };
    size_t n = iface_audio_block_clock_step(clk, RATE, nm->ticks, nm->crystal);
    iface_audio_block_timing_for(lead, n, 0, nm->ticks, RATE, nm->ticks, nm->crystal, nm->cells, NULL, edges);
    *contiguous = !(ed == ED_BREAKS && (blk % 5u) == 4u);
    if (ed == ED_CLAMP)
    {
        const int64_t d = (int64_t)(RATE * nm->ticks);
        switch (blk % 4u)
        {
            case 0:
                edges[n] += 5 * d; /* poslední hrana za koncem snímku */
                edges[n - 1] += 3 * d;
                break;
            case 1:
                edges[n / 2] = edges[n / 2 - 1] - 7; /* klesající hrana */
                break;
            case 2:
                edges[0] = 3 * d + 11; /* první hrana za začátkem snímku */
                break;
            default:
                edges[0] = -40 * d; /* první hrana před mezí přenosu */
                break;
        };
    };
    return n;
}

/**
 * @brief Porovná referenci s produkční funkcí na všech scénářích jedné normy.
 * @param nm      Norma.
 * @param use_run true = iface_audio_resampler_ctc0_run() s vlastním stavem, false = veřejná
 *                iface_audio_resampler_output_stream_ctc0() (jen konstanty sestavení).
 * @param res     Výsledek (přičítá se).
 */
static void compare_norm(const st_NORM *nm, bool use_run, st_CMP *res)
{
    static uint8_t cells[CELLS_MAX];
    static int64_t edges[BLOCK_MAX + 1];
    static float out_ref[BLOCK_MAX], raw_ref[BLOCK_MAX], out_new[BLOCK_MAX], raw_new[BLOCK_MAX];
    static const unsigned periods[] = { 1, 2, 3, 7, 13, 32, 100, 1000 };
    const int64_t d = (int64_t)(RATE * nm->ticks);
    const size_t carry_max = norm_carry_max(nm);
    const size_t ncells = (size_t)nm->cells;

    st_REF ref;
    memset(&ref, 0, sizeof(ref));
#ifndef CTC0_EQUIV_PUBLIC_ONLY
    st_IFACE_AUDIO_CTC0_RESAMPLER rs;
    memset(&rs, 0, sizeof(rs));
#endif
    st_IFACE_AUDIO_BLOCK_CLOCK clk = { 0 };

    for (unsigned sc = 0; sc < SC_COUNT; sc++)
    {
        for (unsigned pi = 0; pi < G_N_ELEMENTS(periods); pi++)
        {
            if ((sc == SC_SILENCE || sc == SC_DIV2) && pi > 0)
                continue;
            for (unsigned ed = 0; ed < ED_COUNT; ed++)
            {
                /* střídavá vazba z různých počátečních stavů */
                const double seed = (double)((sc * 7u + pi * 3u + ed) % 11u) / 10.0;
                ref.state.hp_mean = seed;
#ifndef CTC0_EQUIV_PUBLIC_ONLY
                if (use_run)
                    iface_audio_resampler_ctc0_state_seed(&rs, seed);
                else
#endif
                    iface_audio_resampler_ctc0_ac_seed(seed);
                for (unsigned blk = 0; blk < 12; blk++)
                {
                    bool contiguous = true;
                    const size_t n = make_edges(nm, &clk, (en_ED)ed, blk, edges, &contiguous);
                    fill_cells((en_SC)sc, periods[pi], blk, cells, ncells, nm->ticks);
                    if (blk == 0)
                        contiguous = false;
                    ref_output_stream_ctc0(&ref, cells, ncells, edges, n, contiguous, d, carry_max, out_ref, raw_ref);
#ifndef CTC0_EQUIV_PUBLIC_ONLY
                    if (use_run)
                    {
                        iface_audio_resampler_ctc0_run(&rs, cells, ncells, edges, n, contiguous, d, carry_max, out_new,
                                                       raw_new);
                    }
                    else
#endif
                    {
                        float *o = iface_audio_resampler_output_stream_ctc0(cells, ncells, edges, n, contiguous, raw_new);
                        TEST_ASSERT_NOT_NULL(o);
                        memcpy(out_new, o, n * sizeof(float));
                        g_free(o);
                    };
                    res->blocks++;
                    res->samples += (unsigned)n;
                    if (memcmp(out_ref, out_new, n * sizeof(float)) != 0 || memcmp(raw_ref, raw_new, n * sizeof(float)) != 0)
                    {
                        if (res->mismatches < 5)
                        {
                            size_t k = 0;
                            while (k < n && out_ref[k] == out_new[k] && raw_ref[k] == raw_new[k])
                                k++;
                            printf("  MISMATCH %s scenario %u period %u edges %u block %u sample %zu: "
                                   "ref %.9g / %.9g, new %.9g / %.9g\n",
                                   nm->name, sc, periods[pi], ed, blk, k, (double)out_ref[k], (double)raw_ref[k],
                                   (double)out_new[k], (double)raw_new[k]);
                        };
                        res->mismatches++;
                    };
                };
            };
        };
    };
}

void setUp(void) {}
void tearDown(void) {}

/** @brief Veřejná funkce sestavení (MZ-800, PAL) proti referenci. */
void test_public_pal_equal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_UINT64(NORM_PAL.ticks, (uint64_t)VIDEO_SCREEN_TICKS);
    TEST_ASSERT_EQUAL_UINT64(NORM_PAL.crystal, (uint64_t)GDGCLK_REAL_TICKS_PER_SEC);
    TEST_ASSERT_EQUAL_UINT64(NORM_PAL.cells, (uint64_t)IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME);
    TEST_ASSERT_EQUAL_size_t(IFACE_AUDIO_CTC0_CARRY_MAX, norm_carry_max(&NORM_PAL));
    st_CMP res = { 0 };
    compare_norm(&NORM_PAL, false, &res);
    printf("public PAL: %u blocks, %u samples, %u differing blocks\n", res.blocks, res.samples, res.mismatches);
    TEST_ASSERT_TRUE(res.blocks > 1000);
    TEST_ASSERT_EQUAL_UINT(0, res.mismatches);
}

#ifndef CTC0_EQUIV_PUBLIC_ONLY
/** @brief iface_audio_resampler_ctc0_run() s konstantami PAL proti referenci. */
void test_run_pal_equal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_CMP res = { 0 };
    compare_norm(&NORM_PAL, true, &res);
    printf("run PAL: %u blocks, %u samples, %u differing blocks\n", res.blocks, res.samples, res.mismatches);
    TEST_ASSERT_TRUE(res.blocks > 1000);
    TEST_ASSERT_EQUAL_UINT(0, res.mismatches);
}

/** @brief iface_audio_resampler_ctc0_run() s konstantami NTSC proti referenci. */
void test_run_ntsc_equal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_size_t(11, norm_carry_max(&NORM_NTSC));
    st_CMP res = { 0 };
    compare_norm(&NORM_NTSC, true, &res);
    printf("run NTSC: %u blocks, %u samples, %u differing blocks\n", res.blocks, res.samples, res.mismatches);
    TEST_ASSERT_TRUE(res.blocks > 1000);
    TEST_ASSERT_EQUAL_UINT(0, res.mismatches);
}
#endif

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_public_pal_equal);
#ifndef CTC0_EQUIV_PUBLIC_ONLY
    RUN_TEST(test_run_pal_equal);
    RUN_TEST(test_run_ntsc_equal);
#endif
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
