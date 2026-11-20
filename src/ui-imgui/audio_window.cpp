/**
 * @file   audio_window.cpp
 * @brief  Okno Audio Mixer: ztlumení a hlavní hlasitost poslechu, hlasitosti kanálů, indikace úrovní a výstupní propust.
 *
 * Oddíl Output má přepínač Mute a posuvník Master volume: stejné veličiny
 * zvukového rozhraní jako tab Sound overlay menu (iface_audio_set_mute(),
 * iface_audio_set_master_gain()). Násobí hotový mix těsně před zvukovým
 * zařízením, hlasitosti kanálů ani záznam nemění; ztlumení se neukládá.
 * Master volume má rozsah 0 až 200 % se značkou na 100 %: tažení myší se
 * u 100 % přichytí a dvojklik vrátí 100 %.
 * Oddíl Channels má vodorovný posuvník a indikátor úrovně pro každý kanál
 * (g_iface_audio.gain[], level[]) a vpravo svislý posuvník, který všem
 * kanálům zapíše stejnou hlasitost, s indikátorem celkové úrovně
 * (total_level, špička výstupu před hlavní hlasitostí a ztlumením).
 * Indikátory ukazují špičku: kanál hrající naplno s hlasitostí 0 dB
 * rozsvítí všechny diody. Pod nimi je volba věrné výstupní propusti (jen na
 * stroji, který ji má).
 */

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

extern "C"
{
    void imgui_audio(bool *p_open);
};

#if HAVE_PSG == 0
/* MZ-700: pouze CTC0, zadny PSG */
static const char *channel_names_ctc_only[] = {
    N_("CTC0")
};
#endif
#if HAVE_PSG >= 1
/* Mono PSG (MZ-800 nativne, MZ-1500 nikdy) */
static const char *channel_names_mono[] = {
    N_("CTC0"), N_("PSG tone1"), N_("PSG tone2"), N_("PSG tone3"), N_("PSG noise")
};
#endif
#if HAVE_PSG == 2
/* Stereo (MZ-1500 nativne, MZ-800 pres allow_psg1) */
static const char *channel_names_stereo[] = {
    N_("CTC0"),
    N_("PSG0 tone1"), N_("PSG0 tone2"), N_("PSG0 tone3"), N_("PSG0 noise"),
    N_("PSG1 tone1"), N_("PSG1 tone2"), N_("PSG1 tone3"), N_("PSG1 noise")
};
#endif

static inline const char **get_channel_names(void) {
#if HAVE_PSG == 0
    return channel_names_ctc_only;
#elif HAVE_PSG == 2
    /* Runtime rozhodnuti: MZ-1500 vzdy stereo, MZ-800 dle allow_psg1 */
    return g_psg_module.stereo ? channel_names_stereo : channel_names_mono;
#else
    return channel_names_mono;
#endif
}

static inline int get_num_channels(void) {
#if HAVE_PSG == 2
    /* Runtime: 5 kanalu (CTC0 + PSG0) nebo 9 kanalu (+ PSG1) */
    return g_psg_module.stereo ? 9 : 5;
#else
    return AUDIO_SRC_CHANNELS_COUNT;
#endif
}

const int NUM_LEDS = 16; // Počet LED indikátorů

/** @brief Největší hodnota posuvníku Master volume v procentech (AUDIO_MASTER_GAIN_MAX). */
static const float MASTER_PERCENT_MAX = AUDIO_MASTER_GAIN_MAX * 100.0f;
/** @brief Šířka pásma kolem 100 %, ve kterém se tažení posuvníku Master volume myší přichytí na 100 % [procenta]. */
static const float MASTER_DETENT_PERCENT = 4.0f;

/**
 * @brief Počet rozsvícených diod indikátoru pro úroveň.
 * @param level Úroveň 0 až 1 (g_iface_audio.level[], total_level).
 * @return Počet diod 0 až NUM_LEDS, zaokrouhleno na nejbližší (úroveň od 0,97 rozsvítí všechny).
 */
static int level_to_leds(float level)
{
    int n = (int)(level * NUM_LEDS + 0.5f);
    if (n < 0)
        n = 0;
    return (n > NUM_LEDS) ? NUM_LEDS : n;
}

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

