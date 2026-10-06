/**
 * @file sdl3_audio.c
 * @brief SDL3 backend audio výstupu (lowlevel část iface_audio).
 *
 * Otevírá výstupní audio stream s callbackem, který odebírá připravené
 * audio snímky z iface_audio (a tím udává tempo emulace). Bez zařízení
 * (headless, nebo po selhání SDL audia či otevření zařízení v GUI) se
 * nic neotevírá a tempo emulace řídí monotónní hodiny (sync_by_timer).
 */

#include "main.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>
#include <string.h>
#include <math.h>

#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "iface/iface_audio.h"

#include "mzarch/mzarch.h"
#include "audio.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#endif

#if AUDIO_FORMAT == AUDIO_FORMAT_S16
#define LOCAL_AUDIO_FORMAT SDL_AUDIO_S16
#endif

#if AUDIO_FORMAT == AUDIO_FORMAT_S32
#define LOCAL_AUDIO_FORMAT SDL_AUDIO_S32
#endif

#if AUDIO_FORMAT == AUDIO_FORMAT_F32
#define LOCAL_AUDIO_FORMAT SDL_AUDIO_F32
#endif

#ifndef LOCAL_AUDIO_FORMAT
#error "Unsupported audio format"
#endif

SDL_AudioStream *g_audio_stream = NULL;

static void SDLCALL sdl3_audio_callback(void *userdata, SDL_AudioStream *astream, int additional_amount, int total_amount)
{
    (void)userdata;
    (void)total_amount;

    // g_print("%s(%d) - additional_amount: %d\n", __func__, __LINE__, additional_amount);

    if (additional_amount == 0)
    {
        // g_print("%s(%d) - DONE! additional_amount is zero\n", __func__, __LINE__);
        return;
    };

    size_t samples_size = 0;
    float *samples = iface_audio_wait_for_data(&samples_size);
    if (samples == NULL)
    {
        // g_print("%s(%d) - DONE! No audio data\n", __func__, __LINE__);
        return;
    };

    SDL_PutAudioStreamData(astream, samples, samples_size);
    SDL_free(samples);
}

void iface_audio_lowlevel_pause(void)
{
    if (g_audio_stream != NULL)
    {
        // bool is_paused = SDL_AudioStreamDevicePaused(g_audio_stream);
        // g_print("%s(%d) - SDL audio stream state: %s\n", __func__, __LINE__, (is_paused) ? "paused" : "running");
        SDL_PauseAudioStreamDevice(g_audio_stream);
        // is_paused = SDL_AudioStreamDevicePaused(g_audio_stream);
        // g_print("%s(%d) - DONE! SDL audio stream state: %s\n", __func__, __LINE__, (is_paused) ? "paused" : "running");
    }
    else if (!g_iface_audio.sync_by_timer) /* headless: NULL stream je očekávaný stav */
    {
        g_print("%s(%d) - SDL audio stream is NULL\n", __func__, __LINE__);
    };
}

void iface_audio_lowlevel_resume(void)
{
    if (g_audio_stream != NULL)
    {
        // bool is_paused = SDL_AudioStreamDevicePaused(g_audio_stream);
        // g_print("%s(%d) - SDL audio stream state: %s\n", __func__, __LINE__, (is_paused) ? "paused" : "running");
        SDL_ResumeAudioStreamDevice(g_audio_stream);
        // is_paused = SDL_AudioStreamDevicePaused(g_audio_stream);
        // g_print("%s(%d) - DONE! SDL audio stream state: %s\n", __func__, __LINE__, (is_paused) ? "paused" : "running");
    }
    else if (!g_iface_audio.sync_by_timer) /* headless: NULL stream je očekávaný stav */
    {
        g_print("%s(%d) - SDL audio stream is NULL\n", __func__, __LINE__);
    };
}

