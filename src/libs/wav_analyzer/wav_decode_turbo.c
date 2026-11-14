/**
 * @file   wav_decode_turbo.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Implementace vrstvy 4b - TURBO dekoder.
 *
 * TURBO signal ma dve casti:
 * - Cast 1: preloader hlavicka + telo (NORMAL 1:1)
 * - Cast 2: skutecna data v TURBO rychlosti (FM, kompletni MZF ramec)
 *
 * Postup dekódování těla (turbo_decode_body_only):
 * 1. Detektor leaderu po celých periodách (nezávislý na rychlosti
 *    i na asymetrii půlvln) najde TURBO LGAP nejvýš TURBO_MAX_GAP pulzů
 *    za předzavaděčem.
 * 2. Hned za ním (nejvýš TURBO_TAPEMARK_MAX_SKIP period) musí být krátký
 *    tapemark (20+20, nejvýš TURBO_TAPEMARK_MAX_LONG dlouhých period, po
 *    periodách) bez ticha mezi; jinak se zkusí další leader do stejné meze.
 *    LGAP a LTM 40+40 cizí nahrávky NORMAL se tak za TURBO nevezme.
 * 3. FM dekodér přečte tělo a checksum (tělo může mít 0 B).
 *
 * Dva typy TURBO předzavaděče, oba čtou jen tělo přes ROM:
 * a) mzftools (fsize=0, loader v comment, parametry v cmnt[1..6]);
 * b) TurboCopy (fsize=90, fstrt=$D400, parametry na $D44D).
 *    Dekoduje wav_decode_turbo_turbocopy_mzf().
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

#include "libs/mzf/mzf.h"
#include "libs/endianity/endianity.h"
#include "wav_decode_turbo.h"
#include "wav_decode_fm.h"
#include "wav_leader.h"
#include "wav_histogram.h"


/**
 * @brief Nejvyšší vzdálenost začátku TURBO LGAP od konce předzavaděče (pulzy).
 *
 * Empirická mez: ve všech záznamech tstdata a z vlastních nástrojů
 * je mezi předzavaděčem a LGAP 1 až 199 půlvln. Leader dál je už další
 * nahrávka (bez meze by 1. soubor vzal data 2. nahrávky).
 */
#define TURBO_MAX_GAP               4000

/** @brief Minimální počet dlouhých i krátkých period krátkého tapemarku (STM 20+20). */
#define TURBO_TAPEMARK_MIN_PERIODS  18

/**
 * @brief Nejvyšší počet dlouhých period tapemarku TURBO.
 *
 * Část TURBO má STM (20 dlouhých period). Mez leží mezi STM a LTM (40):
 * delší běh je hlavička cizí nahrávky NORMAL.
 */
#define TURBO_TAPEMARK_MAX_LONG     30

/**
 * @brief Nejvýš tolik period smí ležet mezi koncem TURBO LGAP a tapemarkem.
 *
 * Detektor leaderu může skončit o pár period dřív (tolerance). Mez váže
 * tapemark na leader, aby se nevzal vzdálený běh dlouhých period.
 * Hodnota 64 je odhad s rezervou [neověřeno na analogových
 * nahrávkách mimo tstdata].
 */
#define TURBO_TAPEMARK_MAX_SKIP     64

/** @brief Půlvlna delší než tato hodnota (us) je ticho mezi nahrávkami, ne součást LGAP a tapemarku. */
#define TURBO_SILENCE_US            20000.0

/**
 * @brief Minimalni pocet pulzu pro detekci TURBO leaderu.
 *
 * TURBO LGAP ma typicky 10000-22000 pulzu. Pouzijeme nizsi prah,
 * aby detektor zachytil i kratsi leadery (napr. body LGAP).
 */
#define TURBO_LEADER_MIN_PULSES     500


/* =========================================================================
 * TURBO body-only dekoder (sdileny pro TurboCopy i mzftools)
 * ========================================================================= */

/**
 * @brief Offset user_params v TurboCopy preloader body (relativne k $D400).
 *
 * TurboCopy TURBO preloader ma na offsetu $4D od $D400:
 *   $D44D: fsize (2B LE)
 *   $D44F: fstrt (2B LE)
 *   $D451: fexec (2B LE)
 */
