/* 
 * File:   pioz80.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 3. července 2015, 21:27
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


#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"

#include "pioz80/pioz80.h"
#include "printer/printer.h"
#include "mz1p16/mz1p16_emu.h"
#include "mz1p16/mcs48.h"
#include "mzarch/mzarch.h"
#include "mzarch/interrupt.h"
#include "ctc8253/ctc8253.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"
#include "memory/memory.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/intlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

//#define DBGLEVEL (DBGNON /* | DBGERR | DBGWAR | DBGINF*/)
//#define DBGLEVEL (DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

st_PIOZ80 g_pioz80;

/* Debug/UI mirror posledniho byte na control port per port (viz pioz80.h).
 * Aktualizovano v pioz80_write_byte pri zapisu na PIOZ80_ADDRTYPE_CTRL.
 * Default 0x00 do prvniho HW init writu. */
uint8_t g_pioz80_port_a_last_ctrl_byte = 0x00;
uint8_t g_pioz80_port_b_last_ctrl_byte = 0x00;


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/**
 * Snapshot relevantních polí PIOZ80 stavu pro výpočet hwlog delta bitmasky.
 * Vyplní se před změnou, po změně se porovná s aktuálním g_pioz80 a sestaví
 * decoded_state_delta_bitmask v rámci payloadu hwlog eventu.
 */
typedef struct st_PIOZ80_HWLOG_SNAPSHOT
{
    uint8_t mode;
    uint8_t io_mask;
    uint8_t icmask;
    uint8_t icena;
    uint8_t icfnc;
    uint8_t iclvl;
    uint8_t interrupt_vector;
    uint8_t data_output;
    uint8_t port_int;
    uint8_t masked_input;
    uint8_t ctrl_expect;
    uint8_t glob_interrupt;
    int8_t  glob_interrupt_port_id;
} st_PIOZ80_HWLOG_SNAPSHOT;


/**
 * Vytvoří snapshot daného portu + globálních polí PIOZ80 do struktury
 * st_PIOZ80_HWLOG_SNAPSHOT. Snapshot nezachycuje ukazatele, jen primitivní
 * hodnoty - lze bezpečně porovnat memcmp / per-pole.
 *
 * @param snap   výstupní snapshot (vyplněno funkcí)
 * @param port   ukazatel na port jehož stav má být snapshotován
 */
static inline void pioz80_hwlog_take_snapshot ( st_PIOZ80_HWLOG_SNAPSHOT *snap,
                                                const st_PIOZ80_PORT *port )
{
    snap->mode = (uint8_t) port->mode;
    snap->io_mask = port->io_mask;
    snap->icmask = port->icmask;
    snap->icena = (uint8_t) port->icena;
    snap->icfnc = (uint8_t) port->icfnc;
    snap->iclvl = (uint8_t) port->iclvl;
    snap->interrupt_vector = port->interrupt_vector;
    snap->data_output = port->data_output;
    snap->port_int = (uint8_t) port->port_int;
    snap->masked_input = port->masked_input;
    snap->ctrl_expect = (uint8_t) port->ctrl_expect;
    snap->glob_interrupt = (uint8_t) g_pioz80.interrupt;
    snap->glob_interrupt_port_id = (int8_t) g_pioz80.interrupt_port_id;
}


/**
 * Sestaví decoded_state_delta_bitmask (24 b) porovnáním snapshot before
 * vs aktuálního stavu portu / globálu. Bity = HWLOG_PIOZ80_DELTA_*.
 *
 * @param snap   snapshot pořízený před změnou
 * @param port   port jehož aktuální stav se porovná
 * @return       24-bit bitmask zachycujicí změněná pole
 */
static inline uint32_t pioz80_hwlog_compute_delta ( const st_PIOZ80_HWLOG_SNAPSHOT *snap,
                                                    const st_PIOZ80_PORT *port )
{
    uint32_t delta = 0;
    if ( snap->mode != (uint8_t) port->mode ) delta |= HWLOG_PIOZ80_DELTA_MODE;
    if ( snap->io_mask != port->io_mask ) delta |= HWLOG_PIOZ80_DELTA_IO_MASK;
    if ( snap->icmask != port->icmask ) delta |= HWLOG_PIOZ80_DELTA_ICMASK;
    if ( snap->icena != (uint8_t) port->icena ) delta |= HWLOG_PIOZ80_DELTA_ICENA;
    if ( snap->icfnc != (uint8_t) port->icfnc ) delta |= HWLOG_PIOZ80_DELTA_ICFNC;
    if ( snap->iclvl != (uint8_t) port->iclvl ) delta |= HWLOG_PIOZ80_DELTA_ICLVL;
    if ( snap->interrupt_vector != port->interrupt_vector ) delta |= HWLOG_PIOZ80_DELTA_VECTOR;
    if ( snap->data_output != port->data_output ) delta |= HWLOG_PIOZ80_DELTA_DATA_OUT;
    if ( snap->port_int != (uint8_t) port->port_int ) delta |= HWLOG_PIOZ80_DELTA_PORT_INT;
    if ( snap->masked_input != port->masked_input ) delta |= HWLOG_PIOZ80_DELTA_MASKED_IN;
    if ( snap->ctrl_expect != (uint8_t) port->ctrl_expect ) delta |= HWLOG_PIOZ80_DELTA_CTRL_EXPECT;
    if ( snap->glob_interrupt != (uint8_t) g_pioz80.interrupt ) delta |= HWLOG_PIOZ80_DELTA_INT_GLOBAL;
    if ( snap->glob_interrupt_port_id != (int8_t) g_pioz80.interrupt_port_id ) delta |= HWLOG_PIOZ80_DELTA_INT_PORT_ID;
    return delta;
}


/**
 * Emit jednoho hwlog PIOZ80 eventu s payloadem 6 B.
 *
 * Layout payloadu:
 *  [0]   port_id (0=A, 1=B, 0xFF=N/A)
 *  [1]   addr / sub-addr (per sub-event)
 *  [2]   value
 *  [3..5] decoded_state_delta_bitmask (24 b LE, viz HWLOG_PIOZ80_DELTA_*)
 *
 * @param sub      sub-event typ (en_HWLOG_PIOZ80_SUB)
 * @param port_id  identifikátor portu nebo 0xFF
 * @param sub_addr addr / dispatch byte / N/A v závislosti na sub-event
 * @param value    raw value zapsaný / přečtený / pin level
 * @param delta    decoded_state_delta_bitmask (max 24 b)
 */
