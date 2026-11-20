/**
 * @file   videorec_audio.c
 * @brief  Renderer audio událostí na PCM v emulačním čase (box filtr + řetězec SDL cesty).
 *
 * Řetězec filtrů je kopie aktivní SDL cesty emulátoru (viz videorec_audio.h):
 * - CTC0: iface_audio_resampler_output_stream_ctc0() (iface_audio_resampler.c),
 *   tj. střídavá vazba -> iface_audio_lowpass_filter() (alpha 0,4)
 *   -> iface_audio_anti_glitch_filter() (0,2); celý řetězec je sdílená funkce
 *   videorec_audio_chain_ctc0(), kterou SDL cesta volá také;
 * - PSG: iface_audio_resampler_process_psg_audio_log(), tj. hlasitostní
 *   tabulka (v rendereru zapečená do `level`) -> parkování -> IIR
 *   `sample += (x - sample) / 6`;
 * - výstupní stupeň: videorec_audio_output_stage() (sdílený s mixem SDL cesty);
 * - mix: iface_audio_mix_channels_with_gain() (iface_audio.c, mono):
 *   CTC0 * gain * iface_audio_ctc0_mix_level() + průměr PSG0 kanálů * gain;
 *   iface_audio_mix_channels_stereo() (iface_audio.c, stereo):
 *   L = CTC0 + průměr PSG0, R = CTC0 + průměr PSG1; každá strana má vlastní
 *   výstupní stupeň s dorovnáním hlasitosti a měkkým omezovačem
 *   (rozložení kanálů viz videorec_audio_set_stereo()). Poměr CTC0 k PSG je
 *   v rendereru zapečený do `level` (iface_audio_build_videorec_levels()).
 * SDL cesta drží stavy filtrů ve `static` proměnných; zde jsou v instanci
 * (st_VIDEOREC_AUDIO_CHSTATE), aby šly uložit a obnovit (plynulý retake).
 *
 * Trvalou úroveň CTC0 odstraňuje střídavá vazba (časová konstanta 0,85 s
 * podle měření na MZ-800), u PSG parkování, obojí stejně jako v SDL cestě.
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
    c->ctc_ac_k = 1.0 - exp(-1000.0 / ((double)VIDEOREC_AUDIO_CTC0_AC_TAU_MS * (double)rate));
    c->out_lp_k = 1.0 - exp(-1.0e6 / ((double)VIDEOREC_AUDIO_OUT_LP_TAU_US * (double)rate));
    c->out_xfade_step = 1000.0f / ((float)VIDEOREC_AUDIO_OUT_XFADE_MS * (float)rate);
    c->out_ac_k = 0.0;
    c->psg_park = true;
    c->out_makeup_off = (float)pow(10.0, VIDEOREC_AUDIO_OUT_MAKEUP_OFF_DB / 20.0);
    c->out_makeup_on = (float)pow(10.0, VIDEOREC_AUDIO_OUT_MAKEUP_ON_DB / 20.0);
    c->out_knee = VIDEOREC_AUDIO_OUT_KNEE;
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

float videorec_audio_chain_ctc0(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, float unit, float *raw)
{
    float r = x;
    if (unit > 0.0f) {
        /* střídavá vazba: střední hodnota v jednotkách 0..1, výstup kolem nuly */
        double x01 = (double)x / (double)unit;
        s->hp_mean += (x01 - s->hp_mean) * c->ctc_ac_k;
        r = (float)((double)x - s->hp_mean * (double)unit);
    }
    if (raw) *raw = r;
    float alpha = c->ctc_lp_alpha;
    s->lp = alpha * r + (1.0f - alpha) * s->lp;          /* iface_audio_lowpass_filter() */
    s->ag = s->ag + c->ctc_ag_factor * (s->lp - s->ag);  /* iface_audio_anti_glitch_filter() */
    return s->ag;
}

void videorec_audio_coef_set_output_coupling(st_VIDEOREC_AUDIO_COEF *c, unsigned rate, bool on)
{
    c->out_ac_k = on ? (1.0 - exp(-1000.0 / ((double)VIDEOREC_AUDIO_OUT_AC_TAU_MS * (double)rate))) : 0.0;
    c->psg_park = !on;
}

float videorec_audio_chain_psg(st_VIDEOREC_AUDIO_CHSTATE *s, const st_VIDEOREC_AUDIO_COEF *c, float x, uint8_t end_value, float *raw)
{
    float r = c->psg_park ? vr_park(&s->park, c, x, end_value) : x;
    if (raw) *raw = r;
    s->lp = s->lp + (r - s->lp) / c->psg_iir_div;        /* IIR PSG v iface_audio_resampler_process_psg_audio_log() */
    return s->lp;
}

float videorec_audio_output_stage(const st_VIDEOREC_AUDIO_COEF *c, double *ac, double *lp, float legacy, float raw, float gain)
{
    /* střídavá vazba výstupu: střední hodnota součtu se odečte od obou variant
     * (při out_ac_k == 0 a *ac == 0 beze změny) */
    *ac += ((double)raw - *ac) * c->out_ac_k;
    legacy -= (float)*ac;
    raw -= (float)*ac;
    *lp += ((double)raw - *lp) * c->out_lp_k;
    float y = legacy + gain * ((float)*lp - legacy);
    /* dorovnání hlasitosti (přechází s vahou propusti) a měkký omezovač špiček */
    y *= c->out_makeup_off + gain * (c->out_makeup_on - c->out_makeup_off);
    return videorec_audio_soft_limit_knee(y, c->out_knee);
}

void videorec_audio_coef_set_output_level(st_VIDEOREC_AUDIO_COEF *c, float makeup_off, float makeup_on, float knee)
{
    c->out_makeup_off = makeup_off;
    c->out_makeup_on = makeup_on;
    c->out_knee = knee;
}

