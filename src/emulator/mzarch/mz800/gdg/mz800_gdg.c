/*
 * File:   mz800_gdg.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:32
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
 *
 *	Registry:
 *
 *		DMD (0xce):
 *			3. bit	- 0 = MZ800, 1 = MZ700
 *			2. bit	- jen MZ800: 0 = 320x200, 1 = 640x200
 *			1. bit	- jen MZ800: 0 = LOW_COLOR, 1 = HI_COLOR (extVRAM)
 *			0. bit	- jen MZ800: 0 = VBANK_A, 1 = VBANK_B (extVRAM)
 *
 *
 *		BOR (0x06cf):
 *			0. - 3. bit	- IGRB barva borderu
 *
 *
 *		PAL (0xf0):
 *			6. bit	- 0 = predvolba barvy v palete
 *					4. az 5. bit - kombinace rovin (cislo barvy): 0 - 3
 *					0. az 3. bit - IGRB barva
 *
 *				  1 = volba aktualni skupiny (PALGRP)
 *					0. - 1. bit - cislo skupiny: 0 - 3
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mz800_gdg.h"
#include "mz800_vramctrl.h"
#include "mz800_hwscroll.h"
#include "mz800_framebuffer.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "mzarch/mzarch.h"
#include "mzarch/mz800/memory/mz800_memory.h"

#ifdef MZ800EMU_CFG_RAM_FASTPATH
/* E1: DMD switch (700/800 mode, SCRW640) meni VRAM mapping -> nutny rebuild
 * RAM fast-path tabulky. mz800_ram_fastpath_rebuild deklarace. */
#include "mzarch/mz800/memory/mz800_memory.h"
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/trace/hwlog.h"
#include "debugger/bp_event.h"
#endif

// nedelej framebuffer
// #define framebuffer_border_changed()
// #define framebuffer_MZ800_screen_changed()

st_GDG g_gdg;

/* Eventy musi byt serazeny vzestupne podle event_column ! */
const struct st_GDGEVENT g_gdgevent[] = {

    /* row: ALL, col: 150 */
    {MZEVENT_GDG_HBLN_END, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_CANVAS_FIRST_COLUMN - 4},

    /* row: ALL, col: 790 */
    /* + row: 45, col: 790 - VBLN_END */
    /* + row: 245, col: 790 - VBLN_START */
    {MZEVENT_GDG_HBLN_START, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_HBLN_FIRST_COLUMN},

    /* row: 0, col: 792 */
    {MZEVENT_GDG_STS_VSYNC_END, 0, 1, VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2},

    /* row: 287, col: 792 */
    {MZEVENT_GDG_STS_VSYNC_START, VIDEO_DISPLAY_HEIGHT - 1, 1, VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2},

    /* row: 46 - 245, col: 794 */
    {MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL, VIDEO_BEAM_CANVAS_FIRST_ROW, VIDEO_CANVAS_HEIGHT, VIDEO_BEAM_BORDER_RIGHT_FIRST_COLUMN},

    /* row: ALL, col: 928 - STS_HSYNC start (z duvodu uspory ho spustime trochu drive ) */
    /* row: 0 - 287, col: 928 */
    //{ MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL, 0, DISPLAY_VISIBLE_HEIGHT, DISPLAY_VISIBLE_LAST_COLUMN + 1 },
    {MZEVENT_GDG_AFTER_LAST_VISIBLE_PIXEL, 0, VIDEO_SCREEN_HEIGHT, VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1},

    /* row: ALL, col: 926 - podle mych mereni zacina zde */
    //{ MZEVENT_GDG_STS_HSYNC_START, 0, BEAM_TOTAL_ROWS, 926 },

    /* row: ALL, col: 950 - realny HSYNC ma delku 80 px, jeho konec nas ale nezajima */
    {MZEVENT_GDG_REAL_HSYNC_START, 0, VIDEO_SCREEN_HEIGHT, VIDEO_REAL_HSYNC_START_COLUMN},

    /* row: ALL, col: 1133 - podle mych mereni konci zde */
    //{ MZEVENT_GDG_STS_HSYNC_END, 0, BEAM_TOTAL_ROWS, 1133 },

    /* row: ALL, col: 1135 STS_HSYNC end (z duvodu uspory jej ukoncime malinko pozdeji) */
    /* row: ALL, col: 1135 */
    {MZEVENT_GDG_SCREEN_ROW_END, 0, VIDEO_SCREEN_HEIGHT, VIDEO_SCREEN_WIDTH},
};

void gdg_init(void)
{
    g_gdg.tempo_rows = GDG_TEMPO_DIVIDER_ROWS;

    g_gdg.total_elapsed.ticks = 0;
    g_gdg.total_elapsed.screens = 0;

    g_gdg.event.event_name = 0;
    g_gdg.event.ticks = g_gdgevent[g_gdg.event.event_name].event_column;

    g_gdg.hbln = HBLN_ACTIVE;
    g_gdg.vbln = VBLN_ACTIVE;

    g_gdg.sts_hsync = HSYN_OFF;
    g_gdg.sts_vsync = VSYN_ACTIVE;

    g_gdg.beam_row = 0;
    g_gdg.screen_is_already_rendered_at_beam_pos = 0;

    g_gdg.screen_need_update_from = 0;
    g_gdg.last_updated_border_pixel = 0;

    g_gdg.tempo_divider = 0;
    g_gdg.tempo = 0;

    g_gdg.regct53g7 = 0;
    g_gdg.cksw = 0;
    g_gdg.row_shrink = 0;
    g_gdg.frame_short_ticks = 0;
    g_gdg.short_ticks_total = 0;

    hwscroll_init();
    framebuffer_init();

    /* Model WAIT u VRAM - žádný rozpracovaný zápis. */
    g_gdg.vram_wrff_release = 0;

#ifdef MZ800EMU_CFG_CLK1M1_SLOW
    g_gdg.ctc0clk = 0;
#endif

    gdg_row_counter_rebuild();
    gdg_geometry_row_start();
}


