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

/**
 * @brief Čítá CTC0 v režimu 3 (nutná podmínka stavu "dělič 2" zvukového kanálu)?
 *
 * @return 1 = režim 3, stav CTC_STATE_COUNTDOWN a GATE0 = 1.
 * @note Sama o děliči nic neříká; jaký dělič čítač právě odpočítává, je vidět
 *       jen na hraně nahrání (viz ctc8253_ctc0_output_event()).
 */
static inline int ctc8253_ctc0_mode3_counting(void)
{
    const st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    return (ctc0->mode == CTC_MODE3) && (ctc0->state == CTC_STATE_COUNTDOWN) && (ctc0->gate == 1);
}

/**
 * @brief Změna OUT0: vstup PA4 Z80 PIO a zvukový kanál CTC0.
 *
 * Režim 3 s děličem 2 (obdélník 554 kHz; hra Ralye, Tatra-sys HD CP/M disk 5,
 * ho nastaví a povolí reproduktor bitem PC0) se do zvuku po hranách neposílá.
 * Výstup zvukového čipu takový obdélník nestíhá a zbyde z něj stálá úroveň
 * (změřeno na MZ-800, viz AUDIO_CTC0_DIV2_FACTOR): první hrana s děličem 2
 * ohlásí audio_ctc0_div2_changed(), další hrany už zvuk nevolají. Stálá úroveň
 * je neslyšitelná (odezní střídavou vazbou), klíčování bitem PC0 dává čistý tón.
 *
 * Dělič platí od nahrání do čítače, ne od zápisu: v režimu 3 mění OUT0 jen
 * hrana, která zároveň nahrála preset, takže st_CTC8253.value je tu právě
 * nahraný dělič. Hrana s jiným děličem, zvednutí OUT0 bránou (GATE0 = 0) nebo
 * řídicím slovem (stav CTC_STATE_INIT) vrací kanál ke skutečnému OUT0. Když
 * se při tom OUT0 nezmění (byl v jedničce), callback nepřijde a stav srovná
 * ctc8253_ctc0_audio_update() volaná z ctc8253_gate() a ctc8253_write_byte().
 *
 * @param value Nová úroveň OUT0 (0/1).
 * @param event_ticks Okamžik hrany v tikách snímku.
 * @note Volá se na každé hraně OUT0 (horká cesta). Běžný tón vyhodnotí stejně
 *       jako dřív jednu podmínku (režim 3 a hodnota 2); zbylé testy se čtou
 *       jen s děličem 2.
 */
static inline void ctc8253_ctc0_output_event(unsigned value, unsigned event_ticks)
{
    //    DBGPRINTF ( DBGINF, "CTC0 output event! (%d) - ticks: %d\n", value, event_ticks );
    //    DBGPRINTF ( DBGINF, "CTC0 output event! (%d) - total: %d\n", value, gdg_compute_total_ticks ( event_ticks ) );
    // DBGPRINTF ( DBGINF, "CTC0 output event! (%d)\n", value );

    pioz80_port_id_event(PIOZ80_PORT_A, PIOZ80_PORT_EVENT_PA4_CTC0, ~value & 0x01);

    const st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];

    if (!((ctc0->mode == CTC_MODE3) && (ctc0->value == 2) && (ctc0->state == CTC_STATE_COUNTDOWN) && (ctc0->gate == 1)))
    {
        audio_ctc0_changed((value & CTC_AUDIO_MASK), gdg_compute_total_ticks(event_ticks));
    }
    else if (!g_audio.ctc0_div2)
    {
        /* první hrana s děličem 2: dál jde do zvuku stálá úroveň maskovaná PC0 */
        audio_ctc0_div2_changed(CTC_AUDIO_MASK, gdg_compute_total_ticks(event_ticks));
    };
}

/* Doxygen viz ctc8253.h. */
void ctc8253_ctc0_audio_update(unsigned event_ticks)
{
    uint64_t total_ticks = gdg_compute_total_ticks(event_ticks);

    if (g_audio.ctc0_div2 && ctc8253_ctc0_mode3_counting())
    {
        audio_ctc0_div2_changed(CTC_AUDIO_MASK, total_ticks);
    }
    else
    {
        audio_ctc0_changed((CTC8253_OUT(0) & CTC_AUDIO_MASK), total_ticks);
    };
}

