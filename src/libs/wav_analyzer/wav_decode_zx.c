/**
 * @file   wav_decode_zx.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace ZX Spectrum nativniho dekoderu.
 *
 * Dekoduje nativni ZX Spectrum kazetovy signal:
 * pilot (~612 us) -> sync (2 kratke pul-periody) -> data (MSB first,
 * bit 1 = 2x ~484 us, bit 0 = 2x ~242 us) -> XOR checksum.
 *
 * Kalibrace prahu z pilot tonu:
 * - threshold = pilot_avg * 0.59 (stred mezi short ~0.395 a long ~0.789 pilotu)
 * - data_max = pilot_avg * 0.90 (cokoliv vetsi = pilot/ticho, konec dat)
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

#include <stdlib.h>
#include <string.h>

#include "wav_decode_zx.h"


/** @brief Koeficient pro vypocet prahu SHORT/LONG z pilot pul-periody. */
#define ZX_THRESHOLD_FACTOR     0.59

/** @brief Koeficient pro vypocet maximalni platne pul-periody. */
#define ZX_DATA_MAX_FACTOR      0.90

/** @brief Koeficient pro detekci sync pulzu (< pilot_avg * SYNC_FACTOR). */
#define ZX_SYNC_FACTOR          0.50


/**
 * @brief Inicializuje ZX dekoder z leaderu.
 *
 * Nastavi pozici za konec pilotu a kalibruje prahy
 * z prumerne pul-periody leader tonu.
 *
 * @param dec Kontext dekoderu k inicializaci. Nesmi byt NULL.
 * @param seq Sekvence pulzu. Nesmi byt NULL.
 * @param leader Informace o leader tonu. Nesmi byt NULL.
 *
 * @pre seq != NULL, leader != NULL, dec != NULL
 * @post dec je pripraveny pro hledani sync a cteni dat.
 */
static void wav_decode_zx_init (
    st_WAV_ZX_DECODER *dec,
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader
) {
    dec->seq = seq;
    dec->pos = leader->start_index + leader->pulse_count;
    dec->pilot_avg_us = leader->avg_period_us;
    dec->threshold_us = leader->avg_period_us * ZX_THRESHOLD_FACTOR;
    dec->data_max_us = leader->avg_period_us * ZX_DATA_MAX_FACTOR;
}


/**
 * @brief Najde sync pulzy za pilotem.
 *
 * Hleda prvni pul-periodu kratsi nez pilot_avg * 0.50 (sync ~145 us
 * vs pilot ~612 us). Overuje, ze nasledujici pul-perioda je take kratka.
 * Preskoci obe sync pul-periody.
 *
 * @param dec Kontext dekoderu. Nesmi byt NULL.
 * @return WAV_ANALYZER_OK pri uspechu,
 *         WAV_ANALYZER_ERROR_DECODE_TAPEMARK pokud sync nenalezen.
 *
 * @post dec->pos ukazuje na prvni datovy pulz za sync.
 */
static en_WAV_ANALYZER_ERROR wav_decode_zx_find_sync (
    st_WAV_ZX_DECODER *dec
) {
    double sync_limit = dec->pilot_avg_us * ZX_SYNC_FACTOR;
    uint32_t search_end = dec->pos + WAV_ZX_SYNC_SEARCH_MAX;

    if ( search_end > dec->seq->count ) {
        search_end = dec->seq->count;
    }

    /* hledame prvni kratky pulz (= zacatek sync) */
    while ( dec->pos < search_end ) {
        if ( dec->seq->pulses[dec->pos].duration_us < sync_limit ) {
            /* nasli jsme prvni sync pul-periodu */
            if ( dec->pos + 1 >= dec->seq->count ) {
                return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
            }

            /* overime, ze druha pul-perioda je take kratka */
            if ( dec->seq->pulses[dec->pos + 1].duration_us < sync_limit ) {
                /* preskocime obe sync pul-periody */
                dec->pos += 2;
                return WAV_ANALYZER_OK;
            }
        }
        dec->pos++;
    }

    return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
}


/**
 * @brief Navratova hodnota cteni jednoho bitu.
 */
typedef enum en_ZX_BIT_RESULT {
    ZX_BIT_ZERO = 0,       /**< bit 0 */
    ZX_BIT_ONE = 1,         /**< bit 1 */
    ZX_BIT_INCOMPLETE = -1, /**< konec dat (pulz > data_max_us) */
    ZX_BIT_ERROR = -2,      /**< chyba (nedostatek pulzu) */
} en_ZX_BIT_RESULT;


/**
 * @brief Precte jeden datovy bit (2 pul-periody = 1 cyklus).
 *
 * Secte delky 2 po sobe jdoucich pul-period. Pokud soucet cyklu
 * >= 2 * threshold, je to bit 1, jinak bit 0.
 * Pokud nektery pulz prekroci data_max_us, signalizuje konec dat.
 *
 * @param dec Kontext dekoderu. Nesmi byt NULL.
 * @return ZX_BIT_ZERO, ZX_BIT_ONE, ZX_BIT_INCOMPLETE nebo ZX_BIT_ERROR.
 *
 * @post dec->pos je posunuto o 2 pri uspechu.
 */
