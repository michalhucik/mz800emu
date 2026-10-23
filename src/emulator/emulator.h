#ifndef EMULATOR_H
#define EMULATOR_H

#include "app/app.h"
#include <glib.h>

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

#include "customspeed.h"

/**
 * @brief Důvod posledního pozastavení emulace.
 *
 * Nastavuje ho ten, kdo pauzu vyvolal, na emu vlákně před
 * @c emulator_pause(true). Čte ho záznam zastavení (run_control.h) na vstupu
 * do pauzy a MCP `emu_run` (pole `stopped_by`). Při každém rozběhu nebo
 * kroku z pauzy se vynuluje na @c NONE (run_control_on_resume()), takže
 * hodnota z minulé pauzy nepřežije do další. Pauza bez nastaveného důvodu
 * (klávesa Pause v GUI z UI vlákna, příkaz fronty, který emulaci jen
 * pozastaví) se v záznamu zastavení hlásí jako @c MANUAL, dokončený krok
 * jako @c STEP.
 */
typedef enum en_EMU_PAUSE_REASON
{
    EMU_PAUSE_REASON_NONE = 0,    /**< Bez konkrétního důvodu / běží. */
    EMU_PAUSE_REASON_FRAMES,      /**< Doběhl požadovaný počet framů (emu_run). */
    EMU_PAUSE_REASON_BREAKPOINT,  /**< Zastaveno breakpointem. */
    EMU_PAUSE_REASON_MANUAL,      /**< Manuální pauza (UI nebo MCP pause). */
    EMU_PAUSE_REASON_RUN_TO,      /**< Zastaveno dočasným breakpointem (Run to cursor,
                                       Step over přes CALL, MCP run_until_addr / step_out). */
    EMU_PAUSE_REASON_EXIT,        /**< Žádost o konec běhu s návratovým kódem (akce `exit`
                                       breakpointu, `--exit-after-frames`, `--timeout-*`).
                                       Kde proces skončit nesmí (GUI, MCP), zůstane jen pauza,
                                       viz automation.h. */
    EMU_PAUSE_REASON_DEADLINE,    /**< Vypršel deadline běhu v emulovaném čase
                                       (MCP parametr `deadline`, run_control.h). */
    EMU_PAUSE_REASON_STEP,        /**< Dokončený krok jedné instrukce (step into,
                                       step over bez CALL). Nastavuje záznam zastavení,
                                       ne zdroj pauzy. */
    EMU_PAUSE_REASON_BP_SATURATION, /**< Auto-pauza po překročení objemu zápisů akcí
                                         breakpointů (breakpoints_fwd_account_bytes()). */
    EMU_PAUSE_REASON_EVENT_TRIGGER, /**< Pauza triggerem záznamu událostí
                                         (eventlog_trigger.h). */
    EMU_PAUSE_REASON_REPLAY,        /**< Přechodná pauza záznamu vstupů: požadavek
                                         (start záznamu nebo přehrávání) se vyřizuje
                                         v paused smyčce, po něm se emulace znovu
                                         rozběhne (replay.h). */
    EMU_PAUSE_REASON_REPLAY_END,    /**< Přehrávání záznamu došlo na konec (replay.h). */
    EMU_PAUSE_REASON_REPLAY_DESYNC, /**< Přehrávání záznamu se rozjelo: hash stavu
                                         se liší od záznamu (replay.h). */
    EMU_PAUSE_REASON_REPLAY_SEEK,   /**< Přehrávání došlo na cílový snímek seeku
                                         (replay_request_seek(), replay.h). */
    EMU_PAUSE_REASON_RUN_UNTIL_TICKS, /**< Běh došel na začátek první instrukce s časem
                                          GDG >= cíl (run_control_stop_at_arm(), MCP
                                          run_until_tstate s target_ticks). */
    EMU_PAUSE_REASON_REPLAY_GOTO     /**< Přehrávání došlo na cílový čas goto
                                          (replay_request_goto(), replay.h). */
} en_EMU_PAUSE_REASON;

/**
 * @brief Globální stav emulátoru (řízení rychlosti, pauza, dev režim).
 *
 * Pole jsou modifikována výhradně na emulátorovém vlákně (mezi instrukcemi
 * = safe-point), případně z UI vlákna přes emulator_* API se synchronizací.
 * Invariant: @c paused a @c snapshot_safepoint jsou nezávislé kanály - viz
 * popis u @c snapshot_safepoint.
 */
