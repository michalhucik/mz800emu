/*
 * File:   ctc8253.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 19. června 2015, 11:49
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

#ifndef CTC8253_H
#define CTC8253_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "mzarch/mzcommon_config.h"
#include <stdint.h>

/* st_EMUEVENT pro clk1m1_event pole - nepodmíněně (layout st_CTC8253
 * nesmí záviset na CLK1M1 volbě, mzhal krok 5). */
#include "mzarch/mzevent.h"

#include "mzarch/mzarch.h"

    typedef void (*ctc8253_out_cb_t)(unsigned value, unsigned event_screen_ticks);

    /* Addresni sbernice */
    typedef enum en_CTCADDR
    {
        CTCADDR_CTC0 = 0, /* Adresa citace 0 */
        CTCADDR_CTC1,     /* Adresa citace 1 */
        CTCADDR_CTC2,     /* Adresa citace 2 */
        CTCADDR_CWREG     /* Adresa Control Word Registru */
    } en_CTCADDR;

    /* Control Word (CW) - SC1, SC0, RL1, RL0, M2, M1, M0, BCD */

    /* CW - vyber citace */
    typedef enum en_CTC_CS
    {
        CTC_CS0 = 0,   /* Select Counter 0 */
        CTC_CS1,       /* Select Counter 1 */
        CTC_CS2,       /* Select Counter 2 */
        CTC_CS_ILLEGAL /* Ilegalni kombo */
    } en_CTC_CS;

    /* CW - Read/Load format */
    typedef enum en_CTC_RLF
    {
        CTC_RLF_LSB = 1, /* Read / Load LSB */
        CTC_RLF_MSB,     /* Read / Load MSB */
        CTC_RLF_LSBMSB   /* Read / Load LSB followed by MSB */
    } en_CTC_RLF;

    /* CW - Mode */
    typedef enum en_CTC_MODE
    {
        CTC_MODE0 = 0, /* Mode 0 - Interrupt on Terminal Count */
        CTC_MODE1,     /* Mode 1 - Programble One-Shot */
        CTC_MODE2,     /* Mode 2 - Rate Generator */
        CTC_MODE3,     /* Mode 3 - Square Wave Generator */
        CTC_MODE4,     /* Mode 4 - Software Triggered Strobe */
        CTC_MODE5,     /* Mode 5 - Hardware Triggered Strobe */
    } en_CTC_MODE;

    typedef enum en_CTC_STATE
    {
        CTC_STATE_INIT,                /* bylo vlozeno CW */
        CTC_STATE_INIT_DONE,           /* bylo vlozeno CW a po nem prisla sestupna CLK */
        CTC_STATE_LOAD,                /* byl zahajen LOAD */
        CTC_STATE_PRESET_ERROR,        /* PRESET = 0x0001 v MODE 2 - nelze pocitat, nove nastaveni VALUE zavola LOAD_DONE */
        CTC_STATE_LOAD_DONE,           /* mame dokoncen LOAD, nasleduje PRESET bez ohledu na stav GATE */
        CTC_STATE_WAIT_GATE1,          /* cekame na GATE = 1 */
        CTC_STATE_PRESET,              /* priprava pred zacatkem odectu, lze jej odvolat pri GATE = 0, do VALUE vlozime PRESET */
        CTC_STATE_PRESET32,            /* priprava pred zacatkem odectu, lze jej odvolat pri GATE = 0, do VALUE vlozime konstantu 32 */
        CTC_STATE_COUNTDOWN,           /* odecet s cilem */
        CTC_STATE_MODE1_TRIGGER_ERROR, /* MODE1: prisel trigger GATE = 0|1, ale jeste nebyl LOAD_DONE, po naslednem dokonceni LOAD zavolame PRESET32 */
        CTC_STATE_BLIND_COUNT,         /* odecet bez cile */

    } en_CTC_STATE;

    typedef struct st_CTC8253
    {
        unsigned out;     /* Vystupni hodnota citace: 0|1 */
        unsigned gate;    /* Posledni znama uroven GATE: 0|1 */
        en_CTC_MODE mode; /* Mode: 0 - 5 */
        unsigned bcd;     /* BCD: 0|1 */
        en_CTC_RLF rlf;   /* Read / Load Format */
        en_CTC_STATE state;
        unsigned load_done;

        unsigned rl_byte; /* pocet bajtu, ktere uz byly do ctc zapsany, nebo precteny - v 8253 je na to skutecne jen jeden registr! */

        unsigned latch_op; /* 1 - pokud mame neco natazeno v read_latch */
        unsigned read_latch;
        unsigned preset_value;
        unsigned preset_latch;
        unsigned value;
        unsigned mode3_destination_value;
        unsigned mode3_half_value;
        ctc8253_out_cb_t output_cb; /* tento callback je zavolan pri kazde zmene vystupniho stavu citace */

        /* Pole existují v OBOU CLK1M1 variantách (mzhal krok 5) - layout
         * struktury nesmí záviset na build volbě. SLOW varianta je
         * nepoužívá. */
        unsigned clk1m1_last_event_total_ticks;
        st_EMUEVENT clk1m1_event;
    } st_CTC8253;

    extern struct st_CTC8253 g_ctc8253[3];

    /* Debug/UI mirror: posledni byte zapsany na CW registr (CTCADDR_CWREG,
     * IORQ 0xD7 / MMIO 0xE007). 8253 hardware CW neumi precist - sequencer
     * pro mode/counter select. Tracking pouze pro Overview tab v debugger UI.
     * Default 0x00 dokud HW init nebyl proveden. */
    extern uint8_t g_ctc8253_last_cw_byte;

    extern void ctc8253_init(void);
    extern uint8_t ctc8253_read_byte(unsigned cs);
    extern void ctc8253_write_byte(unsigned addr, uint8_t value);
    extern void ctc8253_gate(unsigned addr, unsigned gate, unsigned event_screen_ticks);
    extern void ctc8253_clkfall(unsigned cs, unsigned event_ticks);


#define CTC8253_OUT(cs) g_ctc8253[cs].out


#define ctc82530_on_regDMD_changed(mode700, event_ticks)        \
    {                                                           \
        if (mode700)                                            \
        {                                                       \
            ctc8253_gate(0, gdg_get_regct53g7(), event_ticks);  \
        }                                                       \
        else                                                    \
        {                                                       \
            ctc8253_gate(0, 1, event_ticks);                    \
        };                                                      \
    }



    extern void ctc8253_ctc1m1_event(unsigned event_ticks);
    extern void ctc8253_sync_ctc0(void);
#define ctc8253_get_ctc1m1_event_pointer() (&g_ctc8253[CTC_CS0].clk1m1_event)

#include "mzarch/mzhal.h"
#define ctc8253_on_screen_done_event()                      \
    {                                                       \
        st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];             \
        if ((int)ctc0->clk1m1_event.ticks != -1)            \
        {                                                   \
            ctc0->clk1m1_event.ticks -= g_mzhal.video_screen_ticks; \
        };                                                  \
    }

#ifdef __cplusplus
}
#endif

#endif /* CTC8253_H */
