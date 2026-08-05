/*
 * profiler.h - jádro per-function CPU profileru (mutant profiler V1, fáze F1).
 *
 * Subsystém profiler agreguje per-target (CALL / RST / IRQ accept / NMI)
 * metriky calls + inclusive cycles + exclusive cycles + min/max. Vlastní
 * žádný Z80 hook - napojuje se přes Callstack V1 listener API
 * (callstack_register_listener). Profiler set_active vynutí
 * g_callstack_active = 1 pokud bylo OFF a uloží si ownership pro
 * symetrické auto-off při set_active(false).
 *
 * Aktivace:
 *   - g_profiler_active = 0/1 (default 0). Modifikuje výhradně
 *     profiler_set_active. Hot path overhead OFF = 0 (= listener není
 *     zaregistrovaný, callstack listener slot drží NULL → fan-out nulový).
 *
 * Cycles counter:
 *   - cpu->total_cycles je uint32_t (z80.h). Wraparound nastává ~1224 s
 *     při 3.5 MHz, ~5.7 min při 14 MHz. Profiler si drží vlastní 64-bit
 *     baseline (prof_extend_cycles), detekuje wraparound porovnáním
 *     s g_prof_last_seen_32, inkrementuje baseline o 0x100000000.
 *
 * Threading:
 *   - listener volaný z EMU vlákna (= zápis hash mapy i parallel stacku
 *     je v EMU vlákně bez interní sync). UI vlákno čte přes snapshot API
 *     pod dbgapi sync handlerem (F2) - tj. v EMU vlákně atomicky alokuje
 *     kopii.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef PROFILER_H
#define PROFILER_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>


/**
 * @brief Maximální hloubka parallel stacku profileru.
 *
 * Musí být >= CALLSTACK_MAX_DEPTH (256) aby každý on_enter event měl
 * vlastní frame v profileru i v okamžiku, kdy callstack subsystem byl
 * těsně pod overflow. Použita identická hodnota - kdyby chyběla,
 * profiler by jen tiše ignoroval overflow framy (g_prof_overflow_count
 * counter na to upozorní).
 */
#define PROFILER_MAX_DEPTH 256


/* ============================================================================
 * Datové typy
 * ============================================================================ */

/**
 * @brief Akumulátor pro jednu (target_addr, kind) entry.
 *
 * Jedna instance per unikátní cíl volání. Inicializace přes g_new0
 * vynuluje vše, jen cycles_incl_min se po inicializaci nastavuje
 * na UINT64_MAX, aby první update vždy zachytil reálnou hodnotu.
 *
 * @invariant kind je hodnota z en_CALLSTACK_ENTRY_KIND
 *            (CS_KIND_CALL / RST / IRQ_IM* / NMI). SYNTHETIC kindy se
 *            do agregátoru NEukládají.
 * @invariant calls > 0 znamená že entry už dostala alespoň jeden exit;
 *            min/max/sum jsou tedy validní.
 */
typedef struct st_PROF_ENTRY {
    uint16_t target_addr;          /**< CALL/RST/IRQ target adresa. */
    uint8_t  kind;                 /**< @ref en_CALLSTACK_ENTRY_KIND cast. */
    uint8_t  _pad;                 /**< Padding (zarovnání). */
    uint64_t calls;                /**< Počet vstupů (= matched exits) do této entry. */
    uint64_t cycles_incl_sum;      /**< Suma inclusive cycles (= včetně dětí). */
    uint64_t cycles_excl_sum;      /**< Suma exclusive cycles (= bez dětí). */
    uint64_t cycles_incl_min;      /**< Nejkratší inclusive call (= UINT64_MAX, pokud calls == 0). */
    uint64_t cycles_incl_max;      /**< Nejdelší inclusive call. */
} st_PROF_ENTRY;


/**
 * @brief Globální statistiky profileru (pro UI status bar / debug).
 *
 * Snapshot stavu, vyrobený @ref profiler_get_stats nebo součást
 * @ref st_PROF_SNAPSHOT. Hodnoty platí v okamžiku volání - hot path
 * je mezi tím modifikuje, takže pro UI je vždy "aktuální stav v
 * čase volání".
 */
typedef struct st_PROF_STATS {
    bool     active;               /**< Mirror g_profiler_active. */
    uint64_t total_cycles_64;      /**< Cycles od resetu (64-bit extended). */
    uint64_t total_calls;          /**< Celkový počet on_enter eventů. */
    uint32_t irq_entries;          /**< Z toho IRQ accept events (kind IRQ_IM0/1/2). */
    uint32_t unmatched_returns;    /**< CS_FLAG_DIVERGENT exit nebo pop nad prázdným stackem. */
    uint32_t entry_count;          /**< Počet unique (target,kind) bucketů v hash mapě. */
    uint32_t max_depth_reached;    /**< Nejvyšší g_prof_depth od resetu. */
    uint32_t overflow_count;       /**< Push pokus nad PROFILER_MAX_DEPTH (= ztracený frame). */
} st_PROF_STATS;


