/*
 * File:   mz1500_memory.c
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
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/ramlog_hook.h"
#endif

#include "libs/cpu-z80/z80.h"

#include "emulator.h"
#include "mzarch/mzarch.h"
#include "memory/memext.h"
#include "memory/memory.h"
#include "mz1500_memory.h"
#include "memory/rom.h"
#include "mzarch/mz1500/gdg/mz1500_gdg.h"
#include "mzarch/mz1500/gdg/mz1500_vramctrl.h"
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
#include "baseui/baseui.h"

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#define DBGLEVEL (DBGNON /* | DBGERR | DBGWAR | DBGINF*/)
#include "debug.h"


st_MEMORY g_memory;

/* ukazatele na PCG banky */
uint8_t *g_memoryPCG1 = g_memory.PCG;
uint8_t *g_memoryPCG2 = &g_memory.PCG[MEMORY_SIZE_PCG_BANK];
uint8_t *g_memoryPCG3 = &g_memory.PCG[MEMORY_SIZE_PCG_BANK * 2];



/*
 * Makra pro pristup do pameti
 */
#define MEMORY_ROM_READ_BYTE                g_memory.ROM[addr & 0x3fff]
#define MEMORY_CGROM_READ_BYTE              g_memory.ROM[addr & 0x0fff]

#define MEMORY_RAM_READ_BYTE                (g_memory.memram_read[(addr >> 12)])[(addr & 0x0fff)]
#define MEMORY_RAM_WRITE_BYTE               (g_memory.memram_write[(addr >> 12)])[(addr & 0x0fff)] = value;

#define MEMORY_VRAM_READ_BYTE_SYNC          vramctrl_mz700_memop_read_byte_sync(addr & 0x0fff)
#define MEMORY_VRAM_WRITE_BYTE_SYNC         vramctrl_mz700_memop_write_byte_sync(addr & 0x0fff, value)

#define MEMORY_VRAM_READ_BYTE               vramctrl_mz700_memop_read_byte(addr & 0x0fff)
#define MEMORY_VRAM_WRITE_BYTE              vramctrl_mz700_memop_write_byte(addr & 0x0fff, value)

/* PCG pristup — pcg_id = spec_id - 2 (0, 1, 2) */
#define MEMORY_PCG_READ_BYTE_SYNC           vramctrl_mz700_memop_read_byte_sync((pcg_id * MEMORY_SIZE_PCG_BANK) + (addr & 0x3fff))
#define MEMORY_PCG_WRITE_BYTE_SYNC          vramctrl_mz700_memop_write_byte_sync(((pcg_id * MEMORY_SIZE_PCG_BANK) + (addr & 0x3fff)), value)

#define MEMORY_PCG_READ_BYTE                vramctrl_mz700_memop_read_byte((pcg_id * MEMORY_SIZE_PCG_BANK) + (addr & 0x3fff))
#define MEMORY_PCG_WRITE_BYTE               vramctrl_mz700_memop_write_byte(((pcg_id * MEMORY_SIZE_PCG_BANK) + (addr & 0x3fff)), value)


/*******************************************************************************
 *
 * Mapovani MZ-1500 pameti ROM_0000, ROM_UPPER, SPEC
 *
 ******************************************************************************/


/**
 * OUT 0xE0 - memory unmap ROM 0000
 */
static inline void memory_mmap_rom_bottom_off(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe0 (0000 - 0FFF: RAM), PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map &= ~(MEMORY_MZ1500_MAP_FLAG_ROM_0000);
}


/**
 * OUT 0xE1 - memory unmap ROM UPPER (VRAM D000, porty E000, ROM E800)
 */
static inline void memory_mmap_rom_upper_off(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe1 (D000 - FFFF: RAM), PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map &= ~(MEMORY_MZ1500_MAP_FLAG_ROM_UPPER);
}


/**
 * OUT 0xE2 - memory map ROM 0000
 */
static inline void memory_mmap_rom_0000_on(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe2 (0000 - 0FFF: ROM), PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map |= MEMORY_MZ1500_MAP_FLAG_ROM_0000;
}


/**
 * OUT 0xE3 - memory map ROM UPPER + zrus SPEC
 */
static inline void memory_mmap_rom_upper_on(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe3 (D000 - FFFF: ROM), PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map |= MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
    g_memory.map &= ~(MEMORY_MZ1500_MAP_D000_MASK);
}


/**
 * OUT 0xE4 - memory map ROM 0000 + ROM UPPER
 */
static inline void memory_mmap_all_on(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe4, PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map = (MEMORY_MZ1500_MAP_FLAG_ROM_0000 | MEMORY_MZ1500_MAP_FLAG_ROM_UPPER);
}


/**
 * OUT 0xE5 - memory map SPEC D000 (value & 0x03 urcuje SPEC ID)
 */
