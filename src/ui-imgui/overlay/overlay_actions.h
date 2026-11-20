/**
 * @file   overlay_actions.h
 * @brief  Tabulka akcí overlay menu: popisek, ikona, zkratka, funkce a dostupnost na jednom místě.
 *
 * Každý ovládací prvek overlaye, který zasahuje do emulátoru, je řádek
 * tabulky akcí. Z téhož řádku se kreslí tlačítko (popisek, ikona, štítek
 * zkratky), registruje se klávesová zkratka pro okno overlaye a spouští
 * se funkce. Funkce akce je vždy tatáž, kterou volá klasické menu nebo
 * globální zkratka (imgui_global_shortcuts()); overlay nemá vlastní cestu
 * do jádra.
 *
 * Zkratky: globální zkratky emulátoru (imgui_global_shortcuts()) se
 * obsluhují při fokusu hlavního okna, a to jen dokud overlay klávesy
 * nezadržuje; při fokusu okna overlaye tedy neběží a overlay stejné
 * kombinace obsluhuje sám z této tabulky
 * (overlay_actions_handle_shortcuts()). Štítek zkratky tedy znamená
 * "funguje právě teď": akce bez zkratky ho nemá a nedostupná akce na
 * zkratku nereaguje. Globální zkratky volá i okno debuggeru, když má fokus
 * ono (debugger_window.cpp); přesune-li uživatel při otevřeném overlayi
 * fokus do okna debuggeru (okno ve vlastním okně systému), obslouží se
 * tam globální zkratky i při otevřeném overlayi a overlay o nich neví
 * (chování za běhu neověřeno).
 *
 * Dostupnost se počítá jen z kopie stavu (st_OVERLAY_VIEW); nedostupná
 * akce má přeložený důvod pro tooltip. Podmínky kazety odpovídají oknu
 * Virtual CMT (cmt_window.cpp, cw_actions()); jádro je při provedení
 * ověřuje znovu.
 *
 * Akce s hodnotou (výběr ROM, posuvníky, výběry z N možností) mají v řádku
 * tabulky arg == OVL_ACTION_ARG_VALUE: prvek je vyžádá funkcí
 * overlay_action_request_value() a funkce akce dostane předanou hodnotu
 * místo argumentu z tabulky. Zkratku taková akce nemá.
 *
 * Spuštění je odložené: prvek akci jen vyžádá (overlay_action_request())
 * a rámec ji provede po vykreslení okna overlaye
 * (overlay_actions_run_pending()), mimo Begin() / End() a bez drženého
 * device_state_lock(). Podle řádku tabulky se overlay před akcí nebo po
 * ní zavře: okno nástroje overlay zavírá, modální dialog se otevírá nad
 * ním.
 *
 * @par Vlákna
 * Jen UI vlákno.
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

#ifndef OVERLAY_ACTIONS_H
#define OVERLAY_ACTIONS_H

#include "libs/imgui/imgui.h"

#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"

/**
 * @brief Akce overlay menu.
 *
 * Akce mechanik tvoří souvislé čtveřice: akce mechaniky i je
 * `OVL_ACTION_FDD_*_0 + i` pro i = 0 až 3.
 */
