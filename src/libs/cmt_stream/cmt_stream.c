/**
 * @file   cmt_stream.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.0
 * @brief  Implementace sjednocujícího wrapperu nad bitstream a vstream.
 *
 * Dispatchuje volání na správný typ streamu přes switch/case.
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
#include <assert.h>

#include "cmt_stream.h"

#include "libs/generic_driver/generic_driver.h"
#include "../baseui_compat.h"

#include "../generic_driver/memory_driver.h"

static st_DRIVER *g_driver_realloc = &g_memory_driver_realloc;


/**
 * @brief Uvolní stream (vnitřní data i wrapper strukturu).
 *
 * Bezpečné volání s NULL ukazatelem.
 *
 * @param stream Ukazatel na stream wrapper (NULL je bezpečné)
 */
void cmt_stream_destroy ( st_CMT_STREAM *stream ) {
    if ( !stream ) return;
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            cmt_bitstream_destroy ( stream->str.bitstream );
            break;
        case CMT_STREAM_TYPE_VSTREAM:
            cmt_vstream_destroy ( stream->str.vstream );
            break;
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
            break;
    }
    baseui_tools_mem_free ( stream );
}


/**
 * @brief Vytvoří nový prázdný wrapper pro daný typ streamu.
 *
 * Vnitřní stream (bitstream/vstream) je NULL — musí se nastavit zvlášť.
 *
 * @param type Typ streamu (BITSTREAM nebo VSTREAM)
 * @return Ukazatel na nový wrapper, nebo NULL při selhání
 */
st_CMT_STREAM* cmt_stream_new ( en_CMT_STREAM_TYPE type ) {
    st_CMT_STREAM *stream = NULL;
    switch ( type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
        case CMT_STREAM_TYPE_VSTREAM:
            stream = (st_CMT_STREAM*) baseui_tools_mem_alloc0 ( sizeof ( st_CMT_STREAM ) );
            if ( !stream ) {
                fprintf ( stderr, "%s():%d: Error: can't allocate memory (%zu B)\n", __func__, __LINE__, sizeof ( st_CMT_STREAM ) );
                return NULL;
            }
            stream->stream_type = type;
            break;

        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, type );
    }
    return stream;
}


/**
 * @brief Vytvoří stream z WAV souboru (přes handler).
 *
 * Typ (bitstream/vstream) se zvolí parametrem type.
 *
 * @param h Handler na WAV data
 * @param polarity Polarita signálu
 * @param type Typ cílového streamu (BITSTREAM nebo VSTREAM)
 * @return Ukazatel na nový stream, nebo NULL při selhání
 */
st_CMT_STREAM* cmt_stream_new_from_wav ( st_HANDLER *h, en_CMT_STREAM_POLARITY polarity, en_CMT_STREAM_TYPE type ) {

    st_CMT_STREAM *stream = cmt_stream_new ( type );
    if ( !stream ) {
        return NULL;
    }

    switch ( type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
        {
            st_CMT_BITSTREAM *bitstream = cmt_bitstream_new_from_wav ( h, polarity );
            if ( !bitstream ) {
                fprintf ( stderr, "%s():%d: Error: can't create bitstream from WAV\n", __func__, __LINE__ );
                cmt_stream_destroy ( stream );
                return NULL;
            }
            stream->str.bitstream = bitstream;
            break;
        }

        case CMT_STREAM_TYPE_VSTREAM:
        {
            st_CMT_VSTREAM *vstream = cmt_vstream_new_from_wav ( h, polarity );
            if ( !vstream ) {
                fprintf ( stderr, "%s():%d: Error: can't create vstream from WAV\n", __func__, __LINE__ );
                cmt_stream_destroy ( stream );
                return NULL;
            }
            stream->str.vstream = vstream;
            break;
        }

        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, type );
            cmt_stream_destroy ( stream );
            return NULL;
    }

    return stream;
}


/**
 * @brief Vrátí typ streamu (BITSTREAM nebo VSTREAM).
 * @param stream Ukazatel na stream wrapper
 * @return Typ streamu
 */
en_CMT_STREAM_TYPE cmt_stream_get_stream_type ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    return stream->stream_type;
}


/**
 * @brief Vrátí textový název typu streamu ("bitstream" nebo "vstream").
 * @param stream Ukazatel na stream wrapper
 * @return Řetězec "bitstream", "vstream" nebo "unknown"
 */
const char* cmt_stream_get_stream_type_txt ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return "bitstream";
        case CMT_STREAM_TYPE_VSTREAM:
            return "vstream";
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return "unknown";
}


/**
 * @brief Vrátí velikost datové oblasti streamu v bajtech.
 * @param stream Ukazatel na stream wrapper
 * @return Velikost v bajtech
 */
uint32_t cmt_stream_get_size ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return cmt_bitstream_get_size ( stream->str.bitstream );
        case CMT_STREAM_TYPE_VSTREAM:
            return cmt_vstream_get_size ( stream->str.vstream );
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return 0;
}


/**
 * @brief Vrátí vzorkovací frekvenci streamu (Hz).
 * @param stream Ukazatel na stream wrapper
 * @return Vzorkovací frekvence v Hz
 */
