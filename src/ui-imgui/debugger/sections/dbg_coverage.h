/**
 * @file   dbg_coverage.h
 * @brief  Záložka "Coverage" okna CPU Profiler a značky pokrytí kódu.
 *
 * Záložka ovládá sběr pokrytí kódu (emulator/debugger/coverage/coverage.h:
 * start, stop, reset, značka, export) a ukazuje souhrn a tabulku řádků
 * s filtrem. Pomocné funkce (barva a popis stavu, sekce tooltipu) sdílí
 * s okny Disassembly #1-#5, která kreslí značky pokrytí. Data čte přes
 * dbg_coverage_data.h.
 *
 * @par Vlákna
 * Jen UI vlákno.
 *
 * @par Licence: GPLv3
 */

#ifndef DBG_COVERAGE_H
#define DBG_COVERAGE_H

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí obsah záložky (toolbar, filtr, tabulka, stavový řádek).
 *
 * @pre Volat uvnitř okna CPU Profiler (mezi ImGui::Begin a End), UI vlákno.
 * @post Případně odeslané příkazy (start, stop, reset, značka, export) jsou
 *       synchronně vyřízené emu vláknem nebo zahozené po timeoutu.
 */
void dbg_coverage_render ( void );

/**
 * @brief Barva značky stavu adresy.
 * @param state Bit COVERAGE_STATE_* (coverage_pc_state()); 0 = nevykonáno.
 * @return Barva ImGui (IM_COL32); u 0 průhledná.
 */
uint32_t dbg_coverage_state_color ( unsigned state );

/**
 * @brief Popis stavu adresy pro uživatele (přeložený).
 * @param state Bit COVERAGE_STATE_*; 0 = nevykonáno.
 * @return Statický řetězec (překlad gettextu).
 */
const char *dbg_coverage_state_label ( unsigned state );

/**
 * @brief Vykreslí sekci "Coverage" do otevřeného tooltipu.
 *
 * Stav adresy z bitmap; u úrovně detail navíc verze kódu na adrese
 * (prostor a offset buňky, bajty klíče, čítače). Vyžádá si plnou kopii
 * dat (dbg_cov_data_poll(true)), takže čítače se mohou ukázat až
 * v dalším snímku GUI.
 *
 * @param pc Logická adresa řádku.
 * @pre Volat mezi ImGui::BeginTooltip a EndTooltip.
 */
void dbg_coverage_render_tooltip ( uint16_t pc );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBG_COVERAGE_H */
