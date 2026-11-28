/**
 * @file   iface_audio_resampler.c
 * @brief  Převzorkování zvukového logu snímku na bloky SDL cesty (44 100 Hz).
 *
 * CTC0 se nejdřív rozdělí na buňky mezilehlého proudu
 * (iface_audio_resampler_ctc0_cells()) a vzorek průměruje buňky svého
 * přesného intervalu (krajní podílem překryvu); PSG průměruje výchylku
 * kanálu přes interval vzorku v celých taktech logu
 * (iface_audio_resampler_psg_average()). Intervaly vzorků
 * leží na souvislé časové ose (iface_audio_block_timing(),
 * iface_audio_block_ticks() v iface_audio.h); buňky CTC0 a kus intervalu
 * PSG přesahující hranici bloku modul přenáší do dalšího bloku.
 * Stavy filtrů a přenesené buňky jsou statické: modul používá jen vlákno,
 * které vyrábí bloky (iface_audio_wait_for_data()).
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#define M_PI_2 (M_PI / 2.0)
#endif

#include "iface_audio.h"
#include "iface_audio_resampler.h"
#include "audio.h"
#include "emulator.h"
#include "videorec/videorec_audio.h"

/* Doxygen viz iface_audio_resampler.h. */
void iface_audio_resampler_ctc0_cells(const st_AUDIO_SOURCE_LOG *source, uint64_t offset, uint64_t span, uint8_t *cells, size_t cells_count)
{
    if (source->samples_count == 0)
    {
        memset(cells, source->last_value, cells_count);
        return;
    };

    /* t_j = offset + floor(j * span / cells_count): celá část a zbytek kroku (bez dělení ve smyčce) */
    const uint64_t step = span / cells_count;
    const uint64_t step_rem = span % cells_count;
    uint64_t current_time = offset;
    uint64_t acc = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;

    for (size_t i = 0; i < cells_count; i++)
    {
        /* Posun na událost platnou v current_time. Podmínka `current_time + 1
         * >= total_time` je původní `current_time >= total_time - 1` bez
         * podtečení: událost přesně na začátku snímku má count_ticks == 0 a
         * `total_time - 1` by přetekl na maximum (kanál by celý snímek držel
         * starou hodnotu). Index nepřekročí poslední událost. */
        while ((current_time + 1 >= total_time) && (event_index + 1 < source->samples_count))
        {
            event_index++;
            total_time += source->samples[event_index].count_ticks;
        };

        cells[i] = source->samples[event_index].value;

        current_time += step;
        acc += step_rem;
        if (acc >= cells_count)
        {
            acc -= cells_count;
            current_time++;
        };
    };
}

/**
 * Jednoduchý IIR Low-pass filtr (jednopólový) s přechodovou frekvencí cca 4 kHz
 * Simuluje analogový filtr ze Sharp MZ-800
 *
 * @param input  Vstupní vzorek (převzorkovaný signál)
 * @return       Filtrovaný vzorek
 */
static inline float iface_audio_lowpass_filter(int channel, float input)
{
    static float prev_output[AUDIO_SRC_CHANNELS_COUNT] = {0};
    float alpha = 0.4f; // Řízení útlumu vyšších frekvencí (čím menší, tím silnější filtr a vice útlumu na vyšších frekvencích)

    prev_output[channel] = alpha * input + (1.0f - alpha) * prev_output[channel];
    return prev_output[channel];
}

/**
 * Dvoupólový IIR Low-pass filtr (lepší potlačení vysokých frekvencí)
 * Simuluje analogový filtr ze Sharp MZ-800
 *
 * @param input  Vstupní vzorek (převzorkovaný signál)
 * @return       Filtrovaný vzorek
 */
