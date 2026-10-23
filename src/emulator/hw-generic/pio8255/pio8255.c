/*
 * File:   pio8255.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 21. června 2015, 23:13
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
#include <string.h>
#include <glib.h>

#include "pio8255.h"
#include "mzarch/mzarch.h"
#include "mzarch/interrupt.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"
#include "audio.h"
#include "ctc8253/ctc8253.h"
#include "cmt/cmt.h"
#include "cmt/cmthack.h"

// #define DBGLEVEL (DBGNON /* | DBGERR | DBGWAR | DBGINF*/)
// #define DBGLEVEL (DBGNON | DBGERR | DBGWAR | DBGINF )
#include "debug.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

#define iface_keyboard_pool_keyboard_events() // v multitask modu je toto volano z jineho vlakna

/*
 * Emulace PIO8255
 *
 *
 */

#define DEF_PIO8255_PORTA 0x00
#define DEF_PIO8255_PORTB 0x01
#define DEF_PIO8255_PORTC 0x02
#define DEF_PIO8255_MASTER 0x03

st_PIO8255 g_pio8255;

void pio8255_keyboard_matrix_reset(void)
{
    memset(&g_pio8255.keyboard_matrix, 0xff, sizeof(g_pio8255.keyboard_matrix));
}

void pio8255_vkbd_matrix_reset(void)
{
    memset(&g_pio8255.vkbd_matrix, 0xff, sizeof(g_pio8255.vkbd_matrix));
}

void pio8255_vkbd_probe_arm(int col, int bit)
{
    if (col < 0 || col > 9 || bit < 0 || bit > 7)
    {
        /* mimo rozsah - probe necháme neaktivní */
        g_pio8255.vkbd_probe.active = false;
        return;
    }
    g_pio8255.vkbd_probe.col = (uint8_t)col;
    g_pio8255.vkbd_probe.bit = (uint8_t)bit;
    g_pio8255.vkbd_probe.seen = false;
    g_pio8255.vkbd_probe.active = true;
}

bool pio8255_vkbd_probe_check(void)
{
    return g_pio8255.vkbd_probe.seen;
}

void pio8255_vkbd_probe_disarm(void)
{
    g_pio8255.vkbd_probe.active = false;
}

void pio8255_autotype_set_key_down_ms(double ms)
{
    g_pio8255.vkbd_autotype_kd_ms = ms;
    g_pio8255.vkbd_autotype_kd_ticks = ms * (GDGCLK_BASE / 1000);
}

void pio8255_autotype_set_key_up_ms(double ms)
{
    g_pio8255.vkbd_autotype_ku_ms = ms;
    g_pio8255.vkbd_autotype_ku_ticks = ms * (GDGCLK_BASE / 1000);
}

/** @brief Začátek textu autotype poslední generace (emu vlákno). */
static const char *s_autotype_start = NULL;
/** @brief Generace posledního spuštění autotype (atomicky). */
static gint s_autotype_gen = 0;
/** @brief Generace posledního dokončeného / zastaveného autotype (atomicky). */
static gint s_autotype_done_gen = 0;
/** @brief Zpracované bajty textu poslední generace (atomicky). */
static gint s_autotype_typed = 0;


void pio8255_set_autotype(char *txt)
{
    if (txt)
    {
        s_autotype_start = txt;
        g_atomic_int_set(&s_autotype_typed, 0);
        g_atomic_int_inc(&s_autotype_gen);
    }
    else
    {
        g_atomic_int_set(&s_autotype_done_gen, g_atomic_int_get(&s_autotype_gen));
    };
    g_pio8255.vkbd_autotype = txt;
    g_pio8255.vkbd_autotype_col = -1;
}


void pio8255_autotype_progress(int *gen, int *done_gen, int *typed)
{
    if (gen) *gen = g_atomic_int_get(&s_autotype_gen);
    if (done_gen) *done_gen = g_atomic_int_get(&s_autotype_done_gen);
    if (typed) *typed = g_atomic_int_get(&s_autotype_typed);
}

void pio8255_init(void)
{
    PIO8255_KEYBOARD_MATRIX_RESET();
    memset(&g_pio8255.vkbd_matrix, 0xff, sizeof(g_pio8255.vkbd_matrix));
    memset(&g_pio8255.vkbd_probe, 0x00, sizeof(g_pio8255.vkbd_probe)); /* probe neaktivní */
    g_pio8255.signal_PA = 0x00;
    g_pio8255.signal_PA_keybord_column = 0x00;
    g_pio8255.signal_PC = 0x00;
    g_pio8255.last_cw_byte = 0x00;
    pio8255_autotype_set_key_down_ms(20);
    pio8255_autotype_set_key_up_ms(20);
}

