#include "main.h"
#include <stdio.h>
#include <stdint.h>

#include "mzarch/mzarch_config.h"
#include "libs/cpu-z80/z80.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/psg/psg.h"

#if HAVE_PIOZ80
#include "hw-generic/pioz80/pioz80.h"
#endif

#include "hw-generic/joy/joy.h"

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

// debug
#include "hw-generic/gdg/gdg.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/iorqlog.h"
#include "debugger/io_catalog.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
#include <json-glib/json-glib.h>
#include "mcp/event_bus.h"
#endif
#else
/* No-op definice pro release/non-debug build - flag g_tracelog_iorq_unconnected
 * v non-debug kompilaci neexistuje. */
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

    case 0xce:
        /* cteme DMD status: 0xce */
        //            printf ("sts: 0x%02x\n", g_mzarch_main.cpu->pc );
        // nyni uz mame overeno, ze cteni z 0xce se chova stejne v 700 i 800 modu
        retval = gdg_read_dmd_status_ioop();
        break;

    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
        /* cteme z PIO8255: 0xd0 - 0xd3  */
        if (!GDG_DMD_TEST_MODE700)
        {
            retval = pio8255_read(port_lsb & 0x03);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;

    case 0xd4:
    case 0xd5:
    case 0xd6:
        /* cteme z CTC8253: 0xd4 - 0xd6, 0xd7 cist nelze */
        if (!GDG_DMD_TEST_MODE700)
        {
            retval = ctc8253_read_byte(port_lsb & 0x03);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;

#if CFG_HWEXT_HAVE_FDC
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
        /* cteme do WDC: 0xd8 - 0xdf */
        if (g_fdc[FDC0].connected)
        {
            fdc_read_byte(&g_fdc[FDC0], port_lsb, &retval);
            // printf("%s() - FDC addr: 0x%02x, value: 0x%02x, regPC: 0x%04x, gdg_screens: %u, gdg_px: %u\n", __func__, addr&0xff, retval, g_mzarch_main.cpu->pc, g_gdg.total_elapsed.screens, g_gdg.total_elapsed.ticks);
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

    case 0xe0:
    case 0xe1:
        /* cteme memory mapper: 0xe0 - 0xe1
         * Memory mapper je write-only - read vrací bus latch (= ghost). */
        memory_map_pread(port_lsb);
        retval = g_mzarch_main.regDBUS_latch;
        TRACELOG_IORQ_MARK_UNCONNECTED();
        break;

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xe8:
    case 0xe9:
    case 0xeb:
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef:
        /* cteme Pezik: 0xe8 - 0xef */
        if (g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected)
        {
            retval = ramdisk_pezik_read_byte(addr);
        }
        else
        {
            retval = g_mzarch_main.regDBUS_latch;
            TRACELOG_IORQ_MARK_UNCONNECTED();
        };
        break;

    case 0xea:
        /* pezik, nebo std ramdisk */
        if (g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected)
        {
            retval = ramdisk_pezik_read_byte(addr);
        }
        else if (g_ramdisk.std.connected)
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

    case 0xf0:
        /* cteme JOY1 */
        if (g_pio8255.signal_PA_joy1_enabled)
        {
            retval = joy_read_byte(JOY_DEVID_0);
        }
        else
        {
            retval = 0xff;
        };
        break;

    case 0xf1:
        /* cteme JOY2 */
        if (g_pio8255.signal_PA_joy2_enabled)
        {
            retval = joy_read_byte(JOY_DEVID_1);
        }
        else
        {
            retval = 0xff;
        };
        break;

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

    /* trace-suite: pre-set "unconnected" - každá větev v switch která chip
     * skutečně obslouží explicitně zavolá TRACELOG_IORQ_MARK_HANDLED().
     * Default-case + "if (X.connected) {} bez else" zůstávají = 1 = ghost. */
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

    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf:
    case 0xf0:
        /* zapisujeme do GDG: 0xcc - 0xcf, 0xf0 */
        gdg_write_byte(addr, value);
        TRACELOG_IORQ_MARK_HANDLED();
        break;

    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
        /* zapisujeme do PIO8255: 0xd0 - 0xd3 */
        if (!GDG_DMD_TEST_MODE700)
        {
            pio8255_write(port_lsb & 0x03, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        /* MZ-700 mode: PIO mapped jen na E000-E008, IORQ D0-D3 = ghost. */
        break;

    case 0xd4:
    case 0xd5:
    case 0xd6:
    case 0xd7:
        /* zapisujeme do CTC8253: 0xd4 - 0xd7 */
        if (!GDG_DMD_TEST_MODE700)
        {
            ctc8253_write_byte(port_lsb & 0x03, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        /* MZ-700 mode: CTC mapped jen na E004-E007, IORQ D4-D7 = ghost. */
        break;

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
            // printf("%s() - FDC addr: 0x%02x, value: 0x%02x, regPC: 0x%04x, gdg_screens: %u, gdg_px: %u\n", __func__, addr&0xff, value, g_mzarch_main.cpu->pc, g_gdg.total_elapsed.screens, g_gdg.total_elapsed.ticks);
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

#if CFG_HWEXT_HAVE_RAMDISK
    case 0xe8:
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef:
        /* zapisujeme na Pezik: 0xe8, 0xec - 0xef */
        if (g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected)
        {
            ramdisk_pezik_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

    case 0xf2:
        psg_write_byte(PSG_CH_RIGHT | PSG_CH_LEFT, value);   /* MZ-800: mono, channel se ignoruje; nebo oba kanály, pokud je povoleno stereo */
        TRACELOG_IORQ_MARK_HANDLED();
        break;

    case 0xf3:
        /* Levý PSG — pouze pokud je povoleno stereo (HW compatibility experiment) */
        if (g_psg_module.stereo)
        {
            psg_write_byte(PSG_CH_LEFT, value);
            TRACELOG_IORQ_MARK_HANDLED();
        }
        break;

    case 0xf9:
        /* Pravý PSG — pouze pokud je povoleno stereo (HW compatibility experiment) */
        if (g_psg_module.stereo)
        {
            psg_write_byte(PSG_CH_RIGHT, value);
            TRACELOG_IORQ_MARK_HANDLED();
        }
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
    case 0xe9:
    case 0xea:
    case 0xeb:
        if (g_ramdisk.pezik[RAMDISK_PEZIK_E8].connected)
        {
            /* zapisujeme na Pezik: 0xe9 - 0xeb */
            ramdisk_pezik_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        }
        else if (g_ramdisk.std.connected)
        {
            /* zapisujeme na Pezik: 0xe9 - 0xeb */
            ramdisk_std_write_byte(addr, value);
            TRACELOG_IORQ_MARK_HANDLED();
        };
        break;
#endif

    default:
        // printf("%s() - Unsupported port: 0x%04x, 0x%02x\n", __func__, addr, port_lsb);
        /* Default = ghost (žádný chip neobsloužil) - flag zůstává 1. */
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
/* iorqlog.h je už includován výše bezpodmínečně kvůli TRACELOG_IORQ_MARK_UNCONNECTED. */

/**
 * Variant port_read_cb s CDL recording.
 *
 * Aktivuje se přes z80_set_pread() při zapnutí debuggeru. Hot path bez
 * debuggeru zůstává nedotčen (běží port_read_cb).
 *
 * @param cpu
 * @param addr  Plná 16-bit port adresa (low byte = port, high byte = obsah B
 *              registru pro IN r,(C) / akumulátor pro IN A,(n)).
 * @param user_data
 * @return Hodnota přečtená z portu.
 */
uint8_t port_read_with_logging_cb(z80_t *cpu, uint16_t addr, void *user_data)
{
    /*
     * Memory Heatmap recording do iorq-8bit mapy. GDG sub-funkce přes high
     * byte (16-bit IORQ) zatím neloguje - layout iorq-gdg.mh se upřesní později.
     */
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        mhmap_inc(MHMAP_REGION_IORQ_8BIT, addr & 0xff, MHMAP_ACCESS_R);
    };

    /* Flag pro detekci ghost-bus reads. port_read_cb nastaví na 1 pokud
     * cílový port není v aktuální HW konfiguraci osazený / mapovaný. */
    g_tracelog_iorq_unconnected = 0;
    /* Indikace CPU debug cesty pro filtraci v MREQ/IORQ hooks (sdílí s mhmap). */
    g_dbg_in_cpu_path = 1;
    uint8_t value = port_read_cb(cpu, addr, user_data);
    g_dbg_in_cpu_path = 0;

    /* trace-suite iorqlog: zaznamenat I/O event PO čtení (známe value
     * i ghost-bus flag). source_addr = aktuální BC registr (= IN A,(C)
     * source). Event type se rozhodne podle g_tracelog_iorq_unconnected. */
    if (TEST_TRACE_IORQLOG_ACTIVE) {
        uint16_t bc = z80_get_reg(cpu, Z80_REG_BC);
        en_IORQLOG_EVENT_TYPE etype = g_tracelog_iorq_unconnected
            ? IORQLOG_EVENT_IORQ_UNCONNECTED
            : IORQLOG_EVENT_IORQ;
        iorqlog_record(etype, IORQLOG_DIR_IN, bc, addr, value, 0u);
    }

    /* I/O Ports panel activity tracking + history - gated default OFF
     * (zero overhead). Flag aktivovan jen pri otevrenem panelu. */
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
     * Nezávislé na io_window_tracking - umožní UI Event Viewer fungovat
     * i s vypnutým I/O Ports panelem. Subtype rozlišuje NORMAL vs
     * UNCONNECTED ghost (g_tracelog_iorq_unconnected byl nastaven /
     * resetnut PŘED voláním port_read_cb a má aktuální hodnotu PO
     * návratu z callbacku). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_IORQ_IN ) ) ) {
        uint8_t sub = g_tracelog_iorq_unconnected
                          ? EVENTLOG_IORQ_SUB_UNCONNECTED
                          : EVENTLOG_IORQ_SUB_NORMAL;
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_IORQ_IN, sub,
                          g_mzarch_main.instruction_addr, pl );
    }

    /* D.2 IORQ_R BP hook - matchuje na low byte (= 8-bit IO addr). */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_R ] ) {
        breakpoints_enforce_iorq_r ( addr, value );
    }

    return value;
}


/**
 * Variant port_write_cb s Memory Heatmap recording.
 *
 * @param cpu
 * @param addr  Plná 16-bit port adresa.
 * @param value Zapisovaná hodnota.
 * @param user_data
 */
void port_write_with_logging_cb(z80_t *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    if (TEST_DEBUGGER_MHMAP_ACTIVE) {
        mhmap_inc(MHMAP_REGION_IORQ_8BIT, addr & 0xff, MHMAP_ACCESS_W);
    };

    /* Flag pro detekci ghost-bus writes - port_write_cb pre-set 1 v entry
     * a každá handled větev nuluje. Inverzní polarita oproti read side
     * (write nemá natural marker jako regDBUS_latch read). */
    g_tracelog_iorq_unconnected = 0;
    g_dbg_in_cpu_path = 1;
    port_write_cb(cpu, addr, value, user_data);
    g_dbg_in_cpu_path = 0;

    /* trace-suite iorqlog: zaznamenat OUT event PO write (známe ghost flag). */
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
     * Subtype rozlišuje NORMAL vs UNCONNECTED - port_write_cb pre-set
     * g_tracelog_iorq_unconnected=1 a každá handled větev jej nuluje
     * (= inverzní polarita oproti read side, viz komentář u flagu). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_IORQ_OUT ) ) ) {
        uint8_t sub = g_tracelog_iorq_unconnected
                          ? EVENTLOG_IORQ_SUB_UNCONNECTED
                          : EVENTLOG_IORQ_SUB_NORMAL;
        uint32_t pl = (uint32_t) addr | ( (uint32_t) value << 16 );
        eventlog_record ( EVENTLOG_CAT_IORQ_OUT, sub,
                          g_mzarch_main.instruction_addr, pl );
    }

    /* D.2 IORQ_W BP hook. */
    if ( g_bptmap.per_type_active[ BPTMAP_IDX_IORQ_W ] ) {
        breakpoints_enforce_iorq_w ( addr, value );
    }

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* Mutant mcp-server V1.A.5: io_write event emit guarded subscriberem.
     * Bez subscribera (= žádný MCP klient na topicu "io_write") hot path
     * jen načte 1 atomic + branch. Subscriber existuje => sestavíme
     * JsonObject s portem, hodnotou a T-state counterem a emitujeme. */
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
 * @brief Side-effect-free probe variant pro IORQ read.
 *
 * V1.6+ TODO 4.2 5b / V1.7+ CHECKLIST 3.4: implementace přes mirror
 * gettery z `g_io_ports[]` (= `io_catalog_probe_byte` dispatch). Pro
 * každý port se zkusí najít entry s registrovaným `read_value` mirror
 * callbackem (= side-effect free čtení internal chip state). Pokud
 * není nalezen (port write-only bez mirror, např. 0xDB FDC data, nebo
 * unconnected), fallback `regDBUS_latch` jako "bus ghost".
 *
 * Po dokončení 2.2 mirrors mají všechny core chipy (PPI 8255, CTC 8253,
 * GDG status, FDC 0xD8-DA + 0xDC-DF, banking 0xE0-E6, Z80 PIO 0xFC/FD,
 * JOY 0xF0/F1) registrované mirror callbacky v io_catalog. Probe path
 * tedy vrací konzistentní hodnoty s Overview I/O Ports panelem (=
 * `port[N]` v BP DSL ukáže to samé co Overview zobrazí na řádku
 * portu N).
 *
 * Pozn: tato funkce **NEvolá** `mzarch_main_insideop_iorq()` ani
 * trace-suite hooky - probe nesmi mutovat zadny global state vcetne
 * IORQ event counter. Mirror callbacky v io_catalog jsou designem
 * side-effect-free (per `read_value` doc v io_catalog.h).
 */
uint8_t port_read_no_se_cb(uint16_t addr)
{
    uint8_t v;
    if (io_catalog_probe_byte((uint8_t)(addr & 0xFFu), &v))
        return v;
    return g_mzarch_main.regDBUS_latch;
}
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
