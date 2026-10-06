/*
 * File:   mhmap.h
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
 * @file mhmap.h
 * @brief Memory Heatmap - počítá přístupy R/W/X ke každé paměťové buňce.
 *
 * Memory Heatmap (MH) během běhu emulovaného programu agreguje statistiku
 * přístupů ke každé paměťové buňce a každému I/O portu. Klasifikuje přístupy
 * do čtyř typů:
 *
 *  - R = data read (mimo M1 a mimo operand bajt právě dekódované instrukce)
 *  - W = zápis do paměti / portu (kromě stack write klasifikovaného jako S)
 *  - X = bajt načtený jako součást právě dekódované instrukce (M1 + operandy)
 *  - S = stack write (V5) - zápis cílící do některého aktivního stack regionu
 *        (viz stack_regions.h). Klasifikace je region-based: pokud bus_addr
 *        spadá do <limit..base> některého definovaného stack regionu, jde
 *        o S, jinak W. S a W jsou exkluzivní pro jeden write event. Pokud
 *        není definován žádný stack region (= g_stack_regions_active = false),
 *        chování je beze změny vůči R/W/X (= S counter zůstane 0).
 *
 * Loguje paralelně logickou CPU adresu (mapa @c bus) i fyzickou paměť
 * (per-arch sada regionů). Klasický CDL bitmap (FCEUX-style) je z counterů
 * derivovatelný: flag = (count > 0).
 *
 * Aktivace: režim @c g_debugger.mhmap_mode (@ref en_DEBUGGER_MHMAP_MODE: OFF /
 * WITH_WINDOW = jen při otevřeném okně debuggeru / ALWAYS = i bez něj),
 * vyhodnocený makrem @c TEST_DEBUGGER_MHMAP_ACTIVE. Hot path emulátoru bez aktivního debuggeru i bez aktivní MH
 * zůstává nedotčen, žádné větvení v běžné instrukční smyčce. Pomalou cestu
 * přes @c memory_*_with_logging_cb použije emulátor, pokud je aktivní debugger
 * NEBO MH (callback swap pokrývá obojí).
 *
 * Statická alokace v BSS, žádný malloc. Velikost @c g_mhmap (16 B na buňku,
 * dominuje region Memext 512 K buněk = 8 MiB):
 *  - MZ-800: 13 508 608 B (~12.9 MiB, 844 288 buněk) - změřeno
 *    sizeof(st_MHMAP) v testovacím buildu MZ-800 (2026-10-04),
 *  - MZ-1500: 11 276 288 B (~10.8 MiB), MZ-700: 10 883 072 B (~10.4 MiB) -
 *    jen výpočtem z MHMAP_SIZE_* níže [neměřeno].
 *
 * Modul je arch-specific - region enum a state struct se liší podle MZARCH:
 *  - MZ-800: 4 VRAM plane, banking ROM_LOWER/CG/UPPER, GDG 16-bit IORQ
 *    (MZ-700 mód MZ-800 používá regiony VRAM700_CG / VRAM700)
 *  - MZ-1500: 1 VRAM, 3 PCG banky, CGROM, banking ROM/ROM_UPPER, žádné GDG
 *  - MZ-700 (samostatná arch): jako MZ-1500 bez PCG bank
 */

#ifndef MHMAP_H
#define MHMAP_H

#include "mzarch/mzarch_config.h"

#if defined(MZ800EMU_CFG_DEBUGGER_ENABLED) && ( ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 ) )

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>

#include "debugger.h"     /* en_DEBUGGER_MHMAP_MODE */


    /**
     * @brief Druh přístupu k cele.
     *
     * Pro paměťové mapy se používají všechny tři. Pro porty (@c MH_REGION_IORQ_*)
     * dávají smysl jen @c MHMAP_ACCESS_R (IN) a @c MHMAP_ACCESS_W (OUT); X se na
     * portech nepočítá (Z80 nečte port jako instrukci).
     */
    typedef enum en_MHMAP_ACCESS
    {
        MHMAP_ACCESS_R = 0,           /**< data read mimo právě dekódovanou instrukci */
        MHMAP_ACCESS_W,               /**< zápis mimo stack regiony (= běžná data) */
        MHMAP_ACCESS_X,               /**< execute - bajt právě dekódované instrukce (M1 nebo její operand) */
        MHMAP_ACCESS_S,               /**< stack write (V5) - zápis do aktivního stack regionu */
        MHMAP_ACCESS__COUNT,          /**< sentinel - počet typů, ne validní typ */
    } en_MHMAP_ACCESS;


    /**
     * @brief Výsledek resolveru sběrnicové adresy.
     *
     * @c MHMAP_RESOLVE_DIRECT - @p out_region a @p out_offset jsou platné, caller
     * volá @ref mhmap_inc.
     *
     * @c MHMAP_RESOLVE_SKIP - fyzický recording přeskočit. Důvody:
     *  - MZ-800 VRAM (0x8000-0xBFFF): planar logika je netriviální, klasifikaci
     *    dělá hook uvnitř @c vramctrl_mz800_memop_*.
     *  - MZ-1500: některé adresy v ROM_UPPER nemají fyzický cíl (memory-mapped
     *    porty E000-E00F, F000-FFFF v SPEC modu vrací 0xFF).
     *
     * Bus mapu zapisuje caller vždy bez ohledu na výsledek resolveru.
     */
    typedef enum en_MHMAP_RESOLVE_RESULT
    {
        MHMAP_RESOLVE_DIRECT = 0,         /**< @p out_region + @p out_offset jsou platné */
        MHMAP_RESOLVE_SKIP,               /**< caller přeskočí fyzický zápis */
    } en_MHMAP_RESOLVE_RESULT;