typedef enum en_OVL_ACTION {
    OVL_ACTION_NONE = 0,       /**< žádná akce */
    /* řada akcí */
    OVL_ACTION_RESUME,         /**< zavřít overlay a rozběhnout emulaci (i když před otevřením stála) */
    OVL_ACTION_SPEED,          /**< přepnout maximální rychlost (emulator_max_speed(), jako Alt+M) */
    OVL_ACTION_QUICK_SAVE,     /**< Quick Save (snapshot_ui_request_quicksave(), jako Alt+F8) */
    OVL_ACTION_QUICK_LOAD,     /**< Quick Load (snapshot_ui_request_quickload(), jako Alt+F9); overlay se před ním zavře */
    OVL_ACTION_SCREENSHOT,     /**< uložit screenshot (screenshot_ui_save_quick(), jako Alt+F) */
    OVL_ACTION_FULLSCREEN,     /**< přepnout fullscreen (switch_fullscreen(), jako Alt+Ctrl+Enter) */
    OVL_ACTION_RESET,          /**< reset stroje (mzarch_request_user_reset(), jako F12); overlay se potom zavře */
    /* mechaniky řadiče FDC0 */
    OVL_ACTION_FDD_INSERT_0,   /**< vložit nebo vyměnit disketu v FDD 0 (fdc_ui_mount(), jako Alt+1) */
    OVL_ACTION_FDD_INSERT_1,   /**< totéž pro FDD 1 (Alt+2) */
    OVL_ACTION_FDD_INSERT_2,   /**< totéž pro FDD 2 (Alt+3) */
    OVL_ACTION_FDD_INSERT_3,   /**< totéž pro FDD 3 (Alt+4) */
    OVL_ACTION_FDD_EJECT_0,    /**< vysunout disketu z FDD 0 (fdc_request_umount(), jako Alt+Shift+1) */
    OVL_ACTION_FDD_EJECT_1,    /**< totéž pro FDD 1 (Alt+Shift+2) */
    OVL_ACTION_FDD_EJECT_2,    /**< totéž pro FDD 2 (Alt+Shift+3) */
    OVL_ACTION_FDD_EJECT_3,    /**< totéž pro FDD 3 (Alt+Shift+4) */
    OVL_ACTION_FDD_SAVE_0,     /**< uložit změny diskety v FDD 0 (fdc_request_sync(), položka "Sync now") */
    OVL_ACTION_FDD_SAVE_1,     /**< totéž pro FDD 1 */
    OVL_ACTION_FDD_SAVE_2,     /**< totéž pro FDD 2 */
    OVL_ACTION_FDD_SAVE_3,     /**< totéž pro FDD 3 */
    OVL_ACTION_FDD_WP_0,       /**< přepnout volbu "Read-only" mechaniky FDD 0 (fdc_request_set_readonly(), položka klasického menu) */
    OVL_ACTION_FDD_WP_1,       /**< totéž pro FDD 1 */
    OVL_ACTION_FDD_WP_2,       /**< totéž pro FDD 2 */
    OVL_ACTION_FDD_WP_3,       /**< totéž pro FDD 3 */
    /* kazeta */
    OVL_ACTION_CMT_REWIND,     /**< převinout na první přehratelný blok (REPLAY_CMT_SEEK) */
    OVL_ACTION_CMT_PLAY,       /**< přehrávat, zrušit pauzu uživatele nebo rozjet stojící nahrávání (REPLAY_CMT_PLAY / REPLAY_CMT_PAUSE 0) */
    OVL_ACTION_CMT_PAUSE,      /**< pauza uživatele při přehrávání (REPLAY_CMT_PAUSE 1) */
    OVL_ACTION_CMT_STOP,       /**< zastavit pásku (REPLAY_CMT_STOP_BUTTON) */
    OVL_ACTION_CMT_RECORD,     /**< nahrávat; při nahrávání nahrávání ukončit (REPLAY_CMT_RECORD / REPLAY_CMT_STOP_BUTTON) */
    OVL_ACTION_CMT_MODE_TAPE,  /**< režim načítání Tape (cmt_request_set_loading_mode()) */
    OVL_ACTION_CMT_MODE_BOOST, /**< režim načítání Boost */
    OVL_ACTION_CMT_MODE_DIRECT, /**< režim načítání Direct */
    OVL_ACTION_CMT_CHANGE,     /**< otevřít jinou pásku dialogem souboru (imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_OPEN)) */
    OVL_ACTION_CMT_EJECT,      /**< vysunout pásku (imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT), jako Tape > Eject okna Virtual CMT) */
    OVL_ACTION_CMT_WINDOW,     /**< zavřít overlay a ukázat okno Virtual CMT (g_gui->showVirtualCmtWindow, jako Alt+C) */
    /* Quick Disk */
    OVL_ACTION_QD_INSERT,      /**< vložit Quick Disk (qdisk_ui_mount()) */
    OVL_ACTION_QD_EJECT,       /**< vysunout Quick Disk (qdisk_request_umount()) */
    OVL_ACTION_QD_SAVE,        /**< uložit změny Quick Disku (qdisk_request_sync(), položka "Save now") */
    OVL_ACTION_QD_WP,          /**< přepnout volbu "Write Protected" Quick Disku (qdisk_request_set_write_protected(), položka klasického menu) */
    /* nedávná média */
    OVL_ACTION_BROWSE_FILES,   /**< vybrat obraz diskety do první prázdné mechaniky, jinak do FDD 0 (fdc_ui_mount()) */
    OVL_ACTION_RECENT_CLEAR,   /**< vyprázdnit historii nedávných médií všech druhů (recent_media_clear()); klasické menu obdobu nemá */
    /* tab Machine */
    OVL_ACTION_PAUSE_TOGGLE,   /**< přepnout pauzu emulace (dbg_ui_pause_toggle(), jako Alt+P a položka "Pause Emulation"); stav po zavření overlaye pak určuje uživatel */
    OVL_ACTION_ROM_SELECT,     /**< vybrat ROM (rom_request_select(), položka menu ROM); hodnota = index řádku g_rom_config->roms */
    OVL_ACTION_SPEED_NORMAL,   /**< normální rychlost (emulator_switch_to_normal_speed(), jako Alt+N) */
    OVL_ACTION_SPEED_CUSTOM,   /**< vlastní rychlost (emulator_switch_to_custom_speed(), položka "Custom CPU Speed") */
    OVL_ACTION_SPEED_SET,      /**< nastavit vlastní rychlost (customspeed_set_request(), posuvník "Custom Speed Options"); hodnota = procenta */
    OVL_ACTION_SPEED_DOWN,     /**< zpomalit o 10 % (customspeed_step_down_request(10), položka "Decrease - step 10 %") */
    OVL_ACTION_SPEED_UP,       /**< zrychlit o 10 % (customspeed_step_up_request(10), položka "Increase - step 10 %") */
    OVL_ACTION_SNAPSHOT_SAVE,  /**< uložit snapshot do souboru (snapshot_ui_request_save(), jako Alt+F6); dialog se otevře nad overlayem */
    OVL_ACTION_SNAPSHOT_LOAD,  /**< načíst snapshot ze souboru (snapshot_ui_request_load(), jako Alt+F7); overlay se před ním zavře */
    OVL_ACTION_SNAPSHOT_SETUP, /**< zavřít overlay a ukázat okno Snapshot Settings (g_gui->showSnapshotSetupWindow, položka "Snapshot Setup...") */
    OVL_ACTION_CLASSIC_MENU,   /**< zavřít overlay a otevřít klasické menu (imgui_overlay_request_classic_menu()) */
    /* tab Display */
    OVL_ACTION_STARTUP_SIZE,   /**< velikost okna při startu (display_set_window_startup_scale()); hodnota = en_DISPLAY_STARTUP_SIZE */
    OVL_ACTION_LOCK_ASPECT,    /**< přepnout uzamčení poměru stran okna (display_set_locked_aspect_ratio()) */
    OVL_ACTION_FIX_ASPECT_W,   /**< srovnat poměr stran okna podle šířky (fix_window_aspect_ratio('W'), jako Alt+W) */
    OVL_ACTION_FIX_ASPECT_H,   /**< srovnat poměr stran okna podle výšky (fix_window_aspect_ratio('H'), jako Alt+H) */
    OVL_ACTION_COLORS,         /**< barevné schéma (display_set_colors()); hodnota = en_DISPLAY_COLOR_SCHEMA */
    OVL_ACTION_SCANLINES,      /**< přepnout scanlines CRT (display_set_scanlines()) */
    OVL_ACTION_SCANLINES_INTENSITY, /**< intenzita scanlines (display_set_scanlines_intensity()); hodnota = procenta */
    OVL_ACTION_FRAMERATE,      /**< řízení snímkové frekvence (display_set_framerate_mode()); hodnota = en_DISPLAY_FRAMERATE_MODE */
    OVL_ACTION_CUSTOM_FPS,     /**< vlastní snímková frekvence (g_display.custom_fps a display_set_framerate_mode(CUSTOM), jako posuvník klasického menu); hodnota = Hz */
    OVL_ACTION_FORCED_REDRAW,  /**< přepnout vynucené překreslení každého snímku (display_set_forced_redrawing()) */
    /* tab Sound */
    OVL_ACTION_LISTEN,         /**< přepnout poslech při otevřeném overlayi (imgui_overlay_set_listen(), přepínač Listen); nedostupné, když pauza není overlaye nebo emulace běží z jiného důvodu */
    OVL_ACTION_MASTER_VOLUME,  /**< hlavní hlasitost poslechu (iface_audio_set_master_gain()); hodnota = procenta 0 až 200 */
    OVL_ACTION_MUTE,           /**< přepnout ztlumení poslechu (iface_audio_set_mute()) */
    OVL_ACTION_OUTPUT_FILTER,  /**< přepnout věrnou výstupní propust (iface_audio_set_output_filter(), volba "Hardware output filter" okna Audio Mixer) */
    OVL_ACTION_CHANNEL_VOLUME, /**< hlasitost kanálu (zápis g_iface_audio.gain[] jako posuvník okna Audio Mixer); hodnota = OVL_CHANNEL_VOLUME_VALUE(kanál, procenta) */
    OVL_ACTION_CHANNELS_RESET, /**< hlasitosti všech kanálů na 100 % (výchozí stav g_iface_audio.gain[]) */
    /* tab Input */
    OVL_ACTION_VKBD,           /**< zavřít overlay a ukázat virtuální klávesnici (g_gui->showVirtualKeyboardWindow, jako Alt+K) */
    OVL_ACTION_AUTOTYPE,       /**< zavřít overlay a ukázat okno Autotype Buffer (g_gui->showAutotypeWindow) */
    OVL_ACTION_JOY_DEVICE,     /**< zařízení prvního portu joysticku (joy_request_set_type() a otevření nebo zavření zařízení jako "Apply" okna Joystick Setup); hodnota = en_OVERLAY_JOY_TYPE */
    OVL_ACTION_JOY_SETUP,      /**< zavřít overlay a ukázat okno Joystick Setup (g_gui->showJoystickSetupWindow) */
    /* tab Capture */
    OVL_ACTION_SCREENSHOT_AS,  /**< uložit screenshot do zvoleného souboru (screenshot_ui_save_as()); dialog se otevře nad overlayem */
    OVL_ACTION_SERIES,         /**< spustit nebo ukončit sérii snímků (screenshot_ui_toggle_series(), jako Alt+Shift+F) */
    OVL_ACTION_SCREENSHOT_FOLDER, /**< otevřít výstupní adresář screenshotů (screenshot_ui_open_output_folder()) */
    OVL_ACTION_SCREENSHOT_SETUP,  /**< zavřít overlay a ukázat nastavení screenshotů (g_gui->showScreenshotSetupWindow) */
    OVL_ACTION_VIDEO_TOGGLE,   /**< spustit nebo ukončit video záznam (videorec_ui_toggle_recording(), jako Alt+O) */
    OVL_ACTION_VIDEO_PAUSE,    /**< pozastavit nebo obnovit video záznam (videorec_ui_toggle_pause(), jako Alt+Shift+O) */
    OVL_ACTION_VIDEO_REALTIME, /**< přepnout časovou základnu záznamu (videorec_ui_toggle_timebase(), jako Alt+U) */
    OVL_ACTION_VIDEO_FOLDER,   /**< otevřít výstupní adresář video záznamu (videorec_ui_open_output_folder()) */
    OVL_ACTION_VIDEO_SETUP,    /**< zavřít overlay a ukázat nastavení video záznamu (g_gui->showVideorecSetupWindow) */
    OVL_ACTION_REPLAY_RECORD,  /**< dialog startu záznamu vstupů (imgui_replay_open_record_dialog()); overlay se před ním zavře */
    OVL_ACTION_REPLAY_PLAY,    /**< dialog přehrání záznamu vstupů (imgui_replay_open_play_dialog()); overlay se před ním zavře */
    OVL_ACTION_REPLAY_STOP,    /**< zastavit záznam nebo přehrávání vstupů (replay_request_stop() s ohlášením výsledku, položka "Stop Replay") */
    OVL_ACTION_REPLAY_WINDOW,  /**< zavřít overlay a ukázat okno Replay Control (g_gui->showReplayWindow, jako Alt+J) */
    OVL_ACTION_COUNT           /**< počet akcí (není akce) */
} en_OVL_ACTION;

