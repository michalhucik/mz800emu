/*
 * menu_debugger.cpp — Podmenu "Debugger" / "DbgTool"
 *
 * Sdílená implementace pro dva callsity:
 *  - Hlavní menu emulátoru (caller = DBG_MENU_CALLER_TOPMENU, label
 *    "Debugger"). První položka "MZ-800 Debugger" (Alt+D) toggle-uje
 *    okno debuggeru, oddělena separátorem.
 *  - Menubar hlavního okna debuggeru (caller = DBG_MENU_CALLER_DEBUGGER_WINDOW,
 *    label "DbgTool"). První položka + separator se vynechá - okno
 *    debuggeru je už zobrazené, toggle nedává smysl.
 *
 * Obsahuje položky pro otevření dalších oken debuggeru:
 * - Breakpoints (Alt+B) — okno breakpointů (placeholder)
 * - Memory Map — banking + memext debug okno (per 4 kB stránku)
 * - Memory Browser (Alt+E) — memory browser (placeholder)
 * - Disassembler — disassembler (placeholder)
 *
 * Položky Breakpoints, Memory Browser, Disassembler jsou prozatím
 * disabled — budou implementovány v pozdějších fázích.
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
#include "emulator/mzarch/mzhal.h"
#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "mzarch/mzarch_config.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "debugger/debugger.h"
#include "ui-imgui/debugger/heatmap/mhmap_window.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

extern "C"
{
    void imgui_menu_debugger(en_DBG_MENU_CALLER caller);
}

void imgui_menu_debugger(en_DBG_MENU_CALLER caller)
{
    /* Label menu se liší podle volajícího (= odlišuje topmenu od debugger
     * window menubaru aby user věděl kde je). */
    const char *menu_label = (caller == DBG_MENU_CALLER_DEBUGGER_WINDOW)
                              ? _L("DbgTool")
                              : _L("Debugger");
    if (ImGui::BeginMenu(menu_label))
    {
        /*
         * Hlavní okno debuggeru — toggle přes debugger_show_hide_main_window().
         * Zkratka Alt+D je obsloužena v global_shortcuts.cpp.
         *
         * Volání z menubaru hlavního okna debuggeru tuto položku vynechá -
         * okno je už zobrazené, toggle nedává smysl.
         */
        if (caller != DBG_MENU_CALLER_DEBUGGER_WINDOW)
        {
            if (ImGui::MenuItem(_L("MZ-800 Debugger"), "Alt+D", g_gui->showDebuggerWindow))
            {
                debugger_show_hide_main_window_request();
            };
        };

        /*
         * DBG Workplace - perzistované preference, která auxiliary debug
         * okna se otevírají/zavírají automaticky společně s hlavním oknem
         * debuggeru. Pro caller=TOPMENU je toto druhá položka (= za
         * MZ-800 Debugger toggle), pro caller=DEBUGGER_WINDOW je první
         * (= MZ-800 Debugger toggle se vynechává). Separator za submenu
         * je společný divider pro obě variants.
         */
        if (ImGui::BeginMenu(_L("DBG Workplace")))
        {
            bool wp_cpu = (g_debugger.wp_cpu_registers != 0);
            if (ImGui::MenuItem(_L("CPU Registers"), NULL, wp_cpu))
            {
                g_debugger.wp_cpu_registers = wp_cpu ? 0 : 1;
            };
            bool wp_mm = (g_debugger.wp_memory_map != 0);
            if (ImGui::MenuItem(_L("Memory Map"), NULL, wp_mm))
            {
                g_debugger.wp_memory_map = wp_mm ? 0 : 1;
            };
            bool wp_sm = (g_debugger.wp_stack_monitor != 0);
            if (ImGui::MenuItem(_L("Stack Monitor"), NULL, wp_sm))
            {
                g_debugger.wp_stack_monitor = wp_sm ? 0 : 1;
            };
            bool wp_cs = (g_debugger.wp_callstack != 0);
            if (ImGui::MenuItem(_L("Callstack (experimental)"), NULL, wp_cs))
            {
                g_debugger.wp_callstack = wp_cs ? 0 : 1;
            };
            bool wp_bp = (g_debugger.wp_breakpoints != 0);
            if (ImGui::MenuItem(_L("Breakpoints"), NULL, wp_bp))
            {
                g_debugger.wp_breakpoints = wp_bp ? 0 : 1;
            };
            bool wp_w = (g_debugger.wp_watch != 0);
            if (ImGui::MenuItem(_L("Watch"), NULL, wp_w))
            {
                g_debugger.wp_watch = wp_w ? 0 : 1;
            };
            /* membrowser mutant V0: Memory Browser workplace slot. */
            bool wp_mb = (g_debugger.wp_membrowser != 0);
            if (ImGui::MenuItem(_L("Memory Browser"), NULL, wp_mb))
            {
                g_debugger.wp_membrowser = wp_mb ? 0 : 1;
            };
            /* V3 multi-view: workplace sloty pro sekundarni Memory Browser
             * okna #2..#5. Kazde tlacitko per-instance unikatni label
             * (ImGui ID derived from label). */
            bool wp_mb2 = (g_debugger.wp_membrowser_extra[0] != 0);
            if (ImGui::MenuItem(_L("Memory Browser #2"), NULL, wp_mb2))
            {
                g_debugger.wp_membrowser_extra[0] = wp_mb2 ? 0 : 1;
            };
            bool wp_mb3 = (g_debugger.wp_membrowser_extra[1] != 0);
            if (ImGui::MenuItem(_L("Memory Browser #3"), NULL, wp_mb3))
            {
                g_debugger.wp_membrowser_extra[1] = wp_mb3 ? 0 : 1;
            };
            bool wp_mb4 = (g_debugger.wp_membrowser_extra[2] != 0);
            if (ImGui::MenuItem(_L("Memory Browser #4"), NULL, wp_mb4))
            {
                g_debugger.wp_membrowser_extra[2] = wp_mb4 ? 0 : 1;
            };
            bool wp_mb5 = (g_debugger.wp_membrowser_extra[3] != 0);
            if (ImGui::MenuItem(_L("Memory Browser #5"), NULL, wp_mb5))
            {
                g_debugger.wp_membrowser_extra[3] = wp_mb5 ? 0 : 1;
            };
            bool wp_p = (g_debugger.wp_profiler != 0);
            if (ImGui::MenuItem(_L("CPU Profiler"), NULL, wp_p))
            {
                g_debugger.wp_profiler = wp_p ? 0 : 1;
            };
            bool wp_bm = (g_debugger.wp_bookmarks != 0);
            if (ImGui::MenuItem(_L("Bookmarks"), NULL, wp_bm))
            {
                g_debugger.wp_bookmarks = wp_bm ? 0 : 1;
            };
            bool wp_sy = (g_debugger.wp_symbols != 0);
            if (ImGui::MenuItem(_L("Symbols"), NULL, wp_sy))
            {
                g_debugger.wp_symbols = wp_sy ? 0 : 1;
            };
            bool wp_va = (g_debugger.wp_variables != 0);
            if (ImGui::MenuItem(_L("Variables"), NULL, wp_va))
            {
                g_debugger.wp_variables = wp_va ? 0 : 1;
            };

            ImGui::Separator();

            /* Disassembly #2..#5 - každé na vlastním řádku jako samostatný
             * checkbox. Label musí být per-instance unikátní pro ImGui ID,
             * proto použijeme různé string konstanty. */
            bool wp_d2 = (g_debugger.wp_disasm_extra[0] != 0);
            if (ImGui::MenuItem(_L("Disassembly #2"), NULL, wp_d2))
            {
                g_debugger.wp_disasm_extra[0] = wp_d2 ? 0 : 1;
            };
            bool wp_d3 = (g_debugger.wp_disasm_extra[1] != 0);
            if (ImGui::MenuItem(_L("Disassembly #3"), NULL, wp_d3))
            {
                g_debugger.wp_disasm_extra[1] = wp_d3 ? 0 : 1;
            };
            bool wp_d4 = (g_debugger.wp_disasm_extra[2] != 0);
            if (ImGui::MenuItem(_L("Disassembly #4"), NULL, wp_d4))
            {
                g_debugger.wp_disasm_extra[2] = wp_d4 ? 0 : 1;
            };
            bool wp_d5 = (g_debugger.wp_disasm_extra[3] != 0);
            if (ImGui::MenuItem(_L("Disassembly #5"), NULL, wp_d5))
            {
                g_debugger.wp_disasm_extra[3] = wp_d5 ? 0 : 1;
            };

            ImGui::Separator();

            /* Per-chip-panels F1 scaffold: workplace toggles pro per-chip
             * detail okna. Opt-in, default 0. */
            bool wp_ctc = (g_debugger.wp_show_ctc != 0);
            if (ImGui::MenuItem(_L("CTC State"), NULL, wp_ctc))
            {
                g_debugger.wp_show_ctc = wp_ctc ? 0 : 1;
            };
            bool wp_ppi = (g_debugger.wp_show_ppi != 0);
            if (ImGui::MenuItem(_L("PPI State"), NULL, wp_ppi))
            {
                g_debugger.wp_show_ppi = wp_ppi ? 0 : 1;
            };
        if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
            /* Z80 PIO je jen na MZ-800 / MZ-1500. MZ-700 (HAVE_PIOZ80=0)
             * čip nemá → workplace slot vypnut. */
            bool wp_pioz = (g_debugger.wp_show_pioz80 != 0);
            if (ImGui::MenuItem(_L("Z80 PIO State"), NULL, wp_pioz))
            {
                g_debugger.wp_show_pioz80 = wp_pioz ? 0 : 1;
            };
        }
        if (g_mzhal.psg_count >= 1) { /* runtime capability, mzhal krok 8 */
            /* PSG SN76489 je jen na MZ-800 (mono) / MZ-1500 (stereo).
             * MZ-700 (HAVE_PSG=0) čip nemá → workplace slot vypnut. */
            bool wp_psg = (g_debugger.wp_show_psg != 0);
            if (ImGui::MenuItem(_L("PSG State"), NULL, wp_psg))
            {
                g_debugger.wp_show_psg = wp_psg ? 0 : 1;
            };
            /* psg-audio-scope mutant F1: workplace slot pro PSG Audio
             * Scope (samostatný panel od PSG State, opt-in default 0). */
            bool wp_psg_audio_scope = (g_debugger.wp_show_psg_audio_scope != 0);
            if (ImGui::MenuItem(_L("PSG Audio Scope"), NULL, wp_psg_audio_scope))
            {
                g_debugger.wp_show_psg_audio_scope = wp_psg_audio_scope ? 0 : 1;
            };
        }
            /* gdg-panel F1 scaffold: GDG je ve všech 3 archech, žádný guard. */
            bool wp_gdg = (g_debugger.wp_show_gdg != 0);
            if (ImGui::MenuItem(_L("GDG State"), NULL, wp_gdg))
            {
                g_debugger.wp_show_gdg = wp_gdg ? 0 : 1;
            };

            ImGui::EndMenu();
        };

        ImGui::Separator();

        /* ===== CPU & execution ===== */

        /*
         * CPU Registers (Variant B) - samostatné plovoucí okno s registr fileom,
         * flagy a special sekcí. Toggle přes g_gui->showCpuWindow.
         * Zkratka Alt+Shift+R obsloužena v global_shortcuts.cpp (Alt+R bez
         * Shift kolidoval s NVIDIA Overlay).
         */
        if (ImGui::MenuItem(_L("CPU Registers"), "Alt+Shift+R", g_gui->showCpuWindow))
        {
            g_gui->showCpuWindow = !g_gui->showCpuWindow;
        };
        /*
         * Stack Monitor - samostatné plovoucí okno s hex dumpem paměti
         * kolem aktuálního SP + tlačítkem pro rychlý SP_THRESHOLD BPT
         * (Set BP from SP-256). Toggle přes g_gui->showStackWindow.
         * Zkratka Alt+S obsloužena v global_shortcuts.cpp.
         */
        if (ImGui::MenuItem(_L("Stack Monitor"), "Alt+S", g_gui->showStackWindow))
        {
            g_gui->showStackWindow = !g_gui->showStackWindow;
        };
        /*
         * V8: Stack Regions - samostatné okno s tabulkou stack regionů
         * (1 řádek per region: Name / Base / Limit / SP% / Min / Push /
         * Pop / Trend / [E][R][X] akce). Otevírá se odděleně od Stack
         * Monitor okna (Alt+S), zkratka Alt+Shift+S.
         */
        if (ImGui::MenuItem(_L("Stack Regions"), "Alt+Shift+S",
                             g_gui->showStackRegionsWindow))
        {
            g_gui->showStackRegionsWindow = !g_gui->showStackRegionsWindow;
        };
        /*
         * V9: Stack History - samostatné okno se SP history sparkline.
         * Plot resize s velikostí okna (X i Y). Otevírá se odděleně od
         * Stack Monitor okna (Alt+S), zkratka Alt+Shift+H.
         */
        if (ImGui::MenuItem(_L("Stack History"), "Alt+Shift+H",
                             g_gui->showStackHistoryWindow))
        {
            g_gui->showStackHistoryWindow = !g_gui->showStackHistoryWindow;
        };
        /*
         * Callstack (mutant callstack, Fáze 2C) - samostatné plovoucí okno
         * se shadow stackem volání (CALL/RST/IRQ/NMI frames + divergence
         * counters). Toggle přes g_gui->showCallstackWindow. Aktivace
         * subsystému (g_callstack_active) přes Active checkbox v okně
         * nebo CLI --callstack / INI [CALLSTACK] active = 1.
         */
        if (ImGui::MenuItem(_L("Callstack (experimental)"), NULL, g_gui->showCallstackWindow))
        {
            g_gui->showCallstackWindow = !g_gui->showCallstackWindow;
        };
        /*
         * CPU Profiler (mutant profiler V1) - samostatné plovoucí okno
         * s per-function profilací (Excl/Incl cycles, Calls, Min/Max).
         * Toggle přes g_gui->showProfilerWindow. Aktivace subsystému
         * (g_profiler_active) přes Active checkbox v okně nebo CLI
         * --profiler / INI [PROFILER] active = 1. Zkratka Alt+Shift+P
         * obsloužena v global_shortcuts.cpp (Alt+P = pause toggle, takže
         * profiler je v combo variantě).
         */
        if (ImGui::MenuItem(_L("CPU Profiler"), "Alt+Shift+P", g_gui->showProfilerWindow))
        {
            g_gui->showProfilerWindow = !g_gui->showProfilerWindow;
        };

        ImGui::Separator();

        /* ===== Control flow ===== */

        /*
         * Okno breakpointů — toggle přes g_gui->showBreakpointsWindow.
         * Zkratka Alt+B je obsloužena v global_shortcuts.cpp.
         */
        if (ImGui::MenuItem(_L("Breakpoints"), "Alt+B", g_gui->showBreakpointsWindow))
        {
            g_gui->showBreakpointsWindow = !g_gui->showBreakpointsWindow;
        };
        /*
         * Variables panel - live $vars storage smart BP (D.6.2).
         * Zkratka Alt+V obsloužena v global_shortcuts.cpp.
         */
        if (ImGui::MenuItem(_L("Variables"), "Alt+V", g_gui->showVarsWindow))
        {
            g_gui->showVarsWindow = !g_gui->showVarsWindow;
        };
        /*
         * Watch panel - user-defined paměťové hlídky (V1 Phase C).
         * Zkratka Alt+Shift+W obsloužena v global_shortcuts.cpp.
         * (Alt+W bez Shift = fix aspect ratio by Width; combo varianta
         * je analogická Alt+Shift+R/S/H pro debugger okna.)
         */
        if (ImGui::MenuItem(_L("Watch"), "Alt+Shift+W", g_gui->showWatchWindow))
        {
            g_gui->showWatchWindow = !g_gui->showWatchWindow;
        };

        ImGui::Separator();

        /* ===== Memory ===== */

        /*
         * Memory Map - banking + memext debug okno (per 4 kB stránku).
         * Toggle přes g_gui->showMemoryMapWindow. Bez globální zkratky
         * (Alt+Shift+M je obsazený pro switch CUSTOM speed).
         */
        if (ImGui::MenuItem(_L("Memory Map"), NULL, g_gui->showMemoryMapWindow))
        {
            g_gui->showMemoryMapWindow = !g_gui->showMemoryMapWindow;
        };
        /*
         * Memory Browser (V0 hex MVP - membrowser mutant) - hex view paměti
         * přes dbgapi_regions plnou paletu regionů + 8 encodings.
         * Toggle přes g_gui->showMemoryBrowserWindow + globální zkratka Alt+E.
         */
        if (ImGui::MenuItem(_L("Memory Browser"), "Alt+E",
                            g_gui->showMemoryBrowserWindow))
        {
            g_gui->showMemoryBrowserWindow = !g_gui->showMemoryBrowserWindow;
        };
        /*
         * V3 multi-view: submenu "Other memory browsers" - 4 sekundarni
         * Memory Browser okna (#2 - #5). Kazde je nezavisla instance s
         * vlastnim regionem/encodingem/cursorem; sdileji pouze freeze
         * subsystem. Toggle pres g_gui->showMemoryBrowserWindowExtra[N-2].
         */
        if (ImGui::BeginMenu(_L("Other memory browsers")))
        {
            if (ImGui::MenuItem(_L("Memory Browser #2"), NULL,
                                g_gui->showMemoryBrowserWindowExtra[0]))
            {
                g_gui->showMemoryBrowserWindowExtra[0] = !g_gui->showMemoryBrowserWindowExtra[0];
            };
            if (ImGui::MenuItem(_L("Memory Browser #3"), NULL,
                                g_gui->showMemoryBrowserWindowExtra[1]))
            {
                g_gui->showMemoryBrowserWindowExtra[1] = !g_gui->showMemoryBrowserWindowExtra[1];
            };
            if (ImGui::MenuItem(_L("Memory Browser #4"), NULL,
                                g_gui->showMemoryBrowserWindowExtra[2]))
            {
                g_gui->showMemoryBrowserWindowExtra[2] = !g_gui->showMemoryBrowserWindowExtra[2];
            };
            if (ImGui::MenuItem(_L("Memory Browser #5"), NULL,
                                g_gui->showMemoryBrowserWindowExtra[3]))
            {
                g_gui->showMemoryBrowserWindowExtra[3] = !g_gui->showMemoryBrowserWindowExtra[3];
            };
            ImGui::EndMenu();
        };
        /*
         * V5: Memory Diff - side-by-side hex porovnani dvou snapshotu
         * (live / manual / auto @ pause). Singleton, dostupne z menu
         * vedle Memory Browser; nezavisle na hlavnim okne membrowser.
         */
        if (ImGui::MenuItem(_L("Memory Diff"), NULL,
                            g_gui->showMemoryDiffWindow))
        {
            g_gui->showMemoryDiffWindow = !g_gui->showMemoryDiffWindow;
        };
