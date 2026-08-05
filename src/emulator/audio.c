/*
 * File:   audio.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 28. července 2015, 13:38
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "mzarch/mzhal.h"
#include <glib.h>

// Lokalizace
#include "i18n.h"

#include "audio.h"
#include "hw-generic/psg/psg.h"

#include "cfgmain.h"
#include "emulator.h"
#include "iface/iface_audio.h"

st_AUDIO g_audio;

#define MAX_UINT32 0xFFFFFFFFu // Maximální hodnota unsigned int (32bit)
#define DEFAULT_VOLUME (1 * MAX_UINT32)

uint32_t audio_float01_to_uint32(float value)
{
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    uint64_t ret = (uint64_t)roundf(value * MAX_UINT32);
    return (uint32_t) (ret < MAX_UINT32) ? ret : MAX_UINT32;
}

float audio_uint32_to_float01(uint32_t value)
{
    return (float)value / (float)MAX_UINT32;
}

static inline st_AUDIO_LOG *audio_log_create(uint64_t first_timestamp, bool stereo)
{
    st_AUDIO_LOG *log = (st_AUDIO_LOG *)malloc(sizeof(st_AUDIO_LOG));
    log->stereo = stereo;
    log->first_timestamp = first_timestamp;
    log->last_timestamp = first_timestamp;
    log->last_psg_timestamp = first_timestamp;

    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_MAX; src_id++)
    {
        st_AUDIO_SOURCE_LOG *src = (st_AUDIO_SOURCE_LOG *)malloc(sizeof(st_AUDIO_SOURCE_LOG));
        src->samples = NULL;
        src->samples_count = 0;
        src->last_timestamp = first_timestamp;
        src->last_value = 0;
        log->src[src_id] = src;
    };

    return log;
}

static inline void audio_log_init_src(st_AUDIO_LOG *log, int src_id, AUDIO_BUF_t sample_value)
{
    st_AUDIO_SOURCE_LOG *src = log->src[src_id];
    src->last_value = sample_value;
}

void audio_log_destroy(st_AUDIO_LOG *audio_log)
{
    if (audio_log == NULL)
    {
        return;
    };

    for (int i = 0; i < AUDIO_SRC_CHANNELS_MAX; i++)
    {
        if (audio_log->src[i] != NULL)
        {
            if (audio_log->src[i]->samples != NULL)
            {
                free(audio_log->src[i]->samples);
            };
            free(audio_log->src[i]);
            audio_log->src[i] = NULL;
        };
    };

    free(audio_log);
}

void audio_propagatecfg_volume(void *e, void *data)
{
    uint32_t volume = cfgelement_get_unsigned_value((CFGELM *)e);
    float *gain = (float *)data;
    *gain = audio_uint32_to_float01(volume);
}

void audio_savecfg_volume(void *e, void *data)
{
    float *gain = (float *)data;
    uint32_t volume = audio_float01_to_uint32(*gain);
    cfgelement_set_unsigned_value((CFGELM *)e, volume);
}

void audio_print_volume_info(void)
{
    g_print("\n");
    const char *volume_info = _("Audio volume settings:");
    g_print("%s\n", volume_info);
    GString *str = g_string_new(NULL);
    for (size_t i = 0; i < strlen(volume_info); i++)
    {
        g_string_append_c(str, ' ');
    };
    g_print("%sCTC0  : %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SRC_CTC0] * 100.0f);
    if (g_mzhal.audio_src_channels > 1) {
    g_print("%sPSG0_0: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0] * 100.0f);
    g_print("%sPSG0_1: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1] * 100.0f);
    g_print("%sPSG0_2: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2] * 100.0f);
    g_print("%sPSG0_3: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3] * 100.0f);
    };
    /* PSG1: MZ-1500 vzdy stereo, MZ-800 jen kdyz allow_psg1=YES */
    if (g_mzhal.psg_count == 2 && g_psg_module.stereo)
    {
        g_print("%sPSG1_0: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0] * 100.0f);
        g_print("%sPSG1_1: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1] * 100.0f);
        g_print("%sPSG1_2: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2] * 100.0f);
        g_print("%sPSG1_3: %7.2f %%\n", str->str, g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3] * 100.0f);
    }
    g_print("\n");
    g_string_free(str, TRUE);
}

void audio_init(void)
{

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "AUDIO");

    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "volume_8253", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SRC_CTC0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SRC_CTC0]);

    if (g_mzhal.audio_src_channels > 1) {
    elm = cfgmodule_register_new_element(cmod, "volume_psg0", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_0]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_1]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg2", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_2]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg3", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG0 + PSG_CHANNEL_3]);
    };

    if (g_mzhal.psg_count == 2) {
    /* PSG1 volume konfigurace — jen pro stereo PSG (MZ-1500) */
    elm = cfgmodule_register_new_element(cmod, "volume_psg1_0", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_0]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_1", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_1]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_2", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_2]);

    elm = cfgmodule_register_new_element(cmod, "volume_psg1_3", CFGENTYPE_UNSIGNED, DEFAULT_VOLUME, 0, MAX_UINT32);
    cfgelement_set_propagate_cb(elm, audio_propagatecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3]);
    cfgelement_set_save_cb(elm, audio_savecfg_volume, &g_iface_audio.gain[AUDIO_SOURCE_ID_SHIFT_PSG1 + PSG_CHANNEL_3]);
    };

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    g_audio.log = audio_log_create(0, g_psg_module.stereo);
    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_MAX; src_id++)
    {
        audio_log_init_src(g_audio.log, src_id, 0);
    };

    g_audio.old_log = NULL;
    APP_MUTEX_CREATE(g_audio.old_log_mutex);

    g_audio.ctc0_output = 0;

    audio_print_volume_info();
}

