/**
 * @file   videorec_rt.c
 * @brief  Implementace hodin vzorkovače a zvukového jitter bufferu režimu "podle reality" (viz videorec_rt.h).
 *
 * @par Licence: GPLv3
 */

#include "videorec_rt.h"

#include <math.h>
#include <string.h>

/*******************************************************************************
 *
 *                  Hodiny vzorkovače
 *
 ******************************************************************************/

void videorec_rt_clock_init(st_VIDEOREC_RT_CLOCK *c, int64_t period_us, unsigned max_catchup)
{
    memset(c, 0, sizeof(*c));
    c->period_num = (period_us > 0) ? period_us : 1;
    c->period_den = 1;
    c->max_catchup = max_catchup ? max_catchup : 1;
}

void videorec_rt_clock_init_fps(st_VIDEOREC_RT_CLOCK *c, unsigned fps_num, unsigned fps_den, unsigned max_catchup)
{
    memset(c, 0, sizeof(*c));
    c->period_num = 1000000LL * (int64_t)(fps_den ? fps_den : 1);
    c->period_den = (int64_t)(fps_num ? fps_num : 1);
    c->max_catchup = max_catchup ? max_catchup : 1;
}

unsigned videorec_rt_clock_due(st_VIDEOREC_RT_CLOCK *c, int64_t now_us)
{
    if (!c->started) {
        c->started = true;
        c->base_us = now_us;
        c->done = 1;
        return 1;
    }
    if (now_us < c->base_us) return 0;
    /* počet termínů base + k * num / den <= now (k = 0, 1, ...), tj. k <= (now - base) * den / num */
    uint64_t elapsed = (uint64_t)(((now_us - c->base_us) * c->period_den) / c->period_num) + 1;
    if (elapsed <= c->done) return 0;
    uint64_t n = elapsed - c->done;
    if (n > c->max_catchup) {
        /* Velké zpoždění (zaseknutí hostitele): nedohánět, založit řadu znovu. */
        c->rebased++;
        c->lost_ticks += n - 1;
        c->base_us = now_us;
        c->done = 1;
        return 1;
    }
    c->done = elapsed;
    return (unsigned)n;
}

int64_t videorec_rt_clock_next_deadline(const st_VIDEOREC_RT_CLOCK *c)
{
    if (!c->started) return 0;
    /* ceil(done * num / den): první celé us, ve kterém je tick `done` splatný */
    return c->base_us + ((int64_t)c->done * c->period_num + c->period_den - 1) / c->period_den;
}

unsigned videorec_rt_video_delay_ticks(unsigned fps_num, unsigned fps_den)
{
    uint64_t num = fps_num ? fps_num : 1u;
    uint64_t den = fps_den ? fps_den : 1u;
    /* round(ms * num / (1000 * den)) celočíselně */
    uint64_t q = 1000u * den;
    uint64_t ticks = ((uint64_t)VIDEOREC_RT_VIDEO_DELAY_MS * num + q / 2u) / q;
    return (ticks >= 1u) ? (unsigned)ticks : 1u;
}

unsigned videorec_rt_sdl_chunk(unsigned fps_num, unsigned fps_den)
{
    uint64_t num = fps_num ? fps_num : 1u;
    uint64_t den = fps_den ? fps_den : 1u;
    return (unsigned)((uint64_t)VIDEOREC_RT_SDL_RATE * den / num);
}

/*******************************************************************************
 *
 *                  Zvukový jitter buffer
 *
 ******************************************************************************/

