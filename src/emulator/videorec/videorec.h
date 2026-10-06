/**
 * @file   videorec.h
 * @brief  Lepidlo video záznamu: řízení session, hooky v jádře, writer vlákno.
 *
 * Modul spojuje čisté části video záznamu (párování obrazu se zvukem
 * videorec_pipe, model segmentů videorec_sidecar, ZMBV enkodér a AVI zapisovač
 * z mzlib_avirec) s emulátorem:
 *
 * - **Hook konce snímku** videorec_on_screen_done() (emu vlákno) bere hotový
 *   framebuffer každého emulovaného snímku. V časové základně **emulated**
 *   (výchozí) platí: jeden emulovaný snímek = jeden video snímek (snímková
 *   frekvence platformy: 50 fps PAL, 60 fps NTSC),
 *   nezávisle na rychlosti emulace (100 %, custom speed, MAX SPEED).
 * - **Režim podle reality** (časová základna **realtime**, přepínatelná za
 *   běhu přes videorec_request_timebase()): vlákno vzorkovače
 *   ("videorec-rt") 50x (60x u NTSC) za sekundu monotónních hodin zapíše poslední snímek,
 *   který šel na obrazovku (tap videorec_on_displayed_frame() v
 *   iface_video_framebuffer_screen_done()), se zvukem z výstupu SDL cesty
 *   (tap videorec_rt_audio_output() v iface_audio_wait_for_data()),
 *   převzorkovaným a zarovnaným k obrazu (videorec_rt.h). Nahrávka tak
 *   ukazuje, co uživatel viděl a slyšel: turbo je zrychlené, pauza podle
 *   `realtime_pause`. Přepnutí časové základny je hranice segmentu.
 *   Podrobnosti viz "Režim podle reality" níže.
 * - **Tap zvuku** videorec_audio_tap() a videorec_audio_horizon() (emu vlákno)
 *   předávají živé změny hodnot zvukových kanálů a horizont doručení.
 * - **Writer vlákno** ("videorec-writer") kóduje snímky (ZMBV) a zapisuje AVI.
 *   S emu vláknem komunikuje přes GAsyncQueue zpráv; při plné frontě emu vlákno
 *   čeká (backpressure - zpomalí MAX SPEED, ale nic se neztratí).
 * - **Požadavky z UI** (start, stop, pauza nahrávání, marker) se jen zaznamenají
 *   a provedou se na nejbližším konci snímku v emu vlákně. Stop se v pauze
 *   emulace provede hned v paused smyčce (videorec_on_emulation_paused()).
 * - **Události pro UI** (start, uložení, selhání, retake, šev) jsou
 *   strukturované (videorec_get_event()); jádro je bez i18n, text skládá UI.
 *
 * Výstup: `<jméno>.avi` (při překročení AVI_WRITER_MAX_BYTES další party
 * `<jméno>_002.avi`, ...) a sidecar `<jméno>.cuts.json` zapsaný při ukončení
 * nahrávání.
 *
 * @par Podpora platforem
 * Nahrávat jde na všech platformách: MZ-700 PAL a NTSC, MZ-800 a MZ-1500.
 * Vše, čím se liší, bere lepidlo z per-arch maker přes st_VIDEOREC_PLATFORM
 * (videorec_platform_arch.h): rozměry framebufferu (MZ-800 928x288,
 * MZ-700 a MZ-1500 704x232; AVI má řádky zdvojené), takty na snímek
 * a GDG takty, snímková frekvence (PAL 50, NTSC 60 fps), počet zvukových
 * kanálů (MZ-700 jen CTC0, MZ-800 a MZ-1500 CTC0 + 2x4 PSG). Zvuk má
 * v AVI vždy 2 kanály: při jednom PSG (a na MZ-700) L == R, při dvou PSG
 * (MZ-1500, MZ-800 s `allow_psg1`) skutečné stereo jako SDL cesta
 * (L = CTC0 + PSG0, R = CTC0 + PSG1, viz videorec_audio_set_stereo()).
 * Frekvence zvuku musí dát celý počet vzorků na snímek (48000/60 = 800,
 * 44100/60 = 735), jinak videorec_request_start() vrátí chybu. Sidecar
 * (verze 4) popisuje platformu, framebuffer, canvas a skutečné fps.
 *
 * @par Vlákna a zámky
 * Stav session je pod hlavním zámkem modulu (`g_vr.mutex`, jediný
 * "nelistový" zámek). Vedle něj existují krátké listové zámky, které se smí
 * brát pod hlavním zámkem, ale pod nimi se už žádný jiný zámek modulu nebere:
 * | Zámek | Chrání | Kdo ho bere |
 * |-------|--------|-------------|
 * | `g_vr.mutex` | stav session (požadavky, pipe, sidecar, větve, realtime fronta) | emu vlákno (hooky), UI/MCP (`videorec_request_*()`), vzorkovač (tick) |
 * | `g_vr_status_mutex` | zveřejněná kopie stavu pro videorec_get_status() | lepidlo pod `g_vr.mutex`, UI, writer |
 * | `g_vr_err_mutex` | text poslední chyby | kdokoli, i pod `g_vr.mutex` |
 * | `g_vr_rtv_mutex` | poslední zobrazený snímek (realtime) | emu vlákno (tap), vzorkovač pod `g_vr.mutex` |
 * | `g_vr_rtc.mutex` | řízení vlákna vzorkovače (termín, běh) | vzorkovač jen při čekání, lepidlo pod `g_vr.mutex` |
 * | mutex jitter bufferu (st_VIDEOREC_RT_AUDIO) | zvuk SDL cesty pro realtime | SDL audio callback (jen tento zámek), vzorkovač pod `g_vr.mutex` |
 * | mutex bariéry st_VR_SYNC | retake bariéra s writerem | emu vlákno (čeká pod `g_vr.mutex`), writer (nikdy nebere `g_vr.mutex`) |
 *
 * Pořadí je vždy `g_vr.mutex` -> listový zámek, takže cyklus nevznikne.
 * Vlákna: emu (hooky, MCP příkazy přes dbgapi se zpracují také v emu
 * vlákně), UI, writer "videorec-writer", vzorkovač "videorec-rt" a SDL
 * audio callback. Rychlý test videorec_is_active() je jedno atomické čtení,
 * kterým hooky v hot path zjišťují, zda vůbec mají něco dělat;
 * videorec_get_status() čte jen zveřejněnou kopii stavu pod listovým
 * zámkem (UI se jí nikdy nezdrží na emu vlákně). Funkce `videorec_request_*()` volá UI
 * (nebo jiné) vlákno, hooky volá výhradně emu vlákno,
 * videorec_on_snapshot_loaded() volá vlákno nahrávající snapshot: UI vlákno
 * při pozastavené emulaci, nebo emu vlákno při zpracování dbgapi/MCP příkazu
 * (v paused smyčce i za běhu emulace mezi snímky - viz popis funkce).
 * Writer vlákno nikdy nebere zámek modulu; lepidlo na writer čeká jen při
 * backpressure (bez zámku) a při bariéře před retake (vr_writer_sync()).
 *
 * @par Režim podle reality
 * - **Vlákna:** vzorkovač běží ve vlastním vlákně "videorec-rt" po celou dobu
 *   session (v obou časových základnách - sleduje i stav emulátoru pro
 *   události). Tick bere zámek modulu jen na dobu zpracování ticku (kopie
 *   snímku, vyrobení zvuku jednoho snímku, předání writeru); na writer nikdy nečeká
 *   (reálný čas nelze zdržet - backpressure se nepoužívá). Emu vlákno
 *   v ustáleném realtime stavu stojí na konci snímku jedno atomické čtení;
 *   tap zobrazeného snímku kopíruje snímek pod vlastním krátkým zámkem
 *   (nejvýš 50x / 60x za sekundu i při MAX SPEED, protože i zobrazení je omezené
 *   na jeden snímek); tap zvuku kopíruje blok SDL cesty (882 / 735 vzorků)
 *   pod zámkem jitter bufferu.
 * - **Zpoždění obrazu:** obraz se zapisuje se zpožděním
 *   VIDEOREC_RT_VIDEO_DELAY_MS (40 ms, na ticky zaokrouhleno: 2 při 50 i 60
 *   snímcích/s), aby seděl se zvukem, který prochází jitter bufferem
 *   (VIDEOREC_RT_AUDIO_TARGET_MS); naměřená průměrná odchylka zvuku proti
 *   obrazu je do +-15 ms při 50 i 60 snímcích/s (GUI i headless), měření
 *   viz videorec_rt.h. Značky (markery, události,
 *   hranice segmentů) se proto v realtime vztahují k pozici `počet snímků +
 *   délka zpožďovací fronty` = snímek, ve kterém se aktuální obraz objeví.
 * - **Pauza emulace** podle `realtime_pause`: `skip` = nezapisuje se (hodiny
 *   vzorkovače stojí, obraz i zvuk po pauze navážou), `freeze` = zapisuje se
 *   zamrzlý obraz a ticho, `freeze_capped` = jako freeze nejvýš
 *   `realtime_pause_cap_s` sekund, pak skip.
 * - **Krokování debuggeru** = pauza, během které proběhla akce debuggeru
 *   (krok, step over, run to cursor - videorec_on_debugger_step()), pauza
 *   zastavená breakpointem (`g_emulator.pause_reason`), a také běh
 *   k dočasnému breakpointu a pauza po něm (step over a run to cursor krátce
 *   zruší `g_emulator.paused`; krokování končí ručním pokračováním bez akce
 *   debuggeru). Zapisuje se jen při `record_debugger_steps` (pak jako freeze
 *   bez limitu - obraz se mění s každým krokem, pokud debugger obnovuje
 *   obrazovku), jinak skip.
 *   V časové základně emulated nastavení nemá vliv (snímky vzniklé
 *   krokováním jsou emulační čas jako každý jiný).
 * - **Rychlost** podle `realtime_speed`: `as_seen` = zapisuje se, co je na
 *   obrazovce (turbo zrychleně); `emulated_when_fast` = při rychlosti != 100 %
 *   (vlastní rychlost i MAX SPEED) se nahrávání automaticky přepne do
 *   emulačního času a po návratu na 100 % zpět (obojí hranice segmentu).
 *   Zvuk při rychlosti > 100 % podle `realtime_turbo_audio`: `as_heard` =
 *   jak ho hraje SDL cesta, `silence` = ticho, `attenuate` = útlum na
 *   VIDEOREC_TURBO_ATTENUATE_GAIN.
 * - **Snapshot a retake:** retake (DISCARD) je pojem emulačního času.
 *   Nahrání snapshotu v realtime je vždy šev (nový segment s výchozím
 *   přechodem, nová větev, událost SEAM); snapshot uložený v realtime úseku
 *   nikdy nevede na retake (větev se označí jako realtime). Retake na bod
 *   v emulačním úseku před realtime úsekem zůstává možný (realtime snímky
 *   za bodem se zahodí jako každé jiné).
 * - **Headless a GUI bez audio zařízení** (`sync_by_timer`; v GUI po selhání
 *   otevření zařízení): zvuk se vyrábí stejnou
 *   SDL cestou v synchronizaci emu vlákna jednou za periodu snímku
 *   (20 ms při 50, 16,7 ms při 60 snímcích/s; iface_audio_20ms_sync()),
 *   jen když jitter buffer potřebuje data - emuluje se tak zařízení, které
 *   si data žádá. Nic se nepřehrává; nahrávka má zvuk "jak by zněl".
 *
 * @par Události stavu (sidecar od verze 3)
 * Při `state_marks = sidecar` se do sidecaru zapisují události
 * `{frame, kind, value}` (VIDEOREC_SC_EVENT_*): časová základna (start
 * a každé přepnutí), rychlost (start a každá změna), začátek a konec pauzy
 * emulace, nahrání snapshotu (retake / šev) a reset. Při `auto_markers` se
 * navíc pro pauzu, změnu rychlosti, nahrání snapshotu a reset vloží běžný
 * marker s anglickým popiskem ("Pause", "Speed 400%", "Speed MAX",
 * "Snapshot loaded", "Reset"). Markery "Pause", "Speed ..." a "Snapshot
 * loaded" jen v efektivní základně realtime: v emulačním čase pauza ani
 * změna rychlosti ve videu vidět nejsou, retake navazuje bez švu a šev je
 * už hranice segmentu s přechodem - marker by byl šum (MCP agent navíc
 * pauzuje po každém kroku). Událost v `events`
 * vzniká v obou základnách. "Pause" navíc jen pro pauzu uživatele
 * (krokování debuggeru a doběhnutí N snímků marker nedostávají). "Reset"
 * se vkládá v obou základnách. V emulačním čase sleduje pauzu i rychlost
 * emu vlákno (začátek pauzy v paused smyčce, konec a změnu rychlosti na
 * konci emulovaného snímku) - pozice jsou přesné a zachytí se i pauza
 * kratší než jeden snímek. V realtime je sleduje vzorkovač (50 / 60 Hz); pozice je
 * pozice obrazu, pauza kratší než tick se nezaznamená.
 * Retake zahodí události od bodu snapshotu a v bodě snapshotu znovu zapíše
 * platný stav (časová základna, rychlost, probíhající pauza; bez auto
 * markerů), takže časová osa stavu v sidecaru zůstává úplná.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_H
#define VIDEOREC_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "videorec_audio.h"
#include "videorec_rt.h"
#include "videorec_sidecar.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Co udělat s nahrávkou po nahrání snapshotu během nahrávání (retake). */
typedef enum {
    VIDEOREC_RETAKE_OFF = 0,     /**< Žádný retake: vznikne šev (nový segment) s výchozím přechodem. */
    VIDEOREC_RETAKE_DISCARD = 1, /**< Snapshot z této session: zahodit snímky po bodu snapshotu. */
    VIDEOREC_RETAKE_SEAM = 2     /**< Ponechat vše, v místě nahrání vznikne šev s přechodem. */
} en_VIDEOREC_RETAKE;

