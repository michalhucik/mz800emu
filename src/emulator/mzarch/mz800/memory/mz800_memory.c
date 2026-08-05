/* 
 * File:   mz800_memory.c
 * Author: chaky
 *
 * Created on 15. června 2015, 17:33
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

#include "mzarch/mzarch_config.h"

#include <stdio.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"

#include "emulator.h"
#include "mzarch/mzarch.h"
#include "memory/memext.h"
#include "memory/memory.h"
#include "mz800_memory.h"
#include "memory/rom.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "debugger/bptmap.h"
#include "debugger/breakpoints.h"
#include "debugger/stack_regions.h"
#include "debugger/trace/iorqlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/io_history.h"
#include "debugger/trace/eventlog.h"
#include "debugger/io_activity.h"
#include "baseui/baseui.h"

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

st_MEMORY g_memory;

uint8_t *g_memoryVRAM = g_memory.VRAM;
uint8_t *g_memoryVRAM_I = g_memory.VRAM;
uint8_t *g_memoryVRAM_II = &g_memory.VRAM [ MEMORY_SIZE_VRAM_BANK ];
uint8_t *g_memoryVRAM_III = g_memory.EXVRAM;
uint8_t *g_memoryVRAM_IV = &g_memory.EXVRAM [ MEMORY_SIZE_VRAM_BANK ];


#define MEMORY_ROM_READ_BYTE                g_memory.ROM [ addr & 0x3fff ]

//#define MEMORY_RAM_READ_BYTE                g_memory.RAM [ addr ]
//#define MEMORY_RAM_WRITE_BYTE               g_memory.RAM [ addr ] = value;
#define MEMORY_RAM_READ_BYTE                ( g_memory.memram_read[( addr >> 12 )] ) [ (addr & 0x0fff) ]
#define MEMORY_RAM_WRITE_BYTE               ( g_memory.memram_write[( addr >> 12 )] ) [ (addr & 0x0fff) ] = value;

#define MEMORY_VRAM_MZ700_READ_BYTE_SYNC    vramctrl_mz700_memop_read_byte_sync ( addr & ~0xe000 )
#define MEMORY_VRAM_MZ700_WRITE_BYTE_SYNC   vramctrl_mz700_memop_write_byte_sync ( addr & ~0xe000, value )

#define MEMORY_VRAM_MZ700_READ_BYTE         vramctrl_mz700_memop_read_byte ( addr & ~0xe000 )
#define MEMORY_VRAM_MZ700_WRITE_BYTE        vramctrl_mz700_memop_write_byte ( addr & ~0xe000, value )

#define MEMORY_VRAM_MZ800_READ_BYTE_SYNC    vramctrl_mz800_memop_read_byte_sync ( addr & 0x3fff )
#define MEMORY_VRAM_MZ800_WRITE_BYTE_SYNC   vramctrl_mz800_memop_write_byte_sync ( addr & 0x3fff, value )
#define MEMORY_VRAM_MZ800_WRITE_BYTE        vramctrl_mz800_memop_write_byte ( addr & 0x3fff, value )

#define MEMORY_VRAM_MZ800_READ_BYTE         vramctrl_mz800_memop_read_byte ( addr & 0x3fff )


/*******************************************************************************
 *
 * Mapovani pameti ROM_0000, ROM_E000, CG_ROM, CG_RAM, VRAM a RAM
 * 
 ******************************************************************************/


/**
 * OUT 0xE0 - memory unmap ROM 0000 , CGROM
 */
static inline void memory_mmap_rom_bottom_off ( void ) {
    g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_0000 | MEMORY_MZ800_MAP_FLAG_ROM_1000 );
}


/**
 * OUT 0xE1 - memory unmap ROM E000, coz v MZ700 znamena i VRAM na D000
 */
static inline void memory_mmap_rom_upper_off ( void ) {
    g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_E000 );
}


/**
 * OUT 0xE2 - memory map ROM 0000
 */
static inline void memory_mmap_rom_0000_on ( void ) {
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_0000;
}


/**
 * OUT 0xE3 - memory map ROM E000, coz v MZ700 znamena i VRAM na D000
 */
static inline void memory_mmap_rom_upper_on ( void ) {
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
}


/**
 * OUT 0xE4 - memory map ROM 0000, ROM E000
 * MZ700: unmap CGROM, CGRAM
 * MZ800: map CGROM, VRAM
 */
static inline void memory_mmap_all_on ( void ) {
    g_memory.map |= ( MEMORY_MZ800_MAP_FLAG_ROM_0000 | MEMORY_MZ800_MAP_FLAG_ROM_1000 | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM | MEMORY_MZ800_MAP_FLAG_ROM_E000 );
    if ( GDG_DMD_TEST_MODE700 ) {
        g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_1000 | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
    };
}


/**
 * IN 0xE0 - memory map CG-ROM, CG-RAM, VRAM - podle mode
 * 
 * MZ-700: CG-ROM, CG-RAM
 * MZ-800: CG-ROM, VRAM
 */
static inline void memory_mmap_vram_on ( void ) {
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_1000 | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM;
}


/**
 * IN 0xE1 - memory umap CG-ROM, CG-RAM, VRAM - podle mode
 * 
 * MZ-700: CG-ROM, CG-RAM
 * MZ-800: CG-ROM, VRAM
 */
static inline void memory_mmap_vram_off ( void ) {
    g_memory.map &= ~( MEMORY_MZ800_MAP_FLAG_ROM_1000 | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM );
}


/**
 * Mapovani pameti pres IORQ - pwrite.
 * 
 * @param mmap_port
 */
void memory_map_pwrite ( uint8_t mmap_port, uint8_t value ) {
    (void)value; /* MZ-800 nepotrebuje value */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat banking switch (logicky GDG, fyzicky
     * v memory.c). Sub-event = low byte portu (E0..E6). */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        if ( mmap_port >= 0xE0 && mmap_port <= 0xE6 ) {
            uint8_t payload[ 6 ] = {
                mmap_port, value, 0, 0, 0, 0
            };
            hwlog_record ( HWLOG_CHIP_GDG_BANKING, mmap_port, payload );
        }
    }
    /* HWE: mmio:bank_switch a mmio:mode_change vyřazené (= IORQ_W na E0-E6
     * porty je expressivnější + zachová value bandwidth). */
#endif

    switch ( mmap_port ) {
        case MMAP_MZ800_PWRITE_E0:
            memory_mmap_rom_bottom_off ( );
            break;

        case MMAP_MZ800_PWRITE_E1:
            memory_mmap_rom_upper_off ( );
            break;

        case MMAP_MZ800_PWRITE_E2:
            memory_mmap_rom_0000_on ( );
            break;

        case MMAP_MZ800_PWRITE_E3:
            memory_mmap_rom_upper_on ( );
            break;

        case MMAP_MZ800_PWRITE_E4:
            memory_mmap_all_on ( );
            break;

        case MMAP_MZ800_PWRITE_E5:
            /* Aktivace "Prohibited" banking mode. Read $E009-$FFFF nyni vraci
             * 0x1A shadow byte (= empiricky overeno na realnem HW). Persistuje
             * pres OUT E0/E1/E2/E3/E4 a pres DMD bit 3 switch (= per-architektura
             * memory state, drzen v g_memory.map flag bit 4).
             * Reset: jen OUT E6, memory_reset / gdg_reset. */
            g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            break;

        case MMAP_MZ800_PWRITE_E6:
            /* Deaktivace "Prohibited" banking mode (Return). Banking se vrati
             * na normalni mapping per ROM_E000 / DMD mode. */
            g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
            break;
    };

