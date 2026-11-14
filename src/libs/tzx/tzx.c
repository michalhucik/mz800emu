/**
 * @file   tzx.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Implementace TZX knihovny - parsovani bloku a generovani CMT vstreamu.
 *
 * Implementuje parsovani vsech audio bloku TZX v1.20 (0x10-0x15, 0x18, 0x20)
 * a generovani CMT vstreamu z naparsovanych dat (0x18 jen s kompresí RLE),
 * vytváření bloků 0x15, 0x18 a informačních bloků.
 *
 * Generovani pouziva vstream (RLE kodovane pulzy) pro maximalni presnost.
 * Kazdy pulz (half-period) je preveden z T-states na pocet vzorku
 * pri dane vzorkovaci frekvenci a CPU taktu.
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
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#include "../cmt_stream/cmt_stream.h"
#include "../generic_driver/generic_driver.h"
#include "../endianity/endianity.h"

#include "tzx.h"


/* ========================================================================= */
/*  Alokatory                                                                */
/* ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* tzx_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* tzx_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  tzx_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_TZX_ALLOCATOR g_tzx_default_allocator = {
    tzx_default_alloc,
    tzx_default_alloc0,
    tzx_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_TZX_ALLOCATOR *g_tzx_allocator = &g_tzx_default_allocator;


/**
 * @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL.
 * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset.
 */
void tzx_set_allocator ( const st_TZX_ALLOCATOR *allocator ) {
    g_tzx_allocator = allocator ? allocator : &g_tzx_default_allocator;
}


/* ========================================================================= */
/*  Error callback                                                           */
/* ========================================================================= */

/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void tzx_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static tzx_error_cb g_tzx_error_cb = tzx_default_error_cb;


/**
 * @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL.
 * @param cb Ukazatel na callback funkci, nebo NULL pro reset.
 */
void tzx_set_error_callback ( tzx_error_cb cb ) {
    g_tzx_error_cb = cb ? cb : tzx_default_error_cb;
}


/* ========================================================================= */
/*  Konfigurace                                                              */
/* ========================================================================= */

/**
 * @brief Inicializuje konfiguraci TZX generatoru na vychozi hodnoty.
 * @param config Ukazatel na konfiguraci k inicializaci.
 */
void tzx_config_init ( st_TZX_CONFIG *config ) {
    if ( !config ) return;
    config->sample_rate = CMTSTREAM_DEFAULT_RATE;
    config->stream_type = CMT_STREAM_TYPE_VSTREAM;
    config->cpu_clock = TZX_DEFAULT_CPU_CLOCK;
}


/* ========================================================================= */
/*  Chybove retezce                                                          */
/* ========================================================================= */

/**
 * @brief Vrati textovy popis chyboveho kodu en_TZX_ERROR.
 * @param err Chybovy kod.
 * @return Ukazatel na staticky retezec s popisem chyby.
 */
const char* tzx_error_string ( en_TZX_ERROR err ) {
    switch ( err ) {
        case TZX_OK:                      return "OK";
        case TZX_ERROR_NULL_INPUT:        return "NULL input parameter";
        case TZX_ERROR_INVALID_BLOCK:     return "Invalid block data";
        case TZX_ERROR_UNSUPPORTED:       return "Unsupported block type";
        case TZX_ERROR_STREAM_CREATE:     return "Failed to create CMT stream";
        case TZX_ERROR_ALLOC:             return "Memory allocation failed";
        case TZX_ERROR_IO:                return "I/O error";
        case TZX_ERROR_INVALID_SIGNATURE: return "Invalid file signature (expected ZXTape! or TapeMZ!)";
        case TZX_ERROR_INVALID_VERSION:   return "Unsupported file version";
        case TZX_ERROR_UNEXPECTED_EOF:    return "Unexpected end of file";
        case TZX_ERROR_UNKNOWN_BLOCK:     return "Unknown block ID";
        default:                          return "Unknown TZX error";
    }
}


/* ========================================================================= */
/*  Pomocne funkce                                                           */
/* ========================================================================= */

/** @brief Precte little-endian WORD z raw dat. */
static inline uint16_t read_le16 ( const uint8_t *p ) {
    return (uint16_t) p[0] | ( (uint16_t) p[1] << 8 );
}

/** @brief Precte little-endian 3-bajtove cislo z raw dat. */
static inline uint32_t read_le24 ( const uint8_t *p ) {
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 );
}

/** @brief Precte little-endian DWORD z raw dat. */
static inline uint32_t read_le32 ( const uint8_t *p ) {
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}


/**
 * @brief Vytvoří nový prázdný vstream pro generování.
 * @param config Konfigurace generátoru.
 * @param start Počáteční hodnota vstreamu (úroveň prvního pulzu, 0|1).
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* tzx_create_vstream ( const st_TZX_CONFIG *config, int start ) {
    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    return cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, start ? 1 : 0, CMT_STREAM_POLARITY_NORMAL );
}


/**
 * @brief Normalizuje vstupní úroveň pro pauzu: 0, 1 nebo TZX_LEVEL_AFTER_PAUSE.
 * @param level Úroveň od volajícího (jiná nenulová hodnota než 1 a
 *        TZX_LEVEL_AFTER_PAUSE = 1).
 * @return 0, 1 nebo TZX_LEVEL_AFTER_PAUSE.
 */
static inline uint8_t tzx_level_norm ( uint8_t level ) {
    return ( level == TZX_LEVEL_AFTER_PAUSE ) ? TZX_LEVEL_AFTER_PAUSE : ( level ? 1 : 0 );
}


/**
 * @brief Přidá jeden pulz (půlvlnu) na aktuální úrovni a úroveň otočí.
 *
 * Převede T-states na počet vzorků (nejméně 1) a zapíše je do vstreamu.
 *
 * @param vs Vstream.
 * @param tstates Délka pulzu v T-states.
 * @param[in,out] level Aktuální úroveň (0|1); po zápisu se otočí.
 * @param config Konfigurace generátoru.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě zápisu.
 */
static inline int tzx_add_pulse ( st_CMT_VSTREAM *vs, uint32_t tstates, uint8_t *level, const st_TZX_CONFIG *config ) {
    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    uint32_t clock = config->cpu_clock ? config->cpu_clock : TZX_DEFAULT_CPU_CLOCK;
    uint32_t samples = (uint32_t) ( (double) tstates * (double) rate / (double) clock + 0.5 );
    if ( samples == 0 ) samples = 1;
    if ( EXIT_FAILURE == cmt_vstream_add_value ( vs, *level, samples ) ) return EXIT_FAILURE;
    *level ^= 1;
    return EXIT_SUCCESS;
}


/**
 * @brief Přidá datové bajty do vstreamu s danými délkami pulzů.
 *
 * Každý bajt se kóduje od MSb k LSb, každý bit jsou dva pulzy stejné
 * délky (první na aktuální úrovni, druhý na opačné).
 *
 * @param vs Vstream.
 * @param data Data k zakódování.
 * @param data_length Počet bajtů.
 * @param used_bits_last Použité bity v posledním bajtu (1-8).
 * @param zero_tstates Délka půlvlny bitu ZERO v T-states.
 * @param one_tstates Délka půlvlny bitu ONE v T-states.
 * @param[in,out] level Aktuální úroveň (0|1).
 * @param config Konfigurace generátoru.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int tzx_add_data_bits ( st_CMT_VSTREAM *vs, const uint8_t *data, uint32_t data_length,
                                uint8_t used_bits_last, uint16_t zero_tstates, uint16_t one_tstates,
                                uint8_t *level, const st_TZX_CONFIG *config ) {

    if ( data_length == 0 ) return EXIT_SUCCESS;

    for ( uint32_t i = 0; i < data_length; i++ ) {
        uint8_t byte = data[i];
        int bits = ( i == data_length - 1 ) ? used_bits_last : 8;

        for ( int b = 0; b < bits; b++ ) {
            uint16_t pulse_len = ( byte & 0x80 ) ? one_tstates : zero_tstates;

            /* celá perioda = 2 půlvlny */
            if ( EXIT_FAILURE == tzx_add_pulse ( vs, pulse_len, level, config ) ) return EXIT_FAILURE;
            if ( EXIT_FAILURE == tzx_add_pulse ( vs, pulse_len, level, config ) ) return EXIT_FAILURE;

            byte <<= 1;
        }
    }

    return EXIT_SUCCESS;
}


