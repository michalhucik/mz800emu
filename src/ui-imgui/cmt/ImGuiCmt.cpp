/**
 * @file ImGuiCmt.cpp
 * @brief Seznamy rychlostí kazety pro ImGui a modální editor vlastních délek pulzů (Custom Tape Pulses).
 */
#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <vector>
#include <string>

// Lokalizace
#include "i18n.h"

#include "libs/cmtspeed/cmtspeed.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmt_tmz.h"

#include "ui/cmt/UiCmt.hpp"
#include "ImGuiCmt.hpp"

#include "libs/imgui/imgui.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "libs/mztape/mztape.h"

struct
{
    std::vector<ImGuiCmtSpeedList_t> mz;
    std::vector<ImGuiCmtSpeedList_t> mz_default;
    std::vector<ImGuiCmtSpeedList_t> zx;
} g_imgui_speeds;

void createList(std::vector<ImGuiCmtSpeedList_t> &list, const std::vector<UiCmtSpeedList_t> &ui_list)
{
    list.clear();
    for (size_t i = 0; i < ui_list.size(); i++)
    {
        ImGuiCmtSpeedList_t item;
        item.cmtspeed = ui_list[i].cmtspeed;
        item.cmtspeed_txt = ui_list[i].cmtspeed_txt;
        item.label_txt = ui_list[i].cmtspeed_txt.c_str();
        item.label_txt += "##speed_row_";
        item.label_txt += std::to_string(i);
        list.push_back(item);
    };
}

std::vector<ImGuiCmtSpeedList_t> ImGuiCmt::getMzSpeedList(void)
{
    if (g_imgui_speeds.mz.empty())
    {
        std::vector<UiCmtSpeedList_t> ui_list = UiCmt::getMzSpeedList();
        createList(g_imgui_speeds.mz, ui_list);
    };
    return g_imgui_speeds.mz;
}

std::vector<ImGuiCmtSpeedList_t> ImGuiCmt::getMzSpeedListDefault(void)
{
    if (g_imgui_speeds.mz_default.empty())
    {
        std::vector<UiCmtSpeedList_t> ui_list = UiCmt::getMzSpeedListDefault();
        createList(g_imgui_speeds.mz_default, ui_list);
    };
    return g_imgui_speeds.mz_default;
}

std::vector<ImGuiCmtSpeedList_t> ImGuiCmt::getZxSpeedList(void)
{
    if (g_imgui_speeds.zx.empty())
    {
        std::vector<UiCmtSpeedList_t> ui_list = UiCmt::getZxSpeedList();
        createList(g_imgui_speeds.zx, ui_list);
    };
    return g_imgui_speeds.zx;
}


/**
 * @brief Stav editoru vlastních délek pulzů (jediný pro celé UI).
 *
 * Invariant: open_request a open platí jen pro scope; us[] jsou upravované
 * hodnoty v µs, initial_us[] hodnoty při otevření (Revert).
 */
static struct
{
    const char *scope;     /**< okno, které editor otevřelo (NULL = žádné) */
    int owner;             /**< údaj volajícího (např. index bloku) */
    bool open_request;     /**< OpenPopup se zavolá v nejbližším drawPulsesEditor() */
    double us[4];          /**< upravované délky v µs: LONG high, LONG low, SHORT high, SHORT low */
    double initial_us[4];  /**< délky při otevření */
} g_imgui_pulses_editor = {nullptr, 0, false, {0, 0, 0, 0}, {0, 0, 0, 0}};

void ImGuiCmt::openPulsesEditor(const char *scope, int owner, const st_MZTAPE_PULSES_LENGTH *initial)
{
    g_imgui_pulses_editor.scope = scope;
    g_imgui_pulses_editor.owner = owner;
    g_imgui_pulses_editor.open_request = true;
    g_imgui_pulses_editor.initial_us[0] = initial->long_pulse.high * 1e6;
    g_imgui_pulses_editor.initial_us[1] = initial->long_pulse.low * 1e6;
    g_imgui_pulses_editor.initial_us[2] = initial->short_pulse.high * 1e6;
    g_imgui_pulses_editor.initial_us[3] = initial->short_pulse.low * 1e6;
    for (int i = 0; i < 4; i++)
        g_imgui_pulses_editor.us[i] = g_imgui_pulses_editor.initial_us[i];
}