#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /* Banking switch zmenil g_memory.map -> prepocti fast-path tabulku. */
    mz800_ram_fastpath_rebuild ( );
#endif
}


/**
 * Mapovani pameti pres IORQ - pread
 * 
 * @param mmap_port
 */
void memory_map_pread ( uint8_t mmap_port ) {
    switch ( mmap_port ) {
        case MMAP_MZ800_PREAD_E0:
            memory_mmap_vram_on ( );
            break;

        case MMAP_MZ800_PREAD_E1:
            memory_mmap_vram_off ( );
            break;
    };

#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /* IN E0/E1 zmenil g_memory.map (VRAM on/off) -> prepocti fast-path. */
    mz800_ram_fastpath_rebuild ( );
#endif
}


/*******************************************************************************
 *
 * Inicializace pameti RAM, MEMEXT, VRAM a EXVRAM
 * 
 ******************************************************************************/

void memory_reconnect_ram ( void ) {
    if ( MEMEXT_TEST_CONNECTED ) {
        int i;
        for ( i = 0; i < MEMORY_MEMRAM_POINTS; i++ ) {
            g_memory.memram_read[i] = memext_get_ram_read_pointer_by_addr_point ( i );
            g_memory.memram_write[i] = memext_get_ram_write_pointer_by_addr_point ( i );
        };
    } else {
        int i;
        for ( i = 0; i < MEMORY_MEMRAM_POINTS; i++ ) {
            g_memory.memram_read[i] = &g_memory.RAM[( i << 12 )];
            g_memory.memram_write[i] = &g_memory.RAM[( i << 12 )];
        };
    };

#ifdef MZ800EMU_CFG_RAM_FASTPATH
    mz800_ram_fastpath_rebuild ( );
#endif
}


#ifdef MZ800EMU_CFG_RAM_FASTPATH
/**
 * @brief Prepocet RAM-access fast-path page-table (E1, KROK 1 navrhu D3).
 *
 * Pro kazdou ze 16 stranek (4 KiB) urci podle aktualniho mapovaciho stavu
 * (memmap_query), zda je to cista DRAM. Pokud ano, do read/write fast-path
 * tabulky zapise bazi banku (z g_memory.memram_read/write, ktere uz reflektuji
 * pripadny memext banking). Pokud ne (ROM/CG-ROM/VRAM/CGRAM/mapped-ports/
 * PROHIBITED), zapise NULL = "NEEDS_CALLBACK" -> jadro spadne na memory_*_cb
 * (presny GDG sync, regDBUS_latch, CDL).
 *
 * Volat pri KAZDE zmene mapovani (banking switch OUT E0-E6, DMD switch 700/800)
 * a pri zmene read/write callbacku (logging on/off) - viz
 * mz800_ram_fastpath_resync. Cold path (vzacne vuci memory pristupum), takze
 * cena O(16) je amortizovana.
 *
 * Gating: tabulka se napln vzdy, ale fast-path je v jadre AKTIVNI jen pokud je
 * nainstalovan obycejny read callback (memory_read_cb). Pri logging callbacku
 * (debugger/CDL: memory_read_with_logging_cb) ma read vedlejsi efekty (X/R
 * klasifikace, history) ktere fast-path neumi -> enabled=false = baseline.
 *
 * @pre g_mzarch_main.cpu != NULL (z80_create probehl).
 * @post Fast-path tabulky v cpu-> aktualni vuci g_memory.map a regDMD.
 * @note Cista RAM write fast-path se povoluje JEN pro KIND_RAM stranky -
 *       u ROM stranek je zapis do RAM "pod ROM" potlaceny (write makro nic
 *       neudela), takze write fast-path tam NESMI byt aktivni.
 */
void mz800_ram_fastpath_rebuild ( void ) {

    if ( g_mzarch_main.cpu == NULL ) return;

    uint8_t *read_table[16];
    uint8_t *write_table[16];

    int i;
    for ( i = 0; i < 16; i++ ) {
        if ( memmap_query ( (uint8_t) i ) == MEMMAP_KIND_RAM ) {
            read_table[i]  = g_memory.memram_read[i];
            write_table[i] = g_memory.memram_write[i];
        } else {
            read_table[i]  = NULL;
            write_table[i] = NULL;
        };
    };

    /*
     * Gating: fast-path aktivni jen pro cisty memory_read_cb. Pri jakemkoli
     * jinem read callbacku (logging/CDL nebo budouci diff-verify) zustava
     * plny callback (enabled=false), aby se nezravala bit-identita.
     */
    bool enabled = ( g_mzarch_main.cpu->mread_cb == memory_read_cb );

    z80_set_ram_fastpath ( g_mzarch_main.cpu, read_table, write_table,
                           &g_mzarch_main.regDBUS_latch, enabled );
}


/**
 * @brief Resync fast-path po zmene read/write callbacku (logging on/off).
 *
 * Tenky wrapper na mz800_ram_fastpath_rebuild - volat po z80_set_mread/
 * z80_set_mwrite, aby se prepocital gating (enabled). Tabulky se prepoctou
 * take (je to levne a bezpecne).
 *
 * @pre g_mzarch_main.cpu != NULL.
 */
void mz800_ram_fastpath_resync ( void ) {
    mz800_ram_fastpath_rebuild ( );
}
#endif /* MZ800EMU_CFG_RAM_FASTPATH */


/**
 * Inicializace obsahu DRAM podle toho jak jsem ji odpozoroval na svem MZ-800:
 * 
 * RAM:
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00
 * 
 * VRAM:
 * FF 00 FF 00 FF 00 FF 00 FF 00 FF 00 FF 00 FF 00
 * 
 * EXVRAM:
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF
 * 
 */
void memory_init ( void ) {

    uint32_t i;
    uint16_t *addr;

    for ( i = 0; i < 0xffff; i += 4 ) {
        addr = ( uint16_t* ) & g_memory.RAM [ i ];
        *addr++ = 0xffff;
        *addr = 0x0000;
    };

    for ( i = 0; i < MEMORY_SIZE_VRAM; i += 2 ) {
        addr = ( uint16_t* ) & g_memory.VRAM [ i ];
        *addr = 0x00ff;
    };

    for ( i = 0; i < MEMORY_SIZE_VRAM; i += 4 ) {
        addr = ( uint16_t* ) & g_memory.EXVRAM [ i ];
        if ( i & 0x80 ) {
            *addr++ = 0x0000;
            *addr = 0xffff;
        } else {
            *addr++ = 0xffff;
            *addr = 0x0000;
        };
    };

    rom_init ( );

    g_memory.map = 0;

    memext_init ( );
    memory_reconnect_ram ( );
}


