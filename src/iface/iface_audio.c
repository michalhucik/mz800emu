#include "main.h"
#include <stdio.h>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
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
#include "time_profiler.h"
#include "videorec/videorec.h"
#include "audio_capture.h"

/* Režim podle reality převzorkovává výstup této cesty - musí znát jeho frekvenci a blok
 * (blok = IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC: 882 při 50, 735 při 60 snímcích/s;
 * musí vyjít celý, jinak by se zvuk proti obrazu posouval). */
G_STATIC_ASSERT(IFACE_AUDIO_SAMPLE_RATE == VIDEOREC_RT_SDL_RATE);
G_STATIC_ASSERT(IFACE_AUDIO_SAMPLE_RATE % VIDEO_SCREENS_PER_SEC == 0);
/* Tabulka úrovní video záznamu musí pojmout všechny hodnoty zvukového logu. */
G_STATIC_ASSERT(AUDIO_LEVEL_VALUES_COUNT <= VIDEOREC_AUDIO_LEVELS);

#if (LINUX && defined(USE_SDL2) && defined(USE_SDL_AUDIO)) || defined(MZ800EMU_CFG_AUDIO_DISABLED)
#define AUDIO_SYNC_BY_NSTIMER
#endif // LINUX

st_IFACE_AUDIO g_iface_audio;

void iface_audio_debug_save_raw_audio_file(const char *filename, void *samples, size_t samples_size)
{
    FILE *f = fopen(filename, "ab");
    if (f == NULL)
    {
        fprintf(stderr, "%s():%d - Unable to open file: %s\n", __func__, __LINE__, filename);
        return;
    };

    fwrite(samples, 1, samples_size, f);
    fclose(f);
}

void print_audiolog(const st_AUDIO_SOURCE_LOG *source, uint64_t start_time, uint64_t end_time)
{
    static uint64_t log_cnt = 0;
    g_print("Audio log c. %" PRIu64 "\n", log_cnt);
    g_print("Start time: %" PRIu64 "\n", start_time);
    g_print("End time: %" PRIu64 "\n", end_time);
    g_print("Samples count: %u\n", source->samples_count);
    g_print("Last value: %u\n", source->last_value);
    for (unsigned int i = 0; i < source->samples_count; i++)
    {
        g_print("Sample %d - value: %u, count_ticks: %u\n", i, source->samples[i].value, source->samples[i].count_ticks);
    };
    g_print("\n");
    log_cnt++;
}

/* Doxygen viz iface_audio.h. */
float iface_audio_ctc0_mix_level(void)
{
    float ctc0_gain = videorec_audio_sdl_ctc0_gain(IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC,
                                                   IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC);
    return AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / (float)PSG_CHANNELS_COUNT / ctc0_gain;
}

/** @brief Koeficienty výstupního stupně pro IFACE_AUDIO_SAMPLE_RATE (videorec_audio_coef_init()). Jen vlákno mixu. */
static st_VIDEOREC_AUDIO_COEF s_out_coef;
/** @brief s_out_coef je spočtené. */
static bool s_out_coef_ready = false;
/** @brief Stav výstupní propusti L, R (běží stále, i když je volba vypnutá). Jen vlákno mixu. */
static double s_out_lp[2] = {0.0, 0.0};
/** @brief Stav střídavé vazby výstupu L, R (střední hodnota součtu kanálů; bez AUDIO_HAVE_OUTPUT_COUPLING 0). Jen vlákno mixu. */
static double s_out_ac[2] = {0.0, 0.0};
/** @brief Váha výstupní propusti v mixu 0..1; záporná = ještě nenastavená (první blok začne rovnou v cílovém stavu). */
static float s_out_gain = -1.0f;

/* Doxygen viz iface_audio.h. */
bool iface_audio_output_filter_enabled(void)
{
    return g_iface_audio.output_filter;
}

/* Doxygen viz iface_audio.h. */
void iface_audio_set_output_filter(bool on)
{
#if AUDIO_HAVE_OUTPUT_FILTER
    g_iface_audio.output_filter = on;
#else
    /* samostatný MZ-700 výstupní propust nemá (viz AUDIO_HAVE_OUTPUT_FILTER) */
    (void)on;
    g_iface_audio.output_filter = false;
#endif
}

