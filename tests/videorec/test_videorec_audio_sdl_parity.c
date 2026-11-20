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
#define T_CTC0_TO_PSG 1.49f            /* AUDIO_CTC0_TO_PSG_CHANNEL_RATIO (audio.h) */
#define T_HALF_FACTOR 0.49f            /* AUDIO_LEVEL_HALF_FACTOR (audio.h) */
#define T_CTC0_AC_TAU_S 0.85           /* AUDIO_CTC0_AC_TAU_MS (audio.h): střídavá vazba CTC0 */
#define T_OUT_LP_TAU_S 460e-6          /* AUDIO_OUT_LP_TAU_US (audio.h): výstupní dolní propust */
#define T_OUT_AC_TAU_S 0.57            /* AUDIO_OUT_AC_TAU_MS (audio.h): střídavá vazba celého výstupu */
#define T_MAKEUP_ON_DB 8.0f            /* AUDIO_OUT_MAKEUP_FILTER_ON_DB (audio.h): dorovnání hlasitosti se zapnutou propustí */
#define T_MAKEUP_OFF_DB 4.0f           /* AUDIO_OUT_MAKEUP_FILTER_OFF_DB (audio.h): dorovnání s vypnutou propustí a bez ní */
#define T_KNEE 0.7f                    /* AUDIO_OUT_LIMIT_KNEE (audio.h): koleno měkkého omezovače */
/* AUDIO_HAVE_OUTPUT_COUPLING: vazbu výstupu mají počítače s PSG (MZ-800, MZ-1500), samostatný MZ-700 ne */
#define T_COUPLING (P->ch > 1)

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
    double ctc_ac_mean;                            /* střídavá vazba CTC0: střední hodnota 0..1 */
    uint8_t psg_park_prev[T_CH_MAX]; size_t psg_park_cnt[T_CH_MAX]; float psg_park_gain[T_CH_MAX]; int psg_park_gcnt[T_CH_MAX]; /* :337-340 */
    float psg_iir[T_CH_MAX];                       /* :377 */
    double out_lp[2];                              /* výstupní propust L, R (iface_audio.c s_out_lp) */
    double out_ac[2];                              /* střídavá vazba výstupu L, R (iface_audio.c s_out_ac) */
} ref_state_t;

/** Volba výstupní propusti pro oba běhy scénáře (SDL cesta i renderer); nastavuje test. */
static bool s_filter_on = false;

/** Kopie iface_audio_resampler_process_audio_log() (iface_audio_resampler.c:29-67). */
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
        while ((current_time + 1 >= total_time) && (event_index + 1 < source->samples_count)) {
            event_index++;
            total_time += source->samples[event_index].count_ticks;
        }
        samples[i] = source->samples[event_index].value;
    }
}

/** Kopie iface_audio_lowpass_filter() (iface_audio_resampler.c:76-83). */
static inline float ref_lowpass(ref_state_t *s, int channel, float input)
{
    float alpha = 0.4f;
    s->lp_prev[channel] = alpha * input + (1.0f - alpha) * s->lp_prev[channel];
    return s->lp_prev[channel];
}

/** Kopie iface_audio_anti_glitch_filter() (iface_audio_resampler.c:154-162). */
static inline float ref_anti_glitch(ref_state_t *s, int channel, float input)
{
    float smooth_factor = 0.2f;
    float output = s->ag_prev[channel] + smooth_factor * (input - s->ag_prev[channel]);
    s->ag_prev[channel] = output;
    return output;
}

/**
 * Řetězec CTC0 po převzorkování (iface_audio_resampler_output_stream_ctc0()):
 * střídavá vazba, low-pass, anti-glitch. Nezávislý zápis vzorce sdílené funkce
 * videorec_audio_chain_ctc0(): střední hodnota v jednotkách 0..1 se blíží
 * k `resampled / unit` s časovou konstantou T_CTC0_AC_TAU_S, výstup vazby je
 * `resampled - mean * unit`.
 */
