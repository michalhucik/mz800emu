/* 
 * File:   cmtext_container.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:20
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
 * @file cmtext_container.c
 * @brief Kontejner pásky Virtual CMT (implementace cmtext_container.h).
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "libs/generic_driver/generic_driver.h"

#include "baseui/baseui.h"

#include "cmtext_container.h"


void cmtext_container_tapeindex_destroy ( st_CMTEXT_TAPE_INDEX *index, int count_blocks ) {
    if ( !index ) return;
    int i;
    for ( i = 0; i < count_blocks; i++ ) {
        if ( index[i].bltype == CMTEXT_BLOCK_TYPE_MZF ) {
            if ( index[i].item.mzf.fname ) baseui_tools_mem_free ( index[i].item.mzf.fname );
        } else if ( index[i].bltype == CMTEXT_BLOCK_TYPE_TAPHEADER ) {
            if ( index[i].item.taphdr.fname ) baseui_tools_mem_free ( index[i].item.taphdr.fname );
        };
    };
    baseui_tools_mem_free ( index );
}


void cmtext_container_tape_destroy ( st_CMTEXT_CONTAINER_TAPE *tape, int count_blocks ) {
    if ( !tape ) return;
    if ( tape->h ) {
        generic_driver_close ( tape->h );
    };
    cmtext_container_tapeindex_destroy ( tape->index, count_blocks );
    baseui_tools_mem_free ( tape );
}


void cmtext_container_destroy ( st_CMTEXT_CONTAINER *container ) {
    if ( !container ) return;
    if ( container->name ) baseui_tools_mem_free ( container->name );
    if ( container->filepath ) baseui_tools_mem_free ( container->filepath );
    cmtext_container_tape_destroy ( container->tape, container->count_blocks );
    baseui_tools_mem_free ( container );
}


st_CMTEXT_TAPE_INDEX* cmtext_container_tape_index_aloc ( st_CMTEXT_TAPE_INDEX *index, int count_items ) {
    uint32_t size = ( count_items + 1 ) * sizeof ( st_CMTEXT_TAPE_INDEX );

    if ( !index ) {
        index = (st_CMTEXT_TAPE_INDEX*) baseui_tools_mem_alloc ( size );
    } else {
        index = (st_CMTEXT_TAPE_INDEX*) baseui_tools_mem_realloc ( index, size );
    };

    if ( !index ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d)\n", __func__, __LINE__, size );
        return NULL;
    };

    return index;
}


st_CMTEXT_CONTAINER_TAPE* cmtext_container_tape_new ( st_HANDLER *h, st_CMTEXT_TAPE_INDEX *index ) {
    st_CMTEXT_CONTAINER_TAPE *tape = (st_CMTEXT_CONTAINER_TAPE*) baseui_tools_mem_alloc0 ( sizeof ( st_CMTEXT_CONTAINER_TAPE ) );
    if ( !tape ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d).", __func__, __LINE__, (int) sizeof ( st_CMTEXT_CONTAINER_TAPE ) );
        return NULL;
    };
    tape->h = h;
    tape->index = index;
    return tape;
}


st_CMTEXT_CONTAINER* cmtext_container_new (
                                            en_CMTEXT_CONTAINER_TYPE type,
                                            char *filepath,
                                            int count_blocks,
                                            st_CMTEXT_CONTAINER_TAPE *tape,
                                            cmtext_container_cb_next_block cb_next_block,
                                            cmtext_container_cb_previous_block cb_previous_block,
                                            cmtext_container_cb_open_block cb_open_block
                                            ) {

    st_CMTEXT_CONTAINER *container = (st_CMTEXT_CONTAINER*) baseui_tools_mem_alloc0 ( sizeof ( st_CMTEXT_CONTAINER ) );
    if ( !container ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d).", __func__, __LINE__, (int) sizeof ( st_CMTEXT_CONTAINER ) );
        return NULL;
    };
    container->type = type;
    container->count_blocks = count_blocks;

    int filepath_size = sizeof ( char ) * ( strlen ( filepath ) + 1 );
    container->filepath = (char*) baseui_tools_mem_alloc0 ( filepath_size );
    if ( !container->filepath ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d).", __func__, __LINE__, filepath_size );
        cmtext_container_destroy ( container );
        return NULL;
    };
    strncpy ( container->filepath, filepath, filepath_size );

    container->name = baseui_tools_basename ( filepath );

    container->tape = tape;
    container->cb_next_block = cb_next_block;
    container->cb_previous_block = cb_previous_block;
    container->cb_open_block = cb_open_block;
    return container;
}


const char* cmtext_container_get_name ( st_CMTEXT_CONTAINER *container ) {
    assert ( container );
    assert ( container->name );
    return container->name;
}

const char* cmtext_container_get_filepath ( st_CMTEXT_CONTAINER *container ) {
    assert ( container );
    assert ( container->filepath );
    return container->filepath;
}


int cmtext_container_set_filepath ( st_CMTEXT_CONTAINER *container, const char *filepath ) {
    assert ( container );
    assert ( filepath );
    size_t size = strlen ( filepath ) + 1;
    char *path = ( char* ) baseui_tools_mem_alloc0 ( ( uint32_t ) size );
    char *name = baseui_tools_basename ( filepath );
    if ( !path || !name ) {
        if ( path ) baseui_tools_mem_free ( path );
        if ( name ) baseui_tools_mem_free ( name );
        return EXIT_FAILURE;
    };
    memcpy ( path, filepath, size );
    if ( container->filepath ) baseui_tools_mem_free ( container->filepath );
    if ( container->name ) baseui_tools_mem_free ( container->name );
    container->filepath = path;
    container->name = name;
    return EXIT_SUCCESS;
}


en_CMTEXT_CONTAINER_TYPE cmtext_container_get_type ( st_CMTEXT_CONTAINER *container ) {
    assert ( container );
    return container->type;
}


int cmtext_container_get_count_blocks ( st_CMTEXT_CONTAINER *container ) {
    assert ( container );
    return container->count_blocks;
}


en_CMTEXT_BLOCK_TYPE cmtext_container_get_block_type ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    return container->tape->index[block_id].bltype;
}


en_CMTEXT_BLOCK_SPEED cmtext_container_get_block_speed ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    return container->tape->index[block_id].blspeed;
}


void cmtext_container_set_block_speed ( st_CMTEXT_CONTAINER *container, int block_id, en_CMTEXT_BLOCK_SPEED blspeed ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    container->tape->index[block_id].blspeed = blspeed;
}


const char* cmtext_container_get_block_fname ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    char *fname;
    switch ( container->tape->index[block_id].bltype ) {
        case CMTEXT_BLOCK_TYPE_WAV:
            fname = "WAV";
            break;

        case CMTEXT_BLOCK_TYPE_MZF:
            fname = container->tape->index[block_id].item.mzf.fname;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            fname = container->tape->index[block_id].item.taphdr.fname;
            break;

        case CMTEXT_BLOCK_TYPE_TAPDATA:
            fname = "";
            break;

        case CMTEXT_BLOCK_TYPE_TZX:
        case CMTEXT_BLOCK_TYPE_CONTROL:
            /* statický název typu bloku (tmz_block_id_name()) */
            fname = (char*) container->tape->index[block_id].item.tzx.name;
            break;

        default:
            fname = "UNKNOWN BLOCK NAME";
    };
    return fname;
}


