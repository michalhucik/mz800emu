/*
 * File:   mz800_gdg.h
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

#ifndef MZ800_GDG_H
#define MZ800_GDG_H

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

#define REGISTER_DMD_FLAG_MZ700 (1 << 3)
#define REGISTER_DMD_FLAG_SCRW640 (1 << 2)
#define REGISTER_DMD_FLAG_HICOLOR (1 << 1)
#define REGISTER_DMD_FLAG_VBANK (1 << 0)

#define GDG_MZ800_DMD_TEST_MZ700 (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700)
#define GDG_MZ800_DMD_TEST_SCRW640 (g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640)
#define GDG_MZ800_DMD_TEST_HICOLOR (g_gdg.regDMD & REGISTER_DMD_FLAG_HICOLOR)
#define GDG_MZ800_DMD_TEST_VBANK (g_gdg.regDMD & REGISTER_DMD_FLAG_VBANK)

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
        unsigned regBOR;    /* Border register */
        unsigned regPALGRP; /* Palette Group register */
        unsigned regPAL0;   /* Palette0 register */
        unsigned regPAL1;   /* Palette1 register */
        unsigned regPAL2;   /* Palette2 register */
        unsigned regPAL3;   /* Palette3 register */

        unsigned regct53g7; /* rizeni GATE pro CTC0 v ctc8253 v rezimu MZ700 */

        unsigned cksw;      /* CKSW (Superimpose) bit, set zapisem na 0xCF07 bit 7.
                             * Citelny v Status registru bit 2 (0xCE i 0xE008).
                             * Emulator hodnotu udrzuje, ale zmenu HSYN timingu
                             * (CKSW=1 zkracuje horizontalni periodu o 16 pxCLK)
                             * NEemulujeme - viz mz800-knowledge public/reference/agent/hw/08a-video-timing.md.
                             */

        unsigned tempo;
        unsigned tempo_divider;

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
        unsigned ctc0clk;
#endif

        /* Stav pro HDL-presny WAIT model 800 grafickych rezimu (DMD bit 3 = 0,
         * VRAM 0x8000-0xBFFF). Po WRITE do VRAM bezi "horka faze" delky
         * 32 CLK0 (320x200) nebo 17 CLK0 (640x200), behem ktere dalsi VRAM
         * pristup dostane WAIT podle lookup tabulek tw_wr[]/tw_ww[].
         *
         * vram800_hot_phase_end_total_ticks: kumulativni timestamp (gdg_total_ticks)
         *   konce horke faze (= T3f posledniho WRITE + 32 nebo 17 CLK0). Hodnota 0
         *   znamena "zadny predchozi WRITE / mimo horkou fazi".
         * vram800_hot_phase_clk0_phase: pozice prvniho WRITE v ramci radku (mod 80),
         *   index do tw_wr[]/tw_ww[]. Pro 640x200 se pri lookup pricte rotace +16.
         *
         * Reset: gdg_init() / gdg_reset() / snapshot load. Aktualizace: pri kazdem
         * WRITE z mzarch_main_insideop_mreq_mz800_vramctrl_write(). READ stav
         * neaktualizuje (READ negeneruje VRAM strobe). */
        uint64_t vram800_hot_phase_end_total_ticks;
        unsigned vram800_hot_phase_clk0_phase;

    } st_GDG;

    extern st_GDG g_gdg;

    extern const struct st_GDGEVENT g_gdgevent[];

#define GDG_TEST_VBLN (g_gdg.vbln == 0)

    extern void gdg_init(void);
    extern void gdg_reset(void);
    extern uint8_t gdg_read_dmd_status_memop(void);
    extern uint8_t gdg_read_dmd_status_ioop(void);
    extern void gdg_write_byte(unsigned addr, uint8_t value);

    /**
     * @brief Nastaví registr DMD stejnou cestou jako OUT (CEh), pro
     *        debugger.
     *
     * Provede totéž co větev CEh v gdg_write_byte() - maskování na dolní
     * 4 bity, nic při beze změny, aktualizace framebufferu do aktuální
     * pozice paprsku a gdg_set_regDMD(): ctc82530_on_regDMD_changed()
     * (GATE0 CTC0 = 1 v 800 módu, = regct53g7 v 700 módu), vynulování
     * g_vramctrl.mz700_wr_latch_is_used při přechodu do 800 módu
     * a přepočet RAM fast-path tabulky (MZ800EMU_CFG_RAM_FASTPATH).
     *
     * Na rozdíl od gdg_write_byte(0x00CE, value) NEzapíše záznam hwlog
     * (GDG_MODE) a NEspustí HW event breakpoint BP_EVENT_GDG_MODE_CHANGE -
     * zásah z debuggeru není OUT programu a nemá se v trace ani
     * v breakpointech tvářit jako OUT.
     *
     * Čas změny (event_ticks pro CTC0) = gdg_get_insigeop_ticks(); mezi
     * instrukcemi je instruction_insideop_sync_ticks = 0 (nuluje ho
     * hlavní smyčka mzarch po každé instrukci), takže jde o aktuální
     * g_gdg.total_elapsed.ticks.
     *
     * @param value Nová hodnota DMD (použijí se bity 0-3).
     *
     * @pre Volat jen z emu vlákna mezi instrukcemi (drain fronty dbgapi)
     *      nebo když emu vlákno neběží.
     * @post g_gdg.regDMD == (value & 0x0F).
     */
    extern void gdg_debug_set_regDMD(uint8_t value);

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

#endif /* MZ800_GDG_H */