static float ref_ctc_chain(ref_state_t *s, float resampled, float unit, float *raw)
{
    if (unit > 0.0f) {
        double k = 1.0 - exp(-1.0 / (T_CTC0_AC_TAU_S * (double)T_RATE));
        s->ctc_ac_mean += ((double)resampled / (double)unit - s->ctc_ac_mean) * k;
        resampled = (float)((double)resampled - s->ctc_ac_mean * (double)unit);
    }
    if (raw) *raw = resampled; /* před low-passem: vstup výstupní propusti */
    float filtered1 = ref_lowpass(s, 0, resampled);
    return ref_anti_glitch(s, 0, filtered1);
}

/** Řetězec PSG po převzorkování (iface_audio_resampler.c:334-379): parkování, IIR /6. */
static float ref_psg_chain(ref_state_t *s, int channel, float resampled, uint8_t end_value, float *raw, bool park)
{
    if (!park) {
        /* se střídavou vazbou výstupu se kanály PSG neparkují */
    } else if ((end_value == 0) || (end_value != s->psg_park_prev[channel])) {
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
    if (raw) *raw = resampled; /* po parkování, před IIR: vstup výstupní propusti */
    float iir_x = 6.0f;
    s->psg_iir[channel] = s->psg_iir[channel] + (resampled - s->psg_iir[channel]) / iir_x;
    return s->psg_iir[channel];
}

/** Kopie iface_audio_resampler_output_stream_ctc0() (iface_audio_resampler.c:190-293). */
static void ref_output_stream_ctc0(ref_state_t *s, const uint8_t *input_samples, size_t input_samples_count,
                                   size_t output_samples_count, float *output_samples, float *raw_out)
{
    double ratio = (double)input_samples_count / (double)output_samples_count;
    for (size_t i = 0; i < output_samples_count; i++) {
        double index = i * ratio;
        size_t start = (size_t)index;
        size_t end = (size_t)((i + 1) * ratio);
        if (end >= input_samples_count) end = input_samples_count - 1;
        size_t ones_count = 0;
        for (size_t j = start; j < end; j++)
            if (input_samples[j]) ones_count++;
        float resampled = (float)ones_count / (float)(end - start + 1);
        float unit = (float)(end - start) / (float)(end - start + 1);
        output_samples[i] = ref_ctc_chain(s, resampled, unit, &raw_out[i]);
    }
}

/** Kopie iface_audio_resampler_process_psg_audio_log() (iface_audio_resampler.c:295-409). */
static void ref_process_psg(ref_state_t *s, int channel, const ref_src_t *source, uint64_t log_count_samples,
                            const float *volume, size_t samples_count, float *samples, float *raw_out)
{
    if (source->samples_count == 0) {
        for (size_t i = 0; i < samples_count; i++) raw_out[i] = samples[i] = volume[source->last_value];
        return;
    }
    uint64_t step = log_count_samples / samples_count;
    uint64_t current_time = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;
    for (size_t i = 0; i < samples_count; i++, current_time += step) {
        while ((current_time + 1 >= total_time) && (event_index + 1 < source->samples_count)) {
            event_index++;
            total_time += source->samples[event_index].count_ticks;
        }
        uint8_t current_value = source->samples[event_index].value;
        float resampled = volume[current_value];
        samples[i] = ref_psg_chain(s, channel, resampled, current_value, &raw_out[i], !T_COUPLING);
    }
}

/**
 * Kopie iface_audio_ctc0_mix_level() (iface_audio.c): poměr CTC0 k jednomu
 * kanálu PSG dělený počtem kanálů a středním zesílením převzorkování CTC0;
 * stejně i bez PSG (MZ-700).
 */
static float ref_ctc0_mix_level(void)
{
    return T_CTC0_TO_PSG / (float)T_PSG_CH / videorec_audio_sdl_ctc0_gain(T_CTC_N, T_SPF);
}

/**
 * Výstupní stupeň mixu (nezávislý zápis videorec_audio_output_stage() pro váhu
 * 0 nebo 1): vypnutá propust = součet po filtrech kanálů krát dorovnání
 * T_MAKEUP_OFF_DB; zapnutá = součet před filtry kanálů po dolní propusti
 * 1. řádu T_OUT_LP_TAU_S (propust běží vždy) krát dorovnání T_MAKEUP_ON_DB.
 * Za tím měkký omezovač s kolenem T_KNEE (pod kolenem beze změny, nad ním
 * `knee + (1 - knee) * tanh((|y| - knee) / (1 - knee))`); výsledek je
 * v rozsahu [-1, 1]. Před tím střídavá vazba
 * výstupu (jen T_COUPLING): střední hodnota součtu před filtry se blíží
 * k součtu s časovou konstantou T_OUT_AC_TAU_S a odečte se od obou součtů.
 */
static float ref_output_stage(double *ac, double *lp, float legacy, float raw)
{
    if (T_COUPLING) {
        double k2 = 1.0 - exp(-1.0 / (T_OUT_AC_TAU_S * (double)T_RATE));
        *ac += ((double)raw - *ac) * k2;
        legacy -= (float)*ac;
        raw -= (float)*ac;
    }
    double k = 1.0 - exp(-1.0 / (T_OUT_LP_TAU_S * (double)T_RATE));
    *lp += ((double)raw - *lp) * k;
    float y = s_filter_on ? (float)*lp : legacy;
    y *= s_filter_on ? powf(10.0f, T_MAKEUP_ON_DB / 20.0f) : powf(10.0f, T_MAKEUP_OFF_DB / 20.0f);
    float a = fabsf(y);
    if (a > T_KNEE) {
        a = T_KNEE + (1.0f - T_KNEE) * tanhf((a - T_KNEE) / (1.0f - T_KNEE));
        y = (y < 0.0f) ? -a : a;
    }
    return y;
}

/**
 * Dorovnání hlasitosti výstupního stupně v aktuálním stavu propusti (násobitel).
 * Absolutní meze a očekávané úrovně testů jsou zapsané pro výstup bez dorovnání
 * a tímto násobitelem se přepočítávají.
 */
static double ref_makeup(void)
{
    return pow(10.0, (double)(s_filter_on ? T_MAKEUP_ON_DB : T_MAKEUP_OFF_DB) / 20.0);
}

/** Kopie iface_audio_mix_channels_with_gain() (iface_audio.c), bez měření úrovní; PSG jen při HAVE_PSG >= 1. */
static void ref_mix(ref_state_t *s, float ch[T_CH_MAX][T_SPF_MAX], float raw[T_CH_MAX][T_SPF_MAX], const float *gain, float *output)
{
    const float ctc0_mix = ref_ctc0_mix_level();
    for (size_t i = 0; i < T_SPF; i++) {
        float sample_ctc0 = ch[0][i] * gain[0];
        sample_ctc0 *= ctc0_mix;
        float raw_ctc0 = raw[0][i] * gain[0] * ctc0_mix;
        float sample_psg0 = 0.0f, raw_psg0 = 0.0f;
        if (T_CH > 1) { /* #if HAVE_PSG >= 1 */
            for (size_t c = 1; c < 1 + T_PSG_CH; c++) { sample_psg0 += ch[c][i] * gain[c]; raw_psg0 += raw[c][i] * gain[c]; }
            sample_psg0 /= (float)T_PSG_CH;
            raw_psg0 /= (float)T_PSG_CH;
        }
        output[i] = ref_output_stage(&s->out_ac[0], &s->out_lp[0], sample_ctc0 + sample_psg0, raw_ctc0 + raw_psg0);
    }
}

/** Kopie iface_audio_mix_channels_stereo() (iface_audio.c), bez měření úrovní; výstup prokládaně L, R. */
static void ref_mix_stereo(ref_state_t *s, float ch[T_CH_MAX][T_SPF_MAX], float raw[T_CH_MAX][T_SPF_MAX], const float *gain, float *output)
{
    const float ctc0_mix = ref_ctc0_mix_level();
    for (size_t i = 0; i < T_SPF; i++) {
        float sample_ctc0 = ch[0][i] * gain[0];
        sample_ctc0 *= ctc0_mix;
        float raw_ctc0 = raw[0][i] * gain[0] * ctc0_mix;
        float sample_psg0 = 0.0f, raw_psg0 = 0.0f;
        for (size_t c = 1; c < (1 + T_PSG_CH); c++) { sample_psg0 += ch[c][i] * gain[c]; raw_psg0 += raw[c][i] * gain[c]; }
        sample_psg0 /= (float)T_PSG_CH;
        raw_psg0 /= (float)T_PSG_CH;
        float sample_psg1 = 0.0f, raw_psg1 = 0.0f;
        for (size_t c = (1 + T_PSG_CH); c < T_CH; c++) { sample_psg1 += ch[c][i] * gain[c]; raw_psg1 += raw[c][i] * gain[c]; }
        sample_psg1 /= (float)T_PSG_CH;
        raw_psg1 /= (float)T_PSG_CH;
        output[i * 2] = ref_output_stage(&s->out_ac[0], &s->out_lp[0], sample_ctc0 + sample_psg0, raw_ctc0 + raw_psg0);
        output[i * 2 + 1] = ref_output_stage(&s->out_ac[1], &s->out_lp[1], sample_ctc0 + sample_psg1, raw_ctc0 + raw_psg1);
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

/** Přidá změnu hodnoty kanálu. @param t Čas [takty GDG]. @param ch Kanál. @param v Hodnota 0..30. */
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
 * 28-52, aby plató CTC0 bylo čisté); PSG2 tón 1,1 kHz s hodnotou 26 = poloviční
 * výchylka stupně 11 (jako přehrávání vzorků přes hlasitost s děličem 001h; snímky 5-25);
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
    square(2, 5 * T_TPF + 80, 25 * T_TPF + 80, 8080, 26); /* ~1,1 kHz, poloviční výchylka stupně 11 */
    uint32_t lfsr = 0xACE1u;
    for (uint64_t t = 52 * T_TPF + 80; t < 90 * T_TPF; t += 80 * 37) {
        lfsr = (lfsr >> 1) ^ (uint32_t)(-(int32_t)(lfsr & 1u) & 0xB400u);
        add_ev(t, 3, (lfsr & 1u) ? 9 : 0);
    }
    add_ev(90 * T_TPF + 80, 3, 0);
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    for (size_t i = 0; i < s_nev; i++) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, s_ev[i].t % T_TPF, "event on a frame start");
}

