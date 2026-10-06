/**
 * @file   videorec_menu.h
 * @brief  UI video záznamu: podmenu Tools -> Video Recording, akce zkratek,
 *         REC indikátor, notifikace událostí, dialog nastavení a okno
 *         dálkového ovládání (videorec_remote_window.cpp).
 *
 * Vrstva jen volá API lepidla videorec.h (požadavky se v jádře provedou na
 * nejbližším konci emulovaného snímku, stop i v pauze emulace). Všechny
 * funkce volá výhradně UI (hlavní) vlákno uvnitř ImGui snímku.
 *
 * @par Licence: GPLv3
 */

#ifndef VIDEOREC_MENU_H
#define VIDEOREC_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emulator/videorec/videorec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stav položek podmenu Video Recording (čistá logika, bez ImGui).
 *
 * Řetězce jsou anglické zdroje (gettext msgid, značené N_()); UI je
 * překládá přes `_L()`.
 *
 * Invarianty: `pause_enabled == marker_enabled`; `pause_checked` => `pause_enabled`;
 * `start_stop_enabled` je vždy true (video záznam je na všech platformách).
 */
typedef struct st_VIDEOREC_UI_MENU_MODEL {
    const char *start_stop_label; /**< "Start Recording" (nenahrává se) nebo "Stop Recording" (nahrává se nebo čeká start). */
    bool start_stop_enabled;      /**< Položku Start/Stop lze použít. */
    bool pause_enabled;           /**< Pauza nahrávání má smysl (stav RECORDING nebo PAUSED). */
    bool pause_checked;           /**< Nahrávání je v record-pause. */
    bool marker_enabled;          /**< Marker lze vložit (stav RECORDING nebo PAUSED). */
} st_VIDEOREC_UI_MENU_MODEL;

/**
 * @brief Klávesové zkratky video záznamu.
 *
 * Jediný zdroj pravdy: tabulka v videorec_menu.cpp drží pro každou zkratku
 * klávesu, modifikátor Shift a text popisku. Z ní čte obsluha zkratek
 * (imgui_videorec_shortcuts(), volaná z global_shortcuts.cpp), podmenu
 * i okno dálkového ovládání, takže se popisky nemohou rozejít se skutečnou
 * obsluhou. Všechny zkratky jsou Alt + klávesa (případně + Shift).
 */
typedef enum {
    VIDEOREC_SHORTCUT_TOGGLE_RECORDING = 0, /**< Start / Stop nahrávání (Alt+O). */
    VIDEOREC_SHORTCUT_TOGGLE_PAUSE,          /**< Pozastavit / pokračovat v nahrávání (Alt+Shift+O). */
    VIDEOREC_SHORTCUT_ADD_MARKER,            /**< Vložit marker "Marker N" (Alt+L). */
    VIDEOREC_SHORTCUT_REMOTE_WINDOW,         /**< Zobrazit / skrýt okno dálkového ovládání (Alt+Shift+L). */
    VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE,       /**< Přepnout časovou základnu emulační čas / podle reality (Alt+U). */
    VIDEOREC_SHORTCUT_COUNT                  /**< Počet zkratek (není zkratka). */
} en_VIDEOREC_SHORTCUT;

/**
 * @brief Text zkratky pro zobrazení (např. "Alt+Shift+O").
 * @param id Zkratka.
 * @return Statický řetězec (nepřekládá se); "" pro neplatné `id`.
 */
const char *videorec_ui_shortcut_label(en_VIDEOREC_SHORTCUT id);

/**
 * @brief Klávesa a modifikátor zkratky (pro test shody s popiskem).
 * @param id        Zkratka.
 * @param imgui_key Výstup: hodnota ImGuiKey (nesmí být NULL); pro neplatné `id` 0.
 * @param shift     Výstup: vyžaduje Shift (nesmí být NULL).
 */
void videorec_ui_shortcut_keys(en_VIDEOREC_SHORTCUT id, int *imgui_key, bool *shift);

/**
 * @brief Obslouží klávesové zkratky video záznamu (Alt+O, Alt+Shift+O, Alt+L, Alt+Shift+L, Alt+U).
 *
 * Pro každou zkratku z tabulky: je-li držen Alt, stav Shiftu odpovídá a klávesa
 * byla v tomto snímku stisknuta (bez autorepeat), provede akci zkratky.
 *
 * @pre Uvnitř ImGui snímku (UI vlákno); volá global_shortcuts.cpp.
 * @par Side effects Viz akce videorec_ui_toggle_recording(),
 *      videorec_ui_toggle_pause(), videorec_ui_add_marker(),
 *      videorec_ui_toggle_timebase(); Alt+Shift+L přepne
 *      `g_gui->showVideorecRemoteWindow`.
 */