/**
 * @brief Počet řádků rastru od prvního řádku canvasu (0 až VIDEO_SCREEN_HEIGHT - 1).
 * @param row Řádek rastru emulátoru.
 * @return (row - VIDEO_BEAM_CANVAS_FIRST_ROW) mod VIDEO_SCREEN_HEIGHT.
 */
static inline unsigned gdg_rows_from_canvas(unsigned row)
{
    return (row + VIDEO_SCREEN_HEIGHT - VIDEO_BEAM_CANVAS_FIRST_ROW) % VIDEO_SCREEN_HEIGHT;
}


/* Posun čítače adresy řádku, viz mz800_gdg.h. */
void gdg_row_counter_advance(void)
{
    unsigned next = (g_gdg.beam_row + 1) % VIDEO_SCREEN_HEIGHT;
    if (next == VIDEO_BEAM_CANVAS_FIRST_ROW)
    {
        g_gdg.row_addr_next = 0;
        g_gdg.char_row_next = 0;
        return;
    };
    g_gdg.char_row_next = (g_gdg.char_row + 1) & 0x07;
    if (!(g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700) || (g_gdg.char_row_next == 0))
    {
        g_gdg.row_addr_next = g_gdg.row_addr + 1;
    }
    else
    {
        g_gdg.row_addr_next = g_gdg.row_addr;
    };
}


/* Převzetí čítače na začátku řádku, viz mz800_gdg.h. */
void gdg_row_counter_enter_row(void)
{
    g_gdg.row_addr = g_gdg.row_addr_next;
    g_gdg.char_row = g_gdg.char_row_next;
    g_gdg.row_addr_of_row[g_gdg.beam_row] = (uint16_t)g_gdg.row_addr;
}


/* Geometrie řádku podle DMD, viz mz800_gdg.h. */
void gdg_geometry_row_start(void)
{
    unsigned g = GDG_DMD_IS_GEOM640(g_gdg.regDMD);
    g_gdg.geom640_cs = g;
    g_gdg.geom640_ce = g;
    g_gdg.geom640_be = g;
}


/**
 * @brief Nejzazší konec M-cyklu zápisu DMD (sloupec), který ještě změní
 *        začátek canvasu, konec canvasu a konec borderu aktuálního řádku.
 *
 * O každé hraně řádku rozhoduje poslední hrana registru DMD (334 + 16k px)
 * před ní, která zápis převezme, když sestupná hrana /IORQ P přišla nejpozději
 * 1,5 px před ní (GDG_DMD_REG_WR_LEAD; konec M-cyklu E leží v P - 170,5 px,
 * tedy E <= sloupec hrany + 14):
 *  - začátek canvasu: hrana 334 px (sloupec 148), E <= 162. Změřeno po 1 px
 *    (mz800-knowledge hw-measurement/vysledky.md, oddíl 18: P 332,5 / 333,5);
 *  - konec canvasu: hrana 974 px (sloupec 788), E <= 802. Na stroji změřeno
 *    jen rozmezí P 969,5 až 973,5 px s mezistavy; hrana podle C modelu
 *    netlistu (mez modelu 974,5 / 975,5, o 2 px později než stroj jako
 *    u změřených hran) [uvnitř rozmezí neověřeno];
 *  - konec borderu: hrana 1102 px (sloupec 916), E <= 930. Na stroji rozmezí
 *    P 1087,5 až 1106,5 px, C model 1102,5 / 1103,5 [uvnitř rozmezí neověřeno].
 * Mezistavy uvnitř konce canvasu (zápis kolem 950 až 970 px) se neemulují.
 */
#define GDG_GEOM_CS_LAST_E 162u
#define GDG_GEOM_CE_LAST_E 802u
#define GDG_GEOM_BE_LAST_E 930u

/**
 * @brief Promítne zápis DMD do geometrie aktuálního řádku.
 *
 * @param value Nová hodnota DMD.
 * @pre Emu vlákno; čas GDG je konec M-cyklu zápisu.
 * @post Hrany, o kterých zápis ještě rozhodl, mají geometrii nového režimu;
 *       zápis s koncem M-cyklu až v dalším řádku aktuální řádek nemění
 *       (další řádek začne podle DMD).
 */
static void gdg_geometry_dmd_written(uint8_t value)
{
    unsigned row;
    unsigned col = gdg_get_beam_pos(g_gdg.total_elapsed.ticks, &row);
    if (row != g_gdg.beam_row)
        return;
    unsigned g = GDG_DMD_IS_GEOM640(value);
    if (col <= GDG_GEOM_CS_LAST_E)
        g_gdg.geom640_cs = g;
    if (col <= GDG_GEOM_CE_LAST_E)
        g_gdg.geom640_ce = g;
    if (col <= GDG_GEOM_BE_LAST_E)
        g_gdg.geom640_be = g;
}


/* Ustálený stav čítače podle DMD, viz mz800_gdg.h. */
void gdg_row_counter_rebuild(void)
{
    int mode700 = (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700) ? 1 : 0;
    unsigned row;
    for (row = 0; row < VIDEO_SCREEN_HEIGHT; row++)
    {
        unsigned c = gdg_rows_from_canvas(row);
        g_gdg.row_addr_of_row[row] = (uint16_t)(mode700 ? (c / 8) : c);
    };
    g_gdg.row_addr = g_gdg.row_addr_of_row[g_gdg.beam_row];
    g_gdg.char_row = gdg_rows_from_canvas(g_gdg.beam_row) & 0x07;
    gdg_row_counter_advance();
}

static inline void gdg_set_regDMD(uint8_t value, unsigned event_ticks)
{
    g_gdg.regDMD = value;
    int mode700 = (value & REGISTER_DMD_FLAG_MZ700) ? 1 : 0;
    ctc82530_on_regDMD_changed(mode700, event_ticks);
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    /* DMD switch (bit MZ700, SCRW640) zmenil VRAM mapping -> prepocti
     * fast-path tabulku (VRAM stranky 8/A/C/D mohou prejit na RAM a zpet). */
    mz800_ram_fastpath_rebuild();
#endif
}

