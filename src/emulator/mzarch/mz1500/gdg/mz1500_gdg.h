/*
 * File:   mz1500_gdg.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:38
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

#ifndef MZ1500_GDG_H
#define MZ1500_GDG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "mzarch/mzarch_config.h"

#include "hw-generic/gdg/video.h"
#include "mzarch/mzevent.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdgclk.h"


// Tohle jsem meril jenom na MZ-800, nicmene ty rozdilz meyi platformami budou asi marginalni, proto jsem to pouzil globalne pro vsechny MZ-architektury.
#define IORQ_RD_TICKS 12   /* Delka IORQ RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define IORQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka IORQ WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_M1_TICKS 7 /* Delka MREQ M1 RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_RD_TICKS 9    /* Delka MREQ DATA RD pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */
#define MREQ_WR_TICKS 9    /* ??? TODO: zmerit ??? Delka MREQ DATA WR pulzu v GTG taktech - cteni probehne pri jeho nabezne hrane */

    /*
     * MZ-1500 DMD registr (port 0xF0):
     *   bit 0: MODE — 0 = MZ-700, 1 = MZ-1500 (PCG)
     *   bit 1: PRIORITY — 0 = BPF (Background-PCG-Foreground), 1 = BFP (Background-Foreground-PCG)
     */
#define REGISTER_DMD_FLAG_MZ1500_MODE  (1 << 0)
#define REGISTER_DMD_FLAG_MZ1500_PRIO  (1 << 1)

    /* zachovavame nazev makra pro kompatibilitu s event handlerem — jen zmena semantiky */
#define GDG_MZ800_DMD_TEST_MZ700      (!(g_gdg.regDMD & REGISTER_DMD_FLAG_MZ1500_MODE))
#define GDG_MZ1500_DMD_TEST_MODE1500  (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ1500_MODE)
#define GDG_MZ1500_DMD_TEST_PMODE_BFP (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ1500_PRIO)

#define SIGNAL_GDG_HBLNK (g_gdg.hbln)
#define SIGNAL_GDG_VBLNK (g_gdg.vbln)
#define SIGNAL_GDG_STS_HS (g_gdg.sts_hsync)
#define SIGNAL_GDG_STS_VS (g_gdg.sts_vsync)
#define SIGNAL_GDG_TEMPO (g_gdg.tempo & 1)

#define HBLN_ACTIVE 0
#define HBLN_OFF 1
#define VBLN_ACTIVE 0
#define VBLN_OFF 1
#define HSYN_ACTIVE 0
#define HSYN_OFF 1
#define VSYN_ACTIVE 0
#define VSYN_OFF 1

    typedef struct st_GDGEVENT
    {
        en_MZEVENT event;
        unsigned start_row;    /* od ktereho radku event volame */
        unsigned num_rows;     /* pocet radku na kterych se volani opakuje */
        unsigned event_column; /* na kterem sloupci se event zavola */
    } st_GDGEVENT;

    typedef struct st_GDG_TIMESTAMP
    {
        unsigned screens; /* celkovy pocet vykonanych obrazovek */
        unsigned ticks;   /* celkovy pocet vykonanych pixelu z posledniho nedokonceneho screenu
                           * Hodnota 0 odpovida 1. pixelu viditelneho obrazu <0; 354431>
                           */

    } st_GDG_TIMESTAMP;

    typedef struct st_GDG
    {
        st_EMUEVENT event;

        st_GDG_TIMESTAMP total_elapsed; /* Celkovy pocet vykonanych snimku a pixelu */

        unsigned beam_row;
        unsigned screen_is_already_rendered_at_beam_pos; /* pokud byla pauza a probehnul render obrazovky, tak tady mame posledmi pozici paprsku, ktera uz je zobrazena */

        unsigned screen_need_update_from;   /* od ktereho pixelu aktualniho radku je potreba updatovat framebuffer */
        unsigned last_updated_border_pixel; /* od ktereho pixelu aktualniho radku je potreba updatovat framebuffer */

        unsigned sts_vsync; /* STS Vsync, ktery vidime na status registru - neodpovida skutecnemu VS */
        unsigned sts_hsync; /* STS Hsync, ktery vidime na status registru - neodpovida skutecnemu HS */
        unsigned hbln;      /* HBLN: 0 - pokud se sloupec paprsku nachazi mimo screen */
        unsigned vbln;      /* VBLN: 0 - pokud se radek paprsku nachazi mimo screen */
        //        unsigned hsync;     /* Skutecny Hsync, ktery se posila na vstup CTC1 */

        unsigned regDMD;    /* Display Mode register */
        unsigned regBOR;    /* Border register — MZ-1500 nema border port, vzdy 0 */

        unsigned regct53g7; /* rizeni GATE pro CTC0 v ctc8253 v rezimu MZ700 */

        int mode1500_color[8]; /* MZ-1500: barevna paleta (port 0xF1) */

        unsigned tempo;
        unsigned tempo_divider;

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
        unsigned ctc0clk;
#endif

    } st_GDG;

    extern st_GDG g_gdg;

    extern const struct st_GDGEVENT g_gdgevent[];

