/*
 * dbg_focus_to.cpp — Modální dialog "Focus To..." pro sekci Disassembled
 *
 * PŘEHLED
 * =======
 * Modální dialog umožňující zadat hex adresu pro nastavení focusu
 * dolní tabulky disassembly. Otevírá se z context menu položkou
 * "Focus To..." v editačním režimu.
 *
 * LAYOUT
 * ======
 *
 *  ┌─── Focus To... ───────────────────┐
 *  │                                   │
 *  │  Address:  [0012] [▼]            │
 *  │                                   │
 *  │           [Cancel]    [Apply]     │
 *  └───────────────────────────────────┘
 *
 * CHOVÁNÍ
 * =======
 * - Při otevření se předvyplní poslední použitá adresa a celá se selektuje
 * - Pokud je historie prázdná, předvyplní se "0000"
 * - Textový vstup: max 4 hex znaky, uppercase, CharsHexadecimal
 * - Pokud uživatel zadá méně než 4 znaky, doplní se zleva nulami
 * - Roletka (▼): historie posledních unikátních hodnot, seřazená vzestupně
 * - ESC zavře dialog
 * - ENTER = Apply (odešle hodnotu)
 * - Pokud je text selektován a uživatel stiskne [0-9A-F], přepíše se
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "debugger/debugger.h"

#include "dbg_focus_to.h"
#include "dbg_disassembled.h"
#include "../debugger_state.h"

#include <stdio.h>
#include <string.h>

/* Maximální počet položek v historii */
#define FOCUS_HISTORY_MAX 32

/* Stav dialogu */
static bool s_should_open = false;  /* Požadavek na otevření popupu */
static bool s_focus_input = false;  /* Nastavit focus na InputText (první frame) */
static char s_addr_buf[5] = "0000"; /* Buffer textového vstupu (4 hex znaky + '\0') */

/*
 * Cílová instance disassembly view - kam se při Apply zapíše focus_addr.
 * Nastavuje dbg_focus_to_open(), používá dbg_focus_to_render() při Apply.
 *
 * Lifetime: pointer drží mezi otevřením dialogu a Apply/Cancel. Pokud
 * je view destruováno mezitím, dbg_disasm_view_destroy() musí zavolat
 * dbg_focus_to_invalidate_target_if_matches() aby pointer nezůstal
 * dangling. Pokud je s_target == NULL při Apply, akce je silent no-op.
 */
static DisassembledView *s_target = NULL;

/* Historie adres — seřazená vzestupně, unikátní hodnoty */
static uint16_t s_history[FOCUS_HISTORY_MAX];
static int s_history_count = 0;

/* Poslední použitá hodnota (pro předvyplnění při otevření) */
static uint16_t s_last_value = 0;
static bool s_has_last_value = false;

/* Pozice kurzoru v okamžiku otevření dialogu - dialog se zobrazí
 * blízko klikajícího řádku (= ne na zapamatované pozici z minula).
 * SetNextWindowPos s ImGuiCond_Appearing umožní uživateli pak dialog
 * přemístit, než se znovu otevře. */
static ImVec2 s_open_mouse_pos = ImVec2(0.0f, 0.0f);
static bool s_have_open_mouse_pos = false;


/*
 * history_add — přidá adresu do historie (pokud tam ještě není).
 * Po přidání seřadí historii vzestupně (bubble sort — max 32 prvků).
 */
static void history_add(uint16_t addr)
{
    /* Kontrola, zda adresa už v historii je */
    for (int i = 0; i < s_history_count; i++)
    {
        if (s_history[i] == addr)
            return;
    };

    /* Přidáme na konec (pokud je místo) */
    if (s_history_count < FOCUS_HISTORY_MAX)
    {
        s_history[s_history_count++] = addr;
    }
    else
    {
        /* Plná historie — odstraníme první a přidáme na konec */
        memmove(&s_history[0], &s_history[1],
                (FOCUS_HISTORY_MAX - 1) * sizeof(uint16_t));
        s_history[FOCUS_HISTORY_MAX - 1] = addr;
    };

    /* Seřadíme vzestupně */
    for (int i = 0; i < s_history_count - 1; i++)
    {
        for (int j = i + 1; j < s_history_count; j++)
        {
            if (s_history[i] > s_history[j])
            {
                uint16_t tmp = s_history[i];
                s_history[i] = s_history[j];
                s_history[j] = tmp;
            };
        };
    };
}


void dbg_focus_to_open(DisassembledView *target)
{
    s_should_open = true;
    s_focus_input = true;
    s_target = target;
    /* Zachytíme pozici myši v okamžiku otevření. Použije se v
     * dbg_focus_to_render() přes ImGui::SetNextWindowPos s Appearing,
     * aby se dialog objevil u kurzoru, ne na zapamatované pozici z
     * předchozího otevření. */
    s_open_mouse_pos = ImGui::GetMousePos();
    s_have_open_mouse_pos = true;

    /* Předvyplníme poslední použitou hodnotu, nebo 0000 pokud historie prázdná */
    if (s_has_last_value)
    {
        snprintf(s_addr_buf, sizeof(s_addr_buf), "%04X", s_last_value);
    }
    else
    {
        snprintf(s_addr_buf, sizeof(s_addr_buf), "0000");
    };
}


