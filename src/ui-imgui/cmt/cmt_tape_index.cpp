#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <vector>
#include <string>

// Lokalizace
#include "i18n.h"

#include "ui-imgui/filechooser/res/CustomFont.h"
#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "emulator/device_request.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_block.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/cmt/cmt_mzf.h"
#include "hw-generic/cmt/cmt_tap.h"

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mztape/mztape.h"
#include "libs/zxtape/zxtape.h"
#include "libs/cmtspeed/cmtspeed.h"

#include "CmtGlyphRenderer.h"
#include "ImGuiCmt.hpp"
#include "imgui_cmt.h"
#include "emulator/replay.h"

extern "C"
{
    void imgui_cmt_tape_index_window(bool *p_open);
    void imgui_cmt_tape_update_filelist(void);
    void imgui_cmt_tape_filelist_refresh(void);
};

/**
 * @brief Příznak "seznam bloků pásky je zastaralý" (atomický, libovolné vlákno).
 *
 * Jádro kazety hlásí změnu pásky z emu vlákna; seznam bloků
 * (g_tapeFileList) ale smí měnit jen UI vlákno, které ho při kreslení
 * prochází. Výchozí 1: seznam se sestaví při prvním snímku.
 */
static gint s_filelist_dirty = 1;

/**
 * @brief Označí seznam bloků pásky jako zastaralý (ui_cmt_window_update() jádra).
 *
 * Seznam sám nepřestavuje, jen nastaví příznak; přestaví ho UI vlákno
 * v imgui_cmt_tape_filelist_refresh().
 *
 * @note Libovolné vlákno (typicky emu vlákno ze zásahu do kazety).
 */
void imgui_cmt_tape_update_filelist(void)
{
    g_atomic_int_set(&s_filelist_dirty, 1);
}

/**
 * @brief Přestaví seznam bloků pásky, když ho jádro označilo jako zastaralý.
 *
 * @pre UI vlákno, drží zámek stavu zařízení (device_state_lock()): čte
 *      kontejner pásky, který emu vlákno mění při otevření a vysunutí.
 * @post Při změně (příznak nebo právě otevřené okno Virtual CMT)
 *       g_tapeFileList odpovídá vložené pásce; bez pásky, se zavřeným oknem
 *       Virtual CMT nebo u pásky bez indexu bloků je prázdný a okno Tape
 *       Filelist zavřené.
 */
void imgui_cmt_tape_filelist_refresh(void)
{
    /* Seznam se sestavuje jen při otevřeném okně Virtual CMT; po jeho
     * otevření ho sestavit znovu. */
    static bool s_last_show = false;
    bool show_opened = (g_gui->showVirtualCmtWindow && !s_last_show);
    s_last_show = g_gui->showVirtualCmtWindow;

    if (!g_atomic_int_compare_and_exchange(&s_filelist_dirty, 1, 0) && !show_opened)
    {
        return;
    };

    g_tapeFileList.clear();

    if ((!CMT_TEST_FILLED) || (!g_gui->showVirtualCmtWindow))
    {
        g_gui->showVirtualCmtTapeIndexWindow = false;
        return;
    };

    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);

    if (cmtext_container_get_type(container) != CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE)
    {
        g_gui->showVirtualCmtTapeIndexWindow = false;
        return;
    };

    UiCmt::updateTapeFilelist();
}

/**
 * @brief Výběr bloku v seznamu (kliknutí / dvojklik).
 *
 * Provede emu vlákno (cmt_tape_select_block() přes replay_host_cmt()), aby
 * výběr zachytil záznam vstupů. Seznam bloků obnoví jádro kazety.
 */
void imgui_cmt_tape_set_block(int block_id, bool forced)
{
    replay_host_cmt(forced ? REPLAY_CMT_SELECT_FORCED : REPLAY_CMT_SELECT, block_id, NULL);
}