// static unsigned long bdcounter = 0;

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
/*
 * HWE - HW event BP hook (cmt:motor). PC3 bit = motor on/off control.
 * Voláno z pio8255_write po update signal_pc03 (= edge na write
 * targetu). Enforce vrstva si rozhodne dle trigger condition.
 */
static inline void pio8255_bp_event_cmt_motor ( unsigned new_pc03 )
{
    if ( g_bp_event_active[ BP_EVENT_CMT_MOTOR ] ) {
        bp_event_fire ( BP_EVENT_CMT_MOTOR, (int32_t) ( new_pc03 & 1 ) );
    }
}


/*
 * HWE - HW event BP wrapper pro CMT motor sense bit (PC4 input).
 *
 * Vrací aktuální simulovaný stav motorového sense bitu, který se vrací
 * v PortC read jako bit 4. Při CMTHACK = g_pio8255.signal_pc04
 * (= toggle při PC03 nábežné hraně, viz pio8255_write). Bez CMTHACK =
 * fabulace na základě CMT_TEST_STOP / CMT_TEST_PAUSED stavu.
 *
 * TODO: nahradit reálnou simulací motorového sense (= poklepávajícího
 * spínače, který opravdové MZ-800 dává náhodně 0/1 dokud motor točí).
 */
static inline unsigned pio8255_cmt_mstate ( void )
{
    if ( CMTHACK_TEST_IS_INSTALLED ) {
        return g_pio8255.signal_pc04 & 1;
    }
    return ( ( CMT_TEST_STOP ) || ( CMT_TEST_PAUSED ) ) ? 0 : 1;
}
#endif

/* data do CMT */
static inline void pio8255_set_pc01(uint8_t new_pc01)
{
    if (new_pc01 != g_pio8255.signal_pc01)
    {
        g_pio8255.signal_pc01 = new_pc01;
        cmt_write_data(new_pc01);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* HWE - HW event BP hook (cmt:out). PC1 bit = data do CMT. */
        if ( g_bp_event_active[ BP_EVENT_CMT_OUT ] ) {
            bp_event_fire ( BP_EVENT_CMT_OUT, (int32_t) ( new_pc01 & 1 ) );
        }
#endif
        // printf("bdc=%lu\n", bdcounter++);
#if 0
        static uint32_t last = 0;
#ifdef LINUX
        printf ( "CMT: %d - %lu, %lu\n", new_pc01, gdg_get_total_ticks ( ) - last, ( gdg_get_total_ticks ( ) - last ) / 8 );
#else
        printf ( "CMT: %d - %llu, %llu\n", new_pc01, gdg_get_total_ticks ( ) - last, ( gdg_get_total_ticks ( ) - last ) / 8 );
#endif
        last = gdg_get_total_ticks ( );
#endif
    };
}

void pio8255_write(int addr, uint8_t value)
{

    DBGPRINTF(DBGINF, "addr = 0x%02x, value = 0x%02x, PC = 0x%04x\n", addr, value, g_mzarch_main.instruction_addr);
    //printf("%s():%d - addr: %d, value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mzarch_main.instruction_addr);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat write do PIO8255.
     *
     * Sub-event vybrán dle addr (0..3 = PORT_A, PORT_B, PORT_C, CW).
     *
     * Payload (per HW-log_format_CZ.md):
     *   [0] = addr (0..3)
     *   [1] = value (raw byte)
     *   [2..5] = rezervováno
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        uint8_t sub;
        switch ( addr & 0x03 ) {
            case 0: sub = HWLOG_PIO8255_PORT_A_WRITE;  break;
            case 1: sub = HWLOG_PIO8255_PORT_B_WRITE;  break;
            case 2: sub = HWLOG_PIO8255_PORT_C_WRITE;  break;
            default: sub = HWLOG_PIO8255_CONTROL_WRITE; break;
        }
        uint8_t payload[ 6 ] = {
            (uint8_t)( addr & 0xff ), value, 0, 0, 0, 0
        };
        hwlog_record ( HWLOG_CHIP_PIO8255, sub, payload );
    }
    /* HWE: ppi:pa_write a ppi:pc_change vyřazené (= IORQ_W na port C/A
     * je expressivnější, žádný information loss). */
