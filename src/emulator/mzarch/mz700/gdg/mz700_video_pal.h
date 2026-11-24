/*
 * File:   mz700_video_pal.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 19. června 2015, 15:37
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

#ifndef MZ700_VIDEO_PAL_H
#define MZ700_VIDEO_PAL_H

#ifdef __cplusplus
extern "C" {
#endif


    /*
     *
     * 	Generator video signalu Sharp MZ-700 RGBI PAL (zakladni nastaveni pro Evropu):
     *
     *
     *  Master CLK = 17 734 475 Hz, T = 1 / master = 56,39 ns.
     *
     *  Měření na skutečném MZ-700 PAL logickým analyzátorem OMEGA (2026-11-24,
     *  emu-experiments mz700-meas, VYSLEDKY2.md a VYSLEDKY3.md; HS, VS a barvy
     *  na obrazovém konektoru, hodiny na vývodu obvodu videa):
     *  - řádek 1136 T, HS v nule 81 T [změřeno]
     *  - obraz 338 až 977 T po sestupné hraně HS (639 T) [změřeno]
     *  - snímek 312 řádků, VS v nule 3 řádky, obraz na řádcích 68 až 267
     *    počítáno od řádku, ve kterém VS začíná [změřeno]
     *  Starší měření z 2026-05-08 (řádek 1136 T, HACTIVE 639 T) s tím souhlasí;
     *  při něm vyhořel zdroj počítače a hodnoty byly do 2026-11-24 interpolované
     *  z měření MZ-1500 a MZ-800.
     *
     *  Borderové rozměry převzaté z MZ-1500 (32/32/16/16) [neověřeno];
     *  stroj sám žádný border nemá.
     *
     *  Vertikální timing odvozen z MZ-800 PAL (VSYNC pozice rows 270-311)
     *  [s měřením VS z 2026-11-24 nesrovnáno].
     *
     *
     * 	Radkove casovani:
     *	=================
     *
     * 	Hsync:		   81 T
     * 	Back Porch:	  225 T
     * 	Video Enable:	  704 T  (display = 32 + 640 + 32)
     * 	Front Porch:	  126 T
     *  	-----------------------
     * 	Cely radek:	1 136 T  [overeno mereni]
     *
     *  Sum check: 81 + 225 + 704 + 126 = 1136
     *  Canvas začíná 338 T po sestupné hraně HS [změřeno]: 81 + 225 + 32 = 338.
     *
     *
     * 	Snimkove casovani:
     * 	==================
     *
     * 	Cely snimek:	354 432 T	( 312 radku, 50 Hz )
     *
     *
     * 	Horni border:	 16 radku   (z MZ-1500 [neovereno])
     * 	Screen:		200 radku
     * 	Dolni border:	 16 radku   (z MZ-1500 [neovereno])
     *
     *
     * 	Levy border:	 32 T   (z MZ-1500 [neovereno])
     * 	Screen:		640 T   [overeno mereni]
     * 	Pravy border:	 32 T   (z MZ-1500 [neovereno])
     *
     *
     *
     *
     *   +----------------------------- Screen ------------------------------+
     *   |                                                                   |
     *   |                                                                   |
     *   |                                                                   |
     *   |  0,0 -> +------------- Display (visible area) ----------+         |
     *   |  beam   |Top border                                     |         |
     *   |  start  |                                               |         |
     *   |  here   |                                               |         |
     *   |         | Left  +----------- Canvas ------------+ Right |         |
     *   |         | border|                               | border|         |
     *   |         |       |                               |       |         |
     *   |         |       |                               |       |         |
     *   |         |       |                               |       |         |
     *   |         |       |                               |       |         |
     *   |         |       |                               |       |         |
     *   |         |       |                               |       |         |
     *   |         |       +---------- 640 x 200 ----------+       |         |
     *   |         |Botom border                                   |         |
     *   |         |                                               |         |
     *   |         |                                               |         |
     *   |         +------------------ 704 x 232 ------------------+         |
     *   |                                                                   |
     *   |                                                                   |
     *   |                                                                   |
     *   +--------------------------- 1136 x 312 ----------------------------+
     *
     *
     *
     */


    /*
     *
     * Definice rozmeru
     *
     */