static inline void memory_mmap_spec_on(uint8_t value) {
    value &= 0x03;
    DBGPRINTF(DBGINF, "pwrite = 0xe5, mount SPEC - value: 0x%02x, PC = 0x%04x\n", value, g_mzarch_main.instruction_addr);
    g_memory.map &= ~(MEMORY_MZ1500_MAP_D000_MASK);
    g_memory.map |= (value + 1) << MEMORY_MZ1500_FLAG_SPEC_BITPOS;
    g_memory.map |= MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;
}


/**
 * OUT 0xE6 - memory umap SPEC D000
 */
static inline void memory_mmap_spec_off(void) {
    DBGPRINTF(DBGINF, "pwrite = 0xe6, umount SPEC, PC = 0x%04x\n", g_mzarch_main.instruction_addr);
    g_memory.map &= ~(MEMORY_MZ1500_MAP_D000_MASK);
}


/**
 * Mapovani pameti pres IORQ - pwrite.
 */
void memory_map_pwrite(uint8_t mmap_port, uint8_t value) {

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

    switch (mmap_port) {
        case 0xe0:
            memory_mmap_rom_bottom_off();
            break;

        case 0xe1:
            memory_mmap_rom_upper_off();
            break;

        case 0xe2:
            memory_mmap_rom_0000_on();
            break;

        case 0xe3:
            memory_mmap_rom_upper_on();
            break;

        case 0xe4:
            memory_mmap_all_on();
            break;

        case 0xe5:
            memory_mmap_spec_on(value);
            break;

        case 0xe6:
            memory_mmap_spec_off();
            break;
    };
}


/**
 * Mapovani pameti pres IORQ - pread.
 * MZ-1500 nema pread mapovani.
 */
void memory_map_pread(uint8_t mmap_port) {
    (void)mmap_port;
}


/*******************************************************************************
 *
 * Inicializace pameti RAM, MEMEXT, VRAM a PCG
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
 * Studena inicializace pameti
 */
void memory_init(void) {

    DBGPRINTF(DBGINF, "\n");

    uint32_t i;
    uint16_t *addr;

    /* inicializace RAM */
    uint16_t value = 0x00ff;
    for (i = 0; i < 0xffff; i += 4) {
        addr = (uint16_t *)&g_memory.RAM[i];
        if ((i & 0x7f) == 0x00) {
            value = ~value;
        };
        *addr++ = value;
        *addr = value;
    };

    /* inicializace VRAM */
    for (i = 0; i < MEMORY_SIZE_VRAM; i += 2) {
        addr = (uint16_t *)&g_memory.VRAM[i];
        *addr = 0x00ff;
    };

    rom_init();

    g_memory.map = 0;

    memext_init();
    memory_reconnect_ram();
}


/**
 * Tepla inicializace pameti po resetu
 */
void memory_reset(void) {
    DBGPRINTF(DBGINF, "\n");
    g_memory.map = MEMORY_MZ1500_MAP_FLAG_ROM_0000 | MEMORY_MZ1500_MAP_FLAG_ROM_UPPER;

    memext_reset();
    memory_reconnect_ram();
}


/*******************************************************************************
 *
 * Cteni z aktualne mapovane pameti
 *
 ******************************************************************************/

/*
 * 0x0000 - 0x0FFF: RAM nebo ROM
 */
#define memory_internal_read_0000_0fff(a) { if (0x00 == a) { if (MEMORY_MZ1500_MAP_TEST_ROM_0000) return MEMORY_ROM_READ_BYTE; return MEMORY_RAM_READ_BYTE; } }

/*
 * 0xD000 - 0xDFFF: RAM / VRAM / CGROM / PCG
 */
#define memory_internal_read_d000_dfff(a) { if (0x0d == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { return MEMORY_VRAM_READ_BYTE; } \
        else if (spec_id == 1) { return MEMORY_CGROM_READ_BYTE; } \
        else { int pcg_id = spec_id - 2; return MEMORY_PCG_READ_BYTE; }; \
    } else { return MEMORY_RAM_READ_BYTE; }; }; }

#define memory_internal_read_d000_dfff_sync(a) { if (0x0d == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { return MEMORY_VRAM_READ_BYTE_SYNC; } \
        else if (spec_id == 1) { return MEMORY_CGROM_READ_BYTE; } \
        else { int pcg_id = spec_id - 2; return MEMORY_PCG_READ_BYTE_SYNC; }; \
    } else { return MEMORY_RAM_READ_BYTE; }; }; }

/*
 * 0xE000 - 0xEFFF: RAM / porty+ROM / CGROM / PCG
 */
#define memory_internal_read_e000_efff(a) { if (0x0e == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { return memory_internal_read_rom_e000_efff(addr); } \
        else if (spec_id == 1) { return MEMORY_CGROM_READ_BYTE; } \
        else { int pcg_id = spec_id - 2; return MEMORY_PCG_READ_BYTE; }; \
    } else { return MEMORY_RAM_READ_BYTE; }; }; }

