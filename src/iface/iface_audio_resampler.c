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
#include "audio.h"
#include "emulator.h"

/**
 * Vytvori audio stream z audio logu v sample rate odpovidajicimu output_frequency. Hodnoty ve vystupnim streamu jsou stale unipolarni a v rozsahu 0 az 15.
 *
 * @param source Ukazatel na audio log.
 * @param log_count_samples Celkovy pocet vzorku v logu v px_clk. (skutecna delka logu)
 * @param output_frequency Frekvence vystupniho streamu.
 * @param output_length_ms Delka vystupniho streamu v milisekundach. (pokud je jina, nez skutecna delka, tak dojde k jeho kompresi, nebo k roztazeni)
 * @param output_samples_count Ukazatel na promennou, do ktere se ulozi pocet vzorku vystupniho streamu.
 * @return Ukazatel na alokovany buffer s vystupnim streamem.
 */
uint8_t *iface_audio_resampler_process_audio_log(const st_AUDIO_SOURCE_LOG *source, uint64_t log_count_samples, size_t output_samples_count)
{
    size_t samples_count = output_samples_count;

    size_t samples_size = samples_count * sizeof(uint8_t);
    uint8_t *samples = g_malloc0(samples_size);
    if (!samples)
        return NULL;

    if (source->samples_count == 0)
    {
        memset(samples, source->last_value, samples_size);
        return samples;
    };

    uint64_t step = log_count_samples / samples_count;
    uint64_t current_time = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;

    for (size_t i = 0; i < samples_count; i++, current_time += step)
    {
        while (current_time >= (total_time - 1))
        {
            if (event_index < source->samples_count)
            {
                event_index++;
                total_time += source->samples[event_index].count_ticks;
            }
            else
            {
                total_time = log_count_samples;
                break;
            };
        };

        uint8_t current_value = source->samples[event_index].value;
        samples[i] = current_value;
    };

    return samples;
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
    static float prev_output[AUDIO_SRC_CHANNELS_MAX] = {0};
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
    static float prev_output_1[AUDIO_SRC_CHANNELS_MAX] = {0}; // Minulý výstup
    static float prev_output_2[AUDIO_SRC_CHANNELS_MAX] = {0}; // Předminulý výstup

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
    static float x1[AUDIO_SRC_CHANNELS_MAX] = {0}; // Minulé vstupy
    static float x2[AUDIO_SRC_CHANNELS_MAX] = {0}; // Minulé vstupy
    static float y1[AUDIO_SRC_CHANNELS_MAX] = {0}; // Minulé výstupy
    static float y2[AUDIO_SRC_CHANNELS_MAX] = {0}; // Minulé výstupy

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
    static float prev_value[AUDIO_SRC_CHANNELS_MAX] = {0};
    float smooth_factor = 0.2f; // Čím menší, tím pomalejší změna (pohlcuje lupání)

    float output = prev_value[channel] + smooth_factor * (input - prev_value[channel]);
    prev_value[channel] = output;
    return output;
}

static inline float iface_audio_anti_glitch_limiter(int channel, float input)
{
    static float prev_value[AUDIO_SRC_CHANNELS_MAX] = {0};
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

/**
 * Vytvori audio stream ve float formatu z dvouhodnotoveho vstupniho streamu (ctc0).
 *
 * @param input_samples Ukazatel na vstupni stream.
 * @param input_samples_count Pocet vzorku ve vstupnim streamu.
 * @param output_samples_count Pocet vzorku ve vystupnim streamu. (pokud je jiny, nez vstupni, tak dojde k resamplingu na vystupni frequenci)
 * @param parked_samples Pocet vzorku, ktere jsou limitni pro setrvavaní na stejne nenulove hodnotě. Po jeho prekroceni se zacne hodnota snizovat k 0.
 * @param parked_fade Pocet vzorku, ktere se pouziji pro plynuly prechod ze zaparkované hodnoty na 0.
 * @return Ukazatel na alokovany buffer s vystupnim streamem.
 */
float *iface_audio_resampler_output_stream_ctc0(const uint8_t *input_samples, size_t input_samples_count, size_t output_samples_count, size_t parked_samples, size_t parked_fade)
{
    int channel = 0;
    size_t output_samples_size = output_samples_count * sizeof(float);
    float *output_samples = g_malloc0(output_samples_size);
    if (!output_samples)
    {
        return NULL;
    };

    double ratio = (double)input_samples_count / (double)output_samples_count;
    // static float last_filtered = 0.0f; // Uchováváme poslední filtrovaný vzorek pro plynulost

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

        // Počítání jedniček v bloku
        size_t ones_count = 0;
        for (size_t j = start; j < end; j++)
        {
            if (input_samples[j])
                ones_count++;
        };

        float resampled = (float)ones_count / (float)(end - start + 1);

        // Pokud je hodnota zaparkovaná na konstantni hodnote, tak postupne snizujeme hodnotu k 0
        static uint8_t parking_previous_resampled = 0;
        static size_t parking_counter = 0;
        static float parking_gain = 0;
        static int parking_gain_counter = 0;

        if ((end_value == 0) || (end_value != parking_previous_resampled))
        {
            parking_previous_resampled = end_value;
            parking_counter = 0;
        }
        else
        {
            // Trvale stejnou hodnotu povolíme jen po dobu parked_samples
            if (parking_counter < parked_samples)
            {
                parking_counter++;
                parking_gain = 1.0f;
                parking_gain_counter = 0;
            }
            else
            {
                // Budeme postupne snizovat hodnotu az k 0 po dobu urcenou parked_fade
                resampled *= parking_gain;
                if (parking_gain > 0.0f)
                {
                    if (parking_gain_counter-- == 0)
                    {
                        parking_gain -= 1.0f / 20;
                        parking_gain_counter = parked_fade / 20;
                    };
                }
                else
                {
                    parking_gain = 0.0f;
                };
            };
        };

        // Aplikujeme jednopólový filtr
        float filtered1 = iface_audio_lowpass_filter(channel, resampled);
        // float filtered1 = resampled;

        // float filtered2 = iface_audio_lowpass_filter_2nd_order(filtered1);
        float filtered2 = filtered1;

        // Aplikujeme Butterworthův filtr
        // float filtered3 = iface_audio_lowpass_filter_butterworth(filtered2);
        float filtered3 = filtered2;

        float filtered4 = iface_audio_anti_glitch_filter(channel, filtered3);
        // float filtered4 = filtered3;

        // float final_output = iface_audio_anti_glitch_limiter(filtered4);
        float final_output = filtered4;

        // output_samples[i] = final_output;
        //  output_samples[i] = last_sample_value;

        output_samples[i] = final_output;
        // output_samples[i] = (last_filtered + final_output) * 0.5f;
        // last_filtered = final_output;

        // output_samples[i] = resampled;
    };

    return output_samples;
}

