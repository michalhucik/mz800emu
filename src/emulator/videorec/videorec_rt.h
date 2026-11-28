/**
 * @file   videorec_rt.h
 * @brief  Čisté části režimu "podle reality": hodiny vzorkovače, fronta snímků a převzorkování zvuku SDL výstupu.
 *
 * V režimu podle reality (časová základna `realtime`, viz videorec.h) zapisuje
 * video snímky vzorkovač v tikách monotónních hodin, a to tempem emulace
 * (krystal / takty snímku, PAL 50,036326, NTSC 59,922743 ticku za sekundu);
 * zvuk se bere z výstupu SDL cesty (to, co šlo do reproduktoru), ne
 * z emulačního rendereru. Tento modul obsahuje části bez závislosti na
 * emulátoru, aby šly testovat samostatně:
 *
 * - **st_VIDEOREC_RT_CLOCK** - rozvrh ticků vzorkovače s absolutními termíny
 *   `base + k * period` (bez driftu), dohánění zpoždění a nové založení řady
 *   při velkém zpoždění;
 * - **st_VIDEOREC_RT_FRAMEQ** - malá fronta emulovaných snímků mezi koncem
 *   snímku (emu vlákno) a tikem vzorkovače: každý tik vezme další snímek
 *   v pořadí, takže při stejném tempu se každý emulovaný snímek zapíše právě
 *   jednou bez ohledu na fázi dokončení snímku proti tiku (rezerva fronty
 *   pohltí kolísání); opakování a zahození jen při skutečném rozchodu tempa;
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
 * st_VIDEOREC_RT_FRAMEQ není thread-safe; lepidlo (videorec.c) ho používá jen
 * pod svým listovým zámkem zobrazeného snímku.
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
 * snímek). Snímek trvá podle krystalu, blok má proto 881 nebo 882 vzorků
 * (MZ-800, MZ-700 PAL) a 735 nebo 736 vzorků (MZ-1500, MZ-700 NTSC);
 * videorec_rt_sdl_chunk() pro frekvenci krystal / takty snímku vrací ten
 * delší. Největší je 882 (PAL); iface_audio.c hlídá, že se blok vejde.
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
 * Zpoždění skládá zpožďovací fronta obrazu v lepidle a rezerva fronty snímků
 * (VIDEOREC_RT_FRAMEQ_TARGET - 1 snímků; při 50 i 60 snímcích/s celé
 * zpoždění, zpožďovací fronta je prázdná): zapsaný snímek je v průměru
 * `ticky * perioda + půl periody` starý jako dřív, kdy vzorkovač bral poslední
 * zobrazený snímek a celé zpoždění dávala zpožďovací fronta. Při prázdné
 * frontě snímků (pauza, krokování debuggeru) se zapisuje poslední zobrazený
 * obraz bez zpoždění.
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

/**
 * @brief Tolerance náběhu jitter bufferu [ms]: čtení začne, když naplnění chybí do cíle nejvýš tolik.
 *
 * Bloky SDL cesty podle krystalu mají 881 nebo 882 vzorků (PAL): tři bloky
 * 881 = 2643 vzorků jsou o 3 vzorky pod cílem 2646. Bez tolerance by se čekalo
 * na čtvrtý blok, ticho na začátku každého úseku by bylo o snímek delší
 * (naměřeno 60 ms místo 40 ms) a přebytek by se zahazoval.
 */
#define VIDEOREC_RT_AUDIO_PRIME_TOL_MS 5u

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
 * Perioda je `1000000 * fps_den / fps_num` us jako zlomek (zkrácený
 * největším společným dělitelem); např. 60 snímků/s dá přesně 60 ticků za
 * každou sekundu monotónních hodin, krystal / takty snímku (17 734 475 /
 * 354 432) přesně 50,036326 ticku za sekundu. Mezivýsledky nepřetečou pro
 * řadu delší než 100 dní ani u zlomku krystalu.
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
 * Horní celá část `VIDEOREC_RT_SDL_RATE * fps_den / fps_num`: 882 při 50,
 * 735 při 60 snímcích/s (tam dělení vyjde celé); pro frekvenci krystal /
 * takty snímku 882 (PAL) a 736 (NTSC), tedy delší ze dvou délek bloku, které
 * SDL cesta podle krystalu vydává.
 *
 * @param fps_num Čitatel snímkové frekvence (0 se nahradí 1).
 * @param fps_den Jmenovatel snímkové frekvence (0 se nahradí 1).
 * @return Vzorků na blok; pro fps >= 50 nejvýš VIDEOREC_RT_SDL_CHUNK_MAX.
 * @note Čistá funkce bez vedlejších efektů, libovolné vlákno.
 */
