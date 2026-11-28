/**
 * @file   test_audio_psg_average_equiv.c
 * @brief  Převod PSG SDL cesty: zrychlené cesty jsou bitově shodné s dřívější implementací.
 *
 * iface_audio_resampler_psg_average() počítá kanál beze změny v celém
 * logu (`samples_count == 0`, typicky ztlumený kanál; MZ-1500 jich má při
 * jednom hrajícím tónu sedm) bez obecné smyčky přes vzorky. Referenční
 * implementace v tomto testu je doslovná kopie iface_audio_resampler_psg_average()
 * a iface_audio_resampler_process_psg_audio_log() z exact-clock `f9da3035`
 * (iface_audio_resampler.c, včetně iterátoru logu), jen s přejmenovanými
 * symboly. Obě dostanou stejné posloupnosti bloků a porovnají se bitově
 * (memcmp): průměry vzorků, hodnoty na konci intervalů, celý stav přenosu
 * (st_IFACE_AUDIO_PSG_CARRY) po každém bloku a u převodu kanálu i výstup
 * a mezivýstup (`raw_out`). Bitová shoda je správná mez: zrychlená cesta
 * počítá tytéž výrazy v plovoucí čárce ve stejném pořadí.
 *
 * Vstupy: PAL i NTSC, délka snímku logu 100 %, 50 % a 200 %, posun začátku
 * snímku v logu 0 až 79 taktů, držený konec za posledním krokem PSG
 * (valid_end), navazující i nenavazující log; logy beze změny (s různou
 * hodnotou i po bloku se změnami, kdy přenos nese kus jiné hodnoty a
 * pending), jedna událost, řídký a hustý tón, náhodné úseky s úseky nulové
 * délky; výchylky z tabulky různých řádů i náhodné.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "audio.h"
#include "iface/iface_audio.h"
#include "iface/iface_audio_resampler.h"

/** @brief Vzorkovací kmitočet SDL cesty. */
#define RATE 44100ull
/** @brief Nejvíc událostí logu snímku. */
#define EVENTS_MAX 60000u
/** @brief Bloků na scénář. */
#define BLOCKS 400u

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------------- */
/* Reference: doslovná kopie z exact-clock f9da3035 (iface_audio_resampler.c, */
/* iterátor logu, průměr a převod kanálu PSG), symboly přejmenované ref_*.   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Iterátor úseků zvukového logu kanálu (pro integraci výchylky přes čas).
 *
 * Úsek j platí na taktech `[start, end)` logu; poslední úsek (a log bez
 * událostí) trvá do nekonečna (`end` = UINT64_MAX). Posouvá se jen dopředu.
 */
typedef struct st_REF_PSG_LOG_ITER
{
    const st_AUDIO_SOURCE_LOG *src; /**< Log kanálu. */
    size_t index;                   /**< Aktuální úsek. */
    uint64_t start;                 /**< Začátek aktuálního úseku [takty logu]. */
    uint64_t end;                   /**< Konec aktuálního úseku (bez); UINT64_MAX u posledního. */
    uint8_t value;                  /**< Hodnota aktuálního úseku. */
} st_REF_PSG_LOG_ITER;

/**
 * @brief Založí iterátor na prvním úseku logu.
 * @param it  Iterátor (výstup).
 * @param src Log kanálu; musí žít, dokud se iterátor používá.
 */
static inline void ref_psg_iter_init(st_REF_PSG_LOG_ITER *it, const st_AUDIO_SOURCE_LOG *src)
{
    it->src = src;
    it->index = 0;
    it->start = 0;
    if (src->samples_count == 0)
    {
        it->end = UINT64_MAX;
        it->value = (uint8_t)src->last_value;
        return;
    };
    it->value = (uint8_t)src->samples[0].value;
    it->end = (src->samples_count > 1) ? src->samples[0].count_ticks : UINT64_MAX;
}

/**
 * @brief Posune iterátor na úsek, který obsahuje takt `t`.
 * @param it Iterátor.
 * @param t  Takt logu, `t >= it->start` (iterátor jde jen dopředu).
 * @post `it->start <= t < it->end`; úseky nulové délky se přeskočí.
 */
