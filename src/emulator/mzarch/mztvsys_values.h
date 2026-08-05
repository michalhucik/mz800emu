/**
 * @file mztvsys_values.h
 * @brief Hodnoty TV systémů (PAL/NTSC) - jediný zdroj pravdy.
 *
 * Samostatná hlavička bez zmínky o MZTVSYS: je tak includovatelná i v
 * compile-once TU knihovny mz_emucore, kde je identifikátor MZTVSYS
 * poisonovaný (emucore_poison.h) a runtime hodnota se čte z
 * g_mzhal.tvsys. Per-EXE TU používají mztvsys.h, která tyto hodnoty
 * přebírá a navíc vynucuje -DMZTVSYS.
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

#ifndef MZTVSYS_VALUES_H
#define MZTVSYS_VALUES_H

/** @brief PAL: 50 Hz obrazovek za sekundu. */
#define MZTVSYS_PAL 50

/** @brief NTSC: 60 Hz obrazovek za sekundu. */
#define MZTVSYS_NTSC 60

#endif /* MZTVSYS_VALUES_H */