void imgui_videorec_shortcuts(void);

/**
 * @brief Akce přepnout časovou základnu nahrávání (Alt+U, menu, okno dálkového ovládání).
 *
 * Přečte požadovanou základnu ze souhrnného stavu (videorec_get_status())
 * a požádá o opačnou přes videorec_ui_set_timebase().
 *
 * @pre UI vlákno. Na nepodporované platformě no-op.
 */
void videorec_ui_toggle_timebase(void);

/**
 * @brief Nastaví časovou základnu nahrávání a ohlásí ji notifikací.
 *
 * Volá videorec_request_timebase(): platí hned pro běžící (nebo čekající)
 * nahrávku - přepnutí je hranice segmentu - i pro příští start (nastavení
 * se uloží do INI). Notifikace "Recording time base: ..." se zobrazí, jen
 * když není otevřené chybové okno; text se zapamatuje i jako poslední
 * událost (videorec_ui_last_event()).
 *
 * @param tb Požadovaná časová základna.
 * @pre UI vlákno. Na nepodporované platformě no-op (bez notifikace).
 * @par Side effects Mění g_videorec_settings.timebase.
 */
void videorec_ui_set_timebase(en_VIDEOREC_TIMEBASE tb);

/**
 * @brief Anglický msgid názvu časové základny ("Emulated time" / "Real time").
 * @param tb Časová základna; jiná hodnota = "Emulated time".
 * @return Statický msgid (přeložit přes `_()` / `_L()`).
 */
const char *videorec_ui_timebase_msgid(en_VIDEOREC_TIMEBASE tb);

/**
 * @brief Anglický msgid popisu časové základny pro řádek stavu okna dálkového ovládání.
 *
 * Bez nahrávání (`!active`) popisuje nastavení pro příští start. Při
 * nahrávání ukazuje skutečnou základnu a v realtime i činnost vzorkovače
 * (živě / zamrzlý obraz / nezapisuje se); požadovaný realtime, který zatím
 * není efektivní (přepínání, `emulated_when_fast` při rychlosti != 100 %),
 * má vlastní text.
 *
 * @param st     Souhrnný stav (nesmí být NULL).
 * @param active Nahrává se nebo je nahrávání pozastavené (stav != IDLE).
 * @return Statický msgid.
 */
const char *videorec_ui_timebase_status_msgid(const st_VIDEOREC_STATUS *st, bool active);

/**
 * @brief Složí text REC indikátoru: "REC HH:MM:SS · <režim>" (přeložený).
 *
 * Režim je skutečná časová základna ("emulated" / "real-time");
 * v pozastaveném nahrávání začíná text "PAUSE".
 *
 * @param st   Souhrnný stav (nesmí být NULL; stav != IDLE).
 * @param buf  Výstup (nesmí být NULL).
 * @param size Velikost výstupu (doporučeno >= 96); delší text se zkrátí.
 */
void videorec_ui_overlay_text(const st_VIDEOREC_STATUS *st, char *buf, size_t size);

/**
 * @brief Předvolby nastavení režimu nahrávání (dialog nastavení).
 *
 * Předvolba nastaví jen klíče své definice; ostatní hodnoty se nemění
 * a všechny zůstávají dál ručně upravitelné.
 */
typedef enum {
    VIDEOREC_UI_PRESET_GAMEPLAY = 0, /**< "Gameplay showcase": emulační čas, pauza = skip, značky stavu do sidecaru. */
    VIDEOREC_UI_PRESET_LIVE,         /**< "Live / tutorial": podle reality, pauza = freeze_capped, značky stavu do sidecaru (export s `--state-overlay icons`). */
    VIDEOREC_UI_PRESET_COUNT         /**< Počet předvoleb (není předvolba). */
} en_VIDEOREC_UI_PRESET;

/**
 * @brief Aplikuje předvolbu na nastavení.
 * @param s Nastavení (nesmí být NULL); mění se jen `timebase`, `realtime_pause` a `state_marks`.
 * @param p Předvolba; neplatná hodnota = no-op.
 */
