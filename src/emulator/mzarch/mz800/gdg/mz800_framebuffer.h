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

/**
 * @brief Nakreslí aktuální řádek canvasu v režimu MZ-700 (na konci HBLN).
 *
 * Nedělá nic, pokud řádek už nakreslilo přepnutí režimu uprostřed řádku
 * (g_gdg.mz700_skip_row == beam_row + 1).
 *
 * @pre Emu vlákno; aktuální řádek je v canvasu.
 */
extern void framebuffer_update_MZ700_current_screen_row(void);

/**
 * @brief Obraz aktuálního řádku při přepnutí DMD z režimu MZ-800 do MZ-700
 *        nebo zpět.
 *
 * Nový režim se na obraze projeví od bodu canvasu @p x (spočítá ho
 * gdg_write_regDMD() podle měření U7); body před ním zůstanou ve starém
 * režimu. Při přechodu do režimu MZ-700 se volá dvakrát: před změnou DMD
 * (@p done = 0, dokreslí grafiku po @p x) a po ní (@p done = 1, nakreslí
 * zbytek řádku v režimu MZ-700). Při přechodu z režimu MZ-700 stačí volání
 * s @p done = 0: nakreslí část řádku před @p x v režimu MZ-700 a grafiku od
 * @p x nechá na kreslení s paprskem (g_gdg.canvas_split_x).
 *
 * Část řádku v režimu MZ-700 se kreslí hned, s obsahem VRAM v okamžiku
 * zápisu DMD (zápis CPU do textové VRAM se na stroji ve viditelné části
 * řádku stejně projeví až od dalšího řádku).
 *
 * @param x Bod canvasu, od kterého platí nový režim (může být < 0 nebo
 *          >= VIDEO_CANVAS_WIDTH: celý řádek nový, resp. starý režim).
 * @param to700 Nenulové = přechod do režimu MZ-700.
 * @param done Nenulové = volání po změně registru DMD.
 * @pre Emu vlákno; čas GDG je konec M-cyklu zápisu DMD.
 * @post g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED.
 */
extern void framebuffer_mode700_switch(int x, int to700, int done);

/**
 * @brief Zapomene stav kreslení vázaný na jeden řádek rastru.
 *
 * Pomocný řádek režimu 640 (kam je nakreslený) a místo přepnutí 320 -> 640
 * platí jen v řádku, kde vznikly. Volá se na konci každého řádku (událost
 * MZEVENT_GDG_SCREEN_ROW_END) a po načtení snapshotu, aby stav nepřešel
 * na stejný řádek dalšího snímku.
 *
 * @pre Emu vlákno (nebo pozastavená emulace).
 * @post Další kreslení řádku v režimu 640 začne načtením od začátku bajtu,
 *       žádný řádek nemá zaznamenané přepnutí 320 -> 640.
 */
extern void framebuffer_row_state_reset(void);
extern void framebuffer_update_MZ700_all_rows(void);
/**
 * @brief Zápis CPU do VRAM v režimu MZ-800: dokreslí aktuální řádek po poslední buňku, kterou už video fetch přečetl.
 *
 * Volá se před vlastním zápisem; čas GDG je konec M-cyklu zápisu. Buňky do
 * meze FB_VRAM_WR_FETCHED_AHEAD sloupců za koncem M-cyklu se nakreslí se
 * starým obsahem (na stroji čte video fetch buňku asi 33,5 px před jejím
 * začátkem, měření U8), další buňky už ukážou nový.
 *
 * @pre Emu vlákno; režim MZ-800.
 * @post g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED.
 */
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
 * Registr DMD se na skutečném MZ-800 taktuje v 334 + 16k px a zápis převezme,
 * když sestupná hrana /IORQ P přišla nejpozději 1,5 px před hranou (jedno
 * pravidlo pro přepnutí rozlišení, režim MZ-700 i hrany řádku; viz
 * GDG_DMD_REG_WR_LEAD v mz800_gdg.c). Hranice bajtu režimu 320 (sloupec
 * 154 + 16k, 340 + 16k px) leží 6 px za hranou registru, přepnutí na ní tedy
 * platí pro konec M-cyklu nejvýš 8 sloupců za hranicí.
 *
 * Změřeno analyzátorem (emu-experiments gdg-meas): první měření, test 4,
 * 1441 zápisů - zpoždění od P po začátek přechodu -2,5 až 12,5 px; měření 8,
 * U19 - zápis s P = 622,5 px (0,5 px za hranou 622) se projeví až na další
 * hranici (5 z 5 průchodů). Do 2026-11-24 tu bylo 10: první měření jsem četl
 * jako "hranice 340 + 16k aspoň 5,5 px po P", hranice v něm ale byla 338 + 16k
 * (souřadnice režimu 640), a emulátor bral zápis o 2 px déle než stroj
 * (ověřeno stejným skriptem explore1.py nad během v emulátoru: -4,5 až 10,5).
 */
