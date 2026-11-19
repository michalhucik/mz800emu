#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "emulator/replay.h"
#include "topmenu.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/cmt/imgui_cmt.h"

extern "C"
{
    void imgui_menu_snapshot(void);
} // extern "C"

/* Stav pro řízení save/load procesu */
static bool s_snapshot_save_requested = false;
static bool s_snapshot_load_requested = false;
static bool s_snapshot_quicksave_requested = false;
static bool s_snapshot_quickload_requested = false;
static bool s_was_running_before_pause = false;

/* Přístup k flagům z jiných modulů */
extern "C" bool snapshot_ui_is_save_requested(void) { return s_snapshot_save_requested; }
extern "C" bool snapshot_ui_is_load_requested(void) { return s_snapshot_load_requested; }
extern "C" bool snapshot_ui_is_quicksave_requested(void) { return s_snapshot_quicksave_requested; }
extern "C" bool snapshot_ui_is_quickload_requested(void) { return s_snapshot_quickload_requested; }

extern "C" void snapshot_ui_clear_save_request(void) { s_snapshot_save_requested = false; }
extern "C" void snapshot_ui_clear_load_request(void) { s_snapshot_load_requested = false; }
extern "C" void snapshot_ui_clear_quicksave_request(void) { s_snapshot_quicksave_requested = false; }
extern "C" void snapshot_ui_clear_quickload_request(void) { s_snapshot_quickload_requested = false; }

extern "C" bool snapshot_ui_was_running_before_pause(void) { return s_was_running_before_pause; }

/**
 * @brief Po vyřízení právě vyžádaného uložení snapshotu má emulace běžet.
 *
 * Pro volajícího, který emulaci zastavil sám ještě před žádostí o uložení
 * (overlay menu: žádost pak vidí emulaci stát a běh by po sobě neobnovila)
 * a mezitím se rozhodl, že má emulace běžet (zavření overlaye). Přepíše
 * zapamatovaný stav "před pauzou běžela": běh obnoví dokončení uložení
 * (snapshot_ui_handle_result()) nebo zrušení dialogu Save Snapshot.
 *
 * @pre UI vlákno; žádost o Quick Save nebo Save Snapshot čeká nebo se vyřizuje.
 * @post snapshot_ui_was_running_before_pause() vrací true do příští žádosti.
 */
extern "C" void snapshot_ui_run_after_request(void) { s_was_running_before_pause = true; }

/* Pomocná funkce pro pause + nastavení requestu */
static void snapshot_pause_and_request(bool *request_flag)
{
    s_was_running_before_pause = !EMULATOR_TEST_PAUSED;
    if (s_was_running_before_pause)
        dbg_ui_pause();
    *request_flag = true;
}

/* Veřejné funkce pro spouštění snapshot akcí (z global_shortcuts a menu).
 * Načtení snapshotu za záznamu / přehrávání vstupů nejde: záznam by ho
 * nezopakoval (a načtení jde přímo z UI vlákna, mimo bránu záznamu).
 * Načtení pásku v kazetofonu nahradí: Load a Quick Load jdou přes dotaz
 * Virtual CMT (probíhající nahrávání, neuložené změny pásky), ten po
 * odsouhlasení zavolá variantu "_now". */

/**
 * @brief Load Snapshot (menu, klávesová zkratka): přes dotaz Virtual CMT.
 *
 * Při záznamu nebo přehrávání vstupů nic. Jinak zahájí automat dotazu
 * (imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD)): bez
 * nahrávání a neuložených změn pásky, nebo po odsouhlasení, zavolá
 * snapshot_ui_request_load_now() (pauza a dialog souboru), a to vždy až
 * mimo device_state_lock() (pauza synchronně čeká na emu vlákno).
 * Vybraný soubor jde zpět do automatu (SNAPSHOT_LOAD_FILE).
 *
 * @pre UI vlákno.
 */
extern "C" void snapshot_ui_request_load(void)
{
    if (!replay_is_active()) imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_SNAPSHOT_LOAD);
}

/** @brief Load Snapshot po dotazu Virtual CMT: pauza a dialog souboru (bez dalšího dotazu). */
extern "C" void snapshot_ui_request_load_now(void)
{
    if (!replay_is_active()) snapshot_pause_and_request(&s_snapshot_load_requested);
}
extern "C" void snapshot_ui_request_save(void) { snapshot_pause_and_request(&s_snapshot_save_requested); }
/**
 * @brief Quick Load (menu, klávesová zkratka): přes dotaz Virtual CMT.
 *
 * Při záznamu nebo přehrávání vstupů nic. Jinak zahájí automat dotazu
 * (IMGUI_CMT_TAPE_ACTION_QUICK_LOAD), který po odsouhlasení (nebo bez
 * čeho by se ptal) zavolá snapshot_ui_request_quickload_now() mimo
 * device_state_lock().
 *
 * @pre UI vlákno.
 */
extern "C" void snapshot_ui_request_quickload(void)
{
    if (!replay_is_active()) imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_QUICK_LOAD);
}

/** @brief Quick Load po dotazu Virtual CMT: pauza a požadavek (bez dalšího dotazu). */
extern "C" void snapshot_ui_request_quickload_now(void)
{
    if (!replay_is_active()) snapshot_pause_and_request(&s_snapshot_quickload_requested);
}
extern "C" void snapshot_ui_request_quicksave(void) { snapshot_pause_and_request(&s_snapshot_quicksave_requested); }


void imgui_menu_snapshot(void)
{
    if (ImGui::BeginMenu(_L("Snapshot")))
    {
        bool replay_active = replay_is_active();
        if (ImGui::MenuItem(_L("Load Snapshot..."), "Alt+F7", false, !replay_active))
        {
            snapshot_ui_request_load();
        }
        if (replay_active)
        {
            ImGui::SetItemTooltip("%s", _("Not available while a replay is recorded or played."));
        };

        if (ImGui::MenuItem(_L("Save Snapshot..."), "Alt+F6"))
        {
            snapshot_pause_and_request(&s_snapshot_save_requested);
        }

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Quick Load"), "Alt+F9", false, !replay_active))
        {
            snapshot_ui_request_quickload();
        }


        if (ImGui::MenuItem(_L("Quick Save"), "Alt+F8"))
        {
            snapshot_pause_and_request(&s_snapshot_quicksave_requested);
        }

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Snapshot Setup...")))
        {
            g_gui->showSnapshotSetupWindow = true;
        }

        ImGui::EndMenu();
    }
}