/** @brief Časová základna nahrávky. */
typedef enum {
    VIDEOREC_TIMEBASE_EMULATED = 0, /**< Emulační čas: jeden emulovaný snímek = jeden video snímek. */
    VIDEOREC_TIMEBASE_REALTIME = 1  /**< Podle reality: 50 (60 u NTSC) snímků za sekundu reálného času, co šlo na obrazovku. */
} en_VIDEOREC_TIMEBASE;

/** @brief Chování režimu podle reality při pauze emulace (INI `realtime_pause`). */
typedef enum {
    VIDEOREC_RT_PAUSE_SKIP = 0,     /**< `skip`: během pauzy se nezapisuje (nahrávka pauzu přeskočí). */
    VIDEOREC_RT_PAUSE_FREEZE = 1,   /**< `freeze`: zapisuje se zamrzlý obraz a ticho po celou pauzu. */
    VIDEOREC_RT_PAUSE_FREEZE_CAPPED /**< `freeze_capped`: jako freeze nejvýš `realtime_pause_cap_s` sekund, pak skip. */
} en_VIDEOREC_RT_PAUSE;

/** @brief Chování režimu podle reality při rychlosti emulace != 100 % (INI `realtime_speed`). */
typedef enum {
    VIDEOREC_RT_SPEED_AS_SEEN = 0,          /**< `as_seen`: zapisuje se, co je na obrazovce (zrychleně / zpomaleně). */
    VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST = 1 /**< `emulated_when_fast`: při rychlosti != 100 % se nahrává v emulačním čase. */
} en_VIDEOREC_RT_SPEED;

/** @brief Zvuk režimu podle reality při rychlosti > 100 % (INI `realtime_turbo_audio`). */
typedef enum {
    VIDEOREC_TURBO_AUDIO_AS_HEARD = 0, /**< `as_heard`: zvuk tak, jak ho hraje SDL cesta. */
    VIDEOREC_TURBO_AUDIO_SILENCE = 1,  /**< `silence`: ticho. */
    VIDEOREC_TURBO_AUDIO_ATTENUATE     /**< `attenuate`: zeslabený na VIDEOREC_TURBO_ATTENUATE_GAIN. */
} en_VIDEOREC_TURBO_AUDIO;

/** @brief Zápis událostí stavu do sidecaru (INI `state_marks`). */
typedef enum {
    VIDEOREC_STATE_MARKS_NONE = 0,   /**< `none`: pole `events` zůstane prázdné. */
    VIDEOREC_STATE_MARKS_SIDECAR = 1 /**< `sidecar`: události stavu do sidecaru (pro export s indikátory). */
} en_VIDEOREC_STATE_MARKS;

/** @brief Zesílení zvuku při `realtime_turbo_audio = attenuate` (0,25 = -12 dB; rozhodnutí Task 18). */
#define VIDEOREC_TURBO_ATTENUATE_GAIN 0.25f

/** @brief Výchozí limit zamrzlé pauzy `realtime_pause_cap_s` [s]. */
#define VIDEOREC_RT_PAUSE_CAP_DEFAULT_S 3u
/** @brief Minimum `realtime_pause_cap_s` [s]. */
#define VIDEOREC_RT_PAUSE_CAP_MIN_S 1u
/** @brief Maximum `realtime_pause_cap_s` [s]. */
#define VIDEOREC_RT_PAUSE_CAP_MAX_S 60u

/**
 * @brief Název časové základny pro INI a UI ("emulated", "realtime").
 * @param v Hodnota.
 * @return Statický řetězec; neplatná hodnota = "emulated".
 */
