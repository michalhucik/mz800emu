/**
 * @file   overlay_view.h
 * @brief  Kopie stavu emulátoru pro overlay menu (st_OVERLAY_VIEW), plnění pod zámkem a testovací stavy.
 *
 * Overlay menu (overlay.h) kreslí jen z této kopie: UI vlákno ji jednou za
 * snímek vyplní funkcí overlay_view_fill() s drženým device_state_lock()
 * a kreslicí kód pak stav jádra nečte. Kopie nevlastní nic z jádra (žádný
 * ukazatel do paměti jádra), takže zásah jiného vlákna mezi dvěma snímky
 * v ní nenechá ukazatel do uvolněné paměti.
 *
 * Testy (tests/ui/test_overlay.cpp) vyplní kopii i bez jádra funkcí
 * overlay_view_make_fixture().
 *
 * Kopie nese stav pro horní lištu (platforma, rychlost, pauza, replay),
 * pro tab Media (mechaniky FDC0, kazeta, Quick Disk, nedávná média), pro
 * tab Machine (nabídka ROM, dostupnost vlastní rychlosti), pro tab
 * Display (fullscreen, velikost okna, barvy, scanlines, snímková
 * frekvence), pro tab Sound (hlavní hlasitost, ztlumení, výstupní propust,
 * hlasitosti kanálů), pro tab Input (zařízení joysticku) a pro tab Capture
 * (série snímků, video záznam, replay). Stav screenshotů, video záznamu a
 * replay mají vlastní synchronizaci a plní se zvlášť funkcí
 * overlay_view_fill_capture(), mimo zámek zařízení (jako je čte klasické
 * menu). Rychlost je
 * rychlost nastavená (požadovaná), ne právě uplatněná: emu vlákno ji
 * uplatňuje na hranici snímku, takže při stojící emulaci by se jinak
 * změna z overlaye neukázala. Stav kazety se nepočítá znovu:
 * overlay_view_fill() ho bere z kopie okna Virtual CMT (cmt_view_fill()).
 * Nedávná média jsou kopie historie jádra (recent_media.h) a plní se
 * zvlášť funkcí overlay_view_fill_recent(), mimo zámek zařízení: z jádra
 * se čtou jen po změně historie. Existenci souborů zjišťuje pracovní
 * vlákno, jen když jsou položky vidět, a UI vlákno na disk nikdy nečeká:
 * kopie nese poslední známý stav (st_OVERLAY_VIEW_RECENT::checked).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef OVERLAY_VIEW_H
#define OVERLAY_VIEW_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Velikost pole st_OVERLAY_VIEW::platform_text včetně ukončovací nuly. */
#define OVERLAY_VIEW_PLATFORM_TEXT_SIZE 16
/** @brief Počet mechanik v kopii (mechaniky 0 až 3 řadiče FDC0). */
#define OVERLAY_VIEW_FDD_COUNT 4
/** @brief Velikost polí se jménem souboru média (UTF-8) včetně ukončovací nuly; delší jméno se zkrátí na hranici znaku. */
#define OVERLAY_VIEW_NAME_SIZE 512
/** @brief Velikost polí s cestou souboru média (UTF-8) včetně ukončovací nuly; delší cesta se zkrátí na hranici znaku. */
#define OVERLAY_VIEW_PATH_SIZE 1024
/** @brief Velikost pole st_OVERLAY_VIEW::media_disabled_reason včetně ukončovací nuly. */
#define OVERLAY_VIEW_REASON_SIZE 160
/** @brief Největší počet položek nedávných médií jednoho druhu v kopii (RECENT_MEDIA_MAX historie jádra). */
#define OVERLAY_VIEW_RECENT_PER_KIND 12
/** @brief Největší počet položek nedávných médií v kopii (st_OVERLAY_VIEW::recent): tři druhy média. */
#define OVERLAY_VIEW_RECENT_MAX (3 * OVERLAY_VIEW_RECENT_PER_KIND)
/** @brief Největší počet řádků nabídky ROM v kopii (ROM a nadpisy oddílů); další řádky konfigurace se do kopie nedávají. */
#define OVERLAY_VIEW_ROM_MAX 32
/** @brief Velikost pole st_OVERLAY_VIEW_ROM::label včetně ukončovací nuly; delší popis se zkrátí na hranici znaku. */
#define OVERLAY_VIEW_ROM_LABEL_SIZE 128

/** @brief Největší počet zvukových kanálů v kopii (CTC0 a dva čipy PSG po čtyřech kanálech). */
#define OVERLAY_VIEW_AUDIO_CHANNELS_MAX 9

/** @brief Největší hlavní hlasitost poslechu v kopii (AUDIO_MASTER_GAIN_MAX v audio.h; 2,0 = 200 %). */
#define OVERLAY_VIEW_MASTER_GAIN_MAX 2.0f
/** @brief Velikost pole st_OVERLAY_VIEW_CAPTURE::replay_text včetně ukončovací nuly; delší text se zkrátí na hranici znaku. */
#define OVERLAY_VIEW_REPLAY_TEXT_SIZE 256