#define GDG_TEST_VBLN (g_gdg.vbln == 0)

    extern void gdg_init(void);
    extern void gdg_reset(void);
    extern uint8_t gdg_read_dmd_status_memop(void);

    /**
     * @brief Status 0xE008 (čtení v režimu MZ-700) bez vedlejšího efektu.
     *
     * Stejná hodnota jako gdg_read_dmd_status_memop() pro stejný stav; na
     * MZ-700 a MZ-1500 bez načtení joysticku z hostitele (bity 1-4 ze stavu
     * páček z posledního čtení CPU, joymz_peek_status_bits14()). Na MZ-800
     * je čtení statusu samo bez vedlejšího efektu, funkce je shodná.
     * Pro debugger a mapované čtení memory_read_byte_no_se().
     *
     * @return Status byte (HBLNK, TEMPO, ...).
     * @pre Emu vlákno, nebo pauza.
     */
    extern uint8_t gdg_peek_dmd_status_memop(void);
    extern uint8_t gdg_read_dmd_status_ioop(void);
    extern void gdg_write_byte(unsigned addr, uint8_t value);

#define gdg_compute_total_ticks(now_ticks) (now_ticks + ((uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS))
#define gdg_get_total_ticks() gdg_compute_total_ticks(g_gdg.total_elapsed.ticks)

/**
 * @brief Délka právě končícího snímku v pxCLK.
 *
 * Na této platformě konstanta; na MZ-800 ji zkracuje bit CKSW
 * (mz800_gdg.h). O tuto hodnotu posouvá konec snímku události časované od
 * začátku snímku (CTC0, PIO-Z80, 20ms časovač).
 */
#define gdg_get_ending_frame_ticks() ( VIDEO_SCREEN_TICKS )

/**
 * @brief Tiky od poslední sestupné hrany skutečného HSYNC (CLK1 čítače 8253).
 *
 * @param ticks Tiky od začátku snímku.
 * @return Počet tiků od hrany, 0 až VIDEO_SCREEN_WIDTH - 1.
 */
#define gdg_get_ticks_since_hsync_fall(ticks) \
    ( ( VIDEO_GET_SCREEN_COL(ticks) + VIDEO_SCREEN_WIDTH - VIDEO_REAL_HSYNC_START_COLUMN ) % VIDEO_SCREEN_WIDTH )

/**
 * @brief Čas události aktuálního řádku v tikách od začátku snímku.
 *
 * @param column Sloupec události z tabulky g_gdgevent.
 * @return Tiky od začátku snímku (řádek má pevnou délku).
 */
#define gdg_get_event_ticks(column) ( g_gdg.beam_row * VIDEO_SCREEN_WIDTH + (column) )

/** @brief Sloupec paprsku v aktuálním čase GDG (řádek má pevnou délku). */
#define gdg_get_beam_col() ( VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks) )

/**
 * @brief Virtuální pozice paprsku (řádek * VIDEO_SCREEN_WIDTH + sloupec).
 *
 * Na této platformě má řádek pevnou délku, pozice je rovna tikům od začátku
 * snímku. Na MZ-800 ji mění bit CKSW (mz800_gdg.h).
 */