void videorec_ui_apply_preset(st_VIDEOREC_SETTINGS *s, en_VIDEOREC_UI_PRESET p);

/**
 * @brief Odpovídá nastavení předvolbě (všechny klíče, které předvolba nastavuje)?
 * @param s Nastavení (nesmí být NULL).
 * @param p Předvolba; neplatná hodnota = false.
 * @return true, když by videorec_ui_apply_preset() nic nezměnila.
 */
bool videorec_ui_preset_matches(const st_VIDEOREC_SETTINGS *s, en_VIDEOREC_UI_PRESET p);

/**
 * @brief Anglický msgid názvu předvolby ("Gameplay showcase", "Live / tutorial").
 * @param p Předvolba; neplatná hodnota = "".
 * @return Statický msgid.
 */
const char *videorec_ui_preset_msgid(en_VIDEOREC_UI_PRESET p);

/**
 * @brief Potvrzení dialogu nastavení (OK): zapíše upravené nastavení do g_videorec_settings.
 *
 * Časová základna se přepíná i za běhu (Alt+U, okno, MCP), proto ji OK
 * mění jen tehdy, když na ni uživatel v dialogu sáhl (přepínač nebo
 * předvolba, `timebase_touched`): pak se přepne přes videorec_ui_set_timebase()
 * (platí hned, i pro běžící nahrávku), pokud se liší od aktuální hodnoty.
 * Jinak zůstane aktuální g_videorec_settings.timebase. Ostatní hodnoty platí
 * od dalšího startu nahrávání.
 *
 * @param edited           Upravená kopie nastavení (nesmí být NULL).
 * @param timebase_touched Uživatel v dialogu změnil časovou základnu (přepínač / předvolba).
 * @pre UI vlákno.
 */
void videorec_ui_settings_apply(const st_VIDEOREC_SETTINGS *edited, bool timebase_touched);

/**
 * @brief Srovná časovou základnu kopie v dialogu s aktuální hodnotou (každý snímek dialogu).
 *
 * Dokud uživatel na časovou základnu v dialogu nesáhl, kopie sleduje
 * g_videorec_settings.timebase, takže přepínač i "Current settings" ukazují
 * hodnotu přepnutou za běhu (Alt+U). Dotčenou hodnotu nemění.
 *
 * @param edited           Kopie nastavení v dialogu (nesmí být NULL).
 * @param timebase_touched Uživatel v dialogu změnil časovou základnu.
 * @pre UI vlákno.
 */
void videorec_ui_setup_sync_timebase(st_VIDEOREC_SETTINGS *edited, bool timebase_touched);

/**
 * @brief Spočítá stav položek podmenu ze stavu nahrávání.
 * @param state         Stav nahrávání (videorec_get_state()).
 * @param start_pending Start byl přijat, ale ještě nezpracován (čeká na konec snímku).
 * @return Model položek (řetězce jsou statické literály).
 */
st_VIDEOREC_UI_MENU_MODEL videorec_ui_menu_model(en_VIDEOREC_STATE state, bool start_pending);

/**
 * @brief Převede počet snímků na čas `HH:MM:SS` (zaokrouhleno dolů).
 * @param frames Počet snímků / index snímku.
 * @param fps    Snímková frekvence nahrávky (videorec_get_fps(): 50 nebo 60); 0 se nahradí 1.
 * @param buf    Výstup (nesmí být NULL).
 * @param size   Velikost výstupu (doporučeno >= 16); delší text se zkrátí.
 */
void videorec_ui_format_time(uint64_t frames, unsigned fps, char *buf, size_t size);

/**
 * @brief Akce Start / Stop (Alt+O, menu).
 *
 * Nenahrává-li se a nečeká start: požádá o start s vygenerovaným jménem
 * a úrovněmi kanálů z iface_audio_build_videorec_levels(); chyba startu
 * se zobrazí jako chybová notifikace; při pozastavené emulaci se zobrazí
 * upozornění, že nahrávání začne po jejím obnovení. Jinak požádá o stop
 * (čekající start tím zruší).
 *
 * @par Side effects Může vytvořit soubor nahrávky a adresář výstupu.
 */
void videorec_ui_toggle_recording(void);

/** @brief Akce pozastavit / pokračovat v nahrávání (Alt+Shift+O); bez nahrávání no-op. */
void videorec_ui_toggle_pause(void);