/** @brief Stav záznamu a přehrávání vstupů v kopii (st_OVERLAY_VIEW_CAPTURE::replay_mode); odpovídá en_REPLAY_MODE. */
typedef enum en_OVERLAY_REPLAY_MODE {
    OVERLAY_REPLAY_OFF = 0,   /**< žádný záznam ani přehrávání */
    OVERLAY_REPLAY_RECORDING, /**< běží záznam vstupů */
    OVERLAY_REPLAY_PLAYING,   /**< přehrává se záznam vstupů */
} en_OVERLAY_REPLAY_MODE;

/** @brief Zařízení portu joysticku v kopii (st_OVERLAY_VIEW_INPUT::joy_type); pořadí segmentů None / Numeric keypad / Joystick, odpovídá en_JOY_TYPE. */
typedef enum en_OVERLAY_JOY_TYPE {
    OVERLAY_JOY_NONE = 0,   /**< port bez zařízení */
    OVERLAY_JOY_NUM_KEYPAD, /**< numerická klávesnice hostitele */
    OVERLAY_JOY_JOYSTICK,   /**< joystick nebo gamepad hostitele */
    OVERLAY_JOY_TYPE_COUNT  /**< počet voleb (není volba) */
} en_OVERLAY_JOY_TYPE;

/** @brief Režim rychlosti emulace v kopii (st_OVERLAY_VIEW::speed_mode). */
typedef enum en_OVERLAY_SPEED_MODE {
    OVERLAY_SPEED_NORMAL = 0, /**< nastavená rychlost 100 % (po uplatnění EMULATOR_TEST_NORMAL_SPEED) */
    OVERLAY_SPEED_CUSTOM,     /**< nastavená vlastní rychlost jiná než 100 % (po uplatnění EMULATOR_TEST_CUSTOM_SPEED) */
    OVERLAY_SPEED_MAX,        /**< maximální rychlost bez čekání na snímek (EMULATOR_TEST_MAX_SPEED) */
} en_OVERLAY_SPEED_MODE;

/** @brief Stav transportu kazety v kopii (st_OVERLAY_VIEW_CMT::state); odpovídá en_CMT_VIEW_STATE. */
typedef enum en_OVERLAY_CMT_STATE {
    OVERLAY_CMT_STOP = 0, /**< páska stojí */
    OVERLAY_CMT_PLAY,     /**< přehrávání (i v pauze, viz st_OVERLAY_VIEW_CMT::paused) */
    OVERLAY_CMT_RECORD,   /**< nahrávání */
} en_OVERLAY_CMT_STATE;

/** @brief Režim načítání kazety v kopii (st_OVERLAY_VIEW_CMT::mode); pořadí segmentů Tape / Boost / Direct. */
typedef enum en_OVERLAY_CMT_MODE {
    OVERLAY_CMT_MODE_TAPE = 0, /**< signál v reálném čase */
    OVERLAY_CMT_MODE_BOOST,    /**< signál při maximální rychlosti po dobu běhu motoru */
    OVERLAY_CMT_MODE_DIRECT,   /**< přímé načtení MZF do paměti */
    OVERLAY_CMT_MODE_COUNT     /**< počet režimů (není režim) */
} en_OVERLAY_CMT_MODE;

/** @brief Druh média položky nedávných médií (st_OVERLAY_VIEW_RECENT::kind). */
typedef enum en_OVERLAY_MEDIA_KIND {
    OVERLAY_MEDIA_DSK = 0, /**< obraz diskety */
    OVERLAY_MEDIA_CMT,     /**< páska */
    OVERLAY_MEDIA_QD,      /**< Quick Disk */
    OVERLAY_MEDIA_KIND_COUNT /**< počet druhů (není druh) */
} en_OVERLAY_MEDIA_KIND;