uint32_t cmt_stream_get_rate ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return cmt_bitstream_get_rate ( stream->str.bitstream );
        case CMT_STREAM_TYPE_VSTREAM:
            return cmt_vstream_get_rate ( stream->str.vstream );
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return 0;
}


/**
 * @brief Vrátí celkovou délku streamu (v sekundách).
 * @param stream Ukazatel na stream wrapper
 * @return Délka v sekundách
 */
double cmt_stream_get_length ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return cmt_bitstream_get_length ( stream->str.bitstream );
        case CMT_STREAM_TYPE_VSTREAM:
            return cmt_vstream_get_length ( stream->str.vstream );
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return 0;
}


/**
 * @brief Vrátí celkový počet vzorků.
 * @param stream Ukazatel na stream wrapper
 * @return Počet vzorků
 */
uint64_t cmt_stream_get_count_scans ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return cmt_bitstream_get_count_scans ( stream->str.bitstream );
        case CMT_STREAM_TYPE_VSTREAM:
            return cmt_vstream_get_count_scans ( stream->str.vstream );
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return 0;
}


/**
 * @brief Vrátí délku jednoho vzorku (v sekundách).
 * @param stream Ukazatel na stream wrapper
 * @return Délka jednoho vzorku v sekundách (1/rate)
 */
double cmt_stream_get_scantime ( st_CMT_STREAM *stream ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            return cmt_bitstream_get_scantime ( stream->str.bitstream );
        case CMT_STREAM_TYPE_VSTREAM:
            return cmt_vstream_get_scantime ( stream->str.vstream );
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
    return 0;
}


/**
 * @brief Nastaví polaritu streamu. Deleguje na příslušnou change_polarity funkci.
 * @param stream Ukazatel na stream wrapper
 * @param polarity Nová polarita signálu
 */
void cmt_stream_set_polarity ( st_CMT_STREAM *stream, en_CMT_STREAM_POLARITY polarity ) {
    assert ( stream != NULL );
    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            cmt_bitstream_change_polarity ( stream->str.bitstream, polarity );
            break;
        case CMT_STREAM_TYPE_VSTREAM:
            cmt_vstream_change_polarity ( stream->str.vstream, polarity );
            break;
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }
}


/**
 * @brief Uloží stream jako WAV soubor.
 *
 * Vstream se nejdřív převede na bitstream (s danou vzorkovací frekvencí),
 * bitstream se exportuje přímo.
 *
 * @param stream Ukazatel na stream wrapper
 * @param rate Vzorkovací frekvence výstupního WAV (Hz)
 * @param filename Cesta k výstupnímu souboru
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
 */
int cmt_stream_save_wav ( st_CMT_STREAM *stream, uint32_t rate, char *filename ) {
    assert ( stream != NULL );

    st_CMT_BITSTREAM *bitstream = NULL;
    st_CMT_BITSTREAM *bitstream_new = NULL;

    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
            bitstream = stream->str.bitstream;
            break;
        case CMT_STREAM_TYPE_VSTREAM:
            bitstream_new = cmt_bitstream_new_from_vstream ( stream->str.vstream, rate );
            bitstream = bitstream_new;
            break;
        default:
            fprintf ( stderr, "%s():%d: Error: unknown stream type '%d'\n", __func__, __LINE__, stream->stream_type );
    }

    if ( !bitstream ) return EXIT_FAILURE;

    st_HANDLER *hwav = generic_driver_open_memory ( NULL, g_driver_realloc, 1 );
    if ( !hwav ) {
        fprintf ( stderr, "%s():%d: Error: can't open memory handler\n", __func__, __LINE__ );
        if ( bitstream_new ) cmt_bitstream_destroy ( bitstream_new );
        return EXIT_FAILURE;
    }

    generic_driver_set_handler_readonly_status ( hwav, 0 );
    hwav->spec.memspec.swelling_enabled = 1;

    if ( EXIT_FAILURE == cmt_bitstream_create_wav ( hwav, bitstream ) ) {
        fprintf ( stderr, "%s():%d: Error: can't create WAV from bitstream\n", __func__, __LINE__ );
        generic_driver_close ( hwav );
        if ( bitstream_new ) cmt_bitstream_destroy ( bitstream_new );
        return EXIT_FAILURE;
    }

    uint32_t size = hwav->spec.memspec.size;

    char buff[100];
    if ( size < 1024 ) {
        snprintf ( buff, sizeof ( buff ), "%d B", size );
    } else if ( size < ( 1024 * 1024 ) ) {
        snprintf ( buff, sizeof ( buff ), "%0.2f kB", ( (float) size / 1024 ) );
    } else {
        snprintf ( buff, sizeof ( buff ), "%0.2f MB", ( (float) size / ( 1024 * 1024 ) ) );
    }

    int ret = EXIT_SUCCESS;
    printf ( "Save WAV (%s): %s\n", buff, filename );
    if ( EXIT_FAILURE == generic_driver_save_memory ( hwav, filename ) ) {
        fprintf ( stderr, "%s():%d: Error: can't write WAV file\n", __func__, __LINE__ );
        ret = EXIT_FAILURE;
    }

    generic_driver_close ( hwav );
    if ( bitstream_new ) cmt_bitstream_destroy ( bitstream_new );
    return ret;
}


const char* cmt_stream_version ( void ) {
    return CMT_STREAM_VERSION;
}
