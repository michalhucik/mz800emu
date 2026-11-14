/*
 * test_cmt_dialogs.cpp - dotazy Virtual CMT a okno zpráv (src/ui-imgui/cmt/cmt_tape_actions.cpp,
 * src/ui-imgui/message/message_window.cpp) se skutečným ImGui ve skrytém okně SDL + GL.
 *
 * Každý snímek vykreslí dialogy automatu (imgui_cmt_tape_dialogs() pod zámkem
 * stavu zařízení) a pak okno zpráv (imgui_message_window()) ve stejném
 * pořadí jako main_window.cpp. Volby tlačítek vnucuje testovací háček
 * imgui_cmt_tape_test_choose() (klikání myší do skrytého okna by záviselo
 * na rozložení); otevírání a zavírání popupů je skutečné ImGui.
 *
 * - zpráva zařazená ve stejném snímku jako dialog neuložených změn dialog
 *   nezavře, počká ve frontě; po Discard changes proběhne akce (ukončení)
 *   a pak se ukáže zpráva;
 * - nahrávání: Stop recording, STOP (emu vlákno simuluje test) zařadí
 *   výsledek nahrávání ve stejném snímku, kdy se otevírá dialog neuložených
 *   změn - dialog zůstane otevřený, Discard dokončí akci;
 * - dialog zavřený jiným modálem se po jeho zavření otevře znovu (akce
 *   nevisí); druhý požadavek během dotazu rozpracovaný dotaz nezmění;
 * - návrat souborového dialogu s horším stavem pásky se zeptá znovu;
 * - stejné zprávy po sobě se sloučí do jedné s počtem;
 * - přehrávání záznamu vstupů: žádný dotaz (ukončení, vysunutí, dialog
 *   otevřený před startem přehrávání se zavře a akce proběhne); nahrávání
 *   záznamu vstupů: dotaz bez Save tape;
 * - akce zvolená v dialogu (i zahájená pod zámkem) proběhne až po uvolnění
 *   zámku stavu zařízení (stuby hlídají, že ji nikdo nevolá pod zámkem);
 * - větev Save tape: uložení do souboru pásky a pokračování akce, selhání
 *   uložení (akce zrušena, páska Modified), Save As pro formát, který pásku
 *   neunese, zrušený Save As; Save As po výměně pásky nic neuloží a čekající
 *   akci zruší (UI i emu vlákno);
 * - souhlas s nahráváním neplatí pro nahrávání spuštěné po něm;
 * - ukončení zadané při otevřeném cizím popupu se neztratí;
 * - imgui_modal_open_when_free(): dialog zavřený cizím modálem se otevře znovu;
 * - fronta zpráv: mez 8, chyba před informacemi, vytlačení;
 * - postupný import více souborů (další až po výsledku předchozího
 *   s čítačem z výsledku, do prázdného decku s čítačem 0, pořadí souborů,
 *   nečitelný soubor, jiná páska zbytek zruší), zprávy výsledků úprav
 *   (smazání, chyba přesunu, jiná páska, odmítnutí při záznamu vstupů,
 *   příliš mnoho rozsahů, export celé pásky), výsledek úpravy čtený ve
 *   stejném držení zámku jako vyzvednutí z fronty, otevření přetaženého
 *   souboru obvyklým postupem (dotaz na neuložené změny).
 * Bez SDL/GL test skončí kódem 77 (ctest SKIP).
 *
 * Licence: GPLv3
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdio.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

extern "C" {
#include "mztest.h"
#include "device_request.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/mzf/mzf.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "snapshot/snapshot_request.h"
#include "replay.h"
}
#include <string>

#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "ui-imgui/message/message_window.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/devices/device_request_ui.h"

/* UI globál, který jinak vytváří sdlapp_imgui_video.cpp. */
static MyImGui s_gui;
MyImGui *g_gui = &s_gui;

/* ================================================================
 * Stuby UI funkcí, které automat volá v jiných modulech (test je nelinkuje)
 * ================================================================ */

static int s_snapshot_load_now = 0;
static int s_snapshot_cancelled = 0;
static int s_snapshot_submit = 0;
/** @brief Počet volání akcí, které v aplikaci čekají na emu vlákno, pod zámkem stavu zařízení (musí být 0). */
static int s_called_under_lock = 0;
/** @brief Stav záznamu vstupů pro automat (stuby imgui_cmt_replay_*()). */
static bool s_replay_recording = false;
static bool s_replay_playing = false;

/** @brief Zaznamená volání akce pod zámkem stavu zařízení (v aplikaci by čekala na emu vlákno). */
static void note_lock(void)
{
    if (device_state_lock_held()) s_called_under_lock++;
}

extern "C" bool imgui_cmt_replay_recording(void) { return s_replay_recording; }
extern "C" bool imgui_cmt_replay_playing(void) { return s_replay_playing; }
/** @brief Okno Virtual CMT se v tomto testu nekreslí: modály dostanou výchozí polohu ImGui (jako se zavřeným oknem). */
extern "C" void imgui_cmt_window_modal_next_pos(void) {}
extern "C" void snapshot_ui_request_load_now(void) { note_lock(); s_snapshot_load_now++; }
extern "C" void snapshot_ui_request_quickload_now(void) { note_lock(); }
extern "C" void snapshot_ui_load_cancelled(void) { note_lock(); s_snapshot_cancelled++; }
extern "C" void imgui_replay_open_play_dialog_now(void) { note_lock(); }
extern "C" void imgui_replay_start_play(const char *path) { note_lock(); (void)path; }
/** @brief Počet zastavení přehrávání záznamu vstupů z automatu (imgui_replay_stop_play()). */
static int s_replay_stop = 0;
extern "C" void imgui_replay_stop_play(void) { note_lock(); s_replay_stop++; }
extern "C" void snapshot_ui_submit(en_SNAPSHOT_REQ_KIND kind, const char *path, const char *description)
{
    note_lock();
    s_snapshot_submit++;
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
/* Dialog analýzy (cmt_analyze_dialog.cpp) test nelinkuje: okno Virtual CMT se tu nekreslí (pokrývá ho ui-cmt-analyze). */
#include "ui-imgui/cmt/cmt_analyze_dialog.h"
void cmt_analyze_dialog_frame(void) {}
bool cmt_analyze_dialog_request_result(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit)
{
    (void)res;
    (void)edit;
    return false;
}
/* Modály informací o pásce a převodu profilu test nelinkuje (pokrývá je ui-cmt-tools). */
#include "ui-imgui/cmt/cmt_tapeinfo_dialog.h"
#include "ui-imgui/cmt/cmt_convert_dialog.h"
void cmt_tapeinfo_dialog_frame(void) {}
bool cmt_tapeinfo_dialog_request_result(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit)
{
    (void)res;
    (void)edit;
    return false;
}
void cmt_convert_dialog_frame(void) {}
bool cmt_convert_dialog_request_result(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit)
{
    (void)res;
    (void)edit;
    return false;
}

/* ================================================================
 * SDL3 + GL + ImGui (vzor test_bus_window.cpp)
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
    s_win = SDL_CreateWindow("mz800emu-test", 1024, 768, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
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

/** @brief Volitelný cizí modál, který test vykresluje (ID "###other_modal"). */
static bool s_other_request = false;
static bool s_other_close = false;

/** @brief Jeden snímek: dialogy Virtual CMT, cizí modál (je-li), okno zpráv - pořadí jako main_window.cpp. */
static void frame(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    device_state_lock();
    imgui_cmt_tape_dialogs();
    device_state_unlock();
    imgui_cmt_tape_run_deferred();
    if (s_other_request) {
        /* modál, který OpenPopup() volá bez ohledu na ostatní (zavře jiný na téže úrovni) */
        ImGui::OpenPopup("###other_modal");
        s_other_request = false;
    }
    if (ImGui::BeginPopupModal("Other###other_modal", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("other");
        if (s_other_close) {
            ImGui::CloseCurrentPopup();
            s_other_close = false;
        }
        ImGui::EndPopup();
    }
    imgui_message_window();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/** @brief Vloží novou pásku a označí ji Modified. */
static void modified_tape(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    cmttmz_set_modified(1);
    TEST_ASSERT_TRUE(cmt_has_unsaved_changes());
}

void setUp(void)
{
    imgui_cmt_tape_test_reset();
    imgui_message_test_clear();
    s_other_request = s_other_close = false;
    s_snapshot_load_now = s_snapshot_cancelled = s_snapshot_submit = 0;
    s_replay_stop = 0;
    s_called_under_lock = 0;
    s_replay_recording = s_replay_playing = false;
    device_request_clear();
    cmt_eject();
    if (s_ui_ok) frame(); /* první čtení čítače nahrávání (nic neohlásí) */
}

void tearDown(void)
{
    s_replay_recording = s_replay_playing = false;
    cmt_stop();
    cmt_eject();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_called_under_lock, "an action that waits for the emulator thread ran under the device lock");
}

/*
 * Dialog neuložených změn a zpráva zařazená ve stejném snímku: dialog
 * zůstane otevřený přes několik snímků, zpráva čeká; Discard changes provede
 * akci (ukončení), pak se zpráva ukáže.
 */
void test_unsaved_dialog_survives_queued_message(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    imgui_message_create(MESSAGE_TYPE_INFO, "message queued in the same frame");
    for (int i = 0; i < 4; i++) {
        frame();
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "unsaved-changes dialog must stay open");
        TEST_ASSERT_FALSE_MESSAGE(imgui_message_test_popup_open(), "the message must wait in its queue");
    }
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUIT, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_message_test_popup_open(), "the queued message is shown after the dialog");
    TEST_ASSERT_EQUAL_UINT(1, imgui_message_test_queued());
}

