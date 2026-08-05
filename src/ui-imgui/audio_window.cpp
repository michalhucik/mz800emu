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

#include "iface/iface_audio.h"
#include "emulator/mzarch/mzhal.h"

extern "C"
{
    void imgui_audio(bool *p_open);
};

/* MZ-700: pouze CTC0, zadny PSG */
static const char *channel_names_ctc_only[] = {
    N_("CTC0")
};
/* Mono PSG (MZ-800 nativne, MZ-1500 nikdy) */
static const char *channel_names_mono[] = {
    N_("CTC0"), N_("PSG tone1"), N_("PSG tone2"), N_("PSG tone3"), N_("PSG noise")
};
/* Stereo (MZ-1500 nativne, MZ-800 pres allow_psg1) */
static const char *channel_names_stereo[] = {
    N_("CTC0"),
    N_("PSG0 tone1"), N_("PSG0 tone2"), N_("PSG0 tone3"), N_("PSG0 noise"),
    N_("PSG1 tone1"), N_("PSG1 tone2"), N_("PSG1 tone3"), N_("PSG1 noise")
};

static inline const char **get_channel_names(void) {
    /* Runtime dle g_mzhal (mzhal krok 8); u psg_count == 2 rozhoduje
     * stereo flag (MZ-1500 vzdy, MZ-800 dle allow_psg1). */
    if (g_mzhal.psg_count == 0)
        return channel_names_ctc_only;
    if (g_mzhal.psg_count == 2)
        return g_psg_module.stereo ? channel_names_stereo : channel_names_mono;
    return channel_names_mono;
}

static inline int get_num_channels(void) {
    /* Runtime: u psg_count == 2 je 5 kanalu (CTC0 + PSG0) nebo 9 (+ PSG1). */
    if (g_mzhal.psg_count == 2)
        return g_psg_module.stereo ? 9 : 5;
    return (int)g_mzhal.audio_src_channels;
}

const int NUM_LEDS = 16; // Počet LED indikátorů

// Získání nejvyšší aktuální hlasitosti všech kanálů
float get_max_channel_db()
{
    int num_channels = get_num_channels();
    float maxGain = -60.0f; // Nejnižší hodnota dB
    for (int i = 0; i < num_channels; i++)
    {
        float dbGain = 20.0f * log10(g_iface_audio.gain[i] + 0.0001f);
        if (dbGain > maxGain)
        {
            maxGain = dbGain;
        }
    }
    return maxGain;
}

void imgui_audio(bool *p_open)
{
    if (!*p_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always); // Automatická velikost
    /* Auto-layout při fresh open - cache _L() do lokální proměnné (ID stabilita). */
    const char *audio_title = _L("Audio Mixer");
    auto_layout_first_use_portrait(audio_title, 320.0f, 400.0f);
    ImGui::Begin(audio_title, p_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    const float sliderWidth = 200.0f;                                             // Pevná šířka sliderů
    const float ledSpacing = 2.0f;                                                // Mezera mezi LED
    const float ledSize = (sliderWidth - (NUM_LEDS - 1) * ledSpacing) / NUM_LEDS; // Šířka LED

    // Runtime výběr názvů kanálů a jejich počtu podle stavu PSG modulu
    const char **names = get_channel_names();
    int num_channels = get_num_channels();

    // Výpočet výšky pro globální slider a LED indikátor
    const float singleChannelHeight = 95.0f;
    const float globalSliderHeight = singleChannelHeight * num_channels;
    const float globalLedHeight = (globalSliderHeight / NUM_LEDS) - 2; // Každá LED zabere 1/16 výšky

    // Správná inicializace globálního slideru podle nejvyšší aktuální hlasitosti
    static float globalDbGain = get_max_channel_db();

    // Horizontální slidery a LED bary (hlavní obsah)
    ImGui::BeginGroup();
    for (int i = 0; i < num_channels; i++)
    {
        ImGui::PushID(i);

        // Horizontální slider pro každý kanál
        float dbGain = 20.0f * log10(g_iface_audio.gain[i] + 0.0001f);
        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderFloat(_L(names[i]), &dbGain, -60.0f, 0.0f, _("%.1f dB")))
        {
            g_iface_audio.gain[i] = powf(10.0f, dbGain / 20.0f);
        }

        ImGui::Dummy(ImVec2(0, 5));

        // Horizontalni LED indikátory pro každý kanál
        float level = g_iface_audio.level[i];
        int numActiveLeds = (int)(level * NUM_LEDS);

        ImGui::BeginGroup();
        for (int j = 0; j < NUM_LEDS; j++)
        {
            ImVec4 color;
            if (j < numActiveLeds)
            {
                if (j >= 13)
                    color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Červená (přebuzení)
                else if (j >= 10)
                    color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Oranžová
                else if (j >= 5)
                    color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Žlutá
                else
                    color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Zelená
            }
            else
            {
                color = ImVec4(0.2f, 0.2f, 0.2f, 1.0f); // Šedá (vypnutá)
            }

            GString *led_id = g_string_new(NULL);
            g_string_printf(led_id, "##led_%d_%d", i, j);
            ImGui::ColorButton(led_id->str, color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(ledSize, 15));
            g_string_free(led_id, TRUE);

            if (j < NUM_LEDS - 1)
                ImGui::SameLine(0, ledSpacing);
        }
        ImGui::EndGroup();

        ImGui::NewLine();
        ImGui::PopID();
    }
    ImGui::EndGroup();

    // Globální slider
    ImGui::SameLine();
    ImGui::BeginGroup();

    if (ImGui::VSliderFloat("##Global", ImVec2(30, globalSliderHeight), &globalDbGain, -60.0f, 0.0f, _("%.0f dB")))
    {
        float globalGain = powf(10.0f, globalDbGain / 20.0f);
        for (int i = 0; i < num_channels; i++)
        {
            g_iface_audio.gain[i] = globalGain;
        }
    }

    ImGui::SameLine();

    // LED indikátor pro globální úroveň
    float avgLevel = g_iface_audio.total_level;
    int numActiveLeds = (int)(avgLevel * NUM_LEDS);

    ImGui::BeginGroup();
    for (int j = NUM_LEDS - 1; j >= 0; j--) // LED indikátor vertikálně
    {
        ImVec4 color;
        if (j < numActiveLeds)
        {
            if (j >= 13)
                color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Červená
            else if (j >= 10)
                color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Oranžová
            else if (j >= 5)
                color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Žlutá
            else
                color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Zelená
        }
        else
        {
            color = ImVec4(0.2f, 0.2f, 0.2f, 1.0f); // Šedá (vypnutá)
        }

        GString *global_led_id = g_string_new(NULL);
        g_string_printf(global_led_id, "##global_led_%d", j);
        ImGui::ColorButton(global_led_id->str, color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(30, globalLedHeight - 1));
        g_string_free(global_led_id, TRUE);
    }
    ImGui::EndGroup();

    ImGui::EndGroup();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        *p_open = false;
    };

    ImGui::End();
}