/** @brief Testovací stavy kopie pro overlay_view_make_fixture(). */
typedef enum en_OVERLAY_FIXTURE {
    OVERLAY_FIXTURE_DEFAULT = 0, /**< obsah konceptů media-v2, media-insert-v2, machine-v1, display-v1, sound-v1, input-v1 a capture-v1: MZ-800, 100 %, pauza; ROM Standard z nabídky MZ-800; fullscreen, velikost okna Fullscreen, uzamčený poměr stran, barvy Normal, scanlines 35 %, Swap With V-Sync; hlavní hlasitost 80 %, propust zapnutá, pět kanálů (šum 70 %); joystick na numerické klávesnici; série snímků a video záznam vypnuté, záznam podle reality, replay vypnutý; FDD 0 cpm-system.dsk (činná), FDD 1 games-vol2.dsk s neuloženými změnami, FDD 2 a 3 prázdné; páska flappy.mzf hraje, blok 2 z 5, 01:12 / 03:05, režim Boost; Quick Disk prázdný; nedávná média: šest obrazů disket konceptu, cpm-system.dsk, dvě pásky a jeden Quick Disk */
    OVERLAY_FIXTURE_EMPTY,       /**< žádné médium: mechaniky prázdné, bez pásky, Quick Disk prázdný; emulace běží; historie médií prázdná; vlastní rychlost není k obnovení, okno v okně, scanlines vypnuté; ztlumeno, hlavní hlasitost 0 %, stroj bez výstupní propusti a jen s kanálem CTC0; port joysticku bez zařízení a žádný joystick hostitele; poslední replay skončil s výsledkem */
    OVERLAY_FIXTURE_LONG_TEXTS,  /**< jména souborů o 200 znacích, cesty s diakritikou, všechna média vložená a s neuloženými změnami, vlastní rychlost 1000 %; vybraná ROM s dlouhým popisem, režim Custom FPS 1000 Hz; plná historie médií (12 položek každého druhu) s dlouhými jmény, každá třetí položka chybí na disku; devět zvukových kanálů (dva čipy PSG); běží série snímků, video záznam je pozastavený, běží záznam vstupů */
    OVERLAY_FIXTURE_REPLAY,      /**< obsah DEFAULT při aktivním záznamu nebo přehrávání vstupů: karty médií zakázané s důvodem */
    OVERLAY_FIXTURE_NO_FDC,      /**< obsah DEFAULT s odpojeným řadičem disket a odpojeným Quick Diskem */
    OVERLAY_FIXTURE_COUNT,       /**< počet testovacích stavů (není platný stav) */
} en_OVERLAY_FIXTURE;

/**
 * @brief Stav jedné disketové mechaniky řadiče FDC0 v kopii.
 *
 * Invarianty: name a path jsou ukončené nulou a platné UTF-8; při
 * !mounted jsou prázdné a unsaved, readonly, fs_readonly, shared_readonly
 * i size_kb jsou nulové a shared_with i wp_blocker jsou -1. active platí
 * jen při connected. shared_readonly znamená readonly. shared_with a
 * wp_blocker jsou -1 nebo číslo mechaniky přes oba řadiče (0 až 3 = FDC0,
 * 4 až 7 = FDC1), nikdy číslo této mechaniky.
 */
typedef struct st_OVERLAY_VIEW_FDD {
    bool present;                      /**< build mechaniky má (CFG_HWEXT_HAVE_FDC); false = řádek se nekreslí */
    bool connected;                    /**< řadič FDC0 je připojený */
    bool mounted;                      /**< v mechanice je obraz diskety */
    char name[OVERLAY_VIEW_NAME_SIZE]; /**< jméno souboru obrazu bez cesty */
    char path[OVERLAY_VIEW_PATH_SIZE]; /**< celá cesta obrazu */
    bool readonly;                     /**< obraz je chráněný proti zápisu (volba uživatele, soubor nebo sdílení s jinou mechanikou) */
    bool user_readonly;                /**< volba "Read-only" mechaniky v konfiguraci (platí i pro prázdnou mechaniku; položka klasického menu) */
    bool fs_readonly;                  /**< soubor vloženého obrazu nejde zapisovat (volbu pak nejde změnit, jako v klasickém menu) */
    bool shared_readonly;              /**< obraz má ochranu, protože tentýž soubor byl při vložení už v jiné mechanice (fdc_drive_shared_readonly()) */
    int shared_with;                   /**< mechanika, která tentýž soubor drží (fdc_drive_shared_with()); -1 = žádná nebo obraz ochranu kvůli sdílení nemá */
    int wp_blocker;                    /**< mechanika, která tentýž soubor drží se zápisem, takže tady ochranu nejde sundat (fdc_drive_write_blocker()); -1 = žádná */
    bool unsaved;                      /**< obraz má neuložené změny, které jde uložit (režim cached; podmínka položky "Sync now" klasického menu) */
    bool active;                       /**< mechanika je vybraná a motor běží (registr 0DCh řadiče v okamžiku plnění) */
    unsigned size_kb;                  /**< velikost dat obrazu v KB (z geometrie načtené při vložení); 0 = neznámá */
} st_OVERLAY_VIEW_FDD;

/**
 * @brief Stav kazety v kopii (výtah z kopie okna Virtual CMT, st_CMT_VIEW).
 *
 * Invarianty: !has_tape => name a path prázdné, block_count == 0,
 * current_block == -1, state == OVERLAY_CMT_STOP. 0 <= progress <= 1.
 * elapsed_s < 0 nebo total_s < 0 = čas není známý. first_playable je -1
 * nebo index bloku v rozsahu 0 až block_count - 1.
 */