void audio_reset_log(uint64_t new_timestamp)
{
    /* Zničit starý log a vytvořit čistý s novým timestampem */
    audio_log_destroy(g_audio.log);
    g_audio.log = audio_log_create(new_timestamp, g_psg_module.stereo);
    for (int src_id = 0; src_id < AUDIO_SRC_CHANNELS_MAX; src_id++) {
        audio_log_init_src(g_audio.log, src_id, 0);
    }

    /* Zahodit i old_log — obsahuje zastaralá data */
    APP_MUTEX_LOCK(g_audio.old_log_mutex);
    audio_log_destroy(g_audio.old_log);
    g_audio.old_log = NULL;
    APP_MUTEX_UNLOCK(g_audio.old_log_mutex);
}


void audio_exit(void)
{
    audio_log_destroy(g_audio.log);
    g_audio.log = NULL;

    if (g_audio.old_log_mutex)
    {
        APP_MUTEX_LOCK(g_audio.old_log_mutex);
        audio_log_destroy(g_audio.old_log);
        g_audio.old_log = NULL;
        APP_MUTEX_UNLOCK(g_audio.old_log_mutex);

        APP_MUTEX_DESTROY(g_audio.old_log_mutex);
        g_audio.old_log_mutex = NULL;
    };
}

static inline void audio_changed(st_AUDIO_LOG *log, en_AUDIO_SOURCE audio_source, AUDIO_BUF_t value, uint64_t total_event_ticks)
{
    // st_AUDIO_LOG *log = g_audio.log;

    int src_id = audio_source_id(audio_source);

    st_AUDIO_SOURCE_LOG *src = log->src[src_id];

    if (src->last_value == value)
    {
        return;
    };

    st_AUDIO_LOG_EVENT *last_sample = NULL;

    if (src->samples_count == 0)
    {
        src->samples = (st_AUDIO_LOG_EVENT *)malloc(sizeof(st_AUDIO_LOG_EVENT));
        last_sample = &src->samples[0];
        src->samples_count = 1;
        last_sample->value = src->last_value;
    }
    else
    {
        last_sample = &src->samples[src->samples_count - 1];
    };

    last_sample->count_ticks = total_event_ticks - src->last_timestamp;
    src->last_timestamp = total_event_ticks;

    st_AUDIO_LOG_EVENT *new_samples = (st_AUDIO_LOG_EVENT *)realloc(src->samples, (src->samples_count + 1) * sizeof(st_AUDIO_LOG_EVENT));
    if (new_samples == NULL)
    {
        fprintf(stderr, "%s():%d - Failed to reallocate memory for audio log\n", __func__, __LINE__);
        return;
    };
    src->samples = new_samples;
    st_AUDIO_LOG_EVENT *sample = &src->samples[src->samples_count];
    src->samples_count++;

    sample->value = value;
    sample->count_ticks = 0;
    src->last_value = value;
}

void audio_ctc0_changed(bool ctc0_state, uint64_t total_event_ticks)
{
    AUDIO_BUF_t value = (ctc0_state) ? AUDIO_MAXVAL_PER_CHANNEL : 0;
    audio_changed(g_audio.log, AUDIO_SRC_CTC0, value, total_event_ticks);
    g_audio.ctc0_output = ctc0_state;
}

