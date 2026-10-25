/*
 * test_eventlog_decoder.c - unit testy pro rich Detail decoder
 *                          (Vlna 3 Commit 21).
 *
 * Testuje per-kategorie decode helpers v debugger/eventlog_decoder.c.
 * Sanity check pattern: zostrojit event s definovaným payloadem, zavolat
 * eventlog_decode_detail(), ověřit že výstupní string obsahuje očekávané
 * substring (= robustní vůči drobným formátovacím změnám).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <stdio.h>
#include <string.h>

#include "debugger/eventlog_decoder.h"
#include "debugger/trace/eventlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/marklog.h"


void setUp ( void )
{
    /* Decoder je čistě analytická vrstva nad ringem - nepotřebuje init
     * eventlog ringu. marklog_register() lazy ensure_registry() inicializuje
     * potřebné struktury při prvním volání, nevolat marklog_init() (vyžaduje
     * cfg propagate a v izolovaném unit testu by zaassertovalo). */
}


void tearDown ( void )
{
}


/* ================================================================
 * Helper: postaví event a zavolá decoder.
 * ================================================================ */

static void run_decode ( uint8_t category, uint8_t subtype, uint16_t pc,
                          uint32_t payload, char *out, size_t out_len )
{
    st_EVENTLOG_EVENT e = { 0 };
    e.category = category;
    e.subtype  = subtype;
    e.pc       = pc;
    e.payload  = payload;
    eventlog_decode_detail ( &e, out, out_len );
}


/* ================================================================
 * CPU_INT state bitmask
 * ================================================================ */

void test_eventlog_detail_cpu_int_bitmask ( void )
{
    char buf[ 128 ];
    /* IM2 + IFF1 + IFF2 = "IM=2 IFF1=1 IFF2=1". */
    uint32_t bits = INTLOG_STATE_BIT_IM2 | INTLOG_STATE_BIT_IFF1
                  | INTLOG_STATE_BIT_IFF2;
    run_decode ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, bits, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IM=2" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IFF1=1" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IFF2=1" ) );

    /* Přidat RETI bit -> string obsahuje " RETI". */
    bits |= INTLOG_STATE_BIT_RETI;
    run_decode ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, bits, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "RETI" ) );

    /* EI bit -> " EI" (a NE "DI"). */
    run_decode ( EVENTLOG_CAT_CPU_INT, 0, 0x4000,
                 INTLOG_STATE_BIT_IM2 | INTLOG_STATE_BIT_EI,
                 buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "EI" ) );
    TEST_ASSERT_NULL ( strstr ( buf, "DI" ) );

    /* DI bit -> " DI". Regrese: DI se dříve do Events vůbec nelogovalo
     * (mzarch_di_cb nevolal intlog) a decoder neměl DI příznak. */
    run_decode ( EVENTLOG_CAT_CPU_INT, 0, 0x4000,
                 INTLOG_STATE_BIT_IM1 | INTLOG_STATE_BIT_DI,
                 buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "DI" ) );
}


/* ================================================================
 * CPU_PIN_EDGE source + edge
 * ================================================================ */

void test_eventlog_detail_cpu_pin_edge ( void )
{
    char buf[ 128 ];
    /* PIOZ80@PA4 rising edge: subtype = source, payload[1] = edge. */
    uint32_t payload = (uint32_t) 4u | ( (uint32_t) INTLOG_EDGE_RISING << 8 );
    run_decode ( EVENTLOG_CAT_CPU_PIN_EDGE, INTLOG_CHIP_PIOZ80_PA4, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "PIOZ80@PA4" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "rising" ) );

    /* CTC2 falling. */
    payload = (uint32_t) 0u | ( (uint32_t) INTLOG_EDGE_FALLING << 8 );
    run_decode ( EVENTLOG_CAT_CPU_PIN_EDGE, INTLOG_CHIP_CTC2, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "CTC2" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "falling" ) );
}


/* ================================================================
 * IRQ_ACK_IM2 vector + ISR
 * ================================================================ */

void test_eventlog_detail_irq_ack_im2 ( void )
{
    char buf[ 128 ];
    /* PIOZ80_PORT_A, vector=0x40, isr=0x4042. */
    uint32_t payload = (uint32_t) 0x0040u | ( (uint32_t) 0x4042u << 16 );
    run_decode ( EVENTLOG_CAT_IRQ_ACK_IM2, INTLOG_CHIP_PIOZ80_PORT_A, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "PIOZ80_PA" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "vec=0x0040" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "isr=0x4042" ) );
}


/* ================================================================
 * IORQ decode + port name lookup
 * ================================================================ */