static en_ZX_BIT_RESULT wav_decode_zx_read_bit (
    st_WAV_ZX_DECODER *dec
) {
    if ( dec->pos + 1 >= dec->seq->count ) {
        return ZX_BIT_INCOMPLETE;
    }

    double p0 = dec->seq->pulses[dec->pos].duration_us;
    double p1 = dec->seq->pulses[dec->pos + 1].duration_us;

    /* kontrola, ze oba pulzy jsou v platnem rozsahu */
    if ( p0 > dec->data_max_us || p1 > dec->data_max_us ) {
        return ZX_BIT_INCOMPLETE;
    }

    dec->pos += 2;

    double cycle = p0 + p1;
    if ( cycle >= 2.0 * dec->threshold_us ) {
        return ZX_BIT_ONE;
    }
    return ZX_BIT_ZERO;
}


/**
 * @brief Navratova hodnota cteni jednoho bajtu.
 */
typedef enum en_ZX_BYTE_RESULT {
    ZX_BYTE_OK = 0,            /**< bajt uspesne precten */
    ZX_BYTE_INCOMPLETE = -1,   /**< konec dat uprostred bajtu */
} en_ZX_BYTE_RESULT;


/**
 * @brief Precte 8 bitu MSB first a sestavi bajt.
 *
 * Nema stop bit (na rozdil od Sharp MZ FM).
 *
 * @param dec Kontext dekoderu. Nesmi byt NULL.
 * @param[out] out_byte Vystupni bajt. Nesmi byt NULL.
 * @return ZX_BYTE_OK pri uspechu, ZX_BYTE_INCOMPLETE pri konci dat.
 *
 * @post dec->pos je posunuto o 16 pul-period pri uspechu (8 bitu * 2).
 */
static en_ZX_BYTE_RESULT wav_decode_zx_read_byte (
    st_WAV_ZX_DECODER *dec,
    uint8_t *out_byte
) {
    uint8_t byte = 0;

    for ( int bit = 7; bit >= 0; bit-- ) {
        en_ZX_BIT_RESULT br = wav_decode_zx_read_bit ( dec );
        if ( br == ZX_BIT_INCOMPLETE || br == ZX_BIT_ERROR ) {
            return ZX_BYTE_INCOMPLETE;
        }
        if ( br == ZX_BIT_ONE ) {
            byte |= ( 1 << bit );
        }
    }

    *out_byte = byte;
    return ZX_BYTE_OK;
}


en_WAV_ANALYZER_ERROR wav_decode_zx_decode_block (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    uint8_t **out_data,
    uint32_t *out_size,
    en_WAV_CRC_STATUS *out_crc,
    uint32_t *out_consumed_until
) {
    if ( !seq || !leader || !out_data || !out_size || !out_crc || !out_consumed_until ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_data = NULL;
    *out_size = 0;
    *out_crc = WAV_CRC_NOT_AVAILABLE;
    *out_consumed_until = 0;

    /* === 1. Inicializace z leaderu === */
    st_WAV_ZX_DECODER dec;
    wav_decode_zx_init ( &dec, seq, leader );

    /* === 2. Nalezeni sync === */
    en_WAV_ANALYZER_ERROR err = wav_decode_zx_find_sync ( &dec );
    if ( err != WAV_ANALYZER_OK ) {
        return err;
    }

    /* === 3. Cteni flag bajtu === */
    uint8_t flag;
    if ( wav_decode_zx_read_byte ( &dec, &flag ) != ZX_BYTE_OK ) {
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /* === 4. Cteni dat podle typu bloku === */
    uint8_t *data = NULL;
    uint32_t data_size = 0;

    if ( flag == WAV_ZX_FLAG_HEADER ) {
        /*
         * Header blok: presne 19 bajtu (flag + 17B header + checksum).
         * Flag uz mame, precteme zbylych 18 bajtu.
         */
        data = ( uint8_t* ) malloc ( WAV_ZX_HEADER_SIZE );
        if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

        data[0] = flag;

        for ( uint32_t i = 1; i < WAV_ZX_HEADER_SIZE; i++ ) {
            if ( wav_decode_zx_read_byte ( &dec, &data[i] ) != ZX_BYTE_OK ) {
                free ( data );
                return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
            }
        }
        data_size = WAV_ZX_HEADER_SIZE;
    } else {
        /*
         * Datovy blok (flag == 0xFF nebo jiny): cteme bajty
         * do dynamickeho bufferu, dokud read_byte neukonci s INCOMPLETE.
         */
        uint32_t capacity = WAV_ZX_DATA_INITIAL_CAPACITY;
        data = ( uint8_t* ) malloc ( capacity );
        if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

        data[0] = flag;
        data_size = 1;

        while ( 1 ) {
            uint8_t byte;
            if ( wav_decode_zx_read_byte ( &dec, &byte ) != ZX_BYTE_OK ) {
                break; /* konec dat */
            }

            /* rozsireni bufferu pokud je treba */
            if ( data_size >= capacity ) {
                uint32_t new_cap = capacity * 2;
                uint8_t *new_data = ( uint8_t* ) realloc ( data, new_cap );
                if ( !new_data ) {
                    free ( data );
                    return WAV_ANALYZER_ERROR_ALLOC;
                }
                data = new_data;
                capacity = new_cap;
            }

            data[data_size++] = byte;
        }
    }

    /* minimalni velikost: alespon flag + 1 bajt */
    if ( data_size < 2 ) {
        free ( data );
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    /* === 5. XOR checksum verifikace === */
    uint8_t xor_check = 0;
    for ( uint32_t i = 0; i < data_size; i++ ) {
        xor_check ^= data[i];
    }

    *out_crc = ( xor_check == 0 ) ? WAV_CRC_OK : WAV_CRC_ERROR;
    *out_data = data;
    *out_size = data_size;
    *out_consumed_until = dec.pos;

    return WAV_ANALYZER_OK;
}
