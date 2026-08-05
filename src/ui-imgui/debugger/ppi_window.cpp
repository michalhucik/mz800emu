/**
 * @file ppi_window.cpp
 * @brief Debugger: PPI 8255 chip state inspector window (per-chip-panels F3).
 *
 * Live snapshot stavu Intel 8255 PPI:
 *  - Last Control Word + dekód (Mode Set vs. Bit Set/Reset).
 *  - Port A: raw + per-bit popis (klávesnicový sloupec, JOY1/JOY2 enable bity).
 *  - Port B: 10x8 keyboard matrix grid (kombinace fyzické a virtuální
 *    klávesnice; toggle Combined / Physical / Virtual).
 *  - Port C: raw + per-bit popis OUT/IN bitů (CTC0 audio mask, CMT data/motor,
 *    CTC2 INT mask, motor IN, computed IN bity 5-7).
 *  - VKBD autotype state (collapsible, default closed).
 *
 * Side-effect free: čte výhradně raw fields `g_pio8255` + mirror `last_cw_byte`.
 * NEVOLÁ `pio8255_read()`, `pio8255_pc4_get()`, `pio8255_pa5_get()` ani jiné
 * gettery - ty mají side-effecty (sample CMT, JOY probe). Bity PC5/PC6/PC7
 * v hlavičce zakomentované (computed on demand v `pio8255_pc?_get()`); UI je
 * jen označí "computed on read (not in mirror)".
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"

#include "i18n.h"
#include "emulator/mzarch/mzhal.h"
#include "hw-generic/pio8255/pio8255.h"

#include "ui-imgui/debugger/chip_window_helpers.h"
#include "ui-imgui/debugger/ppi_window.h"

#include <stdint.h>
#include <stdio.h>

extern "C"
{
    void imgui_ppi_state_window(bool *p_open);
}

/**
 * @brief Mód zobrazení 10x8 keyboard matrix gridu.
 *
 * Combined = AND fyzické a virtuální matice (= co reálně dostane CPU po IN
 * z Port B při daném sloupci PA0-3). Physical = jen `keyboard_matrix[]`
 * (fyzická SDL klávesnice). Virtual = jen `vkbd_matrix[]` (autotype / VKBD).
 */
enum en_ppi_matrix_view
{
    PPI_MATRIX_COMBINED = 0,
    PPI_MATRIX_PHYSICAL = 1,
    PPI_MATRIX_VIRTUAL = 2,
};

/**
 * @brief Vykreslí 8-znakovou binární reprezentaci 8-bit hodnoty (MSB vlevo)
 *        jako prostý ImGui text.
 *
 * Pomocná lokální utilita pro inline zobrazení vedle hex hodnoty (nepoužívá
 * 3-sloupcovou tabulku, jen jeden text).
 *
 * @param value  8-bit hodnota.
 */
static void ppi_render_bin8_inline(uint8_t value)
{
    char buf[10];
    for (int i = 7; i >= 0; --i)
        buf[7 - i] = (value & (1u << i)) ? '1' : '0';
    buf[8] = '\0';
    ImGui::TextUnformatted(buf);
}

/**
 * @brief Vykreslí dekód Last Control Word PPI 8255.
 *
 * Bit 7 = 1 → Mode Set (D6-D5 Group A mode 0-2, D4 PA dir, D3 PC4-7 dir,
 * D2 Group B mode 0-1, D1 PB dir, D0 PC0-3 dir).
 * Bit 7 = 0 → Bit Set/Reset (D3-D1 cílový bit PC0-7, D0 set/reset).
 *
 * Neaktivní interpretace se renderuje disabled (šedý text).
 *
 * @param cw  Hodnota `g_pio8255.last_cw_byte`.
 */
