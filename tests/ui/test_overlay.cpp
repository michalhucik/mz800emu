/**
 * @file   test_overlay.cpp
 * @brief  Testy overlay menu (src/ui-imgui/overlay/): rámec, stav otevření, fokus, klávesy, pauza, snímky.
 *
 * Skutečné ImGui ve skrytém okně SDL + GL (vzor test_cmt_window.cpp) nad
 * skutečným jádrem (mztest). Hlavní okno emulátoru nahrazuje ov_frame():
 * okno se stejnými příznaky (MAIN_WINDOW_FLAGS) a stejným pořadím kroků
 * jako ShowFullScreenImageEmulatorWindow() v main_window.cpp (pravidlo
 * klávesnice main_window_keyboard_enabled(), pravidlo přepínače oken
 * main_window_nav_windowing_allowed(), vrácení fokusu overlayi,
 * F10 pravidlem main_window_overlay_open_key() jen při fokusu obrazu,
 * potom imgui_overlay_frame()).
 * main_window.cpp samotný test nelinkuje (táhne celé UI).
 *
 * Co se ověřuje:
 * - kopie stavu st_OVERLAY_VIEW: plnění z jádra a testovací stav;
 * - otevření a zavření mění imgui_overlay_is_open(), volba tabu;
 * - tři snímky bez pádu, okno OVERLAY_WINDOW_ID, vyvážené zásobníky ImGui;
 * - klávesy: F10 a Esc zavírají (stisk F10, který overlay otevřel, ho
 *   nezavře), číslice 1 až 6 a Ctrl+Tab / Ctrl+Shift+Tab přepínají taby;
 *   myš: klik na tab a na Close;
 * - klávesnice emulovaného stroje (Review Focus 1): při zavřeném overlayi
 *   pravidlo klávesy pouští, při otevřeném ne a matice stroje je klidová,
 *   po zavření se pravidlo vrátí a matice zůstane klidová;
 * - pauza (Review Focus 2): běžící emulace po zavření běží, stojící stojí,
 *   imgui_overlay_note_pause_changed() zapamatovaný stav přepíše;
 * - poslech na tabu Sound: emulace stojí i na něm, přepínač Listen pauzu
 *   overlaye ruší a vrací, sám se vypíná při změně tabu, výsuvném listu a
 *   cizí pauze, cizí pauzu ani pauzu uživatele nerozbíhá
 *   (test_sound_listen_toggle());
 * - trvale zapnutý ImGuiConfigFlags_NavEnableKeyboard při zavřeném
 *   overlayi: klávesy navigace ImGui ani Ctrl+Tab neodnesou fokus z obrazu
 *   a dojdou do matice stroje; z okna nástroje Ctrl+Tab fokus do obrazu
 *   vrátí (test_nav_keyboard_probe());
 * - vizuální systém (overlay_style.h): jednotka u a nejmenší písmo, písma
 *   a ikony, náhradní písmo při chybějících souborech, kontrasty spočítané
 *   z tokenů barev (text 4,5:1, okraje prvků a text zakázaných prvků 3:1,
 *   text nad bílým obrazem při krytí plochy 85 %);
 * - rámec při 640 x 480, 1280 x 720 a 2560 x 1440: nic mimo okno, nic se
 *   nepřekrývá, ústup prvků horní lišty v úzkém okně;
 * - widgety (overlay_widgets.h) v galerii gallery_draw(): geometrie ve
 *   třech rozměrech, tlačítko, přepínač, segmentová volba a posuvník myší
 *   i klávesnicí, zakázaný stav, výpustka u textu o 200 znacích.
 *
 * - pauza při selhání: nepovedené zastavení nic nerozbíhá, nepovedené
 *   rozběhnutí se opakuje se stropem, pauzu cizího původu zavření nepřebije;
 * - tab Media (overlay_tab_media.cpp) a tabulka akcí (overlay_actions.h):
 *   kopie stavu nad skutečným jádrem (bez diskety, s disketou, vysunutí
 *   z jiného místa mezi dvěma plněními, odpojený řadič, motor, replay),
 *   testovací stavy, zkratky mechanik na klávesách 1 až 4, pravidla
 *   dostupnosti; geometrie pěti stavů ve čtyřech rozměrech okna s texty
 *   běžnými i o 40 % delšími, výpustka a tooltip u jména o 200 znacích;
 *   každé tlačítko zařadí správný požadavek (fronty
 *   device_request_test_take_pending() a replay_test_take_ui_cmt(), stuby
 *   funkcí UI); zakázané karty při replay a bez řadiče nic nezařadí;
 *   zkratky akcí v okně overlaye; akce zavírající overlay a pauza po nich;
 *   výchozí fokus a pohyb šipkami; vysunutí diskety zvenčí mezi snímky.
 *   Další registrace ui-overlay-de a ui-overlay-cs (MZ_OVERLAY_TEST_LANG)
 *   spustí jen geometrii rámce, tabu Media a výsuvného listu v němčině
 *   a v češtině (v češtině se v okně 640 x 480 uplatní poslední stupně
 *   ústupu horní lišty: štítek pauzy jen ikonou);
 * - opravy z review vizuálního systému: ořez linky nadpisu karty při
 *   rolování, sdílená paměť souborů písem (jedno čtení z disku, stejné
 *   glyfy jako načtení ze souboru), japonské znaky v neproporcionálním
 *   písmu, zakázaný posuvník a klik na číselný údaj posuvníku;
 * - nedávná média a výsuvný list výměny média (overlay_insert_sheet.cpp):
 *   kopie historie jádra (pořadí, existence souborů, strop, příliš dlouhá
 *   cesta); geometrie listu u každé mechaniky, kazety a Quick Disku ve
 *   třech rozměrech okna; číslice a Enter zařadí vložení správného souboru
 *   do správné mechaniky; filtr, Backspace, mezera; Esc zavře jen list;
 *   zkratky tabů a akcí při otevřeném listu nefungují; Browse files...,
 *   Eject a Write protect; list kazety (automat akcí s páskou, stub
 *   imgui_cmt_tape_open_path()) a Quick Disku; soubor, který už
 *   neexistuje, se z historie odstraní; pruh nedávných médií a volba
 *   mechaniky "Insert into"; modální dialog nad overlayem (Review Focus 6:
 *   skutečný souborový dialog test nelinkuje, zastupuje ho modální popup
 *   ImGui).
 *
 * - taby Machine a Display (overlay_tab_machine.cpp, overlay_tab_display.cpp):
 *   kopie stavu nad skutečným jádrem (nabídka ROM, nastavená rychlost,
 *   g_display, dotaz na fullscreen); geometrie ve čtyřech rozměrech okna, s
 *   delšími texty a v němčině; každý prvek zavolá funkci klasického menu
 *   (stuby snapshotů a pauzy, fronta požadavků, stav g_customspeed a
 *   g_display) a čte stav z kopie; rozbalovací výběr myší i klávesnicí,
 *   jeho poloha v malém okně; nové zkratky; žádost o klasické menu;
 *   krytí plochy 55 % na tabu Display.
 * - opravy z review O3: krok nebo krátký běh emulace mezi dvěma snímky UI
 *   (jiné číslo běhu jádra, run_log.h) zavření nepřebije; toast potvrzení
 *   (skutečné imgui_snapshot_notification()) se při otevřeném overlayi
 *   kreslí nad ním.
 *
 * Stav emulace: dbg_ui_pause() a dbg_ui_run() stubuje test (mění
 * g_emulator.paused a počítá volání, umí selhat); emu vlákno neběží.
 * Funkce UI jiných modulů, které akce volají (snapshoty, screenshot,
 * automat akcí s páskou), stubuje test a počítá jejich volání.
 *
 * Režim snímků (mimo CTest): proměnná MZ_OVERLAY_SHOT_DIR = adresář, do
 * kterého se uloží snímky všech tabů z testovacího stavu
 * OVERLAY_FIXTURE_DEFAULT, jména `<etapa>-<jazyk>-<šířka>x<výška>-<tab>.png`,
 * a galerie widgetů ve všech stavech `<etapa>-gallery.png` (při jiném
 * rozměru než 1280 x 720 `<etapa>-gallery-<šířka>x<výška>.png`).
 * Volitelně MZ_OVERLAY_SHOT_W a MZ_OVERLAY_SHOT_H (výchozí 1280 x 720),
 * MZ_OVERLAY_SHOT_LANG (jazyk katalogu z MZTEST_LOCALE_DIR, výchozí en)
 * a MZ_OVERLAY_SHOT_PREFIX (označení etapy ve jménech, výchozí o2).
 * MZ_OVERLAY_SHOT_SET=media uloží místo toho tab Media ve všech
 * testovacích stavech, jména `<etapa>-<jazyk>-<šířka>x<výška>-<stav>.png`
 * (stavy default, empty, long, replay, nofdc); MZ_OVERLAY_SHOT_SET=o5
 * uloží taby Machine a Display a rozbalený výběr ROM (jména `<etapa>-
 * <jazyk>-<šířka>x<výška>-machine.png`, `-display.png`, `-machine-rom.png`);
 * MZ_OVERLAY_SHOT_SET=sheet
 * uloží výsuvný list, pruh nedávných médií a volbu mechaniky (stavy
 * recent, fdd0, fdd3, cmt, qd, filter, pick, long-fdd1, empty-fdd0).
 * Bez MZ_OVERLAY_SHOT_DIR test nic nezapisuje. Bez SDL / GL vrací 77.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <locale.h>
#include <math.h>
#include <glib/gstdio.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_opengl3.h"

/* hlavičky jádra (mají vlastní extern "C") */
#include "mztest.h"
#include "device_request.h"
#include "replay.h"
#include "emulator/emulator.h"
#include "emulator/customspeed.h"
#include "emulator/display.h"
#include "hw-generic/memory/rom.h"
#include "emulator/recent_media/recent_media.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/cmt/cmt_request.h"
#include "iface/iface_video.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/memory_driver.h"
#include "debugger/png_encode.h"
#include "debugger/run_control/run_log.h"

#include "ui-imgui/main_window_focus.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/videorec/videorec_menu.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/screenshot/screenshot_ui.h"
#include "audio.h"
#include "iface/iface_audio.h"
#include "iface/iface_joy.h"
#include "iface/iface_keyboard.h"
#include "hw-generic/joy/joy.h"
#include "emulator/videorec/videorec.h"
#include "ui-imgui/message/message_window.h"
#include "ui-imgui/overlay/overlay.h"
#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"
#include "i18n.h"

/* Písma aplikace a katalogy překladů (tests/ui/CMakeLists.txt); prázdné = nejsou. */
#ifndef MZTEST_FONT_DIR
#define MZTEST_FONT_DIR ""
#endif
#ifndef MZTEST_LOCALE_DIR
#define MZTEST_LOCALE_DIR ""
#endif

/* ================================================================
 * Stuby řízení emulace (dbgapi_helpers.cpp test nelinkuje)
 * ================================================================ */

/** @brief Počet volání dbg_ui_pause() od posledního vynulování. */
static int s_pause_calls = 0;
/** @brief Počet volání dbg_ui_run() od posledního vynulování. */
static int s_run_calls = 0;

/** @brief dbg_ui_pause() má selhat (emu vlákno neodpovědělo): emulaci nezastaví a vrátí false. */
static bool s_pause_fail = false;
/** @brief Kolik příštích volání dbg_ui_run() má selhat (emulaci nerozběhne a vrátí false). */
static int s_run_fail = 0;

/** @brief Strop pokusů o rozběhnutí emulace (OVL_RUN_RETRY_ATTEMPTS v overlay.cpp). */
#define OVL_TEST_RUN_ATTEMPTS 5

/** @brief Stub: zastaví emulaci (jen příznak g_emulator.paused), počítá volání; při s_pause_fail selže. */
extern "C" bool dbg_ui_pause(void)
{
    s_pause_calls++;
    if (s_pause_fail) return false;
    g_emulator.paused = true;
    return true;
}

/** @brief Počet volání dbg_ui_pause_toggle() od posledního vynulování. */
static int s_pause_toggle_calls = 0;

/** @brief Stub: přepne pauzu emulace (jen příznak g_emulator.paused) a počítá volání. */
extern "C" bool dbg_ui_pause_toggle(void)
{
    s_pause_toggle_calls++;
    g_emulator.paused = !g_emulator.paused;
    return true;
}

/** @brief Stub: rozběhne emulaci (jen příznak g_emulator.paused), počítá volání; při s_run_fail > 0 selže. */
extern "C" bool dbg_ui_run(void)
{
    s_run_calls++;
    if (s_run_fail > 0) {
        s_run_fail--;
        return false;
    }
    g_emulator.paused = false;
    return true;
}

/* ================================================================
 * Stuby funkcí UI, které volá tabulka akcí (test jejich moduly nelinkuje)
 * ================================================================ */

/** @brief UI globál, který jinak vytváří sdlapp_imgui_video.cpp. */
static MyImGui s_gui;
MyImGui *g_gui = &s_gui;

/** @brief Počet volání snapshot_ui_request_quicksave(). */
static int s_quicksave_calls = 0;
/** @brief Počet volání snapshot_ui_request_quickload(). */
static int s_quickload_calls = 0;
/** @brief Byl overlay při posledním volání snapshot_ui_request_quickload() otevřený? */
static bool s_quickload_overlay_open = false;
/** @brief Stála emulace při posledním volání snapshot_ui_request_quickload()? */
static bool s_quickload_paused = false;
/** @brief Počet volání screenshot_ui_save_quick(). */
static int s_screenshot_calls = 0;
/** @brief Počet volání náhradního přepnutí fullscreenu (test_fullscreen_cb()). */
static int s_fullscreen_calls = 0;
/** @brief Počet volání imgui_cmt_tape_action(). */
static int s_tape_action_calls = 0;
/** @brief Akce z posledního volání imgui_cmt_tape_action(). */
static en_IMGUI_CMT_TAPE_ACTION s_tape_action_last = IMGUI_CMT_TAPE_ACTION_NONE;

/** @brief Stub: Quick Save (menu_snapshot.cpp). */
extern "C" void snapshot_ui_request_quicksave(void)
{
    s_quicksave_calls++;
}

/** @brief Stav stubu: žádost o Quick Save čeká na vyzvednutí hlavním oknem. */
static bool s_stub_quicksave_requested = false;
/** @brief Stav stubu: žádost o Save Snapshot čeká na vyzvednutí hlavním oknem. */
static bool s_stub_save_requested = false;
/** @brief Počet volání snapshot_ui_run_after_request(). */
static int s_run_after_request_calls = 0;

/** @brief Stub: čeká žádost o Quick Save (menu_snapshot.cpp)? */
extern "C" bool snapshot_ui_is_quicksave_requested(void)
{
    return s_stub_quicksave_requested;
}

/** @brief Stub: čeká žádost o Save Snapshot (menu_snapshot.cpp)? */
extern "C" bool snapshot_ui_is_save_requested(void)
{
    return s_stub_save_requested;
}

/** @brief Stub: po vyřízení uložení snapshotu má emulace běžet (menu_snapshot.cpp). */
extern "C" void snapshot_ui_run_after_request(void)
{
    s_run_after_request_calls++;
}

/** @brief Stub: Quick Load (menu_snapshot.cpp); zapamatuje stav overlaye a pauzy v době volání. */
extern "C" void snapshot_ui_request_quickload(void)
{
    s_quickload_calls++;
    s_quickload_overlay_open = imgui_overlay_is_open();
    s_quickload_paused = g_emulator.paused;
}

/** @brief Počet volání snapshot_ui_request_save(). */
static int s_snapshot_save_calls = 0;
/** @brief Počet volání snapshot_ui_request_load(). */
static int s_snapshot_load_calls = 0;
/** @brief Byl overlay při posledním volání snapshot_ui_request_load() otevřený? */
static bool s_snapshot_load_overlay_open = false;

/** @brief Stub: Save Snapshot... (menu_snapshot.cpp). */
extern "C" void snapshot_ui_request_save(void)
{
    s_snapshot_save_calls++;
}

/** @brief Stub: Load Snapshot... (menu_snapshot.cpp); zapamatuje stav overlaye v době volání. */
extern "C" void snapshot_ui_request_load(void)
{
    s_snapshot_load_calls++;
    s_snapshot_load_overlay_open = imgui_overlay_is_open();
}

/** @brief Stub: uložení screenshotu (screenshot_ui.cpp). */
extern "C" void screenshot_ui_save_quick(void)
{
    s_screenshot_calls++;
}

/** @brief Počet volání stubu screenshot_ui_save_as(). */
static int s_shot_as_calls = 0;
/** @brief Počet volání stubu screenshot_ui_toggle_series(). */
static int s_series_calls = 0;
/** @brief Počet volání stubu screenshot_ui_open_output_folder(). */
static int s_shot_folder_calls = 0;
/** @brief Počet volání stubu videorec_ui_toggle_recording(). */
static int s_video_toggle_calls = 0;
/** @brief Počet volání stubu videorec_ui_toggle_pause(). */
static int s_video_pause_calls = 0;
/** @brief Počet volání stubu videorec_ui_toggle_timebase(). */
static int s_video_timebase_calls = 0;
/** @brief Počet volání stubu videorec_ui_open_output_folder(). */
static int s_video_folder_calls = 0;
/** @brief Stub videorec_ui_is_start_pending(): čeká start nahrávání. */
static bool s_video_start_pending = false;
/** @brief Počet volání stubu imgui_replay_open_record_dialog(). */
static int s_replay_record_calls = 0;
/** @brief Počet volání stubu imgui_replay_open_play_dialog(). */
static int s_replay_play_calls = 0;
/** @brief Počet volání stubu imgui_replay_request(). */
static int s_replay_request_calls = 0;
/** @brief Byl overlay otevřený ve chvíli, kdy stub dostal žádost o dialog replay? */
static bool s_replay_dialog_overlay_open = false;

/** @brief Stub: stav rozhraní joysticku hostitele (iface_joy.c v testu není). */
st_IFACE_JOY g_iface_joy;
/** @brief Počet volání stubu iface_joy_open_sysdevice(). */
static int s_joy_open_calls = 0;
/** @brief Počet volání stubu iface_joy_close_device(). */
static int s_joy_close_calls = 0;
/** @brief Počet volání stubu iface_joy_set_params(). */
static int s_joy_params_calls = 0;

/** @brief Návratová hodnota stubu iface_joy_open_sysdevice() (EXIT_SUCCESS / EXIT_FAILURE). */
static int s_joy_open_result = EXIT_SUCCESS;

/** @brief Stub: otevření zařízení joysticku (iface_joy.c). */
extern "C" int iface_joy_open_sysdevice(en_JOY_DEVID joy_devid, st_IFACE_JOY_SYSDEVICE *joy_sysdevice)
{
    (void)joy_devid;
    (void)joy_sysdevice;
    s_joy_open_calls++;
    return s_joy_open_result;
}

/** @brief Stub: zavření zařízení joysticku (iface_joy.c). */
extern "C" void iface_joy_close_device(en_JOY_DEVID joy_devid)
{
    (void)joy_devid;
    s_joy_close_calls++;
}

/** @brief Stub: mapování joysticku (iface_joy.c). */
extern "C" void iface_joy_set_params(en_JOY_DEVID joy_devid, st_IFACE_JOY_PARAMS *params)
{
    (void)joy_devid;
    (void)params;
    s_joy_params_calls++;
}

/** @brief Stub: výchozí mapování joysticku (iface_joy.c). */
extern "C" void iface_joy_set_default_params(st_IFACE_JOY_PARAMS *params)
{
    memset(params, 0, sizeof(*params));
}

/** @brief Stub: uložení screenshotu do zvoleného souboru (screenshot_ui.cpp). */
extern "C" void screenshot_ui_save_as(void)
{
    s_shot_as_calls++;
}

/** @brief Stub: série snímků (screenshot_ui.cpp). */
extern "C" void screenshot_ui_toggle_series(void)
{
    s_series_calls++;
}

/** @brief Stub: výstupní adresář screenshotů (screenshot_ui.cpp). */
extern "C" void screenshot_ui_open_output_folder(void)
{
    s_shot_folder_calls++;
}

/** @brief Stub: start a konec video záznamu (videorec_menu.cpp). */
extern "C" void videorec_ui_toggle_recording(void)
{
    s_video_toggle_calls++;
}

/** @brief Stub: pauza video záznamu (videorec_menu.cpp). */
extern "C" void videorec_ui_toggle_pause(void)
{
    s_video_pause_calls++;
}

/** @brief Stub: časová základna video záznamu (videorec_menu.cpp). */
extern "C" void videorec_ui_toggle_timebase(void)
{
    s_video_timebase_calls++;
}

/** @brief Stub: výstupní adresář video záznamu (videorec_menu.cpp). */
extern "C" void videorec_ui_open_output_folder(void)
{
    s_video_folder_calls++;
}

/** @brief Stub: čeká start nahrávání? (videorec_menu.cpp). */
extern "C" bool videorec_ui_is_start_pending(void)
{
    return s_video_start_pending;
}

/** @brief Stub: model položek menu video záznamu se stejnou logikou jako videorec_menu.cpp. */
extern "C" st_VIDEOREC_UI_MENU_MODEL videorec_ui_menu_model(en_VIDEOREC_STATE state, bool start_pending)
{
    st_VIDEOREC_UI_MENU_MODEL m;
    memset(&m, 0, sizeof(m));
    const bool active = (state != VIDEOREC_STATE_IDLE);
    m.start_stop_label = (active || start_pending) ? "Stop Recording" : "Start Recording";
    m.start_stop_enabled = true;
    m.pause_enabled = active;
    m.pause_checked = m.pause_enabled && (state == VIDEOREC_STATE_PAUSED);
    m.marker_enabled = m.pause_enabled;
    return m;
}

/** @brief Stub: dialog startu záznamu vstupů (menu_replay.cpp). */
extern "C" void imgui_replay_open_record_dialog(void)
{
    s_replay_record_calls++;
    s_replay_dialog_overlay_open = imgui_overlay_is_open();
}

/** @brief Stub: dialog přehrání záznamu vstupů (menu_replay.cpp). */
extern "C" void imgui_replay_open_play_dialog(void)
{
    s_replay_play_calls++;
    s_replay_dialog_overlay_open = imgui_overlay_is_open();
}

/** @brief Stub: ohlášení výsledku požadavku replay (menu_replay.cpp). */
extern "C" void imgui_replay_request(uint32_t seq)
{
    (void)seq;
    s_replay_request_calls++;
}

/** @brief Stub: automat akcí s páskou (cmt_tape_actions.cpp). */
extern "C" void imgui_cmt_tape_action(en_IMGUI_CMT_TAPE_ACTION action)
{
    s_tape_action_calls++;
    s_tape_action_last = action;
}

/** @brief Počet volání imgui_cmt_tape_open_path(). */
static int s_tape_open_calls = 0;
/** @brief Cesta z posledního volání imgui_cmt_tape_open_path(). */
static char s_tape_open_path[1024];
/** @brief Návratová hodnota stubu imgui_cmt_tape_open_path() (false = běží jiný dotaz automatu). */
static bool s_tape_open_result = true;

/** @brief Stub: otevření pásky ze souboru obvyklým postupem (cmt_tape_actions.cpp); zapamatuje cestu. */
extern "C" bool imgui_cmt_tape_open_path(const char *path)
{
    s_tape_open_calls++;
    g_strlcpy(s_tape_open_path, path ? path : "", sizeof(s_tape_open_path));
    return s_tape_open_result;
}

extern "C"
{
    /* toast potvrzení a chybové okno (snapshot_notification.cpp, test ho linkuje celý) */
    void imgui_snapshot_notification(void);
    void snapshot_notification_show(const char *message, bool is_error);
}

/** @brief Stub: okno zpráv (message_window.cpp); volá ho cmt_view.cpp při zprávě bez viditelného okna Virtual CMT. */
extern "C" void imgui_message_create(ui_message_type_t type, const char *message)
{
    (void)type;
    (void)message;
}

/** @brief Náhrada přepnutí fullscreenu ve video rozhraní po dobu testu. */
static void test_fullscreen_cb(void)
{
    s_fullscreen_calls++;
}

/** @brief Stav fullscreenu, který hlásí náhradní dotaz video rozhraní (test_is_fullscreen_cb()). */
static bool s_test_fullscreen = false;
/** @brief Počet volání náhradního srovnání poměru stran okna. */
static int s_fix_aspect_calls = 0;
/** @brief Argument posledního volání náhradního srovnání poměru stran ('W' nebo 'H'). */
static int s_fix_aspect_last = 0;
/** @brief Kolikrát náhrada hlavního okna otevřela klasické menu na žádost overlaye. */
static int s_classic_menu_opened = 0;

/** @brief Náhrada dotazu na fullscreen ve video rozhraní po dobu testu. */
static bool test_is_fullscreen_cb(void)
{
    return s_test_fullscreen;
}

/** @brief Náhrada srovnání poměru stran okna ve video rozhraní po dobu testu. */
static void test_fix_aspect_cb(char correction_by)
{
    s_fix_aspect_calls++;
    s_fix_aspect_last = correction_by;
}

/* ================================================================
 * SDL3 + GL + ImGui (vzor test_cmt_window.cpp)
 * ================================================================ */

static SDL_Window *s_win = NULL;
static SDL_GLContext s_gl = NULL;
static ImGuiContext *s_ctx = NULL;
static bool s_ui_ok = false;
static int s_canvas_w = 1280, s_canvas_h = 720;

typedef void (APIENTRY *fn_gen_fb)(GLsizei, GLuint *);
typedef void (APIENTRY *fn_bind_fb)(GLenum, GLuint);
typedef void (APIENTRY *fn_fb_tex2d)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *fn_check_fb)(GLenum);
static fn_bind_fb s_bind_fb = NULL;
static GLuint s_fbo = 0;
#define TEST_GL_FRAMEBUFFER 0x8D40
#define TEST_GL_COLOR_ATTACHMENT0 0x8CE0
#define TEST_GL_FRAMEBUFFER_COMPLETE 0x8CD5

/**
 * @brief Vytvoří skryté okno SDL s kontextem GL a kontext ImGui (klávesová navigace zapnutá jako v aplikaci).
 * @return true = připraveno; false = SDL nebo GL není k dispozici.
 */
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
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    /* jako myimgui.cpp: klávesová navigace ImGui je zapnutá trvale */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!ImGui_ImplSDL3_InitForOpenGL(s_win, s_gl)) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 130")) return false;
    ImGui::StyleColorsDark();
    /* písmo aplikace (DroidSans 28 px), je-li dostupné */
    if (MZTEST_FONT_DIR[0]) {
        char *path = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            static const ImWchar i18n_ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
            ImFont *font = io.Fonts->AddFontFromFileTTF(path, 28.0f, NULL, i18n_ranges);
            if (font) io.FontDefault = font;
        }
        g_free(path);
        /* písma overlaye jako myimgui_set_fonts(): samostatná, za základním */
        overlay_style_load_fonts(io.Fonts, MZTEST_FONT_DIR);
    }
    return true;
}

/** @brief Zruší kontext ImGui, kontext GL a okno SDL. */
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
 * Snímek aplikace s náhradou hlavního okna
 * ================================================================ */

/** @brief Výsledek ov_frame(): pravidlo klávesnice hlavního okna v posledním snímku. */
static bool s_main_keyboard = false;
/** @brief ov_frame() kreslí i okno nástroje (jiné okno, které může dostat fokus). */
static bool s_tool_window = false;
/** @brief Velikost hlavního viewportu vnucená ov_frame() (x <= 0 = podle okna SDL); kreslí se jen do paměti ImGui. */
static ImVec2 s_display(0.0f, 0.0f);
/** @brief ov_frame() drží otevřený modální dialog (náhrada souborového dialogu a dotazů emulátoru nad overlayem). */
static bool s_modal = false;
/** @brief ov_frame() kreslí i galerii widgetů přes celý viewport. */
static bool s_gallery = false;
/** @brief Kopie stavu, ze které ov_frame() kreslí overlay místo plnění z jádra; NULL = imgui_overlay_frame(). */
static const st_OVERLAY_VIEW *s_fixture = NULL;
/** @brief Dočasný adresář testu (media_test_dsk()); NULL = nevznikl. */
static char *s_tmpdir = NULL;
/** @brief Cesta testovacího DSK v s_tmpdir; NULL = nevznikl. */
static char *s_dsk = NULL;
/** @brief Připojení řadiče FDC0 před testem (vrací tearDown). */
static unsigned s_saved_fdc_connected = 0;
/** @brief Původní přepnutí fullscreenu video rozhraní (vrací tearDown). */
static void (*s_saved_fullscreen_cb)(void) = NULL;
/** @brief Původní dotaz na fullscreen video rozhraní (vrací tearDown). */
static bool (*s_saved_is_fullscreen_cb)(void) = NULL;
/** @brief Původní srovnání poměru stran okna video rozhraní (vrací tearDown). */
static void (*s_saved_fix_aspect_cb)(char) = NULL;
/** @brief Nastavení zobrazení před testem (vrací tearDown). */
static st_DISPLAY s_saved_display;
/** @brief Development Mode před testem (vrací tearDown). */
static bool s_saved_development_mode = false;

static void gallery_draw(ImVec2 pos, ImVec2 size);
static void o6_drain(void);

/**
 * @brief Jeden snímek: náhrada hlavního okna emulátoru, volitelně okno nástroje, overlay.
 *
 * Pořadí a pravidla jako ShowFullScreenImageEmulatorWindow() a
 * imgui_main_window() v main_window.cpp; výsledek pravidla klávesnice
 * uloží do s_main_keyboard.
 */
static void ov_frame(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (s_display.x > 0.0f) ImGui::GetIO().DisplaySize = s_display;
    ImGui::NewFrame();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    if (imgui_overlay_take_main_focus_request() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::SetNextWindowFocus();
    ImGui::Begin("##MainEmulatorWindow", NULL, MAIN_WINDOW_FLAGS);
    const bool overlay_open = imgui_overlay_is_open();
    s_main_keyboard = main_window_keyboard_enabled(false, imgui_overlay_blocks_keyboard());
    /* jako ShowFullScreenImageEmulatorWindow(): uvolnění zadrženého joysticku po zavření overlaye */
    (void)imgui_overlay_blocks_joystick();
    /* jako ShowFullScreenImageEmulatorWindow(): Ctrl+Tab přepíná okna ImGui jen mimo obraz a overlay */
    main_window_nav_windowing_set(main_window_nav_windowing_allowed(false, imgui_overlay_blocks_keyboard()));
    if (overlay_open && main_window_keyboard_focus(false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        imgui_overlay_request_focus();
    /* jako ShowFullScreenImageEmulatorWindow(): žádost položky "Overlay Menu..." klasického menu z minulého snímku */
    if (imgui_overlay_take_open_request()) imgui_overlay_open();
    /* žádost overlaye o klasické menu, jako v ShowFullScreenImageEmulatorWindow() (popup sám test nekreslí) */
    if (imgui_overlay_take_classic_menu_request()) s_classic_menu_opened++;
    /* jako ShowFullScreenImageEmulatorWindow(): levý Alt + levé tlačítko nad obrazem */
    if (!imgui_overlay_is_open() && ImGui::IsWindowHovered() && main_window_overlay_open_click()) imgui_overlay_open();
    if (s_main_keyboard) {
        /* jako ShowFullScreenImageEmulatorWindow(): F10 bez modifikátorů jen při fokusu obrazu */
        if (main_window_overlay_open_key()) imgui_overlay_open();
    }
    ImGui::End();
    if (s_tool_window) {
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_Always);
        ImGui::Begin("Tool###ov_test_tool");
        ImGui::Button("Tool button");
        ImGui::End();
    }
    if (s_gallery) gallery_draw(vp->Pos, vp->Size);
    if (s_fixture) imgui_overlay_draw(s_fixture, vp->Pos, vp->Size);
    else imgui_overlay_frame();
    /* toast potvrzení, po overlayi jako v imgui_main_window() */
    imgui_snapshot_notification();
    /* modální dialog nad overlayem (souborový dialog a dotazy emulátoru jsou modální popupy ImGui) */
    if (s_modal && !ImGui::IsPopupOpen("Test modal###ov_test_modal")) ImGui::OpenPopup("Test modal###ov_test_modal");
    if (ImGui::BeginPopupModal("Test modal###ov_test_modal", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Button("Modal button");
        if (!s_modal) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Render();
}

/** @brief Stiskne a pustí klávesu (dva snímky ov_frame()). */
static void ov_key(ImGuiKey key)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    ov_frame();
    io.AddKeyEvent(key, false);
    ov_frame();
}

/**
 * @brief Stiskne a pustí klávesu s drženým Ctrl a volitelně Shift.
 * @param key Klávesa. @param shift true = navíc Shift.
 */
static void ov_ctrl_key(ImGuiKey key, bool shift)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    ov_frame();
    io.AddKeyEvent(key, true);
    ov_frame();
    io.AddKeyEvent(key, false);
    ov_frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    ov_frame();
}

/** @brief Klikne levým tlačítkem na bod (čtyři snímky ov_frame()). */
static void ov_click(ImVec2 p)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    ov_frame();
    io.AddMouseButtonEvent(0, true);
    ov_frame();
    io.AddMouseButtonEvent(0, false);
    ov_frame();
    ov_frame();
}

/** @brief Klikne do středu prvku overlaye podle jména (imgui_overlay_test_item_rect()). */
static void ov_click_item(const char *name)
{
    ImVec2 mn, mx;
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_test_item_rect(name, &mn, &mx), name);
    ov_click(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));
}

/** @brief Má fokus okno overlaye (nebo jeho potomek)? */
static bool ov_overlay_focused(void)
{
    ImGuiWindow *w = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    ImGuiWindow *nav = GImGui->NavWindow;
    return w && nav && nav->RootWindow == w;
}

/** @brief Provede čekající vstupy z UI jako emu vlákno na hranici snímku (zapíše matici stroje). */
static void ov_drain_inputs(void)
{
    replay_on_frame_boundary();
}

/** @brief Je matice klávesnice stroje klidová (žádná klávesa)? */
static bool ov_matrix_idle(void)
{
    for (int i = 0; i < 10; i++)
        if (g_pio8255.keyboard_matrix[i] != 0xff) return false;
    return true;
}

/** @brief Stav emulátoru (pauza, MAX SPEED) před testem (vrací tearDown). */
static st_EMULATOR s_saved_emulator;
/** @brief Nastavení vlastní rychlosti před testem (vrací tearDown). */
static st_CUSTOMSPEED s_saved_customspeed;

void setUp(void)
{
    s_saved_emulator = g_emulator;
    s_saved_customspeed = g_customspeed;
    s_saved_display = g_display;
    s_saved_development_mode = EMULATOR_TEST_DEVELOPMENT_MODE;
    g_emulator.paused = false;
    g_emulator.max_speed = false;
    g_emulator.max_speed_boost = false;
    g_customspeed.speed_in_percentage = 100;
    g_customspeed.speed_in_percentage_requested = 100;
    g_customspeed.previous_speed_in_percentage = 100;
    s_pause_toggle_calls = 0;
    s_snapshot_save_calls = s_snapshot_load_calls = 0;
    s_snapshot_load_overlay_open = false;
    s_test_fullscreen = false;
    s_fix_aspect_calls = s_fix_aspect_last = 0;
    s_classic_menu_opened = 0;
    imgui_overlay_take_classic_menu_request();
    s_gui.showSnapshotSetupWindow = false;
    g_replay_mode = REPLAY_MODE_OFF;
    s_pause_calls = 0;
    s_run_calls = 0;
    s_pause_fail = false;
    s_run_fail = 0;
    s_tool_window = false;
    s_gallery = false;
    s_fixture = NULL;
    s_display = ImVec2(0.0f, 0.0f);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    overlay_test_set_stretch(false);
    s_saved_fdc_connected = g_fdc[FDC0].connected;
    s_saved_fullscreen_cb = g_iface_video_callbacks ? g_iface_video_callbacks->switch_fullscreen : NULL;
    s_saved_is_fullscreen_cb = g_iface_video_callbacks ? g_iface_video_callbacks->is_fullscreen : NULL;
    s_saved_fix_aspect_cb = g_iface_video_callbacks ? g_iface_video_callbacks->fix_window_aspect_ratio : NULL;
    if (g_iface_video_callbacks) {
        g_iface_video_callbacks->switch_fullscreen = test_fullscreen_cb;
        g_iface_video_callbacks->is_fullscreen = test_is_fullscreen_cb;
        g_iface_video_callbacks->fix_window_aspect_ratio = test_fix_aspect_cb;
    }
    device_request_clear();
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    overlay_actions_test_take_last();
    s_quicksave_calls = s_quickload_calls = s_screenshot_calls = s_fullscreen_calls = s_tape_action_calls = 0;
    s_tape_action_last = IMGUI_CMT_TAPE_ACTION_NONE;
    s_tape_open_calls = 0;
    s_tape_open_path[0] = '\0';
    s_tape_open_result = true;
    s_gui.showVirtualCmtWindow = false;
    s_modal = false;
    /* historie médií: každý test začíná s prázdnou (mount v jiném testu do ní zapisuje) */
    recent_media_reset();
    overlay_sheet_test_take_missing_reports();
    o6_drain();
    s_video_start_pending = false;
}

void tearDown(void)
{
    if (device_state_lock_held()) device_state_unlock();
    /* žádné čekající opakování rozběhnutí do dalšího testu */
    s_pause_fail = false;
    s_run_fail = 0;
    imgui_overlay_close();
    /* zadržení joysticku nepřenášet do dalšího testu (hostitelský joystick testů je v klidu) */
    (void)imgui_overlay_blocks_joystick();
    g_emulator.paused = false;
    for (int i = 0; i < 2; i++) imgui_overlay_frame();
    s_fixture = NULL;
    s_modal = false;
    overlay_test_set_stretch(false);
    recent_media_reset();
    for (unsigned d = 0; d < FDC_NUM_DRIVES; d++)
        if (fdc_test_drive_id_mounted(&g_fdc[FDC0], d)) fdc_umount(&g_fdc[FDC0], d);
    g_fdc[FDC0].connected = s_saved_fdc_connected;
    if (g_iface_video_callbacks) {
        g_iface_video_callbacks->switch_fullscreen = s_saved_fullscreen_cb;
        g_iface_video_callbacks->is_fullscreen = s_saved_is_fullscreen_cb;
        g_iface_video_callbacks->fix_window_aspect_ratio = s_saved_fix_aspect_cb;
    }
    g_display = s_saved_display;
    emulator_set_development_mode(s_saved_development_mode);
    g_replay_mode = REPLAY_MODE_OFF;
    device_request_clear();
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    if (s_ui_ok) {
        /* žádná držená klávesa ani tlačítko do dalšího testu */
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        io.AddMouseButtonEvent(0, false);
        io.AddMousePosEvent(-1.0f, -1.0f);
        s_tool_window = false;
        s_gallery = false;
        s_display = ImVec2(0.0f, 0.0f);
        ov_frame();
        ov_frame();
    }
    main_window_keyboard_release();
    ov_drain_inputs();
    g_emulator = s_saved_emulator;
    g_customspeed = s_saved_customspeed;
}

/* ================================================================
 * Testy bez kreslení
 * ================================================================ */

/** Kopie stavu: plnění z jádra (rychlost, pauza, replay) a testovací stav. */
void test_view_fill_and_fixture(void)
{
    st_OVERLAY_VIEW v;
    memset(&v, 0xAA, sizeof(v));
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("MZ-800", v.platform_text);
    TEST_ASSERT_EQUAL_UINT(100, v.speed_percent);
    TEST_ASSERT_EQUAL_INT(OVERLAY_SPEED_NORMAL, v.speed_mode);
    TEST_ASSERT_FALSE(v.paused);
    TEST_ASSERT_FALSE(v.replay_active);

    /* nastavená (požadovaná) rychlost; uplatněnou mění emu vlákno až na hranici snímku */
    g_customspeed.speed_in_percentage_requested = 250;
    g_emulator.paused = true;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_UINT(250, v.speed_percent);
    TEST_ASSERT_EQUAL_INT(OVERLAY_SPEED_CUSTOM, v.speed_mode);
    TEST_ASSERT_TRUE(v.paused);

    g_emulator.max_speed = true;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(OVERLAY_SPEED_MAX, v.speed_mode);

    memset(&v, 0xAA, sizeof(v));
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    TEST_ASSERT_EQUAL_STRING("MZ-800", v.platform_text);
    TEST_ASSERT_EQUAL_UINT(100, v.speed_percent);
    TEST_ASSERT_EQUAL_INT(OVERLAY_SPEED_NORMAL, v.speed_mode);
    TEST_ASSERT_TRUE(v.paused);
    TEST_ASSERT_FALSE(v.replay_active);

    /* NULL nic neudělá */
    overlay_view_fill(NULL);
    overlay_view_make_fixture(NULL, OVERLAY_FIXTURE_DEFAULT);
    overlay_view_reset(NULL);
}

/** Otevření, zavření a přepnutí mění imgui_overlay_is_open(); volba tabu. */
void test_open_close_and_tab_api(void)
{
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    imgui_overlay_open();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    imgui_overlay_close();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_close();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_toggle();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    imgui_overlay_toggle();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    imgui_overlay_set_tab(OVERLAY_TAB_SOUND);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    imgui_overlay_set_tab(OVERLAY_TAB_COUNT);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    imgui_overlay_set_tab((en_OVERLAY_TAB)-1);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    /* zavření a otevření tab nemění */
    imgui_overlay_open();
    imgui_overlay_close();
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
}

/**
 * Pauza (Review Focus 2): běžící emulace po zavření běží, stojící stojí;
 * imgui_overlay_note_pause_changed() zapamatovaný stav přepíše.
 */
void test_pause_restore(void)
{
    /* běží -> otevřít = stojí -> zavřít = běží */
    g_emulator.paused = false;
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);

    /* stojí -> otevřít -> zavřít = stojí, bez zásahu do emulace */
    s_pause_calls = s_run_calls = 0;
    g_emulator.paused = true;
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_pause_calls);
    imgui_overlay_close();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* stojí -> otevřít -> akce Resume (rozběhne a oznámí) -> zavřít = běží */
    s_pause_calls = s_run_calls = 0;
    g_emulator.paused = true;
    imgui_overlay_open();
    dbg_ui_run();
    imgui_overlay_note_pause_changed(false);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);

    /* běží -> otevřít -> akce, která nechá emulaci stát (oznámí) -> zavřít = stojí */
    s_pause_calls = s_run_calls = 0;
    g_emulator.paused = false;
    imgui_overlay_open();
    imgui_overlay_note_pause_changed(true);
    imgui_overlay_close();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* oznámení při zavřeném overlayi nic nezmění: další otevření si stav zjistí samo */
    g_emulator.paused = false;
    imgui_overlay_note_pause_changed(true);
    imgui_overlay_open();
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);

    /* review O5: zavření ve snímku, kdy čeká uložení snapshotu vyžádané z overlaye, emulaci nerozbíhá
     * (uložení by ji zase zastavilo a nechalo stát); rozběhnutí se předá dokončení uložení */
    const bool requested[][2] = {{true, false}, {false, true}};
    for (const bool *rq : requested) {
        s_pause_calls = s_run_calls = s_run_after_request_calls = 0;
        g_emulator.paused = false;
        imgui_overlay_open();
        TEST_ASSERT_TRUE(g_emulator.paused);
        s_stub_quicksave_requested = rq[0];
        s_stub_save_requested = rq[1];
        imgui_overlay_close();
        s_stub_quicksave_requested = s_stub_save_requested = false;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_run_calls, "closing does not run the emulation under a pending snapshot save");
        TEST_ASSERT_TRUE(g_emulator.paused);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_after_request_calls, "the save is told to run the emulation when it ends");
    }
    /* emulace, která stála už před otevřením, se uložení nepředává */
    s_run_after_request_calls = 0;
    g_emulator.paused = true;
    imgui_overlay_open();
    s_stub_quicksave_requested = true;
    imgui_overlay_close();
    s_stub_quicksave_requested = false;
    TEST_ASSERT_EQUAL_INT(0, s_run_after_request_calls);
    TEST_ASSERT_TRUE(g_emulator.paused);
    g_emulator.paused = false;
}

/* ================================================================
 * Testy s kreslením
 * ================================================================ */

/** Tři snímky bez pádu: okno se stabilním ID přes celý viewport, vyvážené zásobníky ImGui, prvky lišty. */
void test_draw_three_frames(void)
{
    ov_frame();
    TEST_ASSERT_NULL_MESSAGE(ImGui::FindWindowByName(OVERLAY_WINDOW_ID), "a closed overlay draws nothing");

    imgui_overlay_open();
    ImGuiContext &g = *GImGui;
    for (int i = 0; i < 3; i++) {
        ov_frame();
        TEST_ASSERT_EQUAL_INT(0, g.CurrentWindowStack.Size);
        TEST_ASSERT_EQUAL_INT(0, g.StyleVarStack.Size);
        TEST_ASSERT_EQUAL_INT(0, g.ColorStack.Size);
        TEST_ASSERT_EQUAL_INT(0, g.FontStack.Size);
    }
    ImGuiWindow *w = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(w->WasActive);
    TEST_ASSERT_FALSE_MESSAGE((w->Flags & ImGuiWindowFlags_Popup) != 0, "the overlay must not be a popup");
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    TEST_ASSERT_FLOAT_WITHIN(0.5f, vp->Pos.x, w->Pos.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, vp->Pos.y, w->Pos.y);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, vp->Size.x, w->Size.x);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, vp->Size.y, w->Size.y);
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the overlay takes the focus when opened");

    /* prvky lišty leží uvnitř okna a taby jdou zleva doprava */
    ImVec2 mn, mx, prev_max(vp->Pos.x, 0.0f);
    for (int i = 0; i < (int)OVERLAY_TAB_COUNT; i++) {
        char name[8];
        snprintf(name, sizeof(name), "tab%d", i);
        TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_test_item_rect(name, &mn, &mx), name);
        TEST_ASSERT_TRUE(mn.x >= prev_max.x && mx.x > mn.x);
        TEST_ASSERT_TRUE(mn.y >= vp->Pos.y && mx.y <= vp->Pos.y + vp->Size.y);
        prev_max = mx;
    }
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("close", &mn, &mx));
    TEST_ASSERT_TRUE_MESSAGE(mn.x >= prev_max.x, "Close does not overlap the tabs at 1280 x 720");
    TEST_ASSERT_TRUE(mx.x <= vp->Pos.x + vp->Size.x);
    TEST_ASSERT_FALSE(imgui_overlay_test_item_rect("tab6", NULL, NULL));
    TEST_ASSERT_FALSE(imgui_overlay_test_item_rect("nothing", NULL, NULL));

    /* modály aplikace čekají na zavření popupů: overlay žádný popup neotevírá */
    TEST_ASSERT_FALSE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

    imgui_overlay_close();
    ov_frame();
    ov_frame();
    w = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    TEST_ASSERT_TRUE(w == NULL || !w->WasActive);

    /* čisté kreslení z testovacího stavu bez otevření; NULL a nulová plocha nic nekreslí */
    st_OVERLAY_VIEW v;
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    imgui_overlay_draw(NULL, vp->Pos, vp->Size);
    TEST_ASSERT_FALSE(imgui_overlay_test_item_rect("close", NULL, NULL));
    imgui_overlay_draw(&v, vp->Pos, ImVec2(0.0f, 0.0f));
    TEST_ASSERT_FALSE(imgui_overlay_test_item_rect("close", NULL, NULL));
    imgui_overlay_draw(&v, vp->Pos, vp->Size);
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("close", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, g.CurrentWindowStack.Size); /* jen výchozí okno ImGui */
    ImGui::Render();
}

/** Klávesy: F10 otevře a nezavře tímtéž stiskem, F10 a Esc zavřou, číslice a Ctrl+Tab přepínají taby. */
void test_keys(void)
{
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "the picture has the focus");

    /* F10 při fokusu obrazu otevře; týž stisk overlay nezavře */
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_F10, true);
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "F10 opens the overlay");
    ov_frame();
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "a held F10 does not close it again");
    io.AddKeyEvent(ImGuiKey_F10, false);
    ov_frame();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_TRUE(ov_overlay_focused());

    /* číslice 1 až 6 */
    const en_OVERLAY_TAB order[] = {OVERLAY_TAB_CAPTURE, OVERLAY_TAB_MACHINE, OVERLAY_TAB_INPUT, OVERLAY_TAB_DISPLAY, OVERLAY_TAB_SOUND, OVERLAY_TAB_MEDIA};
    for (en_OVERLAY_TAB t : order) {
        ov_key((ImGuiKey)(ImGuiKey_1 + (int)t));
        TEST_ASSERT_EQUAL_INT(t, imgui_overlay_get_tab());
        TEST_ASSERT_TRUE(imgui_overlay_is_open());
    }
    /* jiné číslice a písmena nic */
    ov_key(ImGuiKey_7);
    ov_key(ImGuiKey_0);
    ov_key(ImGuiKey_A);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());

    /* Ctrl+Tab další, Ctrl+Shift+Tab předchozí, s přetočením; fokus zůstává v overlayi */
    ov_ctrl_key(ImGuiKey_Tab, false);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    ov_ctrl_key(ImGuiKey_Tab, true);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    ov_ctrl_key(ImGuiKey_Tab, true);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_CAPTURE, imgui_overlay_get_tab());
    ov_ctrl_key(ImGuiKey_Tab, false);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "Ctrl+Tab does not move the focus to another window");

    /* F10 zavře */
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "F10 closes the overlay");
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "the picture has the focus again");

    /* znovu otevřít, Esc zavře */
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "Esc closes the overlay");

    /* po zavření platí pravidlo hlavního okna: fokus má obraz, přepínání oken ImGui klávesami Ctrl+Tab je vypnuté */
    ov_frame();
    TEST_ASSERT_TRUE(s_main_keyboard);
    TEST_ASSERT_TRUE(GImGui->ConfigNavWindowingKeyNext == 0);
    TEST_ASSERT_TRUE(GImGui->ConfigNavWindowingKeyPrev == 0);
    /* fokus v okně nástroje: přepínání je zpět s výchozími zkratkami ImGui */
    s_tool_window = true;
    for (int i = 0; i < 3; i++) ov_frame();
    ov_click(ImVec2(100.0f, 100.0f));
    TEST_ASSERT_FALSE(s_main_keyboard);
    TEST_ASSERT_EQUAL_INT(ImGuiMod_Ctrl | ImGuiKey_Tab, GImGui->ConfigNavWindowingKeyNext);
    TEST_ASSERT_EQUAL_INT(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, GImGui->ConfigNavWindowingKeyPrev);
}

/** Myš: klik na tab ho zvolí, klik na Close zavře. */
void test_mouse(void)
{
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    ov_click_item("tab3");
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    ov_click_item("tab1");
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_click_item("close");
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "Close closes the overlay");
}

/**
 * @brief Stiskne a pustí F10 s drženým modifikátorem a vrátí, zda je overlay otevřený.
 * @param mod Modifikátor ImGui (ImGuiMod_Shift, ImGuiMod_Ctrl, ImGuiMod_Alt, ImGuiMod_Super).
 * @return Stav imgui_overlay_is_open() po puštění klávesy i modifikátoru.
 */
static bool ov_f10_with_mod(ImGuiKey mod)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(mod, true);
    ov_frame();
    io.AddKeyEvent(ImGuiKey_F10, true);
    ov_frame();
    io.AddKeyEvent(ImGuiKey_F10, false);
    ov_frame();
    io.AddKeyEvent(mod, false);
    ov_frame();
    return imgui_overlay_is_open();
}

/**
 * @brief Klikne levým tlačítkem do obrazu s drženým Alt a vrátí, zda je overlay otevřený.
 *
 * Posílá modifikátor Alt i klávesu konkrétní strany, jako backend SDL.
 *
 * @param alt_key ImGuiKey_LeftAlt nebo ImGuiKey_RightAlt.
 * @return Stav imgui_overlay_is_open() po puštění tlačítka i klávesy.
 */
static bool ov_alt_click(ImGuiKey alt_key)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(900.0f, 500.0f);
    io.AddKeyEvent(ImGuiMod_Alt, true);
    io.AddKeyEvent(alt_key, true);
    ov_frame();
    io.AddMouseButtonEvent(0, true);
    ov_frame();
    io.AddMouseButtonEvent(0, false);
    ov_frame();
    io.AddKeyEvent(alt_key, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    ov_frame();
    return imgui_overlay_is_open();
}

/**
 * Pravidla otevření z hlavního okna (main_window_focus.h): F10 otevírá jen
 * bez Shift, Ctrl, Alt a Super a jen při fokusu obrazu (ne při fokusu okna
 * nástroje, např. debuggeru); myší otevírá jen levý Alt + levé tlačítko,
 * pravý Alt (AltGr) ne.
 */
void test_open_rules(void)
{
    s_tool_window = true;
    for (int i = 0; i < 3; i++) ov_frame();
    ov_click(ImVec2(900.0f, 500.0f));
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "the picture has the focus");
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* F10 s modifikátorem neotevírá */
    TEST_ASSERT_FALSE_MESSAGE(ov_f10_with_mod(ImGuiMod_Shift), "Shift+F10 does not open the overlay");
    TEST_ASSERT_FALSE_MESSAGE(ov_f10_with_mod(ImGuiMod_Ctrl), "Ctrl+F10 does not open the overlay");
    TEST_ASSERT_FALSE_MESSAGE(ov_f10_with_mod(ImGuiMod_Alt), "Alt+F10 does not open the overlay");
    TEST_ASSERT_FALSE_MESSAGE(ov_f10_with_mod(ImGuiMod_Super), "Super+F10 does not open the overlay");

    /* F10 při fokusu okna nástroje neotevírá */
    ov_click(ImVec2(100.0f, 100.0f));
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "the tool window has the focus");
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "F10 in a tool window does not open the overlay");

    /* zpět do obrazu: samotné F10 otevírá */
    ov_click(ImVec2(900.0f, 500.0f));
    TEST_ASSERT_TRUE(s_main_keyboard);
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "plain F10 opens the overlay");
    imgui_overlay_close();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* myš: pravý Alt (AltGr) neotevírá, levý Alt ano */
    TEST_ASSERT_FALSE_MESSAGE(ov_alt_click(ImGuiKey_RightAlt), "right Alt + click does not open the overlay");
    TEST_ASSERT_TRUE_MESSAGE(ov_alt_click(ImGuiKey_LeftAlt), "left Alt + click opens the overlay");
    imgui_overlay_close();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
}

/**
 * Klávesnice emulovaného stroje (Review Focus 1).
 *
 * Při zavřeném overlayi pravidlo hlavního okna klávesy pouští. Otevření
 * pošle klidovou matici (stisknutá klávesa stroje se uvolní), při otevřeném
 * overlayi pravidlo nepustí nic (ani F10, Esc, číslice, ani když je
 * otevřené jiné okno), po zavření se pravidlo vrátí a matice je klidová.
 * Samotnou obsluhu událostí SDL (iface_sdl3_events_cb()) a sken
 * iface_keyboard_full_scan() test nevolá (potřebují okno sdlapp).
 */
void test_emulated_keyboard(void)
{
    s_tool_window = true;
    for (int i = 0; i < 3; i++) ov_frame();
    /* klik do obrazu mimo okno nástroje */
    ov_click(ImVec2(900.0f, 500.0f));
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "the emulated keyboard gets keys while the overlay is closed");

    /* stroj drží klávesu (sken hostitele před otevřením) */
    uint8_t pressed[10];
    memset(pressed, 0xff, sizeof(pressed));
    pressed[3] = 0xf7;
    replay_host_keyboard_submit(pressed, -1, 0xff);
    ov_drain_inputs();
    TEST_ASSERT_EQUAL_HEX8(0xf7, g_pio8255.keyboard_matrix[3]);

    /* otevření: klidová matice */
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_F10, true);
    ov_frame();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_drain_inputs();
    TEST_ASSERT_TRUE_MESSAGE(ov_matrix_idle(), "opening the overlay releases the keys of the machine");
    ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "nothing goes to the machine in the first frame after opening");
    io.AddKeyEvent(ImGuiKey_F10, false);
    ov_frame();
    TEST_ASSERT_FALSE(s_main_keyboard);

    /* klávesy při otevřeném overlayi: nic do stroje */
    const ImGuiKey keys[] = {ImGuiKey_A, ImGuiKey_Space, ImGuiKey_Enter, ImGuiKey_3, ImGuiKey_UpArrow, ImGuiKey_Tab, ImGuiKey_F5};
    for (ImGuiKey k : keys) {
        io.AddKeyEvent(k, true);
        ov_frame();
        TEST_ASSERT_FALSE(s_main_keyboard);
        io.AddKeyEvent(k, false);
        ov_frame();
        TEST_ASSERT_FALSE(s_main_keyboard);
    }
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_drain_inputs();
    TEST_ASSERT_TRUE(ov_matrix_idle());

    /* fokus vrácený do obrazu (jako po zavření dialogu): stroj dál nic, overlay si fokus vezme zpět */
    ImGui::FocusWindow(ImGui::FindWindowByName("##MainEmulatorWindow"));
    ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "nothing goes to the machine even when the picture gets the focus back");
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the overlay takes the focus back");

    /* zavření klávesou Esc (i klávesa stroje): dokud je držená, do stroje nejde nic */
    io.AddKeyEvent(ImGuiKey_Escape, true);
    ov_frame();
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "the closing key press does not reach the machine in the closing frame");
    for (int i = 0; i < 4; i++) {
        ov_frame();
        TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "a held closing key does not reach the machine after closing");
    }
    /* po uvolnění klávesy zase jdou: fokus má obraz, ne okno nástroje */
    io.AddKeyEvent(ImGuiKey_Escape, false);
    ov_frame();
    ov_frame();
    ov_drain_inputs();
    TEST_ASSERT_TRUE_MESSAGE(ov_matrix_idle(), "no key stays pressed after closing");
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "the emulated keyboard gets keys again after closing");
}

/**
 * @brief Pošle matici stroje sken klávesnice hostitele se stisknutými klávesami Ctrl a Tab.
 *
 * Stejná cesta jako v aplikaci při fokusu obrazu: iface_keyboard_full_scan()
 * převede stav kláves hostitele na matici a předá ji záznamu vstupů; do
 * matice stroje (g_pio8255.keyboard_matrix) ji zapíše ov_drain_inputs().
 *
 * @param down true = Ctrl a Tab stisknuté, false = žádná klávesa.
 */
static void ov_host_scan_ctrl_tab(bool down)
{
    bool state[SDL_SCANCODE_COUNT];
    memset(state, 0, sizeof(state));
    state[SDL_SCANCODE_LCTRL] = down;
    state[SDL_SCANCODE_TAB] = down;
    iface_keyboard_full_scan(state, down ? SDL_KMOD_LCTRL : SDL_KMOD_NONE, SDL_SCANCODE_COUNT);
    ov_drain_inputs();
}

/**
 * Trvale zapnutý ImGuiConfigFlags_NavEnableKeyboard nebere klávesy
 * emulovanému stroji při zavřeném overlayi.
 *
 * Stroj dostává klávesy podle pravidla main_window_keyboard_enabled()
 * (fokus hlavního okna), ne podle io.WantCaptureKeyboard. Test s oknem
 * nástroje vedle obrazu tiskne šipky, Tab, Enter, mezerník a Esc (klávesy
 * navigace ImGui a zároveň klávesy stroje) a ověřuje, že fokus hlavnímu
 * oknu zůstává. Ctrl+Tab a Ctrl+Shift+Tab (přepínač oken ImGui) při fokusu
 * obrazu fokus nechávají v obraze v každém snímku stisku a sken hostitele
 * s Ctrl a Tab dojde do matice stroje. Při fokusu okna nástroje přepínač
 * funguje: Ctrl+Tab vrátí fokus do obrazu a odtud už zpět nevede.
 * Hodnotu io.WantCaptureKeyboard test jen vypisuje.
 */
void test_nav_keyboard_probe(void)
{
    ImGuiIO &io = ImGui::GetIO();
    TEST_ASSERT_TRUE((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
    s_tool_window = true;
    for (int i = 0; i < 3; i++) ov_frame();
    ov_click(ImVec2(900.0f, 500.0f));
    TEST_ASSERT_TRUE(s_main_keyboard);
    printf("nav-probe: picture focused, WantCaptureKeyboard=%d NavActive=%d\n", (int)io.WantCaptureKeyboard, (int)io.NavActive);

    const ImGuiKey keys[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                             ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_Space, ImGuiKey_Escape, ImGuiKey_A};
    bool want_capture_any = false;
    for (ImGuiKey k : keys) {
        io.AddKeyEvent(k, true);
        ov_frame();
        want_capture_any = want_capture_any || io.WantCaptureKeyboard;
        TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "ImGui navigation keys keep the focus in the picture");
        io.AddKeyEvent(k, false);
        ov_frame();
        TEST_ASSERT_TRUE(s_main_keyboard);
    }
    printf("nav-probe: navigation keys pressed, WantCaptureKeyboard seen=%d, picture keeps the focus=1\n", (int)want_capture_any);

    /* Ctrl+Tab a Ctrl+Shift+Tab při fokusu obrazu: přepínač oken ImGui je vypnutý, fokus zůstává v každém snímku */
    for (int shift = 0; shift < 2; shift++) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        ov_frame();
        TEST_ASSERT_TRUE(s_main_keyboard);
        io.AddKeyEvent(ImGuiKey_Tab, true);
        ov_frame();
        TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "Ctrl+Tab keeps the focus in the picture while pressed");
        TEST_ASSERT_NULL_MESSAGE(GImGui->NavWindowingTarget, "the ImGui window switcher does not start over the picture");
        if (!shift) {
            /* obraz má klávesnici: sken hostitele s Ctrl a Tab dojde do matice stroje */
            ov_host_scan_ctrl_tab(true);
            TEST_ASSERT_FALSE_MESSAGE(ov_matrix_idle(), "Ctrl and Tab reach the keyboard matrix of the machine");
            ov_host_scan_ctrl_tab(false);
            TEST_ASSERT_TRUE(ov_matrix_idle());
        }
        io.AddKeyEvent(ImGuiKey_Tab, false);
        ov_frame();
        TEST_ASSERT_TRUE(s_main_keyboard);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        ov_frame();
        ov_frame();
        TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "Ctrl+Tab keeps the focus in the picture after release");
        TEST_ASSERT_NOT_NULL(GImGui->NavWindow);
        TEST_ASSERT_EQUAL_STRING("##MainEmulatorWindow", GImGui->NavWindow->Name);
    }
    printf("nav-probe: after Ctrl+Tab with a tool window open, picture keeps the focus=%d (NavWindow=%s)\n", (int)s_main_keyboard,
           GImGui->NavWindow ? GImGui->NavWindow->Name : "(none)");

    /* fokus v okně nástroje: přepínač oken funguje a Ctrl+Tab vrátí fokus do obrazu */
    ov_click(ImVec2(100.0f, 100.0f));
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "the tool window has the focus");
    ov_ctrl_key(ImGuiKey_Tab, false);
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "Ctrl+Tab in a tool window returns the focus to the picture");
    /* a z obrazu už zpět nevede */
    ov_ctrl_key(ImGuiKey_Tab, false);
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(s_main_keyboard, "Ctrl+Tab over the picture does not go back to the tool window");
}

/* ================================================================
 * Vizuální systém: jednotka, kontrasty, písma
 * ================================================================ */

/** Jednotka u = min(šířka / 1280, výška / 720), nejméně 0,85; písmo nikdy pod 12 skutečných pixelů. */
void test_style_unit(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, overlay_unit_for(ImVec2(1280.0f, 720.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, overlay_unit_for(ImVec2(2560.0f, 1440.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, overlay_unit_for(ImVec2(1920.0f, 1080.0f)));
    /* rozhoduje menší z obou poměrů */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, overlay_unit_for(ImVec2(1280.0f, 1440.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, overlay_unit_for(ImVec2(2560.0f, 720.0f)));
    /* malé okno: spodní mez */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, OVL_UNIT_MIN, overlay_unit_for(ImVec2(640.0f, 480.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, OVL_UNIT_MIN, overlay_unit_for(ImVec2(320.0f, 200.0f)));

    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {2560.0f, 1440.0f}};
    const float fonts[] = {OVL_FONT_TAB, OVL_FONT_CARD_TITLE, OVL_FONT_VALUE, OVL_FONT_BODY, OVL_FONT_HINT};
    for (const float *sz : sizes) {
        overlay_style_set_viewport(ImVec2(sz[0], sz[1]));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, overlay_unit_for(ImVec2(sz[0], sz[1])), overlay_unit());
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f * overlay_unit(), overlay_px(8.0f));
        TEST_ASSERT_TRUE(overlay_border_px() >= 1.0f);
        for (float f : fonts) TEST_ASSERT_TRUE_MESSAGE(overlay_font_px(f) >= OVL_FONT_MIN_PX, "no text below 12 real pixels");
    }
    overlay_style_set_viewport(ImVec2(640.0f, 480.0f));
    TEST_ASSERT_TRUE(overlay_is_narrow());
    overlay_style_set_viewport(ImVec2(1280.0f, 720.0f));
    TEST_ASSERT_FALSE(overlay_is_narrow());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 17.0f, overlay_font_px(OVL_FONT_BODY)); /* 14 em = 17 px výšky řádku písma Inter */
    overlay_style_set_viewport(ImVec2(2560.0f, 1440.0f));
    TEST_ASSERT_FALSE(overlay_is_narrow());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 34.0f, overlay_font_px(OVL_FONT_BODY));

    TEST_ASSERT_EQUAL_HEX32(IM_COL32(0x11, 0x11, 0x13, 0x80), overlay_color_alpha(OVL_COL_PANEL, 0.5f));
    overlay_style_set_viewport(ImVec2(1280.0f, 720.0f));
}

/** @brief Složka sRGB (0 až 255) převedená na lineární hodnotu podle WCAG 2. */
static double wcag_linear(int c)
{
    const double v = c / 255.0;
    return (v <= 0.04045) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

/** @brief Relativní jas barvy podle WCAG 2. */
static double wcag_luminance(ImU32 col)
{
    return 0.2126 * wcag_linear((int)((col >> IM_COL32_R_SHIFT) & 0xFF)) + 0.7152 * wcag_linear((int)((col >> IM_COL32_G_SHIFT) & 0xFF))
           + 0.0722 * wcag_linear((int)((col >> IM_COL32_B_SHIFT) & 0xFF));
}

/** @brief Kontrastní poměr dvou neprůhledných barev podle WCAG 2 (1 až 21). */
static double wcag_contrast(ImU32 a, ImU32 b)
{
    const double la = wcag_luminance(a), lb = wcag_luminance(b);
    return (la > lb) ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/**
 * @brief Barva vrstvy s danou neprůhledností nad neprůhledným podkladem (míchání složek sRGB jako v kreslení ImGui).
 * @param top Barva vrstvy. @param bottom Barva podkladu. @param alpha Neprůhlednost vrstvy 0 až 1.
 */
static ImU32 blend_over(ImU32 top, ImU32 bottom, double alpha)
{
    int ch[3];
    const int shifts[3] = {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT};
    for (int i = 0; i < 3; i++) {
        const double t = (double)((top >> shifts[i]) & 0xFF), b = (double)((bottom >> shifts[i]) & 0xFF);
        ch[i] = (int)(t * alpha + b * (1.0 - alpha) + 0.5);
    }
    return IM_COL32(ch[0], ch[1], ch[2], 0xFF);
}

/**
 * @brief Ověří a vypíše kontrast dvojice tokenů.
 * @param what Popis dvojice. @param fg Popředí. @param bg Pozadí. @param min Nejmenší přípustný poměr.
 */
static void contrast_check(const char *what, ImU32 fg, ImU32 bg, double min)
{
    const double c = wcag_contrast(fg, bg);
    printf("contrast: %-46s %5.2f (min %.1f)\n", what, c, min);
    TEST_ASSERT_TRUE_MESSAGE(c >= min, what);
}

/**
 * Kontrasty spočítané z tokenů: text 4,5:1 proti kartě, ploše prvku a ploše
 * vybraného prvku; okraj ovládacích prvků, akcent a text zakázaných prvků
 * 3:1 proti kartě; hlavní a vedlejší text 4,5:1 proti ploše overlaye s
 * krytím 85 % nad bílým obrazem.
 */
void test_style_contrast(void)
{
    /* kontrolní hodnoty výpočtu */
    TEST_ASSERT_TRUE(fabs(wcag_contrast(IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255)) - 21.0) < 0.01);
    TEST_ASSERT_TRUE(fabs(wcag_contrast(OVL_COL_CARD, OVL_COL_CARD) - 1.0) < 0.01);

    contrast_check("text / card", OVL_COL_TEXT, OVL_COL_CARD, 4.5);
    contrast_check("text / raised (button)", OVL_COL_TEXT, OVL_COL_RAISED, 4.5);
    contrast_check("text / selected (segment)", OVL_COL_TEXT, OVL_COL_SELECTED, 4.5);
    contrast_check("text / pressed", OVL_COL_TEXT, OVL_COL_PRESSED, 4.5);
    contrast_check("secondary text / card", OVL_COL_TEXT_SECONDARY, OVL_COL_CARD, 4.5);
    contrast_check("secondary text / raised", OVL_COL_TEXT_SECONDARY, OVL_COL_RAISED, 4.5);
    contrast_check("secondary text / selected", OVL_COL_TEXT_SECONDARY, OVL_COL_SELECTED, 4.5);
    contrast_check("red text / card", OVL_COL_RED_TEXT, OVL_COL_CARD, 4.5);
    contrast_check("red text / selected (hovered button)", OVL_COL_RED_TEXT, OVL_COL_SELECTED, 4.5);
    contrast_check("accent text / card", OVL_COL_ACCENT_TEXT, OVL_COL_CARD, 4.5);
    contrast_check("accent text / selected (hovered button)", OVL_COL_ACCENT_TEXT, OVL_COL_SELECTED, 4.5);
    contrast_check("amber text / card", OVL_COL_AMBER, OVL_COL_CARD, 4.5);
    contrast_check("green text / card", OVL_COL_GREEN_TEXT, OVL_COL_CARD, 4.5);

    contrast_check("control border / card", OVL_COL_BORDER_CONTROL, OVL_COL_CARD, 3.0);
    contrast_check("control border / raised (button fill)", OVL_COL_BORDER_CONTROL, OVL_COL_RAISED, 3.0);
    contrast_check("accent (focus, toggle, underline) / card", OVL_COL_ACCENT, OVL_COL_CARD, 3.0);
    contrast_check("accent (segment line) / selected", OVL_COL_ACCENT, OVL_COL_SELECTED, 3.0);
    contrast_check("red (icon, border) / card", OVL_COL_RED, OVL_COL_CARD, 3.0);
    contrast_check("green (indicator) / card", OVL_COL_GREEN, OVL_COL_CARD, 3.0);
    contrast_check("disabled text / card", OVL_COL_TEXT_DISABLED, OVL_COL_CARD, 3.0);

    /* posudek v2 N01: jezdec posuvníku (barva vedlejšího textu). Sloupec karet tabu Display má pruh posuvníku
     * s podkladem karty; jinde leží jezdec na ploše overlaye. Třetí dvojice je pozadí změřené na snímku tabu
     * Display nad modrou obrazovkou stroje (#264878), kde byl původní jezdec 1,00:1. */
    contrast_check("scrollbar grab / card (display column track)", OVL_COL_TEXT_SECONDARY, OVL_COL_CARD, 3.0);
    contrast_check("scrollbar grab / panel", OVL_COL_TEXT_SECONDARY, OVL_COL_PANEL, 3.0);
    contrast_check("scrollbar grab / display tab over the blue screen", OVL_COL_TEXT_SECONDARY, IM_COL32(0x26, 0x48, 0x78, 0xFF), 3.0);

    /* review V4: dvojice, které widgety skutečně kreslí. Prvky, které nesou informaci: 3:1 */
    contrast_check("slider track, LED off / card", OVL_COL_MUTED, OVL_COL_CARD, 3.0);
    contrast_check("LED off / raised (drive row)", OVL_COL_MUTED, OVL_COL_RAISED, 3.0);
    contrast_check("slider knob / accent (filled track)", OVL_COL_KNOB, OVL_COL_ACCENT, 3.0);
    contrast_check("slider knob / track", OVL_COL_KNOB, OVL_COL_MUTED, 3.0);
    contrast_check("toggle knob (on) / accent", OVL_COL_KNOB, OVL_COL_ACCENT, 3.0);
    contrast_check("toggle knob (off) / raised", OVL_COL_TEXT_SECONDARY, OVL_COL_RAISED, 3.0);
    /* zakázané prvky: 3:1 se nevyžaduje (gui/DECISIONS.md, rulingy z opravné vlny); mez drží změřený stav */
    contrast_check("disabled: toggle knob / track (on)", OVL_COL_TEXT_DISABLED, OVL_COL_BORDER_STRONG, 2.1);
    contrast_check("disabled: toggle knob / card (off)", OVL_COL_TEXT_DISABLED, OVL_COL_CARD, 3.0);
    contrast_check("disabled: control border / card", OVL_COL_BORDER_STRONG, OVL_COL_CARD, 1.9);
    contrast_check("disabled: slider track / card", OVL_COL_BORDER_STRONG, OVL_COL_CARD, 1.9);
    contrast_check("disabled: slider knob / track", OVL_COL_TEXT_DISABLED, OVL_COL_BORDER_STRONG, 2.1);
    contrast_check("disabled: slider fill / track", OVL_COL_MUTED_DIM, OVL_COL_BORDER_STRONG, 1.4);

    /* plocha overlaye s krytím 85 % nad bílým obrazem (nejhorší případ) */
    const ImU32 panel_white = blend_over(OVL_COL_PANEL, IM_COL32(255, 255, 255, 255), OVL_PANEL_OPACITY);
    printf("contrast: panel over a white picture = #%02X%02X%02X\n", (unsigned)((panel_white >> IM_COL32_R_SHIFT) & 0xFF),
           (unsigned)((panel_white >> IM_COL32_G_SHIFT) & 0xFF), (unsigned)((panel_white >> IM_COL32_B_SHIFT) & 0xFF));
    contrast_check("text / panel 85 % over white", OVL_COL_TEXT, panel_white, 4.5);
    contrast_check("secondary text / panel 85 % over white", OVL_COL_TEXT_SECONDARY, panel_white, 4.5);
    contrast_check("amber text (Paused) / panel 85 % over white", OVL_COL_AMBER, panel_white, 4.5);
    contrast_check("accent (tab underline, focus) / panel over white", OVL_COL_ACCENT, panel_white, 3.0);
    /* tab Display (K04): plocha má krytí jen 55 %; text mimo karty je jen v horní liště a v patičce,
     * které jsou přitmavené další vrstvou 60 % (OVL_BAR_EXTRA_OPACITY v overlay.cpp) */
    const ImU32 preview_white = blend_over(OVL_COL_PANEL, IM_COL32(255, 255, 255, 255), OVL_PANEL_OPACITY_PREVIEW);
    const ImU32 bar_white = blend_over(OVL_COL_PANEL, preview_white, 0.60);
    contrast_check("text / bar on the Display tab over white", OVL_COL_TEXT, bar_white, 4.5);
    contrast_check("secondary text / bar on the Display tab over white", OVL_COL_TEXT_SECONDARY, bar_white, 4.5);
    contrast_check("amber text (Paused) / bar on the Display tab over white", OVL_COL_AMBER, bar_white, 4.5);
    contrast_check("accent (tab underline, focus) / bar on the Display tab over white", OVL_COL_ACCENT, bar_white, 3.0);
    /* a nad černým obrazem */
    const ImU32 panel_black = blend_over(OVL_COL_PANEL, IM_COL32(0, 0, 0, 255), OVL_PANEL_OPACITY);
    contrast_check("secondary text / panel 85 % over black", OVL_COL_TEXT_SECONDARY, panel_black, 4.5);
}

/** Písma overlaye: s dostupnými soubory tři samostatná písma a ikony, jinak základní písmo. */
void test_style_fonts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImFont *base = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
    TEST_ASSERT_NOT_NULL(overlay_font_ui());
    TEST_ASSERT_NOT_NULL(overlay_font_ui_bold());
    TEST_ASSERT_NOT_NULL(overlay_font_mono());
    char *inter = g_build_filename(MZTEST_FONT_DIR, "Inter-Regular.ttf", NULL);
    const bool have_files = MZTEST_FONT_DIR[0] && g_file_test(inter, G_FILE_TEST_IS_REGULAR);
    g_free(inter);
    if (!have_files) TEST_IGNORE_MESSAGE("overlay font files are not available");

    TEST_ASSERT_TRUE(overlay_font_ui() != base);
    TEST_ASSERT_TRUE(overlay_font_ui_bold() != base && overlay_font_ui_bold() != overlay_font_ui());
    TEST_ASSERT_TRUE(overlay_font_mono() != base && overlay_font_mono() != overlay_font_ui());
    TEST_ASSERT_TRUE(overlay_style_has_icons());
    /* základní písmo zůstává první v atlasu a ikony Lucide v něm nejsou (kolize s mzglyphs) */
    TEST_ASSERT_TRUE(io.Fonts->Fonts[0] == base);
    TEST_ASSERT_FALSE_MESSAGE(base->IsGlyphInFont((ImWchar)0xE14D), "Lucide must not be merged into the base UI font");
    TEST_ASSERT_TRUE_MESSAGE(overlay_font_ui()->IsGlyphInFont((ImWchar)0xE14D), "ICON_LC_SAVE in the overlay font");
    TEST_ASSERT_TRUE_MESSAGE(overlay_font_ui_bold()->IsGlyphInFont((ImWchar)0xE0A9), "ICON_LC_CPU in the bold overlay font");
    /* ikony se berou z Lucide (druhý zdroj), ne ze soukromé oblasti písma Inter, které má na týchž bodech jiné glyfy */
    const ImWchar icons[] = {0xE14D /* save */, 0xE0A9 /* cpu */, 0xE284 /* keyboard */, 0xE12E /* pause */, 0xE13C /* play */, 0xE149 /* rotate-cw */};
    for (ImWchar c : icons) {
        const ImFontGlyph *glyph = overlay_font_ui()->GetFontBaked(20.0f)->FindGlyphNoFallback(c);
        TEST_ASSERT_NOT_NULL(glyph);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)glyph->SourceIdx, "the icon glyph comes from lucide.ttf");
        glyph = overlay_font_ui_bold()->GetFontBaked(20.0f)->FindGlyphNoFallback(c);
        TEST_ASSERT_NOT_NULL(glyph);
        TEST_ASSERT_EQUAL_INT(1, (int)glyph->SourceIdx);
    }
    /* národní znaky a azbuka v písmu overlaye */
    TEST_ASSERT_TRUE(overlay_font_ui()->IsGlyphInFont((ImWchar)0x0159)); /* ř */
    TEST_ASSERT_TRUE(overlay_font_ui()->IsGlyphInFont((ImWchar)0x0416)); /* Ж */

    /* review V2: lucide.ttf a NotoSansJP-Regular.ttf se čtou jednou a všechny zdroje sdílejí jeden buffer, který atlas nevlastní */
    char *cjk = g_build_filename(MZTEST_FONT_DIR, "NotoSansJP-Regular.ttf", NULL);
    const bool have_cjk = g_file_test(cjk, G_FILE_TEST_IS_REGULAR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(have_cjk ? 2 : 1, overlay_style_test_font_file_loads(), "each shared font file is read from the disk once");
    int size_a = 0, size_b = 0;
    const void *data_a = overlay_style_font_file_data(cjk, &size_a);
    /* jiný zápis téže cesty (oddělovače) dá stejnou paměť */
    char *cjk_alt = g_strdup(cjk);
    for (char *c = cjk_alt; *c; c++)
        if (*c == '\\') *c = '/';
    const void *data_b = overlay_style_font_file_data(cjk_alt, &size_b);
    g_free(cjk_alt);
    g_free(cjk);
    TEST_ASSERT_TRUE(data_a == data_b);
    TEST_ASSERT_EQUAL_INT(size_a, size_b);
    TEST_ASSERT_EQUAL_INT(have_cjk ? 2 : 1, overlay_style_test_font_file_loads());
    int n_cjk = 0, n_lucide = 0;
    const void *p_cjk = NULL, *p_lucide = NULL;
    for (int i = 0; i < io.Fonts->Sources.Size; i++) {
        const ImFontConfig &src = io.Fonts->Sources[i];
        const bool is_cjk = strstr(src.Name, "NotoSansJP") != NULL, is_lucide = strstr(src.Name, "lucide") != NULL;
        if (!is_cjk && !is_lucide) continue;
        TEST_ASSERT_FALSE_MESSAGE(src.FontDataOwnedByAtlas, src.Name);
        const void **first = is_cjk ? &p_cjk : &p_lucide;
        if (!*first) *first = src.FontData;
        TEST_ASSERT_TRUE_MESSAGE(*first == src.FontData, "all sources of one file share one buffer");
        if (is_cjk) n_cjk++;
        else n_lucide++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n_lucide, "lucide.ttf is merged into both Inter cuts");
    if (have_cjk) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(3, n_cjk, "NotoSansJP is merged into Inter Regular, Inter SemiBold and JetBrains Mono");
        TEST_ASSERT_TRUE(p_cjk == data_a);
        /* review V3: japonské znaky i v neproporcionálním písmu (jména souborů) */
        const ImWchar jp[] = {0x3042 /* hiragana a */, 0x30AB /* katakana ka */, 0x6F22 /* kanji */};
        for (ImWchar c : jp) {
            TEST_ASSERT_NOT_NULL_MESSAGE(overlay_font_mono()->GetFontBaked(16.0f)->FindGlyphNoFallback(c), "Japanese glyph in the monospace overlay font");
            TEST_ASSERT_NOT_NULL(overlay_font_ui()->GetFontBaked(16.0f)->FindGlyphNoFallback(c));
            TEST_ASSERT_NOT_NULL(overlay_font_ui_bold()->GetFontBaked(16.0f)->FindGlyphNoFallback(c));
        }
        /* ikony zůstávají druhým zdrojem i po sloučení japonských znaků */
        TEST_ASSERT_EQUAL_INT(1, (int)overlay_font_ui()->GetFontBaked(20.0f)->FindGlyphNoFallback((ImWchar)0xE14D)->SourceIdx);
    }
}

/**
 * Review V2: základní písmo UI slučuje japonské znaky ze sdíleného bufferu
 * (overlay_style_add_shared_font()) místo ImFontAtlas::AddFontFromFileTTF().
 * Se stejným nastavením, velikostí a rozsahy jako v myimgui_set_fonts() musí
 * dát obě cesty stejné glyfy.
 */
void test_shared_font_matches_file_font(void)
{
    ImGuiIO &io = ImGui::GetIO();
    char *droid = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
    char *cjk = g_build_filename(MZTEST_FONT_DIR, "NotoSansJP-Regular.ttf", NULL);
    const bool have_files = MZTEST_FONT_DIR[0] && g_file_test(droid, G_FILE_TEST_IS_REGULAR) && g_file_test(cjk, G_FILE_TEST_IS_REGULAR);
    if (!have_files) {
        g_free(droid);
        g_free(cjk);
        TEST_IGNORE_MESSAGE("DroidSans.ttf or NotoSansJP-Regular.ttf is not available");
    }
    /* nastavení jako myimgui_set_fonts() */
    static const ImWchar i18n_ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
    ImFontConfig base_cfg;
    base_cfg.OversampleH = 3;
    base_cfg.OversampleV = 3;
    base_cfg.PixelSnapH = true;
    ImFontConfig cjk_cfg;
    cjk_cfg.MergeMode = true;
    cjk_cfg.OversampleH = 2;
    cjk_cfg.OversampleV = 1;
    cjk_cfg.PixelSnapH = true;
    const int loads = overlay_style_test_font_file_loads();

    ImFont *from_file = io.Fonts->AddFontFromFileTTF(droid, 28.0f, &base_cfg, i18n_ranges);
    TEST_ASSERT_NOT_NULL(from_file);
    TEST_ASSERT_NOT_NULL(io.Fonts->AddFontFromFileTTF(cjk, 28.0f, &cjk_cfg, io.Fonts->GetGlyphRangesJapanese()));
    ImFont *from_shared = io.Fonts->AddFontFromFileTTF(droid, 28.0f, &base_cfg, i18n_ranges);
    TEST_ASSERT_NOT_NULL(from_shared);
    TEST_ASSERT_TRUE_MESSAGE(overlay_style_add_shared_font(io.Fonts, cjk, 28.0f, &cjk_cfg, io.Fonts->GetGlyphRangesJapanese()) == from_shared,
                             "the shared source is merged into the font added last");
    TEST_ASSERT_EQUAL_INT_MESSAGE(loads, overlay_style_test_font_file_loads(), "the file is already in the shared memory");
    /* zdroje: stejné jméno a nastavení, liší se jen vlastnictvím dat */
    TEST_ASSERT_EQUAL_INT(2, from_file->Sources.Size);
    TEST_ASSERT_EQUAL_INT(2, from_shared->Sources.Size);
    const ImFontConfig *sa = from_file->Sources[1], *sb = from_shared->Sources[1];
    TEST_ASSERT_EQUAL_STRING(sa->Name, sb->Name);
    TEST_ASSERT_EQUAL_INT(sa->FontDataSize, sb->FontDataSize);
    TEST_ASSERT_EQUAL_FLOAT(sa->SizePixels, sb->SizePixels);
    TEST_ASSERT_TRUE(sa->GlyphRanges == sb->GlyphRanges);
    TEST_ASSERT_EQUAL_INT(sa->OversampleH, sb->OversampleH);
    TEST_ASSERT_EQUAL_INT(sa->OversampleV, sb->OversampleV);
    TEST_ASSERT_TRUE(sa->FontDataOwnedByAtlas);
    TEST_ASSERT_FALSE(sb->FontDataOwnedByAtlas);

    const ImWchar chars[] = {'A', 'g', 0x0159 /* ř */, 0x0416 /* Ж */, 0x3042, 0x30AB, 0x6F22, 0xFF21 /* A plné šířky */, 0x4E00};
    for (ImWchar c : chars) {
        char msg[48];
        snprintf(msg, sizeof(msg), "U+%04X", (unsigned)c);
        const ImFontGlyph *ga = from_file->GetFontBaked(28.0f)->FindGlyphNoFallback(c);
        const ImFontGlyph *gb = from_shared->GetFontBaked(28.0f)->FindGlyphNoFallback(c);
        TEST_ASSERT_NOT_NULL_MESSAGE(ga, msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(gb, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)ga->SourceIdx, (int)gb->SourceIdx, msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ga->AdvanceX, gb->AdvanceX, msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ga->X0, gb->X0, msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ga->Y0, gb->Y0, msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ga->X1, gb->X1, msg);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ga->Y1, gb->Y1, msg);
    }
    /* znaky mimo předané rozsahy (hangul, thajština): obě cesty se shodují i v tom, zda glyf mají */
    const ImWchar outside[] = {0xAC00, 0x0E01, 0x05D0};
    for (ImWchar c : outside) {
        const bool in_file = from_file->GetFontBaked(28.0f)->FindGlyphNoFallback(c) != NULL;
        const bool in_shared = from_shared->GetFontBaked(28.0f)->FindGlyphNoFallback(c) != NULL;
        TEST_ASSERT_EQUAL_INT(in_file ? 1 : 0, in_shared ? 1 : 0);
    }
    g_free(droid);
    g_free(cjk);
}

/* ================================================================
 * Galerie widgetů
 * ================================================================ */

/** @brief Největší počet obdélníků zaznamenaných galerií. */
#define GAL_RECT_MAX 96

/**
 * @brief Obdélník prvku galerie z posledního gallery_draw().
 *
 * Invariant: parent je index dřívějšího záznamu (karta nebo okno) nebo -1.
 */
typedef struct st_GAL_RECT {
    char name[32]; /**< jméno prvku */
    ImVec2 min;    /**< levý horní roh */
    ImVec2 max;    /**< pravý dolní roh */
    int parent;    /**< index rodiče v s_gal; -1 = bez rodiče */
} st_GAL_RECT;

/** @brief Obdélníky prvků galerie z posledního gallery_draw(). */
static st_GAL_RECT s_gal[GAL_RECT_MAX];
/** @brief Počet platných záznamů v s_gal. */
static int s_gal_n = 0;
/** @brief Index karty (nebo okna), do které patří právě kreslené prvky. */
static int s_gal_parent = -1;
/** @brief Jméno prvku, který má po vykreslení dostat fokus navigace (jednorázově); NULL = žádný. */
static const char *s_gal_focus = NULL;
/** @brief Svislé rolování okna galerie vnucené testem; záporné = neměnit. */
static float s_gal_scroll = -1.0f;
/** @brief Největší svislé rolování okna galerie z posledního gallery_draw(). */
static float s_gal_scroll_max = 0.0f;

/** @brief Hodnoty prvků galerie; drží se mezi snímky, aby šly měnit myší a klávesnicí. */
static struct {
    bool toggle_off, toggle_on, toggle_focus, toggle_disabled;
    int seg, seg_disabled, seg_focus;
    float slider, slider_focus, slider_disabled;
    int clicks_change, clicks_disabled, clicks_long;
    int slider_disabled_changes; /**< kolikrát zakázaný posuvník ohlásil změnu hodnoty */
} s_gs;

/** @brief Text o 200 znacích (jméno souboru bez mezer). */
static char s_long_text[201];

/** @brief Nastaví hodnoty prvků galerie na výchozí. */
static void gallery_reset(void)
{
    memset(&s_gs, 0, sizeof(s_gs));
    s_gs.toggle_on = s_gs.toggle_disabled = true;
    s_gs.seg_disabled = 1;
    s_gs.seg_focus = 1;
    s_gs.slider = 80.0f;
    s_gs.slider_focus = 35.0f;
    s_gs.slider_disabled = 70.0f;
    for (int i = 0; i < 200; i++) s_long_text[i] = (char)((i % 10 == 9) ? '-' : ('a' + i % 26));
    memcpy(s_long_text + 196, ".dsk", 4);
    s_long_text[200] = '\0';
    s_gal_focus = NULL;
    s_gal_scroll = -1.0f;
}

/**
 * @brief Najede myší na bod a čeká, až se objeví tooltip.
 * @param x Vodorovná souřadnice bodu.
 * @param y Svislá souřadnice bodu.
 * @return true = do 60 snímků (asi 1,2 s) je aktivní okno tooltipu ImGui.
 * @pre Běží smyčka snímků testu (ov_frame()); myš po návratu zůstává na bodě.
 */
static bool ov_hover_tooltip(float x, float y)
{
    ImGui::GetIO().AddMousePosEvent(x, y);
    for (int i = 0; i < 60; i++) {
        SDL_Delay(20);
        ov_frame();
        ImGuiContext &g = *GImGui;
        for (int w = 0; w < g.Windows.Size; w++)
            if ((g.Windows[w]->Flags & ImGuiWindowFlags_Tooltip) && g.Windows[w]->Active) return true;
    }
    return false;
}

/**
 * @brief Zaznamená obdélník naposledy vykresleného prvku pod jménem; prvku jménem s_gal_focus dá fokus navigace.
 * @param name Jméno prvku.
 * @return Index záznamu, -1 při plné tabulce.
 */
static int gal_note(const char *name)
{
    if (s_gal_n >= GAL_RECT_MAX) return -1;
    st_GAL_RECT *r = &s_gal[s_gal_n];
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->min = ImGui::GetItemRectMin();
    r->max = ImGui::GetItemRectMax();
    r->parent = s_gal_parent;
    if (s_gal_focus && strcmp(s_gal_focus, name) == 0) {
        ImGui::FocusItem();
        s_gal_focus = NULL;
    }
    return s_gal_n++;
}

/** @brief Najde záznam prvku galerie podle jména; NULL = v posledním kreslení nebyl. */
static const st_GAL_RECT *gal_find(const char *name)
{
    for (int i = 0; i < s_gal_n; i++)
        if (strcmp(s_gal[i].name, name) == 0) return &s_gal[i];
    return NULL;
}

/**
 * @brief Začne kartu galerie a zaznamená ji (obdélník doplní gal_card_end()).
 * @param title Nadpis karty "Text##id". @param name Jméno záznamu. @param width Šířka karty.
 * @return Návratová hodnota ovl_card_begin().
 */
static bool gal_card_begin(const char *title, const char *name, float width)
{
    int idx = -1;
    if (s_gal_n < GAL_RECT_MAX) {
        idx = s_gal_n++;
        snprintf(s_gal[idx].name, sizeof(s_gal[idx].name), "%s", name);
        s_gal[idx].parent = 0; /* okno galerie */
        s_gal[idx].min = s_gal[idx].max = ImVec2(0.0f, 0.0f);
    }
    s_gal_parent = idx;
    return ovl_card_begin(title, ImVec2(width, 0.0f));
}

/** @brief Ukončí kartu galerie a doplní její obdélník. */
static void gal_card_end(void)
{
    ovl_card_end();
    if (s_gal_parent >= 0) {
        s_gal[s_gal_parent].min = ImGui::GetItemRectMin();
        s_gal[s_gal_parent].max = ImGui::GetItemRectMax();
    }
    s_gal_parent = -1;
}

/**
 * @brief Galerie widgetů overlaye ve všech stavech (běžný, fokus, vybraný, zakázaný) přes danou plochu.
 *
 * Okno "###OverlayGallery" s plochou overlaye (krytí 85 %); čtyři karty,
 * v úzkém okně pod sebou. Obdélníky prvků ukládá do s_gal (index 0 = okno).
 * Texty galerie se nepřekládají (není to UI aplikace).
 *
 * @param pos  Levý horní roh plochy.
 * @param size Velikost plochy.
 * @pre Mezi ImGui::NewFrame() a ImGui::Render(), mimo Begin() / End() jiného okna.
 */
static void gallery_draw(ImVec2 pos, ImVec2 size)
{
    s_gal_n = 0;
    s_gal_parent = -1;
    if (!s_long_text[0]) gallery_reset();
    overlay_style_set_viewport(size);
    overlay_style_push();
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(overlay_px(OVL_MARGIN), overlay_px(16.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, overlay_color_alpha(OVL_COL_PANEL, OVL_PANEL_OPACITY));
    const bool visible = ImGui::Begin("###OverlayGallery", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!visible) {
        ImGui::End();
        overlay_style_pop();
        return;
    }
    snprintf(s_gal[0].name, sizeof(s_gal[0].name), "window");
    s_gal[0].min = ImGui::GetWindowPos();
    s_gal[0].max = ImVec2(s_gal[0].min.x + ImGui::GetWindowSize().x, s_gal[0].min.y + ImGui::GetWindowSize().y);
    s_gal[0].parent = -1;
    s_gal_n = 1;
    if (s_gal_scroll >= 0.0f) ImGui::SetScrollY(s_gal_scroll);
    s_gal_scroll_max = ImGui::GetScrollMaxY();

    const bool narrow = overlay_is_narrow();
    const float gap = overlay_px(16.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float col_w = narrow ? avail : floorf((avail - gap) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(overlay_px(OVL_GRID), overlay_px(12.0f)));

    /* levý sloupec */
    ImGui::BeginGroup();
    if (gal_card_begin("Buttons##gal_buttons", "card_buttons", col_w)) {
        if (ovl_button("Change##gal", NULL, NULL, OVL_BUTTON_NORMAL)) s_gs.clicks_change++;
        gal_note("btn_change");
        ImGui::SameLine();
        ovl_button("Resume##gal", ICON_LC_PLAY, NULL, OVL_BUTTON_PRIMARY);
        gal_note("btn_primary");
        ImGui::SameLine();
        ovl_button("Reset##gal", ICON_LC_ROTATE_CW, "F12", OVL_BUTTON_DANGER);
        gal_note("btn_danger");

        ovl_button("Insert disk##gal", NULL, NULL, OVL_BUTTON_DASHED);
        gal_note("btn_dashed");
        ImGui::SameLine();
        ovl_button("##gal_eject", OVL_ICON_EJECT, NULL, OVL_BUTTON_NORMAL);
        gal_note("btn_eject");
        ImGui::SameLine();
        ovl_button("##gal_save", ICON_LC_SAVE, NULL, OVL_BUTTON_NORMAL);
        gal_note("btn_save");
        ImGui::SameLine();
        ImGui::BeginDisabled();
        ovl_button("##gal_eject_disabled", OVL_ICON_EJECT, NULL, OVL_BUTTON_NORMAL);
        gal_note("btn_eject_dis");
        ImGui::SameLine();
        ovl_button("Insert disk##gal_disabled", NULL, NULL, OVL_BUTTON_DASHED);
        gal_note("btn_dashed_dis");
        ImGui::EndDisabled();

        ovl_test_force_focus_next();
        ovl_button("Quick Save##gal", ICON_LC_DOWNLOAD, "Alt+F8", OVL_BUTTON_NORMAL);
        gal_note("btn_focus");
        ImGui::SameLine();
        ImGui::BeginDisabled();
        if (ovl_button("Quick Load##gal", ICON_LC_UPLOAD, "Alt+F9", OVL_BUTTON_NORMAL)) s_gs.clicks_disabled++;
        gal_note("btn_disabled");
        ImGui::EndDisabled();

        /* tlačítko přes celou šířku: samotný popisek se středí */
        ovl_button_ex("Open tape window##gal", NULL, NULL, OVL_BUTTON_NORMAL, ImVec2(-1.0f, 0.0f));
        gal_note("btn_wide");
        /* popisek o 200 znacích: štítek ustoupí, popisek dostane výpustku */
        char long_label[260];
        snprintf(long_label, sizeof(long_label), "%s##gal_long", s_long_text);
        if (ovl_button_ex(long_label, ICON_LC_FOLDER_OPEN, "Alt+Ctrl+Enter", OVL_BUTTON_NORMAL, ImVec2(-1.0f, 0.0f))) s_gs.clicks_long++;
        gal_note("btn_long");
    }
    gal_card_end();

    if (gal_card_begin("Sliders and long labels##gal_sliders", "card_sliders", col_w)) {
        ovl_row_begin("Master volume");
        gal_note("lbl_slider");
        ovl_slider_percent("gal_slider", &s_gs.slider, 0.0f, 100.0f);
        gal_note("slider");
        ovl_row_end();

        ovl_row_begin("Focused");
        gal_note("lbl_slider_focus");
        ovl_test_force_focus_next();
        ovl_slider_percent_ex("gal_slider_focus", &s_gs.slider_focus, 0.0f, 100.0f, 5.0f, 0.0f);
        gal_note("slider_focus");
        ovl_row_end();

        ImGui::BeginDisabled();
        ovl_row_begin("Disabled");
        gal_note("lbl_slider_dis");
        if (ovl_slider_percent("gal_slider_disabled", &s_gs.slider_disabled, 0.0f, 100.0f)) s_gs.slider_disabled_changes++;
        gal_note("slider_disabled");
        ovl_row_end();
        ImGui::EndDisabled();

        /* popisek řádku o 200 znacích nesmí překrýt prvek */
        ovl_row_begin(s_long_text);
        gal_note("lbl_long");
        ovl_test_force_focus_next();
        static const char *const modes[] = {"Tape", "Boost", "Direct"};
        ovl_segmented("gal_seg_focus", modes, 3, &s_gs.seg_focus);
        gal_note("seg_focus");
        ovl_row_end();
    }
    gal_card_end();
    ImGui::EndGroup();

    /* pravý sloupec */
    if (!narrow) ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    if (gal_card_begin("Toggles and segments##gal_toggles", "card_toggles", col_w)) {
        ovl_row_begin("Mute");
        gal_note("lbl_toggle_off");
        ovl_row_right(ovl_toggle_size().x);
        ovl_toggle("gal_toggle_off", &s_gs.toggle_off);
        gal_note("toggle_off");
        ovl_row_end();

        ovl_row_begin("Fullscreen");
        gal_note("lbl_toggle_on");
        ovl_row_right(ovl_toggle_size().x + overlay_px(OVL_GRID) + ovl_keycap_size("Alt+Ctrl+Enter").x);
        ovl_toggle("gal_toggle_on", &s_gs.toggle_on);
        gal_note("toggle_on");
        ImGui::SameLine();
        ovl_keycap("Alt+Ctrl+Enter");
        gal_note("keycap_row");
        ovl_row_end();

        ovl_row_begin("Focused");
        gal_note("lbl_toggle_focus");
        ovl_row_right(ovl_toggle_size().x);
        ovl_test_force_focus_next();
        ovl_toggle("gal_toggle_focus", &s_gs.toggle_focus);
        gal_note("toggle_focus");
        ovl_row_end();

        ImGui::BeginDisabled();
        ovl_row_begin("Disabled");
        gal_note("lbl_toggle_dis");
        ovl_row_right(2.0f * ovl_toggle_size().x + overlay_px(OVL_GRID));
        bool disabled_off = false;
        ovl_toggle("gal_toggle_disabled_off", &disabled_off);
        gal_note("toggle_dis_off");
        ImGui::SameLine();
        ovl_toggle("gal_toggle_disabled", &s_gs.toggle_disabled);
        gal_note("toggle_disabled");
        ovl_row_end();
        ImGui::EndDisabled();

        ovl_row_begin("CPU speed");
        gal_note("lbl_seg");
        static const char *const speeds[] = {"Normal", "Custom", "Max"};
        static const char *const speed_keys[] = {"Alt+N", NULL, "Alt+M"};
        ovl_segmented_ex("gal_seg", speeds, speed_keys, 3, &s_gs.seg, 0.0f);
        gal_note("seg");
        ovl_row_end();

        ImGui::BeginDisabled();
        ovl_row_begin("Color schema");
        gal_note("lbl_seg_dis");
        static const char *const colors[] = {"Normal", "Grayscale", "Green"};
        ovl_segmented("gal_seg_disabled", colors, 3, &s_gs.seg_disabled);
        gal_note("seg_disabled");
        ovl_row_end();
        ImGui::EndDisabled();
    }
    gal_card_end();

    if (gal_card_begin("Text and status##gal_status", "card_status", col_w)) {
        ovl_keycap("1");
        gal_note("keycap_1");
        ImGui::SameLine();
        ovl_keycap("F10");
        gal_note("keycap_f10");
        ImGui::SameLine();
        ovl_keycap("Alt+Ctrl+Enter");
        gal_note("keycap_long");
        ImGui::SameLine();
        ImGui::BeginDisabled();
        ovl_keycap("Esc");
        gal_note("keycap_dis");
        ImGui::EndDisabled();

        ovl_status_chip("MZ-800", OVL_STATUS_NEUTRAL);
        gal_note("chip_neutral");
        ImGui::SameLine();
        ovl_status_chip_icon(ICON_LC_PLAY, "Playing", OVL_STATUS_ACTIVE);
        gal_note("chip_active");
        ImGui::SameLine();
        ovl_status_chip_icon(ICON_LC_PAUSE, "Paused", OVL_STATUS_WARNING);
        gal_note("chip_warning");
        ImGui::SameLine();
        ovl_status_chip("Recording", OVL_STATUS_DANGER);
        gal_note("chip_danger");

        ovl_led(true);
        gal_note("led_on");
        ImGui::SameLine();
        ovl_led(false);
        gal_note("led_off");
        ImGui::SameLine();
        const char *const icons[] = {ICON_LC_SAVE, ICON_LC_CPU, ICON_LC_MONITOR, ICON_LC_VOLUME_2, ICON_LC_KEYBOARD, ICON_LC_CAMERA, ICON_LC_CASSETTE_TAPE, OVL_ICON_EJECT};
        for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
            char name[16];
            snprintf(name, sizeof(name), "icon%d", i);
            ovl_icon(icons[i], (i == 0) ? OVL_COL_TEXT : OVL_COL_TEXT_SECONDARY);
            gal_note(name);
            ImGui::SameLine();
        }
        ImGui::NewLine();

        /* jméno souboru o 200 znacích neproporcionálním písmem: výpustka */
        ImGui::PushFont(overlay_font_mono(), overlay_font_size(overlay_font_mono(), OVL_FONT_BODY));
        ovl_text_ellipsis(s_long_text, 0.0f);
        gal_note("text_long");
        ovl_text_ellipsis(s_long_text, overlay_px(120.0f));
        gal_note("text_fixed");
        ImGui::SameLine();
        ovl_text_ellipsis("cpm-system.dsk", overlay_px(240.0f));
        gal_note("text_short");
        ImGui::PopFont();

        ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT_SECONDARY);
        ovl_text_ellipsis("Secondary text: CP/M, 320 KB", 0.0f);
        gal_note("text_secondary");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT_DISABLED);
        ovl_text_ellipsis("Disabled text: FD controller is not connected.", 0.0f);
        gal_note("text_disabled");
        ImGui::PopStyleColor();
    }
    gal_card_end();
    ImGui::EndGroup();

    ImGui::PopStyleVar();
    ImGui::End();
    overlay_style_pop();
}

/** @brief Klikne do středu prvku galerie podle jména. */
static void gal_click(const char *name)
{
    const st_GAL_RECT *r = gal_find(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    ov_click(ImVec2((r->min.x + r->max.x) * 0.5f, (r->min.y + r->max.y) * 0.5f));
}

/** @brief Dá prvku galerie fokus navigace a zviditelní kurzor navigace (jako po stisku šipky). */
static void gal_focus(const char *name)
{
    s_gal_focus = name;
    ov_frame();
    ov_frame();
    TEST_ASSERT_NULL_MESSAGE(s_gal_focus, name);
    ImGui::SetNavCursorVisible(true);
    ov_frame();
}

/** @brief Zapne galerii s výchozími hodnotami v dané velikosti viewportu a nechá ustálit rozvržení. */
static void gal_start(float w, float h)
{
    gallery_reset();
    s_display = ImVec2(w, h);
    s_gallery = true;
    for (int i = 0; i < 4; i++) ov_frame();
}

/**
 * @brief Najde prvek galerie; v úzkém okně k němu doroluje (karta mimo výřez se nekreslí).
 * @param name Jméno prvku.
 * @return Záznam z posledního kreslení; NULL = prvek se nenašel při žádném rolování.
 */
static const st_GAL_RECT *gal_seek(const char *name)
{
    const float scroll_max = s_gal_scroll_max;
    for (float scroll = 0.0f; scroll <= scroll_max + 1.0f; scroll += 100.0f) {
        s_gal_scroll = (scroll < scroll_max) ? scroll : scroll_max;
        for (int i = 0; i < 4; i++) ov_frame();
        const st_GAL_RECT *r = gal_find(name);
        if (r) return r;
    }
    return NULL;
}

/** @brief Překrývají se dva obdélníky o víc než půl pixelu? */
static bool rects_overlap(ImVec2 amin, ImVec2 amax, ImVec2 bmin, ImVec2 bmax)
{
    return amin.x < bmax.x - 0.5f && bmin.x < amax.x - 0.5f && amin.y < bmax.y - 0.5f && bmin.y < amax.y - 0.5f;
}

/* ================================================================
 * Geometrie
 * ================================================================ */

/**
 * Rámec při 640 x 480, 1280 x 720 a 2560 x 1440 na všech tabech: prvky
 * horní lišty a patičky leží v okně a ve svém pruhu a nepřekrývají se;
 * obsah je mezi lištou a patičkou. V úzkém okně ustoupí štítky platformy
 * a rychlosti, štítek pauzy a Close zůstávají.
 */
void test_frame_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {2560.0f, 1440.0f}};
    g_emulator.paused = true; /* štítek Paused */
    imgui_overlay_open();
    for (const float *sz : sizes) {
        s_display = ImVec2(sz[0], sz[1]);
        const float u = overlay_unit_for(s_display);
        for (int t = 0; t < (int)OVERLAY_TAB_COUNT; t++) {
            imgui_overlay_set_tab((en_OVERLAY_TAB)t);
            for (int i = 0; i < 3; i++) ov_frame();
            char msg[96];
            snprintf(msg, sizeof(msg), "%.0fx%.0f tab %d", sz[0], sz[1], t);

            const char *const bar_names[] = {"tab0", "tab1", "tab2", "tab3", "tab4", "tab5", "chip_platform", "chip_speed", "chip_paused", "close"};
            ImVec2 mn[10], mx[10];
            bool drawn[10];
            for (int i = 0; i < 10; i++) {
                drawn[i] = imgui_overlay_test_item_rect(bar_names[i], &mn[i], &mx[i]);
                if (!drawn[i]) continue;
                TEST_ASSERT_TRUE_MESSAGE(mn[i].x >= -0.5f && mx[i].x <= sz[0] + 0.5f, msg);
                TEST_ASSERT_TRUE_MESSAGE(mn[i].y >= -0.5f && mx[i].y <= OVL_TOPBAR_H * u + 0.5f, msg);
                TEST_ASSERT_TRUE_MESSAGE(mx[i].x > mn[i].x && mx[i].y > mn[i].y, msg);
                for (int j = 0; j < i; j++)
                    if (drawn[j]) TEST_ASSERT_FALSE_MESSAGE(rects_overlap(mn[i], mx[i], mn[j], mx[j]), msg);
            }
            for (int i = 0; i < 6; i++) TEST_ASSERT_TRUE_MESSAGE(drawn[i], msg);
            TEST_ASSERT_TRUE_MESSAGE(drawn[8], "Paused stays");
            TEST_ASSERT_TRUE_MESSAGE(drawn[9], "Close stays");
            if (sz[0] >= 1280.0f) {
                TEST_ASSERT_TRUE_MESSAGE(drawn[6] && drawn[7], "platform and speed chips are shown in a wide window");
            } else {
                TEST_ASSERT_FALSE_MESSAGE(drawn[6], "the platform chip gives way first in a narrow window");
            }

            ImVec2 cmin, cmax, hmin, hmax, rmin, rmax;
            TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_test_item_rect("content", &cmin, &cmax), msg);
            /* ImGui velikost podřízeného okna zaokrouhluje na celé pixely */
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2.0f, OVL_TOPBAR_H * u, cmin.y, msg);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2.0f, sz[1] - OVL_FOOTER_H * u, cmax.y, msg);
            TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_test_item_rect("footer_hints", &hmin, &hmax), msg);
            TEST_ASSERT_TRUE_MESSAGE(hmin.x >= 0.0f && hmax.x <= sz[0] + 0.5f && hmin.y >= cmax.y - 2.0f && hmax.y <= sz[1] + 0.5f, msg);
            if (imgui_overlay_test_item_rect("footer_right", &rmin, &rmax)) {
                TEST_ASSERT_TRUE_MESSAGE(rmax.x <= sz[0] + 0.5f && rmin.y >= cmax.y - 2.0f && rmax.y <= sz[1] + 0.5f, msg);
                TEST_ASSERT_FALSE_MESSAGE(rects_overlap(hmin, hmax, rmin, rmax), msg);
            } else {
                TEST_ASSERT_TRUE_MESSAGE(sz[0] < 1280.0f, "the classic menu hint is shown in a wide window");
            }
        }
    }

    /* štítek Muted: jen při ztlumení; leží v liště, nic nepřekrývá a ustupuje až po platformě a rychlosti */
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_test_item_rect("chip_muted", NULL, NULL), "no Muted chip without mute");
    const bool saved_mute = iface_audio_get_mute();
    iface_audio_set_mute(true);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    for (const float *sz : sizes) {
        s_display = ImVec2(sz[0], sz[1]);
        const float u = overlay_unit_for(s_display);
        for (int i = 0; i < 3; i++) ov_frame();
        char msg[96];
        snprintf(msg, sizeof(msg), "muted %.0fx%.0f", sz[0], sz[1]);
        const char *const bar_names[] = {"tab0", "tab1", "tab2", "tab3", "tab4", "tab5", "chip_platform", "chip_speed", "chip_muted", "chip_paused", "close"};
        ImVec2 mn[11], mx[11];
        bool drawn[11];
        for (int i = 0; i < 11; i++) {
            drawn[i] = imgui_overlay_test_item_rect(bar_names[i], &mn[i], &mx[i]);
            if (!drawn[i]) continue;
            TEST_ASSERT_TRUE_MESSAGE(mn[i].x >= -0.5f && mx[i].x <= sz[0] + 0.5f, msg);
            TEST_ASSERT_TRUE_MESSAGE(mn[i].y >= -0.5f && mx[i].y <= OVL_TOPBAR_H * u + 0.5f, msg);
            for (int j = 0; j < i; j++)
                if (drawn[j]) TEST_ASSERT_FALSE_MESSAGE(rects_overlap(mn[i], mx[i], mn[j], mx[j]), msg);
        }
        TEST_ASSERT_TRUE_MESSAGE(drawn[9] && drawn[10], "Paused and Close stay");
        if (sz[0] >= 2560.0f) TEST_ASSERT_TRUE_MESSAGE(drawn[8], "the Muted chip is shown in a wide window");
        if (!drawn[8]) TEST_ASSERT_FALSE_MESSAGE(drawn[6] || drawn[7], "Muted gives way only after the platform and the speed");
        if (drawn[8] && drawn[9]) TEST_ASSERT_TRUE_MESSAGE(mx[8].x <= mn[9].x, "Muted stands before Paused");
        printf("topbar muted %.0fx%.0f: platform %d speed %d muted %d (width %.0f)\n", sz[0], sz[1], (int)drawn[6], (int)drawn[7], (int)drawn[8],
               drawn[8] ? mx[8].x - mn[8].x : 0.0f);
    }
    /* 640 x 480 bez pauzy: štítek Muted zůstane jako samotná ikona (posudek N04). Se štítkem Paused vedle sebe mu
     * při této šířce chybí několik pixelů a ustoupí celý (viz výpis výše). */
    if (overlay_style_has_icons()) {
        g_emulator.paused = false;
        s_display = ImVec2(640.0f, 480.0f);
        for (int i = 0; i < 3; i++) ov_frame();
        ImVec2 mmn, mmx, cmn, cmx, tmn, tmx;
        TEST_ASSERT_FALSE(imgui_overlay_test_item_rect("chip_paused", NULL, NULL));
        TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_test_item_rect("chip_muted", &mmn, &mmx), "the Muted chip stays as an icon at 640x480");
        TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("close", &cmn, &cmx));
        TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("tab5", &tmn, &tmx));
        TEST_ASSERT_TRUE_MESSAGE(mmn.x >= tmx.x && mmx.x <= cmn.x, "the Muted icon lies between the tabs and Close");
        TEST_ASSERT_TRUE_MESSAGE(mmx.x - mmn.x < 2.0f * (mmx.y - mmn.y), "the chip holds only the icon");
        printf("topbar muted 640x480 running: muted width %.0f\n", mmx.x - mmn.x);
        g_emulator.paused = true;
    }
    iface_audio_set_mute(saved_mute);
    imgui_overlay_close();
}

/**
 * Widgety při 640 x 480, 1280 x 720 a 2560 x 1440: každý prvek leží ve své
 * kartě, karty vodorovně v okně (svisle se úzké okno roluje), sourozenci se
 * nepřekrývají; popisek řádku o 200 znacích nepřekryje svůj prvek.
 */
void test_widgets_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {2560.0f, 1440.0f}};
    for (const float *sz : sizes) {
        gal_start(sz[0], sz[1]);
        /* úzké okno se roluje: karta mimo výřez se nekreslí, proto se galerie projde po krocích rolování */
        const float scroll_max = s_gal_scroll_max;
        if (sz[0] >= 1280.0f) TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, scroll_max, "the gallery fits without scrolling");
        else TEST_ASSERT_TRUE_MESSAGE(scroll_max > 0.0f, "the gallery scrolls in a small window");
        int seen_max = 0;
        bool seen_buttons = false, seen_sliders = false, seen_toggles = false, seen_status = false;
        for (float scroll = 0.0f; scroll <= scroll_max + 1.0f; scroll += sz[1] * 0.4f) {
            s_gal_scroll = (scroll < scroll_max) ? scroll : scroll_max;
            for (int i = 0; i < 4; i++) ov_frame();
            if (s_gal_n > seen_max) seen_max = s_gal_n;
            seen_buttons = seen_buttons || gal_find("btn_long") != NULL;
            seen_sliders = seen_sliders || gal_find("seg_focus") != NULL;
            seen_toggles = seen_toggles || gal_find("seg_disabled") != NULL;
            seen_status = seen_status || gal_find("text_disabled") != NULL;
            TEST_ASSERT_TRUE(s_gal_n < GAL_RECT_MAX);
            const float pad = floorf(overlay_px(OVL_CARD_PAD));
            for (int i = 1; i < s_gal_n; i++) {
                const st_GAL_RECT *r = &s_gal[i];
                char msg[224];
                snprintf(msg, sizeof(msg), "%.0fx%.0f %s", sz[0], sz[1], r->name);
                TEST_ASSERT_TRUE_MESSAGE(r->max.x > r->min.x && r->max.y > r->min.y, msg);
                TEST_ASSERT_TRUE_MESSAGE(r->parent >= 0, msg);
                const st_GAL_RECT *p = &s_gal[r->parent];
                snprintf(msg, sizeof(msg), "%.0fx%.0f %s (%.1f %.1f %.1f %.1f) in %s (%.1f %.1f %.1f %.1f)", sz[0], sz[1], r->name, r->min.x, r->min.y, r->max.x,
                         r->max.y, p->name, p->min.x, p->min.y, p->max.x, p->max.y);
                if (r->parent == 0) {
                    /* karta v okně: vodorovně vždy; svisle jen tam, kde se nemá rolovat */
                    TEST_ASSERT_TRUE_MESSAGE(r->min.x >= p->min.x - 0.5f && r->max.x <= p->max.x + 0.5f, msg);
                    if (sz[0] >= 1280.0f) TEST_ASSERT_TRUE_MESSAGE(r->min.y >= p->min.y - 0.5f && r->max.y <= p->max.y + 0.5f, msg);
                } else {
                    /* prvek v kartě: uvnitř vnitřního okraje karty */
                    TEST_ASSERT_TRUE_MESSAGE(r->min.x >= p->min.x + pad - 1.0f && r->max.x <= p->max.x - pad + 1.0f, msg);
                    TEST_ASSERT_TRUE_MESSAGE(r->min.y >= p->min.y + pad - 1.0f && r->max.y <= p->max.y - pad + 1.0f, msg);
                }
                for (int j = 1; j < i; j++) {
                    if (s_gal[j].parent != r->parent) continue;
                    if (rects_overlap(r->min, r->max, s_gal[j].min, s_gal[j].max)) {
                        snprintf(msg, sizeof(msg), "%.0fx%.0f %s overlaps %s", sz[0], sz[1], r->name, s_gal[j].name);
                        TEST_FAIL_MESSAGE(msg);
                    }
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(seen_buttons && seen_sliders && seen_toggles && seen_status, "every card of the gallery was drawn and checked");
        if (sz[0] >= 1280.0f) TEST_ASSERT_TRUE_MESSAGE(seen_max > 60, "the gallery recorded its widgets");
        s_gal_scroll = 0.0f;
        for (int i = 0; i < 4; i++) ov_frame();
        /* dvousloupcové rozvržení se v úzkém okně skládá pod sebe */
        const st_GAL_RECT *a = gal_find("card_buttons"), *b = gal_find("card_toggles");
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_NULL(b);
        if (sz[0] >= 1280.0f) TEST_ASSERT_TRUE_MESSAGE(b->min.x >= a->max.x, "two columns in a wide window");
        else TEST_ASSERT_TRUE_MESSAGE(b->min.y >= a->max.y, "one column in a narrow window");
        /* velikosti rostou s jednotkou */
        const st_GAL_RECT *bt = gal_find("btn_eject");
        TEST_ASSERT_NOT_NULL(bt);
        TEST_ASSERT_FLOAT_WITHIN(1.5f, OVL_CONTROL_H * overlay_unit_for(ImVec2(sz[0], sz[1])), bt->max.y - bt->min.y);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.5f, bt->max.y - bt->min.y, bt->max.x - bt->min.x, "an icon-only button is a square");
    }

    /* popisek řádku zkrácený výpustkou má tooltip s plným zněním, nezkrácený ne (posudek N03) */
    gal_start(1280.0f, 720.0f);
    const st_GAL_RECT *lbl = gal_find("lbl_long");
    TEST_ASSERT_NOT_NULL(lbl);
    TEST_ASSERT_TRUE_MESSAGE(ov_hover_tooltip((lbl->min.x + lbl->max.x) * 0.5f, (lbl->min.y + lbl->max.y) * 0.5f), "a truncated row label shows a tooltip with the full text");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    for (int i = 0; i < 3; i++) ov_frame();
    lbl = gal_find("lbl_slider");
    TEST_ASSERT_NOT_NULL(lbl);
    TEST_ASSERT_FALSE_MESSAGE(ov_hover_tooltip((lbl->min.x + lbl->max.x) * 0.5f, (lbl->min.y + lbl->max.y) * 0.5f), "a row label that fits has no tooltip");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    for (int i = 0; i < 3; i++) ov_frame();
}

/* ================================================================
 * Ovládání widgetů
 * ================================================================ */

/** Tlačítko: klik a Enter / mezerník při fokusu ho zvolí; zakázané nereaguje. */
void test_button_mouse_and_keys(void)
{
    gal_start(1280.0f, 720.0f);
    gal_click("btn_change");
    TEST_ASSERT_EQUAL_INT(1, s_gs.clicks_change);
    gal_click("btn_disabled");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_gs.clicks_disabled, "a disabled button does not react to the mouse");
    gal_click("btn_long");
    TEST_ASSERT_EQUAL_INT(1, s_gs.clicks_long);

    gal_focus("btn_change");
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_gs.clicks_change, "Enter presses the focused button");
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, s_gs.clicks_change, "Space presses the focused button");
    /* šipka vpravo přejde na sousední tlačítko; původní už na Enter nereaguje */
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT(3, s_gs.clicks_change);
}

/** Přepínač mění hodnotu klikem i klávesou (mezerník, Enter); zakázaný ne. */
void test_toggle_mouse_and_keys(void)
{
    gal_start(1280.0f, 720.0f);
    TEST_ASSERT_FALSE(s_gs.toggle_off);
    gal_click("toggle_off");
    TEST_ASSERT_TRUE_MESSAGE(s_gs.toggle_off, "a click turns the toggle on");
    gal_click("toggle_off");
    TEST_ASSERT_FALSE_MESSAGE(s_gs.toggle_off, "a second click turns it off");
    gal_click("toggle_disabled");
    TEST_ASSERT_TRUE_MESSAGE(s_gs.toggle_disabled, "a disabled toggle does not change");

    gal_focus("toggle_on");
    TEST_ASSERT_TRUE(s_gs.toggle_on);
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_FALSE_MESSAGE(s_gs.toggle_on, "Space flips the focused toggle");
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE_MESSAGE(s_gs.toggle_on, "Enter flips the focused toggle");
    /* šipka nahoru přejde na přepínač o řádek výš */
    ov_key(ImGuiKey_UpArrow);
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_TRUE_MESSAGE(s_gs.toggle_off, "the arrow moves the focus to the toggle above");
    TEST_ASSERT_TRUE(s_gs.toggle_on);
}

/** Segmentová volba mění hodnotu klikem i klávesou (šipky mezi segmenty, Enter / mezerník volí). */
void test_segmented_mouse_and_keys(void)
{
    gal_start(1280.0f, 720.0f);
    const st_GAL_RECT *r = gal_find("seg");
    TEST_ASSERT_NOT_NULL(r);
    const float w = r->max.x - r->min.x, cy = (r->min.y + r->max.y) * 0.5f;
    TEST_ASSERT_EQUAL_INT(0, s_gs.seg);
    /* klik do pravého segmentu, potom do levého */
    ov_click(ImVec2(r->max.x - w * 0.1f, cy));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_gs.seg, "a click selects the segment");
    ov_click(ImVec2(r->min.x + w * 0.1f, cy));
    TEST_ASSERT_EQUAL_INT(0, s_gs.seg);

    /* fokus je po kliku na levém segmentu: šipka vpravo fokus posune, hodnotu nemění, Enter zvolí */
    ov_key(ImGuiKey_RightArrow);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_gs.seg, "moving the focus does not select");
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gs.seg, "Enter selects the focused segment");
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_gs.seg, "Space selects the focused segment");
    ov_key(ImGuiKey_LeftArrow);
    ov_key(ImGuiKey_LeftArrow);
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_INT(0, s_gs.seg);

    /* zakázaná volba se nemění */
    const st_GAL_RECT *d = gal_find("seg_disabled");
    TEST_ASSERT_NOT_NULL(d);
    ov_click(ImVec2(d->max.x - (d->max.x - d->min.x) * 0.1f, (d->min.y + d->max.y) * 0.5f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_gs.seg_disabled, "a disabled segmented control does not change");
}

/** Posuvník: klik do drážky a tažení nastaví hodnotu, šipky ji při fokusu mění o krok, Home a End na kraje; zakázaný ne. */
void test_slider_mouse_and_keys(void)
{
    gal_start(1280.0f, 720.0f);
    const st_GAL_RECT *r = gal_find("slider");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, s_gs.slider);
    const float cy = (r->min.y + r->max.y) * 0.5f;
    /* klik u levého okraje: nejmenší hodnota; tažení doprava hodnotu zvětší */
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(r->min.x + 2.0f, cy);
    ov_frame();
    io.AddMouseButtonEvent(0, true);
    ov_frame();
    ov_frame();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.0f, s_gs.slider, "a click at the left end gives the minimum");
    io.AddMousePosEvent(r->min.x + (r->max.x - r->min.x) * 0.4f, cy);
    ov_frame();
    ov_frame();
    const float dragged = s_gs.slider;
    TEST_ASSERT_TRUE_MESSAGE(dragged > 20.0f && dragged < 80.0f, "dragging changes the value");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, floorf(dragged + 0.5f), dragged, "the value snaps to the step");
    io.AddMouseButtonEvent(0, false);
    ov_frame();
    ov_frame();

    /* klávesnice: fokus je po kliku na posuvníku */
    ov_key(ImGuiKey_RightArrow);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, dragged + 1.0f, s_gs.slider, "Right adds one step");
    ov_key(ImGuiKey_LeftArrow);
    ov_key(ImGuiKey_LeftArrow);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, dragged - 1.0f, s_gs.slider, "Left subtracts one step");
    ov_key(ImGuiKey_End);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s_gs.slider);
    ov_key(ImGuiKey_RightArrow);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 100.0f, s_gs.slider, "the value stays in the range");
    ov_key(ImGuiKey_Home);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, s_gs.slider);
    /* šipka dolů fokus posune na další posuvník (krok 5); první se už nemění */
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_RightArrow);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, s_gs.slider);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 40.0f, s_gs.slider_focus, "the next slider moves by its own step");

    gal_click("slider_disabled");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 70.0f, s_gs.slider_disabled, "a disabled slider does not change");

    /* review V9: klik na číselný údaj vpravo hodnotu nemění (dřív nastavil maximum) */
    s_gs.slider = 40.0f;
    ov_frame();
    r = gal_find("slider");
    TEST_ASSERT_NOT_NULL(r);
    ov_click(ImVec2(r->max.x - 3.0f, (r->min.y + r->max.y) * 0.5f));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 40.0f, s_gs.slider, "a click on the number does not change the value");
    /* konec drážky těsně před údajem dál funguje */
    char num[32];
    snprintf(num, sizeof(num), "%.0f %%", 100.0f);
    ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_BODY));
    const float field_w = floorf(ImGui::CalcTextSize(num).x + 2.0f * overlay_px(8.0f));
    ImGui::PopFont();
    ov_click(ImVec2(r->max.x - field_w - 2.0f, (r->min.y + r->max.y) * 0.5f));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 100.0f, s_gs.slider, "a click at the right end of the track gives the maximum");

    /* review V8: zakázaný posuvník hodnotu mimo rozsah nepřepíše a změnu nehlásí */
    s_gs.slider_disabled = 150.0f;
    s_gs.slider_disabled_changes = 0;
    for (int i = 0; i < 3; i++) ov_frame();
    gal_click("slider_disabled");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 150.0f, s_gs.slider_disabled, "a disabled slider never writes the value");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_gs.slider_disabled_changes, "a disabled slider never reports a change");
    s_gs.slider_disabled = -20.0f;
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.0f, s_gs.slider_disabled);
    TEST_ASSERT_EQUAL_INT(0, s_gs.slider_disabled_changes);
}

/**
 * @brief Výška verzálky "H" v pixelech pro písmo a návrhovou velikost.
 * @param font Písmo. @param design Velikost (em) v návrhových bodech.
 * @return Výška kresby glyfu; 0, když glyf chybí.
 */
static float font_cap_height(ImFont *font, float design)
{
    const ImFontGlyph *glyph = font->GetFontBaked(overlay_font_size(font, design))->FindGlyphNoFallback((ImWchar)'H');
    return glyph ? glyph->Y1 - glyph->Y0 : 0.0f;
}

/**
 * Posudek výsledku R01 a R02: tokeny písma jsou velikosti em. ImGui bere
 * velikost jako výšku řádku, takže bez převodu vycházelo písmo o pětinu až
 * čtvrtinu menší (jméno "cpm-system.dsk" neproporcionálně 89 px místo asi
 * 118). Měří šířku známého řetězce a výšku verzálek proti tokenům.
 */
void test_font_sizes_match_tokens(void)
{
    char *inter = g_build_filename(MZTEST_FONT_DIR, "Inter-Regular.ttf", NULL);
    const bool have_files = MZTEST_FONT_DIR[0] && g_file_test(inter, G_FILE_TEST_IS_REGULAR) && overlay_font_mono() != overlay_font_ui();
    g_free(inter);
    if (!have_files) TEST_IGNORE_MESSAGE("overlay font files are not available");

    const float sizes[][2] = {{1280.0f, 720.0f}, {640.0f, 480.0f}};
    for (const float *sz : sizes) {
        overlay_style_set_viewport(ImVec2(sz[0], sz[1]));
        const float u = overlay_unit();
        const float em_body = (OVL_FONT_BODY * u < OVL_FONT_MIN_PX) ? OVL_FONT_MIN_PX : OVL_FONT_BODY * u;
        const float em_hint = (OVL_FONT_HINT * u < OVL_FONT_MIN_PX) ? OVL_FONT_MIN_PX : OVL_FONT_HINT * u;
        /* JetBrains Mono: krok znaku 0,6 em */
        ImGui::PushFont(overlay_font_mono(), overlay_font_size(overlay_font_mono(), OVL_FONT_BODY));
        const float name_w = ImGui::CalcTextSize("cpm-system.dsk").x;
        ImGui::PopFont();
        /* Inter: výška verzálek 0,727 em */
        const float cap_hint = font_cap_height(overlay_font_ui(), OVL_FONT_HINT);
        const float cap_body = font_cap_height(overlay_font_ui(), OVL_FONT_BODY);
        const float cap_title = font_cap_height(overlay_font_ui_bold(), OVL_FONT_CARD_TITLE);
        const float cap_value = font_cap_height(overlay_font_ui(), OVL_FONT_VALUE);
        printf("font sizes %.0fx%.0f: mono name %.1f px (em %.1f), caps hint %.1f body %.1f title %.1f value %.1f; ImGui sizes hint %.0f body %.0f mono %.0f\n", sz[0],
               sz[1], name_w, em_body, cap_hint, cap_body, cap_title, cap_value, overlay_font_px(OVL_FONT_HINT), overlay_font_px(OVL_FONT_BODY),
               overlay_font_size(overlay_font_mono(), OVL_FONT_BODY));
        /* tolerance: velikost ImGui se zaokrouhluje na celé pixely, kresba glyfu na pixely atlasu */
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.06f * 14.0f * 0.6f * em_body, 14.0f * 0.6f * em_body, name_w, "the monospace name has the width of its token size");
        TEST_ASSERT_TRUE_MESSAGE(cap_hint >= 0.727f * em_hint - 1.0f && cap_hint <= 0.727f * em_hint + 1.5f, "capital height of the hint text");
        TEST_ASSERT_TRUE_MESSAGE(cap_body >= 0.727f * em_body - 1.0f && cap_body <= 0.727f * em_body + 1.5f, "capital height of the body text");
        TEST_ASSERT_TRUE_MESSAGE(cap_hint >= 8.0f, "no text with capitals under 8 px");
        if (sz[0] >= 1280.0f) {
            TEST_ASSERT_TRUE_MESSAGE(name_w >= 113.0f, "the file name is at least 113 px wide at 1280 x 720");
            TEST_ASSERT_TRUE(cap_title >= 9.0f && cap_value >= 12.0f && cap_body >= 9.5f);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(name_w >= 97.0f, "the file name is at least 97 px wide at 640 x 480");
        }
    }
}

/** @brief Výsledky měření z review_widgets_draw(). */
static struct {
    ImU32 dis_text, dis_chip, dis_led, dis_icon; /**< barvy pasivních prvků uvnitř zakázaného stavu */
    ImU32 en_text, en_chip, en_led, en_icon;     /**< tytéž prvky mimo zakázaný stav */
    float avail;                                 /**< šířka obsahu úzkého místa */
    float seg_natural, seg_fill, seg_fixed;      /**< šířky segmentové volby o šesti segmentech (width 0, < 0, daná) */
    float fixed_w;                               /**< daná šířka pro seg_fixed a slider_fixed */
    float slider_fill, slider_fixed;             /**< šířky posuvníku (zbývající šířka, daná šířka) */
} s_rw;

/**
 * @brief Jeden snímek s oknem ve stylu overlaye: pasivní prvky v zakázaném stavu a prvky v úzkém místě (review V6, V10).
 * @post s_rw nese změřené barvy a šířky.
 */
static void review_widgets_frame(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
    ImGui::NewFrame();
    overlay_style_set_viewport(ImVec2(1280.0f, 720.0f));
    overlay_style_push();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
    if (ImGui::Begin("###OverlayReviewWidgets", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        /* V6: uvnitř zakázaného stavu barva zakázaného textu bez ohledu na barvu, kterou prvek dostal */
        for (int pass = 0; pass < 2; pass++) {
            const bool dis = (pass == 0);
            if (dis) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_AMBER);
            ovl_text_ellipsis("Unsaved changes", 0.0f);
            ImGui::PopStyleColor();
            (dis ? s_rw.dis_text : s_rw.en_text) = ovl_test_last_passive_color();
            ovl_status_chip_icon(ICON_LC_PAUSE, "Paused", OVL_STATUS_WARNING);
            (dis ? s_rw.dis_chip : s_rw.en_chip) = ovl_test_last_passive_color();
            ovl_led(true);
            (dis ? s_rw.dis_led : s_rw.en_led) = ovl_test_last_passive_color();
            ovl_icon(ICON_LC_SAVE, OVL_COL_AMBER);
            (dis ? s_rw.dis_icon : s_rw.en_icon) = ovl_test_last_passive_color();
            if (dis) ImGui::EndDisabled();
        }

        /* V10: úzké místo (užší než šest čtvercových segmentů a než dřívější nejmenší šířka posuvníku) */
        ImGui::BeginChild("review_narrow", ImVec2(overlay_px(110.0f), overlay_px(300.0f)));
        s_rw.avail = ImGui::GetContentRegionAvail().x;
        s_rw.fixed_w = floorf(overlay_px(100.0f));
        static const char *const six[] = {"Small", "Medium", "Large", "Huge", "Full screen", "Custom"};
        static int sel = 0;
        ovl_segmented("review_seg_natural", six, 6, &sel);
        s_rw.seg_natural = ImGui::GetItemRectSize().x;
        ovl_segmented_ex("review_seg_fill", six, NULL, 6, &sel, -1.0f);
        s_rw.seg_fill = ImGui::GetItemRectSize().x;
        ovl_segmented_ex("review_seg_fixed", six, NULL, 6, &sel, s_rw.fixed_w);
        s_rw.seg_fixed = ImGui::GetItemRectSize().x;
        static float value = 50.0f;
        ovl_slider_percent("review_slider_fill", &value, 0.0f, 100.0f);
        s_rw.slider_fill = ImGui::GetItemRectSize().x;
        ovl_slider_percent_ex("review_slider_fixed", &value, 0.0f, 100.0f, 1.0f, s_rw.fixed_w);
        s_rw.slider_fixed = ImGui::GetItemRectSize().x;
        ImGui::EndChild();
    }
    ImGui::End();
    overlay_style_pop();
    ImGui::Render();
}

/**
 * Opravy widgetů po review O2: pasivní prvky v zakázaném stavu (V6), tooltip
 * tlačítka bez řezání uprostřed znaku UTF-8 (V7), segmentová volba a posuvník
 * v úzkém místě (V10).
 */
void test_review_widget_fixes(void)
{
    memset(&s_rw, 0, sizeof(s_rw));
    for (int i = 0; i < 3; i++) review_widgets_frame();

    /* V6 */
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(OVL_COL_TEXT_DISABLED, s_rw.dis_text, "disabled text ignores the pushed colour");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(OVL_COL_TEXT_DISABLED, s_rw.dis_chip, "a disabled status chip loses its status colour");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(OVL_COL_MUTED, s_rw.dis_led, "a disabled LED is drawn as off");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(OVL_COL_TEXT_DISABLED, s_rw.dis_icon, "a disabled icon ignores its colour");
    TEST_ASSERT_EQUAL_HEX32(OVL_COL_AMBER, s_rw.en_text);
    TEST_ASSERT_EQUAL_HEX32(OVL_COL_AMBER, s_rw.en_chip);
    TEST_ASSERT_EQUAL_HEX32(OVL_COL_GREEN, s_rw.en_led);
    TEST_ASSERT_EQUAL_HEX32(OVL_COL_AMBER, s_rw.en_icon);

    /* V7: popisek 300 dvoubajtových znaků (600 bajtů, dřív se řezal na 511) zůstane celý a platný */
    std::string label;
    for (int i = 0; i < 300; i++) label += "\xC5\x99"; /* ř */
    const std::string with_id = label + "##review_id";
    const char *label_end = ImGui::FindRenderedTextEnd(with_id.c_str());
    const std::string tip = ovl_button_tooltip_text(with_id.c_str(), label_end, "Alt+F8");
    TEST_ASSERT_TRUE_MESSAGE(g_utf8_validate(tip.c_str(), -1, NULL), "the tooltip is valid UTF-8");
    TEST_ASSERT_TRUE(tip == label + " (Alt+F8)");
    TEST_ASSERT_TRUE(ovl_button_tooltip_text(with_id.c_str(), label_end, NULL) == label);
    TEST_ASSERT_TRUE(ovl_button_tooltip_text(NULL, NULL, "F12") == " (F12)");

    /* V10: nic nepřesáhne zbývající ani danou šířku */
    TEST_ASSERT_TRUE(s_rw.avail > 0.0f && s_rw.fixed_w > 0.0f && s_rw.fixed_w <= s_rw.avail);
    TEST_ASSERT_TRUE_MESSAGE(s_rw.avail < 6.0f * floorf(overlay_px(OVL_CONTROL_H)), "the place is narrower than six square segments");
    TEST_ASSERT_TRUE_MESSAGE(s_rw.seg_natural <= s_rw.avail + 0.5f, "a segmented control of natural width fits the narrow place");
    TEST_ASSERT_TRUE_MESSAGE(s_rw.seg_fill <= s_rw.avail + 0.5f, "a segmented control over the remaining width fits");
    TEST_ASSERT_TRUE_MESSAGE(s_rw.seg_fixed <= s_rw.fixed_w + 0.5f, "a segmented control keeps the given width");
    TEST_ASSERT_TRUE(s_rw.seg_natural > 6.0f && s_rw.seg_fixed > 6.0f);
    TEST_ASSERT_TRUE_MESSAGE(s_rw.slider_fill <= s_rw.avail + 0.5f, "a slider over the remaining width fits the narrow place");
    TEST_ASSERT_TRUE_MESSAGE(s_rw.slider_fixed <= s_rw.fixed_w + 0.5f, "a slider keeps the given width");
    TEST_ASSERT_TRUE(s_rw.slider_fixed > 0.0f);
}

/**
 * Review V5: písma overlaye přidaná za písma aplikace v pořadí z
 * myimgui_set_fonts(): základní písmo se sloučenými japonskými znaky a
 * znaky Sharp (mzglyphs), neproporcionální Cousine s mzglyphs a teprve pak
 * písma overlaye. Písma overlaye se nesmí sloučit do Cousine a ikony Lucide
 * se nesmí splést s mzglyphs na stejných kódových bodech. Ikony souborového
 * dialogu (vestavěná data aplikace) test nepřidává.
 */
void test_fonts_production_order(void)
{
    ImGuiIO &io = ImGui::GetIO();
    char *droid = g_build_filename(MZTEST_FONT_DIR, "DroidSans.ttf", NULL);
    char *cjk = g_build_filename(MZTEST_FONT_DIR, "NotoSansJP-Regular.ttf", NULL);
    char *cousine = g_build_filename(MZTEST_FONT_DIR, "Cousine-Regular.ttf", NULL);
    char *mzglyphs = g_build_filename(MZTEST_FONT_DIR, "..", "symbols", "mzglyphs.ttf", NULL);
    char *inter = g_build_filename(MZTEST_FONT_DIR, "Inter-Regular.ttf", NULL);
    const bool have_files = MZTEST_FONT_DIR[0] && g_file_test(droid, G_FILE_TEST_IS_REGULAR) && g_file_test(cousine, G_FILE_TEST_IS_REGULAR)
                            && g_file_test(mzglyphs, G_FILE_TEST_IS_REGULAR) && g_file_test(inter, G_FILE_TEST_IS_REGULAR);
    ImFont *base = NULL, *mono = NULL;
    if (have_files) {
        /* nastavení, velikosti a rozsahy jako myimgui_set_fonts() */
        static const ImWchar i18n_ranges[] = {0x0020, 0x024F, 0x0400, 0x04FF, 0};
        static const ImWchar mz_ranges[] = {0xE000, 0xE000, 0xE100, 0xE4FF, 0};
        static const ImWchar mono_ranges[] = {0x0020, 0x024F, 0};
        ImFontConfig base_cfg;
        base_cfg.OversampleH = 3;
        base_cfg.OversampleV = 3;
        base_cfg.PixelSnapH = true;
        base = io.Fonts->AddFontFromFileTTF(droid, 28.0f, &base_cfg, i18n_ranges);
        if (g_file_test(cjk, G_FILE_TEST_IS_REGULAR)) {
            ImFontConfig cjk_cfg;
            cjk_cfg.MergeMode = true;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.PixelSnapH = true;
            overlay_style_add_shared_font(io.Fonts, cjk, 28.0f, &cjk_cfg, io.Fonts->GetGlyphRangesJapanese());
        }
        ImFontConfig mz_cfg;
        mz_cfg.MergeMode = true;
        mz_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(mzglyphs, 20.0f, &mz_cfg, mz_ranges);
        ImFontConfig mono_cfg;
        mono_cfg.OversampleH = 2;
        mono_cfg.OversampleV = 2;
        mono_cfg.PixelSnapH = true;
        mono = io.Fonts->AddFontFromFileTTF(cousine, 28.0f, &mono_cfg, mono_ranges);
        ImFontConfig mono_mz_cfg;
        mono_mz_cfg.MergeMode = true;
        mono_mz_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(mzglyphs, 20.0f, &mono_mz_cfg, mz_ranges);
    }
    g_free(droid);
    g_free(cjk);
    g_free(cousine);
    g_free(mzglyphs);
    g_free(inter);
    if (!have_files) TEST_IGNORE_MESSAGE("application or overlay font files are not available");
    TEST_ASSERT_NOT_NULL(base);
    TEST_ASSERT_NOT_NULL(mono);
    const int base_sources = base->Sources.Size;
    TEST_ASSERT_EQUAL_INT(2, mono->Sources.Size);

    TEST_ASSERT_TRUE(overlay_style_load_fonts(io.Fonts, MZTEST_FONT_DIR));
    /* písma overlaye jsou samostatná a do písem aplikace nic nepřidala */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, mono->Sources.Size, "nothing is merged into the monospace font of the application");
    TEST_ASSERT_EQUAL_INT(base_sources, base->Sources.Size);
    ImFont *const ovl[] = {overlay_font_ui(), overlay_font_ui_bold(), overlay_font_mono()};
    for (ImFont *f : ovl) {
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_TRUE(f != base && f != mono);
    }
    TEST_ASSERT_TRUE(overlay_style_has_icons());
    TEST_ASSERT_NOT_NULL(strstr(overlay_font_ui()->Sources[0]->Name, "Inter-Regular"));
    TEST_ASSERT_NOT_NULL(strstr(overlay_font_mono()->Sources[0]->Name, "JetBrainsMono"));
    /* kódový bod U+E14D: v písmech aplikace znak Sharp z mzglyphs, v písmu overlaye ikona Lucide */
    const ImFontGlyph *g_ovl = overlay_font_ui()->GetFontBaked(20.0f)->FindGlyphNoFallback((ImWchar)0xE14D);
    TEST_ASSERT_NOT_NULL(g_ovl);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(overlay_font_ui()->Sources[g_ovl->SourceIdx]->Name, "lucide"), "the overlay icon comes from lucide.ttf");
    const ImFontGlyph *g_mono = mono->GetFontBaked(20.0f)->FindGlyphNoFallback((ImWchar)0xE14D);
    if (g_mono) TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mono->Sources[g_mono->SourceIdx]->Name, "mzglyphs"), "the application glyph stays a Sharp character");
    const ImFontGlyph *g_base = base->GetFontBaked(20.0f)->FindGlyphNoFallback((ImWchar)0xE14D);
    if (g_base) TEST_ASSERT_NOT_NULL(strstr(base->Sources[g_base->SourceIdx]->Name, "mzglyphs"));
    /* neproporcionální písmo overlaye znaky Sharp ani ikony nenese (DECISIONS, review V3) */
    TEST_ASSERT_NULL(overlay_font_mono()->GetFontBaked(20.0f)->FindGlyphNoFallback((ImWchar)0xE100));
}

/**
 * @brief Zkopíruje soubor písma z MZTEST_FONT_DIR do adresáře.
 * @param dir Cílový adresář. @param file Jméno souboru.
 * @return true = soubor byl zkopírován.
 */
static bool font_copy(const char *dir, const char *file)
{
    char *src = g_build_filename(MZTEST_FONT_DIR, file, NULL);
    char *dst = g_build_filename(dir, file, NULL);
    gchar *data = NULL;
    gsize len = 0;
    const bool ok = g_file_get_contents(src, &data, &len, NULL) && g_file_set_contents(dst, data, (gssize)len, NULL);
    g_free(data);
    g_free(src);
    g_free(dst);
    return ok;
}

/**
 * Review V5: chybí jen Inter-Regular.ttf. Základní řez padá na písmo
 * aplikace, ikony se nekreslí (kreslí je základní řez), tučný a
 * neproporcionální řez se načtou; overlay i galerie kreslí bez pádu a s
 * vyváženými zásobníky ImGui.
 */
void test_missing_regular_font(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImFont *base = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
    char *probe = g_build_filename(MZTEST_FONT_DIR, "Inter-SemiBold.ttf", NULL);
    const bool have_files = MZTEST_FONT_DIR[0] && g_file_test(probe, G_FILE_TEST_IS_REGULAR);
    g_free(probe);
    if (!have_files) TEST_IGNORE_MESSAGE("overlay font files are not available");

    static const char *const files[] = {"Inter-SemiBold.ttf", "JetBrainsMono-Regular.ttf", "lucide.ttf"};
    char *dir = g_dir_make_tmp("mz800-overlay-fonts-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(dir);
    bool copied = true;
    for (const char *f : files) copied = font_copy(dir, f) && copied;

    const bool loaded = copied && overlay_style_load_fonts(io.Fonts, dir);
    ImFont *const ui = overlay_font_ui(), *const bold = overlay_font_ui_bold(), *const mono = overlay_font_mono();
    const bool icons = overlay_style_has_icons();
    int stacks = 0;
    bool drawn = false;
    if (copied) {
        gal_start(1280.0f, 720.0f);
        drawn = gal_find("btn_save") != NULL;
        imgui_overlay_open();
        ImGuiContext &g = *GImGui;
        for (int i = 0; i < 3; i++) {
            ov_frame();
            stacks += g.FontStack.Size + g.ColorStack.Size + g.StyleVarStack.Size;
        }
        drawn = drawn && imgui_overlay_test_item_rect("tab5", NULL, NULL) && imgui_overlay_test_item_rect("close", NULL, NULL);
        imgui_overlay_close();
        s_gallery = false;
        ov_frame();
    }
    /* úklid a zpět písma overlaye pro další testy, ještě před vyhodnocením */
    for (const char *f : files) {
        char *p = g_build_filename(dir, f, NULL);
        g_remove(p);
        g_free(p);
    }
    g_rmdir(dir);
    g_free(dir);
    overlay_style_load_fonts(io.Fonts, MZTEST_FONT_DIR);

    TEST_ASSERT_TRUE(copied);
    TEST_ASSERT_FALSE_MESSAGE(loaded, "an incomplete set of fonts is reported");
    TEST_ASSERT_TRUE_MESSAGE(ui == base, "the regular cut falls back to the application font");
    TEST_ASSERT_TRUE_MESSAGE(bold != base && bold != NULL, "the bold cut is loaded");
    TEST_ASSERT_TRUE_MESSAGE(mono != base && mono != NULL, "the monospace cut is loaded");
    TEST_ASSERT_FALSE_MESSAGE(icons, "no icons without the regular cut");
    TEST_ASSERT_TRUE_MESSAGE(drawn, "the gallery and the overlay draw");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, stacks, "balanced ImGui stacks");
}

/** Text o 200 znacích: položka nepřeteče danou šířku ani kartu; krátký text se nezkracuje. */
void test_text_ellipsis(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {2560.0f, 1440.0f}};
    for (const float *sz : sizes) {
        gal_start(sz[0], sz[1]);
        TEST_ASSERT_NOT_NULL(gal_seek("text_disabled"));
        const st_GAL_RECT *card = gal_find("card_status");
        const st_GAL_RECT *lng = gal_find("text_long");
        const st_GAL_RECT *fixed = gal_find("text_fixed");
        const st_GAL_RECT *shrt = gal_find("text_short");
        TEST_ASSERT_NOT_NULL(card);
        TEST_ASSERT_NOT_NULL(lng);
        TEST_ASSERT_NOT_NULL(fixed);
        TEST_ASSERT_NOT_NULL(shrt);
        const float inner = (card->max.x - card->min.x) - 2.0f * floorf(overlay_px(OVL_CARD_PAD));
        /* zbývající šířka: text vyplní kartu a nepřeteče */
        TEST_ASSERT_TRUE(lng->max.x - lng->min.x <= inner + 0.5f);
        TEST_ASSERT_TRUE_MESSAGE(lng->max.x - lng->min.x >= inner - 2.0f, "the truncated text uses the whole width");
        /* daná šířka */
        TEST_ASSERT_TRUE(fixed->max.x - fixed->min.x <= overlay_px(120.0f) + 0.5f);
        TEST_ASSERT_TRUE(fixed->max.x - fixed->min.x >= overlay_px(120.0f) - 1.5f);
        /* krátký text má přirozenou šířku pod mezí */
        TEST_ASSERT_TRUE(shrt->max.x - shrt->min.x < overlay_px(240.0f) - 1.0f);

        /* dlouhý popisek tlačítka a řádku zůstane ve svém místě (kartu kontroluje test_widgets_geometry) */
        TEST_ASSERT_NOT_NULL(gal_seek("seg_focus"));
        const st_GAL_RECT *lbl = gal_find("lbl_long");
        const st_GAL_RECT *seg = gal_find("seg_focus");
        TEST_ASSERT_NOT_NULL(lbl);
        TEST_ASSERT_NOT_NULL(seg);
        TEST_ASSERT_TRUE_MESSAGE(lbl->max.x <= seg->min.x, "a long row label ends before the control");
    }
}

/** Chybějící soubory písem: overlay i widgety kreslí základním písmem a bez ikon, nic nespadne. */
void test_missing_fonts_fallback(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImFont *base = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
    TEST_ASSERT_FALSE(overlay_style_load_fonts(io.Fonts, "no-such-overlay-font-dir"));
    TEST_ASSERT_TRUE(overlay_font_ui() == base);
    TEST_ASSERT_TRUE(overlay_font_ui_bold() == base);
    TEST_ASSERT_TRUE(overlay_font_mono() == base);
    TEST_ASSERT_FALSE(overlay_style_has_icons());
    TEST_ASSERT_FALSE(overlay_style_load_fonts(io.Fonts, NULL));
    TEST_ASSERT_FALSE(overlay_style_load_fonts(NULL, MZTEST_FONT_DIR));

    gal_start(1280.0f, 720.0f);
    TEST_ASSERT_NOT_NULL(gal_find("btn_save"));
    imgui_overlay_open();
    ImGuiContext &g = *GImGui;
    for (int i = 0; i < 3; i++) {
        ov_frame();
        TEST_ASSERT_EQUAL_INT(0, g.FontStack.Size);
        TEST_ASSERT_EQUAL_INT(0, g.ColorStack.Size);
        TEST_ASSERT_EQUAL_INT(0, g.StyleVarStack.Size);
    }
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("tab5", NULL, NULL));
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("close", NULL, NULL));
    imgui_overlay_close();
    s_gallery = false;
    ov_frame();

    /* zpět písma overlaye pro další testy; sdílené soubory se z disku nečtou znovu (review V2) */
    const int loads = overlay_style_test_font_file_loads();
    if (MZTEST_FONT_DIR[0]) overlay_style_load_fonts(io.Fonts, MZTEST_FONT_DIR);
    TEST_ASSERT_EQUAL_INT(loads, overlay_style_test_font_file_loads());
}

/* ================================================================
 * Tab Media
 * ================================================================ */

/** @brief Kopie stavu, ze které ov_frame() kreslí při s_fixture (media_start()). */
static st_OVERLAY_VIEW s_fixture_view;

/** @brief Jména testovacích stavů pro zprávy a soubory snímků (index en_OVERLAY_FIXTURE). */
static const char *const s_fixture_names[OVERLAY_FIXTURE_COUNT] = {"default", "empty", "long", "replay", "nofdc"};

/**
 * @brief Začne kreslit tab Media z testovacího stavu v dané velikosti viewportu a nechá ustálit rozvržení.
 * @param f Testovací stav. @param w Šířka viewportu. @param h Výška viewportu.
 */
static void media_start(en_OVERLAY_FIXTURE f, float w, float h)
{
    overlay_view_make_fixture(&s_fixture_view, f);
    s_fixture = &s_fixture_view;
    s_display = ImVec2(w, h);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    for (int i = 0; i < 4; i++) ov_frame();
}

/** @brief Obdélník prvku tabu podle jména; false = v posledním kreslení nebyl. */
static bool media_rect(const char *name, ImVec2 *mn, ImVec2 *mx)
{
    return imgui_overlay_test_item_rect(name, mn, mx);
}

/**
 * @brief Rolované okno obsahu tabu; NULL = nekreslilo se.
 *
 * Běžně plocha mezi horní lištou a patičkou; tab Display v širokém okně
 * roluje jen sloupec karet (vnořené okno "ovl_disp_col"), a to má přednost.
 */
static ImGuiWindow *media_content_window(void)
{
    ImGuiContext &g = *GImGui;
    ImGuiWindow *found = NULL;
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow *w = g.Windows[i];
        if (!w->WasActive || !strstr(w->Name, "ovl_content") || strstr(w->Name, "ovl_card")) continue;
        if (strstr(w->Name, "ovl_disp_col")) return w;
        if (!found) found = w;
    }
    return found;
}

/** @brief Vrátí na začátek všechny rolované plochy obsahu tabů (i ty, které se právě nekreslí). */
static void media_content_scroll_reset(void)
{
    ImGuiContext &g = *GImGui;
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow *w = g.Windows[i];
        if (strstr(w->Name, "ovl_content") && !strstr(w->Name, "ovl_card")) ImGui::SetScrollY(w, 0.0f);
    }
}

/**
 * @brief Zkontroluje skupinu sourozenců: každý vykreslený leží v rodiči (s vnitřním okrajem) a žádné dva se nepřekrývají.
 * @param msg Popis situace do zprávy. @param parent Jméno rodiče. @param inset Vnitřní okraj rodiče v pixelech.
 * @param names Jména sourozenců. @param count Počet jmen.
 * @return Počet vykreslených sourozenců.
 */
static int media_check_group(const char *msg, const char *parent, float inset, const char *const *names, int count)
{
    ImVec2 pmin, pmax;
    if (!media_rect(parent, &pmin, &pmax)) return 0;
    ImVec2 mn[40], mx[40];
    bool drawn[40];
    int n = 0;
    TEST_ASSERT_TRUE(count <= 40);
    for (int i = 0; i < count; i++) {
        drawn[i] = media_rect(names[i], &mn[i], &mx[i]);
        if (!drawn[i]) continue;
        n++;
        char m[320];
        snprintf(m, sizeof(m), "%s: %s (%.1f %.1f %.1f %.1f) in %s (%.1f %.1f %.1f %.1f)", msg, names[i], mn[i].x, mn[i].y, mx[i].x, mx[i].y, parent, pmin.x, pmin.y,
                 pmax.x, pmax.y);
        TEST_ASSERT_TRUE_MESSAGE(mx[i].x > mn[i].x && mx[i].y > mn[i].y, m);
        TEST_ASSERT_TRUE_MESSAGE(mn[i].x >= pmin.x + inset - 1.0f && mx[i].x <= pmax.x - inset + 1.0f, m);
        TEST_ASSERT_TRUE_MESSAGE(mn[i].y >= pmin.y + inset - 1.0f && mx[i].y <= pmax.y - inset + 1.0f, m);
        for (int j = 0; j < i; j++) {
            if (!drawn[j] || !rects_overlap(mn[i], mx[i], mn[j], mx[j])) continue;
            snprintf(m, sizeof(m), "%s: %s overlaps %s", msg, names[i], names[j]);
            TEST_FAIL_MESSAGE(m);
        }
    }
    return n;
}

/**
 * @brief Geometrie tabu Media v posledním kreslení: karty v ploše obsahu, prvky v kartách, nic se nepřekrývá.
 * @param msg Popis situace do zprávy. @param all true = všechny karty musí být vidět celé (okno, které se neroluje).
 */
static void media_check_geometry(const char *msg, bool all)
{
    static const char *const cards[] = {"card_fdd", "card_cmt", "card_qd", "card_recent", "card_actions"};
    static const char *const rows[] = {"fdd0", "fdd1", "fdd2", "fdd3"};
    static const char *const cmt[] = {"cmt_name", "cmt_modified", "cmt_progress", "cmt_block", "cmt_time", "cmt_rewind", "cmt_play",
                                      "cmt_pause", "cmt_stop",    "cmt_record",   "cmt_mode",  "cmt_change", "cmt_window"};
    static const char *const qd[] = {"qd_name", "qd_unsaved", "qd_insert", "qd_save", "qd_eject"};
    /* pruh nedávných médií: text prázdného stavu nebo položky, vždy tlačítko */
    static char chip_names[OVERLAY_VIEW_RECENT_MAX][20];
    static const char *recent[3 + OVERLAY_VIEW_RECENT_MAX];
    recent[0] = "recent_empty";
    recent[1] = "recent_browse";
    recent[2] = "recent_clear";
    for (int i = 0; i < OVERLAY_VIEW_RECENT_MAX; i++) {
        snprintf(chip_names[i], sizeof(chip_names[i]), "recent_chip%d", i);
        recent[3 + i] = chip_names[i];
    }
    static const char *const acts[] = {"act_resume", "act_speed", "act_quick_save", "act_quick_load", "act_screenshot", "act_fullscreen", "act_reset"};
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    char m[320];

    /* karty: vodorovně uvnitř okna s okrajem obsahu, navzájem se nepřekrývají */
    ImVec2 mn[5], mx[5];
    bool drawn[5];
    const float margin = floorf(overlay_px(OVL_MARGIN));
    for (int i = 0; i < 5; i++) {
        drawn[i] = media_rect(cards[i], &mn[i], &mx[i]);
        snprintf(m, sizeof(m), "%s: %s", msg, cards[i]);
        TEST_ASSERT_TRUE_MESSAGE(drawn[i], m);
        TEST_ASSERT_TRUE_MESSAGE(mn[i].x >= vp->Pos.x + margin - 1.0f && mx[i].x <= vp->Pos.x + vp->Size.x - margin + 1.0f, m);
        TEST_ASSERT_TRUE_MESSAGE(mx[i].x > mn[i].x && mx[i].y > mn[i].y, m);
        for (int j = 0; j < i; j++) {
            snprintf(m, sizeof(m), "%s: %s overlaps %s", msg, cards[i], cards[j]);
            TEST_ASSERT_FALSE_MESSAGE(rects_overlap(mn[i], mx[i], mn[j], mx[j]), m);
        }
        if (all) {
            ImVec2 cmin, cmax;
            TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
            snprintf(m, sizeof(m), "%s: %s (%.1f to %.1f) in content (%.1f to %.1f)", msg, cards[i], mn[i].y, mx[i].y, cmin.y, cmax.y);
            TEST_ASSERT_TRUE_MESSAGE(mn[i].y >= cmin.y - 0.5f && mx[i].y <= cmax.y + 0.5f, m);
        }
    }

    /* řádky mechanik a jejich prvky */
    const int nrows = media_check_group(msg, "card_fdd", pad, rows, 4);
    if (all) TEST_ASSERT_EQUAL_INT_MESSAGE(4, nrows, msg);
    for (int d = 0; d < 4; d++) {
        char names[6][24];
        const char *ptrs[6];
        static const char *const parts[] = {"name", "state", "insert", "save", "eject", "keycap"};
        for (int k = 0; k < 6; k++) {
            snprintf(names[k], sizeof(names[k]), "fdd%d_%s", d, parts[k]);
            ptrs[k] = names[k];
        }
        const int n = media_check_group(msg, rows[d], 0.0f, ptrs, 6);
        if (all) TEST_ASSERT_TRUE_MESSAGE(n >= 2, msg); /* nejméně jméno a tlačítko */
    }
    const int ncmt = media_check_group(msg, "card_cmt", pad, cmt, 13);
    if (all) TEST_ASSERT_TRUE_MESSAGE(ncmt >= 10, msg);
    const int nqd = media_check_group(msg, "card_qd", pad, qd, 5);
    if (all) TEST_ASSERT_TRUE_MESSAGE(nqd >= 2, msg);
    const int nrecent = media_check_group(msg, "card_recent", pad, recent, 3 + OVERLAY_VIEW_RECENT_MAX);
    if (all) TEST_ASSERT_TRUE_MESSAGE(nrecent >= 2, msg);
    /* text prázdného stavu jen bez položek */
    if (all) TEST_ASSERT_TRUE_MESSAGE(media_rect("recent_empty", NULL, NULL) != media_rect("recent_chip0", NULL, NULL), msg);
    const int nacts = media_check_group(msg, "card_actions", pad, acts, 7);
    if (all) TEST_ASSERT_EQUAL_INT_MESSAGE(7, nacts, msg);
}

/** @brief Vyprázdní fronty požadavků a počítadla stubů (nic z předchozího kroku testu). */
static void media_drain(void)
{
    device_request_clear();
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    overlay_actions_test_take_last();
    s_quicksave_calls = s_quickload_calls = s_screenshot_calls = s_fullscreen_calls = s_tape_action_calls = 0;
    s_tape_action_last = IMGUI_CMT_TAPE_ACTION_NONE;
}

/** @brief Klikne do středu prvku tabu Media podle jména. */
static void media_click(const char *name)
{
    ImVec2 mn, mx;
    TEST_ASSERT_TRUE_MESSAGE(media_rect(name, &mn, &mx), name);
    ov_click(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));
}

/** @brief Klikne do vodorovného zlomku prvku tabu Media (0 = levý okraj, 1 = pravý). */
static void media_click_at(const char *name, float fx)
{
    ImVec2 mn, mx;
    TEST_ASSERT_TRUE_MESSAGE(media_rect(name, &mn, &mx), name);
    ov_click(ImVec2(mn.x + (mx.x - mn.x) * fx, (mn.y + mx.y) * 0.5f));
}

/**
 * @brief Stiskne a pustí klávesu s drženými modifikátory.
 * @param key Klávesa. @param mods Kombinace ImGuiMod_Alt, ImGuiMod_Shift a ImGuiMod_Ctrl.
 */
static void ov_mod_key(ImGuiKey key, int mods)
{
    ImGuiIO &io = ImGui::GetIO();
    if (mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, true);
    if (mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    if (mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
    ov_frame();
    io.AddKeyEvent(key, true);
    ov_frame();
    io.AddKeyEvent(key, false);
    ov_frame();
    if (mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, false);
    if (mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    if (mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    ov_frame();
}

/** @brief Zapíše malý DSK (2 stopy, 16 x 256 B) do souboru (jako tests/peripherals/test_fdc_request.c). */
static void write_test_dsk(const char *path)
{
    st_HANDLER h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1024));
    size_t desc_size = dsk_tools_compute_description_size(1);
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *)g_malloc0(desc_size);
    desc->tracks = 2;
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description(desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, dsk_tools_create_image(&h, desc));
    g_free(desc);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
}

/** @brief Cesta testovacího DSK v dočasném adresáři testu (vytvoří ho při prvním volání). */
static const char *media_test_dsk(void)
{
    if (!s_tmpdir) s_tmpdir = g_dir_make_tmp("mz-overlay-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_tmpdir);
    if (!s_dsk) s_dsk = g_build_filename(s_tmpdir, "overlay-test.dsk", NULL);
    if (!g_file_test(s_dsk, G_FILE_TEST_EXISTS)) write_test_dsk(s_dsk);
    return s_dsk;
}

/** @brief Vynuluje počítadla stubů tabu Capture a příznaky oken, která otevírají taby Input a Capture. */
static void o6_drain(void)
{
    s_shot_as_calls = s_series_calls = s_shot_folder_calls = 0;
    s_video_toggle_calls = s_video_pause_calls = s_video_timebase_calls = s_video_folder_calls = 0;
    s_replay_record_calls = s_replay_play_calls = s_replay_request_calls = 0;
    s_replay_dialog_overlay_open = false;
    s_gui.showVirtualKeyboardWindow = false;
    s_gui.showAutotypeWindow = false;
    s_gui.showJoystickSetupWindow = false;
    s_gui.showScreenshotSetupWindow = false;
    s_gui.showVideorecSetupWindow = false;
    s_gui.showReplayWindow = false;
}

/**
 * @brief Počká, až pracovní vlákno zjišťování existence souborů historie doběhne (nejvýš 5 s).
 * @return true = vlákno doběhlo (výsledek může čekat na vyzvednutí).
 */
static bool recent_wait_probe(void)
{
    for (int i = 0; i < 5000; i++) {
        /* busy je i nevyzvednutý výsledek: vyzvednout ho smí jen plnění */
        static st_OVERLAY_VIEW scratch;
        if (!overlay_view_recent_busy()) return true;
        overlay_view_fill_recent(&scratch, false);
        if (!overlay_view_recent_busy()) return true;
        g_usleep(1000);
    }
    return false;
}

/**
 * @brief Vyplní kopii ze skutečného jádra pod zámkem a doplní nedávná média s ustáleným stavem existence souborů.
 *
 * Nedávná média plní mimo zámek jako imgui_overlay_frame(); na pracovní
 * vlákno počká (test), UI na ně nečeká nikdy.
 */
static void media_fill(st_OVERLAY_VIEW *v)
{
    device_state_lock();
    overlay_view_fill(v);
    device_state_unlock();
    TEST_ASSERT_FALSE_MESSAGE(device_state_lock_held(), "recent media are filled outside the device lock");
    overlay_view_fill_capture(v);
    overlay_view_fill_recent(v, true);
    TEST_ASSERT_TRUE(recent_wait_probe());
    overlay_view_fill_recent(v, true);
}

/** @brief Snímky overlaye nad skutečným jádrem, dokud se stav existence souborů historie neustálí (žádost, výsledek, kreslení). */
static void ov_frames_settled(void)
{
    overlay_view_recent_recheck();
    ov_frame();
    TEST_ASSERT_TRUE(recent_wait_probe());
    ov_frame();
    ov_frame();
}

/**
 * Review V1: oddělovací linka nadpisu karty se kreslí až k okrajům karty,
 * ale ne mimo výřez rolovaného obsahu tabu. V malém okně se obsah roluje;
 * při žádném rolování nesmí žádný příkaz kreslení karty mít ořez mimo
 * plochu obsahu (dřív linka přejela horní lištu nebo patičku).
 */
void test_card_title_clip(void)
{
    media_start(OVERLAY_FIXTURE_DEFAULT, 640.0f, 480.0f);
    ImGuiWindow *content = media_content_window();
    TEST_ASSERT_NOT_NULL(content);
    const float scroll_max = content->ScrollMax.y;
    TEST_ASSERT_TRUE_MESSAGE(scroll_max > 50.0f, "the tab scrolls in a small window");
    int checked = 0, cards_cut = 0;
    for (float scroll = 0.0f; scroll <= scroll_max + 1.0f; scroll += 7.0f) {
        ImGui::SetScrollY(content, (scroll < scroll_max) ? scroll : scroll_max);
        for (int i = 0; i < 2; i++) ov_frame();
        ImVec2 cmin, cmax;
        TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
        ImGuiContext &g = *GImGui;
        for (int w = 0; w < g.Windows.Size; w++) {
            ImGuiWindow *card = g.Windows[w];
            if (!card->WasActive || !strstr(card->Name, "ovl_card")) continue;
            /* karta přesahující plochu obsahu: právě u ní linka dřív přetekla */
            if (card->Pos.y < cmin.y - 0.5f || card->Pos.y + card->Size.y > cmax.y + 0.5f) cards_cut++;
            for (int c = 0; c < card->DrawList->CmdBuffer.Size; c++) {
                const ImDrawCmd &cmd = card->DrawList->CmdBuffer[c];
                if (cmd.ElemCount == 0) continue;
                char msg[200];
                snprintf(msg, sizeof(msg), "scroll %.0f %s: clip %.1f to %.1f, content %.1f to %.1f", scroll, card->Name, cmd.ClipRect.y, cmd.ClipRect.w, cmin.y, cmax.y);
                TEST_ASSERT_TRUE_MESSAGE(cmd.ClipRect.y >= cmin.y - 0.5f && cmd.ClipRect.w <= cmax.y + 0.5f, msg);
                TEST_ASSERT_TRUE_MESSAGE(cmd.ClipRect.x >= cmin.x - 0.5f && cmd.ClipRect.z <= cmax.x + 0.5f, msg);
                checked++;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(checked > 100, "draw commands of the cards were checked");
    TEST_ASSERT_TRUE_MESSAGE(cards_cut > 10, "some cards crossed the edge of the content while scrolling");
}

/**
 * Pauza při selhání (review R1 a R2): nepovedené zastavení nic nerozbíhá,
 * nepovedené rozběhnutí se opakuje se stropem, pauza cizího původu se při
 * zavření nepřebije.
 */
void test_pause_failures(void)
{
    /* zastavení selže: emulace běží pod menu, zavření nic nerozbíhá */
    s_pause_fail = true;
    imgui_overlay_open();
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    TEST_ASSERT_FALSE(g_emulator.paused);
    imgui_overlay_close();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_run_calls, "nothing to resume after a failed pause");
    s_pause_fail = false;

    /* rozběhnutí selže dvakrát: opakuje se po OVL_RUN_RETRY_INTERVAL_FRAMES snímcích, třetí pokus uspěje */
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    s_run_fail = 2;
    imgui_overlay_close();
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the first resume failed");
    for (int i = 0; i < 19; i++) imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_calls, "no retry before the interval");
    imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);
    TEST_ASSERT_TRUE(g_emulator.paused);
    for (int i = 0; i < 20; i++) imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT(3, s_run_calls);
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "the third resume worked");
    for (int i = 0; i < 100; i++) imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, s_run_calls, "no more attempts once the emulation runs");

    /* strop: pět opakování, potom emulace zůstane stát */
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_open();
    s_run_fail = 1000;
    imgui_overlay_close();
    for (int i = 0; i < 400; i++) imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, s_run_calls, "one attempt at closing and five retries");
    TEST_ASSERT_TRUE(g_emulator.paused);
    s_run_fail = 0;
    g_emulator.paused = false;

    /* opakování skončí, když emulaci mezitím rozběhne někdo jiný */
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_open();
    s_run_fail = 1000;
    imgui_overlay_close();
    g_emulator.paused = false;
    for (int i = 0; i < 100; i++) imgui_overlay_frame();
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    s_run_fail = 0;

    /* otevření během čekajícího opakování: pauza je pořád overlaye, nové zastavení se nevolá, zavření rozběhne */
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_open();
    s_run_fail = 1;
    imgui_overlay_close();
    TEST_ASSERT_TRUE(g_emulator.paused);
    imgui_overlay_open();
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);
}

/** Pauza cizího původu (review R2): emulaci rozběhne a znovu zastaví někdo jiný, zavření ji nerozbíhá. */
void test_pause_changed_by_others(void)
{
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    for (int i = 0; i < 2; i++) ov_frame();
    g_emulator.paused = false; /* MCP run */
    ov_frame();
    g_emulator.paused = true; /* MCP pause, breakpoint */
    ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "a pause made by someone else stays");
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* beze změny zvenčí zavření rozběhne jako dřív */
    g_emulator.paused = false;
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);

    /* review W1: krok (nebo krátký běh) mezi dvěma snímky UI - overlay emulaci nikdy neviděl běžet,
     * ale číslo běhu jádra se změnilo: pauza už není overlaye */
    st_RUN_LOG_RECORD rec;
    run_log_record_clear(&rec);
    s_run_calls = 0;
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    for (int i = 0; i < 2; i++) ov_frame();
    run_log_begin(); /* MCP step: emu vlákno opustí pauzu a hned do ní vstoupí */
    run_log_end(&rec);
    for (int i = 0; i < 2; i++) ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "a step made by someone else between two frames keeps the pause");
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* totéž bez jediného snímku mezi krokem a zavřením: rozhoduje číslo běhu při zavření */
    g_emulator.paused = false;
    imgui_overlay_open();
    ov_frame();
    run_log_begin();
    run_log_end(&rec);
    imgui_overlay_close();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the run number is compared when the overlay closes");
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* nové otevření si pamatuje nové číslo běhu: zavření zase rozbíhá */
    g_emulator.paused = false;
    imgui_overlay_open();
    for (int i = 0; i < 2; i++) ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
}

/**
 * Otevření z klasického menu (položka "Overlay Menu..."): žádost otevře
 * overlay až v příštím snímku hlavního okna, overlay dostane fokus a
 * emulaci zastaví jako po F10; při otevřeném popupu se žádost zahodí a při
 * otevřeném overlayi nemá účinek. Samotné klasické menu (topmenu.cpp) test
 * nelinkuje; ověřuje se cesta žádosti, kterou položka volá.
 */
void test_open_request_from_classic_menu(void)
{
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_request_open();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "the request does not open the overlay inside the popup frame");
    TEST_ASSERT_FALSE(g_emulator.paused);
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "the main window opens the overlay in its next frame");
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the overlay opened from the classic menu has the focus");
    TEST_ASSERT_FALSE(s_main_keyboard);
    /* při otevřeném overlayi žádost nic nedělá a nezůstane viset do dalšího otevření */
    imgui_overlay_request_open();
    for (int i = 0; i < 2; i++) ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a request made while the overlay was open is not kept");

    /* otevřený popup (klasické menu se nezavřelo, modál odjinud): žádost se zahodí, overlay se neotevře ani později */
    s_modal = true;
    for (int i = 0; i < 3; i++) ov_frame();
    imgui_overlay_request_open();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "no overlay under an open popup");
    s_modal = false;
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a dropped request does not come back");
    TEST_ASSERT_FALSE(imgui_overlay_take_open_request());
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
}

/** @brief Je v posledním kreslení v horní liště štítek Paused? */
static bool sound_chip_paused(void)
{
    return imgui_overlay_test_item_rect("chip_paused", NULL, NULL);
}

/** @brief Klikne na přepínač Listen tabu Sound (čtyři snímky; žádost se uplatní v posledním z nich). */
static void sound_click_listen(void)
{
    ov_click_item("snd_listen");
}

/**
 * @brief Otevře overlay za běhu emulace na tabu Media, přejde na tab Sound a vynuluje počítadla volání.
 * @post Emulace stojí pauzou overlaye, přepínač Listen je vypnutý, s_pause_calls == 1 a s_run_calls == 0.
 */
static void sound_open_on_sound_tab(void)
{
    g_emulator.paused = false;
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    imgui_overlay_open();
    ov_frame();
    ov_key(ImGuiKey_4);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
}

/**
 * Poslech na tabu Sound (rozhodnutí 2026-11-19: přepínač Listen nahradil
 * samočinný běh emulace na tabu): emulace stojí na všech tabech; zapnutý
 * přepínač pauzu overlaye ruší, vypnutý ji vrací. Vypíná se sám při změně
 * tabu, při výsuvném listu, při zavření a když emulaci zastaví někdo jiný;
 * pauzu, která overlayi nepatří (před otevřením, Alt+P, cizí zásah),
 * nerozbíhá a je zakázaný. Klávesnice emulovaného stroje zůstává odpojená
 * i při poslechu a štítek Paused odpovídá skutečnosti.
 */
void test_sound_listen_toggle(void)
{
    /* otevřít za běhu -> tab Sound stojí, Listen vypnutý -> zapnout: běží -> vypnout: stojí -> zavřít: běží */
    imgui_overlay_open();
    TEST_ASSERT_TRUE(g_emulator.paused);
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_TRUE(sound_chip_paused());
    ov_key(ImGuiKey_4);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the emulation stays paused on the Sound tab");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_run_calls, "the Sound tab alone does not resume");
    TEST_ASSERT_TRUE(sound_chip_paused());
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    TEST_ASSERT_TRUE(media_rect("snd_listen", NULL, NULL));
    TEST_ASSERT_TRUE(media_rect("snd_listen_note", NULL, NULL));
    sound_click_listen();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "Listen resumes the emulation");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_ON, imgui_overlay_listen_state());
    TEST_ASSERT_FALSE_MESSAGE(sound_chip_paused(), "no Paused chip while listening");
    TEST_ASSERT_FALSE_MESSAGE(s_main_keyboard, "the emulated keyboard stays detached while listening");
    TEST_ASSERT_TRUE(imgui_overlay_blocks_keyboard());
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_calls, "one run call per switching Listen on");
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    TEST_ASSERT_FALSE(s_main_keyboard);
    sound_click_listen();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "switching Listen off pauses again");
    TEST_ASSERT_EQUAL_INT(2, s_pause_calls);
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    TEST_ASSERT_TRUE(sound_chip_paused());
    imgui_overlay_close();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "closing resumes the pause taken back after listening");
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());

    /* zapnout a přepnout tab: stojí; po návratu na Sound je Listen vypnutý a emulace stojí dál */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    ov_key(ImGuiKey_1);
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "another tab ends listening");
    TEST_ASSERT_EQUAL_INT(2, s_pause_calls);
    TEST_ASSERT_TRUE(sound_chip_paused());
    ov_key(ImGuiKey_4);
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state(), "Listen is off on every arrival at the Sound tab");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);

    /* zapnout a zavřít overlay: emulace běží dál (stav před otevřením), nic dalšího se nevolá */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_calls, "nothing is retried after closing while listening");

    /* otevření rovnou na tabu Sound (tab se pamatuje): emulace se zastaví jako jinde a Listen je vypnutý */
    s_pause_calls = s_run_calls = 0;
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    imgui_overlay_open();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "opening on the Sound tab pauses");
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_TRUE(sound_chip_paused());
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state(), "Listen is off after every opening");
    /* žádost mimo tab Sound nebo při zavřeném overlayi nemá účinek */
    imgui_overlay_set_tab(OVERLAY_TAB_MACHINE);
    imgui_overlay_set_listen(true);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    imgui_overlay_set_listen(true);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);

    /* otevřít při pauze: Listen zakázaný s důvodem, klik nic nedělá, zavření nechá stát */
    s_pause_calls = s_run_calls = 0;
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    g_emulator.paused = true;
    imgui_overlay_open();
    ov_frame();
    ov_key(ImGuiKey_4);
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state());
    media_drain();
    sound_click_listen();
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "a disabled Listen requests nothing");
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "a pause from before the opening is not resumed by Listen");
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state());
    TEST_ASSERT_TRUE(sound_chip_paused());
    {
        ImVec2 mn, mx;
        TEST_ASSERT_TRUE(media_rect("snd_listen", &mn, &mx));
        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        for (int i = 0; i < 3; i++) ov_frame();
        TEST_ASSERT_TRUE_MESSAGE(ov_hover_tooltip((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), "the disabled Listen shows its reason");
        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        for (int i = 0; i < 3; i++) ov_frame();
    }
    /* ani přímá žádost cizí pauzu nerozbíhá */
    imgui_overlay_set_listen(true);
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_TRUE(g_emulator.paused);
    imgui_overlay_close();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    TEST_ASSERT_EQUAL_INT(0, s_pause_calls);
    /* totéž při otevření rovnou na tabu Sound */
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    imgui_overlay_open();
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state());
    imgui_overlay_close();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    g_emulator.paused = false;

    /* Alt+P během poslechu: Listen se vypne, pauza je uživatele; zakázaný přepínač ji nerozbíhá ani po návratu
     * na tab */
    s_pause_toggle_calls = 0;
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    ov_mod_key(ImGuiKey_P, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(1, s_pause_toggle_calls);
    TEST_ASSERT_TRUE(g_emulator.paused);
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "Listen does not resume a pause made by the user");
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state(), "Alt+P while listening switches Listen off and disables it");
    TEST_ASSERT_TRUE(sound_chip_paused());
    sound_click_listen();
    ov_key(ImGuiKey_1);
    ov_key(ImGuiKey_4);
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    imgui_overlay_close();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "a pause made by the user while listening stays after closing");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    g_emulator.paused = false;
    /* druhé Alt+P emulaci rozběhne: běží z vůle uživatele, Listen ukazuje zapnuto a je zakázaný, klik ani jiný
     * tab ji nezastaví a zavření nic nemění */
    sound_open_on_sound_tab();
    ov_mod_key(ImGuiKey_P, ImGuiMod_Alt);
    TEST_ASSERT_FALSE(g_emulator.paused);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_RUNNING, imgui_overlay_listen_state());
    media_drain();
    sound_click_listen();
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "a disabled Listen does not pause an emulation resumed by the user");
    ov_key(ImGuiKey_1);
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "an emulation resumed by the user is not paused by a tab switch");
    TEST_ASSERT_FALSE(sound_chip_paused());
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);

    /* cizí pauza během poslechu (MCP pause, breakpoint): Listen se vypne, overlay ji nerozbíhá a není jeho */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    g_emulator.paused = true;
    for (int i = 0; i < 50; i++) ov_frame();
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_TRUE(sound_chip_paused());
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state());
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    ov_key(ImGuiKey_1);
    ov_key(ImGuiKey_4);
    for (int i = 0; i < 30; i++) ov_frame();
    imgui_overlay_close();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "a pause made by someone else while listening stays");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    g_emulator.paused = false;

    /* výsuvný list výměny média poslech ukončí (z tabu Sound ho uživatel neotevře; pojistka pravidla) */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    overlay_sheet_open(OVL_SHEET_CMT, 0);
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the insert sheet ends listening");
    TEST_ASSERT_EQUAL_INT(2, s_pause_calls);
    overlay_sheet_reset();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    TEST_ASSERT_TRUE(g_emulator.paused);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);

    /* Quick Save na jiném tabu a hned přechod na Sound se zapnutím poslechu: rozběhnutí počká na konec uložení */
    s_pause_calls = s_run_calls = s_run_after_request_calls = 0;
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    imgui_overlay_open();
    ov_frame();
    s_stub_quicksave_requested = true;
    ov_key(ImGuiKey_4);
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the Sound tab does not resume under a pending snapshot save");
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    sound_click_listen();
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "Listen waits for the pending snapshot save");
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_ON, imgui_overlay_listen_state());
    TEST_ASSERT_TRUE_MESSAGE(sound_chip_paused(), "the Paused chip tells the truth while Listen waits");
    s_stub_quicksave_requested = false;
    ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "the emulation runs once the save is done");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(0, s_run_after_request_calls);
    /* zavření se zapnutým přepínačem ještě během uložení: rozběhnutí se předá uložení jako z jiného tabu */
    sound_open_on_sound_tab();
    s_stub_quicksave_requested = true;
    sound_click_listen();
    TEST_ASSERT_TRUE(g_emulator.paused);
    imgui_overlay_close();
    s_stub_quicksave_requested = false;
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    TEST_ASSERT_EQUAL_INT(1, s_run_after_request_calls);
    TEST_ASSERT_TRUE(g_emulator.paused);
    /* Quick Save během poslechu: uložení si emulaci zastaví a rozběhne samo, overlay do toho nevstupuje; Listen
     * se vypne a po rozběhnutí si overlay pauzu vezme zpět */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    g_emulator.paused = true; /* snapshot_pause_and_request() */
    s_stub_quicksave_requested = true;
    for (int i = 0; i < 5; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_calls, "the overlay does not resume under a snapshot save");
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_BLOCKED_PAUSED, imgui_overlay_listen_state(), "a snapshot save switches Listen off");
    TEST_ASSERT_EQUAL_INT(1, s_pause_calls);
    s_stub_quicksave_requested = false;
    g_emulator.paused = false; /* dokončení uložení emulaci rozběhne */
    ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.paused, "the overlay takes the pause once the save has resumed the emulation");
    TEST_ASSERT_EQUAL_INT(2, s_pause_calls);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);

    /* rychlé přepínání tabů při poslechu: změna tabu mezi dvěma snímky poslech ukončí, i když se tab vrátí */
    sound_open_on_sound_tab();
    static const en_OVERLAY_TAB others[] = {OVERLAY_TAB_MEDIA, OVERLAY_TAB_MACHINE, OVERLAY_TAB_DISPLAY, OVERLAY_TAB_INPUT, OVERLAY_TAB_CAPTURE};
    for (int round = 0; round < 20; round++) {
        const en_OVERLAY_TAB other = others[round % 5];
        imgui_overlay_set_listen(true);
        ov_frame();
        TEST_ASSERT_FALSE(g_emulator.paused);
        TEST_ASSERT_FALSE(sound_chip_paused());
        if (round % 2 == 0) {
            /* tam a zpět bez snímku */
            imgui_overlay_set_tab(other);
            imgui_overlay_set_tab(OVERLAY_TAB_SOUND);
        } else {
            imgui_overlay_set_tab(other);
            ov_frame();
            TEST_ASSERT_TRUE(g_emulator.paused);
            imgui_overlay_set_tab(OVERLAY_TAB_SOUND);
        }
        ov_frame();
        TEST_ASSERT_TRUE(g_emulator.paused);
        TEST_ASSERT_TRUE(sound_chip_paused());
        TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state());
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(20, s_run_calls, "one run per switching Listen on");
    TEST_ASSERT_EQUAL_INT_MESSAGE(21, s_pause_calls, "one pause at opening and one per end of listening");
    imgui_overlay_close();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "closing after fast tab switching resumes the emulation");

    /* rozběhnutí pro poslech selže: opakuje se po intervalu, nejvýš pětkrát; potom se Listen vypne a zůstane
     * pauza overlaye */
    sound_open_on_sound_tab();
    s_run_fail = 2;
    sound_click_listen();
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_TRUE_MESSAGE(sound_chip_paused(), "the Paused chip tells the truth after a failed resume");
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_ON, imgui_overlay_listen_state());
    for (int i = 0; i < 19; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_run_calls, "no retry before the interval");
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(2, s_run_calls);
    for (int i = 0; i < 21; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(3, s_run_calls);
    TEST_ASSERT_FALSE(g_emulator.paused);
    sound_click_listen();
    TEST_ASSERT_TRUE(g_emulator.paused);
    s_run_calls = 0;
    s_run_fail = 1000;
    sound_click_listen();
    for (int i = 0; i < 400; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_TEST_RUN_ATTEMPTS, s_run_calls, "the attempts to resume for listening are capped");
    TEST_ASSERT_TRUE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_OFF, imgui_overlay_listen_state(), "Listen switches off after the last failed attempt");
    /* nové zapnutí má znovu plný počet pokusů */
    s_run_calls = 0;
    sound_click_listen();
    for (int i = 0; i < 400; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVL_TEST_RUN_ATTEMPTS, s_run_calls);
    s_run_fail = 0;
    s_run_calls = 0;
    imgui_overlay_close();
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "closing still resumes the pause of the overlay");
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);

    /* zastavení při vypnutí poslechu selže: emulace běží pod menu, štítek Paused není, Listen je zakázaný
     * a zavření nic nerozbíhá */
    sound_open_on_sound_tab();
    sound_click_listen();
    TEST_ASSERT_FALSE(g_emulator.paused);
    s_pause_fail = true;
    sound_click_listen();
    TEST_ASSERT_EQUAL_INT(2, s_pause_calls);
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_FALSE(sound_chip_paused());
    s_pause_fail = false;
    for (int i = 0; i < 30; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_pause_calls, "the failed pause is not repeated");
    TEST_ASSERT_EQUAL_INT(OVERLAY_LISTEN_BLOCKED_RUNNING, imgui_overlay_listen_state());
    imgui_overlay_close();
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_EQUAL_INT(1, s_run_calls);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);

    /* dostupnost akce a její důvody podle kopie stavu */
    static st_OVERLAY_VIEW v;
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_OFF, v.sound.listen, "the paused fixture offers Listen");
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_LISTEN, &v));
    TEST_ASSERT_NULL_MESSAGE(overlay_action_def(OVL_ACTION_LISTEN)->keycap, "Listen has no shortcut");
    v.sound.listen = OVERLAY_LISTEN_ON;
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_LISTEN, &v));
    v.sound.listen = OVERLAY_LISTEN_BLOCKED_PAUSED;
    TEST_ASSERT_EQUAL_STRING(_("The emulation was paused before the menu was opened. Resume it to listen."), overlay_action_reason(OVL_ACTION_LISTEN, &v));
    v.sound.listen = OVERLAY_LISTEN_BLOCKED_RUNNING;
    TEST_ASSERT_EQUAL_STRING(_("The emulation is running."), overlay_action_reason(OVL_ACTION_LISTEN, &v));
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_EMPTY);
    TEST_ASSERT_FALSE(v.paused);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_LISTEN_BLOCKED_RUNNING, v.sound.listen, "a running fixture shows Listen on and disabled");

    /* značka ztlumení pro titulek hlavního okna: jen při ztlumení, za běhu i při pauze */
    const bool saved_mute = iface_audio_get_mute();
    iface_audio_set_mute(false);
    TEST_ASSERT_NULL(emulator_get_mute_status_as_text());
    iface_audio_set_mute(true);
    TEST_ASSERT_NOT_NULL(emulator_get_mute_status_as_text());
    g_emulator.paused = true;
    TEST_ASSERT_NOT_NULL(emulator_get_mute_status_as_text());
    g_emulator.paused = false;
    iface_audio_set_mute(saved_mute);
}

/**
 * Kopie stavu tabu Media nad skutečným jádrem: bez diskety, s disketou,
 * po vysunutí z jiného místa mezi dvěma plněními, odpojený řadič, motor,
 * kazeta bez pásky, replay; testovací stavy a jejich invarianty.
 */
void test_media_view_fill(void)
{
    static st_OVERLAY_VIEW v, v_before;
    g_fdc[FDC0].connected = FDC_CONNECTED;
    media_fill(&v);
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
        TEST_ASSERT_TRUE(v.fdd[i].present);
        TEST_ASSERT_TRUE(v.fdd[i].connected);
        TEST_ASSERT_FALSE(v.fdd[i].mounted);
        TEST_ASSERT_FALSE(v.fdd[i].active);
        TEST_ASSERT_EQUAL_STRING("", v.fdd[i].name);
        TEST_ASSERT_EQUAL_STRING("", v.fdd[i].path);
    }
    TEST_ASSERT_EQUAL_STRING("", v.media_disabled_reason);
    TEST_ASSERT_EQUAL_UINT(0, v.recent_count);
    /* kazeta bez pásky (stav z cmt_view_fill()) */
    TEST_ASSERT_FALSE(v.cmt.has_tape);
    TEST_ASSERT_EQUAL_STRING("", v.cmt.name);
    TEST_ASSERT_EQUAL_INT(OVERLAY_CMT_STOP, v.cmt.state);
    TEST_ASSERT_EQUAL_INT(0, v.cmt.block_count);
    TEST_ASSERT_EQUAL_INT(-1, v.cmt.current_block);
    TEST_ASSERT_EQUAL_INT(-1, v.cmt.first_playable);
    TEST_ASSERT_TRUE(v.cmt.progress >= 0.0 && v.cmt.progress <= 1.0);
    TEST_ASSERT_TRUE(v.qd.present);

    /* s disketou v FDD 1 */
    const char *dsk = media_test_dsk();
    gchar *dsk_copy = g_strdup(dsk);
    fdc_mount_dskfile(&g_fdc[FDC0], 1, dsk_copy);
    g_free(dsk_copy);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    media_fill(&v);
    TEST_ASSERT_TRUE(v.fdd[1].mounted);
    TEST_ASSERT_EQUAL_STRING("overlay-test.dsk", v.fdd[1].name);
    TEST_ASSERT_EQUAL_STRING(dsk, v.fdd[1].path);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(8, v.fdd[1].size_kb, "2 tracks of 16 sectors of 256 bytes");
    TEST_ASSERT_FALSE(v.fdd[1].unsaved);
    TEST_ASSERT_FALSE(v.fdd[0].mounted);

    /* kontrolka: vybraná mechanika a běžící motor v registru 0DCh */
    const uint8_t motor = g_fdc[FDC0].wd279x.MOTOR;
    g_fdc[FDC0].wd279x.MOTOR = 0x81;
    media_fill(&v);
    TEST_ASSERT_TRUE(v.fdd[1].active);
    TEST_ASSERT_FALSE(v.fdd[0].active);
    g_fdc[FDC0].wd279x.MOTOR = 0x01; /* motor stojí */
    media_fill(&v);
    TEST_ASSERT_FALSE(v.fdd[1].active);
    g_fdc[FDC0].wd279x.MOTOR = motor;

    /* vysunutí z jiného místa mezi dvěma plněními: nová kopie je prázdná, stará zůstává platná */
    media_fill(&v_before);
    fdc_umount(&g_fdc[FDC0], 1);
    media_fill(&v);
    TEST_ASSERT_FALSE(v.fdd[1].mounted);
    TEST_ASSERT_EQUAL_STRING("", v.fdd[1].name);
    TEST_ASSERT_EQUAL_UINT(0, v.fdd[1].size_kb);
    TEST_ASSERT_TRUE(v_before.fdd[1].mounted);
    TEST_ASSERT_EQUAL_STRING("overlay-test.dsk", v_before.fdd[1].name);

    /* odpojený řadič */
    g_fdc[FDC0].connected = FDC_DISCONNECTED;
    media_fill(&v);
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
        TEST_ASSERT_TRUE(v.fdd[i].present);
        TEST_ASSERT_FALSE(v.fdd[i].connected);
    }
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_FDD_INSERT_0, &v));
    g_fdc[FDC0].connected = FDC_CONNECTED;

    /* záznam vstupů: karty médií zakázané s důvodem */
    g_replay_mode = REPLAY_MODE_RECORDING;
    media_fill(&v);
    TEST_ASSERT_TRUE(v.replay_active);
    TEST_ASSERT_TRUE(v.media_disabled_reason[0] != '\0');
    TEST_ASSERT_EQUAL_STRING(v.media_disabled_reason, overlay_action_reason(OVL_ACTION_FDD_INSERT_0, &v));
    TEST_ASSERT_EQUAL_STRING(v.media_disabled_reason, overlay_action_reason(OVL_ACTION_CMT_RECORD, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_QD_INSERT, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_QUICK_LOAD, &v));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_QUICK_SAVE, &v));
    g_replay_mode = REPLAY_MODE_OFF;

    /* testovací stavy */
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    TEST_ASSERT_EQUAL_STRING("cpm-system.dsk", v.fdd[0].name);
    TEST_ASSERT_TRUE(v.fdd[0].active);
    TEST_ASSERT_EQUAL_STRING("games-vol2.dsk", v.fdd[1].name);
    TEST_ASSERT_TRUE(v.fdd[1].unsaved);
    TEST_ASSERT_FALSE(v.fdd[2].mounted);
    TEST_ASSERT_FALSE(v.fdd[3].mounted);
    TEST_ASSERT_EQUAL_STRING("flappy.mzf", v.cmt.name);
    TEST_ASSERT_EQUAL_INT(OVERLAY_CMT_PLAY, v.cmt.state);
    TEST_ASSERT_EQUAL_INT(1, v.cmt.current_block);
    TEST_ASSERT_EQUAL_INT(5, v.cmt.block_count);
    TEST_ASSERT_EQUAL_INT(OVERLAY_CMT_MODE_BOOST, v.cmt.mode);
    TEST_ASSERT_TRUE(v.qd.present && v.qd.connected && !v.qd.mounted);
    /* nedávná média konceptu: od naposledy vloženého, tři druhy */
    TEST_ASSERT_EQUAL_UINT(10, v.recent_count);
    TEST_ASSERT_EQUAL_STRING("games-vol2.dsk", v.recent[0].name);
    TEST_ASSERT_EQUAL_INT(OVERLAY_MEDIA_DSK, v.recent[0].kind);
    TEST_ASSERT_EQUAL_STRING("flappy.mzf", v.recent[1].name);
    TEST_ASSERT_EQUAL_INT(OVERLAY_MEDIA_CMT, v.recent[1].kind);
    TEST_ASSERT_EQUAL_STRING("D:/MZ/disks/utils.dsk", v.recent[2].path);
    for (unsigned i = 1; i < v.recent_count; i++) TEST_ASSERT_TRUE(v.recent[i - 1].time >= v.recent[i].time);
    TEST_ASSERT_TRUE(v.recent_now >= v.recent[0].time);

    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_EMPTY);
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) TEST_ASSERT_TRUE(v.fdd[i].connected && !v.fdd[i].mounted);
    TEST_ASSERT_FALSE(v.cmt.has_tape);
    TEST_ASSERT_FALSE(v.paused);

    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_LONG_TEXTS);
    TEST_ASSERT_EQUAL_INT(200, (int)g_utf8_strlen(v.fdd[0].name, -1));
    TEST_ASSERT_TRUE(g_utf8_validate(v.fdd[0].path, -1, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(v.fdd[0].path, "\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD"), "the path keeps its diacritics");
    TEST_ASSERT_EQUAL_INT(200, (int)g_utf8_strlen(v.cmt.name, -1));
    TEST_ASSERT_EQUAL_INT(200, (int)g_utf8_strlen(v.qd.name, -1));

    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_REPLAY);
    TEST_ASSERT_TRUE(v.replay_active);
    TEST_ASSERT_TRUE(v.media_disabled_reason[0] != '\0');

    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_NO_FDC);
    TEST_ASSERT_FALSE(v.fdd[0].connected);
    TEST_ASSERT_FALSE(v.qd.connected);
    TEST_ASSERT_EQUAL_STRING("", v.media_disabled_reason);

    /* zkrácení textu nekončí uprostřed znaku UTF-8 */
    char small[6];
    overlay_view_copy_text(small, sizeof(small), "ab\xC5\xBE\xC5\xBE\xC5\xBE");
    TEST_ASSERT_EQUAL_STRING("ab\xC5\xBE", small);
    overlay_view_copy_text(small, sizeof(small), NULL);
    TEST_ASSERT_EQUAL_STRING("", small);
    overlay_view_copy_text(small, sizeof(small), "a\xFF" "b");
    TEST_ASSERT_EQUAL_STRING("a", small);
}

/** Tabulka akcí: řádky v pořadí akcí, zkratky mechanik na klávesách 1 až 4, štítek jen u dostupné akce. */
void test_action_table(void)
{
    for (int i = 0; i < (int)OVL_ACTION_COUNT; i++) {
        const st_OVL_ACTION_DEF *def = overlay_action_def((en_OVL_ACTION)i);
        TEST_ASSERT_EQUAL_INT(i, (int)def->id);
        TEST_ASSERT_TRUE_MESSAGE((def->chord == 0) == (def->keycap == NULL), "a shortcut and its key cap go together");
        if (i > 0) TEST_ASSERT_NOT_NULL(def->run);
    }
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, (int)overlay_action_def(OVL_ACTION_COUNT)->id);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, (int)overlay_action_def((en_OVL_ACTION)-1)->id);

    /* mechaniky FDD 0 až 3 jsou na klávesách 1 až 4 (obsluha v imgui_global_shortcuts()), ne 0 až 3 */
    for (int d = 0; d < 4; d++) {
        char want[32];
        const st_OVL_ACTION_DEF *ins = overlay_action_def((en_OVL_ACTION)(OVL_ACTION_FDD_INSERT_0 + d));
        const st_OVL_ACTION_DEF *ej = overlay_action_def((en_OVL_ACTION)(OVL_ACTION_FDD_EJECT_0 + d));
        snprintf(want, sizeof(want), "Alt+%d", d + 1);
        TEST_ASSERT_EQUAL_STRING(want, ins->keycap);
        TEST_ASSERT_EQUAL_INT((int)(ImGuiMod_Alt | (ImGuiKey_1 + d)), (int)ins->chord);
        TEST_ASSERT_EQUAL_INT(d, ins->arg);
        snprintf(want, sizeof(want), "Alt+Shift+%d", d + 1);
        TEST_ASSERT_EQUAL_STRING(want, ej->keycap);
        TEST_ASSERT_EQUAL_INT((int)(ImGuiMod_Alt | ImGuiMod_Shift | (ImGuiKey_1 + d)), (int)ej->chord);
    }
    /* Resume zkratku nemá (F10 patří tlačítku Close), Reset je F12 */
    TEST_ASSERT_NULL(overlay_action_def(OVL_ACTION_RESUME)->keycap);
    TEST_ASSERT_EQUAL_STRING("F12", overlay_action_def(OVL_ACTION_RESET)->keycap);

    /* štítek znamená "funguje právě teď" */
    static st_OVERLAY_VIEW v;
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    TEST_ASSERT_EQUAL_STRING("Alt+1", overlay_action_keycap(OVL_ACTION_FDD_INSERT_0, &v));
    TEST_ASSERT_EQUAL_STRING("Alt+Shift+2", overlay_action_keycap(OVL_ACTION_FDD_EJECT_1, &v));
    TEST_ASSERT_NULL_MESSAGE(overlay_action_keycap(OVL_ACTION_FDD_EJECT_2, &v), "an empty drive has nothing to eject");
    TEST_ASSERT_EQUAL_STRING("Alt+F9", overlay_action_keycap(OVL_ACTION_QUICK_LOAD, &v));
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_REPLAY);
    TEST_ASSERT_NULL(overlay_action_keycap(OVL_ACTION_FDD_INSERT_0, &v));
    TEST_ASSERT_NULL(overlay_action_keycap(OVL_ACTION_QUICK_LOAD, &v));
    TEST_ASSERT_EQUAL_STRING("Alt+F8", overlay_action_keycap(OVL_ACTION_QUICK_SAVE, &v));
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_NO_FDC);
    TEST_ASSERT_NULL(overlay_action_keycap(OVL_ACTION_FDD_INSERT_3, &v));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_STOP, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_RESUME, NULL));

    /* pravidla kazety podle okna Virtual CMT */
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT); /* hraje, Boost */
    TEST_ASSERT_NOT_NULL_MESSAGE(overlay_action_reason(OVL_ACTION_CMT_PLAY, &v), "already playing");
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_PAUSE, &v));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_STOP, &v));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_REWIND, &v));
    TEST_ASSERT_NOT_NULL_MESSAGE(overlay_action_reason(OVL_ACTION_CMT_RECORD, &v), "stop the tape first");
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_MODE_TAPE, &v));
    TEST_ASSERT_NULL_MESSAGE(overlay_action_reason(OVL_ACTION_CMT_MODE_BOOST, &v), "the selected mode is never disabled");
    TEST_ASSERT_NOT_NULL_MESSAGE(overlay_action_reason(OVL_ACTION_CMT_MODE_DIRECT, &v), "Direct only with a stopped tape");
    v.cmt.user_paused = v.cmt.paused = true;
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_PLAY, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_CMT_PAUSE, &v));
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_EMPTY); /* bez pásky */
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_CMT_PLAY, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_CMT_STOP, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_CMT_REWIND, &v));
    TEST_ASSERT_NULL_MESSAGE(overlay_action_reason(OVL_ACTION_CMT_RECORD, &v), "recording works without a tape");
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_CMT_MODE_DIRECT, &v));
    v.cmt.direct_supported = false;
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_CMT_MODE_DIRECT, &v));
    /* Quick Disk: vložit jen do prázdné mechaniky, řízený Unicardem nejde */
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_QD_INSERT, &v));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_QD_EJECT, &v));
    v.qd.unicard = true;
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_QD_INSERT, &v));
}

/**
 * Geometrie tabu Media (Review Focus 3 a 4): všech pět testovacích stavů
 * při 640 x 480, 1280 x 720, 1920 x 1080 a 2560 x 1440, s texty běžnými
 * i o 40 % delšími. Karty v ploše obsahu, prvky v kartách, nic se
 * nepřekrývá; od 1280 x 720 se obsah neroluje; jméno souboru o 200
 * znacích končí před tlačítky a pod ukazatelem myši ukáže plné znění.
 */
void test_media_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}};
    for (int stretch = 0; stretch < 2; stretch++) {
        overlay_test_set_stretch(stretch != 0);
        for (const float *sz : sizes) {
            for (int f = 0; f < (int)OVERLAY_FIXTURE_COUNT; f++) {
                media_start((en_OVERLAY_FIXTURE)f, sz[0], sz[1]);
                char msg[96];
                snprintf(msg, sizeof(msg), "%.0fx%.0f %s%s", sz[0], sz[1], s_fixture_names[f], stretch ? " +40%" : "");
                ImGuiWindow *content = media_content_window();
                TEST_ASSERT_NOT_NULL_MESSAGE(content, msg);
                const bool wide = sz[0] >= 1280.0f;
                if (wide) {
                    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, content->ScrollMax.y, msg);
                    media_check_geometry(msg, true);
                    /* dva sloupce: kazeta vpravo od mechanik, sloupce končí ve stejné výšce */
                    ImVec2 amin, amax, bmin, bmax, qmin, qmax;
                    TEST_ASSERT_TRUE(media_rect("card_fdd", &amin, &amax));
                    TEST_ASSERT_TRUE(media_rect("card_cmt", &bmin, &bmax));
                    TEST_ASSERT_TRUE(media_rect("card_qd", &qmin, &qmax));
                    TEST_ASSERT_TRUE_MESSAGE(bmin.x >= amax.x, msg);
                    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amax.y, qmax.y, msg);
                } else {
                    /* úzké okno: jeden sloupec, obsah se roluje; kontrola na začátku a na konci */
                    TEST_ASSERT_TRUE_MESSAGE(content->ScrollMax.y > 0.0f, msg);
                    media_check_geometry(msg, false);
                    ImVec2 amin, amax, bmin, bmax;
                    TEST_ASSERT_TRUE(media_rect("card_fdd", &amin, &amax));
                    TEST_ASSERT_TRUE(media_rect("card_cmt", &bmin, &bmax));
                    TEST_ASSERT_TRUE_MESSAGE(bmin.y >= amax.y, msg);
                    TEST_ASSERT_TRUE_MESSAGE(media_rect("fdd0_name", NULL, NULL), msg);
                    ImGui::SetScrollY(content, content->ScrollMax.y);
                    for (int i = 0; i < 3; i++) ov_frame();
                    media_check_geometry(msg, false);
                    TEST_ASSERT_TRUE_MESSAGE(media_rect("act_reset", NULL, NULL), msg);
                    ImGui::SetScrollY(content, 0.0f);
                    for (int i = 0; i < 2; i++) ov_frame();
                }

                /* zakázané karty mají pod nadpisem důvod */
                const bool reason = media_rect("card_reason", NULL, NULL);
                if (f == OVERLAY_FIXTURE_REPLAY || f == OVERLAY_FIXTURE_NO_FDC) TEST_ASSERT_TRUE_MESSAGE(reason, msg);
                else TEST_ASSERT_FALSE_MESSAGE(reason, msg);
                /* štítky zkratek mechanik jen tam, kde zkratka funguje */
                const bool keycap = media_rect("fdd0_keycap", NULL, NULL);
                if (f == OVERLAY_FIXTURE_REPLAY || f == OVERLAY_FIXTURE_NO_FDC) TEST_ASSERT_FALSE_MESSAGE(keycap, msg);
                else TEST_ASSERT_TRUE_MESSAGE(keycap, msg);
            }
        }
    }
    overlay_test_set_stretch(false);

    /* jméno o 200 znacích: zkrácené před tlačítky, tooltip s plným zněním */
    media_start(OVERLAY_FIXTURE_LONG_TEXTS, 1280.0f, 720.0f);
    ImVec2 nmin, nmax, bmin, bmax, rmin, rmax;
    TEST_ASSERT_TRUE(media_rect("fdd0_name", &nmin, &nmax));
    TEST_ASSERT_TRUE(media_rect("fdd0_insert", &bmin, &bmax));
    TEST_ASSERT_TRUE(media_rect("fdd0", &rmin, &rmax));
    TEST_ASSERT_TRUE_MESSAGE(nmax.x <= bmin.x, "the file name ends before the Change button");
    TEST_ASSERT_TRUE_MESSAGE(nmax.x - nmin.x > (rmax.x - rmin.x) * 0.2f, "the file name still has a reasonable width");
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((nmin.x + nmax.x) * 0.5f, (nmin.y + nmax.y) * 0.5f);
    bool tooltip = false;
    for (int i = 0; i < 60 && !tooltip; i++) {
        SDL_Delay(20);
        ov_frame();
        ImGuiContext &g = *GImGui;
        for (int w = 0; w < g.Windows.Size; w++)
            if ((g.Windows[w]->Flags & ImGuiWindowFlags_Tooltip) && g.Windows[w]->Active) tooltip = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(tooltip, "a truncated file name shows a tooltip with the full text");
}

/**
 * Tlačítka tabu Media zařadí správný požadavek (zachyceno frontami
 * požadavků a stuby), akce zavírající overlay ho zavřou a nedostupné akce
 * nic nezařadí.
 */
void test_media_buttons(void)
{
    en_REPLAY_CMT_OP op;
    int32_t arg;
    st_REPLAY_CMT_GUARD guard;
    const char *name;
    st_DEVICE_REQ_ARGS args;

    /* koncept: disketa v FDD 0 a 1 (FDD 1 s neuloženými změnami), páska hraje v režimu Boost */
    media_start(OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();

    media_click("fdd1_eject");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_EJECT_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_umount", name);
    TEST_ASSERT_EQUAL_INT(FDC0, args.i[0]);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    media_click("fdd1_save");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_SAVE_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_sync", name);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    TEST_ASSERT_FALSE_MESSAGE(media_rect("fdd0_save", NULL, NULL), "no save button without unsaved changes");

    /* Change, Insert disk a klik do řádku: výsuvný list výměny diskety u té mechaniky (dialog souboru až z listu) */
    int sheet_drive = -1;
    media_click("fdd0_insert");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive));
    TEST_ASSERT_EQUAL_INT(0, sheet_drive);
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click("fdd3_insert");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive));
    TEST_ASSERT_EQUAL_INT(3, sheet_drive);
    ov_key(ImGuiKey_Escape);
    media_click("fdd2_name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive), "a click anywhere in the row opens the sheet");
    TEST_ASSERT_EQUAL_INT(2, sheet_drive);
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "opening the sheet runs no action");
    TEST_ASSERT_FALSE_MESSAGE(device_request_test_take_pending(NULL, NULL), "opening the sheet queues nothing");

    /* transport */
    media_click("cmt_stop");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_STOP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt(&op, &arg));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_STOP_BUTTON, op);
    media_click("cmt_pause");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_PAUSE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &guard));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_PAUSE, op);
    TEST_ASSERT_EQUAL_INT(1, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_NOT_STOP, guard.flags);
    media_click("cmt_rewind");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_REWIND, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt_guard(&op, &arg, &guard));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_SEEK, op);
    TEST_ASSERT_EQUAL_INT(0, arg);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CMT_GUARD_TAPE, guard.flags);
    TEST_ASSERT_EQUAL_UINT(1, guard.tape_serial);
    /* nedostupné: Play (už hraje) a Record (nejdřív Stop) nic nezařadí */
    media_click("cmt_play");
    media_click("cmt_record");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt(&op, &arg));

    /* režim načítání: Tape jde i při přehrávání, Direct ne */
    media_click_at("cmt_mode", 1.0f / 6.0f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_MODE_TAPE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LOADING_MODE_NAME, name);
    media_click_at("cmt_mode", 5.0f / 6.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "Direct is disabled while the tape plays");
    media_click_at("cmt_mode", 0.5f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the selected mode does nothing");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    /* jiná páska: výsuvný list výměny pásky; dialog souboru a automat akcí s páskou až z listu */
    media_click("cmt_change");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_CMT, overlay_sheet_target(NULL));
    TEST_ASSERT_EQUAL_INT(0, s_tape_action_calls);
    media_click("sheet_browse");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_CHANGE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_tape_action_calls);
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_OPEN, s_tape_action_last);
    TEST_ASSERT_FALSE_MESSAGE(overlay_sheet_is_open(), "Browse files... closes the sheet");

    /* nedávná média: položky místo prázdného stavu a Browse files... (první prázdná mechanika = FDD 2) */
    TEST_ASSERT_FALSE(media_rect("recent_empty", NULL, NULL));
    TEST_ASSERT_TRUE(media_rect("recent_chip0", NULL, NULL));
    media_click("recent_browse");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_BROWSE_FILES, overlay_actions_test_take_last());

    /* řada akcí */
    g_emulator.max_speed = false;
    media_click("act_speed");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE_MESSAGE(g_emulator.max_speed, "Speed switches the maximum speed like Alt+M");
    media_click("act_speed");
    TEST_ASSERT_FALSE(g_emulator.max_speed);
    overlay_actions_test_take_last();
    media_click("act_quick_save");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_SAVE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_quicksave_calls);
    media_click("act_screenshot");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_screenshot_calls);
    media_click("act_fullscreen");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FULLSCREEN, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_fullscreen_calls);

    /* prázdný stav: Record bez pásky, Insert disk u Quick Disku; Play, Stop a Rewind ne */
    media_start(OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f);
    media_drain();
    media_click("cmt_record");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_RECORD, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(replay_test_take_ui_cmt(&op, &arg));
    TEST_ASSERT_EQUAL_INT(REPLAY_CMT_RECORD, op);
    media_click("cmt_play");
    media_click("cmt_stop");
    media_click("cmt_rewind");
    media_click("cmt_pause");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt(&op, &arg));
    /* bez historie: text prázdného stavu; Insert disk u Quick Disku otevře list a dialog souboru je v něm */
    TEST_ASSERT_TRUE(media_rect("recent_empty", NULL, NULL));
    TEST_ASSERT_FALSE(media_rect("recent_chip0", NULL, NULL));
    media_click("qd_insert");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_QD, overlay_sheet_target(NULL));
    TEST_ASSERT_TRUE_MESSAGE(media_rect("sheet_empty", NULL, NULL), "the sheet says there are no recent disks");
    media_click("sheet_browse");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QD_INSERT, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click_at("cmt_mode", 5.0f / 6.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_CMT_MODE_DIRECT, overlay_actions_test_take_last(), "Direct works with a stopped deck");
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LOADING_MODE_NAME, name);

    /* vložený Quick Disk s neuloženými změnami: uložit a vysunout */
    media_start(OVERLAY_FIXTURE_LONG_TEXTS, 1280.0f, 720.0f);
    media_drain();
    media_click("qd_save");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QD_SAVE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_sync", name);
    media_click("qd_eject");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QD_EJECT, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_umount", name);
    TEST_ASSERT_FALSE_MESSAGE(media_rect("qd_insert", NULL, NULL), "a mounted Quick Disk offers no Insert disk");

    /* replay (Review Focus 5): karty médií nic nezařadí, Quick Load je zakázaný, Quick Save jde */
    media_start(OVERLAY_FIXTURE_REPLAY, 1280.0f, 720.0f);
    media_drain();
    static const char *const dead[] = {"fdd0_insert", "fdd1_eject", "fdd1_save", "fdd2_name", "cmt_stop",      "cmt_rewind",     "cmt_change",
                                       "qd_insert",   "recent_browse", "recent_chip0", "recent_chip1", "recent_chip2", "act_quick_load"};
    for (const char *n : dead) {
        media_click(n);
        TEST_ASSERT_FALSE_MESSAGE(overlay_sheet_is_open(), n);
    }
    media_click_at("cmt_mode", 1.0f / 6.0f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, s_tape_open_calls);
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_FALSE(replay_test_take_ui_cmt(&op, &arg));
    TEST_ASSERT_EQUAL_INT(0, s_tape_action_calls);
    TEST_ASSERT_EQUAL_INT(0, s_quickload_calls);
    media_click("act_quick_save");
    TEST_ASSERT_EQUAL_INT(1, s_quicksave_calls);

    /* odpojený řadič: mechaniky a Quick Disk nic, kazeta funguje */
    media_start(OVERLAY_FIXTURE_NO_FDC, 1280.0f, 720.0f);
    media_drain();
    media_click("fdd0_insert");
    media_click("fdd1_eject");
    media_click("qd_insert");
    media_click("recent_browse");
    media_click("recent_chip0"); /* obraz diskety: žádná mechanika není dostupná */
    {
        /* zakázané Browse files... říká proč: otevírá dialog diskety a řadič není připojen (posudek N08) */
        const char *why = overlay_action_reason(OVL_ACTION_BROWSE_FILES, &s_fixture_view);
        TEST_ASSERT_NOT_NULL(why);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, "disk image"), why);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, "FDC0"), why);
        ImVec2 bmn, bmx;
        TEST_ASSERT_TRUE(media_rect("recent_browse", &bmn, &bmx));
        TEST_ASSERT_TRUE_MESSAGE(ov_hover_tooltip((bmn.x + bmx.x) * 0.5f, (bmn.y + bmx.y) * 0.5f), "the disabled Browse files... button shows its reason");
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        for (int i = 0; i < 3; i++) ov_frame();
    }
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    media_click("cmt_stop");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_STOP, overlay_actions_test_take_last());
    media_drain();
}

/**
 * Akce, které overlay zavírají, a stav pauzy po nich (Review Focus 2):
 * Resume rozběhne i emulaci, která před otevřením stála; Reset zavře a
 * vrátí stav před otevřením; Quick Load se volá až po zavření (pauzu si
 * řídí sám); okno Virtual CMT overlay zavře a ukáže se.
 */
void test_media_closing_actions(void)
{
    /* Resume: emulace před otevřením stála, po Resume běží */
    g_emulator.paused = true;
    imgui_overlay_open();
    media_start(OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    media_click("act_resume");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_RESUME, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "Resume closes the overlay");
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "Resume runs the emulation even if it was paused before");

    /* Reset: zavře; emulace, která běžela, běží, a která stála, stojí */
    for (int was_paused = 0; was_paused < 2; was_paused++) {
        g_emulator.paused = was_paused != 0;
        imgui_overlay_open();
        TEST_ASSERT_TRUE(g_emulator.paused);
        for (int i = 0; i < 3; i++) ov_frame();
        media_drain();
        media_click("act_reset");
        TEST_ASSERT_EQUAL_INT(OVL_ACTION_RESET, overlay_actions_test_take_last());
        TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "Reset closes the overlay");
        TEST_ASSERT_EQUAL_INT(was_paused, g_emulator.paused ? 1 : 0);
    }

    /* Quick Load: nejdřív zavření (obnova běhu), potom stejná funkce jako Alt+F9 */
    g_emulator.paused = false;
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();
    media_click("act_quick_load");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_LOAD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_quickload_calls);
    TEST_ASSERT_FALSE_MESSAGE(s_quickload_overlay_open, "the overlay is closed before Quick Load starts");
    TEST_ASSERT_FALSE_MESSAGE(s_quickload_paused, "Quick Load sees the emulation as it was before the overlay opened");
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* Quick Save: overlay zůstává, pauza se nemění */
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();
    media_click("act_quick_save");
    TEST_ASSERT_EQUAL_INT(1, s_quicksave_calls);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_TRUE(g_emulator.paused);

    /* okno Virtual CMT: overlay se zavře a okno se ukáže */
    g_gui->showVirtualCmtWindow = false;
    media_click("cmt_window");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_WINDOW, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_gui->showVirtualCmtWindow);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a tool window closes the overlay");
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "and the emulation runs again");
    g_gui->showVirtualCmtWindow = false;
}

/**
 * Zkratky akcí v okně overlaye (K02): stejné kombinace jako globální
 * zkratky; mechaniky na klávesách 1 až 4; nedostupná akce na zkratku
 * nereaguje; číslice bez Alt dál přepínají taby a Alt + číslice tab nemění.
 */
void test_media_shortcuts(void)
{
    const char *name;
    st_DEVICE_REQ_ARGS args;
    imgui_overlay_open();
    media_start(OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    TEST_ASSERT_TRUE(ov_overlay_focused());

    for (int d = 0; d < 4; d++) {
        ov_mod_key((ImGuiKey)(ImGuiKey_1 + d), ImGuiMod_Alt);
        TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_INSERT_0 + d, overlay_actions_test_take_last());
        TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab(), "Alt + digit does not switch tabs");
    }
    ov_mod_key(ImGuiKey_0, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "Alt+0 is not a drive shortcut");
    ov_mod_key(ImGuiKey_2, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_EJECT_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_umount", name);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    ov_mod_key(ImGuiKey_3, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "an empty drive has nothing to eject");

    g_emulator.max_speed = false;
    ov_mod_key(ImGuiKey_M, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_emulator.max_speed);
    g_emulator.max_speed = false;
    ov_mod_key(ImGuiKey_M, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "Alt+Shift+M is not in the table");
    ov_mod_key(ImGuiKey_F8, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_SAVE, overlay_actions_test_take_last());
    ov_mod_key(ImGuiKey_F, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT, overlay_actions_test_take_last());
    /* Alt+Ctrl+Enter přepne fullscreen a nezvolí prvek s fokusem (řádek FDD 0) */
    ov_mod_key(ImGuiKey_Enter, ImGuiMod_Alt | ImGuiMod_Ctrl);
    TEST_ASSERT_EQUAL_INT(1, s_fullscreen_calls);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FULLSCREEN, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* číslice bez Alt přepínají taby jako dřív */
    ov_key(ImGuiKey_2);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    ov_key(ImGuiKey_1);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());

    /* zkratky, které overlay zavírají */
    ov_mod_key(ImGuiKey_C, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_WINDOW, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    g_gui->showVirtualCmtWindow = false;
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    ov_key(ImGuiKey_F12);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_RESET, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    ov_mod_key(ImGuiKey_F9, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_LOAD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_quickload_calls);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* replay: zkratky médií a Quick Load nic nedělají */
    imgui_overlay_open();
    media_start(OVERLAY_FIXTURE_REPLAY, 1280.0f, 720.0f);
    media_drain();
    ov_mod_key(ImGuiKey_1, ImGuiMod_Alt);
    ov_mod_key(ImGuiKey_2, ImGuiMod_Alt | ImGuiMod_Shift);
    ov_mod_key(ImGuiKey_F9, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, s_quickload_calls);
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
}

/**
 * Fokus tabu Media (K07): výchozí fokus má řádek FDD 0; šipky jdou po
 * řádcích, Enter volí "Change" / "Insert disk"; šipka vpravo přejde na
 * tlačítko vysunutí; po přepnutí tabu a zpět je fokus zase na FDD 0.
 */
void test_media_focus(void)
{
    const char *name;
    st_DEVICE_REQ_ARGS args;
    imgui_overlay_open();
    media_start(OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();

    int sheet_drive = -1;
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive), "Enter on a drive row opens its sheet");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sheet_drive, "the default focus is the FDD 0 row");
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    /* po zavření listu má fokus zase řádek, který ho otevřel */
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sheet_drive, "arrow down moves to the next drive");
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    /* vpravo z řádku FDD 1: uložení, vysunutí (tlačítko Change zastávkou není) */
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_FDD_SAVE_1, overlay_actions_test_take_last(), "arrow right moves to the save button");
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_sync", name);
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_EJECT_1, overlay_actions_test_take_last());

    /* jiný tab a zpět: fokus znovu na FDD 0 */
    ov_key(ImGuiKey_3);
    ov_key(ImGuiKey_1);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_FDD, overlay_sheet_target(&sheet_drive));
    TEST_ASSERT_EQUAL_INT(0, sheet_drive);
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_TRUE(ov_overlay_focused());

    /* Esc zavírá i s fokusem na řádku */
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
}

/**
 * Tentýž soubor obrazu ve dvou mechanikách nad skutečným jádrem: druhá
 * mechanika má v kopii stavu ochranu kvůli sdílení, řádek nese poznámku
 * "Shared with FDD 0, write-protected" a přepínač Write protect je zakázaný
 * s důvodem, dokud první mechanika smí zapisovat. Po vysunutí první
 * mechaniky poznámka zmizí, ochrana zůstane a přepínač ji sundá.
 */
void test_media_shared_image(void)
{
    const char *name = NULL;
    st_DEVICE_REQ_ARGS args;
    st_OVERLAY_VIEW v;
    char text[192];
    g_fdc[FDC0].connected = FDC_CONNECTED;
    fdc_cfg_set_readonly(&g_fdc[FDC0], 0, 0);
    fdc_cfg_set_readonly(&g_fdc[FDC0], 1, 0);
    gchar *dsk = g_strdup(media_test_dsk());
    fdc_mount_dskfile(&g_fdc[FDC0], 0, dsk);
    fdc_mount_dskfile(&g_fdc[FDC0], 1, dsk);
    g_free(dsk);

    /* kopie stavu */
    media_fill(&v);
    TEST_ASSERT_FALSE(v.fdd[0].readonly);
    TEST_ASSERT_FALSE(v.fdd[0].shared_readonly);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[0].shared_with);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[0].wp_blocker);
    TEST_ASSERT_TRUE(v.fdd[1].readonly);
    TEST_ASSERT_TRUE(v.fdd[1].shared_readonly);
    TEST_ASSERT_FALSE_MESSAGE(v.fdd[1].user_readonly, "the user option of the drive is not touched");
    TEST_ASSERT_EQUAL_INT(0, v.fdd[1].shared_with);
    TEST_ASSERT_EQUAL_INT(0, v.fdd[1].wp_blocker);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[2].shared_with);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[2].wp_blocker);

    /* text řádku: poznámka místo "Read-only"; první mechanika bez poznámky */
    overlay_media_fdd_info_text(&v.fdd[1], text, sizeof(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "Shared with FDD 0, write-protected"), text);
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "Read-only"), text);
    overlay_media_fdd_info_text(&v.fdd[0], text, sizeof(text));
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "Shared"), text);
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "Read-only"), text);

    /* přepínač: zapnutý, zakázaný s důvodem; první mechaniku nic neomezuje */
    TEST_ASSERT_TRUE(overlay_fdd_wp_on(&v.fdd[1]));
    TEST_ASSERT_EQUAL_STRING("The image is also inserted in FDD 0, which may write to it.", overlay_action_reason(OVL_ACTION_FDD_WP_1, &v));
    TEST_ASSERT_FALSE(overlay_fdd_wp_on(&v.fdd[0]));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_FDD_WP_0, &v));

    /* okno nad skutečným jádrem: řádek se stavem je vidět, klik na přepínač nic nezařadí */
    imgui_overlay_open();
    s_display = ImVec2(1280.0f, 720.0f);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();
    TEST_ASSERT_TRUE_MESSAGE(media_rect("fdd1_state", NULL, NULL), "the row shows the shared note");
    media_click("fdd1_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    ov_key(ImGuiKey_Escape);

    /* první mechanika soubor vysune: poznámka zmizí, ochrana zůstane a jde sundat */
    fdc_umount(&g_fdc[FDC0], 0);
    media_fill(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.fdd[1].readonly, "no silent unlock");
    TEST_ASSERT_TRUE(v.fdd[1].shared_readonly);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[1].shared_with);
    TEST_ASSERT_EQUAL_INT(-1, v.fdd[1].wp_blocker);
    overlay_media_fdd_info_text(&v.fdd[1], text, sizeof(text));
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "Shared"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "Read-only"), text);
    TEST_ASSERT_TRUE(overlay_fdd_wp_on(&v.fdd[1]));
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_FDD_WP_1, &v));

    for (int i = 0; i < 2; i++) ov_frame();
    media_click("fdd1_insert");
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_WP_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_set_readonly", name);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, args.i[2], "the toggle turns the protection off although the user option is off");
    ov_key(ImGuiKey_Escape);
    ov_key(ImGuiKey_Escape);

    /* kopie bez sdílení: obecný text */
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    v.fdd[0].readonly = true;
    overlay_media_fdd_info_text(&v.fdd[0], text, sizeof(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "Read-only"), text);
    overlay_media_fdd_info_text(NULL, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("", text);

    fdc_umount(&g_fdc[FDC0], 1);
}

/**
 * Stav zařízení změněný jiným vláknem mezi dvěma snímky (Review Focus 5):
 * disketa vysunutá zvenčí (jako MCP) při otevřeném overlayi nad skutečným
 * jádrem. Další snímek kreslí prázdnou mechaniku; nic nespadne.
 */
void test_media_external_eject(void)
{
    g_fdc[FDC0].connected = FDC_CONNECTED;
    gchar *dsk = g_strdup(media_test_dsk());
    fdc_mount_dskfile(&g_fdc[FDC0], 1, dsk);
    g_free(dsk);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));

    imgui_overlay_open();
    s_display = ImVec2(1280.0f, 720.0f);
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(media_rect("fdd1_eject", NULL, NULL), "the mounted drive has an eject button");
    TEST_ASSERT_FALSE(media_rect("fdd0_eject", NULL, NULL));

    fdc_umount(&g_fdc[FDC0], 1);
    ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(media_rect("fdd1_eject", NULL, NULL), "the next frame shows the drive empty");
    ImGuiContext &g = *GImGui;
    TEST_ASSERT_EQUAL_INT(0, g.CurrentWindowStack.Size);

    /* odpojení řadiče zvenčí: karta se zakáže s důvodem */
    g_fdc[FDC0].connected = FDC_DISCONNECTED;
    ov_frame();
    TEST_ASSERT_TRUE(media_rect("card_reason", NULL, NULL));
    TEST_ASSERT_FALSE(media_rect("fdd0_keycap", NULL, NULL));
    /* a začátek záznamu vstupů také */
    g_fdc[FDC0].connected = FDC_CONNECTED;
    g_replay_mode = REPLAY_MODE_RECORDING;
    ov_frame();
    TEST_ASSERT_TRUE(media_rect("card_reason", NULL, NULL));
    g_replay_mode = REPLAY_MODE_OFF;
    imgui_overlay_close();
}

/* ================================================================
 * Nedávná média a výsuvný list výměny média
 * ================================================================ */

/** @brief Dočasné soubory médií vytvořené testy (cesty vlastní pole; maže je main). */
static GPtrArray *s_tmp_files = NULL;

/**
 * @brief Vytvoří v dočasném adresáři testu soubor média a vrátí jeho cestu.
 * @param name Jméno souboru; přípona ".dsk" dá platný obraz diskety, jiná krátký soubor.
 * @return Cesta (platí do konce procesu).
 */
static const char *tmp_media(const char *name)
{
    if (!s_tmpdir) s_tmpdir = g_dir_make_tmp("mz-overlay-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_tmpdir);
    if (!s_tmp_files) s_tmp_files = g_ptr_array_new_with_free_func(g_free);
    char *path = g_build_filename(s_tmpdir, name, NULL);
    for (guint i = 0; i < s_tmp_files->len; i++) {
        if (strcmp((const char *)g_ptr_array_index(s_tmp_files, i), path) == 0) {
            g_free(path);
            path = (char *)g_ptr_array_index(s_tmp_files, i);
            if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
                if (g_str_has_suffix(path, ".dsk")) write_test_dsk(path);
                else TEST_ASSERT_TRUE(g_file_set_contents(path, "media", 5, NULL));
            }
            return path;
        }
    }
    if (g_str_has_suffix(path, ".dsk")) write_test_dsk(path);
    else TEST_ASSERT_TRUE(g_file_set_contents(path, "media", 5, NULL));
    g_ptr_array_add(s_tmp_files, path);
    return path;
}

/** @brief Kopie stavu se skutečnými soubory v historii médií (sheet_view_real()). */
static st_OVERLAY_VIEW s_real_view;

/**
 * @brief Přidá do s_real_view položku nedávných médií se skutečným souborem.
 * @param kind Druh média. @param path Cesta. @param age_s Stáří v sekundách.
 */
static void real_recent(en_OVERLAY_MEDIA_KIND kind, const char *path, long long age_s)
{
    TEST_ASSERT_TRUE(s_real_view.recent_count < OVERLAY_VIEW_RECENT_MAX);
    st_OVERLAY_VIEW_RECENT *r = &s_real_view.recent[s_real_view.recent_count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    overlay_view_copy_text(r->path, sizeof(r->path), path);
    gchar *base = g_path_get_basename(path);
    overlay_view_copy_text(r->name, sizeof(r->name), base);
    g_free(base);
    r->time = s_real_view.recent_now - age_s;
    r->checked = true;
    r->checked_us = (long long)g_get_monotonic_time();
    r->is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
}

/**
 * @brief Začne kreslit tab Media z testovacího stavu, jehož historie médií ukazuje na skutečné soubory.
 *
 * Historie: diskety alpha-disk.dsk (nejnovější), twin-peaks.dsk,
 * twilight.dsk, páska tape-one.mzf a Quick Disk quick.mzq, v tomto pořadí.
 *
 * @param base Testovací stav, ze kterého se bere vše kromě historie.
 */
static void sheet_view_real(en_OVERLAY_FIXTURE base)
{
    overlay_view_make_fixture(&s_real_view, base);
    s_real_view.recent_count = 0;
    s_real_view.recent_now = 1800000000ll;
    real_recent(OVERLAY_MEDIA_DSK, tmp_media("alpha-disk.dsk"), 60);
    real_recent(OVERLAY_MEDIA_DSK, tmp_media("twin-peaks.dsk"), 7200);
    real_recent(OVERLAY_MEDIA_DSK, tmp_media("twilight.dsk"), 3 * 86400);
    real_recent(OVERLAY_MEDIA_CMT, tmp_media("tape-one.mzf"), 4 * 86400);
    real_recent(OVERLAY_MEDIA_QD, tmp_media("quick.mzq"), 5 * 86400);
    s_fixture = &s_real_view;
    s_display = ImVec2(1280.0f, 720.0f);
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    for (int i = 0; i < 4; i++) ov_frame();
}

/** @brief Stiskne klávesu znaku i se znakem, jak je posílá klávesnice (dva snímky a jeden na doručení znaku). */
static void ov_type(ImGuiKey key, unsigned int c)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddInputCharacter(c);
    ov_frame();
    io.AddKeyEvent(key, false);
    ov_frame();
    ov_frame();
}

/**
 * @brief Ověří, že ve frontě zásahů čeká právě vložení diskety do dané mechaniky.
 * @param drive Mechanika. @param path Očekávaná cesta.
 */
static void expect_fdc_mount(int drive, const char *path)
{
    const char *name = NULL;
    st_DEVICE_REQ_ARGS args;
    TEST_ASSERT_TRUE_MESSAGE(device_request_test_take_pending(&name, &args), "a mount request is queued");
    TEST_ASSERT_EQUAL_STRING("fdc_mount", name);
    TEST_ASSERT_EQUAL_INT(FDC0, args.i[0]);
    TEST_ASSERT_EQUAL_INT(drive, args.i[1]);
    TEST_ASSERT_EQUAL_STRING(path, args.str);
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
}

/**
 * Kopie nedávných médií nad skutečnou historií jádra: pořadí od naposledy
 * vloženého přes druhy, jména, existence souborů (ověřená na žádost a po
 * změně historie), odstranění, strop 12 na druh a cesta, která se do kopie
 * nevejde.
 */
void test_recent_view_fill(void)
{
    static st_OVERLAY_VIEW v;
    const char *dsk = tmp_media("fill-one.dsk"), *tape = tmp_media("fill-tape.mzf"), *qd = tmp_media("fill-qd.mzq");
    char *gone = g_build_filename(s_tmpdir, "fill-gone.dsk", NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(gone, "x", 1, NULL));

    media_fill(&v);
    TEST_ASSERT_EQUAL_UINT(0, v.recent_count);

    recent_media_add(RECENT_MEDIA_DSK, dsk);
    recent_media_add(RECENT_MEDIA_CMT, tape);
    recent_media_add(RECENT_MEDIA_DSK, gone);
    recent_media_add(RECENT_MEDIA_QD, s_tmpdir); /* adresář virtuálního Quick Disku */
    recent_media_add(RECENT_MEDIA_QD, qd);
    media_fill(&v);
    TEST_ASSERT_EQUAL_UINT(5, v.recent_count);
    TEST_ASSERT_TRUE(v.recent_now > 0);
    for (unsigned i = 1; i < v.recent_count; i++) TEST_ASSERT_TRUE_MESSAGE(v.recent[i - 1].time >= v.recent[i].time, "newest first");
    int n_kind[OVERLAY_MEDIA_KIND_COUNT] = {0, 0, 0};
    bool seen_dir = false;
    for (unsigned i = 0; i < v.recent_count; i++) {
        const st_OVERLAY_VIEW_RECENT *r = &v.recent[i];
        n_kind[r->kind]++;
        TEST_ASSERT_FALSE_MESSAGE(r->missing, r->path);
        gchar *base = g_path_get_basename(r->path);
        TEST_ASSERT_EQUAL_STRING(base, r->name);
        g_free(base);
        TEST_ASSERT_TRUE(r->time > 0 && r->time <= v.recent_now);
        if (strcmp(r->path, s_tmpdir) == 0) {
            seen_dir = true;
            TEST_ASSERT_TRUE(r->is_dir);
            TEST_ASSERT_EQUAL_INT(OVERLAY_MEDIA_QD, r->kind);
        } else {
            TEST_ASSERT_FALSE(r->is_dir);
        }
    }
    TEST_ASSERT_TRUE(seen_dir);
    TEST_ASSERT_EQUAL_INT(2, n_kind[OVERLAY_MEDIA_DSK]);
    TEST_ASSERT_EQUAL_INT(1, n_kind[OVERLAY_MEDIA_CMT]);
    TEST_ASSERT_EQUAL_INT(2, n_kind[OVERLAY_MEDIA_QD]);
    /* položky téhož druhu v pořadí historie jádra */
    const st_OVERLAY_VIEW_RECENT *first_dsk = NULL;
    for (unsigned i = 0; i < v.recent_count && !first_dsk; i++)
        if (v.recent[i].kind == OVERLAY_MEDIA_DSK) first_dsk = &v.recent[i];
    TEST_ASSERT_EQUAL_STRING(gone, first_dsk->path);

    /* soubor zmizel: bez žádosti se existence znovu ověří nejdřív za sekundu, na žádost hned */
    g_remove(gone);
    overlay_view_recent_recheck();
    media_fill(&v);
    int missing = 0;
    for (unsigned i = 0; i < v.recent_count; i++)
        if (v.recent[i].missing) {
            missing++;
            TEST_ASSERT_EQUAL_STRING(gone, v.recent[i].path);
        }
    TEST_ASSERT_EQUAL_INT(1, missing);

    /* odstranění z historie se v kopii projeví při příštím plnění */
    TEST_ASSERT_TRUE(recent_media_remove_path(RECENT_MEDIA_DSK, gone));
    media_fill(&v);
    TEST_ASSERT_EQUAL_UINT(4, v.recent_count);

    /* strop 12 na druh a cesta delší než pole kopie (položka se vynechá, nezkracuje) */
    for (int i = 0; i < 15; i++) {
        char name[64];
        snprintf(name, sizeof(name), "C:/nowhere/tape-%02d.mzf", i);
        recent_media_add(RECENT_MEDIA_CMT, name);
    }
    GString *lng = g_string_new("C:/");
    while (lng->len < OVERLAY_VIEW_PATH_SIZE + 10) g_string_append(lng, "very-long-directory/");
    g_string_append(lng, "disk.dsk");
    recent_media_add(RECENT_MEDIA_DSK, lng->str);
    g_string_free(lng, TRUE);
    media_fill(&v);
    n_kind[0] = n_kind[1] = n_kind[2] = 0;
    for (unsigned i = 0; i < v.recent_count; i++) {
        n_kind[v.recent[i].kind]++;
        TEST_ASSERT_TRUE(strlen(v.recent[i].path) < OVERLAY_VIEW_PATH_SIZE);
    }
    TEST_ASSERT_EQUAL_INT(OVERLAY_VIEW_RECENT_PER_KIND, n_kind[OVERLAY_MEDIA_CMT]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n_kind[OVERLAY_MEDIA_DSK], "the path that does not fit is left out");
    TEST_ASSERT_TRUE(v.recent_count <= OVERLAY_VIEW_RECENT_MAX);
    g_free(gone);
}

/**
 * @brief Geometrie otevřeného listu: list leží v ploše obsahu, jeho prvky v listu a nepřekrývají se.
 * @param msg  Popis situace do zprávy.
 * @param rows Nejmenší očekávaný počet řádků seznamu.
 * @return Počet řádků seznamu.
 */
static int sheet_check_geometry(const char *msg, int rows)
{
    static const char *const parts[] = {"sheet_title", "sheet_close", "sheet_now", "sheet_filter", "sheet_label", "sheet_row0", "sheet_row1", "sheet_row2", "sheet_row3",
                                        "sheet_row4",  "sheet_row5",  "sheet_empty", "sheet_browse", "sheet_eject", "sheet_wp",   "sheet_wp_label"};
    ImVec2 smin, smax, cmin, cmax;
    char m[320];
    TEST_ASSERT_TRUE_MESSAGE(media_rect("sheet", &smin, &smax), msg);
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
    snprintf(m, sizeof(m), "%s: sheet (%.1f %.1f %.1f %.1f) in content (%.1f %.1f %.1f %.1f)", msg, smin.x, smin.y, smax.x, smax.y, cmin.x, cmin.y, cmax.x, cmax.y);
    TEST_ASSERT_TRUE_MESSAGE(smin.x >= cmin.x - 0.5f && smax.x <= cmax.x + 0.5f && smin.y >= cmin.y - 0.5f && smax.y <= cmax.y + 0.5f, m);
    const int n = media_check_group(msg, "sheet", floorf(overlay_px(16.0f)), parts, (int)(sizeof(parts) / sizeof(parts[0])));
    TEST_ASSERT_TRUE_MESSAGE(n >= 6, msg); /* titulek, Esc, Now, filtr, nadpis, Browse, Eject a seznam nebo text */
    int count = 0;
    for (int i = 0; i < 6; i++) {
        char name[16];
        snprintf(name, sizeof(name), "sheet_row%d", i);
        if (media_rect(name, NULL, NULL)) count++;
    }
    TEST_ASSERT_TRUE_MESSAGE(count >= rows && count <= 6, msg);
    /* seznam, nebo text prázdného stavu, ne obojí */
    TEST_ASSERT_TRUE_MESSAGE((count == 0) == media_rect("sheet_empty", NULL, NULL), msg);
    /* text filtru leží v poli filtru */
    ImVec2 fmin, fmax, tmin, tmax;
    if (media_rect("sheet_filter_text", &tmin, &tmax)) {
        TEST_ASSERT_TRUE(media_rect("sheet_filter", &fmin, &fmax));
        TEST_ASSERT_TRUE_MESSAGE(tmin.x >= fmin.x && tmax.x <= fmax.x && tmin.y >= fmin.y && tmax.y <= fmax.y, msg);
    }
    return count;
}

/**
 * Geometrie listu (Review Focus 3 a 4): u každé mechaniky, u kazety a u
 * Quick Disku při 640 x 480, 1280 x 720 a 1920 x 1080, s konceptem, s
 * dlouhými jmény a s texty o 40 % delšími. V širokém okně list nezakrývá
 * tlačítko, které ho otevřelo (u mechanik je vpravo od něj, u kazety a Quick
 * Disku vlevo); v úzkém zabírá šířku obsahu. Štítky číslic jsou u položek
 * jen při prázdném filtru. Volba mechaniky leží v ploše obsahu.
 */
void test_sheet_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {1920.0f, 1080.0f}};
    const en_OVERLAY_FIXTURE fixtures[] = {OVERLAY_FIXTURE_DEFAULT, OVERLAY_FIXTURE_LONG_TEXTS, OVERLAY_FIXTURE_EMPTY};
    imgui_overlay_open();
    for (int stretch = 0; stretch < 2; stretch++) {
        overlay_test_set_stretch(stretch != 0);
        for (const float *sz : sizes) {
            for (en_OVERLAY_FIXTURE f : fixtures) {
                const bool wide = sz[0] >= 1280.0f;
                for (int target = 0; target < 6; target++) {
                    media_start(f, sz[0], sz[1]);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%.0fx%.0f %s%s target %d", sz[0], sz[1], s_fixture_names[f], stretch ? " +40%" : "", target);
                    const char *anchor_name = NULL;
                    char anchor_buf[24];
                    if (target < 4) {
                        overlay_sheet_open(OVL_SHEET_FDD, target);
                        snprintf(anchor_buf, sizeof(anchor_buf), "fdd%d_insert", target);
                        anchor_name = anchor_buf;
                    } else if (target == 4) {
                        overlay_sheet_open(OVL_SHEET_CMT, 0);
                        anchor_name = "cmt_change";
                    } else {
                        if (s_fixture_view.qd.mounted) continue; /* vložený Quick Disk list nenabízí */
                        overlay_sheet_open(OVL_SHEET_QD, 0);
                        anchor_name = "qd_insert";
                    }
                    for (int i = 0; i < 4; i++) ov_frame();
                    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), msg);
                    const int min_rows = (f == OVERLAY_FIXTURE_EMPTY) ? 0 : ((f == OVERLAY_FIXTURE_LONG_TEXTS) ? 3 : 1);
                    const int rows = sheet_check_geometry(msg, min_rows);
                    if (f == OVERLAY_FIXTURE_LONG_TEXTS && sz[1] >= 720.0f) TEST_ASSERT_EQUAL_INT_MESSAGE(6, rows, msg);
                    if (f == OVERLAY_FIXTURE_DEFAULT && target < 4 && sz[1] >= 720.0f) TEST_ASSERT_EQUAL_INT_MESSAGE(6, rows, "six recent disks of the concept");
                    /* ochrana proti zápisu jen u mechanik a Quick Disku */
                    TEST_ASSERT_EQUAL_INT_MESSAGE(target != 4 ? 1 : 0, media_rect("sheet_wp", NULL, NULL) ? 1 : 0, msg);

                    ImVec2 smin, smax, amin, amax, cmin, cmax;
                    TEST_ASSERT_TRUE(media_rect("sheet", &smin, &smax));
                    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
                    if (wide) {
                        TEST_ASSERT_TRUE_MESSAGE(media_rect(anchor_name, &amin, &amax), msg);
                        if (target < 4) TEST_ASSERT_TRUE_MESSAGE(smin.x >= amax.x, "the sheet is right of the drive button");
                        else TEST_ASSERT_TRUE_MESSAGE(smax.x <= amin.x, "the sheet is left of the tape or Quick Disk button");
                        TEST_ASSERT_TRUE_MESSAGE(smax.x - smin.x < (cmax.x - cmin.x) * 0.6f, msg);
                    } else {
                        TEST_ASSERT_TRUE_MESSAGE(smax.x - smin.x > (cmax.x - cmin.x) * 0.9f, "the sheet takes the width of a narrow window");
                    }
                    /* zásobníky ImGui jsou vyvážené i s listem */
                    TEST_ASSERT_EQUAL_INT(0, GImGui->CurrentWindowStack.Size);
                    TEST_ASSERT_EQUAL_INT(0, GImGui->FontStack.Size);
                    TEST_ASSERT_EQUAL_INT(0, GImGui->ColorStack.Size);
                    TEST_ASSERT_EQUAL_INT(0, GImGui->StyleVarStack.Size);
                    overlay_sheet_reset();
                }

                /* volba mechaniky u první položky pruhu, která je obrazem diskety */
                media_start(f, sz[0], sz[1]);
                for (unsigned i = 0; i < s_fixture_view.recent_count; i++) {
                    if (s_fixture_view.recent[i].kind != OVERLAY_MEDIA_DSK) continue;
                    overlay_sheet_open_pick(&s_fixture_view.recent[i], (int)i);
                    for (int k = 0; k < 4; k++) ov_frame();
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%.0fx%.0f %s%s pick", sz[0], sz[1], s_fixture_names[f], stretch ? " +40%" : "");
                    static const char *const parts[] = {"sheet_title", "sheet_close", "pick_name", "pick_fdd0", "pick_fdd1", "pick_fdd2", "pick_fdd3"};
                    ImVec2 pmin, pmax, cmin, cmax;
                    TEST_ASSERT_TRUE_MESSAGE(media_rect("pick", &pmin, &pmax), msg);
                    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
                    TEST_ASSERT_TRUE_MESSAGE(pmin.x >= cmin.x - 0.5f && pmax.x <= cmax.x + 0.5f && pmin.y >= cmin.y - 0.5f && pmax.y <= cmax.y + 0.5f, msg);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(7, media_check_group(msg, "pick", floorf(overlay_px(16.0f)), parts, 7), msg);
                    overlay_sheet_reset();
                    break;
                }
            }
        }
    }
    overlay_test_set_stretch(false);
    imgui_overlay_close();
}

/**
 * Klávesy listu (K01): číslice a Enter zařadí vložení správného souboru do
 * správné mechaniky; psaní jde do filtru, štítky číslic pak neplatí a
 * číslice se píší do filtru; Backspace maže; Esc zavře jen list; při
 * otevřeném listu nefungují zkratky tabů ani akcí; F10 zavře overlay i s
 * listem.
 */
void test_sheet_keys(void)
{
    const char *d1 = tmp_media("alpha-disk.dsk"), *d2 = tmp_media("twin-peaks.dsk"), *d3 = tmp_media("twilight.dsk");
    imgui_overlay_open();
    sheet_view_real(OVERLAY_FIXTURE_DEFAULT);
    media_drain();
    int drive = -1;

    /* číslice vloží položku do mechaniky, u které je list otevřený, a tab nepřepne */
    media_click("fdd1_insert");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_FDD, overlay_sheet_target(&drive));
    TEST_ASSERT_EQUAL_INT(1, drive);
    TEST_ASSERT_TRUE(media_rect("sheet_row2", NULL, NULL));
    TEST_ASSERT_FALSE_MESSAGE(media_rect("sheet_row3", NULL, NULL), "three recent disks");
    ov_type(ImGuiKey_2, '2');
    expect_fdc_mount(1, d2);
    TEST_ASSERT_FALSE_MESSAGE(overlay_sheet_is_open(), "inserting closes the sheet");
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab(), "a digit in the sheet does not switch tabs");
    TEST_ASSERT_EQUAL_INT(0, s_tape_open_calls);

    /* Enter vloží vybranou položku: po otevření první, po šipce dolů druhou */
    media_click("fdd3_insert");
    ov_key(ImGuiKey_Enter);
    expect_fdc_mount(3, d1);
    media_click("fdd0_insert");
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_Enter);
    expect_fdc_mount(0, d3);
    /* numerická klávesnice jako číslice */
    media_click("fdd2_insert");
    ov_type(ImGuiKey_Keypad3, '3');
    expect_fdc_mount(2, d3);
    /* číslice bez položky nic nevloží a list nechá otevřený; tab se nepřepne */
    media_click("fdd2_insert");
    ov_type(ImGuiKey_5, '5');
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", overlay_sheet_test_filter(), "digits 1 to 6 never start a filter");
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    /* psaní jde do filtru: zůstanou jen vyhovující položky (bez ohledu na velikost písmen) */
    ov_type(ImGuiKey_T, 'T');
    ov_type(ImGuiKey_W, 'w');
    TEST_ASSERT_EQUAL_STRING("Tw", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE(media_rect("sheet_row1", NULL, NULL));
    TEST_ASSERT_FALSE_MESSAGE(media_rect("sheet_row2", NULL, NULL), "two disks match");
    TEST_ASSERT_TRUE(media_rect("sheet_filter_text", NULL, NULL));
    /* s neprázdným filtrem číslice nevkládá, píše se do filtru */
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_EQUAL_STRING("Tw1", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), "a digit does not insert while a filter is typed");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_TRUE_MESSAGE(media_rect("sheet_empty", NULL, NULL), "nothing matches");
    /* Backspace maže poslední znak; mezera je znak filtru */
    ov_key(ImGuiKey_Backspace);
    TEST_ASSERT_EQUAL_STRING("Tw", overlay_sheet_test_filter());
    ov_type(ImGuiKey_Space, ' ');
    TEST_ASSERT_EQUAL_STRING("Tw ", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), "Space types into the filter and selects nothing");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    ov_key(ImGuiKey_Backspace);
    ov_type(ImGuiKey_I, 'i');
    TEST_ASSERT_EQUAL_STRING("Twi", overlay_sheet_test_filter());
    /* Enter vloží první vyhovující (twin-peaks.dsk je v historii před twilight.dsk) */
    ov_key(ImGuiKey_Enter);
    expect_fdc_mount(2, d2);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());

    /* nový list má filtr prázdný; znak s diakritikou a mazání po znacích UTF-8 */
    media_click("fdd0_insert");
    TEST_ASSERT_EQUAL_STRING("", overlay_sheet_test_filter());
    ov_type(ImGuiKey_A, 'a');
    ImGui::GetIO().AddInputCharacter(0x0159); /* ř */
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_EQUAL_STRING("a\xC5\x99", overlay_sheet_test_filter());
    ov_key(ImGuiKey_Backspace);
    TEST_ASSERT_EQUAL_STRING("a", overlay_sheet_test_filter());
    ov_key(ImGuiKey_Backspace);
    ov_key(ImGuiKey_Backspace);
    TEST_ASSERT_EQUAL_STRING("", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE(overlay_sheet_is_open());

    /* zkratky tabů a akcí jsou při otevřeném listu vypnuté */
    g_emulator.max_speed = false;
    ov_mod_key(ImGuiKey_M, ImGuiMod_Alt);
    ov_mod_key(ImGuiKey_2, ImGuiMod_Alt);
    ov_mod_key(ImGuiKey_1, ImGuiMod_Alt | ImGuiMod_Shift);
    ov_key(ImGuiKey_F12);
    ov_ctrl_key(ImGuiKey_Tab, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "no overlay action runs while the sheet is open");
    TEST_ASSERT_FALSE(g_emulator.max_speed);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    /* Esc zavře jen list, druhé Esc overlay */
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "Esc closes only the sheet");
    /* po zavření zkratky zase fungují */
    ov_key(ImGuiKey_2);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    ov_key(ImGuiKey_1);
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* F10 zavře overlay i s listem; nové otevření overlaye list neukáže */
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE(media_rect("sheet", NULL, NULL));

    /* klik na položku vloží, klik mimo list ho zavře, tlačítko Esc v titulku také */
    media_click("fdd3_insert");
    media_click("sheet_row1");
    expect_fdc_mount(3, d2);
    media_click("fdd3_insert");
    ImVec2 cmin, cmax, smin, smax;
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
    TEST_ASSERT_TRUE(media_rect("sheet", &smin, &smax));
    ov_click(ImVec2(cmin.x + 6.0f, cmax.y - 6.0f));
    TEST_ASSERT_FALSE_MESSAGE(overlay_sheet_is_open(), "a click outside closes the sheet");
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the click outside reaches nothing under the sheet");
    media_click("fdd3_insert");
    media_click("sheet_close");
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    imgui_overlay_close();
}

/**
 * Číslice listu podle klávesy, ne podle znaku (české rozložení: horní řada
 * bez Shiftu píše "+", "ě", "š", ...): klávesa 1 se znakem "+" a klávesa 2
 * se znakem "ě" při prázdném filtru vloží první a druhou položku a filtr
 * zůstane prázdný; klávesa bez položky nevloží nic a znak se zahodí; při
 * neprázdném filtru jde znak do filtru a nic se nevloží; se Shiftem klávesa
 * nevkládá a její znak (česky číslice) filtr začne; volba mechaniky
 * "Insert into" bere klávesu bez ohledu na znak.
 */
void test_sheet_keys_layout(void)
{
    const char *d1 = tmp_media("alpha-disk.dsk"), *d2 = tmp_media("twin-peaks.dsk");
    imgui_overlay_open();
    sheet_view_real(OVERLAY_FIXTURE_DEFAULT);
    media_drain();

    /* prázdný filtr: klávesa 1 + "+" vloží první položku */
    media_click("fdd1_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_type(ImGuiKey_1, 0x002B);
    expect_fdc_mount(1, d1);
    TEST_ASSERT_FALSE_MESSAGE(overlay_sheet_is_open(), "key 1 inserts on a Czech layout");
    /* klávesa 2 + "ě" vloží druhou položku */
    media_click("fdd2_insert");
    ov_type(ImGuiKey_2, 0x011B);
    expect_fdc_mount(2, d2);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click("fdd2_insert");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", overlay_sheet_test_filter(), "the character of a digit key never reaches the filter");

    /* klávesa bez položky (jsou tři): nic se nevloží a znak "ř" se zahodí */
    ov_type(ImGuiKey_5, 0x0159);
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    TEST_ASSERT_EQUAL_STRING("", overlay_sheet_test_filter());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());

    /* neprázdný filtr: znak klávesy číslice jde do filtru a nic se nevloží */
    ov_type(ImGuiKey_T, 't');
    ov_type(ImGuiKey_1, 0x002B);
    TEST_ASSERT_EQUAL_STRING("t+", overlay_sheet_test_filter());
    ov_type(ImGuiKey_2, 0x011B);
    TEST_ASSERT_EQUAL_STRING("t+\xC4\x9B", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), "a digit key does not insert while a filter is typed");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    for (int i = 0; i < 3; i++) ov_key(ImGuiKey_Backspace);
    TEST_ASSERT_EQUAL_STRING("", overlay_sheet_test_filter());

    /* Shift + klávesa číslice nevkládá; znak (na českém rozložení "1") filtr začne */
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Shift, true);
        ov_frame();
        ov_type(ImGuiKey_1, '1');
        io.AddKeyEvent(ImGuiMod_Shift, false);
        ov_frame();
    }
    TEST_ASSERT_EQUAL_STRING("1", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());

    /* volba mechaniky "Insert into": klávesa 2 se znakem "ě" vloží do FDD 1 */
    media_click("recent_chip0");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_PICK_DRIVE, overlay_sheet_target(NULL));
    ov_type(ImGuiKey_2, 0x011B);
    expect_fdc_mount(1, d1);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    imgui_overlay_close();
}

/**
 * Ovládání listu: Browse files... otevře dialog stejnou akcí jako zkratka
 * a list zavře; Eject vysune; přepínač Write protect zařadí změnu volby a
 * list nechá otevřený; list kazety vkládá přes automat akcí s páskou a
 * ochranu proti zápisu nemá; list Quick Disku vkládá zásahem qdisk_mount.
 */
void test_sheet_controls(void)
{
    const char *name = NULL;
    st_DEVICE_REQ_ARGS args;
    imgui_overlay_open();
    sheet_view_real(OVERLAY_FIXTURE_DEFAULT);
    media_drain();

    /* mechanika s disketou: Browse, Eject, Write protect */
    media_click("fdd1_insert");
    media_click("sheet_browse");
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_FDD_INSERT_1, overlay_actions_test_take_last(), "Browse files... is the file dialog of that drive");
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click("fdd1_insert");
    media_click("sheet_eject");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_EJECT_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_umount", name);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click("fdd1_insert");
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_WP_1, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("fdc_set_readonly", name);
    TEST_ASSERT_EQUAL_INT(1, args.i[1]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, args.i[2], "the toggle turns the protection on");
    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), "the toggle keeps the sheet open");
    /* zapnutá volba se přepíná zpět na vypnutou */
    s_real_view.fdd[1].user_readonly = true;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("sheet_wp");
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_INT(0, args.i[2]);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_WP_1, overlay_actions_test_take_last());
    /* soubor chráněný na disku: volbu nejde změnit (jako klasické menu) */
    s_real_view.fdd[1].fs_readonly = true;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    ov_key(ImGuiKey_Escape);

    /* prázdná mechanika: Eject nic nedělá, ochranu jde nastavit předem */
    media_click("fdd2_insert");
    media_click("sheet_eject");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_WP_2, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_INT(2, args.i[1]);
    ov_key(ImGuiKey_Escape);

    /* kazeta: vložení přes automat akcí s páskou (dotazy na neuložené změny), bez ochrany proti zápisu, Eject */
    media_click("cmt_change");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_CMT, overlay_sheet_target(NULL));
    TEST_ASSERT_TRUE(media_rect("sheet_row0", NULL, NULL));
    TEST_ASSERT_FALSE_MESSAGE(media_rect("sheet_row1", NULL, NULL), "only tapes are offered");
    TEST_ASSERT_FALSE(media_rect("sheet_wp", NULL, NULL));
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_EQUAL_INT(1, s_tape_open_calls);
    TEST_ASSERT_EQUAL_STRING(tmp_media("tape-one.mzf"), s_tape_open_path);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_FALSE_MESSAGE(device_request_test_take_pending(NULL, NULL), "the tape is opened by the tape action flow, not by the sheet");
    media_click("cmt_change");
    media_click("sheet_eject");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CMT_EJECT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(IMGUI_CMT_TAPE_ACTION_EJECT, s_tape_action_last);
    /* jiný dotaz automatu běží: páska se nevymění, list se zavře se zprávou */
    s_tape_open_result = false;
    media_click("cmt_change");
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT(2, s_tape_open_calls);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    s_tape_open_result = true;

    /* Quick Disk: jen do prázdné mechaniky; vložení zásahem qdisk_mount; ochrana proti zápisu */
    media_click("qd_insert");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_QD, overlay_sheet_target(NULL));
    TEST_ASSERT_TRUE(media_rect("sheet_row0", NULL, NULL));
    TEST_ASSERT_FALSE(media_rect("sheet_row1", NULL, NULL));
    media_click("sheet_wp");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QD_WP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_set_write_protected", name);
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_mount", name);
    TEST_ASSERT_EQUAL_STRING(tmp_media("quick.mzq"), args.str);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, args.i[0], "an image, not a directory");
    TEST_ASSERT_FALSE(overlay_sheet_is_open());

    /* virtuální Quick Disk nabízí adresáře, ne obrazy */
    s_real_view.qd.directory = true;
    real_recent(OVERLAY_MEDIA_QD, s_tmpdir, 6 * 86400);
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("qd_insert");
    TEST_ASSERT_TRUE(media_rect("sheet_row0", NULL, NULL));
    TEST_ASSERT_FALSE(media_rect("sheet_row1", NULL, NULL));
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_mount", name);
    TEST_ASSERT_EQUAL_STRING(s_tmpdir, args.str);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, args.i[0], "a directory");

    /* cíl listu přestane být dostupný (začátek záznamu vstupů): list se sám zavře */
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    s_real_view.replay_active = true;
    overlay_view_copy_text(s_real_view.media_disabled_reason, sizeof(s_real_view.media_disabled_reason), "Not available while a replay is active.");
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    /* a jiný tab ho zavře také */
    s_real_view.replay_active = false;
    s_real_view.media_disabled_reason[0] = '\0';
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_click_item("tab2");
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_DISPLAY, imgui_overlay_get_tab());
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    imgui_overlay_close();
}

/**
 * Soubor, který už neexistuje (nad skutečnou historií jádra): položka je v
 * listu, při pokusu o vložení se z historie odstraní se zprávou, nic se
 * nezařadí a list zůstane otevřený s kratším seznamem. Existující soubor
 * se potom vloží skutečně a jádro ho zapíše na začátek historie.
 */
void test_sheet_missing_file(void)
{
    const char *keep = tmp_media("keep.dsk");
    char *gone = g_build_filename(s_tmpdir, "gone.dsk", NULL);
    write_test_dsk(gone);
    g_fdc[FDC0].connected = FDC_CONNECTED;
    recent_media_add(RECENT_MEDIA_DSK, keep);
    recent_media_add(RECENT_MEDIA_DSK, gone);

    imgui_overlay_open();
    s_display = ImVec2(1280.0f, 720.0f);
    for (int i = 0; i < 4; i++) ov_frame();
    media_drain();
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(media_rect("sheet_row1", NULL, NULL));

    g_remove(gone);
    /* existenci zjišťuje pracovní vlákno: list se o zmizení dozví z jeho dalšího kola */
    ov_frames_settled();
    ov_type(ImGuiKey_1, '1'); /* gone.dsk je naposledy vložená */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, overlay_sheet_test_take_missing_reports(), "the missing file is reported");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, recent_media_count(RECENT_MEDIA_DSK), "and removed from the history");
    TEST_ASSERT_EQUAL_STRING(keep, recent_media_get(RECENT_MEDIA_DSK, 0));
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_TRUE_MESSAGE(overlay_sheet_is_open(), "the sheet stays open");
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_TRUE(media_rect("sheet_row0", NULL, NULL));
    TEST_ASSERT_FALSE_MESSAGE(media_rect("sheet_row1", NULL, NULL), "the removed item is gone from the list");

    /* existující soubor: zásah provede emu vlákno, disketa je v mechanice a v historii nahoře */
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_EQUAL_INT(0, overlay_sheet_test_take_missing_reports());
    device_request_poll();
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_STRING(keep, fdc_get_dsk_filepath(&g_fdc[FDC0], 0));
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(media_rect("fdd0_eject", NULL, NULL), "the drive shows the inserted disk");
    TEST_ASSERT_TRUE_MESSAGE(media_rect("recent_chip0", NULL, NULL), "the strip shows the history of the core");
    TEST_ASSERT_FALSE(media_rect("recent_chip1", NULL, NULL));
    imgui_overlay_close();
    g_free(gone);
}

/**
 * Pruh nedávných médií (K09): položky s tooltipem, páska a Quick Disk se
 * vloží rovnou, u obrazu diskety se otevře volba mechaniky (volné mechaniky
 * první, klávesy 1 až 4 = FDD 0 až FDD 3); Esc zavře jen volbu; chybějící
 * soubor se nevloží.
 */
void test_recent_strip(void)
{
    const char *name = NULL;
    st_DEVICE_REQ_ARGS args;
    const char *d1 = tmp_media("alpha-disk.dsk"), *d2 = tmp_media("twin-peaks.dsk");
    imgui_overlay_open();
    sheet_view_real(OVERLAY_FIXTURE_DEFAULT);
    media_drain();
    /* pořadí pruhu: alpha-disk.dsk, twin-peaks.dsk, twilight.dsk, tape-one.mzf, quick.mzq */
    for (int i = 0; i < 5; i++) {
        char chip[20];
        snprintf(chip, sizeof(chip), "recent_chip%d", i);
        TEST_ASSERT_TRUE_MESSAGE(media_rect(chip, NULL, NULL), chip);
    }
    TEST_ASSERT_FALSE(media_rect("recent_chip5", NULL, NULL));
    TEST_ASSERT_FALSE(media_rect("recent_empty", NULL, NULL));

    /* páska rovnou, Quick Disk rovnou */
    media_click("recent_chip3");
    TEST_ASSERT_EQUAL_INT(1, s_tape_open_calls);
    TEST_ASSERT_EQUAL_STRING(tmp_media("tape-one.mzf"), s_tape_open_path);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    media_click("recent_chip4");
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_STRING("qdisk_mount", name);
    TEST_ASSERT_EQUAL_STRING(tmp_media("quick.mzq"), args.str);

    /* obraz diskety: volba mechaniky, volné mechaniky (FDD 2 a 3) nad obsazenými */
    media_click("recent_chip1");
    TEST_ASSERT_EQUAL_INT(OVL_SHEET_PICK_DRIVE, overlay_sheet_target(NULL));
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    ImVec2 p0, p1, p2, p3, q;
    TEST_ASSERT_TRUE(media_rect("pick_fdd0", &p0, &q));
    TEST_ASSERT_TRUE(media_rect("pick_fdd1", &p1, &q));
    TEST_ASSERT_TRUE(media_rect("pick_fdd2", &p2, &q));
    TEST_ASSERT_TRUE(media_rect("pick_fdd3", &p3, &q));
    TEST_ASSERT_TRUE_MESSAGE(p2.y < p3.y && p3.y < p0.y && p0.y < p1.y, "free drives first");
    /* klávesa 2 = FDD 1, i když není druhá v pořadí */
    ov_type(ImGuiKey_2, '2');
    expect_fdc_mount(1, d2);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MEDIA, imgui_overlay_get_tab());
    /* Enter = první volná mechanika (FDD 2); klik na řádek */
    media_click("recent_chip0");
    ov_key(ImGuiKey_Enter);
    expect_fdc_mount(2, d1);
    media_click("recent_chip0");
    media_click("pick_fdd3");
    expect_fdc_mount(3, d1);
    /* Esc zavře jen volbu */
    media_click("recent_chip0");
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    /* položky jsou zastávky navigace: po zavření volby má fokus zase položka, Enter ji zvolí, šipka vpravo přejde na další */
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_SHEET_PICK_DRIVE, overlay_sheet_target(NULL), "the chip has the focus again and Enter selects it");
    ov_key(ImGuiKey_Escape);
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_tape_open_calls, "three steps right of the first chip is the tape");
    TEST_ASSERT_FALSE(overlay_sheet_is_open());

    /* chybějící soubor: nevloží se, ohlásí se */
    s_real_view.recent[0].missing = true;
    overlay_view_copy_text(s_real_view.recent[0].path, sizeof(s_real_view.recent[0].path), "C:/no-such-dir/alpha-disk.dsk");
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("recent_chip0");
    TEST_ASSERT_EQUAL_INT(1, overlay_sheet_test_take_missing_reports());
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    /* Quick Disk s vloženým médiem: položka Quick Disku nic nedělá (nejdřív vysunout, jako klasické menu) */
    s_real_view.qd.mounted = true;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("recent_chip4");
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    /* nahrávání na pásku: položka pásky nic nedělá */
    s_real_view.cmt.state = OVERLAY_CMT_RECORD;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("recent_chip3");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_tape_open_calls, "no new tape request while recording");
    imgui_overlay_close();
}

/** @brief Zámek stavu náhradního zjišťování existence (probe_block_fn()). */
static GMutex s_probe_test_mutex;
/** @brief Podmínka: náhradní zjišťování smí pokračovat (s_probe_test_release). */
static GCond s_probe_test_cond;
/** @brief Náhradní zjišťování má stát, dokud je false (pod s_probe_test_mutex). */
static bool s_probe_test_release = true;
/** @brief Počet vstupů do náhradního zjišťování (pod s_probe_test_mutex). */
static int s_probe_test_calls = 0;

/**
 * @brief Náhradní "existuje": zablokuje se, dokud test nepovolí pokračovat (nedostupná síťová cesta).
 * @param path Cesta; jméno s "gone" hlásí jako chybějící, ostatní jako soubor.
 * @return Stav cesty.
 */
static en_OVERLAY_VIEW_PATH_STATE probe_block_fn(const char *path)
{
    g_mutex_lock(&s_probe_test_mutex);
    s_probe_test_calls++;
    while (!s_probe_test_release) g_cond_wait(&s_probe_test_cond, &s_probe_test_mutex);
    g_mutex_unlock(&s_probe_test_mutex);
    return strstr(path, "gone") ? OVERLAY_VIEW_PATH_MISSING : OVERLAY_VIEW_PATH_FILE;
}

/**
 * Review O4 X1: zjišťování existence souborů historie běží v pracovním
 * vlákně mimo zámek zařízení. Zablokované zjišťování (nedostupná cesta)
 * nezdrží snímek UI ani nedrží device_state_lock(), další kola se
 * nehromadí, položky se kreslí s posledním známým stavem a položka s
 * neznámým stavem se nevloží. Mimo tab Media se existence nezjišťuje.
 * Po uvolnění se výsledek projeví v kopii.
 */
void test_recent_probe_never_blocks_ui(void)
{
    static st_OVERLAY_VIEW v;
    g_fdc[FDC0].connected = FDC_CONNECTED;
    /* ustálit předchozí kola a potom teprve nahradit zjišťování */
    TEST_ASSERT_TRUE(recent_wait_probe());
    g_mutex_lock(&s_probe_test_mutex);
    s_probe_test_release = false;
    s_probe_test_calls = 0;
    g_mutex_unlock(&s_probe_test_mutex);
    overlay_view_test_set_exists_fn(probe_block_fn);
    recent_media_add(RECENT_MEDIA_DSK, "//unreachable-host/share/keep.dsk");
    recent_media_add(RECENT_MEDIA_DSK, "//unreachable-host/share/gone.dsk");

    /* jiný tab: položky nejsou vidět, existence se nezjišťuje */
    imgui_overlay_open();
    imgui_overlay_set_tab(OVERLAY_TAB_MACHINE);
    s_display = ImVec2(1280.0f, 720.0f);
    for (int i = 0; i < 4; i++) ov_frame();
    g_usleep(20000);
    g_mutex_lock(&s_probe_test_mutex);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_probe_test_calls, "no existence checks while the recent media are not visible");
    g_mutex_unlock(&s_probe_test_mutex);

    /* tab Media: kolo se zadá a zablokuje; snímky běží dál */
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    const gint64 t0 = g_get_monotonic_time();
    for (int i = 0; i < 30; i++) {
        overlay_view_recent_recheck();
        ov_frame();
        TEST_ASSERT_FALSE_MESSAGE(device_state_lock_held(), "the device lock is not held across the check");
    }
    const gint64 dt = g_get_monotonic_time() - t0;
    TEST_ASSERT_TRUE_MESSAGE(dt < 5 * G_USEC_PER_SEC, "30 frames must not wait for the blocked check");
    TEST_ASSERT_TRUE_MESSAGE(overlay_view_recent_busy(), "the check is still blocked");
    for (int i = 0; i < 200; i++) {
        g_mutex_lock(&s_probe_test_mutex);
        const int calls = s_probe_test_calls;
        g_mutex_unlock(&s_probe_test_mutex);
        if (calls > 0) break;
        g_usleep(1000);
    }
    g_mutex_lock(&s_probe_test_mutex);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_probe_test_calls, "one blocked check, further rounds do not pile up");
    g_mutex_unlock(&s_probe_test_mutex);
    /* položky jsou vidět s neznámým stavem a nejdou vložit naslepo */
    TEST_ASSERT_TRUE(media_rect("recent_chip0", NULL, NULL));
    TEST_ASSERT_TRUE(media_rect("recent_chip1", NULL, NULL));
    overlay_view_fill_recent(&v, true);
    TEST_ASSERT_EQUAL_UINT(2, v.recent_count);
    TEST_ASSERT_FALSE(v.recent[0].checked);
    TEST_ASSERT_FALSE(v.recent[0].missing);
    media_drain();
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_FALSE_MESSAGE(device_request_test_take_pending(NULL, NULL), "an item with an unknown state is not inserted");
    TEST_ASSERT_EQUAL_INT(0, overlay_sheet_test_take_missing_reports());
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));

    /* disk odpověděl: výsledek se projeví, chybějící soubor je šedý a při pokusu o vložení zmizí z historie */
    g_mutex_lock(&s_probe_test_mutex);
    s_probe_test_release = true;
    g_cond_broadcast(&s_probe_test_cond);
    g_mutex_unlock(&s_probe_test_mutex);
    TEST_ASSERT_TRUE(recent_wait_probe());
    overlay_view_fill_recent(&v, false);
    TEST_ASSERT_TRUE(v.recent[0].checked && v.recent[1].checked);
    int missing = 0;
    for (unsigned i = 0; i < v.recent_count; i++)
        if (v.recent[i].missing) {
            missing++;
            TEST_ASSERT_NOT_NULL(strstr(v.recent[i].path, "gone"));
        }
    TEST_ASSERT_EQUAL_INT(1, missing);

    overlay_view_test_set_exists_fn(NULL);
    TEST_ASSERT_TRUE(recent_wait_probe());
    imgui_overlay_close();
}

/**
 * @brief Náhradní "existuje": na cestě se slovem "blocked" stojí, dokud test nepovolí pokračovat; ostatní odpoví hned.
 * @param path Cesta; jméno s "gone" hlásí jako chybějící, ostatní jako soubor.
 * @return Stav cesty.
 */
static en_OVERLAY_VIEW_PATH_STATE probe_block_one_fn(const char *path)
{
    if (strstr(path, "blocked")) {
        g_mutex_lock(&s_probe_test_mutex);
        s_probe_test_calls++;
        while (!s_probe_test_release) g_cond_wait(&s_probe_test_cond, &s_probe_test_mutex);
        g_mutex_unlock(&s_probe_test_mutex);
    }
    return strstr(path, "gone") ? OVERLAY_VIEW_PATH_MISSING : OVERLAY_VIEW_PATH_FILE;
}

/**
 * Review O6 Z1 až Z3: výsledek zjišťování existence se zveřejní po každé
 * cestě, takže jedna nedostupná cesta nezablokuje vložení ostatních
 * položek; položka se z historie maže jen podle čerstvého výsledku;
 * neověřená položka Quick Disku se nezakazuje podle druhu cesty.
 */
void test_recent_probe_per_path(void)
{
    static st_OVERLAY_VIEW v;
    const char *name = NULL;
    st_DEVICE_REQ_ARGS args;
    g_fdc[FDC0].connected = FDC_CONNECTED;
    TEST_ASSERT_TRUE(recent_wait_probe());
    recent_media_clear();
    const char *ok = tmp_media("z1-ok.dsk");
    g_mutex_lock(&s_probe_test_mutex);
    s_probe_test_release = false;
    s_probe_test_calls = 0;
    g_mutex_unlock(&s_probe_test_mutex);
    overlay_view_test_set_exists_fn(probe_block_one_fn);
    /* nedostupná cesta je v historii starší: v kole přijde na řadu až po dostupné */
    recent_media_add(RECENT_MEDIA_DSK, "//unreachable-host/share/blocked.dsk");
    recent_media_add(RECENT_MEDIA_DSK, ok);

    imgui_overlay_open();
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    s_display = ImVec2(1280.0f, 720.0f);
    for (int i = 0; i < 4; i++) ov_frame();
    /* počkat, až se pracovní vlákno zablokuje na nedostupné cestě */
    int calls = 0;
    for (int i = 0; i < 5000 && calls == 0; i++) {
        g_mutex_lock(&s_probe_test_mutex);
        calls = s_probe_test_calls;
        g_mutex_unlock(&s_probe_test_mutex);
        if (calls == 0) g_usleep(1000);
    }
    TEST_ASSERT_EQUAL_INT(1, calls);
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(overlay_view_recent_busy(), "the round still hangs on the unreachable path");
    overlay_view_fill_recent(&v, true);
    TEST_ASSERT_EQUAL_UINT(2, v.recent_count);
    TEST_ASSERT_NOT_NULL(strstr(v.recent[0].path, "z1-ok"));
    TEST_ASSERT_TRUE_MESSAGE(v.recent[0].checked, "the reachable path has its result while the round hangs");
    TEST_ASSERT_FALSE(v.recent[0].missing);
    TEST_ASSERT_TRUE(v.recent[0].checked_us > 0);
    TEST_ASSERT_FALSE(v.recent[1].checked);
    /* a jde vložit */
    media_drain();
    media_click("fdd0_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    ov_type(ImGuiKey_1, '1');
    TEST_ASSERT_TRUE_MESSAGE(device_request_test_take_pending(&name, &args), "the reachable item is inserted while another path hangs");
    TEST_ASSERT_NOT_NULL(name);

    /* Z2: zastaralé "chybí" položku nemaže, čerstvé ano */
    st_OVERLAY_VIEW_RECENT stale = v.recent[0];
    stale.checked = true;
    stale.missing = true;
    stale.checked_us = (long long)g_get_monotonic_time() - 2 * OVERLAY_VIEW_RECENT_FRESH_US;
    overlay_sheet_test_take_missing_reports();
    overlay_sheet_request_insert(&stale, 0);
    ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, overlay_sheet_test_take_missing_reports(), "a stale 'missing' does not remove the item");
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    stale.checked_us = (long long)g_get_monotonic_time();
    overlay_sheet_request_insert(&stale, 0);
    ov_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, overlay_sheet_test_take_missing_reports(), "a fresh 'missing' removes the item");
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));

    /* Z3: neověřená položka Quick Disku se podle druhu cesty nezakazuje; ověřená ano */
    st_OVERLAY_VIEW qv;
    overlay_view_make_fixture(&qv, OVERLAY_FIXTURE_DEFAULT);
    qv.qd.directory = false;
    st_OVERLAY_VIEW_RECENT qd;
    memset(&qd, 0, sizeof(qd));
    qd.kind = OVERLAY_MEDIA_QD;
    qd.is_dir = true;
    if (!overlay_action_reason(OVL_ACTION_QD_INSERT, &qv)) {
        TEST_ASSERT_NULL_MESSAGE(overlay_sheet_insert_reason(&qv, &qd), "an unchecked Quick Disk item is not disabled by its kind");
        qd.checked = true;
        TEST_ASSERT_NOT_NULL(overlay_sheet_insert_reason(&qv, &qd));
        qd.is_dir = false;
        TEST_ASSERT_NULL(overlay_sheet_insert_reason(&qv, &qd));
    }

    g_mutex_lock(&s_probe_test_mutex);
    s_probe_test_release = true;
    g_cond_broadcast(&s_probe_test_cond);
    g_mutex_unlock(&s_probe_test_mutex);
    TEST_ASSERT_TRUE(recent_wait_probe());
    overlay_view_test_set_exists_fn(NULL);
    TEST_ASSERT_TRUE(recent_wait_probe());
    imgui_overlay_close();
    recent_media_clear();
}

/**
 * Review O4 X4: tlačítko "Clear" v pruhu nedávných médií je vidět jen s
 * neprázdnou historií, vyprázdní historii všech druhů a vložená média
 * nechá být.
 */
void test_recent_clear(void)
{
    g_fdc[FDC0].connected = FDC_CONNECTED;
    const char *dsk = media_test_dsk();
    fdc_mount_dskfile(&g_fdc[FDC0], 0, (char *)dsk);
    recent_media_add(RECENT_MEDIA_CMT, tmp_media("clear-tape.mzf"));
    recent_media_add(RECENT_MEDIA_QD, tmp_media("clear-qd.mzq"));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));

    imgui_overlay_open();
    s_display = ImVec2(1280.0f, 720.0f);
    for (int i = 0; i < 4; i++) ov_frame();
    media_drain();
    TEST_ASSERT_TRUE(media_rect("recent_chip0", NULL, NULL));
    static st_OVERLAY_VIEW v;
    media_fill(&v);
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_RECENT_CLEAR, &v));
    media_click("recent_clear");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_RECENT_CLEAR, overlay_actions_test_take_last());
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) TEST_ASSERT_EQUAL_UINT(0, recent_media_count((en_RECENT_MEDIA_KIND)k));
    TEST_ASSERT_TRUE_MESSAGE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0), "the inserted disk stays");
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(media_rect("recent_empty", NULL, NULL), "the strip shows the empty state");
    TEST_ASSERT_FALSE_MESSAGE(media_rect("recent_clear", NULL, NULL), "no Clear button without history");
    TEST_ASSERT_FALSE(media_rect("recent_chip0", NULL, NULL));
    media_fill(&v);
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_RECENT_CLEAR, &v));
    imgui_overlay_close();
}

/**
 * Modální dialog nad overlayem (Review Focus 6, K11): souborový dialog a
 * dotazy emulátoru jsou modální popupy ImGui. Při otevřeném modálu klávesy
 * do listu ani do overlaye nejdou (nic se nevloží, nic se nezavře); po jeho
 * zavření se fokus vrátí do overlaye a list funguje dál. Druhý požadavek na
 * dialog během otevřeného modálu nic nerozbije.
 */
void test_sheet_modal_above(void)
{
    const char *d1 = tmp_media("alpha-disk.dsk");
    imgui_overlay_open();
    sheet_view_real(OVERLAY_FIXTURE_DEFAULT);
    media_drain();
    media_click("fdd2_insert");
    TEST_ASSERT_TRUE(overlay_sheet_is_open());

    s_modal = true;
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(ov_overlay_focused(), "the modal dialog has the focus");
    ov_type(ImGuiKey_1, '1');
    ov_type(ImGuiKey_T, 't');
    ov_key(ImGuiKey_Enter);
    ov_key(ImGuiKey_F10);
    TEST_ASSERT_FALSE_MESSAGE(device_request_test_take_pending(NULL, NULL), "keys do not reach the sheet under a modal dialog");
    TEST_ASSERT_EQUAL_STRING("", overlay_sheet_test_filter());
    TEST_ASSERT_TRUE(overlay_sheet_is_open());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    /* modál leží nad overlayem i nad listem */
    ImGuiContext &g = *GImGui;
    ImGuiWindow *modal = ImGui::FindWindowByName("Test modal###ov_test_modal");
    ImGuiWindow *overlay = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    TEST_ASSERT_NOT_NULL(modal);
    TEST_ASSERT_NOT_NULL(overlay);
    int modal_index = -1, sheet_index = -1;
    for (int i = 0; i < g.Windows.Size; i++) {
        if (g.Windows[i] == modal) modal_index = i;
        if (g.Windows[i]->WasActive && strstr(g.Windows[i]->Name, "ovl_sheet")) sheet_index = i;
    }
    TEST_ASSERT_TRUE(sheet_index >= 0);
    TEST_ASSERT_TRUE_MESSAGE(modal_index > sheet_index, "the modal dialog is drawn above the overlay and its sheet");

    /* po zavření modálu se fokus vrátí do overlaye a list přijímá klávesy */
    s_modal = false;
    for (int i = 0; i < 4; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the focus returns to the overlay");
    ov_type(ImGuiKey_1, '1');
    expect_fdc_mount(2, d1);
    TEST_ASSERT_FALSE(overlay_sheet_is_open());

    /* Browse files... dvakrát po sobě (druhý požadavek během "otevřeného" dialogu): nic nespadne, overlay zůstává */
    media_click("fdd2_insert");
    media_click("sheet_browse");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FDD_INSERT_2, overlay_actions_test_take_last());
    s_modal = true;
    for (int i = 0; i < 3; i++) ov_frame();
    ov_mod_key(ImGuiKey_1, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "overlay shortcuts are off under a modal dialog");
    s_modal = false;
    for (int i = 0; i < 4; i++) ov_frame();
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    TEST_ASSERT_TRUE(ov_overlay_focused());
    ov_mod_key(ImGuiKey_1, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_FDD_INSERT_0, overlay_actions_test_take_last(), "Alt+1 still opens the file dialog directly");
    TEST_ASSERT_FALSE(overlay_sheet_is_open());
    imgui_overlay_close();
}

/* ================================================================
 * Taby Machine a Display
 * ================================================================ */

/**
 * @brief Začne kreslit tab z testovacího stavu v dané velikosti viewportu a nechá ustálit rozvržení.
 * @param tab Tab. @param f Testovací stav. @param w Šířka viewportu. @param h Výška viewportu.
 */
static void form_start(en_OVERLAY_TAB tab, en_OVERLAY_FIXTURE f, float w, float h)
{
    overlay_view_make_fixture(&s_fixture_view, f);
    s_fixture = &s_fixture_view;
    s_display = ImVec2(w, h);
    imgui_overlay_set_tab(tab);
    /* obsah tabu od začátku (rolování z předchozího kroku testu se nepřenáší) */
    media_content_scroll_reset();
    for (int i = 0; i < 4; i++) ov_frame();
}

/** @brief Doroluje obsah tabu na konec (tab Display má v širokém okně jeden sloupec karet a smí se rolovat). */
static void form_scroll_end(void)
{
    ImGuiWindow *content = media_content_window();
    if (!content || content->ScrollMax.y <= 0.0f) return;
    ImGui::SetScrollY(content, content->ScrollMax.y);
    for (int i = 0; i < 3; i++) ov_frame();
}

/**
 * @brief Geometrie karet tabu v posledním kreslení: karty vodorovně v okně s okrajem obsahu a navzájem se nepřekrývají.
 * @param msg Popis situace do zprávy. @param cards Jména karet. @param count Počet karet (nejvýš 8).
 * @param all true = všechny karty musí být vidět celé v ploše obsahu (okno, které se neroluje).
 */
static void form_check_cards(const char *msg, const char *const *cards, int count, bool all)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float margin = floorf(overlay_px(OVL_MARGIN));
    ImVec2 mn[8], mx[8];
    char m[320];
    TEST_ASSERT_TRUE(count <= 8);
    for (int i = 0; i < count; i++) {
        snprintf(m, sizeof(m), "%s: %s", msg, cards[i]);
        TEST_ASSERT_TRUE_MESSAGE(media_rect(cards[i], &mn[i], &mx[i]), m);
        TEST_ASSERT_TRUE_MESSAGE(mn[i].x >= vp->Pos.x + margin - 1.0f && mx[i].x <= vp->Pos.x + vp->Size.x - margin + 1.0f, m);
        TEST_ASSERT_TRUE_MESSAGE(mx[i].x > mn[i].x && mx[i].y > mn[i].y, m);
        for (int j = 0; j < i; j++) {
            snprintf(m, sizeof(m), "%s: %s overlaps %s", msg, cards[i], cards[j]);
            TEST_ASSERT_FALSE_MESSAGE(rects_overlap(mn[i], mx[i], mn[j], mx[j]), m);
        }
        if (all) {
            ImVec2 cmin, cmax;
            TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
            snprintf(m, sizeof(m), "%s: %s (%.1f to %.1f) in content (%.1f to %.1f)", msg, cards[i], mn[i].y, mx[i].y, cmin.y, cmax.y);
            TEST_ASSERT_TRUE_MESSAGE(mn[i].y >= cmin.y - 0.5f && mx[i].y <= cmax.y + 0.5f, m);
        }
    }
}

/** @brief Jména karet tabu Machine. */
static const char *const s_machine_cards[] = {"card_machine", "card_speed", "card_snapshots", "card_more"};
/** @brief Jména karet tabu Display v pořadí shora dolů (nahoře karty, jejichž účinek je vidět na obraze). */
static const char *const s_display_cards[] = {"card_colors", "card_scanlines", "card_window", "card_framerate"};

/**
 * @brief Geometrie tabu Machine v posledním kreslení: karty v okně, prvky v kartách, nic se nepřekrývá.
 * @param msg Popis situace do zprávy. @param all true = všechny karty a všechny prvky musí být vidět.
 */
static void machine_check_geometry(const char *msg, bool all)
{
    static const char *const machine[] = {"mach_emulation", "mach_emulation_keycap", "mach_reset", "mach_reset_keycap", "mach_rom"};
    static const char *const speed[] = {"mach_speed", "mach_speed_down", "mach_speed_slider", "mach_speed_up"};
    static const char *const snaps[] = {"snap_quick_save", "snap_quick_load", "snap_save", "snap_load", "snap_setup"};
    static const char *const more[] = {"more_text", "more_classic"};
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    form_check_cards(msg, s_machine_cards, 4, all);
    const int n1 = media_check_group(msg, "card_machine", pad, machine, 5);
    const int n2 = media_check_group(msg, "card_speed", pad, speed, 4);
    const int n3 = media_check_group(msg, "card_snapshots", pad, snaps, 5);
    const int n4 = media_check_group(msg, "card_more", pad, more, 2);
    if (all) {
        TEST_ASSERT_TRUE_MESSAGE(n1 >= 3, msg); /* štítky zkratek smí ustoupit */
        TEST_ASSERT_EQUAL_INT_MESSAGE(4, n2, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(5, n3, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, n4, msg);
    }
}

/**
 * @brief Geometrie tabu Display v posledním kreslení: karty v okně, prvky v kartách, nic se nepřekrývá.
 * @param msg Popis situace do zprávy. @param all true = všechny karty a všechny prvky musí být vidět.
 * @param custom_fps true = karta Frame rate má řádek vlastní frekvence.
 */
static void display_check_geometry(const char *msg, bool all, bool custom_fps)
{
    static const char *const window[] = {"disp_fullscreen", "disp_fullscreen_keycap", "disp_startup", "disp_lock", "disp_fix_w", "disp_fix_h"};
    static const char *const colors[] = {"disp_colors"};
    static const char *const scan[] = {"disp_scanlines", "disp_intensity"};
    static const char *const rate[] = {"disp_framerate", "disp_fps_down", "disp_fps", "disp_fps_up", "disp_redraw"};
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    form_check_cards(msg, s_display_cards, 4, all);
    const int n1 = media_check_group(msg, "card_window", pad, window, 6);
    const int n2 = media_check_group(msg, "card_colors", pad, colors, 1);
    const int n3 = media_check_group(msg, "card_scanlines", pad, scan, 2);
    const int n4 = media_check_group(msg, "card_framerate", pad, rate, 5);
    if (all) {
        TEST_ASSERT_TRUE_MESSAGE(n1 >= 5, msg); /* štítek zkratky smí ustoupit */
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, n2, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, n3, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(custom_fps ? 5 : 2, n4, msg);
        /* věta pod kartami: v ploše obsahu, pod kartami */
        ImVec2 nmin, nmax, cmin, cmax, kmin, kmax;
        TEST_ASSERT_TRUE_MESSAGE(media_rect("disp_note", &nmin, &nmax), msg);
        TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
        TEST_ASSERT_TRUE_MESSAGE(nmin.x >= cmin.x && nmax.x <= cmax.x && nmax.y <= cmax.y + 0.5f, msg);
        for (int i = 0; i < 4; i++) {
            TEST_ASSERT_TRUE(media_rect(s_display_cards[i], &kmin, &kmax));
            TEST_ASSERT_TRUE_MESSAGE(nmin.y >= kmax.y, msg);
        }
    }
}

/**
 * Geometrie tabů Machine a Display (Review Focus 3 a 4): testovací stavy
 * default, empty, long a replay při 640 x 480, 1280 x 720, 1920 x 1080 a
 * 2560 x 1440, s texty běžnými i o 40 % delšími. Karty v ploše obsahu,
 * prvky v kartách, nic se nepřekrývá; od 1280 x 720 se obsah neroluje a
 * karty jsou ve dvou sloupcích, v úzkém okně v jednom.
 */
void test_machine_display_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}};
    const en_OVERLAY_FIXTURE fixtures[] = {OVERLAY_FIXTURE_DEFAULT, OVERLAY_FIXTURE_EMPTY, OVERLAY_FIXTURE_LONG_TEXTS, OVERLAY_FIXTURE_REPLAY};
    for (int stretch = 0; stretch < 2; stretch++) {
        overlay_test_set_stretch(stretch != 0);
        for (const float *sz : sizes) {
            for (en_OVERLAY_FIXTURE f : fixtures) {
                for (int t = 0; t < 2; t++) {
                    const en_OVERLAY_TAB tab = t ? OVERLAY_TAB_DISPLAY : OVERLAY_TAB_MACHINE;
                    form_start(tab, f, sz[0], sz[1]);
                    char msg[112];
                    snprintf(msg, sizeof(msg), "%s %.0fx%.0f %s%s", t ? "display" : "machine", sz[0], sz[1], s_fixture_names[f], stretch ? " +40%" : "");
                    ImGuiWindow *content = media_content_window();
                    TEST_ASSERT_NOT_NULL_MESSAGE(content, msg);
                    const bool wide = sz[0] >= 1280.0f;
                    const bool custom_fps = s_fixture_view.display.framerate_mode == (int)DISPLAY_FRAMERATE_MODE_CUSTOM;
                    const char *const *cards = t ? s_display_cards : s_machine_cards;
                    ImVec2 amin, amax, bmin, bmax;
                    if (wide && !t) {
                        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, content->ScrollMax.y, msg);
                        machine_check_geometry(msg, true);
                        /* dva sloupce: třetí karta je vpravo od první a začíná ve stejné výšce */
                        TEST_ASSERT_TRUE(media_rect(cards[0], &amin, &amax));
                        TEST_ASSERT_TRUE(media_rect(cards[2], &bmin, &bmax));
                        TEST_ASSERT_TRUE_MESSAGE(bmin.x >= amax.x, msg);
                        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amin.y, bmin.y, msg);
                        /* sloupce končí ve stejné výšce */
                        TEST_ASSERT_TRUE(media_rect(cards[1], &amin, &amax));
                        TEST_ASSERT_TRUE(media_rect(cards[3], &bmin, &bmax));
                        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amax.y, bmax.y, msg);
                    } else {
                        /* úzké okno (K03) a tab Display v každém okně (posudek R03): jeden sloupec, obsah se smí
                         * rolovat; kontrola na začátku a na konci */
                        if (!wide) TEST_ASSERT_TRUE_MESSAGE(content->ScrollMax.y > 0.0f, msg);
                        if (wide) {
                            /* sloupec karet u levého okraje nechává nejméně polovinu plochy obsahu nezakrytou */
                            ImVec2 cmin, cmax;
                            TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
                            float covered = 0.0f;
                            for (int c = 0; c < 4; c++) {
                                TEST_ASSERT_TRUE(media_rect(cards[c], &amin, &amax));
                                const float y0 = (amin.y > cmin.y) ? amin.y : cmin.y, y1 = (amax.y < cmax.y) ? amax.y : cmax.y;
                                if (y1 > y0) covered += (amax.x - amin.x) * (y1 - y0);
                                TEST_ASSERT_TRUE_MESSAGE(amax.x <= cmin.x + (cmax.x - cmin.x) * 0.5f, msg);
                            }
                            const float free_part = 1.0f - covered / ((cmax.x - cmin.x) * (cmax.y - cmin.y));
                            if (f == OVERLAY_FIXTURE_DEFAULT && !stretch) printf("display tab %.0fx%.0f: %.0f %% of the content area is not covered by cards\n", sz[0], sz[1], free_part * 100.0f);
                            TEST_ASSERT_TRUE_MESSAGE(free_part >= 0.5f, msg);
                        }
                        if (t) display_check_geometry(msg, false, custom_fps);
                        else machine_check_geometry(msg, false);
                        for (int c = 1; c < 4; c++) {
                            TEST_ASSERT_TRUE(media_rect(cards[c - 1], &amin, &amax));
                            TEST_ASSERT_TRUE(media_rect(cards[c], &bmin, &bmax));
                            TEST_ASSERT_TRUE_MESSAGE(bmin.y >= amax.y, msg);
                            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amin.x, bmin.x, msg);
                        }
                        ImGui::SetScrollY(content, content->ScrollMax.y);
                        for (int i = 0; i < 3; i++) ov_frame();
                        if (t) display_check_geometry(msg, false, custom_fps);
                        else machine_check_geometry(msg, false);
                        TEST_ASSERT_TRUE_MESSAGE(media_rect(t ? "disp_redraw" : "more_classic", NULL, NULL), msg);
                        ImGui::SetScrollY(content, 0.0f);
                        for (int i = 0; i < 2; i++) ov_frame();
                    }
                    /* štítek zkratky jen u akce, která právě funguje: Alt+P vždy, je-li na něj místo */
                    if (!t && wide && !stretch) TEST_ASSERT_TRUE_MESSAGE(media_rect("mach_emulation_keycap", NULL, NULL), msg);
                }
            }
        }
    }
    overlay_test_set_stretch(false);

    /* K04: plocha overlaye má na tabu Display krytí 55 %, jinde 85 % (první vrchol seznamu okna je jeho pozadí) */
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    ImGuiWindow *ow = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    TEST_ASSERT_NOT_NULL(ow);
    TEST_ASSERT_TRUE(ow->DrawList->VtxBuffer.Size > 0);
    TEST_ASSERT_INT_WITHIN(2, (int)(OVL_PANEL_OPACITY_PREVIEW * 255.0f + 0.5f), (int)(ow->DrawList->VtxBuffer[0].col >> IM_COL32_A_SHIFT));
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    TEST_ASSERT_INT_WITHIN(2, (int)(OVL_PANEL_OPACITY * 255.0f + 0.5f), (int)(ow->DrawList->VtxBuffer[0].col >> IM_COL32_A_SHIFT));
}

/**
 * Kopie stavu tabů Machine a Display nad skutečným jádrem: nabídka ROM
 * podle konfigurace (vývojové ROM jen v Development Mode, právě jedna
 * vybraná), nastavená rychlost a dostupnost vlastní rychlosti, nastavení
 * zobrazení z g_display a stav fullscreenu z video rozhraní.
 */
void test_machine_display_view_fill(void)
{
    static st_OVERLAY_VIEW v;

    /* nabídka ROM: řádky konfigurace bez vývojových, právě jedna vybraná */
    TEST_ASSERT_NOT_NULL(g_rom_config);
    int expect = 0, expect_dev = 0;
    for (const st_ROM_CONFIG_ROW *row = g_rom_config->roms; row->type != ROM_CONFIG_TYPE_NONE; row++) {
        if (!row->enabled || row->type != ROM_CONFIG_TYPE_ROM) continue;
        if (row->flags & ROM_CONFIG_FLAG_DEVELOPMENT) expect_dev++;
        else expect++;
    }
    const bool saved_dev = EMULATOR_TEST_DEVELOPMENT_MODE;
    emulator_set_development_mode(false);
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    int roms = 0, current = 0;
    for (unsigned i = 0; i < v.machine.rom_count; i++) {
        const st_OVERLAY_VIEW_ROM *r = &v.machine.rom[i];
        if (r->section) {
            TEST_ASSERT_FALSE(r->current);
            TEST_ASSERT_TRUE_MESSAGE(i > 0 || r->label[0], "the list does not start with a bare separator");
            continue;
        }
        roms++;
        TEST_ASSERT_TRUE(r->label[0] != '\0');
        TEST_ASSERT_EQUAL_INT(ROM_CONFIG_TYPE_ROM, g_rom_config->roms[r->row].type);
        TEST_ASSERT_FALSE(g_rom_config->roms[r->row].flags & ROM_CONFIG_FLAG_DEVELOPMENT);
        TEST_ASSERT_EQUAL_INT(ROM_TEST_CURRENT(&g_rom_config->roms[r->row]) ? 1 : 0, r->current ? 1 : 0);
        if (r->current) current++;
    }
    TEST_ASSERT_EQUAL_INT(expect, roms);
    TEST_ASSERT_TRUE(current <= 1);
    emulator_set_development_mode(true);
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    roms = 0;
    for (unsigned i = 0; i < v.machine.rom_count; i++)
        if (!v.machine.rom[i].section) roms++;
    TEST_ASSERT_EQUAL_INT_MESSAGE(expect + expect_dev, roms, "development ROMs only in Development Mode");
    emulator_set_development_mode(saved_dev);

    /* rychlost: kopie nese nastavenou (požadovanou) hodnotu, i když ji emu vlákno ještě neuplatnilo */
    g_customspeed.speed_in_percentage = 100;
    g_customspeed.speed_in_percentage_requested = 100;
    g_customspeed.previous_speed_in_percentage = 100;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(OVERLAY_SPEED_NORMAL, v.speed_mode);
    TEST_ASSERT_FALSE_MESSAGE(v.machine.custom_speed_available, "no custom speed to restore");
    g_customspeed.previous_speed_in_percentage = 250;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_TRUE(v.machine.custom_speed_available);
    g_customspeed.speed_in_percentage_requested = 180;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVERLAY_SPEED_CUSTOM, v.speed_mode, "the requested speed counts while the emulation stands");
    TEST_ASSERT_EQUAL_UINT(180, v.speed_percent);
    TEST_ASSERT_TRUE(v.machine.custom_speed_available);

    /* zobrazení */
    g_display.startup_window_size = DISPLAY_STARTUP_SIZE_BIG;
    g_display.locked_window_aspect_ratio = 1;
    g_display.color_schema = DISPLAY_GREEN;
    g_display.scanlines = 1;
    g_display.scanlines_intensity = 42;
    g_display.framerate_mode = DISPLAY_FRAMERATE_MODE_CUSTOM;
    g_display.custom_fps = 144;
    g_display.forced_full_screen_redrawing = 1;
    s_test_fullscreen = true;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(DISPLAY_STARTUP_SIZE_BIG, v.display.startup_size);
    TEST_ASSERT_TRUE(v.display.locked_aspect);
    TEST_ASSERT_EQUAL_INT(DISPLAY_GREEN, v.display.color_schema);
    TEST_ASSERT_TRUE(v.display.scanlines);
    TEST_ASSERT_EQUAL_INT(42, v.display.scanlines_intensity);
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_CUSTOM, v.display.framerate_mode);
    TEST_ASSERT_EQUAL_INT(144, v.display.custom_fps);
    TEST_ASSERT_TRUE(v.display.forced_redraw);
    TEST_ASSERT_TRUE_MESSAGE(v.display.fullscreen, "the fullscreen state comes from the video interface");
    TEST_ASSERT_TRUE(v.display.slave_hz == 50 || v.display.slave_hz == 60);
    s_test_fullscreen = false;
    g_display.scanlines = 0;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_FALSE(v.display.fullscreen);
    TEST_ASSERT_FALSE(v.display.scanlines);
    /* rozhraní bez dotazu: stav "v okně" */
    g_iface_video_callbacks->is_fullscreen = NULL;
    s_test_fullscreen = true;
    device_state_lock();
    overlay_view_fill(&v);
    device_state_unlock();
    TEST_ASSERT_FALSE(v.display.fullscreen);

    /* testovací stavy */
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    TEST_ASSERT_TRUE(v.machine.rom_count > 0 && v.machine.rom[0].current);
    TEST_ASSERT_TRUE(v.display.fullscreen && v.display.scanlines);
    TEST_ASSERT_EQUAL_INT(35, v.display.scanlines_intensity);
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_VSYNC, v.display.framerate_mode);
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_LONG_TEXTS);
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_CUSTOM, v.display.framerate_mode);
    TEST_ASSERT_EQUAL_INT((int)sizeof(v.machine.rom[2].label) - 1, (int)strlen(v.machine.rom[2].label));
}

/**
 * Prvky tabu Machine: každý zařadí správný požadavek nebo zavolá funkci
 * klasického menu (zachyceno stuby, frontou požadavků a stavem jádra),
 * ukazuje stav z kopie a nedostupný prvek nic nedělá.
 */
void test_machine_controls(void)
{
    const char *name;
    st_DEVICE_REQ_ARGS args;
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();

    /* Emulation: kopie říká Paused; klik na Running přepne pauzu jako Alt+P, klik na zvolený segment nic */
    media_click_at("mach_emulation", 0.75f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the selected segment does nothing");
    media_click_at("mach_emulation", 0.25f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_PAUSE_TOGGLE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_pause_toggle_calls);
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "the emulation runs under the menu");
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    /* stav určil uživatel: zavření už nic nerozbíhá ani nezastavuje */
    s_run_calls = s_pause_calls = 0;
    imgui_overlay_close();
    TEST_ASSERT_EQUAL_INT(0, s_run_calls);
    TEST_ASSERT_FALSE(g_emulator.paused);
    /* a naopak: běžící emulace zastavená z overlaye po zavření stojí */
    imgui_overlay_open();
    s_fixture_view.paused = true;
    for (int i = 0; i < 3; i++) ov_frame();
    media_click_at("mach_emulation", 0.25f); /* rozběhnout */
    s_fixture_view.paused = false;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click_at("mach_emulation", 0.75f); /* a zastavit */
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_PAUSE_TOGGLE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_emulator.paused);
    s_run_calls = 0;
    imgui_overlay_close();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_run_calls, "a pause chosen by the user stays after closing");
    TEST_ASSERT_TRUE(g_emulator.paused);
    g_emulator.paused = false;

    /* Reset: jako F12, overlay se zavře a stav pauzy se vrátí */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    media_click("mach_reset");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_RESET, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    TEST_ASSERT_FALSE(g_emulator.paused);

    /* ROM: rozbalit myší, zvolit třetí řádek nabídky (první ROM za nadpisem oddílu) */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    media_click("mach_rom");
    TEST_ASSERT_TRUE_MESSAGE(ovl_dropdown_is_open(), "a click opens the list");
    ImVec2 imin, imax, rmin, rmax;
    TEST_ASSERT_FALSE_MESSAGE(ovl_test_dropdown_item_rect(1, &imin, &imax), "a section heading is not an item");
    TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(2, &imin, &imax));
    TEST_ASSERT_TRUE(media_rect("mach_rom", &rmin, &rmax));
    TEST_ASSERT_TRUE_MESSAGE(imin.y >= rmax.y, "the list opens below the control");
    ov_click(ImVec2((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f));
    ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(ovl_dropdown_is_open(), "choosing an item closes the list");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_ROM_SELECT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(s_fixture_view.machine.rom[2].row, overlay_actions_test_last_value());
    if (g_rom_config->roms[s_fixture_view.machine.rom[2].row].type == ROM_CONFIG_TYPE_ROM) {
        TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
        TEST_ASSERT_EQUAL_STRING("rom_select", name);
        TEST_ASSERT_EQUAL_INT(s_fixture_view.machine.rom[2].row, args.i[0]);
    }
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    /* zvolená položka nic nemění; klik mimo seznam ho zavře beze změny */
    media_click("mach_rom");
    TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(0, &imin, &imax));
    ov_click(ImVec2((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the current ROM is not selected again");
    media_click("mach_rom");
    TEST_ASSERT_TRUE(ovl_dropdown_is_open());
    ov_click(ImVec2(rmin.x - 40.0f, rmin.y - 40.0f));
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_FALSE(ovl_dropdown_is_open());
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    /* index, který v nabídce není, se zahodí */
    overlay_action_request_value(OVL_ACTION_ROM_SELECT, 9999);
    ov_frame();
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    media_drain();

    /* CPU speed: Max zapne maximální rychlost, Normal ji vypne, Custom obnoví vlastní rychlost */
    g_customspeed.speed_in_percentage = 100;
    g_customspeed.speed_in_percentage_requested = 100;
    g_customspeed.previous_speed_in_percentage = 250;
    media_click_at("mach_speed", 0.92f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_emulator.max_speed);
    s_fixture_view.speed_mode = OVERLAY_SPEED_MAX;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click_at("mach_speed", 0.08f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_NORMAL, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(g_emulator.max_speed);
    TEST_ASSERT_EQUAL_UINT(100, g_customspeed.speed_in_percentage_requested);
    s_fixture_view.speed_mode = OVERLAY_SPEED_NORMAL;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click_at("mach_speed", 0.5f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_CUSTOM, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(250, g_customspeed.speed_in_percentage_requested, "Custom restores the previous custom speed");

    /* Custom speed: tlačítka po 10 %, posuvník nastaví hodnotu */
    g_customspeed.speed_in_percentage_requested = 100;
    media_click("mach_speed_up");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_UP, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_UINT(110, g_customspeed.speed_in_percentage_requested);
    media_click("mach_speed_down");
    media_click("mach_speed_down");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_DOWN, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_UINT(90, g_customspeed.speed_in_percentage_requested);
    media_click_at("mach_speed_slider", 0.4f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_SET, overlay_actions_test_take_last());
    const int set = overlay_actions_test_last_value();
    TEST_ASSERT_TRUE_MESSAGE(set > 100 && set < 400, "the slider sets a value inside its range");
    TEST_ASSERT_EQUAL_UINT((unsigned)set, g_customspeed.speed_in_percentage_requested);

    /* review Y1: rychlost nad 400 % (zkratky klasického menu). Rozsah se po dobu držení jezdce nezkracuje:
     * po stisku hodnota odpovídá poloze ukazatele v rozsahu do 1000 % a pod stojícím ukazatelem dál neklesá */
    {
        ImGuiIO &io = ImGui::GetIO();
        s_fixture_view.speed_mode = OVERLAY_SPEED_CUSTOM;
        s_fixture_view.speed_percent = 1000;
        g_customspeed.speed_in_percentage_requested = 1000;
        for (int i = 0; i < 3; i++) ov_frame();
        media_drain();
        ImVec2 smin, smax;
        TEST_ASSERT_TRUE(media_rect("mach_speed_slider", &smin, &smax));
        io.AddMousePosEvent(smin.x + (smax.x - smin.x) * 0.5f, (smin.y + smax.y) * 0.5f);
        ov_frame();
        io.AddMouseButtonEvent(0, true);
        ov_frame();
        ov_frame();
        TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_SET, overlay_actions_test_take_last());
        const unsigned first = g_customspeed.speed_in_percentage_requested;
        TEST_ASSERT_TRUE_MESSAGE(first > 400 && first < 1000, "the pressed slider maps the pointer onto the range up to 1000 %");
        for (int i = 0; i < 6; i++) {
            /* kopie stavu sleduje nastavenou rychlost jako ve skutečné aplikaci */
            s_fixture_view.speed_percent = g_customspeed.speed_in_percentage_requested;
            ov_frame();
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(first, g_customspeed.speed_in_percentage_requested, "the value does not fall while the slider is held still");
        io.AddMouseButtonEvent(0, false);
        for (int i = 0; i < 2; i++) ov_frame();
        media_drain();
        s_fixture_view.speed_mode = OVERLAY_SPEED_NORMAL;
        s_fixture_view.speed_percent = 100;
    }

    /* review Y2: emulace stojí (uplatněných 100 %), posuvník nastaví 250 %, Normal a Custom: vrátí se 250 %,
     * ačkoli jádro si požadovanou rychlost nezapamatovalo */
    {
        g_customspeed.speed_in_percentage = 100;
        g_customspeed.speed_in_percentage_requested = 100;
        g_customspeed.previous_speed_in_percentage = 100;
        overlay_action_request_value(OVL_ACTION_SPEED_SET, 250);
        ov_frame();
        TEST_ASSERT_EQUAL_UINT(250, g_customspeed.speed_in_percentage_requested);
        overlay_action_request(OVL_ACTION_SPEED_NORMAL);
        ov_frame();
        TEST_ASSERT_EQUAL_UINT(100, g_customspeed.speed_in_percentage_requested);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(100, g_customspeed.previous_speed_in_percentage, "the core does not remember a speed it never applied");
        TEST_ASSERT_EQUAL_INT(250, overlay_action_custom_speed_restore_value());
        st_OVERLAY_VIEW filled;
        device_state_lock();
        overlay_view_fill(&filled);
        device_state_unlock();
        TEST_ASSERT_TRUE_MESSAGE(filled.machine.custom_speed_available, "Custom is available: the overlay remembers the speed");
        TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_SPEED_CUSTOM, &filled));
        overlay_action_request(OVL_ACTION_SPEED_CUSTOM);
        ov_frame();
        TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_CUSTOM, overlay_actions_test_take_last());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(250, g_customspeed.speed_in_percentage_requested, "Custom returns the speed set while the emulation stood");
        /* rychlost, kterou si mezitím zapamatovalo jádro (klasické menu), má přednost před pamětí overlaye */
        g_customspeed.speed_in_percentage_requested = 100;
        g_customspeed.previous_speed_in_percentage = 300;
        TEST_ASSERT_EQUAL_INT(300, overlay_action_custom_speed_restore_value());
        /* a při uplatněné vlastní rychlosti (jádro by nic neobnovilo) ji Custom vrátí také */
        g_customspeed.speed_in_percentage = 300;
        overlay_action_request(OVL_ACTION_SPEED_CUSTOM);
        ov_frame();
        TEST_ASSERT_EQUAL_UINT(300, g_customspeed.speed_in_percentage_requested);
        g_customspeed.speed_in_percentage = 100;
        g_customspeed.speed_in_percentage_requested = 100;
        media_drain();
    }

    /* snapshoty */
    media_click("snap_quick_save");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_SAVE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_quicksave_calls);
    media_click("snap_save");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SNAPSHOT_SAVE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_save_calls);
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "the save dialog opens above the overlay");
    media_click("snap_load");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SNAPSHOT_LOAD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_load_calls);
    TEST_ASSERT_FALSE_MESSAGE(s_snapshot_load_overlay_open, "the overlay is closed before Load Snapshot starts");
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    media_click("snap_quick_load");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_QUICK_LOAD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_quickload_calls);
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    g_gui->showSnapshotSetupWindow = false;
    media_click("snap_setup");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SNAPSHOT_SETUP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_gui->showSnapshotSetupWindow);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a settings window closes the overlay");
    g_gui->showSnapshotSetupWindow = false;

    /* Open classic menu: overlay se zavře a hlavní okno dostane právě jednu žádost o klasické menu */
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE(imgui_overlay_take_classic_menu_request());
    s_classic_menu_opened = 0;
    media_click("more_classic");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CLASSIC_MENU, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_classic_menu_opened, "the main window opens the classic menu once");
    TEST_ASSERT_FALSE(imgui_overlay_take_classic_menu_request());
    /* pravé tlačítko myši kdekoli v overlayi dělá totéž (nápověda v patičce) */
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();
    s_classic_menu_opened = 0;
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(640.0f, 660.0f);
        ov_frame();
        io.AddMouseButtonEvent(1, true);
        ov_frame();
        io.AddMouseButtonEvent(1, false);
        ov_frame();
        ov_frame();
    }
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CLASSIC_MENU, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a right click closes the overlay");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_classic_menu_opened, "and the main window opens the classic menu");
    /* nad rozbaleným výběrem pravé tlačítko klasické menu neotevírá */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    media_click("mach_rom");
    TEST_ASSERT_TRUE(ovl_dropdown_is_open());
    s_classic_menu_opened = 0;
    {
        ImGuiIO &io = ImGui::GetIO();
        ImVec2 imin, imax;
        TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(0, &imin, &imax));
        io.AddMousePosEvent((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f);
        ov_frame();
        io.AddMouseButtonEvent(1, true);
        ov_frame();
        io.AddMouseButtonEvent(1, false);
        ov_frame();
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_classic_menu_opened, "no classic menu from a right click over an open list");
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    imgui_overlay_close();
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();

    /* review Y3: je-li otevřený jiný popup (modál z jiného zdroje), žádost se zahodí a neotevře se ani později */
    s_modal = true;
    for (int i = 0; i < 3; i++) ov_frame();
    imgui_overlay_request_classic_menu();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_take_classic_menu_request(), "no classic menu over an open modal");
    s_modal = false;
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_take_classic_menu_request(), "the dropped request does not come back");

    /* replay: ROM, Quick Load a Load Snapshot jsou zakázané */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_REPLAY, 1280.0f, 720.0f);
    media_drain();
    s_quickload_calls = s_snapshot_load_calls = 0;
    media_click("mach_rom");
    TEST_ASSERT_FALSE_MESSAGE(ovl_dropdown_is_open(), "the ROM list does not open during a replay");
    media_click("snap_load");
    media_click("snap_quick_load");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, s_quickload_calls + s_snapshot_load_calls);
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_ROM_SELECT, &s_fixture_view));
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* bez vlastní rychlosti k obnovení segment Custom nereaguje */
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f);
    media_drain();
    media_click_at("mach_speed", 0.5f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "Custom needs a custom speed");
    imgui_overlay_close();
}

/**
 * Prvky tabu Display: každý zavolá funkci klasického menu (zachyceno
 * stavem g_display a stuby video rozhraní), ukazuje stav z kopie a
 * nedostupný prvek nic nedělá.
 */
void test_display_controls(void)
{
    imgui_overlay_open();
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();

    /* Fullscreen: přepínač volá přepnutí jako Alt+Ctrl+Enter */
    media_click("disp_fullscreen");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FULLSCREEN, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_fullscreen_calls);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* Window size on startup: šest segmentů v pořadí Half až Fullscreen */
    g_display.startup_window_size = DISPLAY_STARTUP_SIZE_FULLSCREEN;
    media_click_at("disp_startup", 0.5f / 6.0f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_STARTUP_SIZE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(DISPLAY_STARTUP_SIZE_HALF, overlay_actions_test_last_value());
    TEST_ASSERT_EQUAL_INT(DISPLAY_STARTUP_SIZE_HALF, g_display.startup_window_size);
    media_click_at("disp_startup", 5.5f / 6.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the selected size does nothing");

    /* Lock window aspect ratio: kopie říká zapnuto, přepnutí vypne */
    g_display.locked_window_aspect_ratio = 1;
    media_click("disp_lock");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_LOCK_ASPECT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, g_display.locked_window_aspect_ratio);

    /* Fix aspect ratio: řádek je při 1280 x 720 u spodní hrany rolovaného sloupce, proto na konci sloupce */
    form_scroll_end();
    media_click("disp_fix_w");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FIX_ASPECT_W, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT('W', s_fix_aspect_last);
    media_click("disp_fix_h");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FIX_ASPECT_H, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT('H', s_fix_aspect_last);
    TEST_ASSERT_EQUAL_INT(2, s_fix_aspect_calls);
    media_content_scroll_reset();
    for (int i = 0; i < 3; i++) ov_frame();

    /* Color schema */
    g_display.color_schema = DISPLAY_NORMAL;
    media_click_at("disp_colors", 0.5f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_COLORS, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(DISPLAY_GRAYSCALE, g_display.color_schema);
    media_click_at("disp_colors", 0.9f);
    TEST_ASSERT_EQUAL_INT(DISPLAY_GREEN, g_display.color_schema);
    overlay_actions_test_take_last();

    /* scanlines: kopie říká zapnuto na 35 %; přepnutí vypne, posuvník nastaví intenzitu */
    g_display.scanlines = 1;
    media_click("disp_scanlines");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCANLINES, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, g_display.scanlines);
    media_click_at("disp_intensity", 0.6f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCANLINES_INTENSITY, overlay_actions_test_take_last());
    const int intensity = overlay_actions_test_last_value();
    TEST_ASSERT_TRUE(intensity > 35 && intensity <= DISPLAY_SCANLINES_INTENSITY_MAX);
    TEST_ASSERT_EQUAL_INT(intensity, g_display.scanlines_intensity);

    /* Frame rate control: všech pět režimů klasického menu, zvolený podle kopie (karta je ve sloupci poslední) */
    g_display.framerate_mode = DISPLAY_FRAMERATE_MODE_VSYNC;
    form_scroll_end();
    media_click("disp_framerate");
    TEST_ASSERT_TRUE(ovl_dropdown_is_open());
    ImVec2 imin, imax;
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(i, NULL, NULL));
    TEST_ASSERT_FALSE(ovl_test_dropdown_item_rect(5, NULL, NULL));
    TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(4, &imin, &imax));
    ov_click(ImVec2((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f));
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FRAMERATE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_CUSTOM, overlay_actions_test_last_value());
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_CUSTOM, g_display.framerate_mode);
    TEST_ASSERT_FALSE_MESSAGE(media_rect("disp_fps", NULL, NULL), "the copy still says V-Sync: no Custom FPS row");
    media_click("disp_framerate");
    TEST_ASSERT_TRUE(ovl_test_dropdown_item_rect(0, &imin, &imax));
    ov_click(ImVec2((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f));
    TEST_ASSERT_EQUAL_INT(DISPLAY_FRAMERATE_MODE_SLAVE, overlay_actions_test_last_value());
    overlay_actions_test_take_last();

    /* Forced redraw every screen */
    g_display.forced_full_screen_redrawing = 0;
    media_click("disp_redraw");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FORCED_REDRAW, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, g_display.forced_full_screen_redrawing);

    /* Custom FPS: řádek s hodnotou jen v režimu Custom FPS; tlačítka po 1 Hz, posuvník */
    s_fixture_view.display.framerate_mode = (int)DISPLAY_FRAMERATE_MODE_CUSTOM;
    s_fixture_view.display.custom_fps = 100;
    for (int i = 0; i < 3; i++) ov_frame();
    media_drain();
    TEST_ASSERT_TRUE(media_rect("disp_fps", NULL, NULL));
    g_display.framerate_mode = DISPLAY_FRAMERATE_MODE_VSYNC;
    media_click("disp_fps_up");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CUSTOM_FPS, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(101, g_display.custom_fps);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_FRAMERATE_MODE_CUSTOM, g_display.framerate_mode, "the value is applied through the Custom FPS mode");
    media_click("disp_fps_down");
    TEST_ASSERT_EQUAL_INT(99, g_display.custom_fps);
    media_click_at("disp_fps", 0.5f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CUSTOM_FPS, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(g_display.custom_fps > 100 && g_display.custom_fps <= DISPLAY_CUSTOM_FPS_MAX);
    /* hodnota mimo rozsah se omezí */
    overlay_action_request_value(OVL_ACTION_CUSTOM_FPS, 5);
    ov_frame();
    TEST_ASSERT_EQUAL_INT(DISPLAY_CUSTOM_FPS_MIN, g_display.custom_fps);

    /* vypnuté scanlines: posuvník intenzity nereaguje; mimo režim Custom FPS akce hodnotu nepřijme */
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f);
    media_drain();
    g_display.scanlines_intensity = 50;
    media_click_at("disp_intensity", 0.9f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(50, g_display.scanlines_intensity);
    g_display.custom_fps = 77;
    overlay_action_request_value(OVL_ACTION_CUSTOM_FPS, 200);
    ov_frame();
    TEST_ASSERT_EQUAL_INT(77, g_display.custom_fps);
    /* akce s hodnotou nejde vyžádat bez hodnoty a naopak */
    overlay_action_request(OVL_ACTION_COLORS);
    overlay_action_request_value(OVL_ACTION_SCANLINES, 1);
    ov_frame();
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    /* přepínač Fullscreen ukazuje stav z kopie: v tomto stavu "v okně", klik zase jen žádá přepnutí */
    s_fullscreen_calls = 0;
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f); /* zpět na začátek obsahu */
    media_click("disp_fullscreen");
    TEST_ASSERT_EQUAL_INT(1, s_fullscreen_calls);
    imgui_overlay_close();
}

/**
 * Posudek výsledku v2, nálezy N01 a N02.
 *
 * N01: tab Display v širokém okně roluje jen sloupec karet; posuvník je hned
 * u pravého okraje karet a jeho jezdec má nejméně 8 jednotek overlaye. V
 * úzkém okně se roluje celý obsah tabu se stejně širokým jezdcem.
 *
 * N02: v řadě akcí tabu Media se štítky zkratek ubírají v pevném pořadí,
 * množina akcí se štítkem i šířky tlačítek jsou stejné ve stavech default,
 * replay a nofdc; zakázaná akce štítek nemá. Na tabu Display má přepínač
 * Fullscreen štítek při 1280 x 720 i 640 x 480, při zákazu akce se
 * neposune, a dvojice By width / By height má štítky obě, nebo žádné.
 */
void test_scrollbar_and_keycap_groups(void)
{
    /* N01: široké okno */
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    ImGuiWindow *col = media_content_window();
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(col->Name, "ovl_disp_col"), "the display tab scrolls its card column");
    TEST_ASSERT_TRUE_MESSAGE(col->ScrollMax.y > 0.0f && col->ScrollbarY, "the card column scrolls at 1280x720");
    const float bar_pad = overlay_px(OVL_SCROLLBAR_PAD);
    ImRect bar = ImGui::GetWindowScrollbarRect(col, ImGuiAxis_Y);
    ImVec2 kmin, kmax;
    TEST_ASSERT_TRUE(media_rect("card_colors", &kmin, &kmax));
    printf("display scrollbar 1280x720: grab %.1f px wide, %.1f px right of the cards\n", bar.GetWidth() - 2.0f * bar_pad, bar.Min.x + bar_pad - kmax.x);
    TEST_ASSERT_TRUE_MESSAGE(bar.GetWidth() - 2.0f * bar_pad >= overlay_px(8.0f) - 0.5f, "scrollbar grab at least 8 units wide");
    TEST_ASSERT_TRUE_MESSAGE(bar.Min.x >= kmax.x - 0.5f, "scrollbar does not cover the cards");
    TEST_ASSERT_TRUE_MESSAGE(bar.Min.x + bar_pad - kmax.x <= overlay_px(16.0f), "scrollbar grab at most 16 units from the cards");
    /* na konci sloupce je vidět karta Frame rate i věta pod kartami */
    form_scroll_end();
    ImVec2 cmin, cmax, nmin, nmax;
    TEST_ASSERT_TRUE(imgui_overlay_test_item_rect("content", &cmin, &cmax));
    TEST_ASSERT_TRUE(media_rect("disp_note", &nmin, &nmax));
    TEST_ASSERT_TRUE_MESSAGE(nmin.y >= cmin.y && nmax.y <= cmax.y + 0.5f, "the note is visible after scrolling to the end");
    TEST_ASSERT_TRUE(media_rect("card_framerate", &kmin, &kmax));
    TEST_ASSERT_TRUE_MESSAGE(kmin.y >= cmin.y - 0.5f && kmax.y <= cmax.y + 0.5f, "the Frame rate card is visible after scrolling to the end");

    /* N01: úzké okno roluje celý obsah */
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, 640.0f, 480.0f);
    ImGuiWindow *content = media_content_window();
    TEST_ASSERT_NOT_NULL(content);
    TEST_ASSERT_NULL_MESSAGE(strstr(content->Name, "ovl_disp_col"), "the narrow display tab scrolls the whole content");
    TEST_ASSERT_TRUE(content->ScrollMax.y > 0.0f && content->ScrollbarY);
    bar = ImGui::GetWindowScrollbarRect(content, ImGuiAxis_Y);
    TEST_ASSERT_TRUE_MESSAGE(bar.GetWidth() - 2.0f * overlay_px(OVL_SCROLLBAR_PAD) >= overlay_px(8.0f) - 0.5f, "scrollbar grab at least 8 units wide (narrow)");

    /* N02: řada akcí tabu Media */
    static const char *const acts[] = {"act_resume", "act_speed", "act_quick_save", "act_quick_load", "act_screenshot", "act_fullscreen", "act_reset"};
    static const en_OVL_ACTION ids[] = {OVL_ACTION_RESUME,     OVL_ACTION_SPEED,      OVL_ACTION_QUICK_SAVE, OVL_ACTION_QUICK_LOAD,
                                        OVL_ACTION_SCREENSHOT, OVL_ACTION_FULLSCREEN, OVL_ACTION_RESET};
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}};
    const en_OVERLAY_FIXTURE fixtures[] = {OVERLAY_FIXTURE_DEFAULT, OVERLAY_FIXTURE_REPLAY, OVERLAY_FIXTURE_NO_FDC};
    /* pořadí důležitosti štítků (indexy do acts): Reset, Quick Save, Quick Load, Speed, Screenshot, Fullscreen;
     * o štítek přicházejí od konce */
    static const int keep_order[] = {6, 2, 3, 1, 4, 5};
    for (const float *sz : sizes) {
        float x0[7] = {0}, x1[7] = {0};
        bool base_shown[7] = {false}, base_avail[7] = {false};
        bool first = true;
        for (en_OVERLAY_FIXTURE f : fixtures) {
            form_start(OVERLAY_TAB_MEDIA, f, sz[0], sz[1]);
            form_scroll_end(); /* v úzkém okně je řada akcí až na konci obsahu */
            char msg[160];
            bool shown[7], avail[7];
            for (int i = 0; i < 7; i++) {
                snprintf(msg, sizeof(msg), "media actions %.0fx%.0f %s %s", sz[0], sz[1], s_fixture_names[f], acts[i]);
                ImVec2 mn, mx;
                TEST_ASSERT_TRUE_MESSAGE(media_rect(acts[i], &mn, &mx), msg);
                if (first) {
                    x0[i] = mn.x;
                    x1[i] = mx.x;
                } else {
                    /* řada se při změně stavu nepřeskládá */
                    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, x0[i], mn.x, msg);
                    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, x1[i], mx.x, msg);
                }
                char kc_name[48];
                snprintf(kc_name, sizeof(kc_name), "%s_keycap", acts[i]);
                shown[i] = media_rect(kc_name, NULL, NULL);
                avail[i] = overlay_action_keycap(ids[i], &s_fixture_view) != NULL;
                if (!avail[i]) TEST_ASSERT_FALSE_MESSAGE(shown[i], msg); /* bez zkratky nebo zakázaná akce */
                /* množina akcí se štítkem nezávisí na stavu: dostupná akce má štítek právě tehdy, když ho má ve stavu default */
                if (!first && avail[i] && base_avail[i]) TEST_ASSERT_EQUAL_INT_MESSAGE(base_shown[i] ? 1 : 0, shown[i] ? 1 : 0, msg);
            }
            /* pořadí ubírání: akce bez štítku nesmí předcházet méně důležitou akci se štítkem */
            bool missing = false;
            for (int k = 0; k < 6; k++) {
                const int i = keep_order[k];
                if (!avail[i]) continue;
                snprintf(msg, sizeof(msg), "media actions %.0fx%.0f %s: %s keeps its key cap before a more important one", sz[0], sz[1], s_fixture_names[f], acts[i]);
                if (shown[i]) TEST_ASSERT_FALSE_MESSAGE(missing, msg);
                else missing = true;
            }
            if (first) {
                char list[160] = "";
                for (int i = 0; i < 7; i++) {
                    base_shown[i] = shown[i];
                    base_avail[i] = avail[i];
                    if (shown[i]) snprintf(list + strlen(list), sizeof(list) - strlen(list), " %s", acts[i] + 4);
                }
                printf("media actions %.0fx%.0f key caps:%s\n", sz[0], sz[1], list[0] ? list : " none");
                if (sz[0] == 1280.0f) {
                    /* anglicky při 1280 x 720 mají štítek nejméně nejčastější akce */
                    TEST_ASSERT_TRUE_MESSAGE(shown[6], "Reset shows F12 at 1280x720");
                    TEST_ASSERT_TRUE_MESSAGE(shown[2], "Quick Save shows Alt+F8 at 1280x720");
                    TEST_ASSERT_TRUE_MESSAGE(shown[3], "Quick Load shows Alt+F9 at 1280x720");
                }
            }
            first = false;
        }
    }

    /* N02: tab Display - štítek u Fullscreen v obou velikostech, přepínač se zákazem neposune, dvojice tlačítek jednotně */
    for (int s = 0; s < 2; s++) {
        const float *sz = sizes[s];
        char msg[96];
        snprintf(msg, sizeof(msg), "display key caps %.0fx%.0f", sz[0], sz[1]);
        form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, sz[0], sz[1]);
        ImVec2 dmin, dmax, rmin, rmax;
        TEST_ASSERT_TRUE_MESSAGE(media_rect("disp_fullscreen", &dmin, &dmax), msg);
        TEST_ASSERT_TRUE_MESSAGE(overlay_action_keycap(OVL_ACTION_FULLSCREEN, &s_fixture_view) != NULL, msg);
        TEST_ASSERT_TRUE_MESSAGE(media_rect("disp_fullscreen_keycap", NULL, NULL), msg);
        form_scroll_end();
        const bool kw = media_rect("disp_fix_w_keycap", NULL, NULL), kh = media_rect("disp_fix_h_keycap", NULL, NULL);
        if (overlay_action_keycap(OVL_ACTION_FIX_ASPECT_W, &s_fixture_view) && overlay_action_keycap(OVL_ACTION_FIX_ASPECT_H, &s_fixture_view))
            TEST_ASSERT_EQUAL_INT_MESSAGE(kw ? 1 : 0, kh ? 1 : 0, msg);
        form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_REPLAY, sz[0], sz[1]);
        TEST_ASSERT_TRUE_MESSAGE(media_rect("disp_fullscreen", &rmin, &rmax), msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, dmin.x, rmin.x, msg);
    }
    form_start(OVERLAY_TAB_MEDIA, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
}

/**
 * Rozbalovací výběr klávesnicí a nové zkratky: Enter rozbalí seznam s
 * fokusem na zvolené položce, šipky se pohybují, Enter volí, Esc zavře
 * jen seznam (overlay zůstane otevřený); výchozí fokus tabů; zkratky
 * Alt+P, Alt+N, Alt+F6, Alt+F7, Alt+W a Alt+H v okně overlaye.
 */
void test_machine_display_keyboard(void)
{
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    TEST_ASSERT_TRUE(ov_overlay_focused());

    /* výchozí fokus tabu Machine: zvolený segment řádku Emulation (K17); Enter na něm nic nemění */
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the default focus is the selected Emulation segment");
    ov_key(ImGuiKey_LeftArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_PAUSE_TOGGLE, overlay_actions_test_take_last(), "arrow left moves to Running");
    TEST_ASSERT_EQUAL_INT(1, s_pause_toggle_calls);
    g_emulator.paused = true;

    /* dolů přes Reset na ROM; Reset se cestou nespustí */
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_DownArrow);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE_MESSAGE(ovl_dropdown_is_open(), "Enter opens the ROM list");
    TEST_ASSERT_FALSE_MESSAGE(ov_overlay_focused(), "the list has the focus");
    /* číslice při rozbaleném seznamu tab nepřepne */
    ov_key(ImGuiKey_3);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    /* Esc zavře jen seznam */
    ov_key(ImGuiKey_Escape);
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_FALSE(ovl_dropdown_is_open());
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "Esc closes the list, not the overlay");
    TEST_ASSERT_TRUE(ov_overlay_focused());
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    /* znovu rozbalit: fokus má zvolená položka (první), šipka dolů přeskočí nadpis oddílu na další ROM */
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_TRUE(ovl_dropdown_is_open());
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_Enter);
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_ROM_SELECT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT_MESSAGE(s_fixture_view.machine.rom[2].row, overlay_actions_test_last_value(), "arrow down skips the section heading");
    TEST_ASSERT_FALSE(ovl_dropdown_is_open());
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the focus returns to the overlay");
    /* mezerník volí také */
    ov_key(ImGuiKey_Space);
    TEST_ASSERT_TRUE(ovl_dropdown_is_open());
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_DownArrow);
    ov_key(ImGuiKey_Space);
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_ROM_SELECT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(s_fixture_view.machine.rom[3].row, overlay_actions_test_last_value());
    /* teď Esc zavře overlay */
    ov_key(ImGuiKey_Escape);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* zkratky tabulky akcí */
    g_emulator.paused = false;
    imgui_overlay_open();
    form_start(OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    s_pause_toggle_calls = 0;
    ov_mod_key(ImGuiKey_P, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_PAUSE_TOGGLE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_pause_toggle_calls);
    ov_mod_key(ImGuiKey_P, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "Alt+Shift+P is not Pause");
    g_customspeed.speed_in_percentage_requested = 180;
    ov_mod_key(ImGuiKey_N, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SPEED_NORMAL, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_UINT(100, g_customspeed.speed_in_percentage_requested);
    ov_mod_key(ImGuiKey_F6, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SNAPSHOT_SAVE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_save_calls);
    ov_mod_key(ImGuiKey_W, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FIX_ASPECT_W, overlay_actions_test_take_last());
    ov_mod_key(ImGuiKey_H, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_FIX_ASPECT_H, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT('H', s_fix_aspect_last);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    ov_mod_key(ImGuiKey_F7, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SNAPSHOT_LOAD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_snapshot_load_calls);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* výchozí fokus tabu Display: zvolený segment řádku Color schema (první prvek sloupce karet); Enter na
     * něm nic nemění, šipka vpravo přejde na Grayscale */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_DISPLAY, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    g_display.color_schema = DISPLAY_NORMAL;
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the default focus is the selected Color schema segment");
    ov_key(ImGuiKey_RightArrow);
    ov_key(ImGuiKey_Enter);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_COLORS, overlay_actions_test_take_last(), "arrow right moves to Grayscale");
    TEST_ASSERT_EQUAL_INT(DISPLAY_GRAYSCALE, g_display.color_schema);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_fullscreen_calls, "the Fullscreen toggle is no longer the default item");
    imgui_overlay_close();
}

/**
 * Rozbalený seznam v malém okně a u okraje: zůstává v hlavním viewportu,
 * není užší než prvek a u spodního okraje se rozbalí nahoru.
 */
void test_dropdown_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {2560.0f, 1440.0f}};
    for (const float *sz : sizes) {
        for (int t = 0; t < 2; t++) {
            imgui_overlay_open();
            form_start(t ? OVERLAY_TAB_DISPLAY : OVERLAY_TAB_MACHINE, OVERLAY_FIXTURE_LONG_TEXTS, sz[0], sz[1]);
            const char *item = t ? "disp_framerate" : "mach_rom";
            char msg[96];
            snprintf(msg, sizeof(msg), "%s %.0fx%.0f", item, sz[0], sz[1]);
            ImGuiWindow *content = media_content_window();
            TEST_ASSERT_NOT_NULL(content);
            if (t && content->ScrollMax.y > 0.0f) {
                /* úzké okno: karta Frame rate je až dole */
                ImGui::SetScrollY(content, content->ScrollMax.y);
                for (int i = 0; i < 3; i++) ov_frame();
            }
            ImVec2 rmin, rmax;
            TEST_ASSERT_TRUE_MESSAGE(media_rect(item, &rmin, &rmax), msg);
            media_click(item);
            TEST_ASSERT_TRUE_MESSAGE(ovl_dropdown_is_open(), msg);
            ImGuiContext &g = *GImGui;
            TEST_ASSERT_TRUE_MESSAGE(g.OpenPopupStack.Size > 0 && g.OpenPopupStack.back().Window, msg);
            const ImGuiWindow *list = g.OpenPopupStack.back().Window;
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            TEST_ASSERT_TRUE_MESSAGE(list->Pos.x >= vp->Pos.x && list->Pos.x + list->Size.x <= vp->Pos.x + vp->Size.x + 0.5f, msg);
            TEST_ASSERT_TRUE_MESSAGE(list->Pos.y >= vp->Pos.y && list->Pos.y + list->Size.y <= vp->Pos.y + vp->Size.y + 0.5f, msg);
            TEST_ASSERT_TRUE_MESSAGE(list->Size.x >= rmax.x - rmin.x - 0.5f, msg);
            /* seznam nesahá přes patičku overlaye (posudek N10) */
            TEST_ASSERT_TRUE_MESSAGE(list->Pos.y + list->Size.y <= sz[1] - floorf(OVL_FOOTER_H * overlay_unit_for(ImVec2(sz[0], sz[1]))) + 1.0f, msg);
            printf("dropdown %s: list y %.1f to %.1f, item y %.1f to %.1f\n", msg, list->Pos.y, list->Pos.y + list->Size.y, rmin.y, rmax.y);
            /* seznam prvek nepřekrývá: je celý pod ním, nebo celý nad ním */
            TEST_ASSERT_TRUE_MESSAGE(list->Pos.y >= rmax.y || list->Pos.y + list->Size.y <= rmin.y, msg);
            ov_key(ImGuiKey_Escape);
            for (int i = 0; i < 2; i++) ov_frame();
            TEST_ASSERT_FALSE_MESSAGE(ovl_dropdown_is_open(), msg);
            TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), msg);
            imgui_overlay_close();
            for (int i = 0; i < 2; i++) ov_frame();
        }
    }
}

/**
 * @brief Pořadí okna v pořadí kreslení ImGui (větší = výš).
 * @param w Okno.
 * @return Index v ImGuiContext::Windows; -1 = okno není.
 */
static int window_display_index(const ImGuiWindow *w)
{
    const ImGuiContext &g = *GImGui;
    for (int i = 0; i < g.Windows.Size; i++)
        if (g.Windows[i] == w) return i;
    return -1;
}

/**
 * @brief Ověří, že toast potvrzení je v posledním snímku vykreslený nad overlayem a všemi jeho podokny.
 * @param msg Popis situace do zprávy.
 */
static void expect_toast_above_overlay(const char *msg)
{
    const ImGuiContext &g = *GImGui;
    ImGuiWindow *toast = ImGui::FindWindowByName("##SnapshotNotification");
    ImGuiWindow *overlay = ImGui::FindWindowByName(OVERLAY_WINDOW_ID);
    TEST_ASSERT_NOT_NULL_MESSAGE(toast, msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(overlay, msg);
    TEST_ASSERT_TRUE_MESSAGE(toast->WasActive && !toast->Hidden, msg);
    TEST_ASSERT_TRUE_MESSAGE(toast->DrawList->VtxBuffer.Size > 0, msg);
    const int toast_index = window_display_index(toast);
    for (int i = 0; i < g.Windows.Size; i++) {
        if (g.Windows[i]->RootWindow != overlay || !g.Windows[i]->WasActive) continue;
        TEST_ASSERT_TRUE_MESSAGE(toast_index > i, msg);
    }
    /* skutečné pořadí kreslení: seznam toastu jde do výstupu ImGui za seznamem overlaye */
    const ImDrawData *dd = ImGui::GetDrawData();
    int dd_toast = -1, dd_overlay = -1;
    for (int i = 0; i < dd->CmdLists.Size; i++) {
        if (dd->CmdLists[i] == toast->DrawList) dd_toast = i;
        if (dd->CmdLists[i] == overlay->DrawList) dd_overlay = i;
    }
    TEST_ASSERT_TRUE_MESSAGE(dd_overlay >= 0 && dd_toast > dd_overlay, msg);
}

/**
 * Review W2: toast potvrzení (Quick Save, screenshot) je při otevřeném
 * overlayi vidět. Kreslí ho skutečné imgui_snapshot_notification(); okno
 * toastu vzniklo před otevřením overlaye (jinak by bylo nahoře už jako
 * nové okno).
 */
void test_toast_above_overlay(void)
{
    /* toast poprvé při zavřeném overlayi: okno vznikne */
    snapshot_notification_show("First toast", false);
    for (int i = 0; i < 2; i++) ov_frame();
    ImGuiWindow *toast = ImGui::FindWindowByName("##SnapshotNotification");
    TEST_ASSERT_NOT_NULL(toast);

    /* overlay se otevře a vezme si fokus: bez opravy by toast zůstal pod ním */
    imgui_overlay_open();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE(ov_overlay_focused());
    expect_toast_above_overlay("toast shown before the overlay opened");

    /* potvrzení akce spuštěné z overlaye */
    snapshot_notification_show("Quick save done", false);
    for (int i = 0; i < 3; i++) ov_frame();
    expect_toast_above_overlay("toast shown while the overlay is open");

    /* klik do overlaye mu vrátí fokus; toast zůstává nahoře a klik nepohltí */
    ov_click_item("tab1");
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_MACHINE, imgui_overlay_get_tab());
    expect_toast_above_overlay("toast after a click into the overlay");
    TEST_ASSERT_TRUE_MESSAGE(ov_overlay_focused(), "the toast never takes the focus");

    /* modální dialog nad overlayem: toast jeho pořadí nemění */
    s_modal = true;
    for (int i = 0; i < 3; i++) ov_frame();
    ImGuiWindow *modal = ImGui::FindWindowByName("Test modal###ov_test_modal");
    TEST_ASSERT_NOT_NULL(modal);
    TEST_ASSERT_TRUE_MESSAGE(window_display_index(modal) > window_display_index(toast), "the toast stays under a modal dialog");
    s_modal = false;
    for (int i = 0; i < 4; i++) ov_frame();
    imgui_overlay_close();
}

/* ================================================================
 * Režim snímků
 * ================================================================ */

/**
 * @brief Uloží celé plátno jako PNG.
 * @param dir Adresář. @param file Jméno souboru.
 * @return true = uloženo.
 */
static bool shot_save(const char *dir, const char *file)
{
    const int w = s_canvas_w, h = s_canvas_h;
    size_t pitch = (size_t)w * 4u;
    unsigned char *px = (unsigned char *)malloc(pitch * (size_t)h);
    unsigned char *img = (unsigned char *)malloc(pitch * (size_t)h);
    if (!px || !img) { free(px); free(img); return false; }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* GL má počátek dole */
    for (int y = 0; y < h; y++) memcpy(img + (size_t)y * pitch, px + (size_t)(h - 1 - y) * pitch, pitch);
    size_t len = 0;
    uint8_t *png = png_encode_rgba(img, (uint32_t)w, (uint32_t)h, &len);
    bool ok = false;
    if (png) {
        gchar *path = g_build_filename(dir, file, NULL);
        ok = g_file_set_contents(path, (const gchar *)png, (gssize)len, NULL);
        if (!ok) fprintf(stderr, "can't write %s\n", path);
        else printf("shot: %s\n", path);
        g_free(path);
        g_free(png);
    }
    free(px);
    free(img);
    return ok;
}

/**
 * @brief Snímky všech tabů z testovacího stavu OVERLAY_FIXTURE_DEFAULT nad jednobarevným "obrazem" a snímek galerie widgetů nad bílou plochou.
 * @param dir    Adresář snímků.
 * @param prefix Označení etapy do jména souboru.
 * @param lang   Jazyk do jména souboru.
 * @return Počet snímků, které se nepodařilo uložit.
 */
static int shots_run(const char *dir, const char *prefix, const char *lang)
{
    static const char *const tab_names[OVERLAY_TAB_COUNT] = {"media", "machine", "display", "sound", "input", "capture"};
    st_OVERLAY_VIEW v;
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    int failed = 0;
    for (int t = 0; t < (int)OVERLAY_TAB_COUNT; t++) {
        imgui_overlay_set_tab((en_OVERLAY_TAB)t);
        /* tři snímky: rozvržení se ustálí */
        for (int i = 0; i < 3; i++) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            imgui_overlay_draw(&v, vp->Pos, vp->Size);
            ImGui::Render();
        }
        if (s_bind_fb && s_fbo) s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, s_canvas_w, s_canvas_h);
        /* náhrada obrazu emulátoru: světlá plocha, aby bylo vidět ztmavení */
        glClearColor(0.25f, 0.55f, 0.95f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        char file[160];
        snprintf(file, sizeof(file), "%s-%s-%dx%d-%s.png", prefix, lang, s_canvas_w, s_canvas_h, tab_names[t]);
        if (!shot_save(dir, file)) failed++;
    }

    /* galerie widgetů ve všech stavech */
    for (int i = 0; i < 3; i++) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        gallery_draw(vp->Pos, vp->Size);
        ImGui::Render();
    }
    if (s_bind_fb && s_fbo) s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_canvas_w, s_canvas_h);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f); /* nejsvětlejší možný obraz */
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    char file[160];
    if (s_canvas_w == 1280 && s_canvas_h == 720) snprintf(file, sizeof(file), "%s-gallery.png", prefix);
    else snprintf(file, sizeof(file), "%s-gallery-%dx%d.png", prefix, s_canvas_w, s_canvas_h);
    if (!shot_save(dir, file)) failed++;
    return failed;
}

/**
 * @brief Snímky tabu Media ve všech testovacích stavech nad jednobarevným "obrazem".
 * @param dir    Adresář snímků.
 * @param prefix Označení etapy do jména souboru.
 * @param lang   Jazyk do jména souboru.
 * @return Počet snímků, které se nepodařilo uložit.
 */
static int shots_run_media(const char *dir, const char *prefix, const char *lang)
{
    static st_OVERLAY_VIEW v;
    int failed = 0;
    for (int f = 0; f < (int)OVERLAY_FIXTURE_COUNT; f++) {
        overlay_view_make_fixture(&v, (en_OVERLAY_FIXTURE)f);
        /* přepnutí tabu tam a zpět: tab dá fokus svému výchozímu prvku jako po otevření */
        imgui_overlay_set_tab(OVERLAY_TAB_MACHINE);
        imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
        for (int i = 0; i < 4; i++) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            imgui_overlay_draw(&v, vp->Pos, vp->Size);
            ImGui::Render();
        }
        if (s_bind_fb && s_fbo) s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, s_canvas_w, s_canvas_h);
        glClearColor(0.25f, 0.55f, 0.95f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        char file[160];
        snprintf(file, sizeof(file), "%s-%s-%dx%d-%s.png", prefix, lang, s_canvas_w, s_canvas_h, s_fixture_names[f]);
        if (!shot_save(dir, file)) failed++;
    }
    return failed;
}

/**
 * @brief Vykreslí několik snímků overlaye z kopie stavu (bez náhrady hlavního okna).
 * @param v Kopie stavu. @param n Počet snímků.
 */
static void shot_frames(const st_OVERLAY_VIEW *v, int n)
{
    for (int i = 0; i < n; i++) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        imgui_overlay_draw(v, vp->Pos, vp->Size);
        ImGui::Render();
    }
}

/**
 * @brief Snímky výsuvného listu výměny média, pruhu nedávných médií a volby mechaniky nad jednobarevným "obrazem".
 *
 * Stavy: recent (pruh s položkami, bez listu), fdd0 a fdd3 (list u první a
 * poslední mechaniky), cmt, qd, filter (list u FDD 0 s textem ve filtru),
 * pick (volba "Insert into"), long-fdd1 (jména o 200 znacích a chybějící
 * soubory) a empty-fdd0 (prázdná historie).
 *
 * @param dir    Adresář snímků.
 * @param prefix Označení etapy do jména souboru.
 * @param lang   Jazyk do jména souboru.
 * @return Počet snímků, které se nepodařilo uložit.
 */
static int shots_run_sheet(const char *dir, const char *prefix, const char *lang)
{
    static st_OVERLAY_VIEW v;
    static const char *const states[] = {"recent", "fdd0", "fdd3", "cmt", "qd", "filter", "pick", "long-fdd1", "empty-fdd0"};
    int failed = 0;
    for (int s = 0; s < (int)(sizeof(states) / sizeof(states[0])); s++) {
        overlay_sheet_reset();
        overlay_view_make_fixture(&v, (s == 7) ? OVERLAY_FIXTURE_LONG_TEXTS : ((s == 8) ? OVERLAY_FIXTURE_EMPTY : OVERLAY_FIXTURE_DEFAULT));
        /* přepnutí tabu tam a zpět: tab dá fokus svému výchozímu prvku jako po otevření */
        imgui_overlay_set_tab(OVERLAY_TAB_MACHINE);
        imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
        shot_frames(&v, 3);
        switch (s) {
            case 1:
            case 5:
            case 8:
                overlay_sheet_open(OVL_SHEET_FDD, 0);
                break;
            case 2:
                overlay_sheet_open(OVL_SHEET_FDD, 3);
                break;
            case 3:
                overlay_sheet_open(OVL_SHEET_CMT, 0);
                break;
            case 4:
                overlay_sheet_open(OVL_SHEET_QD, 0);
                break;
            case 6:
                for (unsigned i = 0; i < v.recent_count; i++) {
                    if (v.recent[i].kind != OVERLAY_MEDIA_DSK) continue;
                    overlay_sheet_open_pick(&v.recent[i], (int)i);
                    break;
                }
                break;
            case 7:
                overlay_sheet_open(OVL_SHEET_FDD, 1);
                break;
            default:
                break;
        }
        if (s == 5) {
            shot_frames(&v, 2);
            ImGui::GetIO().AddInputCharacter('d');
            ImGui::GetIO().AddInputCharacter('i');
        }
        shot_frames(&v, 4);
        if (s_bind_fb && s_fbo) s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, s_canvas_w, s_canvas_h);
        glClearColor(0.25f, 0.55f, 0.95f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        char file[160];
        snprintf(file, sizeof(file), "%s-%s-%dx%d-%s.png", prefix, lang, s_canvas_w, s_canvas_h, states[s]);
        if (!shot_save(dir, file)) failed++;
    }
    overlay_sheet_reset();
    return failed;
}

/**
 * @brief Uloží současný výstup ImGui nad jednobarevným "obrazem" jako snímek.
 * @param dir Adresář snímků. @param file Jméno souboru.
 * @return true = uloženo.
 */
static bool shot_render_save(const char *dir, const char *file)
{
    if (s_bind_fb && s_fbo) s_bind_fb(TEST_GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_canvas_w, s_canvas_h);
    glClearColor(0.25f, 0.55f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return shot_save(dir, file);
}

/**
 * @brief Snímky tabů Machine a Display z testovacího stavu OVERLAY_FIXTURE_DEFAULT a rozbaleného výběru ROM.
 *
 * Soubory `<etapa>-<jazyk>-<šířka>x<výška>-machine.png`, `-display.png`,
 * `-machine-rom.png` (výběr ROM rozbalený klikem myši) a, pokud se tab
 * Display roluje, `-display-end.png` (odrolováno na konec).
 *
 * @param dir    Adresář snímků.
 * @param prefix Označení etapy do jména souboru.
 * @param lang   Jazyk do jména souboru.
 * @return Počet snímků, které se nepodařilo uložit.
 */
static int shots_run_o5(const char *dir, const char *prefix, const char *lang)
{
    static st_OVERLAY_VIEW v;
    int failed = 0;
    char file[160];
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);

    /* přepnutí tabu tam a zpět: tab dá fokus svému výchozímu prvku jako po otevření */
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    imgui_overlay_set_tab(OVERLAY_TAB_MACHINE);
    shot_frames(&v, 4);
    snprintf(file, sizeof(file), "%s-%s-%dx%d-machine.png", prefix, lang, s_canvas_w, s_canvas_h);
    if (!shot_render_save(dir, file)) failed++;

    /* rozbalený výběr ROM: klik na prvek */
    ImVec2 mn, mx;
    if (imgui_overlay_test_item_rect("mach_rom", &mn, &mx)) {
        io.AddMousePosEvent((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        shot_frames(&v, 1);
        io.AddMouseButtonEvent(0, true);
        shot_frames(&v, 1);
        io.AddMouseButtonEvent(0, false);
        shot_frames(&v, 3);
        /* ukazatel nad druhou ROM nabídky */
        ImVec2 imin, imax;
        if (ovl_test_dropdown_item_rect(2, &imin, &imax)) {
            io.AddMousePosEvent((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f);
            shot_frames(&v, 2);
        }
        snprintf(file, sizeof(file), "%s-%s-%dx%d-machine-rom.png", prefix, lang, s_canvas_w, s_canvas_h);
        if (!ovl_dropdown_is_open() || !shot_render_save(dir, file)) failed++;
        io.AddMousePosEvent(-1.0f, -1.0f);
        io.AddKeyEvent(ImGuiKey_Escape, true);
        shot_frames(&v, 1);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        shot_frames(&v, 3);
    } else {
        failed++;
    }

    imgui_overlay_set_tab(OVERLAY_TAB_DISPLAY);
    shot_frames(&v, 4);
    snprintf(file, sizeof(file), "%s-%s-%dx%d-display.png", prefix, lang, s_canvas_w, s_canvas_h);
    if (!shot_render_save(dir, file)) failed++;

    /* tab Display odrolovaný na konec (karta Frame rate a věta pod kartami; posudek v2 N01) */
    ImGuiWindow *content = media_content_window();
    if (content && content->ScrollMax.y > 0.0f) {
        ImGui::SetScrollY(content, content->ScrollMax.y);
        shot_frames(&v, 3);
        snprintf(file, sizeof(file), "%s-%s-%dx%d-display-end.png", prefix, lang, s_canvas_w, s_canvas_h);
        if (!shot_render_save(dir, file)) failed++;
        ImGui::SetScrollY(content, 0.0f);
        shot_frames(&v, 2);
    }
    return failed;
}

/* ================================================================
 * Taby Sound, Input a Capture
 * ================================================================ */

/** @brief Jména karet tabu Sound. */
static const char *const s_sound_cards[] = {"card_output", "card_channels"};
/** @brief Jména karet tabu Input. */
static const char *const s_input_cards[] = {"card_keyboard", "card_joystick"};
/** @brief Jména karet tabu Capture. */
static const char *const s_capture_cards[] = {"card_screenshot", "card_video", "card_replay"};

/**
 * @brief Geometrie tabu Sound, Input nebo Capture v posledním kreslení: karty v okně, prvky v kartách, nic se nepřekrývá.
 * @param msg Popis situace do zprávy. @param tab Tab. @param all true = všechny karty a všechny prvky musí být vidět.
 */
static void o6_check_geometry(const char *msg, en_OVERLAY_TAB tab, bool all)
{
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    if (tab == OVERLAY_TAB_SOUND) {
        static const char *const output[] = {"snd_listen_icon", "snd_listen", "snd_master_icon", "snd_master", "snd_mute", "snd_filter", "snd_filter_note", "snd_listen_note"};
        static const char *const channels[] = {"snd_ch0", "snd_ch1", "snd_ch2", "snd_ch3", "snd_ch4", "snd_ch5", "snd_ch6", "snd_ch7", "snd_ch8", "snd_reset"};
        form_check_cards(msg, s_sound_cards, 2, all);
        const int n1 = media_check_group(msg, "card_output", pad, output, 8);
        const int n2 = media_check_group(msg, "card_channels", pad, channels, 10);
        if (all) {
            const int filter = s_fixture_view.sound.has_output_filter ? 2 : 0;
            TEST_ASSERT_EQUAL_INT_MESSAGE(5 + (overlay_style_has_icons() ? 1 : 0) + filter, n1, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)s_fixture_view.sound.channel_count + 1, n2, msg);
        }
    } else if (tab == OVERLAY_TAB_INPUT) {
        static const char *const keyboard[] = {"inp_vkbd", "inp_autotype", "inp_note"};
        static const char *const joystick[] = {"inp_joy_device", "inp_joy_setup"};
        form_check_cards(msg, s_input_cards, 2, all);
        const int n1 = media_check_group(msg, "card_keyboard", pad, keyboard, 3);
        const int n2 = media_check_group(msg, "card_joystick", pad, joystick, 2);
        if (all) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(3, n1, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(2, n2, msg);
        }
    } else {
        static const char *const shot[] = {"cap_shot", "cap_shot_as", "cap_series", "cap_series_keycap", "cap_shot_folder", "cap_shot_setup"};
        static const char *const video[] = {"cap_video", "cap_video_pause", "cap_realtime", "cap_realtime_keycap", "cap_video_folder", "cap_video_setup"};
        static const char *const replay[] = {"cap_replay_record", "cap_replay_play", "cap_replay_stop", "cap_replay_window", "cap_replay_status"};
        form_check_cards(msg, s_capture_cards, 3, all);
        const int n1 = media_check_group(msg, "card_screenshot", pad, shot, 6);
        const int n2 = media_check_group(msg, "card_video", pad, video, 6);
        const int n3 = media_check_group(msg, "card_replay", pad, replay, 5);
        if (all) {
            TEST_ASSERT_TRUE_MESSAGE(n1 >= 5, msg); /* štítek zkratky smí ustoupit */
            TEST_ASSERT_TRUE_MESSAGE(n2 >= 5, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(5, n3, msg);
        }
    }
}

/**
 * Geometrie tabů Sound, Input a Capture (Review Focus 3 a 4): testovací
 * stavy default, empty, long a replay při 640 x 480, 1280 x 720,
 * 1920 x 1080 a 2560 x 1440, s texty běžnými i o 40 % delšími. Karty v
 * ploše obsahu, prvky v kartách, nic se nepřekrývá; od 1280 x 720 se obsah
 * neroluje a karty jsou vedle sebe ve stejné výšce, v úzkém okně pod sebou.
 */
void test_o6_geometry(void)
{
    const float sizes[][2] = {{640.0f, 480.0f}, {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}};
    const en_OVERLAY_FIXTURE fixtures[] = {OVERLAY_FIXTURE_DEFAULT, OVERLAY_FIXTURE_EMPTY, OVERLAY_FIXTURE_LONG_TEXTS, OVERLAY_FIXTURE_REPLAY};
    const en_OVERLAY_TAB tabs[] = {OVERLAY_TAB_SOUND, OVERLAY_TAB_INPUT, OVERLAY_TAB_CAPTURE};
    static const char *const tab_names[] = {"sound", "input", "capture"};
    static const char *const last_item[] = {"snd_reset", "inp_joy_setup", "cap_replay_status"};
    for (int stretch = 0; stretch < 2; stretch++) {
        overlay_test_set_stretch(stretch != 0);
        for (const float *sz : sizes) {
            for (en_OVERLAY_FIXTURE f : fixtures) {
                for (int t = 0; t < 3; t++) {
                    form_start(tabs[t], f, sz[0], sz[1]);
                    char msg[112];
                    snprintf(msg, sizeof(msg), "%s %.0fx%.0f %s%s", tab_names[t], sz[0], sz[1], s_fixture_names[f], stretch ? " +40%" : "");
                    ImGuiWindow *content = media_content_window();
                    TEST_ASSERT_NOT_NULL_MESSAGE(content, msg);
                    const bool wide = sz[0] >= 1280.0f;
                    const char *const *cards = (t == 0) ? s_sound_cards : ((t == 1) ? s_input_cards : s_capture_cards);
                    const int ncards = (t == 2) ? 3 : 2;
                    ImVec2 amin, amax, bmin, bmax;
                    if (wide) {
                        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, content->ScrollMax.y, msg);
                        o6_check_geometry(msg, tabs[t], true);
                        /* sloupce: každá další karta je vpravo od předchozí, začíná i končí ve stejné výšce */
                        for (int c = 1; c < ncards; c++) {
                            TEST_ASSERT_TRUE(media_rect(cards[c - 1], &amin, &amax));
                            TEST_ASSERT_TRUE(media_rect(cards[c], &bmin, &bmax));
                            TEST_ASSERT_TRUE_MESSAGE(bmin.x >= amax.x, msg);
                            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amin.y, bmin.y, msg);
                            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amax.y, bmax.y, msg);
                        }
                    } else {
                        /* úzké okno (K03): jeden sloupec; kontrola na začátku a na konci rolování */
                        o6_check_geometry(msg, tabs[t], false);
                        for (int c = 1; c < ncards; c++) {
                            TEST_ASSERT_TRUE(media_rect(cards[c - 1], &amin, &amax));
                            TEST_ASSERT_TRUE(media_rect(cards[c], &bmin, &bmax));
                            TEST_ASSERT_TRUE_MESSAGE(bmin.y >= amax.y, msg);
                            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, amin.x, bmin.x, msg);
                        }
                        ImGui::SetScrollY(content, content->ScrollMax.y);
                        for (int i = 0; i < 3; i++) ov_frame();
                        o6_check_geometry(msg, tabs[t], false);
                        TEST_ASSERT_TRUE_MESSAGE(media_rect(last_item[t], NULL, NULL), msg);
                        ImGui::SetScrollY(content, 0.0f);
                        for (int i = 0; i < 2; i++) ov_frame();
                    }
                }
            }
        }
    }
    overlay_test_set_stretch(false);

    /* počet řádků kanálů a řádek propusti podle kopie */
    form_start(OVERLAY_TAB_SOUND, OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f);
    TEST_ASSERT_TRUE(media_rect("snd_ch0", NULL, NULL));
    TEST_ASSERT_FALSE_MESSAGE(media_rect("snd_ch1", NULL, NULL), "a machine without PSG has one channel");
    TEST_ASSERT_FALSE_MESSAGE(media_rect("snd_filter", NULL, NULL), "no filter row on a machine without the filter");
    TEST_ASSERT_TRUE_MESSAGE(media_rect("snd_listen_note", NULL, NULL), "the sentence about hearing the changes is always there");
    TEST_ASSERT_TRUE_MESSAGE(media_rect("snd_listen", NULL, NULL), "the Listen toggle is always there");

    /* přepínač Listen ve všech čtyřech stavech (jiná věta pod kartou, zakázaný přepínač): 640 x 480 a
     * 1280 x 720, texty běžné i o 40 % delší; přepínač je první řádek karty Output */
    static const int listen_states[] = {OVERLAY_LISTEN_OFF, OVERLAY_LISTEN_ON, OVERLAY_LISTEN_BLOCKED_PAUSED, OVERLAY_LISTEN_BLOCKED_RUNNING};
    for (int stretch = 0; stretch < 2; stretch++) {
        overlay_test_set_stretch(stretch != 0);
        for (int z = 0; z < 2; z++) {
            for (int st : listen_states) {
                form_start(OVERLAY_TAB_SOUND, OVERLAY_FIXTURE_DEFAULT, sizes[z][0], sizes[z][1]);
                s_fixture_view.sound.listen = st;
                s_fixture_view.paused = (st == OVERLAY_LISTEN_OFF || st == OVERLAY_LISTEN_BLOCKED_PAUSED);
                for (int i = 0; i < 4; i++) ov_frame();
                char msg[112];
                snprintf(msg, sizeof(msg), "sound listen=%d %.0fx%.0f%s", st, sizes[z][0], sizes[z][1], stretch ? " +40%" : "");
                ImGuiWindow *content = media_content_window();
                TEST_ASSERT_NOT_NULL_MESSAGE(content, msg);
                const bool wide = sizes[z][0] >= 1280.0f;
                if (wide) TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 0.0f, content->ScrollMax.y, msg);
                o6_check_geometry(msg, OVERLAY_TAB_SOUND, wide);
                ImVec2 lmin, lmax, mmin, mmax, cmin, cmax;
                TEST_ASSERT_TRUE_MESSAGE(media_rect("snd_listen", &lmin, &lmax), msg);
                TEST_ASSERT_TRUE_MESSAGE(media_rect("snd_master", &mmin, &mmax), msg);
                TEST_ASSERT_TRUE_MESSAGE(media_rect("card_output", &cmin, &cmax), msg);
                TEST_ASSERT_TRUE_MESSAGE(lmax.y <= mmin.y, msg);
                TEST_ASSERT_TRUE_MESSAGE(lmax.x <= cmax.x && lmin.x >= cmin.x, msg);
                if (wide) TEST_ASSERT_EQUAL_INT_MESSAGE(s_fixture_view.paused, sound_chip_paused(), msg);
            }
        }
    }
    overlay_test_set_stretch(false);
    form_start(OVERLAY_TAB_SOUND, OVERLAY_FIXTURE_LONG_TEXTS, 1280.0f, 720.0f);
    TEST_ASSERT_TRUE_MESSAGE(media_rect("snd_ch8", NULL, NULL), "two PSG chips give nine channels");
    /* štítky zkratek jen u akcí, které právě fungují */
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1920.0f, 1080.0f);
    TEST_ASSERT_TRUE(media_rect("cap_series_keycap", NULL, NULL));
    TEST_ASSERT_TRUE(media_rect("cap_realtime_keycap", NULL, NULL));
}

/** @brief Živý stav podvrženého hostitelského joysticku (test_overlay_holds_joystick()). */
static uint8_t s_fake_joy = 0xff;

/** @brief Náhrada g_joy.host_scan: vrací s_fake_joy. */
static uint8_t fake_joy_scan(en_JOY_DEVID joy_devid)
{
    (void)joy_devid;
    return s_fake_joy;
}

/**
 * Joystick emulovaného stroje je při otevřeném overlayi v klidu.
 *
 * Port typu JOYSTICK (živé čtení hostitele při každém IN): otevření zadržení
 * nastaví, platí i při poslechu na tabu Sound (běžící emulace); po zavření trvá, dokud
 * je tlačítko hostitelského joysticku stisknuté, a s jeho uvolněním skončí.
 * Port typu NUM_KEYPAD: směr držený při otevření se klidovým skenem uvolní.
 */
void test_overlay_holds_joystick(void)
{
    const en_JOY_TYPE saved_type0 = g_joy.dev[JOY_DEVID_0].type;
    const en_JOY_TYPE saved_type1 = g_joy.dev[JOY_DEVID_1].type;
    const joy_host_read_fn saved_scan = g_joy.host_scan;
    g_joy.host_scan = fake_joy_scan;
    g_joy.dev[JOY_DEVID_0].type = JOY_TYPE_JOYSTICK;
    g_joy.dev[JOY_DEVID_1].type = JOY_TYPE_NUM_KEYPAD;
    const uint8_t pressed = (uint8_t) ~((1 << JOY_STATEBIT_LEFT) | (1 << JOY_STATEBIT_TRIG1));
    s_fake_joy = pressed;
    g_joy.dev[JOY_DEVID_1].state = (uint8_t) ~(1 << JOY_STATEBIT_UP); /* směr držený na numerické klávesnici */

    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_FALSE(joy_host_is_held());
    TEST_ASSERT_FALSE(imgui_overlay_blocks_joystick());
    TEST_ASSERT_EQUAL_HEX8(pressed, joy_read_byte(JOY_DEVID_0));

    /* otevření: oba porty v klidu */
    imgui_overlay_open();
    ov_drain_inputs();
    TEST_ASSERT_TRUE(joy_host_is_held());
    TEST_ASSERT_TRUE(imgui_overlay_blocks_joystick());
    TEST_ASSERT_EQUAL_HEX8(0xff, joy_read_byte(JOY_DEVID_0));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xff, joy_read_byte(JOY_DEVID_1), "the keypad joystick direction held on opening leaks into the machine");
    for (int i = 0; i < 2; i++) ov_frame();

    /* tab Sound se zapnutým poslechem: emulace běží, joystick zůstává v klidu */
    ov_key(ImGuiKey_4);
    TEST_ASSERT_EQUAL_INT(OVERLAY_TAB_SOUND, imgui_overlay_get_tab());
    TEST_ASSERT_TRUE(g_emulator.paused);
    ov_click_item("snd_listen");
    TEST_ASSERT_FALSE_MESSAGE(g_emulator.paused, "Listen resumes the emulation");
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE(joy_host_is_held());
    TEST_ASSERT_TRUE(imgui_overlay_blocks_joystick());
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xff, joy_read_byte(JOY_DEVID_0), "the host joystick leaks into the machine while listening");
    TEST_ASSERT_EQUAL_HEX8(0xff, joy_read_byte(JOY_DEVID_1));

    /* zavření s drženým tlačítkem: zadržení trvá */
    imgui_overlay_close();
    for (int i = 0; i < 3; i++) ov_frame();
    TEST_ASSERT_TRUE_MESSAGE(joy_host_is_held(), "a button held while closing leaks into the machine");
    TEST_ASSERT_TRUE(imgui_overlay_blocks_joystick());
    TEST_ASSERT_EQUAL_HEX8(0xff, joy_read_byte(JOY_DEVID_0));

    /* uvolnění: joystick zpět u stroje */
    s_fake_joy = 0xff;
    ov_frame();
    TEST_ASSERT_FALSE(joy_host_is_held());
    TEST_ASSERT_FALSE(imgui_overlay_blocks_joystick());
    s_fake_joy = pressed;
    TEST_ASSERT_EQUAL_HEX8(pressed, joy_read_byte(JOY_DEVID_0));
    for (int i = 0; i < 2; i++) ov_frame();
    TEST_ASSERT_FALSE_MESSAGE(joy_host_is_held(), "a later press must not hold the joystick again");

    /* zavření s joystickem v klidu uvolní hned v prvním snímku */
    s_fake_joy = 0xff;
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    imgui_overlay_open();
    TEST_ASSERT_TRUE(joy_host_is_held());
    ov_frame();
    imgui_overlay_close();
    ov_frame();
    TEST_ASSERT_FALSE(joy_host_is_held());

    g_joy.host_scan = saved_scan;
    g_joy.dev[JOY_DEVID_0].type = saved_type0;
    g_joy.dev[JOY_DEVID_1].type = saved_type1;
    g_joy.dev[JOY_DEVID_0].state = 0xff;
    g_joy.dev[JOY_DEVID_1].state = 0xff;
    s_fake_joy = 0xff;
}

/**
 * Kopie stavu tabů Sound, Input a Capture nad skutečným jádrem: hlavní
 * hlasitost, ztlumení, propust a hlasitosti kanálů se čtou ze zvukového
 * rozhraní, počet kanálů odpovídá stroji, zařízení joysticku portu a stav
 * záznamů se čtou z jádra; testovací stavy splňují invarianty.
 */
void test_o6_view_fill(void)
{
    static st_OVERLAY_VIEW v;
    const float saved_master = iface_audio_get_master_gain();
    const bool saved_mute = iface_audio_get_mute();
    const bool saved_filter = iface_audio_output_filter_enabled();
    float saved_gain[AUDIO_SRC_CHANNELS_COUNT];
    memcpy(saved_gain, g_iface_audio.gain, sizeof(saved_gain));
    const en_JOY_TYPE saved_joy = g_joy.dev[JOY_DEVID_0].type;

    iface_audio_set_master_gain(0.6f);
    iface_audio_set_mute(true);
    iface_audio_set_output_filter(false);
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = 0.1f * (float)(i + 1);
    g_joy.dev[JOY_DEVID_0].type = JOY_TYPE_NUM_KEYPAD;
    media_fill(&v);
    TEST_ASSERT_TRUE(v.sound.master_gain == 0.6f);
    TEST_ASSERT_TRUE(v.sound.mute);
    TEST_ASSERT_FALSE(v.sound.output_filter);
    TEST_ASSERT_EQUAL_INT(AUDIO_HAVE_OUTPUT_FILTER != 0, v.sound.has_output_filter);
    TEST_ASSERT_TRUE(v.sound.channel_count == 1 || v.sound.channel_count == 5 || v.sound.channel_count == 9);
    TEST_ASSERT_TRUE(v.sound.channel_count <= AUDIO_SRC_CHANNELS_COUNT);
#if HAVE_PSG == 2
    TEST_ASSERT_EQUAL_UINT(g_psg_module.stereo ? 9 : 5, v.sound.channel_count);
    TEST_ASSERT_EQUAL_INT(g_psg_module.stereo, v.sound.stereo_names);
#else
    TEST_ASSERT_EQUAL_UINT(AUDIO_SRC_CHANNELS_COUNT, v.sound.channel_count);
    TEST_ASSERT_FALSE(v.sound.stereo_names);
#endif
    for (unsigned i = 0; i < v.sound.channel_count; i++) TEST_ASSERT_TRUE(v.sound.channel_gain[i] == g_iface_audio.gain[i]);
    TEST_ASSERT_EQUAL_INT((int)OVERLAY_JOY_NUM_KEYPAD, v.input.joy_type);
    TEST_ASSERT_EQUAL_INT(g_iface_joy.available_count > 0, v.input.joy_available);

    /* tab Capture: nic neběží; čekající start nahrávání přepne popisek na Stop */
    TEST_ASSERT_EQUAL_INT((int)OVERLAY_REPLAY_OFF, v.capture.replay_mode);
    TEST_ASSERT_FALSE(v.capture.series_active);
    TEST_ASSERT_FALSE(v.capture.video_stop);
    TEST_ASSERT_FALSE(v.capture.video_pause_enabled);
    TEST_ASSERT_FALSE(v.capture.video_paused);
    s_video_start_pending = true;
    media_fill(&v);
    TEST_ASSERT_TRUE(v.capture.video_stop);
    TEST_ASSERT_FALSE_MESSAGE(v.capture.video_pause_enabled, "a pending start cannot be paused yet");
    s_video_start_pending = false;

    iface_audio_set_master_gain(saved_master);
    iface_audio_set_mute(saved_mute);
    iface_audio_set_output_filter(saved_filter);
    memcpy(g_iface_audio.gain, saved_gain, sizeof(saved_gain));
    g_joy.dev[JOY_DEVID_0].type = saved_joy;

    /* testovací stavy: invarianty kopie */
    for (int f = 0; f < (int)OVERLAY_FIXTURE_COUNT; f++) {
        overlay_view_make_fixture(&v, (en_OVERLAY_FIXTURE)f);
        TEST_ASSERT_TRUE(v.sound.master_gain >= 0.0f && v.sound.master_gain <= OVERLAY_VIEW_MASTER_GAIN_MAX);
        TEST_ASSERT_TRUE(v.sound.channel_count >= 1 && v.sound.channel_count <= OVERLAY_VIEW_AUDIO_CHANNELS_MAX);
        TEST_ASSERT_TRUE(v.input.joy_type >= 0 && v.input.joy_type < (int)OVERLAY_JOY_TYPE_COUNT);
        TEST_ASSERT_TRUE(!v.capture.video_paused || v.capture.video_pause_enabled);
        TEST_ASSERT_TRUE(g_utf8_validate(v.capture.replay_text, -1, NULL));
    }
    overlay_view_reset(&v);
    TEST_ASSERT_TRUE(v.sound.master_gain == 1.0f);
    TEST_ASSERT_FALSE(v.sound.mute);
}

/**
 * Tab Sound: každý prvek zavolá funkci zvukového rozhraní a stav bere z
 * kopie. Master volume a Mute mění jen hlavní hlasitost a ztlumení
 * (hlasitosti kanálů zůstávají), posuvník kanálu zapisuje g_iface_audio.gain[]
 * jako okno Audio Mixer, Reset channels vrací všechny kanály na 100 %.
 */
void test_sound_controls(void)
{
    const float saved_master = iface_audio_get_master_gain();
    const bool saved_mute = iface_audio_get_mute();
    const bool saved_filter = iface_audio_output_filter_enabled();
    float saved_gain[AUDIO_SRC_CHANNELS_COUNT];
    memcpy(saved_gain, g_iface_audio.gain, sizeof(saved_gain));
    iface_audio_set_master_gain(1.0f);
    iface_audio_set_mute(false);
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = 0.5f;

    imgui_overlay_open();
    form_start(OVERLAY_TAB_SOUND, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    /* kanály kopie nesmí přesáhnout kanály stroje, pro který je test přeložený */
    if (s_fixture_view.sound.channel_count > AUDIO_SRC_CHANNELS_COUNT) s_fixture_view.sound.channel_count = AUDIO_SRC_CHANNELS_COUNT;
    for (int i = 0; i < 2; i++) ov_frame();

    /* Master volume: klik do drážky nastaví hlavní hlasitost, kanály se nemění */
    media_click_at("snd_master", 0.25f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_MASTER_VOLUME, overlay_actions_test_take_last());
    const int pct = overlay_actions_test_last_value();
    TEST_ASSERT_TRUE_MESSAGE(pct > 0 && pct < 80, "a click left of the knob lowers the volume");
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, (float)pct / 100.0f, iface_audio_get_master_gain());
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) TEST_ASSERT_TRUE_MESSAGE(g_iface_audio.gain[i] == 0.5f, "the master volume does not rewrite the channel gains");
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* Mute: přepne ztlumení proti stavu v kopii; hlavní hlasitost zůstává */
    media_click("snd_mute");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_MUTE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(iface_audio_get_mute());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, (float)pct / 100.0f, iface_audio_get_master_gain());
    s_fixture_view.sound.mute = true;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click("snd_mute");
    TEST_ASSERT_FALSE(iface_audio_get_mute());
    s_fixture_view.sound.mute = false;

    /* Hardware output filter: kopie říká zapnuto, klik vypne (na stroji, který propust má) */
    iface_audio_set_output_filter(true);
    media_click("snd_filter");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_OUTPUT_FILTER, overlay_actions_test_take_last());
#if AUDIO_HAVE_OUTPUT_FILTER
    TEST_ASSERT_FALSE(iface_audio_output_filter_enabled());
#endif

    /* posuvník kanálu: zapisuje hlasitost právě toho kanálu */
    const unsigned last = s_fixture_view.sound.channel_count - 1;
    char name[24];
    snprintf(name, sizeof(name), "snd_ch%u", last);
    media_click_at(name, 0.2f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CHANNEL_VOLUME, overlay_actions_test_take_last());
    const int value = overlay_actions_test_last_value();
    TEST_ASSERT_EQUAL_INT((int)last, value / 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, (float)(value % 1000) / 100.0f, g_iface_audio.gain[last]);
    TEST_ASSERT_TRUE(g_iface_audio.gain[last] != 0.5f);
    for (unsigned i = 0; i < last; i++) TEST_ASSERT_TRUE(g_iface_audio.gain[i] == 0.5f);

    /* Reset channels: všechny kanály na 100 %, hlavní hlasitost zůstává */
    media_click("snd_reset");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_CHANNELS_RESET, overlay_actions_test_take_last());
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) TEST_ASSERT_TRUE(g_iface_audio.gain[i] == 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, (float)pct / 100.0f, iface_audio_get_master_gain());

    /* klávesnice: výchozí fokus má Master volume, šipka vpravo přidá procento */
    imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
    imgui_overlay_set_tab(OVERLAY_TAB_SOUND);
    for (int i = 0; i < 4; i++) ov_frame();
    media_drain();
    ov_key(ImGuiKey_RightArrow);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_MASTER_VOLUME, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(81, overlay_actions_test_last_value());

    imgui_overlay_close();
    iface_audio_set_master_gain(saved_master);
    iface_audio_set_mute(saved_mute);
    iface_audio_set_output_filter(saved_filter);
    memcpy(g_iface_audio.gain, saved_gain, sizeof(saved_gain));
}

/**
 * Tab Input: Virtual keyboard, Autotype buffer a Joystick Setup... ukážou
 * své okno a overlay zavřou (K11), Alt+K dělá totéž co tlačítko; volba
 * Device zařadí změnu typu prvního portu, "Joystick" bez zařízení
 * hostitele nejde zvolit a při replay je volba zakázaná celá.
 */
void test_input_controls(void)
{
    const char *name;
    st_DEVICE_REQ_ARGS args;
    const en_JOY_TYPE saved_joy = g_joy.dev[JOY_DEVID_0].type;

    imgui_overlay_open();
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    o6_drain();
    media_click("inp_vkbd");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VKBD, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showVirtualKeyboardWindow);
    TEST_ASSERT_FALSE_MESSAGE(imgui_overlay_is_open(), "a tool window closes the overlay");

    imgui_overlay_open();
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    ov_mod_key(ImGuiKey_K, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VKBD, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showVirtualKeyboardWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    imgui_overlay_open();
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    media_click("inp_autotype");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_AUTOTYPE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showAutotypeWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    imgui_overlay_open();
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    media_click("inp_joy_setup");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_SETUP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showJoystickSetupWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* Device: kopie říká Numeric keypad; klik na zvolený segment nic, klik na None zařadí změnu typu portu 1 */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    media_click_at("inp_joy_device", 0.5f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "the selected segment does nothing");
    media_click_at("inp_joy_device", 0.1f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT((int)OVERLAY_JOY_NONE, overlay_actions_test_last_value());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_INT((int)JOY_DEVID_0, args.i[0]);
    TEST_ASSERT_EQUAL_INT((int)JOY_TYPE_NONE, args.i[1]);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    /* Joystick: kopie tvrdí, že joystick je; hostitel mezitím žádný nemá, zásah se proto nezařadí */
    memset(&g_iface_joy, 0, sizeof(g_iface_joy));
    s_joy_open_calls = s_joy_close_calls = s_joy_params_calls = 0;
    media_click_at("inp_joy_device", 0.9f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT((int)OVERLAY_JOY_JOYSTICK, overlay_actions_test_last_value());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, s_joy_open_calls);
    /* s joystickem hostitele: změna typu a otevření prvního dostupného zařízení s mapováním, jako Apply okna Joystick Setup */
    g_iface_joy.available_count = 1;
    media_click_at("inp_joy_device", 0.9f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_INT((int)JOY_TYPE_JOYSTICK, args.i[1]);
    TEST_ASSERT_EQUAL_INT(1, s_joy_open_calls);
    TEST_ASSERT_EQUAL_INT(1, s_joy_params_calls);
    TEST_ASSERT_EQUAL_INT(0, s_joy_close_calls);
    TEST_ASSERT_EQUAL_INT(0, overlay_actions_test_take_joy_open_failures());
    /* review O6 Z6: nepovedené otevření zařízení se oznámí */
    s_joy_open_result = EXIT_FAILURE;
    overlay_action_request_value(OVL_ACTION_JOY_DEVICE, (int)OVERLAY_JOY_JOYSTICK);
    ov_frame();
    s_joy_open_result = EXIT_SUCCESS;
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, overlay_actions_test_take_joy_open_failures(), "a failed joystick open is reported");
    device_request_test_take_pending(NULL, NULL);
    s_joy_open_calls = 1;
    s_joy_params_calls = 1;
    /* odchod z volby Joystick zařízení zavře */
    s_fixture_view.input.joy_type = (int)OVERLAY_JOY_JOYSTICK;
    for (int i = 0; i < 2; i++) ov_frame();
    media_click_at("inp_joy_device", 0.1f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(device_request_test_take_pending(&name, &args));
    TEST_ASSERT_EQUAL_INT((int)JOY_TYPE_NONE, args.i[1]);
    TEST_ASSERT_EQUAL_INT(1, s_joy_close_calls);
    memset(&g_iface_joy, 0, sizeof(g_iface_joy));
    device_request_clear();

    /* bez joysticku hostitele je segment Joystick zakázaný */
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_EMPTY, 1280.0f, 720.0f);
    media_drain();
    media_click_at("inp_joy_device", 0.9f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OVL_ACTION_NONE, overlay_actions_test_take_last(), "no joystick, no choice");
    media_click_at("inp_joy_device", 0.5f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_JOY_DEVICE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT((int)OVERLAY_JOY_NUM_KEYPAD, overlay_actions_test_last_value());
    device_request_clear();

    /* záznam nebo přehrávání vstupů: volba je zakázaná celá s důvodem */
    form_start(OVERLAY_TAB_INPUT, OVERLAY_FIXTURE_REPLAY, 1280.0f, 720.0f);
    media_drain();
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_JOY_DEVICE, &s_fixture_view));
    media_click_at("inp_joy_device", 0.1f);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_FALSE(device_request_test_take_pending(NULL, NULL));

    imgui_overlay_close();
    g_joy.dev[JOY_DEVID_0].type = saved_joy;
}

/**
 * Tab Capture: každé tlačítko zavolá funkci klasického menu, stav bere z
 * kopie a dostupnost odpovídá klasickému menu (Pause Recording jen při
 * nahrávání, Record Inputs... a Play Replay... jen při vypnutém replay,
 * Stop Replay jen při běžícím). Okna nastavení a Replay Control overlay
 * zavřou; dialogy replay se otevírají po zavření overlaye. Zkratky dělají
 * totéž co prvky.
 */
void test_capture_controls(void)
{
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_drain();
    o6_drain();

    media_click("cap_shot");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_screenshot_calls);
    media_click("cap_shot_as");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT_AS, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_shot_as_calls);
    media_click("cap_series");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SERIES, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_series_calls);
    media_click("cap_shot_folder");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT_FOLDER, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_shot_folder_calls);
    TEST_ASSERT_TRUE_MESSAGE(imgui_overlay_is_open(), "screenshot actions keep the overlay open");

    media_click("cap_video");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_TOGGLE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_toggle_calls);
    /* Pause Recording: bez nahrávání zakázané jako v klasickém menu */
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_VIDEO_PAUSE, &s_fixture_view));
    TEST_ASSERT_NULL(overlay_action_keycap(OVL_ACTION_VIDEO_PAUSE, &s_fixture_view));
    media_click("cap_video_pause");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, s_video_pause_calls);
    ov_mod_key(ImGuiKey_O, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT(0, s_video_pause_calls);
    media_click("cap_realtime");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_REALTIME, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_timebase_calls);
    media_click("cap_video_folder");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_FOLDER, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_folder_calls);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* Stop Replay: bez replay zakázané */
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_REPLAY_STOP, &s_fixture_view));
    media_click("cap_replay_stop");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_NONE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(0, s_replay_request_calls);

    /* zkratky: Alt+Shift+F, Alt+O, Alt+U */
    o6_drain();
    ov_mod_key(ImGuiKey_F, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SERIES, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_series_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_screenshot_calls - 1, "Alt+Shift+F is not Alt+F");
    ov_mod_key(ImGuiKey_O, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_TOGGLE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_toggle_calls);
    ov_mod_key(ImGuiKey_U, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_REALTIME, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_timebase_calls);

    /* okna nastavení a Replay Control overlay zavřou (K11) */
    media_click("cap_shot_setup");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_SCREENSHOT_SETUP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showScreenshotSetupWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_click("cap_video_setup");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_SETUP, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showVideorecSetupWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    media_click("cap_replay_window");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_REPLAY_WINDOW, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showReplayWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    ov_mod_key(ImGuiKey_J, ImGuiMod_Alt);
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_REPLAY_WINDOW, overlay_actions_test_take_last());
    TEST_ASSERT_TRUE(s_gui.showReplayWindow);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* Record Inputs... a Play Replay...: overlay se zavře dřív, než se dialog vyžádá */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    media_click("cap_replay_record");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_REPLAY_RECORD, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_replay_record_calls);
    TEST_ASSERT_FALSE_MESSAGE(s_replay_dialog_overlay_open, "the overlay closes before the record dialog");
    TEST_ASSERT_FALSE(imgui_overlay_is_open());
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_DEFAULT, 1280.0f, 720.0f);
    o6_drain();
    media_click("cap_replay_play");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_REPLAY_PLAY, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_replay_play_calls);
    TEST_ASSERT_FALSE(s_replay_dialog_overlay_open);
    TEST_ASSERT_FALSE(imgui_overlay_is_open());

    /* běžící replay: Record a Play zakázané, Stop povolené a hlásí výsledek jako klasické menu */
    imgui_overlay_open();
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_REPLAY, 1280.0f, 720.0f);
    media_drain();
    o6_drain();
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_REPLAY_RECORD, &s_fixture_view));
    TEST_ASSERT_NOT_NULL(overlay_action_reason(OVL_ACTION_REPLAY_PLAY, &s_fixture_view));
    media_click("cap_replay_record");
    media_click("cap_replay_play");
    TEST_ASSERT_EQUAL_INT(0, s_replay_record_calls + s_replay_play_calls);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());
    media_click("cap_replay_stop");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_REPLAY_STOP, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_replay_request_calls);
    TEST_ASSERT_TRUE(imgui_overlay_is_open());

    /* nahrává se (pozastaveno): tlačítko znamená Stop, Pause Recording je dostupné i zkratkou */
    form_start(OVERLAY_TAB_CAPTURE, OVERLAY_FIXTURE_LONG_TEXTS, 1280.0f, 720.0f);
    media_drain();
    o6_drain();
    TEST_ASSERT_NULL(overlay_action_reason(OVL_ACTION_VIDEO_PAUSE, &s_fixture_view));
    media_click("cap_video_pause");
    TEST_ASSERT_EQUAL_INT(OVL_ACTION_VIDEO_PAUSE, overlay_actions_test_take_last());
    TEST_ASSERT_EQUAL_INT(1, s_video_pause_calls);
    ov_mod_key(ImGuiKey_O, ImGuiMod_Alt | ImGuiMod_Shift);
    TEST_ASSERT_EQUAL_INT(2, s_video_pause_calls);
    media_click("cap_video");
    TEST_ASSERT_EQUAL_INT(1, s_video_toggle_calls);
    imgui_overlay_close();
}

/**
 * @brief Snímky tabů Sound, Input a Capture (prefix o6) z testovacího stavu DEFAULT a tabu Sound ve stavech přepínače Listen.
 * @param dir Adresář snímků. @param prefix Začátek jména souboru. @param lang Kód jazyka pro jméno souboru.
 * @return Počet snímků, které se nepodařilo uložit.
 */
static int shots_run_o6(const char *dir, const char *prefix, const char *lang)
{
    static st_OVERLAY_VIEW v;
    static const en_OVERLAY_TAB tabs[] = {OVERLAY_TAB_SOUND, OVERLAY_TAB_INPUT, OVERLAY_TAB_CAPTURE};
    static const char *const names[] = {"sound", "input", "capture"};
    int failed = 0;
    char file[160];
    overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(-1.0f, -1.0f);
    for (int t = 0; t < 3; t++) {
        /* přepnutí tabu tam a zpět: tab dá fokus svému výchozímu prvku jako po otevření */
        imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
        imgui_overlay_set_tab(tabs[t]);
        shot_frames(&v, 4);
        snprintf(file, sizeof(file), "%s-%s-%dx%d-%s.png", prefix, lang, s_canvas_w, s_canvas_h, names[t]);
        if (!shot_render_save(dir, file)) failed++;
    }
    /* tab Sound v dalších stavech přepínače Listen (stav DEFAULT výše je vypnutý poslech) */
    static const int listen[] = {OVERLAY_LISTEN_ON, OVERLAY_LISTEN_BLOCKED_PAUSED, OVERLAY_LISTEN_BLOCKED_RUNNING};
    static const char *const listen_names[] = {"sound-listen-on", "sound-listen-blocked-paused", "sound-listen-blocked-running"};
    for (int i = 0; i < 3; i++) {
        overlay_view_make_fixture(&v, OVERLAY_FIXTURE_DEFAULT);
        v.sound.listen = listen[i];
        v.paused = (listen[i] == OVERLAY_LISTEN_BLOCKED_PAUSED);
        imgui_overlay_set_tab(OVERLAY_TAB_MEDIA);
        imgui_overlay_set_tab(OVERLAY_TAB_SOUND);
        shot_frames(&v, 4);
        snprintf(file, sizeof(file), "%s-%s-%dx%d-%s.png", prefix, lang, s_canvas_w, s_canvas_h, listen_names[i]);
        if (!shot_render_save(dir, file)) failed++;
    }
    return failed;
}

/**
 * @brief Režim snímků: nastaví jazyk a plátno, vytvoří framebuffer a uloží snímky.
 * @param dir Adresář snímků (MZ_OVERLAY_SHOT_DIR).
 * @return Návratový kód procesu (0 = vše uloženo).
 */
static int shots_main(const char *dir)
{
    const char *ev;
    if ((ev = getenv("MZ_OVERLAY_SHOT_W")) && atoi(ev) > 0) s_canvas_w = atoi(ev);
    if ((ev = getenv("MZ_OVERLAY_SHOT_H")) && atoi(ev) > 0) s_canvas_h = atoi(ev);
    const char *lang = getenv("MZ_OVERLAY_SHOT_LANG");
    /* jazyk je součást jména souboru: oddělovač cesty ani ".." nesmí zapsat mimo adresář */
    if (lang && (strchr(lang, '/') || strchr(lang, '\\') || strstr(lang, ".."))) {
        fprintf(stderr, "MZ_OVERLAY_SHOT_LANG must not contain '/', '\\' or '..'\n");
        return 1;
    }
    const char *prefix = getenv("MZ_OVERLAY_SHOT_PREFIX");
    if (!prefix || !*prefix) prefix = "o2";
    if (strchr(prefix, '/') || strchr(prefix, '\\') || strstr(prefix, "..")) {
        fprintf(stderr, "MZ_OVERLAY_SHOT_PREFIX must not contain '/', '\\' or '..'\n");
        return 1;
    }
    if (lang && *lang && MZTEST_LOCALE_DIR[0]) {
        static char lang_env[64];
        snprintf(lang_env, sizeof(lang_env), "LANGUAGE=%s", lang);
        putenv(lang_env);
        setlocale(LC_ALL, "");
        bindtextdomain(I18N_TEXTDOMAIN, MZTEST_LOCALE_DIR);
        bind_textdomain_codeset(I18N_TEXTDOMAIN, "UTF-8");
        textdomain(I18N_TEXTDOMAIN);
    }
    s_ui_ok = ui_setup();
    if (!s_ui_ok) {
        fprintf(stderr, "SDL/GL not available\n");
        return 1;
    }
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
        if (check_fb(TEST_GL_FRAMEBUFFER) != TEST_GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO incomplete\n");
            s_fbo = 0;
        }
    } else {
        fprintf(stderr, "FBO functions not available, reading the back buffer\n");
        s_bind_fb = NULL;
    }
    const char *set = getenv("MZ_OVERLAY_SHOT_SET");
    int failed;
    if (set && strcmp(set, "media") == 0) failed = shots_run_media(dir, prefix, (lang && *lang) ? lang : "en");
    else if (set && strcmp(set, "sheet") == 0) failed = shots_run_sheet(dir, prefix, (lang && *lang) ? lang : "en");
    else if (set && strcmp(set, "o5") == 0) failed = shots_run_o5(dir, prefix, (lang && *lang) ? lang : "en");
    else if (set && strcmp(set, "o6") == 0) failed = shots_run_o6(dir, prefix, (lang && *lang) ? lang : "en");
    else failed = shots_run(dir, prefix, (lang && *lang) ? lang : "en");
    ui_teardown();
    return failed ? 1 : 0;
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    const char *shot_dir = getenv("MZ_OVERLAY_SHOT_DIR");
    if (shot_dir && *shot_dir) {
        int rc = shots_main(shot_dir);
        mztest_teardown();
        return rc;
    }

    /* druhá registrace v CTest: jen geometrie v daném jazyce */
    const char *test_lang = getenv("MZ_OVERLAY_TEST_LANG");
    const bool lang_run = test_lang && *test_lang && MZTEST_LOCALE_DIR[0];
    if (lang_run) {
        static char lang_env[64];
        snprintf(lang_env, sizeof(lang_env), "LANGUAGE=%s", test_lang);
        putenv(lang_env);
        setlocale(LC_ALL, "");
        bindtextdomain(I18N_TEXTDOMAIN, MZTEST_LOCALE_DIR);
        bind_textdomain_codeset(I18N_TEXTDOMAIN, "UTF-8");
        textdomain(I18N_TEXTDOMAIN);
    }

    /* kreslení potřebuje SDL + GL; bez nich proběhnou jen testy bez kreslení a ctest test ukáže jako přeskočený */
    s_ui_ok = ui_setup();

    if (lang_run) {
        UNITY_BEGIN();
        if (s_ui_ok) RUN_TEST(test_frame_geometry);
        if (s_ui_ok) RUN_TEST(test_media_geometry);
        if (s_ui_ok) RUN_TEST(test_sheet_geometry);
        if (s_ui_ok) RUN_TEST(test_machine_display_geometry);
        if (s_ui_ok) RUN_TEST(test_dropdown_geometry);
        if (s_ui_ok) RUN_TEST(test_o6_geometry);
        int lang_result = UNITY_END();
        if (s_ui_ok) ui_teardown();
        mztest_teardown();
        return (!s_ui_ok && lang_result == 0) ? 77 : lang_result;
    }

    UNITY_BEGIN();
    RUN_TEST(test_view_fill_and_fixture);
    RUN_TEST(test_open_close_and_tab_api);
    RUN_TEST(test_pause_restore);
    if (s_ui_ok) RUN_TEST(test_draw_three_frames);
    if (s_ui_ok) RUN_TEST(test_keys);
    if (s_ui_ok) RUN_TEST(test_open_rules);
    if (s_ui_ok) RUN_TEST(test_mouse);
    if (s_ui_ok) RUN_TEST(test_emulated_keyboard);
    if (s_ui_ok) RUN_TEST(test_nav_keyboard_probe);
    RUN_TEST(test_style_unit);
    RUN_TEST(test_style_contrast);
    if (s_ui_ok) RUN_TEST(test_style_fonts);
    if (s_ui_ok) RUN_TEST(test_font_sizes_match_tokens);
    if (s_ui_ok) RUN_TEST(test_frame_geometry);
    if (s_ui_ok) RUN_TEST(test_widgets_geometry);
    if (s_ui_ok) RUN_TEST(test_button_mouse_and_keys);
    if (s_ui_ok) RUN_TEST(test_toggle_mouse_and_keys);
    if (s_ui_ok) RUN_TEST(test_segmented_mouse_and_keys);
    if (s_ui_ok) RUN_TEST(test_slider_mouse_and_keys);
    if (s_ui_ok) RUN_TEST(test_text_ellipsis);
    RUN_TEST(test_pause_failures);
    if (s_ui_ok) RUN_TEST(test_pause_changed_by_others);
    if (s_ui_ok) RUN_TEST(test_sound_listen_toggle);
    if (s_ui_ok) RUN_TEST(test_open_request_from_classic_menu);
    if (s_ui_ok) RUN_TEST(test_overlay_holds_joystick);
    RUN_TEST(test_media_view_fill);
    RUN_TEST(test_action_table);
    if (s_ui_ok) RUN_TEST(test_media_geometry);
    if (s_ui_ok) RUN_TEST(test_card_title_clip);
    if (s_ui_ok) RUN_TEST(test_media_buttons);
    if (s_ui_ok) RUN_TEST(test_media_closing_actions);
    if (s_ui_ok) RUN_TEST(test_media_shortcuts);
    if (s_ui_ok) RUN_TEST(test_media_focus);
    if (s_ui_ok) RUN_TEST(test_media_external_eject);
    if (s_ui_ok) RUN_TEST(test_media_shared_image);
    RUN_TEST(test_recent_view_fill);
    if (s_ui_ok) RUN_TEST(test_sheet_geometry);
    if (s_ui_ok) RUN_TEST(test_sheet_keys);
    if (s_ui_ok) RUN_TEST(test_sheet_keys_layout);
    if (s_ui_ok) RUN_TEST(test_sheet_controls);
    if (s_ui_ok) RUN_TEST(test_sheet_missing_file);
    if (s_ui_ok) RUN_TEST(test_recent_strip);
    if (s_ui_ok) RUN_TEST(test_recent_probe_never_blocks_ui);
    if (s_ui_ok) RUN_TEST(test_recent_probe_per_path);
    if (s_ui_ok) RUN_TEST(test_recent_clear);
    if (s_ui_ok) RUN_TEST(test_sheet_modal_above);
    if (s_ui_ok) RUN_TEST(test_toast_above_overlay);
    RUN_TEST(test_machine_display_view_fill);
    if (s_ui_ok) RUN_TEST(test_machine_display_geometry);
    if (s_ui_ok) RUN_TEST(test_machine_controls);
    if (s_ui_ok) RUN_TEST(test_display_controls);
    if (s_ui_ok) RUN_TEST(test_scrollbar_and_keycap_groups);
    if (s_ui_ok) RUN_TEST(test_machine_display_keyboard);
    if (s_ui_ok) RUN_TEST(test_o6_geometry);
    RUN_TEST(test_o6_view_fill);
    if (s_ui_ok) RUN_TEST(test_sound_controls);
    if (s_ui_ok) RUN_TEST(test_input_controls);
    if (s_ui_ok) RUN_TEST(test_capture_controls);
    if (s_ui_ok) RUN_TEST(test_dropdown_geometry);
    /* poslední: mění písma v atlasu */
    if (s_ui_ok) RUN_TEST(test_shared_font_matches_file_font);
    if (s_ui_ok) RUN_TEST(test_review_widget_fixes);
    if (s_ui_ok) RUN_TEST(test_fonts_production_order);
    if (s_ui_ok) RUN_TEST(test_missing_regular_font);
    if (s_ui_ok) RUN_TEST(test_missing_fonts_fallback);
    int result = UNITY_END();
    if (s_tmpdir) {
        if (s_dsk) g_remove(s_dsk);
        for (guint i = 0; s_tmp_files && i < s_tmp_files->len; i++) g_remove((const char *)g_ptr_array_index(s_tmp_files, i));
        g_rmdir(s_tmpdir);
    }

    if (s_ui_ok) ui_teardown();
    mztest_teardown();
    if (!s_ui_ok && result == 0) {
        fprintf(stderr, "ui-overlay: SDL/GL not available, overlay drawing skipped\n");
        return 77;
    }
    return result;
}
