/* 
 * File:   mz1500_video.h
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

#ifndef MZ1500_VIDEO_H
#define MZ1500_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif


    /*
     *
     * 	Generator video signalu Sharp MZ-1500 RGBI NTSC (zakladni nastaveni pro Japonsko):
     *
     *
     *
     * 	Radkove casovani:
     *	=================
     *
     * 	Hsync:		   65 T
     * 	Back Porch:	  ??? T
     * 	Video Enable:	  640 T
     * 	Front Porch:	  ??? T
     *  	-----------------------
     * 	Cely radek:	  912 T
     *
     *
     * 	Snimkove casovani:
     * 	==================
     *
     * 	Vsync:		  2 736 T	( 3  radky )
     * 	Back porch:	    ??? T	( 19 radku + 2 T ) ???
     * 	Video enable:	182 400 T	( 200 radku )
     * 	Front porch:	    ??? T	( 3 radky ) ???
     * 	---------------------------------------------------
     * 	Cely snimek:	238 944 T	( 262 radku )
     *
     *
     * 	Horni border:	 16 radku ???
     * 	Screen:		200 radku
     * 	Dolni border:	 16 radku ???
     *
     *
     * 	Levy border:	 32 T ???
     * 	Screen:		640 T
     * 	Pravy border:	 32 T ???
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
     *   +---------------------------- 912 x 262 ----------------------------+     
     * 
     * 
     * 
     */


    /*
     * 
     * Definice rozmeru
     * 
     */

#define VIDEO_BORDER_LEFT_WIDTH     32
#define VIDEO_BORDER_RIGHT_WIDTH    32
#define VIDEO_BORDER_TOP_HEIGHT     16
#define VIDEO_BORDER_BOTOM_HEIGHT   16

    /* celkove rozmery uzivatelsky pouzitelne (kreslitelne) oblasti */
#define VIDEO_CANVAS_WIDTH          640
#define VIDEO_CANVAS_HEIGHT         200


    /* celkove rozmery viditelneho displeje */
#define VIDEO_DISPLAY_WIDTH         ( VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH + VIDEO_BORDER_RIGHT_WIDTH )
#define VIDEO_DISPLAY_HEIGHT        ( VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT + VIDEO_BORDER_BOTOM_HEIGHT )


#define VIDEO_BORDER_TOP_WIDTH      VIDEO_DISPLAY_WIDTH
#define VIDEO_BORDER_LEFT_HEIGHT    VIDEO_CANVAS_HEIGHT


#define VIDEO_BORDER_RIGHT_HEIGHT   VIDEO_CANVAS_HEIGHT
#define VIDEO_BORDER_BOTOM_WIDTH    VIDEO_DISPLAY_WIDTH


    /* Definice obrazoveho radku */
#define VIDEO_H_SYNC_TICKS          65 /* skutecny Hsync na video vystupu (soucast VIDEO_SCREEN_WIDTH; delka LOW urovne CLK1 pro ctc8253) */
#define VIDEO_H_BACK_PORCH_TICKS    104
#define VIDEO_H_ENABLED_TICKS       VIDEO_DISPLAY_WIDTH
//#define VIDEO_H_FRONT_PORCH_TICKS   104
#define VIDEO_H_FRONT_PORCH_TICKS   39


    /* Celkove rozmery screen */
#define VIDEO_SCREEN_WIDTH          ( VIDEO_H_SYNC_TICKS + VIDEO_H_BACK_PORCH_TICKS + VIDEO_H_ENABLED_TICKS + VIDEO_H_FRONT_PORCH_TICKS )
/** Sloupec sestupné hrany skutečného HSYNC (= CLK1 čítače 8253, událost
 *  MZEVENT_GDG_REAL_HSYNC_START); HSYNC je pak LOW po VIDEO_H_SYNC_TICKS. */
#define VIDEO_REAL_HSYNC_START_COLUMN ( VIDEO_DISPLAY_WIDTH + VIDEO_H_FRONT_PORCH_TICKS )
#define VIDEO_SCREEN_HEIGHT         ( 16 + 200 + 16 + 30)
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

#define VIDEO_BEAM_VSYNC_FIRST_ROW              15
#define VIDEO_BEAM_VSYNC_LAST_ROW               247


#define VIDEO_SCREENS_PER_SEC                   60



#define VIDEO_GET_SCREEN_ROW( screen_ticks ) ( screen_ticks / VIDEO_SCREEN_WIDTH )
#define VIDEO_GET_SCREEN_COL( screen_ticks ) ( screen_ticks % VIDEO_SCREEN_WIDTH )

#define VIDEO_GET_DISPLAY_ADDR( display_row, display_col ) ( ( display_row * VIDEO_DISPLAY_WIDTH ) + display_col )


#ifdef __cplusplus
}
#endif

#endif /* MZ1500_VIDEO_H */

