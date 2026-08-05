/**
 * @file   cooperation.h
 * @brief  Cooperation hint - self-binding instrukce pro AI klienta
 *         (mutant mcp-server V1.A.1).
 *
 * Podle rozboru sekce 3.3.2 ("cooperation hint") MCP server poskytuje
 * stateful slot, do kterého si AI klient dobrovolně uloží instrukci,
 * čím sám sebe omezuje mezi jednotlivými requesty (MCP je per-request
 * stateless - bez tohoto slotu by AI musela hint pamatovat v konverzaci
 * a uživatel by jeho dodržení nemohl auditovat).
 *
 * Hint je **dobrovolný** - V1.A.1 server hint persistne a vysílá MSG
 * notifikaci do UI ("Activity Log" panel) ale **neenforcoval** žádnou
 * policy. Tj. AI klient se může rozhodnout hint porušit; v takovém
 * případě je porušení zaznamenané v Activity Logu. Tvrdé enforcement
 * je odložené na V1.A.5+ (= security profile).
 *
 * Třídy módů (viz @ref en_COOPERATION_HINT):
 *   FREE         - žádné omezení (default po startu, default po clear).
 *   READ_ONLY    - AI se zavazuje volat jen read-only Tools / Resources.
 *   PAUSED_ONLY  - AI se zavazuje volat Tools jen pokud emu je v pauze.
 *
 * Životní cyklus:
 *   1. mcp_pipe_main / tcp_server bootstrap zavolá cooperation_hint_init().
 *   2. AI klient přes Tool emu_cooperation_hint_set volá
 *      cooperation_hint_set(mode, until).
 *   3. UI panel (= Activity Log V1.C) zobrazí current mode.
 *   4. Při shutdown se hint nepersistne (= v paměti only).
 *
 * Thread-safety:
 *   V1.A.1 implementace je single-threaded read/write z dispatch handleru
 *   běžícího v MCP stdin reader vlákně. Pokud V1.C UI panel přistoupí na
 *   g_cooperation_hint z hlavního vlákna, je třeba přidat zámek - zatím
 *   API zveřejňuje pouze cooperation_hint_get_*(), které dělají read pod
 *   GLib atomic / mutex (přidáme až bude UI consumer).
 *
 * Soubor je obalen guardem MZ800EMU_CFG_MCP_SERVER_ENABLED stejně jako
 * dispatch.c.
 *
 * @par Licence:
 * GPL-3.0-or-later.
 */

#ifndef MZ800EMU_SRC_EMULATOR_MCP_COOPERATION_H
#define MZ800EMU_SRC_EMULATOR_MCP_COOPERATION_H

#ifndef MZ800EMU_MCP_TEST_BUILD
#include "../mzarch/mzcommon_config.h"
#endif

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Módy cooperation hintu.
 *
 * Hodnoty odpovídají wire stringům v JSONL requestu (= "free",
 * "read_only", "paused_only"). Server akceptuje pouze tyto tři; jiný
 * string AI klient dostane jako INVALID_PARAMS error.
 */
typedef enum en_COOPERATION_HINT
{
    COOPERATION_HINT_FREE        = 0,  /**< Bez omezení (= default po startu). */
    COOPERATION_HINT_READ_ONLY   = 1,  /**< Jen read-only Tools / Resources. */
    COOPERATION_HINT_PAUSED_ONLY = 2,  /**< Tools jen pokud emu paused. */
} en_COOPERATION_HINT;


/**
 * @brief Globální stav cooperation hintu.
 *
 * V1.A.1 single instance - žádný per-session hint (= MCP server má vždy
 * jedno aktivní spojení v daný okamžik). Pokud V1.A.5+ povolí více
 * souběžných klientů, hint bude per-session.
 *
 * @invariant mode je vždy validní en_COOPERATION_HINT hodnota.
 * @invariant until ukazatel je buď NULL nebo platný heap string
 *            alokovaný přes g_strdup (uvolnit g_free).
 * @invariant Po cooperation_hint_init je struct vyresetovaný
 *            (mode=FREE, until=NULL, set_at_us=0).
 */
