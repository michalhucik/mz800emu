/**
 * @file   eventlog_decoder.c
 * @brief  Implementace rich Detail decoderu pro Event Viewer.
 *
 * Per-kategorie decode helper funkce + dispatch z @ref
 * eventlog_decode_detail(). Layout decoded textu viz hlavička.
 *
 * Decoder helper pravidla:
 *  - Pure C, žádná závislost na živém emu state.
 *  - Buffer-safe: snprintf s respektováním @c buf_len, žádný overflow.
 *  - Pro neznámé hodnoty zachovat raw hex info (= lepší než prázdný text).
 *  - User-facing texty v angličtině (Makefile MO katalog je opt-in).
 *
 * @author Michal Hucik <hucik@ordoz.com>
 *
 * Licence: GPLv3
 */

#include "main.h"
#include "mzarch/mzhal.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "eventlog_decoder.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* MZARCH makro je definováno přes -DMZARCH=N v Makefile per target.
 * Použito v eventlog_decode_ambient() pro per-arch banking texty. */
#include "debugger/trace/eventlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/marklog.h"
#include "debugger/io_catalog.h"
#include "hw-generic/fdc/wd279x.h"

/* ===========================================================================
 *  Internal helpers - lookup
 * =========================================================================== */

/**
 * @brief Najde human-readable jméno portu z @c g_io_ports[].
 *
 * Vrací krátký @c name (typicky "GDG DMD", "PSG L"). Match strategie:
 *  - 16-bit GDG family 0xCF&lt;RR&gt; -&gt; full addr match
 *  - jinak 8-bit match na low byte (= IORQ bus může mít junk v high)
 *
 * @param port16  16-bit port address (= eventlog payload low 16b).
 * @return Static string z katalogu (NEVOLAT free), nebo NULL.
 */
static const char *decoder_lookup_port_name ( uint16_t port16 )
{
    const uint8_t low = (uint8_t) ( port16 & 0xFFu );
    const uint8_t high = (uint8_t) ( ( port16 >> 8 ) & 0xFFu );

    /* GDG 0xCF<RR> family (scroll/border) - full 16-bit lookup. */
    const int is_gdg_cf = ( low == 0xCF && high >= 0x01 && high <= 0x07 );
    uint16_t key16 = is_gdg_cf ? (uint16_t) ( 0xCF00u | high ) : 0u;

    for ( size_t i = 0; i < g_io_ports_count; i++ ) {
        const st_IO_PORT_DESC *p = &g_io_ports[ i ];
        if ( !p->name ) continue;
        if ( is_gdg_cf ) {
            if ( p->addr == key16 ) return p->name;
        } else {
            if ( p->addr <= 0xFFu && ( p->addr & 0xFFu ) == low ) return p->name;
        }
    }
    return NULL;
}


/* ===========================================================================
 *  Internal helpers - chip-specific decoders
 * =========================================================================== */

/**
 * @brief Dekóduje CPU_INT state bitmask.
 *
 * Bitová pole odpovídají @c INTLOG_STATE_BIT_* v intlog.h. Výstup formy
 * @c "IM=2 IFF1=1 IFF2=1 RETI EI" (DI analogicky s příznakem DI).
 * Nepřítomné bity nepíšou nic.
 */
static void decode_cpu_int_state ( uint32_t bits, char *buf, size_t buf_len )
{
    /* IM mode = MSB z IM0/IM1/IM2 trojice (= klasický Z80 IM 0/1/2). */
    int im = 0;
    if ( bits & INTLOG_STATE_BIT_IM2 )      im = 2;
    else if ( bits & INTLOG_STATE_BIT_IM1 ) im = 1;
    else if ( bits & INTLOG_STATE_BIT_IM0 ) im = 0;
    else                                    im = -1;

    const int iff1 = ( bits & INTLOG_STATE_BIT_IFF1 ) ? 1 : 0;
    const int iff2 = ( bits & INTLOG_STATE_BIT_IFF2 ) ? 1 : 0;
    const int reti = ( bits & INTLOG_STATE_BIT_RETI ) ? 1 : 0;
    const int ei   = ( bits & INTLOG_STATE_BIT_EI )   ? 1 : 0;
    const int di   = ( bits & INTLOG_STATE_BIT_DI )   ? 1 : 0;

    /* Pokud žádný IMx bit, vypsat aspoň "?". */
    if ( im >= 0 ) {
        snprintf ( buf, buf_len, "IM=%d IFF1=%d IFF2=%d%s%s%s",
                   im, iff1, iff2,
                   reti ? " RETI" : "",
                   ei ? " EI" : "",
                   di ? " DI" : "" );
    } else {
        snprintf ( buf, buf_len, "IFF1=%d IFF2=%d%s%s%s",
                   iff1, iff2,
                   reti ? " RETI" : "",
                   ei ? " EI" : "",
                   di ? " DI" : "" );
    }
}


/**
 * @brief Vrátí krátký label pro INTLOG source chip (CTC2 / PIOZ80 / ...).
 */
static const char *decode_int_source ( uint8_t src )
{
    switch ( src ) {
        case INTLOG_CHIP_NONE:             return "?";
        case INTLOG_CHIP_CTC2:             return "CTC2";
        case INTLOG_CHIP_PIOZ80:           return "PIOZ80";
        case INTLOG_CHIP_PIOZ80_PA0:       return "PIOZ80@PA0";
        case INTLOG_CHIP_PIOZ80_PA1:       return "PIOZ80@PA1";
        case INTLOG_CHIP_PIOZ80_PA2:       return "PIOZ80@PA2";
        case INTLOG_CHIP_PIOZ80_PA3:       return "PIOZ80@PA3";
        case INTLOG_CHIP_PIOZ80_PA4:       return "PIOZ80@PA4";
        case INTLOG_CHIP_PIOZ80_PA5:       return "PIOZ80@PA5";
        case INTLOG_CHIP_PIOZ80_PA6:       return "PIOZ80@PA6";
        case INTLOG_CHIP_PIOZ80_PA7:       return "PIOZ80@PA7";
        case INTLOG_CHIP_PIOZ80_PB0:       return "PIOZ80@PB0";
        case INTLOG_CHIP_PIOZ80_PB1:       return "PIOZ80@PB1";
        case INTLOG_CHIP_PIOZ80_PB2:       return "PIOZ80@PB2";
        case INTLOG_CHIP_PIOZ80_PB3:       return "PIOZ80@PB3";
        case INTLOG_CHIP_PIOZ80_PB4:       return "PIOZ80@PB4";
        case INTLOG_CHIP_PIOZ80_PB5:       return "PIOZ80@PB5";
        case INTLOG_CHIP_PIOZ80_PB6:       return "PIOZ80@PB6";
        case INTLOG_CHIP_PIOZ80_PB7:       return "PIOZ80@PB7";
        case INTLOG_CHIP_FDC:              return "FDC";
        case INTLOG_CHIP_CPU:              return "CPU";
        case INTLOG_CHIP_PIOZ80_PORT_A:    return "PIOZ80_PA";
        case INTLOG_CHIP_PIOZ80_PORT_B:    return "PIOZ80_PB";
        case INTLOG_CHIP_VECTOR_BUS_LATCH: return "BUS_LATCH";
        default:                           return NULL;
    }
}


/**
 * @brief Dekóduje CPU_PIN_EDGE event (= INT/NMI pin transition).
 *
 * Subtype = source chip, payload low 8b = pin index, payload b8..b15 = edge.
 */
static void decode_cpu_pin_edge ( uint8_t subtype, uint32_t payload,
                                  char *buf, size_t buf_len )
{
    const char *src = decode_int_source ( subtype );
    const uint8_t edge = (uint8_t) ( ( payload >> 8 ) & 0xFFu );
    const char *edge_str = "?";
    switch ( edge ) {
        case INTLOG_EDGE_NONE:    edge_str = "level"; break;
        case INTLOG_EDGE_RISING:  edge_str = "rising"; break;
        case INTLOG_EDGE_FALLING: edge_str = "falling"; break;
    }
    if ( src ) {
        snprintf ( buf, buf_len, "%s %s", src, edge_str );
    } else {
        snprintf ( buf, buf_len, "src=%u %s", (unsigned) subtype, edge_str );
    }
}


/**
 * @brief Dekóduje IRQ_ACK_IM2 event.
 *
 * Subtype = source chip, payload low 16b = vector table addr, high 16b = ISR
 * addr. Formát: @c "PIOZ80_PA vec=0x40 isr=0x4042".
 */
static void decode_irq_ack_im2 ( uint8_t subtype, uint32_t payload,
                                 char *buf, size_t buf_len )
{
    const char *src = decode_int_source ( subtype );
    const unsigned vec = (unsigned) ( payload & 0xFFFFu );
    const unsigned isr = (unsigned) ( ( payload >> 16 ) & 0xFFFFu );
    if ( src ) {
        snprintf ( buf, buf_len, "%s vec=0x%04X isr=0x%04X",
                   src, vec, isr );
    } else {
        snprintf ( buf, buf_len, "src=%u vec=0x%04X isr=0x%04X",
                   (unsigned) subtype, vec, isr );
    }
}


/**
 * @brief Dekóduje IORQ_IN/IORQ_OUT event.
 *
 * Payload low 16b = port (16-bit IORQ adresa = B z BC), bity 16..23 =
 * value byte. Lookup jména portu přes io_catalog.
 */
static void decode_iorq ( uint8_t subtype, uint32_t payload,
                          char *buf, size_t buf_len )
{
    const unsigned port  = (unsigned) ( payload & 0xFFFFu );
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    const char *name = decoder_lookup_port_name ( (uint16_t) port );
    const char *unc = ( subtype == EVENTLOG_IORQ_SUB_UNCONNECTED )
                      ? " [unconnected]" : "";
    if ( name ) {
        snprintf ( buf, buf_len, "port=0x%04X (%s) val=0x%02X%s",
                   port, name, value, unc );
    } else {
        snprintf ( buf, buf_len, "port=0x%04X val=0x%02X%s",
                   port, value, unc );
    }
}