/**
 * @brief Odstup konce M-cyklu zápisu DMD od místa, které rozhoduje o paletě
 *        po návratu z režimu MZ-700 (sloupce = pxCLK).
 *
 * Na stroji (mz800-knowledge hw-measurement/vysledky.md, oddíl 17, U3)
 * nechá zápis DMD 00h barvy buňky (P - 333) div 16, když sestupná hrana
 * /IORQ P leží v 333 až 973 px od sestupné hrany HSYNC (hranice mezi
 * 332,5 / 333,5 a 972,5 / 973,5 px, okno přesně 640 px). U OUT (C),A leží
 * konec M-cyklu emulátoru E (sloupec) v P - 170,5 px, okno je tedy
 * E = 163 až 802 a buňka (E - 163) div 16, tj. sloupec E - 9 v canvasu.
 */
#define GDG_PALRET_LEAD_TICKS 9u

/**
 * @brief Nastaví paletové registry tak, jak je zanechá režim MZ-700 při návratu do režimu MZ-800.
 *
 * V režimu MZ-700 jdou barvy přes paletové registry: liché položky (1, 3)
 * nesou barvu popředí a sudé (0, 2) barvu pozadí, obojí jako barva režimu
 * MZ-700 (nenulová s intenzitou; bity 3 a 7 bajtu barvy neovlivňují).
 * Při zápisu DMD bez bitu 3 v registrech zůstane, co v nich právě bylo.
 *
 * Ve vodorovném okně canvasu je zdrojem atribut právě zobrazované buňky.
 * Změřeno na MZ-800 (mz800-knowledge hw-measurement/vysledky.md, oddíly 17
 * a 19): okno a buňka viz GDG_PALRET_LEAD_TICKS. Atribut se bere podle
 * čítače adresy řádku (st_GDG.row_addr) i na řádcích mimo canvas, kde čítač
 * běží dál: pod canvasem textový řádek 25 (buňky 24 až 39 přetékají na
 * offset 0 až 15), pak další přes konec snímku až po začátek canvasu.
 *
 * Mimo okno je zdrojem bajt CG-RAM znaku 0 (rovina I, offset r, adresa
 * 0C000h + r v režimu MZ-700), kde r je řádek vzoru posledního řádku
 * rastru, jehož okno už proběhlo: před oknem řádek vzoru předchozího řádku
 * rastru, za oknem řádek vzoru tohoto řádku. Bajt se dekóduje jako atribut
 * (bity 6 až 4 popředí, 2 až 0 pozadí). Pravidlo je z C modelu netlistu GDG
 * (gdg-mz800 c-whid-0; emu-experiments sapo-regress/model) a sedí se všemi
 * měřeními: v měřeních 5 až 9 (oddíly 16 a 17) ležela v CG-RAM výplň 0Fh
 * a stroj dal 0, 0, F, F (položky 3, 1, 2, 0); v měření 14 (oddíl 19) nuly
 * a stroj dal černou, jen po řádku rastru se vzorem 01h na 0C000h barvu 9
 * v sudých položkách. Model čte buňku DRAM se sloupcovým bitem 7 = 1, kterou
 * CPU nezapíše; na stroji je to táž buňka jako 0C000h + r (paměť sloupcové
 * bity 0 a 7 nerozlišuje, viz čtvrté čtení v DMD 0Ch). Kód znaku 0 mimo okno
 * je doložený modelem; měření ho nevyvrací, ale samo ho neurčí
 * (v měření 14 byly kódy všech znaků 00h) [neověřeno na stroji s jinými kódy].
 *
 * @pre Emu vlákno; g_gdg.regDMD má ještě bit MZ-700; čas GDG je konec
 *      M-cyklu zápisu DMD.
 * @post g_gdg.regPAL0 až regPAL3 nastavené; PALGRP se nemění.
 */
static void gdg_palette_after_mz700(void)
{
    unsigned row;
    unsigned col = gdg_get_beam_pos(g_gdg.total_elapsed.ticks, &row);
    uint8_t src;

    if ((row == g_gdg.beam_row) && (col >= GDG_PALRET_LEAD_TICKS + VIDEO_BEAM_CANVAS_FIRST_COLUMN)
        && (col - GDG_PALRET_LEAD_TICKS <= VIDEO_BEAM_CANVAS_LAST_COLUMN))
    {
        /* v okně: atribut právě zobrazované buňky */
        unsigned x = (col - GDG_PALRET_LEAD_TICKS - VIDEO_BEAM_CANVAS_FIRST_COLUMN) / 16;
        unsigned offset = (g_gdg.row_addr * 40u + x) & 0x03ff;
        src = g_memoryVRAM[0x1800 + offset];
    }
    else
    {
        /* mimo okno: vzor znaku 0 z CG-RAM, řádek vzoru posledního řádku
         * rastru, jehož okno už proběhlo. Když M-cyklus končí až v dalším,
         * ještě nezpracovaném řádku, je tím řádkem beam_row. */
        unsigned r = g_gdg.char_row;
        if ((row == g_gdg.beam_row) && (col < GDG_PALRET_LEAD_TICKS + VIDEO_BEAM_CANVAS_FIRST_COLUMN))
            r = (r + 7) & 0x07;
        src = g_memoryVRAM[r];
    };
    unsigned f = (src >> 4) & 0x07;
    unsigned b = src & 0x07;
    /* barva režimu MZ-700: nenulová s intenzitou */
    unsigned fg = f ? (f | 0x08) : 0;
    unsigned bg = b ? (b | 0x08) : 0;
    g_gdg.regPAL1 = fg;
    g_gdg.regPAL3 = fg;
    g_gdg.regPAL0 = bg;
    g_gdg.regPAL2 = bg;
}

