#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "libs/imgui/imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <glib.h>
#include <math.h>
#include <string.h>

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "emulator_measuring.h"

extern "C"
{
    void imgui_measuring_frame_timing(bool *p_open);
};

void imgui_measuring_frame_timing(bool *p_open)
{
    if (!*p_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(280, 320), ImGuiCond_FirstUseEver);
    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *ft_title = _L("Frame Timing Accuracy");
    auto_layout_first_use_portrait(ft_title, 400.0f, 400.0f);
    if (ImGui::Begin(ft_title, p_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (EMULATOR_TEST_MAX_SPEED)
        {
            ImGui::Text(_("Frame timing measurement is disabled when the emulator is running at max speed."));
        }
        else
        {
            if (ImGui::BeginTable("FrameTimingTable", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 180.0f); // Fixní šířka pro hodnoty

                auto printRow = [](const char *label, float value, const char *format = "%.4f ms")
                {
                    char buffer[32]; // Pro formátovanou hodnotu
                    snprintf(buffer, sizeof(buffer), format, value);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(label);
                    ImGui::TableNextColumn();

                    // Vypočítání šířky textu
                    float textWidth = ImGui::CalcTextSize(buffer).x;
                    float columnWidth = ImGui::GetColumnWidth();
                    float cursorX = ImGui::GetCursorPosX() + columnWidth - textWidth - ImGui::GetStyle().ItemSpacing.x;

                    ImGui::SetCursorPosX(cursorX);
                    ImGui::TextUnformatted(buffer);
                };

                st_EMULATOR_MEASURING_FRAME_TIMING_STATS *stats = &g_emulator_measuring.frame_timing.stats;

                float ref_frame_time = 1000.0f / g_mzhal.video_screens_per_sec;
                printRow(_("Reference frame time:"), ref_frame_time);
                printRow(_("Average time:"), stats->mean);
                printRow(_("Max time:"), stats->max);
                printRow(_("Min time:"), stats->min);
                printRow(_("Median:"), stats->median);
                printRow(_("Std. deviation:"), stats->stddev);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Separator();
                ImGui::TableNextColumn();
                ImGui::Separator();

                printRow(_("Frames count:"), stats->samples_count, "%.0f     ");
                printRow(_("Total width:"), stats->total_width, "%.2f    s");
                printRow(_("Real total width:"), stats->real_total_width, "%.4f    s");

                ImGui::EndTable();

                ImGui::Separator();

                ImGui::Text("\n");
                ImGui::Text(_("Statistics Update Interval (s):"));
                int updateInterval = EMULATOR_MEASURING_FRAME_TIMING_GET_PREDEF_TIME_IN_SEC();
                if (ImGui::SliderInt("###Update Interval (s) slider", &updateInterval, 1, MAX_MEASURING_FRAME_TIMING_IN_SECS))
                {
                    EMULATOR_MEASURING_FRAME_TIMING_SET_PREDEF_TIME_IN_SEC(updateInterval);
                };
                if (ImGui::InputInt("###Update Interval (s) input", &updateInterval, 1, MAX_MEASURING_FRAME_TIMING_IN_SECS))
                {
                    EMULATOR_MEASURING_FRAME_TIMING_SET_PREDEF_TIME_IN_SEC(updateInterval);
                };

                ImGui::Separator();
                ImGui::Text("\n");
                ImGui::Text(_("Statistics from last (s):"));
                int measuringWidth = EMULATOR_MEASURING_FRAME_TIMING_GET_MEASURING_WIDTH_IN_SEC();
                if (ImGui::SliderInt("###Statistics from last (s) slider", &measuringWidth, updateInterval, MAX_MEASURING_FRAME_TIMING_IN_SECS))
                {
                    if (measuringWidth < updateInterval)
                        EMULATOR_MEASURING_FRAME_TIMING_SET_PREDEF_TIME_IN_SEC(measuringWidth);

                    EMULATOR_MEASURING_FRAME_TIMING_SET_MEASURING_WIDTH_IN_SEC(measuringWidth);
                };
                if (ImGui::InputInt("###Statistics from last (s) input", &measuringWidth, updateInterval, MAX_MEASURING_FRAME_TIMING_IN_SECS))
                {
                    if (measuringWidth < updateInterval)
                        EMULATOR_MEASURING_FRAME_TIMING_SET_PREDEF_TIME_IN_SEC(measuringWidth);

                    EMULATOR_MEASURING_FRAME_TIMING_SET_MEASURING_WIDTH_IN_SEC(measuringWidth);
                };
            };
        };

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            *p_open = false;
        };

        ImGui::End();
    }

    if (!*p_open)
    {
        emulator_measuring_frame_timing_reset();
    }
}