/**
 * @brief Střední hodnota součtu kanálů každé strany ze středních hodnot kanálů.
 *
 * Váží střední hodnoty kanálů stejně jako mix: CTC0 hlasitostí, násobitelem
 * iface_audio_ctc0_mix_level() a středním zesílením převzorkování, kanály PSG
 * hlasitostí a výchylkou plné úrovně děleno PSG_CHANNELS_COUNT. Stereo: L =
 * CTC0 + PSG0, R = CTC0 + PSG1; mono: obě strany CTC0 + PSG0.
 *
 * @param mean   Střední hodnoty kanálů (audio_out_ac_means()), AUDIO_SRC_CHANNELS_COUNT prvků.
 * @param stereo Stereo rozložení (dva čipy PSG).
 * @param out    Výstup: střední hodnota levé a pravé strany.
 */
static void iface_audio_output_ac_from_means(const double *mean, bool stereo, double out[2])
{
    float ctc0_gain = videorec_audio_sdl_ctc0_gain(IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC,
                                                   IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC);
    double ctc0 = mean[0] * (double)g_iface_audio.gain[0] * (double)iface_audio_ctc0_mix_level() * (double)ctc0_gain;
    double psg[2] = {0.0, 0.0};
    for (int ch = 1; ch < AUDIO_SRC_CHANNELS_COUNT; ch++)
    {
        int chip = (ch - 1) / PSG_CHANNELS_COUNT;
        psg[chip & 1] += mean[ch] * (double)g_iface_audio.SN76489_volume_value[ch][AUDIO_MAXVAL_PER_CHANNEL]
                         * (double)g_iface_audio.gain[ch] / (double)PSG_CHANNELS_COUNT;
    };
    out[0] = ctc0 + psg[0];
    out[1] = stereo ? (ctc0 + psg[1]) : out[0];
}

/* Doxygen viz iface_audio.h. */
void iface_audio_output_ac_means(uint64_t now_ticks, bool stereo, double out[2])
{
    double mean[AUDIO_SRC_CHANNELS_COUNT];
    audio_out_ac_means(now_ticks, mean);
    iface_audio_output_ac_from_means(mean, stereo, out);
}

/**
 * @brief Připraví výstupní stupeň mixu před zpracováním bloku.
 *
 * Spočte koeficienty (jednou) a při prvním bloku položí váhu výstupní propusti
 * rovnou do stavu podle volby (bez přechodu).
 */
static inline void iface_audio_output_stage_prepare(void)
{
    if (!s_out_coef_ready)
    {
        videorec_audio_coef_init(&s_out_coef, IFACE_AUDIO_SAMPLE_RATE);
        /* střídavá vazba celého výstupu (MZ-800, MZ-1500) místo parkování PSG */
        videorec_audio_coef_set_output_coupling(&s_out_coef, IFACE_AUDIO_SAMPLE_RATE, AUDIO_HAVE_OUTPUT_COUPLING != 0);
        s_out_coef_ready = true;
    };
    if (s_out_gain < 0.0f)
    {
        s_out_gain = g_iface_audio.output_filter ? 1.0f : 0.0f;
    };
}

/**
 * @brief Mono mix: CTC0 + průměr kanálů PSG0, výstupní stupeň.
 *
 * Součet = CTC0 * gain[0] * iface_audio_ctc0_mix_level()
 *        + součet(PSG0 kanál * gain[kanál]) / PSG_CHANNELS_COUNT,
 * počítaný dvakrát: z kanálů po jejich filtrech (`channels`) a před nimi
 * (`raw`). Výstupní stupeň videorec_audio_output_stage() z nich udělá vzorek:
 * s vypnutou volbou [AUDIO] output_filter součet po filtrech oříznutý shora
 * na 1,0 (dřívější chování), se zapnutou součet před filtry po dolní propusti
 * 460 us; přepnutí za běhu je plynulý přechod 10 ms. CTC0 má 1,49 násobek
 * výchylky jednoho kanálu PSG s útlumem 0 (AUDIO_CTC0_TO_PSG_CHANNEL_RATIO).
 * Indikace úrovní (g_iface_audio.level[]) se počítá z kanálů po filtrech,
 * před poměrem CTC0 a před dělením počtem kanálů.
 *
 * @param channels    Kanály po filtrech (AUDIO_SRC_CHANNELS_COUNT polí po output_size vzorcích).
 * @param raw         Kanály před filtry (stejný tvar).
 * @param output      Výstup, output_size vzorků v rozsahu [-1, 1].
 * @param output_size Počet vzorků.
 * @post g_iface_audio.level[] a total_level obsahují střední úrovně bloku;
 *       stav výstupní propusti a váha přechodu jsou posunuté o output_size vzorků.
 */
