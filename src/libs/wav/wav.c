/**
 * @file   wav.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.1
 * @brief  Implementace knihovny pro čtení a zápis WAV souborů.
 *
 * Parsování hlavičky:
 *   - Validuje RIFF/WAVE identifikátory
 *   - Prochází chunky sekvenčně (chunk scanning) — přeskakuje neznámé chunky
 *     (LIST, fact, bext, ...) dokud nenajde "fmt " a "data"
 *   - Respektuje RIFF padding (zarovnání chunk_size na sudý bajt)
 *   - Korektně konvertuje endianitu všech polí
 *
 * Čtení vzorků:
 *   - wav_get_sample_float() - normalizovaná hodnota (PCM [-1.0, 1.0],
 *     IEEE float beze změny)
 *   - wav_get_bit_value_of_sample() — 1-bit kvantizace (prahování)
 *   - wav_decode_sample() - jediný převod bajtů vzorku na hodnotu
 *   - Podpora 8/16/24/32/64-bit PCM a IEEE float 32/64 bit (i ve
 *     WAVE_FORMAT_EXTENSIBLE), volitelný výběr kanálu
 *
 * Zápis:
 *   - wav_write() — generuje kompletní WAV soubor (RIFF + fmt + data)
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-10-18 (2.1.0): čtení IEEE float a WAVE_FORMAT_EXTENSIBLE; převod
 *   vzorků sjednocen do wav_decode_sample().
 * - 2026-11-10 (2.1.1): hledání chunku v 64 bitech - velikost chunku z
 *   nedůvěryhodného souboru nepřeteče (dřív nekonečná smyčka).
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
#include <stdlib.h>
#include <string.h>

#include "../baseui_compat.h"

#include "libs/endianity/endianity.h"
#include "libs/generic_driver/generic_driver.h"

#include "wav.h"


/** @brief Textové popisy chybových kódů indexované hodnotou en_WAV_ERROR */
static const char *s_wav_error_strings[] = {
    [WAV_OK]                    = "OK",
    [WAV_ERROR_IO]              = "I/O error",
    [WAV_ERROR_BAD_RIFF]        = "missing RIFF/WAVE identifier",
    [WAV_ERROR_NO_FMT_CHUNK]    = "missing required 'fmt ' chunk",
    [WAV_ERROR_NO_DATA_CHUNK]   = "missing required 'data' chunk",
    [WAV_ERROR_UNSUPPORTED_CODEC] = "unsupported format code (only PCM and IEEE float)",
    [WAV_ERROR_UNSUPPORTED_BPS] = "unsupported bit depth",
    [WAV_ERROR_CORRUPT_HEADER]  = "inconsistent header",
    [WAV_ERROR_ALLOC]           = "memory allocation failed",
    [WAV_ERROR_INVALID_PARAM]   = "invalid parameter",
};

/** @brief Počet položek v poli s_wav_error_strings */
#define WAV_ERROR_COUNT ( sizeof ( s_wav_error_strings ) / sizeof ( s_wav_error_strings[0] ) )


/**
 * @brief Vrátí textový popis chybového kódu.
 *
 * @param err Chybový kód
 * @return Textový popis, nikdy NULL
 */
const char* wav_error_string ( en_WAV_ERROR err ) {
    if ( (unsigned) err < WAV_ERROR_COUNT && s_wav_error_strings[err] != NULL ) {
        return s_wav_error_strings[err];
    }
    return "unknown error";
}


/* ========================================================================
 * Interní pomocné funkce
 * ======================================================================== */

/**
 * @brief Validuje RIFF hlavičku — kontroluje "RIFF" a "WAVE" tagy.
 *
 * @param h         Otevřený handler s WAV daty
 * @param riff_size Výstupní velikost RIFF dat (overall_size z hlavičky)
 * @return WAV_OK při úspěchu, jinak chybový kód
 */
