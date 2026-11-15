/*
 * test_cmt_analyze.cpp - dialog "Analyze recording - block N" okna Virtual CMT
 * (src/ui-imgui/cmt/cmt_analyze_dialog.cpp) se skutečným ImGui
 * ve skrytém okně SDL + GL nad skutečným jádrem kazety a frontou zásahů.
 *
 * Páska testu: PRE (MZF), surový blok 0x18 se 60 000 pulzy (krátké a dlouhé
 * jako signál Sharp, na konci šum), POST (MZF). Analýzu dělá testovací
 * analyzátor: syntetický výsledek analyzátoru (cmttmz_analysis_test_from_result())
 * se soubory LOADER a FLAPPY bez chyby, HISCORE s chybou součtu těla a neznámým
 * úsekem na konci (koncept cmt-analyze-v2), nebo výsledek bez souborů.
 * Pracovní "vlákno" je v testech řízené (cmt_analyze_dialog_test_set_manual_runner()):
 * úloha čeká ve frontě testu, dokud ji test neprovede - testy nespí ani
 * nečekají na čas (výjimka: jeden test se skutečným vláknem GLib, který čeká
 * na příznak hotovo s horní mezí).
 *
 * Snímek okna = kreslení okna (imgui_cmt_window(), kopie stavu z jádra),
 * imgui_cmt_tape_dialogs(), device_request_poll() (emu vlákno) a vyřízení
 * výsledků (imgui_device_request_results()), tedy stejná cesta jako
 * aplikace.
 *
 * Pokrytí: vstupy Analyze... (kontextové menu za Set
 * speed, menu Tape) a jejich zákazy (PLAY, jiný než surový blok, záznam
 * a přehrávání vstupů); rozvržení a texty podle SPEC a konceptu v2 (pořadí
 * prvků, sloupce, Checksum místo CRC, výsledky, zaškrtnutí, výchozí fokus,
 * sbalené Signal details); Enter = Add (apply replace 0, skutečné uplatnění,
 * zpráva a výběr prvního přidaného bloku); Replace u neuloženého bloku
 * s varováním (replace 1); odškrtnutí všeho zakáže akce; změna volby =
 * zastaralé výsledky, Analyze again je obnoví s novými volbami; stav běží
 * (akce zakázané, Cancel, Escape, zavření okna a ukončení aplikace analýzu
 * opustí a data se uvolní, až úloha doběhne); změna pásky
 * zakáže akce s textem; nic nenalezeno (Signal details rozbalené,
 * histogram) a chyba analýzy; nápověda vybraného řádku, tooltip důvodu,
 * mezerník, histogram vybraného souboru se značkami; zavření dialogu
 * a ztracený výsledek se zásahem uplatnění ve frontě (data drží modul do
 * výsledku); skutečné vlákno; geometrie při 100 % a 175 % i s texty
 * o 40 % delšími a na malé pracovní ploše s rozbaleným histogramem. Dále:
 * druhá analýza čeká na opuštěnou, pozdní kopie
 * pulzů se zahodí, Checksum u souboru bez těla, zpráva bez výsledku úpravy,
 * popup a otevření při volnu, ukončení s běžícím vláknem (připojení /
 * odpojení), nejdelší pulz jednou za kopii, Enter na zaškrtávátku; s
 * MZ_CMT_BENCH=1 měření bloku 10 000 000 pulzů (jinak IGNORE). Dále blok
 * 500 000 pulzů se skutečným analyzátorem ve skutečném vlákně
 * (dialog kreslí, Cancel, úloha se uvolní sama; bez měření času) a dialog
 * přežije snímek bez obsahu okna kazety (sbalené okno).
 *
 * Režim snímků (mimo CTest): MZ_CMT_SHOT_DIR = adresář; jako ui-cmt-window
 * MZ_CMT_SHOT_FONT, MZ_CMT_SHOT_MONO, MZ_CMT_SHOT_SCALE, MZ_CMT_SHOT_LANG
 * + MZ_CMT_SHOT_LOCALEDIR, MZ_CMT_SHOT_W / _H, MZ_CMT_SHOT_STRETCH=1.
 * Sada j4: found (vybraný HISCORE s chybou těla), empty (nic nenalezeno,
 * rozbalené Signal details), running, stale, details (vybraný HISCORE
 * s rozbaleným histogramem); při měřítku nad 1,5 pracovní plocha
 * 1920 x 1040 (tělo dialogu se posouvá). Jména
 * "j4-<jazyk>-<měřítko v %>-<stav>.png", výřez okna i modálu, "geo:"
 * a "check:" na stderr.
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
#include "hw-generic/cmt/cmt_tmz_analyze.h"
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
#include "libs/wav_analyzer/wav_analyzer.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/cmt/cmt_window.h"
#include "ui-imgui/cmt/cmt_analyze_dialog.h"
#include "ui-imgui/cmt/cmt_tokens.h"
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
 * Páska testu
 * ================================================================ */

/** @brief Dočasný soubor pásky (maže tearDown). */
static char *s_path = NULL;

/** @brief Počet pulzů surového bloku testu. */
#define AT_PULSES 60000u

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

/** @brief Přidá blok 0x46 (MZ800EMU-CMT) s MZF do souboru. */
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

/**
 * @brief Délky pulzů surového bloku: krátké a dlouhé (asi 250 a 500 µs při GDGCLK_BASE / 2), na konci šum.
 * @param n Počet. @return Pole (uvolnit free()).
 */
static uint32_t *raw_lengths(uint32_t n)
{
    uint32_t *p = (uint32_t *)malloc(n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        if (i >= 52000u) p[i] = 600u + (i * 97u) % 900u;
        else if (i % 4u == 0u) p[i] = 4430u + (i * 13u) % 300u - 150u;
        else p[i] = 2215u + (i * 7u) % 200u - 100u;
    }
    return p;
}

/** @brief Surový blok 0x18 s pulzy raw_lengths(). @param b Výstup (data převezme soubor nebo páska). */
static void make_raw_block(st_TZX_BLOCK *b, uint32_t n)
{
    uint32_t *p = raw_lengths(n);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(GDGCLK_BASE / 2, 1000, p, n, b));
    free(p);
}

/** @brief Uloží pásku PRE, surový blok, POST a otevře ji (blok 1 surový, uložený). */
static void open_analyze_tape(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    add_mzf(f, "PRE", 64);
    st_TZX_BLOCK raw;
    make_raw_block(&raw, AT_PULSES);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &raw));
    add_mzf(f, "POST", 64);
    gchar *dir = g_strdup_printf("mztest_cmt_analyze_%u", (unsigned)g_random_int());
    gchar *full = g_build_filename(g_get_tmp_dir(), dir, NULL);
    g_mkdir_with_parents(full, 0700);
    g_free(s_path);
    s_path = g_build_filename(full, "analyze.tmz", NULL);
    g_free(dir);
    g_free(full);
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    TEST_ASSERT_TRUE(g_file_set_contents(s_path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    device_state_lock();
    int rc = cmt_open_file(s_path, NULL);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
}

/** @brief Připojí na konec pásky neuložený surový blok (jako záznam; blok 3). @param n Počet pulzů. */
static void append_unsaved_raw(uint32_t n = AT_PULSES)
{
    st_TZX_BLOCK raw;
    make_raw_block(&raw, n);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_NONE;
    device_state_lock();
    int rc = cmttmz_append_blocks(&raw, &sp, 1);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    free(raw.data);
}

/** @brief Kopie stavu z jádra (pod zámkem). */
static void fill(st_CMT_VIEW *v)
{
    device_state_lock();
    cmt_view_fill(v);
    device_state_unlock();
}

/* ================================================================
 * Testovací analyzátor (syntetický výsledek analyzátoru)
 * ================================================================ */

/** @brief Druh výsledku testovacího analyzátoru. */
typedef enum en_AT_SYN {
    AT_SYN_FOUND = 0, /**< LOADER, FLAPPY, HISCORE (chyba těla), neznámý úsek */
    AT_SYN_NONE,      /**< žádný soubor (jen neznámý úsek přes celý blok) */
    AT_SYN_MANY,      /**< 120 souborů bez chyby (dlouhá tabulka) */
    AT_SYN_REAL,      /**< skutečný analyzátor cmttmz_analyze_pulses() */
    AT_SYN_FAIL,      /**< analyzátor bez výsledku (NULL, jako selhání alokace) */
    AT_SYN_HEADER_ONLY, /**< LOADER jen s hlavičkou (součet těla není k dispozici, oprava HEADER_ONLY) */
    AT_SYN_SLOW,      /**< jako FOUND, ale analyzátor 400 ms spí (běžící vlákno při ukončení) */
    AT_SYN_CANNOT,    /**< LOADER (NORMAL) a GAME ve formátu TURBO (řádek Cannot add, snímek s nápovědou) */
    AT_SYN_GATED,     /**< skutečný analyzátor, po něm vlákno čeká na otevření s_syn_gate (analýza "běží", dokud ji test nepustí) */
} en_AT_SYN;

static en_AT_SYN s_syn = AT_SYN_FOUND;
/** @brief Volby posledního volání testovacího analyzátoru. */
static st_CMTTMZ_ANALYZE_OPTS s_syn_opts;
/** @brief Počet volání testovacího analyzátoru. */
static int s_syn_calls = 0;
/** @brief Brána AT_SYN_GATED: 0 = pracovní vlákno čeká, 1 = vrátí výsledek (atomicky; tearDown ji otevře). */
static gint s_syn_gate = 1;

/** @brief MZF pro syntetický výsledek (hlavička v pořadí hostitele). */
static st_MZF *syn_mzf(const char *name, uint8_t ftype, uint16_t fsize)
{
    st_MZF *z = (st_MZF *)calloc(1, sizeof(st_MZF));
    make_hdr(&z->header, name, ftype, fsize);
    z->body = (uint8_t *)malloc(fsize);
    for (int i = 0; i < fsize; i++) z->body[i] = (uint8_t)(i * 5 + 7);
    z->body_size = fsize;
    return z;
}

/** @brief Přidá soubor NORMAL do syntetického výsledku. */
static void syn_add(st_WAV_ANALYZER_RESULT *r, uint32_t start, uint32_t end, st_MZF *mzf, en_WAV_CRC_STATUS body)
{
    r->files = (st_WAV_ANALYZER_FILE_RESULT *)realloc(r->files, (r->file_count + 1) * sizeof(st_WAV_ANALYZER_FILE_RESULT));
    st_WAV_ANALYZER_FILE_RESULT *f = &r->files[r->file_count++];
    r->file_capacity = r->file_count;
    memset(f, 0, sizeof(*f));
    f->format = WAV_TAPE_FORMAT_NORMAL;
    f->header_leader_pulse = start;
    f->leader.start_index = start;
    f->consumed_until_pulse = end;
    f->header_crc = WAV_CRC_OK;
    f->body_crc = body;
    f->short_pulse_us = 250.0;
    f->long_pulse_us = 500.0;
    f->mzf = mzf;
}

/** @brief Testovací analyzátor (cmt_analyze_dialog_analyzer_fn). */
static st_CMTTMZ_ANALYSIS *syn_analyzer(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts)
{
    s_syn_calls++;
    if (opts) s_syn_opts = *opts;
    if (s_syn == AT_SYN_REAL) return cmttmz_analyze_pulses(p, opts);
    if (s_syn == AT_SYN_GATED) {
        st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, opts);
        /* mez jen proti uváznutí testu, který bránu neotevře (ne měření času) */
        const gint64 t0 = g_get_monotonic_time();
        while (!g_atomic_int_get(&s_syn_gate) && g_get_monotonic_time() - t0 < 120 * G_USEC_PER_SEC) g_usleep(1000);
        return a;
    }
    if (s_syn == AT_SYN_FAIL) return NULL;
    if (s_syn == AT_SYN_SLOW) g_usleep(400000);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    if (s_syn == AT_SYN_HEADER_ONLY) {
        syn_add(&res, 10, 15000, syn_mzf("LOADER", 0x01, 1024), WAV_CRC_NOT_AVAILABLE);
        res.files[0].recovery_status = WAV_RECOVERY_HEADER_ONLY;
    } else if (s_syn == AT_SYN_FOUND || s_syn == AT_SYN_SLOW) {
        syn_add(&res, 10, 15000, syn_mzf("LOADER", 0x01, 1024), WAV_CRC_OK);
        syn_add(&res, 15000, 40000, syn_mzf("FLAPPY", 0x01, 12288), WAV_CRC_OK);
        syn_add(&res, 40000, 52000, syn_mzf("HISCORE", 0x03, 512), WAV_CRC_ERROR);
    } else if (s_syn == AT_SYN_CANNOT) {
        syn_add(&res, 10, 15000, syn_mzf("LOADER", 0x01, 1024), WAV_CRC_OK);
        syn_add(&res, 15000, 40000, syn_mzf("GAME", 0x01, 12288), WAV_CRC_OK);
        res.files[1].format = WAV_TAPE_FORMAT_TURBO;
    } else if (s_syn == AT_SYN_MANY) {
        for (uint32_t k = 0; k < 120; k++) {
            char nm[16];
            snprintf(nm, sizeof(nm), "FILE%03u", (unsigned)k);
            syn_add(&res, 10 + k * 400, 10 + (k + 1) * 400, syn_mzf(nm, 0x01, 128), WAV_CRC_OK);
        }
    }
    return cmttmz_analysis_test_from_result(p, opts, &res);
}