static inline void iface_audio_mix_channels_with_gain(float **channels, float **raw, float *output, size_t output_size)
{
    float *gain = g_iface_audio.gain;
    float level[AUDIO_SRC_CHANNELS_COUNT] = {0};
    float total_level = 0.0f;
    const float ctc0_mix = iface_audio_ctc0_mix_level();
    const bool filter_on = g_iface_audio.output_filter;

    iface_audio_output_stage_prepare();

    for (size_t i = 0; i < output_size; i++)
    {
        float sample_ctc0 = channels[0][i] * gain[0];
        level[0] += fabs(sample_ctc0);
        sample_ctc0 *= ctc0_mix;
        float raw_ctc0 = raw[0][i] * gain[0] * ctc0_mix;

        float sample_psg0 = 0.0f;
        float raw_psg0 = 0.0f;

#if HAVE_PSG >= 1
        for (size_t ch = 1; ch < (1 + PSG_CHANNELS_COUNT); ch++)
        {
            float sample = channels[ch][i] * gain[ch];
            level[ch] += fabs(sample);
            sample_psg0 += sample;
            raw_psg0 += raw[ch][i] * gain[ch];
        };
        sample_psg0 /= (float)PSG_CHANNELS_COUNT;
        raw_psg0 /= (float)PSG_CHANNELS_COUNT;
#endif /* HAVE_PSG >= 1 */

        float sample_max = sample_ctc0 + sample_psg0;

        s_out_gain = videorec_audio_output_gain_step(&s_out_coef, s_out_gain, filter_on);
        output[i] = videorec_audio_output_stage(&s_out_coef, &s_out_ac[0], &s_out_lp[0], sample_max, raw_ctc0 + raw_psg0, s_out_gain);
        total_level += fabs(output[i]);
    }
    /* mono: pravá strana má stejný stav jako levá (pro případné přepnutí na stereo) */
    s_out_lp[1] = s_out_lp[0];
    s_out_ac[1] = s_out_ac[0];

    for (size_t ch = 0; ch < AUDIO_SRC_CHANNELS_COUNT; ch++)
    {
        g_iface_audio.level[ch] = level[ch] / output_size;
    };

    g_iface_audio.total_level = total_level / output_size;
}

/**
 * @brief Stereo mix pro dva čipy PSG (MZ-1500, MZ-800 s druhým PSG).
 *
 * L = CTC0 + průměr PSG0 (kanály 1-4), R = CTC0 + průměr PSG1 (kanály 5-8).
 * CTC0 se násobí gain[0] a iface_audio_ctc0_mix_level(), každá strana má
 * vlastní výstupní stupeň (viz iface_audio_mix_channels_with_gain()).
 *
 * @param channels    Kanály po filtrech (AUDIO_SRC_CHANNELS_COUNT polí po output_size vzorcích).
 * @param raw         Kanály před filtry (stejný tvar).
 * @param output      Prokládaný výstup L, R; 2 * output_size vzorků v rozsahu [-1, 1].
 * @param output_size Počet stereo vzorků.
 * @post g_iface_audio.level[] a total_level obsahují střední úrovně bloku;
 *       stav výstupní propusti a váha přechodu jsou posunuté o output_size vzorků.
 */
static inline void iface_audio_mix_channels_stereo(float **channels, float **raw, float *output, size_t output_size)
{
    float *gain = g_iface_audio.gain;
    float level[AUDIO_SRC_CHANNELS_COUNT] = {0};
    float total_level = 0.0f;
    const float ctc0_mix = iface_audio_ctc0_mix_level();
    const bool filter_on = g_iface_audio.output_filter;

    iface_audio_output_stage_prepare();

    for (size_t i = 0; i < output_size; i++)
    {
        float sample_ctc0 = channels[0][i] * gain[0];
        level[0] += fabs(sample_ctc0);
        sample_ctc0 *= ctc0_mix;
        float raw_ctc0 = raw[0][i] * gain[0] * ctc0_mix;

        /* PSG0 → levý kanál */
        float sample_psg0 = 0.0f;
        float raw_psg0 = 0.0f;
        for (size_t ch = 1; ch < (1 + PSG_CHANNELS_COUNT); ch++)
        {
            float sample = channels[ch][i] * gain[ch];
            level[ch] += fabs(sample);
            sample_psg0 += sample;
            raw_psg0 += raw[ch][i] * gain[ch];
        };
        sample_psg0 /= (float)PSG_CHANNELS_COUNT;
        raw_psg0 /= (float)PSG_CHANNELS_COUNT;

        /* PSG1 → pravý kanál */
        float sample_psg1 = 0.0f;
        float raw_psg1 = 0.0f;
        for (size_t ch = (1 + PSG_CHANNELS_COUNT); ch < AUDIO_SRC_CHANNELS_COUNT; ch++)
        {
            float sample = channels[ch][i] * gain[ch];
            level[ch] += fabs(sample);
            sample_psg1 += sample;
            raw_psg1 += raw[ch][i] * gain[ch];
        };
        sample_psg1 /= (float)PSG_CHANNELS_COUNT;
        raw_psg1 /= (float)PSG_CHANNELS_COUNT;

        float left = sample_ctc0 + sample_psg0;
        float right = sample_ctc0 + sample_psg1;

        s_out_gain = videorec_audio_output_gain_step(&s_out_coef, s_out_gain, filter_on);
        output[i * 2]     = videorec_audio_output_stage(&s_out_coef, &s_out_ac[0], &s_out_lp[0], left, raw_ctc0 + raw_psg0, s_out_gain);
        output[i * 2 + 1] = videorec_audio_output_stage(&s_out_coef, &s_out_ac[1], &s_out_lp[1], right, raw_ctc0 + raw_psg1, s_out_gain);

        total_level += (fabs(output[i * 2]) + fabs(output[i * 2 + 1])) * 0.5f;
    }

    for (size_t ch = 0; ch < AUDIO_SRC_CHANNELS_COUNT; ch++)
    {
        g_iface_audio.level[ch] = level[ch] / output_size;
    };

    g_iface_audio.total_level = total_level / output_size;
}