const char *videorec_timebase_name(en_VIDEOREC_TIMEBASE v);
/**
 * @brief Převede název na časovou základnu.
 * @param name Název (nesmí být NULL).
 * @param out  Výstup; při neúspěchu se nemění.
 * @return true při známém názvu.
 */
bool videorec_timebase_from_name(const char *name, en_VIDEOREC_TIMEBASE *out);
/**
 * @brief Název chování při pauze ("skip", "freeze", "freeze_capped").
 * @param v Hodnota.
 * @return Statický řetězec; neplatná hodnota = "skip".
 */
const char *videorec_rt_pause_name(en_VIDEOREC_RT_PAUSE v);
/**
 * @brief Převede název na chování při pauze.
 * @param name Název (nesmí být NULL).
 * @param out  Výstup; při neúspěchu se nemění.
 * @return true při známém názvu.
 */
bool videorec_rt_pause_from_name(const char *name, en_VIDEOREC_RT_PAUSE *out);
/**
 * @brief Název chování při rychlosti != 100 % ("as_seen", "emulated_when_fast").
 * @param v Hodnota.
 * @return Statický řetězec; neplatná hodnota = "as_seen".
 */
const char *videorec_rt_speed_name(en_VIDEOREC_RT_SPEED v);
/**
 * @brief Převede název na chování při rychlosti != 100 %.
 * @param name Název (nesmí být NULL).
 * @param out  Výstup; při neúspěchu se nemění.
 * @return true při známém názvu.
 */
bool videorec_rt_speed_from_name(const char *name, en_VIDEOREC_RT_SPEED *out);
/**
 * @brief Název zvuku při turbu ("as_heard", "silence", "attenuate").
 * @param v Hodnota.
 * @return Statický řetězec; neplatná hodnota = "as_heard".
 */
const char *videorec_turbo_audio_name(en_VIDEOREC_TURBO_AUDIO v);
/**
 * @brief Převede název na zvuk při turbu.
 * @param name Název (nesmí být NULL).
 * @param out  Výstup; při neúspěchu se nemění.
 * @return true při známém názvu.
 */
bool videorec_turbo_audio_from_name(const char *name, en_VIDEOREC_TURBO_AUDIO *out);
/**
 * @brief Název zápisu událostí stavu ("none", "sidecar").
 * @param v Hodnota.
 * @return Statický řetězec; neplatná hodnota = "sidecar".
 */
const char *videorec_state_marks_name(en_VIDEOREC_STATE_MARKS v);
/**
 * @brief Převede název na zápis událostí stavu.
 * @param name Název (nesmí být NULL).
 * @param out  Výstup; při neúspěchu se nemění.
 * @return true při známém názvu.
 */
bool videorec_state_marks_from_name(const char *name, en_VIDEOREC_STATE_MARKS *out);

/**
 * @brief Globální nastavení video záznamu (plní INI modul, čte se při startu nahrávání).
 *
 * Změny se projeví až u dalšího startu nahrávání. Zapisuje INI modul
 * (videorec_config_init()) a UI vlákno (OK v dialogu nastavení) - zápis
 * **není chráněn žádným zámkem**. Čte jen videorec_request_start(), která si
 * potřebné hodnoty zkopíruje (pod zámkem modulu, ten ale chrání jen stav
 * lepidla, ne zápis do této struktury). Bezpečné je to proto, že zápis
 * i čtení běží typicky ve stejném (UI) vlákně; jediný teoretický souběh je
 * CLI `--record` (videorec_request_start() v emu vlákně při bootu) proti
 * současnému potvrzení dialogu nastavení - prakticky nedosažitelné.
 *
 * Výjimka: `timebase` mění i videorec_request_timebase() (přepnutí za běhu
 * platí pro běžící session i pro příští start).
 *
 * Invarianty: `audio_rate` je 44100 nebo 48000 (jiná hodnota se při startu
 * nahradí 48000); `transition_ms` a `keyframe_interval` libovolné;
 * `realtime_pause_cap_s` v rozsahu VIDEOREC_RT_PAUSE_CAP_MIN_S ..
 * VIDEOREC_RT_PAUSE_CAP_MAX_S (jiná hodnota se při startu omezí).
 */
typedef struct st_VIDEOREC_SETTINGS {
    char output_dir[1024];                     /**< Adresář výstupu; prázdné = `<home_dir emulátoru>/videos` (viz videorec_resolve_output_dir()). */
    unsigned audio_rate;                       /**< Vzorkovací frekvence zvuku [Hz]: 44100 nebo 48000. */
    en_VIDEOREC_RETAKE retake_mode;            /**< Chování při nahrání snapshotu během nahrávání. */
    en_VIDEOREC_TRANSITION default_transition; /**< Výchozí přechod na hranici segmentu (pro sidecar). */
    unsigned transition_ms;                    /**< Délka přechodu [ms] (pro sidecar). */
    unsigned keyframe_interval;                /**< Max. počet delta snímků mezi klíčovými snímky ZMBV. */
    en_VIDEOREC_TIMEBASE timebase;             /**< Časová základna (INI `timebase`); za běhu viz videorec_request_timebase(). */
    en_VIDEOREC_RT_PAUSE realtime_pause;       /**< Pauza emulace v režimu podle reality. */
    unsigned realtime_pause_cap_s;             /**< Limit zamrzlé pauzy pro `freeze_capped` [s]. */
    en_VIDEOREC_RT_SPEED realtime_speed;       /**< Rychlost != 100 % v režimu podle reality. */
    en_VIDEOREC_TURBO_AUDIO realtime_turbo_audio; /**< Zvuk při rychlosti > 100 % v režimu podle reality. */
    en_VIDEOREC_STATE_MARKS state_marks;       /**< Zápis událostí stavu do sidecaru. */
    bool auto_markers;                         /**< Automatické markery (pauza, rychlost, snapshot, reset). */
    bool record_debugger_steps;                /**< Režim podle reality: zapisovat snímky při krokování debuggeru. */
} st_VIDEOREC_SETTINGS;

/** @brief Globální nastavení video záznamu (výchozí hodnoty viz videorec.c). */
extern st_VIDEOREC_SETTINGS g_videorec_settings;

/**
 * @brief Parametry požadavku na start nahrávání.
 *
 * Invarianty: `path` je řetězec ukončený nulou. Řádky `level` nad počet
 * zvukových kanálů platformy se ignorují.
 */
typedef struct st_VIDEOREC_START {
    char path[1024];                                                 /**< Cílový .avi soubor; prázdné = vygenerované jméno (viz videorec_request_start()). */
    float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS]; /**< Úroveň kanálu pro hodnotu 0..15 (viz iface_audio_build_videorec_levels()). */
    uint64_t stop_after_frames;                                      /**< Po kolika zapsaných snímcích nahrávání ukončit (0 = bez limitu). */
    bool quit_after_stop;                                            /**< Po automatickém ukončení (stop_after_frames) ukončit emulátor. */
} st_VIDEOREC_START;

/** @brief Stav nahrávání. */
typedef enum {
    VIDEOREC_STATE_IDLE = 0,  /**< Nenahrává se. */
    VIDEOREC_STATE_RECORDING, /**< Nahrává se (každý emulovaný snímek se zapíše). */
    VIDEOREC_STATE_PAUSED     /**< Record-pause: emulace běží, snímky se nezapisují. */
} en_VIDEOREC_STATE;

/** @brief Bit v g_videorec_active: nahrávání aktivní (stav RECORDING nebo PAUSED). */
#define VIDEOREC_FLAG_ACTIVE 0x01
/** @brief Bit v g_videorec_active: čeká požadavek (start, stop, pauza, marker, snapshot) ke zpracování na konci snímku. */
#define VIDEOREC_FLAG_REQ 0x02
/**
 * @brief Bit v g_videorec_active: efektivní časová základna je realtime.
 *
 * Snímky zapisuje vlákno vzorkovače; hook konce snímku i tap emulačního
 * zvuku jsou v ustáleném stavu nečinné (viz videorec_wants_emu_audio()).
 */
#define VIDEOREC_FLAG_RT 0x04

/**
 * @brief Atomické slovo bitových příznaků VIDEOREC_FLAG_* (ne prostý bool).
 *
 * Obě informace jsou v jednom slově, aby nečinný hook konce snímku stál
 * přesně jedno atomické čtení (nula = nic nedělat). Bity mění jen modul
 * (atomické OR/AND); čte se bez zámku. Mimo modul se má číst jen přes
 * videorec_is_active().
 */
extern volatile gint g_videorec_active;

/**
 * @brief Rychlý test, zda probíhá nahrávání (pro hot path, 1 atomické čtení).
 * @return true pokud je nastaven bit VIDEOREC_FLAG_ACTIVE (stav RECORDING nebo PAUSED).
 * @note Bezpečné z libovolného vlákna; výsledek může být okamžitě zastaralý.
 */
