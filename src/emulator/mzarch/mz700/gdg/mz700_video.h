/* 
 * File:   mz700_video.h
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

#ifndef MZ700_VIDEO_H
#define MZ700_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hodnoty MZTVSYS_PAL/NTSC - nutné PŘED #if porovnáním níže (jinak past
 * nedefinovaného makra, viz mztvsys.h). */
#include "mzarch/mztvsys.h"

#if MZTVSYS == MZTVSYS_NTSC
#include "mz700_video_ntsc.h"
#elif MZTVSYS == MZTVSYS_PAL
#include "mz700_video_pal.h"
#else
#error "MZTVSYS not defined or unknown for MZ-700 (expected MZTVSYS_PAL or MZTVSYS_NTSC)"
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZ700_VIDEO_H */

