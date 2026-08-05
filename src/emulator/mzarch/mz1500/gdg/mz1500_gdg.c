/*
 * File:   mz1500_gdg.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:32
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

/*
 *
 *	MZ-1500 GDG registry:
 *
 *	DMD (port 0xF0):
 *		bit 0: MODE — 0 = MZ-700, 1 = MZ-1500 (PCG)
 *		bit 1: PRIORITY — 0 = BPF, 1 = BFP
 *
 *	Barevna paleta (port 0xF1):
 *		bity 4-6: index barvy (0-7)
 *		bity 0-2: barva (0-7)
 *
 *	CTC0 GATE (MEMOP 0xE008):
 *		bit 0: GATE signal pro CTC kanal 0
 *
 */

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mz1500_gdg.h"
#include "mz1500_vramctrl.h"
#include "mz1500_hwscroll.h"
#include "mz1500_framebuffer.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/joy/joymz-1x03.h"
#include "mzarch/mzarch.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

// nedelej framebuffer
// #define framebuffer_border_changed()
// #define framebuffer_MZ800_screen_changed()

st_GDG g_gdg __attribute__((aligned(64)));

/* Eventy musi byt serazeny vzestupne podle event_column ! */
const struct st_GDGEVENT g_gdgevent[] = {

    /* row: ALL, col: 150 */
    {MZEVENT_GDG_HBLN_END, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_CANVAS_FIRST_COLUMN - 4},

    /* row: ALL, col: 790 */
    /* + row: 45, col: 790 - VBLN_END */
    /* + row: 245, col: 790 - VBLN_START */
    {MZEVENT_GDG_HBLN_START, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_HBLN_FIRST_COLUMN},

    /* row: 0, col: 792 */
    {MZEVENT_GDG_STS_VSYNC_END, 0, 1, VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2},

    /* row: 287, col: 792 */
    {MZEVENT_GDG_STS_VSYNC_START, VIDEO_DISPLAY_HEIGHT - 1, 1, VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2},

    /* row: 46 - 245, col: 794 */
    {MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL, VIDEO_BEAM_CANVAS_FIRST_ROW, VIDEO_CANVAS_HEIGHT, VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN},

    /* row: ALL, col: 928 - STS_HSYNC start (z duvodu uspory ho spustime trochu drive ) */
    /* row: 0 - 287, col: 928 */
    //{ MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL, 0, DISPLAY_VISIBLE_HEIGHT, DISPLAY_VISIBLE_LAST_COLUMN + 1 },
    {MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1},

    /* row: ALL, col: 926 - podle mych mereni zacina zde */
    //{ MZEVENT_GDG_STS_HSYNC_START, 0, BEAM_TOTAL_ROWS, 926 },

    /* row: ALL, col: VIDEO_DISPLAY_WIDTH + VIDEO_H_FRONT_PORCH_TICKS = 704 + 39 = 743 */
    {MZEVENT_GDG_REAL_HSYNC_START, 0, VIDEO_SCREEN_HEIGHT, VIDEO_DISPLAY_WIDTH + VIDEO_H_FRONT_PORCH_TICKS},

    /* row: ALL, col: 1133 - podle mych mereni konci zde */
    //{ MZEVENT_GDG_STS_HSYNC_END, 0, BEAM_TOTAL_ROWS, 1133 },

    /* row: ALL, col: 1135 STS_HSYNC end (z duvodu uspory jej ukoncime malinko pozdeji) */
    /* row: ALL, col: 1135 */
    {MZEVENT_GDG_SCREEN_ROW_END, 0, VIDEO_SCREEN_HEIGHT, VIDEO_SCREEN_WIDTH},
};

void gdg_init(void)
{
    /* Redzone poison superset st_GDG (mzhal 9c-3). */
    gdg_redzone_fill(&g_gdg);

    g_gdg.total_elapsed.ticks = 0;
    g_gdg.total_elapsed.screens = 0;

    g_gdg.event.event_name = 0;
    g_gdg.event.ticks = g_gdgevent[g_gdg.event.event_name].event_column;

    g_gdg.hbln = HBLN_ACTIVE;
    g_gdg.vbln = VBLN_ACTIVE;

    g_gdg.sts_hsync = HSYN_OFF;
    g_gdg.sts_vsync = VSYN_ACTIVE;

    g_gdg.beam_row = 0;
    g_gdg.screen_is_already_rendered_at_beam_pos = 0;

    g_gdg.screen_need_update_from = 0;
    g_gdg.last_updated_border_pixel = 0;

    g_gdg.tempo_divider = 0;
    g_gdg.tempo = 0;

    g_gdg.regct53g7 = 0;

    hwscroll_init();
    framebuffer_init();

    g_vramctrl.mz700_wr_latch_is_used = 0;

}

static inline void gdg_set_regDMD(uint8_t value, unsigned event_ticks)
{
    g_gdg.regDMD = value & 0x03;
    /* MZ-1500: mode700 = !(bit 0) */
    int mode700 = !(value & REGISTER_DMD_FLAG_MZ1500_MODE);
    ctc82530_on_regDMD_changed(mode700, event_ticks);
    if (!mode700)
    {
        g_vramctrl.mz700_wr_latch_is_used = 0;
    };
}