#define TC_LOADER_PARAMS_OFFSET     0x4D

/** @brief Offset původního cmnt[0..6] programu v těle předzavaděče TurboCopy ($D453, 7 B). */
#define TC_LOADER_CMNT_OFFSET       0x53

/** @brief Minimalni velikost TurboCopy preloader body pro extrakci parametru. */
#define TC_LOADER_MIN_SIZE          ( TC_LOADER_PARAMS_OFFSET + 6 )

/**
 * @brief Offsety metadat v mzftools preloader comment oblasti.
 *
 * mzftools TURBO preloader (fsize=0) uklada metadata v comment:
 *   cmnt[1..2]: user fsize (LE)
 *   cmnt[3..4]: user fstrt (LE)
 *   cmnt[5..6]: user fexec (LE)
 */
#define MZF_LOADER_OFF_SIZE    1
#define MZF_LOADER_OFF_FROM    3
#define MZF_LOADER_OFF_EXEC   5


/**
 * @brief Dekóduje část TURBO (jen tělo, bez hlavičky) za předzavaděčem.
 *
 * Sdílená implementace pro oba typy TURBO předzavaděče (TurboCopy
 * i mzftools). Oba zavaděče upraví ROM a volají RDATA, která čte jen tělo.
 * Leader TURBO se hledá nejvýš TURBO_MAX_GAP pulzů za search_from_pulse,
 * hned za ním musí být krátký tapemark (TURBO_TAPEMARK_MIN_PERIODS až
 * TURBO_TAPEMARK_MAX_LONG dlouhých period, nejvýš TURBO_TAPEMARK_MAX_SKIP
 * period od leaderu) bez ticha delšího než TURBO_SILENCE_US.
 *
 * @param seq Sekvence pulzů.
 * @param search_from_pulse Pozice za předzavaděčem, od které se hledá TURBO LGAP.
 * @param preloader_header Hlavička předzavaděče (ftype, fname).
 * @param user_body_size Velikost uživatelského programu v bajtech; 0 je
 *        platné prázdné tělo (přečte se jen checksum).
 * @param user_fstrt Load adresa programu.
 * @param user_fexec Exec adresa programu.
 * @param[out] out_mzf Výstupní MZF (volající uvolní přes mzf_free). Nesmí být NULL.
 * @param[out] out_body_result Výsledek dekódování těla (data alokuje funkce,
 *             uvolní volající). Může být NULL.
 * @param[out] out_consumed_until Pozice za posledním pulzem. Může být NULL.
 * @param[out] out_turbo_leader Detekovaný TURBO leader. Může být NULL.
 * @param[in,out] rec Obnova neúplného těla (wav_decode_fm_read_body()). Může být NULL.
 * @return WAV_ANALYZER_OK při úspěchu (i při chybě CRC těla, ta je
 *         v out_body_result); WAV_ANALYZER_ERROR_NO_LEADER, když vhodný
 *         leader s tapemarkem v mezích není; WAV_ANALYZER_ERROR_DECODE_DATA
 *         při chybě čtení těla; WAV_ANALYZER_ERROR_ALLOC.
 */
