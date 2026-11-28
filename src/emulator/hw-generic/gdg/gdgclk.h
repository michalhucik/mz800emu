/*
 * File:   gdgclk.h
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

#ifndef GDGCLK_H
#define GDGCLK_H

/**
 * @file gdgclk.h
 * @brief Hodinové konstanty platformy: výběr hlavičky podle MZARCH a převod taktů na skutečný čas.
 *
 * Emulátor má pro takt GDG dvě veličiny (obě v Hz, definované v hlavičce
 * platformy):
 * - GDGCLK_BASE: emulované takty za jmenovitou sekundu, tj. takty snímku
 *   krát celý počet snímků (50 nebo 60). Patří k determinismu stroje
 *   (časové konstanty v taktech, autotype, kazeta, replay, video záznam);
 *   nemění se.
 * - GDGCLK_REAL_BASE: kmitočet krystalu skutečného počítače. Převod taktů
 *   na skutečný čas (zvukový výstup, tempo emulace, zachycení zvuku, výpisy
 *   v Hz a sekundách) jde přes GDGCLK_REAL_TICKS_PER_SEC.
 *
 * Počet taktů snímku (VIDEO_SCREEN_TICKS) je v obou případech stejný; liší
 * se jen délka snímku ve skutečném čase.
 */

#include <stdint.h>

#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdgclk.h"
#else
#if MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdgclk.h"
#else
#if MZARCH == 700
#include "mzarch/mz700/gdg/mz700_gdgclk.h"
#else
#error GDGCLK_H: Unsupported MZARCH
#endif /* MZARCH==700 */
#endif /* MZARCH==1500*/
#endif /* MZARCH==800 */

/**
 * @brief Skutečné takty GDG za sekundu (kmitočet krystalu) pro převod taktů na skutečný čas.
 *
 * Jediné místo, přes které se emulované takty převádějí na skutečný čas
 * (sekundy, Hz, vzorky zvuku): `čas [s] = takty / GDGCLK_REAL_TICKS_PER_SEC`.
 * Hodnota je GDGCLK_REAL_BASE jako `uint64_t`, aby součin s počtem taktů
 * nebo vzorků nepřetekl v typu int.
 *
 * Patří jen do výstupu a výpisů (zvukový výstup a jeho tempo, zachycení
 * zvuku, výpisy). Všechno, co je součástí determinismu stroje (časové
 * konstanty v taktech, kazeta, replay, video záznam), počítá dál
 * s GDGCLK_BASE.
 */
#define GDGCLK_REAL_TICKS_PER_SEC ((uint64_t)GDGCLK_REAL_BASE)

/**
 * @brief Skutečná snímková frekvence v Hz (krystal / takty snímku), typ double.
 *
 * PAL 17 734 475 / 354 432 = 50,036326 Hz, NTSC 14 318 180 / 238 944 =
 * 59,922743 Hz. Jen pro výpisy a metadata; celočíselné výpočty pracují se
 * zlomkem GDGCLK_REAL_TICKS_PER_SEC / VIDEO_SCREEN_TICKS.
 */
#define GDGCLK_REAL_FPS ((double)GDGCLK_REAL_TICKS_PER_SEC / (double)VIDEO_SCREEN_TICKS)

#endif /* GDGCLK_H */
