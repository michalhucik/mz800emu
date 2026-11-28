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
/**
 * @brief Vzorkovací kmitočet mezilehlého proudu CTC0 v emulovaném čase [vzorky za jmenovitou sekundu].
 *
 * Jeden vzorek na 2 * GDGCLK_CTC0_DIVIDER taktů GDG: MZ-800 a MZ-700 PAL
 * 553,8 kHz (17 721 600 / 32), MZ-1500 a MZ-700 NTSC 448,02 kHz. Počítá se
 * v emulovaných taktech (GDGCLK_BASE), takže na snímek připadá celý počet
 * vzorků (IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME) nezávisle na krystalu.
 */
#define IFACE_AUDIO_CTC5253_SAMPLE_RATE (GDGCLK_BASE / (GDGCLK_CTC0_DIVIDER * 2))

/**
 * @brief Počet vzorků mezilehlého proudu CTC0 na jeden snímek (MZ-800 11 076, NTSC 7467).
 *
 * Délka snímku v taktech děleno 2 * GDGCLK_CTC0_DIVIDER; nezávisí na
 * krystalu ani na počtu výstupních vzorků bloku.
 */
#define IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME (IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC)

/**
 * @brief Nejvyšší počet vzorků bloku SDL cesty (jeden snímek) na kanál.
 *
 * Blok má podle krystalu zlomkový průměr
 * `IFACE_AUDIO_SAMPLE_RATE * VIDEO_SCREEN_TICKS / GDGCLK_REAL_TICKS_PER_SEC`
 * vzorků (PAL 881,3597, NTSC 735,9476); iface_audio_block_clock_next() vydává
 * vždy dolní nebo horní celou část, takže nejdelší blok je horní celá část:
 * PAL 882, NTSC 736. Pro dimenzování polí a jako jmenovitý blok tam, kde
 * na délce bloku nezáleží (iface_audio_ctc0_mix_level()).
 */
#define IFACE_AUDIO_BLOCK_SAMPLES_MAX                                                                   \
    ((size_t)(((uint64_t)IFACE_AUDIO_SAMPLE_RATE * (uint64_t)VIDEO_SCREEN_TICKS + GDGCLK_REAL_TICKS_PER_SEC \
               - 1u) / GDGCLK_REAL_TICKS_PER_SEC))

/**
 * @brief Nejvyšší počet buněk mezilehlého proudu CTC0, které se přenášejí do dalšího bloku.
 *
 * Interval vzorku SDL cesty (iface_audio_block_timing()) leží na souvislé
 * časové ose, takže první vzorek bloku začíná až o necelý vzorek před
 * začátkem snímku. Buňky proudu CTC0 z konce předchozího snímku, které
 * poslední vzorek předchozího bloku nezapočítal nebo započítal jen částí
 * (buňka, ve které leží jeho koncová hrana), se proto přenášejí
 * (iface_audio_resampler_output_stream_ctc0()). Jejich počet je
 * `ceil(rem * M / (44100 * takty snímku))` pro zbytek akumulátoru
 * `rem < GDGCLK_REAL_TICKS_PER_SEC`, tedy nejvýš horní celá část počtu
 * buněk na jeden výstupní vzorek: MZ-800 a MZ-700 PAL 13 (12,57 buňky na
 * vzorek), MZ-1500 a MZ-700 NTSC 11 (10,15).
 */
#define IFACE_AUDIO_CTC0_CARRY_MAX                                                                                  \
    ((size_t)(((uint64_t)GDGCLK_REAL_TICKS_PER_SEC * (uint64_t)IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME                   \
               + (uint64_t)IFACE_AUDIO_SAMPLE_RATE * (uint64_t)VIDEO_SCREEN_TICKS - 1u)                             \
              / ((uint64_t)IFACE_AUDIO_SAMPLE_RATE * (uint64_t)VIDEO_SCREEN_TICKS)))

/**
 * @brief Nejdelší snímek ve zvukovém logu [takty], se kterým počítá iface_audio_block_timing().
 *
 * Hranice vzorku PSG je `floor(hrana * délka / (44100 * takty snímku))`
 * (iface_audio_block_tick()), bod iface_audio_block_point() `ceil(...) - 1`
 * téhož součinu; hrana je nejvýš `44100 * takty snímku`, součin se musí
 * vejít do uint64_t.
 * Snímek vlastní rychlosti (nejvýš CUSTOMSPEED_MAX_VALUE = 4000 %, tedy
 * 40 snímků) je o několik řádů kratší (PAL mez 1,18e9 taktů); delší
 * délku volající ořízne.
 */
