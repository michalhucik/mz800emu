/**
 * @file   test_videorec_audio_sdl_parity.c
 * @brief  Parita zvuku video záznamu se SDL cestou emulátoru (iface_audio).
 *
 * Funkce SDL cesty (src/iface/iface_audio_resampler.c, src/iface/iface_audio.c)
 * nejdou v testu přímo zkompilovat: stavy filtrů jsou `static` uvnitř funkcí
 * (nejdou resetovat mezi testy) a soubory závisí na iface/SDL/glib a globálním
 * g_iface_audio. Test proto obsahuje **referenční kopii jejich matematiky**
 * (vzor test_psg_scope.c) s odkazy na řádky; jediná změna proti originálu je
 * přesun `static` stavů do struktury a nahrazení g_malloc za pole. Pokud se
 * SDL cesta změní, je nutné kopii aktualizovat.
 *
 * Dvě úrovně parity (obě při 44 100 Hz, kde SDL cesta běží), pro MZ-800
 * (mono, 5 kanálů), MZ-1500 (stereo, 9 kanálů, mix
 * iface_audio_mix_channels_stereo()), MZ-800 s druhým PSG (stereo)
 * a MZ-700 NTSC (jen CTC0, 60 snímků/s):
 * 1. **Řetězec filtrů** (parkování -> low-pass -> anti-glitch u CTC0,
 *    parkování -> IIR u PSG) se stejným vstupem po vzorcích: výstup musí být
 *    bit po bitu shodný (tolerance 0) - stejné operace ve float ve stejném pořadí.
 * 2. **Celá cesta** ze stejných událostí (syntetický audio log v taktech
 *    dané platformy): liší se jen vstupní převzorkování - renderer průměruje úroveň
 *    přes interval vzorku (box filtr, přesný čas), SDL cesta bere bodové
 *    vzorky (PSG; krok `log_count / 882` celočíselně, tj. 401 místo 401,85
 *    taktu - časová osa se uvnitř snímku zkracuje až o cca 1,9 vzorku a na
 *    začátku snímku se srovná) a u CTC0 podíl nenulových vzorků 553,8 kHz
 *    proudu dělený `(délka bloku + 1)` (zesílení cca 0,926; renderer ho
 *    nahrazuje průměrnou korekcí videorec_audio_sdl_ctc0_gain()). Rozdíly jsou
 *    proto jen na hranách obdélníků (posun hrany o zlomek až cca 2 vzorky),
 *    ne v úrovních ani v časování parkování. Tolerance viz jednotlivé asserty.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/videorec/videorec_audio.h"

/* ================================================================
 * Parametry platforem (paritní k <arch>_gdgclk.h, <arch>_video*.h, iface_audio.h;
 * skutečné hodnoty maker hlídá test_videorec_platform.c)
 * ================================================================ */

/** @brief Parametry SDL cesty jedné platformy pro paritní test. */
typedef struct {
    const char *name;  /**< Popis do výpisu. */
    uint64_t clk;      /**< GDGCLK_BASE = VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC. */
    uint64_t tpf;      /**< VIDEO_SCREEN_TICKS. */
    unsigned fps;      /**< VIDEO_SCREENS_PER_SEC. */
    unsigned ctc_div;  /**< GDGCLK_CTC0_DIVIDER. */
    unsigned ch;       /**< Počet kanálů v logu (AUDIO_SRC_CHANNELS_COUNT; MZ-800 bez druhého PSG jen 5). */
    bool stereo;       /**< Mix iface_audio_mix_channels_stereo() (jinak _with_gain()). */
} plat_t;

/** MZ-800: PAL 50 Hz, druhý PSG vypnutý (mono mix CTC0 + PSG0). */
static const plat_t PLAT_MZ800 = { "MZ-800", 17721600ull, 354432ull, 50, 16, 5, false };
/** MZ-1500: NTSC 60 Hz, dva PSG (stereo mix). */
static const plat_t PLAT_MZ1500 = { "MZ-1500", 14336640ull, 238944ull, 60, 16, 9, true };
/** MZ-700 NTSC: 60 Hz, jen CTC0 (dělička CTC0 13). */
static const plat_t PLAT_MZ700N = { "MZ-700 NTSC", 14336640ull, 238944ull, 60, 13, 1, false };

/** Platforma aktuálního testu (nastavuje každý test, výchozí MZ-800 v setUp()). */
static const plat_t *P = &PLAT_MZ800;

#define T_CLK (P->clk)                 /* GDGCLK_BASE */
#define T_TPF (P->tpf)                 /* VIDEO_SCREEN_TICKS */
#define T_FPS (P->fps)                 /* VIDEO_SCREENS_PER_SEC */
#define T_RATE 44100u                  /* IFACE_AUDIO_SAMPLE_RATE */
#define T_SPF (T_RATE / T_FPS)         /* 882 / 735, iface_audio.c:220 */
/* IFACE_AUDIO_CTC5253_SAMPLE_RATE / fps (celočíselně jako iface_audio.h:22 a iface_audio.c:230):
 * MZ-800 11076, MZ-1500 7467, MZ-700 NTSC 551409 / 60 = 9190 */
#define T_CTC_N ((size_t)((T_CLK / (P->ctc_div * 2)) / T_FPS))
#define T_PARK ((T_RATE / 1000) * 45)  /* iface_audio.c:224 */
#define T_FADE ((T_RATE / 1000) * 20)  /* iface_audio.c:226 */
#define T_CH (P->ch)                   /* počet kanálů logu */
#define T_PSG_CH 4                     /* PSG_CHANNELS_COUNT */

