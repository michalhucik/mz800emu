/*
 * debugger_window.cpp — Hlavní okno debuggeru (ImGui)
 *
 * PŘEHLED
 * =======
 * Hlavní okno debuggeru MZ-800/MZ-1500. Obsahuje:
 * - Top menu (File, Emulation, Screen, Debugger Settings)
 * - Středovou část s moduly (Disassembled, registry, stack, ...)
 * - Iconbar (toolbar s tlačítky pro řízení emulace)
 *
 * LAYOUT OKNA
 * ===========
 *
 *  ┌──────────────────────────────────────────────────┐
 *  │ File  Emulation  Screen  Debugger settings       │ ← menubar
 *  ├───────────────────┬──────────────────────────────┤
 *  │                   │                              │
 *  │   Disassembled    │   (budoucí sekce:            │
 *  │   (historie +     │    CPU Registers, Stack,     │
 *  │    disassembly +  │    Memory Map, GDG, CTC,     │
 *  │    slider)        │    Internals, ...)           │
 *  │                   │                              │
 *  ├───────────────────┴──────────────────────────────┤
 *  │ [Continue][Pause] | [StepOver][StepInto][RunTo]  │ ← iconbar
 *  │                   | [BP] [MEM] [DASM]            │
 *  └──────────────────────────────────────────────────┘
 *
 * DVA REŽIMY DEBUGGERU
 * ====================
 *
 * 1. ANIMAČNÍ REŽIM (emulace běží):
 *    - Debugger periodicky obnovuje svůj stav
 *    - Interaktivní prvky ve středové části jsou disabled
 *    - Pokud uživatel klikne na interaktivní prvek, emulace se automaticky
 *      pozastaví a zobrazí se modální info okno "Emulation was paused..."
 *
 * 2. EDITAČNÍ REŽIM (emulace v pauze):
 *    - Všechny prvky jsou enabled
 *    - Uživatel může editovat registry, disassembly, navigovat atd.
 *    - Při obnovení emulace (F5/Continue) se přejde zpět do animačního režimu
 *
 * SYNCHRONIZACE S EMULÁTOREM
 * ==========================
 * - Stav emulace: EMULATOR_TEST_PAUSED (g_emulator.paused)
 * - Viditelnost debuggeru: g_gui->showDebuggerWindow
 * - Aktivita v jádru: g_debugger.active — synchronizujeme s showDebuggerWindow
 * - UI stav: g_dbg_ui (DebuggerUIState) — focus, selekce, split ratio
 *
 * INICIALIZACE
 * ============
 * Při prvním otevření okna (initialized == false):
 * 1. Zavoláme debugger_ui_state_init() — inicializace UI stavu
 * 2. Zavoláme debugger_ui_state_refresh_from_cpu() — sync s CPU registry
 * 3. Nastavíme g_debugger.active = 1
 *
 * Při zavření okna:
 * 1. Nastavíme g_debugger.active = 0
 *
 * KLÁVESOVÉ ZKRATKY
 * =================
 * - ESC: zavře okno debuggeru
 * - F5: Continue (pokračovat v emulaci)
 * - Ctrl+F5: Pause (pozastavit emulaci)
 * - F7: Step Into
 * - F8: Step Over
 * - F4: Run to Cursor
 * - Ctrl+R: Forced Full Screen Refresh
 * - Všechny globální zkratky (Alt+P, Alt+M, Alt+B, Alt+D, F12, ...)
 * (Zkratky F4/F5/F7/F8 jsou obslouženy v iconbaru)
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

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "emulator/emulator.h"
#include "debugger/debugger.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/message/message_window.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"

#include "debugger_window.h"
#include "debugger_state.h"
#include "menu/dbg_topmenu.h"
#include "ui-imgui/auto_layout.h"
#include "iconbar/dbg_iconbar.h"
#include "sections/dbg_disassembled.h"
#include "sections/dbg_inline_asm.h"
#include "sections/dbg_focus_to.h"


/*
 * DBG Workplace - aplikuje persistované workplace flagy g_debugger.wp_*
 * na g_gui->show* flagy. Volá se ze show/hide hlavního okna debuggeru:
 *  - show: open=true → pro každý wp_* flag = 1 nastavit odpovídající
 *    show* = true.
 *  - hide: open=false → pro každý wp_* flag = 1 nastavit show* = false.
 *
 * Pokud user mezitím okno otevřel/zavřel mimo workplace logiku, příští
 * toggle hlavního okna ho zarovná zpět dle workplace preference.
 */