static inline bool videorec_is_active(void)
{
    return (g_atomic_int_get(&g_videorec_active) & VIDEOREC_FLAG_ACTIVE) != 0;
}

/**
 * @brief Rychlý test pro tap emulačního zvuku: nahrává se a v emulačním čase (1 atomické čtení).
 *
 * V režimu podle reality se zvuk bere z výstupu SDL cesty, události kanálů
 * se proto do emulačního rendereru nepouštějí. Souběh s přepnutím časové
 * základny řeší kontrola pod zámkem ve videorec_audio_tap() /
 * videorec_audio_horizon().
 *
 * @return true pokud je nastaven bit VIDEOREC_FLAG_ACTIVE a není nastaven VIDEOREC_FLAG_RT.
 * @note Bezpečné z libovolného vlákna; výsledek může být okamžitě zastaralý.
 */
static inline bool videorec_wants_emu_audio(void)
{
    return (g_atomic_int_get(&g_videorec_active) & (VIDEOREC_FLAG_ACTIVE | VIDEOREC_FLAG_RT)) == VIDEOREC_FLAG_ACTIVE;
}

/**
 * @brief Tap zobrazeného snímku pro režim podle reality (volá iface_video_framebuffer_screen_done()).
 *
 * Zkopíruje snímek do slotu "poslední zobrazený snímek" (latest wins), ze
 * kterého si vzorkovač bere obraz. Bez nahrávání stojí jedno atomické čtení.
 * Aktivní je po celou session (i v emulačním čase), aby při přepnutí na
 * realtime byl k dispozici aktuální obraz; počet kopií je omezený počtem
 * zobrazených snímků (nejvýš cca 50 / 60 za sekundu i při MAX SPEED).
 *
 * @param pixels Framebuffer VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT (indexy barev).
 * @pre Volá jen emu vlákno (framebuffer patří emu vláknu).
 */
void videorec_rt_video_tap(const uint8_t *pixels);

/** @copydoc videorec_rt_video_tap */
static inline void videorec_on_displayed_frame(const uint8_t *pixels)
{
    if (g_atomic_int_get(&g_videorec_active) & VIDEOREC_FLAG_ACTIVE) videorec_rt_video_tap(pixels);
}

/**
 * @brief Inicializuje modul a spustí writer vlákno a vlákno vzorkovače ("videorec-rt").
 *
 * Při prvním volání inicializuje i jitter buffer zvuku režimu podle reality
 * (ten se pak nikdy neuvolňuje - producent v SDL callbacku může běžet kdykoli).
 *
 * Volá emu vlákno při inicializaci emulátoru (emulator_thread(), vedle
 * snapshot_init()).
 *
 * @post Modul přijímá požadavky; stav IDLE.
 * @note Opakované volání bez videorec_exit() je no-op.
 */
void videorec_init(void);

/**
 * @brief Synchronně ukončí případné nahrávání, vlákno vzorkovače a writer vlákno.
 *
 * Probíhá-li nahrávání, vyprázdní párování (pipe_flush), uzavře segment, zapíše
 * sidecar a finalizuje AVI. Čekající nezpracovaný start se zruší (otevřený
 * soubor se zavře a smaže). Pak počká na dokončení všech zápisů a ukončí writer
 * vlákno (join).
 *
 * Volá emulator_quit() v emu vlákně (před snapshot_exit()).
 *
 * @post Stav IDLE, writer vlákno neběží. Bezpečné volat i bez videorec_init()
 *       a opakovaně.
 */
void videorec_exit(void);

/**
 * @brief Požádá o start nahrávání.
 *
 * Funkce hned vytvoří cílový AVI soubor (aby chyba typu neexistující adresář
 * byla hlášena synchronně) a zaznamená požadavek. Vlastní začátek nahrávání
 * nastane na nejbližším konci emulovaného snímku: ten snímek se ještě
 * nezapisuje, první zapsaný snímek je až ten následující.
 *
 * Cesta: je-li `s->path` neprázdná, použije se přesně a existující soubor se
 * přepíše (platí i pro CLI `--record`). Prázdná cesta = vygenerované jméno
 * `<adresář>/<platforma>_%Y%m%d_%H%M%S.avi` (mz800_, mz700_, mz1500_), kde adresář určí
 * videorec_resolve_output_dir(); pokud už existuje ono nebo jeho
 * `.cuts.json`, přidá se přípona `_2`, `_3`, ... (vygenerované jméno nikdy
 * nepřepíše existující nahrávku).
 *
 * @param s Parametry (zkopírují se; NULL = chyba).
 * @return true pokud byl požadavek přijat; false při chybě - text chyby
 *         (anglicky, UI ho překládá přes `_()`) je pak ve videorec_get_last_error().
 *         Chyby: parametry platformy nedávají celý počet vzorků zvuku na snímek
 *         ("Video recording is not possible on this platform: ...", viz
 *         videorec_platform_check(); u podporovaných frekvencí 44100 a 48000 Hz
 *         nenastává), modul neinicializován, nahrávání už běží nebo start už
 *         čeká, soubor nelze vytvořit.
 *
 * @par Vlákna
 * Libovolné vlákno kromě volání z hooků modulu (UI vlákno, CLI start v emu
 * vlákně před hlavní smyčkou). Bere zámek modulu.
 *
 * @par Side effects
 * Vytvoří (přepíše) soubor `s->path` nebo vygenerovaný soubor; vypíše hlášku
 * na stderr.
 */
bool videorec_request_start(const st_VIDEOREC_START *s);

/**
 * @brief Požádá o ukončení nahrávání (provede se na nejbližším konci snímku).
 *
 * Čeká-li ještě nezpracovaný start, zruší ho (soubor se zavře a smaže).
 * Bez nahrávání no-op.
 *
 * @par Vlákna
 * Libovolné vlákno mimo hooky modulu. Bere zámek modulu.
 */
void videorec_request_stop(void);

/**
 * @brief Požádá o přepnutí record-pause (RECORDING <-> PAUSED) na nejbližším konci snímku.
 *
 * Pauza uzavře aktuální segment, obnovení otevře nový segment s výchozím
 * přechodem. Dvě volání před nejbližším koncem snímku se navzájem zruší.
 * Bez nahrávání no-op.
 *
 * @par Vlákna
 * Libovolné vlákno mimo hooky modulu. Bere zámek modulu.
 */
void videorec_request_pause_toggle(void);

/**
 * @brief Požádá o explicitní nastavení record-pause (na nejbližším konci snímku).
 *
 * Na rozdíl od videorec_request_pause_toggle() je idempotentní: čekající
 * požadavek na přepnutí se nastaví tak, aby po nejbližším konci snímku byl
 * stav PAUSED (`paused` = true) nebo RECORDING (`paused` = false), bez ohledu
 * na to, kolikrát se volala tato funkce nebo toggle před tímto koncem snímku.
 * Je-li cílový stav už aktuální, čekající přepnutí se zruší (no-op).
 * Pauza uzavře segment, obnovení otevře nový (stejně jako toggle).
 *
 * @param paused Cílový stav: true = record-pause, false = nahrávat.
 * @return true pokud nahrávání běží (stav RECORDING nebo PAUSED) a požadavek
 *         byl přijat; false bez nahrávání (i když čeká nezpracovaný start) -
 *         pak se nic nemění.
 *
 * @par Vlákna
 * Libovolné vlákno mimo hooky modulu. Bere zámek modulu.
 */
bool videorec_request_pause_set(bool paused);

/**
 * @brief Požádá o vložení markeru na aktuální pozici nahrávky.
 *
 * Marker dostane index snímku, který se zapíše jako další (zpracuje se na
 * nejbližším konci snímku). Bez nahrávání no-op.
 *
 * @param label Popisek (zkopíruje se; NULL = prázdný).
 *
 * @par Vlákna
 * Libovolné vlákno mimo hooky modulu. Bere zámek modulu.
 */
void videorec_request_marker(const char *label);

/**
 * @brief Vrátí stav nahrávání.
 * @return Stav.
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
en_VIDEOREC_STATE videorec_get_state(void);

/**
 * @brief Vrátí počet snímků nahrávky (zapsané + čekající na zvuk), 0 bez nahrávání.
 * @return Počet snímků aktuální nahrávky (po posledním konci snímku).
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
uint64_t videorec_get_frames(void);

/**
 * @brief Vrátí text poslední chyby (anglicky; prázdný řetězec = žádná chyba).
 *
 * Text se zkopíruje pod interním zámkem chybových textů do bufferu vlastního
 * volajícímu vláknu (thread-local), takže je konzistentní i při souběžném
 * zápisu z jiného vlákna.
 *
 * @return Ukazatel na thread-local kopii; platí do dalšího volání této funkce
 *         ze stejného vlákna (a do konce vlákna). Hodnota chyby se mění při
 *         startu nahrávání (vynuluje se nebo nastaví chyba startu) a když
 *         nahrávání skončí chybou zápisu.
 * @par Vlákna Libovolné vlákno.
 */