static en_WAV_ANALYZER_ERROR turbo_decode_body_only (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from_pulse,
    const st_MZF_HEADER *preloader_header,
    uint16_t user_body_size,
    uint16_t user_fstrt,
    uint16_t user_fexec,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    st_WAV_LEADER_INFO *out_turbo_leader,
    st_WAV_BODY_RECOVERY *rec
) {
    *out_mzf = NULL;
    if ( out_consumed_until ) *out_consumed_until = 0;
    if ( out_body_result ) memset ( out_body_result, 0, sizeof ( *out_body_result ) );
    if ( out_turbo_leader ) memset ( out_turbo_leader, 0, sizeof ( *out_turbo_leader ) );

    /* fsize 0 je platné prázdné tělo: přečte se jen checksum */

    /*
     * === 1.+2. TURBO LGAP a krátký tapemark ===
     * Leader hledá detektor nezávislý na rychlosti (dřív okno "90 % půlvln
     * pod 150 us" nenašlo 1:1, 3:2 ani TC-1.21 2:1), leader
     * i tapemark po celých periodách kvůli asymetrickým pulzům Intercopy
     * 2800/3200 Bd. Kandidát se
     * přijme, jen když začíná do TURBO_MAX_GAP pulzů za předzavaděčem, hned
     * za ním je krátký tapemark (ne LTM) a mezi nimi není ticho (jinak jde
     * o další nahrávku).
     */
    st_WAV_LEADER_INFO turbo_leader;
    st_WAV_FM_DECODER dec;
    uint32_t from = search_from_pulse;
    for ( ;; ) {
        en_WAV_ANALYZER_ERROR lerr = wav_leader_detect_period ( seq, from, TURBO_LEADER_MIN_PULSES,
                                                                WAV_ANALYZER_DEFAULT_TOLERANCE, &turbo_leader );
        if ( lerr != WAV_ANALYZER_OK ) return WAV_ANALYZER_ERROR_NO_LEADER;
        if ( turbo_leader.start_index > search_from_pulse + TURBO_MAX_GAP ) return WAV_ANALYZER_ERROR_NO_LEADER;
        uint32_t data_start = turbo_leader.start_index + turbo_leader.pulse_count;
        wav_decode_fm_init ( &dec, seq, data_start, turbo_leader.avg_period_us );
        if ( wav_decode_fm_find_tapemark_period ( &dec, TURBO_TAPEMARK_MIN_PERIODS, TURBO_TAPEMARK_MIN_PERIODS,
                                                  TURBO_TAPEMARK_MAX_LONG,
                                                  TURBO_TAPEMARK_MAX_SKIP ) == WAV_ANALYZER_OK ) {
            int silence = 0;
            for ( uint32_t q = turbo_leader.start_index; q < dec.pos && q < seq->count; q++ ) {
                if ( seq->pulses[q].duration_us > TURBO_SILENCE_US ) {
                    silence = 1;
                    break;
                }
            }
            if ( !silence ) break;
        }
        from = data_start;   /* další kandidát za tímto leaderem */
    }

    if ( out_turbo_leader ) {
        *out_turbo_leader = turbo_leader;
    }

    /* synchronizaci 2L přeskočila už wav_decode_fm_find_tapemark_period */

    /* === 3. Dekodovani body dat === */

    uint32_t body_pulse_start = dec.pos;
    uint8_t *body_data = ( uint8_t* ) malloc ( user_body_size ? user_body_size : 1 );
    if ( !body_data ) return WAV_ANALYZER_ERROR_ALLOC;

    /* === 4. Tělo a CRC === */

    en_WAV_CRC_STATUS body_crc;
    uint16_t body_crc_stored = 0, body_crc_computed = 0;
    uint32_t body_end_pos = 0;
    en_WAV_ANALYZER_ERROR err = wav_decode_fm_read_body ( &dec, body_data, user_body_size, rec, &body_crc,
                                                          &body_crc_stored, &body_crc_computed, &body_end_pos );
    if ( err != WAV_ANALYZER_OK ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    if ( body_crc == WAV_CRC_ERROR && !( rec && rec->status ) ) {
        fprintf ( stderr, "WARNING: body CRC mismatch (stored=0x%04X, computed=0x%04X)\n",
                  body_crc_stored, body_crc_computed );
    }

    if ( out_body_result ) {
        out_body_result->format = WAV_TAPE_FORMAT_TURBO;
        out_body_result->data = ( uint8_t* ) malloc ( user_body_size ? user_body_size : 1 );
        if ( out_body_result->data ) {
            memcpy ( out_body_result->data, body_data, user_body_size );
        }
        out_body_result->data_size = user_body_size;
        out_body_result->crc_status = body_crc;
        out_body_result->crc_stored = body_crc_stored;
        out_body_result->crc_computed = body_crc_computed;
        out_body_result->pulse_start = body_pulse_start;
        out_body_result->pulse_end = body_end_pos;
        out_body_result->is_header = 0;
    }

    /* === 5. Sestaveni MZF === */

    st_MZF *mzf = ( st_MZF* ) calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( body_data );
        return WAV_ANALYZER_ERROR_ALLOC;
    }

    memcpy ( &mzf->header, preloader_header, sizeof ( st_MZF_HEADER ) );
    mzf->header.fsize = user_body_size;
    mzf->header.fstrt = user_fstrt;
    mzf->header.fexec = user_fexec;

    mzf->body = body_data;
    mzf->body_size = user_body_size;

    *out_mzf = mzf;

    if ( out_consumed_until ) {
        *out_consumed_until = body_end_pos;
    }

    return WAV_ANALYZER_OK;
}