/* ============================================================================
 *
 *  Per-architektura: region enum, velikosti regionů, state struct
 *
 * ============================================================================
 */

#if MZARCH == 800

    /**
     * @brief Identifikátor regionu MH pro MZ-800 (a MZ-700 mód).
     *
     * VRAM regiony jsou rozděleny do dvou skupin:
     *  - @c MHMAP_REGION_VRAM = fyzický pohled, 32 KB linear stack 4 bank
     *    (VRAM1 0x0000-0x1FFF, VRAM2 0x2000-0x3FFF, VRAM3 0x4000-0x5FFF,
     *    VRAM4 0x6000-0x7FFF). Recording dle WE bitmask v hardware.
     *  - mode-specific regiony = co se dělalo z hlediska "logické plane"
     *    v daném grafickém režimu, indexované sběrnicovým offsetem
     *    (= addr před & 0x3fff). Selekce dle WF_PLANE bitů + DMD mode.
     *
     * MZ-700 mód využívá @c VRAM700_CG (CG-RAM na C000-CFFF) a
     * @c VRAM700 (text+attr na D000-DFFF). Fyzicky to leží v VRAM1
     * (Plane I), takže recording probíhá také do @c VRAM (32 KB).
     */
    typedef enum en_MHMAP_REGION
    {
        MHMAP_REGION_BUS = 0,             /**< logická CPU adresa 0000h-FFFFh */
        MHMAP_REGION_RAM,                 /**< hlavní DRAM 64 KB */
        MHMAP_REGION_ROM_LOWER,           /**< ROM monitor 0000h-0FFFh, 4 KB */
        MHMAP_REGION_ROM_CG,              /**< CG-ROM 1000h-1FFFh, 4 KB */
        MHMAP_REGION_ROM_UPPER,           /**< ROM monitor E000h-FFFFh, 8 KB */

        /* === VRAM === */
        MHMAP_REGION_VRAM,                /**< 32 KB fyzická VRAM (4× 8 KB stack VRAM1..4) */
        MHMAP_REGION_VRAM700_CG,          /**< 4 KB - CG-RAM v MZ-700 modu, bus C000-CFFF */
        MHMAP_REGION_VRAM700,             /**< 4 KB - text+attr v MZ-700 modu, bus D000-DFFF */

        /* MZ-800 mode-specific - 320x200 hicolor (16 barev), všechny 4 plane */
        MHMAP_REGION_VRAM800_320_HC_I,    /**< 8 KB, indexováno (bus_addr - 0x8000) */
        MHMAP_REGION_VRAM800_320_HC_II,
        MHMAP_REGION_VRAM800_320_HC_III,
        MHMAP_REGION_VRAM800_320_HC_IV,

        /* MZ-800 320x200 4 barvy, banka A (VBANK=0) */
        MHMAP_REGION_VRAM800_320_4A_I,    /**< 8 KB */
        MHMAP_REGION_VRAM800_320_4A_II,

        /* MZ-800 320x200 4 barvy, banka B (VBANK=1) */
        MHMAP_REGION_VRAM800_320_4B_I,    /**< 8 KB */
        MHMAP_REGION_VRAM800_320_4B_II,

        /* MZ-800 640x200 4 barvy, 2 logické plane */
        MHMAP_REGION_VRAM800_640_4_I,     /**< 16 KB, indexováno (bus_addr - 0x8000) */
        MHMAP_REGION_VRAM800_640_4_II,

        /* MZ-800 640x200 2 barvy, banka A/B */
        MHMAP_REGION_VRAM800_640_2A,      /**< 16 KB */
        MHMAP_REGION_VRAM800_640_2B,      /**< 16 KB */

        MHMAP_REGION_IORQ_8BIT,           /**< 8-bit I/O porty 00h-FFh, 256 cells */
        MHMAP_REGION_IORQ_GDG,            /**< 16-bit GDG sub-funkce (placeholder) */
        MHMAP_REGION_MEMEXT,              /**< Memext RAM, 512 KB (jen pokud připojen) */
        MHMAP_REGION__COUNT,              /**< sentinel */
    } en_MHMAP_REGION;

    #define MHMAP_SIZE_BUS                0x10000
    #define MHMAP_SIZE_RAM                0x10000
    #define MHMAP_SIZE_ROM_LOWER          0x1000
    #define MHMAP_SIZE_ROM_CG             0x1000
    #define MHMAP_SIZE_ROM_UPPER          0x2000
    #define MHMAP_SIZE_MEMEXT             0x80000  /**< 512 KB Memext RAM (= MEMEXT_RAM_SIZE) */

    /* VRAM */
    #define MHMAP_SIZE_VRAM               0x8000  /**< 32 KB fyzická 4 banky × 8 KB */
    #define MHMAP_SIZE_VRAM700_CG         0x1000  /**< 4 KB MZ-700 CG-RAM */
    #define MHMAP_SIZE_VRAM700            0x1000  /**< 4 KB MZ-700 text+attr */
    #define MHMAP_SIZE_VRAM800_320_PLANE  0x2000  /**< 8 KB - per logická plane v 320 modu */
    #define MHMAP_SIZE_VRAM800_640_PLANE  0x4000  /**< 16 KB - per logická plane v 640 modu */

    #define MHMAP_SIZE_IORQ_8BIT          0x100
    #define MHMAP_SIZE_IORQ_GDG           0x100   /**< placeholder - TODO upřesnit */

    /**
     * @name Offsety bank ve fyzickém vram[] (32 KB).
     *
     * VRAM1..VRAM4 jsou 4 fyzické 8 KB banky uvnitř g_memory.VRAM (16 KB)
     * a g_memory.EXVRAM (16 KB). Layout vram region 1:1 odpovídá
     * g_memoryVRAM_I/II/III/IV pointerům.
     * @{
     */
    #define MHMAP_VRAM_BANK1_OFFSET       0x0000  /**< VRAM1 = Plane I bank */
    #define MHMAP_VRAM_BANK2_OFFSET       0x2000  /**< VRAM2 = Plane II bank */
    #define MHMAP_VRAM_BANK3_OFFSET       0x4000  /**< VRAM3 = Plane III bank (= EXVRAM offset 0) */
    #define MHMAP_VRAM_BANK4_OFFSET       0x6000  /**< VRAM4 = Plane IV bank */
    /** @} */

