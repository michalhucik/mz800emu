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
#include "hw-generic/gdg/gdg_state.h"
#include "mzarch/mzarch.h"
#include "hw-generic/gdg/gdgclk.h"


// Tohle jsem meril jenom na MZ-800, nicmene ty rozdilz meyi platformami budou asi marginalni, proto jsem to pouzil globalne pro vsechny MZ-architektury.
/* Delky sbernicovych pulzu - vycleneno do vlastniho souboru (mzhal
 * krok 6), odsud se plni i g_mzhal. */
#include "mz700_bus_timing.h"

/* Realny MZ-700 nema zadny DMD registr ani paletu - porty 0xF0/0xF1
 * nedekoduje. Pipeline je trvale v rezimu MZ-700 -> predikat osy DMD
 * je konstanta a kompilator mrtve vetve slozi. */
#define GDG_DMD_TEST_MODE700 (1)

/* SIGNAL_GDG_*, HBLN/VBLN/HSYN/VSYN konstanty, externy g_gdg /
 * g_gdgevent, GDG_TEST_VBLN a prototypy gdg_* jsou sdílené
 * v hw-generic/gdg/gdg_state.h (mzhal 11c-2b). */

    /* st_GDGEVENT / st_GDG_TIMESTAMP / st_GDG: superset layout
     * spolecny vsem architekturam - viz hw-generic/gdg/gdg_state.h
     * (mzhal 9c-2, per-arch typedefy smazany ve stejnem commitu). */


/* gdg_compute_total_ticks / gdg_get_total_ticks /
 * gdg_proximate_clk1m1_event: runtime static inline
 * v hw-generic/gdg/gdg_state.h (mzhal 10d). Zde zbyva jen
 * insigeop varianta (zadna platformni konstanta). */
#define gdg_get_insigeop_ticks() (g_gdg.total_elapsed.ticks + g_mzarch_main.instruction_insideop_sync_ticks)
#define gdg_get_event_pointer() (&g_gdg.event)
#define gdg_get_regct53g7() (g_gdg.regct53g7)


#define gdg_on_screen_done_event()                       \
    {                                                    \
        g_gdg.total_elapsed.screens++;                   \
        g_mzarch_main.cursor_timer++;                          \
        g_gdg.total_elapsed.ticks -= VIDEO_SCREEN_TICKS; \
        g_gdg.beam_row = 0;                              \
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