#define IFACE_AUDIO_SPAN_TICKS_MAX (UINT64_MAX / ((uint64_t)IFACE_AUDIO_SAMPLE_RATE * (uint64_t)VIDEO_SCREEN_TICKS))

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
    /** Výchylka kanálu pro hodnotu zvukového logu 0 až 31 (iface_audio_set_src_volume()). */
    AUDIO_OUTPUT_t SN76489_volume_value[AUDIO_SRC_CHANNELS_COUNT][AUDIO_LEVEL_VALUES_COUNT];

    float gain[AUDIO_SRC_CHANNELS_COUNT]; // nastaveni hlasitosti pro jednotlive zdroje
    /**
     * @brief Indikace úrovně kanálů 0 až 1 (okno Audio Mixer).
     *
     * Rozkmit kanálu v posledním bloku (největší minus nejmenší vzorek před
     * filtry kanálu) krát hlasitost kanálu gain[]: kanál PSG s útlumem 0
     * i obdélník CTC0 dávají při hlasitosti 100 % hodnotu 1,0, stálá
     * (neslyšitelná) úroveň 0. Náběh je okamžitý, doběh exponenciální
     * (iface_audio_level_follow()). Zapisuje vlákno mixu, čte UI vlákno
     * (jednotlivé hodnoty float, bez zámku).
     */
    float level[AUDIO_SRC_CHANNELS_COUNT];
    /**
     * @brief Indikace celkové úrovně výstupu 0 až 1.
     *
     * Největší absolutní hodnota smíchaného výstupu v posledním bloku za
     * dorovnáním hlasitosti a měkkým omezovačem, před hlavní hlasitostí
     * a ztlumením (ve stereo rozložení větší z obou stran); hodnoty nad
     * AUDIO_OUT_LIMIT_KNEE znamenají, že omezovač špičky stlačuje. Doběh
     * a vlákna jako level[].
     */
    float total_level;
    int output_channels;  /* aktuální počet output kanálů (1=mono, 2=stereo) */

    /**
     * @brief Volba věrné výstupní propusti (INI [AUDIO] output_filter, výchozí zapnuto).
     *
     * Jen při AUDIO_HAVE_OUTPUT_FILTER; na samostatném MZ-700 je vždy false
     * (stroj propust nemá, volba neexistuje).
     *
     * true = smíchaný signál (PSG i CTC0) prochází dolní propustí 1. řádu
     * s časovou konstantou AUDIO_OUT_LP_TAU_US jako mezi čipem PSG a zesilovačem
     * skutečného MZ-800 a filtry jednotlivých kanálů se vynechají; false =
     * dřívější zvuk (low-pass a anti-glitch CTC0, IIR PSG). Platí pro SDL cestu,
     * video záznam i reference.wav; přepnutí za běhu je plynulý přechod.
     * Zapisuje UI vlákno nebo vlákno emulátoru, čtou vlákno emulátoru
     * a vlákno zvukového výstupu (jeden bajt, bez zámku).
     */
    bool output_filter;

    /**
     * @brief Hlavní hlasitost poslechu 0 až AUDIO_MASTER_GAIN_MAX (INI [AUDIO] volume_master a volume_master_boost, výchozí 1).
     *
     * Násobí hotový smíchaný blok těsně před předáním zvukovému zařízení
     * (iface_audio_apply_master()), až za odběrem pro video záznam a pro
     * zachycení zvuku; hlasitosti kanálů (gain[]) ani indikace úrovní
     * (level[], total_level) se jí nemění. Hodnoty nad 1 přidávají (až
     * +6 dB), blok pak projde měkkým omezovačem.
     * Invariant: 0 <= master_gain <= AUDIO_MASTER_GAIN_MAX.
     * Zapisuje UI vlákno nebo vlákno emulátoru (iface_audio_set_master_gain()),
     * čte vlákno zvukového výstupu (jedna hodnota float, bez zámku).
     */
    float master_gain;

    /**
     * @brief Ztlumení poslechu (výchozí vypnuto; do INI se neukládá, po startu je vždy vypnuto).
     *
     * true = zvukové zařízení dostává ticho; master_gain se nemění, takže
     * zrušení ztlumení vrátí původní hlasitost. Na záznam nemá vliv (viz
     * master_gain). Vlákna jako master_gain.
     */
    bool mute;

    /**
     * @brief Synchronizace emulace podle monotónních hodin místo audio callbacku.
     *
     * true = backend neotevřel žádné audio zařízení (headless režim, nebo
     * v GUI selhalo otevření zařízení - viz device_open_failed), takže
     * neexistuje audio callback, který by odebíral připravené audio snímky.
     * iface_audio_20ms_sync() pak ve stavu IFACE_AUDIO_BUFFER_STATE_NORMAL
     * čeká na absolutní termín podle g_get_monotonic_time() (jeden snímek
     * podle krystalu, iface_audio_frames_to_real_us(1), na jednu 20ms
     * synchronizační událost) místo čekání na play_cond.
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
     *   termínu `timer_base_us + iface_audio_frames_to_real_us(k)` (k = počet
     *   událostí od založení řady) podle monotónních hodin - termíny jsou
     *   absolutní, bez driftu. Zpoždění do 5 period se dohání (průměrné tempo
     *   zůstává přesné), větší (pauza, MAX SPEED, pomalý hostitel) řadu
     *   termínů založí znovu od aktuálního času.
     * Ve stavu UNSYNC (MAX SPEED) a PAUSED neblokuje.
     *
     * Tempo je v obou případech podle krystalu: zařízení odebírá 44 100
     * vzorků za sekundu po blocích délky iface_audio_block_clock_next()
     * (průměr VIDEO_SCREEN_TICKS / GDGCLK_REAL_TICKS_PER_SEC s), termíny
     * bez zařízení jsou iface_audio_frames_to_real_us(); obojí dává
     * 50,036326 (PAL) nebo 59,922743 (NTSC) snímků za sekundu.
     *
     * @pre Volá jen emulační vlákno, po iface_audio_init().
     * @post Při ukončování aplikace (sdlapp neběží) může zavolat emulator_quit().
     */
    extern void iface_audio_20ms_sync(void);

    /**
     * @brief Stav rozdělení zvukového výstupu na bloky podle krystalu.
     *
     * Blok SDL cesty je výstup jednoho snímku. Snímek trvá
     * VIDEO_SCREEN_TICKS / GDGCLK_REAL_TICKS_PER_SEC s, tedy zlomkový počet
     * vzorků IFACE_AUDIO_SAMPLE_RATE (PAL 881,3597, NTSC 735,9476).
     * Akumulátor drží zbytek po celočíselném dělení, takže blok má vždy
     * dolní nebo horní celou část a součet za n bloků je přesně
     * `floor(n * IFACE_AUDIO_SAMPLE_RATE * VIDEO_SCREEN_TICKS / GDGCLK_REAL_TICKS_PER_SEC)`
     * (žádný dlouhodobý drift).
     *
     * Zbytek je zároveň fáze vzorků: `rem` před krokem udává v jednotkách
     * `1 / IFACE_AUDIO_SAMPLE_RATE` taktu, o kolik první vzorek bloku začíná
     * před začátkem snímku (iface_audio_block_edge(), iface_audio_block_timing()).
     * Časy vzorků tak leží na jedné souvislé ose bez skoků na hranicích bloků.
     * Stav vede jen výstupní cesta (není součástí emulace, snapshotu, replay
     * ani otisku stroje).
     *
     * Invariant: `rem < GDGCLK_REAL_TICKS_PER_SEC`. Nulový stav je platný
     * počáteční stav.
     */
    typedef struct st_IFACE_AUDIO_BLOCK_CLOCK
    {
        uint64_t rem; /**< Zbytek: zlomek vzorku v jednotkách 1 / GDGCLK_REAL_TICKS_PER_SEC. */
    } st_IFACE_AUDIO_BLOCK_CLOCK;

    /**
     * @brief Obecný krok akumulátoru bloků: vzorků na jeden snímek pro dané hodiny.
     *
     * `rem += rate * frame_ticks`, výsledek je `rem / crystal_hz` a v `rem`
     * zůstane zbytek. Celočíselně, bez plovoucí čárky. Parametry jsou
     * explicitní kvůli testům obou norem v jednom sestavení;
     * iface_audio_block_clock_next() je volá s konstantami platformy.
     *
     * @param c           Stav akumulátoru (nesmí být NULL), `rem < crystal_hz`.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty snímku.
     * @param crystal_hz  Takty za skutečnou sekundu, > 0.
     * @return Počet vzorků bloku: dolní nebo horní celá část `rate * frame_ticks / crystal_hz`.
     * @pre `rem + rate * frame_ticks` se vejde do uint64_t.
     * @post `c->rem < crystal_hz`.
     */
    static inline size_t iface_audio_block_clock_step(st_IFACE_AUDIO_BLOCK_CLOCK *c, uint64_t rate, uint64_t frame_ticks,
                                                      uint64_t crystal_hz)
    {
        const uint64_t acc = c->rem + rate * frame_ticks;
        c->rem = acc % crystal_hz;
        return (size_t)(acc / crystal_hz);
    }

    /**
     * @brief Obecný převod počtu snímků na skutečnou dobu v us.
     *
     * `floor(frames * frame_ticks * 1000000 / crystal_hz)` přes podíl
     * a zbytek `frames` po dělení `crystal_hz` (mezivýsledek nepřeteče,
     * když `(crystal_hz - 1) * frame_ticks * 1000000` se vejde do uint64_t).
     *
     * @param frames      Počet snímků.
     * @param frame_ticks Takty snímku.
     * @param crystal_hz  Takty za skutečnou sekundu, > 0.
     * @return Doba v us (výsledek musí být menší než INT64_MAX).
     * @note Čistá funkce.
     */
    static inline int64_t iface_audio_frames_to_us(uint64_t frames, uint64_t frame_ticks, uint64_t crystal_hz)
    {
        const uint64_t num = frame_ticks * 1000000u;
        const uint64_t q = frames / crystal_hz;
        const uint64_t r = frames % crystal_hz;
        return (int64_t)(q * num + (r * num) / crystal_hz);
    }

    /**
     * @brief Hrana `i` bloku na souvislé časové ose vzorků.
     *
     * Souvislá časová osa: vzorek n (od začátku akumulátoru bloků) pokrývá
     * jmenovité takty `[n * crystal_hz / rate, (n + 1) * crystal_hz / rate)`,
     * kde snímek k začíná v taktu `k * frame_ticks`. V jednotkách
     * `1 / rate` taktu jsou hranice celočíselné: hrana `i` bloku je
     * `i * crystal_hz - lead` od začátku snímku, kde `lead` je zbytek
     * akumulátoru (st_IFACE_AUDIO_BLOCK_CLOCK::rem) PŘED krokem, který dal
     * délku bloku. Hrana 0 (začátek prvního vzorku) je proto v intervalu
     * `(-crystal_hz, 0]`: první vzorek začíná až o necelý vzorek dřív než
     * snímek; poslední hrana bloku (`i` = počet vzorků) je
     * `rate * frame_ticks - rem_po_kroku`, tedy v `(0, rate * frame_ticks]`.
     * Vzorek bloku tak obsahuje právě ty vzorky, jejichž interval končí
     * uvnitř snímku, a hrany sousedních bloků na sebe navazují bez mezery
     * a bez překryvu (stejné dělení jako průběžný index rendereru
     * videorec_audio_render()).
     *
     * @param lead       Zbytek akumulátoru před krokem bloku, `< crystal_hz`.
     * @param i          Index hrany 0 až počet vzorků bloku.
     * @param crystal_hz Takty za skutečnou sekundu (krystal), > 0.
     * @return Hrana v jednotkách `1 / rate` jmenovitého taktu od začátku snímku.
     * @note Čistá funkce; `i * crystal_hz` se pro bloky do tisíců vzorků vejde do int64.
     */
    static inline int64_t iface_audio_block_edge(uint64_t lead, size_t i, uint64_t crystal_hz)
    {
        return (int64_t)((uint64_t)i * crystal_hz) - (int64_t)lead;
    }

    /**
     * @brief Bod vzorku PSG: poslední takt intervalu vzorku ve snímku.
     *
     * SDL cesta od průměru PSG přes interval vzorku (iface_audio_block_tick(),
     * iface_audio_resampler_psg_average()) body nepoužívá; funkce zůstává pro
     * iface_audio_block_timing_for() a testy časové osy.
     *
     * Snímek trvá `span_ticks` taktů zvukového logu (při 100 % takty
     * snímku, při vlastní rychlosti délka synchronizační události), a celý se
     * mapuje na `rate * frame_ticks` jednotek souvislé osy. Vrací
     * `ceil(end_edge * span_ticks / (rate * frame_ticks)) - 1`, tedy takt,
     * ve kterém interval vzorku končí. Leží vždy uvnitř snímku
     * `[0, span_ticks - 1]`, takže bod žádného vzorku nepotřebuje data
     * předchozího logu.
     *
     * Počítá se jako `(x - 1) / (rate * frame_ticks)` pro součin
     * `x = end_edge * span_ticks >= 1` (pro celá čísla je `ceil(x / den) - 1
     * = floor((x - 1) / den)`), tedy bez přičtení `den - 1`: výsledek je
     * správný pro celý rozsah `span_ticks` až IFACE_AUDIO_SPAN_TICKS_MAX
     * (dřívější zápis `(x + den - 1) / den` na samé mezi přetekl a vracel 0).
     *
     * @param end_edge    Koncová hrana vzorku (iface_audio_block_edge() s `i + 1`), v `(0, rate * frame_ticks]`;
     *                    hodnota mimo rozsah se ořízne.
     * @param span_ticks  Délka snímku v taktech logu, >= 1 a nejvýš IFACE_AUDIO_SPAN_TICKS_MAX pro dané `rate * frame_ticks`.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty jmenovitého snímku.
     * @return Takt od začátku snímku, 0 až `span_ticks - 1` (0 pro `span_ticks` 0).
     * @note Čistá funkce, celočíselně.
     */
    static inline uint64_t iface_audio_block_point(int64_t end_edge, uint64_t span_ticks, uint64_t rate, uint64_t frame_ticks)
    {
        const uint64_t den = rate * frame_ticks;
        uint64_t e = (end_edge > 0) ? (uint64_t)end_edge : 1u;
        if (e > den)
            e = den;
        const uint64_t x = e * span_ticks;
        return (x > 0u) ? (x - 1u) / den : 0u;
    }

    /**
     * @brief Buňka mezilehlého proudu CTC0, ve které leží hrana souvislé osy (zaokrouhleno dolů).
     *
     * Snímek má `cells` buněk (IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME) bez ohledu
     * na délku logu; buňka b pokrývá jednotky `[b * rate * frame_ticks / cells,
     * (b + 1) * rate * frame_ticks / cells)`. Záporná hrana (začátek prvního
     * vzorku bloku) dá zápornou buňku: -1 je poslední buňka předchozího snímku.
     * Vzorek s hranami `a`, `b` průměruje buňky od `cell(a)` do `cell(b)`
     * včetně, krajní vážené podílem překryvu (iface_audio_block_cell_pos());
     * poslední buňka bloku je o `cells` větší než první buňka dalšího bloku
     * v jeho souřadnicích.
     *
     * @param edge        Hrana (iface_audio_block_edge()).
     * @param cells       Buňky na snímek, > 0.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty jmenovitého snímku.
     * @return `floor(edge * cells / (rate * frame_ticks))`.
     * @note Čistá funkce; `|edge| * cells` se musí vejít do uint64_t. Záporná
     *       hrana se zaokrouhluje přes `(x - 1) / den + 1` místo
     *       `(x + den - 1) / den`, takže na mezi kontraktu nepřeteče.
     */
    static inline int64_t iface_audio_block_cell(int64_t edge, uint64_t cells, uint64_t rate, uint64_t frame_ticks)
    {
        const uint64_t den = rate * frame_ticks;
        if (edge >= 0)
            return (int64_t)(((uint64_t)edge * cells) / den);
        const uint64_t x = ((uint64_t)(-edge)) * cells;
        return (x > 0u) ? -(int64_t)((x - 1u) / den + 1u) : 0;
    }

    /**
     * @brief Poloha hrany souvislé osy v proudu buněk CTC0 v jednotkách `1 / (rate * frame_ticks)` buňky.
     *
     * `edge * cells`: buňka b pokrývá polohy `[b * d, (b + 1) * d)`, kde
     * `d = rate * frame_ticks`, takže buňka hrany je `floor(poloha / d)`
     * (= iface_audio_block_cell()) a zbytek je poloha hrany uvnitř buňky.
     * Vzorek s polohami `A`, `B` pokrývá buňky `floor(A / d)` až
     * `floor(B / d)`, první jen částí `d - (A mod d)`, poslední částí
     * `B mod d`; délka každého vzorku je `crystal_hz * cells` jednotek
     * (MZ-800 12,567 buňky).
     *
     * @param edge  Hrana (iface_audio_block_edge()).
     * @param cells Buňky na snímek, > 0.
     * @return Poloha `edge * cells` (může být záporná).
     * @note Čistá funkce; `|edge| * cells` se musí vejít do int64_t (bloky do tisíců vzorků ano).
     */
    static inline int64_t iface_audio_block_cell_pos(int64_t edge, uint64_t cells)
    {
        return edge * (int64_t)cells;
    }

    /**
     * @brief Časování vzorků jednoho bloku SDL cesty pro dané hodiny (obecná verze).
     *
     * Pro blok o `samples` vzorcích se zbytkem akumulátoru `lead` (před
     * krokem) spočte body vzorků PSG v taktech logu (`offset` +
     * iface_audio_block_point()) a polohy hranic vzorků v proudu buněk CTC0
     * (iface_audio_block_cell_pos() pro hrany 0 až `samples`). Vzorky leží na
     * souvislé časové ose (iface_audio_block_edge()): čas prvního vzorku
     * se nezakládá znovu na začátek bloku, takže stálý tón nemá postranní
     * složky z periody akumulátoru. Platformní verze je
     * iface_audio_block_timing(); tahle existuje kvůli testům obou norem
     * v jednom sestavení.
     *
     * @param lead        Zbytek akumulátoru před krokem bloku, `< crystal_hz`.
     * @param samples     Počet vzorků bloku (z téhož kroku akumulátoru).
     * @param offset      Začátek snímku v logu [takty od first_timestamp].
     * @param span        Délka snímku v logu [takty], >= 1, nejvýš `UINT64_MAX / (rate * frame_ticks)`.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty jmenovitého snímku.
     * @param crystal_hz  Takty za skutečnou sekundu.
     * @param cells       Buňky proudu CTC0 na snímek.
     * @param points      Výstup: `samples` bodů PSG v taktech od začátku logu (neklesající), nebo NULL.
     * @param edge_pos    Výstup: `samples + 1` poloh hranic v jednotkách `1 / (rate * frame_ticks)`
     *                    buňky (rostoucí po `crystal_hz * cells`; první v `(-crystal_hz * cells, 0]`,
     *                    poslední v `(0, cells * rate * frame_ticks]`), nebo NULL.
     * @note Čistá funkce.
     */
    static inline void iface_audio_block_timing_for(uint64_t lead, size_t samples, uint64_t offset, uint64_t span,
                                                    uint64_t rate, uint64_t frame_ticks, uint64_t crystal_hz,
                                                    uint64_t cells, uint64_t *points, int64_t *edge_pos)
    {
        for (size_t i = 0; i <= samples; i++)
        {
            const int64_t e = iface_audio_block_edge(lead, i, crystal_hz);
            if (edge_pos)
                edge_pos[i] = iface_audio_block_cell_pos(e, cells);
            if (points && i > 0)
                points[i - 1] = offset + iface_audio_block_point(e, span, rate, frame_ticks);
        };
    }

    /**
     * @brief Takt logu snímku, ve kterém leží hrana souvislé osy (zaokrouhleno dolů).
     *
     * Snímek trvá `span_ticks` taktů logu a mapuje se na `rate * frame_ticks`
     * jednotek souvislé osy (jako u iface_audio_block_point()). Vrací
     * `floor(edge * span_ticks / (rate * frame_ticks))` od začátku snímku;
     * záporná hrana (začátek prvního vzorku bloku) dá záporný takt v konci
     * předchozího snímku. Při 100 % je to `floor(edge / rate)`: hranice
     * vzorků leží na celých taktech společné osy všech bloků, stejně jako
     * hranice vzorků rendereru videorec_audio_render() (průměr PSG přes
     * interval vzorku, iface_audio_resampler_psg_average()).
     *
     * @param edge        Hrana (iface_audio_block_edge()), `|edge| <= rate * frame_ticks`.
     * @param span_ticks  Délka snímku v taktech logu, nejvýš `UINT64_MAX / (rate * frame_ticks)`.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty jmenovitého snímku.
     * @return Takt od začátku snímku (může být záporný); 0 pro `span_ticks` 0.
     * @note Čistá funkce, celočíselně, bez přičtení jmenovatele (nepřeteče na mezi).
     */
    static inline int64_t iface_audio_block_tick(int64_t edge, uint64_t span_ticks, uint64_t rate, uint64_t frame_ticks)
    {
        const uint64_t den = rate * frame_ticks;
        if (edge >= 0)
            return (int64_t)(((uint64_t)edge * span_ticks) / den);
        const uint64_t x = ((uint64_t)(-edge)) * span_ticks;
        return (x > 0u) ? -(int64_t)((x - 1u) / den + 1u) : 0;
    }

    /**
     * @brief Hranice vzorků bloku v taktech logu snímku pro dané hodiny (obecná verze).
     *
     * `ticks[i] = iface_audio_block_tick(iface_audio_block_edge(lead, i), ...)`
     * pro i = 0 až `samples`: vzorek i pokrývá takty `[ticks[i], ticks[i + 1])`
     * od začátku snímku. `ticks[0]` je v `(-takty vzorku, 0]`, `ticks[samples]`
     * v `[0, span]`; sousední bloky na sebe při 100 % navazují (konec bloku
     * minus takty snímku = začátek dalšího). Platformní verze je
     * iface_audio_block_ticks().
     *
     * @param lead        Zbytek akumulátoru před krokem bloku, `< crystal_hz`.
     * @param samples     Počet vzorků bloku.
     * @param span        Délka snímku v logu [takty], 1 až `UINT64_MAX / (rate * frame_ticks)`.
     * @param rate        Vzorkovací kmitočet výstupu [Hz].
     * @param frame_ticks Takty jmenovitého snímku.
     * @param crystal_hz  Takty za skutečnou sekundu.
     * @param ticks       Výstup: `samples + 1` neklesajících hranic [takty od začátku snímku].
     * @note Čistá funkce.
     */
    static inline void iface_audio_block_ticks_for(uint64_t lead, size_t samples, uint64_t span, uint64_t rate,
                                                   uint64_t frame_ticks, uint64_t crystal_hz, int64_t *ticks)
    {
        for (size_t i = 0; i <= samples; i++)
            ticks[i] = iface_audio_block_tick(iface_audio_block_edge(lead, i, crystal_hz), span, rate, frame_ticks);
    }

    /**
     * @brief Hranice vzorků bloku v taktech logu s konstantami platformy.
     *
     * iface_audio_block_ticks_for() s IFACE_AUDIO_SAMPLE_RATE,
     * VIDEO_SCREEN_TICKS a GDGCLK_REAL_TICKS_PER_SEC.
     *
     * @param lead    Zbytek akumulátoru před krokem bloku.
     * @param samples Počet vzorků bloku, nejvýš IFACE_AUDIO_BLOCK_SAMPLES_MAX.
     * @param span    Délka snímku v logu [takty], 1 až IFACE_AUDIO_SPAN_TICKS_MAX.
     * @param ticks   Výstup: `samples + 1` hranic [takty od začátku snímku].
     * @note Čistá funkce; libovolné vlákno.
     */
    extern void iface_audio_block_ticks(uint64_t lead, size_t samples, uint64_t span, int64_t *ticks);

    /**
     * @brief Časování vzorků bloku SDL cesty s konstantami platformy.
     *
     * iface_audio_block_timing_for() s IFACE_AUDIO_SAMPLE_RATE,
     * VIDEO_SCREEN_TICKS, GDGCLK_REAL_TICKS_PER_SEC
     * a IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME.
     *
     * @param lead       Zbytek akumulátoru před krokem (st_IFACE_AUDIO_BLOCK_CLOCK::rem před iface_audio_block_clock_next()).
     * @param samples    Počet vzorků bloku, nejvýš IFACE_AUDIO_BLOCK_SAMPLES_MAX.
     * @param offset     Začátek snímku v logu [takty od first_timestamp].
     * @param span       Délka snímku v logu [takty], 1 až IFACE_AUDIO_SPAN_TICKS_MAX.
     * @param points     Výstup: `samples` bodů PSG, nebo NULL.
     * @param edge_pos   Výstup: `samples + 1` poloh hranic v proudu buněk CTC0
     *                   (jednotky `1 / (IFACE_AUDIO_SAMPLE_RATE * VIDEO_SCREEN_TICKS)` buňky), nebo NULL.
     * @note Čistá funkce; libovolné vlákno.
     */
    extern void iface_audio_block_timing(uint64_t lead, size_t samples, uint64_t offset, uint64_t span, uint64_t *points,
                                         int64_t *edge_pos);

    /**
     * @brief Zesílení převzorkování CTC0 SDL cesty pro dané hodiny (obecná verze).
     *
     * Vzorek SDL cesty je vážený součet buněk proudu CTC0 přes přesný
     * interval vzorku (krajní buňky podílem překryvu,
     * iface_audio_block_cell_pos()) dělený délkou intervalu v buňkách plus
     * jedna (dělitel `+ 1` zachovává úroveň dřívějšího převzorkování, které
     * dělilo počtem celých buněk plus jedna). Interval má vždy
     * `r = crystal_hz * cells / (rate * frame_ticks)` buněk (MZ-800 12,567,
     * NTSC 10,146), takže trvalá jednička dá u každého vzorku stejně
     * `r / (r + 1)` (MZ-800 0,926291); zesílení nezávisí na vzorku ani na
     * délce bloku.
     *
     * @return Zesílení 0 až 1.
     * @note Čistá funkce.
     */
    static inline double iface_audio_ctc0_sdl_gain_for(uint64_t rate, uint64_t frame_ticks, uint64_t crystal_hz, uint64_t cells)
    {
        const double len = (double)(crystal_hz * cells);
        return len / (len + (double)(rate * frame_ticks));
    }

    /**
     * @brief Zesílení převzorkování CTC0 SDL cesty s konstantami platformy.
     *
     * iface_audio_ctc0_sdl_gain_for() s IFACE_AUDIO_SAMPLE_RATE,
     * VIDEO_SCREEN_TICKS, GDGCLK_REAL_TICKS_PER_SEC
     * a IFACE_AUDIO_CTC0_SAMPLES_PER_FRAME.
     *
     * @return Zesílení (ze vzorce: MZ-800 a MZ-700 PAL 0,926291, MZ-1500 a MZ-700 NTSC 0,910283).
     * @note Čistá funkce; libovolné vlákno.
     */
    extern float iface_audio_ctc0_sdl_gain(void);

    /**
     * @brief Počet vzorků dalšího bloku SDL cesty (jeden snímek).
     *
     * iface_audio_block_clock_step() s IFACE_AUDIO_SAMPLE_RATE,
     * VIDEO_SCREEN_TICKS a GDGCLK_REAL_TICKS_PER_SEC.
     *
     * @param c Stav akumulátoru (nesmí být NULL).
     * @return Počet vzorků bloku na kanál: PAL 881 nebo 882, NTSC 735 nebo 736;
     *         nejvýš IFACE_AUDIO_BLOCK_SAMPLES_MAX.
     * @post Invariant st_IFACE_AUDIO_BLOCK_CLOCK platí.
     * @note Čistá funkce nad `c`; vlákno, které stav vlastní.
     */
    extern size_t iface_audio_block_clock_next(st_IFACE_AUDIO_BLOCK_CLOCK *c);

    /**
     * @brief Skutečná doba `frames` snímků v mikrosekundách (podle krystalu).
     *
     * `floor(frames * VIDEO_SCREEN_TICKS * 1000000 / GDGCLK_REAL_TICKS_PER_SEC)`
     * (iface_audio_frames_to_us() s konstantami platformy),
     * počítáno přes podíl a zbytek po dělení krystalem, takže mezivýsledek
     * nepřeteče pro žádné `frames` a chyba se nesčítá (termín k-tého snímku
     * je spočtený přímo z k). Snímek PAL trvá 19 985,48 us, NTSC 16 688,15 us.
     *
     * @param frames Počet snímků.
     * @return Doba v us; pro frames > asi 4,6e14 (PAL) výsledek přeteče int64 (prakticky nedosažitelné).
     * @note Čistá funkce; libovolné vlákno.
     */
    extern int64_t iface_audio_frames_to_real_us(uint64_t frames);
    /**
     * @brief Naplní tabulku výchylek kanálu pro hodnoty zvukového logu.
     *
     * Hodnoty 0 až 15: 0 = ticho, 15 = plná výchylka, mezi nimi krok 2 dB
     * (stupeň `v` má útlum `2 * (15 - v)` dB). Hodnoty 16 až 30: výchylka
     * stupně `v - 15` krát AUDIO_LEVEL_HALF_FACTOR (tón PSG s děličem 001h).
     * Hodnota 31 (AUDIO_LEVEL_CTC0_DIV2) má výchylku 0: píše ji jen kanál
     * CTC0, který tuto tabulku nepoužívá.
     *
     * @param id     Index kanálu (0 až AUDIO_SRC_CHANNELS_COUNT - 1).
     * @param volume Hlasitost v procentech (100 = plná výchylka 1,0).
     * @post g_iface_audio.SN76489_volume_value[id] je přepsaná; při
     *       MZ800EMU_CFG_AUDIO_DISABLED se nemění nic.
     */
    extern void iface_audio_set_src_volume(int id, int volume);

    /**
     * @brief Je zapnutá věrná výstupní propust (st_IFACE_AUDIO::output_filter)?
     * @return true = zapnutá; bez AUDIO_HAVE_OUTPUT_FILTER (MZ-700) vždy false.
     * @note Lze volat z libovolného vlákna.
     */
    extern bool iface_audio_output_filter_enabled(void);

    /**
     * @brief Zapne nebo vypne věrnou výstupní propust.
     *
     * Všechny zvukové cesty změnu převezmou u dalšího bloku nebo snímku
     * a přejdou plynule (10 ms). Do INI se zapíše při ukládání konfigurace.
     * Bez AUDIO_HAVE_OUTPUT_FILTER (MZ-700) nedělá nic, propust zůstává vypnutá.
     *
     * @param on Nový stav.
     * @note Lze volat z libovolného vlákna.
     */
    extern void iface_audio_set_output_filter(bool on);

    /**
     * @brief Střední hodnota smíchaného signálu pro střídavou vazbu výstupu.
     *
     * Ze středních hodnot kanálů vedených emulátorem v emulovaném čase
     * (audio_out_ac_means()) spočte střední hodnotu součtu kanálů levé
     * a pravé strany se stejnými vahami jako mix (hlasitosti, poměr CTC0).
     * Renderer nahrávky ji dostane na začátku
     * (videorec_audio_set_output_mean()), aby začal bez doznívajícího
     * stejnosměrného posunu. Bez AUDIO_HAVE_OUTPUT_COUPLING vrací nuly.
     *
     * @param now_ticks Čas (osa gdg_get_total_ticks()).
     * @param stereo    Stereo rozložení (L = CTC0 + PSG0, R = CTC0 + PSG1); mono dává obě strany stejné.
     * @param out       Výstup: střední hodnota levé a pravé strany.
     * @pre Vlákno emulátoru, po iface_audio_init() a audio_init().
     */
    extern void iface_audio_output_ac_means(uint64_t now_ticks, bool stereo, double out[2]);

    /**
     * @brief Násobitel kanálu CTC0 v mixu (mimo uživatelskou hlasitost).
     *
     * Takový, aby výsledná výchylka CTC0 byla
     * AUDIO_CTC0_TO_PSG_CHANNEL_RATIO násobek výchylky jednoho kanálu PSG
     * s útlumem 0. Kanál PSG se v mixu dělí PSG_CHANNELS_COUNT a převzorkování
     * CTC0 v SDL cestě má zesílení iface_audio_ctc0_sdl_gain()
     * (asi 0,926 na PAL, 0,910 na NTSC), proto
     * `AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / PSG_CHANNELS_COUNT / zesílení`.
     * Zesílení je u každého vzorku stejné (interval vzorku má vždy stejnou
     * délku v buňkách), takže násobitel nezávisí na délce bloku (881 nebo
     * 882, NTSC 735 nebo 736 vzorků) a mix nemoduluje úroveň CTC0.
     * Emulátory bez PSG (MZ-700) nemají CTC0 s čím poměřovat; používají
     * stejnou absolutní úroveň jen kvůli pohodlí, aby MZ-800 v režimu MZ-700
     * a samostatný MZ-700 hrály stejně hlasitě (věrnosti se to netýká, na
     * skutečném MZ-700 úroveň měřena není [neověřeno]).
     *
     * @return Násobitel vzorku CTC0 před sečtením s PSG.
     * @note Čistá funkce konstant překladu; lze volat z libovolného vlákna.
     */
    extern float iface_audio_ctc0_mix_level(void);
    extern void iface_audio_set_master_volume(int volume);

    /**
     * @brief Nastaví hlavní hlasitost poslechu (st_IFACE_AUDIO::master_gain).
     *
     * Hlasitosti kanálů (gain[], okno Audio Mixer) se nemění; hlavní
     * hlasitost je další násobitel hotového mixu. Projeví se od dalšího
     * zvukového bloku plynulým přechodem přes jeden blok. Do INI se zapíše
     * při ukládání konfigurace. Video záznam ani zachycení zvuku neovlivní.
     *
     * @param gain Hlasitost 0 až AUDIO_MASTER_GAIN_MAX (1 = 100 %, výchozí; 2 = 200 %);
     *             hodnota mimo rozsah se ořízne, NaN se bere jako 1.
     * @note Lze volat z libovolného vlákna.
     */
    extern void iface_audio_set_master_gain(float gain);

    /**
     * @brief Hlavní hlasitost poslechu.
     * @return Hodnota 0 až AUDIO_MASTER_GAIN_MAX (výchozí 1); ztlumení ji nemění.
     * @note Lze volat z libovolného vlákna.
     */
    extern float iface_audio_get_master_gain(void);

    /**
     * @brief Ztlumí nebo obnoví poslech (st_IFACE_AUDIO::mute).
     *
     * Hlavní hlasitost ani hlasitosti kanálů se nemění. Projeví se od
     * dalšího zvukového bloku plynulým přechodem přes jeden blok. Do INI se
     * neukládá: platí jen do konce běhu emulátoru, který startuje vždy
     * neztlumený. Video záznam ani zachycení zvuku neovlivní.
     *
     * @param mute true = ticho.
     * @note Lze volat z libovolného vlákna.
     */
    extern void iface_audio_set_mute(bool mute);

    /**
     * @brief Je poslech ztlumený?
     * @return true = ztlumený (výchozí false).
     * @note Lze volat z libovolného vlákna.
     */
    extern bool iface_audio_get_mute(void);

    /**
     * @brief Uplatní hlavní hlasitost a ztlumení na hotový smíchaný blok.
     *
     * Cílový násobitel je 0 při ztlumení, jinak master_gain. Je-li cílový
     * i naposledy uplatněný násobitel přesně 1, blok se nemění ani nečte
     * (výchozí stav: výstup je bitově shodný s mixem). Jinak se vzorky
     * násobí; liší-li se cílový násobitel od naposledy uplatněného, přejde
     * se k němu lineárně přes délku bloku (bez lupnutí), další blok už má
     * násobitel stálý. První blok po startu (a po
     * iface_audio_master_restart()) začíná rovnou na cílovém násobiteli:
     * s nižší hlasitostí z INI se nepřechází z plné hlasitosti.
     *
     * Je-li cílový nebo naposledy uplatněný násobitel větší než 1 (hlavní
     * hlasitost nad 100 %), projde vynásobený blok měkkým omezovačem
     * videorec_audio_soft_limit(): vzorky do kolena AUDIO_OUT_LIMIT_KNEE
     * zůstanou jen vynásobené, větší se stlačí k 1,0 a tvrdý ořez
     * nenastane. Do 100 % včetně se omezovač nevolá a chování je stejné
     * jako před rozšířením rozsahu.
     *
     * Volá iface_audio_wait_for_data() na bloku pro zvukové zařízení až po
     * předání téhož bloku video záznamu (videorec_rt_audio_output()) a
     * zachycení zvuku (audio_capture_sdl_output()): záznam je na poslechu
     * nezávislý.
     *
     * @param samples  Prokládané vzorky (L, R); NULL se ignoruje.
     * @param frames   Počet snímků (vzorků na kanál).
     * @param channels Počet kanálů v prokládání (1 nebo 2); 0 se ignoruje.
     * @pre Jen vlákno, které volá iface_audio_wait_for_data() (drží stav
     *      naposledy uplatněného násobitele): vlákno zvukového výstupu, v
     *      režimu bez zvukového zařízení vlákno emulátoru; testy z jednoho vlákna.
     * @post Naposledy uplatněný násobitel je cílový násobitel; všechny
     *       vzorky bloku, který byl na vstupu v [-1, 1], jsou v [-1, 1].
     */
    extern void iface_audio_apply_master(float *samples, size_t frames, unsigned channels);

    /**
     * @brief Příští blok iface_audio_apply_master() začne rovnou na cílovém násobiteli (bez přechodu).
     *
     * Výchozí stav po startu procesu; volání je potřeba jen tam, kde se má
     * stav "první blok" obnovit (testy).
     *
     * @pre Vlákno, které volá iface_audio_wait_for_data(), právě bloky nezpracovává
     *      (příznak se nechrání zámkem).
     */
    extern void iface_audio_master_restart(void);

    /**
     * @brief Sestaví tabulku úrovní zvukových kanálů pro renderer video záznamu.
     *
     * Odpovídá mixu SDL cesty (iface_audio_mix_channels_with_gain()):
     * - kanál 0 (CTC0): SDL cesta počítá vážený součet nenulových buněk přes
     *   interval vzorku dělený `(délka intervalu v buňkách + 1)`, úroveň je tedy
     *   0 pro hodnotu 0 a pro nenulovou hodnotu zesílení iface_audio_ctc0_sdl_gain()
     *   (cca 0,926 u MZ-800), násobené `gain[0]` a iface_audio_ctc0_mix_level()
     *   (vychází `gain[0] * AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / PSG_CHANNELS_COUNT`,
     *   stejně i bez PSG na MZ-700);
     *   hodnota 31 (AUDIO_LEVEL_CTC0_DIV2, režim 3 s děličem 2) má úroveň
     *   AUDIO_CTC0_DIV2_FACTOR krát úroveň ostatních nenulových hodnot;
     * - kanály PSG: hlasitostní tabulka `SN76489_volume_value` (AUDIO_OUTPUT_t
     *   je při AUDIO_FORMAT_F32 `float` v rozsahu 0..1) krát `gain[ch]`, děleno
     *   PSG_CHANNELS_COUNT (průměr přes 4 kanály jako v SDL mixu); hodnoty
     *   16 až 30 (poloviční výchylka, tón s děličem 001h) jsou v tabulce také,
     *   hodnotu 31 kanály PSG nepíšou a má u nich úroveň 0.
     * Filtry SDL cesty (střídavá vazba CTC0, parkování, low-pass, IIR, výstupní
     * propust) aplikuje renderer sám (videorec_audio.h), tabulka obsahuje jen úrovně.
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

/** @brief Časová konstanta doběhu indikace úrovní [ms] (iface_audio_level_follow()). */
#define IFACE_AUDIO_LEVEL_RELEASE_MS 300

