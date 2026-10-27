/**
 * @file   pcg700_ui.cpp
 * @brief  UI zařízení PCG-700: překladové řetězce deskriptoru, položka
 *         podmenu a okno s náhledem PCG RAM (kontrakt v pcg700_ui.h).
 *
 * Generické podmenu zařízení (menu_devbus.cpp) bere název, popisky voleb
 * a popisky hodnot jako msgid z deskriptoru (pcg700_devbus.c). Jádro
 * emulátoru lokalizační makra nemá, proto je tady seznam N_() se stejnými
 * texty: jen kvůli xgettext (.pot). Při změně textu v deskriptoru změnit
 * i tady.
 *
 * Okno "PCG-700 Characters" kreslí 2 x 128 glyfů PCG RAM (kódy 80h-0FFh
 * obou sad) přímo do draw listu ImGui; čte instanci modelu pod zámkem
 * stavu zařízení (imgui_devbus_windows()).
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include <stdint.h>
#include <stddef.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "pcg700_ui.h"
#include "ui-imgui/mz700/devbus/menu_devbus.h"
#include "hw-generic/devbus/devbus.h"
#include "mzarch/mz700/devices/pcg700/pcg700_devbus.h"


/**
 * @brief Texty deskriptoru PCG-700 pro xgettext (název, popisky voleb a hodnot).
 *
 * Pole se za běhu nepoužívá k překladu (to dělá menu_devbus.cpp přes _L()
 * z deskriptoru); existuje jen kvůli .pot.
 */
static const char *const s_pcg700_msgids[] = {
    N_("PCG-700 (HAL)"),
    N_("HSW switch"),
    N_("NOM (standard character set)"),
    N_("PCG"),
    N_("SSW jumper (software control)"),
    N_("OFF (software cannot switch PCG)"),
    N_("ON (bit 3 of 0E012h switches PCG)"),
    N_("GSW jumper (registers)"),
    N_("OFF (registers disconnected)"),
    N_("ON (registers connected)"),
    N_("HSW PCG with SSW OFF"),
    N_("PCG on, HSW decides"),
    N_("PCG stays off"),
    N_("Write strobe"),
    N_("Rising edge of bit 4"),
    N_("Every write while bit 4 is set"),
    N_("Falling edge of bit 4 (experimental)"),
    N_("Decode writes also with DRAM at 0E000h"),
    N_("RAM after power on"),
    N_("Zeros"),
    N_("Copy of CG-ROM characters"),
};

/** @brief Je otevřené okno náhledu (jen UI vlákno). */
static bool s_show_window = false;


/** @brief Položky na konci podmenu zařízení (UI vlákno, drží zámek stavu zařízení). */
static void pcg700_ui_menu(void)
{
    ImGui::MenuItem(_L("Show PCG RAM characters"), NULL, &s_show_window);
}


/**
 * @brief Nakreslí 128 glyfů jedné sady PCG RAM v mřížce 16 x 8.
 *
 * @param ram   PCG RAM ve formátu CG-ROM (levý bod = bit 0), 2 kB.
 * @param set   0 = sada 1 (RAM 000h-3FFh), 1 = sada 2 (400h-7FFh).
 * @param scale Velikost bodu v pixelech.
 */
static void pcg700_ui_draw_set(const uint8_t *ram, unsigned set, float scale)
{
    const float cell = 8.0f * scale + 2.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImU32 bg = IM_COL32(0, 0, 0, 255);
    const ImU32 fg = IM_COL32(255, 255, 255, 255);

    dl->AddRectFilled(origin, ImVec2(origin.x + 16 * cell, origin.y + 8 * cell), IM_COL32(48, 48, 48, 255));
    for (unsigned g = 0; g < 128; g++) {
        float gx = origin.x + (float) (g % 16) * cell + 1.0f;
        float gy = origin.y + (float) (g / 16) * cell + 1.0f;
        dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + 8 * scale, gy + 8 * scale), bg);
        for (unsigned r = 0; r < 8; r++) {
            uint8_t b = ram[set * 0x400u + g * 8u + r];
            /* Souvislé úseky bodů jedním obdélníkem (méně vrcholů). */
            unsigned x = 0;
            while (x < 8) {
                if (!((b >> x) & 1u)) { x++; continue; }
                unsigned x0 = x;
                while (x < 8 && ((b >> x) & 1u)) x++;
                float y = gy + (float) r * scale;
                dl->AddRectFilled(ImVec2(gx + (float) x0 * scale, y), ImVec2(gx + (float) x * scale, y + scale), fg);
            }
        }
    }
    ImGui::Dummy(ImVec2(16 * cell, 8 * cell));
    if (ImGui::IsItemHovered()) {
        ImVec2 m = ImGui::GetMousePos();
        int col = (int) ((m.x - origin.x) / cell);
        int row = (int) ((m.y - origin.y) / cell);
        if (col >= 0 && col < 16 && row >= 0 && row < 8) {
            unsigned code = 0x80u + (unsigned) (row * 16 + col);
            ImGui::SetTooltip(_("Code %02Xh, set %u, PCG RAM %03Xh"), code, set + 1,
                              set * 0x400u + (code - 0x80u) * 8u);
        }
    }
}


/** @brief Okno náhledu (UI vlákno, každý snímek; zámek drží imgui_devbus_windows()). */
static void pcg700_ui_windows(void)
{
    if (!s_show_window) return;
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_L("PCG-700 Characters###pcg700_chars"), &s_show_window, ImGuiWindowFlags_AlwaysAutoResize)) {
        int slot = pcg700_devbus_slot();
        if (slot < 0 || !devbus_is_connected(slot)) {
            ImGui::TextUnformatted(_("The PCG-700 is not connected."));
        } else {
            const st_PCG700 *pcg = pcg700_devbus_instance();
            ImGui::Text(_("Display: %s"), pcg700_core_display_enabled(pcg) ? _("PCG RAM") : _("CG-ROM"));
            ImGui::SameLine();
            ImGui::Text(_("Control 0E012h: %02Xh"), pcg->ctrl);
            ImGui::TextUnformatted(_("Character set 1, codes 80h-FFh"));
            pcg700_ui_draw_set(pcg->ram, 0, 2.0f);
            ImGui::TextUnformatted(_("Character set 2, codes 80h-FFh"));
            pcg700_ui_draw_set(pcg->ram, 1, 2.0f);
        }
    }
    ImGui::End();
}


void pcg700_ui_register(void)
{
    (void) s_pcg700_msgids;
    (void) devbus_ui_register_extra(PCG700_DEVBUS_ID, pcg700_ui_menu, pcg700_ui_windows);
}