typedef struct st_OVERLAY_VIEW_CMT {
    bool has_tape;                     /**< páska vložená */
    char name[OVERLAY_VIEW_NAME_SIZE]; /**< jméno souboru pásky bez cesty; prázdné u nové pásky bez souboru */
    char path[OVERLAY_VIEW_PATH_SIZE]; /**< celá cesta pásky; prázdná bez souboru */
    bool modified;                     /**< páska má neuložené změny */
    en_OVERLAY_CMT_STATE state;        /**< stav transportu */
    bool paused;                       /**< páska stojí v PLAY nebo RECORD (pauza uživatele nebo stojící motor) */
    bool user_paused;                  /**< pauza uživatele */
    int block_count;                   /**< počet bloků pásky */
    int current_block;                 /**< otevřený blok (index od 0), -1 bez bloku; při RECORD block_count */
    double elapsed_s;                  /**< pozice od začátku pásky v s; záporná = neznámá */
    double total_s;                    /**< délka pásky v s; záporná = neznámá */
    double progress;                   /**< průběh 0 až 1: pozice na pásce při známé délce, jinak průběh otevřeného bloku */
    en_OVERLAY_CMT_MODE mode;          /**< režim načítání */
    bool direct_supported;             /**< přímé načtení s vybranou ROM jde (st_CMT_VIEW::direct_supported) */
    bool current_has_mzf;              /**< otevřený blok nese MZF (podmínka přechodu do Direct); true i bez otevřeného bloku */
    int first_playable;                /**< první přehratelný blok (cíl Rewind); -1 = žádný */
    unsigned tape_serial;              /**< čítač výměn pásky jádra v době plnění (podmínka operací s číslem bloku) */
    bool replay_recording;             /**< běží záznam vstupů: výměna pásky a režim načítání se zamykají */
    bool replay_playing;               /**< přehrává se záznam vstupů: pásku ovládá záznam */
} st_OVERLAY_VIEW_CMT;

/**
 * @brief Stav Quick Disku v kopii.
 *
 * Invarianty: name a path jsou ukončené nulou a platné UTF-8; při
 * !mounted jsou prázdné a unsaved je false.
 */
typedef struct st_OVERLAY_VIEW_QD {
    bool present;                      /**< build Quick Disk má (CFG_HWEXT_HAVE_QDISK); false = karta se nekreslí */
    bool connected;                    /**< Quick Disk je připojený */
    bool unicard;                      /**< mechaniku řídí Unicard (klasické menu ji pak nenabízí) */
    bool directory;                    /**< virtuální Quick Disk: médium je adresář, ne soubor obrazu */
    bool mounted;                      /**< médium je vložené (cesta obrazu nebo adresáře je nastavená) */
    char name[OVERLAY_VIEW_NAME_SIZE]; /**< jméno souboru nebo adresáře bez cesty */
    char path[OVERLAY_VIEW_PATH_SIZE]; /**< celá cesta */
    bool unsaved;                      /**< obraz má neuložené změny (podmínka položky "Save now" klasického menu) */
    bool write_protected;              /**< volba "Write Protected" Quick Disku v konfiguraci (položka klasického menu) */
} st_OVERLAY_VIEW_QD;

/**
 * @brief Položka nedávných médií v kopii.
 *
 * Invarianty: name a path jsou ukončené nulou a platné UTF-8; path je
 * celá cesta z historie (položka, jejíž cesta se do pole nevejde, se do
 * kopie nedává). Při !checked jsou missing a is_dir false (stav není
 * známý).
 */
typedef struct st_OVERLAY_VIEW_RECENT {
    en_OVERLAY_MEDIA_KIND kind;        /**< druh média */
    char name[OVERLAY_VIEW_NAME_SIZE]; /**< jméno souboru nebo adresáře bez cesty */
    char path[OVERLAY_VIEW_PATH_SIZE]; /**< celá cesta */
    bool checked;                      /**< existence už byla aspoň jednou zjištěna (missing a is_dir platí); false = pracovní vlákno ještě neodpovědělo */
    bool missing;                      /**< soubor nebo adresář už neexistuje (kreslí se šedě); poslední známý stav */
    bool is_dir;                       /**< cesta je adresář (virtuální Quick Disk); poslední známý stav */
    long long checked_us;              /**< čas posledního zjištění existence (g_get_monotonic_time()); platí při checked */
    long long time;                    /**< čas posledního vložení v sekundách od 1. 1. 1970 UTC; 0 = neznámý */
} st_OVERLAY_VIEW_RECENT;

/** @brief Stáří výsledku zjištění existence (st_OVERLAY_VIEW_RECENT::checked_us), do kterého se podle stavu "chybí" smí položka smazat z historie, v mikrosekundách. */
#define OVERLAY_VIEW_RECENT_FRESH_US 3000000LL