/**
 * @brief Dekóduje MMIO_R/MMIO_W event.
 *
 * Payload low 16b = full addr (0xE000-0xE008), bity 16..23 = value.
 */
static void decode_mmio ( uint32_t payload, char *buf, size_t buf_len )
{
    const unsigned addr  = (unsigned) ( payload & 0xFFFFu );
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    snprintf ( buf, buf_len, "addr=0x%04X val=0x%02X", addr, value );
}


/**
 * @brief Dekóduje GDG DMD byte na human-readable mode string.
 *
 * DMD layout (mz800.txt + io_catalog.c::decode_dmd):
 *   bity 1..0 = barevný režim (00=2col, 01=4col, 10=16col, 11=text)
 *   bity 3..2 = screen typ:
 *               00 = 320x200 (MZ-800 native)
 *               01 = 640x200 (MZ-800 native)
 *               10 = MZ-700 mode
 *               11 = illegal
 *
 * Pro běžné kombinace generuje krátký label "320x200x16", "640x200x4",
 * "MZ-700 text", atd. Pro illegal/neznámé padá zpět na raw layout.
 */
static const char *decode_dmd_mode_short ( uint8_t dmd )
{
    const unsigned screen = ( dmd >> 2 ) & 0x03u;
    const unsigned col    =   dmd        & 0x03u;
    /* col coding: 00=2 barvy, 01=4 barvy, 10=16 barvy. */
    if ( screen == 0 ) {
        if ( col == 0 ) return "320x200x2";
        if ( col == 1 ) return "320x200x4";
        if ( col == 2 ) return "320x200x16";
    } else if ( screen == 1 ) {
        if ( col == 0 ) return "640x200x2";
        if ( col == 1 ) return "640x200x4";
    } else if ( screen == 2 ) {
        return "MZ-700 mode";
    }
    return "?";
}


/**
 * @brief Dekóduje GDG_MODE event - DMD byte zapsaný na port 0xCE.
 *
 * Payload bytes (hwlog 2-byte layout přes addr+value):
 *   [0] addr low  (= 0xCE)
 *   [1] addr high (typ. 0x00)
 *   [2] value     (= DMD byte)
 */
static void decode_gdg_mode ( uint32_t payload, char *buf, size_t buf_len )
{
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    snprintf ( buf, buf_len, "DMD=0x%02X (%s)",
               value, decode_dmd_mode_short ( (uint8_t) value ) );
}


/**
 * @brief Vrátí krátký label pro GDG_BANKING port (= subtype 0xE0..0xE6).
 *
 * Sémantika sjednocená s mz800/mz1500 banking switch (viz
 * hwlog.h::en_HWLOG_GDG_BANKING_SUB).
 */
static const char *decode_gdg_banking_op ( uint8_t port )
{
    switch ( port ) {
        case HWLOG_GDG_BANKING_E0: return "ROM bottom OFF";
        case HWLOG_GDG_BANKING_E1: return "ROM upper OFF";
        case HWLOG_GDG_BANKING_E2: return "ROM bottom ON";
        case HWLOG_GDG_BANKING_E3: return "ROM upper ON";
        case HWLOG_GDG_BANKING_E4: return "ALL ON (reset)";
        case HWLOG_GDG_BANKING_E5: return "EXROM/SPEC ON";
        case HWLOG_GDG_BANKING_E6: return "EXROM/SPEC OFF";
        default:                   return "?";
    }
}


/**
 * @brief Dekóduje GDG_BANKING event (subtype = port E0..E6).
 */
static void decode_gdg_banking ( uint8_t subtype, uint32_t payload,
                                 char *buf, size_t buf_len )
{
    /* Payload [0]=port low, [1]=value (per mz800_memory.c hwlog emit). */
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    snprintf ( buf, buf_len, "port 0x%02X (%s) val=0x%02X",
               (unsigned) subtype, decode_gdg_banking_op ( subtype ), value );
}


/**
 * @brief Dekóduje GDG_HWSCROLL event.
 *
 * Subtype = scroll register index (msb adresy 0xCFRR, RR = 0..5):
 *   0 = SOF1 (start of frame low)
 *   1 = SOF2 (start of frame high)
 *   2 = SW1  (screen width low / SW)
 *   3 = SW2  (screen width high)
 *   4 = SSA  (scroll start address)
 *   5 = SEA  (scroll end address)
 *
 * Payload [0]=addr low (0xCF), [1]=addr high (= subtype), [2]=value.
 */
static void decode_gdg_hwscroll ( uint8_t subtype, uint32_t payload,
                                  char *buf, size_t buf_len )
{
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    const char *reg;
    switch ( subtype ) {
        case 0: reg = "SOF1"; break;
        case 1: reg = "SOF2"; break;
        case 2: reg = "SW1";  break;
        case 3: reg = "SW2";  break;
        case 4: reg = "SSA";  break;
        case 5: reg = "SEA";  break;
        default: reg = "?";   break;
    }
    snprintf ( buf, buf_len, "%s=0x%02X", reg, value );
}


/**
 * @brief Dekóduje GDG_COLORS event (BORDER / PALGRP / PAL / PCG / PACKETGROUP).
 *
 * Payload [0]=addr low, [1]=addr high, [2]=value byte. Pro PAL nese
 * value v low nibble číslo palety (0..3) + high nibble vlastní hodnotu.
 */
static void decode_gdg_colors ( uint8_t subtype, uint32_t payload,
                                char *buf, size_t buf_len )
{
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    switch ( subtype ) {
        case HWLOG_GDG_COLORS_BORDER:
            snprintf ( buf, buf_len, "BORDER=0x%02X", value );
            return;
        case HWLOG_GDG_COLORS_PALGRP:
            snprintf ( buf, buf_len, "PALGRP=0x%02X", value );
            return;
        case HWLOG_GDG_COLORS_PAL: {
            /* PAL: low 2 bity index, high nibble (po >>4) reálná barva.
             * Sjednocené s emu encoding (port 0xF0, bit 6 = 0 = PAL). */
            const unsigned idx = value & 0x03u;
            const unsigned col = ( value >> 4 ) & 0x0Fu;
            snprintf ( buf, buf_len, "PAL[%u]=0x%X (raw=0x%02X)",
                       idx, col, value );
            return;
        }
        case HWLOG_GDG_COLORS_PCG:
            snprintf ( buf, buf_len, "PCG=0x%02X", value );
            return;
        case HWLOG_GDG_COLORS_PACKETGROUP:
            snprintf ( buf, buf_len, "PACKETGROUP=0x%02X", value );
            return;
        default:
            snprintf ( buf, buf_len, "sub=%u val=0x%02X",
                       (unsigned) subtype, value );
            return;
    }
}


/**
 * @brief Dekóduje GDG_VIDEO event (subtype už říká vše).
 */
static void decode_gdg_video ( uint8_t subtype, char *buf, size_t buf_len )
{
    const char *label;
    switch ( subtype ) {
        case HWLOG_GDG_VIDEO_VBLN_START: label = "VBLN start"; break;
        case HWLOG_GDG_VIDEO_VBLN_END:   label = "VBLN end";   break;
        case HWLOG_GDG_VIDEO_VS_START:   label = "VS start";   break;
        case HWLOG_GDG_VIDEO_VS_END:     label = "VS end";     break;
        case HWLOG_GDG_VIDEO_HBLN_START: label = "HBLN start"; break;
        case HWLOG_GDG_VIDEO_HBLN_END:   label = "HBLN end";   break;
        case HWLOG_GDG_VIDEO_HS_START:   label = "HS start";   break;
        case HWLOG_GDG_VIDEO_HS_END:     label = "HS end";     break;
        default:                         label = "?";          break;
    }
    snprintf ( buf, buf_len, "%s", label );
}


/**
 * @brief Dekóduje GDG_WFRF event (subtype: 0=WF, 1=RF).
 *
 * Payload [2]=value byte (= WF nebo RF register).
 */
static void decode_gdg_wfrf ( uint8_t subtype, uint32_t payload,
                              char *buf, size_t buf_len )
{
    const unsigned value = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    const char *reg = ( subtype == 0 ) ? "WF" : ( subtype == 1 ) ? "RF" : "?";
    /* WF/RF top 3 bity = mode (000=PSET, 001=AND, 010=OR, 011=XOR, 100=NOT
     * a další - viz mz-800.txt GDG WF/RF). Plane select v low 4 bitech. */
    if ( subtype == 0 ) {
        const unsigned mode = ( value >> 5 ) & 0x07u;
        const char *mode_name = "?";
        switch ( mode ) {
            case 0: mode_name = "REPL"; break;
            case 1: mode_name = "PSET"; break;
            case 2: mode_name = "AND";  break;
            case 3: mode_name = "OR";   break;
            case 4: mode_name = "XOR";  break;
        }
        snprintf ( buf, buf_len, "%s=0x%02X (%s)", reg, value, mode_name );
    } else {
        snprintf ( buf, buf_len, "%s=0x%02X", reg, value );
    }
}


/**
 * @brief Dekóduje PIO8255 event.
 *
 * Subtype: PORT_A_WRITE / PORT_B_WRITE / PORT_C_WRITE / CONTROL_WRITE.
 * Payload [0]=addr (0..3), [1]=value byte.
 */