void test_eventlog_detail_iorq_decode ( void )
{
    char buf[ 128 ];
    /* port=0xCE val=0x08 (= GDG DMD write).
     * Port lookup: io_catalog má 0xCE entry "GDG DMD" (write). */
    uint32_t payload = (uint32_t) 0x00CEu | ( (uint32_t) 0x08u << 16 );
    run_decode ( EVENTLOG_CAT_IORQ_OUT, EVENTLOG_IORQ_SUB_NORMAL, 0x4000,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "port=0x00CE" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "val=0x08" ) );
    /* Name match nepovinný - io_catalog má GDG entry, ale test by neměl
     * být citlivý na konkrétní wording katalogu. Stačí že port + value
     * jsou ve výstupu. */
}


void test_eventlog_detail_iorq_unconnected ( void )
{
    char buf[ 128 ];
    uint32_t payload = (uint32_t) 0x0099u | ( (uint32_t) 0xFFu << 16 );
    run_decode ( EVENTLOG_CAT_IORQ_IN, EVENTLOG_IORQ_SUB_UNCONNECTED, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "unconnected" ) );
}


/* ================================================================
 * MMIO decode
 * ================================================================ */

void test_eventlog_detail_mmio_decode ( void )
{
    char buf[ 128 ];
    uint32_t payload = (uint32_t) 0xE003u | ( (uint32_t) 0x80u << 16 );
    run_decode ( EVENTLOG_CAT_MMIO_W, 0, 0, payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "addr=0xE003" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "val=0x80" ) );
}


/* ================================================================
 * GDG_MODE -> DMD interpretace
 * ================================================================ */

void test_eventlog_detail_gdg_mode_decode ( void )
{
    char buf[ 128 ];
    /* DMD=0x02 (= screen=00 320x200, col=10 16-barev) -> "320x200x16".
     * Payload [0]=addr low (0xCE), [1]=addr high (0), [2]=DMD value. */
    uint32_t payload = (uint32_t) 0xCEu
                     | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x02u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_MODE, 0, 0, payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "DMD=0x02" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "320x200x16" ) );

    /* DMD=0x04 (screen=01 640x200, col=00 2-barev) -> "640x200x2". */
    payload = (uint32_t) 0xCEu | ( (uint32_t) 0x00u << 8 )
            | ( (uint32_t) 0x04u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_MODE, 0, 0, payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "640x200x2" ) );

    /* DMD=0x08 (screen=10 MZ-700 mode). */
    payload = (uint32_t) 0xCEu | ( (uint32_t) 0x00u << 8 )
            | ( (uint32_t) 0x08u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_MODE, 0, 0, payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "MZ-700" ) );
}


/**
 * @brief Popisek všech 16 hodnot DMD podle báze (mz-800.txt, hw/09a):
 *        bit 0 = obraz A/B, bit 1 = HICOLOR, bit 2 = 640, bit 3 = MZ-700
 *        (bity 0-2 pak bez účinku); 03h a 07h nedokumentované.
 */
void test_eventlog_detail_gdg_mode_all_values ( void )
{
    static const char *const c_expected[ 16 ] = {
        "320x200x4 A", "320x200x4 B", "320x200x16", "320x200x16 undoc",
        "640x200x2 A", "640x200x2 B", "640x200x4", "640x200x4 undoc",
        "MZ-700 mode", "MZ-700 mode (undoc)", "MZ-700 mode (undoc)",
        "MZ-700 mode (undoc)", "MZ-700 mode (undoc)", "MZ-700 mode (undoc)",
        "MZ-700 mode (undoc)", "MZ-700 mode (undoc)"
    };
    char buf[ 128 ];
    char expected[ 64 ];
    for ( unsigned dmd = 0; dmd < 16; dmd++ ) {
        uint32_t payload = (uint32_t) 0xCEu | ( (uint32_t) dmd << 16 );
        run_decode ( EVENTLOG_CAT_GDG_MODE, 0, 0, payload, buf, sizeof ( buf ) );
        snprintf ( expected, sizeof ( expected ), "DMD=0x%02X (%s)", dmd, c_expected[ dmd ] );
        TEST_ASSERT_EQUAL_STRING ( expected, buf );
    }
}


/* ================================================================
 * GDG_BANKING (port E0..E6)
 * ================================================================ */

void test_eventlog_detail_gdg_banking_decode ( void )
{
    char buf[ 128 ];
    /* subtype = E0 (= ROM bottom OFF). Payload [0]=port low, [1]=value. */
    uint32_t payload = (uint32_t) 0xE0u | ( (uint32_t) 0x00u << 8 );
    run_decode ( EVENTLOG_CAT_GDG_BANKING, HWLOG_GDG_BANKING_E0, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0xE0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "ROM bottom OFF" ) );
}


/* ================================================================
 * GDG_COLORS (BORDER / PAL[idx])
 * ================================================================ */