#if 0

#define MEMORY_DUMP_VRAM_FILE "vram.dat"


void memory_write_vram ( void ) {
    FILE *fp;

    if ( !( fp = ui_utils_file_open ( MEMORY_DUMP_VRAM_FILE, "wb" ) ) ) {
        baseui_error ( "Can't open file '%s': %s", MEMORY_DUMP_VRAM_FILE, strerror ( errno ) );
        return;
    };

    if ( sizeof (g_memory.VRAM ) != ui_utils_file_write ( &g_memory.VRAM, 1, sizeof (g_memory.VRAM ), fp ) ) {
        baseui_error ( "Can't write to file '%s': %s", MEMORY_DUMP_VRAM_FILE, strerror ( errno ) );
    };

    if ( sizeof (g_memory.EXVRAM ) != ui_utils_file_write ( &g_memory.EXVRAM, 1, sizeof (g_memory.VRAM ), fp ) ) {
        baseui_error ( "Can't write to file '%s': %s", MEMORY_DUMP_VRAM_FILE, strerror ( errno ) );
    };

    fclose ( fp );
}

#endif


void memory_reset ( void ) {
    g_memory.map = MEMORY_MZ800_MAP_FLAG_ROM_0000 | MEMORY_MZ800_MAP_FLAG_ROM_1000 | MEMORY_MZ800_MAP_FLAG_ROM_E000;

    memext_reset ( );
    memory_reconnect_ram ( );
}


/*******************************************************************************
 *
 * Cteni z aktualne mapovane pameti
 * 
 ******************************************************************************/

/**
 * Makra pro cteni z prislusne casti pameti a nasledny return - v zavislosti na mapovani.
 * 
 * Pokud je precteno, tak makro provede return pri kterem vrati prectenou hodnotu, 
 * jinak se pokracuje a nasleduje dalsi radek z volajici funkce.
 * 
 * a = addr >> 12
 * 
 * @param addr
 * @return
 */
#define memory_internal_read_0000_0fff(a) { if ( 0x00 == a ) { if ( MEMORY_MZ800_MAP_TEST_ROM_0000 ) return MEMORY_ROM_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_1000_1fff(a) { if ( 0x01 == a ) { if ( MEMORY_MZ800_MAP_TEST_ROM_1000 ) return MEMORY_ROM_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_8000_9fff_sync(a) { if ( ( 0x08 == a) || ( 0x09 == a) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 ) return MEMORY_VRAM_MZ800_READ_BYTE_SYNC; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_8000_9fff(a) { if ( ( 0x08 == a) || ( 0x09 == a) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 ) return MEMORY_VRAM_MZ800_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_a000_bfff_sync(a) { if ( ( 0x0a == a) || ( 0x0b == a) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 ) return MEMORY_VRAM_MZ800_READ_BYTE_SYNC; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_a000_bfff(a) { if ( ( 0x0a == a) || ( 0x0b == a) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 ) return MEMORY_VRAM_MZ800_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_c000_cfff_sync(a) { if ( 0x0c == a ) { if ( MEMORY_MZ800_MAP_TEST_CGRAM ) return MEMORY_VRAM_MZ700_READ_BYTE_SYNC; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_c000_cfff(a) { if ( 0x0c == a ) { if ( MEMORY_MZ800_MAP_TEST_CGRAM ) return MEMORY_VRAM_MZ700_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_d000_dfff_sync(a) { if ( 0x0d == a ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) return MEMORY_VRAM_MZ700_READ_BYTE_SYNC; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_d000_dfff(a) { if ( 0x0d == a ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) return MEMORY_VRAM_MZ700_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
/* Pozn. k bankingu $E000-$FFFF na MZ-800 (per VYHODNOCENI.md, banking-e800,
 * verze v0.5 testu):
 *   - Clear stav (! ROM_E000, NE Prohibited): cele $E000-$FFFF = DRAM
 *     (zadny mapped ports overlay v clear stavu).
 *   - Prohibited stav (po OUT E5, persistuje pres OUT E0-E4 a DMD switch):
 *     cele $E000-$FFFF = 0x1A shadow byte (vcetne $E000-$E008 mapped ports area).
 *     Empiricky overeno v0.5 T4. Rusi jen OUT E6 nebo gdg_reset.
 *   - K3 (ROM_E000 set, NE Prohibited):
 *     - 700 mode (DMD bit 3 = 1):
 *         $E000-$E008 = mapped ports (PIO/CTC/GDG)
 *         $E009-$E00F = 0x1A shadow byte (HW vraci natvrdo 0x1A nezavisle na
 *                       ROM image obsahu - hardcoded pro shodu s realnym HW;
 *                       empiricky overeno v0.5 T0/T1/T2)
 *         $E010-$FFFF = ROM
 *     - 800 native (DMD bit 3 = 0): mapped ports area cela "off"
 *         $E000-$E00F = 0xFF (empiricky overeno v0.5 T3 + v0.4 T6)
 *         $E010-$FFFF = ROM */
#define memory_internal_read_e000_efff_sync(a) { if ( 0x0e == a ) { if ( MEMORY_MZ800_MAP_TEST_PROHIBITED ) return 0x1A; if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) return memory_internal_read_rom_e000_efff_sync ( addr ); return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_e000_efff(a) { if ( 0x0e == a ) { if ( MEMORY_MZ800_MAP_TEST_PROHIBITED ) return 0x1A; if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) return memory_internal_read_rom_e000_efff ( addr ); return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_f000_ffff(a) { if ( 0x0f == a ) { if ( MEMORY_MZ800_MAP_TEST_PROHIBITED ) return 0x1A; if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) return MEMORY_ROM_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }


/**
 * Synchronizovane cteni z 0xe000 - 0xefff
 * 
 * @param addr
 * @return 
 */