#define memory_internal_read_e000_efff_sync(a) { if (0x0e == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { return memory_internal_read_rom_e000_efff_sync(addr); } \
        else if (spec_id == 1) { return MEMORY_CGROM_READ_BYTE; } \
        else { int pcg_id = spec_id - 2; return MEMORY_PCG_READ_BYTE_SYNC; }; \
    } else { return MEMORY_RAM_READ_BYTE; }; }; }

/*
 * 0xF000 - 0xFFFF: RAM / ROM / 0xFF (pokud SPEC)
 */
#define memory_internal_read_f000_ffff(a) { if (0x0f == a) { \
    if (MEMORY_MZ1500_MAP_TEST_RAM_UPPER) { return MEMORY_RAM_READ_BYTE; } \
    else { if (!MEMORY_MZ1500_MAP_TEST_D000_SPEC) return MEMORY_ROM_READ_BYTE; return 0xff; } } }


/**
 * Synchronizovane cteni z 0xE000 - 0xEFFF (porty a horni ROM)
 *
 * MZ-1500 má E000-E008 namapované **nativně bez podmínek** na PIO8255 /
 * CTC8253 / GDG status (= ne jen v MZ-700 mode jako MZ-800). Recording
 * trace-suite iorqlog MREQ_MAPPED je tedy aktivní stejně jako u MZ-800.
 */
static inline uint8_t memory_internal_read_rom_e000_efff_sync(uint16_t addr) {

    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni ROM - žádný iorqlog event (ROM read není mapped port) */
    if (addr_low > 0x0f) return MEMORY_ROM_READ_BYTE;

    /* cteni z E009 - E00F - latch fallback, žádný emit */
    if (addr_low > 0x08) return g_mzarch_main.regDBUS_latch;

    uint8_t retval;

    /* cteni z E008 (GDG status) */
    if (0x08 == addr_low) {
        mzarch_main_insideop_mreq_e00x();
        retval = gdg_read_dmd_status_memop();
    } else if (addr_low & 0x04) {
        /* cteni z CTC8253 - kontrolni registr cist nelze, latch fallback */
        if (0x07 == addr_low) return g_mzarch_main.regDBUS_latch;
        mzarch_main_insideop_mreq_e00x();
        retval = ctc8253_read_byte(addr_low & 0x03);
    } else {
        /* cteni z PIO8255 */
        mzarch_main_insideop_mreq_e00x();
        retval = pio8255_read(addr & 0x03);
    }

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * trace-suite iorqlog: emit MREQ-mapped IN event pro CTC/PIO/GDG
     * namapované na 0xE000-0xE008. Emit jen v reálné CPU instrukční cestě
     * (g_dbg_in_cpu_path), ne při pomocných čteních z memory_load_block /
     * debug browseru. source_addr = aktuální PC.
     */
    if (TEST_TRACE_IORQLOG_ACTIVE && g_dbg_in_cpu_path) {
        iorqlog_record(IORQLOG_EVENT_MREQ_MAPPED, IORQLOG_DIR_IN,
                       g_mzarch_main.instruction_addr, addr, retval, 0u);
    }
#endif

    return retval;
}


/**
 * Cteni z 0xE000 - 0xEFFF - bez synchronizace
 *
 * Bez vedlejšího efektu: mapované porty (8255, 8253, status GDG) se čtou
 * přes *_peek funkce čipů, které stav nemění (memory_read_byte_no_se,
 * memory_read_byte = debugger, Memory Browser, Watch, výrazy BP).
 */
static inline uint8_t memory_internal_read_rom_e000_efff(uint16_t addr) {
    unsigned addr_low = addr & 0x0fff;

    /* cteni z horni ROM */
    if (addr_low > 0x0f) return MEMORY_ROM_READ_BYTE;

    /* cteni z E009 - E00F */
    if (addr_low > 0x08) return g_mzarch_main.regDBUS_latch;

    /* cteni z E008 (GDG status) */
    if (0x08 == addr_low) return gdg_peek_dmd_status_memop();

    /* cteni z CTC8253 */
    if (addr_low & 0x04) {
        /* Kontrolni registr cist nelze */
        if (0x07 == addr_low) {
            return g_mzarch_main.regDBUS_latch;
        };
        return ctc8253_peek_byte(addr_low & 0x03);
    };

    /* cteni z PIO8255 */
    return pio8255_peek(addr & 0x03);
}


uint8_t memory_internal_read_sync(uint16_t addr) {

    unsigned addr_high = addr >> 12;

    memory_internal_read_0000_0fff(addr_high);
    memory_internal_read_d000_dfff_sync(addr_high);
    memory_internal_read_e000_efff_sync(addr_high);
    memory_internal_read_f000_ffff(addr_high);
    return MEMORY_RAM_READ_BYTE;
}