#define T_CH_MAX 9                     /* CTC0 + 2x4 PSG */
#define T_SPF_MAX 882                  /* největší T_SPF (50 snímků/s) */
#define T_CTC_MAX 11076                /* největší T_CTC_N (MZ-800) */
#define T_FRAMES_MAX 100               /* nejdelší scénář [snímky] */

/* ================================================================
 * Referenční kopie SDL cesty
 * ================================================================ */

/** Kopie st_AUDIO_LOG_EVENT / st_AUDIO_SOURCE_LOG (audio.h) pro jeden kanál a snímek. */
typedef struct { uint32_t count_ticks; uint8_t value; } ref_event_t;
typedef struct { uint8_t last_value; ref_event_t samples[4096]; unsigned samples_count; } ref_src_t;

/** Stavy, které SDL cesta drží ve `static` proměnných (přesunuto do struktury). */
typedef struct {
    float lp_prev[T_CH_MAX];                       /* iface_audio_lowpass_filter, :81 */
    float ag_prev[T_CH_MAX];                       /* iface_audio_anti_glitch_filter, :159 */
    uint8_t ctc_park_prev; size_t ctc_park_cnt; float ctc_park_gain; int ctc_park_gcnt; /* :230-233 */
    uint8_t psg_park_prev[T_CH_MAX]; size_t psg_park_cnt[T_CH_MAX]; float psg_park_gain[T_CH_MAX]; int psg_park_gcnt[T_CH_MAX]; /* :345-348 */
    float psg_iir[T_CH_MAX];                       /* :385 */
} ref_state_t;

/** Kopie iface_audio_resampler_process_audio_log() (iface_audio_resampler.c:29-70). */
static void ref_process_audio_log(const ref_src_t *source, uint64_t log_count_samples, size_t samples_count, uint8_t *samples)
{
    if (source->samples_count == 0) {
        memset(samples, source->last_value, samples_count);
        return;
    }
    uint64_t step = log_count_samples / samples_count;
    uint64_t current_time = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;
    for (size_t i = 0; i < samples_count; i++, current_time += step) {
        while (current_time >= (total_time - 1)) {
            if (event_index < source->samples_count) {
                event_index++;
                total_time += source->samples[event_index].count_ticks;
            } else {
                total_time = log_count_samples;
                break;
            }
        }
        samples[i] = source->samples[event_index].value;
    }
}

/** Kopie iface_audio_lowpass_filter() (iface_audio_resampler.c:79-86). */
static inline float ref_lowpass(ref_state_t *s, int channel, float input)
{
    float alpha = 0.4f;
    s->lp_prev[channel] = alpha * input + (1.0f - alpha) * s->lp_prev[channel];
    return s->lp_prev[channel];
}

/** Kopie iface_audio_anti_glitch_filter() (iface_audio_resampler.c:157-165). */
static inline float ref_anti_glitch(ref_state_t *s, int channel, float input)
{
    float smooth_factor = 0.2f;
    float output = s->ag_prev[channel] + smooth_factor * (input - s->ag_prev[channel]);
    s->ag_prev[channel] = output;
    return output;
}

/** Kopie parkování CTC0 (iface_audio_resampler.c:229-266). */
static inline float ref_ctc_park(ref_state_t *s, float resampled, uint8_t end_value, size_t parked_samples, size_t parked_fade)
{
    if ((end_value == 0) || (end_value != s->ctc_park_prev)) {
        s->ctc_park_prev = end_value;
        s->ctc_park_cnt = 0;
    } else {
        if (s->ctc_park_cnt < parked_samples) {
            s->ctc_park_cnt++;
            s->ctc_park_gain = 1.0f;
            s->ctc_park_gcnt = 0;
        } else {
            resampled *= s->ctc_park_gain;
            if (s->ctc_park_gain > 0.0f) {
                if (s->ctc_park_gcnt-- == 0) {
                    s->ctc_park_gain -= 1.0f / 20;
                    s->ctc_park_gcnt = parked_fade / 20;
                }
            } else {
                s->ctc_park_gain = 0.0f;
            }
        }
    }
    return resampled;
}

/** Řetězec CTC0 po převzorkování (iface_audio_resampler.c:229-288): parkování, low-pass, anti-glitch. */
static float ref_ctc_chain(ref_state_t *s, float resampled, uint8_t end_value)
{
    resampled = ref_ctc_park(s, resampled, end_value, T_PARK, T_FADE);
    float filtered1 = ref_lowpass(s, 0, resampled);
    return ref_anti_glitch(s, 0, filtered1);
}

/** Řetězec PSG po převzorkování (iface_audio_resampler.c:342-387): parkování, IIR /6. */
static float ref_psg_chain(ref_state_t *s, int channel, float resampled, uint8_t end_value)
{
    if ((end_value == 0) || (end_value != s->psg_park_prev[channel])) {
        s->psg_park_prev[channel] = end_value;
        s->psg_park_cnt[channel] = 0;
    } else {
        if (s->psg_park_cnt[channel] < T_PARK) {
            s->psg_park_cnt[channel]++;
            s->psg_park_gain[channel] = 1.0f;
            s->psg_park_gcnt[channel] = 0;
        } else {
            resampled *= s->psg_park_gain[channel];
            if (s->psg_park_gain[channel] > 0.0f) {
                if (s->psg_park_gcnt[channel]-- == 0) {
                    s->psg_park_gain[channel] -= 1.0f / 20;
                    s->psg_park_gcnt[channel] = T_FADE / 20;
                }
            } else {
                s->psg_park_gain[channel] = 0.0f;
            }
        }
    }
    float iir_x = 6.0f;
    s->psg_iir[channel] = s->psg_iir[channel] + (resampled - s->psg_iir[channel]) / iir_x;
    return s->psg_iir[channel];
}