static inline uint8_t memory_internal_read_rom_e000_efff_sync ( uint16_t addr ) {

    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni rom */
    if ( addr_low > 0x0f ) return MEMORY_ROM_READ_BYTE;

    /* $E000-$E00F oblast (NE Prohibited - tu uz vyresilo nadrazene makro):
     *   - 800 native (DMD bit 3 = 0): cela mapped ports area "off", vraci 0xFF
     *     (empiricky overeno v0.5 T3 i v0.4 T6).
     *   - 700 mode (DMD bit 3 = 1):
     *       $E000-$E008 = mapped ports (PIO/CTC/GDG)
     *       $E009-$E00F = 0x1A shadow byte (empiricky overeno HW v0.5 T0/T1/T2;
     *                     ROM image v emu ma na techto offsetech ruzne padding
     *                     bajty (FF/00/...) ale HW vraci konstantni 0x1A
     *                     bez ohledu na PIO state - takze tady vracime natvrdo
     *                     0x1A pro shodu s HW). */
    if ( ! GDG_DMD_TEST_MODE700 ) return 0xFF;

    /* 700 mode: $E009-$E00F shadow byte. */
    if ( addr_low > 0x08 ) return 0x1A;

    /* 700 mode: $E000-$E008 mapped ports. */
    uint8_t retval;

    /* cteni z E008 ( regDMD ) */
    if ( 0x08 == addr_low ) {
        mzarch_main_insideop_mreq_e00x ( );
        retval = gdg_read_dmd_status_memop ( );
    } else if ( addr_low & 0x04 ) {
        /* cteni z CTC8253 */

        /* Kontrol registr cist nelze */
        if ( 0x07 == addr_low ) {
            return g_mzarch_main.regDBUS_latch;
        };
        mzarch_main_insideop_mreq_e00x ( );
        retval = ctc8253_read_byte ( addr_low & 0x03 );
    } else {
        /* cteni z PIO8255 */
        mzarch_main_insideop_mreq_e00x ( );
        retval = pio8255_read ( addr & 0x03 );
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * trace-suite iorqlog: emit MREQ-mapped event pro CTC/PIO/GDG namapované
     * v MZ-700 mode na 0xE000-0xE008 (synchronní cesta - skutečný CPU read).
     * Emit jen v reálné CPU instrukční cestě (g_dbg_in_cpu_path), ne při
     * pomocných čteních z @c memory_load_block / debug browseru.
     * source_addr = aktuální PC (= odkud CPU instrukce přistupuje).
     */
    if ( TEST_TRACE_IORQLOG_ACTIVE && g_dbg_in_cpu_path ) {
        iorqlog_record ( IORQLOG_EVENT_MREQ_MAPPED, IORQLOG_DIR_IN,
                         g_mzarch_main.instruction_addr, addr, retval, 0u );
    }
#endif

    return retval;
}


/**
 * Cteni z 0xe000 - 0xefff - bez synchronizace
 * 
 * @param addr
 * @return 
 */
static inline uint8_t memory_internal_read_rom_e000_efff ( uint16_t addr ) {
    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni rom */
    if ( addr_low > 0x0f ) return MEMORY_ROM_READ_BYTE;

    /* $E000-$E00F (NE Prohibited - tu uz vyresilo nadrazene makro).
     * 800 native: cela oblast "off" (0xFF). 700 mode: $E000-$E008 mapped ports,
     * $E009-$E00F = 0x1A shadow byte. Detail v sync verzi. */
    if ( ! GDG_DMD_TEST_MODE700 ) return 0xFF;

    /* 700 mode: $E009-$E00F shadow byte. */
    if ( addr_low > 0x08 ) return 0x1A;

    /* 700 mode: $E000-$E008 mapped ports. */

    /* cteni z E008 ( regDMD ) */
    if ( 0x08 == addr_low ) return gdg_read_dmd_status_memop ( );


    /* cteni z CTC8253 */
    if ( addr_low & 0x04 ) {

        /* Kontrol registr cist nelze */
        if ( 0x07 == addr_low ) {
            return g_mzarch_main.regDBUS_latch;
        };
        // TODO: prozatim vracime 0x00
        return 0x00;
    };

    /* cteni z PIO8255 */
    return pio8255_read ( addr & 0x03 );
}


uint8_t memory_internal_read_sync ( uint16_t addr ) {

    unsigned addr_high = addr >> 12;

    memory_internal_read_0000_0fff ( addr_high );
    memory_internal_read_1000_1fff ( addr_high );
    memory_internal_read_8000_9fff_sync ( addr_high );
    memory_internal_read_a000_bfff_sync ( addr_high );
    memory_internal_read_c000_cfff_sync ( addr_high );
    memory_internal_read_d000_dfff_sync ( addr_high );
    memory_internal_read_e000_efff_sync ( addr_high );
    memory_internal_read_f000_ffff ( addr_high );
    return MEMORY_RAM_READ_BYTE;
}


/**
 * Cteni z aktualne mapovane pameti se zachovanim synchronizace u VRAM a 0xe00x periferii.
 * 
 * @param cpu
 * @param addr
 * @param m1_state
 * @param user_data
 * @return 
 */
uint8_t memory_read_cb ( z80_t *cpu, uint16_t addr, int m1_state, void *user_data ) {
    ( void ) cpu;
    ( void ) m1_state;
    ( void ) user_data;
    uint8_t retval = memory_internal_read_sync ( addr );
    g_mzarch_main.regDBUS_latch = retval;
    return retval;
}



#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * Cteni z aktualne mapovane pameti se zachovanim synchronizace u VRAM a 0xe00x periferii.
 * + debugging = ukladani historie poslednich vykonanych bajtu
 * 
 * @param cpu
 * @param addr
 * @param m1_state
 * @param user_data
 * @return 
 */
uint8_t memory_read_with_logging_cb ( z80_t *cpu, uint16_t addr, int m1_state, void *user_data ) {
    ( void ) cpu;
    ( void ) user_data;

    /*
     * Klasifikace přístupu (X vs R) - musí proběhnout PŘED voláním
     * memory_internal_read_sync, protože pokud adresa směřuje do MZ-800 VRAM,
     * sync zavolá vramctrl_mz800_memop_read_byte_internal, která pro CDL
     * recording potřebuje znát kind (přečte si ho z g_mhmap_pending_read_kind).
     *
     * X = bajt právě dekódované instrukce (M1 fetch, prefix M1, nebo
     *     immediate operand). R = data read mimo instrukci.
     *
     * Pravidlo:
     *  1) is_m1_start (m1_state && addr == instruction_addr) = M1 první bajt.
     *  2) m1_state = M1 prefix opcode uvnitř téže instrukce (např. CB po DD).
     *  3) addr == instruction_addr + byte_position = sekvenční operand bajt.
     *  4) jinak = data read (např. LD A,(HL) čte z HL po M1, kde HL nesedí
     *     na instruction_addr+1).
     *
     * Bez podmínky (3) by single-bajt instrukce typu LD A,(HL) klasifikovaly
     * následující data read jako X (byte_position=1, < 4), což je chybně.
     *
     * g_debugger_history.byte_position musí být udržován NEZÁVISLE na stavu
     * CPU Instruction History (jinak při cpuhist=OFF + mhmap=ALWAYS by zůstal na 0).
     * Counter se aktualizuje vždy, cpuhist jen plní row[].byte[].
     */
    en_MHMAP_ACCESS access_kind;
    int is_m1_start = ( m1_state ) && ( addr == g_mzarch_main.instruction_addr );
    if ( is_m1_start ) {
        access_kind = MHMAP_ACCESS_X;
    } else if ( m1_state ) {
        /* M1 prefix opcode uvnitř téže instrukce (DD, FD, ED, CB; DD CB,
         * FD CB). instruction_addr stále drží první bajt instrukce. */
        access_kind = MHMAP_ACCESS_X;
    } else if ( g_debugger_history.byte_position < DEBUGGER_MAX_INSTR_BYTES
                && addr == (uint16_t)( g_mzarch_main.instruction_addr + g_debugger_history.byte_position ) ) {
        /* Sekvenční immediate operand bajt - addr navazuje na konec
         * instrukčního streamu. */
        access_kind = MHMAP_ACCESS_X;
    } else {
        access_kind = MHMAP_ACCESS_R;
    };
    g_mhmap_pending_read_kind = access_kind;

    /* Označit CPU debug cestu - VRAM hook (vramctrl_*) recordinguje jen pokud
     * je flag nastaven. Mimo CPU cestu (memory_load_block, debug memory browser)
     * flag zůstává 0 a hook se přeskočí. */
    g_dbg_in_cpu_path = 1;
    uint8_t retval = memory_internal_read_sync ( addr );
    g_dbg_in_cpu_path = 0;
    g_mzarch_main.regDBUS_latch = retval;

    /*
     * 2.4b MEM_R BP enforce hook - jen pro data reads (access_kind == R),
     * NE pro instruction fetch (X = M1 opcode + prefix + immediate operand).
     * Filtrace přes access_kind classification eliminuje fire pro každou
     * instrukci (= spam). Volá se PO memory_internal_read_sync, aby
     * ctx.Value = právě přečtený byte. Fast-skip via per_type_active flag
     * (zero overhead když žádný MEM_R BP není aktivní).
     */
    if ( access_kind == MHMAP_ACCESS_R
         && g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] ) {
        breakpoints_enforce_mem_r ( addr, retval );
    }

    /*
     * V1.5.E - I/O Ports panel MMIO event tracking.
     * Filter: jen data reads (= R kind), ne instruction fetch (X), na
     * rozsahu 0xE000-0xE008 (= MZ-700 mode mirror PIO/CTC/GDG v ROM space).
     * Gated stejne jako IORQ varianta pres g_io_window_tracking_active
     * (= zero overhead pri zavrenem panelu). Per-event je addr ulozeno
     * jako 16-bit MMIO adresa (nikoli BC reg z IORQ).
     */
    if ( access_kind == MHMAP_ACCESS_R
         && addr >= 0xE000u && addr <= 0xE008u
         && g_io_window_tracking_active ) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit ( addr, retval,
                                  true /* is_in / is_read */, frame_n );
        io_history_record_mem ( true /* is_read */, addr, retval,
                                 g_mzarch_main.instruction_addr,
                                 frame_n,
                                 (uint16_t) g_gdg.beam_row,
                                 (uint16_t) VIDEO_GET_SCREEN_COL (
                                     g_gdg.total_elapsed.ticks ),
                                 g_mzarch_main.cpu->total_cycles );
    }

    /* event-viewer Vlna 1: paralelní fan-out MMIO_R do eventlog ringu.
     * Stejný filter (R kind + 0xE000-0xE008 rozsah) ale gated přes
     * eventlog active mask místo io_window panelu. */
    if ( access_kind == MHMAP_ACCESS_R
         && addr >= 0xE000u && addr <= 0xE008u
         && TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_MMIO_R ) ) ) {
        uint32_t pl = (uint32_t) addr | ( (uint32_t) retval << 16 );
        eventlog_record ( EVENTLOG_CAT_MMIO_R, 0,
                          g_mzarch_main.instruction_addr, pl );
    }

    /* Aktualizace byte_position counteru pro klasifikaci dalších čtení.
     * Resetuje se na M1 (start nové instrukce), inkrementuje pro každý
     * následující X-typ read v rámci téže instrukce. */
    if ( is_m1_start ) {
        g_debugger_history.byte_position = 1;
    } else if ( access_kind == MHMAP_ACCESS_X ) {
        g_debugger_history.byte_position++;
    };

    /* CPU Instruction History (cpuhist) - dle cpuhist_mode (default jen při aktivním debug okně). */
    if ( TEST_DEBUGGER_CPUHIST_ACTIVE ) {
        if ( is_m1_start ) {
            g_debugger_history.position++;
            int position = debugger_history_position ( g_debugger_history.position );
            g_debugger_history.row[position].addr = addr;
            g_debugger_history.row[position].byte[0] = retval;
        } else if ( access_kind == MHMAP_ACCESS_X ) {
            int position = debugger_history_position ( g_debugger_history.position );
            /* byte_position byl právě inkrementován v bloku výše;
             * cílový slot je tedy [byte_position - 1]. Pojistka proti OOB. */
            unsigned slot = g_debugger_history.byte_position - 1;
            if ( slot < DEBUGGER_MAX_INSTR_BYTES ) {
                g_debugger_history.row[position].byte[slot] = retval;
            };
        };
    };

    /* Memory Heatmap recording - jen pokud aktivní MH. Bus mapa se aktualizuje
     * vždy (logická CPU adresa). Fyzická mapa podle resolveru. */
    if ( TEST_DEBUGGER_MHMAP_ACTIVE ) {
        mhmap_inc ( MHMAP_REGION_BUS, addr, access_kind );
        en_MHMAP_REGION region;
        unsigned offset;
        if ( mhmap_resolve_mem ( addr, &region, &offset ) == MHMAP_RESOLVE_DIRECT ) {
            mhmap_inc ( region, offset, access_kind );
            /* Pro MZ-700 VRAM oblasti (VRAM700_CG, VRAM700) také zaznamenat
             * fyzický access do vram regionu (Plane I = VRAM1 bank).
             * VRAM700_CG mapuje na fyzickou plane I offset 0x000-0xFFF,
             * VRAM700 na 0x1000-0x1FFF. */
            if ( region == MHMAP_REGION_VRAM700_CG ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + offset, access_kind );
            } else if ( region == MHMAP_REGION_VRAM700 ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + 0x1000 + offset, access_kind );
            } else if ( region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED ) {
                /* Memext recording - pokud RAM access prošel přes namapovaný
                 * Memext bank, log do flat 512 KB MEMEXT regionu navíc. */
                int32_t mx_off = memext_get_ram_offset_from_pointer (
                    g_memory.memram_read[ addr >> 12 ] );
                if ( mx_off >= 0 ) {
                    mhmap_inc ( MHMAP_REGION_MEMEXT,
                                (unsigned) mx_off + ( addr & 0x0fff ),
                                access_kind );
                };
            };
        };
    };

    return retval;
}
#endif


