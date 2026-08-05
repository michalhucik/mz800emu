/*
 * dbg_profiler.h - render core CPU Profiler panelu (mutant profiler V1, F3).
 *
 * Sdílený render core volaný z profiler_window.cpp (wrapper). Zatím
 * jediný caller - architektura ponechává možnost embedovat panel
 * i mimo dedikované okno (např. budoucí dokovaný dashboard).
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

#ifndef DBG_PROFILER_H
#define DBG_PROFILER_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Vykreslí celý CPU Profiler panel (toolbar + tabulka + status bar).
 *
 * Volat MEZI ImGui::Begin a ImGui::End cílového okna - funkce nevytváří
 * vlastní okno, jen plní obsah aktuálně otevřeného. Renderuje:
 *
 *   - toolbar: Start/Stop tlačítko, Reset, filter input, Kind checkboxy
 *     (CALL / RST / IRQ / NMI), IRQ-in-parent toggle;
 *   - sortable tabulka 10 sloupců (Function / Addr / Kind / Calls /
 *     Excl% / Excl / Incl / Avg / Min / Max);
 *   - status bar: Running/Stopped ikona, Total cycles, Total calls,
 *     IRQ entries, Unmatched, Buckets.
 *
 * Refresh: throttle 250 ms (= 4 Hz polling) přes dbgapi
 * CMD_GET_PROFILER. Mezi refresh ticky se renderuje cache poslední
 * odpovědi.
 *
 * Threading: jen UI vlákno.
 */
void dbg_profiler_render ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBG_PROFILER_H */
