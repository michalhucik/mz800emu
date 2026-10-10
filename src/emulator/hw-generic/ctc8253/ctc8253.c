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

#include "mzarch/mzarch_config.h"

#include "ctc8253.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"
#include "gdg/video.h"

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

/* LOW pulz HSYNC nesmí přecházet přes konec řádku (test v ctc8253_clk_is_high) */
_Static_assert((VIDEO_REAL_HSYNC_START_COLUMN + VIDEO_H_SYNC_TICKS) <= VIDEO_SCREEN_WIDTH, "HSYNC pulse wraps over row end");

/**
 * @brief Zjistí úroveň vstupu CLK čítače v okamžiku zápisu z CPU.
 *
 * Potřebuje ji pravidlo nahrání hodnoty (viz st_CTC8253.load_wait_rise):
 * hodnota dopsaná při CLK = HIGH se nahraje až druhou sestupnou hranou,
 * dopsaná při CLK = LOW první.
 *
 * - CLK2 = OUT1 (kaskáda, bez inverze - změřeno: CTC2 se mění na začátku LOW
 *   pulzu OUT1).
 * - CLK1 = HSYNC: LOW od sestupné hrany (sloupec VIDEO_REAL_HSYNC_START_COLUMN,
 *   kde se volá ctc8253_clkfall(CTC_CS1)) po dobu VIDEO_H_SYNC_TICKS, jinak
 *   HIGH. Délka pulzu podle video.h / báze 08a-video-timing.md, na HW
 *   neměřeno vůči CTC [neověřeno].
 * - CLK0 = 1M1 (pxCLK / GDGCLK_CTC0_DIVIDER): sestupná hrana tam, kde je
 *   total_ticks % GDGCLK_CTC0_DIVIDER == 0, střída 50 % (hypotéza; u děliče
 *   /13 v MZ-700 NTSC tím spíš neověřeno).
 *
 * @param cs Index čítače (CTC_CS0..CTC_CS2).
 * @param ticks Okamžik zápisu v tikách snímku (gdg_get_insigeop_ticks()).
 * @return 1 = CLK je HIGH, 0 = CLK je LOW.
 * @note Bez vedlejších efektů.
 */
static unsigned ctc8253_clk_is_high(unsigned cs, unsigned ticks)
{
    switch (cs)
    {
    case CTC_CS2:
        return g_ctc8253[CTC_CS1].out;

    case CTC_CS1:
    {
        unsigned col = VIDEO_GET_SCREEN_COL(ticks);
        if ((col >= VIDEO_REAL_HSYNC_START_COLUMN) && (col < (VIDEO_REAL_HSYNC_START_COLUMN + VIDEO_H_SYNC_TICKS)))
        {
            return 0;
        };
        return 1;
    }

    default:
    {
        unsigned phase = (unsigned)(gdg_compute_total_ticks(ticks) % GDGCLK_CTC0_DIVIDER);
        return (phase >= (GDGCLK_CTC0_DIVIDER / 2)) ? 1 : 0;
    }
    };
}

/**
 * @brief Velikost kroku čítače v režimu 3 pro nejbližší sestupnou hranu CLK.
 *
 * Podle datasheetu 8254 a měření na HW MZ-800 (CTCLOAD 3: N = 4 čte
 * 04 02 04 02, N = 5 čte 05 04 02 05 02 05 04 02): čítač se snižuje po 2;
 * při lichém N je první krok po nahrání -1 při OUT = 1 a -3 při OUT = 0.
 * Fáze OUT = 1 tak trvá (N + 1) / 2 CLK, fáze OUT = 0 (N - 1) / 2 CLK.
 *
 * @param ctc Čítač v režimu 3 ve stavu CTC_STATE_COUNTDOWN.
 * @return Krok 1, 2 nebo 3.
 * @note "Hned po nahrání" se pozná podle value == preset_value: při lichém
 *       presetu nabývá čítač mezi nahráními jen sudých hodnot. Po přepisu
 *       hodnoty uprostřed půlperiody (nový preset_value) může tento test
 *       výjimečně trefit náhodnou shodu (neměřeno).
 */