/*
 * Zavření okna při nahrávání -> Stop recording -> STOP
 * -> ve stejném snímku se otevírá dialog neuložených změn. Dialog zůstane,
 * Discard dokončí ukončení. Druhý požadavek na ukončení během dotazu
 * rozpracovaný dotaz nezmění. Výsledek nahrávání jde jen do
 * řádku zpráv okna Virtual CMT (kopie stavu ho čte z jádra), okno zpráv
 * ho neukáže (jedno místo pro jednu událost).
 */
void test_stop_recording_then_unsaved_dialog_with_record_message(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_STOP_REC);
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown()); /* "Stopping the recording..." */
    /* druhé zavření okna během čekání na STOP: ignorovat */
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());

    /* emu vlákno provede STOP (replay_host_cmt() test nevyřizuje): výsledek
     * nahrávání se zařadí; páska má změny */
    device_state_lock();
    cmt_stop();
    cmttmz_set_modified(1);
    device_state_unlock();
    for (int i = 0; i < 4; i++) {
        frame();
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "unsaved-changes dialog must open and stay");
        TEST_ASSERT_FALSE(imgui_message_test_popup_open());
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, imgui_message_test_queued(), "the recording result goes to the window's message line only");
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUIT, imgui_cmt_tape_test_take_last_run());
    frame();
    TEST_ASSERT_FALSE(imgui_message_test_popup_open());
}

/*
 * Dialog zavřený jiným modálem (OpenPopup() na téže úrovni) se po
 * zavření toho modálu otevře znovu; akce nevisí, Cancel ji zruší.
 */
void test_dialog_closed_by_other_modal_reopens(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    s_other_request = true;
    frame(); /* cizí OpenPopup() náš dialog zavře */
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, imgui_cmt_tape_test_dialog_shown(), "waits while the other modal is open");
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    s_other_close = true;
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "reopened after the other modal closed");
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
}

/*
 * Souborový dialog spuštěný bez neuložených změn; páska mezitím
 * dostala změny -> návrat dialogu se zeptá znovu. Bez zhoršení akce proběhne
 * hned. Zrušené načtení snapshotu vrátí běh (snapshot_ui_load_cancelled()).
 */
void test_file_action_rechecks_state(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_load_now);

    /* stav se nezhoršil: akce hned */
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "x.mzs");
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, imgui_cmt_tape_test_take_last_run());

    /* znovu, ale páska mezitím dostala změny: dotaz, Cancel = zrušené načtení */
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
    (void)imgui_cmt_tape_test_take_last_run();
    cmttmz_set_modified(1);
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "x.mzs");
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_cancelled);
}

/* Stejné zprávy po sobě se sloučí do jedné s počtem; jiná zpráva je nová položka. */
void test_message_dedup(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    imgui_message_create(MESSAGE_TYPE_ERROR, "same error");
    imgui_message_create(MESSAGE_TYPE_ERROR, "same error");
    imgui_message_create(MESSAGE_TYPE_ERROR, "same error");
    TEST_ASSERT_EQUAL_UINT(1, imgui_message_test_queued());
    TEST_ASSERT_EQUAL_UINT(3, imgui_message_test_front_count());
    imgui_message_create(MESSAGE_TYPE_INFO, "same error");
    TEST_ASSERT_EQUAL_UINT(2, imgui_message_test_queued());
    frame();
    TEST_ASSERT_TRUE(imgui_message_test_popup_open());
}

/* ================================================================
 * Uložení a nahrávání: další případy
 * ================================================================ */

/** @brief Cesta dočasného souboru testu (g_free). @param name Jméno. */
static char *ff_tmp(const char *name)
{
    gchar *n = g_strdup_printf("mztest_cmt_dialogs_%u_%s", (unsigned)g_random_int(), name);
    char *p = g_build_filename(g_get_tmp_dir(), n, NULL);
    g_free(n);
    return p;
}

/** @brief Vyřídí zásahy fronty zařízení (za emu vlákno) a předá jejich výsledky automatu. */
static void ff_device_round(void)
{
    device_request_poll();
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res)) (void)imgui_cmt_device_request_result(&res);
}

/*
 * Při přehrávání záznamu vstupů se na pásku přehrávání dotaz neklade -
 * vysunutí proběhne bez dialogu; dialog otevřený před startem přehrávání
 * se zavře a akce proběhne (dřív visel na "Stopping..."). Ukončení viz
 * test_quit_during_playback_stops_it_first().
 */
void test_replay_playback_never_asks(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    s_replay_playing = true;
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
    frame();
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_dialog_shown());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_EJECT, imgui_cmt_tape_test_take_last_run());

    /* dialog nahrávání otevřený před startem přehrávání */
    s_replay_playing = false;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_STOP_REC);
    frame(); /* "Stopping the recording..." - STOP by přehrávání zahodilo */
    s_replay_playing = true;
    frame();
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_dialog_shown());
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NEW_TAPE, imgui_cmt_tape_test_take_last_run());
}