void test_eventlog_detail_gdg_colors_decode ( void )
{
    char buf[ 128 ];
    /* BORDER=0xE0. Payload [0]=addr low (0xCF), [1]=addr high (6), [2]=val. */
    uint32_t payload = (uint32_t) 0xCFu | ( (uint32_t) 0x06u << 8 )
                     | ( (uint32_t) 0xE0u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_COLORS, HWLOG_GDG_COLORS_BORDER, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "BORDER=0xE0" ) );

    /* PAL: value 0x9F (idx=3, col=9). */
    payload = (uint32_t) 0xF0u | ( (uint32_t) 0x00u << 8 )
            | ( (uint32_t) 0x9Fu << 16 );
    run_decode ( EVENTLOG_CAT_GDG_COLORS, HWLOG_GDG_COLORS_PAL, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "PAL" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0x9F" ) );
}


/* ================================================================
 * PIO8255 - port write + CW decode
 * ================================================================ */

void test_eventlog_detail_pio8255_decode ( void )
{
    char buf[ 128 ];
    /* Port A=0x42. Payload [0]=addr (0), [1]=value. */
    uint32_t payload = (uint32_t) 0x00u | ( (uint32_t) 0x42u << 8 );
    run_decode ( EVENTLOG_CAT_PIO8255, HWLOG_PIO8255_PORT_A_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Port A=0x42" ) );

    /* CW=0x80 (mode set bit + mode 0 + all outputs). */
    payload = (uint32_t) 0x03u | ( (uint32_t) 0x80u << 8 );
    run_decode ( EVENTLOG_CAT_PIO8255, HWLOG_PIO8255_CONTROL_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "CW=0x80" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "mode set" ) );

    /* CW = 0x05 (PC2 set bit). */
    payload = (uint32_t) 0x03u | ( (uint32_t) 0x05u << 8 );
    run_decode ( EVENTLOG_CAT_PIO8255, HWLOG_PIO8255_CONTROL_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "PC2 set" ) );
}


/* ================================================================
 * CTC8253 - CW decode + counter write
 * ================================================================ */

void test_eventlog_detail_ctc8253_cw_decode ( void )
{
    char buf[ 128 ];
    /* CW = 0xB6: SC=2 (counter 2), RW=11 (LSB+MSB), MODE=011 (square wave),
     *           BCD=0.
     * 0xB6 = 1011_0110. */
    uint32_t payload = (uint32_t) 0x03u | ( (uint32_t) 0xB6u << 8 );
    run_decode ( EVENTLOG_CAT_CTC8253, HWLOG_CTC8253_CONTROL_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "CW=0xB6" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "cnt 2" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "square wave" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "BIN" ) );

    /* Counter 1 write 0x42. Payload [0]=addr (1), [1]=value. */
    payload = (uint32_t) 0x01u | ( (uint32_t) 0x42u << 8 );
    run_decode ( EVENTLOG_CAT_CTC8253, HWLOG_CTC8253_COUNTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "CTC1=0x42" ) );
}


/* ================================================================
 * PIOZ80 mode + ICW decode
 * ================================================================ */

void test_eventlog_detail_pioz80_decode ( void )
{
    char buf[ 128 ];
    /* Port A, MODE write, mode bits = 10 (= bidir, value 0x80=10000000b).
     * Payload [0]=port_id (0=A), [1]=sub_addr, [2]=value. */
    uint32_t payload = (uint32_t) 0x00u | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x80u << 16 );
    run_decode ( EVENTLOG_CAT_PIOZ80, HWLOG_PIOZ80_MODE_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "A MODE" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "bidir" ) );

    /* ICW = 0xB7 (EI=1, mode=0 OR, lvl=1 high, MF=1, pattern 0111). */
    payload = (uint32_t) 0x01u | ( (uint32_t) 0x00u << 8 )
            | ( (uint32_t) 0xB7u << 16 );
    run_decode ( EVENTLOG_CAT_PIOZ80, HWLOG_PIOZ80_INT_CTRL_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "B ICW" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "EI=1" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "MF=1" ) );
}


/* ================================================================
 * PSG register decode (latch + data)
 * ================================================================ */

void test_eventlog_detail_psg_decode ( void )
{
    char buf[ 128 ];
    /* Latch byte 0x80 = 1000_0000: ch 0 (A), type 0 (period). */
    uint32_t payload = (uint32_t) 0x80u | ( (uint32_t) 0x01u << 8 )
                     | ( (uint32_t) 0x00u << 16 );  /* mono */
    run_decode ( EVENTLOG_CAT_PSG, HWLOG_PSG_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0x80" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "ch A" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "period" ) );

    /* Latch attn = 0x90 (ch A, type 1 attn). */
    payload = (uint32_t) 0x90u | ( (uint32_t) 0x01u << 8 );
    run_decode ( EVENTLOG_CAT_PSG, HWLOG_PSG_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "attn" ) );

    /* Data byte (bit 7 = 0) = period hi. */
    payload = (uint32_t) 0x12u | ( (uint32_t) 0x01u << 8 );
    run_decode ( EVENTLOG_CAT_PSG, HWLOG_PSG_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "period hi" ) );
}


/* ================================================================
 * FDC command opcode decode
 * ================================================================ */

