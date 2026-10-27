/*
 * File:   mz700_memory.c
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

#include <string.h>

#include "libs/cpu-z80/z80.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/ramlog_hook.h"
#endif

#include "mzarch/mzarch.h"
#include "memory/memext.h"
#include "memory/memory.h"
#include "mz700_memory.h"
#include "mzarch/mz700/devbus/mz700_devbus.h"
#include "hw-generic/devbus/devbus.h"
#include "memory/rom.h"
#include "mzarch/mz700/gdg/mz700_gdg.h"
#include "mzarch/mz700/gdg/mz700_vramctrl.h"
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
#include "debugger/io_activity.h"
#include "debugger/trace/eventlog.h"

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

st_MEMORY g_memory;
uint8_t *g_memoryVRAM = g_memory.VRAM;

/*
 * Makra pro pristup do pameti
 */
#define MEMORY_ROM_READ_BYTE                g_memory.ROM [ addr & 0x3fff ]

#define MEMORY_RAM_READ_BYTE                ( g_memory.memram_read[( addr >> 12 )] ) [ (addr & 0x0fff) ]
#define MEMORY_RAM_WRITE_BYTE               ( g_memory.memram_write[( addr >> 12 )] ) [ (addr & 0x0fff) ] = value;

/* MZ-700 ma jednoduchou 4 KB VRAM (0xD000-0xDFFF), bez bank/PCG bloku.
 * Maska 0x0fff prevadi sbernicovou adresu na offset uvnitr VRAM bufferu. */
#define MEMORY_VRAM_MZ700_READ_BYTE_SYNC    vramctrl_mz700_memop_read_byte_sync ( addr & 0x0fff )
#define MEMORY_VRAM_MZ700_WRITE_BYTE_SYNC   vramctrl_mz700_memop_write_byte_sync ( addr & 0x0fff, value )

#define MEMORY_VRAM_MZ700_READ_BYTE         vramctrl_mz700_memop_read_byte ( addr & 0x0fff )
#define MEMORY_VRAM_MZ700_WRITE_BYTE        vramctrl_mz700_memop_write_byte ( addr & 0x0fff, value )


/*******************************************************************************
 *
 * Mapovani MZ-700 pameti ROM_0000, ROM_E000 (VRAM_D000, porty E000, ROM E800)
 *
 ******************************************************************************/


/**
 * OUT 0xE0 - unmap ROM 0000
 */
static inline void memory_mmap_rom_bottom_off(void) {
    g_memory.map &= ~(MEMORY_MZ700_MAP_FLAG_ROM_0000);
}


/**
 * OUT 0xE1 - unmap ROM UPPER - VRAM D000, ROM E000 (porty)
 */
static inline void memory_mmap_rom_upper_off(void) {
    g_memory.map &= ~(MEMORY_MZ700_MAP_FLAG_ROM_E000);
}


/**
 * OUT 0xE2 - memory map ROM 0000
 */
static inline void memory_mmap_rom_bottom_on(void) {
    g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_0000;
}


/**
 * OUT 0xE3 - memory map ROM E000
 */
static inline void memory_mmap_rom_upper_on(void) {
    g_memory.map |= MEMORY_MZ700_MAP_FLAG_ROM_E000;
}


/**
 * OUT 0xE4 - memory map ROM 0000 + ROM E000, deaktivuje Prohibited.
 *
 * Plne nastavi default banking stav: ROM 0000 mapped, ROM E000 (vc. ports/VRAM
 * D000) mapped, Prohibited rezim zruseny.
 */
static inline void memory_mmap_all_on(void) {
    g_memory.map = (MEMORY_MZ700_MAP_FLAG_ROM_0000 | MEMORY_MZ700_MAP_FLAG_ROM_E000);
}

/**
 * OUT 0xE5 - aktivuje Prohibited mode.
 *
 * Po aktivaci Prohibited modu cteni z $E800-$FFFF vraci 0xFF (= ROM E800/code
 * a F000 monitor odpojeny). Ruseno OUT 0xE6 nebo memory_reset.
 */
static inline void memory_mmap_prohibited_on(void) {
    g_memory.map |= MEMORY_MZ700_MAP_FLAG_PROHIBITED;
}

/**
 * OUT 0xE6 - ukonci Prohibited mode.
 *
 * Vrati banking do stavu pred Prohibited (ROM E800/code zase mapped).
 */