static void dbg_workplace_apply(bool open)
{
    if (g_debugger.wp_cpu_registers)   g_gui->showCpuWindow         = open;
    if (g_debugger.wp_memory_map)      g_gui->showMemoryMapWindow   = open;
    if (g_debugger.wp_stack_monitor)   g_gui->showStackWindow       = open;
    if (g_debugger.wp_callstack)       g_gui->showCallstackWindow   = open;
    if (g_debugger.wp_breakpoints)     g_gui->showBreakpointsWindow = open;
    if (g_debugger.wp_watch)           g_gui->showWatchWindow       = open;
    if (g_debugger.wp_membrowser)      g_gui->showMemoryBrowserWindow = open;
    /* V3 multi-view: workplace flagy pro sekundarni Memory Browser okna. */
    for (int i = 0; i < 4; i++)
    {
        if (g_debugger.wp_membrowser_extra[i])
            g_gui->showMemoryBrowserWindowExtra[i] = open;
    };
    if (g_debugger.wp_profiler)        g_gui->showProfilerWindow    = open;
    if (g_debugger.wp_bookmarks)       g_gui->showBookmarksWindow   = open;
    if (g_debugger.wp_symbols)         g_gui->showSymbolsWindow     = open;
    if (g_debugger.wp_variables)       g_gui->showVarsWindow        = open;
    /* Per-chip-panels: per-chip detail okna do workplace. CTC + PPI ve
     * všech archech, Z80 PIO + PSG jen MZ-800 / MZ-1500. */
    if (g_debugger.wp_show_ctc)        g_gui->showCtcStateWindow    = open;
    if (g_debugger.wp_show_ppi)        g_gui->showPpiStateWindow    = open;
    if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
    if (g_debugger.wp_show_pioz80)     g_gui->showPiozStateWindow   = open;
    }
    if (g_mzhal.psg_count >= 1) { /* runtime capability, mzhal krok 8 */
    if (g_debugger.wp_show_psg)            g_gui->showPsgStateWindow         = open;
    if (g_debugger.wp_show_psg_audio_scope) g_gui->showPsgAudioScopeWindow   = open;
    }
    /* gdg-panel F1 scaffold: GDG je ve všech 3 archech, žádný guard. */
    if (g_debugger.wp_show_gdg)        g_gui->showGdgStateWindow    = open;
    for (int i = 0; i < 4; i++)
    {
        if (g_debugger.wp_disasm_extra[i])
            g_gui->showDisasmExtraWindow[i] = open;
    };
}


/*
 * ui_debugger_show_main_window — nastaví viditelnost okna debuggeru na true.
 * Volá se z jádra debuggeru (debugger_show_main_window() v debugger.c).
 */
void ui_debugger_show_main_window(void)
{
    g_gui->showDebuggerWindow = true;
    dbg_workplace_apply(true);
}

/*
 * ui_debugger_hide_main_window — nastaví viditelnost okna debuggeru na false.
 * Volá se z jádra debuggeru (debugger_hide_main_window() v debugger.c).
 */
void ui_debugger_hide_main_window(void)
{
    g_gui->showDebuggerWindow = false;
    dbg_workplace_apply(false);
}


/*
 * Sledování přechodu z animačního do editačního režimu.
 * Potřebujeme detekovat okamžik, kdy se emulace zastavila,
 * abychom provedli refresh UI stavu z CPU registrů.
 */
static bool s_was_paused = false;