static inline void ref_psg_iter_seek(st_REF_PSG_LOG_ITER *it, uint64_t t)
{
    while (t >= it->end)
    {
        it->index++;
        it->start = it->end;
        it->value = (uint8_t)it->src->samples[it->index].value;
        it->end = (it->index + 1 < it->src->samples_count) ? it->start + it->src->samples[it->index].count_ticks : UINT64_MAX;
    };
}

/**
 * @brief Integrál výchylky přes takty `[a, b)` logu.
 * @param it     Iterátor (posune se na úsek obsahující `b - 1`, u `a == b` na `a`).
 * @param a      Začátek [takty logu].
 * @param b      Konec (bez), `b >= a`.
 * @param volume Výchylky hodnot logu.
 * @param pieces Výstup: počet úseků, přes které interval vede (1 = celý v jednom).
 * @return Součet `výchylka * takty`.
 */
static inline double ref_psg_iter_integrate(st_REF_PSG_LOG_ITER *it, uint64_t a, uint64_t b, const float *volume, unsigned *pieces)
{
    ref_psg_iter_seek(it, a);
    double sum = 0.0;
    uint64_t t = a;
    unsigned k = 1;
    while (it->end < b)
    {
        sum += (double)volume[it->value] * (double)(it->end - t);
        t = it->end;
        ref_psg_iter_seek(it, t);
        k++;
    };
    sum += (double)volume[it->value] * (double)(b - t);
    *pieces = k;
    return sum;
}

/** @brief Reference: iface_audio_resampler_psg_average() z f9da3035 (doslovně). */
static void ref_psg_average(const st_AUDIO_SOURCE_LOG *source, uint64_t frame_offset, uint64_t frame_span,
                                       uint64_t valid_end, const int64_t *ticks, size_t n, bool contiguous,
                                       const float *volume, st_IFACE_AUDIO_PSG_CARRY *carry, float *avg,
                                       uint8_t *end_value)
{
    st_REF_PSG_LOG_ITER it;
    ref_psg_iter_init(&it, source);

    /* Kus prvního vzorku před začátkem snímku: z předchozího bloku, jinak
     * (nová časová osa) výchylkou na začátku snímku. */
    const int64_t lead_len = (ticks[0] < 0) ? -ticks[0] : 0;
    if (!contiguous || !carry->valid)
    {
        ref_psg_iter_seek(&it, frame_offset);
        carry->len = lead_len;
        carry->sum = (double)volume[it.value] * (double)lead_len;
        carry->pending = 0;
    }
    else if (carry->pending > 0)
    {
        /* Konec předchozího snímku za jeho posledním krokem PSG měl předchozí log
         * jen jako drženou hodnotu; tento log ho nese skutečně (začíná tím krokem):
         * takty [frame_offset - pending, frame_offset). */
        const uint64_t p = ((uint64_t)carry->pending < frame_offset) ? (uint64_t)carry->pending : frame_offset;
        unsigned pieces = 0;
        carry->sum += ref_psg_iter_integrate(&it, frame_offset - p, frame_offset, volume, &pieces);
        if ((uint64_t)carry->pending > p)
        {
            /* neočekávaně krátký začátek logu: zbytek hodnotou na jeho začátku */
            st_REF_PSG_LOG_ITER it0;
            ref_psg_iter_init(&it0, source);
            carry->sum += (double)volume[it0.value] * (double)((uint64_t)carry->pending - p);
        };
        carry->pending = 0;
    };
    ref_psg_iter_seek(&it, frame_offset);

    int64_t prev = 0; /* začátek intervalu vzorku od začátku snímku (první vzorek od 0 + carry) */
    for (size_t i = 0; i < n; i++)
    {
        int64_t b = ticks[i + 1];
        if (b < prev)
            b = prev;
        if (b > (int64_t)frame_span)
            b = (int64_t)frame_span;
        const uint64_t la = frame_offset + (uint64_t)prev, lb = frame_offset + (uint64_t)b;
        if (i > 0 && la < it.end && lb <= it.end)
        {
            /* Celý interval v aktuálním úseku logu (iterátor stojí na úseku konce
             * předchozího intervalu): přesně jeho výchylka, bez integrace (stejný
             * výsledek jako obecná cesta níže). */
            avg[i] = volume[it.value];
            if (end_value)
                end_value[i] = it.value;
            prev = b;
            continue;
        };
        unsigned pieces = 0;
        const double sum = ref_psg_iter_integrate(&it, la, lb, volume, &pieces);
        if (i == 0)
        {
            const int64_t len = carry->len + (b - prev);
            avg[0] = (len > 0) ? (float)((carry->sum + sum) / (double)len) : volume[it.value];
        }
        else if (pieces == 1 || b == prev)
        {
            avg[i] = volume[it.value]; /* celý interval v jednom úseku: přesně jeho výchylka */
        }
        else
        {
            avg[i] = (float)(sum / (double)(b - prev));
        };
        if (end_value)
            end_value[i] = it.value;
        prev = b;
    };

    /* Zbytek snímku za poslední hranicí patří prvnímu vzorku dalšího bloku; jeho
     * část za valid_end (za posledním krokem PSG) doplní až další log. */
    const uint64_t tail_a = frame_offset + (uint64_t)prev, tail_b = frame_offset + frame_span;
    uint64_t vend = valid_end;
    if (vend < tail_a)
        vend = tail_a;
    if (vend > tail_b)
        vend = tail_b;
    unsigned pieces = 0;
    carry->sum = ref_psg_iter_integrate(&it, tail_a, vend, volume, &pieces);
    carry->len = (int64_t)frame_span - prev;
    carry->pending = (int64_t)(tail_b - vend);
    carry->valid = true;
}

