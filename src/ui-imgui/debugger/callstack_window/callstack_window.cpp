/*
 * callstack_window.cpp - Callstack okno wrapper (mutant callstack Fáze 2C).
 *
 * Tenky wrapper kolem dbg_callstack_render() ze sections/dbg_callstack.cpp.
 * Vytvari top-level ImGui okno s vlastnim title a persistentni geometrii
 * (ImGui ini), uvnitr vola sdileny render core.
 *
 * Stabilni ID: "Callstack###callstack_window" (= prefix lze pozdeji
 * obohatit o arch / mode bez invalidace pozice/velikosti v ini).
 *
 * Default size: 500x510 px (= 7 columns tabulka + toolbar + footer).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "callstack_window.h"
#include "ui-imgui/debugger/sections/dbg_callstack.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/auto_layout.h"


extern "C" void callstack_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Per-frame init refresh kontroleru. Bez tohoto by Callstack okno
     * otevrene samostatne (= bez hlavniho debug okna) nikdy nedostalo
     * should_refresh = true a panel by ukazoval zamrzlou cache.
     * Funkce je idempotentni v ramci jednoho framu. */
    dbg_disassembled_frame_init ( );

    /* Default size pri prvnim pouziti - 11 columns (5 hex + Sym stretch
     * + Kind + raster Frame/Sline/Px/Pxclk + Cyc-in) + toolbar + footer.
     * User-modified velikost si ImGui zapamatuje pres ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 880.0f, 560.0f ),
                                ImGuiCond_FirstUseEver );

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;

    /* Stabilni ID pres ###. */
    const char *title = _L ( "Callstack (experimental)###callstack_window" );
    auto_layout_first_use_portrait ( title, 880.0f, 560.0f );
    if ( !ImGui::Begin ( title, p_open, wflags ) ) {
        ImGui::End ( );
        return;
    };

    /* Veskery vlastni obsah - toolbar + tabulka + footer. */
    dbg_callstack_render ( );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
