/*
 * test_cmt_tools.cpp - nástroje nad páskou okna Virtual CMT:
 * modál Tape information (src/ui-imgui/cmt/cmt_tapeinfo_dialog.cpp),
 * nemodální okno Tape check (cmt_check_window.cpp), modál Convert to
 * profile (cmt_convert_dialog.cpp) a nápověda k analýze po otevření pásky
 * s jediným surovým blokem. Skutečné ImGui ve skrytém okně SDL + GL nad
 * skutečným jádrem kazety a frontou zásahů.
 *
 * Snímek = kreslení okna Virtual CMT (imgui_cmt_window(), viditelnost
 * g_gui->showVirtualCmtWindow), okna Tape check (imgui_cmt_check_window()),
 * imgui_cmt_tape_dialogs(), device_request_poll() (emu vlákno) a vyřízení
 * výsledků (imgui_device_request_results()) - stejné pořadí jako
 * main_window.cpp.
 *
 * Pokrytí: vstupy (menu Tape, kontextové menu řádku)
 * a jejich zákazy s důvodem; formulář načte hodnoty z jádra; OK beze změny
 * nic nepošle; se změnou pošle zápis, Enter v poli = OK, varování jádra jde
 * do řádku zpráv; věta o formátu u pásky z MZF a věta o dalším bloku
 * informací; změna pásky zakáže OK a nechá úpravy, odmítnutí zápisu kvůli
 * změně pásky dialog nechá otevřený; okno kontroly: souhrn, řádky, klik
 * vybere blok (okno kontroly zůstane navrchu), čtení jen při otevření
 * a Check again, zastaralé nálezy po změně pásky, páska bez nálezů; převod:
 * seznam profilů s aktuálním, rychlost jen u profilu, který ji přijímá,
 * zásah s ID a rychlostí, zpráva po převodu nebo varování jádra, "Nothing to change.";
 * jedno místo převodu čísel bloků z textů jádra (cmt_view_core_text());
 * nápověda k analýze; geometrie při 100 % a 175 %, texty o 40 % delší.
 * Dále: Reload po výměně pásky (výzva, titulek),
 * zastaralé nálezy po připojení záznamu, značka [unverified] v popisu
 * profilu, modály přežijí snímek bez obsahu okna kazety.
 *
 * Režim snímků (mimo CTest): MZ_CMT_SHOT_DIR = adresář; jako ui-cmt-analyze
 * MZ_CMT_SHOT_FONT, MZ_CMT_SHOT_MONO, MZ_CMT_SHOT_SCALE, MZ_CMT_SHOT_LANG
 * + MZ_CMT_SHOT_LOCALEDIR, MZ_CMT_SHOT_W / _H, MZ_CMT_SHOT_STRETCH=1.
 * Sada j5: info (vyplněný formulář), info-format (páska z MZF a další blok
 * informací), check (nálezy), check-none, check-outdated, convert-speed,
 * convert-nospeed. Jména "j5-<jazyk>-<měřítko v %>-<stav>.png".
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
#include <string>
#include <vector>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

extern "C" {
#include "mztest.h"
#include "device_request.h"
#include "replay.h"
#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_meta.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "snapshot/snapshot_request.h"
#include "debugger/png_encode.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/mzf/mzf.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/cmt/cmt_window.h"
#include "ui-imgui/cmt/cmt_tokens.h"
#include "ui-imgui/cmt/cmt_tapeinfo_dialog.h"
#include "ui-imgui/cmt/cmt_convert_dialog.h"
#include "ui-imgui/cmt/cmt_check_window.h"
#include "ui-imgui/cmt/cmt_analyze_dialog.h"
#include "ui-imgui/devices/device_request_ui.h"
#include "hw-generic/cmt/cmt_request.h"
#include "ui-imgui/filechooser/res/CustomFont.cpp"
#include "ui-imgui/filechooser/res/CustomFont.h"
#include "i18n.h"

#ifndef MZTEST_FONT_DIR
#define MZTEST_FONT_DIR ""
#endif

/* UI globál, který jinak vytváří sdlapp_imgui_video.cpp. */
static MyImGui s_gui;
MyImGui *g_gui = &s_gui;

/* ================================================================
 * Stuby UI funkcí jiných modulů (test je nelinkuje)
 * ================================================================ */

extern "C" void snapshot_ui_request_load_now(void) {}
extern "C" void snapshot_ui_request_quickload_now(void) {}
extern "C" void snapshot_ui_load_cancelled(void) {}
extern "C" void imgui_replay_open_play_dialog_now(void) {}
extern "C" void imgui_replay_start_play(const char *path) { (void)path; }
extern "C" void imgui_replay_stop_play(void) {}
extern "C" void snapshot_ui_submit(en_SNAPSHOT_REQ_KIND kind, const char *path, const char *description)
{
    (void)kind;
    (void)path;
    (void)description;
}
extern "C" bool imgui_unicard_device_request_result(const st_DEVICE_REQ_RESULT *res)
{
    (void)res;
    return false;
}

/* ================================================================
 * SDL3 + GL + ImGui
 * ================================================================ */

static SDL_Window *s_win = NULL;
static SDL_GLContext s_gl = NULL;
static ImGuiContext *s_ctx = NULL;
static bool s_ui_ok = false;
static int s_canvas_w = 2400, s_canvas_h = 1800;

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

/* ================================================================
 * Pásky testu
 * ================================================================ */

/** @brief Dočasný soubor pásky (maže tearDown). */
static char *s_path = NULL;

/** @brief Hlavička MZF (pořadí hostitele). */
static void make_hdr(st_MZF_HEADER *hdr, const char *name, uint8_t ftype, uint16_t fsize)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = ftype;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, strlen(name));
    hdr->fsize = fsize;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
}

/** @brief Přidá blok 0x46 (MZ800EMU-CMT, 1:1) s MZF do souboru. */
static void add_mzf(st_TZX_FILE *f, const char *name, uint16_t fsize)
{
    st_MZF_HEADER hdr;
    make_hdr(&hdr, name, 0x01, fsize);
    uint8_t *body = (uint8_t *)malloc(fsize);
    for (int i = 0; i < fsize; i++) body[i] = (uint8_t)(i * 3 + 1);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, fsize, CMTSPEED_1_1, NULL, 1000);
    free(body);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
}

/** @brief Přidá blok s daty (kopie), např. řídicí blok TZX. */
static void add_raw(st_TZX_FILE *f, uint8_t id, const uint8_t *data, uint32_t len)
{
    st_TZX_BLOCK b;
    b.id = id;
    b.length = len;
    b.data = NULL;
    if (len) {
        b.data = (uint8_t *)malloc(len);
        memcpy(b.data, data, len);
    }
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Přidá surový blok 0x18 s @p n pulzy (krátké a dlouhé jako Sharp). */
static void add_csw(st_TZX_FILE *f, uint32_t n)
{
    uint32_t *p = (uint32_t *)malloc(n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) p[i] = (i % 4u == 0u) ? 4430u : 2215u;
    st_TZX_BLOCK raw;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(GDGCLK_BASE / 2, 1000, p, n, &raw));
    free(p);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &raw));
}

/** @brief Přidá blok 0x32 s jedinou položkou Title. */
static void add_archive_title(st_TZX_FILE *f, const char *title)
{
    uint8_t buf[300];
    size_t len = strlen(title);
    uint32_t pos = 3;
    buf[pos++] = 0x00;
    buf[pos++] = (uint8_t)len;
    memcpy(buf + pos, title, len);
    pos += (uint32_t)len;
    buf[0] = (uint8_t)((pos - 2) & 0xFF);
    buf[1] = (uint8_t)((pos - 2) >> 8);
    buf[2] = 1;
    add_raw(f, TZX_BLOCK_ID_ARCHIVE_INFO, buf, pos);
}

/** @brief Nový soubor s hlavičkou TMZ. */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/** @brief Cesta dočasného souboru pásky v novém adresáři. @param file Jméno souboru. */
static void new_path(const char *file)
{
    gchar *dir = g_strdup_printf("mztest_cmt_tools_%u", (unsigned)g_random_int());
    gchar *full = g_build_filename(g_get_tmp_dir(), dir, NULL);
    g_mkdir_with_parents(full, 0700);
    g_free(s_path);
    s_path = g_build_filename(full, file, NULL);
    g_free(dir);
    g_free(full);
}

