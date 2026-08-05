/*
 * File:   vars_window.h
 *
 * Live $vars panel - samostatné dokovatelné ImGui okno pro správu
 * user-defined `$name` proměnných smart breakpointů (V1, fáze D.6.2).
 *
 * Funkce:
 *   - Tabulka přes všechny záznamy z bp_vars storage (Name | Dec | Hex | [x])
 *   - Inline edit hodnoty (manual poke do bp_var_set)
 *   - "Add Var" form (name + value -> bp_var_set)
 *   - "Clear All" tlačítko (= bp_vars_clear_all, vynuluje hodnoty)
 *   - "Clear Storage" tlačítko (= bp_vars_clear_storage, smaže záznamy)
 *
 * Viditelnost okna drží g_gui->showVarsWindow (toggle z Debugger menu).
 *
 * Refresh: každý frame čte aktuální storage. Pro V1 bez race protection -
 * BP enforcement vlákno může v okamžiku čtení mutovat storage. Případný
 * artefakt (zobrazená hodnota chvíli stale) je akceptovatelný; race-free
 * UI je V2 task.
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
 * ---------------------------------------------------------------------------
 */

#ifndef VARS_WINDOW_H
#define VARS_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavního $vars okna.
 *
 * Volá se z imgui_main_window každý frame, pokud je
 * g_gui->showVarsWindow nastaveno na true. Pokud uživatel okno
 * zavře přes (X), ImGui vynuluje *p_open na false.
 *
 * @param p_open Pointer na bool řídící viditelnost
 *               (typicky &g_gui->showVarsWindow).
 *
 * Side effects: může mutovat bp_vars storage (Add/Edit/Delete/Clear).
 *
 * Threading: jen UI vlákno. V1 bez race protection vůči EMU vláknu
 * (= BP action executor) - viz file-level dokumentace.
 */
extern void vars_window_render ( bool *p_open );


/**
 * @brief Toggle viditelnost okna.
 *
 * Přepne g_gui->showVarsWindow. Volá se z menu položek.
 */
extern void vars_window_show_hide ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* VARS_WINDOW_H */
