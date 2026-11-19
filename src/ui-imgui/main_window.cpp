#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstring>

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/plotter/plotter_window.h"
#include "iface/iface_keyboard.h"
#include "hw-generic/pio8255/pio8255.h"
#include "imgui_windows.h"
#include "ui-imgui/cmt/cmt_window.h"
#include "ui-imgui/cmt/cmt_check_window.h"
#include "ui-imgui/main_window_focus.h"
#include "ui-imgui/overlay/overlay.h"
#include "emulator/emulator.h"
#include "emulator/replay.h"

#include "mzarch/mzarch_config.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "ui-imgui/debugger/sections/dbg_extra_disasm.h"
#include "ui-imgui/debugger/sections/dbg_focus_to.h"
#include "ui-imgui/debugger/sections/dbg_inline_asm.h"
#include "ui-imgui/debugger/profiler/profiler_window.h"
#include "ui-imgui/debugger/ramlog_window/ramlog_window.h"
#include "ui-imgui/debugger/memsearch_window/memsearch_window.h"
/* Per-chip-panels F1 scaffold: per-chip detail okna pro CTC/PPI/PIO/PSG. */
#include "ui-imgui/debugger/ctc_window.h"
#include "ui-imgui/debugger/ppi_window.h"
#if HAVE_PIOZ80
#include "ui-imgui/debugger/pioz80_window.h"
#endif
#if HAVE_PSG >= 1
#include "ui-imgui/debugger/psg_window.h"
#include "ui-imgui/debugger/psg_audio_scope_window.h"
#endif
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
#include "mzarch/mzarch_config.h"
#include "emulator/emulator_measuring.h"
#include "version_check/version_check.h"
#include "message/message_window.h"
#include "ui-imgui/videorec/videorec_menu.h"
#include "ui-imgui/screenshot/screenshot_ui.h"
#include "ui-imgui/scanlines/scanlines_overlay.h"
#include "ui-imgui/audiocap/audio_capture_ui.h"
#include "iface/iface_audio.h"
#include "device_request.h"
#if MZARCH == 700
#include "ui-imgui/mz700/devbus/menu_devbus.h"
#endif
#include "ui-imgui/debugger/busmap/bus_window.h"

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
    void snapshot_notification_show(const char *message, bool is_error);
    bool snapshot_notification_error_active(void);
    void imgui_snapshot_quicksave_handler(void);
    void imgui_snapshot_request_results(void);

    /* Zásahy do zařízení přes emu vlákno (device_request_ui.h) */
    void imgui_device_request_results(void);

    /* Jazyk */
    void imgui_language_window(void);

    /* MCP server settings (V0.B.4). Pri NO_MCP_TCP=1 je no-op. */
    void imgui_mcp_settings_dialog(void);
};

/**
 * @brief Jednorázové upozornění, že emulátor běží bez audio zařízení.
 *
 * Pokud iface_audio_lowlevel_init() nedokázala inicializovat SDL audio nebo
 * otevřít výstupní zařízení (g_iface_audio.device_open_failed), emulace běží
 * bez zvuku s tempem podle systémových hodin. Při prvním volání s nastaveným
 * příznakem zobrazí toast přes snapshot_notification_show(); další volání
 * nedělají nic (je-li právě otevřené chybové okno, upozornění zůstane čekající
 * a zobrazí se v pozdějším snímku; za běhu se příznak nemění, upozornění se tak ukáže jen jednou
 * za běh aplikace).
 *
 * @par Vlákna Jen UI vlákno (příznak se zapisuje před startem emu vlákna).
 * @post Upozornění bylo zobrazeno nejvýše jednou.
 */
static void imgui_audio_device_notice(void)
{
    static bool s_shown = false;
    if (s_shown || !g_iface_audio.device_open_failed)
        return;
    /* Aktivní chybové okno nepřepisovat; upozornění zůstává čekající a ukáže se v pozdějším snímku. */
    if (snapshot_notification_error_active())
        return;
    s_shown = true;
    snapshot_notification_show(_("No audio device - running without sound"), false);
}

