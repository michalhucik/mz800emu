/*
 * stack_history_window.cpp - V9 samostatne okno s SP history sparkline.
 *
 * Plot resize s velikosti okna (X i Y) - umoznuje detailni prubeh SP v
 * case ve velkem zobrazeni, paralelne s hlavnim Stack Monitor oknem.
 * Sdileny state g_stack.selected_history_idx / show_events s hlavni
 * SP history sekci v Stack Monitor okne (= klik v jednom okne se projevi
 * v druhem).
 *
 * Render delegovan na sdilene helpery dbg_stack_panel_render_history_plot
 * a dbg_stack_panel_render_history_info, ktere byly extrahovany z V2.1
 * stack_panel_render_history_sparkline pri V9 refaktoru.
 *
 * Refresh dat (regions + history) probiha pres
 * dbg_stack_panel_frame_refresh(), ktery je sdileny s hlavnim oknem a
 * idempotentni v ramci framu.
 *
 * V9.1: top action row - tlacitko [Stack Monitor] (otevre + focusne
 * hlavni okno) + right-aligned [SP history] checkbox (toggle history
 * recording pres dbg_stack_panel_set_history_enabled helper, sdileny
 * shadow s hlavnim oknem).
 *
 * Stabilni ID: "Stack History###stack_history_window".
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

#include <stdio.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "stack_history_window.h"
#include "ui-imgui/debugger/stack_window/stack_window.h"
#include "ui-imgui/debugger/sections/dbg_stack_panel.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/bootstrap/myimgui.h"


/**
 * @brief Render top-level Stack History okna.
 *
 * Layout:
 *   - top info row: Samples/Slope + (Selected) + Show events
 *   - separator
 *   - plot rect vyplnujici zbylou plochu okna (= grow s velikosti okna)
 *
 * Pri vypnute history recording (= dbg_stack_panel_history_enabled() ==
 * false) vykresli hint a vraci se bez vykresleni plotu.
 *
 * @param p_open Pointer na bool s viditelnosti. NULL nebo false = no-op.
 */
/**
 * @brief V9.2/V9.3: Pending flag pro bring-to-front request.
 *
 * Nastaven pres `stack_history_window_request_focus()` z cross-link
 * buttonu v jinem okne (= Stack Monitor "Stack History" tlacitko).
 * Konzumovan v `stack_history_window_render` PRED Begin
 * (`SetNextWindowFocus`) i PO Begin (V9.3 explicitni
 * `Platform_SetWindowFocus` pro OS-level raise v multi-viewport mode).
 * Detailni popis patternu + root cause viz s_focus_pending v
 * stack_window.cpp.
 */
static bool s_focus_pending = false;


extern "C" void stack_history_window_request_focus ( void ) {
    s_focus_pending = true;
}


