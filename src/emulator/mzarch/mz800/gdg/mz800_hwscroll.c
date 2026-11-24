/*
 * File:   mz800_hwscroll.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 19:44
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
 *		reg. SSA (0x04cf):
 *			- pocatecni adresa HW scrollu, hodnoty 0x00 - 0x78, increment po 5
 *
 *		reg. SEA (0x05cf):
 *			- koncova adresa HW scrollu, hodnoty 0x05 - 0x7d, increment po 5
 *
 *		reg. SW (0x03cf):
 *			- sirka HW scrollu, hodnoty 0x05 - 0x7d, increment po 5
 *
 *		reg. SOF1 (0x01cf) - 8 bitu:
 *		reg. SOF2 (0x02cf) - 2 bity:
 *			- 10 bitovy registr, kterym je urcen offset scrollu, hodnoty 0x00 - 0x3e8, increment po 5
 *			Pri hodnote 0x00 se neprovadi zadny scroll.
 *
 *
 *		Scroll je dostupny pouze v rezimech MZ800.
 *		Scroll je aktivni pokud jsou splneny vsechny naslednujici podminky:
 *
 *		SOF > 0x00
 *              SSA <= 0x78 ( << 6 = 0x1e00 )
 *		SEA >= 0x05 ( << 6 = 0x0140 )
 *              SW > SOF
 *		SEA > SSA
 *              (SW = SEA - SSA podle měření na MZ-800 scroll nevypíná, viz hwscroll_regs_changed())
 *
 *      Update - tohle se zrejme nekontroluje:
 *
 *              SOF <= 0x3e8 ( << 3 = 0x1f40 )
 *              SEA <= 0x7d ( << 6 = 0x1f40 )
 *
 */

#include <stdio.h>
#include <stdint.h>

#define DBGHWSCROLL 0

#include "mz800_hwscroll.h"
#include "mz800_gdg.h"
#include "mz800_framebuffer.h"

// nedelej framebuffer
//#define framebuffer_MZ800_screen_changed()

st_HWSCROLL g_hwscroll;

void hwscroll_init(void)
{
    g_hwscroll.enabled = 0;
}

void hwscroll_reset(void)
{
    g_hwscroll.regSSA = 0;
    g_hwscroll.regSEA = 125 << 6;
    g_hwscroll.regSW = 125 << 6;
    g_hwscroll.regSOF = 0;
    g_hwscroll.enabled = 0;
}

/**
 * @brief Dokreslí uplynulou část aktuálního obrazového řádku se stávajícím
 *        mapováním scrollu.
 *
 * Framebuffer se doplňuje líně až k aktuálnímu sloupci paprsku. Před změnou
 * kteréhokoliv registru scrollu se proto musí dokreslit to, co paprsek už
 * prošel, ještě se STARÝM mapováním adres - jinak by se nové mapování
 * uplatnilo i zpětně na začátek řádku. Stejný postup používá zápis palety
 * a DMD v mz800_gdg.c.
 *
 * V režimu MZ-700 scroll neplatí a nic se nedokresluje.
 *
 * @post Pixely aktuálního řádku až k paprsku jsou ve framebufferu,
 *       g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED (jen režim MZ-800).
 */
static inline void hwscroll_flush_elapsed_pixels(void)
{
    if (!GDG_MZ800_DMD_TEST_MZ700)
    {
        /* Místo, odkud platí nový registr, viz GDG_SCROLL_WR_LEAD_TICKS. */
        framebuffer_MZ800_reg_write_begin(GDG_SCROLL_WR_LEAD_TICKS, 0);
    };
}

/**
 * @brief Přepočítá příznak g_hwscroll.enabled podle aktuálních registrů.
 *
 * Podmínky 1 až 5 (SOF > 0, SSA <= 1E00h, SEA >= 140h, SW > SOF, SEA > SSA)
 * jsou z původního modelu emulátoru, na stroji neověřené [neověřeno].
 * Rovnost SW = SEA - SSA scroll nezapíná ani nevypíná: na skutečném MZ-800
 * odpovídá obraz s hodnotami, které ji porušují, vzorci posunu adres, ve
 * kterém SW vystupuje samostatně (mz800-knowledge hw-measurement/vysledky.md
 * oddíl 17, U4; C model netlistu totéž, hw/10c-hw-scroll.md OPRAVA
 * 2026-11-22). Emulátor do opravy scroll při porušení vypínal.
 *
 * @pre Uplynulá část řádku je dokreslená (hwscroll_flush_elapsed_pixels()
 *      před změnou registru).
 */