/*
 * Ukončení během přehrávání záznamu vstupů
 * okno nezavře (veto), přehrávání zastaví běžnou cestou UI a čeká na jeho
 * konec; páska uživatele (uschovaná, Modified) je pak zpět a automat se
 * zeptá jako vždy. Bez neuložených změn aplikaci po konci přehrávání ukončí.
 */
void test_quit_during_playback_stops_it_first(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    s_replay_playing = true;
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_quit_requested(), "the window stays open while the playback runs");
    TEST_ASSERT_EQUAL_INT(1, s_replay_stop);
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested()); /* druhé zavření: nic navíc */
    TEST_ASSERT_EQUAL_INT(1, s_replay_stop);
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_dialog_shown());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());

    /* přehrávání skončilo, páska uživatele je zpět a má neuložené změny */
    modified_tape();
    s_replay_playing = false;
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "the question about the user's tape");
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUIT, imgui_cmt_tape_test_take_last_run());

    /* páska uživatele bez změn: po konci přehrávání se aplikace ukončí */
    cmt_eject();
    s_replay_playing = true;
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    s_replay_playing = false;
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUIT, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_EQUAL_INT(2, s_replay_stop);
}

/*
 * Při nahrávání záznamu vstupů se dotaz klade, ale Save tape se
 * nenabízí (uložení se odmítá): volba Save nic neudělá, Discard changes
 * akci provede.
 */
void test_replay_recording_offers_no_save(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    s_replay_recording = true;
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "Save tape is not offered, the dialog stays");
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_save_as_open());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NEW_TAPE, imgui_cmt_tape_test_take_last_run());
}

/*
 * Akce zvolená v dialogu (Discard) a akce zahájená pod zámkem stavu
 * zařízení (okno Virtual CMT, menu Devices) proběhne až po jeho uvolnění
 * (imgui_cmt_tape_run_deferred()), ve stejném snímku; také Cancel načtení
 * snapshotu po výběru souboru (snapshot_ui_load_cancelled()). Stuby
 * hlídají, že je nikdo nevolá pod zámkem (tearDown()).
 */
void test_dialog_action_runs_after_lock_release(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_load_now);
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());

    /* soubor vybrán, páska se mezitím zhoršila (nahrávání): dotaz, Cancel */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "x.mzs");
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_cancelled);
    cmt_stop();
    cmttmz_set_modified(0);
    frame();
    imgui_message_test_clear();
    (void)imgui_cmt_tape_test_take_last_run();

    /* akce zahájená pod zámkem bez dotazu: odložená, ne provedená (asertace
     * až po uvolnění zámku: selhání pod zámkem by zablokovalo další testy) */
    device_state_lock();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_QUICK_LOAD);
    bool deferred_active = imgui_cmt_tape_flow_active();
    en_IMGUI_CMT_TAPE_ACTION ran_under_lock = imgui_cmt_tape_test_take_last_run();
    device_state_unlock();
    TEST_ASSERT_TRUE(deferred_active);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, ran_under_lock);
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUICK_LOAD, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());

    /* odložená akce: druhý požadavek do jejího provedení se ignoruje */
    device_state_lock();
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "a.mzs");
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
    device_state_unlock();
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_submit);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, imgui_cmt_tape_test_take_last_run());
}

/*
 * Odložená akce (rozhodnutá pod zámkem) se před provedením
 * ověří znovu - páska mezitím dostala neuložené změny nebo začalo
 * nahrávání, tedy dotaz místo provedení.
 */
void test_deferred_action_rechecks_state(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    device_state_lock();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT); /* nic k dotazu: odloženo */
    cmttmz_set_modified(1);                             /* zhoršení před provedením */
    device_state_unlock();
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());

    /* nahrávání začne mezi rozhodnutím a provedením: dotaz na nahrávání */
    cmttmz_set_modified(0);
    device_state_lock();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    int rc = cmt_record();
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    imgui_cmt_tape_run_deferred();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NEW_TAPE, imgui_cmt_tape_test_take_last_run());
}

/*
 * Save tape u pásky, jejíž soubor ji unese (.tmz): zásah uložení,
 * po úspěchu čekající akce pokračuje, páska už není Modified.
 */
void test_save_tape_success_continues_action(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    char *path = ff_tmp("save_ok.tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(path));
    cmttmz_set_modified(1);
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, imgui_cmt_tape_test_save_as_open(), "saved to the tape file, no Save As");
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    ff_device_round();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NEW_TAPE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    g_remove(path);
    g_free(path);
}

/*
 * Selhání uložení (adresář neexistuje) akci zruší, páska zůstane
 * Modified.
 */
void test_save_tape_failure_aborts_action(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    char *dir = ff_tmp("no_such_dir");
    char *path = g_build_filename(dir, "tape.tmz", NULL);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    cmt_tape_path_changed(path);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_set_filepath(g_cmt.ext->container, path));
    cmttmz_set_modified(1);
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_save_as_open());
    ff_device_round();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    g_free(path);
    g_free(dir);
}

/*
 * Selhání uložení z dialogu neuložených změn při skrytém
 * okně Virtual CMT jde do okna zpráv (chyba); při viditelném okně do řádku
 * zpráv. Při ukončení aplikace je chyba modální vždy.
 */
void test_save_failure_hidden_window_and_exit_is_modal(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    char *dir = ff_tmp("no_such_dir2");
    char *path = g_build_filename(dir, "tape.tmz", NULL);
    for (int round = 0; round < 3; round++) {
        /* 0 = Eject, okno skryté; 1 = Eject, okno vidět; 2 = ukončení, okno vidět */
        imgui_message_test_clear();
        imgui_cmt_tape_test_reset();
        cmt_view_test_force_window_shown(round == 0 ? 0 : 1);
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
        cmt_tape_path_changed(path);
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_set_filepath(g_cmt.ext->container, path));
        cmttmz_set_modified(1);
        if (round == 2)
            TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
        else
            imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
        frame();
        imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
        frame();
        ff_device_round();
        TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
        TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
        if (round == 1) {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, imgui_message_test_queued(), "visible window: the error goes to its message line");
        } else {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1, imgui_message_test_queued(), round == 0 ? "hidden window: modal error" : "exit: always modal");
            ui_message_type_t type;
            char text[256];
            TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
            TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
            TEST_ASSERT_TRUE(g_str_has_prefix(text, "The tape could not be saved."));
        }
        cmttmz_set_modified(0);
        cmt_eject();
    }
    cmt_view_test_force_window_shown(-1);
    imgui_message_test_clear();
    g_free(path);
    g_free(dir);
}

/*
 * Selhání uložení z dialogu neuložených změn při ukončení
 * aplikace je modální i v okrajových cestách při viditelném okně: (a) cesta
 * ze Save As delší než mez fronty zásahů (zásah se nezařadí), (b) zásah
 * odmítnutý, protože mezitím začal záznam vstupů (MCP replay_record).
 */