typedef struct st_EMULATOR
{
    bool max_speed;          /**< Maximální rychlost (warp) - bez čekání na frame. */

    /**
     * @brief Aktuální MAX SPEED zapnula automatika (CMT cpu_boost), ne uživatel.
     *
     * Nastavuje se jen v emulator_max_speed_boost(true), a to pouze když
     * MAX SPEED předtím vypnutá byla. Každé uživatelské nastavení rychlosti
     * (emulator_max_speed() - menu, klávesová zkratka, CLI --speed, MCP
     * set_speed) příznak shodí; automatika pak MAX SPEED nevypne.
     * Invariant: @c max_speed_boost == true implikuje @c max_speed == true.
     */
    bool max_speed_boost;

    bool paused;            /**< Emulace pozastavena (= CPU smyčka stojí). */

    bool development_mode;   /**< Vývojářský režim (extra UI a diagnostika). */

    bool show_demo_window;   /**< ImGui demo okno viditelné (dev pomůcka). */

    /**
     * @brief Dedikovaný safe-point příznak pro snapshot_save() guard (0019).
     *
     * Snapshot lze bezpečně uložit jen v safe-pointu se zastaveným CPU.
     * Historicky to bylo signalizováno transientním setem @c paused, což
     * ale rušilo actual_frames logiku v běhovém wait-loopu (dispatch.c
     * EMULATOR_TEST_PAUSED) - viz 0018. Tento flag je samostatný kanál:
     * snapshot_save() ho akceptuje jako rovnocenný safe-point, ale běhový
     * wait-loop ani control-plane drain ho NEvidí (čtou jen @c paused).
     *
     * Nastavuje a obnovuje se výhradně na emu vlákně mezi instrukcemi
     * (BP-action FWD_SNAPSHOT, checkpoint záznamu vstupů v bodě hranice
     * snímku), proto nevyžaduje atomic. Akceptuje ho i
     * snapshot_save_to_buffer(). Klidová hodnota
     * je false. Save/restore pattern umožňuje bezpečné vnoření.
     */
    bool snapshot_safepoint;

    /**
     * @brief Důvod posledního pozastavení emulace (záznam zastavení, emu_run `stopped_by`).
     *
     * Nastavuje ten, kdo pauzu vyvolal (frame-bounded stop, breakpoint hit,
     * manuální pauza, deadline), na emu vlákně před @c emulator_pause(true).
     * Při rozběhu nebo kroku z pauzy se vynuluje na @c EMU_PAUSE_REASON_NONE
     * (run_control_on_resume()). Viz enum.
     */
    en_EMU_PAUSE_REASON pause_reason;
} st_EMULATOR;

extern st_EMULATOR g_emulator;

#define EMULATOR_TEST_PAUSED (g_emulator.paused)
#define EMULATOR_TEST_MAX_SPEED (g_emulator.max_speed)

/**
 * @brief Test, zda je aktivní snapshot safe-point (0019).
 *
 * True pokud běží BP-action snapshot (FWD_SNAPSHOT) a dočasně povolil
 * snapshot_save() guard, aniž by se dotkl @c paused. Viz st_EMULATOR.
 */
#define EMULATOR_TEST_SNAPSHOT_SAFEPOINT (g_emulator.snapshot_safepoint)

#define EMULATOR_TEST_NORMAL_SPEED ((!EMULATOR_TEST_MAX_SPEED) && (CUSTOMSPEED_TEST_SPEED_100))
#define EMULATOR_TEST_CUSTOM_SPEED ((!EMULATOR_TEST_MAX_SPEED) && (!CUSTOMSPEED_TEST_SPEED_100))

#define EMULATOR_TEST_DEVELOPMENT_MODE (g_emulator.development_mode)
#define emulator_set_development_mode(value) (g_emulator.development_mode = value)

