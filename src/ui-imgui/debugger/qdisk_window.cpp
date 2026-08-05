/**
 * @file qdisk_window.cpp
 * @brief Debugger: QDisk chip state inspector window.
 *
 * Real-time snapshot vnitrniho stavu QuickDisk subsystemu - SIO kanaly A/B
 * (registry W0-W7, R0-R2), drive stav (mount, R/O 3-state, storage_mode,
 * dirty flag, position), v rezimu VIRTUAL i virt state machine. Side-effect
 * free: jen cte z `g_qdisk`, zadny setter/write. Refresh per frame.
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

#include "hw-generic/qdisk/qdisk.h"

extern "C"
{
    void imgui_qdisk_state_window(bool *p_open);
}


/**
 * @brief Vykresli jeden radek tabulky "Reg | Hex | Bin" pro 8-bit hodnotu.
 *
 * @param name    Nazev registru (zobrazi se v 1. sloupci, bez prekladu).
 * @param value   Hodnota pro hex + bin zobrazeni.
 * @param tooltip Volitelny tooltip nad nazvem registru (NULL = bez).
 */
static void render_hex8_row(const char *name, uint8_t value, const char *tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("0x%02X", value);
    ImGui::TableSetColumnIndex(2);
    char binbuf[10];
    for (int i = 7; i >= 0; --i)
        binbuf[7 - i] = (value & (1u << i)) ? '1' : '0';
    binbuf[8] = '\0';
    ImGui::Text("%s", binbuf);
}

/**
 * @brief Vrati textovou reprezentaci QDISK_TYPE_*.
 */
static const char *qdisk_type_str(unsigned t)
{
    switch (t)
    {
    case QDISK_TYPE_IMAGE:
        return "IMAGE";
    case QDISK_TYPE_VIRTUAL:
        return "VIRTUAL";
    case QDISK_TYPE_UNICARD:
        return "UNICARD";
    default:
        return "?";
    }
}

/**
 * @brief Vrati textovou reprezentaci en_QDISK_STORAGE_MODE.
 */
static const char *qdisk_storage_str(en_QDISK_STORAGE_MODE m)
{
    switch (m)
    {
    case QDISK_STORAGE_CACHED:
        return "cached";
    case QDISK_STORAGE_DIRECT:
        return "direct";
    case QDISK_STORAGE_DISCARD:
        return "discard";
    default:
        return "?";
    }
}

/**
 * @brief Vykresli decodovany QDSTS_* status (raw + bit-by-bit).
 *
 * @param status Hodnota g_qdisk.status (kombinace QDSTS_IMG_READY a dalsich).
 */
static void render_decoded_qdsts(unsigned status)
{
    ImGui::Text("Raw:          0x%X", status);
    ImGui::Text("  IMG_READY:    %s", (status & QDSTS_IMG_READY)    ? "1" : "0");
    ImGui::Text("  HEAD_HOME:    %s", (status & QDSTS_HEAD_HOME)    ? "1" : "0");
    ImGui::Text("  IMG_SYNC:     %s", (status & QDSTS_IMG_SYNC)     ? "1" : "0");
    ImGui::Text("  IMG_READONLY: %s", (status & QDSTS_IMG_READONLY) ? "1" : "0");
}

/**
 * @brief Vykresli tabulku SIO registru pro jeden kanal (Wreg[0-7] + Rreg[0-2]).
 *
 * @param table_id ImGui table ID (musi byt unique mezi kanaly A/B).
 * @param c        Pointer na SIO channel strukturu.
 */
