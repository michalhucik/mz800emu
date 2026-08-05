/*
 * dbg_callstack.h - Callstack panel pro debugger (mutant callstack, Fáze 2A).
 *
 * Sdílený render core pro Callstack okno. Vykresluje toolbar (Active /
 * Reset / Hide IRQ / Hide Synth filter + depth indicator), tabulku
 * shadow stacku (sloupce #/Ret/Call/Target/Sym/Kind/Cyc-in) a status
 * footer se statistikami divergence / sp-swap / overflow.
 *
 * Layout (Variant B - samostatné plovoucí okno):
 *
 *   +--- Callstack -------------------------------------------+
 *   | [Active] [Reset] [Hide IRQ] [Hide Synth]  depth: N / M  |
 *   +---+------+------+--------+-----------+-------+----------+
 *   | # | Ret  | Call | Target | Sym       | Kind  | Cyc-in   |
 *   +---+------+------+--------+-----------+-------+----------+
 *   | 0 | 8123 | 8120 | 8500   | render    | CALL  | 1234     |
 *   | 1 | 8521 | 8500 | 9000   | clear     | CALL  | 567      |
 *   +---+------+------+--------+-----------+-------+----------+
 *   | Diverg: 2  SP-swap: 0  Overflow: 0                       |
 *   +---------------------------------------------------------+
 *
 * Refresh: per-frame přes DbgRefreshController tick (= 100 ms + force
 * při pauze/step). Sekce volá dbgapi CMD_GET_CALLSTACK, callee alokuje
 * pole entries, UI ho zkopíruje do své cache a uvolní pres
 * callstack_snapshot_free. Mezi refresh tiky panel pracuje z cache.
 *
 * Interakce řádky:
 *   - klik LMB    = dbg_disasm_view_focus_to(main, call_site_addr)
 *   - Shift+klik  = dbg_disasm_view_focus_to(main, target_addr)
 *   - double-klik = dbg_disasm_show_in_slot(1, call_site_addr) - otevře
 *                   sekundární disasm okno na adrese call site
 *
 * Stable ID strategie: okno má `###callstack_window` (tři hashe pro
 * stabilní pozici/velikost i při dynamickém title bez ztráty ini záznamu).
 *
 * Threading: jen UI vlákno. Cache je single-thread access. Dbgapi
 * submit_cmd_sync zajišťuje safe-point čtení z emu vlákna.
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

#ifndef DBG_CALLSTACK_H
#define DBG_CALLSTACK_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Vykreslí Callstack panel do aktuálního ImGui kontejneru.
 *
 * Předpokládá se, že caller (= callstack_window.cpp wrapper) již vytvořil
 * top-level okno přes ImGui::Begin a panel obsadí celou dostupnou plochu
 * (= toolbar + tabulka + footer).
 *
 * Data čte přes dbgapi (CMD_GET_CALLSTACK) v okamžicích kdy
 * g_dbg_ui.refresh.should_refresh == true. Mezi refresh tiky panel
 * pracuje s cache (panel_state.entries, panel_state.stats).
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame ImGui,
 *      uvnitř ImGui::Begin() okna.
 */
void dbg_callstack_render ( void );


/**
 * @brief Registruje cfgmain sekci [CALLSTACK_PANEL] pro persistenci UI
 *        preferencí.
 *
 * Sekce obsahuje:
 *   hide_irq    = bool 0/1   (filtr IRQ/NMI rows)
 *   hide_synth  = bool 0/1   (filtr SYNTHETIC rows)
 *
 * Aktivace subsystému (g_callstack_active) je v samostatné sekci
 * [CALLSTACK] (= jádro callstack.c, viz callstack_init).
 *
 * Volá se z debugger_init() na hlavním vlákně. Idempotentní (= druhé
 * volání je no-op).
 *
 * Side effects: po načtení aplikuje hodnoty do panel state struktury.
 *
 * @pre g_cfgmain inicializován.
 */
void dbg_callstack_cfg_init ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBG_CALLSTACK_H */