#ifdef __cplusplus
extern "C"
{
#endif
    /**
     * @brief Ukončí emulátor (emu vlákno se vrátí do emulator_thread()).
     *
     * - @p exit_value == 0: běžný konec. Projde úklidem (dopsání videa,
     *   série screenshotů, zachycení zvuku, uložení snapshotu a INI).
     *   Při zapnuté automatizaci se předtím zapíše `result.json` s důvodem
     *   `quit`, pokud ho nezapsal už řízený konec.
     * - @p exit_value != 0: havarijní konec. Úklid se přeskočí (stav může
     *   být rozbitý). Při zapnuté automatizaci se zapíše `result.json`
     *   s důvodem `error` a návratový kód se změní na
     *   @c AUTOMATION_EXIT_ERROR. Na Windows se mimo `--headless` čeká 5 s,
     *   aby uživatel viděl výpis; v `--headless` se nečeká.
     *
     * Volá se jen z emu vlákna mimo rozpracovanou instrukci (paused smyčka,
     * 20ms událost, háček konce snímku, inicializace).
     *
     * @param exit_value Návratový kód procesu (0 = úspěch).
     * @post Funkce se nevrací (longjmp do emulator_thread()).
     */
    void emulator_quit(int exit_value);

    /**
     * @brief Řízený konec s libovolným návratovým kódem a s úklidem.
     *
     * Na rozdíl od emulator_quit() s nenulovým kódem projde stejným úklidem
     * jako běžný konec (video, zvuk, screenshoty, snapshot, INI), takže
     * artefakty jsou po skončení procesu úplné. `result.json` sem nezapisuje,
     * to dělá volající (automation_finish()) předem.
     *
     * Kontext volání stejný jako u emulator_quit().
     *
     * @param exit_value Návratový kód procesu (0-255).
     * @post Funkce se nevrací (longjmp do emulator_thread()).
     */
    void emulator_quit_with_code(int exit_value);

    gpointer emulator_thread(gpointer ptr);

    /**
     * @brief Uživatelské zapnutí/vypnutí MAX SPEED.
     *
     * Volají ho menu, klávesové zkratky, CLI --speed a MCP set_speed.
     * Shodí příznak g_emulator.max_speed_boost - o rychlosti od této chvíle
     * rozhodl uživatel, automatika (emulator_max_speed_boost) ji nevypne.
     *
     * @param value true = MAX SPEED, false = normální/vlastní rychlost.
     * @post g_emulator.max_speed == value, g_emulator.max_speed_boost == false.
     */
    void emulator_max_speed(bool value);

    /**
     * @brief Automatické zapnutí/vypnutí MAX SPEED (CMT cpu_boost).
     *
     * - value == true: pokud MAX SPEED neběží, zapne ji a označí jako
     *   automatickou (max_speed_boost = true). Běží-li už (uživatelská nebo
     *   dříve automatická), nic nemění.
     * - value == false: vypne MAX SPEED jen tehdy, když ji zapnula
     *   automatika (max_speed_boost == true). Uživatelem zvolenou MAX SPEED
     *   nechá beze změny.
     *
     * @param value true = automatika chce MAX SPEED, false = už ji nepotřebuje.
     * @post Platí invariant max_speed_boost => max_speed.
     */
    void emulator_max_speed_boost(bool value);

    /**
     * @brief Pozastaví nebo rozběhne emulaci.
     *
     * Při shodě s aktuálním stavem nic nedělá. Jinak nastaví
     * g_emulator.paused a provede vedlejší efekty:
     * - pauza: reset měření časování snímků, událost
     *   MZEVENT_BREAK_EMULATION_PAUSED, zrušení dočasného breakpointu
     *   (bptmap_reset_temporary_event - Run to cursor, Step over, MCP
     *   run_until_addr / step_out platí jen do příští pauzy, nezávisle na
     *   otevřeném okně debuggeru);
     * - rozběh: zruší g_debugger.run_to_temporary_breakpoint a s otevřeným
     *   debuggerem přepne fokus na hlavní okno;
     * - oba směry: segment MAX SPEED benchmarku a pauza/obnovení zvuku.
     *
     * Důvod pauzy (g_emulator.pause_reason) nenastavuje - to dělá volající.
     *
     * @param value true = pauza, false = běh
     */
    void emulator_pause(bool value);
    void emulator_switch_to_normal_speed(void);
    void emulator_switch_to_custom_speed(void);
    const char *emulator_get_speed_status_as_text(void);
#ifdef __cplusplus
}
#endif

#endif // EMULATOR_H