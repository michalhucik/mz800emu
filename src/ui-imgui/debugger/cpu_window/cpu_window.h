/*
 * File:   cpu_window.h
 *
 * CPU Registers - samostatné dokovatelné ImGui okno (Variant B layoutu).
 *
 * Wrapper kolem dbg_cpu_panel_render() z sections/dbg_cpu_panel.cpp.
 * Otevře/zavře vlastní ImGui::Begin/End kontext s persistencí pozice
 * a velikosti (přes ImGui ini). Nemá žádnou vlastní logiku - veškerý
 * render obstarává sdílená dbg_cpu_panel_render() funkce, která
 * obsadí celou dostupnou plochu uvnitř Begin/End.
 *
 * Visibilita drží g_gui->showCpuWindow.
 *
 * Refresh: každý frame čte aktuální stav CPU přes dbgapi (GET_ALL_REGS
 * + GET_CPU_FLAGS), refresh řízený přes DbgRefreshController v
 * dbg_cpu_panel.cpp. Threading: jen UI vlákno.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v cpu_window.cpp.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef CPU_WINDOW_H
#define CPU_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavního CPU Registers okna.
 *
 * Volá se z main_window každý frame, pokud `*p_open` je true.
 * Pokud uživatel okno zavře přes (X), ImGui vynuluje *p_open na
 * false - caller musí mít p_open ve své persistent struktuře.
 *
 * Wrapper otevírá ImGui::Begin se stabilním ID `###cpu_window`
 * (= title se může měnit pro arch-specific označení bez ztráty
 * pozice/velikosti). Render delegovan na dbg_cpu_panel_render(),
 * která obsadí celou dostupnou plochu.
 *
 * @param p_open Pointer na bool řídící viditelnost (typicky
 *               &g_gui->showCpuWindow).
 *
 * Side effects: může spustit auto-pauzu emulátoru přes
 *               dbg_autopause_on_interaction() (klik na editovatelnou
 *               hodnotu registru / flag bitu v running mode).
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame ImGui.
 */
extern void cpu_window_render ( bool *p_open );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* CPU_WINDOW_H */
