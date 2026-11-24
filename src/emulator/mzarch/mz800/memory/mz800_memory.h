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
                                                     na MZ-800. Aktivni = cteni CELE \
                                                     $E000-$FFFF vraci posledni bajt na \
                                                     datove sbernici (mereni na MZ-800; \
                                                     drivejsi 0x1A byl kod LD A,(DE)), \
                                                     vcetne mapped ports $E000-$E008 - \
                                                     ctecí makra testuji tento priznak jako \
                                                     PRVNI podminku, takze se k portum \
                                                     nedojde (v0.5 T4: $E001 nevraci \
                                                     klavesnicovy scan); zapis se neprovede. \
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
    /** Roviny III a IV pro čtení (video fetch, čtení CPU): g_memory.EXVRAM, nebo
     *  při neosazeném rozšíření VRAM buffer se samými 0FFh. */
    extern uint8_t *g_memoryVRAM_III;
    extern uint8_t *g_memoryVRAM_IV;

    /** Roviny III a IV pro zápis CPU: g_memory.EXVRAM, nebo při neosazeném
     *  rozšíření VRAM odpadní buffer (zápis se ztratí). */
    extern uint8_t *g_memoryVRAM_III_WR;
    extern uint8_t *g_memoryVRAM_IV_WR;

    /** 1 = osazené rozšíření VRAM MZ-1R25 (roviny III a IV, 32 KB VRAM),
     *  0 = jen základních 16 KB. Konfigurace [MZ800] exvram v INI, výchozí 1.
     *  Mění ho jen mz800_memory_set_exvram(). */
    extern unsigned g_mz800_exvram_installed;

    /**
     * @brief Osadí nebo odpojí rozšíření VRAM MZ-1R25 (roviny III a IV).
     *
     * Bez rozšíření čte video fetch i CPU z rovin III a IV 0FFh a zápisy do
     * nich se ztratí (mz800-knowledge hw/10-vram-organization.md; C model
     * netlistu gdg-mz800 gdg-documented/20-extended-vram-missing.md, model
     * VRAM s vc_installed = 0). Co čte GDG z neosazených patic na skutečném
     * MZ-800, je na HW neověřené [neověřeno]. Obsah g_memory.EXVRAM zůstává
     * (snapshot, debugger) a po novém osazení je zase vidět.
     *
     * Bez kontroly v rychlé cestě: přepíná jen ukazatele rovin.
     *
     * @param installed Nenulové = osazeno.
     * @pre Emu vlákno, nebo neběžící emulace.
     * @post g_mz800_exvram_installed, g_memoryVRAM_III/IV a *_WR odpovídají.
     */
    extern void mz800_memory_set_exvram(unsigned installed);

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
#define MEMORY_MZ800_MAP_TEST_CGRAM (GDG_MZ800_DMD_TEST_MZ700 && MEMORY_MZ800_MAP_TEST_VRAM)
#define MEMORY_MZ800_MAP_TEST_VRAM_D000 (GDG_MZ800_DMD_TEST_MZ700 && MEMORY_MZ800_MAP_TEST_ROM_E000)

#define MEMORY_MZ800_MAP_TEST_VRAM_8000 ((!GDG_MZ800_DMD_TEST_MZ700) && MEMORY_MZ800_MAP_TEST_VRAM)
#define MEMORY_MZ800_MAP_TEST_VRAM_A000 (MEMORY_MZ800_MAP_TEST_VRAM_8000 && GDG_MZ800_DMD_TEST_SCRW640)

/**
 * @brief Nedokumentované režimy DMD 0Ch až 0Fh (režim MZ-700 s bitem 640).
 *
 * Podle C modelu netlistu GDG (emu-experiments hw-fixes/research/dmd0c/SPEC.md,
 * pravidlo 3) má buňka VRAM v těchto režimech adresu jako v 640x200: CPU
 * adresa a jde na offset roviny I (a >> 1) & 1FFFh (A0 se neuplatní) a okno
 * VRAM je 0A000h až 0DFFFh, když je připojená CG-RAM (jinak jen 0D000h až
 * 0DFFFh). Na stroji změřeno jen to, že bajt zapsaný v 0Ch se přečte v 0Fh
 * a ne v 08h (mz800-knowledge hw-measurement/vysledky.md oddíl 17, U5);
 * okno 0A000h až 0BFFFh je jen z modelu [neověřeno na HW].
 */
#define MEMORY_MZ800_MAP_TEST_DMD0C ((g_gdg.regDMD & 0x0c) == 0x0c)
/** @brief Okno 0A000h až 0BFFFh v DMD 0Ch až 0Fh (text a atributy, viz MEMORY_MZ800_MAP_TEST_DMD0C). */
#define MEMORY_MZ800_MAP_TEST_VRAM700_A000 (MEMORY_MZ800_MAP_TEST_CGRAM && MEMORY_MZ800_MAP_TEST_DMD0C)

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
