/*
 * File:   mhmap.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 29. dubna 2026
 *
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

/**
 * @file mhmap.c
 * @brief Implementace Memory Heatmap - viz mh.h pro popis modulu.
 */

#include "mzarch/mzarch_config.h"

#if defined(MZ800EMU_CFG_DEBUGGER_ENABLED) && ( ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 ) )

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "mhmap.h"
#include "debugger.h"

#include "memory/memory.h"
#include "mzarch/mzarch_platform_functions.h"

#if MZARCH == 800
#include "mzarch/mz800/memory/mz800_memory.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/memory/mz1500_memory.h"
#elif MZARCH == 700
#include "mzarch/mz700/memory/mz700_memory.h"
#endif


/**
 * @brief Globální stav Memory Heatmap - statická alokace v BSS.
 *
 * Velikost ~2.7 MB pro MZ-800 po V5 (cell má 16 B = 4× uint32 R/W/X/S).
 * Před V5 mělo 12 B per cell (~2 MB). Žádný malloc, žádné lazy alloc.
 */
st_MHMAP g_mhmap;


/**
 * @brief Pending VRAM read access kind. Default R.
 */
en_MHMAP_ACCESS g_mhmap_pending_read_kind = MHMAP_ACCESS_R;


/**
 * @brief Příznak "aktuální access prochází CPU debug cestou". Default 0.
 * Viz mhmap.h pro popis (sdíleno mezi MHMAP, iorqlog a dalšími debug
 * subsystémy).
 */
unsigned g_dbg_in_cpu_path = 0;


void mhmap_init ( void )
{
    mhmap_reset ( );
}


void mhmap_reset ( void )
{
    memset ( &g_mhmap, 0, sizeof ( g_mhmap ) );
}


void mhmap_set_mode ( en_DEBUGGER_MHMAP_MODE mode )
{
    g_debugger.mhmap_mode = mode;
    /*
     * Triggernout aktualizaci CPU callbacků. Funkce sama uvnitř spočítá
     * (CPUHIST_ACTIVE || MHMAP_ACTIVE) a podle toho swapuje mezi
     * rychlou a pomalou cestou.
     */
    mzarch_platform_fn_debugger_state_changed ( TEST_DEBUGGER_ACTIVE );
}


