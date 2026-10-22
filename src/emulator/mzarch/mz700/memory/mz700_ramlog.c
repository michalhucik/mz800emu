/**
 * @file   mz700_ramlog.c
 * @brief  Záznam zápisů do paměti - cíle zápisu na MZ-700 (viz ramlog_hook.h).
 *
 * Cíle odpovídají makrům `memory_internal_write_*` v mz700_memory.c
 * a smyčce memory_load_block(); při změně mapování zápisu se musí upravit
 * současně.
 *
 * | CPU adresa | Cíl |
 * |------------|-----|
 * | 0000-0FFF | RAM, s ROM_0000 zápis pod ROM (zahodí se) |
 * | 1000-CFFF | RAM |
 * | D000-DFFF s VRAM | VRAM, offset 000-FFF |
 * | E000-E008 s ROM | port (není paměť) |
 * | E009-FFFF s ROM | zápis pod ROM |
 * | D000-FFFF bez ROM | RAM |
 *
 * @par Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>

#include "debugger/trace/ramlog_hook.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"


uint32_t ramlog_platform_map_key(void)
{
    /* ramlog_platform_targets() čte jen g_memory.map (ROM 0000, ROM a VRAM E000/D000). */
    return g_memory.map;
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


/** @brief Buňka VRAM (offset 000-FFF). */
static void rl_vram(uint16_t addr, st_RAMLOG_TARGET *t)
{
    memset(t, 0, sizeof(*t));
    t->space = RAMLOG_SPACE_VRAM;
    t->offset = addr & 0x0FFFu;
    t->cell = &g_memory.VRAM[t->offset];
}


unsigned ramlog_platform_targets(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio)
{
    *mmio = false;
    switch (addr >> 12) {
        case 0x0:
            if (MEMORY_MZ700_MAP_TEST_ROM_0000) rl_rom(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0xD:
            if (MEMORY_MZ700_MAP_TEST_VRAM_D000) rl_vram(addr, &out[0]);
            else rl_ram(addr, &out[0]);
            return 1;
        case 0xE:
            if (MEMORY_MZ700_MAP_TEST_ROM_E000) {
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
            if (MEMORY_MZ700_MAP_TEST_ROM_E000) rl_rom(addr, &out[0]);
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
        case 0x0: if (MEMORY_MZ700_MAP_TEST_ROM_0000) return false; return rl_ram(addr, out);
        case 0xD: if (MEMORY_MZ700_MAP_TEST_VRAM_D000) { rl_vram(addr, out); return true; } return rl_ram(addr, out);
        case 0xE: case 0xF: if (MEMORY_MZ700_MAP_TEST_ROM_E000) return false; return rl_ram(addr, out);
        default: return rl_ram(addr, out);
    }
}



unsigned ramlog_platform_query_cells(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio)
{
    return ramlog_platform_targets(addr, out, mmio);
}

void ramlog_platform_space_sizes(uint32_t out[RAMLOG_SPACE_COUNT])
{
    memset(out, 0, sizeof(uint32_t) * RAMLOG_SPACE_COUNT);
    out[RAMLOG_SPACE_RAM] = MEMORY_SIZE_RAM;
    out[RAMLOG_SPACE_VRAM] = MEMORY_SIZE_VRAM;
    out[RAMLOG_SPACE_MEMEXT] = 0x80000u;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
