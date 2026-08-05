/*
 * mz800_regions.c - REGION_LIST enumerace pro MZ-800
 *
 * Per-arch implementace pro REGION_LIST API z dbgapi_regions.h. Naplňuje
 * pole st_REGION_DESC regiony specifickými pro MZ-800: monitor ROM, CG-ROM,
 * VRAM (4 plane), CG-RAM a VRAM 700 (jen v MZ-700 compat módu), Memext
 * (RAM + FLASH banky), Ramdisk STD/PEZIK, PROHIBITED shadow.
 *
 * Banking detection:
 *   - mapped_now flag se počítá proti aktuálnímu g_memory.map + GDG DMD.
 *   - Pro fyzické VRAM plane je mapped_now=1 pokud DMD = MZ-800 mód a VRAM
 *     je viditelný (flag VRAM v g_memory.map).
 *   - Pro Memext/Ramdisk banky je mapped_now vždy 0 v REGION sense (jednotlivé
 *     banky nejsou logicky mapované; Z80 vidí jeden aktuální bank přes
 *     memram_read/write pointery).
 *
 * Connection detection:
 *   - Memext: g_memext.connection == MEMEXT_CONNECTION_YES
 *   - Ramdisk STD: g_ramdisk.std.connected
 *   - Ramdisk PEZIK: g_ramdisk.pezik[i].connected
 *   - PROHIBITED shadow: MEMORY_MZ800_MAP_TEST_PROHIBITED
 *   - EXVRAM (plane III/IV): vždy connected=1 - emu nemá runtime detection
 *     pro exVRAM, pole g_memory.EXVRAM je vždy alokované [neověřeno: zda
 *     je v reálném HW vždy připojené, či existuje odpojený stav].
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

#if MZARCH == 800

#include <stdio.h>
#include <stdint.h>

#include "debugger/dbgapi_regions.h"
#include "debugger/dbgapi_regions_arch.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"


/* Pomocný buffer pro snprintf jmen (lokální, vlákno-bezpečné jen pro
 * jednoho vyzyvatele - REGION_LIST je serialised přes dbgapi mutex). */
static char s_name_buf[64];


