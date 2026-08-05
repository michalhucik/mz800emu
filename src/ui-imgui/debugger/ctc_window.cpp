/**
 * @file ctc_window.cpp
 * @brief Debugger: CTC 8253 chip state inspector window (per-chip-panels F2).
 *
 * Real-time snapshot vnitřního stavu Intel 8253 PIT - 3 nezávislé čítače.
 * Okno se obnovuje per frame (polling pattern, stejný jako CPU/FDC/Watch).
 *
 * Sekce:
 *  - Global header: poslední byte zapsaný do Control Word registru
 *    + dekód SC / RLF / Mode / BCD polí.
 *  - Per-channel CTC0/CTC1/CTC2: out, gate, mode, bcd, rlf, state,
 *    load_done, rl_byte, latch_op, read_latch, preset/value (counter)
 *    a (pokud build s MZ800EMU_CFG_CLK1M1_FAST) scheduled event distance.
 *
 * Side-effect free invariant:
 *  - Čte pouze raw fieldy `g_ctc8253[i]` (struct je veřejná v ctc8253.h)
 *    a mirror byte `g_ctc8253_last_cw_byte`.
 *  - NIKDY nevolá `ctc8253_read_byte()` / `ctc8253_write_byte()` - obě
 *    cesty mají side-effecty (sequencer posun rl_byte, latch reset,
 *    CW dispatch). Mirror byte se aktualizuje uvnitř ctc8253.c při
 *    zápisu na CW registr.
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <cstdio>

#include "i18n.h"

#include "mzarch/mzcommon_config.h"

#include "hw-generic/ctc8253/ctc8253.h"

#include "ui-imgui/debugger/chip_window_helpers.h"
#include "ui-imgui/debugger/ctc_window.h"

extern "C"
{
    void imgui_ctc_state_window(bool *p_open);
}

/* ---------- Lokální popisné stringy pro enum dekódy ---------- */

/**
 * Slovní popis CTC modů (Mode 0-5) dle Intel 8253 datasheet.
 * Indexováno hodnotou en_CTC_MODE (0..5).
 */
static const char *const k_ctc_mode_names[] = {
    N_("Mode 0 Interrupt on Terminal Count"),
    N_("Mode 1 Hardware retriggerable one-shot"),
    N_("Mode 2 Rate generator"),
    N_("Mode 3 Square wave"),
    N_("Mode 4 Software trigger strobe"),
    N_("Mode 5 Hardware trigger strobe"),
};

/**
 * Slovní popis en_CTC_RLF (Read/Load Format).
 * Hodnota 0 nepoužívána (8253 ji bere jako "Counter Latch" cmd, ale interně
 * v emu se rlf inicializuje na 1..3); zobrazujeme placeholder pro robustnost.
 */
static const char *const k_ctc_rlf_names[] = {
    N_("Latch"),     /* 0 - 8253 HW interpretuje jako latch counter cmd */
    N_("LSB only"),  /* 1 = CTC_RLF_LSB */
    N_("MSB only"),  /* 2 = CTC_RLF_MSB */
    N_("LSB + MSB"), /* 3 = CTC_RLF_LSBMSB */
};

/**
 * Slovní popis en_CTC_STATE (vnitřní stavový automat čítače).
 * Indexováno hodnotou enum (0..10).
 */
static const char *const k_ctc_state_names[] = {
    N_("INIT"),                /* bylo vlozeno CW */
    N_("INIT_DONE"),           /* po sestupne CLK */
    N_("LOAD"),                /* zahajen LOAD */
    N_("PRESET_ERROR"),        /* PRESET = 1 v MODE 2 */
    N_("LOAD_DONE"),           /* nasleduje PRESET */
    N_("WAIT_GATE1"),          /* cekame na GATE = 1 */
    N_("PRESET"),              /* priprava pred zacatkem odectu */
    N_("PRESET32"),            /* priprava, do VALUE vlozime 32 */
    N_("COUNTDOWN"),           /* odecet s cilem */
    N_("MODE1_TRIGGER_ERROR"), /* MODE1: trigger pred LOAD_DONE */
    N_("BLIND_COUNT"),         /* odecet bez cile */
};

/**
 * Popis SC (Select Counter) pole z Control Word.
 * 0..2 = CTC0..2, 3 = ilegalni (8253) / Read-Back (8254 - tady nepouzito).
 */
static const char *const k_ctc_sc_names[] = {
    N_("Counter 0"),
    N_("Counter 1"),
    N_("Counter 2"),
    N_("Illegal (Read-Back not supported)"),
};

/* ---------- Pomocné rutiny ---------- */