/** Kopie iface_audio_resampler_output_stream_ctc0() (iface_audio_resampler.c:193-296). */
static void ref_output_stream_ctc0(ref_state_t *s, const uint8_t *input_samples, size_t input_samples_count,
                                   size_t output_samples_count, float *output_samples)
{
    double ratio = (double)input_samples_count / (double)output_samples_count;
    for (size_t i = 0; i < output_samples_count; i++) {
        double index = i * ratio;
        size_t start = (size_t)index;
        size_t end = (size_t)((i + 1) * ratio);
        if (end >= input_samples_count) end = input_samples_count - 1;
        uint8_t end_value = input_samples[end];
        size_t ones_count = 0;
        for (size_t j = start; j < end; j++)
            if (input_samples[j]) ones_count++;
        float resampled = (float)ones_count / (float)(end - start + 1);
        output_samples[i] = ref_ctc_chain(s, resampled, end_value);
    }
}

/** Kopie iface_audio_resampler_process_psg_audio_log() (iface_audio_resampler.c:298-417). */
static void ref_process_psg(ref_state_t *s, int channel, const ref_src_t *source, uint64_t log_count_samples,
                            const float *volume, size_t samples_count, float *samples)
{
    if (source->samples_count == 0) {
        for (size_t i = 0; i < samples_count; i++) samples[i] = volume[source->last_value];
        return;
    }
    uint64_t step = log_count_samples / samples_count;
    uint64_t current_time = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;
    for (size_t i = 0; i < samples_count; i++, current_time += step) {
        while (current_time >= (total_time - 1)) {
            if (event_index < source->samples_count) {
                event_index++;
                total_time += source->samples[event_index].count_ticks;
            } else {
                total_time = log_count_samples;
                break;
            }
        }
        uint8_t current_value = source->samples[event_index].value;
        float resampled = volume[current_value];
        samples[i] = ref_psg_chain(s, channel, resampled, current_value);
    }
}

/** Kopie iface_audio_mix_channels_with_gain() (iface_audio.c:66-101), bez měření úrovní; PSG jen při HAVE_PSG >= 1. */
static void ref_mix(float ch[T_CH_MAX][T_SPF_MAX], const float *gain, float *output)
{
    for (size_t i = 0; i < T_SPF; i++) {
        float sample_ctc0 = ch[0][i] * gain[0];
        float sample_psg0 = 0.0f;
        if (T_CH > 1) { /* #if HAVE_PSG >= 1 */
            for (size_t c = 1; c < 1 + T_PSG_CH; c++) sample_psg0 += ch[c][i] * gain[c];
            sample_psg0 /= (float)T_PSG_CH;
        }
        float sample_max = sample_ctc0 + sample_psg0;
        output[i] = (sample_max <= 1) ? sample_max : 1.0f;
    }
}

/** Kopie iface_audio_mix_channels_stereo() (iface_audio.c:103-155), bez měření úrovní; výstup prokládaně L, R. */
static void ref_mix_stereo(float ch[T_CH_MAX][T_SPF_MAX], const float *gain, float *output)
{
    for (size_t i = 0; i < T_SPF; i++) {
        float sample_ctc0 = ch[0][i] * gain[0];
        float sample_psg0 = 0.0f;
        for (size_t c = 1; c < (1 + T_PSG_CH); c++) sample_psg0 += ch[c][i] * gain[c];
        sample_psg0 /= (float)T_PSG_CH;
        float sample_psg1 = 0.0f;
        for (size_t c = (1 + T_PSG_CH); c < T_CH; c++) sample_psg1 += ch[c][i] * gain[c];
        sample_psg1 /= (float)T_PSG_CH;
        float left = sample_ctc0 + sample_psg0;
        float right = sample_ctc0 + sample_psg1;
        output[i * 2] = (left <= 1.0f) ? left : 1.0f;
        output[i * 2 + 1] = (right <= 1.0f) ? right : 1.0f;
    }
}

/* ================================================================
 * Syntetický zdroj událostí
 * ================================================================ */

/** Jedna změna hodnoty kanálu v absolutním čase. */
typedef struct { uint64_t t; uint8_t ch; uint8_t v; } ev_t;

/** Pole změn hodnot kanálů (realloc, roste po 2x), `s_nev` platných z `s_cap`. */
static ev_t *s_ev;
static size_t s_nev, s_cap;

/** Přidá změnu hodnoty kanálu. @param t Čas [takty GDG]. @param ch Kanál. @param v Hodnota 0..15. */
static void add_ev(uint64_t t, uint8_t ch, uint8_t v)
{
    if (s_nev == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 4096;
        s_ev = realloc(s_ev, s_cap * sizeof(*s_ev));
    }
    s_ev[s_nev++] = (ev_t){ t, ch, v };
}

/** qsort komparátor: podle času, pak podle kanálu. @return <0, 0, >0. */
static int ev_cmp(const void *a, const void *b)
{
    const ev_t *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return (int)x->ch - (int)y->ch;
}

/** Obdélník na kanálu `ch` v intervalu [from, to) s půlperiodou `half` taktů, hodnoty v / 0; na konci 0. */
static void square(uint8_t ch, uint64_t from, uint64_t to, uint64_t half, uint8_t v)
{
    unsigned k = 0;
    for (uint64_t t = from; t < to; t += half, k++) add_ev(t, ch, (k & 1) ? 0 : v);
    add_ev(to, ch, 0);
}

