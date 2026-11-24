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

/**
 * @brief Barva režimu MZ-700 (0 až 7) jako IGRB: 0 pro 0, jinak s intenzitou.
 * @param c Barva (bity 0 až 2).
 */
static inline uint8_t framebuffer_mz700_igrb ( unsigned c ) {
    c &= 7;
    return (uint8_t) ( c ? ( 8u | c ) : 0u );
}


/**
 * @brief Nakreslí řádek canvasu v nedokumentovaných režimech DMD 0Ch až 0Fh.
 *
 * Pravidla z C modelu netlistu GDG (emu-experiments hw-fixes/research/dmd0c/
 * SPEC.md, pravidla 1, 2 a 5; referenční implementace dmd0c_ref.c sedí s modelem
 * ve 522 240 z 522 240 bodů a se záznamy U5 ze skutečného MZ-800, měření 8, 14,
 * 16 a 17, ve 218 880 z 218 880 bodů):
 * - buňka k (0 až 39) čte znak C a atribut A z offsetu (40 * row_addr + k)
 *   mod 1024, vzor P = CG[sada * 800h + 8 * C + r] (sada = bit 7 atributu,
 *   r = řádek znaku) a čtvrtý bajt F = CG[sada * 800h + (P >> 5) * 100h +
 *   ((8 * C + r) & 0FFh)];
 * - body 0 až 7 buňky jsou bity 0 až 7 vzoru P v barvách atributu A,
 *   body 8 až 15 bity 0 až 7 atributu následující buňky v barvách bajtu F
 *   (u buňky 39 místo atributu následující buňky bajt F); 1 bod = 1 px;
 * - barva c se převádí na IGRB c ? 8 | c : 0, bez palety.
 * Adresa F stojí na předpokladu, že paměť VRAM nepoužije sloupcové bity 0 a 7
 * (shoda se strojem; datasheet MB81416 nedohledán [neověřeno]). Mechanismus
 * druhé poloviny buňky (atribut následující buňky) je z chování modelu,
 * v netlistu nerozebraný (hypotéza).
 *
 * @param beam_row Řádek rastru v canvasu.
 * @param p Cíl: sloupec 154 řádku; body se kreslí do p[-2] až p[637]
 *        (sloupce 152 až 791, poloha jako 640x200).
 * @pre g_gdg.row_addr_of_row[beam_row] platí; p[-2] je platná paměť.
 */
static void framebuffer_DMD0C_row_render ( unsigned beam_row, uint8_t *p ) {
    const uint8_t *vram = g_memoryVRAM;
    unsigned text_off = ( g_gdg.row_addr_of_row [ beam_row ] * 40u ) & 0x03ff;
    unsigned r = ( beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW ) & 0x07;
    uint8_t attr[41], pat[40], fourth[40];
    unsigned k;
    for ( k = 0; k < 40; k++ ) {
        unsigned off = ( text_off + k ) & 0x03ff;
        unsigned ch = vram [ 0x1000 + off ];
        unsigned at = vram [ 0x1800 + off ];
        unsigned set = ( at >> 7 ) * 0x800u;
        unsigned lo = ( ch * 8u + r ) & 0xffu;
        unsigned pt = vram [ set + ch * 8u + r ];
        attr[k] = (uint8_t) at;
        pat[k] = (uint8_t) pt;
        fourth[k] = vram [ set + ( ( pt >> 5 ) << 8 ) + lo ];
    };
    attr[40] = fourth[39];
    uint8_t *o = p - 2;
    for ( k = 0; k < 40; k++ ) {
        uint8_t fg1 = framebuffer_mz700_igrb ( attr[k] >> 4 ), bg1 = framebuffer_mz700_igrb ( attr[k] );
        uint8_t fg2 = framebuffer_mz700_igrb ( fourth[k] >> 4 ), bg2 = framebuffer_mz700_igrb ( fourth[k] );
        unsigned x = attr[k + 1];
        unsigned j;
        for ( j = 0; j < 8; j++ ) {
            o[j] = ( ( pat[k] >> j ) & 1u ) ? fg1 : bg1;
            o[8 + j] = ( ( x >> j ) & 1u ) ? fg2 : bg2;
        };
        o += 16;
    };
}


/**
 * @brief Nakreslí celý řádek canvasu v režimu MZ-700 (DMD 08h až 0Fh).
 *
 * Text a atributy se čtou od offsetu 40 * row_addr_of_row[beam_row] mod 1024
 * (čítač adresy řádku GDG, viz st_GDG.row_addr), offset buňky přetéká na
 * 1024 jako na stroji (textový řádek 25 bere buňky 24 až 39 z offsetu 0
 * až 15). Řádek znaku je řádek canvasu mod 8. DMD 0Ch až 0Fh kreslí
 * framebuffer_DMD0C_row_render() (zapisuje i 2 body před @p p).
 *
 * @param beam_row Řádek rastru v canvasu.
 * @param p Cíl: první bod canvasu řádku (VIDEO_CANVAS_WIDTH bajtů, v DMD
 *        0Ch až 0Fh navíc p[-2] a p[-1]).
 * @pre g_gdg.row_addr_of_row[beam_row] platí pro tento řádek.
 */