/**
 * Vykreslí 8-bit hodnotu jako binární řetězec MSB-first (např. "11010010").
 * Buffer musí mít alespoň 9 bytů.
 */
static void format_bin8(uint8_t value, char *out)
{
    for (int i = 7; i >= 0; --i)
        out[7 - i] = (value & (1u << i)) ? '1' : '0';
    out[8] = '\0';
}

/**
 * Vykreslí Global header sekci - Last Control Word byte a jeho dekód.
 *
 * Side-effect free: čte pouze `g_ctc8253_last_cw_byte` (mirror).
 */
static void render_global_header(void)
{
    if (!ImGui::CollapsingHeader(_L("Last Control Word"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    uint8_t cw = g_ctc8253_last_cw_byte;

    /* Dekód polí dle Intel 8253 CW formátu:
     *   D7-D6 = SC  (Select Counter)
     *   D5-D4 = RLF (Read/Load Format)
     *   D3-D1 = M   (Mode 0-5; hodnoty 6 a 7 se mapují na Mode 2 a 3)
     *   D0    = BCD (0 = binary, 1 = BCD)
     */
    unsigned sc = (cw >> 6) & 0x03u;
    unsigned rlf = (cw >> 4) & 0x03u;
    unsigned mode_raw = (cw >> 1) & 0x07u;
    unsigned bcd = cw & 0x01u;

    /* Mode field je 3-bit, ale 8253 platí jen 0..5; 6 = Mode 2 alias,
     * 7 = Mode 3 alias (datasheet, M2/M1/M0 dekód). */
    unsigned mode_resolved = mode_raw;
    if (mode_raw == 6) mode_resolved = 2;
    else if (mode_raw == 7) mode_resolved = 3;

    char binbuf[9];
    format_bin8(cw, binbuf);

    if (ImGui::BeginTable("##ctc_cw", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Value"));
        ImGui::TableSetupColumn(_("Bin"));
        ImGui::TableHeadersRow();

        /* Surový byte. */
        render_hex8_row("CW byte", cw,
                        _("Last byte written to the CW register "
                          "(IORQ 0xD7 / MMIO 0xE007). Mirror, not a HW read."));

        /* SC field (D7-D6). */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("SC (D7-D6)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("Select Counter - which counter "
                                      "was configured by this CW"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(_(k_ctc_sc_names[sc]));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", sc);

        /* RLF field (D5-D4). */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("RLF (D5-D4)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("Read/Load Format - how counter "
                                      "registers will be accessed"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(_(k_ctc_rlf_names[rlf]));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", rlf);

        /* Mode field (D3-D1). */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("Mode (D3-D1)"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("Counter operating mode 0-5. "
                                      "Values 6/7 are aliases of Mode 2/3."));
        ImGui::TableSetColumnIndex(1);
        if (mode_resolved < 6)
            ImGui::TextUnformatted(_(k_ctc_mode_names[mode_resolved]));
        else
            ImGui::TextDisabled("???");
        ImGui::TableSetColumnIndex(2);
        if (mode_raw != mode_resolved)
            ImGui::Text("%u -> %u", mode_raw, mode_resolved);
        else
            ImGui::Text("%u", mode_raw);

        /* BCD field (D0). */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("BCD (D0)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("0 = binary counting, 1 = BCD (4-decade)"));
        ImGui::TableSetColumnIndex(1);
        if (bcd)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "1 (BCD)");
        else
            ImGui::TextDisabled("0 (binary)");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", bcd);

        /* Surová binární reprezentace pro rychlou kontrolu. */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("Bits"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("Binary layout (MSB left): SC|SC|RL|RL|M|M|M|BCD"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(binbuf);

        ImGui::EndTable();
    }
}

/**
 * Vykreslí per-channel sekci jednoho čítače (CTC0/CTC1/CTC2).
 *
 * @param cs           Counter index 0..2.
 * @param header_name  Lokalizovaný popisek collapsing headeru (např. "CTC0").
 *
 * Side-effect free: čte pouze raw fieldy `g_ctc8253[cs]`.
 */
static void render_channel(unsigned cs, const char *header_name)
{
    if (!ImGui::CollapsingHeader(header_name, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const st_CTC8253 *c = &g_ctc8253[cs];

    /* Tabulka názvů musí být unikátní per kanál kvůli ImGui ID. */
    char table_id[32];
    std::snprintf(table_id, sizeof(table_id), "##ctc_chan_%u", cs);

    if (ImGui::BeginTable(table_id, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Value"));
        ImGui::TableSetupColumn(_("Bin"));
        ImGui::TableHeadersRow();

        /* I/O signály. */
        render_flag_row("out", (int)c->out,
                        _("Current counter output (0 / 1) - "
                          "feeds output_cb consumer (e.g. PSG, INT)"));
        render_flag_row("gate", (int)c->gate,
                        _("Last known level of the GATE input"));

        /* Konfigurace. */
        render_enum_row("mode", (unsigned)c->mode, k_ctc_mode_names,
                        sizeof(k_ctc_mode_names) / sizeof(k_ctc_mode_names[0]),
                        _("Counter operating mode"));
        render_flag_row("bcd", (int)c->bcd,
                        _("0 = binary counting, 1 = BCD"));
        render_enum_row("rlf", (unsigned)c->rlf, k_ctc_rlf_names,
                        sizeof(k_ctc_rlf_names) / sizeof(k_ctc_rlf_names[0]),
                        _("Read/Load Format - how many bytes and in what order "
                          "are transferred via the data port"));

        /* Stavový automat. */
        render_enum_row("state", (unsigned)c->state, k_ctc_state_names,
                        sizeof(k_ctc_state_names) / sizeof(k_ctc_state_names[0]),
                        _("Internal state of the counter / sequencer"));

        /* Load/latch sequencer. */
        render_flag_row("load_done", (int)c->load_done,
                        _("LOAD sequence finished (preset value valid)"));
        render_dec_row("rl_byte", c->rl_byte,
                       _("Bytes already written / read in current R/L "
                         "sequence (8253 has only one internal register)"));
        render_flag_row("latch_op", (int)c->latch_op,
                        _("1 = read_latch holds a latched snapshot for reading"));

        /* Counter hodnoty. */
        render_hex16_row("read_latch", (uint16_t)c->read_latch,
                         _("Snapshot value for the pending read sequence"));
        render_hex16_row("preset_value", (uint16_t)c->preset_value,
                         _("Active preset / divisor (counter reaches 0)"));
        render_hex16_row("preset_latch", (uint16_t)c->preset_latch,
                         _("Queued preset waiting to be applied "
                           "(rolling load)"));
        render_hex16_row("value", (uint16_t)c->value,
                         _("Current counter value (16-bit, live countdown)"));

        /* Mode 3 specifické - jen pokud aktuální mode je 3. */
        if (c->mode == CTC_MODE3)
        {
            render_hex16_row("mode3_destination_value",
                             (uint16_t)c->mode3_destination_value,
                             _("Mode 3: target value for the current half-period"));
            render_hex16_row("mode3_half_value",
                             (uint16_t)c->mode3_half_value,
                             _("Mode 3: half period (used to generate "
                               "50% duty square wave)"));
        }

        /* CLK1M1_FAST optimalizace - CTC0 si pamatuje "last event ticks"
         * a má scheduled event. Ostatní kanály nemají vlastní event, ale
         * fieldy ve struct existují vždy (jen jsou unused pro CTC1/CTC2). */
        render_dec_row("clk1m1_last_event_total_ticks",
                       c->clk1m1_last_event_total_ticks,
                       _("Total ticks at the time of the last CLK1M1 event "
                         "(used for skip-ahead math)"));

        /* st_EMUEVENT::ticks == -1 znamená "neaktivní". */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("clk1m1_event.ticks");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("Scheduled event ticks (-1 = inactive). "
                                "Active only for CTC0 in MZ800 architecture."));
        ImGui::TableSetColumnIndex(1);
        if ((int)c->clk1m1_event.ticks == -1)
            ImGui::TextDisabled(_("(inactive)"));
        else
            ImGui::Text("%u", c->clk1m1_event.ticks);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("-");

        /* Output callback presence (ne null = někdo poslouchá změny OUT). */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("output_cb");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                              _("Registered callback invoked on OUT change "
                                "(typically audio sink / INT line)"));
        ImGui::TableSetColumnIndex(1);
        if (c->output_cb != nullptr)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               _("registered"));
        else
            ImGui::TextDisabled(_("(none)"));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("-");

        ImGui::EndTable();
    }
}

void imgui_ctc_state_window(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);

    /* Stable ID "###CTCState" (tři hashe) drží layout přes různé title
     * variants (paused/running atd. v budoucnu). */
    if (!ImGui::Begin(_L("CTC 8253 State###CTCState"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* Global header s posledním Control Word a jeho dekódem. */
    render_global_header();

    /* Per-channel sekce. */
    render_channel(0, _L("CTC0"));
    render_channel(1, _L("CTC1"));
    render_channel(2, _L("CTC2"));

    ImGui::End();
}