void test_eventlog_detail_fdc_command_decode ( void )
{
    char buf[ 128 ];
    /* CMD register (addr 0) write 0x00 = Restore (top nibble 0). */
    uint32_t payload = (uint32_t) 0x00u | ( (uint32_t) 0x00u << 8 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "CMD" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Restore" ) );

    /* CMD = 0x80 = Read Sector. */
    payload = (uint32_t) 0x00u | ( (uint32_t) 0x80u << 8 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Read Sector" ) );

    /* CMD = 0xF0 = Write Track. */
    payload = (uint32_t) 0x00u | ( (uint32_t) 0xF0u << 8 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Write Track" ) );

    /* TRACK register (addr 1) write 0x05 - bez command interpretace. */
    payload = (uint32_t) 0x01u | ( (uint32_t) 0x05u << 8 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "TRACK" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0x05" ) );
    /* Pro non-CMD reg žádné "Restore"-like keyword nesmí být. */
    TEST_ASSERT_NULL ( strstr ( buf, "Restore" ) );
}


/* ================================================================
 * FDC COMMAND_ISSUED dekódovaný command dispatch (Vlna 5 Commit 30)
 * ================================================================ */

void test_eventlog_detail_fdc_command_issued_restore ( void )
{
    char buf[ 128 ];
    /* cmd_type = WD279X_CMD_RESTORE (0x00), side=0, T=0, S=0. */
    uint32_t payload = (uint32_t) 0x00u
                     | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x00u << 16 )
                     | ( (uint32_t) 0x00u << 24 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_COMMAND_ISSUED, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Restore" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "side=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "T=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "S=0" ) );
}


void test_eventlog_detail_fdc_command_issued_read_sector ( void )
{
    char buf[ 128 ];
    /* cmd_type = WD279X_CMD_READ_SECTOR (0x08), side=0, T=10, S=5. */
    uint32_t payload = (uint32_t) 0x08u
                     | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x0Au << 16 )
                     | ( (uint32_t) 0x05u << 24 );
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_COMMAND_ISSUED, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Read Sector" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "T=10" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "S=5" ) );
}


void test_eventlog_detail_fdc_command_issued_unknown ( void )
{
    char buf[ 128 ];
    /* cmd_type = WD279X_CMD_UNKNOWN (0xFF). */
    uint32_t payload = (uint32_t) 0xFFu;
    run_decode ( EVENTLOG_CAT_FDC, HWLOG_FDC_COMMAND_ISSUED, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Unknown" ) );
}


void test_eventlog_subtype_fdc_command_short ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_FDC,
                                     HWLOG_FDC_COMMAND_ISSUED,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "CMD", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_FDC,
                                     HWLOG_FDC_REGISTER_WRITE,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "REG_W", buf );
}


void test_eventlog_subtype_fdc_command_full ( void )
{
    char buf[ 64 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_FDC,
                                    HWLOG_FDC_COMMAND_ISSUED,
                                    buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "WD279x command dispatch" ) );

    eventlog_decode_subtype_full ( EVENTLOG_CAT_FDC,
                                    HWLOG_FDC_REGISTER_WRITE,
                                    buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "register write" ) );
}


/* ================================================================
 * MEMEXT bank switch
 * ================================================================ */

void test_eventlog_detail_memext_decode ( void )
{
    char buf[ 128 ];
    /* page=4, bank=0x12, type=0 (Luftner). */
    uint32_t payload = (uint32_t) 0x04u | ( (uint32_t) 0x12u << 8 )
                     | ( (uint32_t) 0x00u << 16 );
    run_decode ( EVENTLOG_CAT_MEMEXT, HWLOG_MEMEXT_BANK_SWITCH, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "page=4" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "bank=0x12" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Luftner" ) );

    /* type=1 (Pehu). */
    payload = (uint32_t) 0x04u | ( (uint32_t) 0x12u << 8 )
            | ( (uint32_t) 0x01u << 16 );
    run_decode ( EVENTLOG_CAT_MEMEXT, HWLOG_MEMEXT_BANK_SWITCH, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Pehu" ) );
}


/* ================================================================
 * BP_FIRE - reason + action label
 * ================================================================ */

void test_eventlog_detail_bp_fire_decode ( void )
{
    char buf[ 128 ];
    /* bp_id=5, reason=2, subtype=MARK. */
    uint32_t payload = (uint32_t) 5u | ( (uint32_t) 2u << 16 );
    run_decode ( EVENTLOG_CAT_BP_FIRE, EVENTLOG_BP_FIRE_SUB_MARK, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "BP #5" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "reason=2" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "MARK" ) );
}


/* ================================================================
 * USER_MARK - marker name lookup
 * ================================================================ */

void test_eventlog_detail_user_mark_decode ( void )
{
    char buf[ 128 ];
    /* Registrovat marker s daným jménem, pak ho dekódovat z payloadu. */
    uint16_t mid = marklog_register ( "isr_entry" );
    TEST_ASSERT_TRUE ( mid != (uint16_t) 0xFFFF );

    run_decode ( EVENTLOG_CAT_USER_MARK, 0, 0x4000,
                  (uint32_t) mid, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "isr_entry" ) );

    /* Neregistrované id -> "marker_id=N" fallback. */
    run_decode ( EVENTLOG_CAT_USER_MARK, 0, 0,
                  0x5555u, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "marker_id=" ) );
}


