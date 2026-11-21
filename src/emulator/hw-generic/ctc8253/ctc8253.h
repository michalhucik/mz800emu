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

#include "mzarch/mzarch_config.h"
#include <stdint.h>

#ifdef MZ800EMU_CFG_CLK1M1_FAST
#include "mzarch/mzarch.h"
#endif

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
        CTC_STATE_STROBE,              /* rezimy 4 a 5: OUT = 0 po dobu jedne periody CLK, dalsi hrana ho vrati do 1 */

    } en_CTC_STATE;

    typedef struct st_CTC8253
    {
        unsigned out;     /* Vystupni hodnota citace: 0|1 */
        unsigned gate;    /* Posledni znama uroven GATE: 0|1 */
        en_CTC_MODE mode; /* Mode: 0 - 5 */
        unsigned bcd;     /* BCD: 0|1 */
        en_CTC_RLF rlf;   /* Read / Load Format */
        en_CTC_STATE state;
        /**
         * Historický příznak "hodnota zapsána ve stavu INIT". Nový kód ho
         * nenastavuje (nahrání řídí load_wait_rise); zůstává kvůli formátu
         * snapshotu a hwlog - snapshot ze starší verze s load_done = 1 se
         * dokončí původní cestou INIT -> LOAD_DONE v ctc8253_clkfall().
         */
        unsigned load_done;
        /**
         * 1 = nejbližší sestupná hrana CLK nedělá nic, jen tento příznak
         * vynuluje. Nastavuje ho ctc8253_write_byte() po dopsání hodnoty,
         * která nestihla náběžnou hranu CLK (zápis při CLK = HIGH nebo méně
         * než asi 250 ns před náběžnou hranou; viz ctc8253_load_skips_edge()),
         * nuluje ctc8253_clkfall() a zápis řídicího slova. Změřeno na HW
         * MZ-800 (emu-experiments/ctc-mode0-load a ctc-meas): hodnotu nahraje
         * sestupná hrana CLK, před kterou po zápisu proběhla náběžná hrana;
         * okamžik zápisu CW nerozhoduje.
         */
        unsigned load_wait_rise;

        /**
         * 1 = byl zapsán dolní bajt dvoubajtové hodnoty (RL = LSB+MSB) a čeká
         * se na horní. Povel Counter Latch v tom stavu nevrací rl_byte na 0
         * (HW MZ-800: povel zachycení mezi bajty děliče zápis nenaruší).
         * Invariant: wr_half = 1 jen při rlf = CTC_RLF_LSBMSB a rl_byte = 1.
         */
        unsigned wr_half;

        unsigned rl_byte; /* pocet bajtu, ktere uz byly do ctc zapsany, nebo precteny - v 8253 je na to skutecne jen jeden registr! */

        unsigned latch_op; /* 1 - pokud mame neco natazeno v read_latch */
        unsigned read_latch;
        unsigned preset_value;
        unsigned preset_latch;
        /**
         * Aktuální stav čítače tak, jak ho čte CPU (dolních 16 bitů; hodnota
         * 0x10000 / 0x10001 jen hned po nahrání presetu 0 / režimu 3 s N = 1).
         * Při čítání BCD (bcd = 1) je v kódu BCD (0x10000 = 10 000).
         * V režimu 3 čítá po 2 (liché N: první krok po nahrání -1 při OUT = 1,
         * -3 při OUT = 0), viz ctc8253_mode3_step().
         */
        unsigned value;
        /** Nepoužívá se (dřívější model režimu 3); zůstává kvůli formátu snapshotu a hwlog. */
        unsigned mode3_destination_value;
        /** Nepoužívá se (dřívější model režimu 3); zůstává kvůli formátu snapshotu a hwlog. */
        unsigned mode3_half_value;
        ctc8253_out_cb_t output_cb; /* tento callback je zavolan pri kazde zmene vystupniho stavu citace */

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        unsigned clk1m1_last_event_total_ticks;
        st_EMUEVENT clk1m1_event;
#endif
    } st_CTC8253;

    extern struct st_CTC8253 g_ctc8253[3];

    /* Debug/UI mirror: posledni byte zapsany na CW registr (CTCADDR_CWREG,
     * IORQ 0xD7 / MMIO 0xE007). 8253 hardware CW neumi precist - sequencer
     * pro mode/counter select. Tracking pouze pro Overview tab v debugger UI.
     * Default 0x00 dokud HW init nebyl proveden. */
    extern uint8_t g_ctc8253_last_cw_byte;

    extern void ctc8253_init(void);
    extern uint8_t ctc8253_read_byte(unsigned cs);

    /**
     * @brief Bajt, který by vrátilo čtení čítače @p cs, bez vedlejšího efektu.
     *
     * Stejná volba zdroje (read_latch při Counter Latch, jinak value) a bajtu
     * (LSB / MSB / podle rl_byte u LSBMSB) jako ctc8253_read_byte(), ale
     * rl_byte ani latch_op nemění. Pro debugger a mapované čtení 0E004h-0E006h
     * v režimu MZ-700 (memory_read_byte_no_se, Memory Browser, Watch).
     *
     * @param cs Index čítače CTC_CS0..CTC_CS2; jiná hodnota vrátí 0x00.
     * @return Bajt čítače.
     * @note CTC0 se v rychlé cestě (MZ800EMU_CFG_CLK1M1_FAST) před čtením
     *       nedosynchronizuje (ctc8253_sync_ctc0() mění stav): vrátí hodnotu
     *       z poslední synchronizace, která se od čtení CPU v tomtéž okamžiku
     *       může lišit. CTC1 a CTC2 se synchronizací nečtou.
     * @pre Emu vlákno, nebo pauza.
     */
    extern uint8_t ctc8253_peek_byte(unsigned cs);
    extern void ctc8253_write_byte(unsigned addr, uint8_t value);
    extern void ctc8253_gate(unsigned addr, unsigned gate, unsigned event_screen_ticks);
    extern void ctc8253_clkfall(unsigned cs, unsigned event_ticks);

    /**
     * @brief Zapíše do zvukového kanálu CTC0 úroveň podle stavu čítače a bitu PC0.
     *
     * Pro změny, které nepřicházejí hranou OUT0: zápis bitu PC0 (8255),
     * zastavení bránou a řídicí slovo. Běží-li CTC0 v režimu 3 s děličem 2
     * (stav ohlášený hranou OUT0, viz audio_ctc0_div2_changed(), a čítač dál
     * čítá v režimu 3 s GATE0 = 1), jde do kanálu stálá úroveň maskovaná PC0;
     * jinak OUT0 AND PC0 (audio_ctc0_changed()). Okamžitý stav OUT0 se při
     * děliči 2 nebere: obdélník 554 kHz je proti klíčování náhodný.
     *
     * @param event_ticks Okamžik změny v tikách snímku (gdg_get_insigeop_ticks()).
     * @pre Vlákno emulátoru; zvukový log existuje (audio_init()).
     * @post Kanál CTC0 zvukového logu odpovídá stavu čítače a PC0.
     */
    extern void ctc8253_ctc0_audio_update(unsigned event_ticks);

    /**
     * @brief Odvodí stav zvukového kanálu CTC0 ze stavu čítače po načtení snapshotu.
     *
     * Stav "režim 3 s děličem 2" ani úroveň kanálu ve snapshotu nejsou.
     * Dělič se bere ze st_CTC8253.preset_value: liší se od právě odpočítávaného
     * nejvýš do konce jedné půlperiody, pak ho opraví nejbližší hrana OUT0.
     * Do zvukového logu nepíše (audio_ctc0_restore()).
     *
     * @pre g_ctc8253 a PC0 (g_pio8255) jsou už načtené; volat před
     *      audio_reset_log().
     */
    extern void ctc8253_ctc0_audio_restore(void);


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



