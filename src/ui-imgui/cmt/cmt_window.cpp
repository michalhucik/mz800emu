#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <string>

// Lokalizace
#include "i18n.h"

#include "ui-imgui/filechooser/res/CustomFont.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/cmt/cmt_mzf.h"
#include "hw-generic/cmt/cmt_tap.h"

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/cmtspeed/cmtspeed.h"

#include "CmtGlyphRenderer.h"
#include "imgui_cmt.h"
#include "ImGuiCmt.hpp"

extern "C"
{
    void imgui_cmt(bool *p_open);
};

typedef enum
{
    CMT_DISPLAY_REMAIN_TIME = 0,
    CMT_DISPLAY_PLAY_TIME,
} cmt_display_t;

cmt_display_t g_ui_cmt_timeinfo = CMT_DISPLAY_REMAIN_TIME;

typedef struct ui_cmt_t
{
    double total_time;
    double play_time;
} ui_cmt_t;

ui_cmt_t g_ui_cmt;

void imgui_cmt_row0_time_label_and_boost_switch(void)
{
    if (ImGui::BeginTable("TableLayout", 4, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Spacer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("CmtHack", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("CpuBoost", ImGuiTableColumnFlags_WidthFixed);

        // První sloupec (Label)
        ImGui::TableNextColumn();
        if (g_ui_cmt_timeinfo == CMT_DISPLAY_PLAY_TIME)
        {
            ImGui::Text(_("Playing time:"));
        }
        else
        {
            ImGui::Text(_("Remaining time:"));
        };

        // Druhý sloupec (Expandující mezera)
        ImGui::TableNextColumn();
        // Prázdná buňka -> expandující prostor

        // Třetí sloupec - CMT Hack checkbox (disabled pokud CMT není
        // v klidovém stavu = STOP). Aktivace bezpečná jen v STOP.
        ImGui::TableNextColumn();
        bool ui_cmt_hack = CMTHACK_TEST_IS_INSTALLED;
        ImGui::BeginDisabled(!CMT_TEST_STOP);
        if (ImGui::Checkbox(_L("CMT Hack"), &ui_cmt_hack))
        {
            cmthack_load_rom_patch(!CMTHACK_TEST_IS_INSTALLED);
        };
        ImGui::EndDisabled();

        // Čtvrtý sloupec - CPU Boost checkbox (původní)
        ImGui::TableNextColumn();
        bool ui_cmt_cpuBoost = CMT_TEST_CPU_BOOST_ENABLED;
        if (ImGui::Checkbox(_L("CPU Boost"), &ui_cmt_cpuBoost))
        {
            cmt_cpu_boost_set(CMT_TEST_CPU_BOOST_ENABLED ? CMT_CPU_BOOST_DISABLED : CMT_CPU_BOOST_ENABLED);
        };

        ImGui::EndTable();
    };
}

void imgui_cmt_row1_time_display(void)
{
    uint32_t print_time = g_ui_cmt.play_time;
    bool show_minus = false;

    if (g_ui_cmt_timeinfo == CMT_DISPLAY_REMAIN_TIME)
    {
        print_time = g_ui_cmt.total_time - g_ui_cmt.play_time;
        if ((g_ui_cmt.total_time - g_ui_cmt.play_time) > print_time)
        {
            print_time++;
        };
        show_minus = true;
    };

    GString *str = g_string_new("");
    g_string_append_printf(str, "%c%02d:%02d", show_minus ? '-' : ' ', print_time / 60, print_time % 60);

    ImGui::Spacing();

    ImGui::SetWindowFontScale(5.0f);

    // Zarovnání textu na střed
    float textWidth = ImGui::CalcTextSize(str->str).x;
    float windowWidth = ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
    ImGui::Text("%s", str->str);
    ImGui::SetItemTooltip(_("Click here to change the time info mode."));
    if (ImGui::IsItemClicked())
    {
        g_ui_cmt_timeinfo = (g_ui_cmt_timeinfo == CMT_DISPLAY_REMAIN_TIME) ? CMT_DISPLAY_PLAY_TIME : CMT_DISPLAY_REMAIN_TIME;
    };
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Spacing();

    g_string_free(str, TRUE);
}

void imgui_cmt_stream_info(st_CMTEXT_BLOCK *block, char **value_stream, char **value_rate, char **value_size)
{
    if (!block)
    {
        return;
    };

    en_CMTEXT_BLOCK_TYPE block_type = cmtext_block_get_type(g_cmt.ext->block);

    if ((block_type != CMTEXT_BLOCK_TYPE_WAV) || (!cmtext_block_get_stream(block)))
    {
        return;
    };

    en_CMT_STREAM_TYPE stream_type = cmtext_block_get_stream_type(block);
    if (stream_type == CMT_STREAM_TYPE_BITSTREAM)
    {
        *value_stream = g_strdup_printf("bitstream");
    }
    else if (stream_type == CMT_STREAM_TYPE_VSTREAM)
    {
        *value_stream = g_strdup_printf("vstream");
    }
    else
    {
        fprintf(stderr, "%s():%d - Unknown cmt stream type '%d'\n", __func__, __LINE__, stream_type);
    };

    uint32_t rate = cmtext_block_get_rate(block);
    if (rate < 1000000)
    {
        *value_rate = g_strdup_printf("%0.2f kHz", ((float)rate / 1000));
    }
    else
    {
        *value_rate = g_strdup_printf("%0.2f MHz", ((float)rate / 1000000));
    };

    uint32_t size = cmtext_block_get_size(block);

    if (size)
    {
        if (size < 1024)
        {
            *value_size = g_strdup_printf("%d B", size);
        }
        else if (size < (1024 * 1024))
        {
            *value_size = g_strdup_printf("%0.2f kB", ((float)size / 1024));
        }
        else
        {
            *value_size = g_strdup_printf("%0.2f MB", ((float)size / (1024 * 1024)));
        };
    };
}

/*
 * LEP/L16 jsou edge/timebase formáty. Interně zůstávají reprezentované jako
 * VSTREAM s rate 20 kHz (LEP) / 62.5 kHz (L16), protože to přesně odpovídá
 * jednotce 50 us / 16 us. V UI ale tento údaj není "sample rate" jako u WAV.
 *
 * Vrací "LEP" nebo "L16" a příslušný timebase v mikrosekundách.
 * Pro ostatní formáty vrací NULL.
 */
static const char *imgui_cmt_edge_format(unsigned *timebase_us)
{
    if (timebase_us)
        *timebase_us = 0;

    if (!g_cmt.ext)
        return NULL;

    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    if (!container)
        return NULL;

    const char *filepath = cmtext_container_get_filepath(container);
    if (!filepath)
        return NULL;

    const char *ext = cmtext_get_filename_extension(filepath);
    if (!ext)
        return NULL;

    if (g_ascii_strcasecmp(ext, "lep") == 0)
    {
        if (timebase_us)
            *timebase_us = 50;
        return "LEP";
    }

    if (g_ascii_strcasecmp(ext, "l16") == 0)
    {
        if (timebase_us)
            *timebase_us = 16;
        return "L16";
    }

    return NULL;
}

void imgui_tape_info_table(void)
{
    if (ImGui::BeginTable("TapeInfoTable", 7, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchProp))
    {
        // Nastavení sloupců
        ImGui::TableSetupColumn("Label1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Separator1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Spacer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Label2", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Separator2", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value2", ImGuiTableColumnFlags_WidthFixed);

        // Data tabulky
        const char *labels[][2] = {
            {_("Block"), _("of")},
            {_("Type"), _("FSize")},
            {_("Name"), _("FExec")},
            {_("Speed"), _("FStart")},
            {_("Stream"), ""},
            {_("Rate"), _("SSize")}};

        char default_value[] = "--     ";
        char *values[6][2] = {0};
        for (int i = 0; i < 6; i++)
        {
            values[i][0] = default_value;
            values[i][1] = default_value;
        };

        if (CMT_TEST_FILLED)
        {
            st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
            int total_blocks = cmtext_container_get_count_blocks(container);
            int play_block = cmtext_block_get_block_id(g_cmt.ext->block) + 1;

            // played block
            values[0][0] = g_strdup_printf("%02d", play_block);
            // total blocks
            values[0][1] = g_strdup_printf("%02d", total_blocks);

            unsigned edge_timebase_us = 0;
            const char *edge_format = imgui_cmt_edge_format(&edge_timebase_us);

            if ((EXIT_SUCCESS == cmtext_is_playable(g_cmt.ext)) && (g_cmt.playsts != CMTEXT_BLOCK_PLAYSTS_PAUSE))
            {
                char buff[100];
                uint16_t file_bdspeed = 0;
                st_MZF_HEADER *mzfhdr = NULL;
                st_CMTEXT_TAPE_ITEM_TAPHDR *taphdr = NULL;
                st_CMTEXT_TAPE_ITEM_TAPDATA *tapdata = NULL;

                switch (cmtext_block_get_type(g_cmt.ext->block))
                {
                case CMTEXT_BLOCK_TYPE_MZF:
                    file_bdspeed = (!g_cmt.ext->block->cb_get_bdspeed) ? 0 : g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext);
                    mzfhdr = cmtmzf_block_get_spec_mzfheader(g_cmt.ext->block);
                    g_assert(mzfhdr != NULL);

                    // filetype
                    values[1][0] = g_strdup_printf("0x%02x", mzfhdr->ftype);

                    mzf_tools_get_fname(mzfhdr, (char *)&buff);
                    // filename
                    values[2][0] = g_strdup_printf("%s", buff);

                    // file size
                    values[1][1] = g_strdup_printf("0x%04x", mzfhdr->fsize);
                    // file exec
                    values[2][1] = g_strdup_printf("0x%04x", mzfhdr->fexec);
                    // file start
                    values[3][1] = g_strdup_printf("0x%04x", mzfhdr->fstrt);

                    // speed
                    values[3][0] = g_strdup_printf("%d Bd", file_bdspeed);
                    break;

                case CMTEXT_BLOCK_TYPE_WAV:
                    // LEP/L16 používají pro playback stejný generic stream block
                    // jako WAV, ale v UI zobrazíme skutečný formát.
                    values[1][0] = g_strdup_printf("%s", edge_format ? edge_format : "WAV");
                    break;

                case CMTEXT_BLOCK_TYPE_TAPHEADER:
                    file_bdspeed = (!g_cmt.ext->block->cb_get_bdspeed) ? 0 : g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext);
                    taphdr = cmttap_block_get_spec_tapheader(g_cmt.ext->block);
                    g_assert(taphdr != NULL);

                    // filetype
                    values[1][0] = g_strdup_printf("0x%02x", taphdr->code);

                    // filename
                    values[2][0] = g_strdup_printf("%s", taphdr->fname);

                    // speed
                    values[3][0] = g_strdup_printf("%d Bd", file_bdspeed);
                    break;

                case CMTEXT_BLOCK_TYPE_TAPDATA:
                    file_bdspeed = (!g_cmt.ext->block->cb_get_bdspeed) ? 0 : g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext);
                    tapdata = cmttap_block_get_spec_tapdata(g_cmt.ext->block);
                    g_assert(tapdata != NULL);

                    // file size
                    values[1][1] = g_strdup_printf("0x%04x", tapdata->size - 2);

                    // speed
                    values[3][0] = g_strdup_printf("%d Bd", file_bdspeed);
                    break;

                default:
                    fprintf(stderr, "%s():%d - Unknown cmtext block type '%d'\n", __func__, __LINE__, cmtext_block_get_type(g_cmt.ext->block));
                };

                imgui_cmt_stream_info(g_cmt.ext->block, &values[4][0], &values[5][0], &values[5][1]);

                if (edge_format)
                {
                    labels[5][0] = _("Timebase");

                    if ((values[5][0] != NULL) && (values[5][0] != default_value))
                    {
                        g_free(values[5][0]);
                    }
                    values[5][0] = g_strdup_printf("%u \xC2\xB5s", edge_timebase_us);
                }
            }
            else if (EXIT_SUCCESS == cmtext_is_recordable(g_cmt.ext))
            {
                // U LEP/L16 zobrazit konkrétní formát místo obecného SAVE.
                values[1][0] = g_strdup_printf("%s", edge_format ? edge_format : "SAVE");
                imgui_cmt_stream_info(g_cmt.ext->block, &values[4][0], &values[5][0], &values[5][1]);

                if (edge_format)
                {
                    labels[5][0] = _("Timebase");

                    if ((values[5][0] != NULL) && (values[5][0] != default_value))
                    {
                        g_free(values[5][0]);
                    }
                    values[5][0] = g_strdup_printf("%u \xC2\xB5s", edge_timebase_us);
                }
            };
        };

        // Vykreslení tabulky
        for (int i = 0; i < 6; i++)
        {
            ImGui::TableNextRow();

            // Label (levý)
            ImGui::TableNextColumn();
            ImGui::Text("%s", labels[i][0]);

            // Dvojtečka
            ImGui::TableNextColumn();
            ImGui::Text(":");

            // Hodnota (levá)
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255)); // Zelená
            ImGui::SetNextItemWidth(28.0f * 16);                            // min 16 znaků
            ImGui::Text("%s", values[i][0]);
            ImGui::PopStyleColor();

            // Spacer
            ImGui::TableNextColumn();

            // Label (pravý)
            ImGui::TableNextColumn();
            ImGui::Text("%s", labels[i][1]);

            // Dvojtečka (pokud existuje)
            ImGui::TableNextColumn();
            if (labels[i][1][0] != '\0')
                ImGui::Text(":");

            // Hodnota (pravá)
            ImGui::TableNextColumn();
            if (labels[i][1][0] != '\0')
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255)); // Zelená
                ImGui::SetNextItemWidth(28.0f * 4);                             // min 4 znaky
                ImGui::Text("%s", values[i][1]);
                ImGui::PopStyleColor();
            };
        };
        ImGui::EndTable();

        for (int i = 0; i < 6; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                if ((values[i][j] != default_value) && (values[i][j] != NULL))
                {
                    g_free(values[i][j]);
                };
            };
        };
    };
}

