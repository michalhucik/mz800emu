/**
 * @file   mz800_ramlog.c
 * @brief  Záznam zápisů do paměti - cíle zápisu na MZ-800 (viz ramlog_hook.h).
 *
 * Cíle odpovídají makrům `memory_internal_write_*` v mz800_memory.c
 * a smyčce memory_load_block(); při změně mapování zápisu se musí upravit
 * současně.
 *
 * | CPU adresa | Cíl |
 * |------------|-----|
 * | 0000-0FFF, 1000-1FFF | RAM, s ROM_0000 / ROM_1000 zápis pod ROM (zahodí se) |
 * | 2000-7FFF, F000-FFFF bez ROM | RAM (s MemExt aktivní banka) |
 * | 8000-BFFF s VRAM v režimu 800 | plány grafické VRAM podle vramctrl_mz800_write_target() |
 * | C000-CFFF s CG-RAM (režim 700) | VRAM plán I, offset 0000-0FFF |
 * | D000-DFFF s VRAM 700 | VRAM plán I, offset 1000-1FFF |
 * | E000-E008 s ROM | port (není paměť) |
 * | E009-FFFF s ROM | zápis pod ROM |
 *
 * @par Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>

#include "debugger/trace/ramlog_hook.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"


uint32_t ramlog_platform_map_key(void)
{
    /* ramlog_platform_targets() čte g_memory.map (ROM, VRAM/CG-RAM, PROHIBITED)
     * a z DMD režim MZ-700 (CG-RAM C000, VRAM D000 místo 8000) a šířku 640
     * (VRAM i v A000-BFFF). Zápisové registry grafické VRAM ne, viz
     * ramlog_hook.h. */
    return (uint32_t) g_memory.map
           | ( (uint32_t) ( g_gdg.regDMD & ( REGISTER_DMD_FLAG_MZ700 | REGISTER_DMD_FLAG_SCRW640 ) ) << 8 );
}


bool ramlog_platform_ram_cell(const uint8_t *ptr, uint8_t *out_space, uint32_t *out_offset)
{
    /* Porovnání přes uintptr_t (ukazatele do různých polí, viz memext.c). */
    uintptr_t p = (uintptr_t) ptr, base = (uintptr_t) g_memory.RAM;
    if (p >= base && p - base < sizeof(g_memory.RAM)) {
        *out_space = RAMLOG_SPACE_RAM;
        *out_offset = (uint32_t) (p - base);
        return true;
    }
    if (MEMEXT_TEST_CONNECTED) {
        int32_t off = memext_get_ram_offset_from_pointer(ptr);
        if (off >= 0) {
            *out_space = RAMLOG_SPACE_MEMEXT;
            *out_offset = (uint32_t) off;
            return true;
        }
    }
    return false;
}


/** @brief Cíl RAM na CPU adrese podle memram_write (hlavní RAM nebo aktivní banka MemExt). */
static bool rl_ram(uint16_t addr, st_RAMLOG_TARGET *t)
{
    memset(t, 0, sizeof(*t));
    t->cell = g_memory.memram_write[addr >> 12] + (addr & 0x0FFFu);
    if (!ramlog_platform_ram_cell(t->cell, &t->space, &t->offset)) {
        /* Neznámý cíl (nemělo by nastat): zaznamenat jako RAM na CPU adrese. */
        t->space = RAMLOG_SPACE_RAM;
        t->offset = addr;
    }
    return true;
}


/** @brief Zápis pod ROM (zahodí se): prostor ROM, offset = CPU adresa. */
static void rl_rom(uint16_t addr, st_RAMLOG_TARGET *t)
{
    memset(t, 0, sizeof(*t));
    t->space = RAMLOG_SPACE_ROM;
    t->offset = addr;
}


/** @brief Buňka VRAM 700 / CG-RAM v plánu I (offset 0000-1FFF). */
static void rl_vram700(uint16_t addr, st_RAMLOG_TARGET *t)
{
    memset(t, 0, sizeof(*t));
    t->space = RAMLOG_SPACE_VRAM;
    t->offset = addr & 0x1FFFu;
    t->cell = &g_memory.VRAM[t->offset];
}


/** @brief Buňka plánu p grafické VRAM na fyzické adrese phy. */
static uint8_t *rl_plane_cell(unsigned p, unsigned phy)
{
    switch (p) {
        case 0: return &g_memory.VRAM[phy];
        case 1: return &g_memory.VRAM[MEMORY_SIZE_VRAM_BANK + phy];
        case 2: return &g_memory.EXVRAM[phy];
        default: return &g_memory.EXVRAM[MEMORY_SIZE_VRAM_BANK + phy];
    }
}