unsigned videorec_rt_sdl_chunk(unsigned fps_num, unsigned fps_den);

/**
 * @brief Cílová úroveň fronty snímků [snímky], měřená v tiku před odebráním.
 *
 * Vzorkovač začne odebírat, až fronta obsahuje tolik snímků; v ustáleném
 * stavu (emulace i vzorkovač stejným tempem) je před každým tikem ve frontě
 * právě tolik snímků a tik vezme nejstarší. Rezerva `TARGET - 1` snímků
 * pokryje zpoždění dokončení snímku o fázi plus dvě periody (aspoň 40 ms
 * PAL, 33,3 ms NTSC) bez opakování; dřívější dokončení frontu jen dočasně
 * prodlouží. Naměřeno (mz800emu, emu-experiments exact-clock/mereni/):
 * headless kolísá dokončení snímku do +-1,8 ms (rtrec-diag-phase-800.txt),
 * se zvukovým zařízením do +-10 ms (snímky se dokončují v periodě zařízení)
 * a navíc se dokončování na několik sekund posouvá o celý snímek oběma směry
 * (rtrec-diag-queue-device-800.txt; zvuk se posouvá s ním). Zpoždění obrazu
 * `(TARGET - 1) * perioda + fáze` je stejné jako celé zpoždění
 * VIDEOREC_RT_VIDEO_DELAY_MS (2 ticky), zpožďovací fronta lepidla je proto
 * při 50 i 60 snímcích/s prázdná.
 */
#define VIDEOREC_RT_FRAMEQ_TARGET 3u

/**
 * @brief Přebytek nad cíl [snímky], který musí trvat celé okno, aby regulace začala zahazovat.
 *
 * Úroveň 1 až TARGET + SURPLUS - 1 (1 až 4) je normální stav: posun
 * dokončování snímků o celý snímek se zvukovým zařízením (viz
 * VIDEOREC_RT_FRAMEQ_TARGET) spolu s periodou zařízení posune úroveň o 2
 * oběma směry a nesmí se opravovat opakováním nebo zahozením (zvuk se
 * posouvá stejně). Nedostatek se neopravuje oknem: až když fronta opravdu
 * dojde, zopakuje se obraz a fronta se naplní znovu (simulace s posunem
 * o snímek a periodou zařízení 10 ms dávala s opravou nedostatku oknem
 * zbytečná opakování).
 */
#define VIDEOREC_RT_FRAMEQ_SURPLUS 2u

/**
 * @brief Doba [s], do které druhé vyprázdnění fronty v živém běhu přepne do pomalého režimu.
 *
 * Emulace, která opakovaně nestíhá tempo krystalu (pomalý počítač, ladicí
 * sestavení), by jinak po každém vyprázdnění opakovala obraz po dávkách do
 * naplnění rezervy. V pomalém režimu se plní jen na 1 snímek, takže se
 * opakuje po jednom a rovnoměrně (jako dřívější vzorkovač "poslední obraz");
 * rezerva se obnoví po jednom opakování za okno, až se fronta po tuto dobu
 * nevyprázdní.
 *
 * Pomalý režim se zapne jen při trvalém nedostatku: druhé vyprázdnění do této
 * doby, mezi kterým a předchozím vyprázdněním úroveň fronty nedosáhla
 * VIDEOREC_RT_FRAMEQ_TARGET + VIDEOREC_RT_FRAMEQ_SURPLUS (`ahead`). Emulace,
 * která jen ojediněle zasekla a pak zaostání dohnala, frontu po zaseknutí
 * přeplní nad tuto mez; pomalý počítač snímky nikdy nedohání, takže mez
 * nepřekročí (rozkolísání o jeden snímek ji nedosáhne). Úroveň nad cílem
 * v pomalém režimu (emulace zase napřed) pomalý režim hned ukončí, aby
 * přebytek vybíjela běžná regulace.
 */
