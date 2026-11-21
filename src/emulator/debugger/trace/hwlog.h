/**
 * @file   hwlog.h
 * @brief  HW Log - per-chip register/state changes.
 *
 * Subsystém trace-suite. Loguje všechny změny stavu HW registrů,
 * dělíček, vstupů/výstupů. Per-chip a per-event-type stratifikace.
 *
 * Per-event záznam (24 B fixed):
 *
 * | Offset | Velikost | Pole                                  |
 * |--------|----------|----------------------------------------|
 * | 0      | 8 B      | pxclk_total                            |
 * | 8      | 4 B      | screens_total                          |
 * | 12     | 4 B      | pxclk_in_screen                        |
 * | 16     | 1 B      | chip_id (GDG_*, PIO8255, CTC8253, ...) |
 * | 17     | 1 B      | sub_event_type (per chip-specific)     |
 * | 18     | 6 B      | payload (chip-specific)                |
 *
 * Detail chip-specific payloadu: viz `docs/cz/debugger/formats/HW-log_format.md`.
 *
 * @section v1_scope V1 scope
 *
 * V1 implementuje hooky pro:
 *  - GDG_MODE (DMD register write)
 *  - GDG_BANKING (memory map / port E0-E4 OUT)
 *  - GDG_COLORS (PALGRP, PAL0-3, BORDER OUT)
 *  - GDG_HWSCROLL (CRTC scroll register write)
 *  - PIO8255 (port write events)
 *  - CTC8253 (port write events)
 *  - PSG (data write events)
 *  - PIOZ80 (write/read/IRQ ack/RETI/bus input change)
 *
 * NEDOPLNĚNO ve V1:
 *  - WFRF, GDG_VIDEO (sync events)
 *  - QD, FDC, MEMEXT, RD per-chip detail (rezervovány enum hodnoty)
 *
 * Header obsahuje initial state všech podporovaných chipů jako binární
 * snapshot v `<dir>/<name>_initial_state.bin`.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef HWLOG_H
#define HWLOG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "tlog_common.h"

typedef enum en_HWLOG_CHIP
{
    HWLOG_CHIP_GDG_MODE     = 0x01,    /**< DMD, mode změna */
    HWLOG_CHIP_GDG_BANKING  = 0x02,    /**< Memory map / port E0-E6 OUT */
    HWLOG_CHIP_GDG_HWSCROLL = 0x03,    /**< Hardware scroll register */
    HWLOG_CHIP_GDG_COLORS   = 0x04,    /**< Paleta / PCG / border */
    HWLOG_CHIP_GDG_WFRF     = 0x05,    /**< Write Format / Read Format register */
    HWLOG_CHIP_GDG_VIDEO    = 0x06,    /**< HS, VS, HBLN, VBLN sync events */
    HWLOG_CHIP_PIO8255      = 0x10,    /**< 8255 PPI write */
    HWLOG_CHIP_CTC8253      = 0x11,    /**< 8253 CTC write */
    HWLOG_CHIP_PIOZ80       = 0x12,    /**< Z80 PIO state events (write/read/IRQ ack/RETI/bus) */
    HWLOG_CHIP_PSG          = 0x20,    /**< SN76489 PSG write */
    HWLOG_CHIP_QD           = 0x30,    /**< Quick Disk */
    HWLOG_CHIP_FDC          = 0x31,    /**< WD279x FDC */
    HWLOG_CHIP_MEMEXT       = 0x40,    /**< Memext bank switch */
    HWLOG_CHIP_RD           = 0x41,    /**< Ramdisk operations */
} en_HWLOG_CHIP;

/* Sub-event types pro GDG_COLORS chip */
typedef enum en_HWLOG_GDG_COLORS_SUB
{
    HWLOG_GDG_COLORS_BORDER       = 0x01,
    HWLOG_GDG_COLORS_PALGRP       = 0x02,
    HWLOG_GDG_COLORS_PAL          = 0x03,    /**< payload[0] = PAL index (0-3), payload[1] = value */
    HWLOG_GDG_COLORS_PCG          = 0x04,
    HWLOG_GDG_COLORS_PACKETGROUP  = 0x05,
} en_HWLOG_GDG_COLORS_SUB;