static inline unsigned ctc8253_mode3_step(const st_CTC8253 *ctc)
{
    if ((ctc->value == ctc->preset_value) && (ctc->preset_value & 1))
    {
        return (ctc->out == 1) ? 1 : 3;
    };
    return 2;
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST

/**
 * @brief Počet sestupných hran CLK do konce půlperiody v režimu 3.
 *
 * @param ctc Čítač v režimu 3 ve stavu CTC_STATE_COUNTDOWN.
 * @return Počet hran (>= 1); poslední z nich přepne OUT a nahraje preset.
 */
static inline unsigned ctc8253_mode3_clocks_to_tc(const st_CTC8253 *ctc)
{
    unsigned step = ctc8253_mode3_step(ctc);
    if (ctc->value <= step)
    {
        return 1;
    };
    return 1 + ((ctc->value - step) + 1) / 2;
}

/**
 * @brief Posune čítač v režimu 3 o @p clocks hran CLK, bez dosažení konce půlperiody.
 *
 * Pro rychlou cestu CTC0, která čítač dopočítává z uplynulých tiků.
 *
 * @param ctc Čítač v režimu 3 ve stavu CTC_STATE_COUNTDOWN.
 * @param clocks Počet hran; musí být menší než ctc8253_mode3_clocks_to_tc().
 * @post value je hodnota po @p clocks hranách (>= 2). Kdyby volající
 *       předal víc hran, hodnota se zastaví na 2 (poslední krok před koncem
 *       půlperiody) místo podtečení.
 */
static inline void ctc8253_mode3_advance(st_CTC8253 *ctc, unsigned clocks)
{
    if (clocks == 0)
        return;
    unsigned step = ctc8253_mode3_step(ctc);
    unsigned total = step + 2 * (clocks - 1);
    ctc->value = (ctc->value > total + 1) ? ctc->value - total : 2;
}

#endif

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
        g_ctc8253[cs].load_wait_rise = 0;
        g_ctc8253[cs].mode = CTC_MODE0;
        g_ctc8253[cs].out = 0;
        g_ctc8253[cs].value = 0;
        g_ctc8253[cs].preset_value = 0xffff;
        g_ctc8253[cs].rl_byte = 0;
        g_ctc8253[cs].latch_op = 0;
        g_ctc8253[cs].rlf = CTC_RLF_LSBMSB;
        g_ctc8253[cs].bcd = 0;
    };

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    g_ctc8253[CTC_CS0].clk1m1_event.ticks = -1;
    g_ctc8253[CTC_CS0].clk1m1_event.event_name = MZEVENT_CTC0;
#endif
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST

/**
 * @brief Dopočítá hodnotu CTC0 z tiků uplynulých od posledního zpracování (rychlá cesta).
 *
 * @param event_total_ticks Okamžik (celkové tiky), ke kterému se čítač dopočítá.
 * @pre CTC0 je ve stavu >= CTC_STATE_COUNTDOWN a má naplánovaný event.
 * @post value odpovídá počtu celých period CLK0 od clk1m1_last_event_total_ticks;
 *       ten se posune o zpracované periody. V režimu 3 čítá po 2
 *       (ctc8253_mode3_advance()), v BLIND_COUNT se drží v 16 bitech.
 */
static inline void ctc8253_update_ctc0_by_totalticks(unsigned event_total_ticks)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    unsigned elapsed_ticks = event_total_ticks - ctc0->clk1m1_last_event_total_ticks;
    unsigned decremented = elapsed_ticks / GDGCLK_CTC0_DIVIDER;
    if ((ctc0->mode == CTC_MODE3) && (ctc0->state == CTC_STATE_COUNTDOWN))
    {
        ctc8253_mode3_advance(ctc0, decremented);
    }
    else
    {
        ctc0->value -= decremented;
        if (ctc0->state == CTC_STATE_BLIND_COUNT)
        {
            ctc0->value &= 0xffff;
        };
    };
    ctc0->clk1m1_last_event_total_ticks += decremented * GDGCLK_CTC0_DIVIDER;
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

#endif

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

#ifdef MZ800EMU_CFG_CLK1M1_FAST
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
#endif

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

