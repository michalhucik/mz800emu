/*
 * File:   mz700_gdg.h
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

#ifndef MZ700_GDG_H
#define MZ700_GDG_H

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
     * MZ-700 DMD registr (port 0xF0):
     *   bit 0: MODE — 0 = MZ-700, 1 = MZ-700 (PCG)
     *   bit 1: PRIORITY — 0 = BPF (Background-PCG-Foreground), 1 = BFP (Background-Foreground-PCG)
     */
#define REGISTER_DMD_FLAG_MZ700_MODE  (1 << 0)
#define REGISTER_DMD_FLAG_MZ700_PRIO  (1 << 1)

    /* zachovavame nazev makra pro kompatibilitu s event handlerem — jen zmena semantiky */
#define GDG_MZ800_DMD_TEST_MZ700      (!(g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700_MODE))
#define GDG_MZ700_DMD_TEST_MODE700  (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700_MODE)
#define GDG_MZ700_DMD_TEST_PMODE_BFP (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700_PRIO)

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
        unsigned regBOR;    /* Border register — MZ-700 nema border port, vzdy 0 */

        unsigned regct53g7; /* rizeni GATE pro CTC0 v ctc8253 v rezimu MZ700 */

        int mode700_color[8]; /* MZ-700: barevna paleta (port 0xF1) */

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
    extern uint8_t gdg_read_dmd_status_ioop(void);
    extern void gdg_write_byte(unsigned addr, uint8_t value);

#define gdg_compute_total_ticks(now_ticks) (now_ticks + ((uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS))
#define gdg_get_total_ticks() gdg_compute_total_ticks(g_gdg.total_elapsed.ticks)
#define gdg_get_insigeop_ticks() (g_gdg.total_elapsed.ticks + g_mzarch_main.instruction_insideop_sync_ticks)
#define gdg_proximate_clk1m1_event(now_ticks) (now_ticks + (GDGCLK_CTC0_DIVIDER - (gdg_compute_total_ticks(now_ticks) % GDGCLK_CTC0_DIVIDER)))
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

#endif /* MZ700_GDG_H */
