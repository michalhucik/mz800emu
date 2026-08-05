/* 
 * File:   mz800_framebuffer.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 5. července 2015, 8:40
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
#include "hw-generic/memory/memory_arch.h" /* per-arch memory - dříve tranzitivně přes memory.h (mzhal 11c-2c) */


#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mz800_framebuffer.h"
#include "mz800_framebuffer_done.h"
#include "mz800_gdg.h"
#include "mz800_hwscroll.h"
#include "hw-generic/gdg/video.h"
#include "memory/memory.h"
#include "iface/iface_video.h"

gdg_framebuffer_t g_framebuffer;

#define BIT0POS( val, pos ) ( ( val & 1 ) << pos )

/* Definice barev pro MZ700 */
static const int c_MZ700_COLORMAP [] = { 0, 9, 10, 11, 12, 13, 14, 15 };


void framebuffer_init(void) 
{
    memset(g_framebuffer.pixbuff, 0, sizeof(g_framebuffer.pixbuff));
    g_framebuffer.pixels = g_framebuffer.pixbuff[0];
    g_framebuffer.pixels_id = 0;

    g_framebuffer.framebuffer_state = FB_STATE_NOT_CHANGED;
    g_framebuffer.screen_changes = SCRSTS_IS_NOT_CHANGED;
    g_framebuffer.border_changes = SCRSTS_IS_NOT_CHANGED;
    framebudef_clear_flag_20ms_passed();
}

static inline void framebuffer_update_MZ700_screen_row ( unsigned beam_row ) {
    unsigned mz700_mask = 0;
    unsigned mz700_attr_fgc = 0;
    unsigned mz700_attr_bgc = 0;
    unsigned mz700_attr_grp;
    unsigned mz700_cgram_addr = 0;

    uint8_t *CGRAM = &g_memoryVRAM[0];
    uint8_t *VRAM_CHAR = &g_memoryVRAM [ 0x1000 ];
    uint8_t *VRAM_ATTR = &g_memoryVRAM [ 0x1800 ];

    uint8_t *p = g_framebuffer.pixels + ( beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH ) * sizeof ( uint8_t );

    unsigned pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW;

    unsigned vram_addr = ( ( pos_Y / 8 ) * 40 );

    uint8_t *vram_data = &VRAM_CHAR [ vram_addr ];
    uint8_t *vram_attr = &VRAM_ATTR [ vram_addr ];

    unsigned pos_X;

    for ( pos_X = 0; pos_X < VIDEO_CANVAS_WIDTH; pos_X += 16 ) {

        mz700_attr_fgc = ( *vram_attr >> 4 ) & 0x07;
        mz700_attr_bgc = *vram_attr & 0x07;
        mz700_attr_grp = ( ( *vram_attr << 4 ) & 0x0800 );

        mz700_cgram_addr = mz700_attr_grp | ( *vram_data * 8 );

        mz700_mask = CGRAM [ mz700_cgram_addr + pos_Y % 8 ];
        mz700_mask = mz700_mask >> ( ( pos_X & 0x0f ) / 2 );

        vram_data++;
        vram_attr++;

        /* 0. pixel */
        p[0] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[1] = p[0];
        mz700_mask = mz700_mask >> 1;

        /* 1. pixel */
        p[2] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[3] = p[2];
        mz700_mask = mz700_mask >> 1;

        /* 2. pixel */
        p[4] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[5] = p[4];
        mz700_mask = mz700_mask >> 1;

        /* 3. pixel */
        p[6] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[7] = p[6];
        mz700_mask = mz700_mask >> 1;

        /* 4. pixel */
        p[8] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[9] = p[8];
        mz700_mask = mz700_mask >> 1;

        /* 5. pixel */
        p[10] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[11] = p[10];
        mz700_mask = mz700_mask >> 1;

        /* 6. pixel */
        p[12] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[13] = p[12];
        mz700_mask = mz700_mask >> 1;

        /* 7. pixel */
        p[14] = ( mz700_mask & 1 ) ? c_MZ700_COLORMAP [ mz700_attr_fgc ] : c_MZ700_COLORMAP [ mz700_attr_bgc ];
        p[15] = p[14];

        p += 16;
    };
}


