#include "main.h"
#include <stdio.h>
#include <stdint.h>

#include "mzarch/mzarch_config.h"
#include "libs/cpu-z80/z80.h"
#include "mzarch/mz700/gdg/mz700_gdg.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/psg/psg.h"

#if HAVE_PIOZ80
#include "hw-generic/pioz80/pioz80.h"
#endif

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif /* CFG_HWEXT_HAVE_FDC */

#if CFG_HWEXT_HAVE_IDE8
#include "hw-generic/ide8/ide8.h"
#endif /* CFG_HWEXT_HAVE_IDE8 */

#if CFG_HWEXT_HAVE_RAMDISK
#include "hw-generic/ramdisk/ramdisk.h"
#endif /* CFG_HWEXT_HAVE_RAMDISK */

#include "hw-generic/unicard/unicard.h"

#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#endif /* CFG_HWEXT_HAVE_QDISK */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/iorqlog.h"
#include "debugger/io_catalog.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
#endif
#else
/* No-op definice pro release/non-debug build. */
#define TRACELOG_IORQ_MARK_UNCONNECTED()  do { } while (0)
#define TRACELOG_IORQ_MARK_HANDLED()      do { } while (0)
#endif

uint8_t port_read_cb(z80_t *cpu, uint16_t addr, void *user_data)
{
    (void)cpu;
    (void)user_data;

    uint8_t retval;
    uint8_t port_lsb = addr & 0xff;

    mzarch_main_insideop_iorq();

    switch (port_lsb)
    {

    case 0x50:
    case 0x51:
        /* cteme Unicard */
        if (UNICARD_TEST_IS_CONNECTED)
        {
            retval = unicard_read_byte(addr);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;

#if CFG_HWEXT_HAVE_RAMDISK
    case 0x68:
    case 0x69:
    case 0x6a:
    case 0x6b:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
        /* cteme posunuty Pezik: 0x68 - 0x6f */
        if (g_ramdisk.pezik[RAMDISK_PEZIK_68].connected)
        {
            retval = ramdisk_pezik_read_byte(addr);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_IDE8
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7b:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
        /* cteme IDE8: 0x78 - 0x7f */
        if (IDE8_TEST_CONNECTED)
        {
            retval = ide8_read_byte(port_lsb & 0x07);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

    /* MZ-700: porty 0xCE (DMD status), 0xD0-0xD3 (PIO8255), 0xD4-0xD6 (CTC8253)
     * jsou pristupne pouze pres MEMOP (0xE000-0xE008), ne pres IORQ */

#if CFG_HWEXT_HAVE_FDC
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
        /* cteme do WDC: 0xd8 - 0xdf */
        if (g_fdc[FDC0].connected)
        {
            fdc_read_byte(&g_fdc[FDC0], port_lsb, &retval);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_FDC
    /* FDC1 (sekundární) - chip data porty 0x58 - 0x5b. */
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
        if (g_fdc[FDC1].connected)
        {
            fdc_read_byte(&g_fdc[FDC1], port_lsb, &retval);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

    /* MZ-700: porty 0xE0-0xE1 (memory mapper PREAD) neexistuji */

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xea:
        /* cteme std ramdisk: 0xea */
        if (g_ramdisk.std.connected)
        {
            retval = ramdisk_std_read_byte(port_lsb);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_QDISK
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
        /* cteme QDISC: 0xf4 - 0xf7 */
        if (g_qdisk.connected)
        {
            retval = qdisk_read_byte(port_lsb & 0x03);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xf8:
    case 0xf9:
        /* cteme standardni ramdisk */
        if (g_ramdisk.std.connected)
        {
            retval = ramdisk_std_read_byte(port_lsb);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;
#endif

#if HAVE_PIOZ80
    case 0xfc:
    case 0xfd:
    case 0xfe:
    case 0xff:
        /* cteme z PIOZ80: 0xfc - 0xff */
        retval = pioz80_read_byte(port_lsb & 0x03);
        break;
#endif

    default:
        // pri cteni neobsazeneho portu vracime posledni byte, ktery byl na sbernici
        // printf("%s() - Unsupported port: 0x%04x, 0x%02x\n", __func__, addr, port_lsb);
        retval = g_mzarch_main.regDBUS_latch;
        TRACELOG_IORQ_MARK_UNCONNECTED();
        break;
    };

    return retval;
}

void port_write_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    (void)cpu;
    (void)user_data;

    uint8_t port_lsb = addr & 0xff;

    mzarch_main_insideop_iorq();

    /* trace-suite: pre-set "unconnected" - viz mz800_iorq.c port_write_cb. */
    TRACELOG_IORQ_MARK_UNCONNECTED();

    switch (port_lsb)
    {
    case 0x01:
        cmthack_load_file();
        TRACELOG_IORQ_MARK_HANDLED();
        break;

    case 0x02:
        cmthack_read_mzf_body();
        TRACELOG_IORQ_MARK_HANDLED();
        break;

    case 0x50:
    case 0x51:
        /* zapisujeme do Unicard */
        if (UNICARD_TEST_IS_CONNECTED)
        {
            unicard_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;

#if CFG_HWEXT_HAVE_RAMDISK
    case 0x68:
    case 0x69:
    case 0x6a:
    case 0x6b:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
        /* zapisujeme na posunuty Pezik: 0x68 - 0x6f */
        if (g_ramdisk.pezik[RAMDISK_PEZIK_68].connected)
        {
            ramdisk_pezik_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_IDE8
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7b:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
        /* zapisujeme na IDE8: 0x78 - 0x7f */
        if (IDE8_TEST_CONNECTED)
        {
            ide8_write_byte(port_lsb & 0x07, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

    /* MZ-700: porty 0xCC-0xCF (VRAM radič, HW scroll, border) neexistuji */
    /* MZ-700: porty 0xD0-0xD3 (PIO8255), 0xD4-0xD7 (CTC8253) jsou pres MEMOP, ne IORQ */

#if CFG_HWEXT_HAVE_FDC
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
        /* zapisujeme do WDC: 0xd8 - 0xdf */
        if (g_fdc[FDC0].connected)
        {
            fdc_write_byte(&g_fdc[FDC0], port_lsb, &value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_FDC
    /* FDC1 (sekundární) - porty 0x58 - 0x5f. */
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:
        if (g_fdc[FDC1].connected)
        {
            fdc_write_byte(&g_fdc[FDC1], port_lsb, &value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
        /* zapisujeme na memory mapper: 0xe0 - 0xe6 */
        memory_map_pwrite(port_lsb, value);
        TRACELOG_IORQ_MARK_HANDLED();
        break;

    case 0xe7:
        /* zapisujeme na memext mapper: 0xe7 */
        if (MEMEXT_TEST_CONNECTED)
        {
            memext_map_pwrite(((addr >> 12) & 0x0f), value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;


#if CFG_HWEXT_HAVE_QDISK
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
        /* zapisujeme do QDISC: 0xf4 - 0xf7 */
        if (g_qdisk.connected)
        {
            qdisk_write_byte(port_lsb & 0x03, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xfa:
        /* zapisujeme na std ramdisk: 0xfa */
        if (g_ramdisk.std.connected)
        {
            ramdisk_std_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

#if HAVE_PIOZ80
    case 0xfc:
    case 0xfd:
    case 0xfe:
    case 0xff:
        /* zapisujeme do PIOZ80: 0xfc - 0xff */
        pioz80_write_byte(port_lsb & 0x03, value);
        TRACELOG_IORQ_MARK_HANDLED();
        break;
#endif

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xea:
    case 0xeb:
        /* zapisujeme na std ramdisk: 0xea - 0xeb */
        if (g_ramdisk.std.connected)
        {
            ramdisk_std_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

    default:
        // printf("%s() - Unsupported port: 0x%04x, 0x%02x\n", __func__, addr, port_lsb);
        /* Default = ghost. */
        break;
    }
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "debugger/bptmap.h"
#include "debugger/breakpoints.h"
#include "debugger/io_activity.h"
#include "debugger/io_history.h"
#include "debugger/trace/eventlog.h"
/* iorqlog.h includován výše bezpodmínečně. */

/**
 * Variant port_read_cb s CDL recording + iorqlog.
 * Aktivuje se přes z80_set_pread() při zapnutí debuggeru.
 */
uint8_t port_read_with_logging_cb(z80_t *cpu, uint16_t addr, void *user_data)
{
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        mhmap_inc(MHMAP_REGION_IORQ_8BIT, addr & 0xff, MHMAP_ACCESS_R);
    };
    g_tracelog_iorq_unconnected = 0;
    g_dbg_in_cpu_path = 1;
    uint8_t value = port_read_cb(cpu, addr, user_data);
    g_dbg_in_cpu_path = 0;
    if (TEST_TRACE_IORQLOG_ACTIVE) {
        uint16_t bc = z80_get_reg(cpu, Z80_REG_BC);
        en_IORQLOG_EVENT_TYPE etype = g_tracelog_iorq_unconnected
            ? IORQLOG_EVENT_IORQ_UNCONNECTED
            : IORQLOG_EVENT_IORQ;
        iorqlog_record(etype, IORQLOG_DIR_IN, bc, addr, value, 0u);
    }
    /* I/O Ports panel activity tracking + history - gated default OFF. */
    if ( g_io_window_tracking_active ) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit ( addr, value,
                                  true /* is_in */, frame_n );
        io_history_record ( true /* is_in */, addr, value,
                            g_mzarch_main.instruction_addr,
                            frame_n,
                            (uint16_t) g_gdg.beam_row,
                            (uint16_t) VIDEO_GET_SCREEN_COL (
                                g_gdg.total_elapsed.ticks ),
                            g_mzarch_main.cpu->total_cycles );
    }
    /* event-viewer Vlna 1: paralelní fan-out IORQ_IN do eventlog ringu.
     * Subtype rozlišuje NORMAL vs UNCONNECTED ghost (viz mz800_iorq.c
     * pro detailní popis g_tracelog_iorq_unconnected pre/post stavu). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_IORQ_IN ) ) ) {
        uint8_t sub = g_tracelog_iorq_unconnected
                          ? EVENTLOG_IORQ_SUB_UNCONNECTED
                          : EVENTLOG_IORQ_SUB_NORMAL;
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_IORQ_IN, sub,
                          g_mzarch_main.instruction_addr, pl );
    }
    /* D.2 IORQ_R BP hook - viz mz800_iorq.c. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_R ] ) {
        breakpoints_enforce_iorq_r ( addr, value );
    }
    return value;
}


/**
 * Variant port_write_cb s Memory Heatmap recording + iorqlog.
 */
void port_write_with_logging_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        mhmap_inc(MHMAP_REGION_IORQ_8BIT, addr & 0xff, MHMAP_ACCESS_W);
    };
    g_tracelog_iorq_unconnected = 0;
    g_dbg_in_cpu_path = 1;
    port_write_cb(cpu, addr, value, user_data);
    g_dbg_in_cpu_path = 0;
    if (TEST_TRACE_IORQLOG_ACTIVE) {
        uint16_t bc = z80_get_reg(cpu, Z80_REG_BC);
        en_IORQLOG_EVENT_TYPE etype = g_tracelog_iorq_unconnected
            ? IORQLOG_EVENT_IORQ_UNCONNECTED
            : IORQLOG_EVENT_IORQ;
        iorqlog_record(etype, IORQLOG_DIR_OUT, bc, addr, value, 0u);
    }
    /* I/O Ports panel activity tracking + history - gated default OFF. */
    if ( g_io_window_tracking_active ) {
        uint32_t frame_n = (uint32_t) g_gdg.total_elapsed.screens;
        io_activity_record_hit ( addr, value,
                                  false /* is_in */, frame_n );
        io_history_record ( false /* is_in */, addr, value,
                            g_mzarch_main.instruction_addr,
                            frame_n,
                            (uint16_t) g_gdg.beam_row,
                            (uint16_t) VIDEO_GET_SCREEN_COL (
                                g_gdg.total_elapsed.ticks ),
                            g_mzarch_main.cpu->total_cycles );
    }
    /* event-viewer Vlna 1: paralelní fan-out IORQ_OUT do eventlog ringu.
     * Subtype rozlišuje NORMAL vs UNCONNECTED (viz mz800_iorq.c). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_IORQ_OUT ) ) ) {
        uint8_t sub = g_tracelog_iorq_unconnected
                          ? EVENTLOG_IORQ_SUB_UNCONNECTED
                          : EVENTLOG_IORQ_SUB_NORMAL;
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_IORQ_OUT, sub,
                          g_mzarch_main.instruction_addr, pl );
    }
    /* D.2 IORQ_W BP hook - viz mz800_iorq.c. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_W ] ) {
        breakpoints_enforce_iorq_w ( addr, value );
    }

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Mutant mcp-server V1.A.5: io_write event emit guarded subscriberem
     * (viz mz800_iorq.c port_write_with_logging_cb). */
    if ( event_bus_has_subscriber ( "io_write" ) ) {
        JsonObject *payload = json_object_new ( );
        json_object_set_int_member ( payload, "port",   (gint64) addr );
        json_object_set_int_member ( payload, "value",  (gint64) value );
        json_object_set_int_member ( payload, "cycles",
            (gint64) g_mzarch_main.cpu->total_cycles );
        event_bus_emit ( "io_write", payload );
    }
#endif
}


/**
 * @brief Side-effect-free probe variant pro IORQ read (MZ-700).
 *
 * V1.7+ CHECKLIST 3.4: implementace přes mirror gettery z `g_io_ports[]`
 * (= `io_catalog_probe_byte` dispatch). io_catalog.c je per-arch
 * kompilované, takže pro MZ-700 binárku obsahuje jen MZ_AVAIL_700
 * entries. Mirror callbacky (PPI Port B + CW, CTC counter + CW,
 * WD279x R/W latches, banking 0xE0-E6) vrací konzistentní hodnoty
 * s Overview I/O Ports panelem. MZ-700 nemá joystick na 0xF0/F1 ani
 * Z80 PIO - tyto entries v g_io_ports nejsou (MZ_AVAIL_800/1500 only),
 * helper je nenajde a vrátí false → latch fallback.
 *
 * Pozn: tato funkce **NEvolá** `mzarch_main_insideop_iorq()` ani
 * trace-suite hooky - probe nesmi mutovat zadny global state.
 */
uint8_t port_read_no_se_cb(uint16_t addr)
{
    uint8_t v;
    if (io_catalog_probe_byte((uint8_t)(addr & 0xFFu), &v))
        return v;
    return g_mzarch_main.regDBUS_latch;
}
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