/**
 * @brief Náhradní režim bez audio zařízení po selhání SDL audia mimo headless.
 *
 * Nahrazuje dřívější návrat false, který shodil iface_init() a emulace se
 * vůbec nespustila. Přepne synchronizaci emulace na monotónní hodiny
 * (g_iface_audio.sync_by_timer - stejná cesta jako headless režim, včetně
 * výroby zvuku pro video záznam podle reality) a nastaví
 * g_iface_audio.device_open_failed, podle kterého UI jednou zobrazí
 * upozornění. Stav bufferu zůstává NORMAL z iface_audio_buffer_init(), takže
 * emulace běží 100 % reálným časem bez zvuku; pauza, vlastní rychlost
 * a MAX SPEED procházejí stejným stavovým automatem jako se zařízením.
 *
 * @param reason Krátký anglický popis selhání pro log.
 * @param detail Text chyby z SDL_GetError() (nesmí být NULL, může být prázdný).
 * @return Vždy true - inicializace audia pokračuje bez zařízení.
 * @pre g_audio_stream == NULL; volá jen iface_audio_lowlevel_init() před
 *      startem emulačního vlákna (členy g_iface_audio bez zámku).
 * @post g_iface_audio.sync_by_timer == true a g_iface_audio.device_open_failed == true.
 */
static bool sdl3_audio_fallback_no_device(const char *reason, const char *detail)
{
    g_printerr("WARNING: %s (%s) - running without sound, emulation synced by system clock\n", reason, detail);
    g_iface_audio.sync_by_timer = true;
    g_iface_audio.device_open_failed = true;
    return true;
}

bool iface_audio_lowlevel_init(void)
{
    if (!sdl3_backend_audio_init())
    {
        /* Selhal SDL audio subsystém (žádný použitelný audio driver, např.
         * neplatný SDL_AUDIO_DRIVER). Detail už vypsal backend. */
        return sdl3_audio_fallback_no_device("Failed to initialize SDL audio", SDL_GetError());
    };

    /* Headless režim: žádný SDL audio device neotevíráme. @c g_audio_stream
     * zůstává NULL, což ostatní funkce v tomto souboru (pause/resume) i v
     * iface_audio už korektně ošetřují. Nahoře v iface_audio_init je
     * potřeba zachovat alokaci interních bufferů, takže vracíme TRUE.
     *
     * Bez zařízení neexistuje audio callback, který by udával tempo emulace,
     * proto iface_audio_20ms_sync() synchronizuje podle monotónních hodin
     * (sync_by_timer). Stav bufferu zůstává NORMAL z iface_audio_buffer_init(),
     * stejně jako po iface_audio_pause_emulation(0) s reálným zařízením níže -
     * pauza a MAX SPEED pak procházejí stejným stavovým automatem jako v GUI. */
    if (sdlapp_option_present("--headless"))
    {
        g_print("Headless mode: skipping SDL audio device open, emulation synced by system clock\n");
        g_iface_audio.sync_by_timer = true;
        return true;
    };

    /* Open audio device */

    SDL_AudioSpec spec;
    SDL_zero(spec);

    spec.freq = IFACE_AUDIO_SAMPLE_RATE;
    spec.format = LOCAL_AUDIO_FORMAT;
    spec.channels = 2; /* vždy stereo — mono data zduplikuje iface_audio */

    g_print("Initializing SDL audio: %d Hz, %s, %d bits (%s)\n", spec.freq, (spec.channels == 1) ? "mono" : "stereo", SDL_AUDIO_BITSIZE(spec.format), AUDIO_FORMAT_NAME);

    g_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl3_audio_callback, NULL);
    if (!g_audio_stream)
    {
        /* Subsystém běží, ale výstupní zařízení nejde otevřít (žádné
         * zařízení, odpojené, obsazené). */
        return sdl3_audio_fallback_no_device("Couldn't open audio device", SDL_GetError());
    };

    bool is_paused = SDL_AudioStreamDevicePaused(g_audio_stream);
    g_iface_audio.state = (is_paused) ? IFACE_AUDIO_BUFFER_STATE_PAUSED : IFACE_AUDIO_BUFFER_STATE_NORMAL;

    // g_print("SDL audio stream created - %s\n", (is_paused) ? "paused" : "running");

    // Chceme aby byl vzdy spusten
    iface_audio_pause_emulation(0);

    return true;
}

void iface_audio_lowlevel_quit(void)
{
    if (g_audio_stream != NULL)
    {
        SDL_PauseAudioStreamDevice(g_audio_stream);
    };
    SDL_DestroyAudioStream(g_audio_stream);
    g_audio_stream = NULL;

    sdl3_backend_audio_quit();
}
