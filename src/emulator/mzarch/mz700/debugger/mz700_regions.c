/*
 * mz700_regions.c - REGION_LIST enumerace pro MZ-700
 *
 * Per-arch implementace pro REGION_LIST API. MZ-700 má:
 *   - Monitor ROM (4 KB lower + 8 KB upper)
 *   - User RAM 64 KB
 *   - VRAM 700 char + attr (2 + 2 KB, fyzicky jedno pole 4 KB)
 *   - Memext (Luftner/PEHU) přes port 0xE7
 *   - Ramdisk STD (port 0xEA) + Ramdisk PEZIK 0x68 (jen pi=0; PEZIK 0xE8
 *     není na MZ-700 v IORQ namapován, viz mz700_iorq.c poznámka u 0xE8)
 *   - PROHIBITED shadow
 *
 * MZ-700 nemá CG-ROM v ROM oblasti ani CG-RAM (= znakový generátor je
 * pevně v monitor ROM); CG-RAM koncept patří k MZ-800 (v MZ-700 compat
 * módu) a sem nepatří.
 *
 * Banking: g_memory.map flags ROM_0000 / ROM_E000 / PROHIBITED.
 *
 * Licence: GPLv3
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#if MZARCH == 700

#include <stdio.h>
#include <stdint.h>

#include "debugger/dbgapi_regions.h"
#include "debugger/dbgapi_regions_arch.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "mzarch/mz700/memory/mz700_memory.h"


/* Pomocný buffer pro snprintf jmen (lokální, vlákno-bezpečné jen pro
 * jednoho vyzyvatele - REGION_LIST je serialised přes dbgapi mutex). */
static char s_name_buf[64];


int mz700_regions_collect(st_REGION_DESC *out, int max_count)
{
    int count = 0;

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_LOGICAL, 0, "Logical Z80",
        0x0000, 0x10000, 1, 1, 1);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_RAM, 0, "User RAM (64 KB)",
        0x0000, MEMORY_SIZE_RAM, 1, 1, 1);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_ROM_LOWER, 0, "Monitor ROM lower (4 KB)",
        0x0000, ROM_SIZE_0000, 0, 1,
        MEMORY_MZ700_MAP_TEST_ROM_0000 ? 1 : 0);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_ROM_UPPER, 0, "Monitor ROM upper (8 KB)",
        0xE000, ROM_SIZE_E000, 0, 1,
        MEMORY_MZ700_MAP_TEST_ROM_E000 ? 1 : 0);

    /* VRAM 700 char + attr (fyzicky jedno pole g_memory.VRAM s velikostí
     * MEMORY_SIZE_VRAM = 0x1000 = 4 KB - viz memory.h:65). Znaková 2 KB
     * (D000-D7FF = VRAM[0x000-0x7FF]) + atributová 2 KB (D800-DFFF =
     * VRAM[0x800-0xFFF]); obraz používá prvních 1000 bajtů každé, zbytek
     * je RAM (např. blok B kitu SUC). */
    int vram_mapped = MEMORY_MZ700_MAP_TEST_VRAM_D000 ? 1 : 0;

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_CHAR, 0, "VRAM 700 char (2 KB)",
        0xD000, 0x0800, 1, 1, vram_mapped);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_ATTR, 0, "VRAM 700 attr (2 KB)",
        0xD800, 0x0800, 1, 1, vram_mapped);

    /* ---- Memext (Luftner/PEHU) - pokud připojen, port 0xE7. ---- *
     * Luftner: 128 RAM banks + 128 FLASH banks (sub_id 0x80..0xFF).
     * PEHU: 64 bank po 8 KB = 128 raw 4 KB kusů (MEMEXT_PEHU_RAW_BANKS),
     * žádný FLASH.
     * Region = raw 4 KB kus (MEMEXT_RAW_BANK_SIZE), sub_id = raw index. */
    if (MEMEXT_TEST_CONNECTED) {
        int is_luftner = MEMEXT_TEST_TYPE_LUFTNER ? 1 : 0;
        const char *type_label = is_luftner ? "Luftner" : "PEHU";
        int ram_bank_count = is_luftner ? MEMEXT_LUFTNER_BANKS : MEMEXT_PEHU_RAW_BANKS;

        for (int bank = 0; bank < ram_bank_count; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Memext %s RAM bank 0x%02X (4 KB)", type_label, bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_MEMEXT_RAM, bank, s_name_buf,
                0xFFFFFFFFu, MEMEXT_RAW_BANK_SIZE, 1, 1, 0);
        }

        if (is_luftner) {
            for (int bank = MEMEXT_LUFTNER_BANKS;
                 bank < 2 * MEMEXT_LUFTNER_BANKS; bank++)
            {
                snprintf(s_name_buf, sizeof(s_name_buf),
                    "Memext Luftner FLASH bank 0x%02X (4 KB)", bank);
                dbgapi_regions_add(out, &count, max_count,
                    REGION_KIND_MEMEXT_FLASH, bank, s_name_buf,
                    0xFFFFFFFFu, MEMEXT_RAW_BANK_SIZE, 0, 1, 0);
            }
        }
    }

    /* ---- Ramdisk STD: enumerace per bank (každá 64 KB), port 0xEA. ----
     * Počet bank = (size_mask + 1). R/O dle type. */
    if (RAMDISK_TEST_STD_CONNECTED) {
        int n_banks = g_ramdisk.std.size + 1;
        int is_ro = (g_ramdisk.std.type & RAMDISK_IS_READONLY) ? 1 : 0;
        for (int bank = 0; bank < n_banks; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Ramdisk STD bank 0x%02X (64 KB)", bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_RAMDISK_STD, bank, s_name_buf,
                0xFFFFFFFFu, 0x10000, is_ro ? 0 : 1, 1, 0);
        }
    }

    /* ---- Ramdisk PEZIK 0x68 (jen instance pi=0). ---- *
     * 8 banků × 64 KB = 512 KB. sub_id 0..7 = bank index 0..7.
     * PEZIK 0xE8 (instance pi=1) není na MZ-700 v IORQ namapován, takže
     * se ho ani neenumeruje (viz mz700_iorq.c, poznámka u 0xE8). */
    if (g_ramdisk.pezik[RAMDISK_PEZIK_68].connected) {
        for (int bank = 0; bank < 8; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Ramdisk PEZIK 68 bank 0x%02X (64 KB)", bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_RAMDISK_PEZIK, bank, s_name_buf,
                0xFFFFFFFFu, 0x10000, 1, 1, 0);
        }
    }

    /* PROHIBITED shadow - jen pokud aktivní. */
    if (MEMORY_MZ700_MAP_TEST_PROHIBITED) {
        dbgapi_regions_add(out, &count, max_count,
            REGION_KIND_PROHIBITED_SHADOW, 0, "Prohibited shadow (0xFF)",
            0xE800, 0x10000 - 0xE800, 0, 1, 1);
    }

    return count;
}

#endif /* MZARCH == 700 */
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
