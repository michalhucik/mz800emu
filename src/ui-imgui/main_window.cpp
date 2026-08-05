#include "main.h"
#include "emulator/mzarch/mzhal.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/plotter/plotter_window.h"
#include "iface/iface_keyboard.h"
#include "hw-generic/pio8255/pio8255.h"
#include "imgui_windows.h"
#include "emulator/emulator.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "ui-imgui/debugger/sections/dbg_extra_disasm.h"
#include "ui-imgui/debugger/sections/dbg_focus_to.h"
#include "ui-imgui/debugger/sections/dbg_inline_asm.h"
#include "ui-imgui/debugger/profiler/profiler_window.h"
/* Per-chip-panels F1 scaffold: per-chip detail okna pro CTC/PPI/PIO/PSG. */
#include "ui-imgui/debugger/ctc_window.h"
#include "ui-imgui/debugger/ppi_window.h"
#include "ui-imgui/debugger/pioz80_window.h"
#include "ui-imgui/debugger/psg_window.h"
#include "ui-imgui/debugger/psg_audio_scope_window.h"
/* gdg-panel F1 scaffold: GDG state inspector (per-arch dispatch uvnitř). */
#include "ui-imgui/debugger/gdg_window.h"
/* Memory Browser V0 hex MVP - hex view paměti přes dbgapi_regions. */
#include "ui-imgui/debugger/membrowser/membrowser_window.h"
#include "ui-imgui/debugger/membrowser/membrowser_diff_window.h"
#include "ui-imgui/debugger/membrowser/membrowser_pcg.h"
#include "ui-imgui/debugger/membrowser/membrowser_char_inserter.h"
/* Disassembler V1 - samostatné range-based disasm okno (mutant). */
#include "ui-imgui/debugger/dasm_window/dasm_window.h"
#endif
#include "emulator/emulator_measuring.h"
#include "version_check/version_check.h"
#include "message/message_window.h"

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
/* MCP Activity okno (V1.C.2) - render funkce volaná za běhu pokud
 * g_gui->showMcpActivityWindow je true. Init/shutdown řídí bootstrap
 * UI vrstva (myimgui_init_cb / myimgui_destroy_cb). */
#include "ui-imgui/mcp_activity/mcp_activity_window.h"
#include "ui-imgui/mcp_activity/action_toast.h"
#endif

extern "C"
{
    void imgui_main_window(GLuint texture);

    /* Snapshot dialogy */
    void imgui_snapshot_save_dialog(void);
    void imgui_snapshot_load_dialog(void);
    void imgui_snapshot_setup_dialog(void);
    void imgui_snapshot_notification(void);
    void imgui_snapshot_quicksave_handler(void);

    /* Jazyk */
    void imgui_language_window(void);

    /* MCP server settings (V0.B.4). Pri NO_MCP_TCP=1 je no-op. */
    void imgui_mcp_settings_dialog(void);
};

static void ShowOverlayWindow(void)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    // vzdy nahore (tohle nechceme, protoze pak nefunguji ostatni okna)
    // ImGui::SetNextWindowFocus();

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    if (ImGui::Begin("##OverlayWindow", NULL, flags))
    {
        if (!g_gui->justOpenedOverlay)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_LeftAlt)))
            {
                g_gui->showOverlay = false;
            }
        }

        ImGui::GetWindowDrawList()->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(10, 10, 10, 230));
        ImGui::PopStyleVar(2);

        // Centrum okna pro umístění tlačítek
        ImVec2 windowSize = viewport->Size;
        ImVec2 buttonSize = ImVec2(150, 50);
        ImVec2 center = ImVec2(windowSize.x / 2.0f, windowSize.y / 2.0f);

        ImGui::SetCursorPos(ImVec2(center.x - 80, center.y - 110));
        if (ImGui::Button("Action 1", buttonSize))
        {
            g_print("Selected Action 1\n");
        }

        ImGui::SetCursorPos(ImVec2(center.x - 80, center.y - 50));
        if (ImGui::Button("Action 2", buttonSize))
        {
            g_gui->showOverlay = false;
            g_print("Selected Action 2\n");
        }

        ImGui::SetCursorPos(ImVec2(center.x - 80, center.y + 10));
        if (ImGui::Button("Action 3", buttonSize))
        {
            g_print("Selected Action 3\n");
        }

        ImGui::SetCursorPos(ImVec2(center.x - 80, center.y + 70));
        if (ImGui::Button("Action 4", buttonSize))
        {
            g_print("Selected Action 4\n");
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::End();
    }

    ImGui::PopStyleVar(2);
}