/** @brief Uloží soubor (uvolní ho) do s_path; @p open = otevřít jako pásku. */
static void save_tape(st_TZX_FILE *f, const char *file, bool open = true)
{
    new_path(file);
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    TEST_ASSERT_TRUE(g_file_set_contents(s_path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    if (!open) return;
    device_state_lock();
    int rc = cmt_open_file(s_path, NULL);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
}

/** @brief Druh pásky testu. */
typedef enum en_TT_TAPE {
    TT_TAPE_INFO = 0, /**< tools.tmz: informace (0x32, 0x43 MZ-800), LOADER, Raw recording, GAME */
    TT_TAPE_PLAIN,    /**< plain.tmz: LOADER, Raw recording, GAME (bez informací) */
    TT_TAPE_FINDINGS, /**< check.tmz: A, Loop end bez začátku, B, Group start bez konce, C */
    TT_TAPE_MZF,      /**< game.mzf: jeden soubor MZF (formát informace neunese) */
    TT_TAPE_OTHER_INFO, /**< other.tmz: LOADER, GAME, blok 0x32 na konci (formulář ho nevidí) */
} en_TT_TAPE;

/**
 * @brief Uloží a otevře pásku odd.tmz s jedním blokem 0x46 profilu, který registr nezná ("MZ800EMU-XYZ").
 *
 * Kontrola pásky k němu dá nález se závažností Info ("Profile ... is not known");
 * převod nemá aktuální profil v nabídce.
 */
static void open_odd_profile_tape(void)
{
    st_TZX_FILE *f = new_tmz();
    st_MZF_HEADER hdr;
    make_hdr(&hdr, "ODD", 0x01, 64);
    uint8_t body[64];
    for (int i = 0; i < 64; i++) body[i] = (uint8_t)i;
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 64, CMTSPEED_1_1, NULL, 1000);
    TEST_ASSERT_NOT_NULL(b);
    /* ID profilu stejné délky, které registr nezná */
    bool patched = false;
    for (uint32_t i = 0; i + 12 <= b->length; i++) {
        if (memcmp(b->data + i, "MZ800EMU-CMT", 12) != 0) continue;
        memcpy(b->data + i, "MZ800EMU-XYZ", 12);
        patched = true;
        break;
    }
    TEST_ASSERT_TRUE_MESSAGE(patched, "profile ID found in the block");
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
    save_tape(f, "odd.tmz");
}

/** @brief Zapíše informace o pásce přímo jádrem (příprava pásky, ne cesta okna). */
static void core_set_info(const char *title, const char *year, const char *language, const char *comment, int machine)
{
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    snprintf(info.title, sizeof(info.title), "%s", title);
    snprintf(info.year, sizeof(info.year), "%s", year);
    snprintf(info.language, sizeof(info.language), "%s", language);
    snprintf(info.comment, sizeof(info.comment), "%s", comment);
    info.machine = machine;
    int changed = 0;
    int rc = cmt_tape_info_set(&info, &changed);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
}

/** @brief Informace o pásce přímo z jádra. */
static st_CMTTMZ_TAPE_INFO core_info(void)
{
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    return info;
}

/** @brief Otevře pásku testu. */
static void open_test_tape(en_TT_TAPE t)
{
    st_TZX_FILE *f = NULL;
    switch (t) {
        case TT_TAPE_INFO:
        case TT_TAPE_PLAIN:
            f = new_tmz();
            add_mzf(f, "LOADER", 64);
            add_csw(f, 2000);
            add_mzf(f, "GAME", 64);
            save_tape(f, t == TT_TAPE_INFO ? "tools.tmz" : "plain.tmz");
            if (t == TT_TAPE_INFO)
                core_set_info("Games collection 1", "1986", "English", "Recorded from the original cassette.\nSide A", TMZ_MACHINE_MZ800);
            break;
        case TT_TAPE_FINDINGS:
            f = new_tmz();
            add_mzf(f, "A", 64);
            add_raw(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
            add_mzf(f, "B", 64);
            {
                const uint8_t grp[] = {1, 'G'};
                add_raw(f, TZX_BLOCK_ID_GROUP_START, grp, 2);
            }
            add_mzf(f, "C", 64);
            save_tape(f, "check.tmz");
            break;
        case TT_TAPE_MZF:
        {
            st_MZF_HEADER hdr;
            make_hdr(&hdr, "GAME", 0x01, 64);
            uint8_t file[sizeof(st_MZF_HEADER) + 64];
            memcpy(file, &hdr, sizeof(hdr));
            for (int i = 0; i < 64; i++) file[sizeof(hdr) + i] = (uint8_t)i;
            new_path("game.mzf");
            TEST_ASSERT_TRUE(g_file_set_contents(s_path, (const gchar *)file, sizeof(file), NULL));
            device_state_lock();
            int rc = cmt_open_file(s_path, NULL);
            device_state_unlock();
            TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
            break;
        }
        case TT_TAPE_OTHER_INFO:
            f = new_tmz();
            add_mzf(f, "LOADER", 64);
            add_mzf(f, "GAME", 64);
            add_archive_title(f, "Side B");
            save_tape(f, "other.tmz");
            break;
    }
}

/** @brief Kopie stavu z jádra (pod zámkem). */
static void fill(st_CMT_VIEW *v)
{
    device_state_lock();
    cmt_view_fill(v);
    device_state_unlock();
}

/** @brief Čítač výměn pásky z jádra. */
static unsigned tape_serial(void)
{
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    return tv.tape_serial;
}

/** @brief ID profilu bloku z jádra ("" = žádný). */
static std::string block_profile(int id)
{
    char buf[64] = "";
    device_state_lock();
    int rc = cmttmz_block_profile(id, buf, sizeof(buf));
    device_state_unlock();
    return rc == EXIT_SUCCESS ? std::string(buf) : std::string();
}

/* ================================================================
 * Snímky okna a dialogů
 * ================================================================ */

static ImFont *s_mono = NULL;
/* Stub: okno bere velké počítadlo a čísla z monospace písma aplikace. */
ImFont *myimgui_get_monospace_font(void) { return s_mono; }

#define TT_W_ID "###virtual_cmt"
#define TT_CHECK_ID "###cmt_tape_check"

/** @brief Snímek provede i zásahy fronty (device_request_poll()). */
static bool s_poll = true;
/** @brief Náhradní velikost displeje (0 = okno SDL testu, plátno 2400 x 1800). */
static ImVec2 s_display = ImVec2(0.0f, 0.0f);
/** @brief Kopie stavu, kterou test plní každý snímek (zpráva a její úroveň, tt_view()). */
static st_CMT_VIEW s_tv;
/** @brief Písma už načtená. */
static bool s_fonts_done = false;

/** @brief Načte písma aplikace (DroidSans a Cousine 28 px) jednou; bez souborů výchozí písmo ImGui. */
static void tt_fonts(void)
{
    if (s_fonts_done) return;
    s_fonts_done = true;
    if (!MZTEST_FONT_DIR[0]) return;
    ImGuiIO &io = ImGui::GetIO();
    char *path = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        static const ImWchar ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
        ImFont *f = io.Fonts->AddFontFromFileTTF(path, 28.0f, NULL, ranges);
        if (f) io.FontDefault = f;
    }
    g_free(path);
    path = g_build_filename(MZTEST_FONT_DIR, "Cousine-Regular.ttf", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) s_mono = io.Fonts->AddFontFromFileTTF(path, 28.0f);
    g_free(path);
}

static void tt_scale(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

/**
 * @brief Jeden snímek aplikace v pořadí main_window.cpp.
 * @param render true = vykreslit do GL (snímky).
 */
static void tt_frame(bool render = false)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    /* náhradní velikost displeje (hlavní viewport a jeho pracovní plocha, např. 1920 x 1040) */
    if (s_display.x > 0.0f) ImGui::GetIO().DisplaySize = s_display;
    ImGui::NewFrame();
    if (ImGui::FindWindowByName(TT_W_ID)) ImGui::SetWindowPos(TT_W_ID, ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    device_state_lock();
    if (g_gui->showVirtualCmtWindow) imgui_cmt_window(&g_gui->showVirtualCmtWindow);
    imgui_cmt_check_window();
    imgui_cmt_tape_dialogs();
    device_state_unlock();
    if (s_poll) device_request_poll();
    imgui_device_request_results();
    /* kopie stavu testu: zpráva a její úroveň (tt_view()) */
    device_state_lock();
    cmt_view_fill(&s_tv);
    device_state_unlock();
    ImGui::Render();
    if (render) {
        if (s_bind_fb && s_fbo) {
            s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
            glViewport(0, 0, s_canvas_w, s_canvas_h);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

static void tt_frames(int n)
{
    for (int i = 0; i < n; i++) tt_frame();
}

typedef const std::vector<st_CMT_WINDOW_TEST_ITEM> &(*fn_items)(void);

/** @brief Prvek seznamu podle jména. */
static const st_CMT_WINDOW_TEST_ITEM *find_item(fn_items f, const std::string &name)
{
    for (const st_CMT_WINDOW_TEST_ITEM &it : f())
        if (it.name == name) return &it;
    return NULL;
}

static const st_CMT_WINDOW_TEST_ITEM *w_item(const std::string &n) { return find_item(imgui_cmt_window_test_items, n); }
static const st_CMT_WINDOW_TEST_ITEM *i_item(const std::string &n) { return find_item(cmt_tapeinfo_dialog_test_items, n); }
static const st_CMT_WINDOW_TEST_ITEM *c_item(const std::string &n) { return find_item(cmt_check_window_test_items, n); }
static const st_CMT_WINDOW_TEST_ITEM *v_item(const std::string &n) { return find_item(cmt_convert_dialog_test_items, n); }

/** @brief Text prvku ("" = nebyl). */
static std::string item_text(const st_CMT_WINDOW_TEST_ITEM *it)
{
    return it ? it->text : std::string();
}

/** @brief Jméno prvku s indexem ("ck_row_2"). */
static std::string nm(const char *base, int i)
{
    return std::string(base) + "_" + std::to_string(i);
}

/** @brief Klik na střed prvku. */
static void tt_click_item(const st_CMT_WINDOW_TEST_ITEM *it, int button = 0)
{
    TEST_ASSERT_NOT_NULL(it);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    tt_frame();
    io.AddMouseButtonEvent(button, true);
    tt_frame();
    io.AddMouseButtonEvent(button, false);
    tt_frame();
    tt_frame();
}

static void w_click(const std::string &name, int button = 0)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name.c_str());
    tt_click_item(it, button);
}

/** @brief Stisk klávesy. */
static void tt_key(ImGuiKey key)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    tt_frame();
    io.AddKeyEvent(key, false);
    tt_frame();
    tt_frame();
}

/** @brief Myš pryč z oken. */
static void tt_mouse_away(void)
{
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    tt_frame();
}

/** @brief Zahodí zásahy fronty bez provedení. */
static void tt_drain(void)
{
    while (device_request_test_take_pending(NULL, NULL)) {}
}

/** @brief Najde zařazený zásah daného jména (fronta se vyprázdní). */
static bool tt_take_request(const char *name, st_DEVICE_REQ_ARGS *args)
{
    bool found = false;
    const char *n;
    st_DEVICE_REQ_ARGS a;
    while (device_request_test_take_pending(&n, &a)) {
        if (!found && n && strcmp(n, name) == 0) {
            found = true;
            if (args) *args = a;
        }
    }
    return found;
}

/** @brief Okno ve Full vlevo nahoře, 30 x 23 výšek rámečku, páska testu otevřená. */
static void tt_setup(en_TT_TAPE tape = TT_TAPE_INFO, float scale = 1.0f, bool stretch = false)
{
    tt_fonts();
    tt_scale(scale);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    imgui_cmt_window_test_set_stretch(stretch);
    imgui_cmt_window_test_reset();
    cmt_tapeinfo_dialog_test_reset();
    cmt_convert_dialog_test_reset();
    cmt_check_window_test_reset();
    g_gui->showVirtualCmtWindow = true;
    s_poll = true;
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_reset(&s_tv);
    open_test_tape(tape);
    tt_frames(3);
    float F = ImGui::GetFrameHeight();
    /* s náhradním displejem (s_display) nejvýš jeho velikost, jako omezení okna aplikace pracovní plochou */
    ImVec2 wsz(30.0f * F, 23.0f * F);
    if (s_display.x > 0.0f) wsz = ImVec2(ImMin(wsz.x, s_display.x), ImMin(wsz.y, s_display.y));
    ImGui::SetWindowSize(TT_W_ID, wsz, ImGuiCond_Always);
    tt_frames(4);
}

/** @brief Menu Tape > položka (klik). */
static void tt_menu(const char *item)
{
    w_click("menu_tape");
    w_click(item);
    tt_mouse_away();
    tt_frames(3);
}

/** @brief Otevře Tape information z menu Tape. */
static void tt_open_info(void)
{
    tt_menu("menu_tapeinfo");
    TEST_ASSERT_TRUE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "Tape information... opens the dialog");
}

/** @brief Otevře převod z kontextového menu řádku @p block. */
static void tt_open_convert(int block)
{
    w_click(nm("name", block), 1);
    w_click("ctx_convert");
    tt_mouse_away();
    tt_frames(3);
    TEST_ASSERT_TRUE_MESSAGE(cmt_convert_dialog_is_open(), "Convert to profile... opens the dialog");
}

/**
 * @brief Kopie stavu okna se zprávou řádku zpráv (úroveň zprávy).
 *
 * Nová kopie zprávu z doby před svým prvním plněním nepřevezme (čítač zpráv),
 * proto test drží vlastní kopii s_tv, kterou plní každý snímek (tt_frame()).
 *
 * @return Kopie stavu.
 */
static st_CMT_VIEW tt_view(void)
{
    return s_tv;
}

void setUp(void)
{
    g_replay_mode = REPLAY_MODE_OFF;
    cmt_view_test_force_window_shown(-1);
    tt_drain();
}

void tearDown(void)
{
    if (device_state_lock_held()) device_state_unlock();
    g_replay_mode = REPLAY_MODE_OFF;
    tt_drain();
    {
        st_DEVICE_REQ_RESULT res;
        while (device_request_take_result(&res)) {}
    }
    if (s_ctx) {
        g_gui->showVirtualCmtWindow = true;
        cmt_tapeinfo_dialog_test_reset();
        cmt_convert_dialog_test_reset();
        cmt_check_window_test_reset();
        imgui_cmt_window_test_set_stretch(false);
        imgui_cmt_window_test_set_work_size(ImVec2(0.0f, 0.0f));
        s_display = ImVec2(0.0f, 0.0f);
        /* sbalené okno kazety (test snímku bez obsahu) nesmí přejít do dalšího testu */
        if (ImGui::FindWindowByName(TT_W_ID)) ImGui::SetWindowCollapsed(TT_W_ID, false);
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    device_state_lock();
    cmt_stop();
    cmt_eject();
    device_state_unlock();
    if (s_path) {
        gchar *dir = g_path_get_dirname(s_path);
        g_remove(s_path);
        g_rmdir(dir);
        g_free(dir);
        g_free(s_path);
        s_path = NULL;
    }
}

/* ================================================================
 * Testy
 * ================================================================ */

/*
 * Vstupy a zákazy: menu Tape > Tape information... (jen ve STOP, s páskou,
 * mimo záznam a přehrávání vstupů) a Tape > Check tape (každý stav
 * transportu i replay, jen s páskou); kontextové menu řádku Convert to
 * profile... za Analyze... (ta je hned za Set speed), zakázané u surového bloku, při více
 * vybraných, v PLAY a při záznamu i přehrávání vstupů.
 */
void test_j5_entry_points(void)
{
    tt_setup(TT_TAPE_PLAIN);
    /* menu Tape ve STOP */
    w_click("menu_tape");
    const st_CMT_WINDOW_TEST_ITEM *ti = w_item("menu_tapeinfo");
    const st_CMT_WINDOW_TEST_ITEM *ck = w_item("menu_check");
    TEST_ASSERT_NOT_NULL_MESSAGE(ti, "Tape menu has Tape information...");
    TEST_ASSERT_NOT_NULL_MESSAGE(ck, "Tape menu has Check tape");
    TEST_ASSERT_EQUAL_STRING("Tape information...", ti->text.c_str());
    TEST_ASSERT_EQUAL_STRING("Check tape", ck->text.c_str());
    TEST_ASSERT_FALSE(ti->disabled);
    TEST_ASSERT_FALSE(ck->disabled);
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    /* kontextové menu: Convert to profile... hned za Analyze... (Analyze... je za Set speed) */
    w_click("name_0", 1);
    const st_CMT_WINDOW_TEST_ITEM *cv = w_item("ctx_convert");
    TEST_ASSERT_NOT_NULL_MESSAGE(cv, "context menu has Convert to profile...");
    TEST_ASSERT_EQUAL_STRING("Convert to profile...", cv->text.c_str());
    TEST_ASSERT_FALSE(cv->disabled);
    TEST_ASSERT_TRUE_MESSAGE(w_item("ctx_speed")->max.y <= w_item("ctx_analyze")->min.y + 0.5f, "Analyze... is after Set speed");
    TEST_ASSERT_TRUE_MESSAGE(w_item("ctx_analyze")->max.y <= cv->min.y + 0.5f, "Convert to profile... is after Analyze...");
    TEST_ASSERT_TRUE_MESSAGE(cv->max.y <= w_item("ctx_move_up")->min.y + 0.5f, "Convert to profile... is before Move up");
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    /* surový blok: převést nejde */
    w_click("name_1", 1);
    cv = w_item("ctx_convert");
    TEST_ASSERT_TRUE(cv->disabled);
    TEST_ASSERT_EQUAL_STRING("This block cannot be converted to another profile.", cv->reason.c_str());
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    /* víc vybraných řádků */
    imgui_cmt_window_test_set_selected({0, 2}, tape_serial());
    tt_frames(2);
    w_click("name_0", 1);
    cv = w_item("ctx_convert");
    TEST_ASSERT_TRUE(cv->disabled);
    TEST_ASSERT_EQUAL_STRING("Select a single block to convert it.", cv->reason.c_str());
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    imgui_cmt_window_test_set_selection(-1, tape_serial());
    /* PLAY: informace a převod zakázané, kontrola povolená */
    device_state_lock();
    cmt_play();
    device_state_unlock();
    tt_frames(2);
    w_click("menu_tape");
    TEST_ASSERT_TRUE(w_item("menu_tapeinfo")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w_item("menu_tapeinfo")->reason.c_str());
    TEST_ASSERT_FALSE_MESSAGE(w_item("menu_check")->disabled, "Check tape works while the tape plays");
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    w_click("name_0", 1);
    TEST_ASSERT_TRUE(w_item("ctx_convert")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w_item("ctx_convert")->reason.c_str());
    tt_key(ImGuiKey_Escape);
    tt_mouse_away();
    device_state_lock();
    cmt_stop();
    device_state_unlock();
    tt_frames(2);
    /* záznam a přehrávání vstupů: informace a převod zakázané, kontrola (přímé čtení) povolená */
    const int modes[2] = {REPLAY_MODE_RECORDING, REPLAY_MODE_PLAYING};
    for (int mode : modes) {
        g_replay_mode = mode;
        tt_frames(2);
        w_click("menu_tape");
        TEST_ASSERT_TRUE_MESSAGE(w_item("menu_tapeinfo")->disabled, "Tape information... disabled during an input replay");
        TEST_ASSERT_FALSE(w_item("menu_tapeinfo")->reason.empty());
        TEST_ASSERT_FALSE_MESSAGE(w_item("menu_check")->disabled, "Check tape works during an input replay");
        tt_key(ImGuiKey_Escape);
        tt_mouse_away();
        w_click("name_0", 1);
        TEST_ASSERT_TRUE_MESSAGE(w_item("ctx_convert")->disabled, "Convert disabled during an input replay");
        tt_key(ImGuiKey_Escape);
        tt_mouse_away();
        g_replay_mode = REPLAY_MODE_OFF;
    }
    /* bez pásky: obě položky zakázané */
    device_state_lock();
    cmt_eject();
    device_state_unlock();
    tt_frames(2);
    w_click("menu_tape");
    TEST_ASSERT_TRUE(w_item("menu_tapeinfo")->disabled);
    TEST_ASSERT_TRUE(w_item("menu_check")->disabled);
    TEST_ASSERT_EQUAL_STRING("No tape inserted.", w_item("menu_check")->reason.c_str());
    tt_key(ImGuiKey_Escape);
}

/*
 * Formulář načte hodnoty z jádra: titulek s jménem pásky, deset polí
 * v pořadí SPEC (Comment víceřádkový se zalomením), Target machine MZ-800;
 * bez věty o formátu (TMZ) a bez věty o dalším bloku informací.
 */
void test_j5_info_form_reads_values(void)
{
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    TEST_ASSERT_EQUAL_STRING("Tape information - tools.tmz###cmt_tapeinfo", cmt_tapeinfo_dialog_test_title());
    static const char *labels[10] = {"Title", "Publisher", "Author", "Year", "Language", "Type", "Price", "Protection", "Origin", "Comment"};
    for (int k = 0; k < 10; k++) TEST_ASSERT_EQUAL_STRING(labels[k], item_text(i_item(nm("ti_label", k))).c_str());
    TEST_ASSERT_EQUAL_STRING("Games collection 1", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_EQUAL_STRING("", item_text(i_item("ti_field_1")).c_str());
    TEST_ASSERT_EQUAL_STRING("1986", item_text(i_item("ti_field_3")).c_str());
    TEST_ASSERT_EQUAL_STRING("English", item_text(i_item("ti_field_4")).c_str());
    TEST_ASSERT_EQUAL_STRING("Recorded from the original cassette.\nSide A", item_text(i_item("ti_field_9")).c_str());
    TEST_ASSERT_EQUAL_STRING("Target machine", item_text(i_item("ti_machine_label")).c_str());
    TEST_ASSERT_EQUAL_STRING("MZ-800", item_text(i_item("ti_machine")).c_str());
    /* pořadí shora: pole, Target machine, tlačítka */
    TEST_ASSERT_TRUE(i_item("ti_field_0")->max.y <= i_item("ti_field_1")->min.y + 0.5f);
    TEST_ASSERT_TRUE(i_item("ti_field_9")->max.y <= i_item("ti_machine")->min.y + 0.5f);
    TEST_ASSERT_TRUE(i_item("ti_machine")->max.y <= i_item("ti_ok")->min.y + 0.5f);
    TEST_ASSERT_TRUE_MESSAGE(i_item("ti_field_9")->max.y - i_item("ti_field_9")->min.y > 2.0f * (i_item("ti_field_0")->max.y - i_item("ti_field_0")->min.y),
                             "Comment is a multi-line field");
    TEST_ASSERT_EQUAL_STRING("OK", item_text(i_item("ti_ok")).c_str());
    TEST_ASSERT_EQUAL_STRING("Cancel", item_text(i_item("ti_cancel")).c_str());
    TEST_ASSERT_NULL_MESSAGE(i_item("ti_format_note"), "a TMZ tape keeps the information");
    TEST_ASSERT_NULL(i_item("ti_other_note"));
    TEST_ASSERT_FALSE(i_item("ti_ok")->disabled);
    /* Cancel zavře */
    tt_click_item(i_item("ti_cancel"));
    TEST_ASSERT_FALSE(cmt_tapeinfo_dialog_is_open());
}

/*
 * OK beze změny nic nezařadí (chová se jako Cancel); páska beze změny.
 * Escape zavře dialog s úpravami bez dotazu.
 */
void test_j5_info_ok_without_change(void)
{
    tt_setup(TT_TAPE_INFO);
    const unsigned serial = tape_serial();
    tt_open_info();
    s_poll = false;
    tt_drain();
    tt_click_item(i_item("ti_ok"));
    TEST_ASSERT_FALSE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "OK without a change closes the dialog");
    TEST_ASSERT_FALSE_MESSAGE(tt_take_request(CMT_REQUEST_TAPE_INFO_SET_NAME, NULL), "OK without a change sends nothing");
    /* úprava a vrácení na původní text = beze změny */
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Other");
    cmt_tapeinfo_dialog_test_set_text(0, "Games collection 1");
    tt_frames(2);
    tt_click_item(i_item("ti_ok"));
    TEST_ASSERT_FALSE(tt_take_request(CMT_REQUEST_TAPE_INFO_SET_NAME, NULL));
    /* Escape s neuloženou úpravou: zavře, nic nepošle */
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(1, "Publisher X");
    tt_frames(2);
    tt_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_FALSE(tt_take_request(CMT_REQUEST_TAPE_INFO_SET_NAME, NULL));
    s_poll = true;
    tt_frames(2);
    TEST_ASSERT_EQUAL_UINT(serial, tape_serial());
}

/*
 * OK se změnou zařadí zápis s čítačem pásky a správnými daty; po provedení
 * jádro nese nové hodnoty (cizí pole beze změny), dialog je zavřený
 * a řádek zpráv říká "Tape information updated."; Enter v jednořádkovém
 * poli = OK; varování jádra (znaky mimo ISO 8859-1) jde do řádku zpráv.
 */
void test_j5_info_ok_sends_changes(void)
{
    tt_setup(TT_TAPE_INFO);
    const unsigned serial = tape_serial();
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Games collection 2");
    cmt_tapeinfo_dialog_test_set_text(2, "Somebody");
    cmt_tapeinfo_dialog_test_set_machine(TMZ_MACHINE_MZ700);
    tt_frames(2);
    s_poll = false;
    tt_drain();
    tt_click_item(i_item("ti_ok"));
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE_MESSAGE(tt_take_request(CMT_REQUEST_TAPE_INFO_SET_NAME, &a), "OK with a change queues the write");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)serial, a.i[5], "the write carries the tape serial of the form");
    /* zásah zahozený: dialog dál čeká; zavřít a znovu, tentokrát s provedením */
    cmt_tapeinfo_dialog_test_reset();
    s_poll = true;
    tt_frames(2);
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Games collection 2");
    cmt_tapeinfo_dialog_test_set_text(2, "Somebody");
    cmt_tapeinfo_dialog_test_set_machine(TMZ_MACHINE_MZ700);
    tt_frames(2);
    tt_click_item(i_item("ti_ok"));
    tt_frames(3);
    TEST_ASSERT_FALSE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "the dialog closes after a successful write");
    st_CMTTMZ_TAPE_INFO info = core_info();
    TEST_ASSERT_EQUAL_STRING("Games collection 2", info.title);
    TEST_ASSERT_EQUAL_STRING("Somebody", info.author);
    TEST_ASSERT_EQUAL_STRING("1986", info.year);
    TEST_ASSERT_EQUAL_STRING("Recorded from the original cassette.\nSide A", info.comment);
    TEST_ASSERT_EQUAL_INT(TMZ_MACHINE_MZ700, info.machine);
    TEST_ASSERT_EQUAL_STRING("Tape information updated.", item_text(w_item("message")).c_str());
    /* Enter v jednořádkovém poli = OK */
    tt_open_info();
    tt_click_item(i_item("ti_field_4"));
    ImGui::GetIO().AddInputCharacter('!');
    tt_frame();
    tt_key(ImGuiKey_Enter);
    tt_frames(3);
    TEST_ASSERT_FALSE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "Enter in a single-line field = OK");
    TEST_ASSERT_EQUAL_STRING("English!", core_info().language);
    /* varování jádra do řádku zpráv (varování) */
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(1, "\xC5\x98" "eho\xC5\x99");
    tt_frames(2);
    tt_click_item(i_item("ti_ok"));
    tt_frames(3);
    TEST_ASSERT_FALSE(cmt_tapeinfo_dialog_is_open());
    st_CMT_VIEW tv = tt_view();
    TEST_ASSERT_EQUAL_STRING("Characters outside ISO 8859-1 were replaced with '?' in: publisher", tv.message.c_str());
    TEST_ASSERT_TRUE(tv.message_is_warning);
    TEST_ASSERT_EQUAL_STRING("?eho?", core_info().publisher);
}