void imgui_cmt_row2_tape_info(void)
{
    if (ImGui::BeginTable("TwoColumnTable", 2, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit))
    {
        bool btn_tapeindex_enambled = false;
        if (CMT_TEST_FILLED)
        {
            st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
            int total_blocks = cmtext_container_get_count_blocks(container);
            if (total_blocks > 1)
            {
                btn_tapeindex_enambled = true;
            };
        };

        ImGui::TableSetupColumn("Column 1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Column 2");

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        imgui_tape_info_table();

        ImGui::TableNextColumn();

        ImGui::BeginDisabled(!btn_tapeindex_enambled);
        if (ImGui::Button(ICON_IGFD_FILE_LIST_THUMBNAILS))
        {
            g_gui->showVirtualCmtTapeIndexWindow = !g_gui->showVirtualCmtTapeIndexWindow;
        };
        ImGui::EndDisabled();

        ImGui::EndTable();
    };
}

void imgui_cmt_row4_combo_mzspeed(void)
{
    bool is_available_default_mz_speed = (CMT_TEST_FILLED && (cmtext_block_get_block_speed(g_cmt.ext->block) != CMTEXT_BLOCK_SPEED_DEFAULT)) ? false : true;

    if (!is_available_default_mz_speed)
    {
        ImGui::Text(" ");
        return;
    };

    std::vector<ImGuiCmtSpeedList_t> speed_list = ImGuiCmt::getMzSpeedList();

    int ui_speed_id = 0;
    for (size_t n = 0; n < speed_list.size(); n++)
    {
        if (CMT_TEST_MZ_CMTSPEED(speed_list[n].cmtspeed))
        {
            ui_speed_id = static_cast<int>(n);
            break;
        };
    };

    bool is_dissabled = (!CMT_TEST_STOP);

    if (is_dissabled)
    {
        ImGui::BeginDisabled();
    };

    ImGui::Text(_("Default MZ Speed:"));
    ImGui::SameLine();
    ImGui::Text(" ");
    ImGui::SameLine();

    // Nastavení maximální dostupné šířky pro Combo box
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

    if (ImGui::BeginCombo("##SpeedSelect", speed_list[ui_speed_id].label_txt.c_str(), 0))
    {
        for (size_t n = 0; n < speed_list.size(); n++)
        {
            const bool is_selected = (ui_speed_id == static_cast<int>(n));
            if (ImGui::Selectable(speed_list[n].label_txt.c_str(), is_selected))
            {
                ui_speed_id = n;
                cmt_change_speed(speed_list[n].cmtspeed);
            };

            if (is_selected)
                ImGui::SetItemDefaultFocus();
        };
        ImGui::EndCombo();
    };

    if (is_dissabled)
    {
        ImGui::EndDisabled();
    };
}