static void hwscroll_regs_changed(void)
{

    /* jsou splneny vsechny podminky pro scroll? */
    if ((g_hwscroll.regSOF > 0) &&
        //( g_hwscroll.regSOF <= 0x1f40 ) &&
        (g_hwscroll.regSSA <= 0x1e00) &&
        (g_hwscroll.regSEA >= 0x0140) &&
        //( g_hwscroll.regSEA <= 0x1f40 ) &&
        (g_hwscroll.regSW > g_hwscroll.regSOF) &&
        (g_hwscroll.regSEA > g_hwscroll.regSSA))
    {

        g_hwscroll.enabled = 1;
#if DBGHWSCROLL
        printf("SOF: 0x%04x, SSA: 0x%04x, SEA: 0x%04x, SW: 0x%04x, HW scroll is ENABLED\n", g_hwscroll.regSOF, g_hwscroll.regSSA, g_hwscroll.regSEA, g_hwscroll.regSW);
#endif
    }
    else
    {
        g_hwscroll.enabled = 0;
#if DBGHWSCROLL
        printf("SOF: 0x%04x, SSA: 0x%04x, SEA: 0x%04x, SW: 0x%04x, HW scroll is DISABLED\n", g_hwscroll.regSOF, g_hwscroll.regSSA, g_hwscroll.regSEA, g_hwscroll.regSW);

        if (!(g_hwscroll.regSOF > 0))
        {
            printf("\t! regSOF > 0\n");
        };
        //        if ( ! ( g_hwscroll.regSOF <= 0x1f40 ) ) {
        //            printf ( "\t! regSOF <= 0x1f40\n" );
        //        };
        if (!(g_hwscroll.regSSA <= 0x1e00))
        {
            printf("\t! regSSA <= 0x1e00\n");
        };
        if (!(g_hwscroll.regSEA >= 0x0140))
        {
            printf("\t! regSEA >= 0x0140\n");
        };
        //        if ( ! ( g_hwscroll.regSEA <= 0x1f40 ) ) {
        //            printf ( "\t! regSEA <= 0x1f40\n" );
        //        };
        if (!(g_hwscroll.regSEA > g_hwscroll.regSSA))
        {
            printf("\t! regSEA > regSSA\n");
        };
        if (!(g_hwscroll.regSW > g_hwscroll.regSOF))
        {
            printf("\t! regSW > regSOF\n");
        };
#endif
    };
}

/**
 * @brief Zápis do registru scrollu z portu xxCFh (OUT CPU).
 *
 * @param addr  Horní bajt adresy portu: 01h SOF_LO, 02h SOF_HI, 03h SW,
 *              04h SSA, 05h SEA. Jiné hodnoty se ignorují (jen dokreslení).
 * @param value Zapsaná hodnota.
 *
 * @pre Volat z emu vlákna; g_gdg.total_elapsed.ticks odpovídá okamžiku zápisu.
 * @post Uplynulá část řádku je vykreslená se starým mapováním, registry
 *       a g_hwscroll.enabled mají nový stav.
 */
