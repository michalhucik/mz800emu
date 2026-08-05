#ifndef EMULATOR_H
#define EMULATOR_H

#include "app/app.h"
#include <glib.h>

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

#include "customspeed.h"

/**
 * @brief Důvod posledního pozastavení emulace (diagnostika pro emu_run).
 *
 * Slouží jen jako signál pro MCP `emu_run` (pole `stopped_by`), aby klient
 * rozlišil, zda emulace doběhla požadovaný počet framů, nebo ji zastavil
 * breakpoint / manuální pauza. Není to autoritativní stav emulace - pauzy
 * z jiných důvodů (HALT, fatal) ho nemusí nastavit, pak zůstane @c NONE.
 */
typedef enum en_EMU_PAUSE_REASON
{
    EMU_PAUSE_REASON_NONE = 0,    /**< Bez konkrétního důvodu / běží. */
    EMU_PAUSE_REASON_FRAMES,      /**< Doběhl požadovaný počet framů (emu_run). */
    EMU_PAUSE_REASON_BREAKPOINT,  /**< Zastaveno breakpointem. */
    EMU_PAUSE_REASON_MANUAL       /**< Manuální pauza (UI nebo MCP pause). */
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
    bool paused;             /**< Emulace pozastavena (= CPU smyčka stojí). */

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
     * (BP-action FWD_SNAPSHOT), proto nevyžaduje atomic. Klidová hodnota
     * je false. Save/restore pattern umožňuje bezpečné vnoření.
     */
    bool snapshot_safepoint;

    /**
     * @brief Důvod posledního pozastavení emulace (pro emu_run `stopped_by`).
     *
     * Nastavuje ten, kdo pauzu vyvolal (frame-bounded stop, breakpoint hit,
     * manuální pauza), na emu vlákně před @c emulator_pause(true). `emu_run`
     * ho před spuštěním vynuluje na @c EMU_PAUSE_REASON_NONE a po doběhnutí
     * přečte. Diagnostický signál, ne autoritativní stav (viz enum).
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
    void emulator_quit(int exit_value);

    /**
     * @brief Deinicializace emulátorových subsystémů (flush + destroy).
     *
     * Volá version_check/snapshot/cfgmain/measuring/platform_fn exit
     * sekvenci (zapisuje INI, CDL export, media writeback a uvolňuje
     * stav emulace).
     *
     * Závazné pořadí při shutdownu (threading kontrakt, mzhal krok 12):
     * g_thread_join(emu_thread) -> shutdown MCP transportů ->
     * dbgapi_dispatcher_shutdown + dbgapi_destroy -> emulator_teardown()
     * -> iface_exit(). Funkce běží na main vlákně v single-threaded
     * kontextu - nesmí běžet souběžně s emu vláknem ani s MCP dispatch
     * vlákny (uvolňuje stav, na který dispatch sahá).
     *
     * Preconditions: emu vlákno joinnuté, MCP/dbgapi transporty
     * shutdownuté. Postconditions: stav emulace uvolněn; volat max 1x.
     * Při abnormálním exitu (emulator_quit s nenulovou hodnotou) se
     * nevolá.
     */
    void emulator_teardown(void);
    gpointer emulator_thread(gpointer ptr);

    void emulator_max_speed(bool value);
    void emulator_pause(bool value);
    void emulator_switch_to_normal_speed(void);
    void emulator_switch_to_custom_speed(void);
    const char *emulator_get_speed_status_as_text(void);
#ifdef __cplusplus
}
#endif

#endif // EMULATOR_H