/**
 * @brief Zápis CPU do 8253: řídicí slovo (CW), Counter Latch nebo bajt hodnoty čítače.
 *
 * - CW: nastaví režim, RL formát a BCD, stav CTC_STATE_INIT a startovní
 *   úroveň OUT (režim 0: 0, ostatní: 1). Čítač pak čeká na hodnotu.
 * - Counter Latch (RL = 00): zachytí value do read_latch.
 * - Hodnota: po posledním bajtu (podle RL formátu) přejde čítač ze stavů
 *   < CTC_STATE_LOAD_DONE do LOAD_DONE a load_wait_rise se nastaví podle
 *   úrovně CLK v okamžiku zápisu (ctc8253_clk_is_high()). Režim 0 při
 *   přepisu během čítání shodí OUT hned prvním bajtem. Režimy 2 a 3 během
 *   čítání jen uloží nový preset, použije se při dalším nahrání.
 *
 * @param addr Adresa v rámci 8253 (spodní 2 bity: CTCADDR_CTC0..CTCADDR_CWREG).
 * @param value Zapisovaný bajt.
 * @note Vedlejší efekty: změna OUT (callback), u CTC0 v rychlé cestě
 *       přeplánování eventu MZEVENT_CTC0, záznam do hwlog (debugger).
 * @pre Volat z emu vlákna (CPU IORQ / MMIO).
 */
void ctc8253_write_byte(unsigned addr, uint8_t value)
{

    en_CTC_CS cs;

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    en_CTC_STATE old_state;
#endif

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

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        if ((cs == CTC_CS0) && (g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };

        old_state = g_ctc8253[cs].state;
#endif
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

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        if ((cs == CTC_CS0) && (g_ctc8253[CTC_CS0].state >= CTC_STATE_COUNTDOWN) && ((int)g_ctc8253[CTC_CS0].clk1m1_event.ticks != -1))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };

        old_state = g_ctc8253[cs].state;
#endif

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
            /* MSM82C53-2: N = 1 v rezimu 3 = perioda 65537 CLK (baze hw/06-ctc-8253.md) */
            if (g_ctc8253[cs].preset_value == 1)
            {
                g_ctc8253[cs].preset_value = 0x10001;
            };
        };

        /* Dokoncen LOAD: hodnota se nahraje na sestupne hrane CLK, pred kterou
         * po zapisu probehla vzestupna hrana (HW MZ-800, CTCLOAD 1-3); na tom,
         * kdy prislo CW, nezalezi. Zapis pri CLK = HIGH -> nejblizsi sestupnou
         * hranu preskocime. */

        if (g_ctc8253[cs].state < CTC_STATE_LOAD_DONE)
        {
            g_ctc8253[cs].state = CTC_STATE_LOAD_DONE;
            g_ctc8253[cs].load_done = 0;
            g_ctc8253[cs].load_wait_rise = ctc8253_clk_is_high(cs, gdg_get_insigeop_ticks());
        }
        else if (g_ctc8253[cs].state == CTC_STATE_MODE1_TRIGGER_ERROR)
        {
            g_ctc8253[cs].state = CTC_STATE_PRESET32;
        };
    };

#ifdef MZ800EMU_CFG_CLK1M1_FAST
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
#endif
}

/**
 * @brief Zpracuje sestupnou hranu vstupu CLK čítače.
 *
 * Chování podle režimu (0-3; 4 a 5 nejsou implementované). Společné:
 * ve stavu CTC_STATE_LOAD_DONE s load_wait_rise = 1 hrana hodnotu nenahraje,
 * jen vynuluje load_wait_rise (hodnota byla dopsána při CLK = HIGH, nahraje
 * ji až další sestupná hrana).
 *
 * - Režim 0: nahrání presetu, pak -1 za hranu; při dosažení 0 OUT = 1 a čítač
 *   ukazuje 0000h po celou periodu CLK, 0FFFFh až po další hraně (HW,
 *   CTCLOAD 1-3), dál čítá dolů v 16 bitech.
 * - Režim 2: preset .. 1, při 1 OUT = 0 na jednu periodu, pak nové nahrání.
 * - Režim 3: po 2 (ctc8253_mode3_step()); při dosažení 0 se přepne OUT
 *   a nahraje preset.
 *
 * @param cs Index čítače (CTC_CS0..CTC_CS2).
 * @param event_ticks Okamžik hrany v tikách snímku (pro callback výstupu).
 * @note Vedlejší efekty: změna OUT volá output_cb (CTC1 -> takt CTC2,
 *       CTC2 -> přerušení, CTC0 -> zvuk a PIO).
 * @pre Volat z emu vlákna.
 */