#define VIDEO_BORDER_LEFT_WIDTH     32  /* z MZ-1500 [neovereno] */
#define VIDEO_BORDER_RIGHT_WIDTH    32  /* z MZ-1500 [neovereno] */
#define VIDEO_BORDER_TOP_HEIGHT     16  /* z MZ-1500 [neovereno] */
#define VIDEO_BORDER_BOTOM_HEIGHT   16  /* z MZ-1500 [neovereno] */

    /* celkove rozmery uzivatelsky pouzitelne (kreslitelne) oblasti */
#define VIDEO_CANVAS_WIDTH          640  /* [overeno mereni HACTIVE = 639 T ≈ 640] */
#define VIDEO_CANVAS_HEIGHT         200  /* MZ-700 native */


    /* celkove rozmery viditelneho displeje */
#define VIDEO_DISPLAY_WIDTH         ( VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH + VIDEO_BORDER_RIGHT_WIDTH )
#define VIDEO_DISPLAY_HEIGHT        ( VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT + VIDEO_BORDER_BOTOM_HEIGHT )


#define VIDEO_BORDER_TOP_WIDTH      VIDEO_DISPLAY_WIDTH
#define VIDEO_BORDER_LEFT_HEIGHT    VIDEO_CANVAS_HEIGHT


#define VIDEO_BORDER_RIGHT_HEIGHT   VIDEO_CANVAS_HEIGHT
#define VIDEO_BORDER_BOTOM_WIDTH    VIDEO_DISPLAY_WIDTH


    /*
     * Definice obrazoveho radku
     * Změřeno na skutečném MZ-700 PAL (2026-11-24, emu-experiments mz700-meas,
     * VYSLEDKY3.md): HS v nule 81 T, canvas začíná 338 T po sestupné hraně HS.
     * S levým okrajem 32 T vychází back porch 225 T a front porch 126 T.
     */
#define VIDEO_H_SYNC_TICKS          81  /* skutecny Hsync na video vystupu (soucast VIDEO_SCREEN_WIDTH; delka LOW urovne CLK1 pro ctc8253) [zmereno] */
#define VIDEO_H_BACK_PORCH_TICKS    225 /* canvas 338 T po hrane HS [zmereno] */
#define VIDEO_H_ENABLED_TICKS       VIDEO_DISPLAY_WIDTH  /* 704 T */
#define VIDEO_H_FRONT_PORCH_TICKS   126 /* zbytek radku */


    /* Celkove rozmery screen */
#define VIDEO_SCREEN_WIDTH          ( VIDEO_H_SYNC_TICKS + VIDEO_H_BACK_PORCH_TICKS + VIDEO_H_ENABLED_TICKS + VIDEO_H_FRONT_PORCH_TICKS )
/** Sloupec sestupné hrany skutečného HSYNC (= CLK1 čítače 8253, událost
 *  MZEVENT_GDG_REAL_HSYNC_START); HSYNC je pak LOW po VIDEO_H_SYNC_TICKS. */
#define VIDEO_REAL_HSYNC_START_COLUMN ( VIDEO_DISPLAY_WIDTH + VIDEO_H_FRONT_PORCH_TICKS )
#define VIDEO_SCREEN_HEIGHT         312  /* [zmereno 2026-11-24] */
#define VIDEO_SCREEN_TICKS          ( VIDEO_SCREEN_HEIGHT * VIDEO_SCREEN_WIDTH )