/**
 * Scénář 2 s (100 snímků): CTC0 obdélník 1 kHz, pak trvale 1 (parkování, snímky
 * 30-50), ticho, znovu obdélník 2 kHz; PSG1 tón 440 Hz hlasitost 13 (mimo snímky
 * 28-52, aby plató CTC0 bylo čisté); PSG2 tón 1,1 kHz hlasitost 11 (snímky 5-25);
 * PSG3 nepravidelné změny v krocích PSG děličky (80 taktů) ve snímcích 52-90.
 * Hrany CTC0 jsou na násobcích 32 taktů (perioda 553,8 kHz proudu), PSG na
 * násobcích 80. Žádná událost neleží přesně na začátku snímku a žádný kanál PSG
 * nedrží nenulovou hodnotu přes celý snímek - obojí jsou známé odchylky SDL
 * cesty, které renderer záměrně nekopíruje (samostatné testy níže).
 */
static void build_scenario(void)
{
    s_nev = 0;
    square(0, 160, 30 * T_TPF + 160, 8864, 15);         /* ~1 kHz */
    add_ev(30 * T_TPF + 320, 0, 15);                    /* trvale 1 -> parkování */
    add_ev(50 * T_TPF + 320, 0, 0);                     /* ticho */
    square(0, 60 * T_TPF + 160, 100 * T_TPF - 3200, 4384, 15); /* ~2 kHz */
    square(1, 80, 28 * T_TPF + 80, 20160, 13);          /* ~440 Hz (mimo plató CTC0) */
    square(1, 52 * T_TPF + 80, 100 * T_TPF - 800, 20160, 13);
    square(2, 5 * T_TPF + 80, 25 * T_TPF + 80, 8080, 11); /* ~1,1 kHz */
    uint32_t lfsr = 0xACE1u;
    for (uint64_t t = 52 * T_TPF + 80; t < 90 * T_TPF; t += 80 * 37) {
        lfsr = (lfsr >> 1) ^ (uint32_t)(-(int32_t)(lfsr & 1u) & 0xB400u);
        add_ev(t, 3, (lfsr & 1u) ? 9 : 0);
    }
    add_ev(90 * T_TPF + 80, 3, 0);
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    for (size_t i = 0; i < s_nev; i++) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, s_ev[i].t % T_TPF, "event on a frame start");
}

/** Hlasitostní tabulka PSG (iface_audio.c:271-282 pro F32, volume 100). */
static void psg_volume(float vol[VIDEOREC_AUDIO_LEVELS])
{
    for (int v = 0; v < VIDEOREC_AUDIO_LEVELS; v++) {
        if (v == 0) vol[v] = 0;
        else if (v == 15) vol[v] = (float)(((float)1 / 100) * 100);
        else vol[v] = (float)((((float)1 / 100) * 100) * pow(10, -((float)(15 - v) / 10)));
    }
}

/**
 * Výstup SDL cesty (reference) pro celý scénář: `frames * T_SPF` vzorků int16
 * levé (`out`) a pravé strany (`out_r`, nebo NULL). Mono mix jde do obou
 * stran stejně (iface_audio_wait_for_data() ho duplikuje do L = R).
 */
static void run_sdl(unsigned frames, const float *gain, int16_t *out, int16_t *out_r)
{
    static ref_state_t s;
    memset(&s, 0, sizeof(s));
    float vol[VIDEOREC_AUDIO_LEVELS];
    psg_volume(vol);
    static ref_src_t src[T_CH_MAX];
    static uint8_t ctc[T_CTC_MAX];
    static float chs[T_CH_MAX][T_SPF_MAX];
    float mix[2 * T_SPF_MAX];
    uint8_t cur[T_CH_MAX] = { 0 };
    size_t e = 0;
    for (unsigned f = 0; f < frames; f++) {
        uint64_t f0 = f * T_TPF, f1 = f0 + T_TPF;
        /* Log snímku jako audio_changed() / audiolog_finish_20ms_frame(): samples[0] = hodnota
         * před první změnou, count_ticks = trvání; žádná změna -> samples_count 0, last_value. */
        uint64_t last_ts[T_CH_MAX];
        for (unsigned c = 0; c < T_CH; c++) { src[c].samples_count = 0; src[c].last_value = cur[c]; last_ts[c] = f0; }
        for (; e < s_nev && s_ev[e].t < f1; e++) {
            ref_src_t *sr = &src[s_ev[e].ch];
            if (sr->last_value == s_ev[e].v) continue;
            if (sr->samples_count == 0) { sr->samples[0].value = sr->last_value; sr->samples_count = 1; }
            sr->samples[sr->samples_count - 1].count_ticks = (uint32_t)(s_ev[e].t - last_ts[s_ev[e].ch]);
            last_ts[s_ev[e].ch] = s_ev[e].t;
            sr->samples[sr->samples_count].value = s_ev[e].v;
            sr->samples[sr->samples_count].count_ticks = 0;
            sr->samples_count++;
            sr->last_value = s_ev[e].v;
            cur[s_ev[e].ch] = s_ev[e].v;
        }
        for (unsigned c = 0; c < T_CH; c++) {
            if (src[c].samples_count) src[c].samples[src[c].samples_count - 1].count_ticks = (uint32_t)(f1 - last_ts[c]);
            src[c].samples[src[c].samples_count].count_ticks = 0; /* zarážka (originál ji nečte, viz hlavička) */
            src[c].samples[src[c].samples_count].value = src[c].last_value;
        }
        ref_process_audio_log(&src[0], T_TPF, T_CTC_N, ctc);
        ref_output_stream_ctc0(&s, ctc, T_CTC_N, T_SPF, chs[0]);
        for (unsigned c = 1; c < T_CH; c++) ref_process_psg(&s, (int)c, &src[c], T_TPF, vol, T_SPF, chs[c]);
        if (P->stereo) ref_mix_stereo(chs, gain, mix);
        else ref_mix(chs, gain, mix);
        for (unsigned i = 0; i < T_SPF; i++) {
            for (unsigned side = 0; side < 2; side++) {
                int16_t *o = side ? out_r : out;
                if (!o) continue;
                float y = P->stereo ? mix[i * 2 + side] : mix[i];
                if (y < -1.0f) y = -1.0f;
                o[f * T_SPF + i] = (int16_t)lrint(y * 32767.0);
            }
        }
    }
}