/**
 * @brief Hodnota akce OVL_ACTION_CHANNEL_VOLUME: kanál a hlasitost v jednom čísle.
 * @param channel Index kanálu (0 = CTC0; pořadí g_iface_audio.gain[]).
 * @param percent Hlasitost v procentech 0 až 100.
 */
#define OVL_CHANNEL_VOLUME_VALUE(channel, percent) ((channel) * 1000 + (percent))

/**
 * @brief Hodnota st_OVL_ACTION_DEF::arg akce s hodnotou.
 *
 * Funkce run takové akce dostane místo argumentu hodnotu předanou
 * overlay_action_request_value(); funkce reason dostane tuto konstantu
 * (dostupnost na hodnotě nezávisí).
 */
#define OVL_ACTION_ARG_VALUE (-0x7fffffff - 1)

/** @brief Co se stane s overlayem při spuštění akce. */
typedef enum en_OVL_ACTION_CLOSE {
    OVL_ACTION_CLOSE_NO = 0, /**< overlay zůstává otevřený (modální dialog akce se otevře nad ním) */
    OVL_ACTION_CLOSE_BEFORE, /**< overlay se zavře a obnoví stav pauzy, potom se akce provede (akce, která si pauzu řídí sama) */
    OVL_ACTION_CLOSE_AFTER,  /**< akce se provede a potom se overlay zavře (okno nástroje, reset) */
    OVL_ACTION_CLOSE_RESUME, /**< akce = zavřít overlay a rozběhnout emulaci bez ohledu na stav před otevřením */
} en_OVL_ACTION_CLOSE;