/* ================================================================
 * Snímky okna a dialogu
 * ================================================================ */

static ImFont *s_mono = NULL;
/* Stub: okno bere velké počítadlo a čísla z monospace písma aplikace. */
ImFont *myimgui_get_monospace_font(void) { return s_mono; }

#define AT_W_ID "###virtual_cmt"

/** @brief Okno Virtual CMT je otevřené (g_gui->showVirtualCmtWindow testu). */
static bool s_open = true;
/** @brief Snímek provede i zásahy fronty (device_request_poll()). */
static bool s_poll = true;
/** @brief Pevný krok času snímku (s), 0 = backend. */
static float s_dt = 0.0f;
/** @brief Funkce testu, která se jednou zavolá uvnitř příštího snímku po kreslení okna a dialogů (NULL = žádná). */
static void (*s_in_frame)(void) = NULL;

/** @brief Písma už načtená (at_fonts() nebo režim snímků z proměnných). */
static bool s_fonts_done = false;

/** @brief Načte písma aplikace (DroidSans a Cousine 28 px) jednou; bez souborů výchozí písmo ImGui. */
static void at_fonts(void)
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

static void at_scale(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

/**
 * @brief Jeden snímek aplikace: okno Virtual CMT, dialogy kazety, emu vlákno (zásahy), výsledky zásahů.
 * @param render true = vykreslit do GL (snímky).
 */
static void at_frame(bool render = false)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (s_dt > 0.0f) ImGui::GetIO().DeltaTime = s_dt;
    ImGui::NewFrame();
    if (ImGui::FindWindowByName(AT_W_ID)) ImGui::SetWindowPos(AT_W_ID, ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    device_state_lock();
    if (s_open) imgui_cmt_window(&s_open);
    imgui_cmt_tape_dialogs();
    /* zásah testu uvnitř snímku (po kreslení okna a dialogů) */
    if (s_in_frame) {
        s_in_frame();
        s_in_frame = NULL;
    }
    device_state_unlock();
    if (s_poll) device_request_poll();
    imgui_device_request_results();
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

static void at_frames(int n)
{
    for (int i = 0; i < n; i++) at_frame();
}

/** @brief Prvek okna Virtual CMT z posledního snímku. */
static const st_CMT_WINDOW_TEST_ITEM *w_item(const char *name)
{
    for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items())
        if (it.name == name) return &it;
    return NULL;
}

/** @brief Prvek dialogu z posledního snímku. */
static const st_CMT_WINDOW_TEST_ITEM *d_item(const char *name)
{
    for (const st_CMT_WINDOW_TEST_ITEM &it : cmt_analyze_dialog_test_items())
        if (it.name == name) return &it;
    return NULL;
}

/** @brief Text prvku dialogu ("" = nebyl). */
static std::string d_text(const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = d_item(name);
    return it ? it->text : std::string();
}

/** @brief Kreslil se modál dialogu v posledním snímku? @return true = ano. */
static bool d_drawn(void)
{
    ImVec2 a, b, c, d;
    return cmt_analyze_dialog_test_rect(&a, &b, &c, &d);
}

/** @brief Jméno prvku s indexem ("ad_name_2"). */
static std::string nm(const char *base, int i)
{
    return std::string(base) + "_" + std::to_string(i);
}

/** @brief Klik na střed prvku (okna nebo dialogu). @param it Prvek. @param button Tlačítko myši. */
static void at_click_item(const st_CMT_WINDOW_TEST_ITEM *it, int button)
{
    TEST_ASSERT_NOT_NULL(it);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    at_frame();
    io.AddMouseButtonEvent(button, true);
    at_frame();
    io.AddMouseButtonEvent(button, false);
    at_frame();
    at_frame();
}

static void w_click(const char *name, int button)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    at_click_item(it, button);
}

static void d_click(const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = d_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    at_click_item(it, 0);
}

/** @brief Stisk klávesy. */
static void at_key(ImGuiKey key)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    at_frame();
    io.AddKeyEvent(key, false);
    at_frame();
    at_frame();
}

/** @brief Myš pryč z oken. */
static void at_mouse_away(void)
{
    ImGui::GetIO().AddMousePosEvent(-1.0f, -1.0f);
    at_frame();
}

/** @brief Zahodí zásahy fronty bez provedení (bez device_request_clear()). */
static void at_drain(void)
{
    while (device_request_test_take_pending(NULL, NULL)) {}
}

/**
 * @brief Najde v zařazených zásazích zásah daného jména (fronta se vyprázdní).
 * @param name Název. @param[out] args Argumenty. @return true = nalezen.
 */
static bool at_take_request(const char *name, st_DEVICE_REQ_ARGS *args)
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

/** @brief Okno ve Full, pozice vlevo nahoře, velikost 30 x 23 výšek rámečku, páska testu otevřená. */
static void at_setup(float scale = 1.0f, bool stretch = false)
{
    at_fonts();
    at_scale(scale);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    imgui_cmt_window_test_set_stretch(stretch);
    imgui_cmt_window_test_reset();
    cmt_analyze_dialog_test_reset();
    cmt_analyze_dialog_test_set_manual_runner(true);
    cmt_analyze_dialog_test_set_analyzer(syn_analyzer);
    s_syn = AT_SYN_FOUND;
    s_syn_calls = 0;
    s_open = true;
    s_poll = true;
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    open_analyze_tape();
    at_frames(3);
    float F = ImGui::GetFrameHeight();
    ImGui::SetWindowSize(AT_W_ID, ImVec2(30.0f * F, 23.0f * F), ImGuiCond_Always);
    at_frames(4);
}

/** @brief Otevře dialog z kontextového menu řádku bloku @p block (bez provedení úlohy analýzy). */
static void at_open_dialog(int block)
{
    w_click(nm("name", block).c_str(), 1);
    w_click("ctx_analyze", 0);
    at_mouse_away();
    /* modál se ustálí (vystředění po změně velikosti), klik pak trefí jeho prvky */
    at_frames(3);
    TEST_ASSERT_TRUE_MESSAGE(cmt_analyze_dialog_is_open(), "Analyze... opens the dialog");
}

/** @brief Otevře dialog a provede analýzu (kopie pulzů zásahem, úloha synchronně). */
static void at_open_analyzed(int block)
{
    at_open_dialog(block);
    at_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_run_jobs(), "one analysis job after the pulse copy");
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
}

void setUp(void)
{
    g_replay_mode = REPLAY_MODE_OFF;
    cmt_view_test_force_window_shown(-1);
    at_drain();
}

void tearDown(void)
{
    if (device_state_lock_held()) device_state_unlock();
    g_replay_mode = REPLAY_MODE_OFF;
    /* pracovní vlákno testu velkého bloku nesmí čekat na bránu déle než test */
    g_atomic_int_set(&s_syn_gate, 1);
    /* zásahy zahodit bez provedení a výsledky bez vyřízení: data zásahů uplatnění pak uvolní reset dialogu */
    at_drain();
    {
        st_DEVICE_REQ_RESULT res;
        while (device_request_take_result(&res)) {}
    }
    if (s_ctx) {
        s_open = true;
        cmt_analyze_dialog_test_reset();
        imgui_cmt_window_test_set_stretch(false);
        imgui_cmt_window_test_set_work_size(ImVec2(0.0f, 0.0f));
        /* sbalené okno kazety (test snímku bez obsahu) nesmí přejít do dalšího testu */
        if (ImGui::FindWindowByName(AT_W_ID)) ImGui::SetWindowCollapsed(AT_W_ID, false);
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

/** @brief Spustí přehrávání bloku 1 (surový blok pásky testu). */
static void at_play_raw(void)
{
    device_state_lock();
    int rc = cmt_tape_seek_block(1);
    cmt_play();
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
}

/** @brief Zastaví pásku. */
static void at_stop(void)
{
    device_state_lock();
    cmt_stop();
    device_state_unlock();
}

/*
 * Analýza na pozadí: když hraje surový blok, okno si vezme kopii pulzů
 * a spustí analýzu; dokud blok hraje, nic nehlásí; po zastavení ohlásí
 * počet souborů, které jdou přidat; Analyze... pak otevře dialog rovnou
 * s výsledkem, bez nové kopie a nové analýzy.
 */
void test_auto_analyze_while_playing(void)
{
    at_setup();
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
    at_play_raw();
    /* snímky: zásah kopie pulzů, jeho výsledek, úloha ve frontě řízeného "vlákna" */
    at_frames(4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_queued_jobs(), "one background analysis while the raw block plays");
    TEST_ASSERT_FALSE_MESSAGE(cmt_analyze_dialog_is_open(), "the background analysis does not open the dialog");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_queued_jobs(), "the block is analyzed once");
    const st_CMT_WINDOW_TEST_ITEM *msg = w_item("message");
    TEST_ASSERT_TRUE_MESSAGE(!msg || msg->text.find("files found") == std::string::npos, "nothing is reported while the block plays");

    at_stop();
    at_frames(3);
    msg = w_item("message");
    TEST_ASSERT_NOT_NULL_MESSAGE(msg, "the find is reported after the tape stops");
    /* AT_SYN_FOUND: LOADER a FLAPPY jdou přidat, HISCORE má chybu těla; čísla bloků od 1 */
    TEST_ASSERT_TRUE_MESSAGE(msg->text.find("Block 2: files found in the recording: 2") != std::string::npos, msg->text.c_str());

    /* dialog převezme hotový výsledek */
    const int calls = s_syn_calls;
    at_open_dialog(1);
    at_frames(3);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(calls, s_syn_calls, "the dialog takes the background result, no new analysis");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_live_pulses());
    d_click("ad_cancel");
    at_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_pulses(), "the pulse copy is freed with the dialog");

    /* týž blok znovu: po převzetí výsledku se na pozadí neanalyzuje podruhé */
    at_play_raw();
    at_frames(4);
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
    at_stop();
}

