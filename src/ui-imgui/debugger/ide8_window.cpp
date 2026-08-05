/**
 * @file ide8_window.cpp
 * @brief Debugger: IDE8 State inspector window.
 *
 * Real-time snapshot stavu IDE8 radice (8-bit IDE/ATA rozhrani pro 2 HDD):
 *   - Controller: sdilene registry (sector count/sector/cylinder/head/
 *     features) + vybrana mechanika,
 *   - per drive (Master/Slave): mount + backing soubor, CHS geometrie,
 *     kapacita, posledni prikaz, addressing (CHS/LBA), busmode (8/16 bit),
 *     dekodovany status registr a aktualni blok/transfer pozice.
 *
 * Side-effect free: jen cte z `g_ide8`, zadny setter/write. Refresh per
 * frame. Vzorem je qdisk_window.cpp.
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

#include "hw-generic/ide8/ide8.h"

extern "C"
{
    void imgui_ide8_state_window(bool *p_open);
}


/**
 * @brief Vrati textovou reprezentaci en_IDE8_CMD (posledni prikaz mechaniky).
 */
static const char *ide8_cmd_str(en_IDE8_CMD cmd)
{
    switch (cmd)
    {
    case IDE8_CMD_NONE:
        return "NONE";
    case IDE8_CMD_RESET:
        return "RESET";
    case IDE8_CMD_SECTOR_READ:
        return "SECTOR_READ";
    case IDE8_CMD_SECTOR_WRITE:
        return "SECTOR_WRITE";
    case IDE8_CMD_SET_FEATURES:
        return "SET_FEATURES";
    default:
        return "?";
    }
}

/**
 * @brief Vykresli dekodovany IDE8 status registr (raw + bit-by-bit).
 *
 * @param status Hodnota drive->status (kombinace IDE8_STS_* bitu).
 */
static void render_decoded_status(uint8_t status)
{
    ImGui::Text("status: 0x%02X", (unsigned)status);
    ImGui::Text("  BUSY:      %d", (status & IDE8_STS_BUSY)      ? 1 : 0);
    ImGui::Text("  READY:     %d", (status & IDE8_STS_READY)     ? 1 : 0);
    ImGui::Text("  WRERROR:   %d", (status & IDE8_STS_WRERROR)   ? 1 : 0);
    ImGui::Text("  SEEKOK:    %d", (status & IDE8_STS_SEEKOK)    ? 1 : 0);
    ImGui::Text("  DRQ:       %d", (status & IDE8_STS_DRQ)       ? 1 : 0);
    ImGui::Text("  CORRECTED: %d", (status & IDE8_STS_CORRECTED) ? 1 : 0);
    ImGui::Text("  INDEX:     %d", (status & IDE8_STS_INDEX)     ? 1 : 0);
    ImGui::Text("  ERROR:     %d", (status & IDE8_STS_ERROR)     ? 1 : 0);
}

/**
 * @brief Vykresli sekci se stavem jedne IDE8 mechaniky.
 *
 * @param drive Pointer na st_IDE8_DRIVE (Master nebo Slave).
 */
static void render_drive_section(const st_IDE8_DRIVE *drive)
{
    bool connected = (drive->connected == IDE8_STATE_CONNECTED);
    ImGui::Text("connected: %s", connected ? "YES" : "NO");

    const char *fp = (drive->filepath && drive->filepath[0]) ? drive->filepath : "-";
    ImGui::Text("filepath:  %s", fp);
    ImGui::Text("file:      %s", drive->fp ? "open" : "closed");

    ImGui::Separator();
    ImGui::Text("geometry C/H/S: %d / %d / %d", drive->geo_c, drive->geo_h, drive->geo_s);

    /* Kapacita = total_blocks * 512 B (IDE8_SECTOR_SIZE). */
    unsigned long total_bytes = (unsigned long)drive->total_blocks * (unsigned long)IDE8_SECTOR_SIZE;
    ImGui::Text("total_blocks:   %u (%lu MB)",
                (unsigned)drive->total_blocks, total_bytes / (1024ul * 1024ul));

    ImGui::Separator();
    ImGui::Text("cmd:        0x%02X (%s)", (unsigned)drive->cmd, ide8_cmd_str(drive->cmd));
    ImGui::Text("addressing: %s", (drive->addressing == IDE8_ADDRESSING_LBA) ? "LBA" : "CHS");
    ImGui::Text("busmode:    %s", (drive->busmode == IDE8_BUSMODE_8) ? "8-bit" : "16-bit");
    ImGui::Text("block:      %u (0x%08X)", (unsigned)drive->block, (unsigned)drive->block);
    ImGui::Text("data_pos:     %d", drive->data_pos);
    ImGui::Text("sector_count: %d", drive->sector_count);

    ImGui::Separator();
    render_decoded_status(drive->status);
}


/**
 * @brief Render IDE8 State debugger okno (viz ide8_window.h).
 */
void imgui_ide8_state_window(bool *p_open)
{
    if (g_mzhal.have_ide8) { /* runtime capability, mzhal krok 8 */
    ImGui::SetNextWindowSize(ImVec2(440, 560), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("IDE8 State"), p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* === Controller === (default open) */
    if (ImGui::CollapsingHeader(_L("Controller"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("selected:        %s",
                    (g_ide8.selected == IDE8_DRIVE_SLAVE) ? "SLAVE" : "MASTER");
        ImGui::Separator();
        ImGui::Text("regSECTOR_COUNT: 0x%02X", (unsigned)g_ide8.regSECTOR_COUNT);
        ImGui::Text("regSECTOR:       0x%02X", (unsigned)g_ide8.regSECTOR);
        ImGui::Text("regCYLINDER:     0x%04X", (unsigned)g_ide8.regCYLINDER);
        ImGui::Text("regHEAD:         0x%02X", (unsigned)g_ide8.regHEAD);
        ImGui::Text("regFEATURES:     0x%02X", (unsigned)g_ide8.regFEATURES);
    }

    /* === Master === (default open) */
    if (ImGui::CollapsingHeader(_L("Drive Master"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_drive_section(&g_ide8.drive[IDE8_DRIVE_MASTER]);
    }

    /* === Slave === (sbaleno) */
    if (ImGui::CollapsingHeader(_L("Drive Slave"), 0))
    {
        render_drive_section(&g_ide8.drive[IDE8_DRIVE_SLAVE]);
    }

    ImGui::End();
    } else {
    (void)p_open;
    }
}
