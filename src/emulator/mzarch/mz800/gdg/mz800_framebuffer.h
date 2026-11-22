/*
 * File:   mz800_framebuffer.h
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

#ifndef MZ800_FRAMEBUFFER_H
#define MZ800_FRAMEBUFFER_H
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
extern void framebuffer_update_MZ700_all_rows(void);
extern void framebuffer_MZ800_screen_changed(void);

/**
 * @brief O kolik tiků před koncem M-cyklu zápisu na 0F0h se změní paleta na obraze.
 *
 * Na skutečném MZ-800 se nová barva objeví 3,5 nebo 4,5 px po sestupné hraně
 * /IORQ, v rastru 2 px (měření analyzátorem, emu-experiments gdg-meas, první
 * měření: 1717 přechodů), tedy 11,5 nebo 10,5 px před koncem M-cyklu.
 * V mřížce emulátoru je to 12 nebo 11 sloupců (bod režimu 320 má 2 sloupce).
 */
#define GDG_PALETTE_WR_LEAD_TICKS 11u

/**
 * @brief Předstih přepnutí rozlišení (DMD, 320 <-> 640) před koncem M-cyklu zápisu na 0CEh.
 *
 * Na skutečném MZ-800 se režim přepne až na nejbližší hranici 16 px (hranice
 * bajtu režimu 320), která leží aspoň 5,5 px po sestupné hraně /IORQ, bez
 * přechodového jevu (měření analyzátorem, emu-experiments gdg-meas, první
 * měření: 1441 zápisů uvnitř canvasu). V mřížce emulátoru je to první
 * hranice 16 sloupců canvasu nejvýš tolik sloupců před koncem M-cyklu.
 */
#define GDG_DMD_WR_LEAD_TICKS 10u

/**
 * @brief Předstih zápisu registrů scrollu (01CFh až 05CFh) před koncem M-cyklu.
 *
 * Nový offset platí od nejbližší hranice 16 px, která na skutečném MZ-800
 * leží 11,5 až 26,5 px po sestupné hraně /IORQ (měření analyzátorem,
 * emu-experiments gdg-meas, první měření: 1363 zápisů offsetu). Emulátor bral
 * hranici o 2 sloupce později. Změřen je jen offset (SOF); pro SW, SSA a SEA
 * se používá stejný předstih [neověřeno].
 */
#define GDG_SCROLL_WR_LEAD_TICKS 2u

/**
 * @brief Začátek zápisu do registru GDG, který mění obraz canvasu (režim MZ-800).
 *
 * Volá se před změnou registru; čas GDG je konec M-cyklu zápisu. Místo změny
 * na řádku leží @p lead sloupců před koncem M-cyklu, při @p align16 zaokrouhlené
 * nahoru na hranici 16 sloupců canvasu. Aktuální řádek dokreslí po místo
 * změny se starou hodnotou registru. Když je řádek už nakreslený dál (uzavřený
 * řádek, dokreslení po paprsek při pauze), vrátí kreslení na místo změny
 * a zbytek nechá překreslit funkci framebuffer_MZ800_reg_write_end().
 *
 * @param lead    Předstih místa změny před koncem M-cyklu (sloupce).
 * @param align16 Nenulové = místo změny na nejbližší další hranici 16 sloupců.
 * @pre Emu vlákno; registr drží starou hodnotu; režim MZ-800 (DMD bit 3 = 0).
 * @post g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED. Po změně
 *       registru musí následovat framebuffer_MZ800_reg_write_end().
 */
extern void framebuffer_MZ800_reg_write_begin(unsigned lead, int align16);

/**
 * @brief Konec zápisu do registru GDG: překreslí část řádku za místem změny.
 *
 * Volá se po změně registru. Dělá něco jen tehdy, když
 * framebuffer_MZ800_reg_write_begin() vrátila kreslení zpět: tehdy dokreslí
 * řádek s novou hodnotou registru tam, kam byl nakreslený předtím.
 *
 * @pre Emu vlákno; předcházelo framebuffer_MZ800_reg_write_begin().
 */
extern void framebuffer_MZ800_reg_write_end(void);

extern void framebuffer_border_current_row_fill(void);
extern void framebuffer_border_all_rows_fill(void);
/**
 * @brief Dokreslí border aktuálního řádku starou barvou až po paprsek.
 *
 * Pro místa, kde se obraz má srovnat s časem GDG beze změny barvy (návrat
 * z kroku debuggeru). Pro zápis barvy slouží framebuffer_border_write().
 *
 * @pre Emu vlákno.
 * @post g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED.
 */
extern void framebuffer_border_changed(void);

/**
 * @brief Zápis nové barvy borderu: dokreslí aktuální řádek po místo změny.
 *
 * Volá se z obsluhy zápisu na port 06CFh před změnou g_gdg.regBOR. Čas GDG
 * (g_gdg.total_elapsed.ticks) je konec M-cyklu zápisu. Na skutečném MZ-800
 * se barva na výstupu RGBI změní už během zápisového pulzu, 1,5 nebo 2,5 px
 * po sestupné hraně /IORQ a jen na sudém px od hrany HSYNC (měření
 * analyzátorem, emu-experiments gdg-meas, první měření: 1846 přechodů).
 * V mřížce emulátoru je to sudý ze sloupců o GDG_BORDER_WR_LEAD_TICKS
 * a GDG_BORDER_WR_LEAD_TICKS + 1 před koncem M-cyklu.
 *
 * Do místa změny nakreslí starou barvu. Když je řádek ve framebufferu už
 * uzavřený (konec viditelné části se zpracoval mezi místem změny a koncem
 * M-cyklu), přebarví border od místa změny novou barvou.
 *
 * @param new_value Nová barva borderu (IGRB, 0-15).
 * @pre Emu vlákno; g_gdg.regBOR ještě drží starou barvu.
 * @post g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;
 *       g_gdg.last_updated_border_pixel posunutý na místo změny.
 */
extern void framebuffer_border_write(unsigned new_value);

/**
 * @brief O kolik tiků před koncem M-cyklu zápisu na 06CFh se změní barva borderu.
 *
 * Změna padne na sudý sloupec: o tuto hodnotu, nebo o 1 víc. Viz
 * framebuffer_border_write().
 */
#define GDG_BORDER_WR_LEAD_TICKS 13u

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

#endif /* MZ800_FRAMEBUFFER_H */
