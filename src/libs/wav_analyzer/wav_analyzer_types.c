/**
 * @file   wav_analyzer_types.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Implementace společných funkcí knihovny wav_analyzer.
 *
 * Obsahuje implementace destruktorů, textových popisů chybových kódů,
 * inicializaci výchozí konfigurace a názvy formátů.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wav_analyzer_types.h"


/** @brief Textové popisy chybových kódů wav analyzéru (anglicky). */
static const char *g_wav_analyzer_error_strings[] = {
    [WAV_ANALYZER_OK]                   = "success",
    [WAV_ANALYZER_ERROR_ALLOC]          = "memory allocation failed",
    [WAV_ANALYZER_ERROR_INVALID_PARAM]  = "invalid parameter",
    [WAV_ANALYZER_ERROR_IO]             = "I/O error",
    [WAV_ANALYZER_ERROR_WAV_FORMAT]     = "unsupported WAV format",
    [WAV_ANALYZER_ERROR_NO_SIGNAL]      = "no detectable signal in WAV",
    [WAV_ANALYZER_ERROR_NO_LEADER]      = "no leader tone found",
    [WAV_ANALYZER_ERROR_UNKNOWN_FORMAT] = "unknown tape format",
    [WAV_ANALYZER_ERROR_DECODE_TAPEMARK] = "tapemark detection failed",
    [WAV_ANALYZER_ERROR_DECODE_DATA]    = "data decoding failed",
    [WAV_ANALYZER_ERROR_DECODE_CHECKSUM] = "checksum mismatch",
    [WAV_ANALYZER_ERROR_DECODE_INCOMPLETE] = "incomplete data block",
    [WAV_ANALYZER_ERROR_BUFFER_OVERFLOW] = "output buffer overflow",
};


/** @brief Textové názvy kazetových formátů (anglicky). */
static const char *g_wav_tape_format_names[] = {
    [WAV_TAPE_FORMAT_UNKNOWN]       = "UNKNOWN",
    [WAV_TAPE_FORMAT_NORMAL]        = "NORMAL",
    [WAV_TAPE_FORMAT_MZ80B]         = "MZ-80B",
    [WAV_TAPE_FORMAT_SINCLAIR]      = "SINCLAIR",
    [WAV_TAPE_FORMAT_CPM_CMT]       = "CPM-CMT",
    [WAV_TAPE_FORMAT_CPM_TAPE]      = "CPM-TAPE",
    [WAV_TAPE_FORMAT_TURBO]         = "TURBO",
    [WAV_TAPE_FORMAT_FASTIPL]       = "FASTIPL",
    [WAV_TAPE_FORMAT_BSD]           = "BSD",
    [WAV_TAPE_FORMAT_FSK]           = "FSK",
    [WAV_TAPE_FORMAT_SLOW]          = "SLOW",
    [WAV_TAPE_FORMAT_DIRECT]        = "DIRECT",
    [WAV_TAPE_FORMAT_ZX_SPECTRUM]   = "ZX SPECTRUM",
};


const char* wav_analyzer_error_string ( en_WAV_ANALYZER_ERROR err ) {
    if ( err < 0 || err >= ( int ) ( sizeof ( g_wav_analyzer_error_strings ) / sizeof ( g_wav_analyzer_error_strings[0] ) ) ) {
        return "unknown error";
    }
    const char *str = g_wav_analyzer_error_strings[err];
    return str ? str : "unknown error";
}


const char* wav_tape_format_name ( en_WAV_TAPE_FORMAT format ) {
    if ( format < 0 || format >= WAV_TAPE_FORMAT_COUNT ) {
        return "UNKNOWN";
    }
    const char *str = g_wav_tape_format_names[format];
    return str ? str : "UNKNOWN";
}