#endif

    int bit_setres;

    switch (addr)
    {

    case DEF_PIO8255_PORTA:
        DBGPRINTF(DBGINF, "addr = %d (PORT_A), value = 0x%02x )\n", addr, value);
        // printf("%s():%d - addr: %d (PORT_A), value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mzarch_main.instruction_addr);
        g_pio8255.signal_PA = value;

        // 0. - 3. bit vzorkovani klavesnice - aktivni pri H
        // 10 - 15: 74LS145 nevybere zadny sloupec -> index 10 (Port B cte 0xFF,
        // overeno na HW MZ-800, KBDCOL 2026-10-08)
        int pahalf = value & 0x0f;
        g_pio8255.signal_PA_keybord_column = (pahalf <= 9) ? pahalf : 10;

        // PA4 (maska 10h) strobe JOY1 (IN 0F0h), aktivní v L.
        // PA5 (maska 20h) strobe JOY2 (IN 0F1h), aktivní v L. PA6 není zapojen.
        // Zdroj: SM str. 35; ROM @JOYIN (0FA00h) strobuje oba zápisem 0CFh.
        g_pio8255.signal_PA_joy1_enabled = (!(value & 0x10)) ? 1 : 0;
        g_pio8255.signal_PA_joy2_enabled = (!(value & 0x20)) ? 1 : 0;

        // 7. bit cursor timer reset - aktivni pri L
        if (!(value & 0x80))
        {
            mz800_main_cursor_timer_reset();
        };

        break;

    case DEF_PIO8255_PORTC:

        DBGPRINTF(DBGINF, "addr = %d (PORT_C), value = 0x%02x), PC = 0x%04x\n", addr, value, g_mzarch_main.instruction_addr);
        // printf("%s():%d - addr: %d (PORT_C), value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mzarch_main.instruction_addr);

        /* blokovani CTC0 - zvukovy vystup */
        g_pio8255.signal_pc00 = (value >> 0) & 0x01;
        DBGPRINTF(DBGINF, "audio ctc0 mask (pc00): %d\n", g_pio8255.signal_pc00);
        audio_ctc0_changed((CTC8253_OUT(0) & CTC_AUDIO_MASK), gdg_compute_total_ticks(gdg_get_insigeop_ticks()));

        /* data do CMT */
        pio8255_set_pc01((value >> 1) & 0x01);

        /* blokovani CTC2 - preruseni z CTC */
        g_pio8255.signal_pc02 = (value >> 2) & 0x01;
        DBGPRINTF(DBGINF, "interrupt ctc2 mask (pc02): %d\n", g_pio8255.signal_pc02);
        mzarch_interrupt_manager();

        /* rizeni motoru CMT - nabezna hrana provede zmenu */
        unsigned old_pc03_state = g_pio8255.signal_pc03;
        g_pio8255.signal_pc03 = (value >> 3) & 0x01;
        DBGPRINTF(DBGINF, "cmt motor driver (pc03): %d\n", g_pio8255.signal_pc03);
        if ((old_pc03_state == 0) && (g_pio8255.signal_pc03 == 1))
        {
            if (CMTHACK_TEST_IS_INSTALLED)
            {
                g_pio8255.signal_pc04 = (~g_pio8255.signal_pc04) & 1;
                DBGPRINTF(DBGINF, "set CMT motor for CMT hack (pc04 - input): %d\n", g_pio8255.signal_pc04);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
                /* V1.5 fáze 2.2: HWE BP_EVENT_CMT_MSTATE edge fire pro
                 * CMTHACK případ - pc04 toggle se děje na PC03 nábežné
                 * hraně. Fire na skutečné mstate change, nezávisle na
                 * PortC pollingu. */
                if ( g_bp_event_active[ BP_EVENT_CMT_MSTATE ] ) {
                    bp_event_fire ( BP_EVENT_CMT_MSTATE,
                                    (int32_t) ( g_pio8255.signal_pc04 & 1 ) );
                }
#endif
            }
            else if (!CMT_TEST_STOP)
            {
                if (CMT_TEST_PAUSED)
                {
                    cmt_pause(0);
                }
                else
                {
                    cmt_pause(1);
                };
                DBGPRINTF(DBGINF, "set CMT motor (virtual CMT): %d\n", ((CMT_TEST_STOP) || (CMT_TEST_PAUSED)) ? 0 : 1);
                /* V1.5 fáze 2.2: pro non-CMTHACK případ se BP_EVENT_CMT_MSTATE
                 * fire dělá v cmt.c při změně CMT state (pause/play/stop) -
                 * mstate odvozený z (CMT_TEST_STOP || CMT_TEST_PAUSED). */
            }
            else
            {
                DBGPRINTF(DBGINF, "set CMT motor (virtual CMT): ignored\n");
            };
        };
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        pio8255_bp_event_cmt_motor ( g_pio8255.signal_pc03 );
#endif
        break;

    case DEF_PIO8255_MASTER:

        DBGPRINTF(DBGINF, "addr = %d (MASTER_PORT), value = 0x%02x, PC = 0x%04x\n", addr, value, g_mzarch_main.instruction_addr);
        // printf("%s():%d - addr: %d (MASTER_PORT), value: 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, value, g_mzarch_main.instruction_addr);

        /* Debug/UI mirror: zachyt jakykoliv write na Control word
         * (Mode Set i Bit Set/Reset). HW samotne tento registr neuklada;
         * tracking je cistne pro Overview tab v debugger UI. */
        g_pio8255.last_cw_byte = value;

        if (value & 0x80)
        {
            if (value == 0x8a)
            {
                /* nastaveni MODE0, PortA: out, PortC_up: in, PortB: in, PortC_down: out */
                /* vystupy jsou nastaveny na 0 */
                g_pio8255.signal_PA = 0x00;
                g_pio8255.signal_PA_keybord_column = 0x00;
                /* PA4 = PA5 = 0: strobe obou joysticků aktivní */
                g_pio8255.signal_PA_joy1_enabled = 1;
                g_pio8255.signal_PA_joy2_enabled = 1;
                g_pio8255.signal_pc00 = 0;
                pio8255_set_pc01(0);
                g_pio8255.signal_pc02 = 0;
                DBGPRINTF(DBGINF, "reset - pc00 - pc03 = 0x00\n");

                audio_ctc0_changed((CTC8253_OUT(0) & CTC_AUDIO_MASK), gdg_compute_total_ticks((gdg_get_insigeop_ticks())));
                mzarch_interrupt_manager();
#if (DBGLEVEL & DBGWAR)
            }
            else
            {
                DBGPRINTF(DBGWAR, "addr = %d, value = 0x%02x - UNSUPORTED MODE! PC: 0x%04x\n", addr, value, g_mzarch_main.cpu->pc);
#endif
            };
        }
        else
        {
            bit_setres = (value >> 1) & 0x07;

            if (bit_setres == 0)
            {
                g_pio8255.signal_pc00 = value & 0x01;
                DBGPRINTF(DBGINF, "audio ctc0 mask (pc00): %d\n", g_pio8255.signal_pc00);
                audio_ctc0_changed((CTC8253_OUT(0) & CTC_AUDIO_MASK), gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
            }
            else if (bit_setres == 1)
            {
                pio8255_set_pc01(value & 0x01);
            }
            else if (bit_setres == 2)
            {
                g_pio8255.signal_pc02 = value & 0x01;
                DBGPRINTF(DBGINF, "interrupt ctc2 mask (pc02): %d\n", g_pio8255.signal_pc02);
                mzarch_interrupt_manager();
            }
            else if (bit_setres == 3)
            {
                /* rizeni motoru CMT - nabezna hrana provede zmenu */
                unsigned old_pc03_state = g_pio8255.signal_pc03;
                g_pio8255.signal_pc03 = value & 0x01;
                DBGPRINTF(DBGINF, "cmt motor driver (pc03): %d\n", g_pio8255.signal_pc03);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
                pio8255_bp_event_cmt_motor ( g_pio8255.signal_pc03 );
#endif
                if ((old_pc03_state == 0) && (g_pio8255.signal_pc03 == 1))
                {
                    if (CMTHACK_TEST_IS_INSTALLED)
                    {
                        g_pio8255.signal_pc04 = (~g_pio8255.signal_pc04) & 1;
                        DBGPRINTF(DBGINF, "set CMT motor for CMT hack (pc04 - input): %d\n", g_pio8255.signal_pc04);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
                        /* V1.5 fáze 2.2: HWE BP_EVENT_CMT_MSTATE edge fire
                         * pro CMTHACK případ (bit_setres=3 cesta). */
                        if ( g_bp_event_active[ BP_EVENT_CMT_MSTATE ] ) {
                            bp_event_fire ( BP_EVENT_CMT_MSTATE,
                                            (int32_t) ( g_pio8255.signal_pc04 & 1 ) );
                        }
#endif
                    }
                    else if (!CMT_TEST_STOP)
                    {
                        if (CMT_TEST_PAUSED)
                        {
                            cmt_pause(0);
                        }
                        else
                        {
                            cmt_pause(1);
                        };
                        DBGPRINTF(DBGINF, "set CMT motor (virtual CMT): %d\n", ((CMT_TEST_STOP) || (CMT_TEST_PAUSED)) ? 0 : 1);
                    }
                    else
                    {
                        DBGPRINTF(DBGINF, "set CMT motor (virtual CMT): ignored\n");
                    };
                };
            }
            else
            {
                /* TODO: ostatni bity nas budou zajimat pozdeji:) */
                printf("PIO8255 NOT IMPL. - bit: %d, value: %d\n", bit_setres, value & 1);
            };
        };
        break;
    };

#if 0
    static unsigned int last_pc01_change = 0;
    static int last_pc01_state = 0;

    if ( g_pio8255.signal_pc01 != last_pc01_state ) {
        printf ( "CMT: %d, %d\n", last_pc01_state, g_timer.counter_gdg - last_pc01_change );
        last_pc01_state = g_pio8255.signal_pc01;
        last_pc01_change = g_timer.counter_gdg;
    };
#endif
}