/* ================================================================
 * CPU_CTRL - HALT + RST
 * ================================================================ */

void test_eventlog_detail_cpu_ctrl_decode ( void )
{
    char buf[ 128 ];
    run_decode ( EVENTLOG_CAT_CPU_CTRL, EVENTLOG_CPU_CTRL_SUB_HALT_ENTER,
                  0, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "HALT enter" ) );

    run_decode ( EVENTLOG_CAT_CPU_CTRL, EVENTLOG_CPU_CTRL_SUB_RST_38,
                  0, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "RST 0x38" ) );
}


/* ================================================================
 * GDG_VIDEO subtype label
 * ================================================================ */

void test_eventlog_detail_gdg_video_decode ( void )
{
    char buf[ 128 ];
    run_decode ( EVENTLOG_CAT_GDG_VIDEO, HWLOG_GDG_VIDEO_VBLN_START,
                  0, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "VBLN" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "start" ) );

    run_decode ( EVENTLOG_CAT_GDG_VIDEO, HWLOG_GDG_VIDEO_VS_END,
                  0, 0, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "VS" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "end" ) );
}


/* ================================================================
 * GDG_HWSCROLL register naming
 * ================================================================ */

void test_eventlog_detail_gdg_hwscroll_decode ( void )
{
    char buf[ 128 ];
    /* SOF1 (subtype=0), value=0x40. */
    uint32_t payload = (uint32_t) 0xCFu | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x40u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_HWSCROLL, 0, 0, payload,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "SOF1=0x40" ) );

    /* SSA (subtype=4), value=0x12. */
    payload = (uint32_t) 0xCFu | ( (uint32_t) 0x04u << 8 )
            | ( (uint32_t) 0x12u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_HWSCROLL, 4, 0, payload,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "SSA=0x12" ) );
}


/* ================================================================
 * GDG_WFRF (WF mode decode)
 * ================================================================ */

void test_eventlog_detail_gdg_wfrf_decode ( void )
{
    char buf[ 128 ];
    /* WF (subtype=0), value=0x80 = 1000_0000 (mode=100=XOR). */
    uint32_t payload = (uint32_t) 0xCCu | ( (uint32_t) 0x00u << 8 )
                     | ( (uint32_t) 0x80u << 16 );
    run_decode ( EVENTLOG_CAT_GDG_WFRF, 0, 0, payload,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "WF=0x80" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "XOR" ) );

    /* RF (subtype=1), value=0xFF. */
    payload = (uint32_t) 0xCDu | ( (uint32_t) 0x00u << 8 )
            | ( (uint32_t) 0xFFu << 16 );
    run_decode ( EVENTLOG_CAT_GDG_WFRF, 1, 0, payload,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "RF=0xFF" ) );
}


/* ================================================================
 * QD a RD
 * ================================================================ */

void test_eventlog_detail_qd_decode ( void )
{
    char buf[ 128 ];
    /* ctrl A (addr 2), value 0x18. */
    uint32_t payload = (uint32_t) 0x02u | ( (uint32_t) 0x18u << 8 );
    run_decode ( EVENTLOG_CAT_QD, HWLOG_QD_REGISTER_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "ctrl A" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0x18" ) );
}


void test_eventlog_detail_rd_decode ( void )
{
    char buf[ 128 ];
    /* STD write port=0xFA val=0x42. */
    uint32_t payload = (uint32_t) 0xFAu | ( (uint32_t) 0x42u << 8 );
    run_decode ( EVENTLOG_CAT_RD, HWLOG_RD_STD_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "STD" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0xFA" ) );

    /* Pezik. */
    payload = (uint32_t) 0xECu | ( (uint32_t) 0xABu << 8 );
    run_decode ( EVENTLOG_CAT_RD, HWLOG_RD_PEZIK_WRITE, 0,
                  payload, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "Pezik" ) );
}


/* ================================================================
 * Unknown / out-of-range fallback
 * ================================================================ */

void test_eventlog_detail_unknown_payload ( void )
{
    char buf[ 128 ];
    /* Kategorie mimo definovaný rozsah -> fallback raw payload. */
    run_decode ( (uint8_t) ( EVENTLOG_CAT_COUNT + 1 ), 0, 0,
                  0xDEADBEEFu, buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "payload=" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "DEADBEEF" ) );
}


