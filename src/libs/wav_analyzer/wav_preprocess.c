/**
 * @file   wav_preprocess.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Implementace vrstvy 0 - předspracování WAV signálu.
 *
 * Obsahuje implementace DC offset korekce, IIR high-pass filtru,
 * peak normalizace a výběru kanálu ze sterea.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libs/wav/wav.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/endianity/endianity.h"

#include "wav_preprocess.h"


en_WAV_ANALYZER_ERROR wav_preprocess_load_samples (
    st_HANDLER *h,
    const st_WAV_SIMPLE_HEADER *sh,
    uint16_t channel,
    double **out_buffer,
    uint32_t *out_count
) {
    if ( !h || !sh || !out_buffer || !out_count ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( channel >= sh->channels ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    uint32_t total_samples = sh->blocks;
    if ( total_samples == 0 ) {
        return WAV_ANALYZER_ERROR_NO_SIGNAL;
    }

    /*
     * Hromadne nacteni: precte cely datovy blok WAV jednim volanim
     * generic_driver_read() a konvertuje na double buffer.
     * Radove rychlejsi nez per-vzorek I/O (143M operaci -> 1 operace).
     */
    uint16_t sample_bytes = sh->bits_per_sample / 8;
    uint32_t raw_size = total_samples * sh->block_size;
    uint8_t *raw = ( uint8_t* ) malloc ( raw_size );
    if ( !raw ) {
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    if ( EXIT_SUCCESS != generic_driver_read ( h, sh->real_data_offset, raw, raw_size ) ) {
        free ( raw );
        return WAV_ANALYZER_ERROR_IO;
    }

    double *buffer = ( double* ) malloc ( total_samples * sizeof ( double ) );
    if ( !buffer ) {
        free ( raw );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    /* offset kanalu v ramci jednoho bloku */
    uint32_t ch_offset = channel * sample_bytes;

    for ( uint32_t i = 0; i < total_samples; i++ ) {
        uint8_t *sample_ptr = raw + ( ( uint32_t ) i * sh->block_size ) + ch_offset;

        /* jediný převod vzorků v knihovně wav (PCM 8-64 bit, IEEE float) */
        if ( wav_decode_sample ( sh, sample_ptr, &buffer[i] ) != WAV_OK ) {
            free ( raw );
            free ( buffer );
            return WAV_ANALYZER_ERROR_INVALID_PARAM;
        }
    }

    free ( raw );

    *out_buffer = buffer;
    *out_count = total_samples;
    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_preprocess_dc_offset (
    double *buffer,
    uint32_t count
) {
    if ( !buffer || count == 0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* spočítáme průměr */
    double sum = 0.0;
    for ( uint32_t i = 0; i < count; i++ ) {
        sum += buffer[i];
    }
    double mean = sum / count;

    /* odečteme průměr od každého vzorku */
    for ( uint32_t i = 0; i < count; i++ ) {
        buffer[i] -= mean;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_preprocess_highpass (
    double *buffer,
    uint32_t count,
    double alpha
) {
    if ( !buffer || count == 0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( alpha <= 0.0 || alpha >= 1.0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /*
     * 1-pólový IIR high-pass filtr:
     * y[n] = alpha * (y[n-1] + x[n] - x[n-1])
     *
     * Mezní frekvence fc = (1 - alpha) * fs / (2 * pi * alpha); výchozí
     * alpha 0.997 dává asi 21 Hz při 44,1 kHz (alpha nezávisí na fs).
     */
    double prev_x = buffer[0];
    double prev_y = buffer[0];

    for ( uint32_t i = 1; i < count; i++ ) {
        double x = buffer[i];
        double y = alpha * ( prev_y + x - prev_x );
        buffer[i] = y;
        prev_x = x;
        prev_y = y;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_preprocess_normalize (
    double *buffer,
    uint32_t count
) {
    if ( !buffer || count == 0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* najdeme maximální absolutní hodnotu */
    double max_abs = 0.0;
    for ( uint32_t i = 0; i < count; i++ ) {
        double abs_val = fabs ( buffer[i] );
        if ( abs_val > max_abs ) {
            max_abs = abs_val;
        }
    }

    /* pokud je signál nulový, nedělej nic */
    if ( max_abs < 1e-15 ) {
        return WAV_ANALYZER_OK;
    }

    /* normalizujeme */
    double scale = 1.0 / max_abs;
    for ( uint32_t i = 0; i < count; i++ ) {
        buffer[i] *= scale;
    }

    return WAV_ANALYZER_OK;
}


uint16_t wav_preprocess_select_channel (
    st_HANDLER *h,
    const st_WAV_SIMPLE_HEADER *sh,
    en_WAV_CHANNEL_SELECT channel_select
) {
    if ( !h || !sh ) return 0;

    /* mono - vždy kanál 0 */
    if ( sh->channels == 1 ) return 0;

    switch ( channel_select ) {
        case WAV_CHANNEL_LEFT:
            return 0;
        case WAV_CHANNEL_RIGHT:
            return ( sh->channels > 1 ) ? 1 : 0;
        case WAV_CHANNEL_AUTO:
            break;
    }

    /*
     * Automatický výběr: porovnáme RMS obou kanálů.
     * Vzorkujeme každých N vzorků pro rychlost.
     */
    double rms_left = 0.0;
    double rms_right = 0.0;
    uint32_t step = ( sh->blocks > 10000 ) ? ( sh->blocks / 10000 ) : 1;
    uint32_t samples_counted = 0;

    for ( uint32_t i = 0; i < sh->blocks; i += step ) {
        double val_l = 0.0, val_r = 0.0;
        wav_get_sample_float ( h, sh, i, 0, &val_l );
        wav_get_sample_float ( h, sh, i, 1, &val_r );
        rms_left += val_l * val_l;
        rms_right += val_r * val_r;
        samples_counted++;
    }

    if ( samples_counted > 0 ) {
        rms_left = sqrt ( rms_left / samples_counted );
        rms_right = sqrt ( rms_right / samples_counted );
    }

    return ( rms_right > rms_left ) ? 1 : 0;
}


en_WAV_ANALYZER_ERROR wav_preprocess_run (
    st_HANDLER *h,
    const st_WAV_SIMPLE_HEADER *sh,
    const st_WAV_ANALYZER_CONFIG *config,
    double **out_buffer,
    uint32_t *out_count
) {
    if ( !h || !sh || !config || !out_buffer || !out_count ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* 1. výběr kanálu */
    uint16_t channel = wav_preprocess_select_channel ( h, sh, config->channel );

    /* 2. načtení vzorků */
    double *buffer = NULL;
    uint32_t count = 0;
    en_WAV_ANALYZER_ERROR err = wav_preprocess_load_samples ( h, sh, channel, &buffer, &count );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* 3. DC offset korekce */
    if ( config->enable_dc_offset ) {
        err = wav_preprocess_dc_offset ( buffer, count );
        if ( err != WAV_ANALYZER_OK ) {
            free ( buffer );
            return err;
        }
    }

    /* 4. high-pass filtr */
    if ( config->enable_highpass ) {
        err = wav_preprocess_highpass ( buffer, count, config->highpass_alpha );
        if ( err != WAV_ANALYZER_OK ) {
            free ( buffer );
            return err;
        }
    }

    /* 5. normalizace */
    if ( config->enable_normalize ) {
        err = wav_preprocess_normalize ( buffer, count );
        if ( err != WAV_ANALYZER_OK ) {
            free ( buffer );
            return err;
        }
    }

    *out_buffer = buffer;
    *out_count = count;
    return WAV_ANALYZER_OK;
}