/**
 * @brief Akce vložit marker (Alt+L) s popiskem "Marker N".
 *
 * N se čísluje od 1 v rámci session nahrávání (při nové session se čítač
 * vynuluje). Bez nahrávání no-op. Ekvivalent videorec_ui_add_marker_label(NULL).
 */
void videorec_ui_add_marker(void);

/**
 * @brief Akce vložit marker s vlastním popiskem (okno dálkového ovládání).
 *
 * Každý marker (i s vlastním popiskem) zvýší čítač N session, takže
 * výchozí popisek "Marker N" odpovídá pořadí markeru v nahrávce.
 *
 * @param label Popisek (zkopíruje se do sidecaru beze změny); NULL nebo
 *              prázdný řetězec = "Marker N". Bez nahrávání no-op.
 */
void videorec_ui_add_marker_label(const char *label);

/**
 * @brief Je přijatý start nahrávání, který jádro ještě nezpracovalo (pohled UI)?
 * @return Stav "čeká start" UI vrstvy (viz imgui_videorec_poll_events()).
 */
bool videorec_ui_is_start_pending(void);

/**
 * @brief Text poslední události nahrávání ve stejné (přeložené) podobě jako notifikace.
 *
 * Plní ho imgui_videorec_poll_events() při každé události (start, uložení,
 * chyba, retake, šev) a videorec_ui_toggle_recording() při chybě startu.
 *
 * @param is_error Výstup (smí být NULL): poslední událost je chyba.
 * @return Text (UTF-8, platí do další události); "" = zatím žádná událost.
 */
const char *videorec_ui_last_event(bool *is_error);

/**
 * @brief Anglický msgid popisku režimu retake (pro Combo v dialogu i v okně).
 * @param mode Režim (en_VIDEOREC_RETAKE); hodnota mimo rozsah = popisek DISCARD.
 * @return Statický msgid (přeložit přes `_()` / `_L()`).
 */
const char *videorec_ui_retake_mode_msgid(int mode);

/** @brief Počet režimů retake (hodnoty en_VIDEOREC_RETAKE 0 .. VIDEOREC_UI_RETAKE_MODES-1) pro Combo. */
#define VIDEOREC_UI_RETAKE_MODES 3

/**
 * @brief Otevře výstupní adresář nahrávek v systémovém správci souborů.
 *
 * Adresář určí videorec_resolve_output_dir() (případně ho vytvoří). Chyba se
 * ohlásí chybovou notifikací.
 */
void videorec_ui_open_output_folder(void);

/**
 * @brief Popisek a barva stavu nahrávání v okně dálkového ovládání (čistá logika).
 *
 * Invarianty: `state_label` je statický anglický msgid.
 */
typedef struct st_VIDEOREC_UI_REMOTE_MODEL {
    const char *state_label;           /**< "IDLE", "STARTING", "REC" nebo "PAUSED" (msgid, přeložit `_()`). */
    unsigned state_rgba;               /**< Barva stavu jako IM_COL32 (šedá / oranžová / červená / žlutá). */
    const char *pause_label;           /**< "Pause Recording" nebo "Resume Recording" (msgid). */
    st_VIDEOREC_UI_MENU_MODEL actions; /**< Povolení akcí (stejná pravidla jako podmenu). */
} st_VIDEOREC_UI_REMOTE_MODEL;

/**
 * @brief Spočítá model okna dálkového ovládání ze souhrnného stavu.
 * @param st            Souhrnný stav (videorec_get_status(); nesmí být NULL).
 * @param start_pending Stav "čeká start" UI (videorec_ui_is_start_pending()).
 * @return Model (řetězce jsou statické literály).
 */
st_VIDEOREC_UI_REMOTE_MODEL videorec_ui_remote_model(const st_VIDEOREC_STATUS *st, bool start_pending);

