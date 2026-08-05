#ifndef IFACE_AUDIO_H
#define IFACE_AUDIO_H

#include "main.h"
#include "app/app_thread.h"
#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "hw-generic/psg/psg.h"


// frekvence na jake chceme mit audio output
#define IFACE_AUDIO_SAMPLE_RATE 44100

// frq na ktere je nativne generovan zvuk z i8253 (je to nevyhodne pro prevod na 48kHz i na 44.1kHz)
//#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / GDGCLK_1M1_DIVIDER) // 1M1

// výhodný delič pro prevod na 44.1kHz => 2
//#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / (GDGCLK_1M1_DIVIDER / 2)) // 2M2
/* IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / (GDGCLK_CTC0_DIVIDER*2))
 * zrušeno (mzhal 11c-2c): jediný konzument (iface_audio.c resampler)
 * čte výraz runtime z g_mzhal od dávky 10b. */

// frq na ktere je nativne generovan zvuk z PSG (je to nevyhodne pro prevod na 48kHz i na 44.1kHz)

// výhodný delič pro prevod na 44.1kHz => 443.04/44.1 = 10.05




#define AUDIO_FORMAT_S16 1 // 16-bit signed integer
#define AUDIO_FORMAT_S32 2 // 32-bit signed integer
#define AUDIO_FORMAT_F32 3 // 32-bit floating point

#define AUDIO_FORMAT AUDIO_FORMAT_F32 // Jaky format vystupniho streamu budeme pouzivame (S16, S32, F32)
// #define AUDIO_FORMAT AUDIO_FORMAT_S16 // Jaky format vystupniho streamu budeme pouzivame (S16, S32, F32)

#if AUDIO_FORMAT == AUDIO_FORMAT_S16
#define AUDIO_FORMAT_NAME "S16"
typedef int16_t AUDIO_OUTPUT_t;
#define AUDIO_OUTPUT_MAX_VALUE 0x7fff
#endif

#if AUDIO_FORMAT == AUDIO_FORMAT_S32
#define AUDIO_FORMAT_NAME "S32"
typedef int32_t AUDIO_OUTPUT_t;
#define AUDIO_OUTPUT_MAX_VALUE 0x7fffffff
#endif

#if AUDIO_FORMAT == AUDIO_FORMAT_F32
#define AUDIO_FORMAT_NAME "F32"
typedef float AUDIO_OUTPUT_t;
#define AUDIO_OUTPUT_MAX_VALUE 1
#endif

typedef enum en_IFACE_AUDIO_BUFFER_STATE
{
    IFACE_AUDIO_BUFFER_STATE_NORMAL = 0,
    IFACE_AUDIO_BUFFER_STATE_UNSYNC,
    IFACE_AUDIO_BUFFER_STATE_PAUSED,
    IFACE_AUDIO_BUFFER_STATE_EXITING,
} en_IFACE_AUDIO_BUFFER_STATE;

typedef struct st_IFACE_AUDIO
{
    en_IFACE_AUDIO_BUFFER_STATE state;
    en_IFACE_AUDIO_BUFFER_STATE state_beffore_pause;

    app_mutex_t *mutex;
    app_cond_t *frame_cond;
    app_cond_t *play_cond;
    uint32_t prepared_frame;
    uint32_t played_frame;

    AUDIO_OUTPUT_t channel_scan_value;
    AUDIO_OUTPUT_t SN76489_volume_value[AUDIO_SRC_CHANNELS_MAX][PSG_COUNT_VOLUME_LEVELS];

    float gain[AUDIO_SRC_CHANNELS_MAX]; // nastaveni hlasitosti pro jednotlive zdroje
    float level[AUDIO_SRC_CHANNELS_MAX]; // prumerna uroven zvuku na kanale pro indikaci (superset, mzhal 11c-2)
    float total_level;
    int output_channels;  /* aktuální počet output kanálů (1=mono, 2=stereo) */
} st_IFACE_AUDIO;

extern st_IFACE_AUDIO g_iface_audio;

#ifdef __cplusplus
extern "C"
{
#endif
    /***************************************************
     *
     * Verejne funkce obsazene v iface_audio.c
     *
     */

    extern bool iface_audio_init(void);
    extern void iface_audio_exit(void);
    extern void iface_audio_update_buffer_state(void);
    extern void iface_audio_pause_emulation(unsigned value);
    extern void iface_audio_20ms_sync(void);
    extern void iface_audio_set_src_volume(int id, int volume);
    extern void iface_audio_set_master_volume(int volume);

    extern float *iface_audio_wait_for_data(size_t *samples_size);

    /***************************************************
     *
     * Verejne funkce pozadovane v lowlevel implementaci
     *
     */
    extern bool iface_audio_lowlevel_init(void);
    extern void iface_audio_lowlevel_quit(void);
    extern void iface_audio_lowlevel_pause(void);
    extern void iface_audio_lowlevel_resume(void);

#ifdef __cplusplus
}
#endif
#endif // IFACE_AUDIO_H