/*
 * Věta o formátu u pásky z MZF (storable 0) a ztlumená věta o dalším bloku
 * informací dál na pásce.
 */
void test_j5_info_notes(void)
{
    tt_setup(TT_TAPE_MZF);
    tt_open_info();
    TEST_ASSERT_EQUAL_STRING("This tape file format cannot store this information. Save the tape as TMZ to keep it.",
                             item_text(i_item("ti_format_note")).c_str());
    TEST_ASSERT_NULL(i_item("ti_other_note"));
    TEST_ASSERT_EQUAL_STRING("Not specified", item_text(i_item("ti_machine")).c_str());
    TEST_ASSERT_TRUE(i_item("ti_machine")->max.y <= i_item("ti_format_note")->min.y + 0.5f);
    TEST_ASSERT_TRUE(i_item("ti_format_note")->max.y <= i_item("ti_ok")->min.y + 0.5f);
    tearDown();
    tt_setup(TT_TAPE_OTHER_INFO);
    tt_open_info();
    TEST_ASSERT_NULL(i_item("ti_format_note"));
    TEST_ASSERT_EQUAL_STRING("This tape has another information block further on; it is left unchanged.",
                             item_text(i_item("ti_other_note")).c_str());
    TEST_ASSERT_EQUAL_STRING("", item_text(i_item("ti_field_0")).c_str());
}

/*
 * Změna pásky: (a) před OK - OK zakázané s výzvou k Reload, úpravy zůstanou;
 * (b) zápis odmítnutý jádrem kvůli změně pásky mezi zařazením a provedením -
 * dialog zůstane otevřený s úpravami a výzvou; (c) Reload
 * přečte pásku znovu: pole, která uživatel změnil, zůstanou, ostatní
 * převezmou nové hodnoty, OK pak zapíše nad aktuální páskou.
 */
