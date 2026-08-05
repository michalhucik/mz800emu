/*
 * cpu_window.cpp - CPU Registers v samostatném plovoucím okně (Variant B).
 *
 * Tenký wrapper kolem dbg_cpu_panel_render() ze sections/dbg_cpu_panel.cpp.
 * Vytváří top-level ImGui okno s vlastním title a persistentní geometrií
 * (ImGui ini), uvnitř volá sdílený render core.
 *
 * Layout obsahu: viz dbg_cpu_panel.cpp - registr file, flagy, special
 * sekce, focus tlačítka. Tady je pouze obálka okna.
 *
 * Stabilní ID: "CPU Registers###cpu_window" (= prefix lze později
 * obohatit o arch / mode bez invalidace pozice/velikosti v ini).
 *
 * Default size: ImGui auto-fit při prvním použití (nemá smysl ručně
 * předepisovat - panel se sám smrští na potřebnou výšku registr fileu).
 * Po prvním použití si ImGui zapamatuje user-modified velikost přes ini.
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

#include "cpu_window.h"
#include "ui-imgui/debugger/sections/dbg_cpu_panel.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/auto_layout.h"


/**
 * @brief Render top-level CPU Registers okna.
 *
 * Otevře ImGui::Begin se stabilním ID a delegovuje obsah na
 * dbg_cpu_panel_render(). Stejný pattern jako bm_window_render.
 *
 * Před renderem volá dbg_disassembled_frame_init() (= per-frame guarded
 * init g_dbg_ui + dbg_refresh_tick). Bez tohoto by CPU okno otevřené
 * samostatně (= bez hlavního debug okna a bez sekundárních disasm oken)
 * nikdy nedostalo should_refresh = true a panel by ukazoval zamrzlou
 * cache. Funkce je idempotentní v rámci jednoho framu, takže současné
 * otevření CPU okna i hlavního debuggeru nezpůsobí dvojitý refresh.
 *
 * @param p_open Pointer na bool s viditelností. NULL nebo false = no-op.
 */
extern "C" void cpu_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Per-frame init refresh kontroleru. Volání idempotentní v rámci
     * jednoho framu (frame guard přes ImGui::GetFrameCount uvnitř funkce). */
    dbg_disassembled_frame_init ( );

    /*
     * Auto-resize okna podle obsahu (V3.3, Michal 2026-05-11). User
     * nemůže manuálně resize, ale to je pro CPU panel acceptable -
     * obsah je fixní (tabulka + flagy + cyc), velikost by stejně
     * "blikala" mezi malou a velkou (Cycles & raster collapsed/expanded).
     * Persistence pozice přes ImGui ini funguje normálně.
     */
    ImGuiWindowFlags wflags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

    /*
     * Stabilní ID přes ###. Title bez arch suffixu - Variant B nemá
     * arch v titulku, jen sdílený "CPU Registers" textový label.
     *
     * Auto-layout při fresh open (= bez záznamu v imgui.ini): hledá volné
     * místo v primárním viewportu mimo ostatní debug okna.
     */
    const char *cpu_title = _L ( "CPU Registers###cpu_window" );
    auto_layout_first_use_portrait ( cpu_title, 320.0f, 480.0f );
    if ( !ImGui::Begin ( cpu_title, p_open, wflags ) ) {
        ImGui::End ( );
        return;
    };

    /* Veškerý vlastní obsah - registry, flagy, special sekce. */
    dbg_cpu_panel_render ( );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