int mz800_regions_collect(st_REGION_DESC *out, int max_count)
{
    int count = 0;

    /* ---- LOGICAL: vždy přítomný, mapuje na celých 64 KB Z80 prostoru. ---- */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_LOGICAL, 0, "Logical Z80",
        0x0000, 0x10000, 1, 1, 1);

    /* ---- RAM: plná 64 KB user DRAM bez ohledu na banking. ---- */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_RAM, 0, "User RAM (64 KB)",
        0x0000, MEMORY_SIZE_RAM, 1, 1, 1);

    /* ---- ROM_LOWER: monitor ROM 0x0000-0x0FFF (4 KB). ---- */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_ROM_LOWER, 0, "Monitor ROM lower (4 KB)",
        0x0000, ROM_SIZE_0000, 0, 1,
        MEMORY_MZ800_MAP_TEST_ROM_0000 ? 1 : 0);

    /* ---- ROM_UPPER: monitor ROM 0xE000-0xFFFF (8 KB). ---- */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_ROM_UPPER, 0, "Monitor ROM upper (8 KB)",
        0xE000, ROM_SIZE_E000, 0, 1,
        MEMORY_MZ800_MAP_TEST_ROM_E000 ? 1 : 0);

    /* ---- CGROM: 4 KB. Logical base 0x1000 (MZ-800 native, ROM_1000 flag). ---- */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_CGROM, 0, "CG-ROM (4 KB)",
        0x1000, ROM_SIZE_CGROM, 0, 1,
        MEMORY_MZ800_MAP_TEST_ROM_1000 ? 1 : 0);

    /* ---- MZ-700 compat: CGRAM_700, VRAM 700 char/attr. ---- *
     * V MZ-800 native modu jsou tyto regiony "skryté" za VRAM/banking;
     * v MZ-700 compat modu (DMD bit MZ700) jsou logicky přístupné.
     * Vždy enumerujeme - mapped_now reflektuje aktuální DMD. */
    int is_700_mode = (GDG_DMD_TEST_MODE700) ? 1 : 0;

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_CGRAM_700, 0, "CG-RAM (MZ-700 mode, 4 KB)",
        0xC000, 0x1000, 1, 1,
        (is_700_mode && MEMORY_MZ800_MAP_TEST_CGRAM) ? 1 : 0);

    /* VRAM 700: char + attr jsou fyzicky jedno pole g_memory.VRAM[0..0x0FFF],
     * ale logicky 1 KB znaková (D000-D3FF) + 1 KB attr (D800-DBFF).
     * Pro REGION_LIST exposujeme oba jako samostatné regiony s 1 KB každý.
     * [neověřeno přesné mirror chování ve VRAM ve 4 KB - rozbor V2 sekce
     * 1.1 řádek 43 zmiňuje "fyzicky jeden array s mirrors"]. */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_CHAR, 0, "VRAM 700 char (1 KB)",
        0xD000, 0x0400, 1, 1,
        (is_700_mode && MEMORY_MZ800_MAP_TEST_VRAM_D000) ? 1 : 0);

    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_700_ATTR, 0, "VRAM 700 attr (1 KB)",
        0xD800, 0x0400, 1, 1,
        (is_700_mode && MEMORY_MZ800_MAP_TEST_VRAM_D000) ? 1 : 0);

    /* ---- VRAM PHYS PLANE I-IV (MZ-800 native, 8 KB každý). ---- *
     * Plane I: g_memory.VRAM[0..0x1FFF]
     * Plane II: g_memory.VRAM[0x2000..0x3FFF]
     * Plane III: g_memory.EXVRAM[0..0x1FFF]
     * Plane IV: g_memory.EXVRAM[0x2000..0x3FFF]
     * Pointery: g_memoryVRAM_I/II/III/IV.
     *
     * Plane III/IV jsou "connected" jen pokud je připojené EXVRAM rozšíření.
     * Emu detection není (TODO viz framebuffer.c:274, vramctrl.c:70).
     * Pro V-1b enumerujeme jako connected=1 a noted v doc.
     *
     * mapped_now: 1 pokud DMD je v MZ-800 native modu a VRAM flag set. */
    int mz800_vram_mapped =
        (!is_700_mode && MEMORY_MZ800_MAP_TEST_VRAM_8000) ? 1 : 0;
    int mz800_vram_a000_mapped =
        (!is_700_mode && MEMORY_MZ800_MAP_TEST_VRAM_A000) ? 1 : 0;

    /* Plane I (logical base 0x8000 v 4-plane SCRW320 modu). */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_PHYS_PLANE, 0, "VRAM plane I (8 KB)",
        0x8000, MEMORY_SIZE_VRAM_BANK, 1, 1, mz800_vram_mapped);

    /* Plane II (logical base 0xA000 v SCRW640 modu - jinak nemapovaný). */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_PHYS_PLANE, 1, "VRAM plane II (8 KB)",
        0xA000, MEMORY_SIZE_VRAM_BANK, 1, 1, mz800_vram_a000_mapped);

    /* Plane III (EXVRAM první půlka, 8 KB). Nemapovaný do CPU prostoru
     * přímo - je viditelný jen pro GDG. Logical_base = 0xFFFFFFFF. */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_PHYS_PLANE, 2, "VRAM plane III (8 KB, exVRAM)",
        0xFFFFFFFFu, MEMORY_SIZE_VRAM_BANK, 1, 1, 0);

    /* Plane IV (EXVRAM druhá půlka, 8 KB). Dtto. */
    dbgapi_regions_add(out, &count, max_count,
        REGION_KIND_VRAM_PHYS_PLANE, 3, "VRAM plane IV (8 KB, exVRAM)",
        0xFFFFFFFFu, MEMORY_SIZE_VRAM_BANK, 1, 1, 0);

    /* ---- Memext: RAM + FLASH banky. ---- *
     * Luftner: 128 RAM banks (4 KB) + 128 FLASH banks (rawbank 0x80..0xFF).
     * PEHU: 64 RAM banks (MEMEXT_PEHU_BANKS), žádný FLASH.
     *
     * Bank size = MEMEXT_RAW_BANK_SIZE = 0x1000 = 4 KB (per memext.h:45).
     *
     * Enumerujeme jen pokud Memext připojen - jinak by uživatel viděl
     * 256 disconnected entries. */
    if (MEMEXT_TEST_CONNECTED) {
        int is_luftner = MEMEXT_TEST_TYPE_LUFTNER ? 1 : 0;
        const char *type_label = is_luftner ? "Luftner" : "PEHU";

        /* Počet RAM banek: 128 u Luftneru, 64 u PEHU.
         * PEHU má interně 8 KB banky (MEMEXT_PEHU_BANK_SIZE), ale debug API
         * iteruje přes raw 4 KB banky (= memext_get_ram_read_pointer_by_rawbank
         * iteruje přes g_memext.RAM po 4 KB krocích bez ohledu na typ). */
        int ram_bank_count = is_luftner ? MEMEXT_LUFTNER_BANKS : MEMEXT_PEHU_BANKS;

        for (int bank = 0; bank < ram_bank_count; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Memext %s RAM bank 0x%02X (4 KB)", type_label, bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_MEMEXT_RAM, bank, s_name_buf,
                0xFFFFFFFFu, MEMEXT_RAW_BANK_SIZE, 1, 1, 0);
        }

        /* FLASH banks: 0x80..0xFF jen pro Luftner. */
        if (is_luftner) {
            for (int bank = MEMEXT_LUFTNER_BANKS;
                 bank < 2 * MEMEXT_LUFTNER_BANKS; bank++)
            {
                snprintf(s_name_buf, sizeof(s_name_buf),
                    "Memext Luftner FLASH bank 0x%02X (4 KB)", bank);
                /* FLASH je R-only z pohledu Memory Browseru - writable=0. */
                dbgapi_regions_add(out, &count, max_count,
                    REGION_KIND_MEMEXT_FLASH, bank, s_name_buf,
                    0xFFFFFFFFu, MEMEXT_RAW_BANK_SIZE, 0, 1, 0);
            }
        }
    }

    /* ---- Ramdisk STD: enumerace per bank (každá 64 KB). ---- *
     * Počet bank = (size_mask + 1) - viz ramdisk.h:62-68. R/O dle type. */
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

    /* ---- Ramdisk PEZIK: 2 instance × 8 banků po 64 KB = 512 KB každá. ---- *
     * Celková velikost = 8 * 64 KB = 512 KB (ramdisk.c:257 - 8 * DEF_BANK_SIZE).
     * Latch addressing 19 bitů (bank top 3 bity z addr, 16-bit latch).
     *
     * Per-bank enumerace: každá instance emit 8 regionů po 64 KB.
     * sub_id encoding: pezik_instance * 8 + bank_idx
     *   - 0..7  = pezik index 0 (port 0x68), banky 0..7
     *   - 8..15 = pezik index 1 (port 0xE8), banky 0..7
     * Read/write dekóduje sub_id zpět na (instance, bank_offset). */
    for (int pi = 0; pi < 2; pi++) {
        if (!g_ramdisk.pezik[pi].connected) continue;
        const char *port_label = (pi == RAMDISK_PEZIK_E8) ? "E8" : "68";
        for (int bank = 0; bank < 8; bank++) {
            snprintf(s_name_buf, sizeof(s_name_buf),
                "Ramdisk PEZIK %s bank 0x%02X (64 KB)", port_label, bank);
            dbgapi_regions_add(out, &count, max_count,
                REGION_KIND_RAMDISK_PEZIK, pi * 8 + bank, s_name_buf,
                0xFFFFFFFFu, 0x10000, 1, 1, 0);
        }
    }

    /* ---- PROHIBITED shadow: virtuální region $E009-$FFFF vracející 0x1A. ---- *
     * Enumerujeme jen pokud aktuálně aktivní (OUT 0xE5). Velikost = 0x10000
     * - 0xE009 = 0x1FF7 bajtů (= 8183). Logical base 0xE009. */
    if (MEMORY_MZ800_MAP_TEST_PROHIBITED) {
        dbgapi_regions_add(out, &count, max_count,
            REGION_KIND_PROHIBITED_SHADOW, 0, "Prohibited shadow (0x1A)",
            0xE009, 0x10000 - 0xE009, 0, 1, 1);
    }

    return count;
}

#endif /* MZARCH == 800 */
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
