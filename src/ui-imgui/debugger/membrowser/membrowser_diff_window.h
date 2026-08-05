/**
 * @file membrowser_diff_window.h
 * @brief Memory Browser V5 Diff view - samostatné okno.
 *
 * UI nad @ref membrowser_diff_engine + interní snapshot store. Zobrazí
 * side-by-side hex Source A vs Source B s magenta highlight rozdílných
 * bajtů, statistikou (Changed: X of Y, P %) a exportem do textu.
 *
 * Source A/B - každý nezávisle:
 *   - LIVE: aktuální obsah vybraného regionu (čteno per frame přes
 *     dbgapi_regions_read; cached do interního bufferu).
 *   - SNAPSHOT_MANUAL: snapshot pořízený tlačítkem "Snapshot Now" v
 *     diff toolbar (uložen interně, persistuje do zavření okna).
 *   - SNAPSHOT_AUTO: auto-snapshot pořízený automaticky při pause emu
 *     (pokud je aktivní toggle "Auto-snapshot at pause" v toolbar).
 *
 * Region výběr: dropdown přes region_descriptor_label, same pattern jako
 * hlavní Memory Browser. Region musí být zvolen pro každý source nezávisle.
 *
 * Fallback per brief: žádný .mzs load - cesta interní snapshot only
 * (= cíl je funkční diff view, ne perfect file integration). Load .mzs
 * lze přidat v V5.1.
 *
 * Threading: jen UI vlákno. Polling pause edge detect je v render funkci.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_DIFF_WINDOW_H
#define MEMBROWSER_DIFF_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render Memory Diff okna (singleton).
 *
 * Lazy-inicializuje state při prvním volání. Žádný explicit init.
 *
 * @param p_open Pointer na bool řídící viditelnost. Po close křížkem
 *               ImGui vynuluje na false.
 *
 * @pre Volat pouze z UI vlákna mezi NewFrame a EndFrame.
 */
void membrowser_diff_window_render ( bool *p_open );

/**
 * @brief Cleanup interních buffer alokací (volat při shutdown).
 *
 * Uvolní all alokované snapshot/live buffery. Bezpečné volat
 * vícekrát.
 */
void membrowser_diff_window_cleanup ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_DIFF_WINDOW_H */
