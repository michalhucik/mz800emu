/*
 * File:   mz1500_framebuffer.c
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

/*
 *
 * MZ-1500 framebuffer — znakovy rendering s PCG overlay
 *
 * VRAM layout (4 KB na 0xD000-0xDFFF):
 *   0x000-0x3FF: CHAR VRAM — kody znaku z CGROM (40x25 = 1000 bajtu)
 *   0x400-0x7FF: PCGLO VRAM — dolnich 8 bitu PCG adresy
 *   0x800-0xBFF: ATTR VRAM — atributy (fg/bg barva, CGRAM/CGROM)
 *   0xC00-0xFFF: PCGHI VRAM — hornich 2 bity PCG adresy + PCG enable
 *
 * Rezimy zobrazeni (DMD registr, port 0xF0):
 *   bit 0 = 0: MZ-700 kompatibilni rezim (jen CHAR + ATTR)
 *   bit 0 = 1: MZ-1500 rezim (CHAR + ATTR + PCG overlay)
 *   bit 1: priorita vrstev — 0 = BPF, 1 = BFP
 *
 * Priorita vrstev:
 *   BPF: pozadi → PCG → popredi (foreground prepise PCG)
 *   BFP: pozadi → popredi → PCG (PCG prepise foreground)
 *
 */
#include "hw-generic/memory/memory_arch.h" /* per-arch memory - dříve tranzitivně přes memory.h (mzhal 11c-2c) */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mz1500_framebuffer.h"
#include "mz1500_framebuffer_done.h"
#include "mz1500_gdg.h"
#include "hw-generic/gdg/video.h"
#include "memory/memory.h"
#include "iface/iface_video.h"

gdg_framebuffer_t g_framebuffer;

/* MZ-700/1500 barvy: index 0-7 → IGRB hodnota v pixbuf */
static const uint8_t c_MZ700_COLORMAP[8] = {0, 9, 10, 11, 12, 13, 14, 15};

/* VRAM offsety pro MZ-1500 */
#define MZ1500_VRAM_CHAR  0x000
#define MZ1500_VRAM_PCGLO 0x400
#define MZ1500_VRAM_ATTR  0x800
#define MZ1500_VRAM_PCGHI 0xC00

/* MZ-1500: bit 3 v PCGHI = PCG povoleno na teto pozici */
#define MZ1500_ATTRPCG_ENABLED 0x08

/* pocet znaku na radek */
#define CHARS_WIDTH 40


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


/*
 * MZ-700 kompatibilni rezim: znakovy rendering s CGROM
 */
static inline void framebuffer_update_MZ700_screen_row(unsigned beam_row)
{
    uint8_t *CGROM = g_rom.cgrom;
    uint8_t *VRAM_CHAR = &g_memory.VRAM[MZ1500_VRAM_CHAR];
    uint8_t *VRAM_ATTR = &g_memory.VRAM[MZ1500_VRAM_ATTR];

    uint8_t *p = g_framebuffer.pixels + (beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH);

    unsigned pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW;
    unsigned vram_addr = (pos_Y / 8) * CHARS_WIDTH;
    unsigned char_line = pos_Y % 8;

    uint8_t *vram_data = &VRAM_CHAR[vram_addr];
    uint8_t *vram_attr = &VRAM_ATTR[vram_addr];

    for (unsigned char_x = 0; char_x < CHARS_WIDTH; char_x++)
    {
        unsigned attr_fgc = (*vram_attr >> 4) & 0x07;
        unsigned attr_bgc = *vram_attr & 0x07;
        unsigned attr_grp = ((*vram_attr << 4) & 0x0800);

        unsigned cg_addr = attr_grp | (*vram_data * 8);
        unsigned mask = CGROM[cg_addr + char_line];

        vram_data++;
        vram_attr++;

        uint8_t fgc = c_MZ700_COLORMAP[attr_fgc];
        uint8_t bgc = c_MZ700_COLORMAP[attr_bgc];

        /* 8 znakovych pixelu → 16 display pixelu (pixel doubling) */
        p[0] = (mask & 1) ? fgc : bgc;
        p[1] = p[0];
        mask >>= 1;

        p[2] = (mask & 1) ? fgc : bgc;
        p[3] = p[2];
        mask >>= 1;

        p[4] = (mask & 1) ? fgc : bgc;
        p[5] = p[4];
        mask >>= 1;

        p[6] = (mask & 1) ? fgc : bgc;
        p[7] = p[6];
        mask >>= 1;

        p[8] = (mask & 1) ? fgc : bgc;
        p[9] = p[8];
        mask >>= 1;

        p[10] = (mask & 1) ? fgc : bgc;
        p[11] = p[10];
        mask >>= 1;

        p[12] = (mask & 1) ? fgc : bgc;
        p[13] = p[12];
        mask >>= 1;

        p[14] = (mask & 1) ? fgc : bgc;
        p[15] = p[14];

        p += 16;
    }
}