/* Doxygen viz ctc8253.h. */
void ctc8253_ctc0_audio_restore(void)
{
    const st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    bool div2 = ctc8253_ctc0_mode3_counting() && (ctc0->preset_value == 2);
    audio_ctc0_restore(div2 ? (CTC_AUDIO_MASK != 0) : ((ctc0->out & CTC_AUDIO_MASK) != 0), div2);
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

/* LOW pulz HSYNC nesmí přecházet přes konec řádku (test v ctc8253_load_skips_edge) */
_Static_assert((VIDEO_REAL_HSYNC_START_COLUMN + VIDEO_H_SYNC_TICKS) <= VIDEO_SCREEN_WIDTH, "HSYNC pulse wraps over row end");

/**
 * Poloha náběžné hrany /WR před koncem M-cyklu zápisu, v půltikách pixel
 * clocku: 0,5 T = 2,5 tiku (časování Z80; emulátor zápis razítkuje koncem
 * M-cyklu). Čip 8253 jedná na náběžné hraně /WR (změřeno na MZ-800,
 * emu-experiments/ctc-meas).
 */
#define CTC8253_WR_EDGE_HALFTICKS 5

/**
 * Předstih zápisu děliče před náběžnou hranou CLK, v půltikách pixel clocku.
 * Změřeno na MZ-800 (ctc-meas, 2026-11-15): zápis do 180 ns po sestupné
 * hraně CLK0 se nahraje první sestupnou hranou, od 225 ns až druhou;
 * náběžná hrana CLK0 je 455 ns po sestupné. Předstih je tedy mezi 230
 * a 275 ns; 9 půltiků = 254 ns.
 */
#define CTC8253_LOAD_SETUP_HALFTICKS 9

/**
 * Zpoždění účinku změny GATE0 v ticích pixel clocku od konce M-cyklu zápisu
 * na 0E008h: novou úroveň vidí sestupné hrany CLK0 pozdější než zápis + toto
 * zpoždění. Čip vzorkuje GATE náběžnou hranou CLK: změna na pinu do 7/16
 * periody po sestupné hraně se projeví první sestupnou hranou, pozdější až
 * druhou (ctc-meas, 2026-11-15; pin GATE0 se mění asi 13 ns po náběžné hraně
 * /WR). V jednotkách konce M-cyklu: fáze 3 až 10 tiků první hrana, jinak
 * druhá. Pro dělič 16 vychází 5 tiků; pro dělič 13 (MZ-700 NTSC) je hodnota
 * odvozená stejným vzorcem a neměřená [neověřeno].
 */
#define CTC8253_CTC0_GATE_DELAY (GDGCLK_CTC0_DIVIDER - ((GDGCLK_CTC0_DIVIDER + 4) / 2 + 1))

/**
 * @brief Rozhodne, zda zápis mine nejbližší náběžnou hranu CLK.
 *
 * @param p Tiky od poslední sestupné hrany CLK do konce M-cyklu zápisu.
 * @param rise2 Dvojnásobek vzdálenosti náběžné hrany CLK od sestupné, v ticích.
 * @return 1 = nejbližší sestupná hrana po zápisu hodnotu nenahraje, 0 = nahraje.
 * @note Zápis, jehož náběžná hrana /WR padla ještě před poslední sestupnou
 *       hranu CLK (2 * p < CTC8253_WR_EDGE_HALFTICKS), má celou nízkou úroveň
 *       před sebou a náběžnou hranu stihne.
 */
static inline unsigned ctc8253_misses_rising_edge(unsigned p, unsigned rise2)
{
    if ((2 * p) < CTC8253_WR_EDGE_HALFTICKS)
        return 0;
    return (((2 * p) - CTC8253_WR_EDGE_HALFTICKS + CTC8253_LOAD_SETUP_HALFTICKS) > rise2) ? 1 : 0;
}

/**
 * @brief Zjistí, zda nejbližší sestupná hrana CLK po zápisu hodnotu ještě nenahraje.
 *
 * Pravidlo nahrání (viz st_CTC8253.load_wait_rise): hodnotu nahraje sestupná
 * hrana CLK, před kterou po zápisu proběhla náběžná hrana, a to s předstihem
 * CTC8253_LOAD_SETUP_HALFTICKS. Zápis při CLK = HIGH nebo těsně před
 * náběžnou hranou se proto nahraje až druhou sestupnou hranou.
 *
 * - CLK0 = 1M1 (pxCLK / GDGCLK_CTC0_DIVIDER): sestupná hrana tam, kde je
 *   total_ticks % GDGCLK_CTC0_DIVIDER == GDG_CTC0_CLK_PHASE_TICKS (na MZ-800
 *   0,0 až 0,2 px po sestupné hraně HSYNC, změřeno), náběžná v polovině
 *   periody (změřeno 8,1/16; u děliče 13 v MZ-700 NTSC neměřeno [neověřeno]).
 *   Změřeno: první hranu stihnou 4 fáze ze 16.
 * - CLK1 = HSYNC: LOW od sestupné hrany (sloupec VIDEO_REAL_HSYNC_START_COLUMN,
 *   kde se volá ctc8253_clkfall(CTC_CS1)) po dobu VIDEO_H_SYNC_TICKS
 *   (změřeno 4,51 us = 80 tiků na MZ-800). První hranu stihne jen zápis
 *   v prvních asi 4 us po sestupné hraně (změřeno).
 * - CLK2 = OUT1 (kaskáda, bez inverze): rozhoduje úroveň OUT1 v okamžiku
 *   zápisu. OUT1 se mění až po sestupné hraně HSYNC, takže zápis při
 *   OUT1 = LOW má do náběžné hrany vždy dost času.
 *
 * @param cs Index čítače (CTC_CS0..CTC_CS2).
 * @param ticks Okamžik zápisu v tikách snímku (gdg_get_insigeop_ticks()).
 * @return 1 = nejbližší sestupnou hranu přeskočit, 0 = nahrát na ní.
 * @note Bez vedlejších efektů.
 */
static unsigned ctc8253_load_skips_edge(unsigned cs, unsigned ticks)
{
    switch (cs)
    {
    case CTC_CS2:
        return g_ctc8253[CTC_CS1].out;

    case CTC_CS1:
    {
        /* tiky od poslední sestupné hrany HSYNC (na MZ-800 závisí poloha
         * hrany v řádku na bitu CKSW) */
        unsigned p = gdg_get_ticks_since_hsync_fall(ticks);
        return ctc8253_misses_rising_edge(p, 2 * VIDEO_H_SYNC_TICKS);
    }

    default:
    {
        unsigned p = (unsigned)((gdg_compute_total_ticks(ticks) + GDGCLK_CTC0_DIVIDER - GDG_CTC0_CLK_PHASE_TICKS) % GDGCLK_CTC0_DIVIDER);
        return ctc8253_misses_rising_edge(p, GDGCLK_CTC0_DIVIDER);
    }
    };
}

/**
 * @brief Převede hodnotu čítače v kódu BCD (až 5 dekád) na počet.
 *
 * @param v Hodnota tak, jak ji čte CPU (0x10000 = 10 000).
 * @return Počet. Neplatné číslice A až F se berou vahou 10 až 15
 *         (chování čipu s nimi není změřeno [neověřeno]).
 */
static inline unsigned ctc8253_bcd2bin(unsigned v)
{
    return (v & 0x0f) + ((v >> 4) & 0x0f) * 10 + ((v >> 8) & 0x0f) * 100 + ((v >> 12) & 0x0f) * 1000 + ((v >> 16) & 0x0f) * 10000;
}

/**
 * @brief Převede počet (0 až 19 999) na hodnotu čítače v kódu BCD.
 *
 * @param n Počet.
 * @return Hodnota v BCD; 10 000 dá 0x10000.
 */
static inline unsigned ctc8253_bin2bcd(unsigned n)
{
    return (n % 10) | (((n / 10) % 10) << 4) | (((n / 100) % 10) << 8) | (((n / 1000) % 10) << 12) | ((n / 10000) << 16);
}

/**
 * @brief Počet, který čítač právě drží (binárně i v BCD).
 *
 * @param ctc Čítač.
 * @return Počet zbývajících kroků podle st_CTC8253.value a bitu BCD.
 */
static inline unsigned ctc8253_count(const st_CTC8253 *ctc)
{
    return ctc->bcd ? ctc8253_bcd2bin(ctc->value) : ctc->value;
}

/**
 * @brief Uloží počet do st_CTC8253.value v kódování podle bitu BCD.
 *
 * @param ctc Čítač.
 * @param n Počet.
 */
static inline void ctc8253_set_count(st_CTC8253 *ctc, unsigned n)
{
    ctc->value = ctc->bcd ? ctc8253_bin2bcd(n) : n;
}

/**
 * @brief Sníží čítač o @p n kroků bez přetečení přes nulu.
 *
 * @param ctc Čítač.
 * @param n Počet kroků; je-li větší než stav čítače, čítač skončí na nule.
 */
static inline void ctc8253_dec(st_CTC8253 *ctc, unsigned n)
{
    unsigned cnt = ctc8253_count(ctc);
    ctc8253_set_count(ctc, (cnt > n) ? (cnt - n) : 0);
}

/**
 * @brief Sníží čítač o @p n kroků s přetečením (0 -> 0FFFFh, v BCD 0 -> 9999).
 *
 * Pro čítání bez cíle (CTC_STATE_BLIND_COUNT).
 *
 * @param ctc Čítač.
 * @param n Počet kroků.
 */
static inline void ctc8253_dec_wrap(st_CTC8253 *ctc, unsigned n)
{
    unsigned mod = ctc->bcd ? 10000 : 0x10000;
    unsigned cnt = ctc8253_count(ctc) % mod;
    ctc8253_set_count(ctc, (cnt + mod - (n % mod)) % mod);
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

/**
 * @brief Počet sestupných hran CLK do konce půlperiody v režimu 3.
 *
 * @param ctc Čítač v režimu 3 ve stavu CTC_STATE_COUNTDOWN.
 * @return Počet hran (>= 1); poslední z nich přepne OUT a nahraje preset.
 */
static inline unsigned ctc8253_mode3_clocks_to_tc(const st_CTC8253 *ctc)
{
    unsigned step = ctc8253_mode3_step(ctc);
    unsigned cnt = ctc8253_count(ctc);
    if (cnt <= step)
    {
        return 1;
    };
    return 1 + ((cnt - step) + 1) / 2;
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
    unsigned cnt = ctc8253_count(ctc);
    ctc8253_set_count(ctc, (cnt > total + 1) ? cnt - total : 2);
}

/**
 * @brief Je čítač ve stavu, kdy každá sestupná hrana CLK mění jeho hodnotu?
 *
 * @param ctc Čítač.
 * @return 1 pro CTC_STATE_COUNTDOWN, CTC_STATE_BLIND_COUNT a CTC_STATE_STROBE.
 */
static inline int ctc8253_is_counting(const st_CTC8253 *ctc)
{
    return (ctc->state == CTC_STATE_COUNTDOWN) || (ctc->state == CTC_STATE_BLIND_COUNT) || (ctc->state == CTC_STATE_STROBE);
}

void ctc8253_init(void)
{
    g_ctc8253_last_cw_byte = 0x00;

    /* Reset: stav "dělič 2" zvukového kanálu končí bez události ve zvukovém
     * logu (čas resetu tu není k dispozici a log při prvním volání ještě
     * neexistuje). Kanál se srovná při nejbližší změně OUT0 nebo PC0. */
    audio_ctc0_restore(g_audio.ctc0_output != 0, false);

    g_ctc8253[CTC_CS0].output_cb = ctc8253_ctc0_output_event;
    g_ctc8253[CTC_CS1].output_cb = ctc8253_ctc1_output_event;
    g_ctc8253[CTC_CS2].output_cb = ctc8253_ctc2_output_event;

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    /* před ctc8253_gate(): ta by jinak dohnala událost z předchozího běhu */
    g_ctc8253[CTC_CS0].clk1m1_event.ticks = -1;
    g_ctc8253[CTC_CS0].clk1m1_event.event_name = MZEVENT_CTC0;
#endif

    ctc8253_gate(CTC_CS0, 0, 0);
    ctc8253_gate(CTC_CS1, 1, 0);
    ctc8253_gate(CTC_CS2, 1, 0);

    unsigned cs;
    for (cs = CTC_CS0; cs <= CTC_CS2; cs++)
    {
        g_ctc8253[cs].state = CTC_STATE_INIT_DONE;
        g_ctc8253[cs].load_done = 0;
        g_ctc8253[cs].load_wait_rise = 0;
        g_ctc8253[cs].wr_half = 0;
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
#endif
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST

/*
 * Rychlá cesta CTC0: sestupné hrany CLK0 se nezpracovávají po jedné.
 *
 *  - clk1m1_last_event_total_ticks = čas poslední sestupné hrany, kterou
 *    st_CTC8253.value už zahrnuje,
 *  - clk1m1_event.ticks = nejbližší hrana, na které se děje víc než prostý
 *    krok čítače (nahrání, změna OUT, přeskočená hrana); -1 = žádná.
 *
 * Hrany mezi nimi dopočítá ctc8253_ctc0_advance() z uplynulého času.
 */

/**
 * @brief Posune CTC0 o @p n hran CLK0, na kterých se děje jen prostý krok čítače.
 *
 * @param ctc0 Čítač 0 ve stavu podle ctc8253_is_counting().
 * @param n Počet hran; nesmí zahrnout hranu naplánované události.
 */
static inline void ctc8253_ctc0_advance(st_CTC8253 *ctc0, unsigned n)
{
    if (n == 0)
        return;

    if (ctc0->state == CTC_STATE_BLIND_COUNT)
    {
        ctc8253_dec_wrap(ctc0, (ctc0->mode == CTC_MODE3) ? (2 * n) : n);
    }
    else if (ctc0->mode == CTC_MODE3)
    {
        ctc8253_mode3_advance(ctc0, n);
    }
    else
    {
        ctc8253_dec(ctc0, n);
    };
}

/**
 * @brief Dopočítá hodnotu CTC0 z tiků uplynulých od poslední zpracované hrany.
 *
 * @param event_total_ticks Okamžik (celkové tiky), ke kterému se čítač dopočítá.
 * @pre CTC0 je ve stavu podle ctc8253_is_counting().
 * @post value zahrnuje sestupné hrany CLK0 do @p event_total_ticks kromě hrany
 *       naplánované události a hran za ní (ty zpracuje ctc8253_ctc1m1_event());
 *       clk1m1_last_event_total_ticks se posune o zpracované hrany.
 * @note Leží-li poslední zpracovaná hrana nepatrně v budoucnosti (hrana
 *       zpracovaná v předstihu kvůli zpoždění GATE0), nedělá nic.
 */
static inline void ctc8253_update_ctc0_by_totalticks(unsigned event_total_ticks)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];

    if ((unsigned)(ctc0->clk1m1_last_event_total_ticks - event_total_ticks) <= (2 * GDGCLK_CTC0_DIVIDER))
        return;

    unsigned elapsed_ticks = event_total_ticks - ctc0->clk1m1_last_event_total_ticks;
    unsigned n = elapsed_ticks / GDGCLK_CTC0_DIVIDER;

    if ((int)ctc0->clk1m1_event.ticks != -1)
    {
        unsigned to_event = (unsigned)gdg_compute_total_ticks(ctc0->clk1m1_event.ticks) - ctc0->clk1m1_last_event_total_ticks;
        unsigned before_event = (to_event < 0x80000000u) ? (to_event / GDGCLK_CTC0_DIVIDER) : 0;
        if (before_event > 0)
            before_event--;
        if (n > before_event)
            n = before_event;
    };

    ctc8253_ctc0_advance(ctc0, n);
    ctc0->clk1m1_last_event_total_ticks += n * GDGCLK_CTC0_DIVIDER;
}

/**
 * @brief Naplánuje událost CTC0 na první sestupnou hranu CLK0 po @p after_ticks.
 *
 * @param after_ticks Čas v tikách snímku; událost bude na první hraně za ním.
 * @post clk1m1_last_event_total_ticks = hrana před naplánovanou (value ji
 *       zahrnuje); je-li událost dřív než g_mzarch_main.event, převezme ji
 *       plánovač.
 */
static void ctc8253_ctc0_schedule(unsigned after_ticks)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    ctc0->clk1m1_event.ticks = gdg_proximate_clk1m1_event(after_ticks);
    ctc0->clk1m1_last_event_total_ticks = (unsigned)gdg_compute_total_ticks(ctc0->clk1m1_event.ticks) - GDGCLK_CTC0_DIVIDER;

    if (ctc0->clk1m1_event.ticks <= g_mzarch_main.event.ticks)
    {
        g_mzarch_main.event.ticks = ctc0->clk1m1_event.ticks;
        g_mzarch_main.event.event_name = MZEVENT_CTC0;
    };
}

/**
 * @brief Zpracuje v dosavadním stavu všechny sestupné hrany CLK0 do @p until_ticks včetně.
 *
 * Pro změnu GATE0, která se projeví až po CTC8253_CTC0_GATE_DELAY: hrany do
 * té doby ještě vidí starou úroveň. Hrana tak může být zpracována o několik
 * tiků dřív, než k ní dojde čas emulace (výstupní callback dostane její
 * skutečný čas).
 *
 * @param until_ticks Čas v tikách snímku.
 * @post Naplánovaná událost CTC0 (pokud je) leží za @p until_ticks a plánovač o ní ví.
 */
static void ctc8253_ctc0_catch_up(unsigned until_ticks)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];

    while (((int)ctc0->clk1m1_event.ticks != -1) && (ctc0->clk1m1_event.ticks <= until_ticks))
    {
        ctc8253_ctc1m1_event(ctc0->clk1m1_event.ticks);
    };

    if (ctc8253_is_counting(ctc0))
    {
        ctc8253_update_ctc0_by_totalticks((unsigned)gdg_compute_total_ticks(until_ticks));
    };

    if (((int)ctc0->clk1m1_event.ticks != -1) && (ctc0->clk1m1_event.ticks <= g_mzarch_main.event.ticks))
    {
        g_mzarch_main.event.ticks = ctc0->clk1m1_event.ticks;
        g_mzarch_main.event.event_name = MZEVENT_CTC0;
    };
}