extern "C" void stack_history_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Per-frame init refresh kontroleru - bez tohoto by okno ukazovalo
     * zamrzlou cache pokud je otevrene samostatne (bez hlavniho debug
     * okna a bez Stack Monitor okna). */
    dbg_disassembled_frame_init ( );

    /* Default size pri prvnim pouziti - 873x539 px (= dostatecne pro
     * pohodlne sledovani sparkline + info radku). User si pak velikost
     * upravi a ImGui ji ulozi pres ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 873.0f, 539.0f ),
                                ImGuiCond_FirstUseEver );

    /* V9.2: Konzumace pending focus requestu (cross-link button z hlavniho
     * Stack Monitor okna). Viz stack_window.cpp s_focus_pending pro detail
     * patternu (SetNextWindowFocus PRED Begin = ImGui pre frame preview
     * okno do popredi z-orderu, backend volanim SDL_RaiseWindow promitne
     * change do OS-level z-orderu). */
    bool was_pending = s_focus_pending;
    if ( s_focus_pending ) {
        ImGui::SetNextWindowFocus ( );
        s_focus_pending = false;
    };

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;

    /* Stabilni ID pres ###. */
    if ( !ImGui::Begin ( _L ( "Stack History###stack_history_window" ),
                          p_open, wflags ) ) {
        ImGui::End ( );
        return;
    };

    /* V9.3: Multi-viewport platform raise (viz stack_window.cpp pro
     * detailni komentar root-cause). ImGui SetNextWindowFocus ZMENI
     * pouze interni z-order, nikoliv Platform_SetWindowFocus -> v
     * multi-viewport mode okno v separate platform window zustava
     * pod ostatnimi OS window. Po Begin explicitne zavolame
     * Platform_SetWindowFocus (= SDL_RaiseWindow). */
    if ( was_pending ) {
        ImGuiViewport *vp = ImGui::GetWindowViewport ( );
        ImGuiViewport *main_vp = ImGui::GetMainViewport ( );
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO ( );
        if ( vp && vp != main_vp && vp->PlatformWindowCreated
             && pio.Platform_SetWindowFocus ) {
            pio.Platform_SetWindowFocus ( vp );
        };
    };

    /* Refresh dat (sdileny s hlavnim Stack Monitor oknem - idempotentni
     * per frame). */
    dbg_stack_panel_frame_refresh ( );

    /* V9.1: top action row - vlevo tlacitko [Stack Monitor] (otevre +
     * focusne hlavni Stack Monitor okno), vpravo zarovnany [SP history]
     * checkbox (toggle history recording, sdileny shadow s hlavnim
     * oknem pres dbg_stack_panel_set_history_enabled helper).
     *
     * Cross-link zajistuje, ze i kdyz user otevre samostatne Stack
     * History okno (napr. pres Alt+Shift+H), muze odtud rychle dostat
     * Stack Monitor na vrch obrazovky bez prochazeni menu. */
    if ( ImGui::Button ( _L ( "Stack Monitor###sh_open_stack_mon" ) ) ) {
        g_gui->showStackWindow = true;
        /* V9.2: request_focus misto ImGui::SetWindowFocus(name). Pattern
         * pres pending flag + SetNextWindowFocus PRED Begin v dalsim framu
         * cizovho okna - viz s_focus_pending v stack_window.cpp pro detail.
         * Puvodni SetWindowFocus(name) volane AFTER Begin cilovho okna v
         * tomtez framu neaplikovalo platform raise (= title flash, OS
         * z-order beze zmeny). */
        stack_window_request_focus ( );
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _ ( "Open and focus the Stack Monitor window" ) );
    };

    ImGui::SameLine ( );

    /* Right-align checkbox: spocti sirku samotneho widgetu (label +
     * checkbox box + inner spacing) a umisti kurzor o tolik vlevo od
     * praveho okraje radku. */
    bool hist_enabled = dbg_stack_panel_history_enabled ( );
    float cb_w = ImGui::CalcTextSize ( _ ( "SP history" ) ).x
               + ImGui::GetFrameHeight ( )
               + ImGui::GetStyle ( ).ItemInnerSpacing.x;
    float avail_w = ImGui::GetContentRegionAvail ( ).x;
    if ( avail_w > cb_w ) {
        ImGui::SetCursorPosX (
            ImGui::GetCursorPosX ( ) + avail_w - cb_w );
    };
    if ( ImGui::Checkbox ( _L ( "SP history###sh_top_history_toggle" ),
                            &hist_enabled ) )
    {
        /* Posle CMDRQ STACK_HISTORY_ENABLE pres sdileny helper -
         * stejna logika jako sticky header checkbox v hlavnim Stack
         * Monitor okne. */
        dbg_stack_panel_set_history_enabled ( hist_enabled );
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _ ( "Record SP changes for sparkline + stack creep "
                "detection" ) );
    };

    ImGui::Separator ( );

    /* Pokud history neni zapnuta - hint user otevrit Stack Monitor a
     * zapnout. */
    if ( !dbg_stack_panel_history_enabled ( ) ) {
        ImGui::TextDisabled ( "%s",
            _ ( "(SP history is disabled, enable in Stack Monitor)" ) );
        ImGui::End ( );
        return;
    };

    /* Info radek nad plotem - Samples/Slope + Selected + Show events.
     * Suffix "win" pro ImGui ID Clear/Show events tlacitek - rozliseni
     * od hlavni Stack Monitor SP history sekce (suffix "main").
     * V9.1: druhy parametr false = neukazovat [Stack History] tlacitko
     * (= redundance, jsme jiz uvnitr Stack History okna; top action
     * row jiz ma [Stack Monitor] btn pro zpetny smer). */
    dbg_stack_panel_render_history_info ( "win", false );

    ImGui::Separator ( );

    /* Plot area = zbyly prostor okna (= grow s velikosti okna). Minimum
     * 60 px vyska, 80 px sirka - chranene proti prilis malemu oknu. */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float plot_w = avail.x;
    float plot_h = avail.y;
    if ( plot_w < 80.0f ) plot_w = 80.0f;
    if ( plot_h < 60.0f ) plot_h = 60.0f;

    dbg_stack_panel_render_history_plot ( plot_w, plot_h );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