/**
 * Výstup rendereru video záznamu pro celý scénář (stejné úrovně jako
 * iface_audio_build_videorec_levels(), rozložení L/R jako lepidlo podle
 * `P->stereo`): levá strana do `out`, pravá do `out_r` (nebo NULL).
 */
static void run_renderer(unsigned frames, const float *gain, int16_t *out, int16_t *out_r)
{
    float vol[VIDEOREC_AUDIO_LEVELS];
    psg_volume(vol);
    float lv[T_CH_MAX][VIDEOREC_AUDIO_LEVELS];
    float k = videorec_audio_sdl_ctc0_gain(T_CTC_N, T_SPF);
    for (int v = 0; v < VIDEOREC_AUDIO_LEVELS; v++) {
        lv[0][v] = (v ? 1.0f : 0.0f) * gain[0] * k;
        for (unsigned c = 1; c < T_CH; c++) lv[c][v] = vol[v] * gain[c] / (float)T_PSG_CH;
    }
    static st_VIDEOREC_AUDIO a;
    videorec_audio_init(&a, T_CLK, T_RATE, T_CH, (const float (*)[VIDEOREC_AUDIO_LEVELS])lv, NULL, 0, true);
    videorec_audio_set_stereo(&a, P->stereo);
    for (size_t e = 0; e < s_nev; e++) TEST_ASSERT_EQUAL_INT(0, videorec_audio_event(&a, s_ev[e].ch, s_ev[e].v, s_ev[e].t));
    static int16_t st[T_FRAMES_MAX * T_SPF_MAX * 2];
    TEST_ASSERT_EQUAL_size_t(frames * T_SPF, videorec_audio_render(&a, frames * T_TPF, st, frames * T_SPF));
    for (size_t i = 0; i < frames * T_SPF; i++) {
        out[i] = st[i * 2];
        if (out_r) out_r[i] = st[i * 2 + 1];
    }
    videorec_audio_free(&a);
}

void setUp(void) { P = &PLAT_MZ800; }
void tearDown(void) {}

/* ================================================================
 * Testy
 * ================================================================ */

/* 1. Řetězec filtrů: stejný vstup po vzorcích -> bit po bitu stejný výstup (CTC0 i PSG). */
static void test_chain_bit_exact(void)
{
    st_VIDEOREC_AUDIO_COEF coef;
    videorec_audio_coef_init(&coef, T_RATE);
    static ref_state_t s;
    memset(&s, 0, sizeof(s));
    st_VIDEOREC_AUDIO_CHSTATE cc, cp;
    memset(&cc, 0, sizeof(cc));
    memset(&cp, 0, sizeof(cp));
    uint32_t lfsr = 0x1234u;
    unsigned run = 0;
    uint8_t ev = 0;
    float x = 0;
    unsigned mismatch = 0;
    for (unsigned i = 0; i < 200000; i++) {
        /* úseky konstantní hodnoty různé délky (i delší než 45 ms -> parkování) */
        if (run == 0) {
            lfsr = lfsr * 1103515245u + 12345u;
            run = 1 + ((lfsr >> 8) % ((lfsr & 0x10000u) ? 4000u : 40u));
            ev = (uint8_t)((lfsr >> 20) & 0x0Fu);
            x = (float)((lfsr >> 4) & 0xFFu) / 255.0f;
        }
        run--;
        float r1 = ref_ctc_chain(&s, x, ev);
        float r2 = ref_psg_chain(&s, 1, x, ev);
        float n1 = videorec_audio_chain_ctc0(&cc, &coef, x, ev);
        float n2 = videorec_audio_chain_psg(&cp, &coef, x, ev);
        if (memcmp(&r1, &n1, sizeof(float)) != 0 || memcmp(&r2, &n2, sizeof(float)) != 0) mismatch++;
    }
    printf("chain parity: 200000 samples, %u mismatches\n", mismatch);
    TEST_ASSERT_EQUAL_UINT(0, mismatch);
}