/**
 * @brief Změní rychlost bloku pásky z výběru v seznamu bloků.
 *
 * Právě přehrávaný blok se nemění. Je-li blok připravený (aktuální),
 * otevře se znovu, aby se stream přegeneroval.
 *
 * @param entry Řádek seznamu bloků.
 * @param cmtspeed Nová rychlost: CMTSPEED_NONE = výchozí rychlost Virtual CMT,
 *        poměr, nebo CMTSPEED_CUSTOM (pak je pulses povinné).
 * @param pulses Vlastní délky pulzů pro CMTSPEED_CUSTOM (jinak nullptr).
 */
void imgui_cmt_tape_speed_changed(const TapeFileEntry_t &entry, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses = nullptr)
{
    /* Blok změní emu vlákno (cmt_tape_change_block_speed()), seznam bloků
     * pak označí jako zastaralý. */
    cmt_request_set_block_speed(entry.id, cmtspeed, entry.cmtspeed_type == CMT_SPEED_TYPE_MZ, pulses);
}

void imgui_cmt_tape_create_speed_combo(const TapeFileEntry_t &entry)
{
    if (entry.cmtspeed_type == CMT_SPEED_TYPE_NONE)
    {
        ImGui::Text("%s", entry.cmtspeed_txt.c_str());
        return;
    };

    /* Rychlost bloku záznam vstupů nezopakuje. */
    bool is_disabled = entry.played || imgui_cmt_replay_recording();

    if (is_disabled)
    {
        ImGui::BeginDisabled();
    };

    std::vector<ImGuiCmtSpeedList_t> speed_list;
    if (entry.cmtspeed_type == CMT_SPEED_TYPE_MZ)
    {
        speed_list = ImGuiCmt::getMzSpeedListDefault();
    }
    else if (entry.cmtspeed_type == CMT_SPEED_TYPE_ZX)
    {
        speed_list = ImGuiCmt::getZxSpeedList();
    };

    if (ImGui::BeginCombo("##SpeedCombo", entry.cmtspeed_txt.c_str(), 0))
    {
        for (size_t n = 0; n < speed_list.size(); n++)
        {
            bool is_selected = false;
            if (((entry.block_speed == CMTEXT_BLOCK_SPEED_DEFAULT) && (n == 0)) || ((entry.block_speed == CMTEXT_BLOCK_SPEED_SET) && (entry.cmtspeed == speed_list[n].cmtspeed)))
            {
                is_selected = true;
            };

            if (ImGui::Selectable(speed_list[n].label_txt.c_str(), is_selected))
            {
                if (cmtspeed_is_custom(speed_list[n].cmtspeed))
                {
                    /* délky se zadají v editoru (výchozí jsou současné pulzy bloku) */
                    ImGuiCmt::openPulsesEditor("cmt_tape_index", entry.id, (entry.has_pulses) ? &entry.pulses : &g_cmt.mz_custom_pulses);
                }
                else
                {
                    imgui_cmt_tape_speed_changed(entry, speed_list[n].cmtspeed);
                };
            };

            if (is_selected)
            {
                ImGui::SetItemDefaultFocus();
            };
        };
        ImGui::EndCombo();
    };

    if (is_disabled)
    {
        ImGui::EndDisabled();
    };
}

