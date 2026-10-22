/**
 * @file   coverage_export.h
 * @brief  Pokrytí kódu: řádky výstupu, sloupce, stav jako JSON a export do souboru.
 *
 * Společný kód pro MCP (`coverage_status`, `coverage_read`,
 * `coverage_export`), pro uložení při konci emulátoru
 * (`--coverage-save-on-exit`) a pro souhrn v `result.json`. Pracuje nad
 * snímkem dat (st_COVERAGE_SNAPSHOT), ne nad živými daty, takže ho smí
 * volat libovolné vlákno.
 *
 * Řádek je adresa (úroveň branch, jen vykonané adresy), nebo jednotka
 * (úroveň detail). Řádky jsou seřazené podle PC, u detail dál podle buňky
 * a klíče; `k` je pořadí řádku v rámci téže adresy (kurzor stránkování).
 *
 * @par Licence: GPLv3
 */

#ifndef COVERAGE_EXPORT_H
#define COVERAGE_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <json-glib/json-glib.h>

#include "coverage.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Jeden řádek výstupu pokrytí. */
typedef struct st_COVERAGE_ROW {
    uint16_t pc;                  /**< Logická adresa. */
    uint16_t k;                   /**< Pořadí řádku v rámci téže adresy (0 = první). */
    const st_COVERAGE_UNIT *unit; /**< Jednotka (detail, ukazuje do snímku), jinak NULL. */
} st_COVERAGE_ROW;

/**
 * @name Bity stavů větve (coverage_state_bits())
 * @{
 */
#define COVERAGE_STATE_NONE           0x01u /**< Nepodmíněná instrukce. */
#define COVERAGE_STATE_UNOBSERVED     0x02u /**< Podmíněná, bez výsledku. */
#define COVERAGE_STATE_TAKEN_ONLY     0x04u /**< Jen podmínka splněna. */
#define COVERAGE_STATE_NOT_TAKEN_ONLY 0x08u /**< Jen podmínka nesplněna. */
#define COVERAGE_STATE_BOTH           0x10u /**< Obě větve. */
/** @} */

/**
 * @brief Jméno úrovně.
 * @param level Úroveň.
 * @return "branch" nebo "detail"; statický řetězec.
 */
const char *coverage_level_name ( en_COVERAGE_LEVEL level );

/**
 * @brief Sloupce řádků úrovně (pořadí = výchozí výstup).
 * @param level Úroveň.
 * @param n     Výstup: počet sloupců.
 * @return Pole statických jmen.
 */
const char *const *coverage_columns ( en_COVERAGE_LEVEL level, size_t *n );

/**
 * @brief Stav sběru jako JSON objekt (MCP odpovědi, export, result.json).
 * @param s Stav.
 * @return Nový objekt (vlastní volající, json_object_unref).
 */
JsonObject *coverage_status_to_json ( const st_COVERAGE_STATUS *s );

/**
 * @brief Seřazené řádky snímku.
 *
 * @param s Snímek; pole jednotek se seřadí na místě (PC, buňka, klíč).
 * @param n Výstup: počet řádků.
 * @return Nové pole (g_free), nikdy NULL; řádky ukazují do @p s.
 */
st_COVERAGE_ROW *coverage_rows_build ( st_COVERAGE_SNAPSHOT *s, size_t *n );

/**
 * @brief Stav větve řádku.
 * @return "none", "unobserved", "taken_only", "not_taken_only" nebo "both".
 */
const char *coverage_row_state ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r );

/**
 * @brief Příznaky řádku (detail: jednotka; branch: decision a int_entry z bitmap).
 * @return Bity COVERAGE_F_*.
 */
uint16_t coverage_row_flags ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r );

/**
 * @brief Epocha vzniku řádku (adresy u branch, jednotky u detail).
 */
uint32_t coverage_row_epoch ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r );