/**
 * Callback pro cteni bajtu se synchronizaci.
 */
uint8_t memory_read_cb(z80_t *cpu, uint16_t addr, int m1_state, void *user_data) {
    (void)cpu;
    (void)m1_state;
    (void)user_data;
    uint8_t retval = memory_internal_read_sync(addr);
    g_mzarch_main.regDBUS_latch = retval;
    return retval;
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * Callback pro cteni bajtu se synchronizaci + debugging historie.
 */
uint8_t memory_read_with_logging_cb(z80_t *cpu, uint16_t addr, int m1_state, void *user_data) {
    (void)user_data;

    /* Klasifikace přístupu (X vs R) - viz mz800_memory.c pro detailní popis
     * pravidla; pro MZ-1500 platí totéž (Z80 dispatching je stejný). */
    en_MHMAP_ACCESS access_kind;
    int is_m1_start = (m1_state) && (addr == g_mzarch_main.instruction_addr);
    if (is_m1_start) {
        access_kind = MHMAP_ACCESS_X;
    } else if (m1_state) {
        /* M1 prefix opcode uvnitř téže instrukce (DD/FD/ED/CB ...). */
        access_kind = MHMAP_ACCESS_X;
    } else if ((uint16_t)cpu->pc == (uint16_t)(addr + 1)) {
        /* Operand instrukce (FETCH z PC, PC už posunuté); datové čtení
         * hned za instrukcí má cpu->pc == addr (N60). */
        access_kind = MHMAP_ACCESS_X;
    } else {
        access_kind = MHMAP_ACCESS_R;
    };
    g_mhmap_pending_read_kind = access_kind;

    /* Označit CPU debug cestu - VRAM hook (vramctrl_*) a iorqlog MREQ hook
     * recordují jen pokud je flag nastaven. Mimo CPU cestu (memory_load_block,
     * debug memory browser) flag zůstává 0 a hooky se přeskočí. */
    g_dbg_in_cpu_path = 1;
    uint8_t retval = memory_internal_read_sync(addr);
    g_dbg_in_cpu_path = 0;
    g_mzarch_main.regDBUS_latch = retval;

    /*
     * 2.4b MEM_R BP enforce hook - jen pro data reads (access_kind == R),
     * NE pro instruction fetch (X = M1 opcode + prefix + immediate operand).
     * Viz mz800_memory.c pro detailní popis sémantiky filtru. Volá se PO
     * memory_internal_read_sync, aby ctx.Value = právě přečtený byte.
     * Fast-skip via per_type_active flag.
     */
    if (access_kind == MHMAP_ACCESS_R
        && g_bptmap.per_type_active[BPTMAP_IDX_MEM_R]) {
        breakpoints_enforce_mem_r(addr, retval);
    }

    /*
     * V1.5.E - I/O Ports panel MMIO event tracking.
     * Filter: jen data reads (= R kind), ne instruction fetch (X), na
     * rozsahu 0xE000-0xE008 (= MZ-700 mode mirror PIO/CTC/GDG v ROM space).
     * Gated stejne jako IORQ varianta pres g_io_window_tracking_active.
     */
    if (access_kind == MHMAP_ACCESS_R
        && addr >= 0xE000u && addr <= 0xE008u
        && g_io_window_tracking_active) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit(addr, retval,
                               true /* is_in / is_read */, frame_n);
        io_history_record_mem(true /* is_read */, addr, retval,
                              g_mzarch_main.instruction_addr,
                              frame_n,
                              (uint16_t) g_gdg.beam_row,
                              (uint16_t) VIDEO_GET_SCREEN_COL(
                                  g_gdg.total_elapsed.ticks),
                              g_mzarch_main.cpu->total_cycles);
    }

    /* event-viewer Vlna 1: paralelní fan-out MMIO_R do eventlog ringu. */
    if (access_kind == MHMAP_ACCESS_R
        && addr >= 0xE000u && addr <= 0xE008u
        && TEST_TRACE_EVENTLOG_ACTIVE
        && (g_eventlog_active_mask & (1ULL << EVENTLOG_CAT_MMIO_R))) {
        uint32_t pl = (uint32_t) addr | ((uint32_t) retval << 16);
        eventlog_record(EVENTLOG_CAT_MMIO_R, 0,
                        g_mzarch_main.instruction_addr, pl);
    }

    /* Aktualizace byte_position counteru pro klasifikaci dalších čtení. */
    if (is_m1_start) {
        g_debugger_history.byte_position = 1;
    } else if (access_kind == MHMAP_ACCESS_X) {
        g_debugger_history.byte_position++;
    };
    /* Bajty instrukce tak, jak je CPU načetlo (cputrack, nezávisle na cpuhist). */
    if (access_kind == MHMAP_ACCESS_X) {
        debugger_insn_fetch_record(is_m1_start, g_debugger_history.byte_position - 1, addr, retval);
    };

    /* CPU Instruction History (cpuhist) - dle cpuhist_mode (default jen při aktivním debug okně). */
    if (TEST_DEBUGGER_CPUHIST_ACTIVE) {
        if (is_m1_start) {
            g_debugger_history.position++;
            int position = debugger_history_position(g_debugger_history.position);
            g_debugger_history.row[position].addr = addr;
            g_debugger_history.row[position].byte[0] = retval;
            debugger_history_capture_regs(&g_debugger_history.row[position].regs, cpu);
        } else if (access_kind == MHMAP_ACCESS_X) {
            int position = debugger_history_position(g_debugger_history.position);
            unsigned slot = g_debugger_history.byte_position - 1;
            if (slot < DEBUGGER_MAX_INSTR_BYTES) {
                g_debugger_history.row[position].byte[slot] = retval;
            };
        };
    };

    /* Memory Heatmap recording - jen pokud aktivní MH. */
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        mhmap_inc(MHMAP_REGION_BUS, addr, access_kind);
        en_MHMAP_REGION region;
        unsigned offset;
        if (mhmap_resolve_mem(addr, &region, &offset) == MHMAP_RESOLVE_DIRECT) {
            mhmap_inc(region, offset, access_kind);
            /* Memext recording - pokud RAM access prošel přes Memext bank,
             * log do flat 512 KB MEMEXT regionu. */
            if (region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED) {
                int32_t mx_off = memext_get_ram_offset_from_pointer(
                    g_memory.memram_read[addr >> 12]);
                if (mx_off >= 0) {
                    mhmap_inc(MHMAP_REGION_MEMEXT,
                              (unsigned) mx_off + (addr & 0x0fff),
                              access_kind);
                };
            };
        };
    };

    return retval;
}
#endif