int pio8255_autotype_get_matrix(char c, uint8_t *ret, bool *ret_shift)
{

    struct
    {
        char c;
        int col;
        int row;
        int shift;
    } key[] = {
        // col0
        {'_', 0, 7, 0},                    // Blank key
                                           // 6 => [Graph]
                                           //{ ???, 0, 5, 0 }, // Sipka dolu
        {PIO8255_VKBDAUTO_LIBRA, 0, 5, 1}, // Libra + shift
                                           // 4 => [Alpha]
        {'\t', 0, 3, 0},
        {';', 0, 2, 0},
        {'+', 0, 2, 1},
        {':', 0, 1, 0},
        {'*', 0, 1, 1},
        {'\n', 0, 0, 0},

        // col1
        {'Y', 1, 7, 0},
        {'y', 1, 7, 1},
        {'Z', 1, 6, 0},
        {'z', 1, 6, 1},
        {'@', 1, 5, 0},
        {'`', 1, 5, 1},
        {'[', 1, 4, 0},
        {'{', 1, 4, 1},
        {']', 1, 3, 0},
        {'}', 1, 3, 1},
        {']', 1, 2, 0},
        {'}', 1, 2, 1},

        // col2
        {'Q', 2, 7, 0},
        {'q', 2, 7, 1},
        {'R', 2, 6, 0},
        {'r', 2, 6, 1},
        {'S', 2, 5, 0},
        {'s', 2, 5, 1},
        {'T', 2, 4, 0},
        {'t', 2, 4, 1},
        {'U', 2, 3, 0},
        {'u', 2, 3, 1},
        {'V', 2, 2, 0},
        {'v', 2, 2, 1},
        {'W', 2, 1, 0},
        {'w', 2, 1, 1},
        {'X', 2, 0, 0},
        {'x', 2, 0, 1},

        // col3
        {'I', 3, 7, 0},
        {'i', 3, 7, 1},
        {'J', 3, 6, 0},
        {'j', 3, 6, 1},
        {'K', 3, 5, 0},
        {'k', 3, 5, 1},
        {'L', 3, 4, 0},
        {'l', 3, 4, 1},
        {'M', 3, 3, 0},
        {'m', 3, 3, 1},
        {'N', 3, 2, 0},
        {'n', 3, 2, 1},
        {'O', 3, 1, 0},
        {'o', 3, 1, 1},
        {'P', 3, 0, 0},
        {'p', 3, 0, 1},

        // col4
        {'A', 4, 7, 0},
        {'a', 4, 7, 1},
        {'B', 4, 6, 0},
        {'b', 4, 6, 1},
        {'C', 4, 5, 0},
        {'c', 4, 5, 1},
        {'D', 4, 4, 0},
        {'d', 4, 4, 1},
        {'E', 4, 3, 0},
        {'e', 4, 3, 1},
        {'F', 4, 2, 0},
        {'f', 4, 2, 1},
        {'G', 4, 1, 0},
        {'g', 4, 1, 1},
        {'H', 4, 0, 0},
        {'h', 4, 0, 1},

        // col5
        {'1', 5, 7, 0},
        {'!', 5, 7, 1},
        {'2', 5, 6, 0},
        {'"', 5, 6, 1},
        {'3', 5, 5, 0},
        {'#', 5, 5, 1},
        {'4', 5, 4, 0},
        {'$', 5, 4, 1},
        {'5', 5, 3, 0},
        {'%', 5, 3, 1},
        {'6', 5, 2, 0},
        {'&', 5, 2, 1},
        {'7', 5, 1, 0},
        {'\'', 5, 1, 1},
        {'8', 5, 0, 0},
        {'(', 5, 0, 1},

        // col6
        {'\\', 6, 7, 0},
        {'|', 6, 7, 1},
        {'^', 6, 6, 0},
        {'~', 6, 6, 1},
        {'-', 6, 5, 0},
        {'=', 6, 5, 1},
        {' ', 6, 4, 0},
        //{ ' ', 6, 4, 1 },
        {'0', 6, 3, 0},
        {PIO8255_VKBDAUTO_PI, 6, 3, 1}, // PI
        {'9', 6, 2, 0},
        {')', 6, 2, 1},
        {',', 6, 1, 0},
        {'<', 6, 1, 1},
        {'.', 6, 0, 0},
        {'>', 6, 0, 1},

        // col7
        {'?', 7, 1, 0},
        //{ '?', 6, 1, 1 },
        {'/', 7, 0, 0},
        //{ '/', 6, 0, 1 },

        {0, 0, 0, 0},
    };

    int i = 0;
    while (1)
    {
        if ((key[i].c == c) || (key[i].c == 0))
            break;
        i++;
    };

    if (!key[i].c)
        return -1;

    *ret = 0xff & ~(1 << key[i].row);
    *ret_shift = (key[i].shift) ? true : false;
    return key[i].col;
}