void framebuffer_update_MZ700_current_screen_row ( void ) {
    framebuffer_update_MZ700_screen_row ( g_gdg.beam_row );
}


void framebuffer_update_MZ700_all_rows ( void ) {
    int i;
    for ( i = VIDEO_BEAM_CANVAS_FIRST_ROW; i <= VIDEO_BEAM_CANVAS_LAST_ROW; i++ ) {
        framebuffer_update_MZ700_screen_row ( i );
    };
}


/* 
 * 
 * Na konci viditelneho radku zaktualizujeme aktualni radek borderu ve framebufferu.
 * 
 */
void framebuffer_border_current_row_fill ( void ) {

    /* Pokud uz je radek ve framebufferu hotov, tak jdeme pryc */
    if ( g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1 ) return;

    unsigned beam_row = g_gdg.beam_row;
    uint8_t *p = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH;

    if ( ( beam_row < VIDEO_BORDER_TOP_HEIGHT ) || ( beam_row > VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT - 1 ) ) {
        memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, VIDEO_DISPLAY_WIDTH - g_gdg.last_updated_border_pixel );
    } else {
        if ( g_gdg.last_updated_border_pixel < VIDEO_BORDER_LEFT_WIDTH ) {
            memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, VIDEO_BORDER_LEFT_WIDTH - g_gdg.last_updated_border_pixel );
        };
        unsigned pos_x = ( g_gdg.last_updated_border_pixel >= VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH ) ? g_gdg.last_updated_border_pixel : VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH;
        memset ( &p [ pos_x ], g_gdg.regBOR, VIDEO_DISPLAY_WIDTH - pos_x );
    };
}


/*
 * 
 *  Prave doslo ke zmene borderu. Nez provedeme zmenu, tak zaktualizujeme aktualni radek az po misto, kde nastala zmena.
 *
 */
void framebuffer_border_changed ( void ) {

    g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;

    unsigned beam_col = VIDEO_GET_SCREEN_COL ( g_gdg.total_elapsed.ticks );
    unsigned beam_row = VIDEO_GET_SCREEN_ROW ( g_gdg.total_elapsed.ticks );


    /* Osetreni situace, kdy OUT provedl zmenu na hranici mezi dvema snimky. */
    if ( beam_row == VIDEO_SCREEN_HEIGHT ) {
        beam_row = 0;
    };

    /* Pokud jsme na radku, ktery je mimo viditelnou oblast, tak neni potreba aktualizovat framebuffer. */
    if ( beam_row > VIDEO_BEAM_DISPLAY_LAST_ROW ) return;

    /* Pokud je tento radek ve framebufferu uz hotov, tak muzeme jit pryc. */
    if ( ( beam_row == g_gdg.beam_row ) && ( g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1 ) ) return;

    /* Pokud doslo ke zmene na zacatku framebufferu, tak neni co updatovat. */
    if ( beam_col == 0 ) return;

    /* Osetreni maximalni horni hranice paprsku na radku. */
    beam_col = ( beam_col > VIDEO_BEAM_DISPLAY_LAST_COLUMN ) ? ( VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1 ) : beam_col;

    /* */
    if ( beam_row != g_gdg.beam_row ) {
        g_gdg.last_updated_border_pixel = 0;
    }
    /*
     *  Nyni provedeme aktualizaci framebufferu. 
     *  Ulozime stav od posledni aktualizace po nynejsi pozici - 1.
     *  Do last_updated ulozime pozici od ktere bude platit novy stav.
     *  Pokud do last_updated ulozime DISPLAY_VISIBLE_LAST_COLUMN + 1, tak to znamena, ze uz je cely radek hotovy.
     */
    uint8_t *p = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH;

    /* Jsme v hornim, nebo dolnim borderu? */
    if ( ( beam_row <= VIDEO_BEAM_BORDER_TOP_LAST_ROW ) || ( beam_row >= VIDEO_BEAM_BORDER_BOTOM_FIRST_ROW ) ) {
        memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, beam_col - g_gdg.last_updated_border_pixel );

        g_gdg.last_updated_border_pixel = beam_col;

    } else {

        /* Je potreba aktualizovat levy border? */
        if ( g_gdg.last_updated_border_pixel <= VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN ) {

            unsigned last_pixel = ( beam_col < VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN ) ? beam_col : VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN + 1;
            memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, last_pixel - g_gdg.last_updated_border_pixel );

            g_gdg.last_updated_border_pixel = last_pixel;
        };

        /* Je potreba aktualizovat pravy border? */
        if ( beam_col >= VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN ) {

            unsigned start_pixel = ( g_gdg.last_updated_border_pixel >= VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN ) ? g_gdg.last_updated_border_pixel : VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN;
            memset ( &p [ start_pixel ], g_gdg.regBOR, beam_col - start_pixel );

            g_gdg.last_updated_border_pixel = beam_col;
        };
    };
}