static inline void pioz80_hwlog_emit ( en_HWLOG_PIOZ80_SUB sub,
                                       uint8_t port_id, uint8_t sub_addr,
                                       uint8_t value, uint32_t delta )
{
    if ( !TEST_TRACE_HWLOG_DISPATCH ) return;
    uint8_t payload[ 6 ];
    payload[ 0 ] = port_id;
    payload[ 1 ] = sub_addr;
    payload[ 2 ] = value;
    payload[ 3 ] = (uint8_t) ( delta & 0xff );
    payload[ 4 ] = (uint8_t) ( ( delta >> 8 ) & 0xff );
    payload[ 5 ] = (uint8_t) ( ( delta >> 16 ) & 0xff );
    hwlog_record ( HWLOG_CHIP_PIOZ80, (uint8_t) sub, payload );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */


#if ( DBGLEVEL )


/*******************************************************************************
 * 
 * 
 * Nastroje pro DBGPRINT
 *  
 * 
 ******************************************************************************/


static inline char pioz80_dbg_get_port_name ( en_PIOZ80_PORT_ID value ) {
    return ( 'A' + value );
}


static inline const char* pioz80_dbg_get_icfnc_name ( en_PIOZ80_ICFNC value ) {
    if ( value == PIOZ80_ICFNC_OR ) return "Or";
    return "And";
}


static inline const char* pioz80_dbg_get_iclvl_name ( en_PIOZ80_ICLVL value ) {
    if ( value == PIOZ80_ICLVL_LOW ) return "Low";
    return "High";
}


static inline const char* pioz80_dbg_get_icena_status ( en_PIOZ80_ICENA value ) {
    if ( value == PIOZ80_ICENA_DISABLED ) return "Disabled";
    return "Enabled";
}


static inline char* pioz80_dbg_get_fullmask_string ( uint8_t io_mask, uint8_t icmask ) {
    static char retval[9];
    int i;
    char *c = &retval[7];
    for ( i = 0; i < 8; i++ ) {
        *c = ( io_mask & 0x01 ) ? 'i' : 'o'; // i - input, o - output
        if ( !( icmask & 0x01 ) ) {
            *c -= 0x20; // I - int input, O - int output
        };
        io_mask >>= 1;
        icmask >>= 1;
        c--;
    };
    return retval;
}


static inline char* pioz80_dbg_get_byte_string ( uint8_t value ) {
    static char retval[9];
    int i;
    char *c = &retval[7];
    for ( i = 0; i < 8; i++ ) {
        *c = ( value & 0x01 ) ? '1' : '0';
        value >>= 1;
        c--;
    };
    return retval;
}


static inline const char* pioz80_dbg_get_intstate_string ( en_PIOZ80_INTERRUPT value ) {
    const char *retval;
    switch ( value ) {
        case PIOZ80_INTERRUPT_NONE:
            retval = "INTERRUPT_NONE";
            break;

        case PIOZ80_INTERRUPT_PENDING:
            retval = "INTERRUPT_PENDING";
            break;

        case PIOZ80_INTERRUPT_NEXTPRIO:
            retval = "INTERRUPT_NEXTPRIO";
            break;

        case PIOZ80_INTERRUPT_RECEIVED:
            retval = "INTERRUPT_RECEIVED";
            break;

        default:
            retval = "UNKNOWN";
            break;
    };
    return retval;
}


static inline const char* pioz80_dbg_get_icfnc_result_string ( en_PIOZ80_ICFNC_RESULT value ) {
    if ( value == PIOZ80_INTFNC_RESULT_FALSE ) {
        return "FALSE";
    };
    return "TRUE";
}


static inline const char* pioz80_dbg_get_port_event_name ( en_PIOZ80_PORT_EVENT value ) {
    const char *retval;
    switch ( value ) {

        case PIOZ80_PORT_EVENT_PA4_CTC0:
            retval = "PA4_CTC0";
            break;

        case PIOZ80_PORT_EVENT_PA5_VBLN:
            retval = "PA5_VBLN";
            break;

        case PIOZ80_PORT_EVENT_PA01_PRINTER:
            retval = "PA01_PRINTER";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_MODE3_LEAVED:
            retval = "MODE3_LEAVED";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_WR_DATA:
            retval = "WR_DATA";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_CHANGED_IC_FNCLVL:
            retval = "CHANGED_IC_FNCLVL";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_CHANGED_IOMSK:
            retval = "CHANGED_IOMSK";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_INT_PENDING_RESET:
            retval = "INT_PENDING_RESET";
            break;

        case PIOZ80_PORT_EVENT_CPUBUS_INTACK:
            retval = "CPUBUS_INTACK";
            break;

        case PIOZ80_PORT_EVENT_CPUBUS_RETI:
            retval = "CPUBUS_RETI";
            break;

        case PIOZ80_PORT_EVENT_INTERNAL_CHANGED_ICENA:
            retval = "CHANGED_ICENA";
            break;

        default:
            retval = "UNKNOWN";
            break;
    };
    return retval;
}

#endif

/*******************************************************************************
 * 
 * 
 * Popis PIO-Z80
 *  
 * 
 ******************************************************************************/


/**
 * Inicializace PIO-Z80
 * 
 */
void pioz80_init ( void ) {
    g_pioz80_port_a_last_ctrl_byte = 0x00;
    g_pioz80_port_b_last_ctrl_byte = 0x00;
    DBGPRINTF ( DBGINF, "\n" );
    memset ( &g_pioz80, 0x00, sizeof ( g_pioz80 ) );
    g_pioz80.port[0].port_id = PIOZ80_PORT_A;
    g_pioz80.port[1].port_id = PIOZ80_PORT_B;
    g_pioz80.icena_event.ticks = -1;
    g_pioz80.icena_event.event_name = MZEVENT_PIOZ80;
    g_pioz80.icena_event_port_id = PIOZ80_PORT_NONE;
}


/**
 * Resetovani obvodu PIO-Z80
 * 
 */
void pioz80_reset ( void ) {
    DBGPRINTF ( DBGINF, "\n" );
    en_PIOZ80_PORT_ID port_id;
    for ( port_id = PIOZ80_PORT_A; port_id < PIOZ80_PORT_COUNT; port_id++ ) {
        st_PIOZ80_PORT *port = &g_pioz80.port[port_id];
        // na interrupt vector se podle DS pri resetu nesaha
        port->io_mask = 0xff;
        port->icmask = 0xff;
        port->mode = PIOZ80_PORT_MODE1_INPUT;
        port->icena = PIOZ80_ICENA_DISABLED;
        port->masked_input = 0x00;
        port->data_output = 0x00;
        port->last_intfnc_result = PIOZ80_INTFNC_RESULT_FALSE;
        port->ctrl_expect = PIOZ80_CWSTATE_EXPECT_COMMAND;
        port->port_int = PIOZ80_PORT_INT_NONE;
        // TODO: tyto registry nejsou popsany v manualu, tak je potreba overit zda se resetuji a jak
        port->icfnc = PIOZ80_ICFNC_OR;
        port->iclvl = PIOZ80_ICLVL_LOW;
    };

    g_pioz80.interrupt = PIOZ80_INTERRUPT_NONE;
    g_pioz80.interrupt_port_id = PIOZ80_PORT_NONE;

    g_pioz80.icena_event.ticks = -1;
    g_pioz80.icena_event.event_name = MZEVENT_PIOZ80;
    g_pioz80.icena_event_port_id = PIOZ80_PORT_NONE;
}


void pioz80_set_printer_std ( en_PIOZ80_PRINTER_STD std ) {
    if ( g_pioz80.printer_std == std ) return; /* beze změny */
    g_pioz80.printer_std = std;
    DBGPRINTF ( DBGINF, "pioz80: printer std = %s\n",
                ( std == PIOZ80_PRINTER_STD_MZ ) ? "MZ" : "Centronics" );

    /* Změna standardu mění význam/polaritu řídicích signálů na konektoru, takže
     * uzavřeme stávající capture soubor (má-li obsah). Další zachycený bajt
     * založí nový soubor se správným prefixem (printer-mz / printer-centronics).
     * Capture i plotter sdílí jeden konektor. */
    printer_close_file ( );
}


en_PIOZ80_PRINTER_STD pioz80_get_printer_std ( void ) {
    return g_pioz80.printer_std;
}


/**
 * Ziskani vysledku z aktualni intfunc (AND/OR)
 * 
 * @param port
 * @return 
 */
static inline en_PIOZ80_ICFNC_RESULT pioz80_port_get_intfnc_result ( st_PIOZ80_PORT *port ) {
    if ( port->icfnc == PIOZ80_ICFNC_OR ) {
        if ( port->masked_input ) {
            return PIOZ80_INTFNC_RESULT_TRUE;
        };
    } else {
        uint8_t mask = ~port->icmask;
        if ( ( port->masked_input & mask ) == mask ) {
            return PIOZ80_INTFNC_RESULT_TRUE;
        };
    };
    return PIOZ80_INTFNC_RESULT_FALSE;
}


/**
 * Eskalace stavu signalu /INT. Port A ma vzdy prioritu.
 * 
 * @param port_event
 */
static inline void pioz80_interrupt_manager ( en_PIOZ80_PORT_EVENT port_event ) {
    ( void ) port_event;

    // pokud jsme ve stavu INTERRUPT_RECEIVED, tak tu neni co resit - vysvobodi nas jen RESET, nebo RETI
    if ( g_pioz80.interrupt == PIOZ80_INTERRUPT_RECEIVED ) return;

    en_PIOZ80_PORT_ID port_id_pending = PIOZ80_PORT_NONE;
    en_PIOZ80_PORT_ID port_id;

    for ( port_id = PIOZ80_PORT_A; port_id < PIOZ80_PORT_COUNT; port_id++ ) {

        st_PIOZ80_PORT *port = &g_pioz80.port[port_id];

        // ve stavu INT_RECEIVED muze byt vzdy bud jen jeden port, nebo zadny
        if ( port->port_int == PIOZ80_PORT_INT_RECEIVED ) {
            DBGPRINTF ( DBGINF, "INTERRUPT_RECEIVED - screens: %d, ticks: %d\n", g_gdg.total_elapsed.screens, gdg_get_insigeop_ticks ( ) );
            g_pioz80.interrupt = PIOZ80_INTERRUPT_RECEIVED;
            g_pioz80.interrupt_port_id = port_id;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
            /* HWE - HW event BP hooks (irq:pioz80_a / irq:pioz80_b),
             * V1.5 HWE rename z pio:porta_int / pio:portb_int. Signal
             * sémantika: hodnota = 1 (= INT line raised). HWE.3 doplní
             * fire na PIOZ80_PORT_INT_NONE pro 0 hranu. */
            if ( port_id == PIOZ80_PORT_A ) {
                if ( g_bp_event_active[ BP_EVENT_IRQ_PIOZ80_A ] ) {
                    bp_event_fire ( BP_EVENT_IRQ_PIOZ80_A, 1 );
                }
            } else if ( port_id == PIOZ80_PORT_B ) {
                if ( g_bp_event_active[ BP_EVENT_IRQ_PIOZ80_B ] ) {
                    bp_event_fire ( BP_EVENT_IRQ_PIOZ80_B, 1 );
                }
            }
#endif
            // tohle je zarucene novy stav a projevi se okamzite - predchozi stav byl "1"
            mzarch_interrupt_manager ( );
            return;
        } else if ( ( port->port_int == PIOZ80_PORT_INT_PENDING ) && ( port->icena == PIOZ80_ICENA_ENABLED ) ) {
            // cekajici udalosti na portu A maji vzdy prednost
            if ( port_id_pending == PIOZ80_PORT_NONE ) {
                port_id_pending = port_id;
            };
        };
    };

    if ( port_id_pending == PIOZ80_PORT_NONE ) {
        // setrvavame v klidovem stavu
        if ( g_pioz80.interrupt == PIOZ80_INTERRUPT_NONE ) return;

        // prechazime ze stavu INT do klidoveho stavu - vzdy okamzite
        DBGPRINTF ( DBGINF, "INTERRUPT_NONE - screens: %d, ticks: %d\n", g_gdg.total_elapsed.screens, gdg_get_insigeop_ticks ( ) );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* HWE - HW event BP hook (irq:pioz80_a / irq:pioz80_b) - clear edge.
         * Posíláme 0 pro oba ports (= clear ne identifikuje konkrétní port). */
        if ( g_bp_event_active[ BP_EVENT_IRQ_PIOZ80_A ] ) {
            bp_event_fire ( BP_EVENT_IRQ_PIOZ80_A, 0 );
        }
        if ( g_bp_event_active[ BP_EVENT_IRQ_PIOZ80_B ] ) {
            bp_event_fire ( BP_EVENT_IRQ_PIOZ80_B, 0 );
        }
#endif

        g_pioz80.interrupt = PIOZ80_INTERRUPT_NONE;
        mzarch_interrupt_manager ( );

        return;
    };

    // setrvavame v neklidovem stavu
    if ( g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING ) return;

    // prechazime z klidoveho stavu do INT
    DBGPRINTF ( DBGINF, "INTERRUPT_PENDING - screens: %d, ticks: %d\n", g_gdg.total_elapsed.screens, gdg_get_insigeop_ticks ( ) );
    g_pioz80.interrupt = PIOZ80_INTERRUPT_PENDING;
    mzarch_interrupt_manager ( );
}


/**
 * Resetovani cekajiciho interruptu - aktivuje se pouze nastavenim 4. bitu v ICRW
 * 
 * @param port
 */
static inline void pioz80_port_interrupt_reset ( st_PIOZ80_PORT *port ) {
    DBGPRINTF ( DBGINF, "port: %c, RESET PORT INTERRUPT SIGNAL, port_int_signal = %d, icena = %s\n", pioz80_dbg_get_port_name ( port->port_id ), port->port_int, pioz80_dbg_get_icena_status ( port->icena ) );
    // PENDING -> NONE
    // REPENDING -> RECEIVED
    port->port_int &= ~PIOZ80_PORT_INT_PENDING_BIT;
    pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_INTERNAL_INT_PENDING_RESET );
}


