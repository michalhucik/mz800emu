/*
 * test_bus_window.cpp - okno Bus Map (src/ui-imgui/debugger/busmap/bus_window.cpp)
 *
 * UI test s reálným jádrem (devbus s falešným adaptérem a dummy zařízením
 * na 74h-75h, bus_map_snapshot nad statickou tabulkou MZ-800 testovacího
 * jádra) a stuby UI helperů (dbgapi peek, reset aktivity, panel
 * breakpointu, skok do okna I/O Ports, monospace font):
 * - tři snímky s otevřeným oknem bez assertu, zavření vypne klienta sledování;
 * - klik na buňku vybere adresu i řádek seznamu, klik na řádek vybere jeho první buňku;
 * - hover na řádek seznamu zvýrazní jeho buňky v mřížce (další snímek);
 * - vypnuté sledování: řádek aktivity má "-" místo nul; detail má stejnou výšku
 *   se sledováním, bez něj i bez vlastníka (R04); adresa bez vlastníka má
 *   "Flags: -" (R08);
 * - pod přirozenou šířkou se nic nepřeskládá (buňka i seznam na minimu, dva snímky);
 * - peek v pauze automaticky s textem "(automatic while paused)", za běhu bez něj;
 * - záložka MMIO, zrušená záložka Devices (2) = Ports; šířka seznamu, buňka
 *   a oddělovač shodné pro obě záložky (R05);
 * - persistence: klíče color_mode, active_tab, list_width, tracking_active,
 *   bez show_legend a inspector_width; po parse + propagate bez INI zůstává
 *   Owners (V6);
 * - MZ-800: seznam má víc řádků než se vejde, vybraný poslední řádek je vidět;
 * - ikona stavu: tooltip ikony i tooltip řádku říkají stav slovem;
 * - sbalení detailu šipkou: pevná výška sbaleného detailu, seznam dostane
 *   víc řádků, persistence detail_open (výchozí rozbaleno, uložení a načtení);
 * - s písmem aplikace (DroidSans 28 px z ui_resources, chybí-li, test se
 *   přeskočí): přirozená šířka okna <= 1824 px (95 % FullHD), při výšce
 *   720 px buňka na mezi čitelnosti V1 (zkratka + 2 x 2 px) a shodná na
 *   obou záložkách, mřížka celá právě od min_full_h; za nejdelším názvem
 *   vlastníka >= 22 px do sloupce Ports (P04);
 * - pravá část pro výšky okna od minima do přirozené výšky (100 % i 175 %):
 *   tlačítko Break on OUT je celé vidět, nebo má pravá část viditelný
 *   posuvník a kolečkem se k němu dojede (P01);
 * - výběr adresy s viditelným řádkem seznam neodroluje, řádek mimo pohled
 *   (i po zmenšení okna) se dorovná;
 * - záhlaví sloupce ikony: značka "(?)" nad ikonami, tooltip <= 35 em (P02,
 *   P03); záložka MMIO má sloupec Addresses (P05).
 *   Druhá registrace CTest (ui-bus-window-de) spustí
 *   s MZ_BUS_TEST_LANG=de jen test šířky v němčině (bez katalogu de se
 *   přeskočí).
 *
 * Režim smoke screenshotů (mimo CTest): proměnná MZ_BUS_SHOT_DIR = adresář
 * pro BMP snímky stavů okna; volitelně MZ_BUS_SHOT_FONT (TTF hlavního
 * písma, 28 px jako v aplikaci), MZ_BUS_SHOT_MONO (monospace TTF),
 * MZ_BUS_SHOT_SCALE (např. 1.75), MZ_BUS_SHOT_LANG + MZ_BUS_SHOT_LOCALEDIR
 * (jazyk překladu), MZ_BUS_SHOT_W / MZ_BUS_SHOT_H (rozměr plátna). Kreslí se
 * do FBO skrytého okna, snímky se ukládají přes SDL_SaveBMP.
 *
 * Licence: GPLv3
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <locale.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

extern "C" {
#include "mztest.h"
#include "emulator/emulator.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "hw-generic/devbus/devbus.h"
#include "debugger/io_activity.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/bus_map.h"
#include "../peripherals/devbus_dummy.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/busmap/bus_window.h"
#include "i18n.h"

#ifndef MZTEST_FONT_DIR
#define MZTEST_FONT_DIR ""
#endif
#ifndef MZTEST_LOCALE_DIR
#define MZTEST_LOCALE_DIR ""
#endif

/* UI globál, který jinak vytváří sdlapp_imgui_video.cpp. */
static MyImGui s_gui;
MyImGui *g_gui = &s_gui;

/* ================================================================
 * Stuby UI helperů (skutečné implementace jsou v ui-imgui, test je nelinkuje)
 * ================================================================ */

static int s_recompute = 0;
static int s_peeks = 0;
static int s_resets = 0;
static int s_bp_opens = 0;
static int s_io_jumps = 0;
static uint16_t s_last_jump = 0xFFFF;
static ImFont *s_mono = NULL;

extern "C" void dbg_ui_debugger_state_recompute(void) { s_recompute++; }
extern "C" bool dbg_ui_bus_peek(uint16_t addr, bool mmio, uint8_t *value, uint8_t *driven, int *source)
{
    (void) mmio;
    s_peeks++;
    if (value) *value = (uint8_t) (addr & 0xFF);
    if (driven) *driven = 0xFF;
    if (source) *source = DBGAPI_BUS_PEEK_SRC_DEVBUS;
    return true;
}
extern "C" bool dbg_ui_io_activity_reset_port(uint16_t port, bool is_8bit) { (void) port; (void) is_8bit; s_resets++; return true; }
extern "C" void bpt_edit_panel_open_new_iorq(uint16_t port_addr, bool is_write) { (void) port_addr; (void) is_write; s_bp_opens++; }
extern "C" void io_window_request_show_port(uint16_t addr) { s_io_jumps++; s_last_jump = addr; }
extern "C" void io_window_heat_thresholds(int *text_active, int *bg_hot)
{
    if (text_active) *text_active = 1;
    if (bg_hot) *bg_hot = 10000;
}
ImFont *myimgui_get_monospace_font(void) { return s_mono; }

/* ================================================================
 * SDL3 + GL + ImGui (vzor test_ui_smoke.cpp)
 * ================================================================ */

static SDL_Window *s_win = NULL;
static SDL_GLContext s_gl = NULL;
static ImGuiContext *s_ctx = NULL;
static bool s_ui_ok = false;
static int s_canvas_w = 1280, s_canvas_h = 800;
/** Písmo aplikace pro testy geometrie (DroidSans 28 px); NULL = výchozí písmo ImGui. */
static ImFont *s_app_font = NULL;
/** Písmo, které frame_begin() pushne pro celý snímek (NULL = žádné). */
static ImFont *s_frame_font = NULL;
/** Počet snímků, po kterých ImGui hlásí kolizi ID položky pod myší (ConfigDebugHighlightIdConflicts). */
static int s_id_conflicts = 0;
/** Jazyk běhu (MZ_BUS_TEST_LANG), "" = angličtina. */
static const char *s_test_lang = "";