void test_eventlog_detail_null_safety ( void )
{
    char buf[ 16 ];
    /* NULL event -> prázdný string. */
    eventlog_decode_detail ( NULL, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_CHAR ( '\0', buf[ 0 ] );

    /* NULL buf -> no crash. */
    st_EVENTLOG_EVENT e = { 0 };
    eventlog_decode_detail ( &e, NULL, 16 );

    /* buf_len = 0 -> no crash. */
    eventlog_decode_detail ( &e, buf, 0 );
}


/* ================================================================
 * Subtype short / full decoder (Vlna 3 Commit 22)
 * ================================================================ */

/**
 * @brief IORQ NORMAL -> "NORMAL", UNCONNECTED -> "UNCONN".
 */
void test_subtype_short_iorq_normal ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_IORQ_IN,
                                     EVENTLOG_IORQ_SUB_NORMAL,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "NORMAL", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_IORQ_OUT,
                                     EVENTLOG_IORQ_SUB_UNCONNECTED,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "UNCONN", buf );
}


/**
 * @brief PIOZ80 BUS_INPUT_CHANGE -> "BUSIN" (= žádné PIN tooltip
 *        ovlivnění).
 */
void test_subtype_short_pioz80_bus_in ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_PIOZ80,
                                     HWLOG_PIOZ80_BUS_INPUT_CHANGE,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "BUSIN", buf );

    /* Vrácení RETI z PIO daisy chainu. */
    eventlog_decode_subtype_short ( EVENTLOG_CAT_PIOZ80,
                                     HWLOG_PIOZ80_RETI_APPLIED,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "RETI", buf );
}


/**
 * @brief GDG_VIDEO HBLN_START -> "HBLN_S" (= 6 znaků <= 8 limit).
 */
void test_subtype_short_gdg_video_hbln_s ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_GDG_VIDEO,
                                     HWLOG_GDG_VIDEO_HBLN_START,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "HBLN_S", buf );
}


/**
 * @brief GDG_BANKING port E0 -> "E0", E4 -> "E4" (= jedno-znaková
 *        kompresia hex digitu po prefixu).
 */
void test_subtype_short_gdg_banking_port ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_GDG_BANKING,
                                     HWLOG_GDG_BANKING_E0,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "E0", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_GDG_BANKING,
                                     HWLOG_GDG_BANKING_E4,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "E4", buf );
}


/**
 * @brief Kategorie bez smysluplného subtype (= GDG_MODE/MMIO_R/W/WFRF)
 *        dostávají "-".
 */
void test_subtype_short_no_subtype_dash ( void )
{
    char buf[ 16 ];
    eventlog_decode_subtype_short ( EVENTLOG_CAT_GDG_MODE, 0,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_MMIO_W, 0,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_GDG_WFRF, 0,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "-", buf );
}


/**
 * @brief Plný text PIOZ80 IRQ_ACK_M2 začíná "IM 2 IRQ acknowledge".
 */
void test_subtype_full_pioz80_irq_ack ( void )
{
    char buf[ 96 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_PIOZ80,
                                    HWLOG_PIOZ80_IRQ_ACK_M2,
                                    buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IM 2 IRQ acknowledge" ) );
}


/**
 * @brief Plný text GDG_BANKING E2 obsahuje "ROM 0000 ON" popis.
 */
void test_subtype_full_gdg_banking_e2 ( void )
{
    char buf[ 96 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_GDG_BANKING,
                                    HWLOG_GDG_BANKING_E2,
                                    buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "0xE2" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "ROM 0000 ON" ) );
}


/**
 * @brief Plný text CPU_CTRL RST_38 obsahuje "RST 38h".
 */
void test_subtype_full_cpu_ctrl_rst38 ( void )
{
    char buf[ 96 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_CPU_CTRL,
                                    EVENTLOG_CPU_CTRL_SUB_RST_38,
                                    buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "RST 38h" ) );
}


/* ================================================================
 * Vlna 5 Commit 31 - SYS kategorie (decoder + short/full)
 * ================================================================ */

void test_decoder_sys_cold_reset ( void )
{
    char buf[ 96 ];
    run_decode ( EVENTLOG_CAT_SYS, EVENTLOG_SYS_COLD_RESET, 0, 0u,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "cold reset" ) );
}


void test_decoder_sys_snapshot_save ( void )
{
    char buf[ 96 ];
    run_decode ( EVENTLOG_CAT_SYS, EVENTLOG_SYS_SNAPSHOT_SAVE, 0, 0xDEADBEEFu,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "snapshot save" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "DEADBEEF" ) );
}


void test_decoder_sys_mzf_inject ( void )
{
    char buf[ 96 ];
    run_decode ( EVENTLOG_CAT_SYS, EVENTLOG_SYS_MZF_INJECT, 0, 0x12345678u,
                  buf, sizeof ( buf ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "MZF inject" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "12345678" ) );
}