static inline float iface_audio_lowpass_filter_2nd_order(int channel, float input)
{
    static float prev_output_1[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Minulý výstup
    static float prev_output_2[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Předminulý výstup

    float alpha = 0.1f; // Určuje agresivitu filtru (nižší = silnější filtr)
    float beta = 0.9f;  // Určuje vliv starších vzorků (vyšší = silnější útlum vyšších frekvencí)

    float output = alpha * input + (1.0f - alpha) * prev_output_1[channel] - beta * prev_output_2[channel];

    // Posuneme historii vzorků
    prev_output_2[channel] = prev_output_1[channel];
    prev_output_1[channel] = output;

    return output;
}

/**
 * Dvoupólový IIR Low-pass filtr (Butterworth, stabilní)
 * @param input  Vstupní vzorek
 * @return       Filtrovaný vzorek
 */
static inline float iface_audio_lowpass_filter_butterworth(int channel, float input)
{
    static float x1[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Minulé vstupy
    static float x2[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Minulé vstupy
    static float y1[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Minulé výstupy
    static float y2[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Minulé výstupy

    // Koeficienty pro 2. řád Butterworthova filtru, fc = 4kHz, fs = 44.1kHz
    // const float b0 = 0.206572083826147;
    // const float b1 = 0.413144167652294;
    // const float b2 = 0.206572083826147;
    // const float a1 = -0.369527377351241;
    // const float a2 = 0.195815712655833;

    // Nové koeficienty pro 6 kHz Butterworth filtr (vypočteno v Pythonu)
    const float b0 = 0.11205521f;
    const float b1 = 0.22411041f;
    const float b2 = 0.11205521f;
    const float a1 = -0.8559895f;
    const float a2 = 0.30421033f;

    // Výpočet filtru (standardní rovnice IIR filtru)
    float output = b0 * input + b1 * x1[channel] + b2 * x2[channel] - a1 * y1[channel] - a2 * y2[channel];

    // Posunutí historie vzorků
    x2[channel] = x1[channel];
    x1[channel] = input;
    y2[channel] = y1[channel];
    y1[channel] = output;

    return output;
}

/**
 * Anti-glitch filtr pro eliminaci lupání
 * Omezuje skokové změny mezi vzorky
 *
 * @param input  Nová hodnota
 * @return       Hladší hodnota bez lupání
 */
static inline float iface_audio_anti_glitch_filter(int channel, float input)
{
    static float prev_value[AUDIO_SRC_CHANNELS_COUNT] = {0};
    float smooth_factor = 0.2f; // Čím menší, tím pomalejší změna (pohlcuje lupání)

    float output = prev_value[channel] + smooth_factor * (input - prev_value[channel]);
    prev_value[channel] = output;
    return output;
}

static inline float iface_audio_anti_glitch_limiter(int channel, float input)
{
    static float prev_value[AUDIO_SRC_CHANNELS_COUNT] = {0};
    float max_delta = 0.2f; // Maximální povolený skok mezi vzorky

    float delta = input - prev_value[channel];
    if (delta > max_delta)
        delta = max_delta;
    if (delta < -max_delta)
        delta = -max_delta;

    float output = prev_value[channel] + delta;
    prev_value[channel] = output;
    return output;
}

G_STATIC_ASSERT(IFACE_AUDIO_CTC0_CARRY_MAX <= IFACE_AUDIO_CTC0_CARRY_LIMIT);

/** @brief Stav převodu CTC0 SDL cesty. Jen vlákno, které volá resampler; mimo emulaci. */
static st_IFACE_AUDIO_CTC0_RESAMPLER s_ctc0;

/* Doxygen viz iface_audio_resampler.h. */
void iface_audio_resampler_ctc0_state_seed(st_IFACE_AUDIO_CTC0_RESAMPLER *rs, double mean01)
{
    if (mean01 < 0.0)
        mean01 = 0.0;
    if (mean01 > 1.0)
        mean01 = 1.0;
    rs->chain.hp_mean = mean01;
}

/* Doxygen viz iface_audio_resampler.h. */
void iface_audio_resampler_ctc0_ac_seed(double mean01)
{
    iface_audio_resampler_ctc0_state_seed(&s_ctc0, mean01);
}

/** @brief Jednotek polohy na buňku proudu CTC0 této platformy (iface_audio_block_cell_pos()). */
#define CTC0_CELL_UNITS ((int64_t)IFACE_AUDIO_SAMPLE_RATE * (int64_t)VIDEO_SCREEN_TICKS)

/**
 * @brief Buňka polohy (zaokrouhleno dolů, i pro zápornou polohu).
 * @param pos Poloha v jednotkách 1 / `d` buňky.
 * @param d   Jednotek na buňku, > 0.
 * @return `floor(pos / d)`.
 * @note Pro zápornou polohu `-pos + d - 1` nepřeteče: poloha je nejvýš
 *       IFACE_AUDIO_CTC0_CARRY_LIMIT buněk před začátkem snímku.
 */
static inline int64_t ctc0_cell_of(int64_t pos, int64_t d)
{
    return (pos >= 0) ? (pos / d) : -((-pos + d - 1) / d);
}

/**
 * @brief Přičte váhu buňky k součtu podle její hodnoty.
 *
 * Hodnota 0 nepřispívá, AUDIO_LEVEL_CTC0_DIV2 jde do `div2_w`, každá jiná
 * nenulová hodnota do `ones_w`.
 *
 * @param v      Hodnota buňky.
 * @param w      Váha (počet jednotek polohy), >= 0.
 * @param ones_w Součet vah buněk s plnou výchylkou.
 * @param div2_w Součet vah buněk AUDIO_LEVEL_CTC0_DIV2.
 */
static inline void ctc0_add_cell(uint8_t v, int64_t w, int64_t *ones_w, int64_t *div2_w)
{
    if (v)
    {
        if (v == AUDIO_LEVEL_CTC0_DIV2)
            *div2_w += w;
        else
            *ones_w += w;
    };
}

/** @brief Bajt 01h ve všech bajtech slova. */
#define CTC0_SWAR_ONES 0x0101010101010101ull

/**
 * @brief Počet nenulových bajtů slova (bez tabulky a bez instrukce POPCNT).
 *
 * Bit 7 bajtu `((x & 7Fh) + 7Fh) | x` je 1 právě pro nenulový bajt (součet
 * nepřeteče do sousedního bajtu); jedničky v bitech 0 se sečtou násobením
 * do nejvyššího bajtu.
 *
 * @param x Osm bajtů.
 * @return 0 až 8.
 */
static inline int64_t ctc0_swar_nonzero(uint64_t x)
{
    const uint64_t low7 = 0x7F7F7F7F7F7F7F7Full;
    const uint64_t t = ((((x & low7) + low7) | x) >> 7) & CTC0_SWAR_ONES;
    return (int64_t)((t * CTC0_SWAR_ONES) >> 56);
}

/**
 * @brief Spočítá buňky `[from, to)` s nenulovou hodnotou a s hodnotou AUDIO_LEVEL_CTC0_DIV2.
 *
 * Po osmi bajtech (SWAR): nenulové bajty slova a bajty, které se liší od
 * AUDIO_LEVEL_CTC0_DIV2 (nenulové bajty `x ^ 1F..1Fh`). Zbytek kratší než
 * slovo se na little endian hostiteli načte celým slovem s maskou, pokud
 * slovo leží v poli; jinak (konec pole, jiné pořadí bajtů) po bajtech.
 * Výsledek je přesný počet, nezávislý na způsobu.
 *
 * @param cells       Buňky snímku.
 * @param cells_count Počet buněk (pole se nečte za ním).
 * @param from        První buňka, `from <= to`.
 * @param to          Konec (bez), `to <= cells_count`.
 * @param nonzero     Výstup: přičte počet nenulových buněk.
 * @param div2        Výstup: přičte počet buněk AUDIO_LEVEL_CTC0_DIV2.
 */
static inline void ctc0_count_cells(const uint8_t *cells, size_t cells_count, size_t from, size_t to, int64_t *nonzero,
                                    int64_t *div2)
{
    const uint64_t div2_bytes = (uint64_t)AUDIO_LEVEL_CTC0_DIV2 * CTC0_SWAR_ONES;
    int64_t nz = 0, d2 = 0;
    size_t j = from;
    while (to - j >= 8u)
    {
        uint64_t x;
        memcpy(&x, cells + j, sizeof(x));
        nz += ctc0_swar_nonzero(x);
        d2 += 8 - ctc0_swar_nonzero(x ^ div2_bytes);
        j += 8u;
    };
    const size_t rest = to - j;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if (rest > 0u && j + 8u <= cells_count)
    {
        /* platné jsou nejnižší `rest` bajty (nejnižší adresy) */
        const uint64_t mask = (~0ull) >> (8u * (8u - rest));
        uint64_t x;
        memcpy(&x, cells + j, sizeof(x));
        nz += ctc0_swar_nonzero(x & mask);
        d2 += (int64_t)rest - ctc0_swar_nonzero((x ^ div2_bytes) & mask);
        j = to;
    };
#else
    (void)cells_count;
#endif
    for (; j < to; j++)
    {
        const uint8_t v = cells[j];
        nz += (v != 0);
        d2 += (v == AUDIO_LEVEL_CTC0_DIV2);
    };
    *nonzero += nz;
    *div2 += d2;
}

/* Doxygen viz iface_audio_resampler.h. */
void iface_audio_resampler_ctc0_run(st_IFACE_AUDIO_CTC0_RESAMPLER *rs, const uint8_t *cells, size_t cells_count,
                                    const int64_t *edge_pos, size_t n, bool contiguous, int64_t cell_units,
                                    size_t carry_max, float *out, float *raw_out)
{
    if (!rs->coef_ready)
    {
        videorec_audio_coef_init(&rs->coef, IFACE_AUDIO_SAMPLE_RATE);
        rs->coef_ready = true;
    };
    if (carry_max > IFACE_AUDIO_CTC0_CARRY_LIMIT)
        carry_max = IFACE_AUDIO_CTC0_CARRY_LIMIT;

    const int64_t d = cell_units;
    const int64_t lo = -(int64_t)carry_max * d; /* nejnižší poloha, kterou přenos pokryje */
    const int64_t hi = (int64_t)cells_count * d; /* konec snímku */

    /* Kolik buněk před začátkem snímku první vzorek potřebuje (podle akumulátoru
     * vždy nejvýš carry_max; meze jen kvůli bezpečnosti). */
    int64_t a = edge_pos[0];
    if (a > 0)
        a = 0;
    if (a < lo)
        a = lo;
    const size_t need = (size_t)(-ctc0_cell_of(a, d));
    if (!contiguous || (need != rs->carry_count))
    {
        /* Nová časová osa: přenesené buňky nahradí stav na začátku snímku. */
        memset(rs->carry, cells[0], need);
        rs->carry_count = need;
    };

    /* Buňka a zbytek levé hrany; délka posledního intervalu s podílem a zbytkem
     * (step = -1: zatím žádná, příští délka se vydělí). */
    int64_t ja = ctc0_cell_of(a, d);
    int64_t fa = a - ja * d;
    int64_t step = -1, step_q = 0, step_r = 0;
    /* Dělitel a zesílení pro naposledy použitou délku intervalu. */
    int64_t unit_len = -1;
    double div = 0.0;
    float unit = 0.0f;

    for (size_t i = 0; i < n; i++)
    {
        int64_t b = edge_pos[i + 1];
        if (b < a)
            b = a;
        if (b > hi)
            b = hi;
        const int64_t len = b - a;

        /* Buňka a zbytek pravé hrany bez dělení, je-li délka stejná jako minule:
         * b = (ja + q) * d + (fa + r), 0 <= fa + r < 2d. */
        int64_t jb, fb;
        if (len == step)
        {
            jb = ja + step_q;
            fb = fa + step_r;
            if (fb >= d)
            {
                fb -= d;
                jb++;
            };
        }
        else
        {
            jb = ctc0_cell_of(b, d);
            fb = b - jb * d;
            step = len;
            step_q = len / d;
            step_r = len % d;
        };

        /* Vážený součet jedniček přes [a, b): vnitřní buňky ja + 1 .. jb - 1 celé
         * (váha d), první bez části fa, buňka jb jen částí fb. Buňky s hodnotou
         * AUDIO_LEVEL_CTC0_DIV2 (režim 3 s děličem 2) zvlášť, mají váhu
         * AUDIO_CTC0_DIV2_FACTOR. */
        int64_t ones_w = 0;
        int64_t div2_w = 0;
        if (ja < 0)
        {
            /* Interval začíná v přenesených buňkách předchozího snímku (jen první
             * vzorek bloku): obecné vážení každé buňky zvlášť. */
            for (int64_t j = ja; j <= jb; j++)
            {
                int64_t w = d;
                if (j == ja)
                    w -= fa;
                if (j == jb)
                    w -= d - fb;
                if (w <= 0 || j >= (int64_t)cells_count)
                    continue;
                const uint8_t v = (j < 0) ? rs->carry[(int64_t)rs->carry_count + j] : cells[j];
                ctc0_add_cell(v, w, &ones_w, &div2_w);
            };
        }
        else if (ja == jb)
        {
            /* Celý interval v jedné buňce (při b > a leží ja před koncem snímku). */
            if (len > 0)
                ctc0_add_cell(cells[ja], len, &ones_w, &div2_w);
        }
        else
        {
            /* ja < jb <= cells_count: levá buňka leží ve snímku, pravá jen při fb > 0. */
            ctc0_add_cell(cells[ja], d - fa, &ones_w, &div2_w);
            int64_t nonzero = 0, div2 = 0;
            ctc0_count_cells(cells, cells_count, (size_t)(ja + 1), (size_t)jb, &nonzero, &div2);
            ones_w += (nonzero - div2) * d;
            div2_w += div2 * d;
            if (fb > 0)
                ctc0_add_cell(cells[jb], fb, &ones_w, &div2_w);
        };

        /* Dělitel = délka intervalu v buňkách + 1 (zesílení r / (r + 1) jako
         * dřívější dělení počtem celých buněk + 1, u každého vzorku stejné);
         * pro stejnou délku jako minule se nepočítá znovu (stejné bity). */
        if (len != unit_len)
        {
            div = (double)len + (double)d;
            /* úroveň vzorku trvale v jedničce (zesílení převzorkování tohoto vzorku) */
            unit = (float)((double)len / div);
            unit_len = len;
        };
        float resampled;
        if (div2_w == 0 && ones_w == 0)
            resampled = 0.0f; /* 0 / div */
        else if (div2_w == 0 && ones_w == len)
            resampled = unit; /* stejný výraz jako unit */
        else
            resampled = (float)(((double)ones_w + (double)AUDIO_CTC0_DIV2_FACTOR * (double)div2_w) / div);

        /* Střídavá vazba (místo dřívějšího parkování stálé úrovně), jednopólový
         * low-pass a anti-glitch - stejná funkce jako v rendereru video záznamu. */
        float raw = 0.0f;
        out[i] = videorec_audio_chain_ctc0(&rs->chain, &rs->coef, resampled, unit, &raw);
        if (raw_out)
            raw_out[i] = raw;
        a = b;
        ja = jb;
        fa = fb;
    };

    /* Buňky od buňky poslední hrany do konce snímku přejdou do dalšího bloku
     * (jeho první vzorek je průměruje spolu se začátkem dalšího snímku). */
    int64_t start = ctc0_cell_of(a, d);
    if (start < 0)
        start = 0;
    size_t tail = cells_count - (size_t)start;
    if (tail > carry_max)
        tail = carry_max;
    memcpy(rs->carry, cells + (cells_count - tail), tail);
    rs->carry_count = tail;
}

/* Doxygen viz iface_audio_resampler.h. */
float *iface_audio_resampler_output_stream_ctc0(const uint8_t *cells, size_t cells_count, const int64_t *edge_pos,
                                                size_t output_samples_count, bool contiguous, float *raw_out)
{
    float *output_samples = g_malloc0(output_samples_count * sizeof(float));
    if (!output_samples)
    {
        return NULL;
    };
    iface_audio_resampler_ctc0_run(&s_ctc0, cells, cells_count, edge_pos, output_samples_count, contiguous,
                                   CTC0_CELL_UNITS, IFACE_AUDIO_CTC0_CARRY_MAX, output_samples, raw_out);
    return output_samples;
}

/**
 * @brief Iterátor úseků zvukového logu kanálu (pro integraci výchylky přes čas).
 *
 * Úsek j platí na taktech `[start, end)` logu; poslední úsek (a log bez
 * událostí) trvá do nekonečna (`end` = UINT64_MAX). Posouvá se jen dopředu.
 */
typedef struct st_PSG_LOG_ITER
{
    const st_AUDIO_SOURCE_LOG *src; /**< Log kanálu. */
    size_t index;                   /**< Aktuální úsek. */
    uint64_t start;                 /**< Začátek aktuálního úseku [takty logu]. */
    uint64_t end;                   /**< Konec aktuálního úseku (bez); UINT64_MAX u posledního. */
    uint8_t value;                  /**< Hodnota aktuálního úseku. */
} st_PSG_LOG_ITER;

/**
 * @brief Založí iterátor na prvním úseku logu.
 * @param it  Iterátor (výstup).
 * @param src Log kanálu; musí žít, dokud se iterátor používá.
 */
static inline void psg_iter_init(st_PSG_LOG_ITER *it, const st_AUDIO_SOURCE_LOG *src)
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
static inline void psg_iter_seek(st_PSG_LOG_ITER *it, uint64_t t)
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
static inline double psg_iter_integrate(st_PSG_LOG_ITER *it, uint64_t a, uint64_t b, const float *volume, unsigned *pieces)
{
    psg_iter_seek(it, a);
    double sum = 0.0;
    uint64_t t = a;
    unsigned k = 1;
    while (it->end < b)
    {
        sum += (double)volume[it->value] * (double)(it->end - t);
        t = it->end;
        psg_iter_seek(it, t);
        k++;
    };
    sum += (double)volume[it->value] * (double)(b - t);
    *pieces = k;
    return sum;
}

/* Doxygen viz iface_audio_resampler.h. */
void iface_audio_resampler_psg_average(const st_AUDIO_SOURCE_LOG *source, uint64_t frame_offset, uint64_t frame_span,
                                       uint64_t valid_end, const int64_t *ticks, size_t n, bool contiguous,
                                       const float *volume, st_IFACE_AUDIO_PSG_CARRY *carry, float *avg,
                                       uint8_t *end_value)
{
    st_PSG_LOG_ITER it;
    psg_iter_init(&it, source);

    /* Kus prvního vzorku před začátkem snímku: z předchozího bloku, jinak
     * (nová časová osa) výchylkou na začátku snímku. */
    const int64_t lead_len = (ticks[0] < 0) ? -ticks[0] : 0;
    if (!contiguous || !carry->valid)
    {
        psg_iter_seek(&it, frame_offset);
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
        carry->sum += psg_iter_integrate(&it, frame_offset - p, frame_offset, volume, &pieces);
        if ((uint64_t)carry->pending > p)
        {
            /* neočekávaně krátký začátek logu: zbytek hodnotou na jeho začátku */
            st_PSG_LOG_ITER it0;
            psg_iter_init(&it0, source);
            carry->sum += (double)volume[it0.value] * (double)((uint64_t)carry->pending - p);
        };
        carry->pending = 0;
    };
    psg_iter_seek(&it, frame_offset);

    int64_t prev = 0; /* začátek intervalu vzorku od začátku snímku (první vzorek od 0 + carry) */
    if (source->samples_count == 0 && n > 0)
    {
        /* Kanál beze změny v celém logu (typicky ztlumený kanál): jediný úsek bez
         * konce. Bitově totéž co obecná smyčka níže: první vzorek s přeneseným
         * zbytkem stejným výrazem, ostatní celé v úseku (výchylka bez dělení);
         * hranice `prev` se posouvá stejným ořezem, jen bez větvení na vzorek. */
        const uint8_t v = it.value;
        const float level = volume[v];
        int64_t b = ticks[1];
        if (b < 0)
            b = 0;
        if (b > (int64_t)frame_span)
            b = (int64_t)frame_span;
        double sum = 0.0; /* = psg_iter_integrate() přes jeden úsek */
        sum += (double)level * (double)((uint64_t)b);
        const int64_t len = carry->len + b;
        avg[0] = (len > 0) ? (float)((carry->sum + sum) / (double)len) : level;
        if (end_value)
            end_value[0] = v;
        /* hranice po ořezu: max(0, ticks[1..n]) omezené délkou snímku */
        int64_t last = b;
        for (size_t i = 1; i < n; i++)
        {
            avg[i] = level;
            if (ticks[i + 1] > last)
                last = ticks[i + 1];
        };
        if (end_value && n > 1)
            memset(end_value + 1, v, n - 1);
        prev = (last > (int64_t)frame_span) ? (int64_t)frame_span : last;
        n = 0; /* obecná smyčka se přeskočí */
    };
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
        const double sum = psg_iter_integrate(&it, la, lb, volume, &pieces);
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
    carry->sum = psg_iter_integrate(&it, tail_a, vend, volume, &pieces);
    carry->len = (int64_t)frame_span - prev;
    carry->pending = (int64_t)(tail_b - vend);
    carry->valid = true;
}

/** @brief Rozpracovaný interval vzorku na hranici bloku po kanálech PSG SDL cesty (jen vlákno resampleru). */
static st_IFACE_AUDIO_PSG_CARRY s_psg_carry[AUDIO_SRC_CHANNELS_COUNT];

/* Doxygen viz iface_audio_resampler.h. */
float *iface_audio_resampler_process_psg_audio_log(int channel, const st_AUDIO_SOURCE_LOG *source,
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

    if (source->samples_count == 0)
    {
        /* Kanál beze změny v celém logu (typicky ztlumený): průměr přímo do výstupu,
         * bez parkování a bez IIR (jako dosud); hodnoty na konci intervalů se tu
         * nepoužijí. Výstup je bitově týž jako přes s_avg. */
        iface_audio_resampler_psg_average(source, frame_offset, frame_span, valid_end, ticks, samples_count, contiguous,
                                          SN76489_volume_value, &s_psg_carry[channel], samples, NULL);
        if (raw_out)
            memcpy(raw_out, samples, samples_count * sizeof(float));
        return samples;
    };
    iface_audio_resampler_psg_average(source, frame_offset, frame_span, valid_end, ticks, samples_count, contiguous,
                                      SN76489_volume_value, &s_psg_carry[channel], s_avg, s_end);

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


/**
 * Vytvori audio stream ve float formatu z PSG vstupniho streamu.
 *
 * @param input_samples Ukazatel na vstupni stream.
 * @param input_samples_count Pocet vzorku ve vstupnim streamu.
 * @param output_samples_count Pocet vzorku ve vystupnim streamu. (pokud je jiny, nez vstupni, tak dojde k resamplingu na vystupni frekvenci)
 * @param SN76489_volume_value Pole s odpovidajicimi float hodnotami pro kazdou uroven 0-15.
 * @param parked_samples Pocet vzorku, ktere jsou limitni pro setrvavaní na stejne nenulove hodnotě. Po jeho prekroceni se zacne hodnota snizovat k 0.
 * @param parked_fade Pocet vzorku, ktere se pouziji pro plynuly prechod ze zaparkované hodnoty na 0.
 * @return Ukazatel na alokovany buffer s vystupnim streamem.
 */
float *iface_audio_resampler_output_stream_psg(int channel, const uint8_t *input_samples, size_t input_samples_count, size_t output_samples_count, float *SN76489_volume_value, size_t parked_samples, size_t parked_fade)
{
    size_t output_samples_size = output_samples_count * sizeof(float);
    float *output_samples = g_malloc0(output_samples_size);
    if (!output_samples)
    {
        return NULL;
    };

    double ratio = (double)input_samples_count / (double)output_samples_count;
    static float last_filtered[AUDIO_SRC_CHANNELS_COUNT] = {0}; // Uchováváme poslední filtrovaný vzorek pro plynulost
    // static float last_sampled_value[AUDIO_SRC_CHANNELS_COUNT] = {0};

    for (size_t i = 0; i < output_samples_count; i++)
    {
        double index = i * ratio;
        size_t start = (size_t)index;
        size_t end = (size_t)((i + 1) * ratio);

        if (end >= input_samples_count)
        {
            end = input_samples_count - 1;
        };

        uint8_t end_value = input_samples[end];

        // Vypočítání průměrné hlasitosti v daném rozsahu
        float sum = 0.0f;
        for (size_t j = start; j < end; j++)
        {
            sum += SN76489_volume_value[input_samples[j] & 0x0f]; // Použij dolních 4 bity pro rozsah 0-15
        };

        float resampled = sum / (float)(end - start + 1);

#if 1
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

        // float iir_x = 4.0f;
        // static float sample_value[AUDIO_SRC_CHANNELS_COUNT] = {0}; // posledni hodnota pro IIR filtr
        // sample_value[channel] = sample_value[channel] + (resampled - sample_value[channel]) / iir_x;
        // float filtered1 = sample_value[channel];

        // Aplikujeme jednopólový filtr
        // float filtered1 = iface_audio_lowpass_filter(channel, resampled);
        float filtered1 = resampled;

        // float filtered2 = iface_audio_lowpass_filter_2nd_order(filtered1);
        float filtered2 = filtered1;

        // Aplikujeme Butterworthův filtr
        // float filtered3 = iface_audio_lowpass_filter_butterworth(channel, filtered2);
        float filtered3 = filtered2;

        // float filtered4 = iface_audio_anti_glitch_filter(channel, filtered3);
        float filtered4 = filtered3;

        // float final_output = iface_audio_anti_glitch_limiter(filtered4);
        float final_output = filtered4;

        // output_samples[i] = final_output;
        //  output_samples[i] = last_sample_value;

        // output_samples[i] = final_output;
        output_samples[i] = (last_filtered[channel] + final_output) * 0.5f;
        last_filtered[channel] = final_output;

        // output_samples[i] = resampled;
    };

    return output_samples;
}
