/**
 * @file   videorec_rt.h
 * @brief  Čisté části režimu "podle reality": hodiny vzorkovače a převzorkování zvuku SDL výstupu.
 *
 * V režimu podle reality (časová základna `realtime`, viz videorec.h) se video
 * snímek nezapisuje za každý emulovaný snímek, ale fps-krát za sekundu reálného
 * času (50 nebo 60 podle platformy, monotónní hodiny), a zvuk se bere z výstupu SDL cesty (to, co šlo do
 * reproduktoru), ne z emulačního rendereru. Tento modul obsahuje části bez
 * závislosti na emulátoru, aby šly testovat samostatně:
 *
 * - **st_VIDEOREC_RT_CLOCK** - rozvrh ticků vzorkovače s absolutními termíny
 *   `base + k * period` (bez driftu), dohánění zpoždění a nové založení řady
 *   při velkém zpoždění;
 * - **st_VIDEOREC_RT_AUDIO** - kruhový buffer F32 stereo vzorků SDL výstupu
 *   (producent = SDL audio callback, v headless synchronizace emu vlákna
 *   jednou za snímek), převzorkování na `audio_rate` lineární interpolací a korekce
 *   driftu hodin zvukového zařízení proti monotónním hodinám (P regulátor
 *   naplnění bufferu, který mírně mění poměr převzorkování).
 *
 * @par Zarovnání zvuku k obrazu
 * Buffer funguje jako jitter buffer: spotřebitel začne číst, až je naplnění
 * aspoň `target` vstupních vzorků, a regulátor pak drží průměrné naplnění na
 * `target`. Zvuk vydaný v ticku je tedy v průměru o `target / in_rate`
 * sekund starší než okamžik ticku; obraz se proto zpožďuje o odpovídající
 * počet ticků (viz VIDEOREC_RT_VIDEO_DELAY_MS, videorec_rt_video_delay_ticks()
 * a lepidlo videorec.c).
 * Při vyčerpání bufferu (pauza zařízení, výpadek) se vydává ticho a buffer se
 * plní znovu do `target` (re-prime), takže zpoždění zvuku proti obrazu se
 * po výpadku obnoví na stejnou hodnotu - nedriftuje.
 *
 * @par Vlákna
 * st_VIDEOREC_RT_CLOCK není thread-safe (používá ho jen vlákno vzorkovače).
 * st_VIDEOREC_RT_AUDIO je thread-safe: má vlastní krátký GMutex, pod kterým
 * se jen kopírují vzorky (push) nebo vyrobí jeden tick výstupu (pull);
 * žádný jiný zámek se pod ním nebere.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_RT_H
#define VIDEOREC_RT_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vzorkovací frekvence výstupu SDL cesty [Hz] (= IFACE_AUDIO_SAMPLE_RATE, kontrola v iface_audio.c).
 */
#define VIDEOREC_RT_SDL_RATE 44100u

/**
 * @brief Největší blok SDL cesty [stereo vzorky] - pro dimenzování bufferů.
 *
 * Blok SDL cesty je výstup jedné synchronizační události emulace (jeden
 * snímek): videorec_rt_sdl_chunk() = `VIDEOREC_RT_SDL_RATE / fps`, tj. 882
 * při 50 snímcích/s (MZ-800, MZ-700 PAL) a 735 při 60 snímcích/s (MZ-1500,
 * MZ-700 NTSC). Největší je při nejnižší podporované frekvenci 50 snímků/s.
 * Jitter buffer sám na velikosti bloku nezávisí.
 */
#define VIDEOREC_RT_SDL_CHUNK_MAX (VIDEOREC_RT_SDL_RATE / 50u)