static inline void memory_mmap_prohibited_off(void) {
    g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
}

/**
 * Mapovani pameti pres IORQ - pwrite.
 */
void memory_map_pwrite(uint8_t mmap_port, uint8_t value) {

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat banking switch (logicky GDG, fyzicky
     * v memory.c). Sub-event = low byte portu (E0..E6). */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        if ( mmap_port >= MMAP_MZ700_PWRITE_E0 && mmap_port <= MMAP_MZ700_PWRITE_E6 ) {
            uint8_t payload[ 6 ] = {
                mmap_port, value, 0, 0, 0, 0
            };
            hwlog_record ( HWLOG_CHIP_GDG_BANKING, mmap_port, payload );
        }
    }
    /* HWE: mmio:bank_switch a mmio:mode_change vyřazené (= IORQ_W na E0-E6
     * porty je expressivnější + zachová value bandwidth). */
#endif

    switch (mmap_port) {
        case MMAP_MZ700_PWRITE_E0:
            memory_mmap_rom_bottom_off ( );
            break;

        case MMAP_MZ700_PWRITE_E1:
            memory_mmap_rom_upper_off ( );
            break;

        case MMAP_MZ700_PWRITE_E2:
            memory_mmap_rom_bottom_on ( );
            break;

        case MMAP_MZ700_PWRITE_E3:
            memory_mmap_rom_upper_on ( );
            break;

        case MMAP_MZ700_PWRITE_E4:
            memory_mmap_all_on ( );
            break;

        case MMAP_MZ700_PWRITE_E5:
            memory_mmap_prohibited_on();
            break;

        case MMAP_MZ700_PWRITE_E6:
            memory_mmap_prohibited_off();
            break;
    };
}


/*******************************************************************************
 *
 * Inicializace pameti RAM, MEMEXT, VRAM
 *
 ******************************************************************************/

void memory_reconnect_ram(void) {
    if (MEMEXT_TEST_CONNECTED) {
        int i;
        for (i = 0; i < MEMORY_MEMRAM_POINTS; i++) {
            g_memory.memram_read[i] = memext_get_ram_read_pointer_by_addr_point(i);
            g_memory.memram_write[i] = memext_get_ram_write_pointer_by_addr_point(i);
        };
    } else {
        int i;
        for (i = 0; i < MEMORY_MEMRAM_POINTS; i++) {
            g_memory.memram_read[i] = &g_memory.RAM[(i << 12)];
            g_memory.memram_write[i] = &g_memory.RAM[(i << 12)];
        };
    };

}


/**
 * Inicializace obsahu DRAM podle toho jak jsem ji odpozoroval na svem MZ-800:
 * 
 * RAM:
 * FF FF 00 00 FF FF 00 00 FF FF 00 00 FF FF 00 00
 * 
 * VRAM:
 * FF 00 FF 00 FF 00 FF 00 FF 00 FF 00 FF 00 FF 00
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

    rom_init ( );

    g_memory.map = 0;

    memext_init ( );
    memory_reconnect_ram ( );
}


/**
 * Tepla inicializace pameti po resetu.
 *
 * Default banking po resetu: ROM 0000 + ROM E000 mapped, Prohibited rezim
 * zruseny. ROM E800 a F000 monitor jsou tedy viditelne (= NOT Prohibited).
 */