//#define VIDEO_V_SYNC_TICKS          ( 3 * VIDEO_SCREEN_WIDTH )
//#define VIDEO_V_BACK_PORCH_TICKS    ( 19 * VIDEO_SCREEN_WIDTH + 2 ) /* uz si fakt nejsem jisty, zda jsem tohle zmeril presne */
//#define VIDEO_V_ENABLED_TICKS       ( 287 * VIDEO_SCREEN_WIDTH )
//#define VIDEO_V_FRONT_PORCH_TICKS   ( 3 * VIDEO_SCREEN_WIDTH )


//#define BEAM_SCREEN_TICKS           ( VIDEO_V_SYNC_TICKS + VIDEO_V_BACK_PORCH_TICKS + VIDEO_V_ENABLED_TICKS + VIDEO_V_FRONT_PORCH_TICKS )



    /*
     *
     * Definice umisteni na ceste paprsku
     *
     */

#define VIDEO_BEAM_BORDER_TOP_FIRST_COLUMN      0
#define VIDEO_BEAM_BORDER_TOP_FIRST_ROW         0
#define VIDEO_BEAM_BORDER_TOP_LAST_COLUMN       ( VIDEO_DISPLAY_WIDTH - 1 )
#define VIDEO_BEAM_BORDER_TOP_LAST_ROW          ( VIDEO_BORDER_TOP_HEIGHT - 1 )

#define VIDEO_BEAM_CANVAS_FIRST_COLUMN          ( VIDEO_BORDER_LEFT_WIDTH )
#define VIDEO_BEAM_CANVAS_FIRST_ROW             ( VIDEO_BEAM_BORDER_TOP_LAST_ROW + 1 )
#define VIDEO_BEAM_CANVAS_LAST_COLUMN           ( VIDEO_BEAM_CANVAS_FIRST_COLUMN + VIDEO_CANVAS_WIDTH - 1 )
#define VIDEO_BEAM_CANVAS_LAST_ROW              ( VIDEO_BEAM_CANVAS_FIRST_ROW + VIDEO_CANVAS_HEIGHT - 1 )

#define VIDEO_BEAM_BORDER_LEFT_FIRST_COLUMN     0
#define VIDEO_BEAM_BORDER_LEFT_FIRST_ROW        ( VIDEO_BEAM_CANVAS_FIRST_ROW )
#define VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN      ( VIDEO_BORDER_LEFT_WIDTH - 1 )
#define VIDEO_BEAM_BORDER_LEFT_LAST_ROW         ( VIDEO_BEAM_CANVAS_LAST_ROW )

#define VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN    ( VIDEO_BEAM_CANVAS_LAST_COLUMN + 1 )
#define VIDEO_BEAM_BORDER_RIGHT_FIRST_ROW       ( VIDEO_BEAM_CANVAS_FIRST_ROW )
#define VIDEO_BEAM_BORDER_RIGHT_LAST_COLUMN     ( VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN + VIDEO_BORDER_RIGHT_WIDTH - 1 )
#define VIDEO_BEAM_BORDER_RIGHT_LAST_ROW        ( VIDEO_BEAM_CANVAS_LAST_ROW )

#define VIDEO_BEAM_BORDER_BOTOM_FIRST_COLUMN    0
#define VIDEO_BEAM_BORDER_BOTOM_FIRST_ROW       ( VIDEO_BEAM_CANVAS_LAST_ROW + 1 )
#define VIDEO_BEAM_BORDER_BOTOM_LAST_COLUMN     ( VIDEO_DISPLAY_WIDTH - 1 )
#define VIDEO_BEAM_BORDER_BOTOM_LAST_ROW        ( VIDEO_BEAM_BORDER_BOTOM_FIRST_ROW + VIDEO_BORDER_BOTOM_HEIGHT - 1 )

#define VIDEO_BEAM_DISPLAY_FIRST_COLUMN         0
#define VIDEO_BEAM_DISPLAY_FIRST_ROW            0
#define VIDEO_BEAM_DISPLAY_LAST_COLUMN          ( VIDEO_DISPLAY_WIDTH - 1 )
#define VIDEO_BEAM_DISPLAY_LAST_ROW             ( VIDEO_DISPLAY_HEIGHT - 1 )