/**
 * Sub-event types pro GDG_VIDEO chip (raster sync events).
 *
 * VBLN/VS edges = ~200 events/sec při 50 fps - emit vždy.
 *
 * HBLN/HS edges = ~31000 events/sec - emit pouze pokud je nastaveno
 * `g_hwlog_config.hs_decimation > 0` (= každý N-tý edge). Default 0 =
 * vypnuto (viz `--hwlog-hs-decimation N` CLI flag). Decimace globální
 * pro HS i HBLN dohromady (jeden counter).
 */
typedef enum en_HWLOG_GDG_VIDEO_SUB
{
    HWLOG_GDG_VIDEO_VBLN_START = 0x01,    /**< vertical blanking start */
    HWLOG_GDG_VIDEO_VBLN_END   = 0x02,    /**< vertical blanking end */
    HWLOG_GDG_VIDEO_VS_START   = 0x03,    /**< STS_VSYNC start */
    HWLOG_GDG_VIDEO_VS_END     = 0x04,    /**< STS_VSYNC end */
    HWLOG_GDG_VIDEO_HBLN_START = 0x05,    /**< horizontal blanking start (decim) */
    HWLOG_GDG_VIDEO_HBLN_END   = 0x06,    /**< horizontal blanking end (decim) */
    HWLOG_GDG_VIDEO_HS_START   = 0x07,    /**< STS_HSYNC start (decim) */
    HWLOG_GDG_VIDEO_HS_END     = 0x08,    /**< STS_HSYNC end (decim) */
    HWLOG_GDG_VIDEO_CKSW       = 0x09,    /**< zápis bitu CKSW (port 0x07CF); payload[0..1] = adresa, payload[2] = zapsaný bajt (bit 7 = CKSW) */
} en_HWLOG_GDG_VIDEO_SUB;

/**
 * Sub-event types pro PSG chip.
 *
 * REGISTER_WRITE = klasický write byte do PSG datového portu (jeden 8b
 * registr SN76489AN, dekóduje se interně přes latch). Externí parser
 * získá kompletní stav z (initial state v hwlog headeru) + sumace všech
 * REGISTER_WRITE eventů.
 */
typedef enum en_HWLOG_PSG_SUB
{
    HWLOG_PSG_REGISTER_WRITE = 0x01,    /**< write byte do PSG datového portu */
} en_HWLOG_PSG_SUB;

/**
 * Sub-event types pro CTC8253 chip.
 *
 * CONTROL_WRITE = write do CW registru (port 0x03 / D7-D6 = chip select,
 *                 D5-D4 = RLF, D3-D1 = MODE, D0 = BCD).
 * COUNTER_WRITE = write do datového counteru (port 0x00..0x02 = CTC0..CTC2).
 *                 Pořadí LSB/MSB se interně ukládá do rl_byte.
 */
typedef enum en_HWLOG_CTC8253_SUB
{
    HWLOG_CTC8253_CONTROL_WRITE = 0x01,
    HWLOG_CTC8253_COUNTER_WRITE = 0x02,
} en_HWLOG_CTC8253_SUB;

/**
 * Sub-event types pro PIO8255 chip.
 *
 * Per-port write events. Sub-event type přesně mapuje na write addr
 * (0..3) v 8255. CONTROL_WRITE = write do CW registru (addr 3) =
 * konfigurace mode + směr portů + bit set/reset.
 */
typedef enum en_HWLOG_PIO8255_SUB
{
    HWLOG_PIO8255_PORT_A_WRITE  = 0x01,
    HWLOG_PIO8255_PORT_B_WRITE  = 0x02,
    HWLOG_PIO8255_PORT_C_WRITE  = 0x03,
    HWLOG_PIO8255_CONTROL_WRITE = 0x04,
} en_HWLOG_PIO8255_SUB;