typedef struct st_COOPERATION_HINT_STATE
{
    en_COOPERATION_HINT mode;       /**< Aktuální mód hintu. */
    char               *until;      /**< Volitelný "until" hint (např. ISO timestamp). NULL = otevřená délka. */
    uint64_t            set_at_us;  /**< g_get_monotonic_time() v okamžiku posledního set/clear. */
} st_COOPERATION_HINT_STATE;


/**
 * @brief Globální instance cooperation hintu.
 *
 * Definice v cooperation.c. Dispatch handler ho čte / modifikuje.
 * UI Activity Log panel ho v budoucnu bude číst pro zobrazení current
 * mode (V1.C).
 */
extern st_COOPERATION_HINT_STATE g_cooperation_hint;


/**
 * @brief Inicializuje cooperation hint do default stavu (FREE, no until).
 *
 * Volat jednou při startu MCP serveru (= z mcp_pipe_main před stdin
 * readerem a z tcp_server_start před přijetím prvního klienta). Funkce
 * je idempotentní: pokud je už inicializováno, uvolní starý until
 * string a přenastaví všechna pole na default.
 *
 * Side effects: může g_free starý g_cooperation_hint.until.
 *
 * Thread-safety: volat z transport bootstrap vlákna před spuštěním
 * stdin reader / TCP accept loop.
 */
void cooperation_hint_init(void);


/**
 * @brief Nastaví cooperation hint na zadaný mód + optional "until" hint.
 *
 * Aktualizuje globální g_cooperation_hint atomicky pro samotná pole
 * (= V1.A.1 single-threaded, žádný formal lock zatím). Pokud volající
 * předá non-NULL until, funkce ho zkopíruje (= AI klient si stringy
 * nemusí držet po návratu).
 *
 * @param mode  Nový mód (validovaný caller-em, ale funkce respektuje
 *              i out-of-range hodnoty = uloží je as-is; preferován je
 *              FREE / READ_ONLY / PAUSED_ONLY).
 * @param until Volitelný "until" string, např. "next user message",
 *              ISO 8601 timestamp nebo "next 30 min". NULL nebo
 *              prázdný string = otevřená délka (uloží se NULL).
 *
 * @post g_cooperation_hint.mode == mode.
 * @post g_cooperation_hint.until je owned heap string (g_strdup) nebo NULL.
 * @post g_cooperation_hint.set_at_us je aktualizováno na g_get_monotonic_time().
 */
void cooperation_hint_set(en_COOPERATION_HINT mode, const char *until);


/**
 * @brief Vyresetuje hint na FREE (= ekvivalent cooperation_hint_set(FREE, NULL)).
 *
 * Zkratka pro AI klienta, který chce explicit clear, aniž by musel
 * znát enum hodnotu pro FREE. Sémantický ekvivalent
 * cooperation_hint_set(COOPERATION_HINT_FREE, NULL).
 *
 * @post g_cooperation_hint.mode == COOPERATION_HINT_FREE.
 * @post g_cooperation_hint.until == NULL.
 */
void cooperation_hint_clear(void);


/**
 * @brief Vrátí wire-string identifikátor módu (= "free" / "read_only" /
 *        "paused_only" / "unknown").
 *
 * Použití: dispatch handler odpovídá AI klientovi success payloadem
 * obsahujícím echo módu jako string. Symetricky cooperation_hint_mode_from_str
 * mapuje opačným směrem.
 *
 * @param mode Mód.
 * @return Static C string (nikdy NULL).
 */
const char *cooperation_hint_mode_to_str(en_COOPERATION_HINT mode);


/**
 * @brief Mapuje wire-string na en_COOPERATION_HINT.
 *
 * Akceptuje přesné case-sensitive řetězce "free", "read_only",
 * "paused_only". Pro neznámou hodnotu vrací false a nezasahuje out_mode.
 *
 * @param[in]  s        Vstupní string (NULL bezpečné = false).
 * @param[out] out_mode Výsledný mód při úspěchu (může být NULL pokud
 *                       caller jen ověřuje validitu).
 * @return true pokud string odpovídá známému módu, jinak false.
 */
bool cooperation_hint_mode_from_str(const char *s, en_COOPERATION_HINT *out_mode);


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_MCP_SERVER_ENABLED */

#endif /* MZ800EMU_SRC_EMULATOR_MCP_COOPERATION_H */