/** @brief Rozpracovaný interval vzorku na hranici bloku po kanálech PSG SDL cesty (jen vlákno resampleru). */
static st_IFACE_AUDIO_PSG_CARRY s_ref_psg_carry[AUDIO_SRC_CHANNELS_COUNT];

/** @brief Reference: iface_audio_resampler_process_psg_audio_log() z f9da3035 (doslovně). */
static float *ref_process_psg_audio_log(int channel, const st_AUDIO_SOURCE_LOG *source,
                                                   uint64_t frame_offset, uint64_t frame_span, uint64_t valid_end,
                                                   const int64_t *ticks, bool contiguous, float *SN76489_volume_value,
                                                   size_t output_samples_count, size_t parked_samples,
                                                   size_t parked_fade, float *raw_out)
{
    size_t samples_count = output_samples_count;
#if AUDIO_HAVE_OUTPUT_COUPLING
    (void)parked_samples;
    (void)parked_fade;
#endif

    size_t samples_size = samples_count * sizeof(float);
    float *samples = g_malloc0(samples_size);
    if (!samples)
        return NULL;

    /* Průměrná výchylka přes interval vzorku a hodnota na jeho konci (pro parkování). */
    static float s_avg[IFACE_AUDIO_BLOCK_SAMPLES_MAX];
    static uint8_t s_end[IFACE_AUDIO_BLOCK_SAMPLES_MAX];
    if (samples_count > IFACE_AUDIO_BLOCK_SAMPLES_MAX)
        samples_count = IFACE_AUDIO_BLOCK_SAMPLES_MAX;
    ref_psg_average(source, frame_offset, frame_span, valid_end, ticks, samples_count, contiguous,
                                      SN76489_volume_value, &s_ref_psg_carry[channel], s_avg, s_end);

    if (source->samples_count == 0)
    {
        /* kanál beze změny v celém logu: výchylka poslední hodnoty bez parkování a bez IIR (jako dosud) */
        for (size_t i = 0; i < samples_count; i++)
        {
            samples[i] = s_avg[i];
            if (raw_out)
                raw_out[i] = samples[i];
        };
        return samples;
    };

    for (size_t i = 0; i < samples_count; i++)
    {
        float resampled = s_avg[i];

#if !AUDIO_HAVE_OUTPUT_COUPLING
        /* Parkování stálé úrovně jen tam, kde výstup nemá střídavou vazbu;
         * na MZ-800 a MZ-1500 stálou úroveň odstraní vazba výstupu (mix). */
        uint8_t end_value = s_end[i];
        // Pokud je hodnota zaparkovaná na konstantni hodnote, tak postupne snizujeme hodnotu k 0
        static uint8_t parking_previous_resampled[AUDIO_SRC_CHANNELS_COUNT] = {0};
        static size_t parking_counter[AUDIO_SRC_CHANNELS_COUNT] = {0};
        static float parking_gain[AUDIO_SRC_CHANNELS_COUNT] = {0};
        static int parking_gain_counter[AUDIO_SRC_CHANNELS_COUNT] = {0};

        if ((end_value == 0) || (end_value != parking_previous_resampled[channel]))
        {
            parking_previous_resampled[channel] = end_value;
            parking_counter[channel] = 0;
        }
        else
        {
            // Trvale stejnou hodnotu povolíme jen po dobu parked_samples
            if (parking_counter[channel] < parked_samples)
            {
                parking_counter[channel]++;
                parking_gain[channel] = 1.0f;
                parking_gain_counter[channel] = 0;
            }
            else
            {
                // Budeme postupne snizovat hodnotu az k 0 po dobu urcenou parked_fade
                resampled *= parking_gain[channel];
                if (parking_gain[channel] > 0.0f)
                {
                    if (parking_gain_counter[channel]-- == 0)
                    {
                        parking_gain[channel] -= 1.0f / 20;
                        parking_gain_counter[channel] = parked_fade / 20;
                    };
                }
                else
                {
                    parking_gain[channel] = 0.0f;
                };
            };
        };
#endif

        /* vzorek po parkování, před IIR: vstup výstupní propusti */
        if (raw_out)
            raw_out[i] = resampled;

        float iir_x = 6.0f;
        static float sample_value[AUDIO_SRC_CHANNELS_COUNT] = {0}; // posledni hodnota pro IIR filtr
        sample_value[channel] = sample_value[channel] + (resampled - sample_value[channel]) / iir_x;
        samples[i] = sample_value[channel];
    };

    return samples;
}