/**
 * @brief Snapshot agregátoru pro UI / CSV export.
 *
 * Vyrobený @ref profiler_snapshot_get. Caller vlastní pole @c entries
 * (allokované přes g_malloc) a musí ho uvolnit přes
 * @ref profiler_snapshot_free.
 */
typedef struct st_PROF_SNAPSHOT {
    st_PROF_STATS  stats;          /**< Stav v okamžiku snapshot. */
    st_PROF_ENTRY *entries;        /**< Pole entries (callee allocated). */
    int            entry_count;    /**< Délka entries (= stats.entry_count). */
} st_PROF_SNAPSHOT;


/* ============================================================================
 * Globální gate
 * ============================================================================ */

/**
 * @brief Gate flag - non-zero = profiler aktivní (listener registered).
 *
 * Modifikuje výhradně @ref profiler_set_active. Mimo hot path raw čtení
 * OK (stejný pattern jako g_callstack_active). UI status bar ho přes
 * snapshot API dostává v @ref st_PROF_STATS.active.
 *
 * @note Měnit hodnotu přímo bez @ref profiler_set_active je zakázáno -
 *       registrace callstack listeneru by zůstala v rozporu.
 */
extern uint8_t g_profiler_active;


/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * @brief Inicializace profileru.
 *
 * Idempotentní. Alokuje hash mapu, zaregistruje cfgmain sekci [PROFILER]
 * s klíčem `active` (bool, default 0). Po načtení INI hodnota propaguje
 * přes @ref profiler_set_active (= pokud `active=1` v INI, automaticky
 * zapne).
 *
 * Volá se z debugger_init() na hlavním vlákně PO @ref callstack_init
 * (profiler logicky nadstavbuje callstack).
 *
 * @pre g_mzarch_main.cpu už je inicializované (= proběhl mzarch
 *      platform init).
 */
void profiler_init ( void );


/**
 * @brief Aplikace CLI options (--profiler).
 *
 * Pokud byl předán --profiler flag, vynutí g_profiler_active = 1 bez ohledu
 * na INI hodnotu. Volá se po @ref profiler_init. Vzor: callstack_apply_cli_options.
 *
 * Profiler set_active(true) implicitne aktivuje i callstack (= profiler je
 * listener nad callstack); ownership flag g_profiler_owns_callstack je nastaven
 * tak, aby pri profiler shutdown se callstack vratil do stavu pred zapnutim.
 */
void profiler_apply_cli_options ( void );


/**
 * @brief Uvolnění zdrojů profileru.
 *
 * Idempotentní. Odregistruje listener (= pokud byl), vrátí callstack
 * do stavu před @ref profiler_set_active (= vypne ho, pokud profiler
 * ho zapnul), uvolní hash mapu, vynuluje stav. Po volání musí být
 * profiler v "uninitialized" stavu - další @ref profiler_init znovu
 * funguje.
 */
void profiler_shutdown ( void );


/**
 * @brief Přepnutí profileru ON/OFF.
 *
 * Při ON:
 *   - pokud g_callstack_active == 0 → callstack_set_active(true) +
 *     zapamatuje ownership (g_profiler_owns_callstack = 1);
 *   - registruje listener (prof_on_enter / prof_on_exit);
 *   - volá profiler_reset() pro čistý start měření;
 *   - g_profiler_active = 1.
 *
 * Při OFF:
 *   - callstack_unregister_listener();
 *   - pokud ownership, callstack_set_active(false);
 *   - g_profiler_active = 0;
 *   - data v agregátoru se NEzahazují (UI je může dále zobrazit).
 *
 * Idempotentní (= opakované volání se stejnou hodnotou no-op).
 *
 * @param enable true = aktivovat, false = deaktivovat.
 *
 * @note Volat z hlavního vlákna před startem emu vlákna nebo z emu
 *       vlákna v safe-point (dbgapi sync handler). Callstack listener
 *       slot manipulace není atomická.
 */
void profiler_set_active ( bool enable );


/**
 * @brief Vynulování agregátoru + globálních counterů + baseline cycles.
 *
 * Idempotentní. Nezasahuje @ref g_profiler_active (= reset za běhu OK).
 *
 * Vynuluje:
 *   - hash mapa entries (uvolní všechny st_PROF_ENTRY);
 *   - total_calls, irq_entries, unmatched_returns, overflow_count,
 *     max_depth_reached;
 *   - parallel stack (g_prof_depth = 0);
 *   - baseline 64-bit cycles counter (= bere aktuální cpu->total_cycles
 *     jako "0" pro další měření).
 */
void profiler_reset ( void );


