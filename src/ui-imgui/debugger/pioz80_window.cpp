/**
 * @file pioz80_window.cpp
 * @brief Debugger: Z80 PIO chip state inspector window (per-chip-panels F4).
 *
 * Real-time snapshot vnitřního stavu Z80 PIO - global INT/IEO stav a interrupt
 * daisy-chain priorita, per-port view (Port A / Port B): mode (Output / Input /
 * Bidirectional / Control), I/O mask (relevantní jen v Mode 3), ctrl sequencer
 * očekávání (`ctrl_expect`), datový výstupní registr, masked input snapshot,
 * IM2 interrupt vektor, ICFNC / ICLVL / ICMASK / ICENA Interrupt Control Word
 * pole, port-level pending/received stav a poslední ICFNC výsledek.
 *
 * Last Control Byte mirror (`g_pioz80_port_a/b_last_ctrl_byte`) se dekóduje
 * podle ctrl_expect kontextu - HW samotné registry pro control nemá (Z80 PIO
 * řídicí port je sekvenční stavový automat). Dekódovací logika přesně mapuje
 * cestu v `pioz80_port_wr_ctrl` (`pioz80.c`): IVW (D0=0), MCW (D3..D0=1111,
 * D7..D6 = mode), ICW (D3..D0=0111), IDW (D3..D0=0011), follow-up MASK po ICW
 * s ICMSK_BIT, follow-up IO_SELECT po Mode 3 Set.
 *
 * Side-effect free: čte výhradně `g_pioz80` strukturu a mirror byty. NIKDY
 * nevolá `pioz80_read_byte()` / `pioz80_write_byte()` / `pioz80_interrupt_*()`
 * (= side-effects: sequencer posun, interrupt ack).
 *
 * Per-arch: Z80 PIO je dostupné jen na MZ-800 a MZ-1500 (`HAVE_PIOZ80 == 1`).
 * MZ-700 (`HAVE_PIOZ80 == 0`) PIO čip nemá - celé tělo souboru je vyřazené
 * compile-time, funkce `imgui_pioz80_state_window()` se v MZ-700 buildu
 * vůbec nedefinuje (žádná deklarace, žádný linker symbol, žádný UI hook).
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"

#include "i18n.h"

#include "mzarch/mzcommon_config.h"

#include "ui-imgui/debugger/chip_window_helpers.h"
#include "ui-imgui/debugger/pioz80_window.h"


#include "hw-generic/pioz80/pioz80.h"

extern "C"
{
    void imgui_pioz80_state_window(bool *p_open);
}

/* ------------------------------------------------------------------
 * Enum label tabulky - musí indexačně odpovídat hodnotám en_PIOZ80_*.
 * Anglické zdrojové texty obalené N_() pro extrakci do .pot; runtime
 * překlad přes _() provádí render_enum_row() případně volající.
 * ------------------------------------------------------------------ */

/** @brief Labely en_PIOZ80_PORT_MODE (Output / Input / Bidirectional / Control). */
static const char *const g_pioz80_port_mode_names[] = {
    N_("Mode 0 - Output"),
    N_("Mode 1 - Input"),
    N_("Mode 2 - Bidirectional"),
    N_("Mode 3 - Control"),
};

/** @brief Labely en_PIOZ80_EXPECT (další očekávaný byte na control portu). */
static const char *const g_pioz80_expect_names[] = {
    N_("Command"),
    N_("IO Mask Control Word (Mode 3 follow-up)"),
    N_("Interrupt Mask Control Word (ICW follow-up)"),
};

/** @brief Labely en_PIOZ80_ICENA. */
static const char *const g_pioz80_icena_names[] = {
    N_("Disabled"),
    N_("Enabled"),
};

/** @brief Labely en_PIOZ80_ICFNC (OR = jakýkoliv bit, AND = všechny bity). */
static const char *const g_pioz80_icfnc_names[] = {
    N_("OR (any unmasked bit)"),
    N_("AND (all unmasked bits)"),
};

/** @brief Labely en_PIOZ80_ICLVL (úroveň pro porovnání s ICFNC). */
static const char *const g_pioz80_iclvl_names[] = {
    N_("LOW (active 0)"),
    N_("HIGH (active 1)"),
};

