/* myimgui.h - definitions for My ImGui */
#ifndef MYIMGUI_H
#define MYIMGUI_H

#include <glib.h>
#include "libs/sdlapp/sdlapp.h"

typedef struct FRGBA
{
    float r;
    float g;
    float b;
    float a;
} FRGBA;

typedef void (*MyImGuiEventCb)(SdlAppWindow *win, SDL_Event *event, gpointer user_data);
typedef void (*MyImGuiRenderCb)(SdlAppWindow *win, gpointer user_data);

typedef struct MyImGui
{
    SdlApp *app;
    SdlAppWindow *win;
    gpointer ctxptr;

    gboolean use_bg_color;
    FRGBA bg;

    MyImGuiEventCb event_cb;
    MyImGuiRenderCb render_cb;

    // flagy pro zpracovani klavesovych udalosti pro pio8255
    bool MainWindowHasFocus;
    bool VkbdWindowHasFocus;
    bool haveKeyboardEvents;

    // Overlay
    bool showOverlay;
    bool justOpenedOverlay;

    // Dalsi okna
    bool showVersionCheckSetupWindow;
    bool showVersionCheckResultWindow;
    bool showAboutWindow;
    bool showVideoIfaceMeasurementWindow;
    bool showMaxSpeedBenchWindow;        /**< MAX SPEED benchmark okno. */
    bool showAudioWindow;
    bool showVirtualCmtWindow;
    bool showCmtFixMzfsizeWindow;
    bool showVirtualCmtTapeIndexWindow;
    bool showVirtualKeyboardWindow;
    bool showSnapshotSetupWindow;
    bool showVideorecSetupWindow;        /**< Dialog nastavení video záznamu (Tools -> Video Recording -> Settings...). */
    bool showVideorecRemoteWindow;       /**< Okno dálkového ovládání nahrávání (Tools -> Video Recording -> Remote Control..., Alt+Shift+L); persistence [VIDEOREC_UI] remote_window. */
    bool showLanguageWindow;
    bool showPezikSettingsWindow;
    bool showRomSettingsWindow;
    bool showAutotypeWindow;
    bool showMemextMapWindow;
    bool showDskCreateWindow;
    bool showFdcStorageSwitchWindow;
    bool showFdcStateWindow;
    bool showQdiskStorageSwitchWindow;   /**< QD: switch storage mode popup (Faze 3) */
    bool showQdiskStateWindow;           /**< QD: debugger state inspector okno (Faze 6) */
    bool showPlotterWindow;              /**< Plotter MZ-1P16: živý náhled kresby + ovládání (mz1p16-plotter) */
    bool showRamdiskStateWindow;         /**< Memory Disk State: stav vsech ramdisku (STD + Pezik E8/68) */
    bool showIde8StateWindow;            /**< IDE8 State: stav IDE8 radice (Master + Slave) */
    bool showCtcStateWindow;             /**< Per-chip-panels: CTC 8253 state inspector (F1 scaffold) */
    bool showPpiStateWindow;             /**< Per-chip-panels: PPI 8255 state inspector (F1 scaffold) */
    bool showPiozStateWindow;            /**< Per-chip-panels: Z80 PIO state inspector (F1 scaffold) */
    bool showPsgStateWindow;             /**< Per-chip-panels: PSG SN76489 state inspector (F1 scaffold) */
    bool showGdgStateWindow;             /**< gdg-panel: GDG video LSI state inspector (F1 scaffold) */
    bool showPsgAudioScopeWindow;        /**< PSG Audio Scope: dynamická audio analýza (psg-audio-scope F1) */
    bool showJoystickSetupWindow;
    bool showDebuggerWindow;
    bool showBreakpointsWindow;
    bool showMemoryMapWindow;       /**< Memory Map debug okno - banking + memext per 4 kB stránku */
    bool showMemoryHeatmapWindow;
    bool showVarsWindow;            /**< $vars panel pro smart BP user vars (D.6.2) */
    bool showWatchWindow;           /**< Watch panel - user-defined paměťové hlídky (V1 Phase A) */
    bool showMemoryBrowserWindow;   /**< Memory Browser - hex view paměti přes dbgapi_regions (V0 hex MVP) */
    /**
     * V3 multi-view: 4 sekundární Memory Browser okna (#2 - #5). Každé
     * okno je nezávislá instance st_MEMBROWSER_STATE + per-instance
     * persist sekce [MEMBROWSER_WINDOW_2..5]. Default false - uživatel
     * je otevírá z menu Debugger - Memory Browser #N. Sdílí pouze
     * globální freeze subsystém s hlavním oknem (= úmyslný cross-talk
     * pro cheat-engine semantiku). Index: 0 = #2, 1 = #3, 2 = #4, 3 = #5.
     */
    bool showMemoryBrowserWindowExtra[4];
    /**
     * V5: Memory Diff okno - side-by-side hex porovnání dvou snapshotů
     * (live / manual / auto @ pause). Singleton, žádné multi-instance.
     * Default false. Otevírá se z menu Debugger - Memory Diff.
     */
    bool showMemoryDiffWindow;
    /**
     * V6: PCG glyph editor (MZ-1500). 8x8 bitmap editor pro 1024 chars
     * v každé z 3 PCG bank. Otevírá se jen z context menu nad PCG_1500
     * regionem v Memory Browseru; na jiných archech region neexistuje,
     * takže se okno otevřít nedá (render by zobrazil "region not
     * available" hlášku).
     */
    bool showMembrowserPcgEditor;
    /**
     * V1.5+: Char Inserter okno - paleta znaků pro vkládání do Memory
     * Browseru. Tabbed (SharpMZ EU / JP / KOI8-CS), 16x16 grid bytů.
     * Otevírá se z context menu hex view ("Insert character...") nebo
     * z menu Debugger. Default false.
     */
    bool showMembrowserCharInserter;
    bool showIoWindow;              /**< I/O Ports viewer panel (D.7) */
    bool showSymbolsWindow;         /**< Symbol Browser panel (D.8.6) */
    bool showBookmarksWindow;       /**< Bookmarks panel (pojmenované adresové záložky) */
    bool showCpuWindow;             /**< CPU Registers - Variant B samostatné plovoucí okno */
    bool showStackWindow;           /**< Stack Monitor - samostatné plovoucí okno (hex dump kolem SP) */
    bool showStackRegionsWindow;    /**< V8: Stack Regions - samostatné okno s tabulkou regionů (1 řádek per region) */
    bool showStackHistoryWindow;    /**< V9: Stack History - samostatné okno s SP history sparkline (plot resize s velikostí okna) */
    bool showCallstackWindow;       /**< Callstack - samostatné plovoucí okno se shadow stackem (CALL/RET/IRQ/NMI frames) */
    bool showProfilerWindow;        /**< CPU Profiler - per-function CPU profilace (mutant profiler V1) */
    bool showEventViewerWindow;     /**< Events - real-time pohled na eventlog ring (Log + Strip tab) */
    /**
     * Sekundární Disassembly okna (#2-#5) - 4 nezávislé instance
     * DisassembledView dostupné z menu Debugger -> Další disassembly.
     * Každé okno má vlastní focus_addr, selected_row, scroll, atd.
     * Sdílí pouze paměť, symboly, breakpointy. Layout perzistuje přes
     * imgui.ini, focus_addr přes cfgmain (klíče debugger.disasm_extra*).
     * Index: 0 = #2, 1 = #3, 2 = #4, 3 = #5.
     */
    bool showDisasmExtraWindow[4];
    /**
     * Disassembler V1 - samostatné range-based disassembler okno
     * (mutant disassembler-window-v1). Uživatel zadá rozsah adres
     * From/To, vidí read-only listing s auto-labely (S/L/D/W) a
     * volitelně doplněnými symboly. Default false, otevírá se z
     * iconbar tlačítka DASM, top-menu Debugger nebo Alt+Shift+D
     * shortcut. Window state per ImGui ini.
     */
    bool showDisassemblerWindow;
    /**
     * MCP Server Settings dialog (V0.B.4). Konfigurační okno pro
     * INI sekci [MCP] - TCP port, bind adresa, security profile,
     * auto-start flag. Default false, otevírá se z menu
     * Tools -> MCP TCP Server -> Settings... Persistuje pouze
     * po explicitním Save (volá cfgroot_save). Při buildu s
     * NO_MCP_TCP=1 zůstává flag false a okno se nikdy nerenderuje.
     */
    bool showMcpSettingsWindow;
    /**
     * MCP Activity okno (V1.C.2 mutant mcp-server). Real-time log MCP
     * akcí (= DBGAPI_MSG_MCP_ACTION broadcast z V-1.3). Per-session,
     * NEpersistovaný v INI; default false, otevírá se z menu Debugger.
     * Při buildu s MZ800EMU_NO_MCP zůstává flag false a renderer je
     * no-op (= window inicializace přes #ifdef guard). Při buildu bez
     * debuggeru (= MZ800EMU_NO_DEBUGGER) MSG broadcast neexistuje,
     * okno se sice vykreslí, ale ring buffer zůstane prázdný.
     */
    bool showMcpActivityWindow;
} MyImGui;