/*
 * 2. Celá cesta ze stejných událostí (2 s, MZ-800, 44 100 Hz).
 *
 * Tolerance:
 * - RMS rozdílu <= 5 % RMS signálu a max. |rozdíl| <= 4000 (12 % rozsahu):
 *   rozdíly jsou jen na hranách obdélníků - hrana se proti SDL posune
 *   o zlomek až cca 2 vzorky (bodové vzorkování SDL s krokem 401 místo
 *   401,85 taktu) a box filtr dá na hraně mezihodnotu; obdélník 1-2 kHz má
 *   hranu každých 11-22 vzorků. Naměřeno: RMS 2,3 %, max 2986.
 * - Střední hodnota plató CTC0 před parkováním do 1 % (korekce zesílení CTC0;
 *   naměřeno 24277,4 vs 24277,0).
 * - Začátek a konec útlumu parkování CTC0 do +-3 vzorků (naměřeno shodně).
 *
 * Proměnná prostředí PARITY_DUMP=<soubor> zapíše oba výstupy (int16 LE,
 * prokládaně SDL, renderer) pro ruční analýzu.
 */
static void test_full_path_parity(void)
{
    const unsigned F = 100;
    const float gain[T_CH_MAX] = { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[T_FRAMES_MAX * T_SPF_MAX], ren[T_FRAMES_MAX * T_SPF_MAX];
    build_scenario();
    run_sdl(F, gain, ref, NULL);
    run_renderer(F, gain, ren, NULL);

    double se = 0, sr = 0, mx = 0;
    for (size_t i = 0; i < F * T_SPF; i++) {
        double d = (double)ren[i] - ref[i];
        se += d * d;
        sr += (double)ref[i] * ref[i];
        if (fabs(d) > mx) mx = fabs(d);
    }
    if (getenv("PARITY_DUMP")) {
        FILE *fd = fopen(getenv("PARITY_DUMP"), "wb");
        for (size_t i = 0; i < F * T_SPF; i++) { fwrite(&ref[i], 2, 1, fd); fwrite(&ren[i], 2, 1, fd); }
        fclose(fd);
    }
    double rms_d = sqrt(se / (F * T_SPF)), rms_r = sqrt(sr / (F * T_SPF));
    printf("full path: RMS ref %.1f, RMS diff %.1f (%.1f %%), max |diff| %.0f\n", rms_r, rms_d, 100.0 * rms_d / rms_r, mx);
    TEST_ASSERT_TRUE_MESSAGE(rms_d <= 0.05 * rms_r, "RMS difference above 5 % of the signal");
    TEST_ASSERT_TRUE_MESSAGE(mx <= 4000.0, "max difference above 4000");

    /* Plató CTC0 (snímek 31: trvale 1, ostatní kanály tiché nebo už zaparkované) */
    double m_ref = 0, m_ren = 0;
    for (size_t i = 31 * T_SPF; i < 32 * T_SPF; i++) { m_ref += ref[i]; m_ren += ren[i]; }
    printf("CTC0 plateau mean: SDL %.1f, renderer %.1f\n", m_ref / T_SPF, m_ren / T_SPF);
    TEST_ASSERT_DOUBLE_WITHIN(0.01 * fabs(m_ref), m_ref, m_ren);

    /* Parkování CTC0: první vzorek, kde úroveň klesne pod plató o víc než 3000 (začátek útlumu),
     * a první vzorek pod 3000 (konec útlumu). */
    int on_ref = -1, on_ren = -1, off_ref = -1, off_ren = -1;
    size_t p0 = 30 * T_SPF;
    double plateau_ref = m_ref / T_SPF, plateau_ren = m_ren / T_SPF;
    for (size_t i = p0 + 1000; i < 50 * T_SPF; i++) {
        if (on_ref < 0 && ref[i] < plateau_ref - 3000) on_ref = (int)(i - p0);
        if (on_ren < 0 && ren[i] < plateau_ren - 3000) on_ren = (int)(i - p0);
        if (off_ref < 0 && ref[i] < 3000) off_ref = (int)(i - p0);
        if (off_ren < 0 && ren[i] < 3000) off_ren = (int)(i - p0);
    }
    printf("CTC0 parking: fade starts SDL %d / renderer %d, faded SDL %d / renderer %d (samples after onset)\n",
           on_ref, on_ren, off_ref, off_ren);
    TEST_ASSERT_TRUE(on_ref > 0 && on_ren > 0 && off_ref > 0 && off_ren > 0);
    TEST_ASSERT_INT_WITHIN(3, on_ref, on_ren);
    TEST_ASSERT_INT_WITHIN(3, off_ref, off_ren);
    free(s_ev);
    s_ev = NULL;
    s_cap = s_nev = 0;
}

/** Střední hodnota úseku [from, to) vzorků. */
static double mean16(const int16_t *x, size_t from, size_t to)
{
    double m = 0;
    for (size_t i = from; i < to; i++) m += x[i];
    return m / (double)(to - from);
}

/*
 * Známá odchylka SDL cesty, kterou renderer NEkopíruje (dokumentační test):
 * kanál PSG bez změny v celém snímku jde v SDL cestě přes časnou větev
 * `samples_count == 0` (iface_audio_resampler.c:307-314), která vrátí
 * `volume[last_value]` bez parkování a bez IIR. Trvalá nenulová hodnota PSG
 * se proto v SDL cestě nikdy nezaparkuje (zůstane stejnosměrná úroveň),
 * renderer ji po 45 ms zaparkuje jako CTC0. Rozdíl je jen ve stejnosměrné
 * složce (neslyšitelná, ubírá rezervu před ořezem).
 */
static void test_known_deviation_psg_constant_level(void)
{
    const float gain[T_CH_MAX] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[10 * T_SPF_MAX], ren[10 * T_SPF_MAX];
    s_nev = 0;
    add_ev(80, 2, 10);
    run_sdl(10, gain, ref, NULL);
    run_renderer(10, gain, ren, NULL);
    double lvl = 32767.0 * pow(10, -0.5) / 4.0; /* hlasitost 10 = 10^-0,5, průměr 4 kanálů */
    printf("PSG constant level: SDL frame 5 mean %.1f, renderer %.1f (level %.1f)\n", mean16(ref, 5 * T_SPF, 6 * T_SPF),
           mean16(ren, 5 * T_SPF, 6 * T_SPF), lvl);
    TEST_ASSERT_DOUBLE_WITHIN(2.0, lvl, mean16(ref, 5 * T_SPF, 6 * T_SPF)); /* SDL: trvalá úroveň */
    TEST_ASSERT_DOUBLE_WITHIN(2.0, 0.0, mean16(ren, 5 * T_SPF, 6 * T_SPF)); /* renderer: zaparkováno */
    free(s_ev);
    s_ev = NULL;
    s_cap = s_nev = 0;
}

/*
 * Známá chyba SDL cesty, kterou renderer NEkopíruje (dokumentační test):
 * změna kanálu přesně v prvním taktu logu snímku dá `samples[0].count_ticks == 0`
 * a podmínka `current_time >= (total_time - 1)` (iface_audio_resampler.c:51
 * a :325) pak přeteče (0 - 1 v uint64_t), takže SDL cesta celý snímek drží
 * starou hodnotu kanálu. Kroky PSG v audio.c padají na `last_psg_timestamp`,
 * který je zároveň `first_timestamp` dalšího logu, takže změna PSG v prvním
 * kroku snímku tento stav vyvolá [neověřeno na živém emulátoru].
 */
static void test_known_sdl_bug_event_at_frame_start(void)
{
    const float gain[T_CH_MAX] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[3 * T_SPF_MAX], ren[3 * T_SPF_MAX];
    s_nev = 0;
    square(1, 80, 3 * T_TPF - 800, 20160, 15);
    add_ev(T_TPF, 2, 15); /* PSG2 zapnut přesně na začátku snímku 1 */
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    run_sdl(3, gain, ref, NULL);
    run_renderer(3, gain, ren, NULL);
    /* PSG2 (hlasitost 15, úroveň 0,25) ve snímku 1: SDL ho nepustí, renderer ano; PSG1 v obou */
    double d1 = mean16(ren, T_SPF + 100, 2 * T_SPF) - mean16(ref, T_SPF + 100, 2 * T_SPF);
    printf("event at frame start: renderer - SDL mean in frame 1 = %.1f (PSG2 level %.1f)\n", d1, 32767.0 * 0.25);
    TEST_ASSERT_DOUBLE_WITHIN(400.0, 32767.0 * 0.25, d1);
    free(s_ev);
    s_ev = NULL;
    s_cap = s_nev = 0;
}

/** Uvolní pole událostí scénáře. */
static void free_events(void)
{
    free(s_ev);
    s_ev = NULL;
    s_cap = s_nev = 0;
}

/**
 * Porovná výstup rendereru se SDL cestou se stejnými tolerancemi jako
 * test_full_path_parity() (RMS rozdílu <= 5 % RMS signálu, max <= 4000).
 * @param tag Popis do výpisu. @param ref SDL cesta. @param ren Renderer. @param n Počet vzorků.
 */
static void compare(const char *tag, const int16_t *ref, const int16_t *ren, size_t n)
{
    double se = 0, sr = 0, mx = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)ren[i] - ref[i];
        se += d * d;
        sr += (double)ref[i] * ref[i];
        if (fabs(d) > mx) mx = fabs(d);
    }
    double rms_d = sqrt(se / (double)n), rms_r = sqrt(sr / (double)n);
    printf("%s %s: RMS ref %.1f, RMS diff %.1f (%.1f %%), max |diff| %.0f\n", P->name, tag, rms_r, rms_d,
           100.0 * rms_d / rms_r, mx);
    TEST_ASSERT_TRUE_MESSAGE(rms_r > 100.0, "reference is (almost) silent");
    TEST_ASSERT_TRUE_MESSAGE(rms_d <= 0.05 * rms_r, "RMS difference above 5 % of the signal");
    TEST_ASSERT_TRUE_MESSAGE(mx <= 4000.0, "max difference above 4000");
}

