/*
 * File:   mz800_gdgclk.h
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

#ifndef MZ800_GDGCLK_H
#define MZ800_GDGCLK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "hw-generic/gdg/video.h"

/**
 * @file mz800_gdgclk.h
 * @brief Hodinové konstanty MZ-800 (PAL): takt GDG a děliče pro CPU a CTC0.
 *
 * Emulátor nepočítá čas z frekvence krystalu, ale z celých snímků:
 * emulovaný takt je počet taktů snímku krát 50 snímků za sekundu. Je to
 * záměrné zjednodušení: zvuk, obraz, nahrávání i převod WAV kazety pak
 * pracují s celým počtem vzorků a taktů na snímek. Důsledek: všechno
 * odvozené z taktu (CPU, CTC0, zvuk) běží o 0,073 % pomaleji než
 * skutečný počítač.
 *
 * | Signál        | Skutečný počítač | Emulátor      |
 * |---------------|------------------|---------------|
 * | takt GDG      | 17 734 475 Hz    | 17 721 600 Hz |
 * | CPU (/5)      | 3 546 895 Hz     | 3 544 320 Hz  |
 * | CTC0 (/16)    | 1 108 405 Hz     | 1 107 600 Hz  |
 * | snímky        | 50,036 Hz        | 50 Hz         |
 */

/**
 * @brief Frekvence krystalu skutečného MZ-800 v Hz.
 *
 * Emulace podle ní neběží; slouží jen měření a výpisům (poměr emulovaného
 * taktu ke skutečnému).
 */
#define GDGCLK_REAL_BASE 17734475

/**
 * @brief Emulovaný takt GDG v Hz: 1136 * 312 * 50 = 17 721 600.
 *
 * Záměrně vázaný na celý počet snímků za sekundu, ne na krystal
 * (viz popis souboru).
 */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)

/** @brief Počet taktů GDG na takt CPU. Emulovaný CPU: 3 544 320 Hz (skutečný 3 546 895 Hz). */
#define GDGCLK2CPU_DIVIDER 5
/** @brief Počet taktů GDG na takt vstupu CTC0. Emulovaný vstup: 1 107 600 Hz (skutečný 1 108 405 Hz). */
#define GDGCLK_CTC0_DIVIDER 16

#ifdef __cplusplus
}
#endif

#endif /* MZ800_GDGCLK_H */