void test_subtype_sys_short_long ( void )
{
    char buf[ 32 ];

    /* Short forms. */
    eventlog_decode_subtype_short ( EVENTLOG_CAT_SYS,
                                     EVENTLOG_SYS_COLD_RESET,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "COLD_RST", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_SYS,
                                     EVENTLOG_SYS_SNAPSHOT_LOAD,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "SNAP_LD", buf );

    eventlog_decode_subtype_short ( EVENTLOG_CAT_SYS,
                                     EVENTLOG_SYS_MZF_INJECT,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "MZF_IN", buf );

    /* Full forms - musí obsahovat smysluplný popisek. */
    char full[ 96 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_SYS,
                                    EVENTLOG_SYS_COLD_RESET,
                                    full, sizeof ( full ) );
    TEST_ASSERT_NOT_NULL ( strstr ( full, "Cold reset" ) );

    eventlog_decode_subtype_full ( EVENTLOG_CAT_SYS,
                                    EVENTLOG_SYS_MZF_INJECT,
                                    full, sizeof ( full ) );
    TEST_ASSERT_NOT_NULL ( strstr ( full, "MZF" ) );
}


/**
 * @brief Neznámá kombinace (= existující kategorie ale subtype mimo
 *        rozsah) -> short fallback = decimální zápis.
 */
void test_subtype_unknown_fallback ( void )
{
    char buf[ 16 ];
    /* PIO8255 má subtype 1..4 - hodnota 99 = unknown. */
    eventlog_decode_subtype_short ( EVENTLOG_CAT_PIO8255, 99,
                                     buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_STRING ( "99", buf );

    /* Full fallback = "<short> (subtype N)". */
    char full[ 64 ];
    eventlog_decode_subtype_full ( EVENTLOG_CAT_PIO8255, 99,
                                    full, sizeof ( full ) );
    TEST_ASSERT_NOT_NULL ( strstr ( full, "99" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( full, "subtype" ) );
}


/**
 * @brief NULL buf / buf_len = 0 jsou no-op (= no crash, žádný write).
 */
void test_subtype_null_safety ( void )
{
    char buf[ 16 ] = { 'X', 0 };

    eventlog_decode_subtype_short ( EVENTLOG_CAT_CPU_CTRL, 0, NULL, 16 );
    eventlog_decode_subtype_short ( EVENTLOG_CAT_CPU_CTRL, 0, buf, 0 );
    /* buf nezměněn (žádný write). */
    TEST_ASSERT_EQUAL_CHAR ( 'X', buf[ 0 ] );

    eventlog_decode_subtype_full ( EVENTLOG_CAT_CPU_CTRL, 0, NULL, 16 );
    eventlog_decode_subtype_full ( EVENTLOG_CAT_CPU_CTRL, 0, buf, 0 );
    TEST_ASSERT_EQUAL_CHAR ( 'X', buf[ 0 ] );
}


/* ================================================================
 * Commit 24 - ambient state decoder (Vlna 4)
 * ================================================================ */

/**
 * Ambient = 0 (= IFF1=0, IM=0, reason=IFF_RESET, banking=DEFAULT).
 * Reason 0 NENÍ NONE, ale IFF_RESET - musí být v textu.
 */
void test_eventlog_ambient_decode_zero_is_iff_reset ( void )
{
    char buf [ 64 ];
    eventlog_decode_ambient ( 0, buf, sizeof ( buf ) );

    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IFF1=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IM=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "reason=RESET" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "bank=" ) );
}


/**
 * Ambient s reason=NONE (= mimo BP fire stack) - text vynechá "reason=".
 */
void test_eventlog_ambient_decode_reason_none_skipped ( void )
{
    /* Reason NONE = 7 = bity 3..5 plné. */
    uint16_t a = (uint16_t) ( EVENTLOG_AMBIENT_REASON_NONE
                              << EVENTLOG_AMBIENT_REASON_SHIFT );
    char buf [ 64 ];
    eventlog_decode_ambient ( a, buf, sizeof ( buf ) );

    /* Reason se v textu nezobrazuje (= "NONE" je default, šum). */
    TEST_ASSERT_NULL ( strstr ( buf, "reason=" ) );
    /* IFF1 / IM / bank vždy. */
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IFF1=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IM=0" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "bank=" ) );
}


/**
 * Ambient IFF1=1 IM=2 - text obsahuje "IFF1=1 IM=2".
 */
void test_eventlog_ambient_decode_iff1_im2 ( void )
{
    uint16_t a = EVENTLOG_AMBIENT_IFF1
               | (uint16_t) ( 2u << EVENTLOG_AMBIENT_IM_SHIFT )
               | (uint16_t) ( EVENTLOG_AMBIENT_REASON_NONE
                              << EVENTLOG_AMBIENT_REASON_SHIFT );
    char buf [ 64 ];
    eventlog_decode_ambient ( a, buf, sizeof ( buf ) );

    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IFF1=1" ) );
    TEST_ASSERT_NOT_NULL ( strstr ( buf, "IM=2" ) );
}


/**
 * Ambient reason=NMI_ACK - text obsahuje "reason=NMI_ACK".
 */