void test_j5_info_tape_changed_keeps_edits(void)
{
    const char *changed = "The tape changed. Press Reload to read it again; your changes are kept.";
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "My edit");
    tt_frames(2);
    TEST_ASSERT_NULL_MESSAGE(i_item("ti_reload"), "no Reload while the tape is unchanged");
    /* páska se změní jinou cestou (jádro) */
    core_set_info("Changed elsewhere", "1990", "English", "", TMZ_MACHINE_MZ800);
    tt_frames(3);
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_TRUE(i_item("ti_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING(changed, i_item("ti_ok")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING(changed, item_text(i_item("ti_status")).c_str());
    TEST_ASSERT_EQUAL_STRING("My edit", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(i_item("ti_reload"), "Reload offered after the tape changed");
    tt_click_item(i_item("ti_cancel"));
    /* (b) odmítnutí při provedení */
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Second edit");
    tt_frames(2);
    s_poll = false;
    tt_click_item(i_item("ti_ok"));
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    /* páska se změní, než emu vlákno zásah provede */
    core_set_info("Changed again", "1991", "German", "", TMZ_MACHINE_MZ800);
    s_poll = true;
    tt_frames(4);
    TEST_ASSERT_TRUE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "a write refused because the tape changed keeps the dialog open");
    TEST_ASSERT_EQUAL_STRING("Second edit", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_EQUAL_STRING(changed, item_text(i_item("ti_status")).c_str());
    TEST_ASSERT_NULL_MESSAGE(i_item("ti_error"), "the refusal is shown once (status line)");
    TEST_ASSERT_TRUE(i_item("ti_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING("Changed again", core_info().title);
    /* (c) Reload: úprava titulu zůstane, rok a jazyk převezme z pásky */
    tt_click_item(i_item("ti_reload"));
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING("Second edit", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_EQUAL_STRING("1991", item_text(i_item("ti_field_3")).c_str());
    TEST_ASSERT_EQUAL_STRING("German", item_text(i_item("ti_field_4")).c_str());
    TEST_ASSERT_NULL(i_item("ti_status"));
    TEST_ASSERT_NULL(i_item("ti_reload"));
    TEST_ASSERT_FALSE(i_item("ti_ok")->disabled);
    tt_click_item(i_item("ti_ok"));
    tt_frames(3);
    TEST_ASSERT_FALSE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "OK after Reload writes over the current tape");
    st_CMTTMZ_TAPE_INFO info = core_info();
    TEST_ASSERT_EQUAL_STRING("Second edit", info.title);
    TEST_ASSERT_EQUAL_STRING("1991", info.year);
    TEST_ASSERT_EQUAL_STRING("German", info.language);
}

/*
 * Páska vyměněná při otevřeném formuláři informací (jiné
 * jméno a cesta, jako MCP emu_cmt_open) - výzva řekne, že v decku je jiná
 * páska, titulek do Reload jmenuje pásku, ze které je formulář; Reload
 * převezme jméno nové pásky do titulku, úprava pole zůstane. Změna téže
 * pásky dá dál původní výzvu.
 */
void test_finalb_info_reload_other_tape(void)
{
    const char *other = "A different tape is in the deck now. Press Reload to read it; your changes are kept.";
    const char *changed = "The tape changed. Press Reload to read it again; your changes are kept.";
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    TEST_ASSERT_EQUAL_STRING("Tape information - tools.tmz###cmt_tapeinfo", cmt_tapeinfo_dialog_test_title());
    cmt_tapeinfo_dialog_test_set_text(0, "My edit");
    tt_frames(2);
    /* jiná páska do decku jinou cestou; soubor první pásky už není potřeba */
    gchar *old_path = g_strdup(s_path);
    st_TZX_FILE *f = new_tmz();
    add_mzf(f, "OTHER", 64);
    save_tape(f, "other.tmz");
    {
        gchar *dir = g_path_get_dirname(old_path);
        g_remove(old_path);
        g_rmdir(dir);
        g_free(dir);
        g_free(old_path);
    }
    tt_frames(3);
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING(other, item_text(i_item("ti_status")).c_str());
    TEST_ASSERT_TRUE(i_item("ti_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING(other, i_item("ti_ok")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Tape information - tools.tmz###cmt_tapeinfo", cmt_tapeinfo_dialog_test_title(),
                                     "until Reload the title names the tape the form was read from");
    tt_click_item(i_item("ti_reload"));
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Tape information - other.tmz###cmt_tapeinfo", cmt_tapeinfo_dialog_test_title(),
                                     "Reload takes the name of the tape in the deck");
    TEST_ASSERT_EQUAL_STRING("My edit", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", item_text(i_item("ti_field_3")).c_str(), "fields the user did not change come from the new tape");
    TEST_ASSERT_NULL(i_item("ti_status"));
    TEST_ASSERT_FALSE(i_item("ti_ok")->disabled);
    /* téže pásce se změní informace jinou cestou: výzva o změně, ne o jiné pásce */
    core_set_info("Changed elsewhere", "1990", "", "", -1);
    tt_frames(3);
    TEST_ASSERT_EQUAL_STRING(changed, item_text(i_item("ti_status")).c_str());
    TEST_ASSERT_EQUAL_STRING("Tape information - other.tmz###cmt_tapeinfo", cmt_tapeinfo_dialog_test_title());
}

/*
 * Snímek, ve kterém okno kazety nekreslí obsah (Begin() vrátí false; tady okno sbalené
 * kódem - s otevřeným modálem ho uživatel sbalit nemůže), modály okna nezavře: formulář informací i dialog
 * převodu zůstanou se svým stavem, okno se v dalším snímku samo rozbalí (skrytý modál by blokoval vstup)
 * a modál se kreslí dál.
 */
void test_finalb_modals_kept_without_window_content(void)
{
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Kept while collapsed");
    tt_frames(2);
    ImGui::SetWindowCollapsed(TT_W_ID, true);
    tt_frame();
    TEST_ASSERT_TRUE_MESSAGE(ImGui::FindWindowByName(TT_W_ID)->Collapsed, "the Virtual CMT window drew no content in this frame");
    TEST_ASSERT_TRUE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "a frame without window content does not cancel the dialog");
    tt_frames(3);
    TEST_ASSERT_FALSE_MESSAGE(ImGui::FindWindowByName(TT_W_ID)->Collapsed, "the window expands again while its modal is open");
    TEST_ASSERT_TRUE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Kept while collapsed", item_text(i_item("ti_field_0")).c_str(), "the edits are kept");
    TEST_ASSERT_TRUE_MESSAGE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel), "the modal is open");
    /* OK zapíše úpravu ze stavu před snímkem bez obsahu */
    tt_click_item(i_item("ti_ok"));
    tt_frames(3);
    TEST_ASSERT_FALSE(cmt_tapeinfo_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING("Kept while collapsed", core_info().title);

    /* převod: vybraný profil zůstane */
    tt_open_convert(2); /* blok LOADER za dvěma bloky informací (0x32, 0x43) */
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("ROM-800-NOMINAL"));
    tt_frames(2);
    ImGui::SetWindowCollapsed(TT_W_ID, true);
    tt_frame();
    TEST_ASSERT_TRUE(ImGui::FindWindowByName(TT_W_ID)->Collapsed);
    TEST_ASSERT_TRUE_MESSAGE(cmt_convert_dialog_is_open(), "a frame without window content does not cancel the conversion dialog");
    tt_frames(3);
    TEST_ASSERT_TRUE(cmt_convert_dialog_is_open());
    TEST_ASSERT_NOT_NULL(v_item("cv_ok"));
    TEST_ASSERT_FALSE_MESSAGE(v_item("cv_ok")->disabled, "the chosen profile is kept");
    TEST_ASSERT_EQUAL_STRING("Set by the profile", item_text(v_item("cv_speed_fixed")).c_str());
}

/*
 * Okno Tape check: souhrn jako nadpis, řádky se slovem závažnosti, číslem
 * bloku od 1 a jménem; čtení jen při otevření a Check again (ne každý snímek);
 * klik vybere blok v tabulce, okno kontroly zůstane navrchu; po změně pásky
 * zastaralé (text, klik nic nevybere), Check again nálezy obnoví.
 */
void test_j5_check_window(void)
{
    tt_setup(TT_TAPE_FINDINGS);
    tt_menu("menu_check");
    TEST_ASSERT_TRUE(cmt_check_window_is_open());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_check_window_test_reads(), "the findings are read once when the window opens");
    tt_frames(5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_check_window_test_reads(), "the findings are not read every frame");
    TEST_ASSERT_EQUAL_STRING("Blocks checked: 5. Warnings: 2.", item_text(c_item("ck_summary")).c_str());
    TEST_ASSERT_EQUAL_STRING("Warning", item_text(c_item("ck_sev_0")).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Block 2", item_text(c_item("ck_block_0")).c_str(), "a control block by its number only");
    TEST_ASSERT_EQUAL_STRING("Loop end without a loop start", item_text(c_item("ck_text_0")).c_str());
    TEST_ASSERT_EQUAL_STRING("Warning", item_text(c_item("ck_sev_1")).c_str());
    TEST_ASSERT_EQUAL_STRING("Block 4", item_text(c_item("ck_block_1")).c_str());
    TEST_ASSERT_EQUAL_STRING("Group start without a group end", item_text(c_item("ck_text_1")).c_str());
    TEST_ASSERT_NULL(c_item("ck_outdated"));
    /* klik na druhý nález: výběr bloku 3 (od 0); okno kazety se dopředu nevynáší, okno kontroly zůstane vidět */
    ImGui::SetWindowFocus(TT_CHECK_ID);
    tt_frame();
    tt_click_item(c_item("ck_row_1"));
    tt_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, imgui_cmt_window_test_get_selection(), "a click on a finding selects its block");
    ImGuiWindow *nav = GImGui->NavWindow;
    TEST_ASSERT_NOT_NULL(nav);
    TEST_ASSERT_TRUE_MESSAGE(strstr(nav->RootWindow->Name, TT_CHECK_ID) != NULL, "the Tape check window stays in front");
    /* Compact: klik přepne na Full (seznam bloků) */
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    tt_frames(3);
    ImGui::SetWindowFocus(TT_CHECK_ID);
    tt_frames(2);
    tt_click_item(c_item("ck_row_0"));
    tt_frames(2);
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_FULL, imgui_cmt_window_get_layout());
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_window_test_get_selection());
    /* sbalené okno kazety: klik ho rozbalí */
    ImGui::SetWindowCollapsed(TT_W_ID, true);
    tt_frames(2);
    ImGui::SetWindowFocus(TT_CHECK_ID);
    tt_frames(2);
    tt_click_item(c_item("ck_row_1"));
    tt_frames(2);
    TEST_ASSERT_FALSE_MESSAGE(ImGui::FindWindowByName(TT_W_ID)->Collapsed, "a click on a finding expands a collapsed Virtual CMT window");
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    /* změna pásky: zastaralé */
    device_state_lock();
    int rem = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(&rem, 1));
    device_state_unlock();
    tt_frames(3);
    TEST_ASSERT_EQUAL_STRING("The tape changed. Run the check again.", item_text(c_item("ck_outdated")).c_str());
    TEST_ASSERT_EQUAL_INT(1, cmt_check_window_test_reads());
    imgui_cmt_window_test_set_selection(-1, tape_serial());
    /* okno kontroly dopředu (klik nesmí trefit tabulku okna kazety pod ním) */
    ImGui::SetWindowFocus(TT_CHECK_ID);
    tt_frames(2);
    tt_click_item(c_item("ck_row_0"));
    tt_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, imgui_cmt_window_test_get_selection(), "an outdated finding selects nothing");
    /* Check again */
    tt_click_item(c_item("ck_again"));
    TEST_ASSERT_EQUAL_INT(2, cmt_check_window_test_reads());
    TEST_ASSERT_NULL(c_item("ck_outdated"));
    TEST_ASSERT_EQUAL_STRING("Blocks checked: 4. Warnings: 2.", item_text(c_item("ck_summary")).c_str());
    TEST_ASSERT_EQUAL_STRING("Block 1", item_text(c_item("ck_block_0")).c_str());
    /* znovu z menu: přečte znovu */
    tt_menu("menu_check");
    TEST_ASSERT_EQUAL_INT(3, cmt_check_window_test_reads());
}

/*
 * Bloky připojené záznamem (REC a STOP: cmttmz_append_blocks(), čítač výměn pásky se
 * nemění) označí nálezy jako zastaralé; Check again zkontroluje i nové bloky.
 */
void test_finalb_check_outdated_after_recording(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_menu("menu_check");
    TEST_ASSERT_EQUAL_STRING("No problems found. Blocks checked: 3.", item_text(c_item("ck_summary")).c_str());
    TEST_ASSERT_NULL(c_item("ck_outdated"));
    const unsigned serial = tape_serial();
    /* připojení jako po nahrávání: blok MZF na konec pásky */
    st_MZF_HEADER hdr;
    make_hdr(&hdr, "RECORDED", 0x01, 64);
    uint8_t body[64];
    for (int i = 0; i < 64; i++) body[i] = (uint8_t)(i * 5);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 64, CMTSPEED_1_1, NULL, 0);
    TEST_ASSERT_NOT_NULL(b);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    device_state_lock();
    int rc = cmttmz_append_blocks(b, &sp, 1);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    b->data = NULL; /* data převzala páska */
    tmz_block_free(b);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(serial, tape_serial(), "appending a recording keeps the tape serial");
    tt_frames(3);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("The tape changed. Run the check again.", item_text(c_item("ck_outdated")).c_str(),
                                     "blocks appended by a recording make the findings outdated");
    tt_click_item(c_item("ck_again"));
    TEST_ASSERT_NULL(c_item("ck_outdated"));
    TEST_ASSERT_EQUAL_STRING("No problems found. Blocks checked: 4.", item_text(c_item("ck_summary")).c_str());
}

/* Páska bez nálezů: "No problems found. Blocks checked: N.", žádné řádky; vysunutí pásky = zastaralé. */
void test_j5_check_no_problems(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_menu("menu_check");
    TEST_ASSERT_EQUAL_STRING("No problems found. Blocks checked: 3.", item_text(c_item("ck_summary")).c_str());
    TEST_ASSERT_NULL(c_item("ck_row_0"));
    device_state_lock();
    cmt_eject();
    device_state_unlock();
    tt_frames(3);
    TEST_ASSERT_EQUAL_STRING("The tape changed. Run the check again.", item_text(c_item("ck_outdated")).c_str());
}

/*
 * Převod profilu: titulek, seznam profilů s ID a popisem, aktuální profil
 * označený a vybraný; Convert beze změny (aktuální profil, výchozí
 * rychlost) zakázané; rychlost jen u profilu, který ji přijímá; Convert
 * zařadí zásah s blokem, ID a rychlostí; po provedení zpráva a výběr
 * převedeného bloku, profil bloku v jádře změněný.
 */