/**
 * @brief Řádek nabídky ROM v kopii (položka "ROM" klasického menu).
 *
 * Invarianty: label je ukončený nulou a platné UTF-8; u řádku section je
 * current false a row se nepoužívá; label smí být prázdný jen u řádku
 * section (oddělovač bez nadpisu).
 */
typedef struct st_OVERLAY_VIEW_ROM {
    char label[OVERLAY_VIEW_ROM_LABEL_SIZE]; /**< přeložený popis ROM nebo nadpis oddílu */
    bool section;                            /**< řádek je nadpis oddílu nebo oddělovač, ne ROM */
    bool current;                            /**< tato ROM je právě vybraná (ROM_TEST_CURRENT()) */
    int row;                                 /**< index řádku v g_rom_config->roms (argument výběru ROM) */
} st_OVERLAY_VIEW_ROM;

/**
 * @brief Stav pro tab Machine v kopii.
 *
 * Invarianty: rom_count <= OVERLAY_VIEW_ROM_MAX; nejvýš jeden řádek rom má
 * current == true; řádky jsou v pořadí konfigurace ROM, bez zakázaných
 * řádků a bez vývojových ROM mimo Development Mode.
 */
typedef struct st_OVERLAY_VIEW_MACHINE {
    unsigned rom_count;                            /**< počet platných řádků rom */
    st_OVERLAY_VIEW_ROM rom[OVERLAY_VIEW_ROM_MAX]; /**< nabídka ROM */
    bool custom_speed_available;                   /**< volba "Custom" má co nastavit: vlastní rychlost je nastavená, nebo ji akce umí vrátit (overlay_action_custom_speed_restore_value() != 0) */
} st_OVERLAY_VIEW_MACHINE;

/**
 * @brief Stav pro tab Display v kopii (nastavení g_display a stav okna).
 *
 * Invarianty: startup_size je hodnota en_DISPLAY_STARTUP_SIZE,
 * color_schema hodnota en_DISPLAY_COLOR_SCHEMA a framerate_mode hodnota
 * en_DISPLAY_FRAMERATE_MODE (display.h); scanlines_intensity je v rozsahu
 * DISPLAY_SCANLINES_INTENSITY_MIN až _MAX a custom_fps v rozsahu
 * DISPLAY_CUSTOM_FPS_MIN až _MAX, pokud je jádro drží v rozsahu (kopie je
 * neupravuje).
 */
typedef struct st_OVERLAY_VIEW_DISPLAY {
    bool fullscreen;         /**< hlavní okno je právě ve fullscreenu (dotaz video rozhraní is_fullscreen(); false, když ho rozhraní nemá) */
    int startup_size;        /**< velikost okna při startu */
    bool locked_aspect;      /**< poměr stran okna je uzamčený */
    int color_schema;        /**< barevné schéma */
    bool scanlines;          /**< scanlines CRT zapnuté */
    int scanlines_intensity; /**< intenzita scanlines v procentech */
    int framerate_mode;      /**< řízení snímkové frekvence */
    int custom_fps;          /**< vlastní snímková frekvence v Hz (platí v režimu Custom FPS) */
    int slave_hz;            /**< snímková frekvence emulovaného stroje v Hz (text volby "Slave") */
    bool forced_redraw;      /**< vynucené překreslení každého snímku */
} st_OVERLAY_VIEW_DISPLAY;

/**
 * @brief Stav přepínače Listen tabu Sound v kopii (overlay.h, oddíl Tab Sound).
 *
 * Stav neplní overlay_view_fill() (není to stav jádra), ale rámec overlaye
 * v imgui_overlay_frame() podle vlastnictví pauzy; testovací stavy ho
 * nastavují v overlay_view_make_fixture().
 */
typedef enum en_OVERLAY_LISTEN {
    OVERLAY_LISTEN_OFF = 0,         /**< vypnuto a dostupné: emulace stojí pauzou overlaye */
    OVERLAY_LISTEN_ON,              /**< zapnuto a dostupné: emulace běží kvůli poslechu, nebo se na žádost o poslech právě rozbíhá (st_OVERLAY_VIEW::paused říká, co platí) */
    OVERLAY_LISTEN_BLOCKED_PAUSED,  /**< vypnuto a zakázáno: emulace stojí a pauza není overlaye (stála před otevřením, zastavil ji uživatel nebo někdo jiný) */
    OVERLAY_LISTEN_BLOCKED_RUNNING, /**< zapnuto a zakázáno: emulace běží z jiného důvodu než kvůli poslechu (rozběhl ji uživatel nebo někdo jiný) */
} en_OVERLAY_LISTEN;