/*
 * Volba Find files while playing vypnutá (menu Tape): hrající surový blok
 * se neanalyzuje (řádek zpráv okna se mezi testy nemaže, proto se neověřuje).
 */
void test_auto_analyze_disabled(void)
{
    at_setup();
    w_click("menu_tape", 0);
    TEST_ASSERT_NOT_NULL(w_item("menu_auto_analyze"));
    TEST_ASSERT_EQUAL_STRING("on", w_item("menu_auto_analyze")->text.c_str());
    w_click("menu_auto_analyze", 0);
    at_mouse_away();
    at_frames(2);
    at_play_raw();
    at_frames(4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_queued_jobs(), "no background analysis with the option off");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    at_stop();
    at_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_queued_jobs(), "nothing is analyzed after the stop either");
}

/*
 * Vstupy: kontextové menu řádku Analyze... (za Set speed, před Move up) a menu
 * Tape > Analyze recording... pro vybraný blok; zákazy s důvodem u jiného než
 * surového bloku, mimo STOP a při záznamu i přehrávání vstupů (fronta by
 * zásah odmítla). Povolená položka zařadí kopii pulzů s čítačem pásky a blokem.
 */
void test_j4_entry_points(void)
{
    at_setup();
    /* surový blok ve STOP: povoleno, pořadí v menu */
    w_click("name_1", 1);
    const st_CMT_WINDOW_TEST_ITEM *an = w_item("ctx_analyze");
    TEST_ASSERT_NOT_NULL_MESSAGE(an, "context menu has Analyze...");
    TEST_ASSERT_FALSE(an->disabled);
    TEST_ASSERT_EQUAL_STRING("Analyze...", an->text.c_str());
    TEST_ASSERT_TRUE_MESSAGE(w_item("ctx_speed")->min.y < an->min.y, "Analyze... is after Set speed");
    TEST_ASSERT_TRUE_MESSAGE(an->max.y <= w_item("ctx_move_up")->min.y + 0.5f, "Analyze... is before Move up");
    at_key(ImGuiKey_Escape);
    at_mouse_away();
    /* blok MZF: zakázané s důvodem */
    w_click("name_0", 1);
    an = w_item("ctx_analyze");
    TEST_ASSERT_TRUE(an->disabled);
    TEST_ASSERT_EQUAL_STRING("Only a raw recording can be analyzed.", an->reason.c_str());
    at_key(ImGuiKey_Escape);
    at_mouse_away();
    /* menu Tape s jedním vybraným surovým blokem */
    {
        st_CMT_VIEW tv;
        cmt_view_reset(&tv);
        fill(&tv);
        imgui_cmt_window_test_set_selection(1, tv.tape_serial);
    }
    at_frames(2);
    w_click("menu_tape", 0);
    const st_CMT_WINDOW_TEST_ITEM *mt = w_item("menu_analyze");
    TEST_ASSERT_NOT_NULL_MESSAGE(mt, "Tape menu has Analyze recording...");
    TEST_ASSERT_EQUAL_STRING("Analyze recording...", mt->text.c_str());
    TEST_ASSERT_FALSE(mt->disabled);
    at_key(ImGuiKey_Escape);
    at_mouse_away();
    /* PLAY: zakázané "Stop the tape first." */
    device_state_lock();
    cmt_play();
    device_state_unlock();
    at_frames(2);
    w_click("name_1", 1);
    an = w_item("ctx_analyze");
    TEST_ASSERT_TRUE(an->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", an->reason.c_str());
    at_key(ImGuiKey_Escape);
    at_mouse_away();
    device_state_lock();
    cmt_stop();
    device_state_unlock();
    at_frames(2);
    /* záznam a přehrávání vstupů: zakázané (zásah by fronta odmítla) */
    const int modes[2] = {REPLAY_MODE_RECORDING, REPLAY_MODE_PLAYING};
    for (int mode : modes) {
        g_replay_mode = mode;
        at_frames(2);
        w_click("name_1", 1);
        an = w_item("ctx_analyze");
        TEST_ASSERT_TRUE_MESSAGE(an && an->disabled, "Analyze... disabled during an input replay");
        TEST_ASSERT_FALSE(an->reason.empty());
        at_key(ImGuiKey_Escape);
        at_mouse_away();
        w_click("menu_tape", 0);
        TEST_ASSERT_TRUE_MESSAGE(w_item("menu_analyze")->disabled, "Tape > Analyze recording... disabled during an input replay");
        at_key(ImGuiKey_Escape);
        at_mouse_away();
        g_replay_mode = REPLAY_MODE_OFF;
    }
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    /* zakázaná položka nic nezařadí; povolená zařadí kopii pulzů */
    s_poll = false;
    at_drain();
    at_frames(2);
    w_click("name_1", 1);
    w_click("ctx_analyze", 0);
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE_MESSAGE(at_take_request(CMT_REQUEST_ANALYSIS_PULSES_NAME, &a), "Analyze... queues the pulse copy");
    TEST_ASSERT_EQUAL_INT(1, a.i[0]);
    TEST_ASSERT_TRUE(cmt_analyze_dialog_is_open());
}

/*
 * Rozvržení a texty podle SPEC a konceptu v2: titulek, věta výsledku, souhrn,
 * sloupce (Checksum, ne CRC), řádky (výsledek a součet), zaškrtnutí jen u
 * souborů, které jdou přidat, neznámý úsek bez zaškrtnutí, pořadí prvků shora,
 * sbalené Signal details, výchozí fokus Add files after recording.
 */
void test_j4_layout_and_texts(void)
{
    at_setup();
    at_open_analyzed(1);
    TEST_ASSERT_EQUAL_STRING("Analyze recording - block 2###cmt_analyze", cmt_analyze_dialog_test_title());
    TEST_ASSERT_EQUAL_STRING("3 files found, 1 with a checksum error.", d_text("ad_headline").c_str());
    TEST_ASSERT_TRUE_MESSAGE(d_text("ad_summary").find("60 000 pulses") != std::string::npos, d_text("ad_summary").c_str());
    static const char *hdr[9] = {"", "#", "Name", "Type", "Format", "Start", "Size", "Checksum", "Result"};
    for (int c = 0; c < 9; c++) TEST_ASSERT_EQUAL_STRING(hdr[c], d_text(nm("ad_hdr", c).c_str()).c_str());
    for (const st_CMT_WINDOW_TEST_ITEM &it : cmt_analyze_dialog_test_items())
        TEST_ASSERT_TRUE_MESSAGE(it.text.find("CRC") == std::string::npos, it.name.c_str());
    static const char *names[4] = {"LOADER", "FLAPPY", "HISCORE", "Unknown signal"};
    static const char *sums[4] = {"OK", "OK", "Body error", "-"};
    static const char *results[4] = {"Will be added", "Will be added", "Stays raw", "Stays raw"};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], d_text(nm("ad_name", i).c_str()).c_str());
        TEST_ASSERT_EQUAL_STRING(sums[i], d_text(nm("ad_checksum", i).c_str()).c_str());
        TEST_ASSERT_EQUAL_STRING(results[i], d_text(nm("ad_result", i).c_str()).c_str());
    }
    TEST_ASSERT_EQUAL_STRING("Sharp standard", d_text("ad_format_0").c_str());
    TEST_ASSERT_EQUAL_STRING("01 machine code", d_text("ad_type_0").c_str());
    TEST_ASSERT_EQUAL_STRING("12 288 B", d_text("ad_size_1").c_str());
    TEST_ASSERT_EQUAL_STRING("-", d_text("ad_size_3").c_str());
    /* zaškrtnutí: soubory, které jdou přidat, zaškrtnuté; chybný soubor zakázaný; neznámý úsek žádné */
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_check_0").c_str());
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_check_1").c_str());
    TEST_ASSERT_TRUE(d_item("ad_check_2")->disabled);
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_check_2").c_str());
    TEST_ASSERT_NULL_MESSAGE(d_item("ad_check_3"), "Unknown signal has no check box");
    /* pořadí prvků shora (SPEC) */
    const char *order[] = {"ad_headline", "ad_summary", "ad_table", "ad_hint", "ad_tol_label", "ad_rep_header", "ad_details", "ad_add"};
    for (size_t k = 0; k + 1 < sizeof(order) / sizeof(order[0]); k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(d_item(order[k]), order[k]);
        TEST_ASSERT_TRUE_MESSAGE(d_item(order[k])->max.y <= d_item(order[k + 1])->min.y + 0.5f, order[k]);
    }
    TEST_ASSERT_EQUAL_STRING("Leader tolerance", d_text("ad_tol_label").c_str());
    TEST_ASSERT_EQUAL_STRING("20 %", d_text("ad_tol_value").c_str());
    TEST_ASSERT_EQUAL_STRING("Repair damaged header", d_text("ad_rep_header").c_str());
    TEST_ASSERT_EQUAL_STRING("Repair damaged body", d_text("ad_rep_body").c_str());
    TEST_ASSERT_EQUAL_STRING("Repair incomplete BSD file", d_text("ad_rep_bsd").c_str());
    TEST_ASSERT_EQUAL_STRING("Signal details", d_text("ad_details").c_str());
    TEST_ASSERT_NULL_MESSAGE(d_item("ad_hist"), "Signal details are collapsed by default");
    TEST_ASSERT_NULL_MESSAGE(d_item("ad_warning"), "a saved recording has no warning");
    TEST_ASSERT_EQUAL_STRING("Add files after recording", d_text("ad_add").c_str());
    TEST_ASSERT_EQUAL_STRING("Replace recording", d_text("ad_replace").c_str());
    TEST_ASSERT_EQUAL_STRING("Cancel", d_text("ad_cancel").c_str());
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
    TEST_ASSERT_FALSE(d_item("ad_replace")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_add")->max.x <= d_item("ad_replace")->min.x);
    TEST_ASSERT_TRUE(d_item("ad_replace")->max.x <= d_item("ad_cancel")->min.x);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("add", d_text("ad_focus").c_str(), "the default button is Add files after recording");
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_table_dim").c_str());
}

/*
 * Enter = Add files after recording: zásah uplatnění s replace 0, skutečné
 * uplatnění na pásku (soubory za surovým blokem), dialog zavřený, řádek
 * zpráv "Files added from the recording: 2", vybraný první přidaný blok; data
 * zásahu se uvolní po výsledku.
 */
