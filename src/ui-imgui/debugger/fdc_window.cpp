/**
 * @file fdc_window.cpp
 * @brief Debugger: FDC chip state inspector window.
 *
 * Real-time snapshot vnitřního stavu FDC chipu - registry, state machine,
 * sticky flags, drives. Slouží pro ladění chování chipu při bootech /
 * formátech / I/O sekvencích.
 *
 * License: GPLv3.
 */

#include "main.h"
#include "emulator/mzarch/mzhal.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>

#include "i18n.h"

#include "mzarch/mzcommon_config.h"

#include "ui-imgui/debugger/chip_window_helpers.h"

#include "hw-generic/fdc/fdc.h"

#include "hw-generic/fdc/wd279x.h"

extern "C"
{
    void imgui_fdc_state_window(bool *p_open);
}


/** Render chip state dané instance FDC. */
static void render_new_chip_state(st_FDC *fdc)
{
    const st_WD279X *c = &fdc->wd279x;

    if (ImGui::CollapsingHeader(_L("Registers (true-bus)"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##fdc_regs", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn(_("Reg"));
            ImGui::TableSetupColumn(_("Hex"));
            ImGui::TableSetupColumn(_("Bin"));
            ImGui::TableHeadersRow();

            render_hex8_row("STATUS",   c->regSTATUS,
                            _("Status registr (per type I/II/III sémantika)"));
            render_hex8_row("COMMAND",  c->regCOMMAND,
                            _("Poslední přijatý příkaz (true-bus opcode)"));
            render_hex8_row("TRACK",    c->regTRACK);
            render_hex8_row("SECTOR",   c->regSECTOR);
            render_hex8_row("DATA",     c->regDATA);
            render_hex8_row("MOTOR",    c->MOTOR,
                            _("bit 0..1 = drive id, bit 2 = EDR, bit 7 = motor"));
            render_hex8_row("SIDE",     c->SIDE);
            render_hex8_row("DENSITY",  c->DENSITY);
            render_hex8_row("EINT",     c->EINT,
                            _("HD Patch interrupt enable (port 0xDFh)"));
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(_L("Transfer state"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##fdc_xfer", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn(_("Field"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableSetupColumn(_("Bin"));
            ImGui::TableHeadersRow();

            render_dec_row("data_counter",        c->data_counter,
                           _("Zbývajících bytů pro R/W transfer (0 = idle)"));
            render_dec_row("buffer_pos",          c->buffer_pos);
            render_dec_row("current_sector_size", c->current_sector_size);
            render_dec_row("status_mode", (unsigned)c->status_mode,
                           _("0 = Type I, 1 = Type II/III"));
            render_dec_row("multiblock_rw",       c->multiblock_rw);
            render_dec_row("reading_status_counter", c->reading_status_counter);
            render_dec_row("waitForInt",          c->waitForInt,
                           _("HD Patch INT throttle counter (> 2 fires)"));
            render_dec_row("positioned_track",    c->positioned_track);
            render_dec_row("positioned_side",     c->positioned_side);
            render_dec_row("positioned_sector",   c->positioned_sector);
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(_L("Sticky flags"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("##fdc_flags", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn(_("Flag"));
            ImGui::TableSetupColumn(_("Set"));
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();

            render_flag_row("intrq_active",        c->intrq_active);
            render_flag_row("pending_busy_status", c->pending_busy_status);
            render_flag_row("pending_drq",         c->pending_drq);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("direction_latch");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%+d", (int)c->direction_latch);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("-");
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(_L("Type III state (WRITE/READ TRACK)"), 0))
    {
        if (ImGui::BeginTable("##fdc_type3", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn(_("Field"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableSetupColumn(_("Bin"));
            ImGui::TableHeadersRow();

            render_dec_row("write_track_stage",   c->write_track_stage,
                           _("0=idle, 1=wait IDAM, 2=ID, 3=data, 4=wait next, 5=done"));
            render_dec_row("write_track_counter", c->write_track_counter);
            render_dec_row("rt_sectors",          c->rt_sectors);
            render_dec_row("rt_sector_bytes",     c->rt_sector_bytes);
            render_dec_row("rt_ssize_code",       c->rt_ssize_code);
            render_dec_row("rt_cached_sec_idx",   (unsigned)(c->rt_cached_sec_idx & 0xFF));
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(_L("Drives"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (unsigned i = 0; i < FDC_NUM_DRIVES; i++)
        {
            const st_FDDrive *d = &fdc->drive[i];
            char hdr[64];
            snprintf(hdr, sizeof(hdr), "FDD %u %s%s###fdc%u_drv_%u", i,
                     d->mounted ? "(mounted)" : "(empty)",
                     d->readonly ? " [R/O]" : "",
                     fdc->index, i);
            if (!ImGui::TreeNode(hdr))
                continue;

            const char *mode = (d->storage_mode == FDC_STORAGE_DIRECT)  ? "direct"
                             : (d->storage_mode == FDC_STORAGE_DISCARD) ? "discard"
                                                                            : "cached";

            ImGui::Text("file: %s", d->mounted ? d->filename : "-");
            ImGui::Text("storage: %s   readonly: %d   user_ro: %d   fs_ro: %d",
                        mode, d->readonly, d->user_readonly, d->fs_readonly);

            if (d->geometry_valid)
            {
                ImGui::Text("geom: tracks=%u sides=%u total=%u image=%u B",
                            (unsigned)d->geometry.tracks,
                            (unsigned)d->geometry.sides,
                            (unsigned)d->geometry.total_tracks,
                            (unsigned)d->geometry.image_size);
            }
            else
            {
                ImGui::TextDisabled("geom: (none)");
            }

            ImGui::TreePop();
        }
    }
}


void imgui_fdc_state_window(bool *p_open)
{
    if (g_mzhal.have_fdc) { /* runtime capability, mzhal krok 8 */
    ImGui::SetNextWindowSize(ImVec2(420, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("FDC State"), p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* Záložka pro každou instanci FDC (FDC0 standard / FDC1 secondary). */
    if (ImGui::BeginTabBar("##fdc_state_tabs"))
    {
        for (unsigned n = 0; n < FDC_INSTANCE_COUNT; n++)
        {
            const char *tab_label = (n == FDC0) ? _L("FDC0 (standard)")
                                                 : _L("FDC1 (secondary)");
            if (ImGui::BeginTabItem(tab_label))
            {
                ImGui::Text(_("Connected: %s"),
                            (g_fdc[n].connected == FDC_CONNECTED) ? _("yes") : _("no"));
                ImGui::Separator();
                render_new_chip_state(&g_fdc[n]);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    } else {
    (void)p_open;
    }
}