/** @brief Labely en_PIOZ80_PORT_INT (pending/received stav portu). */
static const char *const g_pioz80_port_int_names[] = {
    N_("None (idle)"),
    N_("Pending (waiting for service)"),
    N_("Received (being serviced)"),
    N_("Re-Pending (queued during service)"),
};

/** @brief Labely en_PIOZ80_ICFNC_RESULT (poslední vyhodnocení ICFNC). */
static const char *const g_pioz80_icfnc_result_names[] = {
    N_("FALSE"),
    N_("TRUE"),
};

/** @brief Labely en_PIOZ80_PORT_ID (PORT_A / PORT_B, NONE jako sentinel). */
static const char *const g_pioz80_port_id_names[] = {
    N_("Port A"),
    N_("Port B"),
};

/* ------------------------------------------------------------------
 * Helpery - render konkrétních polí Z80 PIO (lokální use).
 * ------------------------------------------------------------------ */

/**
 * @brief Vykreslí dvouřádkový rozpis bitů en_PIOZ80_INTERRUPT (bit 0 INT,
 *        bit 1 IEO) jako dvě flag řádky uvnitř existující tabulky.
 *
 * @param value  Hodnota `g_pioz80.interrupt` (kombinace INT a IEO bitů).
 *
 * @pre Volající musí mít otevřenou tabulku se 3 sloupci (Name / Value / Bin).
 */
static void render_pioz80_interrupt_bits(unsigned value)
{
    render_flag_row("INT (bit 0)", (value & PIOZ80_INTERRUPT_INT_BIT) ? 1 : 0,
                    _("Active = PIO holds /INT on the bus"));
    render_flag_row("IEO (bit 1)", (value & PIOZ80_INTERRUPT_IEO_BIT) ? 1 : 0,
                    _("Interrupt Enable Out (daisy chain to the next device)"));
}

/**
 * @brief Dekóduje Last Control Byte jako čitelný popis podle ctrl_expect
 *        kontextu (replay logiky z `pioz80_port_wr_ctrl` v `pioz80.c`).
 *
 * Mapování (read-only, žádné side-effecty):
 *  - PIOZ80_CWSTATE_EXPECT_INTMCW: byte je Interrupt Mask navazující na ICW
 *    (bit 0 = on, 1 = off, per ICMASK).
 *  - PIOZ80_CWSTATE_EXPECT_IOMCW: byte je IO Select navazující na Mode 3
 *    (bit 0 = output, 1 = input, per IO mask).
 *  - PIOZ80_CWSTATE_EXPECT_COMMAND a (byte & 0x01) == 0: Interrupt Vector
 *    Word (zápis IM2 vektoru, bit 0 vždy 0 v platném zápisu).
 *  - Jinak rozhoduje `byte & 0x0F`:
 *     - 0x0F (MCW): Mode Set, mode = (byte >> 6) & 0x03.
 *     - 0x07 (ICW): Interrupt Control Word, bit 7 ICENA, bit 6 ICFNC, bit 5
 *       ICLVL, bit 4 ICMSK (follow-up Interrupt Mask).
 *     - 0x03 (IDW): Interrupt Disable Word (zkrácená forma ICW), bit 7 = ICENA.
 *     - jiné: neplatný control word.
 *
 * @param byte           Hodnota mirror (`g_pioz80_port_a/b_last_ctrl_byte`).
 * @param expect_before  ctrl_expect platné PŘED zápisem tohoto byte.
 *                       Mirror v UI nemá historii pre-state, takže předáváme
 *                       AKTUÁLNÍ `port->ctrl_expect` jako přibližný kontext -
 *                       u Mode 3 a ICW s ICMSK_BIT se po zápisu posune dále,
 *                       u ostatních cest zůstává na COMMAND.
 *
 * @return Textový popis pro zobrazení v UI (vlastní pointer na string literál
 *         nebo statický buffer; není určen ke kopírování ani uchovávání mezi
 *         framy).
 */