#define VIDEOREC_RT_FRAMEQ_SLOW_S 10u

/**
 * @brief Kapacita fronty snímků [snímky]; při přetečení se zahazuje nejstarší.
 *
 * Fronta drží jen ukazatele (obraz sdílí lepidlo, nezměněné snímky mají
 * společný obraz). Pokryje cíl plus dohnání pozdního probuzení vzorkovače
 * až o 1 s (max_catchup lepidla = ticky za 1 s), takže se při něm nic
 * nezahodí. Paměť v nejhorším případě (100 %, vzorkovač stojí 1 s, každý
 * snímek jiný obraz): 64 obrazů, na MZ-800 asi 17 MB. Při MAX SPEED se
 * zobrazuje nejvýš 50 (60) obrazů za sekundu, položky sdílejí málo obrazů.
 */
#define VIDEOREC_RT_FRAMEQ_CAP 64u

/** @brief Režim odběru z fronty snímků (videorec_rt_frameq_take()). */
typedef enum {
    VIDEOREC_RT_FRAMEQ_LIVE = 0, /**< Živý běh 100 %: plnění na cíl, zahození trvalého přebytku (okno), prázdná fronta = opakování a nové plnění. */
    VIDEOREC_RT_FRAMEQ_FREE,     /**< Rychlost != 100 %: přebytek nad cíl se zahodí, jinak další snímek, prázdná = poslední obraz. */
    VIDEOREC_RT_FRAMEQ_DRAIN     /**< Pauza emulace se zápisem: dopsat zbylé snímky, pak poslední obraz. */
} en_VIDEOREC_RT_FRAMEQ_MODE;

/** @brief Co má tik zapsat (výsledek videorec_rt_frameq_take()). */
typedef enum {
    VIDEOREC_RT_TAKE_FRAME = 0, /**< Další snímek z fronty (`*item_out`, vlastnictví přechází na volajícího). */
    VIDEOREC_RT_TAKE_PREV,      /**< Zopakovat obraz zapsaný minulým tikem (ve frontě jsou novější snímky, ještě se nesmí předběhnout). */
    VIDEOREC_RT_TAKE_LATEST     /**< Fronta je prázdná: poslední zobrazený obraz (může být novější než minulý zápis, např. krok debuggeru). */
} en_VIDEOREC_RT_TAKE;

/** @brief Statistiky fronty snímků (čítače od videorec_rt_frameq_init(); videorec_rt_frameq_clear() je nemaže). */
typedef struct st_VIDEOREC_RT_FRAMEQ_STATS {
    uint64_t pushed;         /**< Vložených snímků. */
    uint64_t taken;          /**< Snímků odebraných tikem (VIDEOREC_RT_TAKE_FRAME). */
    uint64_t overflow_drops; /**< Zahozených při plné kapacitě (vkládání). */
    uint64_t surplus_drops;  /**< Zahozených regulací (přebytek v okně LIVE, nad cíl ve FREE, videorec_rt_frameq_trim(), duplicity obrazu videorec_rt_frameq_merge_dups()). */
    uint64_t underflows;     /**< Vyprázdnění fronty v živém běhu mimo plnění (emulace nestíhá; opakování a nové plnění). */
    uint64_t restore_fills;  /**< Opakování při obnově rezervy po pomalém režimu. */
    uint64_t prime_trims;    /**< Zahozených při dokončení plnění nad cíl (dva snímky mezi tiky v náběhu). */
    uint64_t purged;         /**< Odebraných podle značky (videorec_rt_frameq_purge_tag()). */
} st_VIDEOREC_RT_FRAMEQ_STATS;