/**
 * Byla splnena podminka pro aktivaci interruptu
 * 
 * @param port
 * @param port_event
 */
static inline void pioz80_port_interrupt_activate ( st_PIOZ80_PORT *port, en_PIOZ80_PORT_EVENT port_event ) {

    // port uz nyni ceka na vyrizeni interruptu
    if ( port->port_int & PIOZ80_PORT_INT_PENDING_BIT ) return;

    if ( port->port_int == PIOZ80_PORT_INT_NONE ) {
        port->port_int = PIOZ80_PORT_INT_PENDING;
        DBGPRINTF ( DBGINF, "port: %c, ACTIVATE PORT INTERRUPT SIGNAL, icena = %s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icena_status ( port->icena ) );
    } else {
        port->port_int = PIOZ80_PORT_INT_REPENDING;
        DBGPRINTF ( DBGINF, "port: %c, RE-ACTIVATE PORT INTERRUPT SIGNAL, icena = %s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icena_status ( port->icena ) );
    };

    pioz80_interrupt_manager ( port_event );
}


/**
 * Byla splnena podminka pro deaktivaci interruptu v dobe, kdy jsme po ACK a cekame na RETI (jsme ve stavu REPENDING)
 * 
 * @param port
 */
static inline void pioz80_port_interrupt_deactivate ( st_PIOZ80_PORT *port ) {
    port->port_int &= ~PIOZ80_PORT_INT_PENDING_BIT;
    DBGPRINTF ( DBGINF, "port: %c, DE-ACTIVATE PORT INTERRUPT SIGNAL\n", pioz80_dbg_get_port_name ( port->port_id ) );
}


/**
 * Nacteni skutecneho stavu vstupnich pinu zvolene brany.
 * 
 * Konstantni hodnoty na jednotlivych portech.
 * 
 * PA = 0x03
 *  PA0 - vstupni hodnota z nezapojeneho LPT
 *  PA1 - vstupni hodnota z nezapojeneho LPT
 * 
 * PB = 0xff - zpusobeno pripojenim na vstupy do invertoru
 * 
 * TODO: proverit - PA6 a PA7 mi podle mereni obcas vykazovaly hazadrni stav "1"
 * 
 * 
 * @param port
 * @return 
 */
static inline uint8_t pioz80_port_get_raw_input ( st_PIOZ80_PORT *port ) {
    const uint8_t g_pioz80_port_input[] = { 0x03, 0xff };
    uint8_t retval = g_pioz80_port_input[port->port_id];
    if ( port->port_id == PIOZ80_PORT_A ) {
        retval |= ( CTC8253_OUT ( 0 ) == 1 ) ? 0 : 1 << 4; /* invertovany CTC0 */
        retval |= SIGNAL_GDG_VBLNK ? 1 << 5 : 0x00;
        /* Centronics: připojená virtuální tiskárna řídí signál BUSY (PA0)
         * podle úrovně STROBE (PA7, výstupní bit v data_output). Reálné
         * chování pozorované v ovladači MRS: po assertu STROBE (PA7=1) host
         * čeká, až tiskárna potvrdí příjem zvednutím BUSY=1; po deassertu
         * (PA7=0) tiskárna BUSY uvolní. Modelujeme tedy nekonečně rychlou
         * tiskárnu: BUSY (PA0) zrcadlí STROBE (PA7).
         * Bez tohoto by LPT vstup floatoval na 1 (busy) a polling smyčka
         * čekající na potvrzení by se zacyklila. */
        if ( g_printer.active ) {
            if ( ( port->data_output >> 7 ) & 0x01 ) {
                retval |= 0x01;   /* STROBE asserted -> BUSY = 1 (bajt přijat) */
            } else {
                retval &= ~( ( uint8_t ) 0x01 ); /* STROBE klid -> BUSY = 0 */
            };
        };
        /* MZ-1P16 plotter: pokud je připojený, je to REÁLNÉ zařízení, které
         * generuje vlastní stavové signály - na rozdíl od printer capture
         * (nekonečně rychlá tiskárna se zrcadlovým BUSY). Plotter má proto
         * PRIORITU: jeho skutečné výstupy přebijí printer mirror. Když běží
         * oba, host vidí stav plotteru (= reálná latence tisku); printer jen
         * pasivně odposlouchává bajty na hraně STROBE, neřídí handshake.
         *
         * Zapojení stavových signálů (PŘÍMO z výstupního latche jádra
         * g_mz1p16_cpu.P1):
         *   PA0 (BUSY/RDA) = plotter P1.4: P1.4=0 -> PA0=0, P1.4=1 -> PA0=1.
         *   PA1 (STA)      = plotter P1.7: P1.7=0 -> PA1=0, P1.7=1 -> PA1=1.
         *
         * Před přečtením stavu se plotter MCU deterministicky odkrokuje
         * (drive_poll). Host čte BUSY v poll smyčce a každé čtení firmware
         * posune, takže BUSY včas přejde busy->ready a poll smyčka neuvázne. */
        if ( g_mz1p16.active ) {
            /* Serializace přístupu k plotter jádru (race s UI/emu vláknem):
             * zámek kolem krokování + čtení stavu. drive_poll sám zámek nebere
             * (předpokládá držení volajícím). */
            mz1p16_emu_lock ( );
            mz1p16_emu_drive_poll ( );
            uint8_t p1 = g_mz1p16_cpu.P1;
            mz1p16_emu_unlock ( );

            if ( p1 & 0x10u ) { retval |= 0x01; } else { retval &= ~( ( uint8_t ) 0x01 ); };
            if ( p1 & 0x80u ) { retval |= 0x02; } else { retval &= ~( ( uint8_t ) 0x02 ); };
        };
    };
    return retval;
}


/**
 * Nacteni hodnoty data portu dle io_mask.
 * 
 * @param port
 * @return 
 */
static inline uint8_t pioz80_port_get_iomask_input ( st_PIOZ80_PORT *port ) {
    uint8_t retval = 0x00;
    uint8_t port_pins = pioz80_port_get_raw_input ( port );
    retval = ( port_pins & port->io_mask ) | ( port->data_output & ( ~port->io_mask ) );
    //DBGPRINTF ( DBGINF, "port: %c, port_pins = 0x%02x (\"%s\"), input_register = 0x%02x (\"%s\")\n", pioz80_dbg_get_port_name ( port->port_id ), port_pins, pioz80_dbg_get_byte_string ( port_pins ), retval, pioz80_dbg_get_byte_string ( retval ) );
    return retval;
}


/**
 * Precteme port io_mask input a bity v urovni a masce, ktere nas zajimaji nastavime '1'.
 * 
 * @param port
 * @return 
 */
static inline uint8_t pioz80_port_get_intmask_result ( st_PIOZ80_PORT *port ) {
    uint8_t data_input = pioz80_port_get_iomask_input ( port );
    uint8_t result = ( port->iclvl == PIOZ80_ICLVL_LOW ) ? ~data_input : data_input;
    result &= ( ~port->icmask );
    return result;
}


/**
 * Cteni z data registru.
 * 
 * @param port
 * @return 
 */
static inline uint8_t pioz80_port_rd_data ( st_PIOZ80_PORT *port ) {
    uint8_t retval = pioz80_port_get_iomask_input ( port );
    DBGPRINTF ( DBGINF, "port: %c, retval = 0x%02x, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), retval, g_mzarch_main.instruction_addr );
    return retval;
}


/**
 * IORQ - cteni z PIO-Z80
 * 
 * @param addr
 * @return 
 */
uint8_t pioz80_read_byte ( en_PIOZ80_ADDR addr ) {

    en_PIOZ80_PORT_ID port_id = ( addr & PIOZ80_PORT_ID_MASK );
    en_PIOZ80_ADDRTYPE addr_type = ( addr & PIOZ80_ADDR_TYPE_MASK );

    if ( PIOZ80_ADDRTYPE_DATA == addr_type ) {
        st_PIOZ80_PORT *port = &g_pioz80.port[port_id];
        uint8_t retval = pioz80_port_rd_data ( port );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* Hwlog DATA_READ - shadow latch hodnoty vrácené CPU. Read sám
         * o sobě v PIO Z80 negeneruje stavovou změnu (delta = 0). */
        pioz80_hwlog_emit ( HWLOG_PIOZ80_DATA_READ, (uint8_t) port_id,
                            (uint8_t) addr, retval, 0 );
#endif
        return retval;
    };

    // cteni z ctrl adresy vraci vzdy 0xff
    uint8_t retval = 0xff;
    DBGPRINTF ( DBGINF, "read from CTRL reg - port: %c, retval = 0x%02x, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port_id ), retval, g_mzarch_main.instruction_addr );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Hwlog DATA_READ pro nestandardní čtení control adresy (vrací 0xFF)
     * - logujeme s addr = 0/1 (ctrl A/B) aby parser viděl SW pokus o čtení
     * write-only registru. */
    pioz80_hwlog_emit ( HWLOG_PIOZ80_DATA_READ, (uint8_t) port_id,
                        (uint8_t) addr, retval, 0 );
#endif
    return retval;
}


/**
 * Zpracovani udalosti pri zmene urovni vstupnich pinu, vystupnich bitu a intcrtl
 * 
 * @param port
 * @param port_event
 * @param pinvalue
 */
static inline void pioz80_port_event ( st_PIOZ80_PORT *port, en_PIOZ80_PORT_EVENT port_event, int pinvalue ) {

#if DBGLEVEL
    char dbg_pinvalue[16];
    if ( port_event <= PIOZ80_PORT_EVENT_PA5_VBLN ) {
        snprintf ( dbg_pinvalue, sizeof ( dbg_pinvalue ), ", pinvalue = %d,", pinvalue );
    } else {
        dbg_pinvalue[0] = 0x00;
    };
#else
    ( void ) pinvalue;
#endif

    // dokud cekame na INTMCW, tak nelze vytvorit interrupt
    if ( port->ctrl_expect == PIOZ80_CWSTATE_EXPECT_INTMCW ) {
        DBGPRINTF ( DBGINF, "port: %c, ignored = EXPECT_INTMCW, event = %s%s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_port_event_name ( port_event ), dbg_pinvalue );
        return;
    };

    // pokud je INT_PENDING
    if ( port->port_int == PIOZ80_PORT_INT_PENDING ) {
        DBGPRINTF ( DBGINF, "port: %c, ignored = PENDING, event = %s%s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_port_event_name ( port_event ), dbg_pinvalue );
        return;
    };

    uint8_t old_masked_input = port->masked_input;
    port->masked_input = pioz80_port_get_intmask_result ( port );

    // event nezpusobil zadnou zmenu
    if ( old_masked_input == port->masked_input ) {
        DBGPRINTF ( DBGINF, "port: %c, ignored = SAME_INPUT, data_input = 0x%02x, event = %s%s\n", pioz80_dbg_get_port_name ( port->port_id ), port->masked_input, pioz80_dbg_get_port_event_name ( port_event ), dbg_pinvalue );
        return;
    };

    en_PIOZ80_ICFNC_RESULT last_result = port->last_intfnc_result;
    port->last_intfnc_result = pioz80_port_get_intfnc_result ( port );

    // vysledek intfunc je stale stejny
    if ( last_result == port->last_intfnc_result ) {
        DBGPRINTF ( DBGINF, "port: %c, ignored = SAME_RESULT, result = %s, data_input = 0x%02x, event = %s%s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icfnc_result_string ( port->last_intfnc_result ), port->masked_input, pioz80_dbg_get_port_event_name ( port_event ), dbg_pinvalue );
        return;
    };

    DBGPRINTF ( DBGINF, "port: %c, result = %s, data_input = 0x%02x, event = %s%s\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icfnc_result_string ( port->last_intfnc_result ), port->masked_input, pioz80_dbg_get_port_event_name ( port_event ), dbg_pinvalue );

    if ( port->last_intfnc_result == PIOZ80_INTFNC_RESULT_TRUE ) {
        pioz80_port_interrupt_activate ( port, port_event );
    } else if ( port->port_int == PIOZ80_PORT_INT_REPENDING ) {
        // pokud jsme v RE-PENDING (obsluhujeme potvrzeny interrupt), tak je mozne z PENDING prejit do NONE
        pioz80_port_interrupt_deactivate ( port );
    };
}


/**
 * Zpracovani lokalne vyvolane udalosti nad portem
 * 
 * @param port
 * @param port_event
 */
static inline void pioz80_port_event_internal ( st_PIOZ80_PORT *port, en_PIOZ80_PORT_EVENT port_event ) {
    pioz80_port_event ( port, port_event, 0 );
}


/**
 * Zapis do datoveho registru - data urcena pro vystup.
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_data ( st_PIOZ80_PORT *port, uint8_t value ) {
    port->data_output = value;
    DBGPRINTF ( DBGINF, "port: %c, value = 0x%02x, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), port->data_output, g_mzarch_main.instruction_addr );

    /* Centronics: PA7 je STROBE (RDP). Při zápisu na bránu PA sledujeme hranu
     * PA7 a na aktivaci STROBE zachytíme aktuální bajt na datové bráně PB. */
    if ( port->port_id == PIOZ80_PORT_A ) {
        uint8_t pa7 = ( value >> 7 ) & 0x01;

        /* STROBE/RDP polarita podle zadního DIP SW2/SW3 (standard tiskárny).
         * MZ-800 ROM drží STROBE (PA7) v klidu na 0 a pulzuje na 1, ale firmware
         * plotteru čeká v klidu /INT = 1 (INT=0 bere jako aktivní strobe -> jde
         * busy). Pro MZ tiskárnu je tedy STROBE INVERTOVANÝ: INT = !PA7
         * (idle PA7=0 -> INT=1 ready; strobe PA7=1 -> INT=0 aktivní). Switch
         * invertuje VÝSTUPNÍ řídicí signál (PA7), vstup BUSY/PA0 zůstává přímý
         * (viz čtení PA výše). Centronics = bez inverze. */
        uint8_t strobe = PIOZ80_TEST_PRINTER_MZ ? ( pa7 ^ 0x01 ) : pa7;
        uint8_t pb  = g_pioz80.port[ PIOZ80_PORT_B ].data_output;
        printer_pa_strobe_update ( strobe, pb );

        /* MZ-1P16 plotter (koexistence s printer): host data na PB ->
         * datová sběrnice plotteru P2 (firmware je čte IN A,P2). Zápis PA ->
         * /INT plotteru (STROBE).
         *
         * Polarita /INT: /INT = efektivní STROBE po aplikaci zadního DIP
         * SW2/SW3 (viz výše). Pro MZ standard (plotter MZ-1P16) je STROBE
         * invertovaný oproti PA7. Firmware /INT čte pollingem (JNI), ne
         * přerušením.
         *
         * Krokování (DETERMINISTICKÉ, event-driven - viz mz1p16_emu_drive_poll):
         * tady jen nastavíme data + STROBE a na aktivační hraně STROBE synchronně
         * odkrokujeme firmware, dokud bajt nepřijme (zvedne BUSY). NEvážeme na
         * čas - tempo dalšího kreslení řídí host poll smyčka (čtení BUSY ->
         * drive_poll). Když plotter neaktivní, je to no-op. */
        if ( g_mz1p16.active ) {
            /* Serializace přístupu k plotter jádru (race s UI/emu vláknem). */
            mz1p16_emu_lock ( );

            /* Detekce sestupné hrany /INT (aktivace STROBE): klid /INT=1 ->
             * aktivní /INT=0. Na této hraně host právě "položil" nový bajt. */
            uint8_t prev_int = g_mz1p16_cpu.INT;

            g_mz1p16.host_data  = pb;
            g_mz1p16_cpu.P2_in  = pb;
            g_mz1p16_cpu.INT    = strobe;  /* /INT = STROBE (po polaritě DIP SW2/SW3) */

            /* Robustní deterministický handshake: na aktivaci STROBE odkrokuj
             * firmware, dokud bajt SKUTEČNĚ NEPŘIJME (zvedne BUSY). Tím se bajt
             * přijme právě jednou, nezávisle na časování - mizí "zuby". */
            if ( prev_int != 0 && strobe == 0 ) {
                mz1p16_emu_sync_read_byte ( );
            };

            mz1p16_emu_unlock ( );
        };
    };

    // pripadny INT je spusten ihned
    // TODO: tento fenomen se zrejme neobjevuje uplne vzdy
    pioz80_port_event_internal ( port, PIOZ80_PORT_EVENT_INTERNAL_WR_DATA );
}


/**
 * Ulozeni (inicializace) posledniho stavu na portu
 * 
 * @param port
 */
static inline void pioz80_port_save_input_state ( st_PIOZ80_PORT *port ) {
    port->masked_input = pioz80_port_get_intmask_result ( port );
    port->last_intfnc_result = pioz80_port_get_intfnc_result ( port );
    DBGPRINTF ( DBGINF, "port: %c, data_input = 0x%02x, intfnc_result = %s\n", pioz80_dbg_get_port_name ( port->port_id ), port->masked_input, pioz80_dbg_get_icfnc_result_string ( port->last_intfnc_result ) );
}


/**
 * Nastaveni interrupt vektoru
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_ivector ( st_PIOZ80_PORT *port, uint8_t value ) {
    port->interrupt_vector = value;
    DBGPRINTF ( DBGINF, "port: %c, ivector = 0x%02x, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), port->interrupt_vector, g_mzarch_main.instruction_addr );
}


/**
 * Zpracovani Mode Control Word
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_mcw ( st_PIOZ80_PORT *port, uint8_t value ) {

    switch ( value ) {

        case PIOZ80_PORT_MODE0_OUTPUT:
            port->io_mask = 0x00;
            DBGPRINTF ( DBGINF, "port: %c, MODE0-OUTPUT, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_mzarch_main.instruction_addr );
            break;

        case PIOZ80_PORT_MODE1_INPUT:
            port->io_mask = 0xff;
            DBGPRINTF ( DBGINF, "port: %c, MODE1-INPUT, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_mzarch_main.instruction_addr );
            break;

        case PIOZ80_PORT_MODE2_BIDIR:
            // TODO: tento rezim je platny jen pro branu A - nemam uplne ve vsem overeno jak se chova pri nastaveni pro B
            // vicemene se to vsak v aplikaci MZ-800 zrejme chova stejne jako v MODE1
            port->io_mask = 0xff;
            DBGPRINTF ( DBGINF, "port: %c, MODE2-BIDIR, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_mzarch_main.instruction_addr );
            break;

        case PIOZ80_PORT_MODE3_USER:
            port->ctrl_expect = PIOZ80_CWSTATE_EXPECT_IOMCW;
            DBGPRINTF ( DBGINF, "port: %c, MODE3-USER in latch - waiting for IOMCW..., PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_mzarch_main.instruction_addr );
            return;
            break;
    };

    // TODO: tento fenomen se zrejme neobjevuje uplne vzdy
    if ( ( port->mode == PIOZ80_PORT_MODE3_USER ) && ( port->icfnc == PIOZ80_ICFNC_OR ) ) {
        // vyvolani interruptu probehne v podstate ihned uvnitr IORQ operace
        pioz80_port_interrupt_activate ( port, PIOZ80_PORT_EVENT_INTERNAL_MODE3_LEAVED );
    };

    port->mode = value;
}


/**
 * Zpracovani zmeny IC_ENA.
 * Pokud je DISABLED, tak zmena probehne ihned.
 * Pokud je ENABLED, tak zmena probehne za 3 CPU takty po nasledujicim M1
 * 
 * @param port
 * @param value
 * @return 
 *      0 - normalni casovani
 *      1 - byl vytvoren posunuty event
 */
static inline int pioz80_port_set_icena ( st_PIOZ80_PORT *port, en_PIOZ80_ICENA value ) {

    en_PIOZ80_ICENA old_icena = port->icena;
    if ( old_icena == value ) return 0;

    if ( value == PIOZ80_ICENA_DISABLED ) {
        port->icena = value;
        pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_INTERNAL_CHANGED_ICENA );
        return 0;
    };

    unsigned tstates;
    uint8_t byte = memory_read_byte ( g_mzarch_main.instruction_addr );

    if ( byte == 0xd3 ) {

        // vykonavame OUT (#), a - 11 tstates
        tstates = 11;

    } else {

        uint8_t byte = memory_read_byte ( g_mzarch_main.instruction_addr + 1 );

        switch ( byte >> 8 ) {

            case 0x0a:
                // vykonavame OUTI, OUTD - 16 tstates
                tstates = 16;
                break;

            case 0x0b:
                // vykonavame OTIR, OTDR - 16/21 tstates
                if ( ( g_mzarch_main.cpu->bc.w >> 8 ) == 0x00 ) {
                    tstates = 16;
                } else {
                    tstates = 21;
                };
                break;

            default:
                // vykonavame OUT (c), r, OUT (C),0 - 12 tstates
                tstates = 12;
                break;
        };
    };

    tstates += 3;
    unsigned event_ticks = mz800_main_get_instruction_start_ticks ( ) + ( tstates * GDGCLK2CPU_DIVIDER );

    MZ800_MAIN_SET_EVENT ( MZEVENT_PIOZ80, event_ticks );
    g_pioz80.icena_event.ticks = event_ticks;
    g_pioz80.icena_event_port_id = port->port_id;

    return 1;
}


/**
 * Zpracovani Interrupt Control Word
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_icw ( st_PIOZ80_PORT *port, uint8_t value ) {

    port->iclvl = ( value & PIOZ80_ICLVL_BIT ) ? PIOZ80_ICLVL_HIGH : PIOZ80_ICLVL_LOW;
    port->icfnc = ( value & PIOZ80_ICFNC_BIT ) ? PIOZ80_ICFNC_AND : PIOZ80_ICFNC_OR;
    en_PIOZ80_ICENA icena = ( value & PIOZ80_ICENA_BIT ) ? PIOZ80_ICENA_ENABLED : PIOZ80_ICENA_DISABLED;

    if ( value & PIOZ80_ICMSK_BIT ) {
        port->icena = icena;
        DBGPRINTF ( DBGINF, "port: %c, icfnc = %s, iclvl = %s, icena: %s, waiting for int_mask..., PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icfnc_name ( port->icfnc ), pioz80_dbg_get_iclvl_name ( port->iclvl ), pioz80_dbg_get_icena_status ( port->icena ), g_mzarch_main.instruction_addr );
        port->ctrl_expect = PIOZ80_CWSTATE_EXPECT_INTMCW;
        pioz80_port_interrupt_reset ( port );
    } else {
        DBGPRINTF ( DBGINF, "port: %c, icfnc = %s, iclvl = %s, icena: %s, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icfnc_name ( port->icfnc ), pioz80_dbg_get_iclvl_name ( port->iclvl ), pioz80_dbg_get_icena_status ( icena ), g_mzarch_main.instruction_addr );
        int icena_event = pioz80_port_set_icena ( port, icena );
        if ( port->mode == PIOZ80_PORT_MODE3_USER ) {
            pioz80_port_event_internal ( port, PIOZ80_PORT_EVENT_INTERNAL_CHANGED_IC_FNCLVL );
            if ( !icena_event ) {
                pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_INTERNAL_CHANGED_ICENA );
            };
        };
    };
}


/**
 * Zpracovani Interrupt Disable Word
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_idw ( st_PIOZ80_PORT *port, en_PIOZ80_ICENA value ) {
    DBGPRINTF ( DBGINF, "port: %c, icena: %s, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), pioz80_dbg_get_icena_status ( value ), g_mzarch_main.instruction_addr );
    int icena_event = pioz80_port_set_icena ( port, value );
    if ( !icena_event ) {
        pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_INTERNAL_CHANGED_ICENA );
    };
}


/**
 * Zpracovani Interrupt Mask Control Word
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_intmcw ( st_PIOZ80_PORT *port, uint8_t value ) {
    port->icmask = value;
    DBGPRINTF ( DBGINF, "port: %c, fullmask = 0x%02x (\"%s\"), PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), port->icmask, pioz80_dbg_get_fullmask_string ( port->io_mask, port->icmask ), g_mzarch_main.instruction_addr );
    port->ctrl_expect = PIOZ80_CWSTATE_EXPECT_COMMAND;
    // predchozi ctrl bajt resetoval interrupt, nini si ulozime startovaci stav
    pioz80_port_save_input_state ( port );
}


/**
 * Zpracovani I/O Mask Control Word
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl_iomcw ( st_PIOZ80_PORT *port, uint8_t value ) {
    en_PIOZ80_PORT_MODE old_mcr = port->mode;
    port->mode = PIOZ80_PORT_MODE3_USER;
    port->io_mask = value;
    DBGPRINTF ( DBGINF, "port: %c, MODE3-USER, io_mask = 0x%02x (\"%s\"), PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), port->io_mask, pioz80_dbg_get_fullmask_string ( port->io_mask, port->icmask ), g_mzarch_main.instruction_addr );
    port->ctrl_expect = PIOZ80_CWSTATE_EXPECT_COMMAND;
    if ( old_mcr == PIOZ80_PORT_MODE3_USER ) {
        //  po zmene I/O masky muze byt splnena podminka pro okamzity INT, ktery probehne ihned IORQ
        pioz80_port_event_internal ( port, PIOZ80_PORT_EVENT_INTERNAL_CHANGED_IOMSK );
    } else {
        pioz80_port_save_input_state ( port );
    };
}


/**
 * Zapis do control registru
 * 
 * @param port
 * @param value
 */
static inline void pioz80_port_wr_ctrl ( st_PIOZ80_PORT *port, uint8_t value ) {

    switch ( port->ctrl_expect ) {

        case PIOZ80_CWSTATE_EXPECT_COMMAND:

            if ( PIOZ80_CTRL_IVW == ( value & PIOZ80_CTRL_IVW_MASK ) ) {
                pioz80_port_wr_ctrl_ivector ( port, value & 0xfe );
                break;
            };

            uint8_t cmd = value & PIOZ80_CTRL_MASK;

            switch ( cmd ) {

                case PIOZ80_CTRL_MCW:
                    pioz80_port_wr_ctrl_mcw ( port, ( value >> 6 ) & 0x03 );
                    break;

                case PIOZ80_CTRL_ICW:
                    pioz80_port_wr_ctrl_icw ( port, ( value >> 4 ) & 0x0f );
                    break;

                case PIOZ80_CTRL_IDW:
                    pioz80_port_wr_ctrl_idw ( port, ( value >> 7 ) & 0x01 );
                    break;

                default:
                    DBGPRINTF ( DBGINF, "port: %c, UNKNOWN CONTROL WORD!, value = 0x%02x, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), value, g_mzarch_main.instruction_addr );
                    break;
            };
            break;

        case PIOZ80_CWSTATE_EXPECT_INTMCW:
            pioz80_port_wr_ctrl_intmcw ( port, value );
            break;

        case PIOZ80_CWSTATE_EXPECT_IOMCW:
            pioz80_port_wr_ctrl_iomcw ( port, value );
            break;
    };
}


/**
 * IORQ - zapis do PIO-Z80
 *
 * @param addr
 * @param value
 */
void pioz80_write_byte ( en_PIOZ80_ADDR addr, uint8_t value ) {

    // printf("%s():%d - addr: %d, value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mzarch_main.instruction_addr);

    en_PIOZ80_PORT_ID port_id = ( addr & PIOZ80_PORT_ID_MASK );
    en_PIOZ80_ADDRTYPE addr_type = ( addr & PIOZ80_ADDR_TYPE_MASK );

    st_PIOZ80_PORT *port = &g_pioz80.port[port_id];

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Hwlog snapshot před změnou - odložené emit po dokončení dispatchu.
     * Sub-event typ určujeme z (addr_type, ctrl_expect_pre, value). */
    st_PIOZ80_HWLOG_SNAPSHOT snap;
    en_PIOZ80_EXPECT pre_ctrl_expect = port->ctrl_expect;
    int hwlog_active = TEST_TRACE_HWLOG_DISPATCH;
    if ( hwlog_active ) {
        pioz80_hwlog_take_snapshot ( &snap, port );
    }
#endif

    if ( PIOZ80_ADDRTYPE_DATA == addr_type ) {
        pioz80_port_wr_data ( port, value );
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if ( hwlog_active ) {
            uint32_t delta = pioz80_hwlog_compute_delta ( &snap, port );
            pioz80_hwlog_emit ( HWLOG_PIOZ80_DATA_WRITE, (uint8_t) port_id,
                                (uint8_t) addr, value, delta );
        }
#endif
    } else {
        pioz80_port_wr_ctrl ( port, value );
        /* Debug/UI mirror: zachyt jakykoliv byte zapsany na control port
         * (= MCW, ICW, INT vector, IOW mask post-MCW3, IM mask post-ICW).
         * HW samotne neuklada, sequencer state je v port->ctrl_expect a
         * port->mode. Cache reflektuje posledni byte tak, jak ho CPU
         * zapsal, bez interpretace. */
        if ( port_id == PIOZ80_PORT_A ) {
            g_pioz80_port_a_last_ctrl_byte = value;
        } else {
            g_pioz80_port_b_last_ctrl_byte = value;
        };
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        if ( hwlog_active ) {
            /* Identifikace sub-eventu podle pre-state ctrl_expect a value.
             * - Pokud čekáme MASK po ICW s MF=1, je to MASK_WRITE.
             * - Pokud čekáme IO_SELECT po Mode 3, je to IO_SELECT_WRITE.
             * - V ANY stavu rozhoduje value (D0=0 -> VECTOR, jinak D3-D0). */
            en_HWLOG_PIOZ80_SUB sub;
            if ( pre_ctrl_expect == PIOZ80_CWSTATE_EXPECT_INTMCW ) {
                sub = HWLOG_PIOZ80_MASK_WRITE;
            } else if ( pre_ctrl_expect == PIOZ80_CWSTATE_EXPECT_IOMCW ) {
                sub = HWLOG_PIOZ80_IO_SELECT_WRITE;
            } else if ( ( value & PIOZ80_CTRL_IVW_MASK ) == PIOZ80_CTRL_IVW ) {
                sub = HWLOG_PIOZ80_VECTOR_WRITE;
            } else {
                uint8_t cmd = value & PIOZ80_CTRL_MASK;
                if ( cmd == PIOZ80_CTRL_MCW ) {
                    sub = HWLOG_PIOZ80_MODE_WRITE;
                } else if ( cmd == PIOZ80_CTRL_ICW ) {
                    sub = HWLOG_PIOZ80_INT_CTRL_WRITE;
                } else if ( cmd == PIOZ80_CTRL_IDW ) {
                    sub = HWLOG_PIOZ80_INT_CTRL_WRITE; /* IDW = krátká forma ICW */
                } else {
                    /* nevalidní control word - pořád ho logujeme jako ICW
                     * pro maximální informativnost (ladění chybného sw) */
                    sub = HWLOG_PIOZ80_INT_CTRL_WRITE;
                }
            }
            uint32_t delta = pioz80_hwlog_compute_delta ( &snap, port );
            pioz80_hwlog_emit ( sub, (uint8_t) port_id, (uint8_t) addr,
                                value, delta );
        }
#endif
    };
}

#if DBGLEVEL
static unsigned dbg_interrupt_ack_ticks = 0;
#endif


/**
 * @brief Callback jádra Z80 pro potvrzení přerušení (INTACK) v IM 2.
 *
 * Z80 PIO položí na sběrnici vektor jen tehdy, když má samo čekající
 * přerušení (port A má přednost před B); pak port označí jako přijatý.
 * Jinak (přerušení z CTC2, /EXINT, FDC) zůstane od sběrnice odpojené a CPU
 * přečte ducha sběrnice (g_mzarch_main.regDBUS_latch). Na MZ-800 je to
 * poslední bajt, který šel po datové sběrnici: čtení i zápis paměti, OUT,
 * IN; při přijetí z HALT bajt za instrukcí HALT (změřeno 2026-11-26,
 * emu-experiments im2-vector/hwtest/VYSLEDKY1.md). Na MZ-1500 drží latch
 * jen poslední bajt přečtený z paměti (neměřeno). Jádro pak vezme záznam
 * tabulky (I << 8) | vektor bez maskování bitu 0.
 *
 * @param cpu       Instance CPU (využita jen v ladicím výpisu).
 * @param user_data Nepoužito.
 * @return Vektor z PIO, nebo duch sběrnice, když PIO přerušení nežádá.
 * @pre Volá se z emulátorového vlákna při přijetí INT v IM 2.
 * @post Při vlastním přerušení PIO je port ve stavu RECEIVED.
 */
uint8_t pioz80_interrupt_ack_im2_cb ( z80_t *cpu, void *user_data ) {
#if !DBGLEVEL && !defined(MZ800EMU_CFG_DEBUGGER_ENABLED)
    ( void ) cpu;
#endif
    ( void ) user_data;
    if ( g_pioz80.interrupt != PIOZ80_INTERRUPT_PENDING ) {
        /* INT ACK, ale PIOZ80 NENI autorem interruptu, takže zůstane od datové sběrnice odpojen a načte se "duch sbernice" (g_mzarch_main.regDBUS_latch). */
        return g_mzarch_main.regDBUS_latch;
    };

    st_PIOZ80_PORT *port;
    en_PIOZ80_PORT_ID port_id;

    // cekajici udalosti na portu A maji vzdy prednost
    for ( port_id = PIOZ80_PORT_A; port_id < PIOZ80_PORT_COUNT; port_id++ ) {

        port = &g_pioz80.port[port_id];

        if ( ( port->port_int == PIOZ80_PORT_INT_PENDING ) && ( port->icena == PIOZ80_ICENA_ENABLED ) ) {
            break;
        };
    };

#if DBGLEVEL
    dbg_interrupt_ack_ticks = gdg_get_total_ticks ( );
    uint16_t dbg_vector_addr = ( cpu->i << 8 ) | port->interrupt_vector;
    uint16_t dbg_newPC = ( memory_read_byte ( dbg_vector_addr + 1 ) << 8 ) | memory_read_byte ( dbg_vector_addr );
    DBGPRINTF ( DBGINF, "port: %c, INTERRUPT_RECEIVED - return ivector = 0x%04x (newPC = 0x%04x), PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), dbg_vector_addr, dbg_newPC, g_mzarch_main.instruction_addr );
#endif

    /* Pozn.: dříve zde byl trace-suite intlog hook (IRQ_ACK_IM2 + PIO_STATE
     * IM2_JUMP). Přesunut do mzarch.c:mzarch_main_process_interrupt() kvůli
     * early return na začátku funkce - pokud IRQ nepatří PIOZ80 (např. CTC2
     * v mzdos), tato funkce vrací ducha sběrnice (regDBUS_latch) ještě před
     * hookem a žádný IRQ_ACK_IM2 event se
     * nevyemituje, ačkoliv CPU dispatch v IM 2 reálně proběhl. Centrální hook
     * v mzarch.c pokrývá všechny IRQ zdroje. */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Hwlog snapshot před modifikací port_int (a před save_input_state).
     * Jsme uvnitř if(g_pioz80.interrupt == PENDING) - víme, že IRQ skutečně
     * patří PIOZ80, takže emit dává smysl (na rozdíl od intlog hooku, který
     * by emit i pro non-PIOZ80 IRQ a tam je proto v mzarch.c). */
    st_PIOZ80_HWLOG_SNAPSHOT hwlog_snap;
    int hwlog_active = TEST_TRACE_HWLOG_DISPATCH;
    if ( hwlog_active ) {
        pioz80_hwlog_take_snapshot ( &hwlog_snap, port );
    }
#endif

    port->port_int = PIOZ80_PORT_INT_RECEIVED;

    // casovani neemulujeme - deaktivace INT nastane po 3 CPU taktech
    pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_CPUBUS_INTACK );

    pioz80_port_save_input_state ( port );

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if ( hwlog_active ) {
        uint32_t delta = pioz80_hwlog_compute_delta ( &hwlog_snap, port );
        /* sub_addr = vrácený interrupt_vector LSB pro rychlou identifikaci */
        pioz80_hwlog_emit ( HWLOG_PIOZ80_IRQ_ACK_M2, (uint8_t) port->port_id,
                            port->interrupt_vector, port->interrupt_vector,
                            delta );
    }
#endif

    return port->interrupt_vector;
}


/* TODO: zjistit co se stane, kdyz PIO po IM 2 CB nedostane RETI, ale opet dostane INT ACK a jak v takovem pripade vypada IM2 INT ACK */


/**
 * Potvrzeni o tom, ze CPU prijal interrupt - predevsim v pripadech, kdy neni v rezimu IM 2
 * 
 */
void pioz80_interrupt_ack ( void ) {

    // bud na zadny interrupt necekame, nebo uz byl potvrzen pres IM 2 callback
    if ( g_pioz80.interrupt != PIOZ80_INTERRUPT_PENDING ) return;

    // casovani neemulujeme - deaktivace INT nastane po 3 CPU taktech

    st_PIOZ80_PORT *port;
    en_PIOZ80_PORT_ID port_id;

    // cekajici udalosti na portu A maji vzdy prednost
    for ( port_id = PIOZ80_PORT_A; port_id < PIOZ80_PORT_COUNT; port_id++ ) {

        port = &g_pioz80.port[port_id];

        if ( ( port->port_int == PIOZ80_PORT_INT_PENDING ) && ( port->icena == PIOZ80_ICENA_ENABLED ) ) {
            break;
        };
    };

#if DBGLEVEL
    dbg_interrupt_ack_ticks = gdg_get_total_ticks ( );
#endif

    DBGPRINTF ( DBGINF, "port: %c, INTERRUPT_RECEIVED, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_mzarch_main.instruction_addr );

    port->port_int = PIOZ80_PORT_INT_RECEIVED;

    pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_CPUBUS_INTACK );

    pioz80_port_save_input_state ( port );
}


/**
 * Potvrzeni o navratu z interruptu
 * 
 * @param cpu
 * @param user_data
 */
void pioz80_interrupt_reti_cb ( z80_t *cpu, void *user_data ) {
#if !DBGLEVEL
    ( void ) cpu;
    ( void ) user_data;
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite intlog: RETI vykonáno (CPU právě dokončil obsluhu IRQ).
     * Emit i v případě, že PIOZ80 není v INTERRUPT_RECEIVED (RETI mohlo
     * patřit jinému zdroji - CTC, FDC). State bits = aktuální IM/IFF. */
    if ( TEST_TRACE_INTLOG_DISPATCH ) {
        uint32_t bits = INTLOG_STATE_BIT_RETI
            | ( cpu->iff1 ? INTLOG_STATE_BIT_IFF1 : 0 )
            | ( cpu->iff2 ? INTLOG_STATE_BIT_IFF2 : 0 )
            | ( cpu->im == 0 ? INTLOG_STATE_BIT_IM0 :
                cpu->im == 1 ? INTLOG_STATE_BIT_IM1 : INTLOG_STATE_BIT_IM2 );
        intlog_record_cpu_int_state ( bits );
    }
#endif

    // neemulujeme - skutecne PIO v tomto pripade prejde na 4 CPU takty do INTERRUPT_NEXTPRIO a pak zpet do INTERRUPT_PENDING
    //if ( g_pioz80.interrupt == PIOZ80_INTERRUPT_PENDING ) {};

    if ( g_pioz80.interrupt != PIOZ80_INTERRUPT_RECEIVED ) return;

    // casovani neemulujeme - tohle by melo nastat az 4 CPU takty po zacatku RETI

    st_PIOZ80_PORT *port = &g_pioz80.port[g_pioz80.interrupt_port_id];

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Hwlog snapshot před návratem PIO do NONE (viz delta_INT_GLOBAL bit). */
    st_PIOZ80_HWLOG_SNAPSHOT hwlog_snap_reti;
    en_PIOZ80_PORT_ID hwlog_reti_port_id = port->port_id;
    int hwlog_active_reti = TEST_TRACE_HWLOG_DISPATCH;
    if ( hwlog_active_reti ) {
        pioz80_hwlog_take_snapshot ( &hwlog_snap_reti, port );
    }
#endif

#if DBGLEVEL
    unsigned dbg_interrupt_ticks = gdg_get_total_ticks ( ) - dbg_interrupt_ack_ticks;
    DBGPRINTF ( DBGINF, "port: %c, INTERRUPT_RETI, screens: %d, ticks: %d, interrupt_ticks = %d, PC = 0x%04x\n", pioz80_dbg_get_port_name ( port->port_id ), g_gdg.total_elapsed.screens, gdg_get_insigeop_ticks ( ), dbg_interrupt_ticks, g_mzarch_main.instruction_addr );
#endif


    port->port_int &= ~PIOZ80_PORT_INT_RECEIVED_BIT;

    // jedine misto (mimo reset a pioz80_interrupt_manager(), kde se saha na top signaly )
    g_pioz80.interrupt = PIOZ80_INTERRUPT_NONE;
    g_pioz80.interrupt_port_id = PIOZ80_PORT_NONE;

    pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_CPUBUS_RETI );

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite intlog: PIO_STATE event s RETI_APPLIED bitem - signalizuje
     * "PIOZ80 právě dokončil daisy chain handshake, vrácen do NONE state". */
    if ( TEST_TRACE_INTLOG_DISPATCH ) {
        uint32_t state = INTLOG_STATE_BIT_PIO_RETI_APPLIED;
        if ( g_pioz80.interrupt & PIOZ80_INTERRUPT_INT_BIT ) state |= INTLOG_STATE_BIT_PIO_READY;
        if ( g_pioz80.port[ PIOZ80_PORT_A ].icena == PIOZ80_ICENA_ENABLED
             || g_pioz80.port[ PIOZ80_PORT_B ].icena == PIOZ80_ICENA_ENABLED ) {
            state |= INTLOG_STATE_BIT_PIO_ARMED;
        }
        intlog_record_pio_state ( state );
    }

    /* trace-suite hwlog: RETI_APPLIED s decoded delta vůči stavu před
     * dispatch RETI. Emit jen pokud byl PIOZ80 zdrojem IRQ (jsme za guard
     * `if (interrupt != RECEIVED) return`). */
    if ( hwlog_active_reti ) {
        uint32_t delta = pioz80_hwlog_compute_delta ( &hwlog_snap_reti, port );
        pioz80_hwlog_emit ( HWLOG_PIOZ80_RETI_APPLIED,
                            (uint8_t) hwlog_reti_port_id,
                            0, 0, delta );
    }
#endif
}


void pioz80_port_id_event ( en_PIOZ80_PORT_ID port_id, en_PIOZ80_PORT_EVENT port_event, int pinvalue ) {
    st_PIOZ80_PORT *port = &g_pioz80.port[port_id];

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Hwlog snapshot pro BUS_INPUT_CHANGE - přechod úrovně CTC0/VBLN/...
     * na vstupních pinech může změnit masked_input + intfnc result + interrupt
     * stav. Logujeme jen externí pin-edge eventy (PA4_CTC0, PA5_VBLN), ne
     * RESET ani interní. RESET má vlastní pioz80_reset() a interní eventy
     * se dají odvodit z write hooků. */
    st_PIOZ80_HWLOG_SNAPSHOT hwlog_snap_bus;
    int hwlog_active_bus = TEST_TRACE_HWLOG_DISPATCH
        && ( port_event == PIOZ80_PORT_EVENT_PA4_CTC0
             || port_event == PIOZ80_PORT_EVENT_PA5_VBLN );
    if ( hwlog_active_bus ) {
        pioz80_hwlog_take_snapshot ( &hwlog_snap_bus, port );
    }
#endif

    pioz80_port_event ( port, port_event, pinvalue );

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if ( hwlog_active_bus ) {
        uint32_t delta = pioz80_hwlog_compute_delta ( &hwlog_snap_bus, port );
        /* sub_addr = bit pos pinu kterého se event týká (PA4=4, PA5=5).
         * value = nová úroveň (0/1). */
        uint8_t pin_bit = ( port_event == PIOZ80_PORT_EVENT_PA4_CTC0 ) ? 4 : 5;
        pioz80_hwlog_emit ( HWLOG_PIOZ80_BUS_INPUT_CHANGE, (uint8_t) port_id,
                            pin_bit, (uint8_t) ( pinvalue & 0x01 ), delta );
    }
#endif
}


/**
 * @brief Periodicka resync stavu tiskarny/plotteru do interruptu brany A.
 *
 * Pripojena tiskarna/plotter rizene vstupni signaly BUSY (PA0) a status (PA1).
 * Na rozdil od CTC0 (PA4) a VBLANK (PA5), ktere maji vlastni hranove eventy,
 * tyto signaly hranovy event nemaji - jejich zmena (zapnuti/vypnuti capture,
 * pripojeni/odpojeni plotteru, zmena BUSY behem kresby) by jinak sama o sobe
 * neprehodnotila interrupt brany A.
 *
 * Tiskarna je externi asynchronni zarizeni; interrupt nemusi byt cyklus-presny.
 * Staci proto "jednou za cas" (per-frame) prehodnotit interrupt brany A z
 * aktualnich vstupnich pinu. Vlastni prehodnoceni resi pioz80_port_event, ktere
 * cte ZIVE piny (vc. stavu tiskarny/plotteru) a diky interni SAME_INPUT
 * pojistce vyvola zmenu interruptu jen pri skutecne zmene - nic spurious.
 * Stav CTC0/VBLANK se mezi jejich hranovymi eventy nemeni, takze tahle resync
 * je pro ne neutralni.
 *
 * Vola se z per-frame eventu emulatoru (po dokroceni plotteru).
 */
void pioz80_input_resync ( void ) {
    pioz80_port_id_event ( PIOZ80_PORT_A, PIOZ80_PORT_EVENT_PA01_PRINTER, 0 );
}


/**
 * Casove posunuta udalost - IC_ENA byl nastaven na ENABLED.
 * Zmena se projevi 3 CPU takty po nasledujicim M1.
 *
 * @param event_ticks
 */
void pioz80_icena_event ( void ) {
    st_PIOZ80_PORT *port = &g_pioz80.port[g_pioz80.icena_event_port_id];
    DBGPRINTF ( DBGINF, "port: %c, IC_ENA - time shifted event\n", pioz80_dbg_get_port_name ( port->port_id ) );
    port->icena = PIOZ80_ICENA_ENABLED;
    pioz80_interrupt_manager ( PIOZ80_PORT_EVENT_INTERNAL_CHANGED_ICENA );
    g_pioz80.icena_event.ticks = -1;
    g_pioz80.icena_event_port_id = PIOZ80_PORT_NONE;
}