void test_g6_quit_save_errors_always_modal(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);

    /* (a) nová páska bez souboru: Save As, cesta delší než DEVICE_REQUEST_STR_MAX */
    modified_tape();
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_save_as_open());
    std::string long_path(DEVICE_REQUEST_STR_MAX + 16, 'a');
    long_path += ".tmz";
    imgui_cmt_tape_test_save_as_finish(long_path.c_str());
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, imgui_message_test_queued(), "exit: a too long path is reported modally");
    ui_message_type_t type;
    char text[256];
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
    TEST_ASSERT_EQUAL_STRING("The tape could not be saved: the file path is too long.", text);
    cmttmz_set_modified(0);
    cmt_eject();

    /* (b) páska se souborem: zásah uložení odmítnutý záznamem vstupů */
    imgui_message_test_clear();
    imgui_cmt_tape_test_reset();
    char *path = ff_tmp("quit_replay.tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    cmt_tape_path_changed(path);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_set_filepath(g_cmt.ext->container, path));
    cmttmz_set_modified(1);
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    g_replay_mode = REPLAY_MODE_RECORDING;
    ff_device_round();
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, imgui_message_test_queued(), "exit: a refused save is reported modally");
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
    TEST_ASSERT_EQUAL_STRING("The tape cannot be saved while an input replay is recorded or played.", text);
    cmttmz_set_modified(0);
    cmt_eject();
    g_remove(path);
    g_free(path);
    cmt_view_test_force_window_shown(-1);
    imgui_message_test_clear();
}

/** @brief Kopie stavu okna pro čtení řádku zpráv (první plnění převezme jen čítač zpráv). */
static st_CMT_VIEW s_ff_view;

/** @brief Začne sledovat řádek zpráv: kopie znovu od začátku a první plnění. */
static void ff_message_begin(void)
{
    cmt_view_reset(&s_ff_view);
    device_state_lock();
    cmt_view_fill(&s_ff_view);
    device_state_unlock();
}

/**
 * @brief Poslední zpráva v řádku zpráv okna Virtual CMT od ff_message_begin().
 * @param[out] is_error Chyba (smí být NULL). @return Text zprávy ("" = žádná).
 */
static std::string ff_message_line(bool *is_error)
{
    device_state_lock();
    cmt_view_fill(&s_ff_view);
    device_state_unlock();
    if (is_error) *is_error = s_ff_view.message_is_error;
    return s_ff_view.message;
}

/*
 * Save / Save as uloží jen pásku, kterou uživatel
 * viděl (čítač výměn pásky). (a) Ukončení aplikace -> Save tape -> Save As;
 * během výběru souboru jiné vlákno (MCP emu_cmt_open) vloží jinou pásku:
 * nic se neuloží, ukončení se zruší (nepokračuje jako po úspěchu), zpráva
 * "The tape changed; ..." modálně (okno se zavírá). (b) Eject -> Save tape
 * -> Save As, páska se vymění až po zařazení zásahu: emu vlákno neuloží
 * (CMT_REQUEST_RC_TAPE_CHANGED), vysunutí se zruší, zpráva v řádku zpráv.
 * (c) Menu Save as... bez čekající akce: tatáž páska se uloží, jiná ne.
 */
void test_ff_save_as_tape_changed(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    const char *changed = "The tape changed; the action was cancelled.";
    cmt_view_test_force_window_shown(1);
    ui_message_type_t type;
    char text[256];
    char *a_path = ff_tmp("tape_a.tmz");
    ff_message_begin();

    /* (a) ukončení: páska vyměněná během dialogu Save As */
    modified_tape();
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_save_as_open());
    modified_tape(); /* jiná páska (MCP) */
    imgui_cmt_tape_test_save_as_finish(a_path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, device_request_pending(), "no save request for another tape");
    frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_tape_flow_active(), "the pending quit is cancelled");
    TEST_ASSERT_EQUAL_INT_MESSAGE(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run(), "the emulator is not closed");
    TEST_ASSERT_FALSE(g_file_test(a_path, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, imgui_message_test_queued(), "exit: reported modally");
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
    TEST_ASSERT_EQUAL_STRING(changed, text);
    cmttmz_set_modified(0);
    cmt_eject();
    imgui_message_test_clear();
    imgui_cmt_tape_test_reset();

    /* (b) vysunutí: páska vyměněná mezi zařazením a provedením zásahu (emu vlákno) */
    modified_tape();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_EJECT);
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_save_as_open());
    imgui_cmt_tape_test_save_as_finish(a_path);
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    modified_tape();
    ff_device_round();
    frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_tape_flow_active(), "the pending eject is cancelled");
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_FILLED, "the other tape stays in the deck");
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(a_path, G_FILE_TEST_EXISTS), "the emulator thread does not save another tape");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    bool is_error = false;
    TEST_ASSERT_EQUAL_STRING(changed, ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_EQUAL_UINT(0, imgui_message_test_queued());
    cmttmz_set_modified(0);
    cmt_eject();
    imgui_cmt_tape_test_reset();

    /* (c) menu Save as...: tatáž páska se uloží, po výměně ne */
    modified_tape();
    imgui_cmt_tape_save(true);
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_save_as_open());
    imgui_cmt_tape_test_save_as_finish(a_path);
    ff_device_round();
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(a_path, G_FILE_TEST_EXISTS), "the same tape is saved");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    g_remove(a_path);
    modified_tape();
    imgui_cmt_tape_save(true);
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_save_as_open());
    modified_tape();
    imgui_cmt_tape_test_save_as_finish(a_path);
    ff_device_round();
    TEST_ASSERT_FALSE(g_file_test(a_path, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_STRING(changed, ff_message_line(NULL).c_str());
    cmttmz_set_modified(0);
    cmt_eject();
    g_remove(a_path);
    g_free(a_path);
    cmt_view_test_force_window_shown(-1);
    imgui_message_test_clear();
}

/*
 * Páska, kterou formát jejího souboru neunese (nová páska bez
 * souboru), jde přes Save As: zrušený dialog akci zruší; vybraná cesta
 * pásku uloží a akce pokračuje.
 */
void test_save_tape_save_as_fallback_and_cancel(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_save_as_open());
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    imgui_cmt_tape_test_save_as_finish(NULL);
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    char *base = ff_tmp("saved_as");
    char *want = g_strconcat(base, ".tmz", NULL);
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_NEW_TAPE);
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_SAVE);
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_save_as_open());
    imgui_cmt_tape_test_save_as_finish(base); /* bez přípony: doplní .tmz */
    ff_device_round();
    TEST_ASSERT_TRUE(g_file_test(want, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NEW_TAPE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    g_remove(want);
    g_free(want);
    g_free(base);
}

/*
 * Souhlas Discard recording platí jen pro nahrávání, které tehdy
 * běželo. Skončí-li a začne-li jiné (např. přes MCP během výběru souboru),
 * návrat souborového dialogu se zeptá znovu.
 */
void test_recording_consent_not_reused(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
    frame();
    TEST_ASSERT_EQUAL_INT(1, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_load_now); /* dialog souboru "otevřen" */

    /* během výběru souboru nahrávání skončí a začne nové */
    cmt_stop();
    cmttmz_set_modified(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "y.mzs");
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, imgui_cmt_tape_test_dialog_shown(), "a new recording asks again");
    imgui_message_test_clear(); /* výsledek prvního nahrávání ve frontě: dialog by čekal na jeho okno */
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_EQUAL_INT(0, s_snapshot_submit);

    /* totéž nahrávání: souhlas platí, akce proběhne bez dotazu */
    imgui_message_test_clear();
    imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    imgui_cmt_tape_file_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD_FILE, "z.mzs");
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_submit);
}