/* ------------------------------------------------------------------------- */
/* Vstupy a srovnání                                                          */
/* ------------------------------------------------------------------------- */

/** @brief Události logu (jeden log v každém okamžiku). */
static st_AUDIO_LOG_EVENT s_ev[EVENTS_MAX];
/** @brief Výchylky hodnot logu 0 až 31. */
static float s_vol[32];

/** @brief Deterministický generátor (xorshift64). */
static uint64_t rnd(uint64_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 7;
    *st ^= *st << 17;
    return *st;
}

/**
 * @brief Log kanálu pro jeden blok.
 * @param kind  0 = beze změny (`samples_count` 0, hodnota náhodná, i 0),
 *              1 = jedna událost, 2 = řídký obdélník, 3 = husté náhodné
 *              úseky včetně úseků nulové délky.
 * @param total Délka logu [takty] (posun začátku snímku + délka snímku).
 * @param st    Stav generátoru.
 * @return Log (u kind 0 bez pole událostí, jinak ukazuje do s_ev).
 */
static st_AUDIO_SOURCE_LOG make_log(int kind, uint64_t total, uint64_t *st)
{
    st_AUDIO_SOURCE_LOG log;
    log.last_timestamp = total;
    const AUDIO_BUF_t v = (AUDIO_BUF_t) ((rnd(st) % 3u == 0u) ? 0u : rnd(st) % 31u);
    if (kind == 0) {
        log.samples = NULL;
        log.samples_count = 0;
        log.last_value = v;
        return log;
    }
    unsigned n = 0;
    uint64_t t = 0;
    const uint64_t half = 300u + rnd(st) % 2700u;
    while (t < total && n < EVENTS_MAX) {
        uint64_t run;
        if (kind == 1) run = total;
        else if (kind == 2) run = half;
        else run = rnd(st) % 41u;
        s_ev[n].count_ticks = (uint32_t) run;
        s_ev[n].value = (kind == 2) ? ((n & 1u) ? 0 : v) : (kind == 1) ? v : (AUDIO_BUF_t) (rnd(st) % 31u);
        t += run;
        n++;
    }
    log.samples = s_ev;
    log.samples_count = n;
    log.last_value = s_ev[n - 1].value;
    return log;
}

/** @brief Dvě kopie stavu přenosu jsou bitově stejné. */
static bool carry_equal(const st_IFACE_AUDIO_PSG_CARRY *a, const st_IFACE_AUDIO_PSG_CARRY *b)
{
    return memcmp(&a->sum, &b->sum, sizeof(a->sum)) == 0 && a->len == b->len && a->pending == b->pending
           && a->valid == b->valid;
}