/**
 * @brief Vykreslí okno dálkového ovládání nahrávání (`###VideorecRemote`).
 *
 * Zobrazí se, když je `g_gui->showVideorecRemoteWindow`. Obsahuje tlačítka
 * Start/Stop, Pause/Resume, Add Marker (s volitelným popiskem), u každého
 * viditelný text klávesové zkratky (videorec_ui_shortcut_label()), stav
 * nahrávání, čas a počet snímků videa, velikost souborů, aktuální segment,
 * časovou základnu (videorec_ui_timebase_status_msgid()), poslední událost,
 * přepínač časové základny emulační čas / podle reality s viditelnou zkratkou
 * (platí hned, i během nahrávání), přepínač režimu retake (platí od dalšího
 * startu) a tlačítka Open Output Folder a Settings... Na nepodporované
 * platformě ukáže hlášku a ovládání je zakázané.
 *
 * Stav čte jen přes videorec_get_status() - nečeká na emu vlákno. Akce
 * (tlačítka) volají stejné funkce jako zkratky a menu.
 *
 * @pre UI vlákno uvnitř ImGui snímku.
 */
void imgui_videorec_remote_window(void);

/**
 * @brief Zaregistruje persistenci viditelnosti okna (sekce `[VIDEOREC_UI]`, klíč `remote_window`).
 *
 * Načte hodnotu z INI do interní cache (g_gui ještě nemusí existovat).
 * Uložení: viditelnost z g_gui, pokud byla cache aplikovaná
 * (imgui_videorec_remote_apply_persisted()), jinak hodnota cache - headless
 * běh tak uloženou hodnotu nepřepíše.
 *
 * @pre cfgmain_init() proběhl (g_cfgmain existuje). Opakované volání je no-op.
 */
void imgui_videorec_remote_cfg_init(void);

/**
 * @brief Aplikuje načtenou viditelnost okna do `g_gui->showVideorecRemoteWindow`.
 * @pre g_gui existuje (GUI režim) a proběhl imgui_videorec_remote_cfg_init().
 */
void imgui_videorec_remote_apply_persisted(void);

/**
 * @brief Vykreslí podmenu Tools -> Video Recording.
 *
 * Položky: Start/Stop, Pause Recording, Add Marker, Record in Real Time
 * (zaškrtávací, přepíná časovou základnu - Alt+U), Settings..., Open Output
 * Folder a Remote Control...
 *
 * @pre Volá se uvnitř otevřeného menu (mezi ImGui::BeginMenu() a EndMenu()).
 *      Na nepodporované platformě jsou položky zakázané s tooltipem.
 */
void imgui_videorec_menu(void);

/**
 * @brief Vykreslí dialog nastavení video záznamu (okno `###VideorecSetup`).
 *
 * Zobrazí se, když je `g_gui->showVideorecSetupWindow`. Edituje lokální
 * kopii g_videorec_settings (včetně sekce režimu podle reality s předvolbami
 * Gameplay showcase a Live / tutorial), OK ji aplikuje přes
 * videorec_ui_settings_apply() (uloží se do INI při ukládání konfigurace),
 * Cancel / zavření ji zahodí. Změny platí od dalšího startu nahrávání,
 * změněná časová základna se přepne hned.
 */
void imgui_videorec_setup_dialog(void);

/**
 * @brief Vykreslí REC indikátor v pravém horním rohu obrazu emulátoru.
 *
 * Při stavu != IDLE nakreslí do foreground draw listu kruh a text
 * `REC HH:MM:SS · režim` (nebo `PAUSE ...`, viz videorec_ui_overlay_text();
 * režim = skutečná časová základna). Stav čte přes videorec_get_status()
 * (nečeká na emu vlákno). Indikátor je jen v UI, do nahrávky se nedostane
 * (záznam bere framebuffer emulátoru, v realtime poslední zobrazený framebuffer - bez UI).
 *
 * @pre Volá se hned po ImGui::Image() s obrazem emulátoru: oblast obrazu
 *      se bere z GetItemRectMin() / GetItemRectMax().
 */
void imgui_videorec_overlay(void);

/**
 * @brief Zpracuje nové události nahrávání a zobrazí k nim notifikace.
 *
 * Vyzvedne všechny události od naposledy zpracované (videorec_get_event())
 * a zobrazí přeložené texty přes snapshot_notification_show_ex(): start,
 * uložení (s cestou), chyba (modální okno), retake (s časem), šev.
 * Běžná notifikace se nezobrazí, dokud je otevřené chybové okno
 * (snapshot_notification_error_active()). Událost STARTED nebo FAILED
 * ukončí stav "čeká start"; ten se ukončí i tehdy, když jádro start už
 * nečeká (videorec_is_start_pending()) a nenahrává se.
 * Volá se jednou za UI snímek.
 */
void imgui_videorec_poll_events(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOREC_MENU_H */