/**
 * @brief Funkce akce.
 * @param v   Kopie stavu z posledního plnění (platná po dobu volání).
 * @param arg Argument z řádku tabulky (číslo mechaniky, režim).
 * @pre UI vlákno, mimo Begin() / End(), volající nedrží device_state_lock().
 */
typedef void (*overlay_action_run_fn)(const st_OVERLAY_VIEW *v, int arg);

/**
 * @brief Dostupnost akce.
 * @param v   Kopie stavu.
 * @param arg Argument z řádku tabulky.
 * @return NULL = akce je dostupná; jinak přeložený důvod, proč není (platný do dalšího volání).
 */
typedef const char *(*overlay_action_reason_fn)(const st_OVERLAY_VIEW *v, int arg);

/**
 * @brief Řádek tabulky akcí.
 *
 * Invarianty: řádek s indexem i má id == i; chord == 0 <=> keycap == NULL;
 * run není NULL (kromě řádku OVL_ACTION_NONE); reason smí být NULL (akce
 * je dostupná vždy).
 */
typedef struct st_OVL_ACTION_DEF {
    en_OVL_ACTION id;                /**< akce, které řádek patří */
    const char *label;               /**< anglický popisek (msgid; překládá se při kreslení); NULL = prvek bez textu */
    const char *icon;                /**< ikona (ICON_LC_* nebo OVL_ICON_EJECT); NULL = bez ikony */
    ImGuiKeyChord chord;             /**< klávesová zkratka v okně overlaye (stejná jako globální zkratka emulátoru); 0 = žádná */
    const char *keycap;              /**< text štítku zkratky (názvy kláves se nepřekládají); NULL = žádná zkratka */
    overlay_action_run_fn run;       /**< funkce akce */
    int arg;                         /**< argument funkcí run a reason */
    overlay_action_reason_fn reason; /**< dostupnost; NULL = vždy dostupná */
    en_OVL_ACTION_CLOSE close;       /**< chování overlaye při spuštění */
} st_OVL_ACTION_DEF;

