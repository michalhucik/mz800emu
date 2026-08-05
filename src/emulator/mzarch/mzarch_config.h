/*
 * File:   mzarch_config.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 16. srpna 2016, 9:50
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

#ifndef MZARCH_CONFIG_H
#define MZARCH_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

#if MZARCH == 800
#include "mz800/mz800_config.h"
#else
#if MZARCH == 1500
#include "mz1500/mz1500_config.h"
#else
#if MZARCH == 700
#include "mz700/mz700_config.h"
#else
#error "Unknown MZARCH value"
#endif
#endif
#endif

/*
 * Capability makra (CFG_HWEXT_HAVE_*, HAVE_PIOZ80, HAVE_PSG)
 * ====================================================================
 *
 * Definuje VYHRADNE per-arch mz*_config.h (vzdy vsechna, hodnotou 0/1;
 * HAVE_PSG = max pocet PSG instanci 0/2). Fallback #ifndef bloky byly
 * odstraneny v mzhal kroku 8e - kazde TU vidi hodnoty pres dispatcher
 * vyse a chybejici definice chyta -Werror=undef.
 *
 * Sdileny (compile-once kandidatni) kod tato makra uz NEKONZUMUJE - cte
 * runtime pole g_mzhal (psg_count, have_pioz80, have_fdc, ...). Zbyvajici
 * compile-time konzumenti jsou per-EXE whitelist (mzarch.c, mz*_iorq.c,
 * interrupt.c, bootstrap.c) a dokumentovane odklady (io_catalog.c,
 * dbgapi media slot case bloky, iface_keyboard* FDC/JOY, joy subsystem)
 * - viz mzhal/KROK8-PLAN.md.
 */


    /*
     * Platformně nezávislé toggly (DEBUGGER, MCP, CLK1M1, AUDIO,
     * MEMORY_MAKE_STATISTICS) jsou vyčleněné do mzcommon_config.h
     * (mzhal krok 5) - hlavičky citlivé na layout je includují přímo.
     */
#include "mzcommon_config.h"

#ifdef __cplusplus
}
#endif

#endif /* MZARCH_CONFIG_H */