#elif MZARCH == 1500

    /**
     * @brief Identifikátor regionu MH pro MZ-1500.
     *
     * MZ-1500 má znakově orientovanou grafiku jako MZ-700 (žádný planar
     * interleave, žádné WF/RF registry), ale navíc 3 PCG banky pro
     * programovatelný character generator a sprity.
     */
    typedef enum en_MHMAP_REGION
    {
        MHMAP_REGION_BUS = 0,         /**< logická CPU adresa 0000h-FFFFh */
        MHMAP_REGION_RAM,             /**< hlavní DRAM 64 KB */
        MHMAP_REGION_ROM,             /**< monitor ROM, 16 KB (lower 0000h + upper E000h-FFFFh ve fyzickém ROM blob) */
        MHMAP_REGION_CGROM,           /**< CG-ROM 4 KB (mapovaný na D000-DFFF / E000-EFFF přes SPEC=1) */
        MHMAP_REGION_VRAM,            /**< znaková + atributová VRAM, 4 KB */
        MHMAP_REGION_PCG_1,           /**< PCG bank 1, 8 KB (4 KB viděné na D000 + 4 KB viděné na E000) */
        MHMAP_REGION_PCG_2,           /**< PCG bank 2, 8 KB */
        MHMAP_REGION_PCG_3,           /**< PCG bank 3, 8 KB */
        MHMAP_REGION_IORQ_8BIT,       /**< 8-bit I/O porty 00h-FFh, 256 cells */
        MHMAP_REGION_MEMEXT,          /**< Memext RAM, 512 KB (jen pokud připojen) */
        MHMAP_REGION__COUNT,          /**< sentinel */
    } en_MHMAP_REGION;

    #define MHMAP_SIZE_BUS                0x10000
    #define MHMAP_SIZE_RAM                0x10000
    #define MHMAP_SIZE_ROM                0x4000   /**< 16 KB - celá ROM */
    #define MHMAP_SIZE_CGROM              0x1000   /**< 4 KB */
    #define MHMAP_SIZE_VRAM               0x1000   /**< 4 KB - znaková + atributová */
    #define MHMAP_SIZE_PCG_BANK           0x2000   /**< 8 KB per bank */
    #define MHMAP_SIZE_IORQ_8BIT          0x100
    #define MHMAP_SIZE_MEMEXT             0x80000  /**< 512 KB Memext RAM (= MEMEXT_RAM_SIZE) */