void test_eventlog_ambient_decode_reason_nmi_ack ( void )
{
    uint16_t a = (uint16_t) ( EVENTLOG_AMBIENT_REASON_IFF_NMI_ACK
                              << EVENTLOG_AMBIENT_REASON_SHIFT );
    char buf [ 64 ];
    eventlog_decode_ambient ( a, buf, sizeof ( buf ) );

    TEST_ASSERT_NOT_NULL ( strstr ( buf, "reason=NMI_ACK" ) );
}


/**
 * Decoder NULL safety - buf=NULL nebo buf_len=0 nesmí padat.
 */
void test_eventlog_ambient_decode_null_safety ( void )
{
    char buf [ 8 ] = { 0 };
    /* Nedělá nic, ale nesmí padat. */
    eventlog_decode_ambient ( 0, NULL, 0 );
    eventlog_decode_ambient ( 0, buf, 0 );
    /* Malý buffer - musí být NUL-terminated. */
    eventlog_decode_ambient ( 0xFFFFu, buf, sizeof ( buf ) );
    TEST_ASSERT_EQUAL_INT ( 0, buf [ sizeof ( buf ) - 1 ] );
}


/* ================================================================
 * Main
 * ================================================================ */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_eventlog_detail_cpu_int_bitmask );
    RUN_TEST ( test_eventlog_detail_cpu_pin_edge );
    RUN_TEST ( test_eventlog_detail_irq_ack_im2 );

    RUN_TEST ( test_eventlog_detail_iorq_decode );
    RUN_TEST ( test_eventlog_detail_iorq_unconnected );
    RUN_TEST ( test_eventlog_detail_mmio_decode );

    RUN_TEST ( test_eventlog_detail_gdg_mode_decode );
    RUN_TEST ( test_eventlog_detail_gdg_mode_all_values );
    RUN_TEST ( test_eventlog_detail_gdg_banking_decode );
    RUN_TEST ( test_eventlog_detail_gdg_colors_decode );
    RUN_TEST ( test_eventlog_detail_gdg_video_decode );
    RUN_TEST ( test_eventlog_detail_gdg_hwscroll_decode );
    RUN_TEST ( test_eventlog_detail_gdg_wfrf_decode );

    RUN_TEST ( test_eventlog_detail_pio8255_decode );
    RUN_TEST ( test_eventlog_detail_ctc8253_cw_decode );
    RUN_TEST ( test_eventlog_detail_pioz80_decode );
    RUN_TEST ( test_eventlog_detail_psg_decode );

    RUN_TEST ( test_eventlog_detail_fdc_command_decode );
    RUN_TEST ( test_eventlog_detail_fdc_command_issued_restore );
    RUN_TEST ( test_eventlog_detail_fdc_command_issued_read_sector );
    RUN_TEST ( test_eventlog_detail_fdc_command_issued_unknown );
    RUN_TEST ( test_eventlog_subtype_fdc_command_short );
    RUN_TEST ( test_eventlog_subtype_fdc_command_full );
    RUN_TEST ( test_eventlog_detail_memext_decode );
    RUN_TEST ( test_eventlog_detail_qd_decode );
    RUN_TEST ( test_eventlog_detail_rd_decode );

    RUN_TEST ( test_eventlog_detail_bp_fire_decode );
    RUN_TEST ( test_eventlog_detail_user_mark_decode );
    RUN_TEST ( test_eventlog_detail_cpu_ctrl_decode );

    RUN_TEST ( test_eventlog_detail_unknown_payload );
    RUN_TEST ( test_eventlog_detail_null_safety );

    /* Vlna 3 Commit 22 - subtype short / full decoder. */
    RUN_TEST ( test_subtype_short_iorq_normal );
    RUN_TEST ( test_subtype_short_pioz80_bus_in );
    RUN_TEST ( test_subtype_short_gdg_video_hbln_s );
    RUN_TEST ( test_subtype_short_gdg_banking_port );
    RUN_TEST ( test_subtype_short_no_subtype_dash );
    RUN_TEST ( test_subtype_full_pioz80_irq_ack );
    RUN_TEST ( test_subtype_full_gdg_banking_e2 );
    RUN_TEST ( test_subtype_full_cpu_ctrl_rst38 );
    RUN_TEST ( test_subtype_unknown_fallback );
    RUN_TEST ( test_subtype_null_safety );

    /* Vlna 4 Commit 24 - ambient state decoder. */
    RUN_TEST ( test_eventlog_ambient_decode_zero_is_iff_reset );
    RUN_TEST ( test_eventlog_ambient_decode_reason_none_skipped );
    RUN_TEST ( test_eventlog_ambient_decode_iff1_im2 );
    RUN_TEST ( test_eventlog_ambient_decode_reason_nmi_ack );
    RUN_TEST ( test_eventlog_ambient_decode_null_safety );

    /* Vlna 5 Commit 31 - SYS kategorie. */
    RUN_TEST ( test_decoder_sys_cold_reset );
    RUN_TEST ( test_decoder_sys_snapshot_save );
    RUN_TEST ( test_decoder_sys_mzf_inject );
    RUN_TEST ( test_subtype_sys_short_long );

    return UNITY_END ( );
}