/**
 * @brief V9.3 pattern: pending flag pro bring-to-front request hlavního
 *        debug okna.
 *
 * Nastaven přes `debugger_window_request_focus()` (typicky z
 * `dbg_disasm_show_in_slot(0, ..)` při kliku na decode adresu ve stack
 * monitoru / bookmarks / CPU panelu). Konzumován v `imgui_debugger_window`
 * dual-step patternem - viz V9.3 analýza v stack_window.cpp.
 */
static bool s_focus_pending = false;


extern "C" void debugger_window_request_focus(void)
{
    s_focus_pending = true;
}


/*
 * render_pause_info_modal — modální okno "Emulation was paused..."
 *
 * Zobrazí se pouze pokud pauza byla vyvolána kliknutím na interaktivní
 * prvek v debuggeru (ne externě přes Alt+P, breakpoint apod.).
 *
 * Obsahuje informační text a tlačítko OK pro potvrzení.
 * Po kliknutí na OK se okno zavře a uživatel může pracovat s debuggerem.
 */
static void render_pause_info_modal(void)
{
    if (!g_dbg_ui.show_pause_info_modal)
        return;

    ImGui::OpenPopup(_L("Info##dbg_pause"));

    if (ImGui::BeginPopupModal(_L("Info##dbg_pause"), NULL,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("%s", _("Emulation was paused. Now you can make any changes."));
        ImGui::Separator();

        /* Zarovnání tlačítka OK na střed */
        float btn_width = 120.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button("OK", ImVec2(btn_width, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            g_dbg_ui.show_pause_info_modal = false;
            ImGui::CloseCurrentPopup();
        };

        ImGui::EndPopup();
    };
}


/*
 * Titul okna debuggeru — dynamický podle architektury (g_mzhal.arch_display_name).
 * Suffix " - Disassembly #1" odkazuje na hlavní disasm instanci v okně
 * (= je to "Disassembly #1", sekundární jsou #2..#5 v samostatných oknech).
 *
 * MZ-800:  "MZ-800 Debugger - Disassembly #1"
 * MZ-1500: "MZ-1500 Debugger - Disassembly #1"
 * MZ-700:  "MZ-700 Debugger - Disassembly #1"
 */
static const char *get_window_title(void)
{
    /* Runtime z g_mzhal (mzhal 11i); statický buffer = stabilní ImGui ID. */
    static char title[64];
    if (title[0] == '\0') {
        g_snprintf(title, sizeof(title), "%s Debugger - Disassembly #1",
                   g_mzhal.arch_display_name);
    }
    return title;
}


void imgui_debugger_window(bool *p_open)
{
    /* Focus-on-open: detekce false -> true transition. Při otevření okna
     * (Alt+D, File menu, ...) zavoláme SetNextWindowFocus jen jednou,
     * aby se okno vykreslilo nad ostatními. Žádné OS-level always-on-top
     * = standardní z-order, klik na jiné okno funguje normálně. */
    static bool s_prev_open = false;
    bool focus_on_open = (*p_open && !s_prev_open);
    s_prev_open = *p_open;

    if (!*p_open)
    {
        /*
         * Okno je zavřené — zajistíme korektní deaktivaci debuggeru.
         * Sem se dostaneme buď po debugger_hide_main_window() (z ESC, Alt+D,
         * File→Hide), nebo po kliknutí na X tlačítko v titulbaru (ImGui
         * nastaví *p_open = false interně).
         *
         * Kontrola g_debugger.active zabraňuje duplicitnímu volání
         * v případě, že debugger_hide_main_window() již proběhl.
         */
        if (g_debugger.active)
        {
            debugger_hide_main_window();
        };
        return;
    };

    /*
     * Inicializace při prvním otevření.
     * Provedeme pouze jednou — stav přetrvává po dobu života okna.
     */
    if (!g_dbg_ui.initialized)
    {
        debugger_ui_state_init();
        debugger_ui_state_refresh_from_cpu();
        g_debugger.active = 1;
        s_was_paused = EMULATOR_TEST_PAUSED;
    };

    /*
     * Synchronizace g_debugger.active s viditelností okna.
     * Jádro debuggeru potřebuje vědět, že je debugger aktivní,
     * aby provádělo záznam historie instrukcí atd.
     */
    if (!g_debugger.active)
    {
        g_debugger.active = 1;
    };

    /*
     * Detekce přechodu z animačního do editačního režimu.
     * Pokud se emulace právě zastavila (přešla z běhu do pauzy),
     * provedeme refresh UI stavu z aktuálních CPU registrů.
     */
    bool is_paused = EMULATOR_TEST_PAUSED;
    if (is_paused && !s_was_paused)
    {
        /* Přechod do editačního režimu — sync s CPU + okamžitý refresh dat */
        debugger_ui_state_refresh_from_cpu();
        dbg_refresh_request();
    };
    s_was_paused = is_paused;

    /*
     * Detekce resetu (i z paused stavu, kde se emulace nepřepnula z run
     * do pause - normální transition trigger by se nezavolal). UI by jinak
     * zobrazovala starý focus_addr i po změně PC na 0000.
     */
    static unsigned s_last_reset_count = 0;
    if (g_mzarch_main.reset_count != s_last_reset_count)
    {
        s_last_reset_count = g_mzarch_main.reset_count;
        debugger_ui_state_refresh_from_cpu();
        dbg_refresh_request();
    };

    /*
     * Detekce změny PC za paused stavu (= krokování F7/F8, dokončení
     * Run To Cursor / temporary BP). is_paused→is_paused přechod neexistuje
     * (transition výše se nespustí), ale focus_addr se musí přepočítat
     * pokud nové PC je mimo aktuálně viditelné okno disasm tabulky.
     *
     * Příklad: BP na 0000 obsahuje JP E8000. Po Step Into přes JP je PC
     * = E8000, ale focus_addr stále 0000. Bez tohoto checku by zelený PC
     * marker zůstal nezobrazen a uživatel by viděl statický 0000-okolí.
     *
     * Pokud je PC v dosahu zobrazených řádků, jen request refresh
     * (= rerender highlightu na správném řádku, ne scroll). Pokud mimo,
     * sync focus_addr na pc (= scroll na nové místo).
     */
    static uint16_t s_last_known_pc = 0;
    uint16_t current_pc = g_mzarch_main.cpu->pc;
    if (is_paused && current_pc != s_last_known_pc)
    {
        if (!dbg_dasm_is_addr_visible(current_pc))
            debugger_ui_state_refresh_from_cpu();
        dbg_refresh_request();
    };
    s_last_known_pc = current_pc;

    /*
     * Centralizované řízení refreshe dat — vyhodnotí pravidla a nastaví
     * g_dbg_ui.refresh.should_refresh pro všechny sekce v tomto framu.
     * Musí být voláno PŘED renderingem sekcí (dbg_disassembled_render atd.).
     *
     * Volá se přes dbg_disassembled_frame_init() (idempotentní per frame
     * přes guard ImGui::GetFrameCount()). Stejnou funkci volá i
     * dbg_extra_disasm_render_all() na svém začátku - díky guardu se
     * tick provede přesně 1× per frame, ať už je otevřené hlavní okno,
     * extra okno, nebo obojí současně.
     */
    dbg_disassembled_frame_init();

    /*
     * Nastavení okna:
     * - NoCollapse: zakazujeme sbalení okna (konvence projektu)
     * - MenuBar: okno má vlastní menubar
     * - Počáteční velikost se nastaví jen při prvním otevření
     */
    /* Vychozi velikost - dynamicky vypocet podle obsahu (CalcTextSize
     * + style) tak aby cely header + disasm tabulka byly viditelne hned
     * pri prvnim spusteni. Vyska 904 px = ~30 viditelnych disasm radku
     * (radek ~20 px) + horni historie + headery + slider. Aplikuje se
     * jen pri FirstUseEver - ulozeny layout v imgui.ini ji prepise.
     *
     * include_table_extras=false: T-states sloupec je defaultně skrytý
     * (per-instance show_tstates = false pro main, viz dbg_disassembled.cpp).
     * Pokud user T-states zapne, ImGui umožní okno roztáhnout - počáteční
     * šířka se počítá pro výchozí stav, ne pro hypotetický maximální.
     *
     * Vychozi pozice - dbg_auto_layout najde volné místo na primárním
     * monitoru, prednostne napravo od SDL emulator okna (= mimo main
     * viewport rect). Multi-viewport mod ImGui vytvori samostatne
     * platform okno pokud pozice leží mimo SDL window. */
    {
        float w = dbg_disasm_view_compute_default_width(false /* without T-states */);
        auto_layout_first_use_portrait(get_window_title(), w, 904.0f);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;

    /* V9.3 dual-step bring-to-front (krok 1 ze 2): konzumace pending flagu
     * z `debugger_window_request_focus()` PŘED Begin. ImGui-internal
     * z-order zvedne, ale v multi-viewport módu nestačí - krok 2 následuje
     * po Begin (= explicitní Platform_SetWindowFocus na cílový viewport).
     * focus_on_open (= jednorázový focus při otevření) se aplikuje
     * paralelně. */
    bool was_focus_pending = s_focus_pending;
    if (focus_on_open || s_focus_pending)
        ImGui::SetNextWindowFocus();
    s_focus_pending = false;

    if (!ImGui::Begin(get_window_title(), p_open, flags))
    {
        ImGui::End();
        /* X tlačítko mohlo nastavit *p_open = false i při kolapsnutém okně */
        if (!*p_open && g_debugger.active)
            debugger_hide_main_window();
        return;
    };

    /* V9.3 dual-step bring-to-front (krok 2 ze 2): po Begin víme, do
     * kterého platform viewportu okno patří. Pokud je v separate
     * viewportu (multi-viewport mode + okno mimo main window obrys),
     * volej explicit Platform_SetWindowFocus (= SDL_RaiseWindow) pro
     * OS-level bring-to-front. Bez tohoto by SetNextWindowFocus jen
     * blikl title bez reálného raise OS okna nad ostatní.
     * focus_on_open NEpotřebuje krok 2 - při otevření okna ImGui samo
     * platform raise provede; krok 2 je čistě pro request_focus cestu. */
    if (was_focus_pending)
    {
        ImGuiViewport *vp = ImGui::GetWindowViewport();
        ImGuiViewport *main_vp = ImGui::GetMainViewport();
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
        if (vp && vp != main_vp && vp->PlatformWindowCreated
            && pio.Platform_SetWindowFocus)
        {
            pio.Platform_SetWindowFocus(vp);
        };
    };

    /* =====================
     * TOP MENU
     * =====================
     * Menubar s položkami File, Emulation, Screen, Debugger Settings.
     * Detailní popis v dbg_topmenu.cpp.
     */
    dbg_topmenu_render(p_open);

    /* =====================
     * STŘEDOVÁ ČÁST
     * =====================
     * Hlavní pracovní oblast debuggeru. Vertikálně rozdělena na sloupce.
     * Aktuálně implementován pouze levý sloupec (Disassembled).
     * Pravý sloupec (registry, stack, memory map, ...) bude doplněn později.
     *
     * V animačním režimu (emulace běží):
     *   Středová část zobrazuje aktuální stav, ale interaktivní prvky
     *   jsou logicky disabled. Pokud uživatel klikne na interaktivní prvek,
     *   ten sám zavolá dbg_autopause_on_interaction(), která pozastaví
     *   emulaci a zobrazí modální info okno. Neinteraktivní prvky (historie)
     *   a ovládací prvky (menu, iconbar) tuto funkci nevolají.
     *
     * V editačním režimu (emulace v pauze):
     *   Plná interakce — navigace, selekce, context menu, editace.
     */

    /*
     * Výpočet dostupné výšky pro středovou část.
     * Odečteme odhadovanou výšku iconbaru:
     *   5px Dummy + 2px separator + 5px Dummy + 36px ikony + ~20px padding/spacing = ~68px
     */
    float content_height = ImGui::GetContentRegionAvail().y - 68.0f;
    if (content_height < 100.0f) content_height = 100.0f;

    /* Středová část - single-column layout (Variant B).
     *
     * Disassembled obsadí celou dostupnou šířku debug okna. CPU registry,
     * memory map a další náhledy jsou vyčleněny do samostatných plovoucích
     * oken - konkrétně CPU Registers v cpu_window.cpp (Alt+R, default
     * visible). Po zavření user-em zůstanou zavřené až do dalšího toggle.
     *
     * Historicky V0 obsahoval dvousloupcový BeginChild + SameLine +
     * BeginChild pattern s CPU panelem v pravém sloupci - odstraněno při
     * migraci z Variant A na Variant B.
     */
    dbg_disassembled_render(content_height);

    /* =====================
     * ICONBAR
     * =====================
     * Toolbar s tlačítky ve spodní části okna.
     * Obsahuje: Continue, Pause, Step Over, Step Into, Run to Cursor,
     *           Breakpoints, Memory, Disassembler.
     * Detailní popis v dbg_iconbar.cpp.
     */
    dbg_iconbar_render();

    /*
     * AUTOMATICKÁ PAUZA — ARCHITEKTURA
     * =================================
     * V animačním režimu (emulace běží) jsou interaktivní prvky
     * ve středové části logicky disabled. Pokud uživatel klikne
     * na interaktivní prvek (řádek disassembly, budoucí editor registrů...),
     * emulace se automaticky pozastaví a zobrazí se modální info okno.
     *
     * Implementace: interaktivní prvky volají dbg_autopause_on_interaction()
     * (definovaná v debugger_state.cpp). Ta rozhodne, zda je potřeba
     * pozastavit emulaci a zobrazit modal. Pokud vrátí true, volající
     * přeskočí normální zpracování kliku.
     *
     * Tímto se vyhýbáme plošné detekci kliků v okně, která chybně
     * zachytávala i kliknutí na menubar, iconbar tlačítka a neinteraktivní
     * prvky (horní tabulka historie).
     */

    /* =====================
     * MODÁLNÍ OKNO — PAUZA
     * =====================
     * Zobrazí se pouze pokud pauza byla vyvolána kliknutím na interaktivní
     * prvek v debuggeru (automatická pauza z animačního režimu).
     */
    render_pause_info_modal();

    /*
     * Pozn.: dbg_iasm_render() a dbg_focus_to_render() byly přesunuty do
     * main_window.cpp, aby se renderovaly i když hlavní debug okno není
     * otevřené. Sekundární disasm okna (#2 - #5) tak mohou tyto modální
     * dialogy otevřít z context menu nezávisle na stavu hlavního okna.
     */

    /* =====================
     * KLÁVESOVÉ ZKRATKY OKNA
     * =====================
     * ESC: zavře okno debuggeru
     * Alt+D: toggle okna (zavře debugger)
     * Ctrl+R: vynutí refresh obrazovky emulátoru
     */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            debugger_hide_main_window();
        };

        /* Ctrl+R: Forced Full Screen Refresh */
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false))
        {
            debugger_forced_screen_update();
        };

        /*
         * Globální zkratky (Alt+P, Alt+M, Alt+B, Alt+D, F12, ...).
         * Normálně se volají z main_window.cpp, ale jen pokud má hlavní okno
         * focus. Když má focus debugger, musíme je volat odtud.
         * Guard !MainWindowHasFocus zabraňuje dvojímu zavolání na jednom framu.
         */
        if (!g_gui->MainWindowHasFocus)
        {
            imgui_global_shortcuts();
        };
    };

    ImGui::End();

    /* X tlačítko v titulbaru — ImGui nastavil *p_open = false v Begin() */
    if (!*p_open && g_debugger.active)
        debugger_hide_main_window();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
