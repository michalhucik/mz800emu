/**
 * @file   dbg_coverage_data.h
 * @brief  Data pokrytí kódu pro okna GUI (sdílená kopie v UI vlákně).
 *
 * Okna Disassembly #1-#5 (značky, tooltip) a záložka Coverage okna CPU
 * Profiler čtou pokrytí z jedné kopie dat, kterou si UI vlákno vyžádá
 * přes DBGAPI_CMD_COVERAGE:
 *
 * - **lehký snímek** (stav, bitmapy, epochy, bez jednotek) nejvýš 2x za
 *   sekundu, když sběr běží, jinak jednou za 2 s (zachytí reset nebo start
 *   přes MCP), a jen když ho v daném snímku GUI někdo chce
 *   (dbg_cov_data_poll()). Příkaz dbgapi je synchronní a za běhu emulace
 *   čeká na konec emulovaného snímku, proto řídká obnova;
 * - **plný snímek** (i jednotky úrovně detail) a z něj seřazené řádky
 *   nejvýš 1x za sekundu, jen když ho někdo chce a data se od minulé kopie
 *   změnila (počet instrukcí, generace, epocha).
 *
 * Bez otevřeného okna se nic nežádá, takže zavřené okno sběr nezpomaluje.
 *
 * @par Vlákna
 * Jen UI vlákno.
 *
 * @par Licence: GPLv3
 */

#ifndef DBG_COVERAGE_DATA_H
#define DBG_COVERAGE_DATA_H

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/coverage/coverage_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Interval obnovy lehkého snímku, když sběr běží (s). */
#define DBG_COV_DATA_LIGHT_INTERVAL_S 0.5

/** @brief Interval obnovy lehkého snímku, když sběr neběží (s). */
#define DBG_COV_DATA_IDLE_INTERVAL_S 2.0

/** @brief Nejkratší interval obnovy plného snímku (s). */
#define DBG_COV_DATA_FULL_INTERVAL_S 1.0

/**
 * @brief Obnoví data, pokud uplynul interval (volá každé viditelné okno v každém snímku GUI).
 *
 * @param need_units true = volající potřebuje řádky s jednotkami (tabulka,
 *                   tooltip úrovně detail); obnoví se i plný snímek.
 * @post Při plné frontě dbgapi nebo timeoutu zůstanou předchozí data.
 *       Volání bez uplynulého intervalu nic nežádá.
 */
void dbg_cov_data_poll ( bool need_units );

/**
 * @brief Vynutí obnovu obou snímků při příštím dbg_cov_data_poll().
 *
 * Volá se po operaci, která data mění (start, stop, reset, značka).
 */
void dbg_cov_data_invalidate ( void );

/**
 * @brief Aspoň jedna kopie dat se podařila.
 * @return false = stav ještě není známý (okno ukáže "no data").
 */
bool dbg_cov_data_valid ( void );

/**
 * @brief Poslední známý stav sběru.
 * @return Ukazatel do kopie (platí do dalšího dbg_cov_data_poll()); bez
 *         platné kopie nulový stav.
 */
const st_COVERAGE_STATUS *dbg_cov_data_status ( void );

/**
 * @brief Stav adresy pro značku v okně Disassembly.
 * @param pc Logická adresa.
 * @return 0 = bez dat nebo nevykonaná adresa; jinak bit COVERAGE_STATE_*
 *         (coverage_pc_state()).
 */
unsigned dbg_cov_data_pc_state ( uint16_t pc );

/**
 * @brief Plný snímek (pro funkce coverage_row_*()).
 * @return Snímek, nebo NULL, dokud se plný snímek nepodařil.
 */
const st_COVERAGE_SNAPSHOT *dbg_cov_data_full ( void );

/**
 * @brief Seřazené řádky plného snímku (podle PC, pak buňky a klíče).
 * @param n Výstup: počet řádků.
 * @return Pole řádků (platí do další obnovy plného snímku), nebo NULL.
 */
const st_COVERAGE_ROW *dbg_cov_data_rows ( size_t *n );

/**
 * @brief Pořadové číslo obnovy plného snímku.
 *
 * Zvýší se při každé nové kopii; okna podle něj přepočítávají své
 * odvozené seznamy (filtr tabulky).
 *
 * @return Číslo obnovy (0 = žádná).
 */
uint32_t dbg_cov_data_full_serial ( void );

/**
 * @brief Řádky plného snímku pro jednu adresu.
 * @param pc    Logická adresa.
 * @param first Výstup: index prvního řádku adresy v dbg_cov_data_rows().
 * @return Počet řádků adresy (0 = žádný nebo bez plného snímku).
 */
size_t dbg_cov_data_rows_for_pc ( uint16_t pc, size_t *first );

/**
 * @brief Provede operaci sběru (START, STOP, RESET, MARK) a vynutí obnovu dat.
 *
 * @param op        Operace.
 * @param cfg       Nastavení (jen START, jinak smí být NULL).
 * @param reset     START: začít s prázdnými daty.
 * @param err       Výstup: anglický text chyby ("" = OK; timeout má
 *                  vlastní text); smí být NULL.
 * @param err_size  Velikost @p err.
 * @return true = provedeno.
 * @pre UI vlákno; blokuje do vyřízení emu vláknem (limit vyzvednutí 1 s).
 */
bool dbg_cov_data_op ( en_DBGAPI_COVERAGE_OP op, const st_COVERAGE_CONFIG *cfg, bool reset,
                       char *err, size_t err_size );

/**
 * @brief Zapíše aktuální data (nový plný snímek) do souboru JSON nebo CSV.
 *
 * Stejný formát jako MCP coverage_export (coverage_export_write()).
 *
 * @param path     Cesta.
 * @param csv      true = CSV, false = JSON.
 * @param out_rows Výstup: počet zapsaných řádků (smí být NULL).
 * @param err      Výstup: anglický text chyby; smí být NULL.
 * @param err_size Velikost @p err.
 * @return true = zapsáno.
 * @pre UI vlákno.
 */
bool dbg_cov_data_export ( const char *path, bool csv, size_t *out_rows, char *err, size_t err_size );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBG_COVERAGE_DATA_H */