static en_WAV_ERROR wav_check_riff_header ( st_HANDLER *h, uint32_t *riff_size ) {

    uint8_t work_buffer[sizeof ( st_WAV_RIFF_HEADER )];
    st_WAV_RIFF_HEADER *riff_hdr = NULL;

    if ( EXIT_SUCCESS != generic_driver_direct_read ( h, 0, (void**) &riff_hdr, &work_buffer, sizeof ( work_buffer ) ) ) {
        return WAV_ERROR_IO;
    }

    if ( 0 != strncmp ( (char*) riff_hdr->riff_tag, WAV_TAG_RIFF, sizeof ( riff_hdr->riff_tag ) ) ) {
        return WAV_ERROR_BAD_RIFF;
    }

    if ( 0 != strncmp ( (char*) riff_hdr->wave_tag, WAV_TAG_WAVE, sizeof ( riff_hdr->wave_tag ) ) ) {
        return WAV_ERROR_BAD_RIFF;
    }

    *riff_size = endianity_bswap32_LE ( riff_hdr->overall_size );

    return WAV_OK;
}


/**
 * @brief Přečte hlavičku chunku na daném offsetu.
 *
 * @param h              Otevřený handler s WAV daty
 * @param offset         Offset hlavičky chunku v datech
 * @param chunk_tag_out  Výstupní tag chunku (4 bajty)
 * @param chunk_size_out Výstupní velikost dat chunku
 * @return WAV_OK při úspěchu, jinak chybový kód
 */
static en_WAV_ERROR wav_read_chunk_header ( st_HANDLER *h, uint32_t offset,
                                            uint8_t chunk_tag_out[4], uint32_t *chunk_size_out ) {

    uint8_t work_buffer[sizeof ( st_WAV_CHUNK_HEADER )];
    st_WAV_CHUNK_HEADER *chunk_header = NULL;

    if ( EXIT_SUCCESS != generic_driver_direct_read ( h, offset, (void**) &chunk_header, &work_buffer, sizeof ( work_buffer ) ) ) {
        return WAV_ERROR_IO;
    }

    memcpy ( chunk_tag_out, chunk_header->chunk_tag, 4 );
    *chunk_size_out = endianity_bswap32_LE ( chunk_header->chunk_size );

    return WAV_OK;
}


/**
 * @brief Vyhledá chunk s daným tagem v RIFF struktuře (chunk scanning).
 *
 * Začíná od start_offset, prochází chunky sekvenčně. Přeskakuje chunky
 * s neshodným tagem, respektuje RIFF padding (zarovnání chunk_size na sudý bajt).
 *
 * Při nalezení vrátí offset na začátek dat chunku (za hlavičkou) a velikost dat chunku.
 * Hledaný chunk smí sahat za konec souboru (useknutý poslední chunk "data"
 * se přijme, čtení vzorků pak skončí na konci souboru). Přeskakovaný
 * chunk, který sahá za konec souboru, končí hledání chybou: další
 * hlavička chunku se už nepřečte (offset se počítá v 64 bitech, takže
 * velikost chunku z nedůvěryhodného souboru nepřeteče a hledání vždy
 * postoupí dopředu).
 *
 * @param h               Otevřený handler s WAV daty
 * @param start_offset    Počáteční offset pro vyhledávání
 * @param riff_end        Konec RIFF dat (omezení pro chunk scanning)
 * @param wanted_tag      Hledaný tag chunku (4 znaky)
 * @param data_offset_out Výstupní offset dat nalezeného chunku
 * @param chunk_size_out  Výstupní velikost dat nalezeného chunku
 * @return WAV_OK při nalezení; WAV_ERROR_NO_DATA_CHUNK pokud chunk nenalezen nebo
 *         offset přesáhne 32 bitů; chyba čtení hlavičky chunku (také chunk
 *         za koncem souboru)
 */