void ctc8253_ctc0_restart_event(void)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    ctc0->clk1m1_event.event_name = MZEVENT_CTC0;
    ctc8253_ctc0_schedule(g_gdg.total_elapsed.ticks);
}

void ctc8253_sync_ctc0(void)
{
    if (!(g_ctc8253[CTC_CS0].latch_op == 1))
    {
        if (ctc8253_is_counting(&g_ctc8253[CTC_CS0]))
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
 *          io_catalog.c) a mapped-read pres `memory_read_byte` /
 *          `memory_read_byte_no_se` jde nosync cestou pres ctc8253_peek_byte().
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
         * memory_read_byte, ktera CTC cte pres ctc8253_peek_byte bez mutace;
         * UI okna ctou raw g_ctc8253[] side-effect-free). Jediny
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

uint8_t ctc8253_peek_byte(unsigned cs)
{
    if (cs > CTC_CS2) return 0x00;

    unsigned value = (g_ctc8253[cs].latch_op == 1) ? g_ctc8253[cs].read_latch : g_ctc8253[cs].value;

    switch (g_ctc8253[cs].rlf)
    {
    case CTC_RLF_LSB:
        return (value & 0xff);
    case CTC_RLF_MSB:
        return (value >> 8) & 0xff;
    case CTC_RLF_LSBMSB:
        return (g_ctc8253[cs].rl_byte == 0) ? (value & 0xff) : ((value >> 8) & 0xff);
    };
    return 0x00;
}

