/*
 * File:   sym_window.h
 *
 * Symbol Browser panel - dokovatelné ImGui okno pro správu symbol DB
 * (D.8.6).
 *
 * Funkce:
 *   - Tabulka přes všechny symboly z sym_db (Name | Addr | Bank |
 *     Source | Comment)
 *   - Search filter (= substring match na Name nebo Comment)
 *   - "Load From..." (= file dialog, auto-detekce formátu dle suffixu)
 *   - "Save .lbl As..." (= serializace jen LBL záznamů)
 *   - "Add User Label" form (addr + name + comment)
 *   - "Delete Selected" (= jen LBL záznamy, ostatní z buildchain
 *     nelze mazat - reload souboru je obnoví)
 *   - Inline edit Comment column - automatická promote na LBL
 *
 * Viditelnost: g_gui->showSymbolsWindow (přidáno do MyImGui).
 *
 * Threading: jen UI vlákno; sym_db lookup volá i disassembler section,
 * ale ten také běží na UI vlákně. Žádný přístup z hot path EMU vlákna.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef SYM_WINDOW_H
#define SYM_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render hlavního Symbol Browser okna.
 *
 * Volá se z imgui_main_window každý frame, pokud je
 * g_gui->showSymbolsWindow == true. Po zavření křížkem ImGui
 * vynuluje *p_open na false.
 *
 * @param p_open Pointer na bool řídící viditelnost
 *               (typicky &g_gui->showSymbolsWindow).
 *
 * Side effects: může mutovat sym_db storage (Add / Delete / Set comment
 * / Load file / Save file).
 *
 * Threading: jen UI vlákno.
 */
extern void sym_window_render ( bool *p_open );


/**
 * @brief Toggle viditelnost okna.
 *
 * Přepne g_gui->showSymbolsWindow. Volá se z menu položky.
 */
extern void sym_window_show_hide ( void );


/**
 * @brief V1.E.6.A - otevře okno a požaduje focus na symbol s danou adresou.
 *
 * Použití: routing z Activity okna (= dvojklik na řádek s
 * entity_kind = DBGAPI_ENTITY_KIND_SYMBOL, entity_id = adresa).
 * Nastaví show flag, vyčistí filter (aby symbol byl viditelný)
 * a označí selection podle sym_db_lookup_by_addr(addr, 0). Render-loop
 * při dalším frame scrollne na cílový řádek.
 *
 * Pokud symbol s danou adresou neexistuje, focus se uplatní pouze
 * jako otevření okna a vyčištění filtru (= žádný selection change).
 *
 * Thread-safety: UI vlákno only.
 *
 * @param addr  CPU 16-bit adresa (= entity_id z MSG_DATA).
 */
extern void sym_window_focus_addr ( uint16_t addr );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* SYM_WINDOW_H */