void imgui_cmt_button_previous_action(void)
{
    assert(g_cmt.ext->container->cb_previous_block);
    if (!g_cmt.ext->container->cb_previous_block)
        return;

    gboolean fl_play = CMT_TEST_PLAY;
    gboolean fl_paused = CMT_TEST_PAUSED;

    if (fl_play)
        cmt_stop();

    if (EXIT_FAILURE == g_cmt.ext->container->cb_previous_block())
        return;

    if (fl_play)
    {
        if (fl_paused)
        {
            cmt_play_paused();
        }
        else
        {
            cmt_play();
        };
    };

    // Aktualizace seznamu souborů se musi zavolat z UI
    imgui_cmt_tape_update_filelist();
}

void imgui_cmt_button_next_action(void)
{
    assert(g_cmt.ext->container->cb_next_block);
    if (!g_cmt.ext->container->cb_next_block)
        return;

    gboolean fl_play = CMT_TEST_PLAY;
    gboolean fl_paused = CMT_TEST_PAUSED;

    if (fl_play)
        cmt_stop();

    if (EXIT_FAILURE == g_cmt.ext->container->cb_next_block())
        return;

    if (fl_play)
    {
        if (fl_paused)
        {
            cmt_play_paused();
        }
        else
        {
            cmt_play();
        };
    };

    // Aktualizace seznamu souborů se musi zavolat z UI
    imgui_cmt_tape_update_filelist();
}

