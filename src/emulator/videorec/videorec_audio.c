/**
 * @file   videorec_audio.c
 * @brief  Renderer audio událostí na PCM v emulačním čase (box filtr + řetězec SDL cesty).
 *
 * Řetězec filtrů je kopie aktivní SDL cesty emulátoru (viz videorec_audio.h):
 * - CTC0: iface_audio_resampler_output_stream_ctc0() (iface_audio_resampler.c:193-296),
 *   tj. parkování (:229-266) -> iface_audio_lowpass_filter() (:79-86, alpha 0,4)
 *   -> iface_audio_anti_glitch_filter() (:157-165, 0,2); filtry 2. řádu,
 *   Butterworth a limiter jsou v SDL cestě zakomentované (:272-282);
 * - PSG: iface_audio_resampler_process_psg_audio_log() (:298-417), tj. hlasitostní
 *   tabulka (v rendereru zapečená do `level`) -> parkování (:342-382) -> IIR
 *   `sample += (x - sample) / 6` (:384-387);
 * - mix: iface_audio_mix_channels_with_gain() (iface_audio.c:66-101, mono):
 *   CTC0 * gain + průměr PSG0 kanálů * gain, ořez `<= 1`;
 *   iface_audio_mix_channels_stereo() (iface_audio.c:103-155, stereo):
 *   L = CTC0 + průměr PSG0, R = CTC0 + průměr PSG1, ořez každé strany `<= 1`
 *   (rozložení kanálů viz videorec_audio_set_stereo()).
 * SDL cesta drží stavy filtrů ve `static` proměnných; zde jsou v instanci
 * (st_VIDEOREC_AUDIO_CHSTATE), aby šly uložit a obnovit (plynulý retake).
 *
 * DC blok (horní propust), který tu byl dřív, je odstraněný: trvalou úroveň
 * (CTC0 držený na 1) řeší parkování stejně jako SDL cesta, takže nahrávka
 * zní jako výstup emulátoru a nevzniká přechodový jev DC bloku (doznívání
 * cca 2000 vzorků po každém skoku trvalé úrovně).
 *
 * @par Licence: GPLv3
 */

#include "videorec_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** @brief Doba konstantní nenulové hodnoty před parkováním [ms] (iface_audio.c:223-224). */
#define VR_PARK_MS 45u
/** @brief Doba útlumu zaparkované hodnoty [ms] (iface_audio.c:225-226). */
#define VR_PARK_FADE_MS 20u
/** @brief Počet kroků útlumu parkování (iface_audio_resampler.c:257-258). */
#define VR_PARK_FADE_STEPS 20

void videorec_audio_coef_init(st_VIDEOREC_AUDIO_COEF *c, unsigned rate)
{
    c->park_samples = (rate / 1000u) * VR_PARK_MS;
    c->park_fade = (rate / 1000u) * VR_PARK_FADE_MS;
    if (rate == VIDEOREC_AUDIO_SDL_RATE) {
        /* Přesně konstanty SDL cesty (bez přepočtu, aby nevznikla odchylka v posledním bitu). */
        c->ctc_lp_alpha = 0.4f;
        c->ctc_ag_factor = 0.2f;
        c->psg_iir_div = 6.0f;
        return;
    }
    /* Stejná zlomová frekvence jednopólového filtru: pól (1 - a) za sekundu stejný. */
    double k = (double)VIDEOREC_AUDIO_SDL_RATE / (double)rate;
    c->ctc_lp_alpha = (float)(1.0 - pow(1.0 - 0.4, k));
    c->ctc_ag_factor = (float)(1.0 - pow(1.0 - 0.2, k));
    c->psg_iir_div = (float)(1.0 / (1.0 - pow(1.0 - 1.0 / 6.0, k)));
}

float videorec_audio_sdl_ctc0_gain(size_t ctc_samples, size_t out_samples)
{
    /* Stejné dělení na bloky jako iface_audio_resampler.c:203-227, vstup trvale nenulový. */
    double ratio = (double)ctc_samples / (double)out_samples;
    double sum = 0.0;
    for (size_t i = 0; i < out_samples; i++) {
        double index = i * ratio;
        size_t start = (size_t)index;
        size_t end = (size_t)((i + 1) * ratio);
        if (end >= ctc_samples) end = ctc_samples - 1;
        sum += (float)(end - start) / (float)(end - start + 1);
    }
    return (float)(sum / (double)out_samples);
}

/**
 * @brief Parkování (kopie iface_audio_resampler.c:229-266 a :342-382).
 *
 * @param p         Stav parkování kanálu.
 * @param c         Koeficienty (doby parkování).
 * @param x         Vstup.
 * @param end_value Hodnota kanálu na konci vzorku.
 * @return Vstup vynásobený aktuálním zesílením útlumu (nebo beze změny).
 */