/**
 * @brief Zpoždění obrazu v režimu podle reality [ms]; na ticky převádí videorec_rt_video_delay_ticks().
 *
 * Obraz zapsaný v ticku `k` je snímek navzorkovaný o videorec_rt_video_delay_ticks()
 * ticků dřív, protože zvuk prochází jitter bufferem s cílovým naplněním
 * VIDEOREC_RT_AUDIO_TARGET_MS (naplnění se měří včetně bloku přidaného
 * v aktuální periodě). Zpoždění je dané v milisekundách, aby platilo pro
 * 50 i 60 snímků/s; obě dávají po zaokrouhlení 2 ticky (40 ms, resp. 33,3 ms).
 *
 * Měření (report Task 18 a Task 26, e2e/realtime_av_sync*.sh a testovací
 * program s nástupem tónu CTC0 současně s vyplněním VRAM; chyba = posun
 * zvuku proti obrazu v realtime nahrávce minus týž posun v nahrávce
 * v emulačním čase, kladná = zvuk pozdě; průměr přes 15-36 událostí na běh):
 * - 50 snímků/s, 2 ticky: Bloxorz (Task 18) GUI 0 ms a -20..0 ms,
 *   headless -0,2 ms a -20..0 ms; MZ-700 PAL (Task 26) headless -7,7
 *   a -14,4 ms, GUI -0,2 ms;
 * - 60 snímků/s, 2 ticky (Task 26): MZ-700 NTSC headless -3,6 a +1,1 ms,
 *   MZ-1500 headless +0,6 a +1,4 ms, GUI +11,8 a +11,3 ms;
 * - 60 snímků/s, 3 ticky by posunuly chybu o -16,7 ms (headless cca -16,
 *   GUI cca -5); zkouška s cílem jitter bufferu 3 bloky (50 ms při 60 fps)
 *   dala headless -15,8 a -12,9 ms, GUI -1,7 a -11,5 ms - nic lepšího,
 *   proto zůstává 2 ticky a cíl 60 ms.
 * Chyba jednotlivých událostí kolísá o jeden snímek (fáze ticku proti
 * zobrazení snímku). Rozdíl GUI proti headless je fáze, ve které producent
 * blok dodá (hypotéza: synchronizační událost emulace leží uvnitř snímku,
 * headless producent blok vyrobí hned po ní) [neověřeno].
 * Měřicí skripty (generátor testovacího MZF, běh a analýza A/V): repozitář
 * emu-experiments, video-capture/e2e/ (mk_av_mzf.py, av_run.sh, av_ana.py).
 */
#define VIDEOREC_RT_VIDEO_DELAY_MS 40u

/**
 * @brief Cílové naplnění zvukového jitter bufferu [ms] (3 bloky SDL při 50, 3,6 bloku při 60 snímcích/s).
 *
 * Producent dodává bloky po jednom snímku (20 ms při 50, 16,7 ms při 60
 * snímcích/s), zařízení si je může brát i po dvou najednou; 60 ms dává
 * rezervu proti podtečení i při takových dávkách (viz
 * test_audio_bursty_producer). Cíl je v ms, ne v blocích - viz měření
 * u VIDEOREC_RT_VIDEO_DELAY_MS.
 */
#define VIDEOREC_RT_AUDIO_TARGET_MS 60u

/** @brief Kapacita zvukového bufferu [ms]; přebytek nad ní se zahazuje od nejstarších vzorků. */
#define VIDEOREC_RT_AUDIO_CAPACITY_MS 1000u

/**
 * @brief Časová konstanta regulátoru driftu [s].
 *
 * Odchylka naplnění od cíle se vyrovná zhruba za tuto dobu (korekce poměru =
 * odchylka / (in_rate * konstanta)). Naplnění skáče po celých blocích
 * producenta (882 vzorků při 50, 735 při 60 snímcích/s); při konstantě 10 s
 * to dělá kolísání korekce nejvýš 882 / 441000 = 0,2 % (před vyhlazením
 * průměrem). Trvalá odchylka
 * naplnění P regulátoru je `drift * in_rate * konstanta` (při driftu
 * 100 ppm 44 vzorků = 1 ms).
 */
#define VIDEOREC_RT_AUDIO_DRIFT_TC_S 10.0