static inline void framebuffer_MZ700_row_render ( unsigned beam_row, uint8_t *p ) {
    if ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) {
        /* DMD 0Ch až 0Fh: nedokumentované režimy podle C modelu netlistu. */
        framebuffer_DMD0C_row_render ( beam_row, p );
        return;
    };
    unsigned mz700_mask = 0;
    unsigned mz700_attr_fgc = 0;
    unsigned mz700_attr_bgc = 0;
    unsigned mz700_attr_grp;
    unsigned mz700_cgram_addr = 0;

    uint8_t *CGRAM = &g_memoryVRAM[0];
    uint8_t *VRAM_CHAR = &g_memoryVRAM [ 0x1000 ];
    uint8_t *VRAM_ATTR = &g_memoryVRAM [ 0x1800 ];

    unsigned pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW;

    /* Offset textu podle čítače adresy řádku, 10 bitů (přetéká na 1024). */
    unsigned vram_addr = ( g_gdg.row_addr_of_row [ beam_row ] * 40u ) & 0x03ff;

    unsigned pos_X;

    for ( pos_X = 0; pos_X < VIDEO_CANVAS_WIDTH; pos_X += 16 ) {

        unsigned vram_data = VRAM_CHAR [ vram_addr ];
        unsigned vram_attr = VRAM_ATTR [ vram_addr ];

        mz700_attr_fgc = ( vram_attr >> 4 ) & 0x07;
        mz700_attr_bgc = vram_attr & 0x07;
        mz700_attr_grp = ( ( vram_attr << 4 ) & 0x0800 );

        mz700_cgram_addr = mz700_attr_grp | ( vram_data * 8 );

        mz700_mask = CGRAM [ mz700_cgram_addr + pos_Y % 8 ];
        mz700_mask = mz700_mask >> ( ( pos_X & 0x0f ) / 2 );

        vram_addr = ( vram_addr + 1 ) & 0x03ff;

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


/** Pomocný řádek canvasu pro kreslení části řádku (přepnutí režimu uprostřed
 *  řádku); 2 bajty před ním pro body DMD 0Ch až 0Fh ve sloupcích 152 a 153. */
static uint8_t s_row_tmp_buf[VIDEO_CANVAS_WIDTH + 2];
static uint8_t * const s_row_tmp = &s_row_tmp_buf[2];

/** Body režimu 640 aktuálního řádku podle indexu bodu (0 až 639), viz framebuffer_MZ800_fill_640(). */
static uint8_t s_row640[VIDEO_CANVAS_WIDTH];

/** Kam je s_row640 nakreslený (index bodu); 0 = nic. Platí jen pro řádek s_row640_row. */
static unsigned s_row640_done;

/** Řádek, pro který platí s_row640_done (jiný řádek = nic nakreslené); ~0u = žádný.
 *  Nuluje framebuffer_row_state_reset() na konci řádku a po načtení snapshotu. */
static unsigned s_row640_row = ~0u;

/** Řádek a sloupec canvasu přepnutí 320 -> 640 v aktuálním řádku (framebuffer_MZ800_resolution_switch()):
 *  grafika 640 tam začíná už bodem s indexem sloupce (sloupec framebufferu 152 + x), ne x + 2.
 *  ~0u = v tomto řádku nebylo; nuluje framebuffer_row_state_reset(). */
static unsigned s_sw640_row = ~0u;
static unsigned s_sw640_x;

/** Sloupec framebufferu prvního bodu canvasu v režimu 640 (338 px). */
#define FB_CANVAS640_FIRST_COLUMN ( VIDEO_BEAM_CANVAS_FIRST_COLUMN - 2 )


/**
 * @brief První sloupec za levým borderem řádku (152 v geometrii 640, jinak 154).
 * @param beam_row Řádek rastru.
 */
static inline unsigned framebuffer_border_left_end ( unsigned beam_row ) {
    return ( ( beam_row == g_gdg.beam_row ) && g_gdg.geom640_cs ) ? FB_CANVAS640_FIRST_COLUMN : VIDEO_BORDER_LEFT_WIDTH;
}


/**
 * @brief První sloupec pravého borderu řádku (792 v geometrii 640, jinak 794).
 * @param beam_row Řádek rastru.
 */
static inline unsigned framebuffer_border_right_first ( unsigned beam_row ) {
    return ( ( beam_row == g_gdg.beam_row ) && g_gdg.geom640_ce ) ? VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN - 2 : VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN;
}


/**
 * @brief Konec viditelné části řádku ve framebufferu (border v režimu 640 končí o 1 px dřív).
 * @param beam_row Řádek rastru.
 * @return Sloupec za posledním viditelným bodem.
 */
static inline unsigned framebuffer_visible_end ( unsigned beam_row ) {
    unsigned end = VIDEO_DISPLAY_WIDTH;
    if ( beam_row == g_gdg.beam_row ) {
        end -= g_gdg.row_shrink;
        if ( g_gdg.geom640_be && !g_gdg.row_shrink ) end -= 1;
    };
    return end;
}


/**
 * @brief Ukazatel na první bod canvasu řádku ve framebufferu.
 * @param beam_row Řádek rastru.
 * @return Ukazatel do g_framebuffer.pixels.
 */
static inline uint8_t* framebuffer_canvas_row ( unsigned beam_row ) {
    return g_framebuffer.pixels + ( beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH );
}


/**
 * @brief Nakreslí celý řádek canvasu v režimu MZ-700 do framebufferu.
 * @param beam_row Řádek rastru v canvasu.
 */
static inline void framebuffer_update_MZ700_screen_row ( unsigned beam_row ) {
    framebuffer_MZ700_row_render ( beam_row, framebuffer_canvas_row ( beam_row ) );
}


/**
 * @brief Nakreslí v režimu MZ-700 jen body canvasu @p from až @p to - 1 aktuálního řádku.
 * @param from První bod canvasu (0 až VIDEO_CANVAS_WIDTH).
 * @param to Bod za posledním (0 až VIDEO_CANVAS_WIDTH).
 */
static void framebuffer_MZ700_current_row_range ( unsigned from, unsigned to ) {
    if ( from >= to ) return;
    framebuffer_MZ700_row_render ( g_gdg.beam_row, s_row_tmp );
    memcpy ( framebuffer_canvas_row ( g_gdg.beam_row ) + from, s_row_tmp + from, to - from );
}


/* Řádek v režimu MZ-700 na konci HBLN, viz mz800_framebuffer.h. */
void framebuffer_update_MZ700_current_screen_row ( void ) {
    /* Řádek už nakreslila změna režimu uprostřed řádku (framebuffer_mode700_switch()). */
    if ( g_gdg.mz700_skip_row == g_gdg.beam_row + 1 ) return;
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
        unsigned left_end = framebuffer_border_left_end ( beam_row );
        if ( g_gdg.last_updated_border_pixel < left_end ) {
            memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, left_end - g_gdg.last_updated_border_pixel );
        };
        unsigned right_first = framebuffer_border_right_first ( beam_row );
        unsigned pos_x = ( g_gdg.last_updated_border_pixel >= right_first ) ? g_gdg.last_updated_border_pixel : right_first;
        memset ( &p [ pos_x ], g_gdg.regBOR, VIDEO_DISPLAY_WIDTH - pos_x );
    };

    /* Krátký řádek (CKSW = 1): viditelná část končí o row_shrink bodů dřív,
     * zbytek řádku ve framebufferu je zatemnění (černá, IGRB 0). V režimu
     * 640 končí border o 1 px dřív (1113 px místo 1114). */
    unsigned end = framebuffer_visible_end ( beam_row );
    if ( end < VIDEO_DISPLAY_WIDTH ) {
        memset ( &p [ end ], 0x00, VIDEO_DISPLAY_WIDTH - end );
    };
}


