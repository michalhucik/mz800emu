/**
 * @file   overlay_tab_util.cpp
 * @brief  Společné stavební prvky tabů Sound, Input a Capture overlay menu (overlay_tab_util.h).
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

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tab_util.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"

bool ovl_tab_card_begin(const char *title, const char *id, ImVec2 size)
{
    char full[256];
    snprintf(full, sizeof(full), "%s###%s", title ? title : "", id);
    return ovl_card_begin(full, size);
}

void ovl_tab_finish_content(float x, float y)
{
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

void ovl_tab_toggle_row(const char *label, const char *id, bool on, en_OVL_ACTION action, const st_OVERLAY_VIEW *v, const char *test_name, bool focus)
{
    const float gap = floorf(overlay_px(OVL_TAB_GAP));
    const char *keycap = overlay_action_keycap(action, v);
    const ImVec2 ts = ovl_toggle_size();
    const float kc_w = keycap ? ovl_keycap_size(keycap).x : 0.0f;
    /* sloupec popisku ustoupí štítku zkratky, nejvýš na OVL_TAB_TOGGLE_LABEL_MIN_FRACTION šířky řádku */
    float fraction = OVL_TAB_TOGGLE_LABEL_FRACTION;
    const float row_w = ImGui::GetContentRegionAvail().x;
    if (keycap && row_w > 1.0f) {
        const float need = 1.0f - (ts.x + 2.0f * gap + kc_w) / row_w;
        if (need < fraction) fraction = (need > OVL_TAB_TOGGLE_LABEL_MIN_FRACTION) ? need : OVL_TAB_TOGGLE_LABEL_MIN_FRACTION;
    }
    ovl_row_begin_ex(label, fraction);
    const bool show_kc = keycap && ts.x + gap + kc_w <= ImGui::GetContentRegionAvail().x;
    ovl_row_right(ts.x + (show_kc ? gap + kc_w : 0.0f));
    const char *reason = overlay_action_reason(action, v);
    bool value = on;
    if (reason) ImGui::BeginDisabled();
    const bool changed = ovl_toggle(id, &value);
    if (reason) ImGui::EndDisabled();
    overlay_test_note_item(test_name);
    if (focus && overlay_default_focus_wanted()) overlay_focus_last_item();
    if (reason) ovl_tooltip_disabled(reason);
    else if (keycap && !show_kc) ovl_tooltip(keycap);
    if (changed && !reason) overlay_action_request(action);
    if (show_kc) {
        ImGui::SameLine(0.0f, gap);
        ovl_keycap(keycap);
        char kc_name[32];
        snprintf(kc_name, sizeof(kc_name), "%.20s_keycap", test_name);
        overlay_test_note_item(kc_name);
    }
    ovl_row_end();
}

bool ovl_tab_action_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, const char *icon, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name)
{
    const char *reason = overlay_action_reason(id, v);
    const char *keycap = overlay_action_keycap(id, v);
    char label[384];
    snprintf(label, sizeof(label), "%s###ovl_action_%d", text ? text : "", (int)id);

    if (reason) ImGui::BeginDisabled();
    bool pressed = ovl_button_ex(label, icon, keycap, kind, size);
    if (reason) ImGui::EndDisabled();
    if (test_name) overlay_test_note_item(test_name);

    if (reason) {
        ovl_tooltip_disabled(reason);
        pressed = false;
    }
    if (pressed) overlay_action_request(id);
    return pressed;
}

float ovl_tab_note_height(const char *text, float wrap_w)
{
    if (!text || !*text) return 0.0f;
    ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_HINT));
    const float h = ImGui::CalcTextSize(text, NULL, false, wrap_w > 1.0f ? wrap_w : 1.0f).y;
    ImGui::PopFont();
    return ceilf(h);
}

float ovl_tab_note(const char *text, float x, float y, float wrap_w, const char *test_name)
{
    if (!text || !*text) return 0.0f;
    const float h = ovl_tab_note_height(text, wrap_w);
    ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(y)));
    ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_HINT));
    ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT_SECONDARY);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (wrap_w > 1.0f ? wrap_w : 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (test_name) overlay_test_note_item(test_name);
    return h;
}