/*
 * Reálná cesta zavření okna: požadavek na ukončení při otevřeném
 * cizím popupu (menu, dialog souboru) dotaz nezobrazí, ale neztratí se -
 * dotaz se ukáže, jakmile popup zmizí; Cancel ukončení zruší (veto), další
 * zavření a Discard aplikaci ukončí.
 */
void test_quit_request_waits_for_foreign_popup(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    s_other_request = true;
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_quit_requested()); /* veto: okno se nezavře */
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT(0, imgui_cmt_tape_test_dialog_shown());
    TEST_ASSERT_TRUE(imgui_cmt_tape_flow_active());
    s_other_close = true;
    frame();
    frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, imgui_cmt_tape_test_dialog_shown(), "the question appears once the popup closed");
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());

    TEST_ASSERT_FALSE(imgui_cmt_quit_requested());
    frame();
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_DISCARD);
    frame();
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_QUIT, imgui_cmt_tape_test_take_last_run());
}

/*
 * imgui_modal_open_when_free() s dialogem převedeným na pravidlo modálů
 * (vzor varování FDC / QD / MZF Size Warning): cizí OpenPopup() ho zavře,
 * dialog čeká a po zavření cizího modálu se otevře znovu.
 */
void test_modal_open_when_free_reopens(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    bool want = true;
    int shown = 0;
    auto converted_frame = [&]() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        shown = 0;
        if (want) imgui_modal_open_when_free("Converted###converted_modal");
        if (ImGui::BeginPopupModal("Converted###converted_modal", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            shown = 1;
            ImGui::TextUnformatted("converted");
            if (!want) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (s_other_request) {
            ImGui::OpenPopup("###other_modal");
            s_other_request = false;
        }
        if (ImGui::BeginPopupModal("Other###other_modal", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("other");
            if (s_other_close) {
                ImGui::CloseCurrentPopup();
                s_other_close = false;
            }
            ImGui::EndPopup();
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    };
    converted_frame();
    TEST_ASSERT_EQUAL_INT(1, shown);
    s_other_request = true;
    converted_frame(); /* cizí OpenPopup() převedený dialog zavře */
    converted_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, shown, "waits while the other modal is open");
    s_other_close = true;
    converted_frame();
    converted_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, shown, "reopened after the other modal closed");
    want = false;
    converted_frame();
    converted_frame();
    TEST_ASSERT_EQUAL_INT(0, shown);
}

/* Fronta zpráv: mez 8, chyba před čekající informace (za zobrazenou), vytlačení nejstarší ne-chyby. */
void test_message_queue_limit_and_errors(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    char text[64];
    ui_message_type_t type;
    for (int i = 0; i < 8; i++) {
        g_snprintf(text, sizeof(text), "info %d", i);
        imgui_message_create(MESSAGE_TYPE_INFO, text);
    }
    TEST_ASSERT_EQUAL_UINT(8, imgui_message_test_queued());
    imgui_message_create(MESSAGE_TYPE_INFO, "info overflow"); /* plná fronta: zahodí se */
    TEST_ASSERT_EQUAL_UINT(8, imgui_message_test_queued());
    TEST_ASSERT_TRUE(imgui_message_test_at(7, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("info 7", text);

    frame(); /* "info 0" se zobrazí: chyba ho nepředběhne */
    TEST_ASSERT_TRUE(imgui_message_test_popup_open());
    imgui_message_create(MESSAGE_TYPE_ERROR, "error A"); /* vytlačí "info 1" */
    TEST_ASSERT_EQUAL_UINT(8, imgui_message_test_queued());
    TEST_ASSERT_TRUE(imgui_message_test_at(0, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("info 0", text);
    TEST_ASSERT_TRUE(imgui_message_test_at(1, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(MESSAGE_TYPE_ERROR, type);
    TEST_ASSERT_EQUAL_STRING("error A", text);
    TEST_ASSERT_TRUE(imgui_message_test_at(2, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("info 2", text);

    imgui_message_create(MESSAGE_TYPE_ERROR, "error B"); /* za dřívější chybu, vytlačí "info 2" */
    TEST_ASSERT_TRUE(imgui_message_test_at(2, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("error B", text);
    TEST_ASSERT_TRUE(imgui_message_test_at(3, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("info 3", text);

    /* fronta jen z chyb: další chyba se zahodí */
    imgui_message_test_clear();
    for (int i = 0; i < 8; i++) {
        g_snprintf(text, sizeof(text), "error %d", i);
        imgui_message_create(MESSAGE_TYPE_ERROR, text);
    }
    imgui_message_create(MESSAGE_TYPE_ERROR, "error overflow");
    TEST_ASSERT_EQUAL_UINT(8, imgui_message_test_queued());
    TEST_ASSERT_TRUE(imgui_message_test_at(7, &type, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("error 7", text);
}

/* ================================================================
 * Výsledky úprav pásky z okna
 * ================================================================ */

/**
 * @brief Zapíše soubor MZF (hlavička 128 B + tělo) do dočasného adresáře.
 * @param name Jméno souboru i MZF (ASCII, nejvýš 16 znaků). @param fsize Velikost těla.
 * @return Cesta (g_free).
 */
static char *h4_mzf(const char *name, uint16_t fsize)
{
    uint8_t buf[128 + 512];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;
    memset(buf + 1, 0x0D, 17);
    memcpy(buf + 1, name, strlen(name));
    buf[0x12] = (uint8_t)fsize;
    buf[0x13] = (uint8_t)(fsize >> 8);
    buf[0x14] = 0x00;
    buf[0x15] = 0x12;
    buf[0x16] = 0x00;
    buf[0x17] = 0x12;
    for (int i = 0; i < fsize && i < 512; i++) buf[128 + i] = (uint8_t)(i * 7 + 3);
    gchar *file = g_strdup_printf("%s.mzf", name);
    char *path = ff_tmp(file);
    g_free(file);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)buf, 128 + fsize, NULL));
    return path;
}

/** @brief Jméno MZF bloku pásky (bez koncových 0x0D) přes MZF hlavičku jádra; "" bez MZF. */
static std::string h4_block_name(int id)
{
    st_CMTTMZ_BLOCK_UI bui;
    cmttmz_get_block_ui(id, &bui);
    std::string s;
    for (uint32_t i = 0; i < bui.mzf_name_len; i++) s += (char)bui.mzf_name[i];
    return s;
}

/** @brief Počet bloků vložené pásky (0 bez pásky). */
static int h4_block_count(void)
{
    st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    return c ? cmtext_container_get_count_blocks(c) : 0;
}

/** @brief Vyřídí zásahy fronty (za emu vlákno) a výsledky předá cestou aplikace (imgui_device_request_results()). */
static void h4_device_round(void)
{
    device_request_poll();
    imgui_device_request_results();
}

/*
 * Import více souborů postupně: další až po výsledku předchozího
 * s čítačem výměn pásky z výsledku. Do prázdného decku s čítačem 0 (nová
 * páska), pak na začátek pásky v pořadí souborů; nečitelný soubor řetěz
 * zastaví (chyba zůstane v řádku zpráv, zbytek se neimportuje).
 * Vybere se první blok prvního souboru.
 */
void test_h4_import_chain(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("ALPHA", 64), *b = h4_mzf("BETA", 32), *c = h4_mzf("GAMMA", 16), *d = h4_mzf("DELTA", 16);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    const char *p1[2] = {a, b};
    imgui_cmt_tape_import_paths(p1, 2, -1, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, device_request_pending(), "one import at a time");
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_import_active());
    h4_device_round();
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, h4_block_count());
    gchar *base = g_path_get_basename(a);
    gchar *exp = g_strdup_printf("Imported %s, blocks: 1", base);
    bool is_error = true;
    TEST_ASSERT_EQUAL_STRING(exp, ff_message_line(&is_error).c_str());
    TEST_ASSERT_FALSE(is_error);
    g_free(exp);
    g_free(base);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, device_request_pending(), "the second file is queued after the first result");
    h4_device_round();
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(2, h4_block_count());
    TEST_ASSERT_EQUAL_STRING("ALPHA", h4_block_name(0).c_str());
    TEST_ASSERT_EQUAL_STRING("BETA", h4_block_name(1).c_str());
    int sel = -1;
    TEST_ASSERT_TRUE(cmt_view_take_select_after_edit(cmttmz_get_tape_serial(), &sel));
    TEST_ASSERT_EQUAL_INT(0, sel);

    /* na začátek: GAMMA, nečitelný, DELTA -> řetěz se u nečitelného zastaví:
     * GAMMA, ALPHA, BETA; chyba nečitelného souboru zůstane v řádku zpráv i po posledním souboru */
    char *bad = ff_tmp("missing.mzf");
    const char *p2[3] = {c, bad, d};
    imgui_cmt_tape_import_paths(p2, 3, 0, cmttmz_get_tape_serial());
    for (int i = 0; i < 4; i++) {
        h4_device_round();
        frame();
    }
    std::string err = ff_message_line(&is_error);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp("Can't import ", err.c_str(), 13), err.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.c_str(), "missing.mzf"), err.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.c_str(), "The remaining files were not imported."), err.c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, h4_block_count(), "the file after the failing one is not imported");
    TEST_ASSERT_EQUAL_STRING("GAMMA", h4_block_name(0).c_str());
    TEST_ASSERT_EQUAL_STRING("ALPHA", h4_block_name(1).c_str());
    TEST_ASSERT_EQUAL_STRING("BETA", h4_block_name(2).c_str());
    TEST_ASSERT_TRUE(cmt_view_take_select_after_edit(cmttmz_get_tape_serial(), &sel));
    TEST_ASSERT_EQUAL_INT(0, sel);

    /* jiná páska mezi zařazením a provedením: zbytek se zruší, varování */
    const char *p3[2] = {a, b};
    imgui_cmt_tape_import_paths(p3, 2, -1, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("The tape changed; the action was cancelled. The remaining files were not imported.",
                             ff_message_line(&is_error).c_str());
    TEST_ASSERT_FALSE(is_error);
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_EQUAL_INT(0, h4_block_count());

    const char *files[5] = {a, b, c, d, bad};
    for (const char *f : files) {
        g_remove(f);
        g_free((char *)f);
    }
    cmt_view_test_force_window_shown(-1);
}

/*
 * Zprávy výsledků úprav: smazání jednoho / více bloků, chyba přesunu textem
 * jádra, jiná páska, odmítnutí při záznamu vstupů, příliš mnoho rozsahů,
 * export celé pásky.
 */
void test_h4_edit_result_messages(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("ONE", 16), *b = h4_mzf("TWO", 16), *c = h4_mzf("THREE", 16);
    const char *p[3] = {a, b, c};
    imgui_cmt_tape_import_paths(p, 3, -1, 0u);
    for (int i = 0; i < 3; i++) h4_device_round();
    TEST_ASSERT_EQUAL_INT(3, h4_block_count());
    bool is_error = false;

    int one = 1;
    TEST_ASSERT_TRUE(imgui_cmt_tape_remove_blocks(cmttmz_get_tape_serial(), &one, 1) > 0);
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("Deleted block 2.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_FALSE(is_error);
    int sel = -1;
    TEST_ASSERT_TRUE(cmt_view_take_select_after_edit(cmttmz_get_tape_serial(), &sel));
    TEST_ASSERT_EQUAL_INT(1, sel);
    TEST_ASSERT_TRUE(imgui_cmt_tape_move_block(cmttmz_get_tape_serial(), 0, 7) > 0);
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("Target position 7 is out of range (0 to 1)", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_FALSE(cmt_view_take_select_after_edit(cmttmz_get_tape_serial(), &sel));
    /* jiná páska */
    unsigned old = cmttmz_get_tape_serial();
    TEST_ASSERT_TRUE(imgui_cmt_tape_move_block(old, 0, 1) > 0);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    device_state_unlock();
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("The tape changed; the action was cancelled.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    /* záznam vstupů: fronta zásah odmítne */
    int both[2] = {0, 1};
    TEST_ASSERT_TRUE(imgui_cmt_tape_remove_blocks(cmttmz_get_tape_serial(), both, 2) > 0);
    g_replay_mode = REPLAY_MODE_RECORDING;
    h4_device_round();
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_EQUAL_STRING("The tape cannot be edited while an input replay is recorded or played.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_EQUAL_INT(2, h4_block_count());
    TEST_ASSERT_TRUE(imgui_cmt_tape_remove_blocks(cmttmz_get_tape_serial(), both, 2) > 0);
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("Deleted 2 blocks.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_FALSE_MESSAGE(cmt_view_take_select_after_edit(cmttmz_get_tape_serial(), &sel), "an empty tape has no block to select");
    /* příliš mnoho oddělených bloků: nic se nezařadí */
    int many[300];
    for (int i = 0; i < 300; i++) many[i] = 2 * i;
    TEST_ASSERT_EQUAL_UINT32(0u, imgui_cmt_tape_remove_blocks(cmttmz_get_tape_serial(), many, 300));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_EQUAL_STRING("Too many separate blocks selected.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);

    /* export celé pásky: text jádra; prázdná páska = chyba */
    const char *pp[1] = {a};
    imgui_cmt_tape_import_paths(pp, 1, -1, cmttmz_get_tape_serial());
    h4_device_round();
    char *wav = ff_tmp("whole.wav");
    device_state_lock();
    imgui_cmt_tape_export_tape(cmttmz_get_tape_serial());
    device_state_unlock();
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(NULL));
    imgui_cmt_tape_test_export_tape_finish(wav);
    h4_device_round();
    std::string m = ff_message_line(&is_error);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp("Tape exported: ", m.c_str(), 15), m.c_str());
    TEST_ASSERT_FALSE(is_error);
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    g_remove(wav);
    g_free(wav);

    const char *files[3] = {a, b, c};
    for (const char *f : files) {
        g_remove(f);
        g_free((char *)f);
    }
    cmt_view_test_force_window_shown(-1);
}

/*
 * Výsledek úpravy se čte ve stejném držení zámku jako vyzvednutí
 * z fronty. Výsledek vyzvednutý a zachycený pod zámkem se ohlásí správně,
 * i když mezitím další úpravy kruh výsledků přepsaly.
 */
void test_h4_capture_under_lock(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("CAPTURE", 16), *b = h4_mzf("OTHER", 16);
    const char *p[1] = {a};
    imgui_cmt_tape_import_paths(p, 1, -1, 0u);
    device_request_poll();
    st_DEVICE_REQ_RESULT res;
    device_state_lock();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    imgui_cmt_device_request_capture(&res);
    device_state_unlock();
    /* 33 dalších úprav kruh výsledků přepíše */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(b, h4_block_count(), NULL));
    for (int i = 0; i < 33; i++) {
        TEST_ASSERT_TRUE(cmt_request_move_block_on_tape(cmttmz_get_tape_serial(), 0, 1) > 0);
        device_request_poll();
    }
    st_DEVICE_REQ_RESULT junk;
    while (device_request_take_result(&junk)) {}
    st_CMT_REQUEST_EDIT_RESULT r;
    TEST_ASSERT_FALSE_MESSAGE(cmt_request_get_edit_result(res.seq, &r), "the ring no longer holds the first result");
    TEST_ASSERT_TRUE(imgui_cmt_device_request_result(&res));
    gchar *base = g_path_get_basename(a);
    gchar *exp = g_strdup_printf("Imported %s, blocks: 1", base);
    TEST_ASSERT_EQUAL_STRING(exp, ff_message_line(NULL).c_str());
    g_free(exp);
    g_free(base);
    g_remove(a);
    g_remove(b);
    g_free(a);
    g_free(b);
    cmt_view_test_force_window_shown(-1);
}


/**
 * @brief Zapíše TMZ s blokem 0x43 (takt @p clock Hz) a za ním jedním blokem MZF "WARN" (pro varování taktu).
 * @param name Jméno souboru v dočasném adresáři. @param clock Takt CPU v Hz.
 * @return Cesta (g_free).
 */
static char *wave_clock_tmz(const char *name, uint32_t clock)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    st_TZX_BLOCK *mi = tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ800, clock, 0);
    TEST_ASSERT_NOT_NULL(mi);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, mi));
    free(mi);
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = 0x01;
    memset(&hdr.fname, 0x0D, sizeof(hdr.fname));
    memcpy(&hdr.fname, "WARN", 4);
    hdr.fsize = 16;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[16];
    for (int i = 0; i < 16; i++) body[i] = (uint8_t)(i * 5 + 1);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 16, CMTSPEED_1_1, NULL, 0);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    char *path = ff_tmp(name);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    return path;
}

/** @brief Zapíše TAP se dvěma bloky (hlavička ZX a data). @param name Jméno souboru. @return Cesta (g_free). */
static char *wave_tap(const char *name)
{
    GByteArray *ba = g_byte_array_new();
    uint8_t hdr[19];
    memset(hdr, 0, sizeof(hdr));
    hdr[1] = 3;
    memcpy(&hdr[2], "TAPTEST   ", 10);
    hdr[12] = 4;
    hdr[15] = 0x80;
    uint8_t x = 0;
    for (int i = 0; i < 18; i++) x ^= hdr[i];
    hdr[18] = x;
    uint8_t data[6] = {0xFF, 1, 2, 3, 4, 0};
    x = 0;
    for (int i = 0; i < 5; i++) x ^= data[i];
    data[5] = x;
    const uint8_t l1[2] = {sizeof(hdr), 0}, l2[2] = {sizeof(data), 0};
    g_byte_array_append(ba, l1, 2);
    g_byte_array_append(ba, hdr, sizeof(hdr));
    g_byte_array_append(ba, l2, 2);
    g_byte_array_append(ba, data, sizeof(data));
    char *path = ff_tmp(name);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)ba->data, ba->len, NULL));
    g_byte_array_free(ba, TRUE);
    return path;
}

/*
 * Postupný import nikdy nečeká navždy - každý konec zásahu
 * ho ukončí nebo posune: odmítnutí frontou (záznam vstupů), jiná páska
 * (test_h4_import_chain), nezařazený zásah (dlouhá cesta), ztracený výsledek
 * (device_request_clear()). Každý konec ohlásí zprávu a zbytek souborů se
 * neimportuje; nový import pak jde.
 */
void test_wave_import_chain_always_ends(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("CHAIN1", 16), *b = h4_mzf("CHAIN2", 16);
    bool is_error = false;
    const char *p[2] = {a, b};

    /* odmítnutí frontou (záznam vstupů začal mezi zařazením a provedením) */
    imgui_cmt_tape_import_paths(p, 2, -1, 0u);
    g_replay_mode = REPLAY_MODE_RECORDING;
    h4_device_round();
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_EQUAL_STRING("The tape cannot be edited while an input replay is recorded or played. The remaining files were not imported.",
                             ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    /* nezařazený zásah: cesta delší než argument zásahu, uprostřed řetězu */
    std::string longp(1100, 'x');
    longp += ".mzf";
    const char *pl[3] = {a, longp.c_str(), b};
    imgui_cmt_tape_import_paths(pl, 3, -1, 0u);
    for (int i = 0; i < 3; i++) h4_device_round();
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_STRING("The file could not be imported: the file path is too long. The remaining files were not imported.",
                             ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(is_error);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, h4_block_count(), "only the file before the long path is imported");

    /* ztracený výsledek: hlídač (imgui_cmt_tape_dialogs()) import ukončí v dalším snímku */
    imgui_cmt_tape_import_paths(p, 2, -1, cmttmz_get_tape_serial());
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_import_active());
    device_request_clear();
    frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_cmt_tape_test_import_active(), "one frame with an empty queue is not enough");
    frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_cmt_tape_test_import_active(), "a lost result ends the import");
    TEST_ASSERT_EQUAL_STRING("The import could not be completed. The remaining files were not imported.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    /* další import jde (žádné "Another import is still in progress.") */
    imgui_cmt_tape_import_paths(p, 1, -1, cmttmz_get_tape_serial());
    h4_device_round();
    TEST_ASSERT_EQUAL_INT(2, h4_block_count());
    /* čekající zásah se nepovažuje za ztracený */
    imgui_cmt_tape_import_paths(p, 2, -1, cmttmz_get_tape_serial());
    for (int i = 0; i < 3; i++) frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_cmt_tape_test_import_active(), "a queued import waits for its result");
    h4_device_round();
    h4_device_round();
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(4, h4_block_count());

    g_remove(a);
    g_remove(b);
    g_free(a);
    g_free(b);
    cmt_view_test_force_window_shown(-1);
}