void framebuffer_update_MZ700_current_screen_row(void)
{
    framebuffer_update_MZ700_screen_row(g_gdg.beam_row);
}


void framebuffer_update_MZ700_all_rows(void)
{
    for (int i = VIDEO_BEAM_CANVAS_FIRST_ROW; i <= VIDEO_BEAM_CANVAS_LAST_ROW; i++)
    {
        framebuffer_update_MZ700_screen_row(i);
    }
}


/*
 * MZ-1500 rezim: znakovy rendering s PCG overlay
 *
 * Kazdy znak ma 8x8 pixelu z CGROM + volitelny PCG overlay.
 * PCG overlay: 3 PCG banky daji 3-bitovy barevny index.
 * Priorita vrstev: BPF (pozadi→PCG→popredi) nebo BFP (pozadi→popredi→PCG).
 */
static inline void framebuffer_MZ1500_screen_row_fill(unsigned beam_row)
{
    uint8_t *CGROM = g_rom.cgrom;
    uint8_t *VRAM = g_memory.VRAM;
    uint8_t *PCG0 = g_memoryPCG1;
    uint8_t *PCG1 = g_memoryPCG2;
    uint8_t *PCG2 = g_memoryPCG3;

    /* PCG barevna paleta: mode_color[i] → IGRB hodnota */
    uint8_t pcg_colormap[8];
    for (int i = 0; i < 8; i++)
    {
        pcg_colormap[i] = c_MZ700_COLORMAP[g_gdg.mode_color[i]];
    }

    uint8_t *p = g_framebuffer.pixels + (beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH);

    unsigned pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW;
    unsigned vram_addr = (pos_Y / 8) * CHARS_WIDTH;
    unsigned char_line = pos_Y % 8;

    uint8_t *char_p = &VRAM[MZ1500_VRAM_CHAR + vram_addr];
    uint8_t *attr_p = &VRAM[MZ1500_VRAM_ATTR + vram_addr];
    uint8_t *pcglo_p = &VRAM[MZ1500_VRAM_PCGLO + vram_addr];
    uint8_t *pcghi_p = &VRAM[MZ1500_VRAM_PCGHI + vram_addr];

    for (unsigned char_x = 0; char_x < CHARS_WIDTH; char_x++)
    {
        unsigned attr_fgc = (*attr_p >> 4) & 0x07;
        unsigned attr_bgc = *attr_p & 0x07;
        unsigned attr_grp = ((*attr_p << 4) & 0x0800);

        unsigned cg_addr = attr_grp | (*char_p * 8);
        unsigned mask = CGROM[cg_addr + char_line];

        uint8_t fgc = c_MZ700_COLORMAP[attr_fgc];
        uint8_t bgc = c_MZ700_COLORMAP[attr_bgc];

        if (*pcghi_p & MZ1500_ATTRPCG_ENABLED)
        {
            /* PCG je na teto pozici povoleno */
            unsigned pcg_addr = (((*pcghi_p & 0xC0) << 2) | *pcglo_p) * 8;

            unsigned pcg0_mask = PCG0[pcg_addr + char_line];
            unsigned pcg1_mask = PCG1[pcg_addr + char_line];
            unsigned pcg2_mask = PCG2[pcg_addr + char_line];

            for (int bit = 0; bit < 8; bit++)
            {
                /* PCG barva: 3 bity z hornich bitu PCG bank */
                unsigned pcg_color_id = ((pcg0_mask & 0x80) >> 7) |
                                        ((pcg1_mask & 0x80) >> 6) |
                                        ((pcg2_mask & 0x80) >> 5);

                if (GDG_MZ1500_DMD_TEST_PMODE_BFP)
                {
                    /* BFP: pozadi → popredi → PCG */
                    uint8_t char_color = (mask & 1) ? fgc : bgc;
                    p[0] = char_color;
                    p[1] = char_color;
                    if (pcg_color_id != 0)
                    {
                        uint8_t pcg_color = pcg_colormap[pcg_color_id];
                        p[0] = pcg_color;
                        p[1] = pcg_color;
                    }
                }
                else
                {
                    /* BPF: pozadi → PCG → popredi */
                    uint8_t pcg_color = pcg_colormap[pcg_color_id];
                    p[0] = pcg_color;
                    p[1] = pcg_color;
                    if (mask & 1)
                    {
                        p[0] = fgc;
                        p[1] = fgc;
                    }
                }

                p += 2;
                mask >>= 1;
                pcg0_mask <<= 1;
                pcg1_mask <<= 1;
                pcg2_mask <<= 1;
            }
        }
        else
        {
            /* PCG neni povoleno — zobrazujeme jako v mode700 */
            for (int bit = 0; bit < 8; bit++)
            {
                uint8_t color = (mask & 1) ? fgc : bgc;
                p[0] = color;
                p[1] = color;
                p += 2;
                mask >>= 1;
            }
        }

        char_p++;
        attr_p++;
        pcglo_p++;
        pcghi_p++;
    }
}