const char* wav_recovery_status_string ( uint32_t status ) {
    if ( status == WAV_RECOVERY_NONE ) return "complete";
    if ( status & WAV_RECOVERY_BSD_GAPS ) return "BSD chunks missing";
    if ( status & WAV_RECOVERY_BSD_INCOMPLETE ) return "BSD incomplete (missing terminator chunk)";
    if ( status & WAV_RECOVERY_PARTIAL_BODY ) return "partial body";
    if ( status & WAV_RECOVERY_HEADER_ONLY ) return "header only";
    if ( status & WAV_RECOVERY_FSIZE_REPAIRED ) return "fsize repaired from body length";
    if ( status & WAV_RECOVERY_COPY_MERGED ) return "copies merged";
    return "unknown recovery status";
}


void wav_analyzer_config_default ( st_WAV_ANALYZER_CONFIG *config ) {
    if ( !config ) return;
    memset ( config, 0, sizeof ( *config ) );
    config->pulse_mode = WAV_PULSE_MODE_ZERO_CROSSING;
    config->tolerance = WAV_ANALYZER_DEFAULT_TOLERANCE;
    config->schmitt_high = WAV_ANALYZER_DEFAULT_SCHMITT_HIGH;
    config->schmitt_low = WAV_ANALYZER_DEFAULT_SCHMITT_LOW;
    config->min_leader_pulses = WAV_ANALYZER_DEFAULT_MIN_LEADER_PULSES;
    config->channel = WAV_CHANNEL_LEFT;
    config->polarity = WAV_SIGNAL_POLARITY_NORMAL;
    config->enable_dc_offset = 1;
    config->enable_highpass = 1;
    config->enable_normalize = 1;
    config->highpass_alpha = 0.997;
    config->histogram_bin_width = WAV_ANALYZER_DEFAULT_HISTOGRAM_BIN_WIDTH;
    config->verbose = 0;
    config->keep_unknown = 0;
    config->pass_count = 1;
    config->raw_format = WAV_RAW_FORMAT_DIRECT;
    config->recover_bsd = 0;
    config->recover_body = 0;
    config->recover_header = 0;
}


void wav_pulse_stats_compute ( const st_WAV_PULSE_SEQUENCE *seq,
                               st_WAV_PULSE_STATS *out_stats ) {
    if ( !seq || !out_stats ) return;

    memset ( out_stats, 0, sizeof ( *out_stats ) );

    if ( seq->count == 0 ) return;

    out_stats->count = seq->count;
    out_stats->min_us = seq->pulses[0].duration_us;
    out_stats->max_us = seq->pulses[0].duration_us;

    /*
     * Welfordova jednoprůchodová metoda pro průměr a rozptyl.
     * Numericky stabilní i pro velké soubory dat.
     */
    double mean = 0.0;
    double m2 = 0.0;

    for ( uint32_t i = 0; i < seq->count; i++ ) {
        double val = seq->pulses[i].duration_us;

        if ( val < out_stats->min_us ) out_stats->min_us = val;
        if ( val > out_stats->max_us ) out_stats->max_us = val;

        double delta = val - mean;
        mean += delta / ( double ) ( i + 1 );
        double delta2 = val - mean;
        m2 += delta * delta2;
    }

    out_stats->avg_us = mean;
    out_stats->stddev_us = ( seq->count > 1 ) ? sqrt ( m2 / ( double ) seq->count ) : 0.0;
}


void wav_analyzer_raw_block_destroy ( st_WAV_ANALYZER_RAW_BLOCK *block ) {
    if ( !block ) return;
    free ( block->data );
    block->data = NULL;
    block->data_size = 0;
}


void wav_pulse_sequence_destroy ( st_WAV_PULSE_SEQUENCE *seq ) {
    if ( !seq ) return;
    free ( seq->pulses );
    seq->pulses = NULL;
    seq->count = 0;
    seq->capacity = 0;
}


void wav_leader_list_destroy ( st_WAV_LEADER_LIST *list ) {
    if ( !list ) return;
    free ( list->leaders );
    list->leaders = NULL;
    list->count = 0;
    list->capacity = 0;
}


void wav_decode_result_list_destroy ( st_WAV_DECODE_RESULT_LIST *list ) {
    if ( !list ) return;
    for ( uint32_t i = 0; i < list->count; i++ ) {
        free ( list->results[i].data );
        list->results[i].data = NULL;
    }
    free ( list->results );
    list->results = NULL;
    list->count = 0;
    list->capacity = 0;
}
