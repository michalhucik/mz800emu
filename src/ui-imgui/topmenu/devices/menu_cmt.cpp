#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"

#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "libs/cmtspeed/cmtspeed.h"

extern "C"
{
    void imgui_menu_cmt(void);
}

void imgui_menu_cmt(void)
{
    if (ImGui::BeginMenu(_L("CMT")))
    {
        /*
         *
         * Submenu CMT
         *
         */

        if (ImGui::MenuItem(_L("Use CMT Hack"), NULL, CMTHACK_TEST_IS_INSTALLED))
        {
            cmt_request_set_hack(!CMTHACK_TEST_IS_INSTALLED);
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("Set Default CMT MZ Speed"), CMT_TEST_STOP))
        {
            if (MenuRadioItem(_("1:1 - 1200 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_1_1)))
            {
                cmt_request_set_speed(CMTSPEED_1_1);
            };

            if (MenuRadioItem(_("2:1 - 2 400 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_2_1)))
            {
                cmt_request_set_speed(CMTSPEED_2_1);
            };

            if (MenuRadioItem(_("2:1 - 2 400 Bd (cp/m)"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_2_1_CPM)))
            {
                cmt_request_set_speed(CMTSPEED_2_1_CPM);
            };

            if (MenuRadioItem(_("7:3 - 2 800 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_7_3)))
            {
                cmt_request_set_speed(CMTSPEED_7_3);
            };

            if (MenuRadioItem(_("8:3 - 3 200 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_8_3)))
            {
                cmt_request_set_speed(CMTSPEED_8_3);
            };

            if (MenuRadioItem(_("3:1 - 3 600 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_3_1)))
            {
                cmt_request_set_speed(CMTSPEED_3_1);
            };

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Fast Open CMT File & Play..."), NULL, false))
        {
            cmt_ui_open(true);
        };

        if (ImGui::MenuItem(_L("Show Virtual CMT"), "Alt+C", g_gui->showVirtualCmtWindow))
        {
            g_gui->showVirtualCmtWindow = !g_gui->showVirtualCmtWindow;
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("CMT Options")))
        {
            if (ImGui::MenuItem(_L("Enable CPU Boost"), NULL, CMT_TEST_CPU_BOOST_ENABLED))
            {
                cmt_cpu_boost_set(CMT_TEST_CPU_BOOST_ENABLED ? CMT_CPU_BOOST_DISABLED : CMT_CPU_BOOST_ENABLED);
            };

            ImGui::SeparatorText(_("MZF"));

            if (ImGui::MenuItem(_L("Enable MZF Size Check"), NULL, CMT_TEST_MZF_SIZE_CHECK_ENABLED))
            {
                cmt_mzfsize_check_set((CMT_TEST_MZF_SIZE_CHECK_ENABLED) ? CMT_MZFSIZE_CHECK_DISABLED : CMT_MZFSIZE_CHECK_ENABLED);
            };

            if (ImGui::MenuItem(_L("Fix FNAME Terminator"), NULL, CMTHACK_TEST_FIX_FNAME_TERMINATOR))
            {
                cmt_request_set_fix_fname_terminator(!CMTHACK_TEST_FIX_FNAME_TERMINATOR);
            };

            ImGui::EndMenu();
        };

        ImGui::EndMenu();
    };
}
