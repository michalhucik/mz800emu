/**
 * @file ramdisk_window.cpp
 * @brief Debugger: Memory Disk State inspector window.
 *
 * Real-time snapshot stavu vsech paametovych disku (ramdisku) emulatoru:
 *   - MR-1R18 kompatibilni standardni ramdisk (typ STD/SRAM/ROM, velikost,
 *     aktualni banka, offset, backing soubor),
 *   - Pezik 0xE8-0xEF (latch, portmask, backup, soubor) - jen MZ-700/MZ-800,
 *   - Pezik 0x68-0x6F (latch, portmask, backup, soubor).
 *
 * Side-effect free: jen cte z `g_ramdisk`, zadny setter/write. Refresh per
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

#include "hw-generic/ramdisk/ramdisk.h"

extern "C"
{
    void imgui_ramdisk_state_window(bool *p_open);
}


/**
 * @brief Vrati textovou reprezentaci en_RAMDISK_TYPE standardniho ramdisku.
 *
 * @param t Hodnota g_ramdisk.std.type.
 * @return Staticky retezec ("STD (volatile)" / "SRAM (backed)" /
 *         "ROM (read-only)" / "?").
 */
static const char *ramdisk_std_type_str(en_RAMDISK_TYPE t)
{
    switch (t)
    {
    case RAMDISK_TYPE_STD:
        return "STD (volatile)";
    case RAMDISK_TYPE_SRAM:
        return "SRAM (backed)";
    case RAMDISK_TYPE_ROM:
        return "ROM (read-only)";
    default:
        return "?";
    }
}

/**
 * @brief Zformatuje kapacitu ramdisku z bankmask hodnoty do bufferu.
 *
 * Pocet bank = bankmask + 1 (0x00=1, 0x03=4, 0x07=8, 0x0f=16, 0xff=256),
 * kazda banka ma 64 KB. Vystup je napr. "4 banks x 64 KB = 256 KB".
 *
 * @param buf      Cilovy buffer.
 * @param bufsize  Velikost bufferu v bytech.
 * @param bankmask Hodnota g_ramdisk.std.size (en_RAMDISK_BANKMASK).
 */
static void ramdisk_std_size_str(char *buf, size_t bufsize, en_RAMDISK_BANKMASK bankmask)
{
    unsigned banks = (unsigned)bankmask + 1u;
    unsigned long total_kb = (unsigned long)banks * 64ul;
    if (total_kb >= 1024ul)
        snprintf(buf, bufsize, "%u banks x 64 KB = %lu MB", banks, total_kb / 1024ul);
    else
        snprintf(buf, bufsize, "%u banks x 64 KB = %lu KB", banks, total_kb);
}

/**
 * @brief Vykresli sekci se stavem standardniho MR-1R18 ramdisku.
 */
static void render_std_section(void)
{
    const st_RAMDISKSTD *r = &g_ramdisk.std;
    bool connected = (r->connected == RAMDISK_CONNECTED);

    ImGui::Text("connected: %s", connected ? "YES" : "NO");
    ImGui::Text("type:      %s", ramdisk_std_type_str(r->type));

    char sizebuf[48];
    ramdisk_std_size_str(sizebuf, sizeof(sizebuf), r->size);
    ImGui::Text("size:      0x%02X (%s)", (unsigned)r->size, sizebuf);

    ImGui::Separator();
    ImGui::Text("bank:      %u (0x%02X)", (unsigned)r->bank, (unsigned)r->bank);
    ImGui::Text("offset:    %u (0x%04X)", (unsigned)r->offset, (unsigned)r->offset);

    ImGui::Separator();
    const char *fp = (r->filepath && r->filepath[0]) ? r->filepath : "-";
    ImGui::Text("filepath:  %s", fp);
    ImGui::Text("memory:    %s", r->memory ? "allocated" : "NULL");
}

/**
 * @brief Vykresli sekci se stavem jednoho Pezik ramdisku.
 *
 * @param pezik_type RAMDISK_PEZIK_E8 nebo RAMDISK_PEZIK_68.
 */
static void render_pezik_section(int pezik_type)
{
    const st_RAMDISKPEZIK *p = &g_ramdisk.pezik[pezik_type];
    bool connected = (p->connected == RAMDISK_CONNECTED);

    ImGui::Text("connected: %s", connected ? "YES" : "NO");
    ImGui::Text("latch:     0x%04X", (unsigned)p->latch);
    ImGui::Text("portmask:  0x%02X", (unsigned)p->portmask);
    ImGui::Text("backuped:  %s", (p->backuped == PEZIK_BACKUPED_YES) ? "YES" : "NO");

    /* Pezik ma pevnou velikost 8 bank x 64 KB = 512 KB. */
    ImGui::Text("size:      8 banks x 64 KB = 512 KB (fixed)");

    ImGui::Separator();
    const char *fp = (p->filepath && p->filepath[0]) ? p->filepath : "-";
    ImGui::Text("filepath:  %s", fp);
    ImGui::Text("memory:    %s", p->memory ? "allocated" : "NULL");
}


/**
 * @brief Render Memory Disk State debugger okno (viz ramdisk_window.h).
 */
void imgui_ramdisk_state_window(bool *p_open)
{
    if (g_mzhal.have_ramdisk) { /* runtime capability, mzhal krok 8 */
    ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("Memory Disk State"), p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* === MR-1R18 (Standard) === (default open) */
    if (ImGui::CollapsingHeader(_L("MR-1R18 (Standard)"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_std_section();
    }

    /* === Pezik 0xE8 - 0xEF === (jen MZ-700/MZ-800; MZ-1500 tento port nema) */
/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
    if (g_mzhal.arch != 1500
        && ImGui::CollapsingHeader(_L("Pezik 0xE8 - 0xEF"), 0))
    {
        render_pezik_section(RAMDISK_PEZIK_E8);
    }

    /* === Pezik 0x68 - 0x6F === */
    if (ImGui::CollapsingHeader(_L("Pezik 0x68 - 0x6F"), 0))
    {
        render_pezik_section(RAMDISK_PEZIK_68);
    }

    ImGui::End();
    } else {
    (void)p_open;
    }
}