/**
 * Cteni z aktualne mapovane pameti - bez synchronizace.
 */
uint8_t memory_read_byte(uint16_t addr) {
    unsigned addr_high = addr >> 12;
    memory_internal_read_0000_0fff(addr_high);
    memory_internal_read_d000_dfff(addr_high);
    memory_internal_read_e000_efff(addr_high);
    memory_internal_read_f000_ffff(addr_high);
    uint8_t value = MEMORY_RAM_READ_BYTE;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.2 MEM_R BP hook - viz mz800_memory.c. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_R ] ) {
        breakpoints_enforce_mem_r ( addr, value );
    }
#endif
    return value;
}


/**
 * V1.6+ TODO 4.2 5a: debugger-safe variant memory_read_byte (mz1500).
 *
 * Bez MEM_R BP fire (= side-effect free pro BP expression eval).
 */
uint8_t memory_read_byte_no_se(uint16_t addr) {
    unsigned addr_high = addr >> 12;
    memory_internal_read_0000_0fff(addr_high);
    memory_internal_read_d000_dfff(addr_high);
    memory_internal_read_e000_efff(addr_high);
    memory_internal_read_f000_ffff(addr_high);
    return MEMORY_RAM_READ_BYTE;
}



/*******************************************************************************
 *
 * Zapis do aktualne mapovane pameti
 *
 ******************************************************************************/


/*
 * 0x0000 - 0x0FFF: RAM (pokud neni ROM)
 */
#define memory_internal_write_0000_0fff(a) { if (0x00 == a) { if (!MEMORY_MZ1500_MAP_TEST_ROM_0000) MEMORY_RAM_WRITE_BYTE; return; } }

/*
 * 0x1000 - 0xCFFF: vzdy RAM
 */
#define memory_internal_write_1000_cfff(a) { if ((0x01 <= a) && (0x0c >= a)) { MEMORY_RAM_WRITE_BYTE; return; } }

/*
 * 0xD000 - 0xDFFF: RAM / VRAM / CGROM(readonly) / PCG
 */
#define memory_internal_write_d000_dfff(a) { if (0x0d == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { MEMORY_VRAM_WRITE_BYTE; return; } \
        else if (spec_id == 1) { return; } \
        else { int pcg_id = spec_id - 2; MEMORY_PCG_WRITE_BYTE; return; }; \
    } else { MEMORY_RAM_WRITE_BYTE; return; }; }; }

#define memory_internal_write_d000_dfff_sync(a) { if (0x0d == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { MEMORY_VRAM_WRITE_BYTE_SYNC; return; } \
        else if (spec_id == 1) { return; } \
        else { int pcg_id = spec_id - 2; MEMORY_PCG_WRITE_BYTE_SYNC; return; }; \
    } else { MEMORY_RAM_WRITE_BYTE; return; }; }; }

/*
 * 0xE000 - 0xEFFF: RAM / porty / CGROM(readonly) / PCG
 */