void test_j4_enter_adds(void)
{
    at_setup();
    at_open_analyzed(1);
    s_poll = false;
    at_drain();
    at_key(ImGuiKey_Enter);
    /* zásah čeká ve frontě (s_poll false); argumenty ověří druhý běh níže */
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_pending_applies());
    s_poll = true;
    at_frames(4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_pending_applies(), "apply data freed after the result");
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    TEST_ASSERT_EQUAL_INT(5, tv.block_count);
    TEST_ASSERT_EQUAL_STRING("Raw recording", tv.blocks[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("LOADER", tv.blocks[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING("FLAPPY", tv.blocks[3].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Files added from the recording: 2", w_item("message")->text.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_window_test_get_selection(), "the first added block is selected");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    /* argumenty zásahu: replace 0 */
    st_DEVICE_REQ_ARGS a;
    tearDown();
    at_setup();
    at_open_analyzed(1);
    s_poll = false;
    at_drain();
    at_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, &a));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.i[0], "Enter = Add (replace 0)");
}

/*
 * Replace: varování nad tlačítky jen u neuloženého bloku; Replace recording
 * zařadí replace 1 a surový blok nahradí soubory (HISCORE a neznámý úsek
 * zůstanou surové).
 */
void test_j4_replace_unsaved_warning(void)
{
    at_setup();
    append_unsaved_raw();
    at_frames(3);
    at_open_analyzed(3);
    TEST_ASSERT_EQUAL_STRING("This recording is not saved yet. Replacing it cannot be undone.", d_text("ad_warning").c_str());
    TEST_ASSERT_TRUE(d_item("ad_warning")->max.y <= d_item("ad_add")->min.y);
    s_poll = false;
    at_drain();
    d_click("ad_replace");
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, &a));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.i[0], "Replace recording = replace 1");
    s_poll = true;
    at_frames(3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_pending_applies(), "a request taken out of the queue frees its data");
    /* skutečné Replace nad uloženým blokem */
    tearDown();
    at_setup();
    at_open_analyzed(1);
    TEST_ASSERT_NULL(d_item("ad_warning"));
    d_click("ad_replace");
    at_frames(3);
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    TEST_ASSERT_EQUAL_INT(5, tv.block_count);
    TEST_ASSERT_EQUAL_STRING("LOADER", tv.blocks[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("FLAPPY", tv.blocks[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Raw recording", tv.blocks[3].name.c_str());
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_window_test_get_selection());
}

/* Odškrtnutí všech souborů zakáže obě akce s důvodem; zaškrtnutí jedné je povolí. */
void test_j4_uncheck_all_disables(void)
{
    at_setup();
    at_open_analyzed(1);
    d_click("ad_check_0");
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_check_0").c_str());
    TEST_ASSERT_EQUAL_STRING("Stays raw", d_text("ad_result_0").c_str());
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
    d_click("ad_check_1");
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_replace")->disabled);
    TEST_ASSERT_EQUAL_STRING("Check at least one file to add it.", d_item("ad_add")->reason.c_str());
    s_poll = false;
    at_drain();
    /* fokus na zaškrtávátku řádku 2 (klik): Enter ho přepne, Add nespustí */
    at_key(ImGuiKey_Enter);
    TEST_ASSERT_FALSE_MESSAGE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, NULL), "Enter on a check box does not add");
    TEST_ASSERT_TRUE(cmt_analyze_dialog_is_open());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1", d_text("ad_check_1").c_str(), "Enter toggles the focused check box");
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
    /* Enter na zaškrtávátku opravy: přepne ho (výsledky zastarají), Add nespustí */
    d_click("ad_rep_header");
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    at_key(ImGuiKey_Enter);
    TEST_ASSERT_FALSE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, NULL));
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_table_dim").c_str());
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
}

/*
 * Změna volby = zastaralé výsledky: tabulka ztlumená, akce zakázané s textem
 * SPEC; Analyze again spustí analýzu s novými volbami a akce povolí. Vrácení
 * volby na hodnotu výsledku zastaralost zruší. Reset vrátí toleranci 20 %.
 */
void test_j4_stale_options_and_analyze_again(void)
{
    at_setup();
    at_open_analyzed(1);
    d_click("ad_rep_body");
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_table_dim").c_str());
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_replace")->disabled);
    TEST_ASSERT_EQUAL_STRING("Analyze again to apply the new options.", d_item("ad_add")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING("Analyze again to apply the new options.", d_item("ad_replace")->reason.c_str());
    TEST_ASSERT_FALSE(d_item("ad_again")->disabled);
    /* zpět na volby výsledku: nic zastaralého */
    d_click("ad_rep_body");
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_table_dim").c_str());
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
    /* opravy: Analyze again s novými volbami */
    d_click("ad_rep_header");
    d_click("ad_again");
    TEST_ASSERT_EQUAL_STRING("running", d_text("ad_state").c_str());
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_again")->disabled);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_INT(1, s_syn_opts.repair_header);
    TEST_ASSERT_EQUAL_INT(0, s_syn_opts.repair_body);
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_table_dim").c_str());
    TEST_ASSERT_FALSE(d_item("ad_add")->disabled);
    /* tolerance: klik u pravého kraje slideru nastaví hodnotu podle polohy (jiná než 20 %) = zastaralé */
    {
        st_CMT_WINDOW_TEST_ITEM it = *d_item("ad_tol_slider");
        it.min.x = it.max.x - 6.0f;
        at_click_item(&it, 0);
    }
    at_mouse_away();
    TEST_ASSERT_TRUE_MESSAGE(d_text("ad_tol_value") != "20 %", d_text("ad_tol_value").c_str());
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_table_dim").c_str());
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    d_click("ad_tol_reset");
    TEST_ASSERT_EQUAL_STRING("20 %", d_text("ad_tol_value").c_str());
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_table_dim").c_str());
}

/*
 * Stav běží (Analyzing..., akce i Analyze again zakázané);
 * Cancel, Escape, zavření okna i ukončení aplikace analýzu opustí - úloha
 * zůstane živá, dokud neproběhne, pak uvolní výsledek i kopii pulzů sama.
 */
void test_j4_running_cancel_frees(void)
{
    for (int how = 0; how < 4; how++) {
        at_setup();
        at_open_dialog(1);
        at_frames(2);
        TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_queued_jobs());
        TEST_ASSERT_EQUAL_STRING("running", d_text("ad_state").c_str());
        TEST_ASSERT_EQUAL_STRING("Analyzing...", d_text("ad_headline").c_str());
        TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
        TEST_ASSERT_TRUE(d_item("ad_replace")->disabled);
        TEST_ASSERT_TRUE(d_item("ad_again")->disabled);
        TEST_ASSERT_FALSE(d_item("ad_cancel")->disabled);
        TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_live_jobs());
        if (how == 0) d_click("ad_cancel");
        else if (how == 1) at_key(ImGuiKey_Escape);
        else if (how == 2) {
            s_open = false; /* okno zavřené: kreslí se jen dialogy kazety */
            at_frames(2);
        } else {
            cmt_analyze_dialog_shutdown();
        }
        at_frames(1);
        TEST_ASSERT_FALSE_MESSAGE(cmt_analyze_dialog_is_open(), "the dialog is closed");
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_live_jobs(), "the job lives until it finishes");
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_live_pulses(), "the job keeps its pulse copy");
        TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
        at_frames(2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_jobs(), "the abandoned job freed itself");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_pulses(), "the pulse copy is freed");
        TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
        s_open = true;
    }
}

/* Změna pásky (úprava jinou cestou) při otevřeném dialogu: dialog zůstane, akce zakázané s textem SPEC. */
void test_j4_tape_changed(void)
{
    at_setup();
    at_open_analyzed(1);
    /* úprava pásky mimo dialog: smazání posledního bloku (čítač výměn pásky vzroste) */
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    const int del[1] = {2};
    (void)imgui_cmt_tape_remove_blocks(tv.tape_serial, del, 1);
    at_frames(3);
    TEST_ASSERT_TRUE(cmt_analyze_dialog_is_open());
    const char *t = "The tape changed. Close this dialog and analyze again.";
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_replace")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_again")->disabled);
    TEST_ASSERT_EQUAL_STRING(t, d_item("ad_add")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING(t, d_text("ad_warning").c_str());
    s_poll = false;
    at_drain();
    at_key(ImGuiKey_Enter);
    TEST_ASSERT_FALSE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, NULL));
    d_click("ad_cancel");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
}

