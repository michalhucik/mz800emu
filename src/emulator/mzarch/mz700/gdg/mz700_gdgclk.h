/*
 * File:   mz700_gdgclk.h
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

#ifndef MZ700_GDGCLK_H
#define MZ700_GDGCLK_H

#include "hw-generic/gdg/video.h"

/**
 * @file mz700_gdgclk.h
 * @brief Hodinové konstanty MZ-700 (PAL i NTSC): takt GDG a děliče pro CPU a CTC0.
 *
 * Takty jsou dvojí. Emulovaný stroj má celý počet taktů na snímek
 * (VIDEO_SCREEN_TICKS) a pro všechno, co patří k determinismu stroje,
 * platí jmenovitý takt GDGCLK_BASE = takty snímku krát 50 (PAL) nebo 60
 * (NTSC). Skutečný čas se z taktů počítá podle krystalu (GDGCLK_REAL_BASE):
 * PAL 50,036326, NTSC 59,922743 snímku za sekundu; CPU, CTC0 i zvuk mají
 * kmitočet skutečného počítače.
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
 * | Signál         | Skutečný počítač | Jmenovitý takt |
 * |----------------|------------------|----------------|
 * | PAL takt GDG   | 17 734 475 Hz    | 17 721 600 Hz |
 * | PAL CPU (/5)   | 3 546 895 Hz     | 3 544 320 Hz  |
 * | PAL CTC0 (/16) | 1 108 405 Hz     | 1 107 600 Hz  |
 * | PAL snímky     | 50,036 Hz        | 50 Hz         |
 * | NTSC takt GDG  | 14 318 180 Hz    | 14 336 640 Hz |
 * | NTSC CPU (/4)  | 3 579 545 Hz     | 3 584 160 Hz  |
 * | NTSC CTC0 (/16)| 894 886 Hz       | 896 040 Hz    |
 * | NTSC snímky    | 59,923 Hz        | 60 Hz         |
 *
 * Sloupec "Skutečný počítač" je dopočet z krystalu.
 *
 * Význam maker je v obou větvích stejný:
 * - GDGCLK_REAL_BASE: frekvence krystalu skutečného počítače v Hz; podle ní
 *   se převádějí takty na skutečný čas (GDGCLK_REAL_TICKS_PER_SEC v gdgclk.h).
 * - GDGCLK_BASE: jmenovitý takt GDG v Hz (emulované takty za jmenovitou
 *   sekundu), vázaný na celý počet snímků za sekundu; patří k determinismu
 *   stroje.
 * - GDGCLK2CPU_DIVIDER: počet taktů GDG na takt CPU.
 * - GDGCLK_CTC0_DIVIDER: počet taktů GDG na takt vstupu CTC0.
 */

#if MZTVSYS == MZTVSYS_NTSC

/** @brief Frekvence krystalu skutečného MZ-700 NTSC v Hz (převod taktů na skutečný čas). */
#define GDGCLK_REAL_BASE 14318180
/** @brief Jmenovitý takt GDG v Hz: 912 * 262 * 60 = 14 336 640 (determinismus stroje, ne skutečný čas). */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)
/** @brief Počet taktů GDG na takt CPU. CPU: 3 579 545 Hz podle krystalu (jmenovitě 3 584 160 Hz). */
#define GDGCLK2CPU_DIVIDER 4
/**
 * @brief Počet taktů GDG na takt vstupu CTC0. Vstup: 894 886 Hz podle krystalu (jmenovitě 896 040 Hz).
 *
 * Na MZ-1500 změřeno (894 945 Hz, čtvrtina hodin CPU; báze mz800-knowledge,
 * hw-measurement/vysledky.md, oddíl 24). Pro japonský MZ-700 to plyne nepřímo:
 * ROM 1Z-009B má tabulku not shodnou s ROM MZ-1500; na stroji samotném změřeno
 * není. Dřívější dělič 13 (1,101398 MHz) neměl zdroj.
 */
#define GDGCLK_CTC0_DIVIDER 16

#elif MZTVSYS == MZTVSYS_PAL

/** @brief Frekvence krystalu skutečného MZ-700 PAL v Hz (převod taktů na skutečný čas). */
#define GDGCLK_REAL_BASE 17734475
/** @brief Jmenovitý takt GDG v Hz: 1136 * 312 * 50 = 17 721 600 (determinismus stroje, ne skutečný čas). */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)
/** @brief Počet taktů GDG na takt CPU. CPU: 3 546 895 Hz podle krystalu (jmenovitě 3 544 320 Hz). */
#define GDGCLK2CPU_DIVIDER 5
/** @brief Počet taktů GDG na takt vstupu CTC0. Vstup: 1 108 405 Hz podle krystalu (jmenovitě 1 107 600 Hz). */
#define GDGCLK_CTC0_DIVIDER 16


#else
#error "MZTVSYS not defined or unknown for MZ-700 (expected MZTVSYS_PAL or MZTVSYS_NTSC)"
#endif

#endif /* MZ700_GDGCLK_H */