static void ppi_render_cw_decode(uint8_t cw)
{
    const bool is_mode_set = (cw & 0x80) != 0;

    /* Mode Set sekce */
    if (is_mode_set)
        ImGui::TextUnformatted(_("Mode Set (D7=1)"));
    else
        ImGui::TextDisabled("%s", _("Mode Set (D7=1)"));

    ImGui::Indent();
    {
        const unsigned group_a_mode = (cw >> 5) & 0x03;
        const unsigned pa_dir = (cw >> 4) & 0x01;
        const unsigned pc_upper_dir = (cw >> 3) & 0x01;
        const unsigned group_b_mode = (cw >> 2) & 0x01;
        const unsigned pb_dir = (cw >> 1) & 0x01;
        const unsigned pc_lower_dir = cw & 0x01;

        ImVec4 c = is_mode_set ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
        ImGui::TextColored(c, "D6-D5  Group A mode: %u (%s)", group_a_mode,
                           group_a_mode == 0   ? "Mode 0 basic"
                           : group_a_mode == 1 ? "Mode 1 strobed"
                                               : "Mode 2 bidirectional");
        ImGui::TextColored(c, "D4     Port A dir: %s", pa_dir ? "input" : "output");
        ImGui::TextColored(c, "D3     Port C upper (PC4-7) dir: %s",
                           pc_upper_dir ? "input" : "output");
        ImGui::TextColored(c, "D2     Group B mode: %u (%s)", group_b_mode,
                           group_b_mode ? "Mode 1 strobed" : "Mode 0 basic");
        ImGui::TextColored(c, "D1     Port B dir: %s", pb_dir ? "input" : "output");
        ImGui::TextColored(c, "D0     Port C lower (PC0-3) dir: %s",
                           pc_lower_dir ? "input" : "output");
    }
    ImGui::Unindent();

    /* BSR sekce */
    if (!is_mode_set)
        ImGui::TextUnformatted(_("Bit Set/Reset (D7=0)"));
    else
        ImGui::TextDisabled("%s", _("Bit Set/Reset (D7=0)"));

    ImGui::Indent();
    {
        const unsigned bsr_bit = (cw >> 1) & 0x07;
        const unsigned bsr_set = cw & 0x01;
        ImVec4 c = !is_mode_set ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
        ImGui::TextColored(c, "D3-D1  PC bit select: %u (PC%u)", bsr_bit, bsr_bit);
        ImGui::TextColored(c, "D0     Action: %s", bsr_set ? "SET (1)" : "RESET (0)");
    }
    ImGui::Unindent();
}

/**
 * @brief Vykreslí 10x8 grid keyboard matrix (sloupce 0-9 vodorovně, bity 0-7
 *        svisle).
 *
 * Buňka aktivní (bit = 0, klávesa stisknutá - active LOW) se kreslí zelenou
 * tečkou; neaktivní (bit = 1) tmavou tečkou. Header tooltip vysvětluje
 * polaritu.
 *
 * @param mode  Který datový zdroj zobrazit: Combined (AND kbd & vkbd), jen
 *              kbd, nebo jen vkbd.
 */
static void ppi_render_keyboard_matrix(en_ppi_matrix_view mode)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("ppi_kbd_matrix", 11, flags))
        return;

    /* Hlavička: prázdná buňka pro řádek "bit" + 10 sloupců. */
    ImGui::TableSetupColumn(_("bit"));
    for (int col = 0; col < 10; ++col)
    {
        char hdr[8];
        snprintf(hdr, sizeof(hdr), "%d", col);
        ImGui::TableSetupColumn(hdr);
    }
    ImGui::TableHeadersRow();

    /* Tooltip nad hlavičkou - polarita active LOW. */
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          _("0 = key pressed, 1 = released (active LOW). "
                            "Rows = bit 0..7, columns = PA0-3 selected column."));

    const ImVec4 col_on(0.30f, 1.00f, 0.30f, 1.0f);
    const ImVec4 col_off(0.30f, 0.30f, 0.30f, 1.0f);

    for (int bit = 0; bit < 8; ++bit)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("b%d", bit);
        for (int col = 0; col < 10; ++col)
        {
            ImGui::TableSetColumnIndex(1 + col);
            uint8_t value;
            switch (mode)
            {
                case PPI_MATRIX_PHYSICAL:
                    value = g_pio8255.keyboard_matrix[col];
                    break;
                case PPI_MATRIX_VIRTUAL:
                    value = g_pio8255.vkbd_matrix[col];
                    break;
                case PPI_MATRIX_COMBINED:
                default:
                    value = g_pio8255.keyboard_matrix[col] &
                            g_pio8255.vkbd_matrix[col];
                    break;
            }
            const bool active = (value & (1u << bit)) == 0;
            ImGui::TextColored(active ? col_on : col_off, active ? "*" : ".");
        }
    }

    ImGui::EndTable();
}

