/*
 * File:   ctc8253.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 19. června 2015, 11:47
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

#include "mzarch/mzcommon_config.h"

#include "ctc8253.h"
#include "hw-generic/gdg/gdg.h"
#include "mzarch/mzhal.h"

#include "mzarch/mzarch.h"
#include "mzarch/interrupt.h"
#include "pioz80/pioz80.h"
#include "pio8255/pio8255.h"
#include "audio.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

// #define DBGLEVEL (DBGNON /* | DBGERR | DBGWAR | DBGINF*/)
// #define DBGLEVEL (DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

struct st_CTC8253 g_ctc8253[3];

/* Debug/UI mirror posledniho CW byte (viz ctc8253.h). Aktualizovano v
 * ctc8253_write_byte pri zapisu do CWREG. Default 0x00. */
uint8_t g_ctc8253_last_cw_byte = 0x00;

// #include "cmt/cmt.h"

// mame audio?
//#define audio_ctc0_changed(value, event_ticks)

static inline void ctc8253_ctc0_output_event(unsigned value, unsigned event_ticks)
{
    //    DBGPRINTF ( DBGINF, "CTC0 output event! (%d) - ticks: %d\n", value, event_ticks );
    //    DBGPRINTF ( DBGINF, "CTC0 output event! (%d) - total: %d\n", value, gdg_compute_total_ticks ( event_ticks ) );
    // DBGPRINTF ( DBGINF, "CTC0 output event! (%d)\n", value );

    pioz80_port_id_event(PIOZ80_PORT_A, PIOZ80_PORT_EVENT_PA4_CTC0, ~value & 0x01);

    /* Bugfix pro hru Ralye (Tatra-sys HD cpm disk 5) - nastavi ctc0 mode: 3, preset: 2 a povoli audio (pc00) - na Sharpu ten zvuk zrejme neprojde filtrem */
    if (!((g_ctc8253[CTC_CS0].mode == CTC_MODE3) && (g_ctc8253[CTC_CS0].preset_value == 2)))
    {
        audio_ctc0_changed((value & CTC_AUDIO_MASK), gdg_compute_total_ticks(event_ticks));
    };
}

static inline void ctc8253_ctc1_output_event(unsigned value, unsigned event_ticks)
{
    if (value != 0)
        return;
    ctc8253_clkfall(CTC_CS2, event_ticks);
}

static inline void ctc8253_ctc2_output_event(unsigned value, unsigned event_ticks)
{
    (void)value;
    (void)event_ticks;
    //DBGPRINTF(DBGINF, "CTC2 output event! (%d)\n", value);
    mzarch_interrupt_manager();
}