/**
 * @brief Složí hodnotu Port C 8255 ze signálů (společné pro read a peek).
 *
 * Bity: 0 = blokování CTC0_OUT (PC0), 1 = data do CMT (čte se 0),
 * 2 = zákaz přerušení z CTC (PC2), 3 = řízení motoru CMT (čte se 0),
 * 4 = stav motoru CMT, 5 = data z CMT, 6 = cursor timer, 7 = VBLNK.
 *
 * @param cmt_in_data Úroveň dat z pásky (bit 0); read ji předá po
 *        cmt_read_data(), peek z g_cmt.output bez dosynchronizování.
 * @return Hodnota Port C.
 *
 * Bez vedlejších efektů (jen čte signály). HWE BP_EVENT_CURSOR a
 * BP_EVENT_CMT_MSTATE se vyhodnocují u zdroje signálu (mz800_gdg_event.c,
 * cmt.c, pio8255_write), ne zde.
 */
static inline uint8_t pio8255_portc_compose ( unsigned cmt_in_data )
{
    uint8_t retval = 0x00;
    retval |= SIGNAL_GDG_VBLNK ? 1 << 7 : 0;
    retval |= mz800_main_get_cursor_timer_state() << 6;
    retval |= ( cmt_in_data & 1 ) << 5;
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    retval |= pio8255_cmt_mstate ( ) << 4;
#else
    if (CMTHACK_TEST_IS_INSTALLED)
    {
        retval |= g_pio8255.signal_pc04 << 4;
    }
    else
    {
        retval |= ((CMT_TEST_STOP) || (CMT_TEST_PAUSED)) ? 0 : (1 << 4);
    };
#endif
    // retval |= SIGNAL_GDG_VBLNK << 4; /* simulujeme promenlivy stav motoru (jinak neni mozne spustit hru "24" - musel by se manualne menit stav motoru ) */

    retval |= g_pio8255.signal_pc02 << 2;
    retval |= g_pio8255.signal_pc00;
    return retval;
}


