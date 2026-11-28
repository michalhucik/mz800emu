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
 * Takty jsou dvojí. Emulovaný stroj má celý počet taktů na snímek
 * (VIDEO_SCREEN_TICKS = 912 x 262) a pro všechno, co patří k determinismu
 * stroje, platí jmenovitý takt GDGCLK_BASE = takty snímku krát 60. Skutečný
 * čas se z taktů počítá podle krystalu (GDGCLK_REAL_BASE): snímek trvá
 * 238 944 / 14 318 180 s, tedy 59,922743 snímku za sekundu, a CPU, CTC0
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
 * | takt GDG      | 14 318 180 Hz    | 14 336 640 Hz                |
 * | CPU (/4)      | 3 579 545 Hz     | 3 584 160 Hz                 |
 * | CTC0 (/16)    | 894 886 Hz       | 896 040 Hz                   |
 * | snímky        | 59,923 Hz        | 60 Hz                        |
 *
 * Sloupec "Skutečný počítač" je dopočet z krystalu 14 318 180 Hz.
 */

/**
 * @brief Frekvence krystalu skutečného MZ-1500 v Hz.
 *
 * Podle ní se převádějí takty na skutečný čas (tempo emulace, zvukový
 * výstup, zachycení zvuku, výpisy); v kódu přes GDGCLK_REAL_TICKS_PER_SEC
 * (gdgclk.h).
 */
#define GDGCLK_REAL_BASE 14318180

/**
 * @brief Jmenovitý takt GDG v Hz: emulované takty za jmenovitou sekundu, 912 * 262 * 60 = 14 336 640.
 *
 * Vázaný na celý počet snímků za sekundu, ne na krystal; patří
 * k determinismu stroje (časové konstanty v taktech, kazeta, replay,
 * video záznam). Pro převod na skutečný čas se nepoužívá (viz popis souboru).
 */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)

/** @brief Počet taktů GDG na takt CPU. CPU: 3 579 545 Hz podle krystalu (jmenovitě 3 584 160 Hz). */
#define GDGCLK2CPU_DIVIDER 4

/**
 * @brief Počet taktů GDG na takt vstupu CTC0. Vstup: 894 886 Hz podle krystalu (jmenovitě 896 040 Hz).
 *
 * Změřeno na MZ-1500: vstup CTC0 894 945 Hz je čtvrtina hodin CPU, tedy
 * krystal / 16 (báze mz800-knowledge, hw-measurement/vysledky.md, oddíl 24).
 * Sedí s tabulkou not v ROM (nota A = 03F8h = 1016, pípnutí asi 881 Hz).
 */
#define GDGCLK_CTC0_DIVIDER 16

#endif /* MZ1500_GDGCLK_H */
