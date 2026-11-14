/**
 * @file   wav_decode_bsd.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.3.0
 * @brief  Implementace vrstvy 4d - BSD dekoder.
 *
 * BSD signal: hlavicka (fsize=0) nasledovana sekvenci chunku.
 * Kazdy chunk ma vlastni kratky tapemark (STM) a obsahuje
 * 258 bajtu (2B ID + 256B data) s checksumem.
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

#include "libs/endianity/endianity.h"

#include "wav_decode_bsd.h"
#include "wav_decode_fm.h"


/**
 * @brief Pocatecni kapacita bufferu pro akumulaci dat chunku.
 *
 * Buffer roste dynamicky - zacina na 4 chunky (4 * 256 = 1024 B)
 * a zdvojnasobuje se dle potreby.
 */
#define BSD_BODY_INITIAL_CAPACITY   ( 4 * WAV_BSD_CHUNK_DATA_SIZE )


en_WAV_ANALYZER_ERROR wav_decode_bsd_decode_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    const st_WAV_LEADER_INFO *leader,
    const st_MZF_HEADER *header,
    uint32_t search_from_pulse,
    int allow_partial,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    uint32_t *out_recovery_status,
    st_WAV_BSD_INFO *out_info
) {
    if ( !seq || !leader || !header || !out_mzf ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_mzf = NULL;
    st_WAV_BSD_INFO info = { 0, 0, 0, 0 };
    if ( out_info ) *out_info = info;
    if ( out_consumed_until ) *out_consumed_until = 0;
    if ( out_recovery_status ) *out_recovery_status = WAV_RECOVERY_NONE;

    /* === 1. Inicializace FM dekoderu === */

    /*
     * Kalibrujeme prah z leaderu hlavicky (BSD pouziva
     * stejnou rychlost pro hlavicku i chunky).
     */
    st_WAV_FM_DECODER dec;
    wav_decode_fm_init ( &dec, seq, search_from_pulse, leader->avg_period_us );

    /* === 2. Dynamicky buffer pro akumulaci dat chunku === */

    uint8_t *body_data = ( uint8_t* ) malloc ( BSD_BODY_INITIAL_CAPACITY );
    if ( !body_data ) return WAV_ANALYZER_ERROR_ALLOC;

    uint32_t body_size = 0;
    uint32_t body_capacity = BSD_BODY_INITIAL_CAPACITY;
    uint32_t chunk_count = 0;

    /* CRC prvního kusu bez platné kopie (hlásí se chyba kteréhokoli kusu) */
    uint16_t first_err_stored = 0, first_err_computed = 0;
    uint32_t first_chunk_pulse_start = 0;

    int found_terminator = 0;

    /* === 3. Dekodovani chunku === */

    en_WAV_ANALYZER_ERROR err;

    /*
     * Pozice za poslednim uspesne prectenim chunkem.
     * Pri partial recovery se dec.pos vraci na tuto pozici,
     * aby find_tapemark nepozrel leadery nasledujicich souboru.
     */
    uint32_t last_good_pos = dec.pos;

    for ( ;; ) {
        /* ochrana proti nekonecne smycce */
        if ( chunk_count >= WAV_BSD_MAX_CHUNKS ) {
            free ( body_data );
            return WAV_ANALYZER_ERROR_BUFFER_OVERFLOW;
        }

        /* hledame kratky tapemark (STM: 20 LONG + 20 SHORT) */
        err = wav_decode_fm_find_tapemark ( &dec, WAV_TAPEMARK_SHORT );
        if ( err != WAV_ANALYZER_OK ) {
            /* zadny dalsi tapemark - konec chunku */
            break;
        }

        /* sync blok (2L = max 4 LONG pul-periody) */
        {
            int sync_skip = 0;
            while ( dec.pos < seq->count &&
                    seq->pulses[dec.pos].duration_us >= dec.threshold_us &&
                    sync_skip < 4 ) {
                dec.pos++;
                sync_skip++;
            }
        }

        if ( chunk_count == 0 ) {
            first_chunk_pulse_start = dec.pos;
        }
        uint32_t chunk_data_start = dec.pos;   /* první datová půlvlna 1. kopie kusu */

        /* precteme 258B chunk (2B ID + 256B data) */
        uint8_t chunk[WAV_BSD_CHUNK_SIZE];
        dec.crc_accumulator = 0;

        err = wav_decode_fm_read_block ( &dec, chunk, WAV_BSD_CHUNK_SIZE );
        if ( err != WAV_ANALYZER_OK ) {
            if ( allow_partial && chunk_count > 0 ) {
                /*
                 * Recovery mod: zachovame dosavadni chunky.
                 * Vracime dec.pos na konec posledniho uspesneho chunku,
                 * aby consumed_until neukazoval za falsny tapemark
                 * (ktery muze byt leader nasledujiciho souboru).
                 */
                dec.pos = last_good_pos;
                if ( out_recovery_status )
                    *out_recovery_status = WAV_RECOVERY_BSD_INCOMPLETE;
                break;
            }
            free ( body_data );
            return WAV_ANALYZER_ERROR_DECODE_DATA;
        }

        /*
         * CRC každého kusu. BASIC 1Z-016 zapisuje za kusem
         * kopii za oddělovačem 256 S; při chybě CRC
         * 1. kopie se použije kopie s platným checksumem. Konec kopie se
         * hledá vždy, aby se oddělovač ani kopie nehlásily jako další kus.
         * Nahrávka bez kopií (basic-bsd.wav) kopii nenajde: hledání skončí
         * na tónu dalšího kusu.
         */
        en_WAV_CRC_STATUS crc;
        uint16_t crc_stored = 0, crc_computed = 0;
        wav_decode_fm_verify_checksum ( &dec, &crc, &crc_stored, &crc_computed );
        /* počty se přičtou až po kontrole ID (zahozený cizí kus se nepočítá) */
        int chunk_from_copy = 0;
        int chunk_crc_error = 0;
        {
            uint8_t *copy = NULL;
            en_WAV_CRC_STATUS copy_crc = WAV_CRC_NOT_AVAILABLE;
            uint32_t copy_end = dec.pos;
            en_WAV_ANALYZER_ERROR cerr = wav_decode_fm_try_copy2 ( seq, chunk_data_start, dec.threshold_us,
                                                                   WAV_BSD_CHUNK_SIZE, &copy, &copy_crc,
                                                                   NULL, NULL, &copy_end );
            if ( cerr == WAV_ANALYZER_ERROR_ALLOC ) {
                free ( body_data );
                return WAV_ANALYZER_ERROR_ALLOC;
            }
            if ( cerr == WAV_ANALYZER_OK && copy_end > dec.pos ) dec.pos = copy_end;
            if ( crc != WAV_CRC_OK ) {
                if ( copy_crc == WAV_CRC_OK && copy ) {
                    memcpy ( chunk, copy, WAV_BSD_CHUNK_SIZE );
                    chunk_from_copy = 1;
                } else {
                    chunk_crc_error = 1;
                }
            }
            free ( copy );
        }

        /* extrakce chunk ID (prvni 2 bajty, LE) */
        uint16_t chunk_id;
        memcpy ( &chunk_id, chunk, 2 );
        chunk_id = endianity_bswap16_LE ( chunk_id );

        /*
         * Validace chunk ID sekvence: realne BSD chunky maji
         * sekvencni ID (0x0000, 0x0001, ...) nebo terminator (0xFFFF).
         * Pokud je ID mimo ocekavanou sekvenci, jedna se o data
         * z jineho souboru na pasce (falsny tapemark).
         */
        uint16_t expected_id = ( uint16_t ) chunk_count;

        /*
         * Obnova: kus s chybou CRC a ID, které se od očekávaného liší nejvýš o
         * WAV_BSD_ID_MAX_FLIPS bitů, patří na očekávané místo (poškozené je i ID; jiné ID
         * znamená cizí data); kus s platným CRC a ID o málo vyšším znamená chybějící kusy.
         */
        uint32_t gap = 0;
        if ( allow_partial && chunk_id != expected_id && chunk_id != WAV_BSD_LAST_CHUNK_ID ) {
            int flips = 0;
            for ( uint16_t x = ( uint16_t ) ( chunk_id ^ expected_id ); x; x &= ( uint16_t ) ( x - 1 ) ) flips++;
            if ( chunk_crc_error ) {
                if ( flips <= WAV_BSD_ID_MAX_FLIPS ) chunk_id = expected_id;
            } else if ( chunk_id > expected_id && ( uint32_t ) ( chunk_id - expected_id ) <= WAV_BSD_MAX_GAP
                        && chunk_count + ( uint32_t ) ( chunk_id - expected_id ) < WAV_BSD_MAX_CHUNKS ) {
                gap = ( uint32_t ) ( chunk_id - expected_id );
                expected_id = chunk_id;
            }
        }

        if ( chunk_id != expected_id && chunk_id != WAV_BSD_LAST_CHUNK_ID ) {
            /* chunk ID mimo sekvenci - konec BSD dat */
            dec.pos = last_good_pos;
            if ( out_recovery_status )
                *out_recovery_status |= WAV_RECOVERY_BSD_INCOMPLETE;
            break;
        }

        /* chybějící kusy: nuly na jejich místě */
        if ( gap ) {
            uint32_t need = body_size + ( gap + 1 ) * WAV_BSD_CHUNK_DATA_SIZE;
            if ( need > body_capacity ) {
                uint32_t new_cap = body_capacity;
                while ( new_cap < need ) new_cap *= 2;
                uint8_t *new_data = ( uint8_t* ) realloc ( body_data, new_cap );
                if ( !new_data ) {
                    free ( body_data );
                    return WAV_ANALYZER_ERROR_ALLOC;
                }
                body_data = new_data;
                body_capacity = new_cap;
            }
            memset ( &body_data[body_size], 0, gap * WAV_BSD_CHUNK_DATA_SIZE );
            body_size += gap * WAV_BSD_CHUNK_DATA_SIZE;
            chunk_count += gap;
            info.gap_chunks += gap;
            if ( out_recovery_status ) *out_recovery_status |= WAV_RECOVERY_BSD_GAPS;
        }

        /* zvetsime buffer pokud je potreba */
        if ( body_size + WAV_BSD_CHUNK_DATA_SIZE > body_capacity ) {
            uint32_t new_cap = body_capacity * 2;
            uint8_t *new_data = ( uint8_t* ) realloc ( body_data, new_cap );
            if ( !new_data ) {
                free ( body_data );
                return WAV_ANALYZER_ERROR_ALLOC;
            }
            body_data = new_data;
            body_capacity = new_cap;
        }

        /* pripojime datovou cast chunku (256 bajtu za ID) */
        /* kus je přijatý: započítat použitou kopii nebo chybu CRC */
        if ( chunk_from_copy ) info.from_copy++;
        if ( chunk_crc_error ) {
            if ( info.crc_errors == 0 ) {
                first_err_stored = crc_stored;
                first_err_computed = crc_computed;
            }
            info.crc_errors++;
        }

        memcpy ( &body_data[body_size], &chunk[2], WAV_BSD_CHUNK_DATA_SIZE );
        body_size += WAV_BSD_CHUNK_DATA_SIZE;
        chunk_count++;

        /* aktualizujeme pozici za poslednim uspesnym chunkem */
        last_good_pos = dec.pos;

        /* chunk ID == 0xFFFF znamena posledni chunk */
        if ( chunk_id == WAV_BSD_LAST_CHUNK_ID ) {
            found_terminator = 1;
            break;
        }
    }

    /* detekce chybejiciho terminatoru (informacni, i bez allow_partial) */
    if ( chunk_count > 0 && !found_terminator ) {
        if ( out_recovery_status )
            *out_recovery_status |= WAV_RECOVERY_BSD_INCOMPLETE;
    }

    /* === 4. Sestaveni MZF struktury === */

    if ( chunk_count == 0 ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    /*
     * Bez terminatoru a bez allow_partial zahodime data.
     * Toto zachovava stavajici chovani (bez --recover-bsd).
     */
    if ( !found_terminator && !allow_partial ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    st_MZF *mzf = ( st_MZF* ) calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    /* kopirujeme hlavicku a aktualizujeme fsize */
    memcpy ( &mzf->header, header, sizeof ( st_MZF_HEADER ) );
    mzf->header.fsize = ( uint16_t ) body_size;

    mzf->body = body_data;
    mzf->body_size = body_size;

    *out_mzf = mzf;

    /* výsledek těla: chyba, když některý kus nemá platnou kopii */
    info.chunks = chunk_count;
    if ( out_info ) *out_info = info;
    if ( out_body_result ) {
        out_body_result->format = WAV_TAPE_FORMAT_BSD;
        out_body_result->data = NULL;   /* data jsou primo v mzf->body */
        out_body_result->data_size = body_size;
        out_body_result->crc_status = info.crc_errors ? WAV_CRC_ERROR : WAV_CRC_OK;
        out_body_result->crc_stored = first_err_stored;
        out_body_result->crc_computed = first_err_computed;
        out_body_result->pulse_start = first_chunk_pulse_start;
        out_body_result->pulse_end = dec.pos;
        out_body_result->is_header = 0;
    }

    /* === 5. Consumed until === */

    if ( out_consumed_until ) {
        /*
         * Pokud chybi terminator, pouzijeme last_good_pos (pozici za
         * poslednim uspesne prectenim chunkem). Jinak find_tapemark
         * mohl skenovat daleko za BSD daty a spotrebovat leadery
         * nasledujicich souboru na pasce.
         */
        *out_consumed_until = found_terminator ? dec.pos : last_good_pos;
    }

    return WAV_ANALYZER_OK;
}