static inline float vr_park(st_VIDEOREC_AUDIO_PARK *p, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value)
{
    if ((end_value == 0) || (end_value != p->prev)) {
        p->prev = end_value;
        p->counter = 0;
    } else if (p->counter < c->park_samples) {
        p->counter++;
        p->gain = 1.0f;
        p->gain_counter = 0;
    } else {
        x *= p->gain;
        if (p->gain > 0.0f) {
            if (p->gain_counter-- == 0) {
                p->gain -= 1.0f / VR_PARK_FADE_STEPS;
                p->gain_counter = (int)(c->park_fade / VR_PARK_FADE_STEPS);
            }
        } else {
            p->gain = 0.0f;
        }
    }
    return x;
}

float videorec_audio_chain_ctc0(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value)
{
    float r = vr_park(&s->park, c, x, end_value);
    float alpha = c->ctc_lp_alpha;
    s->lp = alpha * r + (1.0f - alpha) * s->lp;          /* iface_audio_lowpass_filter() */
    s->ag = s->ag + c->ctc_ag_factor * (s->lp - s->ag);  /* iface_audio_anti_glitch_filter() */
    return s->ag;
}

float videorec_audio_chain_psg(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value)
{
    float r = vr_park(&s->park, c, x, end_value);
    s->lp = s->lp + (r - s->lp) / c->psg_iir_div;        /* iface_audio_resampler.c:384-386 */
    return s->lp;
}

/** @brief Vyprázdní frontu bez uvolnění paměti (kapacita zůstává). */
static void queue_clear(st_VIDEOREC_AUDIO_QUEUE *q) { q->head = 0; q->count = 0; }

/**
 * @brief Přidá událost na konec fronty; při nedostatku místa nejdřív posune okno na začátek, pak zdvojnásobí kapacitu.
 *
 * @return 0 při úspěchu, -1 při selhání realloc (fronta se nezmění).
 */
static int queue_push(st_VIDEOREC_AUDIO_QUEUE *q, uint64_t ticks, uint8_t value)
{
    if (q->head > 0 && q->head + q->count == q->cap) {
        memmove(q->ev, q->ev + q->head, q->count * sizeof(*q->ev));
        q->head = 0;
    }
    if (q->head + q->count == q->cap) {
        size_t ncap = q->cap ? q->cap * 2 : 256;
        st_VIDEOREC_AUDIO_EVENT *n = realloc(q->ev, ncap * sizeof(*n));
        if (!n) return -1;
        q->ev = n; q->cap = ncap;
    }
    q->ev[q->head + q->count].ticks = ticks;
    q->ev[q->head + q->count].value = value;
    q->count++;
    return 0;
}

void videorec_audio_init(st_VIDEOREC_AUDIO *a, uint64_t clk_hz, unsigned rate, unsigned channels,
                         const float level[][VIDEOREC_AUDIO_LEVELS], const uint8_t *values,
                         uint64_t origin_ticks, bool sdl_chain)
{
    memset(a, 0, sizeof(*a));
    a->clk_hz = clk_hz;
    a->rate = rate;
    a->channels = channels > VIDEOREC_AUDIO_MAX_CHANNELS ? VIDEOREC_AUDIO_MAX_CHANNELS : channels;
    for (unsigned c = 0; c < a->channels; c++)
        memcpy(a->level[c], level[c], sizeof(a->level[c]));
    a->sdl_chain = sdl_chain;
    for (unsigned c = 0; c < VIDEOREC_AUDIO_MAX_CHANNELS; c++) a->route[c] = VIDEOREC_AUDIO_ROUTE_LR;
    a->stereo = false;
    videorec_audio_coef_init(&a->coef, rate);
    videorec_audio_rebase(a, origin_ticks, values);
}

void videorec_audio_set_stereo(st_VIDEOREC_AUDIO *a, bool stereo)
{
    a->stereo = stereo;
    for (unsigned c = 0; c < VIDEOREC_AUDIO_MAX_CHANNELS; c++) {
        if (c == 0) a->route[c] = VIDEOREC_AUDIO_ROUTE_LR;                          /* CTC0 */
        else if (c <= VIDEOREC_AUDIO_PSG_CHANNELS) a->route[c] = stereo ? VIDEOREC_AUDIO_ROUTE_L : VIDEOREC_AUDIO_ROUTE_LR; /* PSG0 */
        else a->route[c] = stereo ? VIDEOREC_AUDIO_ROUTE_R : 0u;                    /* PSG1 */
    }
}