void videorec_rt_audio_init(st_VIDEOREC_RT_AUDIO *ra, unsigned in_rate, unsigned out_rate, unsigned target_ms,
                            unsigned capacity_ms)
{
    memset(ra, 0, sizeof(*ra));
    g_mutex_init(&ra->mutex);
    ra->in_rate = in_rate ? in_rate : 1;
    ra->out_rate = out_rate ? out_rate : 1;
    ra->target = (uint32_t)((uint64_t)ra->in_rate * target_ms / 1000u);
    if (ra->target < 1) ra->target = 1;
    ra->cap = (uint32_t)((uint64_t)ra->in_rate * capacity_ms / 1000u);
    if (ra->cap < ra->target * 2u) ra->cap = ra->target * 2u;
    ra->ring = g_new0(float, (size_t)ra->cap * 2u);
    ra->starved = true;
}

void videorec_rt_audio_free(st_VIDEOREC_RT_AUDIO *ra)
{
    g_free(ra->ring);
    ra->ring = NULL;
    g_mutex_clear(&ra->mutex);
}

void videorec_rt_audio_reset(st_VIDEOREC_RT_AUDIO *ra, bool enabled, unsigned out_rate)
{
    g_mutex_lock(&ra->mutex);
    ra->wr = 0;
    ra->rd = 0;
    ra->frac = 0.0;
    ra->starved = true;
    memset(&ra->stats, 0, sizeof(ra->stats));
    g_atomic_int_set(&ra->enabled, enabled ? 1 : 0);
    if (out_rate) ra->out_rate = out_rate;
    g_mutex_unlock(&ra->mutex);
}

void videorec_rt_audio_push(st_VIDEOREC_RT_AUDIO *ra, const float *stereo, size_t frames)
{
    g_mutex_lock(&ra->mutex);
    if (g_atomic_int_get(&ra->enabled) && ra->ring) {
        for (size_t i = 0; i < frames; i++) {
            size_t slot = (size_t)(ra->wr % ra->cap) * 2u;
            ra->ring[slot] = stereo[2 * i];
            ra->ring[slot + 1] = stereo[2 * i + 1];
            ra->wr++;
        }
        ra->stats.pushed += frames;
        if (ra->wr - ra->rd > ra->cap) {
            /* Plná kapacita: nejstarší vzorky jsou přepsané - posunout čtení. */
            uint64_t drop = (ra->wr - ra->rd) - ra->cap;
            ra->rd += drop;
            ra->frac = 0.0;
            ra->stats.overflow_drops += drop;
            ra->stats.consumed += drop;
        }
    }
    g_mutex_unlock(&ra->mutex);
}

bool videorec_rt_audio_wants_data(st_VIDEOREC_RT_AUDIO *ra, size_t chunk)
{
    g_mutex_lock(&ra->mutex);
    bool want = g_atomic_int_get(&ra->enabled) && (ra->wr - ra->rd) < (uint64_t)ra->target + chunk;
    g_mutex_unlock(&ra->mutex);
    return want;
}

bool videorec_rt_audio_is_enabled(st_VIDEOREC_RT_AUDIO *ra)
{
    /* Bez zámku, jedno atomické čtení: předběžný test producenta (push ho ověří pod zámkem). */
    return g_atomic_int_get(&ra->enabled) != 0;
}

size_t videorec_rt_audio_fill(st_VIDEOREC_RT_AUDIO *ra)
{
    g_mutex_lock(&ra->mutex);
    size_t f = (size_t)(ra->wr - ra->rd);
    g_mutex_unlock(&ra->mutex);
    return f;
}

void videorec_rt_audio_get_stats(st_VIDEOREC_RT_AUDIO *ra, st_VIDEOREC_RT_AUDIO_STATS *out)
{
    g_mutex_lock(&ra->mutex);
    *out = ra->stats;
    g_mutex_unlock(&ra->mutex);
}

/**
 * @brief Převede vzorek F32 se zesílením na int16 s ořezem.
 * @param x    Vzorek.
 * @param gain Zesílení.
 * @return `round(clamp(gain * x, -1, 1) * 32767)`.
 */
static inline int16_t rt_to_s16(float x, float gain)
{
    float v = x * gain;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)lrintf(v * 32767.0f);
}