#define gdg_get_beam_vpos(ticks) ( ticks )
/**
 * @brief Okamžik právě probíhajícího přístupu na sběrnici (IORQ, MMIO) v tikách GDG.
 *
 * Callback sběrnice nejdřív zavolá mzarch_main_insideop_*(), která posune
 * g_gdg.total_elapsed.ticks na aktuální T-stav instrukce (cpu->op_tstate,
 * u PSG a VRAM včetně čekacích stavů) a stejný posun zapíše do
 * g_mzarch_main.instruction_insideop_sync_ticks. Čas přístupu je proto
 * přímo g_gdg.total_elapsed.ticks. Dřívější přičtení
 * instruction_insideop_sync_ticks započítalo provedenou část instrukce
 * dvakrát a datovalo zápisy do CTC, PIO 8255 (beeper) a DMD o tuto část
 * později (u přístupu v posledním M-cyklu až za konec instrukce).
 *
 * Mezi instrukcemi (drain fronty debuggeru, dbgapi) je posun nulový
 * a výsledkem je konec předchozí instrukce.
 *
 * @return Tiky od začátku aktuálního snímku (bez g_gdg.total_elapsed.screens;
 *         na celkový čas převádí gdg_compute_total_ticks()).
 * @pre Volat z emu vlákna.
 */
#define gdg_get_insigeop_ticks() (g_gdg.total_elapsed.ticks)

/**
 * @brief Fáze sestupné hrany CLK0 obvodu 8253 vůči času emulátoru (viz MZ-800).
 *
 * Na této platformě neměřeno [neověřeno]: hrana tam, kde
 * gdg_compute_total_ticks() % GDGCLK_CTC0_DIVIDER == 0 (stav před opravou
 * na MZ-800). Rozhodne měření U21 (mz800-knowledge hw-measurement/vysledky.md).
 */
#define GDG_CTC0_CLK_PHASE_TICKS 0u

/**
 * @brief Čas první sestupné hrany CLK0 ostře po @p now_ticks (tiky snímku).
 * @param now_ticks Tiky od začátku snímku.
 */
#define gdg_proximate_clk1m1_event(now_ticks) (now_ticks + (GDGCLK_CTC0_DIVIDER - ((gdg_compute_total_ticks(now_ticks) + GDGCLK_CTC0_DIVIDER - GDG_CTC0_CLK_PHASE_TICKS) % GDGCLK_CTC0_DIVIDER)))
#define gdg_get_event_pointer() (&g_gdg.event)
#define gdg_get_regct53g7() (g_gdg.regct53g7)

#ifdef MZ800EMU_CFG_CLK1M1_FAST
#define gdg_1m1_on_screen_done_event()
#else
#define gdg_1m1_on_screen_done_event() {g_gdg.ctc0clk++;}
#endif

/**
 * @brief Uzavře snímek: zvýší čítač snímků a odečte délku snímku od tiků.
 *
 * Volá se jen z gdg_process_events() při zpracování události konce
 * posledního řádku. V tu chvíli platí
 * g_gdg.total_elapsed.ticks >= VIDEO_SCREEN_TICKS (událost má ticks ==
 * VIDEO_SCREEN_TICKS). Pokud by to neplatilo (konec snímku zpracovaný
 * dvakrát), unsigned odečet by přetekl na ~2^32 a emulace by minuty
 * zpracovávala události bez instrukcí. Pojistka to nahlásí přes
 * mzarch_main_report_screen_done_underflow() a tiky srovná na 0.
 * Kontrola je jednou za snímek, ne v per-instruction hot path.
 */
#define gdg_on_screen_done_event()                       \
    {                                                    \
        g_gdg.total_elapsed.screens++;                   \
        g_mzarch_main.cursor_timer++;                          \
        if (g_gdg.total_elapsed.ticks >= VIDEO_SCREEN_TICKS) \
        {                                                \
            g_gdg.total_elapsed.ticks -= VIDEO_SCREEN_TICKS; \
        }                                                \
        else                                             \
        {                                                \
            mzarch_main_report_screen_done_underflow(g_gdg.total_elapsed.ticks); \
            g_gdg.total_elapsed.ticks = 0;               \
        }                                                \
        g_gdg.beam_row = 0;                              \
        gdg_1m1_on_screen_done_event();                  \
    }

    static inline void gdg_get_timestamp(st_GDG_TIMESTAMP *tm)
    {
        tm->screens = g_gdg.total_elapsed.screens;
        tm->ticks = g_gdg.total_elapsed.ticks;
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ1500_GDG_H */
