#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "emulator/mzarch/mzhal.h"
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
    void imgui_measuring_gdg(bool *p_open);
};

static GString *format_uint32_with_thousands(uint32_t number)
{
    GString *str1 = g_string_new(NULL);
    g_string_printf(str1, "%'u", number);

    GString *formatted = g_string_new(NULL);
    gchar *p = str1->str;

    while (*p)
    {
        gunichar ch = g_utf8_get_char(p);

        if (g_ascii_isdigit(ch))
        {
            g_string_append_unichar(formatted, ch);
        }
        else
        {
            // Nahradíme oddělovač mezerou
            g_string_append_c(formatted, ' ');
        }

        p = g_utf8_next_char(p);
    }

    g_string_free(str1, TRUE);
    return formatted;
}

/* Runtime z g_mzhal (mzhal 10b, cold - UI render). */
#define NATIVE_FPS (float)((float)g_mzhal.gdgclk_real_base / g_mzhal.video_screen_ticks)
#define SIMULATED_FPS (float)((float)g_mzhal.gdgclk_base / g_mzhal.video_screen_ticks)

void imgui_measuring_gdg(bool *p_open)
{
    if (!*p_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(280, 320), ImGuiCond_FirstUseEver);
    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *gdg_title = _L("Accuracy of GDG");
    auto_layout_first_use_portrait(gdg_title, 400.0f, 400.0f);
    if (ImGui::Begin(gdg_title, p_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::BeginTable("GDG Table", 2, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 180.0f); // Fixní šířka pro hodnoty

            auto printRow = [](const char *label, char *txt)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();

                // Vypočítání šířky textu
                float textWidth = ImGui::CalcTextSize(txt).x;
                float columnWidth = ImGui::GetColumnWidth();
                float cursorX = ImGui::GetCursorPosX() + columnWidth - textWidth - ImGui::GetStyle().ItemSpacing.x;

                ImGui::SetCursorPosX(cursorX);
                ImGui::TextUnformatted(txt);
            };

            st_EMULATOR_MEASURING_GDG *msgdg = &g_emulator_measuring.gdg;
            g_rw_lock_reader_lock(&msgdg->rwlock);

            GString *str = g_string_new(NULL);
            g_string_printf(str, "%.4f  s", msgdg->elapsed_time);
            printRow(_("Measured time:"), str->str);
            g_string_free(str, TRUE);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Separator();
            ImGui::TableNextColumn();
            ImGui::Separator();

            str = format_uint32_with_thousands(g_mzhal.gdgclk_real_base);
            g_string_append_printf(str, " Hz");
            printRow(_("Native GDG freq.:"), str->str);
            g_string_free(str, TRUE);

            str = format_uint32_with_thousands(g_mzhal.gdgclk_real_base / g_mzhal.gdgclk2cpu_divider);
            g_string_append_printf(str, " Hz");
            printRow(_("Native CPU freq.:"), str->str);
            g_string_free(str, TRUE);

            str = g_string_new(NULL);
            g_string_printf(str, "%.4f Hz", NATIVE_FPS);
            printRow(_("Native FPS:"), str->str);
            g_string_free(str, TRUE);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Separator();
            ImGui::TableNextColumn();
            ImGui::Separator();

            str = format_uint32_with_thousands(g_mzhal.gdgclk_base);
            g_string_append_printf(str, " Hz");
            printRow(_("Simulated GDG freq.:"), str->str);
            g_string_free(str, TRUE);

            str = format_uint32_with_thousands(g_mzhal.gdgclk_base / g_mzhal.gdgclk2cpu_divider);
            g_string_append_printf(str, " Hz");
            printRow(_("Simulated CPU freq.:"), str->str);
            g_string_free(str, TRUE);

            str = g_string_new(NULL);
            g_string_printf(str, "%.4f Hz", SIMULATED_FPS);
            printRow(_("Simulated FPS:"), str->str);
            g_string_free(str, TRUE);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Separator();
            ImGui::TableNextColumn();
            ImGui::Separator();

            str = format_uint32_with_thousands(msgdg->current_gdg_frequency);
            g_string_append_printf(str, " Hz");
            printRow(_("Current GDG freq.:"), str->str);
            g_string_free(str, TRUE);

            str = format_uint32_with_thousands(msgdg->current_gdg_frequency / g_mzhal.gdgclk2cpu_divider);
            g_string_append_printf(str, " Hz");
            printRow(_("Current CPU freq.:"), str->str);
            g_string_free(str, TRUE);

            str = g_string_new(NULL);
            g_string_printf(str, "%.4f Hz", msgdg->current_fps);
            printRow(_("Current FPS:"), str->str);
            g_string_free(str, TRUE);

            str = g_string_new(NULL);
            g_string_printf(str, "%.4f  %%", msgdg->current_gdg_frequency_percent);
            printRow(_("Current is:"), str->str);
            g_string_free(str, TRUE);

            ImGui::EndTable();

            ImGui::Separator();

            ImGui::Text("\n");
            ImGui::Text(_("Update Interval (s):"));
            int updateInterval = EMULATOR_MEASURING_GET_GDG_UPDATE_TIME_SEC();
            if (ImGui::SliderInt("###Update Interval (s) slider", &updateInterval, 1, MAX_MEASURING_GDG_IN_SECS))
            {
                EMULATOR_MEASURING_SET_GDG_ENABLED_AND_UPDATE_TIME_SEC(true, updateInterval);
            };
            if (ImGui::InputInt("###Update Interval (s) input", &updateInterval, 1, MAX_MEASURING_GDG_IN_SECS))
            {
                EMULATOR_MEASURING_SET_GDG_ENABLED_AND_UPDATE_TIME_SEC(true, updateInterval);
            };

            g_rw_lock_reader_unlock(&msgdg->rwlock);
        };

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            *p_open = false;
        };

        ImGui::End();
    }

    if (!*p_open)
    {
        EMULATOR_MEASURING_SET_GDG_ENABLED_AND_UPDATE_TIME_SEC(false, EMULATOR_MEASURING_GET_GDG_UPDATE_TIME_SEC());
    }
}