/**
 * @brief Sloupec hrany registru DMD v řádku (mod 16): 334 + 16k px po sestupné
 *        hraně HSYNC, sloupec 0 = 186 px.
 *
 * C model netlistu GDG (gdg-mz800, mz800-knowledge hw-measurement/vysledky.md
 * oddíl 17, "Srovnání s C modelem netlistu") taktuje registr DMD v 334 + 16k px.
 */
#define GDG_DMD_REG_EDGE_COLUMN 148

/**
 * @brief O kolik sloupců před koncem M-cyklu zápisu DMD smí ležet hrana registru, která zápis ještě převezme.
 *
 * Na stroji platí zápis pro hranu v 334 + 16k px, jestliže sestupná hrana
 * /IORQ P přišla nejpozději 1,5 px před ní (oddíly 17 a 18). U OUT (C),A
 * leží konec M-cyklu E (sloupec) v P - 170,5 px, hrana R (sloupec) tedy
 * převezme zápis pro E <= R + 14.
 */
#define GDG_DMD_REG_WR_LEAD 14

/**
 * @brief Odstup prvního bodu nového režimu od hrany registru DMD při přepnutí
 *        z a do režimu MZ-700 (sloupce).
 *
 * Změřeno na MZ-800 (U7, oddíl 17; emu-experiments gdg-meas VYSLEDKY8.md):
 * 320 -> 700, 640 -> 700 a 700 -> 320 od 340 + 16k px, tedy 22 px po hraně
 * registru; 700 -> 640 od 346 + 16k px, 12 px po hraně (C model 22,5, 20,5
 * a 12,0 CLK0; rozdíl 1,5 px je posun proudu pixelů modelu v režimech 320
 * a MZ-700).
 */
#define GDG_DMD700_SHOW_TICKS 22
#define GDG_DMD700_TO640_SHOW_TICKS 12

/**
 * @brief Bod canvasu aktuálního řádku, od kterého se ukáže přepnutí z nebo do režimu MZ-700.
 *
 * @param show Odstup od hrany registru (GDG_DMD700_SHOW_TICKS nebo
 *        GDG_DMD700_TO640_SHOW_TICKS).
 * @return Bod canvasu (může být záporný nebo >= VIDEO_CANVAS_WIDTH);
 *         VIDEO_CANVAS_WIDTH, když M-cyklus končí až v dalším, ještě
 *         nezpracovaném řádku (aktuální řádek celý ve starém režimu).
 * @pre Emu vlákno; čas GDG je konec M-cyklu zápisu DMD.
 */
static int gdg_dmd700_switch_x(int show)
{
    unsigned row;
    unsigned col = gdg_get_beam_pos(g_gdg.total_elapsed.ticks, &row);
    if (row != g_gdg.beam_row)
        return VIDEO_CANVAS_WIDTH;
    int d = (int)col - GDG_DMD_REG_WR_LEAD - GDG_DMD_REG_EDGE_COLUMN;
    int k = (d >= 0) ? ((d + 15) / 16) : -((-d) / 16);
    int edge = GDG_DMD_REG_EDGE_COLUMN + 16 * k;
    return edge + show - (int)VIDEO_BEAM_CANVAS_FIRST_COLUMN;
}

/**
 * @brief Zápis DMD s vedlejšími efekty GDG (společné jádro OUT CEh
 *        a gdg_debug_set_regDMD()).
 *
 * Maskuje na bity 0-3; beze změny hodnoty nic nedělá. Jinak podle
 * PŮVODNÍHO režimu aktualizuje framebuffer (z 800 módu dokreslí řádek
 * do místa změny, ze 700 módu jen označí změnu obrazu)
 * a zavolá gdg_set_regDMD() s časem gdg_get_insigeop_ticks().
 *
 * Mezi režimy MZ-800 se obraz přepíná až na hranici 16 sloupců canvasu
 * (GDG_DMD_WR_LEAD_TICKS, měřeno na MZ-800 pro 320 <-> 640). Přepnutí z a do
 * režimu MZ-700 se na obraze ukáže až od bodu podle hrany registru DMD
 * (gdg_dmd700_switch_x(), framebuffer_mode700_switch()); registr sám
 * (mapování paměti, GATE0) platí hned. Při návratu z režimu MZ-700 se
 * přepíšou paletové registry (gdg_palette_after_mz700()).
 *
 * Bez záznamu hwlog a bez HW event breakpointu - ty řeší volající
 * gdg_write_byte().
 *
 * @param value Zapisovaná hodnota (bity 4-7 se ignorují).
 *
 * @pre Emu vlákno (nebo neběžící emu vlákno).
 */
static void gdg_write_regDMD(uint8_t value)
{
    value = value & 0x0f;

    if (g_gdg.regDMD == value)
        return;

    gdg_geometry_dmd_written(value);
    /* Jiná geometrie (320 / 640): sloupce 152, 153, 792, 793 a 927 patří
     * jednou canvasu, jednou borderu; border kreslit znovu (jinak by v nich
     * zůstal obsah z dřívějších snímků). */
    if (GDG_DMD_IS_GEOM640(value) != GDG_DMD_IS_GEOM640(g_gdg.regDMD))
        g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;

    if (!GDG_MZ800_DMD_TEST_MZ700)
    {
        /*
         * TODO: pri zmenach rezimu 700 / 800 a naopak je potreba osetrit framebuffer.
         * MZ700 -> MZ800 - do mista zmeny ponechat MZ700 obsah, zbytek updatovat standardne v MZ800
         * MZ800 -> MZ700 - udelat update jen do zmeny rezimu, zbytek vygenerovat v 700
         *
         */
        if (value & REGISTER_DMD_FLAG_MZ700)
        {
            int x = gdg_dmd700_switch_x(GDG_DMD700_SHOW_TICKS);
            framebuffer_mode700_switch(x, 1, 0);
            gdg_set_regDMD(value, gdg_get_insigeop_ticks());
            framebuffer_mode700_switch(x, 1, 1);
            return;
        }
        else
        {
            unsigned old_dmd = g_gdg.regDMD;
            framebuffer_MZ800_reg_write_begin(GDG_DMD_WR_LEAD_TICKS, 1);
            gdg_set_regDMD(value, gdg_get_insigeop_ticks());
            framebuffer_MZ800_reg_write_end();
            framebuffer_MZ800_resolution_switch(old_dmd, value, GDG_DMD_WR_LEAD_TICKS);
            return;
        };
    }
    else
    {
        g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
        if (!(value & REGISTER_DMD_FLAG_MZ700))
        {
            /* Návrat z režimu MZ-700: v paletě zůstanou barvy z tohoto režimu. */
            gdg_palette_after_mz700();
            framebuffer_mode700_switch(gdg_dmd700_switch_x((value & REGISTER_DMD_FLAG_SCRW640)
                                                               ? GDG_DMD700_TO640_SHOW_TICKS
                                                               : GDG_DMD700_SHOW_TICKS),
                                       0, 0);
        };
    };
    gdg_set_regDMD(value, gdg_get_insigeop_ticks());

    /*
                DEBUGGER_MMAP_FULL_UPDATE ( );
     */
}