/** @brief Druh logu bloku podle vzoru scénáře. */
static int pick_kind(int pattern, unsigned blk, uint64_t *st)
{
    switch (pattern) {
        case 0: return (int) (rnd(st) % 4u);                      /* náhodně */
        case 1: return (rnd(st) % 5u == 0u) ? 2 + (int) (rnd(st) % 2u) : 0;   /* převážně beze změny */
        case 2: return 0;                                          /* jen beze změny */
        default: return (blk % 2u) ? 0 : 3;                        /* střídavě husté a beze změny */
    }
}

/**
 * @brief Jedna řada bloků: iface_audio_resampler_psg_average() proti referenci.
 * @return Počet porovnaných bloků s logem beze změny.
 */
static unsigned run_average(uint64_t frame_ticks, uint64_t crystal, unsigned pct, int pattern, uint64_t seed)
{
    static int64_t ticks[IFACE_AUDIO_BLOCK_SAMPLES_MAX + 1];
    static float ra[IFACE_AUDIO_BLOCK_SAMPLES_MAX], pa[IFACE_AUDIO_BLOCK_SAMPLES_MAX];
    static uint8_t re[IFACE_AUDIO_BLOCK_SAMPLES_MAX], pe[IFACE_AUDIO_BLOCK_SAMPLES_MAX];
    uint64_t st = seed;
    const uint64_t span = frame_ticks * pct / 100u;
    st_IFACE_AUDIO_BLOCK_CLOCK clk = { 0 };
    st_IFACE_AUDIO_PSG_CARRY rc, pc;
    memset(&rc, 0, sizeof(rc));
    memset(&pc, 0, sizeof(pc));
    unsigned consts = 0;
    for (unsigned blk = 0; blk < BLOCKS; blk++) {
        const uint64_t lead = clk.rem;
        const size_t n = iface_audio_block_clock_step(&clk, RATE, frame_ticks, crystal);
        iface_audio_block_ticks_for(lead, n, span, RATE, frame_ticks, crystal, ticks);
        const uint64_t off = rnd(&st) % 80u;
        const int kind = pick_kind(pattern, blk, &st);
        st_AUDIO_SOURCE_LOG log = make_log(kind, off + span, &st);
        const uint64_t h = rnd(&st) % 80u;
        const uint64_t valid = (rnd(&st) % 4u == 0u) ? UINT64_MAX : off + span - h;
        const bool contiguous = (blk > 0) && (rnd(&st) % 10u != 0u);
        const bool with_end = (rnd(&st) % 4u != 0u);
        memset(ra, 0xA5, sizeof(ra));
        memset(pa, 0xA5, sizeof(pa));
        memset(re, 0x5A, sizeof(re));
        memset(pe, 0x5A, sizeof(pe));
        ref_psg_average(&log, off, span, valid, ticks, n, contiguous, s_vol, &rc, ra, with_end ? re : NULL);
        iface_audio_resampler_psg_average(&log, off, span, valid, ticks, n, contiguous, s_vol, &pc, pa,
                                          with_end ? pe : NULL);
        if (memcmp(ra, pa, sizeof(ra)) != 0 || memcmp(re, pe, sizeof(re)) != 0 || !carry_equal(&rc, &pc)) {
            printf("MISMATCH %llu ticks %u %% pattern %d block %u kind %d n %zu\n", (unsigned long long) frame_ticks,
                   pct, pattern, blk, kind, n);
            TEST_FAIL_MESSAGE("PSG average differs from the f9da3035 reference");
        }
        if (kind == 0) consts++;
    }
    return consts;
}

/**
 * @brief Jedna řada bloků: převod všech kanálů PSG (iface_audio_resampler_process_psg_audio_log()) proti referenci.
 * @return Počet porovnaných vzorků.
 */
