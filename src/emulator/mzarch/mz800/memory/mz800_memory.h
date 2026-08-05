/*
 * File:   mz800_memory.h
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

#ifndef MZ800_MEMORY_H
#define MZ800_MEMORY_H

/* Per-arch velikost VRAM (mzhal 11c-2c, presunuto z memory.h) */
#define MEMORY_SIZE_VRAM (MEMORY_SIZE_VRAM_BANK * 2)    /* 16 KB - 4 roviny po 8 KB (2 banky) */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     *
     *  Flagy mapovani pameti
     *
     */
#define MEMORY_MZ800_MAP_FLAG_ROM_0000 (1 << 0)
#define MEMORY_MZ800_MAP_FLAG_ROM_1000 (1 << 1)
#define MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM (1 << 2) /* MZ700: 0xc000 - 0xcfff (CGRAM) \
                                                    MZ800: 0x8000 - 0x9fff | 0xbfff (VRAM) */
#define MEMORY_MZ800_MAP_FLAG_ROM_E000 (1 << 3)   /* + MZ700: 0xd000 - 0xdfff (atributova VRAM) */
#define MEMORY_MZ800_MAP_FLAG_PROHIBITED (1 << 4) /* OUT 0xE5/E6 - "Prohibited" banking mode \
                                                     na MZ-800. Aktivni = ctene $E009-$FFFF \
                                                     vraci 0x1A shadow byte; $E000-$E008 \
                                                     mapped ports zustavaji funkcni. \
                                                     Persistuje pres OUT E0/E1/E2/E3/E4 i \
                                                     pres DMD bit 3 switch (700 <-> 800). \
                                                     Reset: OUT E6, gdg_init/reset. \
                                                     Empiricky overeno: viz \
                                                     emu-experiments/mz700-mzarch/ \
                                                     test-programs/banking-e800/ \
                                                     vysledky/VYHODNOCENI.md. */

    /* Memory map porty pro IORQ - PWRITE */
    typedef enum en_MMAP_MZ800_PWRITE
    {
        MMAP_MZ800_PWRITE_E0 = 0xe0, /* memory unmap ROM 0000 , CGROM */
        MMAP_MZ800_PWRITE_E1 = 0xe1, /* memory unmap ROM E000, coz v MZ700 znamena i VRAM na D000 */
        MMAP_MZ800_PWRITE_E2 = 0xe2, /* memory map ROM 0000 */
        MMAP_MZ800_PWRITE_E3 = 0xe3, /* memory map ROM E000, coz v MZ700 znamena i VRAM na D000 */
        MMAP_MZ800_PWRITE_E4 = 0xe4, /* memory map ROM 0000, ROM E000, MZ700: unmap CGROM, CGRAM, MZ800: map CGROM, VRAM */
        /* pozustatky z MZ-700 - v MZ-800 ponekud nefunkcni */
        MMAP_MZ800_PWRITE_E5 = 0xe5, /* map EXROM */
        MMAP_MZ800_PWRITE_E6 = 0xe6, /* unmap EXROM */
    } en_MMAP_MZ800_PWRITE;

    /* Memory map porty pro IORQ - PREAD */
    typedef enum en_MMAP_MZ800_PREAD
    {
        MMAP_MZ800_PREAD_E0 = 0xe0, /* memory map CG-ROM, CG-RAM, VRAM - podle mode */
        MMAP_MZ800_PREAD_E1 = 0xe1, /* memory unmap CG-ROM, CG-RAM, VRAM - podle mode */
    } en_MMAP_MZ800_PREAD;

    extern uint8_t *g_memoryVRAM;
    extern uint8_t *g_memoryVRAM_I;
    extern uint8_t *g_memoryVRAM_II;
    extern uint8_t *g_memoryVRAM_III;
    extern uint8_t *g_memoryVRAM_IV;

    /*
     *
     *  Testy mapovacich stavu
     *
     */
#define MEMORY_MZ800_MAP_TEST_ROM_0000 (g_memory.map & MEMORY_MZ800_MAP_FLAG_ROM_0000)
#define MEMORY_MZ800_MAP_TEST_ROM_1000 (g_memory.map & MEMORY_MZ800_MAP_FLAG_ROM_1000)
#define MEMORY_MZ800_MAP_TEST_ROM_E000 (g_memory.map & MEMORY_MZ800_MAP_FLAG_ROM_E000)
#define MEMORY_MZ800_MAP_TEST_PROHIBITED (g_memory.map & MEMORY_MZ800_MAP_FLAG_PROHIBITED)
#define MEMORY_MZ800_MAP_TEST_VRAM (g_memory.map & MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM)
#define MEMORY_MZ800_MAP_TEST_CGRAM (GDG_DMD_TEST_MODE700 && MEMORY_MZ800_MAP_TEST_VRAM)
#define MEMORY_MZ800_MAP_TEST_VRAM_D000 (GDG_DMD_TEST_MODE700 && MEMORY_MZ800_MAP_TEST_ROM_E000)

#define MEMORY_MZ800_MAP_TEST_VRAM_8000 ((!GDG_DMD_TEST_MODE700) && MEMORY_MZ800_MAP_TEST_VRAM)
#define MEMORY_MZ800_MAP_TEST_VRAM_A000 (MEMORY_MZ800_MAP_TEST_VRAM_8000 && GDG_MZ800_DMD_TEST_SCRW640)

    // static inline int memory_test_addr_is_vram(uint16_t addr)
    // {
    //     int a = addr >> 12;
    //     if (
    //         ((MEMORY_MZ800_MAP_TEST_CGRAM) && (a == 0x0c)) ||
    //         ((MEMORY_MZ800_MAP_TEST_VRAM_D000) && (a == 0x0d)) ||
    //         ((MEMORY_MZ800_MAP_TEST_VRAM_8000) && (a == 0x08)) ||
    //         ((MEMORY_MZ800_MAP_TEST_VRAM_A000) && (a == 0x0a)))
    //     {
    //         return 1;
    //     };
    //     return 0;
    // }

#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /**
     * @brief Prepocet RAM-access fast-path page-table (E1, KROK 1).
     *
     * Volat pri kazde zmene mapovani (banking switch, DMD switch). Detail viz
     * definice v mz800_memory.c.
     */
    void mz800_ram_fastpath_rebuild ( void );

    /**
     * @brief Resync fast-path gating po zmene read/write callbacku (logging on/off).
     */
    void mz800_ram_fastpath_resync ( void );
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZ800_MEMORY_H */