/* FBO pro režim screenshotů (funkce GL 3.0 přes SDL_GL_GetProcAddress). */
typedef void (APIENTRY *fn_gen_fb)(GLsizei, GLuint *);
typedef void (APIENTRY *fn_bind_fb)(GLenum, GLuint);
typedef void (APIENTRY *fn_fb_tex2d)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *fn_check_fb)(GLenum);
static fn_bind_fb s_bind_fb = NULL;
static GLuint s_fbo = 0;
#define TEST_GL_FRAMEBUFFER 0x8D40
#define TEST_GL_COLOR_ATTACHMENT0 0x8CE0
#define TEST_GL_FRAMEBUFFER_COMPLETE 0x8CD5

static bool ui_setup(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    s_win = SDL_CreateWindow("mz800emu-test", s_canvas_w, s_canvas_h, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!s_win) return false;
    s_gl = SDL_GL_CreateContext(s_win);
    if (!s_gl) return false;
    SDL_GL_MakeCurrent(s_win, s_gl);
    s_ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL;
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = true;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    if (GImGui->DebugDrawIdConflictsId != 0) s_id_conflicts++;
    if (s_frame_font) ImGui::PushFont(s_frame_font, 28.0f);
}

static void frame_end(void)
{
    if (s_frame_font) ImGui::PopFont();
    ImGui::Render();
    if (s_bind_fb && s_fbo) {
        s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, s_canvas_w, s_canvas_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/** Jeden snímek s oknem Bus Map ukotveným v levém horním rohu; size > 0 vynutí velikost okna. */
static void frame_with_window_sized(bool *open, float w, float h)
{
    frame_begin();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    if (w > 0.0f) ImGui::SetWindowSize(_L("Bus Map###bus_map"), ImVec2(w, h), ImGuiCond_Always);
    bus_window_render(open);
    frame_end();
}

static void frame_with_window(bool *open) { frame_with_window_sized(open, 0.0f, 0.0f); }

/* ================================================================
 * devbus s falešným adaptérem a dummy zařízením
 * ================================================================ */

static void fake_hooks_changed(void) { }
static uint8_t fake_bus_latch(void) { return 0x5A; }
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, NULL, fake_hooks_changed, NULL, NULL, NULL, NULL,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
    fake_bus_latch, NULL, NULL,
};
static st_DEVBUS_DUMMY s_a;
static st_DEVBUS_DEVICE_DESC s_desc_a;
static char *s_ini_path;
static struct st_CFGROOT *s_saved_root;
static struct st_CFGROOT *s_tmp_root;

void setUp(void)
{
    s_recompute = s_peeks = s_resets = s_bp_opens = s_io_jumps = 0;
    memset(&s_a, 0, sizeof(s_a));
    devbus_dummy_make_desc(&s_desc_a, "dummy", "DUMMY", 0);
    s_ini_path = g_build_filename(g_get_tmp_dir(), "mz_bus_window_test.ini", NULL);
    g_remove(s_ini_path);
    s_saved_root = g_cfgmain;
    s_tmp_root = cfgroot_new(s_ini_path);
    g_cfgmain = s_tmp_root;
    devbus_init(&s_fake_ops);
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    devbus_connect(0);
    g_emulator.paused = false;
    g_io_window_tracking_active = 0;
    bus_window_show_tab(0);
}

void tearDown(void)
{
    bool closed = false;
    s_frame_font = NULL;
    frame_with_window(&closed);
    devbus_exit();
    g_cfgmain = s_saved_root;
    cfgroot_destroy(s_tmp_root);
    g_remove(s_ini_path);
    g_free(s_ini_path);
}

/** Klik levým tlačítkem na souřadnice (čtyři snímky: pohyb, stisk, uvolnění, snímek po kliku pro odvozený stav). */
static void click_at(bool *open, float x, float y)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame_with_window(open);
    io.AddMouseButtonEvent(0, true);
    frame_with_window(open);
    io.AddMouseButtonEvent(0, false);
    frame_with_window(open);
    frame_with_window(open);
}

/** Klik levým tlačítkem při vynucené velikosti okna (vzor click_at()). */
static void click_at_sized(bool *open, float x, float y, float w, float h)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame_with_window_sized(open, w, h);
    io.AddMouseButtonEvent(0, true);
    frame_with_window_sized(open, w, h);
    io.AddMouseButtonEvent(0, false);
    frame_with_window_sized(open, w, h);
    frame_with_window_sized(open, w, h);
}

/** Načte písmo aplikace (DroidSans 28 px) jednou; false = soubor není (test se přeskočí). */
static bool app_font_ready(void)
{
    if (s_app_font) return true;
    if (!MZTEST_FONT_DIR[0]) return false;
    char *path = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) s_app_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 28.0f);
    g_free(path);
    return s_app_font != NULL;
}

/* ================================================================
 * Testy
 * ================================================================ */

/* Tři snímky bez pádu; otevření zapne sledování (klient Bus Map), zavření vypne. */
static void test_render_frames_and_tracking(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    for (int i = 0; i < 3; i++) frame_with_window(&open);
    TEST_ASSERT_TRUE(io_activity_tracking_client_active(IO_TRACKING_CLIENT_BUS_MAP));
    TEST_ASSERT_EQUAL_UINT8(1, g_io_window_tracking_active);
    TEST_ASSERT_TRUE(s_recompute >= 1);
    int rc = s_recompute;
    open = false;
    frame_with_window(&open);
    TEST_ASSERT_FALSE(io_activity_tracking_client_active(IO_TRACKING_CLIENT_BUS_MAP));
    TEST_ASSERT_EQUAL_UINT8(0, g_io_window_tracking_active);
    TEST_ASSERT_EQUAL_INT(rc + 1, s_recompute);
}

/* Výběr programově, klikem na buňku (vybere i řádek) a klikem na řádek (vybere jeho první buňku). */
static void test_click_selects_cell_and_row(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0x70, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bool mmio = true;
    TEST_ASSERT_EQUAL_UINT(0x70, bus_window_selected(&mmio));
    TEST_ASSERT_FALSE(mmio);

    float x = 0, y = 0;
    TEST_ASSERT_TRUE(bus_window_cell_center(0x74, &x, &y));
    click_at(&open, x, y);
    TEST_ASSERT_EQUAL_UINT(0x74, bus_window_selected(&mmio));
    TEST_ASSERT_FALSE(mmio);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.row_count >= 2);
    TEST_ASSERT_TRUE(d.selected_row >= 0);
    int dummy_row = d.selected_row;

    /* klik na první řádek seznamu (nejnižší port) vybere jeho první adresu */
    TEST_ASSERT_TRUE(bus_window_row_center(0, &x, &y));
    click_at(&open, x, y);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.selected_row);
    TEST_ASSERT_TRUE(dummy_row != 0);
    TEST_ASSERT_TRUE(bus_window_selected(NULL) < 0x74);

    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    frame_with_window(&open);
}

