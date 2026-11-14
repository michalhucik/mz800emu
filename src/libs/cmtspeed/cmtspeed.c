/**
 * @file   cmtspeed.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.0
 * @brief  Implementace datových polí knihovny cmtspeed — násobitele rychlostí a textové poměry.
 *
 * Obsahuje definice globálních polí g_cmtspeed_divisor[] a g_cmtspeed_ratio[],
 * která jsou indexována hodnotami enumu en_CMTSPEED.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: Přidána hodnota CMTSPEED_CUSTOM (vlastní délky pulzů, UniCMT) a cmtspeed_is_custom (2.1.0).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "cmtspeed.h"

/** @brief Násobitel rychlosti indexovaný hodnotami en_CMTSPEED.
 *
 * Hodnota na indexu odpovídajícím danému enumu udává poměr vůči základní
 * baudové rychlosti. Např. index CMTSPEED_2_1 obsahuje 2.0.
 * Pro CMTSPEED_CUSTOM obsahuje 0 (délky pulzů nese volající zvlášť).
 */
const double g_cmtspeed_divisor[] = {
                                     0,
                                     ( (double) 1 / 1 ),
                                     ( (double) 2 / 1 ),
                                     ( (double) 2 / 1 ), // cpm
                                     ( (double) 3 / 1 ),
                                     ( (double) 3 / 2 ),
                                     ( (double) 7 / 3 ),
                                     ( (double) 8 / 3 ),
                                     ( (double) 9 / 7 ),
                                     ( (double) 25 / 14 ),
                                     0, // custom - vlastní délky pulzů, nejde o poměr
};

/** @brief Textové popisy poměrů rychlosti indexované hodnotami en_CMTSPEED.
 *
 * Řetězce ve formátu "X:Y", např. "1:1", "7:3". Pro CMTSPEED_NONE obsahuje "?:?",
 * pro CMTSPEED_2_1_CPM obsahuje "2:1 (cp/m)", pro CMTSPEED_CUSTOM "custom".
 */
const char *g_cmtspeed_ratio[] = {
                                  "?:?",
                                  "1:1",
                                  "2:1",
                                  "2:1 (cp/m)",
                                  "3:1",
                                  "3:2",
                                  "7:3",
                                  "8:3",
                                  "9:7",
                                  "25:14",
                                  "custom",
};


const char* cmtspeed_version ( void ) {
    return CMTSPEED_VERSION;
}