static en_WAV_ERROR wav_find_chunk ( st_HANDLER *h, uint32_t start_offset, uint64_t riff_end,
                                     const char *wanted_tag, uint32_t *data_offset_out,
                                     uint32_t *chunk_size_out ) {

    uint64_t offset = start_offset;

    while ( offset + sizeof ( st_WAV_CHUNK_HEADER ) <= riff_end && offset + sizeof ( st_WAV_CHUNK_HEADER ) <= UINT32_MAX ) {
        uint8_t tag[4];
        uint32_t size;
        en_WAV_ERROR err = wav_read_chunk_header ( h, ( uint32_t ) offset, tag, &size );
        if ( err != WAV_OK ) return err;

        if ( 0 == strncmp ( (char*) tag, wanted_tag, 4 ) ) {
            *data_offset_out = ( uint32_t ) ( offset + sizeof ( st_WAV_CHUNK_HEADER ) );
            *chunk_size_out = size;
            return WAV_OK;
        }

        /* přeskok na další chunk: hlavička + data + RIFF padding (zarovnání na 2 B), v 64 bitech */
        uint64_t advance = sizeof ( st_WAV_CHUNK_HEADER ) + ( uint64_t ) size;
        if ( size & 1 ) advance++;  /* RIFF padding - lichý chunk se zarovná na sudý bajt */
        offset += advance;
    }

    return WAV_ERROR_NO_DATA_CHUNK; /* výchozí chyba - chunk nenalezen */
}


/* ========================================================================
 * Parsování WAV hlavičky
 * ======================================================================== */

/**
 * @brief Ocas GUID SubFormat (bajty 4..15) společný pro KSDATAFORMAT_SUBTYPE_PCM
 *        a _IEEE_FLOAT, jak je uložen ve WAV (ksmedia.h, DEFINE_WAVEFORMATEX_GUID).
 */
static const uint8_t s_ks_guid_tail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

/**
 * @brief Interní parsování — vyplní caller-owned st_WAV_SIMPLE_HEADER.
 *
 * Validuje RIFF strukturu, vyhledá fmt a data chunky, konvertuje endianitu
 * a sestaví výstupní hlavičku. Přijme PCM, IEEE float a WAVE_FORMAT_EXTENSIBLE
 * se SubFormat PCM nebo IEEE float (fmt aspoň 40 B, cbSize aspoň 22,
 * platné bity <= kontejner). Neznámý SubFormat = WAV_ERROR_UNSUPPORTED_CODEC,
 * poškozené rozšíření = WAV_ERROR_CORRUPT_HEADER.
 *
 * @param h  Otevřený handler s WAV daty
 * @param sh Výstupní hlavička (vlastněná volajícím)
 * @return WAV_OK při úspěchu, jinak chybový kód
 */
