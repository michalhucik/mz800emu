/**
 * @file   videorec_rt.c
 * @brief  Implementace hodin vzorkovače a zvukového jitter bufferu režimu "podle reality" (viz videorec_rt.h).
 *
 * @par Licence: GPLv3
 */

#include "videorec_rt.h"

#include <limits.h>
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
    int64_t num = 1000000LL * (int64_t)(fps_den ? fps_den : 1);
    int64_t den = (int64_t)(fps_num ? fps_num : 1);
    /* zkrácení zlomku: menší mezivýsledky ve videorec_rt_clock_due() a _next_deadline()
     * (krystal PAL: 354 432 000 000 / 17 734 475 -> 14 177 280 000 / 709 379) */
    int64_t a = num, b = den;
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    c->period_num = num / a;
    c->period_den = den / a;
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
    /* horní celá část: u zlomkové frekvence nejdelší blok, u celé přesně */
    return (unsigned)(((uint64_t)VIDEOREC_RT_SDL_RATE * den + num - 1u) / num);
}

/*******************************************************************************
 *
 *                  Fronta emulovaných snímků
 *
 ******************************************************************************/

void videorec_rt_frameq_init(st_VIDEOREC_RT_FRAMEQ *q, unsigned window, void (*release)(void *item))
{
    memset(q, 0, sizeof(*q));
    q->window = window ? window : 1u;
    q->release = release;
    q->starved = true;
    q->last_mode = VIDEOREC_RT_FRAMEQ_LIVE;
    q->win_min = UINT_MAX;
    q->underflow_ago = UINT_MAX;
    q->restarted = true;
    q->ahead = false;
}

/**
 * @brief Zahodí nejstarší položku fronty (uvolní ji callbackem).
 * @param q Instance s `len > 0`.
 */
static void frameq_drop_head(st_VIDEOREC_RT_FRAMEQ *q)
{
    void *it = q->item[q->head];
    q->item[q->head] = NULL;
    q->head = (q->head + 1u) % VIDEOREC_RT_FRAMEQ_CAP;
    q->len--;
    if (q->release) q->release(it);
}

/** @brief Začne nové okno regulátoru úrovně. @param q Instance. */
static void frameq_window_reset(st_VIDEOREC_RT_FRAMEQ *q)
{
    q->win_n = 0;
    q->win_min = UINT_MAX;
}

void videorec_rt_frameq_restart(st_VIDEOREC_RT_FRAMEQ *q)
{
    q->starved = true;
    q->filling = false;
    q->draining = false;
    q->restarted = true;
    frameq_window_reset(q);
}

void videorec_rt_frameq_clear(st_VIDEOREC_RT_FRAMEQ *q)
{
    while (q->len) frameq_drop_head(q);
    q->head = 0;
    q->last_mode = VIDEOREC_RT_FRAMEQ_LIVE;
    q->slow = false;
    q->ahead = false;
    q->underflow_ago = UINT_MAX;
    videorec_rt_frameq_restart(q);
}

void videorec_rt_frameq_push(st_VIDEOREC_RT_FRAMEQ *q, void *item, uint8_t tag)
{
    if (q->len == VIDEOREC_RT_FRAMEQ_CAP) {
        /* Plná fronta (vzorkovač neodebírá - MAX SPEED, pauza vzorkovače): zahodit nejstarší. */
        frameq_drop_head(q);
        q->stats.overflow_drops++;
    }
    unsigned slot = (q->head + q->len) % VIDEOREC_RT_FRAMEQ_CAP;
    q->item[slot] = item;
    q->tag[slot] = tag;
    q->len++;
    q->stats.pushed++;
}

unsigned videorec_rt_frameq_trim(st_VIDEOREC_RT_FRAMEQ *q, unsigned keep)
{
    unsigned n = 0;
    while (q->len > keep) {
        frameq_drop_head(q);
        q->stats.surplus_drops++;
        n++;
    }
    return n;
}