static const char *pioz80_decode_last_ctrl_byte(uint8_t byte,
                                                en_PIOZ80_EXPECT expect_before)
{
    static char buf[128];

    if (expect_before == PIOZ80_CWSTATE_EXPECT_INTMCW)
    {
        snprintf(buf, sizeof(buf),
                 _("Interrupt Mask follow-up (0=on, 1=off): 0x%02X"), byte);
        return buf;
    }

    if (expect_before == PIOZ80_CWSTATE_EXPECT_IOMCW)
    {
        snprintf(buf, sizeof(buf),
                 _("IO Select follow-up (0=output, 1=input): 0x%02X"), byte);
        return buf;
    }

    /* expect = COMMAND -> dekód podle bitového vzoru. */
    if ((byte & PIOZ80_CTRL_IVW_MASK) == PIOZ80_CTRL_IVW)
    {
        snprintf(buf, sizeof(buf),
                 _("Interrupt Vector Word (IM2 vector = 0x%02X)"),
                 byte & 0xFE);
        return buf;
    }

    uint8_t cmd = byte & PIOZ80_CTRL_MASK;
    switch (cmd)
    {
        case PIOZ80_CTRL_MCW:
        {
            unsigned mode = (byte >> 6) & 0x03;
            const char *mode_label = (mode < 4) ? g_pioz80_port_mode_names[mode]
                                                : "???";
            snprintf(buf, sizeof(buf),
                     _("Mode Control Word -> %s"), mode_label);
            return buf;
        }
        case PIOZ80_CTRL_ICW:
        {
            int icena_bit = (byte & (PIOZ80_ICENA_BIT << 4)) ? 1 : 0;
            int icfnc_bit = (byte & (PIOZ80_ICFNC_BIT << 4)) ? 1 : 0;
            int iclvl_bit = (byte & (PIOZ80_ICLVL_BIT << 4)) ? 1 : 0;
            int icmsk_bit = (byte & (PIOZ80_ICMSK_BIT << 4)) ? 1 : 0;
            snprintf(buf, sizeof(buf),
                     _("Interrupt Control Word (ENA=%d FNC=%s LVL=%s MASK_follows=%d)"),
                     icena_bit,
                     icfnc_bit ? "AND" : "OR",
                     iclvl_bit ? "HIGH" : "LOW",
                     icmsk_bit);
            return buf;
        }
        case PIOZ80_CTRL_IDW:
        {
            int icena_bit = (byte >> 7) & 0x01;
            snprintf(buf, sizeof(buf),
                     _("Interrupt Disable Word (ENA=%d)"), icena_bit);
            return buf;
        }
        default:
            snprintf(buf, sizeof(buf),
                     _("Invalid control word (cmd=0x%X)"), cmd);
            return buf;
    }
}

/**
 * @brief Vykreslí kompletní obsah jednoho portu Z80 PIO uvnitř collapsing
 *        header.
 *
 * Sekce:
 *  - Last Control Byte (mirror) - hex + dekód podle ctrl_expect kontextu
 *  - Mode (Output / Input / Bidir / Control)
 *  - IO Mask (relevantní jen v Mode 3, jinak disabled popisek)
 *  - ctrl_expect (další očekávaný byte na control portu)
 *  - Data Output / Masked Input
 *  - Interrupt Vector (IM2, bit 0 vždy 0)
 *  - ICFNC / ICLVL / ICMASK / ICENA
 *  - Port-level INT stav (port_int) + last_intfnc_result
 *
 * @param port            Pointer na st_PIOZ80_PORT (= `&g_pioz80.port[i]`).
 * @param port_label      Anglický název portu pro UI ("Port A" / "Port B").
 * @param last_ctrl_byte  Mirror posledního zapsaného control byte pro tento
 *                        port (`g_pioz80_port_a/b_last_ctrl_byte`).
 * @param table_id_suffix Suffix pro `BeginTable` identifikátor (musí být
 *                        unikátní napříč voláními uvnitř jednoho framu).
 *
 * @pre `port != NULL`. Funkce se volá uvnitř `ImGui::Begin/End` okna.
 * @post Žádné side-effecty na `*port` ani na chipu.
 */
