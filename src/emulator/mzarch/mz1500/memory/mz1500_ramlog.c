/**
 * @file   mz1500_ramlog.c
 * @brief  Záznam zápisů do paměti - cíle zápisu na MZ-1500 (viz ramlog_hook.h).
 *
 * Cíle odpovídají makrům `memory_internal_write_*` v mz1500_memory.c
 * a smyčce memory_load_block(); při změně mapování zápisu se musí upravit
 * současně.
 *
 * | CPU adresa | Cíl |
 * |------------|-----|
 * | 0000-0FFF | RAM, s ROM_0000 zápis pod ROM (zahodí se) |
 * | 1000-CFFF | RAM |
 * | D000-DFFF s ROM_UPPER | SPEC 0: VRAM 000-FFF, SPEC 1: CG-ROM (zahodí se), SPEC 2-4: PCG banka 1-3 |
 * | E000-EFFF s ROM_UPPER | SPEC 0: E000-E008 port, jinak zahodí se; SPEC 1: zahodí se; SPEC 2-4: PCG |
 * | F000-FFFF s ROM_UPPER | zápis pod ROM |
 * | D000-FFFF bez ROM_UPPER | RAM |
 *
 * PCG: zápis jde makrem MEMORY_PCG_WRITE_BYTE přes konec pole VRAM do
 * následujícího pole PCG (index VRAM = banka * 2000h + (addr & 3FFFh)),
 * offset v prostoru PCG je proto `banka * 2000h + (addr & 3FFFh) - 1000h`.
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
    /* ramlog_platform_targets() čte jen g_memory.map (ROM 0000, horní ROM,
     * SPEC_ID = VRAM / CG-ROM / PCG banka v D000-EFFF). */
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


/** @brief Zápis se zahodí (pod ROM / CG-ROM): prostor ROM, offset = CPU adresa. */
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


/** @brief Buňka PCG banky pcg_id (0-2) na CPU adrese D000-EFFF. */
static void rl_pcg(uint16_t addr, int pcg_id, st_RAMLOG_TARGET *t)
{
    memset(t, 0, sizeof(*t));
    t->space = RAMLOG_SPACE_PCG;
    t->offset = (uint32_t) pcg_id * MEMORY_SIZE_PCG_BANK + (addr & 0x3FFFu) - 0x1000u;
    t->cell = &g_memory.PCG[t->offset];
}


unsigned ramlog_platform_targets(uint16_t addr, st_RAMLOG_TARGET out[RAMLOG_TARGETS_MAX], bool *mmio)
{
    *mmio = false;
    unsigned page = addr >> 12;
    if (page == 0x0) {
        if (MEMORY_MZ1500_MAP_TEST_ROM_0000) rl_rom(addr, &out[0]);
        else rl_ram(addr, &out[0]);
        return 1;
    }
    if (page < 0xD || !MEMORY_MZ1500_MAP_TEST_ROM_UPPER) {
        rl_ram(addr, &out[0]);
        return 1;
    }
    if (page == 0xF) {
        rl_rom(addr, &out[0]);
        return 1;
    }
    int spec_id = MEMORY_MZ1500_SPEC_ID;
    if (spec_id == 0) {
        if (page == 0xD) {
            rl_vram(addr, &out[0]);
            return 1;
        }
        if (addr <= 0xE008u) {
            *mmio = true;
            return 0;
        }
        rl_rom(addr, &out[0]);
        return 1;
    }
    if (spec_id == 1) {
        rl_rom(addr, &out[0]);
        return 1;
    }
    rl_pcg(addr, spec_id - 2, &out[0]);
    return 1;
}


bool ramlog_platform_load_target(uint16_t addr, bool ramonly, st_RAMLOG_TARGET *out)
{
    if (ramonly) return rl_ram(addr, out);
    unsigned page = addr >> 12;
    if (page == 0x0 && MEMORY_MZ1500_MAP_TEST_ROM_0000) return false;
    if (page >= 0xD && MEMORY_MZ1500_MAP_TEST_ROM_UPPER) return false;
    return rl_ram(addr, out);
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
    out[RAMLOG_SPACE_PCG] = MEMORY_SIZE_PCG;
    out[RAMLOG_SPACE_MEMEXT] = 0x80000u;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