void memory_reset(void) {
    g_memory.map = MEMORY_MZ700_MAP_FLAG_ROM_0000 | MEMORY_MZ700_MAP_FLAG_ROM_E000;
    g_memory.map &= ~MEMORY_MZ700_MAP_FLAG_PROHIBITED;
    memext_reset();
    memory_reconnect_ram();
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
#define memory_internal_read_0000_0fff(a) { if ( 0x00 == a ) { if ( MEMORY_MZ700_MAP_TEST_ROM_0000 ) return MEMORY_ROM_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_d000_dfff_sync(a) { if ( 0x0d == a ) { if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) return MEMORY_VRAM_MZ700_READ_BYTE_SYNC; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_d000_dfff(a) { if ( 0x0d == a ) { if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) return MEMORY_VRAM_MZ700_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_e000_efff_sync(a) { if ( 0x0e == a ) { if ( MEMORY_MZ700_MAP_TEST_ROM_E000 ) return memory_internal_read_rom_e000_efff_sync ( addr ); return MEMORY_RAM_READ_BYTE; } }
#define memory_internal_read_e000_efff(a) { if ( 0x0e == a ) { if ( MEMORY_MZ700_MAP_TEST_ROM_E000 ) return memory_internal_read_rom_e000_efff ( addr ); return MEMORY_RAM_READ_BYTE; } }
/* $F000-$FFFF: monitor ROM (= horni cast 16K ROM image, addr & 0x3FFF).
 *   - Clear (! ROM_E000): DRAM
 *   - K3 (ROM_E000) + Prohibited: 0xFF (= ROM E800/F000 odpojeno)
 *   - K3 (ROM_E000) + ! Prohibited: ROM bajt
 */
#define memory_internal_read_f000_ffff(a) { if ( 0x0f == a ) { if ( ! MEMORY_MZ700_MAP_TEST_ROM_E000 ) return MEMORY_RAM_READ_BYTE; if ( MEMORY_MZ700_MAP_TEST_PROHIBITED ) return 0xff; return MEMORY_ROM_READ_BYTE; } }


/**
 * Synchronizovane cteni z 0xe000 - 0xefff
 * 
 * @param addr
 * @return 
 */
static inline uint8_t memory_internal_read_rom_e000_efff_sync ( uint16_t addr ) {

    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni rom ($E010-$EFFF):
     *   - Prohibited: 0xFF (= ROM odpojeno)
     *   - NOT Prohibited: ROM bajt
     */
    if ( addr_low > 0x0f ) {
        if ( MEMORY_MZ700_MAP_TEST_PROHIBITED ) return 0xff;
        return MEMORY_ROM_READ_BYTE;
    }

    /* cteni z E009 - E00F */
    if ( addr_low > 0x08 ) return g_mzarch_main.regDBUS_latch;

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
 * Bez vedlejšího efektu: mapované porty (8255, 8253, status GDG) se čtou
 * přes *_peek funkce čipů, které stav nemění (memory_read_byte_no_se,
 * memory_read_byte = debugger, Memory Browser, Watch, výrazy BP).
 *
 * @param addr
 * @return 
 */
static inline uint8_t memory_internal_read_rom_e000_efff ( uint16_t addr ) {
    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni rom ($E010-$EFFF):
     *   - Prohibited: 0xFF (= ROM odpojeno)
     *   - NOT Prohibited: ROM bajt
     */
    if ( addr_low > 0x0f ) {
        if ( MEMORY_MZ700_MAP_TEST_PROHIBITED ) return 0xff;
        return MEMORY_ROM_READ_BYTE;
    };

    /* cteni z E009 - E00F */
    if ( addr_low > 0x08 ) return g_mzarch_main.regDBUS_latch;

    /* cteni z E008 ( regDMD ) */
    if ( 0x08 == addr_low ) return gdg_peek_dmd_status_memop ( );


    /* cteni z CTC8253 */
    if ( addr_low & 0x04 ) {

        /* Kontrol registr cist nelze */
        if ( 0x07 == addr_low ) {
            return g_mzarch_main.regDBUS_latch;
        };
        return ctc8253_peek_byte ( addr_low & 0x03 );
    };

    /* cteni z PIO8255 */
    return pio8255_peek ( addr & 0x03 );
}


uint8_t memory_internal_read_sync ( uint16_t addr ) {
    unsigned addr_high = addr >> 12;
    memory_internal_read_0000_0fff ( addr_high );
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
     *  3) cpu->pc == addr + 1 = operand instrukce: jádro ho čte z PC
     *     (FETCH, rPC++), PC je v okamžiku čtení už za ním.
     *  4) jinak = data read (RD, PC neposouvá). I čtení hned za dosud
     *     načtenými bajty instrukce (POP ze SP = PC, LD A,(nn) s nn = PC)
     *     je data: cpu->pc == addr (N60, viz mz800_memory.c).
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
    } else if ( (uint16_t) cpu->pc == (uint16_t) ( addr + 1 ) ) {
        /* Operand instrukce (FETCH z PC, PC už posunuté). */
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

    /* event-viewer Vlna 1: paralelní fan-out MMIO_R do eventlog ringu. */
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
    /* Bajty instrukce tak, jak je CPU načetlo (cputrack, nezávisle na cpuhist). */
    if ( access_kind == MHMAP_ACCESS_X ) {
        debugger_insn_fetch_record ( is_m1_start, g_debugger_history.byte_position - 1, addr, retval );
    };

    /* CPU Instruction History (cpuhist) - dle cpuhist_mode (default jen při aktivním debug okně). */
    if ( TEST_DEBUGGER_CPUHIST_ACTIVE ) {
        if ( is_m1_start ) {
            g_debugger_history.position++;
            int position = debugger_history_position ( g_debugger_history.position );
            g_debugger_history.row[position].addr = addr;
            g_debugger_history.row[position].byte[0] = retval;
            debugger_history_capture_regs ( &g_debugger_history.row[position].regs, cpu );
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
#if MZARCH == 800
            /* MZ-800: MZ-700 mód má samostatné regiony VRAM700_CG/VRAM700,
             * plus fyzickou VRAM (Plane I = VRAM1 bank). Recording do obou. */
            if ( region == MHMAP_REGION_VRAM700_CG ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + offset, access_kind );
            } else if ( region == MHMAP_REGION_VRAM700 ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + 0x1000 + offset, access_kind );
            } else
#endif
            if ( region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED ) {
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
#define memory_internal_write_0000_0fff(a) { if ( 0x00 == a ) { if ( ! MEMORY_MZ700_MAP_TEST_ROM_0000 ) MEMORY_RAM_WRITE_BYTE; return; } }
#define memory_internal_write_1000_cfff(a) { if ( ( 0x01 <= a ) && ( 0x0c >= a ) ) { MEMORY_RAM_WRITE_BYTE; return; } }
#define memory_internal_write_d000_dfff_sync(a) { if ( 0x0d == a ) { if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) { MEMORY_VRAM_MZ700_WRITE_BYTE_SYNC; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_d000_dfff(a) { if ( 0x0d == a ) { if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) { MEMORY_VRAM_MZ700_WRITE_BYTE; } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_e000_efff_sync(a) { if ( 0x0e == a ) { if ( MEMORY_MZ700_MAP_TEST_ROM_E000 ) { memory_internal_write_rom_e000_sync ( addr, value ); } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_e000_efff(a) { if ( 0x0e == a ) { if ( MEMORY_MZ700_MAP_TEST_ROM_E000 ) { memory_internal_write_rom_e000 ( addr, value ); } else { MEMORY_RAM_WRITE_BYTE; } return; } }
#define memory_internal_write_f000_ffff(a) { if ( 0x0f == a ) { if ( ! MEMORY_MZ700_MAP_TEST_ROM_E000 ) MEMORY_RAM_WRITE_BYTE; return; } }


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
    memory_internal_write_1000_cfff ( addr_high );
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
     * Filter: 0xE000-0xE008 (PIO/CTC/GDG) + MMIO rozsahy připojených zařízení
     * devbus v 0xE000-0xE0FF (mz700_devbus_mmio_tracked(), bitmapa).
     * Gated pres g_io_window_tracking_active. Hook PRED memory_write_cb
     * (= konzistentni s MEM_W BP a MHmap, ktere take fire pred write,
     * aby UI videlo "starou" hodnotu pres [addr] no-side-effect deref;
     * hodnota `value` je nove zapisovana data).
     */
    if ( g_io_window_tracking_active && mz700_devbus_mmio_tracked ( addr ) ) {
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
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_MMIO_W ) )
         && mz700_devbus_mmio_tracked ( addr ) ) {
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_MMIO_W, 0,
                          g_mzarch_main.instruction_addr, pl );
    }

    /*
     * Memory Heatmap recording (před vlastním zápisem - resolver musí vidět
     * current banking, který se ještě nezměnil tímto write callbackem;
     * banking změny dělá port write).
     *
     * V5: W vs S klasifikace dle aktivních stack regionů
     * (stack_regions_classify_write). Volba se aplikuje konzistentně na
     * všech mhmap_inc voláních v tomto bloku.
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
#if MZARCH == 800
            /* MZ-800: MZ-700 mód má samostatné regiony VRAM700_CG/VRAM700,
             * plus fyzickou VRAM (Plane I = VRAM1 bank). */
            if ( region == MHMAP_REGION_VRAM700_CG ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + offset, access );
            } else if ( region == MHMAP_REGION_VRAM700 ) {
                mhmap_inc ( MHMAP_REGION_VRAM, MHMAP_VRAM_BANK1_OFFSET + 0x1000 + offset, access );
            } else