unsigned ramlog_platform_targets(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio)
{
    *mmio = false;
    switch (addr >> 12) {
        case 0x0:
            if (MEMORY_MZ800_MAP_TEST_ROM_0000) rl_rom(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0x1:
            if (MEMORY_MZ800_MAP_TEST_ROM_1000) rl_rom(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0x8: case 0x9: case 0xA: case 0xB: {
            bool vram = ((addr >> 12) < 0xA) ? MEMORY_MZ800_MAP_TEST_VRAM_8000 : MEMORY_MZ800_MAP_TEST_VRAM_A000;
            if (!vram) {
                rl_ram(addr, &out[0]);
                return 1;
            }
            unsigned phy = 0, we = 0;
            vramctrl_mz800_write_target(addr & 0x3FFFu, &phy, &we);
            unsigned n = 0;
            for (unsigned p = 0; p < 4; p++) {
                if (!(we & (1u << p))) continue;
                memset(&out[n], 0, sizeof(out[n]));
                out[n].space = RAMLOG_SPACE_VRAM;
                out[n].offset = p * MEMORY_SIZE_VRAM_BANK + phy;
                out[n].plane = (uint8_t) p;
                out[n].has_plane = true;
                out[n].cell = rl_plane_cell(p, phy);
                n++;
            }
            return n;
        }
        case 0xC:
            if (MEMORY_MZ800_MAP_TEST_CGRAM) rl_vram700(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0xD:
            if (MEMORY_MZ800_MAP_TEST_VRAM_D000) rl_vram700(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0xE:
            if (MEMORY_MZ800_MAP_TEST_ROM_E000) {
                if (addr <= 0xE008u) {
                    *mmio = true;
                    return 0;
                }
                rl_rom(addr, &out[0]);
            } else {
                rl_ram(addr, &out[0]);
            }
            return 1;
        case 0xF:
            if (MEMORY_MZ800_MAP_TEST_ROM_E000) rl_rom(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        default:
            rl_ram(addr, &out[0]);
            return 1;
    }
}


bool ramlog_platform_load_target(uint16_t addr, bool ramonly, st_RAMLOG_TARGET *out)
{
    if (ramonly) return rl_ram(addr, out);
    switch (addr >> 12) {
        case 0x0: if (MEMORY_MZ800_MAP_TEST_ROM_0000) return false; return rl_ram(addr, out);
        case 0x1: if (MEMORY_MZ800_MAP_TEST_ROM_1000) return false; return rl_ram(addr, out);
        case 0x8: case 0x9: case 0xA: case 0xB: {
            bool vram = ((addr >> 12) < 0xA) ? MEMORY_MZ800_MAP_TEST_VRAM_8000 : MEMORY_MZ800_MAP_TEST_VRAM_A000;
            if (!vram) return rl_ram(addr, out);
            /* Grafická VRAM: souhrn bez buňky (RAMLOG_FLAG_NO_MAP). */
            memset(out, 0, sizeof(*out));
            out->space = RAMLOG_SPACE_VRAM;
            out->offset = addr & 0x3FFFu;
            return true;
        }
        case 0xC: if (MEMORY_MZ800_MAP_TEST_CGRAM) { rl_vram700(addr, out); return true; } return rl_ram(addr, out);
        case 0xD: if (MEMORY_MZ800_MAP_TEST_VRAM_D000) { rl_vram700(addr, out); return true; } return rl_ram(addr, out);
        case 0xE: case 0xF: if (MEMORY_MZ800_MAP_TEST_ROM_E000) return false; return rl_ram(addr, out);
        default: return rl_ram(addr, out);
    }
}



unsigned ramlog_platform_query_cells(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio)
{
    unsigned page = addr >> 12;
    bool vram800 = (page >= 0x8 && page <= 0x9 && MEMORY_MZ800_MAP_TEST_VRAM_8000)
                   || (page >= 0xA && page <= 0xB && MEMORY_MZ800_MAP_TEST_VRAM_A000);
    if (!vram800) return ramlog_platform_targets(addr, out, mmio);
    *mmio = false;
    unsigned phy = 0, we = 0;
    vramctrl_mz800_write_target(addr & 0x3FFFu, &phy, &we);
    for (unsigned p = 0; p < 4; p++) {
        memset(&out[p], 0, sizeof(out[p]));
        out[p].space = RAMLOG_SPACE_VRAM;
        out[p].offset = p * MEMORY_SIZE_VRAM_BANK + phy;
        out[p].plane = (uint8_t) p;
        out[p].has_plane = true;
        out[p].cell = rl_plane_cell(p, phy);
    }
    return 4;
}

void ramlog_platform_space_sizes(uint32_t out[RAMLOG_SPACE_COUNT])
{
    memset(out, 0, sizeof(uint32_t) * RAMLOG_SPACE_COUNT);
    out[RAMLOG_SPACE_RAM] = MEMORY_SIZE_RAM;
    out[RAMLOG_SPACE_VRAM] = 4u * MEMORY_SIZE_VRAM_BANK;
    out[RAMLOG_SPACE_MEMEXT] = 0x80000u;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