static en_WAV_ERROR wav_read_simple_header ( st_HANDLER *h, st_WAV_SIMPLE_HEADER *sh ) {

    memset ( sh, 0x00, sizeof ( st_WAV_SIMPLE_HEADER ) );

    /* 1. Validace RIFF hlavičky */
    uint32_t riff_size = 0;
    en_WAV_ERROR err = wav_check_riff_header ( h, &riff_size );
    if ( err != WAV_OK ) return err;

    /* konec RIFF dat (pro omezení chunk scanningu); 64 bitů - riff_size 0xFFFFFFFF nepřeteče */
    uint64_t riff_end = sizeof ( st_WAV_RIFF_HEADER ) + ( uint64_t ) riff_size;

    /* začátek chunk oblasti (za RIFF hlavičkou) */
    uint32_t chunks_start = sizeof ( st_WAV_RIFF_HEADER );

    /* 2. Najít "fmt " chunk (chunk scanning) */
    uint32_t fmt_data_offset = 0;
    uint32_t fmt_size = 0;
    err = wav_find_chunk ( h, chunks_start, riff_end, WAV_TAG_FMT, &fmt_data_offset, &fmt_size );
    if ( err != WAV_OK ) {
        if ( err == WAV_ERROR_NO_DATA_CHUNK ) return WAV_ERROR_NO_FMT_CHUNK;
        return err;
    }

    /* minimální velikost fmt chunku je 16 bajtů */
    if ( fmt_size < sizeof ( st_WAV_FMT16 ) ) {
        return WAV_ERROR_CORRUPT_HEADER;
    }

    /* 3. Přečíst obsah fmt chunku */
    uint8_t work_buffer[sizeof ( st_WAV_FMT16 )];
    st_WAV_FMT16 *fmt = NULL;

    if ( EXIT_SUCCESS != generic_driver_direct_read ( h, fmt_data_offset, (void**) &fmt, &work_buffer, sizeof ( work_buffer ) ) ) {
        return WAV_ERROR_IO;
    }

    /* konverze endianity PŘED validací */
    uint16_t format_code = endianity_bswap16_LE ( fmt->format_code );
    uint16_t channels = endianity_bswap16_LE ( fmt->channels );
    uint32_t sample_rate = endianity_bswap32_LE ( fmt->sample_rate );
    uint32_t bytes_per_sec = endianity_bswap32_LE ( fmt->bytes_per_sec );
    uint16_t block_size = endianity_bswap16_LE ( fmt->block_size );
    uint16_t bits_per_sample = endianity_bswap16_LE ( fmt->bits_per_sample );

    /*
     * Formátový kód: PCM, IEEE float, nebo EXTENSIBLE se SubFormat PCM/float
     *. format_code zůstává surový, sample_format je skutečné kódování.
     */
    uint16_t sample_format = format_code;
    uint16_t valid_bits = bits_per_sample;
    if ( format_code == WAVE_FORMAT_CODE_EXTENSIBLE ) {
        if ( fmt_size < sizeof ( st_WAV_FMT16 ) + sizeof ( st_WAV_FMT_EXTENSIBLE_TAIL ) ) {
            return WAV_ERROR_CORRUPT_HEADER;
        }
        uint8_t tail_buffer[sizeof ( st_WAV_FMT_EXTENSIBLE_TAIL )];
        st_WAV_FMT_EXTENSIBLE_TAIL *tail = NULL;
        if ( EXIT_SUCCESS != generic_driver_direct_read ( h, fmt_data_offset + sizeof ( st_WAV_FMT16 ), (void**) &tail,
                                                          &tail_buffer, sizeof ( tail_buffer ) ) ) {
            return WAV_ERROR_IO;
        }
        if ( endianity_bswap16_LE ( tail->cb_size ) < sizeof ( st_WAV_FMT_EXTENSIBLE_TAIL ) - sizeof ( uint16_t ) ) {
            return WAV_ERROR_CORRUPT_HEADER;
        }
        uint32_t data1 = (uint32_t) tail->sub_format[0] | ( (uint32_t) tail->sub_format[1] << 8 )
                         | ( (uint32_t) tail->sub_format[2] << 16 ) | ( (uint32_t) tail->sub_format[3] << 24 );
        if ( memcmp ( &tail->sub_format[4], s_ks_guid_tail, sizeof ( s_ks_guid_tail ) ) != 0
             || !( data1 == WAVE_FORMAT_CODE_PCM || data1 == WAVE_FORMAT_CODE_IEEE_FLOAT ) ) {
            return WAV_ERROR_UNSUPPORTED_CODEC;
        }
        sample_format = (uint16_t) data1;
        uint16_t vb = endianity_bswap16_LE ( tail->valid_bits_per_sample );
        if ( vb > bits_per_sample ) return WAV_ERROR_CORRUPT_HEADER;
        if ( vb != 0 ) valid_bits = vb;   /* 0 = celý kontejner */
    } else if ( format_code != WAVE_FORMAT_CODE_PCM && format_code != WAVE_FORMAT_CODE_IEEE_FLOAT ) {
        return WAV_ERROR_UNSUPPORTED_CODEC;
    }

    /* validace bitové hloubky podle kódování: PCM 8/16/24/32/64, float 32/64 */
    if ( sample_format == WAVE_FORMAT_CODE_IEEE_FLOAT ) {
        if ( !( bits_per_sample == 32 || bits_per_sample == 64 ) ) return WAV_ERROR_UNSUPPORTED_BPS;
    } else if ( !( bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 24 ||
                   bits_per_sample == 32 || bits_per_sample == 64 ) ) {
        return WAV_ERROR_UNSUPPORTED_BPS;
    }

    /* ochrana proti dělení nulou — block_size nesmí být 0 */
    if ( block_size == 0 ) {
        return WAV_ERROR_CORRUPT_HEADER;
    }

    /* 4. Najít "data" chunk (chunk scanning — od začátku chunků, ne od konce fmt) */
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    err = wav_find_chunk ( h, chunks_start, riff_end, WAV_TAG_DATA, &data_offset, &data_size );
    if ( err != WAV_OK ) return err;

    /* 5. Sestavit výstupní hlavičku */
    sh->format_code = format_code;
    sh->channels = channels;
    sh->sample_rate = sample_rate;
    sh->bytes_per_sec = bytes_per_sec;
    sh->block_size = block_size;
    sh->bits_per_sample = bits_per_sample;
    sh->sample_format = sample_format;
    sh->valid_bits_per_sample = valid_bits;
    sh->real_data_offset = data_offset;
    sh->data_size = data_size;
    sh->blocks = data_size / block_size;
    sh->count_sec = (double) sh->blocks / sample_rate;

    return WAV_OK;
}