void gdg_reset(void)
{
    /* Detekce korupce sousednich sekci supersetu (mzhal 9c-3);
     * v debug buildu tvrdy assert, v release jen log. */
    if (!gdg_redzone_check(&g_gdg)) {
        fprintf(stderr, "GDG: redzone corrupted (detected at reset)\n");
        assert(0 && "GDG redzone corrupted");
    }
    g_gdg.regct53g7 = 0;
    gdg_set_regDMD(0, 0); /* MZ-1500: reset = MZ-700 rezim (bit 0 = 0) */

    /* MZ-1500 barevna paleta — vychozi hodnoty */
    memset(g_gdg.mode_color, 0, sizeof(g_gdg.mode_color));

    g_gdg.regBOR = 0; /* MZ-1500 nema border port, vzdy cerny */

    vramctrl_reset();
    hwscroll_reset();

    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
}

uint8_t gdg_read_dmd_status_ioop(void)
{
    /* MZ-1500: port 0xCE neni pristupny pres IORQ — vracime DBUS latch */
    return g_mzarch_main.regDBUS_latch;
}

uint8_t gdg_read_dmd_status_memop(void)
{
    /* MZ-1500: cteni statusu pres MEMOP (0xE008), obsah je stejný jako u MZ-700.
     * Bity 1-4 default HIGH (= no joystick / released / pulse done). */
    uint8_t retval = 0x1E;
    retval |= SIGNAL_GDG_HBLNK ? (1 << 7) : 0x00;
    retval |= SIGNAL_GDG_TEMPO;
    /* MZ-1X03 joystick: pouze pokud je alespon jeden joy device aktivni v
     * iface_joy konfiguraci. Makro JOYMZ_TEST_ANY_ACTIVE compile-out na 0
     * kdyz zadny joystick neni nakonfigurovany. */
    if ( JOYMZ_TEST_ANY_ACTIVE ) {
        retval = (retval & ~0x1E)
               | (joymz_get_status_bits14( gdg_get_total_ticks() ) & 0x1E);
    }
    return retval;
}

void gdg_write_byte(unsigned addr, uint8_t value)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat write do MZ-1500 GDG.
     *
     * MZ-1500 GDG má jen 3 porty (0x08 CTC0 GATE, 0xF0 DMD, 0xF1 paleta).
     * Klasifikace per port (per HW-log_format_CZ.md analogicky mz800).
     */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        unsigned low = addr & 0xff;
        uint8_t payload[ 6 ] = {
            (uint8_t)( addr & 0xff ),
            (uint8_t)( ( addr >> 8 ) & 0xff ),
            value, 0, 0, 0
        };
        if ( low == 0xf0 ) {
            hwlog_record ( HWLOG_CHIP_GDG_MODE, 0, payload );
        } else if ( low == 0xf1 ) {
            /* MZ-1500 paleta: bity 4-6 = index, bity 0-2 = barva. */
            hwlog_record ( HWLOG_CHIP_GDG_COLORS, HWLOG_GDG_COLORS_PAL, payload );
        } else if ( low == 0x08 ) {
            /* CTC0 GATE register - logujeme jako GDG_MODE s odlišným
             * addr_low (= 0x08) pro rozlišení od DMD (= 0xF0). */
            hwlog_record ( HWLOG_CHIP_GDG_MODE, 0, payload );
        }
    }
    /* HWE - HW event BP hooks (mode + palette).
     * MZ-1500 nema separatni palgrp/border (= 8-color paleta jednoduse,
     * border je pevne cerny). Nove BP_EVENT_GDG_PALGRP_CHANGE a
     * BP_EVENT_GDG_BORDER_CHANGE proto v MZ-1500 nikdy nefire. */
    {
        unsigned low = addr & 0xff;
        if ( low == 0xf0 || low == 0x08 ) {
            if ( g_bp_event_active[ BP_EVENT_GDG_MODE_CHANGE ] ) {
                bp_event_fire ( BP_EVENT_GDG_MODE_CHANGE, (int32_t) value );
            }
        } else if ( low == 0xf1 ) {
            if ( g_bp_event_active[ BP_EVENT_GDG_PALETTE_CHANGE ] ) {
                bp_event_fire ( BP_EVENT_GDG_PALETTE_CHANGE, (int32_t) value );
            }
        }
    }
#endif

    switch (addr & 0xff)
    {

    case 0x08:
        /* zapis na status registr 0xE008 — CTC0 GATE */
        value = value & 0x01;
        if (value != g_gdg.regct53g7)
        {
            g_gdg.regct53g7 = value;
            ctc8253_gate(0, value, gdg_get_insigeop_ticks());
        };
        break;

    case 0xf0:
        /* MZ-1500: Display Mode registr — bit 0 = MODE, bit 1 = PRIORITY */
        value = value & 0x03;
        if (g_gdg.regDMD != value)
        {
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
            gdg_set_regDMD(value, gdg_get_insigeop_ticks());
        };
        break;

    case 0xf1:
    {
        /* MZ-1500: barevna paleta — bity 4-6 = index (0-7), bity 0-2 = barva (0-7) */
        int i = (value >> 4) & 0x07;
        int color = value & 0x07;
        if (g_gdg.mode_color[i] != color)
        {
            g_gdg.mode_color[i] = color;
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
        };
        break;
    }

    default:
        break;
    };
}