/** RMS rozdílu stran L - R (míra "opravdového" sterea). @param l L. @param r R. @param n Vzorků. @return RMS. */
static double rms_lr(const int16_t *l, const int16_t *r, size_t n)
{
    double d = 0;
    for (size_t i = 0; i < n; i++) {
        double a = (double)l[i] - r[i];
        d += a * a;
    }
    return sqrt(d / (double)n);
}

/*
 * 3. Stereo MZ-1500 (60 snímků/s, 735 vzorků na snímek, 9 kanálů): mix
 *    iface_audio_mix_channels_stereo() - L = CTC0 + PSG0, R = CTC0 + PSG1.
 *    Scénář: CTC0 obdélník ve snímcích 0-20 (do obou stran), PSG0 kanál 1
 *    tón jen vlevo, PSG1 kanál 5 tón jen vpravo, PSG1 kanál 7 nepravidelné
 *    změny vpravo. Hrany na násobcích 320 taktů (perioda CTC proudu 32,
 *    krok PSG děličky MZ-1500 64 taktů) a nikdy na začátku snímku.
 *    Tolerance jako u mono parity (rozdíly jen na hranách). Navíc se ověří,
 *    že se strany opravdu liší (RMS L-R) a že renderer dává stejný rozdíl
 *    stran jako SDL cesta (do 5 %).
 */