const char *videorec_get_last_error(void);

/** @brief Druh události nahrávání (viz st_VIDEOREC_EVENT). */
typedef enum {
    VIDEOREC_EVENT_NONE = 0, /**< Žádná událost (neplatný záznam). */
    VIDEOREC_EVENT_STARTED,  /**< Nahrávání začalo; `path` = cílový .avi, `frame` = 0. */
    VIDEOREC_EVENT_SAVED,    /**< Writer dokončil zápis nahrávky; `path` = první part, `frame` = počet snímků. */
    VIDEOREC_EVENT_FAILED,   /**< Nahrávka skončila chybou (i po stopu při dobíhání writeru); `path` = první part, `frame` = počet snímků, `text` = anglický popis chyby. */
    VIDEOREC_EVENT_RETAKE,   /**< Retake zahodil snímky; `frame` = index bodu snapshotu (nahrávka pokračuje od něj). */
    VIDEOREC_EVENT_SEAM      /**< V místě nahrání snapshotu vznikl šev; `frame` = index snímku švu. */
} en_VIDEOREC_EVENT;

/** @brief Počet posledních událostí, které si modul pamatuje (kruhový buffer). */
#define VIDEOREC_EVENT_RING 16

/**
 * @brief Strukturovaná událost nahrávání pro UI.
 *
 * Jádro je bez i18n: UI skládá přeložený text z druhu události a hodnot
 * (`frame` převede na čas, `path` dosadí do šablony). `text` je anglický
 * popis: u FAILED text chyby, jinak stejný text jako videorec_get_last_event().
 *
 * Invarianty: `seq != 0` pro platnou událost; řetězce jsou ukončené nulou.
 */
typedef struct st_VIDEOREC_EVENT {
    uint32_t seq;           /**< Pořadové číslo události (viz videorec_get_event_seq()). */
    en_VIDEOREC_EVENT kind; /**< Druh události. */
    uint64_t frame;         /**< Index / počet snímků nahrávky (význam podle druhu). */
    char path[1024];        /**< Cesta k nahrávce (STARTED, SAVED, FAILED), jinak prázdné. */
    char text[256];         /**< Anglický text události nebo chyby. */
} st_VIDEOREC_EVENT;

/**
 * @brief Vrátí text poslední události nahrávání (anglicky; prázdný řetězec = zatím žádná).
 *
 * Texty podle druhu události (viz en_VIDEOREC_EVENT):
 * - `"Recording started: <cesta>"`, `"Recording saved: <cesta>"`,
 *   `"Recording failed: <chyba>"`;
 * - `"Retake: rewound to HH:MM:SS"` - retake zahodil snímky; čas je pozice
 *   bodu snapshotu v nahrávce (index snímku / fps platformy, zaokrouhleno dolů);
 * - `"Recording seam (snapshot loaded)"` - v místě nahrání vznikl šev.
 *
 * Start, retake a šev vznikají na konci snímku v emu vlákně (retake a šev
 * tedy se zpožděním nejvýš jednoho emulovaného snímku po nahrání snapshotu);
 * uložení a selhání vydává writer vlákno po dokončení zápisu nahrávky.
 *
 * Text obsahuje proměnné části, není tedy gettext klíčem - UI má používat
 * strukturovanou videorec_get_event() a překládat šablonu.
 *
 * Text se zkopíruje pod listovým zámkem chybových textů do bufferu vlastního
 * volajícímu vláknu (thread-local), takže je konzistentní i při souběžném
 * zápisu z emu nebo writer vlákna.
 *
 * @return Ukazatel na thread-local kopii; platí do dalšího volání této funkce
 *         ze stejného vlákna (a do konce vlákna). Text se mezi sessions
 *         nemaže (zůstává poslední událost).
 * @par Vlákna Libovolné vlákno.
 */
const char *videorec_get_last_event(void);

/**
 * @brief Vrátí pořadové číslo poslední události (viz videorec_get_event()).
 *
 * Číslo se zvýší po každé nové události (start, uložení, selhání, retake,
 * šev), a to až po zápisu jejích dat. UI si pamatuje naposledy zpracované
 * číslo a při změně si vyzvedne všechny novější události přes
 * videorec_get_event(). Přetečení (2^32 událostí) je pro porovnání na
 * rovnost neškodné.
 *
 * @return Pořadové číslo (0 = od startu programu žádná událost).
 * @par Vlákna Libovolné vlákno (jedno atomické čtení, bez zámku).
 */
uint32_t videorec_get_event_seq(void);

/**
 * @brief Vyzvedne událost s daným pořadovým číslem.
 *
 * Modul drží posledních VIDEOREC_EVENT_RING událostí; starší jsou přepsané.
 *
 * @param seq Pořadové číslo (1 .. videorec_get_event_seq()).
 * @param out Výstup (nesmí být NULL); při neúspěchu se nemění.
 * @return true pokud je událost `seq` k dispozici; false pro 0, budoucí
 *         nebo už přepsané číslo.
 * @par Vlákna Libovolné vlákno; bere listový zámek chybových textů.
 */
bool videorec_get_event(uint32_t seq, st_VIDEOREC_EVENT *out);

/**
 * @brief Určí (a případně vytvoří) adresář pro vygenerovaná jména nahrávek.
 *
 * - `g_videorec_settings.output_dir` neprázdný: použije se; chybí-li, pokusí
 *   se ho vytvořit (g_mkdir_with_parents);
 * - prázdný: `<home_dir emulátoru>/videos` (`g_sdlapp->paths->home_dir`),
 *   vytvoří se; když vytvoření selže, adresář Videa uživatele
 *   (g_get_user_special_dir), a když ani ten neexistuje, aktuální adresář ".".
 *
 * @param out  Výstupní buffer (nesmí být NULL).
 * @param size Velikost bufferu (> 0); delší cesta se zkrátí.
 * @return true pokud výsledný adresář existuje; false pokud nastavený
 *         adresář nelze vytvořit (cesta je v `out` přesto vyplněna - start
 *         pak selže s chybou "Cannot create video file").
 * @par Vlákna UI vlákno nebo videorec_request_start(); g_videorec_settings
 *      čte bez zámku (zapisuje ho jen UI vlákno).
 * @par Side effects Může vytvořit adresář.
 */
bool videorec_resolve_output_dir(char *out, size_t size);

/**
 * @brief Je video záznam na této platformě podporován?
 * @return Vždy true (MZ-700 PAL/NTSC, MZ-800, MZ-1500); funkce zůstává kvůli
 *         MCP (`supported` ve stavu nahrávání), které ji dotazuje; UI ji nepoužívá.
 */
bool videorec_is_supported(void);

/**
 * @brief Snímková frekvence nahrávky na této platformě [snímky/s].
 *
 * 50 na MZ-800 a MZ-700 PAL, 60 na MZ-1500 a MZ-700 NTSC (fps_num / fps_den
 * parametrů platformy, celočíselně). UI podle ní převádí index snímku na čas.
 *
 * @return Snímků za sekundu, >= 1.
 * @par Vlákna Libovolné vlákno; čte jen konstantní parametry platformy.
 */
unsigned videorec_get_fps(void);

/**
 * @brief Hook konce emulovaného snímku (emu vlákno, každý snímek).
 *
 * Když se nenahrává a nečeká žádný požadavek (g_videorec_active == 0),
 * vrací po jednom atomickém čtení. Jinak pod zámkem zpracuje požadavky (start, marker, snapshot,
 * pauza, stop), předá framebuffer párování (při pauze jen spotřebuje zvuk),
 * při nesouvislosti času (reset, nahrání snapshotu) udělá šev (rebase + nový
 * segment), hlídá `stop_after_frames` a chybu writeru. Po uvolnění zámku
 * případně čeká na writer (backpressure) a při `quit_after_stop` ukončí
 * emulátor přes emulator_quit() (longjmp - funkce se pak nevrátí).
 *
 * @pre Volá jen emu vlákno z mz800_main_event_callback_screen_done(), kdy
 *      g_framebuffer.pixels obsahuje kompletní snímek.
 */
void videorec_on_screen_done(void);

/**
 * @brief Hook vstupu do paused smyčky: emulace stojí, běh k dočasnému BP skončil (emu vlákno).
 *
 * Zruší příznak běhu k dočasnému breakpointu (videorec_on_debugger_run()) -
 * i bez nahrávání, aby nepřežil do další session. Byl-li příznak nastavený
 * a nahrává se, aktuální pauza se označí jako zastavení na dočasném
 * breakpointu (krokování debuggeru). Zrušení i označení proběhnou atomicky
 * vůči ticku vzorkovače (pod zámkem modulu).
 *
 * Musí se volat při vstupu do paused smyčky, **před** prvním zpracováním
 * fronty dbgapi: pokračování (MCP `run`, další `step_over`) zpracované už
 * v první iteraci smyčky zruší pauzu, takže pozdější hook
 * videorec_on_emulation_paused() by emulaci viděl běžet a příznak by
 * přežil do následujícího volného běhu (v realtime bez
 * `record_debugger_steps` by se ten pak nezapisoval). Nový běh k dočasnému
 * breakpointu spuštěný z té fronty příznak nastaví znovu až po tomto hooku.
 *
 * Bez nahrávání stojí jedno atomické čtení a jedno compare-and-exchange.
 *
 * @pre Volá jen emu vlákno na začátku mzzarch_main_do_emulator_paused()
 *      (g_emulator.paused == true), bez držení zámku modulu.
 * @post Příznak běhu k dočasnému breakpointu je zrušený.
 */
