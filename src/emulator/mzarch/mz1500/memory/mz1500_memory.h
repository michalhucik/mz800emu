/*
 * File:   mz1500_memory.h
 * Author: chaky
 *
 * Created on 15. června 2015, 17:34
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

#ifndef MZ1500_MEMORY_H
#define MZ1500_MEMORY_H

/* Per-arch velikosti pameti (mzhal 11c-2c, presunuto z memory.h) */
#define MEMORY_SIZE_VRAM 0x1000                          /* 4 KB - znakova + atributova VRAM */
#define MEMORY_SIZE_PCG (MEMORY_SIZE_PCG_BANK * 3)       /* 24 KB - 3 PCG banky */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     *
     *  Flagy mapovani pameti
     *
     *  Rozlozeni bitu v g_memory.map:
     *
     *  bit 0: ROM_0000 — ROM na 0x0000-0x0FFF
     *  bit 1: ROM_UPPER — horni oblast (VRAM D000, porty E000, ROM E800)
     *  bit 2-4: SPEC — specialni mapovani D000-EFFF (0=zadne, 1=CGROM, 2=PCG1, 3=PCG2, 4=PCG3)
     *
     */
#define MEMORY_MZ1500_MAP_FLAG_ROM_0000  (1 << 0)
#define MEMORY_MZ1500_MAP_FLAG_ROM_UPPER (1 << 1)

#define MEMORY_MZ1500_FLAG_SPEC_BITPOS    2
#define MEMORY_MZ1500_MAP_D000_MASK       (7 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)
#define MEMORY_MZ1500_MAP_D000_NONE       (0 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)
#define MEMORY_MZ1500_MAP_D000_CGROM      (1 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)
#define MEMORY_MZ1500_MAP_D000_PCG1       (2 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)
#define MEMORY_MZ1500_MAP_D000_PCG2       (3 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)
#define MEMORY_MZ1500_MAP_D000_PCG3       (4 << MEMORY_MZ1500_FLAG_SPEC_BITPOS)

    /* globalni ukazatele na PCG banky */
    extern uint8_t *g_memoryPCG1;
    extern uint8_t *g_memoryPCG2;
    extern uint8_t *g_memoryPCG3;

    /*
     *
     *  Testy mapovacich stavu
     *
     */
#define MEMORY_MZ1500_MAP_TEST_ROM_0000    (g_memory.map & MEMORY_MZ1500_MAP_FLAG_ROM_0000)
#define MEMORY_MZ1500_MAP_TEST_ROM_UPPER   (g_memory.map & MEMORY_MZ1500_MAP_FLAG_ROM_UPPER)
#define MEMORY_MZ1500_MAP_TEST_RAM_UPPER   (!MEMORY_MZ1500_MAP_TEST_ROM_UPPER)

    /* z map urcime SPEC id: 0 = zadne, 1 = CGROM, 2-4 = PCG1-3 */
#define MEMORY_MZ1500_SPEC_ID             ((g_memory.map & MEMORY_MZ1500_MAP_D000_MASK) >> MEMORY_MZ1500_FLAG_SPEC_BITPOS)

    /* testy stavu D000 oblasti */
#define MEMORY_MZ1500_MAP_TEST_D000_VRAM   (MEMORY_MZ1500_MAP_TEST_ROM_UPPER && !(g_memory.map & MEMORY_MZ1500_MAP_D000_MASK))
#define MEMORY_MZ1500_MAP_TEST_D000_SPEC   (MEMORY_MZ1500_MAP_TEST_ROM_UPPER && (g_memory.map & MEMORY_MZ1500_MAP_D000_MASK))
#define MEMORY_MZ1500_MAP_TEST_E000_PORTS  (MEMORY_MZ1500_MAP_TEST_D000_VRAM)
#define MEMORY_MZ1500_MAP_TEST_E800_ROM    (MEMORY_MZ1500_MAP_TEST_D000_VRAM)

#ifdef __cplusplus
}
#endif

#endif /* MZ1500_MEMORY_H */