void ctc8253_clkfall(unsigned cs, unsigned event_ticks)
{

    if ((g_ctc8253[cs].state == CTC_STATE_LOAD_DONE) && (g_ctc8253[cs].load_wait_rise))
    {
        g_ctc8253[cs].load_wait_rise = 0;
        return;
    };

    switch (g_ctc8253[cs].mode)
    {

    case CTC_MODE0:
        if (g_ctc8253[cs].state == CTC_STATE_BLIND_COUNT)
        {
            g_ctc8253[cs].value = (g_ctc8253[cs].value - 1) & 0xffff;
            return;
        }
        else if (g_ctc8253[cs].state >= CTC_STATE_COUNTDOWN)
        {
            g_ctc8253[cs].value--;
            if (g_ctc8253[cs].value == 0x0000)
            {
                ctc8253_set_out(cs, 1, event_ticks);
                g_ctc8253[cs].state = CTC_STATE_BLIND_COUNT;
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
            unsigned step = ctc8253_mode3_step(&g_ctc8253[cs]);

            if (g_ctc8253[cs].value > step)
            {
                g_ctc8253[cs].value -= step;
            }
            else
            {
                /* konec pulperiody: prepnout OUT a nahrat preset */
                g_ctc8253[cs].value = g_ctc8253[cs].preset_value;

                if (g_ctc8253[cs].out == 1)
                {
                    ctc8253_set_out(cs, 0, event_ticks);
                }
                else
                {

                    ctc8253_set_out(cs, 1, event_ticks);

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

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    en_CTC_STATE old_state = g_ctc8253[cs].state;
#endif

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

#ifdef MZ800EMU_CFG_CLK1M1_FAST
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
#endif
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST

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
            /* dopocitat hrany pred touto (posledni zpracuje ctc8253_clkfall()) */
            elapsed_ticks -= GDGCLK_CTC0_DIVIDER;
            if ((ctc0->mode == CTC_MODE3) && (ctc0->state == CTC_STATE_COUNTDOWN))
            {
                ctc8253_mode3_advance(ctc0, elapsed_ticks / GDGCLK_CTC0_DIVIDER);
            }
            else
            {
                ctc0->value -= elapsed_ticks / GDGCLK_CTC0_DIVIDER;
            };
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
                /* posledni krok pred koncem pulperiody */
                if (ctc0->value != ctc0->preset_value)
                {
                    ctc0->value = 2;
                };
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
        /* M3 - posledni krok pulperiody (viz ctc8253_mode3_clocks_to_tc()) */

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
            destination_clk1m1_falls = ctc8253_mode3_clocks_to_tc(ctc0);
            break;

        case CTC_MODE4:
        case CTC_MODE5:
            // DBGPRINTF ( DBGWARN, "Unsupported mode: %d, on CTC: %d\n", g_ctc8253[cs].mode, cs );
            destination_clk1m1_falls = -1;
            break;
        };

        if ((int)destination_clk1m1_falls != -1)
        {
            ctc0->clk1m1_event.ticks = event_ticks + (destination_clk1m1_falls * GDGCLK_CTC0_DIVIDER);
        }
        else
        {
            ctc0->clk1m1_event.ticks = -1;
        }
    }
    else if ((old_state == CTC_STATE_COUNTDOWN) && (ctc0->state == CTC_STATE_PRESET))
    {
        /* M2 - skoncil COUNTDOWN */
        ctc0->clk1m1_event.ticks = event_ticks + (1 * GDGCLK_CTC0_DIVIDER);
    }
    else
    {
        ctc0->clk1m1_event.ticks = -1;
    };
}

#endif
