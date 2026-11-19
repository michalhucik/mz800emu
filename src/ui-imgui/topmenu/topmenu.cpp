#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>
#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "emulator/replay.h"
#include "emulator/device_request.h"
#include "mzarch/mzarch_platform_functions.h"
#include "mzarch/mzarch_request.h"
// #include "iface/iface_video.h"
#include "topmenu.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/overlay/overlay.h"

// #if HAVE_JOY
// #include "iface/iface_joy.h"
// #endif

extern "C"
{
    void imgui_topmenu_handler(void);
    void imgui_topmenu_body(void);
} // extern "C"

/**
 * @brief Obsah klasického menu emulátoru (popup pravého tlačítka myši nad obrazem).
 *
 * První položka "Overlay Menu..." otevírá overlay menu: zvolení popup
 * zavře a overlay otevře hlavní okno až v příštím snímku
 * (imgui_overlay_request_open()), aby popup nezůstal pod overlayem a
 * overlay dostal fokus. Sloupec zkratek položky ("F10, Alt+Click")
 * připomíná obě cesty mimo menu: klávesu F10 a levý Alt s levým
 * tlačítkem myši nad obrazem (main_window_overlay_open_click()).
 * Následují nabídky zařízení (pod
 * device_state_lock(); při aktivním záznamu nebo přehrávání vstupů
 * zakázané), rozhraní, rychlosti, emulátoru, snapshotů a replay, položky
 * Pause Emulation a Reset, nabídka debuggeru (jen v buildu s debuggerem),
 * nástroje a About.
 *
 * @pre UI vlákno; uvnitř otevřeného popupu (po úspěšném ImGui::BeginPopup()).
 * @post Popup je uzavřen (ImGui::EndPopup()).
 */
void imgui_topmenu_body(void)
{
    /* Overlay menu: stejná cesta jako F10 nebo levý Alt + klik nad obrazem; při otevřeném overlayi položka
     * nemá co dělat. Názvy kláves ve sloupci zkratek se nepřekládají. */
    if (ImGui::MenuItem(_L("Overlay Menu..."), "F10, Alt+Click", false, !imgui_overlay_is_open()))
    {
        imgui_overlay_request_open();
    };
    ImGui::Separator();

    /* Záznam a přehrání vstupů: nastavení stroje se během nich měnit nesmí
     * (záznam by změnu nezopakoval, rozhodnutí M6a v1). */
    bool replay_active = replay_is_active();
    ImGui::BeginDisabled(replay_active);
    /* Menu zařízení čte cesty a buffery zařízení; zásahy (device_request.h)
     * provádí emu vlákno, které po dobu kreslení menu čeká. */
    device_state_lock();
    imgui_menu_devices();
    device_state_unlock();
    /* Fast Open CMT File & Play...: akce dotazu Virtual CMT až bez zámku */
    imgui_cmt_tape_run_deferred();
    ImGui::EndDisabled();
    if (replay_active && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", _("Machine settings cannot be changed while a replay is recorded or played. "
                                  "Stop it in the Replay menu first."));
    };
    imgui_menu_interface();
    imgui_menu_speed();
    imgui_menu_emulator();
    imgui_menu_snapshot();
    imgui_menu_replay();
    ImGui::Separator();

    if ((ImGui::MenuItem(_L("Pause Emulation"), "Alt+P", EMULATOR_TEST_PAUSED)) || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_P))
    {
        dbg_ui_pause_toggle();
    };

    if ((ImGui::MenuItem(_L("Reset"), "F12")) || ImGui::Shortcut(ImGuiKey_F12))
    {
        mzarch_request_user_reset();
    }

    ImGui::Separator();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    imgui_menu_debugger(DBG_MENU_CALLER_TOPMENU);
    ImGui::Separator();
#endif

    imgui_menu_tools();

    ImGui::Separator();

    if (ImGui::MenuItem(_L("About...")))
    {
        g_gui->showAboutWindow = true;
    };

    ImGui::EndPopup();
}

void imgui_topmenu_handler(void) // pouzivame pokud je emulator zobrazen jako pozadi
{
    if (ImGui::BeginPopupContextVoid("EmulatorPopupMenu", ImGuiPopupFlags_MouseButtonRight))
    {
        imgui_topmenu_body();
    }
}