/**
 * @brief Přidá pauzu (ticho) do vstreamu podle TZX 1.20.
 *
 * Při vysoké aktuální úrovni (poslední pulz byl nízký) nejdřív 1 ms
 * vysoko, aby se dokončila hrana posledního pulzu, zbytek nízko. Při
 * nízké úrovni a po pauze (TZX_LEVEL_AFTER_PAUSE) je celá pauza nízko.
 * Po pauze je úroveň TZX_LEVEL_AFTER_PAUSE: další pulz vytvoří hranu.
 * Pause 0 nic nezapíše a úroveň nemění.
 *
 * @param vs Vstream.
 * @param pause_ms Doba pauzy v milisekundách.
 * @param[in,out] level Aktuální úroveň (0|1|TZX_LEVEL_AFTER_PAUSE); po pauze > 0
 *        je TZX_LEVEL_AFTER_PAUSE.
 * @param config Konfigurace generátoru.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static int tzx_add_pause_to_vstream ( st_CMT_VSTREAM *vs, uint16_t pause_ms, uint8_t *level, const st_TZX_CONFIG *config ) {
    if ( pause_ms == 0 ) return EXIT_SUCCESS;   /* Pause 0 úroveň nemění (TZX 1.20) */

    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    uint32_t total = (uint32_t) ( (double) pause_ms * rate / 1000.0 + 0.5 );
    if ( total == 0 ) total = 1;
    uint32_t high = 0;
    if ( *level == 1 ) {
        /* aktuální úroveň vysoká = poslední pulz byl nízký: 1 ms vysoko dokončí jeho hranu */
        high = rate / 1000;
        if ( high == 0 ) high = 1;
        if ( high > total ) high = total;
        if ( EXIT_FAILURE == cmt_vstream_add_value_long ( vs, 1, high ) ) return EXIT_FAILURE;
    }
    /* pauza LOW splyne s předchozím během LOW: součet smí přesáhnout jeden event */
    if ( total > high && EXIT_FAILURE == cmt_vstream_add_value_long ( vs, 0, total - high ) ) return EXIT_FAILURE;
    *level = TZX_LEVEL_AFTER_PAUSE;   /* signál nízko, další pulz vytvoří hranu (jako ZX) */
    return EXIT_SUCCESS;
}


/* ========================================================================= */
/*  Parsovani bloku                                                          */
/* ========================================================================= */

en_TZX_ERROR tzx_parse_standard_speed ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_STANDARD_SPEED *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 4 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x10 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->pause_ms = read_le16 ( raw_data );
    result->data_length = read_le16 ( raw_data + 2 );
    result->data = (uint8_t*) raw_data + 4;

    if ( raw_length < (uint32_t)( 4 + result->data_length ) ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x10 data truncated: have %u, need %u bytes\n", raw_length, 4 + result->data_length );
        return TZX_ERROR_INVALID_BLOCK;
    }

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_turbo_speed ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_TURBO_SPEED *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 0x12 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x11 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->pilot_pulse = read_le16 ( raw_data + 0x00 );
    result->sync1_pulse = read_le16 ( raw_data + 0x02 );
    result->sync2_pulse = read_le16 ( raw_data + 0x04 );
    result->zero_pulse  = read_le16 ( raw_data + 0x06 );
    result->one_pulse   = read_le16 ( raw_data + 0x08 );
    result->pilot_count = read_le16 ( raw_data + 0x0A );
    result->used_bits   = raw_data[0x0C];
    result->pause_ms    = read_le16 ( raw_data + 0x0D );
    result->data_length = read_le24 ( raw_data + 0x0F );
    result->data = (uint8_t*) raw_data + 0x12;

    if ( raw_length < 0x12 + result->data_length ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x11 data truncated: have %u, need %u bytes\n", raw_length, 0x12 + result->data_length );
        return TZX_ERROR_INVALID_BLOCK;
    }
    if ( result->used_bits == 0 ) result->used_bits = 8;

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_pure_tone ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PURE_TONE *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 4 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x12 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->pulse_length = read_le16 ( raw_data );
    result->pulse_count  = read_le16 ( raw_data + 2 );

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_pulse_sequence ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PULSE_SEQUENCE *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 1 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x13 too short\n" ); return TZX_ERROR_INVALID_BLOCK; }

    result->pulse_count = raw_data[0];
    result->pulses = (uint16_t*) ( raw_data + 1 );

    if ( raw_length < (uint32_t)( 1 + result->pulse_count * 2 ) ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x13 data truncated\n" );
        return TZX_ERROR_INVALID_BLOCK;
    }

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_pure_data ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PURE_DATA *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 0x0A ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x14 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->zero_pulse  = read_le16 ( raw_data + 0x00 );
    result->one_pulse   = read_le16 ( raw_data + 0x02 );
    result->used_bits   = raw_data[0x04];
    result->pause_ms    = read_le16 ( raw_data + 0x05 );
    result->data_length = read_le24 ( raw_data + 0x07 );
    result->data = (uint8_t*) raw_data + 0x0A;

    if ( raw_length < 0x0A + result->data_length ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x14 data truncated\n" );
        return TZX_ERROR_INVALID_BLOCK;
    }
    if ( result->used_bits == 0 ) result->used_bits = 8;

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_direct_recording ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_DIRECT_RECORDING *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 0x08 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x15 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->tstates_per_sample = read_le16 ( raw_data + 0x00 );
    result->pause_ms           = read_le16 ( raw_data + 0x02 );
    result->used_bits           = raw_data[0x04];
    result->data_length         = read_le24 ( raw_data + 0x05 );
    result->data = (uint8_t*) raw_data + 0x08;

    if ( raw_length < 0x08 + result->data_length ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x15 data truncated\n" );
        return TZX_ERROR_INVALID_BLOCK;
    }
    if ( result->used_bits == 0 ) result->used_bits = 8;

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_csw_recording ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_CSW_RECORDING *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 0x0E ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x18 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    /* prvni 4 bajty jsou DWORD delka bloku (uz zpracovano parserem vys) */
    uint32_t block_len = read_le32 ( raw_data );

    /* DWORD délka pokrývá 10 B hlavičky + data; data musí ležet v raw_length */
    if ( block_len < 10 || block_len - 10 > raw_length - 0x0E ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18 length %u inconsistent with raw data (%u bytes)\n", block_len, raw_length );
        return TZX_ERROR_INVALID_BLOCK;
    }

    result->pause_ms         = read_le16 ( raw_data + 0x04 );
    result->sample_rate      = read_le24 ( raw_data + 0x06 );
    result->compression_type = raw_data[0x09];
    result->pulse_count      = read_le32 ( raw_data + 0x0A );
    result->data = (uint8_t*) raw_data + 0x0E;
    result->data_length = block_len - 10;

    return TZX_OK;
}


en_TZX_ERROR tzx_parse_pause ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PAUSE *result ) {
    if ( !raw_data || !result ) { g_tzx_error_cb ( __func__, __LINE__, "NULL input\n" ); return TZX_ERROR_NULL_INPUT; }
    if ( raw_length < 2 ) { g_tzx_error_cb ( __func__, __LINE__, "Block 0x20 too short: %u bytes\n", raw_length ); return TZX_ERROR_INVALID_BLOCK; }

    result->pause_ms = read_le16 ( raw_data );

    return TZX_OK;
}


/* ========================================================================= */
/*  Generovani audio streamu                                                 */
/* ========================================================================= */

/**
 * @brief Generátor bloku 0x10 s úrovní signálu.
 *
 * Pilot má TZX_PILOT_COUNT_HEADER/DATA pulzů podle flag bajtu (jeden pulz
 * na iteraci), pak sync, data a pauza.
 *
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_standard_speed ( const st_TZX_STANDARD_SPEED *block, const st_TZX_CONFIG *config,
                                            uint8_t *level, en_TZX_ERROR *err ) {

    uint8_t lv = *level ? 1 : 0;
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    /* počet pulzů pilotu podle flag bajtu */
    uint16_t pilot_count = ( block->data_length > 0 && block->data[0] < 128 )
        ? TZX_PILOT_COUNT_HEADER : TZX_PILOT_COUNT_DATA;

    for ( uint16_t i = 0; i < pilot_count; i++ ) {
        if ( EXIT_FAILURE == tzx_add_pulse ( vs, TZX_PILOT_PULSE_LENGTH, &lv, config ) ) goto fail;
    }

    if ( EXIT_FAILURE == tzx_add_pulse ( vs, TZX_SYNC1_PULSE_LENGTH, &lv, config ) ) goto fail;
    if ( EXIT_FAILURE == tzx_add_pulse ( vs, TZX_SYNC2_PULSE_LENGTH, &lv, config ) ) goto fail;

    if ( EXIT_FAILURE == tzx_add_data_bits ( vs, block->data, block->data_length, 8,
                                              TZX_ZERO_BIT_PULSE_LENGTH, TZX_ONE_BIT_PULSE_LENGTH, &lv, config ) ) goto fail;

    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, block->pause_ms, &lv, config ) ) goto fail;

    *level = lv;
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor bloku 0x11 s úrovní signálu (pilot = pilot_count pulzů).
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_turbo_speed ( const st_TZX_TURBO_SPEED *block, const st_TZX_CONFIG *config,
                                         uint8_t *level, en_TZX_ERROR *err ) {

    uint8_t lv = *level ? 1 : 0;
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    for ( uint16_t i = 0; i < block->pilot_count; i++ ) {
        if ( EXIT_FAILURE == tzx_add_pulse ( vs, block->pilot_pulse, &lv, config ) ) goto fail;
    }

    if ( EXIT_FAILURE == tzx_add_pulse ( vs, block->sync1_pulse, &lv, config ) ) goto fail;
    if ( EXIT_FAILURE == tzx_add_pulse ( vs, block->sync2_pulse, &lv, config ) ) goto fail;

    if ( EXIT_FAILURE == tzx_add_data_bits ( vs, block->data, block->data_length, block->used_bits,
                                              block->zero_pulse, block->one_pulse, &lv, config ) ) goto fail;

    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, block->pause_ms, &lv, config ) ) goto fail;

    *level = lv;
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor bloku 0x12 s úrovní signálu.
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě. Při 0 pulzech vrátí vstream s 0 vzorky.
 */
