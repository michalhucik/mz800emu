/*
 * test_ui_functional.cpp - funkční testy UI logiky (menu, dialogy, notifikace)
 *
 * Testuje UI video záznamu (src/ui-imgui/videorec/):
 * - model položek podmenu Video Recording podle stavu nahrávání;
 * - formát času REC indikátoru;
 * - vykreslení podmenu (otevřeného) a dialogu nastavení v ImGui snímku bez assertu;
 * - akce Start/Stop a notifikace z událostí jádra (přes skutečné lepidlo
 *   videorec.c, snímky simulované ručním voláním hooku konce snímku);
 * - okno dálkového ovládání (Task 13): model stavu, vykreslení, marker s vlastním
 *   popiskem, poslední událost; tabulka zkratek (popisky odpovídají klávesám)
 *   a jejich obsluha simulovaným stiskem Alt (+ Shift) + klávesy;
 * - režim podle reality (Task 19): zkratka Alt+U přepíná časovou základnu,
 *   předvolby nastavení, text REC indikátoru s režimem, popis časové
 *   základny v okně, potvrzení dialogu nepřepíše základnu přepnutou za běhu,
 *   vykreslení okna, dialogu a menu v režimu podle reality.
 *
 * Notifikace se zachytávají stubem snapshot_notification_show_ex() (skutečná
 * implementace je ve snapshot_notification.cpp, který test nelinkuje). Stub
 * snapshot_notification_error_active() modeluje otevřené chybové okno: otevře
 * se chybovou notifikací a zůstane otevřené, dokud ho test nezavře
 * (s_modal_open = false).
 *
 * Licence: GPLv3
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <string>

#include "libs/imgui/imgui.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

extern "C" {
#include "mztest.h"
#include "emulator/emulator.h"
#include "hw-generic/gdg/gdg.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/videorec/videorec_menu.h"
#include "i18n.h"

/* UI globál, který jinak vytváří sdlapp_imgui_video.cpp. */
static MyImGui s_gui;
MyImGui *g_gui = &s_gui;

/* ================================================================
 * Stub notifikací (zachytí poslední zprávu)
 * ================================================================ */

static std::string s_note;
static bool s_note_error = false;
static int s_note_count = 0;
/** Model otevřeného chybového (modálního) okna. */
static bool s_modal_open = false;

extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title)
{
    (void)error_title;
    s_note = message ? message : "";
    s_note_error = is_error;
    s_note_count++;
    if (is_error) s_modal_open = true;
}

extern "C" bool snapshot_notification_error_active(void)
{
    return s_modal_open;
}

extern "C" void snapshot_notification_show(const char *message, bool is_error)
{
    snapshot_notification_show_ex(message, is_error, NULL);
}

/* ================================================================
 * SDL3 + GL + ImGui (vzor test_ui_smoke.cpp)
 * ================================================================ */

static SDL_Window *s_win = NULL;
static SDL_GLContext s_gl = NULL;
static ImGuiContext *s_ctx = NULL;
static bool s_ui_ok = false;

static bool ui_setup(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    s_win = SDL_CreateWindow("mz800emu-test", 640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!s_win) return false;
    s_gl = SDL_GL_CreateContext(s_win);
    if (!s_gl) return false;
    SDL_GL_MakeCurrent(s_win, s_gl);
    s_ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL;
    if (!ImGui_ImplSDL3_InitForOpenGL(s_win, s_gl)) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 130")) return false;
    return true;
}

static void ui_teardown(void)
{
    if (s_ctx) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(s_ctx);
        s_ctx = NULL;
    }
    if (s_gl) SDL_GL_DestroyContext(s_gl);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}