/**
 * Sub-event types pro GDG_BANKING chip.
 *
 * Logicky řízeno GDG, ale fyzicky se přepínání map dělá v memory.c.
 * Sub-event = low byte portu pro snadnou identifikaci (E0..E6).
 *
 * MZ-800 sémantika:
 *   E0 = ROM bottom OFF      (= map VRAM/CGRAM dolů)
 *   E1 = ROM upper OFF
 *   E2 = ROM 0000 ON         (= reset map)
 *   E3 = ROM upper ON
 *   E4 = ALL ON              (= reset all)
 *   E5/E6 = EXROM (ignorováno na mz800)
 *
 * MZ-1500 sémantika:
 *   E0..E4 stejné jako mz800
 *   E5 = SPEC ON s value
 *   E6 = SPEC OFF
 */
typedef enum en_HWLOG_GDG_BANKING_SUB
{
    HWLOG_GDG_BANKING_E0 = 0xE0,
    HWLOG_GDG_BANKING_E1 = 0xE1,
    HWLOG_GDG_BANKING_E2 = 0xE2,
    HWLOG_GDG_BANKING_E3 = 0xE3,
    HWLOG_GDG_BANKING_E4 = 0xE4,
    HWLOG_GDG_BANKING_E5 = 0xE5,
    HWLOG_GDG_BANKING_E6 = 0xE6,
} en_HWLOG_GDG_BANKING_SUB;

/**
 * Sub-event types pro QD (Quick Disk) chip.
 *
 * Generický REGISTER_WRITE event - addr (= SIO addr 0..3 = data A,
 * data B, ctrl A, ctrl B) v payload. Decoded SIO command stream
 * dopočítá externí parser.
 */
typedef enum en_HWLOG_QD_SUB
{
    HWLOG_QD_REGISTER_WRITE = 0x01,
} en_HWLOG_QD_SUB;

/**
 * Sub-event types pro FDC (WD279x) chip.
 *
 * REGISTER_WRITE: generický write na BUS sběrnici FDC. Addr (0..3 =
 *   command/status, track, sector, data) v payload[0], raw byte (před
 *   BUS xlate inverzí) v payload[1]. Decoded WD279x command + execution
 *   dopočítá externí parser. Sharp invertuje data (= dataset INVERT_DATA),
 *   to je kontrola fyzické úrovně bytu na sběrnici.
 *
 * COMMAND_ISSUED: dekódovaný command dispatch (= zápis na FDCPORT_CMDSTS
 *   po BUS xlate inverzi). Komplementární k REGISTER_WRITE - vždy dvojice
 *   událostí při command write (= REG_W raw byte + CMD dekódovaný typ).
 *   Payload layout (6 B):
 *     [0] = en_WD279X_COMMAND_TYPE hodnota (dekódovaný typ)
 *     [1] = SIDE (0/1, z g_fdc.chip.SIDE / fdc_get_side())
 *     [2] = Track registr (regTRACK, true-bus)
 *     [3] = Sector registr (regSECTOR, true-bus)
 *     [4] = Command flags (= dolní 4 bity raw cmd byte = side/multi/update bits)
 *     [5] = Raw command byte (true-bus, po inverzi)
 *   Pouze prvních 4 B se zaokrouhleně přenese do eventlog ringu (= UI Detail);
 *   bajty 4..5 (flags + raw) jsou dostupné jen v hwlog disk chunku pro
 *   off-line parsery. UI dekóduje typ + side + T + S z primárních 4 B.
 */
typedef enum en_HWLOG_FDC_SUB
{
    HWLOG_FDC_REGISTER_WRITE = 0x01, /**< Raw BUS write byte. */
    HWLOG_FDC_COMMAND_ISSUED = 0x02, /**< Dekódovaný command dispatch. */
} en_HWLOG_FDC_SUB;

/**
 * Sub-event types pro RD (Ramdisk - Pezik a STD).
 *
 * STD_WRITE: write do std ramdisku (port 0xFA na MZ-800).
 * PEZIK_WRITE: write do Pezik ramdisku (porty 0xE8, 0xEC..0xEF).
 */