#endif
            if ( region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED ) {
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
    /* Záznam zápisů (ramlog, háček B): logovací cesta je sama alternativní. */
    if ( g_ramlog_active ) {
        ramlog_hook_logging_write ( cpu, addr, value, user_data );
    } else {
        /* Čistý zápis (memory_write_cb, nebo obálka devbus se zařízením). */
        g_mz700_cpu_ops.mwrite ( cpu, addr, value, user_data );
    };
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
    memory_write_byte_no_bp ( addr, value );
}


/**
 * @brief Vlastni zapis bez synchronizace a bez MEM_W breakpointu (telo memory_write_byte_no_bp).
 *
 * Stejny zapis jako memory_write_byte() (banking, VRAM, mapovane porty), ale
 * bez vyhodnoceni MEM_W breakpointu. Pro zapisy debuggerem
 * (debugger_memory_write_byte, region Logical: MCP region_write, Memory
 * Browser, freeze) - MEM_W breakpoint hlida zapisy CPU.
 *
 * Volající jdou přes g_mz700_cpu_ops.raw_write (mz700_devbus.h): bez
 * připojeného zařízení devbus je to tato funkce, se zařízením s MMIO obálka,
 * která zápis navíc ohlásí zařízení (DEVBUS_ACC_DEBUGGER).
 *
 * @param addr Adresa v mapě CPU.
 * @param value Zapisovaný bajt.
 */