/**
 * @brief Maximální relativní korekce poměru převzorkování (0,5 % = cca 8,6 centu výšky tónu).
 *
 * Hodiny zvukových zařízení se proti systémovým hodinám liší typicky o desítky
 * ppm; 0,5 % dává velkou rezervu a změna výšky je pod prahem slyšitelnosti
 * při plynulé regulaci [neověřeno poslechem].
 */
#define VIDEOREC_RT_AUDIO_MAX_CORR 0.005

/**
 * @brief Váha exponenciálního průměru naplnění (na jeden pull).
 *
 * Okamžité naplnění kolísá o celý blok producenta (882 / 735 vzorků), regulátor
 * proto pracuje s průměrem (časová konstanta cca 100 ticků = 2 s při 50, 1,7 s při 60
 * snímcích/s, nejméně pětkrát
 * kratší než VIDEOREC_RT_AUDIO_DRIFT_TC_S - smyčka zůstává stabilní).
 */
#define VIDEOREC_RT_AUDIO_EMA_ALPHA 0.01

/**
 * @brief Rozvrh ticků vzorkovače podle monotónních hodin.
 *
 * Perioda je zlomek `period_num / period_den` us (u 60 Hz 1000000 / 60,
 * tj. 16 666,67 us - celočíselná perioda by se proti reálnému času
 * rozcházela o 40 ppm). Termín k-tého ticku řady je
 * `base_us + k * period_num / period_den` (absolutní - hrubá granularita
 * uspání ani zaokrouhlení periody nezpůsobí drift); tick je splatný, jakmile
 * aktuální čas termín dosáhne. Zpoždění do `max_catchup` ticků se
 * dohání (videorec_rt_clock_due() vrátí víc ticků najednou), větší zpoždění
 * (zaseknutí hostitele) řadu založí znovu od aktuálního času - zameškaný čas
 * se nedohání a počítá se do `rebased`.
 *
 * Invarianty: `period_num > 0`, `period_den > 0`; `started` => `done >= 1`
 * (tick v `base_us` je vždy první vydaný tick řady).
 */
typedef struct st_VIDEOREC_RT_CLOCK {
    int64_t period_num;   /**< Čitatel periody ticku [us] (20000 / 1 pro 50 Hz, 1000000 / 60 pro 60 Hz). */
    int64_t period_den;   /**< Jmenovatel periody ticku. */
    unsigned max_catchup; /**< Max. počet ticků vydaných najednou (dohánění); větší zpoždění = nová řada. */
    bool started;         /**< Řada založená (první volání videorec_rt_clock_due() proběhlo). */
    int64_t base_us;      /**< Čas ticku 0 aktuální řady [us]. */
    uint64_t done;        /**< Počet vydaných ticků aktuální řady. */
    uint64_t rebased;     /**< Kolikrát se řada založila znovu kvůli velkému zpoždění (statistika). */
    uint64_t lost_ticks;  /**< Odhad ticků zahozených při novém založení řady (statistika). */
} st_VIDEOREC_RT_CLOCK;

/**
 * @brief Inicializuje rozvrh (řada se založí při prvním videorec_rt_clock_due()).
 * @param c           Instance (nesmí být NULL).
 * @param period_us   Perioda [us], > 0.
 * @param max_catchup Max. počet ticků najednou, >= 1.
 * @post `started == false`, statistiky nulové.
 */
void videorec_rt_clock_init(st_VIDEOREC_RT_CLOCK *c, int64_t period_us, unsigned max_catchup);

/**
 * @brief Inicializuje rozvrh pro snímkovou frekvenci `fps_num / fps_den` (perioda bez zaokrouhlení).
 *
 * Perioda je `1000000 * fps_den / fps_num` us jako zlomek; např. 60 snímků/s
 * dá přesně 60 ticků za každou sekundu monotónních hodin.
 *
 * @param c           Instance (nesmí být NULL).
 * @param fps_num     Čitatel snímkové frekvence, > 0 (0 se nahradí 1).
 * @param fps_den     Jmenovatel snímkové frekvence, > 0 (0 se nahradí 1).
 * @param max_catchup Max. počet ticků najednou, >= 1.
 * @post `started == false`, statistiky nulové.
 */