typedef enum en_HWLOG_RD_SUB
{
    HWLOG_RD_STD_WRITE   = 0x01,
    HWLOG_RD_PEZIK_WRITE = 0x02,
} en_HWLOG_RD_SUB;

/**
 * Sub-event types pro MEMEXT chip.
 *
 * BANK_SWITCH = write do mapovacího portu Memextu (0xE0..0xE4 v MZ-800).
 *               payload obsahuje address point (= bus page, 0..15) + nový
 *               raw bank value zapsaný uživatelem.
 */
typedef enum en_HWLOG_MEMEXT_SUB
{
    HWLOG_MEMEXT_BANK_SWITCH = 0x01,
} en_HWLOG_MEMEXT_SUB;

/**
 * Sub-event types pro PIOZ80 chip.
 *
 * Plnohodnotný stavový pohled na Z80 PIO. Logujeme:
 *  - každou změnu řídícího slova podle jeho významu (MODE, VECTOR,
 *    ICW, MASK, IO_SELECT)
 *  - data write a read na portech 0FEh, 0FFh
 *  - externí změnu vstupních pinů (CTC0 do PA4, VBLN do PA5, RESET)
 *  - IM 2 IRQ ack a RETI dokončení
 *
 * Payload (6 B) per všechny sub-events ve formátu:
 *  - [0] port_id (0 = A, 1 = B, 0xFF = N/A pro globální events)
 *  - [1] addr nebo sub_addr (per sub-event - typ. PIO addr 0..3 pro
 *        data a control writes, vector pro IRQ_ACK_M2)
 *  - [2] value (raw byte zapsaný, přečtený nebo úroveň pinu)
 *  - [3..5] decoded_state_delta_bitmask (24 b LE) - viz dokumentace
 *        v docs/cz/debugger/formats/HW-log_format.md, sekce PIOZ80
 */
typedef enum en_HWLOG_PIOZ80_SUB
{
    HWLOG_PIOZ80_MODE_WRITE       = 0x01,    /**< zápis Mode Control Word */
    HWLOG_PIOZ80_VECTOR_WRITE     = 0x02,    /**< zápis Interrupt Vector */
    HWLOG_PIOZ80_INT_CTRL_WRITE   = 0x03,    /**< zápis Interrupt Control word (ICW) */
    HWLOG_PIOZ80_MASK_WRITE       = 0x04,    /**< zápis Mask Follows (po ICW s MF=1) */
    HWLOG_PIOZ80_IO_SELECT_WRITE  = 0x05,    /**< zápis I/O Select Mask (po Mode 3) */
    HWLOG_PIOZ80_DATA_WRITE       = 0x06,    /**< OUT na data port */
    HWLOG_PIOZ80_DATA_READ        = 0x07,    /**< IN z data portu */
    HWLOG_PIOZ80_BUS_INPUT_CHANGE = 0x08,    /**< externí změna PA/PB pinů */
    HWLOG_PIOZ80_IRQ_ACK_M2       = 0x09,    /**< M2 IORQ INTA cycle (vector vrácen) */
    HWLOG_PIOZ80_RETI_APPLIED     = 0x0A,    /**< RETI prošlo PIO daisy chain */
} en_HWLOG_PIOZ80_SUB;

/**
 * Bity v decoded_state_delta_bitmask (3 B = 24 b LE) v PIOZ80 payloadu
 * pole [3..5]. Indikují co se v rámci eventu reálně změnilo / co je nová
 * hodnota interního stavu po eventu. Externí parser podle baseline
 * (initial_state PIOZ80 TLV) + sumace bitů rekonstruuje stav v libovolném
 * čase.
 *
 * Bit význam je per sub-event mírně specifický - viz HW-log_format_CZ.md.
 */
