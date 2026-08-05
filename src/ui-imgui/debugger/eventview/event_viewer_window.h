/*
 * File:   event_viewer_window.h
 *
 * Events panel - samostatné dokovatelné ImGui okno pro real-time
 * pohled na ring eventů z @c emulator/debugger/trace/eventlog.c.
 *
 * Vlna 1 Commit 7: pouze Log tab s tabulkou
 *   Frame | Cycle | Sline | Px | PC | Cat | Sub | Detail
 * + filter řádek + per-kategorie visibility checkboxy + quick filter
 * preset buttony (Only IRQ / banking / video / PSG / memory / Hide noise)
 * + Total/Filtered counters + cross-window context menu
 * ("Show in disasm" / "Show port in Overview").
 *
 * Vlna 2 Commit 10: Strip tab base canvas - Mesen-style 2D vizualizace
 * per-frame eventů. X = pixel column 0..VIDEO_SCREEN_WIDTH-1, Y =
 * scanline 0..VIDEO_SCREEN_HEIGHT-1. Bod per event s color = kategorie,
 * size = subtype priority. Strip toolbar: "Fit to window" vs "1:1" mode,
 * zoom slider 0.5..4x, per-kategorie color picker v Colors collapsible
 * sekci.
 *
 * Vlna 2 Commit 11: Strip hit-test + hover tooltip + click-pause UX.
 * Per-frame draw cache (vector<DRAWN_EVENT>) populovaný v render loopu,
 * lineární hit-test s radius + slack tolerance. Tooltip s Pxclk/Frame/
 * Sline/Px/PC/Cat/Sub/Detail (reuse Vlna 1 dekódérů). Single click =
 * select highlight (bílý outline), double click = pause emu + dbg_disasm
 * focus, right click = context popup (Pause+disasm / Pause here /
 * Show in Log tab / Copy). Cross-tab switch přes
 * ImGuiTabItemFlags_SetSelected + SetScrollHereY ve scroll cíli.
 *
 * Lifecycle: při open / close volá @c eventlog_notify_window_open(), což
 * v módu @c EVENTLOG_MODE_WHEN_WINDOW_OPEN spouští / zastavuje recording
 * ringu (= úspora hot-path overhead u uživatelů kteří okno nepoužívají).
 *
 * Visibilita drží @c g_gui->showEventViewerWindow.
 *
 * Persistence: vlastní cfg sekce @c [EVENT_VIEWER_WINDOW] (klíče
 * @c window_open, @c filter_expression). Aby emu vrstva nemusela
 * vědět o UI flagu, sekci registruje a propaguje samostatně UI vrstva.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef EVENT_VIEWER_WINDOW_H
#define EVENT_VIEWER_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Render Events okna.
 *
 * Volá se z @c main_window každý frame, pokud @c *p_open je @c true.
 * Pokud uživatel okno zavře přes (X), ImGui vynuluje @c *p_open na
 * @c false - caller drží @c p_open ve své persistent struktuře
 * (typicky @c &g_gui->showEventViewerWindow).
 *
 * Side effects:
 *   - Detekuje open / close edge a volá @c eventlog_notify_window_open().
 *   - Quick filter button nebo Clear button mutuje interní UI stav
 *     (filter expression, ring obsah).
 *
 * Thread safety: jen UI vlákno. Čtení ringu přes
 * @c eventlog_get_event() je read-only proti hot-path emu vláknu;
 * krátký data race vůči souběžnému zápisu má za následek nejvýše
 * "ulomek" eventu zobrazený jeden frame.
 *
 * @param p_open  Pointer na bool řídící viditelnost.
 */
extern void event_viewer_window_render ( bool *p_open );


/**
 * @brief Cross-window: vyžádá fokus Events okna v příštím framu.
 *
 * Nastaví interní pending flag - příští render volání provede
 * @c ImGui::SetNextWindowFocus() PŘED @c ImGui::Begin (= z-order raise
 * + ImGui Platform_SetWindowFocus pro OS-level raise v multi-viewport
 * režimu). Po aplikaci se flag clearuje.
 *
 * Idempotentní v rámci framu. Thread safety: pouze UI vlákno.
 */
extern void event_viewer_window_request_focus ( void );


/**
 * @brief Registrace cfgelementů Events okna do daného CFGMOD.
 *
 * Volá se z @c debugger_init() po
 * @c cfgroot_register_new_module("EVENT_VIEWER_WINDOW"). Vnitřně
 * inicializuje UI defaulty a registruje cfg klíče
 * (@c window_open, @c filter_expression).
 *
 * @param cmod_void  Pointer na @c st_CFGMODULE (cast přes @c void* aby
 *                   header nemusel includovat cfgfile interní typy).
 */
extern void event_viewer_window_register_persistence ( void *cmod_void );


/**
 * @brief Aplikuje načtenou hodnotu @c window_open z cfg do
 *        @c g_gui->showEventViewerWindow.
 *
 * Cfg parse / propagate u @c [EVENT_VIEWER_WINDOW] proběhne v
 * @c debugger_init() PŘED inicializací @c g_gui (= MyImGui struktura
 * existuje, ale flag se zatím nečte). Tato funkce se volá z
 * @c myimgui_init() callbacku po vytvoření @c g_gui aby se uložené
 * UI volby propagovaly do živé struktury.
 *
 * Idempotentní - opakované volání pouze přepíše flag stejnou hodnotou.
 */
extern void event_viewer_window_apply_persisted ( void );


/**
 * @brief Registrace cfgelementů [EVENT_LOG_FILTERS] sekce (Commit 15).
 *
 * Samostatná sekce pro user-defined saved filter presets. Klíče jsou
 * předregistrované jako fixní pole @c preset_NN_name a @c preset_NN_expr
 * pro @c NN = 00..31 (= max 32 presetů per session, viz
 * @ref EVW_SAVED_PRESET_MAX_COUNT). Prázdný @c name = slot nepoužitý.
 *
 * Volá se z @c debugger_init() po
 * @c cfgroot_register_new_module("EVENT_LOG_FILTERS"). Defaulty
 * presetů = prázdné (= žádná pre-seed library; user si naplní sám).
 *
 * @param cmod_void  Pointer na @c st_CFGMODULE.
 */
extern void event_viewer_window_register_filters_persistence ( void *cmod_void );


/**
 * @brief Aplikuje načtené @c preset_NN_name / @c preset_NN_expr z cfg
 *        do interní @c std::vector saved presetů.
 *
 * Volá se z @c debugger_init() po @c cfgmodule_propagate() sekce
 * @c [EVENT_LOG_FILTERS]. Cfg-vlastněné string buffery po propagate
 * obsahují uložené hodnoty - tato funkce je zkopíruje do
 * @c st_EVW_SAVED_PRESET vektoru. Prázdné name slotů přeskočí.
 *
 * Idempotentní - opakované volání zahodí starý obsah vektoru a načte
 * znovu.
 */
extern void event_viewer_window_apply_filters_persisted ( void );


/**
 * @brief Před cfg save serialize aktuální in-memory presetový vektor
 *        zpět do cfg-vlastněných stringů.
 *
 * Volá se z UI před @c cfgroot_save_to_file(). UI vrstva nemá hook
 * "before save", takže serializace běží přímo z handleru "Save current
 * as..." popupu + "Delete" + při shutdown. Pro bezpečnost také před
 * každým re-save (= idempotentní stav cfg).
 */
extern void event_viewer_window_sync_filters_to_cfg ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* EVENT_VIEWER_WINDOW_H */