/**
 * @brief Stav pro tab Sound v kopii (zvukové rozhraní g_iface_audio a přepínač Listen).
 *
 * Invarianty: 0 <= master_gain <= OVERLAY_VIEW_MASTER_GAIN_MAX; channel_count je 1 (jen CTC0), 5
 * (CTC0 a jeden čip PSG) nebo 9 (dva čipy PSG) a nikdy víc než
 * OVERLAY_VIEW_AUDIO_CHANNELS_MAX; channel_gain[i] pro i < channel_count
 * je hlasitost kanálu 0 až 1 (pořadí kanálů jako v okně Audio Mixer);
 * stereo_names platí jen při channel_count == 9; output_filter platí jen
 * při has_output_filter; listen je hodnota en_OVERLAY_LISTEN.
 */
typedef struct st_OVERLAY_VIEW_SOUND {
    int listen;                                          /**< stav přepínače Listen (en_OVERLAY_LISTEN); plní rámec overlaye, ne overlay_view_fill() */
    float master_gain;                                   /**< hlavní hlasitost poslechu 0 až OVERLAY_VIEW_MASTER_GAIN_MAX, 1 = 100 % (iface_audio_get_master_gain()) */
    bool mute;                                           /**< poslech je ztlumený (iface_audio_get_mute()) */
    bool has_output_filter;                              /**< stroj má věrnou výstupní propust (AUDIO_HAVE_OUTPUT_FILTER); false = řádek se nekreslí */
    bool output_filter;                                  /**< věrná výstupní propust je zapnutá (iface_audio_output_filter_enabled()) */
    unsigned channel_count;                              /**< počet kanálů, které ukazuje okno Audio Mixer */
    bool stereo_names;                                   /**< kanály se jmenují "PSG0 ..." a "PSG1 ..." (dva čipy PSG) */
    float channel_gain[OVERLAY_VIEW_AUDIO_CHANNELS_MAX]; /**< hlasitosti kanálů 0 až 1 (g_iface_audio.gain[]) */
} st_OVERLAY_VIEW_SOUND;

/**
 * @brief Stav pro tab Input v kopii.
 *
 * Invariant: joy_type je hodnota en_OVERLAY_JOY_TYPE.
 */
typedef struct st_OVERLAY_VIEW_INPUT {
    int joy_type;       /**< zařízení prvního portu joysticku (g_joy.dev[JOY_DEVID_0].type) */
    bool joy_available; /**< hostitel má aspoň jeden joystick nebo gamepad (g_iface_joy.available_count > 0; podmínka volby "Joystick") */
} st_OVERLAY_VIEW_INPUT;

/**
 * @brief Stav pro tab Capture v kopii (série snímků, video záznam, replay).
 *
 * Invarianty: video_paused => video_pause_enabled; replay_mode je hodnota
 * en_OVERLAY_REPLAY_MODE; replay_text je ukončený nulou a platné UTF-8
 * (při replay_mode == OVERLAY_REPLAY_OFF text výsledku posledního replay
 * nebo prázdný, při přehrávání text odchylky nebo prázdný, při záznamu
 * prázdný).
 */
typedef struct st_OVERLAY_VIEW_CAPTURE {
    bool series_active;                            /**< běží série snímků "Record Changed Frames" (screenshot_series_get_status()) */
    bool video_stop;                               /**< tlačítko nahrávání znamená "Stop Recording": nahrává se nebo čeká start (videorec_ui_menu_model()) */
    bool video_pause_enabled;                      /**< "Pause Recording" je dostupné (nahrává se) */
    bool video_paused;                             /**< nahrávání je pozastavené */
    bool video_realtime;                           /**< časová základna záznamu je "podle reality" (VIDEOREC_TIMEBASE_REALTIME) */
    int replay_mode;                               /**< stav záznamu a přehrávání vstupů */
    unsigned replay_frame;                         /**< snímek od startu záznamu nebo přehrávání */
    unsigned replay_frames;                        /**< délka přehrávaného záznamu ve snímcích */
    unsigned replay_events;                        /**< počet zaznamenaných událostí */
    char replay_text[OVERLAY_VIEW_REPLAY_TEXT_SIZE]; /**< doplňující text stavu (anglicky z jádra, nepřekládá se) */
} st_OVERLAY_VIEW_CAPTURE;

/**
 * @brief Kopie stavu emulátoru, ze které overlay menu kreslí.
 *
 * Invarianty: platform_text je vždy ukončený nulou; speed_percent je
 * nastavená (požadovaná) vlastní rychlost v procentech (1 až
 * CUSTOMSPEED_MAX_VALUE) a při speed_mode == OVERLAY_SPEED_NORMAL je 100.
 * Při OVERLAY_SPEED_MAX nese speed_percent nastavenou vlastní rychlost, ne
 * skutečně dosaženou (tu jádro bez měření neposkytuje). media_disabled_reason je ukončený
 * nulou; neprázdný <=> karty médií jsou jako celek zakázané.
 * recent_count <= OVERLAY_VIEW_RECENT_MAX; položky recent jsou seřazené od
 * naposledy vložené (položky téhož druhu v pořadí historie jádra) a žádný
 * druh jich nemá víc než OVERLAY_VIEW_RECENT_PER_KIND.
 *
 * Kopie nevlastní žádnou paměť jádra a nemá žádný ukazatel; lze ji volně
 * kopírovat.
 */