/**
 * @brief Řádek tabulky akcí.
 * @param id Akce.
 * @return Řádek; pro hodnotu mimo rozsah řádek OVL_ACTION_NONE. Nikdy NULL.
 */
const st_OVL_ACTION_DEF *overlay_action_def(en_OVL_ACTION id);

/**
 * @brief Důvod, proč akce není dostupná.
 * @param id Akce.
 * @param v  Kopie stavu; NULL = akce není dostupná.
 * @return NULL = akce je dostupná; jinak přeložený text pro tooltip (smí být prázdný; platný do dalšího volání).
 */
const char *overlay_action_reason(en_OVL_ACTION id, const st_OVERLAY_VIEW *v);

/**
 * @brief Stav přepínače "Write protect" disketové mechaniky.
 *
 * Zapnutý je při volbě uživatele (platí i pro prázdnou mechaniku), u obrazu,
 * jehož soubor nejde zapisovat, a u obrazu s ochranou kvůli sdílení s jinou
 * mechanikou. Podle něj akce OVL_ACTION_FDD_WP_* volí, zda ochranu zapíná
 * nebo vypíná.
 *
 * @param d Mechanika v kopii stavu (ne NULL).
 * @return true = přepínač je zapnutý.
 */
bool overlay_fdd_wp_on(const st_OVERLAY_VIEW_FDD *d);