// Konstantní velikost tlačítek
const ImVec2 CMT_BUTTON_SIZE(100, 50);
const float CMT_SEPARATOR_WIDTH = 10.0f;    // Pevná šířka sloupce se separátorem
const float CMT_SIDE_BUTTON_WIDTH = 110.0f; // Pevná šířka třetího sloupce

void imgui_cmt_main_controls(CmtGlyphRenderer renderer)
{
    if (ImGui::BeginTable("MainControls", 4, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchSame))
    {
        // První řada tlačítek
        ImGui::TableNextRow();

        bool btn_previous_enabled = false;
        bool btn_next_enabled = false;

        if (CMT_TEST_FILLED)
        {
            st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
            int total_blocks = cmtext_container_get_count_blocks(container);
            int play_block = cmtext_block_get_block_id(g_cmt.ext->block);

            if (play_block > 0)
            {
                btn_previous_enabled = true;
            };

            if (play_block < (total_blocks - 1))
            {
                btn_next_enabled = true;
            };
        };

        bool btn_backward_enabled = false;
        bool btn_forward_enabled = false;

        // PREVIOUS
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_previous_enabled);
        if (ImGui::Button("##cmt_btn_previous", CMT_BUTTON_SIZE))
        {
            imgui_cmt_button_previous_action();
        };
        renderer.drawPrevious();
        ImGui::EndDisabled();

        // BACKWARD
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_backward_enabled);
        if (ImGui::Button("##cmt_btn_backward", CMT_BUTTON_SIZE))
        {
            g_print("Button backward clicked\n");
        };
        renderer.drawBackward();
        ImGui::EndDisabled();

        // FORWARD
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_forward_enabled);
        if (ImGui::Button("##cmt_btn_forward", CMT_BUTTON_SIZE))
        {
            g_print("Button forward clicked\n");
        };
        renderer.drawForward();
        ImGui::EndDisabled();

        // NEXT
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_next_enabled);
        if (ImGui::Button("##cmt_btn_next", CMT_BUTTON_SIZE))
        {
            imgui_cmt_button_next_action();
        };
        renderer.drawNext();
        ImGui::EndDisabled();

        // Druhá řada tlačítek
        ImGui::TableNextRow();

        bool btn_stop_enabled = (CMT_TEST_FILLED && !CMT_TEST_STOP);
        bool btn_record_enabled = (!CMT_TEST_FILLED && !CMT_TEST_RECORD);
        bool btn_play_enabled = (CMT_TEST_FILLED && CMT_TEST_STOP);
        bool btn_pause_enabled = CMT_TEST_FILLED;

        // STOP
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_stop_enabled);
        if (ImGui::Button("##cmt_btn_stop", CMT_BUTTON_SIZE))
        {
            if (CMT_TEST_RECORD)
            {
                st_CMTEXT_BLOCK *block = cmtext_get_block(g_cmt.ext);
                uint32_t size = cmtext_block_get_size(block);
                if (!size)
                {
                    cmt_eject();
                }
                else
                {
                    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
                    const char *fpath = cmtext_container_get_filepath(container);
                    char *filepath = g_strdup(fpath);
                    cmt_eject();
                    cmt_open_file_by_extension(filepath);
                    g_free(filepath);
                };
            }
            else
            {
                cmt_stop();
            };
        };
        renderer.drawStop();
        ImGui::EndDisabled();

        // RECORD
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_record_enabled);
        if (ImGui::Button("##cmt_btn_record", CMT_BUTTON_SIZE))
        {
            cmt_ui_record();
        };
        renderer.drawRecord(CMT_TEST_RECORD);
        ImGui::EndDisabled();

        // PLAY
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_play_enabled);
        if (ImGui::Button("##cmt_btn_play", CMT_BUTTON_SIZE))
        {
            cmt_play();
        };
        renderer.drawPlay(CMT_TEST_PLAY);
        ImGui::EndDisabled();

        // PAUSE
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!btn_pause_enabled);
        if (ImGui::Button("##cmt_btn_pause", CMT_BUTTON_SIZE))
        {
            cmt_pause(!CMT_TEST_PAUSED);
        };
        renderer.drawPause(CMT_TEST_PAUSED);
        ImGui::EndDisabled();

        ImGui::EndTable();
    };
}