/*
 * Wrapper pro kompatibilitu s event handlerem.
 * Parametr last_pixel se ignoruje — MZ-1500 renderuje po celych radcich.
 */
void framebuffer_MZ800_current_screen_row_fill(unsigned last_pixel)
{
    (void)last_pixel;
    framebuffer_MZ1500_screen_row_fill(g_gdg.beam_row);
}


void framebuffer_MZ800_all_screen_rows_fill(void)
{
    for (int i = VIDEO_BEAM_CANVAS_FIRST_ROW; i <= VIDEO_BEAM_CANVAS_LAST_ROW; i++)
    {
        framebuffer_MZ1500_screen_row_fill(i);
    }
}


/*
 *
 * Na konci viditelneho radku zaktualizujeme aktualni radek borderu ve framebufferu.
 *
 */
void framebuffer_border_current_row_fill(void)
{
    /* Pokud uz je radek ve framebufferu hotov, tak jdeme pryc */
    if (g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1)
        return;

    unsigned beam_row = g_gdg.beam_row;
    uint8_t *p = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH;

    if ((beam_row < VIDEO_BORDER_TOP_HEIGHT) || (beam_row > VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT - 1))
    {
        memset(&p[g_gdg.last_updated_border_pixel], g_gdg.regBOR, VIDEO_DISPLAY_WIDTH - g_gdg.last_updated_border_pixel);
    }
    else
    {
        if (g_gdg.last_updated_border_pixel < VIDEO_BORDER_LEFT_WIDTH)
        {
            memset(&p[g_gdg.last_updated_border_pixel], g_gdg.regBOR, VIDEO_BORDER_LEFT_WIDTH - g_gdg.last_updated_border_pixel);
        }
        unsigned pos_x = (g_gdg.last_updated_border_pixel >= VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH) ? g_gdg.last_updated_border_pixel : VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH;
        memset(&p[pos_x], g_gdg.regBOR, VIDEO_DISPLAY_WIDTH - pos_x);
    }
}


/*
 *
 *  Prave doslo ke zmene borderu. Nez provedeme zmenu, tak zaktualizujeme aktualni radek az po misto, kde nastala zmena.
 *
 */
void framebuffer_border_changed(void)
{
    g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;

    unsigned beam_col = VIDEO_GET_SCREEN_COL(g_gdg.total_elapsed.ticks);
    unsigned beam_row = VIDEO_GET_SCREEN_ROW(g_gdg.total_elapsed.ticks);

    /* Osetreni situace, kdy OUT provedl zmenu na hranici mezi dvema snimky. */
    if (beam_row == VIDEO_SCREEN_HEIGHT)
    {
        beam_row = 0;
    }

    /* Pokud jsme na radku, ktery je mimo viditelnou oblast, tak neni potreba aktualizovat framebuffer. */
    if (beam_row > VIDEO_BEAM_DISPLAY_LAST_ROW)
        return;

    /* Pokud je tento radek ve framebufferu uz hotov, tak muzeme jit pryc. */
    if ((beam_row == g_gdg.beam_row) && (g_gdg.last_updated_border_pixel == VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1))
        return;

    /* Pokud doslo ke zmene na zacatku framebufferu, tak neni co updatovat. */
    if (beam_col == 0)
        return;

    /* Osetreni maximalni horni hranice paprsku na radku. */
    beam_col = (beam_col > VIDEO_BEAM_DISPLAY_LAST_COLUMN) ? (VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1) : beam_col;

    if (beam_row != g_gdg.beam_row)
    {
        g_gdg.last_updated_border_pixel = 0;
    }

    /*
     *  Nyni provedeme aktualizaci framebufferu.
     *  Ulozime stav od posledni aktualizace po nynejsi pozici - 1.
     *  Do last_updated ulozime pozici od ktere bude platit novy stav.
     */
    uint8_t *p = g_framebuffer.pixels + beam_row * VIDEO_DISPLAY_WIDTH;

    /* Jsme v hornim, nebo dolnim borderu? */
    if ((beam_row <= VIDEO_BEAM_BORDER_TOP_LAST_ROW) || (beam_row >= VIDEO_BEAM_BORDER_BOTOM_FIRST_ROW))
    {
        memset(&p[g_gdg.last_updated_border_pixel], g_gdg.regBOR, beam_col - g_gdg.last_updated_border_pixel);
        g_gdg.last_updated_border_pixel = beam_col;
    }
    else
    {
        /* Je potreba aktualizovat levy border? */
        if (g_gdg.last_updated_border_pixel <= VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN)
        {
            unsigned last_pixel = (beam_col < VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN) ? beam_col : VIDEO_BEAM_BORDER_LEFT_LAST_COLUMN + 1;
            memset(&p[g_gdg.last_updated_border_pixel], g_gdg.regBOR, last_pixel - g_gdg.last_updated_border_pixel);
            g_gdg.last_updated_border_pixel = last_pixel;
        }

        /* Je potreba aktualizovat pravy border? */
        if (beam_col >= VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN)
        {
            unsigned start_pixel = (g_gdg.last_updated_border_pixel >= VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN) ? g_gdg.last_updated_border_pixel : VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN;
            memset(&p[start_pixel], g_gdg.regBOR, beam_col - start_pixel);
            g_gdg.last_updated_border_pixel = beam_col;
        }
    }
}