/**
 * Cteni z aktualne mapovane pameti - bez synchronizace.
 * 
 * @param addr
 * @return 
 */
uint8_t memory_read_byte ( uint16_t addr ) {
    unsigned addr_high = addr >> 12;
    memory_internal_read_0000_0fff ( addr_high );
    memory_internal_read_1000_1fff ( addr_high );
    memory_internal_read_8000_9fff ( addr_high );
    memory_internal_read_a000_bfff ( addr_high );
    memory_internal_read_c000_cfff ( addr_high );
    memory_internal_read_d000_dfff ( addr_high );
    memory_internal_read_e000_efff ( addr_high );
    memory_internal_read_f000_ffff ( addr_high );
    uint8_t value = MEMORY_RAM_READ_BYTE;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.2 MEM_R BP hook. Fast-skip via per_type_active flag - branch
     * predictor naučí "vždy false" v default stavu = zero impact. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] ) {
        breakpoints_enforce_mem_r ( addr, value );
    }
#endif
    return value;
}


/**
 * V1.6+ TODO 4.2 5a: debugger-safe variant memory_read_byte.
 *
 * Identicke chovani jako memory_read_byte ale bez MEM_R BP fire
 * side-effectu. Pouziva se v BP expression evaluatoru pro [addr]
 * deref (vs [addr]! ktery volá full memory_read_byte = with side-effect).
 *
 * @param addr 16-bit virtuální adresa CPU mapy.
 * @return Bajt přečtený z aktuálně namapované paměti.
 */