#define VIDEO_BEAM_HBLN_FIRST_COLUMN            ( VIDEO_BEAM_CANVAS_LAST_COLUMN - 3 )

/**
 * @brief Okno čekání procesoru při přístupu do VRAM (v taktech GDGCLK).
 *
 * Změřeno na skutečném MZ-700 PAL (2026-11-24, emu-experiments mz700-meas,
 * VYSLEDKY3.md): každý přístup do VRAM (čtení i zápis, znaky i atributy), který padne
 * do okna, čeká do jeho konce - i první v řádku a i v řádcích mimo obraz. Mimo okno
 * se nečeká. Na stroji začíná /WAIT 1 takt po sestupné hraně /MREQ; čeká se, když
 * tento začátek padne do 241 až 976 taktů po sestupné hraně HS, a /WAIT končí
 * v 976,5 taktu (obraz 338 až 977).
 *
 * Hodnoty níže jsou v čase, ke kterému emulátor přístup počítá, a jsou seřízené tak,
 * aby smyčka s jedním přístupem na průchod dávala stejné délky průchodů jako stroj
 * (emu-experiments mz700-fixes, tools/calib.py): čeká průchod, jehož značka (začátek
 * /IORQ instrukce IN těsně před přístupem) je 203 až 933 taktů po hraně HS, a počet
 * čekacích taktů procesoru je ceil((934 - značka) / 5). Počet čekacích taktů je
 * změřený: z toho, o kolik později přijde na /WAIT krátký pulz dalšího zápisu
 * programu do paměti než v kontrolním úseku bez čekání (mz700-meas,
 * vram_wait_tstates.py; 4 624 z 4 627 průchodů, zbylé tři jsou zápisy znaku se
 * značkou 933, které čekaly 2 T místo 1 T). Do mz800new 4571f537 byl počet jen
 * odvozený z konce /WAIT a vycházel o 1 T menší (ceil((929 - značka) / 5)).
 * VIDEO_VRAMWAIT_FIRST_FROM_HSYNC je první takt okna od sestupné hrany HS,
 * VIDEO_VRAMWAIT_VRAM_TICKS délka okna. VIDEO_VRAMWAIT_TAIL_TICKS je počet taktů,
 * o které čekání končí před koncem okna: přístup v posledním taktu okna tak dostane
 * jeden čekací takt procesoru a přístup, kterému do konce zbývá celý násobek T,
 * nečeká o T déle (jádro přičítá k čekání vždy začatý takt procesoru).
 */
#define VIDEO_VRAMWAIT_FIRST_FROM_HSYNC         253
#define VIDEO_VRAMWAIT_VRAM_TICKS               731
#define VIDEO_VRAMWAIT_TAIL_TICKS               1
/** První sloupec okna čekání v řádku emulátoru. */
#define VIDEO_BEAM_VRAMWAIT_FIRST_COLUMN        ( ( VIDEO_REAL_HSYNC_START_COLUMN + VIDEO_VRAMWAIT_FIRST_FROM_HSYNC ) % VIDEO_SCREEN_WIDTH )

    /* VSYNC pozice odvozena z MZ-800 PAL [neovereno - bez VSYNC mereni] */
#define VIDEO_BEAM_VSYNC_FIRST_ROW              270
#define VIDEO_BEAM_VSYNC_LAST_ROW               311


#define VIDEO_SCREENS_PER_SEC                   50



#define VIDEO_GET_SCREEN_ROW( screen_ticks ) ( screen_ticks / VIDEO_SCREEN_WIDTH )
#define VIDEO_GET_SCREEN_COL( screen_ticks ) ( screen_ticks % VIDEO_SCREEN_WIDTH )

#define VIDEO_GET_DISPLAY_ADDR( display_row, display_col ) ( ( display_row * VIDEO_DISPLAY_WIDTH ) + display_col )


#ifdef __cplusplus
}
#endif

#endif /* MZ700_VIDEO_PAL_H */
