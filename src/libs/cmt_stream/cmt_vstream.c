/**
 * @file   cmt_vstream.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Implementace vstream formátu — pulzně-délkové kódování CMT signálu.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
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
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "baseui/baseui.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"

#include "cmt_vstream.h"
#include "cmt_bitstream.h"
#include "cmt_stream.h"


/**
 * @brief Uvolní vstream a jeho data.
 *
 * Bezpečné volání s NULL ukazatelem.
 *
 * @param stream Ukazatel na vstream (NULL je bezpečné)
 */
void cmt_vstream_destroy ( st_CMT_VSTREAM *stream ) {
    if ( !stream ) return;
    if ( stream->data ) baseui_tools_mem_free ( stream->data );
    baseui_tools_mem_free ( stream );
}


/**
 * @brief Resetuje čtecí pozici na začátek streamu.
 * @param stream Ukazatel na vstream
 */
void cmt_vstream_read_reset ( st_CMT_VSTREAM *stream ) {
    stream->last_read_position = 0;
    stream->last_read_position_sample = 0;
    stream->last_read_value = stream->start_value;
}


/*
 * Vytvoří nový prázdný vstream s danou vzorkovací frekvencí,
 * minimální délkou eventu, počáteční hodnotou a polaritou.
 */
st_CMT_VSTREAM* cmt_vstream_new ( uint32_t rate, en_CMT_VSTREAM_BYTELENGTH min_byte_length, int value, en_CMT_STREAM_POLARITY polarity ) {
    if ( !( ( min_byte_length == 1 ) || ( min_byte_length == 2 ) || ( min_byte_length == 4 ) ) ) {
        fprintf ( stderr, "%s():%d: Error: invalid min_byte_length (%d).\n", __func__, __LINE__, min_byte_length );
        return NULL;
    }
    if ( !rate ) {
        fprintf ( stderr, "%s():%d: Error: bad sample rate (%u).\n", __func__, __LINE__, rate );
        return NULL;
    }
    st_CMT_VSTREAM *stream = baseui_tools_mem_alloc0 ( sizeof ( st_CMT_VSTREAM ) );
    if ( !stream ) {
        fprintf ( stderr, "%s():%d: Error: can't allocate memory (%zu B).\n", __func__, __LINE__, sizeof ( st_CMT_VSTREAM ) );
        return NULL;
    }
    stream->rate = rate;
    stream->scan_time = (double) 1 / rate;
    stream->msticks = round ( 0.001 / ( (double) 1 / rate ) );
    stream->start_value = value & 1;
    stream->polarity = polarity;

    stream->capacity = 64;
    stream->data = baseui_tools_mem_alloc0 ( stream->capacity );
    if ( !stream->data ) {
        fprintf ( stderr, "%s():%d: Error: can't allocate memory (%u B).\n", __func__, __LINE__, stream->capacity );
        cmt_vstream_destroy ( stream );
        return NULL;
    }

    stream->stream_length = 0;
    stream->min_byte_length = min_byte_length;
    stream->last_set_value = value & 1;
    stream->size = min_byte_length;
    stream->last_event_byte_length = min_byte_length;

    cmt_vstream_read_reset ( stream );

    return stream;
}


/*
 * Přečte délku posledního zapsaného eventu (od konce datové oblasti).
 * Používá bezpečný memcpy přístup.
 */
static inline uint32_t cmt_vstream_get_last_event ( st_CMT_VSTREAM *cmt_vstream ) {
    uint32_t pos = cmt_vstream->size - cmt_vstream->last_event_byte_length;
    if ( sizeof ( uint8_t ) == cmt_vstream->last_event_byte_length ) {
        return cmt_vstream->data[pos];
    } else if ( sizeof ( uint16_t ) == cmt_vstream->last_event_byte_length ) {
        return cmt_vstream_read_u16 ( cmt_vstream->data, pos );
    }
    return cmt_vstream_read_u32 ( cmt_vstream->data, pos );
}


/* Geometrický růst zabraňuje realokaci a kopírování celé datové oblasti
 * při každé hraně. Logická velikost streamu zůstává v poli size. */