void videorec_on_emulation_stopped(void);

/**
 * @brief Hook paused smyčky emulace: začátek pauzy a čekající stop (emu vlákno).
 *
 * V emulačním čase (i během čekání na návrat z realtime) zaznamená začátek
 * pauzy na přesné pozici. Příznak běhu k dočasnému breakpointu neruší - to
 * dělá videorec_on_emulation_stopped() při vstupu do smyčky (tento hook
 * běží až po zpracování fronty dbgapi, kdy emulace už může znovu běžet).
 *
 * V pauze emulace nevznikají snímky, takže požadavky čekající na konec
 * snímku by se zpracovaly až po odpauzování. Stop se provede hned: čekající
 * snímky se dopíšou (stejně jako při stopu na konci snímku), sidecar a AVI
 * se finalizují a writer pak vydá událost SAVED nebo FAILED. Ostatní
 * požadavky (start, record-pause, marker, snapshot) dál čekají na nejbližší
 * konec snímku - jsou vázané na pozici v nahrávce.
 *
 * Bez nahrávání a bez požadavku stojí jedno atomické čtení.
 *
 * @pre Volá jen emu vlákno z paused smyčky (mzzarch_main_do_emulator_paused()),
 *      bez držení zámku modulu.
 * @post Byl-li požadován stop, stav je IDLE.
 */
void videorec_on_emulation_paused(void);

/**
 * @brief Tap živé změny hodnoty zvukového kanálu (emu vlákno).
 *
 * @param src_id Index kanálu (audio_source_id(): 0 = CTC0, 1..4 = PSG0, 5..8 = PSG1).
 * @param value  Nová hodnota 0..15.
 * @param ticks  Absolutní čas změny v GDG taktech.
 *
 * Ochrana švu: událost s časem menším než předchozí událost téhož kanálu
 * znamená skok času zpět (nahraný snapshot, který frame hook ještě nezachytil).
 * Pak se tato i další události nepouštějí do pipe a horizonty se ignorují až
 * do švu na nejbližším konci snímku (vynucený šev i při navazujícím taktu
 * konce snímku). Události se mezitím podrží (nejvýš 262144) a při retake
 * s dostupným stavem zvuku se přehrají do nové časové osy; při švu se zahodí.
 * Kontrola proti renderované pozici pipe se záměrně nepoužívá: PSG události
 * se doplňují líně s časem v minulosti (až perioda synchronizace krát
 * rychlost; perioda 20 ms při 50, 16,7 ms při 60 snímcích/s), takže by dávala
 * falešné švy. Skok času dopředu tap nepozná - pokrývá ho
 * videorec_on_snapshot_loaded() (hold od okamžiku nahrání) a frame hook.
 *
 * V režimu podle reality (VIDEOREC_FLAG_RT) se událost ignoruje.
 *
 * @pre Volá audio_changed() jen pro živý log (`log == g_audio.log`) a jen
 *      když videorec_wants_emu_audio().
 */
void videorec_audio_tap(unsigned src_id, uint8_t value, uint64_t ticks);

/**
 * @brief Oznámí horizont doručení zvukových událostí (emu vlákno).
 *
 * Všechny události s časem < `ticks` už byly předány přes videorec_audio_tap().
 * Hotové snímky do horizontu se vyrenderují a předají writeru; poté může emu
 * vlákno čekat (backpressure).
 *
 * Během ochrany švu (viz videorec_audio_tap()) se horizont ignoruje.
 *
 * @param ticks Horizont v GDG taktech (`g_audio.log->last_psg_timestamp` po audio_log_fill_psg()).
 * V režimu podle reality (VIDEOREC_FLAG_RT) se horizont ignoruje.
 *
 * @pre Volá audiolog_finish_20ms_frame() jen když videorec_wants_emu_audio().
 */
void videorec_audio_horizon(uint64_t ticks);

/**
 * @brief Vrátí identifikátor aktuální session nahrávání (0 = nenahrává se).
 * @return Náhodné nenulové 64bitové ID přidělené při startu, nebo 0.
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
uint64_t videorec_get_session_id(void);

/**
 * @brief Zda čeká přijatý, ale ještě nezpracovaný start nahrávání.
 *
 * Start z videorec_request_start() se provede až na nejbližším konci snímku;
 * do té doby videorec_get_state() vrací VIDEOREC_STATE_IDLE. UI tím pozná,
 * že čekající start mezitím zanikl (zpracoval se a nahrávání už skončilo,
 * nebo ho zrušil jiný volající přes videorec_request_stop()).
 *
 * @return true pokud start čeká na zpracování.
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
bool videorec_is_start_pending(void);

/** @brief Co režim podle reality právě dělá (pro REC indikátor UI). */
typedef enum {
    VIDEOREC_RT_ACTIVITY_OFF = 0,  /**< Efektivní časová základna je emulační čas (nebo se nenahrává). */
    VIDEOREC_RT_ACTIVITY_LIVE,     /**< Realtime: zapisuje se živý obraz a zvuk. */
    VIDEOREC_RT_ACTIVITY_FROZEN,   /**< Realtime: pauza emulace, zapisuje se zamrzlý obraz (freeze / krokování). */
    VIDEOREC_RT_ACTIVITY_SKIPPING  /**< Realtime: pauza emulace nebo record-pause, nezapisuje se. */
} en_VIDEOREC_RT_ACTIVITY;

/**
 * @brief Souhrnný stav nahrávání pro UI (okno dálkového ovládání), viz videorec_get_status().
 *
 * Skládá se ze dvou nezávisle zveřejňovaných částí:
 * - **stav lepidla** (emu vlákno, resp. volající `videorec_request_*()`):
 *   `state`, `start_pending`, `frames`, `segment`, `segment_open`,
 *   `retake_mode`, `path`; aktualizuje se na konci každého zpracovaného
 *   snímku a po každém požadavku, který stav mění;
 * - **stav writeru** (writer vlákno): `bytes`, `parts`; aktualizuje se po
 *   každém zapsaném snímku, truncate, rolloveru a uzavření nahrávky. Writer
 *   je za lepidlem pozadu (fronta až stovky snímků), velikost proto
 *   odpovídá snímkům, které už writer skutečně zapsal.
 *
 * Invarianty: při `state == VIDEOREC_STATE_IDLE` jsou `frames`, `segment`
 * a `segment_open` nulové, `timebase_effective == EMULATED` a
 * `rt_activity == OFF`; `segment_open` => `segment >= 1`;
 * `start_pending` => `state == VIDEOREC_STATE_IDLE`;
 * `timebase_effective == REALTIME` => `timebase == REALTIME`.
 */
typedef struct st_VIDEOREC_STATUS {
    en_VIDEOREC_STATE state;        /**< Stav nahrávání (stejný jako videorec_get_state()). */
    bool start_pending;             /**< Start byl přijat a čeká na nejbližší konec snímku (videorec_is_start_pending()). */
    uint64_t frames;                /**< Počet snímků nahrávky (stejný jako videorec_get_frames()); 0 bez nahrávání. */
    unsigned segment;               /**< Počet segmentů sidecaru = pořadové číslo aktuálního (při pauze posledního) segmentu, od 1; 0 bez nahrávání. */
    bool segment_open;              /**< Segment je otevřený (nahrává se); false během record-pause. */
    en_VIDEOREC_RETAKE retake_mode; /**< Režim retake běžící session (kopie z okamžiku startu; bez nahrávání nevýznamné). */
    char path[1024];                /**< Cesta prvního AVI partu běžící, čekající (přijatý start) nebo poslední session (prázdné = zatím žádná). */
    uint64_t bytes;                 /**< Bajty všech AVI partů běžící nebo poslední session zapsané writerem: uzavřené party
                                         velikostí na disku, otevřený part bez indexu (avi_writer_bytes()). Po dokončení
                                         zápisu nahrávky = součet velikostí partů na disku. 0 dokud writer session neotevřel. */
    unsigned parts;                 /**< Počet AVI partů běžící nebo poslední session (0 dokud writer session neotevřel). */
    en_VIDEOREC_TIMEBASE timebase;  /**< Požadovaná časová základna běžící / čekající session; bez nahrávání nastavení pro příští start. */
    en_VIDEOREC_TIMEBASE timebase_effective; /**< Skutečná časová základna (realtime + `emulated_when_fast` při turbu = emulated); bez nahrávání EMULATED. */
    en_VIDEOREC_RT_ACTIVITY rt_activity;     /**< Činnost režimu podle reality (stav posledního ticku vzorkovače). */
} st_VIDEOREC_STATUS;