/**
 * @brief Notifikace o resetu CPU (= cpu->total_cycles se vrátil na 0).
 *
 * Při hard reset CPU se cpu->total_cycles nuluje. Profiler by potom
 * v prof_extend_cycles detekoval "wraparound" (now32 < last_seen_32)
 * a posunul baseline o 4G, což by způsobilo nesmyslné inclusive cycles
 * hodnoty.
 *
 * Tato funkce zresetuje JEN baseline counter (anchor_32, last_seen_32,
 * baseline_64) bez zahození agregátoru. Voláním je informačně známo
 * že další prof_extend_cycles bude vidět "now32 = 0" jako legitimní
 * začátek nové éry.
 *
 * F1 TODO: napojení do mzarch reset cesty zatím neexistuje (= žádný
 * registered emu_reset hook v API). Funkce je veřejná, aby ji mohla
 * v budoucích fázích volat platform reset. Bez napojení uživatel
 * po hard resetu vidí "divné" hodnoty inclusive cycles, dokud
 * neudělá Profiler Reset přes UI (F3) - acceptable degradation V1.
 */
void profiler_notify_cpu_reset ( void );


/* ============================================================================
 * Snapshot API (pro UI / dbgapi handler)
 * ============================================================================ */

/**
 * @brief Atomický snapshot agregátoru + stats.
 *
 * Alokuje pole st_PROF_ENTRY o velikosti entry_count, naplní obsahem
 * hash mapy v libovolném pořadí (UI sort vlastní). Vyplní @c stats
 * podle aktuálního stavu. Caller vlastní @c entries a musí ho uvolnit
 * přes @ref profiler_snapshot_free.
 *
 * Pokud je hash mapa prázdná, vrátí @c entries = NULL, entry_count = 0,
 * return code 0 (= success).
 *
 * Threading: stejný kontrakt jako callstack_snapshot_get - voláno z
 * EMU vlákna (dbgapi sync handler).
 *
 * @param out Cílová struktura. Nesmí být NULL.
 * @return 0 OK, -1 alokační chyba (= out->entries = NULL, count = 0).
 *
 * @post Pokud return == 0 a out->entry_count > 0, caller vlastní
 *       out->entries a musí ho uvolnit přes profiler_snapshot_free.
 */
int profiler_snapshot_get ( st_PROF_SNAPSHOT *out );


/**
 * @brief Uvolnění snapshot pole.
 *
 * Bezpečné pro NULL entries (= no-op). Po volání není entries pointer
 * platný.
 *
 * @param snap Snapshot struct získaný z @ref profiler_snapshot_get.
 *             Sám snap se neuvolňuje (= stack alokace volajícího).
 */
void profiler_snapshot_free ( st_PROF_SNAPSHOT *snap );


/**
 * @brief Formát exportu agregátoru profileru.
 *
 * Použito v @ref profiler_export_to_file. Hodnoty odpovídají
 * en kódování v MCP Tool `emu_profiler_export` (format="csv"/"json").
 */
typedef enum en_PROF_EXPORT_FORMAT {
    PROF_EXPORT_CSV  = 0,   /**< CSV (UTF-8, '\n', header + entry rows). */
    PROF_EXPORT_JSON = 1    /**< JSON (stats objekt + entries pole). */
} en_PROF_EXPORT_FORMAT;


/**
 * @brief Export agregátoru do souboru.
 *
 * Vyrobí atomický snapshot (= profiler_snapshot_get), zformátuje a zapíše
 * do souboru. Snapshot se po dokončení uvolní. Funkce je read-only nad
 * stavem profileru (nemění active flag, agregátor ani baseline cycles).
 *
 * Threading: voláno z EMU vlákna v dbgapi sync handleru (= stejný kontrakt
 * jako profiler_snapshot_get).
 *
 * @param filepath      Cílová cesta. NULL nebo prázdný řetězec = chyba.
 * @param format        @ref en_PROF_EXPORT_FORMAT hodnota.
 * @param out_entry_count Volitelný OUT pointer pro počet zapsaných entries
 *                        (NULL = neplnit).
 * @return  0 OK; -1 open/write chyba; -2 neplatný format/parametry;
 *         -3 alokační chyba snapshot.
 *
 * @post Při návratu != 0 obsah souboru může být částečně zapsaný
 *       (caller by měl při chybě soubor zahodit).
 */
int profiler_export_to_file ( const char *filepath, int format,
                              int *out_entry_count );


/**
 * @brief Levný náhled statistik bez alokace entries.
 *
 * Pro status bar refresh (= 4 Hz polling). Vyplní @c out aktuálním
 * stavem counterů - žádná alokace, žádný kopírovaný seznam entries.
 *
 * @param out Cílová struktura. NULL = no-op.
 */
void profiler_get_stats ( st_PROF_STATS *out );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* PROFILER_H */