static void decode_pio8255 ( uint8_t subtype, uint32_t payload,
                             char *buf, size_t buf_len )
{
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    switch ( subtype ) {
        case HWLOG_PIO8255_PORT_A_WRITE:
            snprintf ( buf, buf_len, "Port A=0x%02X", value );
            return;
        case HWLOG_PIO8255_PORT_B_WRITE:
            snprintf ( buf, buf_len, "Port B=0x%02X", value );
            return;
        case HWLOG_PIO8255_PORT_C_WRITE:
            snprintf ( buf, buf_len, "Port C=0x%02X", value );
            return;
        case HWLOG_PIO8255_CONTROL_WRITE: {
            /* CW bit 7 = 1 -> mode set, 0 -> bit set/reset.
             * mode set: A mode (bity 6-5), C upper dir (bit 3), A dir (bit 4),
             * B mode (bit 2), C lower dir (bit 0), B dir (bit 1).
             * bit set/reset: bity 3..1 = bit index, bit 0 = set(1)/reset(0). */
            if ( value & 0x80 ) {
                const unsigned a_mode = ( value >> 5 ) & 0x03u;
                snprintf ( buf, buf_len,
                           "CW=0x%02X (mode set, A mode %u)",
                           value, a_mode );
            } else {
                const unsigned bit = ( value >> 1 ) & 0x07u;
                const unsigned set = value & 0x01u;
                snprintf ( buf, buf_len,
                           "CW=0x%02X (PC%u %s)",
                           value, bit, set ? "set" : "reset" );
            }
            return;
        }
        default:
            snprintf ( buf, buf_len, "sub=%u val=0x%02X",
                       (unsigned) subtype, value );
            return;
    }
}


/**
 * @brief Dekóduje 8253 CTC Control Word (CW).
 *
 * CW bity:
 *   D7-D6 = SC (select counter 0..2; 3 = invalid / readback)
 *   D5-D4 = RW (read/write LSB-MSB; 00=latch, 01=LSB, 10=MSB, 11=LSB then MSB)
 *   D3-D1 = MODE (0..5)
 *   D0    = BCD (0 = binary, 1 = BCD)
 */
static void decode_ctc8253_cw ( uint8_t cw, char *buf, size_t buf_len )
{
    const unsigned sc   = ( cw >> 6 ) & 0x03u;
    const unsigned rw   = ( cw >> 4 ) & 0x03u;
    const unsigned mode = ( cw >> 1 ) & 0x07u;
    const unsigned bcd  =   cw        & 0x01u;

    const char *mode_name;
    /* CTC8253 mode 0..5 (mode 6,7 = aliasy 2,3). */
    switch ( mode & 0x07u ) {
        case 0: mode_name = "intr on terminal";        break;
        case 1: mode_name = "hw retrig one-shot";      break;
        case 2: case 6: mode_name = "rate gen";        break;
        case 3: case 7: mode_name = "square wave";     break;
        case 4: mode_name = "sw trig strobe";          break;
        case 5: mode_name = "hw trig strobe";          break;
        default: mode_name = "?";                      break;
    }

    const char *rw_name;
    switch ( rw ) {
        case 0: rw_name = "latch"; break;
        case 1: rw_name = "LSB";   break;
        case 2: rw_name = "MSB";   break;
        case 3: rw_name = "LSB+MSB"; break;
        default: rw_name = "?";    break;
    }

    snprintf ( buf, buf_len,
               "CW=0x%02X: cnt %u mode %u (%s) RW=%s %s",
               (unsigned) cw, sc, mode, mode_name, rw_name,
               bcd ? "BCD" : "BIN" );
}


/**
 * @brief Dekóduje CTC8253 event (CONTROL_WRITE / COUNTER_WRITE).
 *
 * Payload [0]=addr (0..3), [1]=value, [2]=pre-write rl_byte (jen pro COUNTER).
 */
static void decode_ctc8253 ( uint8_t subtype, uint32_t payload,
                             char *buf, size_t buf_len )
{
    const unsigned addr  = (unsigned) ( payload & 0xFFu );
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    if ( subtype == HWLOG_CTC8253_CONTROL_WRITE ) {
        decode_ctc8253_cw ( (uint8_t) value, buf, buf_len );
    } else if ( subtype == HWLOG_CTC8253_COUNTER_WRITE ) {
        snprintf ( buf, buf_len, "CTC%u=0x%02X", addr, value );
    } else {
        snprintf ( buf, buf_len, "sub=%u addr=%u val=0x%02X",
                   (unsigned) subtype, addr, value );
    }
}


/**
 * @brief Dekóduje PIOZ80 event.
 *
 * Payload [0]=port_id (0=A, 1=B, 0xFF=N/A), [1]=sub_addr,
 *         [2]=value, [3..5]=delta bitmask (24b LE, neviditelná zde -
 *         payload je 4B = jen low 8b z delta).
 */
