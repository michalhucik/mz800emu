/* 
 * File:   cmtext_block.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:25
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
#include <stdint.h>
#include <math.h>
#include <assert.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "mzarch/mzhal.h"

#include "baseui/baseui.h"

#include "cmtext_block.h"
#include "cmtext.h"


void cmtext_block_destroy ( st_CMTEXT_BLOCK *block ) {
    if ( !block ) return;
    assert ( block->spec == NULL );
    if ( !block->stream ) cmt_stream_destroy ( block->stream );
    baseui_tools_mem_free ( block );
}


st_CMTEXT_BLOCK* cmtext_block_new ( int block_id,
                                    en_CMTEXT_BLOCK_TYPE type,
                                    st_CMT_STREAM *stream,
                                    en_CMTEXT_BLOCK_SPEED block_speed,
                                    uint16_t pause_after,
                                    void *spec ) {

    st_CMTEXT_BLOCK *block = (st_CMTEXT_BLOCK*) baseui_tools_mem_alloc0 ( sizeof ( st_CMTEXT_BLOCK ) );
    if ( !block ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d).\n", __func__, __LINE__, (int) sizeof ( st_CMTEXT_BLOCK ) );
        return NULL;
    };

    block->block_id = block_id;
    block->type = type;
    block->stream = stream;
    block->block_speed = block_speed;
    block->pause_after = pause_after;
    block->spec = spec;

    return block;
}


void cmtext_block_play ( void *cmtext ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = (st_CMTEXT*) cmtext;
    st_CMTEXT_BLOCK *block = cext->block;
    assert ( block != NULL );
    assert ( block->stream != NULL );
    if ( block->stream->stream_type != CMT_STREAM_TYPE_VSTREAM ) return;
    st_CMT_VSTREAM *stream = block->stream->str.vstream;
    assert ( stream != NULL );
    cmt_vstream_read_reset ( stream );
}


const char* cmtext_block_get_playname ( void *cmtext ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = (st_CMTEXT*) cmtext;
    assert ( cext->container != NULL );
    if ( cext->container->type == CMTEXT_CONTAINER_TYPE_SINGLE ) return cmtext_container_get_name ( cext->container );
    return "";
}


void cmtext_block_set_polarity ( void *cmtext, en_CMT_STREAM_POLARITY polarity ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = (st_CMTEXT*) cmtext;
    st_CMTEXT_BLOCK *block = cext->block;
    assert ( block != NULL );
    assert ( block->stream != NULL );
    cmt_stream_set_polarity ( block->stream, polarity );
}


en_CMTEXT_BLOCK_PLAYSTS cmtext_block_get_output ( st_CMTEXT_BLOCK *block, uint64_t play_gdgticks, int *output, uint64_t *transferred_gdgticks ) {

    if ( CMT_STREAM_TYPE_VSTREAM == block->stream->stream_type ) {
        st_CMT_VSTREAM *vstream = block->stream->str.vstream;
        /* konverze GDG ticků na vzorky: play_gdgticks * rate / g_mzhal.gdgclk_base */
        uint64_t play_scans_count = round ( (double) play_gdgticks * vstream->rate / g_mzhal.gdgclk_base );
        if ( EXIT_SUCCESS == cmt_vstream_get_value ( vstream, play_scans_count, output ) ) return CMTEXT_BLOCK_PLAYSTS_BODY;
        // Prekrocili jsme delku streamu
        uint64_t pause_scans_count = block->pause_after * vstream->msticks;
        uint64_t total_scans_count = vstream->scans + pause_scans_count;
        if ( play_scans_count < total_scans_count ) return CMTEXT_BLOCK_PLAYSTS_PAUSE; // prehravame pauzu
        // pauza uz skoncila
        /* konverze zpět ze vzorků na GDG ticky: count * g_mzhal.gdgclk_base / rate */
        *transferred_gdgticks = (uint64_t) round ( (double) ( play_scans_count - total_scans_count ) * g_mzhal.gdgclk_base / vstream->rate );
        return CMTEXT_BLOCK_PLAYSTS_STOP;

    } else if ( CMT_STREAM_TYPE_BITSTREAM == block->stream->stream_type ) {

        st_CMT_BITSTREAM *bitstream = block->stream->str.bitstream;
        double play_time = play_gdgticks * ( 1 / (double) g_mzhal.gdgclk_base );

        if ( play_time < bitstream->stream_length ) {
            uint32_t sample_position = cmt_bitstream_get_position_by_time ( bitstream, play_time );
            *output = cmt_bitstream_get_value_on_position ( bitstream, sample_position );
            return CMTEXT_BLOCK_PLAYSTS_BODY;
        };
        // Prekrocili jsme delku streamu
        double total_length = ( block->pause_after * 0.001 ) + bitstream->stream_length;
        if ( play_time < total_length ) return CMTEXT_BLOCK_PLAYSTS_PAUSE; // prehravame pauzu
        // pauza uz skoncila
        *transferred_gdgticks = ( play_time - total_length ) * (double) g_mzhal.gdgclk_base;
        return CMTEXT_BLOCK_PLAYSTS_STOP;
    };
    fprintf ( stderr, "%s():%d - Unknown stream type '%d'\n", __func__, __LINE__, block->stream->stream_type );
    return CMTEXT_BLOCK_PLAYSTS_STOP;
}


en_CMTEXT_BLOCK_TYPE cmtext_block_get_type ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    return block->type;
}


uint32_t cmtext_block_get_rate ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    assert ( block->stream != NULL );
    return cmt_stream_get_rate ( block->stream );
}


uint32_t cmtext_block_get_size ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    if ( !block->stream ) return 0;
    return cmt_stream_get_size ( block->stream );
}


uint16_t cmtext_block_get_pause_after ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    return block->pause_after;
}


st_CMT_STREAM* cmtext_block_get_stream ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    return block->stream;
}


en_CMT_STREAM_TYPE cmtext_block_get_stream_type ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    assert ( block->stream != NULL );
    return cmt_stream_get_stream_type ( block->stream );
}


int cmtext_block_get_block_id ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    return block->block_id;
}


uint64_t cmtext_block_get_count_scans ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    if ( block->stream != NULL ) return cmt_stream_get_count_scans ( block->stream );
    return 0;
}


double cmtext_block_get_scantime ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    if ( block->stream != NULL ) return cmt_stream_get_scantime ( block->stream );
    return 0;
}


en_CMTEXT_BLOCK_SPEED cmtext_block_get_block_speed ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    return block->block_speed;
}