// Druhá tabulka s 2 řadami a 1 tlačítkem (zarovnaná vpravo)
void imgui_cmt_side_controls(CmtGlyphRenderer renderer)
{
    float availableWidth = ImGui::GetContentRegionAvail().x;
    float buttonOffset = availableWidth - CMT_BUTTON_SIZE.x; // Zarovnání doprava

    if (ImGui::BeginTable("SideControls", 1, ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchSame))
    {
        bool btn_open_enabled = CMT_TEST_STOP;
        bool btn_eject_enabled = (CMT_TEST_FILLED && CMT_TEST_STOP);

        // První řada - 1 tlačítko (zarovnané vpravo)
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + buttonOffset);
        ImGui::BeginDisabled(!btn_open_enabled);
        if (ImGui::Button(ICON_IGFD_FOLDER_OPEN, CMT_BUTTON_SIZE))
        {
            cmt_ui_open(false);
        };
        ImGui::EndDisabled();

        // Druhá řada - 1 tlačítko (zarovnané vpravo)
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + buttonOffset);
        ImGui::BeginDisabled(!btn_eject_enabled);
        if (ImGui::Button("##cmt_btn_eject", CMT_BUTTON_SIZE))
        {
            cmt_eject();
        };
        renderer.drawEject();
        ImGui::EndDisabled();

        ImGui::EndTable();
    };
}