/*
 * Varování importu dřívějšího souboru řetězu (takt CPU)
 * nepřepíše informace o dalším souboru - úroveň zprávy během řetězu neklesne.
 */
void test_wave_import_chain_keeps_warning(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *x = h4_mzf("XBLOCK", 16);
    char *w = wave_clock_tmz("clock.tmz", 4000000u);
    char *t = wave_tap("plain.tap");
    const char *one[1] = {x};
    imgui_cmt_tape_import_paths(one, 1, -1, 0u);
    h4_device_round();
    TEST_ASSERT_EQUAL_INT(1, h4_block_count());
    /* w před XBLOCK: XBLOCK teď hraje taktem bloku 0x43 (varování); TAP za ním takt nepoužívá (bez varování) */
    const char *p[2] = {w, t};
    imgui_cmt_tape_import_paths(p, 2, 0, cmttmz_get_tape_serial());
    for (int i = 0; i < 3; i++) h4_device_round();
    TEST_ASSERT_FALSE(imgui_cmt_tape_test_import_active());
    TEST_ASSERT_EQUAL_INT(5, h4_block_count());
    bool is_error = true;
    std::string m = ff_message_line(&is_error);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(m.c_str(), "clock.tmz"), m.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(m.c_str(), "The tape clock setting now applies to different blocks"), m.c_str());
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    TEST_ASSERT_FALSE(is_error);
    char *files[3] = {x, w, t};
    for (char *f : files) {
        g_remove(f);
        g_free(f);
    }
    cmt_view_test_force_window_shown(-1);
}