/**
 * @brief Přebarví už nakreslený border řádku ve sloupcích @p col až @p end - 1.
 *
 * Body canvasu nechává být. Konec ořízne na viditelnou šířku řádku
 * (zatemnění krátkého řádku při CKSW = 1 zůstává černé).
 *
 * @param beam_row Řádek framebufferu (viditelný).
 * @param col      První sloupec s novou barvou.
 * @param end      První sloupec, který se už nemění.
 * @param value    Nová barva borderu.
 * @pre Sloupce col až end - 1 jsou ve framebufferu nakreslené.
 */
static void framebuffer_border_repaint ( unsigned beam_row, unsigned col, unsigned end, unsigned value ) {
    unsigned vis_end = framebuffer_visible_end ( beam_row );
    if ( end > vis_end ) end = vis_end;
    if ( col >= end ) return;
    uint8_t *p = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH;

    if ( ( beam_row <= VIDEO_BEAM_BORDER_TOP_LAST_ROW ) || ( beam_row >= VIDEO_BEAM_BORDER_BOTOM_FIRST_ROW ) ) {
        memset ( &p [ col ], value, end - col );
        return;
    };
    unsigned left_end_col = framebuffer_border_left_end ( beam_row );
    if ( col < left_end_col ) {
        unsigned left_end = ( end < left_end_col ) ? end : left_end_col;
        memset ( &p [ col ], value, left_end - col );
    };
    unsigned right_first = framebuffer_border_right_first ( beam_row );
    unsigned start = ( col > right_first ) ? col : right_first;
    if ( start < end ) {
        memset ( &p [ start ], value, end - start );
    };
}


/**
 * @brief Společné tělo framebuffer_border_changed() a framebuffer_border_write().
 *
 * @param write     0 = jen dokreslit starou barvou po paprsek;
 *                  1 = zápis nové barvy (místo změny je před koncem M-cyklu).
 * @param new_value Nová barva borderu (jen při write = 1).
 */