float *iface_audio_wait_for_data(size_t *output_samples_size)
{
    APP_MUTEX_LOCK(g_iface_audio.mutex);
    while ((0 == (g_iface_audio.prepared_frame - g_iface_audio.played_frame)) && (g_iface_audio.state <= IFACE_AUDIO_BUFFER_STATE_UNSYNC))
    {
        APP_COND_WAIT_TIMEOUT_MS(g_iface_audio.frame_cond, g_iface_audio.mutex, 1);
        if (!sdlapp_is_running(g_sdlapp))
        {
            APP_MUTEX_UNLOCK(g_iface_audio.mutex);
            return NULL;
        };
    };

    g_iface_audio.played_frame = g_iface_audio.prepared_frame;
    APP_COND_SIGNAL(g_iface_audio.play_cond);

    en_IFACE_AUDIO_BUFFER_STATE state = g_iface_audio.state;

    if (state == IFACE_AUDIO_BUFFER_STATE_PAUSED)
    {
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        return NULL;
    };

    if (state == IFACE_AUDIO_BUFFER_STATE_EXITING)
    {
        printf("SDL audio player callback exiting\r\n");
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        return NULL;
    };

    st_AUDIO_LOG *audiolog = NULL;

    // prevezmeme si posledni audio log
    if (state == IFACE_AUDIO_BUFFER_STATE_NORMAL)
    {
        audiolog = g_audio.old_log;
        g_audio.old_log = NULL;
    }
    else // IFACE_AUDIO_BUFFER_STATE_UNSYNC
    {
        APP_MUTEX_LOCK(g_audio.old_log_mutex);
        audiolog = g_audio.old_log;
        g_audio.old_log = NULL;
        APP_MUTEX_UNLOCK(g_audio.old_log_mutex);
    };

    APP_MUTEX_UNLOCK(g_iface_audio.mutex);

    if (audiolog == NULL)
    {
        printf("%s():%d - No audio log\n", __func__, __LINE__);
        return NULL;
    };

    // static TimeProfiler *profiler = NULL;
    // if (profiler == NULL)
    // {
    //     profiler = time_profiler_init("Resampler", 5000);
    // };
    // time_profiler_start(profiler);

    float *all_samples[AUDIO_SRC_CHANNELS_COUNT];
    /* kanály před jejich filtry (CTC0 po střídavé vazbě, PSG po parkování): vstup výstupní propusti */
    float *raw_samples[AUDIO_SRC_CHANNELS_COUNT];
    size_t output_samples_count = IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC;
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        raw_samples[i] = g_malloc0(output_samples_count * sizeof(float));
    };
    bool stereo = audiolog->stereo;
    *output_samples_size = output_samples_count * 2 * sizeof(float); /* vždy stereo output pro SDL */
    // 45 ms => pocet vzorku po kterych povazujeme nemenici se hodnotu za zaparkovanou
    size_t parked_samples = (IFACE_AUDIO_SAMPLE_RATE / 1000) * 45;
    // 20 ms => pocet vzorku do kterych rozprostreme prechod zaparkovane hodnoty k 0
    size_t parked_samples_fade = (IFACE_AUDIO_SAMPLE_RATE / 1000) * 20;

    // CTC0 resamplujeme samostatne
    uint64_t log_count_samples = (audiolog->last_timestamp - audiolog->first_timestamp);
    size_t ctc0_samples_uint8_count = IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC;
    uint8_t *ctc0_samples_uint8 = iface_audio_resampler_process_audio_log(audiolog->src[0], log_count_samples, ctc0_samples_uint8_count);

    /* Střídavá vazba CTC0: log, který nenavazuje na předchozí zpracovaný
     * (start, pauza, MAX SPEED, headless bez odběru), převezme stav vedený
     * emulátorem v emulovaném čase; navazující log pokračuje ve vlastním stavu. */
    static uint64_t next_first_timestamp = UINT64_MAX;
    if (audiolog->first_timestamp != next_first_timestamp)
    {
        iface_audio_resampler_ctc0_ac_seed(audiolog->ctc0_ac_mean);
        /* totéž pro střídavou vazbu celého výstupu (bez ní jsou střední hodnoty nulové) */
        iface_audio_output_ac_from_means(audiolog->out_ac_mean, stereo, s_out_ac);
    };
    next_first_timestamp = audiolog->last_psg_timestamp;

    all_samples[0] = iface_audio_resampler_output_stream_ctc0(ctc0_samples_uint8, ctc0_samples_uint8_count, output_samples_count, raw_samples[0]);
    free(ctc0_samples_uint8);