void imgui_ppi_state_window(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_FirstUseEver);

    /* Stable ID "###PPIState" (tři hashe) drží layout přes různé title
     * variants (paused/running, atd. v budoucnu). */
    if (!ImGui::Begin(_L("PPI 8255 State###PPIState"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* --------------------------------------------------------------
     * Last Control Word
     * -------------------------------------------------------------- */
    if (ImGui::CollapsingHeader(_L("Last Control Word###PPI_CW"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        const uint8_t cw = g_pio8255.last_cw_byte;
        if (ImGui::BeginTable("ppi_cw_tbl", 3,
                              ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("hex");
            ImGui::TableSetupColumn("bin");
            render_hex8_row("last_cw_byte", cw,
                            _("Last byte written to Control port (debug "
                              "mirror - 8255 CW sequencer is not HW readable)."));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ppi_render_cw_decode(cw);
    }

    /* --------------------------------------------------------------
     * Port A
     * -------------------------------------------------------------- */
    if (ImGui::CollapsingHeader(_L("Port A###PPI_PA"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        const uint8_t pa = (uint8_t)g_pio8255.signal_PA;
        if (ImGui::BeginTable("ppi_pa_tbl", 3,
                              ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("hex");
            ImGui::TableSetupColumn("bin");
            render_hex8_row("signal_PA", pa,
                            _("Output port A (sampled keyboard column + JOY enables)."));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextUnformatted(_("Bit decode:"));
        ImGui::Indent();
        ImGui::Text("D3-D0  %s: %u",
                    _("keyboard column (PA0-3)"),
                    g_pio8255.signal_PA_keybord_column);
        ImGui::Text("D4     %s: %u",
                    _("JOY1 enable (active LOW)"),
                    g_pio8255.signal_PA_joy1_enabled);
        ImGui::Text("D5     %s: %u",
                    _("JOY2 enable (active LOW)"),
                    g_pio8255.signal_PA_joy2_enabled);
        ImGui::TextDisabled("D6-D7  %s", _("reserved"));
        ImGui::Unindent();
    }

    /* --------------------------------------------------------------
     * Port B - keyboard matrix grid
     * -------------------------------------------------------------- */
    if (ImGui::CollapsingHeader(_L("Port B - Keyboard Matrix###PPI_PB"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        /* Persistent mód napříč framy. */
        static int s_matrix_view = (int)PPI_MATRIX_COMBINED;

        ImGui::TextUnformatted(_("View:"));
        ImGui::SameLine();
        ImGui::RadioButton(_L("Combined###PPI_PB_VIEW_C"),
                           &s_matrix_view, (int)PPI_MATRIX_COMBINED);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("AND of physical and virtual matrix "
                                "(what CPU actually reads)."));
        ImGui::SameLine();
        ImGui::RadioButton(_L("Physical###PPI_PB_VIEW_P"),
                           &s_matrix_view, (int)PPI_MATRIX_PHYSICAL);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("g_pio8255.keyboard_matrix[] only "
                                "(SDL keyboard)."));
        ImGui::SameLine();
        ImGui::RadioButton(_L("Virtual###PPI_PB_VIEW_V"),
                           &s_matrix_view, (int)PPI_MATRIX_VIRTUAL);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("g_pio8255.vkbd_matrix[] only "
                                "(VKBD / autotype)."));

        ImGui::Spacing();
        ppi_render_keyboard_matrix((en_ppi_matrix_view)s_matrix_view);

        ImGui::Spacing();
        ImGui::TextDisabled("%s", _("Cell '*' = key pressed (bit=0, active LOW), "
                                    "'.' = released (bit=1)."));
    }

    /* --------------------------------------------------------------
     * Port C
     * -------------------------------------------------------------- */
    if (ImGui::CollapsingHeader(_L("Port C###PPI_PC"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        const uint8_t pc = (uint8_t)g_pio8255.signal_PC;
        if (ImGui::BeginTable("ppi_pc_tbl", 3,
                              ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("hex");
            ImGui::TableSetupColumn("bin");
            render_hex8_row("signal_PC", pc,
                            _("Port C raw output state (D0-D3 OUT, "
                              "D4-D7 IN; D5-D7 computed on read)."));
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextUnformatted(_("Bit decode:"));
        ImGui::Indent();
        if (g_mzhal.audio_ctc0_gate_pc00) {
            ImGui::Text("D0 OUT  pc00 = %u  (%s)", g_pio8255.signal_pc00,
                        _("CTC0 audio mask (0 = mute, 1 = pass)"));
        } else {
            ImGui::TextDisabled("D0 OUT  pc00 - %s (MZ-700)",
                                _("CTC0 audio mask not present"));
        }
        ImGui::Text("D1 OUT  pc01 = %u  (%s)", g_pio8255.signal_pc01,
                    _("CMT data out"));
        ImGui::Text("D2 OUT  pc02 = %u  (%s)", g_pio8255.signal_pc02,
                    _("CTC2 INT mask (0 = blocked, 1 = enabled)"));
        ImGui::Text("D3 OUT  pc03 = %u  (%s)", g_pio8255.signal_pc03,
                    _("CMT motor control (rising edge toggles)"));
        ImGui::Text("D4 IN   pc04 = %u  (%s)", g_pio8255.signal_pc04,
                    _("CMT motor state"));
        ImGui::TextDisabled("D5 IN   pc05 - %s",
                            _("CMT data in - computed on read (not in mirror)"));
        ImGui::TextDisabled("D6 IN   pc06 - %s",
                            _("Cursor blink NE556@OUT - computed on read"));
        ImGui::TextDisabled("D7 IN   pc07 - %s",
                            _("VBLN - computed on read"));
        ImGui::Unindent();
        /* Pozn.: Prefixy "D0 OUT", "D1 OUT" atd. + "pc00", "pc01" jsou jména
         * registrových bitů a interních proměnných emu (technické identifikátory,
         * neměníme; uživatel je musí umět spárovat s datasheet). */
    }

    /* --------------------------------------------------------------
     * VKBD autotype state (collapsible, default closed)
     * -------------------------------------------------------------- */
    if (ImGui::CollapsingHeader(_L("VKBD Autotype###PPI_VKBD"),
                                /* default closed */ 0))
    {
        if (ImGui::BeginTable("ppi_vkbd_tbl", 3,
                              ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("value");
            ImGui::TableSetupColumn("extra");

            /* vkbd_autotype - string pointer (může být NULL). */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("vkbd_autotype");
            ImGui::TableSetColumnIndex(1);
            if (g_pio8255.vkbd_autotype != NULL)
                ImGui::Text("\"%s\"", g_pio8255.vkbd_autotype);
            else
                ImGui::TextDisabled("%s", _("(none)"));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("-");

            render_dec_row("vkbd_autotype_col",
                           (unsigned)g_pio8255.vkbd_autotype_col,
                           _("Current column index into vkbd_autotype string."));
            render_hex8_row("vkbd_autotype_ret",
                            g_pio8255.vkbd_autotype_ret,
                            _("Last decoded matrix byte (column<<4 | bit)."));
            render_flag_row("vkbd_autotype_ret_shift",
                            g_pio8255.vkbd_autotype_ret_shift ? 1 : 0,
                            _("Shift modifier required for current char."));
            render_dec_row("vkbd_autotype_keydown",
                           (unsigned)g_pio8255.vkbd_autotype_keydown,
                           _("Currently in key-down phase (vs. key-up gap)."));

            /* ms fieldy jsou double - vlastní render. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("vkbd_autotype_kd_ms");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", g_pio8255.vkbd_autotype_kd_ms);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("ms");

            render_dec_row("vkbd_autotype_kd_ticks",
                           (unsigned)g_pio8255.vkbd_autotype_kd_ticks,
                           _("Key-down duration in gdgclk ticks."));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("vkbd_autotype_ku_ms");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", g_pio8255.vkbd_autotype_ku_ms);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("ms");

            render_dec_row("vkbd_autotype_ku_ticks",
                           (unsigned)g_pio8255.vkbd_autotype_ku_ticks,
                           _("Key-up gap duration in gdgclk ticks."));
            render_dec_row("vkbd_autotype_start_ticks",
                           (unsigned)g_pio8255.vkbd_autotype_start_ticks,
                           _("Tick stamp of current phase start."));

            ImGui::EndTable();
        }
    }

    ImGui::End();
}