uint8_t memory_read_byte_no_se ( uint16_t addr ) {
    unsigned addr_high = addr >> 12;
    memory_internal_read_0000_0fff ( addr_high );
    memory_internal_read_1000_1fff ( addr_high );
    memory_internal_read_8000_9fff ( addr_high );
    memory_internal_read_a000_bfff ( addr_high );
    memory_internal_read_c000_cfff ( addr_high );
    memory_internal_read_d000_dfff ( addr_high );
    memory_internal_read_e000_efff ( addr_high );
    memory_internal_read_f000_ffff ( addr_high );
    return MEMORY_RAM_READ_BYTE;
}



/*******************************************************************************
 *
 * Zapis do aktualne mapovane pameti
 * 
 ******************************************************************************/


/**
 * Makra pro zapis do prislusne casti pameti a nasledny return - v zavislosti na mapovani.
 * 
 * Pokud je zapsano, tak makro provede return,
 * jinak se pokracuje a nasleduje dalsi radek z volajici funkce.
 * 
 * a = addr >> 12
 * 
 * @param addr
 * @param value
 */
#define memory_internal_write_0000_0fff(a) { if ( 0x00 == a ) { if ( ! MEMORY_MZ800_MAP_TEST_ROM_0000 ) MEMORY_RAM_WRITE_BYTE; return; } }
#define memory_internal_write_1000_1fff(a) { if ( 0x01 == a ) { if ( ! MEMORY_MZ800_MAP_TEST_ROM_1000 ) MEMORY_RAM_WRITE_BYTE; return; } }
#define memory_internal_write_2000_7fff(a) { if ( ( 0x02 <= a ) && ( 0x07 >= a ) ) { MEMORY_RAM_WRITE_BYTE; return; } }
#define memory_internal_write_8000_9fff_sync(a) { if ( ( 0x08 == a ) || ( 0x09 == a ) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 ) { MEMORY_VRAM_MZ800_WRITE_BYTE_SYNC; } else { MEMORY_RAM_WRITE_BYTE; }; return; } }
#define memory_internal_write_a000_bfff_sync(a) { if ( ( 0x0a == a ) || ( 0x0b == a ) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 ) { MEMORY_VRAM_MZ800_WRITE_BYTE_SYNC; } else { MEMORY_RAM_WRITE_BYTE; }; return; } }
#define memory_internal_write_8000_9fff(a) { if ( ( 0x08 == a ) || ( 0x09 == a ) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 ) { MEMORY_VRAM_MZ800_WRITE_BYTE; } else { MEMORY_RAM_WRITE_BYTE; }; return; } }
#define memory_internal_write_a000_bfff(a) { if ( ( 0x0a == a ) || ( 0x0b == a ) ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 ) { MEMORY_VRAM_MZ800_WRITE_BYTE; } else { MEMORY_RAM_WRITE_BYTE; }; return; } }
#define memory_internal_write_c000_cfff_sync(a) { if ( 0x0c == a ) { if ( MEMORY_MZ800_MAP_TEST_CGRAM ) { MEMORY_VRAM_MZ700_WRITE_BYTE_SYNC; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_c000_cfff(a) { if ( 0x0c == a ) { if ( MEMORY_MZ800_MAP_TEST_CGRAM ) { MEMORY_VRAM_MZ700_WRITE_BYTE; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_d000_dfff_sync(a) { if ( 0x0d == a ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) { MEMORY_VRAM_MZ700_WRITE_BYTE_SYNC; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_d000_dfff(a) { if ( 0x0d == a ) { if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) { MEMORY_VRAM_MZ700_WRITE_BYTE; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_e000_efff_sync(a) { if ( 0x0e == a ) { if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) { memory_internal_write_rom_e000_sync ( addr, value ); } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_e000_efff(a) { if ( 0x0e == a ) { if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) { memory_internal_write_rom_e000 ( addr, value ); } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_f000_ffff(a) { if ( 0x0f == a ) { if ( ! MEMORY_MZ800_MAP_TEST_ROM_E000 ) MEMORY_RAM_WRITE_BYTE; return; } }