/* Nic nenalezeno: věta, prázdná tabulka s textem, Signal details rozbalené s histogramem, akce zakázané; chyba analýzy. */
void test_j4_nothing_found_and_error(void)
{
    at_setup();
    s_syn = AT_SYN_NONE;
    at_open_analyzed(1);
    TEST_ASSERT_EQUAL_STRING("No files found.", d_text("ad_headline").c_str());
    TEST_ASSERT_EQUAL_STRING("No files found in this recording.", d_text("ad_body_0").c_str());
    TEST_ASSERT_NULL(d_item("ad_name_0"));
    TEST_ASSERT_NOT_NULL_MESSAGE(d_item("ad_hist"), "Signal details open by themselves when nothing was found");
    TEST_ASSERT_EQUAL_STRING("Pulse lengths, whole recording", d_text("ad_hist_caption").c_str());
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_TRUE(d_item("ad_replace")->disabled);
    /* chyba: analyzátor bez výsledku (selhání alokace) -> text chyby místo tabulky, akce zakázané */
    tearDown();
    at_setup();
    s_syn = AT_SYN_FAIL;
    at_open_dialog(1);
    at_frames(2);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(2);
    TEST_ASSERT_EQUAL_STRING("error", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_STRING("The recording could not be analyzed.", d_text("ad_headline").c_str());
    TEST_ASSERT_EQUAL_STRING("The analysis failed: not enough memory.", d_text("ad_body_0").c_str());
    TEST_ASSERT_TRUE(d_item("ad_add")->disabled);
    TEST_ASSERT_EQUAL_STRING("The recording could not be analyzed.", d_item("ad_add")->reason.c_str());
    TEST_ASSERT_FALSE_MESSAGE(d_item("ad_again")->disabled, "Analyze again stays available after an error");
    /* chyba kopie pulzů (text jádra): dialog otevřený přímo nad blokem MZF (okno to nedovolí, jádro odmítne) */
    tearDown();
    at_setup();
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    cmt_analyze_dialog_open(&tv, 0);
    at_frames(3);
    TEST_ASSERT_EQUAL_STRING("error", d_text("ad_state").c_str());
    /* text jádra s číslem bloku od 1 jako okno (jádro píše "Block 0") */
    TEST_ASSERT_EQUAL_STRING("Block 1 is not a raw signal block (CSW recording)", d_text("ad_body_0").c_str());
    TEST_ASSERT_TRUE(d_item("ad_again")->disabled);
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
}

/*
 * Vybraný řádek: nápověda pod tabulkou se jménem a důvodem jádra, tooltip
 * důvodu u výsledku, mezerník přepne zaškrtnutí vybraného řádku, histogram
 * vybraného souboru se značkami krátkého a dlouhého pulzu.
 */
void test_j4_row_hint_space_histogram(void)
{
    at_setup();
    at_open_analyzed(1);
    d_click("ad_name_2");
    /* soubor, který zůstane surový: důvod i co zkusit bez najetí myší */
    TEST_ASSERT_EQUAL_STRING("HISCORE: Checksum error in the body. Cannot be added. You can try \"Repair damaged body\" and Analyze again.",
                             d_text("ad_hint").c_str());
    TEST_ASSERT_EQUAL_STRING("Checksum error in the body", d_item("ad_result_2")->reason.c_str());
    /* delší nápověda změnila výšku dialogu: znovu vystředěný se ustálí, klik pak trefí řádek */
    at_frames(3);
    d_click("ad_name_0");
    at_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0", d_text("ad_check_0").c_str(), "Space toggles the check box of the selected row");
    at_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_check_0").c_str());
    /* histogram vybraného souboru */
    cmt_analyze_dialog_test_set_details(true);
    at_frames(2);
    TEST_ASSERT_EQUAL_STRING("Pulse lengths, LOADER", d_text("ad_hist_caption").c_str());
    TEST_ASSERT_EQUAL_STRING("250 µs", d_text("ad_hist_short").c_str());
    TEST_ASSERT_EQUAL_STRING("500 µs", d_text("ad_hist_long").c_str());
    cmt_analyze_dialog_test_select_row(-1);
    at_frames(2);
    TEST_ASSERT_EQUAL_STRING("Pulse lengths, whole recording", d_text("ad_hist_caption").c_str());
    TEST_ASSERT_NULL(d_item("ad_hist_short"));
}

/*
 * Data zásahu uplatnění drží modul i po zavření dialogu: dokud zásah čeká ve
 * frontě, nic se neuvolní a provedení čte platná data; ukončení aplikace
 * (pořadí main.c: cmt_request_exit() na emu vlákně, připojení emu vlákna,
 * pak shutdown UI) data uvolní - zásah, který by přesto proběhl, skončí
 * chybou a pásku nezmění; zásah vytažený z fronty bez
 * provedení (ztracený výsledek) data uvolní až hlídač po prázdné frontě bez
 * výsledku.
 */
void test_j4_apply_pending_after_close(void)
{
    at_setup();
    at_open_analyzed(1);
    s_poll = false;
    at_drain();
    d_click("ad_add");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    at_frames(5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_pending_applies(), "queued apply keeps its data");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_live_pulses());
    /* zavřený dialog zásah nezruší: provedení čte platná data */
    s_poll = true;
    at_frames(3);
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_pending_applies());
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    TEST_ASSERT_EQUAL_INT(5, tv.block_count);
    /* ukončení aplikace se zásahem ve frontě v pořadí aplikace: emu vlákno v cmt_exit()
     * vymaže registr (cmt_request_exit()), main.c ho připojí, teprve pak UI zavolá shutdown - ten data
     * uvolní bezpodmínečně; zásah, který by přesto proběhl, data nenajde a pásku nezmění */
    at_setup();
    at_open_analyzed(1);
    cmt_view_reset(&tv);
    fill(&tv);
    const int before = tv.block_count;
    s_poll = false;
    at_drain();
    d_click("ad_add");
    at_frames(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_pending_applies(), "queued apply keeps its data");
    cmt_request_exit();
    cmt_analyze_dialog_shutdown();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_pending_applies(), "shutdown frees the data of a queued request");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    s_poll = true;
    at_frames(3);
    cmt_view_reset(&tv);
    fill(&tv);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, tv.block_count, "a cancelled request does not change the tape");
    /* ztracený zásah */
    at_setup();
    at_open_analyzed(1);
    s_poll = false;
    at_drain();
    d_click("ad_add");
    TEST_ASSERT_TRUE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, NULL));
    at_frames(3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_pending_applies(), "a lost request frees its data");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
}

/* Skutečné vlákno GLib: analýza (skutečný analyzátor) doběhne a dialog ji převezme; čeká se na příznak hotovo s mezí. */
void test_j4_real_thread(void)
{
    at_setup();
    cmt_analyze_dialog_test_set_manual_runner(false);
    s_syn = AT_SYN_REAL;
    at_open_dialog(1);
    /* čekání na příznak hotovo (stav dialogu) s krátkým spánkem a mezí 30 s */
    gint64 t0 = g_get_monotonic_time();
    while (d_text("ad_state") != "done" && d_text("ad_state") != "error" && g_get_monotonic_time() - t0 < 30 * G_USEC_PER_SEC) {
        at_frame();
        g_usleep(2000);
    }
    TEST_ASSERT_TRUE_MESSAGE(d_text("ad_state") == "done" || d_text("ad_state") == "error", "the worker thread finished");
    at_frames(5);
    TEST_ASSERT_EQUAL_INT(1, s_syn_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_threads(), "the finished thread is joined");
    d_click("ad_cancel");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_jobs());
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
}

/*
 * Řízené "vlákno": druhá analýza, zatímco dřívější opuštěná ještě běží, čeká (stav waiting)
 * a spustí se až po doběhnutí první; výsledek první se do nového dialogu nedostane.
 */
void test_j4_second_analysis_waits(void)
{
    at_setup();
    at_open_dialog(1);
    at_frames(2);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_queued_jobs());
    d_click("ad_cancel");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    /* dialog znovu: první úloha "běží" dál (fronta testu), nová čeká */
    s_syn = AT_SYN_NONE;
    at_open_dialog(1);
    at_frames(3);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("waiting", d_text("ad_state").c_str(), "the second analysis waits for the abandoned one");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_queued_jobs(), "only the first job is running");
    TEST_ASSERT_EQUAL_STRING("Analyzing...", d_text("ad_headline").c_str());
    TEST_ASSERT_EQUAL_INT(2, cmt_analyze_dialog_test_live_jobs());
    /* první doběhne (výsledek FOUND se zahodí), pak se spustí druhá */
    s_syn = AT_SYN_FOUND;
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_live_jobs(), "the abandoned job freed itself");
    at_frames(2);
    TEST_ASSERT_EQUAL_STRING("running", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_queued_jobs());
    s_syn = AT_SYN_NONE;
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("No files found.", d_text("ad_headline").c_str(), "the dialog shows its own analysis, not the abandoned one");
}

/*
 * Kopie pulzů se starým seq (dialog mezitím opuštěný a otevřený nad jiným blokem) se vyzvedne,
 * uvolní a nikdy neukáže; dialog vezme jen kopii svého bloku.
 */
void test_j4_stale_pulse_copy_dropped(void)
{
    at_setup();
    s_syn = AT_SYN_NONE;
    append_unsaved_raw(30000);
    at_frames(3);
    s_poll = false;
    at_drain();
    /* kopie bloku 2 (60 000 pulzů): zásah zařazený, dialog opuštěný */
    at_open_dialog(1);
    d_click("ad_cancel");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    /* emu vlákno zásah provede; výsledek čeká ve frontě výsledků */
    device_request_poll();
    /* dialog nad blokem 4 (30 000 pulzů) přímo (ve snímku by se výsledek vyřídil se zavřeným dialogem) */
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    cmt_analyze_dialog_open(&tv, 3);
    /* tento snímek: dialog čeká na kopii bloku 4, přijde výsledek staré kopie */
    at_frame();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("copying", d_text("ad_state").c_str(), "the old pulse copy is not taken by the new dialog");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_pulses(), "the old copy is freed");
    s_poll = true;
    at_frames(3);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_TRUE_MESSAGE(d_text("ad_summary").find("30 000 pulses") != std::string::npos, d_text("ad_summary").c_str());
    TEST_ASSERT_EQUAL_STRING("Analyze recording - block 4###cmt_analyze", cmt_analyze_dialog_test_title());
    d_click("ad_cancel");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
}

/* Soubor bez těla (jen hlavička, součet těla není k dispozici) ukáže v Checksum "-", ne OK. */
void test_j4_checksum_missing_body(void)
{
    at_setup();
    s_syn = AT_SYN_HEADER_ONLY;
    at_open_analyzed(1);
    TEST_ASSERT_EQUAL_STRING("LOADER", d_text("ad_name_0").c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("-", d_text("ad_checksum_0").c_str(), "header OK and body not available is not OK");
    TEST_ASSERT_EQUAL_STRING("Stays raw", d_text("ad_result_0").c_str());
}

/* Úspěch uplatnění bez výsledku úpravy (přepsaný kruh) přesto ohlásí zprávu. */
void test_j4_success_without_edit_result(void)
{
    at_setup();
    st_DEVICE_REQ_RESULT res;
    memset(&res, 0, sizeof(res));
    res.seq = 0xFFFFFFF0u;
    res.name = CMT_REQUEST_ANALYSIS_APPLY_NAME;
    res.status = DEVICE_REQ_DONE;
    res.rc = 0;
    TEST_ASSERT_TRUE(cmt_analyze_dialog_request_result(&res, NULL));
    at_frames(3);
    TEST_ASSERT_EQUAL_STRING("Files added from the recording.", w_item("message")->text.c_str());
}

/* Zavřené okno dialog opustí i s popupem; otevření čeká, je-li otevřený jiný popup. */
void test_j4_popup_closed_outside(void)
{
    at_setup();
    at_open_dialog(1);
    at_frames(2);
    TEST_ASSERT_TRUE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    s_open = false;
    at_frames(2);
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    TEST_ASSERT_FALSE_MESSAGE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
                              "no popup stays open after leaving the dialog outside the modal");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    s_open = true;
    at_frames(2);
    /* otevření při otevřeném kontextovém menu: dialog počká, menu nezavře */
    w_click("name_0", 1);
    TEST_ASSERT_NOT_NULL(w_item("ctx_play"));
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    s_poll = false;
    cmt_analyze_dialog_open(&tv, 1);
    at_frame();
    TEST_ASSERT_NOT_NULL_MESSAGE(w_item("ctx_play"), "the context menu stays open");
    TEST_ASSERT_FALSE_MESSAGE(d_drawn(), "the dialog waits for the menu");
    at_key(ImGuiKey_Escape);
    at_mouse_away();
    at_frames(2);
    TEST_ASSERT_TRUE(d_drawn());
    at_drain();
}

/*
 * Ukončení aplikace s běžícím vláknem analýzy - doběhne-li do meze, připojí se (data uvolněná po
 * návratu); jinak se odpojí a uvolní data samo, až doběhne.
 */
void test_j4_exit_joins_worker(void)
{
    at_setup();
    cmt_analyze_dialog_test_set_manual_runner(false);
    s_syn = AT_SYN_SLOW;
    at_open_dialog(1);
    at_frames(2);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_threads());
    cmt_analyze_dialog_test_set_exit_join_ms(5000);
    cmt_analyze_dialog_shutdown();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_threads(), "the worker is joined within the bound");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_jobs(), "joined: the job is freed");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    /* mez kratší než analýza: vlákno se odpojí, data uvolní samo */
    tearDown();
    at_setup();
    cmt_analyze_dialog_test_set_manual_runner(false);
    s_syn = AT_SYN_SLOW;
    at_open_dialog(1);
    at_frames(2);
    cmt_analyze_dialog_test_set_exit_join_ms(10);
    gint64 t0 = g_get_monotonic_time();
    cmt_analyze_dialog_shutdown();
    TEST_ASSERT_TRUE_MESSAGE(g_get_monotonic_time() - t0 < 250000, "shutdown does not wait longer than the bound");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_threads(), "the worker is detached");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_live_jobs());
    t0 = g_get_monotonic_time();
    while (cmt_analyze_dialog_test_live_jobs() != 0 && g_get_monotonic_time() - t0 < 10 * G_USEC_PER_SEC) g_usleep(5000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_jobs(), "the detached worker freed its data");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
}