void videorec_rt_clock_init_fps(st_VIDEOREC_RT_CLOCK *c, unsigned fps_num, unsigned fps_den, unsigned max_catchup);

/**
 * @brief Zpoždění obrazu v režimu podle reality v ticích vzorkovače pro danou snímkovou frekvenci.
 *
 * `round(VIDEOREC_RT_VIDEO_DELAY_MS * fps_num / (1000 * fps_den))`, nejméně 1:
 * 50 i 60 snímků/s = 2 ticky.
 *
 * @param fps_num Čitatel snímkové frekvence (0 se nahradí 1, jako u videorec_rt_clock_init_fps()).
 * @param fps_den Jmenovatel snímkové frekvence (0 se nahradí 1).
 * @return Počet ticků zpoždění, >= 1.
 * @note Čistá funkce bez vedlejších efektů, libovolné vlákno.
 */
unsigned videorec_rt_video_delay_ticks(unsigned fps_num, unsigned fps_den);

/**
 * @brief Velikost bloku SDL cesty [stereo vzorky] pro danou snímkovou frekvenci.
 *
 * `VIDEOREC_RT_SDL_RATE * fps_den / fps_num` (celočíselné dělení): 882 při 50,
 * 735 při 60 snímcích/s. Celé je jen u frekvencí, které přijme
 * videorec_platform_check() se vzorkovací frekvencí VIDEOREC_RT_SDL_RATE.
 *
 * @param fps_num Čitatel snímkové frekvence (0 se nahradí 1).
 * @param fps_den Jmenovatel snímkové frekvence (0 se nahradí 1).
 * @return Vzorků na blok; pro fps >= 50 nejvýš VIDEOREC_RT_SDL_CHUNK_MAX.
 * @note Čistá funkce bez vedlejších efektů, libovolné vlákno.
 */
unsigned videorec_rt_sdl_chunk(unsigned fps_num, unsigned fps_den);

/**
 * @brief Vrátí počet ticků, jejichž termín už nastal, a označí je za vydané.
 *
 * První volání založí řadu (`base_us = now_us`) a vrátí 1 (tick 0). Další
 * volání vrátí počet termínů `base + k * period_num / period_den <= now_us`
 * pro k od `done`.
 * Je-li jich víc než `max_catchup`, řada se založí znovu od `now_us` a vrátí
 * se 1 (statistiky `rebased`, `lost_ticks`).
 *
 * @param c      Instance.
 * @param now_us Aktuální čas monotónních hodin [us] (neklesající mezi voláními).
 * @return Počet ticků k vydání (0 .. max_catchup).
 */
unsigned videorec_rt_clock_due(st_VIDEOREC_RT_CLOCK *c, int64_t now_us);

/**
 * @brief Termín dalšího (ještě nevydaného) ticku.
 * @param c Instance.
 * @return Čas [us], zaokrouhlený nahoru na celé us (v tento čas je tick už
 *         splatný); před založením řady 0 (= "hned").
 */
int64_t videorec_rt_clock_next_deadline(const st_VIDEOREC_RT_CLOCK *c);

/** @brief Statistiky zvukového bufferu (pro testy a report; čítače od posledního resetu). */
typedef struct st_VIDEOREC_RT_AUDIO_STATS {
    uint64_t pushed;         /**< Vstupních vzorků (stereo framů) přijatých od producenta. */
    uint64_t consumed;       /**< Vstupních vzorků spotřebovaných převzorkováním nebo zahozením. */
    uint64_t produced;       /**< Výstupních vzorků vydaných z dat (bez ticha). */
    uint64_t silence;        /**< Výstupních vzorků ticha (čekání na naplnění, podtečení). */
    uint64_t underruns;      /**< Počet podtečení (data došla uprostřed ticku). */
    uint64_t overruns;       /**< Počet zahození přebytku (naplnění nad cíl + rezervu). */
    uint64_t overflow_drops; /**< Vstupních vzorků zahozených při plné kapacitě bufferu. */
    double corr;             /**< Aktuální relativní korekce poměru převzorkování. */
    double max_abs_corr;     /**< Největší |korekce| od resetu (po prvním naplnění). */
    double fill_ema;         /**< Průměrné naplnění [vstupní vzorky]. */
} st_VIDEOREC_RT_AUDIO_STATS;