#define memory_internal_write_e000_efff(a) { if (0x0e == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { memory_internal_write_rom_e000(addr, value); return; } \
        else if (spec_id == 1) { return; } \
        else { int pcg_id = spec_id - 2; MEMORY_PCG_WRITE_BYTE; return; }; \
    } else { MEMORY_RAM_WRITE_BYTE; return; }; }; }

#define memory_internal_write_e000_efff_sync(a) { if (0x0e == a) { \
    if (MEMORY_MZ1500_MAP_TEST_ROM_UPPER) { \
        int spec_id = MEMORY_MZ1500_SPEC_ID; \
        if (spec_id == 0) { memory_internal_write_rom_e000_sync(addr, value); return; } \
        else if (spec_id == 1) { return; } \
        else { int pcg_id = spec_id - 2; MEMORY_PCG_WRITE_BYTE_SYNC; return; }; \
    } else { MEMORY_RAM_WRITE_BYTE; return; }; }; }

/*
 * 0xF000 - 0xFFFF: RAM / ROM(readonly) / 0xFF(SPEC)
 */
#define memory_internal_write_f000_ffff(a) { if (0x0f == a) { if (MEMORY_MZ1500_MAP_TEST_RAM_UPPER) { MEMORY_RAM_WRITE_BYTE; return; } else { return; } } }


/**
 * Synchronizovany zapis na E000-E008 (porty)
 *
 * MZ-1500 má E000-E008 nativně bez podmínek (viz read protějšek).
 */
static inline void memory_internal_write_rom_e000_sync(uint16_t addr, uint8_t value) {

    if (addr > 0xe008) return;

    mzarch_main_insideop_mreq_e00x();

    if (addr == 0xe008) {
        gdg_write_byte(addr, value);
    } else if (addr & 0x04) {
        ctc8253_write_byte(addr & 0x03, value);
    } else {
        pio8255_write(addr & 0x03, value);
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /*
     * trace-suite iorqlog: emit MREQ-mapped OUT event pro CTC/PIO/GDG na
     * 0xE000-0xE008. Emit jen pokud jdeme z reálné CPU instrukční cesty.
     */
    if (TEST_TRACE_IORQLOG_ACTIVE && g_dbg_in_cpu_path) {
        iorqlog_record(IORQLOG_EVENT_MREQ_MAPPED, IORQLOG_DIR_OUT,
                       g_mzarch_main.instruction_addr, addr, value, 0u);
    }
#endif
}


/**
 * Zapis na E000-E008 - bez synchronizace
 */
static inline void memory_internal_write_rom_e000(uint16_t addr, uint8_t value) {

    if (addr > 0xe008) return;

    if (addr == 0xe008) {
        gdg_write_byte(addr, value);
    } else if (addr & 0x04) {
        ctc8253_write_byte(addr & 0x03, value);
    } else {
        pio8255_write(addr & 0x03, value);
    };
}


/**
 * Callback pro zapis bajtu se synchronizaci.
 */
void memory_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data) {
    (void)cpu;
    (void)user_data;
    unsigned addr_high = addr >> 12;

    memory_internal_write_0000_0fff(addr_high);
    memory_internal_write_1000_cfff(addr_high);
    memory_internal_write_d000_dfff_sync(addr_high);
    memory_internal_write_e000_efff_sync(addr_high);
    memory_internal_write_f000_ffff(addr_high);
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/**
 * Callback pro zapis bajtu se synchronizaci + CDL recording.
 *
 * Aktivuje se přes z80_set_mwrite() při zapnutí debuggeru.
 */
void memory_write_with_logging_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data) {
    (void)cpu;
    (void)user_data;
    unsigned addr_high = addr >> 12;

    /* MEM_W BP enforce hook PŘED vlastním zápisem - condition může číst
     * "starou" hodnotu přes [addr] no-side-effect deref. Fast-skip via
     * per_type_active flag (zero overhead když žádný MEM_W BP není aktivní).
     * Tento callback je aktivní v debugger active mode (z80_set_mwrite),
     * takže pokrývá všechny CPU writes. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] ) {
        breakpoints_enforce_mem_w ( addr, value );
    }

    /*
     * V1.5.E - I/O Ports panel MMIO event tracking (write side).
     * Filter: rozsah 0xE000-0xE008. Gated pres g_io_window_tracking_active.
     */
    if (addr >= 0xE000u && addr <= 0xE008u
        && g_io_window_tracking_active) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit(addr, value,
                               false /* is_in - jsme write */, frame_n);
        io_history_record_mem(false /* is_read */, addr, value,
                              g_mzarch_main.instruction_addr,
                              frame_n,
                              (uint16_t) g_gdg.beam_row,
                              (uint16_t) VIDEO_GET_SCREEN_COL(
                                  g_gdg.total_elapsed.ticks),
                              g_mzarch_main.cpu->total_cycles);
    }

    /* event-viewer Vlna 1: paralelní fan-out MMIO_W do eventlog ringu. */
    if (addr >= 0xE000u && addr <= 0xE008u
        && TEST_TRACE_EVENTLOG_ACTIVE
        && (g_eventlog_active_mask & (1ULL << EVENTLOG_CAT_MMIO_W))) {
        uint32_t pl = (uint32_t) addr | ((uint32_t) value << 16);
        eventlog_record(EVENTLOG_CAT_MMIO_W, 0,
                        g_mzarch_main.instruction_addr, pl);
    }

    /* Memory Heatmap recording PŘED vlastním zápisem - resolver musí vidět
     * current banking. Jen pokud aktivní MH.
     *
     * V5: W vs S klasifikace dle aktivních stack regionů. Volba se aplikuje
     * konzistentně (BUS + region + memext). */
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        en_MHMAP_ACCESS access = MHMAP_ACCESS_W;
        if ( g_stack_regions_active && stack_regions_classify_write ( addr ) ) {
            access = MHMAP_ACCESS_S;
        }
        mhmap_inc(MHMAP_REGION_BUS, addr, access);
        en_MHMAP_REGION region;
        unsigned offset;
        if (mhmap_resolve_mem(addr, &region, &offset) == MHMAP_RESOLVE_DIRECT) {
            mhmap_inc(region, offset, access);
            if (region == MHMAP_REGION_RAM && MEMEXT_TEST_CONNECTED) {
                int32_t mx_off = memext_get_ram_offset_from_pointer(
                    g_memory.memram_write[addr >> 12]);
                if (mx_off >= 0) {
                    mhmap_inc(MHMAP_REGION_MEMEXT,
                              (unsigned) mx_off + (addr & 0x0fff),
                              access);
                };
            };
        };
    };

    /* Označit CPU debug cestu - iorqlog MREQ hook v E000-E008 dispatch
     * a VRAM hook recordují jen pokud je flag nastaven. */
    /* Záznam zápisů (ramlog, háček B): logovací cesta je sama alternativní. */
    if (g_ramlog_active) {
        g_dbg_in_cpu_path = 1;
        ramlog_hook_logging_write(cpu, addr, value, user_data);
        g_dbg_in_cpu_path = 0;
        return;
    }
    /* Zápis jako funkce (vzor MZ-800 a MZ-700): makra memory_internal_write_*
     * končí return, který tak ukončí jen memory_write_cb, a příznak CPU cesty
     * se po zápisu vynuluje. Rozepsaná makra přímo tady ho nechávala 1. */
    g_dbg_in_cpu_path = 1;
    memory_write_cb(cpu, addr, value, user_data);
    g_dbg_in_cpu_path = 0;
}
#endif


