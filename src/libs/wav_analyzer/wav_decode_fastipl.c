/**
 * @file   wav_decode_fastipl.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.5.0
 * @brief  Implementace vrstvy 4c - FASTIPL dekoder.
 *
 * FASTIPL signal je standardni dvou-blokovy MZF zaznam:
 * - Blok 1 (header): LGAP + LTM + $BB hlavicka(128B) + CRC
 *   Zapsan pri NORMAL rychlosti (1:1).
 * - Blok 2 (body): LGAP + STM + body data + CRC
 *   Zapsan pri TURBO rychlosti (1:1 az 8:3).
 *
 * $BB hlavicka obsahuje embedded loader a realne parametry
 * (fsize, fstrt, fexec) na nestandardnich offsetech ($1A-$1F).
 * Standardni fsize pole ($12-$13) obsahuje bajty loader kodu.
 *
 * Dekoder extrahuje realne parametry z $BB hlavicky, najde body
 * LGAP leader a dekoduje telo pomoci FM dekoderu.
 *
 * Intercopy pise body LGAP kratsi nez header LGAP (5500 vs 11000
 * pulzu) a body tapemark je STM (20+20), ne LTM (40+40).
 *
 * Leader a tapemark těla se hledají po celých periodách (dvojicích
 * půlvln): Intercopy má při 2800/3200 Bd asymetrické půlvlny, které se
 * po kvantizaci na 44100 Hz překrývají, periody ale zůstávají oddělené.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/endianity/endianity.h"

#include "wav_decode_fastipl.h"
#include "wav_decode_fm.h"
#include "wav_leader.h"


/**
 * @brief Minimalni pocet pulzu pro detekci body LGAP leaderu.
 */
#define FASTIPL_DATA_LEADER_MIN_PULSES  500

/**
 * @brief FM threshold faktor pro FASTIPL body pri turbo rychlostech.
 *
 * Prah se porovnává s průměrem půlvlny v dvojici (tapemark po periodách,
 * wav_decode_fm_find_tapemark_period) a při čtení bitů. Nižší než globální
 * WAV_ANALYZER_FM_THRESHOLD_FACTOR (1.6) kvůli asymetrickému duty cycle
 * při vysokých rychlostech (2800+ Bd).
 */
#define FASTIPL_FM_THRESHOLD_FACTOR     1.4

/** @brief Minimální počet dlouhých i krátkých period tapemarku těla (STM 20+20). */
#define FASTIPL_TAPEMARK_MIN_PERIODS    18

/** @brief Offset fname v MZF hlavicce (1 bajt za ftype). */
#define MZF_FNAME_OFFSET    1
/** @brief Delka fname v MZF hlavicce. */
#define MZF_FNAME_LENGTH    17