void gdg_debug_set_regDMD(uint8_t value)
{
    gdg_write_regDMD(value);
}

/**
 * @brief Reset GDG (zapnutí nebo tlačítko RESET).
 *
 * DMD = 0, tedy režim MZ-800 320x200@4 jako na HW (netlist GDG po resetu
 * DMD nuluje; Service Manual MZ-800 str. 6: po zapnutí i po RESET je stroj
 * v režimu MZ-800, režim MZ-700 zapíná až monitor zápisem 08h na port CEh).
 * GATE0 CTC0 je proto do zápisu DMD z ROM v 1 (režim 800).
 * Dál vynuluje okraj, latch GATE0 režimu 700, CKSW, PALGRP a paletu (všechny
 * čtyři položky IGRB 0, černá, jako na HW: simulace netlistu GDG po /PORT
 * i po /MNRT), resetuje VRAM řadič a HW scroll, zruší
 * rozpracovaný zápis modelu WAIT u VRAM a označí obraz k překreslení.
 *
 * @post Volající má hned potom zavolat memory_reset(), mapa VRAM závisí
 *       na DMD.
 * @pre Emu vlákno (nebo neběžící emu vlákno).
 */
void gdg_reset(void)
{
    g_gdg.regct53g7 = 0; // musi byt pri resetu nastaveno drive, nez regDMD!
    gdg_set_regDMD(0x00, 0); // TODO: proc tam posilam 0 a ne g_gdg.total_elapsed.ticks ?

    if (g_gdg.regBOR != 0)
    {
        g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;
    };
    g_gdg.regBOR = 0;

    g_gdg.regPALGRP = 0;
    g_gdg.regPAL0 = 0;
    g_gdg.regPAL1 = 0;
    g_gdg.regPAL2 = 0;
    g_gdg.regPAL3 = 0;

    /* Klopný obvod CKSW má asynchronní reset. Délka rozběhnutého řádku
     * (row_shrink) je po načtení bitu už daná a reset ji nemění. */
    if (g_gdg.cksw)
    {
        g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;
    };
    g_gdg.cksw = 0;

    vramctrl_reset();
    hwscroll_reset();

    /* Model WAIT u VRAM - žádný rozpracovaný zápis. */
    g_gdg.vram_wrff_release = 0;

    g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
}

/**
 * @brief O kolik tiků před koncem M-cyklu čtení vzorkuje procesor Status registr.
 *
 * Změřeno na MZ-800 instrukcí IN A,(0CEh) (emu-experiments gdg-meas, první
 * měření, 2920 čtení): bit HBLN je 1 pro náběžnou hranu /IORQ 337 až 976 px,
 * přičemž hrany signálu leží v 336 a 976 px. Procesor tedy bere data asi
 * 1 px před náběžnou hranou /IORQ, 3,5 px před koncem M-cyklu; v mřížce
 * emulátoru 4 sloupce.
 */
#define GDG_STATUS_SAMPLE_LEAD_TICKS 4

/**
 * @brief O kolik sloupců dřív než události emulátoru leží hrany stavového HSYNC.
 *
 * Stavový HSYNC (bit 5) má na MZ-800 hrany 184 a 1112 px od sestupné hrany
 * /HSYN (měření i netlist), tedy sloupce -2 a 926. Události emulátoru, které
 * mění g_gdg.sts_hsync, jsou ve sloupcích 0 (konec řádku) a 928 (konec
 * viditelné části), aby se ušetřily dvě události na řádek. Rozdíl se
 * dorovnává jen při čtení Status registru.
 */
#define GDG_STATUS_HSYNC_LEAD_TICKS 2

/**
 * @brief Stavové bity Status registru tak, jak je vidí procesor při čtení.
 *
 * Čas GDG je konec M-cyklu čtení; stav se bere GDG_STATUS_SAMPLE_LEAD_TICKS
 * dřív. Protože události, které mezitím proběhly, už g_gdg.hbln, vbln,
 * sts_hsync a sts_vsync změnily, počítá se HBLN a HSYNC z polohy paprsku
 * a VBLN a VSYNC se v řádku své hrany vrací na hodnotu před hranou.
 *
 * @param hbln [out] Bit 7 (1 = viditelná část řádku).
 * @param vbln [out] Bit 6.
 * @param hs   [out] Bit 5 (stavový HSYNC).
 * @param vs   [out] Bit 4.
 * @pre Emu vlákno; g_gdg.total_elapsed.ticks je konec M-cyklu čtení a události
 *      do této chvíle jsou zpracované (mzarch_main_insideop_*()).
 */
