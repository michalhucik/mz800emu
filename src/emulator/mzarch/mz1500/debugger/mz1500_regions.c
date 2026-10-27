/*
 * mz1500_regions.c - REGION_LIST enumerace pro MZ-1500
 *
 * Per-arch implementace pro REGION_LIST API. MZ-1500 má:
 *   - Standardní monitor ROM (4 KB lower + 8 KB upper)
 *   - CG-ROM (4 KB, mapovaná do D000-EFFF přes SPEC=1)
 *   - VRAM 700 char + attr (společné s MZ-700)
 *   - PCG bank 1/2/3 (3× 8 KB, mapované do D000-EFFF přes SPEC=2/3/4)
 *   - Memext (Luftner/PEHU) - port 0xE7 stejný jako MZ-800
 *   - Ramdisk STD (port 0xEA) + Ramdisk PEZIK 0x68 (jen pi=0); PEZIK 0xE8
 *     není na MZ-1500 v IORQ namapován (port 0xE9 koliduje s PSG, viz
 *     mz1500_iorq.c poznámka u 0xE8)
 *   - Bez PROHIBITED (koncept neexistuje)
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

#if MZARCH == 1500

#include <stdio.h>
#include <stdint.h>

#include "debugger/dbgapi_regions.h"
#include "debugger/dbgapi_regions_arch.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "mzarch/mz1500/memory/mz1500_memory.h"


static char s_name_buf[64];


int mz1500_regions_collect(st_REGION_DESC *out, int max_count)
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
        MEMORY_MZ1500_MAP_TEST_ROM_0000 ? 1 : 0);

    /* Monitor ROM upper - MZ-1500 mapuje E800-FFFF (= 0x1800 bajtů viditelné),
     * pole je 0x2000 (8 KB) jako u MZ-800/700 (rom.h). Logical_base 0xE000
     * pro konzistenci s MZ-800 (přesný viditelný rozsah řídí banking,
     * ale data jsou v ROM[0x2000..0x3FFF] vždy stejná). */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_ROM_UPPER, 0, "Monitor ROM upper (8 KB)",
        0xE000, ROM_SIZE_E000, 0, 1,
        MEMORY_MZ1500_MAP_TEST_ROM_UPPER ? 1 : 0);

    /* CG-ROM 4 KB. MZ-1500 ji mapuje přes SPEC=1 do D000-EFFF (Doxygen
     * memory.h:140). Logical_base 0xD000 (= aktuální mapování při SPEC=1). */
    int spec_id = MEMORY_MZ1500_SPEC_ID; /* 0=VRAM_TEXT, 1=CGROM, 2..4=PCG */
    int rom_upper = MEMORY_MZ1500_MAP_TEST_ROM_UPPER ? 1 : 0;

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_CGROM, 0, "CG-ROM (4 KB)",
        0xD000, ROM_SIZE_CGROM, 0, 1,
        (rom_upper && spec_id == 1) ? 1 : 0);

    /* VRAM 700 char + attr (společné s MZ-700, MEMORY_SIZE_VRAM = 0x1000):
     * znaková 2 KB (D000-D7FF) + atributová 2 KB (D800-DFFF). */
    int vram_mapped = (rom_upper && spec_id == 0) ? 1 : 0;

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_CHAR, 0, "VRAM 700 char (2 KB)",
        0xD000, 0x0800, 1, 1, vram_mapped);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_ATTR, 0, "VRAM 700 attr (2 KB)",
        0xD800, 0x0800, 1, 1, vram_mapped);

    /* PCG banky 1, 2, 3 (každá 8 KB). Logical base D000 (kde se mapuje
     * přes SPEC=2/3/4). g_memory.PCG[MEMORY_SIZE_PCG] = 3 * 8 KB. */
    for (int pcg_bank = 0; pcg_bank < 3; pcg_bank++) {
        snprintf(s_name_buf, sizeof(s_name_buf),
            "PCG bank %d (8 KB)", pcg_bank + 1);
        int pcg_mapped = (rom_upper && spec_id == (pcg_bank + 2)) ? 1 : 0;
        dbgapi_regions_add(out, &count, max_count,
            REGION_KIND_PCG_1500, pcg_bank, s_name_buf,
            0xD000, MEMORY_SIZE_PCG_BANK, 1, 1, pcg_mapped);
    }

    /* Memext (Luftner/PEHU) - pokud připojen. MZ-1500 má port 0xE7.
     * Luftner: 128 RAM banks + 128 FLASH banks. PEHU: 64 bank po 8 KB =
     * 128 raw 4 KB kusů (MEMEXT_PEHU_RAW_BANKS), žádný FLASH.
     * Region = raw 4 KB kus, sub_id = raw index. */
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
     * PEZIK 0xE8 (instance pi=1) není na MZ-1500 v IORQ namapován
     * (port 0xE9 koliduje s PSG, viz mz1500_iorq.c poznámka u 0xE8). */
    if (g_ramdisk.pezik[RAMDISK_PEZIK_68].connected) {
        for (int bank = 0; bank < 8; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Ramdisk PEZIK 68 bank 0x%02X (64 KB)", bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_RAMDISK_PEZIK, bank, s_name_buf,
                0xFFFFFFFFu, 0x10000, 1, 1, 0);
        }
    }

    return count;
}

#endif /* MZARCH == 1500 */
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
