/**
 * @file   cmt_bitstream.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.1
 * @brief  Implementace bitstream formátu — kondenzovaný WAV, 1 bit = 1 vzorek.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
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
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include "../baseui_compat.h"

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"

#include "cmt_bitstream.h"
#include "cmt_vstream.h"

/** @brief WAV level konstanta pro HIGH stav při konverzi bitstream -> 8-bit PCM */
#define CMT_BITSTREAM_WAV_LEVEL_HIGH   -48
/** @brief WAV level konstanta pro LOW stav při konverzi bitstream -> 8-bit PCM */
#define CMT_BITSTREAM_WAV_LEVEL_LOW     48


/**
 * @brief Uvolní bitstream a jeho data.
 *
 * Bezpečné volání s NULL ukazatelem.
 *
 * @param stream Ukazatel na bitstream (NULL je bezpečné)
 */
void cmt_bitstream_destroy ( st_CMT_BITSTREAM *stream ) {
    if ( !stream ) return;
    if ( stream->data ) {
        baseui_tools_mem_free ( stream->data );
    }
    baseui_tools_mem_free ( stream );
}


/**
 * @brief Vytvoří nový prázdný bitstream s danou vzorkovací frekvencí,
 * počtem bloků a polaritou.
 *
 * Pokud blocks == 0, vytvoří stream bez datové oblasti.
 *
 * @param rate Vzorkovací frekvence (Hz)
 * @param blocks Počet 32-bitových bloků (0 = bez datové oblasti)
 * @param polarity Polarita signálu
 * @return Ukazatel na nový bitstream, nebo NULL při selhání alokace
 */
st_CMT_BITSTREAM* cmt_bitstream_new ( uint32_t rate, uint32_t blocks, en_CMT_STREAM_POLARITY polarity ) {

    st_CMT_BITSTREAM *stream = baseui_tools_mem_alloc0 ( sizeof ( st_CMT_BITSTREAM ) );

    if ( !stream ) {
        fprintf ( stderr, "%s():%d: Error: can't allocate memory (%zu B)\n", __func__, __LINE__, sizeof ( st_CMT_BITSTREAM ) );
        return NULL;
    }

    stream->rate = rate;
    stream->scan_time = ( 1 / (double) rate );
    stream->polarity = polarity;

    if ( blocks ) {
        stream->data = baseui_tools_mem_alloc0 ( ( CMT_BITSTREAM_BLOCK_SIZE / 8 ) * blocks );
        if ( !stream->data ) {
            fprintf ( stderr, "%s():%d: Error: can't allocate memory (%zu B)\n", __func__, __LINE__, (size_t) ( CMT_BITSTREAM_BLOCK_SIZE / 8 ) * blocks );
            cmt_bitstream_destroy ( stream );
            return NULL;
        }
        stream->blocks = blocks;
    }

    return stream;
}


/**
 * @brief Vytvoří bitstream převzorkováním dat z vstreamu.
 *
 * Pokud dst_rate == 0, použije vzorkovací frekvenci vstreamu.
 * Při rozdílné frekvenci provádí resampling s detekcí majoritní hodnoty.
 *
 * @param vstream Zdrojový vstream
 * @param dst_rate Cílová vzorkovací frekvence (0 = použít frekvenci vstreamu)
 * @return Ukazatel na nový bitstream, nebo NULL při selhání
 */