/**
 * @brief Čtení z 8255 (CPU cesta, IN 0D0h-0D3h / čtení 0E000h-0E003h).
 *
 * @param addr Port 8255: DEF_PIO8255_PORTA (výstupní latch PA; pro
 *             PA0-3 > 9 vrací 0xFF), DEF_PIO8255_PORTB (klávesnice),
 *             DEF_PIO8255_PORTC (VBLNK, kurzor, CMT, PC0-3).
 * @return Hodnota portu. Port B: řádky vybraného sloupce, 0 = stisknuto
 *         (`keyboard_matrix[col] & vkbd_matrix[col]`, případně autotype);
 *         pro PA0-3 = 10-15 0xFF (74LS145 nevybere žádný sloupec, ověřeno
 *         na HW MZ-800). Nepodporovaná adresa (control port): 0x00.
 *
 * Vedlejší efekty: čtení Port B zpracuje čekající události klávesnice
 * hostitele, posouvá autotype a vyhodnocuje vkbd probe (viz
 * st_PIO8255_VKBD_PROBE).
 *
 * Volá se z emulačního vlákna.
 */
uint8_t pio8255_read(int addr)
{

    DBGPRINTF(DBGINF, "addr = 0x%02x, PC = 0x%04x\n", addr, g_mzarch_main.instruction_addr);

    switch (addr)
    {

    case DEF_PIO8255_PORTA:
        if (g_pio8255.signal_PA_keybord_column < 10)
            return g_pio8255.signal_PA;
        return 0xff;
        break;

    case DEF_PIO8255_PORTB:

        if (g_pio8255.vkbd_autotype)
        {
            if (g_pio8255.vkbd_autotype_col == -1)
            {
                while (g_pio8255.vkbd_autotype[0] != 0x00)
                {
                    g_pio8255.vkbd_autotype_col = pio8255_autotype_get_matrix(g_pio8255.vkbd_autotype[0], &g_pio8255.vkbd_autotype_ret, &g_pio8255.vkbd_autotype_ret_shift);
                    g_pio8255.vkbd_autotype++;
                    if (g_pio8255.vkbd_autotype_col != -1)
                        break;
                };
                g_pio8255.vkbd_autotype_start_ticks = gdg_get_total_ticks();
                g_pio8255.vkbd_autotype_keydown = (g_pio8255.vkbd_autotype_ret_shift) ? 0 : 1;
                /* Průběh pro GUI (pio8255_autotype_progress()). */
                g_atomic_int_set(&s_autotype_typed, (gint)(g_pio8255.vkbd_autotype - s_autotype_start));
                /* Konec textu: autotype skončil. Ukončí ho emu vlákno (dřív to
                 * dělalo jen otevřené okno autotype z UI vlákna, se zavřeným
                 * oknem zůstal "běžící" napořád). */
                if ((g_pio8255.vkbd_autotype_col == -1) && (g_pio8255.vkbd_autotype[0] == 0x00))
                {
                    pio8255_set_autotype(NULL);
                };
            };

            if ((g_pio8255.vkbd_autotype != NULL) && (g_pio8255.vkbd_autotype_col != -1))
            {

                uint8_t ret = 0xff;

                if (g_pio8255.vkbd_autotype_keydown == 0)
                {
                    if ((g_pio8255.vkbd_autotype_ret_shift) && (g_pio8255.signal_PA_keybord_column == 8))
                    {
                        ret = 0xfe;
                    };
                }
                else if (g_pio8255.vkbd_autotype_keydown == 1)
                {
                    if ((g_pio8255.vkbd_autotype_ret_shift) && (g_pio8255.signal_PA_keybord_column == 8))
                    {
                        ret = 0xfe;
                    }
                    else if (g_pio8255.signal_PA_keybord_column == (unsigned)g_pio8255.vkbd_autotype_col)
                    {
                        ret = g_pio8255.vkbd_autotype_ret;
                    };
                };

                uint64_t now_ticks = gdg_get_total_ticks();

                if (g_pio8255.vkbd_autotype_keydown <= 1)
                {
                    if (now_ticks >= (g_pio8255.vkbd_autotype_start_ticks + g_pio8255.vkbd_autotype_kd_ticks))
                    {
                        g_pio8255.vkbd_autotype_keydown++;
                        g_pio8255.vkbd_autotype_start_ticks = now_ticks;
                    };
                }
                else
                {
                    if (now_ticks >= (g_pio8255.vkbd_autotype_start_ticks + g_pio8255.vkbd_autotype_ku_ticks))
                    {
                        g_pio8255.vkbd_autotype_col = -1;
                    };
                };

                return ret;
            };
        };

        iface_keyboard_pool_keyboard_events();

        /* PA0-3 = 10-15: 74LS145 nevybere žádný sloupec, řádky drží
         * pull-up -> 0xFF. Ověřeno na HW MZ-800 (KBDCOL, 2026-10-08).
         * Index 10 je mimo keyboard_matrix[] i vkbd_matrix[]. */
        if (g_pio8255.signal_PA_keybord_column >= 10)
        {
            return 0xff;
        };

        // g_pio8255.keyboard_matrix [ 2 ] &= 0xdf;
        uint8_t retval = g_pio8255.keyboard_matrix[g_pio8255.signal_PA_keybord_column] & g_pio8255.vkbd_matrix[g_pio8255.signal_PA_keybord_column];

        /* Probe hook (fix 0016): pokud MCP/HID vrstva čeká na ověření
         * dosednutí vstříknuté klávesy a guest právě skenuje cílový
         * sloupec, zaznamenej to. Celý hook je za jediným testem na
         * vkbd_probe.active, takže když probe není ozbrojen (= běžný
         * provoz), hot path se nemění (drží feedback_emu_perf_dispatch). */
        if (g_pio8255.vkbd_probe.active)
        {
            if (g_pio8255.signal_PA_keybord_column == (unsigned)g_pio8255.vkbd_probe.col)
            {
                g_pio8255.vkbd_probe.seen = true;
            }
        }

        DBGPRINTF(DBGINF, "addr = 0x%02x, keyboard_matrix[%d] = 0x%02x, PC = 0x%04x\n", addr, g_pio8255.signal_PA_keybord_column, retval, g_mzarch_main.instruction_addr);
        //printf("%s():%d - addr: %d, keyboard_matrix[%d] = 0x%02x, PC = 0x%04x\n", __FUNCTION__, __LINE__, addr, g_pio8255.signal_PA_keybord_column, retval, g_mzarch_main.instruction_addr);

        // // !!! DEBUG: Stisk "3" pro Batman Demo
        // static int first = 1;
        // if (first)
        // {
        //     first = 0;
        //     retval = 0xdf;
        // };

        return retval;

        /* TODO: prozatim mame jen ty nejpodstatnejsi bity */
    case DEF_PIO8255_PORTC:
        /*
         * 0. blokovani CTC0_OUT
         * 1. data do CMT
         * 2. zakaz preruseni z CTC
         * 3. rizeni motoru CMT 0 - 1 - 0
         * 4. test stavu motoru CMT
         * 5. data z CMT
         * 6. cursor timer
         * 7. VBLNK
         *
         */

        {
            /* cmt_read_data() dosynchronizuje pásku (cmt_update_output, může
             * přejít na další blok nebo do STOP) - volá se před složením
             * bitu 4 (stav motoru), stejně jako dřív.
             * V1.5 fáze 2.2 + 2.3: HWE BP_EVENT_CMT_IN fire přesunut
             * do cmt.c cmt_update_output (= edge-based, fire jen na
             * skutečnou změnu vstupního signálu z pásky; legacy
             * lifecycle fire v cmt_play/cmt_eject odstraněn). */
            unsigned cmt_in_data = cmt_read_data() & 1;
            retval = pio8255_portc_compose ( cmt_in_data );
        }

        // DBGPRINTF ( "PIO8255_PORTC read - retval = 0x%02x\n", retval );
        // printf ( "PIO8255_PORTC read - retval = 0x%02x, PC = 0x%04x\n", retval & 0x10, g_mzarch_main.cpu->pc );

        // printf ( "8255 PortB_5: %d, %lu\n", ( retval >> 5 ) & 1, gdg_get_total_ticks ( ) );

        return retval;
        break;

    default:
        DBGPRINTF(DBGWAR, "addr = %d - unsupported addr\n", addr);
        break;
    };
    return 0x00;
}