/*
 * Snímek, ve kterém okno kazety nekreslí obsah (sbalené kódem), dialog analýzy s výsledkem
 * nezavře: výsledek, odškrtnutý řádek i výběr zůstanou, okno se samo rozbalí a dialog se kreslí dál; Add pak
 * zařadí uplatnění se stavem z doby před snímkem bez obsahu.
 */
void test_finalb_dialog_kept_without_window_content(void)
{
    at_setup();
    at_open_analyzed(1);
    d_click("ad_check_0");
    TEST_ASSERT_EQUAL_STRING("0", d_text("ad_check_0").c_str());
    ImGui::SetWindowCollapsed(AT_W_ID, true);
    at_frame();
    TEST_ASSERT_TRUE_MESSAGE(ImGui::FindWindowByName(AT_W_ID)->Collapsed, "the Virtual CMT window drew no content in this frame");
    TEST_ASSERT_TRUE_MESSAGE(cmt_analyze_dialog_is_open(), "a frame without window content does not cancel the dialog");
    at_frames(3);
    TEST_ASSERT_FALSE_MESSAGE(ImGui::FindWindowByName(AT_W_ID)->Collapsed, "the window expands again while its modal is open");
    TEST_ASSERT_TRUE(d_drawn());
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0", d_text("ad_check_0").c_str(), "the unchecked row stays unchecked");
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_check_1").c_str());
    s_poll = false;
    at_drain();
    d_click("ad_add");
    TEST_ASSERT_TRUE_MESSAGE(at_take_request(CMT_REQUEST_ANALYSIS_APPLY_NAME, NULL), "Add queues the apply after the frame without content");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
}

/*
 * Nejdelší pulz celé kopie se počítá jednou za kopii (pracovní vlákno), ne při každé změně
 * výběru; histogram vybraného řádku i celého záznamu bez dalšího průchodu.
 */
void test_j4_histogram_max_once(void)
{
    at_setup();
    const int before = cmt_analyze_dialog_test_max_passes();
    at_open_analyzed(1);
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_max_passes() - before);
    cmt_analyze_dialog_test_set_details(true);
    for (int r = -1; r < 4; r++) {
        cmt_analyze_dialog_test_select_row(r);
        at_frames(2);
    }
    TEST_ASSERT_EQUAL_STRING("Pulse lengths, Unknown signal", d_text("ad_hist_caption").c_str());
    /* Analyze again nad touž kopií: maximum se nepočítá znovu */
    d_click("ad_again");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_max_passes() - before, "one pass per pulse copy");
}

/**
 * @brief Syntetická kopie pulzů bloku 1 pásky testu (velký blok).
 *
 * Krátké a dlouhé pulzy jako raw_lengths(), každý miliontý pulz dlouhé ticho.
 *
 * @param n Počet pulzů. @param tape_serial Čítač výměn pásky kopie.
 * @return Kopie (vlastnictví převezme dialog, cmt_analyze_dialog_test_open_with_pulses()).
 */
static st_CMTTMZ_PULSES *at_big_pulses(uint32_t n, unsigned tape_serial)
{
    st_CMTTMZ_PULSES *p = (st_CMTTMZ_PULSES *)calloc(1, sizeof(st_CMTTMZ_PULSES));
    TEST_ASSERT_NOT_NULL(p);
    p->lengths = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(p->lengths);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = (i % 4u == 0u) ? 4430u + (i * 13u) % 300u - 150u : 2215u + (i * 7u) % 200u - 100u;
        if (i % 1000000u == 999999u) l = 8860800u;
        p->lengths[i] = l;
        sum += l;
    }
    p->sample_rate = GDGCLK_BASE / 2;
    p->count = n;
    p->first_level = 0;
    p->total_us = (uint64_t)((double)sum * 1e6 / (double)p->sample_rate + 0.5);
    p->block_id = 1;
    p->tape_serial = tape_serial;
    return p;
}

/*
 * Obří blok v běžném běhu testů: blok 500 000 pulzů, skutečný analyzátor ve
 * skutečném vlákně; vlákno po analýze drží brána, takže analýza "běží", dokud ji test nepustí - dialog
 * mezitím kreslí snímky (stav running, Cancel povolené), Cancel dialog zavře a úloha žije dál; po otevření
 * brány doběhne a uvolní se sama. Bez měření a bez tvrzení o čase (mez čekání jen proti uváznutí).
 */
void test_finalb_big_block_draws_and_cancels(void)
{
    at_setup();
    cmt_analyze_dialog_test_set_manual_runner(false);
    s_syn = AT_SYN_GATED;
    g_atomic_int_set(&s_syn_gate, 0);
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    cmt_analyze_dialog_test_open_with_pulses(&tv, 1, at_big_pulses(500000u, tv.tape_serial));
    for (int i = 0; i < 10; i++) {
        at_frame();
        TEST_ASSERT_TRUE_MESSAGE(d_drawn(), "the dialog draws a frame while the analysis runs");
        TEST_ASSERT_EQUAL_STRING("running", d_text("ad_state").c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(d_text("ad_summary").find("500 000 pulses") != std::string::npos, d_text("ad_summary").c_str());
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_threads());
    TEST_ASSERT_FALSE(d_item("ad_cancel")->disabled);
    d_click("ad_cancel");
    TEST_ASSERT_FALSE_MESSAGE(cmt_analyze_dialog_is_open(), "Cancel leaves the running analysis");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_live_jobs(), "the job lives until the worker finishes");
    g_atomic_int_set(&s_syn_gate, 1);
    const gint64 t0 = g_get_monotonic_time();
    while ((cmt_analyze_dialog_test_live_jobs() != 0 || cmt_analyze_dialog_test_threads() != 0)
           && g_get_monotonic_time() - t0 < 120 * G_USEC_PER_SEC) {
        at_frame();
        g_usleep(2000);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_live_jobs(), "the abandoned job freed itself");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_live_pulses());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmt_analyze_dialog_test_threads(), "the finished worker is joined");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
}

/*
 * Měření obřího bloku: jen s MZ_CMT_BENCH=1 (doba běhu). Syntetická kopie
 * 10 000 000 pulzů: analýza ve skutečném vlákně, mezitím se kreslí snímky (počet a nejdelší snímek), doba
 * histogramu celého záznamu a vybraného řádku na UI vlákně. Hodnoty na stderr ("bench:"). Běžný běh testů
 * pokrývá menší blok v test_finalb_big_block_draws_and_cancels().
 */
void test_j4_giant_block_bench(void)
{
    const char *ev = getenv("MZ_CMT_BENCH");
    if (!ev || atoi(ev) != 1) TEST_IGNORE_MESSAGE("set MZ_CMT_BENCH=1 to measure a block of 10 000 000 pulses");
    at_setup();
    cmt_analyze_dialog_test_set_manual_runner(false);
    s_syn = AT_SYN_REAL;
    const uint32_t n = 10000000u;
    st_CMT_VIEW tv;
    cmt_view_reset(&tv);
    fill(&tv);
    st_CMTTMZ_PULSES *p = at_big_pulses(n, tv.tape_serial);
    gint64 t0 = g_get_monotonic_time();
    cmt_analyze_dialog_test_open_with_pulses(&tv, 1, p);
    int frames = 0;
    gint64 worst = 0;
    while (d_text("ad_state") != "done" && d_text("ad_state") != "error" && g_get_monotonic_time() - t0 < 600 * G_USEC_PER_SEC) {
        gint64 f0 = g_get_monotonic_time();
        at_frame();
        gint64 f1 = g_get_monotonic_time();
        if (f1 - f0 > worst) worst = f1 - f0;
        frames++;
        g_usleep(5000);
    }
    gint64 t1 = g_get_monotonic_time();
    fprintf(stderr, "bench: analysis of %u pulses %.2f s (worker thread), state '%s', %d frames drawn meanwhile, longest frame %.1f ms\n",
            n, (double)(t1 - t0) / 1e6, d_text("ad_state").c_str(), frames, (double)worst / 1000.0);
    TEST_ASSERT_TRUE_MESSAGE(frames > 1, "the window keeps drawing during the analysis");
    cmt_analyze_dialog_test_set_details(true);
    gint64 h0 = g_get_monotonic_time();
    at_frame();
    gint64 h1 = g_get_monotonic_time();
    cmt_analyze_dialog_test_select_row(0);
    gint64 h2 = g_get_monotonic_time();
    at_frame();
    gint64 h3 = g_get_monotonic_time();
    fprintf(stderr, "bench: frame with the whole-recording histogram %.1f ms, frame with a selected row histogram %.1f ms\n",
            (double)(h1 - h0) / 1000.0, (double)(h3 - h2) / 1000.0);
    d_click("ad_cancel");
}

/**
 * @brief Kontrola geometrie dialogu: prvky uvnitř modálu a ořezu, bez překryvu, text ne širší než ořez.
 * @param ctx Popis. @return Počet vad (vypsané na stderr).
 */
static int at_dialog_faults(const char *ctx)
{
    const float tol = 0.5f;
    int faults = 0;
    ImVec2 wmin, wmax, imin, imax;
    if (!cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax)) {
        fprintf(stderr, "%s: dialog not drawn\n", ctx);
        return 1;
    }
    std::vector<st_CMT_WINDOW_TEST_ITEM> items = cmt_analyze_dialog_test_items();
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
        /* prvek posouvaného těla smí být svisle mimo okno (odrolovaný), vodorovně ne */
        bool in_win = it.min.x >= imin.x - tol && it.max.x <= imax.x + tol
                      && (it.scroll_y || (it.min.y >= imin.y - tol && it.max.y <= imax.y + tol));
        if (!in_clip || !in_win) {
            fprintf(stderr, "%s: item '%s' (%.1f,%.1f)-(%.1f,%.1f) outside %s\n", ctx, it.name.c_str(), it.min.x, it.min.y, it.max.x,
                    it.max.y, in_clip ? "dialog" : "clip rect");
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

/**
 * @brief Dialog se vejde do 90 % pracovní plochy, leží v pracovní ploše viewportu, okno samo se neposouvá
 *        (posouvá se jen tělo) a tlačítka patičky jsou vidět celá.
 * @param work Pracovní plocha (náhradní, velikost). @param ctx Popis. @return Počet vad.
 */
static int at_dialog_size_faults(ImVec2 work, const char *ctx)
{
    int faults = 0;
    ImVec2 wmin, wmax, imin, imax;
    if (!cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax)) return 1;
    if (wmax.x - wmin.x > 0.9f * work.x + 1.0f || wmax.y - wmin.y > 0.9f * work.y + 1.0f) {
        fprintf(stderr, "%s: dialog %.0f x %.0f px exceeds 90 %% of %.0f x %.0f\n", ctx, wmax.x - wmin.x, wmax.y - wmin.y, work.x, work.y);
        faults++;
    }
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    if (wmin.x < vp->WorkPos.x - 0.5f || wmin.y < vp->WorkPos.y - 0.5f || wmax.x > vp->WorkPos.x + vp->WorkSize.x + 0.5f
        || wmax.y > vp->WorkPos.y + vp->WorkSize.y + 0.5f) {
        fprintf(stderr, "%s: dialog (%.0f,%.0f)-(%.0f,%.0f) outside the work area of the viewport\n", ctx, wmin.x, wmin.y, wmax.x, wmax.y);
        faults++;
    }
    ImGuiWindow *dw = ImGui::FindWindowByName("###cmt_analyze");
    if (dw && dw->ScrollbarY) {
        fprintf(stderr, "%s: the dialog window scrolls (only its body may)\n", ctx);
        faults++;
    }
    const char *btn[3] = {"ad_add", "ad_replace", "ad_cancel"};
    for (const char *b : btn) {
        const st_CMT_WINDOW_TEST_ITEM *it = d_item(b);
        if (!it || it->scroll_y || it->min.y < imin.y - 0.5f || it->max.y > imax.y + 0.5f || it->max.x > imax.x + 0.5f) {
            fprintf(stderr, "%s: button %s not fully visible in the footer\n", ctx, b);
            faults++;
        }
    }
    return faults;
}

