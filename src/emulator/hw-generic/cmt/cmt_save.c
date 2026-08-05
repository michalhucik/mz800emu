/* 
 * File:   cmt_save.c
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

#include <stdio.h>
#include <string.h>
#include <assert.h>


#include "baseui/baseui.h"
#include "mzarch/mzhal.h"
#include "fs_layer.h"
#include "cmt.h"
#include "cmtext.h"
#include "cmt_save.h"


char *g_cmt_save_fileext[] = {
                              NULL
};

st_CMTEXT_INFO g_cmt_save_info = {
                                  "SAVE-WAV",
                                  g_cmt_save_fileext,
                                  "Cmt extension for save SAVE-WAV",
                                  CMTEXT_TYPE_RECORDABLE
};

extern st_CMTEXT *g_cmt_save;


static void cmtsave_blockspec_destroy ( st_CMTSAVE_BLOCKSPEC *blspec ) {
    if ( !blspec ) return;
    if ( blspec->filepath ) baseui_tools_mem_free ( blspec->filepath );
    baseui_tools_mem_free ( blspec );
}


void cmtsave_block_close ( st_CMTEXT_BLOCK *block ) {
    if ( !block ) return;
    cmtsave_blockspec_destroy ( block->spec );
    block->spec = (st_CMTSAVE_BLOCKSPEC*) NULL;
    cmtext_block_destroy ( block );
}


static void cmtsave_container_close ( st_CMTEXT_CONTAINER *container ) {
    cmtext_container_destroy ( container );
}


static st_CMTSAVE_BLOCKSPEC* cmtsave_blockspec_new ( char *filename ) {
    st_CMTSAVE_BLOCKSPEC *blspec = (st_CMTSAVE_BLOCKSPEC*) baseui_tools_mem_alloc0 ( sizeof ( st_CMTSAVE_BLOCKSPEC ) );
    int len = strlen ( filename );
    blspec->filepath = (char*) baseui_tools_mem_alloc0 ( len + 1 );
    snprintf ( blspec->filepath, len + 1, "%s", filename );
    FILE *fh;
    FS_LAYER_FOPEN ( fh, filename, FS_LAYER_FMODE_W );
    if ( !fh ) {
        cmtsave_blockspec_destroy ( blspec );
        return NULL;
    };
    fclose ( fh );
    return blspec;
}


static void cmtsave_eject ( void ) {
    cmtsave_block_close ( g_cmt_save->block );
    g_cmt_save->block = (st_CMTEXT_BLOCK*) NULL;
    cmtsave_container_close ( g_cmt_save->container );
    g_cmt_save->container = (st_CMTEXT_CONTAINER*) NULL;
}


static st_CMT_STREAM* cmtsave_stream_new ( int value ) {

    st_CMT_STREAM *stream = cmt_stream_new ( CMT_STREAM_TYPE_VSTREAM );
    if ( !stream ) {
        return NULL;
    };

    //stream->str.vstream = cmt_vstream_new ( g_mzhal.gdgclk_base, CMT_VSTREAM_BYTELENGTH16, value, CMT_STREAM_POLARITY_NORMAL );
    stream->str.vstream = cmt_vstream_new ( CMTSAVE_DEFAULT_SAMPLERATE, CMT_VSTREAM_BYTELENGTH8, value, CMT_STREAM_POLARITY_NORMAL );
    if ( !stream->str.vstream ) {
        cmt_stream_destroy ( stream );
        return NULL;
    };

    printf ( "%s stream type: %s\n", cmtext_get_name ( g_cmt_save ), cmt_stream_get_stream_type_txt ( stream ) );
    printf ( "%s rate: %d Hz\n", cmtext_get_name ( g_cmt_save ), cmt_stream_get_rate ( stream ) );

    return stream;
}


static st_CMTEXT_BLOCK* cmtsave_block_open ( char *filename ) {

    printf ( "%s\nOpen: %s\n", cmtext_get_description ( g_cmt_save ), filename );

    st_CMT_STREAM *stream = NULL;

    st_CMTEXT_BLOCK *block = cmtext_block_new ( 0, CMTEXT_BLOCK_TYPE_WAV, stream, CMTEXT_BLOCK_SPEED_NONE, 0, NULL );
    if ( !block ) {
        //cmt_stream_destroy ( stream );
        return NULL;
    };

    st_CMTSAVE_BLOCKSPEC *blspec = cmtsave_blockspec_new ( filename );
    if ( !blspec ) {
        cmtext_block_destroy ( block );
        return NULL;
    };

    block->spec = blspec;

#if 0
    block->cb_play = cmtext_block_play;
    block->cb_get_playname = cmtext_block_get_playname;
    block->cb_set_polarity = cmtext_block_set_polarity;
    block->cb_set_speed = (cmtext_block_cb_set_speed) NULL;
    block->cb_get_bdspeed = (cmtext_block_cb_get_bdspeed) NULL;
#endif

    return block;
}


static int cmtsave_container_open ( char *filename ) {

    cmtsave_eject ( );

    st_CMTEXT_CONTAINER *container = cmtext_container_new ( CMTEXT_CONTAINER_TYPE_SINGLE, filename, 1, NULL, NULL, NULL, NULL );
    if ( !container ) {
        return EXIT_FAILURE;
    };

    st_CMTEXT_BLOCK *block = cmtsave_block_open ( filename );
    if ( !block ) {
        baseui_error ( "%s: Can't create block\n", cmtext_get_description ( g_cmt_save ) );
        cmtsave_container_close ( container );
        return EXIT_FAILURE;
    };

    g_cmt_save->container = container;
    g_cmt_save->block = block;

    return EXIT_SUCCESS;
}


static void cmtsave_write_data ( uint64_t play_ticks, int value ) {
    assert ( g_cmt_save->block );
    st_CMTEXT_BLOCK *block = g_cmt_save->block;
    st_CMTSAVE_BLOCKSPEC *blspec = block->spec;
    st_CMT_STREAM *stream = block->stream;
    if ( !stream ) {
        stream = cmtsave_stream_new ( ~value );
        if ( !stream ) return;
        block->stream = stream;
        blspec->last_event = ( play_ticks > g_mzhal.gdgclk_base ) ? ( play_ticks - g_mzhal.gdgclk_base ) : 0;
    };

    uint32_t count_ticks = play_ticks - blspec->last_event;
    double ratecnv = (double) g_mzhal.gdgclk_base / cmt_stream_get_rate ( stream );
    uint32_t count_samples = round ( (double) count_ticks / ratecnv );

    //printf ( "SAVE: %d - %d - %d\n", value, count_ticks, count_samples );

    if ( stream->stream_type == CMT_STREAM_TYPE_VSTREAM ) {
        st_CMT_VSTREAM *vstream = stream->str.vstream;
        cmt_vstream_add_value ( vstream, value, count_samples );
    } else if ( stream->stream_type == CMT_STREAM_TYPE_BITSTREAM ) {
        printf ( "%s(): %d - Bitstream is not implemented\n", __func__, __LINE__ );
    } else {
        printf ( "%s(): %d - Unsupported stream type %d\n", __func__, __LINE__, stream->stream_type );
    };

    blspec->last_event = play_ticks;
}


static void cmtsave_stop ( void ) {
    assert ( g_cmt_save->block );
    st_CMTEXT_BLOCK *block = g_cmt_save->block;
    st_CMTSAVE_BLOCKSPEC *blspec = block->spec;
    st_CMT_STREAM *stream = block->stream;
    if ( !stream ) return;
    cmt_stream_save_wav ( stream, CMTSAVE_DEFAULT_SAMPLERATE, blspec->filepath );
}


static void cmtsave_exit ( void ) {
    cmtsave_eject ( );
}

st_CMTEXT g_cmt_save_extension = {
                                  &g_cmt_save_info,
                                  (st_CMTEXT_CONTAINER*) NULL,
                                  (st_CMTEXT_BLOCK*) NULL,
                                  NULL, //cmtwav_init,
                                  cmtsave_exit,
                                  cmtsave_container_open,
                                  cmtsave_stop,
                                  cmtsave_eject,
                                  cmtsave_write_data,
};

st_CMTEXT *g_cmt_save = &g_cmt_save_extension;