/**
 * @brief Autotype hodnota Port B bez posunu autotype (pro pio8255_peek).
 *
 * Zopakuje větev autotype z pio8255_read() nad lokálními kopiemi stavu:
 * když čeká na další znak (vkbd_autotype_col == -1), najde ho jako read,
 * ale ukazatel, sloupec, fázi stisku ani čas neuloží. Časový posun fáze
 * (keydown -> keyup) read dělá až po výpočtu hodnoty, hodnotu tedy
 * neovlivní.
 *
 * @param[out] ret Hodnota Port B, kterou by read vrátil (platí při true).
 * @return true = hodnotu určuje autotype, false = read by pokračoval
 *         klávesnicovou maticí (autotype neaktivní nebo text skončil).
 */
static bool pio8255_autotype_peek ( uint8_t *ret )
{
    if (!g_pio8255.vkbd_autotype) return false;

    int col = g_pio8255.vkbd_autotype_col;
    uint8_t key_ret = g_pio8255.vkbd_autotype_ret;
    bool key_shift = g_pio8255.vkbd_autotype_ret_shift;
    int keydown = g_pio8255.vkbd_autotype_keydown;

    if (col == -1)
    {
        const char *p = g_pio8255.vkbd_autotype;
        while (p[0] != 0x00)
        {
            col = pio8255_autotype_get_matrix(p[0], &key_ret, &key_shift);
            p++;
            if (col != -1) break;
        };
        keydown = (key_shift) ? 0 : 1;
    };

    if (col == -1) return false;

    uint8_t value = 0xff;
    if (keydown == 0)
    {
        if ((key_shift) && (g_pio8255.signal_PA_keybord_column == 8)) value = 0xfe;
    }
    else if (keydown == 1)
    {
        if ((key_shift) && (g_pio8255.signal_PA_keybord_column == 8))
        {
            value = 0xfe;
        }
        else if (g_pio8255.signal_PA_keybord_column == (unsigned)col)
        {
            value = key_ret;
        };
    };
    *ret = value;
    return true;
}