void dbg_focus_to_invalidate_target_if_matches(const DisassembledView *view)
{
    if (s_target == view)
    {
        s_target = NULL;
        /*
         * Pokud byl pending požadavek na otevření dialogu, zrušíme ho -
         * neotevřeme dialog s NULL targetem.
         */
        s_should_open = false;
    };
}


void dbg_focus_to_render(void)
{
    if (s_should_open)
    {
        ImGui::OpenPopup(_L("Focus To...##dbg_focus"));
        s_should_open = false;
    };

    /* Pozice dialogu při Appearing = u kurzoru (zachycená v open()).
     * Po přemístění uživatelem zůstane do dalšího open. */
    if (s_have_open_mouse_pos)
    {
        ImGui::SetNextWindowPos(s_open_mouse_pos, ImGuiCond_Appearing);
        s_have_open_mouse_pos = false;
    };

    if (!ImGui::BeginPopupModal(_L("Focus To...##dbg_focus"), NULL,
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings))
        return;

    /* ESC → zavřít jen tento popup (ne rodičovské okno debuggeru) */
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        s_target = NULL;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    };

    /* Popisek */
    ImGui::Text("%s", _("Address:"));
    ImGui::SameLine();

    /* Šířka textového vstupu — 4 hex znaky + drobný padding */
    float char_w = ImGui::CalcTextSize("0").x;
    float input_w = char_w * 6.0f;

    /* Textový vstup — hex filtrace, max 4 znaky, AutoSelectAll */
    ImGuiInputTextFlags input_flags =
        ImGuiInputTextFlags_CharsHexadecimal |
        ImGuiInputTextFlags_CharsUppercase |
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_AutoSelectAll;

    /* Při prvním zobrazení nastavíme focus na InputText */
    if (s_focus_input)
    {
        ImGui::SetKeyboardFocusHere();
        s_focus_input = false;
    };

    ImGui::SetNextItemWidth(input_w);
    bool enter_pressed = ImGui::InputText("##focus_addr", s_addr_buf,
                                           sizeof(s_addr_buf), input_flags);

    /*
     * Roletka s historií — malý dropdown tlačítko vedle textového vstupu.
     * ImGuiComboFlags_NoPreview zobrazí jen šipku ▼ bez textového náhledu.
     * Při výběru položky se hodnota vyplní do InputText.
     */
    if (s_history_count > 0)
    {
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight()); /* šířka = výška (čtvercové tlačítko) */
        if (ImGui::BeginCombo("##focus_history", NULL, ImGuiComboFlags_NoPreview))
        {
            for (int i = 0; i < s_history_count; i++)
            {
                char label[5];
                snprintf(label, sizeof(label), "%04X", s_history[i]);
                if (ImGui::Selectable(label, false))
                {
                    /* Vybraná hodnota z historie → vyplníme do InputText */
                    snprintf(s_addr_buf, sizeof(s_addr_buf), "%04X", s_history[i]);
                };
            };
            ImGui::EndCombo();
        };
    };

    ImGui::Separator();

    /* Tlačítka Cancel a Apply */
    if (ImGui::Button(_L("Cancel")))
    {
        s_target = NULL;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    };

    ImGui::SameLine();

    if (ImGui::Button(_L("Apply")) || enter_pressed)
    {
        /*
         * Parsování adresy — debuger_hextext_to_uint32 převede hex text na číslo.
         * Pokud uživatel zadal méně než 4 znaky, funkce je interpretuje správně
         * (např. "D9" → 0x00D9). Doplnění nul zleva je implicitní.
         */
        uint32_t addr = debuger_hextext_to_uint32(s_addr_buf);
        uint16_t focus_addr = (uint16_t)(addr & 0xFFFF);

        /* Uložíme do historie a jako poslední hodnotu */
        history_add(focus_addr);
        s_last_value = focus_addr;
        s_has_last_value = true;

        /*
         * Nastavíme focus v disassembly tabulce cílové instance.
         * Pokud target zmizel mezi otevřením a Apply (= zavřené sekundární
         * okno), je no-op - dialog se zavře tiše. Apply z Focus To dialogu
         * je explicit user akce, používáme focus_to (= auto-disable
         * Follow PC pokud target instance běží + emu není v pauze).
         */
        if (s_target != NULL)
        {
            dbg_disasm_view_focus_to(s_target, focus_addr);
        };

        s_target = NULL;
        ImGui::CloseCurrentPopup();
    };

    ImGui::EndPopup();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