/**
 * @brief Fronta emulovaných snímků pro vzorkovač (kruhová, pevná kapacita).
 *
 * Položky jsou neprůhledné ukazatele (lepidlo: sdílený obraz s počitadlem
 * referencí); fronta je uvolňuje callbackem `release` při zahození
 * a vyčištění. Ke každé položce patří 8bitová značka (lepidlo: snímek
 * z běhu k dočasnému breakpointu).
 *
 * Regulace v režimu LIVE: nejmenší úroveň fronty před odebráním se sleduje
 * v okně `window` ticků; je-li v celém okně aspoň VIDEOREC_RT_FRAMEQ_TARGET +
 * VIDEOREC_RT_FRAMEQ_SURPLUS, začne vybíjení přebytku (emulace trvale
 * napřed: zkrácené snímky CKSW, zvukové zařízení s rychlejšími hodinami,
 * dohnané zaseknutí): po jednom nejstarším snímku za okno, dokud úroveň
 * v celém okně neklesne na cíl (velký přebytek se nejdřív srazí najednou na
 * horní mez normálního stavu). Emulace pozadu se pozná až prázdnou frontou:
 * zopakuje se obraz a fronta se naplní na cíl; opakuje-li se vyprázdnění do
 * VIDEOREC_RT_FRAMEQ_SLOW_S bez dohnání mezi nimi, plní se jen na 1 snímek
 * (pomalý režim, ukončí ho úroveň nad cílem). Krátké
 * kolísání (jednotlivé snímky dřív nebo později) ani posun o snímek se
 * neopravuje, takže nevznikají páry opakování a zahození.
 *
 * Invarianty: `len <= VIDEOREC_RT_FRAMEQ_CAP`, `head < VIDEOREC_RT_FRAMEQ_CAP`;
 * platné položky jsou `item[(head + i) % CAP]` pro `i < len`.
 *
 * Ownership: položky ve frontě vlastní fronta; videorec_rt_frameq_pop()
 * a videorec_rt_frameq_take() s VIDEOREC_RT_TAKE_FRAME předávají vlastnictví
 * volajícímu.
 */
typedef struct st_VIDEOREC_RT_FRAMEQ {
    void *item[VIDEOREC_RT_FRAMEQ_CAP];  /**< Položky (kruhově od `head`). */
    uint8_t tag[VIDEOREC_RT_FRAMEQ_CAP]; /**< Značky položek. */
    unsigned head;                       /**< Index nejstarší položky. */
    unsigned len;                        /**< Počet položek. */
    unsigned window;                     /**< Délka okna regulace [ticky], >= 1. */
    unsigned win_n;                      /**< Ticků v aktuálním okně. */
    unsigned win_min;                    /**< Nejmenší úroveň v okně (UINT_MAX = prázdné okno). */
    bool starved;                        /**< Plní se na cíl (start, po vyprázdnění, po pauze nebo jiné rychlosti). */
    bool filling;                        /**< Plnění už probíhá v režimu LIVE (vkládají se opakování); `filling` => `starved`. */
    bool draining;                       /**< Vybíjí se trvalý přebytek (po jednom snímku za okno). */
    bool slow;                           /**< Pomalý režim: plní se jen na 1 snímek (viz VIDEOREC_RT_FRAMEQ_SLOW_S). */
    bool restarted;                      /**< Plnění po vyčištění nebo videorec_rt_frameq_restart() (ne po vyprázdnění): přesah při dokončení se zahodí. */
    bool ahead;                          /**< Od posledního vyprázdnění v LIVE dosáhla úroveň TARGET + SURPLUS (emulace dohnala zaostání); vyprázdnění pak pomalý režim nezapne. */
    unsigned underflow_ago;              /**< Odběrů od posledního vyprázdnění v LIVE (UINT_MAX = žádné). */
    en_VIDEOREC_RT_FRAMEQ_MODE last_mode; /**< Režim posledního odběru (VIDEOREC_RT_FRAMEQ_LIVE po vyčištění). */
    void (*release)(void *item);         /**< Uvolnění zahozené položky (NULL = nic). */
    st_VIDEOREC_RT_FRAMEQ_STATS stats;   /**< Statistiky. */
} st_VIDEOREC_RT_FRAMEQ;

/**
 * @brief Inicializuje prázdnou frontu ve stavu plnění.
 * @param q       Instance (nesmí být NULL; obsah se přepíše).
 * @param window  Délka okna regulace [ticky] (0 se nahradí 1); lepidlo: ticky za 1 s.
 * @param release Uvolnění položky, nebo NULL.
 * @post Prázdná, `starved`, statistiky nulové.
 */