void test_j5_convert_dialog(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_open_convert(0);
    TEST_ASSERT_EQUAL_STRING("Convert to profile - block 1###cmt_convert", cmt_convert_dialog_test_title());
    std::vector<st_CMT_VIEW_PROFILE> profiles;
    cmt_view_read_profiles(&profiles);
    TEST_ASSERT_TRUE(profiles.size() > 2);
    int cur = -1, rom = -1, tc = -1;
    for (size_t i = 0; i < profiles.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(profiles[i].id.c_str(), item_text(v_item(nm("cv_id", (int)i))).c_str());
        TEST_ASSERT_EQUAL_STRING(profiles[i].description.c_str(), item_text(v_item(nm("cv_desc", (int)i))).c_str());
        if (profiles[i].id == "MZ800EMU-CMT") cur = (int)i;
        if (profiles[i].id == "ROM-800-NOMINAL") rom = (int)i;
        if (profiles[i].id == "TC-1.21") tc = (int)i;
    }
    TEST_ASSERT_TRUE(cur >= 0 && rom >= 0 && tc >= 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("(current)", item_text(v_item(nm("cv_current", cur))).c_str(), "the current profile is marked");
    TEST_ASSERT_NULL(v_item(nm("cv_current", rom)));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1", item_text(v_item(nm("cv_row", cur))).c_str(), "the current profile is selected");
    TEST_ASSERT_TRUE(v_item("cv_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING("Nothing to change.", v_item("cv_ok")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING("Convert", item_text(v_item("cv_ok")).c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(v_item("cv_speed"), "MZ800EMU-CMT accepts a speed: Speed combo");
    TEST_ASSERT_FALSE(v_item("cv_speed")->disabled);
    TEST_ASSERT_NULL(v_item("cv_speed_fixed"));
    TEST_ASSERT_NULL_MESSAGE(v_item("cv_note"), "MZ800EMU-CMT keeps the speed changeable: no note");
    /* Enter beze změny nic (dialog zůstane, žádný zásah) */
    s_poll = false;
    tt_drain();
    tt_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE_MESSAGE(cmt_convert_dialog_is_open(), "Enter with nothing to change keeps the dialog open");
    TEST_ASSERT_FALSE(tt_take_request(CMT_REQUEST_CONVERT_BLOCK_NAME, NULL));
    s_poll = true;
    /* profil bez rychlosti (klik do seznamu) */
    tt_click_item(v_item(nm("cv_row", rom)));
    TEST_ASSERT_EQUAL_STRING("1", item_text(v_item(nm("cv_row", rom))).c_str());
    TEST_ASSERT_NULL_MESSAGE(v_item("cv_speed"), "ROM-800-NOMINAL has no Speed combo");
    TEST_ASSERT_EQUAL_STRING("Set by the profile", item_text(v_item("cv_speed_fixed")).c_str());
    TEST_ASSERT_FALSE(v_item("cv_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING("After the conversion, Set speed cannot change this block.",
                             item_text(v_item("cv_note")).c_str());
    /* profil s rychlostí: TC-1.21 3:1 - zásah */
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("TC-1.21"));
    cmt_convert_dialog_test_set_speed(CMTSPEED_3_1);
    tt_frames(2);
    TEST_ASSERT_NOT_NULL(v_item("cv_speed"));
    TEST_ASSERT_FALSE(v_item("cv_speed")->disabled);
    TEST_ASSERT_TRUE_MESSAGE(item_text(v_item("cv_speed")).rfind("3:1", 0) == 0, item_text(v_item("cv_speed")).c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(v_item("cv_note"), "TC-1.21 blocks cannot change speed in the emulator");
    s_poll = false;
    tt_drain();
    tt_click_item(v_item("cv_ok"));
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE_MESSAGE(tt_take_request(CMT_REQUEST_CONVERT_BLOCK_NAME, &a), "Convert queues the conversion");
    TEST_ASSERT_EQUAL_INT(0, a.i[0]);
    TEST_ASSERT_EQUAL_INT((int)CMTSPEED_3_1, a.i[1]);
    TEST_ASSERT_EQUAL_STRING("TC-1.21", a.str);
    TEST_ASSERT_EQUAL_INT((int)tape_serial(), a.i[5]);
    TEST_ASSERT_FALSE(cmt_convert_dialog_is_open());
    /* skutečný převod: TC-1.21 3:1 (zpráva o převodu, výběr převedeného bloku) */
    s_poll = true;
    tt_open_convert(0);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("TC-1.21"));
    cmt_convert_dialog_test_set_speed(CMTSPEED_3_1);
    tt_frames(2);
    tt_click_item(v_item("cv_ok"));
    tt_frames(4);
    TEST_ASSERT_EQUAL_STRING("TC-1.21", block_profile(0).c_str());
    st_CMT_VIEW tv = tt_view();
    TEST_ASSERT_EQUAL_STRING("Block 1 converted to TC-1.21.", tv.message.c_str());
    TEST_ASSERT_FALSE(tv.message_is_warning);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, imgui_cmt_window_test_get_selection(), "the converted block is selected");
    /* ROM-800-NOMINAL bez rychlosti (rychlost se u něj neposílá ani po volbě u jiného profilu): 3:1 TC-1.21
     * ROM-800-NOMINAL nepřijme - varování jádra místo zprávy o převodu */
    tt_open_convert(0);
    cmt_convert_dialog_test_set_speed(CMTSPEED_2_1);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("ROM-800-NOMINAL"));
    tt_frames(2);
    tt_click_item(v_item("cv_ok"));
    tt_frames(4);
    TEST_ASSERT_EQUAL_STRING("ROM-800-NOMINAL", block_profile(0).c_str());
    tv = tt_view();
    TEST_ASSERT_TRUE_MESSAGE(tv.message.rfind("Settings of profile TC-1.21 not carried over:", 0) == 0, tv.message.c_str());
    TEST_ASSERT_TRUE(tv.message_is_warning);
    /* zpět na MZ800EMU-CMT 2:1: bez varování */
    tt_open_convert(0);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("MZ800EMU-CMT"));
    cmt_convert_dialog_test_set_speed(CMTSPEED_2_1);
    tt_frames(2);
    tt_click_item(v_item("cv_ok"));
    tt_frames(4);
    TEST_ASSERT_EQUAL_STRING("MZ800EMU-CMT", block_profile(0).c_str());
    tv = tt_view();
    TEST_ASSERT_EQUAL_STRING("Block 1 converted to MZ800EMU-CMT.", tv.message.c_str());
    TEST_ASSERT_FALSE(tv.message_is_warning);
    /* tentýž profil a tatáž rychlost: jádro pásku nezmění - "Nothing to change." (informace) */
    const unsigned serial_before = tape_serial();
    tt_open_convert(0);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("MZ800EMU-CMT"));
    cmt_convert_dialog_test_set_speed(CMTSPEED_2_1);
    tt_frames(2);
    tt_click_item(v_item("cv_ok"));
    tt_frames(4);
    tv = tt_view();
    TEST_ASSERT_EQUAL_STRING("Nothing to change.", tv.message.c_str());
    TEST_ASSERT_FALSE(tv.message_is_warning);
    TEST_ASSERT_EQUAL_UINT(serial_before, tape_serial());
}

/*
 * Čísla bloků v textech jádra (od 0) se v okně ukazují od 1 jako v tabulce
 * (cmt_view_core_text()): "Block N", "block N", "Row N", i záporné cíle
 * skoků; jiná čísla a slova beze změny.
 */
void test_j5_core_text_numbers(void)
{
    TEST_ASSERT_EQUAL_STRING("Jump to block 8 outside the tape", cmt_view_core_text("Jump to block 7 outside the tape").c_str());
    TEST_ASSERT_EQUAL_STRING("Call to block 0 outside the tape", cmt_view_core_text("Call to block -1 outside the tape").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 3 does not exist", cmt_view_core_text("Block 2 does not exist").c_str());
    TEST_ASSERT_EQUAL_STRING("Row 1 failed", cmt_view_core_text("Row 0 failed").c_str());
    TEST_ASSERT_EQUAL_STRING("Body length 64 B differs from the header size 100 B",
                             cmt_view_core_text("Body length 64 B differs from the header size 100 B").c_str());
    TEST_ASSERT_EQUAL_STRING("Unknown block ID 0x99", cmt_view_core_text("Unknown block ID 0x99").c_str());
    TEST_ASSERT_EQUAL_STRING("Subblock 2", cmt_view_core_text("Subblock 2").c_str());
    /* jen přesný tvar jádra: za číslem konec nebo znak, který není písmeno ani číslice */
    TEST_ASSERT_EQUAL_STRING("Block 0x18 is unknown", cmt_view_core_text("Block 0x18 is unknown").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 12a", cmt_view_core_text("Block 12a").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 13", cmt_view_core_text("Block 12").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 2: Row 4, block 6.", cmt_view_core_text("Block 1: Row 3, block 5.").c_str());
    TEST_ASSERT_EQUAL_STRING("Block x", cmt_view_core_text("Block x").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 1234567890", cmt_view_core_text("Block 1234567890").c_str());
    /* přechod na další řád, malé "row" (analýza), seznamy čísel bloků */
    TEST_ASSERT_EQUAL_STRING("Block 10", cmt_view_core_text("Block 9").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 100 does not exist", cmt_view_core_text("Block 99 does not exist").c_str());
    TEST_ASSERT_EQUAL_STRING("Can't build a tape block from row 3", cmt_view_core_text("Can't build a tape block from row 2").c_str());
    TEST_ASSERT_EQUAL_STRING("Blocks that cannot be played were skipped: 4, 6 and 2 more",
                             cmt_view_core_text("Blocks that cannot be played were skipped: 3, 5 and 2 more").c_str());
    TEST_ASSERT_EQUAL_STRING("Blocks the player cannot play (skipped): 4 (ID 0x18), 8 (ID 0x20)",
                             cmt_view_core_text("Blocks the player cannot play (skipped): 3 (ID 0x18), 7 (ID 0x20)").c_str());
    TEST_ASSERT_EQUAL_STRING("Speed of block 4 cannot be stored in MZT; Blocks that cannot be played were skipped: 1",
                             cmt_view_core_text("Speed of block 3 cannot be stored in MZT; Blocks that cannot be played were skipped: 0").c_str());
    TEST_ASSERT_EQUAL_STRING("Something: 3, 5", cmt_view_core_text("Something: 3, 5").c_str());
    /* cesta souboru se nikdy nezmění */
    const char *p = "C:/Tape Block 5/Game Row 3.wav";
    TEST_ASSERT_EQUAL_STRING("Block 3 exported: C:/Tape Block 5/Game Row 3.wav",
                             cmt_view_core_text_path("Block 2 exported: C:/Tape Block 5/Game Row 3.wav", p).c_str());
    TEST_ASSERT_EQUAL_STRING("Tape saved: C:/Tape Block 5/Game Row 3.wav (Speed of block 4 cannot be stored in MZT)",
                             cmt_view_core_text_path("Tape saved: C:/Tape Block 5/Game Row 3.wav (Speed of block 3 cannot be stored in MZT)", p).c_str());
    TEST_ASSERT_EQUAL_STRING("Tape exported: C:/Tape Block 5/Game Row 3.wav, 1.0 s (Blocks that cannot be played were skipped: 3, 8)",
                             cmt_view_core_text_path("Tape exported: C:/Tape Block 5/Game Row 3.wav, 1.0 s (Blocks that cannot be played were skipped: 2, 7)", p)
                                 .c_str());
    TEST_ASSERT_EQUAL_STRING("Can't import ... Block 5/Game Row 3.wav: Block 3 is broken",
                             cmt_view_core_text_path("Can't import ... Block 5/Game Row 3.wav: Block 2 is broken", p).c_str());
    TEST_ASSERT_EQUAL_STRING("Block 3 does not exist", cmt_view_core_text_path("Block 2 does not exist", p).c_str());
    TEST_ASSERT_EQUAL_STRING("", cmt_view_core_text(NULL).c_str());
}

/*
 * Escape v otevřeném výběru Target machine zavře jen výběr,
 * dialog zůstane otevřený a úpravy polí se neztratí; další Escape dialog zavře.
 */
void test_j5_info_escape_in_combo(void)
{
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Kept title");
    cmt_tapeinfo_dialog_test_set_text(2, "Kept author");
    tt_frames(2);
    tt_click_item(i_item("ti_machine"));
    TEST_ASSERT_TRUE_MESSAGE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel), "a popup is open");
    tt_key(ImGuiKey_Escape);
    TEST_ASSERT_TRUE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "Escape in the open combo keeps the dialog");
    TEST_ASSERT_EQUAL_STRING("Kept title", item_text(i_item("ti_field_0")).c_str());
    TEST_ASSERT_EQUAL_STRING("Kept author", item_text(i_item("ti_field_2")).c_str());
    tt_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE_MESSAGE(cmt_tapeinfo_dialog_is_open(), "the next Escape closes the dialog");
}

/* Totéž ve výběru rychlosti dialogu převodu. */
void test_j5_convert_escape_in_combo(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_open_convert(0);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("TC-1.21"));
    tt_frames(2);
    tt_click_item(v_item("cv_speed"));
    tt_key(ImGuiKey_Escape);
    TEST_ASSERT_TRUE_MESSAGE(cmt_convert_dialog_is_open(), "Escape in the open speed combo keeps the dialog");
    TEST_ASSERT_EQUAL_STRING("1", item_text(v_item(nm("cv_row", 2))).c_str());
    tt_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(cmt_convert_dialog_is_open());
}

