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
     * CTC0 v SDL cestě má střední zesílení videorec_audio_sdl_ctc0_gain()
     * (asi 0,926), proto
     * `AUDIO_CTC0_TO_PSG_CHANNEL_RATIO / PSG_CHANNELS_COUNT / zesílení`.
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
     * - kanál 0 (CTC0): SDL cesta počítá podíl nenulových vzorků dělený
     *   `(délka bloku + 1)`, úroveň je tedy 0 pro hodnotu 0 a pro nenulovou
     *   hodnotu střední zesílení bloku videorec_audio_sdl_ctc0_gain()
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