bool videorec_audio_is_stereo(const st_VIDEOREC_AUDIO *a)
{
    return a->stereo;
}

void videorec_audio_free(st_VIDEOREC_AUDIO *a)
{
    for (unsigned c = 0; c < VIDEOREC_AUDIO_MAX_CHANNELS; c++) {
        free(a->q[c].ev);
        a->q[c].ev = NULL; a->q[c].cap = 0; queue_clear(&a->q[c]);
    }
}

void videorec_audio_rebase(st_VIDEOREC_AUDIO *a, uint64_t origin_ticks, const uint8_t *values)
{
    for (unsigned c = 0; c < a->channels; c++) {
        queue_clear(&a->q[c]);
        a->value[c] = values ? (uint8_t)(values[c] & 0x0F) : 0;
    }
    a->origin = origin_ticks;
    a->n = 0;
}

void videorec_audio_get_state(const st_VIDEOREC_AUDIO *a, st_VIDEOREC_AUDIO_STATE *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned c = 0; c < a->channels; c++) {
        out->value[c] = a->value[c];
        out->ch[c] = a->flt[c];
    }
}

void videorec_audio_set_state(st_VIDEOREC_AUDIO *a, const st_VIDEOREC_AUDIO_STATE *st)
{
    for (unsigned c = 0; c < a->channels; c++) {
        a->value[c] = (uint8_t)(st->value[c] & 0x0F);
        a->flt[c] = st->ch[c];
    }
}

int videorec_audio_event(st_VIDEOREC_AUDIO *a, unsigned ch, uint8_t value, uint64_t ticks)
{
    if (ch >= a->channels) return -1;
    return queue_push(&a->q[ch], ticks, (uint8_t)(value & 0x0F));
}

/** @brief Čas (takty) začátku vzorku `n`; počítáno z `n`, aby se zaokrouhlení nekumulovalo. */
static inline uint64_t sample_start(const st_VIDEOREC_AUDIO *a, uint64_t n)
{
    return a->origin + (n * a->clk_hz) / a->rate;
}

/**
 * @brief Převede součet jedné strany na vzorek int16 (ořez jako SDL cesta, pak rozsah [-1, 1]).
 * @param a Instance (rozhoduje `sdl_chain`).
 * @param y Součet kanálů strany.
 * @return Vzorek int16.
 */
static inline int16_t mix_to_s16(const st_VIDEOREC_AUDIO *a, double y)
{
    if (a->sdl_chain) {
        float m = (float)y;
        y = (m <= 1) ? m : 1.0f; /* ořez mixu jako iface_audio_mix_channels_with_gain() / _stereo() */
    }
    if (y > 1.0) y = 1.0;
    if (y < -1.0) y = -1.0;
    return (int16_t)lrint(y * 32767.0);
}

size_t videorec_audio_render(st_VIDEOREC_AUDIO *a, uint64_t horizon_ticks, int16_t *out, size_t max_frames)
{
    size_t done = 0;
    while (done < max_frames) {
        uint64_t s = sample_start(a, a->n);
        uint64_t e = sample_start(a, a->n + 1);
        if (e > horizon_ticks) break;

        double yl = 0.0, yr = 0.0;
        for (unsigned c = 0; c < a->channels; c++) {
            st_VIDEOREC_AUDIO_QUEUE *q = &a->q[c];
            uint64_t t = s;
            uint8_t v = a->value[c];
            double sum = 0.0;
            while (q->count && q->ev[q->head].ticks < e) {
                uint64_t et = q->ev[q->head].ticks < t ? t : q->ev[q->head].ticks;
                sum += a->level[c][v] * (double)(et - t);
                t = et;
                v = q->ev[q->head].value;
                q->head++; q->count--;
            }
            sum += a->level[c][v] * (double)(e - t);
            a->value[c] = v;
            double x = sum / (double)(e - s);
            if (a->sdl_chain) {
                /* v = hodnota na konci vzorku (parkování, obdoba end_value v SDL cestě);
                 * filtr běží vždy, i u kanálu, který se právě nemixuje */
                x = (c == 0) ? videorec_audio_chain_ctc0(&a->flt[c], &a->coef, (float)x, v)
                             : videorec_audio_chain_psg(&a->flt[c], &a->coef, (float)x, v);
            }
            if (a->route[c] & VIDEOREC_AUDIO_ROUTE_L) yl += x;
            if (a->route[c] & VIDEOREC_AUDIO_ROUTE_R) yr += x;
        }

        out[done * 2] = mix_to_s16(a, yl);
        out[done * 2 + 1] = mix_to_s16(a, yr);
        a->n++;
        done++;
    }
    return done;
}
