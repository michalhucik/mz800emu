/*
 * File:   mz1500_gdgclk.h
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

#ifndef MZ1500_GDGCLK_H
#define MZ1500_GDGCLK_H

#include "hw-generic/gdg/video.h"

/**
 * @file mz1500_gdgclk.h
 * @brief Hodinové konstanty MZ-1500 (NTSC): takt GDG a děliče pro CPU a CTC0.
 *
 * Emulátor nepočítá čas z frekvence krystalu, ale z celých snímků:
 * emulovaný takt je počet taktů snímku krát 60 snímků za sekundu. Je to
 * záměrné zjednodušení: zvuk, obraz, nahrávání i převod WAV kazety pak
 * pracují s celým počtem vzorků a taktů na snímek. Důsledek: všechno
 * odvozené z taktu (CPU, CTC0, zvuk) běží o 0,129 % rychleji než
 * skutečný počítač.
 *
 * | Signál        | Skutečný počítač | Emulátor      |
 * |---------------|------------------|---------------|
 * | takt GDG      | 14 318 180 Hz    | 14 336 640 Hz |
 * | CPU (/4)      | 3 579 545 Hz     | 3 584 160 Hz  |
 * | CTC0 (/16)    | 894 886 Hz       | 896 040 Hz    |
 * | snímky        | 59,923 Hz        | 60 Hz         |
 *
 * Sloupec "Skutečný počítač" je dopočet z krystalu 14 318 180 Hz.
 */

/**
 * @brief Frekvence krystalu skutečného MZ-1500 v Hz.
 *
 * Emulace podle ní neběží; slouží jen měření a výpisům (poměr emulovaného
 * taktu ke skutečnému).
 */
#define GDGCLK_REAL_BASE 14318180

/**
 * @brief Emulovaný takt GDG v Hz: 912 * 262 * 60 = 14 336 640.
 *
 * Záměrně vázaný na celý počet snímků za sekundu, ne na krystal
 * (viz popis souboru).
 */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)

/** @brief Počet taktů GDG na takt CPU. Emulovaný CPU: 3 584 160 Hz (skutečný 3 579 545 Hz). */
#define GDGCLK2CPU_DIVIDER 4

/**
 * @brief Počet taktů GDG na takt vstupu CTC0. Emulovaný vstup: 896 040 Hz (skutečný 894 886 Hz).
 *
 * Změřeno na MZ-1500: vstup CTC0 894 945 Hz je čtvrtina hodin CPU, tedy
 * krystal / 16 (báze mz800-knowledge, hw-measurement/vysledky.md, oddíl 24).
 * Sedí s tabulkou not v ROM (nota A = 03F8h = 1016, pípnutí asi 881 Hz).
 */
#define GDGCLK_CTC0_DIVIDER 16

#endif /* MZ1500_GDGCLK_H */