typedef struct st_OVERLAY_VIEW {
    char platform_text[OVERLAY_VIEW_PLATFORM_TEXT_SIZE];    /**< název stroje pro stavový štítek ("MZ-800", "MZ-700", "MZ-1500") */
    unsigned speed_percent;                                 /**< nastavená rychlost v procentech */
    en_OVERLAY_SPEED_MODE speed_mode;                       /**< režim rychlosti */
    bool paused;                                            /**< emulace stojí (EMULATOR_TEST_PAUSED) */
    bool replay_active;                                     /**< běží záznam nebo přehrávání vstupů (replay_is_active()) */
    /* tab Media */
    st_OVERLAY_VIEW_FDD fdd[OVERLAY_VIEW_FDD_COUNT];        /**< mechaniky 0 až 3 řadiče FDC0 */
    st_OVERLAY_VIEW_CMT cmt;                                /**< kazeta */
    st_OVERLAY_VIEW_QD qd;                                  /**< Quick Disk */
    char media_disabled_reason[OVERLAY_VIEW_REASON_SIZE];   /**< přeložený důvod, proč jsou karty médií zakázané (aktivní replay); prázdný = nejsou */
    unsigned recent_count;                                  /**< počet platných položek recent */
    st_OVERLAY_VIEW_RECENT recent[OVERLAY_VIEW_RECENT_MAX]; /**< nedávná média všech druhů od naposledy vloženého */
    long long recent_now;                                   /**< současný čas v sekundách od 1. 1. 1970 UTC (pro stáří položek recent) */
    /* taby Machine a Display */
    st_OVERLAY_VIEW_MACHINE machine;                        /**< nabídka ROM a dostupnost vlastní rychlosti */
    st_OVERLAY_VIEW_DISPLAY display;                        /**< nastavení zobrazení a stav okna */
    /* taby Sound, Input a Capture */
    st_OVERLAY_VIEW_SOUND sound;                            /**< hlasitosti a výstupní propust */
    st_OVERLAY_VIEW_INPUT input;                            /**< zařízení joysticku */
    st_OVERLAY_VIEW_CAPTURE capture;                        /**< série snímků, video záznam a replay (plní overlay_view_fill_capture()) */
} st_OVERLAY_VIEW;

/**
 * @brief Nastaví kopii do výchozího stavu (prázdná platforma, 100 %, běží, bez replay, žádné zařízení ani médium, hlavní hlasitost 100 %, jeden zvukový kanál).
 * @param v Kopie; NULL se ignoruje.
 * @post Kopie splňuje invarianty st_OVERLAY_VIEW.
 */
void overlay_view_reset(st_OVERLAY_VIEW *v);

/**
 * @brief Vyplní kopii ze stavu jádra.
 *
 * Jediné místo, kde overlay menu čte stav jádra: g_emulator, g_customspeed,
 * g_rom_config a g_rom (nabídka ROM), g_display a dotaz video rozhraní
 * is_fullscreen() (stav okna),
 * replay_is_active(), g_fdc[FDC0] (připojení, mechaniky, registr motoru),
 * g_qdisk, stav kazety přes cmt_view_fill() (do vlastní kopie okna
 * Virtual CMT, ze které se opíší skalární údaje), zvukové rozhraní
 * (g_iface_audio, počet kanálů podle g_psg_module) a zařízení joysticku
 * (g_joy, g_iface_joy). Nedávná média neplní (recent_count zůstane 0): ta
 * doplní overlay_view_fill_recent() po uvolnění zámku; stejně tak stav
 * tabu Capture doplní overlay_view_fill_capture().
 *
 * @param v Kopie; NULL se ignoruje. Předchozí obsah se přepíše celý.
 * @pre UI vlákno, volající drží device_state_lock().
 * @post Kopie splňuje invarianty st_OVERLAY_VIEW; jádro se nemění. Vedlejší
 *       efekt cmt_view_fill(): ohlášení výsledku nahrávání do řádku zpráv
 *       okna Virtual CMT (jako při kreslení toho okna).
 */
void overlay_view_fill(st_OVERLAY_VIEW *v);

/**
 * @brief Doplní do kopie stav tabu Capture: sérii snímků, video záznam a replay.
 *
 * Čte screenshot_series_get_status(), videorec_get_state(),
 * videorec_get_status(), videorec_ui_is_start_pending() a
 * replay_get_status(): tytéž dotazy, kterými svůj stav zjišťuje klasické
 * menu, a stejně jako ono bez zámku zařízení (služby mají vlastní
 * synchronizaci).
 *
 * @param v Kopie po overlay_view_fill(); NULL se ignoruje. Přepíše se jen pole capture.
 * @pre UI vlákno; volající nedrží device_state_lock().
 */