static void ShowFullScreenImageEmulatorWindow(GLuint texture)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    // Vypnutí paddingu a okraje okna!
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    static ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##MainEmulatorWindow", NULL, flags);

// Prozatim nemame nic smysluplneho pro zobrazovani overlay okna
#if 0
    // Detekce ALT + kliknutí levým tlačítkem myši
    if (!g_gui->showOverlay && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_LeftAlt))
    {
        g_gui->showOverlay = true;
        g_gui->justOpenedOverlay = true; // zamezi okamzitemu zavreni okna pri otevreni
    };
#endif

    // Zjistíme, zda má okno, ci jeho potomek focus. Pripadne zda ma focus VKBD okno
    if ((!g_gui->showVirtualKeyboardWindow) && (g_gui->VkbdWindowHasFocus))
    {
        g_gui->VkbdWindowHasFocus = false;
    };
    g_gui->MainWindowHasFocus = (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || g_gui->VkbdWindowHasFocus);

    // obsluha klávesnice pro PIO8255 podle zaznamenanych klavesovych udalosti
    static gboolean pvevious_MainWindowHasFocus = FALSE;
    if (!g_gui->MainWindowHasFocus && pvevious_MainWindowHasFocus)
    {
        g_gui->haveKeyboardEvents = FALSE;
        pio8255_keyboard_matrix_reset();
    };
    pvevious_MainWindowHasFocus = g_gui->MainWindowHasFocus;
    if (g_gui->haveKeyboardEvents)
    {
        int numkeys = 0;
        const bool *keyboard_matrix = SDL_GetKeyboardState(&numkeys);
        SDL_Keymod kmod = SDL_GetModState();
        iface_keyboard_full_scan(keyboard_matrix, kmod, numkeys);
        g_gui->haveKeyboardEvents = FALSE;
    };

    // Detekce pravého kliknutí -> otevření popup menu
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        ImGui::OpenPopup("EmulatorPopupMenu");
    };

    ImGui::Image(texture, ImVec2(viewport->Size.x, viewport->Size.y));
    ImGui::PopStyleVar(2);

    // bool isPopupOpen = ImGui::IsPopupOpen("ContextMenu", ImGuiPopupFlags_AnyPopup);

    if (ImGui::BeginPopup("EmulatorPopupMenu"))
    {
        imgui_topmenu_body();
    };

    // Zjistíme, zda má okno focus (bez popupu)
    // bool hasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !isPopupOpen;

    if (g_gui->MainWindowHasFocus && !g_gui->showOverlay)
    {
        imgui_global_shortcuts();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::End();
    ImGui::PopStyleVar(2);

    // Pokud je overlay aktivní, zobrazíme ho
    if (g_gui->showOverlay)
    {
        ShowOverlayWindow();
        g_gui->justOpenedOverlay = false;
    }
}

