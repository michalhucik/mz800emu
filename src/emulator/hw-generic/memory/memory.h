/*
 * File:   memory.h
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

#ifndef MEMORY_H
#define MEMORY_H
#include "mzarch/mzcommon_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */

#include "libs/cpu-z80/z80.h"
#include "memory/rom.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef MEMORY_MAKE_STATISTICS

#define MEMORY_STATISTIC_DAT_FILE "memstats.dat"
#define MEMORY_STATISTIC_TXT_FILE "memstats.txt"

    typedef struct st_MEMORY_STATS
    {
        unsigned long long read[16];
        unsigned long long write[16];
    } st_MEMORY_STATS;
#endif

    /*
     *
     * Velikost jednotlivych pameti
     *
     */
#define MEMORY_SIZE_RAM 0x10000

/*
 * Superset dimenze polí st_MEMORY (mzhal 9d): pole mají MAX velikost
 * přes všechny architektury, takže st_MEMORY má jediný layout pro
 * všechna 4 EXE. Skutečnou velikost paměti dané platformy nese
 * g_mzhal (mem_vram_size, mem_exvram_size, mem_pcg_size; 0 = paměť
 * na platformě neexistuje). Per-arch makra MEMORY_SIZE_VRAM /
 * MEMORY_SIZE_PCG zůstávají pro per-EXE kód a plnění g_mzhal.
 */
#define MEMORY_SIZE_VRAM_BANK 0x2000
#define MEMORY_SIZE_VRAM_MAX (MEMORY_SIZE_VRAM_BANK * 2) /* 16 KB (MZ-800) */
#define MEMORY_SIZE_PCG_BANK 0x2000
#define MEMORY_SIZE_PCG_MAX (MEMORY_SIZE_PCG_BANK * 3)   /* 24 KB (MZ-1500) */

/* Per-arch MEMORY_SIZE_VRAM / MEMORY_SIZE_PCG jsou v mz*_memory.h
 * (mzhal 11c-2c) - sdílený kód čte g_mzhal.mem_*_size. */

#define MEMORY_MEMRAM_POINTS 0x10

    /*
     *
     *  Fyzicke umisteni vsech pameti
     *
     */
    typedef struct st_MEMORY
    {
        uint8_t map;
        uint8_t *memram_read[MEMORY_MEMRAM_POINTS];
        uint8_t *memram_write[MEMORY_MEMRAM_POINTS];
        uint8_t ROM[ROM_SIZE_TOTAL];
        uint8_t RAM[MEMORY_SIZE_RAM];
        /* Superset (mzhal 9d): pole MAX velikosti existují na všech
         * platformách; skutečné využití viz g_mzhal.mem_*_size, mimo
         * vlastní platformu zůstávají nulová (BSS) a nikdo je nesmí
         * plnit ani číst. */
        uint8_t VRAM[MEMORY_SIZE_VRAM_MAX];
        uint8_t EXVRAM[MEMORY_SIZE_VRAM_MAX];         /* MZ-800: rozsirena VRAM (banky III, IV) */
        uint8_t PCG[MEMORY_SIZE_PCG_MAX];             /* MZ-1500: 3 PCG banky x 8 KB */
    } st_MEMORY;

    extern st_MEMORY g_memory;