float videorec_audio_output_gain_step(const st_VIDEOREC_AUDIO_COEF *c, float gain, bool on)
{
    if (on) {
        gain += c->out_xfade_step;
        if (gain > 1.0f) gain = 1.0f;
    } else {
        gain -= c->out_xfade_step;
        if (gain < 0.0f) gain = 0.0f;
    }
    return gain;
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
    /* střídavá vazba CTC0 začíná ustálená na počáteční hodnotě: úroveň
     * hodnoty v jednotkách úrovně "jedničky" (hodnota 31 dá 0,64) */
    a->flt[0].hp_mean = 0.0;
    if (a->channels && a->value[0]) {
        float unit = a->level[0][VIDEOREC_AUDIO_CTC0_ON_VALUE];
        double m = (unit > 0.0f) ? ((double)a->level[0][a->value[0]] / (double)unit) : 1.0;
        a->flt[0].hp_mean = (m < 0.0) ? 0.0 : ((m > 1.0) ? 1.0 : m);
    }
}

void videorec_audio_set_output_filter(st_VIDEOREC_AUDIO *a, bool on, bool immediate)
{
    a->out_filter = on;
    if (immediate) a->out_gain = on ? 1.0f : 0.0f;
}

void videorec_audio_set_output_coupling(st_VIDEOREC_AUDIO *a, bool on)
{
    videorec_audio_coef_set_output_coupling(&a->coef, a->rate, on);
}

void videorec_audio_set_output_mean(st_VIDEOREC_AUDIO *a, double l, double r)
{
    a->out_ac[0] = l;
    a->out_ac[1] = r;
}

void videorec_audio_set_ctc0_mean(st_VIDEOREC_AUDIO *a, double mean01)
{
    if (mean01 < 0.0) mean01 = 0.0;
    if (mean01 > 1.0) mean01 = 1.0;
    a->flt[0].hp_mean = mean01;
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
        a->value[c] = values ? (uint8_t)(values[c] & VIDEOREC_AUDIO_VALUE_MASK) : 0;
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
    out->out_lp[0] = a->out_lp[0];
    out->out_lp[1] = a->out_lp[1];
    out->out_ac[0] = a->out_ac[0];
    out->out_ac[1] = a->out_ac[1];
    out->out_gain = a->out_gain;
    out->out_filter = a->out_filter;
}

void videorec_audio_set_state(st_VIDEOREC_AUDIO *a, const st_VIDEOREC_AUDIO_STATE *st)
{
    for (unsigned c = 0; c < a->channels; c++) {
        a->value[c] = (uint8_t)(st->value[c] & VIDEOREC_AUDIO_VALUE_MASK);
        a->flt[c] = st->ch[c];
    }
    a->out_lp[0] = st->out_lp[0];
    a->out_lp[1] = st->out_lp[1];
    a->out_ac[0] = st->out_ac[0];
    a->out_ac[1] = st->out_ac[1];
    a->out_gain = st->out_gain;
    a->out_filter = st->out_filter;
}

int videorec_audio_event(st_VIDEOREC_AUDIO *a, unsigned ch, uint8_t value, uint64_t ticks)
{
    if (ch >= a->channels) return -1;
    return queue_push(&a->q[ch], ticks, (uint8_t)(value & VIDEOREC_AUDIO_VALUE_MASK));
}

/** @brief Čas (takty) začátku vzorku `n`; počítáno z `n`, aby se zaokrouhlení nekumulovalo. */
static inline uint64_t sample_start(const st_VIDEOREC_AUDIO *a, uint64_t n)
{
    return a->origin + (n * a->clk_hz) / a->rate;
}

/**
 * @brief Převede vzorek jedné strany na int16 (ořez na [-1, 1]; po výstupním stupni se už neuplatní).
 * @param y Vzorek strany.
 * @return Vzorek int16.
 */
static inline int16_t mix_to_s16(double y)
{
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

        double yl = 0.0, yr = 0.0;   /* součet po filtrech kanálů (bez sdl_chain surový součet) */
        double rl = 0.0, rr = 0.0;   /* součet před filtry kanálů (vstup výstupní propusti) */
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
            float raw = (float)x;
            if (a->sdl_chain) {
                /* v = hodnota na konci vzorku (parkování, obdoba end_value v SDL cestě);
                 * filtr běží vždy, i u kanálu, který se právě nemixuje */
                x = (c == 0) ? videorec_audio_chain_ctc0(&a->flt[c], &a->coef, (float)x,
                                                         a->level[c][VIDEOREC_AUDIO_CTC0_ON_VALUE], &raw)
                             : videorec_audio_chain_psg(&a->flt[c], &a->coef, (float)x, v, &raw);
            }
            if (a->route[c] & VIDEOREC_AUDIO_ROUTE_L) { yl += x; rl += raw; }
            if (a->route[c] & VIDEOREC_AUDIO_ROUTE_R) { yr += x; rr += raw; }
        }

        if (a->sdl_chain) {
            /* výstupní stupeň jako mix SDL cesty: střídavá vazba, výstupní propust, plynulý přechod, dorovnání, omezovač */
            a->out_gain = videorec_audio_output_gain_step(&a->coef, a->out_gain, a->out_filter);
            yl = videorec_audio_output_stage(&a->coef, &a->out_ac[0], &a->out_lp[0], (float)yl, (float)rl, a->out_gain);
            yr = videorec_audio_output_stage(&a->coef, &a->out_ac[1], &a->out_lp[1], (float)yr, (float)rr, a->out_gain);
        }
        out[done * 2] = mix_to_s16(yl);
        out[done * 2 + 1] = mix_to_s16(yr);
        a->n++;
        done++;
    }
    return done;
}