/**
 * Hlasitostní tabulka PSG (iface_audio_SN76489_get_output_level() pro F32, volume 100):
 * hodnoty 0..15 plná výchylka stupně, 16..30 výchylka stupně `v - 15` krát
 * T_HALF_FACTOR (tón s děličem 001h), 31 se nepoužívá (0).
 */
static void psg_volume(float vol[VIDEOREC_AUDIO_LEVELS])
{
    for (int v = 0; v < 16; v++) {
        if (v == 0) vol[v] = 0;
        else if (v == 15) vol[v] = (float)(((float)1 / 100) * 100);
        else vol[v] = (float)((((float)1 / 100) * 100) * pow(10, -((float)(15 - v) / 10)));
    }
    for (int v = 16; v < VIDEOREC_AUDIO_LEVELS; v++) vol[v] = (v <= 30) ? (float)(T_HALF_FACTOR * vol[v - 15]) : 0.0f;
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
    static float raws[T_CH_MAX][T_SPF_MAX];
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
        ref_output_stream_ctc0(&s, ctc, T_CTC_N, T_SPF, chs[0], raws[0]);
        for (unsigned c = 1; c < T_CH; c++) ref_process_psg(&s, (int)c, &src[c], T_TPF, vol, T_SPF, chs[c], raws[c]);
        if (P->stereo) ref_mix_stereo(&s, chs, raws, gain, mix);
        else ref_mix(&s, chs, raws, gain, mix);
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
        lv[0][v] = (v ? 1.0f : 0.0f) * gain[0] * k * ref_ctc0_mix_level();
        for (unsigned c = 1; c < T_CH; c++) lv[c][v] = vol[v] * gain[c] / (float)T_PSG_CH;
    }
    static st_VIDEOREC_AUDIO a;
    videorec_audio_init(&a, T_CLK, T_RATE, T_CH, (const float (*)[VIDEOREC_AUDIO_LEVELS])lv, NULL, 0, true);
    videorec_audio_set_stereo(&a, P->stereo);
    videorec_audio_set_output_filter(&a, s_filter_on, true);
    videorec_audio_set_output_coupling(&a, T_COUPLING);
    for (size_t e = 0; e < s_nev; e++) TEST_ASSERT_EQUAL_INT(0, videorec_audio_event(&a, s_ev[e].ch, s_ev[e].v, s_ev[e].t));
    static int16_t st[T_FRAMES_MAX * T_SPF_MAX * 2];
    TEST_ASSERT_EQUAL_size_t(frames * T_SPF, videorec_audio_render(&a, frames * T_TPF, st, frames * T_SPF));
    for (size_t i = 0; i < frames * T_SPF; i++) {
        out[i] = st[i * 2];
        if (out_r) out_r[i] = st[i * 2 + 1];
    }
    videorec_audio_free(&a);
}