/* Per-arch dispatch (mz*_memory.h) je vyčleněn do memory_arch.h
 * (mzhal 11c-2c) - includují ho jen per-EXE / dirty TU. */

    /*
     *
     *  Definice IO operace pouzite pro mapovani
     *
     */
    typedef enum
    {
        MEMORY_MAPRQ_IOOP_IN = 0,
        MEMORY_MAP_IOOP_OUT
    } MEMORY_MAP_IOOP;

    /**
     * @brief Druh paměťového regionu z pohledu Z80 pro 4 kB stránku.
     *
     * Společný enum přes všechny architektury (MZ-700, MZ-800, MZ-1500).
     * Každá architektura má svoji `memmap_query()` funkci, která pro
     * adresní stránku 0..15 (= 4 kB blok) vrátí druh regionu reflektující
     * aktuální banking stav (= `g_memory.map`, případně `g_gdg.regDMD`
     * u MZ-800).
     *
     * Slouží pro Memory Map debug okno (sloupec Banking) - mapování
     * enum -> (text label, barva) je v UI vrstvě.
     *
     * Některé hodnoty enum jsou platné jen pro určité architektury
     * (např. PCG_1/2/3 jen MZ-1500, VRAM_I/II jen MZ-800). UI to musí
     * zvládnout dle běžící architektury.
     */
    typedef enum en_MEMMAP_REGION_KIND
    {
        MEMMAP_KIND_RAM = 0,         /**< Klasická DRAM (případně přes Memext bank pointer). */
        MEMMAP_KIND_ROM_LOW,         /**< MZ-700/800/1500: Monitor ROM 0x0000-0x0FFF (případně 0x1000-0x1FFF). */
        MEMMAP_KIND_ROM_HIGH,        /**< Monitor ROM 0xE000-0xFFFF (E010-FFFF; E000-E00F mají speciální pravidla). */
        MEMMAP_KIND_CGROM,           /**< CG-ROM. MZ-800 native: 0x1000-0x1FFF; MZ-1500 D000-EFFF (SPEC=1). */
        MEMMAP_KIND_VRAM_I,          /**< MZ-800 native: VRAM blok I (0x8000-0x9FFF). */
        MEMMAP_KIND_VRAM_II,         /**< MZ-800 native, SCRW640: VRAM blok II (0xA000-0xBFFF). */
        MEMMAP_KIND_VRAM_TEXT,       /**< MZ-700 / MZ-800 v MZ-700 modu: znaková + atributová VRAM (0xD000-0xDFFF). */
        MEMMAP_KIND_CGRAM,           /**< MZ-700 / MZ-800 v MZ-700 modu: CG-RAM (0xC000-0xCFFF). */
        MEMMAP_KIND_PCG_1,           /**< MZ-1500: PCG bank 1 v 0xD000-0xEFFF (SPEC=2). */
        MEMMAP_KIND_PCG_2,           /**< MZ-1500: PCG bank 2 (SPEC=3). */
        MEMMAP_KIND_PCG_3,           /**< MZ-1500: PCG bank 3 (SPEC=4). */
        MEMMAP_KIND_MAPPED_PORTS,    /**< MZ-700 / MZ-800 v MZ-700 modu / MZ-1500: 0xE000-0xE00F mapped PIO/CTC/GDG ports. */
        MEMMAP_KIND_PROHIBITED,      /**< MZ-700/800: Prohibited mode (OUT E5) - čtení vrací 0x1A / 0xFF shadow. */
        MEMMAP_KIND_UNMAPPED         /**< Nedefinovaný / 0xFF (např. MZ-1500 F000-FFFF + SPEC active). */
    } en_MEMMAP_REGION_KIND;

    /**
     * @brief Vrátí druh regionu pro 4 kB stránku z pohledu Z80.
     *
     * Read-only, side-effect free; čte aktuální `g_memory.map` a
     * případně `g_gdg.regDMD` (MZ-800). Vhodné pro UI hot path
     * (Memory Map debug okno).
     *
     * Per-architektura má vlastní implementaci v
     * `mzarch/<arch>/memory/<arch>_memory.c`. Jméno funkce je
     * jednotné napříč architekturami (= dispatch přes compile-time
     * MZARCH).
     *
     * @param addr_point  Adresní stránka 0..15 (= adresa addr_point*0x1000).
     * @return            Druh regionu na dané stránce; pro neznámé /
     *                    out-of-range hodnoty vrací MEMMAP_KIND_RAM
     *                    (= bezpečný default).
     */
    extern en_MEMMAP_REGION_KIND memmap_query(uint8_t addr_point);

    /*
     *
     *  Obsluzne funkce pameti
     *
     */

    /* Tepla inicializace pameti */
    extern void memory_reset(void);

    /* Studena inicializace pameti */
    extern void memory_init(void);

    /* Zmena pripojeni bank memextu */
    extern void memory_reconnect_ram(void);

    /* Callback pro cteni bajtu */
    extern uint8_t memory_read_cb(z80_t *cpu, uint16_t addr, int m1_state, void *user_data);

    /* Callback pro cteni bajtu + debugovaci informace o vykonavanych bajtech */
    extern uint8_t memory_read_with_logging_cb(z80_t *cpu, uint16_t addr, int m1_state, void *user_data);

    /* Callback pro zapis bajtu */
    extern void memory_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

    /* Callback pro zapis bajtu + debugovaci informace (CDL recording při aktivním debuggeru) */
    extern void memory_write_with_logging_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data);

    /* Nastaveni mapy pameti */
    extern void memory_map_pwrite(uint8_t mmap_port, uint8_t value);
    extern void memory_map_pread(uint8_t mmap_port);

    /* Cteni z aktualne mapovane pameti - bez synchronizace */
    extern uint8_t memory_read_byte(uint16_t addr);

    /**
     * Debugger-safe variant memory_read_byte (V1.6+ TODO 4.2 5a).
     *
     * Identicke chovani jako memory_read_byte (= banking + RAM read),
     * ale BEZ MEM_R BP fire side-effectu. Pouziva se v BP expression
     * evaluatoru pro [addr] deref bez side-effect (vs [addr]! ktery
     * volá memory_read_byte = with side-effect).
     *
     * Bezpecnostni invariant: implementace nesmi volat
     * breakpoints_enforce_mem_r ani jiny side-effect-laden hook.
     * Side-effecty mimo BP fire (= tape state, IRQ ack) v MZ-800
     * memory subsystemu **neexistuji** (= memory_read_byte je pure
     * read mimo BP hook), takze no_se = read - BP fire.
     */
    extern uint8_t memory_read_byte_no_se(uint16_t addr);

    /* Zapis do aktualne mapovane pameti - bez synchronizace */
    extern void memory_write_byte(uint16_t addr, uint8_t value);

    typedef enum en_MEMORY_LOAD
    {
        MEMORY_LOAD_MAPED = 0,
        MEMORY_LOAD_RAMONLY,
    } en_MEMORY_LOAD;

    /* Nacteni datoveho bloku do pameti */
    extern void memory_load_block(uint8_t *data, uint16_t addr, uint16_t size, en_MEMORY_LOAD type);

    static inline uint8_t memory_pure_ram_read_byte(uint16_t addr)
    {
        // return g_memory.RAM [ addr ];
        return (g_memory.memram_read[(addr >> 12)])[(addr & 0x0fff)];
    }

#ifdef MEMORY_MAKE_STATISTICS
    void memory_write_memory_statistics(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_H */