int cmtext_container_get_block_ftype ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    int ftype;
    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            ftype = container->tape->index[block_id].item.mzf.ftype;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            ftype = container->tape->index[block_id].item.taphdr.code;
            break;

        case CMTEXT_BLOCK_TYPE_TAPDATA:
        case CMTEXT_BLOCK_TYPE_WAV:
        default:
            ftype = -1;
    };
    return ftype;
}


int cmtext_container_get_block_fsize ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    int fsize;
    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            fsize = container->tape->index[block_id].item.mzf.fsize;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            fsize = container->tape->index[block_id].item.taphdr.size - 2; // odecist flag a checksum
            break;

        case CMTEXT_BLOCK_TYPE_TAPDATA:
            fsize = container->tape->index[block_id].item.tapdata.size - 2; // odecist flag a checksum
            break;

        case CMTEXT_BLOCK_TYPE_WAV:
        default:
            fsize = -1;
    };
    return fsize;
}


int cmtext_container_get_block_fstrt ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    int fstrt;
    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            fstrt = container->tape->index[block_id].item.mzf.fstrt;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
        case CMTEXT_BLOCK_TYPE_WAV:
        case CMTEXT_BLOCK_TYPE_TAPDATA:
        default:
            fstrt = -1;
    };
    return fstrt;
}


int cmtext_container_get_block_fexec ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    int fexec;
    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            fexec = container->tape->index[block_id].item.mzf.fexec;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
        case CMTEXT_BLOCK_TYPE_WAV:
        case CMTEXT_BLOCK_TYPE_TAPDATA:
        default:
            fexec = -1;
    };
    return fexec;
}


en_CMTSPEED cmtext_container_get_block_cmt_speed ( st_CMTEXT_CONTAINER *container, int block_id ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    en_CMTSPEED cmtspeed;
    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            cmtspeed = container->tape->index[block_id].item.mzf.cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            cmtspeed = container->tape->index[block_id].item.taphdr.cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_TAPDATA:
            cmtspeed = container->tape->index[block_id].item.tapdata.cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_WAV:
        default:
            cmtspeed = CMTSPEED_NONE;
    };
    return cmtspeed;
}


void cmtext_container_set_block_cmt_speed ( st_CMTEXT_CONTAINER *container, int block_id, en_CMTSPEED cmtspeed ) {
    assert ( container );
    assert ( block_id < container->count_blocks );

    switch ( container->tape->index[block_id].bltype ) {

        case CMTEXT_BLOCK_TYPE_MZF:
            container->tape->index[block_id].item.mzf.cmtspeed = cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_TAPHEADER:
            container->tape->index[block_id].item.taphdr.cmtspeed = cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_TAPDATA:
            container->tape->index[block_id].item.tapdata.cmtspeed = cmtspeed;
            break;

        case CMTEXT_BLOCK_TYPE_WAV:
        default:
            break;
    };
}


int cmtext_container_get_block_pulses ( st_CMTEXT_CONTAINER *container, int block_id, st_MZTAPE_PULSES_LENGTH *pulses ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    st_CMTEXT_TAPE_INDEX *idx = &container->tape->index[block_id];
    if ( idx->bltype != CMTEXT_BLOCK_TYPE_MZF ) return EXIT_FAILURE;
    if ( !cmtspeed_is_custom ( idx->item.mzf.cmtspeed ) ) return EXIT_FAILURE;
    *pulses = idx->item.mzf.pulses;
    return EXIT_SUCCESS;
}


int cmtext_container_set_block_pulses ( st_CMTEXT_CONTAINER *container, int block_id, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    assert ( container );
    assert ( block_id < container->count_blocks );
    st_CMTEXT_TAPE_INDEX *idx = &container->tape->index[block_id];
    if ( idx->bltype != CMTEXT_BLOCK_TYPE_MZF ) return EXIT_FAILURE;
    idx->item.mzf.pulses = *pulses;
    return EXIT_SUCCESS;
}