void overlay_view_fill_capture(st_OVERLAY_VIEW *v);

/**
 * @brief Doplní do kopie nedávná média z mezipaměti; nikdy nečeká na disk.
 *
 * Mezipaměť se obnoví z historie jádra po její změně (recent_media_dup();
 * historie má vlastní zámek, zámek zařízení není potřeba a nesmí se kvůli
 * tomu držet). Existenci souborů zjišťuje pracovní vlákno: funkce jen
 * vyzvedne jeho hotový výsledek a při visible == true zadá další kolo,
 * pokud žádné neběží a od posledního výsledku uplynula sekunda (nebo
 * přibyla položka s neznámým stavem, nebo o to žádá
 * overlay_view_recent_recheck()). Běží nejvýš jedno pracovní vlákno;
 * dokud nedoběhne (nedostupná síťová cesta), další kola se nezadávají a
 * kopie nese poslední známý stav. Známý stav položky přežije změnu
 * historie (páruje se podle cesty).
 *
 * Pracovní vlákno je odpojené a pracuje jen s vlastní kopií cest a se
 * statickými daty modulu, takže ukončení aplikace na něj nečeká a vlákno
 * nesáhne na nic, co se při ukončení uvolňuje.
 *
 * @param v       Kopie po overlay_view_fill(); NULL se ignoruje. Přepíše se recent, recent_count a recent_now.
 * @param visible Jsou položky právě vidět (pruh Recent media nebo výsuvný list)? false = existence se nezjišťuje.
 * @pre UI vlákno; volající nemusí (a nemá) držet device_state_lock().
 */
void overlay_view_fill_recent(st_OVERLAY_VIEW *v, bool visible);

/**
 * @brief Vyžádá nové zjištění existence souborů nedávných médií, jakmile to půjde.
 *
 * Kolo se zadá při příštím overlay_view_fill_recent() s visible == true,
 * až nebude běžet předchozí. Volá kód, který historii právě změnil nebo
 * zjistil, že soubor chybí, a testy.
 *
 * @pre UI vlákno.
 */
void overlay_view_recent_recheck(void);

/**
 * @brief Běží zjišťování existence souborů nedávných médií, nebo čeká jeho nevyzvednutý výsledek?
 * @return true = pracovní vlákno pracuje nebo jeho výsledek ještě nevyzvedlo overlay_view_fill_recent().
 * @pre UI vlákno.
 */
bool overlay_view_recent_busy(void);

#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: stav cesty, který vrací náhradní zjišťování existence (overlay_view_exists_fn). */
typedef enum en_OVERLAY_VIEW_PATH_STATE {
    OVERLAY_VIEW_PATH_MISSING = 0, /**< cesta neexistuje */
    OVERLAY_VIEW_PATH_FILE,        /**< cesta existuje a není adresář */
    OVERLAY_VIEW_PATH_DIR,         /**< cesta je adresář */
} en_OVERLAY_VIEW_PATH_STATE;

/**
 * @brief Jen pro testy: funkce, kterou pracovní vlákno zjišťuje existenci cesty.
 * @param path Cesta (UTF-8).
 * @return Stav cesty. Volá se z pracovního vlákna; smí se zablokovat.
 */
typedef en_OVERLAY_VIEW_PATH_STATE (*overlay_view_exists_fn)(const char *path);

/**
 * @brief Jen pro testy: nahradí zjišťování existence cest náhradní funkcí.
 * @param fn Náhradní funkce; NULL = skutečný dotaz na disk. Platí pro kola zadaná po volání.
 * @pre UI vlákno.
 */
void overlay_view_test_set_exists_fn(overlay_view_exists_fn fn);
#endif

/**
 * @brief Vyplní kopii testovacím stavem bez čtení jádra.
 * @param v Kopie; NULL se ignoruje.
 * @param f Testovací stav; neplatná hodnota dá stav jako overlay_view_reset().
 * @post Kopie splňuje invarianty st_OVERLAY_VIEW.
 */
void overlay_view_make_fixture(st_OVERLAY_VIEW *v, en_OVERLAY_FIXTURE f);

/**
 * @brief Zkopíruje text UTF-8 do pole pevné velikosti; při zkrácení neskončí uprostřed znaku.
 * @param dst  Cíl; NULL se ignoruje.
 * @param size Velikost cíle v bajtech včetně ukončovací nuly; 0 se ignoruje.
 * @param src  Zdroj; NULL = prázdný text. Neplatné UTF-8 se zkopíruje jen po první neplatný bajt.
 * @post dst je ukončený nulou a platné UTF-8.
 */
void overlay_view_copy_text(char *dst, size_t size, const char *src);

#endif /* OVERLAY_VIEW_H */