void draw_vertical_separator()
{
    ImVec2 p1 = ImGui::GetCursorScreenPos();
    float separatorX = p1.x + (CMT_SEPARATOR_WIDTH * 0.5f);               // Centrovaný separator v pevném sloupci
    ImVec2 p2 = ImVec2(separatorX, p1.y + 2 * CMT_BUTTON_SIZE.y + 10.0f); // Výška separátoru podle tlačítek
    ImGui::GetWindowDrawList()->AddLine(ImVec2(separatorX, p1.y), p2, IM_COL32(200, 200, 200, 255), 2.0f);
}

void imgui_cmt_row5_buttons_layout()
{
    CmtGlyphRenderer renderer(30.0f, 8.0f);

    if (ImGui::BeginTable("MainLayout", 3, ImGuiTableFlags_NoBordersInBody))
    {
        // 1. Sloupec: Tabulka s hlavními tlačítky (EXPANDUJE)
        ImGui::TableSetupColumn("MainButtons", ImGuiTableColumnFlags_WidthStretch);
        // 2. Sloupec: Pevná šířka separátoru
        ImGui::TableSetupColumn("Separator", ImGuiTableColumnFlags_WidthFixed, CMT_SEPARATOR_WIDTH);
        // 3. Sloupec: Pevná šířka tlačítek na pravé straně
        ImGui::TableSetupColumn("SideButtons", ImGuiTableColumnFlags_WidthFixed, CMT_SIDE_BUTTON_WIDTH);

        ImGui::TableNextRow();

        // **1. Sloupec: Tabulka s hlavními tlačítky (expanduje)**
        ImGui::TableNextColumn();
        imgui_cmt_main_controls(renderer);

        // **2. Sloupec: Centrovaný vertikální separátor**
        ImGui::TableNextColumn();
        draw_vertical_separator();

        // **3. Sloupec: Tabulka s vedlejšími tlačítky (pevná šířka, zarovnaná vpravo)**
        ImGui::TableNextColumn();
        imgui_cmt_side_controls(renderer);

        ImGui::EndTable();
    };
}