static void render_sio_channel_regs(const char *table_id, const st_QDSIO_CHANNEL *c)
{
    ImGui::Text("REG_addr: %u", (unsigned)c->REG_addr);
    if (ImGui::BeginTable(table_id, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn(_("Reg"));
        ImGui::TableSetupColumn(_("Hex"));
        ImGui::TableSetupColumn(_("Bin"));
        ImGui::TableHeadersRow();

        char rname[8];
        for (int i = 0; i < 8; i++)
        {
            snprintf(rname, sizeof(rname), "WR%d", i);
            render_hex8_row(rname, c->Wreg[i]);
        }
        for (int i = 0; i < 3; i++)
        {
            snprintf(rname, sizeof(rname), "RR%d", i);
            render_hex8_row(rname, c->Rreg[i]);
        }
        ImGui::EndTable();
    }
}


void imgui_qdisk_state_window(bool *p_open)
{
    if (g_mzhal.have_qdisk) { /* runtime capability, mzhal krok 8 */
    ImGui::SetNextWindowSize(ImVec2(420, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("QDisk State"), p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* === Device === */
    if (ImGui::CollapsingHeader(_L("Device"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("connected: %s", (g_qdisk.connected == QDISK_CONNECTED) ? "YES" : "NO");
        ImGui::Text("type:      %s", qdisk_type_str(g_qdisk.type));
        ImGui::Spacing();
        render_decoded_qdsts(g_qdisk.status);
    }

    /* === SIO Channel A === (default open) */
    if (ImGui::CollapsingHeader(_L("SIO Channel A"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_sio_channel_regs("##qd_sio_a", &g_qdisk.channel[QDSIO_CHANNEL_A]);
    }

    /* === SIO Channel B === (sbaleno) */
    if (ImGui::CollapsingHeader(_L("SIO Channel B"), 0))
    {
        render_sio_channel_regs("##qd_sio_b", &g_qdisk.channel[QDSIO_CHANNEL_B]);
    }

    /* === Drive / Storage === (default open) */
    if (ImGui::CollapsingHeader(_L("Drive / Storage"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char *fn = g_qdisk.filename[0] ? g_qdisk.filename : "-";
        ImGui::Text("file: %s", fn);

        unsigned pos = g_qdisk.image_position;
        float pct = (QDISK_IMAGE_MAX_SIZE > 0)
                  ? (100.0f * (float)pos / (float)QDISK_IMAGE_MAX_SIZE)
                  : 0.0f;
        ImGui::Text("position: %u / %u (%.1f%%)",
                    pos, (unsigned)QDISK_IMAGE_MAX_SIZE, pct);

        ImGui::Text("storage_mode: %s", qdisk_storage_str(g_qdisk.storage_mode));
        ImGui::Text("readonly:     %d (user=%d fs=%d)",
                    g_qdisk.readonly, g_qdisk.user_readonly, g_qdisk.fs_readonly);
        ImGui::Text("out_crc16:    0x%04X", g_qdisk.out_crc16);

        ImGui::Separator();
        ImGui::Text("handler_valid: %d", g_qdisk.handler_valid);
        if (g_qdisk.handler_valid)
        {
            const char *htype = (g_qdisk.handler.type == HANDLER_TYPE_MEMORY) ? "MEMORY"
                              : (g_qdisk.handler.type == HANDLER_TYPE_FILE)   ? "FILE"
                                                                              : "?";
            ImGui::Text("  type:   %s", htype);
            ImGui::Text("  status: 0x%X (READY=%d RO=%d)",
                        (unsigned)g_qdisk.handler.status,
                        (g_qdisk.handler.status & HANDLER_STATUS_READY) ? 1 : 0,
                        (g_qdisk.handler.status & HANDLER_STATUS_READ_ONLY) ? 1 : 0);
            if (g_qdisk.handler.type == HANDLER_TYPE_MEMORY)
            {
                ImGui::Text("  buffer size: %zu B", g_qdisk.handler.spec.memspec.size);
                ImGui::Text("  open_size:   %zu B", g_qdisk.handler.spec.memspec.open_size);
                ImGui::Text("  updated:     %d", g_qdisk.handler.spec.memspec.updated);
            }
        }

        ImGui::Separator();
        ImGui::Text("has_unsaved_changes: %d", qdisk_drive_has_unsaved_changes());
        ImGui::Text("has_ram_changes:     %d", qdisk_drive_has_ram_changes());
    }

    /* === Virtual mode === (jen pokud VIRTUAL, default open) */
    if (g_qdisk.type == QDISK_TYPE_VIRTUAL)
    {
        if (ImGui::CollapsingHeader(_L("Virtual mode"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("virt_status:    %u", g_qdisk.virt_status);
            ImGui::Text("files_count:    %u", g_qdisk.virt_files_count);
            ImGui::Text("file_num:       %u", g_qdisk.virt_file_num);
            ImGui::Text("saved_filename: %s",
                        g_qdisk.virt_saved_filename[0] ? g_qdisk.virt_saved_filename : "-");
            ImGui::Text("mzfbody_size:   %u", (unsigned)g_qdisk.virt_mzfbody_size);
            ImGui::Text("mzf_fp:         %s", g_qdisk.mzf_fp ? "open" : "closed");
        }
    }

    ImGui::End();
    } else {
    (void)p_open;
    }
}