static inline void framebuffer_border_paint ( int write, unsigned new_value ) {

    g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;

    /* Pozice od začátku aktuálního řádku (délka řádku závisí na CKSW).
     * Změnu na hranici dvou řádků nebo snímků, jejíž konec řádku ještě není
     * zpracovaný, gdg_get_beam_pos() přiřadí následujícímu řádku. */
    unsigned beam_row;
    unsigned beam_col = gdg_get_beam_pos ( g_gdg.total_elapsed.ticks, &beam_row );

    /* Pokud jsme na radku, ktery je mimo viditelnou oblast, tak neni potreba aktualizovat framebuffer. */
    if ( beam_row > VIDEO_BEAM_DISPLAY_LAST_ROW ) return;

    /* Barva se mění už během zápisového pulzu, na sudém sloupci. Změna před
     * sloupcem 0 padla do zatemnění na konci předchozího řádku: celý tento
     * řádek má novou barvu a není co dokreslovat. */
    if ( write ) {
        unsigned lead = GDG_BORDER_WR_LEAD_TICKS + ( ( beam_col + GDG_BORDER_WR_LEAD_TICKS ) & 1u );
        if ( beam_col <= lead ) return;
        beam_col -= lead;
    };

    /* Řádek je ve framebufferu už nakreslený za místo změny (uzavřený řádek,
     * nebo dokreslení po paprsek při pauze): při zápisu tu část přebarvit
     * novou barvou, jinak není co dělat. */
    if ( ( beam_row == g_gdg.beam_row )
         && ( ( beam_col <= g_gdg.last_updated_border_pixel )
              || ( g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1 ) ) ) {
        if ( write ) framebuffer_border_repaint ( beam_row, beam_col, g_gdg.last_updated_border_pixel, new_value );
        return;
    };

    /* Pokud doslo ke zmene na zacatku framebufferu, tak neni co updatovat. */
    if ( beam_col == 0 ) return;

    /* Osetreni maximalni horni hranice paprsku na radku. */
    beam_col = ( beam_col > VIDEO_BEAM_DISPLAY_LAST_COLUMN ) ? ( VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1 ) : beam_col;

    /* V krátkém řádku (CKSW = 1) končí viditelná část o row_shrink bodů dřív;
     * dál už se border nekreslí (zatemnění doplní konec viditelné části). */
    if ( ( beam_row == g_gdg.beam_row ) && ( beam_col > framebuffer_visible_end ( beam_row ) ) ) {
        beam_col = framebuffer_visible_end ( beam_row );
    };

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

        /* Je potreba aktualizovat levy border? (Konec levého borderu závisí
         * na režimu na začátku canvasu; rozhodne se dřív, než ho paprsek
         * dosáhne, viz gdg_geometry_dmd_written().) */
        unsigned left_end = framebuffer_border_left_end ( beam_row );
        if ( g_gdg.last_updated_border_pixel < left_end ) {

            unsigned last_pixel = ( beam_col < left_end ) ? beam_col : left_end;
            memset ( &p [ g_gdg.last_updated_border_pixel ], g_gdg.regBOR, last_pixel - g_gdg.last_updated_border_pixel );

            g_gdg.last_updated_border_pixel = last_pixel;
        };

        /* Je potreba aktualizovat pravy border? */
        unsigned right_first = framebuffer_border_right_first ( beam_row );
        if ( beam_col >= right_first ) {

            unsigned start_pixel = ( g_gdg.last_updated_border_pixel >= right_first ) ? g_gdg.last_updated_border_pixel : right_first;
            memset ( &p [ start_pixel ], g_gdg.regBOR, beam_col - start_pixel );

            g_gdg.last_updated_border_pixel = beam_col;
        };
    };
}


/* Dokreslení borderu po paprsek, viz mz800_framebuffer.h. */
void framebuffer_border_changed ( void ) {
    framebuffer_border_paint ( 0, 0 );
}


/* Zápis nové barvy borderu, viz mz800_framebuffer.h. */
void framebuffer_border_write ( unsigned new_value ) {
    framebuffer_border_paint ( 1, new_value );
}


/**
 * @brief O kolik sloupců za koncem M-cyklu zápisu do VRAM ještě leží buňka, kterou video fetch už přečetl.
 *
 * Na MZ-800 (mz800-knowledge hw-measurement/vysledky.md, oddíl 18, U8 po
 * 1 px) se zapsaný bajt v řádku zápisu ukáže, jestliže T1 zápisu je nejvýš
 * 626,5 px a buňka začíná v 660 px (320x200) nebo 658 px (640x200); od
 * 627,5 px ne, a to v obou režimech na stejném místě. Buňka k (340 + 16k px
 * v mřížce 320) se tedy čte asi 33,5 px před svým začátkem. Bez čekání je T1
 * = konec M-cyklu na stroji - 15 px a konec M-cyklu emulátoru E (sloupec)
 * leží v konci na stroji + 0,5 px - 186; zápis s T1 = 626,5 px má E = 456.
 * Čtená buňka je ta s 16k < (E - 154) + 17,5, pro celé E tedy 16k <= E - 154
 * + 17 (proto konstanta 17). Ověřeno vyhodnocením programu
 * gdg_meas12 v emulátoru skriptem explore12.py: meze U8 po 1 px jako na stroji.
 */
#define FB_VRAM_WR_FETCHED_AHEAD 17

void framebuffer_MZ800_screen_changed ( void ) {
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
    unsigned column = gdg_get_beam_col ( );
    if ( ( column > VIDEO_BEAM_CANVAS_LAST_COLUMN ) || ( g_gdg.beam_row < VIDEO_BEAM_CANVAS_FIRST_ROW ) || ( g_gdg.beam_row > VIDEO_BEAM_CANVAS_LAST_ROW ) ) return;
    /* Buňky, které video fetch už přečetl, se dokreslí se starým obsahem VRAM
     * (volá se před zápisem). Čekací takty zápisu posunou konec M-cyklu
     * a s ním i mez (hypotéza, změřeno jen bez čekání). */
    int x = (int) column - (int) VIDEO_BEAM_CANVAS_FIRST_COLUMN + FB_VRAM_WR_FETCHED_AHEAD;
    if ( x < 0 ) return;
    unsigned lim = ( (unsigned) x / 16u + 1u ) * 16u;
    if ( lim > VIDEO_CANVAS_WIDTH ) lim = VIDEO_CANVAS_WIDTH;
    /* v režimu 640 je buňka k v bodech 16k až 16k + 15 = sloupcích canvasu 16k - 2 až 16k + 13 */
    if ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) lim -= 2;
    framebuffer_MZ800_current_screen_row_fill ( lim );
}