unsigned videorec_rt_frameq_merge_dups(st_VIDEOREC_RT_FRAMEQ *q)
{
    if (q->len < 2) return 0;
    unsigned kept = 1, removed = 0;
    for (unsigned i = 1; i < q->len; i++) {
        unsigned s = (q->head + i) % VIDEOREC_RT_FRAMEQ_CAP;
        unsigned last = (q->head + kept - 1u) % VIDEOREC_RT_FRAMEQ_CAP;
        if (q->item[s] == q->item[last] && q->tag[s] == q->tag[last]) {
            /* stejný obraz jako předchozí položka (snímky bez nového zobrazení) */
            if (q->release) q->release(q->item[s]);
            q->item[s] = NULL;
            removed++;
            continue;
        }
        unsigned d = (q->head + kept) % VIDEOREC_RT_FRAMEQ_CAP;
        q->item[d] = q->item[s];
        q->tag[d] = q->tag[s];
        if (d != s) q->item[s] = NULL;
        kept++;
    }
    q->len = kept;
    q->stats.surplus_drops += removed;
    return removed;
}

unsigned videorec_rt_frameq_pos_offset(const st_VIDEOREC_RT_FRAMEQ *q)
{
    const unsigned reserve = VIDEOREC_RT_FRAMEQ_TARGET - 1u;
    if (q->last_mode == VIDEOREC_RT_FRAMEQ_FREE) {
        /* další tik vezme nejstarší z nejvýš TARGET různých obrazů (duplicity se slučují);
         * obraz na obrazovce je nejnovější */
        unsigned l = 0;
        for (unsigned i = 0; i < q->len && l < VIDEOREC_RT_FRAMEQ_TARGET; i++) {
            unsigned s = (q->head + q->len - 1u - i) % VIDEOREC_RT_FRAMEQ_CAP;
            unsigned n = (s + 1u) % VIDEOREC_RT_FRAMEQ_CAP;
            if (i == 0 || q->item[s] != q->item[n] || q->tag[s] != q->tag[n]) l++;
        }
        return l ? l - 1u : 0u;
    }
    if (q->slow && q->last_mode == VIDEOREC_RT_FRAMEQ_LIVE) {
        /* pomalý režim plní jen na 1 snímek: fronta se odebírá v pořadí, prázdná = zapíše se
         * právě zobrazený obraz */
        unsigned l = q->len ? q->len - 1u : 0u;
        return (l > reserve) ? reserve : l;
    }
    if (q->filling) return reserve; /* plnění v živém běhu: opakování až do rezervy */
    return (q->len > reserve) ? reserve : q->len;
}

void *videorec_rt_frameq_pop(st_VIDEOREC_RT_FRAMEQ *q, uint8_t *tag_out)
{
    if (q->len == 0) return NULL;
    void *it = q->item[q->head];
    if (tag_out) *tag_out = q->tag[q->head];
    q->item[q->head] = NULL;
    q->head = (q->head + 1u) % VIDEOREC_RT_FRAMEQ_CAP;
    q->len--;
    return it;
}

unsigned videorec_rt_frameq_purge_tag(st_VIDEOREC_RT_FRAMEQ *q, uint8_t tag)
{
    unsigned removed = 0, kept = 0;
    for (unsigned i = 0; i < q->len; i++) {
        unsigned s = (q->head + i) % VIDEOREC_RT_FRAMEQ_CAP;
        if (q->tag[s] == tag) {
            if (q->release) q->release(q->item[s]);
            q->item[s] = NULL;
            removed++;
            continue;
        }
        unsigned d = (q->head + kept) % VIDEOREC_RT_FRAMEQ_CAP;
        q->item[d] = q->item[s];
        q->tag[d] = q->tag[s];
        if (d != s) q->item[s] = NULL;
        kept++;
    }
    q->len = kept;
    q->stats.purged += removed;
    return removed;
}