void framebuffer_MZ800_screen_changed ( void ) {
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
    unsigned column = VIDEO_GET_SCREEN_COL ( g_gdg.total_elapsed.ticks );
    if ( ( column < VIDEO_BEAM_CANVAS_FIRST_COLUMN + 3 ) || ( column > VIDEO_BEAM_CANVAS_LAST_COLUMN ) || ( g_gdg.beam_row < VIDEO_BEAM_CANVAS_FIRST_ROW ) || ( g_gdg.beam_row > VIDEO_BEAM_CANVAS_LAST_ROW ) ) return;
    framebuffer_MZ800_current_screen_row_fill ( column - VIDEO_BEAM_CANVAS_FIRST_COLUMN );
}


static inline unsigned framebuffer_MZ800_screen_row_fill ( unsigned beam_row, unsigned last_pixel ) {
    static unsigned vram_data1 = 0;
    static unsigned vram_data2 = 0;
    static unsigned vram_data3 = 0;
    static unsigned vram_data4 = 0;

    unsigned pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW;
    unsigned pos_X = g_gdg.screen_need_update_from;

    uint8_t *p = g_framebuffer.pixels + ( ( beam_row * VIDEO_DISPLAY_WIDTH ) + VIDEO_BORDER_LEFT_WIDTH + pos_X ) * sizeof ( uint8_t );

    unsigned vram_addr;
    vram_addr = ( pos_X / 16 ) + ( pos_Y * 40 );
    if ( pos_X % 16 ) vram_addr++;

    unsigned plt_code;
    unsigned IGRB_output;

    /* TODO: z nepripojenych EXVRAM budeme cist 0xff */

    switch ( g_gdg.regDMD & 0x07 ) {

            /* 320x200 @ 4/A */
        case 0x00:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data1 = g_memoryVRAM_I [ real_vram_addr ];
                    vram_data2 = g_memoryVRAM_II [ real_vram_addr ];
                };

                plt_code = BIT0POS ( vram_data2, 1 ) | BIT0POS ( vram_data1, 0 );

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                    case 2:
                        IGRB_output = g_gdg.regPAL2;
                        break;
                    case 3:
                        IGRB_output = g_gdg.regPAL3;
                        break;
                };

                *p++ = IGRB_output;
                *p++ = IGRB_output;

                vram_data1 = vram_data1 >> 1;
                vram_data2 = vram_data2 >> 1;

                pos_X += 2;
            };
            break;


            /* 320x200 @ 4/B */
        case 0x01:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                plt_code = BIT0POS ( vram_data4, 1 ) | BIT0POS ( vram_data3, 0 );

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                    case 2:
                        IGRB_output = g_gdg.regPAL2;
                        break;
                    case 3:
                        IGRB_output = g_gdg.regPAL3;
                        break;
                };

                *p++ = IGRB_output;
                *p++ = IGRB_output;

                vram_data3 = vram_data3 >> 1;
                vram_data4 = vram_data4 >> 1;

                pos_X += 2;
            };
            break;


            /* 320x200 @ 16 */
        case 0x02:
            while ( pos_X < last_pixel ) {
                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data1 = g_memoryVRAM_I [ real_vram_addr ];
                    vram_data2 = g_memoryVRAM_II [ real_vram_addr ];
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                plt_code = BIT0POS ( vram_data4, 3 ) | BIT0POS ( vram_data3, 2 ) | BIT0POS ( vram_data2, 1 ) | BIT0POS ( vram_data1, 0 );

                if ( g_gdg.regPALGRP == ( ( plt_code >> 2 ) & 0x03 ) ) {
                    switch ( plt_code & 0x03 ) {
                        case 0:
                            IGRB_output = g_gdg.regPAL0;
                            break;
                        case 1:
                            IGRB_output = g_gdg.regPAL1;
                            break;
                        case 2:
                            IGRB_output = g_gdg.regPAL2;
                            break;
                        case 3:
                            IGRB_output = g_gdg.regPAL3;
                            break;
                    };
                } else {
                    IGRB_output = plt_code;
                };

                *p++ = IGRB_output;
                *p++ = IGRB_output;

                vram_data1 = vram_data1 >> 1;
                vram_data2 = vram_data2 >> 1;
                vram_data3 = vram_data3 >> 1;
                vram_data4 = vram_data4 >> 1;

                pos_X += 2;
            };
            break;


            /* 320x200 @ 16 undoc */
        case 0x03:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data1 = g_memoryVRAM_I [ real_vram_addr ];
                    vram_data2 = g_memoryVRAM_II [ real_vram_addr ];
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                if ( g_gdg.regPALGRP == ( BIT0POS ( vram_data4, 1 ) | BIT0POS ( vram_data3, 0 ) ) ) {
                    plt_code = BIT0POS ( vram_data4, 3 ) | BIT0POS ( vram_data3, 2 ) | BIT0POS ( vram_data4, 1 ) | BIT0POS ( vram_data3, 0 );
                } else {
                    plt_code = BIT0POS ( vram_data4, 3 ) | BIT0POS ( vram_data3, 2 ) | BIT0POS ( vram_data2, 1 ) | BIT0POS ( vram_data1, 0 );
                };

                if ( g_gdg.regPALGRP == ( ( plt_code >> 2 ) & 0x03 ) ) {
                    switch ( plt_code & 0x03 ) {
                        case 0:
                            IGRB_output = g_gdg.regPAL0;
                            break;
                        case 1:
                            IGRB_output = g_gdg.regPAL1;
                            break;
                        case 2:
                            IGRB_output = g_gdg.regPAL2;
                            break;
                        case 3:
                            IGRB_output = g_gdg.regPAL3;
                            break;
                    };
                } else {
                    IGRB_output = plt_code;
                };

                *p++ = IGRB_output;
                *p++ = IGRB_output;

                vram_data1 = vram_data1 >> 1;
                vram_data2 = vram_data2 >> 1;
                vram_data3 = vram_data3 >> 1;
                vram_data4 = vram_data4 >> 1;

                pos_X += 2;
            };
            break;


            /* 640x200 @ 2/A */
        case 0x04:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data1 = g_memoryVRAM_I [ real_vram_addr ];
                    vram_data2 = g_memoryVRAM_II [ real_vram_addr ];
                };

                if ( 0x00 == ( pos_X & 0x08 ) ) {
                    /* sudy bajt */
                    plt_code = BIT0POS ( vram_data1, 0 );
                    vram_data1 = vram_data1 >> 1;
                } else {
                    /* lichy bajt */
                    plt_code = BIT0POS ( vram_data2, 0 );
                    vram_data2 = vram_data2 >> 1;
                };

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                };

                *p++ = IGRB_output;

                pos_X++;
            };
            break;


            /* 640x200 @ 2/B */
        case 0x05:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                if ( 0x00 == ( pos_X & 0x08 ) ) {
                    /* sudy bajt */
                    plt_code = BIT0POS ( vram_data3, 0 );
                    vram_data3 = vram_data3 >> 1;
                } else {
                    /* lichy bajt */
                    plt_code = BIT0POS ( vram_data4, 0 );
                    vram_data4 = vram_data4 >> 1;
                };

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                };

                *p++ = IGRB_output;

                pos_X++;
            };
            break;


            /* 640x200 @ 4 */
        case 0x06:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data1 = g_memoryVRAM_I [ real_vram_addr ];
                    vram_data2 = g_memoryVRAM_II [ real_vram_addr ];
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                if ( 0x00 == ( pos_X & 0x08 ) ) {
                    /* sudy bajt */
                    plt_code = BIT0POS ( vram_data3, 1 ) | BIT0POS ( vram_data1, 0 );
                    vram_data1 = vram_data1 >> 1;
                    vram_data3 = vram_data3 >> 1;
                } else {
                    /* lichy bajt */
                    plt_code = BIT0POS ( vram_data4, 1 ) | BIT0POS ( vram_data2, 0 );
                    vram_data2 = vram_data2 >> 1;
                    vram_data4 = vram_data4 >> 1;
                };

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                    case 2:
                        IGRB_output = g_gdg.regPAL2;
                        break;
                    case 3:
                        IGRB_output = g_gdg.regPAL3;
                        break;
                };

                *p++ = IGRB_output;

                pos_X++;
            };
            break;


            /* 640x200 @ 4 undoc */
        case 0x07:
            while ( pos_X < last_pixel ) {

                if ( 0 == ( pos_X & 0x0f ) ) {
                    unsigned real_vram_addr = hwscroll_shift_addr ( vram_addr++ );
                    vram_data3 = g_memoryVRAM_III [ real_vram_addr ];
                    vram_data4 = g_memoryVRAM_IV [ real_vram_addr ];
                };

                if ( 0x00 == ( pos_X & 0x08 ) ) {
                    /* sudy bajt */
                    plt_code = BIT0POS ( vram_data3, 1 ) | BIT0POS ( vram_data3, 0 );
                    vram_data3 = vram_data3 >> 1;
                } else {
                    /* lichy bajt */
                    plt_code = BIT0POS ( vram_data4, 1 ) | BIT0POS ( vram_data4, 0 );
                    vram_data4 = vram_data4 >> 1;
                };

                switch ( plt_code ) {
                    case 0:
                        IGRB_output = g_gdg.regPAL0;
                        break;
                    case 1:
                        IGRB_output = g_gdg.regPAL1;
                        break;
                    case 2:
                        IGRB_output = g_gdg.regPAL2;
                        break;
                    case 3:
                        IGRB_output = g_gdg.regPAL3;
                        break;
                };

                *p++ = IGRB_output;

                pos_X++;
            };
            break;
    };
    return pos_X;
}