/* Hover na řádek dummy zařízení zvýrazní v dalším snímku jeho dvě buňky (74h, 75h). */
static void test_hover_row_highlights_cells(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0x74, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.hover_cells);
    float x = 0, y = 0;
    TEST_ASSERT_TRUE(bus_window_row_center(d.selected_row, &x, &y));
    ImGui::GetIO().AddMousePosEvent(x, y);
    for (int i = 0; i < 3; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(2, d.hover_cells);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.hover_cells);
}

/* Vypnuté sledování: řádek aktivity má pomlčky, ne nuly. */
static void test_tracking_off_shows_dash(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0x74, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_NOT_NULL(strstr(d.activity_line, "0"));
    float detail_on = d.detail_h;
    TEST_ASSERT_TRUE(detail_on > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.detail_fixed_h, detail_on);
    io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_NULL(strstr(d.activity_line, "0"));
    TEST_ASSERT_NOT_NULL(strstr(d.activity_line, "-"));
    /* detail má pevnou výšku ve všech stavech (R04): bez sledování i bez vlastníka */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, detail_on, d.detail_h);
    bus_window_select(0x20, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, detail_on, d.detail_h);
    /* adresa bez vlastníka nemá příznak "no present owner" (R08) */
    TEST_ASSERT_EQUAL_STRING("Flags: -", d.flags_line);
}

/* Pod přirozenou šířkou: buňka na minimu, seznam na minimu, nic se nepřeskládá (dva snímky). */
static void test_min_width_no_reflow(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0x74, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    float natural = d.natural_w;
    TEST_ASSERT_TRUE(natural > 0.0f);
    TEST_ASSERT_TRUE(d.cell_min >= 24.0f);

    /* široké okno: buňka roste, seznam není pod minimem */
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, natural + 400.0f, 1000.0f);
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.cell >= d.cell_min);
    TEST_ASSERT_TRUE(d.list_w >= d.list_min_w);
    float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    TEST_ASSERT_TRUE(bus_window_cell_center(0x00, &gx0, &gy0));

    /* okno přesně na přirozené šířce */
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, natural + 16.0f, 1000.0f);
    st_BUS_WINDOW_DEBUG at_natural;
    bus_window_debug_get(&at_natural);
    TEST_ASSERT_TRUE(bus_window_cell_center(0x00, &gx0, &gy0));

    /* zúžené pod minimum: tatáž geometrie jako na přirozené šířce (jen posuvník) */
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, natural - 300.0f, 1000.0f);
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(bus_window_cell_center(0x00, &gx1, &gy1));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, at_natural.cell, d.cell);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, at_natural.list_w, d.list_w);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.list_min_w, d.list_w);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, gx0, gx1);
    TEST_ASSERT_TRUE(d.selected_row >= 0);
    TEST_ASSERT_TRUE(bus_window_row_center(d.selected_row, &gx1, &gy1));
}

/* V pauze se pro vybranou adresu udělá peek jen jednou za výběr a text to říká; za běhu text bez "automatic". */
static void test_peek_when_paused(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0x74, false);
    frame_with_window(&open);
    TEST_ASSERT_EQUAL_INT(0, s_peeks);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_NULL(strstr(d.peek_line, "automatic"));
    g_emulator.paused = true;
    for (int i = 0; i < 3; i++) frame_with_window(&open);
    TEST_ASSERT_EQUAL_INT(1, s_peeks);
    bus_window_debug_get(&d);
    TEST_ASSERT_NOT_NULL(strstr(d.peek_line, "automatic"));
    TEST_ASSERT_NOT_NULL(strstr(d.peek_line, "74h"));
    bus_window_select(0x75, false);
    frame_with_window(&open);
    TEST_ASSERT_EQUAL_INT(2, s_peeks);
    g_emulator.paused = false;
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_NULL(strstr(d.peek_line, "automatic"));
}

/* Záložka MMIO se vykreslí a drží výběr; zrušená záložka Devices (2) = Ports. */
static void test_mmio_tab(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0xE013, true);
    for (int i = 0; i < 3; i++) frame_with_window(&open);
    bool mmio = false;
    TEST_ASSERT_EQUAL_UINT(0xE013, bus_window_selected(&mmio));
    TEST_ASSERT_TRUE(mmio);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(1, d.active_tab);
    TEST_ASSERT_TRUE(d.row_count >= 1);
    /* sloupec rozsahů se v MMIO jmenuje Addresses (P05) */
    TEST_ASSERT_EQUAL_STRING("Addresses", d.ports_header);
    float mmio_natural = d.natural_w, mmio_list = d.list_min_w, mmio_cell = d.cell, mmio_split = d.split_x;
    bus_window_show_tab(2);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.active_tab);
    TEST_ASSERT_EQUAL_STRING("Ports", d.ports_header);
    /* šířka seznamu, buňka a oddělovač jsou pro obě záložky stejné (R05) */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, mmio_natural, d.natural_w);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, mmio_list, d.list_min_w);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, mmio_cell, d.cell);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, mmio_split, d.split_x);
}

/* Persistence: klíče sekce [BUS_MAP_PANEL]; bez INI zůstává po parse + propagate režim Owners (V6). */
static void test_persistence_keys(void)
{
    CFGMOD *m = cfgroot_register_new_module(g_cfgmain, (char *) "BUS_MAP_PANEL");
    TEST_ASSERT_NOT_NULL(m);
    bus_window_register_persistence(m);
    TEST_ASSERT_NOT_NULL(cfgmodule_get_element_by_name(m, (char *) "color_mode"));
    TEST_ASSERT_NOT_NULL(cfgmodule_get_element_by_name(m, (char *) "active_tab"));
    TEST_ASSERT_NOT_NULL(cfgmodule_get_element_by_name(m, (char *) "list_width"));
    TEST_ASSERT_NOT_NULL(cfgmodule_get_element_by_name(m, (char *) "tracking_active"));
    TEST_ASSERT_NULL(cfgmodule_get_element_by_name(m, (char *) "show_legend"));
    TEST_ASSERT_NULL(cfgmodule_get_element_by_name(m, (char *) "inspector_width"));
    cfgmodule_parse(m);
    cfgmodule_propagate(m);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.color_mode);
    if (s_ui_ok) {
        bool open = true;
        for (int i = 0; i < 2; i++) frame_with_window(&open);
        bus_window_debug_get(&d);
        TEST_ASSERT_EQUAL_INT(0, d.color_mode);
    }
}

/* MZ-800 (tabulka testovacího jádra): v nízkém okně seznam roluje a vybraný poslední řádek je vidět. */
static void test_mz800_list_scroll_selected_visible(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    bus_window_select(0xFC, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, 1100.0f, 520.0f);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.row_count >= 15);
    TEST_ASSERT_TRUE(d.visible_rows < d.row_count);
    TEST_ASSERT_EQUAL_INT(d.row_count - 1, d.selected_row);
    float x = 0, y = 0;
    TEST_ASSERT_TRUE(bus_window_row_center(d.selected_row, &x, &y));
    TEST_ASSERT_TRUE(y > 0.0f && y < 520.0f);
}