st_CMT_BITSTREAM* cmt_bitstream_new_from_vstream ( st_CMT_VSTREAM *vstream, uint32_t dst_rate ) {

    uint32_t rate = cmt_vstream_get_rate ( vstream );
    if ( dst_rate == 0 ) dst_rate = rate;
    double step = (double) rate / dst_rate;

    uint64_t scans = cmt_vstream_get_count_scans ( vstream );
    uint64_t dst_scans = scans / step;
    if ( ( dst_scans * step ) < scans ) dst_scans++;
    uint32_t blocks = cmt_bitstream_compute_required_blocks_from_scans ( dst_scans );

    en_CMT_STREAM_POLARITY polarity = cmt_vstream_get_polarity ( vstream );
    st_CMT_BITSTREAM *stream = cmt_bitstream_new ( dst_rate, blocks, polarity );
    if ( !stream ) return NULL;

    int value = 0;
    uint32_t dst_position = 0;

    double next_scan = step;
    double total_samples = 0;
    uint64_t samples = 0;

    if ( dst_rate == rate ) {
        /* stejná frekvence — přímý přepis bez resamplingu */
        while ( EXIT_SUCCESS == cmt_vstream_read_pulse ( vstream, &samples, &value ) ) {
            uint64_t i;
            for ( i = 0; i < samples; i++ ) {
                cmt_bitstream_set_value_on_position ( stream, dst_position++, value );
            }
        }
    } else {
        /* rozdílná frekvence — resampling s detekcí majoritní hodnoty */
        int previous_value = 0;
        double scan_min_limit = step / 2;
        while ( EXIT_SUCCESS == cmt_vstream_read_pulse ( vstream, &samples, &value ) ) {

            if ( !samples ) continue;   /* event s délkou 0 (cmt_vstream_add_value_long) */
            double d_samples = samples;

            while ( ( total_samples + d_samples ) >= next_scan ) {

                double new_state_samples = ( next_scan - total_samples );
                total_samples += new_state_samples;
                d_samples -= new_state_samples;
                next_scan += step;

                int sample_value;
                if ( previous_value == value ) {
                    sample_value = value;
                } else {
                    sample_value = ( new_state_samples >= scan_min_limit ) ? value : previous_value;
                }
                cmt_bitstream_set_value_on_position ( stream, dst_position++, sample_value );

                previous_value = value;
            }

            previous_value = value;
            total_samples += d_samples;
        }
    }

    return stream;
}


/**
 * @brief Spočítá minimální počet 32-bitových bloků potřebných pro daný počet vzorků.
 *
 * Přijímá uint64_t pro bezpečnou práci s velkými streamy,
 * ale vrací uint32_t (maximálně ~134M bloků = ~4G vzorků).
 *
 * @param scans Počet vzorků
 * @return Potřebný počet bloků (UINT32_MAX při přetečení)
 */
uint32_t cmt_bitstream_compute_required_blocks_from_scans ( uint64_t scans ) {
    uint64_t required_blocks = scans / CMT_BITSTREAM_BLOCK_SIZE;
    if ( scans % CMT_BITSTREAM_BLOCK_SIZE ) required_blocks++;
    if ( required_blocks > UINT32_MAX ) {
        fprintf ( stderr, "%s():%d: Error: required blocks overflow uint32_t (scans=%" PRIu64 ")\n", __func__, __LINE__, scans );
        return UINT32_MAX;
    }
    return (uint32_t) required_blocks;
}


/**
 * @brief Vytvoří bitstream ze souboru WAV (přes handler).
 *
 * Každý WAV vzorek se převede na 1 bit (prahování).
 *
 * @param wav_handler Handler na WAV data
 * @param polarity Polarita signálu
 * @return Ukazatel na nový bitstream, nebo NULL při selhání
 */
st_CMT_BITSTREAM* cmt_bitstream_new_from_wav ( st_HANDLER *wav_handler, en_CMT_STREAM_POLARITY polarity ) {

    st_HANDLER *h = wav_handler;

    en_WAV_ERROR wav_err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &wav_err );

    if ( !sh ) {
        fprintf ( stderr, "%s():%d: Error: can't create wav_simple_header: %s\n", __func__, __LINE__, wav_error_string ( wav_err ) );
        return NULL;
    }

    uint32_t scans = sh->blocks;
    uint32_t blocks = cmt_bitstream_compute_required_blocks_from_scans ( scans );

    st_CMT_BITSTREAM *stream = cmt_bitstream_new ( sh->sample_rate, blocks, polarity );
    if ( !stream ) {
        fprintf ( stderr, "%s():%d: Error: can't create cmt_bitstream.\n", __func__, __LINE__ );
        wav_simple_header_destroy ( sh );
        return NULL;
    }

    stream->scans = scans;
    stream->stream_length = scans * ( 1 / (double) stream->rate );
    en_WAV_POLARITY wav_polarity = ( polarity == CMT_STREAM_POLARITY_NORMAL ) ? WAV_POLARITY_NORMAL : WAV_POLARITY_INVERTED;

    uint32_t i;
    for ( i = 0; i < scans; i++ ) {
        int bit_value = 0;
        if ( WAV_OK != wav_get_bit_value_of_sample ( h, sh, i, wav_polarity, &bit_value ) ) {
            fprintf ( stderr, "%s():%d: Error: can't read sample.\n", __func__, __LINE__ );
            wav_simple_header_destroy ( sh );
            cmt_bitstream_destroy ( stream );
            return NULL;
        }

        cmt_bitstream_set_value_on_position ( stream, i, bit_value );
    }

    wav_simple_header_destroy ( sh );

    return stream;
}