static void frame_begin(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

static void frame_end(void)
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/* ================================================================
 * Emulace (snímky pro lepidlo videorec)
 * ================================================================ */

static const char *OUT_DIR = "tests/data/tmp/ui_videorec";

static void emu_frame(void)
{
    g_gdg.total_elapsed.screens++;
    g_gdg.total_elapsed.ticks = 0;
    videorec_on_screen_done();
    videorec_audio_horizon(gdg_get_total_ticks());
}

/** Smaže soubory v OUT_DIR (vygenerované nahrávky). */
static void clean_out_dir(void)
{
    GDir *d = g_dir_open(OUT_DIR, 0, NULL);
    if (!d) return;
    const char *n;
    while ((n = g_dir_read_name(d)) != NULL) {
        char *p = g_build_filename(OUT_DIR, n, NULL);
        g_remove(p);
        g_free(p);
    }
    g_dir_close(d);
}

void setUp(void)
{
    g_mkdir_with_parents(OUT_DIR, 0755);
    clean_out_dir();
    g_strlcpy(g_videorec_settings.output_dir, OUT_DIR, sizeof(g_videorec_settings.output_dir));
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_SKIP;
    g_videorec_settings.record_debugger_steps = false;
    g_emulator.paused = false;
    videorec_init();
    imgui_videorec_poll_events(); /* zahodit události předchozích testů */
    s_note.clear();
    s_note_error = false;
    s_note_count = 0;
    s_modal_open = false;
}

void tearDown(void)
{
    videorec_exit();
    g_emulator.paused = false;
    imgui_videorec_poll_events();
    clean_out_dir();
}

/* ================================================================
 * Čistá logika
 * ================================================================ */

static void test_menu_model_idle(void)
{
    st_VIDEOREC_UI_MENU_MODEL m = videorec_ui_menu_model(VIDEOREC_STATE_IDLE, false);
    TEST_ASSERT_EQUAL_STRING("Start Recording", m.start_stop_label);
    TEST_ASSERT_TRUE(m.start_stop_enabled);
    TEST_ASSERT_FALSE(m.pause_enabled);
    TEST_ASSERT_FALSE(m.marker_enabled);
}

static void test_menu_model_recording_and_paused(void)
{
    st_VIDEOREC_UI_MENU_MODEL m = videorec_ui_menu_model(VIDEOREC_STATE_RECORDING, false);
    TEST_ASSERT_EQUAL_STRING("Stop Recording", m.start_stop_label);
    TEST_ASSERT_TRUE(m.pause_enabled);
    TEST_ASSERT_FALSE(m.pause_checked);
    TEST_ASSERT_TRUE(m.marker_enabled);

    m = videorec_ui_menu_model(VIDEOREC_STATE_PAUSED, false);
    TEST_ASSERT_EQUAL_STRING("Stop Recording", m.start_stop_label);
    TEST_ASSERT_TRUE(m.pause_checked);
}

/* Čekající start: položka už ukazuje Stop, pauza a marker zatím ne. Video
 * záznam je na všech platformách, akce nejsou nikdy zašedlé kvůli platformě. */
static void test_menu_model_pending(void)
{
    st_VIDEOREC_UI_MENU_MODEL m = videorec_ui_menu_model(VIDEOREC_STATE_IDLE, true);
    TEST_ASSERT_EQUAL_STRING("Stop Recording", m.start_stop_label);
    TEST_ASSERT_TRUE(m.start_stop_enabled);
    TEST_ASSERT_FALSE(m.pause_enabled);
    TEST_ASSERT_FALSE(m.marker_enabled);
}

/* Čas nahrávky podle fps platformy: 50 (MZ-800, MZ-700 PAL) i 60 (MZ-1500, MZ-700 NTSC). */
static void test_format_time(void)
{
    char b[16];
    videorec_ui_format_time(0, 50, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("00:00:00", b);
    videorec_ui_format_time(83 * 50 + 49, 50, b, sizeof(b)); /* 1:23 a 49 snímků */
    TEST_ASSERT_EQUAL_STRING("00:01:23", b);
    videorec_ui_format_time((uint64_t)(2 * 3600 + 5) * 50, 50, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("02:00:05", b);
    videorec_ui_format_time(83 * 60 + 59, 60, b, sizeof(b)); /* 1:23 a 59 snímků při 60 fps */
    TEST_ASSERT_EQUAL_STRING("00:01:23", b);
    videorec_ui_format_time(83 * 60, 50, b, sizeof(b));      /* tytéž snímky při 50 fps = 1:39 */
    TEST_ASSERT_EQUAL_STRING("00:01:39", b);
    videorec_ui_format_time(120, 0, b, sizeof(b));           /* neplatné fps se nahradí 1 */
    TEST_ASSERT_EQUAL_STRING("00:02:00", b);
    TEST_ASSERT_EQUAL_UINT(VIDEO_SCREENS_PER_SEC, videorec_get_fps());
}

/* ================================================================
 * Vykreslení
 * ================================================================ */

/* Podmenu se vykreslí uvnitř otevřeného menu (popup Tools) i rozbalené, bez assertu. */
static void test_menu_renders_in_open_menu(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    for (int i = 0; i < 4; i++) {
        frame_begin();
        ImGui::Begin("host");
        if (i == 0) ImGui::OpenPopup("ToolsPopup");
        if (ImGui::BeginPopup("ToolsPopup")) {
            /* rozbalit podmenu (BeginMenu kontroluje otevřený popup se stejným ID) */
            if (i == 1) ImGui::OpenPopup(_L("Video Recording"));
            imgui_videorec_menu();
            if (i >= 2) TEST_ASSERT_TRUE(ImGui::IsPopupOpen(_L("Video Recording")));
            ImGui::EndPopup();
        }
        ImGui::End();
        frame_end();
    }
}

/* Dialog nastavení: otevření, vykreslení, zavření bez assertu; OK/Cancel nemění nastavení bez akce. */
static void test_setup_dialog_renders(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    st_VIDEOREC_SETTINGS before = g_videorec_settings;
    g_gui->showVideorecSetupWindow = true;
    for (int i = 0; i < 3; i++) {
        frame_begin();
        imgui_videorec_setup_dialog();
        frame_end();
    }
    g_gui->showVideorecSetupWindow = false;
    frame_begin();
    imgui_videorec_setup_dialog();
    frame_end();
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_videorec_settings, sizeof(before));
}

/* Overlay se vykreslí za obrazem (GetItemRect) i během nahrávání. */
static void test_overlay_renders(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    videorec_ui_toggle_recording();
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    frame_begin();
    ImGui::Begin("img");
    ImGui::Dummy(ImVec2(320, 200));
    imgui_videorec_overlay();
    ImGui::End();
    frame_end();
}

/* ================================================================
 * Akce a notifikace
 * ================================================================ */

/* Start/Stop přes akci zkratky Alt+O: notifikace "Recording started" a "Recording saved: <cesta>". */
static void test_toggle_start_stop_notifications(void)
{
    videorec_ui_toggle_recording();
    emu_frame(); /* start se zpracuje na konci snímku */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    imgui_videorec_poll_events();
    TEST_ASSERT_EQUAL_STRING("Recording started", s_note.c_str());
    TEST_ASSERT_FALSE(s_note_error);

    for (int i = 0; i < 3; i++) emu_frame();
    videorec_ui_add_marker();
    videorec_ui_toggle_recording();
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit(); /* writer dokončí zápis a vydá SAVED */
    imgui_videorec_poll_events();
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(s_note.c_str(), "Recording saved: "), s_note.c_str());
    TEST_ASSERT_TRUE(strstr(s_note.c_str(), "mz800_") != NULL);
    TEST_ASSERT_FALSE(s_note_error);
}

/* Start v pauze emulace: upozornění; druhé Alt+O čekající start zruší. */
static void test_toggle_while_emulation_paused(void)
{
    g_emulator.paused = true;
    videorec_ui_toggle_recording();
    TEST_ASSERT_EQUAL_STRING("Recording will start when emulation resumes", s_note.c_str());
    videorec_ui_toggle_recording(); /* zrušit čekající start */
    g_emulator.paused = false;
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
}

/* Chyba startu (adresář nelze vytvořit) -> chybová notifikace. */
static void test_start_error_notification(void)
{
    /* cesta pod existujícím souborem nemůže být adresářem */
    const char *file = "tests/data/tmp/ui_videorec/not_a_dir";
    g_file_set_contents(file, "x", 1, NULL);
    g_strlcpy(g_videorec_settings.output_dir, file, sizeof(g_videorec_settings.output_dir));
    videorec_ui_toggle_recording();
    TEST_ASSERT_TRUE(s_note_error);
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(s_note.c_str(), "Recording failed: "), s_note.c_str());
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
}