/* Převod: páska se po otevření dialogu změní - Convert zakázané s textem, dialog zůstane. */
void test_j5_convert_tape_changed(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_open_convert(0);
    TEST_ASSERT_TRUE(cmt_convert_dialog_test_select("ROM-800-NOMINAL"));
    tt_frames(2);
    TEST_ASSERT_FALSE(v_item("cv_ok")->disabled);
    core_set_info("Changed", "", "", "", -1);
    tt_frames(3);
    TEST_ASSERT_TRUE(cmt_convert_dialog_is_open());
    TEST_ASSERT_TRUE(v_item("cv_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING("The tape changed. Close this dialog and open it again.", v_item("cv_ok")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING("The tape changed. Close this dialog and open it again.", item_text(v_item("cv_status")).c_str());
}

/*
 * Blok, jehož profil není v nabídce (ID přepsané na
 * neznámé), nemá nic předvybraného; Convert je zakázané, dokud uživatel
 * profil nevybere; aktuální profil je vidět v řádku nad seznamem.
 */
void test_j5_convert_unknown_current_profile(void)
{
    tt_setup(TT_TAPE_PLAIN);
    open_odd_profile_tape();
    tt_frames(3);
    TEST_ASSERT_EQUAL_STRING("MZ800EMU-XYZ", block_profile(0).c_str());
    tt_open_convert(0);
    TEST_ASSERT_EQUAL_STRING("Current profile: MZ800EMU-XYZ", item_text(v_item("cv_summary")).c_str());
    for (int i = 0; v_item(nm("cv_row", i)); i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("0", item_text(v_item(nm("cv_row", i))).c_str(), "nothing preselected");
        TEST_ASSERT_NULL(v_item(nm("cv_current", i)));
    }
    TEST_ASSERT_TRUE(v_item("cv_ok")->disabled);
    TEST_ASSERT_EQUAL_STRING("Select a profile.", v_item("cv_ok")->reason.c_str());
    /* Enter nic nepošle */
    s_poll = false;
    tt_drain();
    tt_key(ImGuiKey_Enter);
    TEST_ASSERT_FALSE(tt_take_request(CMT_REQUEST_CONVERT_BLOCK_NAME, NULL));
    TEST_ASSERT_TRUE(cmt_convert_dialog_is_open());
    s_poll = true;
    tt_click_item(v_item(nm("cv_row", 0)));
    TEST_ASSERT_FALSE_MESSAGE(v_item("cv_ok")->disabled, "Convert allowed after picking a profile");
}

/*
 * Páska s víc než 500 nálezy - nadpis "Warnings: more than
 * 500.", souhrnný nález jádra bez bloku je řádek Info a klik na něj nic
 * nevybere.
 */
void test_j5_check_capped(void)
{
    tt_setup(TT_TAPE_PLAIN);
    enum { CALLS = 600 };
    uint8_t data[2 + 2 * CALLS];
    data[0] = (uint8_t)(CALLS & 0xFF);
    data[1] = (uint8_t)(CALLS >> 8);
    for (int k = 0; k < CALLS; k++) {
        data[2 + 2 * k] = (uint8_t)((uint16_t)-30000 & 0xFF);
        data[3 + 2 * k] = (uint8_t)((uint16_t)-30000 >> 8);
    }
    st_TZX_FILE *f = new_tmz();
    add_mzf(f, "A", 64);
    add_raw(f, TZX_BLOCK_ID_CALL_SEQUENCE, data, sizeof(data));
    add_mzf(f, "B", 64);
    save_tape(f, "capped.tmz");
    tt_frames(3);
    tt_menu("menu_check");
    TEST_ASSERT_EQUAL_STRING("Blocks checked: 3. Warnings: more than 500.", item_text(c_item("ck_summary")).c_str());
    TEST_ASSERT_EQUAL_STRING("Info", item_text(c_item("ck_sev_500")).c_str());
    TEST_ASSERT_NULL_MESSAGE(c_item("ck_block_500"), "the summary finding has no block");
    TEST_ASSERT_EQUAL_STRING("More problems were found; only 500 of them are listed.", item_text(c_item("ck_text_500")).c_str());
    TEST_ASSERT_TRUE(c_item("ck_row_500")->disabled);
}

/*
 * Cizí položky na úrovni okna: zápis z formuláře nad páskou s cizí
 * položkou 0x32, druhým blokem 0x32 dál na pásce a blokem 0x30 - cizí
 * položka i oba bloky zůstanou, mění se jen titul.
 */
void test_j5_info_foreign_entries_kept(void)
{
    tt_setup(TT_TAPE_PLAIN);
    st_TZX_FILE *f = new_tmz();
    {
        /* blok 0x32: Title "Old" + cizí položka ID 10h "Foreign" */
        uint8_t buf[64];
        uint32_t pos = 3;
        buf[pos++] = 0x00;
        buf[pos++] = 3;
        memcpy(buf + pos, "Old", 3);
        pos += 3;
        buf[pos++] = 0x10;
        buf[pos++] = 7;
        memcpy(buf + pos, "Foreign", 7);
        pos += 7;
        buf[0] = (uint8_t)((pos - 2) & 0xFF);
        buf[1] = (uint8_t)((pos - 2) >> 8);
        buf[2] = 2;
        add_raw(f, TZX_BLOCK_ID_ARCHIVE_INFO, buf, pos);
    }
    add_mzf(f, "A", 64);
    {
        const uint8_t txt[] = {4, 'N', 'o', 't', 'e'};
        add_raw(f, 0x30, txt, sizeof(txt));
    }
    add_archive_title(f, "Side B");
    save_tape(f, "foreign.tmz");
    tt_frames(3);
    st_CMTTMZ_TAPE_INFO before = core_info();
    TEST_ASSERT_EQUAL_UINT32(1, before.other_entries);
    TEST_ASSERT_EQUAL_UINT32(1, before.other_archive_blocks);
    st_CMT_VIEW tv = tt_view();
    const int blocks_before = tv.block_count;
    tt_open_info();
    TEST_ASSERT_EQUAL_STRING("Old", item_text(i_item("ti_field_0")).c_str());
    cmt_tapeinfo_dialog_test_set_text(0, "New");
    tt_frames(2);
    tt_click_item(i_item("ti_ok"));
    tt_frames(3);
    TEST_ASSERT_FALSE(cmt_tapeinfo_dialog_is_open());
    st_CMTTMZ_TAPE_INFO after = core_info();
    TEST_ASSERT_EQUAL_STRING("New", after.title);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, after.other_entries, "the foreign entry of block 0x32 is kept");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, after.other_archive_blocks, "the second block 0x32 is kept");
    tv = tt_view();
    TEST_ASSERT_EQUAL_INT_MESSAGE(blocks_before, tv.block_count, "no block added or removed");
}

/*
 * Po zápisu informací, který vložil bloky 0x32 a 0x43 na
 * začátek pásky, je vybraný týž blok na novém čísle.
 */
void test_j5_info_keeps_selection(void)
{
    tt_setup(TT_TAPE_PLAIN);
    imgui_cmt_window_test_set_selection(2, tape_serial());
    tt_frames(2);
    tt_open_info();
    cmt_tapeinfo_dialog_test_set_text(0, "Title");
    cmt_tapeinfo_dialog_test_set_machine(TMZ_MACHINE_MZ800);
    tt_frames(2);
    tt_click_item(i_item("ti_ok"));
    tt_frames(4);
    st_CMT_VIEW tv = tt_view();
    TEST_ASSERT_EQUAL_INT(5, tv.block_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, imgui_cmt_window_test_get_selection(), "GAME stays selected at its new number");
    TEST_ASSERT_EQUAL_STRING("GAME", tv.blocks[4].name.c_str());
}

/*
 * Chyba importu nese cestu se slovy "Block 5" a "Row 3";
 * řádek zpráv ji ukáže beze změny.
 */