/**
 * @brief Text štítku zkratky, který má prvek akce ukázat.
 * @param id Akce.
 * @param v  Kopie stavu.
 * @return Text zkratky, je-li akce dostupná a zkratku má; jinak NULL (štítek znamená "funguje právě teď").
 */
const char *overlay_action_keycap(en_OVL_ACTION id, const st_OVERLAY_VIEW *v);

/**
 * @brief Vyžádá spuštění akce po vykreslení overlaye.
 *
 * Do provedení se drží jediná vyžádaná akce; další požadavek ve stejném
 * snímku se ignoruje.
 *
 * @param id Akce; OVL_ACTION_NONE, hodnoty mimo rozsah a akce s hodnotou (OVL_ACTION_ARG_VALUE) se ignorují.
 */
void overlay_action_request(en_OVL_ACTION id);

/**
 * @brief Vyžádá spuštění akce s hodnotou po vykreslení overlaye.
 *
 * Jako overlay_action_request(); hodnotu dostane funkce akce jako svůj
 * argument. Hodnotu si akce ověřuje sama (rozsah, platnost).
 *
 * @param id    Akce s arg == OVL_ACTION_ARG_VALUE; jiná akce se ignoruje.
 * @param value Hodnota (index řádku ROM, procenta, Hz, hodnota výčtu podle akce).
 */
void overlay_action_request_value(en_OVL_ACTION id, int value);

/**
 * @brief Tlačítko akce z tabulky akcí.
 *
 * Ikonu a zkratku bere z tabulky; nedostupnou akci kreslí jako zakázanou a
 * v tooltipu ukáže důvod. Tlačítko jen s ikonou má v tooltipu popisek a
 * zkratku. Zvolení akci vyžádá (overlay_action_request()); akce s hodnotou
 * tímto tlačítkem vyžádat nejde.
 *
 * @param id        Akce.
 * @param v         Kopie stavu.
 * @param text      Přeložený popisek (u tlačítka jen s ikonou text tooltipu); NULL = přeložený popisek z tabulky.
 * @param icon_only true = kreslit jen ikonu.
 * @param kind      Druh tlačítka.
 * @param size      Velikost jako u ovl_button_ex().
 * @param test_name Jméno obdélníku pro testy (overlay_test_note_item()); NULL = nezaznamenávat.
 * @return true = tlačítko bylo zvoleno a akce vyžádána.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push(); kurzor okna stojí na pozici tlačítka.
 */
bool overlay_action_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name);

/**
 * @brief Tlačítko akce z tabulky akcí s rozhodnutím skupiny o štítku zkratky.
 *
 * Jako overlay_action_button(); navíc volající říká, zda skupina tlačítek,
 * do které tlačítko patří, štítky zkratek ukazuje (overlay_action_keycaps_fit()).
 * Skrytá zkratka dostupné akce je v tooltipu za popiskem. Zakázaná akce
 * štítek nekreslí nikdy; šířku tlačítka určuje volající, takže místo po
 * štítku zůstává a skupina se při změně stavu nepřeskládá.
 *
 * @param id          Akce.
 * @param v           Kopie stavu.
 * @param text        Přeložený popisek; NULL = přeložený popisek z tabulky.
 * @param icon_only   true = kreslit jen ikonu (štítek se pak nekreslí nikdy).
 * @param kind        Druh tlačítka.
 * @param size        Velikost jako u ovl_button_ex().
 * @param test_name   Jméno obdélníku pro testy; NULL = nezaznamenávat. Kreslený štítek se zaznamená pod jménem s příponou "_keycap".
 * @param show_keycap true = skupina štítky ukazuje; false = štítek se nekreslí a zkratka jde do tooltipu.
 * @return true = tlačítko bylo zvoleno a akce vyžádána.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push(); kurzor okna stojí na pozici tlačítka.
 */
bool overlay_action_button_ex(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name,
                              bool show_keycap);