void videorec_rt_frameq_init(st_VIDEOREC_RT_FRAMEQ *q, unsigned window, void (*release)(void *item));

/**
 * @brief Uvolní všechny položky a přejde do stavu plnění (statistiky zůstávají).
 * @param q Instance.
 * @post `len == 0`, `starved`, ne pomalý režim, `last_mode == LIVE`.
 */
void videorec_rt_frameq_clear(st_VIDEOREC_RT_FRAMEQ *q);

/**
 * @brief Přejde do stavu plnění bez zahození položek (po dopsání fronty, po obnovení pauzy nahrávání).
 *
 * Další odběr v LIVE nejdřív naplní frontu na cíl; prázdná fronta se přitom
 * nepočítá jako vyprázdnění (`underflows`).
 *
 * @param q Instance.
 * @post `starved`, `!filling`, `!draining`, nové okno regulace. Příznak `ahead` se nemění
 *       (dohnání před přerušením živého běhu platí dál, nuluje ho až vyprázdnění fronty).
 */
void videorec_rt_frameq_restart(st_VIDEOREC_RT_FRAMEQ *q);

/**
 * @brief Zahodí nejstarší položky nad `keep` (`surplus_drops`).
 * @param q    Instance.
 * @param keep Kolik nejnovějších položek ponechat.
 * @return Počet zahozených.
 */
unsigned videorec_rt_frameq_trim(st_VIDEOREC_RT_FRAMEQ *q, unsigned keep);

/**
 * @brief Sloučí sousední položky se stejným obrazem (stejný ukazatel a značka) do jedné.
 *
 * Při rychlosti nad 100 % emulace zobrazí jen jeden obraz za synchronizaci
 * (20 ms), takže několik emulovaných snímků za sebou ve frontě sdílí tentýž
 * obraz (naměřeno při 200 %: dvojice). Režim FREE pak nesmí odebírat po
 * položkách - tik by podle fáze vzal tentýž obraz dvakrát za sebou
 * (opakování) a jindy dva obrazy přeskočil. Po sloučení odpovídá jedna
 * položka jednomu zobrazenému obrazu (jako dřívější slot "poslední obraz").
 * V režimu LIVE se neslučuje: tam je každý emulovaný snímek jeden snímek
 * videa i při nezměněném obraze.
 *
 * @param q Instance.
 * @return Počet zahozených duplicit (počítají se do `surplus_drops`, uvolní je `release`).
 * @post Žádné dvě sousední položky nemají stejný ukazatel a značku; pořadí zachováno.
 */
unsigned videorec_rt_frameq_merge_dups(st_VIDEOREC_RT_FRAMEQ *q);

/**
 * @brief Kolik snímků se podle aktuálního stavu zapíše před obrazem, který je právě na obrazovce (pozice značek).
 *
 * Podle režimu posledního odběru:
 * - FREE: další tik vezme nejstarší z nejvýš TARGET různých obrazů (sousední
 *   položky se stejným obrazem se slučují, videorec_rt_frameq_merge_dups()),
 *   obraz na obrazovce je nejnovější: `min(různých, TARGET) - 1` (0 u prázdné
 *   fronty);
 * - LIVE v pomalém režimu (plní se jen na 1 snímek, opakování se nevkládají
 *   před obraz z fronty): `len - 1` (0 u prázdné fronty: zapíše se právě
 *   zobrazený obraz), nejvýš TARGET - 1;
 * - LIVE při plnění (`filling`): rezerva TARGET - 1 (opakování až do ní);
 * - jinak (LIVE, DRAIN, stav plnění po pauze nebo dopsání, který ještě
 *   neplní): `min(len, TARGET - 1)`.
 * V ustáleném živém běhu je to rezerva, takže pozice odpovídá dřívějšímu
 * konstantnímu zpoždění; +-1 snímek podle fáze.
 *
 * @param q Instance.
 * @return Počet snímků (0 .. TARGET - 1).
 * @note Čistá funkce bez vedlejších efektů.
 */
unsigned videorec_rt_frameq_pos_offset(const st_VIDEOREC_RT_FRAMEQ *q);