#if HAVE_PSG >= 1
    // resamplujeme zvukove kanaly PSG (MZ-800 / MZ-1500); MZ-700 nema PSG
    for (int i = 1; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        //uint8_t *psg_samples_uint8 = iface_audio_resampler_process_audio_log(audiolog->src[i], log_count_samples, IFACE_AUDIO_PSG_SAMPLE_RATE, 20, NULL);

        all_samples[i] = iface_audio_resampler_process_psg_audio_log(i, audiolog->src[i], log_count_samples, g_iface_audio.SN76489_volume_value[i], output_samples_count, parked_samples, parked_samples_fade, raw_samples[i]);
        // all_samples[i] = iface_audio_resampler_output_stream_psg(i, psg_samples_uint8, psg_samples_uint8_count, output_samples_count, g_iface_audio.SN76489_volume_value[i], parked_samples, parked_samples_fade);
    };
#endif /* HAVE_PSG >= 1 */

    audio_log_destroy(audiolog);

    float *output_samples = g_malloc0(*output_samples_size);
    g_iface_audio.output_channels = stereo ? 2 : 1;
    if (stereo)
    {
        iface_audio_mix_channels_stereo(all_samples, raw_samples, output_samples, output_samples_count);
    }
    else
    {
        /* mono mix do dočasného bufferu, pak duplikace do stereo L=R */
        float *mono_buf = g_malloc(output_samples_count * sizeof(float));
        iface_audio_mix_channels_with_gain(all_samples, raw_samples, mono_buf, output_samples_count);
        for (size_t i = 0; i < output_samples_count; i++)
        {
            output_samples[i * 2]     = mono_buf[i];
            output_samples[i * 2 + 1] = mono_buf[i];
        }
        g_free(mono_buf);
    }

    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        g_free(all_samples[i]);
        g_free(raw_samples[i]);
    };

    // time_profiler_stop(profiler);

    // iface_audio_debug_save_raw_audio_file("audio.raw", output_samples, *output_samples_size);

    /* Video záznam podle reality: kopie bloku, který jde do reproduktoru (mimo realtime 1 čtení příznaku). */
    videorec_rt_audio_output(output_samples, output_samples_count);
    /* Zachycení zvuku: stejný blok do sdl.wav (mimo zachycení 1 čtení příznaku). */
    audio_capture_sdl_output(output_samples, output_samples_count);
    return output_samples;
}

/**
 * @brief Výchylka kanálu PSG pro hodnotu zvukového logu.
 *
 * @param value  Hodnota zvukového logu 0 až 30 (viz AUDIO_MAXVAL_PER_CHANNEL):
 *               0 ticho, 1 až 15 plná výchylka stupně (krok 2 dB), 16 až 30
 *               výchylka stupně `value - 15` krát AUDIO_LEVEL_HALF_FACTOR.
 * @param volume Hlasitost v procentech.
 * @return Výchylka v jednotkách AUDIO_OUTPUT_t.
 */
static AUDIO_OUTPUT_t iface_audio_SN76489_get_output_level(int value, int volume)
{
    if (audio_level_is_half((unsigned)value))
    {
        return (AUDIO_OUTPUT_t)(AUDIO_LEVEL_HALF_FACTOR
                                * iface_audio_SN76489_get_output_level((int)audio_level_step((unsigned)value), volume));
    };
    if (value == 0)
    {
        return 0;
    }
    else if (value == 15)
    {
        return (AUDIO_OUTPUT_t)(((float)AUDIO_OUTPUT_MAX_VALUE / 100) * volume);
    };
    return (AUDIO_OUTPUT_t)(((float)AUDIO_OUTPUT_MAX_VALUE / 100) * volume) * pow(10, -((float)(15 - value) / 10));
}