en_VIDEOREC_RT_TAKE videorec_rt_frameq_take(st_VIDEOREC_RT_FRAMEQ *q, en_VIDEOREC_RT_FRAMEQ_MODE mode, void **item_out)
{
    *item_out = NULL;
    q->last_mode = mode;
    if (q->underflow_ago != UINT_MAX) q->underflow_ago++;
    if (mode != VIDEOREC_RT_FRAMEQ_LIVE) {
        /* Emulace a vzorkovač běží záměrně jinak (rychlost != 100 %, pauza): jitter buffer
         * nemá smysl, po návratu do živého běhu se fronta naplní znovu. */
        videorec_rt_frameq_restart(q);
        if (mode == VIDEOREC_RT_FRAMEQ_FREE) {
            /* Nad 100 % sdílí několik emulovaných snímků jeden zobrazený obraz: odebírat po
             * obrazech, ne po položkách (jinak opakování a přeskoky podle fáze tiků). */
            (void)videorec_rt_frameq_merge_dups(q);
            (void)videorec_rt_frameq_trim(q, VIDEOREC_RT_FRAMEQ_TARGET);
        }
        if (q->len == 0) return VIDEOREC_RT_TAKE_LATEST;
        *item_out = videorec_rt_frameq_pop(q, NULL);
        q->stats.taken++;
        return VIDEOREC_RT_TAKE_FRAME;
    }

    unsigned level = q->len;
    /* Plní se na cíl; v pomalém režimu (emulace opakovaně nestíhá) jen na 1 snímek, aby se
     * opakovalo po jednom a rovnoměrně, ne po dávkách. */
    const unsigned fill_to = q->slow ? 1u : VIDEOREC_RT_FRAMEQ_TARGET;
    if (q->starved) {
        if (level < fill_to) {
            /* Plnění (start, po vyprázdnění, po pauze): opakovat obraz před frontou. */
            q->filling = true;
            return level ? VIDEOREC_RT_TAKE_PREV : VIDEOREC_RT_TAKE_LATEST;
        }
        q->starved = false;
        q->filling = false;
        frameq_window_reset(q);
        if (q->restarted && !q->slow && level > fill_to) {
            /* Mezi dvěma tiky přišly dva snímky (nerovnoměrný rozběh po startu nebo pauze):
             * bez zásahu by fronta zůstala o snímek delší a obraz trvale o snímek později
             * proti zvuku. Přesah zahodit hned v náběhu. */
            while (q->len > fill_to) {
                frameq_drop_head(q);
                q->stats.prime_trims++;
            }
            level = q->len;
        }
        q->restarted = false;
    }

    /* Úroveň aspoň horní mez normálního stavu = emulace zaostání dohnala (zaseknutí, ne pomalý
     * počítač, který snímky nedohání). */
    if (level >= VIDEOREC_RT_FRAMEQ_TARGET + VIDEOREC_RT_FRAMEQ_SURPLUS) q->ahead = true;
    if (q->slow && level > VIDEOREC_RT_FRAMEQ_TARGET) {
        /* Pomalý režim, ale emulace je zase napřed: přebytek nesmí zůstat ve frontě - dál
         * běžná regulace (vybíjení). */
        q->slow = false;
    }

    if (level == 0) {
        /* Emulace zaostala o víc, než pokryje rezerva fronty: opakovat a naplnit znovu.
         * Vyprázdní-li se fronta znovu do VIDEOREC_RT_FRAMEQ_SLOW_S a emulace mezitím
         * zaostání nedohnala (`ahead`), trvale nestíhá (pomalý počítač) - pomalý režim.
         * Dvě ojedinělá zaseknutí s dohnáním ho nezapnou. */
        if (q->underflow_ago < VIDEOREC_RT_FRAMEQ_SLOW_S * q->window && !q->ahead) q->slow = true;
        q->ahead = false;
        q->underflow_ago = 0;
        q->starved = true;
        q->filling = true;
        q->stats.underflows++;
        frameq_window_reset(q);
        return VIDEOREC_RT_TAKE_LATEST;
    }

    if (level < q->win_min) q->win_min = level;
    if (++q->win_n >= q->window) {
        unsigned wmin = q->win_min;
        frameq_window_reset(q);
        if (q->slow) {
            /* Pomalý režim: bez vyprázdnění po VIDEOREC_RT_FRAMEQ_SLOW_S emulace zase stíhá -
             * rezervu obnovit po jednom opakování za okno. */
            if (q->underflow_ago >= VIDEOREC_RT_FRAMEQ_SLOW_S * q->window) {
                if (wmin >= VIDEOREC_RT_FRAMEQ_TARGET) {
                    q->slow = false;
                } else {
                    q->stats.restore_fills++;
                    return VIDEOREC_RT_TAKE_PREV;
                }
            }
        } else {
            /* Úroveň do cíl + VIDEOREC_RT_FRAMEQ_SURPLUS - 1 je normální stav (zvukové zařízení
             * posouvá dokončování snímků o celý snímek a zvuk se posouvá s ním). Celé okno
             * přebytek (emulace trvale napřed: zkrácené snímky CKSW, hodiny zařízení, dohnané
             * zaseknutí) = vybíjení: po jednom nejstarším snímku za okno, dokud úroveň v celém
             * okně neklesne na cíl - rovnoměrně, bez skoku o víc snímků. Velký přebytek (po
             * zaseknutí) se nejdřív srazí najednou na horní mez normálního stavu. */
            const unsigned high = VIDEOREC_RT_FRAMEQ_TARGET + VIDEOREC_RT_FRAMEQ_SURPLUS;
            if (wmin >= high) q->draining = true;
            if (q->draining) {
                if (wmin > VIDEOREC_RT_FRAMEQ_TARGET && q->len > 1) {
                    unsigned k = (wmin > high) ? wmin - high + 1u : 1u;
                    for (; k > 0 && q->len > 1; k--) {
                        frameq_drop_head(q);
                        q->stats.surplus_drops++;
                    }
                } else {
                    q->draining = false;
                }
            }
        }
    }
    *item_out = videorec_rt_frameq_pop(q, NULL);
    q->stats.taken++;
    return VIDEOREC_RT_TAKE_FRAME;
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
    ra->tick_scale = 1.0;
}