/*
 * Smazání jednoho bloku nikdy nehlásí "Deleted 1 blocks." -
 * číslo bloku nese výsledek úpravy, ne pamatovaný kruh okna (zásah zařazený
 * jinudy než oknem, jako po přetečení kruhu, dá "Deleted block %d.").
 */
void test_wave_deleted_single_block_message(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("DEL1", 16), *b = h4_mzf("DEL2", 16), *c = h4_mzf("DEL3", 16);
    const char *p[3] = {a, b, c};
    imgui_cmt_tape_import_paths(p, 3, -1, 0u);
    for (int i = 0; i < 3; i++) h4_device_round();
    TEST_ASSERT_EQUAL_INT(3, h4_block_count());
    const int third[1] = {2};
    TEST_ASSERT_TRUE(cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), third, 1) > 0);
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("Deleted block 3.", ff_message_line(NULL).c_str());
    const int two[2] = {0, 1};
    TEST_ASSERT_TRUE(imgui_cmt_tape_remove_blocks(cmttmz_get_tape_serial(), two, 2) > 0);
    h4_device_round();
    TEST_ASSERT_EQUAL_STRING("Deleted 2 blocks.", ff_message_line(NULL).c_str());
    char *files[3] = {a, b, c};
    for (char *f : files) {
        g_remove(f);
        g_free(f);
    }
    cmt_view_test_force_window_shown(-1);
}