static unsigned run_process(uint64_t frame_ticks, uint64_t crystal, unsigned pct, int pattern, uint64_t seed)
{
    static int64_t ticks[IFACE_AUDIO_BLOCK_SAMPLES_MAX + 1];
    static float rraw[IFACE_AUDIO_BLOCK_SAMPLES_MAX], praw[IFACE_AUDIO_BLOCK_SAMPLES_MAX];
    uint64_t st = seed;
    const uint64_t span = frame_ticks * pct / 100u;
    const size_t parked = (RATE / 1000u) * 45u, fade = (RATE / 1000u) * 20u;
    st_IFACE_AUDIO_BLOCK_CLOCK clk = { 0 };
    unsigned compared = 0;
    for (unsigned blk = 0; blk < BLOCKS / 4u; blk++) {
        const uint64_t lead = clk.rem;
        const size_t n = iface_audio_block_clock_step(&clk, RATE, frame_ticks, crystal);
        iface_audio_block_ticks_for(lead, n, span, RATE, frame_ticks, crystal, ticks);
        const uint64_t off = rnd(&st) % 80u;
        const uint64_t valid = off + span - rnd(&st) % 80u;
        const bool contiguous = (blk > 0) && (rnd(&st) % 10u != 0u);
        for (int ch = 1; ch < AUDIO_SRC_CHANNELS_COUNT; ch++) {
            st_AUDIO_SOURCE_LOG log = make_log(pick_kind(pattern, blk + (unsigned) ch, &st), off + span, &st);
            float *r = ref_process_psg_audio_log(ch, &log, off, span, valid, ticks, contiguous, s_vol, n, parked, fade,
                                                 rraw);
            float *p = iface_audio_resampler_process_psg_audio_log(ch, &log, off, span, valid, ticks, contiguous, s_vol, n,
                                                                    parked, fade, praw);
            TEST_ASSERT_NOT_NULL(r);
            TEST_ASSERT_NOT_NULL(p);
            if (memcmp(r, p, n * sizeof(float)) != 0 || memcmp(rraw, praw, n * sizeof(float)) != 0) {
                printf("MISMATCH process %llu ticks %u %% pattern %d block %u channel %d\n",
                       (unsigned long long) frame_ticks, pct, pattern, blk, ch);
                TEST_FAIL_MESSAGE("PSG channel output differs from the f9da3035 reference");
            }
            compared += (unsigned) n;
            g_free(r);
            g_free(p);
        }
    }
    return compared;
}

/** @brief Výchylky: tabulka různých řádů (jako SN76489), nebo náhodné. */
static void set_volumes(int random, uint64_t *st)
{
    for (unsigned v = 0; v < 32; v++) {
        if (!random) s_vol[v] = (v == 0) ? 0.0f : 0.8f / (float) (1u << (v % 8u)) + 0.0013f * (float) v;
        else s_vol[v] = (float) (rnd(st) % 1000003u) / 1000003.0f * 3000.0f;
    }
    s_vol[0] = 0.0f;
}

/** Průměr přes interval vzorku: PAL i NTSC, 100 %, 50 %, 200 %, čtyři vzory logů. */
void test_psg_average_equiv(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint64_t norm[2][2] = { { 354432u, 17734475u }, { 238944u, 14318180u } };
    static const unsigned speeds[] = { 100, 50, 200 };
    unsigned consts = 0, runs = 0;
    uint64_t st = 0x243F6A8885A308D3ull;
    for (int vr = 0; vr < 2; vr++) {
        set_volumes(vr, &st);
        for (unsigned k = 0; k < 2; k++)
            for (unsigned s = 0; s < 3; s++)
                for (int pattern = 0; pattern < 4; pattern++) {
                    consts += run_average(norm[k][0], norm[k][1], speeds[s], pattern, rnd(&st) | 1u);
                    runs++;
                }
    }
    printf("PSG average equal to f9da3035: %u series of %u blocks, %u blocks without change\n", runs, BLOCKS, consts);
    TEST_ASSERT_TRUE(consts > 1000u);
}

/** Převod kanálů PSG (průměr, parkování podle platformy, IIR, raw_out) proti referenci. */
void test_psg_process_equiv(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint64_t norm[2][2] = { { 354432u, 17734475u }, { 238944u, 14318180u } };
    static const unsigned speeds[] = { 100, 50, 200 };
    unsigned total = 0;
    uint64_t st = 0x13198A2E03707344ull;
    set_volumes(0, &st);
    for (unsigned k = 0; k < 2; k++)
        for (unsigned s = 0; s < 3; s++)
            for (int pattern = 0; pattern < 4; pattern++) total += run_process(norm[k][0], norm[k][1], speeds[s], pattern, rnd(&st) | 1u);
    printf("PSG channel output equal to f9da3035: %u samples\n", total);
    TEST_ASSERT_TRUE(total > 100000u);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_psg_average_equiv);
    RUN_TEST(test_psg_process_equiv);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