/**
 * @brief Nakreslí řádek canvasu v grafickém režimu od g_gdg.screen_need_update_from po @p last_pixel.
 *
 * Data bajtu se drží ve statických proměnných: pokračování od bodu uvnitř
 * bajtu předpokládá, že předchozí volání pro tentýž řádek bajt načetlo.
 *
 * @param beam_row Řádek rastru v canvasu.
 * @param last_pixel Bod canvasu za posledním kresleným (sudý).
 * @param dst Cíl: první bod canvasu řádku (framebuffer nebo pomocný řádek).
 * @return Bod canvasu, kam se dokreslilo.
 */
static inline unsigned framebuffer_MZ800_screen_row_fill ( unsigned beam_row, unsigned last_pixel, uint8_t *dst ) {
    static unsigned vram_data1 = 0;
    static unsigned vram_data2 = 0;
    static unsigned vram_data3 = 0;
    static unsigned vram_data4 = 0;

    unsigned pos_X = g_gdg.screen_need_update_from;

    uint8_t *p = dst + pos_X;

    /* Řádek VRAM podle čítače adresy řádku (st_GDG.row_addr): po přepnutí
     * z režimu MZ-700 uprostřed snímku není roven řádku canvasu. */
    unsigned vram_addr;
    vram_addr = ( pos_X / 16 ) + ( g_gdg.row_addr_of_row [ beam_row ] * 40 );
    if ( pos_X % 16 ) vram_addr++;

    unsigned plt_code;
    unsigned IGRB_output;

    /* Neosazené rozšíření VRAM: g_memoryVRAM_III/IV ukazují na samé 0FFh
     * (mz800_memory_set_exvram()). */

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


/**
 * @brief Kreslení grafiky v režimu 640 (DMD 04h až 07h) do aktuálního řádku.
 *
 * Na MZ-800 leží bod režimu 640 s indexem i (0 až 639) v 338 + i px, o 2 px
 * dřív než v režimu 320 (mz800-knowledge hw-measurement/vysledky.md, oddíly
 * 12, 17 a 18; C model netlistu totéž). Přepnutí 320 / 640 uprostřed řádku
 * platí od hranice bajtu režimu 320 (sloupec 154 + 16k): sloupec c canvasu
 * v režimu 640 ukazuje bod c - 152 (U7: 700 -> 640 od 346 + 16k px, na
 * hranici bajtu režimu 640). Sloupce 148 + 16k až 153 + 16k kolem přepnutí
 * 320 / 640 přepíše framebuffer_MZ800_resolution_switch() (U19).
 *
 * Body se kreslí podle indexu do s_row640 (data bajtu drží statické proměnné
 * framebuffer_MZ800_screen_row_fill(), proto souvisle po bajtech) a do
 * framebufferu se kopírují posunuté: sloupce canvasu @p from až
 * @p last_pixel - 1 dostanou body from + 2 až last_pixel + 1. Na začátku
 * canvasu v geometrii 640 (st_GDG.geom640_cs) ještě sloupce 152 a 153 body
 * 0 a 1. Body s indexem 640 a 641 nejsou: sloupce 792 a 793 kreslí border
 * (geometrie 640 na konci canvasu).
 *
 * @param from První sloupec canvasu (g_gdg.screen_need_update_from).
 * @param last_pixel Sloupec canvasu za posledním (sudý).
 * @param keep Sloupce canvasu pod touto hodnotou se nepřepisují (část řádku
 *        v režimu MZ-700, st_GDG.canvas_split_x).
 * @pre Emu vlákno; DMD je režim 640; aktuální řádek je v canvasu.
 */
static void framebuffer_MZ800_fill_640 ( unsigned from, unsigned last_pixel, unsigned keep ) {
    unsigned beam_row = g_gdg.beam_row;
    if ( s_row640_row != beam_row ) {
        s_row640_row = beam_row;
        s_row640_done = 0;
    };
    unsigned first = ( ( from == 0 ) && g_gdg.geom640_cs && ( keep == 0 ) ) ? 0 : from + 2;
    /* Po přepnutí 320 -> 640 v tomto řádku začíná režim 640 už ve sloupci
     * 152 + x bodem x (C model netlistu, hw-fixes/research/sw640/SPEC.md). */
    if ( ( s_sw640_row == beam_row ) && ( from == s_sw640_x ) && ( from > 0 ) ) first = from;
    unsigned end = last_pixel + 2;
    if ( end > VIDEO_CANVAS_WIDTH ) end = VIDEO_CANVAS_WIDTH;
    if ( first < end ) {
        /* Souvislé pokračování, jinak nové načtení od začátku bajtu. */
        unsigned render_from = ( first == s_row640_done ) ? s_row640_done : ( first & ~15u );
        if ( render_from < end ) {
            unsigned saved = g_gdg.screen_need_update_from;
            g_gdg.screen_need_update_from = render_from;
            s_row640_done = framebuffer_MZ800_screen_row_fill ( beam_row, end, s_row640 );
            g_gdg.screen_need_update_from = saved;
        };
        uint8_t *row = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH + FB_CANVAS640_FIRST_COLUMN;
        /* Sloupec canvasu c = index bodu c + 2; chráněná část pod keep. */
        unsigned copy_from = first;
        if ( keep && ( copy_from < keep + 2 ) ) copy_from = keep + 2;
        if ( copy_from < end ) {
            memcpy ( row + copy_from, s_row640 + copy_from, end - copy_from );
        };
    };
    g_gdg.screen_need_update_from = last_pixel;
}


void framebuffer_MZ800_current_screen_row_fill ( unsigned last_pixel ) {
    last_pixel -= last_pixel % 2;
    if ( g_gdg.screen_need_update_from >= last_pixel ) return;
    unsigned beam_row = g_gdg.beam_row;
    unsigned keep = g_gdg.canvas_split_x;
    if ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) {
        framebuffer_MZ800_fill_640 ( g_gdg.screen_need_update_from, last_pixel, keep );
        return;
    };
    if ( ( g_gdg.screen_need_update_from == 0 ) && g_gdg.geom640_cs && ( keep == 0 ) ) {
        /* Začátek canvasu ještě v geometrii 640 (zápis DMD 640 -> 320 po hraně
         * začátku canvasu, ale před první hranicí bajtu): sloupce 152 a 153 jsou
         * canvas. Body 0 a 1 bajtu roviny I v barvách 640x200@2 (hypotéza, na
         * stroji ověřena jen poloha začátku canvasu, U19 po 1 px). */
        uint8_t *row = framebuffer_canvas_row ( beam_row );
        unsigned b = g_memoryVRAM_I [ hwscroll_shift_addr ( g_gdg.row_addr_of_row [ beam_row ] * 40u ) ];
        row[-2] = (uint8_t) ( ( b & 1 ) ? g_gdg.regPAL1 : g_gdg.regPAL0 );
        row[-1] = (uint8_t) ( ( b & 2 ) ? g_gdg.regPAL1 : g_gdg.regPAL0 );
    };
    if ( keep > g_gdg.screen_need_update_from ) {
        /* Grafika začíná až v bodě keep (přepnutí z režimu MZ-700 uprostřed
         * bajtu): celé bajty do pomocného řádku, do framebufferu jen od keep,
         * aby zůstala část řádku v režimu MZ-700. */
        unsigned end = framebuffer_MZ800_screen_row_fill ( beam_row, last_pixel, s_row_tmp );
        if ( end > keep ) {
            memcpy ( framebuffer_canvas_row ( beam_row ) + keep, s_row_tmp + keep, end - keep );
        };
        g_gdg.screen_need_update_from = end;
        return;
    };
    g_gdg.screen_need_update_from = framebuffer_MZ800_screen_row_fill ( beam_row, last_pixel, framebuffer_canvas_row ( beam_row ) );
}