#elif MZARCH == 700

    /**
     * @brief Identifikátor regionu MH pro MZ-700.
     *
     * MZ-700 má znakově orientovanou grafiku (CHAR + ATTR ve VRAM 4 KB),
     * stejně jako MZ-1500, ale bez PCG. Layout je MZ-1500 minus PCG banky.
     */
    typedef enum en_MHMAP_REGION
    {
        MHMAP_REGION_BUS = 0,         /**< logická CPU adresa 0000h-FFFFh */
        MHMAP_REGION_RAM,             /**< hlavní DRAM 64 KB */
        MHMAP_REGION_ROM,             /**< monitor ROM, 16 KB (lower 0000h + upper E000h-FFFFh ve fyzickém ROM blob) */
        MHMAP_REGION_CGROM,           /**< CG-ROM 4 KB */
        MHMAP_REGION_VRAM,            /**< znaková + atributová VRAM, 4 KB */
        MHMAP_REGION_IORQ_8BIT,       /**< 8-bit I/O porty 00h-FFh, 256 cells */
        MHMAP_REGION_MEMEXT,          /**< Memext RAM, 512 KB (jen pokud připojen) */
        MHMAP_REGION__COUNT,          /**< sentinel */
    } en_MHMAP_REGION;

    #define MHMAP_SIZE_BUS                0x10000
    #define MHMAP_SIZE_RAM                0x10000
    #define MHMAP_SIZE_ROM                0x4000   /**< 16 KB - celá ROM */
    #define MHMAP_SIZE_CGROM              0x1000   /**< 4 KB */
    #define MHMAP_SIZE_VRAM               0x1000   /**< 4 KB - znaková + atributová */
    #define MHMAP_SIZE_IORQ_8BIT          0x100
    #define MHMAP_SIZE_MEMEXT             0x80000  /**< 512 KB Memext RAM (= MEMEXT_RAM_SIZE) */