void iface_audio_set_src_volume(int id, int volume)
{
#if defined(MZ800EMU_CFG_AUDIO_DISABLED)
    (void)id;
    (void)volume;
#else
    for (int i = 0; i < AUDIO_LEVEL_VALUES_COUNT; i++)
    {
        g_iface_audio.SN76489_volume_value[id][i] = iface_audio_SN76489_get_output_level(i, volume);
    };
#endif /* MZ800EMU_CFG_AUDIO_DISABLED */
}

void iface_audio_build_videorec_levels(float level[][VIDEOREC_AUDIO_LEVELS], unsigned channels)
{
    /* Střední zesílení převzorkování CTC0 v SDL cestě (podíl nenulových / (délka bloku + 1)),
     * stejné rozměry bloků jako iface_audio_wait_for_data(). */
    float ctc0_gain = videorec_audio_sdl_ctc0_gain(IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC,
                                                   IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC);
    const float ctc0_mix = iface_audio_ctc0_mix_level();
    for (unsigned ch = 0; ch < channels && ch < AUDIO_SRC_CHANNELS_COUNT; ch++)
    {
        for (unsigned v = 0; v < VIDEOREC_AUDIO_LEVELS; v++)
        {
            if (ch == 0)
                /* CTC0: SDL cesta počítá podíl nenulových vzorků => 0 / zesílení bloku;
                 * poměr k PSG jako v mixu SDL cesty */
                level[ch][v] = (v ? ctc0_gain : 0.0f) * g_iface_audio.gain[0] * ctc0_mix;
            else if (v < AUDIO_LEVEL_VALUES_COUNT)
                /* PSG: hlasitostní tabulka + gain, průměr přes 4 kanály jako iface_audio_mix_channels_with_gain() */
                level[ch][v] = (float)g_iface_audio.SN76489_volume_value[ch][v] * g_iface_audio.gain[ch] / (float)PSG_CHANNELS_COUNT;
            else
                /* hodnota mimo obor zvukového logu */
                level[ch][v] = 0.0f;
        }
    }
}

void iface_audio_set_master_volume(int volume)
{
    (void)volume;
#if !defined(MZ800EMU_CFG_AUDIO_DISABLED)
    printf("%s():%d - Not implemented!\n", __func__, __LINE__);
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}

#if !defined(MZ800EMU_CFG_AUDIO_DISABLED)
void iface_audio_buffer_init(void)
{
    g_iface_audio.played_frame = 0;
    g_iface_audio.prepared_frame = 0;
    APP_MUTEX_CREATE(g_iface_audio.mutex);
    APP_COND_CREATE(g_iface_audio.frame_cond);
    APP_COND_CREATE(g_iface_audio.play_cond);
    g_iface_audio.state = IFACE_AUDIO_BUFFER_STATE_NORMAL;
    g_iface_audio.state_beffore_pause = IFACE_AUDIO_BUFFER_STATE_NORMAL;
    g_iface_audio.sync_by_timer = false;
    g_iface_audio.device_open_failed = false;
    g_iface_audio.timer_base_us = 0;
    g_iface_audio.timer_events = 0;
    g_iface_audio.channel_scan_value = 0;
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        iface_audio_set_src_volume(i, 100);
    };
}
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */

bool iface_audio_init(void)
{
#ifndef MZ800EMU_CFG_AUDIO_DISABLED
    iface_audio_buffer_init();

    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++)
    {
        g_iface_audio.gain[i] = 1.0f;
    };
    g_iface_audio.output_filter = (AUDIO_HAVE_OUTPUT_FILTER != 0);

    return iface_audio_lowlevel_init();
#else
    return true;
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}

void iface_audio_exit(void)
{
#ifndef MZ800EMU_CFG_AUDIO_DISABLED
    if (g_iface_audio.mutex)
    {
        APP_MUTEX_LOCK(g_iface_audio.mutex);
        g_iface_audio.state = IFACE_AUDIO_BUFFER_STATE_EXITING;
        APP_COND_SIGNAL(g_iface_audio.frame_cond);
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
    };

    iface_audio_lowlevel_quit();

    if (g_iface_audio.mutex)
    {
        APP_MUTEX_DESTROY(g_iface_audio.mutex);
    };

    if (g_iface_audio.frame_cond)
    {
        APP_COND_DESTROY(g_iface_audio.frame_cond);
    };

    if (g_iface_audio.play_cond)
    {
        APP_COND_DESTROY(g_iface_audio.play_cond);
    };
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}