/* Šev: přeložená šablona bez proměnných, retake: šablona s časem. */
static void test_seam_notification(void)
{
    videorec_ui_toggle_recording();
    emu_frame();
    for (int i = 0; i < 5; i++) emu_frame();
    videorec_on_snapshot_loaded(NULL);
    emu_frame();
    imgui_videorec_poll_events();
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", s_note.c_str());
}

/* Běžná notifikace (toast) nesmí nahradit otevřené chybové okno ani z pozdější
 * dávky událostí (finální review, Task 9 minor). */
static void test_toast_does_not_replace_error_modal(void)
{
    const char *file = "tests/data/tmp/ui_videorec/not_a_dir";
    g_file_set_contents(file, "x", 1, NULL);
    g_strlcpy(g_videorec_settings.output_dir, file, sizeof(g_videorec_settings.output_dir));
    videorec_ui_toggle_recording(); /* chyba startu -> chybové okno */
    TEST_ASSERT_TRUE(s_note_error);
    std::string err = s_note;

    g_strlcpy(g_videorec_settings.output_dir, OUT_DIR, sizeof(g_videorec_settings.output_dir));
    videorec_ui_toggle_recording();
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    imgui_videorec_poll_events(); /* STARTED: toast se nesmí zobrazit přes otevřené okno */
    TEST_ASSERT_TRUE(s_note_error);
    TEST_ASSERT_EQUAL_STRING(err.c_str(), s_note.c_str());

    s_modal_open = false; /* uživatel okno zavřel */
    videorec_on_snapshot_loaded(NULL);
    emu_frame();
    imgui_videorec_poll_events();
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", s_note.c_str());
    TEST_ASSERT_FALSE(s_note_error);
}

