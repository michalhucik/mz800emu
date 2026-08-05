/*
 * File:   mz1500_framebuffer.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 5. července 2015, 8:43
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

/**
 * @file mz1500_framebuffer.h
 * @brief Per-arch vstup framebufferu - layout a API jsou sdílené
 * (framebuffer_state.h), zde jen kontrola, že display plocha
 * architektury vejde do superset bufferu.
 */

#ifndef MZ1500_FRAMEBUFFER_H
#define MZ1500_FRAMEBUFFER_H

#include "hw-generic/gdg/framebuffer_state.h"
#include "hw-generic/gdg/video.h"

/* Superset buffer musí pojmout display plochu této architektury. */
#ifdef __cplusplus
static_assert((VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT) <= GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX,
              "display plocha presahuje GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX");
#else
_Static_assert((VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT) <= GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX,
               "display plocha presahuje GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX");
#endif

#endif /* MZ1500_FRAMEBUFFER_H */