#endif /* MZARCH == ... */


    /**
     * @brief Jedna MH cela - čtveřice čítačů přístupů.
     *
     * Velikost: 16 bajtů. Klasický CDL flag pro daný typ přístupu = (counter > 0).
     *
     * Poznámka k S a W exkluzivitě (V5): pro každý jednotlivý memory write
     * event se inkrementuje BUĎ @c w (= "ne-stack write"), NEBO @c s
     * (= zápis cílící do aktivního stack regionu), nikdy oba současně.
     * Rozhodnutí dělá hot path call site v @c memory.c na základě
     * @c stack_regions_classify_write. Pokud nejsou definované žádné
     * stack regiony, @c s zůstane 0 a chování je beze změny vůči pre-V5
     * (R/W/X) - žádná regrese.
     */
    typedef struct st_MHMAP_CELL
    {
        uint32_t r;                 /**< počet R (data read) přístupů */
        uint32_t w;                 /**< počet W (non-stack write) přístupů */
        uint32_t x;                 /**< počet X (execute - součást instrukce) přístupů */
        uint32_t s;                 /**< počet S (stack write, V5) přístupů */
    } st_MHMAP_CELL;


    /**
     * @brief Globální stav Memory Heatmap - všechny mapy najednou.
     *
     * Statická alokace v BSS. Layout je arch-specific, viz @ref en_MHMAP_REGION
     * pro daný MZARCH. Při startu BSS = 0.
     *
     * @invariant Při MH off jsou hodnoty zmrazené (recording se nevolá);
     *            hot path bez debuggeru i bez MH zůstává bit-identický.
     */
    typedef struct st_MHMAP
    {
        st_MHMAP_CELL bus            [ MHMAP_SIZE_BUS ];
        st_MHMAP_CELL ram            [ MHMAP_SIZE_RAM ];
#if MZARCH == 800
        st_MHMAP_CELL rom_lower      [ MHMAP_SIZE_ROM_LOWER ];
        st_MHMAP_CELL rom_cg         [ MHMAP_SIZE_ROM_CG ];
        st_MHMAP_CELL rom_upper      [ MHMAP_SIZE_ROM_UPPER ];

        /* === VRAM === */
        st_MHMAP_CELL vram           [ MHMAP_SIZE_VRAM ];
        st_MHMAP_CELL vram700_cg     [ MHMAP_SIZE_VRAM700_CG ];
        st_MHMAP_CELL vram700        [ MHMAP_SIZE_VRAM700 ];

        /* MZ-800 320x200@16 (HICOLOR) - všechny 4 plane */
        st_MHMAP_CELL vram800_320_hc_i   [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_hc_ii  [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_hc_iii [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_hc_iv  [ MHMAP_SIZE_VRAM800_320_PLANE ];

        /* MZ-800 320x200@4 banka A/B */
        st_MHMAP_CELL vram800_320_4a_i   [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_4a_ii  [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_4b_i   [ MHMAP_SIZE_VRAM800_320_PLANE ];
        st_MHMAP_CELL vram800_320_4b_ii  [ MHMAP_SIZE_VRAM800_320_PLANE ];

        /* MZ-800 640x200@4 - 2 logické plane */
        st_MHMAP_CELL vram800_640_4_i    [ MHMAP_SIZE_VRAM800_640_PLANE ];
        st_MHMAP_CELL vram800_640_4_ii   [ MHMAP_SIZE_VRAM800_640_PLANE ];

        /* MZ-800 640x200@2 banka A/B */
        st_MHMAP_CELL vram800_640_2a     [ MHMAP_SIZE_VRAM800_640_PLANE ];
        st_MHMAP_CELL vram800_640_2b     [ MHMAP_SIZE_VRAM800_640_PLANE ];

        st_MHMAP_CELL iorq_8bit      [ MHMAP_SIZE_IORQ_8BIT ];
        st_MHMAP_CELL iorq_gdg       [ MHMAP_SIZE_IORQ_GDG ];
        st_MHMAP_CELL memext         [ MHMAP_SIZE_MEMEXT ];
#elif MZARCH == 1500
        st_MHMAP_CELL rom            [ MHMAP_SIZE_ROM ];
        st_MHMAP_CELL cgrom          [ MHMAP_SIZE_CGROM ];
        st_MHMAP_CELL vram           [ MHMAP_SIZE_VRAM ];
        st_MHMAP_CELL pcg_1          [ MHMAP_SIZE_PCG_BANK ];
        st_MHMAP_CELL pcg_2          [ MHMAP_SIZE_PCG_BANK ];
        st_MHMAP_CELL pcg_3          [ MHMAP_SIZE_PCG_BANK ];
        st_MHMAP_CELL iorq_8bit      [ MHMAP_SIZE_IORQ_8BIT ];
        st_MHMAP_CELL memext         [ MHMAP_SIZE_MEMEXT ];
#elif MZARCH == 700
        st_MHMAP_CELL rom            [ MHMAP_SIZE_ROM ];
        st_MHMAP_CELL cgrom          [ MHMAP_SIZE_CGROM ];
        st_MHMAP_CELL vram           [ MHMAP_SIZE_VRAM ];
        st_MHMAP_CELL iorq_8bit      [ MHMAP_SIZE_IORQ_8BIT ];
        st_MHMAP_CELL memext         [ MHMAP_SIZE_MEMEXT ];
#endif
    } st_MHMAP;


    extern st_MHMAP g_mhmap;


    /**
     * @brief Pomocný "out-of-band" parametr pro VRAM read hooky (jen MZ-800).
     *
     * Na MZ-800 se VRAM read obsluhuje uvnitř @c vramctrl_mz800_memop_read_byte_internal,
     * která dostane jen sběrnicovou adresu - nemá info o tom, zda jde o instrukci
     * (X) nebo data read (R). Memory callback (@c memory_read_with_logging_cb)
     * tu informaci má a nastaví ji do této proměnné před voláním read sync.
     *
     * Default: @c MHMAP_ACCESS_R.
     *
     * Na MZ-1500 se nepoužívá (VRAM/PCG access se klasifikuje rovnou v resolveru).
     */
    extern en_MHMAP_ACCESS g_mhmap_pending_read_kind;


    /**
     * @brief Příznak, že aktuální memory/IORQ access prochází CPU debug callbackem.
     *
     * Sdílený flag pro debug/trace subsystémy - rozliší přístupy iniciované
     * běžící Z80 instrukcí (= jdou skrz @c memory_*_with_logging_cb nebo
     * @c port_*_with_logging_cb) od přístupů iniciovaných mimo CPU emulační
     * smyčku (nahrávání MZF přes @c memory_load_block, čtení paměti z debug
     * memory browseru přes @c memory_read_byte, atd.). Pro mimo-CPU přístupy
     * nesmí trace/MHMAP recording zaznamenávat - nejsou to reálné CPU eventy.
     *
     * Použití:
     *  - MHMAP recording v @c vramctrl_mz800_memop_*_byte filtruje jen CPU cestu.
     *  - trace-suite @c iorqlog používá flag k filtraci @c IORQLOG_EVENT_MREQ_MAPPED
     *    emitů uvnitř @c memory_internal_*_rom_e000_*_sync (E000-E008 v MZ-700
     *    mode s namapovanou horní ROM) - aby logoval jen reálná čtení/zápisy
     *    z CPU instrukce, ne pomocná interní volání.
     *
     * Lifecycle: nastavují ho @c memory_*_with_logging_cb a
     * @c port_*_with_logging_cb na 1 před voláním další vrstvy a po návratu
     * vrací zpět na 0. Funkční jen v jednovláknové emulační smyčce.
     *
     * Historie: dříve @c g_mhmap_in_cpu_path - přejmenováno (trace-suite branch)
     * při zobecnění sémantiky pro více debug/trace subsystémů.
     */
    extern unsigned g_dbg_in_cpu_path;


/* === Public API === */

    /**
     * @brief Inicializace MH modulu - vynuluje všechny counters.
     */
    extern void mhmap_init ( void );


    /**
     * @brief Reset všech counterů na nulu.
     *
     * @pre Volat jen z emu vlákna (drain fronty dbgapi, akce BP) nebo
     *      v jednovláknovém kontextu (init, testy). Emu vlákno do
     *      @ref g_mhmap souběžně inkrementuje countery; UI vlákno reset
     *      posílá jako DBGAPI_CMD_CDL_RESET (dbg_ui_mhmap_reset).
     * @post Všechny mapy v @ref g_mhmap jsou vynulovány.
     */
    extern void mhmap_reset ( void );


    /**
     * @brief Nastavit režim MH recording.
     *
     * Nastaví @c g_debugger.mhmap_mode a triggeruje swap CPU callbacků
     * (přes @c mzarch_platform_fn_debugger_state_changed) - pokud změna režimu
     * vyžaduje přechod mezi rychlou a pomalou cestou.
     *
     * @param mode @ref en_DEBUGGER_MHMAP_MODE - OFF / WITH_WINDOW / ALWAYS
     *
     * @pre Volat jen z emu vlákna (dbgapi CDL_START / CDL_STOP, akce BP)
     *      nebo v jednovláknovém kontextu. Swap callbacků z UI vlákna by
     *      běžel souběžně s CPU smyčkou; UI vlákno proto zapíše jen
     *      @c g_debugger.mhmap_mode a přepočet deleguje přes
     *      DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE (dbg_ui_mhmap_set_mode).
     */
    extern void mhmap_set_mode ( en_DEBUGGER_MHMAP_MODE mode );


    /**
     * @brief Vynulovat countery jednoho regionu z tabulky exportních regionů.
     *
     * Region se vybírá indexem do pole z @ref mhmap_get_export_regions
     * (stejné pořadí jako taby okna Memory Heatmap). Ostatní regiony
     * zůstávají beze změny.
     *
     * @param region_index Index regionu (0 .. out_count - 1).
     * @return true při úspěchu, false pokud je index mimo rozsah (nic se
     *         nemění).
     *
     * @pre Volat jen z emu vlákna nebo v jednovláknovém kontextu (viz
     *      @ref mhmap_reset). UI vlákno posílá DBGAPI_CMD_MHMAP_RESET_REGION
     *      (dbg_ui_mhmap_reset_region).
     * @post Při true jsou všechny čtyři countery (R/W/X/S) každé buňky
     *       vybraného regionu nulové; ostatní regiony beze změny. Při false
     *       se @ref g_mhmap nemění.
     */
    extern bool mhmap_reset_region ( size_t region_index );


    /**
     * @brief Operace pro @ref mhmap_merge.
     *
     * Hodnoty se přenášejí číselně v st_DBGAPI_MHMAP_MERGE_PARAM.op,
     * proto se nesmí přečíslovat.
     */
    typedef enum en_MHMAP_MERGE_OP
    {
        MHMAP_MERGE_ADD = 0,    /**< live += src, saturace na UINT32_MAX */
        MHMAP_MERGE_SUB = 1,    /**< live -= src, při podtečení 0 */
    } en_MHMAP_MERGE_OP;


    /**
     * @brief Po buňkách přičíst / odečíst jinou mapu k živým counterům.
     *
     * Pracuje nad celým @ref g_mhmap jako nad polem @c uint32_t (všechny
     * čtyři countery R/W/X/S každé buňky všech regionů). Používá okno
     * Memory Heatmap pro Add / Sub importovaných dat.
     *
     * @param src Zdrojová mapa stejného layoutu (typicky importovaná data).
     *            Jen se čte; vlastní ji volající, musí žít do návratu.
     * @param op  @ref en_MHMAP_MERGE_OP.
     * @return true při úspěchu, false pro @p src == NULL nebo neznámé
     *         @p op (nic se nemění).
     *
     * @pre Volat jen z emu vlákna nebo v jednovláknovém kontextu (viz
     *      @ref mhmap_reset). UI vlákno posílá DBGAPI_CMD_MHMAP_MERGE
     *      (dbg_ui_mhmap_merge).
     * @post Při true platí pro každý counter k: ADD -> live[k] =
     *       min(live[k] + src[k], UINT32_MAX), SUB -> live[k] =
     *       max(live[k] - src[k], 0). Při false se @ref g_mhmap nemění.
     */
    extern bool mhmap_merge ( const st_MHMAP *src, en_MHMAP_MERGE_OP op );


    /**
     * @brief Zaznamenat jeden přístup do dané cely.
     *
     * Volá se z debug callbacků pouze pokud je aktivní MH
     * (@c TEST_DEBUGGER_MHMAP_ACTIVE podle @c g_debugger.mhmap_mode).
     *
     * @param region Identifikátor regionu (@ref en_MHMAP_REGION).
     * @param offset Offset v rámci regionu (0 .. velikost - 1).
     * @param access Druh přístupu (@ref en_MHMAP_ACCESS).
     *
     * @pre @p offset musí spadat do platného rozsahu daného regionu;
     *      hranice se NEKONTROLUJÍ za běhu.
     */
    extern void mhmap_inc ( en_MHMAP_REGION region, unsigned offset, en_MHMAP_ACCESS access );


#if MZARCH == 800
    /**
     * @brief Zaznamenat přístup do více fyzických VRAM bank (jen MZ-800).
     *
     * Inkrementuje cell ve @c vram regionu na offsetech podle bank bitmasky.
     * Pro každou bank set v masce se zapíše na příslušný offset
     * (MHMAP_VRAM_BANK1..4_OFFSET) + @p phy_offset.
     *
     * @param bank_mask  Bitmaska fyzických bank (bit 0 = VRAM1, bit 1 = VRAM2,
     *                   bit 2 = VRAM3, bit 3 = VRAM4). Konvence odpovídá
     *                   @c DEF_PLANE1..4 / WE bitmask ve vramctrl.
     * @param phy_offset Fyzický offset uvnitř bank (0 .. 0x1FFF).
     * @param access     Druh přístupu (R nebo W).
     */
    extern void mhmap_inc_vram_banks ( uint8_t bank_mask, unsigned phy_offset, en_MHMAP_ACCESS access );


    /**
     * @brief Zaznamenat MZ-800 VRAM zápis do fyzické vram + mode-specific mapy.
     *
     * Volá se z hooku @c vramctrl_mz800_memop_write_byte když je aktivní MH
     * a g_dbg_in_cpu_path = 1.
     *
     * @param we_mask     Hardware Write Enable bitmask (DEF_PLANE1..4) -
     *                    určuje fyzické VRAM banky, do kterých se zapsalo.
     *                    Použito pro recording do @c vram regionu.
     * @param wf_plane    Software intent z WF registru (DEF_PLANE1..4) -
     *                    určuje logické plane v daném modu. Použito pro
     *                    recording do mode-specific regionů.
     * @param bus_offset  Sběrnicový offset (= addr po & 0x3fff, rozsah
     *                    0x0000-0x3FFF). Index do mode-specific souborů.
     * @param phy_vram_addr Fyzický offset v plane (po >>1 v 640 modu
     *                    a hwscroll), rozsah 0x0000-0x1FFF.
     */
    extern void mhmap_record_mz800_vram_write ( uint8_t we_mask, uint8_t wf_plane,
                                                unsigned bus_offset, unsigned phy_vram_addr );


    /**
     * @brief Zaznamenat MZ-800 VRAM čtení do fyzické vram + mode-specific mapy.
     *
     * Volá se z hooku @c vramctrl_mz800_memop_read_byte_internal při aktivní MH
     * a g_dbg_in_cpu_path = 1.
     *
     * @param rf_plane    RF_PLANE bitmask (DEF_PLANE1..4) - určuje plane,
     *                    ze kterých se čte. Slouží jak pro fyzický (vram),
     *                    tak pro mode-specific recording.
     * @param bus_offset  Sběrnicový offset (= addr po & 0x3fff).
     * @param phy_vram_addr Fyzický offset v plane.
     * @param access      Druh přístupu (X = instrukce / R = data).
     */
    extern void mhmap_record_mz800_vram_read ( uint8_t rf_plane,
                                               unsigned bus_offset, unsigned phy_vram_addr,
                                               en_MHMAP_ACCESS access );
#endif


    /**
     * @brief Rozhodne, do kterého fyzického regionu náleží daná sběrnicová adresa.
     *
     * @param bus_addr      Logická CPU adresa.
     * @param[out] out_region Pokud @c MHMAP_RESOLVE_DIRECT, obsahuje fyzický region.
     * @param[out] out_offset Pokud @c MHMAP_RESOLVE_DIRECT, obsahuje offset.
     *
     * @return @c MHMAP_RESOLVE_DIRECT pro běžné ROM/RAM/VRAM/PCG oblasti,
     *         @c MHMAP_RESOLVE_SKIP pro VRAM s deferred klasifikací (MZ-800)
     *         nebo adresy bez fyzického cíle (MZ-1500 memory-mapped porty).
     */
    extern en_MHMAP_RESOLVE_RESULT mhmap_resolve_mem ( uint16_t bus_addr,
                                                 en_MHMAP_REGION *out_region,
                                                 unsigned *out_offset );


    /**
     * @brief Exportovat MH data jako sadu souborů.
     *
     * @p meta_path je plná cesta k meta.json souboru (např.
     * @c "./cdl-export/snap1.json"). Region soubory jsou ukládány do parent
     * adresáře s prefixem odvozeným z basename meta.json (bez @c .json):
     * @c snap1.json, @c snap1_bus.cdl, @c snap1_ram.cdl, atd.
     * Cell layout: 16 bajtů per cell = @c r, @c w, @c x, @c s (uint32 LE).
     * Pole @c s (V5) je stack write counter - viz @ref en_MHMAP_ACCESS.
     *
     * Pokud parent adresář neexistuje, vytvoří se přes
     * @c g_mkdir_with_parents.
     *
     * @param meta_path Cesta k cílovému meta.json souboru.
     * @return 0 při úspěchu, -1 při chybě.
     */
    extern int mhmap_export ( const char *meta_path );


    /**
     * @brief Popis jednoho regionu pro účely exportu / vizualizace.
     *
     * Veřejné API pro UI - umožňuje enumeraci dostupných regionů, jejich
     * názvy, velikosti, soubor a pointer na cell array v @ref g_mhmap.
     */
    typedef struct st_MHMAP_EXPORT_REGION
    {
        const char *name;        /**< technický název (do meta.json + filename root) */
        const char *label;       /**< krátký UI label pro tab (např. @c "vram320@16-I") */
        const char *filename;    /**< jméno souboru exportu */
        const void *buffer;      /**< pointer na pole st_MHMAP_CELL v @c g_mhmap */
        size_t size_bytes;       /**< velikost bufferu v bajtech */
        unsigned size_cells;     /**< počet cell */
    } st_MHMAP_EXPORT_REGION;


    /**
     * @brief Vrátit tabulku regionů pro aktuální architekturu.
     *
     * Tabulka je staticky alokovaná v modulu, pointery a size_bytes se
     * vyplňují za běhu (statický inicializátor neumí @c sizeof na
     * non-static datech). Volání tedy není thread-safe pro paralelní
     * přístup z více vláken.
     *
     * @param[out] out_count Počet platných položek (MZ-800 = 23, MZ-1500 = 10,
     *                       MZ-700 = 7; vždy včetně regionu memext).
     * @return Pointer na pole popisů regionů.
     */
    extern const st_MHMAP_EXPORT_REGION *mhmap_get_export_regions ( size_t *out_count );


    /*
     * F8 - mhmap_view_t API pro disassembler-window CDL integraci.
     *
     * Snapshot per-byte CDL flagů (X/R/W) v zadaném CPU rozsahu, který
     * pak @ref dasm_export_collect_labels (a navazující render) konzumuje
     * pro detekci data zón a cold-code komentářů.
     */

    /* Forward deklarace - úplná definice je v dasm_export.h. */
    typedef struct mhmap_view mhmap_view_t;


    /**
     * @brief Vytvoří snapshot CDL flagů pro daný CPU rozsah.
     *
     * Zkopíruje aktuální counter hodnoty z @c g_mhmap.bus na region
     * @p from..@p to do nově alokovaných per-byte flag bufferů.
     * Konverze counter -> flag: hodnota > 0 => 1, jinak 0 (klasický
     * CDL bitmap, FCEUX-style).
     *
     * Pohled je odpojený - další běh emulátoru countery nemění, takže
     * následující @c dasm_export_collect_labels volání není v race s
     * emu vláknem.
     *
     * Pokud je @c g_debugger.mhmap_mode == @c DEBUGGER_MHMAP_MODE_OFF,
     * countery jsou všechny 0 a vrácený snapshot má všechny flagy 0
     * (= žádná data zóna se nedetekuje). UI vrstva má sdělit uživateli
     * stav módu separátně - tato funkce mu pohled vyrobí vždy úspěšně.
     *
     * @param from Počáteční adresa rozsahu (inclusive).
     * @param to   Koncová adresa rozsahu (inclusive).
     *
     * @return Vlastněný @ref mhmap_view_t snapshot. Caller musí volat
     *         @ref mhmap_view_destroy. NULL při @p from > @p to nebo
     *         alokační chybě.
     *
     * @post Vrácený view drží 3 buffery velikosti @c (to - from + 1) B.
     */
    extern mhmap_view_t *mhmap_view_create_for_range ( uint16_t from, uint16_t to );


    /**
     * @brief Uvolní snapshot vytvořený přes @ref mhmap_view_create_for_range.
     *
     * NULL-safe.
     *
     * @param view Snapshot k uvolnění (nebo NULL).
     */
    extern void mhmap_view_destroy ( mhmap_view_t *view );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED && (MZARCH == 800 || MZARCH == 1500 || MZARCH == 700) */

#endif /* MHMAP_H */