/**
 * @brief Vykreslí okno Audio Mixer.
 *
 * Změny se uplatní hned: Mute a Master volume přes zvukové rozhraní
 * (iface_audio_set_mute(), iface_audio_set_master_gain(); stav čte každý
 * snímek, takže souhlasí s tabem Sound overlay menu), posuvníky kanálů
 * zápisem g_iface_audio.gain[], propust přes iface_audio_set_output_filter().
 * Svislý posuvník přepíše hlasitosti všech kanálů jednou hodnotou; jeho
 * poloha se po otevření okna sama neobnovuje podle kanálů (výchozí hodnotu
 * bere jen při prvním vykreslení v běhu programu). Dvojklik na Master volume
 * nastaví 100 % a drží je do puštění tlačítka myši. Esc při fokusu okno zavře.
 *
 * @param p_open Příznak zobrazení okna; false = nic se nekreslí. Zavření okna ho shodí.
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render(); p_open není NULL.
 */
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

    /* Výstup: ztlumení a hlavní hlasitost poslechu (stejné veličiny jako tab Sound overlay menu) */
    ImGui::SeparatorText(_("Output"));
    bool mute = iface_audio_get_mute();
    if (ImGui::Checkbox(_L("Mute"), &mute))
    {
        iface_audio_set_mute(mute);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", _("Silences the sound output; the volumes are kept. Not saved: the emulator always starts unmuted."));
    }
    /* dvojklik na Master volume proběhl a tlačítko myši je ještě stisknuté */
    static bool masterResetHold = false;
    float masterPercent = floorf(iface_audio_get_master_gain() * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat(_L("Master volume"), &masterPercent, 0.0f, MASTER_PERCENT_MAX, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
    {
        /* tažení myší se u 100 % přichytí (klávesnice a zadání čísla krokují volně) */
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && fabsf(masterPercent - 100.0f) <= MASTER_DETENT_PERCENT)
            masterPercent = 100.0f;
        iface_audio_set_master_gain(masterPercent / 100.0f);
    }
    {
        /* značka 100 % na drážce posuvníku (vnitřní okraj drážky jako ImGui::SliderBehavior()) */
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        const float grab = ImGui::GetStyle().GrabMinSize;
        const float x0 = rmin.x + 2.0f + grab * 0.5f;
        const float x1 = rmin.x + sliderWidth - 2.0f - grab * 0.5f;
        const float mx = floorf(x0 + (x1 - x0) * (100.0f / MASTER_PERCENT_MAX)) + 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mx, rmax.y - 5.0f), ImVec2(mx, rmax.y), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    }
    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            /* dvojklik: zpět na 100 % */
            masterResetHold = true;
        }
        ImGui::SetTooltip("%s\n%s", _("Listening volume of the whole mix. The channel volumes and recordings are not affected."),
                          _("The range is 0 to 200 %; values above 100 amplify the sound. Double-click to reset to 100."));
    }

    if (masterResetHold)
    {
        /* po dvojkliku drží 100 %, dokud uživatel tlačítko nepustí (posuvník je druhým stiskem ještě aktivní) */
        iface_audio_set_master_gain(1.0f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            masterResetHold = false;
    }

    /* Kanály: vodorovné posuvníky a úrovně, vpravo svislý posuvník pro všechny kanály najednou */
    ImGui::SeparatorText(_("Channels"));

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
        /* špička kanálu: kanál naplno s hlasitostí 0 dB rozsvítí všechny diody */
        int numActiveLeds = level_to_leds(g_iface_audio.level[i]);

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
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", _("All channels: sets every channel to the same volume."));
    }

    ImGui::SameLine();

    // LED indikátor pro globální úroveň
    /* špička výstupu za omezovačem, před hlavní hlasitostí a ztlumením */
    int numActiveLeds = level_to_leds(g_iface_audio.total_level);

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
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", _("Level of the mix before the master volume and mute."));
    }

    ImGui::EndGroup();

#if AUDIO_HAVE_OUTPUT_FILTER
    /* Věrná výstupní propust (INI [AUDIO] output_filter); přepnutí platí hned.
     * Samostatný MZ-700 propust nemá, volba se tam nenabízí. */
    bool output_filter = iface_audio_output_filter_enabled();
    if (ImGui::Checkbox(_L("Hardware output filter"), &output_filter))
    {
        iface_audio_set_output_filter(output_filter);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", _("Low-pass filter of the real MZ-800 audio output (346 Hz). Off = the previous, brighter sound."));
    }
#endif /* AUDIO_HAVE_OUTPUT_FILTER */

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        *p_open = false;
    };

    ImGui::End();
}