static inline void ctc8253_set_out(unsigned cs, unsigned value, unsigned event_ticks)
{
    /* zadna zmena */
    if (g_ctc8253[cs].out == value)
        return;

    g_ctc8253[cs].out = value;

    if (g_ctc8253[cs].output_cb != NULL)
    {
        g_ctc8253[cs].output_cb(value, event_ticks);
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* HWE - HW event BP hooks (ctc:zc0/zc1/zc2). ZC = output toggle
     * (= edge na out signal). value = nová hodnota out (0/1). */
    if ( cs == CTC_CS0 ) {
        if ( g_bp_event_active[ BP_EVENT_CTC_ZC0 ] ) {
            bp_event_fire ( BP_EVENT_CTC_ZC0, (int32_t) value );
        }
    } else if ( cs == CTC_CS1 ) {
        if ( g_bp_event_active[ BP_EVENT_CTC_ZC1 ] ) {
            bp_event_fire ( BP_EVENT_CTC_ZC1, (int32_t) value );
        }
    } else if ( cs == CTC_CS2 ) {
        if ( g_bp_event_active[ BP_EVENT_CTC_ZC2 ] ) {
            bp_event_fire ( BP_EVENT_CTC_ZC2, (int32_t) value );
        }
    }
#endif
}

void ctc8253_init(void)
{
    g_ctc8253_last_cw_byte = 0x00;
    g_ctc8253[CTC_CS0].output_cb = ctc8253_ctc0_output_event;
    g_ctc8253[CTC_CS1].output_cb = ctc8253_ctc1_output_event;
    g_ctc8253[CTC_CS2].output_cb = ctc8253_ctc2_output_event;
    ctc8253_gate(CTC_CS0, 0, 0);
    ctc8253_gate(CTC_CS1, 1, 0);
    ctc8253_gate(CTC_CS2, 1, 0);

    unsigned cs;
    for (cs = CTC_CS0; cs <= CTC_CS2; cs++)
    {
        g_ctc8253[cs].state = CTC_STATE_INIT_DONE;
        g_ctc8253[cs].load_done = 0;
        g_ctc8253[cs].mode = CTC_MODE0;
        g_ctc8253[cs].out = 0;
        g_ctc8253[cs].value = 0;
        g_ctc8253[cs].preset_value = 0xffff;
        g_ctc8253[cs].rl_byte = 0;
        g_ctc8253[cs].latch_op = 0;
        g_ctc8253[cs].rlf = CTC_RLF_LSBMSB;
        g_ctc8253[cs].bcd = 0;
    };

    g_ctc8253[CTC_CS0].clk1m1_event.ticks = -1;
    g_ctc8253[CTC_CS0].clk1m1_event.event_name = MZEVENT_CTC0;
}


static inline void ctc8253_update_ctc0_by_totalticks(unsigned event_total_ticks)
{
    /* Delička runtime z g_mzhal (mzhal 10e, warm cesta - per CTC sync);
     * unsigned dělení, hodnota per EXE beze změny. */
    const unsigned ctc0_divider = g_mzhal.gdgclk_ctc0_divider;
    unsigned elapsed_ticks = event_total_ticks - g_ctc8253[CTC_CS0].clk1m1_last_event_total_ticks;
    unsigned decremented = elapsed_ticks / ctc0_divider;
    g_ctc8253[CTC_CS0].value -= decremented;
    g_ctc8253[CTC_CS0].clk1m1_last_event_total_ticks += decremented * ctc0_divider;
}

void ctc8253_sync_ctc0(void)
{
    if (!(g_ctc8253[CTC_CS0].latch_op == 1))
    {
        if ((g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };
    };
}


/**
 * @brief Precte bajt z citace 8253 (HW-verne cteni s posunem byte-pointeru).
 *
 * Emuluje cteni datoveho portu 8253 pro vybrany citac @p cs. Vraci bud
 * zalatchovanou hodnotu (`read_latch`, pokud probehl Counter Latch prikaz =
 * `latch_op == 1`), nebo aktualni runtime hodnotu citace (`value`).
 *
 * Podle Read/Load Formatu (`rlf`) vraci LSB, MSB, nebo strida LSB/MSB pres
 * jednobajtovy ukazatel `rl_byte` (8253 ma na to fyzicky jediny registr).
 *
 * @param cs Index citace (CTC_CS0..CTC_CS2).
 * @return Precteny bajt (0..255).
 *
 * @note Side-effecty (zamerne, HW-verne): u CTC_RLF_LSBMSB posune `rl_byte`
 *       0<->1; po precteni posledniho bajtu uvolni `latch_op` (= konec Counter
 *       Latch cteni). Tyto mutace probihaji VZDY - nejsou potlaceny zadnym
 *       debug flagem. Drivejsi guard `if (!TEST_DEBUGGER_MEMOP_CALL)` byl
 *       odstranen, protoze cross-thread cteni `g_debugger.memop_call` (UI
 *       vlakno) racovalo s hostovym CTC ctenim na emu vlakne (CP/M RTC bug).
 *
 * @warning Funkce je urcena vyhradne pro GUEST cteni (CPU IORQ / mapped MMIO
 *          na emu vlakne). Debugger / UI okna NIKDY tuto funkci nevolaji -
 *          ctou raw fieldy `g_ctc8253[cs]` side-effect-free (viz ctc_window.cpp,
 *          io_catalog.c) a mapped-read pres `memory_read_byte` jde nosync
 *          cestou, ktera CTC nevola (vraci konstantu).
 *
 * @pre Volat z emu vlakna v ramci CPU instrukcni cesty.
 * @post U LSBMSB / latch cteni je aktualizovan `rl_byte` / `latch_op`.
 */
uint8_t ctc8253_read_byte(unsigned cs)
{

    uint8_t retval = 0;

    if (!(g_ctc8253[cs].latch_op == 1))
    {
        /*
                if ( ( cs == CTC_CS0 ) && ( g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN ) && ( g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1 ) ) {
                    ctc8253_update_ctc0_by_totalticks ( gdg_compute_total_ticks ( gdg_get_insigeop_ticks ( ) ) );
                };
         */
        if (cs == CTC_CS0)
        {
            ctc8253_sync_ctc0();
        };
    };

    unsigned value = (g_ctc8253[cs].latch_op == 1) ? g_ctc8253[cs].read_latch : g_ctc8253[cs].value;

    switch (g_ctc8253[cs].rlf)
    {

    case CTC_RLF_LSB:
        /* Posun byte-pointeru / uvolneni latche je VZDY HW-verny (bez ohledu
         * na debug stav). Drivejsi guard `if (!TEST_DEBUGGER_MEMOP_CALL)` mel
         * potlacit mutaci pri debuggerem-iniciovanem cteni, jenze debugger se
         * na tuto funkci nikdy nedostane (mapped-read jde pres nosync cestu
         * memory_read_byte, ktera CTC necte pres ctc8253_read_byte - vraci
         * konstantu; UI okna ctou raw g_ctc8253[] side-effect-free). Jediny
         * pripad, kdy guard fakticky firnul, byla cross-thread data-race:
         * UI vlakno nastavi ne-atomicky g_debugger.memop_call=1 (pro sve
         * nesouvisejici disasm cteni pameti) behem soubezneho hostova CTC
         * cteni na emu vlakne -> spurious potlaceni posunu rl_byte/latch_op
         * -> roztrzena 16-bit on-the-fly hodnota -> CP/M RTC hodiny skakaly.
         * Odstranenim guardu je guest cteni deterministicke (srovnano s
         * PIO8255, ktery zadny takovy guard nema a funguje korektne). */
        g_ctc8253[cs].latch_op = 0;
        retval = (value & 0xff);
        break;

    case CTC_RLF_MSB:
        g_ctc8253[cs].latch_op = 0;
        retval = (value >> 8) & 0xff;
        break;

    case CTC_RLF_LSBMSB:
        if (g_ctc8253[cs].rl_byte == 0)
        {
            g_ctc8253[cs].rl_byte = 1;
            retval = (value & 0xff);
        }
        else
        {
            g_ctc8253[cs].rl_byte = 0;
            g_ctc8253[cs].latch_op = 0;
            retval = (value >> 8) & 0xff;
        };
        break;
    };

    DBGPRINTF(DBGINF, "Read CTC addr: %d, value: 0x%02x, PC = 0x%04x\n", cs, retval, g_mzarch_main.instruction_addr);

    return retval;
}

void ctc8253_write_byte(unsigned addr, uint8_t value)
{

    en_CTC_CS cs;

    en_CTC_STATE old_state;

    addr = addr & 0x03;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat write do CTC8253.
     *
     * Sub-event:
     *   CONTROL_WRITE pro addr == CTCADDR_CWREG (= 3, write do CW registru)
     *   COUNTER_WRITE pro addr == CTC0..CTC2 (= 0..2, datový counter)
     *
     * Payload (per HW-log_format_CZ.md):
     *   [0] = addr (0..3)
     *   [1] = value
     *   [2] = pre-write rl_byte counteru (jen pro COUNTER_WRITE; LSB/MSB
     *         pořadí; pro CONTROL_WRITE = 0)
     *   [3..5] = rezervováno
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t sub = ( addr == CTCADDR_CWREG )
                          ? HWLOG_CTC8253_CONTROL_WRITE
                          : HWLOG_CTC8253_COUNTER_WRITE;
        uint8_t pre_rl = ( addr == CTCADDR_CWREG )
                             ? 0
                             : (uint8_t) g_ctc8253[ addr ].rl_byte;
        uint8_t payload[ 6 ] = {
            (uint8_t) addr, value, pre_rl, 0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_CTC8253, sub, payload );
    }
#endif

    DBGPRINTF(DBGINF, "WR 8253 - addr: %d, value: 0x%02x, PC: 0x%04x\n", addr, value, g_mzarch_main.instruction_addr);
    // printf("%s():%d - addr: %d, value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mz800_main.instruction_addr);

    if (addr == CTCADDR_CWREG)
    {
        /* Zapis do CW registru */

        /* Debug/UI mirror: zachyt vsechny zapsane CW bytes vcetne
         * CS_ILLEGAL (= jakkoliv write zustane viditelny v Overview).
         * HW samotne tento registr neuklada. */
        g_ctc8253_last_cw_byte = value;

        cs = value >> 6;

        /* Nepovolena adresa - nereagujeme */
        if (cs == CTC_CS_ILLEGAL)
            return;

        if ((cs == CTC_CS0) && (g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };

        old_state = g_ctc8253[cs].state;
        unsigned rlf = (value >> 4) & 0x03;

        g_ctc8253[cs].rl_byte = 0;

        /* LatchOp - priprava na cteni */
        if (rlf == 0)
        {
            DBGPRINTF(DBGINF, "LatchOP CTC: %d, PC = 0x%04x\n", cs, g_mzarch_main.instruction_addr);
            g_ctc8253[cs].latch_op = 1;

            g_ctc8253[cs].read_latch = g_ctc8253[cs].value;
            /* TODO: proverit jak se chova read latch, zmeni se pokud se dokoncil countdown, nebo pokud prisel trigger, atp. ? */
            return;
        };

        g_ctc8253[cs].latch_op = 0;
        g_ctc8253[cs].rlf = rlf;

        en_CTC_MODE mode = (value >> 1) & 0x07;
        if (mode > CTC_MODE5)
        {
            mode -= 2;
        };
        g_ctc8253[cs].mode = mode;
        g_ctc8253[cs].bcd = value & 0x01;
        g_ctc8253[cs].state = CTC_STATE_INIT;
        g_ctc8253[cs].load_done = 0;
        /* skutecny 8253 zrejme pri initu na value nesaha */
        // g_ctc8253[cs].value = 0;

        DBGPRINTF(DBGINF, "INIT CTC - 0x%02x - addr: %d, RLF: %d, MODE: %d, BCD: %d, PC = 0x%04x\n", value, cs, g_ctc8253[cs].rlf, g_ctc8253[cs].mode, g_ctc8253[cs].bcd, g_mzarch_main.instruction_addr);

        unsigned output_state = (g_ctc8253[cs].mode == CTC_MODE0) ? 0 : 1;
        ctc8253_set_out(cs, output_state, gdg_get_insigeop_ticks());

#if (DBGLEVEL & DBGWAR)
        if (g_ctc8253[cs].mode > CTC_MODE3)
        {
            DBGPRINTF(DBGWAR, "Unsupported mode: %d on CTC: %d\n", g_ctc8253[cs].mode, cs);
        };
#endif
    }
    else
    {

        /* Zapis do citace */
        cs = addr;

        if ((cs == CTC_CS0) && (g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };

        old_state = g_ctc8253[cs].state;

        g_ctc8253[cs].latch_op = 0;

        /* Zapocali jsme LOAD v MODE 0 */
        if (g_ctc8253[cs].mode == CTC_MODE0)
        {
            if (g_ctc8253[cs].state > CTC_STATE_INIT_DONE)
            {
                g_ctc8253[cs].state = CTC_STATE_LOAD;
                ctc8253_set_out(cs, 0, gdg_get_insigeop_ticks());
            };
        };
        DBGPRINTF(DBGINF, "LOAD CTC addr: %d, value: 0x%02x, PC = 0x%04x\n", cs, value, g_mzarch_main.instruction_addr);

        switch (g_ctc8253[cs].rlf)
        {

        case CTC_RLF_LSB:
            g_ctc8253[cs].preset_latch = value;
            break;

        case CTC_RLF_MSB:
            g_ctc8253[cs].preset_latch = value << 8;
            break;

        case CTC_RLF_LSBMSB:
            if (g_ctc8253[cs].rl_byte == 0)
            {
                g_ctc8253[cs].preset_latch = value;
                g_ctc8253[cs].rl_byte = 1;
                return;
            }
            else
            {
                g_ctc8253[cs].preset_latch |= value << 8;
                g_ctc8253[cs].rl_byte = 0;
            };
            break;
        };

        g_ctc8253[cs].preset_value = (g_ctc8253[cs].preset_latch == 0) ? 0x10000 : g_ctc8253[cs].preset_latch;

        if (g_ctc8253[cs].mode == CTC_MODE3)
        {

            if (g_ctc8253[cs].preset_value == 1)
            {
                g_ctc8253[cs].preset_value = 0x10001;
            };

            g_ctc8253[cs].mode3_half_value = g_ctc8253[cs].preset_value;
            if (g_ctc8253[cs].mode3_half_value & 1)
            {
                g_ctc8253[cs].mode3_half_value++;
            };
            g_ctc8253[cs].mode3_half_value >>= 1;
        };

        /* Dokoncen LOAD */

        if (g_ctc8253[cs].state < CTC_STATE_LOAD_DONE)
        {
            if (g_ctc8253[cs].state == CTC_STATE_INIT)
            {
                g_ctc8253[cs].load_done = 1;
            }
            else
            {
                g_ctc8253[cs].state = CTC_STATE_LOAD_DONE;
            };
        }
        else if (g_ctc8253[cs].state == CTC_STATE_MODE1_TRIGGER_ERROR)
        {
            g_ctc8253[cs].state = CTC_STATE_PRESET32;
        };
    };

    if (cs == CTC_CS0)
    {
        if (old_state != g_ctc8253[cs].state)
        {
            /* vytvorime event pro zavolani CTC0 ctc8253_clkfall() */
            g_ctc8253[CTC_CS0].clk1m1_event.ticks = gdg_proximate_clk1m1_event(gdg_get_insigeop_ticks());
            g_ctc8253[CTC_CS0].clk1m1_last_event_total_ticks = gdg_compute_total_ticks(g_ctc8253[CTC_CS0].clk1m1_event.ticks);

            if (g_ctc8253[CTC_CS0].clk1m1_event.ticks <= g_mzarch_main.event.ticks)
            {
                g_mzarch_main.event.ticks = g_ctc8253[CTC_CS0].clk1m1_event.ticks;
                g_mzarch_main.event.event_name = MZEVENT_CTC0;
            };
        };
    };
}

void ctc8253_clkfall(unsigned cs, unsigned event_ticks)
{

    switch (g_ctc8253[cs].mode)
    {

    case CTC_MODE0:
        if (g_ctc8253[cs].state >= CTC_STATE_COUNTDOWN)
        {
            g_ctc8253[cs].value--;
            if (g_ctc8253[cs].value == 0x0000)
            {
                ctc8253_set_out(cs, 1, event_ticks);
                g_ctc8253[cs].state = CTC_STATE_BLIND_COUNT;
                g_ctc8253[cs].value = 0xffff;
            };
            return;
        }
        else if (g_ctc8253[cs].state == CTC_STATE_LOAD_DONE)
        {

            g_ctc8253[cs].value = g_ctc8253[cs].preset_value;

            if (g_ctc8253[cs].gate == 1)
            {
                g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
            };
            return;
        };
        break;

    case CTC_MODE1:
        if (g_ctc8253[cs].state == CTC_STATE_BLIND_COUNT)
        {
            g_ctc8253[cs].value--;
            if (g_ctc8253[cs].value == 0x0000)
            {
                g_ctc8253[cs].value = 0xffff;
            };
            return;
        }
        else if (g_ctc8253[cs].state == CTC_STATE_COUNTDOWN)
        {
            g_ctc8253[cs].value--;
            if (g_ctc8253[cs].value == 0x0000)
            {
                ctc8253_set_out(cs, 1, event_ticks);
                if (g_ctc8253[cs].gate == 1)
                {
                    g_ctc8253[cs].state = CTC_STATE_BLIND_COUNT;
                }
                else
                {
                    g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
                };
            };
            return;
        }
        else if (g_ctc8253[cs].state == CTC_STATE_LOAD_DONE)
        {
            if (g_ctc8253[cs].gate == 1)
            {
                g_ctc8253[cs].state = CTC_STATE_BLIND_COUNT;
            }
            else
            {
                g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
            };
            return;
        }
        else if (g_ctc8253[cs].state == CTC_STATE_PRESET)
        {
            g_ctc8253[cs].value = g_ctc8253[cs].preset_value;
            ctc8253_set_out(cs, 0, event_ticks);
            g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
            return;
        }
        else if (g_ctc8253[cs].state == CTC_STATE_PRESET32)
        {
            g_ctc8253[cs].value = 32;
            g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
            return;
        };
        break;

    case CTC_MODE2:
        if (g_ctc8253[cs].state == CTC_STATE_COUNTDOWN)
        {

            g_ctc8253[cs].value--;

            if (g_ctc8253[cs].value == 0x0001)
            {
                ctc8253_set_out(cs, 0, event_ticks);
                g_ctc8253[cs].state = CTC_STATE_PRESET;
            };
            return;
        }
        else if ((g_ctc8253[cs].state == CTC_STATE_PRESET) || (g_ctc8253[cs].state == CTC_STATE_LOAD_DONE))
        {

            ctc8253_set_out(cs, 1, event_ticks);

            g_ctc8253[cs].value = g_ctc8253[cs].preset_value;

            if (g_ctc8253[cs].value == 0x0001)
            {
                g_ctc8253[cs].state = CTC_STATE_PRESET_ERROR;
            }
            else
            {
                if (g_ctc8253[cs].gate == 1)
                {
                    g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
                }
                else
                {
                    g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
                };
            };
            return;
        };
        break;

    case CTC_MODE3:

        if (g_ctc8253[cs].state == CTC_STATE_COUNTDOWN)
        {

            g_ctc8253[cs].value--;

            if (g_ctc8253[cs].value == g_ctc8253[cs].mode3_destination_value)
            {

                if (g_ctc8253[cs].out == 1)
                {

                    ctc8253_set_out(cs, 0, event_ticks);

                    g_ctc8253[cs].value = g_ctc8253[cs].mode3_half_value;
                    g_ctc8253[cs].mode3_destination_value = 0;
                }
                else
                {

                    ctc8253_set_out(cs, 1, event_ticks);

                    g_ctc8253[cs].value = g_ctc8253[cs].preset_value;
                    g_ctc8253[cs].mode3_destination_value = g_ctc8253[cs].mode3_half_value;

                    if (g_ctc8253[cs].gate == 1)
                    {
                        g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
                    }
                    else
                    {
                        g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
                    };
                };
            };
            return;
        }
        else if ((g_ctc8253[cs].state == CTC_STATE_PRESET) || (g_ctc8253[cs].state == CTC_STATE_LOAD_DONE))
        {
            ctc8253_set_out(cs, 1, event_ticks);

            g_ctc8253[cs].value = g_ctc8253[cs].preset_value;
            g_ctc8253[cs].mode3_destination_value = g_ctc8253[cs].mode3_half_value;

            if (g_ctc8253[cs].gate == 1)
            {
                g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
            };
            return;
        };
        break;

    case CTC_MODE4:
    case CTC_MODE5:
        // DBGPRINTF ( DBGWARN, "Unsupported mode: %d, on CTC: %d\n", g_ctc8253[cs].mode, cs );
        return;
        break;
    };

    if (g_ctc8253[cs].state == CTC_STATE_INIT)
    {
        if (g_ctc8253[cs].load_done == 1)
        {
            g_ctc8253[cs].state = CTC_STATE_LOAD_DONE;
            g_ctc8253[cs].load_done = 0;
        }
        else
        {
            g_ctc8253[cs].state = CTC_STATE_INIT_DONE;
        }
    };
}

void ctc8253_gate(unsigned cs, unsigned gate, unsigned event_ticks)
{
    gate = gate & 0x01;

    /* Gate se nezmenila - jdeme pryc */
    if (g_ctc8253[cs].gate == gate)
        return;

    g_ctc8253[cs].gate = gate;

    /* HWE: ctc:gate0_edge event byl vyřazen v V1.5 HWE redesign
     * (= nebyl v Michalově finálním listu, gate je vstup ne výstup). */

    if (g_ctc8253[cs].state == CTC_STATE_INIT)
        return;

    en_CTC_STATE old_state = g_ctc8253[cs].state;

    switch (g_ctc8253[cs].mode)
    {

    case CTC_MODE0:
        if (g_ctc8253[cs].gate == 0)
        {
            g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
        }
        else
        {
            if (g_ctc8253[cs].out == 0)
            {
                g_ctc8253[cs].state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                g_ctc8253[cs].state = CTC_STATE_BLIND_COUNT;
            };
        };
        break;

    case CTC_MODE1:
        if (g_ctc8253[cs].gate == 1)
        {
            if ((g_ctc8253[cs].state == CTC_STATE_LOAD_DONE) || (g_ctc8253[cs].state == CTC_STATE_WAIT_GATE1) || (g_ctc8253[cs].state == CTC_STATE_COUNTDOWN))
            {
                g_ctc8253[cs].state = CTC_STATE_PRESET;
            }
            else if (g_ctc8253[cs].state == CTC_STATE_INIT_DONE)
            {
                /* Nabezna GATE prisla drive, nez byl dokoncen LOAD */
                ctc8253_set_out(cs, 0, event_ticks);
                g_ctc8253[cs].state = CTC_STATE_MODE1_TRIGGER_ERROR;
            };
        }
        else if (g_ctc8253[cs].state == CTC_STATE_BLIND_COUNT)
        {
            /* v tuto chvili by se melo jednat o sestupnou hranu GATE */
            g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
        };
        break;

    case CTC_MODE2:
    case CTC_MODE3:
        if (g_ctc8253[cs].gate == 0)
        {
            if ((g_ctc8253[cs].state == CTC_STATE_COUNTDOWN) || (g_ctc8253[cs].state == CTC_STATE_PRESET))
            {
                ctc8253_set_out(cs, 1, event_ticks);
                g_ctc8253[cs].state = CTC_STATE_WAIT_GATE1;
            };
        }
        else if ((g_ctc8253[cs].state == CTC_STATE_WAIT_GATE1) && (g_ctc8253[cs].gate == 1))
        {
            g_ctc8253[cs].state = CTC_STATE_PRESET;
        };
        break;

    case CTC_MODE4:
    case CTC_MODE5:
        // DBGPRINTF ( DBGWARN, "Unsupported mode: %d, on CTC: %d\n", g_ctc8253[cs].mode, cs );
        break;
    };

    if (cs == CTC_CS0)
    {
        if (old_state != g_ctc8253[cs].state)
        {

            if ((old_state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
            {
                ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(event_ticks));
            };

            g_ctc8253[CTC_CS0].clk1m1_event.ticks = gdg_proximate_clk1m1_event(event_ticks);
            g_ctc8253[CTC_CS0].clk1m1_last_event_total_ticks = gdg_compute_total_ticks(g_ctc8253[CTC_CS0].clk1m1_event.ticks);

            if (g_ctc8253[CTC_CS0].clk1m1_event.ticks <= g_mzarch_main.event.ticks)
            {
                g_mzarch_main.event.ticks = g_ctc8253[CTC_CS0].clk1m1_event.ticks;
                g_mzarch_main.event.event_name = MZEVENT_CTC0;
            };
        };
    };
}


void ctc8253_ctc1m1_event(unsigned event_ticks)
{

    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];

    unsigned event_total_ticks = gdg_compute_total_ticks(event_ticks);

    /*
     * Co vraci ctc8253_clkfall():
     *
     * if ( ctc0->state == CTC_STATE_INIT )
     *      ret vzdy: CTC_STATE_LOAD_DONE, nebo CTC_STATE_INIT_DONE
     *
     * if ( ctc0->state >= CTC_STATE_LOAD_DONE )
     *      ret M0: CTC_STATE_COUNTDOWN, CTC_STATE_WAIT_GATE1
     *      ret M1: CTC_STATE_BLIND_COUNT, CTC_STATE_WAIT_GATE1
     *      ret M2: CTC_STATE_BLIND_COUNT, CTC_STATE_PRESET_ERROR, CTC_STATE_WAIT_GATE1
     *      ret M3: CTC_STATE_COUNTDOWN, CTC_STATE_WAIT_GATE1
     *
     *
     * if ( ( ctc0->state == CTC_STATE_PRESET ) || ( ctc0->state == CTC_STATE_PRESET32 ) )
     *      ret M1: CTC_STATE_COUNTDOWN
     *      ret M2: CTC_STATE_BLIND_COUNT, CTC_STATE_PRESET_ERROR, nebo CTC_STATE_WAIT_GATE1
     *      ret M3: CTC_STATE_COUNTDOWN, CTC_STATE_WAIT_GATE1
     *
     * if ( ctc0->state >= CTC_STATE_COUNTDOWN )
     *      ret M0 - CTC_STATE_COUNTDOWN, nebo CTC_STATE_BLIND_COUNT
     *      ret M1 - CTC_STATE_COUNTDOWN, CTC_STATE_BLIND_COUNT, CTC_STATE_WAIT_GATE1
     *      ret M2 - CTC_STATE_COUNTDOWN, CTC_STATE_PRESET
     *      ret M3 - CTC_STATE_COUNTDOWN, CTC_STATE_WAIT_GATE1
     *
     */

    if (ctc0->state >= CTC_STATE_COUNTDOWN)
    {
        int elapsed_ticks = event_total_ticks - ctc0->clk1m1_last_event_total_ticks;
        if (elapsed_ticks > 0)
        {
            /* POZOR: SIGNED aritmetika (mzhal 10e, ARITMETIKA-INVENTURA
             * C1) - elapsed_ticks je int a po odečtu deličky může být
             * <= 0; C dělení se zaokrouhluje k nule. Explicitní (int)
             * cast drží signed sémantiku i s unsigned polem g_mzhal
             * (bez castu by C promoce udělala unsigned dělení = bug). */
            elapsed_ticks -= (int)g_mzhal.gdgclk_ctc0_divider;
            ctc0->value -= elapsed_ticks / (int)g_mzhal.gdgclk_ctc0_divider;
        }
        else
        {
            /* pokus o bugfix - pokud se kratce pred eventem cetlo z CTC, tak value uz muze mit destinacni hodnotu - deje se u Galao, kde to zpusobuje vypadek zvuku */
            switch (ctc0->mode)
            {
            case CTC_MODE0:
            case CTC_MODE1:
                ctc0->value = 1;
                break;
            case CTC_MODE2:
                ctc0->value = 2;
                break;
            case CTC_MODE3:
                ctc0->value = ctc0->mode3_destination_value + 1;
                break;
            case CTC_MODE4:
            case CTC_MODE5:
                break;
            };
        }
    };

    en_CTC_STATE old_state = ctc0->state;
    ctc8253_clkfall(CTC_CS0, event_ticks);
    ctc0->clk1m1_last_event_total_ticks = event_total_ticks;

    if (ctc0->state == CTC_STATE_LOAD_DONE)
    {
        ctc0->clk1m1_event.ticks = gdg_proximate_clk1m1_event(event_ticks);
    }
    else if (ctc0->state == CTC_STATE_COUNTDOWN)
    {

        /* Pokud je nyni CTC_STATE_COUNTDOWN, tak nasleduje event pri ocekavanem value: */
        /* M0 - value = 1 */
        /* M1 - value = 1 */
        /* M2 - value = 2 */
        /* M3 - value =  g_ctc8253[cs].mode3_destination_value + 1 */

        unsigned destination_clk1m1_falls = 0;

        switch (ctc0->mode)
        {
        case CTC_MODE0:
        case CTC_MODE1:
            destination_clk1m1_falls = ctc0->value;
            break;

        case CTC_MODE2:
            destination_clk1m1_falls = ctc0->value - 1;
            break;

        case CTC_MODE3:
            destination_clk1m1_falls = ctc0->value - ctc0->mode3_destination_value;
            break;

        case CTC_MODE4:
        case CTC_MODE5:
            // DBGPRINTF ( DBGWARN, "Unsupported mode: %d, on CTC: %d\n", g_ctc8253[cs].mode, cs );
            destination_clk1m1_falls = -1;
            break;
        };

        if ((int)destination_clk1m1_falls != -1)
        {
            ctc0->clk1m1_event.ticks = event_ticks + (destination_clk1m1_falls * g_mzhal.gdgclk_ctc0_divider);
        }
        else
        {
            ctc0->clk1m1_event.ticks = -1;
        }
    }
    else if ((old_state == CTC_STATE_COUNTDOWN) && (ctc0->state == CTC_STATE_PRESET))
    {
        /* M2 - skoncil COUNTDOWN */
        ctc0->clk1m1_event.ticks = event_ticks + (1 * g_mzhal.gdgclk_ctc0_divider);
    }
    else
    {
        ctc0->clk1m1_event.ticks = -1;
    };
}

