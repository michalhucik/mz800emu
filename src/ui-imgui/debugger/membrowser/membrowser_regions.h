/**
 * @file membrowser_regions.h
 * @brief V2 Regions tree - levý sidebar s hierarchickým výběrem regionu.
 *
 * Nahrazuje (resp. doplňuje) plochý Region dropdown z V0. Tree má per-arch
 * filtraci: na MZ-700 se neukazují plane III/IV / PCG / VRAM v 800 modu,
 * na MZ-1500 se ukazují PCG 1..3, atd. Disconnected HW (Memext bez
 * Connect, Ramdisk bez image) zůstává v stromu zobrazený jako disabled
 * (gray-out) - uživatel vidí že platforma má feature, ale není aktivní.
 *
 * Hierarchie (zkráceně, viz V2 brief):
 *   - Logical Z80 (current banking view)
 *   - User RAM (64 KB)
 *   - Monitor ROM { lower, upper }
 *   - CG-ROM
 *   - VRAM { Physical Planes { I, II, III, IV }, 700 char, 700 attr }
 *   - CG-RAM (700 mode)
 *   - PCG (MZ-1500) { Bank 1, 2, 3 }
 *   - Memext { RAM banks, FLASH banks }
 *   - Ramdisk STD { Banks }
 *   - Ramdisk PEZIK 68 / E8 { Banks }
 *
 * Klik na leaf node provede stejný switch jako V0 Region dropdown
 * (přepíše current_key + cursor reset + persist save). Group nody jen
 * collapse/expand bez akce.
 *
 * Performance: tree se regeneruje z dbgapi region snapshotu (refreshovaný
 * per frame v membrowser_window). Snapshot už existuje, my jen iterujeme.
 * Pro 64 KB Ramdisk STD = 256 banks = 256 leafs - ImGui clipper není
 * potřeba, tree node je levný.
 *
 * Layout v okně:
 *   [Regions sidebar | splitter | Hex view | splitter | Layers panel]
 *   Default Regions panel = OPEN (= primary navigation). Layers panel
 *   default OFF (volitelný power-user feature).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_REGIONS_H
#define MEMBROWSER_REGIONS_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>
#include <stdint.h>

#include "membrowser_state.h"
#include "membrowser_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí toolbar tlačítko pro otevření/zavření Regions panelu.
 *
 * Toggle button [Regions ▶/▼] - voláno z toolbar row 1 v membrowser_window.cpp
 * vedle Layers tlačítka. Stav se ukládá ve st->regions_panel_open.
 *
 * @param st Memory Browser state instance.
 */
void membrowser_regions_render_toolbar_toggle ( st_MEMBROWSER_STATE *st );

/**
 * @brief Vykreslí Regions tree do aktuálního child window.
 *
 * Caller je zodpovědný za BeginChild/EndChild kolem volání - obvykle to
 * dělá render path v membrowser_window. Funkce iteruje region snapshot
 * (z snap parametru) a buduje strom přes ImGui::TreeNode. Klik na leaf
 * přepne state->current_key + cursor reset + persist save.
 *
 * Disconnected regiony (z REGION snapshotu r->connected == 0) se zobrazí
 * jako TextDisabled bez selectable - uživatel vidí HW kategorii, ale
 * nemůže ji aktivovat.
 *
 * Per-arch filtrace: rozdíly mezi MZ-800/700/1500 jsou ošetřené tím, že
 * dbgapi snapshot pro každý arch vrací jen relevantní regiony - tree
 * skupiny bez položek nezobrazujeme.
 *
 * @param st         Memory Browser state (pro current_key a panel_open flag).
 * @param snap       Snapshot aktuálních regionů z dbgapi (per-frame).
 */
void membrowser_regions_render_tree ( st_MEMBROWSER_STATE *st,
                                       const st_MEMBROWSER_REGION_SNAPSHOT *snap );

/**
 * @brief Vypočítá optimální startovní šířku Regions panelu (px).
 *
 * Volá se při st->regions_panel_width == 0 (= auto-compute first time).
 * Iteruje typické labely tree skupin (= delší texty po lokalizaci do CZ)
 * a vrací max(CalcTextSize) + reservation na indent/checkbox/border.
 *
 * Musí být voláno v ImGui kontextu (frame already begun).
 *
 * @return Šířka v px clampovaná na 180..420 (validní user range).
 */
int membrowser_regions_compute_optimal_panel_width ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_REGIONS_H */