static st_CMT_VSTREAM* gen_pure_tone ( const st_TZX_PURE_TONE *block, const st_TZX_CONFIG *config,
                                       uint8_t *level, en_TZX_ERROR *err ) {

    uint8_t lv = *level ? 1 : 0;
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    for ( uint16_t i = 0; i < block->pulse_count; i++ ) {
        if ( EXIT_FAILURE == tzx_add_pulse ( vs, block->pulse_length, &lv, config ) ) goto fail;
    }

    if ( block->pulse_count ) *level = lv;   /* blok bez pulzů úroveň nemění */
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor bloku 0x13 s úrovní signálu.
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_pulse_sequence ( const st_TZX_PULSE_SEQUENCE *block, const st_TZX_CONFIG *config,
                                            uint8_t *level, en_TZX_ERROR *err ) {

    uint8_t lv = *level ? 1 : 0;
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    for ( uint8_t i = 0; i < block->pulse_count; i++ ) {
        uint16_t pulse_len = read_le16 ( (const uint8_t*) &block->pulses[i] );
        if ( EXIT_FAILURE == tzx_add_pulse ( vs, pulse_len, &lv, config ) ) goto fail;
    }

    if ( block->pulse_count ) *level = lv;   /* blok bez pulzů úroveň nemění */
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor bloku 0x14 s úrovní signálu (data bez pilotu a sync, pak pauza).
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_pure_data ( const st_TZX_PURE_DATA *block, const st_TZX_CONFIG *config,
                                       uint8_t *level, en_TZX_ERROR *err ) {

    /* bez dat žádný pulz: pauza od úrovně volajícího (po pauze bez 1 ms vysoko) */
    uint8_t lv = block->data_length ? ( *level ? 1 : 0 ) : tzx_level_norm ( *level );
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv == 1 );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    if ( EXIT_FAILURE == tzx_add_data_bits ( vs, block->data, block->data_length, block->used_bits,
                                              block->zero_pulse, block->one_pulse, &lv, config ) ) goto fail;

    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, block->pause_ms, &lv, config ) ) goto fail;

    if ( block->data_length || block->pause_ms ) *level = lv;   /* blok bez signálu úroveň nemění */
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor bloku 0x15 s úrovní signálu.
 *
 * Úrovně vzorků určují bity dat (0 = nízká, 1 = vysoká, MSb první),
 * sousední stejné bity se slučují do jednoho pulzu. Po posledním bitu je
 * aktuální úroveň jeho opakem (bez dat se nemění), pak pauza.
 *
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_direct_recording ( const st_TZX_DIRECT_RECORDING *block, const st_TZX_CONFIG *config,
                                              uint8_t *level, en_TZX_ERROR *err ) {

    /* mez celého bloku (CMT_VSTREAM_MAX_BLOCK_SECONDS) předem, v taktech CPU:
     * bitů < 2^35, tstates <= 65535, pauza <= 65535 ms - bez přetečení */
    {
        uint64_t clk = config->cpu_clock ? config->cpu_clock : TZX_DEFAULT_CPU_CLOCK;
        uint64_t bits = block->data_length ? ( uint64_t ) ( block->data_length - 1 ) * 8u + block->used_bits : 0;
        uint64_t t = bits * block->tstates_per_sample + ( ( uint64_t ) block->pause_ms * clk + 999u ) / 1000u;
        if ( t > ( uint64_t ) CMT_VSTREAM_MAX_BLOCK_SECONDS * clk ) {
            g_tzx_error_cb ( __func__, __LINE__, "Block 0x15: signal longer than %u s\n", CMT_VSTREAM_MAX_BLOCK_SECONDS );
            *err = TZX_ERROR_UNSUPPORTED;
            return NULL;
        }
    }

    uint8_t lv = tzx_level_norm ( *level );   /* bez dat zůstane (pauza od úrovně volajícího) */
    /* vstream začíná úrovní prvního vzorku (jinak by vznikl prázdný první event) */
    int start = ( block->data_length > 0 ) ? ( ( block->data[0] & 0x80 ) ? 1 : 0 ) : ( lv == 1 );
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, start );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    uint32_t clock = config->cpu_clock ? config->cpu_clock : TZX_DEFAULT_CPU_CLOCK;
    uint32_t samples_per_bit = (uint32_t) ( (double) block->tstates_per_sample * rate / clock + 0.5 );
    if ( samples_per_bit == 0 ) samples_per_bit = 1;

    int current_level = -1;
    uint64_t current_samples = 0;   /* dlouhý běh stejných vzorků smí přesáhnout 2^32 */

    for ( uint32_t i = 0; i < block->data_length; i++ ) {
        uint8_t byte = block->data[i];
        int bits = ( i == block->data_length - 1 ) ? block->used_bits : 8;

        for ( int b = 0; b < bits; b++ ) {
            int bit_level = ( byte & 0x80 ) ? 1 : 0;

            if ( bit_level == current_level ) {
                current_samples += samples_per_bit;
            } else {
                /* zapsat předchozí pulz */
                if ( current_level >= 0 && current_samples > 0 ) {
                    if ( EXIT_FAILURE == cmt_vstream_add_value_long ( vs, current_level, current_samples ) ) goto fail;
                }
                current_level = bit_level;
                current_samples = samples_per_bit;
            }

            byte <<= 1;
        }
    }

    /* poslední pulz; další blok začne hranou (opak posledního vzorku) */
    if ( current_level >= 0 && current_samples > 0 ) {
        if ( EXIT_FAILURE == cmt_vstream_add_value_long ( vs, current_level, current_samples ) ) goto fail;
        lv = (uint8_t) !current_level;
    }

    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, block->pause_ms, &lv, config ) ) goto fail;

    *level = lv;
    *err = TZX_OK;
    return vs;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Převede pozici ve vzorcích záznamu na zaokrouhlenou pozici ve vzorcích výstupu.
 *
 * round ( pos * dst_rate / src_rate ) v celých číslech; pos se dělí
 * src_rate předem, aby součin nepřetekl (zbytek < 2^24, dst_rate < 2^32).
 *
 * @param pos Pozice ve vzorcích při src_rate.
 * @param src_rate Frekvence záznamu (> 0, nejvýš 24 bitů).
 * @param dst_rate Výstupní frekvence.
 * @return Pozice ve výstupních vzorcích.
 */
static inline uint64_t tzx_csw_scale ( uint64_t pos, uint32_t src_rate, uint32_t dst_rate ) {
    uint64_t q = pos / src_rate;
    uint64_t r = pos % src_rate;
    return q * dst_rate + ( r * dst_rate + src_rate / 2 ) / src_rate;
}


/* Kontrakt viz tzx.h (tzx_csw_recording_check). */
en_TZX_ERROR tzx_csw_recording_check ( const st_TZX_CSW_RECORDING *block ) {
    if ( !block ) return TZX_ERROR_NULL_INPUT;
    if ( block->compression_type == TZX_CSW_COMPRESSION_ZRLE ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: Z-RLE compression is not supported\n" );
        return TZX_ERROR_UNSUPPORTED;
    }
    if ( block->compression_type != TZX_CSW_COMPRESSION_RLE ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: unknown compression type 0x%02X\n", block->compression_type );
        return TZX_ERROR_INVALID_BLOCK;
    }
    if ( block->sample_rate == 0 || block->sample_rate > 0xFFFFFFu ) {
        /* v souboru je frekvence 24bitová; struktura od volajícího smí mít víc */
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: sample rate %u outside 1-16777215\n", block->sample_rate );
        return TZX_ERROR_INVALID_BLOCK;
    }
    if ( block->data_length > 0 && !block->data ) return TZX_ERROR_NULL_INPUT;

    /* průchod daty: struktura a celková délka ve vzorcích záznamu
     * (pulzů nejvýš 2^32 po nejvýš 2^32 vzorcích: součet se do 64 bitů vejde) */
    const uint8_t *d = block->data;
    uint32_t pos = 0;
    uint64_t total = 0;
    for ( uint32_t i = 0; i < block->pulse_count; i++ ) {
        if ( pos >= block->data_length ) {
            g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: data end after %u of %u pulses\n", i, block->pulse_count );
            return TZX_ERROR_INVALID_BLOCK;
        }
        uint32_t len = d[pos++];
        if ( len == 0 ) {
            if ( block->data_length - pos < 4 ) {
                g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: truncated 32-bit pulse length (pulse %u)\n", i );
                return TZX_ERROR_INVALID_BLOCK;
            }
            len = read_le32 ( d + pos );
            pos += 4;
            if ( len == 0 ) {
                g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: zero pulse length (pulse %u)\n", i );
                return TZX_ERROR_INVALID_BLOCK;
            }
        }
        total += len;
    }
    if ( pos != block->data_length ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: %u data bytes after %u pulses\n", block->data_length - pos,
                         block->pulse_count );
        return TZX_ERROR_INVALID_BLOCK;
    }
    /* mez celého bloku včetně pauzy (zaokrouhlené nahoru na vzorky záznamu) */
    total += ( ( uint64_t ) block->pause_ms * block->sample_rate + 999u ) / 1000u;
    if ( total > ( uint64_t ) CMT_VSTREAM_MAX_BLOCK_SECONDS * block->sample_rate ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: signal longer than %u s\n", CMT_VSTREAM_MAX_BLOCK_SECONDS );
        return TZX_ERROR_UNSUPPORTED;
    }
    return TZX_OK;
}


/**
 * @brief Generátor bloku 0x18 (komprese RLE) s úrovní signálu.
 *
 * Strukturu dat a mez délky bloku ověří předem tzx_csw_recording_check(),
 * ještě před alokací vstreamu.
 * První pulz LOW, úrovně se střídají; konce pulzů se převádějí na výstupní
 * frekvenci kumulativně (tzx_csw_scale), pulz má nejméně 1 výstupní vzorek.
 * Po posledním pulzu je aktuální úroveň jeho opakem (jako po 0x15), pak pauza.
 * Bez pulzů se úroveň pro pauzu bere od volajícího (jako 0x15 bez dat).
 *
 * @param block Naparsovaný blok. @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód (viz tzx_generate_csw_recording()).
 * @return Nový vstream, nebo NULL při chybě.
 */