en_WAV_ANALYZER_ERROR wav_decode_fastipl_decode_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    const uint8_t *bb_header_raw,
    uint32_t search_from_pulse,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    st_WAV_LEADER_INFO *out_data_leader,
    st_WAV_BODY_RECOVERY *rec
) {
    if ( !seq || !bb_header_raw || !out_mzf ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_mzf = NULL;
    if ( out_consumed_until ) *out_consumed_until = 0;
    if ( out_data_leader ) memset ( out_data_leader, 0, sizeof ( *out_data_leader ) );

    /* === 1. Extrakce realnych parametru z $BB hlavicky === */

    uint16_t real_fsize, real_fstrt, real_fexec;
    memcpy ( &real_fsize, &bb_header_raw[WAV_FASTIPL_OFF_FSIZE], 2 );
    memcpy ( &real_fstrt, &bb_header_raw[WAV_FASTIPL_OFF_FSTRT], 2 );
    memcpy ( &real_fexec, &bb_header_raw[WAV_FASTIPL_OFF_FEXEC], 2 );
    real_fsize = endianity_bswap16_LE ( real_fsize );
    real_fstrt = endianity_bswap16_LE ( real_fstrt );
    real_fexec = endianity_bswap16_LE ( real_fexec );

    /* === 2. Hledani body LGAP leaderu === */

    /*
     * search_from_pulse = hdr_res.pulse_end (za header CRC).
     * Prvni leader je body LGAP - kratsi nez header LGAP
     * (5500 pulzu = 11000 pul-period vs 11000 pulzu = 22000 pul-period).
     *
     * Body LGAP muze byt pri NORMAL rychlosti (1:1) nebo
     * pri turbo rychlosti (2:1 az 8:3) - zavisi na nastaveni
     * rychlosti pri zapisu.
     */
    st_WAV_LEADER_INFO data_leader;
    /* leader po celých periodách: asymetrické pulzy Intercopy 2800/3200 Bd */
    en_WAV_ANALYZER_ERROR err = wav_leader_detect_period (
                                    seq, search_from_pulse,
                                    FASTIPL_DATA_LEADER_MIN_PULSES,
                                    WAV_ANALYZER_DEFAULT_TOLERANCE,
                                    &data_leader
                                );

    if ( err != WAV_ANALYZER_OK ) {
        return WAV_ANALYZER_ERROR_NO_LEADER;
    }

    /* vratime body LGAP leader info volajicimu (pro speed_class urceni) */
    if ( out_data_leader ) {
        *out_data_leader = data_leader;
    }

    /* === 3. Dekodovani body dat === */

    /*
     * Body blok: LGAP + STM(20+20) + sync 2L + body(real_fsize B) + CRC.
     * Tapemark se hledá po celých periodách (min 18 + 18), data čte FM
     * dekodér (už po periodách). Fáze dvojic se odvozuje od konce leaderu;
     * když tělo ve fázi 0 nemá platný checksum, zkusí se posun o 1 půlvlnu.
     * Když selžou obě, zůstane výsledek fáze 0.
     */
    uint32_t data_start = data_leader.start_index + data_leader.pulse_count;
    uint8_t *body_data = NULL;
    en_WAV_CRC_STATUS body_crc_status = WAV_CRC_NOT_AVAILABLE;
    uint16_t body_crc_stored = 0, body_crc_computed = 0;
    uint32_t body_pulse_start = 0;
    uint32_t end_pos = 0;
    int have_result = 0;
    st_WAV_BODY_RECOVERY best_rec = { 0, 0, 0, 0 };
    err = WAV_ANALYZER_ERROR_DECODE_TAPEMARK;

    for ( uint32_t phase = 0; phase < 2; phase++ ) {
        st_WAV_FM_DECODER dec;
        wav_decode_fm_init ( &dec, seq, data_start + phase, data_leader.avg_period_us );
        dec.threshold_us = data_leader.avg_period_us * FASTIPL_FM_THRESHOLD_FACTOR;
        if ( wav_decode_fm_find_tapemark_period ( &dec, FASTIPL_TAPEMARK_MIN_PERIODS,
                                                  FASTIPL_TAPEMARK_MIN_PERIODS, 0, 0 ) != WAV_ANALYZER_OK ) {
            continue;
        }
        uint32_t phase_body_start = dec.pos;
        uint8_t *phase_data = NULL;
        en_WAV_CRC_STATUS st = WAV_CRC_NOT_AVAILABLE;
        uint16_t cs = 0, cc = 0;
        uint32_t phase_end = dec.pos;
        st_WAV_BODY_RECOVERY prec = { rec ? rec->enable : 0, 0, 0, 0 };
        if ( real_fsize > 0 ) {
            phase_data = ( uint8_t* ) malloc ( real_fsize );
            if ( !phase_data ) {
                free ( body_data );
                return WAV_ANALYZER_ERROR_ALLOC;
            }
            if ( wav_decode_fm_read_body ( &dec, phase_data, real_fsize, &prec, &st, &cs, &cc, &phase_end ) != WAV_ANALYZER_OK ) {
                free ( phase_data );
                err = WAV_ANALYZER_ERROR_DECODE_DATA;
                continue;
            }
        }
        if ( !have_result || st == WAV_CRC_OK ) {
            /* první výsledek (fáze 0, nebo 1 když 0 selhala), nebo platný checksum */
            free ( body_data );
            body_data = phase_data;
            body_crc_status = st;
            body_crc_stored = cs;
            body_crc_computed = cc;
            body_pulse_start = phase_body_start;
            end_pos = phase_end;
            best_rec = prec;
            have_result = 1;
        } else {
            free ( phase_data );
        }
        if ( real_fsize == 0 || st == WAV_CRC_OK ) break;
    }

    if ( !have_result ) {
        return err;
    }

    if ( rec ) {
        rec->status = best_rec.status;
        rec->read_bytes = best_rec.read_bytes;
        rec->expected_bytes = best_rec.expected_bytes;
    }

    if ( real_fsize > 0 && out_body_result ) {
        out_body_result->format = WAV_TAPE_FORMAT_FASTIPL;
        out_body_result->crc_status = body_crc_status;
        out_body_result->crc_stored = body_crc_stored;
        out_body_result->crc_computed = body_crc_computed;
        out_body_result->pulse_start = body_pulse_start;
        out_body_result->pulse_end = end_pos;
        out_body_result->is_header = 0;
    }

    /* === 4. Sestaveni MZF struktury === */

    st_MZF *mzf = ( st_MZF* ) calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    mzf->header.ftype = WAV_FASTIPL_DEFAULT_FTYPE;
    memcpy ( &mzf->header.fname, &bb_header_raw[MZF_FNAME_OFFSET], MZF_FNAME_LENGTH );
    mzf->header.fsize = real_fsize;
    mzf->header.fstrt = real_fstrt;
    mzf->header.fexec = real_fexec;

    mzf->body = body_data;
    mzf->body_size = real_fsize;

    *out_mzf = mzf;

    if ( out_consumed_until ) {
        *out_consumed_until = end_pos;
    }

    return WAV_ANALYZER_OK;
}
