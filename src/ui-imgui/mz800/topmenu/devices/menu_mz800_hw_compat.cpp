#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"

#include "emulator/mzarch/mz800/mz800_main.h"
#include "emulator/mzarch/mzarch_request.h"

extern "C"
{
    void imgui_menu_mz800_hw_compat(void);
}

void imgui_menu_mz800_hw_compat(void)
{
    if (ImGui::BeginMenu(_L("HW Compatibility Experiments")))
    {
        if (ImGui::MenuItem(_L("Allow PSG1 (stereo)"), NULL, MZ800_TEST_MZ800_HWCOMPAT_ALLOW_PSG1))
        {
            mzarch_request_allow_psg1(!MZ800_TEST_MZ800_HWCOMPAT_ALLOW_PSG1);
        };
        ImGui::SetItemTooltip(_("Enables a second PSG (SN76489), providing stereo sound similar to the MZ-1500. Port 0xF2 is still used as a mono audio PSG (left + right channel).\nPort 0xF3 is only PSG0 (left) and port 0xF9 is only PSG1 (right)."));

        ImGui::EndMenu();
    };
}
