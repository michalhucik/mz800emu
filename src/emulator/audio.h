/*
 * File:   audio.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 28. července 2015, 13:39
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

#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"

#include "mzarch/mzcommon_config.h" /* jen společné toggly; per-arch
                                       nic (mzhal 11c-2) - includy
                                       video.h/gdg.h byly vestigiální */
#include "hw-generic/psg/psg.h"

    /* Superset dimenze polí (mzhal krok 8): pole se dimenzují na maximum
     * napříč platformami (CTC0 + 2x4 PSG); skutečný počet kanálů za běhu
     * čti z g_mzhal.audio_src_channels. */
#define AUDIO_SRC_CHANNELS_MAX 9

    typedef int8_t AUDIO_BUF_t; // Musi to byt jednobajtovy typ !!!

#define AUDIO_MAXVAL_PER_CHANNEL 15

    // extern AUDIO_BUF_t g_attenuator_volume_value [ PSG_OUT_OFF + 1 ];
    extern AUDIO_BUF_t g_attenuator_volume_value[4][16];


#define AUDIO_SOURCE_FLAG_PSG0 0x10
#define AUDIO_SOURCE_MASK_PSG0 0x03
#define AUDIO_SOURCE_ID_SHIFT_PSG0 1
// predbezne pro MZ-1500
#define AUDIO_SOURCE_FLAG_PSG1 0x20
#define AUDIO_SOURCE_MASK_PSG1 0x03
#define AUDIO_SOURCE_ID_SHIFT_PSG1 5

    typedef enum en_AUDIO_SOURCE
    {
        AUDIO_SRC_CTC0 = 0,
        AUDIO_SRC_PSG0_0 = AUDIO_SOURCE_FLAG_PSG0,
        AUDIO_SRC_PSG0_1,
        AUDIO_SRC_PSG0_2,
        AUDIO_SRC_PSG0_3,
        // predbezne pro MZ-1500
        AUDIO_SRC_PSG1_0 = AUDIO_SOURCE_FLAG_PSG1,
        AUDIO_SRC_PSG1_1,
        AUDIO_SRC_PSG1_2,
        AUDIO_SRC_PSG1_3,
    } en_AUDIO_SOURCE;

    static inline size_t audio_source_id(en_AUDIO_SOURCE source)
    {
        if (source == AUDIO_SRC_CTC0)
            return 0;
        else if (source & AUDIO_SOURCE_FLAG_PSG0)
        {
            return (source & AUDIO_SOURCE_MASK_PSG0) + AUDIO_SOURCE_ID_SHIFT_PSG0;
        };
        return (source & AUDIO_SOURCE_MASK_PSG1) + AUDIO_SOURCE_ID_SHIFT_PSG1;
    }

    typedef struct st_AUDIO_LOG_EVENT
    {
        uint32_t count_ticks;
        AUDIO_BUF_t value;
    } st_AUDIO_LOG_EVENT;

    typedef struct st_AUDIO_SOURCE_LOG
    {
        uint64_t last_timestamp;
        AUDIO_BUF_t last_value;
        st_AUDIO_LOG_EVENT *samples;
        unsigned samples_count;
    } st_AUDIO_SOURCE_LOG;

    typedef struct st_AUDIO_LOG
    {
        bool stereo;                /* true = stereo (2 PSG), false = mono */
        uint64_t first_timestamp;
        uint64_t last_timestamp;
        uint64_t last_psg_timestamp;
        st_AUDIO_SOURCE_LOG *src[AUDIO_SRC_CHANNELS_MAX];
    } st_AUDIO_LOG;

    typedef struct st_AUDIO
    {
        unsigned ctc0_output; // Indikuje aktualni stav vystupu z ctc0 (0/1)

        st_AUDIO_LOG *log;
        st_AUDIO_LOG *old_log;
        app_mutex_t *old_log_mutex;
    } st_AUDIO;

    extern st_AUDIO g_audio;

    extern void audio_init(void);

    extern void audio_ctc0_changed(bool ctc0_state, uint64_t total_event_ticks);
    extern void audio_log_fill_psg(uint64_t total_event_ticks);
    extern void audiolog_finish_20ms_frame(uint64_t total_event_ticks);
    extern void audio_log_destroy(st_AUDIO_LOG *audio_log);
    extern void audio_reset_log(uint64_t new_timestamp);
    extern void audio_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