/**
 * @brief Vyplní souhrnný stav nahrávání bez čekání na emu nebo writer vlákno.
 *
 * Na rozdíl od videorec_get_state() a spol. nebere zámek lepidla (ten může emu
 * vlákno držet dlouho, např. během bariéry s writerem před retake), ale jen
 * listový zámek zveřejněného stavu, pod kterým se pouze kopírují data. UI
 * vlákno ji proto smí volat každý snímek, aniž by ho emulace mohla zablokovat.
 * Hodnoty jsou kopie z posledního zveřejnění - mohou být o jeden emulovaný
 * snímek (stav lepidla) nebo o frontu writeru (velikost) pozadu.
 *
 * Údaje writeru (`bytes`, `parts`) se vrací jen pro session, kterou lepidlo
 * zveřejnilo jako aktuální (párování podle generace session); dokud writer
 * nově spuštěnou session neotevře, jsou nulové.
 *
 * @param out Výstup (nesmí být NULL); vždy se celý vyplní.
 * @par Vlákna Libovolné vlákno; nikdy nečeká na zámek lepidla.
 */
void videorec_get_status(st_VIDEOREC_STATUS *out);

/**
 * @brief Přepne časovou základnu nahrávání (za běhu i pro příští start).
 *
 * Nastaví `g_videorec_settings.timebase` a požadovanou časovou základnu
 * běžící nebo čekající session. Přepnutí provede vlákno vzorkovače na
 * nejbližším ticku (nejvýš jedna perioda snímku, 20 ms při 50 a 16,7 ms
 * při 60 snímcích/s): emulated -> realtime hned (i v pauze
 * emulace), realtime -> emulated na nejbližším konci emulovaného snímku
 * (emulační osa musí začít na hranici snímku; v pauze emulace se do té doby
 * nezapisuje). Přepnutí je hranice segmentu s výchozím přechodem, začíná
 * nová větev a do sidecaru se zapíše událost `timebase`. Návrat do
 * emulačního času požádaný v pauze emulace čeká na první konec snímku po
 * pokračování; do té doby vzorkovač nezapisuje (ani zamrzlý obraz podle
 * `realtime_pause`), status hlásí `timebase_effective = EMULATED`
 * a požadavky (marker, record-pause) se zpracují až na tom konci snímku
 * (stop hned, v paused smyčce). Při
 * `realtime_speed = emulated_when_fast` a rychlosti != 100 % zůstane
 * efektivní základna emulační, dokud se rychlost nevrátí na 100 %.
 *
 * @param tb Požadovaná časová základna.
 * @return true pokud se nahrává nebo čeká start (session požadavek převzala);
 *         false bez nahrávání (změnilo se jen nastavení pro příští start).
 * @par Vlákna Libovolné vlákno mimo hooky modulu (UI, MCP/dbgapi). Bere
 *      zámek modulu; zápis do g_videorec_settings bez zámku (viz
 *      st_VIDEOREC_SETTINGS).
 */
bool videorec_request_timebase(en_VIDEOREC_TIMEBASE tb);

/**
 * @brief Tap výstupu SDL cesty pro režim podle reality (F32 stereo 44,1 kHz).
 *
 * Volá iface_audio_wait_for_data() těsně před návratem vyrobeného bloku
 * (SDL audio callback, v headless emu vlákno). Mimo režim podle reality
 * stojí jedno čtení příznaku bez zámku; jinak kopie bloku do jitter bufferu
 * pod jeho krátkým zámkem.
 *
 * @param stereo Prokládané L,R vzorky (`frames * 2` floatů).
 * @param frames Počet stereo vzorků (blok jednoho snímku: 882 při 50, 735 při 60 fps).
 * @par Vlákna Libovolné vlákno; nebere zámek modulu.
 */
void videorec_rt_audio_output(const float *stereo, size_t frames);

/**
 * @brief Bez audio zařízení (headless, GUI po selhání zařízení): potřebuje
 *        režim podle reality další blok zvuku?
 *
 * Bez audio zařízení (sync_by_timer) volá iface_audio_20ms_sync() na každé
 * synchronizační události emulace (jednou za periodu snímku); pokud funkce vrátí true, vyrobí blok stejnou SDL cestou
 * (iface_audio_wait_for_data()) jen pro nahrávku. Při MAX SPEED tak vzniká
 * jen tolik bloků, kolik vzorkovač spotřebuje (jako u skutečného zařízení).
 *
 * @return true pokud je zápis zvuku povolen a jitter buffer je pod cílem + 1 blok.
 * @par Vlákna Libovolné vlákno; nebere zámek modulu.
 */
bool videorec_rt_audio_wants_output(void);

/**
 * @brief Oznámí dokončený krok debuggeru (pro rozpoznání krokování od pauzy).
 *
 * Volá paused smyčka emulace (mzzarch_main_do_emulator_paused()) po
 * návratu z kroku (`g_debugger.step_call` nastavené). Jen atomické
 * zvýšení čítače; bez nahrávání neškodné.
 *
 * @par Vlákna Emu vlákno; nebere zámek modulu.
 */
void videorec_on_debugger_step(void);

/**
 * @brief Oznámí spuštění běhu k dočasnému breakpointu (step over, run to cursor).
 *
 * Volá mzarch_run_to_temporary_breakpoint() před zrušením pauzy. Nastaví
 * příznak "běh k dočasnému breakpointu" (atomicky); ten zruší vstup do
 * paused smyčky (videorec_on_emulation_stopped()), jakmile emulace znovu
 * stojí. Ruční pauza během dlouhého běhu k dočasnému breakpointu se proto
 * také počítá jako krokování ("debugger"). Běh
 * a pauza na dočasném breakpointu (`pause_reason` se u ní nenastavuje) se
 * považují za krokování debuggeru; ruční pokračování po dokončeném kroku
 * je normální běh uživatele. Bez nahrávání neškodné.
 *
 * @par Vlákna Emu vlákno (dbgapi / UI step over); nebere zámek modulu.
 */
void videorec_on_debugger_run(void);

/**
 * @brief Oznámí reset emulovaného počítače (událost `reset` + auto marker).
 *
 * Bez nahrávání no-op. Zaznamená se pod zámkem a zpracuje na nejbližším
 * konci snímku (emulační čas) nebo ticku vzorkovače (realtime).
 *
 * @par Vlákna Emu vlákno (mzarch_main_reset()) mimo hooky modulu; bere zámek modulu.
 */
void videorec_on_reset(void);

/**
 * @brief Statistiky režimu podle reality běžící nebo poslední session (pro testy, log a report).
 *
 * Invarianty: `frames_written <= ticks`.
 */
typedef struct st_VIDEOREC_RT_STATS {
    uint64_t ticks;            /**< Zpracovaných ticků vzorkovače v realtime (zapsaných i přeskočených). */
    uint64_t frames_written;   /**< Snímků zapsaných v realtime. */
    uint64_t frames_repeated;  /**< Z toho snímků, kdy se od minulého ticku nezobrazil nový snímek (opakování). */
    uint64_t multi_ticks;      /**< Probuzení vzorkovače, která zpracovala víc než 1 tick (pozdní probuzení). */
    uint64_t clock_rebased;    /**< Nová založení řady ticků (zpoždění nad limit dohánění). */
    uint64_t lost_ticks;       /**< Ticky ztracené při novém založení řady. */
    st_VIDEOREC_RT_AUDIO_STATS audio; /**< Statistiky jitter bufferu zvuku. */
} st_VIDEOREC_RT_STATS;

/**
 * @brief Vyplní statistiky režimu podle reality.
 * @param out Výstup (nesmí být NULL).
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
void videorec_get_rt_stats(st_VIDEOREC_RT_STATS *out);

/**
 * @brief JEN PRO TESTY: ruční řízení vzorkovače.
 *
 * Při `manual == true` vlákno vzorkovače netickuje samo; test volá
 * videorec_test_rt_tick() s vlastním časem (deterministické integrační
 * testy bez čekání na reálný čas).
 *
 * @param manual true = ruční režim.
 * @par Vlákna Libovolné vlákno.
 */
void videorec_test_rt_manual(bool manual);

/**
 * @brief JEN PRO TESTY: provede tick vzorkovače v čase `now_us` (jako vlákno vzorkovače).
 * @param now_us Čas monotónních hodin [us] (neklesající).
 * @pre Ruční režim (videorec_test_rt_manual(true)); volající nedrží zámek modulu.
 */