/*
 * Geometrie dialogu (nalezeno s vybraným chybným souborem
 * a rozbaleným histogramem, nic nenalezeno, běží, dlouhá tabulka) při 100 %
 * a 175 %, texty běžné i o 40 % delší; pracovní plocha 1920 x 1080 při 175 %
 * a 1366 x 728 při 100 %: dialog nejvýš 90 % pracovní plochy, nic oříznuté.
 */
void test_j4_geometry(void)
{
    const float scales[2] = {1.0f, 1.75f};
    int faults = 0;
    char ctx[128];
    for (float sc : scales) {
        for (int stretch = 0; stretch < 2; stretch++) {
            for (int st = 0; st < 4; st++) {
                ImVec2 work = (sc > 1.5f) ? ImVec2(1920.0f, 1040.0f) : ImVec2(1366.0f, 728.0f);
                at_setup(sc, stretch != 0);
                imgui_cmt_window_test_set_work_size(work);
                s_syn = (st == 1) ? AT_SYN_NONE : ((st == 3) ? AT_SYN_MANY : AT_SYN_FOUND);
                if (st == 2) {
                    at_open_dialog(1);
                    at_frames(3);
                } else {
                    at_open_analyzed(1);
                }
                if (st == 0) {
                    cmt_analyze_dialog_test_select_row(2);
                    cmt_analyze_dialog_test_set_details(true);
                }
                at_frames(4);
                snprintf(ctx, sizeof(ctx), "j4 geometry %.2f%s state %d", sc, stretch ? " +40" : "", st);
                ImGuiWindow *dw = ImGui::FindWindowByName("###cmt_analyze");
                fprintf(stderr, "%s: dialog %.0f x %.0f, content %.0f x %.0f, scrollbar %d\n", ctx, dw ? dw->Size.x : 0.0f,
                        dw ? dw->Size.y : 0.0f, dw ? dw->ContentSize.x : 0.0f, dw ? dw->ContentSize.y : 0.0f, dw ? (int)dw->ScrollbarY : -1);
                /* histogram ROZBALENÝ (stavy 0 a 1): tělo se posouvá, patička vidět */
                faults += at_dialog_faults(ctx) + at_dialog_size_faults(work, ctx);
                if (st == 1) {
                    /* nic nenalezeno: tabulka jen hlavička a dvě věty */
                    const st_CMT_WINDOW_TEST_ITEM *tb = d_item("ad_table");
                    const st_CMT_WINDOW_TEST_ITEM *hd = d_item("ad_hdr_1");
                    const float lh = hd ? hd->max.y - hd->min.y : 0.0f;
                    int lines = 0;
                    while (d_item(nm("ad_body", lines).c_str())) lines++;
                    /* hlavička (řádek + vnitřní okraj), řádky vět, okraje vět: 1,2 + řádky + 0,6 výšky řádku */
                    if (!tb || lines < 2 || tb->max.y - tb->min.y > ( 1.8f + ( float ) lines ) * lh + 2.0f) {
                        fprintf(stderr, "%s: empty table area too high (%.0f px)\n", ctx, tb ? tb->max.y - tb->min.y : -1.0f);
                        faults++;
                    }
                }
                if (st == 0) {
                    /* sbalení a znovu rozbalení: dialog znovu v ploše */
                    cmt_analyze_dialog_test_set_details(false);
                    at_frames(4);
                    faults += at_dialog_faults(ctx) + at_dialog_size_faults(work, ctx);
                }
                cmt_analyze_dialog_test_reset();
                at_frames(1);
                tearDown();
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(0, faults);
}

/**
 * @brief Kontrola posuvníků a hran posouvaného těla.
 *
 * V dialogu nejvýš jeden svislý posuvník (okno, tělo, tabulka); žádný text
 * posouvaného těla není rozříznutý jeho hranou (je celý uvnitř, nebo celý
 * mimo); je-li @p hist, je histogram s popiskem, osou a jednotkou celý
 * uvnitř ořezu těla.
 *
 * @param ctx Popis. @param hist Rozbalený histogram musí být celý vidět. @return Počet vad.
 */
static int at_dialog_scroll_faults(const char *ctx, bool hist)
{
    const float tol = 0.5f;
    int faults = 0;
    ImVec2 bmin, bmax;
    bool body_sb = false, table_sb = false;
    if (!cmt_analyze_dialog_test_body_rect(&bmin, &bmax, &body_sb)) return 1;
    (void)cmt_analyze_dialog_test_table(&table_sb);
    ImGuiWindow *dw = ImGui::FindWindowByName("###cmt_analyze");
    const int bars = (body_sb ? 1 : 0) + (table_sb ? 1 : 0) + ((dw && dw->ScrollbarY) ? 1 : 0);
    if (bars > 1) {
        fprintf(stderr, "%s: %d vertical scrollbars (body %d, table %d)\n", ctx, bars, (int)body_sb, (int)table_sb);
        faults++;
    }
    for (const st_CMT_WINDOW_TEST_ITEM &it : cmt_analyze_dialog_test_items()) {
        if (it.container || it.text.empty() || !it.scroll_y || it.max.y - it.min.y < tol) continue;
        const bool cut_top = it.min.y < it.clip_min.y - tol && it.max.y > it.clip_min.y + tol;
        const bool cut_bottom = it.min.y < it.clip_max.y - tol && it.max.y > it.clip_max.y + tol;
        if (cut_top || cut_bottom) {
            fprintf(stderr, "%s: text '%s' (%.1f-%.1f) cut by the edge of the scrolled area (%.1f-%.1f)\n", ctx, it.name.c_str(), it.min.y,
                    it.max.y, it.clip_min.y, it.clip_max.y);
            faults++;
        }
    }
    if (hist) {
        int seen = 0;
        for (const st_CMT_WINDOW_TEST_ITEM &it : cmt_analyze_dialog_test_items()) {
            if (it.name.rfind("ad_hist", 0) != 0) continue;
            seen++;
            if (it.min.y < bmin.y - tol || it.max.y > bmax.y + tol) {
                fprintf(stderr, "%s: histogram item '%s' (%.1f-%.1f) not fully visible in the body (%.1f-%.1f)\n", ctx, it.name.c_str(),
                        it.min.y, it.max.y, bmin.y, bmax.y);
                faults++;
            }
        }
        if (seen < 3) {
            fprintf(stderr, "%s: histogram not drawn\n", ctx);
            faults++;
        }
    }
    return faults;
}

/*
 * Při 175 % na ploše 1920 x 1040 (texty běžné i o 40 % delší) s rozbaleným Signal details
 * (nalezeno s vybraným souborem, nic nenalezeno) je v dialogu nejvýš jeden svislý posuvník, histogram s osou
 * a popisky je celý vidět bez ručního posunu a žádný text není rozříznutý hranou posouvané oblasti; totéž po
 * sbalení (výchozí stav nalezeno) a při 100 % na 1366 x 728.
 */
void test_e4b_one_scrollbar_histogram_visible(void)
{
    const float scales[2] = {1.0f, 1.75f};
    int faults = 0;
    char ctx[128];
    for (float sc : scales) {
        for (int stretch = 0; stretch < 2; stretch++) {
            for (int st = 0; st < 2; st++) {
                ImVec2 work = (sc > 1.5f) ? ImVec2(1920.0f, 1040.0f) : ImVec2(1366.0f, 728.0f);
                at_setup(sc, stretch != 0);
                imgui_cmt_window_test_set_work_size(work);
                s_syn = (st == 1) ? AT_SYN_NONE : AT_SYN_FOUND;
                at_open_analyzed(1);
                if (st == 0) {
                    at_frames(3);
                    snprintf(ctx, sizeof(ctx), "%.2f%s found", sc, stretch ? " +40" : "");
                    faults += at_dialog_scroll_faults(ctx, false);
                    cmt_analyze_dialog_test_select_row(2);
                    /* rozbalení (jako klik na hlavičku: rozbalení se pozná proti minulému snímku) */
                    at_frames(3);
                    cmt_analyze_dialog_test_set_details(true);
                }
                at_frames(6);
                snprintf(ctx, sizeof(ctx), "%.2f%s %s details", sc, stretch ? " +40" : "", st ? "empty" : "found");
                faults += at_dialog_scroll_faults(ctx, true);
                faults += at_dialog_faults(ctx) + at_dialog_size_faults(work, ctx);
                cmt_analyze_dialog_test_reset();
                at_frames(1);
                tearDown();
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(0, faults);
}

/*
 * Vybraný řádek má ztlumený text text-dim-selected a chybový error-selected (na row-selected
 * aspoň 4,5:1); při 175 % na 1920 px se nejdřív zúží Type a Format, "Unknown signal" zůstane celé.
 */
void test_e4b_selected_row_colors_and_name_width(void)
{
    at_setup();
    at_open_analyzed(1);
    cmt_analyze_dialog_test_select_row(2);
    at_frames(2);
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", (unsigned)CMT_TOK_ERROR_SELECTED);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hex, d_text("ad_sel_checksum_color").c_str(), "Body error on the selected row");
    snprintf(hex, sizeof(hex), "%08X", (unsigned)CMT_TOK_TEXT_DIM_SELECTED);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hex, d_text("ad_sel_result_color").c_str(), "Stays raw on the selected row");
    tearDown();
    /* jen texty v běžné délce: s texty o 40 % delšími se po Type, Format, Checksum a Result zúží nakonec i Name */
    at_setup(1.75f, false);
    imgui_cmt_window_test_set_work_size(ImVec2(1920.0f, 1040.0f));
    at_open_analyzed(1);
    at_frames(3);
    const st_CMT_WINDOW_TEST_ITEM *nm3 = d_item("ad_name_3");
    TEST_ASSERT_NOT_NULL(nm3);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", nm3->reason.c_str(), "Unknown signal is not shortened");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, (int)d_item("ad_format_0")->reason.size(), "Format is shortened first");
}

/*
 * V zastaralých výsledcích je výchozí (primární) Analyze again a Enter (fokus na řádku) ho spustí;
 * po doběhnutí je výchozí zase Add.
 */
void test_e4b_stale_analyze_again_default(void)
{
    at_setup();
    at_open_analyzed(1);
    TEST_ASSERT_EQUAL_STRING("ad_add", d_text("ad_primary").c_str());
    d_click("ad_rep_body");
    TEST_ASSERT_EQUAL_STRING("1", d_text("ad_table_dim").c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ad_again", d_text("ad_primary").c_str(), "Analyze again is the default in the stale state");
    d_click("ad_name_0");
    TEST_ASSERT_EQUAL_INT(0, cmt_analyze_dialog_test_queued_jobs());
    at_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_analyze_dialog_test_queued_jobs(), "Enter runs Analyze again");
    TEST_ASSERT_TRUE(cmt_analyze_dialog_is_open());
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_INT(1, s_syn_opts.repair_body);
    TEST_ASSERT_EQUAL_STRING("ad_add", d_text("ad_primary").c_str());
}

/*
 * Přetažení dialogu (i zčásti mimo pracovní plochu) se nevrátí doprostřed; změna velikosti
 * (rozbalení Signal details) dialog znovu vystředí do plochy.
 */
void test_e4b_drag_not_recentered(void)
{
    at_setup();
    at_open_analyzed(1);
    at_frames(4);
    ImVec2 wmin, wmax, imin, imax;
    TEST_ASSERT_TRUE(cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    /* "přetaženo" tak, že pravá třetina leží za pravým okrajem plochy */
    const ImVec2 dragged(vp->WorkPos.x + vp->WorkSize.x - (wmax.x - wmin.x) * 0.66f, wmin.y + 40.0f);
    ImGui::SetWindowPos("###cmt_analyze", dragged, ImGuiCond_Always);
    at_frames(5);
    TEST_ASSERT_TRUE(cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, dragged.x, wmin.x, "a dragged dialog stays where it was dragged");
    TEST_ASSERT_FLOAT_WITHIN(0.5f, dragged.y, wmin.y);
    cmt_analyze_dialog_test_set_details(true);
    at_frames(6);
    TEST_ASSERT_TRUE(cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax));
    TEST_ASSERT_TRUE_MESSAGE(wmax.x <= vp->WorkPos.x + vp->WorkSize.x + 0.5f, "a size change centres the dialog again");
}

/** @brief Počet otevřených popupů hned po opuštění dialogu uvnitř snímku (-1 = neměřeno). */
static int s_popups_after_abandon = -1;

/** @brief Uvnitř snímku: opustí dialog a změří zásobník popupů ImGui (s_in_frame). */
static void at_abandon_in_frame(void)
{
    cmt_analyze_dialog_abandon();
    s_popups_after_abandon = GImGui->OpenPopupStack.Size;
}

/*
 * Opuštění dialogu, zatímco okno kazety dál kreslí, zavře popup modálu
 * v zásobníku ImGui; ID tabulky výsledků se nemění s novým výsledkem.
 */
void test_e4b_popup_closed_and_table_id(void)
{
    at_setup();
    at_open_analyzed(1);
    at_frames(2);
    bool sb = false;
    const ImGuiID id1 = cmt_analyze_dialog_test_table(&sb);
    TEST_ASSERT_TRUE(id1 != 0);
    /* nový výsledek (Analyze again) a 120 souborů: tatáž tabulka */
    s_syn = AT_SYN_MANY;
    d_click("ad_again");
    TEST_ASSERT_EQUAL_INT(1, cmt_analyze_dialog_test_run_jobs());
    at_frames(5);
    TEST_ASSERT_EQUAL_STRING("done", d_text("ad_state").c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(id1, cmt_analyze_dialog_test_table(&sb), "the results table keeps its ID");
    /* opuštění mimo modál uvnitř snímku (jako opuštění z kreslení okna kazety): popup modálu zavře hned dialog sám,
     * ne až ImGui v dalších snímcích */
    TEST_ASSERT_TRUE(GImGui->OpenPopupStack.Size > 0);
    s_in_frame = at_abandon_in_frame;
    at_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_popups_after_abandon, "abandoning the dialog closes its popup in the same frame");
    TEST_ASSERT_FALSE(cmt_analyze_dialog_is_open());
    at_frames(2);
    TEST_ASSERT_EQUAL_INT(0, GImGui->OpenPopupStack.Size);
}

/* ================================================================
 * Snímky (MZ_CMT_SHOT_DIR, sada j4)
 * ================================================================ */

static char s_shot_tag[64] = "en-100";

/** @brief Uloží levý horní výřez plátna jako PNG. */
static bool shot_save_crop(const char *dir, const char *file, int w, int h)
{
    if (w > s_canvas_w) w = s_canvas_w;
    if (h > s_canvas_h) h = s_canvas_h;
    if (w <= 0 || h <= 0) return false;
    size_t n = (size_t)s_canvas_w * (size_t)s_canvas_h * 4u;
    unsigned char *px = (unsigned char *)malloc(n);
    unsigned char *crop = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!px || !crop) { free(px); free(crop); return false; }
    glReadPixels(0, 0, s_canvas_w, s_canvas_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    size_t src_pitch = (size_t)s_canvas_w * 4u;
    size_t dst_pitch = (size_t)w * 4u;
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * dst_pitch, px + (size_t)(s_canvas_h - 1 - y) * src_pitch, dst_pitch);
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

/**
 * @brief Snímky dialogu analýzy: found, empty, running, stale, details a stavy unsaved, changed, error, cannot - okno Virtual CMT s modálem dialogu.
 * @param dir Adresář. @param scale Měřítko. @return Počet neuložených snímků a snímků s vadou.
 */
static int shots_j4_run(const char *dir, float scale)
{
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    /* 5 až 8: další stavy (varování před Replace u neuloženého bloku, páska se změnila,
     * chyba analýzy, řádek Cannot add s nápovědou) */
    static const char *names[9] = {"found", "empty", "running", "stale", "details", "unsaved", "changed", "error", "cannot"};
    int failed = 0;
    for (int s = 0; s < 9; s++) {
        setUp();
        at_setup(scale, stretch);
        /* fonty a měřítko nastavil main(); at_setup() vrátil styl na měřítko; při 175 % pracovní plocha 1920 x 1040 */
        if (scale > 1.5f) imgui_cmt_window_test_set_work_size(ImVec2(1920.0f, 1040.0f));
        s_syn = (s == 1) ? AT_SYN_NONE : ((s == 7) ? AT_SYN_FAIL : ((s == 8) ? AT_SYN_CANNOT : AT_SYN_FOUND));
        if (s == 5) {
            append_unsaved_raw();
            at_frames(3);
            /* řádek neuloženého bloku do pohledu (při 175 % je tabulka okna kazety kratší) */
            st_CMT_VIEW tv;
            cmt_view_reset(&tv);
            fill(&tv);
            imgui_cmt_window_select_block(3, tv.tape_serial);
            at_frames(3);
        }
        if (s == 2) {
            at_open_dialog(1);
            at_frames(3);
        } else if (s == 7) {
            at_open_dialog(1);
            at_frames(2);
            (void)cmt_analyze_dialog_test_run_jobs();
            at_frames(5);
        } else {
            at_open_analyzed(s == 5 ? 3 : 1);
        }
        if (s == 6) {
            /* páska se změní jinou cestou: blok 0 pryč (čítač výměn pásky) */
            device_state_lock();
            int rem = 0;
            (void)cmt_tape_remove_blocks(&rem, 1);
            device_state_unlock();
            at_frames(3);
        }
        if (s == 0 || s == 4) cmt_analyze_dialog_test_select_row(2);
        if (s == 8) cmt_analyze_dialog_test_select_row(1);
        /* rozbalený histogram vybraného souboru: tělo se při 175 % posouvá */
        if (s == 4) {
            cmt_analyze_dialog_test_set_details(true);
            at_frames(5);
        }
        if (s == 3) d_click("ad_rep_body");
        at_mouse_away();
        for (int i = 0; i < 3; i++) at_frame(true);
        ImVec2 sz(0.0f, 0.0f);
        ImGuiContext &g = *GImGui;
        for (ImGuiWindow *w : g.Windows) {
            if (!w->Active || w->Hidden) continue;
            sz.x = ImMax(sz.x, ceilf(w->Pos.x + w->Size.x) + 1.0f);
            sz.y = ImMax(sz.y, ceilf(w->Pos.y + w->Size.y) + 1.0f);
        }
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("j4-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "", dash ? dash : "", names[s]);
        if (!shot_save_crop(dir, file, (int)sz.x, (int)sz.y)) failed++;
        int faults = at_dialog_faults(file);
        ImVec2 wmin, wmax, imin, imax;
        cmt_analyze_dialog_test_rect(&wmin, &wmax, &imin, &imax);
        fprintf(stderr, "geo: %s crop %.0f x %.0f px, dialog %.0f x %.0f px, font %.1f px\n", file, sz.x, sz.y, wmax.x - wmin.x,
                wmax.y - wmin.y, ImGui::GetFontSize());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
        cmt_analyze_dialog_test_reset();
        at_frame();
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
        /* písma z proměnných: at_fonts() už nic nenačte */
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
        int failed = shots_j4_run(shot_dir, scale);
        ui_teardown();
        mztest_teardown();
        return failed ? 1 : 0;
    }

    s_ui_ok = ui_setup();
    if (!s_ui_ok) {
        mztest_teardown();
        fprintf(stderr, "ui-cmt-analyze: SDL/GL not available, skipped\n");
        return 77;
    }
    UNITY_BEGIN();
    RUN_TEST(test_j4_entry_points);
    RUN_TEST(test_auto_analyze_while_playing);
    RUN_TEST(test_auto_analyze_disabled);
    RUN_TEST(test_j4_layout_and_texts);
    RUN_TEST(test_j4_enter_adds);
    RUN_TEST(test_j4_replace_unsaved_warning);
    RUN_TEST(test_j4_uncheck_all_disables);
    RUN_TEST(test_j4_stale_options_and_analyze_again);
    RUN_TEST(test_j4_running_cancel_frees);
    RUN_TEST(test_j4_tape_changed);
    RUN_TEST(test_j4_nothing_found_and_error);
    RUN_TEST(test_j4_row_hint_space_histogram);
    RUN_TEST(test_j4_apply_pending_after_close);
    RUN_TEST(test_j4_real_thread);
    RUN_TEST(test_j4_second_analysis_waits);
    RUN_TEST(test_j4_stale_pulse_copy_dropped);
    RUN_TEST(test_j4_checksum_missing_body);
    RUN_TEST(test_j4_success_without_edit_result);
    RUN_TEST(test_j4_popup_closed_outside);
    RUN_TEST(test_j4_exit_joins_worker);
    RUN_TEST(test_j4_histogram_max_once);
    RUN_TEST(test_finalb_dialog_kept_without_window_content);
    RUN_TEST(test_finalb_big_block_draws_and_cancels);
    RUN_TEST(test_j4_giant_block_bench);
    RUN_TEST(test_j4_geometry);
    RUN_TEST(test_e4b_one_scrollbar_histogram_visible);
    RUN_TEST(test_e4b_selected_row_colors_and_name_width);
    RUN_TEST(test_e4b_stale_analyze_again_default);
    RUN_TEST(test_e4b_drag_not_recentered);
    RUN_TEST(test_e4b_popup_closed_and_table_id);
    int result = UNITY_END();
    ui_teardown();
    mztest_teardown();
    return result;
}