/**
 * @brief Vloží snímek na konec fronty; plná fronta zahodí nejstarší (`overflow_drops`).
 * @param q    Instance.
 * @param item Položka (vlastnictví přechází na frontu; smí být NULL).
 * @param tag  Značka položky.
 */
void videorec_rt_frameq_push(st_VIDEOREC_RT_FRAMEQ *q, void *item, uint8_t tag);

/**
 * @brief Odebere nejstarší položku bez regulace (dopsání fronty na konci úseku).
 * @param q       Instance.
 * @param tag_out Výstup značky, nebo NULL.
 * @return Položka (vlastnictví přechází na volajícího), NULL u prázdné fronty
 *         (prázdnou frontu od položky NULL rozliší videorec_rt_frameq_len() před voláním).
 */
void *videorec_rt_frameq_pop(st_VIDEOREC_RT_FRAMEQ *q, uint8_t *tag_out);

/**
 * @brief Odebere (a uvolní) všechny položky se značkou `tag`; pořadí ostatních zůstává.
 * @param q   Instance.
 * @param tag Značka.
 * @return Počet odebraných položek (přičte se k `purged`).
 */
unsigned videorec_rt_frameq_purge_tag(st_VIDEOREC_RT_FRAMEQ *q, uint8_t tag);

/** @brief Počet snímků ve frontě. @param q Instance. @return `len`. */
static inline unsigned videorec_rt_frameq_len(const st_VIDEOREC_RT_FRAMEQ *q)
{
    return q->len;
}

/**
 * @brief Rozhodne, co zapíše jeden tik vzorkovače, a případně odebere snímek.
 *
 * - VIDEOREC_RT_FRAMEQ_LIVE: ve stavu plnění vrací VIDEOREC_RT_TAKE_PREV
 *   (fronta neprázdná) nebo VIDEOREC_RT_TAKE_LATEST (prázdná), dokud úroveň
 *   nedosáhne VIDEOREC_RT_FRAMEQ_TARGET (v pomalém režimu 1); přesah nad cíl
 *   v okamžiku dokončení plnění po startu nebo po videorec_rt_frameq_restart()
 *   (dva snímky mezi tiky; ne po vyprázdnění, kdy by přesah vznikal z doběhnutí
 *   emulace a zahození by dělalo páry opakování a vynechání) se zahodí
 *   (`prime_trims`; jinak by obraz zůstal trvale o snímek později proti
 *   zvuku); pak odebírá jeden snímek za tik. Prázdná fronta = vyprázdnění (`underflows`),
 *   VIDEOREC_RT_TAKE_LATEST a nové plnění; druhé vyprázdnění do
 *   VIDEOREC_RT_FRAMEQ_SLOW_S, mezi kterým a předchozím úroveň nedosáhla
 *   cíl + VIDEOREC_RT_FRAMEQ_SURPLUS (`ahead`), zapne pomalý režim; úroveň
 *   nad cílem ho hned ukončí. Na konci každého okna
 *   regulace: úroveň v celém okně aspoň cíl + VIDEOREC_RT_FRAMEQ_SURPLUS
 *   zahájí vybíjení (po jednom snímku za okno do cíle, `surplus_drops`, ve
 *   frontě zůstane aspoň 1 snímek); v pomalém režimu po
 *   VIDEOREC_RT_FRAMEQ_SLOW_S bez vyprázdnění jedno VIDEOREC_RT_TAKE_PREV za
 *   okno (`restore_fills`), dokud úroveň nedosáhne cíle.
 * - VIDEOREC_RT_FRAMEQ_FREE: sloučí sousední položky se stejným obrazem
 *   (videorec_rt_frameq_merge_dups()), zahodí přebytek nad cíl
 *   (`surplus_drops`), pak jako DRAIN.
 * - VIDEOREC_RT_FRAMEQ_DRAIN: neprázdná fronta = další snímek, prázdná =
 *   VIDEOREC_RT_TAKE_LATEST.
 * FREE a DRAIN nastaví stav plnění (videorec_rt_frameq_restart()), takže po
 * návratu do LIVE se fronta nejdřív naplní na cíl (zpoždění obrazu proti
 * zvuku se obnoví). Režim se zapamatuje (`last_mode`, pro
 * videorec_rt_frameq_pos_offset()).
 *
 * @param q        Instance.
 * @param mode     Režim odběru.
 * @param item_out Výstup položky při VIDEOREC_RT_TAKE_FRAME (jinak NULL); nesmí být NULL.
 * @return Co zapsat.
 */
