/*
 * File:   mz700_framebuffer.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 5. července 2015, 8:43
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

#ifndef MZ700_FRAMEBUFFER_H
#define MZ700_FRAMEBUFFER_H
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "hw-generic/gdg/video.h"

typedef enum en_SCRSTS
{
    SCRSTS_IS_NOT_CHANGED = 0,
    SCRSTS_PREVIOUS_IS_CHANGED,
    SCRSTS_THIS_IS_CHANGED,
} en_SCRSTS;

typedef enum en_FBSTATE
{
    FB_STATE_NOT_CHANGED = 0,
    FB_STATE_SCREEN_CHANGED = (1 << 0),
    FB_STATE_BORDER_CHANGED = (1 << 1),
} en_FBSTATE;

#define GDG_FRAMEBUFFER_PIXBUF_COUNT 3
#define GDG_FRAMEBUFFER_PIXBUF_SIZE (VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT)

typedef struct gdg_framebuffer_t
{
    uint8_t pixbuff[GDG_FRAMEBUFFER_PIXBUF_COUNT][GDG_FRAMEBUFFER_PIXBUF_SIZE];
    uint8_t *pixels;
    int pixels_id;

    en_FBSTATE framebuffer_state; /**< nenulova hodnota, pokud je potreba zobrazit novy obsah framebufferu */
    en_SCRSTS screen_changes; /**< nenulova hodnota, pokud v tomto nebo predchozim snimku probehly zmeny */
    en_SCRSTS border_changes; /**< nenulova hodnota, pokud v tomto nebo predchozim snimku probehly zmeny */

    bool flag_20ms_passed;    /**< pokud prave nebezime v Normal speed, tak tento flag zajisti, ze iface_video provede aktualizaci snimku */
} gdg_framebuffer_t;

extern gdg_framebuffer_t g_framebuffer;

extern void framebuffer_init(void);

extern void framebuffer_MZ800_current_screen_row_fill(unsigned last_pixel);
extern void framebuffer_MZ800_all_screen_rows_fill(void);
extern void framebuffer_update_MZ700_current_screen_row(void);

/**
 * @brief Kreslení jednoho řádku plátna MZ-700.
 * @param beam_row Řádek paprsku (VIDEO_BEAM_CANVAS_FIRST_ROW .. VIDEO_BEAM_CANVAS_LAST_ROW).
 */
typedef void (*framebuffer_mz700_row_fn_t)(unsigned beam_row);

/**
 * @brief Vymění kreslení řádku plátna (adaptér devbus).
 *
 * Všechny cesty kreslení plátna MZ-700 (řádek po HBLN, řádek při DMD bit 0
 * = 1, plné překreslení, flush při CMT hacku) volají nastavenou funkci.
 *
 * @param fn Nová funkce; NULL = výchozí kreslení s g_rom.cgrom.
 * @pre Emu vlákno mezi instrukcemi, nebo zastavená emulace.
 */
extern void framebuffer_mz700_set_row_fn(framebuffer_mz700_row_fn_t fn);

/**
 * @brief Výchozí kreslení řádku s jiným generátorem znaků (zdroj glyfů devbus).
 * @param beam_row Řádek paprsku v rozsahu plátna.
 * @param cg       Generátor znaků 4 kB ve formátu CG-ROM.
 * @pre Emu vlákno.
 */
extern void framebuffer_mz700_row_with_cg(unsigned beam_row, const uint8_t *cg);
extern void framebuffer_update_MZ700_all_rows(void);
extern void framebuffer_MZ800_screen_changed(void);

extern void framebuffer_border_current_row_fill(void);
extern void framebuffer_border_all_rows_fill(void);
extern void framebuffer_border_changed(void);

extern void framebuffer_flush_full_screen(void);

#define framebudef_set_flag_20ms_passed() (g_framebuffer.flag_20ms_passed = true)
#define framebudef_clear_flag_20ms_passed() (g_framebuffer.flag_20ms_passed = false)
#define framebudef_get_flag_20ms_passed() (g_framebuffer.flag_20ms_passed)

#define framebuffer_get_state() (g_framebuffer.framebuffer_state)
#define framebuffer_get_screen_changes() (g_framebuffer.screen_changes)
#define framebuffer_get_border_changes() (g_framebuffer.border_changes)

#ifdef __cplusplus
}
#endif

#endif /* MZ700_FRAMEBUFFER_H */
