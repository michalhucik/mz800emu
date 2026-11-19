/**
 * @file   overlay_tab_input.cpp
 * @brief  Tab Input overlay menu: okna virtuální klávesnice a Autotype, zařízení joysticku a Joystick Setup.
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW::input) a akce jen vyžádá
 * (overlay_actions.h). Dvě karty: KEYBOARD (Virtual keyboard a Autotype
 * buffer otevřou své okno a overlay zavřou, ruling K11; věta o tom, že
 * klávesy při otevřeném menu nejdou do emulovaného stroje) a JOYSTICK
 * (Device = zařízení prvního portu joysticku None / Numeric keypad /
 * Joystick, a tlačítko Joystick Setup...).
 *
 * Volba Device dělá totéž co "Apply" okna Joystick Setup pro první port;
 * druhý port, výběr konkrétního zařízení a mapování zůstávají v okně.
 * Volba "Joystick" je dostupná, jen když hostitel nějaký joystick má, a
 * celá volba je zakázaná při záznamu nebo přehrávání vstupů (změnu typu
 * emu vlákno tehdy odmítá).
 *
 * Fokus: výchozí fokus tabu má zvolený segment řádku Device; je-li volba
 * zakázaná, tlačítko Virtual keyboard.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdio.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tab_util.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

#ifdef MZTEST_HEADLESS
/* testy prodlužují přeložené texty tabu o 40 % (overlay_test_set_stretch()) */
#undef _
#define _(STRING) overlay_test_tr(STRING)
#endif

/** @brief Věta pod kartou KEYBOARD. */
static const char *input_keys_note(void)
{
    return _("While this menu is open, keys do not reach the emulated computer.");
}

/**
 * @brief Tlačítko akce u pravého okraje řádku nastavení, široké podle obsahu (nejvýš zbytek řádku).
 * @param id        Akce.
 * @param v         Kopie stavu.
 * @param text      Přeložený popisek.
 * @param test_name Jméno obdélníku pro testy.
 * @param focus     true = tlačítko má dostat výchozí fokus tabu, když ho rámec žádá.
 * @pre Mezi ovl_row_begin() a ovl_row_end().
 */
static void input_row_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, const char *test_name, bool focus)
{
    char label[256];
    snprintf(label, sizeof(label), "%s###ovl_action_%d", text, (int)id);
    const float avail = ImGui::GetContentRegionAvail().x;
    float bw = ovl_button_natural_width(label, NULL, overlay_action_keycap(id, v));
    if (bw > avail) bw = floorf(avail);
    ovl_row_right(bw);
    ovl_tab_action_button(id, v, text, NULL, OVL_BUTTON_NORMAL, ImVec2(bw, 0.0f), test_name);
    if (focus && overlay_default_focus_wanted()) overlay_focus_last_item();
}

/**
 * @brief Výška obsahu karty KEYBOARD.
 * @param wrap_w Vnitřní šířka karty v pixelech (zalomení věty).
 * @return Výška v pixelech.
 */
static float input_keyboard_content_h(float wrap_w)
{
    return 2.0f * ovl_row_height() + floorf(overlay_px(OVL_TAB_GAP)) + ovl_tab_note_height(input_keys_note(), wrap_w);
}

/**
 * @brief Karta KEYBOARD: okna Virtual keyboard a Autotype buffer a věta o klávesách.
 * @param v            Kopie stavu.
 * @param size         Velikost karty.
 * @param focus_button true = výchozí fokus tabu patří tlačítku Virtual keyboard.
 */