/* Čekající start zrušený mimo UI (jádro je v klidu a start nečeká): UI nesmí
 * zůstat ve stavu "čeká start" - další Alt+O musí nahrávání spustit, ne zastavit. */
static void test_start_pending_cleared_when_core_idle(void)
{
    videorec_ui_toggle_recording(); /* čekající start */
    videorec_request_stop();        /* zrušení mimo UI (jiný volající) */
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    imgui_videorec_poll_events();
    videorec_ui_toggle_recording(); /* musí to být start */
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/* ================================================================
 * Okno dálkového ovládání (Task 13) a tabulka zkratek
 * ================================================================ */

/* Popisky zkratek se generují z jedné tabulky: musí odpovídat klávese
 * a modifikátoru, které obsluha skutečně testuje, a být navzájem různé. */
static void test_shortcut_labels_match_keys(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    for (int i = 0; i < VIDEOREC_SHORTCUT_COUNT; i++) {
        int key = 0;
        bool shift = false;
        videorec_ui_shortcut_keys((en_VIDEOREC_SHORTCUT)i, &key, &shift);
        std::string expected = std::string("Alt+") + (shift ? "Shift+" : "") + ImGui::GetKeyName((ImGuiKey)key);
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), videorec_ui_shortcut_label((en_VIDEOREC_SHORTCUT)i));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_TRUE(strcmp(videorec_ui_shortcut_label((en_VIDEOREC_SHORTCUT)i),
                                    videorec_ui_shortcut_label((en_VIDEOREC_SHORTCUT)j)) != 0);
        }
    }
    TEST_ASSERT_EQUAL_STRING("Alt+O", videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_RECORDING));
    TEST_ASSERT_EQUAL_STRING("Alt+Shift+O", videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_PAUSE));
    TEST_ASSERT_EQUAL_STRING("Alt+L", videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_ADD_MARKER));
    TEST_ASSERT_EQUAL_STRING("Alt+Shift+L", videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_REMOTE_WINDOW));
    TEST_ASSERT_EQUAL_STRING("Alt+U", videorec_ui_shortcut_label(VIDEOREC_SHORTCUT_TOGGLE_TIMEBASE));
}