void videorec_test_rt_tick(int64_t now_us);

/**
 * @brief JEN PRO TESTY: přepíše limit velikosti jednoho AVI partu.
 *
 * Umožní testům vyvolat rollover partu (`<jméno>_002.avi`, ...) na malém
 * objemu dat. Není dostupné z UI, INI ani CLI.
 *
 * @param max_bytes Limit v bajtech předaný do st_AVI_WRITER_PARAMS::max_bytes
 *                  (0 = výchozí AVI_WRITER_MAX_BYTES; hodnoty nad ním se
 *                  omezí na AVI_WRITER_MAX_BYTES zapisovačem).
 * @post Platí od dalšího videorec_request_start() (běžící session se nemění).
 * @par Vlákna Libovolné vlákno; bere zámek modulu.
 */
void videorec_test_set_part_limit(uint32_t max_bytes);

/**
 * @brief Informace o nahrávání uložená do snapshotu (pro retake).
 *
 * Identita bodu nahrávky je trojice (session, větev, snímek). **Větev** (take)
 * je souvislý úsek časové osy nahrávky: nová větev s náhodným ID začíná při
 * startu nahrávání, při každém retake (v bodě snapshotu) a při každém švu.
 * Lepidlo drží historii větví aktuální linie (posloupnost dvojic
 * `(take_id, první snímek)`), takže pozná snapshot z větve, jejíž snímky už
 * retake zahodil (opuštěná větev) - takový snapshot vede na šev, ne na retake.
 *
 * Bod "mimo linii" (`take_id == VIDEOREC_TAKE_ID_NONE`) označuje snapshot
 * uložený v době, kdy časová osa byla přerušená a přerušení ještě čekalo na
 * zpracování (viz videorec_get_snapinfo()); jeho stav nepatří žádné větvi,
 * takže jeho pozdější nahrání vede vždy na šev.
 */
typedef struct st_VIDEOREC_SNAPINFO {
    uint64_t session_id; /**< ID session v okamžiku uložení snapshotu (0 = nenahrávalo se). */
    uint64_t frame;      /**< Počet snímků nahrávky v okamžiku uložení (globální index dalšího snímku). */
    uint64_t take_id;    /**< ID větve (take), ve které snapshot vznikl; VIDEOREC_TAKE_ID_NONE = bod mimo linii
                              (neplatný pro retake, také entry ze snapshotu bez nahrávání). */
} st_VIDEOREC_SNAPINFO;

/**
 * @brief Rezervované ID větve "mimo linii": bod snapshotu neplatný pro retake.
 *
 * Žádná větev linie toto ID nikdy nemá (náhodná ID větví jsou nenulová),
 * proto snapshot s ním při nahrání vždy vede na šev. Ve `videorec/state.bin`
 * se zapisuje beze změny formátu (verze 2, pole take_id = 0).
 */
#define VIDEOREC_TAKE_ID_NONE 0u

/**
 * @brief Vyplní informace o nahrávání pro ukládaný snapshot.
 *
 * `frame` zahrnuje i snímky, které ještě čekají na zvuk (ve snapshotu jsou
 * už minulostí; při retake se nejdřív zapíšou). `take_id` je ID aktuální
 * (poslední) větve linie.
 *
 * Výjimka: čeká-li na zpracování přerušení časové osy - nahraný snapshot
 * (videorec_on_snapshot_loaded() před nejbližším koncem snímku, typicky load
 * a hned save v pauze emulace) nebo skok času zpět zachycený tapem zvuku -,
 * ukládaný stav emulace už nepatří poslední větvi (šev nebo retake se
 * provede až na konci snímku). Pak je `take_id = VIDEOREC_TAKE_ID_NONE`
 * (bod mimo linii, pozdější load vždy šev) a stav zvuku se nezachytává.
 * Bez této výjimky by snapshot nesl identitu staré větve a jeho pozdější
 * load by udělal retake: zkrátil by nahrávku za švem a navázal stavem nové
 * osy na starou hru bez švu.
 *
 * Vedlejší efekt (plynulý retake, mimo výjimku výše): lepidlo si pro bod (`take_id`, `frame`)
 * zapamatuje stav audio rendereru (hodnoty kanálů a stavy filtrů) na konci
 * posledního přijatého snímku - hned, nebo až se snímek čekající na zvuk
 * vyrenderuje. Pamatuje se posledních 64 bodů v paměti (ne ve snapshotu:
 * retake je možný jen ve stejné session, tj. ve stejném běhu emulátoru).
 *
 * @param out Výstup (nesmí být NULL).
 * @return true pokud se nahrává (out vyplněn), false jinak (out vynulován).
 * @par Vlákna Vlákno ukládající snapshot při pozastavené emulaci (UI, MCP)
 *      nebo v safe-pointu BP akce (emu vlákno mimo hooky modulu); bere zámek modulu.
 */
bool videorec_get_snapinfo(st_VIDEOREC_SNAPINFO *out);

/**
 * @brief Oznámí, že byl nahrán snapshot (během nahrávání řeší retake / šev).
 *
 * Akce se zaznamená a provede na nejbližším konci snímku v emu vlákně:
 * - `retake_mode == VIDEOREC_RETAKE_DISCARD`, snapshot je z této session,
 *   jeho větev (`info->take_id`) je v aktuální linii a jeho snímek leží
 *   v úseku, který z té větve v nahrávce zůstal (`první snímek větve <=
 *   info->frame <= první snímek následující větve`, u poslední větve
 *   `<= počet snímků nahrávky`), a bod snapshotu neleží před prvním snímkem
 *   aktuálního AVI partu (truncate přes hranici partu V1 neumí; o partu
 *   rozhoduje stav writeru po bariéře - lepidlo nejdřív počká, až writer
 *   zpracuje všechny dříve odeslané snímky, viz videorec.c vr_writer_sync()):
 *   snímky od `info->frame` se zahodí (truncate AVI, sidecar, markery),
 *   větve za bodem snapshotu se z linie odstraní, v bodě snapshotu začne
 *   nová větev a nahrávka pokračuje od toho místa (vynucený klíčový snímek);
 *   čekající snímky před bodem snapshotu se nejdřív zapíšou
 *   (videorec_pipe_flush_until_index()), zbytek se zahodí;
 *   zvuk navazuje plynule: renderer dostane stav zapamatovaný pro bod
 *   snapshotu (videorec_get_snapinfo()) a události podržené od nahrání
 *   snapshotu, takže první snímek po retake zní jako nepřerušený běh;
 *   bez zapamatovaného stavu (cizí běh, víc než 64 novějších snapshotů,
 *   bod v record-pause) pokračují filtry z dosavadního stavu a první snímek
 *   drží hodnoty kanálů z jeho konce;
 *   pokud v bodě snapshotu začala record-pause (snapshot mohl vzniknout
 *   během pauzy, kdy emulace běžela bez zápisu), zůstane tam šev: při
 *   nahrávání se otevře nový segment s výchozím přechodem, při trvající
 *   pauze se segment jen uzavře; událost "Retake: rewound to HH:MM:SS";
 * - jinak (OFF, SEAM, cizí nebo chybějící informace, opuštěná větev, bod mimo
 *   linii (VIDEOREC_TAKE_ID_NONE), bod
 *   před aktuálním partem): šev - čekající snímky se dopíšou, začne nová
 *   větev a otevře se nový segment s výchozím přechodem (při record-pause
 *   se segment neotevírá); událost "Recording seam (snapshot loaded)".
 *
 * Událost se zapíše při zpracování (viz videorec_get_last_event()).
 *
 * Od zavolání do zpracování na konci snímku se události zvuku nepouštějí
 * do pipe a horizont se ignoruje (události už patří nové časové ose a nesmí
 * se promítnout do starých čekajících snímků). Při retake se podržené
 * události přehrají (viz výše); při švu se zahodí a první snímek po švu má
 * zvuk s konstantními hodnotami kanálů platnými na jeho konci.
 *
 * Bez nahrávání no-op.
 *
 * @param info_or_null Informace ze snapshotu, nebo NULL (snapshot bez dat videorec).
 * @par Vlákna Vlákno, které snapshot nahrává - volá ho snap_videorec_after_load():
 *      UI vlákno při pozastavené emulaci, nebo emu vlákno při zpracování
 *      dbgapi/MCP příkazu - drain fronty dbgapi běží jak v paused smyčce
 *      (mzzarch_main_do_emulator_paused()), tak za běhu emulace na konci
 *      snímku (mz800_main_event_callback_screen_done() v mzarch.c, hned za
 *      videorec_on_screen_done(); emulace přitom v pauze není). Na kontextu nezáleží:
 *      funkce jen pod zámkem modulu zaznamená požadavek, zpracuje se na
 *      nejbližším konci snímku.
 */
void videorec_on_snapshot_loaded(const st_VIDEOREC_SNAPINFO *info_or_null);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_H */