float *iface_audio_resampler_process_psg_audio_log(int channel, const st_AUDIO_SOURCE_LOG *source, uint64_t log_count_samples, float *SN76489_volume_value, size_t output_samples_count, size_t parked_samples, size_t parked_fade)
{
    size_t samples_count = output_samples_count;

    size_t samples_size = samples_count * sizeof(float);
    float *samples = g_malloc0(samples_size);
    if (!samples)
        return NULL;

    if (source->samples_count == 0)
    {
        for (size_t i = 0; i < samples_count; i++)
        {
            samples[i] = SN76489_volume_value[source->last_value];
        };
        return samples;
    };

    uint64_t step = log_count_samples / samples_count;
    uint64_t current_time = 0;
    size_t event_index = 0;
    uint64_t total_time = source->samples[event_index].count_ticks;

    // static float last_filtered[AUDIO_SRC_CHANNELS_MAX] = {0}; // Uchováváme poslední filtrovaný vzorek pro plynulost

    for (size_t i = 0; i < samples_count; i++, current_time += step)
    {
        while (current_time >= (total_time - 1))
        {
            if (event_index < source->samples_count)
            {
                event_index++;
                total_time += source->samples[event_index].count_ticks;
            }
            else
            {
                total_time = log_count_samples;
                break;
            };
        };

        uint8_t current_value = source->samples[event_index].value;
        float resampled = SN76489_volume_value[current_value];

#if 1
        uint8_t end_value = current_value;
        // Pokud je hodnota zaparkovaná na konstantni hodnote, tak postupne snizujeme hodnotu k 0
        static uint8_t parking_previous_resampled[AUDIO_SRC_CHANNELS_MAX] = {0};
        static size_t parking_counter[AUDIO_SRC_CHANNELS_MAX] = {0};
        static float parking_gain[AUDIO_SRC_CHANNELS_MAX] = {0};
        static int parking_gain_counter[AUDIO_SRC_CHANNELS_MAX] = {0};

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

        float iir_x = 6.0f;
        static float sample_value[AUDIO_SRC_CHANNELS_MAX] = {0}; // posledni hodnota pro IIR filtr
        sample_value[channel] = sample_value[channel] + (resampled - sample_value[channel]) / iir_x;
        float filtered1 = sample_value[channel];

        // Aplikujeme jednopólový filtr
        // float filtered1 = iface_audio_lowpass_filter(channel, resampled);
        // float filtered1 = resampled;

        // float filtered2 = iface_audio_lowpass_filter_2nd_order(filtered1);
        float filtered2 = filtered1;

        // Aplikujeme Butterworthův filtr
        // float filtered3 = iface_audio_lowpass_filter_butterworth(channel, filtered2);
        float filtered3 = filtered2;

        // float filtered4 = iface_audio_anti_glitch_filter(filtered3);
        float filtered4 = filtered3;

        // float final_output = iface_audio_anti_glitch_limiter(filtered4);
        float final_output = filtered4;

        // output_samples[i] = final_output;
        //  output_samples[i] = last_sample_value;

        samples[i] = final_output;
        // samples[i] = (last_filtered[channel] + final_output) * 0.2f;
        // last_filtered[channel] = final_output;

        // output_samples[i] = resampled;
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
    static float last_filtered[AUDIO_SRC_CHANNELS_MAX] = {0}; // Uchováváme poslední filtrovaný vzorek pro plynulost
    // static float last_sampled_value[AUDIO_SRC_CHANNELS_MAX] = {0};

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
        static uint8_t parking_previous_resampled[AUDIO_SRC_CHANNELS_MAX] = {0};
        static size_t parking_counter[AUDIO_SRC_CHANNELS_MAX] = {0};
        static float parking_gain[AUDIO_SRC_CHANNELS_MAX] = {0};
        static int parking_gain_counter[AUDIO_SRC_CHANNELS_MAX] = {0};

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
        // static float sample_value[AUDIO_SRC_CHANNELS_MAX] = {0}; // posledni hodnota pro IIR filtr
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