void setUp(void) { P = &PLAT_MZ800; s_filter_on = false; }
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
        /* jednotka střídavé vazby jako zesílení převzorkování SDL cesty (12/13 nebo 13/14) */
        float unit = (i & 1u) ? 12.0f / 13.0f : 13.0f / 14.0f;
        float xc = x * unit;
        float rr1 = 0, rr2 = 0, nr1 = 0, nr2 = 0; /* vzorky před filtry kanálů (vstup výstupní propusti) */
        float r1 = ref_ctc_chain(&s, xc, unit, &rr1);
        float r2 = ref_psg_chain(&s, 1, x, ev, &rr2, true);
        float n1 = videorec_audio_chain_ctc0(&cc, &coef, xc, unit, &nr1);
        float n2 = videorec_audio_chain_psg(&cp, &coef, x, ev, &nr2);
        if (memcmp(&r1, &n1, sizeof(float)) != 0 || memcmp(&r2, &n2, sizeof(float)) != 0) mismatch++;
        if (memcmp(&rr1, &nr1, sizeof(float)) != 0 || memcmp(&rr2, &nr2, sizeof(float)) != 0) mismatch++;
    }
    printf("chain parity: 200000 samples, %u mismatches\n", mismatch);
    TEST_ASSERT_EQUAL_UINT(0, mismatch);
}

