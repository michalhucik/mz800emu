/*
 * File:   mz800_hwscroll.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 19:45
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

#ifndef MZ800_HWSCROLL_H
#define MZ800_HWSCROLL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>
#include <stdint.h>

    typedef struct st_HWSCROLL
    {
        unsigned regSSA;
        unsigned regSEA;
        unsigned regSW;
        unsigned regSOF;
        unsigned enabled;

    } st_HWSCROLL;

    extern st_HWSCROLL g_hwscroll;

    extern void hwscroll_init(void);
    extern void hwscroll_reset(void);
    extern void hwscroll_set_reg(unsigned addr, uint8_t value);

    extern unsigned hwscroll_get_ssa(void);
    extern unsigned hwscroll_get_sea(void);
    extern unsigned hwscroll_get_sw(void);
    extern unsigned hwscroll_get_sof(void);

    extern void hwscroll_set_ssa(unsigned value);
    extern void hwscroll_set_sea(unsigned value);
    extern void hwscroll_set_sw(unsigned value);
    extern void hwscroll_set_sof(unsigned value);

#define TEST_HWSCRL_ENABLED (g_hwscroll.enabled)
#define TEST_HWSCRL_ADDR_IN_SCRL_AREA(addr) ((addr >= g_hwscroll.regSSA) && (addr < g_hwscroll.regSEA))
#define TEST_HWSCRL_ADDR_IN_SCRL_AREA_UPDOWN(addr) (addr >= (g_hwscroll.regSEA - g_hwscroll.regSOF))
/**
 * @brief Maska posunuté adresy roviny VRAM (13 bitů, 8 KB).
 *
 * Při SW = SEA - SSA zůstane posunutá adresa v oblasti SSA až SEA - 1. Stroj
 * ale scroll zapne i při porušení této rovnosti (mz800-knowledge
 * hw/10c-hw-scroll.md, OPRAVA 2026-11-22) a adresa pak může vyjít mimo
 * rozsah; sčítačka adresy v GDG přeteče v 13 bitech (hypotéza, podle šířky
 * adresy roviny), emulátor tak nikdy nečte mimo rovinu.
 */
#define HWSCRL_ADDR_MASK 0x1fffu
#define HWSCRL_SHIFT_UP(addr) ((addr + g_hwscroll.regSOF) & HWSCRL_ADDR_MASK)
#define HWSCRL_SHIFT_DOWN(addr) ((addr + g_hwscroll.regSOF - g_hwscroll.regSW) & HWSCRL_ADDR_MASK)
#define HWSCRL_SHIFT_UPDOWN(addr) ((TEST_HWSCRL_ADDR_IN_SCRL_AREA_UPDOWN(addr)) ? HWSCRL_SHIFT_DOWN(addr) : HWSCRL_SHIFT_UP(addr))

    static inline unsigned hwscroll_shift_addr(unsigned addr)
    {

        if (TEST_HWSCRL_ENABLED)
        {

            /* nachazime se v oblasti, ktera ma byt scrollovana? */
            if (TEST_HWSCRL_ADDR_IN_SCRL_AREA(addr))
            {

                if (TEST_HWSCRL_ADDR_IN_SCRL_AREA_UPDOWN(addr))
                {
                    return HWSCRL_SHIFT_DOWN(addr);
                };
                return HWSCRL_SHIFT_UP(addr);
            };
        };

        return addr;
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ800_HWSCROLL_H */
