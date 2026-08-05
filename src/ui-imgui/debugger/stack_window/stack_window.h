/*
 * File:   stack_window.h
 *
 * Stack Monitor - samostatne dokovatelne ImGui okno (Variant B layout).
 *
 * Wrapper kolem dbg_stack_panel_render() ze sections/dbg_stack_panel.cpp.
 * Otevre/zavre vlastni ImGui::Begin/End kontext s persistenci pozice
 * a velikosti (pres ImGui ini). Nema zadnou vlastni logiku - veskery
 * render obstarava sdilena dbg_stack_panel_render() funkce, ktera
 * obsadi celou dostupnou plochu uvnitr Begin/End.
 *
 * Visibilita drzi g_gui->showStackWindow.
 *
 * Refresh: kazdy frame ctve stav stacku pres dbgapi (CMD_STACK_DUMP),
 * refresh rizeny pres DbgRefreshController v dbg_stack_panel.cpp.
 * Threading: jen UI vlakno.
 *
 * V0 stav: pasivni hex dump 40 radku kolem SP + tlacitko "Set BP from
 * SP-256" pro rychly SP_THRESHOLD BPT. Multi-region / watermark / decoder
 * prijdou v V1+.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v stack_window.cpp.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef STACK_WINDOW_H
#define STACK_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavniho Stack Monitor okna.
 *
 * Vola se z main_window kazdy frame, pokud `*p_open` je true. Pokud
 * uzivatel okno zavre pres (X), ImGui vynuluje *p_open na false -
 * caller musi mit p_open ve sve persistent strukture.
 *
 * Wrapper otevira ImGui::Begin se stabilnim ID `###stack_window` (= title
 * se muze menit pro arch-specific oznaceni bez ztraty pozice/velikosti).
 * Render delegovan na dbg_stack_panel_render(), ktera obsadi celou
 * dostupnou plochu.
 *
 * @param p_open Pointer na bool ridici viditelnost (typicky
 *               &g_gui->showStackWindow).
 *
 * Side effects: muze pridat novy SP_THRESHOLD BPT (klik na "Set BP from
 *               SP-256" tlacitko v sticky header).
 *
 * @pre Volat pouze z UI vlakna mezi NewFrame a EndFrame ImGui.
 */
extern void stack_window_render ( bool *p_open );


/**
 * @brief V9.2: Vyžádá si bring-to-front Stack Monitor okna v dalším renderu.
 *
 * Nastaví interní pending flag, který `stack_window_render` v dalším volání
 * vyhodnotí a pokud je true, zavolá `ImGui::SetNextWindowFocus()` PŘED
 * `Begin`. Tím se okno přesune do popředí ImGui z-orderu, což backend
 * imgui_impl_sdl3 přes Platform_SetWindowFocus -> SDL_RaiseWindow promítne
 * do OS-level z-orderu (= reálně viditelné nad ostatními okny).
 *
 * Existuje, protože `ImGui::SetWindowFocus(name)` volané AFTER Begin
 * cílového okna v rámci téhož framu neaplikuje platform raise - okno už
 * bylo v daném framu zaBeginováno. Request flag pattern přesune Set focus
 * BEFORE Begin v dalším framu, což ImGui správně propaguje do backendu.
 *
 * Idempotentní v rámci framu (= násobné volání = jeden focus request).
 *
 * Side effects: nastaví modul-lokální static flag konzumovaný v
 *               stack_window_render při dalším volání.
 *
 * @pre Volat pouze z UI vlakna.
 */
extern void stack_window_request_focus ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* STACK_WINDOW_H */