static inline void memory_internal_write_rom_e000_sync ( uint16_t addr, uint8_t value ) {

    if ( addr > 0xe008 ) return;

    mzarch_main_insideop_mreq_e00x ( );

    if ( addr == 0xe008 ) {
        gdg_write_byte ( addr, value );
    } else if ( addr & 0x04 ) {
        ctc8253_write_byte ( addr & 0x03, value );
    } else {
        pio8255_write ( addr & 0x03, value );
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * trace-suite iorqlog: emit MREQ-mapped OUT event pro CTC/PIO/GDG na
     * 0xE000-0xE008 (MZ-700 mode + horní ROM). Emit jen pokud jdeme z reálné
     * CPU instrukční cesty (filtruje pomocná volání mimo CPU loop).
     * source_addr = PC instrukce, port_or_addr = E00x cílová adresa, value
     * = zapisovaný bajt.
     */
    if ( TEST_TRACE_IORQLOG_ACTIVE && g_dbg_in_cpu_path ) {
        iorqlog_record ( IORQLOG_EVENT_MREQ_MAPPED, IORQLOG_DIR_OUT,
                         g_mzarch_main.instruction_addr, addr, value, 0u );
    }
#endif
}


static inline void memory_internal_write_rom_e000 ( uint16_t addr, uint8_t value ) {

    if ( addr > 0xe008 ) return;

    if ( addr == 0xe008 ) {
        gdg_write_byte ( addr, value );
    } else if ( addr & 0x04 ) {
        ctc8253_write_byte ( addr & 0x03, value );
    } else {
        pio8255_write ( addr & 0x03, value );
    };
}


/**
 * Zapis do aktualne mapovane pameti s ohledem na synchronizaci.
 * 
 * @param cpu
 * @param addr
 * @param value
 * @param user_data
 */
void memory_write_cb ( z80_t *cpu, uint16_t addr, uint8_t value, void *user_data ) {
    ( void ) cpu;
    ( void ) user_data;
    unsigned addr_high = addr >> 12;

    memory_internal_write_0000_0fff ( addr_high );
    memory_internal_write_1000_1fff ( addr_high );
    memory_internal_write_2000_7fff ( addr_high );
    memory_internal_write_8000_9fff_sync ( addr_high );
    memory_internal_write_a000_bfff_sync ( addr_high );
    memory_internal_write_c000_cfff_sync ( addr_high );
    memory_internal_write_d000_dfff_sync ( addr_high );
    memory_internal_write_e000_efff_sync ( addr_high );
    memory_internal_write_f000_ffff ( addr_high );
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * Zapis do aktualne mapovane pameti se zachovanim synchronizace
 * + debugging = CDL recording (W).
 *
 * @param cpu
 * @param addr
 * @param value
 * @param user_data
 */
void memory_write_with_logging_cb ( z80_t *cpu, uint16_t addr, uint8_t value, void *user_data ) {
    /*
     * MEM_W BP enforce hook PŘED vlastním zápisem - condition může číst
     * "starou" hodnotu přes [addr] no-side-effect deref. Fast-skip via
     * per_type_active flag (zero overhead když žádný MEM_W BP není aktivní).
     * Tento callback je aktivní v debugger active mode (z80_set_mwrite),
     * takže pokrývá všechny CPU writes (LD (HL),A, LDIR, PUSH apod.).
     */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] ) {
        breakpoints_enforce_mem_w ( addr, value );
    }

    /*
     * V1.5.E - I/O Ports panel MMIO event tracking (write side).
     * Filter: rozsah 0xE000-0xE008 (= MZ-700 mode mirror PIO/CTC/GDG).
     * Gated pres g_io_window_tracking_active. Hook PRED memory_write_cb
     * (= konzistentni s MEM_W BP a MHmap, ktere take fire pred write,
     * aby UI videlo "starou" hodnotu pres [addr] no-side-effect deref;
     * hodnota `value` je nove zapisovana data).
     */
    if ( addr >= 0xE000u && addr <= 0xE008u
         && g_io_window_tracking_active ) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit ( addr, value,
                                  false /* is_in - jsme write */, frame_n );
        io_history_record_mem ( false /* is_read */, addr, value,
                                 g_mzarch_main.instruction_addr,
                                 frame_n,
                                 (uint16_t) g_gdg.beam_row,
                                 (uint16_t) VIDEO_GET_SCREEN_COL (
                                     g_gdg.total_elapsed.ticks ),
                                 g_mzarch_main.cpu->total_cycles );
    }

    /* event-viewer Vlna 1: paralelní fan-out MMIO_W do eventlog ringu. */
    if ( addr >= 0xE000u && addr <= 0xE008u
         && TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_MMIO_W ) ) ) {
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_MMIO_W, 0,
                          g_mzarch_main.instruction_addr, pl );
    }

    /*
     * Memory Heatmap recording (před vlastním zápisem - resolver musí vidět
     * current banking, který se ještě nezměnil tímto write callbackem;
     * banking změny dělá port write).
     *
     * V5: stack write klasifikace. Pokud jsou definované stack regiony
     * (g_stack_regions_active) a addr leží v <limit..base> některého z nich,
     * klasifikace = S, jinak W. Volba se aplikuje konzistentně na všech
     * mhmap_inc voláních v tomto bloku (BUS + region + případný MEMEXT
     * mirror). VRAM700_* mirroring zůstává W (= stack do VRAM by byl
     * pathologický, ale pro úplnost by takový write byl klasifikován S
     * jen na BUS úrovni - viz access proměnná).
     */
    if ( TEST_DEBUGGER_MHMAP_ACTIVE ) {
        en_MHMAP_ACCESS access = MHMAP_ACCESS_W;
        if ( g_stack_regions_active && stack_regions_classify_write ( addr ) ) {
            access = MHMAP_ACCESS_S;
        }
        mhmap_inc ( MHMAP_REGION_BUS, addr, access );
        en_MHMAP_REGION region;
        unsigned offset;
        if ( mhmap_resolve_mem ( addr, &region, &offset ) == MHMAP_RESOLVE_DIRECT ) {
            mhmap_inc ( region, offset, access );
            /* Pro MZ-700 VRAM oblasti zaznamenat i do fyzické vram (VRAM1 bank). */
            if ( region == MHMAP_REGION_VRAM700_CG ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + offset, access );
            } else if ( region == MHMAP_REGION_VRAM700 ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + 0x1000 + offset, access );
            } else if ( region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED ) {
                /* Memext write - pokud RAM access cílí na Memext bank. */
                int32_t mx_off = memext_get_ram_offset_from_pointer (
                    g_memory.memram_write[ addr >> 12 ] );
                if ( mx_off >= 0 ) {
                    mhmap_inc ( MHMAP_REGION_MEMEXT,
                                (unsigned) mx_off + ( addr & 0x0fff ),
                                access );
                };
            };
        };
    };

    /* Označit CPU debug cestu - VRAM hook ve vramctrl recordinguje jen pokud
     * je flag nastaven. memory_write_cb obsahuje makra s return, ale return
     * ukončí jen sub-call; my se vrátíme zpět a flag vypneme. */
    g_dbg_in_cpu_path = 1;
    memory_write_cb ( cpu, addr, value, user_data );
    g_dbg_in_cpu_path = 0;
}
#endif


/**
 * Zapis do aktualne mapovane pameti - bez synchronizace.
 * 
 * @param addr
 * @param value
 */
void memory_write_byte ( uint16_t addr, uint8_t value ) {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.2 MEM_W BP hook PŘED zápisem - condition může číst "starou" hodnotu
     * přes [addr] no-side-effect deref. Fast-skip via per_type_active flag. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] ) {
        breakpoints_enforce_mem_w ( addr, value );
    }
#endif
    unsigned addr_high = addr >> 12;
    memory_internal_write_0000_0fff ( addr_high );
    memory_internal_write_1000_1fff ( addr_high );
    memory_internal_write_2000_7fff ( addr_high );
    memory_internal_write_8000_9fff ( addr_high );
    memory_internal_write_a000_bfff ( addr_high );
    memory_internal_write_c000_cfff ( addr_high );
    memory_internal_write_d000_dfff ( addr_high );
    memory_internal_write_e000_efff ( addr_high );
    memory_internal_write_f000_ffff ( addr_high );
}


/**
 * Nacteni datoveho bloku do pameti.
 * 
 * @param data
 * @param addr
 * @param size
 * @param type - MEMORY_LOAD_MAPPED, MEMORY_LOAD_RAMONLY
 */
