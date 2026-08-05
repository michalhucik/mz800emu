/*
 * dbg_topmenu.cpp — Top menu hlavního okna debuggeru
 *
 * Menubar je umístěn v horní části hlavního okna debuggeru a obsahuje:
 *
 * FILE
 * ----
 * - Load MZF into RAM — otevře dialog pro načtení MZF souboru do RAM
 *   na zadanou adresu. (Zatím neimplementováno — placeholder.)
 * - Hide — skryje hlavní okno debuggeru. Ekvivalent Alt+D nebo ESC.
 *
 * EMULATION
 * ---------
 * - Reset (F12) — provede reset emulátoru
 * - Max Speed (Alt+M) — přepíná mezi normální a maximální rychlostí
 * - Pause/Resume (Alt+P) — pozastaví nebo obnoví emulaci
 *
 * SCREEN
 * ------
 * - Forced Full Screen Refresh (Ctrl+R) — vynutí kompletní překreslení
 *   obrazovky emulátoru. Užitečné při krokovacím režimu, kdy se obrazovka
 *   normálně neaktualizuje po každém kroku.
 *
 * DEBUGGER SETTINGS
 * -----------------
 * - Screen:
 *   - Auto refresh on edit: pokud uživatel přes debugger způsobí změnu
 *     obrazu (g_debugger.screen_refresh_on_edit), debugger automaticky
 *     vyvolá force screen update. Konkrétně se to týká:
 *       * zápisu do paměti přes debugger_memory_write_byte(), pokud
 *         v aktuálním banking/mode kontextu dopadl do VRAM/CGRAM/PCG
 *         (= zdroj inline assembleru, edit stack cell, future memory
 *         browser, ...);
 *       * změny DMD modu v Memory Map okně (MZ-800);
 *       * změny banking v Memory Map okně (left-click cell, popup
 *         Mount/Umount/Mount-All/Umount-All).
 *     Bez zapnutí flagu zůstává obrazovka beze změny do dalšího framu
 *     nebo step CPU.
 *   - Auto refresh at every CPU step: po každém kroku CPU překreslit
 *     obrazovku (g_debugger.screen_refresh_at_step)
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
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <glib.h>                 /* g_strdup / g_free pro trace dir (g_-spravovaný) */
#include "emulator/emulator.h"
#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "debugger/trace/cputrack.h"
#include "debugger/trace/iorqlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/marklog.h"
#include "iface/iface_video.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/heatmap/mhmap_window.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/topmenu/topmenu.h"

#include "dbg_topmenu.h"

/* Stav otevření dialogu pro výběr CDL exportního adresáře.
 * Drží se mimo menu callback, aby se dialog mohl renderovat i po zavření menu. */
static bool s_cdl_dir_dialog_open = false;

/* Stav sdíleného dialogu "Set directory..." pro trace-suite kanály. Při kliknutí
 * v menu se naplní seznam cílových `char**` (dir polí configu, která se mají
 * přepsat) a otevře se IGFD; vlastní Display + zápis je v dbg_render_trace_dir_dialog
 * (mimo menu, aby dialog přežil zavření menu). Pro "All channels" je targets více. */
static bool   s_trace_dir_dialog_open = false;
static char **s_trace_dir_targets[5];
static int    s_trace_dir_target_count = 0;


/*
 * Menu "File":
 * - Load MZF into RAM (placeholder — zatím neimplementováno)
 * - Hide — zavře okno debuggeru (nastaví *p_open na false)
 */
static void dbg_menu_file(bool *p_open)
{
    if (ImGui::BeginMenu(_L("File")))
    {
        /* Placeholder — Load MZF into RAM bude implementován později */
        if (ImGui::MenuItem(_L("Load MZF into RAM...")))
        {
            /* TODO: implementovat dialog pro načtení MZF do RAM */
        };

        ImGui::Separator();

        /*
         * Hide — skryje okno debuggeru přes debugger_hide_main_window().
         * Stejný efekt jako Alt+D nebo ESC.
         */
        if (ImGui::MenuItem(_L("Hide"), "Alt+D"))
        {
            debugger_hide_main_window();
        };

        ImGui::EndMenu();
    };
}