/** @copydoc wav_simple_header_new_from_handler */
st_WAV_SIMPLE_HEADER* wav_simple_header_new_from_handler ( st_HANDLER *h, en_WAV_ERROR *err ) {

    en_WAV_ERROR local_err = WAV_OK;

    if ( !h ) {
        local_err = WAV_ERROR_INVALID_PARAM;
        if ( err ) *err = local_err;
        return NULL;
    }

    st_WAV_SIMPLE_HEADER *sh = baseui_tools_mem_alloc0 ( sizeof ( st_WAV_SIMPLE_HEADER ) );
    if ( !sh ) {
        local_err = WAV_ERROR_ALLOC;
        if ( err ) *err = local_err;
        return NULL;
    }

    local_err = wav_read_simple_header ( h, sh );
    if ( local_err != WAV_OK ) {
        baseui_tools_mem_free ( sh );
        if ( err ) *err = local_err;
        return NULL;
    }

    if ( err ) *err = WAV_OK;
    return sh;
}


/** @copydoc wav_simple_header_destroy */
void wav_simple_header_destroy ( st_WAV_SIMPLE_HEADER *simple_header ) {
    if ( !simple_header ) return;
    baseui_tools_mem_free ( simple_header );
}


/* ========================================================================
 * Čtení vzorků — normalizovaná float hodnota
 * ======================================================================== */

/** @copydoc wav_get_sample_float */
en_WAV_ERROR wav_get_sample_float ( st_HANDLER *h, const st_WAV_SIMPLE_HEADER *sh,
                                    uint32_t sample_position, uint16_t channel,
                                    double *value ) {

    if ( !h || !sh || !value ) return WAV_ERROR_INVALID_PARAM;
    if ( channel >= sh->channels ) return WAV_ERROR_INVALID_PARAM;
    if ( sample_position >= sh->blocks ) return WAV_ERROR_INVALID_PARAM;

    uint16_t sample_bytes = sh->bits_per_sample / 8;

    /* offset vzorku: rámec * block_size + kanál * vzorek */
    uint32_t offset = sh->real_data_offset
                      + ( sample_position * sh->block_size )
                      + ( channel * sample_bytes );

    uint8_t buffer[WAV_MAX_BITS_PER_SAMPLE / 8];

    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buffer, sample_bytes ) ) {
        return WAV_ERROR_IO;
    }

    return wav_decode_sample ( sh, buffer, value );
}