static void gdg_status_sample(unsigned *hbln, unsigned *vbln, unsigned *hs, unsigned *vs)
{
    unsigned row;
    int col = (int) gdg_get_beam_pos(g_gdg.total_elapsed.ticks, &row);
    int same_row = (row == g_gdg.beam_row);
    int sampled = col - GDG_STATUS_SAMPLE_LEAD_TICKS;

    *vbln = SIGNAL_GDG_VBLNK ? 1 : 0;
    *vs = SIGNAL_GDG_STS_VS ? 1 : 0;

    /* Konec stavového HSYNC v tomto řádku (zkrácený řádek ho má dřív; délka
     * je známá od načtení bitu CKSW ve sloupci VIDEO_BEAM_HBLN_FIRST_COLUMN). */
    int hs_end = (int) (VIDEO_BEAM_DISPLAY_LAST_COLUMN + 1u) - GDG_STATUS_HSYNC_LEAD_TICKS
                 - (same_row ? (int) g_gdg.row_shrink : 0);

    if (sampled < 0)
    {
        /* Vzorek padl na konec předchozího řádku: zatemnění. */
        *hbln = 0;
        *hs = (sampled >= -GDG_STATUS_HSYNC_LEAD_TICKS) ? 1 : 0;
        return;
    };

    *hbln = ((sampled >= (int) (VIDEO_BEAM_CANVAS_FIRST_COLUMN - 4u)) && (sampled < (int) VIDEO_BEAM_HBLN_FIRST_COLUMN)) ? 1 : 0;
    *hs = (sampled < hs_end) ? 1 : 0;

    if (!same_row)
        return;

    /* VBLN se mění s HBLN_START na posledním řádku canvasu a na řádku před
     * prvním; VSYNC ve sloupci konce canvasu - 2 na řádku 0 a na posledním
     * viditelném řádku. Vzorek před hranou vidí hodnotu před ní. */
    if (((row == VIDEO_BEAM_CANVAS_LAST_ROW) || (row == VIDEO_BEAM_CANVAS_FIRST_ROW - 1))
        && (col >= (int) VIDEO_BEAM_HBLN_FIRST_COLUMN) && (sampled < (int) VIDEO_BEAM_HBLN_FIRST_COLUMN))
    {
        *vbln = !*vbln;
    };
    if (((row == 0) || (row == VIDEO_DISPLAY_HEIGHT - 1))
        && (col >= (int) (VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2)) && (sampled < (int) (VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH - 2)))
    {
        *vs = !*vs;
    };
}

uint8_t gdg_read_dmd_status_ioop(void)
{

    uint8_t retval;
    unsigned hbln, vbln, hs, vs;

    gdg_status_sample(&hbln, &vbln, &hs, &vs);
    retval = hbln ? 1 << 7 : 0x00;
    retval |= vbln ? 1 << 6 : 0x00;
    retval |= hs ? 1 << 5 : 0x00;
    retval |= vs ? 1 << 4 : 0x00;
    retval |= g_gdg.cksw ? 1 << 2 : 0x00;
    /* Bit 1 = poloha SW1: 0 = MZ-700 mód, 1 = MZ-800 mód (viz en_MZ800_MODE_SW). */
    retval |= (g_mzarch_main.mode_sw == MZ800_MODE_SW_MZ800) ? 1 << 1 : 0x00;
    retval |= SIGNAL_GDG_TEMPO;
    //    printf ( "read DMD sts = 0x%02x - HB: %d, VB: %d, HS: %d, VS: %d, row: %d, col: %d, PC: 0x%04x\n", retval, SIGNAL_GDG_HBLNK, SIGNAL_GDG_VBLNK, SIGNAL_GDG_STS_HS, SIGNAL_GDG_STS_VS, BEAM_ROW ( g_gdg.screen_ticks_elapsed ), BEAM_COL ( g_gdg.screen_ticks_elapsed ), z80ex_get_reg ( g_mz800_main.cpu, regPC )  );
    return retval;
}

uint8_t gdg_read_dmd_status_memop(void)
{
    uint8_t retval = 0x00;

    /* Čtení 0E008h v režimu MZ-700 nese jen HBLN (bit 7) a TEMPO (bit 0).
     * Bit CKSW tu na rozdíl od portu 0CEh není ani s nastaveným bitem:
     * změřeno na MZ-800 (emu-experiments gdg-meas, třetí měření: 81h a 80h).
     * Okamžik vzorkování je stejný jako u portu 0CEh (gdg_status_sample());
     * u čtení paměti změřený není, plyne z časování sběrnice Z80: data se
     * berou na téže hraně T3 jako u čtení portu [neověřeno]. */
    unsigned hbln, vbln, hs, vs;
    gdg_status_sample(&hbln, &vbln, &hs, &vs);
    retval |= hbln ? 1 << 7 : 0x00;
    retval |= SIGNAL_GDG_TEMPO;

    //    printf ( "read DMD sts = 0x%02x - HB: %d, VB: %d, HS: %d, VS: %d, row: %d, col: %d, PC: 0x%04x\n", retval, SIGNAL_GDG_HBLNK, SIGNAL_GDG_VBLNK, SIGNAL_GDG_STS_HS, SIGNAL_GDG_STS_VS, BEAM_ROW ( g_gdg.screen_ticks_elapsed ), BEAM_COL ( g_gdg.screen_ticks_elapsed ), z80ex_get_reg ( g_mz800_main.cpu, regPC )  );
    return retval;
}

uint8_t gdg_peek_dmd_status_memop(void)
{
    /* Okamžitý stav signálů (bez předstihu vzorkování procesorem). */
    uint8_t retval = 0x00;
    retval |= SIGNAL_GDG_HBLNK ? 1 << 7 : 0x00;
    retval |= SIGNAL_GDG_TEMPO;
    return retval;
}