/* Tooltip ikony stavu i tooltip řádku říkají stav slovem (ve sloupci je jen ikona). */
static void test_state_icon_tooltip(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    ImGuiStyle &st = ImGui::GetStyle();
    float sd = st.HoverStationaryDelay, ds = st.HoverDelayShort, dn = st.HoverDelayNormal;
    st.HoverStationaryDelay = 0.0f; st.HoverDelayShort = 0.0f; st.HoverDelayNormal = 0.0f;
    bool open = true;
    bus_window_select(0x74, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    int row = d.selected_row;
    TEST_ASSERT_TRUE(row >= 0);
    TEST_ASSERT_EQUAL_STRING("", d.state_tip);
    float x = 0, y = 0;
    TEST_ASSERT_TRUE(bus_window_state_icon_center(row, &x, &y));
    ImGui::GetIO().AddMousePosEvent(x, y);
    for (int i = 0; i < 4; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_STRING("State: connected", d.state_tip);
    /* hover ikony drží zvýraznění buněk řádku (dummy 74h-75h) */
    TEST_ASSERT_EQUAL_INT(2, d.hover_cells);
    /* tooltip řádku (sloupec Owner) má stav slovem, tooltip ikony tam není */
    TEST_ASSERT_TRUE(bus_window_row_center(row, &x, &y));
    ImGui::GetIO().AddMousePosEvent(x, y);
    for (int i = 0; i < 4; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_STRING("State: connected", d.row_tip_state);
    TEST_ASSERT_EQUAL_STRING("", d.state_tip);
    /* hover každé vykreslené ikony i řádku: žádná kolize ID (ikona řádku i vs. záhlaví sloupce i) */
    s_id_conflicts = 0;
    for (int r = 0; r < d.row_count; r++) {
        if (!bus_window_state_icon_center(r, &x, &y)) continue;
        ImGui::GetIO().AddMousePosEvent(x, y);
        for (int i = 0; i < 3; i++) frame_with_window(&open);
        TEST_ASSERT_TRUE(bus_window_row_center(r, &x, &y));
        ImGui::GetIO().AddMousePosEvent(x, y);
        for (int i = 0; i < 3; i++) frame_with_window(&open);
    }
    TEST_ASSERT_EQUAL_INT(0, s_id_conflicts);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st.HoverStationaryDelay = sd; st.HoverDelayShort = ds; st.HoverDelayNormal = dn;
}

/* Sbalení detailu šipkou: pevná výška, víc řádků seznamu; persistence detail_open (výchozí 1, uložení, načtení). */
static void test_detail_collapse_persistence(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    CFGMOD *m = cfgroot_register_new_module(g_cfgmain, (char *) "BUS_MAP_PANEL");
    TEST_ASSERT_NOT_NULL(m);
    bus_window_register_persistence(m);
    TEST_ASSERT_NOT_NULL(cfgmodule_get_element_by_name(m, (char *) "detail_open"));
    cfgmodule_parse(m);
    cfgmodule_propagate(m);
    bool open = true;
    const float W = 1100.0f, H = 520.0f;
    bus_window_select(0x74, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.detail_open);
    float h_open = d.detail_h;
    int rows_open = d.visible_rows;
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.detail_fixed_h, h_open);

    float x = 0, y = 0;
    TEST_ASSERT_TRUE(bus_window_detail_toggle_center(&x, &y));
    click_at_sized(&open, x, y, W, H);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_FALSE(d.detail_open);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.detail_fixed_h, d.detail_h);
    TEST_ASSERT_TRUE(d.detail_h < h_open);
    TEST_ASSERT_TRUE(d.visible_rows > rows_open);
    /* sbalený detail má stejnou výšku i bez vlastníka */
    float h_closed = d.detail_h;
    bus_window_select(0x20, false);
    for (int i = 0; i < 2; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, h_closed, d.detail_h);

    /* uložení do INI, rozbalení a načtení uloženého sbaleného stavu */
    cfgroot_save(g_cfgmain);
    gchar *ini = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(s_ini_path, &ini, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(ini, "detail_open"));
    g_free(ini);
    TEST_ASSERT_TRUE(bus_window_detail_toggle_center(&x, &y));
    click_at_sized(&open, x, y, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.detail_open);
    struct st_CFGROOT *r2 = cfgroot_new(s_ini_path);
    CFGMOD *m2 = cfgroot_register_new_module(r2, (char *) "BUS_MAP_PANEL");
    bus_window_register_persistence(m2);
    cfgmodule_parse(m2);
    cfgmodule_propagate(m2);
    bus_window_debug_get(&d);
    TEST_ASSERT_FALSE(d.detail_open);
    cfgroot_destroy(r2);

    /* zpět rozbaleno pro další testy */
    for (int i = 0; i < 2; i++) frame_with_window_sized(&open, W, H);
    TEST_ASSERT_TRUE(bus_window_detail_toggle_center(&x, &y));
    click_at_sized(&open, x, y, W, H);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.detail_open);
}

/* Písmo aplikace 28 px: přirozená šířka okna se vejde do 95 % FullHD (1824 px) bez vodorovného posuvníku. */
static void test_natural_width_fullhd(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    if (!app_font_ready()) TEST_IGNORE_MESSAGE("application font DroidSans.ttf not found");
    if (s_test_lang[0] && strcmp(_("Connect"), "Connect") == 0) TEST_IGNORE_MESSAGE("translation catalog not active");
    s_frame_font = s_app_font;
    bool open = true;
    bus_window_select(0x70, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, 2000.0f, 1000.0f);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    fprintf(stderr, "natural width (%s): content %.1f px, window %.1f px\n", s_test_lang[0] ? s_test_lang : "en", d.natural_w, d.natural_win_w);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 28.0f, d.font_size);
    TEST_ASSERT_TRUE(d.natural_win_w <= 1824.0f);
    /* P04: za nejdelším názvem ve sloupci Owner zbude při přirozené šířce aspoň 0,5 em + mezera sloupců
     * (2 x CellPadding.x) = 22 px při 28 px (2 x největší mezera mezi slovy názvu, 11 px) */
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, d.natural_win_w, 1000.0f);
    bus_window_debug_get(&d);
    float gap = d.ports_col_x - d.owner_col_x - d.owner_text_w;
    fprintf(stderr, "owner gap (%s): name %.1f px, owner x %.1f, ports x %.1f, gap %.1f px\n", s_test_lang[0] ? s_test_lang : "en",
            d.owner_text_w, d.owner_col_x, d.ports_col_x, gap);
    TEST_ASSERT_TRUE(gap >= 22.0f - 0.5f);
    s_frame_font = NULL;
}