/**
 * @brief Zápis CPU do 8253: řídicí slovo (CW), Counter Latch nebo bajt hodnoty čítače.
 *
 * - CW: nastaví režim, RL formát a BCD, stav CTC_STATE_INIT a startovní
 *   úroveň OUT (režim 0: 0, ostatní: 1). Čítač pak čeká na hodnotu.
 * - Counter Latch (RL = 00): zachytí value do read_latch. Rozepsaný
 *   dvoubajtový zápis hodnoty (wr_half) nenaruší (změřeno na MZ-800,
 *   ctc-meas test G); jinak vrací pořadí bajtů na LSB jako dřív.
 * - Hodnota: po posledním bajtu (podle RL formátu) přejde čítač ze stavů
 *   <= CTC_STATE_LOAD_DONE do LOAD_DONE a load_wait_rise se nastaví podle
 *   okamžiku zápisu vůči CLK (ctc8253_load_skips_edge()). Režim 0 při
 *   přepisu během čítání shodí OUT hned prvním bajtem. Režim 4 nahraje novou
 *   hodnotu další hranou CLK i během čítání (báze 06-ctc-8253.md, na HW
 *   neměřeno [neověřeno]). Režimy 1, 2, 3 a 5 během čítání jen uloží nový
 *   preset, použije se při dalším nahrání.
 *
 * @param addr Adresa v rámci 8253 (spodní 2 bity: CTCADDR_CTC0..CTCADDR_CWREG).
 * @param value Zapisovaný bajt.
 * @note Vedlejší efekty: změna OUT (callback), u CTC0 v rychlé cestě
 *       přeplánování eventu MZEVENT_CTC0, záznam do hwlog (debugger). Řídicí
 *       slovo pro CTC0 ukončí stav "dělič 2" zvukového kanálu
 *       (ctc8253_ctc0_audio_update()).
 * @note Řídicí slovo pro jiný čítač mezi dvěma bajty hodnoty dá na HW
 *       náhodný dělič (ctc-meas testy G a V); emulátor to nemodeluje
 *       a hodnotu nahraje správně.
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
        if ((cs == CTC_CS0) && ctc8253_is_counting(&g_ctc8253[CTC_CS0]))
        {
            ctc8253_update_ctc0_by_totalticks(gdg_compute_total_ticks(gdg_get_insigeop_ticks()));
        };

        old_state = g_ctc8253[cs].state;
#endif
        unsigned rlf = (value >> 4) & 0x03;

        /* LatchOp - priprava na cteni */
        if (rlf == 0)
        {
            DBGPRINTF(DBGINF, "LatchOP CTC: %d, PC = 0x%04x\n", cs, g_mzarch_main.instruction_addr);

            /* rozepsaný dvoubajtový zápis hodnoty povel zachycení nenaruší */
            if (!g_ctc8253[cs].wr_half)
            {
                g_ctc8253[cs].rl_byte = 0;
            };

            g_ctc8253[cs].latch_op = 1;

            g_ctc8253[cs].read_latch = g_ctc8253[cs].value;
            /* TODO: proverit jak se chova read latch, zmeni se pokud se dokoncil countdown, nebo pokud prisel trigger, atp. ? */
            return;
        };

        g_ctc8253[cs].rl_byte = 0;
        g_ctc8253[cs].wr_half = 0;
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
        g_ctc8253[cs].load_wait_rise = 0;
        /* skutecny 8253 zrejme pri initu na value nesaha */
        // g_ctc8253[cs].value = 0;

        DBGPRINTF(DBGINF, "INIT CTC - 0x%02x - addr: %d, RLF: %d, MODE: %d, BCD: %d, PC = 0x%04x\n", value, cs, g_ctc8253[cs].rlf, g_ctc8253[cs].mode, g_ctc8253[cs].bcd, g_mzarch_main.instruction_addr);

        unsigned output_state = (g_ctc8253[cs].mode == CTC_MODE0) ? 0 : 1;
        ctc8253_set_out(cs, output_state, gdg_get_insigeop_ticks());

        if ((cs == CTC_CS0) && g_audio.ctc0_div2)
        {
            /* řídicí slovo ukončilo režim 3 s děličem 2; OUT0 se měnit nemusel */
            ctc8253_ctc0_audio_update(gdg_get_insigeop_ticks());
        };
    }
    else
    {

        /* Zapis do citace */
        cs = addr;

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        if ((cs == CTC_CS0) && ctc8253_is_counting(&g_ctc8253[CTC_CS0]))
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
                g_ctc8253[cs].load_wait_rise = 0;
                ctc8253_set_out(cs, 0, gdg_get_insigeop_ticks());
            };
        };
        DBGPRINTF(DBGINF, "LOAD CTC addr: %d, value: 0x%02x, PC = 0x%04x\n", cs, value, g_mzarch_main.instruction_addr);

        int complete = 1;

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
                g_ctc8253[cs].wr_half = 1;
                complete = 0;
            }
            else
            {
                g_ctc8253[cs].preset_latch |= value << 8;
                g_ctc8253[cs].rl_byte = 0;
                g_ctc8253[cs].wr_half = 0;
            };
            break;
        };

        if (complete)
        {
            g_ctc8253[cs].preset_value = (g_ctc8253[cs].preset_latch == 0) ? 0x10000 : g_ctc8253[cs].preset_latch;

            if (g_ctc8253[cs].mode == CTC_MODE3)
            {
                /* MSM82C53-2: N = 1 v rezimu 3 = perioda 65537 CLK (baze hw/06-ctc-8253.md) */
                if (g_ctc8253[cs].preset_value == 1)
                {
                    g_ctc8253[cs].preset_value = 0x10001;
                };
            };

            /* Dokoncen LOAD: hodnotu nahraje sestupna hrana CLK, pred kterou
             * po zapisu probehla vzestupna hrana s dostatecnym predstihem
             * (viz ctc8253_load_skips_edge()); na tom, kdy prislo CW, nezalezi. */

            if ((g_ctc8253[cs].state <= CTC_STATE_LOAD_DONE) || ((g_ctc8253[cs].mode == CTC_MODE4) && (g_ctc8253[cs].state != CTC_STATE_LOAD_DONE)))
            {
                g_ctc8253[cs].state = CTC_STATE_LOAD_DONE;
                g_ctc8253[cs].load_done = 0;
                g_ctc8253[cs].load_wait_rise = ctc8253_load_skips_edge(cs, gdg_get_insigeop_ticks());
            }
            else if (g_ctc8253[cs].state == CTC_STATE_MODE1_TRIGGER_ERROR)
            {
                g_ctc8253[cs].state = CTC_STATE_PRESET32;
            };
        };
    };

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    if (cs == CTC_CS0)
    {
        if (old_state != g_ctc8253[cs].state)
        {
            /* vytvorime event pro zavolani CTC0 ctc8253_clkfall() */
            ctc8253_ctc0_schedule(gdg_get_insigeop_ticks());
        };
    };
