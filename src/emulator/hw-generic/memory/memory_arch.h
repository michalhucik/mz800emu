/**
 * @file memory_arch.h
 * @brief Per-arch dispatch paměťových hlaviček (mzhal 11c-2c).
 *
 * Vyčleněno z memory.h, aby sdílená hlavička byla bez per-arch
 * podmínek (compile-once). Includují ho jen per-EXE TU a sdílené TU
 * s vlastním per-arch větvením (dirty seznam kroku 11) - compile-once
 * TU knihovny mz_emucore ho includovat NESMÍ (poison to chytí).
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

#ifndef MEMORY_ARCH_H
#define MEMORY_ARCH_H

#include "hw-generic/memory/memory.h"

#if MZARCH == 800
#include "mzarch/mz800/memory/mz800_memory.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/memory/mz1500_memory.h"
#elif MZARCH == 700
#include "mzarch/mz700/memory/mz700_memory.h"
#else
#error "Unknown MZARCH value"
#endif

#endif /* MEMORY_ARCH_H */
