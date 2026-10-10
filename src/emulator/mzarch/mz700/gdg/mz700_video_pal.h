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
     *  Mereni STF (logicky analyzator SIGMA/OMEGA, 2026-05-08):
     *  - HSYNC perioda 1136 T = 64,07 us = PAL line rate 15625 Hz [overeno]
     *  - HACTIVE (canvas enable) HIGH = 639 T ≈ 640 T = canvas width [overeno]
     *  - VSYNC nebyl pripojen na zadny ze 7 vstupu - nelze overit
     *
     *  Borderove rozmery prevzate z MZ-1500 (32/32/16/16) [neovereno];
     *  Visible display (704 x 232) je symetricky umisten v 1136 T radku.
     *
     *  Vertikalni timing odvozen z MZ-800 PAL (VSYNC pozice rows 270-311)
     *  [neovereno - bez VSYNC mereni].
     * 
     * V průběhu měření vyhořel zdroj počítače, takže jsem musel hodnoty interpolovat z měření MZ-1500 a MZ-800.
     *
     *
     * 	Radkove casovani:
     *	=================
     *
     * 	Hsync:		   80 T
     * 	Back Porch:	  136 T
     * 	Video Enable:	  704 T  (display = 32 + 640 + 32)
     * 	Front Porch:	  216 T
     *  	-----------------------
     * 	Cely radek:	1 136 T  [overeno mereni]
     *
     *  Sum check: 80 + 136 + 704 + 216 = 1136 ✓
     *  Symetrie: HSync+BP (216 T) = FP (216 T) → display vystreden
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
     * Display (704 T) je symetricky vystreden v 1136 T radku:
     * 1136 - 704 = 432 T → 216 T pred i po display.
     * HSync 80 T (z MZ-800 PAL [neovereno]), zbytek do back porch.
     */
#define VIDEO_H_SYNC_TICKS          80  /* skutecny Hsync na video vystupu (soucast VIDEO_SCREEN_WIDTH; delka LOW urovne CLK1 pro ctc8253) */
#define VIDEO_H_BACK_PORCH_TICKS    136 /* = 216 - HSync = symetricka mezera pred display */
#define VIDEO_H_ENABLED_TICKS       VIDEO_DISPLAY_WIDTH  /* 704 T */
#define VIDEO_H_FRONT_PORCH_TICKS   216 /* = HSync + BP = symetricka mezera za display */


    /* Celkove rozmery screen */
#define VIDEO_SCREEN_WIDTH          ( VIDEO_H_SYNC_TICKS + VIDEO_H_BACK_PORCH_TICKS + VIDEO_H_ENABLED_TICKS + VIDEO_H_FRONT_PORCH_TICKS )
/** Sloupec sestupné hrany skutečného HSYNC (= CLK1 čítače 8253, událost
 *  MZEVENT_GDG_REAL_HSYNC_START); HSYNC je pak LOW po VIDEO_H_SYNC_TICKS. */
#define VIDEO_REAL_HSYNC_START_COLUMN ( VIDEO_DISPLAY_WIDTH + VIDEO_H_FRONT_PORCH_TICKS )
#define VIDEO_SCREEN_HEIGHT         312  /* PAL standard [neovereno - bez VSYNC mereni] */
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