/**
 * @brief Doplní účinek zápisu bitu CKSW, který skončil těsně po jeho načtení.
 *
 * Čip načítá bit jednou za řádek (MZEVENT_GDG_HBLN_START nastaví
 * g_gdg.row_shrink). Klopný obvod bitu se ale překlápí
 * GDG_CKSW_WR_EDGE_TICKS před koncem M-cyklu zápisu, kdežto emulátor zápis
 * provádí s časem konce M-cyklu a s už zpracovanými událostmi do té chvíle.
 * Skončil-li M-cyklus méně než GDG_CKSW_WR_EDGE_TICKS po načtení bitu, čip
 * už načetl novou hodnotu: funkce přepíše zkrácení tohoto řádku a čas čekající
 * události GDG, pokud leží za sloupcem GDG_CKSW_SHRINK_FIRST_COLUMN.
 *
 * @pre g_gdg.cksw už má novou hodnotu; g_gdg.total_elapsed.ticks je čas
 *      konce M-cyklu zápisu a události GDG do něj jsou zpracované. Emu
 *      vlákno, nebo pauza.
 * @post g_gdg.row_shrink odpovídá bitu, pokud zápis spadl do okna; čekající
 *       událost GDG i g_mzarch_main.event mají čas podle nového zkrácení.
 *       Mimo okno beze změny.
 */
static void gdg_cksw_written(void)
{
    unsigned row;
    unsigned col = gdg_get_beam_pos(g_gdg.total_elapsed.ticks, &row);
    unsigned shrink = g_gdg.cksw * GDG_CKSW_ROW_SHRINK;

    if ((row != g_gdg.beam_row) || (col < VIDEO_BEAM_HBLN_FIRST_COLUMN) ||
        (col >= VIDEO_BEAM_HBLN_FIRST_COLUMN + GDG_CKSW_WR_EDGE_TICKS) || (shrink == g_gdg.row_shrink))
    {
        return;
    };

    g_gdg.row_shrink = shrink;

    unsigned column = g_gdgevent[g_gdg.event.event_name].event_column;
    if (column > GDG_CKSW_SHRINK_FIRST_COLUMN)
    {
        g_gdg.event.ticks = gdg_get_event_ticks(column);
        mzarch_main_requeue_event();
    };
}