/**
 * @brief Vejdou se štítky zkratek do všech tlačítek skupiny?
 *
 * Pravidlo skupiny (posudek N02): v jedné řadě nebo skupině tlačítek mají
 * štítek zkratky všechny dostupné akce, nebo žádná. Rozhoduje se ze zkratek
 * v tabulce akcí, jako by byly všechny akce dostupné, takže výsledek
 * nezávisí na tom, která akce je právě zakázaná.
 *
 * @param ids    Akce skupiny.
 * @param texts  Přeložené popisky tlačítek; NULL nebo NULL prvek = popisek z tabulky.
 * @param widths Šířky tlačítek v pixelech.
 * @param count  Počet tlačítek.
 * @return true = každé tlačítko se zkratkou má šířku nejméně pro ikonu, popisek a štítek; false i pro prázdnou skupinu.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push() (měří se písmem overlaye).
 */
bool overlay_action_keycaps_fit(const en_OVL_ACTION *ids, const char *const *texts, const float *widths, int count);

/**
 * @brief Čeká v tomto snímku vyžádaná akce na provedení?
 *
 * Prvek, který by ve stejném snímku udělal něco dalšího (otevření
 * výsuvného listu klávesou Enter stisknutou jako součást zkratky), podle
 * toho ustoupí: v jednom snímku se stane jen jedna věc.
 *
 * @return true = overlay_action_request() v tomto snímku přijal akci.
 */
bool overlay_action_is_pending(void);

/**
 * @brief Obslouží klávesové zkratky akcí v okně overlaye.
 *
 * Projde tabulku a dostupnou akci, jejíž zkratka byla právě stisknuta
 * (klávesa bez opakování, přesně modifikátory zkratky), vyžádá
 * (overlay_action_request()). Zkratky s Alt, Ctrl a F12 nekolidují s
 * číslicemi tabů ani s navigací ImGui.
 *
 * @param v Kopie stavu.
 * @pre Uvnitř okna overlaye, které má fokus; ne při psaní do textového pole.
 */
void overlay_actions_handle_shortcuts(const st_OVERLAY_VIEW *v);

/**
 * @brief Provede vyžádanou akci.
 *
 * Dostupnost ověří znovu (nedostupná akce se zahodí). Podle
 * st_OVL_ACTION_DEF::close overlay zavře před akcí nebo po ní; u
 * OVL_ACTION_CLOSE_RESUME přepíše zapamatovaný stav pauzy
 * (imgui_overlay_note_pause_changed(false)) a zavře.
 *
 * @param v Kopie stavu, ze které se v tomto snímku kreslilo.
 * @pre UI vlákno, mimo Begin() / End() okna overlaye, volající nedrží device_state_lock().
 * @post Žádná akce nečeká.
 */
void overlay_actions_run_pending(const st_OVERLAY_VIEW *v);

/**
 * @brief Vlastní rychlost, kterou by volba "Custom" vrátila z normální rychlosti.
 *
 * Přednost má rychlost, kterou si overlay zapamatoval při své poslední
 * volbě "Normal" (požadovaná, tedy i nastavená při stojící emulaci), pokud
 * od té doby nikdo jiný nezměnil rychlost zapamatovanou jádrem; jinak
 * rychlost zapamatovaná jádrem (previous_speed_in_percentage).
 *
 * @return Rychlost v procentech (jiná než 100); 0 = není co vrátit.
 * @pre UI vlákno; čte g_customspeed (při plnění kopie stavu pod device_state_lock()).
 */
int overlay_action_custom_speed_restore_value(void);

/**
 * @brief Jen pro testy: vyzvedne naposledy provedenou akci.
 * @return Akce provedená od posledního volání (poslední z nich), jinak OVL_ACTION_NONE; voláním se zapomene.
 */
en_OVL_ACTION overlay_actions_test_take_last(void);

/**
 * @brief Jen pro testy: hodnota, se kterou byla provedena poslední akce s hodnotou.
 * @return Hodnota z posledního provedení akce s arg == OVL_ACTION_ARG_VALUE; 0, když žádná neproběhla.
 */
int overlay_actions_test_last_value(void);

/**
 * @brief Jen pro testy: počet nepovedených otevření zařízení joysticku volbou "Joystick".
 * @return Počet oznámených neúspěchů od posledního volání; voláním se vynuluje.
 */
int overlay_actions_test_take_joy_open_failures(void);

#endif /* OVERLAY_ACTIONS_H */