/**
 * @brief Stav adresy podle bitmap snímku (obě úrovně).
 *
 * Značky v okně Disassembled: stav se počítá z bitmap podle logické
 * adresy, tedy bez rozlišení bank a verzí kódu.
 *
 * @param s  Snímek.
 * @param pc Logická adresa.
 * @return 0 = adresa se nevykonala; jinak jeden bit COVERAGE_STATE_*
 *         (NONE = vykonaná nepodmíněná instrukce).
 */
unsigned coverage_pc_state ( const st_COVERAGE_SNAPSHOT *s, uint16_t pc );

/**
 * @brief Filtr řádků (MCP coverage_read, záložka Coverage v GUI).
 *
 * @invariant pc_from <= pc_to.
 */
typedef struct st_COVERAGE_FILTER {
    uint16_t pc_from;       /**< Nejnižší adresa (včetně). */
    uint16_t pc_to;         /**< Nejvyšší adresa (včetně). */
    bool has_space;         /**< Filtr prostoru buňky (jen detail, řádek bez jednotky nevyhoví). */
    uint8_t space;          /**< Prostor (coverage_space_parse()). */
    unsigned state_mask;    /**< Bity COVERAGE_STATE_*, 0 = všechny stavy. */
    bool has_epoch;         /**< Filtr epochy vzniku. */
    uint32_t epoch_from;    /**< Vyhoví coverage_row_epoch() >= epoch_from. */
    uint16_t flags_any;     /**< Řádek musí mít aspoň jeden z příznaků COVERAGE_F_* (0 = bez filtru). */
} st_COVERAGE_FILTER;

/**
 * @brief Nastaví filtr, kterému vyhoví každý řádek (0000h-FFFFh, bez podmínek).
 * @param f Filtr.
 */
void coverage_filter_init ( st_COVERAGE_FILTER *f );

/**
 * @brief Řádek vyhovuje filtru.
 * @param s Snímek.
 * @param r Řádek (coverage_rows_build()).
 * @param f Filtr.
 * @return true = vyhovuje všem podmínkám.
 */
bool coverage_row_match ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r,
                          const st_COVERAGE_FILTER *f );

/**
 * @brief Bity stavů podle jména (i skupiny "partial" a "decision").
 * @param name Jméno stavu.
 * @return Bity COVERAGE_STATE_*, 0 = neznámé jméno.
 */
unsigned coverage_state_bits ( const char *name );

/**
 * @brief Bit příznaku podle jména (jako ve sloupci `flags`).
 * @param name Jméno ("decision", "operand_varied", ...).
 * @return Bit COVERAGE_F_*, 0 = neznámé jméno.
 */
uint16_t coverage_flag_from_name ( const char *name );

/**
 * @brief Hodnota sloupce řádku jako JSON uzel.
 * @param s   Snímek.
 * @param r   Řádek.
 * @param col Jméno sloupce (coverage_columns()).
 * @return Nový uzel (json_node_free); neznámý sloupec = null.
 */
JsonNode *coverage_row_column ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r, const char *col );

/**
 * @brief Zapíše všechny řádky snímku do souboru.
 *
 * JSON: `{"format": "mz800emu-coverage/1", "status", "columns", "rows":
 * [{...}]}`; CSV: hlavička a řádky, příznaky oddělené `|`, operand bez
 * hodnoty prázdný. Soubor se přepíše (g_file_set_contents).
 *
 * @param s        Snímek (jednotky se seřadí).
 * @param path     Cesta.
 * @param csv      true = CSV, false = JSON.
 * @param out_rows Výstup: počet zapsaných řádků (smí být NULL).
 * @param out_err  Výstup při chybě: anglický text (g_free), jinak NULL.
 * @return true = zapsáno.
 */
bool coverage_export_write ( st_COVERAGE_SNAPSHOT *s, const char *path, bool csv,
                             size_t *out_rows, char **out_err );

#ifdef __cplusplus
}
#endif

#endif /* COVERAGE_EXPORT_H */
