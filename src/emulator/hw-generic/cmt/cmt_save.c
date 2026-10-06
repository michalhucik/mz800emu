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
#include <limits.h>


#include "baseui/baseui.h"
#include "fs_layer.h"

#include "hw-generic/gdg/gdgclk.h"
#include "cmt.h"
#include "cmtext.h"
#include "cmt_save.h"


char *g_cmt_save_fileext[] = {
                              "wav",
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

    //stream->str.vstream = cmt_vstream_new ( GDGCLK_BASE, CMT_VSTREAM_BYTELENGTH16, value, CMT_STREAM_POLARITY_NORMAL );
    stream->str.vstream = cmt_vstream_new ( CMTSAVE_DEFAULT_SAMPLERATE, CMT_VSTREAM_BYTELENGTH8, value, CMT_STREAM_POLARITY_NORMAL );
    if ( !stream->str.vstream ) {
        cmt_stream_destroy ( stream );
        return NULL;
    };

    printf ( "%s stream type: %s\n", cmtext_get_name ( g_cmt_save ), cmt_stream_get_stream_type_txt ( stream ) );
    printf ( "%s rate: %d Hz\n", cmtext_get_name ( g_cmt_save ), cmt_stream_get_rate ( stream ) );

    return stream;
}


static uint64_t cmtsave_ticks_to_samples ( uint64_t ticks, uint32_t rate, int *ok ) {
    const uint64_t base = (uint64_t) GDGCLK_BASE;
    if ( ok ) *ok = 0;
    if ( rate == 0 || base == 0 ) return 0;

    uint64_t quotient = ticks / base;
    uint64_t remainder = ticks % base;
    if ( quotient > UINT64_MAX / rate ) return 0;

    uint64_t samples = quotient * rate;
    uint64_t fraction = remainder * (uint64_t) rate;
    uint64_t rounded = ( fraction + base / 2u ) / base;
    if ( samples > UINT64_MAX - rounded ) return 0;

    if ( ok ) *ok = 1;
    return samples + rounded;
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
        blspec->last_event = ( play_ticks > GDGCLK_BASE ) ? ( play_ticks - GDGCLK_BASE ) : 0;
        blspec->quantization_start = blspec->last_event;
    };

    if ( play_ticks <= blspec->last_event ) return;

    /*
     * WAV uses a continuous sample clock.  Quantize the absolute elapsed
     * emulator time to a target sample position and emit only the difference
     * from samples already written.  Per-edge rounding error is therefore
     * carried into following intervals instead of accumulating as drift.
     */
    int ok = 0;
    uint64_t elapsed_ticks = play_ticks - blspec->quantization_start;
    uint64_t target_samples = cmtsave_ticks_to_samples (
        elapsed_ticks, cmt_stream_get_rate ( stream ), &ok );
    if ( !ok ) return;

    uint64_t count_samples;
    uint64_t next_emitted_samples;
    if ( target_samples > blspec->emitted_samples ) {
        count_samples = target_samples - blspec->emitted_samples;
        next_emitted_samples = target_samples;
    } else {
        if ( blspec->emitted_samples == UINT64_MAX ) return;
        count_samples = 1;
        next_emitted_samples = blspec->emitted_samples + 1u;
    };
    if ( count_samples > UINT32_MAX ) return;

    //printf ( "SAVE: %d - %llu\n", value, (unsigned long long) count_samples );

    if ( stream->stream_type == CMT_STREAM_TYPE_VSTREAM ) {
        st_CMT_VSTREAM *vstream = stream->str.vstream;
        if ( EXIT_SUCCESS != cmt_vstream_add_value (
                vstream, value, (uint32_t) count_samples ) ) return;
    } else if ( stream->stream_type == CMT_STREAM_TYPE_BITSTREAM ) {
        printf ( "%s(): %d - Bitstream is not implemented\n", __func__, __LINE__ );
    } else {
        printf ( "%s(): %d - Unsupported stream type %d\n", __func__, __LINE__, stream->stream_type );
    };

    blspec->emitted_samples = next_emitted_samples;
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