#endif
}

/**
 * @brief Zpracuje sestupnou hranu vstupu CLK čítače.
 *
 * Společné: je-li nastaveno load_wait_rise, hrana nedělá nic, jen příznak
 * vynuluje (hodnota byla dopsána příliš pozdě před náběžnou hranou CLK,
 * nahraje ji až další sestupná hrana).
 *
 * Chování podle režimu (L = hrana nahrání, T = hrana spuštění, N = dělič;
 * změřeno na MZ-800, emu-experiments/ctc-meas a ctc-mode0-load):
 *
 * - Režim 0: nahrání presetu, pak -1 za hranu; na L + N OUT = 1 a čítač
 *   ukazuje 0000h po celou periodu CLK, 0FFFFh až po další hraně, dál čítá
 *   dolů bez cíle. GATE = 0 čítání pozastaví.
 * - Režim 1: spuštění náběžnou hranou GATE: na T OUT = 0 a nahrání, na
 *   T + N OUT = 1.
 * - Režim 2: preset .. 1, na L + N - 1 OUT = 0 na jednu periodu, pak nové nahrání.
 * - Režim 3: po 2 (ctc8253_mode3_step()); při dosažení 0 se přepne OUT
 *   a nahraje preset. S děličem 3 jde OUT do nuly a zůstane tam (změřeno
 *   v 255 opakováních z 256; příčina neznámá), čítač pak běží bez cíle.
 * - Režim 4: jako režim 0, ale na L + N jde OUT do 0 na jednu periodu CLK.
 * - Režim 5: jako režim 1, ale OUT jde do 0 až na T + N na jednu periodu CLK.
 *
 * Čítání BCD: value je v kódu BCD, kroky jsou dekadické (0 = 10 000).
 *
 * @param cs Index čítače (CTC_CS0..CTC_CS2).
 * @param event_ticks Okamžik hrany v tikách snímku (pro callback výstupu).
 * @note Vedlejší efekty: změna OUT volá output_cb (CTC1 -> takt CTC2,
 *       CTC2 -> přerušení, CTC0 -> zvuk a PIO).
 * @pre Volat z emu vlákna.
 */