extern MyImGui *g_gui;

#ifdef __cplusplus
extern "C"
{
#endif
    gboolean myimgui_init_cb(SdlAppWindow *win, gpointer user_data);
    void myimgui_render_cb(SdlAppWindow *win, gpointer user_data);
    void myimgui_event_cb(SdlAppWindow *win, SDL_Event *event, gpointer user_data);
    void myimgui_destroy_cb(SdlAppWindow *win, gpointer user_data);

    void myimgui_set_bg_clean(MyImGui *gui, bool enabled);
    gboolean myimgui_get_bg_clean(MyImGui *gui);

    void myimgui_set_bg_color(MyImGui *gui, float r, float g, float b, float a);
    void myimgui_get_bg_color(MyImGui *gui, float *r, float *g, float *b, float *a);

    MyImGuiEventCb myimgui_get_event_cb(MyImGui *gui);
    void myimgui_set_event_cb(MyImGui *gui, MyImGuiEventCb event_cb);

    MyImGuiRenderCb myimgui_get_render_cb(MyImGui *gui);
    void myimgui_set_render_cb(MyImGui *gui, MyImGuiRenderCb render_cb);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
struct ImFont;
/**
 * @brief Vrátí ukazatel na monospace font načtený v myimgui_set_fonts().
 *
 * Monospace font (Cousine-Regular.ttf, 28 px) slouží pro hex dumpy
 * (Memory Browser, případně Disassembler) - bajty pak mají fixed šířku
 * a sloupce hezky lícují. Font má merge mzglyphs (E000/E100-E4FF) takže
 * CG-ROM DISPLAY glyfy fungují i v monospace módu.
 *
 * @return Ukazatel na ImFont; NULL pokud TTF soubor chybí (caller pak
 *         musí použít default font - PushFont(nullptr)). Pokud ImGui
 *         init neproběhl, vrátí NULL.
 *
 * @note Volat pouze z UI vlákna (ImGui kontext aktivní).
 */
ImFont *myimgui_get_monospace_font(void);
#endif

#endif /* MYIMGUI_H */