static void input_card_keyboard(const st_OVERLAY_VIEW *v, ImVec2 size, bool focus_button)
{
    if (ovl_tab_card_begin(_("Keyboard"), "ovl_card_keyboard", size)) {
        const float left = ImGui::GetCursorScreenPos().x;
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        ovl_row_begin(_("Virtual keyboard"));
        input_row_button(OVL_ACTION_VKBD, v, _("Open..."), "inp_vkbd", focus_button);
        ovl_row_end();
        ovl_row_begin(_("Autotype buffer"));
        input_row_button(OVL_ACTION_AUTOTYPE, v, _("Open..."), "inp_autotype", false);
        ovl_row_end();
        float y = ImGui::GetCursorScreenPos().y + floorf(overlay_px(OVL_TAB_GAP));
        y += ovl_tab_note(input_keys_note(), left, y, w, "inp_note");
        ovl_tab_finish_content(left, y);
    }
    ovl_card_end();
    overlay_test_note_item("card_keyboard");
}

/**
 * @brief Karta JOYSTICK: zařízení prvního portu a tlačítko Joystick Setup....
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void input_card_joystick(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Joystick"), "ovl_card_joystick", size)) {
        ovl_row_begin(_("Device"));
        {
            const char *labels[OVERLAY_JOY_TYPE_COUNT] = {_("None"), _("Numeric keypad"), _("Joystick")};
            const char *reason = overlay_action_reason(OVL_ACTION_JOY_DEVICE, v);
            /* volba Joystick potřebuje zařízení hostitele; už zvolenou nechává být */
            const char *no_joy = (!v->input.joy_available && v->input.joy_type != (int)OVERLAY_JOY_JOYSTICK) ? _("No joystick found. Use Joystick Setup to rescan devices.") : NULL;
            const char *reasons[OVERLAY_JOY_TYPE_COUNT] = {reason, reason, reason ? reason : no_joy};
            int sel = (v->input.joy_type >= 0 && v->input.joy_type < (int)OVERLAY_JOY_TYPE_COUNT) ? v->input.joy_type : 0;
            const int shown = sel;
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            if (ovl_segmented_reasons("inp_joy_device", labels, NULL, reasons, (int)OVERLAY_JOY_TYPE_COUNT, &sel, -1.0f)) overlay_action_request_value(OVL_ACTION_JOY_DEVICE, sel);
            overlay_test_note_item("inp_joy_device");
            if (!reason && overlay_default_focus_wanted()) {
                /* ID segmentu jako v ovl_segmented_reasons(): index pod ID prvku */
                ImGui::PushID("inp_joy_device");
                const ImGuiID seg_id = window->GetID(shown);
                ImGui::PopID();
                overlay_focus_item(seg_id);
            }
        }
        ovl_row_end();
        ovl_row_begin(_("Mapping and calibration"));
        input_row_button(OVL_ACTION_JOY_SETUP, v, _("Joystick Setup..."), "inp_joy_setup", false);
        ovl_row_end();
    }
    ovl_card_end();
    overlay_test_note_item("card_joystick");
}

void overlay_tab_input_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    const bool narrow = overlay_is_narrow();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    const float gap = floorf(overlay_px(OVL_TAB_CARD_GAP));
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    const float col_w = narrow ? w : floorf((w - gap) * 0.5f);
    /* zakázaná volba Device fokus nedostane: výchozí prvek je pak tlačítko Virtual keyboard */
    const bool focus_button = overlay_action_reason(OVL_ACTION_JOY_DEVICE, v) != NULL;

    float keyboard_h = ovl_card_height(input_keyboard_content_h(col_w - 2.0f * pad), true);
    float joystick_h = ovl_card_height(2.0f * ovl_row_height(), true);

    float y = origin.y;
    if (!narrow) {
        /* dva sloupce: karty vedle sebe mají výšku té vyšší */
        if (keyboard_h < joystick_h) keyboard_h = joystick_h;
        else joystick_h = keyboard_h;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        input_card_keyboard(v, ImVec2(col_w, keyboard_h), focus_button);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + w - col_w, y));
        input_card_joystick(v, ImVec2(col_w, joystick_h));
        y += keyboard_h;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        input_card_keyboard(v, ImVec2(w, keyboard_h), focus_button);
        y += keyboard_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        input_card_joystick(v, ImVec2(w, joystick_h));
        y += joystick_h;
    }
    ovl_tab_finish_content(origin.x, y);
}
