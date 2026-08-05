#include "main.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <glib.h>
#include <string.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "customspeed.h"
#include "iface/iface_video.h"
#include "topmenu.h"

#include "iface/iface_joy.h"

#include "hw-generic/fdc/fdc.h"

#include "hw-generic/pio8255/pio8255.h"

#include "hw-generic/joy/joy.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#define UI_CUSTOMSPEED_SLIDER_MIN 1
#define UI_CUSTOMSPEED_SLIDER_MAX 400


extern "C"
{
    void imgui_customspeed_setup(void);
}

void imgui_customspeed_setup(void)
{
    ImGui::SeparatorText(_("Current Speed"));

    int cpu_speed = customspeed_get_current_speed();
    static int slider_min = UI_CUSTOMSPEED_SLIDER_MIN;
    static int slider_max = UI_CUSTOMSPEED_SLIDER_MAX;

    if (ImGui::SliderInt("Slider MIN", &slider_min, 1, CUSTOMSPEED_MAX_VALUE - 1))
    {
        if (slider_min > cpu_speed)
        {
            customspeed_set_request(slider_min);
            cpu_speed = customspeed_get_current_speed();
        };

        if (slider_min >= slider_max)
        {
            slider_max = slider_min + 1;
        };
    };

    if (ImGui::SliderInt("Slider MAX", &slider_max, 2, CUSTOMSPEED_MAX_VALUE))
    {
        if (slider_max < cpu_speed)
        {
            customspeed_set_request(slider_max);
            cpu_speed = customspeed_get_current_speed();
        };

        if (slider_max <= slider_min)
        {
            slider_min = slider_max - 1;
        };
    };

    if (ImGui::SliderInt("%###cpu_speed_slide", &cpu_speed, slider_min, slider_max))
    {
        customspeed_set_request(cpu_speed);
    };

    if (ImGui::InputInt("%###cpu_speed_input", &cpu_speed, 1, 10))
    {
        customspeed_set_request(cpu_speed);
        cpu_speed = customspeed_get_current_speed();
    };

    if (cpu_speed < slider_min)
    {
        slider_min = cpu_speed;
    };
    if (cpu_speed > slider_max)
    {
        slider_max = cpu_speed;
    };

    if (ImGui::Button(_L("Reset sliders")))
    {
        emulator_switch_to_normal_speed();
        slider_min = UI_CUSTOMSPEED_SLIDER_MIN;
        slider_max = UI_CUSTOMSPEED_SLIDER_MAX;
    };
}