#if !defined(MZ800EMU_CFG_AUDIO_DISABLED)
/**
 * @brief Počet period, o které smí časová synchronizace zaostat a ještě je dohnat.
 *
 * Větší zpoždění (návrat z pauzy, přepnutí z MAX SPEED, dlouhé zaseknutí
 * hostitele) se nedohání zrychleným během, řada termínů se založí znovu.
 */
#define IFACE_AUDIO_TIMER_MAX_LAG_EVENTS 5

/**
 * @brief Časová synchronizace jedné 20ms události bez audio zařízení.
 *
 * Termín k-té události je `timer_base_us + k * 1000000 / VIDEO_SCREENS_PER_SEC`
 * (absolutní, takže hrubá granularita uspání ve Windows nezpůsobí drift -
 * přesah jedné události zkrátí čekání té následující). Pokud je aktuální čas
 * za termínem o víc než IFACE_AUDIO_TIMER_MAX_LAG_EVENTS period, nebo jde
 * o první událost, řada se založí znovu od aktuálního času a nečeká se.
 *
 * @pre Volá jen emulační vlákno (timer_base_us a timer_events bez zámku).
 * @post Návrat nejdříve v termínu aktuální události (pokud se řada nezakládala).
 */
static void iface_audio_timer_sync(void)
{
    const int64_t period_us = 1000000 / VIDEO_SCREENS_PER_SEC;
    int64_t now = g_get_monotonic_time();

    g_iface_audio.timer_events++;
    int64_t deadline = g_iface_audio.timer_base_us + (int64_t)((g_iface_audio.timer_events * 1000000) / VIDEO_SCREENS_PER_SEC);

    if ((g_iface_audio.timer_base_us == 0) || ((now - deadline) > (IFACE_AUDIO_TIMER_MAX_LAG_EVENTS * period_us)))
    {
        /* první událost nebo velké zpoždění: nová řada, tato událost proběhne hned */
        g_iface_audio.timer_base_us = now;
        g_iface_audio.timer_events = 0;
        return;
    };

    while (now < deadline)
    {
        g_usleep((gulong)(deadline - now));
        now = g_get_monotonic_time();
    };
}
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */

/**
 * @brief Synchronizace emulátoru s audio bufferem (kontrakt viz iface_audio.h).
 *
 * Oznámíme audio callbacku, že máme připravený další audio snímek.
 * Pokud emulátor neběží v MAX SPEED, počkáme, až si callback snímek vezme
 * (s audio zařízením), nebo do dalšího časového termínu (bez audio zařízení -
 * headless nebo náhradní režim po selhání otevření zařízení).
 */
void iface_audio_20ms_sync(void)
{
#ifndef MZ800EMU_CFG_AUDIO_DISABLED
    // Oznamime, ze mame pripraveny dalsi audio frame
    APP_MUTEX_LOCK(g_iface_audio.mutex);
    g_iface_audio.prepared_frame++;
    APP_COND_SIGNAL(g_iface_audio.frame_cond);
    APP_MUTEX_UNLOCK(g_iface_audio.mutex);

    if (g_iface_audio.sync_by_timer && (videorec_rt_audio_wants_output() || audio_capture_wants_sdl()))
    {
        /* Bez audio zařízení (headless, GUI po selhání zařízení): video záznam
         * podle reality chce zvuk "jak by zněl".
         * Vyrobit blok stejnou SDL cestou jen pro nahrávku (nic se nepřehrává); jen když
         * ho jitter buffer potřebuje - jako zařízení, které si data žádá (při MAX SPEED
         * se tak nevyrábí blok za každou 20ms událost).
         * Zachycení zvuku (sdl.wav) chce blok za každý snímek - audio_capture_wants_sdl()
         * platí jen během zachycení se zapnutým sdl.wav. */
        size_t samples_size = 0;
        float *samples = iface_audio_wait_for_data(&samples_size);
        g_free(samples);
    };

    if ((g_iface_audio.state == IFACE_AUDIO_BUFFER_STATE_NORMAL) && g_iface_audio.sync_by_timer)
    {
        /* Bez audio zařízení (headless, nebo GUI po selhání otevření zařízení)
         * není callback, který by snímky odebíral -
         * čekání na play_cond by vždy vypršelo (cca 1 s na událost). Tempo
         * udávají monotónní hodiny. */
        iface_audio_timer_sync();
    }
    else if (g_iface_audio.state == IFACE_AUDIO_BUFFER_STATE_NORMAL)
    {
        // Pockame, az bude posledni audio frame prehran, cimz se synchronizujeme se zvukem
        int timeouts = 0;
        APP_MUTEX_LOCK(g_iface_audio.mutex);
        while ((g_iface_audio.prepared_frame != g_iface_audio.played_frame) && (g_iface_audio.state == IFACE_AUDIO_BUFFER_STATE_NORMAL))
        {
            APP_COND_WAIT_TIMEOUT_MS(g_iface_audio.play_cond, g_iface_audio.mutex, (1000 / VIDEO_SCREENS_PER_SEC));

            if (!sdlapp_is_running(g_sdlapp))
            {
                APP_MUTEX_UNLOCK(g_iface_audio.mutex);
                emulator_quit(EXIT_SUCCESS);
            };

            if (timeouts++ >= VIDEO_SCREENS_PER_SEC) // Pokud zde cekame dele, tak neco neni v poradku
            {
                g_print("%s():%d - timeout! Is Audio module running?\n", __func__, __LINE__);
                break;
            };
        };
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);

#ifdef AUDIO_SYNC_BY_NSTIMER
        // pokud callback neni synchronizovany na 20ms podle audia, tak se synchronizujeme podle get_ticks_ns()
        // while ((get_ticks_ns() - g_iface_audio.last_20ms_sync) < (19 * 1000000))
        // {
        // };
        // g_iface_audio.last_20ms_sync = get_ticks_ns();
        emulator_sync_20ms_delay();
#endif /* AUDIO_SYNC_BY_NSTIMER */
    };