/**
 * @brief Hlavní okno emulátoru: obraz přes celý hlavní viewport, klávesnice stroje, kontextové menu, zkratky.
 *
 * Vykreslí texturu obrazu a indikátory nad ní, vyhodnotí, zda emulovaná
 * klávesnice dostává klávesy (main_window_keyboard_enabled()), při ztrátě
 * tohoto stavu pošle klidovou matici a při čekajících událostech kláves
 * naskenuje klávesnici hostitele do matice stroje. Pravé tlačítko myši
 * (nebo žádost overlay menu, imgui_overlay_take_classic_menu_request())
 * otevře klasické menu, levý Alt + levé tlačítko overlay menu
 * (main_window_overlay_open_click()), stejně jako žádost položky
 * "Overlay Menu..." klasického menu z minulého snímku
 * (imgui_overlay_take_open_request()); při fokusu a zavřeném overlayi
 * otevře overlay menu klávesa F10 bez modifikátorů
 * (main_window_overlay_open_key()) a obslouží se globální zkratky
 * (imgui_global_shortcuts()). F10 se obsluhuje zde, ne v globálních
 * zkratkách, aby overlay neotevírala při fokusu okna debuggeru.
 *
 * Joystick stroje je po dobu otevření overlay menu a po zavření do klidu
 * hostitelského joysticku zadržený (imgui_overlay_blocks_joystick()).
 *
 * Dokud overlay menu klávesy zadržuje (imgui_overlay_blocks_keyboard():
 * je otevřené, nebo se zavřelo a klávesa je ještě stisknutá), klávesy do
 * matice stroje nejdou vůbec a čekající události kláves se zahazují. Když
 * se při otevřeném overlayi fokus vrátí do hlavního okna (např. po zavření
 * dialogu) a není otevřený popup, předá ho zpět overlayi; po zavření
 * overlaye si fokus vezme zpět hlavní okno.
 *
 * Přepínání oken ImGui klávesnicí (Ctrl+Tab) je zapnuté, jen když klávesy
 * nepatří stroji ani overlay menu (main_window_nav_windowing_allowed()):
 * při fokusu obrazu nebo virtuální klávesnice jdou Ctrl i Tab do stroje
 * a fokus zůstává, kde je.
 *
 * @param texture Textura obrazu emulátoru.
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render().
 */