#define HWLOG_PIOZ80_DELTA_MODE        (1u << 0)   /**< port->mode změněn */
#define HWLOG_PIOZ80_DELTA_IO_MASK     (1u << 1)   /**< port->io_mask změněn */
#define HWLOG_PIOZ80_DELTA_ICMASK      (1u << 2)   /**< port->icmask změněn */
#define HWLOG_PIOZ80_DELTA_ICENA       (1u << 3)   /**< port->icena změněn */
#define HWLOG_PIOZ80_DELTA_ICFNC       (1u << 4)   /**< port->icfnc změněn */
#define HWLOG_PIOZ80_DELTA_ICLVL       (1u << 5)   /**< port->iclvl změněn */
#define HWLOG_PIOZ80_DELTA_VECTOR      (1u << 6)   /**< port->interrupt_vector změněn */
#define HWLOG_PIOZ80_DELTA_DATA_OUT    (1u << 7)   /**< port->data_output změněn */
#define HWLOG_PIOZ80_DELTA_PORT_INT    (1u << 8)   /**< port->port_int změněn */
#define HWLOG_PIOZ80_DELTA_MASKED_IN   (1u << 9)   /**< port->masked_input změněn */
#define HWLOG_PIOZ80_DELTA_INT_GLOBAL  (1u << 10)  /**< g_pioz80.interrupt změněn */
#define HWLOG_PIOZ80_DELTA_INT_PORT_ID (1u << 11)  /**< g_pioz80.interrupt_port_id změněn */
#define HWLOG_PIOZ80_DELTA_CTRL_EXPECT (1u << 12)  /**< port->ctrl_expect změněn */

typedef struct st_HWLOG_CONFIG
{
    en_TLOG_MODE mode;
    char *dir;
    char *name;
    unsigned chunk_mb;
    unsigned max_total_mb;
    unsigned save_on_exit;

    /**
     * @brief Decimace HS/HBLN GDG_VIDEO sub-events.
     *
     * 0 = HS i HBLN edges se NElogují (default - bezpečný kvůli per-frame
     * frequency ~31000 events/sec).
     * N > 0 = každý N-tý edge se emituje (counter modulo N == 0).
     *
     * Jeden sdílený counter pro všechny 4 sub-events
     * (HS_START/HS_END/HBLN_START/HBLN_END), tj. dekade napříč typy.
     */
    unsigned hs_decimation;
} st_HWLOG_CONFIG;

extern st_HWLOG_CONFIG g_hwlog_config;
extern int g_hwlog_active;

/* Forward decl pro fan-out gate (= druhý sink přes eventlog ring).
 * Bez externí deklarace by makro @ref TEST_TRACE_HWLOG_DISPATCH nešlo
 * přeložit, pokud se eventlog.h nečte jako celek - držíme extra
 * forward decl pro robustnost. */
extern int g_eventlog_active;

/**
 * @brief Gate pro čistě hwlog disk recording.
 *
 * Pravdivé, pouze pokud běží klasická trace-suite disk persistence
 * (= @c tlog_writer_append() do souboru). Použít na call-sites, které
 * testují vlastní hwlog tlog writer stav (např. interní state machine
 * v @c hwlog.c, podmínky odvozené od @c s_writer_open). NEPOUŽÍVAT
 * jako gate před voláním @c hwlog_record() z chip driverů - tam patří
 * @ref TEST_TRACE_HWLOG_DISPATCH.
 *
 * Default OFF stav: 1 load + 1 branch.
 */
#define TEST_TRACE_HWLOG_ACTIVE (g_hwlog_active)

/**
 * @brief Gate pro DISPATCH volání @c hwlog_record() z chip driverů.
 *
 * Pravdivé, pokud alespoň jeden ze sinků konzumuje hwlog event:
 *
 *  - @c g_hwlog_active   = klasická trace-suite disk persistence
 *  - @c g_eventlog_active = in-memory ring pro Event Viewer UI
 *
 * Sémantika: "má smysl vůbec volat @c hwlog_record(), protože alespoň
 * jeden sink jej využije". Vnitřek @c hwlog_record() pak po vlastních
 * pod-gate odbočí do tlog writer / eventlog ring nezávisle.
 *
 * Toto je makro, které patří na všechna call-sites v chip driverech,
 * která chystají payload a volají @c hwlog_record() / @c hwlog_record_byte()
 * / @c hwlog_record_2byte().
 *
 * Default OFF stav (oba flagy = 0): 1 load + 1 OR + 1 branch.
 */
