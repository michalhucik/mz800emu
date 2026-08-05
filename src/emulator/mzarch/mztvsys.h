/**
 * @file mztvsys.h
 * @brief Hodnoty TV systémů (PAL/NTSC) - jediný zdroj pravdy.
 *
 * Dříve tyto hodnoty definoval build systém (-DMZTVSYS_PAL=50
 * -DMZTVSYS_NTSC=60 per target); přesun do mztvsys_values.h zaručuje, že každé
 * místo porovnávající MZTVSYS vidí skutečné hodnoty bez ohledu na sadu
 * -D definic daného targetu.
 *
 * Samotné MZTVSYS zůstává compile-time parametr targetu
 * (-DMZTVSYS=MZTVSYS_PAL nebo MZTVSYS_NTSC z AddMzEmu.cmake/AddMzTest).
 *
 * Past, kterou tato hlavička zavírá: test `#if MZTVSYS == MZTVSYS_PAL`
 * s NEdefinovanými makry se v preprocesoru tiše vyhodnotí jako 0 == 0 =
 * pravda (PAL větev), bez jediného varování. Proto tuto hlavičku MUSÍ
 * includovat každý soubor, který s MZTVSYS_PAL/MZTVSYS_NTSC porovnává,
 * a #error guard níže chytá TU bez -DMZTVSYS.
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

#ifndef MZTVSYS_H
#define MZTVSYS_H

#include "mzarch/mztvsys_values.h"

#ifndef MZTVSYS
#error "MZTVSYS not defined (expected -DMZTVSYS=MZTVSYS_PAL or MZTVSYS_NTSC)"
#endif

#endif /* MZTVSYS_H */