/** Jeden ImGui snímek se stiskem Alt (+ Shift) + klávesy, během kterého běží obsluha zkratek. */
static void press_alt_key(ImGuiKey key, bool shift)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    io.AddKeyEvent(ImGuiKey_LeftAlt, true);
    if (shift) {
        io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(ImGuiKey_LeftShift, true);
    }
    io.AddKeyEvent(key, true);
    frame_begin();
    imgui_videorec_shortcuts();
    frame_end();
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiKey_LeftShift, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiKey_LeftAlt, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame_begin();
    frame_end();
}

/* Alt+Shift+L přepíná okno; Alt+O spustí nahrávání (obsluha z tabulky). */
static void test_shortcuts_dispatch(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    g_gui->showVideorecRemoteWindow = false;
    press_alt_key(ImGuiKey_L, true);
    TEST_ASSERT_TRUE(g_gui->showVideorecRemoteWindow);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state()); /* ne marker / start */
    press_alt_key(ImGuiKey_L, true);
    TEST_ASSERT_FALSE(g_gui->showVideorecRemoteWindow);

    press_alt_key(ImGuiKey_O, false);
    TEST_ASSERT_TRUE(videorec_ui_is_start_pending());
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/* Model okna: popisek a barva stavu, popisek pauzy, povolení akcí. */
static void test_remote_model(void)
{
    st_VIDEOREC_STATUS st;
    memset(&st, 0, sizeof(st));
    st_VIDEOREC_UI_REMOTE_MODEL m = videorec_ui_remote_model(&st, false);
    TEST_ASSERT_EQUAL_STRING("IDLE", m.state_label);
    TEST_ASSERT_EQUAL_STRING("Start Recording", m.actions.start_stop_label);
    TEST_ASSERT_FALSE(m.actions.marker_enabled);

    m = videorec_ui_remote_model(&st, true);
    TEST_ASSERT_EQUAL_STRING("STARTING", m.state_label);
    TEST_ASSERT_EQUAL_STRING("Stop Recording", m.actions.start_stop_label);

    st.state = VIDEOREC_STATE_RECORDING;
    m = videorec_ui_remote_model(&st, false);
    TEST_ASSERT_EQUAL_STRING("REC", m.state_label);
    TEST_ASSERT_EQUAL_STRING("Pause Recording", m.pause_label);
    TEST_ASSERT_TRUE(m.actions.marker_enabled);

    st.state = VIDEOREC_STATE_PAUSED;
    m = videorec_ui_remote_model(&st, false);
    TEST_ASSERT_EQUAL_STRING("PAUSED", m.state_label);
    TEST_ASSERT_EQUAL_STRING("Resume Recording", m.pause_label);
    st_VIDEOREC_STATUS idle;
    memset(&idle, 0, sizeof(idle));
    TEST_ASSERT_TRUE(m.state_rgba != videorec_ui_remote_model(&idle, false).state_rgba);
    TEST_ASSERT_TRUE(m.actions.start_stop_enabled);
}

/* Okno se vykreslí bez nahrávání, při nahrávání i v pauze nahrávání (bez assertu)
 * a zavření (flag) ho skryje. */