void hwscroll_set_reg(unsigned addr, uint8_t value)
{

    hwscroll_flush_elapsed_pixels();

    // printf ( "addr: 0x%02x, value: 0x%02x, PC: 0x%04x\n", addr, value, z80ex_get_reg ( g_mz800_main.cpu, regPC ) );

    switch (addr)
    {

        /* nastaveni SOF_LO: 0x01cf */
    case 0x01:
        g_hwscroll.regSOF &= 0x03 << 11;
        g_hwscroll.regSOF |= (value & 0xff) << 3;
        // printf ( "LO: %d (%d)\n", value, g_hwscroll.regSOF );
        break;

        /* nastaveni SOF_HI: 0x02cf */
    case 0x02:
        g_hwscroll.regSOF &= 0xff << 3;
        g_hwscroll.regSOF |= (value & 0x03) << 11;
        // printf ( "HI: %d (%d)\n", value, g_hwscroll.regSOF );
        break;

        /* nastaveni SW: 0x03cf */
    case 0x03:
        g_hwscroll.regSW = (value & 0x7f) << 6;
        break;

        /* nastaveni SSA: 0x04cf */
    case 0x04:
        g_hwscroll.regSSA = (value & 0x7f) << 6;
        break;

        /* nastaveni SEA: 0x05cf */
    case 0x05:
        g_hwscroll.regSEA = (value & 0x7f) << 6;
        break;
    };

    hwscroll_regs_changed();

    if (!GDG_MZ800_DMD_TEST_MZ700)
    {
        /* Překreslit už nakreslenou část řádku za místem změny. */
        framebuffer_MZ800_reg_write_end();
    };
}

/*
 * Ladicí přístup k registrům scrollu (debugger, MCP).
 *
 * Hodnoty jsou v jednotkách portu xxCFh, tedy stejné, jaké zapisuje program:
 * SSA, SEA a SW 0..7Fh, SOF 0..3FFh. Interně se ukládají jako bajtové adresy
 * plane (SSA, SEA, SW << 6, SOF << 3).
 */

/** @brief Vrátí SSA v jednotkách portu 04CFh (0..7Fh). */
unsigned hwscroll_get_ssa(void)
{
    return g_hwscroll.regSSA >> 6;
}

/** @brief Vrátí SEA v jednotkách portu 05CFh (0..7Fh). */
unsigned hwscroll_get_sea(void)
{
    return g_hwscroll.regSEA >> 6;
}

/** @brief Vrátí SW v jednotkách portu 03CFh (0..7Fh). */
unsigned hwscroll_get_sw(void)
{
    return g_hwscroll.regSW >> 6;
}

/** @brief Vrátí SOF v jednotkách portů 01CFh/02CFh (0..3FFh). */
unsigned hwscroll_get_sof(void)
{
    return g_hwscroll.regSOF >> 3;
}

/**
 * @brief Nastaví SSA jako zápis do portu 04CFh.
 *
 * @param value Hodnota v jednotkách portu (použije se bit 0-6).
 * @pre Volat z emu vlákna.
 * @post Uplynulá část řádku je vykreslená se starým mapováním.
 */
void hwscroll_set_ssa(unsigned value)
{
    hwscroll_flush_elapsed_pixels();
    g_hwscroll.regSSA = (value & 0x7f) << 6;
    hwscroll_regs_changed();
}

/**
 * @brief Nastaví SEA jako zápis do portu 05CFh.
 *
 * @param value Hodnota v jednotkách portu (použije se bit 0-6).
 * @pre Volat z emu vlákna.
 * @post Uplynulá část řádku je vykreslená se starým mapováním.
 */
void hwscroll_set_sea(unsigned value)
{
    hwscroll_flush_elapsed_pixels();
    g_hwscroll.regSEA = (value & 0x7f) << 6;
    hwscroll_regs_changed();
}

/**
 * @brief Nastaví SW jako zápis do portu 03CFh.
 *
 * @param value Hodnota v jednotkách portu (použije se bit 0-6).
 * @pre Volat z emu vlákna.
 * @post Uplynulá část řádku je vykreslená se starým mapováním.
 */
void hwscroll_set_sw(unsigned value)
{
    hwscroll_flush_elapsed_pixels();
    g_hwscroll.regSW = (value & 0x7f) << 6;
    hwscroll_regs_changed();
}

/**
 * @brief Nastaví celý 10bitový SOF jako zápis do portů 01CFh a 02CFh.
 *
 * @param value Hodnota v jednotkách portu (použije se bit 0-9).
 * @pre Volat z emu vlákna.
 * @post Uplynulá část řádku je vykreslená se starým mapováním.
 */
void hwscroll_set_sof(unsigned value)
{
    hwscroll_flush_elapsed_pixels();
    g_hwscroll.regSOF = (value & 0x3ff) << 3;
    hwscroll_regs_changed();
}