/**
 * @brief IGRB bodu režimu 320x200@4 nebo 320x200@16 z kódu bodu (bity rovin I až IV).
 * @param dmd DMD režimu (bit 1 = 16 barev).
 * @param code Bity bodu: bit 0 rovina I, bit 1 rovina II, bit 2 III, bit 3 IV.
 */
static inline uint8_t framebuffer_color320 ( unsigned dmd, unsigned code ) {
    static const unsigned *const pal[4] = { &g_gdg.regPAL0, &g_gdg.regPAL1, &g_gdg.regPAL2, &g_gdg.regPAL3 };
    if ( dmd & 0x02 ) {
        if ( g_gdg.regPALGRP == ( ( code >> 2 ) & 0x03 ) ) return (uint8_t) *pal [ code & 0x03 ];
        return (uint8_t) ( code & 0x0f );
    };
    return (uint8_t) *pal [ code & 0x03 ];
}


/**
 * @brief IGRB bodu režimu 640x200@2 (04h) nebo 640x200@4 (06h) z bitů dvou rovin.
 * @param dmd DMD režimu (bit 1 = 4 barvy).
 * @param lo Bit roviny I (sudý bajt) / II (lichý bajt).
 * @param hi Bit roviny III / IV (jen 06h).
 */
static inline uint8_t framebuffer_color640 ( unsigned dmd, unsigned lo, unsigned hi ) {
    static const unsigned *const pal[4] = { &g_gdg.regPAL0, &g_gdg.regPAL1, &g_gdg.regPAL2, &g_gdg.regPAL3 };
    unsigned e = ( lo & 1 ) | ( ( dmd & 0x02 ) ? ( ( hi & 1 ) << 1 ) : 0 );
    return (uint8_t) *pal [ e ];
}


