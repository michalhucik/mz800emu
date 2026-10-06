/* 
 * File:   cmtext.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. května 2018, 13:19
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
#include <strings.h>

#include "cmtext.h"
#include "cmtext_container.h"
#include "cmtext_block.h"

/* Pripojeni cmt extenzi */

#include "cmt_wav.h"
#include "cmt_mzf.h"
#include "cmt_mzftape.h"
#include "cmt_tap.h"
#include "cmt_save.h"
#include "cmt_edge.h"

st_CMTEXT *g_cmtext[] = {
                         &g_cmt_wav_extension,
                         &g_cmt_edge_extension,
                         &g_cmt_mzf_extension,
                         &g_cmt_mzftape_extension,
                         &g_cmt_tap_extension,
                         &g_cmt_save_extension,
                         &g_cmt_edge_save_extension,
                         NULL
};

/* Obecne cmtext rutiny */

int g_count_extensions = 0;


void cmtext_init ( void ) {
    int i = 0;
    while ( g_cmtext[i] != NULL ) {
        if ( g_cmtext[i]->cb_init != NULL ) g_cmtext[i]->cb_init ( );
        i++;
    };
    g_count_extensions = i;
}


void cmtext_exit ( void ) {
    int i;
    for ( i = 0; i < g_count_extensions; i++ ) {
        if ( g_cmtext[i]->cb_exit != NULL ) g_cmtext[i]->cb_exit ( );
    };
}


const char* cmtext_get_filename_extension ( const char *filename ) {
    if ( !filename || !filename[0] ) return NULL;
    int end_pos = strlen ( filename ) - 1;
    int pos = end_pos;
    while ( pos ) {
        if ( ( filename[pos] == '/' ) || ( filename[pos] == '\\' ) ) return NULL;
        if ( filename[pos] == '.' ) {
            break;
        };
        pos--;
    };
    if ( !pos ) return NULL;
    pos++;
    if ( pos == end_pos ) return NULL;
    return &filename[pos];
}


st_CMTEXT* cmtext_get_extension ( const char *filename ) {

    const char *file_ext = cmtext_get_filename_extension ( filename );
    if ( !file_ext ) return NULL;

    int i;
    for ( i = 0; i < g_count_extensions; i++ ) {
        if ( ( g_cmtext[i]->info == NULL ) || ( !( g_cmtext[i]->info->type & CMTEXT_TYPE_PLAYABLE ) ) || ( g_cmtext[i]->info->fileext == NULL ) ) continue;
        char **valid_fileexts = g_cmtext[i]->info->fileext;
        int j = 0;
        while ( valid_fileexts[j] != NULL ) {
            if ( 0 == strcasecmp ( file_ext, valid_fileexts[j++] ) ) return g_cmtext[i];
        };
    };

    return NULL;
}


st_CMTEXT* cmtext_get_recording_extension ( void ) {
    int i;
    for ( i = 0; i < g_count_extensions; i++ ) {
        if ( ( g_cmtext[i]->info == NULL ) || ( !( g_cmtext[i]->info->type & CMTEXT_TYPE_RECORDABLE ) ) ) continue;
        return g_cmtext[i];
    };
    return NULL;
}


st_CMTEXT* cmtext_get_recording_extension_for_filename ( const char *filename ) {
    const char *file_ext = cmtext_get_filename_extension ( filename );
    if ( !file_ext ) return cmtext_get_recording_extension ( );

    int i;
    for ( i = 0; i < g_count_extensions; i++ ) {
        if ( ( g_cmtext[i]->info == NULL ) ||
             ( !( g_cmtext[i]->info->type & CMTEXT_TYPE_RECORDABLE ) ) ||
             ( g_cmtext[i]->info->fileext == NULL ) ) continue;
        char **valid_fileexts = g_cmtext[i]->info->fileext;
        int j = 0;
        while ( valid_fileexts[j] != NULL ) {
            if ( 0 == strcasecmp ( file_ext, valid_fileexts[j++] ) ) return g_cmtext[i];
        };
    };
    return NULL;
}


const char* cmtext_get_description ( st_CMTEXT *ext ) {
    if ( ( ext ) && ( ext->info ) && ( ext->info->description ) ) {
        return ext->info->description;
    };
    return "Unknown CMT extension (!)";
}


const char* cmtext_get_name ( st_CMTEXT *ext ) {
    if ( ( ext ) && ( ext->info ) && ( ext->info->name ) ) {
        return ext->info->name;
    };
    return "CMT_UNKNOWN";
}


int cmtext_get_type ( st_CMTEXT *ext ) {
    if ( ( ext ) && ( ext->info ) ) {
        return ext->info->type;
    };
    return CMTEXT_TYPE_UNKNOWN;
}


st_CMTEXT_CONTAINER* cmtext_get_container ( st_CMTEXT *ext ) {
    return ext->container;
}

st_CMTEXT_BLOCK* cmtext_get_block ( st_CMTEXT *ext ) {
    return ext->block;
}


int cmtext_is_playable ( st_CMTEXT *ext ) {
    if ( ( ext ) && ( ext->info ) ) {
        return ( ext->info->type & CMTEXT_TYPE_PLAYABLE ) ? EXIT_SUCCESS : EXIT_FAILURE;
    };
    return EXIT_FAILURE;
}


int cmtext_is_recordable ( st_CMTEXT *ext ) {
    if ( ( ext ) && ( ext->info ) ) {
        return ( ext->info->type & CMTEXT_TYPE_RECORDABLE ) ? EXIT_SUCCESS : EXIT_FAILURE;
    };
    return EXIT_FAILURE;
}