void memory_load_block ( uint8_t *data, uint16_t addr, uint16_t size, en_MEMORY_LOAD type ) {

    uint16_t src_addr = 0;

    while ( size ) {
        //uint8_t *dst = &g_memory.RAM[addr];
        uint8_t *dst = g_memory.memram_write[( addr ) >> 12] + ( addr & 0x0fff );
        uint8_t *src = &data[src_addr];
        uint32_t total_size = addr + size;
        uint32_t load_size;

        if ( type == MEMORY_LOAD_RAMONLY ) {
            uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
            load_size = ( total_size < limit ) ? size : ( limit - addr );
            memcpy ( dst, src, load_size );
        } else {
            if ( addr < 0x1000 ) {
                load_size = ( total_size < 0x1000 ) ? size : ( 0x1000 - addr );
                if ( !MEMORY_MZ800_MAP_TEST_ROM_0000 ) {
                    memcpy ( dst, src, load_size );
                };
            } else if ( addr < 0x2000 ) {
                load_size = ( total_size < 0x2000 ) ? size : ( 0x2000 - addr );
                if ( !MEMORY_MZ800_MAP_TEST_ROM_1000 ) {
                    memcpy ( dst, src, load_size );
                };
            } else if ( addr < 0x8000 ) {
                uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                load_size = ( total_size < limit ) ? size : ( limit - addr );
                memcpy ( dst, src, load_size );
            } else if ( addr < 0xa000 ) {
                load_size = ( total_size < 0xa000 ) ? size : ( 0xa000 - addr );
                if ( !MEMORY_MZ800_MAP_TEST_VRAM_8000 ) {
                    uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                    load_size = ( total_size < limit ) ? size : ( limit - addr );
                    memcpy ( dst, src, load_size );
                } else {
                    uint32_t i;
                    for ( i = 0; i < load_size; i++ ) {
                        vramctrl_mz800_memop_write_byte ( ( addr + i ) & 0x3fff, data[( src_addr + i )] );
                    };
                };
            } else if ( addr < 0xc000 ) {
                load_size = ( total_size < 0xc000 ) ? size : ( 0xc000 - addr );
                if ( !MEMORY_MZ800_MAP_TEST_VRAM_A000 ) {
                    uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                    load_size = ( total_size < limit ) ? size : ( limit - addr );
                    memcpy ( dst, src, load_size );
                } else {
                    uint32_t i;
                    for ( i = 0; i < load_size; i++ ) {
                        vramctrl_mz800_memop_write_byte ( ( addr + i ) & 0x3fff, data[( src_addr + i )] );
                    };
                };
            } else if ( addr < 0xd000 ) {
                load_size = ( total_size < 0xd000 ) ? size : ( 0xd000 - addr );
                if ( MEMORY_MZ800_MAP_TEST_CGRAM ) {
                    dst = &g_memoryVRAM_I[( addr & 0x0fff )];
                };
                memcpy ( dst, src, load_size );
            } else if ( addr < 0xe000 ) {
                load_size = ( total_size < 0xe000 ) ? size : ( 0xe000 - addr );
                if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) {
                    dst = &g_memoryVRAM_I[( 0x1000 | ( addr & 0x0fff ) )];
                };
                memcpy ( dst, src, load_size );
            } else {
                uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                load_size = ( total_size < limit ) ? size : ( limit - addr );
                if ( !MEMORY_MZ800_MAP_TEST_ROM_E000 ) {
                    memcpy ( dst, src, load_size );
                };
            };
        };

        size -= load_size;
        src_addr += load_size;
        addr += load_size;
    };
}


/*******************************************************************************
 *
 * Memory Map debug query (MZ-800)
 *
 * Vrací druh regionu pro 4 kB stránku z pohledu Z80. Reflektuje aktuální
 * banking stav (`g_memory.map`) a DMD mode (`g_gdg.regDMD`). Read-only,
 * side-effect free.
 *
 * Mapování přesně odpovídá makrům `memory_internal_read_*` v tomto souboru;
 * při změně banking logiky musí být aktualizováno současně.
 *
 ******************************************************************************/

en_MEMMAP_REGION_KIND memmap_query ( uint8_t addr_point )
{
    if ( addr_point > 0x0f ) return MEMMAP_KIND_RAM;

    int mz700_mode = GDG_DMD_TEST_MODE700 ? 1 : 0;
    int prohibited = MEMORY_MZ800_MAP_TEST_PROHIBITED ? 1 : 0;

    switch ( addr_point ) {
        case 0x00:
            /* $0000-$0FFF: Monitor ROM low (pokud bit ROM_0000 set), jinak RAM. */
            if ( MEMORY_MZ800_MAP_TEST_ROM_0000 ) return MEMMAP_KIND_ROM_LOW;
            return MEMMAP_KIND_RAM;

        case 0x01:
            /* $1000-$1FFF: V 800 native je ROM_1000 = CG-ROM (= druhá půlka
             * 8 KB monitor ROM image). V MZ-700 modu je tato oblast vždy
             * RAM (memory_mmap_all_on() ROM_1000 vyčistí). Zde test stavu. */
            if ( MEMORY_MZ800_MAP_TEST_ROM_1000 ) return MEMMAP_KIND_CGROM;
            return MEMMAP_KIND_RAM;

        case 0x08:
        case 0x09:
            /* $8000-$9FFF: VRAM I (8 KB) v 800 native modu pokud bit
             * CGRAM_VRAM set. V MZ-700 modu = vždy RAM. */
            if ( MEMORY_MZ800_MAP_TEST_VRAM_8000 ) return MEMMAP_KIND_VRAM_I;
            return MEMMAP_KIND_RAM;

        case 0x0a:
        case 0x0b:
            /* $A000-$BFFF: VRAM II (= rozšířená 8 KB) v 800 native modu
             * pokud SCRW640 + CGRAM_VRAM. Jinak RAM. */
            if ( MEMORY_MZ800_MAP_TEST_VRAM_A000 ) return MEMMAP_KIND_VRAM_II;
            return MEMMAP_KIND_RAM;

        case 0x0c:
            /* $C000-$CFFF: V MZ-700 modu CG-RAM (pokud CGRAM_VRAM bit set),
             * jinak RAM. V 800 native je RAM. */
            if ( MEMORY_MZ800_MAP_TEST_CGRAM ) return MEMMAP_KIND_CGRAM;
            return MEMMAP_KIND_RAM;

        case 0x0d:
            /* $D000-$DFFF: V MZ-700 modu znaková + atributová VRAM (pokud
             * ROM_E000 bit set). Jinak RAM. */
            if ( MEMORY_MZ800_MAP_TEST_VRAM_D000 ) return MEMMAP_KIND_VRAM_TEXT;
            return MEMMAP_KIND_RAM;

        case 0x0e:
            /* $E000-$EFFF: per banking-e800 v0.5 model.
             * - Prohibited (OUT E5 active): celé E000-EFFF vrací 0x1A shadow.
             * - K3 (ROM_E000 set, NE Prohibited):
             *     - 700 mode: $E000-$E008 = mapped ports. UI ale ukazuje
             *       celý 4 kB blok jako jednu kategorii - pojmem ho jako
             *       MAPPED_PORTS (= dominantní vlastnost stránky v 700
             *       modu, byť většina je ROM od $E010).
             *     - 800 native: $E000-$E00F off (0xFF), $E010-$EFFF ROM.
             *       UI: ROM_HIGH (= dominantní obsah stránky).
             * - Clear (! ROM_E000): celá stránka = DRAM. */
            if ( prohibited ) return MEMMAP_KIND_PROHIBITED;
            if ( !MEMORY_MZ800_MAP_TEST_ROM_E000 ) return MEMMAP_KIND_RAM;
            if ( mz700_mode ) return MEMMAP_KIND_MAPPED_PORTS;
            return MEMMAP_KIND_ROM_HIGH;

        case 0x0f:
            /* $F000-$FFFF: K3 -> ROM (Prohibited -> 0x1A); Clear -> RAM. */
            if ( prohibited ) return MEMMAP_KIND_PROHIBITED;
            if ( MEMORY_MZ800_MAP_TEST_ROM_E000 ) return MEMMAP_KIND_ROM_HIGH;
            return MEMMAP_KIND_RAM;

        default:
            /* Stránky 0x02-0x07 = vždy DRAM (ROM tam neexistuje). */
            return MEMMAP_KIND_RAM;
    };
}