/*
 * 2. Celá cesta ze stejných událostí (2 s, MZ-800, 44 100 Hz).
 *
 * Tolerance:
 * - RMS rozdílu <= 10 % RMS signálu a max. |rozdíl| <= 4000 (12 % rozsahu;
 *   mez podílu byla 5 %, dokud CTC0 nesl stejnosměrnou složku a byl hlasitější -
 *   rozdíl sám se nezměnil, RMS 319,6, viz compare()):
 *   rozdíly jsou jen na hranách obdélníků - hrana se proti SDL posune
 *   o zlomek až cca 2 vzorky (bodové vzorkování SDL s krokem 401 místo
 *   401,85 taktu) a box filtr dá na hraně mezihodnotu; obdélník 1-2 kHz má
 *   hranu každých 11-22 vzorků. Naměřeno: RMS 2,3 %, max 2986.
 * - Střední hodnota plató CTC0 do 1 % (korekce zesílení CTC0).
 * - Střídavá vazba CTC0 a celého výstupu: plató (trvale 1 od snímku 30)
 *   doznívá - poměr středních hodnot snímků 45 a 31 je pod
 *   exp(-14 * 0,02 / 0,85) = 0,719 a střední hodnota snímku 45 je v obou
 *   cestách stejná (do 1 % plató).
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
    TEST_ASSERT_TRUE_MESSAGE(rms_d <= 0.10 * rms_r, "RMS difference above 10 % of the signal");
    TEST_ASSERT_TRUE_MESSAGE(mx <= 4000.0 * ref_makeup(), "max difference above 4000 times the makeup gain");

    /* Plató CTC0 (snímek 31: trvale 1, ostatní kanály tiché nebo už zaparkované) */
    double m_ref = 0, m_ren = 0;
    for (size_t i = 31 * T_SPF; i < 32 * T_SPF; i++) { m_ref += ref[i]; m_ren += ren[i]; }
    printf("CTC0 plateau mean: SDL %.1f, renderer %.1f\n", m_ref / T_SPF, m_ren / T_SPF);
    TEST_ASSERT_TRUE(m_ref / T_SPF > 2000.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.01 * fabs(m_ref), m_ref, m_ren);

    /* Střídavá vazba: plató doznívá stejně v obou cestách. Vazbou CTC0 (0,85 s)
     * by kleslo na 0,719; vazba celého výstupu (0,57 s) a doznívající střední
     * hodnota předchozího tónu PSG pokles ještě zrychlí. */
    double d_ref = 0, d_ren = 0;
    for (size_t i = 45 * T_SPF; i < 46 * T_SPF; i++) { d_ref += ref[i]; d_ren += ren[i]; }
    double decay = exp(-14.0 * 0.02 / T_CTC0_AC_TAU_S);
    printf("CTC0 AC coupling: frame 45 / frame 31 SDL %.3f, renderer %.3f (expected %.3f)\n", d_ref / m_ref, d_ren / m_ren, decay);
    TEST_ASSERT_TRUE(d_ref / m_ref < decay && d_ren / m_ren < decay);
    TEST_ASSERT_DOUBLE_WITHIN(0.01 * fabs(m_ref), d_ref, d_ren);
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
 * Stálá úroveň kanálu PSG. Dřív známá odchylka: kanál bez změny v celém snímku
 * jde v SDL cestě přes časnou větev `samples_count == 0`
 * (iface_audio_resampler_process_psg_audio_log()), která vrací
 * `volume[last_value]` bez parkování a bez IIR, takže SDL cesta stálou úroveň
 * držela a renderer ji po 45 ms zaparkoval. Se střídavou vazbou výstupu
 * (MZ-800, MZ-1500) se kanály PSG neparkují a stálou úroveň v obou cestách
 * stejně odvede vazba: doznívá s časovou konstantou 0,57 s.
 */
