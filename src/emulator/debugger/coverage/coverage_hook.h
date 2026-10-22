/**
 * @file   coverage_hook.h
 * @brief  Napojení pokrytí kódu na emulátor: háček po instrukci, buňky, callbacky CPU.
 *
 * Jádro pokrytí (coverage.h) je bez vazby na platformu. Tento modul ho
 * zapojuje:
 * - start a stop přepočítají masku háčků po instrukci
 *   (MZARCH_POST_INSN_COVERAGE) a callbacky CPU
 *   (mzarch_platform_fn_debugger_state_changed()); úroveň detail vynucuje
 *   logovací callbacky (g_coverage_need_fetch v
 *   TEST_DEBUGGER_NEED_DEBUG_CALLBACKS), aby byly k dispozici bajty
 *   instrukce tak, jak je CPU načetlo (g_debugger_insn_fetch),
 * - coverage_hook_on_instruction() sestaví st_COVERAGE_STEP ze stavu CPU
 *   po kroku; úroveň branch čte bajty kroku z paměti po kroku
 *   (memory_read_byte_no_se), detail ze záznamu fetch, chybějící bajt
 *   z paměti s příznakem COVERAGE_F_FETCH_FALLBACK,
 * - fyzická buňka prvního bajtu (jen detail) se určí podle mapování po
 *   kroku: RAM a MemExt podle ukazatele čtení stránky (memram_read),
 *   ROM, VRAM MZ-700, PCG podle ramlog_platform_targets(), s cache po
 *   4 KiB stránkách platnou pro klíč ramlog_platform_map_key() a ukazatel
 *   memram_read stránky (okno portů E000-E008 mimo cache); grafická VRAM
 *   MZ-800 a mapované porty nemají fyzickou buňku (prostor
 *   COVERAGE_SPACE_BUS, offset = CPU adresa, příznak COVERAGE_F_CELL_BUS).
 *   Instrukce, která přepne banking pod sebou, dostane buňku podle nového
 *   mapování,
 * - příznaky původu: přijetí přerušení (COVERAGE_F_INT_ENTRY na první
 *   instrukci obsluhy) a past CMT hacku (COVERAGE_F_CMT_TRAP na instrukci,
 *   během které hack zasáhl) se předávají přes g_coverage_pending_flags.
 *
 * @par Vlákna
 * Vše volá emu vlákno (háček po instrukci, blok událostí, drain dbgapi).
 *
 * @par Licence: GPLv3
 */

#ifndef COVERAGE_HOOK_H
#define COVERAGE_HOOK_H

#include <stdbool.h>
#include <stdint.h>

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "coverage.h"
#include "mzarch/mzarch.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nenulové = běží úroveň detail, CPU musí používat logovací callbacky (bajty z fetch). */
extern int g_coverage_need_fetch;

/**
 * @brief Příznaky původu pro nejbližší krok (COVERAGE_F_INT_ENTRY, COVERAGE_F_CMT_TRAP).
 *
 * Nastavuje je přijetí přerušení a CMT hack, jen při g_coverage_active;
 * háček po instrukci je přičte kroku a vynuluje.
 */
extern uint16_t g_coverage_pending_flags;

/**
 * @brief Spustí sběr a přepne callbacky CPU a masku háčků.
 *
 * @param cfg   Nastavení (coverage_start()).
 * @param reset true = začít s prázdnými daty.
 * @return false = nepodařila se alokace tabulky (sběr neběží).
 * @pre Emu vlákno, mimo instrukci (drain dbgapi, start emulátoru).
 * @post Při úspěchu bit MZARCH_POST_INSN_COVERAGE v masce háčků; u detail
 *       logovací callbacky CPU.
 */
bool coverage_hook_start ( const st_COVERAGE_CONFIG *cfg, bool reset );

/**
 * @brief Zastaví sběr (data zůstanou) a vrátí callbacky CPU a masku háčků.
 * @pre Emu vlákno, mimo instrukci.
 */
void coverage_hook_stop ( void );

/**
 * @brief Háček po instrukci: zapíše krok do pokrytí.
 * @param insn Informace o kroku z rozbočovače (mzarch.c).
 * @pre g_coverage_active; volá ho jen rozbočovač háčků po instrukci.
 */
void coverage_hook_on_instruction ( const st_MZARCH_INSN_INFO *insn );

/** @brief CMT hack zasáhl během právě vykonávané instrukce (studená cesta). */
void coverage_hook_note_cmt_trap ( void );


/** @brief Spuštění pokrytí z konfigurace (`[COVERAGE] mode`, `--coverage`). */
typedef enum en_COVERAGE_CFG_MODE {
    COVERAGE_CFG_MODE_OFF = 0, /**< Nespouštět (výchozí). */
    COVERAGE_CFG_MODE_BRANCH,  /**< Spustit úroveň branch při startu emulátoru. */
    COVERAGE_CFG_MODE_DETAIL,  /**< Spustit úroveň detail při startu emulátoru. */
} en_COVERAGE_CFG_MODE;

/** @brief Konfigurace pokrytí z INI `[COVERAGE]` a CLI. */
typedef struct st_COVERAGE_INI {
    unsigned mode;                 /**< en_COVERAGE_CFG_MODE. */
    unsigned key;                  /**< en_COVERAGE_KEY. */
    unsigned capacity;             /**< Kapacita jednotek (detail). */
    int reset_on_discontinuity;    /**< 1 = reset a snapshot data smažou. */
    char *save_on_exit;            /**< Jen CLI: soubor exportu při konci (.csv = CSV, jinak JSON), g_free; NULL = nic. */
} st_COVERAGE_INI;

/** @brief Konfigurace pokrytí (platí po coverage_cfg_init a coverage_apply_cli_options). */
extern st_COVERAGE_INI g_coverage_ini;

/**
 * @brief Zaregistruje INI sekci `[COVERAGE]` a načte ji do @ref g_coverage_ini.
 * @pre g_cfgmain existuje. Volá se jednou z debugger_init.
 */
void coverage_cfg_init ( void );

/**
 * @brief Přepíše @ref g_coverage_ini hodnotami z CLI (`--coverage`,
 *        `--coverage-key`, `--coverage-capacity`, `--coverage-save-on-exit`).
 *
 * Neplatná hodnota se ohlásí na stderr (anglicky) a ponechá se hodnota z INI.
 */
void coverage_apply_cli_options ( void );

/**
 * @brief Spustí pokrytí podle konfigurace, jen poprvé (start emulátoru).
 *
 * Volá se z mzarch_platform_fn_debugger_state_changed před výběrem
 * callbacků CPU, proto přímo coverage_start() + příznak logovacích
 * callbacků a maska háčků (bez coverage_hook_start(), který by
 * state_changed volal znovu). Pokrytí zastavené přes MCP nebo DSL už
 * konfigurace znovu nespustí.
 */
void coverage_recompute_active ( void );

/**
 * @brief Konec emulátoru: export dat do souboru `--coverage-save-on-exit`
 *        (relativní cesta vůči aktuálnímu adresáři procesu, ne `--work-dir`).
 *
 * Bez nastaveného souboru nebo bez dat nedělá nic. Chybu zápisu ohlásí na
 * stderr. Volá se z debugger_exit před coverage_free().
 */
void coverage_on_exit ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* COVERAGE_HOOK_H */