/** @copydoc wav_decode_sample */
en_WAV_ERROR wav_decode_sample ( const st_WAV_SIMPLE_HEADER *sh, const uint8_t *raw, double *value ) {

    if ( !sh || !raw || !value ) return WAV_ERROR_INVALID_PARAM;

    if ( sh->sample_format == WAVE_FORMAT_CODE_IEEE_FLOAT ) {
        double x;
        if ( sh->bits_per_sample == 32 ) {
            uint32_t u;
            float f;
            memcpy ( &u, raw, sizeof ( u ) );
            u = endianity_bswap32_LE ( u );
            memcpy ( &f, &u, sizeof ( f ) );
            x = (double) f;
        } else if ( sh->bits_per_sample == 64 ) {
            uint64_t u;
            memcpy ( &u, raw, sizeof ( u ) );
            u = endianity_bswap64_LE ( u );
            memcpy ( &x, &u, sizeof ( x ) );
        } else {
            return WAV_ERROR_UNSUPPORTED_BPS;
        }
        if ( x != x ) x = 0.0;                      /* NaN */
        else if ( x > 1.0e300 ) x = 1.0;            /* +Inf */
        else if ( x < -1.0e300 ) x = -1.0;          /* -Inf */
        *value = x;
        return WAV_OK;
    }

    switch ( sh->bits_per_sample ) {

        case 8:
            /* 8-bit PCM je unsigned: 0..255, střed 128 */
            *value = ( (double) raw[0] - 128.0 ) / 128.0;
            break;

        case 16:
        {
            int16_t v;
            memcpy ( &v, raw, sizeof ( v ) );
            v = (int16_t) endianity_bswap16_LE ( (uint16_t) v );
            *value = (double) v / 32768.0;
            break;
        }

        case 24:
        {
            /* 24-bit signed little-endian → rozšíření na int32_t */
            int32_t v = (int32_t) raw[0] | ( (int32_t) raw[1] << 8 ) | ( (int32_t) raw[2] << 16 );
            if ( v & 0x800000 ) v |= (int32_t) 0xFF000000;
            *value = (double) v / 8388608.0;  /* 2^23 */
            break;
        }

        case 32:
        {
            int32_t v;
            memcpy ( &v, raw, sizeof ( v ) );
            v = (int32_t) endianity_bswap32_LE ( (uint32_t) v );
            *value = (double) v / 2147483648.0;  /* 2^31 */
            break;
        }

        case 64:
        {
            int64_t v;
            memcpy ( &v, raw, sizeof ( v ) );
            v = (int64_t) endianity_bswap64_LE ( (uint64_t) v );
            *value = (double) v / 9223372036854775808.0;  /* 2^63 */
            break;
        }

        default:
            return WAV_ERROR_UNSUPPORTED_BPS;
    }

    return WAV_OK;
}


/* ========================================================================
 * Čtení vzorků — 1-bit kvantizace
 * ======================================================================== */

/** @copydoc wav_get_bit_value_of_sample */
en_WAV_ERROR wav_get_bit_value_of_sample ( st_HANDLER *h, const st_WAV_SIMPLE_HEADER *sh,
                                           uint32_t sample_position, en_WAV_POLARITY polarity,
                                           int *bit_value ) {

    if ( !bit_value ) return WAV_ERROR_INVALID_PARAM;

    double sample_float = 0.0;
    en_WAV_ERROR err = wav_get_sample_float ( h, sh, sample_position, 0, &sample_float );
    if ( err != WAV_OK ) return err;

    if ( polarity == WAV_POLARITY_NORMAL ) {
        *bit_value = ( sample_float < 0.0 ) ? 1 : 0;
    } else {
        *bit_value = ( sample_float > 0.0 ) ? 1 : 0;
    }

    return WAV_OK;
}


/* ========================================================================
 * Zápis WAV souboru
 * ======================================================================== */