void framebuffer_MZ800_current_screen_row_fill ( unsigned last_pixel ) {
    last_pixel -= last_pixel % 2;
    if ( g_gdg.screen_need_update_from >= last_pixel ) return;
    unsigned beam_row = g_gdg.beam_row;
    g_gdg.screen_need_update_from = framebuffer_MZ800_screen_row_fill ( beam_row, last_pixel );
}


void framebuffer_MZ800_all_screen_rows_fill ( void ) {
    int i;
    g_gdg.screen_need_update_from = 0;
    for ( i = VIDEO_BEAM_CANVAS_FIRST_ROW; i <= VIDEO_BEAM_CANVAS_LAST_ROW; i++ ) {
        framebuffer_MZ800_screen_row_fill ( i, VIDEO_CANVAS_WIDTH );
    };
}


void framebuffer_border_all_rows_fill ( void ) {

    int i;

    for ( i = 0; i < VIDEO_BORDER_TOP_HEIGHT; i++ ) {
        uint8_t *p = g_framebuffer.pixels + ( i * VIDEO_DISPLAY_WIDTH );
        memset ( p, g_gdg.regBOR, VIDEO_DISPLAY_WIDTH );
    };

    for ( i = VIDEO_BORDER_TOP_HEIGHT; i < ( VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_LEFT_HEIGHT ); i++ ) {
        uint8_t *p = g_framebuffer.pixels + ( i * VIDEO_DISPLAY_WIDTH );
        memset ( p, g_gdg.regBOR, VIDEO_BORDER_LEFT_WIDTH );
    };

    for ( i = VIDEO_BORDER_TOP_HEIGHT; i < ( VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_RIGHT_HEIGHT ); i++ ) {
        uint8_t *p = g_framebuffer.pixels + ( i * VIDEO_DISPLAY_WIDTH ) + ( VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH );
        memset ( p, g_gdg.regBOR, VIDEO_BORDER_RIGHT_WIDTH );
    };

    for ( i = ( VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_RIGHT_HEIGHT ); i < VIDEO_DISPLAY_HEIGHT; i++ ) {
        uint8_t *p = g_framebuffer.pixels + ( i * VIDEO_DISPLAY_WIDTH );
        memset ( p, g_gdg.regBOR, VIDEO_DISPLAY_WIDTH );
    };
}