static void ShowFullScreenImageEmulatorWindow(GLuint texture)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    // Vypnutí paddingu a okraje okna!
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    /* Overlay menu se právě zavřelo: fokus zpět obrazu (otevírá se z něj),
     * ne oknu nástroje, které je v pořadí fokusu výš. Otevřený popup se
     * nechává být (fokus hlavního okna by ho zavřel). */
    if (imgui_overlay_take_main_focus_request() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        ImGui::SetNextWindowFocus();
    };

    /* příznaky v main_window_focus.h (sdílí je test pravidla fokusu) */
    ImGui::Begin("##MainEmulatorWindow", NULL, MAIN_WINDOW_FLAGS);

    /* Levý Alt + levé tlačítko myši nad obrazem otevře overlay menu (cesta myší vedle F10); pravý Alt (AltGr) ne */
    if (!imgui_overlay_is_open() && ImGui::IsWindowHovered() && main_window_overlay_open_click())
    {
        imgui_overlay_open();
    };

    /* Položka "Overlay Menu..." klasického menu: žádost z minulého snímku, popup menu už je zavřený
     * (jinak se žádost zahodí, aby popup nezůstal pod overlayem) */
    if (imgui_overlay_take_open_request())
    {
        imgui_overlay_open();
    };

    /* Stav overlaye pro celý zbytek snímku hlavního okna: otevření zkratkou
     * F10 níže se projeví až v příštím snímku (klidovou matici pošle už
     * imgui_overlay_open()). */
    const bool overlay_open = imgui_overlay_is_open();
    /* otevřený overlay, nebo po zavření ještě držená klávesa (F10, Esc) */
    const bool overlay_blocks = imgui_overlay_blocks_keyboard();
    /* joystick stroje: po zavření overlaye ho uvolní až klid hostitelského joysticku (volat každý snímek) */
    (void)imgui_overlay_blocks_joystick();

    // Zjistíme, zda má okno, ci jeho potomek focus. Pripadne zda ma focus VKBD okno
    if ((!g_gui->showVirtualKeyboardWindow) && (g_gui->VkbdWindowHasFocus))
    {
        g_gui->VkbdWindowHasFocus = false;
    };
    /* pravidlo v main_window_focus.h (sdílí ho testy oken nástrojů a overlaye) */
    g_gui->MainWindowHasFocus = main_window_keyboard_enabled(g_gui->VkbdWindowHasFocus, overlay_blocks);
    /* Ctrl+Tab nesmí odnést fokus z obrazu do okna nástroje (Ctrl i Tab jsou
     * klávesy stroje); při fokusu okna nástroje přepínač oken ImGui funguje */
    main_window_nav_windowing_set(main_window_nav_windowing_allowed(g_gui->VkbdWindowHasFocus, overlay_blocks));

    /* Otevřený overlay a fokus zpět v hlavním okně (zavřel se dialog):
     * vrátit fokus overlayi, jinak by jeho klávesy nefungovaly. Otevřený
     * popup se nechává být (fokus overlaye by ho zavřel). */
    if (overlay_open && main_window_keyboard_focus(false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        imgui_overlay_request_focus();
    };

    // obsluha klávesnice pro PIO8255 podle zaznamenanych klavesovych udalosti
    static gboolean pvevious_MainWindowHasFocus = FALSE;
    if (!g_gui->MainWindowHasFocus && pvevious_MainWindowHasFocus)
    {
        g_gui->haveKeyboardEvents = FALSE;
        /* Ztráta fokusu = žádná klávesa; přes záznam vstupů (replay.h),
         * při záznamu se klidová matice převezme na hranici snímku. */
        main_window_keyboard_release();
    };
    pvevious_MainWindowHasFocus = g_gui->MainWindowHasFocus;
    /* Dokud overlay klávesy zadržuje, do matice se neskenují, ani když má
     * fokus okno virtuální klávesnice (obsluha událostí SDL by příznak
     * nastavila). */
    if (overlay_blocks)
    {
        g_gui->haveKeyboardEvents = FALSE;
    };
    if (g_gui->haveKeyboardEvents)
    {
        int numkeys = 0;
        const bool *keyboard_matrix = SDL_GetKeyboardState(&numkeys);
        SDL_Keymod kmod = SDL_GetModState();
        iface_keyboard_full_scan(keyboard_matrix, kmod, numkeys);
        g_gui->haveKeyboardEvents = FALSE;
    };

    // Detekce pravého kliknutí -> otevření popup menu; totéž na žádost overlay menu (tlačítko "Open classic menu").
    // Žádost overlaye platí jen při volnu (imgui_modal_can_open() uvnitř vyzvednutí), jinak se zahodí.
    const bool classic_menu_request = imgui_overlay_take_classic_menu_request();
    if (classic_menu_request || (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
    {
        ImGui::OpenPopup("EmulatorPopupMenu");
    };

    ImGui::Image(texture, ImVec2(viewport->Size.x, viewport->Size.y));
    /* Scanlines CRT přes obraz (jen zobrazení; indikátory níže i ostatní okna zůstávají nad nimi) */
    imgui_scanlines_overlay();
    /* REC indikátor v pravém horním rohu obrazu (jen UI, do nahrávky se nedostane) */
    imgui_videorec_overlay();
    /* Indikátor série snímků v levém horním rohu obrazu (jen UI) */
    imgui_screenshot_series_overlay();
    /* Indikátor zachycení zvuku (pod indikátorem série, jen UI) */
    imgui_audio_capture_overlay();
    /* Indikátor záznamu / přehrání vstupů (levý dolní roh, jen UI) */
    imgui_replay_overlay();
    ImGui::PopStyleVar(2);

    // bool isPopupOpen = ImGui::IsPopupOpen("ContextMenu", ImGuiPopupFlags_AnyPopup);

    if (ImGui::BeginPopup("EmulatorPopupMenu"))
    {
        imgui_topmenu_body();
    };

    // Zjistíme, zda má okno focus (bez popupu)
    // bool hasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !isPopupOpen;

    /* MainWindowHasFocus je vždy false, dokud overlay klávesy zadržuje (main_window_keyboard_enabled()) */
    if (g_gui->MainWindowHasFocus)
    {
        /* F10 bez modifikátorů otevře overlay menu jen odsud (ne z okna
         * debuggeru, které globální zkratky volá při svém fokusu) */
        if (main_window_overlay_open_key())
        {
            imgui_overlay_open();
        };
        imgui_global_shortcuts();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void imgui_main_window(GLuint texture)
{
    // hlavní okno + scanování klávesnice pro pio8255
    ShowFullScreenImageEmulatorWindow(texture);

    /* Overlay menu nad obrazem (zavřené = jen test příznaku) */
    imgui_overlay_frame();

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

#if MZARCH == 700
    /* Okna volitelných zařízení devbus (zámek stavu zařízení bere sama). */
    imgui_devbus_windows();
#endif

    /* Okno kazety plní kopii stavu z pásky (jméno, kontejner, bloky), kterou
     * otevření a vysunutí v emu vlákně uvolňuje; zásah mezitím neprovede.
     * Viditelnost: --cmt, Alt+C, menu HW & Devices > CMT > Show Virtual CMT. */
    device_state_lock();
    if (g_gui->showVirtualCmtWindow)
    {
        imgui_cmt_window(&g_gui->showVirtualCmtWindow);
    };
    /* nemodální okno Tape check (otevírá ho menu Tape okna kazety; nálezy čte jen při otevření a Check again) */
    imgui_cmt_check_window();
    /* dotaz na neuložené změny pásky, oznámení výsledku nahrávání */
    imgui_cmt_tape_dialogs();
    device_state_unlock();
    /* akce zvolená v dialogu nebo v okně kazety: až bez zámku (může
     * synchronně čekat na emu vlákno, které si zámek bere pro MCP) */
    imgui_cmt_tape_run_deferred();

    /* Modál Custom value... volby Device for Direct MZF Start (menu
     * HW & Devices > CMT > CMT Options): položka nabídky ho jen vyžádá,
     * nabídku klik zavře; nastavení má vlastní zámek modulu. */
    imgui_menu_cmt_bootdev_popup();

    /* Popup kontroly velikosti MZF má vlastní zámek (aktivuje ho i CMT
     * hack uvnitř instrukce, bez zámku stavu zařízení). */
    if (g_gui->showCmtFixMzfsizeWindow)
        imgui_cmt_fix_mzfsize_popup(&g_gui->showCmtFixMzfsizeWindow);

#if CFG_HWEXT_HAVE_FDC
    if (g_gui->showFdcStorageSwitchWindow)
    {
        /* Popup čte stav mechaniky; zásah mezitím neprovede emu vlákno. */
        device_state_lock();
        imgui_fdc_storage_switch_popup(&g_gui->showFdcStorageSwitchWindow);
        device_state_unlock();
    }

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (g_gui->showFdcStateWindow)
        imgui_fdc_state_window(&g_gui->showFdcStateWindow);
#endif
#endif

#if CFG_HWEXT_HAVE_QDISK
    /* Faze 3 (qdisk-rewrite): switch storage mode popup pro DISCARD->jiny
     * mod s pending RAM changes. Aktivuje se z menu_qdisk.cpp. */
    if (g_gui->showQdiskStorageSwitchWindow)
    {
        /* Popup čte stav QD; zásah mezitím neprovede emu vlákno. */
        device_state_lock();
        imgui_qdisk_storage_switch_popup(&g_gui->showQdiskStorageSwitchWindow);
        device_state_unlock();
    }

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Faze 6 (qdisk-rewrite): QDisk State debugger okno - live view chip
     * + drive stavu. Side-effect free, refresh per frame. */
    if (g_gui->showQdiskStateWindow)
        imgui_qdisk_state_window(&g_gui->showQdiskStateWindow);
#endif
#endif

#if (MZARCH == 800) || (MZARCH == 1500)
    /* mz1p16-plotter: GUI okno plotteru MZ-1P16 - živý náhled kresby +
     * ovládání. Plotter je aktivní jen když je okno otevřené; deaktivaci při
     * zavření [X] detekuje imgui_plotter_window podle hrany *p_open uvnitř,
     * proto ho voláme BEZPODMÍNEČNĚ (i při zavřeném okně - tam je render
     * no-op, ale zpracuje open->close hranu a deaktivuje plotter). */
    imgui_plotter_window(&g_gui->showPlotterWindow);
#endif

#if CFG_HWEXT_HAVE_RAMDISK
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* memory-disk-state: Memory Disk State debugger okno - stav vsech
     * ramdisku (MZ-1R18 STD + Pezik E8/68). Side-effect free, per frame. */
    if (g_gui->showRamdiskStateWindow)
        imgui_ramdisk_state_window(&g_gui->showRamdiskStateWindow);
#endif
#endif

#if CFG_HWEXT_HAVE_IDE8
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* memory-disk-state: IDE8 State debugger okno - stav IDE8 radice
     * (Master + Slave). Side-effect free, per frame. */
    if (g_gui->showIde8StateWindow)
        imgui_ide8_state_window(&g_gui->showIde8StateWindow);
#endif
#endif

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
    /* Výsledky snapshotů vyřízených emu vláknem: oznámení, obnovení běhu. */
    imgui_snapshot_request_results();

    /* Zásahy do zařízení odmítnuté emu vláknem (běží replay): oznámení. */
    imgui_device_request_results();

    /* Záznam a přehrání vstupů: dialogy nabídky Replay a oznámení výsledku */
    imgui_replay_dialogs();
    imgui_replay_window();

    /* Video záznam: dialog nastavení, okno dálkového ovládání + notifikace událostí (před vykreslením notifikace) */
    imgui_videorec_setup_dialog();
    imgui_videorec_remote_window();
    imgui_videorec_poll_events();

    /* Screenshot: dialog nastavení (Tools -> Screenshot -> Settings...) */
    imgui_screenshot_setup_dialog();
    imgui_screenshot_window();
    imgui_screenshot_series_poll();

    /* Zachycení zvuku: dialog nastavení a notifikace konce (Tools -> Audio Capture) */
    imgui_audio_capture_setup_dialog();
    imgui_audio_capture_window();
    imgui_audio_capture_poll();

    /* Upozornění na běh bez audio zařízení (jednou, před vykreslením notifikace) */
    imgui_audio_device_notice();

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
    {
        /* Okno čte stav Pezika; zásah mezitím neprovede emu vlákno. */
        device_state_lock();
        imgui_pezik_settings_window(&g_gui->showPezikSettingsWindow);
        device_state_unlock();
    }

    /* ROM nastavení */
    if (g_gui->showRomSettingsWindow)
    {
        /* Okno čte cesty uživatelské ROM v g_rom; zásah mezitím neprovede
         * emu vlákno. */
        device_state_lock();
        imgui_rom_settings_window(&g_gui->showRomSettingsWindow);
        device_state_unlock();
    }

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
    /* Okna debuggeru čtou stav jádra (g_breakpoints, watch, ...), který
     * vytvoří debugger_init() v emu vlákně. Do jejího dokončení se
     * nevykreslují - otevřená okna (např. obnovená z INI) se ukážou o pár
     * snímků později. Show příznaky zůstávají beze změny. */
    if (debugger_is_initialized())
    {
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

    /* Bus Map (D6): vlastníci portů a MMIO adres (všechny platformy); bere
     * zámek stavu zařízení sám kolem snímku, proto mimo zamčený blok. */
    bus_window_render(&g_gui->showBusMapWindow);

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

    /* RAM Write Log - záznam zápisů do paměti. Otevírá se přes menu
     * Debugger -> RAM Write Log nebo "Who wrote this byte?" v Memory
     * Browseru. */
    if (g_gui->showRamlogWindow)
        ramlog_window_render(&g_gui->showRamlogWindow);

    /* Memory Search - postupné hledání proměnných. Otevírá se přes menu
     * Debugger -> Memory Search nebo Alt+Shift+E. */
    if (g_gui->showMemorySearchWindow)
        memsearch_window_render(&g_gui->showMemorySearchWindow);

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
#if HAVE_PIOZ80
    if (g_gui->showPiozStateWindow)
        imgui_pioz80_state_window(&g_gui->showPiozStateWindow);
#endif
#if HAVE_PSG >= 1
    if (g_gui->showPsgStateWindow)
        imgui_psg_state_window(&g_gui->showPsgStateWindow);
    /* psg-audio-scope mutant F1: tick callback (= snapshot do ring bufferu)
     * je nepodmíněný - ring buffer drží 10 s historie i pokud okno není
     * otevřené, takže po otevření je rovnou plný. Vlastní render jen pokud
     * je okno viditelné. */
    psg_audio_scope_tick();
    if (g_gui->showPsgAudioScopeWindow)
        imgui_psg_audio_scope_window(&g_gui->showPsgAudioScopeWindow);
#endif

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

#if (MZARCH == 800) || (MZARCH == 1500) || (MZARCH == 700)
    /* Memory Heatmap (CDL) */
    if (g_gui->showMemoryHeatmapWindow)
        mhmap_window_render(&g_gui->showMemoryHeatmapWindow);
#endif
    } /* debugger_is_initialized() */
#endif

    imgui_file_chooser_window();
    imgui_message_window();
    /* Dialogy Unicard čtou stav Unicardu; zásah mezitím neprovede emu vlákno. */
    device_state_lock();
    imgui_unicard_runtime_mismatch_dialog();
    imgui_unicard_reinit_choice_dialog();
    device_state_unlock();
}