void imgui_main_window(GLuint texture)
{
    // hlavni okno + scanovani klavesnice pro pio8255 + overlay
    ShowFullScreenImageEmulatorWindow(texture);

    if (VERSION_CHECK_TEST_THREAD_DONE)
    {
        version_check_parse_thread_response();
    };

    /* About okno se renderuje AŽ za Version Check Setup oknem, aby v
     * first-run scénáři (kdy se obě otevřou současně) bylo vepředu.
     * ImGui z-order = render order, poslední render je nahoře. About
     * je důležitější pro uživatele než version-check setup. Volání bylo
     * původně na začátku UI dispatchu - viz níže po Version Check Setup. */

    imgui_fps_measurement(&g_gui->showVideoIfaceMeasurementWindow);

    if (g_emulator.show_demo_window)
        ImGui::ShowDemoWindow(&g_emulator.show_demo_window);

    if (EMULATOR_MEASURING_TEST_FRAME_TIMING_ENABLED)
        imgui_measuring_frame_timing(EMULATOR_MEASURING_FRAME_TIMING_ENABLED_PTR);

    if (EMULATOR_MEASURING_TEST_GDG_ENABLED)
        imgui_measuring_gdg(EMULATOR_MEASURING_GDG_ENABLED_PTR);

    imgui_maxspeed_bench(&g_gui->showMaxSpeedBenchWindow);

    if (g_gui->showAudioWindow)
        imgui_audio(&g_gui->showAudioWindow);

    if (g_gui->showVirtualCmtWindow)
        imgui_cmt(&g_gui->showVirtualCmtWindow);

    if (g_gui->showCmtFixMzfsizeWindow)
        imgui_cmt_fix_mzfsize_popup(&g_gui->showCmtFixMzfsizeWindow);

    if (g_mzhal.have_fdc) { /* runtime capability, mzhal krok 8 */
    if (g_gui->showFdcStorageSwitchWindow)
        imgui_fdc_storage_switch_popup(&g_gui->showFdcStorageSwitchWindow);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (g_gui->showFdcStateWindow)
        imgui_fdc_state_window(&g_gui->showFdcStateWindow);
#endif
    }

    if (g_mzhal.have_qdisk) { /* runtime capability, mzhal krok 8 */
    /* Faze 3 (qdisk-rewrite): switch storage mode popup pro DISCARD->jiny
     * mod s pending RAM changes. Aktivuje se z menu_qdisk.cpp. */
    if (g_gui->showQdiskStorageSwitchWindow)
        imgui_qdisk_storage_switch_popup(&g_gui->showQdiskStorageSwitchWindow);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Faze 6 (qdisk-rewrite): QDisk State debugger okno - live view chip
     * + drive stavu. Side-effect free, refresh per frame. */
    if (g_gui->showQdiskStateWindow)
        imgui_qdisk_state_window(&g_gui->showQdiskStateWindow);
#endif
    }

    /* mz1p16-plotter: GUI okno plotteru MZ-1P16 - živý náhled kresby +
     * ovládání. Plotter je aktivní jen když je okno otevřené; deaktivaci při
     * zavření [X] detekuje imgui_plotter_window podle hrany *p_open uvnitř,
     * proto ho voláme BEZPODMÍNEČNĚ (i při zavřeném okně - tam je render
     * no-op, ale zpracuje open->close hranu a deaktivuje plotter). */
    imgui_plotter_window(&g_gui->showPlotterWindow);

    if (g_mzhal.have_ramdisk) { /* runtime capability, mzhal krok 8 */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* memory-disk-state: Memory Disk State debugger okno - stav vsech
     * ramdisku (MR-1R18 STD + Pezik E8/68). Side-effect free, per frame. */
    if (g_gui->showRamdiskStateWindow)
        imgui_ramdisk_state_window(&g_gui->showRamdiskStateWindow);
#endif
    }

    if (g_mzhal.have_ide8) { /* runtime capability, mzhal krok 8 */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* memory-disk-state: IDE8 State debugger okno - stav IDE8 radice
     * (Master + Slave). Side-effect free, per frame. */
    if (g_gui->showIde8StateWindow)
        imgui_ide8_state_window(&g_gui->showIde8StateWindow);
#endif
    }

    if (g_gui->showVirtualCmtTapeIndexWindow)
        imgui_cmt_tape_index_window(&g_gui->showVirtualCmtTapeIndexWindow);

    if (g_gui->showVirtualKeyboardWindow)
        imgui_vkbd(&g_gui->showVirtualKeyboardWindow);

    /* Detekce přechodu hidden -> visible pro Version Check Setup a About
     * okna. Při tomto přechodu zavoláme SetNextWindowFocus() PŘED Begin -
     * tím se okno přesune do popředí z-orderu, nad hlavní emulátorové
     * okno (= ShowFullScreenImageEmulatorWindow). Jen `IsWindowAppearing()
     * + SetWindowFocus()` uvnitř Begin nestačí - main window má
     * NoBringToFrontOnFocus a SetWindowFocus aplikuje až další frame.
     *
     * Universal pro all callers (first-run + menu otevření). */
    static bool s_was_vcheck_open = false;
    if (g_gui->showVersionCheckSetupWindow && !s_was_vcheck_open)
        ImGui::SetNextWindowFocus();
    s_was_vcheck_open = g_gui->showVersionCheckSetupWindow;

    if (g_gui->showVersionCheckSetupWindow)
        imgui_version_check_setup_window(&g_gui->showVersionCheckSetupWindow);

    if (g_gui->showVersionCheckResultWindow)
        imgui_version_check_result_window(&g_gui->showVersionCheckResultWindow);

    /* About okno - renderováno NAKONEC + s detekcí transition hidden ->
     * visible pro SetNextWindowFocus. */
    static bool s_was_about_open = false;
    if (g_gui->showAboutWindow && !s_was_about_open)
        ImGui::SetNextWindowFocus();
    s_was_about_open = g_gui->showAboutWindow;

    if (g_gui->showAboutWindow)
        imgui_ShowAboutWindow(&g_gui->showAboutWindow);

    /* Snapshot dialogy a Quick Save/Load */
    imgui_snapshot_save_dialog();
    imgui_snapshot_load_dialog();
    imgui_snapshot_setup_dialog();
    imgui_snapshot_quicksave_handler();
    imgui_snapshot_notification();

    /* Jazyk */
    imgui_language_window();

    /* MCP server settings (V0.B.4) - okno se kreslí pouze pokud
     * g_gui->showMcpSettingsWindow == true; při buildu s NO_MCP_TCP=1
     * je funkce no-op. */
    imgui_mcp_settings_dialog();

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* MCP Activity okno (V1.C.2 mutant mcp-server). Real-time GUI log
     * MCP akcí (= DBGAPI_MSG_MCP_ACTION broadcast). Init/shutdown řeší
     * bootstrap UI vrstva; zde jen render při zobrazeném flagu. */
    if (g_gui->showMcpActivityWindow)
        mcp_activity_window_render(&g_gui->showMcpActivityWindow);
    /* V1.C.3 - action toast pro destruktivní MCP akce. Render vždy
     * (= queue obsahuje jen platné toasty, prázdná queue je no-op). */
    action_toast_render();
#endif

    /* PEZIK nastavení */
    if (g_gui->showPezikSettingsWindow)
        imgui_pezik_settings_window(&g_gui->showPezikSettingsWindow);

    /* ROM nastavení */
    if (g_gui->showRomSettingsWindow)
        imgui_rom_settings_window(&g_gui->showRomSettingsWindow);

    /* Autotype buffer */
    if (g_gui->showAutotypeWindow)
        imgui_autotype_window(&g_gui->showAutotypeWindow);

    /* MemExt obsah — Load/Save dialogy */
    imgui_memext_load_dialog();
    imgui_memext_save_dialog();

    /* MemExt Map Settings */
    if (g_gui->showMemextMapWindow)
        imgui_memext_map_window(&g_gui->showMemextMapWindow);

    /* DSK Create */
    if (g_gui->showDskCreateWindow)
        imgui_dsk_create_window(&g_gui->showDskCreateWindow);

    /* Joystick Setup */
    if (g_gui->showJoystickSetupWindow)
        imgui_joy_setup_window(&g_gui->showJoystickSetupWindow);

    /* Debugger */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (g_gui->showDebuggerWindow)
        imgui_debugger_window(&g_gui->showDebuggerWindow);

    /* Breakpoints */
    if (g_gui->showBreakpointsWindow)
        imgui_breakpoints_window(&g_gui->showBreakpointsWindow);

    /* $vars panel pro smart BP user-defined proměnné (D.6.2) */
    if (g_gui->showVarsWindow)
        vars_window_render(&g_gui->showVarsWindow);

    /* Watch panel - user-defined paměťové hlídky (V1 Phase A) */
    if (g_gui->showWatchWindow)
        watch_window_render(&g_gui->showWatchWindow);

    /* I/O Ports viewer panel (D.7) */
    if (g_gui->showIoWindow)
        io_window_render(&g_gui->showIoWindow);

    /* Symbol Browser panel (D.8.6) */
    if (g_gui->showSymbolsWindow)
        sym_window_render(&g_gui->showSymbolsWindow);

    /* Bookmarks panel - pojmenované adresové záložky */
    if (g_gui->showBookmarksWindow)
        bm_window_render(&g_gui->showBookmarksWindow);

    /* Memory Map debug okno - banking + memext per 4 kB stránku */
    if (g_gui->showMemoryMapWindow)
        memmap_window_render(&g_gui->showMemoryMapWindow);

    /* Memory Browser - hex view paměti přes dbgapi_regions (V0 hex MVP) */
    if (g_gui->showMemoryBrowserWindow)
        membrowser_window_render(&g_gui->showMemoryBrowserWindow);

    /* Disassembler V1 - samostatné range-based disasm okno (mutant). */
    dasm_window_render();

    /* V3 multi-view: 4 sekundární Memory Browser okna (#2 - #5). Render
     * funkce sama přeskočí zavřené sloty + lazy-create/destroy instance
     * podle g_gui->showMemoryBrowserWindowExtra[] flagů. */
    membrowser_window_render_all_secondary();

    /* V5: Memory Diff - samostatné okno se side-by-side hex porovnáním
     * dvou snapshotů. Singleton, lazy state init při prvním renderu. */
    if (g_gui->showMemoryDiffWindow)
        membrowser_diff_window_render(&g_gui->showMemoryDiffWindow);

    /* V6: PCG glyph editor - samostatné okno (MZ-1500 only). */
    if (g_gui->showMembrowserPcgEditor)
        membrowser_pcg_window_render(&g_gui->showMembrowserPcgEditor);

    /* V1.5+ ASCII edit follow-up: Char Inserter okno - paleta znaků pro
     * vkládání speciálních znaků (SharpMZ EU/JP, KOI8-CS) do Memory
     * Browseru. Show flag se interně testuje ve funkci. */
    membrowser_char_inserter_render();

    /* CPU Registers - Variant B samostatné plovoucí okno */
    if (g_gui->showCpuWindow)
        cpu_window_render(&g_gui->showCpuWindow);

    /* Stack Monitor - samostatné plovoucí okno (hex dump kolem SP) */
    if (g_gui->showStackWindow)
        stack_window_render(&g_gui->showStackWindow);

    /* V8: Stack Regions - samostatné okno s tabulkou stack regionů
     * (1 řádek per region). Otevírá se přes menu Debugger -> Stack
     * Regions nebo Alt+Shift+S. */
    if (g_gui->showStackRegionsWindow)
        stack_regions_window_render(&g_gui->showStackRegionsWindow);

    /* V9: Stack History - samostatné okno se SP history sparkline (plot
     * resize s velikostí okna). Otevírá se přes menu Debugger -> Stack
     * History nebo Alt+Shift+H. */
    if (g_gui->showStackHistoryWindow)
        stack_history_window_render(&g_gui->showStackHistoryWindow);

    /* Callstack (mutant callstack, Fáze 2C) - samostatné plovoucí okno
     * se shadow stackem volání (CALL/RST/IRQ/NMI). Toggle přes menu
     * Debugger -> Callstack. */
    if (g_gui->showCallstackWindow)
        callstack_window_render(&g_gui->showCallstackWindow);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* CPU Profiler (mutant profiler V1) - samostatné plovoucí okno
     * s per-function CPU profilací (Excl/Incl cycles, Calls, Min/Max).
     * Toggle přes Debug menu nebo Alt+Shift+P shortcut. Persistence
     * stavu okna mezi sezeními: INI [PROFILER] show_window. */
    if (g_gui->showProfilerWindow)
        profiler_window_render(&g_gui->showProfilerWindow);

    /* Per-chip-panels: per-chip detail okna. CTC a PPI jsou ve všech 3
     * archech, Z80 PIO + PSG jen v MZ-800 / MZ-1500. Toggle přes menu
     * Debugger -> CTC/PPI/PIO/PSG State nebo Alt+Shift+C/I/Z/G. */
    if (g_gui->showCtcStateWindow)
        imgui_ctc_state_window(&g_gui->showCtcStateWindow);
    if (g_gui->showPpiStateWindow)
        imgui_ppi_state_window(&g_gui->showPpiStateWindow);
    if (g_mzhal.have_pioz80) { /* runtime capability, mzhal krok 8 */
    if (g_gui->showPiozStateWindow)
        imgui_pioz80_state_window(&g_gui->showPiozStateWindow);
    }
    if (g_mzhal.psg_count >= 1) { /* runtime capability, mzhal krok 8 */
    if (g_gui->showPsgStateWindow)
        imgui_psg_state_window(&g_gui->showPsgStateWindow);
    /* psg-audio-scope mutant F1: tick callback (= snapshot do ring bufferu)
     * je nepodmíněný - ring buffer drží 10 s historie i pokud okno není
     * otevřené, takže po otevření je rovnou plný. Vlastní render jen pokud
     * je okno viditelné. */
    psg_audio_scope_tick();
    if (g_gui->showPsgAudioScopeWindow)
        imgui_psg_audio_scope_window(&g_gui->showPsgAudioScopeWindow);
    }

    /* gdg-panel F1 scaffold: GDG state inspector (per-arch render shell).
     * GDG je ve všech 3 archech, žádný HAVE_* guard nepotřeba - per-arch
     * dispatch je uvnitř gdg_window.cpp přes #if MZARCH ==. */
    if (g_gui->showGdgStateWindow)
        imgui_gdg_state_window(&g_gui->showGdgStateWindow);
#endif

    /* Events - eventlog ring viewer (Vlna 1 = Log tab + Strip placeholder).
     * Lifecycle: open / close edge volá eventlog_notify_window_open(),
     * což v módu WHEN_WINDOW_OPEN spouští / zastavuje ring recording.
     * Render volá funkci bezpodmínečně přes flag (= eventlog open transition
     * je závislý na změně tohoto flagu mezi framy, ne na podmínce čtení). */
    event_viewer_window_render(&g_gui->showEventViewerWindow);

    /*
     * Sekundární Disassembly okna (#2 - #5). Render bezpodmínečný -
     * funkce uvnitř iteruje g_gui->showDisasmExtraWindow[] flagy a
     * renderuje jen otevřená okna; zavřená rovnou destrukci instance.
     */
    dbg_extra_disasm_render_all();

    /*
     * Modální dialogy debuggeru renderované na úrovni hlavního okna,
     * aby fungovaly i bez otevřeného hlavního debug okna (sekundární
     * disasm okna #2 - #5 mohou otevřít context-menu položky "Edit row"
     * a "Focus To..." nezávisle na stavu hlavního okna).
     *
     * BeginPopupModal uvnitř těchto funkcí se aktivuje až po OpenPopup
     * (volaný z context menu kterékoliv disasm instance).
     */
    dbg_iasm_render();
    dbg_focus_to_render();

/* SENTINEL: Memory Heatmap platí pro všechny známé architektury
 * (garantováno mzhal.c) - při přidání nové architektury PROVĚŘ. */
    /* Memory Heatmap (CDL) */
    if (g_gui->showMemoryHeatmapWindow)
        mhmap_window_render(&g_gui->showMemoryHeatmapWindow);
#endif

    imgui_file_chooser_window();
    imgui_message_window();
    imgui_unicard_runtime_mismatch_dialog();
    imgui_unicard_reinit_choice_dialog();
}