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
 *   se sledováním, bez něj i bez vlastníka (R04);
 * - pod přirozenou šířkou se nic nepřeskládá (buňka i seznam na minimu, dva snímky);
 * - peek v pauze automaticky s textem "(automatic while paused)", za běhu bez něj;
 * - záložka MMIO, zrušená záložka Devices (2) = Ports; šířka seznamu, buňka
 *   a oddělovač shodné pro obě záložky (R05);
 * - persistence: klíče color_mode, active_tab, list_width, tracking_active,
 *   bez show_legend a inspector_width; po parse + propagate bez INI zůstává
 *   Owners (V6);
 * - MZ-800: seznam má víc řádků než se vejde, vybraný poslední řádek je vidět.
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

#include "libs/imgui/imgui.h"
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
#include "../peripherals/devbus_dummy.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/busmap/bus_window.h"
#include "i18n.h"

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
}

static void frame_end(void)
{
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
    float mmio_natural = d.natural_w, mmio_list = d.list_min_w, mmio_cell = d.cell, mmio_split = d.split_x;
    bus_window_show_tab(2);
    for (int i = 0; i < 2; i++) frame_with_window(&open);
    bus_window_debug_get(&d);
    TEST_ASSERT_EQUAL_INT(0, d.active_tab);
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

    bus_window_select(0x70, false);
    settle(4, W, H);
    bus_window_debug_get(&d);
    /* okno = přirozená šířka obsahu + 2 x okraj okna (při škále je okraj větší než 8 px) */
    float natural = d.natural_w + ImGui::GetStyle().WindowPadding.x * 2.0f;
    float nat_h = H;
    settle(3, natural, nat_h);
    take("01-default.bmp");

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
    bus_window_debug_get(&d);
    fprintf(stderr, "metrics: font %.1f cell %.1f cell_min %.1f natural_w %.1f list_min_w %.1f row_h %.1f rows %d visible_rows@720 %d detail_h %.1f fixed %.1f window_w %.1f\n",
            ImGui::GetFontSize(), d.cell, d.cell_min, d.natural_w, d.list_min_w, d.row_h, d.row_count, d.visible_rows, d.detail_h, d.detail_fixed_h, natural);
    settle(3, natural, nat_h);
    bus_window_debug_get(&d);
    ImGui::GetIO().AddMousePosEvent(d.split_x, d.split_y);
    settle(3, natural, nat_h);
    take("14-splitter-hover.bmp");
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
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
    int result = UNITY_END();

    ui_teardown();
    mztest_teardown();
    return result;
}