static st_CMT_VSTREAM* gen_csw_recording ( const st_TZX_CSW_RECORDING *block, const st_TZX_CONFIG *config,
                                           uint8_t *level, en_TZX_ERROR *err ) {

    /* struktura dat a mez bloku předem, před alokací vstreamu */
    en_TZX_ERROR ce = tzx_csw_recording_check ( block );
    if ( ce != TZX_OK ) {
        *err = ce;
        return NULL;
    }

    uint8_t lv = ( block->pulse_count > 0 ) ? 0 : tzx_level_norm ( *level );
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv == 1 );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    const uint8_t *d = block->data;
    uint32_t pos = 0;
    uint64_t src_end = 0;   /* konec dosud dekódovaných pulzů ve vzorcích záznamu */
    uint64_t emitted = 0;   /* počet zapsaných výstupních vzorků */

    for ( uint32_t i = 0; i < block->pulse_count; i++ ) {
        if ( pos >= block->data_length ) {
            g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: data end after %u of %u pulses\n", i, block->pulse_count );
            goto invalid;
        }
        uint32_t len = d[pos++];
        if ( len == 0 ) {
            /* únik: 32bitová délka little-endian */
            if ( block->data_length - pos < 4 ) {
                g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: truncated 32-bit pulse length (pulse %u)\n", i );
                goto invalid;
            }
            len = read_le32 ( d + pos );
            pos += 4;
            if ( len == 0 ) {
                g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: zero pulse length (pulse %u)\n", i );
                goto invalid;
            }
        }
        src_end += len;
        uint64_t target = tzx_csw_scale ( src_end, block->sample_rate, rate );
        uint64_t n = ( target > emitted ) ? target - emitted : 1;
        /* běh delší než jeden event vstreamu (2^32 vzorků, při 17 721 600 Hz asi 242 s) se rozdělí */
        if ( EXIT_FAILURE == cmt_vstream_add_value_long ( vs, lv, n ) ) goto fail;
        emitted += n;
        lv ^= 1;
    }
    if ( pos != block->data_length ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: %u data bytes after %u pulses\n", block->data_length - pos,
                         block->pulse_count );
        goto invalid;
    }

    /* lv je teď opak úrovně posledního pulzu (bez pulzů úroveň volajícího) */
    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, block->pause_ms, &lv, config ) ) goto fail;

    *level = lv;
    *err = TZX_OK;
    return vs;

invalid:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_INVALID_BLOCK;
    return NULL;

fail:
    cmt_vstream_destroy ( vs );
    *err = TZX_ERROR_STREAM_CREATE;
    return NULL;
}


/**
 * @brief Generátor pauzy s úrovní signálu.
 * @param pause_ms Pauza v ms (0 = žádný stream, úroveň beze změny).
 * @param config Konfigurace.
 * @param[in,out] level Aktuální úroveň; mění se jen při úspěchu.
 * @param err Výstupní chybový kód.
 * @return Nový vstream, nebo NULL při chybě a u pauzy 0 (err TZX_OK).
 */
static st_CMT_VSTREAM* gen_pause ( uint16_t pause_ms, const st_TZX_CONFIG *config, uint8_t *level, en_TZX_ERROR *err ) {

    if ( pause_ms == 0 ) { *err = TZX_OK; return NULL; }

    uint8_t lv = tzx_level_norm ( *level );
    st_CMT_VSTREAM *vs = tzx_create_vstream ( config, lv == 1 );
    if ( !vs ) { *err = TZX_ERROR_ALLOC; return NULL; }

    if ( EXIT_FAILURE == tzx_add_pause_to_vstream ( vs, pause_ms, &lv, config ) ) {
        cmt_vstream_destroy ( vs );
        *err = TZX_ERROR_STREAM_CREATE;
        return NULL;
    }

    *level = lv;
    *err = TZX_OK;
    return vs;
}


