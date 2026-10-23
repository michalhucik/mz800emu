/*
 * File:   mz800_main.h
 * Author: chaky
 *
 * Created on 14. června 2015, 16:16
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

#ifndef MZ800_MAIN_H
#define MZ800_MAIN_H

#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"

#define MZ800_TEST_MZ800_HWCOMPAT_ALLOW_PSG1 (g_mzarch_main.mz800_hwcompat_allow_psg1 == MZ800_HWCOMPAT_ALLOW_PSG1_YES)

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    void mzarch_run_to_temporary_breakpoint(void);
#endif

    /**
     * @brief Povolí nebo zakáže druhý PSG (HW experiment Allow PSG1, stereo).
     *
     * Při povolení inicializuje PSG1 a přepne zvukový modul na stereo.
     *
     * @param value 1 = povolit, 0 = zakázat (vyšší bity se ignorují).
     * @pre Emu vlákno (menu zásah zařadí přes mzarch_request_allow_psg1());
     *      emu vlákno do PSG1 zapisuje.
     * @post g_mzarch_main.mz800_hwcompat_allow_psg1 odpovídá @p value.
     */
    void mz800_mz800_hwcompat_allow_psg1(unsigned value);

#ifdef __cplusplus
}
#endif

#endif /* MZ800_MAIN_H */