/* Písmo aplikace 28 px, výška 720: buňka na mezi čitelnosti V1, mřížka celá právě od min_full_h. */
static void test_cell_shrinks_with_height(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    if (!app_font_ready()) TEST_IGNORE_MESSAGE("application font DroidSans.ttf not found");
    s_frame_font = s_app_font;
    bool open = true;
    bus_window_select(0x70, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, 2000.0f, 1400.0f);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    float W = d.natural_win_w;
    float r_full = d.badge_r;
    /* mez V1: nejširší zkratka portů a popisek řádku + 2 x 2 px, svisle výška písma + 2 x 2 px */
    static st_BUS_MAP_SNAPSHOT snap;
    bus_map_snapshot(&snap, BUS_WANT_ALL);
    frame_begin();
    float widest = ImGui::CalcTextSize("F0").x;
    for (unsigned i = 0; i < 256; i++)
        for (unsigned k = 0; k < snap.ports[i].owner_count; k++) widest = fmaxf(widest, ImGui::CalcTextSize(snap.ports[i].owners[k].abbrev).x);
    float line = ImGui::GetTextLineHeight();
    frame_end();
    TEST_ASSERT_TRUE(d.cell_floor >= widest + 4.0f);
    TEST_ASSERT_TRUE(d.cell_floor >= line + 4.0f);
    TEST_ASSERT_TRUE(d.cell_floor <= d.cell_min);

    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, 720.0f);
    bus_window_debug_get(&d);
    fprintf(stderr, "h720: cell %.1f floor %.1f min %.1f fit_h %.1f grid_full %d min_full_h %.1f badge_r %.2f\n",
            d.cell, d.cell_floor, d.cell_min, d.cell_fit_h, (int) d.grid_full, d.min_full_h, d.badge_r);
    TEST_ASSERT_TRUE(d.cell >= d.cell_floor);
    TEST_ASSERT_TRUE(d.cell < d.cell_min);
    TEST_ASSERT_TRUE(d.badge_r <= r_full);
    TEST_ASSERT_EQUAL_INT(d.min_full_h <= 720.0f, d.grid_full);
    /* přepnutí záložky při zmenšené buňce nic neposune (R05): mez je ze zkratek obou záložek */
    float ports_cell = d.cell, ports_split = d.split_x;
    bus_window_select(0xE004, true);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, 720.0f);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(1, d.active_tab);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, ports_cell, d.cell);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, ports_split, d.split_x);
    bus_window_select(0x70, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, 720.0f);

    /* mřížka celá právě od min_full_h */
    float hmin = d.min_full_h;
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, hmin);
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.grid_full);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.cell_floor, d.cell);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, hmin - 1.0f);
    bus_window_debug_get(&d);
    TEST_ASSERT_FALSE(d.grid_full);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.cell_floor, d.cell);
    s_frame_font = NULL;
}

/* P01: pravá část dojede k celému detailu. Pro výšky okna od minima do přirozené výšky (písmo 28 px, 100 % i 175 %):
 * spodní hrana Break on OUT <= spodní hrana ##bus_right, nebo má pravá část viditelný posuvník (scroll_max_y > 0;
 * samotné scroll_max_y nestačí, s NoScrollbar je > 0 i bez posuvníku) a kolečko nad detailem ji odroluje tak,
 * že tlačítko je celé vidět. Při přirozené výšce posuvník není. */
static void test_right_part_reaches_detail_buttons(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    if (!app_font_ready()) TEST_IGNORE_MESSAGE("application font DroidSans.ttf not found");
    s_frame_font = s_app_font;
    const ImGuiStyle saved = ImGui::GetStyle();
    const float scales[2] = { 1.0f, 1.75f };
    bool open = true;
    st_BUS_WINDOW_DEBUG d;
    for (float scale : scales) {
        ImGui::GetStyle() = saved;
        if (scale != 1.0f) {
            ImGui::GetStyle().ScaleAllSizes(scale);
            ImGui::GetStyle().FontScaleMain = scale;
        }
        bus_window_select(0x70, false);
        for (int i = 0; i < 3; i++) frame_with_window_sized(&open, 2000.0f * scale, 1400.0f * scale);
        bus_window_debug_get(&d);
        TEST_ASSERT_FLOAT_WITHIN(0.5f, 28.0f * scale, d.font_size);
        TEST_ASSERT_TRUE(d.detail_open);
        const float W = d.natural_win_w, h_min = d.min_win_h, h_nat = d.natural_h;
        TEST_ASSERT_TRUE(h_min > 0.0f && h_nat > h_min);
        const float step = roundf(5.0f * scale);
        int scrolled = 0, checked = 0, failures = 0;
        float first_fit = -1.0f;
        for (float h = h_min; h <= h_nat + 0.5f; h += step) {
            for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, h);
            bus_window_debug_get(&d);
            const bool fits = d.btn_bottom <= d.right_bottom + 0.5f;
            const bool scrollbar = d.right_scrollbar && d.right_scroll_max_y > 0.0f;
            if (!fits && !scrollbar) {
                if (failures++ < 3) {
                    fprintf(stderr, "P01 fail: scale %.2f h %.0f btn_bottom %.1f right_bottom %.1f scrollbar %d scroll_max %.1f list_h %.1f\n",
                            scale, h, d.btn_bottom, d.right_bottom, (int) d.right_scrollbar, d.right_scroll_max_y, d.list_h);
                }
                continue;
            }
            if (scrollbar) {
                scrolled++;
                /* seznam zůstává na nejmenší výšce (záhlaví + 3 řádky) */
                TEST_ASSERT_FLOAT_WITHIN(0.5f, d.list_need_h, d.list_h);
                /* kolečkem nad detailem (šipka v titulku) se dojede k tlačítkům */
                float x = 0, y = 0;
                if ((checked++ % 8) == 0 && bus_window_detail_toggle_center(&x, &y) && y < d.right_bottom) {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    frame_with_window_sized(&open, W, h);
                    ImGui::GetIO().AddMouseWheelEvent(0.0f, -100.0f);
                    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, h);
                    bus_window_debug_get(&d);
                    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.right_scroll_max_y, d.right_scroll_y);
                    TEST_ASSERT_TRUE(d.btn_bottom <= d.right_bottom + 0.5f);
                    ImGui::GetIO().AddMouseWheelEvent(0.0f, 100.0f);
                    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
                    for (int i = 0; i < 2; i++) frame_with_window_sized(&open, W, h);
                }
            } else if (first_fit < 0.0f) {
                first_fit = h;
            }
        }
        fprintf(stderr, "P01 scale %.2f: window h %.0f..%.0f, scrollbar at %d heights, failures %d, no scrollbar from h %.0f\n",
                scale, h_min, h_nat, scrolled, failures, first_fit);
        for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, h_nat);
        bus_window_debug_get(&d);
        fprintf(stderr, "P01 scale %.2f at natural height: btn_bottom %.1f right_bottom %.1f scrollbar %d\n",
                scale, d.btn_bottom, d.right_bottom, (int) d.right_scrollbar);
        TEST_ASSERT_EQUAL_INT(0, failures);
        /* při minimu okna se seznam s detailem nevejde (jinak test nic nezkouší), při přirozené výšce ano a bez posuvníku */
        TEST_ASSERT_TRUE(scrolled > 0);
        TEST_ASSERT_FALSE(d.right_scrollbar);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, d.right_scroll_max_y);
        TEST_ASSERT_TRUE(d.btn_bottom <= d.right_bottom + 0.5f);
    }
    ImGui::GetStyle() = saved;
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    s_frame_font = NULL;
}

/* Výběr adresy, jejíž řádek seznamu je celý vidět (klik do mřížky, křížový skok, klik na řádek), seznam
 * neodroluje; řádek mimo pohled se dorovná do pohledu. Dřív SetScrollHereY(0.5) vystředilo i viditelný řádek
 * a odrolování zůstalo i po zvětšení okna (posudek D8, seznam o 32 px v nižším okně). */