/**
 * @brief Vytvoří WAV data z bitstreamu a zapíše je přes handler.
 *
 * Generuje 8-bit mono PCM WAV. Deleguje zápis RIFF struktury
 * na wav_write() z knihovny wav. Pouze konvertuje bitstream data
 * na 8-bit PCM vzorky.
 *
 * @param wav_handler Handler pro zápis WAV dat
 * @param stream Zdrojový bitstream
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
 */
int cmt_bitstream_create_wav ( st_HANDLER *wav_handler, st_CMT_BITSTREAM *stream ) {

    /* konverze bitstreamu na 8-bit PCM pole */
    uint8_t *samples = baseui_tools_mem_alloc ( stream->scans );
    if ( !samples ) {
        fprintf ( stderr, "%s():%d: Error: could not allocate sample buffer (%u B)\n", __func__, __LINE__, stream->scans );
        return EXIT_FAILURE;
    }

    uint32_t i;
    for ( i = 0; i < stream->scans; i++ ) {
        int bit = cmt_bitstream_get_value_on_position ( stream, i );
        /* 8-bit unsigned PCM: záporný puls → pod středem, kladný → nad středem */
        samples[i] = (uint8_t) ( bit ? ( 128 + CMT_BITSTREAM_WAV_LEVEL_HIGH ) : ( 128 + CMT_BITSTREAM_WAV_LEVEL_LOW ) );
    }

    en_WAV_ERROR err = wav_write ( wav_handler, stream->rate, 1, 8, samples, stream->scans );

    baseui_tools_mem_free ( samples );

    if ( err != WAV_OK ) {
        fprintf ( stderr, "%s():%d: Error: wav_write failed: %s\n", __func__, __LINE__, wav_error_string ( err ) );
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Změní polaritu bitstreamu — invertuje všechny bity v datové oblasti.
 *
 * Pokud je aktuální polarita shodná s požadovanou, nedělá nic.
 *
 * @param stream Ukazatel na bitstream
 * @param polarity Požadovaná polarita
 */
void cmt_bitstream_change_polarity ( st_CMT_BITSTREAM *stream, en_CMT_STREAM_POLARITY polarity ) {
    if ( stream->polarity == polarity ) return;
    uint32_t i;
    for ( i = 0; i < stream->blocks; i++ ) {
        stream->data[i] = ~stream->data[i];
    }
    stream->polarity = polarity;
}


/**
 * @brief Vrátí velikost datové oblasti v bajtech.
 * @param bitstream Ukazatel na bitstream
 * @return Velikost v bajtech
 */
uint32_t cmt_bitstream_get_size ( st_CMT_BITSTREAM *bitstream ) {
    assert ( bitstream != NULL );
    return bitstream->blocks * (uint32_t) ( CMT_BITSTREAM_BLOCK_SIZE / 8 );
}


/**
 * @brief Vrátí vzorkovací frekvenci (Hz).
 * @param bitstream Ukazatel na bitstream
 * @return Vzorkovací frekvence v Hz
 */
uint32_t cmt_bitstream_get_rate ( st_CMT_BITSTREAM *bitstream ) {
    assert ( bitstream != NULL );
    return bitstream->rate;
}


/**
 * @brief Vrátí celkovou délku streamu (v sekundách).
 * @param bitstream Ukazatel na bitstream
 * @return Délka v sekundách
 */
double cmt_bitstream_get_length ( st_CMT_BITSTREAM *bitstream ) {
    assert ( bitstream != NULL );
    return bitstream->stream_length;
}


/**
 * @brief Vrátí celkový počet vzorků.
 * @param bitstream Ukazatel na bitstream
 * @return Počet vzorků
 */
uint64_t cmt_bitstream_get_count_scans ( st_CMT_BITSTREAM *bitstream ) {
    assert ( bitstream != NULL );
    return bitstream->scans;
}


/**
 * @brief Vrátí délku jednoho vzorku (v sekundách).
 * @param bitstream Ukazatel na bitstream
 * @return Délka jednoho vzorku v sekundách (1/rate)
 */
double cmt_bitstream_get_scantime ( st_CMT_BITSTREAM *bitstream ) {
    assert ( bitstream != NULL );
    return bitstream->scan_time;
}