/*
 * Menu "Emulation":
 * - Reset (F12) — reset emulátoru
 * - Max Speed (Alt+M) — toggle maximální rychlost
 * - Pause/Resume (Alt+P) — toggle pauza emulace
 *
 * Tyto položky jsou záměrně duplicitní s hlavním menu emulátoru,
 * protože debugger okno může být aktivní místo hlavního okna.
 */
static void dbg_menu_emulation(void)
{
    if (ImGui::BeginMenu(_L("Emulation")))
    {
        if (ImGui::MenuItem(_L("Reset"), "F12"))
        {
            dbg_ui_reset();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Max Speed"), "Alt+M", EMULATOR_TEST_MAX_SPEED))
        {
            emulator_max_speed(!EMULATOR_TEST_MAX_SPEED);
        };

        if (ImGui::MenuItem(_L("Pause Emulation"), "Alt+P", EMULATOR_TEST_PAUSED))
        {
            dbg_ui_pause_toggle();
        };

        ImGui::Separator();

        /* Forced Full Screen Refresh - presunute z drivejsiho top-level
         * menu Screen (= ktere bylo zruseno, melo jen tuto polozku).
         * Vynuti kompletni prekresleni emulatoru obrazovky - klicove pri
         * krokovacim rezimu kde se obrazovka normalne neaktualizuje. */
        if (ImGui::MenuItem(_L("Forced Full Screen Refresh"), "Ctrl+R"))
        {
            debugger_forced_screen_update();
        };

        ImGui::EndMenu();
    };
}


/*
 * Menu "Debugger Settings":
 *
 * Polozka "Animated Updates" (checkbox):
 *   Ridi chovani debuggeru, kdyz emulace bezi (animacni rezim).
 *   - off: debugger se neaktualizuje, zobrazi se indikator "paused"
 *   - on:  debugger se periodicky aktualizuje - uzivatel vidi prubeh
 *          emulace v realnem case (registry, disassembly, stack...)
 *
 * Podmenu "Screen":
 *   - Auto refresh on edit: při debugger-iniciované akci, která může změnit
 *     vizuální obraz, automaticky překreslit obrazovku emulátoru. Pokrývá
 *     (a) zápis do VRAM/CGRAM/PCG přes debugger_memory_write_byte() v aktuálním
 *     banking/mode kontextu (inline assembler, edit stack cell, future
 *     memory browser, ...), a (b) změnu DMD modu / banking v Memory Map
 *     okně. Bez tohoto se změny projeví až při dalším cyklu vykreslování.
 *     Logika je centralizovaná v debugger_screen_refresh_if_enabled().
 *   - Auto refresh at every CPU step: po každém kroku CPU (F7/F8)
 *     překreslit obrazovku. Užitečné při ladění grafického kódu,
 *     ale zpomaluje krokování.
 */

/**
 * @brief Sdílený InputText pro limit velikosti záznamu trace kanálu (max_total_mb).
 *
 * Vykreslí pole "Max size [MB]" s aktuální hodnotou + hint (tooltip). Hodnota
 * je v MB (0 = bez omezení). Aplikuje se až při (znovu)spuštění záznamu kanálu
 * - writer čte max_total_mb v okamžiku open. Změna je prostý zápis configu (int),
 * nepotřebuje recompute ani thread marshalling.
 *
 * @param p_max_total_mb Ukazatel na pole max_total_mb v configu kanálu.
 * @return true pokud uživatel hodnotu právě potvrdil (Enter) - caller ji už má
 *         zapsanou v @p *p_max_total_mb (u "All channels" ji rozkopíruje do všech).
 */
static bool dbg_trace_max_size_input ( unsigned *p_max_total_mb )
{
    char buf[ 16 ];
    snprintf ( buf, sizeof ( buf ), "%u", *p_max_total_mb );
    ImGui::SetNextItemWidth ( 80.0f );
    bool changed = ImGui::InputText ( _L ( "Max size [MB]" ), buf, sizeof ( buf ),
                                      ImGuiInputTextFlags_CharsDecimal |
                                      ImGuiInputTextFlags_EnterReturnsTrue );
    if ( ImGui::IsItemHovered ( ) )
        ImGui::SetTooltip ( "%s", _ ( "Maximum recording size per channel in MB. "
                                      "0 = unlimited. Takes effect when recording "
                                      "(re)starts." ) );
    if ( changed )
        *p_max_total_mb = (unsigned) strtoul ( buf, NULL, 10 );
    return changed;
}


/**
 * @brief Sdílený InputText pro velikost chunku trace kanálu (chunk_mb) + hint.
 *
 * Chunk = RAM buffer per kanál; po naplnění se flushne na disk jako další
 * `<name>.NNN.bin`. Hodnota v MB, ořezaná do rozsahu 1..4096 (shodně s cfg).
 * Aplikuje se až při (znovu)spuštění záznamu (writer čte při open).
 *
 * @param p_chunk_mb Ukazatel na pole chunk_mb v configu kanálu.
 * @return true pokud uživatel hodnotu právě potvrdil (Enter).
 */
static bool dbg_trace_chunk_mb_input ( unsigned *p_chunk_mb )
{
    char buf[ 16 ];
    snprintf ( buf, sizeof ( buf ), "%u", *p_chunk_mb );
    ImGui::SetNextItemWidth ( 80.0f );
    bool changed = ImGui::InputText ( _L ( "Chunk [MB]" ), buf, sizeof ( buf ),
                                      ImGuiInputTextFlags_CharsDecimal |
                                      ImGuiInputTextFlags_EnterReturnsTrue );
    if ( ImGui::IsItemHovered ( ) )
        ImGui::SetTooltip ( "%s", _ ( "RAM buffer size per chunk in MB before it is "
                                      "flushed to disk. Takes effect when recording "
                                      "(re)starts." ) );
    if ( changed ) {
        unsigned v = (unsigned) strtoul ( buf, NULL, 10 );
        if ( v < 1u ) v = 1u;
        if ( v > 4096u ) v = 4096u;
        *p_chunk_mb = v;
    }
    return changed;
}


/**
 * @brief Otevře sdílený IGFD dialog "Set directory..." pro trace kanály.
 *
 * Cílová `dir` pole se naplní volajícím do @ref s_trace_dir_targets před voláním.
 * Display + zápis vybrané cesty řeší @ref dbg_render_trace_dir_dialog (mimo menu).
 *
 * @param current Aktuální cesta (výchozí pozice prohlížeče); NULL/prázdné -> ".".
 */
static void dbg_trace_open_dir_dialog ( const char *current )
{
    s_trace_dir_dialog_open = true;
    IGFD::FileDialogConfig config;
    config.path = ( current && current[ 0 ] ) ? current : ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton;
    ImGuiFileDialog::Instance ( )->OpenDialog ( "TraceSetDir",
                                                _ ( "Select trace output directory" ),
                                                nullptr, config );
}


/**
 * @brief Vykreslí společné položky submenu jednoho trace kanálu.
 *
 * mode radio (Off / Only With Debug Window / Always) -> při změně recompute na
 * emu vlákně (dbg_ui_debugger_state_recompute), Save on Exit, Max size [MB],
 * Chunk [MB] a "Set directory...". Kanálově specifické položky (cputrack PC range,
 * marklog Stdout) přidává volající kolem tohoto helperu.
 */
static void dbg_trace_channel_common_items ( en_TLOG_MODE *mode, char **dir,
                                             unsigned *chunk_mb, unsigned *max_total_mb,
                                             unsigned *save_on_exit )
{
    bool m_off = ( *mode == TLOG_MODE_OFF );
    bool m_win = ( *mode == TLOG_MODE_WITH_WINDOW );
    bool m_alw = ( *mode == TLOG_MODE_ALWAYS );
    if ( ImGui::MenuItem ( _L ( "Off" ), NULL, m_off ) ) {
        *mode = TLOG_MODE_OFF; dbg_ui_debugger_state_recompute ( ); }
    if ( ImGui::MenuItem ( _L ( "Only With Debug Window" ), NULL, m_win ) ) {
        *mode = TLOG_MODE_WITH_WINDOW; dbg_ui_debugger_state_recompute ( ); }
    if ( ImGui::MenuItem ( _L ( "Always" ), NULL, m_alw ) ) {
        *mode = TLOG_MODE_ALWAYS; dbg_ui_debugger_state_recompute ( ); }

    ImGui::Separator ( );

    bool save = ( *save_on_exit != 0 );
    if ( ImGui::MenuItem ( _L ( "Save on Exit" ), NULL, save ) ) {
        *save_on_exit = save ? 0u : 1u; }
    dbg_trace_max_size_input ( max_total_mb );
    dbg_trace_chunk_mb_input ( chunk_mb );
    if ( ImGui::MenuItem ( _L ( "Set directory..." ) ) ) {
        s_trace_dir_targets[ 0 ] = dir;
        s_trace_dir_target_count = 1;
        dbg_trace_open_dir_dialog ( *dir );
    }
}

static void dbg_menu_settings(void)
{
    if (ImGui::BeginMenu(_L("Settings")))
    {
        /* Podmenu: CPU Instruction History
         * Řídí, kdy běží zaznamenávání instrukční historie (32-instr ring
         * zobrazený v horní části Debugger okna).
         *  - Off: vůbec nezaznamenávat.
         *  - Only With Debug Window (default): jen pokud je debug okno otevřené.
         *  - Always: trvale, nezávisle na stavu debug okna.
         *
         * Pozn.: Toto je informativní instrukční historie. Pro plnohodnotné
         * tracking logy s timestampy a RAM dumpem viz subsystém cputrack
         * (Debugger Settings -> Trace Suite). */
        if (ImGui::BeginMenu(_L("CPU Instruction History")))
        {
            bool tl_off         = (g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_OFF);
            bool tl_with_window = (g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_WITH_WINDOW);
            bool tl_window_or_bp = (g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP);
            bool tl_always      = (g_debugger.cpuhist_mode == DEBUGGER_CPUHIST_MODE_ALWAYS);

            if (ImGui::MenuItem(_L("Off"), NULL, tl_off))
            {
                g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_OFF;
                dbg_ui_debugger_state_recompute();
            };

            if (ImGui::MenuItem(_L("Only With Debug Window"), NULL, tl_with_window))
            {
                g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW;
                dbg_ui_debugger_state_recompute();
            };

            if (ImGui::MenuItem(_L("With Debug Window or Breakpoints"), NULL, tl_window_or_bp))
            {
                g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_WITH_WINDOW_OR_BP;
                dbg_ui_debugger_state_recompute();
            };

            if (ImGui::MenuItem(_L("Always"), NULL, tl_always))
            {
                g_debugger.cpuhist_mode = DEBUGGER_CPUHIST_MODE_ALWAYS;
                dbg_ui_debugger_state_recompute();
            };

            ImGui::EndMenu();
        };

        /* Podmenu: CDL (Code/Data Logger)
         * Řídí, kdy běží zaznamenávání access counterů (R/W/X) do paměťových buněk.
         *  - Off (default): vůbec nezaznamenává, žádná režie.
         *  - Only With Debug Window: jen pokud je debug okno otevřené.
         *  - Always: trvale.
         * Recording je drahý (per-access counter inkrement), proto default OFF.
         *
         * Dále:
         *  - Export on Exit: při ukončení emulátoru exportovat CDL data.
         *  - Set directory...: vybrat cílový adresář pro export. */
        if (ImGui::BeginMenu(_L("CDL")))
        {
            bool mh_off         = (g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_OFF);
            bool mh_with_window = (g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_WITH_WINDOW);
            bool mh_always      = (g_debugger.mhmap_mode == DEBUGGER_MHMAP_MODE_ALWAYS);

            if (ImGui::MenuItem(_L("Off"), NULL, mh_off))
            {
                mhmap_set_mode(DEBUGGER_MHMAP_MODE_OFF);
            };

            if (ImGui::MenuItem(_L("Only With Debug Window"), NULL, mh_with_window))
            {
                mhmap_set_mode(DEBUGGER_MHMAP_MODE_WITH_WINDOW);
            };

            if (ImGui::MenuItem(_L("Always"), NULL, mh_always))
            {
                mhmap_set_mode(DEBUGGER_MHMAP_MODE_ALWAYS);
            };

            ImGui::Separator();

            bool export_on_exit = (g_debugger.cdl_export_on_exit != 0);
            if (ImGui::MenuItem(_L("Export on Exit"), NULL, export_on_exit))
            {
                g_debugger.cdl_export_on_exit = export_on_exit ? 0 : 1;
            };

            ImGui::Separator();

            /* Show heatmap window - otevře samostatné Memory Heatmap okno
             * (vizualizace counterů, filtry, import). Toggle stejnou flag
             * jako menu Debugger > Memory Heatmap. */
            if (ImGui::MenuItem(_L("Show heatmap window"), NULL, g_gui->showMemoryHeatmapWindow))
            {
                mhmap_window_show_hide();
            };

            ImGui::Separator();

            /* Set directory... — otevře file browser pro výběr cílového adresáře. */
            if (ImGui::MenuItem(_L("Set directory...")))
            {
                s_cdl_dir_dialog_open = true;
                IGFD::FileDialogConfig config;
                config.path = (g_debugger.cdl_export_dir && g_debugger.cdl_export_dir[0])
                              ? g_debugger.cdl_export_dir : ".";
                config.countSelectionMax = 1;
                config.flags = ImGuiFileDialogFlags_Modal |
                               ImGuiFileDialogFlags_DontShowHiddenFiles |
                               ImGuiFileDialogFlags_ShowDevicesButton;
                ImGuiFileDialog::Instance()->OpenDialog("CdlExportDir",
                                                        _("Select CDL export directory"),
                                                        nullptr, config);
            };

            ImGui::EndMenu();
        };

        /* Podmenu: Trace Suite (cputrack/iorqlog/intlog/hwlog/marklog).
         * Per-subsystém: mode radio (Off/Window/Always) + Save on Exit +
         * Max size [MB] + Chunk [MB] + Set directory; CPU Track navíc PC range,
         * Marker Log navíc Stdout toggle. "All channels" ovládá všech 5 najednou.
         * Plné GUI viewer = V2.
         *
         * Recording start trigger viz mode:
         *  - Off: nikdy nezaznamenává
         *  - With Window: aktivní jen pokud je debug okno otevřené
         *  - Always: trvale, nezávisle na stavu okna
         *
         * Soubory v <trace-dir>/<basename>.json + <basename>.NNN.bin chunky.
         * Detail v docs/cz/debugger/Trace_Suite.md. */
        if (ImGui::BeginMenu(_L("Trace Suite")))
        {
            /* Globální ovládání všech 5 trace kanálů (cputrack/iorqlog/intlog/
             * hwlog/marklog) najednou - mode / Save on Exit / Max size / Chunk /
             * Set directory jedním kliknutím (změna mode přes jediný recompute
             * na emu vlákně). Položky sdílí překlad s per-kanálovými menu (jiný
             * BeginMenu scope = žádná kolize ImGui ID). */
            if (ImGui::BeginMenu(_L("All channels")))
            {
                /* Lokální pole 5 kanálů - společná pole (mode/dir/chunk/max/save).
                 * Smyčky drží All channels v synci s per-kanálovými submenu. */
                struct { en_TLOG_MODE *mode; char **dir; unsigned *chunk_mb;
                         unsigned *max_total_mb; unsigned *save_on_exit; } ch[5] = {
                    { &g_cputrack_config.mode, &g_cputrack_config.dir, &g_cputrack_config.chunk_mb, &g_cputrack_config.max_total_mb, &g_cputrack_config.save_on_exit },
                    { &g_iorqlog_config.mode,  &g_iorqlog_config.dir,  &g_iorqlog_config.chunk_mb,  &g_iorqlog_config.max_total_mb,  &g_iorqlog_config.save_on_exit },
                    { &g_intlog_config.mode,   &g_intlog_config.dir,   &g_intlog_config.chunk_mb,   &g_intlog_config.max_total_mb,   &g_intlog_config.save_on_exit },
                    { &g_hwlog_config.mode,    &g_hwlog_config.dir,    &g_hwlog_config.chunk_mb,    &g_hwlog_config.max_total_mb,    &g_hwlog_config.save_on_exit },
                    { &g_marklog_config.mode,  &g_marklog_config.dir,  &g_marklog_config.chunk_mb,  &g_marklog_config.max_total_mb,  &g_marklog_config.save_on_exit },
                };

                /* Checkmark/agregace jen pokud VŠECH 5 kanálů sdílí daný stav. */
                bool all_off = true, all_win = true, all_alw = true, all_save = true;
                for (auto &c : ch) {
                    all_off  &= (*c.mode == TLOG_MODE_OFF);
                    all_win  &= (*c.mode == TLOG_MODE_WITH_WINDOW);
                    all_alw  &= (*c.mode == TLOG_MODE_ALWAYS);
                    all_save &= (*c.save_on_exit != 0);
                }

                if (ImGui::MenuItem(_L("Off"), NULL, all_off)) {
                    for (auto &c : ch) *c.mode = TLOG_MODE_OFF;
                    dbg_ui_debugger_state_recompute();
                }
                if (ImGui::MenuItem(_L("Only With Debug Window"), NULL, all_win)) {
                    for (auto &c : ch) *c.mode = TLOG_MODE_WITH_WINDOW;
                    dbg_ui_debugger_state_recompute();
                }
                if (ImGui::MenuItem(_L("Always"), NULL, all_alw)) {
                    for (auto &c : ch) *c.mode = TLOG_MODE_ALWAYS;
                    dbg_ui_debugger_state_recompute();
                }

                ImGui::Separator();

                /* Save on Exit pro všech 5: checked jen když mají VŠECHNY zapnuto;
                 * klik přepne všechny na opačný stav. */
                if (ImGui::MenuItem(_L("Save on Exit"), NULL, all_save)) {
                    unsigned v = all_save ? 0u : 1u;
                    for (auto &c : ch) *c.save_on_exit = v;
                }

                /* Max size / Chunk: zobraz reprezentativně hodnotu prvního kanálu;
                 * při potvrzení rozkopíruj do všech 5. */
                unsigned all_mt = *ch[0].max_total_mb;
                if (dbg_trace_max_size_input(&all_mt))
                    for (auto &c : ch) *c.max_total_mb = all_mt;
                unsigned all_chunk = *ch[0].chunk_mb;
                if (dbg_trace_chunk_mb_input(&all_chunk))
                    for (auto &c : ch) *c.chunk_mb = all_chunk;

                /* Set directory pro všech 5 kanálů najednou. */
                if (ImGui::MenuItem(_L("Set directory..."))) {
                    for (int i = 0; i < 5; i++) s_trace_dir_targets[i] = ch[i].dir;
                    s_trace_dir_target_count = 5;
                    dbg_trace_open_dir_dialog(*ch[0].dir);
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            /* CPU Track: společné položky (mode / Save on Exit / Max size /
             * Chunk / Set directory) + navíc Z17b range-scope PC filtr. */
            if (ImGui::BeginMenu(_L("CPU Track")))
            {
                dbg_trace_channel_common_items(&g_cputrack_config.mode,
                                               &g_cputrack_config.dir,
                                               &g_cputrack_config.chunk_mb,
                                               &g_cputrack_config.max_total_mb,
                                               &g_cputrack_config.save_on_exit);

                ImGui::Separator();
                /* Range-scope PC filtr. Buffery se inicializují z config při
                 * každém vykreslení (menu je krátkodobé, držet syncované je
                 * levné a vyhne se "stale buffer" stavu). Hex parsing přes
                 * strtoul, hodnoty ořezané na 16 bitů. */
                ImGui::TextDisabled("%s", _("PC range filter [lo,hi]:"));
                char lo_buf[8];
                char hi_buf[8];
                snprintf(lo_buf, sizeof(lo_buf), "%04X", (unsigned)g_cputrack_config.pc_range_lo);
                snprintf(hi_buf, sizeof(hi_buf), "%04X", (unsigned)g_cputrack_config.pc_range_hi);
                ImGui::SetNextItemWidth(64.0f);
                if (ImGui::InputText(_L("lo (hex)"), lo_buf, sizeof(lo_buf),
                                     ImGuiInputTextFlags_CharsHexadecimal |
                                     ImGuiInputTextFlags_CharsUppercase |
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    g_cputrack_config.pc_range_lo = (uint16_t)(strtoul(lo_buf, NULL, 16) & 0xFFFF);
                }
                ImGui::SetNextItemWidth(64.0f);
                if (ImGui::InputText(_L("hi (hex)"), hi_buf, sizeof(hi_buf),
                                     ImGuiInputTextFlags_CharsHexadecimal |
                                     ImGuiInputTextFlags_CharsUppercase |
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    g_cputrack_config.pc_range_hi = (uint16_t)(strtoul(hi_buf, NULL, 16) & 0xFFFF);
                }
                if (ImGui::MenuItem(_L("Reset range (whole space)"))) {
                    g_cputrack_config.pc_range_lo = 0x0000;
                    g_cputrack_config.pc_range_hi = 0xFFFF;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_L("IORQ Log")))
            {
                dbg_trace_channel_common_items(&g_iorqlog_config.mode,
                                               &g_iorqlog_config.dir,
                                               &g_iorqlog_config.chunk_mb,
                                               &g_iorqlog_config.max_total_mb,
                                               &g_iorqlog_config.save_on_exit);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_L("Interrupt Log")))
            {
                dbg_trace_channel_common_items(&g_intlog_config.mode,
                                               &g_intlog_config.dir,
                                               &g_intlog_config.chunk_mb,
                                               &g_intlog_config.max_total_mb,
                                               &g_intlog_config.save_on_exit);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(_L("HW Log")))
            {
                dbg_trace_channel_common_items(&g_hwlog_config.mode,
                                               &g_hwlog_config.dir,
                                               &g_hwlog_config.chunk_mb,
                                               &g_hwlog_config.max_total_mb,
                                               &g_hwlog_config.save_on_exit);
                ImGui::EndMenu();
            }
            /* Marker Log (marklog) - 5. trace-suite kanál. Společné položky +
             * navíc Stdout toggle (back-compat [BP-MARK] na stdout, nezávislý
             * na binárním logu). */
            if (ImGui::BeginMenu(_L("Marker Log")))
            {
                dbg_trace_channel_common_items(&g_marklog_config.mode,
                                               &g_marklog_config.dir,
                                               &g_marklog_config.chunk_mb,
                                               &g_marklog_config.max_total_mb,
                                               &g_marklog_config.save_on_exit);
                ImGui::Separator();
                bool md_stdout = (g_marklog_config.stdout_enabled != 0);
                if (ImGui::MenuItem(_L("Print marks to stdout"), NULL, md_stdout)) {
                    g_marklog_config.stdout_enabled = md_stdout ? 0u : 1u;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _("Print [BP-MARK] <name> to stdout when a "
                                              "mark fires; independent of the binary log."));
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        ImGui::Separator();

        /* Podmenu: Disassembled
         * UI volby pro dolní tabulku Disassembled (sekce hlavního okna).
         * Hodnoty perzistovány v cfgmain (sekce DEBUGGER) přes
         * cfgelement_set_handlers - viz debugger.c.
         *
         * Show T-states column - per uživatel přesunuto z globálního menu
         * na per-instance flag v hlavičce každého disasm view (krok 5).
         * Globální flag g_debugger.disasm_show_tstates_col byl odstraněn. */
        if (ImGui::BeginMenu(_L("Disassembled")))
        {
            /* Show branch arrows - vizualizace skoků v ICONS gutteru
             * (rozšířený o ~16 px pro arrow svislé čáry, BPT/PC ikony
             * zůstanou napravo). Default ON. Šipky pro CALL = modré,
             * pro JP/JR/DJNZ = šedé. Stub se symbolem se kreslí pokud
             * cíl skoku leží mimo viditelné okno.
             *
             * Pozn: Show branch arrows zůstává globální (per uživatel
             * přání); jen Show T-states column se zpřístupnil per-instance. */
            bool show_arrows = (g_debugger.disasm_show_branch_arrows != 0);
            if (ImGui::MenuItem(_L("Show branch arrows"), NULL, show_arrows))
            {
                g_debugger.disasm_show_branch_arrows = show_arrows ? 0 : 1;
            };

            ImGui::EndMenu();
        };

        /* Podmenu: Screen */
        if (ImGui::BeginMenu(_L("Screen")))
        {
            /*
             * Auto refresh on edit — checkbox.
             * Při editaci VRAM (inline assembler, memory browser) automaticky
             * překreslit obrazovku emulátoru. Hodnota v g_debugger.screen_refresh_on_edit.
             */
            bool screen_refresh_on_edit = (g_debugger.screen_refresh_on_edit != 0);
            if (ImGui::MenuItem(_L("Auto refresh on edit"), NULL, screen_refresh_on_edit))
            {
                g_debugger.screen_refresh_on_edit = screen_refresh_on_edit ? 0 : 1;
            };

            /*
             * Auto refresh at every CPU step — checkbox.
             * Po každém kroku CPU (Step Into/Step Over) překreslit obrazovku.
             * Hodnota v g_debugger.screen_refresh_at_step.
             */
            bool screen_refresh_at_step = (g_debugger.screen_refresh_at_step != 0);
            if (ImGui::MenuItem(_L("Auto refresh at every CPU step"), NULL, screen_refresh_at_step))
            {
                g_debugger.screen_refresh_at_step = screen_refresh_at_step ? 0 : 1;
            };

            ImGui::EndMenu();
        };

        ImGui::EndMenu();
    };
}


/*
 * Render dialogu pro výběr CDL exportního adresáře.
 *
 * Dialog se otevírá z menu "Debugger Settings > CDL > Set directory...",
 * ale display musí být mimo menu (menu se po kliknutí zavře a dialog by se
 * jinak nerenderoval). Po výběru se uložená cesta zapíše do
 * g_debugger.cdl_export_dir (alokovaný malloc, starý se uvolní).
 */
static void dbg_render_cdl_dir_dialog(void)
{
    if (!s_cdl_dir_dialog_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    if (ImGuiFileDialog::Instance()->Display("CdlExportDir"))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
            /* Uvolnit starou hodnotu (alokovanou cfgmodule_*) a nahradit novou.
             * cfgmodule používá strdup, takže i my musíme alokovat na heap. */
            char *new_dir = (char *)malloc(dir.size() + 1);
            if (new_dir)
            {
                memcpy(new_dir, dir.c_str(), dir.size() + 1);
                free(g_debugger.cdl_export_dir);
                g_debugger.cdl_export_dir = new_dir;
            };
        };
        ImGuiFileDialog::Instance()->Close();
        s_cdl_dir_dialog_open = false;
    };
}


/*
 * Render sdíleného dialogu "Set directory..." pro trace-suite kanály.
 *
 * Otevírá se z menu (Trace Suite > <kanál> nebo All channels > Set directory...),
 * display musí být mimo menu (menu se po kliknutí zavře). Po výběru se cesta
 * zapíše do všech cílových dir polí v s_trace_dir_targets - g_free starého +
 * g_strdup nového, shodně s tím, jak trace cfg.dir spravuje
 * cputrack_apply_cli_options / reclife_redirect_path.
 */
static void dbg_render_trace_dir_dialog(void)
{
    if (!s_trace_dir_dialog_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    if (ImGuiFileDialog::Instance()->Display("TraceSetDir"))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
            for (int i = 0; i < s_trace_dir_target_count; i++)
            {
                if (!s_trace_dir_targets[i])
                    continue;
                char *new_dir = g_strdup(dir.c_str());
                g_free(*s_trace_dir_targets[i]);
                *s_trace_dir_targets[i] = new_dir;
            };
        };
        ImGuiFileDialog::Instance()->Close();
        s_trace_dir_dialog_open = false;
        s_trace_dir_target_count = 0;
    };
}


void dbg_topmenu_render(bool *p_open)
{
    if (ImGui::BeginMenuBar())
    {
        dbg_menu_file(p_open);
        /* DbgTool - sdílí impl s topmenu Debugger (imgui_menu_debugger),
         * jen vynechá první položku "MZ-800 Debugger" + separator. Skrz
         * caller param menu má label "DbgTool" (= odlišení od topmenu). */
        imgui_menu_debugger(DBG_MENU_CALLER_DEBUGGER_WINDOW);
        dbg_menu_emulation();
        /* dbg_menu_screen() zruseno - jeho jedina polozka "Forced Full
         * Screen Refresh" presunuta do menu Emulation (oddelena
         * separatorem). */
        dbg_menu_settings();
        ImGui::EndMenuBar();
    };

    /* Dialogy mimo menu bar - musí se renderovat každý frame nezávisle. */
    dbg_render_cdl_dir_dialog();
    dbg_render_trace_dir_dialog();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
