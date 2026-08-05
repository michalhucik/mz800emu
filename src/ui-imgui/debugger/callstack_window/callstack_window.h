/*
 * File:   callstack_window.h
 *
 * Callstack - samostatne dokovatelne ImGui okno (mutant callstack Fáze 2C).
 *
 * Wrapper kolem dbg_callstack_render() ze sections/dbg_callstack.cpp.
 * Otevre/zavre vlastni ImGui::Begin/End kontext s persistenci pozice
 * a velikosti (pres ImGui ini). Nema zadnou vlastni logiku - veskery
 * render obstarava sdilena dbg_callstack_render() funkce.
 *
 * Visibilita drzi g_gui->showCallstackWindow.
 *
 * Refresh: kazdy frame ctve stav shadow stacku pres dbgapi
 * (CMD_GET_CALLSTACK), refresh tick rizen pres DbgRefreshController
 * z dbg_disassembled_frame_init().
 *
 * Threading: jen UI vlakno.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v callstack_window.cpp.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef CALLSTACK_WINDOW_H
#define CALLSTACK_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavniho Callstack okna.
 *
 * Vola se z main_window kazdy frame, pokud `*p_open` je true. Pokud
 * uzivatel okno zavre pres (X), ImGui vynuluje *p_open na false -
 * caller musi mit p_open ve sve persistent strukture.
 *
 * Wrapper otevira ImGui::Begin se stabilnim ID `###callstack_window`
 * (= title se muze menit pro arch-specific oznaceni bez ztraty
 * pozice/velikosti). Render delegovan na dbg_callstack_render(),
 * ktera obsadi celou dostupnou plochu.
 *
 * @param p_open Pointer na bool ridici viditelnost (typicky
 *               &g_gui->showCallstackWindow).
 *
 * @pre Volat pouze z UI vlakna mezi NewFrame a EndFrame ImGui.
 */
extern void callstack_window_render ( bool *p_open );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* CALLSTACK_WINDOW_H */