/* Body na rozhraní přepnutí 320 / 640, viz mz800_framebuffer.h. */
void framebuffer_MZ800_resolution_switch ( unsigned old_dmd, unsigned new_dmd, unsigned lead ) {
    if ( ( ( old_dmd ^ new_dmd ) & 0x0c ) != 0x04 ) return;          /* jen 320 <-> 640 v režimu MZ-800 */
    if ( ( ( old_dmd ^ new_dmd ) & 0x03 ) != 0 ) return;             /* bity 0 a 1 beze změny */
    if ( old_dmd & 0x01 ) return;                                    /* frame B neověřeno */

    unsigned row;
    unsigned col = gdg_get_beam_pos ( g_gdg.total_elapsed.ticks, &row );
    if ( row != g_gdg.beam_row ) return;
    if ( ( row < VIDEO_BEAM_CANVAS_FIRST_ROW ) || ( row > VIDEO_BEAM_CANVAS_LAST_ROW ) ) return;
    if ( col <= lead + VIDEO_BEAM_CANVAS_FIRST_COLUMN ) return;
    unsigned x = ( col - lead - VIDEO_BEAM_CANVAS_FIRST_COLUMN + 15u ) & ~15u;
    if ( ( x < 16 ) || ( x >= VIDEO_CANVAS_WIDTH ) ) return;

    /* Bajty buňky c = k - 1 (k = x / 16) téhož řádku VRAM. */
    unsigned addr = hwscroll_shift_addr ( g_gdg.row_addr_of_row [ row ] * 40u + ( x / 16 ) - 1 );
    uint8_t *p = framebuffer_canvas_row ( row ) + x - 6;              /* sloupec 148 + 16k */
    unsigned m = old_dmd & 0x02;

    if ( old_dmd & 0x04 ) {
        /* 640 -> 320: lichý bajt o (rovina II, s rovinou IV v 06h) buňky c. */
        unsigned o = g_memoryVRAM_II [ addr ], o3 = g_memoryVRAM_IV [ addr ];
        uint8_t c4 = framebuffer_color640 ( old_dmd, o >> 4, o3 >> 4 );
        unsigned b5 = ( o >> 5 ) & 1, b5h = ( o3 >> 5 ) & 1, b6 = ( o >> 6 ) & 1, b6h = ( o3 >> 6 ) & 1;
        uint8_t c5 = framebuffer_color320 ( new_dmd, b5 | ( b5 << 1 ) | ( ( m ? b5h : 0 ) << 2 ) | ( ( m ? b5h : 0 ) << 3 ) );
        uint8_t c6 = framebuffer_color320 ( new_dmd, b6 | ( b6 << 1 ) | ( ( m ? b6h : 0 ) << 2 ) | ( ( m ? b6h : 0 ) << 3 ) );
        p[0] = c4; p[1] = c4;
        p[2] = c5; p[3] = c5;
        p[4] = c6; p[5] = c6;
        return;
    };

    /* 320 -> 640: sloupec 148 zůstává (bod 5 režimu 320), 149 a 150 bity 6
     * a 7 bajtu roviny I (s rovinou III v 06h) buňky c v barvách 640, 151
     * položka 1 (04h) nebo 3 (06h); od 152 normální 640 buňky k. */
    unsigned i1 = g_memoryVRAM_I [ addr ], i3 = g_memoryVRAM_III [ addr ];
    p[1] = framebuffer_color640 ( new_dmd, i1 >> 6, i3 >> 6 );
    p[2] = framebuffer_color640 ( new_dmd, i1 >> 7, i3 >> 7 );
    p[3] = framebuffer_color640 ( new_dmd, 1, 1 );
    s_sw640_row = row;
    s_sw640_x = x;
}


/* Nulování stavu kreslení vázaného na řádek, viz mz800_framebuffer.h. */
void framebuffer_row_state_reset ( void ) {
    s_row640_row = ~0u;
    s_row640_done = 0;
    s_sw640_row = ~0u;
}


/* Přepnutí z a do režimu MZ-700, viz mz800_framebuffer.h. */
void framebuffer_mode700_switch ( int x, int to700, int done ) {
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
    unsigned row = g_gdg.beam_row;
    if ( ( row < VIDEO_BEAM_CANVAS_FIRST_ROW ) || ( row > VIDEO_BEAM_CANVAS_LAST_ROW ) ) return;

    unsigned ux = ( x <= 0 ) ? 0u : ( x >= VIDEO_CANVAS_WIDTH ) ? VIDEO_CANVAS_WIDTH : (unsigned) x;

    if ( to700 ) {
        if ( !done ) {
            /* Před změnou DMD: grafika po místo přepnutí. */
            framebuffer_MZ800_current_screen_row_fill ( ux );
            return;
        };
        /* Po změně DMD: zbytek řádku v režimu MZ-700 hned; HBLN_END tohoto
         * řádku (je-li ještě před námi) řádek už nepřekreslí. */
        if ( ux > 0 ) {
            framebuffer_MZ700_current_row_range ( ux, VIDEO_CANVAS_WIDTH );
            g_gdg.mz700_skip_row = row + 1;
        };
        return;
    };

    if ( done ) return;
    /* Z režimu MZ-700: část před místem přepnutí v režimu MZ-700 hned,
     * grafika od místa přepnutí až s paprskem (paleta zapsaná potom platí). */
    framebuffer_MZ700_current_row_range ( 0, ux );
    g_gdg.screen_need_update_from = ux & ~15u;
    g_gdg.canvas_split_x = ux;
}


/** Kam byl řádek nakreslený před vrácením kreslení ve framebuffer_MZ800_reg_write_begin(); 0 = nic. */
static unsigned s_reg_write_refill_to;