#define TEST_TRACE_HWLOG_DISPATCH (g_hwlog_active || g_eventlog_active)

void hwlog_init ( void );
void hwlog_apply_cli_options ( void );
void hwlog_recompute_active ( int debugger_active );
int  hwlog_start ( void );
void hwlog_stop ( void );
void hwlog_finalize ( void );

/**
 * @brief Uzavřít (uložit) aktuální segment, volitelně přesměrovat na @p path.
 *
 * Runtime finalizace streamovaného hwlog segmentu (analogie
 * @ref cputrack_save_segment). Při zadaném @p path přesměruje @c dir/name na
 * cestu pro NÁSLEDNÝ segment; již zapsané chunk soubory nepřejmenovává.
 *
 * @param path  Cílová cesta dalšího segmentu, nebo NULL (jen flush+restart).
 * @return 0 OK, -1 chyba.
 */
int hwlog_save_segment ( const char *path );

/**
 * @brief Test, zda byl recording zastaven kvůli max_total_mb limitu.
 *
 * Public wrapper nad @ref tlog_writer_is_truncated() pro interní
 * @c s_writer instanci tohoto subsystému. Použití typ. v UI status
 * indicators (např. titulek SDL okna).
 *
 * @return 1 = truncated (recording stopnut), 0 = stále aktivní recording
 *         nebo neaktivní subsystém.
 */
int hwlog_is_truncated ( void );

/**
 * @brief Generický emit per HW event.
 *
 * @param chip       chip_id
 * @param sub        sub-event type (per chip-specific)
 * @param payload    6-byte payload (caller plní per chip-specific layout)
 */
void hwlog_record ( en_HWLOG_CHIP chip, uint8_t sub, const uint8_t payload[ 6 ] );

/**
 * @brief Convenience wrapper - 1 B payload (byte v register write).
 */
void hwlog_record_byte ( en_HWLOG_CHIP chip, uint8_t sub, uint8_t value );

/**
 * @brief Convenience wrapper - 2 B payload (typ. port + value).
 */
void hwlog_record_2byte ( en_HWLOG_CHIP chip, uint8_t sub, uint8_t a, uint8_t b );

/**
 * @brief Emit GDG_VIDEO HS/HBLN edge s decimační logikou.
 *
 * Volat z gdg event kódu při každé HBLN/HS rising/falling. Funkce sama
 * uvnitř udržuje counter a porovnává s `g_hwlog_config.hs_decimation`:
 *
 *  - hs_decimation == 0 => no-op (default, plná frekvence ~31000/sec
 *    by zaplnila chunk za sekundy + zničila real-time perf)
 *  - hs_decimation == N => emit jen každý N-tý event (counter % N == 0)
 *
 * Counter je sdílený pro všechny 4 sub-events (HS_START/HS_END/HBLN_START/
 * HBLN_END) - decimace napříč typy.
 *
 * Předpoklad: caller už zkontroloval @c TEST_TRACE_HWLOG_DISPATCH (= jinak
 * by ani toto volání nemělo proběhnout).
 *
 * @param sub  Jeden z HWLOG_GDG_VIDEO_HBLN_START / _HBLN_END / _HS_START
 *             / _HS_END.
 */
void hwlog_record_gdg_video_hs_edge ( uint8_t sub );

/**
 * @brief Writer kanálu hwlog pro čtení proudu záznamů (trace_read).
 *
 * Vrací writer i po stopu (popis posledního uzavřeného segmentu), viz
 * tlog_writer_read_snapshot() a devdoc/trace/stream-read.md.
 *
 * @return Ukazatel na statický writer modulu (nikdy NULL).
 * @pre Snímek writeru pořizovat jen z emu vlákna; tlog_writer_segment()
 *      z libovolného vlákna.
 */
const st_TLOG_WRITER *hwlog_get_writer ( void );

#ifdef __cplusplus
}
#endif

#endif /* HWLOG_H */