void memory_write_byte_raw ( uint16_t addr, uint8_t value ) {
    unsigned addr_high = addr >> 12;
    memory_internal_write_0000_0fff ( addr_high );
    memory_internal_write_1000_cfff ( addr_high );
    memory_internal_write_d000_dfff ( addr_high );
    memory_internal_write_e000_efff ( addr_high );
    memory_internal_write_f000_ffff ( addr_high );
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/** @brief g_mz700_cpu_ops.raw_write pro ramlog_hook_write(). */
static void memory_write_byte_raw_fn ( uint16_t addr, uint8_t value, void *ctx ) {
    ( void ) ctx;
    g_mz700_cpu_ops.raw_write ( addr, value );
}
#endif


/**
 * Zapis do aktualne mapovane pameti - bez synchronizace, bez MEM_W breakpointu.
 *
 * Viz memory_write_byte_raw(). Se zaznamem zapisu (ramlog) se zapis
 * zaznamena se zdrojem z g_ramlog_src, jinak DEBUGGER.
 *
 * @param addr
 * @param value
 */
void memory_write_byte_no_bp ( uint16_t addr, uint8_t value ) {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if ( g_ramlog_active ) {
        ramlog_hook_write ( addr, value, RAMLOG_SRC_DEBUGGER, memory_write_byte_raw_fn, NULL );
        return;
    };
#endif
    g_mz700_cpu_ops.raw_write ( addr, value );
}


/*******************************************************************************
 *
 * Obálky devbus (mz700_devbus.h): snoop zápisu do stránky 0E000h. Aktivní jen
 * s připojeným zařízením, které má MMIO rozsah; jinak CPU volá memory_write_cb
 * a debugger memory_write_byte_raw přímo.
 *
 ******************************************************************************/


/**
 * @brief Stav mapy stránky 0E000h a příznak Prohibited pro MMIO handler devbus.
 * @return DEVBUS_MMIO_MAPPED nebo DEVBUS_MMIO_DRAM, případně | DEVBUS_ACC_PROHIBITED.
 */
static inline unsigned memory_devbus_map_flags ( void ) {
    return ( MEMORY_MZ700_MAP_TEST_ROM_E000 ? DEVBUS_MMIO_MAPPED : DEVBUS_MMIO_DRAM )
           | ( MEMORY_MZ700_MAP_TEST_PROHIBITED ? DEVBUS_ACC_PROHIBITED : 0u );
}


void memory_write_cb_devbus ( z80_t *cpu, uint16_t addr, uint8_t value, void *user_data ) {
    memory_write_cb ( cpu, addr, value, user_data ); /* základní deska beze změny */
    if ( ( addr >> 12 ) != 0x0e ) return;
    devbus_mmio_dispatch ( addr, value, memory_devbus_map_flags ( ) | DEVBUS_ACC_CPU );
}


void memory_write_byte_raw_devbus ( uint16_t addr, uint8_t value ) {
    memory_write_byte_raw ( addr, value );
    if ( ( addr >> 12 ) != 0x0e ) return;
    devbus_mmio_dispatch ( addr, value, memory_devbus_map_flags ( ) | DEVBUS_ACC_DEBUGGER );
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

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Záznam zápisů: souhrnný záznam bloku (studená cesta). */
    if ( g_ramlog_active ) ramlog_hook_load_block ( addr, size, type == MEMORY_LOAD_RAMONLY );
#endif

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
                if ( !MEMORY_MZ700_MAP_TEST_ROM_0000 ) {
                    memcpy ( dst, src, load_size );
                };
            } else if ( addr < 0xd000 ) {
                uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                load_size = ( total_size < limit ) ? size : ( limit - addr );
                memcpy ( dst, src, load_size );
            } else if ( addr < 0xe000 ) {
                load_size = ( total_size < 0xe000 ) ? size : ( 0xe000 - addr );
                if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) {
                    /* MZ-700 ma jednoduchou 4 KB VRAM bez bank/PCG bloku.
                     * Maska 0x0fff prevadi sbernicovou adresu D000-DFFF na
                     * offset 0-0xFFF uvnitr g_memoryVRAM. Predtim tu byl
                     * pattern (0x1000 | addr) zdedeny z MZ-1500 dual VRAM/PCG
                     * layoutu, ktery pro MZ-700 vedl k out-of-bounds zapisu. */
                    dst = &g_memoryVRAM[( addr & 0x0fff )];
                };
                memcpy ( dst, src, load_size );
            } else {
                uint32_t limit = ( ( addr >> 12 ) + 1 ) << 12;
                load_size = ( total_size < limit ) ? size : ( limit - addr );
                if ( !MEMORY_MZ700_MAP_TEST_ROM_E000 ) {
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
 * Memory Map debug query (MZ-700)
 *
 * Vrací druh regionu pro 4 kB stránku z pohledu Z80. Reflektuje aktuální
 * banking stav (`g_memory.map`). Read-only, side-effect free.
 *
 * Mapování přesně odpovídá makrům `memory_internal_read_*` v tomto souboru.
 *
 ******************************************************************************/

en_MEMMAP_REGION_KIND memmap_query ( uint8_t addr_point )
{
    if ( addr_point > 0x0f ) return MEMMAP_KIND_RAM;

    int prohibited = MEMORY_MZ700_MAP_TEST_PROHIBITED ? 1 : 0;

    switch ( addr_point ) {
        case 0x00:
            /* $0000-$0FFF: Monitor ROM low (pokud ROM_0000 set), jinak RAM. */
            if ( MEMORY_MZ700_MAP_TEST_ROM_0000 ) return MEMMAP_KIND_ROM_LOW;
            return MEMMAP_KIND_RAM;

        case 0x0d:
            /* $D000-$DFFF: Pokud ROM_E000 (= horní oblast) je mapovaná,
             * je zde znaková + atributová VRAM. Jinak RAM. */
            if ( MEMORY_MZ700_MAP_TEST_VRAM_D000 ) return MEMMAP_KIND_VRAM_TEXT;
            return MEMMAP_KIND_RAM;

        case 0x0e:
            /* $E000-$EFFF: Pokud ROM_E000 set, $E000-$E008 mapped ports +
             * $E010-$EFFF horní ROM (E800 monitor). UI ukazuje stránku
             * jako MAPPED_PORTS (= dominantní vlastnost - bez nich
             * neběží I/O). Při Prohibited (OUT E5) jsou $E800-$EFFF
             * odpojené (0xFF) - UI to však signalizuje na stránce
             * $F000-$FFFF, kde je celá ROM odpojená.
             * Pokud ROM_E000 NE set = celé E000-EFFF = RAM. */
            if ( !MEMORY_MZ700_MAP_TEST_ROM_E000 ) return MEMMAP_KIND_RAM;
            return MEMMAP_KIND_MAPPED_PORTS;

        case 0x0f:
            /* $F000-$FFFF: Pokud ROM_E000 NE set = RAM.
             * Pokud ROM_E000 set + Prohibited = celá stránka 0xFF (= ROM
             * F000 odpojeno). Jinak ROM. */
            if ( !MEMORY_MZ700_MAP_TEST_ROM_E000 ) return MEMMAP_KIND_RAM;
            if ( prohibited ) return MEMMAP_KIND_PROHIBITED;
            return MEMMAP_KIND_ROM_HIGH;

        default:
            /* Stránky 0x01-0x0c = vždy DRAM (ROM/VRAM tam neexistují). */
            return MEMMAP_KIND_RAM;
    };
}