/**
 * Zapis do aktualne mapovane pameti - bez synchronizace.
 */
void memory_write_byte(uint16_t addr, uint8_t value) {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.2 MEM_W BP hook - viz mz800_memory.c. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_MEM_W ] ) {
        breakpoints_enforce_mem_w ( addr, value );
    }
#endif
    memory_write_byte_no_bp(addr, value);
}


/**
 * @brief Vlastni zapis bez synchronizace a bez MEM_W breakpointu (telo memory_write_byte_no_bp).
 *
 * Stejny zapis jako memory_write_byte() (banking, VRAM, mapovane porty), ale
 * bez vyhodnoceni MEM_W breakpointu. Pro zapisy debuggerem
 * (debugger_memory_write_byte, region Logical: MCP region_write, Memory
 * Browser, freeze) - MEM_W breakpoint hlida zapisy CPU.
 *
 * @param addr
 * @param value
 */
static void memory_write_byte_raw(uint16_t addr, uint8_t value) {
    unsigned addr_high = addr >> 12;
    memory_internal_write_0000_0fff(addr_high);
    memory_internal_write_1000_cfff(addr_high);
    memory_internal_write_d000_dfff(addr_high);
    memory_internal_write_e000_efff(addr_high);
    memory_internal_write_f000_ffff(addr_high);
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/** @brief memory_write_byte_raw() pro ramlog_hook_write(). */
static void memory_write_byte_raw_fn(uint16_t addr, uint8_t value, void *ctx) {
    (void)ctx;
    memory_write_byte_raw(addr, value);
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
void memory_write_byte_no_bp(uint16_t addr, uint8_t value) {
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (g_ramlog_active) {
        ramlog_hook_write(addr, value, RAMLOG_SRC_DEBUGGER, memory_write_byte_raw_fn, NULL);
        return;
    };
#endif
    memory_write_byte_raw(addr, value);
}


/**
 * Nacteni datoveho bloku do pameti.
 *
 * TODO: implementovat pro MZ-1500 (zatim jen zapis do RAM)
 */
void memory_load_block(uint8_t *data, uint16_t addr, uint16_t size, en_MEMORY_LOAD type) {

    uint16_t src_addr = 0;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Záznam zápisů: souhrnný záznam bloku (studená cesta). */
    if (g_ramlog_active) ramlog_hook_load_block(addr, size, type == MEMORY_LOAD_RAMONLY);
#endif

    while (size) {
        uint8_t *dst = g_memory.memram_write[(addr) >> 12] + (addr & 0x0fff);
        uint8_t *src = &data[src_addr];
        uint32_t total_size = addr + size;
        uint32_t load_size;

        if (type == MEMORY_LOAD_RAMONLY) {
            uint32_t limit = ((addr >> 12) + 1) << 12;
            load_size = (total_size < limit) ? size : (limit - addr);
            memcpy(dst, src, load_size);
        } else {
            if (addr < 0x1000) {
                load_size = (total_size < 0x1000) ? size : (0x1000 - addr);
                if (!MEMORY_MZ1500_MAP_TEST_ROM_0000) {
                    memcpy(dst, src, load_size);
                };
            } else if (addr < 0xd000) {
                uint32_t limit = ((addr >> 12) + 1) << 12;
                load_size = (total_size < limit) ? size : (limit - addr);
                memcpy(dst, src, load_size);
            } else {
                uint32_t limit = ((addr >> 12) + 1) << 12;
                load_size = (total_size < limit) ? size : (limit - addr);
                if (!MEMORY_MZ1500_MAP_TEST_ROM_UPPER) {
                    memcpy(dst, src, load_size);
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
 * Memory Map debug query (MZ-1500)
 *
 * Vrací druh regionu pro 4 kB stránku z pohledu Z80. Reflektuje aktuální
 * banking stav (`g_memory.map` = ROM_0000 + ROM_UPPER + SPEC). Read-only,
 * side-effect free.
 *
 * MZ-1500 má E000-E008 namapované **nativně bez podmínek** (= ne jen
 * v MZ-700 modu jako MZ-800), když SPEC=0 a ROM_UPPER set. Při SPEC=2..4
 * (PCG1/2/3) zabírá PCG celých D000-EFFF (= obě stránky 0x0d a 0x0e).
 *
 ******************************************************************************/

en_MEMMAP_REGION_KIND memmap_query ( uint8_t addr_point )
{
    if ( addr_point > 0x0f ) return MEMMAP_KIND_RAM;

    int rom_upper = MEMORY_MZ1500_MAP_TEST_ROM_UPPER ? 1 : 0;
    int spec_id = MEMORY_MZ1500_SPEC_ID; /* 0..4 */

    switch ( addr_point ) {
        case 0x00:
            /* $0000-$0FFF: ROM_0000 set -> ROM, jinak RAM. */
            if ( MEMORY_MZ1500_MAP_TEST_ROM_0000 ) return MEMMAP_KIND_ROM_LOW;
            return MEMMAP_KIND_RAM;

        case 0x0d:
            /* $D000-$DFFF: ROM_UPPER NE -> RAM. ROM_UPPER + SPEC dle: */
            if ( !rom_upper ) return MEMMAP_KIND_RAM;
            switch ( spec_id ) {
                case 0:  return MEMMAP_KIND_VRAM_TEXT;  /* znaková + atributová VRAM */
                case 1:  return MEMMAP_KIND_CGROM;
                case 2:  return MEMMAP_KIND_PCG_1;
                case 3:  return MEMMAP_KIND_PCG_2;
                case 4:  return MEMMAP_KIND_PCG_3;
                default: return MEMMAP_KIND_RAM;        /* nedef. SPEC, fallback */
            };

        case 0x0e:
            /* $E000-$EFFF: ROM_UPPER NE -> RAM. ROM_UPPER + SPEC=0 ->
             * mapped ports ($E000-$E008) + ROM E800. SPEC=1 -> CGROM.
             * SPEC=2..4 -> PCG1/2/3 (PCG zabírá D000-EFFF dohromady). */
            if ( !rom_upper ) return MEMMAP_KIND_RAM;
            switch ( spec_id ) {
                case 0:  return MEMMAP_KIND_MAPPED_PORTS; /* dominantní pro stránku */
                case 1:  return MEMMAP_KIND_CGROM;
                case 2:  return MEMMAP_KIND_PCG_1;
                case 3:  return MEMMAP_KIND_PCG_2;
                case 4:  return MEMMAP_KIND_PCG_3;
                default: return MEMMAP_KIND_RAM;
            };

        case 0x0f:
            /* $F000-$FFFF: ROM_UPPER NE -> RAM. ROM_UPPER + SPEC=0 -> ROM.
             * ROM_UPPER + SPEC!=0 -> 0xFF (= UNMAPPED). */
            if ( !rom_upper ) return MEMMAP_KIND_RAM;
            if ( spec_id == 0 ) return MEMMAP_KIND_ROM_HIGH;
            return MEMMAP_KIND_UNMAPPED;

        default:
            /* Stránky 0x01-0x0c = vždy DRAM. */
            return MEMMAP_KIND_RAM;
    };
}