void videorec_rt_audio_set_tick_scale(st_VIDEOREC_RT_AUDIO *ra, double scale)
{
    g_mutex_lock(&ra->mutex);
    ra->tick_scale = (scale > 0.0) ? scale : 1.0;
    g_mutex_unlock(&ra->mutex);
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
        const uint64_t tol = (uint64_t)ra->in_rate * VIDEOREC_RT_AUDIO_PRIME_TOL_MS / 1000u;
        if (fill + tol < ra->target) {
            /* Čeká se na naplnění (start, po výpadku): ticho, nic se nespotřebuje. */
            rt_silence(out_or_null, 0, out_frames);
            ra->stats.silence += out_frames;
            g_mutex_unlock(&ra->mutex);
            return;
        }
        ra->starved = false;
        ra->stats.fill_ema = (double)fill;
        if (fill > ra->target && fill <= (uint64_t)ra->target * 4u) {
            /* Náběh na cíl: blok, kterým naplnění dosáhlo cíle (s tolerancí
             * VIDEOREC_RT_AUDIO_PRIME_TOL_MS: 3 x 881 = 2643 stačí), ho může přesáhnout (NTSC
             * 4 x 735 = 2940). Přebytek by zvuk proti obrazu na začátku zpozdil a regulátor
             * by ho stahoval desítky sekund. Nejstarší vzorky zahodit (velký přebytek řeší
             * níže overrun). */
            uint64_t drop = fill - ra->target;
            ra->rd += drop;
            ra->stats.consumed += drop;
            ra->stats.prime_drops += drop;
            fill = ra->target;
            ra->stats.fill_ema = (double)fill;
        }
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
    double step = (double)ra->in_rate / (double)ra->out_rate * ra->tick_scale * (1.0 + corr);

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