void ctc8253_clkfall(unsigned cs, unsigned event_ticks)
{
    st_CTC8253 *ctc = &g_ctc8253[cs];

    if (ctc->load_wait_rise)
    {
        ctc->load_wait_rise = 0;
        return;
    };

    switch (ctc->mode)
    {

    case CTC_MODE0:
    case CTC_MODE4:
        if (ctc->state == CTC_STATE_BLIND_COUNT)
        {
            ctc8253_dec_wrap(ctc, 1);
            return;
        }
        else if (ctc->state == CTC_STATE_STROBE)
        {
            /* konec pulzu režimu 4 */
            ctc8253_set_out(cs, 1, event_ticks);
            ctc8253_dec_wrap(ctc, 1);
            ctc->state = CTC_STATE_BLIND_COUNT;
            return;
        }
        else if (ctc->state >= CTC_STATE_COUNTDOWN)
        {
            ctc8253_dec(ctc, 1);
            if (ctc->value == 0x0000)
            {
                if (ctc->mode == CTC_MODE0)
                {
                    ctc8253_set_out(cs, 1, event_ticks);
                    ctc->state = CTC_STATE_BLIND_COUNT;
                }
                else
                {
                    ctc8253_set_out(cs, 0, event_ticks);
                    ctc->state = CTC_STATE_STROBE;
                };
            };
            return;
        }
        else if (ctc->state == CTC_STATE_LOAD_DONE)
        {

            ctc->value = ctc->preset_value;

            if (ctc->mode == CTC_MODE4)
            {
                /* nové nahrání během pulzu ho ukončí */
                ctc8253_set_out(cs, 1, event_ticks);
            };

            if (ctc->gate == 1)
            {
                ctc->state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
            return;
        };
        break;

    case CTC_MODE1:
    case CTC_MODE5:
        if (ctc->state == CTC_STATE_BLIND_COUNT)
        {
            if (ctc->mode == CTC_MODE5)
            {
                ctc8253_dec_wrap(ctc, 1);
            }
            else
            {
                ctc8253_dec(ctc, 1);
                if (ctc->value == 0x0000)
                {
                    ctc->value = ctc->bcd ? 0x9999 : 0xffff;
                };
            };
            return;
        }
        else if (ctc->state == CTC_STATE_STROBE)
        {
            /* konec pulzu režimu 5 */
            ctc8253_set_out(cs, 1, event_ticks);
            ctc8253_dec_wrap(ctc, 1);
            ctc->state = (ctc->gate == 1) ? CTC_STATE_BLIND_COUNT : CTC_STATE_WAIT_GATE1;
            return;
        }
        else if (ctc->state == CTC_STATE_COUNTDOWN)
        {
            ctc8253_dec(ctc, 1);
            if (ctc->value == 0x0000)
            {
                if (ctc->mode == CTC_MODE5)
                {
                    ctc8253_set_out(cs, 0, event_ticks);
                    ctc->state = CTC_STATE_STROBE;
                }
                else
                {
                    ctc8253_set_out(cs, 1, event_ticks);
                    if (ctc->gate == 1)
                    {
                        ctc->state = CTC_STATE_BLIND_COUNT;
                    }
                    else
                    {
                        ctc->state = CTC_STATE_WAIT_GATE1;
                    };
                };
            };
            return;
        }
        else if (ctc->state == CTC_STATE_LOAD_DONE)
        {
            if (ctc->gate == 1)
            {
                ctc->state = CTC_STATE_BLIND_COUNT;
            }
            else
            {
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
            return;
        }
        else if (ctc->state == CTC_STATE_PRESET)
        {
            ctc->value = ctc->preset_value;
            if (ctc->mode == CTC_MODE1)
            {
                ctc8253_set_out(cs, 0, event_ticks);
            };
            ctc->state = CTC_STATE_COUNTDOWN;
            return;
        }
        else if (ctc->state == CTC_STATE_PRESET32)
        {
            ctc->value = ctc->bcd ? 0x32 : 32;
            ctc->state = CTC_STATE_COUNTDOWN;
            return;
        };
        break;

    case CTC_MODE2:
        if (ctc->state == CTC_STATE_COUNTDOWN)
        {

            ctc8253_dec(ctc, 1);

            if (ctc->value == 0x0001)
            {
                ctc8253_set_out(cs, 0, event_ticks);
                ctc->state = CTC_STATE_PRESET;
            };
            return;
        }
        else if ((ctc->state == CTC_STATE_PRESET) || (ctc->state == CTC_STATE_LOAD_DONE))
        {

            ctc8253_set_out(cs, 1, event_ticks);

            ctc->value = ctc->preset_value;

            if (ctc->value == 0x0001)
            {
                ctc->state = CTC_STATE_PRESET_ERROR;
            }
            else
            {
                if (ctc->gate == 1)
                {
                    ctc->state = CTC_STATE_COUNTDOWN;
                }
                else
                {
                    ctc->state = CTC_STATE_WAIT_GATE1;
                };
            };
            return;
        };
        break;

    case CTC_MODE3:

        if (ctc->state == CTC_STATE_BLIND_COUNT)
        {
            /* dělič 3 po pádu OUT do nuly: čítá po 2 bez cíle */
            ctc8253_dec_wrap(ctc, 2);
            return;
        }
        else if (ctc->state == CTC_STATE_COUNTDOWN)
        {
            unsigned step = ctc8253_mode3_step(ctc);

            if (ctc8253_count(ctc) > step)
            {
                ctc8253_dec(ctc, step);
            }
            else
            {
                /* konec pulperiody: prepnout OUT a nahrat preset */
                ctc->value = ctc->preset_value;

                if (ctc->out == 1)
                {
                    ctc8253_set_out(cs, 0, event_ticks);

                    if (ctc8253_count(ctc) == 3)
                    {
                        /* HW MZ-800: s děličem 3 už se OUT do jedničky nevrátí */
                        ctc->state = CTC_STATE_BLIND_COUNT;
                    };
                }
                else
                {

                    ctc8253_set_out(cs, 1, event_ticks);

                    if (ctc->gate == 1)
                    {
                        ctc->state = CTC_STATE_COUNTDOWN;
                    }
                    else
                    {
                        ctc->state = CTC_STATE_WAIT_GATE1;
                    };
                };
            };
            return;
        }
        else if ((ctc->state == CTC_STATE_PRESET) || (ctc->state == CTC_STATE_LOAD_DONE))
        {
            ctc8253_set_out(cs, 1, event_ticks);

            ctc->value = ctc->preset_value;

            if (ctc->gate == 1)
            {
                ctc->state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
            return;
        };
        break;
    };

    if (ctc->state == CTC_STATE_INIT)
    {
        if (ctc->load_done == 1)
        {
            ctc->state = CTC_STATE_LOAD_DONE;
            ctc->load_done = 0;
        }
        else
        {
            ctc->state = CTC_STATE_INIT_DONE;
        }
    };
}

/**
 * @brief Změna úrovně vstupu GATE čítače.
 *
 * Chování podle režimu (změřeno na MZ-800, emu-experiments/ctc-meas, pokud
 * není uvedeno jinak):
 *
 * - Režimy 0 a 4: GATE = 0 čítání pozastaví, GATE = 1 pokračuje od místa
 *   zastavení (režim 4 podle báze 06-ctc-8253.md, neměřeno [neověřeno]).
 * - Režimy 1 a 5: náběžná hrana spustí (i znovu během pulzu) nahrání na
 *   hraně T; sestupná hrana čítání neovlivní.
 * - Režimy 2 a 3: GATE = 0 zvedne OUT hned, bez čekání na hodiny, a čítání
 *   zastaví; náběžná hrana nahraje dělič znovu na hraně T.
 *
 * Čip vzorkuje GATE náběžnou hranou CLK. U CTC0 (jediný čítač s proměnným
 * hradlem) se proto změna projeví až na sestupných hranách CLK0 pozdějších
 * než @p event_ticks + CTC8253_CTC0_GATE_DELAY; hrany do té doby se zpracují
 * ve starém stavu (ctc8253_ctc0_catch_up()). Výjimkou je GATE = 0 v režimech
 * 2 a 3, které působí hned. U CTC1 a CTC2 (hradla jsou v MZ trvale v 1)
 * se změna projeví od nejbližší sestupné hrany.
 *
 * @param cs Index čítače (CTC_CS0..CTC_CS2).
 * @param gate Nová úroveň (bit 0).
 * @param event_ticks Okamžik změny v tikách snímku (konec M-cyklu zápisu).
 * @note Vedlejší efekty: změna OUT (callback), u CTC0 přeplánování eventu
 *       MZEVENT_CTC0 a případné zpracování hrany CLK0 v předstihu. GATE0 = 0
 *       ukončí stav "dělič 2" zvukového kanálu (ctc8253_ctc0_audio_update()).
 * @pre Volat z emu vlákna.
 */
void ctc8253_gate(unsigned cs, unsigned gate, unsigned event_ticks)
{
    st_CTC8253 *ctc = &g_ctc8253[cs];

    gate = gate & 0x01;

    /* Gate se nezmenila - jdeme pryc */
    if (ctc->gate == gate)
        return;

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    unsigned effect_ticks = event_ticks;

    if (cs == CTC_CS0)
    {
        if (!((gate == 0) && ((ctc->mode == CTC_MODE2) || (ctc->mode == CTC_MODE3))))
        {
            effect_ticks += CTC8253_CTC0_GATE_DELAY;
        };
        ctc8253_ctc0_catch_up(effect_ticks);
    };

    en_CTC_STATE old_state = ctc->state;
#endif

    ctc->gate = gate;

    /* HWE: ctc:gate0_edge event byl vyřazen v V1.5 HWE redesign
     * (= nebyl v Michalově finálním listu, gate je vstup ne výstup). */

    if (ctc->state == CTC_STATE_INIT)
        return;

    switch (ctc->mode)
    {

    case CTC_MODE0:
        if (ctc->gate == 0)
        {
            if (ctc->state >= CTC_STATE_COUNTDOWN)
            {
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
        }
        else if (ctc->state == CTC_STATE_WAIT_GATE1)
        {
            if (ctc->out == 0)
            {
                ctc->state = CTC_STATE_COUNTDOWN;
            }
            else
            {
                ctc->state = CTC_STATE_BLIND_COUNT;
            };
        };
        break;

    case CTC_MODE4:
        /* čítání bez cíle po pulzu se nepozastavuje (ovlivnilo by jen čtenou hodnotu) */
        if (ctc->gate == 0)
        {
            if (ctc->state == CTC_STATE_COUNTDOWN)
            {
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
        }
        else if (ctc->state == CTC_STATE_WAIT_GATE1)
        {
            ctc->state = CTC_STATE_COUNTDOWN;
        };
        break;

    case CTC_MODE1:
    case CTC_MODE5:
        if (ctc->gate == 1)
        {
            if ((ctc->state == CTC_STATE_LOAD_DONE) || (ctc->state == CTC_STATE_WAIT_GATE1) || (ctc->state == CTC_STATE_COUNTDOWN))
            {
                ctc->state = CTC_STATE_PRESET;
            }
            else if (ctc->state == CTC_STATE_STROBE)
            {
                /* nové spuštění během pulzu režimu 5 */
                ctc8253_set_out(cs, 1, event_ticks);
                ctc->state = CTC_STATE_PRESET;
            }
            else if ((ctc->state == CTC_STATE_INIT_DONE) && (ctc->mode == CTC_MODE1))
            {
                /* Nabezna GATE prisla drive, nez byl dokoncen LOAD */
                ctc8253_set_out(cs, 0, event_ticks);
                ctc->state = CTC_STATE_MODE1_TRIGGER_ERROR;
            };
        }
        else if (ctc->state == CTC_STATE_BLIND_COUNT)
        {
            /* v tuto chvili by se melo jednat o sestupnou hranu GATE */
            ctc->state = CTC_STATE_WAIT_GATE1;
        };
        break;

    case CTC_MODE2:
    case CTC_MODE3:
        if (ctc->gate == 0)
        {
            if ((ctc->state == CTC_STATE_COUNTDOWN) || (ctc->state == CTC_STATE_PRESET) || (ctc->state == CTC_STATE_BLIND_COUNT))
            {
                ctc8253_set_out(cs, 1, event_ticks);
                ctc->state = CTC_STATE_WAIT_GATE1;
            };
        }
        else if (ctc->state == CTC_STATE_WAIT_GATE1)
        {
            ctc->state = CTC_STATE_PRESET;
        };
        break;
    };

    if ((cs == CTC_CS0) && g_audio.ctc0_div2)
    {
        /* brána zastavila režim 3 s děličem 2; OUT0 se měnit nemusel */
        ctc8253_ctc0_audio_update(event_ticks);
    };

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    if (cs == CTC_CS0)
    {
        if (old_state != ctc->state)
        {
            ctc8253_ctc0_schedule(effect_ticks);
        };
    };
#endif
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST

/**
 * @brief Počet sestupných hran CLK0 do nejbližší hrany, na které se děje víc než prostý krok čítače.
 *
 * @param ctc0 Čítač 0.
 * @return Počet hran (>= 1), nebo 0, když žádná taková hrana nepřijde
 *         (čítač stojí, nebo čítá bez cíle).
 */
static unsigned ctc8253_ctc0_edges_to_event(const st_CTC8253 *ctc0)
{
    if (ctc0->load_wait_rise)
        return 1;

    switch (ctc0->state)
    {
    case CTC_STATE_LOAD_DONE:
    case CTC_STATE_PRESET:
    case CTC_STATE_PRESET32:
    case CTC_STATE_STROBE:
        return 1;

    case CTC_STATE_COUNTDOWN:
    {
        unsigned cnt = ctc8253_count(ctc0);

        switch (ctc0->mode)
        {
        case CTC_MODE2:
            /* OUT = 0 při hodnotě 1 */
            return (cnt > 1) ? (cnt - 1) : 1;

        case CTC_MODE3:
            return ctc8253_mode3_clocks_to_tc(ctc0);

        default:
            /* režimy 0, 1, 4, 5: událost při hodnotě 0 */
            return (cnt > 0) ? cnt : 1;
        };
    }

    default:
        return 0;
    };
}

/**
 * @brief Událost rychlé cesty CTC0: sestupná hrana CLK0, na které se děje víc než prostý krok.
 *
 * Dopočítá prosté kroky čítače od poslední zpracované hrany, zpracuje tuto
 * hranu (ctc8253_clkfall()) a naplánuje další událost podle stavu čítače.
 *
 * @param event_ticks Čas hrany v tikách snímku.
 * @note Volá ji plánovač (mzarch_main_process_events()) a ctc8253_ctc0_catch_up().
 * @post clk1m1_last_event_total_ticks = čas této hrany; clk1m1_event.ticks
 *       je další událost, nebo -1.
 */
void ctc8253_ctc1m1_event(unsigned event_ticks)
{

    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];

    unsigned event_total_ticks = gdg_compute_total_ticks(event_ticks);

    if (ctc8253_is_counting(ctc0))
    {
        unsigned elapsed_ticks = event_total_ticks - ctc0->clk1m1_last_event_total_ticks;

        if ((elapsed_ticks >= GDGCLK_CTC0_DIVIDER) && (elapsed_ticks < 0x80000000u))
        {
            /* dopocitat hrany pred touto (posledni zpracuje ctc8253_clkfall()) */
            ctc8253_ctc0_advance(ctc0, (elapsed_ticks / GDGCLK_CTC0_DIVIDER) - 1);
        }
        else if ((elapsed_ticks == 0) && (ctc0->state == CTC_STATE_COUNTDOWN))
        {
            /* Hodnota už tuto hranu zahrnuje (snapshot ze starší verze, kde
             * čtení těsně před událostí čítač dopočítalo až na cílovou
             * hodnotu - výpadek zvuku u Galao). Vrátit poslední krok. */
            switch (ctc0->mode)
            {
            case CTC_MODE0:
            case CTC_MODE1:
            case CTC_MODE4:
            case CTC_MODE5:
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
            };
        }
    };

    ctc8253_clkfall(CTC_CS0, event_ticks);
    ctc0->clk1m1_last_event_total_ticks = event_total_ticks;

    unsigned edges = ctc8253_ctc0_edges_to_event(ctc0);

    if (edges != 0)
    {
        ctc0->clk1m1_event.ticks = event_ticks + (edges * GDGCLK_CTC0_DIVIDER);
    }
    else
    {
        ctc0->clk1m1_event.ticks = -1;
    };
}

#endif