st_CMT_VSTREAM* tzx_generate_block ( const st_TZX_BLOCK *block, const st_TZX_CONFIG *config, uint8_t *level,
                                     en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config || !level || ( !block->data && block->length ) ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }

    switch ( block->id ) {
        case TZX_BLOCK_ID_STANDARD_SPEED: {
            st_TZX_STANDARD_SPEED p;
            if ( ( *err = tzx_parse_standard_speed ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_standard_speed ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_TURBO_SPEED: {
            st_TZX_TURBO_SPEED p;
            if ( ( *err = tzx_parse_turbo_speed ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_turbo_speed ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_PURE_TONE: {
            st_TZX_PURE_TONE p;
            if ( ( *err = tzx_parse_pure_tone ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_pure_tone ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_PULSE_SEQUENCE: {
            st_TZX_PULSE_SEQUENCE p;
            if ( ( *err = tzx_parse_pulse_sequence ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_pulse_sequence ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_PURE_DATA: {
            st_TZX_PURE_DATA p;
            if ( ( *err = tzx_parse_pure_data ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_pure_data ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_DIRECT_RECORDING: {
            st_TZX_DIRECT_RECORDING p;
            if ( ( *err = tzx_parse_direct_recording ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_direct_recording ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_CSW_RECORDING: {
            st_TZX_CSW_RECORDING p;
            if ( ( *err = tzx_parse_csw_recording ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_csw_recording ( &p, config, level, err );
        }
        case TZX_BLOCK_ID_PAUSE: {
            st_TZX_PAUSE p;
            if ( ( *err = tzx_parse_pause ( block->data, block->length, &p ) ) != TZX_OK ) return NULL;
            return gen_pause ( p.pause_ms, config, level, err );
        }
        default:
            *err = TZX_ERROR_UNSUPPORTED;
            return NULL;
    }
}


st_CMT_VSTREAM* tzx_generate_standard_speed ( const st_TZX_STANDARD_SPEED *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_standard_speed ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_turbo_speed ( const st_TZX_TURBO_SPEED *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_turbo_speed ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_pure_tone ( const st_TZX_PURE_TONE *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_pure_tone ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_pulse_sequence ( const st_TZX_PULSE_SEQUENCE *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_pulse_sequence ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_pure_data ( const st_TZX_PURE_DATA *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_pure_data ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_direct_recording ( const st_TZX_DIRECT_RECORDING *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_direct_recording ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_csw_recording ( const st_TZX_CSW_RECORDING *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_csw_recording ( block, config, &lv, err );
}


st_CMT_VSTREAM* tzx_generate_pause ( uint16_t pause_ms, const st_TZX_CONFIG *config, en_TZX_ERROR *err ) {
    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;
    if ( !config ) { *err = TZX_ERROR_NULL_INPUT; return NULL; }
    uint8_t lv = 0;
    return gen_pause ( pause_ms, config, &lv, err );
}


/* ========================================================================= */
/*  Nazvy bloku                                                              */
/* ========================================================================= */

/**
 * @brief Vrati lidsky citelny nazev TZX bloku podle jeho ID.
 *
 * Pokryva standardni TZX bloky (0x10-0x5A).
 * Pro nezname ID vraci "Unknown".
 *
 * @param id ID bloku.
 * @return Retezec s nazvem bloku, nebo "Unknown" pro nezname ID.
 */
const char* tzx_block_id_name ( uint8_t id ) {
    switch ( id ) {
        case TZX_BLOCK_ID_STANDARD_SPEED:    return "Standard Speed Data";
        case TZX_BLOCK_ID_TURBO_SPEED:       return "Turbo Speed Data";
        case TZX_BLOCK_ID_PURE_TONE:         return "Pure Tone";
        case TZX_BLOCK_ID_PULSE_SEQUENCE:    return "Pulse Sequence";
        case TZX_BLOCK_ID_PURE_DATA:         return "Pure Data";
        case TZX_BLOCK_ID_DIRECT_RECORDING:  return "Direct Recording";
        case TZX_BLOCK_ID_CSW_RECORDING:     return "CSW Recording";
        case TZX_BLOCK_ID_GENERALIZED_DATA:  return "Generalized Data";
        case TZX_BLOCK_ID_PAUSE:             return "Pause / Stop the Tape";
        case TZX_BLOCK_ID_GROUP_START:       return "Group Start";
        case TZX_BLOCK_ID_GROUP_END:         return "Group End";
        case TZX_BLOCK_ID_JUMP:              return "Jump to Block";
        case TZX_BLOCK_ID_LOOP_START:        return "Loop Start";
        case TZX_BLOCK_ID_LOOP_END:          return "Loop End";
        case TZX_BLOCK_ID_CALL_SEQUENCE:     return "Call Sequence";
        case TZX_BLOCK_ID_RETURN_FROM_SEQ:   return "Return from Sequence";
        case TZX_BLOCK_ID_SELECT_BLOCK:      return "Select Block";
        case TZX_BLOCK_ID_STOP_48K:          return "Stop the Tape if 48K";
        case TZX_BLOCK_ID_SET_SIGNAL_LEVEL:  return "Set Signal Level";
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:  return "Text Description";
        case TZX_BLOCK_ID_MESSAGE:           return "Message Block";
        case TZX_BLOCK_ID_ARCHIVE_INFO:      return "Archive Info";
        case TZX_BLOCK_ID_HARDWARE_TYPE:     return "Hardware Type";
        case TZX_BLOCK_ID_CUSTOM_INFO:       return "Custom Info";
        case TZX_BLOCK_ID_GLUE:              return "Glue Block";
        default:                             return "Unknown";
    }
}


/* ========================================================================= */
/*  Operace s hlavickou                                                      */
/* ========================================================================= */

/**
 * @brief Inicializuje TZX hlavicku na vychozi hodnoty.
 *
 * Nastavi signaturu "ZXTape!", EOF marker 0x1A a verzi 1.20.
 *
 * @param header Ukazatel na hlavicku k inicializaci.
 *
 * @pre header nesmi byt NULL.
 * @post Hlavicka obsahuje platne TZX hodnoty.
 */
void tzx_header_init ( st_TZX_HEADER *header ) {
    if ( !header ) return;
    memcpy ( header->signature, TZX_SIGNATURE, TZX_SIGNATURE_LENGTH );
    header->eof_marker = TZX_EOF_MARKER;
    header->ver_major = TZX_VERSION_MAJOR;
    header->ver_minor = TZX_VERSION_MINOR;
}


/**
 * @brief Zjisti, zda hlavicka obsahuje TZX signaturu "ZXTape!".
 *
 * @param header Ukazatel na hlavicku.
 * @return true pokud je signatura "ZXTape!", false jinak.
 */
bool tzx_header_is_tzx ( const st_TZX_HEADER *header ) {
    if ( !header ) return false;
    return ( memcmp ( header->signature, TZX_SIGNATURE, TZX_SIGNATURE_LENGTH ) == 0 );
}


/**
 * @brief Nacte TZX/TMZ hlavicku z handleru.
 *
 * Cte 10 bajtu od offsetu 0 a overuje signaturu (ZXTape! nebo TapeMZ!)
 * a EOF marker.
 *
 * @param h Otevreny handler.
 * @param header Vystupni buffer pro hlavicku.
 * @return TZX_OK pri uspechu, jinak chybovy kod.
 *
 * @pre h musi byt otevreny a pripraveny ke cteni.
 * @post Pri uspechu obsahuje header platnou hlavicku.
 */
en_TZX_ERROR tzx_read_header ( st_HANDLER *h, st_TZX_HEADER *header ) {
    if ( !h || !header ) return TZX_ERROR_IO;

    if ( EXIT_SUCCESS != generic_driver_read ( h, 0, header, sizeof ( st_TZX_HEADER ) ) ) {
        g_tzx_error_cb ( __func__, __LINE__, "Failed to read TZX header\n" );
        return TZX_ERROR_IO;
    }

    /* overeni signatury - akceptuje "ZXTape!" i "TapeMZ!" */
    static const char tmz_sig[] = "TapeMZ!";
    if ( memcmp ( header->signature, TZX_SIGNATURE, TZX_SIGNATURE_LENGTH ) != 0 &&
         memcmp ( header->signature, tmz_sig, TZX_SIGNATURE_LENGTH ) != 0 ) {
        g_tzx_error_cb ( __func__, __LINE__, "Invalid signature: expected ZXTape! or TapeMZ!\n" );
        return TZX_ERROR_INVALID_SIGNATURE;
    }

    /* overeni EOF markeru */
    if ( header->eof_marker != TZX_EOF_MARKER ) {
        g_tzx_error_cb ( __func__, __LINE__, "Invalid EOF marker: 0x%02X (expected 0x1A)\n", header->eof_marker );
        return TZX_ERROR_INVALID_SIGNATURE;
    }

    /* major verze: TZX i TMZ používají 1 (TZX 1.20, TMZ 1.0/1.1) */
    if ( header->ver_major != 1 ) {
        g_tzx_error_cb ( __func__, __LINE__, "Unsupported major version %u (expected 1)\n", header->ver_major );
        return TZX_ERROR_INVALID_VERSION;
    }

    return TZX_OK;
}


/**
 * @brief Zapise TZX hlavicku do handleru na offset 0.
 *
 * @param h Otevreny handler pro zapis.
 * @param header Hlavicka k zapisu.
 * @return TZX_OK pri uspechu, jinak chybovy kod.
 *
 * @pre h musi byt otevreny pro zapis.
 */
en_TZX_ERROR tzx_write_header ( st_HANDLER *h, const st_TZX_HEADER *header ) {
    if ( !h || !header ) return TZX_ERROR_IO;

    if ( EXIT_SUCCESS != generic_driver_write ( h, 0, (void*) header, sizeof ( st_TZX_HEADER ) ) ) {
        g_tzx_error_cb ( __func__, __LINE__, "Failed to write TZX header\n" );
        return TZX_ERROR_IO;
    }

    return TZX_OK;
}


/* ========================================================================= */
/*  Pomocne funkce pro cteni bloku                                           */
/* ========================================================================= */

/**
 * @brief Zjisti delku dat bloku podle jeho ID (dle TZX/TMZ konvence).
 *
 * Vetsina bloku ma 4bajtovou delku za ID. Specialni pripady:
 * - 0x10: 2B pauza + 2B delka dat
 * - 0x20: 2B pauza (zadna dalsi data)
 * - 0x22, 0x25: zadna data (0 bajtu)
 * - 0x24: 2B repeat count
 *
 * @param h Otevreny handler.
 * @param offset Aktualni offset v handleru (ukazuje na bajt ZA blokovym ID).
 * @param id ID bloku.
 * @param[out] block_data_length Vystupni delka dat bloku.
 * @param[out] total_consumed Celkovy pocet bajtu spotrebovanych timto blokem
 *             (vcetne hlavicky, bez ID bajtu).
 * @return TZX_OK pri uspechu, TZX_ERROR_IO pokud délková pole nejdou
 *         přečíst (useknutý soubor), TZX_ERROR_INVALID_BLOCK pokud by
 *         výsledná délka přetekla uint32_t.
 */
static en_TZX_ERROR tzx_read_block_length ( st_HANDLER *h, uint32_t offset, uint8_t id,
                                             uint32_t *block_data_length, uint32_t *total_consumed ) {

    switch ( id ) {

        /* Blok 0x10 - Standard Speed Data: 2B pauza + 2B delka + data */
        case TZX_BLOCK_ID_STANDARD_SPEED:
        {
            uint8_t buf[4];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 4 ) ) {
                return TZX_ERROR_IO;
            }
            uint16_t data_len = (uint16_t) buf[2] | ( (uint16_t) buf[3] << 8 );
            *block_data_length = 4 + data_len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x20 - Pause: 2B pauza */
        case TZX_BLOCK_ID_PAUSE:
        {
            *block_data_length = 2;
            *total_consumed = 2;
            return TZX_OK;
        }

        /* Bloky bez dat */
        case TZX_BLOCK_ID_GROUP_END:
        case TZX_BLOCK_ID_LOOP_END:
        {
            *block_data_length = 0;
            *total_consumed = 0;
            return TZX_OK;
        }

        /* Blok 0x24 - Loop Start: 2B repeat count */
        case TZX_BLOCK_ID_LOOP_START:
        /* Blok 0x23 - Jump: 2B relative offset */
        case TZX_BLOCK_ID_JUMP:
        {
            *block_data_length = 2;
            *total_consumed = 2;
            return TZX_OK;
        }

        /* Blok 0x27 - Return from Sequence: zadna data */
        case TZX_BLOCK_ID_RETURN_FROM_SEQ:
        {
            *block_data_length = 0;
            *total_consumed = 0;
            return TZX_OK;
        }

        /*
         * Blok 0x11 - Turbo Speed Data:
         * 2B pilot + 2B sync1 + 2B sync2 + 2B zero + 2B one +
         * 2B pilot_tone + 1B used + 2B pause + 3B data_len + data
         * Hlavicka: 15B, data_len na offsetu 0x0F-0x11.
         */
        case TZX_BLOCK_ID_TURBO_SPEED:
        {
            uint8_t buf[18];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 18 ) ) {
                return TZX_ERROR_IO;
            }
            uint32_t data_len = (uint32_t) buf[15]
                              | ( (uint32_t) buf[16] << 8 )
                              | ( (uint32_t) buf[17] << 16 );
            *block_data_length = 18 + data_len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /*
         * Blok 0x14 - Pure Data:
         * 2B zero + 2B one + 1B used + 2B pause + 3B data_len + data
         * Hlavicka: 10B, data_len na offsetu 0x07-0x09.
         */
        case TZX_BLOCK_ID_PURE_DATA:
        {
            uint8_t buf[10];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 10 ) ) {
                return TZX_ERROR_IO;
            }
            uint32_t data_len = (uint32_t) buf[7]
                              | ( (uint32_t) buf[8] << 8 )
                              | ( (uint32_t) buf[9] << 16 );
            *block_data_length = 10 + data_len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /*
         * Blok 0x15 - Direct Recording:
         * 2B tstates + 2B pause + 1B used + 3B data_len + data
         * Hlavicka: 8B, data_len na offsetu 0x05-0x07.
         */
        case TZX_BLOCK_ID_DIRECT_RECORDING:
        {
            uint8_t buf[8];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 8 ) ) {
                return TZX_ERROR_IO;
            }
            uint32_t data_len = (uint32_t) buf[5]
                              | ( (uint32_t) buf[6] << 8 )
                              | ( (uint32_t) buf[7] << 16 );
            *block_data_length = 8 + data_len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x12 - Pure Tone: 2B delka + 2B pocet = fix 4B */
        case TZX_BLOCK_ID_PURE_TONE:
        {
            *block_data_length = 4;
            *total_consumed = 4;
            return TZX_OK;
        }

        /* Blok 0x13 - Pulse Sequence: 1B count + count*2B */
        case TZX_BLOCK_ID_PULSE_SEQUENCE:
        {
            uint8_t count;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &count, 1 ) ) {
                return TZX_ERROR_IO;
            }
            *block_data_length = 1 + (uint32_t) count * 2;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x21 - Group Start: 1B len + text */
        case TZX_BLOCK_ID_GROUP_START:
        /* Blok 0x30 - Text Description: 1B len + text */
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:
        {
            uint8_t len;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &len, 1 ) ) {
                return TZX_ERROR_IO;
            }
            *block_data_length = 1 + len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x31 - Message: 1B time + 1B len + text */
        case TZX_BLOCK_ID_MESSAGE:
        {
            uint8_t buf[2];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 2 ) ) {
                return TZX_ERROR_IO;
            }
            *block_data_length = 2 + buf[1];
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x33 - Hardware Type: 1B count + count*3B */
        case TZX_BLOCK_ID_HARDWARE_TYPE:
        {
            uint8_t count;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &count, 1 ) ) {
                return TZX_ERROR_IO;
            }
            *block_data_length = 1 + (uint32_t) count * 3;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x26 - Call Sequence: 2B count + count*2B offsets */
        case TZX_BLOCK_ID_CALL_SEQUENCE:
        {
            uint8_t buf[2];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 2 ) ) {
                return TZX_ERROR_IO;
            }
            uint16_t count = (uint16_t) buf[0] | ( (uint16_t) buf[1] << 8 );
            *block_data_length = 2 + (uint32_t) count * 2;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x28 - Select Block: 2B delka + data */
        case TZX_BLOCK_ID_SELECT_BLOCK:
        /* Blok 0x32 - Archive Info: 2B delka + data */
        case TZX_BLOCK_ID_ARCHIVE_INFO:
        {
            uint8_t buf[2];
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, buf, 2 ) ) {
                return TZX_ERROR_IO;
            }
            uint16_t len = (uint16_t) buf[0] | ( (uint16_t) buf[1] << 8 );
            *block_data_length = 2 + len;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x35 - Custom Info: 16B ASCII ID + 4B DWORD délka + data (TZX v1.01+) */
        case TZX_BLOCK_ID_CUSTOM_INFO:
        {
            uint32_t length;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset + TZX_CUSTOM_INFO_ID_LENGTH, &length, 4 ) ) {
                return TZX_ERROR_IO;
            }
            length = endianity_bswap32_LE ( length );
            if ( length > UINT32_MAX - ( TZX_CUSTOM_INFO_ID_LENGTH + 4 ) ) {
                return TZX_ERROR_INVALID_BLOCK;
            }
            *block_data_length = TZX_CUSTOM_INFO_ID_LENGTH + 4 + length;
            *total_consumed = *block_data_length;
            return TZX_OK;
        }

        /* Blok 0x5A - Glue Block: fix 9B (bez DWORD delky) */
        case TZX_BLOCK_ID_GLUE:
        {
            *block_data_length = 9;
            *total_consumed = 9;
            return TZX_OK;
        }

        /* Vsechny ostatni bloky: 4B delka za ID */
        default:
        {
            uint32_t length;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &length, 4 ) ) {
                return TZX_ERROR_IO;
            }
            length = endianity_bswap32_LE ( length );
            if ( length > UINT32_MAX - 4 ) {
                return TZX_ERROR_INVALID_BLOCK;
            }
            *block_data_length = length;
            *total_consumed = 4 + length;
            return TZX_OK;
        }
    }
}


/* ========================================================================= */
/*  Nacitani a ukladani TZX souboru                                          */
/* ========================================================================= */

/**
 * @brief Uvolní rozpracovaný seznam bloků a strukturu souboru při chybě načítání.
 *
 * @param blocks Pole bloků (může být NULL).
 * @param count Počet bloků s platnými (nebo NULL) ukazateli na data.
 * @param file Struktura souboru (může být NULL).
 */
static void tzx_load_cleanup ( st_TZX_BLOCK *blocks, uint32_t count, st_TZX_FILE *file ) {
    if ( blocks ) {
        for ( uint32_t i = 0; i < count; i++ ) {
            if ( blocks[i].data ) g_tzx_allocator->free ( blocks[i].data );
        }
        g_tzx_allocator->free ( blocks );
    }
    if ( file ) g_tzx_allocator->free ( file );
}


/**
 * @brief Nacte cely TZX/TMZ soubor (hlavicku + vsechny bloky) z handleru.
 *
 * Prochazi bloky od offsetu 10 (za hlavickou) az do konce souboru.
 * Kazdy blok alokuje samostatne a uklada jeho surova data.
 * Pole bloku se dynamicky zvetsuje.
 *
 * Konec souboru je platný jen na hranici bloku (před bajtem ID). Před
 * alokací dat bloku se ověří, že soubor obsahuje jeho poslední bajt, takže
 * nesmyslná délka v poškozeném souboru nevede k obří alokaci. Při chybě
 * se nikdy nevrací částečně načtený soubor.
 *
 * @param h Otevreny handler s TZX/TMZ daty.
 * @param err Vystupni chybovy kod (muze byt NULL):
 *            - TZX_OK při úspěchu,
 *            - TZX_ERROR_UNEXPECTED_EOF pro blok useknutý v délce nebo datech,
 *            - TZX_ERROR_INVALID_BLOCK pro délku bloku přetékající 32bit offset,
 *            - TZX_ERROR_ALLOC při selhání alokace,
 *            - TZX_ERROR_IO / TZX_ERROR_INVALID_SIGNATURE
 *              jen z hlavičky souboru (tzx_read_header) nebo při NULL handleru.
 *            Selhání čtení uvnitř bloků (useknutí i I/O chyba média) se
 *            hlásí jako TZX_ERROR_UNEXPECTED_EOF - file driver je nerozliší.
 * @return Ukazatel na novou st_TZX_FILE strukturu, nebo NULL pri chybe.
 *
 * @pre h musi byt otevreny a pripraveny ke cteni.
 * @post Volajici vlastni vracenou strukturu a musi ji uvolnit pres tzx_free().
 */
st_TZX_FILE* tzx_load ( st_HANDLER *h, en_TZX_ERROR *err ) {

    en_TZX_ERROR local_err = TZX_OK;
    if ( !err ) err = &local_err;

    if ( !h ) {
        *err = TZX_ERROR_IO;
        return NULL;
    }

    /* alokace struktury souboru */
    st_TZX_FILE *file = g_tzx_allocator->alloc0 ( sizeof ( st_TZX_FILE ) );
    if ( !file ) {
        g_tzx_error_cb ( __func__, __LINE__, "Failed to allocate TZX file structure\n" );
        *err = TZX_ERROR_ALLOC;
        return NULL;
    }

    /* nacteni hlavicky */
    *err = tzx_read_header ( h, &file->header );
    if ( *err != TZX_OK ) {
        g_tzx_allocator->free ( file );
        return NULL;
    }

    /* rozpoznani signatury */
    static const char tmz_sig[] = "TapeMZ!";
    file->is_tmz = ( memcmp ( file->header.signature, tmz_sig, TZX_SIGNATURE_LENGTH ) == 0 );

    /* cteni bloku */
    uint32_t offset = sizeof ( st_TZX_HEADER );
    uint32_t capacity = 16;
    uint32_t count = 0;

    st_TZX_BLOCK *blocks = g_tzx_allocator->alloc0 ( capacity * sizeof ( st_TZX_BLOCK ) );
    if ( !blocks ) {
        g_tzx_error_cb ( __func__, __LINE__, "Failed to allocate block array\n" );
        g_tzx_allocator->free ( file );
        *err = TZX_ERROR_ALLOC;
        return NULL;
    }

    while ( 1 ) {
        /* cteni ID bloku */
        uint8_t id;
        if ( EXIT_SUCCESS != generic_driver_read ( h, offset, &id, 1 ) ) {
            /* konec souboru - normalni ukonceni */
            break;
        }
        offset += 1;

        /* zjisteni delky bloku */
        uint32_t block_data_length = 0;
        uint32_t total_consumed = 0;

        *err = tzx_read_block_length ( h, offset, id, &block_data_length, &total_consumed );
        if ( *err != TZX_OK ) {
            g_tzx_error_cb ( __func__, __LINE__, "Invalid or truncated block 0x%02X length at offset %u\n", id, offset );
            tzx_load_cleanup ( blocks, count, file );
            if ( *err != TZX_ERROR_INVALID_BLOCK ) *err = TZX_ERROR_UNEXPECTED_EOF;
            return NULL;
        }

        /* ochrana proti přetečení offsetu */
        if ( total_consumed > UINT32_MAX - offset ) {
            g_tzx_error_cb ( __func__, __LINE__, "Block 0x%02X length %u overflows file offset\n", id, total_consumed );
            tzx_load_cleanup ( blocks, count, file );
            *err = TZX_ERROR_INVALID_BLOCK;
            return NULL;
        }

        /* sonda posledního bajtu bloku - neexistující data se nealokují */
        if ( total_consumed > 0 ) {
            uint8_t probe;
            if ( EXIT_SUCCESS != generic_driver_read ( h, offset + total_consumed - 1, &probe, 1 ) ) {
                g_tzx_error_cb ( __func__, __LINE__, "Block 0x%02X truncated: needs %u bytes at offset %u\n", id, total_consumed, offset );
                tzx_load_cleanup ( blocks, count, file );
                *err = TZX_ERROR_UNEXPECTED_EOF;
                return NULL;
            }
        }

        /* zvetseni pole pokud je potreba */
        if ( count >= capacity ) {
            capacity *= 2;
            st_TZX_BLOCK *new_blocks = g_tzx_allocator->alloc0 ( capacity * sizeof ( st_TZX_BLOCK ) );
            if ( !new_blocks ) {
                g_tzx_error_cb ( __func__, __LINE__, "Failed to grow block array\n" );
                tzx_load_cleanup ( blocks, count, file );
                *err = TZX_ERROR_ALLOC;
                return NULL;
            }
            memcpy ( new_blocks, blocks, count * sizeof ( st_TZX_BLOCK ) );
            g_tzx_allocator->free ( blocks );
            blocks = new_blocks;
        }

        /* naplneni bloku */
        blocks[count].id = id;
        blocks[count].length = block_data_length;
        blocks[count].data = NULL;

        if ( total_consumed > 0 ) {
            blocks[count].data = g_tzx_allocator->alloc ( total_consumed );
            if ( !blocks[count].data ) {
                g_tzx_error_cb ( __func__, __LINE__, "Failed to allocate block data (%u bytes)\n", total_consumed );
                tzx_load_cleanup ( blocks, count, file );
                *err = TZX_ERROR_ALLOC;
                return NULL;
            }

            if ( EXIT_SUCCESS != generic_driver_read ( h, offset, blocks[count].data, total_consumed ) ) {
                g_tzx_error_cb ( __func__, __LINE__, "Failed to read block 0x%02X data at offset %u\n", id, offset );
                g_tzx_allocator->free ( blocks[count].data );
                blocks[count].data = NULL;
                tzx_load_cleanup ( blocks, count, file );
                *err = TZX_ERROR_UNEXPECTED_EOF;
                return NULL;
            }

            blocks[count].length = total_consumed;
        }

        offset += total_consumed;
        count++;
    }

    file->block_count = count;
    file->block_capacity = capacity;
    file->blocks = blocks;
    *err = TZX_OK;

    return file;
}


/**
 * @brief Zapise cely TZX soubor (hlavicku + vsechny bloky) do handleru.
 *
 * @param h Otevreny handler pro zapis.
 * @param file TZX soubor k zapisu.
 * @return TZX_OK pri uspechu, jinak chybovy kod.
 *
 * @pre h musi byt otevreny pro zapis, file nesmi byt NULL.
 */
en_TZX_ERROR tzx_save ( st_HANDLER *h, const st_TZX_FILE *file ) {

    if ( !h || !file ) return TZX_ERROR_IO;

    /* zapis hlavicky */
    en_TZX_ERROR err = tzx_write_header ( h, &file->header );
    if ( err != TZX_OK ) return err;

    uint32_t offset = sizeof ( st_TZX_HEADER );

    /* zapis bloku */
    for ( uint32_t i = 0; i < file->block_count; i++ ) {

        const st_TZX_BLOCK *block = &file->blocks[i];

        /* zapis ID */
        if ( EXIT_SUCCESS != generic_driver_write ( h, offset, (void*) &block->id, 1 ) ) {
            g_tzx_error_cb ( __func__, __LINE__, "Failed to write block ID 0x%02X\n", block->id );
            return TZX_ERROR_IO;
        }
        offset += 1;

        /* zapis dat bloku (vcetne hlavicky bloku - delka je soucasti dat) */
        if ( block->length > 0 && block->data ) {
            if ( EXIT_SUCCESS != generic_driver_write ( h, offset, block->data, block->length ) ) {
                g_tzx_error_cb ( __func__, __LINE__, "Failed to write block 0x%02X data\n", block->id );
                return TZX_ERROR_IO;
            }
            offset += block->length;
        }
    }

    return TZX_OK;
}


/**
 * @brief Uvolni st_TZX_FILE strukturu vcetne vsech bloku a jejich dat.
 *
 * Bezpecne volat s NULL (no-op).
 *
 * @param file Ukazatel na strukturu k uvolneni (muze byt NULL).
 */
void tzx_free ( st_TZX_FILE *file ) {
    if ( !file ) return;

    if ( file->blocks ) {
        for ( uint32_t i = 0; i < file->block_count; i++ ) {
            if ( file->blocks[i].data ) {
                g_tzx_allocator->free ( file->blocks[i].data );
            }
        }
        g_tzx_allocator->free ( file->blocks );
    }

    g_tzx_allocator->free ( file );
}


/* ========================================================================= */
/*  Manipulace s bloky                                                       */
/* ========================================================================= */


/**
 * @brief Zajisti dostatecnou kapacitu pole bloku.
 *
 * Pokud je aktualni kapacita mensi nez pozadovana, zvetsuje pole
 * na dvojnasobek (nebo na min_capacity, pokud je vetsi).
 *
 * @param file TZX soubor.
 * @param min_capacity Minimalni pozadovana kapacita.
 * @return TZX_OK pri uspechu, TZX_ERROR_ALLOC pri selhani alokace.
 */
static en_TZX_ERROR tzx_ensure_capacity ( st_TZX_FILE *file, uint32_t min_capacity ) {

    if ( file->block_capacity >= min_capacity ) return TZX_OK;

    uint32_t new_cap = file->block_capacity;
    if ( new_cap == 0 ) new_cap = 16;
    while ( new_cap < min_capacity ) new_cap *= 2;

    st_TZX_BLOCK *new_blocks = g_tzx_allocator->alloc0 ( new_cap * sizeof ( st_TZX_BLOCK ) );
    if ( !new_blocks ) {
        g_tzx_error_cb ( __func__, __LINE__, "Failed to grow block array to %u\n", new_cap );
        return TZX_ERROR_ALLOC;
    }

    if ( file->blocks && file->block_count > 0 ) {
        memcpy ( new_blocks, file->blocks, file->block_count * sizeof ( st_TZX_BLOCK ) );
    }
    if ( file->blocks ) {
        g_tzx_allocator->free ( file->blocks );
    }

    file->blocks = new_blocks;
    file->block_capacity = new_cap;
    return TZX_OK;
}


en_TZX_ERROR tzx_file_remove_block ( st_TZX_FILE *file, uint32_t index ) {

    if ( !file ) return TZX_ERROR_NULL_INPUT;
    if ( index >= file->block_count ) return TZX_ERROR_INVALID_BLOCK;

    /* uvolneni dat bloku */
    if ( file->blocks[index].data ) {
        g_tzx_allocator->free ( file->blocks[index].data );
    }

    /* posun zbylych bloku doleva */
    for ( uint32_t i = index; i + 1 < file->block_count; i++ ) {
        file->blocks[i] = file->blocks[i + 1];
    }
    file->block_count--;

    return TZX_OK;
}


en_TZX_ERROR tzx_file_insert_block ( st_TZX_FILE *file, uint32_t index, const st_TZX_BLOCK *block ) {

    if ( !file || !block ) return TZX_ERROR_NULL_INPUT;
    if ( index > file->block_count ) return TZX_ERROR_INVALID_BLOCK;

    /* zvetseni pole pokud je potreba */
    en_TZX_ERROR err = tzx_ensure_capacity ( file, file->block_count + 1 );
    if ( err != TZX_OK ) return err;

    /* posun bloku doprava */
    for ( uint32_t i = file->block_count; i > index; i-- ) {
        file->blocks[i] = file->blocks[i - 1];
    }

    /* vlozeni noveho bloku (kopie struktury, ownership dat predan) */
    file->blocks[index] = *block;
    file->block_count++;

    return TZX_OK;
}


en_TZX_ERROR tzx_file_append_block ( st_TZX_FILE *file, const st_TZX_BLOCK *block ) {

    if ( !file ) return TZX_ERROR_NULL_INPUT;
    return tzx_file_insert_block ( file, file->block_count, block );
}


en_TZX_ERROR tzx_file_move_block ( st_TZX_FILE *file, uint32_t from, uint32_t to ) {

    if ( !file ) return TZX_ERROR_NULL_INPUT;
    if ( from >= file->block_count ) return TZX_ERROR_INVALID_BLOCK;
    if ( to >= file->block_count ) return TZX_ERROR_INVALID_BLOCK;
    if ( from == to ) return TZX_OK;

    /* ulozeni bloku */
    st_TZX_BLOCK saved = file->blocks[from];

    /* posun bloku */
    if ( from < to ) {
        for ( uint32_t i = from; i < to; i++ ) {
            file->blocks[i] = file->blocks[i + 1];
        }
    } else {
        for ( uint32_t i = from; i > to; i-- ) {
            file->blocks[i] = file->blocks[i - 1];
        }
    }

    file->blocks[to] = saved;
    return TZX_OK;
}


en_TZX_ERROR tzx_file_merge ( st_TZX_FILE *dst, st_TZX_FILE *src ) {

    if ( !dst || !src ) return TZX_ERROR_NULL_INPUT;

    if ( src->block_count == 0 ) return TZX_OK;

    /* zvetseni ciloveho pole */
    en_TZX_ERROR err = tzx_ensure_capacity ( dst, dst->block_count + src->block_count );
    if ( err != TZX_OK ) return err;

    /* kopirovani bloku (ownership dat predan) */
    memcpy ( &dst->blocks[dst->block_count], src->blocks,
             src->block_count * sizeof ( st_TZX_BLOCK ) );
    dst->block_count += src->block_count;

    /* vyprazdneni src (data jsou nyni vlastnena dst) */
    g_tzx_allocator->free ( src->blocks );
    src->blocks = NULL;
    src->block_count = 0;
    src->block_capacity = 0;

    return TZX_OK;
}


/* ========================================================================= */
/*  Vytvareni TZX info bloku                                                 */
/* ========================================================================= */


en_TZX_ERROR tzx_block_create_text_description ( const char *text, st_TZX_BLOCK *block ) {

    if ( !text || !block ) return TZX_ERROR_NULL_INPUT;

    uint8_t len = (uint8_t) strlen ( text );
    if ( strlen ( text ) > 255 ) len = 255;

    uint32_t total = 1 + len;
    uint8_t *data = g_tzx_allocator->alloc ( total );
    if ( !data ) return TZX_ERROR_ALLOC;

    data[0] = len;
    memcpy ( data + 1, text, len );

    block->id = TZX_BLOCK_ID_TEXT_DESCRIPTION;
    block->length = total;
    block->data = data;

    return TZX_OK;
}


en_TZX_ERROR tzx_block_create_message ( const char *text, uint8_t time_seconds, st_TZX_BLOCK *block ) {

    if ( !text || !block ) return TZX_ERROR_NULL_INPUT;

    uint8_t len = (uint8_t) strlen ( text );
    if ( strlen ( text ) > 255 ) len = 255;

    uint32_t total = 2 + len;
    uint8_t *data = g_tzx_allocator->alloc ( total );
    if ( !data ) return TZX_ERROR_ALLOC;

    data[0] = time_seconds;
    data[1] = len;
    memcpy ( data + 2, text, len );

    block->id = TZX_BLOCK_ID_MESSAGE;
    block->length = total;
    block->data = data;

    return TZX_OK;
}


en_TZX_ERROR tzx_block_create_archive_info ( const st_TZX_ARCHIVE_ENTRY *entries, uint8_t count, st_TZX_BLOCK *block ) {

    if ( !entries || !block || count == 0 ) return TZX_ERROR_NULL_INPUT;

    /* spocitani celkove delky zaznamu */
    uint32_t records_size = 0;
    for ( uint8_t i = 0; i < count; i++ ) {
        if ( !entries[i].text ) return TZX_ERROR_NULL_INPUT;
        size_t slen = strlen ( entries[i].text );
        if ( slen > 255 ) slen = 255;
        records_size += 2 + (uint32_t) slen; /* 1B type + 1B len + text */
    }

    /* WORD délky těla (count + záznamy) neumí víc než 65535 B (dřív zápis za buffer) */
    if ( 1 + records_size > 0xFFFFu ) return TZX_ERROR_INVALID_BLOCK;

    /* format bloku 0x32: [2B celkova delka za WORD][1B count][zaznamy...] */
    uint16_t body_length = (uint16_t) ( 1 + records_size );
    uint32_t total = 2 + body_length;

    uint8_t *data = g_tzx_allocator->alloc ( total );
    if ( !data ) return TZX_ERROR_ALLOC;

    /* WORD delka (LE) */
    data[0] = (uint8_t) ( body_length & 0xFF );
    data[1] = (uint8_t) ( body_length >> 8 );
    data[2] = count;

    uint32_t pos = 3;
    for ( uint8_t i = 0; i < count; i++ ) {
        size_t slen = strlen ( entries[i].text );
        if ( slen > 255 ) slen = 255;
        data[pos++] = entries[i].type_id;
        data[pos++] = (uint8_t) slen;
        memcpy ( data + pos, entries[i].text, slen );
        pos += (uint32_t) slen;
    }

    block->id = TZX_BLOCK_ID_ARCHIVE_INFO;
    block->length = total;
    block->data = data;

    return TZX_OK;
}


en_TZX_ERROR tzx_block_create_direct_recording (
    uint16_t tstates_per_sample,
    uint16_t pause_ms,
    uint8_t used_bits,
    const uint8_t *data,
    uint32_t data_length,
    st_TZX_BLOCK *block
) {
    if ( !block ) return TZX_ERROR_NULL_INPUT;
    if ( data_length > 0 && !data ) return TZX_ERROR_NULL_INPUT;
    if ( used_bits < 1 || used_bits > 8 ) return TZX_ERROR_INVALID_BLOCK;

    /*
     * Binární formát bloku 0x15:
     * [2B tstates_per_sample LE]
     * [2B pause_ms LE]
     * [1B used_bits_in_last_byte]
     * [3B data_length LE]
     * [data_length bajtu dat]
     */
    uint32_t header_size = 2 + 2 + 1 + 3;
    uint32_t total = header_size + data_length;

    uint8_t *buf = g_tzx_allocator->alloc ( total );
    if ( !buf ) return TZX_ERROR_ALLOC;

    /* tstates_per_sample (LE) */
    buf[0] = ( uint8_t ) ( tstates_per_sample & 0xFF );
    buf[1] = ( uint8_t ) ( tstates_per_sample >> 8 );

    /* pause_ms (LE) */
    buf[2] = ( uint8_t ) ( pause_ms & 0xFF );
    buf[3] = ( uint8_t ) ( pause_ms >> 8 );

    /* used_bits_in_last_byte */
    buf[4] = used_bits;

    /* data_length (3B LE) */
    buf[5] = ( uint8_t ) ( data_length & 0xFF );
    buf[6] = ( uint8_t ) ( ( data_length >> 8 ) & 0xFF );
    buf[7] = ( uint8_t ) ( ( data_length >> 16 ) & 0xFF );

    /* data */
    if ( data_length > 0 ) {
        memcpy ( buf + header_size, data, data_length );
    }

    block->id = TZX_BLOCK_ID_DIRECT_RECORDING;
    block->length = total;
    block->data = buf;

    return TZX_OK;
}


/** @brief Zapíše little-endian DWORD do bufferu. */
static inline void write_le32 ( uint8_t *p, uint32_t v ) {
    p[0] = ( uint8_t ) v;
    p[1] = ( uint8_t ) ( v >> 8 );
    p[2] = ( uint8_t ) ( v >> 16 );
    p[3] = ( uint8_t ) ( v >> 24 );
}


en_TZX_ERROR tzx_block_create_csw_recording ( uint32_t sample_rate, uint16_t pause_ms, const uint32_t *pulses,
                                              uint32_t pulse_count, st_TZX_BLOCK *block ) {

    if ( !pulses || !block || pulse_count == 0 ) return TZX_ERROR_NULL_INPUT;
    if ( sample_rate == 0 || sample_rate > TZX_CSW_MAX_SAMPLE_RATE ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: sample rate %u out of range 1-%u\n", sample_rate,
                         TZX_CSW_MAX_SAMPLE_RATE );
        return TZX_ERROR_INVALID_BLOCK;
    }

    /* délka dat RLE: 1 B na pulz 1-255, 5 B (00h + DWORD) na delší */
    uint64_t rle_len = 0;
    for ( uint32_t i = 0; i < pulse_count; i++ ) {
        if ( pulses[i] == 0 ) {
            g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: zero pulse length (pulse %u)\n", i );
            return TZX_ERROR_INVALID_BLOCK;
        }
        rle_len += ( pulses[i] <= 0xFF ) ? 1 : 5;
    }

    /* blok = DWORD délky + 10 B hlavičky + data; musí se vejít do uint32_t length */
    const uint32_t header_size = 4 + 2 + 3 + 1 + 4;
    if ( rle_len > (uint64_t) UINT32_MAX - header_size ) {
        g_tzx_error_cb ( __func__, __LINE__, "Block 0x18: %llu data bytes exceed the block size limit\n",
                         ( unsigned long long ) rle_len );
        return TZX_ERROR_INVALID_BLOCK;
    }
    uint32_t total = header_size + ( uint32_t ) rle_len;

    uint8_t *buf = g_tzx_allocator->alloc ( total );
    if ( !buf ) return TZX_ERROR_ALLOC;

    /*
     * Binární formát bloku 0x18 (data jako v st_TZX_FILE):
     * [DWORD délka za tímto polem = 10 + N][WORD pause_ms][3B sample_rate]
     * [1B komprese][DWORD pulse_count][N B dat RLE]
     */
    write_le32 ( buf, total - 4 );
    buf[4] = ( uint8_t ) ( pause_ms & 0xFF );
    buf[5] = ( uint8_t ) ( pause_ms >> 8 );
    buf[6] = ( uint8_t ) ( sample_rate & 0xFF );
    buf[7] = ( uint8_t ) ( ( sample_rate >> 8 ) & 0xFF );
    buf[8] = ( uint8_t ) ( ( sample_rate >> 16 ) & 0xFF );
    buf[9] = TZX_CSW_COMPRESSION_RLE;
    write_le32 ( buf + 10, pulse_count );

    uint32_t pos = header_size;
    for ( uint32_t i = 0; i < pulse_count; i++ ) {
        if ( pulses[i] <= 0xFF ) {
            buf[pos++] = ( uint8_t ) pulses[i];
        } else {
            buf[pos++] = 0x00;
            write_le32 ( buf + pos, pulses[i] );
            pos += 4;
        }
    }

    block->id = TZX_BLOCK_ID_CSW_RECORDING;
    block->length = total;
    block->data = buf;

    return TZX_OK;
}


const char* tzx_version ( void ) {
    return TZX_VERSION;
}


const char* tzx_format_version ( void ) {
    return TZX_FORMAT_VERSION;
}