/**
 * @brief Kruhový buffer SDL výstupu s převzorkováním a korekcí driftu.
 *
 * Pozice `wr` a `rd` jsou absolutní čítače vstupních stereo vzorků (framů);
 * vzorek s pozicí `n` leží v `ring[(n % cap) * 2 + {0,1}]`.
 *
 * Invarianty (pod `mutex`): `rd <= wr`, `wr - rd <= cap`; `0 <= frac < 1`;
 * `starved` => z bufferu se nečte, dokud naplnění nedosáhne `target`;
 * `!enabled` => push zahazuje data. `enabled` je atomické (zapisuje se pod
 * `mutex` přes g_atomic_int_set, bez zámku se smí jen atomicky číst -
 * videorec_rt_audio_is_enabled()).
 *
 * Ownership: `ring` vlastní instance (videorec_rt_audio_free()).
 */
typedef struct st_VIDEOREC_RT_AUDIO {
    GMutex mutex;      /**< Zámek celé struktury (inicializuje videorec_rt_audio_init()). */
    float *ring;       /**< Prokládané L,R vzorky, `cap * 2` floatů. */
    uint32_t cap;      /**< Kapacita [stereo vzorky]. */
    uint64_t wr;       /**< Absolutní pozice zápisu. */
    uint64_t rd;       /**< Absolutní pozice čtení (celá část). */
    double frac;       /**< Zlomková část pozice čtení. */
    unsigned in_rate;  /**< Vzorkovací frekvence vstupu [Hz] (SDL cesta: 44100). */
    unsigned out_rate; /**< Vzorkovací frekvence výstupu [Hz] (`audio_rate` nahrávky). */
    uint32_t target;   /**< Cílové naplnění [vstupní vzorky]. */
    volatile gint enabled; /**< Producent smí zapisovat (režim podle reality aktivní); 0/1, atomicky. */
    bool starved;      /**< Čeká se na naplnění do `target` (start, po podtečení). */
    st_VIDEOREC_RT_AUDIO_STATS stats; /**< Statistiky (corr a fill_ema jsou zároveň stav regulátoru). */
} st_VIDEOREC_RT_AUDIO;

/**
 * @brief Inicializuje buffer.
 * @param ra         Instance (nesmí být NULL; obsah se přepíše).
 * @param in_rate    Vstupní frekvence [Hz] (> 0).
 * @param out_rate   Výstupní frekvence [Hz] (> 0).
 * @param target_ms  Cílové naplnění [ms] (VIDEOREC_RT_AUDIO_TARGET_MS).
 * @param capacity_ms Kapacita [ms] (> target_ms; VIDEOREC_RT_AUDIO_CAPACITY_MS).
 * @post Prázdný, `enabled == false`, `starved == true`, statistiky nulové.
 * @note Selhat nemůže (g_malloc při nedostatku paměti ukončí proces).
 */
void videorec_rt_audio_init(st_VIDEOREC_RT_AUDIO *ra, unsigned in_rate, unsigned out_rate, unsigned target_ms,
                            unsigned capacity_ms);

/**
 * @brief Uvolní buffer a zámek.
 * @param ra Instance po videorec_rt_audio_init(); opakované volání je bezpečné jen
 *           po nové inicializaci.
 * @pre Žádné jiné vlákno už instanci nepoužívá.
 */
void videorec_rt_audio_free(st_VIDEOREC_RT_AUDIO *ra);

