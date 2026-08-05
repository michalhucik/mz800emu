/*
 * File:   stack_regions_window.h
 *
 * V8: Stack Regions - samostatne dokovatelne ImGui okno s tabulkou stack
 * regionu. 1-radek-per-region table layout (Name / Base / Limit / SP% /
 * Min / Push / Pop / Trend / Action). Nahrazuje per-region card panel,
 * ktery byl ve V0-V7 v hlavnim Stack Monitor okne (a po jeho V8 cleanup
 * mizi).
 *
 * Render delegovan na funkce v dbg_stack_panel.cpp (= sdilena cache regionu,
 * sdilene modaly Add/Edit, sdileny render helper pro trend sparkline).
 * Tento wrapper vytvari pouze top-level ImGui::Begin/End + table layout.
 *
 * Visibilita: g_gui->showStackRegionsWindow (default false, persistence
 * pres [STACK_PANEL] show_regions_window v cfgmain.ini).
 * Keyboard shortcut: Alt+Shift+S (Alt+S je Stack Monitor okno).
 *
 * Refresh: kazdy frame ctve regiony + history pres
 * dbg_stack_panel_frame_refresh(). Idempotentne s hlavnim Stack Monitor
 * oknem - oba mohou byt otevreny soucasne bez double-refresh.
 * Threading: jen UI vlakno.
 *
 * Stable ID: "Stack Regions###stack_regions_window" - tri hashe pro
 * pripadny dynamicky title bez ztraty pozice/velikosti v ini.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v stack_regions_window.cpp.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef STACK_REGIONS_WINDOW_H
#define STACK_REGIONS_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render Stack Regions okna (V8).
 *
 * Vola se z main_window.cpp kazdy frame, pokud `*p_open` je true. Pokud
 * uzivatel okno zavre pres (X), ImGui vynuluje *p_open na false - caller
 * musi mit p_open ve sve persistent strukture (g_gui->showStackRegionsWindow).
 *
 * Wrapper otevira ImGui::Begin se stabilnim ID `###stack_regions_window`,
 * vykresluje sticky button row (+ Add region) + table 1-radek-per-region.
 * Refresh dat delegovany na dbg_stack_panel_frame_refresh.
 *
 * @param p_open Pointer na bool ridici viditelnost (typicky
 *               &g_gui->showStackRegionsWindow).
 *
 * Side effects: muze otevrit Add / Edit modaly pres
 *               dbg_stack_panel_request_add_modal /
 *               dbg_stack_panel_request_edit_modal (= submit pres dbgapi).
 *
 * @pre Volat pouze z UI vlakna mezi NewFrame a EndFrame ImGui.
 */
extern void stack_regions_window_render ( bool *p_open );


/**
 * @brief V11: Vyžádá si bring-to-front Stack Regions okna v dalším renderu.
 *
 * Pattern shodný s `stack_window_request_focus` / `stack_history_window_request_focus`:
 * nastaví pending flag, který se konzumuje v `stack_regions_window_render`:
 *
 * 1. PŘED `Begin`: `ImGui::SetNextWindowFocus()` přesune okno do popředí
 *    v ImGui-internal z-orderu.
 * 2. PO `Begin`: pokud je okno v separate platform viewport (multi-viewport
 *    mode), explicitně zavolá `Platform_SetWindowFocus(viewport)` =
 *    `SDL_RaiseWindow` pro OS-level bring-to-front.
 *
 * Volá se z cross-link tlačítka v hlavním Stack Monitor okně (V11 "Region:"
 * jako tlačítko) - po nastavení flagu a `g_gui->showStackRegionsWindow = true`
 * se v dalším framu Stack Regions okno otevře a fokusne.
 */
extern void stack_regions_window_request_focus ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* STACK_REGIONS_WINDOW_H */
