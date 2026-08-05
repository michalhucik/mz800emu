/*
 * io_window.h - I/O Ports viewer panel (D.7).
 *
 * Strukturovany pohled na vsechny dokumentovane I/O porty MZ-800 /
 * MZ-700 / MZ-1500 s bit-by-bit popisky a live hodnotami. Inspirovano
 * no$gba IO Map viewer.
 *
 * Layout:
 *   +----------------------------------------------+
 *   | I/O Ports                              [X]  |
 *   +----------------------------------------------+
 *   | Addr | Name        | Hex | Bin      | R/W   |
 *   |------|-------------|-----|----------|-------|
 *   | 0CCh | GDG WF      | 0x42| 01000010 | W     | -- expand
 *   | 0CDh | GDG RF      | ...                    |
 *   | 0CEh | GDG DMD/Sts | 0x07| 00000111 | R/W   | v expanded
 *   |        bit 7 [_] : -                        |
 *   |        bit 2 [v] : DMD2 (high res)          |
 *   |        ...                                  |
 *   |        decoded: screen=320x200, gfx-bits=3  |
 *   +----------------------------------------------+
 *
 * Right-click na port -> context menu "Add IORQ R BP" /
 * "Add IORQ W BP" -> otevre Edit BP panel s pre-fill type+port
 * (volá bpt_edit_panel_open_new_iorq).
 *
 * Live update: kazdy frame cte aktualni hodnoty z g_* state. Refresh
 * je polling-based; pri pause emulatoru hodnoty zustavaji stale.
 *
 * Visibility: g_gui->showIoWindow (pridano do MyImGui).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef IO_WINDOW_H
#define IO_WINDOW_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Render hlavniho I/O Ports okna.
     *
     * Vola se z imgui_main_window kazdy frame, pokud
     * g_gui->showIoWindow == true. Po zavreni krizkem ImGui
     * vynuluje *p_open na false.
     *
     * @param p_open Pointer na bool ridici viditelnost
     *               (typicky &g_gui->showIoWindow).
     *
     * Side effects: muze otevrit Edit BP panel pres
     * bpt_edit_panel_open_new_iorq pri "Add IORQ BP" akci.
     *
     * Threading: jen UI vlakno. Cteni hodnot z g_* je side-effect
     * free, race s emu vlaknem ma nejvyse efekt "stale display
     * jeden frame", ne korupci.
     */
    extern void io_window_render(bool *p_open);

    /**
     * @brief Toggle viditelnost okna.
     *
     * Prepne g_gui->showIoWindow. Vola se z menu polozky.
     */
    extern void io_window_show_hide(void);

    /**
     * @brief Registrace persistence klíčů panelu I/O Ports do cfgmain.
     *
     * Vytvoří modul "IO_PORTS_PANEL" v g_cfgmain a registruje klíče:
     *   - collapse_<chip>      (bool, default 0 = expanded) - per chip skupina
     *   - history_capacity     (uint, default 10000, range 1000..50000)
     *   - history_auto_follow  (bool, default 1)
     *   - tracking_active      (bool, default 0 = user opt-in)
     *   - record_mask          (text, 64-hex bitmap pro 256 portu,
     *                           default vse 'F' = vse zaznamenavano)
     *
     * Po registraci volat cfgmodule_parse + cfgmodule_propagate (= viz
     * existující debugger.c pattern).
     *
     * @param cmod_void Pointer na st_CFGMODULE (typeless kvůli C/C++ headeru).
     */
    extern void io_window_register_persistence(void *cmod_void);

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* IO_WINDOW_H */