#define GDG_DMD_WR_LEAD_TICKS 8u

/**
 * @brief Předstih zápisu registrů scrollu (01CFh až 05CFh) před koncem M-cyklu.
 *
 * Nový offset platí od nejbližší hranice 16 px, která na skutečném MZ-800
 * leží 11,5 až 26,5 px po sestupné hraně /IORQ (měření analyzátorem,
 * emu-experiments gdg-meas, první měření: 1363 zápisů offsetu; v krajních
 * polohách 11,5 a 12,5 px asi pětina zápisů platí až od další hranice, tedy
 * 27,5 a 28,5 px - mez na stroji není ostrá, emulátor bere většinový případ).
 * Do 2026-11-24 tu bylo 2 a emulátor dával 12,5 až 27,5 px (stejný skript
 * explore1.py nad během v emulátoru), o 1 px později. Změřen je jen offset
 * (SOF); pro SW, SSA a SEA se používá stejný předstih (U4: SSA 11,5 až 28,5,
 * SEA 8,5 až 28,5, SW 11,5 až 26,5 px, hruběji).
 */
#define GDG_SCROLL_WR_LEAD_TICKS 3u

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

/**
 * @brief Body na rozhraní při přepnutí 320x200 / 640x200 uprostřed řádku.
 *
 * Volá se po framebuffer_MZ800_reg_write_end() u zápisu DMD, který mění jen
 * bit 2 (00h <-> 04h, 02h <-> 06h). Přepnutí platí v buňce k (hranice
 * 340 + 16k px, sloupec 154 + 16k, viz GDG_DMD_WR_LEAD_TICKS); podle C modelu
 * netlistu GDG (emu-experiments hw-fixes/research/sw640/SPEC.md) se mění
 * sloupce 148 + 16k až 153 + 16k:
 * - 640 -> 320: 148, 149 barva starého režimu pro bit 4 lichého bajtu o buňky
 *   k - 1 (rovina II, v 06h s rovinou IV), 150, 151 nový režim s bity
 *   (o5, o5, o3_5, o3_5), 152, 153 totéž s bitem 6. Úsek položky 3 je
 *   2 px za každý jedničkový bit o5 a o6 (U19: 00h / FFh 4 px, AAh / 55h
 *   2 px, sedí přesně). Model netlistu dává o jeden posun navíc; varianta
 *   bez něj sedí s měřením (výklad, že na čipu chybí hrana z multiplexeru
 *   hodin, je hypotéza);
 * - 320 -> 640: 148 zůstává, 149 a 150 bity 6 a 7 bajtu roviny I (v 06h
 *   s rovinou III) buňky k - 1 v barvách 640, 151 položka 1 (04h) nebo 3
 *   (06h), od 152 normální 640 buňky k (U19: přechod 2 px černá, 1 px
 *   modrá, sedí).
 * Frame B (bit 0) a přepnutí v první buňce canvasu neověřeno, beze změny.
 *
 * @param old_dmd DMD před zápisem.
 * @param new_dmd DMD po zápisu.
 * @param lead GDG_DMD_WR_LEAD_TICKS (jako u framebuffer_MZ800_reg_write_begin()).
 * @pre Emu vlákno; čas GDG je konec M-cyklu zápisu; řádek je dokreslený po místo přepnutí.
 */
extern void framebuffer_MZ800_resolution_switch(unsigned old_dmd, unsigned new_dmd, unsigned lead);

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
