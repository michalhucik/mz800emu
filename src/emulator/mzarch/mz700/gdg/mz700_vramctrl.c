/*
 * File:   mz700_vramctrl.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 18:49
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

/*
 * MZ-700 VRAM kontroler
 *
 * MZ-700 nema MZ-800 graficke rezimy (4-rovinny VRAM, WF/RF registry,
 * HW scroll). VRAM je jednoducha 4 KB pamet na adresach 0xD000-0xDFFF,
 * pristupna primo pres MEMOP.
 *
 * MZ-700 kompatibilni rezim pouziva synchronizaci pristupu k VRAM
 * s generovanim obrazu (HBLN timing).
 */

#include <stdint.h>

#include "mz700_vramctrl.h"

st_VRAMCTRL g_vramctrl;

void vramctrl_reset(void)
{
    g_vramctrl.mz700_wr_latch_is_used = 0;
    g_vramctrl.wait_ticks = 0;
}
