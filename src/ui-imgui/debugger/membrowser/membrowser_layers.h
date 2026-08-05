/**
 * @file membrowser_layers.h
 * @brief V1 Layers - color overlay nad hex view a sidebar panel.
 *
 * Modul drží:
 *   - render funkce pravého kolaps panelu "Layers" (set checkboxů + snapshot
 *     tlačítka), volá se z membrowser_window mezi hex view a status bar
 *   - per-byte color resolver - mapuje (region_id, offset, byte) +
 *     enabled layers -> ImU32 background barvu pro hexview cell
 *
 * Mapping region_id -> mhmap cell:
 *   - REGION_KIND_LOGICAL: mhmap_resolve_mem(offset_as_addr) -> region+offset
 *     v g_mhmap, pak lookup cell
 *   - REGION_KIND_RAM: g_mhmap.ram[offset]
 *   - REGION_KIND_ROM_LOWER/UPPER/CGROM/...: per-arch mapping (viz
 *     membrowser_layers.cpp)
 *   - Pokud kind nemá mhmap pole (VRAM mode-specific složitě, PROHIBITED),
 *     vrátí NULL pointer = layer pass.
 *
 * Hot path: rendering volá resolve_cell_color per visible byte. ListClipper
 * limituje na ~32 viditelných řádků × bytes_per_row = max ~1024 calls per
 * frame. mhmap pointer access je O(1) array lookup, freeze_is_frozen je
 * linear scan max 256 entries. Snapshot byte je O(1) array.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_LAYERS_H
#define MEMBROWSER_LAYERS_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>

#include "membrowser_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí pravý kolaps panel "Layers".
 *
 * Volá se z membrowser_view_render() jako BeginChild po hex view, pokud
 * st->layers_panel_open = true. Obsahuje:
 *   - 7 checkboxů (CDL X/R/W, Heatmap, Δ snapshot, Frozen, Symbols)
 *   - Snapshot Now / Reset Snapshot tlačítka (Δ layer)
 *   - Info text - aktuální počet frozen bytů, snapshot existence
 *
 * @param st                Memory Browser state instance (pro layer flags).
 * @param current_region_id Aktuální region (pro snapshot button context).
 */
void membrowser_layers_render_panel ( st_MEMBROWSER_STATE *st,
                                       int current_region_id );

/**
 * @brief Vykreslí toolbar tlačítko pro otevření/zavření Layers panelu.
 *
 * Toggle button [Layers ▶/▼] - voláno z toolbar row v membrowser_window.cpp.
 *
 * @param st Memory Browser state instance.
 */
void membrowser_layers_render_toolbar_toggle ( st_MEMBROWSER_STATE *st );

/**
 * @brief Vypočítá background barvu pro daný byte podle enabled layers.
 *
 * Pořadí priorit (high to low):
 *   1. Frozen (tmavě fialová)
 *   2. Snapshot Δ (magenta)
 *   3. CDL X (světle modrá)
 *   4. CDL R (světle zelená)
 *   5. CDL W (světle červená)
 *   6. Heatmap (gradient cool→hot)
 *
 * @param st         Memory Browser state (pro enabled flags).
 * @param region_id  Region ID z aktuálního snapshotu.
 * @param region_kind en_REGION_KIND.
 * @param sub_id     Disambiguator pro freeze lookup.
 * @param offset     Offset v rámci regionu.
 * @param logical_base Logická Z80 báze (z st_REGION_DESC.logical_base) -
 *                     pro Logical region je 0, jinak 0xFFFFFFFF nebo
 *                     skutečná adresa kde region "obvykle" sedí.
 * @return ImU32 packed color (0 = no highlight, použít default bg).
 */
uint32_t membrowser_layers_compute_byte_bg ( const st_MEMBROWSER_STATE *st,
                                              int region_id,
                                              int region_kind, int sub_id,
                                              uint32_t offset,
                                              uint32_t logical_base );

/**
 * @brief Vrátí true pokud je aspoň jeden non-symbol layer enabled.
 *
 * Performance shortcut pro hexview - když všechny color layers OFF,
 * přeskočí per-byte bg compute úplně.
 */
bool membrowser_layers_any_color_active ( const st_MEMBROWSER_STATE *st );

/**
 * @brief Vypočítá optimální šířku Layers panelu z lokalizovaných labelů.
 *
 * Volá se z membrowser_window při st->layers_panel_width == 0 (= auto)
 * pro výpočet startovní šířky, aby se po lokalizaci do delších jazyků
 * (CZ/DE) nezkracovaly texty checkboxů a tlačítek. Iteruje přes texty
 * panelu (CDL labels, Snapshot/Reset tlačítka, info řádky) a vrací
 * max(CalcTextSize) + reservation pro checkbox/border padding.
 *
 * Musí být voláno v ImGui kontextu (frame already begun) - používá
 * ImGui::CalcTextSize z aktivního fontu.
 *
 * @return Šířka v px clampovaná na 200..600 (= validní user range).
 */
int membrowser_layers_compute_optimal_panel_width ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_LAYERS_H */