/**
 * @brief Vyprázdní buffer, vynuluje regulátor a statistiky a nastaví povolení zápisu.
 * @param ra       Instance.
 * @param enabled  true = producent smí zapisovat.
 * @param out_rate Nová výstupní frekvence [Hz] (0 = beze změny).
 * @post Prázdný, `starved == true`.
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
void videorec_rt_audio_reset(st_VIDEOREC_RT_AUDIO *ra, bool enabled, unsigned out_rate);

/**
 * @brief Producent: připojí stereo vzorky (F32, prokládané L,R).
 *
 * Při `enabled == false` se data zahodí. Přesáhne-li naplnění kapacitu,
 * zahodí se nejstarší vzorky (`overflow_drops`).
 *
 * @param ra     Instance.
 * @param stereo `frames * 2` floatů (nesmí být NULL při `frames > 0`).
 * @param frames Počet stereo vzorků.
 * @par Vlákna Libovolné vlákno (SDL audio callback, emu vlákno v headless);
 *      bere jen zámek instance (krátce, kopie dat).
 */
void videorec_rt_audio_push(st_VIDEOREC_RT_AUDIO *ra, const float *stereo, size_t frames);

/**
 * @brief Zda producent má dodat další data (emulace zvukového zařízení v headless).
 *
 * Skutečné zařízení si data žádá, když mu dochází; headless producent (synchronizace
 * jednou za snímek, při MAX SPEED tisíckrát za sekundu) se podle této funkce
 * ptá, zda buffer potřebuje další blok.
 *
 * @param ra    Instance.
 * @param chunk Velikost bloku producenta [vstupní vzorky].
 * @return true pokud `enabled` a naplnění < `target + chunk`.
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
bool videorec_rt_audio_wants_data(st_VIDEOREC_RT_AUDIO *ra, size_t chunk);

/**
 * @brief Je zápis povolený? (rychlý test bez zámku, jedno atomické čtení; výsledek může být hned zastaralý).
 * @param ra Instance.
 * @return Hodnota `enabled`.
 */
bool videorec_rt_audio_is_enabled(st_VIDEOREC_RT_AUDIO *ra);

/**
 * @brief Spotřebitel: vyrobí `out_frames` výstupních stereo vzorků (int16) jednoho ticku.
 *
 * - Ve stavu `starved` vydá ticho a nic nespotřebuje, dokud naplnění
 *   nedosáhne `target`; pak začne číst.
 * - Naplnění nad `target * 4` (producent přidal víc, než se stihlo
 *   spotřebovat - např. dlouho nevolaný pull) se zahodí na `target`
 *   (`overruns`).
 * - Poměr převzorkování `in_rate / out_rate * (1 + corr)`, kde `corr` je
 *   P regulace průměrného naplnění k `target` (VIDEOREC_RT_AUDIO_DRIFT_TC_S,
 *   omezeno na +-VIDEOREC_RT_AUDIO_MAX_CORR). Interpolace lineární.
 * - Dojdou-li data uprostřed ticku, zbytek je ticho, buffer se vyprázdní
 *   a přejde do `starved` (`underruns`).
 *
 * Výstup = `gain * vzorek`, oříznutý na [-1, 1] a převedený na int16
 * (`* 32767`, zaokrouhleno).
 *
 * @param ra         Instance.
 * @param out_or_null Výstup `out_frames * 2` int16 (L,R), nebo NULL = data se jen
 *                   spotřebují stejně jako při výrobě (zahození, např. record-pause).
 * @param out_frames Počet výstupních stereo vzorků.
 * @param gain       Zesílení (1 = beze změny, 0 = ticho při zachované spotřebě).
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
void videorec_rt_audio_pull(st_VIDEOREC_RT_AUDIO *ra, int16_t *out_or_null, size_t out_frames, float gain);

/**
 * @brief Aktuální naplnění bufferu [vstupní stereo vzorky].
 * @param ra Instance.
 * @return `wr - rd`.
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
size_t videorec_rt_audio_fill(st_VIDEOREC_RT_AUDIO *ra);

/**
 * @brief Zkopíruje statistiky.
 * @param ra  Instance.
 * @param out Výstup (nesmí být NULL).
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
void videorec_rt_audio_get_stats(st_VIDEOREC_RT_AUDIO *ra, st_VIDEOREC_RT_AUDIO_STATS *out);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_RT_H */