static void test_psg_constant_level_decays(void)
{
    const float gain[T_CH_MAX] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[10 * T_SPF_MAX], ren[10 * T_SPF_MAX];
    s_nev = 0;
    add_ev(80, 2, 10);
    run_sdl(10, gain, ref, NULL);
    run_renderer(10, gain, ren, NULL);
    double lvl = 32767.0 * pow(10, -0.5) / 4.0 * ref_makeup(); /* hlasitost 10 = 10^-0,5, průměr 4 kanálů, dorovnání hlasitosti */
    double want = lvl * exp(-0.11 / T_OUT_AC_TAU_S); /* střed snímku 5 = 0,11 s po skoku */
    printf("PSG constant level: SDL frame 5 mean %.1f, renderer %.1f (level %.1f, expected %.1f)\n",
           mean16(ref, 5 * T_SPF, 6 * T_SPF), mean16(ren, 5 * T_SPF, 6 * T_SPF), lvl, want);
    TEST_ASSERT_DOUBLE_WITHIN(0.02 * want, want, mean16(ref, 5 * T_SPF, 6 * T_SPF));
    TEST_ASSERT_DOUBLE_WITHIN(0.02 * want, want, mean16(ren, 5 * T_SPF, 6 * T_SPF));
    free(s_ev);
    s_ev = NULL;
    s_cap = s_nev = 0;
}

/*
 * Regrese opravené chyby SDL cesty (dříve dokumentační test známé chyby):
 * změna kanálu přesně v prvním taktu logu snímku dá `samples[0].count_ticks == 0`.
 * Původní podmínka `current_time >= (total_time - 1)` pak přetekla (0 - 1
 * v uint64_t) a SDL cesta celý snímek držela starou hodnotu kanálu. Kroky PSG
 * v audio.c padají na `last_psg_timestamp`, který je zároveň `first_timestamp`
 * dalšího logu, takže změna PSG v prvním kroku snímku tento stav vyvolá -
 * ověřeno zachycením zvuku (emu_audio_capture) na živém emulátoru: Batman 14,
 * SAPO-P 16, FX-Soundtrack 1 38 snímků za 30 s s rozdílem DC > 500 proti
 * referenci, po opravě 0.
 */
