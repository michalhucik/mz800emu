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
 * Takty jsou dvojí. Emulovaný stroj má celý počet taktů na snímek
 * (VIDEO_SCREEN_TICKS = 1136 x 312) a pro všechno, co patří k determinismu
 * stroje, platí jmenovitý takt GDGCLK_BASE = takty snímku krát 50. Skutečný
 * čas se z taktů počítá podle krystalu (GDGCLK_REAL_BASE): snímek trvá
 * 354 432 / 17 734 475 s, tedy 50,036326 snímku za sekundu, a CPU, CTC0
 * i zvuk mají kmitočet skutečného počítače.
 *
 * Podle krystalu: zvukový výstup a tempo
 * emulace (živý zvuk, termíny bez zvukového zařízení), zachycení zvuku
 * (reference.wav, sdl.wav, časy v CSV) a výpisy v Hz a sekundách
 * převádějí takty na skutečný čas přes GDGCLK_REAL_TICKS_PER_SEC (gdgclk.h).
 * Zatím podle jmenovitého taktu GDGCLK_BASE: kazeta (převod času pásky na
 * takty) a video záznam AVI (celočíselná snímková frekvence, v emulačním
 * čase pevný počet vzorků na snímek; záznam podle reality bere zvuk ze
 * zvukového výstupu, tedy podle krystalu, obraz vzorkuje 50 / 60 ticky za
 * sekundu).
 *
 * | Signál        | Skutečný počítač | Jmenovitý takt (GDGCLK_BASE) |
 * |---------------|------------------|------------------------------|
 * | takt GDG      | 17 734 475 Hz    | 17 721 600 Hz                |
 * | CPU (/5)      | 3 546 895 Hz     | 3 544 320 Hz                 |
 * | CTC0 (/16)    | 1 108 405 Hz     | 1 107 600 Hz                 |
 * | snímky        | 50,036 Hz        | 50 Hz                        |
 */

/**
 * @brief Frekvence krystalu skutečného MZ-800 v Hz.
 *
 * Podle ní se převádějí takty na skutečný čas (tempo emulace, zvukový
 * výstup, zachycení zvuku, výpisy); v kódu přes GDGCLK_REAL_TICKS_PER_SEC
 * (gdgclk.h).
 */
#define GDGCLK_REAL_BASE 17734475

/**
 * @brief Jmenovitý takt GDG v Hz: emulované takty za jmenovitou sekundu, 1136 * 312 * 50 = 17 721 600.
 *
 * Vázaný na celý počet snímků za sekundu, ne na krystal; patří
 * k determinismu stroje (časové konstanty v taktech, kazeta, replay,
 * video záznam). Pro převod na skutečný čas se nepoužívá (viz popis souboru).
 */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)

/** @brief Počet taktů GDG na takt CPU. CPU: 3 546 895 Hz podle krystalu (jmenovitě 3 544 320 Hz). */
#define GDGCLK2CPU_DIVIDER 5
/** @brief Počet taktů GDG na takt vstupu CTC0. Vstup: 1 108 405 Hz podle krystalu (jmenovitě 1 107 600 Hz). */
#define GDGCLK_CTC0_DIVIDER 16

#ifdef __cplusplus
}
#endif

#endif /* MZ800_GDGCLK_H */