/* =========================================================================
 * Verejne API - TurboCopy TURBO (metadata z preloader body)
 * ========================================================================= */

en_WAV_ANALYZER_ERROR wav_decode_turbo_turbocopy_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from_pulse,
    const st_MZF_HEADER *preloader_header,
    const uint8_t *preloader_body,
    uint32_t preloader_body_size,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    st_WAV_LEADER_INFO *out_turbo_leader,
    st_WAV_BODY_RECOVERY *rec
) {
    if ( !seq || !out_mzf || !preloader_header || !preloader_body ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( preloader_body_size < TC_LOADER_MIN_SIZE ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* extrakce metadat z TurboCopy preloader body ($D44D+) */
    const uint8_t *params = preloader_body + TC_LOADER_PARAMS_OFFSET;
    uint16_t body_size = ( uint16_t ) params[0] | ( ( uint16_t ) params[1] << 8 );
    uint16_t fstrt     = ( uint16_t ) params[2] | ( ( uint16_t ) params[3] << 8 );
    uint16_t fexec     = ( uint16_t ) params[4] | ( ( uint16_t ) params[5] << 8 );

    /*
     * Hlavička předzavaděče má v cmnt[0..6] signaturu TurboCopy; původních 7 B
     * komentáře nese tělo předzavaděče hned za fsize/fstrt/fexec (53h-59h).
     */
    st_MZF_HEADER hdr;
    memcpy ( &hdr, preloader_header, sizeof ( hdr ) );
    if ( preloader_body_size >= TC_LOADER_CMNT_OFFSET + 7 ) {
        memcpy ( &hdr.cmnt[0], preloader_body + TC_LOADER_CMNT_OFFSET, 7 );
    }

    return turbo_decode_body_only (
        seq, search_from_pulse, &hdr,
        body_size, fstrt, fexec,
        out_mzf, out_body_result, out_consumed_until, out_turbo_leader, rec
    );
}


/* =========================================================================
 * Verejne API - mzftools TURBO (metadata z preloader header comment)
 * ========================================================================= */

en_WAV_ANALYZER_ERROR wav_decode_turbo_mzftools_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from_pulse,
    const st_MZF_HEADER *preloader_header,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    st_WAV_LEADER_INFO *out_turbo_leader,
    st_WAV_BODY_RECOVERY *rec
) {
    if ( !seq || !out_mzf || !preloader_header ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* extrakce metadat z mzftools preloader comment oblasti */
    uint16_t body_size, fstrt, fexec;
    memcpy ( &body_size, &preloader_header->cmnt[MZF_LOADER_OFF_SIZE], 2 );
    memcpy ( &fstrt, &preloader_header->cmnt[MZF_LOADER_OFF_FROM], 2 );
    memcpy ( &fexec, &preloader_header->cmnt[MZF_LOADER_OFF_EXEC], 2 );

    /* cmnt hodnoty jsou v originalni LE endianite (Z80 poradi) */
    body_size = endianity_bswap16_LE ( body_size );
    fstrt = endianity_bswap16_LE ( fstrt );
    fexec = endianity_bswap16_LE ( fexec );

    return turbo_decode_body_only (
        seq, search_from_pulse, preloader_header,
        body_size, fstrt, fexec,
        out_mzf, out_body_result, out_consumed_until, out_turbo_leader, rec
    );
}