static void test_event_at_frame_start(void)
{
    const float gain[T_CH_MAX] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[3 * T_SPF_MAX], ren[3 * T_SPF_MAX];
    s_nev = 0;
    square(1, 80, 3 * T_TPF - 800, 20160, 15);
    add_ev(T_TPF, 2, 15); /* PSG2 zapnut přesně na začátku snímku 1 */
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    run_sdl(3, gain, ref, NULL);
    run_renderer(3, gain, ren, NULL);
    /* PSG2 (hlasitost 15, úroveň 0,25) ve snímku 1: SDL cesta i renderer ho
     * pustí (dříve SDL chyběl celou úrovní 0,25 = 8192); PSG1 v obou. */
    double d1 = mean16(ren, T_SPF + 100, 2 * T_SPF) - mean16(ref, T_SPF + 100, 2 * T_SPF);
    printf("event at frame start: renderer - SDL mean in frame 1 = %.1f (PSG2 level %.1f)\n", d1, 32767.0 * 0.25);
    TEST_ASSERT_DOUBLE_WITHIN(400.0, 0.0, d1);
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
 * Porovná výstup rendereru se SDL cestou: RMS rozdílu <= 10 % RMS signálu,
 * max <= 4000.
 *
 * Rozdíl cest je jen na hranách obdélníků PSG (bodové vzorky proti průměru
 * přes vzorek) a jeho absolutní velikost na úrovni CTC0 nezávisí. Od snížení
 * CTC0 na 1,49 násobek kanálu PSG (dřív asi 3,7 násobek) je ale signál tišší,
 * takže stejný rozdíl dává větší podíl: MZ-800 s druhým PSG, levá strana,
 * RMS rozdílu 284,6 -> 283,2, RMS signálu 9038,8 -> 4948,5, podíl 3,1 % -> 5,7 %;
 * pravá strana 309,7 -> 308,7, 8413,4 -> 4122,8, podíl 3,7 % -> 7,5 %.
 * Mez podílu je proto 10 % místo dřívějších 5 %.
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
    TEST_ASSERT_TRUE_MESSAGE(rms_d <= 0.10 * rms_r, "RMS difference above 10 % of the signal");
    TEST_ASSERT_TRUE_MESSAGE(mx <= 4000.0 * ref_makeup(), "max difference above 4000 times the makeup gain");
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
 *    CTC na snímek, 735 výstupních): obdélník, plató (doznívá střídavou vazbou), obdélník.
 *    Samostatný MZ-700 výstupní propust nemá (AUDIO_HAVE_OUTPUT_FILTER 0), test
 *    proto běží jen s vypnutou volbou; střídavá vazba je tam pomůcka emulátoru.
 *    Hrany na násobcích 26 taktů (perioda CTC proudu). L == R v obou cestách.
 */