void imgui_cmt_tape_index_window(bool *p_open)
{
    if (!*p_open)
        return;

    if ((!g_gui->showVirtualCmtWindow) || (!CMT_TEST_FILLED) || g_tapeFileList.empty())
    {
        *p_open = false;
        return;
    };

    bool fl_force_select_playing_row = true;

    ImGui::SetNextWindowSize(ImVec2(700, 300), ImGuiCond_FirstUseEver);
    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *tape_title = _L("Tape Filelist");
    auto_layout_first_use_portrait(tape_title, 500.0f, 400.0f);
    if (!ImGui::Begin(tape_title, p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    };
    /* Přehrání vstupů: kazetu ovládá záznam. */
    const bool replay_lock = imgui_cmt_replay_lock_begin(false);

    static int selected_row = -1;
    if (selected_row >= static_cast<int>(g_tapeFileList.size()))
        selected_row = -1;
    if (fl_force_select_playing_row)
        selected_row = -1;

    static float row_height = ImGui::GetTextLineHeightWithSpacing();

    const ImVec4 green = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    const ImVec4 green_hover = ImVec4(0.6f, 1.0f, 0.6f, 1.0f); // světlejší pro hover

    // Vytvoření tabulky
    ImGuiTableFlags table_flags =
        // ImGuiTableFlags_NoPadInnerX |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("TapeFileTable", 9, table_flags))
    {
        // Zafixování první řádky a sloupce
        ImGui::TableSetupScrollFreeze(1, 1);

        // Definice sloupců (bez hover efektu pro hlavičku)
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(_("Name"));
        ImGui::TableSetupColumn(_("Type"));
        ImGui::TableSetupColumn(_("Size"));
        ImGui::TableSetupColumn(_("Start"));
        ImGui::TableSetupColumn(_("Exec"));
        ImGui::TableSetupColumn(_("Speed"));
        ImGui::TableSetupColumn(_("Length"), ImGuiTableColumnFlags_WidthStretch);

        // Vlastní hlavička bez hover efektu
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 8; ++column)
        {
            ImGui::TableSetColumnIndex(column);

            const char *header_text = nullptr;
            switch (column)
            {
            case 0:
                header_text = "#";
                break;
            case 1:
                header_text = "";
                break;
            case 2:
                header_text = _("Name");
                break;
            case 3:
                header_text = _("Type");
                break;
            case 4:
                header_text = _("Size");
                break;
            case 5:
                header_text = _("Start");
                break;
            case 6:
                header_text = _("Exec");
                break;
            case 7:
                header_text = _("Speed");
                break;
            case 8:
                header_text = _("Length");
                break;
            }

            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0)); // Zruší hover efekt
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));  // Zruší klik efekt
            ImGui::TableHeader(header_text);
            ImGui::PopStyleColor(2); // Obnovení stylu
        }

        // Vykreslení řádků tabulky
        for (size_t i = 0; i < g_tapeFileList.size(); ++i)
        {
            const TapeFileEntry_t &entry = g_tapeFileList[i];

            if (fl_force_select_playing_row)
            {
                if (entry.play_state == PLAY_STATE_PLAYING)
                {
                    selected_row = static_cast<int>(i);
                };
            };

            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

            ImGui::PushID(static_cast<int>(i));

            // === MUSÍME začít prvním sloupcem, jinak pozice nebude platná ===
            ImGui::TableSetColumnIndex(0);

            // Získáme počátek řádku
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            float row_max_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            ImVec2 row_max = ImVec2(row_max_x, row_min.y + row_height);

            // Zjistíme, jestli je kurzor nad celou řádkou. Jen když myš
            // patří tomuto oknu - ne pod modálním popupem ani pod jiným
            // oknem, které seznam překrývá (samotná poloha myši to nepozná).
            // NoPopupHierarchy: editor pulzů se otevírá z tohoto okna a ImGui
            // by jinak myš nad ním počítal jako myš nad seznamem.
            ImVec2 mouse = ImGui::GetMousePos();
            bool is_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_NoPopupHierarchy) &&
                              mouse.y >= row_min.y && mouse.y <= row_max.y &&
                              mouse.x >= row_min.x && mouse.x <= row_max.x;

            ImVec4 current_color = is_hovered ? green_hover : green;

            // Sloupec 0 (už jsme v něm)
            ImGui::Text("%d", entry.id);

            // Další sloupce
            if (ImGui::TableSetColumnIndex(1))
            {
                if (entry.ready)
                {
                    const ImVec2 CMT_BUTTON_SIZE(30, 0);
                    const float CMT_BUTTONS_XOFFSET = 0.0f;
                    const float CMT_BUTTONS_SPACING = 2.0f;
                    CmtGlyphRenderer renderer(18.0f, 4.0f);

                    ImGui::BeginDisabled(!entry.played);
                    if (ImGui::Button("##cmt_btn_stop", CMT_BUTTON_SIZE))
                    {
                        replay_host_cmt(REPLAY_CMT_STOP, 0, NULL);
                    };
                    renderer.drawStop();
                    ImGui::EndDisabled();

                    ImGui::SameLine(CMT_BUTTONS_XOFFSET, CMT_BUTTONS_SPACING);

                    ImGui::BeginDisabled(entry.played);
                    if (ImGui::Button("##cmt_btn_play", CMT_BUTTON_SIZE))
                    {
                        replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
                    };
                    renderer.drawPlay(CMT_TEST_PLAY);
                    ImGui::EndDisabled();

                    ImGui::SameLine(CMT_BUTTONS_XOFFSET, CMT_BUTTONS_SPACING);

                    if (ImGui::Button("##cmt_btn_pause", CMT_BUTTON_SIZE))
                    {
                        replay_host_cmt(REPLAY_CMT_PAUSE, CMT_TEST_PAUSED ? 0 : 1, NULL);
                    };
                    renderer.drawPause(CMT_TEST_PAUSED);
                };
            };

            if (ImGui::TableSetColumnIndex(2))
                ImGui::TextColored(current_color, "%s", entry.name.c_str());

            if (ImGui::TableSetColumnIndex(3))
                ImGui::TextColored(current_color, "%s", entry.ftype_txt.c_str());

            if (ImGui::TableSetColumnIndex(4))
                ImGui::TextColored(current_color, "%s", entry.fsize_txt.c_str());

            if (ImGui::TableSetColumnIndex(5))
                ImGui::TextColored(current_color, "%s", entry.fstrt_txt.c_str());

            if (ImGui::TableSetColumnIndex(6))
                ImGui::TextColored(current_color, "%s", entry.fexec_txt.c_str());

            if (ImGui::TableSetColumnIndex(7))
            {
                ImGui::PushItemWidth(-FLT_MIN);
                if (entry.cmtspeed_type != CMT_SPEED_TYPE_NONE)
                {
                    imgui_cmt_tape_create_speed_combo(entry);
                }
                else
                {
                    ImGui::TextColored(current_color, "%s", entry.cmtspeed_txt.c_str());
                };
                ImGui::PopItemWidth();
            };

            if (ImGui::TableSetColumnIndex(8))
                ImGui::TextColored(current_color, "%s", entry.length_txt.c_str());

            if (is_hovered)
            {
                ImU32 hover_col = ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.3f, 0.3f));
                ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, hover_col);

                // Klik a dvojklik jen pokud není aktivní widget + popup
                if (!ImGui::IsAnyItemActive() &&
                    !ImGui::IsAnyItemHovered() &&
                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
                {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        if (!fl_force_select_playing_row)
                        {
                            selected_row = (selected_row == static_cast<int>(i)) ? -1 : static_cast<int>(i);
                        };

                        imgui_cmt_tape_set_block(entry.id, false);
                    };

                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        imgui_cmt_tape_set_block(entry.id, true);
                    };
                };
            };

            bool is_selected = (selected_row == static_cast<int>(i));
            if (is_selected)
            {
                ImU32 sel_col = ImGui::GetColorU32(ImVec4(0.2f, 0.6f, 0.2f, 0.4f));
                ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, sel_col);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    /* editor vlastních pulzů bloku (otevírá ho výběr "Custom pulses..." v combu) */
    int owner;
    st_MZTAPE_PULSES_LENGTH pulses;
    if (ImGuiCmt::drawPulsesEditor("cmt_tape_index", &owner, &pulses))
    {
        for (const auto &entry : g_tapeFileList)
        {
            if (entry.id == owner)
            {
                /* kopie - speed_changed seznam bloků přestaví */
                const TapeFileEntry_t changed = entry;
                imgui_cmt_tape_speed_changed(changed, CMTSPEED_CUSTOM, &pulses);
                break;
            };
        };
    };

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        *p_open = false;
    };

    imgui_cmt_replay_lock_end(replay_lock);
    ImGui::End();
}
