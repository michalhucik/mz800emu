#ifndef IFACE_AUDIO_H
#define IFACE_AUDIO_H

#include "main.h"
#include "app/app_thread.h"
#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/psg/psg.h"
#include "videorec/videorec_audio.h"


// frekvence na jake chceme mit audio output
#define IFACE_AUDIO_SAMPLE_RATE 44100

// frq na ktere je nativne generovan zvuk z i8253 (je to nevyhodne pro prevod na 48kHz i na 44.1kHz)
//#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / GDGCLK_1M1_DIVIDER) // 1M1

// výhodný delič pro prevod na 44.1kHz => 2
//#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / (GDGCLK_1M1_DIVIDER / 2)) // 2M2
#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / (GDGCLK_CTC0_DIVIDER * 2)) // 276.9 kHz

// frq na ktere je nativne generovan zvuk z PSG (je to nevyhodne pro prevod na 48kHz i na 44.1kHz)
//#define IFACE_AUDIO_MZ_SAMPLE_RATE (GDGCLK_BASE / PSG_DIVIDER) // 17734475 / (16 *5) = 221520 Hz

// výhodný delič pro prevod na 44.1kHz => 443.04/44.1 = 10.05
#define IFACE_AUDIO_PSG_SAMPLE_RATE (GDGCLK_BASE / (PSG_DIVIDER / 2)) // 443040 Hz




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
    AUDIO_OUTPUT_t SN76489_volume_value[AUDIO_SRC_CHANNELS_COUNT][PSG_COUNT_VOLUME_LEVELS];

    float gain[AUDIO_SRC_CHANNELS_COUNT]; // nastaveni hlasitosti pro jednotlive zdroje
    float level[AUDIO_SRC_CHANNELS_COUNT]; // prumerna uroven zvuku na kanale pro indikaci
    float total_level;
    int output_channels;  /* aktuální počet output kanálů (1=mono, 2=stereo) */

    /**
     * @brief Synchronizace emulace podle monotónních hodin místo audio callbacku.
     *
     * true = backend neotevřel žádné audio zařízení (headless režim, nebo
     * v GUI selhalo otevření zařízení - viz device_open_failed), takže
     * neexistuje audio callback, který by odebíral připravené audio snímky.
     * iface_audio_20ms_sync() pak ve stavu IFACE_AUDIO_BUFFER_STATE_NORMAL
     * čeká na absolutní termín podle g_get_monotonic_time() (1 / VIDEO_SCREENS_PER_SEC
     * na jednu 20ms synchronizační událost) místo čekání na play_cond.
     * Nastavuje iface_audio_lowlevel_init() před startem emulačního vlákna,
     * za běhu se nemění.
     */
    bool sync_by_timer;
    /** Počátek aktuální řady termínů časové synchronizace (us, g_get_monotonic_time). Jen emu vlákno. */
    int64_t timer_base_us;
    /** Počet synchronizačních událostí od timer_base_us. Jen emu vlákno. */
    uint64_t timer_events;
    /**
     * @brief Mimo headless se nepodařilo inicializovat SDL audio nebo otevřít
     *        výstupní zařízení; emulace běží bez zvuku (sync_by_timer == true).
     *
     * Nastavuje iface_audio_lowlevel_init() před startem emulačního vlákna,
     * za běhu se nemění; UI podle něj jednou zobrazí upozornění. V headless
     * režimu se zařízení záměrně neotevírá, takže false zůstává, pokud
     * neselže ani samotná inicializace sdl3_main_init(); selže-li, může být
     * true i v headless.
     * Invariant: device_open_failed == true => sync_by_timer == true.
     */
    bool device_open_failed;
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

    /**
     * @brief Synchronizace emulace s reálným časem na 20ms synchronizační události.
     *
     * Oznámí audio callbacku další připravený audio snímek. Ve stavu
     * IFACE_AUDIO_BUFFER_STATE_NORMAL (100 % nebo vlastní rychlost) pak blokuje:
     * - s audio zařízením (sync_by_timer == false): dokud callback snímek
     *   neodebere (tempo udává audio zařízení), nejdéle cca 1 s, pak vypíše
     *   varování "timeout! Is Audio module running?" a pokračuje;
     * - bez audio zařízení (sync_by_timer == true, headless nebo GUI po
     *   selhání otevření zařízení): do absolutního
     *   termínu `timer_base_us + k / VIDEO_SCREENS_PER_SEC s` (k = počet
     *   událostí od založení řady) podle monotónních hodin - termíny jsou
     *   absolutní, bez driftu. Zpoždění do 5 period se dohání (průměrné tempo
     *   zůstává přesné), větší (pauza, MAX SPEED, pomalý hostitel) řadu
     *   termínů založí znovu od aktuálního času.
     * Ve stavu UNSYNC (MAX SPEED) a PAUSED neblokuje.
     *
     * @pre Volá jen emulační vlákno, po iface_audio_init().
     * @post Při ukončování aplikace (sdlapp neběží) může zavolat emulator_quit().
     */
    extern void iface_audio_20ms_sync(void);
    extern void iface_audio_set_src_volume(int id, int volume);
    extern void iface_audio_set_master_volume(int volume);

    /**
     * @brief Sestaví tabulku úrovní zvukových kanálů pro renderer video záznamu.
     *
     * Odpovídá mixu SDL cesty (iface_audio_mix_channels_with_gain()):
     * - kanál 0 (CTC0): SDL cesta počítá podíl nenulových vzorků dělený
     *   `(délka bloku + 1)`, úroveň je tedy 0 pro hodnotu 0 a pro nenulovou
     *   hodnotu střední zesílení bloku videorec_audio_sdl_ctc0_gain()
     *   (cca 0,926 u MZ-800), násobené `gain[0]`;
     * - kanály PSG: hlasitostní tabulka `SN76489_volume_value` (AUDIO_OUTPUT_t
     *   je při AUDIO_FORMAT_F32 `float` v rozsahu 0..1) krát `gain[ch]`, děleno
     *   PSG_CHANNELS_COUNT (průměr přes 4 kanály jako v SDL mixu).
     * Mix PSG0 a PSG1 (stereo PSG na MZ-800 s allow_psg1) se v rendereru sčítá
     * do mono (L == R). Filtry SDL cesty (parkování, low-pass, IIR) aplikuje
     * renderer sám (videorec_audio.h), tabulka obsahuje jen úrovně.
     *
     * @param level    Výstup `level[kanál][hodnota]`, aspoň `channels` řádků.
     * @param channels Počet řádků k vyplnění; řádky nad AUDIO_SRC_CHANNELS_COUNT
     *                 se nemění (volající je má mít vynulované).
     *
     * @pre Volat po iface_audio_init() a audio_init() (nastavené hlasitosti
     *      a gain z INI). Bez zámku - čte konfiguraci, kterou mění UI vlákno;
     *      souběžná změna hlasitosti může dát smíšenou tabulku (neškodné).
     * @note Při MZ800EMU_CFG_AUDIO_DISABLED je hlasitostní tabulka nulová,
     *       PSG kanály pak mají úroveň 0.
     */
    extern void iface_audio_build_videorec_levels(float level[][VIDEOREC_AUDIO_LEVELS], unsigned channels);

    extern float *iface_audio_wait_for_data(size_t *samples_size);

    /***************************************************
     *
     * Verejne funkce pozadovane v lowlevel implementaci
     *
     */
    /**
     * @brief Inicializace audio backendu (otevření výstupního zařízení).
     *
     * S reálným zařízením nastaví stav bufferu a zavolá
     * iface_audio_pause_emulation(0) - tempo emulace pak udává audio callback.
     * V headless režimu (`--headless`) zařízení neotevírá, nastaví
     * g_iface_audio.sync_by_timer = true (tempo podle systémových hodin)
     * a stav bufferu ponechá NORMAL.
     * Selže-li mimo headless inicializace SDL audio subsystému nebo otevření
     * výstupního zařízení, vypíše na stderr anglické varování a pokračuje
     * stejně jako headless (sync_by_timer = true, stav NORMAL) a navíc
     * nastaví g_iface_audio.device_open_failed = true - emulace běží 100 %
     * reálným časem bez zvuku.
     *
     * @return Vždy true (se zařízením, headless i v náhradním režimu bez
     *         zařízení); selhání audia aplikaci nezastaví.
     * @pre Volá iface_audio_init() po iface_audio_buffer_init(), před startem
     *      emulačního vlákna.
     */
    extern bool iface_audio_lowlevel_init(void);
    extern void iface_audio_lowlevel_quit(void);
    /**
     * @brief Pozastaví audio zařízení (při pauze emulace).
     *
     * Bez otevřeného zařízení nic nedělá; varování o NULL streamu vypíše jen
     * mimo headless režim (v headless je NULL stream očekávaný).
     */
    extern void iface_audio_lowlevel_pause(void);
    /**
     * @brief Obnoví audio zařízení (po pauze emulace).
     *
     * Bez otevřeného zařízení nic nedělá; varování o NULL streamu vypíše jen
     * mimo headless režim (v headless je NULL stream očekávaný).
     */
    extern void iface_audio_lowlevel_resume(void);

#ifdef __cplusplus
}
#endif
#endif // IFACE_AUDIO_H