static void test_mono_parity_mz700_ntsc(void)
{
    P = &PLAT_MZ700N;
    TEST_ASSERT_FALSE(s_filter_on); /* MZ-700: jen dosavadní filtry */
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
    /* Úroveň CTC0 bez PSG je stejná jako na MZ-800: rozkmit obdélníku je
     * hlasitost 0,8 krát 1,49 / 4 (obdélník 0,8 kHz ve snímcích 45 až 55; rychlejší
     * obdélník filtry kanálu nenechají doběhnout). Na skutečném MZ-700 neměřeno. */
    double swing = 0.8 * T_CTC0_TO_PSG / T_PSG_CH * 32767.0 * ref_makeup(); /* krát dorovnání hlasitosti; pod kolenem omezovače */
    int mn_ref = 32767, mx_ref = -32768, mn_ren = 32767, mx_ren = -32768;
    for (size_t i = 45 * T_SPF; i < 55 * T_SPF; i++) {
        if (refl[i] < mn_ref) mn_ref = refl[i];
        if (refl[i] > mx_ref) mx_ref = refl[i];
        if (renl[i] < mn_ren) mn_ren = renl[i];
        if (renl[i] > mx_ren) mx_ren = renl[i];
    }
    TEST_ASSERT_DOUBLE_WITHIN(0.03 * swing, swing, (double)(mx_ref - mn_ref));
    TEST_ASSERT_DOUBLE_WITHIN(0.03 * swing, swing, (double)(mx_ren - mn_ren));
    /* střídavá vazba: obdélník zasahuje pod nulu */
    TEST_ASSERT_TRUE(mn_ref < -1000 && mn_ren < -1000);
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

/*
 * 6. Výstupní propust zapnutá (volba [AUDIO] output_filter): stejné scénáře,
 *    obě cesty vynechají filtry kanálů a součet pustí dolní propustí 460 us.
 *    Bodové vzorky SDL cesty a průměr rendereru se liší jen na hranách a propust
 *    rozdíl dál vyhladí; meze jako compare().
 */
static void test_full_path_parity_output_filter(void)
{
    const unsigned F = 100;
    const float gain[T_CH_MAX] = { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t ref[T_FRAMES_MAX * T_SPF_MAX], ren[T_FRAMES_MAX * T_SPF_MAX];
    static int16_t ref_off[T_FRAMES_MAX * T_SPF_MAX];
    build_scenario();
    run_sdl(F, gain, ref_off, NULL);
    s_filter_on = true;
    run_sdl(F, gain, ref, NULL);
    run_renderer(F, gain, ren, NULL);
    compare("output filter", ref, ren, (size_t)F * T_SPF);

    /* propust opravdu působí: výstup se od vypnuté volby liší a je hladší (kroky bez dorovnání
     * hlasitosti, které je se zapnutou propustí o 4 dB větší) */
    const double mk_on = ref_makeup();
    s_filter_on = false;
    const double mk_off = ref_makeup();
    s_filter_on = true;
    double step_on = 0, step_off = 0;
    for (size_t i = 1; i < (size_t)F * T_SPF; i++) {
        step_on += fabs((double)ref[i] - ref[i - 1]) / mk_on;
        step_off += fabs((double)ref_off[i] - ref_off[i - 1]) / mk_off;
    }
    printf("output filter: mean |step| on %.1f, off %.1f\n", step_on / (F * T_SPF), step_off / (F * T_SPF));
    TEST_ASSERT_TRUE(step_on < 0.5 * step_off);
    free_events();
}

static void test_stereo_parity_mz1500_output_filter(void)
{
    P = &PLAT_MZ1500;
    s_filter_on = true;
    const unsigned F = 60;
    const float gain[T_CH_MAX] = { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static int16_t refl[T_FRAMES_MAX * T_SPF_MAX], refr[T_FRAMES_MAX * T_SPF_MAX];
    static int16_t renl[T_FRAMES_MAX * T_SPF_MAX], renr[T_FRAMES_MAX * T_SPF_MAX];
    s_nev = 0;
    square(0, 320, 20 * T_TPF + 320, 7360, 15);               /* CTC0 ~1 kHz, obě strany */
    square(1, 640, 50 * T_TPF + 640, 16320, 13);              /* PSG0 ~440 Hz, jen L */
    square(5, 10 * T_TPF + 960, 58 * T_TPF + 960, 11200, 12); /* PSG1 ~640 Hz, jen R */
    qsort(s_ev, s_nev, sizeof(*s_ev), ev_cmp);
    run_sdl(F, gain, refl, refr);
    run_renderer(F, gain, renl, renr);
    size_t n = (size_t)F * T_SPF;
    compare("stereo L, output filter", refl, renl, n);
    compare("stereo R, output filter", refr, renr, n);
    double dref = rms_lr(refl, refr, n), dren = rms_lr(renl, renr, n);
    TEST_ASSERT_TRUE(dref > 500.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.05 * dref, dref, dren);
    free_events();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chain_bit_exact);
    RUN_TEST(test_full_path_parity);
    RUN_TEST(test_psg_constant_level_decays);
    RUN_TEST(test_event_at_frame_start);
    RUN_TEST(test_stereo_parity_mz1500);
    RUN_TEST(test_mono_parity_mz700_ntsc);
    RUN_TEST(test_stereo_parity_mz800_psg1);
    RUN_TEST(test_full_path_parity_output_filter);
    RUN_TEST(test_stereo_parity_mz1500_output_filter);
    return UNITY_END();
}