uint8_t pio8255_peek(int addr)
{
    switch (addr)
    {
    case DEF_PIO8255_PORTA:
        if (g_pio8255.signal_PA_keybord_column < 10)
            return g_pio8255.signal_PA;
        return 0xff;

    case DEF_PIO8255_PORTB:
    {
        uint8_t ret;
        if (pio8255_autotype_peek(&ret)) return ret;
        /* Bez iface_keyboard_pool_keyboard_events() a bez vkbd probe:
         * matice z posledního zpracování událostí hostitele. */
        if (g_pio8255.signal_PA_keybord_column >= 10) return 0xff;
        return g_pio8255.keyboard_matrix[g_pio8255.signal_PA_keybord_column] & g_pio8255.vkbd_matrix[g_pio8255.signal_PA_keybord_column];
    }

    case DEF_PIO8255_PORTC:
        /* Data z pásky z poslední synchronizace (bez cmt_update_output). */
        return pio8255_portc_compose ( (unsigned) g_cmt.output & 1 );

    default:
        break;
    };
    return 0x00;
}

void pio8255_pc2_set(int value)
{
    g_pio8255.signal_pc02 = value & 1;
}

int pio8255_pc1_get(void)
{
    return g_pio8255.signal_pc01;
}

int pio8255_pc2_get(void)
{
    return g_pio8255.signal_pc02;
}

int pio8255_pc4_get(void)
{
    if (CMTHACK_TEST_IS_INSTALLED)
    {
        return g_pio8255.signal_pc04;
    };
    return ((CMT_TEST_STOP) || (CMT_TEST_PAUSED)) ? 0 : 1;
}

int pio8255_pa4_get(void)
{
    return g_pio8255.signal_PA_joy1_enabled;
}

int pio8255_pa5_get(void)
{
    return g_pio8255.signal_PA_joy2_enabled;
}

int pio8255_pa0_3_get(void)
{
    return g_pio8255.signal_PA_keybord_column;
}
