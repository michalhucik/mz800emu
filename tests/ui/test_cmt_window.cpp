/*
 * test_cmt_window.cpp - kopie stavu Virtual CMT pro okno (src/ui-imgui/cmt/cmt_view.cpp)
 * a snímky oken kazety se skutečným ImGui ve skrytém okně SDL + GL.
 *
 * Testy kopie stavu (cmt_view_fill() nad skutečným jádrem kazety):
 * - bez pásky: has_tape false, prázdný seznam, -1 bloku, neznámé časy;
 * - MZT se dvěma bloky: STOP (jména, druh MZF, původ z importu MZF, délky
 *   známé, délka pásky = součet, pozice 0, motor ve STOP neznámý), PLAY
 *   (pozice a průběh po 2 s, motor běží), pauza uživatele (motor běží
 *   dál, hrana PC3 přepne jen motor), čekání na motor;
 * - po záznamu (blok připojený cmttmz_append_blocks(), jak ho připojuje
 *   cmt_record_finish()): blok Unsaved a Recorded, páska modified; po
 *   uložení Unsaved zmizí, Recorded zůstane; RECORD: aktuální blok =
 *   počet bloků, čas a pulzy záznamu;
 * - jména: znaky Sharp MZ ASCII mimo ASCII v UTF-8,
 *   prázdné jméno, jméno 16 znaků, cesta pásky se jménem souboru 200
 *   znaků; žádné přetečení (std::string);
 * - vysunutí jiným vláknem mezi dvěma plněními: po
 *   cmt_eject() je seznam prázdný; kopie jmen z dřívějšího plnění zůstává
 *   platná (kopie nevlastní nic z jádra); otevření jiné pásky přestaví seznam;
 * - seznam bloků se sestavuje znovu jen při změně čítačů jádra; změna
 *   rychlosti délky nepočítá (otevřený blok má délku nového streamu);
 * - délky 300 bloků: otevření pásky negeneruje žádný stream pro délky
 *   (háček cmttmz_test_get_length_streams()), zásahy ve STOP doplní zbytek;
 * - zásah délek se nezařadí při PLAY, PLAY v pauze ani RECORD, ve STOP
 *   nejvýš jednou za CMT_VIEW_LENGTHS_INTERVAL_FRAMES snímků; zásah
 *   odmítnutý při replay se zopakuje;
 * - délky nemění otisk stroje (s obsahem pásky) ani revizi stavu;
 * - levná délka (bez streamu) bloků TZX 0x10 až 0x14, 0x18 a 0x20, surového
 *   bloku 0x18 ze záznamu a bloku z importu WAV = délka streamu do 1 vzorku;
 * - testovací stavy (cmt_view_make_fixture()): obsah konceptů v2 a invarianty;
 * - kreslení okna imgui_cmt_window() nad skutečným jádrem (jen s SDL + GL;
 *   bez nich vrátí test po úspěchu ostatních kód 77 = ctest SKIP);
 * - okno Virtual CMT (cmt_window.cpp) nad testovacími stavy:
 *   geometrie decku (žádný prvek mimo okno ani mimo ořez, žádné dva se
 *   nepřekrývají) pro všechny stavy, Compact i Full, měřítka 1,0 a 1,75
 *   a texty běžné i o 40 % delší (testovací obal _() v cmt_window.cpp);
 *   Compact -> Full -> Compact vrátí velikost okna a deck se neposune;
 *   tlačítko Blocks přepíná rozvržení; titulek a stabilní ID; texty a stavy
 *   decku podle konceptů v2 (počítadlo 2,2x, Play / Pause, motor, Direct,
 *   záznam, Block time, Remaining). Druhá registrace ui-cmt-window-de
 *   (MZ_CMT_TEST_LANG=de) spustí jen geometrii v němčině;
 * - tabulka bloků a vlastnosti: sloupce a výchozí skryté sloupce,
 *   texty řádků a stavů, značka pozice, vybraný řádek, vlastnosti podle
 *   druhu bloku a pásky, prázdná páska, řádek Recording..., výběr jako stav
 *   okna (jiná páska ho zruší, výměna bloku ne), řídicí bloky, akce řádku
 *   (dvojklik, Enter, šipky, kontextové menu) zachycené ve frontách
 *   požadavků (replay_test_take_ui_cmt(), device_request_test_take_pending(),
 *   imgui_cmt_tape_test_take_last_export()), View > Columns a nastavení
 *   sloupců přes INI ImGui, čas kreslení 300 bloků proti 5 blokům;
 * - deck: pauza uživatele a motor, požadavky tlačítek decku, páska
 *   s řídicími bloky na obou koncích, INI přes soubor, malá obrazovka;
 * - zkratky jen s Ctrl a jen při fokusu okna, Escape
 *   okno nezavře, Alt+C ano; přepínač Loading (zásah režimu, tabulka režim
 *   x prvek, zákazy při PLAY, RECORD a záznamu vstupů), zásah režimu nad
 *   jádrem, řádek zpráv místo okna zpráv, menu Tape;
 * - ROM bez podpory přímého načtení (CMT hacku): příznak kopie
 *   direct_supported sleduje výměnu ROM; segment Direct, Load directly
 *   a Use CMT Hack hlavního menu zakázané s důvodem a bez zásahu, pořadí
 *   důvodů vůči záznamu vstupů, hrající pásce a bloku bez MZF;
 * - hlavní menu HW & Devices > CMT > CMT Options > Device for Direct MZF
 *   Start se zanořením nabídek (kořen nahrazuje lišta menu, viz
 *   g6_nested_frame()): přepínače CMT, FD, QD, RD
 *   s hodnotou BC a modál Custom value... kreslený mimo nabídky (kontrola
 *   zápisu, úvodní nula, Enter, Escape) mění nastavení [CMT] run_mzf_device;
 * - Load directly je jeden
 *   zásah - nad páskou vyměněnou mezi zařazením a provedením neprovede výběr
 *   ani Direct, při záznamu vstupů se odmítne celý; Play při
 *   nahrávání čekajícím na motor se po přechodu decku do PLAY zahodí;
 *   operace okna zahozená emu vláknem kvůli změně decku = varování v řádku
 *   zpráv, každá zjištěná změna jedno, bez časového slučování;
 * - úpravy pásky v okně: vícenásobný výběr (Ctrl, Shift, Ctrl+A)
 *   a "N blocks selected"; Delete (menu, Del) uložených bloků jedním zásahem
 *   s rozsahy, modál u neuložených (Cancel výchozí, Enter i Escape nic);
 *   Move up / Move down (menu, Alt+Up / Alt+Down) a zákazy na krajích a při
 *   více vybraných; tažení řádku s čárou místa vložení (puštění mimo tabulku
 *   a Escape nic); zákaz všech úprav mimo STOP, při nahrávání a při záznamu
 *   i přehrávání vstupů; Import file... (Ctrl+I), Import file here...,
 *   Export tape as WAV...; přetažení souboru z OS (událost SDL) nad tabulku,
 *   nad deck a mimo okno; obnova výběru po výsledku úpravy nad jádrem
 *   (cestou imgui_device_request_results()); Unsaved podle příznaku bloku;
 *   úprava pásky při otevřeném dialogu souboru; geometrie
 *   nových prvků při 100 % a 175 % i s texty o 40 % delšími.
 *
 * Režim snímků (mimo CTest): proměnná MZ_CMT_SHOT_DIR = adresář snímků
 * okna z testovacích stavů. Volitelně MZ_CMT_SHOT_FONT (TTF hlavního písma
 * 28 px), MZ_CMT_SHOT_MONO (monospace TTF), MZ_CMT_SHOT_SCALE (např. 1.75),
 * MZ_CMT_SHOT_LANG + MZ_CMT_SHOT_LOCALEDIR (jazyk), MZ_CMT_SHOT_W /
 * MZ_CMT_SHOT_H (rozměr plátna), MZ_CMT_SHOT_STRETCH=1 (texty o 40 % delší,
 * jazyk "<jazyk>+40"). MZ_CMT_SHOT_SET vybere sadu: h4 (úpravy pásky:
 * více vybraných, kontextové menu, modál mazání, tažení, zpráva importu), g2 (deck), g3
 * (rozvržení Full), g4 (výchozí: Direct, čekání na motor, pauza, záznam),
 * g5 a g6 (všechny testovací stavy v rozvržení Compact i Full, jméno
 * "g5-<jazyk>-<měřítko>-<rozvržení>-<stav>.png", g6 stejně s předponou g6);
 * MZ_CMT_SHOT_LANG nesmí obsahovat oddělovač cesty ani "..";
 * jména "<sada>-<jazyk>-<měřítko v %>-<stav>.png" (výřez velikosti okna),
 * geometrie "geo:" a kontrola "check:" na stderr. Snímky "před" (staré
 * okno a Tape Filelist) jsou archivované mimo repozitář, staré okno už neexistuje.
 * Bez MZ_CMT_SHOT_DIR test nic mimo dočasný adresář nezapisuje.
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
#include <locale.h>
#include <string>
#include <vector>
#include <algorithm>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

extern "C" {
#include "mztest.h"
#include "device_request.h"
#include "machine_hash.h"
#include "replay.h"
#include "state_rev.h"
#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/memory/rom.h"
#include "snapshot/snapshot_request.h"
#include "debugger/png_encode.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/mzf/mzf.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
}

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/topmenu/menu_recent.h"
#include "recent_media/recent_media.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/cmt/cmt_window.h"
#include "ui-imgui/cmt/cmt_tokens.h"
#include "ui-imgui/devices/device_request_ui.h"
#include "ui-imgui/message/message_window.h"
#include "ui-imgui/cmt/ImGuiCmt.hpp"
#include "ui-imgui/main_window_focus.h"
#include "hw-generic/cmt/cmt_request.h"
#include "mzarch/bootstrap_device.h"
#include "ui-imgui/imgui_modal.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
/* data ikon IGFD (static pole) jako myimgui.cpp: zahrnutím .cpp */
#include "ui-imgui/filechooser/res/CustomFont.cpp"
#include "ui-imgui/filechooser/res/CustomFont.h"
#include "i18n.h"

/* Písma aplikace a katalogy překladů (tests/ui/CMakeLists.txt); prázdné = nejsou. */
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
/* imgui_device_request_results() (device_request_ui.cpp) předává výsledky i Unicardu */
extern "C" bool imgui_unicard_device_request_result(const st_DEVICE_REQ_RESULT *res)
{
    (void)res;
    return false;
}

/* ================================================================
 * SDL3 + GL + ImGui (vzor test_bus_window.cpp)
 * ================================================================ */

static SDL_Window *s_win = NULL;
static SDL_GLContext s_gl = NULL;
static ImGuiContext *s_ctx = NULL;
static bool s_ui_ok = false;
static int s_canvas_w = 1280, s_canvas_h = 900;

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
 * Pomocné funkce: páska, čas
 * ================================================================ */

/** @brief Dočasné soubory testu (maže tearDown). */
static char *s_paths[4] = {NULL, NULL, NULL, NULL};

/**
 * @brief Cesta dočasného souboru se zadaným jménem (slot 0 až 3).
 * @param slot Slot. @param name Jméno souboru (základ).
 * @return Cesta v g_get_tmp_dir() (vlastní ji test).
 */
static const char *tmp_named(int slot, const char *name)
{
    gchar *dir = g_strdup_printf("mztest_cmt_window_%u", (unsigned)g_random_int());
    gchar *full_dir = g_build_filename(g_get_tmp_dir(), dir, NULL);
    g_mkdir_with_parents(full_dir, 0700);
    g_free(s_paths[slot]);
    s_paths[slot] = g_build_filename(full_dir, name, NULL);
    g_free(dir);
    g_free(full_dir);
    return s_paths[slot];
}

/**
 * @brief Hlavička MZF.
 * @param hdr Výstup. @param name Jméno v kódech Sharp MZ ASCII (len bajtů). @param len Délka jména.
 * @param ftype Typ. @param fsize Velikost těla.
 */
static void make_hdr(st_MZF_HEADER *hdr, const uint8_t *name, size_t len, uint8_t ftype, uint16_t fsize)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = ftype;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, len);
    hdr->fsize = fsize;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
}

/**
 * @brief Sestaví blok 0x46 (MZ800EMU-CMT, 1:1) s MZF.
 * @param name Jméno (ASCII). @param ftype Typ. @param fsize Tělo. @param pause_ms Pauza za blokem.
 * @return Blok (data převezme soubor nebo cmttmz_append_blocks()).
 */
static st_TZX_BLOCK *mzf_block_raw(const uint8_t *name, size_t len, uint8_t ftype, uint16_t fsize, uint16_t pause_ms)
{
    st_MZF_HEADER hdr;
    make_hdr(&hdr, name, len, ftype, fsize);
    uint8_t *body = (uint8_t *)malloc(fsize ? fsize : 1);
    for (int i = 0; i < fsize; i++) body[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, fsize, CMTSPEED_1_1, NULL, pause_ms);
    free(body);
    TEST_ASSERT_NOT_NULL(b);
    return b;
}

/** @brief Přidá blok MZF se jménem v ASCII do souboru. */
static void add_mzf(st_TZX_FILE *f, const char *name, uint8_t ftype, uint16_t fsize, uint16_t pause_ms)
{
    st_TZX_BLOCK *b = mzf_block_raw((const uint8_t *)name, strlen(name), ftype, fsize, pause_ms);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
}

/** @brief Přidá surový blok 0x18 (CSW, GDGCLK_BASE / 2 jako záznam emulátoru) s n pulzy délky 200 vzorků. */
static void add_raw(st_TZX_FILE *f, uint32_t n)
{
    uint32_t *p = (uint32_t *)malloc(n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) p[i] = 200;
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(GDGCLK_BASE / 2, 1000, p, n, &b));
    free(p);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Prázdný soubor TMZ. */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/** @brief Uloží soubor jako TMZ na cestu a uvolní ho. */
static void save_file(st_TZX_FILE *f, const char *path)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
}

/** @brief Otevře pásku pod zámkem stavu zařízení (jako zásah emu vlákna). */
static void open_tape(const char *path)
{
    device_state_lock();
    int rc = cmt_open_file(path, NULL);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
}

/** @brief Vyplní kopii pod zámkem (jako UI vlákno). */
static void fill(st_CMT_VIEW *v)
{
    device_state_lock();
    cmt_view_fill(v);
    device_state_unlock();
}

/** @brief Výsledek fill_lengths(): počet zásahů, snímků a nejdelší zásah. */
typedef struct st_LENGTHS_RUN {
    int requests;   /**< zařazené a provedené zásahy cmt_request_lengths() */
    int frames;     /**< volání cmt_view_request_lengths() (snímky okna) */
    gint64 worst_us; /**< nejdelší device_request_poll() v µs */
} st_LENGTHS_RUN;

/**
 * @brief Doplní délky bloků jako okno ve STOP: snímek = request, poll emu vlákna, plnění.
 *
 * Ověřuje i pravidla zařazení: nejvýš jeden zásah za
 * CMT_VIEW_LENGTHS_INTERVAL_FRAMES snímků a jen ve STOP.
 *
 * @param v Kopie po fill() (deck ve STOP).
 * @return Počty zásahů a snímků, nejdelší zásah.
 */
static st_LENGTHS_RUN fill_lengths(st_CMT_VIEW *v)
{
    st_LENGTHS_RUN run = {0, 0, 0};
    int since = 1000;
    while (v->lengths_pending && run.frames < 100000) {
        run.frames++;
        since++;
        if (!cmt_view_request_lengths(v)) {
            fill(v);
            continue;
        }
        TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_STOP, v->state);
        TEST_ASSERT_TRUE_MESSAGE(since >= (int)CMT_VIEW_LENGTHS_INTERVAL_FRAMES, "at most one length request per 10 frames");
        since = 0;
        gint64 p0 = g_get_monotonic_time();
        device_request_poll();
        gint64 p1 = g_get_monotonic_time();
        if (p1 - p0 > run.worst_us) run.worst_us = p1 - p0;
        st_DEVICE_REQ_RESULT res;
        while (device_request_take_result(&res)) {
            TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LENGTHS_NAME, res.name);
            TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
        }
        run.requests++;
        fill(v);
    }
    TEST_ASSERT_FALSE_MESSAGE(v->lengths_pending, "all lengths are known after the requests");
    return run;
}

/** @brief Posune GDG čas o takty (jako test_cmt_tmz.c; smyčka snímků v testu neběží). */
static void advance_ticks(uint64_t ticks)
{
    g_gdg.total_elapsed.ticks += (unsigned)ticks;
}

/**
 * @brief Připojí k pásce blok MZF jako záznam (cmttmz_append_blocks(), rychlost DEFAULT jako záznam).
 * @param name Jméno (ASCII). @param fsize Tělo.
 */
static void append_recorded(const char *name, uint16_t fsize)
{
    st_TZX_BLOCK *b = mzf_block_raw((const uint8_t *)name, strlen(name), 0x01, fsize, 0);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    device_state_lock();
    int rc = cmttmz_append_blocks(b, &sp, 1);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    tmz_block_free(b);
}

/**
 * @brief Testovací páska games.tmz jako koncept v2: LOADER, FLAPPY, HISCORE, surový blok, SAVED GAME (záznam).
 * @param slot Slot dočasného souboru.
 * @return Cesta souboru (4 bloky; pátý připojí záznam po otevření).
 */
static const char *make_games_tape(int slot)
{
    st_TZX_FILE *f = new_tmz();
    add_mzf(f, "LOADER", 0x01, 1024, 1000);
    add_mzf(f, "FLAPPY", 0x01, 12288, 1000);
    add_mzf(f, "HISCORE", 0x03, 256, 1000);
    add_raw(f, 4000);
    const char *path = tmp_named(slot, "games.tmz");
    save_file(f, path);
    return path;
}

/** @brief Otevře games.tmz a připojí SAVED GAME jako záznam (5 bloků, Modified). */
static void open_games_tape(void)
{
    open_tape(make_games_tape(0));
    append_recorded("SAVED GAME", 4096);
}

/** @brief Hodnoty g_cmt před testem (obnovuje tearDown). */
static en_CMTSPEED s_saved_speed;
static unsigned s_saved_hack;
static en_CMT_CPU_BOOST s_saved_boost;
/** @brief ROM před testem (testy ROM bez podpory CMT hacku ji mění; vrací ji tearDown i po selhání). */
static st_ROM_CONFIG_ROW *s_saved_rom;

void setUp(void)
{
    g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
    g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
    s_saved_speed = g_cmt.mz_cmtspeed;
    s_saved_hack = g_cmthack.load_patch_installed;
    s_saved_boost = g_cmt.cpu_boost;
    s_saved_rom = g_rom.rom_cfg;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
    g_cmthack.load_patch_installed = 0;
    g_cmt.cpu_boost = CMT_CPU_BOOST_DISABLED;
    /* zásah ve frontě z testu, který skončil chybou, nepatří do dalšího */
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    /* testy bez kreslení okna plní řádek zpráv (fill()); skryté okno zkouší test_g4f_messages_hidden_window() */
    cmt_view_test_force_window_shown(1);
}

/** @brief Kořen konfigurace před test_g2f_ini_round_trip() (vrací ho tearDown po selhání). */
static st_CFGROOT *s_g2f_saved_cfgroot = NULL;

/** @brief test_g2f_ini_round_trip() právě nahradil g_cfgmain dočasným kořenem. */
static bool s_g2f_cfgroot_replaced = false;

void tearDown(void)
{
    /* asertace, která selhala pod device_state_lock() (Unity longjmp), zámek
     * neuvolnila: bez toho by se tearDown na zámku zablokoval navždy */
    if (device_state_lock_held()) device_state_unlock();
    /* test_g2f_ini_round_trip po selhání asertace: konfigurace zpět (dočasný
     * kořen se neuvolní, mohl už být uvolněný) */
    if (s_g2f_cfgroot_replaced)
    {
        g_cfgmain = s_g2f_saved_cfgroot;
        s_g2f_cfgroot_replaced = false;
        imgui_cmt_window_test_cfg_forget();
    }
    device_state_lock();
    cmt_stop();
    cmt_eject();
    /* ROM bez podpory CMT hacku z testu zpět (před úklidem hacku, jako test_cmt_request.c) */
    if (g_rom.rom_cfg != s_saved_rom) rom_reinstall(s_saved_rom);
    /* test, který skutečně nainstaloval ROM patch CMT hacku (zásah režimu Direct)
     * a selhal před odinstalací, nesmí nechat ROM patchovanou */
    if (CMTHACK_TEST_IS_INSTALLED) cmthack_load_rom_patch(0);
    device_state_unlock();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmthack.load_patch_installed = s_saved_hack;
    g_cmt.cpu_boost = s_saved_boost;
    for (int i = 0; i < 4; i++) {
        if (s_paths[i]) {
            gchar *dir = g_path_get_dirname(s_paths[i]);
            g_remove(s_paths[i]);
            g_rmdir(dir);
            g_free(dir);
            g_free(s_paths[i]);
            s_paths[i] = NULL;
        }
    }
}

/* ================================================================
 * Testy kopie stavu (cmt_view_fill)
 * ================================================================ */

/* Bez pásky: nic z pásky, neznámé časy, STOP. */
void test_view_no_tape(void)
{
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_FALSE(v.has_tape);
    TEST_ASSERT_TRUE(v.blocks.empty());
    TEST_ASSERT_EQUAL_INT(0, v.block_count);
    TEST_ASSERT_EQUAL_INT(-1, v.current_block);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_STOP, v.state);
    TEST_ASSERT_TRUE(v.tape_total_s < 0.0);
    TEST_ASSERT_TRUE(v.tape_elapsed_s < 0.0);
    TEST_ASSERT_FALSE(v.motor_known);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_TAPE, v.mode);
    TEST_ASSERT_EQUAL_STRING("1:1", v.default_speed_text.c_str());
}

/**
 * @brief Najde předdefinovanou ROM bez podpory CMT hacku (jako test_cmt_request.c).
 * @return Řádek g_rom_config->roms; NULL = build takovou ROM nemá.
 */
static st_ROM_CONFIG_ROW *find_rom_without_hack(void)
{
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++) {
        if ((r->type == ROM_CONFIG_TYPE_ROM) && r->enabled && (r->flags & ROM_CONFIG_FLAG_CMTHACK_INCOMP)
            && !(r->flags & ROM_CONFIG_FLAG_USER_DEFINED))
            return r;
    }
    return NULL;
}

/**
 * @brief Přepne ROM pod zámkem stavu zařízení (jako zásah emu vlákna; ROM vrací tearDown).
 * @param row Řádek g_rom_config->roms.
 */
static void switch_rom(st_ROM_CONFIG_ROW *row)
{
    device_state_lock();
    rom_reinstall(row);
    device_state_unlock();
}

/**
 * @brief Příznak přímého načtení v kopii sleduje vybranou ROM (bez pásky i s páskou).
 *
 * Výchozí kopie (cmt_view_reset()) počítá s ROM, která přímé načtení umí;
 * plnění nad ROM bez podpory CMT hacku dá false, návrat původní ROM true.
 * Bez takové ROM v buildu se test přeskočí.
 */
void test_view_direct_supported_follows_rom(void)
{
    if (TEST_ROM_CMTHACK_INCOMP) TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    st_ROM_CONFIG_ROW *incomp = find_rom_without_hack();
    if (!incomp) TEST_IGNORE_MESSAGE("no predefined ROM incompatible with the CMT hack in this build");
    st_ROM_CONFIG_ROW *orig = g_rom.rom_cfg;
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.direct_supported, "the reset copy assumes a ROM with direct loading");
    fill(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.direct_supported, "the default ROM supports direct loading");
    switch_rom(incomp);
    fill(&v);
    TEST_ASSERT_FALSE_MESSAGE(v.direct_supported, "an incompatible ROM, no tape");
    open_games_tape();
    fill(&v);
    TEST_ASSERT_FALSE_MESSAGE(v.direct_supported, "an incompatible ROM, tape inserted");
    switch_rom(orig);
    fill(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.direct_supported, "back on the original ROM");
}

/** @brief Uloží MZT ze dvou MZF (PROG1, PROG2) a vrátí cestu. */
static const char *make_two_block_mzt(void)
{
    GByteArray *a = g_byte_array_new();
    const char *names[2] = {"PROG1", "PROG2"};
    for (int k = 0; k < 2; k++) {
        uint8_t raw[128 + 64];
        memset(raw, 0, sizeof(raw));
        raw[0] = 0x01;
        memset(&raw[1], 0x0D, 17);
        memcpy(&raw[1], names[k], strlen(names[k]));
        raw[0x12] = 64;
        raw[0x15] = 0x12;
        raw[0x17] = 0x12;
        for (int i = 0; i < 64; i++) raw[128 + i] = (uint8_t)(i * 3 + k);
        g_byte_array_append(a, raw, sizeof(raw));
    }
    const char *path = tmp_named(1, "two.mzt");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)a->data, (gssize)a->len, NULL));
    g_byte_array_free(a, TRUE);
    return path;
}

/* MZT se dvěma bloky ve STOP, PLAY a pauze. */
void test_view_mzt_stop_play_pause(void)
{
    open_tape(make_two_block_mzt());
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_TRUE(v.has_tape);
    TEST_ASSERT_EQUAL_STRING("two.mzt", v.tape_name.c_str());
    TEST_ASSERT_TRUE(g_str_has_suffix(v.tape_path.c_str(), "two.mzt"));
    TEST_ASSERT_FALSE(v.modified);
    TEST_ASSERT_EQUAL_INT(2, v.block_count);
    TEST_ASSERT_EQUAL_INT(2, (int)v.blocks.size());
    TEST_ASSERT_EQUAL_STRING("PROG1", v.blocks[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("PROG2", v.blocks[1].name.c_str());
    for (int i = 0; i < 2; i++) {
        const st_CMT_VIEW_BLOCK &b = v.blocks[(size_t)i];
        TEST_ASSERT_EQUAL_INT(i, b.id);
        TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_MZF, b.kind);
        TEST_ASSERT_TRUE(b.has_mzf);
        TEST_ASSERT_EQUAL_INT(64, b.fsize);
        TEST_ASSERT_EQUAL_INT(0x1200, b.fstrt);
        TEST_ASSERT_EQUAL_STRING("01 machine code", b.type_text.c_str());
        TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED_MZF, b.source);
        TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_OK, b.status);
        TEST_ASSERT_EQUAL_STRING("OK", b.status_text.c_str());
        TEST_ASSERT_EQUAL_INT(CMT_VIEW_SPEED_DEFAULT, b.speed_kind);
        TEST_ASSERT_EQUAL_STRING("1:1", b.speed_text.c_str());
        TEST_ASSERT_TRUE(b.pulses_us[0] > 0.0 && b.pulses_us[2] > 0.0);
        TEST_ASSERT_TRUE(b.playable && b.selectable);
    }
    /* otevření pásky délky nepočítá: známá je jen délka otevřeného bloku (jeho stream) */
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[0].length_s > 1.0, "the opened block has its length for free");
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[1].length_s < 0.0, "a never opened MZ block is unknown after opening the tape");
    TEST_ASSERT_TRUE(v.tape_total_s < 0.0);
    TEST_ASSERT_TRUE(v.lengths_pending);
    /* zásahy ve STOP doplní zbytek */
    fill_lengths(&v);
    TEST_ASSERT_TRUE(v.blocks[1].length_s > 1.0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, v.blocks[0].length_s + v.blocks[1].length_s, v.tape_total_s);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_STOP, v.state);
    TEST_ASSERT_EQUAL_INT(0, v.current_block);
    TEST_ASSERT_FLOAT_WITHIN(1e-9, 0.0, v.tape_elapsed_s);
    TEST_ASSERT_FALSE_MESSAGE(v.motor_known, "the core does not model the motor in STOP");
    TEST_ASSERT_TRUE(v.direct_block_loadable);

    /* PLAY: 2 s v prvním bloku */
    device_state_lock();
    cmt_play();
    device_state_unlock();
    advance_ticks(2ull * GDGCLK_BASE);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_PLAY, v.state);
    TEST_ASSERT_FALSE(v.paused);
    TEST_ASSERT_TRUE(v.motor_known);
    TEST_ASSERT_TRUE(v.motor_on);
    TEST_ASSERT_EQUAL_INT(0, v.current_block);
    TEST_ASSERT_FLOAT_WITHIN(1e-3, 2.0, v.tape_elapsed_s);
    TEST_ASSERT_FLOAT_WITHIN(1e-3, 2.0 / v.blocks[0].length_s, v.block_progress);

    /* pauza uživatele (D-G2-3): páska stojí, motor dál běží, pozice drží */
    device_state_lock();
    cmt_pause(1);
    device_state_unlock();
    advance_ticks(GDGCLK_BASE);
    fill(&v);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_TRUE(v.user_paused);
    TEST_ASSERT_TRUE(v.motor_known);
    TEST_ASSERT_TRUE(v.motor_on);
    TEST_ASSERT_FLOAT_WITHIN(1e-3, 2.0, v.tape_elapsed_s);

    /* hrana PC3 při pauze uživatele: motor stojí, pauza uživatele trvá */
    device_state_lock();
    cmt_motor_toggle();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_TRUE(v.user_paused);
    TEST_ASSERT_FALSE(v.motor_on);

    /* pokračování: pauza uživatele pryč, motor stojí dál (Waiting for motor); hrana ho rozběhne */
    device_state_lock();
    cmt_pause(0);
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_FALSE(v.user_paused);
    TEST_ASSERT_FALSE(v.motor_on);
    device_state_lock();
    cmt_motor_toggle();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_FALSE(v.paused);
    TEST_ASSERT_TRUE(v.motor_on);

    /* stojící motor bez pauzy uživatele: čeká na motor */
    device_state_lock();
    cmt_motor_toggle();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_FALSE(v.user_paused);
    TEST_ASSERT_FALSE(v.motor_on);

    /* druhý blok (seek ve STOP): pozice od začátku pásky = délka prvního bloku */
    device_state_lock();
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(1, v.current_block);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, v.blocks[0].length_s, v.tape_elapsed_s);
}

/* Po záznamu: blok Unsaved a Recorded, páska modified; uložení Unsaved zruší. RECORD: aktuální blok = počet bloků. */
void test_view_after_recording(void)
{
    open_tape(make_two_block_mzt());
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(2, v.block_count);

    /* RECORD v pauze, motor stojí; aktuální blok je ten, který vzniká */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    cmt_pause(0);
    device_state_unlock();
    advance_ticks(GDGCLK_BASE / 2);
    device_state_lock();
    cmt_write_data(1);
    device_state_unlock();
    advance_ticks(GDGCLK_BASE / 1000);
    device_state_lock();
    cmt_write_data(0);
    device_state_unlock();
    advance_ticks(GDGCLK_BASE / 1000);
    device_state_lock();
    cmt_write_data(1);
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_RECORD, v.state);
    TEST_ASSERT_EQUAL_INT(v.block_count, v.current_block);
    TEST_ASSERT_TRUE(v.motor_known && v.motor_on);
    TEST_ASSERT_TRUE(v.rec_time_s > 0.4);
    TEST_ASSERT_EQUAL_UINT32(1, v.rec_pulses);
    TEST_ASSERT_FALSE(v.direct_block_loadable);
    /* STOP dekóduje tři hrany na surový blok nebo nic (pod mezí dekodéru) */
    device_state_lock();
    cmt_stop();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_STOP, v.state);
    int before = v.block_count;

    /* blok připojený záznamem (jako cmt_record_finish()) */
    append_recorded("SAVED GAME", 64);
    fill(&v);
    TEST_ASSERT_TRUE(v.modified);
    TEST_ASSERT_EQUAL_INT(before + 1, v.block_count);
    const st_CMT_VIEW_BLOCK &nb = v.blocks.back();
    TEST_ASSERT_EQUAL_STRING("SAVED GAME", nb.name.c_str());
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_UNSAVED, nb.status);
    TEST_ASSERT_EQUAL_STRING("Unsaved", nb.status_text.c_str());
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_RECORDED, nb.source);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_OK, v.blocks[0].status);
    /* blok MZ ze záznamu se levně spočítat nedá: doplní ho zásah ve STOP */
    fill_lengths(&v);
    TEST_ASSERT_TRUE(v.blocks.back().length_s > 0.0);

    /* uložení: nic neuloženého, původ záznamu zůstává */
    const char *saved = tmp_named(2, "saved.tmz");
    device_state_lock();
    int rc = cmttmz_save(saved);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    fill(&v);
    TEST_ASSERT_FALSE(v.modified);
    TEST_ASSERT_EQUAL_STRING("saved.tmz", v.tape_name.c_str());
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_OK, v.blocks.back().status);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_RECORDED, v.blocks.back().source);
}

/** @brief Importuje soubor do vložené pásky před blok @p at pod zámkem (jako zásah emu vlákna). */
static void import_into_tape(const char *path, int at)
{
    device_state_lock();
    int rc = cmt_tape_import_file(path, at, NULL);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, rc, cmttmz_get_edit_error());
}

/*
 * Vlastnost Source se u bloku z importu neřídí zdrojem
 * celé pásky. Neuložený blok, který nevznikl záznamem, ale importem, má
 * CMT_VIEW_SOURCE_IMPORTED ("Imported"), i když přišel z MZF do pásky TMZ
 * nebo z TMZ do pásky otevřené z MZT; přesun ho nemění. Po uložení je to
 * blok souboru pásky (dosavadní text). Bloky otevřeného souboru a nahrané
 * bloky si texty nechávají.
 */
void test_final_source_of_imported_block(void)
{
    const char *mzt = make_two_block_mzt();
    /* TMZ ze dvou bloků */
    open_tape(mzt);
    const char *tmz = tmp_named(2, "base.tmz");
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tmz));
    device_state_unlock();
    open_tape(tmz);
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_TAPE_FILE, v.blocks[0].source);

    /* MZT do pásky TMZ: importované bloky Imported, dosavadní From tape file */
    import_into_tape(mzt, 1);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(4, v.block_count);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_TAPE_FILE, v.blocks[0].source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_VIEW_SOURCE_IMPORTED, v.blocks[1].source, "an MZF imported into a TMZ tape");
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED, v.blocks[2].source);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_TAPE_FILE, v.blocks[3].source);
    /* přesun importovaného bloku původ nemění: [T, I, I, T] -> [T, I, T, I] */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 3));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED, v.blocks[1].source);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_TAPE_FILE, v.blocks[2].source);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED, v.blocks[3].source);
    /* nahraný blok zůstává Recorded */
    append_recorded("REC", 64);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_RECORDED, v.blocks.back().source);
    /* po uložení jsou importované bloky bloky souboru pásky */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tmp_named(3, "after.tmz")));
    device_state_unlock();
    fill(&v);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_VIEW_SOURCE_TAPE_FILE, v.blocks[(size_t)i].source, "saved blocks come from the tape file");
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_RECORDED, v.blocks.back().source);

    /* TMZ do pásky otevřené z MZT: importované Imported, dosavadní Imported from MZF */
    open_tape(mzt);
    import_into_tape(tmz, 2);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(4, v.block_count);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED_MZF, v.blocks[0].source);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED_MZF, v.blocks[1].source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_VIEW_SOURCE_IMPORTED, v.blocks[2].source, "a TMZ block imported into a tape opened from MZT");
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SOURCE_IMPORTED, v.blocks[3].source);
    device_state_lock();
    cmt_eject();
    device_state_unlock();
}

/* Značka pásky (checkpoint) nese příznaky neuloženo bloků: návrat vrátí Unsaved. */
void test_view_unsaved_survives_tape_mark(void)
{
    open_tape(make_two_block_mzt());
    append_recorded("REC ONE", 64);
    st_CMTTMZ_TAPE_MARK mark;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    device_state_unlock();
    TEST_ASSERT_EQUAL_UINT32(3, mark.block_count);
    TEST_ASSERT_NOT_NULL(mark.unsaved);
    TEST_ASSERT_EQUAL_UINT8(0, mark.unsaved[0]);
    TEST_ASSERT_EQUAL_UINT8(0, mark.unsaved[1]);
    TEST_ASSERT_EQUAL_UINT8(1, mark.unsaved[2]);
    append_recorded("REC TWO", 64);
    const char *saved = tmp_named(2, "mark.tmz");
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(saved));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_restore(&mark));
    device_state_unlock();
    cmttmz_tape_mark_free(&mark);
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(3, v.block_count);
    TEST_ASSERT_TRUE(v.modified);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_OK, v.blocks[1].status);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_UNSAVED, v.blocks[2].status);
    fill_lengths(&v);
    TEST_ASSERT_TRUE(v.blocks[2].length_s > 0.0);
}

/* Znaky Sharp MZ ASCII mimo ASCII, prázdné jméno, 16 znaků, cesta 200 znaků. */
void test_view_names(void)
{
    /* kód Sharp MZ ASCII (EU) pro "ä" a "Ö" z knihovny, ne z hlavy */
    int c_ae = sharpmz_from_utf8("\xC3\xA4", SHARPMZ_CHARSET_EU);
    int c_oe = sharpmz_from_utf8("\xC3\x96", SHARPMZ_CHARSET_EU);
    TEST_ASSERT_TRUE_MESSAGE(c_ae > 0x7F && c_oe > 0x7F, "Sharp MZ ASCII has non-ASCII codes for a-umlaut and O-umlaut");
    uint8_t intl[6] = {'S', (uint8_t)c_ae, 'T', (uint8_t)c_oe, 'R', 'E'};
    st_TZX_FILE *f = new_tmz();
    st_TZX_BLOCK *b = mzf_block_raw(intl, sizeof(intl), 0x01, 32, 1000);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
    b = mzf_block_raw((const uint8_t *)"", 0, 0x02, 32, 1000);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
    add_mzf(f, "ABCDEFGHIJKLMNOP", 0x03, 32, 0);

    /* obraz pásky s cestou, jejíž jméno souboru má 200 znaků (soubor nemusí existovat) */
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    tzx_free(f);
    std::string base(196, 'n');
    base += ".tmz";
    std::string path = std::string("C:/tapes/") + base;
    device_state_lock();
    int rc = cmt_open_image((const uint8_t *)h.spec.memspec.ptr, h.spec.memspec.size, path.c_str(), NULL, 0);
    device_state_unlock();
    generic_driver_close(&h);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);

    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(3, v.block_count);
    TEST_ASSERT_EQUAL_STRING("S\xC3\xA4T\xC3\x96RE", v.blocks[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("", v.blocks[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("02 BASIC program", v.blocks[1].type_text.c_str());
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", v.blocks[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING("03 data", v.blocks[2].type_text.c_str());
    TEST_ASSERT_EQUAL_INT(200, (int)v.tape_name.size());
    TEST_ASSERT_EQUAL_STRING(base.c_str(), v.tape_name.c_str());
    TEST_ASSERT_EQUAL_STRING(path.c_str(), v.tape_path.c_str());
    TEST_ASSERT_TRUE(v.modified);
}

/*
 * Nástroje nad páskou: profil a převoditelnost bloku v kopii,
 * čtení informací o pásce a kontroly pod zámkem, nabídka profilů.
 */
void test_view_tape_tools_reads(void)
{
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    st_CMTTMZ_TAPE_INFO info;
    std::vector<st_CMTTMZ_CHECK_ITEM> items;
    uint32_t checked = 99;
    /* bez pásky */
    device_state_lock();
    TEST_ASSERT_FALSE(cmt_view_read_tape_info(&info));
    TEST_ASSERT_FALSE(cmt_view_read_tape_check(&items, &checked));
    device_state_unlock();
    TEST_ASSERT_TRUE(items.empty());
    TEST_ASSERT_EQUAL_UINT32(0, checked);

    /* MZT: bloky 0x46 MZ800EMU-CMT, převoditelné */
    open_tape(make_two_block_mzt());
    fill(&v);
    TEST_ASSERT_EQUAL_INT(2, v.block_count);
    TEST_ASSERT_TRUE(v.blocks[0].profile_convertible);
    TEST_ASSERT_EQUAL_STRING("MZ800EMU-CMT", v.blocks[0].profile_id.c_str());
    device_state_lock();
    TEST_ASSERT_TRUE(cmt_view_read_tape_info(&info));
    TEST_ASSERT_TRUE(cmt_view_read_tape_check(&items, &checked));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("", info.title);
    TEST_ASSERT_EQUAL_INT(-1, info.machine);
    TEST_ASSERT_EQUAL_UINT(cmttmz_get_tape_serial(), info.tape_serial);
    TEST_ASSERT_TRUE(items.empty());
    TEST_ASSERT_EQUAL_UINT32(2, checked);

    /* surový blok: nepřevoditelný, bez profilu */
    st_TZX_FILE *f = new_tmz();
    add_raw(f, 100);
    gchar *path = g_build_filename(g_get_tmp_dir(), "mztest_view_tools_raw.tmz", NULL);
    save_file(f, path);
    open_tape(path);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(1, v.block_count);
    TEST_ASSERT_FALSE(v.blocks[0].profile_convertible);
    TEST_ASSERT_EQUAL_STRING("", v.blocks[0].profile_id.c_str());
    device_state_lock();
    cmt_eject();
    device_state_unlock();
    g_remove(path);
    g_free(path);

    /* nabídka profilů: stejná jako registr jádra, MZ800EMU-CMT přijímá rychlost */
    std::vector<st_CMT_VIEW_PROFILE> profiles;
    cmt_view_read_profiles(&profiles);
    TEST_ASSERT_EQUAL_UINT32(cmttmz_profile_count(), (uint32_t)profiles.size());
    bool found = false;
    for (const st_CMT_VIEW_PROFILE &p : profiles)
    {
        TEST_ASSERT_FALSE(p.id.empty());
        if (p.id == "MZ800EMU-CMT")
        {
            found = true;
            TEST_ASSERT_TRUE(p.accepts_speed);
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* Vysunutí a otevření jiné pásky mezi dvěma plněními. */
void test_view_eject_between_fills(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(5, v.block_count);
    std::string kept = v.blocks[1].name; /* kopie, ne ukazatel do indexu */

    /* jiné vlákno (MCP) pásku vysune pod zámkem */
    device_state_lock();
    cmt_eject();
    device_state_unlock();
    /* kopie z dřívějšího plnění je pořád celá (nic neukazuje do uvolněného indexu) */
    TEST_ASSERT_EQUAL_STRING("FLAPPY", v.blocks[1].name.c_str());
    fill(&v);
    TEST_ASSERT_FALSE(v.has_tape);
    TEST_ASSERT_TRUE(v.blocks.empty());
    TEST_ASSERT_EQUAL_INT(-1, v.current_block);
    TEST_ASSERT_EQUAL_STRING("FLAPPY", kept.c_str());

    /* otevření jiné pásky: seznam je ten nový */
    open_tape(make_two_block_mzt());
    fill(&v);
    TEST_ASSERT_EQUAL_INT(2, v.block_count);
    TEST_ASSERT_EQUAL_STRING("PROG1", v.blocks[0].name.c_str());
}

/* Seznam bloků se sestaví znovu jen při změně čítačů jádra; změna rychlosti bloku ho přestaví. */
void test_view_rebuild_only_on_change(void)
{
    open_tape(make_two_block_mzt());
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    unsigned os = v.open_serial, cs = v.change_serial;
    v.blocks[0].name = "MARKER"; /* bez změny v jádru se seznam nepřestaví */
    fill(&v);
    TEST_ASSERT_EQUAL_UINT(os, v.open_serial);
    TEST_ASSERT_EQUAL_UINT(cs, v.change_serial);
    TEST_ASSERT_EQUAL_STRING("MARKER", v.blocks[0].name.c_str());

    /* rychlost bloku 1 (zásah z GUI) -> nový čítač změn, seznam se přestaví, délka odpovídá 2:1 */
    fill_lengths(&v);
    double len1 = v.blocks[1].length_s;
    TEST_ASSERT_TRUE(len1 > 0.0);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1, true, NULL));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("PROG1", v.blocks[0].name.c_str());
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_SPEED_SET, v.blocks[1].speed_kind);
    TEST_ASSERT_EQUAL_STRING("2:1", v.blocks[1].speed_text.c_str());
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[1].length_s < 0.0, "a speed change only invalidates the length of a block that is not open");
    fill_lengths(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[1].length_s > 0.0 && v.blocks[1].length_s < len1, "length follows the block speed");

    /* výchozí rychlost ve STOP: otevřený blok 0 má délku nového streamu hned, blok 1 (SET) se nemění */
    double len0 = v.blocks[0].length_s;
    unsigned streams = cmttmz_test_get_length_streams();
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(streams, cmttmz_test_get_length_streams(), "a speed change generates no stream for lengths");
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[0].length_s > 0.0 && v.blocks[0].length_s < len0, "the open block has the length of its new stream");
    TEST_ASSERT_FALSE(v.lengths_pending);
}

/** @brief Uloží pásku s n bloky MZF "PROGRAM nnn" po 1 KB a vrátí cestu (slot 3). */
static const char *make_many_tape(int n)
{
    st_TZX_FILE *f = new_tmz();
    for (int i = 0; i < n; i++) {
        char name[17];
        snprintf(name, sizeof(name), "PROGRAM %03d", i + 1);
        add_mzf(f, name, 0x01, 1024, (i == n - 1) ? 0 : 1000);
    }
    const char *path = tmp_named(3, "many.tmz");
    save_file(f, path);
    return path;
}

/* Délky 300 bloků: otevření nepočítá žádnou délku (kromě otevřeného bloku), zásahy ve STOP doplní zbytek. */
void test_view_lengths_300_blocks(void)
{
    const char *path = make_many_tape(300);
    unsigned streams0 = cmttmz_test_get_length_streams();
    gint64 t0 = g_get_monotonic_time();
    open_tape(path);
    gint64 t1 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(streams0, cmttmz_test_get_length_streams(), "opening the tape generates no stream for lengths");
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    gint64 t2 = g_get_monotonic_time();
    int unknown = 0;
    for (const st_CMT_VIEW_BLOCK &b : v.blocks)
        if (b.length_s < 0.0) unknown++;
    fprintf(stderr, "lengths: 300 blocks x 1024 B, open %.1f ms, first fill %.1f ms, unknown lengths %d\n",
            (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, unknown);
    TEST_ASSERT_EQUAL_INT(300, v.block_count);
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[0].length_s > 0.0, "the opened block has its length for free");
    TEST_ASSERT_EQUAL_INT(299, unknown);
    TEST_ASSERT_TRUE(v.lengths_pending);
    TEST_ASSERT_TRUE(v.tape_total_s < 0.0);
    /* druhé plnění bez změny je jen kopie skalárů */
    gint64 t3 = g_get_monotonic_time();
    fill(&v);
    gint64 t4 = g_get_monotonic_time();
    fprintf(stderr, "lengths: fill without change %.3f ms\n", (t4 - t3) / 1000.0);

    st_LENGTHS_RUN run = fill_lengths(&v);
    fprintf(stderr, "lengths: filled in %d requests over %d frames, worst request %.1f ms\n", run.requests, run.frames,
            run.worst_us / 1000.0);
    TEST_ASSERT_EQUAL_UINT(streams0 + 299u, cmttmz_test_get_length_streams());
    TEST_ASSERT_TRUE(v.tape_total_s > 300.0);
    for (const st_CMT_VIEW_BLOCK &b : v.blocks) TEST_ASSERT_TRUE(b.length_s > 0.0);
}

/* Zásah délek se zařadí jen ve STOP: ne při PLAY, PLAY v pauze ani RECORD; ve STOP nejvýš jednou za 10 snímků. */
void test_view_lengths_request_only_in_stop(void)
{
    open_tape(make_many_tape(4));
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_TRUE(v.lengths_pending);

    device_state_lock();
    cmt_play();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_PLAY, v.state);
    for (int i = 0; i < 3 * (int)CMT_VIEW_LENGTHS_RETRY_FRAMES; i++)
        TEST_ASSERT_FALSE_MESSAGE(cmt_view_request_lengths(&v), "no length request during PLAY");
    device_state_lock();
    cmt_pause(1);
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_TRUE(v.paused);
    for (int i = 0; i < 3 * (int)CMT_VIEW_LENGTHS_RETRY_FRAMES; i++)
        TEST_ASSERT_FALSE_MESSAGE(cmt_view_request_lengths(&v), "no length request in a paused PLAY");
    device_state_lock();
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_RECORD, v.state);
    for (int i = 0; i < 3 * (int)CMT_VIEW_LENGTHS_RETRY_FRAMES; i++)
        TEST_ASSERT_FALSE_MESSAGE(cmt_view_request_lengths(&v), "no length request during RECORD");
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());

    /* STOP: první zásah hned (snímky se načítaly), další nejdřív po 10 snímcích a po změně čítače */
    device_state_lock();
    cmt_stop();
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_TRUE(v.lengths_pending);
    TEST_ASSERT_TRUE(cmt_view_request_lengths(&v));
    device_request_poll();
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res)) {}
    fill(&v);
    if (v.lengths_pending) {
        for (unsigned i = 1; i < CMT_VIEW_LENGTHS_INTERVAL_FRAMES; i++)
            TEST_ASSERT_FALSE_MESSAGE(cmt_view_request_lengths(&v), "at most one request per 10 frames");
        TEST_ASSERT_TRUE(cmt_view_request_lengths(&v));
        device_request_poll();
        while (device_request_take_result(&res)) {}
    }
}

/* Zásah odmítnutý emu vláknem (přehrávání vstupů začalo mezi zařazením a provedením) se zopakuje. */
void test_view_lengths_retry_after_refusal(void)
{
    open_tape(make_many_tape(3));
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_TRUE(v.lengths_pending);
    TEST_ASSERT_TRUE(cmt_view_request_lengths(&v));
    /* replay začne dřív, než emu vlákno zásah provede */
    g_replay_mode = REPLAY_MODE_PLAYING;
    device_request_poll();
    g_replay_mode = REPLAY_MODE_OFF;
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
    fill(&v);
    TEST_ASSERT_TRUE(v.lengths_pending);
    /* čítač změn se nepohnul: opakování až po CMT_VIEW_LENGTHS_RETRY_FRAMES snímcích */
    for (unsigned i = 1; i < CMT_VIEW_LENGTHS_RETRY_FRAMES; i++)
        TEST_ASSERT_FALSE(cmt_view_request_lengths(&v));
    TEST_ASSERT_TRUE_MESSAGE(cmt_view_request_lengths(&v), "a refused request is retried");
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    fill_lengths(&v);
}

/* Délky nemění stav stroje: otisk stroje (i s obsahem pásky) a revize stavu jsou před a po doplnění stejné. */
void test_view_lengths_keep_machine_hash(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_TRUE(v.lengths_pending);
    st_MACHINE_HASH h0, h1;
    machine_hash_compute(MACHINE_HASH_ALL_PARTS, MACHINE_HASH_FLAG_INCLUDE_MEDIA, &h0);
    uint64_t rev0 = g_state_rev;
    fill_lengths(&v);
    machine_hash_compute(MACHINE_HASH_ALL_PARTS, MACHINE_HASH_FLAG_INCLUDE_MEDIA, &h1);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(h0.total, h1.total, "block lengths do not change the machine hash");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(rev0, g_state_rev, "length requests do not bump the state revision");
}

/* ================================================================
 * Levná délka bloku proti délce streamu
 * ================================================================ */

/** @brief Přidá do souboru blok s danými surovými daty (kopie). */
static void add_raw_block(st_TZX_FILE *f, uint8_t id, const uint8_t *data, uint32_t len)
{
    st_TZX_BLOCK b;
    b.id = id;
    b.length = len;
    b.data = (uint8_t *)malloc(len ? len : 1);
    memcpy(b.data, data, len);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Zapíše 16bitové číslo LE. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/** @brief Zapíše 24bitové číslo LE. */
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }

/**
 * @brief Ověří, že levná délka bloku je délka streamu do 1 výstupního vzorku.
 * @param id Blok. @param what Popis pro hlášení.
 */
static void expect_cheap_equals_stream(int id, const char *what)
{
    int ck = -2, sk = -2;
    double cs = -1.0, ss = -1.0;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_length_paths(id, &ck, &cs, &sk, &ss));
    device_state_unlock();
    fprintf(stderr, "lengths: block %d (%s): cheap %.9f s, stream %.9f s, diff %.3f samples\n", id, what, cs, ss,
            (cs - ss) * (double)GDGCLK_BASE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ck, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sk, what);
    TEST_ASSERT_TRUE_MESSAGE(ss > 0.0, what);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0 / (double)GDGCLK_BASE, ss, cs, what);
}

/* Bloky TZX 0x10 až 0x14, 0x18 a 0x20: levná délka (bez streamu) = délka streamu do 1 vzorku. */
void test_lengths_cheap_tzx_blocks(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    uint8_t buf[0x12 + 64];
    uint8_t data[40];
    for (int i = 0; i < 40; i++) data[i] = (uint8_t)(i * 73 + 5);

    /* 0x10: hlavička ZX (flag 00h = dlouhý pilot), pauza 1000 ms */
    put16(buf, 1000);
    put16(buf + 2, 19);
    memcpy(buf + 4, data, 19);
    buf[4] = 0x00;
    add_raw_block(f, TZX_BLOCK_ID_STANDARD_SPEED, buf, 4 + 19);
    /* 0x11: vlastní časování, 5 bitů v posledním bajtu, pauza 333 ms */
    put16(buf + 0x00, 2000);
    put16(buf + 0x02, 600);
    put16(buf + 0x04, 700);
    put16(buf + 0x06, 811);
    put16(buf + 0x08, 1633);
    put16(buf + 0x0A, 1234);
    buf[0x0C] = 5;
    put16(buf + 0x0D, 333);
    put24(buf + 0x0F, 40);
    memcpy(buf + 0x12, data, 40);
    add_raw_block(f, TZX_BLOCK_ID_TURBO_SPEED, buf, 0x12 + 40);
    /* 0x12: 777 pulzů po 1111 T */
    put16(buf, 1111);
    put16(buf + 2, 777);
    add_raw_block(f, TZX_BLOCK_ID_PURE_TONE, buf, 4);
    /* 0x13: 5 pulzů různé délky, jeden 0 T (generátor dá 1 vzorek) */
    buf[0] = 5;
    put16(buf + 1, 100);
    put16(buf + 3, 0);
    put16(buf + 5, 3333);
    put16(buf + 7, 65535);
    put16(buf + 9, 7);
    add_raw_block(f, TZX_BLOCK_ID_PULSE_SEQUENCE, buf, 11);
    /* 0x14: 3 bity v posledním bajtu, pauza 17 ms */
    put16(buf + 0x00, 900);
    put16(buf + 0x02, 1777);
    buf[0x04] = 3;
    put16(buf + 0x05, 17);
    put24(buf + 0x07, 33);
    memcpy(buf + 0x0A, data, 33);
    add_raw_block(f, TZX_BLOCK_ID_PURE_DATA, buf, 0x0A + 33);
    /* 0x18: 44 100 Hz (poměr k GDGCLK_BASE není celý), délky přes 255 (32bitový únik), pauza 250 ms */
    {
        uint32_t p[600];
        for (int i = 0; i < 600; i++) p[i] = (uint32_t)(7 + (i * 37) % 300);
        st_TZX_BLOCK b;
        TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(44100, 250, p, 600, &b));
        TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
    }
    /* 0x20: pauza 1500 ms a pauza 0 (bez signálu) */
    put16(buf, 1500);
    add_raw_block(f, TZX_BLOCK_ID_PAUSE, buf, 2);
    put16(buf, 0);
    add_raw_block(f, TZX_BLOCK_ID_PAUSE, buf, 2);
    const char *path = tmp_named(3, "blocks.tmz");
    save_file(f, path);
    open_tape(path);

    const char *what[7] = {"0x10", "0x11", "0x12", "0x13", "0x14", "0x18 44100 Hz", "0x20"};
    for (int i = 0; i < 7; i++) expect_cheap_equals_stream(i, what[i]);
    int ck = -2, sk = -2;
    double cs = -1.0, ss = -1.0;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_length_paths(7, &ck, &cs, &sk, &ss));
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ck, "pause 0 has no signal (cheap)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sk, "pause 0 has no signal (stream)");

    /* zásah STOP spočítá všechno bez jediného streamu */
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    unsigned streams = cmttmz_test_get_length_streams();
    fill_lengths(&v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(streams, cmttmz_test_get_length_streams(), "TZX blocks need no stream for their length");
    TEST_ASSERT_TRUE(v.tape_total_s > 0.0);
}

/* Surový blok 0x18 ze záznamu: délka je známá hned po připojení (průchod dat), shodná se streamem. */
void test_lengths_cheap_recorded_raw_block(void)
{
    open_tape(make_two_block_mzt());
    /* jako cmttmz_rec_build_raw(): frekvence GDGCLK_BASE / 2, pauza 0, první pulz 1 vzorek */
    uint32_t n = 20000;
    uint32_t *p = (uint32_t *)malloc(n * sizeof(uint32_t));
    p[0] = 1;
    for (uint32_t i = 1; i < n; i++) p[i] = 3000u + (i * 131u) % 9000u;
    p[n / 2] = 300000u; /* dlouhý pulz přes 32bitový únik */
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(GDGCLK_BASE / 2, 0, p, n, &b));
    free(p);
    /* surový blok: záznam rychlosti nulový (ORIGIN_NONE), jako cmt_record_finish() */
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    unsigned streams = cmttmz_test_get_length_streams();
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_append_blocks(&b, &sp, 1));
    device_state_unlock();
    free(b.data);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(streams, cmttmz_test_get_length_streams(), "appending a raw block generates no stream");
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(3, v.block_count);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[2].kind);
    TEST_ASSERT_TRUE_MESSAGE(v.blocks[2].length_s > 0.0, "the recorded raw block has its length right after appending");
    expect_cheap_equals_stream(2, "0x18 recorded");
    /* délka v kopii (z připojení) je táž levná délka */
    int ck = -2, sk = -2;
    double cs = -1.0, ss = -1.0;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_length_paths(2, &ck, &cs, &sk, &ss));
    device_state_unlock();
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, cs, v.blocks[2].length_s);
}

/* Blok 0x18 z importu WAV (44 100 Hz): levná délka = délka streamu do 1 vzorku. */
void test_lengths_cheap_wav_block(void)
{
    const uint32_t rate = 44100, frames = rate * 2;
    const uint32_t size = 44 + frames;
    uint8_t *w = (uint8_t *)calloc(1, size);
    TEST_ASSERT_NOT_NULL(w);
    uint32_t v32;
    uint16_t v16;
    memcpy(w, "RIFF", 4);
    v32 = size - 8; memcpy(w + 4, &v32, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    v32 = 16; memcpy(w + 16, &v32, 4);
    v16 = 1; memcpy(w + 20, &v16, 2);
    memcpy(w + 22, &v16, 2);
    memcpy(w + 24, &rate, 4);
    memcpy(w + 28, &rate, 4);
    memcpy(w + 32, &v16, 2);
    v16 = 8; memcpy(w + 34, &v16, 2);
    memcpy(w + 36, "data", 4);
    v32 = frames; memcpy(w + 40, &v32, 4);
    /* nepravidelné půlvlny 3 až 29 vzorků */
    uint32_t run = 0, len = 5;
    int hi = 1;
    for (uint32_t i = 0; i < frames; i++) {
        w[44 + i] = hi ? 0xE0 : 0x20;
        if (++run >= len) {
            run = 0;
            hi = !hi;
            len = 3 + (i * 7) % 27;
        }
    }
    const char *path = tmp_named(3, "signal.wav");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)w, size, NULL));
    free(w);
    open_tape(path);
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(1, v.block_count);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[0].kind);
    expect_cheap_equals_stream(0, "0x18 WAV import");
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 2.0, v.blocks[0].length_s);
}

/* Testovací stavy: obsah konceptů v2 a invarianty kopie. */
void test_view_fixtures(void)
{
    st_CMT_VIEW v;
    for (int f = 0; f < CMT_VIEW_FIXTURE_COUNT; f++) {
        cmt_view_make_fixture(&v, (en_CMT_VIEW_FIXTURE)f);
        TEST_ASSERT_EQUAL_INT((int)v.blocks.size(), v.block_count);
        if (!v.has_tape) {
            TEST_ASSERT_TRUE(v.blocks.empty());
            TEST_ASSERT_EQUAL_INT(-1, v.current_block);
        }
        if (v.state == CMT_VIEW_STATE_RECORD) TEST_ASSERT_EQUAL_INT(v.block_count, v.current_block);
        else TEST_ASSERT_TRUE(v.current_block >= -1 && v.current_block < v.block_count);
        TEST_ASSERT_TRUE(v.block_progress >= 0.0 && v.block_progress <= 1.0);
        for (int i = 0; i < v.block_count; i++) TEST_ASSERT_EQUAL_INT(i, v.blocks[(size_t)i].id);
    }
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    const char *names[5] = {"LOADER", "FLAPPY", "HISCORE", "Raw recording", "SAVED GAME"};
    const char *status[5] = {"OK", "OK", "Checksum error", "Unsaved", "Unsaved"};
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], v.blocks[(size_t)i].name.c_str());
        TEST_ASSERT_EQUAL_STRING(status[i], v.blocks[(size_t)i].status_text.c_str());
    }
    TEST_ASSERT_EQUAL_STRING("games.tmz", v.tape_name.c_str());
    TEST_ASSERT_TRUE(v.modified);
    TEST_ASSERT_EQUAL_INT(1, v.current_block);
    TEST_ASSERT_EQUAL_INT(3, cmt_view_fixture_selection(CMT_VIEW_FIXTURE_PLAYING_FULL));
    TEST_ASSERT_FLOAT_WITHIN(0.05, 65.8, v.tape_elapsed_s);
    TEST_ASSERT_FLOAT_WITHIN(1e-9, 0.4, v.block_progress);
    TEST_ASSERT_EQUAL_UINT32(8860800, v.blocks[3].sample_rate);
    TEST_ASSERT_EQUAL_UINT32(61230, v.blocks[3].pulse_count);
    TEST_ASSERT_EQUAL_STRING("01 machine code", v.blocks[0].type_text.c_str());
    TEST_ASSERT_EQUAL_STRING("03 data", v.blocks[2].type_text.c_str());
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    TEST_ASSERT_FLOAT_WITHIN(1e-9, 12.4, v.rec_time_s);
    TEST_ASSERT_EQUAL_UINT32(1234, v.rec_pulses);
    TEST_ASSERT_EQUAL_INT(6, v.current_block + 1);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_WAITING_MOTOR);
    TEST_ASSERT_TRUE(v.paused && v.motor_known && !v.motor_on);
    TEST_ASSERT_FLOAT_WITHIN(1e-9, 21.0, v.tape_elapsed_s);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_DIRECT);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);
    TEST_ASSERT_TRUE(v.direct_block_loadable);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_EMPTY_TAPE);
    TEST_ASSERT_TRUE(v.has_tape && v.blocks.empty() && v.tape_name.empty());
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_LONG_TEXTS);
    TEST_ASSERT_EQUAL_INT(200, (int)v.tape_name.size());
    TEST_ASSERT_EQUAL_INT(16, (int)v.blocks[0].name.size());
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    TEST_ASSERT_EQUAL_INT(300, v.block_count);
}

/** Jazyk a měřítko snímků (do jmen souborů): "<jazyk>-<měřítko v %>". */
static char s_shot_tag[64] = "en-100";

/** Měřítko snímků (MZ_CMT_SHOT_SCALE). */
static float s_shot_scale = 1.0f;

/**
 * @brief Okno Virtual CMT nad skutečným jádrem (imgui_cmt_window(): plnění kopie a kreslení) bez pásky, ve STOP a při PLAY.
 *
 * Totéž volání jako main_window.cpp (pod device_state_lock()); kontroluje,
 * že okno vznikne se stabilním ID a že se okno se skutečnou páskou kreslí.
 */
void test_window_draw_smoke(void)
{
    bool open = true;
    auto frame = [&]() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        device_state_lock();
        imgui_cmt_window(&open);
        device_state_unlock();
        ImGui::Render();
    };
    for (int i = 0; i < 3; i++) frame();
    TEST_ASSERT_NOT_NULL(ImGui::FindWindowByName("###virtual_cmt"));
    open_games_tape();
    for (int i = 0; i < 3; i++) frame();
    device_state_lock();
    cmt_play();
    device_state_unlock();
    advance_ticks(GDGCLK_BASE / 2);
    for (int i = 0; i < 3; i++) frame();
    TEST_ASSERT_TRUE(open);
    TEST_ASSERT_TRUE(ImGui::GetFrameCount() >= 9);
}


/* ================================================================
 * Okno Virtual CMT: deck, rozvržení, geometrie prvků
 * ================================================================ */

/** @brief Monospace písmo testu (Cousine), vrací ho myimgui_get_monospace_font(). */
static ImFont *s_mono = NULL;
/** @brief Písmo aplikace (DroidSans 28 px), je-li dostupné. */
static ImFont *s_app_font = NULL;

/* Stub: okno bere velké počítadlo z monospace písma aplikace. */
ImFont *myimgui_get_monospace_font(void) { return s_mono; }

/** @brief Jazyk běhu testů (MZ_CMT_TEST_LANG, registrace ui-cmt-window-de); NULL = angličtina. */
static const char *s_test_lang = NULL;

/** @brief Stabilní ID nového okna (SPEC: ###virtual_cmt). */
#define W2_ID "###virtual_cmt"

/** @brief Počet sloupců tabulky bloků (en_CW_COL v cmt_window.cpp). */
#define W2_TEST_COLS 10

/**
 * @brief Načte písma aplikace (DroidSans 28 px a Cousine 28 px) jednou.
 *
 * Bez souborů (MZTEST_FONT_DIR prázdné) zůstane výchozí písmo ImGui; testy
 * geometrie pak běží s ním (měří totéž, jen jiné rozměry).
 */
static void w2_fonts(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!MZTEST_FONT_DIR[0]) return;
    ImGuiIO &io = ImGui::GetIO();
    char *path = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        static const ImWchar i18n_ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
        s_app_font = io.Fonts->AddFontFromFileTTF(path, 28.0f, NULL, i18n_ranges);
        if (s_app_font) io.FontDefault = s_app_font;
    }
    g_free(path);
    path = g_build_filename(MZTEST_FONT_DIR, "Cousine-Regular.ttf", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) s_mono = io.Fonts->AddFontFromFileTTF(path, 28.0f);
    g_free(path);
}

/**
 * @brief Nastaví styl ImGui na tmavé téma s měřítkem (jako režim snímků).
 * @param scale Měřítko písma a rozměrů stylu (1.0, 1.75).
 */
static void w2_apply_scale(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

/** @brief Poloha okna Virtual CMT v w2_frame() (výchozí levý horní roh; test středu modálu ji mění). */
static ImVec2 s_w2_pos(0.0f, 0.0f);
/** @brief Pevný krok času snímku w2_frame() v s (0 = čas backendu). */
static float s_w2_dt = 0.0f;
/** @brief w2_frame() kreslí i dialogy automatu pásky (imgui_cmt_tape_dialogs()). */
static bool s_w2_dialogs = false;

/**
 * @brief Jeden snímek nového okna nad danou kopií stavu, okno v levém horním rohu (s_w2_pos).
 * @param v Kopie stavu. @param render true = vykreslit do GL (snímky), jinak jen ImGui::Render().
 */
static void w2_frame(st_CMT_VIEW *v, bool render = false)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    /* pevný krok času (automatický posun při tažení počítá s DeltaTime; backend dává skutečný čas) */
    if (s_w2_dt > 0.0f) ImGui::GetIO().DeltaTime = s_w2_dt;
    ImGui::NewFrame();
    if (ImGui::FindWindowByName(W2_ID)) ImGui::SetWindowPos(W2_ID, s_w2_pos, ImGuiCond_Always);
    bool open = true;
    g_gui->showVirtualCmtWindow = true;
    device_state_lock();
    imgui_cmt_window_draw(v, &open);
    /* dialogy automatu pásky (neuložené změny) jako v main_window.cpp, jen když je test chce */
    if (s_w2_dialogs) imgui_cmt_tape_dialogs();
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

/** @brief Prvek z posledního snímku podle jména; NULL = nebyl vykreslen. */
static const st_CMT_WINDOW_TEST_ITEM *w2_item(const char *name)
{
    for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items())
        if (it.name == name) return &it;
    return NULL;
}

/** @brief Text prvku z posledního snímku ("" = prvek nebyl vykreslen). */
static std::string w2_text(const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item(name);
    return it ? it->text : std::string();
}

/**
 * @brief Kontrola geometrie posledního snímku: žádný prvek mimo okno ani mimo ořez, žádné dva se nepřekrývají.
 *
 * Navíc: žádný text širší než jeho ořez a žádný napůl viditelný
 * řádek posouvané oblasti (tabulka bloků, obsah vlastností): prvek ve
 * svisle posouvané oblasti je buď celý uvnitř ořezu, nebo celý mimo.
 *
 * @param ctx Popis varianty pro hlášení chyby.
 * @return Počet nalezených vad (každá se vypíše na stderr).
 */
static int w2_geometry_faults(const char *ctx)
{
    const float tol = 0.5f;
    int faults = 0;
    ImVec2 imin, imax;
    imgui_cmt_window_test_inner_rect(&imin, &imax);
    const std::vector<st_CMT_WINDOW_TEST_ITEM> &items = imgui_cmt_window_test_items();
    if (items.empty()) {
        fprintf(stderr, "%s: no items recorded\n", ctx);
        return 1;
    }
    /* Prvky ve svisle posouvané oblasti (řádky tabulky, obsah vlastností) smí
     * svisle přesahovat ořez (řádek napůl odrolovaný); vodorovně ne. Pro
     * kontrolu překryvu se ořežou na viditelnou část. Položky popupu leží
     * ve vlastním okně: jen ořez popupu, s prvky okna se neporovnávají. */
    std::vector<st_CMT_WINDOW_TEST_ITEM> vis(items);
    for (st_CMT_WINDOW_TEST_ITEM &it : vis) {
        if (!it.scroll_y) continue;
        it.min.y = ImMax(it.min.y, it.clip_min.y);
        it.max.y = ImMin(it.max.y, it.clip_max.y);
    }
    for (const st_CMT_WINDOW_TEST_ITEM &it : items) {
        if (it.max.x - it.min.x < tol || it.max.y - it.min.y < tol) continue;
        /* napůl viditelný řádek posouvané oblasti (useknutý řádek tabulky nebo vlastností);
         * 1,5 px: horní pixel řádku tabulky leží pod spodní čarou hlavičky (ImGui ořezává
         * tělo od čáry + 1 px), u 125 % navíc půlpixelové souřadnice */
        if (it.scroll_y && !it.popup) {
            const float row_tol = 1.5f;
            float vis = ImMin(it.max.y, it.clip_max.y) - ImMax(it.min.y, it.clip_min.y);
            if (vis > row_tol && vis < (it.max.y - it.min.y) - row_tol) {
                fprintf(stderr, "%s: item '%s' (%.1f,%.1f)-(%.1f,%.1f) only partly visible in the scroll area %.1f-%.1f (half row)\n",
                        ctx, it.name.c_str(), it.min.x, it.min.y, it.max.x, it.max.y, it.clip_min.y, it.clip_max.y);
                faults++;
            }
        }
        /* text širší než jeho ořez (např. věta prázdné pásky) */
        if (!it.container && !it.text.empty() && (it.max.x - it.min.x) > (it.clip_max.x - it.clip_min.x) + tol) {
            fprintf(stderr, "%s: item '%s' is %.1f px wide, its clip rect only %.1f px\n", ctx, it.name.c_str(),
                    it.max.x - it.min.x, it.clip_max.x - it.clip_min.x);
            faults++;
        }
        /* Selectable řádku přes všechny sloupce kreslí ImGui pod ořezem celé tabulky */
        if (it.container && it.scroll_y) continue;
        bool vert_ok = it.scroll_y || (it.min.y >= it.clip_min.y - tol && it.max.y <= it.clip_max.y + tol);
        bool in_clip = it.min.x >= it.clip_min.x - tol && it.max.x <= it.clip_max.x + tol && vert_ok;
        bool in_win = it.popup || it.scroll_y
                   || (it.min.x >= imin.x - tol && it.min.y >= imin.y - tol && it.max.x <= imax.x + tol && it.max.y <= imax.y + tol);
        if (it.scroll_y && !it.popup) in_win = it.min.x >= imin.x - tol && it.max.x <= imax.x + tol;
        if (!in_clip || !in_win) {
            fprintf(stderr, "%s: item '%s' (%.1f,%.1f)-(%.1f,%.1f) outside %s (%.1f,%.1f)-(%.1f,%.1f)\n", ctx, it.name.c_str(),
                    it.min.x, it.min.y, it.max.x, it.max.y, in_clip ? "window" : "clip rect",
                    in_clip ? imin.x : it.clip_min.x, in_clip ? imin.y : it.clip_min.y,
                    in_clip ? imax.x : it.clip_max.x, in_clip ? imax.y : it.clip_max.y);
            faults++;
        }
    }
    for (size_t i = 0; i < vis.size(); i++) {
        const st_CMT_WINDOW_TEST_ITEM &a = vis[i];
        if (a.container || a.max.y - a.min.y < tol) continue;
        for (size_t j = i + 1; j < vis.size(); j++) {
            const st_CMT_WINDOW_TEST_ITEM &b = vis[j];
            if (b.container || b.max.y - b.min.y < tol || a.popup != b.popup) continue;
            float ox = ImMin(a.max.x, b.max.x) - ImMax(a.min.x, b.min.x);
            float oy = ImMin(a.max.y, b.max.y) - ImMax(a.min.y, b.min.y);
            if (ox > tol && oy > tol) {
                fprintf(stderr, "%s: items '%s' and '%s' overlap (%.1f x %.1f px)\n", ctx, a.name.c_str(), b.name.c_str(), ox, oy);
                faults++;
            }
        }
    }
    return faults;
}

/** @brief Testovací stav se jménem pro hlášení. */
typedef struct {
    en_CMT_VIEW_FIXTURE f;
    const char *name;
} st_W2_FIXTURE;

static const st_W2_FIXTURE s_w2_fixtures[] = {
    {CMT_VIEW_FIXTURE_PLAYING_COMPACT, "playing"},
    {CMT_VIEW_FIXTURE_PLAYING_FULL, "playing-full"},
    {CMT_VIEW_FIXTURE_NO_TAPE, "notape"},
    {CMT_VIEW_FIXTURE_EMPTY_TAPE, "empty"},
    {CMT_VIEW_FIXTURE_RECORDING, "recording"},
    {CMT_VIEW_FIXTURE_WAITING_MOTOR, "waiting"},
    {CMT_VIEW_FIXTURE_DIRECT, "direct"},
    {CMT_VIEW_FIXTURE_LONG_TEXTS, "longtexts"},
    {CMT_VIEW_FIXTURE_MANY_BLOCKS, "manyblocks"},
    {CMT_VIEW_FIXTURE_CONTROL_BLOCKS, "controlblocks"},
    {CMT_VIEW_FIXTURE_PAUSED, "paused"},
};

/**
 * @brief Geometrie okna pro všechny testovací stavy, obě rozvržení, měřítka 1,0, 1,25 a 1,75 a texty běžné i o 40 % delší.
 *
 * Okno má výchozí velikost rozvržení (po imgui_cmt_window_test_reset());
 * minimální velikost se počítá z textů, takže ani prodloužené texty se
 * nesmí ořezat ani překrýt. Kontrola zahrnuje i napůl
 * viditelné řádky tabulky a vlastností a texty širší než jejich ořez.
 */
void test_w2_deck_geometry(void)
{
    w2_fonts();
    const float scales[3] = {1.0f, 1.25f, 1.75f};
    int faults = 0;
    st_CMT_VIEW v;
    for (int si = 0; si < 3; si++) {
        w2_apply_scale(scales[si]);
        for (int stretch = 0; stretch < 2; stretch++) {
            imgui_cmt_window_test_set_stretch(stretch != 0);
            for (int lay = 0; lay < 2; lay++) {
                for (const st_W2_FIXTURE &fx : s_w2_fixtures) {
                    imgui_cmt_window_test_reset();
                    imgui_cmt_window_set_layout((en_CMT_WINDOW_LAYOUT)lay);
                    cmt_view_make_fixture(&v, fx.f);
                    for (int i = 0; i < 3; i++) w2_frame(&v);
                    char ctx[160];
                    snprintf(ctx, sizeof(ctx), "%s scale %.2f %s %s %s", s_test_lang ? s_test_lang : "en", scales[si],
                             stretch ? "stretch+40%" : "plain", lay ? "full" : "compact", fx.name);
                    faults += w2_geometry_faults(ctx);
                    /* tabulka bez vodorovného posuvníku při výchozí velikosti Full */
                    if (lay && imgui_cmt_window_test_table_scroll_x()) {
                        fprintf(stderr, "%s: table has a horizontal scrollbar\n", ctx);
                        faults++;
                    }
                }
            }
        }
    }
    imgui_cmt_window_test_set_stretch(false);
    w2_apply_scale(1.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, faults, "deck items clipped or overlapping (details on stderr)");
}

/** @brief Velikost okna z posledního snímku. */
static ImVec2 w2_window_size(void)
{
    ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
    return w ? w->Size : ImVec2(0.0f, 0.0f);
}

/** @brief Kopie prvků decku z posledního snímku (pro porovnání mezi rozvrženími). */
static std::vector<st_CMT_WINDOW_TEST_ITEM> w2_deck_items(void)
{
    std::vector<st_CMT_WINDOW_TEST_ITEM> out;
    for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items())
        if (it.deck) out.push_back(it);
    return out;
}

/**
 * @brief Compact -> Full -> Compact vrátí stejnou velikost okna; deck se mezi rozvrženími neposune.
 *
 * Každé rozvržení má vlastní pamatovanou velikost (i velikost změněnou
 * uživatelem). Deck: stejné prvky, stejná svislá poloha každého prvku
 * vůči oknu a stejná vodorovná vzdálenost od kotvy (levý okraj, nebo
 * u prvků ukotvených vpravo pravý okraj okna); Full je širší, takže prvky
 * vpravo se posunou s pravým okrajem.
 *
 * @param scale Měřítko písma a stylu. @param fixture Testovací stav.
 */
static void w2_layout_persistence_case(float scale, en_CMT_VIEW_FIXTURE fixture)
{
    w2_fonts();
    w2_apply_scale(scale);
    imgui_cmt_window_test_reset();
    st_CMT_VIEW v;
    cmt_view_make_fixture(&v, fixture);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 c1 = w2_window_size();
    TEST_ASSERT_TRUE(c1.x > 0.0f && c1.y > 0.0f);
    /* uživatel okno zvětší */
    ImGui::SetWindowSize(W2_ID, ImVec2(c1.x + 40.0f, c1.y + 24.0f), ImGuiCond_Always);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImVec2 c2 = w2_window_size();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c1.x + 40.0f, c2.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c1.y + 24.0f, c2.y);
    std::vector<st_CMT_WINDOW_TEST_ITEM> deck_c = w2_deck_items();
    ImVec2 c_size = c2;

    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 f1 = w2_window_size();
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_FULL, imgui_cmt_window_get_layout());
    TEST_ASSERT_TRUE(f1.y > c_size.y + ImGui::GetFrameHeight() * 4.0f);
    std::vector<st_CMT_WINDOW_TEST_ITEM> deck_f = w2_deck_items();

    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 c3 = w2_window_size();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c_size.x, c3.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c_size.y, c3.y);

    /* Full si pamatuje svou velikost */
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 f2 = w2_window_size();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, f1.x, f2.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, f1.y, f2.y);

    /* deck: stejné prvky na stejném místě vůči kotvě */
    TEST_ASSERT_EQUAL_INT((int)deck_c.size(), (int)deck_f.size());
    TEST_ASSERT_TRUE(deck_c.size() > 20);
    for (size_t i = 0; i < deck_c.size(); i++) {
        const st_CMT_WINDOW_TEST_ITEM &a = deck_c[i];
        const st_CMT_WINDOW_TEST_ITEM &b = deck_f[i];
        TEST_ASSERT_EQUAL_STRING(a.name.c_str(), b.name.c_str());
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, a.min.y, b.min.y, a.name.c_str());
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, a.max.y, b.max.y, a.name.c_str());
        if (a.right) {
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, c_size.x - a.min.x, f1.x - b.min.x, a.name.c_str());
        } else {
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, a.min.x, b.min.x, a.name.c_str());
        }
    }
    imgui_cmt_window_test_reset();
}

/**
 * @brief Persistence rozvržení a "deck se neposune" při měřítku 1,0 i 1,75 a ve třech stavech.
 */
void test_w2_layout_persistence(void)
{
    const float scales[2] = {1.0f, 1.75f};
    const en_CMT_VIEW_FIXTURE fx[3] = {CMT_VIEW_FIXTURE_PLAYING_COMPACT, CMT_VIEW_FIXTURE_RECORDING, CMT_VIEW_FIXTURE_WAITING_MOTOR};
    for (float sc : scales)
        for (en_CMT_VIEW_FIXTURE f : fx) w2_layout_persistence_case(sc, f);
    w2_apply_scale(1.0f);
}

/** @brief Klik levým tlačítkem myši doprostřed prvku (dva snímky: stisk, uvolnění). */
static void w2_click(st_CMT_VIEW *v, const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    w2_frame(v);
    io.AddMouseButtonEvent(0, true);
    w2_frame(v);
    io.AddMouseButtonEvent(0, false);
    w2_frame(v);
    io.AddMousePosEvent(-1.0f, -1.0f);
    w2_frame(v);
}

/* Tlačítko Blocks přepíná Compact / Full; bez pásky je zakázané. */
void test_w2_blocks_button_toggles_layout(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    st_CMT_VIEW v;
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_COMPACT, imgui_cmt_window_get_layout());
    w2_click(&v, "btn_blocks");
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_FULL, imgui_cmt_window_get_layout());
    TEST_ASSERT_NOT_NULL(w2_item("table"));
    w2_click(&v, "btn_blocks");
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_COMPACT, imgui_cmt_window_get_layout());
    TEST_ASSERT_NULL(w2_item("table"));
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_NO_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("btn_blocks")->disabled);
    imgui_cmt_window_test_reset();
}

/* Titulek: jméno pásky, hvězdička jen u Modified, "(new tape)", stabilní ID ###virtual_cmt;
 * jméno s "##" a "###" titulek nezkrátí. */
void test_w2_title_and_switch(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    st_CMT_VIEW v;
    struct { en_CMT_VIEW_FIXTURE f; const char *title; } cases[] = {
        {CMT_VIEW_FIXTURE_PLAYING_COMPACT, "Virtual CMT - games.tmz *" W2_ID},
        {CMT_VIEW_FIXTURE_WAITING_MOTOR, "Virtual CMT - games.tmz" W2_ID},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, "Virtual CMT - (new tape)" W2_ID},
        {CMT_VIEW_FIXTURE_NO_TAPE, "Virtual CMT" W2_ID},
    };
    ImGuiWindow *first = NULL;
    for (auto &c : cases) {
        cmt_view_make_fixture(&v, c.f);
        for (int i = 0; i < 2; i++) w2_frame(&v);
        ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
        TEST_ASSERT_NOT_NULL(w);
        if (!first) first = w;
        TEST_ASSERT_TRUE_MESSAGE(w == first, "window ID changes with the title");
        TEST_ASSERT_EQUAL_STRING(c.title, imgui_cmt_window_test_title());
    }

    /* jméno s "##" a "###" titulek nezkrátí ani nezmění ID;
     * za každým '#' je mezera nulové šířky (U+200B) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    v.tape_name = "a##b###c.tmz";
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
    TEST_ASSERT_TRUE_MESSAGE(w == first, "a name with ## does not change the window ID");
#define ZW "\xE2\x80\x8B"
    const char *want = "Virtual CMT - a#" ZW "#" ZW "b#" ZW "#" ZW "#" ZW "c.tmz *" W2_ID;
#undef ZW
    const char *title = imgui_cmt_window_test_title();
    TEST_ASSERT_EQUAL_STRING(want, title);
    /* viditelná část končí až před koncovým ###virtual_cmt */
    const char *shown_end = ImGui::FindRenderedTextEnd(title);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(strlen(title) - strlen(W2_ID)), (int)(shown_end - title), "the whole name is shown in the title");
    TEST_ASSERT_TRUE(w->ID == first->ID);
    if (s_app_font) {
        /* písmo aplikace: U+200B má nulovou šířku, titulek je stejně široký jako jméno bez úprav */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        float esc = ImGui::CalcTextSize(title, shown_end).x;
        float plain = ImGui::CalcTextSize("Virtual CMT - a##b###c.tmz *", NULL, false).x;
        ImGui::EndFrame();
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, plain, esc, "the zero-width space takes no room with the application font");
    }
    imgui_cmt_window_test_reset();
}

/**
 * @brief Texty a stavy decku podle konceptů v2 a tabulek SPEC v2 (stav x motor, režim x prvek).
 *
 * Počítadlo velkým řezem (2,2x základní velikost písma), Play / Pause podle
 * akce, motor jen při známém stavu, Direct bez transportu, záznam bez
 * vymyšlené délky, neznámé délky -> čas v bloku s popiskem Block time.
 */
void test_w2_deck_texts_and_states(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    st_CMT_VIEW v;

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("games.tmz", w2_text("tape_name").c_str());
    TEST_ASSERT_EQUAL_STRING("Modified", w2_text("modified").c_str());
    TEST_ASSERT_EQUAL_STRING("01:05.8", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Elapsed", w2_text("counter_caption").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 2 / 5", w2_text("block_line").c_str());
    TEST_ASSERT_EQUAL_STRING("FLAPPY", w2_text("block_name").c_str());
    TEST_ASSERT_NOT_NULL(w2_item("progress"));
    TEST_ASSERT_EQUAL_STRING("Playing", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor on", w2_text("motor_text").c_str());
    TEST_ASSERT_EQUAL_STRING("pause", w2_text("btn_play").c_str());
    TEST_ASSERT_EQUAL_STRING("record", w2_text("btn_record").c_str());
    TEST_ASSERT_EQUAL_STRING("Signal speed", w2_text("speed_label").c_str());
    TEST_ASSERT_EQUAL_STRING("1:1", w2_text("speed_combo").c_str());
    TEST_ASSERT_EQUAL_STRING("Loading", w2_text("loading_label").c_str());
    /* skutečný formát jádra (cmt_tmz_record.c), bez oddělovače tisíců */
    TEST_ASSERT_EQUAL_STRING("Recorded \"SAVED GAME\", 4096 B", w2_text("message").c_str());
    TEST_ASSERT_FALSE(w2_item("btn_open")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_rewind")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_next")->disabled);
    {
        /* velké počítadlo: skutečný řez 2,2x základní velikosti (výška řádku = velikost písma) */
        const st_CMT_WINDOW_TEST_ITEM *c = w2_item("counter");
        TEST_ASSERT_FLOAT_WITHIN(1.0f, ImGui::GetFontSize() * 2.2f, c->max.y - c->min.y);
        /* tlačítka transportu: výška 1,6 x výška rámečku */
        const st_CMT_WINDOW_TEST_ITEM *b = w2_item("btn_play");
        TEST_ASSERT_FLOAT_WITHIN(1.0f, ImGui::GetFrameHeight() * 1.6f, b->max.y - b->min.y);
    }

    /* čekání na motor: ikona Pause (akce), Motor off */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_WAITING_MOTOR);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Waiting for motor", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor off", w2_text("motor_text").c_str());
    TEST_ASSERT_EQUAL_STRING("pause", w2_text("btn_play").c_str());
    TEST_ASSERT_EQUAL_STRING("00:21.0", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("", w2_text("modified").c_str());

    /* pauza uživatele (D-G2-3): Paused, ikona Play (akce pauzu zruší), motor podle jádra */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PAUSED);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Paused", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("play", w2_text("btn_play").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor on", w2_text("motor_text").c_str());
    v.motor_on = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Paused", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor off", w2_text("motor_text").c_str());
    TEST_ASSERT_EQUAL_STRING("play", w2_text("btn_play").c_str());

    /* Direct: počítadlo --:--.-, transport Play / Stop zakázaný, Record povolený (nahrávání
     * řídí motor programu i s CMT hackem), rychlost "Not used in Direct mode" */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_DIRECT);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("--:--.-", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Direct load", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Next LOAD takes this block", w2_text("direct_hint").c_str());
    TEST_ASSERT_NULL(w2_item("progress"));
    TEST_ASSERT_NULL(w2_item("motor_text"));
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_stop")->disabled);
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_record")->disabled, "Record is enabled in Direct mode");
    TEST_ASSERT_EQUAL_STRING("record", w2_text("btn_record").c_str());
    TEST_ASSERT_FALSE(w2_item("btn_rewind")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_prev")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_next")->disabled);
    TEST_ASSERT_EQUAL_STRING("Not used in Direct mode", w2_text("speed_text").c_str());
    TEST_ASSERT_NULL(w2_item("speed_combo"));
    /* blok bez MZF: varování */
    v.direct_block_loadable = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("This block cannot be loaded directly", w2_text("direct_hint").c_str());

    /* nahrávání v Direct: stav decku a čas záznamu jako mimo Direct (ne "Direct load") */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    v.mode = CMT_VIEW_MODE_DIRECT;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Recording", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("00:12.4", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Recording time", w2_text("counter_caption").c_str());
    TEST_ASSERT_EQUAL_STRING("record_active", w2_text("btn_record").c_str());
    TEST_ASSERT_FALSE(w2_item("btn_record")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_stop")->disabled);
    TEST_ASSERT_EQUAL_STRING("Not available while recording.", w2_item("btn_play")->reason.c_str());
    v.paused = true;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Waiting for motor", w2_text("status_text").c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_play")->disabled, "Play starts a recording waiting for the motor also in Direct");

    /* záznam: čas záznamu, pulzy místo průběhu, Record aktivní, Open / Eject a posun bloků zakázané */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("00:12.4", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Recording time", w2_text("counter_caption").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 6 / 6", w2_text("block_line").c_str());
    TEST_ASSERT_EQUAL_STRING("New recording", w2_text("block_name").c_str());
    TEST_ASSERT_EQUAL_STRING("1 234 pulses", w2_text("pulses").c_str());
    TEST_ASSERT_NULL(w2_item("progress"));
    TEST_ASSERT_EQUAL_STRING("Recording", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("record_active", w2_text("btn_record").c_str());
    TEST_ASSERT_FALSE(w2_item("btn_record")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_stop")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_rewind")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_prev")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_next")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_open")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_eject")->disabled);
    TEST_ASSERT_TRUE(w2_item("speed_combo")->disabled);

    /* bez pásky */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_NO_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("No tape", w2_text("no_tape").c_str());
    TEST_ASSERT_NOT_NULL(w2_item("btn_open_tape"));
    TEST_ASSERT_NOT_NULL(w2_item("btn_new_tape"));
    TEST_ASSERT_NULL(w2_item("btn_eject"));
    TEST_ASSERT_EQUAL_STRING("--:--.-", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("No block", w2_text("block_line").c_str());
    TEST_ASSERT_EQUAL_STRING("No tape", w2_text("status_text").c_str());
    TEST_ASSERT_NULL(w2_item("motor_text"));
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_rewind")->disabled);

    /* prázdná páska ve STOP: motor neznámý -> bez indikátoru */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_EMPTY_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("(new tape)", w2_text("tape_name").c_str());
    TEST_ASSERT_EQUAL_STRING("Block 0 / 0", w2_text("block_line").c_str());
    TEST_ASSERT_EQUAL_STRING("Stopped", w2_text("status_text").c_str());
    TEST_ASSERT_NULL(w2_item("motor_text"));
    TEST_ASSERT_EQUAL_STRING("00:00.0", w2_text("counter").c_str());
    TEST_ASSERT_FALSE(w2_item("speed_combo")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_record")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);

    /* délky předchozích bloků neznámé: čas v bloku s popiskem Block time; Remaining nejde */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    v.tape_elapsed_s = -1.0;
    v.tape_total_s = -1.0;
    v.block_pos_s = 12.34;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("00:12.3", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Block time", w2_text("counter_caption").c_str());
    TEST_ASSERT_TRUE(w2_item("counter_area")->disabled);

    /* Remaining: klik na počítadlo přepne na čas do konce pásky (224 - 65,8 = 158,2 s) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FALSE(w2_item("counter_area")->disabled);
    w2_click(&v, "counter_area");
    TEST_ASSERT_EQUAL_STRING("02:38.2", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Remaining", w2_text("counter_caption").c_str());
    w2_click(&v, "counter_area");
    TEST_ASSERT_EQUAL_STRING("01:05.8", w2_text("counter").c_str());
    imgui_cmt_window_test_reset();
}


/* ================================================================
 * Okno Virtual CMT: tabulka bloků, vlastnosti, kontextové menu
 * ================================================================ */

/** @brief Vyprázdní fronty požadavků (operace kazety replay_host_cmt() a zásahy device_request) bez provedení. */
static void w3_drain(void)
{
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    while (device_request_test_take_pending(NULL, NULL)) {}
    imgui_cmt_tape_test_take_last_export(NULL, NULL);
}

/**
 * @brief Najde v zařazených operacích kazety danou operaci.
 * @param op Operace. @param[out] arg Argument nalezené operace.
 * @return true = nalezena (fronta se vyprázdní).
 */
static bool w3_take_cmt_op(en_REPLAY_CMT_OP op, int32_t *arg)
{
    bool found = false;
    en_REPLAY_CMT_OP o;
    int32_t a;
    while (replay_test_take_ui_cmt(&o, &a)) {
        if (o == op && !found) {
            found = true;
            if (arg) *arg = a;
        }
    }
    return found;
}

/**
 * @brief Najde v zařazených zásazích zásah daného jména.
 * @param name Název zásahu. @param[out] args Argumenty nalezeného zásahu.
 * @return true = nalezen (fronta se vyprázdní).
 */
static bool w3_take_request(const char *name, st_DEVICE_REQ_ARGS *args)
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

/** @brief Myš na střed prvku a klik (bez odjezdu myši, menu zůstane otevřené). */
static void w3_click_at(st_CMT_VIEW *v, const char *name, int button)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    w2_frame(v);
    io.AddMouseButtonEvent(button, true);
    w2_frame(v);
    io.AddMouseButtonEvent(button, false);
    w2_frame(v);
    w2_frame(v);
}

/** @brief Dvojklik levým tlačítkem doprostřed prvku. */
static void w3_double_click(st_CMT_VIEW *v, const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    w2_frame(v);
    io.AddMouseButtonEvent(0, true);
    w2_frame(v);
    io.AddMouseButtonEvent(0, false);
    io.AddMouseButtonEvent(0, true);
    w2_frame(v);
    io.AddMouseButtonEvent(0, false);
    w2_frame(v);
    io.AddMousePosEvent(-1.0f, -1.0f);
    w2_frame(v);
}

/** @brief Zavře otevřený popup (Escape, myš pryč). */
static void w3_close_popups(st_CMT_VIEW *v)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    w2_frame(v);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    w2_frame(v);
    w2_frame(v);
}

/** @brief Připraví okno v rozvržení Full s testovacím stavem (výchozí velikost, bez výběru). */
static void w3_setup(st_CMT_VIEW *v, en_CMT_VIEW_FIXTURE f)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    /* jako aplikace (myimgui.cpp): klávesnice ovládá navigaci ImGui */
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_make_fixture(v, f);
    for (int i = 0; i < 3; i++) w2_frame(v);
    /* velikost konceptu cmt-full-v2 (30 x 23 výšek rámečku): výchozí Full je
     * nižší (displej 768 px, D-G2-24), testy tabulky počítají s 5 řádky */
    float F = ImGui::GetFrameHeight();
    ImGui::SetWindowSize(W2_ID, ImVec2(30.0f * F, 23.0f * F), ImGuiCond_Always);
    for (int i = 0; i < 2; i++) w2_frame(v);
    w3_drain();
}

/**
 * @brief Tabulka a vlastnosti podle cmt-full-v2: sloupce, texty řádků, stavy, značka pozice, vybraný řádek, vlastnosti surového bloku.
 *
 * Výchozí viditelné sloupce: značka, #, Name, Type, Length, Status (Size, Load,
 * Exec, Speed skryté); bez vodorovného posuvníku při výchozí velikosti Full.
 */
void test_w3_table_and_properties(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    /* vlastnosti jsou výchozí sbalené (D-G3-9); rozbalit */
    TEST_ASSERT_EQUAL_UINT(0u, imgui_cmt_window_test_props_open());
    w3_click_at(&v, "props_header", 0);
    TEST_ASSERT_EQUAL_UINT(1u, imgui_cmt_window_test_props_open());
    /* bez výběru: výběr se nevnucuje na hrající řádek, vlastnosti pásky */
    TEST_ASSERT_EQUAL_INT(-1, imgui_cmt_window_test_get_selection());
    TEST_ASSERT_EQUAL_STRING("Tape properties", w2_text("props_title").c_str());
    TEST_ASSERT_EQUAL_STRING("games.tmz", w2_text("props_subtitle").c_str());
    TEST_ASSERT_EQUAL_STRING("File", w2_text("prop_label_0").c_str());
    TEST_ASSERT_EQUAL_STRING("C:/Users/user/Tapes/games.tmz", w2_text("prop_value_0").c_str());
    TEST_ASSERT_EQUAL_STRING("5", w2_text("prop_value_1").c_str());
    TEST_ASSERT_EQUAL_STRING("3:44", w2_text("prop_value_2").c_str());
    TEST_ASSERT_EQUAL_STRING("TMZ", w2_text("prop_value_3").c_str());

    /* řádky tabulky se sbalenými vlastnostmi (rozbalené ubírají celé řádky tabulky) */
    imgui_cmt_window_test_set_props_open(0u);
    imgui_cmt_window_test_set_selection(cmt_view_fixture_selection(CMT_VIEW_FIXTURE_PLAYING_FULL), v.tape_serial);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const char *hdr[W2_TEST_COLS] = {"", "#", "Name", "Type", NULL, NULL, NULL, NULL, "Length", "Status"};
    for (int c = 0; c < W2_TEST_COLS; c++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "hdr_%d", c);
        if (hdr[c]) TEST_ASSERT_EQUAL_STRING_MESSAGE(hdr[c], w2_text(nm).c_str(), nm);
        else TEST_ASSERT_NULL_MESSAGE(w2_item(nm), nm);
    }
    const char *names[5] = {"LOADER", "FLAPPY", "HISCORE", "Raw recording", "SAVED GAME"};
    const char *types[5] = {"01 machine code", "01 machine code", "03 data", "-", "01 machine code"};
    const char *lens[5] = {"0:21", "1:52", "0:09", "0:44", "0:38"};
    const char *status[5] = {"OK", "OK", "Checksum error", "Unsaved", "Unsaved"};
    for (int i = 0; i < 5; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "num_%d", i);
        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        TEST_ASSERT_EQUAL_STRING(num, w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "name_%d", i);
        TEST_ASSERT_EQUAL_STRING(names[i], w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "type_%d", i);
        TEST_ASSERT_EQUAL_STRING(types[i], w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "len_%d", i);
        TEST_ASSERT_EQUAL_STRING(lens[i], w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "status_%d", i);
        TEST_ASSERT_EQUAL_STRING(status[i], w2_text(nm).c_str());
    }
    /* značka pozice v řádku 2 (FLAPPY), ne ve vybraném řádku 4 */
    const st_CMT_WINDOW_TEST_ITEM *mark = w2_item("mark");
    const st_CMT_WINDOW_TEST_ITEM *row1 = w2_item("row_1");
    TEST_ASSERT_NOT_NULL(mark);
    TEST_ASSERT_TRUE(mark->min.y >= row1->min.y - 1.0f && mark->max.y <= row1->max.y + 1.0f);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_table_scroll_x());
    imgui_cmt_window_test_set_props_open(1u);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    /* vlastnosti vybraného surového bloku: dva sloupce (Type, Length, Source | Sample rate, Pulse count) */
    TEST_ASSERT_EQUAL_STRING("Block properties", w2_text("props_title").c_str());
    TEST_ASSERT_EQUAL_STRING("Raw recording", w2_text("props_subtitle").c_str());
    const char *pl[5] = {"Type", "Length", "Source", "Sample rate", "Pulse count"};
    const char *pv[5] = {"Raw recording", "0:44", "Recorded", "8 860 800 Hz", "61 230"};
    for (int i = 0; i < 5; i++) {
        char nm[24];
        snprintf(nm, sizeof(nm), "prop_label_%d", i);
        TEST_ASSERT_EQUAL_STRING(pl[i], w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "prop_value_%d", i);
        TEST_ASSERT_EQUAL_STRING(pv[i], w2_text(nm).c_str());
    }
    TEST_ASSERT_TRUE(w2_item("prop_label_3")->min.x > w2_item("prop_value_0")->max.x);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, w2_item("prop_label_0")->min.y, w2_item("prop_label_3")->min.y);

    /* blok MZF: Long pulse / Short pulse (µs) jako "horní / dolní půlvlna", adresy 1200h */
    imgui_cmt_window_test_set_selection(1, v.tape_serial);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const char *ml[9] = {"Type", "Size", "Load address", "Exec address", "Speed", "Long pulse (µs)", "Short pulse (µs)", "Length", "Source"};
    const char *mv[9] = {"01 machine code", "12 288 B", "1200h", "1200h", "1:1", "470 / 494", "240 / 278", "1:52", "From tape file"};
    for (int i = 0; i < 9; i++) {
        char nm[24];
        snprintf(nm, sizeof(nm), "prop_label_%d", i);
        TEST_ASSERT_EQUAL_STRING(ml[i], w2_text(nm).c_str());
        snprintf(nm, sizeof(nm), "prop_value_%d", i);
        TEST_ASSERT_EQUAL_STRING(mv[i], w2_text(nm).c_str());
    }
    /* výchozí velikost Full: aspoň 3 celé řádky tabulky i s rozbalenými vlastnostmi (MANY_BLOCKS) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    /* jiná páska: tabulka se posune na aktuální blok 151 (značka pozice je vidět), výběr se nevnucuje */
    v.tape_serial = 2;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_NOT_NULL(w2_item("mark"));
    TEST_ASSERT_EQUAL_STRING("PROGRAM 151", w2_text("name_150").c_str());
    TEST_ASSERT_EQUAL_INT(-1, imgui_cmt_window_test_get_selection());
    imgui_cmt_window_test_set_selection(150, v.tape_serial);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    /* tělo tabulky (bez hlavičky) má výšku aspoň 3 řádků */
    const st_CMT_WINDOW_TEST_ITEM *tbl = w2_item("table");
    const st_CMT_WINDOW_TEST_ITEM *row = w2_item("row_150");
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_NOT_NULL(row);
    float row_h = ImGui::GetTextLineHeight() + ImGui::GetFontSize() / 7.0f;
    TEST_ASSERT_TRUE_MESSAGE((tbl->max.y - tbl->min.y) - row_h >= 3.0f * row_h - 1.0f, "at least 3 table rows visible");
    TEST_ASSERT_EQUAL_INT(1u, imgui_cmt_window_test_props_open());
    imgui_cmt_window_test_reset();
}

/* Prázdná páska: dvě věty uprostřed tabulky; záznam: dočasný řádek Recording... na konci. */
void test_w3_empty_and_recording_rows(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_EMPTY_TAPE);
    TEST_ASSERT_EQUAL_STRING("This tape is empty.", w2_text("empty_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Press Record to save a program from the computer, or open a tape file.", w2_text("empty_2").c_str());
    TEST_ASSERT_NULL(w2_item("row_0"));
    imgui_cmt_window_test_set_props_open(1u);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("(new tape)", w2_text("prop_value_0").c_str());
    TEST_ASSERT_EQUAL_STRING("0", w2_text("prop_value_1").c_str());
    TEST_ASSERT_EQUAL_STRING("-", w2_text("prop_value_3").c_str());

    imgui_cmt_window_test_set_props_open(0u);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NULL(w2_item("empty_1"));
    TEST_ASSERT_EQUAL_STRING("6", w2_text("rec_num").c_str());
    TEST_ASSERT_EQUAL_STRING("Recording...", w2_text("rec_name").c_str());
    TEST_ASSERT_EQUAL_STRING("0:12", w2_text("rec_len").c_str());
    TEST_ASSERT_NOT_NULL(w2_item("rec_mark"));
    TEST_ASSERT_NULL(w2_item("mark"));
    TEST_ASSERT_EQUAL_STRING("OK", w2_text("status_2").c_str());
    /* tabulka se při záznamu posune na řádek Recording... (za posledním blokem) */
    TEST_ASSERT_NOT_NULL(w2_item("name_4"));
    TEST_ASSERT_TRUE(w2_item("rec_name")->min.y > w2_item("name_4")->min.y);
    TEST_ASSERT_TRUE(w2_item("rec_name")->max.y <= w2_item("rec_name")->clip_max.y + 0.5f);
    imgui_cmt_window_test_reset();
}

/**
 * @brief Výběr řádku je stav okna: klik vybere, změna otevřeného bloku ho nechá, jiná páska ho zruší.
 *
 * Řídicí blok jde vybrat (vlastnosti), přehrát ne; popis a stavy podle
 * cmt_view (Loop start x3, Cannot play).
 */
void test_w3_selection_rules(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    w2_click(&v, "name_3");
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    /* klik jen vybírá, nic nezařadí */
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt(NULL, NULL));
    /* přehrávání přejde na další blok (open_serial roste): výběr zůstane */
    v.open_serial++;
    v.current_block = 2;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    /* jiná páska: výběr se zruší */
    v.tape_serial++;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(-1, imgui_cmt_window_test_get_selection());

    /* řídicí bloky */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    /* sbalené vlastnosti (výchozí): všech sedm řádků je vidět */
    TEST_ASSERT_EQUAL_UINT(0u, imgui_cmt_window_test_props_open());
    TEST_ASSERT_EQUAL_STRING("Loop start x3", w2_text("status_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Loop end", w2_text("status_3").c_str());
    TEST_ASSERT_EQUAL_STRING("Jump +2", w2_text("status_4").c_str());
    TEST_ASSERT_EQUAL_STRING("Cannot play", w2_text("status_5").c_str());
    TEST_ASSERT_EQUAL_STRING("Stop the tape", w2_text("status_6").c_str());
    TEST_ASSERT_EQUAL_STRING("-", w2_text("len_5").c_str());
    w2_click(&v, "name_1");
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_window_test_get_selection());
    w3_click_at(&v, "props_header", 0);
    TEST_ASSERT_EQUAL_STRING("Loop Start", w2_text("prop_value_0").c_str());
    TEST_ASSERT_EQUAL_STRING("Description", w2_text("prop_label_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Loop start x3", w2_text("prop_value_1").c_str());
    /* dvojklik na řídicí blok nic nepřehraje */
    w3_drain();
    w3_double_click(&v, "name_1");
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt(NULL, NULL));
    imgui_cmt_window_test_reset();
}

/**
 * @brief Akce řádku vydají správný požadavek se správným číslem bloku.
 *
 * Dvojklik a Play from here = REPLAY_CMT_SELECT_FORCED, samotný Enter nic,
 * Ctrl+Enter a Go to this block = REPLAY_CMT_SEEK, Load directly =
 * REPLAY_CMT_SELECT + zásah cmt_set_loading_mode (Direct), Export as MZF / WAV =
 * imgui_cmt_tape_export_block(), Set speed = cmt_set_block_speed, Properties =
 * výběr a rozbalení vlastností. U bloku bez MZF je Export as MZF vypnutý.
 */
void test_w3_row_actions(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    /* zastavený deck (export a rychlost bloku jen ve STOP) */
    v.state = CMT_VIEW_STATE_STOP;
    v.motor_known = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    /* sbalené vlastnosti (výchozí): všech pět řádků tabulky je vidět */
    TEST_ASSERT_EQUAL_UINT(0u, imgui_cmt_window_test_props_open());
    w3_drain();
    int32_t arg = -1;

    /* dvojklik = skok a Play */
    w3_double_click(&v, "name_2");
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SELECT_FORCED, &arg));
    TEST_ASSERT_EQUAL_INT(2, arg);

    /* klik na řádek a samotný Enter nic; Ctrl+Enter = skok na blok (D-G3-16) */
    w2_click(&v, "name_4");
    w3_drain();
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    w2_frame(&v);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    w2_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(replay_test_take_ui_cmt(NULL, NULL), "plain Enter on a clicked row does nothing");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    w2_frame(&v);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    w2_frame(&v);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(4, arg);
    /* šipka nahoru přesune fokus navigace a s ním výběr */
    io.AddKeyEvent(ImGuiKey_UpArrow, true);
    w2_frame(&v);
    io.AddKeyEvent(ImGuiKey_UpArrow, false);
    w2_frame(&v);
    w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    w3_drain();

    /* kontextové menu: Play from here */
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_window_test_get_selection());
    TEST_ASSERT_NOT_NULL(w2_item("ctx_play"));
    w3_click_at(&v, "ctx_play", 0);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SELECT_FORCED, &arg));
    TEST_ASSERT_EQUAL_INT(0, arg);
    w3_close_popups(&v);

    /* kontextové menu: Go to this block = skok bez změny transportu */
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_NOT_NULL(w2_item("ctx_seek"));
    TEST_ASSERT_FALSE(w2_item("ctx_seek")->disabled);
    w3_click_at(&v, "ctx_seek", 0);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(2, arg);
    w3_close_popups(&v);

    /* Load directly: jeden zásah (výběr bloku a Direct), žádná samostatná operace výběru */
    w3_click_at(&v, "name_1", 1);
    w3_click_at(&v, "ctx_load", 0);
    TEST_ASSERT_FALSE(w3_take_cmt_op(REPLAY_CMT_SELECT, &arg));
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOAD_DIRECTLY_NAME, &a));
    TEST_ASSERT_EQUAL_INT(1, a.i[0]);
    w3_close_popups(&v);

    /* Export as MZF bloku 1 */
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_export_mzf")->disabled);
    w3_click_at(&v, "ctx_export_mzf", 0);
    int eb = -1;
    bool wav = true;
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export(&eb, &wav));
    TEST_ASSERT_EQUAL_INT(1, eb);
    TEST_ASSERT_FALSE(wav);
    w3_close_popups(&v);

    /* surový blok 4 (index 3): MZF vypnutý, WAV jde */
    w3_click_at(&v, "name_3", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_export_mzf")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_load")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_speed")->disabled);
    w3_click_at(&v, "ctx_export_mzf", 0);
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_take_last_export(NULL, NULL));
    w3_click_at(&v, "name_3", 1);
    w3_click_at(&v, "ctx_export_wav", 0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export(&eb, &wav));
    TEST_ASSERT_EQUAL_INT(3, eb);
    TEST_ASSERT_TRUE(wav);
    w3_close_popups(&v);

    /* Set speed bloku 2 (index 2): třetí položka podnabídky */
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_speed")->disabled);
    w3_click_at(&v, "ctx_speed", 0);
    TEST_ASSERT_NOT_NULL(w2_item("speed_2"));
    w3_click_at(&v, "speed_2", 0);
    std::vector<ImGuiCmtSpeedList_t> speeds = ImGuiCmt::getMzSpeedListDefault();
    TEST_ASSERT_TRUE(w3_take_request("cmt_set_block_speed", &a));
    TEST_ASSERT_EQUAL_INT(2, a.i[0]);
    TEST_ASSERT_EQUAL_INT((int)speeds[2].cmtspeed, a.i[1]);
    TEST_ASSERT_EQUAL_INT(1, a.i[2]);
    w3_close_popups(&v);

    /* Properties: výběr a rozbalení sbalených vlastností */
    TEST_ASSERT_EQUAL_UINT(0u, imgui_cmt_window_test_props_open());
    w3_click_at(&v, "name_4", 1);
    w3_click_at(&v, "ctx_properties", 0);
    TEST_ASSERT_EQUAL_INT(4, imgui_cmt_window_test_get_selection());
    TEST_ASSERT_EQUAL_UINT(1u, imgui_cmt_window_test_props_open());
    w3_close_popups(&v);

    /* při PLAY: Play from here na jiném bloku jde, Export a Load directly ne (Stop the tape first.) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_export_mzf")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_load")->disabled);
    w3_close_popups(&v);
    /* dvojklik na právě hrající blok ho přehraje znovu od začátku (SEEK), nepauzuje */
    w3_drain();
    w3_double_click(&v, "name_1");
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(1, arg);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Přepínač Echo v liště menu okna (vpravo): ukazuje st_CMT_VIEW::echo a klik přepne g_cmt.echo.
 */
void test_w2_echo_toggle(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    int saved = g_cmt.echo;
    g_cmt.echo = 0;
    v.echo = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NOT_NULL(w2_item("btn_echo"));
    TEST_ASSERT_FALSE(w2_item("btn_echo")->disabled);
    TEST_ASSERT_EQUAL_STRING("Echo", w2_text("btn_echo").c_str());
    /* v liště menu: vpravo od View, nad deckem, pravý okraj u pravého okraje decku */
    TEST_ASSERT_TRUE(w2_item("btn_echo")->min.x >= w2_item("menu_view")->max.x);
    TEST_ASSERT_TRUE(w2_item("btn_echo")->max.y <= w2_item("btn_record")->min.y);
    TEST_ASSERT_TRUE(w2_item("btn_echo")->min.x > w2_item("btn_next")->max.x);
    w3_click_at(&v, "btn_echo", 0);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.echo);
    v.echo = true;
    w3_click_at(&v, "btn_echo", 0);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.echo);
    g_cmt.echo = saved;
    imgui_cmt_window_test_reset();
}

/**
 * @brief View > Columns zapne sloupec (stejné příznaky jako hlavička) a nastavení sloupců přežije uložení a načtení INI ImGui.
 *
 * Restart aplikace se simuluje: SaveIniSettingsToMemory(), TableResetSettings()
 * (výchozí stav), LoadIniSettingsFromMemory() a TableLoadSettings() - tatáž
 * cesta, kterou ImGui načte tabulku z imgui.ini při startu.
 */
void test_w3_columns_menu_and_ini(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    TEST_ASSERT_NULL(w2_item("hdr_4"));
    w3_click_at(&v, "menu_view", 0);
    TEST_ASSERT_NOT_NULL(w2_item("menu_columns"));
    w3_click_at(&v, "menu_columns", 0);
    TEST_ASSERT_EQUAL_STRING("off", w2_text("menu_col_4").c_str());
    w3_click_at(&v, "menu_col_4", 0);
    w3_close_popups(&v);
    TEST_ASSERT_EQUAL_STRING("Size", w2_text("hdr_4").c_str());
    TEST_ASSERT_EQUAL_STRING("12 288 B", w2_text("size_1").c_str());

    /* Show all zapne všechny sloupce najednou a pak je zakázané */
    w3_click_at(&v, "menu_view", 0);
    w3_click_at(&v, "menu_columns", 0);
    TEST_ASSERT_NOT_NULL(w2_item("menu_col_all"));
    TEST_ASSERT_FALSE_MESSAGE(w2_item("menu_col_all")->disabled, "Show all is enabled while a column is hidden");
    w3_click_at(&v, "menu_col_all", 0);
    w3_close_popups(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    for (int c = 0; c < 10; c++)
    {
        char nm[32];
        snprintf(nm, sizeof(nm), "hdr_%d", c);
        TEST_ASSERT_NOT_NULL_MESSAGE(w2_item(nm), nm);
    }
    w3_click_at(&v, "menu_view", 0);
    w3_click_at(&v, "menu_columns", 0);
    TEST_ASSERT_TRUE_MESSAGE(w2_item("menu_col_all")->disabled, "Show all is disabled when all columns are shown");
    w3_close_popups(&v);

    size_t len = 0;
    std::string ini(ImGui::SaveIniSettingsToMemory(&len));
    char sect[48];
    snprintf(sect, sizeof(sect), "[Table][0x%08X", imgui_cmt_window_test_table_id());
    TEST_ASSERT_TRUE_MESSAGE(ini.find(sect) != std::string::npos, "table settings in the ImGui ini");

    ImGuiTable *t = ImGui::TableFindByID(imgui_cmt_window_test_table_id());
    TEST_ASSERT_NOT_NULL(t);
    ImGui::TableResetSettings(t);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NULL(w2_item("hdr_4"));
    ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    ImGui::TableLoadSettings(t);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Size", w2_text("hdr_4").c_str());
    imgui_cmt_window_test_reset();
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NULL(w2_item("hdr_4"));
}

/**
 * @brief Čas kreslení okna s 300 bloky se od 5 bloků liší nejvýš o 50 %.
 *
 * 100 snímků každého stavu (Full, výchozí velikost, 10 snímků zahřátí);
 * průměr bez 10 nejpomalejších snímků (plánovač OS). Test měří čas
 * hostitele, proto má ui-cmt-window v CTest RUN_SERIAL. Dále u 300 bloků
 * značka pozice a výběr řádku.
 */
void test_w3_draw_time_300_blocks(void)
{
    st_CMT_VIEW v5, v300;
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_make_fixture(&v5, CMT_VIEW_FIXTURE_PLAYING_FULL);
    cmt_view_make_fixture(&v300, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    double avg[2];
    st_CMT_VIEW *views[2] = {&v5, &v300};
    for (int k = 0; k < 2; k++) {
        std::vector<double> t;
        for (int i = 0; i < 110; i++) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            bool open = true;
            device_state_lock();
            gint64 t0 = g_get_monotonic_time();
            imgui_cmt_window_draw(views[k], &open);
            gint64 t1 = g_get_monotonic_time();
            device_state_unlock();
            ImGui::Render();
            if (i >= 10) t.push_back((double)(t1 - t0));
        }
        std::sort(t.begin(), t.end());
        double s = 0.0;
        for (size_t i = 0; i < 90; i++) s += t[i];
        avg[k] = s / 90.0;
    }
    fprintf(stderr, "draw time: 5 blocks %.1f us, 300 blocks %.1f us (ratio %.2f)\n", avg[0], avg[1], avg[1] / avg[0]);
    TEST_ASSERT_TRUE_MESSAGE(avg[1] <= avg[0] * 1.5, "300 blocks draw at most 50 % slower than 5 blocks");

    /* u 300 bloků sedí značka pozice
     * i výběr - značka v řádku hrajícího bloku 151 (index 150), klik vybere
     * jiný řádek a značka zůstane; přechod na další blok posune značku, výběr ne */
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    TEST_ASSERT_EQUAL_INT(150, v.current_block);
    const st_CMT_WINDOW_TEST_ITEM *mark = w2_item("mark");
    const st_CMT_WINDOW_TEST_ITEM *row = w2_item("row_150");
    TEST_ASSERT_NOT_NULL_MESSAGE(mark, "the position mark is visible with 300 blocks");
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_TRUE_MESSAGE(mark->min.y >= row->min.y - 1.0f && mark->max.y <= row->max.y + 1.0f, "the mark is in the row of block 151");
    w2_click(&v, "name_152");
    TEST_ASSERT_EQUAL_INT(152, imgui_cmt_window_test_get_selection());
    mark = w2_item("mark");
    row = w2_item("row_150");
    TEST_ASSERT_NOT_NULL(mark);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_TRUE_MESSAGE(mark->min.y >= row->min.y - 1.0f && mark->max.y <= row->max.y + 1.0f, "a click does not move the mark");
    TEST_ASSERT_FALSE_MESSAGE(replay_test_take_ui_cmt(NULL, NULL), "a click does not move the tape");
    v.current_block = 151;
    v.open_serial++;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    mark = w2_item("mark");
    row = w2_item("row_151");
    TEST_ASSERT_NOT_NULL(mark);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_TRUE_MESSAGE(mark->min.y >= row->min.y - 1.0f && mark->max.y <= row->max.y + 1.0f, "the mark follows the playing block");
    TEST_ASSERT_EQUAL_INT_MESSAGE(152, imgui_cmt_window_test_get_selection(), "the selection stays");
    TEST_ASSERT_EQUAL_STRING("PROGRAM 153", w2_text("name_152").c_str());
    w3_drain();
    imgui_cmt_window_test_reset();
}


/* ================================================================
 * Deck: pauza uživatele, posun po přehratelných blocích,
 * požadavky tlačítek decku, INI přes soubor, malá obrazovka
 * ================================================================ */

/**
 * @brief Klik na tlačítko decku a kontrola zařazené operace kazety.
 * @param v Kopie. @param name Prvek. @param op Očekávaná operace. @param arg Očekávaný argument.
 */
static void g2f_expect_op(st_CMT_VIEW *v, const char *name, en_REPLAY_CMT_OP op, int32_t arg)
{
    w3_drain();
    TEST_ASSERT_FALSE_MESSAGE(w2_item(name)->disabled, name);
    w2_click(v, name);
    imgui_cmt_tape_run_deferred(); /* akce automatu pásky (New tape, Eject) se provádí až po uvolnění zámku */
    int32_t got = -12345;
    TEST_ASSERT_TRUE_MESSAGE(w3_take_cmt_op(op, &got), name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(arg, got, name);
}

/**
 * @brief Každé tlačítko decku vydá správný požadavek (zachycené fronty, nic se neprovede).
 *
 * Rewind = SEEK na první přehratelný blok; Play / Pause podle stavu (PLAY ze
 * STOP, pauza uživatele z Playing i Waiting for motor, zrušení pauzy
 * z Paused); Stop, Previous, Next; Record ze STOP (bez pásky do nové),
 * při nahrávání Stop; New tape; výběr rychlosti = zásah cmt_set_speed.
 */
void test_g2f_deck_button_requests(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    g2f_expect_op(&v, "btn_rewind", REPLAY_CMT_SEEK, 0);
    g2f_expect_op(&v, "btn_prev", REPLAY_CMT_PREV, 0);
    g2f_expect_op(&v, "btn_play", REPLAY_CMT_PAUSE, 1);
    g2f_expect_op(&v, "btn_stop", REPLAY_CMT_STOP_BUTTON, 0);
    g2f_expect_op(&v, "btn_next", REPLAY_CMT_NEXT, 0);

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_WAITING_MOTOR);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("pause", w2_text("btn_play").c_str());
    g2f_expect_op(&v, "btn_play", REPLAY_CMT_PAUSE, 1);

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PAUSED);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("play", w2_text("btn_play").c_str());
    g2f_expect_op(&v, "btn_play", REPLAY_CMT_PAUSE, 0);

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS); /* STOP, blok 0 */
    for (int i = 0; i < 2; i++) w2_frame(&v);
    g2f_expect_op(&v, "btn_play", REPLAY_CMT_PLAY, 0);
    g2f_expect_op(&v, "btn_record", REPLAY_CMT_RECORD, 0);

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    g2f_expect_op(&v, "btn_record", REPLAY_CMT_STOP_BUTTON, 0);
    g2f_expect_op(&v, "btn_stop", REPLAY_CMT_STOP_BUTTON, 0);

    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_NO_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    g2f_expect_op(&v, "btn_record", REPLAY_CMT_RECORD, 0);
    g2f_expect_op(&v, "btn_new_tape", REPLAY_CMT_NEW_TAPE, 0);

    /* výběr rychlosti: otevřít combo, vybrat 2. položku */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_EMPTY_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_drain();
    w3_click_at(&v, "speed_combo", 0);
    {
        /* položky popupu výběru: druhá v pořadí pod výběrem */
        ImGuiWindow *pop = NULL;
        for (ImGuiWindow *w : ImGui::GetCurrentContext()->Windows)
            if ((w->Flags & ImGuiWindowFlags_Popup) && w->Active) pop = w;
        TEST_ASSERT_NOT_NULL_MESSAGE(pop, "speed combo popup is open");
        ImVec2 c(pop->Pos.x + pop->Size.x * 0.5f, pop->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeightWithSpacing() * 1.5f);
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(c.x, c.y);
        w2_frame(&v);
        io.AddMouseButtonEvent(0, true);
        w2_frame(&v);
        io.AddMouseButtonEvent(0, false);
        w2_frame(&v);
        w2_frame(&v);
    }
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE_MESSAGE(w3_take_request("cmt_set_speed", &a), "speed selection submits cmt_set_speed");
    w3_drain();
    imgui_cmt_window_test_reset();
}

/** @brief Přidá do souboru řídicí blok Text description (0x30) s krátkým textem. */
static void g2f_add_text(st_TZX_FILE *f, const char *text)
{
    uint8_t buf[64];
    size_t n = strlen(text);
    buf[0] = (uint8_t)n;
    memcpy(buf + 1, text, n);
    add_raw_block(f, TZX_BLOCK_ID_TEXT_DESCRIPTION, buf, (uint32_t)(n + 1));
}

/**
 * @brief Páska, která začíná i končí řídicím blokem: jádro i okno se pohybují jen po přehratelných blocích.
 *
 * Bloky: 0 text, 1 MZF, 2 MZF, 3 text. Jádro: Previous z prvního
 * přehratelného bloku i Next z posledního selže a blok zůstane; seek na
 * řídicí blok selže. Okno nad kopií z jádra: Rewind = SEEK na blok 1
 * (ne 0), Previous na bloku 1 a Next na bloku 2 zakázané s důvodem.
 */
void test_g2f_control_block_ends(void)
{
    st_TZX_FILE *f = new_tmz();
    g2f_add_text(f, "start");
    add_mzf(f, "FIRST", 0x01, 64, 1000);
    add_mzf(f, "SECOND", 0x01, 64, 1000);
    g2f_add_text(f, "end");
    const char *path = tmp_named(3, "ctl_ends.tmz");
    save_file(f, path);
    open_tape(path);

    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(4, v.block_count);
    TEST_ASSERT_FALSE(v.blocks[0].playable);
    TEST_ASSERT_FALSE(v.blocks[3].playable);
    TEST_ASSERT_EQUAL_INT(1, v.current_block);

    /* jádro: dozadu z prvního přehratelného bloku nejde, blok zůstane */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(0));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(1, v.current_block);

    /* okno: Rewind míří na blok 1, Previous zakázané, Next povolené */
    w3_setup(&v, CMT_VIEW_FIXTURE_NO_TAPE);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    fill(&v);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("btn_prev")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_next")->disabled);
    g2f_expect_op(&v, "btn_rewind", REPLAY_CMT_SEEK, 1);

    /* jádro: dopředu na blok 2, dál nejde */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_seek_block(3));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_seek_block(0));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(2, v.current_block);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FALSE(w2_item("btn_prev")->disabled);
    TEST_ASSERT_TRUE(w2_item("btn_next")->disabled);
    g2f_expect_op(&v, "btn_prev", REPLAY_CMT_PREV, 0);
    g2f_expect_op(&v, "btn_rewind", REPLAY_CMT_SEEK, 1);

    /* Rewind provedený jádrem: blok 1 */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(1, v.current_block);
    imgui_cmt_window_test_reset();
}

/**
 * @brief [VIRTUAL_CMT_UI] přes skutečný soubor INI: rozvržení a velikosti obou rozvržení přežijí uložení a načtení.
 */
void test_g2f_ini_round_trip(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    gchar *ini = g_build_filename(g_get_tmp_dir(), "mztest_cmt_window_ui.ini", NULL);
    g_remove(ini);
    st_CFGROOT *saved_root = g_cfgmain;
    s_g2f_saved_cfgroot = saved_root;
    s_g2f_cfgroot_replaced = true;
    g_cfgmain = cfgroot_new(ini);
    imgui_cmt_window_test_cfg_forget();
    imgui_cmt_window_cfg_init();
    imgui_cmt_window_test_reset();

    st_CMT_VIEW v;
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 c = w2_window_size();
    ImGui::SetWindowSize(W2_ID, ImVec2(c.x + 48.0f, c.y + 20.0f), ImGuiCond_Always);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImVec2 compact = w2_window_size();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 f0 = w2_window_size();
    ImGui::SetWindowSize(W2_ID, ImVec2(f0.x + 32.0f, f0.y + 16.0f), ImGuiCond_Always);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImVec2 full = w2_window_size();
    cfgroot_save(g_cfgmain);
    cfgroot_destroy(g_cfgmain);

    gchar *text = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(ini, &text, NULL, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "[VIRTUAL_CMT_UI]"), "section written to the INI file");
    g_free(text);

    /* jiný stav v paměti, pak načtení ze souboru */
    imgui_cmt_window_test_reset();
    g_cfgmain = cfgroot_new(ini);
    imgui_cmt_window_test_cfg_forget();
    imgui_cmt_window_cfg_init();
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(CMT_WINDOW_LAYOUT_FULL, imgui_cmt_window_get_layout());
    ImVec2 full2 = w2_window_size();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, full.x, full2.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, full.y, full2.y);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 compact2 = w2_window_size();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, compact.x, compact2.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, compact.y, compact2.y);

    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;
    s_g2f_cfgroot_replaced = false;
    imgui_cmt_window_test_cfg_forget();
    imgui_cmt_window_test_reset();
    g_remove(ini);
    g_free(ini);
}

/**
 * @brief Malá obrazovka: výchozí velikost Full se vejde na displej 768 px; úzká pracovní plocha zalomí řádky decku.
 *
 * (a) Při 100 % je výchozí výška okna Full nejvýš 700 px (displej 768 px
 * s hlavním menu a titulkem okna aplikace). (b) Pracovní plocha užší než
 * jednořádkový deck (175 %, texty o 40 % delší): okno se vejde, Loading
 * je pod Signal speed (stav a motor se zalomí, jen když je třeba), nic se
 * nepřekrývá ani neořízne. (c) Pamatovaná velikost větší než pracovní plocha se
 * zmenší na pracovní plochu.
 */
void test_g2f_small_work_area(void)
{
    w2_fonts();
    st_CMT_VIEW v;

    /* (a) výchozí Full při 100 % */
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 full = w2_window_size();
    fprintf(stderr, "default Full window at 100 %%: %.0f x %.0f px\n", full.x, full.y);
    TEST_ASSERT_TRUE_MESSAGE(full.y <= 700.0f, "default Full height fits a 768 px display");

    /* (b) úzká pracovní plocha */
    w2_apply_scale(1.75f);
    imgui_cmt_window_test_set_stretch(true);
    const en_CMT_VIEW_FIXTURE fx[] = {CMT_VIEW_FIXTURE_PLAYING_COMPACT, CMT_VIEW_FIXTURE_DIRECT, CMT_VIEW_FIXTURE_PAUSED};
    int faults = 0;
    for (en_CMT_VIEW_FIXTURE f : fx) {
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
        cmt_view_make_fixture(&v, f);
        /* šířka jednořádkového decku, pak pracovní plocha o 1 px užší (výška 0 = skutečná) */
        imgui_cmt_window_test_set_work_size(ImVec2(1e6f, 0.0f));
        for (int i = 0; i < 3; i++) w2_frame(&v);
        ImVec2 wide = imgui_cmt_window_test_min_size();
        imgui_cmt_window_test_set_work_size(ImVec2(wide.x - 1.0f, 0.0f));
        for (int i = 0; i < 3; i++) w2_frame(&v);
        ImVec2 one = imgui_cmt_window_test_min_size();
        fprintf(stderr, "deck width: single line %.0f px, wrapped %.0f px\n", wide.x, one.x);
        TEST_ASSERT_TRUE_MESSAGE(one.x < wide.x - 1.0f, "a narrow work area allows a narrower window (wrapped rows)");
        float narrow = one.x;
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
        imgui_cmt_window_test_set_work_size(ImVec2(narrow, 1600.0f));
        for (int i = 0; i < 3; i++) w2_frame(&v);
        ImVec2 ws = w2_window_size();
        TEST_ASSERT_TRUE_MESSAGE(ws.x <= narrow + 0.5f, "window fits the narrow work area");
        faults += w2_geometry_faults("narrow work area");
        const st_CMT_WINDOW_TEST_ITEM *sl = w2_item("speed_label");
        const st_CMT_WINDOW_TEST_ITEM *ll = w2_item("loading_label");
        TEST_ASSERT_TRUE_MESSAGE(ll->min.y >= sl->max.y, "Loading wraps below Signal speed");
        /* stav a motor: zalomí se jen, když se pravý sloupec nevejde (kontrola překryvu výše) */
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, faults, "narrow deck items clipped or overlapping (details on stderr)");

    /* (c) pamatovaná velikost větší než pracovní plocha */
    imgui_cmt_window_test_set_stretch(false);
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_test_set_work_size(ImVec2(1e6f, 1e6f));
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImGui::SetWindowSize(W2_ID, ImVec2(1500.0f, 1100.0f), ImGuiCond_Always);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_window_test_set_work_size(ImVec2(1200.0f, 900.0f));
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    ImVec2 ws = w2_window_size();
    TEST_ASSERT_TRUE_MESSAGE(ws.x <= 1200.5f && ws.y <= 900.5f, "remembered size is clamped to the work area");

    imgui_cmt_window_test_set_work_size(ImVec2(0.0f, 0.0f));
    imgui_cmt_window_test_reset();
}


/**
 * @brief Uloží levý horní výřez plátna (okno v rohu) jako PNG.
 * @param dir Adresář. @param file Jméno souboru. @param w Šířka výřezu v px. @param h Výška výřezu v px.
 * @return true = uloženo.
 */
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
    /* GL má počátek dole: řádek y výřezu = řádek (H - 1 - y) plátna */
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * dst_pitch, px + (size_t)(s_canvas_h - 1 - y) * src_pitch, dst_pitch);
    size_t len = 0;
    uint8_t *png = png_encode_rgba(crop, (uint32_t)w, (uint32_t)h, &len);
    bool ok = false;
    if (png) {
        gchar *path = g_build_filename(dir, file, NULL);
        ok = g_file_set_contents(path, (const gchar *)png, (gssize)len, NULL);
        if (!ok) fprintf(stderr, "can't write %s\n", path);
        g_free(path);
        g_free(png);
    }
    free(px);
    free(crop);
    return ok;
}

/**
 * @brief Snímky "po" nového okna (sada g2): koncepty v2 z testovacích stavů.
 *
 * Jména "g2-<jazyk>-<měřítko v %>-<stav>.png" (při MZ_CMT_SHOT_STRETCH=1
 * jazyk s příponou "+40"); okno ve výchozí velikosti rozvržení, výřez
 * plátna velikosti okna. Ke každému snímku řádky "geo:" a výsledek
 * kontroly geometrie ("check:") na stderr.
 *
 * @return Počet neuložených snímků a snímků s vadou geometrie.
 */
static int shots_g2_run(const char *dir)
{
    struct { en_CMT_VIEW_FIXTURE f; en_CMT_WINDOW_LAYOUT lay; const char *name; } shots[] = {
        {CMT_VIEW_FIXTURE_PLAYING_COMPACT, CMT_WINDOW_LAYOUT_COMPACT, "compact"},
        {CMT_VIEW_FIXTURE_NO_TAPE, CMT_WINDOW_LAYOUT_COMPACT, "compact-notape"},
        {CMT_VIEW_FIXTURE_WAITING_MOTOR, CMT_WINDOW_LAYOUT_COMPACT, "compact-waiting"},
        {CMT_VIEW_FIXTURE_DIRECT, CMT_WINDOW_LAYOUT_COMPACT, "compact-direct"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_COMPACT, "compact-recording"},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, CMT_WINDOW_LAYOUT_COMPACT, "compact-empty"},
        {CMT_VIEW_FIXTURE_LONG_TEXTS, CMT_WINDOW_LAYOUT_COMPACT, "compact-longtexts"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_FULL, "full-recording"},
        {CMT_VIEW_FIXTURE_PLAYING_FULL, CMT_WINDOW_LAYOUT_FULL, "full"},
    };
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    int failed = 0;
    st_CMT_VIEW v;
    setUp();
    for (auto &s : shots) {
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(s.lay);
        cmt_view_make_fixture(&v, s.f);
        for (int i = 0; i < 4; i++) w2_frame(&v, true);
        ImVec2 sz = w2_window_size();
        /* s_shot_tag = "<jazyk>-<měřítko>"; při prodloužení "<jazyk>+40-<měřítko>" */
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("g2-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "",
                                      dash ? dash : "", s.name);
        if (!shot_save_crop(dir, file, (int)(sz.x + 0.5f), (int)(sz.y + 0.5f))) failed++;
        int faults = w2_geometry_faults(file);
        fprintf(stderr, "geo: %s window %.0f x %.0f px, min %.0f x %.0f px, font %.1f px\n", file, sz.x, sz.y,
                imgui_cmt_window_test_min_size().x, imgui_cmt_window_test_min_size().y, ImGui::GetFontSize());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
    }
    tearDown();
    imgui_cmt_window_test_set_stretch(false);
    return failed;
}


/**
 * @brief Snímky "po" tabulky a vlastností (sada g3): rozvržení Full z testovacích stavů.
 *
 * Jména "g3-<jazyk>-<měřítko v %>-<stav>.png" (při MZ_CMT_SHOT_STRETCH=1
 * jazyk s příponou "+40"); okno ve výchozí velikosti Full, výřez velikosti
 * okna; PLAYING_FULL s výběrem řádku 4 jako koncept cmt-full-v2. Ke každému
 * snímku "geo:" a výsledek kontroly geometrie a vodorovného posuvníku
 * tabulky ("check:").
 *
 * @return Počet neuložených snímků a snímků s vadou.
 */
static int shots_g3_run(const char *dir)
{
    struct { en_CMT_VIEW_FIXTURE f; const char *name; } shots[] = {
        {CMT_VIEW_FIXTURE_PLAYING_FULL, "full"},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, "full-empty"},
        {CMT_VIEW_FIXTURE_RECORDING, "full-recording"},
        {CMT_VIEW_FIXTURE_MANY_BLOCKS, "full-manyblocks"},
        {CMT_VIEW_FIXTURE_LONG_TEXTS, "full-longtexts"},
        {CMT_VIEW_FIXTURE_CONTROL_BLOCKS, "full-controlblocks"},
    };
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    int failed = 0;
    st_CMT_VIEW v;
    setUp();
    for (auto &s : shots) {
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
        cmt_view_make_fixture(&v, s.f);
        imgui_cmt_window_test_set_selection(cmt_view_fixture_selection(s.f), v.tape_serial);
        for (int i = 0; i < 4; i++) w2_frame(&v, true);
        ImVec2 sz = w2_window_size();
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("g3-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "",
                                      dash ? dash : "", s.name);
        if (!shot_save_crop(dir, file, (int)(sz.x + 0.5f), (int)(sz.y + 0.5f))) failed++;
        int faults = w2_geometry_faults(file);
        if (imgui_cmt_window_test_table_scroll_x()) {
            fprintf(stderr, "%s: table has a horizontal scrollbar\n", file);
            faults++;
        }
        fprintf(stderr, "geo: %s window %.0f x %.0f px, min %.0f x %.0f px\n", file, sz.x, sz.y,
                imgui_cmt_window_test_min_size().x, imgui_cmt_window_test_min_size().y);
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
    }
    tearDown();
    imgui_cmt_window_test_set_stretch(false);
    return failed;
}

/* ================================================================
 * Tabulka bloků: zastaralé číslo bloku, minimum Full, Enter v tabulce,
 * Set speed jen ve STOP, šířky sloupců, akce na řídicím řádku a ve stavech
 * ================================================================ */

/** @brief Zpráva řádku zpráv po zásazích (kopie stavu nad jádrem). @param mv Kopie. @return Zpráva. */
static std::string g3f_message(st_CMT_VIEW *mv)
{
    fill(mv);
    return mv->message;
}

/**
 * @brief Odložené akce s blokem patří pásce, ze které blok pochází.
 *
 * Kontextové menu se po výměně pásky zavře se zprávou; export nese čítač
 * výměn pásky přes dialog souboru (UI ho zahodí, emu vlákno ho odmítne);
 * editor vlastních pulzů bloku potvrzený nad jinou páskou nic nezmění.
 */
void test_g3f_stale_block_actions(void)
{
    const char *changed = "The tape changed; the action was cancelled.";
    st_CMT_VIEW mv;
    cmt_view_reset(&mv);
    fill(&mv);

    /* (a) kontextové menu: export nese čítač pásky; jiná páska menu zavře */
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "name_3", 1);
    w3_click_at(&v, "ctx_export_wav", 0);
    int eb = -1;
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export(&eb, NULL));
    TEST_ASSERT_EQUAL_INT(3, eb);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, imgui_cmt_tape_test_last_export_serial());
    w3_close_popups(&v);
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_NOT_NULL(w2_item("ctx_export_mzf"));
    v.tape_serial++;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NULL_MESSAGE(w2_item("ctx_export_mzf"), "the context menu closes when the tape changes");
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());
    /* uživatel nic nevybral, zavřené menu je varování, ne chyba */
    TEST_ASSERT_FALSE(mv.message_is_error);
    TEST_ASSERT_TRUE(mv.message_is_warning);
    w3_close_popups(&v);

    /* kratší páska - řádek, ke kterému menu patřilo, už neexistuje: menu se zavře (varování) */
    cmt_view_post_message("", false);
    w3_click_at(&v, "name_4", 1);
    TEST_ASSERT_NOT_NULL(w2_item("ctx_export_mzf"));
    v.blocks.resize(3);
    v.block_count = 3;
    v.tape_serial++;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_NULL_MESSAGE(w2_item("ctx_export_mzf"), "an orphaned context menu closes");
    TEST_ASSERT_FALSE_MESSAGE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel), "no popup left open");
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());
    TEST_ASSERT_TRUE(mv.message_is_warning);
    /* stejná páska, řádek se stejným indexem se znovu objeví: menu se samo neotevře */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    v.tape_serial += 10;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_NULL_MESSAGE(w2_item("ctx_export_mzf"), "the menu does not reappear over a new row");
    w3_close_popups(&v);

    /* (b) editor pulzů bloku: otevřený nad páskou A, potvrzený nad páskou B */
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_speed", 0);
    std::vector<ImGuiCmtSpeedList_t> speeds = ImGuiCmt::getMzSpeedListDefault();
    char nm[32];
    snprintf(nm, sizeof(nm), "speed_%u", (unsigned)(speeds.size() - 1));
    TEST_ASSERT_TRUE(cmtspeed_is_custom(speeds.back().cmtspeed));
    w3_click_at(&v, nm, 0);
    w3_close_popups(&v); /* editor (modál) zavřít Escapem */
    w3_drain();
    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 470.0, 494.0, 240.0, 278.0));
    unsigned opened = v.tape_serial;
    v.tape_serial++;
    imgui_cmt_window_test_confirm_block_pulses(&v, 2, &p);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request("cmt_set_block_speed", NULL), "no block speed for a different tape");
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());
    v.tape_serial = opened;
    imgui_cmt_window_test_confirm_block_pulses(&v, 2, &p);
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request("cmt_set_block_speed", &a));
    TEST_ASSERT_EQUAL_INT(2, a.i[0]);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, a.i[1]);
    TEST_ASSERT_EQUAL_INT(1, a.i[4]);
    TEST_ASSERT_EQUAL_UINT(opened, (unsigned)a.i[5]);
    imgui_cmt_window_test_reset();

    /* (c) export nad skutečným jádrem: dialog dokončený po výměně pásky */
    open_games_tape();
    fill(&mv);
    unsigned sa = mv.tape_serial;
    const char *wav = tmp_named(2, "block.wav");
    device_state_lock();
    imgui_cmt_tape_export_block(3, true, sa);
    device_state_unlock();
    imgui_cmt_tape_test_take_last_export(NULL, NULL);
    imgui_cmt_tape_test_export_finish(3, sa, wav);
    TEST_ASSERT_TRUE(w3_take_request("cmt_export_block", &a));
    TEST_ASSERT_EQUAL_INT(3, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[4]);
    TEST_ASSERT_EQUAL_UINT(sa, (unsigned)a.i[5]);
    open_tape(make_two_block_mzt());
    fill(&mv);
    TEST_ASSERT_NOT_EQUAL_UINT(sa, mv.tape_serial);
    imgui_cmt_tape_test_export_finish(3, sa, wav);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request("cmt_export_block", NULL), "no export of a block of another tape");
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());

    /* (d) emu vlákno: zásah se starým čítačem odmítne (export i rychlost bloku) */
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(cmt_request_export_block_on_tape(sa, 1, wav) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, res.rc);
    TEST_ASSERT_FALSE(g_file_test(wav, G_FILE_TEST_EXISTS));
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    cmt_view_post_message("-", false);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());
    TEST_ASSERT_TRUE(cmt_request_set_block_speed_on_tape(sa, 1, CMTSPEED_3_1, true, NULL) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, res.rc);
    cmt_view_post_message("-", false);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    TEST_ASSERT_EQUAL_STRING(changed, g3f_message(&mv).c_str());
    /* tatáž páska: provede se */
    TEST_ASSERT_TRUE(cmt_request_set_block_speed_on_tape(mv.tape_serial, 1, CMTSPEED_3_1, true, NULL) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(0, res.rc);
}

/**
 * @brief Set speed jen ve STOP (D-G3-14): při PLAY (i jiný než hrající blok) a RECORD zakázané s důvodem.
 */
void test_g3f_set_speed_only_in_stop(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL); /* PLAY na bloku 1 */
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_speed")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w2_item("ctx_speed")->reason.c_str());
    w3_close_popups(&v);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_click_at(&v, "name_4", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_speed")->disabled);
    w3_close_popups(&v);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_speed")->disabled);
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/** @brief Žádná operace kazety ani zásah ve frontách (zahodí obsah). @return true = nic. */
static bool g3f_nothing(void)
{
    bool none = !replay_test_take_ui_cmt(NULL, NULL);
    none = !device_request_test_take_pending(NULL, NULL) && none;
    return none;
}

/**
 * @brief Stisk Enter (s Ctrl nebo bez) ve dvou snímcích okna.
 * @param v Kopie. @param ctrl true = Ctrl+Enter.
 */
static void g3f_enter(st_CMT_VIEW *v, bool ctrl)
{
    ImGuiIO &io = ImGui::GetIO();
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    w2_frame(v);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    w2_frame(v);
}

/**
 * @brief Klik na řádek (vybere ho a dá mu fokus navigace) a Ctrl+Enter.
 * @param v Kopie. @param name Prvek řádku.
 */
static void g3f_click_enter(st_CMT_VIEW *v, const char *name)
{
    w2_click(v, name);
    w3_drain();
    g3f_enter(v, true);
}

/**
 * @brief Akce řádku na řídicím bloku a ve stavech RECORD, Direct a přehrávání záznamu vstupů.
 *
 * Řídicí řádek: Play from here zakázané, Enter ani dvojklik nic. RECORD:
 * dvojklik, Enter ani Play from here nic. Direct: dvojklik nic (Not used
 * in Direct mode), Enter skočí (vybírá blok příštího LOAD). Přehrávání
 * záznamu vstupů: nic. Export as WAV bloku, který player neumí: "This block
 * cannot be played.".
 */
void test_g3f_row_actions_in_states(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS); /* STOP; 1 Loop start, 5 Cannot play */
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_play")->disabled);
    TEST_ASSERT_EQUAL_STRING("This block cannot be played.", w2_item("ctx_play")->reason.c_str());
    w3_click_at(&v, "ctx_play", 0);
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "Play from here on a control row");
    w3_close_popups(&v);
    w3_click_at(&v, "name_5", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_export_wav")->disabled);
    TEST_ASSERT_EQUAL_STRING("This block cannot be played.", w2_item("ctx_export_wav")->reason.c_str());
    w3_close_popups(&v);
    g3f_click_enter(&v, "name_1");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "Enter on a control row");
    w3_double_click(&v, "name_1");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "double-click on a control row");

    /* RECORD */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_drain();
    w3_double_click(&v, "name_4");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "double-click while recording");
    g3f_click_enter(&v, "name_4");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "Enter while recording");
    w3_click_at(&v, "name_4", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_play")->disabled);
    w3_close_popups(&v);

    /* Direct: dvojklik nic, Enter vybere blok příštího LOAD */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_DIRECT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_drain();
    w3_double_click(&v, "name_2");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "double-click in Direct mode");
    g3f_click_enter(&v, "name_2");
    int32_t arg = -1;
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(2, arg);

    /* přehrávání záznamu vstupů: nic */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    v.replay_playing = true;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_drain();
    w3_double_click(&v, "name_2");
    g3f_click_enter(&v, "name_2");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "row actions while an input replay is played");
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_export_mzf")->disabled);
    TEST_ASSERT_TRUE(w2_item("ctx_speed")->disabled);
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/** @brief Snímek s oknem kazety a dalším oknem "hlavní okno" (klik do něj vezme fokus). @param v Kopie. */
static void g3f_frame_with_other(st_CMT_VIEW *v)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(1700.0f, 100.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Always);
    ImGui::Begin("Main##g3f_other");
    ImGui::TextUnformatted("emulator");
    ImGui::End();
    if (ImGui::FindWindowByName(W2_ID)) ImGui::SetWindowPos(W2_ID, ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    bool open = true;
    device_state_lock();
    imgui_cmt_window_draw(v, &open);
    device_state_unlock();
    ImGui::Render();
}

/**
 * @brief Nahrávání v pauze uživatele: Play pauzu zruší.
 *
 * Pauzu uživatele při nahrávání mohou zapnout MCP, staré rozhraní nebo
 * přehrání záznamu vstupů. Okno ukáže "Paused" a tlačítko Play (i zkratka
 * Ctrl+Space) pošle REPLAY_CMT_PAUSE 0; při běžícím nahrávání Play
 * zůstává zakázané (čekání na motor viz test_ff_record_waiting_play_starts()).
 */
void test_g2f2_record_user_pause_resume(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    st_CMT_VIEW v;
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE_MESSAGE(w2_item("btn_play")->disabled, "Play is disabled while recording without a user's pause");

    v.paused = true;
    v.user_paused = true;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Paused", w2_text("status_text").c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_play")->disabled, "Play resumes a paused recording");
    TEST_ASSERT_TRUE(w2_item("btn_record")->disabled == false);
    w3_drain();
    w2_click(&v, "btn_play");
    int32_t arg = -1;
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PAUSE, &arg));
    TEST_ASSERT_EQUAL_INT(0, arg);
    w3_drain();
}

/**
 * @brief Nahrávání čekající na motor rozjede Play z okna.
 *
 * Program, který zapisuje na kazetu a motor sám nezapne, by jinak nahrál
 * nic. Play při RECORD bez pauzy uživatele se stojícím motorem ("Waiting
 * for motor") je povolené a pošle REPLAY_CMT_PAUSE 0 (v jádru "rozjeď
 * čekající pásku", cmt_pause()). Nad skutečným jádrem: cmt_record(), klik
 * na Play, provedení fronty emu vláknem (replay_on_paused() mimo záznam
 * vstupů) -> nahrávání běží. Běžící nahrávání má Play dál zakázané.
 */
void test_ff_record_waiting_play_starts(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    device_state_lock();
    int rc = cmt_record();
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_RECORD, v.state);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_FALSE(v.user_paused);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Waiting for motor", w2_text("status_text").c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_play")->disabled, "Play starts a recording waiting for the motor");
    TEST_ASSERT_EQUAL_STRING("play", w2_text("btn_play").c_str());
    w3_drain();
    w2_click(&v, "btn_play");
    /* emu vlákno mimo záznam vstupů frontu okna provede (jako paused smyčka) */
    replay_on_paused();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_RECORD, v.state);
    TEST_ASSERT_FALSE_MESSAGE(v.paused, "the recording runs after Play");
    TEST_ASSERT_TRUE(v.motor_on);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Recording", w2_text("status_text").c_str());
    TEST_ASSERT_TRUE_MESSAGE(w2_item("btn_play")->disabled, "Play stays disabled while the recording runs");

    /* testovací stav: čekání na motor při nahrávání -> REPLAY_CMT_PAUSE 0 */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    v.paused = true;
    v.user_paused = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Waiting for motor", w2_text("status_text").c_str());
    w3_drain();
    w2_click(&v, "btn_play");
    int32_t arg = -1;
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PAUSE, &arg));
    TEST_ASSERT_EQUAL_INT(0, arg);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/** @brief Provede frontu operací kazety z UI za emu vlákno (mimo záznam vstupů, paused smyčka). */
static void ff_run_ui_cmt(void)
{
    replay_on_paused();
}

/** @brief Podmínky operace z okna. @param flags Bity REPLAY_CMT_GUARD_*. @param serial Čítač výměn pásky. */
static st_REPLAY_CMT_GUARD ff_guard(unsigned flags, unsigned serial)
{
    st_REPLAY_CMT_GUARD g;
    g.flags = flags;
    g.tape_serial = serial;
    return g;
}

/**
 * @brief Operace okna rozhodnuté ze starého snímku emu vlákno zahodí.
 *
 * Nad skutečným jádrem, jednovláknově (fronta replay_host_cmt_guarded()
 * a její provedení replay_on_paused()): (a) SEEK s čítačem jiné pásky se
 * neprovede, se správným ano; (b) pauza (REPLAY_CMT_PAUSE 1) zadaná při
 * PLAY a provedená po konci pásky (STOP) nespustí PLAY čekající na motor,
 * při PLAY pauzuje; (c) SELECT_FORCED na blok, který mezitím začal hrát,
 * pásku nepozastaví, na jiný blok ho přehraje.
 */
void test_ff_guarded_cmt_ops(void)
{
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    /* (a) páska vyměněná mezi rozhodnutím okna a provedením */
    open_games_tape();
    unsigned sa = cmttmz_get_tape_serial();
    st_REPLAY_CMT_GUARD g = ff_guard(REPLAY_CMT_GUARD_TAPE, sa);
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 2, &g);
    open_tape(make_two_block_mzt());
    TEST_ASSERT_NOT_EQUAL_UINT(sa, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_get_open_block_id(), "SEEK for another tape is dropped");
    g = ff_guard(REPLAY_CMT_GUARD_TAPE, cmttmz_get_tape_serial());
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 1, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmttmz_get_open_block_id(), "SEEK for the same tape runs");

    /* (b) pauza zadaná při PLAY, provedená po konci pásky */
    open_games_tape();
    device_state_lock();
    cmt_play();
    device_state_unlock();
    g = ff_guard(REPLAY_CMT_GUARD_NOT_STOP, 0u);
    replay_host_cmt_guarded(REPLAY_CMT_PAUSE, 1, &g);
    device_state_lock();
    cmt_stop(); /* konec pásky */
    device_state_unlock();
    ff_run_ui_cmt();
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_STOP, "a pause requested before the end of the tape does not start it");
    device_state_lock();
    cmt_play();
    device_state_unlock();
    replay_host_cmt_guarded(REPLAY_CMT_PAUSE, 1, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    device_state_lock();
    cmt_stop();
    device_state_unlock();

    /* (c) dvojklik na blok, který mezitím začal hrát (přechod N -> N+1) */
    g = ff_guard(REPLAY_CMT_GUARD_TAPE | REPLAY_CMT_GUARD_NOT_PLAYING_ARG, cmttmz_get_tape_serial());
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 1, NULL);
    ff_run_ui_cmt();
    device_state_lock();
    cmt_play(); /* blok 1 hraje */
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_open_block_id());
    replay_host_cmt_guarded(REPLAY_CMT_SELECT_FORCED, 1, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "double-click on the block that started playing does not pause the tape");
    /* jiný blok: přehraje se */
    replay_host_cmt_guarded(REPLAY_CMT_SELECT_FORCED, 2, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT(2, cmttmz_get_open_block_id());
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
}

/** @brief Výsledek ff_frame_with_main(): pravidlo klávesnice hlavního okna v posledním snímku. */
static bool s_ff_main_keyboard = false;

/**
 * @brief Snímek s náhradou hlavního okna emulátoru (přes celou plochu, vzadu) a oknem kazety.
 *
 * Náhrada má okenní příznaky hlavního okna (MAIN_WINDOW_FLAGS ze sdílené
 * hlavičky main_window_focus.h, tytéž jako main_window.cpp) a uvnitř
 * vyhodnotí totéž pravidlo jako aplikace (main_window_keyboard_focus()),
 * výsledek uloží do s_ff_main_keyboard.
 *
 * @param v Kopie stavu okna kazety.
 */
static void ff_frame_with_main(st_CMT_VIEW *v)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##MainEmulatorWindow", NULL, MAIN_WINDOW_FLAGS);
    s_ff_main_keyboard = main_window_keyboard_focus(false);
    ImGui::End();
    if (ImGui::FindWindowByName(W2_ID)) ImGui::SetWindowPos(W2_ID, ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    bool open = true;
    device_state_lock();
    imgui_cmt_window_draw(v, &open);
    device_state_unlock();
    ImGui::Render();
}

/**
 * @brief Při fokusu okna kazety emulovaná klávesnice nic nedostává.
 *
 * Pravidlo aplikace (main_window_keyboard_focus(), podle něj obsluha
 * událostí SDL pouští klávesy do emulované klávesnice): klik do okna
 * kazety -> false, i během psaní kláves bez Ctrl (ty okno také nic
 * nespustí); klik zpět do obrazu (hlavní okno) -> true. Samotnou obsluhu
 * událostí SDL (iface_sdl3_events_cb()) test nevolá - potřebuje okno
 * sdlapp a smyčku událostí SDL.
 */
void test_ff_keyboard_not_routed_while_cmt_focused(void)
{
    st_CMT_VIEW v;
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < 3; i++) ff_frame_with_main(&v);
    ImGuiWindow *cw = ImGui::FindWindowByName(W2_ID);
    TEST_ASSERT_NOT_NULL(cw);
    /* klik do obrazu (mimo okno kazety) */
    ImVec2 outside(cw->Pos.x + cw->Size.x + 40.0f, cw->Pos.y + cw->Size.y + 40.0f);
    io.AddMousePosEvent(outside.x, outside.y);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, true);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, false);
    ff_frame_with_main(&v);
    ff_frame_with_main(&v);
    TEST_ASSERT_TRUE_MESSAGE(s_ff_main_keyboard, "the emulated keyboard gets keys while the picture has the focus");
    /* klik do okna kazety (na text stavu, ne na tlačítko) */
    ImVec2 inside(cw->Pos.x + cw->Size.x * 0.5f, cw->Pos.y + ImGui::GetFrameHeight() * 0.5f);
    io.AddMousePosEvent(inside.x, inside.y);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, true);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, false);
    ff_frame_with_main(&v);
    ff_frame_with_main(&v);
    TEST_ASSERT_FALSE_MESSAGE(s_ff_main_keyboard, "the emulated keyboard gets nothing while the CMT window has the focus");
    w3_drain();
    /* psaní bez Ctrl při fokusu okna kazety: emulovaná klávesnice nic, okno nic */
    const ImGuiKey keys[] = {ImGuiKey_A, ImGuiKey_Space, ImGuiKey_Enter, ImGuiKey_R, ImGuiKey_Period};
    for (ImGuiKey k : keys) {
        io.AddKeyEvent(k, true);
        ff_frame_with_main(&v);
        TEST_ASSERT_FALSE(s_ff_main_keyboard);
        io.AddKeyEvent(k, false);
        ff_frame_with_main(&v);
        TEST_ASSERT_FALSE(s_ff_main_keyboard);
    }
    TEST_ASSERT_FALSE_MESSAGE(replay_test_take_ui_cmt(NULL, NULL), "keys without Ctrl start no deck action");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    /* zpět do obrazu */
    io.AddMousePosEvent(outside.x, outside.y);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, true);
    ff_frame_with_main(&v);
    io.AddMouseButtonEvent(0, false);
    ff_frame_with_main(&v);
    ff_frame_with_main(&v);
    TEST_ASSERT_TRUE(s_ff_main_keyboard);
    io.AddMousePosEvent(-1.0f, -1.0f);
    ff_frame_with_main(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Enable CPU Boost z hlavního menu nehlásí změnu režimu, která nenastala.
 *
 * Se zapnutým CMT hackem zůstává režim Direct a mění se jen CPU Boost:
 * řádek zpráv nic nového neukáže (dřív "Loading mode: Direct"). Bez hacku
 * "Loading mode: Boost" / "Loading mode: Tape" jako dřív; přepínač Loading
 * okna do Direct dál hlásí "Loading mode: Direct".
 */
void test_ff_menu_boost_message(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    st_DEVICE_REQ_RESULT res;

    g_cmthack.load_patch_installed = 1; /* Direct (hlavní menu, INI) */
    cmt_view_post_message("-", false);
    fill(&v);
    imgui_cmt_menu_set_cpu_boost(true);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, res.rc);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("-", v.message.c_str(), "CPU Boost with the hack on does not report a mode change");
    imgui_cmt_menu_set_cpu_boost(false);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_DISABLED, g_cmt.cpu_boost);
    TEST_ASSERT_EQUAL_STRING("-", v.message.c_str());

    /* bez hacku: výsledný režim */
    g_cmthack.load_patch_installed = 0;
    imgui_cmt_menu_set_cpu_boost(true);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("Loading mode: Boost", v.message.c_str());
    imgui_cmt_menu_set_cpu_boost(false);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("Loading mode: Tape", v.message.c_str());

    /* zásah okna (ne menu) s hackem: Direct se hlásí jako dřív */
    g_cmthack.load_patch_installed = 1;
    cmt_view_post_message("-", false);
    fill(&v);
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_DIRECT) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("Loading mode: Direct", v.message.c_str());
}

/**
 * @brief Okno posílá operace s číslem bloku s čítačem pásky a pauzu s podmínkou NOT_STOP.
 *
 * Rewind, Ctrl+Enter, Go to this block a Load directly = podmínka pásky;
 * dvojklik / Play from here (SELECT_FORCED) navíc NOT_PLAYING_ARG; Pause
 * při Playing = NOT_STOP.
 */
void test_ff_window_ops_carry_guards(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.tape_serial = 77u;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    en_REPLAY_CMT_OP op;
    int32_t arg;
    st_REPLAY_CMT_GUARD g;

    w3_drain();
    w2_click(&v, "btn_play"); /* Playing -> Pause */
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_PAUSE, op);
    TEST_ASSERT_EQUAL_INT(1, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_NOT_STOP, g.flags);

    w3_drain();
    w2_click(&v, "btn_rewind");
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_SEEK, op);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_TAPE, g.flags);
    TEST_ASSERT_EQUAL_UINT(77u, g.tape_serial);

    w3_drain();
    w3_double_click(&v, "name_0");
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_SELECT_FORCED, op);
    TEST_ASSERT_EQUAL_INT(0, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_TAPE | REPLAY_CMT_GUARD_NOT_PLAYING_ARG, g.flags);
    TEST_ASSERT_EQUAL_UINT(77u, g.tape_serial);

    /* Ctrl+Enter na řádku */
    w2_click(&v, "name_2");
    w3_drain();
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    w2_frame(&v);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    w2_frame(&v);
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_SEEK, op);
    TEST_ASSERT_EQUAL_INT(2, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_TAPE, g.flags);
    TEST_ASSERT_EQUAL_UINT(77u, g.tape_serial);

    /* kontextové menu ve STOP: Go to this block a Load directly */
    v.state = CMT_VIEW_STATE_STOP;
    v.motor_known = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_drain();
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_seek", 0);
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_SEEK, op);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_TAPE, g.flags);
    w3_close_popups(&v);
    w3_drain();
    w3_click_at(&v, "name_1", 1);
    w3_click_at(&v, "ctx_load", 0);
    /* Load directly nese čítač pásky v jediném zásahu fronty zařízení */
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    st_DEVICE_REQ_ARGS ld;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOAD_DIRECTLY_NAME, &ld));
    TEST_ASSERT_EQUAL_INT(1, ld.i[0]);
    TEST_ASSERT_EQUAL_INT(1, ld.i[4]);
    TEST_ASSERT_EQUAL_UINT(77u, (unsigned)ld.i[5]);
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Play při nahrávání čekajícím na motor nese podmínku RECORD; po přechodu do PLAY se zahodí.
 *
 * Nad skutečným jádrem: páska, cmt_record() (RECORD, čeká na motor), klik
 * na Play zařadí REPLAY_CMT_PAUSE 0 s REPLAY_CMT_GUARD_RECORD. Než ji emu
 * vlákno provede, deck přejde přes STOP do PLAY čekajícího na motor (jako
 * MCP stop + play): operace se zahodí a přehrávání dál čeká na program
 * (bez podmínky by cmt_pause(0) motor rozběhl).
 */
void test_ff_record_play_guarded(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    device_state_lock();
    int rc = cmt_record();
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_RECORD, v.state);
    TEST_ASSERT_TRUE(v.paused);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_drain();

    /* zařazená operace nese podmínku RECORD */
    w2_click(&v, "btn_play");
    en_REPLAY_CMT_OP op;
    int32_t arg;
    st_REPLAY_CMT_GUARD g;
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &g));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_PAUSE, op);
    TEST_ASSERT_EQUAL_INT(0, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_RECORD, g.flags);
    w3_drain();

    /* deck mezi kliknutím a provedením: RECORD -> STOP -> PLAY čekající na motor */
    w2_click(&v, "btn_play");
    device_state_lock();
    cmt_stop();
    cmt_play_paused();
    device_state_unlock();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    ff_run_ui_cmt();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PAUSED, "Play for the recording does not start a playback waiting for the program");
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Operace okna zahozená emu vláknem kvůli změně decku: varování v řádku zpráv.
 *
 * Nad skutečným jádrem, bez závislosti na reálném čase: (a) dvě operace
 * s podmínkou jiné pásky zahozené v jednom provedení fronty (mezi dvěma
 * voláními cmt_view_poll_dropped()) = jedno varování, další plnění ani další
 * volání ho neopakuje; (b) další zahozená operace hned potom (bez čekání)
 * dá další varování - žádné časové slučování; (c) provedená operace nic
 * nehlásí.
 */
void test_h0_dropped_op_warning_once(void)
{
    const char *dropped = "The tape state changed; the action was cancelled.";
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    cmt_view_post_message("-", false);
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("-", v.message.c_str());
    unsigned other = cmttmz_get_tape_serial() + 1u;
    st_REPLAY_CMT_GUARD g = ff_guard(REPLAY_CMT_GUARD_TAPE, other);

    /* (a) dvě zahozené operace najednou */
    unsigned d0 = cmt_request_get_window_drop_serial();
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 1, &g);
    replay_host_cmt_guarded(REPLAY_CMT_SELECT, 2, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_UINT(d0 + 2u, cmt_request_get_window_drop_serial());
    fill(&v);
    TEST_ASSERT_EQUAL_STRING(dropped, v.message.c_str());
    TEST_ASSERT_TRUE(v.message_is_warning);
    unsigned ps = v.post_serial;
    cmt_view_poll_dropped();
    fill(&v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(ps, v.post_serial, "the warning is shown once");

    /* (b) další zahozená operace hned potom: další varování (bez okna sloučení) */
    cmt_view_post_message("-", false);
    fill(&v);
    ps = v.post_serial;
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 1, &g);
    ff_run_ui_cmt();
    fill(&v);
    TEST_ASSERT_EQUAL_STRING(dropped, v.message.c_str());
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(ps, v.post_serial, "every dropped operation is reported");

    /* (c) provedená operace nic nehlásí */
    cmt_view_post_message("-", false);
    fill(&v);
    g = ff_guard(REPLAY_CMT_GUARD_TAPE, cmttmz_get_tape_serial());
    d0 = cmt_request_get_window_drop_serial();
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 1, &g);
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_open_block_id());
    TEST_ASSERT_EQUAL_UINT(d0, cmt_request_get_window_drop_serial());
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("-", v.message.c_str());
}

/**
 * @brief Výsledek zásahu Load directly z fronty výsledků (ostatní výsledky zahodí).
 * @param[out] res Výsledek. @return true = nalezen.
 */
static bool ff_take_load_directly_result(st_DEVICE_REQ_RESULT *res)
{
    bool found = false;
    st_DEVICE_REQ_RESULT r;
    while (device_request_take_result(&r)) {
        if (!found && strcmp(r.name, CMT_REQUEST_LOAD_DIRECTLY_NAME) == 0) {
            *res = r;
            found = true;
        }
    }
    return found;
}

/**
 * @brief Load directly je jeden zásah: vše, nebo nic.
 *
 * Nad skutečným jádrem, kliky v kontextovém menu řádku pásky A:
 * (a) páska vyměněná mezi zařazením a provedením (páska B se stejnými
 * indexy bloků): emu vlákno nezmění blok ani režim, výsledek
 * CMT_REQUEST_RC_TAPE_CHANGED, okno ukáže "The tape changed; the action
 * was cancelled."; ve frontě operací záznamu vstupů nic není (výběr bloku
 * už nejde zvlášť). (b) Záznam vstupů začal mezi zařazením a provedením:
 * zásah se odmítne celý (blok ani režim se nezmění). (c) Táž páska: blok
 * vybraný a Direct zapnutý jedním zásahem, zpráva "Loading mode: Direct".
 */
void test_ff_load_directly_tape_changed(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    open_games_tape();
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_STOP, v.state);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_TAPE, v.mode);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_drain();
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res)) {}

    /* (a) páska vyměněná mezi zařazením a provedením */
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_load", 0);
    w3_close_popups(&v);
    TEST_ASSERT_FALSE_MESSAGE(replay_test_take_ui_cmt(NULL, NULL), "Load directly does not queue a separate block selection");
    open_tape(make_games_tape(1)); /* páska B: tytéž indexy bloků, jiný čítač */
    TEST_ASSERT_NOT_EQUAL_UINT(v.tape_serial, cmttmz_get_tape_serial());
    device_request_poll();
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_get_open_block_id(), "the block of tape A is not selected on tape B");
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "Direct is not switched on for tape B");
    TEST_ASSERT_TRUE(ff_take_load_directly_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, res.rc);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("The tape changed; the action was cancelled.", v.message.c_str());
    TEST_ASSERT_TRUE(v.message_is_warning);

    /* (b) záznam vstupů začal dřív, než emu vlákno zásah provede: odmítnutý celý */
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_load", 0);
    w3_close_popups(&v);
    g_replay_mode = REPLAY_MODE_RECORDING;
    device_request_poll();
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_get_open_block_id(), "no block selection during input recording");
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "no Direct during input recording");
    TEST_ASSERT_TRUE(ff_take_load_directly_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);

    /* (c) táž páska: blok i Direct jedním zásahem */
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_load", 0);
    w3_close_popups(&v);
    device_request_poll();
    ff_run_ui_cmt();
    TEST_ASSERT_EQUAL_INT(2, cmttmz_get_open_block_id());
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_TRUE(ff_take_load_directly_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(1, res.rc);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING(cmt_view_loading_mode_message(CMT_LOADING_MODE_DIRECT), v.message.c_str());
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Skok na blok z klávesnice je Ctrl+Enter z řádku s fokusem navigace v zaostřeném okně (D-G3-16).
 *
 * (a) Bez fokusu navigace na řádku Ctrl+Enter nic; klik na řádek + samotný
 * Enter nic, Ctrl+Enter právě jeden skok. (b) Klik na řádek a pak klik do
 * jiného okna (hlavní okno obrazu): Ctrl+Enter nic. (c) Navigace klávesnicí
 * v tabulce funguje i s tlačítky decku mimo navigaci (D-G4-1): šipky jen
 * přesouvají výběr, Ctrl+Enter skočí na vybraný řádek.
 */
void test_g3f_enter_needs_table_focus(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    ImGuiIO &io = ImGui::GetIO();
    /* (a) okno zaostřené, žádný řádek nemá fokus navigace */
    ImGui::SetWindowFocus(W2_ID);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_drain();
    g3f_enter(&v, true);
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "Ctrl+Enter without a nav-focused row");
    /* klik na řádek + samotný Enter: nic; Ctrl+Enter: právě jeden skok */
    w2_click(&v, "name_2");
    w3_drain();
    g3f_enter(&v, false);
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "plain Enter on a clicked row");
    g3f_enter(&v, true);
    int32_t arg = -1;
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(2, arg);
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "exactly one seek request");

    /* (b) klik na řádek, klik do jiného okna, Enter */
    for (int i = 0; i < 2; i++) g3f_frame_with_other(&v);
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item("name_4");
    TEST_ASSERT_NOT_NULL(it);
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    io.AddMousePosEvent(c.x, c.y);
    g3f_frame_with_other(&v);
    io.AddMouseButtonEvent(0, true);
    g3f_frame_with_other(&v);
    io.AddMouseButtonEvent(0, false);
    g3f_frame_with_other(&v);
    TEST_ASSERT_EQUAL_INT(4, imgui_cmt_window_test_get_selection());
    io.AddMousePosEvent(1900.0f, 250.0f);
    g3f_frame_with_other(&v);
    io.AddMouseButtonEvent(0, true);
    g3f_frame_with_other(&v);
    io.AddMouseButtonEvent(0, false);
    g3f_frame_with_other(&v);
    w3_drain();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    g3f_frame_with_other(&v);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    g3f_frame_with_other(&v);
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "Ctrl+Enter after a click into another window");
    io.AddMousePosEvent(-1.0f, -1.0f);

    /* (c) navigace klávesnicí v tabulce: šipky jen přesouvají výběr */
    g3f_click_enter(&v, "name_1");
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(1, arg);
    for (int k = 0; k < 2; k++) {
        io.AddKeyEvent(ImGuiKey_DownArrow, true);
        w2_frame(&v);
        io.AddKeyEvent(ImGuiKey_DownArrow, false);
        w2_frame(&v);
    }
    w2_frame(&v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, imgui_cmt_window_test_get_selection(), "arrows move the selection in the table");
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "arrows do not move the tape");
    g3f_enter(&v, true);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(3, arg);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Minimum Full = deck + hlavička tabulky + 3 řádky + sbalená hlavička vlastností + řádek zpráv.
 *
 * Minimum nezávisí na rozbalených vlastnostech; okno jde zmenšit na minimum
 * bez vad geometrie a tabulka ukáže 3 řádky i s rozbalenými vlastnostmi
 * bloku MZF. Displej 768 px (pracovní plocha 1366 x 700 px): Full se vejde
 * při 100 % (3 řádky tabulky) i při 125 %.
 */
void test_g3f_full_min_height(void)
{
    st_CMT_VIEW v;
    w2_fonts();
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    imgui_cmt_window_test_set_selection(1, v.tape_serial); /* MZF: 5 řádků vlastností */
    for (int i = 0; i < 3; i++) w2_frame(&v);
    /* vlastnosti výchozí sbalené (D-G3-9) */
    TEST_ASSERT_EQUAL_UINT(0u, imgui_cmt_window_test_props_open());
    ImVec2 min_closed = imgui_cmt_window_test_min_size();
    w3_click_at(&v, "props_header", 0);
    TEST_ASSERT_EQUAL_UINT(1u, imgui_cmt_window_test_props_open());
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImVec2 min_open = imgui_cmt_window_test_min_size();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, min_closed.y, min_open.y, "the minimum does not depend on the properties");
    /* okno na minimum: 3 řádky tabulky, vlastnosti hlavička, bez vad */
    ImGui::SetWindowSize(W2_ID, min_open, ImGuiCond_Always);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    fprintf(stderr, "Full minimum at 100 %%: %.0f x %.0f px\n", min_open.x, min_open.y);
    TEST_ASSERT_TRUE(w2_window_size().y <= min_open.y + 0.5f);
    for (int r = 0; r < 3; r++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "num_%d", r);
        const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
        TEST_ASSERT_NOT_NULL_MESSAGE(it, nm);
        TEST_ASSERT_TRUE_MESSAGE(it->max.y <= it->clip_max.y + 0.5f, nm);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, w2_geometry_faults("Full at its minimum"), "Full at its minimum");

    /* displej 768 px: 100 % a 125 % */
    const float scales[2] = {1.0f, 1.25f};
    for (float sc : scales) {
        w2_apply_scale(sc);
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
        imgui_cmt_window_test_set_work_size(ImVec2(1366.0f, 700.0f));
        cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
        imgui_cmt_window_test_set_selection(1, v.tape_serial);
        for (int i = 0; i < 3; i++) w2_frame(&v);
        ImVec2 ws = w2_window_size();
        fprintf(stderr, "Full on a 1366 x 700 px work area at %.0f %%: %.0f x %.0f px\n", sc * 100.0f, ws.x, ws.y);
        TEST_ASSERT_TRUE_MESSAGE(ws.y <= 700.5f && ws.x <= 1366.5f, "Full fits a 768 px display");
        if (sc == 1.0f) {
            TEST_ASSERT_NOT_NULL(w2_item("num_2"));
            TEST_ASSERT_TRUE(w2_item("num_2")->max.y <= w2_item("num_2")->clip_max.y + 0.5f);
        }
        /* kontrola geometrie i při 125 % (vlastnosti sbalené i rozbalené) */
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "Full on 768 px at %.0f %%", sc * 100.0f);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, w2_geometry_faults(ctx), ctx);
        imgui_cmt_window_test_set_props_open(1u);
        for (int i = 0; i < 3; i++) w2_frame(&v);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, w2_geometry_faults(ctx), ctx);
    }
    imgui_cmt_window_test_set_work_size(ImVec2(0.0f, 0.0f));
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
}

/** @brief Šířka sloupce tabulky (požadovaná) podle indexu sloupce. */
static float g3f_col_width(int c)
{
    ImGuiTable *t = ImGui::TableFindByID(imgui_cmt_window_test_table_id());
    TEST_ASSERT_NOT_NULL(t);
    return t->Columns[c].WidthRequest;
}

/**
 * @brief Šířky sloupců: šířka uživatele přežije INI, jinou pásku i připojení záznamu; View > Reset column widths vrátí šířky podle obsahu (D-G3-19).
 */
void test_g3f_column_widths(void)
{
    const int type_col = 3; /* en_CW_COL: CW_COL_TYPE */
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    float content = g3f_col_width(type_col);
    TEST_ASSERT_TRUE(content > 10.0f);
    float min0 = imgui_cmt_window_test_min_size().x;
    /* uživatel sloupec rozšíří */
    ImGuiTable *t = ImGui::TableFindByID(imgui_cmt_window_test_table_id());
    t->Columns[type_col].WidthRequest = content + 60.0f;
    t->IsSettingsDirty = true;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    float user = g3f_col_width(type_col);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, content + 60.0f, user);
    /* minimum okna počítá se šířkou uživatele (Status se při minimu neořízne);
     * o 60 px je minimum dál dané deckem, o 400 px už tabulkou */
    t->Columns[type_col].WidthRequest = content + 400.0f;
    t->IsSettingsDirty = true;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    float min_wide = imgui_cmt_window_test_min_size().x;
    fprintf(stderr, "Full minimum width: %.0f px, after widening Type by 400 px %.0f px\n", min0, min_wide);
    TEST_ASSERT_TRUE_MESSAGE(min_wide > min0 + 100.0f, "the minimum width includes the user's column width");
    ImGui::SetWindowSize(W2_ID, ImVec2(min_wide, 23.0f * ImGui::GetFrameHeight()), ImGuiCond_Always);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, w2_geometry_faults("Full at the minimum width with a widened column"), "widened column at the minimum width");
    TEST_ASSERT_FALSE(imgui_cmt_window_test_table_scroll_x());
    t->Columns[type_col].WidthRequest = content + 60.0f;
    t->IsSettingsDirty = true;
    ImGui::SetWindowSize(W2_ID, ImVec2(30.0f * ImGui::GetFrameHeight(), 23.0f * ImGui::GetFrameHeight()), ImGuiCond_Always);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    /* restart: INI ImGui */
    size_t ini_size = 0;
    std::string ini(ImGui::SaveIniSettingsToMemory(&ini_size));
    ImGui::TableResetSettings(t);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    ImGui::TableLoadSettings(t);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, user, g3f_col_width(type_col), "the width survives the ini");
    /* jiná páska a připojení záznamu šířku nepřepíšou */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    v.tape_serial = 7;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    v.change_serial++;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, user, g3f_col_width(type_col), "another tape keeps the user's width");
    /* View > Reset column widths */
    w3_click_at(&v, "menu_view", 0);
    TEST_ASSERT_NOT_NULL(w2_item("menu_reset_widths"));
    w3_click_at(&v, "menu_reset_widths", 0);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, content, g3f_col_width(type_col), "reset restores the content width");
    w3_close_popups(&v);
    imgui_cmt_window_test_reset();
}


/* ================================================================
 * Celé řádky ve Full, prázdná páska, titulek, zprávy s cestou
 * ================================================================ */

/**
 * @brief Výška řádku tabulky z posledního snímku (cw_row_h(): řádek textu + 4 px škálované písmem).
 *
 * Bere se z obdélníků řádků (Selectable přes celý řádek), nejmenší výška:
 * poslední řádek je o 1 px vyšší. Mimo snímek ImGui GetFontSize() měřítko
 * písma nezná. Bez řádků výška ze stylu (FontSizeBase x FontScaleMain).
 *
 * @return Výška v px.
 */
static float g6_row_h(void)
{
    float h = 0.0f;
    for (const st_CMT_WINDOW_TEST_ITEM &x : imgui_cmt_window_test_items())
        if (x.scroll_y && x.container && x.name.compare(0, 4, "row_") == 0) {
            float rh = x.max.y - x.min.y;
            if (h == 0.0f || rh < h) h = rh;
        }
    if (h > 0.0f) return h;
    float fs = ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleMain;
    return fs + fs / 7.0f;
}

/**
 * @brief Počet řádků tabulky (buňka "#"), které jsou v posledním snímku vidět celé.
 * @param first První index bloku. @param last Poslední index bloku. @return Počet.
 */
static int g6_whole_rows(int first, int last)
{
    int n = 0;
    for (int i = first; i <= last; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "num_%d", i);
        const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
        if (it && it->min.y >= it->clip_min.y - 0.5f && it->max.y <= it->clip_max.y + 0.5f) n++;
    }
    return n;
}

/**
 * @brief Výška viditelného těla tabulky je celý počet řádků (ořez řádků tabulky z posledního snímku).
 * @param ctx Popis varianty. @return 0 = celý počet řádků (nebo tabulka bez řádků), 1 = vada (na stderr).
 */
static int g6_table_body_fault(const char *ctx)
{
    const st_CMT_WINDOW_TEST_ITEM *it = NULL;
    for (const st_CMT_WINDOW_TEST_ITEM &x : imgui_cmt_window_test_items())
        if (x.scroll_y && !x.popup && x.name.compare(0, 4, "num_") == 0) { it = &x; break; }
    if (!it) return 0;
    /* ořez těla začíná 1 px pod čarou hlavičky (první pixel řádku leží pod ní) */
    float body = it->clip_max.y - it->clip_min.y + 1.0f;
    float rows = body / g6_row_h();
    if (fabsf(rows - roundf(rows)) * g6_row_h() > 0.5f) {
        fprintf(stderr, "%s: table body %.1f px is not a whole number of %.1f px rows\n", ctx, body, g6_row_h());
        return 1;
    }
    return 0;
}

/**
 * @brief Full bez useknutých řádků: vlastnosti výchozí sbalené, tělo tabulky a vlastností jen celé řádky.
 *
 * (a) Výchozí Full při 100 %: vlastnosti sbalené, aspoň 5 celých řádků tabulky.
 * (b) Pro 100 %, 125 % a 175 %, vlastnosti sbalené i rozbalené a několik stavů
 *     (výběr surového bloku a bloku MZF, 300 bloků odrolovaných na blok 151,
 *     záznam, prázdná páska) a výšku okna od minima po krocích 3 px: žádná
 *     vada geometrie včetně napůl viditelného řádku, tělo tabulky celý počet
 *     řádků. (c) Rozbalené vlastnosti, které se vejdou, nemají posuvník.
 * (d) Kolečko myši nad tabulkou 300 bloků posune po celých řádcích.
 */
void test_g6_full_whole_rows(void)
{
    st_CMT_VIEW v;
    w2_fonts();

    /* (a) výchozí Full při 100 % */
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, imgui_cmt_window_test_props_open(), "properties are collapsed by default");
    int whole = g6_whole_rows(0, 4);
    fprintf(stderr, "default Full at 100 %%: %.0f x %.0f px, %d whole table rows\n", w2_window_size().x, w2_window_size().y, whole);
    TEST_ASSERT_TRUE_MESSAGE(whole >= 5, "default Full shows at least 5 whole table rows");
    TEST_ASSERT_EQUAL_INT(0, w2_geometry_faults("default Full"));
    TEST_ASSERT_EQUAL_INT(0, g6_table_body_fault("default Full"));

    /* (b) výšky okna od minima, tři měřítka, vlastnosti sbalené i rozbalené */
    struct { en_CMT_VIEW_FIXTURE f; int sel; const char *name; } cases[] = {
        {CMT_VIEW_FIXTURE_PLAYING_FULL, 3, "raw selected"},
        {CMT_VIEW_FIXTURE_PLAYING_FULL, 1, "MZF selected"},
        {CMT_VIEW_FIXTURE_MANY_BLOCKS, -1, "300 blocks"},
        {CMT_VIEW_FIXTURE_RECORDING, -1, "recording"},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, -1, "empty"},
    };
    const float scales[3] = {1.0f, 1.25f, 1.75f};
    int faults = 0;
    for (float sc : scales) {
        w2_apply_scale(sc);
        for (unsigned po = 0; po < 2; po++) {
            for (auto &c : cases) {
                imgui_cmt_window_test_reset();
                imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
                imgui_cmt_window_test_set_props_open(po);
                cmt_view_make_fixture(&v, c.f);
                imgui_cmt_window_test_set_selection(c.sel, v.tape_serial);
                for (int i = 0; i < 3; i++) w2_frame(&v);
                ImVec2 min = imgui_cmt_window_test_min_size();
                ImVec2 ws = w2_window_size();
                float span = 3.0f * g6_row_h();
                for (float dh = 0.0f; dh <= span; dh += 3.0f) {
                    ImGui::SetWindowSize(W2_ID, ImVec2(ws.x, min.y + dh), ImGuiCond_Always);
                    for (int i = 0; i < 3; i++) w2_frame(&v);
                    char ctx[128];
                    snprintf(ctx, sizeof(ctx), "scale %.2f props %s %s height %.0f", sc, po ? "open" : "closed", c.name, min.y + dh);
                    faults += w2_geometry_faults(ctx);
                    faults += g6_table_body_fault(ctx);
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, faults, "half rows or clipped items in Full (details on stderr)");

    /* (c) rozbalené vlastnosti, které se vejdou: bez posuvníku */
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    imgui_cmt_window_test_set_props_open(1u);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    imgui_cmt_window_test_set_selection(3, v.tape_serial);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    /* ve výchozí výšce se tři řádky surového bloku vedle 3 řádků tabulky nevejdou:
     * celé řádky a posuvník; o 3 řádky vyšší okno je pojme všechny */
    TEST_ASSERT_TRUE(imgui_cmt_window_test_props_scrollbar());
    ImVec2 dws = w2_window_size();
    ImGui::SetWindowSize(W2_ID, ImVec2(dws.x, dws.y + 3.0f * g6_row_h()), ImGuiCond_Always);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_props_scrollbar(), "expanded properties that fit have no scrollbar");
    for (int i = 0; i < 5; i++) {
        char nm[24];
        snprintf(nm, sizeof(nm), "prop_label_%d", i);
        const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
        TEST_ASSERT_NOT_NULL_MESSAGE(it, nm);
        TEST_ASSERT_TRUE_MESSAGE(it->max.y <= it->clip_max.y + 0.5f, nm);
    }

    /* (d) kolečko myši nad tabulkou 300 bloků: posun po celých řádcích */
    imgui_cmt_window_test_set_props_open(0u);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    v.tape_serial = 9;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    const st_CMT_WINDOW_TEST_ITEM *r = w2_item("name_150");
    TEST_ASSERT_NOT_NULL(r);
    float y150 = r->min.y;
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(r->min.x + 4.0f, r->min.y + 4.0f);
    w2_frame(&v);
    io.AddMouseWheelEvent(0.0f, -1.0f);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    io.AddMousePosEvent(-1.0f, -1.0f);
    w2_frame(&v);
    r = w2_item("name_150");
    TEST_ASSERT_TRUE_MESSAGE(!r || r->min.y < y150 - 1.0f, "the wheel scrolls the table");
    TEST_ASSERT_EQUAL_INT(0, w2_geometry_faults("after the mouse wheel"));
    imgui_cmt_window_test_reset();
}

/**
 * @brief Texty okna: titulek s dlouhým jménem, zpráva s cestou, pevná rychlost v rámečku, počet bloků při záznamu, řídicí blok, zprávy testovacích stavů.
 *
 * (a) Titulek s dlouhým jménem pásky se vejde do titulkového pruhu
 *     (nejméně 2 x ItemInnerSpacing před křížkem) a jméno končí elipsou,
 *     hvězdička Modified zůstane. (b) Zpráva s cestou ukáže jméno
 *     souboru. (c) Pevná rychlost v Direct je v rámečku napravo od
 *     popisku. (d) Vlastnosti pásky při záznamu počítají nahrávaný
 *     blok jako deck. (e) Řídicí blok má ve sloupci Name přeložený druh,
 *     ne anglický typ knihovny. (f) Zprávy testovacích stavů jsou
 *     skutečné texty aplikace. (g) Výběr řádku je neprůhledný.
 */
void test_g6_texts(void)
{
    st_CMT_VIEW v;
    w2_fonts();
    w2_apply_scale(1.0f);

    /* (a) titulek a (b) zpráva s cestou, Compact s dlouhými texty */
    imgui_cmt_window_test_reset();
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_LONG_TEXTS);
    for (int i = 0; i < 4; i++) w2_frame(&v);
    std::string title(imgui_cmt_window_test_title());
    title = title.substr(0, title.find("###"));
    ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
    TEST_ASSERT_NOT_NULL(w);
    const ImGuiStyle &st = ImGui::GetStyle();
    float avail = w->Size.x - 2.0f * (st.FramePadding.x + ImGui::GetFontSize() + 2.0f * st.ItemInnerSpacing.x);
    float tw = ImGui::CalcTextSize(title.c_str()).x;
    fprintf(stderr, "long title: \"%s\" %.0f px of %.0f px\n", title.c_str(), tw, avail);
    TEST_ASSERT_TRUE_MESSAGE(tw <= avail + 0.5f, "the title fits the title bar");
    TEST_ASSERT_TRUE_MESSAGE(title.size() > 5 && title.compare(title.size() - 5, 5, "... *") == 0, title.c_str());
    std::string msg = w2_text("message");
    fprintf(stderr, "message with a path: \"%s\"\n", msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(msg.find("games_backup_copy_number_two.tmz") != std::string::npos, "the file name of a path stays visible");
    TEST_ASSERT_TRUE_MESSAGE(msg.find("...") != std::string::npos, "the message is shortened");

    /* (c) Direct: Signal speed a hodnota v rámečku */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_DIRECT);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const st_CMT_WINDOW_TEST_ITEM *lbl = w2_item("speed_label");
    const st_CMT_WINDOW_TEST_ITEM *frm = w2_item("speed_frame");
    const st_CMT_WINDOW_TEST_ITEM *txt = w2_item("speed_text");
    TEST_ASSERT_NOT_NULL(lbl);
    TEST_ASSERT_NOT_NULL_MESSAGE(frm, "fixed speed is shown in a frame");
    TEST_ASSERT_NOT_NULL(txt);
    TEST_ASSERT_TRUE(frm->min.x > lbl->max.x);
    TEST_ASSERT_TRUE(txt->min.x > frm->min.x && txt->max.x <= frm->max.x + 0.5f);
    /* (f) zpráva testovacího stavu = skutečný text přepnutí režimu */
    TEST_ASSERT_EQUAL_STRING("Loading mode: Direct", w2_text("message").c_str());

    /* (d) záznam: deck "Block 6 / 6" a vlastnosti pásky Blocks 6 */
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    imgui_cmt_window_test_set_props_open(1u);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Block 6 / 6", w2_text("block_line").c_str());
    TEST_ASSERT_EQUAL_STRING("Blocks", w2_text("prop_label_1").c_str());
    TEST_ASSERT_EQUAL_STRING("6", w2_text("prop_value_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Opened games.tmz, blocks: 5", w2_text("message").c_str());

    /* (e) řídicí blok: Name přeložený druh, ne "Loop Start" */
    imgui_cmt_window_test_set_props_open(0u);
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Control block", w2_text("name_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Loop start x3", w2_text("status_1").c_str());
    TEST_ASSERT_EQUAL_STRING("Opened loops.tzx, blocks: 7", w2_text("message").c_str());

    /* (f) čekání na motor: zpráva otevření skutečným formátem */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_WAITING_MOTOR);
    TEST_ASSERT_EQUAL_STRING("Opened games.tmz, blocks: 5", v.message.c_str());

    /* (g) výběr řádku neprůhledný: na zebře i panelu stejná barva (kontrast) */
    TEST_ASSERT_EQUAL_HEX32(0xFFu, (CMT_TOK_ROW_SELECTED >> IM_COL32_A_SHIFT) & 0xFFu);
    imgui_cmt_window_test_reset();
}

/* ================================================================
 * Režimy načítání, zkratky, řádek zpráv, menu Tape
 * ================================================================ */

/** @brief Viditelnost okna po posledním g4_frame() (zkratka Alt+C, Escape). */
static bool s_g4_open = true;

/**
 * @brief Snímek okna s vlastním příznakem viditelnosti (jako main_window.cpp: okno se kreslí jen otevřené).
 * @param v Kopie stavu.
 */
static void g4_frame(st_CMT_VIEW *v)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (ImGui::FindWindowByName(W2_ID)) ImGui::SetWindowPos(W2_ID, ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    device_state_lock();
    if (s_g4_open) imgui_cmt_window_draw(v, &s_g4_open);
    device_state_unlock();
    ImGui::Render();
}

/**
 * @brief Stisk a uvolnění klávesy (volitelně s Ctrl nebo Alt), dva snímky, pak akce automatu pásky po uvolnění zámku.
 * @param v Kopie. @param key Klávesa. @param mod ImGuiMod_Ctrl, ImGuiMod_Alt nebo ImGuiMod_None.
 */
static void g4_key(st_CMT_VIEW *v, ImGuiKey key, ImGuiKey mod)
{
    ImGuiIO &io = ImGui::GetIO();
    if (mod != ImGuiMod_None) io.AddKeyEvent(mod, true);
    io.AddKeyEvent(key, true);
    g4_frame(v);
    io.AddKeyEvent(key, false);
    if (mod != ImGuiMod_None) io.AddKeyEvent(mod, false);
    g4_frame(v);
    imgui_cmt_tape_run_deferred();
}

/** @brief Fronty bez požadavku (operace kazety, zásahy, akce automatu, export, Save As). @return true = nic nezařazeno. */
static bool g4_nothing_queued(void)
{
    bool none = !replay_test_take_ui_cmt(NULL, NULL) && !device_request_test_take_pending(NULL, NULL);
    none = none && (imgui_cmt_tape_test_take_last_run() == IMGUI_CMT_TAPE_ACTION_NONE);
    none = none && !imgui_cmt_tape_test_take_last_export(NULL, NULL);
    none = none && (imgui_cmt_tape_test_save_as_open() == 0);
    return none;
}

/** @brief Připraví okno Compact s testovacím stavem, se zaostřeným oknem a prázdnými frontami. */
static void g4_setup(st_CMT_VIEW *v, en_CMT_VIEW_FIXTURE f)
{
    w3_setup(v, f);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    s_g4_open = true;
    for (int i = 0; i < 3; i++) g4_frame(v);
    ImGui::SetWindowFocus(W2_ID);
    for (int i = 0; i < 2; i++) g4_frame(v);
    imgui_cmt_tape_test_reset();
    w3_drain();
    (void)imgui_cmt_tape_test_take_last_run();
}

/**
 * @brief Při fokusu okna klávesy bez Ctrl nic nespustí, s Ctrl vydají požadavek tlačítka.
 *
 * Klávesy Tab, šipky, R, S, mezerník, Enter, Home, tečka, PageUp, PageDown,
 * O, E bez Ctrl: žádná operace kazety, zásah, akce automatu ani dialog
 * (ani přes navigaci klávesnicí na tlačítko decku). S Ctrl: Play /
 * Pause, Stop, Rewind, Previous / Next, Record (ve STOP), Open...,
 * Save (dialog Save As u pásky bez souboru), Eject. Bez fokusu okna
 * Ctrl+mezerník nic. Escape okno nezavře, Alt+C ano. Zakázaná akce ani
 * se zkratkou nic nevydá.
 */
void test_g4_shortcuts(void)
{
    st_CMT_VIEW v;
    g4_setup(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    /* Tab a šipky by bez zákazu navigace přesunuly fokus navigace na tlačítko
     * decku a mezerník / Enter by ho stiskly (D-G4-1) */
    const ImGuiKey plain[] = {ImGuiKey_Tab, ImGuiKey_DownArrow, ImGuiKey_RightArrow, ImGuiKey_R, ImGuiKey_S, ImGuiKey_Space,
                              ImGuiKey_Enter, ImGuiKey_Home, ImGuiKey_Period, ImGuiKey_PageUp, ImGuiKey_PageDown,
                              ImGuiKey_O, ImGuiKey_E, ImGuiKey_Space};
    for (ImGuiKey k : plain) {
        g4_key(&v, k, ImGuiMod_None);
        TEST_ASSERT_TRUE_MESSAGE(g4_nothing_queued(), ImGui::GetKeyName(k));
    }
    int32_t arg = -1;
    g4_key(&v, ImGuiKey_Space, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PAUSE, &arg));
    TEST_ASSERT_EQUAL_INT(1, arg);
    g4_key(&v, ImGuiKey_Period, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_STOP_BUTTON, NULL));
    g4_key(&v, ImGuiKey_Home, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    TEST_ASSERT_EQUAL_INT(0, arg);
    g4_key(&v, ImGuiKey_PageUp, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PREV, NULL));
    g4_key(&v, ImGuiKey_PageDown, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_NEXT, NULL));
    /* Record při PLAY zakázaný: ani zkratka nic */
    g4_key(&v, ImGuiKey_R, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(g4_nothing_queued());
    /* Eject (páska jádra nemá změny: akce hned) */
    g4_key(&v, ImGuiKey_E, ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_EJECT, imgui_cmt_tape_test_take_last_run());
    w3_drain();
    g4_key(&v, ImGuiKey_O, ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_OPEN, imgui_cmt_tape_test_take_last_run());
    w3_drain();

    /* Paused: Ctrl+mezerník pauzu zruší */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PAUSED);
    for (int i = 0; i < 2; i++) g4_frame(&v);
    g4_key(&v, ImGuiKey_Space, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PAUSE, &arg));
    TEST_ASSERT_EQUAL_INT(0, arg);

    /* STOP: Ctrl+R nahrává, Ctrl+S uloží (páska bez souboru: dialog Save tape as) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    for (int i = 0; i < 2; i++) g4_frame(&v);
    g4_key(&v, ImGuiKey_R, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_RECORD, NULL));
    g4_key(&v, ImGuiKey_S, ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_save_as_open());
    imgui_cmt_tape_test_save_as_finish(NULL);
    w3_drain();

    /* Escape okno nezavře */
    g4_key(&v, ImGuiKey_Escape, ImGuiMod_None);
    TEST_ASSERT_TRUE_MESSAGE(s_g4_open, "Escape must not close the window");

    /* bez fokusu okna: nic */
    ImGui::SetWindowFocus(NULL);
    for (int i = 0; i < 2; i++) g4_frame(&v);
    g4_key(&v, ImGuiKey_Space, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE_MESSAGE(g4_nothing_queued(), "shortcuts only while the window has focus");

    /* Alt+C při fokusu okna okno zavře (globální zkratka běží jen při fokusu hlavního okna) */
    ImGui::SetWindowFocus(W2_ID);
    for (int i = 0; i < 2; i++) g4_frame(&v);
    g4_key(&v, ImGuiKey_C, ImGuiMod_Alt);
    TEST_ASSERT_FALSE_MESSAGE(s_g4_open, "Alt+C closes the window");

    /* Alt+C při fokusu hlavního okna: globální zkratka okno otevře a to ve
     * stejném snímku dostane fokus; tentýž stisk ho nesmí hned zavřít */
    for (int i = 0; i < 2; i++) g4_frame(&v);
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Alt, true);
        io.AddKeyEvent(ImGuiKey_C, true);
        s_g4_open = true; /* jako global_shortcuts.cpp v tomto snímku */
        g4_frame(&v);
        TEST_ASSERT_TRUE_MESSAGE(s_g4_open, "Alt+C that opened the window must not close it in the same frame");
        g4_frame(&v);
        TEST_ASSERT_TRUE_MESSAGE(s_g4_open, "the window stays open while Alt+C is held");
        io.AddKeyEvent(ImGuiKey_C, false);
        io.AddKeyEvent(ImGuiMod_Alt, false);
        g4_frame(&v);
        TEST_ASSERT_TRUE_MESSAGE(s_g4_open, "the window stays open after Alt+C is released");
    }
    /* další stisk při fokusu okna ho zavře */
    g4_key(&v, ImGuiKey_C, ImGuiMod_Alt);
    TEST_ASSERT_FALSE_MESSAGE(s_g4_open, "the next Alt+C closes the window");
    s_g4_open = true;
    imgui_cmt_tape_test_reset();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Přepínač Loading: každá volba vydá jeden zásah režimu; tabulka režim x prvek; zákazy při PLAY, RECORD a záznamu vstupů.
 */
void test_g4_mode_switch(void)
{
    st_CMT_VIEW v;
    st_DEVICE_REQ_ARGS a;
    g4_setup(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT); /* Tape, PLAY */
    TEST_ASSERT_TRUE(w2_item("seg_tape") != NULL);
    /* Tape -> Boost jde i při PLAY */
    w2_click(&v, "seg_boost");
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, a.i[0]);
    /* Direct při PLAY ne */
    TEST_ASSERT_TRUE(w2_item("seg_direct")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w2_item("seg_direct")->reason.c_str());
    v.mode = CMT_VIEW_MODE_BOOST;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w2_click(&v, "seg_tape");
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, a.i[0]);

    /* STOP: Direct */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w2_click(&v, "seg_direct");
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, a.i[0]);

    /* Direct: Play / Pause, Stop a Signal speed "Not used in Direct mode", Record povolený; posun povolený */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_DIRECT);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const char *r = "Not used in Direct mode";
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);
    TEST_ASSERT_EQUAL_STRING(r, w2_item("btn_play")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING(r, w2_item("btn_stop")->reason.c_str());
    TEST_ASSERT_FALSE(w2_item("btn_record")->disabled);
    TEST_ASSERT_EQUAL_STRING(r, w2_text("speed_text").c_str());
    TEST_ASSERT_FALSE(w2_item("btn_rewind")->disabled);
    TEST_ASSERT_FALSE(w2_item("btn_next")->disabled);
    TEST_ASSERT_EQUAL_STRING("--:--.-", w2_text("counter").c_str());
    TEST_ASSERT_EQUAL_STRING("Next LOAD takes this block", w2_text("direct_hint").c_str());
    w2_click(&v, "seg_boost");
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, a.i[0]);

    /* RECORD: Direct ne (hack by pásku nepoužil), Boost ano */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("seg_direct")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the recording first.", w2_item("seg_direct")->reason.c_str());
    w2_click(&v, "seg_boost");
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, a.i[0]);

    /* záznam vstupů: přepínač zamčený (zásah by se odmítl) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    v.replay_recording = true;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("seg_boost")->disabled);
    TEST_ASSERT_EQUAL_STRING("Not available while an input replay is recorded.", w2_item("seg_boost")->reason.c_str());
    TEST_ASSERT_TRUE(w2_item("seg_direct")->disabled);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief ROM bez přímého načtení (!st_CMT_VIEW::direct_supported): segment Direct a Load directly zakázané s důvodem.
 *
 * S ROM, která přímé načtení umí, jsou obě povolené. Bez podpory: obě
 * zakázané s týmž důvodem, klik nic nezařadí, Tape, Boost a ostatní
 * položky menu beze změny. Pořadí důvodů: přehrávání záznamu vstupů (zámek
 * celého okna) první; v menu blok bez MZF před ROM (na takovém řádku Load
 * directly nejde nikdy); ROM před přechodnými stavy (hrající páska, záznam
 * vstupů), protože jejich odstranění by nepomohlo.
 */
void test_g4_direct_unsupported_rom(void)
{
    st_CMT_VIEW v;
    st_DEVICE_REQ_ARGS a;
    const char *r_rom = "The selected ROM does not support direct loading.";
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    for (int i = 0; i < 2; i++) w2_frame(&v);

    /* ROM s přímým načtením: beze změny */
    TEST_ASSERT_TRUE(v.direct_supported);
    TEST_ASSERT_FALSE(w2_item("seg_direct")->disabled);
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_load")->disabled);
    w3_close_popups(&v);
    w3_drain();

    /* ROM bez přímého načtení, STOP */
    v.direct_supported = false;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("seg_direct")->disabled);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("seg_direct")->reason.c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("seg_boost")->disabled, "Boost does not depend on the ROM");
    TEST_ASSERT_FALSE_MESSAGE(w2_item("seg_tape")->disabled, "Tape does not depend on the ROM");
    w2_click(&v, "seg_direct");
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a), "the disabled Direct segment queues nothing");
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_load")->disabled);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("ctx_load")->reason.c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("ctx_play")->disabled, "Play from here does not depend on the ROM");
    TEST_ASSERT_FALSE_MESSAGE(w2_item("ctx_export_mzf")->disabled, "Export as MZF does not depend on the ROM");
    w3_click_at(&v, "ctx_load", 0);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_LOAD_DIRECTLY_NAME, &a), "the disabled Load directly queues nothing");
    w3_close_popups(&v);
    /* blok bez MZF (surový blok, index 3): důvod bloku má přednost */
    w3_click_at(&v, "name_3", 1);
    TEST_ASSERT_EQUAL_STRING("This block does not contain an MZF file.", w2_item("ctx_load")->reason.c_str());
    w3_close_popups(&v);

    /* hrající páska: ROM před "Stop the tape first." (zastavení by nepomohlo) */
    v.state = CMT_VIEW_STATE_PLAY;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("seg_direct")->reason.c_str());
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("ctx_load")->reason.c_str());
    w3_close_popups(&v);

    /* záznam vstupů: ROM před zámkem záznamu, Boost má důvod záznamu */
    v.state = CMT_VIEW_STATE_STOP;
    v.replay_recording = true;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("seg_direct")->reason.c_str());
    TEST_ASSERT_EQUAL_STRING("Not available while an input replay is recorded.", w2_item("seg_boost")->reason.c_str());
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_EQUAL_STRING(r_rom, w2_item("ctx_load")->reason.c_str());
    w3_close_popups(&v);

    /* přehrávání záznamu vstupů zamyká celé okno: jeho důvod je první */
    v.replay_recording = false;
    v.replay_playing = true;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Not available while an input replay is played.", w2_item("seg_direct")->reason.c_str());
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_EQUAL_STRING("Not available while an input replay is played.", w2_item("ctx_load")->reason.c_str());
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Zásah režimu nad skutečným jádrem: Boost a Tape ve STOP i PLAY, Direct mimo STOP odmítnut; kopie ukazuje výsledný režim.
 *
 * Kombinace CMT hack + CPU Boost (hlavní menu, INI) se ukáže jako Direct.
 */
void test_g4_mode_request_core(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_TAPE, v.mode);
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_BOOST) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, res.rc);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_BOOST, v.mode);
    TEST_ASSERT_EQUAL_STRING("Loading mode: Boost", v.message.c_str());
    TEST_ASSERT_FALSE(v.message_is_error);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);

    /* Direct při PLAY: odmítnuto, nic se nezmění, chyba v řádku zpráv */
    device_state_lock();
    cmt_play();
    device_state_unlock();
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_DIRECT) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_BOOST, v.mode);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_TRUE(v.message_is_error);
    TEST_ASSERT_EQUAL_STRING("Stop the tape to switch to or from Direct mode.", v.message.c_str());

    /* Boost -> Tape jde i při PLAY */
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_TAPE) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_TAPE, v.mode);
    TEST_ASSERT_EQUAL_STRING("Loading mode: Tape", v.message.c_str());

    /* odmítnutí při záznamu vstupů: jen řádek zpráv, žádné společné okno zpráv */
    res.status = DEVICE_REQ_REJECTED_REPLAY;
    res.rc = 0;
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_TRUE(v.message_is_error);
    TEST_ASSERT_EQUAL_STRING("The loading mode cannot be changed while an input replay is recorded or played.", v.message.c_str());

    /* hack + Boost z jiného vstupu (hlavní menu, INI): Direct */
    g_cmthack.load_patch_installed = 1;
    g_cmt.cpu_boost = CMT_CPU_BOOST_ENABLED;
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);
}

/**
 * @brief Výsledky uložení a exportu jdou do řádku zpráv (ne do okna zpráv); výsledek záznamu jen jednou.
 */
void test_g4_message_line(void)
{
    imgui_message_test_clear();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    st_DEVICE_REQ_RESULT res;
    memset(&res, 0, sizeof(res));
    res.name = "cmt_save";
    res.status = DEVICE_REQ_DONE;
    res.rc = -1;
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_TRUE(v.message_is_error);
    TEST_ASSERT_TRUE(g_str_has_prefix(v.message.c_str(), "The tape could not be saved."));
    res.name = "cmt_export_block";
    res.status = DEVICE_REQ_REJECTED_REPLAY;
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("A block cannot be exported while an input replay is recorded or played.", v.message.c_str());
    res.name = "cmt_set_speed";
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("The signal speed cannot be changed while an input replay is recorded or played.", v.message.c_str());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, imgui_message_test_queued(), "results go only to the message line");

    /* poslední zpráva zůstává, dokud ji nenahradí další */
    for (int i = 0; i < 3; i++) fill(&v);
    TEST_ASSERT_EQUAL_STRING("The signal speed cannot be changed while an input replay is recorded or played.", v.message.c_str());

    /* otevření pásky: jméno a počet bloků */
    open_games_tape();
    res.name = "cmt_open";
    res.status = DEVICE_REQ_DONE;
    res.rc = 0;
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("Opened games.tmz, blocks: 5", v.message.c_str());
    TEST_ASSERT_FALSE(v.message_is_error);
}

/* ================================================================
 * Direct, skryté okno, zásah režimu
 * ================================================================ */

/**
 * @brief Direct s běžícím transportem (Fast Open & Play, MCP play s hackem): Stop povolený, po Stop jde přepnout režim.
 *
 * Nad skutečným jádrem: hack zapnutý, páska spuštěná jádrem. Okno nabídne
 * Stop (Play a Record zůstávají při PLAY zakázané), Tape / Boost jsou
 * zakázané do zastavení; po Stop Tape vydá zásah režimu. Ve STOP v Direct je
 * Record povolený. Nahrávání v Direct: okno ukazuje stav decku (Waiting for
 * motor, po povelu motoru programu Recording a Motor on), Record (= Stop
 * recording) povolený.
 */
void test_g4f_direct_running_tape_stop(void)
{
    open_games_tape();
    device_state_lock();
    g_cmthack.load_patch_installed = 1;
    cmt_play();
    device_state_unlock();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATE_PLAY, v.state);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_COMPACT);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_stop")->disabled, "Stop works in Direct while the tape runs");
    TEST_ASSERT_TRUE(w2_item("btn_play")->disabled);
    TEST_ASSERT_TRUE(w2_item("seg_tape")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w2_item("seg_tape")->reason.c_str());
    w3_drain();
    w2_click(&v, "btn_stop");
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_STOP_BUTTON, NULL));

    /* emu vlákno provede Stop; pak Stop zakázaný a Tape / Boost povolené */
    device_state_lock();
    cmt_stop_button();
    device_state_unlock();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w2_item("btn_stop")->disabled);
    TEST_ASSERT_EQUAL_STRING("Not used in Direct mode", w2_item("btn_stop")->reason.c_str());
    TEST_ASSERT_FALSE(w2_item("seg_tape")->disabled);
    TEST_ASSERT_FALSE(w2_item("seg_boost")->disabled);
    w3_drain();
    w2_click(&v, "seg_tape");
    const char *name = NULL;
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &a));
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LOADING_MODE_NAME, name);
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, a.i[0]);

    /* STOP v Direct: Record povolený */
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_record")->disabled, "Record works in Direct with the tape stopped");

    /* nahrávání v Direct: stav decku, Record = Stop recording povolený */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    device_state_unlock();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Waiting for motor", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor off", w2_text("motor_text").c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("btn_record")->disabled, "Stop recording works in Direct");
    TEST_ASSERT_FALSE(w2_item("btn_stop")->disabled);
    /* povel motoru programu (hrana PC3 jako ROM @MGON) nahrávání rozjede i s hackem */
    device_state_lock();
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07);
    device_state_unlock();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Recording", w2_text("status_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Motor on", w2_text("motor_text").c_str());
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Zprávy při skrytém okně (zavřeném, sbaleném): chyba a varování modálně, informace se zahodí; zahozené nahrávání = varování; Alt+C zavře i sbalené okno.
 */
void test_g4f_messages_hidden_window(void)
{
    imgui_message_test_clear();
    cmt_view_test_force_window_shown(-1);
    st_CMT_VIEW v;
    g4_setup(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    st_CMT_VIEW mv;
    cmt_view_reset(&mv);
    fill(&mv);
    TEST_ASSERT_TRUE(cmt_view_window_shown());

    /* okno vidět: řádek zpráv, žádné okno zpráv */
    cmt_view_post_message("shown error", true);
    fill(&mv);
    TEST_ASSERT_EQUAL_STRING("shown error", mv.message.c_str());
    TEST_ASSERT_TRUE(mv.message_is_error);
    TEST_ASSERT_EQUAL_UINT(0, imgui_message_test_queued());

    /* zavřené okno */
    s_g4_open = false;
    for (int i = 0; i < 2; i++) g4_frame(&v);
    TEST_ASSERT_FALSE(cmt_view_window_shown());
    cmt_view_post_message("hidden info", false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, imgui_message_test_queued(), "info is dropped while the window is closed");
    cmt_view_post_message("hidden error", true);
    cmt_view_post("hidden warning", CMT_VIEW_MSG_WARNING);
    TEST_ASSERT_EQUAL_UINT(2, imgui_message_test_queued());
    ui_message_type_t type;
    char text[128];
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
    TEST_ASSERT_EQUAL_STRING("hidden error", text);
    TEST_ASSERT_TRUE(imgui_message_test_at(1, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_WARNING, type);
    imgui_message_test_clear();

    /* výsledek nahrávání při zavřeném okně: zahozené modálně (varování), dokončené nic */
    device_state_lock();
    cmt_view_poll_record();
    cmttmz_set_record_discarded("Recording discarded (test)");
    cmt_view_poll_record();
    device_state_unlock();
    TEST_ASSERT_EQUAL_UINT(1, imgui_message_test_queued());
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_WARNING, type);
    TEST_ASSERT_EQUAL_STRING("Recording discarded (test)", text);
    device_state_lock();
    cmttmz_set_record_result("Recorded (test)", 1, 0);
    cmt_view_poll_record();
    device_state_unlock();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, imgui_message_test_queued(), "a finished recording is not reported while the window is closed");
    imgui_message_test_clear();

    /* znovu otevřené okno: zprávy ze zavřeného okna se v řádku neukážou */
    s_g4_open = true;
    for (int i = 0; i < 2; i++) g4_frame(&v);
    fill(&mv);
    TEST_ASSERT_EQUAL_STRING("shown error", mv.message.c_str());

    /* vidět: zahozené nahrávání do řádku jako varování */
    device_state_lock();
    cmttmz_set_record_discarded("Recording discarded (shown)");
    device_state_unlock();
    fill(&mv);
    TEST_ASSERT_EQUAL_STRING("Recording discarded (shown)", mv.message.c_str());
    TEST_ASSERT_TRUE(mv.message_is_warning);
    TEST_ASSERT_FALSE(mv.message_is_error);
    TEST_ASSERT_EQUAL_UINT(0, imgui_message_test_queued());

    /* sbalené okno = nevidět; Alt+C ho zavře */
    ImGui::SetWindowCollapsed(W2_ID, true);
    ImGui::SetWindowFocus(W2_ID);
    for (int i = 0; i < 3; i++) g4_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(cmt_view_window_shown(), "a collapsed window is not shown");
    cmt_view_post_message("collapsed error", true);
    TEST_ASSERT_EQUAL_UINT(1, imgui_message_test_queued());
    g4_key(&v, ImGuiKey_C, ImGuiMod_Alt);
    TEST_ASSERT_FALSE_MESSAGE(s_g4_open, "Alt+C closes a collapsed window");

    ImGui::SetWindowCollapsed(W2_ID, false);
    s_g4_open = true;
    imgui_message_test_clear();
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Zásah režimu nad jádrem: Direct ve STOP, Direct -> Tape zapomene hlavičku hacku, odmítnutí při RECORD a při záznamu vstupů, volby hlavního menu.
 */
void test_g4f_mode_request_core_direct(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    st_DEVICE_REQ_RESULT res;

    /* STOP -> Direct: hack nainstalovaný, Boost vypnutý */
    g_cmt.cpu_boost = CMT_CPU_BOOST_ENABLED;
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_DIRECT) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, res.rc);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_DISABLED, g_cmt.cpu_boost);
    fill(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_MODE_DIRECT, v.mode);

    /* hlavička načtená hackem bez těla; Direct -> Tape ji zapomene */
    int blk = -1;
    for (size_t i = 0; i < v.blocks.size(); i++) {
        if (v.blocks[i].has_mzf && (int)i == v.current_block) blk = (int)i;
    }
    TEST_ASSERT_TRUE_MESSAGE(blk >= 0, "the open block of the test tape carries an MZF");
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.pending = true;
    st.pending_block = blk;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));
    TEST_ASSERT_TRUE(cmthack_tape_state_get(&st));
    device_state_unlock();
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_TAPE) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, res.rc);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    device_state_lock();
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&st), "leaving Direct forgets a header read without its body");
    device_state_unlock();

    /* hlavní menu: Use CMT Hack ve STOP zapne hack a Boost nechá; Enable CPU Boost hack nechá */
    g_cmt.cpu_boost = CMT_CPU_BOOST_ENABLED;
    TEST_ASSERT_TRUE(cmt_request_set_loading_hack(true) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LOADING_MODE_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, res.rc);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);
    TEST_ASSERT_TRUE(cmt_request_set_loading_boost(false) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, res.rc);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_DISABLED, g_cmt.cpu_boost);
    TEST_ASSERT_TRUE(cmt_request_set_loading_hack(false) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);

    /* Use CMT Hack při PLAY: odmítnuto (kontrola STOP), Boost jde */
    device_state_lock();
    cmt_play();
    device_state_unlock();
    TEST_ASSERT_TRUE(cmt_request_set_loading_hack(true) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_TRUE(cmt_request_set_loading_boost(true) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, res.rc);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);
    device_state_lock();
    cmt_stop();
    device_state_unlock();

    /* RECORD: Direct odmítnut */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    device_state_unlock();
    TEST_ASSERT_TRUE(cmt_request_set_loading_mode(CMT_LOADING_MODE_DIRECT) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    device_state_lock();
    cmt_stop();
    device_state_unlock();

    /* záznam vstupů: zásah odmítne device_request_poll() */
    g_replay_mode = REPLAY_MODE_RECORDING;
    TEST_ASSERT_TRUE(cmt_request_set_loading_boost(false) > 0);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);
    g_replay_mode = REPLAY_MODE_OFF;
}

/**
 * @brief Rozvržení Full: po kliknutí na řádek tabulky klávesy bez Ctrl nic, s Ctrl akce decku.
 */
void test_g4f_full_layout_shortcuts(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    s_g4_open = true;
    for (int i = 0; i < 3; i++) g4_frame(&v);
    ImGui::SetWindowFocus(W2_ID);
    for (int i = 0; i < 2; i++) g4_frame(&v);
    /* klik na řádek dá řádku fokus navigace */
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item("name_2");
    TEST_ASSERT_NOT_NULL(it);
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 c((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
    io.AddMousePosEvent(c.x, c.y);
    g4_frame(&v);
    io.AddMouseButtonEvent(0, true);
    g4_frame(&v);
    io.AddMouseButtonEvent(0, false);
    g4_frame(&v);
    imgui_cmt_tape_test_reset();
    w3_drain();
    (void)imgui_cmt_tape_test_take_last_run();
    const ImGuiKey plain[] = {ImGuiKey_R, ImGuiKey_S, ImGuiKey_Space, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Home,
                              ImGuiKey_Period, ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_O, ImGuiKey_E};
    for (ImGuiKey k : plain) {
        g4_key(&v, k, ImGuiMod_None);
        TEST_ASSERT_TRUE_MESSAGE(g4_nothing_queued(), ImGui::GetKeyName(k));
    }
    int32_t arg = -1;
    g4_key(&v, ImGuiKey_Space, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_PAUSE, &arg));
    TEST_ASSERT_EQUAL_INT(1, arg);
    g4_key(&v, ImGuiKey_Enter, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SEEK, &arg));
    /* Home / PageUp / PageDown jsou navigace ImGui: posunou fokus řádku a s ním výběr
     * (jako šipky, D-G3-5); skok jde na řádek, který má fokus (výchozí Full ukazuje všech
     * pět řádků, PageDown skončí na posledním) */
    TEST_ASSERT_EQUAL_INT(imgui_cmt_window_test_get_selection(), arg);
    TEST_ASSERT_TRUE(arg >= 0);
    w3_drain();
    imgui_cmt_tape_test_reset();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Menu Tape podle SPEC: položky, zkratky a zákazy podle stavu; Save as a Export bloku vydají správnou akci.
 */
void test_g4_tape_menu(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS); /* STOP, blok 0 = MZF */
    imgui_cmt_tape_test_reset();
    w3_click_at(&v, "menu_tape", 0);
    const char *items[] = {"menu_new", "menu_open", "menu_save", "menu_save_as", "menu_export_mzf", "menu_export_wav", "menu_eject"};
    const char *texts[] = {"New tape", "Open...", "Save", "Save as...", "Export block as MZF...", "Export block as WAV...", "Eject"};
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(w2_item(items[i]), items[i]);
        TEST_ASSERT_EQUAL_STRING(texts[i], w2_text(items[i]).c_str());
        TEST_ASSERT_FALSE_MESSAGE(w2_item(items[i])->disabled, items[i]);
    }
    w3_click_at(&v, "menu_export_mzf", 0);
    int eb = -1;
    bool wav = true;
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export(&eb, &wav));
    TEST_ASSERT_EQUAL_INT(0, eb);
    TEST_ASSERT_FALSE(wav);
    w3_close_popups(&v);
    w3_click_at(&v, "menu_tape", 0);
    w3_click_at(&v, "menu_save_as", 0);
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_save_as_open());
    imgui_cmt_tape_test_save_as_finish(NULL);
    w3_close_popups(&v);

    /* PLAY: Save, New tape, Export zakázané s důvodem; Eject a Open... ano */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_TRUE(w2_item("menu_save")->disabled);
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", w2_item("menu_save")->reason.c_str());
    TEST_ASSERT_TRUE(w2_item("menu_new")->disabled);
    TEST_ASSERT_TRUE(w2_item("menu_export_wav")->disabled);
    TEST_ASSERT_FALSE(w2_item("menu_eject")->disabled);
    TEST_ASSERT_FALSE(w2_item("menu_open")->disabled);
    w3_close_popups(&v);

    /* surový blok ve STOP: Export as MZF zakázaný */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS);
    v.current_block = 5; /* blok, který player neumí, bez MZF */
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_TRUE(w2_item("menu_export_mzf")->disabled);
    TEST_ASSERT_EQUAL_STRING("The current block does not contain an MZF file.", w2_item("menu_export_mzf")->reason.c_str());
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_tape_test_reset();
    imgui_cmt_window_test_reset();
}


/**
 * @brief Výsledek, který přijde ve snímku, kdy se okno zavřelo, jde modálně.
 *
 * Alt+C zavře okno ve snímku N; zpráva zařazená po kreslení téhož snímku
 * (výsledky zásahů se zpracují až po kreslení oken) nesmí skončit v řádku
 * zpráv zavřeného okna: chyba a varování modálně.
 */
void test_g6_message_in_closing_frame(void)
{
    imgui_message_test_clear();
    cmt_view_test_force_window_shown(-1);
    st_CMT_VIEW v;
    g4_setup(&v, CMT_VIEW_FIXTURE_PLAYING_COMPACT);
    TEST_ASSERT_TRUE(cmt_view_window_shown());
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    g4_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(s_g4_open, "Alt+C closes the window");
    /* stejný snímek, po kreslení okna */
    cmt_view_post_message("error after close", true);
    cmt_view_post("warning after close", CMT_VIEW_MSG_WARNING);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2, imgui_message_test_queued(), "results in the closing frame go modal");
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    g4_frame(&v);
    imgui_message_test_clear();
    cmt_view_test_force_window_shown(1);
    s_g4_open = true;
    imgui_cmt_window_test_reset();
}

/* Hlavní menu HW & Devices > CMT (topmenu/devices/menu_cmt.cpp, linkované do testu). */
extern "C" void imgui_menu_cmt(void);

/**
 * @brief Jeden snímek okna s lištou menu a nabídkou CMT hlavního menu (bez okna kazety).
 * @param[out] log Není-li NULL, texty vykreslené v okně menu včetně popupu
 *                 a tooltipu (logování ImGui do bufferu, ImGui::LogToBuffer()).
 */
static void g6_menu_frame(std::string *log = NULL)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f), ImGuiCond_Always);
    ImGui::Begin("menu host###g6_menu_host", NULL, ImGuiWindowFlags_MenuBar);
    if (log) {
        ImGui::GetCurrentContext()->LogBuffer.clear();
        ImGui::LogToBuffer();
    }
    if (ImGui::BeginMenuBar()) {
        device_state_lock();
        imgui_menu_cmt();
        device_state_unlock();
        ImGui::EndMenuBar();
    }
    /* LogFinish() buffer vyprázdní: text převzít před ním (a před End(), které ho volá samo) */
    if (log) {
        *log = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
}

/** @brief Klik levým tlačítkem na bod (myš, stisk, uvolnění, myš pryč). */
static void g6_menu_click(ImVec2 p)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    g6_menu_frame();
    io.AddMouseButtonEvent(0, true);
    g6_menu_frame();
    io.AddMouseButtonEvent(0, false);
    g6_menu_frame();
}

/**
 * @brief Hlavní menu CMT jde zásahem režimu načítání: Use CMT Hack a Enable CPU Boost.
 *
 * Klik na položky skutečného hlavního menu (imgui_menu_cmt()) zařadí zásah
 * CMT_REQUEST_LOADING_MODE_NAME (i[0] = -1, i[1] hack, i[2] Boost, -1 =
 * ponechat) a nic nezapíše do jádra z UI vlákna: CMT hack ani CPU Boost se
 * bez provedení zásahu nezmění. Návrat na přímý zápis (cmt_cpu_boost_set(),
 * cmthack_load_rom_patch()) test shodí. Zásah Enable CPU Boost se zapnutým
 * hackem test provede (device_request_poll()) a výsledek předá UI
 * (imgui_cmt_device_request_result()): CPU Boost se zapne, hack zůstane
 * a řádek zpráv mlčí. Menu, které by zásah zařadilo přímo
 * cmt_request_set_loading_boost() místo imgui_cmt_menu_set_cpu_boost(),
 * test shodí ("Loading mode: Direct").
 */
void test_g6_main_menu_routing(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    w3_drain();
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < 2; i++) g6_menu_frame();
    ImGuiWindow *host = ImGui::FindWindowByName("menu host###g6_menu_host");
    TEST_ASSERT_NOT_NULL(host);
    const ImGuiStyle &st = ImGui::GetStyle();
    float fs = ImGui::GetFontSize();
    ImRect mb = host->MenuBarRect();
    ImVec2 cmt_menu(mb.Min.x + st.WindowPadding.x + 8.0f, (mb.Min.y + mb.Max.y) * 0.5f);

    /* Use CMT Hack (první položka nabídky CMT) */
    g6_menu_click(cmt_menu);
    ImGuiWindow *pop = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_NOT_NULL_MESSAGE(pop, "the CMT menu is open");
    TEST_ASSERT_TRUE(pop->Active);
    ImVec2 p0 = pop->DC.CursorStartPos;
    g6_menu_click(ImVec2(p0.x + 8.0f, p0.y + fs * 0.5f));
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE_MESSAGE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, &a), "Use CMT Hack queues the loading mode request");
    TEST_ASSERT_EQUAL_INT(-1, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[1]);
    TEST_ASSERT_EQUAL_INT(-1, a.i[2]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, g_cmthack.load_patch_installed, "the UI thread does not install the ROM patch itself");

    /* CMT Options > Enable CPU Boost: podnabídku najde najetí myší po řádcích nabídky
     * (otevřená podnabídka má jméno okna "CMT Options...###Menu_01") */
    g6_menu_click(cmt_menu);
    pop = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_NOT_NULL(pop);
    p0 = pop->DC.CursorStartPos;
    ImGuiWindow *sub = NULL;
    ImVec2 opts(p0.x + 8.0f, 0.0f);
    for (float y = p0.y + fs * 0.5f; y < pop->Pos.y + pop->Size.y && !sub; y += fs * 0.5f) {
        opts.y = y;
        io.AddMousePosEvent(opts.x, opts.y);
        for (int i = 0; i < 3; i++) g6_menu_frame();
        ImGuiWindow *w = ImGui::FindWindowByName("###Menu_01");
        if (w && w->Active && strncmp(w->Name, "CMT Options", 11) == 0) sub = w;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(sub, "the CMT Options submenu is open");
    ImVec2 s0 = sub->DC.CursorStartPos;
    /* s CMT hackem zapnutým (Direct): řádek zpráv okna musí po zásahu mlčet;
     * stav hacku jen příznakem jako test_ff_menu_boost_message() */
    g_cmthack.load_patch_installed = 1;
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    cmt_view_post_message("-", false);
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("-", v.message.c_str());
    /* myš do podnabídky vodorovně (přes hranu), pak klik na první položku */
    io.AddMousePosEvent(s0.x + 8.0f, opts.y);
    g6_menu_frame();
    g6_menu_click(ImVec2(s0.x + 8.0f, s0.y + fs * 0.5f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_CPU_BOOST_DISABLED, g_cmt.cpu_boost, "the UI thread does not switch CPU Boost itself");
    /* zásah z menu provede emu vlákno: CPU Boost se změní
     * jen cestou zásahu, hack zůstane (i[1] = -1) a výsledek nic nehlásí */
    device_request_poll();
    st_DEVICE_REQ_RESULT res;
    bool found = false;
    while (device_request_take_result(&res)) {
        if (strcmp(res.name, CMT_REQUEST_LOADING_MODE_NAME) != 0) continue;
        TEST_ASSERT_FALSE_MESSAGE(found, "Enable CPU Boost queues exactly one loading mode request");
        found = true;
        TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
        TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, res.rc);
        TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "Enable CPU Boost queues the loading mode request");
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost, "the request switched CPU Boost on");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, g_cmthack.load_patch_installed, "the request keeps the CMT hack");
    fill(&v);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("-", v.message.c_str(), "Enable CPU Boost from the main menu with the hack on reports no mode change");
    g_cmthack.load_patch_installed = 0;
    io.AddMousePosEvent(-1.0f, -1.0f);
    g6_menu_frame();
    w3_drain();
}

/**
 * @brief Otevře nabídku CMT, najede na Use CMT Hack a vrátí texty snímku s najetím (položka, tooltip).
 * @param[out] item Bod položky Use CMT Hack (pro klik).
 * @return Texty vykreslené ve snímku (g6_menu_frame() s logem).
 */
static std::string g6_hover_use_cmt_hack(ImVec2 *item)
{
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < 2; i++) g6_menu_frame();
    ImGuiWindow *host = ImGui::FindWindowByName("menu host###g6_menu_host");
    TEST_ASSERT_NOT_NULL(host);
    const ImGuiStyle &st = ImGui::GetStyle();
    float fs = ImGui::GetFontSize();
    ImRect mb = host->MenuBarRect();
    g6_menu_click(ImVec2(mb.Min.x + st.WindowPadding.x + 8.0f, (mb.Min.y + mb.Max.y) * 0.5f));
    ImGuiWindow *pop = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_NOT_NULL_MESSAGE(pop, "the CMT menu is open");
    TEST_ASSERT_TRUE(pop->Active);
    ImVec2 p0 = pop->DC.CursorStartPos;
    *item = ImVec2(p0.x + 8.0f, p0.y + fs * 0.5f);
    io.AddMousePosEvent(item->x, item->y);
    for (int i = 0; i < 2; i++) g6_menu_frame();
    std::string log;
    g6_menu_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("Use CMT Hack") != std::string::npos, "the logged frame shows the CMT menu");
    return log;
}

/** @brief Zavře otevřenou nabídku hlavního menu (myš pryč, Escape). */
static void g6_menu_close(void)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    g6_menu_frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    g6_menu_frame();
    g6_menu_frame();
}

/**
 * @brief Use CMT Hack hlavního menu se skutečnou ROM bez podpory CMT hacku: zakázaná položka s důvodem, žádný zásah.
 *
 * Dřív menu zásah zařadilo, ROM patch se nenainstaloval a výsledek ohlásil
 * jiný režim (při skrytém okně nic). Stav "hack zapnutý a ROM bez podpory"
 * nastat nemá: zapnutý hack výměna ROM vypne (rom_reinstall()), test to ověří
 * a pak menu ukáže zakázanou položku s tooltipem cmt_view_direct_unsupported_reason().
 * S původní ROM tentýž bod tooltip nemá. Bez ROM bez podpory v buildu se
 * test přeskočí; ROM vrátí tearDown.
 */
void test_g6_main_menu_hack_unsupported_rom(void)
{
    if (TEST_ROM_CMTHACK_INCOMP) TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    st_ROM_CONFIG_ROW *incomp = find_rom_without_hack();
    if (!incomp) TEST_IGNORE_MESSAGE("no predefined ROM incompatible with the CMT hack in this build");
    st_ROM_CONFIG_ROW *orig = g_rom.rom_cfg;
    const char *reason = "The selected ROM does not support direct loading.";
    /* hack skutečně zapnutý (patch v ROM), pak ROM bez podpory: hack se vypne */
    device_state_lock();
    cmthack_load_rom_patch(1);
    device_state_unlock();
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    switch_rom(incomp);
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "a ROM without support switches the hack off");
    w2_fonts();
    w2_apply_scale(1.0f);
    w3_drain();

    ImVec2 item;
    std::string log = g6_hover_use_cmt_hack(&item);
    TEST_ASSERT_TRUE_MESSAGE(log.find(reason) != std::string::npos, "the disabled Use CMT Hack shows the reason");
    ImGuiWindow *tip = ImGui::FindWindowByName("##Tooltip_00");
    TEST_ASSERT_TRUE(tip && tip->Active);
    g6_menu_click(item);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, NULL), "Use CMT Hack queues nothing with this ROM");
    g6_menu_close();

    /* původní ROM: položka povolená, bez důvodu */
    switch_rom(orig);
    log = g6_hover_use_cmt_hack(&item);
    TEST_ASSERT_TRUE_MESSAGE(log.find(reason) == std::string::npos, "a supporting ROM shows no reason");
    tip = ImGui::FindWindowByName("##Tooltip_00");
    TEST_ASSERT_FALSE_MESSAGE(tip && tip->Active, "no tooltip with a supporting ROM");
    g6_menu_close();
    w3_drain();
}

/* Modál Custom value... (topmenu/devices/menu_cmt.cpp); test item hook menu_cmt.cpp. */
extern "C" void imgui_menu_cmt_bootdev_popup(void);
extern "C" bool imgui_menu_cmt_test_item_rect(const char *name, float *x0, float *y0, float *x1, float *y1);

/**
 * @brief Jeden snímek se zanořenými nabídkami: lišta menu > HW & Devices > CMT, modál z hlavního okna.
 *
 * V aplikaci je kořenem menu kontextový popup `EmulatorPopupMenu`
 * (main_window.cpp, pravé tlačítko, imgui_topmenu_body()) bez lišty menu;
 * HW & Devices je v něm podnabídka (úroveň 1 zásobníku popupů), uvnitř které
 * se volá imgui_menu_cmt() (menu_devices.cpp), a modál Custom value... kreslí
 * main_window.cpp mimo nabídky a okna (imgui_menu_cmt_bootdev_popup()).
 * Test kořen nahrazuje lištou menu okna (`ImGuiWindowFlags_MenuBar`),
 * takže HW & Devices je tu úroveň 0 a rodič má `MenuBar` - na tom závisí
 * např. CloseCurrentPopup(), proto test není dokladem pro chování závislé na
 * úrovni nabídky nebo na kořeni. Věrně napodobuje to, na čem stojí testovaná
 * chyba: modál otevřený uvnitř nabídek se v nich nenakreslí a po zavření
 * nabídek se imgui_menu_cmt() už nevolá, kdežto modál kreslený mimo nabídky
 * po zavření nabídek ano. Celé imgui_menu_devices() test linkovat nemůže
 * (volá nabídky všech zařízení), proto nadřazenou nabídku nahrazuje stejné
 * BeginMenu(_L("HW & Devices")).
 *
 * @param[out] log Není-li NULL, texty vykreslené ve snímku (nabídky i modál).
 */
static void g6_nested_frame(std::string *log = NULL)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (log) {
        ImGui::GetCurrentContext()->LogBuffer.clear();
        ImGui::LogToBuffer();
    }
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f), ImGuiCond_Always);
    ImGui::Begin("menu host###g6_menu_host", NULL, ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(_L("HW & Devices"))) {
            device_state_lock();
            imgui_menu_cmt();
            device_state_unlock();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
    /* jako main_window.cpp: modál mimo nabídky a okna */
    imgui_menu_cmt_bootdev_popup();
    if (log) {
        *log = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::Render();
}

/** @brief Klik levým tlačítkem na bod v zanořeném menu (myš, stisk, uvolnění). */
static void g6_nested_click(ImVec2 p)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    g6_nested_frame();
    io.AddMouseButtonEvent(0, true);
    g6_nested_frame();
    io.AddMouseButtonEvent(0, false);
    g6_nested_frame();
}

/**
 * @brief Střed prvku nabídky CMT podle jeho jména (obdélník z posledního kreslení, menu_cmt.cpp).
 * @param name Jméno prvku (`bootdev_menu`, `bootdev_cmt`, ..., `bootdev_custom`).
 * @return Střed obdélníku; test selže, prvek-li nebyl nakreslen.
 */
static ImVec2 g6_item_center(const char *name)
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    TEST_ASSERT_TRUE_MESSAGE(imgui_menu_cmt_test_item_rect(name, &x0, &y0, &x1, &y1), name);
    return ImVec2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
}

/**
 * @brief Najetím myši po řádcích okna nabídky otevře podnabídku daného jména.
 * @param parent Okno nabídky. @param child_window Jméno okna podnabídky (`###Menu_NN`).
 * @param title Začátek titulku podnabídky. @param[out] row Bod řádku, který ji otevřel.
 * @return Okno otevřené podnabídky (test selže, když se neotevře).
 */
static ImGuiWindow *g6_hover_submenu(ImGuiWindow *parent, const char *child_window, const char *title, ImVec2 *row)
{
    ImGuiIO &io = ImGui::GetIO();
    float fs = ImGui::GetFontSize();
    ImVec2 p0 = parent->DC.CursorStartPos;
    ImVec2 at(p0.x + 8.0f, 0.0f);
    for (float y = p0.y + fs * 0.5f; y < parent->Pos.y + parent->Size.y; y += fs * 0.5f) {
        at.y = y;
        io.AddMousePosEvent(at.x, at.y);
        for (int i = 0; i < 3; i++) g6_nested_frame();
        ImGuiWindow *w = ImGui::FindWindowByName(child_window);
        if (w && w->Active && strncmp(w->Name, title, strlen(title)) == 0) {
            *row = at;
            /* myš vodorovně do podnabídky (přes hranu), ať se nezavře */
            io.AddMousePosEvent(w->DC.CursorStartPos.x + 8.0f, at.y);
            g6_nested_frame();
            return w;
        }
    }
    TEST_FAIL_MESSAGE(title);
    return NULL;
}

/**
 * @brief Otevře HW & Devices > CMT > CMT Options > Device for Direct MZF Start (zanoření z g6_nested_frame()).
 * @return Okno podnabídky Device for Direct MZF Start (`###Menu_03`).
 */
static ImGuiWindow *g6_open_bootdev_menu(void)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    /* nabídky otevřené z předchozího kroku zavřít (klik na lištu by je jinak zavřel místo otevření) */
    for (int k = 0; k < 6 && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0; k++) {
        io.AddKeyEvent(ImGuiKey_Escape, true);
        g6_nested_frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        g6_nested_frame();
    }
    ImGuiWindow *host = ImGui::FindWindowByName("menu host###g6_menu_host");
    TEST_ASSERT_NOT_NULL(host);
    const ImGuiStyle &st = ImGui::GetStyle();
    ImRect mb = host->MenuBarRect();
    g6_nested_click(ImVec2(mb.Min.x + st.WindowPadding.x + 8.0f, (mb.Min.y + mb.Max.y) * 0.5f));
    ImGuiWindow *dev = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_TRUE_MESSAGE(dev && dev->Active, "HW & Devices is open");
    ImVec2 row;
    ImGuiWindow *cmt = g6_hover_submenu(dev, "###Menu_01", "CMT", &row);
    ImGuiWindow *opts = g6_hover_submenu(cmt, "###Menu_02", "CMT Options", &row);
    (void)opts;
    /* Device for Direct MZF Start: řádek podle obdélníku zaznamenaného menu_cmt.cpp */
    ImVec2 item = g6_item_center("bootdev_menu");
    io.AddMousePosEvent(item.x, item.y);
    for (int i = 0; i < 3; i++) g6_nested_frame();
    ImGuiWindow *bd = ImGui::FindWindowByName("###Menu_03");
    TEST_ASSERT_TRUE_MESSAGE(bd && bd->Active && strncmp(bd->Name, "Device for Direct MZF Start", 27) == 0,
                             "the Device for Direct MZF Start submenu is open");
    io.AddMousePosEvent(bd->DC.CursorStartPos.x + 8.0f, item.y);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    return bd;
}

/**
 * @brief Otevře HW & Devices > CMT > Recent (zanoření z g6_nested_frame()).
 * @return Okno podnabídky Recent (`###Menu_02`).
 */
static ImGuiWindow *g7_open_recent_menu(void)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    /* nabídky otevřené z předchozího kroku zavřít (klik na lištu by je jinak zavřel místo otevření) */
    for (int k = 0; k < 6 && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0; k++) {
        io.AddKeyEvent(ImGuiKey_Escape, true);
        g6_nested_frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        g6_nested_frame();
    }
    ImGuiWindow *host = ImGui::FindWindowByName("menu host###g6_menu_host");
    TEST_ASSERT_NOT_NULL(host);
    const ImGuiStyle &st = ImGui::GetStyle();
    ImRect mb = host->MenuBarRect();
    g6_nested_click(ImVec2(mb.Min.x + st.WindowPadding.x + 8.0f, (mb.Min.y + mb.Max.y) * 0.5f));
    ImGuiWindow *dev = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_TRUE_MESSAGE(dev && dev->Active, "HW & Devices is open");
    ImVec2 row;
    ImGuiWindow *cmt = g6_hover_submenu(dev, "###Menu_01", "CMT", &row);
    (void)cmt;
    /* Recent: řádek podle obdélníku zaznamenaného menu_recent.cpp */
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    TEST_ASSERT_TRUE_MESSAGE(imgui_menu_recent_test_item_rect("recent_menu", &x0, &y0, &x1, &y1), "the Recent item is drawn");
    ImVec2 item((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    io.AddMousePosEvent(item.x, item.y);
    for (int i = 0; i < 3; i++) g6_nested_frame();
    ImGuiWindow *rc = ImGui::FindWindowByName("###Menu_02");
    TEST_ASSERT_TRUE_MESSAGE(rc && rc->Active && strncmp(rc->Name, "Recent", 6) == 0, "the Recent submenu is open");
    io.AddMousePosEvent(rc->DC.CursorStartPos.x + 8.0f, item.y);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    return rc;
}

/** @brief Střed prvku podnabídky Recent podle jména (obdélník z posledního kreslení, menu_recent.cpp). */
static ImVec2 g7_recent_item_center(const char *name)
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    TEST_ASSERT_TRUE_MESSAGE(imgui_menu_recent_test_item_rect(name, &x0, &y0, &x1, &y1), name);
    return ImVec2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
}

/**
 * @brief Hlavní menu HW & Devices > CMT > Recent: naposledy vložené pásky.
 *
 * Podnabídka ukazuje jen pásky (ne obrazy disket) od naposledy vložené;
 * klik na položku zavře nabídky a otevře právě tu pásku stejnou cestou jako
 * dialog Open... (automat akcí, zásah `cmt_open` s její cestou). Prázdná
 * historie pásek ukáže "(empty)"; "Clear recent media" smaže historii všech
 * druhů.
 */
void test_g7_main_menu_recent(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    w3_drain();
    device_state_lock();
    cmt_stop();
    cmt_eject();
    device_state_unlock();
    recent_media_reset();

    gchar *first = g_build_filename(g_get_tmp_dir(), "g7-first-tape.mzf", NULL);
    gchar *second = g_build_filename(g_get_tmp_dir(), "g7-second-tape.mzf", NULL);
    gchar *disk = g_build_filename(g_get_tmp_dir(), "g7-disk.dsk", NULL);
    recent_media_add(RECENT_MEDIA_CMT, first);
    recent_media_add(RECENT_MEDIA_DSK, disk);
    recent_media_add(RECENT_MEDIA_CMT, second);
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_CMT));

    /* položky: druhá (naposledy vložená) nahoře, disketa mezi páskami není */
    g7_open_recent_menu();
    std::string log;
    g6_nested_frame(&log);
    const size_t at_second = log.find("g7-second-tape.mzf");
    const size_t at_first = log.find("g7-first-tape.mzf");
    TEST_ASSERT_TRUE_MESSAGE(at_second != std::string::npos && at_first != std::string::npos, "both tapes are listed");
    TEST_ASSERT_TRUE_MESSAGE(at_second < at_first, "the most recent tape comes first");
    TEST_ASSERT_TRUE_MESSAGE(log.find("g7-disk.dsk") == std::string::npos, "disk images are not listed among tapes");
    TEST_ASSERT_TRUE_MESSAGE(log.find("Clear recent media") != std::string::npos, "Clear recent media row");

    /* klik na starší pásku: nabídky se zavřou a zařadí se otevření právě té pásky */
    g6_nested_click(g7_recent_item_center("recent_1"));
    imgui_cmt_tape_run_deferred(); /* automat akcí s páskou běží až po uvolnění zámku */
    st_DEVICE_REQ_ARGS args;
    TEST_ASSERT_TRUE_MESSAGE(w3_take_request("cmt_open", &args), "a click on a recent tape queues cmt_open");
    TEST_ASSERT_EQUAL_STRING(first, args.str);
    g6_nested_frame();
    ImGuiWindow *m0 = ImGui::FindWindowByName("###Menu_00");
    TEST_ASSERT_FALSE_MESSAGE(m0 && m0->Active, "the click closes the menus");

    /* klik na první položku: druhá páska */
    g7_open_recent_menu();
    g6_nested_click(g7_recent_item_center("recent_0"));
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_TRUE(w3_take_request("cmt_open", &args));
    TEST_ASSERT_EQUAL_STRING(second, args.str);

    /* Clear recent media: všechny druhy pryč, nic se nevkládá */
    g7_open_recent_menu();
    g6_nested_click(g7_recent_item_center("recent_clear"));
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_CMT));
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request("cmt_open", NULL), "clearing inserts nothing");

    /* prázdná historie: zakázaná položka (empty), Clear zakázané */
    g7_open_recent_menu();
    g6_nested_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("(empty)") != std::string::npos, "an empty history shows (empty)");
    g6_nested_click(g7_recent_item_center("recent_empty"));
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request("cmt_open", NULL), "the (empty) row does nothing");

    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    for (int k = 0; k < 6 && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0; k++) {
        io.AddKeyEvent(ImGuiKey_Escape, true);
        g6_nested_frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        g6_nested_frame();
    }
    recent_media_reset();
    w3_drain();
    g_free(first);
    g_free(second);
    g_free(disk);
}

/** @brief Smaže text vstupu s fokusem (Backspace po jednom znaku, BOOTDEV_TEXT_MAX krát). */
static void g6_clear_input(void)
{
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < BOOTDEV_TEXT_MAX; i++) {
        io.AddKeyEvent(ImGuiKey_Backspace, true);
        g6_nested_frame();
        io.AddKeyEvent(ImGuiKey_Backspace, false);
        g6_nested_frame();
    }
}

/** @brief Je modál Custom value... otevřený (okno aktivní)? */
static bool g6_bootdev_modal_open(void)
{
    ImGuiWindow *m = ImGui::FindWindowByName("Device Code for Direct MZF Start###menu_cmt_bootdev_custom");
    return m && m->Active;
}

/**
 * @brief Hlavní menu HW & Devices > CMT > CMT Options > Device for Direct MZF Start: přepínače a Custom value....
 *
 * Zanoření nabídek z g6_nested_frame() (kořen je lišta menu, ne kontextový
 * popup aplikace): klik na položku zavře všechny nabídky, modál Custom
 * value... kreslí funkce z hlavního okna.
 * Řádky se hledají podle obdélníků zaznamenaných menu_cmt.cpp (jméno
 * prvku), ne podle vypočtených souřadnic. FD nastaví `fd`, QD `qd`, RD `rd`,
 * CMT `cmt`; texty řádků nesou hodnotu BC (MZ-800). Custom value... otevře
 * modál s aktuální hodnotou šestnáctkově (a po zavření nabídek je skutečně
 * vidět); jméno zařízení (fd) a desítkové číslo s úvodní nulou (0200, s
 * radou o šestnáctkovém zápisu) Enter neuloží a modál zůstane, platný zápis
 * (0x1234) Enter uloží a modál zavře; Escape zavře bez změny; potom položka
 * Custom value ukazuje hodnotu (1234h). Po zavření modálu smí otevřít jiný
 * modál (imgui_modal_can_open()).
 */
void test_g6_main_menu_bootdev(void)
{
    w2_fonts();
    w2_apply_scale(1.0f);
    w3_drain();
    ImGuiIO &io = ImGui::GetIO();
    char saved[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(saved, sizeof(saved));
    TEST_ASSERT_TRUE(bootdev_setting_set_text("cmt"));
    char t[BOOTDEV_TEXT_MAX];

    /* texty položek s hodnotou BC (MZ-800) */
    g6_open_bootdev_menu();
    std::string log;
    g6_nested_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("CMT - cassette (0100h)") != std::string::npos, "CMT row with 0100h");
    TEST_ASSERT_TRUE_MESSAGE(log.find("FD - floppy disk (0200h)") != std::string::npos, "FD row with 0200h");
    TEST_ASSERT_TRUE_MESSAGE(log.find("QD - Quick Disk (0300h)") != std::string::npos, "QD row with 0300h");
    TEST_ASSERT_TRUE_MESSAGE(log.find("RD - SRAM (0000h)") != std::string::npos, "RD row with 0000h");
    TEST_ASSERT_TRUE_MESSAGE(log.find("Custom value...") != std::string::npos, "Custom value row");

    static const struct { const char *item; const char *want; } rows[] = {
        {"bootdev_fd", "fd"}, {"bootdev_qd", "qd"}, {"bootdev_rd", "rd"}, {"bootdev_cmt", "cmt"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        g6_open_bootdev_menu();
        g6_nested_click(g6_item_center(rows[i].item));
        bootdev_setting_get_text(t, sizeof(t));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(rows[i].want, t, rows[i].item);
        g6_nested_frame();
        ImGuiWindow *m0 = ImGui::FindWindowByName("###Menu_00");
        TEST_ASSERT_FALSE_MESSAGE(m0 && m0->Active, "the click closes the menus");
    }
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_LOADING_MODE_NAME, NULL), "no device request");

    /* Custom value...: nabídky se zavřou a modál se skutečně ukáže */
    g6_open_bootdev_menu();
    g6_nested_click(g6_item_center("bootdev_custom"));
    for (int i = 0; i < 3; i++) g6_nested_frame();
    TEST_ASSERT_TRUE_MESSAGE(g6_bootdev_modal_open(), "Custom value... opens the dialog (nested menus as in the application)");
    g6_nested_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("Current value: 0100h (cmt)") != std::string::npos, "the dialog shows the current value in hex");

    /* jméno zařízení jako vlastní hodnota neplatné, Enter */
    g6_clear_input();
    io.AddInputCharactersUTF8("fd");
    g6_nested_frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    g6_nested_frame(&log);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    g6_nested_frame();
    TEST_ASSERT_TRUE_MESSAGE(log.find("Invalid value") != std::string::npos, "a device name is not a custom value");
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cmt", t, "an invalid value is not stored");
    TEST_ASSERT_TRUE_MESSAGE(g6_bootdev_modal_open(), "the dialog stays open");

    /* desítkové číslo s úvodní nulou: odmítnuto s radou */
    g6_clear_input();
    io.AddInputCharactersUTF8("0200");
    g6_nested_frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    g6_nested_frame(&log);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    g6_nested_frame();
    TEST_ASSERT_TRUE_MESSAGE(log.find("write hexadecimal as 0x0200 or 0200h") != std::string::npos,
                             "a leading zero is refused with advice");
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cmt", t, "0200 is not stored");
    TEST_ASSERT_TRUE(g6_bootdev_modal_open());

    /* platný zápis 0x1234 + Enter */
    g6_clear_input();
    io.AddInputCharactersUTF8("0x1234");
    g6_nested_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("= 1234h") != std::string::npos, "the dialog shows the parsed value");
    io.AddKeyEvent(ImGuiKey_Enter, true);
    g6_nested_frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0x1234", t, "Enter stores the custom value");
    TEST_ASSERT_FALSE_MESSAGE(g6_bootdev_modal_open(), "the dialog closes after OK");
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_modal_can_open(), "no popup is left open after the dialog");
    ImGui::Render();

    /* Custom value ukazuje hodnotu; Escape zavře bez změny */
    g6_open_bootdev_menu();
    g6_nested_frame(&log);
    TEST_ASSERT_TRUE_MESSAGE(log.find("Custom value (1234h)...") != std::string::npos, "the Custom value row shows the value");
    g6_nested_click(g6_item_center("bootdev_custom"));
    for (int i = 0; i < 3; i++) g6_nested_frame();
    TEST_ASSERT_TRUE(g6_bootdev_modal_open());
    io.AddInputCharactersUTF8("9");
    g6_nested_frame();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    g6_nested_frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    bootdev_setting_get_text(t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0x1234", t, "Escape keeps the value");
    TEST_ASSERT_FALSE(g6_bootdev_modal_open());

    io.AddMousePosEvent(-1.0f, -1.0f);
    for (int i = 0; i < 2; i++) g6_nested_frame();
    (void)bootdev_setting_set_text(saved);
    w3_drain();
}

/* ================================================================
 * Úpravy pásky: vícenásobný výběr, mazání, přesun, import, přetažení
 * ================================================================ */

/** @brief Text "The tape changed; the action was cancelled." (varování zrušené akce nad jinou páskou). */
#define H4_CHANGED "The tape changed; the action was cancelled."

/**
 * @brief Okno Full s pěti bloky konceptu ve STOP (úpravy povolené), bez výběru.
 * @param v Kopie (PLAYING_FULL zastavená).
 */
static void h4_setup(st_CMT_VIEW *v)
{
    /* modifikátor nebo tlačítko myši držené testem, který skončil asercí, nesmí platit dál */
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    w3_setup(v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v->state = CMT_VIEW_STATE_STOP;
    v->motor_known = false;
    for (int i = 0; i < 2; i++) w2_frame(v);
    w3_drain();
}

/**
 * @brief Klik levým tlačítkem na prvek s modifikátorem (Ctrl / Shift) drženým po celou dobu.
 * @param v Kopie. @param name Prvek. @param mod ImGuiMod_Ctrl, ImGuiMod_Shift nebo ImGuiKey_None.
 */
static void h4_click_mod(st_CMT_VIEW *v, const char *name, ImGuiKey mod)
{
    ImGuiIO &io = ImGui::GetIO();
    if (mod != ImGuiKey_None) io.AddKeyEvent(mod, true);
    w2_frame(v);
    w2_click(v, name);
    if (mod != ImGuiKey_None) io.AddKeyEvent(mod, false);
    w2_frame(v);
}

/**
 * @brief Stisk klávesy (volitelně s modifikátorem drženým o snímek dřív) ve snímcích okna.
 * @param v Kopie. @param key Klávesa. @param mod Modifikátor nebo ImGuiKey_None.
 */
static void h4_key(st_CMT_VIEW *v, ImGuiKey key, ImGuiKey mod)
{
    ImGuiIO &io = ImGui::GetIO();
    if (mod != ImGuiKey_None) {
        io.AddKeyEvent(mod, true);
        w2_frame(v);
    }
    io.AddKeyEvent(key, true);
    w2_frame(v);
    io.AddKeyEvent(key, false);
    w2_frame(v);
    if (mod != ImGuiKey_None) io.AddKeyEvent(mod, false);
    w2_frame(v);
}

/** @brief Klik mimo všechna okna ImGui (fokus okna se zruší). @param v Kopie. */
static void h4_click_void(st_CMT_VIEW *v)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((float)s_canvas_w - 10.0f, (float)s_canvas_h - 10.0f);
    w2_frame(v);
    io.AddMouseButtonEvent(0, true);
    w2_frame(v);
    io.AddMouseButtonEvent(0, false);
    w2_frame(v);
    io.AddMousePosEvent(-1.0f, -1.0f);
    w2_frame(v);
}

/** @brief Střed prvku z posledního snímku (prvek musí existovat). */
static ImVec2 h4_center(const char *name)
{
    const st_CMT_WINDOW_TEST_ITEM *it = w2_item(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, name);
    return ImVec2((it->min.x + it->max.x) * 0.5f, (it->min.y + it->max.y) * 0.5f);
}

/** @brief Vybrané řádky jako text "1,3,4" (pro hlášení). */
static std::string h4_sel_text(void)
{
    std::string s;
    for (int id : imgui_cmt_window_test_get_selected()) {
        if (!s.empty()) s += ",";
        s += std::to_string(id);
    }
    return s;
}

/**
 * @brief Výběr: klik, Ctrl+klik, Shift+klik, Ctrl+A při fokusu tabulky; vlastnosti při více vybraných "N blocks selected".
 *
 * Dvojklik dál pracuje s jedním řádkem (SELECT_FORCED na řádek pod kurzorem).
 */
void test_h4_multi_select(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    /* vlastnosti sbalené (výchozí): všech pět řádků je vidět, nadpis sekce je v hlavičce */
    w2_click(&v, "name_1");
    TEST_ASSERT_EQUAL_STRING("1", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_STRING("Block properties", w2_text("props_title").c_str());
    h4_click_mod(&v, "name_3", ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_STRING("1,3", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, imgui_cmt_window_test_get_selection(), "the last selected row is the primary one");
    TEST_ASSERT_EQUAL_STRING("2 blocks selected", w2_text("props_title").c_str());
    TEST_ASSERT_NULL_MESSAGE(w2_item("prop_label_0"), "nothing else in the properties of several blocks");
    TEST_ASSERT_NULL(w2_item("props_subtitle"));
    /* Ctrl+klik vybraný řádek ubere */
    h4_click_mod(&v, "name_1", ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_STRING("3", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_INT(3, imgui_cmt_window_test_get_selection());
    TEST_ASSERT_EQUAL_STRING("Block properties", w2_text("props_title").c_str());
    /* Shift+klik: rozsah od posledního kliknutého řádku */
    w2_click(&v, "name_3");
    h4_click_mod(&v, "name_0", ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_STRING("0,1,2,3", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_STRING("4 blocks selected", w2_text("props_title").c_str());
    /* Ctrl+A při fokusu tabulky: vše */
    w2_click(&v, "name_2");
    h4_key(&v, ImGuiKey_A, ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_STRING("0,1,2,3,4", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_STRING("5 blocks selected", w2_text("props_title").c_str());
    /* klik = jeden řádek */
    w2_click(&v, "name_4");
    TEST_ASSERT_EQUAL_STRING("4", h4_sel_text().c_str());
    /* dvojklik pracuje s řádkem pod kurzorem */
    h4_click_mod(&v, "name_1", ImGuiMod_Ctrl);
    w3_drain();
    w3_double_click(&v, "name_2");
    int32_t arg = -1;
    TEST_ASSERT_TRUE(w3_take_cmt_op(REPLAY_CMT_SELECT_FORCED, &arg));
    TEST_ASSERT_EQUAL_INT(2, arg);
    /* jiná páska výběr zruší celý */
    imgui_cmt_window_test_set_selected({0, 2, 4}, v.tape_serial);
    w2_frame(&v);
    v.tape_serial++;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_INT(-1, imgui_cmt_window_test_get_selection());
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Delete u uložených bloků: jeden zásah s rozsahy, bez modálu (kontextové menu i klávesa Del).
 *
 * Položka menu má text "Delete N blocks" při více vybraných, jinak "Delete".
 */
void test_h4_delete_saved(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    imgui_cmt_window_test_set_selected({0, 2}, v.tape_serial);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_NOT_NULL(w2_item("ctx_delete"));
    TEST_ASSERT_EQUAL_STRING("Delete 2 blocks", w2_text("ctx_delete").c_str());
    TEST_ASSERT_FALSE(w2_item("ctx_delete")->disabled);
    w3_click_at(&v, "ctx_delete", 0);
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), "saved blocks are deleted without asking");
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, &a));
    TEST_ASSERT_EQUAL_STRING("0,2", a.str);
    TEST_ASSERT_EQUAL_INT(2, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[4]);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, (unsigned)a.i[5]);
    w3_close_popups(&v);

    /* jeden blok: "Delete"; klávesa Del při fokusu tabulky */
    w2_click(&v, "name_1");
    w3_click_at(&v, "name_1", 1);
    TEST_ASSERT_EQUAL_STRING("Delete", w2_text("ctx_delete").c_str());
    w3_close_popups(&v);
    w2_click(&v, "name_1");
    w3_drain();
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, &a));
    TEST_ASSERT_EQUAL_STRING("1", a.str);
    /* Del bez fokusu okna (klik mimo všechna okna) nic */
    h4_click_void(&v);
    w3_drain();
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL), "Del outside the table does nothing");
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Delete s neuloženým blokem: modál "Delete blocks", Cancel, Enter i Escape nic nepošlou, "Delete blocks" pošle zásah.
 */
void test_h4_delete_unsaved_modal(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    /* jen neuložený blok 4: věta pro jeden blok */
    w3_click_at(&v, "name_4", 1);
    w3_click_at(&v, "ctx_delete", 0);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    TEST_ASSERT_EQUAL_STRING("Delete this unsaved block? Its data is not stored anywhere else.", w2_text("del_text").c_str());
    w3_click_at(&v, "del_cancel", 0);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
    /* blok 2 uložený, blok 4 neuložený: smíšený výběr = celek i počet neuložených */
    imgui_cmt_window_test_set_selected({2, 4}, v.tape_serial);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "name_4", 1);
    w3_click_at(&v, "ctx_delete", 0);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    TEST_ASSERT_EQUAL_STRING("Delete blocks: 2. Unsaved: 1. Data of the unsaved blocks is not stored anywhere else.", w2_text("del_text").c_str());
    TEST_ASSERT_EQUAL_STRING("Delete blocks", w2_text("del_confirm").c_str());
    TEST_ASSERT_EQUAL_STRING("Cancel", w2_text("del_cancel").c_str());
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL));
    w3_click_at(&v, "del_cancel", 0);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL), "Cancel deletes nothing");

    /* dva neuložené (surový záznam a SAVED GAME): Enter = výchozí Cancel */
    imgui_cmt_window_test_set_selected({3, 4}, v.tape_serial);
    w2_frame(&v);
    w2_click(&v, "name_4");
    h4_click_mod(&v, "name_3", ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_STRING("3,4", h4_sel_text().c_str());
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    TEST_ASSERT_EQUAL_STRING("Delete 2 unsaved blocks? Their data is not stored anywhere else.", w2_text("del_text").c_str());
    h4_key(&v, ImGuiKey_Enter, ImGuiKey_None);
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), "Enter closes the dialog");
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL), "the default button is Cancel");
    /* Escape = Cancel */
    w2_click(&v, "name_4");
    h4_click_mod(&v, "name_3", ImGuiMod_Ctrl);
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    h4_key(&v, ImGuiKey_Escape, ImGuiKey_None);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL));
    /* Delete blocks */
    w2_click(&v, "name_4");
    h4_click_mod(&v, "name_3", ImGuiMod_Ctrl);
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    w3_click_at(&v, "del_confirm", 0);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, &a));
    TEST_ASSERT_EQUAL_STRING("3-4", a.str);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, (unsigned)a.i[5]);

    /* páska se změní, zatímco modál čeká - zruší se se zprávou, nic se nesmaže */
    st_CMT_VIEW mv;
    cmt_view_reset(&mv);
    fill(&mv);
    w2_click(&v, "name_4");
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    v.tape_serial++;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), "the dialog closes when the tape changes");
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL));
    TEST_ASSERT_EQUAL_STRING(H4_CHANGED, g3f_message(&mv).c_str());
    TEST_ASSERT_TRUE(mv.message_is_warning);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Move up / Move down (menu i Alt+Up / Alt+Down) pošlou move se správným cílem; na krajích a při více vybraných zakázané.
 */
void test_h4_move_up_down(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    st_DEVICE_REQ_ARGS a;
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_FALSE(w2_item("ctx_move_up")->disabled);
    TEST_ASSERT_FALSE(w2_item("ctx_move_down")->disabled);
    w3_click_at(&v, "ctx_move_up", 0);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(2, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[1]);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, (unsigned)a.i[5]);
    w3_close_popups(&v);
    w3_click_at(&v, "name_2", 1);
    w3_click_at(&v, "ctx_move_down", 0);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(2, a.i[0]);
    TEST_ASSERT_EQUAL_INT(3, a.i[1]);
    w3_close_popups(&v);
    /* kraje */
    w3_click_at(&v, "name_0", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_move_up")->disabled);
    TEST_ASSERT_EQUAL_STRING("This is the first block.", w2_item("ctx_move_up")->reason.c_str());
    TEST_ASSERT_FALSE(w2_item("ctx_move_down")->disabled);
    w3_click_at(&v, "ctx_move_up", 0);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL));
    w3_close_popups(&v);
    w3_click_at(&v, "name_4", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_move_down")->disabled);
    TEST_ASSERT_EQUAL_STRING("This is the last block.", w2_item("ctx_move_down")->reason.c_str());
    w3_close_popups(&v);
    /* více vybraných */
    imgui_cmt_window_test_set_selected({1, 2}, v.tape_serial);
    w2_frame(&v);
    w3_click_at(&v, "name_2", 1);
    TEST_ASSERT_TRUE(w2_item("ctx_move_up")->disabled);
    TEST_ASSERT_EQUAL_STRING("Select a single block to move it.", w2_item("ctx_move_up")->reason.c_str());
    TEST_ASSERT_TRUE(w2_item("ctx_move_down")->disabled);
    TEST_ASSERT_EQUAL_STRING("Select a single block to move it.", w2_item("ctx_move_down")->reason.c_str());
    TEST_ASSERT_FALSE_MESSAGE(w2_item("ctx_delete")->disabled, "Delete works with several blocks");
    w3_close_popups(&v);
    /* pořadí položek za Set speed: Move up, Move down, Import file here..., Delete, pak Properties */
    w3_click_at(&v, "name_3", 1);
    float y_speed = w2_item("ctx_speed")->min.y;
    float y_up = w2_item("ctx_move_up")->min.y, y_down = w2_item("ctx_move_down")->min.y;
    float y_imp = w2_item("ctx_import_here")->min.y, y_del = w2_item("ctx_delete")->min.y;
    float y_prop = w2_item("ctx_properties")->min.y;
    TEST_ASSERT_TRUE(y_speed < y_up && y_up < y_down && y_down < y_imp && y_imp < y_del && y_del < y_prop);
    TEST_ASSERT_EQUAL_STRING("Import file here...", w2_text("ctx_import_here").c_str());
    w3_close_popups(&v);

    /* Alt+Up / Alt+Down při fokusu tabulky s jedním vybraným blokem */
    w2_click(&v, "name_3");
    w3_drain();
    h4_key(&v, ImGuiKey_UpArrow, ImGuiMod_Alt);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(3, a.i[0]);
    TEST_ASSERT_EQUAL_INT(2, a.i[1]);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("3", h4_sel_text().c_str(), "Alt+Up does not move the selection by navigation");
    h4_key(&v, ImGuiKey_DownArrow, ImGuiMod_Alt);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(3, a.i[0]);
    TEST_ASSERT_EQUAL_INT(4, a.i[1]);
    /* Alt+Up na prvním bloku a při více vybraných nic */
    w2_click(&v, "name_0");
    w3_drain();
    h4_key(&v, ImGuiKey_UpArrow, ImGuiMod_Alt);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL));
    h4_click_mod(&v, "name_1", ImGuiMod_Ctrl);
    w3_drain();
    h4_key(&v, ImGuiKey_DownArrow, ImGuiMod_Alt);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL));
    TEST_ASSERT_EQUAL_STRING("0,1", h4_sel_text().c_str());
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Tažení řádku myší: ze stisku na řádku a pohybu myši (stisk drží tlačítko).
 * @param v Kopie. @param from Prvek řádku, kde tažení začne. @param to Bod, kam se myš přesune.
 */
static void h4_drag_to(st_CMT_VIEW *v, const char *from, ImVec2 to)
{
    ImVec2 c = h4_center(from);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(c.x, c.y);
    w2_frame(v);
    io.AddMouseButtonEvent(0, true);
    w2_frame(v);
    io.AddMousePosEvent(c.x, c.y - 12.0f);
    w2_frame(v);
    io.AddMousePosEvent(to.x, to.y);
    w2_frame(v);
    w2_frame(v);
}

/** @brief Pustí tlačítko myši (konec tažení) a odjede myší pryč. */
static void h4_release(st_CMT_VIEW *v)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMouseButtonEvent(0, false);
    w2_frame(v);
    io.AddMousePosEvent(-1.0f, -1.0f);
    w2_frame(v);
    w2_frame(v);
}

/**
 * @brief Tažení řádku 4 nad řádek 1 pošle move(4, 1); čára místa vložení mezi řádky 0 a 1; puštění mimo tabulku a Escape nic.
 */
void test_h4_drag_row(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    ImVec2 c1 = h4_center("name_1");
    const st_CMT_WINDOW_TEST_ITEM *n1 = w2_item("name_1");
    h4_drag_to(&v, "name_4", ImVec2(c1.x, n1->min.y + 1.0f));
    const st_CMT_WINDOW_TEST_ITEM *line = w2_item("drop_line");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "the insertion line is drawn while dragging over the table");
    float ly = (line->min.y + line->max.y) * 0.5f;
    TEST_ASSERT_TRUE_MESSAGE(ly >= w2_item("name_0")->max.y && ly <= w2_item("name_1")->min.y, "the line lies between rows 0 and 1");
    /* tloušťka 2 px x měřítko (při 28 px a 100 % 2 px), přes celou šířku tabulky */
    TEST_ASSERT_FLOAT_WITHIN(0.6f, 2.0f * ImGui::GetFontSize() / 28.0f, line->max.y - line->min.y);
    const st_CMT_WINDOW_TEST_ITEM *tbl = w2_item("table");
    TEST_ASSERT_TRUE(line->min.x <= w2_item("num_1")->min.x && line->max.x >= w2_item("status_1")->max.x);
    TEST_ASSERT_TRUE(line->min.x >= tbl->min.x - 0.5f && line->max.x <= tbl->max.x + 0.5f);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL));
    h4_release(&v);
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(4, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[1]);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, (unsigned)a.i[5]);
    TEST_ASSERT_NULL(w2_item("drop_line"));

    /* dolů: řádek 0 pod řádek 2 (mezera 3) = move(0, 2) */
    const st_CMT_WINDOW_TEST_ITEM *n3 = w2_item("name_3");
    h4_drag_to(&v, "name_0", ImVec2(c1.x, n3->min.y + 1.0f));
    h4_release(&v);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(0, a.i[0]);
    TEST_ASSERT_EQUAL_INT(2, a.i[1]);
    /* na vlastní místo nic */
    h4_drag_to(&v, "name_2", ImVec2(c1.x, w2_item("name_2")->min.y + 1.0f));
    h4_release(&v);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), "a drop on its own place does nothing");
    /* puštění mimo tabulku nic */
    h4_drag_to(&v, "name_4", h4_center("counter_area"));
    TEST_ASSERT_NULL(w2_item("drop_line"));
    h4_release(&v);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), "a drop outside the table does nothing");
    /* Escape během tažení zruší */
    h4_drag_to(&v, "name_4", ImVec2(c1.x, n1->min.y + 1.0f));
    TEST_ASSERT_NOT_NULL(w2_item("drop_line"));
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    w2_frame(&v);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    w2_frame(&v);
    TEST_ASSERT_NULL_MESSAGE(w2_item("drop_line"), "Escape cancels the drag");
    h4_release(&v);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), "Escape cancels the drag");
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Všechny nové akce zakázané mimo STOP, při nahrávání a při záznamu i přehrávání vstupů (zkratky, tažení i menu).
 */
void test_h4_edits_disabled(void)
{
    struct { const char *ctx; int state; bool rep_rec; bool rep_play; const char *reason; } cases[] = {
        {"PLAY", CMT_VIEW_STATE_PLAY, false, false, "Stop the tape first."},
        {"RECORD", CMT_VIEW_STATE_RECORD, false, false, "Stop the tape first."},
        {"input recording", CMT_VIEW_STATE_STOP, true, false, "Not available while an input replay is recorded."},
        {"input playback", CMT_VIEW_STATE_STOP, false, true, "Not available while an input replay is played."},
    };
    for (auto &c : cases) {
        st_CMT_VIEW v;
        h4_setup(&v);
        if (c.state == CMT_VIEW_STATE_RECORD) cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_RECORDING);
        v.state = (en_CMT_VIEW_STATE)c.state;
        v.replay_recording = c.rep_rec;
        v.replay_playing = c.rep_play;
        for (int i = 0; i < 3; i++) w2_frame(&v);
        w3_drain();
        w3_click_at(&v, "name_2", 1);
        const char *items[] = {"ctx_move_up", "ctx_move_down", "ctx_import_here", "ctx_delete"};
        for (const char *nm : items) {
            const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s: %s", c.ctx, nm);
            TEST_ASSERT_NOT_NULL_MESSAGE(it, msg);
            TEST_ASSERT_TRUE_MESSAGE(it->disabled, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, it->reason.c_str(), msg);
        }
        w3_click_at(&v, "ctx_delete", 0);
        w3_close_popups(&v);
        TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), c.ctx);
        /* menu Tape */
        w3_click_at(&v, "menu_tape", 0);
        TEST_ASSERT_TRUE_MESSAGE(w2_item("menu_import")->disabled, c.ctx);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, w2_item("menu_import")->reason.c_str(), c.ctx);
        TEST_ASSERT_TRUE_MESSAGE(w2_item("menu_export_tape")->disabled, c.ctx);
        w3_close_popups(&v);
        /* zkratky a tažení: každá kontrola hned po své akci (w3_take_request() vyprázdní frontu);
         * zakázaná zkratka ukáže důvod v řádku zpráv */
        st_CMT_VIEW mv;
        cmt_view_reset(&mv);
        fill(&mv);
        char msg[96];
        w2_click(&v, "name_2");
        w3_drain();
        cmt_view_post_message("-", false);
        h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
        snprintf(msg, sizeof(msg), "%s: Del", c.ctx);
        TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL), msg);
        TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, g3f_message(&mv).c_str(), msg);
        TEST_ASSERT_TRUE_MESSAGE(mv.message_is_warning, msg);
        cmt_view_post_message("-", false);
        h4_key(&v, ImGuiKey_UpArrow, ImGuiMod_Alt);
        snprintf(msg, sizeof(msg), "%s: Alt+Up", c.ctx);
        TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, g3f_message(&mv).c_str(), msg);
        h4_key(&v, ImGuiKey_DownArrow, ImGuiMod_Alt);
        snprintf(msg, sizeof(msg), "%s: Alt+Down", c.ctx);
        TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), msg);
        cmt_view_post_message("-", false);
        h4_key(&v, ImGuiKey_I, ImGuiMod_Ctrl);
        snprintf(msg, sizeof(msg), "%s: Ctrl+I", c.ctx);
        TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_tape_test_take_last_import(NULL, NULL), msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, g3f_message(&mv).c_str(), msg);
        h4_drag_to(&v, "name_4", h4_center("name_1"));
        TEST_ASSERT_NULL_MESSAGE(w2_item("drop_line"), c.ctx);
        h4_release(&v);
        snprintf(msg, sizeof(msg), "%s: drag", c.ctx);
        TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), msg);
        w3_drain();
        imgui_cmt_window_test_reset();
    }
}

/**
 * @brief Import file... (menu, Ctrl+I) na konec, Import file here... před řádek; Export tape as WAV...; prázdná páska a bez pásky.
 */
void test_h4_import_export_menu(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    int at = 99;
    unsigned serial = 0;
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_EQUAL_STRING("Import file...", w2_text("menu_import").c_str());
    TEST_ASSERT_FALSE(w2_item("menu_import")->disabled);
    TEST_ASSERT_TRUE(w2_item("menu_open")->min.y < w2_item("menu_import")->min.y);
    TEST_ASSERT_EQUAL_STRING("Export tape as WAV...", w2_text("menu_export_tape").c_str());
    w3_click_at(&v, "menu_import", 0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_import(&at, &serial));
    TEST_ASSERT_EQUAL_INT(-1, at);
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, serial);
    w3_close_popups(&v);
    w2_click(&v, "counter_area");
    h4_key(&v, ImGuiKey_I, ImGuiMod_Ctrl);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_import(&at, NULL));
    TEST_ASSERT_EQUAL_INT(-1, at);
    w3_click_at(&v, "name_3", 1);
    w3_click_at(&v, "ctx_import_here", 0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_import(&at, &serial));
    TEST_ASSERT_EQUAL_INT(3, at);
    w3_close_popups(&v);
    w3_click_at(&v, "menu_tape", 0);
    w3_click_at(&v, "menu_export_tape", 0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(&serial));
    TEST_ASSERT_EQUAL_UINT(v.tape_serial, serial);
    w3_close_popups(&v);
    /* prázdná páska: export zakázaný, import ano */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_EMPTY_TAPE);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_TRUE(w2_item("menu_export_tape")->disabled);
    TEST_ASSERT_EQUAL_STRING("The tape is empty.", w2_item("menu_export_tape")->reason.c_str());
    TEST_ASSERT_FALSE(w2_item("menu_import")->disabled);
    w3_close_popups(&v);
    /* bez pásky: import do nové pásky s čítačem 0 (i z výchozího čítače kopie) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_NO_TAPE);
    v.tape_serial = 0xFFFFFFFFu; /* výchozí čítač kopie před prvním plněním */
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_FALSE(w2_item("menu_import")->disabled);
    TEST_ASSERT_TRUE(w2_item("menu_export_tape")->disabled);
    TEST_ASSERT_EQUAL_STRING("No tape inserted.", w2_item("menu_export_tape")->reason.c_str());
    w3_click_at(&v, "menu_import", 0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_import(&at, &serial));
    TEST_ASSERT_EQUAL_INT(-1, at);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, serial, "an import into an empty deck carries tape serial 0");
    w3_close_popups(&v);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Událost přetažení souboru z OS (SDL) nad testovacím oknem.
 * @param type SDL_EVENT_DROP_*. @param x, y Poloha v okně SDL. @param file Soubor (jen DROP_FILE).
 */
static void h4_drop(Uint32 type, float x, float y, const char *file)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.drop.windowID = SDL_GetWindowID(s_win);
    e.drop.x = x;
    e.drop.y = y;
    e.drop.data = file;
    imgui_cmt_window_os_drop_event(&e);
}

/**
 * @brief Přetažení souboru z OS: nad tabulkou import na místo pod kurzorem (čára během tažení), nad deckem otevření, jinde nic.
 *
 * Více souborů najednou: první se zařadí hned, další až po výsledku
 * předchozího (zásah zůstává jeden). Úpravy zakázané (PLAY) = žádný import.
 */
void test_h4_os_drop(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    const st_CMT_WINDOW_TEST_ITEM *n2 = w2_item("name_2");
    ImVec2 p(h4_center("name_2").x, n2->min.y + 1.0f);
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_POSITION, p.x, p.y, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const st_CMT_WINDOW_TEST_ITEM *line = w2_item("drop_line");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "the insertion line is drawn while a file is dragged over the table");
    float ly = (line->min.y + line->max.y) * 0.5f;
    TEST_ASSERT_TRUE(ly >= w2_item("name_1")->max.y && ly <= w2_item("name_2")->min.y);
    h4_drop(SDL_EVENT_DROP_FILE, p.x, p.y, "C:/tapes/prog1.mzf");
    h4_drop(SDL_EVENT_DROP_FILE, p.x, p.y, "C:/tapes/prog2.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    st_DEVICE_REQ_ARGS a;
    const char *n;
    int imports = 0;
    while (device_request_test_take_pending(&n, &a)) {
        if (strcmp(n, CMT_REQUEST_IMPORT_NAME) != 0) continue;
        imports++;
        TEST_ASSERT_EQUAL_STRING("C:/tapes/prog1.mzf", a.str);
        TEST_ASSERT_EQUAL_INT(2, a.i[0]);
        TEST_ASSERT_EQUAL_UINT(v.tape_serial, (unsigned)a.i[5]);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, imports, "the second file waits for the result of the first import");
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_NULL_MESSAGE(w2_item("drop_line"), "no line after the drop");
    imgui_cmt_tape_test_reset();

    /* pod posledním řádkem (prázdná část tabulky) = na konec */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_CONTROL_BLOCKS); /* STOP, 7 bloků */
    imgui_cmt_window_test_set_props_open(0u);
    for (int i = 0; i < 3; i++) w2_frame(&v);
    /* okno 23 výšek rámečku: 7 řádků se vejde a pod nimi zbude místo, jinak test nic neověří */
    const st_CMT_WINDOW_TEST_ITEM *tbl = w2_item("table");
    const st_CMT_WINDOW_TEST_ITEM *last = w2_item("name_6");
    TEST_ASSERT_NOT_NULL(last);
    if (tbl->max.y - last->max.y > 8.0f) {
        ImVec2 q(h4_center("name_6").x, tbl->max.y - 3.0f);
        h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
        h4_drop(SDL_EVENT_DROP_FILE, q.x, q.y, "C:/tapes/end.mzf");
        h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
        for (int i = 0; i < 2; i++) w2_frame(&v);
        TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_IMPORT_NAME, &a));
        TEST_ASSERT_EQUAL_INT(7, a.i[0]);
        imgui_cmt_tape_test_reset();
    }

    /* nad deckem: otevření obvyklým postupem (akce automatu po uvolnění zámku) */
    cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    v.state = CMT_VIEW_STATE_STOP;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    w3_drain();
    (void)imgui_cmt_tape_test_take_last_run();
    ImVec2 d = h4_center("counter_area");
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, d.x, d.y, "C:/tapes/other.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_IMPORT_NAME, NULL), "no import over the deck");
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_OPEN_FILE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_TRUE(w3_take_request("cmt_open", &a));
    TEST_ASSERT_EQUAL_STRING("C:/tapes/other.tmz", a.str);

    /* mimo okno Virtual CMT nic */
    ImVec2 out((float)s_canvas_w - 20.0f, (float)s_canvas_h - 20.0f);
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, out.x, out.y, "C:/tapes/outside.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_TRUE_MESSAGE(g3f_nothing(), "a drop outside the window does nothing");

    /* PLAY: nad tabulkou žádný import, zpráva s důvodem */
    v.state = CMT_VIEW_STATE_PLAY;
    for (int i = 0; i < 2; i++) w2_frame(&v);
    st_CMT_VIEW mv;
    cmt_view_reset(&mv);
    fill(&mv);
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_POSITION, p.x, p.y, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_NULL(w2_item("drop_line"));
    h4_drop(SDL_EVENT_DROP_FILE, p.x, p.y, "C:/tapes/prog1.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(g3f_nothing());
    TEST_ASSERT_EQUAL_STRING("Stop the tape first.", g3f_message(&mv).c_str());
    imgui_cmt_tape_test_reset();
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Obnova výběru po výsledku vlastní úpravy (skutečné jádro): přesun, smazání, import; jiný čítač výběr neobnoví.
 *
 * Výsledky jdou cestou aplikace imgui_device_request_results() (vyzvednutí
 * a čtení výsledku úpravy v jednom držení zámku).
 */
void test_h4_selection_restore(void)
{
    open_games_tape(); /* 5 bloků, SAVED GAME neuložený */
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    /* kopie od začátku nad jádrem (čítače fixture by sestavení bloků mohly přeskočit) */
    cmt_view_reset(&v);
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_window_test_set_selected({0}, v.tape_serial);
    w2_frame(&v);
    /* přesun 0 -> 2 */
    TEST_ASSERT_TRUE(imgui_cmt_tape_move_block(v.tape_serial, 0, 2) > 0);
    device_request_poll();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("LOADER", v.blocks[2].name.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2", h4_sel_text().c_str(), "the moved block stays selected");
    /* smazání 1 a 2: vybraný je blok na místě prvního smazaného */
    int del[2] = {1, 2};
    TEST_ASSERT_TRUE(imgui_cmt_tape_remove_blocks(v.tape_serial, del, 2) > 0);
    device_request_poll();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(3, v.block_count);
    TEST_ASSERT_EQUAL_STRING("1", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_STRING("Deleted 2 blocks.", v.message.c_str());
    /* import před blok 0: vybraný první importovaný */
    const char *mzt = make_two_block_mzt();
    const char *paths[1] = {mzt};
    imgui_cmt_tape_import_paths(paths, 1, 0, v.tape_serial);
    device_request_poll();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(5, v.block_count);
    TEST_ASSERT_EQUAL_STRING("0", h4_sel_text().c_str());
    TEST_ASSERT_TRUE(v.blocks[0].unsaved);
    TEST_ASSERT_EQUAL_STRING("Unsaved", v.blocks[0].status_text.c_str());
    /* výsledek, jehož páska se mezitím znovu změnila: výběr se neobnoví */
    imgui_cmt_window_test_set_selected({}, v.tape_serial);
    w2_frame(&v);
    TEST_ASSERT_TRUE(imgui_cmt_tape_move_block(v.tape_serial, 4, 0) > 0);
    device_request_poll();
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 2)); /* další úprava (MCP) před plněním kopie */
    device_state_unlock();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", h4_sel_text().c_str(), "no selection restored over a newer tape state");
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Sloupec Status: Unsaved podle příznaku bloku, ne podle polohy (neuložený blok přesunutý na začátek pásky).
 */
void test_h4_unsaved_by_flag(void)
{
    open_games_tape(); /* SAVED GAME (4) neuložený */
    st_CMT_VIEW v;
    cmt_view_reset(&v);
    fill(&v);
    TEST_ASSERT_TRUE(v.blocks[4].unsaved);
    TEST_ASSERT_FALSE(v.blocks[0].unsaved);
    /* surový záznam ze souboru: uložený, stav Not decoded */
    TEST_ASSERT_FALSE(v.blocks[3].unsaved);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(4, 0));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_STRING("SAVED GAME", v.blocks[0].name.c_str());
    TEST_ASSERT_TRUE(v.blocks[0].unsaved);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_UNSAVED, v.blocks[0].status);
    TEST_ASSERT_EQUAL_STRING("Unsaved", v.blocks[0].status_text.c_str());
    for (int i = 1; i < 5; i++) TEST_ASSERT_FALSE(v.blocks[(size_t)i].unsaved);
    TEST_ASSERT_EQUAL_STRING("LOADER", v.blocks[1].name.c_str());
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_OK, v.blocks[1].status);

    /* surový neuložený blok (import WAV): Unsaved má přednost před Not decoded */
    GByteArray *ba = g_byte_array_new();
    const uint32_t n = 8000, rate = 44100;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t v32 = 36 + n;
    memcpy(hdr + 4, &v32, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    v32 = 16;
    memcpy(hdr + 16, &v32, 4);
    uint16_t v16 = 1;
    memcpy(hdr + 20, &v16, 2);
    memcpy(hdr + 22, &v16, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &rate, 4);
    memcpy(hdr + 32, &v16, 2);
    v16 = 8;
    memcpy(hdr + 34, &v16, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &n, 4);
    g_byte_array_append(ba, hdr, sizeof(hdr));
    for (uint32_t i = 0; i < n; i++) {
        uint8_t s = ((i / 20) & 1) ? 0xE0 : 0x20;
        g_byte_array_append(ba, &s, 1);
    }
    const char *wav = tmp_named(2, "raw.wav");
    TEST_ASSERT_TRUE(g_file_set_contents(wav, (const gchar *)ba->data, ba->len, NULL));
    g_byte_array_free(ba, TRUE);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(wav, 5, NULL));
    device_state_unlock();
    fill(&v);
    TEST_ASSERT_EQUAL_INT(6, v.block_count);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[5].kind);
    TEST_ASSERT_TRUE(v.blocks[5].unsaved);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_UNSAVED, v.blocks[5].status);
    TEST_ASSERT_EQUAL_STRING("Unsaved", v.blocks[5].status_text.c_str());
    /* surový blok ze souboru pásky (uložený) zůstává Not decoded */
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[4].kind);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_STATUS_NOT_DECODED, v.blocks[4].status);
}

/**
 * @brief Úprava pásky, zatímco čeká dialog souboru (export bloku, export pásky, import) - akce se zruší se zprávou.
 *
 * Skutečné jádro: dialog se otevře nad páskou, pak úprava (přesun bloku)
 * zvedne čítač výměn pásky; dokončený dialog nic nezařadí nad jiným
 * blokem ani pásku neexportuje. Import nese čítač na emu vlákno, které ho
 * odmítne (CMT_REQUEST_RC_TAPE_CHANGED).
 */
void test_h4_edit_while_dialog_open(void)
{
    open_games_tape();
    st_CMT_VIEW mv;
    cmt_view_reset(&mv);
    fill(&mv);
    unsigned s0 = mv.tape_serial;
    const char *wav = tmp_named(2, "tape.wav");
    /* export bloku 3 */
    device_state_lock();
    imgui_cmt_tape_export_block(3, true, s0);
    imgui_cmt_tape_export_tape(s0);
    device_state_unlock();
    imgui_cmt_tape_test_take_last_export(NULL, NULL);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(NULL));
    imgui_cmt_tape_import(1, s0);
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_import(NULL, NULL));
    /* úprava pásky (přesun) */
    TEST_ASSERT_TRUE(imgui_cmt_tape_move_block(s0, 4, 0) > 0);
    device_request_poll();
    imgui_device_request_results();
    fill(&mv);
    TEST_ASSERT_NOT_EQUAL_UINT(s0, mv.tape_serial);
    imgui_cmt_tape_test_export_finish(3, s0, wav);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request("cmt_export_block", NULL), "no block export after an edit");
    TEST_ASSERT_EQUAL_STRING(H4_CHANGED, g3f_message(&mv).c_str());
    cmt_view_post_message("-", false);
    imgui_cmt_tape_test_export_tape_finish(wav);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_EXPORT_TAPE_NAME, NULL), "no tape export after an edit");
    TEST_ASSERT_EQUAL_STRING(H4_CHANGED, g3f_message(&mv).c_str());
    cmt_view_post_message("-", false);
    imgui_cmt_tape_test_import_finish(make_two_block_mzt());
    device_request_poll();
    imgui_device_request_results();
    TEST_ASSERT_EQUAL_STRING(H4_CHANGED, g3f_message(&mv).c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, mv.block_count, "nothing imported into the edited tape");
    TEST_ASSERT_FALSE(g_file_test(wav, G_FILE_TEST_EXISTS));
    /* tatáž páska: export proběhne */
    device_state_lock();
    imgui_cmt_tape_export_tape(mv.tape_serial);
    device_state_unlock();
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(NULL));
    imgui_cmt_tape_test_export_tape_finish(wav);
    device_request_poll();
    imgui_device_request_results();
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_INT(0, strncmp("Tape exported: ", g3f_message(&mv).c_str(), 15));
    imgui_cmt_tape_test_reset();
}

/**
 * @brief Geometrie nových prvků: kontextové menu s novými položkami, modál mazání a čára místa vložení
 *        se neořežou ani nepřekryjí při 100 % a 175 %, s texty běžnými i o 40 % delšími.
 */
void test_h4_geometry(void)
{
    const float scales[2] = {1.0f, 1.75f};
    int faults = 0;
    for (float sc : scales) {
        for (int stretch = 0; stretch < 2; stretch++) {
            st_CMT_VIEW v;
            imgui_cmt_window_test_set_stretch(stretch != 0);
            w2_fonts();
            w2_apply_scale(sc);
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            imgui_cmt_window_test_reset();
            imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
            cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
            v.state = CMT_VIEW_STATE_STOP;
            for (int i = 0; i < 4; i++) w2_frame(&v);
            char ctx[96];
            /* kontextové menu (více vybraných: Delete N blocks) */
            imgui_cmt_window_test_set_selected({3, 4}, v.tape_serial);
            w2_frame(&v);
            w3_click_at(&v, "name_4", 1);
            snprintf(ctx, sizeof(ctx), "h4 menu %.2f %s", sc, stretch ? "+40%" : "plain");
            TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("ctx_delete"), ctx);
            faults += w2_geometry_faults(ctx);
            for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items())
                if (it.popup && (it.max.x > (float)s_canvas_w || it.max.y > (float)s_canvas_h)) {
                    fprintf(stderr, "%s: '%s' outside the screen\n", ctx, it.name.c_str());
                    faults++;
                }
            /* modál mazání */
            w3_click_at(&v, "ctx_delete", 0);
            snprintf(ctx, sizeof(ctx), "h4 modal %.2f %s", sc, stretch ? "+40%" : "plain");
            TEST_ASSERT_TRUE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), ctx);
            faults += w2_geometry_faults(ctx);
            const st_CMT_WINDOW_TEST_ITEM *t = w2_item("del_text");
            const st_CMT_WINDOW_TEST_ITEM *b1 = w2_item("del_confirm");
            const st_CMT_WINDOW_TEST_ITEM *b2 = w2_item("del_cancel");
            TEST_ASSERT_NOT_NULL(t);
            if (t->max.x > (float)s_canvas_w || b2->max.x > (float)s_canvas_w || b2->max.y > (float)s_canvas_h) {
                fprintf(stderr, "%s: the dialog does not fit the screen\n", ctx);
                faults++;
            }
            TEST_ASSERT_TRUE_MESSAGE(b1->max.x <= b2->min.x, ctx);
            w3_click_at(&v, "del_cancel", 0);
            /* čára místa vložení */
            ImVec2 c1 = h4_center("name_1");
            h4_drag_to(&v, "name_4", ImVec2(c1.x, w2_item("name_1")->min.y + 1.0f));
            snprintf(ctx, sizeof(ctx), "h4 drag %.2f %s", sc, stretch ? "+40%" : "plain");
            TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("drop_line"), ctx);
            faults += w2_geometry_faults(ctx);
            h4_release(&v);
            w3_drain();
            /* první a poslední mezera (okraj těla tabulky): čára celá uvnitř ořezu */
            h4_drag_to(&v, "name_4", ImVec2(c1.x, w2_item("name_0")->min.y + 1.0f));
            faults += w2_geometry_faults(ctx);
            ImGuiIO &io = ImGui::GetIO();
            io.AddMousePosEvent(c1.x, w2_item("table")->max.y - 2.0f);
            w2_frame(&v);
            w2_frame(&v);
            faults += w2_geometry_faults(ctx);
            h4_release(&v);
            w3_drain();
        }
    }
    imgui_cmt_window_test_set_stretch(false);
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, faults, "tape edit items (context menu, delete modal, insert line) clipped or overlapping (details on stderr)");
}


/* ================================================================
 * Úpravy pásky, další případy: tažení s čítačem pásky, klávesy během tažení,
 * automatický posun, fronta Alt+šipek, modály uprostřed okna, vzhled.
 * ================================================================ */

/** @brief Kopie stavu pro čtení řádku zpráv v těchto testech (první plnění převezme jen čítač zpráv). */
static st_CMT_VIEW s_wave_mv;

/** @brief Začne sledovat řádek zpráv: zprávy po tomto volání čte wave_message(). */
static void wave_msg_begin(void)
{
    cmt_view_reset(&s_wave_mv);
    fill(&s_wave_mv);
}

/** @brief Poslední zpráva řádku zpráv od wave_msg_begin(). @param[out] warning Úroveň varování (smí být NULL). */
static std::string wave_message(bool *warning)
{
    fill(&s_wave_mv);
    if (warning) *warning = s_wave_mv.message_is_warning;
    return s_wave_mv.message;
}

/*
 * Tažení řádku nese čítač výměn pásky ze začátku tažení.
 * Změna pásky během tažení tažení zruší s varováním a puštění nic
 * nepřesune; klávesy úprav (Del, Alt+Up, Alt+Down, Ctrl+I) se během tažení
 * ignorují a puštění pak přesune ten blok, který uživatel chytil.
 */
void test_wave_drag_carries_tape_serial(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    ImVec2 c1 = h4_center("name_1");
    float y1 = w2_item("name_1")->min.y + 1.0f;
    wave_msg_begin();
    h4_drag_to(&v, "name_4", ImVec2(c1.x, y1));
    TEST_ASSERT_NOT_NULL(w2_item("drop_line"));
    /* páska se změní (MCP, jiná páska): tažení se zruší */
    v.tape_serial++;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    TEST_ASSERT_NULL_MESSAGE(w2_item("drop_line"), "the drag is cancelled when the tape changes");
    bool warn = false;
    TEST_ASSERT_EQUAL_STRING(H4_CHANGED, wave_message(&warn).c_str());
    TEST_ASSERT_TRUE(warn);
    h4_release(&v);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), "nothing is moved over another tape");
    w3_drain();

    /* klávesy úprav během tažení nic; puštění přesune chycený blok s čítačem ze začátku tažení */
    h4_setup(&v);
    w2_click(&v, "name_2");
    w3_drain();
    h4_drag_to(&v, "name_4", ImVec2(c1.x, y1));
    const unsigned serial = v.tape_serial;
    h4_key(&v, ImGuiKey_Delete, ImGuiKey_None);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_REMOVE_BLOCKS_NAME, NULL), "Del is ignored while dragging");
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
    h4_key(&v, ImGuiKey_UpArrow, ImGuiMod_Alt);
    h4_key(&v, ImGuiKey_DownArrow, ImGuiMod_Alt);
    TEST_ASSERT_FALSE_MESSAGE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL), "Alt+arrows are ignored while dragging");
    h4_key(&v, ImGuiKey_I, ImGuiMod_Ctrl);
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_tape_test_take_last_import(NULL, NULL), "Ctrl+I is ignored while dragging");
    ImGui::GetIO().AddMousePosEvent(c1.x, y1);
    w2_frame(&v);
    w2_frame(&v);
    h4_release(&v);
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, &a));
    TEST_ASSERT_EQUAL_INT(4, a.i[0]);
    TEST_ASSERT_EQUAL_INT(1, a.i[1]);
    TEST_ASSERT_EQUAL_UINT(serial, (unsigned)a.i[5]);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/*
 * Soubor z OS nad hlavičkou tabulky = import na konec a čára
 * místa vložení na konci už během tažení.
 */
void test_wave_os_drop_over_header(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    ImVec2 h = h4_center("hdr_2");
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_POSITION, h.x, h.y, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    const st_CMT_WINDOW_TEST_ITEM *line = w2_item("drop_line");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "the insertion line is shown over the header");
    float ly = (line->min.y + line->max.y) * 0.5f;
    TEST_ASSERT_TRUE_MESSAGE(ly >= w2_item("name_4")->max.y - 2.5f, "the line is at the end of the tape");
    h4_drop(SDL_EVENT_DROP_FILE, h.x, h.y, "C:/tapes/head.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_IMPORT_NAME, &a));
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, a.i[0], "a drop over the header imports at the end");
    imgui_cmt_tape_test_reset();
    w3_drain();
    imgui_cmt_window_test_reset();
}

/*
 * Soubor puštěný na tabulku, když je nad oknem otevřený
 * obyčejný popup okna kazety (kontextové menu řádku, menu Tape), se
 * nezahodí mlčky: nic se neimportuje ani neotevře a řádek zpráv ukáže
 * varování "Finish the open dialog first." jako u modálu.
 */
void test_final_os_drop_over_popup_warns(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    (void)imgui_cmt_tape_test_take_last_run();
    bool warn = false;
    /* kontextové menu řádku */
    w3_click_at(&v, "name_4", 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("ctx_play"), "the row context menu is open");
    ImVec2 t = h4_center("name_1");
    wave_msg_begin();
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, t.x, t.y, "C:/tapes/under_menu.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_IMPORT_NAME, NULL));
    TEST_ASSERT_EQUAL_STRING("Finish the open dialog first.", wave_message(&warn).c_str());
    TEST_ASSERT_TRUE(warn);
    w3_close_popups(&v);

    /* menu Tape, soubor nad deckem (otevření) */
    w3_click_at(&v, "menu_tape", 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("menu_open"), "the Tape menu is open");
    ImVec2 d = h4_center("counter_area");
    wave_msg_begin();
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, d.x, d.y, "C:/tapes/under_tape_menu.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT_MESSAGE(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run(), "nothing is opened under a menu");
    TEST_ASSERT_FALSE(w3_take_request("cmt_open", NULL));
    TEST_ASSERT_EQUAL_STRING("Finish the open dialog first.", wave_message(&warn).c_str());
    TEST_ASSERT_TRUE(warn);
    w3_close_popups(&v);

    /* bez popupu stejné puštění importuje (kontrola testu) */
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, t.x, t.y, "C:/tapes/no_menu.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_TRUE(w3_take_request(CMT_REQUEST_IMPORT_NAME, NULL));
    imgui_cmt_tape_test_reset();
    w3_drain();
    imgui_cmt_window_test_reset();
}

/*
 * Přetažení, které se nepoužije, ohlásí jedno varování:
 * víc souborů nad deckem (otevře se první), puštění nad modál, puštění
 * během jiného dotazu automatu; nad sbaleným oknem se soubor otevře.
 */
void test_wave_os_drop_never_silent(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    (void)imgui_cmt_tape_test_take_last_run();
    ImVec2 d = h4_center("counter_area");
    bool warn = false;
    /* víc souborů nad deckem */
    wave_msg_begin();
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, d.x, d.y, "C:/tapes/first.tmz");
    h4_drop(SDL_EVENT_DROP_FILE, d.x, d.y, "C:/tapes/second.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Only one file can be opened as a tape.", wave_message(&warn).c_str());
    TEST_ASSERT_TRUE(warn);
    /* druhé přetažení, než automat vyřídil první: varování, nic dalšího se nezařadí */
    wave_msg_begin();
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, d.x, d.y, "C:/tapes/third.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("Finish the open dialog first.", wave_message(&warn).c_str());
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_OPEN_FILE, imgui_cmt_tape_test_take_last_run());
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(w3_take_request("cmt_open", &a));
    TEST_ASSERT_EQUAL_STRING("C:/tapes/first.tmz", a.str);
    imgui_cmt_tape_test_reset();
    w3_drain();

    /* modál nad oknem: soubor nad tabulkou nic neimportuje, varování */
    w3_click_at(&v, "name_4", 1);
    w3_click_at(&v, "ctx_delete", 0);
    TEST_ASSERT_TRUE(imgui_cmt_window_test_delete_modal_open());
    ImVec2 t = h4_center("name_1");
    wave_msg_begin();
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, t.x, t.y, "C:/tapes/under_modal.mzf");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_IMPORT_NAME, NULL));
    TEST_ASSERT_EQUAL_STRING("Finish the open dialog first.", wave_message(&warn).c_str());
    TEST_ASSERT_TRUE(warn);
    w3_click_at(&v, "del_cancel", 0);
    TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());

    /* sbalené okno: soubor nad titulkem se otevře */
    (void)imgui_cmt_tape_test_take_last_run();
    ImGui::SetWindowCollapsed(W2_ID, true);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
    TEST_ASSERT_TRUE(w && w->Collapsed);
    ImVec2 tb(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f);
    h4_drop(SDL_EVENT_DROP_BEGIN, 0.0f, 0.0f, NULL);
    h4_drop(SDL_EVENT_DROP_FILE, tb.x, tb.y, "C:/tapes/collapsed.tmz");
    h4_drop(SDL_EVENT_DROP_COMPLETE, 0.0f, 0.0f, NULL);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT_MESSAGE(IMGUI_CMT_TAPE_ACTION_OPEN_FILE, imgui_cmt_tape_test_take_last_run(), "a drop over a collapsed window opens");
    ImGui::SetWindowCollapsed(W2_ID, false);
    w2_frame(&v);
    imgui_cmt_tape_test_reset();
    w3_drain();
    imgui_cmt_window_test_reset();
}

/*
 * Tažení k dolnímu okraji tabulky ji posouvá (po celých
 * řádcích), hlouběji v zóně (řádek u okraje) rychleji; Escape tažení zruší.
 */
void test_wave_drag_autoscroll(void)
{
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_MANY_BLOCKS);
    v.state = CMT_VIEW_STATE_STOP;
    v.motor_known = false;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    w3_drain();
    s_w2_dt = 1.0f / 60.0f;
    ImGuiTable *tbl = ImGui::TableFindByID(imgui_cmt_window_test_table_id());
    TEST_ASSERT_NOT_NULL(tbl);
    /* první kreslený řádek */
    char nm[32];
    int first = -1;
    for (int i = 0; i < 300 && first < 0; i++) {
        snprintf(nm, sizeof(nm), "name_%d", i);
        const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
        if (it && it->min.y >= it->clip_min.y) first = i;
    }
    TEST_ASSERT_TRUE(first >= 0);
    snprintf(nm, sizeof(nm), "name_%d", first);
    char nm2[32];
    snprintf(nm2, sizeof(nm2), "name_%d", first + 1);
    TEST_ASSERT_NOT_NULL(w2_item(nm2));
    float rh = w2_item(nm2)->min.y - w2_item(nm)->min.y;
    const st_CMT_WINDOW_TEST_ITEM *table = w2_item("table");
    float bottom = table->max.y - 1.0f;
    float x = h4_center(nm).x;
    float scrolled[2];
    const float depth_y[2] = {bottom - 0.5f * rh, bottom + 0.4f * rh};
    for (int k = 0; k < 2; k++) {
        h4_drag_to(&v, nm, ImVec2(x, depth_y[k]));
        float s0 = tbl->InnerWindow->Scroll.y;
        for (int f = 0; f < 30; f++) w2_frame(&v);
        scrolled[k] = (tbl->InnerWindow->Scroll.y - s0) / rh;
        float whole = tbl->InnerWindow->Scroll.y / rh;
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, floorf(whole + 0.5f), whole, "the table scrolls by whole rows");
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Escape, true);
        w2_frame(&v);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        w2_frame(&v);
        h4_release(&v);
        TEST_ASSERT_FALSE(w3_take_request(CMT_REQUEST_MOVE_BLOCK_NAME, NULL));
        /* další tažení z řádku, který je teď vidět */
        for (int i = 0; i < 300; i++) {
            snprintf(nm, sizeof(nm), "name_%d", i);
            const st_CMT_WINDOW_TEST_ITEM *it = w2_item(nm);
            if (it && it->min.y >= it->clip_min.y) break;
        }
    }
    s_w2_dt = 0.0f;
    char msg[96];
    snprintf(msg, sizeof(msg), "rows scrolled in 30 frames: half zone %.1f, edge %.1f", scrolled[0], scrolled[1]);
    TEST_ASSERT_TRUE_MESSAGE(scrolled[0] >= 1.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(scrolled[1] > scrolled[0], msg);
    w3_drain();
    imgui_cmt_window_test_reset();
}

/*
 * Druhé Alt+Up stisknuté dřív, než přišel výsledek prvního
 * přesunu, se neztratí - provede se nad přesunutým blokem po výsledku.
 */
void test_wave_alt_move_queued(void)
{
    open_games_tape();
    st_CMT_VIEW v;
    w3_setup(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
    cmt_view_reset(&v);
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[3].kind);
    w2_click(&v, "name_3");
    w3_drain();
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    w2_frame(&v);
    for (int k = 0; k < 2; k++) {
        io.AddKeyEvent(ImGuiKey_UpArrow, true);
        w2_frame(&v);
        io.AddKeyEvent(ImGuiKey_UpArrow, false);
        w2_frame(&v);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, device_request_pending(), "one move at a time");
    }
    /* výsledek prvního: čekající přesun se zařadí nad přesunutým blokem */
    device_request_poll();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_INT(CMT_VIEW_KIND_RAW, v.blocks[2].kind);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, device_request_pending(), "the queued Alt+Up is not lost");
    device_request_poll();
    imgui_device_request_results();
    fill(&v);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    w2_frame(&v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMT_VIEW_KIND_RAW, v.blocks[1].kind, "two Alt+Up moved the block by two places");
    TEST_ASSERT_EQUAL_STRING("1", h4_sel_text().c_str());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    w3_drain();
    imgui_cmt_window_test_reset();
}

/** @brief Střed modálu proti středu okna Virtual CMT (px, větší z odchylek x a y). @param ctx Popis pro hlášení. */
static float wave_modal_offset(const char *ctx)
{
    ImGuiWindow *modal = ImGui::GetTopMostPopupModal();
    ImGuiWindow *w = ImGui::FindWindowByName(W2_ID);
    TEST_ASSERT_NOT_NULL_MESSAGE(modal, ctx);
    TEST_ASSERT_NOT_NULL(w);
    float dx = (modal->Pos.x + modal->Size.x * 0.5f) - (w->Pos.x + w->Size.x * 0.5f);
    float dy = (modal->Pos.y + modal->Size.y * 0.5f) - (w->Pos.y + w->Size.y * 0.5f);
    TEST_ASSERT_TRUE_MESSAGE(modal->ViewportId == w->ViewportId, "the modal is in the viewport of the window");
    return ImMax(fabsf(dx), fabsf(dy));
}

/*
 * Modály okna (potvrzení mazání, editor vlastních
 * pulzů, dotaz na neuložené změny) se otevírají uprostřed okna kazety
 * (+-1 px) při různých polohách okna a při 100 % i 175 %; v modálu mazání
 * má hned po otevření myší viditelný fokus navigace Cancel.
 */
void test_wave_modals_centred(void)
{
    const float scales[2] = {1.0f, 1.75f};
    const ImVec2 poss[3] = {ImVec2(0.0f, 0.0f), ImVec2(260.0f, 140.0f), ImVec2(40.0f, 400.0f)};
    char ctx[96];
    for (float sc : scales) {
        for (const ImVec2 &pos : poss) {
            st_CMT_VIEW v;
            w2_fonts();
            w2_apply_scale(sc);
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
            imgui_cmt_window_test_reset();
            imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
            s_w2_pos = pos;
            cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
            v.state = CMT_VIEW_STATE_STOP;
            for (int i = 0; i < 4; i++) w2_frame(&v);
            w3_drain();
            snprintf(ctx, sizeof(ctx), "delete modal %.2f at %.0f,%.0f", sc, pos.x, pos.y);
            w3_click_at(&v, "name_4", 1);
            w3_click_at(&v, "ctx_delete", 0);
            for (int i = 0; i < 3; i++) w2_frame(&v);
            TEST_ASSERT_TRUE_MESSAGE(imgui_cmt_window_test_delete_modal_open(), ctx);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 0.0f, wave_modal_offset(ctx), ctx);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("cancel", w2_text("del_focus").c_str(), ctx);
            w3_click_at(&v, "del_cancel", 0);
            TEST_ASSERT_FALSE(imgui_cmt_window_test_delete_modal_open());
            /* editor vlastních pulzů výchozí rychlosti */
            st_MZTAPE_PULSES_LENGTH p;
            mztape_pulses_set_us(&p, 470.0, 494.0, 240.0, 278.0);
            ImGuiCmt::openPulsesEditor("cmt_window_speed", 0, &p);
            for (int i = 0; i < 4; i++) w2_frame(&v);
            snprintf(ctx, sizeof(ctx), "pulses editor %.2f at %.0f,%.0f", sc, pos.x, pos.y);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 0.0f, wave_modal_offset(ctx), ctx);
            imgui_cmt_window_test_reset();
            w2_frame(&v);
        }
    }
    /* dotaz na neuložené změny (automat pásky, imgui_cmt_tape_dialogs()) nad skutečnou páskou */
    open_games_tape();
    st_CMT_VIEW v;
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
    imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
    s_w2_pos = ImVec2(300.0f, 120.0f);
    cmt_view_reset(&v);
    fill(&v);
    s_w2_dialogs = true;
    for (int i = 0; i < 3; i++) w2_frame(&v);
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
    for (int i = 0; i < 4; i++) w2_frame(&v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 0.0f, wave_modal_offset("unsaved changes dialog"), "unsaved changes dialog");
    imgui_cmt_tape_test_reset();
    imgui_cmt_window_test_reset();
    w2_frame(&v);
    s_w2_dialogs = false;
    s_w2_pos = ImVec2(0.0f, 0.0f);
    w2_apply_scale(1.0f);
    imgui_cmt_window_test_reset();
}

/*
 * Vzhled: při více vybraných hlavička vlastností bez šipky;
 * vybrané řádky s pruhem accent u levé hrany; popisek taženého bloku
 * s číslem, vpravo pod kurzorem, nezakrývá řádky u čáry; během tažení bez
 * rámečku fokusu navigace.
 */
void test_wave_selection_props_drag_label(void)
{
    st_CMT_VIEW v;
    h4_setup(&v);
    w2_click(&v, "name_1");
    const st_CMT_WINDOW_TEST_ITEM *hdr1 = w2_item("props_header");
    TEST_ASSERT_NOT_NULL(hdr1);
    float h_single = hdr1->max.y - hdr1->min.y;
    TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("props_arrow"), "one block: the header has an arrow");
    imgui_cmt_window_test_set_selected({1, 3}, v.tape_serial);
    for (int i = 0; i < 2; i++) w2_frame(&v);
    TEST_ASSERT_EQUAL_STRING("2 blocks selected", w2_text("props_title").c_str());
    TEST_ASSERT_NULL_MESSAGE(w2_item("props_arrow"), "several blocks: no arrow (nothing to expand)");
    const st_CMT_WINDOW_TEST_ITEM *hdr2 = w2_item("props_header");
    TEST_ASSERT_FLOAT_WITHIN(1.0f, h_single, hdr2->max.y - hdr2->min.y);
    /* pruh výběru */
    TEST_ASSERT_NOT_NULL(w2_item("selbar_1"));
    TEST_ASSERT_NOT_NULL(w2_item("selbar_3"));
    TEST_ASSERT_NULL(w2_item("selbar_0"));
    const st_CMT_WINDOW_TEST_ITEM *bar = w2_item("selbar_1");
    TEST_ASSERT_FLOAT_WITHIN(0.6f, 2.0f * ImGui::GetFontSize() / 28.0f, bar->max.x - bar->min.x);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, w2_item("row_1")->max.y - w2_item("row_1")->min.y, bar->max.y - bar->min.y);
    TEST_ASSERT_TRUE(bar->min.x < w2_item("num_1")->min.x);
    /* popisek tažení */
    ImVec2 c1 = h4_center("name_1");
    float y1 = w2_item("name_1")->min.y + 1.0f;
    /* fokus navigace na taženém řádku a viditelný (klávesnice): ImGui by kolem něj kreslil rámeček */
    w2_click(&v, "name_4");
    h4_key(&v, ImGuiKey_UpArrow, ImGuiKey_None);
    h4_key(&v, ImGuiKey_DownArrow, ImGuiKey_None);
    TEST_ASSERT_NOT_NULL_MESSAGE(w2_item("navframe_4"), "the keyboard shows the navigation frame on row 4");
    h4_drag_to(&v, "name_4", ImVec2(c1.x, y1));
    const st_CMT_WINDOW_TEST_ITEM *lab = w2_item("drag_label");
    TEST_ASSERT_NOT_NULL(lab);
    TEST_ASSERT_EQUAL_STRING("5  SAVED GAME", lab->text.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lab->min.x > c1.x && lab->min.y > y1, "the label is below-right of the cursor");
    const char *adjacent[2] = {"name_0", "name_1"};
    for (const char *nm : adjacent) {
        const st_CMT_WINDOW_TEST_ITEM *r = w2_item(nm);
        bool overlap = r->max.x > lab->min.x && r->min.x < lab->max.x && r->max.y > lab->min.y && r->min.y < lab->max.y;
        TEST_ASSERT_FALSE_MESSAGE(overlap, nm);
    }
    for (int i = 0; i < 5; i++) {
        char nf[24];
        snprintf(nf, sizeof(nf), "navframe_%d", i);
        TEST_ASSERT_NULL_MESSAGE(w2_item(nf), "no navigation focus frame while dragging");
    }
    h4_release(&v);
    TEST_ASSERT_NULL(w2_item("drag_label"));
    w3_drain();
    imgui_cmt_window_test_reset();
}

/**
 * @brief Snímky sady h4 (MZ_CMT_SHOT_SET=h4): více vybraných, kontextové menu, modál mazání, tažení s čárou, řádek zpráv po importu.
 *
 * Výřez obsahuje okno i popupy (kontextové menu, modál). Jména
 * "h4-<jazyk>-<měřítko v %>-<stav>.png".
 *
 * @param dir Adresář snímků. @return Počet neuložených snímků a snímků s vadou geometrie.
 */
static int shots_h4_run(const char *dir)
{
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    int failed = 0;
    st_CMT_VIEW v;
    setUp();
    for (int s = 0; s < 5; s++) {
        static const char *names[5] = {"multiselect", "ctxmenu", "delete-modal", "drag", "import-message"};
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
        cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
        v.state = CMT_VIEW_STATE_STOP;
        v.motor_known = false;
        for (int i = 0; i < 4; i++) w2_frame(&v);
        switch (s) {
            case 0:
                imgui_cmt_window_test_set_props_open(1u);
                imgui_cmt_window_test_set_selected({1, 2, 4}, v.tape_serial);
                break;
            case 1:
                imgui_cmt_window_test_set_selected({2}, v.tape_serial);
                w2_frame(&v);
                w3_click_at(&v, "name_2", 1);
                break;
            case 2:
                imgui_cmt_window_test_set_selected({3, 4}, v.tape_serial);
                w2_frame(&v);
                w3_click_at(&v, "name_4", 1);
                w3_click_at(&v, "ctx_delete", 0);
                break;
            case 3:
                h4_drag_to(&v, "name_4", ImVec2(h4_center("name_1").x, w2_item("name_1")->min.y + 1.0f));
                break;
            default:
                imgui_cmt_window_test_set_selected({1}, v.tape_serial);
                cmt_view_set_message(&v, cmt_view_imported_message("prog2.mzf", 1).c_str(), false);
                break;
        }
        for (int i = 0; i < 2; i++) w2_frame(&v, true);
        /* výřez: okno a popupy */
        ImVec2 sz = w2_window_size();
        for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items()) {
            sz.x = ImMax(sz.x, it.max.x + 8.0f);
            sz.y = ImMax(sz.y, it.max.y + 8.0f);
        }
        ImGuiWindow *modal = ImGui::GetTopMostPopupModal();
        if (modal) {
            sz.x = ImMax(sz.x, modal->Pos.x + modal->Size.x + 1.0f);
            sz.y = ImMax(sz.y, modal->Pos.y + modal->Size.y + 1.0f);
        }
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("h4-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "", dash ? dash : "", names[s]);
        if (!shot_save_crop(dir, file, (int)(sz.x + 0.5f), (int)(sz.y + 0.5f))) failed++;
        int faults = w2_geometry_faults(file);
        fprintf(stderr, "geo: %s crop %.0f x %.0f px, font %.1f px\n", file, sz.x, sz.y, ImGui::GetFontSize());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
        if (s == 3) h4_release(&v);
        w3_close_popups(&v);
        w3_drain();
    }
    tearDown();
    imgui_cmt_window_test_set_stretch(false);
    return failed;
}

/**
 * @brief Zapíše MZF "PROG2" (tělo 64 B) jako prog2.mzf pro snímek importu.
 * @return Cesta (slot 3 tmp_named(), platí do tearDown()).
 */
static const char *shots_prog2_mzf(void)
{
    uint8_t raw[128 + 64];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x01;
    memset(&raw[1], 0x0D, 17);
    memcpy(&raw[1], "PROG2", 5);
    raw[0x12] = 64;
    raw[0x15] = 0x12;
    raw[0x17] = 0x12;
    for (int i = 0; i < 64; i++) raw[128 + i] = (uint8_t)(i * 5 + 2);
    const char *path = tmp_named(3, "prog2.mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, sizeof(raw), NULL));
    return path;
}

/**
 * @brief Snímky sady h6 (MZ_CMT_SHOT_SET=h6): stavy sady h4 s dnešním vzhledem a další stavy.
 *
 * Stavy: multiselect, ctxmenu, delete-modal, drag (testovací stav konceptu),
 * import-message (skutečný import prog2.mzf do pásky games.tmz zásahem
 * fronty: nový řádek vybraný, Status Unsaved), disabled-menu (kontextové
 * menu v PLAY se zakázanými položkami a tooltipem důvodu), delete-n (tři
 * uložené bloky vybrané, menu "Delete 3 blocks"), deleted-message
 * (skutečné smazání dvou bloků), tape-menu (menu Tape). Výřez obsahuje okno
 * i všechny otevřené popupy (kontextové menu, podnabídka, tooltip, modál)
 * a jejich hrany: rozměry popupů se zaokrouhlí nahoru a přidá se 1 px.
 * Jména "h6-<jazyk>-<měřítko v %>-<stav>.png".
 *
 * @param dir Adresář snímků. @return Počet neuložených snímků a snímků s vadou geometrie.
 */
static int shots_h6_run(const char *dir)
{
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    int failed = 0;
    static const char *names[9] = {"multiselect", "ctxmenu", "delete-modal", "drag", "import-message",
                                   "disabled-menu", "delete-n", "deleted-message", "tape-menu"};
    for (int s = 0; s < 9; s++) {
        st_CMT_VIEW v;
        setUp();
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(CMT_WINDOW_LAYOUT_FULL);
        const bool real = (s == 4 || s == 7);
        if (real) {
            /* skutečná páska a skutečný zásah fronty (výsledek cestou aplikace) */
            open_games_tape();
            cmt_view_reset(&v);
            fill(&v);
        } else {
            cmt_view_make_fixture(&v, CMT_VIEW_FIXTURE_PLAYING_FULL);
            v.state = CMT_VIEW_STATE_STOP;
            v.motor_known = false;
        }
        for (int i = 0; i < 4; i++) w2_frame(&v);
        switch (s) {
            case 0:
                imgui_cmt_window_test_set_props_open(1u);
                imgui_cmt_window_test_set_selected({1, 2, 4}, v.tape_serial);
                break;
            case 1:
                imgui_cmt_window_test_set_selected({2}, v.tape_serial);
                w2_frame(&v);
                w3_click_at(&v, "name_2", 1);
                break;
            case 2:
                imgui_cmt_window_test_set_selected({3, 4}, v.tape_serial);
                w2_frame(&v);
                w3_click_at(&v, "name_4", 1);
                w3_click_at(&v, "ctx_delete", 0);
                break;
            case 3:
                imgui_cmt_window_test_set_selected({2}, v.tape_serial);
                w2_frame(&v);
                h4_drag_to(&v, "name_4", ImVec2(h4_center("name_1").x, w2_item("name_1")->min.y + 1.0f));
                break;
            case 4: {
                const char *paths[1] = {shots_prog2_mzf()};
                imgui_cmt_tape_import_paths(paths, 1, 2, v.tape_serial);
                device_request_poll();
                imgui_device_request_results();
                fill(&v);
                break;
            }
            case 5:
                v.state = CMT_VIEW_STATE_PLAY;
                for (int i = 0; i < 2; i++) w2_frame(&v);
                w3_click_at(&v, "name_2", 1);
                /* myš nad zakázanou položkou Move up: tooltip s důvodem (pevný krok času, prodleva tooltipu) */
                s_w2_dt = 1.0f / 60.0f;
                ImGui::GetIO().AddMousePosEvent(h4_center("ctx_move_up").x, h4_center("ctx_move_up").y);
                for (int i = 0; i < 40; i++) w2_frame(&v);
                break;
            case 6:
                imgui_cmt_window_test_set_selected({0, 1, 2}, v.tape_serial);
                w2_frame(&v);
                w3_click_at(&v, "name_1", 1);
                break;
            case 7: {
                const int del[2] = {1, 2};
                (void)imgui_cmt_tape_remove_blocks(v.tape_serial, del, 2);
                device_request_poll();
                imgui_device_request_results();
                fill(&v);
                break;
            }
            default:
                w3_click_at(&v, "menu_tape", 0);
                break;
        }
        for (int i = 0; i < 2; i++) w2_frame(&v, true);
        /* výřez: okno, zaznamenané prvky a všechna okna popupů celá (hrany zaokrouhlené nahoru + 1 px) */
        ImVec2 sz = w2_window_size();
        for (const st_CMT_WINDOW_TEST_ITEM &it : imgui_cmt_window_test_items()) {
            if (it.max.x - it.min.x < 0.5f) continue;
            sz.x = ImMax(sz.x, it.max.x + 8.0f);
            sz.y = ImMax(sz.y, it.max.y + 8.0f);
        }
        ImGuiContext &g = *GImGui;
        for (ImGuiWindow *w : g.Windows) {
            if (!w->Active || !(w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) continue;
            sz.x = ImMax(sz.x, ceilf(w->Pos.x + w->Size.x) + 1.0f);
            sz.y = ImMax(sz.y, ceilf(w->Pos.y + w->Size.y) + 1.0f);
        }
        sz.x = ImMin(sz.x, (float)s_canvas_w);
        sz.y = ImMin(sz.y, (float)s_canvas_h);
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("h6-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "", dash ? dash : "", names[s]);
        if (!shot_save_crop(dir, file, (int)ceilf(sz.x), (int)ceilf(sz.y))) failed++;
        int faults = w2_geometry_faults(file);
        fprintf(stderr, "geo: %s crop %.0f x %.0f px, font %.1f px, message '%s'\n", file, ceilf(sz.x), ceilf(sz.y), ImGui::GetFontSize(),
                v.message.c_str());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
        s_w2_dt = 0.0f;
        if (s == 3) h4_release(&v);
        w3_close_popups(&v);
        w3_drain();
        imgui_cmt_tape_test_reset();
        tearDown();
    }
    imgui_cmt_window_test_set_stretch(false);
    return failed;
}

/**
 * @brief Snímky sady g4 (MZ_CMT_SHOT_SET=g4): Direct, čekání na motor, pauza, záznam.
 * @param dir Adresář snímků. @return Počet neuložených snímků a snímků s vadou geometrie.
 */
static int shots_g4_run(const char *dir)
{
    struct { en_CMT_VIEW_FIXTURE f; en_CMT_WINDOW_LAYOUT l; const char *name; } shots[] = {
        {CMT_VIEW_FIXTURE_DIRECT, CMT_WINDOW_LAYOUT_COMPACT, "compact-direct"},
        {CMT_VIEW_FIXTURE_WAITING_MOTOR, CMT_WINDOW_LAYOUT_COMPACT, "compact-waiting"},
        {CMT_VIEW_FIXTURE_PAUSED, CMT_WINDOW_LAYOUT_COMPACT, "compact-paused"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_COMPACT, "compact-recording"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_FULL, "full-recording"},
        {CMT_VIEW_FIXTURE_PLAYING_FULL, CMT_WINDOW_LAYOUT_FULL, "full"},
    };
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    int failed = 0;
    st_CMT_VIEW v;
    setUp();
    for (auto &s : shots) {
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(s.l);
        cmt_view_make_fixture(&v, s.f);
        imgui_cmt_window_test_set_selection(cmt_view_fixture_selection(s.f), v.tape_serial);
        for (int i = 0; i < 4; i++) w2_frame(&v, true);
        ImVec2 sz = w2_window_size();
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("g4-%.*s%s%s-%s.png", lang_len, s_shot_tag, stretch ? "+40" : "",
                                      dash ? dash : "", s.name);
        if (!shot_save_crop(dir, file, (int)(sz.x + 0.5f), (int)(sz.y + 0.5f))) failed++;
        int faults = w2_geometry_faults(file);
        fprintf(stderr, "geo: %s window %.0f x %.0f px, min %.0f x %.0f px\n", file, sz.x, sz.y,
                imgui_cmt_window_test_min_size().x, imgui_cmt_window_test_min_size().y);
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
    }
    tearDown();
    imgui_cmt_window_test_set_stretch(false);
    return failed;
}

/**
 * @brief Snímky sad g5 a g6 (MZ_CMT_SHOT_SET=g5 / g6): všechny testovací stavy v rozvržení Compact i Full.
 *
 * Okno ve výchozí velikosti rozvržení (po imgui_cmt_window_test_reset()),
 * výběr řádku podle cmt_view_fixture_selection(). Jména
 * "<sada>-<jazyk>-<měřítko v %>-<rozvržení>-<stav>.png" (při MZ_CMT_SHOT_STRETCH=1
 * jazyk s příponou "+40"). Sady se liší jen předponou jména (stejné
 * varianty). Ke každému snímku řádek "geo:" (velikost okna,
 * minimum, písmo) a výsledek automatické kontroly ořezu, překryvu
 * a vodorovného posuvníku tabulky ("check:") na stderr.
 *
 * @param dir Adresář snímků (musí existovat). @param set Předpona jmen ("g5", "g6").
 * @return Počet neuložených snímků a snímků s vadou geometrie.
 * @post Okno je ve výchozím stavu testu, prodloužení textů vypnuté.
 */
static int shots_all_run(const char *dir, const char *set)
{
    struct { en_CMT_VIEW_FIXTURE f; en_CMT_WINDOW_LAYOUT l; const char *name; } shots[] = {
        {CMT_VIEW_FIXTURE_PLAYING_COMPACT, CMT_WINDOW_LAYOUT_COMPACT, "compact-playing"},
        {CMT_VIEW_FIXTURE_NO_TAPE, CMT_WINDOW_LAYOUT_COMPACT, "compact-notape"},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, CMT_WINDOW_LAYOUT_COMPACT, "compact-empty"},
        {CMT_VIEW_FIXTURE_WAITING_MOTOR, CMT_WINDOW_LAYOUT_COMPACT, "compact-waiting"},
        {CMT_VIEW_FIXTURE_PAUSED, CMT_WINDOW_LAYOUT_COMPACT, "compact-paused"},
        {CMT_VIEW_FIXTURE_DIRECT, CMT_WINDOW_LAYOUT_COMPACT, "compact-direct"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_COMPACT, "compact-recording"},
        {CMT_VIEW_FIXTURE_LONG_TEXTS, CMT_WINDOW_LAYOUT_COMPACT, "compact-longtexts"},
        {CMT_VIEW_FIXTURE_PLAYING_FULL, CMT_WINDOW_LAYOUT_FULL, "full-playing"},
        {CMT_VIEW_FIXTURE_EMPTY_TAPE, CMT_WINDOW_LAYOUT_FULL, "full-empty"},
        {CMT_VIEW_FIXTURE_DIRECT, CMT_WINDOW_LAYOUT_FULL, "full-direct"},
        {CMT_VIEW_FIXTURE_RECORDING, CMT_WINDOW_LAYOUT_FULL, "full-recording"},
        {CMT_VIEW_FIXTURE_MANY_BLOCKS, CMT_WINDOW_LAYOUT_FULL, "full-manyblocks"},
        {CMT_VIEW_FIXTURE_LONG_TEXTS, CMT_WINDOW_LAYOUT_FULL, "full-longtexts"},
        {CMT_VIEW_FIXTURE_CONTROL_BLOCKS, CMT_WINDOW_LAYOUT_FULL, "full-controlblocks"},
    };
    const char *ev = getenv("MZ_CMT_SHOT_STRETCH");
    bool stretch = ev && atoi(ev) == 1;
    imgui_cmt_window_test_set_stretch(stretch);
    int failed = 0;
    st_CMT_VIEW v;
    setUp();
    for (auto &s : shots) {
        imgui_cmt_window_test_reset();
        imgui_cmt_window_set_layout(s.l);
        cmt_view_make_fixture(&v, s.f);
        imgui_cmt_window_test_set_selection(cmt_view_fixture_selection(s.f), v.tape_serial);
        for (int i = 0; i < 4; i++) w2_frame(&v, true);
        ImVec2 sz = w2_window_size();
        const char *dash = strchr(s_shot_tag, '-');
        int lang_len = dash ? (int)(dash - s_shot_tag) : (int)strlen(s_shot_tag);
        gchar *file = g_strdup_printf("%s-%.*s%s%s-%s.png", set, lang_len, s_shot_tag, stretch ? "+40" : "",
                                      dash ? dash : "", s.name);
        if (!shot_save_crop(dir, file, (int)(sz.x + 0.5f), (int)(sz.y + 0.5f))) failed++;
        int faults = w2_geometry_faults(file);
        if (s.l == CMT_WINDOW_LAYOUT_FULL && imgui_cmt_window_test_table_scroll_x()) {
            fprintf(stderr, "%s: table has a horizontal scrollbar\n", file);
            faults++;
        }
        fprintf(stderr, "geo: %s window %.0f x %.0f px, min %.0f x %.0f px, font %.1f px\n", file, sz.x, sz.y,
                imgui_cmt_window_test_min_size().x, imgui_cmt_window_test_min_size().y, ImGui::GetFontSize());
        fprintf(stderr, "check: %s %s (%d)\n", file, faults ? "FAIL" : "OK", faults);
        failed += faults ? 1 : 0;
        g_free(file);
    }
    tearDown();
    imgui_cmt_window_test_set_stretch(false);
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
        /* jazyk je součást jména souboru snímku: oddělovač cesty ani ".." nesmí zapsat mimo adresář */
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
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        float scale = (ev = getenv("MZ_CMT_SHOT_SCALE")) ? (float)g_ascii_strtod(ev, NULL) : 1.0f;
        if (scale <= 0.0f) scale = 1.0f;
        /* měřítko v procentech (bez desetinné čárky locale) */
        s_shot_scale = scale;
        snprintf(s_shot_tag, sizeof(s_shot_tag), "%s-%03d", (lang && *lang) ? lang : "en", (int)(scale * 100.0f + 0.5f));
        const char *font = getenv("MZ_CMT_SHOT_FONT");
        if (font && *font) {
            /* jako myimgui.cpp: DroidSans 28 px + ikony IGFD (tlačítka dnešního okna) */
            static const ImWchar i18n_ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
            io.Fonts->AddFontFromFileTTF(font, 28.0f, NULL, i18n_ranges);
            static const ImWchar icons_ranges[] = {ICON_MIN_IGFD, ICON_MAX_IGFD, 0};
            ImFontConfig icfg;
            icfg.MergeMode = true;
            icfg.PixelSnapH = true;
            io.Fonts->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_IGFD, 28.0f, &icfg, icons_ranges);
        }
        const char *mono = getenv("MZ_CMT_SHOT_MONO");
        if (mono && *mono) s_mono = io.Fonts->AddFontFromFileTTF(mono, 28.0f);
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
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
            fprintf(stderr, "FBO functions not available, reading the back buffer\n");
            s_bind_fb = NULL;
        }
        /* MZ_CMT_SHOT_SET=g2 / g3 / g4 / g5 / g6 (výchozí g4): sady snímků okna; h4 a h6: úpravy pásky */
        const char *set = getenv("MZ_CMT_SHOT_SET");
        int failed;
        if (set && strcmp(set, "g2") == 0) failed = shots_g2_run(shot_dir);
        else if (set && strcmp(set, "g3") == 0) failed = shots_g3_run(shot_dir);
        else if (set && (strcmp(set, "g5") == 0 || strcmp(set, "g6") == 0)) failed = shots_all_run(shot_dir, set);
        else if (set && strcmp(set, "h4") == 0) failed = shots_h4_run(shot_dir);
        else if (set && strcmp(set, "h6") == 0) failed = shots_h6_run(shot_dir);
        else failed = shots_g4_run(shot_dir);
        ui_teardown();
        mztest_teardown();
        return failed ? 1 : 0;
    }

    /* Plátno pro geometrii nového okna: Full při měřítku 1,75 má asi 1 800 x 1 400 px;
     * menší plátno by prvky ořízlo už výřezem obrazovky (ClipRect). */
    s_canvas_w = 2400;
    s_canvas_h = 1800;

    /* Běh v jiném jazyce (registrace ui-cmt-window-de): jen geometrie nového okna. */
    const char *tl = getenv("MZ_CMT_TEST_LANG");
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
        if (!s_ui_ok) {
            mztest_teardown();
            fprintf(stderr, "ui-cmt-window: SDL/GL not available, window drawing skipped\n");
            return 77;
        }
        ImGui::StyleColorsDark();
        UNITY_BEGIN();
        RUN_TEST(test_w2_deck_geometry);
        int lres = UNITY_END();
        ui_teardown();
        mztest_teardown();
        return lres;
    }

    /* kreslení oken potřebuje SDL + GL; bez nich proběhnou jen testy kopie a ctest test ukáže jako přeskočený */
    s_ui_ok = ui_setup();
    if (s_ui_ok) ImGui::StyleColorsDark();

    UNITY_BEGIN();
    RUN_TEST(test_view_no_tape);
    RUN_TEST(test_view_direct_supported_follows_rom);
    RUN_TEST(test_w2_echo_toggle);
    RUN_TEST(test_view_mzt_stop_play_pause);
    RUN_TEST(test_view_after_recording);
    RUN_TEST(test_view_unsaved_survives_tape_mark);
    RUN_TEST(test_final_source_of_imported_block);
    RUN_TEST(test_view_names);
    RUN_TEST(test_view_tape_tools_reads);
    RUN_TEST(test_view_eject_between_fills);
    RUN_TEST(test_view_rebuild_only_on_change);
    RUN_TEST(test_view_lengths_300_blocks);
    RUN_TEST(test_view_lengths_request_only_in_stop);
    RUN_TEST(test_view_lengths_retry_after_refusal);
    RUN_TEST(test_view_lengths_keep_machine_hash);
    RUN_TEST(test_lengths_cheap_tzx_blocks);
    RUN_TEST(test_lengths_cheap_recorded_raw_block);
    RUN_TEST(test_lengths_cheap_wav_block);
    RUN_TEST(test_view_fixtures);
    if (s_ui_ok) RUN_TEST(test_window_draw_smoke);
    if (s_ui_ok) RUN_TEST(test_w2_deck_geometry);
    if (s_ui_ok) RUN_TEST(test_w2_layout_persistence);
    if (s_ui_ok) RUN_TEST(test_w2_blocks_button_toggles_layout);
    if (s_ui_ok) RUN_TEST(test_w2_title_and_switch);
    if (s_ui_ok) RUN_TEST(test_w2_deck_texts_and_states);
    if (s_ui_ok) RUN_TEST(test_w3_table_and_properties);
    if (s_ui_ok) RUN_TEST(test_w3_empty_and_recording_rows);
    if (s_ui_ok) RUN_TEST(test_w3_selection_rules);
    if (s_ui_ok) RUN_TEST(test_w3_row_actions);
    if (s_ui_ok) RUN_TEST(test_w3_columns_menu_and_ini);
    if (s_ui_ok) RUN_TEST(test_w3_draw_time_300_blocks);
    if (s_ui_ok) RUN_TEST(test_g2f_deck_button_requests);
    if (s_ui_ok) RUN_TEST(test_g2f_control_block_ends);
    if (s_ui_ok) RUN_TEST(test_g2f_ini_round_trip);
    if (s_ui_ok) RUN_TEST(test_g2f_small_work_area);
    if (s_ui_ok) RUN_TEST(test_g4_shortcuts);
    if (s_ui_ok) RUN_TEST(test_g4_mode_switch);
    if (s_ui_ok) RUN_TEST(test_g4_direct_unsupported_rom);
    RUN_TEST(test_g4_mode_request_core);
    RUN_TEST(test_g4_message_line);
    if (s_ui_ok) RUN_TEST(test_g4f_direct_running_tape_stop);
    if (s_ui_ok) RUN_TEST(test_g4f_messages_hidden_window);
    RUN_TEST(test_g4f_mode_request_core_direct);
    if (s_ui_ok) RUN_TEST(test_g4f_full_layout_shortcuts);
    if (s_ui_ok) RUN_TEST(test_g4_tape_menu);
    if (s_ui_ok) RUN_TEST(test_g3f_stale_block_actions);
    if (s_ui_ok) RUN_TEST(test_g3f_set_speed_only_in_stop);
    if (s_ui_ok) RUN_TEST(test_g3f_row_actions_in_states);
    if (s_ui_ok) RUN_TEST(test_g3f_enter_needs_table_focus);
    if (s_ui_ok) RUN_TEST(test_g3f_full_min_height);
    if (s_ui_ok) RUN_TEST(test_g3f_column_widths);
    if (s_ui_ok) RUN_TEST(test_g2f2_record_user_pause_resume);
    if (s_ui_ok) RUN_TEST(test_ff_record_waiting_play_starts);
    RUN_TEST(test_ff_guarded_cmt_ops);
    RUN_TEST(test_ff_menu_boost_message);
    if (s_ui_ok) RUN_TEST(test_ff_window_ops_carry_guards);
    if (s_ui_ok) RUN_TEST(test_ff_load_directly_tape_changed);
    if (s_ui_ok) RUN_TEST(test_ff_record_play_guarded);
    RUN_TEST(test_h0_dropped_op_warning_once);
    if (s_ui_ok) RUN_TEST(test_ff_keyboard_not_routed_while_cmt_focused);
    if (s_ui_ok) RUN_TEST(test_g6_full_whole_rows);
    if (s_ui_ok) RUN_TEST(test_g6_texts);
    if (s_ui_ok) RUN_TEST(test_g6_message_in_closing_frame);
    if (s_ui_ok) RUN_TEST(test_g6_main_menu_routing);
    if (s_ui_ok) RUN_TEST(test_g6_main_menu_hack_unsupported_rom);
    if (s_ui_ok) RUN_TEST(test_g6_main_menu_bootdev);
    if (s_ui_ok) RUN_TEST(test_g7_main_menu_recent);
    RUN_TEST(test_h4_unsaved_by_flag);
    if (s_ui_ok) RUN_TEST(test_h4_multi_select);
    if (s_ui_ok) RUN_TEST(test_h4_delete_saved);
    if (s_ui_ok) RUN_TEST(test_h4_delete_unsaved_modal);
    if (s_ui_ok) RUN_TEST(test_h4_move_up_down);
    if (s_ui_ok) RUN_TEST(test_h4_drag_row);
    if (s_ui_ok) RUN_TEST(test_h4_edits_disabled);
    if (s_ui_ok) RUN_TEST(test_h4_import_export_menu);
    if (s_ui_ok) RUN_TEST(test_h4_os_drop);
    if (s_ui_ok) RUN_TEST(test_h4_selection_restore);
    RUN_TEST(test_h4_edit_while_dialog_open);
    if (s_ui_ok) RUN_TEST(test_h4_geometry);
    if (s_ui_ok) RUN_TEST(test_wave_drag_carries_tape_serial);
    if (s_ui_ok) RUN_TEST(test_wave_os_drop_over_header);
    if (s_ui_ok) RUN_TEST(test_wave_os_drop_never_silent);
    if (s_ui_ok) RUN_TEST(test_final_os_drop_over_popup_warns);
    if (s_ui_ok) RUN_TEST(test_wave_drag_autoscroll);
    if (s_ui_ok) RUN_TEST(test_wave_alt_move_queued);
    if (s_ui_ok) RUN_TEST(test_wave_modals_centred);
    if (s_ui_ok) RUN_TEST(test_wave_selection_props_drag_label);
    int result = UNITY_END();

    if (s_ui_ok) ui_teardown();
    mztest_teardown();
    if (!s_ui_ok && result == 0) {
        /* testy kopie prošly, kreslení oken ověřit nešlo: ctest SKIP (SKIP_RETURN_CODE 77), ne PASS */
        fprintf(stderr, "ui-cmt-window: SDL/GL not available, window drawing skipped\n");
        return 77;
    }
    return result;
}