/** @brief Úroveň indikace, pod kterou doběh skončí nulou. */
#define IFACE_AUDIO_LEVEL_FLOOR 0.001f

    /**
     * @brief Jeden krok indikace úrovně: okamžitý náběh, exponenciální doběh.
     *
     * Vrací větší z hodnot `peak` a `prev * exp(-t / IFACE_AUDIO_LEVEL_RELEASE_MS)`,
     * kde `t` je délka bloku (`frames` vzorků při IFACE_AUDIO_SAMPLE_RATE).
     * Indikátor tak neproblikává mezi bloky a po utichnutí dojde do půl
     * sekundy pod desetinu; držená hodnota pod IFACE_AUDIO_LEVEL_FLOOR se
     * bere jako 0.
     *
     * @param prev   Dosavadní hodnota indikace (0 až 1).
     * @param peak   Špička nového bloku; ořízne se na 0 až 1, NaN se bere jako 0.
     * @param frames Počet vzorků bloku na kanál.
     * @return Nová hodnota indikace 0 až 1.
     * @note Čistá funkce; lze volat z libovolného vlákna.
     */
    extern float iface_audio_level_follow(float prev, float peak, size_t frames);

    /**
     * @brief Smíchá blok kanálů do prokládaného stereo výstupu a aktualizuje indikaci úrovní.
     *
     * Společný mix SDL cesty: součet CTC0 a kanálů PSG s hlasitostmi gain[]
     * a poměrem iface_audio_ctc0_mix_level(), výstupní stupeň
     * videorec_audio_output_stage() (střídavá vazba, výstupní propust podle
     * volby, dorovnání hlasitosti, měkký omezovač). Mono rozložení
     * (`stereo == false`): CTC0 + PSG0 do obou stran, kanály PSG1 se
     * nemixují. Stereo: L = CTC0 + PSG0, R = CTC0 + PSG1. Hlavní hlasitost
     * a ztlumení se zde neuplatňují (iface_audio_apply_master()).
     *
     * @param channels Kanály po jejich filtrech: AUDIO_SRC_CHANNELS_COUNT polí po `frames` vzorcích.
     * @param raw      Kanály před jejich filtry (stejný tvar).
     * @param output   Výstup, 2 * `frames` vzorků v rozsahu [-1, 1] (L, R).
     * @param frames   Počet vzorků na kanál.
     * @param stereo   Stereo rozložení (dva čipy PSG).
     * @pre Jen vlákno mixu (volá iface_audio_wait_for_data(); testy z jednoho vlákna);
     *      ukazatele nejsou NULL.
     * @post g_iface_audio.level[], total_level a output_channels jsou aktualizované;
     *       stav výstupního stupně je posunutý o `frames` vzorků.
     */
    extern void iface_audio_mix_block(float **channels, float **raw, float *output, size_t frames, bool stereo);

    /**
     * @brief Vrátí výstupní stupeň mixu a indikaci úrovní do stavu po startu.
     *
     * Vynuluje stav výstupní propusti a střídavé vazby výstupu, váhu
     * propusti nechá při dalším bloku nastavit podle volby bez přechodu
     * a vynuluje level[] a total_level. Určeno pro testy.
     *
     * @pre Vlákno mixu právě bloky nezpracovává (stav se nechrání zámkem).
     */
    extern void iface_audio_mix_restart(void);

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