static void test_remote_window_renders(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    g_gui->showVideorecRemoteWindow = true;
    for (int phase = 0; phase < 3; phase++) {
        if (phase == 1) {
            videorec_ui_toggle_recording();
            emu_frame();
            emu_frame();
            imgui_videorec_poll_events();
        }
        if (phase == 2) {
            videorec_ui_toggle_pause();
            emu_frame();
        }
        for (int i = 0; i < 2; i++) {
            frame_begin();
            imgui_videorec_remote_window();
            frame_end();
        }
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
    g_gui->showVideorecRemoteWindow = false;
    frame_begin();
    imgui_videorec_remote_window();
    frame_end();
}

/* Marker s vlastním popiskem a výchozí "Marker N" (N = pořadí markeru v session). */
static void test_marker_custom_label(void)
{
    char *avi = g_build_filename(OUT_DIR, "marker_test.avi", NULL);
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, avi, sizeof(s.path));
    TEST_ASSERT_TRUE(videorec_request_start(&s));
    emu_frame();
    emu_frame();
    videorec_ui_add_marker_label("Boss fight");
    emu_frame();
    videorec_ui_add_marker_label("");
    emu_frame();
    videorec_request_stop();
    emu_frame();
    videorec_exit();

    char *cuts = g_build_filename(OUT_DIR, "marker_test.cuts.json", NULL);
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(cuts, &j, NULL, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"Boss fight\""), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"Marker 2\""), j);
    g_free(j);
    g_free(cuts);
    g_free(avi);
}

/* Poslední událost pro okno: stejný (přeložený) text jako notifikace, chyba označená. */
static void test_last_event_text(void)
{
    videorec_ui_toggle_recording();
    emu_frame();
    videorec_on_snapshot_loaded(NULL);
    emu_frame();
    imgui_videorec_poll_events();
    bool err = true;
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_ui_last_event(&err));
    TEST_ASSERT_FALSE(err);
    videorec_ui_toggle_recording(); /* stop */
    emu_frame();

    const char *file = "tests/data/tmp/ui_videorec/not_a_dir";
    g_file_set_contents(file, "x", 1, NULL);
    g_strlcpy(g_videorec_settings.output_dir, file, sizeof(g_videorec_settings.output_dir));
    videorec_ui_toggle_recording(); /* chyba startu */
    TEST_ASSERT_TRUE(g_str_has_prefix(videorec_ui_last_event(&err), "Recording failed: "));
    TEST_ASSERT_TRUE(err);
}

/* ================================================================
 * Režim podle reality (Task 19): přepínač časové základny, předvolby,
 * REC indikátor s režimem, potvrzení nastavení
 * ================================================================ */

/* Alt+U přepíná časovou základnu (bez nahrávání = nastavení pro příští start)
 * a ohlásí ji notifikací; Alt+Shift+U nic nedělá. */
static void test_timebase_shortcut(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
    press_alt_key(ImGuiKey_U, true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
    press_alt_key(ImGuiKey_U, false);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase);
    TEST_ASSERT_EQUAL_STRING("Recording time base: Real time", s_note.c_str());
    TEST_ASSERT_FALSE(s_note_error);
    TEST_ASSERT_EQUAL_STRING("Recording time base: Real time", videorec_ui_last_event(NULL));
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase);
    press_alt_key(ImGuiKey_U, false);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
    TEST_ASSERT_EQUAL_STRING("Recording time base: Emulated time", s_note.c_str());
}

/* Předvolby nastaví jen své klíče a poznají se. */
static void test_presets(void)
{
    st_VIDEOREC_SETTINGS s = g_videorec_settings;
    s.realtime_pause_cap_s = 7;
    s.auto_markers = false;
    s.state_marks = VIDEOREC_STATE_MARKS_NONE;
    TEST_ASSERT_FALSE(videorec_ui_preset_matches(&s, VIDEOREC_UI_PRESET_LIVE));
    videorec_ui_apply_preset(&s, VIDEOREC_UI_PRESET_LIVE);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, s.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_FREEZE_CAPPED, s.realtime_pause);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_MARKS_SIDECAR, s.state_marks);
    TEST_ASSERT_EQUAL_UINT(7, s.realtime_pause_cap_s); /* mimo předvolbu beze změny */
    TEST_ASSERT_FALSE(s.auto_markers);
    TEST_ASSERT_TRUE(videorec_ui_preset_matches(&s, VIDEOREC_UI_PRESET_LIVE));
    TEST_ASSERT_FALSE(videorec_ui_preset_matches(&s, VIDEOREC_UI_PRESET_GAMEPLAY));

    videorec_ui_apply_preset(&s, VIDEOREC_UI_PRESET_GAMEPLAY);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, s.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_SKIP, s.realtime_pause);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_MARKS_SIDECAR, s.state_marks);
    TEST_ASSERT_TRUE(videorec_ui_preset_matches(&s, VIDEOREC_UI_PRESET_GAMEPLAY));

    /* úprava jednotlivé hodnoty předvolbu "rozbije" (zůstává upravitelná) */
    s.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    TEST_ASSERT_FALSE(videorec_ui_preset_matches(&s, VIDEOREC_UI_PRESET_GAMEPLAY));

    TEST_ASSERT_EQUAL_STRING("Gameplay showcase", videorec_ui_preset_msgid(VIDEOREC_UI_PRESET_GAMEPLAY));
    TEST_ASSERT_EQUAL_STRING("Live / tutorial", videorec_ui_preset_msgid(VIDEOREC_UI_PRESET_LIVE));
    TEST_ASSERT_EQUAL_STRING("", videorec_ui_preset_msgid(VIDEOREC_UI_PRESET_COUNT));
    st_VIDEOREC_SETTINGS before = s;
    videorec_ui_apply_preset(&s, VIDEOREC_UI_PRESET_COUNT);
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