bool ImGuiCmt::drawPulsesEditor(const char *scope, int *owner, st_MZTAPE_PULSES_LENGTH *result)
{
    if ((g_imgui_pulses_editor.scope == nullptr) || (strcmp(g_imgui_pulses_editor.scope, scope) != 0))
        return false;

    /* ID popupu nezávislé na překladu titulku ("###"):
     * OpenPopup() i BeginPopupModal() počítají ID jen z části za "###" */
    char title[160];
    snprintf(title, sizeof(title), "%s%s", _("Custom Tape Pulses"), IMGUI_CMT_PULSES_POPUP_ID);

    /* požadavek čeká, dokud je otevřený jiný popup (imgui_modal.h) */
    if (g_imgui_pulses_editor.open_request && imgui_modal_can_open())
    {
        ImGui::OpenPopup(IMGUI_CMT_PULSES_POPUP_ID);
        g_imgui_pulses_editor.open_request = false;
    };

    bool confirmed = false;

    /* uprostřed okna Virtual CMT a v jeho viewportu */
    imgui_cmt_window_modal_next_pos();
    if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(_("Pulse lengths in microseconds (order of the UniCMT CMTSPEED header):"));
        ImGui::Spacing();

        const char *labels[4] = {
            _L("Long pulse high (bit 1)"),
            _L("Long pulse low (bit 1)"),
            _L("Short pulse high (bit 0)"),
            _L("Short pulse low (bit 0)"),
        };
        for (int i = 0; i < 4; i++)
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            ImGui::InputDouble(labels[i], &g_imgui_pulses_editor.us[i], 1.0, 10.0, "%.3f");
        };

        ImGui::Spacing();
        ImGui::TextUnformatted(_("Presets:"));
        int count = 0;
        const UiCmtPulsesPreset_t *presets = UiCmt::getPulsesPresets(&count);
        for (int p = 0; p < count; p++)
        {
            ImGui::SameLine();
            if (ImGui::Button(presets[p].name))
            {
                for (int i = 0; i < 4; i++)
                    g_imgui_pulses_editor.us[i] = presets[p].us[i];
            };
        };
        ImGui::SameLine();
        if (ImGui::Button(_L("Revert")))
        {
            for (int i = 0; i < 4; i++)
                g_imgui_pulses_editor.us[i] = g_imgui_pulses_editor.initial_us[i];
        };

        st_MZTAPE_PULSES_LENGTH pulses;
        bool valid = (EXIT_SUCCESS == mztape_pulses_set_us(&pulses,
                                                           g_imgui_pulses_editor.us[0], g_imgui_pulses_editor.us[1],
                                                           g_imgui_pulses_editor.us[2], g_imgui_pulses_editor.us[3]));
        ImGui::Spacing();
        if (valid)
        {
            double ratio = mztape_pulses_get_ratio(CMTTMZ_FORMATSET, &pulses);
            ImGui::Text(_("Equivalent speed: ~%.2f:1 (%.0f Bd)"), ratio, ratio * MZTAPE_DEFAULT_BDSPEED);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), _("Each length must be greater than 0 and at most %.0f µs."), MZTAPE_PULSE_US_MAX);
        };

        ImGui::Separator();

        if (!valid)
            ImGui::BeginDisabled();
        if (ImGui::Button(_L("OK"), ImVec2(120, 0)))
        {
            *owner = g_imgui_pulses_editor.owner;
            *result = pulses;
            confirmed = true;
            g_imgui_pulses_editor.scope = nullptr;
            ImGui::CloseCurrentPopup();
        };
        if (!valid)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            g_imgui_pulses_editor.scope = nullptr;
            ImGui::CloseCurrentPopup();
        };

        ImGui::EndPopup();
    }
    else if (!g_imgui_pulses_editor.open_request)
    {
        /* popup zavřený jinak než tlačítkem (např. zavřením okna) */
        g_imgui_pulses_editor.scope = nullptr;
    };

    return confirmed;
}