#ifdef MZ800EMU_CFG_CLK1M1_FAST
    extern void ctc8253_ctc1m1_event(unsigned event_ticks);
    extern void ctc8253_sync_ctc0(void);

    /**
     * @brief Naplánuje událost CTC0 rychlé cesty na nejbližší hranu CLK0.
     *
     * Pro snapshot bez uložené události CTC0 (starší verze): událost
     * `clk1m1_event` se nastaví na nejbližší sestupnou hranu CLK0 od
     * aktuálního času GDG a báze `clk1m1_last_event_total_ticks` na
     * aktuální čas, takže ctc8253_ctc1m1_event() zpracuje jedinou hranu
     * a další událost naplánuje podle stavu čítače. Je-li nová událost
     * dřív než `g_mzarch_main.event`, převezme ji plánovač.
     *
     * @pre g_gdg.total_elapsed je už obnovený (GDG se načítá před CTC).
     * @post Průběh CTC0 po loadu nezávisí na stavu stroje před loadem.
     */
    extern void ctc8253_ctc0_restart_event(void);
#define ctc8253_get_ctc1m1_event_pointer() (&g_ctc8253[CTC_CS0].clk1m1_event)
#endif

#ifdef MZ800EMU_CFG_CLK1M1_FAST
#include "gdg/video.h"
/**
 * @brief Konec snímku: posune čekající událost CTC0 o délku skončeného snímku.
 *
 * Délka je gdg_get_ending_frame_ticks() (na MZ-800 ji zkracuje bit CKSW),
 * tedy stejná hodnota, o kterou konec snímku posouvá čas GDG. Makro se
 * rozvíjí v mzarch.c, kde je hlavička GDG vložená.
 */
#define ctc8253_on_screen_done_event()                      \
    {                                                       \
        st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];             \
        if ((int)ctc0->clk1m1_event.ticks != -1)            \
        {                                                   \
            ctc0->clk1m1_event.ticks -= gdg_get_ending_frame_ticks(); \
        };                                                  \
    }
#endif

#ifdef __cplusplus
}
#endif

#endif /* CTC8253_H */
