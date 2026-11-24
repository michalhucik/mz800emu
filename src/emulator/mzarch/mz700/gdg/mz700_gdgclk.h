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

#if MZTVSYS == MZTVSYS_NTSC

#define GDGCLK_REAL_BASE 14318180
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC) /* 262 * 912 * 60 = 14 336 640 */
#define GDGCLK2CPU_DIVIDER 4                                     /* 3,579545 MHz */
/* Vstup CTC0 = krystal / 16 = 894,886 kHz. Na MZ-1500 změřeno (894 945 Hz, čtvrtina
 * hodin CPU; báze mz800-knowledge, hw-measurement/vysledky.md, oddíl 24). Pro japonský
 * MZ-700 to plyne nepřímo: ROM 1Z-009B má tabulku not shodnou s ROM MZ-1500; na stroji
 * samotném změřeno není. Dřívější dělič 13 (1,101398 MHz) neměl zdroj. */
#define GDGCLK_CTC0_DIVIDER 16                                   /* 894,886 kHz */

#elif MZTVSYS == MZTVSYS_PAL

#define GDGCLK_REAL_BASE 17734475                                /* Simulovana frq: 17 721 600 Hz */
#define GDGCLK_BASE (VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC)
#define GDGCLK2CPU_DIVIDER 5                                     /* 3,546895 MHz */
#define GDGCLK_CTC0_DIVIDER 16                                   /* 1,108404 MHz */


#else
#error "MZTVSYS not defined or unknown for MZ-700 (expected MZTVSYS_PAL or MZTVSYS_NTSC)"
#endif

#endif /* MZ700_GDGCLK_H */