#else /* MZ800EMU_CFG_AUDIO_DISABLED */

    // pokud callback neni synchronizovany na 20ms podle audia, tak se synchronizujeme podle ns_timer

    // while ((get_ticks_ns() - g_iface_audio.last_20ms_sync) < (20 * 1000000))
    // {
    // };
    // g_iface_audio.last_20ms_sync = get_ticks_ns();

    // nstimer_wait_for_next_cycle(&g_iface_audio.ns_last, 20 * 1000000);

    emulator_sync_20ms_delay();

#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}

/**
 * Aktualizace stavu audio bufferu - pokud je emulator v max speed, tak se nastavi stav na unsync
 */
void iface_audio_update_buffer_state(void)
{
#if !defined(MZ800EMU_CFG_AUDIO_DISABLED)
    APP_MUTEX_LOCK(g_iface_audio.mutex);
    en_IFACE_AUDIO_BUFFER_STATE current_state = g_iface_audio.state;

    if (current_state == IFACE_AUDIO_BUFFER_STATE_EXITING)
    {
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        return;
    };

    en_IFACE_AUDIO_BUFFER_STATE *state_ptr = (current_state == IFACE_AUDIO_BUFFER_STATE_PAUSED) ? &g_iface_audio.state_beffore_pause : &g_iface_audio.state;

    if (EMULATOR_TEST_MAX_SPEED)
    {
        *state_ptr = IFACE_AUDIO_BUFFER_STATE_UNSYNC;
    }
    else
    {
        *state_ptr = IFACE_AUDIO_BUFFER_STATE_NORMAL;
    };
    APP_MUTEX_UNLOCK(g_iface_audio.mutex);
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}

/**
 * Preprnuti chovani audio bufferu
 *
 * 0 - normal state (NORMAL, UNSYNC)
 * 1 - paused (negenerujeme zvuk)
 *
 */
void iface_audio_pause_emulation(unsigned value)
{
#if defined(MZ800EMU_CFG_AUDIO_DISABLED)
    (void)value;
#else
    APP_MUTEX_LOCK(g_iface_audio.mutex);
    en_IFACE_AUDIO_BUFFER_STATE current_state = g_iface_audio.state;

    if (current_state == IFACE_AUDIO_BUFFER_STATE_EXITING)
    {
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        return;
    };

    if (value)
    {
        if (current_state == IFACE_AUDIO_BUFFER_STATE_PAUSED)
        {
            APP_MUTEX_UNLOCK(g_iface_audio.mutex);
            return;
        };

        g_iface_audio.state_beffore_pause = current_state;
        g_iface_audio.state = IFACE_AUDIO_BUFFER_STATE_PAUSED;
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        iface_audio_lowlevel_pause();
    }
    else
    {
        if (current_state != IFACE_AUDIO_BUFFER_STATE_PAUSED)
        {
            APP_MUTEX_UNLOCK(g_iface_audio.mutex);
            return;
        };

        g_iface_audio.state = g_iface_audio.state_beffore_pause;
        APP_MUTEX_UNLOCK(g_iface_audio.mutex);
        iface_audio_lowlevel_resume();
    };
#endif /* !MZ800EMU_CFG_AUDIO_DISABLED */
}