void imgui_cmt(bool *p_open)
{
    /* Focus-on-open: detekce false -> true transition okna. Při otevření
     * (uživatel zapnul Virtual CMT v menu) zavoláme SetNextWindowFocus jen
     * jednou, aby se okno vykreslilo nad ostatními (jinak by ho multi-
     * viewport docking mohl podstrčit pod main). V dalších framech nic
     * nepřebíjíme = uživatel může kliknout na jiné okno normálně. */
    static bool s_prev_open = false;
    bool focus_on_open = (*p_open && !s_prev_open);
    s_prev_open = *p_open;

    if (!*p_open)
        return;

    if (CMTHACK_TEST_IS_INSTALLED)
    {
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_AlwaysAutoResize | // Okno se automaticky přizpůsobí obsahu
            ImGuiWindowFlags_NoResize |         // Zakáže změnu velikosti
            ImGuiWindowFlags_NoScrollbar |      // Zakáže scrollbar
            ImGuiWindowFlags_NoCollapse |       // Zakáže možnost sbalení okna
            ImGuiWindowFlags_NoDocking;         // Zakáže možnost dockování

        /* okno s jiným ID než normální "Virtual CMT" — oddělené chování */
        {
            char title[256];
            snprintf(title, sizeof(title), "%s##cmthack_is_active", _("Virtual CMT"));

            if (focus_on_open)
                ImGui::SetNextWindowFocus();
            /* Auto-layout při fresh open (cmthack varianta okna). */
            auto_layout_first_use_portrait(title, 400.0f, 500.0f);
            ImGui::Begin(title, p_open, flags);
        }

        // CMT Hack checkbox vpravo nahoře - umožní vypnutí v hack-active
        // variantě okna (= bez restrikcí, CMT v hack modu nemá normální
        // STOP/PLAY/REC state). Vlevo od checkboxu spacer pro zarovnání
        // doprava.
        {
            ImGui::BeginTable("HackHeaderLayout", 2,
                              ImGuiTableFlags_NoBordersInBody
                              | ImGuiTableFlags_SizingStretchProp);
            ImGui::TableSetupColumn("Spacer",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Checkbox",
                                    ImGuiTableColumnFlags_WidthFixed);

            ImGui::TableNextColumn();
            /* expandující mezera */
            ImGui::TableNextColumn();
            bool ui_cmt_hack = CMTHACK_TEST_IS_INSTALLED;
            if (ImGui::Checkbox(_L("CMT Hack"), &ui_cmt_hack))
            {
                cmthack_load_rom_patch(!CMTHACK_TEST_IS_INSTALLED);
            };
            ImGui::EndTable();
        };

        // Text zarovnany na stred
        ImGui::SetWindowFontScale(2.5f);
        ImGui::Text(_("CMT Hack is activated!"));
        ImGui::Text(_("Virtual CMT is not available!"));
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Separator();

        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - 80);
        if (ImGui::Button(_L("Close"), ImVec2(80, 0)))
        {
            *p_open = false;
        };

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            *p_open = false;
        };

        ImGui::End();
        return;
    };

    g_ui_cmt.total_time = 0;
    g_ui_cmt.play_time = 0;
    float progress_playtime = 0.0f;

    GString *str_status = g_string_new("");
    if (!CMT_TEST_FILLED)
    {
        g_string_append(str_status, _("No tape loaded"));
    }
    else
    {
        if (CMT_TEST_RECORD)
        {
            g_ui_cmt.total_time = (99 * 60);
            g_ui_cmt.play_time = cmt_get_playtime();
            progress_playtime = (float)(g_ui_cmt.play_time / g_ui_cmt.total_time);
            g_string_append(str_status, _("Recording"));
        }
        else
        {
            double scan_time = cmtext_block_get_scantime(g_cmt.ext->block);
            uint64_t scans = cmtext_block_get_count_scans(g_cmt.ext->block);
            gdouble body_total_time = (scan_time * scans);

            if (CMT_TEST_PLAY)
            {
                g_string_append(str_status, _("Playing"));
                if (g_cmt.playsts == CMTEXT_BLOCK_PLAYSTS_PAUSE)
                {
                    g_ui_cmt.total_time = 0.001 * cmtext_block_get_pause_after(g_cmt.ext->block);
                    g_ui_cmt.play_time = cmt_get_playtime() - body_total_time;
                }
                else
                {
                    g_ui_cmt.total_time = body_total_time;
                    g_ui_cmt.play_time = cmt_get_playtime();
                };
                progress_playtime = (float)(g_ui_cmt.play_time / g_ui_cmt.total_time);
            }
            else
            {
                g_string_append(str_status, _("Stopped - Ready to Play"));
                g_ui_cmt.total_time = body_total_time;
            };
        };

        if (CMT_TEST_PAUSED)
        {
            g_string_append_printf(str_status, " %s", _("(paused)"));
        };
    };

    ImGui::SetNextWindowSize(ImVec2(556, 674), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;

    if (focus_on_open)
        ImGui::SetNextWindowFocus();
    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *cmt_title = _L("Virtual CMT");
    auto_layout_first_use_portrait(cmt_title, 500.0f, 300.0f);
    ImGui::Begin(cmt_title, p_open, flags);

    imgui_cmt_row0_time_label_and_boost_switch();
    imgui_cmt_row1_time_display();
    imgui_cmt_row2_tape_info();

    float fullWidth = ImGui::GetContentRegionAvail().x;
    if (!CMT_TEST_FILLED)
    {
        ImGui::ProgressBar(-1.0f, ImVec2(fullWidth, 0), _("*** Empty ***"));
    }
    else if (CMT_TEST_RECORD)
    {
        if (CMT_TEST_PAUSED)
        {
            ImGui::ProgressBar(-1.0f, ImVec2(fullWidth, 0), _("Recording (paused)"));
        }
        else
        {
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(fullWidth, 0), _("Recording.."));
        };
    }
    else
    {
        if (CMT_TEST_STOP)
        {
            ImGui::ProgressBar(-1.0f, ImVec2(fullWidth, 0), cmt_get_ui_base_filename());
        }
        else
        {
            ImGui::ProgressBar(progress_playtime, ImVec2(fullWidth, 0), cmt_get_ui_base_filename());
        };
    };

    ImGui::Separator();

    imgui_cmt_row4_combo_mzspeed();

    ImGui::Separator();

    imgui_cmt_row5_buttons_layout();

    ImGui::Separator();

    ImGui::Text("%s", str_status->str);
    g_string_free(str_status, TRUE);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        *p_open = false;
    };

    ImGui::End();
}