static void render_pioz80_port_section(const st_PIOZ80_PORT *port,
                                       const char *port_label,
                                       uint8_t last_ctrl_byte,
                                       const char *table_id_suffix)
{
    if (!ImGui::CollapsingHeader(port_label, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    char table_id[64];

    /* --- Last Control Byte --- */
    snprintf(table_id, sizeof(table_id), "##pioz80_lastcb_%s", table_id_suffix);
    if (ImGui::BeginTable(table_id, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Hex"));
        ImGui::TableSetupColumn(_("Bin"));
        ImGui::TableHeadersRow();

        render_hex8_row("last_ctrl_byte", last_ctrl_byte,
                        _("Mirror: last byte written to the control port (debug only)"));
        ImGui::EndTable();
    }

    /* Dekód Last Control Byte - kontext z aktuálního ctrl_expect (mirror
     * neuchovává pre-state, takže dekóduje jako pohled "co tento byte
     * znamenal vzhledem k current sequencer state"). */
    ImGui::TextDisabled("%s:", _("Decoded"));
    ImGui::SameLine();
    ImGui::TextWrapped("%s",
                       pioz80_decode_last_ctrl_byte(last_ctrl_byte,
                                                    port->ctrl_expect));

    /* --- State table --- */
    snprintf(table_id, sizeof(table_id), "##pioz80_state_%s", table_id_suffix);
    if (ImGui::BeginTable(table_id, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Value"));
        ImGui::TableSetupColumn(_("Bin"));
        ImGui::TableHeadersRow();

        /* Mode Control Register. */
        render_enum_row("mode", (unsigned)port->mode,
                        g_pioz80_port_mode_names,
                        (unsigned)(sizeof(g_pioz80_port_mode_names) /
                                   sizeof(g_pioz80_port_mode_names[0])),
                        _("Active port mode (Mode Set decoded from Last CB)"));

        /* IO Select Register - relevantní jen v Mode 3. */
        if (port->mode == PIOZ80_PORT_MODE3_USER)
        {
            render_hex8_row("io_mask", port->io_mask,
                            _("Mode 3 IO Mask: bit 0 = output, bit 1 = input"));
        }
        else
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("io_mask");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  _("IO Mask is relevant only in Mode 3 (Control)"));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("0x%02X", port->io_mask);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", _("(Mode 3 only)"));
        }

        /* Sequencer state - jaký další byte ctrl port očekává. */
        render_enum_row("ctrl_expect", (unsigned)port->ctrl_expect,
                        g_pioz80_expect_names,
                        (unsigned)(sizeof(g_pioz80_expect_names) /
                                   sizeof(g_pioz80_expect_names[0])),
                        _("Sequential state machine of the control port"));

        /* Data registry. */
        render_hex8_row("data_output", port->data_output,
                        _("Data Output Register - last byte written OUT (data port)"));
        render_hex8_row("masked_input", port->masked_input,
                        _("Masked input snapshot - what IN (data port) returns"));

        /* Interrupt vector pro IM2 mode (7-bit hodnota, bit 0 vždy 0). */
        render_hex8_row("interrupt_vector", port->interrupt_vector,
                        _("IM2 vector (7-bit, bit 0 always 0)"));

        /* Interrupt Control Word pole. */
        render_enum_row("icfnc", (unsigned)port->icfnc,
                        g_pioz80_icfnc_names,
                        (unsigned)(sizeof(g_pioz80_icfnc_names) /
                                   sizeof(g_pioz80_icfnc_names[0])),
                        _("Interrupt Control Function (OR / AND)"));
        render_enum_row("iclvl", (unsigned)port->iclvl,
                        g_pioz80_iclvl_names,
                        (unsigned)(sizeof(g_pioz80_iclvl_names) /
                                   sizeof(g_pioz80_iclvl_names[0])),
                        _("Interrupt Control Level (LOW / HIGH)"));
        render_hex8_row("icmask", port->icmask,
                        _("Interrupt Control Mask: bit 0 = on, bit 1 = off"));
        render_enum_row("icena", (unsigned)port->icena,
                        g_pioz80_icena_names,
                        (unsigned)(sizeof(g_pioz80_icena_names) /
                                   sizeof(g_pioz80_icena_names[0])),
                        _("Interrupt Enable (Enabled / Disabled)"));

        /* Port-level interrupt stav. */
        render_enum_row("port_int", (unsigned)port->port_int,
                        g_pioz80_port_int_names,
                        (unsigned)(sizeof(g_pioz80_port_int_names) /
                                   sizeof(g_pioz80_port_int_names[0])),
                        _("Current interrupt pipeline state of this port"));
        render_enum_row("last_intfnc_result",
                        (unsigned)port->last_intfnc_result,
                        g_pioz80_icfnc_result_names,
                        (unsigned)(sizeof(g_pioz80_icfnc_result_names) /
                                   sizeof(g_pioz80_icfnc_result_names[0])),
                        _("Last ICFNC evaluation (TRUE = INT condition met)"));
        ImGui::EndTable();
    }
}

/**
 * @brief Vykreslí ImGui okno se snapshotem stavu Z80 PIO.
 *
 * Side-effect free: čte výhradně `g_pioz80` a mirror byty.
 * Funkce existuje jen v buildech s `HAVE_PIOZ80 == 1` (MZ-800, MZ-1500).
 *
 * @param p_open  Pointer na visibility flag (nesmí být NULL).
 */
void imgui_pioz80_state_window(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("Z80 PIO State###PIOZ80State"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* ------------------------------------------------------------------
     * Global - INT/IEO daisy-chain stav + scheduled ICENA event.
     * ------------------------------------------------------------------ */
    if (ImGui::CollapsingHeader(_L("Global"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##pioz80_global", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn(_("Field"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableSetupColumn(_("Bin"));
            ImGui::TableHeadersRow();

            /* Raw hodnota interrupt registru + bit dekód. */
            render_hex8_row("interrupt", (uint8_t)g_pioz80.interrupt,
                            _("Daisy-chain interrupt state (bit 0 INT, bit 1 IEO)"));
            render_pioz80_interrupt_bits((unsigned)g_pioz80.interrupt);

            /* Který port požádal o INT. */
            const char *port_name;
            if (g_pioz80.interrupt_port_id == PIOZ80_PORT_A ||
                g_pioz80.interrupt_port_id == PIOZ80_PORT_B)
            {
                port_name = _(g_pioz80_port_id_names[g_pioz80.interrupt_port_id]);
            }
            else
            {
                port_name = _("(none)");
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("interrupt_port_id");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  _("Port holding the current INT (PORT_NONE = idle)"));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(port_name);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", (int)g_pioz80.interrupt_port_id);

            /* ICENA scheduled event (3 CPU takty po M1 v ICW_ENA). */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("icena_event.ticks");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  _("Deferred ICENA event (idle = -1, otherwise GDG ticks)"));
            ImGui::TableSetColumnIndex(1);
            /* Sentinel hodnota: pioz80.h porovnává `(int)ticks != -1`. */
            if ((int)g_pioz80.icena_event.ticks == -1)
            {
                ImGui::TextDisabled("%s", _("idle"));
            }
            else
            {
                ImGui::Text("%u", g_pioz80.icena_event.ticks);
            }
            ImGui::TableSetColumnIndex(2);
            if ((int)g_pioz80.icena_event.ticks == -1)
                ImGui::TextDisabled("-");
            else
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                   "%s", _("scheduled"));

            /* Který port event vyvolal. */
            const char *icena_port_name;
            if (g_pioz80.icena_event_port_id == PIOZ80_PORT_A ||
                g_pioz80.icena_event_port_id == PIOZ80_PORT_B)
            {
                icena_port_name =
                    _(g_pioz80_port_id_names[g_pioz80.icena_event_port_id]);
            }
            else
            {
                icena_port_name = _("(none)");
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("icena_event_port_id");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  _("Port for which icena_event is scheduled"));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(icena_port_name);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", (int)g_pioz80.icena_event_port_id);

            ImGui::EndTable();
        }
    }

    /* ------------------------------------------------------------------
     * Per-port - Port A / Port B.
     * ------------------------------------------------------------------ */
    render_pioz80_port_section(&g_pioz80.port[PIOZ80_PORT_A],
                               _L("Port A"),
                               g_pioz80_port_a_last_ctrl_byte,
                               "porta");
    render_pioz80_port_section(&g_pioz80.port[PIOZ80_PORT_B],
                               _L("Port B"),
                               g_pioz80_port_b_last_ctrl_byte,
                               "portb");

    ImGui::End();
}