/* SENTINEL: Memory Heatmap platí pro všechny známé architektury
 * (garantováno mzhal.c) - při přidání nové architektury PROVĚŘ. */
        /*
         * Memory Heatmap (CDL) - samostatné okno pro vizualizaci access counterů.
         * Toggle přes g_gui->showMemoryHeatmapWindow.
         */
        if (ImGui::MenuItem(_L("Memory Heatmap"), NULL, g_gui->showMemoryHeatmapWindow))
        {
            mhmap_window_show_hide();
        };

        /*
         * V1.5+ ASCII edit follow-up: Char Inserter - paleta znaků pro
         * vkládání speciálních chars (SharpMZ EU/JP, KOI8-CS) do Memory
         * Browseru. Otevírá se primárně z context menu hex view; tato
         * menu položka je sekundární přístup.
         */
        if (ImGui::MenuItem(_L("Char Inserter"), NULL,
                            g_gui->showMembrowserCharInserter))
        {
            g_gui->showMembrowserCharInserter = !g_gui->showMembrowserCharInserter;
        };

        ImGui::Separator();

        /* ===== Code & symbols ===== */

        /*
         * Disassembler (V1): samostatné range-based disassembler okno
         * s auto-labely (S/L/D/W konvence) + sym_db/CDL gating + export
         * do .asm/.s (pasmo / sjasmplus / sdcc-asz80 dialect).
         * Zkratka Alt+Shift+D obsloužena v global_shortcuts.cpp
         * (kombinace s Alt+D = hlavní debugger okno, vzor Alt+B /
         * Alt+Shift+B, Alt+R / Alt+Shift+R apod.).
         */
        if (ImGui::MenuItem(_L("Disassembler"), "Alt+Shift+D",
                            g_gui->showDisassemblerWindow))
        {
            g_gui->showDisassemblerWindow = !g_gui->showDisassemblerWindow;
        };

        /*
         * Submenu "Other disassembly" - 4 sekundární Disassembly okna
         * (#2 - #5). Každé je nezávislá instance DisassembledView bez
         * horní historie a bez animace na PC. Toggle přes
         * g_gui->showDisasmExtraWindow[N]. Render v main_window.cpp.
         */
        if (ImGui::BeginMenu(_L("Other disassembly")))
        {
            if (ImGui::MenuItem(_L("Disassembly #2"), NULL,
                                g_gui->showDisasmExtraWindow[0]))
            {
                g_gui->showDisasmExtraWindow[0] = !g_gui->showDisasmExtraWindow[0];
            };
            if (ImGui::MenuItem(_L("Disassembly #3"), NULL,
                                g_gui->showDisasmExtraWindow[1]))
            {
                g_gui->showDisasmExtraWindow[1] = !g_gui->showDisasmExtraWindow[1];
            };
            if (ImGui::MenuItem(_L("Disassembly #4"), NULL,
                                g_gui->showDisasmExtraWindow[2]))
            {
                g_gui->showDisasmExtraWindow[2] = !g_gui->showDisasmExtraWindow[2];
            };
            if (ImGui::MenuItem(_L("Disassembly #5"), NULL,
                                g_gui->showDisasmExtraWindow[3]))
            {
                g_gui->showDisasmExtraWindow[3] = !g_gui->showDisasmExtraWindow[3];
            };
            ImGui::EndMenu();
        };
        /*
         * Symbol Browser (D.8) - load NoICE / sdldz80 .map / sjasmplus .sym
         * + user write-back .lbl. Disassembler ukazuje jmena misto hex.
         * Zkratka Alt+Y obsloužena v global_shortcuts.cpp.
         */
        if (ImGui::MenuItem(_L("Symbols"), "Alt+Y", g_gui->showSymbolsWindow))
        {
            g_gui->showSymbolsWindow = !g_gui->showSymbolsWindow;
        };
        /*
         * Bookmarks panel - pojmenované adresové záložky (uživatelské).
         * Klik / RMB v Bookmarks okně otevírá Disassembly na danou adresu.
         * Zkratka Alt+Shift+B obsloužena v global_shortcuts.cpp (Alt+B
         * bez Shift = Breakpoints).
         */
        if (ImGui::MenuItem(_L("Bookmarks"), "Alt+Shift+B", g_gui->showBookmarksWindow))
        {
            g_gui->showBookmarksWindow = !g_gui->showBookmarksWindow;
        };

        ImGui::Separator();

        /* ===== I/O & events ===== */

        /*
         * I/O Ports viewer (V1.5.D rework) - bit-by-bit struct view + History
         * + activity tracking. Zkratka Alt+I obsloužena v global_shortcuts.cpp.
         */
        if (ImGui::MenuItem(_L("I/O Ports"), "Alt+I", g_gui->showIoWindow))
        {
            g_gui->showIoWindow = !g_gui->showIoWindow;
        };
        /*
         * Events - real-time pohled na eventlog ring (Vlna 1 = Log tab,
         * Vlna 2 = Strip tab). V módu WHEN_WINDOW_OPEN otevírá ring
         * recording, zavírá ho zase při zavření okna.
         * Toggle přes g_gui->showEventViewerWindow. Bez globální zkratky.
         */
        if (ImGui::MenuItem(_L("Events"), NULL, g_gui->showEventViewerWindow))
        {
            g_gui->showEventViewerWindow = !g_gui->showEventViewerWindow;
        };
        /*
         * MCP Activity okno bylo přesunuto z tohoto menu do Tools ->
         * pod "MCP TCP Server" (viz menu_tools.cpp). Gating je
         * MZ800EMU_CFG_MCP_SERVER_ENABLED (okno funguje i bez TCP).
         */

        ImGui::Separator();

        /*
         * Submenu "Chips" - per-chip detail (state) okna integrovanych LSI:
         * CTC 8253, PPI 8255, Z80 PIO, PSG SN76489, GDG video LSI. CTC a PPI
         * jsou ve vsech 3 archech, Z80 PIO a PSG jen v MZ-800 / MZ-1500, GDG
         * vsude. Shortcuts Alt+Shift+C / +I / +Z / +G / +A / +V obsluhuje
         * global_shortcuts.cpp (funguji i kdyz je okno v submenu).
         */
        if (ImGui::BeginMenu(_L("Chips")))
        {
            if (ImGui::MenuItem(_L("CTC State"), "Alt+Shift+C", g_gui->showCtcStateWindow))
            {
                g_gui->showCtcStateWindow = !g_gui->showCtcStateWindow;
            };
            if (ImGui::MenuItem(_L("PPI State"), "Alt+Shift+I", g_gui->showPpiStateWindow))
            {
                g_gui->showPpiStateWindow = !g_gui->showPpiStateWindow;
            };
        if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("Z80 PIO State"), "Alt+Shift+Z", g_gui->showPiozStateWindow))
            {
                g_gui->showPiozStateWindow = !g_gui->showPiozStateWindow;
            };
        }
        if (g_mzhal.psg_count >= 1) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("PSG State"), "Alt+Shift+G", g_gui->showPsgStateWindow))
            {
                g_gui->showPsgStateWindow = !g_gui->showPsgStateWindow;
            };
            /* psg-audio-scope mutant F1: PSG Audio Scope - samostatné okno
             * pro dynamickou audio analýzu (oscilloscope + plánované
             * envelope / piano roll). Shortcut Alt+Shift+A = Audio. */
            if (ImGui::MenuItem(_L("PSG Audio Scope"), "Alt+Shift+A", g_gui->showPsgAudioScopeWindow))
            {
                g_gui->showPsgAudioScopeWindow = !g_gui->showPsgAudioScopeWindow;
            };
        }
            /* gdg-panel F1 scaffold: GDG inspector toggle. V = Video / GDG.
             * GDG je ve všech 3 archech (MZ-700/MZ-800/MZ-1500), žádný guard. */
            if (ImGui::MenuItem(_L("GDG State"), "Alt+Shift+V", g_gui->showGdgStateWindow))
            {
                g_gui->showGdgStateWindow = !g_gui->showGdgStateWindow;
            };

            ImGui::EndMenu();
        };

        /*
         * Submenu "Storage" - state okna ulozistovych radicu/zarizeni:
         * FDC (WD279x), QDisk (MZ-1F11), Memory Disk (ramdisky MR-1R18 +
         * Pezik), IDE8 (HDD radic). Tyto MenuItems jsou duplicitni k polozkam
         * v sekci HW (menu_fdcontroller.cpp, menu_qdisk.cpp, menu_ramdisk.cpp,
         * menu_ide8.cpp); zamer je mit je dostupne i z Debugger menu vedle
         * ostatnich state oken. Toggle stejnych g_gui flagu, takze obe cesty
         * udrzuji stejny stav.
         */
        if (ImGui::BeginMenu(_L("Storage")))
        {
        if (g_mzhal.have_fdc) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("FDC State"), NULL, g_gui->showFdcStateWindow))
            {
                g_gui->showFdcStateWindow = !g_gui->showFdcStateWindow;
            };
        }
        if (g_mzhal.have_qdisk) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("QDisk State"), NULL, g_gui->showQdiskStateWindow))
            {
                g_gui->showQdiskStateWindow = !g_gui->showQdiskStateWindow;
            };
        }
        if (g_mzhal.have_ramdisk) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("Memory Disk State"), NULL, g_gui->showRamdiskStateWindow))
            {
                g_gui->showRamdiskStateWindow = !g_gui->showRamdiskStateWindow;
            };
        }
        if (g_mzhal.have_ide8) { /* runtime capability, mzhal krok 8 */
            if (ImGui::MenuItem(_L("IDE8 State"), NULL, g_gui->showIde8StateWindow))
            {
                g_gui->showIde8StateWindow = !g_gui->showIde8StateWindow;
            };
        }

            ImGui::EndMenu();
        };

        ImGui::EndMenu();
    };
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
