/*
 * profiler_window.cpp - CPU Profiler okno wrapper (mutant profiler V1, fáze F3).
 *
 * Tenký wrapper kolem dbg_profiler_render() ze sections/dbg_profiler.cpp.
 * Vytváří top-level ImGui okno s vlastním title a persistentní geometrií
 * (ImGui ini), uvnitř volá sdílený render core.
 *
 * Stabilní ID: "CPU Profiler###profiler_window" (= prefix lze později
 * obohatit o arch / mode bez invalidace pozice/velikosti v ini).
 *
 * Default size: 720x520 px (= toolbar + 10 sloupců tabulka + status bar).
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

#include "profiler_window.h"
#include "ui-imgui/debugger/sections/dbg_profiler.h"
#include "ui-imgui/auto_layout.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "emulator/cfgmain.h"


/* ===========================================================================
 *  Cfg persistence - klíč show_window v sekci [PROFILER] (Fáze F5)
 * =========================================================================== */

/**
 * @brief File-static cache pro persistenci show_window mezi INI a g_gui.
 *
 * cfgmain_register_new_element drží pointer do této proměnné, takže
 * propagate (= načtení z INI) i save (= zápis do INI) probíhají bez
 * potřeby existujícího g_gui. Render loop synchronizuje cache <->
 * g_gui->showProfilerWindow per-frame (cheap uint write).
 *
 * Default 0 = okno startuje zavřené po čerstvé instalaci.
 */
static unsigned s_persisted_show_window = 0u;


extern "C" void profiler_window_register_cfg ( void *cmod_void )
{
    if ( !cmod_void ) return;

    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;

    /* show_window - default 0 (= okno zavřené po čerstvé instalaci, user
     * explicit otevře přes Debug menu / Alt+Shift+P). */
    st_CFGELEMENT *elm = cfgmodule_register_new_element ( cmod,
                                                          (char *) "show_window",
                                                          CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm,
                              (void *) &s_persisted_show_window,
                              (void *) &s_persisted_show_window );
}


extern "C" void profiler_window_apply_persisted ( void )
{
    if ( !g_gui ) return;
    g_gui->showProfilerWindow = ( s_persisted_show_window != 0u );
}


extern "C" void profiler_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Default size při prvním použití - 10 sloupců (Function/Addr/Kind/
     * Calls/Excl%/Excl/Incl/Avg/Min/Max) + toolbar + status bar.
     * Šířka 1280 nutná aby všech 10 sloupců bylo viditelných bez
     * horizontálního scrollu (= Function + 9 numerických sloupců s
     * uint64 hodnotami). User-modified velikost si ImGui zapamatuje
     * přes ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 1280.0f, 560.0f ),
                                ImGuiCond_FirstUseEver );

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;

    /* Stabilní ID přes ###. */
    const char *title = _L ( "CPU Profiler###profiler_window" );
    auto_layout_first_use_portrait ( title, 1280.0f, 560.0f );
    if ( !ImGui::Begin ( title, p_open, wflags ) ) {
        ImGui::End ( );
        /* Sync cache i v case sbalit/min, aby shutdown ulozil aktualni stav.
         * *p_open mohlo byt zmeneno krizem v titulbaru ImGui pred Beginem. */
        s_persisted_show_window = *p_open ? 1u : 0u;
        return;
    };

    /* Veškerý vlastní obsah - toolbar + tabulka + status bar. */
    dbg_profiler_render ( );

    ImGui::End ( );

    /* Sync runtime stavu zpět do persistence cache (shutdown save callback
     * cte z teto promenne). p_open == &g_gui->showProfilerWindow, takze
     * zachyti i zavreni oknem krizem v titulbaru behem tohoto framu. */
    s_persisted_show_window = *p_open ? 1u : 0u;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