/* Začátek zápisu do registru měnícího canvas, viz mz800_framebuffer.h. */
void framebuffer_MZ800_reg_write_begin ( unsigned lead, int align16 ) {
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
    s_reg_write_refill_to = 0;

    unsigned row;
    unsigned col = gdg_get_beam_pos ( g_gdg.total_elapsed.ticks, &row );

    /* Konec M-cyklu až v dalším řádku, jehož začátek ještě není zpracovaný:
     * místo změny leží v zatemnění nebo v levém borderu, canvas se nemění. */
    if ( row != g_gdg.beam_row ) return;
    if ( ( row < VIDEO_BEAM_CANVAS_FIRST_ROW ) || ( row > VIDEO_BEAM_CANVAS_LAST_ROW ) ) return;
    if ( col <= lead + VIDEO_BEAM_CANVAS_FIRST_COLUMN ) return;

    unsigned x = col - lead - VIDEO_BEAM_CANVAS_FIRST_COLUMN;
    if ( align16 ) x = ( x + 15u ) & ~15u;
    /* Změna na konci canvasu nebo za ním: celý řádek patří staré hodnotě. */
    if ( x > VIDEO_CANVAS_WIDTH ) x = VIDEO_CANVAS_WIDTH;

    if ( x >= g_gdg.screen_need_update_from ) {
        framebuffer_MZ800_current_screen_row_fill ( x );
        return;
    };

    /* Řádek je nakreslený za místo změny. Kreslení se vrátí na začátek bajtu
     * (tam se znovu načtou data z VRAM), po místo změny se nakreslí starou
     * hodnotou a zbytek doplní framebuffer_MZ800_reg_write_end() novou. */
    s_reg_write_refill_to = g_gdg.screen_need_update_from;
    g_gdg.screen_need_update_from = x & ~15u;
    framebuffer_MZ800_current_screen_row_fill ( x );
}


/* Konec zápisu do registru měnícího canvas, viz mz800_framebuffer.h. */
void framebuffer_MZ800_reg_write_end ( void ) {
    if ( s_reg_write_refill_to ) {
        framebuffer_MZ800_current_screen_row_fill ( s_reg_write_refill_to );
        s_reg_write_refill_to = 0;
    };
}


void framebuffer_MZ800_all_screen_rows_fill ( void ) {
    int i;
    g_gdg.screen_need_update_from = 0;
    int mode640 = ( g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640 ) ? 1 : 0;
    for ( i = VIDEO_BEAM_CANVAS_FIRST_ROW; i <= VIDEO_BEAM_CANVAS_LAST_ROW; i++ ) {
        if ( mode640 ) {
            /* Režim 640: bod i v sloupci 152 + i (viz framebuffer_MZ800_fill_640()). */
            framebuffer_MZ800_screen_row_fill ( i, VIDEO_CANVAS_WIDTH, s_row640 );
            memcpy ( g_framebuffer.pixels + i * VIDEO_DISPLAY_WIDTH + FB_CANVAS640_FIRST_COLUMN, s_row640, VIDEO_CANVAS_WIDTH );
        } else {
            framebuffer_MZ800_screen_row_fill ( i, VIDEO_CANVAS_WIDTH, framebuffer_canvas_row ( i ) );
        };
    };
    s_row640_row = ~0u;
}


void framebuffer_border_all_rows_fill ( void ) {

    int i;
    /* Geometrie podle současného DMD pro všechny řádky (režim 640: canvas
     * o 2 sloupce dřív, border končí o 1 sloupec dřív). */
    unsigned g640 = GDG_DMD_IS_GEOM640 ( g_gdg.regDMD );
    unsigned left_end = g640 ? FB_CANVAS640_FIRST_COLUMN : VIDEO_BORDER_LEFT_WIDTH;
    unsigned right_first = g640 ? ( VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN - 2 ) : VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN;
    unsigned vis_end = g640 ? ( VIDEO_DISPLAY_WIDTH - 1 ) : VIDEO_DISPLAY_WIDTH;

    for ( i = 0; i < VIDEO_DISPLAY_HEIGHT; i++ ) {
        uint8_t *p = g_framebuffer.pixels + ( i * VIDEO_DISPLAY_WIDTH );
        if ( ( i < VIDEO_BORDER_TOP_HEIGHT ) || ( i >= VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT ) ) {
            memset ( p, g_gdg.regBOR, vis_end );
        } else {
            memset ( p, g_gdg.regBOR, left_end );
            memset ( p + right_first, g_gdg.regBOR, vis_end - right_first );
        };
        if ( vis_end < VIDEO_DISPLAY_WIDTH ) p [ vis_end ] = 0x00;
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
        /* Stav kreslení, který platí jen pro aktuální řádek (část řádku
         * v režimu MZ-700, geometrie 640 v okamžiku hran); ostatní řádky se
         * kreslí podle DMD a po smyčce se stav aktuálního řádku vrátí. */
        unsigned saved_split = g_gdg.canvas_split_x;
        unsigned saved_skip = g_gdg.mz700_skip_row;
        unsigned saved_cs = g_gdg.geom640_cs, saved_ce = g_gdg.geom640_ce, saved_be = g_gdg.geom640_be;

        do
        {
            if (g_framebuffer.screen_changes)
            {
                if ((g_gdg.beam_row >= VIDEO_BEAM_CANVAS_FIRST_ROW) && (g_gdg.beam_row <= VIDEO_BEAM_CANVAS_LAST_ROW))
                {
                    if (!GDG_MZ800_DMD_TEST_MZ700)
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

            if (g_gdg.beam_row == beam_row)
            {
                g_gdg.canvas_split_x = 0;
                g_gdg.mz700_skip_row = 0;
                gdg_geometry_row_start();
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

        g_gdg.canvas_split_x = saved_split;
        g_gdg.mz700_skip_row = saved_skip;
        g_gdg.geom640_cs = saved_cs;
        g_gdg.geom640_ce = saved_ce;
        g_gdg.geom640_be = saved_be;

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