/* REC indikátor ukazuje skutečný režim; řádek stavu v okně rozliší činnost realtime. */
static void test_overlay_text_and_timebase_status(void)
{
    st_VIDEOREC_STATUS st;
    memset(&st, 0, sizeof(st));
    char buf[96];
    st.state = VIDEOREC_STATE_RECORDING;
    st.frames = 754 * 50;
    videorec_ui_overlay_text(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("REC 00:12:34 \xC2\xB7 emulated", buf);

    st.timebase = VIDEOREC_TIMEBASE_REALTIME;
    st.timebase_effective = VIDEOREC_TIMEBASE_REALTIME;
    st.rt_activity = VIDEOREC_RT_ACTIVITY_LIVE;
    videorec_ui_overlay_text(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("REC 00:12:34 \xC2\xB7 real-time", buf);
    st.state = VIDEOREC_STATE_PAUSED;
    videorec_ui_overlay_text(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("PAUSE 00:12:34 \xC2\xB7 real-time", buf);

    TEST_ASSERT_EQUAL_STRING("Real time (live)", videorec_ui_timebase_status_msgid(&st, true));
    st.rt_activity = VIDEOREC_RT_ACTIVITY_FROZEN;
    TEST_ASSERT_EQUAL_STRING("Real time (frozen picture)", videorec_ui_timebase_status_msgid(&st, true));
    st.rt_activity = VIDEOREC_RT_ACTIVITY_SKIPPING;
    TEST_ASSERT_EQUAL_STRING("Real time (not writing)", videorec_ui_timebase_status_msgid(&st, true));
    st.timebase_effective = VIDEOREC_TIMEBASE_EMULATED;
    TEST_ASSERT_EQUAL_STRING("Emulated time (real time requested)", videorec_ui_timebase_status_msgid(&st, true));
    TEST_ASSERT_EQUAL_STRING("Real time", videorec_ui_timebase_status_msgid(&st, false));
    st.timebase = VIDEOREC_TIMEBASE_EMULATED;
    TEST_ASSERT_EQUAL_STRING("Emulated time", videorec_ui_timebase_status_msgid(&st, true));
}

/* OK v dialogu: časová základna, na kterou uživatel v dialogu nesáhl, nepřepíše
 * hodnotu přepnutou za běhu; změněná (přepínač nebo předvolba) se přepne hned;
 * ostatní hodnoty se zapíšou. */
static void test_settings_apply_keeps_runtime_timebase(void)
{
    st_VIDEOREC_SETTINGS edited = g_videorec_settings; /* dialog otevřen v EMULATED */
    edited.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    edited.record_debugger_steps = true;
    videorec_ui_set_timebase(VIDEOREC_TIMEBASE_REALTIME); /* Alt+U při otevřeném dialogu */
    videorec_ui_settings_apply(&edited, false);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_FREEZE, g_videorec_settings.realtime_pause);
    TEST_ASSERT_TRUE(g_videorec_settings.record_debugger_steps);

    edited = g_videorec_settings;
    edited.timebase = VIDEOREC_TIMEBASE_EMULATED; /* změna v dialogu */
    videorec_ui_settings_apply(&edited, true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, st.timebase);
}

/* Dialog otevřený v EMULATED, Alt+U přepne na REALTIME: nedotčená kopie se
 * srovná s aktuální hodnotou (přepínač ukazuje realtime) a předvolba Gameplay
 * + OK opravdu vrátí emulační čas (review Task 19). */
static void test_setup_dialog_follows_runtime_timebase(void)
{
    st_VIDEOREC_SETTINGS edited = g_videorec_settings; /* otevření v EMULATED */
    videorec_ui_set_timebase(VIDEOREC_TIMEBASE_REALTIME); /* Alt+U */
    videorec_ui_setup_sync_timebase(&edited, false);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, edited.timebase);
    TEST_ASSERT_FALSE(videorec_ui_preset_matches(&edited, VIDEOREC_UI_PRESET_GAMEPLAY));

    videorec_ui_apply_preset(&edited, VIDEOREC_UI_PRESET_GAMEPLAY); /* dotčeno */
    videorec_ui_setup_sync_timebase(&edited, true);              /* dotčenou nepřepisuje */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, edited.timebase);
    videorec_ui_settings_apply(&edited, true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
}

/* Okno dálkového ovládání, dialog nastavení a menu se vykreslí i s režimem podle reality. */
static void test_realtime_ui_renders(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    videorec_ui_set_timebase(VIDEOREC_TIMEBASE_REALTIME);
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE_CAPPED;
    g_gui->showVideorecRemoteWindow = true;
    g_gui->showVideorecSetupWindow = true;
    for (int i = 0; i < 4; i++) {
        frame_begin();
        imgui_videorec_remote_window();
        imgui_videorec_setup_dialog();
        ImGui::Begin("host");
        if (i == 0) ImGui::OpenPopup("ToolsPopup");
        if (ImGui::BeginPopup("ToolsPopup")) {
            if (i == 1) ImGui::OpenPopup(_L("Video Recording"));
            imgui_videorec_menu();
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(320, 200));
        imgui_videorec_overlay(); /* bez nahrávání nic nekreslí */
        ImGui::End();
        frame_end();
    }
    g_gui->showVideorecRemoteWindow = false;
    g_gui->showVideorecSetupWindow = false;
    frame_begin();
    imgui_videorec_setup_dialog();
    frame_end();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase); /* Cancel nic nezmění */
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_ui_ok = ui_setup();

    UNITY_BEGIN();
    RUN_TEST(test_menu_model_idle);
    RUN_TEST(test_menu_model_recording_and_paused);
    RUN_TEST(test_menu_model_pending);
    RUN_TEST(test_format_time);
    RUN_TEST(test_menu_renders_in_open_menu);
    RUN_TEST(test_setup_dialog_renders);
    RUN_TEST(test_overlay_renders);
    RUN_TEST(test_toggle_start_stop_notifications);
    RUN_TEST(test_toggle_while_emulation_paused);
    RUN_TEST(test_start_error_notification);
    RUN_TEST(test_seam_notification);
    RUN_TEST(test_toast_does_not_replace_error_modal);
    RUN_TEST(test_start_pending_cleared_when_core_idle);
    RUN_TEST(test_shortcut_labels_match_keys);
    RUN_TEST(test_shortcuts_dispatch);
    RUN_TEST(test_remote_model);
    RUN_TEST(test_remote_window_renders);
    RUN_TEST(test_marker_custom_label);
    RUN_TEST(test_last_event_text);
    RUN_TEST(test_timebase_shortcut);
    RUN_TEST(test_presets);
    RUN_TEST(test_overlay_text_and_timebase_status);
    RUN_TEST(test_settings_apply_keeps_runtime_timebase);
    RUN_TEST(test_setup_dialog_follows_runtime_timebase);
    RUN_TEST(test_realtime_ui_renders);
    int result = UNITY_END();

    ui_teardown();
    mztest_teardown();
    return result;
}
