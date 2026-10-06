/* 
 * File:   cmt_save.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 31. července 2018, 9:57
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


#ifndef CMT_SAVE_H
#define CMT_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cmtext.h"

    extern st_CMTEXT g_cmt_save_extension;


    typedef struct st_CMTSAVE_BLOCKSPEC {
        char *filepath;
        uint64_t last_event;
        uint64_t quantization_start;
        uint64_t emitted_samples;
    } st_CMTSAVE_BLOCKSPEC;


    void cmtsave_block_close ( st_CMTEXT_BLOCK *block );

#define CMTSAVE_DEFAULT_SAMPLERATE  44100
//#define CMTSAVE_DEFAULT_SAMPLERATE  192000

#ifdef __cplusplus
}
#endif

#endif /* CMT_SAVE_H */