void audio_log_fill_psg(uint64_t total_event_ticks)
{
    /* Platformy bez PSG: pouze udrzime last_psg_timestamp v synchronizaci,
     * abychom v audiolog_finish_20ms_frame mohli pouzit log->last_psg_timestamp
     * jako konzistentni casovou znacku zacatku noveho frame logu. */
    if (g_mzhal.psg_count == 0)
    {
        g_audio.log->last_psg_timestamp = total_event_ticks;
        return;
    };

    st_AUDIO_LOG *log = g_audio.log;

    /* Loop-invariant hoist: cteni g_mzhal NEpatri dovnitr scan smycky.
     * psg_divider runtime z g_mzhal (mzhal 10c; hodnota per EXE beze
     * zmeny, ve smycce jen compare + add). */
    const int psg1_present = (g_mzhal.psg_count == 2);
    const uint64_t psg_divider = g_mzhal.psg_divider;

    while ((total_event_ticks - log->last_psg_timestamp) >= psg_divider)
    {
        psg_step();   /* Steppuje PSG0 (a PSG1 pokud stereo) */

        for (int channel = 0; channel < PSG_CHANNELS_COUNT; channel++)
        {
            /* PSG0 logování */
            AUDIO_BUF_t scan_value = 0;
            if (g_psg.channel[channel].attn != PSG_OUT_OFF)
            {
                if (g_psg.channel[channel].output_signal)
                {
                    scan_value = AUDIO_MAXVAL_PER_CHANNEL - g_psg.channel[channel].attn;
                };
            };
            audio_changed(g_audio.log, (en_AUDIO_SOURCE)(AUDIO_SOURCE_FLAG_PSG0 | channel), scan_value, log->last_psg_timestamp);

            /* PSG1 logování — stereo PSG (MZ-1500 nativne, MZ-800 pres allow_psg1).
             * Runtime check g_psg_module.stereo pro MZ-800 dynamic case. */
            if (psg1_present && g_psg_module.stereo)
            {
                AUDIO_BUF_t scan_value1 = 0;
                if (g_psg_module.psg[1].channel[channel].attn != PSG_OUT_OFF)
                {
                    if (g_psg_module.psg[1].channel[channel].output_signal)
                    {
                        scan_value1 = AUDIO_MAXVAL_PER_CHANNEL - g_psg_module.psg[1].channel[channel].attn;
                    };
                };
                audio_changed(g_audio.log, (en_AUDIO_SOURCE)(AUDIO_SOURCE_FLAG_PSG1 | channel), scan_value1, log->last_psg_timestamp);
            }
        };

        log->last_psg_timestamp += psg_divider;
    };
}

void audiolog_finish_20ms_frame(uint64_t total_event_ticks)
{
    // dokoncime PSG
    audio_log_fill_psg(total_event_ticks);

    // zaevidujeme konec frame
    st_AUDIO_LOG *log = g_audio.log;
    log->last_timestamp = total_event_ticks;

    // uzavreme posledni eventy (runtime pocet kanalu platformy)
    for (int i = 0; i < (int)g_mzhal.audio_src_channels; i++)
    {
        st_AUDIO_SOURCE_LOG *src = log->src[i];
        if (src->samples_count > 0)
        {
            st_AUDIO_LOG_EVENT *last_sample = &src->samples[src->samples_count - 1];
            last_sample->count_ticks = total_event_ticks - src->last_timestamp;
        };
    };

    // Vytvorime log pro nasledujici frame
    st_AUDIO_LOG *new_log = audio_log_create(log->last_psg_timestamp, g_psg_module.stereo);

    // Zkopirujeme posledni hodnoty do noveho frame
    int first_src_id = 1; // Preskocime CTC0?
    uint32_t ticks_remaining = total_event_ticks - log->last_psg_timestamp;
    if ((ticks_remaining == 0) || (log->src[0]->samples_count == 0))
    {
        first_src_id = 0; // Nepreskocime CTC0
    }
    else
    {
        // Budeme muset zkopirovat i presahujici eventy z CTC0
        st_AUDIO_SOURCE_LOG *src = log->src[0];
        // Nejprve najdeme prvni event, ktery se nevesel do uzavreneho frame
        unsigned first_event = src->samples_count - 1;
        uint64_t event_timestamp = 0;
        while (first_event > 0)
        {
            st_AUDIO_LOG_EVENT *e = &src->samples[first_event];
            if (e->count_ticks >= ticks_remaining)
            {
                event_timestamp = new_log->first_timestamp + ticks_remaining;
                break;
            };
            ticks_remaining -= e->count_ticks;
            first_event--;
        };

        // Zinicializujeme novy log CTC0 podle posledniho nedokonceneho eventu
        audio_log_init_src(new_log, 0, src->samples[first_event].value);

        // Zkopirujeme posledni eventy CTC0
        first_event++;
        if (first_event < src->samples_count)
        {
            for (unsigned i = first_event; i < src->samples_count; i++)
            {
                st_AUDIO_LOG_EVENT *e = &src->samples[i];
                audio_changed(new_log, AUDIO_SRC_CTC0, e->value, event_timestamp);
                event_timestamp += e->count_ticks;
            };

            // odstranime posledni eventy
            src->samples_count = first_event;
        };
    };

    // Zkopirujeme posledni hodnoty PSG (runtime pocet kanalu platformy)
    for (int i = first_src_id; i < (int)g_mzhal.audio_src_channels; i++)
    {
        st_AUDIO_SOURCE_LOG *src = log->src[i];
        audio_log_init_src(new_log, i, src->last_value);
    };

    // Swap logu
    if (EMULATOR_TEST_NORMAL_SPEED)
    {
        if (g_audio.old_log != NULL)
        {
            audio_log_destroy(g_audio.old_log);
        };
        g_audio.old_log = log;
    }
    else // MZ800_EMULATION_SPEED_MAX
    {
        APP_MUTEX_LOCK(g_audio.old_log_mutex);
        if (g_audio.old_log != NULL)
        {
            audio_log_destroy(g_audio.old_log);
        };
        g_audio.old_log = log;
        APP_MUTEX_UNLOCK(g_audio.old_log_mutex);
    };
    g_audio.log = new_log;
}