/*
 * MZ-1500 nema MZ-800 graficke rezimy — jen nastavime flag zmeny
 */
void framebuffer_MZ800_screen_changed(void)
{
    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
}


void framebuffer_border_all_rows_fill(void)
{
    int i;

    for (i = 0; i < VIDEO_BORDER_TOP_HEIGHT; i++)
    {
        uint8_t *p = g_framebuffer.pixels + (i * VIDEO_DISPLAY_WIDTH);
        memset(p, g_gdg.regBOR, VIDEO_DISPLAY_WIDTH);
    }

    for (i = VIDEO_BORDER_TOP_HEIGHT; i < (VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_LEFT_HEIGHT); i++)
    {
        uint8_t *p = g_framebuffer.pixels + (i * VIDEO_DISPLAY_WIDTH);
        memset(p, g_gdg.regBOR, VIDEO_BORDER_LEFT_WIDTH);
    }

    for (i = VIDEO_BORDER_TOP_HEIGHT; i < (VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_RIGHT_HEIGHT); i++)
    {
        uint8_t *p = g_framebuffer.pixels + (i * VIDEO_DISPLAY_WIDTH) + (VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH);
        memset(p, g_gdg.regBOR, VIDEO_BORDER_RIGHT_WIDTH);
    }

    for (i = (VIDEO_BORDER_TOP_HEIGHT + VIDEO_BORDER_RIGHT_HEIGHT); i < VIDEO_DISPLAY_HEIGHT; i++)
    {
        uint8_t *p = g_framebuffer.pixels + (i * VIDEO_DISPLAY_WIDTH);
        memset(p, g_gdg.regBOR, VIDEO_DISPLAY_WIDTH);
    }
}


/*
 *
 * Sem si odskocime, pokud jsme zastavili emulator v cmthack a potrebujeme vykreslit kompletni screen.
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
                        framebuffer_MZ1500_screen_row_fill(g_gdg.beam_row);
                    }
                    else
                    {
                        framebuffer_update_MZ700_screen_row(g_gdg.beam_row);
                    }
                }
                g_gdg.screen_need_update_from = 0;
            }

            if (g_framebuffer.border_changes)
            {
                if (g_gdg.beam_row <= VIDEO_BEAM_DISPLAY_LAST_ROW)
                {
                    framebuffer_border_current_row_fill();
                    g_gdg.last_updated_border_pixel = 0;
                }
            }

            if (g_gdg.beam_row < VIDEO_SCREEN_HEIGHT - 1)
            {
                g_gdg.beam_row++;
            }
            else
            {
                g_gdg.beam_row = 0;
            }

        } while (g_gdg.beam_row != beam_row);

        if (g_framebuffer.screen_changes)
        {
            g_framebuffer.framebuffer_state |= FB_STATE_SCREEN_CHANGED;
        }

        if (g_framebuffer.border_changes)
        {
            g_framebuffer.framebuffer_state |= FB_STATE_BORDER_CHANGED;
        }
    }

    if (g_framebuffer.framebuffer_state || iface_video_get_redraw_full_screen_request())
    {
        framebuffer_screen_done();
        g_framebuffer.framebuffer_state = FB_STATE_NOT_CHANGED;
    }
}