static void decode_pioz80 ( uint8_t subtype, uint32_t payload,
                            char *buf, size_t buf_len )
{
    const unsigned port_id = (unsigned) ( payload & 0xFFu );
    const unsigned value   = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    const char *port_name = ( port_id == 0 ) ? "A"
                          : ( port_id == 1 ) ? "B"
                          : "?";

    switch ( subtype ) {
        case HWLOG_PIOZ80_MODE_WRITE: {
            /* Mode Control Word: high nibble (bity 7..6) = mode 0..3.
             *   00 = Mode 0 byte output
             *   01 = Mode 1 byte input
             *   10 = Mode 2 bidirectional (jen port A)
             *   11 = Mode 3 bit control
             * Mode 3 spotřebuje další byte (I/O Select Mask). */
            const unsigned mode = ( value >> 6 ) & 0x03u;
            const char *mname;
            switch ( mode ) {
                case 0: mname = "out";    break;
                case 1: mname = "in";     break;
                case 2: mname = "bidir";  break;
                case 3: mname = "bit";    break;
                default: mname = "?";     break;
            }
            snprintf ( buf, buf_len, "%s MODE=0x%02X (mode %u %s)",
                       port_name, value, mode, mname );
            return;
        }
        case HWLOG_PIOZ80_VECTOR_WRITE:
            snprintf ( buf, buf_len, "%s VECTOR=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_INT_CTRL_WRITE: {
            /* ICW byte (per Z80 PIO datasheet):
             *   bit 7 = EI (enable)
             *   bit 6 = AND/OR (1=AND, 0=OR)
             *   bit 5 = HIGH/LOW (active level)
             *   bit 4 = MF (mask follows)
             *   bits 3..0 = 0111b (= fixed pattern pro ICW). */
            const unsigned ena  = ( value >> 7 ) & 1u;
            const unsigned mode = ( value >> 6 ) & 1u;
            const unsigned lvl  = ( value >> 5 ) & 1u;
            const unsigned mf   = ( value >> 4 ) & 1u;
            snprintf ( buf, buf_len,
                       "%s ICW=0x%02X (EI=%u %s active=%s MF=%u)",
                       port_name, value, ena,
                       mode ? "AND" : "OR",
                       lvl ? "high" : "low", mf );
            return;
        }
        case HWLOG_PIOZ80_MASK_WRITE:
            snprintf ( buf, buf_len, "%s MASK=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_IO_SELECT_WRITE:
            snprintf ( buf, buf_len, "%s IO_SEL=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_DATA_WRITE:
            snprintf ( buf, buf_len, "%s DATA_W=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_DATA_READ:
            snprintf ( buf, buf_len, "%s DATA_R=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_BUS_INPUT_CHANGE:
            snprintf ( buf, buf_len, "%s BUS_IN=0x%02X", port_name, value );
            return;
        case HWLOG_PIOZ80_IRQ_ACK_M2:
            snprintf ( buf, buf_len, "%s IRQ_ACK vec=0x%02X",
                       port_name, value );
            return;
        case HWLOG_PIOZ80_RETI_APPLIED:
            snprintf ( buf, buf_len, "%s RETI applied", port_name );
            return;
        default:
            snprintf ( buf, buf_len, "%s sub=%u val=0x%02X",
                       port_name, (unsigned) subtype, value );
            return;
    }
}


/**
 * @brief Dekóduje PSG event (REGISTER_WRITE).
 *
 * Payload [0]=raw byte, [1]=channel mask, [2]=stereo flag. SN76489 register
 * write má dva tvary:
 *   - latch (bit 7 = 1): bits 6..5 = channel (0..2 tone, 3 noise),
 *                        bit 4 = type (0 = period, 1 = attenuator),
 *                        bits 3..0 = data low nibble (period low 4b nebo attn).
 *   - data  (bit 7 = 0): bits 5..0 = period high 6 bits (jen pro tone/noise period
 *                        po předchozím latch).
 *
 * Full frequency decode vyžaduje track latch state (= stateful, follow-up).
 * Tady jen identifikujeme kterého registru se write týká.
 */
static void decode_psg ( uint8_t subtype, uint32_t payload,
                         char *buf, size_t buf_len )
{
    if ( subtype != HWLOG_PSG_REGISTER_WRITE ) {
        snprintf ( buf, buf_len, "sub=%u payload=0x%08X",
                   (unsigned) subtype, (unsigned) payload );
        return;
    }
    const unsigned value   = (unsigned) ( payload & 0xFFu );
    const unsigned channel = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    const unsigned stereo  = (unsigned) ( ( payload >> 16 ) & 0xFFu );

    /* Side label pro stereo. */
    const char *side = "mono";
    if ( stereo ) {
        if ( channel == 0x01 ) side = "L";
        else if ( channel == 0x02 ) side = "R";
        else side = "LR";
    }

    if ( value & 0x80 ) {
        /* Latch byte. */
        const unsigned ch_idx = ( value >> 5 ) & 0x03u;
        const unsigned typ    = ( value >> 4 ) & 0x01u;
        const char *ch_name;
        switch ( ch_idx ) {
            case 0: ch_name = "ch A"; break;
            case 1: ch_name = "ch B"; break;
            case 2: ch_name = "ch C"; break;
            case 3: ch_name = "noise"; break;
            default: ch_name = "?"; break;
        }
        const char *typ_name = typ ? "attn" : "period lo";
        snprintf ( buf, buf_len, "%s 0x%02X (latch %s %s)",
                   side, value, ch_name, typ_name );
    } else {
        snprintf ( buf, buf_len, "%s 0x%02X (period hi)", side, value );
    }
}


/**
 * @brief Dekóduje WD279x FDC command byte.
 *
 * Top nibble identifikuje typ command:
 *   0x00 = Restore
 *   0x10 = Seek
 *   0x20 = Step (no update)
 *   0x30 = Step (update)
 *   0x40 = Step In (no update)
 *   0x50 = Step In (update)
 *   0x60 = Step Out (no update)
 *   0x70 = Step Out (update)
 *   0x80 = Read Sector (single)
 *   0x90 = Read Sector (multi)
 *   0xA0 = Write Sector (single)
 *   0xB0 = Write Sector (multi)
 *   0xC0 = Read Address
 *   0xD0 = Force Interrupt
 *   0xE0 = Read Track
 *   0xF0 = Write Track
 */
static const char *decode_fdc_command ( uint8_t cmd )
{
    switch ( cmd & 0xF0u ) {
        case 0x00: return "Restore";
        case 0x10: return "Seek";
        case 0x20: return "Step";
        case 0x30: return "Step+upd";
        case 0x40: return "Step In";
        case 0x50: return "Step In+upd";
        case 0x60: return "Step Out";
        case 0x70: return "Step Out+upd";
        case 0x80: return "Read Sector";
        case 0x90: return "Read Sector M";
        case 0xA0: return "Write Sector";
        case 0xB0: return "Write Sector M";
        case 0xC0: return "Read Addr";
        case 0xD0: return "Force Intr";
        case 0xE0: return "Read Track";
        case 0xF0: return "Write Track";
        default:   return "?";
    }
}


/**
 * @brief Dekóduje FDC event - jak REGISTER_WRITE, tak COMMAND_ISSUED subtypy.
 *
 * REGISTER_WRITE payload: [0]=addr offset (0..7, viz Sharp FDC mapping),
 *   [1]=raw byte. Chip-internal offsety 0..3 jsou: 0=CMD/STATUS, 1=TRACK,
 *   2=SECTOR, 3=DATA. Offsety 4..7 jsou external Sharp logic (side select,
 *   motor, ...).
 *
 * COMMAND_ISSUED payload (4 B primary - eventlog ring):
 *   [0]=cmd_type (@ref en_WD279X_COMMAND_TYPE), [1]=side (0/1),
 *   [2]=track_reg, [3]=sector_reg. Flags + raw command byte (5./6. byte
 *   hwlog payloadu) v eventlog NEjsou - dostupné jen v hwlog disk chunku.
 */
static void decode_fdc ( uint8_t subtype, uint32_t payload,
                         char *buf, size_t buf_len )
{
    if ( subtype == HWLOG_FDC_COMMAND_ISSUED ) {
        const unsigned cmd_type = (unsigned) ( payload & 0xFFu );
        const unsigned side     = (unsigned) ( ( payload >> 8 ) & 0xFFu );
        const unsigned track    = (unsigned) ( ( payload >> 16 ) & 0xFFu );
        const unsigned sector   = (unsigned) ( ( payload >> 24 ) & 0xFFu );
        const char *name = wd279x_command_type_name (
            (en_WD279X_COMMAND_TYPE) cmd_type );
        snprintf ( buf, buf_len, "%s side=%u T=%u S=%u",
                   name, side, track, sector );
        return;
    }
    if ( subtype != HWLOG_FDC_REGISTER_WRITE ) {
        snprintf ( buf, buf_len, "sub=%u payload=0x%08X",
                   (unsigned) subtype, (unsigned) payload );
        return;
    }
    const unsigned addr  = (unsigned) ( payload & 0xFFu );
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    const char *reg;
    switch ( addr & 0x07u ) {
        case 0: reg = "CMD";    break;
        case 1: reg = "TRACK";  break;
        case 2: reg = "SECTOR"; break;
        case 3: reg = "DATA";   break;
        case 4: reg = "EXT4";   break;
        case 5: reg = "EXT5";   break;
        case 6: reg = "EXT6";   break;
        case 7: reg = "EXT7";   break;
        default: reg = "?";     break;
    }
    if ( ( addr & 0x07u ) == 0 ) {
        snprintf ( buf, buf_len, "reg=%u (%s)=0x%02X (%s)",
                   addr, reg, value, decode_fdc_command ( (uint8_t) value ) );
    } else {
        snprintf ( buf, buf_len, "reg=%u (%s)=0x%02X", addr, reg, value );
    }
}


/**
 * @brief Dekóduje MEMEXT event (BANK_SWITCH).
 *
 * Payload [0]=addr_point (bus page 0..15), [1]=value, [2]=type (0=Luftner,
 * 1=Pehu).
 */
static void decode_memext ( uint8_t subtype, uint32_t payload,
                            char *buf, size_t buf_len )
{
    if ( subtype != HWLOG_MEMEXT_BANK_SWITCH ) {
        snprintf ( buf, buf_len, "sub=%u payload=0x%08X",
                   (unsigned) subtype, (unsigned) payload );
        return;
    }
    const unsigned page  = (unsigned) ( payload & 0xFFu );
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    const unsigned typ   = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    snprintf ( buf, buf_len, "page=%u bank=0x%02X (%s)",
               page, value, typ ? "Pehu" : "Luftner" );
}


/**
 * @brief Dekóduje QD (Quick Disk SIO) event.
 *
 * Payload [0]=SIO addr (0..3 = data A, data B, ctrl A, ctrl B),
 *         [1]=value.
 */
static void decode_qd ( uint8_t subtype, uint32_t payload,
                        char *buf, size_t buf_len )
{
    if ( subtype != HWLOG_QD_REGISTER_WRITE ) {
        snprintf ( buf, buf_len, "sub=%u payload=0x%08X",
                   (unsigned) subtype, (unsigned) payload );
        return;
    }
    const unsigned addr  = (unsigned) ( payload & 0xFFu );
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    const char *reg;
    switch ( addr & 0x03u ) {
        case 0: reg = "data A"; break;
        case 1: reg = "data B"; break;
        case 2: reg = "ctrl A"; break;
        case 3: reg = "ctrl B"; break;
        default: reg = "?";     break;
    }
    snprintf ( buf, buf_len, "reg=%u (%s)=0x%02X", addr, reg, value );
}


/**
 * @brief Dekóduje RD (Ramdisk) event (STD_WRITE / PEZIK_WRITE).
 *
 * Payload [0]=port low, [1]=value, [2]=port high (= addr high pro 0xEB std
 * variant, latch upper pro Pezik).
 */
static void decode_rd ( uint8_t subtype, uint32_t payload,
                        char *buf, size_t buf_len )
{
    const unsigned port  = (unsigned) ( payload & 0xFFu );
    const unsigned value = (unsigned) ( ( payload >> 8 ) & 0xFFu );
    const char *kind = ( subtype == HWLOG_RD_STD_WRITE )   ? "STD"
                     : ( subtype == HWLOG_RD_PEZIK_WRITE ) ? "Pezik"
                     : "?";
    snprintf ( buf, buf_len, "%s port=0x%02X val=0x%02X",
               kind, port, value );
}


/**
 * @brief Dekóduje BP_FIRE event.
 *
 * Payload bits 0..15 = bp_id, bits 16..23 = reason (en_BP_REASON).
 * Subtype = action class (HALT/MARK/CONTINUE/IGNORE/ENABLE/DISABLE).
 */
static void decode_bp_fire ( uint8_t subtype, uint32_t payload,
                             char *buf, size_t buf_len )
{
    const unsigned bp_id  = (unsigned) ( payload & 0xFFFFu );
    const unsigned reason = (unsigned) ( ( payload >> 16 ) & 0xFFu );
    const char *action;
    switch ( subtype ) {
        case EVENTLOG_BP_FIRE_SUB_HALT:     action = "HALT";    break;
        case EVENTLOG_BP_FIRE_SUB_MARK:     action = "MARK";    break;
        case EVENTLOG_BP_FIRE_SUB_CONTINUE: action = "CONT";    break;
        case EVENTLOG_BP_FIRE_SUB_IGNORE:   action = "IGN";     break;
        case EVENTLOG_BP_FIRE_SUB_ENABLE:   action = "ENABLE";  break;
        case EVENTLOG_BP_FIRE_SUB_DISABLE:  action = "DISABLE"; break;
        default:                            action = "?";       break;
    }
    snprintf ( buf, buf_len, "BP #%u reason=%u %s",
               bp_id, reason, action );
}


/**
 * @brief Dekóduje USER_MARK event - vyhledá jméno markeru přes marklog.
 *
 * Payload = marker_id (16-bit). marklog_get_name() vrátí registrovaný
 * string z internal registry; pokud nenalezeno (= test/race), padá zpět
 * na "marker_id=N".
 */
static void decode_user_mark ( uint32_t payload, char *buf, size_t buf_len )
{
    const uint16_t mid = (uint16_t) ( payload & 0xFFFFu );
    const char *name = marklog_get_name ( mid );
    if ( name && name[ 0 ] != '\0' ) {
        snprintf ( buf, buf_len, "\"%s\"", name );
    } else {
        snprintf ( buf, buf_len, "marker_id=%u", (unsigned) mid );
    }
}


/**
 * @brief Dekóduje CPU_CTRL event (HALT_ENTER/EXIT, RST 00..38).
 */
static void decode_cpu_ctrl ( uint8_t subtype, char *buf, size_t buf_len )
{
    switch ( subtype ) {
        case EVENTLOG_CPU_CTRL_SUB_HALT_ENTER:
            snprintf ( buf, buf_len, "HALT enter" );   return;
        case EVENTLOG_CPU_CTRL_SUB_HALT_EXIT:
            snprintf ( buf, buf_len, "HALT exit" );    return;
        case EVENTLOG_CPU_CTRL_SUB_RST_00:
            snprintf ( buf, buf_len, "RST 0x00" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_08:
            snprintf ( buf, buf_len, "RST 0x08" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_10:
            snprintf ( buf, buf_len, "RST 0x10" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_18:
            snprintf ( buf, buf_len, "RST 0x18" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_20:
            snprintf ( buf, buf_len, "RST 0x20" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_28:
            snprintf ( buf, buf_len, "RST 0x28" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_30:
            snprintf ( buf, buf_len, "RST 0x30" );     return;
        case EVENTLOG_CPU_CTRL_SUB_RST_38:
            snprintf ( buf, buf_len, "RST 0x38" );     return;
        default:
            snprintf ( buf, buf_len, "sub=%u", (unsigned) subtype );
            return;
    }
}


/* ===========================================================================
 *  Public API
 * =========================================================================== */

void eventlog_decode_detail ( const st_EVENTLOG_EVENT *e,
                              char *buf, size_t buf_len )
{
    if ( !buf || buf_len == 0 ) return;
    if ( !e ) { buf[ 0 ] = '\0'; return; }

    switch ( e->category ) {
        case EVENTLOG_CAT_CPU_INT:
            decode_cpu_int_state ( e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_CPU_PIN_EDGE:
            decode_cpu_pin_edge ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_IRQ_ACK_IM2:
            decode_irq_ack_im2 ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_IORQ_IN:
        case EVENTLOG_CAT_IORQ_OUT:
            decode_iorq ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_MMIO_R:
        case EVENTLOG_CAT_MMIO_W:
            decode_mmio ( e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_MODE:
            decode_gdg_mode ( e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_BANKING:
            decode_gdg_banking ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_HWSCROLL:
            decode_gdg_hwscroll ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_COLORS:
            decode_gdg_colors ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_VIDEO:
            decode_gdg_video ( e->subtype, buf, buf_len );
            return;
        case EVENTLOG_CAT_GDG_WFRF:
            decode_gdg_wfrf ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_PIO8255:
            decode_pio8255 ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_CTC8253:
            decode_ctc8253 ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_PIOZ80:
            decode_pioz80 ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_PSG:
            decode_psg ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_FDC:
            decode_fdc ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_MEMEXT:
            decode_memext ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_QD:
            decode_qd ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_RD:
            decode_rd ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_BP_FIRE:
            decode_bp_fire ( e->subtype, e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_USER_MARK:
            decode_user_mark ( e->payload, buf, buf_len );
            return;
        case EVENTLOG_CAT_CPU_CTRL:
            decode_cpu_ctrl ( e->subtype, buf, buf_len );
            return;
        case EVENTLOG_CAT_SYS:
            /* Vlna 5 Commit 31 - SYS lifecycle dekódování. */
            switch ( e->subtype ) {
                case EVENTLOG_SYS_COLD_RESET:
                    snprintf ( buf, buf_len, "cold reset" );
                    return;
                case EVENTLOG_SYS_WARM_RESET:
                    snprintf ( buf, buf_len, "warm reset" );
                    return;
                case EVENTLOG_SYS_SNAPSHOT_SAVE:
                    snprintf ( buf, buf_len, "snapshot save (hash=0x%08X)",
                               (unsigned) e->payload );
                    return;
                case EVENTLOG_SYS_SNAPSHOT_LOAD:
                    snprintf ( buf, buf_len, "snapshot load (hash=0x%08X)",
                               (unsigned) e->payload );
                    return;
                case EVENTLOG_SYS_MZF_INJECT:
                    snprintf ( buf, buf_len, "MZF inject (hash=0x%08X)",
                               (unsigned) e->payload );
                    return;
                case EVENTLOG_SYS_EMU_STARTED:
                    snprintf ( buf, buf_len, "emulator started" );
                    return;
                case EVENTLOG_SYS_EMU_STOPPED:
                    snprintf ( buf, buf_len, "emulator stopped" );
                    return;
                default:
                    snprintf ( buf, buf_len, "SYS subtype=%u payload=0x%08X",
                               (unsigned) e->subtype, (unsigned) e->payload );
                    return;
            }
        default:
            snprintf ( buf, buf_len, "payload=0x%08X",
                       (unsigned) e->payload );
            return;
    }
}


/* ===========================================================================
 *  Subtype short / full decoder (Vlna 3 Commit 22)
 *
 *  Krátké zkratky (max 8 chars) pro Sub sloupec Log tabu + plný popis
 *  pro hover tooltip nad buňkou. Implementace pure C, bez závislosti
 *  na živém emu state - lze unit-testovat z test_eventlog_decoder.c.
 * =========================================================================== */

/**
 * @brief Safe-copy literal stringu do bufferu (interní helper).
 *
 * Wrap snprintf("%s",...) aby short / full helpers měly konzistentní
 * volání nezávislé na délce literálu. Caller musí garantovat
 * @c buf != NULL && buf_len > 0.
 *
 * @param buf      Cílový buffer (NUL-terminated po návratu).
 * @param buf_len  Velikost @c buf v bytech.
 * @param s        Zdrojový literal (nesmí být @c NULL).
 */
static void sub_copy ( char *buf, size_t buf_len, const char *s )
{
    snprintf ( buf, buf_len, "%s", s );
}


void eventlog_decode_subtype_short ( uint8_t category, uint8_t subtype,
                                      char *buf, size_t buf_len )
{
    if ( !buf || buf_len == 0 ) return;

    switch ( category ) {
        case EVENTLOG_CAT_CPU_INT:
            /* Single state-record subtype - bitmap je v payloadu. */
            sub_copy ( buf, buf_len, "STATE" );
            return;

        case EVENTLOG_CAT_CPU_PIN_EDGE:
            /* subtype = INTLOG_CHIP_* hodnota (zdroj signálu na CPU INT). */
            switch ( subtype ) {
                case INTLOG_CHIP_CTC2:        sub_copy ( buf, buf_len, "CTC2"   ); return;
                case INTLOG_CHIP_PIOZ80:      sub_copy ( buf, buf_len, "PIOZ80" ); return;
                case INTLOG_CHIP_PIOZ80_PA0:  sub_copy ( buf, buf_len, "PIO_PA0" ); return;
                case INTLOG_CHIP_PIOZ80_PA1:  sub_copy ( buf, buf_len, "PIO_PA1" ); return;
                case INTLOG_CHIP_PIOZ80_PA2:  sub_copy ( buf, buf_len, "PIO_PA2" ); return;
                case INTLOG_CHIP_PIOZ80_PA3:  sub_copy ( buf, buf_len, "PIO_PA3" ); return;
                case INTLOG_CHIP_PIOZ80_PA4:  sub_copy ( buf, buf_len, "PIO_PA4" ); return;
                case INTLOG_CHIP_PIOZ80_PA5:  sub_copy ( buf, buf_len, "PIO_PA5" ); return;
                case INTLOG_CHIP_PIOZ80_PA6:  sub_copy ( buf, buf_len, "PIO_PA6" ); return;
                case INTLOG_CHIP_PIOZ80_PA7:  sub_copy ( buf, buf_len, "PIO_PA7" ); return;
                case INTLOG_CHIP_PIOZ80_PB0:  sub_copy ( buf, buf_len, "PIO_PB0" ); return;
                case INTLOG_CHIP_PIOZ80_PB1:  sub_copy ( buf, buf_len, "PIO_PB1" ); return;
                case INTLOG_CHIP_PIOZ80_PB2:  sub_copy ( buf, buf_len, "PIO_PB2" ); return;
                case INTLOG_CHIP_PIOZ80_PB3:  sub_copy ( buf, buf_len, "PIO_PB3" ); return;
                case INTLOG_CHIP_PIOZ80_PB4:  sub_copy ( buf, buf_len, "PIO_PB4" ); return;
                case INTLOG_CHIP_PIOZ80_PB5:  sub_copy ( buf, buf_len, "PIO_PB5" ); return;
                case INTLOG_CHIP_PIOZ80_PB6:  sub_copy ( buf, buf_len, "PIO_PB6" ); return;
                case INTLOG_CHIP_PIOZ80_PB7:  sub_copy ( buf, buf_len, "PIO_PB7" ); return;
                case INTLOG_CHIP_FDC:         sub_copy ( buf, buf_len, "FDC"    ); return;
            }
            break;

        case EVENTLOG_CAT_IRQ_ACK_IM2:
            /* subtype = source chip ID. */
            switch ( subtype ) {
                case INTLOG_CHIP_PIOZ80_PORT_A:
                    sub_copy ( buf, buf_len, "PIO_A" ); return;
                case INTLOG_CHIP_PIOZ80_PORT_B:
                    sub_copy ( buf, buf_len, "PIO_B" ); return;
                case INTLOG_CHIP_CTC2:
                    sub_copy ( buf, buf_len, "CTC2" ); return;
                case INTLOG_CHIP_VECTOR_BUS_LATCH:
                    sub_copy ( buf, buf_len, "BUSLAT" ); return;
            }
            break;

        case EVENTLOG_CAT_IORQ_IN:
        case EVENTLOG_CAT_IORQ_OUT:
            switch ( subtype ) {
                case EVENTLOG_IORQ_SUB_NORMAL:
                    sub_copy ( buf, buf_len, "NORMAL" ); return;
                case EVENTLOG_IORQ_SUB_UNCONNECTED:
                    sub_copy ( buf, buf_len, "UNCONN" ); return;
            }
            break;

        case EVENTLOG_CAT_BP_FIRE:
            switch ( subtype ) {
                case EVENTLOG_BP_FIRE_SUB_HALT:     sub_copy ( buf, buf_len, "HALT"    ); return;
                case EVENTLOG_BP_FIRE_SUB_MARK:     sub_copy ( buf, buf_len, "MARK"    ); return;
                case EVENTLOG_BP_FIRE_SUB_CONTINUE: sub_copy ( buf, buf_len, "CONT"    ); return;
                case EVENTLOG_BP_FIRE_SUB_IGNORE:   sub_copy ( buf, buf_len, "IGN"     ); return;
                case EVENTLOG_BP_FIRE_SUB_ENABLE:   sub_copy ( buf, buf_len, "ENABLE"  ); return;
                case EVENTLOG_BP_FIRE_SUB_DISABLE:  sub_copy ( buf, buf_len, "DISABLE" ); return;
            }
            break;

        case EVENTLOG_CAT_CPU_CTRL:
            switch ( subtype ) {
                case EVENTLOG_CPU_CTRL_SUB_HALT_ENTER: sub_copy ( buf, buf_len, "HALT_ENT" ); return;
                case EVENTLOG_CPU_CTRL_SUB_HALT_EXIT:  sub_copy ( buf, buf_len, "HALT_EXT" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_00:     sub_copy ( buf, buf_len, "RST 00h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_08:     sub_copy ( buf, buf_len, "RST 08h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_10:     sub_copy ( buf, buf_len, "RST 10h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_18:     sub_copy ( buf, buf_len, "RST 18h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_20:     sub_copy ( buf, buf_len, "RST 20h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_28:     sub_copy ( buf, buf_len, "RST 28h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_30:     sub_copy ( buf, buf_len, "RST 30h"  ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_38:     sub_copy ( buf, buf_len, "RST 38h"  ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_COLORS:
            switch ( subtype ) {
                case HWLOG_GDG_COLORS_BORDER:      sub_copy ( buf, buf_len, "BORDER" ); return;
                case HWLOG_GDG_COLORS_PALGRP:      sub_copy ( buf, buf_len, "PALGRP" ); return;
                case HWLOG_GDG_COLORS_PAL:         sub_copy ( buf, buf_len, "PAL"    ); return;
                case HWLOG_GDG_COLORS_PCG:         sub_copy ( buf, buf_len, "PCG"    ); return;
                case HWLOG_GDG_COLORS_PACKETGROUP: sub_copy ( buf, buf_len, "PKTGRP" ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_BANKING:
            /* subtype = port low byte E0..E6 - kompaktní "E<hex>". */
            snprintf ( buf, buf_len, "E%X", (unsigned) ( subtype & 0xFu ) );
            return;

        case EVENTLOG_CAT_GDG_HWSCROLL:
            /* subtype = HW scroll register kind (per gdg implementace). */
            snprintf ( buf, buf_len, "REG%u", (unsigned) subtype );
            return;

        case EVENTLOG_CAT_GDG_VIDEO:
            switch ( subtype ) {
                case HWLOG_GDG_VIDEO_VBLN_START: sub_copy ( buf, buf_len, "VBLN_S" ); return;
                case HWLOG_GDG_VIDEO_VBLN_END:   sub_copy ( buf, buf_len, "VBLN_E" ); return;
                case HWLOG_GDG_VIDEO_VS_START:   sub_copy ( buf, buf_len, "VS_S"   ); return;
                case HWLOG_GDG_VIDEO_VS_END:     sub_copy ( buf, buf_len, "VS_E"   ); return;
                case HWLOG_GDG_VIDEO_HBLN_START: sub_copy ( buf, buf_len, "HBLN_S" ); return;
                case HWLOG_GDG_VIDEO_HBLN_END:   sub_copy ( buf, buf_len, "HBLN_E" ); return;
                case HWLOG_GDG_VIDEO_HS_START:   sub_copy ( buf, buf_len, "HS_S"   ); return;
                case HWLOG_GDG_VIDEO_HS_END:     sub_copy ( buf, buf_len, "HS_E"   ); return;
            }
            break;

        case EVENTLOG_CAT_PIO8255:
            switch ( subtype ) {
                case HWLOG_PIO8255_PORT_A_WRITE:  sub_copy ( buf, buf_len, "PORT_A" ); return;
                case HWLOG_PIO8255_PORT_B_WRITE:  sub_copy ( buf, buf_len, "PORT_B" ); return;
                case HWLOG_PIO8255_PORT_C_WRITE:  sub_copy ( buf, buf_len, "PORT_C" ); return;
                case HWLOG_PIO8255_CONTROL_WRITE: sub_copy ( buf, buf_len, "CTRL_W" ); return;
            }
            break;

        case EVENTLOG_CAT_CTC8253:
            switch ( subtype ) {
                case HWLOG_CTC8253_CONTROL_WRITE: sub_copy ( buf, buf_len, "CTRL_W" ); return;
                case HWLOG_CTC8253_COUNTER_WRITE: sub_copy ( buf, buf_len, "CNT_W"  ); return;
            }
            break;

        case EVENTLOG_CAT_PIOZ80:
            switch ( subtype ) {
                case HWLOG_PIOZ80_MODE_WRITE:       sub_copy ( buf, buf_len, "MODE_W"  ); return;
                case HWLOG_PIOZ80_VECTOR_WRITE:     sub_copy ( buf, buf_len, "VECT_W"  ); return;
                case HWLOG_PIOZ80_INT_CTRL_WRITE:   sub_copy ( buf, buf_len, "ICW_W"   ); return;
                case HWLOG_PIOZ80_MASK_WRITE:       sub_copy ( buf, buf_len, "MASK_W"  ); return;
                case HWLOG_PIOZ80_IO_SELECT_WRITE:  sub_copy ( buf, buf_len, "IOSEL_W" ); return;
                case HWLOG_PIOZ80_DATA_WRITE:       sub_copy ( buf, buf_len, "DATA_W"  ); return;
                case HWLOG_PIOZ80_DATA_READ:        sub_copy ( buf, buf_len, "DATA_R"  ); return;
                case HWLOG_PIOZ80_BUS_INPUT_CHANGE: sub_copy ( buf, buf_len, "BUSIN"   ); return;
                case HWLOG_PIOZ80_IRQ_ACK_M2:       sub_copy ( buf, buf_len, "IRQACK"  ); return;
                case HWLOG_PIOZ80_RETI_APPLIED:     sub_copy ( buf, buf_len, "RETI"    ); return;
            }
            break;

        case EVENTLOG_CAT_PSG:
        case EVENTLOG_CAT_QD:
            /* Tyto kategorie mají jediný REGISTER_WRITE subtype (= 0x01).
             * Konstanty HWLOG_PSG_REGISTER_WRITE / HWLOG_QD_REGISTER_WRITE
             * jsou shodné, prezentace stejná. */
            sub_copy ( buf, buf_len, "REG_W" );
            return;

        case EVENTLOG_CAT_FDC:
            switch ( subtype ) {
                case HWLOG_FDC_REGISTER_WRITE: sub_copy ( buf, buf_len, "REG_W" ); return;
                case HWLOG_FDC_COMMAND_ISSUED: sub_copy ( buf, buf_len, "CMD"   ); return;
            }
            break;

        case EVENTLOG_CAT_RD:
            switch ( subtype ) {
                case HWLOG_RD_STD_WRITE:   sub_copy ( buf, buf_len, "STD_W" ); return;
                case HWLOG_RD_PEZIK_WRITE: sub_copy ( buf, buf_len, "PEZIK" ); return;
            }
            break;

        case EVENTLOG_CAT_MEMEXT:
            /* Aktuálně jediný subtype = BANK_SWITCH. */
            sub_copy ( buf, buf_len, "BANK_W" );
            return;

        case EVENTLOG_CAT_USER_MARK:
            /* Marker name je v payloadu / Detail sloupci - Sub zkratka. */
            sub_copy ( buf, buf_len, "MARK" );
            return;

        case EVENTLOG_CAT_SYS:
            switch ( subtype ) {
                case EVENTLOG_SYS_COLD_RESET:    sub_copy ( buf, buf_len, "COLD_RST" ); return;
                case EVENTLOG_SYS_WARM_RESET:    sub_copy ( buf, buf_len, "WARM_RST" ); return;
                case EVENTLOG_SYS_SNAPSHOT_SAVE: sub_copy ( buf, buf_len, "SNAP_SAV" ); return;
                case EVENTLOG_SYS_SNAPSHOT_LOAD: sub_copy ( buf, buf_len, "SNAP_LD"  ); return;
                case EVENTLOG_SYS_MZF_INJECT:    sub_copy ( buf, buf_len, "MZF_IN"   ); return;
                case EVENTLOG_SYS_EMU_STARTED:   sub_copy ( buf, buf_len, "STARTED"  ); return;
                case EVENTLOG_SYS_EMU_STOPPED:   sub_copy ( buf, buf_len, "STOPPED"  ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_MODE:
        case EVENTLOG_CAT_MMIO_R:
        case EVENTLOG_CAT_MMIO_W:
        case EVENTLOG_CAT_GDG_WFRF:
            /* Žádný smysluplný subtype - jediný typ eventu per kategorie. */
            sub_copy ( buf, buf_len, "-" );
            return;

        default:
            break;
    }

    /* Fallback - neznámá kombinace (cat / subtype). */
    snprintf ( buf, buf_len, "%u", (unsigned) subtype );
}


void eventlog_decode_subtype_full ( uint8_t category, uint8_t subtype,
                                     char *buf, size_t buf_len )
{
    if ( !buf || buf_len == 0 ) return;

    switch ( category ) {
        case EVENTLOG_CAT_CPU_INT:
            sub_copy ( buf, buf_len, "CPU interrupt state change" );
            return;

        case EVENTLOG_CAT_CPU_PIN_EDGE:
            switch ( subtype ) {
                case INTLOG_CHIP_CTC2:
                    sub_copy ( buf, buf_len, "CTC2 INT pin edge" ); return;
                case INTLOG_CHIP_PIOZ80:
                    sub_copy ( buf, buf_len, "PIOZ80 INT pin edge (chip-level)" ); return;
                case INTLOG_CHIP_PIOZ80_PA0:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 0 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PA1:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 1 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PA2:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 2 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PA3:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 3 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PA4:
                    sub_copy ( buf, buf_len, "PIOZ80 PA4 pin edge (CTC0 input, inverted)" ); return;
                case INTLOG_CHIP_PIOZ80_PA5:
                    sub_copy ( buf, buf_len, "PIOZ80 PA5 pin edge (VBLN)" ); return;
                case INTLOG_CHIP_PIOZ80_PA6:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 6 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PA7:
                    sub_copy ( buf, buf_len, "PIOZ80 Port A bit 7 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB0:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 0 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB1:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 1 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB2:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 2 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB3:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 3 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB4:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 4 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB5:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 5 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB6:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 6 pin edge" ); return;
                case INTLOG_CHIP_PIOZ80_PB7:
                    sub_copy ( buf, buf_len, "PIOZ80 Port B bit 7 pin edge" ); return;
                case INTLOG_CHIP_FDC:
                    sub_copy ( buf, buf_len, "FDC INT pin edge" ); return;
            }
            break;

        case EVENTLOG_CAT_IRQ_ACK_IM2:
            switch ( subtype ) {
                case INTLOG_CHIP_PIOZ80_PORT_A:
                    sub_copy ( buf, buf_len, "IM 2 IRQ ack from PIOZ80 Port A" ); return;
                case INTLOG_CHIP_PIOZ80_PORT_B:
                    sub_copy ( buf, buf_len, "IM 2 IRQ ack from PIOZ80 Port B" ); return;
                case INTLOG_CHIP_CTC2:
                    sub_copy ( buf, buf_len, "IM 2 IRQ ack from CTC2" ); return;
                case INTLOG_CHIP_VECTOR_BUS_LATCH:
                    sub_copy ( buf, buf_len, "IM 2 IRQ ack via floating bus latch" ); return;
            }
            break;

        case EVENTLOG_CAT_IORQ_IN:
            switch ( subtype ) {
                case EVENTLOG_IORQ_SUB_NORMAL:
                    sub_copy ( buf, buf_len, "IN from mapped port" ); return;
                case EVENTLOG_IORQ_SUB_UNCONNECTED:
                    sub_copy ( buf, buf_len, "IN from unmapped port (ghost read)" ); return;
            }
            break;

        case EVENTLOG_CAT_IORQ_OUT:
            switch ( subtype ) {
                case EVENTLOG_IORQ_SUB_NORMAL:
                    sub_copy ( buf, buf_len, "OUT to mapped port" ); return;
                case EVENTLOG_IORQ_SUB_UNCONNECTED:
                    sub_copy ( buf, buf_len, "OUT to unmapped port (ghost write)" ); return;
            }
            break;

        case EVENTLOG_CAT_BP_FIRE:
            switch ( subtype ) {
                case EVENTLOG_BP_FIRE_SUB_HALT:
                    sub_copy ( buf, buf_len, "Breakpoint fired - halt emulator" ); return;
                case EVENTLOG_BP_FIRE_SUB_MARK:
                    sub_copy ( buf, buf_len, "Breakpoint fired - mark action" ); return;
                case EVENTLOG_BP_FIRE_SUB_CONTINUE:
                    sub_copy ( buf, buf_len, "Breakpoint fired - continue action (log/poke/set/var)" ); return;
                case EVENTLOG_BP_FIRE_SUB_IGNORE:
                    sub_copy ( buf, buf_len, "Breakpoint fired - ignore action (reserved)" ); return;
                case EVENTLOG_BP_FIRE_SUB_ENABLE:
                    sub_copy ( buf, buf_len, "Breakpoint fired - enable another BP" ); return;
                case EVENTLOG_BP_FIRE_SUB_DISABLE:
                    sub_copy ( buf, buf_len, "Breakpoint fired - disable BP (self or other)" ); return;
            }
            break;

        case EVENTLOG_CAT_CPU_CTRL:
            switch ( subtype ) {
                case EVENTLOG_CPU_CTRL_SUB_HALT_ENTER:
                    sub_copy ( buf, buf_len, "HALT instruction executed (CPU halted)" ); return;
                case EVENTLOG_CPU_CTRL_SUB_HALT_EXIT:
                    sub_copy ( buf, buf_len, "CPU resumed from HALT (IRQ/NMI woke up)" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_00:
                    sub_copy ( buf, buf_len, "RST 00h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_08:
                    sub_copy ( buf, buf_len, "RST 08h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_10:
                    sub_copy ( buf, buf_len, "RST 10h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_18:
                    sub_copy ( buf, buf_len, "RST 18h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_20:
                    sub_copy ( buf, buf_len, "RST 20h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_28:
                    sub_copy ( buf, buf_len, "RST 28h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_30:
                    sub_copy ( buf, buf_len, "RST 30h dispatched" ); return;
                case EVENTLOG_CPU_CTRL_SUB_RST_38:
                    sub_copy ( buf, buf_len, "RST 38h dispatched" ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_COLORS:
            switch ( subtype ) {
                case HWLOG_GDG_COLORS_BORDER:
                    sub_copy ( buf, buf_len, "Border color write" ); return;
                case HWLOG_GDG_COLORS_PALGRP:
                    sub_copy ( buf, buf_len, "Palette group select write" ); return;
                case HWLOG_GDG_COLORS_PAL:
                    sub_copy ( buf, buf_len, "Palette register write (PAL[0..3])" ); return;
                case HWLOG_GDG_COLORS_PCG:
                    sub_copy ( buf, buf_len, "PCG control write" ); return;
                case HWLOG_GDG_COLORS_PACKETGROUP:
                    sub_copy ( buf, buf_len, "Packet group write" ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_BANKING:
            switch ( subtype ) {
                case HWLOG_GDG_BANKING_E0:
                    sub_copy ( buf, buf_len, "Banking port 0xE0 (ROM bottom OFF / VRAM down)" ); return;
                case HWLOG_GDG_BANKING_E1:
                    sub_copy ( buf, buf_len, "Banking port 0xE1 (ROM upper OFF)" ); return;
                case HWLOG_GDG_BANKING_E2:
                    sub_copy ( buf, buf_len, "Banking port 0xE2 (ROM 0000 ON, reset map)" ); return;
                case HWLOG_GDG_BANKING_E3:
                    sub_copy ( buf, buf_len, "Banking port 0xE3 (ROM upper ON)" ); return;
                case HWLOG_GDG_BANKING_E4:
                    sub_copy ( buf, buf_len, "Banking port 0xE4 (ALL ON, reset all)" ); return;
                case HWLOG_GDG_BANKING_E5:
                    sub_copy ( buf, buf_len, "Banking port 0xE5 (EXROM/SPEC)" ); return;
                case HWLOG_GDG_BANKING_E6:
                    sub_copy ( buf, buf_len, "Banking port 0xE6 (EXROM/SPEC OFF)" ); return;
            }
            break;

        case EVENTLOG_CAT_GDG_HWSCROLL:
            /* HW scroll register - per gdg implementace; full popis dohledá UI z payloadu. */
            snprintf ( buf, buf_len,
                       "GDG HW scroll register write (kind %u)",
                       (unsigned) subtype );
            return;

        case EVENTLOG_CAT_GDG_VIDEO:
            switch ( subtype ) {
                case HWLOG_GDG_VIDEO_VBLN_START:
                    sub_copy ( buf, buf_len, "Vertical blanking start" ); return;
                case HWLOG_GDG_VIDEO_VBLN_END:
                    sub_copy ( buf, buf_len, "Vertical blanking end" ); return;
                case HWLOG_GDG_VIDEO_VS_START:
                    sub_copy ( buf, buf_len, "VSYNC start" ); return;
                case HWLOG_GDG_VIDEO_VS_END:
                    sub_copy ( buf, buf_len, "VSYNC end" ); return;
                case HWLOG_GDG_VIDEO_HBLN_START:
                    sub_copy ( buf, buf_len, "Horizontal blanking start" ); return;
                case HWLOG_GDG_VIDEO_HBLN_END:
                    sub_copy ( buf, buf_len, "Horizontal blanking end" ); return;
                case HWLOG_GDG_VIDEO_HS_START:
                    sub_copy ( buf, buf_len, "HSYNC start" ); return;
                case HWLOG_GDG_VIDEO_HS_END:
                    sub_copy ( buf, buf_len, "HSYNC end" ); return;
            }
            break;

        case EVENTLOG_CAT_PIO8255:
            switch ( subtype ) {
                case HWLOG_PIO8255_PORT_A_WRITE:
                    sub_copy ( buf, buf_len, "8255 PPI Port A write" ); return;
                case HWLOG_PIO8255_PORT_B_WRITE:
                    sub_copy ( buf, buf_len, "8255 PPI Port B write" ); return;
                case HWLOG_PIO8255_PORT_C_WRITE:
                    sub_copy ( buf, buf_len, "8255 PPI Port C write" ); return;
                case HWLOG_PIO8255_CONTROL_WRITE:
                    sub_copy ( buf, buf_len, "8255 PPI Control Word write" ); return;
            }
            break;

        case EVENTLOG_CAT_CTC8253:
            switch ( subtype ) {
                case HWLOG_CTC8253_CONTROL_WRITE:
                    sub_copy ( buf, buf_len, "8253 CTC Control Word write" ); return;
                case HWLOG_CTC8253_COUNTER_WRITE:
                    sub_copy ( buf, buf_len, "8253 CTC Counter register write" ); return;
            }
            break;

        case EVENTLOG_CAT_PIOZ80:
            switch ( subtype ) {
                case HWLOG_PIOZ80_MODE_WRITE:
                    sub_copy ( buf, buf_len, "Mode Control Word write" ); return;
                case HWLOG_PIOZ80_VECTOR_WRITE:
                    sub_copy ( buf, buf_len, "Interrupt Vector write" ); return;
                case HWLOG_PIOZ80_INT_CTRL_WRITE:
                    sub_copy ( buf, buf_len, "Interrupt Control word (ICW) write" ); return;
                case HWLOG_PIOZ80_MASK_WRITE:
                    sub_copy ( buf, buf_len, "Mask Follows (per-pin mask) write" ); return;
                case HWLOG_PIOZ80_IO_SELECT_WRITE:
                    sub_copy ( buf, buf_len, "I/O Select Mask write" ); return;
                case HWLOG_PIOZ80_DATA_WRITE:
                    sub_copy ( buf, buf_len, "OUT to data port" ); return;
                case HWLOG_PIOZ80_DATA_READ:
                    sub_copy ( buf, buf_len, "IN from data port" ); return;
                case HWLOG_PIOZ80_BUS_INPUT_CHANGE:
                    sub_copy ( buf, buf_len, "External PA/PB pin change" ); return;
                case HWLOG_PIOZ80_IRQ_ACK_M2:
                    sub_copy ( buf, buf_len, "IM 2 IRQ acknowledge (vector returned)" ); return;
                case HWLOG_PIOZ80_RETI_APPLIED:
                    sub_copy ( buf, buf_len, "RETI propagated through PIO daisy chain" ); return;
            }
            break;

        case EVENTLOG_CAT_PSG:
            sub_copy ( buf, buf_len, "SN76489 PSG register write" );
            return;

        case EVENTLOG_CAT_FDC:
            switch ( subtype ) {
                case HWLOG_FDC_REGISTER_WRITE:
                    sub_copy ( buf, buf_len, "WD279x FDC register write" );
                    return;
                case HWLOG_FDC_COMMAND_ISSUED:
                    sub_copy ( buf, buf_len, "WD279x command dispatch" );
                    return;
            }
            break;

        case EVENTLOG_CAT_QD:
            sub_copy ( buf, buf_len, "Quick Disk register write" );
            return;

        case EVENTLOG_CAT_RD:
            switch ( subtype ) {
                case HWLOG_RD_STD_WRITE:
                    sub_copy ( buf, buf_len, "Standard Ramdisk write (port 0xFA)" ); return;
                case HWLOG_RD_PEZIK_WRITE:
                    sub_copy ( buf, buf_len, "Pezik Ramdisk write (ports 0xE8/0xEC-0xEF)" ); return;
            }
            break;

        case EVENTLOG_CAT_MEMEXT:
            sub_copy ( buf, buf_len, "Memory extension bank switch" );
            return;

        case EVENTLOG_CAT_USER_MARK:
            sub_copy ( buf, buf_len, "User mark (named marker)" );
            return;

        case EVENTLOG_CAT_GDG_MODE:
            sub_copy ( buf, buf_len, "GDG Display Mode Descriptor (DMD) write" );
            return;

        case EVENTLOG_CAT_MMIO_R:
            sub_copy ( buf, buf_len, "MMIO read (0xE000-0xE008 region)" );
            return;

        case EVENTLOG_CAT_MMIO_W:
            sub_copy ( buf, buf_len, "MMIO write (0xE000-0xE008 region)" );
            return;

        case EVENTLOG_CAT_GDG_WFRF:
            sub_copy ( buf, buf_len, "GDG Write Format / Read Format register write" );
            return;

        case EVENTLOG_CAT_SYS:
            switch ( subtype ) {
                case EVENTLOG_SYS_COLD_RESET:
                    sub_copy ( buf, buf_len, "Cold reset (power-on / restart)" ); return;
                case EVENTLOG_SYS_WARM_RESET:
                    sub_copy ( buf, buf_len, "Warm reset" ); return;
                case EVENTLOG_SYS_SNAPSHOT_SAVE:
                    sub_copy ( buf, buf_len, "Snapshot saved to disk" ); return;
                case EVENTLOG_SYS_SNAPSHOT_LOAD:
                    sub_copy ( buf, buf_len, "Snapshot loaded from disk" ); return;
                case EVENTLOG_SYS_MZF_INJECT:
                    sub_copy ( buf, buf_len, "MZF tape file injected via cmthack" ); return;
                case EVENTLOG_SYS_EMU_STARTED:
                    sub_copy ( buf, buf_len, "Emulator main loop started" ); return;
                case EVENTLOG_SYS_EMU_STOPPED:
                    sub_copy ( buf, buf_len, "Emulator main loop stopped" ); return;
            }
            break;

        default:
            break;
    }

    /* Fallback - neznámá kombinace (cat / subtype). Slož short + decimal. */
    char short_code[ 16 ];
    eventlog_decode_subtype_short ( category, subtype,
                                     short_code, sizeof ( short_code ) );
    snprintf ( buf, buf_len, "%s (subtype %u)",
               short_code, (unsigned) subtype );
}


/* ===========================================================================
 *  Ambient state decoder (Vlna 4 Commit 24)
 * =========================================================================== */

/**
 * @brief Vrátí krátký label pro reason kód (3 bity).
 *
 * Volá se z @ref eventlog_decode_ambient. Hodnoty mapují
 * @ref en_EVENTLOG_AMBIENT_REASON.
 */
static const char *ambient_reason_label ( uint8_t reason )
{
    switch ( reason ) {
        case EVENTLOG_AMBIENT_REASON_IFF_RESET:   return "RESET";
        case EVENTLOG_AMBIENT_REASON_IFF_EI:      return "EI";
        case EVENTLOG_AMBIENT_REASON_IFF_DI:      return "DI";
        case EVENTLOG_AMBIENT_REASON_IFF_INT_ACK: return "INT_ACK";
        case EVENTLOG_AMBIENT_REASON_IFF_NMI_ACK: return "NMI_ACK";
        case EVENTLOG_AMBIENT_REASON_IFF_RETI:    return "RETI";
        case EVENTLOG_AMBIENT_REASON_IFF_RETN:    return "RETN";
        case EVENTLOG_AMBIENT_REASON_NONE:        return "NONE";
        default:                                   return "?";
    }
}


/**
 * @brief Vrátí per-arch label pro banking kód (3 bity).
 *
 * Per-arch interpretace - viz tabulka v eventlog.h
 * @c en_EVENTLOG_AMBIENT_BANKING.
 */
static const char *ambient_banking_label ( uint8_t banking )
{
    /* Runtime dle g_mzhal.arch (mzhal 11f) - labely per arch
     * odpovídají per-arch klasifikaci v eventlog.c. */
    if ( g_mzhal.arch == 800 ) {
    switch ( banking ) {
        case EVENTLOG_AMBIENT_BANKING_DEFAULT:      return "DEFAULT";
        case EVENTLOG_AMBIENT_BANKING_ALL_RAM:      return "ALL_RAM";
        case EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF:  return "ROM_LOW_OFF";
        case EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF: return "ROM_HIGH_OFF";
        case EVENTLOG_AMBIENT_BANKING_CGROM:        return "CGROM";
        case EVENTLOG_AMBIENT_BANKING_VRAM_640:     return "VRAM_640";
        case EVENTLOG_AMBIENT_BANKING_OTHER:        return "OTHER";
        default:                                     return "?";
    }
    }
    if ( g_mzhal.arch == 700 ) {
    switch ( banking ) {
        case EVENTLOG_AMBIENT_BANKING_DEFAULT:      return "DEFAULT";
        case EVENTLOG_AMBIENT_BANKING_ALL_RAM:      return "ALL_RAM";
        case EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF:  return "ROM_LOW_OFF";
        case EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF: return "ROM_HIGH_OFF";
        case EVENTLOG_AMBIENT_BANKING_OTHER:        return "OTHER";
        default:                                     return "?";
    }
    }
    if ( g_mzhal.arch == 1500 ) {
    switch ( banking ) {
        case EVENTLOG_AMBIENT_BANKING_DEFAULT:      return "DEFAULT";
        case EVENTLOG_AMBIENT_BANKING_ALL_RAM:      return "ALL_RAM";
        case EVENTLOG_AMBIENT_BANKING_CGROM:        return "CGROM";
        case EVENTLOG_AMBIENT_BANKING_VRAM_640:     return "PCG_1";
        case EVENTLOG_AMBIENT_BANKING_PCG_HIGH:     return "PCG_2/3";
        case EVENTLOG_AMBIENT_BANKING_OTHER:        return "OTHER";
        default:                                     return "?";
    }
    }
    return "?";
}


void eventlog_decode_ambient ( uint16_t ambient, char *buf, size_t buf_len )
{
    if ( !buf || buf_len == 0 ) return;

    uint8_t iff1    = (uint8_t) ( ambient & EVENTLOG_AMBIENT_IFF1 ) ? 1u : 0u;
    uint8_t im      = (uint8_t) ( ( ambient & EVENTLOG_AMBIENT_IM_MASK )
                                  >> EVENTLOG_AMBIENT_IM_SHIFT );
    uint8_t reason  = (uint8_t) ( ( ambient & EVENTLOG_AMBIENT_REASON_MASK )
                                  >> EVENTLOG_AMBIENT_REASON_SHIFT );
    uint8_t banking = (uint8_t) ( ( ambient & EVENTLOG_AMBIENT_BANKING_MASK )
                                  >> EVENTLOG_AMBIENT_BANKING_SHIFT );

    /* Reason NONE = default mimo BP fire - vynech ho z textu, jinak by
     * zaplnil každý event. IFF1/IM/bank vždy uvádíme, i pro ambient=0
     * (= "IFF1=0 IM=0 bank=DEFAULT" je validní snapshot tuhle chvíli). */
    if ( reason == EVENTLOG_AMBIENT_REASON_NONE ) {
        snprintf ( buf, buf_len, "IFF1=%u IM=%u bank=%s",
                   (unsigned) iff1, (unsigned) im,
                   ambient_banking_label ( banking ) );
    } else {
        snprintf ( buf, buf_len, "IFF1=%u IM=%u reason=%s bank=%s",
                   (unsigned) iff1, (unsigned) im,
                   ambient_reason_label ( reason ),
                   ambient_banking_label ( banking ) );
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