/** @copydoc wav_write */
en_WAV_ERROR wav_write ( st_HANDLER *h, uint32_t sample_rate, uint16_t channels,
                          uint16_t bits_per_sample, const void *data, uint32_t data_size ) {

    if ( !h || !data ) return WAV_ERROR_INVALID_PARAM;
    if ( !( bits_per_sample == 8 || bits_per_sample == 16 ) ) return WAV_ERROR_UNSUPPORTED_BPS;
    if ( channels == 0 || sample_rate == 0 ) return WAV_ERROR_INVALID_PARAM;

    uint16_t block_size = ( bits_per_sample * channels ) / 8;
    uint32_t bytes_per_sec = ( sample_rate * block_size );

    /* celková velikost RIFF (za hlavičkou) */
    uint32_t riff_data_size = sizeof ( st_WAV_CHUNK_HEADER ) + sizeof ( st_WAV_FMT16 )
                            + sizeof ( st_WAV_CHUNK_HEADER ) + data_size;

    uint32_t pos = 0;

    /* RIFF hlavička */
    st_WAV_RIFF_HEADER riff_hdr;
    memcpy ( riff_hdr.riff_tag, WAV_TAG_RIFF, 4 );
    riff_hdr.overall_size = endianity_bswap32_LE ( riff_data_size );
    memcpy ( riff_hdr.wave_tag, WAV_TAG_WAVE, 4 );

    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, &riff_hdr, sizeof ( riff_hdr ) ) ) {
        return WAV_ERROR_IO;
    }
    pos += sizeof ( riff_hdr );

    /* fmt chunk hlavička */
    st_WAV_CHUNK_HEADER fmt_chunk_hdr;
    memcpy ( fmt_chunk_hdr.chunk_tag, WAV_TAG_FMT, 4 );
    fmt_chunk_hdr.chunk_size = endianity_bswap32_LE ( (uint32_t) sizeof ( st_WAV_FMT16 ) );

    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, &fmt_chunk_hdr, sizeof ( fmt_chunk_hdr ) ) ) {
        return WAV_ERROR_IO;
    }
    pos += sizeof ( fmt_chunk_hdr );

    /* fmt data */
    st_WAV_FMT16 fmt;
    fmt.format_code = endianity_bswap16_LE ( WAVE_FORMAT_CODE_PCM );
    fmt.channels = endianity_bswap16_LE ( channels );
    fmt.sample_rate = endianity_bswap32_LE ( sample_rate );
    fmt.bytes_per_sec = endianity_bswap32_LE ( bytes_per_sec );
    fmt.block_size = endianity_bswap16_LE ( block_size );
    fmt.bits_per_sample = endianity_bswap16_LE ( bits_per_sample );

    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, &fmt, sizeof ( fmt ) ) ) {
        return WAV_ERROR_IO;
    }
    pos += sizeof ( fmt );

    /* data chunk hlavička */
    st_WAV_CHUNK_HEADER data_chunk_hdr;
    memcpy ( data_chunk_hdr.chunk_tag, WAV_TAG_DATA, 4 );
    data_chunk_hdr.chunk_size = endianity_bswap32_LE ( data_size );

    if ( EXIT_SUCCESS != generic_driver_write ( h, pos, &data_chunk_hdr, sizeof ( data_chunk_hdr ) ) ) {
        return WAV_ERROR_IO;
    }
    pos += sizeof ( data_chunk_hdr );

    /* vzorková data — bulk zápis */
    if ( data_size > 0 ) {
        if ( EXIT_SUCCESS != generic_driver_write ( h, pos, (void*) data, data_size ) ) {
            return WAV_ERROR_IO;
        }
    }

    return WAV_OK;
}


/* ========================================================================
 * Zpětně kompatibilní wrappery
 * ======================================================================== */

/** @copydoc wav_simple_header_new_from_handler_compat */
st_WAV_SIMPLE_HEADER* wav_simple_header_new_from_handler_compat ( st_HANDLER *h ) {
    return wav_simple_header_new_from_handler ( h, NULL );
}


/** @copydoc wav_get_bit_value_of_sample_compat */
int wav_get_bit_value_of_sample_compat ( st_HANDLER *h, st_WAV_SIMPLE_HEADER *sh,
                                         uint32_t sample_position, en_WAV_POLARITY polarity,
                                         int *bit_value ) {
    en_WAV_ERROR err = wav_get_bit_value_of_sample ( h, sh, sample_position, polarity, bit_value );
    return ( err == WAV_OK ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


const char* wav_version ( void ) {
    return WAV_VERSION;
}