void test_j5_import_error_keeps_path(void)
{
    tt_setup(TT_TAPE_PLAIN);
    gchar *dir = g_build_filename(g_get_tmp_dir(), "mztest cmt tools Block 5", NULL);
    gchar *path = g_build_filename(dir, "Game Row 3.mzf", NULL);
    const char *paths[1] = {path};
    imgui_cmt_tape_import_paths(paths, 1, -1, tape_serial());
    tt_frames(5);
    std::string msg = tt_view().message;
    TEST_ASSERT_TRUE_MESSAGE(msg.find(path) != std::string::npos, msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(msg.find("Block 6") == std::string::npos && msg.find("Row 4") == std::string::npos, msg.c_str());
    g_free(path);
    g_free(dir);
}

/*
 * Měření (jen MZ_CMT_BENCH=1, jinak IGNORE): páska s 10 000 bloky
 * a nálezem u každého druhého bloku; měří dobu čtení okna kontroly pod
 * zámkem stavu zařízení (průchod jádra a jména bloků s nálezem) a snímek
 * okna s 501 řádky.
 */
void test_j5_check_bench(void)
{
    const char *ev = getenv("MZ_CMT_BENCH");
    if (!ev || atoi(ev) != 1) TEST_IGNORE_MESSAGE("set MZ_CMT_BENCH=1 to measure the tape check");
    tt_setup(TT_TAPE_PLAIN);
    st_TZX_FILE *f = new_tmz();
    for (int i = 0; i < 5000; i++) {
        add_mzf(f, "X", 64);
        add_raw(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    }
    save_tape(f, "bench.tmz");
    tt_frames(3);
    std::vector<st_CMTTMZ_CHECK_ITEM> items;
    uint32_t checked = 0;
    device_state_lock();
    gint64 t0 = g_get_monotonic_time();
    cmt_view_read_tape_check(&items, &checked);
    gint64 t1 = g_get_monotonic_time();
    st_CMT_VIEW_BLOCK b;
    for (const st_CMTTMZ_CHECK_ITEM &it : items)
        if (it.block_id >= 0) cmt_view_read_block(it.block_id, &b);
    gint64 t2 = g_get_monotonic_time();
    device_state_unlock();
    tt_menu("menu_check");
    gint64 t3 = g_get_monotonic_time();
    tt_frame();
    gint64 t4 = g_get_monotonic_time();
    tt_frame();
    gint64 t5 = g_get_monotonic_time();
    fprintf(stderr, "bench: %u blocks checked, %u findings; core check %.2f ms, block names %.2f ms; "
                    "frame with the window %.2f ms, next frame %.2f ms\n",
            (unsigned)checked, (unsigned)items.size(), (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t4 - t3) / 1000.0, (t5 - t4) / 1000.0);
}

/* Nápověda k analýze: otevření pásky s jediným surovým blokem; jiná páska bez nápovědy. */
void test_j5_open_single_raw_hint(void)
{
    tt_setup(TT_TAPE_PLAIN);
    st_TZX_FILE *f = new_tmz();
    add_csw(f, 3000);
    save_tape(f, "raw.tmz", false);
    TEST_ASSERT_NOT_EQUAL(0u, cmt_request_open(s_path, false));
    tt_frames(4);
    TEST_ASSERT_EQUAL_STRING("Opened raw.tmz, blocks: 1. Use Analyze to find files.", item_text(w_item("message")).c_str());
    /* páska s víc bloky: obvyklá zpráva */
    gchar *raw_path = g_strdup(s_path);
    st_TZX_FILE *g = new_tmz();
    add_mzf(g, "A", 64);
    add_csw(g, 3000);
    save_tape(g, "two.tmz", false);
    TEST_ASSERT_NOT_EQUAL(0u, cmt_request_open(s_path, false));
    tt_frames(4);
    TEST_ASSERT_EQUAL_STRING("Opened two.tmz, blocks: 2", item_text(w_item("message")).c_str());
    gchar *dir = g_path_get_dirname(raw_path);
    g_remove(raw_path);
    g_rmdir(dir);
    g_free(dir);
    g_free(raw_path);
}

/**
 * @brief Kontrola geometrie: prvky uvnitř okna a ořezu, bez překryvu, text ne širší než ořez.
 * @param items Prvky. @param imin Vnitřek okna min. @param imax Vnitřek okna max. @param ctx Popis. @return Počet vad.
 */
static int tt_faults(const std::vector<st_CMT_WINDOW_TEST_ITEM> &in, ImVec2 imin, ImVec2 imax, const char *ctx)
{
    const float tol = 0.5f;
    int faults = 0;
    std::vector<st_CMT_WINDOW_TEST_ITEM> items = in;
    for (st_CMT_WINDOW_TEST_ITEM &it : items) {
        if (!it.scroll_y) continue;
        it.min.y = ImMax(it.min.y, it.clip_min.y);
        it.max.y = ImMin(it.max.y, it.clip_max.y);
    }
    for (const st_CMT_WINDOW_TEST_ITEM &it : items) {
        if (it.max.x - it.min.x < tol || it.max.y - it.min.y < tol) continue;
        if (!it.container && !it.text.empty() && (it.max.x - it.min.x) > (it.clip_max.x - it.clip_min.x) + tol) {
            fprintf(stderr, "%s: item '%s' wider than its clip\n", ctx, it.name.c_str());
            faults++;
        }
        if (it.container && it.scroll_y) continue;
        bool in_clip = it.min.x >= it.clip_min.x - tol && it.max.x <= it.clip_max.x + tol
                       && (it.scroll_y || (it.min.y >= it.clip_min.y - tol && it.max.y <= it.clip_max.y + tol));
        bool in_win = it.min.x >= imin.x - tol && it.max.x <= imax.x + tol && (it.scroll_y || (it.min.y >= imin.y - tol && it.max.y <= imax.y + tol));
        if (!in_clip || !in_win) {
            fprintf(stderr, "%s: item '%s' (%.1f,%.1f)-(%.1f,%.1f) outside %s\n", ctx, it.name.c_str(), it.min.x, it.min.y, it.max.x, it.max.y,
                    in_clip ? "window" : "clip rect");
            faults++;
        }
    }
    for (size_t i = 0; i < items.size(); i++) {
        const st_CMT_WINDOW_TEST_ITEM &a = items[i];
        if (a.container || a.max.y - a.min.y < tol || a.max.x - a.min.x < tol) continue;
        for (size_t j = i + 1; j < items.size(); j++) {
            const st_CMT_WINDOW_TEST_ITEM &b = items[j];
            if (b.container || b.max.y - b.min.y < tol || b.max.x - b.min.x < tol) continue;
            float ox = ImMin(a.max.x, b.max.x) - ImMax(a.min.x, b.min.x);
            float oy = ImMin(a.max.y, b.max.y) - ImMax(a.min.y, b.min.y);
            if (ox > tol && oy > tol) {
                fprintf(stderr, "%s: items '%s' and '%s' overlap\n", ctx, a.name.c_str(), b.name.c_str());
                faults++;
            }
        }
    }
    return faults;
}

/** @brief Okno nejvýš 90 % pracovní plochy a v pracovní ploše viewportu, bez posuvníku okna. */
static int tt_size_faults(ImVec2 wmin, ImVec2 wmax, ImVec2 work, const char *id, const char *ctx)
{
    int faults = 0;
    if (wmax.x - wmin.x > 0.9f * work.x + 1.0f || wmax.y - wmin.y > 0.9f * work.y + 1.0f) {
        fprintf(stderr, "%s: %.0f x %.0f px exceeds 90 %% of %.0f x %.0f\n", ctx, wmax.x - wmin.x, wmax.y - wmin.y, work.x, work.y);
        faults++;
    }
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    if (wmin.x < vp->WorkPos.x - 0.5f || wmin.y < vp->WorkPos.y - 0.5f || wmax.x > vp->WorkPos.x + vp->WorkSize.x + 0.5f
        || wmax.y > vp->WorkPos.y + vp->WorkSize.y + 0.5f) {
        fprintf(stderr, "%s: (%.0f,%.0f)-(%.0f,%.0f) outside the work area\n", ctx, wmin.x, wmin.y, wmax.x, wmax.y);
        faults++;
    }
    ImGuiWindow *w = ImGui::FindWindowByName(id);
    if (w && (w->ScrollbarY || w->ScrollbarX)) {
        fprintf(stderr, "%s: the window itself scrolls\n", ctx);
        faults++;
    }
    return faults;
}

/** @brief Stavy snímků a geometrie. */
typedef enum en_TT_STATE {
    TT_ST_INFO = 0,      /**< vyplněný formulář */
    TT_ST_INFO_FORMAT,   /**< páska z MZF: věta o formátu */
    TT_ST_CHECK,         /**< nálezy */
    TT_ST_CHECK_NONE,    /**< bez nálezů */
    TT_ST_CHECK_OUTDATED, /**< zastaralé nálezy */
    TT_ST_CONVERT_SPEED, /**< převod, profil s rychlostí */
    TT_ST_CONVERT_NOSPEED, /**< převod, profil bez rychlosti */
    TT_ST_INFO_RELOAD,   /**< formulář po změně pásky: výzva a Reload */
    TT_ST_CHECK_INFO,    /**< nález se závažností Info (neznámý profil) */
    TT_ST_COUNT
} en_TT_STATE;

static const char *s_state_names[TT_ST_COUNT] = {"info", "info-format", "check", "check-none", "check-outdated", "convert-speed",
                                                  "convert-nospeed", "info-reload", "check-info"};

/**
 * @brief Připraví stav @p st (otevře pásku, dialog nebo okno).
 * @param st Stav. @param scale Měřítko. @param stretch Texty o 40 % delší.
 */
static void tt_prepare(en_TT_STATE st, float scale, bool stretch)
{
    switch (st) {
        case TT_ST_INFO:
            tt_setup(TT_TAPE_INFO, scale, stretch);
            tt_open_info();
            break;
        case TT_ST_INFO_FORMAT:
            tt_setup(TT_TAPE_MZF, scale, stretch);
            tt_open_info();
            break;
        case TT_ST_CHECK:
        case TT_ST_CHECK_OUTDATED:
            tt_setup(TT_TAPE_FINDINGS, scale, stretch);
            tt_menu("menu_check");
            if (st == TT_ST_CHECK_OUTDATED) {
                device_state_lock();
                int rem = 4;
                cmt_tape_remove_blocks(&rem, 1);
                device_state_unlock();
            }
            break;
        case TT_ST_CHECK_NONE:
            tt_setup(TT_TAPE_PLAIN, scale, stretch);
            tt_menu("menu_check");
            break;
        case TT_ST_CONVERT_SPEED:
        case TT_ST_CONVERT_NOSPEED:
            tt_setup(TT_TAPE_PLAIN, scale, stretch);
            tt_open_convert(0);
            cmt_convert_dialog_test_select(st == TT_ST_CONVERT_SPEED ? "TC-1.21" : "ROM-800-NOMINAL");
            if (st == TT_ST_CONVERT_SPEED) cmt_convert_dialog_test_set_speed(CMTSPEED_3_1);
            break;
        case TT_ST_INFO_RELOAD:
            tt_setup(TT_TAPE_INFO, scale, stretch);
            tt_open_info();
            cmt_tapeinfo_dialog_test_set_text(0, "My edit");
            tt_frames(2);
            /* páska se změní jinou cestou (jádro): výzva a Reload */
            core_set_info("Changed elsewhere", "1990", "English", "", TMZ_MACHINE_MZ800);
            break;
        case TT_ST_CHECK_INFO:
            tt_setup(TT_TAPE_PLAIN, scale, stretch);
            open_odd_profile_tape();
            tt_frames(3);
            tt_menu("menu_check");
            break;
        default:
            break;
    }
    tt_mouse_away();
    tt_frames(4);
}

/** @brief Geometrie stavu (dialog nebo okno kontroly). @return Počet vad. */
static int tt_state_faults(en_TT_STATE st, ImVec2 work, const char *ctx)
{
    ImVec2 wmin, wmax, imin, imax;
    if (st == TT_ST_INFO || st == TT_ST_INFO_FORMAT || st == TT_ST_INFO_RELOAD) {
        if (!cmt_tapeinfo_dialog_test_rect(&wmin, &wmax, &imin, &imax)) return 1;
        return tt_faults(cmt_tapeinfo_dialog_test_items(), imin, imax, ctx) + tt_size_faults(wmin, wmax, work, "###cmt_tapeinfo", ctx);
    }
    if (st == TT_ST_CONVERT_SPEED || st == TT_ST_CONVERT_NOSPEED) {
        if (!cmt_convert_dialog_test_rect(&wmin, &wmax, &imin, &imax)) return 1;
        return tt_faults(cmt_convert_dialog_test_items(), imin, imax, ctx) + tt_size_faults(wmin, wmax, work, "###cmt_convert", ctx);
    }
    if (!cmt_check_window_test_rect(&wmin, &wmax, &imin, &imax)) return 1;
    return tt_faults(cmt_check_window_test_items(), imin, imax, ctx) + tt_size_faults(wmin, wmax, work, TT_CHECK_ID, ctx);
}

/*
 * Geometrie všech stavů při 100 % (pracovní plocha
 * 1366 x 728) a 175 % (1920 x 1040), texty běžné i o 40 % delší: dialog
 * i okno nejvýš 90 % pracovní plochy, v ploše viewportu, nic oříznuté ani
 * překryté, okno samo se neposouvá.
 */
void test_j5_geometry(void)
{
    const float scales[2] = {1.0f, 1.75f};
    int faults = 0;
    char ctx[128];
    for (float sc : scales) {
        for (int stretch = 0; stretch < 2; stretch++) {
            for (int st = 0; st < TT_ST_COUNT; st++) {
                ImVec2 work = (sc > 1.5f) ? ImVec2(1920.0f, 1040.0f) : ImVec2(1366.0f, 728.0f);
                tt_prepare((en_TT_STATE)st, sc, stretch != 0);
                imgui_cmt_window_test_set_work_size(work);
                tt_frames(4);
                snprintf(ctx, sizeof(ctx), "j5 geometry %.2f%s %s", sc, stretch ? " +40" : "", s_state_names[st]);
                faults += tt_state_faults((en_TT_STATE)st, work, ctx);
                tearDown();
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(0, faults);
}

/**
 * @brief Jako tt_setup() při 175 % na displeji 1920 x 1040 (hlavní viewport i pracovní plocha), okno kazety
 *        výchozí šířky 30 výšek rámečku a výšky celé plochy.
 * @param tape Páska. @param stretch Texty o 40 % delší.
 */
static void tt_setup_1920(en_TT_TAPE tape, bool stretch)
{
    s_display = ImVec2(1920.0f, 1040.0f);
    tt_setup(tape, 1.75f, stretch);
    imgui_cmt_window_test_set_work_size(s_display);
    tt_frames(4);
}

/** @brief Protínají se dva obdélníky (víc než o 0,5 px)? */
static bool tt_overlap(ImVec2 amin, ImVec2 amax, ImVec2 bmin, ImVec2 bmax)
{
    return ImMin(amax.x, bmax.x) - ImMax(amin.x, bmin.x) > 0.5f && ImMin(amax.y, bmax.y) - ImMax(amin.y, bmin.y) > 0.5f;
}

/** @brief Index okna v pořadí kreslení ImGui (vyšší = navrchu), -1 = není. */
static int tt_window_order(const char *name)
{
    ImGuiWindow *w = ImGui::FindWindowByName(name);
    if (!w) return -1;
    for (int i = 0; i < GImGui->Windows.Size; i++)
        if (GImGui->Windows[i] == w->RootWindow) return i;
    return -1;
}

/*
 * Při 175 % na 1920 x 1040 okno Tape check nezakrývá tabulku bloků okna kazety; klik na nález
 * vybere řádek a odroluje ho do pohledu, vybraný řádek nezakrývá žádné okno a okno kontroly zůstane navrchu.
 */
void test_e4b_check_window_beside_table(void)
{
    tt_setup_1920(TT_TAPE_FINDINGS, false);
    tt_menu("menu_check");
    tt_frames(5);
    ImVec2 cmin, cmax, cimin, cimax;
    TEST_ASSERT_TRUE(cmt_check_window_test_rect(&cmin, &cmax, &cimin, &cimax));
    const st_CMT_WINDOW_TEST_ITEM *tb = w_item("table");
    TEST_ASSERT_NOT_NULL(tb);
    fprintf(stderr, "check window (%.0f,%.0f)-(%.0f,%.0f), table (%.0f,%.0f)-(%.0f,%.0f)\n", cmin.x, cmin.y, cmax.x, cmax.y, tb->min.x,
            tb->min.y, tb->max.x, tb->max.y);
    TEST_ASSERT_FALSE_MESSAGE(tt_overlap(cmin, cmax, tb->min, tb->max), "the Tape check window does not cover the block table");
    TEST_ASSERT_TRUE_MESSAGE(cmin.x >= -0.5f && cmin.y >= -0.5f && cmax.x <= s_display.x + 0.5f && cmax.y <= s_display.y + 0.5f,
                             "the Tape check window lies on the work area");
    /* klik na druhý nález (blok 4 od 1) */
    tt_click_item(c_item("ck_row_1"));
    tt_frames(3);
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    const st_CMT_WINDOW_TEST_ITEM *row = w_item(nm("name", 3));
    TEST_ASSERT_NOT_NULL_MESSAGE(row, "the selected row is drawn");
    TEST_ASSERT_TRUE_MESSAGE(row->min.y >= row->clip_min.y - 0.5f && row->max.y <= row->clip_max.y + 0.5f, "the selected row is scrolled into view");
    TEST_ASSERT_TRUE(cmt_check_window_test_rect(&cmin, &cmax, &cimin, &cimax));
    const ImVec2 rmin(tb->min.x, row->min.y), rmax(tb->max.x, row->max.y);
    TEST_ASSERT_FALSE_MESSAGE(tt_overlap(cmin, cmax, rmin, rmax), "no window covers the selected row");
    TEST_ASSERT_TRUE_MESSAGE(tt_window_order(TT_CHECK_ID) > tt_window_order(TT_W_ID), "the Tape check window stays visible in front");
}

/*
 * Mezi sloupcem bloku a textem nálezu aspoň 24 px (100 %).
 */
void test_e4b_check_column_gap(void)
{
    tt_setup(TT_TAPE_FINDINGS);
    tt_menu("menu_check");
    tt_frames(2);
    for (int i = 0; i < 2; i++) {
        const st_CMT_WINDOW_TEST_ITEM *b = c_item(nm("ck_block", i));
        const st_CMT_WINDOW_TEST_ITEM *t = c_item(nm("ck_text", i));
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE_MESSAGE(t->min.x - b->max.x >= 24.0f - 0.5f, "gap between the block and the text of a finding");
    }
}

/*
 * Při 175 % na 1920 x 1040 je formulář celý vidět bez posunu (texty běžné), s delšími texty
 * hrana posouvané oblasti neprotíná pole ani popisek; pole přijme nejvýš 255 znaků; nad tlačítky aspoň 8 px (100 %).
 */
void test_e4b_info_form_fits(void)
{
    for (int stretch = 0; stretch < 2; stretch++) {
        tt_setup_1920(TT_TAPE_INFO, stretch != 0);
        tt_open_info();
        tt_frames(4);
        int cut = 0;
        for (const st_CMT_WINDOW_TEST_ITEM &it : cmt_tapeinfo_dialog_test_items()) {
            if (!it.scroll_y) continue;
            const bool c = (it.min.y < it.clip_max.y - 0.5f && it.max.y > it.clip_max.y + 0.5f)
                           || (it.min.y < it.clip_min.y - 0.5f && it.max.y > it.clip_min.y + 0.5f);
            if (c) fprintf(stderr, "+%d: '%s' cut by the edge of the scrolled form\n", stretch * 40, it.name.c_str());
            cut += c ? 1 : 0;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, cut, "the edge of the scrolled form cuts no field or label");
        if (!stretch) {
            const st_CMT_WINDOW_TEST_ITEM *m = i_item("ti_machine");
            TEST_ASSERT_NOT_NULL(m);
            TEST_ASSERT_TRUE_MESSAGE(m->max.y <= m->clip_max.y + 0.5f, "Target machine is visible without scrolling");
        }
        tearDown();
    }
    /* 100 %: dvojice polí na jednom řádku se stejně vysokými popisky, mez 255 znaků při psaní, mezera nad tlačítky */
    tt_setup(TT_TAPE_INFO);
    tt_open_info();
    for (int f = 3; f <= 7; f += 2) {
        const st_CMT_WINDOW_TEST_ITEM *l = i_item(nm("ti_label", f));
        const st_CMT_WINDOW_TEST_ITEM *r = i_item(nm("ti_label", f + 1));
        const st_CMT_WINDOW_TEST_ITEM *fl = i_item(nm("ti_field", f));
        const st_CMT_WINDOW_TEST_ITEM *fr = i_item(nm("ti_field", f + 1));
        TEST_ASSERT_NOT_NULL(l);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, l->min.y, r->min.y, "both labels of a pair on one line");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, fl->min.y, fr->min.y, "both fields of a pair on one line");
        TEST_ASSERT_TRUE_MESSAGE(r->min.x > fl->max.x, "the second label right of the first field");
    }
    tt_click_item(i_item("ti_field_0"));
    std::string longer(300, 'x');
    ImGui::GetIO().AddInputCharactersUTF8(longer.c_str());
    tt_frames(2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(255u, (uint32_t)g_utf8_strlen(item_text(i_item("ti_field_0")).c_str(), -1),
                                     "a field takes at most 255 characters");
    tt_key(ImGuiKey_Escape);
    const st_CMT_WINDOW_TEST_ITEM *m = i_item("ti_machine");
    const st_CMT_WINDOW_TEST_ITEM *ok = i_item("ti_ok");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE_MESSAGE(ok->min.y - m->max.y >= 8.0f - 0.5f, "at least 8 px above the buttons");
}

/*
 * Vybraný profil výplní výběru tabulky (row-selected, jiná než primární tlačítko);
 * při 100 % aspoň 8 profilů v jednom řádku; popisy s jednotkou µs. Značka nejistoty
 * "[unverified]" z knihovny zůstává (profil UNICMT), popis je jinak týž jako v knihovně.
 */
void test_e4b_convert_list(void)
{
    tt_setup(TT_TAPE_PLAIN);
    tt_open_convert(0);
    tt_frames(2);
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", (unsigned)CMT_TOK_ROW_SELECTED);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hex, item_text(v_item("cv_sel_fill")).c_str(), "the selected profile uses the table selection fill");
    TEST_ASSERT_TRUE(CMT_TOK_ROW_SELECTED != CMT_TOK_SEGMENT_ON);
    const float Lh = ImGui::GetTextLineHeight();
    int one_line = 0, rows = 0;
    bool micro = false, unverified = false;
    for (int i = 0; v_item(nm("cv_desc", i)); i++) {
        const st_CMT_WINDOW_TEST_ITEM *d = v_item(nm("cv_desc", i));
        rows++;
        if (d->max.y - d->min.y <= Lh + 0.5f) one_line++;
        TEST_ASSERT_NULL_MESSAGE(strstr(d->text.c_str(), " us"), d->text.c_str());
        /* UNICMT: knihovna uvádí neověřenou strukturu - okno ji ukáže jako MCP */
        if (item_text(v_item(nm("cv_id", i))) == "UNICMT") {
            const char *lib = NULL;
            for (uint32_t k = 0; k < cmttmz_profile_count(); k++) {
                const char *id = NULL, *desc = NULL;
                int speed = 0;
                if (EXIT_SUCCESS == cmttmz_profile_get(k, &id, &desc, &speed) && id && !strcmp(id, "UNICMT")) lib = desc;
            }
            TEST_ASSERT_NOT_NULL(lib);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(lib, "[unverified]"), lib);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(lib, d->text.c_str(), "the description of the library, unverified mark included");
            unverified = true;
        }
        if (strstr(d->text.c_str(), "\xC2\xB5s")) micro = true;
    }
    fprintf(stderr, "%d of %d profiles in one line\n", one_line, rows);
    /* "(current)" hned pod ID aktuálního profilu, uvnitř jeho řádku */
    for (int i = 0; i < rows; i++) {
        const st_CMT_WINDOW_TEST_ITEM *c = v_item(nm("cv_current", i));
        if (!c) continue;
        const st_CMT_WINDOW_TEST_ITEM *id = v_item(nm("cv_id", i));
        TEST_ASSERT_NOT_NULL(id);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, id->max.y, c->min.y, "(current) right under the profile ID");
        const st_CMT_WINDOW_TEST_ITEM *next = v_item(nm("cv_id", i + 1));
        if (next) TEST_ASSERT_TRUE_MESSAGE(c->max.y <= next->min.y + 0.5f, "(current) inside its row");
    }
    TEST_ASSERT_TRUE_MESSAGE(one_line >= 8, "at least 8 profiles in one line at 100 %");
    TEST_ASSERT_TRUE_MESSAGE(micro, "pulse lengths in µs");
    TEST_ASSERT_TRUE_MESSAGE(unverified, "the UNICMT profile is listed with the mark of the library");
}

/* ================================================================
 * Snímky (MZ_CMT_SHOT_DIR, sada j5)
 * ================================================================ */

static char s_shot_tag[64] = "en-100";

/** @brief Uloží levý horní výřez plátna jako PNG. */
static bool shot_save_crop(const char *dir, const char *file, int w, int h)
{
    /* s náhradní velikostí displeje (s_display) jen jeho plocha */
    const int lim_w = (s_display.x > 0.0f) ? (int)s_display.x : s_canvas_w;
    const int lim_h = (s_display.y > 0.0f) ? (int)s_display.y : s_canvas_h;
    if (w > lim_w) w = lim_w;
    if (h > lim_h) h = lim_h;
    if (w <= 0 || h <= 0) return false;
    size_t n = (size_t)s_canvas_w * (size_t)s_canvas_h * 4u;
    unsigned char *px = (unsigned char *)malloc(n);
    unsigned char *crop = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!px || !crop) { free(px); free(crop); return false; }
    glReadPixels(0, 0, s_canvas_w, s_canvas_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    size_t src_pitch = (size_t)s_canvas_w * 4u;
    size_t dst_pitch = (size_t)w * 4u;
    /* s náhradní velikostí displeje kreslí ImGui do spodní části framebufferu GL (osa y nahoru) */
    const int disp_h = (s_display.y > 0.0f) ? (int)s_display.y : s_canvas_h;
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * dst_pitch, px + (size_t)(disp_h - 1 - y) * src_pitch, dst_pitch);
    size_t len = 0;
    uint8_t *png = png_encode_rgba(crop, (uint32_t)w, (uint32_t)h, &len);
    bool ok = false;
    if (png) {
        gchar *path = g_build_filename(dir, file, NULL);
        ok = g_file_set_contents(path, (const gchar *)png, (gssize)len, NULL);
        g_free(path);
        g_free(png);
    }
    free(px);
    free(crop);
    return ok;
}

/** @brief Snímky nástrojů nad páskou (všechny stavy). @return Počet neuložených snímků a snímků s vadou. */
static int shots_j5_run(const char *dir, float scale)
{
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    int failed = 0;
    for (int s = 0; s < TT_ST_COUNT; s++) {
        setUp();
        /* 175 %: displej 1920 x 1040 (hlavní viewport, okno kazety v ploše; okno kontroly vedle tabulky) */
        if (scale > 1.5f) s_display = ImVec2(1920.0f, 1040.0f);
        tt_prepare((en_TT_STATE)s, scale, stretch);
        ImVec2 work = (scale > 1.5f) ? ImVec2(1920.0f, 1040.0f) : ImVec2(0.0f, 0.0f);
        if (scale > 1.5f) imgui_cmt_window_test_set_work_size(work);
        tt_mouse_away();
        for (int i = 0; i < 3; i++) tt_frame(true);
        ImVec2 sz(0.0f, 0.0f);
        ImGuiContext &g = *GImGui;
        for (ImGuiWindow *w : g.Windows) {
            if (!w->Active || w->Hidden) continue;
            sz.x = ImMax(sz.x, ceilf(w->Pos.x + w->Size.x) + 1.0f);
            sz.y = ImMax(sz.y, ceilf(w->Pos.y + w->Size.y) + 1.0f);
        }
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("j5-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "", dash ? dash : "", s_state_names[s]);
        if (!shot_save_crop(dir, file, (int)sz.x, (int)sz.y)) failed++;
        ImVec2 real_work = (work.x > 0.0f) ? work : ImGui::GetMainViewport()->WorkSize;
        int faults = tt_state_faults((en_TT_STATE)s, real_work, file);
        fprintf(stderr, "geo: %s crop %.0f x %.0f px, font %.1f px\n", file, sz.x, sz.y, ImGui::GetFontSize());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
        tearDown();
    }
    return failed;
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    const char *shot_dir = getenv("MZ_CMT_SHOT_DIR");
    if (shot_dir && *shot_dir) {
        const char *ev;
        if ((ev = getenv("MZ_CMT_SHOT_W")) && atoi(ev) > 0) s_canvas_w = atoi(ev);
        if ((ev = getenv("MZ_CMT_SHOT_H")) && atoi(ev) > 0) s_canvas_h = atoi(ev);
        const char *lang = getenv("MZ_CMT_SHOT_LANG");
        if (lang && (strchr(lang, '/') || strchr(lang, '\\') || strstr(lang, ".."))) {
            fprintf(stderr, "MZ_CMT_SHOT_LANG must not contain '/', '\\' or '..'\n");
            return 1;
        }
        const char *ldir = getenv("MZ_CMT_SHOT_LOCALEDIR");
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
        float scale = (ev = getenv("MZ_CMT_SHOT_SCALE")) ? (float)g_ascii_strtod(ev, NULL) : 1.0f;
        if (scale <= 0.0f) scale = 1.0f;
        snprintf(s_shot_tag, sizeof(s_shot_tag), "%s-%03d", (lang && *lang) ? lang : "en", (int)(scale * 100.0f + 0.5f));
        const char *font = getenv("MZ_CMT_SHOT_FONT");
        if (font && *font) {
            static const ImWchar ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
            ImFont *f = io.Fonts->AddFontFromFileTTF(font, 28.0f, NULL, ranges);
            static const ImWchar icons_ranges[] = {ICON_MIN_IGFD, ICON_MAX_IGFD, 0};
            ImFontConfig icfg;
            icfg.MergeMode = true;
            icfg.PixelSnapH = true;
            io.Fonts->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_IGFD, 28.0f, &icfg, icons_ranges);
            if (f) io.FontDefault = f;
        }
        const char *mono = getenv("MZ_CMT_SHOT_MONO");
        if (mono && *mono) s_mono = io.Fonts->AddFontFromFileTTF(mono, 28.0f);
        s_fonts_done = true;
        fn_gen_fb gen_fb = (fn_gen_fb)SDL_GL_GetProcAddress("glGenFramebuffers");
        fn_fb_tex2d fb_tex = (fn_fb_tex2d)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        fn_check_fb check_fb = (fn_check_fb)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
        s_bind_fb = (fn_bind_fb)SDL_GL_GetProcAddress("glBindFramebuffer");
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
            s_bind_fb = NULL;
        }
        int failed = shots_j5_run(shot_dir, scale);
        ui_teardown();
        mztest_teardown();
        return failed ? 1 : 0;
    }

    s_ui_ok = ui_setup();
    if (!s_ui_ok) {
        mztest_teardown();
        fprintf(stderr, "ui-cmt-tools: SDL/GL not available, skipped\n");
        return 77;
    }
    UNITY_BEGIN();
    RUN_TEST(test_j5_entry_points);
    RUN_TEST(test_j5_info_form_reads_values);
    RUN_TEST(test_j5_info_ok_without_change);
    RUN_TEST(test_j5_info_ok_sends_changes);
    RUN_TEST(test_j5_info_notes);
    RUN_TEST(test_j5_info_tape_changed_keeps_edits);
    RUN_TEST(test_finalb_info_reload_other_tape);
    RUN_TEST(test_finalb_modals_kept_without_window_content);
    RUN_TEST(test_j5_info_escape_in_combo);
    RUN_TEST(test_j5_info_foreign_entries_kept);
    RUN_TEST(test_j5_info_keeps_selection);
    RUN_TEST(test_j5_check_window);
    RUN_TEST(test_j5_check_no_problems);
    RUN_TEST(test_finalb_check_outdated_after_recording);
    RUN_TEST(test_j5_check_capped);
    RUN_TEST(test_j5_check_bench);
    RUN_TEST(test_j5_convert_dialog);
    RUN_TEST(test_j5_convert_escape_in_combo);
    RUN_TEST(test_j5_convert_tape_changed);
    RUN_TEST(test_j5_convert_unknown_current_profile);
    RUN_TEST(test_j5_core_text_numbers);
    RUN_TEST(test_j5_open_single_raw_hint);
    RUN_TEST(test_j5_import_error_keeps_path);
    RUN_TEST(test_j5_geometry);
    RUN_TEST(test_e4b_check_window_beside_table);
    RUN_TEST(test_e4b_check_column_gap);
    RUN_TEST(test_e4b_info_form_fits);
    RUN_TEST(test_e4b_convert_list);
    int result = UNITY_END();
    ui_teardown();
    mztest_teardown();
    return result;
}