/**
 * @brief Vyplní tichem výstupní vzorky od indexu `from` do `n`.
 * @param out  Výstup nebo NULL (pak nic).
 * @param from První vzorek.
 * @param n    Počet vzorků celkem.
 */
static void rt_silence(int16_t *out, size_t from, size_t n)
{
    if (out && n > from) memset(out + from * 2u, 0, (n - from) * 2u * sizeof(int16_t));
}

void videorec_rt_audio_pull(st_VIDEOREC_RT_AUDIO *ra, int16_t *out_or_null, size_t out_frames, float gain)
{
    g_mutex_lock(&ra->mutex);
    uint64_t fill = ra->wr - ra->rd;

    if (ra->starved) {
        if (fill < ra->target) {
            /* Čeká se na naplnění (start, po výpadku): ticho, nic se nespotřebuje. */
            rt_silence(out_or_null, 0, out_frames);
            ra->stats.silence += out_frames;
            g_mutex_unlock(&ra->mutex);
            return;
        }
        ra->starved = false;
        ra->stats.fill_ema = (double)fill;
    }

    if (fill > (uint64_t)ra->target * 4u) {
        /* Přebytek (spotřebitel dlouho neodebíral): zahodit nejstarší až na cíl. */
        uint64_t drop = fill - ra->target;
        ra->rd += drop;
        ra->frac = 0.0;
        ra->stats.consumed += drop;
        ra->stats.overruns++;
        fill = ra->target;
        ra->stats.fill_ema = (double)fill;
    }

    /* P regulace průměrného naplnění k cíli = korekce driftu hodin zařízení. */
    ra->stats.fill_ema += VIDEOREC_RT_AUDIO_EMA_ALPHA * ((double)fill - ra->stats.fill_ema);
    double corr = (ra->stats.fill_ema - (double)ra->target) / ((double)ra->in_rate * VIDEOREC_RT_AUDIO_DRIFT_TC_S);
    if (corr > VIDEOREC_RT_AUDIO_MAX_CORR) corr = VIDEOREC_RT_AUDIO_MAX_CORR;
    if (corr < -VIDEOREC_RT_AUDIO_MAX_CORR) corr = -VIDEOREC_RT_AUDIO_MAX_CORR;
    ra->stats.corr = corr;
    if (fabs(corr) > ra->stats.max_abs_corr) ra->stats.max_abs_corr = fabs(corr);
    double step = (double)ra->in_rate / (double)ra->out_rate * (1.0 + corr);

    double pos = ra->frac;
    for (size_t j = 0; j < out_frames; j++) {
        uint64_t i = (uint64_t)pos;
        if (i + 1 >= fill) {
            /* Podtečení: zbytek ticha, buffer vyprázdnit a čekat na nové naplnění. */
            rt_silence(out_or_null, j, out_frames);
            ra->stats.silence += out_frames - j;
            ra->stats.underruns++;
            ra->stats.consumed += fill;
            ra->rd = ra->wr;
            ra->frac = 0.0;
            ra->starved = true;
            g_mutex_unlock(&ra->mutex);
            return;
        }
        if (out_or_null) {
            size_t s0 = (size_t)((ra->rd + i) % ra->cap) * 2u;
            size_t s1 = (size_t)((ra->rd + i + 1) % ra->cap) * 2u;
            float t = (float)(pos - (double)i);
            float l = ra->ring[s0] + (ra->ring[s1] - ra->ring[s0]) * t;
            float r = ra->ring[s0 + 1] + (ra->ring[s1 + 1] - ra->ring[s0 + 1]) * t;
            out_or_null[2 * j] = rt_to_s16(l, gain);
            out_or_null[2 * j + 1] = rt_to_s16(r, gain);
        }
        pos += step;
    }
    uint64_t adv = (uint64_t)pos;
    ra->rd += adv;
    ra->frac = pos - (double)adv;
    ra->stats.consumed += adv;
    ra->stats.produced += out_frames;
    g_mutex_unlock(&ra->mutex);
}