static int cmt_vstream_ensure_capacity ( st_CMT_VSTREAM *stream, uint32_t needed ) {
    if ( needed <= stream->capacity ) return EXIT_SUCCESS;

    uint32_t capacity = stream->capacity ? stream->capacity : 64;
    while ( capacity < needed ) {
        if ( capacity > UINT32_MAX / 2 ) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    uint8_t *data = baseui_tools_mem_realloc ( stream->data, capacity );
    if ( !data ) {
        fprintf ( stderr, "%s():%d: Error: can't allocate memory (%u B).\n", __func__, __LINE__, capacity );
        return EXIT_FAILURE;
    }
    stream->data = data;
    stream->capacity = capacity;
    return EXIT_SUCCESS;
}


/*
 * Zapíše délku posledního eventu (od konce datové oblasti).
 * Pokud hodnota přesáhne kapacitu aktuální šířky, automaticky
 * eskaluje na širší formát (uint8→uint16→uint32) a realokuje data.
 */
static inline int cmt_vstream_set_last_event ( st_CMT_VSTREAM *cmt_vstream, uint32_t count_samples ) {
    uint32_t pos = cmt_vstream->size - cmt_vstream->last_event_byte_length;

    if ( sizeof ( uint8_t ) == cmt_vstream->last_event_byte_length ) {

        if ( count_samples < 0xff ) {
            cmt_vstream->data[pos] = (uint8_t) count_samples;
            return EXIT_SUCCESS;
        } else {
            cmt_vstream->data[pos] = 0xff;
            if ( EXIT_SUCCESS != cmt_vstream_ensure_capacity ( cmt_vstream, cmt_vstream->size + sizeof ( uint16_t ) ) ) {
                return EXIT_FAILURE;
            }
            cmt_vstream->last_event_byte_length = sizeof ( uint16_t );
            cmt_vstream->size += sizeof ( uint16_t );
            return cmt_vstream_set_last_event ( cmt_vstream, count_samples );
        }

    } else if ( sizeof ( uint16_t ) == cmt_vstream->last_event_byte_length ) {

        pos = cmt_vstream->size - cmt_vstream->last_event_byte_length;
        if ( count_samples < 0xffff ) {
            cmt_vstream_write_u16 ( cmt_vstream->data, pos, (uint16_t) count_samples );
            return EXIT_SUCCESS;
        } else {
            cmt_vstream_write_u16 ( cmt_vstream->data, pos, (uint16_t) 0xffff );
            if ( EXIT_SUCCESS != cmt_vstream_ensure_capacity ( cmt_vstream, cmt_vstream->size + sizeof ( uint32_t ) ) ) {
                return EXIT_FAILURE;
            }
            cmt_vstream->last_event_byte_length = sizeof ( uint32_t );
            cmt_vstream->size += sizeof ( uint32_t );
            return cmt_vstream_set_last_event ( cmt_vstream, count_samples );
        }

    }

    pos = cmt_vstream->size - cmt_vstream->last_event_byte_length;
    cmt_vstream_write_u32 ( cmt_vstream->data, pos, (uint32_t) count_samples );
    return EXIT_SUCCESS;
}


/*
 * Přidá hodnotu do vstreamu. Pokud je hodnota shodná s poslední zapsanou,
 * prodlouží aktuální event. Jinak vytvoří nový event.
 * count_samples nesmí být 0.
 */
int cmt_vstream_add_value ( st_CMT_VSTREAM *cmt_vstream, int value, uint32_t count_samples ) {

    if ( count_samples == 0 ) {
        fprintf ( stderr, "%s():%d: Error: count_samples can't be 0.\n", __func__, __LINE__ );
        return EXIT_FAILURE;
    }

    if ( ( value & 1 ) == cmt_vstream->last_set_value ) {
        /* stejná hodnota — prodloužíme aktuální event */
        uint32_t last_event = cmt_vstream_get_last_event ( cmt_vstream );

        if ( count_samples >= ( 0xffffffff - last_event ) ) {
            fprintf ( stderr, "%s():%d: Error: event overflow (would exceed 0xFFFFFFFF).\n", __func__, __LINE__ );
            return EXIT_FAILURE;
        }

        cmt_vstream->stream_length += ( (double) count_samples * cmt_vstream->scan_time );
        cmt_vstream->scans += count_samples;

        return cmt_vstream_set_last_event ( cmt_vstream, ( last_event + count_samples ) );
    }

    /* jiná hodnota — nový event */
    if ( EXIT_SUCCESS != cmt_vstream_ensure_capacity ( cmt_vstream, cmt_vstream->size + cmt_vstream->min_byte_length ) ) {
        return EXIT_FAILURE;
    }

    cmt_vstream->last_set_value = value & 1;

    cmt_vstream->last_event_byte_length = cmt_vstream->min_byte_length;
    cmt_vstream->size += cmt_vstream->min_byte_length;

    cmt_vstream->stream_length += ( (double) count_samples * cmt_vstream->scan_time );
    cmt_vstream->scans += count_samples;

    return cmt_vstream_set_last_event ( cmt_vstream, ( count_samples ) );
}


/*
 * Změní polaritu vstreamu. Invertuje start_value, last_set_value
 * a last_read_value. Aktualizuje polarity field.
 * Pokud je aktuální polarita shodná s požadovanou, nedělá nic.
 */
void cmt_vstream_change_polarity ( st_CMT_VSTREAM *stream, en_CMT_STREAM_POLARITY polarity ) {
    if ( stream->polarity == polarity ) return;
    stream->polarity = polarity;
    stream->start_value = ~stream->start_value & 0x01;
    stream->last_set_value = ~stream->last_set_value & 0x01;
    stream->last_read_value = ~stream->last_read_value & 0x01;
}


/*
 * Vrátí velikost datové oblasti v bajtech.
 */
uint32_t cmt_vstream_get_size ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->size;
}