/*
 * Export celé pásky nese čítač výměn pásky až na emu vlákno.
 * Úprava pásky (MCP) mezi volbou souboru a provedením = nic se nezapíše,
 * varování; deck mezitím v PLAY = export se nezařadí, varování.
 */
void test_wave_export_tape_carries_serial(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    cmt_view_test_force_window_shown(1);
    ff_message_begin();
    char *a = h4_mzf("EXP1", 16), *b = h4_mzf("EXP2", 16);
    const char *p[2] = {a, b};
    imgui_cmt_tape_import_paths(p, 2, -1, 0u);
    for (int i = 0; i < 2; i++) h4_device_round();
    char *wav = ff_tmp("serial.wav");
    g_remove(wav);
    /* úprava po zařazení, před provedením */
    device_state_lock();
    imgui_cmt_tape_export_tape(cmttmz_get_tape_serial());
    device_state_unlock();
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(NULL));
    imgui_cmt_tape_test_export_tape_finish(wav);
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    device_state_unlock();
    h4_device_round();
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(wav, G_FILE_TEST_EXISTS), "nothing is exported for an edited tape");
    bool is_error = true;
    TEST_ASSERT_EQUAL_STRING("The tape changed; the action was cancelled.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    /* deck v PLAY v okamžiku volby souboru: nic se nezařadí */
    device_state_lock();
    imgui_cmt_tape_export_tape(cmttmz_get_tape_serial());
    device_state_unlock();
    TEST_ASSERT_TRUE(imgui_cmt_tape_test_take_last_export_tape(NULL));
    g_cmt.paused = 0;
    cmt_play();
    imgui_cmt_tape_test_export_tape_finish(wav);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, device_request_pending(), "no export is queued outside STOP");
    TEST_ASSERT_EQUAL_STRING("The tape state changed; the action was cancelled.", ff_message_line(&is_error).c_str());
    TEST_ASSERT_TRUE(s_ff_view.message_is_warning);
    cmt_stop();
    g_remove(a);
    g_remove(b);
    g_free(a);
    g_free(b);
    g_free(wav);
    cmt_view_test_force_window_shown(-1);
}

/*
 * Soubor přetažený nad okno mimo tabulku = otevření obvyklým postupem:
 * u pásky s neuloženými změnami se automat zeptá, Cancel nic neotevře.
 */
void test_h4_drop_open_asks(void)
{
    if (!s_ui_ok) TEST_IGNORE_MESSAGE("SDL/GL not available");
    modified_tape();
    imgui_cmt_tape_open_path("C:/tapes/dropped.tmz");
    frame();
    TEST_ASSERT_EQUAL_INT(2, imgui_cmt_tape_test_dialog_shown());
    imgui_cmt_tape_test_choose(IMGUI_CMT_TEST_CHOICE_CANCEL);
    frame();
    TEST_ASSERT_FALSE(imgui_cmt_tape_flow_active());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_NONE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    /* bez neuložených změn hned */
    cmttmz_set_modified(0);
    imgui_cmt_tape_open_path("C:/tapes/dropped.tmz");
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_OPEN_FILE, imgui_cmt_tape_test_take_last_run());
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    device_request_clear();
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_ui_ok = ui_setup();
    if (!s_ui_ok) {
        /* bez SDL/GL nejde nic z testu: ctest ho ukáže jako přeskočený (SKIP_RETURN_CODE 77) */
        fprintf(stderr, "ui-cmt-dialogs: SDL/GL not available, test skipped\n");
        ui_teardown();
        mztest_teardown();
        return 77;
    }
    ImGui::StyleColorsDark();

    UNITY_BEGIN();
    RUN_TEST(test_unsaved_dialog_survives_queued_message);
    RUN_TEST(test_stop_recording_then_unsaved_dialog_with_record_message);
    RUN_TEST(test_dialog_closed_by_other_modal_reopens);
    RUN_TEST(test_file_action_rechecks_state);
    RUN_TEST(test_message_dedup);
    RUN_TEST(test_replay_playback_never_asks);
    RUN_TEST(test_replay_recording_offers_no_save);
    RUN_TEST(test_quit_during_playback_stops_it_first);
    RUN_TEST(test_dialog_action_runs_after_lock_release);
    RUN_TEST(test_deferred_action_rechecks_state);
    RUN_TEST(test_save_tape_success_continues_action);
    RUN_TEST(test_save_tape_failure_aborts_action);
    RUN_TEST(test_save_failure_hidden_window_and_exit_is_modal);
    RUN_TEST(test_g6_quit_save_errors_always_modal);
    RUN_TEST(test_save_tape_save_as_fallback_and_cancel);
    RUN_TEST(test_ff_save_as_tape_changed);
    RUN_TEST(test_recording_consent_not_reused);
    RUN_TEST(test_quit_request_waits_for_foreign_popup);
    RUN_TEST(test_modal_open_when_free_reopens);
    RUN_TEST(test_message_queue_limit_and_errors);
    RUN_TEST(test_h4_import_chain);
    RUN_TEST(test_h4_edit_result_messages);
    RUN_TEST(test_h4_capture_under_lock);
    RUN_TEST(test_wave_import_chain_always_ends);
    RUN_TEST(test_wave_import_chain_keeps_warning);
    RUN_TEST(test_wave_deleted_single_block_message);
    RUN_TEST(test_wave_export_tape_carries_serial);
    RUN_TEST(test_h4_drop_open_asks);
    int result = UNITY_END();

    ui_teardown();
    mztest_teardown();
    return result;
}