void gdg_write_byte(unsigned addr, uint8_t value)
{
    // printf("%s():%d - addr = 0x%04x, value = 0x%02x, PC = 0x%04x\n", __func__, __LINE__, addr, value, g_mz800_main.instruction_addr);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* trace-suite hwlog: zaznamenat write do GDG (klasifikace podle low byte adresy).
     * Detail per port v docs/cz/debugger/formats/HW-log_format.md. */
    if ( TEST_TRACE_HWLOG_DISPATCH ) {
        unsigned low = addr & 0xff;
        uint8_t payload[ 6 ] = { (uint8_t)( addr & 0xff ),
                                 (uint8_t)( ( addr >> 8 ) & 0xff ),
                                 value, 0, 0, 0 };
        if ( low == 0xce ) {
            hwlog_record ( HWLOG_CHIP_GDG_MODE, 0, payload );
        } else if ( low == 0xcf ) {
            unsigned msb = ( addr >> 8 ) & 0xff;
            if ( msb < 6 ) {
                hwlog_record ( HWLOG_CHIP_GDG_HWSCROLL, (uint8_t) msb, payload );
            } else if ( msb == 6 ) {
                hwlog_record ( HWLOG_CHIP_GDG_COLORS, HWLOG_GDG_COLORS_BORDER, payload );
            } else if ( msb == 7 ) {
                hwlog_record ( HWLOG_CHIP_GDG_VIDEO, HWLOG_GDG_VIDEO_CKSW, payload );
            }
        } else if ( low == 0xf0 ) {
            uint8_t sub = ( value & 0x40 ) ? HWLOG_GDG_COLORS_PALGRP : HWLOG_GDG_COLORS_PAL;
            hwlog_record ( HWLOG_CHIP_GDG_COLORS, sub, payload );
        } else if ( low == 0xcc || low == 0xcd ) {
            hwlog_record ( HWLOG_CHIP_GDG_WFRF, (uint8_t)( low & 0x01 ), payload );
        }
    }
    /* HWE - HW event BP hooks (mode_change / palette_change /
     * palgrp_change / border_change). Rozdeleno dle write target portu:
     *   0xCE              -> mode_change (DMD)
     *   0xCF, msb=6       -> border_change (border barva)
     *   0xCF, msb=7       -> cksw_change (bit CKSW)
     *   0xCF, msb<6       -> HW scroll (= zadny BP event)
     *   0xF0 & 0x40       -> palgrp_change (palette group selector)
     *   0xF0 & ~0x40      -> palette_change (konkretni paleta) */
    {
        unsigned low = addr & 0xff;
        if ( low == 0xce ) {
            if ( g_bp_event_active[ BP_EVENT_GDG_MODE_CHANGE ] ) {
                bp_event_fire ( BP_EVENT_GDG_MODE_CHANGE, (int32_t) value );
            }
        } else if ( low == 0xcf ) {
            unsigned msb = ( addr >> 8 ) & 0xff;
            if ( msb == 6 ) {
                if ( g_bp_event_active[ BP_EVENT_GDG_BORDER_CHANGE ] ) {
                    bp_event_fire ( BP_EVENT_GDG_BORDER_CHANGE, (int32_t) value );
                }
            } else if ( msb == 7 ) {
                /* cksw_change: Value = zapsaný bit CKSW (0 nebo 1), při
                 * každém zápisu na port, i když se hodnota nemění. */
                if ( g_bp_event_active[ BP_EVENT_GDG_CKSW_CHANGE ] ) {
                    bp_event_fire ( BP_EVENT_GDG_CKSW_CHANGE, (int32_t) ( ( value >> 7 ) & 1 ) );
                }
            }
        } else if ( low == 0xf0 ) {
            if ( value & 0x40 ) {
                if ( g_bp_event_active[ BP_EVENT_GDG_PALGRP_CHANGE ] ) {
                    bp_event_fire ( BP_EVENT_GDG_PALGRP_CHANGE, (int32_t) value );
                }
            } else {
                if ( g_bp_event_active[ BP_EVENT_GDG_PALETTE_CHANGE ] ) {
                    bp_event_fire ( BP_EVENT_GDG_PALETTE_CHANGE, (int32_t) value );
                }
            }
        }
    }
#endif

    /* vramm controller: 0xcc, 0xcd */
    if (0xcc == (addr & 0xfe))
    {
        vramctrl_mz800_set_wf_rf_reg(addr & 0x01, value);
        return;
    };

    unsigned addr_msb = (addr >> 8) & 0xff;

    switch (addr & 0xff)
    {

    case 0x08:
        /* zapis na status registr 0xe008 v rezimu MZ-700 */
        value = value & 0x01;
        if (value != g_gdg.regct53g7)
        {
            g_gdg.regct53g7 = value;
            ctc8253_gate(0, value, gdg_get_insigeop_ticks());
        };
        break;

        /* regDMD */
    case 0xce:
        gdg_write_regDMD(value);
        break;

    case 0xcf:

        /* HW scroll: 0x01cf - 0x05cf */
        if ((addr_msb != 0) && (addr_msb < 6))
        {

            hwscroll_set_reg(addr_msb, value);

            /* BORDER: 0x06cf */
        }
        else if (addr_msb == 6)
        {
            value = value & 0x0f;

            if (g_gdg.regBOR != value)
            {
                // printf ( "BORDER: 0x%02x, screen: %d, ticks: %d\n", value, g_gdg.total_elapsed.screens, mz800_main_get_instruction_start_ticks ( ) );
                framebuffer_border_write ( value );
                g_gdg.regBOR = value;
            };

            /* CKSW (Superimpose): 0x07cf */
        }
        else if (addr_msb == 7)
        {
            /* Bit 7 hodnoty = stav signálu CKSW (1 = řádek kratší
             * o GDG_CKSW_ROW_SHRINK pxCLK). Status registr (0CEh / 0E008h
             * bit 2) vidí novou hodnotu hned. Délku řádku čip mění jen při
             * načtení bitu v HBLN_START (mz800_gdg_event.c): zápis před ním
             * platí už pro tento řádek, pozdější až pro další. Zdroj:
             * mz800-knowledge hw/11-palette-attributes.md, "Jak rychle se
             * změna CKSW projeví" (simulace netlistu). */
            unsigned cksw = (value >> 7) & 1;
            if (cksw != g_gdg.cksw)
            {
                g_gdg.cksw = cksw;
                gdg_cksw_written();
                /* Krátký řádek má jinou viditelnou šířku - border překreslit. */
                g_framebuffer.border_changes = SCRSTS_THIS_IS_CHANGED;
            };
        };
        break;

    case 0xf0:
    {
        /* Zápis palety nebo skupiny palet. Na obraze se projeví
         * GDG_PALETTE_WR_LEAD_TICKS před koncem M-cyklu (měřeno na MZ-800);
         * řádek se proto před první změnou registru dokreslí po toto místo
         * a po změnách se případně překreslí jeho už nakreslený zbytek. */
        int pal_touched = 0;
#define GDG_PALETTE_WRITE_BEGIN() \
    do { \
        if ( !pal_touched && !GDG_MZ800_DMD_TEST_MZ700 ) { \
            framebuffer_MZ800_reg_write_begin ( GDG_PALETTE_WR_LEAD_TICKS, 0 ); \
            pal_touched = 1; \
        } \
    } while ( 0 )

        /* nastaveni PALGRP */
        if (value & 0x40)
        {

            if (g_gdg.regPALGRP != (value & 0x03))
            {
                GDG_PALETTE_WRITE_BEGIN();
                g_gdg.regPALGRP = value & 0x03;
            };

            /* nastaveni PAL */
        }
        else
        {

            unsigned pal_value = value & 0x0f;

            switch ((value & 0x30) >> 4)
            {

            case 0:
                if ((REGISTER_DMD_FLAG_SCRW640 | REGISTER_DMD_FLAG_HICOLOR | REGISTER_DMD_FLAG_VBANK) == (g_gdg.regDMD & (REGISTER_DMD_FLAG_SCRW640 | REGISTER_DMD_FLAG_HICOLOR | REGISTER_DMD_FLAG_VBANK)))
                {
                    /* undoc mode! */
                    if (g_gdg.regPAL1 != pal_value)
                    {
                        GDG_PALETTE_WRITE_BEGIN();
                        g_gdg.regPAL1 = pal_value;
                    };
                };
                if (g_gdg.regPAL0 != pal_value)
                {
                    GDG_PALETTE_WRITE_BEGIN();
                    g_gdg.regPAL0 = pal_value;
                };
                break;

            case 1:
                if (g_gdg.regPAL1 != pal_value)
                {
                    GDG_PALETTE_WRITE_BEGIN();
                    g_gdg.regPAL1 = pal_value;
                };
                break;

            case 2:
                if (g_gdg.regPAL2 != pal_value)
                {
                    GDG_PALETTE_WRITE_BEGIN();
                    g_gdg.regPAL2 = pal_value;
                };
                break;

            case 3:
                if ((REGISTER_DMD_FLAG_SCRW640 | REGISTER_DMD_FLAG_HICOLOR | REGISTER_DMD_FLAG_VBANK) == (g_gdg.regDMD & (REGISTER_DMD_FLAG_SCRW640 | REGISTER_DMD_FLAG_HICOLOR | REGISTER_DMD_FLAG_VBANK)))
                {
                    if (g_gdg.regPAL2 != pal_value)
                    {
                        GDG_PALETTE_WRITE_BEGIN();
                        g_gdg.regPAL2 = pal_value;
                    };
                };
                if (g_gdg.regPAL3 != pal_value)
                {
                    GDG_PALETTE_WRITE_BEGIN();
                    g_gdg.regPAL3 = pal_value;
                };
                break;
            };
        };
        if ( pal_touched ) framebuffer_MZ800_reg_write_end ( );
#undef GDG_PALETTE_WRITE_BEGIN
    }
        break;
    };
}
