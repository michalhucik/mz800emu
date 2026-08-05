/*
 * File:   profiler_window.h
 *
 * CPU Profiler - samostatné plovoucí ImGui okno (mutant profiler V1, fáze F3).
 *
 * Wrapper kolem dbg_profiler_render() ze sections/dbg_profiler.cpp.
 * Otevře/zavře vlastní ImGui::Begin/End kontext s persistencí pozice
 * a velikosti (přes ImGui ini). Žádná vlastní logika - veškerý render
 * obstarává sdílená funkce dbg_profiler_render().
 *
 * Visibilita: g_gui->showProfilerWindow (toggle z Debugger menu nebo
 * Alt+Shift+P shortcut, konzistentní s ostatními debugger okny).
 * Persistence stavu mezi sezeními: INI sekce [PROFILER] klíč
 * show_window (= viz profiler_window_apply_persisted níže).
 *
 * Refresh: dbg_profiler.cpp si drží vlastní throttle (250 ms / 4 Hz)
 * pro polling dbgapi snapshot.
 *
 * Threading: jen UI vlákno.
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

#ifndef PROFILER_WINDOW_H
#define PROFILER_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavního CPU Profiler okna.
 *
 * Volá se z main_window každý frame, pokud `*p_open` je true. Pokud
 * uživatel okno zavře přes (X), ImGui vynuluje *p_open na false -
 * caller musí mít p_open v persistent struktuře.
 *
 * Wrapper otevírá ImGui::Begin se stabilním ID `###profiler_window`
 * (= title se může měnit pro arch-specific označení bez ztráty
 * pozice/velikosti). Render delegován na dbg_profiler_render(),
 * která obsadí celou dostupnou plochu.
 *
 * @param p_open Pointer na bool řídící viditelnost. NULL nebo *p_open
 *               == false → no-op.
 *
 * @pre Volat pouze z UI vlákna mezi ImGui::NewFrame a ImGui::EndFrame.
 */
extern void profiler_window_render ( bool *p_open );


/**
 * @brief Registrace klíče show_window v cfgmodule sekce [PROFILER].
 *
 * Volá se z profiler_init() (C jádro) přes pomocný void* na st_CFGMODULE,
 * obdobně jako event_viewer_window_register_persistence(). Zaregistruje
 * cfgelement nad file-static cache (s_persisted_show_window), která drží
 * hodnotu načtenou z INI dokud g_gui neexistuje. Po aplikaci (= viz
 * profiler_window_apply_persisted) g_gui->showProfilerWindow drží runtime
 * stav a sync zpět do cache probíhá per-frame v profiler_window_render
 * (cheap uint write).
 *
 * @param cmod_void Cíl: st_CFGMODULE* sekce [PROFILER]. NULL = no-op.
 *
 * @pre g_cfgmain inicializovaný, sekce [PROFILER] vytvořená.
 */
extern void profiler_window_register_cfg ( void *cmod_void );


/**
 * @brief Aplikace persistovaného show_window flagu na g_gui.
 *
 * Volá se z hlavního vlákna POTÉ co byl g_gui alokován (= analogicky
 * event_viewer_window_apply_persisted). Přečte file-static cache
 * naplněnou cfgmain propagate cb a nastaví g_gui->showProfilerWindow.
 *
 * Bezpečné pro g_gui == NULL (= no-op, hodnota čeká v cache).
 */
extern void profiler_window_apply_persisted ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* PROFILER_WINDOW_H */
