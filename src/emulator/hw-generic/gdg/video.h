/* 
 * File:   video.h
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

#ifndef VIDEO_H
#define VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#if MZARCH == 800
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "mzarch/mz800/gdg/mz800_video.h"
#else
#if MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_video.h"
#else
#if MZARCH == 700
#include "mzarch/mz700/gdg/mz700_video.h"
#else
#error "Unknown MZARCH value"
#endif
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_H */