static void test_select_visible_row_keeps_list_scroll(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool open = true;
    const float W = 1100.0f, H = 640.0f;
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_TRUE(d.visible_rows < d.row_count);
    /* seznam nahoru kolečkem (stav tabulky přetrvává z předchozích testů) */
    float x = 0, y = 0;
    int first = -1, last = -1;
    for (int r = 0; r < d.row_count; r++) if (bus_window_row_center(r, NULL, NULL)) { if (first < 0) first = r; last = r; }
    TEST_ASSERT_TRUE(first >= 0);
    TEST_ASSERT_TRUE(bus_window_row_center(first, &x, &y));
    ImGui::GetIO().AddMousePosEvent(x, y);
    frame_with_window_sized(&open, W, H);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 100.0f);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.list_scroll_y);
    first = last = -1;
    for (int r = 0; r < d.row_count; r++) if (bus_window_row_center(r, NULL, NULL)) { if (first < 0) first = r; last = r; }
    /* řádek v dolní polovině seznamu, celý vidět (poslední vykreslený může být useknutý) */
    const int target = last - 1;
    TEST_ASSERT_TRUE(target > first + 2);
    /* adresa řádku: klik na řádek vybere jeho první adresu */
    float y0 = 0, y1 = 0;
    TEST_ASSERT_TRUE(bus_window_row_center(target, &x, &y0));
    click_at_sized(&open, x, y0, W, H);
    unsigned addr = bus_window_selected(NULL);
    TEST_ASSERT_TRUE(bus_window_row_center(first, &x, &y));
    click_at_sized(&open, x, y, W, H);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(first, d.selected_row);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.list_scroll_y);
    /* výběr adresy viditelného řádku (jako klik do mřížky): seznam se nehýbe */
    bus_window_select(addr, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(target, d.selected_row);
    TEST_ASSERT_TRUE(bus_window_row_center(target, &x, &y1));
    fprintf(stderr, "select visible row: list_h %.1f, row %d, scroll %.1f, row y %.1f -> %.1f\n", d.list_h, target, d.list_scroll_y, y0, y1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.list_scroll_y);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, y0, y1);
    /* vybraný řádek mimo pohled (poslední) se dorovná do pohledu */
    bus_window_select(0xFC, false);
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(d.row_count - 1, d.selected_row);
    TEST_ASSERT_TRUE(d.list_scroll_y > 0.0f);
    TEST_ASSERT_TRUE(bus_window_row_center(d.selected_row, &x, &y1));
    TEST_ASSERT_TRUE(y1 > 0.0f && y1 < H);
    /* nižší okno = kratší seznam: vybraný poslední řádek se dorovná (seznam odroluje dál), ne zůstane pod hranou */
    const float scroll_tall = d.list_scroll_y, list_tall = d.list_h;
    for (int i = 0; i < 3; i++) frame_with_window_sized(&open, W, H - 120.0f);
    bus_window_debug_get(&d);
    fprintf(stderr, "shrink: list_h %.1f -> %.1f, scroll %.1f -> %.1f\n", list_tall, d.list_h, scroll_tall, d.list_scroll_y);
    TEST_ASSERT_TRUE(d.list_h < list_tall);
    TEST_ASSERT_TRUE(d.list_scroll_y > scroll_tall + 0.5f);
}

/* P02, P03: záhlaví sloupce ikony má značku "(?)" s tooltipem tvarů zalomeným na 35 em. */
static void test_state_header_tooltip(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    ImGuiStyle &st = ImGui::GetStyle();
    float sd = st.HoverStationaryDelay, ds = st.HoverDelayShort, dn = st.HoverDelayNormal;
    st.HoverStationaryDelay = 0.0f; st.HoverDelayShort = 0.0f; st.HoverDelayNormal = 0.0f;
    bool open = true;
    bus_window_select(0x74, false);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st_BUS_WINDOW_DEBUG d;
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.hdr_tip_w);
    float x = 0, y = 0, ix = 0;
    TEST_ASSERT_TRUE(bus_window_state_header_center(&x, &y));
    TEST_ASSERT_TRUE(bus_window_state_icon_center(d.selected_row, &ix, NULL));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, ix, x); /* značka nad ikonami */
    s_id_conflicts = 0;
    ImGui::GetIO().AddMousePosEvent(x, y);
    for (int i = 0; i < 4; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    fprintf(stderr, "header tooltip: width %.1f px, limit %.1f px\n", d.hdr_tip_w, d.font_size * 35.0f + st.WindowPadding.x * 2.0f);
    TEST_ASSERT_TRUE(d.hdr_tip_w > 0.0f);
    TEST_ASSERT_TRUE(d.hdr_tip_w <= d.font_size * 35.0f + st.WindowPadding.x * 2.0f);
    TEST_ASSERT_EQUAL_INT(0, s_id_conflicts);
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    st.HoverStationaryDelay = sd; st.HoverDelayShort = ds; st.HoverDelayNormal = dn;
}

/* ================================================================
 * Režim smoke screenshotů
 * ================================================================ */