/*
 * Vrátí vzorkovací frekvenci (Hz).
 */
uint32_t cmt_vstream_get_rate ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->rate;
}


/*
 * Vrátí celkovou délku streamu (v sekundách).
 */
double cmt_vstream_get_length ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->stream_length;
}


/*
 * Vrátí celkový počet vzorků.
 */
uint64_t cmt_vstream_get_count_scans ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->scans;
}


/*
 * Vrátí délku jednoho vzorku (v sekundách).
 */
double cmt_vstream_get_scantime ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->scan_time;
}


/*
 * Vrátí aktuální polaritu vstreamu.
 */
en_CMT_STREAM_POLARITY cmt_vstream_get_polarity ( st_CMT_VSTREAM *vstream ) {
    assert ( vstream != NULL );
    return vstream->polarity;
}


/*
 * Vytvoří vstream z WAV souboru (přes handler).
 * Převede sekvenci WAV vzorků na RLE pulzy.
 */
st_CMT_VSTREAM* cmt_vstream_new_from_wav ( st_HANDLER *h, en_CMT_STREAM_POLARITY polarity ) {

    en_WAV_ERROR wav_err;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( h, &wav_err );

    if ( !sh ) {
        fprintf ( stderr, "%s():%d: Error: can't create wav_simple_header: %s\n", __func__, __LINE__, wav_error_string ( wav_err ) );
        return NULL;
    }

    if ( !sh->blocks ) {
        fprintf ( stderr, "%s():%d: Error: WAV is empty.\n", __func__, __LINE__ );
        wav_simple_header_destroy ( sh );
        return NULL;
    }

    en_WAV_POLARITY wav_polarity = ( polarity == CMT_STREAM_POLARITY_NORMAL ) ? WAV_POLARITY_NORMAL : WAV_POLARITY_INVERTED;

    int bit_value = 0;
    int last_bit_value = 0;

    if ( WAV_OK != wav_get_bit_value_of_sample ( h, sh, 0, wav_polarity, &bit_value ) ) {
        fprintf ( stderr, "%s():%d: Error: can't read sample.\n", __func__, __LINE__ );
        wav_simple_header_destroy ( sh );
        return NULL;
    }
    last_bit_value = bit_value;

    st_CMT_VSTREAM* vstream = cmt_vstream_new ( sh->sample_rate, CMT_VSTREAM_BYTELENGTH8, bit_value, polarity );
    if ( !vstream ) {
        fprintf ( stderr, "%s():%d: Error: could not create cmt vstream.\n", __func__, __LINE__ );
        return NULL;
    }

    uint32_t count_scans = 0;
    uint32_t i = 0;
    uint32_t last_saved_scan = 0;
    for ( i = 0; i < sh->blocks; i++ ) {
        if ( WAV_OK != wav_get_bit_value_of_sample ( h, sh, i, wav_polarity, &bit_value ) ) {
            fprintf ( stderr, "%s():%d: Error: can't read sample.\n", __func__, __LINE__ );
            wav_simple_header_destroy ( sh );
            cmt_vstream_destroy ( vstream );
            return NULL;
        }
        count_scans++;

        if ( last_bit_value == bit_value ) continue;

        cmt_vstream_add_value ( vstream, bit_value, count_scans );
        last_bit_value = bit_value;
        count_scans = 0;
        last_saved_scan = i;
    }


    if ( last_saved_scan != ( i - 1 ) ) {
        if ( !count_scans ) count_scans++;
        cmt_vstream_add_value ( vstream, bit_value, count_scans );
    }

    wav_simple_header_destroy ( sh );

    return vstream;
}


