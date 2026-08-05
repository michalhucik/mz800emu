/*
 * stack_window.cpp - Stack Monitor v samostatnem plovoucim okne.
 *
 * Tenky wrapper kolem dbg_stack_panel_render() ze sections/dbg_stack_panel.cpp.
 * Vytvari top-level ImGui okno s vlastnim title a persistentni geometrii
 * (ImGui ini), uvnitr vola sdileny render core.
 *
 * Layout obsahu: viz dbg_stack_panel.cpp - sticky header se SP / Depth /
 * Region + tlacitko "Set BP from SP-256", pod tim hex dump tabulka
 * 40 radku kolem aktualniho SP. Tady je pouze obalka okna.
 *
 * Stabilni ID: "Stack Monitor###stack_window" (= prefix lze pozdeji
 * obohatit o arch / mode bez invalidace pozice/velikosti v ini).
 *
 * Default size: explicitni hint pri prvnim pouziti (40 radku hex dumpu
 * je vysoke, default ImGui auto-fit by smrsklo na nejmensi). Po prvnim
 * pouziti si ImGui zapamatuje user-modified velikost pres ini.
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

#include "stack_window.h"
#include "ui-imgui/debugger/sections/dbg_stack_panel.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/auto_layout.h"


/**
 * @brief Render top-level Stack Monitor okna.
 *
 * Otevre ImGui::Begin se stabilnim ID a deleguje obsah na
 * dbg_stack_panel_render(). Stejny pattern jako cpu_window_render /
 * bm_window_render.
 *
 * Pred renderem vola dbg_disassembled_frame_init() (= per-frame guarded
 * init g_dbg_ui + dbg_refresh_tick). Bez tohoto by Stack okno otevrene
 * samostatne (= bez hlavniho debug okna a bez CPU okna) nikdy nedostalo
 * should_refresh = true a panel by ukazoval zamrzlou cache. Funkce je
 * idempotentni v ramci jednoho framu, takze soucasne otevreni Stack
 * okna i hlavniho debuggeru nezpusobi dvojity refresh.
 *
 * @param p_open Pointer na bool s viditelnosti. NULL nebo false = no-op.
 */
/**
 * @brief V9.2/V9.3: Pending flag pro bring-to-front request.
 *
 * Nastaven přes `stack_window_request_focus()` z cross-link buttonu v
 * jiném okně (= Stack History "Stack Monitor" tlačítko). Konzumován v
 * `stack_window_render`:
 *
 * 1. PŘED `Begin` (pokud true): `ImGui::SetNextWindowFocus()` přesune
 *    okno do popředí v ImGui-internal z-orderu (g.Windows array).
 * 2. PO `Begin` (V9.3 fix): pokud okno je v separate platform viewport
 *    (multi-viewport mode), explicitně zavoláme
 *    `Platform_SetWindowFocus(viewport)` = `SDL_RaiseWindow` pro
 *    OS-level bring-to-front.
 *
 * Root cause V9.3: ImGui core `FocusWindow()` (= co dělá
 * `SetNextWindowFocus`) volá jen `BringWindowToFocusFront` +
 * `BringWindowToDisplayFront` = interní pole. `Platform_SetWindowFocus`
 * callback je v imgui.cpp volán POUZE z `NavUpdateWindowingApplyFocus`
 * (Ctrl+Tab handler). API-level focus změny tedy do platform backendu
 * neprojdou - v multi-viewport mode okno v separate OS window zůstává
 * pod ostatními ve z-orderu i přes "fokus" v ImGui pohledu.
 */
static bool s_focus_pending = false;


extern "C" void stack_window_request_focus ( void ) {
    s_focus_pending = true;
}


extern "C" void stack_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Per-frame init refresh kontroleru. */
    dbg_disassembled_frame_init ( );

    /* Default size pri prvnim pouziti - dost vysoke aby se vesly vsech
     * 40 radku hex dumpu + header. Sirka ~525 px pojme Addr/Byte/Word/Decode
     * sloupce v hlavni tabulce; side panel "Regions" je samostatne okno
     * (nezavisle na sirce Stack Monitoru). User-modified velikost si ImGui
     * zapamatuje pres ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 525.0f, 834.0f ),
                                ImGuiCond_FirstUseEver );

    /* V9.2: Konzumace pending focus requestu (z cross-link buttonu v
     * jinem okne). SetNextWindowFocus PRED Begin = ImGui v ramci framu
     * okno preview do popredi z-orderu, backend imgui_impl_sdl3 pres
     * Platform_SetWindowFocus volata pri update viewport callbacku
     * provede SDL_RaiseWindow (= OS-level bring-to-front). */
    bool was_pending = s_focus_pending;
    if ( s_focus_pending ) {
        ImGui::SetNextWindowFocus ( );
        s_focus_pending = false;
    };

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;

    /* Stabilni ID pres ###. Title bez arch suffixu - shoda s cpu_window. */
    const char *stack_title = _L ( "Stack Monitor###stack_window" );
    auto_layout_first_use_portrait ( stack_title, 525.0f, 834.0f );
    if ( !ImGui::Begin ( stack_title, p_open, wflags ) ) {
        ImGui::End ( );
        return;
    };

    /* V9.3: Multi-viewport platform raise.
     *
     * SetNextWindowFocus / FocusWindow ImGui core ZMENI pouze interni
     * z-order arrays (g.Windows, g.WindowsFocusOrder) pres
     * BringWindowToFocusFront + BringWindowToDisplayFront. NEVOLA
     * Platform_SetWindowFocus callback - ten je v imgui.cpp zavolan
     * POUZE z NavUpdateWindowingApplyFocus (= Ctrl+Tab navigation).
     *
     * Dusledek: pokud je cilove okno v separate platform viewport
     * (ImGuiConfigFlags_ViewportsEnable ON + okno mimo main window
     * obrys), OS-level z-order zustane beze zmeny - ImGui-internal
     * focus se zmeni (title blikne), ale OS window order ne.
     *
     * Fix: po Begin (= ImGui priradi viewport a vse je consistent)
     * zkontrolujeme, ze okno je v separate viewport, a explicitne
     * zavolame Platform_SetWindowFocus = SDL_RaiseWindow. */
    if ( was_pending ) {
        ImGuiViewport *vp = ImGui::GetWindowViewport ( );
        ImGuiViewport *main_vp = ImGui::GetMainViewport ( );
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO ( );
        if ( vp && vp != main_vp && vp->PlatformWindowCreated
             && pio.Platform_SetWindowFocus ) {
            pio.Platform_SetWindowFocus ( vp );
        };
    };

    /* Veskery vlastni obsah - sticky header + hex dump tabulka. */
    dbg_stack_panel_render ( );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
