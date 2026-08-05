/*
 * File:   stack_history_window.h
 *
 * V9: Stack History - samostatne dokovatelne ImGui okno se SP history
 * sparkline plotem. Paralelni okno k V8 Stack Regions oknu: plot resize
 * s velikosti okna (X i Y), default 800x300 px. User si tak muze otevrit
 * sparkline ve velkem (= detail prubehu SP v case bez kompromisu s vyskou
 * hlavniho Stack Monitor okna).
 *
 * Layout:
 *   +- Stack History --------------------------+
 *   | [info row - Samples/Slope/Selected]      |
 *   | [Show events]                            |
 *   +------------------------------------------+
 *   | sparkline plot (= grow s oknem)          |
 *   |                                          |
 *   +------------------------------------------+
 *
 * Plot vyska = ContentRegionAvail.y - info_h (= grow), plot sirka =
 * ContentRegionAvail.x. Minimum plot vyska 60 px, minimum plot sirka
 * 80 px (= chranen pred prilis malym oknem).
 *
 * Render delegovan na sdilene helpery v dbg_stack_panel.cpp
 * (dbg_stack_panel_render_history_plot, _render_history_info), takze
 * funkcnost je byte-shodna s SP history sekci v hlavnim Stack Monitor
 * okne. Klik na sample v jednom okne se projevi v druhem (sdileny state
 * g_stack.selected_history_idx).
 *
 * Visibilita: g_gui->showStackHistoryWindow (default false, persistence
 * pres [STACK_PANEL] show_history_window v cfgmain.ini).
 * Keyboard shortcut: Alt+Shift+H.
 *
 * Pri vypnute history (= g_stack.history_enabled == false) okno vykresli
 * hint "Enable SP history in Stack Monitor" (= info v info radku) a plot
 * se nevykresli.
 *
 * Stable ID: "Stack History###stack_history_window" - tri hashe pro
 * pripadny dynamicky title bez ztraty pozice/velikosti v ini.
 *
 * Threading: jen UI vlakno.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v stack_history_window.cpp.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef STACK_HISTORY_WINDOW_H
#define STACK_HISTORY_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render Stack History okna (V9).
 *
 * Otevre top-level ImGui okno se stabilnim ID `###stack_history_window`,
 * vykresli info radek + Show events nad plot area, pak SP history plot
 * vyplnujici zbylou plochu okna (= resize plot pri resize okna). Pod
 * plotem volitelne Selected info pokud klik aktivni.
 *
 * Pred renderem vola dbg_disassembled_frame_init (= per-frame guarded
 * init g_dbg_ui + dbg_refresh_tick), aby okno fungovalo i bez hlavniho
 * debug okna nebo Stack Monitor okna.
 *
 * Refresh dat pres dbg_stack_panel_frame_refresh - idempotentni v ramci
 * framu (= soucasne otevreni hlavniho Stack Monitor okna nezpusobi
 * double CMDRQ).
 *
 * @param p_open Pointer na bool s viditelnosti (typicky
 *               &g_gui->showStackHistoryWindow). NULL nebo false = no-op.
 *
 * @pre Volat pouze z UI vlakna mezi NewFrame a EndFrame ImGui.
 */
extern void stack_history_window_render ( bool *p_open );


/**
 * @brief V9.2: Vyžádá si bring-to-front Stack History okna v dalším renderu.
 *
 * Nastaví interní pending flag, který `stack_history_window_render` v
 * dalším volání vyhodnotí a pokud je true, zavolá
 * `ImGui::SetNextWindowFocus()` PŘED `Begin`. Tím se okno přesune do
 * popředí ImGui z-orderu, což backend imgui_impl_sdl3 přes
 * Platform_SetWindowFocus -> SDL_RaiseWindow promítne do OS-level
 * z-orderu (= reálně viditelné nad ostatními okny).
 *
 * Důvod existence shodný s `stack_window_request_focus` - viz tam.
 *
 * Idempotentní v rámci framu.
 *
 * Side effects: nastaví modul-lokální static flag konzumovaný v
 *               stack_history_window_render při dalším volání.
 *
 * @pre Volat pouze z UI vlakna.
 */
extern void stack_history_window_request_focus ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* STACK_HISTORY_WINDOW_H */