static void test_stereo_parity_mz1500(void)
{
    P = &PLAT_MZ1500;
    const unsigned F = 60;
    const float gain[T_CH_MAX] = { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t refl[T_FRAMES_MAX * T_SPF_MAX], refr[T_FRAMES_MAX * T_SPF_MAX];
    static int16_t renl[T_FRAMES_MAX * T_SPF_MAX], renr[T_FRAMES_MAX * T_SPF_MAX];
    s_nev = 0;
    square(0, 320, 20 * T_TPF + 320, 7360, 15);               /* CTC0 ~1 kHz, obě strany */
    square(1, 640, 50 * T_TPF + 640, 16320, 13);              /* PSG0 ~440 Hz, jen L */
    square(5, 10 * T_TPF + 960, 58 * T_TPF + 960, 11200, 12); /* PSG1 ~640 Hz, jen R */
    uint32_t lfsr = 0xBEEFu;
    for (uint64_t t = 30 * T_TPF + 320; t < 55 * T_TPF; t += 320 * 13) {
        lfsr = (lfsr >> 1) ^ (uint32_t)(-(int32_t)(lfsr & 1u) & 0xB400u);
        add_ev(t, 7, (lfsr & 1u) ? 10 : 0);
    }
    add_ev(55 * T_TPF + 320, 7, 0);
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    for (size_t i = 0; i < s_nev; i++) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, s_ev[i].t % T_TPF, "event on a frame start");

    run_sdl(F, gain, refl, refr);
    run_renderer(F, gain, renl, renr);
    size_t n = (size_t)F * T_SPF;
    compare("stereo L", refl, renl, n);
    compare("stereo R", refr, renr, n);

    double dref = rms_lr(refl, refr, n), dren = rms_lr(renl, renr, n);
    printf("MZ-1500 stereo: RMS(L-R) SDL %.1f, renderer %.1f\n", dref, dren);
    TEST_ASSERT_TRUE(dref > 1000.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.05 * dref, dref, dren);
    free_events();
}

/*
 * 4. Mono MZ-700 NTSC (jen CTC0, dělička 13 => proud 551 409 Hz, 9190 vzorků
 *    CTC na snímek, 735 výstupních): obdélník, plató s parkováním, obdélník.
 *    Hrany na násobcích 26 taktů (perioda CTC proudu). L == R v obou cestách.
 */
static void test_mono_parity_mz700_ntsc(void)
{
    P = &PLAT_MZ700N;
    const unsigned F = 60;
    const float gain[T_CH_MAX] = { 0.8f };
    static int16_t refl[T_FRAMES_MAX * T_SPF_MAX], refr[T_FRAMES_MAX * T_SPF_MAX];
    static int16_t renl[T_FRAMES_MAX * T_SPF_MAX], renr[T_FRAMES_MAX * T_SPF_MAX];
    s_nev = 0;
    square(0, 260, 20 * T_TPF + 260, 26 * 170, 15);               /* ~1,6 kHz */
    add_ev(20 * T_TPF + 520, 0, 15);                              /* trvale 1 -> parkování */
    add_ev(35 * T_TPF + 520, 0, 0);
    square(0, 40 * T_TPF + 260, 60 * T_TPF - 2600, 26 * 340, 15); /* ~0,8 kHz */
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    for (size_t i = 0; i < s_nev; i++) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, s_ev[i].t % T_TPF, "event on a frame start");

    run_sdl(F, gain, refl, refr);
    run_renderer(F, gain, renl, renr);
    size_t n = (size_t)F * T_SPF;
    compare("mono", refl, renl, n);
    TEST_ASSERT_EQUAL_INT16_ARRAY(refl, refr, n);
    TEST_ASSERT_EQUAL_INT16_ARRAY(renl, renr, n);
    free_events();
}

/*
 * 5. MZ-800 s druhým PSG (allow_psg1): stereo mix 9 kanálů při 50 snímcích/s
 *    (stejná matematika jako MZ-1500, jiné takty).
 */
static void test_stereo_parity_mz800_psg1(void)
{
    static const plat_t mz800_stereo = { "MZ-800 + PSG1", 17721600ull, 354432ull, 50, 16, 9, true };
    P = &mz800_stereo;
    const unsigned F = 40;
    const float gain[T_CH_MAX] = { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t refl[T_FRAMES_MAX * T_SPF_MAX], refr[T_FRAMES_MAX * T_SPF_MAX];
    static int16_t renl[T_FRAMES_MAX * T_SPF_MAX], renr[T_FRAMES_MAX * T_SPF_MAX];
    s_nev = 0;
    square(0, 160, 10 * T_TPF + 160, 8864, 15);
    square(2, 80, 30 * T_TPF + 80, 20160, 13);  /* PSG0 jen L */
    square(6, 400, 38 * T_TPF + 400, 8080, 11); /* PSG1 jen R */
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    run_sdl(F, gain, refl, refr);
    run_renderer(F, gain, renl, renr);
    size_t n = (size_t)F * T_SPF;
    compare("stereo L", refl, renl, n);
    compare("stereo R", refr, renr, n);
    TEST_ASSERT_TRUE(rms_lr(renl, renr, n) > 1000.0);
    free_events();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chain_bit_exact);
    RUN_TEST(test_full_path_parity);
    RUN_TEST(test_known_deviation_psg_constant_level);
    RUN_TEST(test_known_sdl_bug_event_at_frame_start);
    RUN_TEST(test_stereo_parity_mz1500);
    RUN_TEST(test_mono_parity_mz700_ntsc);
    RUN_TEST(test_stereo_parity_mz800_psg1);
    return UNITY_END();
}