void mhmap_inc ( en_MHMAP_REGION region, unsigned offset, en_MHMAP_ACCESS access )
{
    st_MHMAP_CELL *cell = NULL;

    switch ( region )
    {
        case MHMAP_REGION_BUS:           cell = &g_mhmap.bus           [ offset ]; break;
        case MHMAP_REGION_RAM:           cell = &g_mhmap.ram           [ offset ]; break;
#if MZARCH == 800
        case MHMAP_REGION_ROM_LOWER:     cell = &g_mhmap.rom_lower     [ offset ]; break;
        case MHMAP_REGION_ROM_CG:        cell = &g_mhmap.rom_cg        [ offset ]; break;
        case MHMAP_REGION_ROM_UPPER:     cell = &g_mhmap.rom_upper     [ offset ]; break;

        case MHMAP_REGION_VRAM:                cell = &g_mhmap.vram                [ offset ]; break;
        case MHMAP_REGION_VRAM700_CG:          cell = &g_mhmap.vram700_cg          [ offset ]; break;
        case MHMAP_REGION_VRAM700:             cell = &g_mhmap.vram700             [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_HC_I:    cell = &g_mhmap.vram800_320_hc_i    [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_HC_II:   cell = &g_mhmap.vram800_320_hc_ii   [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_HC_III:  cell = &g_mhmap.vram800_320_hc_iii  [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_HC_IV:   cell = &g_mhmap.vram800_320_hc_iv   [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_4A_I:    cell = &g_mhmap.vram800_320_4a_i    [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_4A_II:   cell = &g_mhmap.vram800_320_4a_ii   [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_4B_I:    cell = &g_mhmap.vram800_320_4b_i    [ offset ]; break;
        case MHMAP_REGION_VRAM800_320_4B_II:   cell = &g_mhmap.vram800_320_4b_ii   [ offset ]; break;
        case MHMAP_REGION_VRAM800_640_4_I:     cell = &g_mhmap.vram800_640_4_i     [ offset ]; break;
        case MHMAP_REGION_VRAM800_640_4_II:    cell = &g_mhmap.vram800_640_4_ii    [ offset ]; break;
        case MHMAP_REGION_VRAM800_640_2A:      cell = &g_mhmap.vram800_640_2a      [ offset ]; break;
        case MHMAP_REGION_VRAM800_640_2B:      cell = &g_mhmap.vram800_640_2b      [ offset ]; break;

        case MHMAP_REGION_IORQ_GDG:      cell = &g_mhmap.iorq_gdg      [ offset ]; break;
#elif MZARCH == 1500
        case MHMAP_REGION_ROM:           cell = &g_mhmap.rom           [ offset ]; break;
        case MHMAP_REGION_CGROM:         cell = &g_mhmap.cgrom         [ offset ]; break;
        case MHMAP_REGION_VRAM:          cell = &g_mhmap.vram          [ offset ]; break;
        case MHMAP_REGION_PCG_1:         cell = &g_mhmap.pcg_1         [ offset ]; break;
        case MHMAP_REGION_PCG_2:         cell = &g_mhmap.pcg_2         [ offset ]; break;
        case MHMAP_REGION_PCG_3:         cell = &g_mhmap.pcg_3         [ offset ]; break;
#elif MZARCH == 700
        case MHMAP_REGION_ROM:           cell = &g_mhmap.rom           [ offset ]; break;
        case MHMAP_REGION_CGROM:         cell = &g_mhmap.cgrom         [ offset ]; break;
        case MHMAP_REGION_VRAM:          cell = &g_mhmap.vram          [ offset ]; break;
#endif
        case MHMAP_REGION_IORQ_8BIT:     cell = &g_mhmap.iorq_8bit     [ offset ]; break;
        case MHMAP_REGION_MEMEXT:        cell = &g_mhmap.memext        [ offset ]; break;
        default:                      return;
    }

    switch ( access )
    {
        case MHMAP_ACCESS_R: cell->r++; break;
        case MHMAP_ACCESS_W: cell->w++; break;
        case MHMAP_ACCESS_X: cell->x++; break;
        case MHMAP_ACCESS_S: cell->s++; break;
        default: break;
    }
}


#if MZARCH == 800
void mhmap_inc_vram_banks ( uint8_t bank_mask, unsigned phy_offset, en_MHMAP_ACCESS access )
{
    /*
     * vram region (32 KB) je linear stack 4 fyzických bank po 8 KB:
     *   VRAM1: 0x0000-0x1FFF
     *   VRAM2: 0x2000-0x3FFF
     *   VRAM3: 0x4000-0x5FFF
     *   VRAM4: 0x6000-0x7FFF
     * Bank mask konvence shoduje s DEF_PLANE1..4 / WE bitmask ve vramctrl.
     */
    if ( bank_mask & 0x01 ) mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + phy_offset, access );
    if ( bank_mask & 0x02 ) mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK2_OFFSET + phy_offset, access );
    if ( bank_mask & 0x04 ) mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK3_OFFSET + phy_offset, access );
    if ( bank_mask & 0x08 ) mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK4_OFFSET + phy_offset, access );
}


/**
 * @brief Recording do mode-specific VRAM regionů dle aktuálního DMD modu.
 *
 * Pravidla mapování plane bitmask → mode-specific regiony:
 *  - 320x200@16 (HICOLOR=1, SCRW640=0): PLANE1..4 → I, II, III, IV
 *  - 320x200@4A (HICOLOR=0, SCRW640=0, VBANK=0): PLANE1→I, PLANE2→II
 *  - 320x200@4B (HICOLOR=0, SCRW640=0, VBANK=1): PLANE3→I, PLANE4→II
 *  - 640x200@4 (HICOLOR=1, SCRW640=1): PLANE1→I, PLANE3→II
 *  - 640x200@2A (HICOLOR=0, SCRW640=1, VBANK=0): PLANE1→ jediná
 *  - 640x200@2B (HICOLOR=0, SCRW640=1, VBANK=1): PLANE3→ jediná
 *
 * @param plane_mask Bitmaska plane (DEF_PLANE1..4 konvence) - SW intent
 *                   z WF_PLANE pro write nebo RF_PLANE pro read.
 * @param bus_offset Index do mode-specific souboru (8 KB pro 320 mode,
 *                   16 KB pro 640 mode).
 * @param access     R / W / X
 */
static void mhmap_record_mz800_vram_mode_specific ( uint8_t plane_mask,
                                                    unsigned bus_offset,
                                                    en_MHMAP_ACCESS access )
{
    int hicolor = GDG_MZ800_DMD_TEST_HICOLOR;
    int scrw640 = GDG_MZ800_DMD_TEST_SCRW640;
    int vbank = g_vramctrl.regWFRF_VBANK;

    if ( !scrw640 )
    {
        if ( hicolor )
        {
            /* 320x200@16 - všechny 4 plane mapované 1:1 */
            if ( plane_mask & 0x01 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_HC_I,   bus_offset, access );
            if ( plane_mask & 0x02 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_HC_II,  bus_offset, access );
            if ( plane_mask & 0x04 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_HC_III, bus_offset, access );
            if ( plane_mask & 0x08 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_HC_IV,  bus_offset, access );
        }
        else
        {
            /* 320x200@4 - 2 plane podle banky */
            if ( !vbank )
            {
                if ( plane_mask & 0x01 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_4A_I,  bus_offset, access );
                if ( plane_mask & 0x02 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_4A_II, bus_offset, access );
            }
            else
            {
                if ( plane_mask & 0x04 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_4B_I,  bus_offset, access );
                if ( plane_mask & 0x08 ) mhmap_inc ( MHMAP_REGION_VRAM800_320_4B_II, bus_offset, access );
            };
        };
    }
    else
    {
        if ( hicolor )
        {
            /* 640x200@4 - 2 logické plane */
            if ( plane_mask & 0x01 ) mhmap_inc ( MHMAP_REGION_VRAM800_640_4_I,  bus_offset, access );
            if ( plane_mask & 0x04 ) mhmap_inc ( MHMAP_REGION_VRAM800_640_4_II, bus_offset, access );
        }
        else
        {
            /* 640x200@2 - 1 plane podle banky */
            if ( !vbank )
            {
                if ( plane_mask & 0x01 ) mhmap_inc ( MHMAP_REGION_VRAM800_640_2A, bus_offset, access );
            }
            else
            {
                if ( plane_mask & 0x04 ) mhmap_inc ( MHMAP_REGION_VRAM800_640_2B, bus_offset, access );
            };
        };
    };
}


void mhmap_record_mz800_vram_write ( uint8_t we_mask, uint8_t wf_plane,
                                     unsigned bus_offset, unsigned phy_vram_addr )
{
    /* Fyzická vram.cdl - dle WE bitmask (= co se reálně do fyzických VRAM zapsalo). */
    mhmap_inc_vram_banks ( we_mask, phy_vram_addr, MHMAP_ACCESS_W );
    /* Mode-specific - dle WF_PLANE (= SW intent, "do které logické plane chtěl program psát"). */
    mhmap_record_mz800_vram_mode_specific ( wf_plane, bus_offset, MHMAP_ACCESS_W );
}


void mhmap_record_mz800_vram_read ( uint8_t rf_plane,
                                    unsigned bus_offset, unsigned phy_vram_addr,
                                    en_MHMAP_ACCESS access )
{
    /* Pro read jsou fyzické banky a SW intent identické (= regRF_PLANE). */
    mhmap_inc_vram_banks ( rf_plane, phy_vram_addr, access );
    mhmap_record_mz800_vram_mode_specific ( rf_plane, bus_offset, access );
}
#endif


/* ============================================================================
 *
 *  Resolver - per-arch implementace
 *
 * ============================================================================
 */

#define MH_RESOLVE_RETURN_RAM( addr )       \
    do                                       \
    {                                        \
        *out_region = MHMAP_REGION_RAM;         \
        *out_offset = (unsigned)( addr );    \
        return MHMAP_RESOLVE_DIRECT;            \
    } while ( 0 )


#if MZARCH == 800

en_MHMAP_RESOLVE_RESULT mhmap_resolve_mem ( uint16_t bus_addr, en_MHMAP_REGION *out_region, unsigned *out_offset )
{
    /* Horní 4 bity adresy = index stránky 0x0..0xF (každá stránka 4 KB). */
    unsigned page = bus_addr >> 12;

    switch ( page )
    {
        case 0x0: /* 0000-0FFF: ROM monitor lower nebo RAM */
            if ( MEMORY_MZ800_MAP_TEST_ROM_0000 )
            {
                *out_region = MHMAP_REGION_ROM_LOWER;
                *out_offset = bus_addr;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0x1: /* 1000-1FFF: CG-ROM nebo RAM */
            if ( MEMORY_MZ800_MAP_TEST_ROM_1000 )
            {
                *out_region = MHMAP_REGION_ROM_CG;
                *out_offset = bus_addr - 0x1000;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0x8: /* 8000-8FFF: VRAM v MZ-800 mode (plane 1+2) - klasifikuje vramctrl_ */
        case 0x9:
            if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 )
            {
                return MHMAP_RESOLVE_SKIP;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xA: /* A000-AFFF: exVRAM v MZ-800 + 640 mode (plane 3+4) - klasifikuje vramctrl_ */
        case 0xB:
            if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 )
            {
                return MHMAP_RESOLVE_SKIP;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xC: /* C000-CFFF: CG-RAM v MZ-700 mode - vrací VRAM700_CG.
                   * Caller (memory_*_with_logging_cb) musí navíc zaznamenat
                   * fyzický access do vram regionu (VRAM1 bank, offset = bus_addr & 0xFFF). */
            if ( MEMORY_MZ800_MAP_TEST_CGRAM )
            {
                *out_region = MHMAP_REGION_VRAM700_CG;
                *out_offset = bus_addr & 0x0FFF;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xD: /* D000-DFFF: textová+atributová VRAM v MZ-700 mode - vrací VRAM700.
                   * Caller musí navíc zaznamenat do vram regionu
                   * (VRAM1 bank, offset = (bus_addr & 0x1FFF), tj. 0x1000-0x1FFF). */
            if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 )
            {
                *out_region = MHMAP_REGION_VRAM700;
                *out_offset = bus_addr & 0x0FFF;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xE: /* E000-EFFF: ROM monitor upper, RAM, mapped ports, nebo "off" */
        case 0xF:
            if ( MEMORY_MZ800_MAP_TEST_ROM_E000 )
            {
                /* V Prohibited stavu CPU cte 0x1A shadow byte (NE z ROM),
                 * fyzicky cil pak neni ROM - skip resolveru.
                 * Empiricky overeno banking-e800 v0.4 + v0.5. */
                if ( MEMORY_MZ800_MAP_TEST_PROHIBITED )
                {
                    return MHMAP_RESOLVE_SKIP;
                }
                /* V 800 native (DMD bit 3 = 0) + K3 je $E000-$E00F = 0xFF
                 * (= mapped ports area "off"). Cil neni ROM - skip resolveru.
                 * Empiricky overeno banking-e800 v0.5 T3. */
                if ( ! GDG_DMD_TEST_MODE700 && page == 0xE
                     && ( bus_addr & 0x0FFF ) <= 0x000F )
                {
                    return MHMAP_RESOLVE_SKIP;
                }
                /* V 700 mode K3 je $E000-$E008 mapped ports (PIO/CTC/GDG),
                 * fyzicky cil neni ROM - skip resolveru. */
                if ( GDG_DMD_TEST_MODE700 && page == 0xE
                     && ( bus_addr & 0x0FFF ) <= 0x0008 )
                {
                    return MHMAP_RESOLVE_SKIP;
                }
                *out_region = MHMAP_REGION_ROM_UPPER;
                *out_offset = bus_addr - 0xE000;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        default: /* 0x2-0x7: hlavní RAM, žádný banking */
            MH_RESOLVE_RETURN_RAM ( bus_addr );
    }
}

#elif MZARCH == 1500

en_MHMAP_RESOLVE_RESULT mhmap_resolve_mem ( uint16_t bus_addr, en_MHMAP_REGION *out_region, unsigned *out_offset )
{
    unsigned page = bus_addr >> 12;

    switch ( page )
    {
        case 0x0: /* 0000-0FFF: ROM (lower) nebo RAM */
            if ( MEMORY_MZ1500_MAP_TEST_ROM_0000 )
            {
                *out_region = MHMAP_REGION_ROM;
                *out_offset = bus_addr;       /* ROM[0x0000-0x0FFF] = monitor lower */
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xD: /* D000-DFFF: VRAM / CGROM / PCG_x / RAM (podle ROM_UPPER + SPEC) */
        case 0xE: /* E000-EFFF: ROM (upper) / CGROM / PCG_x / RAM (podle ROM_UPPER + SPEC) */
        {
            if ( !MEMORY_MZ1500_MAP_TEST_ROM_UPPER )
            {
                /* Banking RAM - běžná hlavní paměť */
                MH_RESOLVE_RETURN_RAM ( bus_addr );
            }

            unsigned spec_id = MEMORY_MZ1500_SPEC_ID;

            if ( spec_id == 0 )
            {
                /* spec=0: D000 = VRAM, E000 = ROM_UPPER + memory-mapped porty */
                if ( page == 0xD )
                {
                    *out_region = MHMAP_REGION_VRAM;
                    *out_offset = bus_addr & 0x0FFF;
                    return MHMAP_RESOLVE_DIRECT;
                }
                /* page == 0xE: porty E000-E00F (PIO/CTC/GDG status) nemají
                 * fyzický cíl v ROM. ROM upper je v E010-EFFF (offset 0x2010-0x2FFF
                 * v ROM blob) a F000-FFFF (offset 0x3000-0x3FFF). */
                if ( ( bus_addr & 0x0FFF ) <= 0x000F )
                {
                    /* Memory-mapped porty - fyzický cíl není ROM, jen bus zápis. */
                    return MHMAP_RESOLVE_SKIP;
                }
                *out_region = MHMAP_REGION_ROM;
                *out_offset = bus_addr & 0x3FFF;
                return MHMAP_RESOLVE_DIRECT;
            }

            if ( spec_id == 1 )
            {
                /* spec=1: CGROM (4 KB), mapuje se identicky na D000 i E000 */
                *out_region = MHMAP_REGION_CGROM;
                *out_offset = bus_addr & 0x0FFF;
                return MHMAP_RESOLVE_DIRECT;
            }

            /* spec=2,3,4: PCG bank 1, 2, 3 (8 KB). Z bus se vidí 4 KB okno
             * na D000 (= PCG offset 0x0000-0x0FFF) a 4 KB okno na E000 (= offset
             * 0x1000-0x1FFF). MH offset = (bus_addr & 0x3FFF) - 0x1000. */
            unsigned pcg_id = spec_id - 2;
            *out_region = (en_MHMAP_REGION)( MHMAP_REGION_PCG_1 + pcg_id );
            *out_offset = ( bus_addr & 0x3FFF ) - 0x1000;
            return MHMAP_RESOLVE_DIRECT;
        }

        case 0xF: /* F000-FFFF: ROM (upper) / 0xFF (spec) / RAM */
            if ( !MEMORY_MZ1500_MAP_TEST_ROM_UPPER )
            {
                MH_RESOLVE_RETURN_RAM ( bus_addr );
            }
            if ( MEMORY_MZ1500_MAP_TEST_D000_SPEC )
            {
                /* SPEC mode v D000 → F000-FFFF vrací 0xFF, žádný fyzický cíl */
                return MHMAP_RESOLVE_SKIP;
            }
            *out_region = MHMAP_REGION_ROM;
            *out_offset = bus_addr & 0x3FFF;     /* ROM[0x3000-0x3FFF] */
            return MHMAP_RESOLVE_DIRECT;

        default: /* 0x1-0xC: vždy RAM (žádný banking) */
            MH_RESOLVE_RETURN_RAM ( bus_addr );
    }
}

#elif MZARCH == 700

en_MHMAP_RESOLVE_RESULT mhmap_resolve_mem ( uint16_t bus_addr, en_MHMAP_REGION *out_region, unsigned *out_offset )
{
    unsigned page = bus_addr >> 12;

    switch ( page )
    {
        case 0x0: /* 0000-0FFF: ROM monitor lower nebo RAM */
            if ( MEMORY_MZ700_MAP_TEST_ROM_0000 )
            {
                *out_region = MHMAP_REGION_ROM;
                *out_offset = bus_addr;       /* ROM[0x0000-0x0FFF] = monitor lower */
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xD: /* D000-DFFF: VRAM (CHAR+ATTR, 4 KB) nebo RAM */
            if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 )
            {
                *out_region = MHMAP_REGION_VRAM;
                *out_offset = bus_addr & 0x0FFF;
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xE: /* E000-EFFF: I/O porty E000-E007, ROM E800 nebo RAM */
            if ( MEMORY_MZ700_MAP_TEST_ROM_E000 )
            {
                /* E000-E008: memory-mapped porty (8255 PPI, 8253 PIT, GDG) -
                 * fyzický cíl není ROM, jen bus zápis (skip resolveru). */
                if ( ( bus_addr & 0x0FFF ) <= 0x0008 )
                {
                    return MHMAP_RESOLVE_SKIP;
                }
                /* E800-EFFF: ROM E800 (code extension) - jen pokud NENI Prohibited
                 * (vnejsi MAP_TEST_ROM_E000 uz overen). V Prohibited stavu CPU
                 * cte 0xFF, ale fyzicky cil pak neni ROM - skip resolveru. */
                if ( ! MEMORY_MZ700_MAP_TEST_PROHIBITED && ( bus_addr & 0x0FFF ) >= 0x0800 )
                {
                    *out_region = MHMAP_REGION_ROM;
                    *out_offset = bus_addr & 0x3FFF;
                    return MHMAP_RESOLVE_DIRECT;
                }
                /* E009-E7FF: bez definovaného fyzického cíle. */
                return MHMAP_RESOLVE_SKIP;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        case 0xF: /* F000-FFFF: ROM monitor upper, RAM, nebo "off" (Prohibited) */
            if ( MEMORY_MZ700_MAP_TEST_ROM_E000 )
            {
                /* V Prohibited stavu CPU cte 0xFF (ROM odpojeno), fyzicky
                 * cil pak neni ROM - skip resolveru. */
                if ( MEMORY_MZ700_MAP_TEST_PROHIBITED )
                {
                    return MHMAP_RESOLVE_SKIP;
                }
                *out_region = MHMAP_REGION_ROM;
                *out_offset = bus_addr & 0x3FFF;     /* ROM[0x3000-0x3FFF] */
                return MHMAP_RESOLVE_DIRECT;
            }
            MH_RESOLVE_RETURN_RAM ( bus_addr );

        default: /* 0x1-0xC: vždy RAM */
            MH_RESOLVE_RETURN_RAM ( bus_addr );
    }
}

#endif /* MZARCH == ... */


/* ============================================================================
 *
 *  Export MH dat do adresářové struktury
 *
 * ============================================================================
 */

/* Veřejná deklarace st_MHMAP_EXPORT_REGION + mhmap_get_export_regions je
 * v mhmap.h. Implementace tabulky je tady (statická, vyplněná za běhu). */


const st_MHMAP_EXPORT_REGION *mhmap_get_export_regions ( size_t *out_count )
{
#if MZARCH == 800
    static st_MHMAP_EXPORT_REGION regions[ ] = {
        { "bus",                   "bus",          "bus.cdl",                   NULL, 0, MHMAP_SIZE_BUS               },
        { "ram",                   "ram",          "ram.cdl",                   NULL, 0, MHMAP_SIZE_RAM               },
        { "rom-lower",             "rom-lower",    "rom-lower.cdl",             NULL, 0, MHMAP_SIZE_ROM_LOWER         },
        { "rom-cg",                "rom-cg",       "rom-cg.cdl",                NULL, 0, MHMAP_SIZE_ROM_CG            },
        { "rom-upper",             "rom-upper",    "rom-upper.cdl",             NULL, 0, MHMAP_SIZE_ROM_UPPER         },
        { "vram",                  "vram",         "vram.cdl",                  NULL, 0, MHMAP_SIZE_VRAM              },
        { "vram700-cg",            "vram700-cg",   "vram700-cg.cdl",            NULL, 0, MHMAP_SIZE_VRAM700_CG        },
        { "vram700",               "vram700",      "vram700.cdl",               NULL, 0, MHMAP_SIZE_VRAM700           },
        { "vram800-320x200_16-I",  "vram320@16-I", "vram800-320x200_16-I.cdl",  NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_16-II", "vram320@16-II","vram800-320x200_16-II.cdl", NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_16-III","vram320@16-III","vram800-320x200_16-III.cdl",NULL,0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_16-IV", "vram320@16-IV","vram800-320x200_16-IV.cdl", NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_4A-I",  "vram320@4A-I", "vram800-320x200_4A-I.cdl",  NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_4A-II", "vram320@4A-II","vram800-320x200_4A-II.cdl", NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_4B-I",  "vram320@4B-I", "vram800-320x200_4B-I.cdl",  NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-320x200_4B-II", "vram320@4B-II","vram800-320x200_4B-II.cdl", NULL, 0, MHMAP_SIZE_VRAM800_320_PLANE },
        { "vram800-640x200_4-I",   "vram640@4-I",  "vram800-640x200_4-I.cdl",   NULL, 0, MHMAP_SIZE_VRAM800_640_PLANE },
        { "vram800-640x200_4-II",  "vram640@4-II", "vram800-640x200_4-II.cdl",  NULL, 0, MHMAP_SIZE_VRAM800_640_PLANE },
        { "vram800-640x200_2A",    "vram640@2A",   "vram800-640x200_2A.cdl",    NULL, 0, MHMAP_SIZE_VRAM800_640_PLANE },
        { "vram800-640x200_2B",    "vram640@2B",   "vram800-640x200_2B.cdl",    NULL, 0, MHMAP_SIZE_VRAM800_640_PLANE },
        { "iorq-8bit",             "iorq-8bit",    "iorq-8bit.cdl",             NULL, 0, MHMAP_SIZE_IORQ_8BIT         },
        { "iorq-gdg",              "iorq-gdg",     "iorq-gdg.cdl",              NULL, 0, MHMAP_SIZE_IORQ_GDG          },
        { "memext",                "memext",       "memext.cdl",                NULL, 0, MHMAP_SIZE_MEMEXT            },
    };
    /* Doplnit pointery a velikosti za běhu (nelze v static inicializátoru). */
    regions[  0 ].buffer = g_mhmap.bus;                 regions[  0 ].size_bytes = sizeof ( g_mhmap.bus );
    regions[  1 ].buffer = g_mhmap.ram;                 regions[  1 ].size_bytes = sizeof ( g_mhmap.ram );
    regions[  2 ].buffer = g_mhmap.rom_lower;           regions[  2 ].size_bytes = sizeof ( g_mhmap.rom_lower );
    regions[  3 ].buffer = g_mhmap.rom_cg;              regions[  3 ].size_bytes = sizeof ( g_mhmap.rom_cg );
    regions[  4 ].buffer = g_mhmap.rom_upper;           regions[  4 ].size_bytes = sizeof ( g_mhmap.rom_upper );
    regions[  5 ].buffer = g_mhmap.vram;                regions[  5 ].size_bytes = sizeof ( g_mhmap.vram );
    regions[  6 ].buffer = g_mhmap.vram700_cg;          regions[  6 ].size_bytes = sizeof ( g_mhmap.vram700_cg );
    regions[  7 ].buffer = g_mhmap.vram700;             regions[  7 ].size_bytes = sizeof ( g_mhmap.vram700 );
    regions[  8 ].buffer = g_mhmap.vram800_320_hc_i;    regions[  8 ].size_bytes = sizeof ( g_mhmap.vram800_320_hc_i );
    regions[  9 ].buffer = g_mhmap.vram800_320_hc_ii;   regions[  9 ].size_bytes = sizeof ( g_mhmap.vram800_320_hc_ii );
    regions[ 10 ].buffer = g_mhmap.vram800_320_hc_iii;  regions[ 10 ].size_bytes = sizeof ( g_mhmap.vram800_320_hc_iii );
    regions[ 11 ].buffer = g_mhmap.vram800_320_hc_iv;   regions[ 11 ].size_bytes = sizeof ( g_mhmap.vram800_320_hc_iv );
    regions[ 12 ].buffer = g_mhmap.vram800_320_4a_i;    regions[ 12 ].size_bytes = sizeof ( g_mhmap.vram800_320_4a_i );
    regions[ 13 ].buffer = g_mhmap.vram800_320_4a_ii;   regions[ 13 ].size_bytes = sizeof ( g_mhmap.vram800_320_4a_ii );
    regions[ 14 ].buffer = g_mhmap.vram800_320_4b_i;    regions[ 14 ].size_bytes = sizeof ( g_mhmap.vram800_320_4b_i );
    regions[ 15 ].buffer = g_mhmap.vram800_320_4b_ii;   regions[ 15 ].size_bytes = sizeof ( g_mhmap.vram800_320_4b_ii );
    regions[ 16 ].buffer = g_mhmap.vram800_640_4_i;     regions[ 16 ].size_bytes = sizeof ( g_mhmap.vram800_640_4_i );
    regions[ 17 ].buffer = g_mhmap.vram800_640_4_ii;    regions[ 17 ].size_bytes = sizeof ( g_mhmap.vram800_640_4_ii );
    regions[ 18 ].buffer = g_mhmap.vram800_640_2a;      regions[ 18 ].size_bytes = sizeof ( g_mhmap.vram800_640_2a );
    regions[ 19 ].buffer = g_mhmap.vram800_640_2b;      regions[ 19 ].size_bytes = sizeof ( g_mhmap.vram800_640_2b );
    regions[ 20 ].buffer = g_mhmap.iorq_8bit;           regions[ 20 ].size_bytes = sizeof ( g_mhmap.iorq_8bit );
    regions[ 21 ].buffer = g_mhmap.iorq_gdg;            regions[ 21 ].size_bytes = sizeof ( g_mhmap.iorq_gdg );
    regions[ 22 ].buffer = g_mhmap.memext;              regions[ 22 ].size_bytes = sizeof ( g_mhmap.memext );
#elif MZARCH == 1500
    static st_MHMAP_EXPORT_REGION regions[ ] = {
        { "bus",       "bus",       "bus.cdl",       NULL, 0, MHMAP_SIZE_BUS       },
        { "ram",       "ram",       "ram.cdl",       NULL, 0, MHMAP_SIZE_RAM       },
        { "rom",       "rom",       "rom.cdl",       NULL, 0, MHMAP_SIZE_ROM       },
        { "cgrom",     "cgrom",     "cgrom.cdl",     NULL, 0, MHMAP_SIZE_CGROM     },
        { "vram",      "vram",      "vram.cdl",      NULL, 0, MHMAP_SIZE_VRAM      },
        { "pcg-1",     "pcg-1",     "pcg-1.cdl",     NULL, 0, MHMAP_SIZE_PCG_BANK  },
        { "pcg-2",     "pcg-2",     "pcg-2.cdl",     NULL, 0, MHMAP_SIZE_PCG_BANK  },
        { "pcg-3",     "pcg-3",     "pcg-3.cdl",     NULL, 0, MHMAP_SIZE_PCG_BANK  },
        { "iorq-8bit", "iorq-8bit", "iorq-8bit.cdl", NULL, 0, MHMAP_SIZE_IORQ_8BIT },
        { "memext",    "memext",    "memext.cdl",    NULL, 0, MHMAP_SIZE_MEMEXT    },
    };
    regions[ 0 ].buffer = g_mhmap.bus;       regions[ 0 ].size_bytes = sizeof ( g_mhmap.bus );
    regions[ 1 ].buffer = g_mhmap.ram;       regions[ 1 ].size_bytes = sizeof ( g_mhmap.ram );
    regions[ 2 ].buffer = g_mhmap.rom;       regions[ 2 ].size_bytes = sizeof ( g_mhmap.rom );
    regions[ 3 ].buffer = g_mhmap.cgrom;     regions[ 3 ].size_bytes = sizeof ( g_mhmap.cgrom );
    regions[ 4 ].buffer = g_mhmap.vram;      regions[ 4 ].size_bytes = sizeof ( g_mhmap.vram );
    regions[ 5 ].buffer = g_mhmap.pcg_1;     regions[ 5 ].size_bytes = sizeof ( g_mhmap.pcg_1 );
    regions[ 6 ].buffer = g_mhmap.pcg_2;     regions[ 6 ].size_bytes = sizeof ( g_mhmap.pcg_2 );
    regions[ 7 ].buffer = g_mhmap.pcg_3;     regions[ 7 ].size_bytes = sizeof ( g_mhmap.pcg_3 );
    regions[ 8 ].buffer = g_mhmap.iorq_8bit; regions[ 8 ].size_bytes = sizeof ( g_mhmap.iorq_8bit );
    regions[ 9 ].buffer = g_mhmap.memext;    regions[ 9 ].size_bytes = sizeof ( g_mhmap.memext );
#elif MZARCH == 700
    static st_MHMAP_EXPORT_REGION regions[ ] = {
        { "bus",       "bus",       "bus.cdl",       NULL, 0, MHMAP_SIZE_BUS       },
        { "ram",       "ram",       "ram.cdl",       NULL, 0, MHMAP_SIZE_RAM       },
        { "rom",       "rom",       "rom.cdl",       NULL, 0, MHMAP_SIZE_ROM       },
        { "cgrom",     "cgrom",     "cgrom.cdl",     NULL, 0, MHMAP_SIZE_CGROM     },
        { "vram",      "vram",      "vram.cdl",      NULL, 0, MHMAP_SIZE_VRAM      },
        { "iorq-8bit", "iorq-8bit", "iorq-8bit.cdl", NULL, 0, MHMAP_SIZE_IORQ_8BIT },
        { "memext",    "memext",    "memext.cdl",    NULL, 0, MHMAP_SIZE_MEMEXT    },
    };
    regions[ 0 ].buffer = g_mhmap.bus;       regions[ 0 ].size_bytes = sizeof ( g_mhmap.bus );
    regions[ 1 ].buffer = g_mhmap.ram;       regions[ 1 ].size_bytes = sizeof ( g_mhmap.ram );
    regions[ 2 ].buffer = g_mhmap.rom;       regions[ 2 ].size_bytes = sizeof ( g_mhmap.rom );
    regions[ 3 ].buffer = g_mhmap.cgrom;     regions[ 3 ].size_bytes = sizeof ( g_mhmap.cgrom );
    regions[ 4 ].buffer = g_mhmap.vram;      regions[ 4 ].size_bytes = sizeof ( g_mhmap.vram );
    regions[ 5 ].buffer = g_mhmap.iorq_8bit; regions[ 5 ].size_bytes = sizeof ( g_mhmap.iorq_8bit );
    regions[ 6 ].buffer = g_mhmap.memext;    regions[ 6 ].size_bytes = sizeof ( g_mhmap.memext );
#endif
    *out_count = sizeof ( regions ) / sizeof ( regions[ 0 ] );
    return regions;
}


/**
 * @brief Zapsat jeden region do souboru.
 */
static int mhmap_write_region_file ( const char *dir, const char *prefix,
                                     const st_MHMAP_EXPORT_REGION *r )
{
    /* Pokud prefix neprázdný, jméno = "<prefix>_<filename>", jinak jen filename. */
    char *file_with_prefix = NULL;
    if ( prefix && *prefix )
    {
        file_with_prefix = g_strdup_printf ( "%s_%s", prefix, r->filename );
    }
    char *path = g_build_filename ( dir, file_with_prefix ? file_with_prefix : r->filename, NULL );

    FILE *f = fopen ( path, "wb" );
    if ( !f )
    {
        fprintf ( stderr, "mhmap_export: cannot open '%s' for writing\n", path );
        g_free ( path );
        g_free ( file_with_prefix );
        return 1;
    }
    size_t n = fwrite ( r->buffer, 1, r->size_bytes, f );
    int err = ferror ( f );
    fclose ( f );
    if ( n != r->size_bytes || err )
    {
        fprintf ( stderr, "mhmap_export: short write to '%s' (%zu/%zu bytes)\n",
                  path, n, r->size_bytes );
        g_free ( path );
        g_free ( file_with_prefix );
        return 1;
    }
    g_free ( path );
    g_free ( file_with_prefix );
    return 0;
}


/**
 * @brief Vytvořit meta.json se seznamem regionů a metadaty.
 *
 * Implementace přes json-glib (JsonBuilder + JsonGenerator). Strukturní
 * kontrakt JSON souboru je nezměněn vůči předchozí ručně skládané verzi:
 *
 *   {
 *     "format_version": 2,
 *     "format": "memory-heatmap",
 *     "mzarch": <int>,
 *     "created_at": "YYYY-MM-DDThh:mm:ss",
 *     "file_prefix": "<prefix>",        // nepřítomný, pokud prefix=NULL/""
 *     "cell_size_bytes": <int>,
 *     "cell_layout": "r:uint32_le, w:uint32_le, x:uint32_le",
 *     "regions": [
 *       { "name": "...", "file": "...", "size_cells": <int>, "size_bytes": <int> },
 *       ...
 *     ]
 *   }
 *
 * Formátování (whitespace, indentace, případné trailing newline) se může
 * mírně lišit oproti dřívějšímu výstupu - kontrakt je strukturní, ne
 * byte-identický.
 *
 * @param meta_path  Plná cesta k meta.json souboru.
 * @param prefix     Prefix pro region soubory (typicky basename meta.json bez .json),
 *                   může být NULL = bez prefixu.
 * @param regions    Pole exportovaných regionů (vlastník: caller).
 * @param count      Počet prvků v poli regions.
 * @return 0 při úspěchu, jinak nenulová hodnota (chyba I/O nebo serializace).
 */
static int mhmap_write_meta_json ( const char *meta_path,
                                const char *prefix,
                                const st_MHMAP_EXPORT_REGION *regions,
                                size_t count )
{
    /* Časová značka exportu - ISO 8601 lokální čas. */
    char ts_buf[ 32 ] = { 0 };
    {
        time_t now = time ( NULL );
        struct tm tm_local;
#if defined(_WIN32)
        localtime_s ( &tm_local, &now );
#else
        localtime_r ( &now, &tm_local );
#endif
        strftime ( ts_buf, sizeof ( ts_buf ), "%Y-%m-%dT%H:%M:%S", &tm_local );
    }

    JsonBuilder *builder = json_builder_new ( );
    json_builder_begin_object ( builder );

    json_builder_set_member_name ( builder, "format_version" );
    json_builder_add_int_value ( builder, 2 );

    json_builder_set_member_name ( builder, "format" );
    json_builder_add_string_value ( builder, "memory-heatmap" );

    json_builder_set_member_name ( builder, "mzarch" );
    json_builder_add_int_value ( builder, MZARCH );

    json_builder_set_member_name ( builder, "created_at" );
    json_builder_add_string_value ( builder, ts_buf );

    if ( prefix && *prefix )
    {
        json_builder_set_member_name ( builder, "file_prefix" );
        json_builder_add_string_value ( builder, prefix );
    }

    json_builder_set_member_name ( builder, "cell_size_bytes" );
    json_builder_add_int_value ( builder, (gint64) sizeof ( st_MHMAP_CELL ) );

    json_builder_set_member_name ( builder, "cell_layout" );
    json_builder_add_string_value ( builder, "r:uint32_le, w:uint32_le, x:uint32_le, s:uint32_le" );

    json_builder_set_member_name ( builder, "regions" );
    json_builder_begin_array ( builder );
    for ( size_t i = 0; i < count; i++ )
    {
        json_builder_begin_object ( builder );

        json_builder_set_member_name ( builder, "name" );
        json_builder_add_string_value ( builder, regions[ i ].name );

        json_builder_set_member_name ( builder, "file" );
        if ( prefix && *prefix )
        {
            char *file_field = g_strdup_printf ( "%s_%s", prefix, regions[ i ].filename );
            json_builder_add_string_value ( builder, file_field );
            g_free ( file_field );
        }
        else
        {
            json_builder_add_string_value ( builder, regions[ i ].filename );
        }

        json_builder_set_member_name ( builder, "size_cells" );
        json_builder_add_int_value ( builder, regions[ i ].size_cells );

        json_builder_set_member_name ( builder, "size_bytes" );
        json_builder_add_int_value ( builder, (gint64) regions[ i ].size_bytes );

        json_builder_end_object ( builder );
    }
    json_builder_end_array ( builder );

    json_builder_end_object ( builder );

    JsonGenerator *gen = json_generator_new ( );
    /* json_builder_get_root() vrací JsonNode vlastněný builderem - NEsmí
     * se volat json_node_free(), uvolní se s g_object_unref(builder).
     * Generator si interně bere referenci přes json_generator_set_root. */
    JsonNode *root = json_builder_get_root ( builder );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    /* Místo json_generator_to_file() (na Windows má některé verze problém
     * s UTF-8 cestami) generujeme do paměti a zapisujeme binárně. */
    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );
    int rc = 0;
    FILE *fp = g_fopen ( meta_path, "wb" );
    if ( !fp )
    {
        fprintf ( stderr, "mhmap_export: cannot open '%s' for writing\n", meta_path );
        rc = 1;
    }
    else
    {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) rc = 1;
        if ( ferror ( fp ) ) rc = 1;
        fclose ( fp );
    }
    g_free ( out );
    g_object_unref ( gen );
    g_object_unref ( builder );
    return rc;
}


int mhmap_export ( const char *meta_path )
{
    if ( !meta_path || !*meta_path )
    {
        fprintf ( stderr, "mhmap_export: empty meta_path\n" );
        return -1;
    }

    /* Rozparsovat meta_path na adresář + basename (bez .json) = prefix.
     * Příklad: "/foo/bar/snap.json" → dir="/foo/bar", prefix="snap".
     * Pokud meta_path nekončí na .json, použijeme basename celý jako prefix. */
    char *dir   = g_path_get_dirname  ( meta_path );
    char *base  = g_path_get_basename ( meta_path );
    char *dot   = strrchr ( base, '.' );
    if ( dot && g_ascii_strcasecmp ( dot, ".json" ) == 0 )
    {
        *dot = 0;
    };

    if ( g_mkdir_with_parents ( dir, 0755 ) != 0 )
    {
        fprintf ( stderr, "mhmap_export: cannot create directory '%s'\n", dir );
        g_free ( dir );
        g_free ( base );
        return -1;
    }

    size_t count = 0;
    const st_MHMAP_EXPORT_REGION *regions = mhmap_get_export_regions ( &count );

    int errors = 0;
    for ( size_t i = 0; i < count; i++ )
    {
        errors += mhmap_write_region_file ( dir, base, &regions[ i ] );
    }

    errors += mhmap_write_meta_json ( meta_path, base, regions, count );

    g_free ( dir );
    g_free ( base );

    if ( errors )
    {
        fprintf ( stderr, "mhmap_export: %d error(s) during export to '%s'\n", errors, meta_path );
        return -1;
    }

    printf ( "Memory Heatmap exported to '%s' (%zu regions)\n", meta_path, count );
    return 0;
}


/* =========================================================================
 *  F8 - mhmap_view_t API pro disassembler-window CDL integraci
 * ========================================================================= */

/* Plnou definici @ref mhmap_view_t drží dasm_export.h (= veřejný typ
 * pro standalone testovatelnost). Sem include přitáhne struct layout,
 * aby create/destroy mohly alokovat členy. */
#include "dasm_export.h"


mhmap_view_t *mhmap_view_create_for_range ( uint16_t from, uint16_t to )
{
    if ( from > to ) return NULL;

    mhmap_view_t *view = (mhmap_view_t*) calloc ( 1, sizeof ( mhmap_view_t ) );
    if ( !view ) return NULL;

    const size_t span = (size_t) ( to - from ) + 1u;

    view->from        = from;
    view->to          = to;
    view->exec_flag   = (uint8_t*) calloc ( span, 1 );
    view->read_flag   = (uint8_t*) calloc ( span, 1 );
    view->write_flag  = (uint8_t*) calloc ( span, 1 );

    if ( !view->exec_flag || !view->read_flag || !view->write_flag )
    {
        mhmap_view_destroy ( view );
        return NULL;
    }

    /* Převod counter -> CDL flag (count > 0 == 1, jinak 0). Čteme přímo
     * z @c g_mhmap.bus[addr] - to jsou logické CPU adresy 0000h-FFFFh.
     * Pokud je mhmap_mode OFF, countery jsou 0 a flag pole zůstanou nuly
     * (= caller musí uživateli sdělit, že CDL data nejsou nasbíraná). */
    for ( size_t i = 0; i < span; i++ )
    {
        const unsigned addr = (unsigned) from + (unsigned) i;
        const st_MHMAP_CELL *cell = &g_mhmap.bus [ addr ];
        if ( cell->x ) view->exec_flag  [ i ] = 1;
        if ( cell->r ) view->read_flag  [ i ] = 1;
        if ( cell->w || cell->s ) view->write_flag [ i ] = 1;
    }

    return view;
}


void mhmap_view_destroy ( mhmap_view_t *view )
{
    if ( !view ) return;
    free ( view->exec_flag );
    free ( view->read_flag );
    free ( view->write_flag );
    free ( view );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED && (MZARCH == 800 || MZARCH == 1500) */