/*
 * Vytvoří vstream z bitstreamu (konverze bitstream → vstream).
 *
 * Prochází bitstream bit po bitu a seskupuje po sobě jdoucí stejné hodnoty
 * do RLE eventů vstreamu.
 *
 * Pokud dst_rate == 0 nebo se rovná rate bitstreamu, přepis je 1:1.
 * Při jiném dst_rate se provede resampling — vzorkovací frekvence výsledného
 * vstreamu bude dst_rate.
 */
st_CMT_VSTREAM* cmt_vstream_new_from_bitstream ( st_CMT_BITSTREAM *bitstream, uint32_t dst_rate ) {
    assert ( bitstream != NULL );

    if ( bitstream->scans == 0 ) {
        fprintf ( stderr, "%s():%d: Error: bitstream is empty.\n", __func__, __LINE__ );
        return NULL;
    }

    uint32_t src_rate = bitstream->rate;
    if ( dst_rate == 0 ) dst_rate = src_rate;

    /* počáteční bit */
    int start_value = cmt_bitstream_get_value_on_position ( bitstream, 0 );

    st_CMT_VSTREAM *vstream = cmt_vstream_new ( dst_rate, CMT_VSTREAM_BYTELENGTH8, start_value, bitstream->polarity );
    if ( !vstream ) return NULL;

    if ( dst_rate == src_rate ) {
        /* stejná frekvence — přímý přepis bez resamplingu */
        int current_value = start_value;
        uint32_t run_length = 0;

        uint32_t i;
        for ( i = 0; i < bitstream->scans; i++ ) {
            int bit = cmt_bitstream_get_value_on_position ( bitstream, i );
            if ( bit == current_value ) {
                run_length++;
            } else {
                /* ukončení běhu předchozí hodnoty */
                if ( run_length > 0 ) {
                    cmt_vstream_add_value ( vstream, current_value, run_length );
                }
                current_value = bit;
                run_length = 1;
            }
        }
        /* poslední běh */
        if ( run_length > 0 ) {
            cmt_vstream_add_value ( vstream, current_value, run_length );
        }
    } else {
        /* resampling — převzorkování z src_rate na dst_rate */
        double step = (double) src_rate / dst_rate;
        uint64_t dst_scans = (uint64_t) ( bitstream->scans / step );
        if ( ( dst_scans * step ) < bitstream->scans ) dst_scans++;

        int current_value = start_value;
        uint32_t run_length = 0;

        uint64_t i;
        for ( i = 0; i < dst_scans; i++ ) {
            /* pozice ve zdrojovém bitstreamu */
            uint32_t src_pos = (uint32_t) ( i * step );
            if ( src_pos >= bitstream->scans ) src_pos = bitstream->scans - 1;

            int bit = cmt_bitstream_get_value_on_position ( bitstream, src_pos );
            if ( bit == current_value ) {
                run_length++;
            } else {
                if ( run_length > 0 ) {
                    cmt_vstream_add_value ( vstream, current_value, run_length );
                }
                current_value = bit;
                run_length = 1;
            }
        }
        if ( run_length > 0 ) {
            cmt_vstream_add_value ( vstream, current_value, run_length );
        }
    }

    return vstream;
}