en_VIDEOREC_RT_TAKE videorec_rt_frameq_take(st_VIDEOREC_RT_FRAMEQ *q, en_VIDEOREC_RT_FRAMEQ_MODE mode, void **item_out);

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
    uint64_t prime_drops;    /**< Vstupních vzorků zahozených při náběhu nad cíl (náběh přesně na `target`). */
    double corr;            /**< Aktuální relativní korekce poměru převzorkování. */
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
    double tick_scale; /**< Skutečná délka ticku / jmenovitá (`out_frames / out_rate` s); násobí poměr převzorkování, > 0, výchozí 1. */
    st_VIDEOREC_RT_AUDIO_STATS stats;/**< Statistiky (corr a fill_ema jsou zároveň stav regulátoru). */
} st_VIDEOREC_RT_AUDIO;

/**
 * @brief Inicializuje buffer.
 * @param ra         Instance (nesmí být NULL; obsah se přepíše).
 * @param in_rate    Vstupní frekvence [Hz] (> 0).
 * @param out_rate   Výstupní frekvence [Hz] (> 0).
 * @param target_ms  Cílové naplnění [ms] (VIDEOREC_RT_AUDIO_TARGET_MS).
 * @param capacity_ms Kapacita [ms] (> target_ms; VIDEOREC_RT_AUDIO_CAPACITY_MS).
 * @post Prázdný, `enabled == false`, `starved == true`, statistiky nulové, `tick_scale == 1`.
 * @note Selhat nemůže (g_malloc při nedostatku paměti ukončí proces).
 */
void videorec_rt_audio_init(st_VIDEOREC_RT_AUDIO *ra, unsigned in_rate, unsigned out_rate, unsigned target_ms,
                            unsigned capacity_ms);

/**
 * @brief Nastaví poměr skutečné délky ticku vzorkovače ke jmenovité (dopředná část korekce driftu).
 *
 * Spotřebitel bere za tik `out_frames` výstupních vzorků, které v souboru
 * trvají `out_frames / out_rate` s (jmenovitý tik, AVI 50/1 nebo 60/1).
 * Tiká-li vzorkovač jiným známým tempem (tempem emulace podle krystalu),
 * přichází za tik `in_rate * skutečná délka ticku` vstupních vzorků; poměr
 * převzorkování se proto násobí `scale` = skutečná / jmenovitá délka ticku
 * (PAL 50 * 354 432 / 17 734 475 = 0,999274, NTSC 60 * 238 944 / 14 318 180 =
 * 1,001289). Regulátor driftu pak dorovnává jen hodiny zařízení a nedrží
 * trvalou odchylku naplnění (posun zvuku proti obrazu by byl
 * `(scale - 1) * in_rate * VIDEOREC_RT_AUDIO_DRIFT_TC_S` vzorků: PAL -7,3 ms,
 * NTSC +12,9 ms).
 *
 * @param ra    Instance.
 * @param scale Poměr (> 0; jinak se použije 1). Platí do další změny,
 *              videorec_rt_audio_reset() ho nemění.
 * @par Vlákna Libovolné vlákno (bere zámek instance).
 */
void videorec_rt_audio_set_tick_scale(st_VIDEOREC_RT_AUDIO *ra, double scale);

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
 *   nedosáhne `target` (s tolerancí VIDEOREC_RT_AUDIO_PRIME_TOL_MS); pak začne
 *   číst. Přesah posledního bloku nad `target` (do `target * 4`) se přitom
 *   zahodí od nejstarších vzorků (`prime_drops`), takže čtení začíná
 *   s cílovým naplněním (nejvýš o toleranci méně).
 * - Naplnění nad `target * 4` (producent přidal víc, než se stihlo
 *   spotřebovat - např. dlouho nevolaný pull) se zahodí na `target`
 *   (`overruns`).
 * - Poměr převzorkování `in_rate / out_rate * tick_scale * (1 + corr)`, kde `corr` je
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