/*
 *
 * Sem si odskocime, pokud jsme zastavili emulator v cmthack a potrebujeme vykreslit kompletni screeen.
 *
 */
void framebuffer_flush_full_screen(void)
{

    if (g_framebuffer.screen_changes | g_framebuffer.border_changes)
    {

        unsigned beam_row = g_gdg.beam_row;

        do
        {
            if (g_framebuffer.screen_changes)
            {
                if ((g_gdg.beam_row >= VIDEO_BEAM_CANVAS_FIRST_ROW) && (g_gdg.beam_row <= VIDEO_BEAM_CANVAS_LAST_ROW))
                {
                    if (!GDG_DMD_TEST_MODE700)
                    {
                        framebuffer_MZ800_current_screen_row_fill(VIDEO_CANVAS_WIDTH);
                    }
                    else
                    {
                        framebuffer_update_MZ700_current_screen_row();
                    };
                };
                g_gdg.screen_need_update_from = 0;
            };

            if (g_framebuffer.border_changes)
            {
                if (g_gdg.beam_row <= VIDEO_BEAM_DISPLAY_LAST_ROW)
                {
                    framebuffer_border_current_row_fill();
                    g_gdg.last_updated_border_pixel = 0;
                };
            };

            if (g_gdg.beam_row < VIDEO_SCREEN_HEIGHT - 1)
            {
                g_gdg.beam_row++;
            }
            else
            {
                g_gdg.beam_row = 0;
            };

        } while (g_gdg.beam_row != beam_row);

        if (g_framebuffer.screen_changes)
        {
            g_framebuffer.framebuffer_state |= FB_STATE_SCREEN_CHANGED;
        };

        if (g_framebuffer.border_changes)
        {
            g_framebuffer.framebuffer_state |= FB_STATE_BORDER_CHANGED;
        };
    };

    if (g_framebuffer.framebuffer_state || iface_video_get_redraw_full_screen_request())
    {
        framebuffer_screen_done();
        g_framebuffer.framebuffer_state = FB_STATE_NOT_CHANGED;
    };
}