static bool shot_save(const char *dir, const char *name)
{
    size_t n = (size_t) s_canvas_w * (size_t) s_canvas_h * 4u;
    unsigned char *px = (unsigned char *) malloc(n);
    unsigned char *flipped = (unsigned char *) malloc(n);
    if (!px || !flipped) { free(px); free(flipped); return false; }
    glReadPixels(0, 0, s_canvas_w, s_canvas_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    size_t pitch = (size_t) s_canvas_w * 4u;
    for (int y = 0; y < s_canvas_h; y++) memcpy(flipped + (size_t) y * pitch, px + (size_t) (s_canvas_h - 1 - y) * pitch, pitch);
    SDL_Surface *surf = SDL_CreateSurfaceFrom(s_canvas_w, s_canvas_h, SDL_PIXELFORMAT_RGBA32, flipped, (int) pitch);
    bool ok = false;
    if (surf) {
        char *path = g_build_filename(dir, name, NULL);
        ok = SDL_SaveBMP(surf, path);
        if (!ok) fprintf(stderr, "SDL_SaveBMP(%s): %s\n", path, SDL_GetError());
        g_free(path);
        SDL_DestroySurface(surf);
    }
    free(px);
    free(flipped);
    return ok;
}

static st_DEVBUS_DUMMY s_b, s_c;
static st_DEVBUS_DEVICE_DESC s_desc_b, s_desc_c;
static st_DEVBUS_PORT_RANGE s_ports_b[1];

/** Sekvence stavů okna do BMP; vrací počet neuložených snímků. */
static int shots_run(const char *dir)
{
    /* tři zařízení: K&P-like na 70h-73h (odpojené), dummy 74h-75h (připojené), MMIO-only (odpojené) */
    setUp();
    memset(&s_b, 0, sizeof(s_b));
    memset(&s_c, 0, sizeof(s_c));
    devbus_dummy_make_desc(&s_desc_b, "kp80", "KP80", 0);
    s_ports_b[0] = s_desc_b.ports[0];
    s_ports_b[0].first = 0x70; s_ports_b[0].last = 0x73;
    s_desc_b.ports = s_ports_b; s_desc_b.port_count = 1;
    s_desc_b.name = "K&P 80 Column Card";
    s_desc_b.mmio_count = 0;
    s_b.slot = devbus_register(&s_desc_b, &s_b);
    devbus_dummy_make_desc(&s_desc_c, "suc80", "SUC80", 0);
    s_desc_c.name = "SUC 80 kit";
    s_desc_c.port_count = 0;
    s_c.slot = devbus_register(&s_desc_c, &s_c);

    int failed = 0;
    bool open = true;
    float W = (float) s_canvas_w - 20.0f, H = (float) s_canvas_h - 20.0f;
    auto settle = [&](int n, float w, float h) { for (int i = 0; i < n; i++) frame_with_window_sized(&open, w, h); };
    auto take = [&](const char *name) { if (!shot_save(dir, name)) failed++; };
    st_BUS_WINDOW_DEBUG d;
    /* Geometrie mřížky pro měřicí skript nad snímkem (okno je v (0, 0), souřadnice = pixely snímku). */
    auto geo = [&](const char *name) {
        bus_window_debug_get(&d);
        fprintf(stderr, "geo: %s font %.1f cell %.1f gap %.1f grid_x %.1f grid_y %.1f badge_r %.2f badge_off %.1f cell_min %.1f cell_floor %.1f grid_full %d min_full_h %.1f natural_w %.1f natural_win_w %.1f detail_open %d detail_h %.1f visible_rows %d rows %d\n",
                name, d.font_size, d.cell, d.gap, d.grid_x, d.grid_y, d.badge_r, d.badge_off, d.cell_min, d.cell_floor, (int) d.grid_full,
                d.min_full_h, d.natural_w, d.natural_win_w, (int) d.detail_open, d.detail_h, d.visible_rows, d.row_count);
    };

    bus_window_select(0x70, false);
    settle(4, W, H);
    bus_window_debug_get(&d);
    /* okno = přirozená šířka obsahu + 2 x okraj okna (při škále je okraj větší než 8 px) */
    float natural = d.natural_w + ImGui::GetStyle().WindowPadding.x * 2.0f;
    float nat_h = H;
    settle(3, natural, nat_h);
    take("01-default.bmp");
    geo("01-default");

    float x = 0, y = 0;
    if (bus_window_row_center(d.selected_row, &x, &y)) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(3, natural, nat_h);
        take("02-hover-row.bmp");
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    }
    if (bus_window_cell_center(0x5A, &x, &y)) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(40, natural, nat_h); /* tooltip po prodlevě */
        take("03-cell-tooltip.bmp");
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    }
    bus_window_debug_set(2, -1);
    settle(3, natural, nat_h);
    take("04-drive.bmp");
    bus_window_debug_set(1, -1);
    settle(3, natural, nat_h);
    take("05-activity.bmp");
    bus_window_debug_set(0, 0);
    settle(3, natural, nat_h);
    take("06-tracking-off.bmp");
    bus_window_debug_set(0, 1);
    bus_window_select(0xE004, true);
    settle(3, natural, nat_h);
    take("07-mmio.bmp");
    bus_window_select(0x20, false);
    settle(3, natural, nat_h);
    take("08-no-owner.bmp");
    bus_window_select(0x70, false);
    settle(3, natural - 320.0f, nat_h);
    take("09-narrow.bmp");
    g_emulator.paused = true;
    settle(3, natural, nat_h);
    take("10-paused.bmp");
    g_emulator.paused = false;
    bus_window_select(0xFC, false);
    settle(3, natural, nat_h * 0.7f);
    take("11-scroll-last-row.bmp");
    bus_window_select(0x70, false);
    settle(3, natural + 150.0f, nat_h);
    take("12-wide.bmp");
    settle(3, natural, 720.0f);
    take("13-h720.bmp");
    geo("13-h720");
    /* totéž při šířce 1885 bodů (kritérium plánu D8; buňka závisí jen na výšce) */
    settle(3, 1885.0f, 720.0f);
    geo("h720-w1885");
    settle(3, natural, 720.0f);
    bus_window_debug_get(&d);
    fprintf(stderr, "metrics: font %.1f cell %.1f cell_min %.1f natural_w %.1f list_min_w %.1f row_h %.1f rows %d visible_rows@720 %d detail_h %.1f fixed %.1f window_w %.1f\n",
            ImGui::GetFontSize(), d.cell, d.cell_min, d.natural_w, d.list_min_w, d.row_h, d.row_count, d.visible_rows, d.detail_h, d.detail_fixed_h, natural);
    settle(3, natural, nat_h);
    bus_window_debug_get(&d);
    ImGui::GetIO().AddMousePosEvent(d.split_x, d.split_y);
    settle(3, natural, nat_h);
    take("14-splitter-hover.bmp");
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);

    /* D8: sbalený detail při výšce 720 (klik na šipku), pak zpět rozbalit */
    auto click = [&](float cx, float cy, float w, float h) {
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(cx, cy);
        settle(1, w, h);
        io.AddMouseButtonEvent(0, true);
        settle(1, w, h);
        io.AddMouseButtonEvent(0, false);
        settle(1, w, h);
        io.AddMousePosEvent(-1.0f, -1.0f);
        settle(3, w, h);
    };
    bus_window_select(0x70, false);
    settle(3, natural, 720.0f);
    if (bus_window_detail_toggle_center(&x, &y)) {
        click(x, y, natural, 720.0f);
        take("15-h720-collapsed.bmp");
        geo("15-h720-collapsed");
        if (bus_window_detail_toggle_center(&x, &y)) click(x, y, natural, 720.0f);
    }
    /* D8: nejmenší výška s celou mřížkou (min_full_h) a o 1 px méně */
    settle(3, natural, nat_h);
    bus_window_debug_get(&d);
    float hmin = d.min_full_h;
    settle(3, natural, hmin);
    take("16-min-full-h.bmp");
    geo("16-min-full-h");
    settle(3, natural, hmin - 1.0f);
    geo("min-full-h-minus-1");
    /* D8: ikony stavu, hover ikony nepřipojeného zařízení (K&P-like) s tooltipem */
    bus_window_select(0x70, false);
    settle(3, natural, nat_h);
    bus_window_debug_get(&d);
    if (bus_window_state_icon_center(d.selected_row, &x, &y)) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(40, natural, nat_h); /* tooltip po prodlevě */
        take("17-state-icons.bmp");
        bus_window_debug_get(&d);
        fprintf(stderr, "state_tip: '%s'\n", d.state_tip);
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    }
    /* Opravná vlna D8: tooltip záhlaví sloupce ikony (P03) */
    settle(3, natural, nat_h);
    if (bus_window_state_header_center(&x, &y)) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(40, natural, nat_h);
        take("18-header-tooltip.bmp");
        bus_window_debug_get(&d);
        fprintf(stderr, "hdr_tip: width %.1f px (35 em = %.1f px)\n", d.hdr_tip_w, d.font_size * 35.0f);
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    }
    /* Opravná vlna D8: rozložení seznamu (P04) při přirozené šířce */
    settle(3, natural, nat_h);
    bus_window_debug_get(&d);
    fprintf(stderr, "cols: owner_x %.1f owner_text_w %.1f ports_x %.1f gap %.1f header '%s'\n",
            d.owner_col_x, d.owner_text_w, d.ports_col_x, d.ports_col_x - d.owner_col_x - d.owner_text_w, d.ports_header);
    /* Opravná vlna D8: nízké okno (P01) - výška 720 a minimum okna, posuvník pravé části nahoře a dole */
    auto right_state = [&](const char *name) {
        bus_window_debug_get(&d);
        fprintf(stderr, "right: %s natural_h %.0f list_h %.1f list_need %.1f scrollbar %d scroll %.1f / %.1f btn_bottom %.1f right_bottom %.1f\n",
                name, d.natural_h, d.list_h, d.list_need_h, (int) d.right_scrollbar, d.right_scroll_y, d.right_scroll_max_y, d.btn_bottom, d.right_bottom);
    };
    auto wheel_down = [&](float w, float h) {
        if (!bus_window_detail_toggle_center(&x, &y)) return;
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(1, w, h);
        ImGui::GetIO().AddMouseWheelEvent(0.0f, -100.0f);
        settle(2, w, h);
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
        settle(3, w, h);
    };
    auto wheel_up = [&](float w, float h) {
        if (!bus_window_detail_toggle_center(&x, &y)) return;
        ImGui::GetIO().AddMousePosEvent(x, y);
        settle(1, w, h);
        ImGui::GetIO().AddMouseWheelEvent(0.0f, 100.0f);
        settle(2, w, h);
        ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
        settle(3, w, h);
    };
    bus_window_select(0x70, false);
    settle(3, natural, 720.0f);
    right_state("h720");
    wheel_down(natural, 720.0f);
    take("19-h720-right-scrolled.bmp");
    right_state("h720-scrolled");
    wheel_up(natural, 720.0f);
    bus_window_debug_get(&d);
    float hwin = d.min_win_h;
    settle(3, natural, hwin);
    take("20-min-h.bmp");
    right_state("min-h");
    wheel_down(natural, hwin);
    take("21-min-h-scrolled.bmp");
    right_state("min-h-scrolled");
    wheel_up(natural, hwin);
    tearDown();
    return failed;
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    const char *shot_dir = getenv("MZ_BUS_SHOT_DIR");
    if (shot_dir && *shot_dir) {
        const char *ev;
        if ((ev = getenv("MZ_BUS_SHOT_W")) && atoi(ev) > 0) s_canvas_w = atoi(ev);
        if ((ev = getenv("MZ_BUS_SHOT_H")) && atoi(ev) > 0) s_canvas_h = atoi(ev);
        const char *lang = getenv("MZ_BUS_SHOT_LANG");
        const char *ldir = getenv("MZ_BUS_SHOT_LOCALEDIR");
        if (lang && *lang && ldir && *ldir) {
            static char lang_env[64];
            snprintf(lang_env, sizeof(lang_env), "LANGUAGE=%s", lang);
            putenv(lang_env);
            setlocale(LC_ALL, "");
            bindtextdomain(I18N_TEXTDOMAIN, ldir);
            bind_textdomain_codeset(I18N_TEXTDOMAIN, "UTF-8");
            textdomain(I18N_TEXTDOMAIN);
        }
        s_ui_ok = ui_setup();
        if (!s_ui_ok) { fprintf(stderr, "SDL/GL not available\n"); return 1; }
        ImGuiIO &io = ImGui::GetIO();
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        float scale = (ev = getenv("MZ_BUS_SHOT_SCALE")) ? (float) atof(ev) : 1.0f;
        if (scale <= 0.0f) scale = 1.0f;
        const char *font = getenv("MZ_BUS_SHOT_FONT");
        if (font && *font) io.Fonts->AddFontFromFileTTF(font, 28.0f);
        const char *mono = getenv("MZ_BUS_SHOT_MONO");
        if (mono && *mono) s_mono = io.Fonts->AddFontFromFileTTF(mono, 28.0f);
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
        /* FBO velikosti plátna */
        fn_gen_fb gen_fb = (fn_gen_fb) SDL_GL_GetProcAddress("glGenFramebuffers");
        fn_fb_tex2d fb_tex = (fn_fb_tex2d) SDL_GL_GetProcAddress("glFramebufferTexture2D");
        fn_check_fb check_fb = (fn_check_fb) SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        s_bind_fb = (fn_bind_fb) SDL_GL_GetProcAddress("glBindFramebuffer");
        if (gen_fb && fb_tex && check_fb && s_bind_fb) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_canvas_w, s_canvas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gen_fb(1, &s_fbo);
            s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
            fb_tex(TEST_GL_FRAMEBUFFER, TEST_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            if (check_fb(TEST_GL_FRAMEBUFFER) != TEST_GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); s_fbo = 0; }
        } else {
            fprintf(stderr, "FBO functions not available, reading the back buffer\n");
            s_bind_fb = NULL;
        }
        int failed = shots_run(shot_dir);
        ui_teardown();
        mztest_teardown();
        return failed ? 1 : 0;
    }

    /* Běh v jiném jazyce (registrace ui-bus-window-de): jen test šířky, ostatní testy čtou anglické texty. */
    const char *tl = getenv("MZ_BUS_TEST_LANG");
    if (tl && *tl) {
        static char lang_env[64];
        snprintf(lang_env, sizeof(lang_env), "LANGUAGE=%s", tl);
        putenv(lang_env);
        setlocale(LC_ALL, "");
        if (MZTEST_LOCALE_DIR[0]) {
            bindtextdomain(I18N_TEXTDOMAIN, MZTEST_LOCALE_DIR);
            bind_textdomain_codeset(I18N_TEXTDOMAIN, "UTF-8");
            textdomain(I18N_TEXTDOMAIN);
        }
        s_test_lang = tl;
        s_ui_ok = ui_setup();
        UNITY_BEGIN();
        RUN_TEST(test_natural_width_fullhd);
        int lres = UNITY_END();
        ui_teardown();
        mztest_teardown();
        return lres;
    }

    s_ui_ok = ui_setup();

    UNITY_BEGIN();
    RUN_TEST(test_render_frames_and_tracking);
    RUN_TEST(test_click_selects_cell_and_row);
    RUN_TEST(test_hover_row_highlights_cells);
    RUN_TEST(test_tracking_off_shows_dash);
    RUN_TEST(test_min_width_no_reflow);
    RUN_TEST(test_peek_when_paused);
    RUN_TEST(test_mmio_tab);
    RUN_TEST(test_persistence_keys);
    RUN_TEST(test_mz800_list_scroll_selected_visible);
    RUN_TEST(test_state_icon_tooltip);
    RUN_TEST(test_detail_collapse_persistence);
    RUN_TEST(test_natural_width_fullhd);
    RUN_TEST(test_cell_shrinks_with_height);
    RUN_TEST(test_right_part_reaches_detail_buttons);
    RUN_TEST(test_select_visible_row_keeps_list_scroll);
    RUN_TEST(test_state_header_tooltip);
    int result = UNITY_END();

    ui_teardown();
    mztest_teardown();
    return result;
}
