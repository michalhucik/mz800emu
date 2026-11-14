/**
 * @file   wav_decode_cpmtape.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.2.0
 * @brief  Implementace vrstvy 4e2 - dekodér CPM-TAPE (Manchester).
 *
 * Postup:
 * 1. Pilot: pulzy (půlvlny) v poměru 2:1:1:2 půlek bitu, opakovaně
 *    (vzor "011" = L H H L H L, sousední stejné úrovně splývají). Z pilotu
 *    se určí délka půlky bitu.
 * 2. Pulzy se rozloží na půlky bitu (1 nebo 2; delší pulz = konec bloku),
 *    dvojice půlek dají bit. Zarovnání dvojic: obě půlky bitu musí mít
 *    různou úroveň. Polarita: vzor pilotu musí dát 0,1,1 (1,0,0 = obrácený).
 * 3. Konec pilotu: tři jedničky za sebou (vzor končí 1,1 a sync je bit 1).
 * 4. Flag, data a checksum (16bitový součet dat bez flagu, L, H) LSB první.
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

#include "wav_decode_cpmtape.h"


/** @brief Nejvíc pulzů mezi koncem hlavičky a pilotem těla (jinak jde o jinou nahrávku). */
#define CPMTAPE_BODY_SEARCH_MAX  50000u
/** @brief Délka hlavičky v bajtech. */
#define CPMTAPE_HEADER_SIZE      128u


/**
 * @brief Čtečka půlek bitu ze sekvence pulzů.
 *
 * @invariant left > 0 jen když pos > 0 (zbytek pulzu pos - 1)
 */
typedef struct st_MRD {
    const st_WAV_PULSE_SEQUENCE *seq;   /**< sekvence pulzů */
    uint32_t pos;                       /**< další nepřečtený pulz */
    uint32_t left;                      /**< nepřečtené půlky pulzu pos - 1 */
    int level;                          /**< úroveň pulzu pos - 1 */
    double h;                           /**< délka půlky bitu (µs) */
    int pol;                            /**< úroveň první půlky bitu 1 */
} st_MRD;


/**
 * @brief Počet půlek bitu v pulzu.
 * @param dur Délka pulzu (µs). @param h Půlka bitu (µs).
 * @return 1, 2, nebo 0 (mimo Manchester: příliš krátký, nebo 3 a víc půlek).
 */
static int halves_of ( double dur, double h ) {
    double r = dur / h;
    if ( r < 0.5 ) return 0;
    if ( r < 1.5 ) return 1;
    if ( r < 2.5 ) return 2;
    return 0;
}


/**
 * @brief Přečte další půlku bitu.
 * @param r Čtečka. @param[out] level Úroveň půlky.
 * @return 0 = OK, -1 = konec bloku (pulz mimo Manchester nebo konec sekvence; pos zůstane na něm).
 */
static int next_half ( st_MRD *r, int *level ) {
    if ( !r->left ) {
        if ( r->pos >= r->seq->count ) return -1;
        const st_WAV_PULSE *p = &r->seq->pulses[r->pos];
        int n = halves_of ( p->duration_us, r->h );
        if ( !n ) return -1;
        r->left = ( uint32_t ) n;
        r->level = p->level ? 1 : 0;
        r->pos++;
    }
    r->left--;
    *level = r->level;
    return 0;
}


/**
 * @brief Přečte bit (dvě půlky různé úrovně).
 * @param r Čtečka. @param[out] bit Hodnota.
 * @return 0 = OK, -1 = konec bloku, -2 = porušený Manchester (stejné úrovně půlek).
 */
static int read_bit ( st_MRD *r, int *bit ) {
    int a, b;
    if ( next_half ( r, &a ) ) return -1;
    if ( next_half ( r, &b ) ) {
        /*
         * Druhá půlka posledního bitu bloku splyne s pauzou (Pezik nemá koncový
         * bit): delší pulz opačné úrovně, nebo konec signálu. Pulz zůstane
         * nepřečtený (pos na něm), další čtení skončí koncem bloku.
         */
        if ( r->pos >= r->seq->count || ( r->seq->pulses[r->pos].level ? 1 : 0 ) != a ) {
            *bit = ( a == r->pol ) ? 1 : 0;
            return 0;
        }
        return -1;
    }
    if ( a == b ) return -2;
    *bit = ( a == r->pol ) ? 1 : 0;
    return 0;
}


/**
 * @brief Přečte bajt LSB první.
 * @param r Čtečka. @param[out] v Bajt.
 * @return 0 = OK, jinak chyba read_bit().
 */
static int read_byte ( st_MRD *r, uint8_t *v ) {
    uint8_t x = 0;
    for ( int i = 0; i < 8; i++ ) {
        int b, e = read_bit ( r, &b );
        if ( e ) return e;
        x |= ( uint8_t ) ( b << i );
    }
    *v = x;
    return 0;
}


/**
 * @brief Najde pilot: aspoň WAV_CPMTAPE_MIN_PATTERNS cyklů pulzů 2:1:1:2 půlek.
 * @param seq Sekvence. @param from Začátek hledání. @param limit Konec hledání (index pulzu, kde smí pilot začít).
 * @param[out] start Index prvního pulzu pilotu. @param[out] h Průměrná půlka bitu (µs).
 * @return 1 = nalezen, 0 = ne.
 */
static int find_pilot ( const st_WAV_PULSE_SEQUENCE *seq, uint32_t from, uint32_t limit, uint32_t *start, double *h ) {
    static const int cyc[4] = { 2, 1, 1, 2 };
    uint32_t i = from;
    while ( i < limit && i + 8 <= seq->count ) {
        /* odhad půlky: průměr 4 nejkratších z 8 pulzů (v cyklu jsou 4 jednopůlkové) */
        double v[8];
        for ( int k = 0; k < 8; k++ ) v[k] = seq->pulses[i + k].duration_us;
        for ( int a = 0; a < 8; a++ ) {
            for ( int b = a + 1; b < 8; b++ ) {
                if ( v[b] < v[a] ) {
                    double t = v[a];
                    v[a] = v[b];
                    v[b] = t;
                }
            }
        }
        double hh = ( v[0] + v[1] + v[2] + v[3] ) / 4.0;
        int n0[4];
        int phase = -1;
        for ( int k = 0; hh > 0 && k < 4; k++ ) n0[k] = halves_of ( seq->pulses[i + k].duration_us, hh );
        for ( int p = 0; hh > 0 && p < 4 && phase < 0; p++ ) {
            if ( n0[0] == cyc[p] && n0[1] == cyc[( p + 1 ) % 4] && n0[2] == cyc[( p + 2 ) % 4] && n0[3] == cyc[( p + 3 ) % 4] ) {
                phase = p;
            }
        }
        if ( phase < 0 ) {
            i++;
            continue;
        }
        uint32_t runs = 0, units = 0;
        double sum = 0;
        uint32_t j = i;
        while ( j < seq->count ) {
            int n = halves_of ( seq->pulses[j].duration_us, hh );
            if ( n != cyc[( phase + runs ) % 4] ) break;
            sum += seq->pulses[j].duration_us;
            units += ( uint32_t ) n;
            runs++;
            j++;
            if ( ( runs & 15 ) == 0 ) hh = sum / units;   /* průběžné upřesnění */
        }
        if ( runs >= 4u * WAV_CPMTAPE_MIN_PATTERNS ) {
            *start = i;
            *h = sum / units;
            return 1;
        }
        i += runs ? runs : 1;
    }
    return 0;
}


/**
 * @brief Od začátku pilotu zarovná půlky na bity, určí polaritu a přečte pilot a sync.
 * @param r Čtečka (seq a h nastavené; pos = začátek pilotu). @param[out] patterns Počet vzorů pilotu.
 * @return 0 = za sync, jinak chyba.
 */
static int sync_pilot ( st_MRD *r, uint32_t *patterns ) {
    /* zarovnání: posun 0 nebo 1 půlky, 8 dvojic musí mít různé úrovně */
    st_MRD base = *r;
    int off = -1;
    for ( int o = 0; o < 2 && off < 0; o++ ) {
        st_MRD t = base;
        int lv, ok = 1;
        for ( int k = 0; k < o && ok; k++ ) ok = next_half ( &t, &lv ) == 0;
        for ( int k = 0; k < 8 && ok; k++ ) {
            int a, b;
            ok = next_half ( &t, &a ) == 0 && next_half ( &t, &b ) == 0 && a != b;
        }
        if ( ok ) off = o;
    }
    if ( off < 0 ) return -1;
    *r = base;
    int lv;
    for ( int k = 0; k < off; k++ ) next_half ( r, &lv );
    /* polarita: vzor 0,1,1 má víc jedniček než 1,0,0 */
    st_MRD t = *r;
    t.pol = 1;
    int ones = 0;
    for ( int k = 0; k < 12; k++ ) {
        int b;
        if ( read_bit ( &t, &b ) ) return -1;
        ones += b;
    }
    r->pol = ( ones >= 6 ) ? 1 : 0;
    /* konec pilotu: tři jedničky za sebou (vzor ..., 1, 1 a sync 1) */
    int b1 = -1, b2 = -1;
    uint32_t zeros = 0;
    for ( ;; ) {
        int b, e = read_bit ( r, &b );
        if ( e ) return e;
        if ( b == 0 ) zeros++;
        if ( b == 1 && b1 == 1 && b2 == 1 ) break;
        b2 = b1;
        b1 = b;
    }
    *patterns = zeros;
    return ( zeros >= WAV_CPMTAPE_MIN_PATTERNS ) ? 0 : -1;
}


/**
 * @brief Přečte blok: flag, data a checksum.
 * @param r Čtečka za sync. @param flag Očekávaný flag. @param data Výstup (size B). @param size Délka.
 * @param[out] stored Checksum z pásky. @param[out] computed Vypočtený checksum.
 * @param[out] out_read Počet přečtených datových bajtů, i při chybě. Může být NULL.
 * @return WAV_ANALYZER_OK, WAV_ANALYZER_ERROR_DECODE_DATA.
 */
static en_WAV_ANALYZER_ERROR read_block ( st_MRD *r, uint8_t flag, uint8_t *data, uint32_t size,
                                          uint16_t *stored, uint16_t *computed, uint32_t *out_read ) {
    uint8_t f;
    if ( out_read ) *out_read = 0;
    if ( read_byte ( r, &f ) || f != flag ) return WAV_ANALYZER_ERROR_DECODE_DATA;
    uint16_t sum = 0;
    for ( uint32_t i = 0; i < size; i++ ) {
        if ( read_byte ( r, &data[i] ) ) return WAV_ANALYZER_ERROR_DECODE_DATA;
        sum = ( uint16_t ) ( sum + data[i] );
        if ( out_read ) *out_read = i + 1;
    }
    uint8_t lo, hi;
    if ( read_byte ( r, &lo ) || read_byte ( r, &hi ) ) return WAV_ANALYZER_ERROR_DECODE_DATA;
    *stored = ( uint16_t ) ( lo | ( hi << 8 ) );
    *computed = sum;
    return WAV_ANALYZER_OK;
}


/** @brief Rychlosti Intercopy V10.2 v režimu CPM (popisek v Bd). */
/**
 * @brief Rychlosti Intercopy V10.2 CPM a jejich půlka bitu v T (takt CPU MZ-800 3 546 895 Hz).
 *
 * Změřeno v emulátoru (záznam hran PC1): 1183, 741, 637, 559, 494 T.
 */
static const struct {
    uint16_t bd;     /**< rychlost v Bd, jak ji nabízí Intercopy */
    uint16_t half;   /**< půlka bitu v T */
} g_ic_cpm[] = { { 1500, 1183 }, { 2400, 741 }, { 2800, 637 }, { 3200, 559 }, { 3600, 494 } };

/**
 * @brief Rychlost Intercopy CPM podle průměrné půlky bitu.
 *
 * Vybere rychlost, jejíž změřená půlka bitu (g_ic_cpm) je nejblíž průměrné
 * půlce z pilotu hlavičky.
 *
 * @param half_us Průměrná půlka bitu z pilotu hlavičky (µs).
 * @return Nejbližší rychlost z g_ic_cpm.
 */
static uint16_t cpmtape_intercopy_bd ( double half_us ) {
    uint16_t best = 2400;
    double bd_diff = 1e9;
    for ( size_t k = 0; k < sizeof ( g_ic_cpm ) / sizeof ( g_ic_cpm[0] ); k++ ) {
        double d = half_us - g_ic_cpm[k].half / 3.546895;
        if ( d < 0 ) d = -d;
        if ( d < bd_diff ) { bd_diff = d; best = g_ic_cpm[k].bd; }
    }
    return best;
}


/**
 * @brief Zkusí přečíst koncový bit Intercopy: bit 1, jehož druhá půlka splyne s pauzou nebo koncem.
 * @param r Čtečka za checksumem (posune se jen při úspěchu).
 * @return 1 = koncový bit (Intercopy), 0 = ne (Pezik: následuje pilot těla).
 */
static int end_bit ( st_MRD *r ) {
    st_MRD t = *r;
    int a, b;
    if ( next_half ( &t, &a ) ) return 0;
    if ( a != t.pol ) return 0;                  /* první půlka bitu 1 */
    if ( next_half ( &t, &b ) == 0 ) {
        /* druhá půlka ještě v Manchesteru: Intercopy jen když hned potom končí blok */
        int c;
        st_MRD u = t;
        if ( b == a || next_half ( &u, &c ) == 0 ) return 0;
    }
    *r = t;
    return 1;
}


en_WAV_ANALYZER_ERROR wav_decode_cpmtape_decode_mzf (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from,
    st_MZF **out_mzf,
    st_WAV_DECODE_RESULT *out_header_result,
    st_WAV_DECODE_RESULT *out_body_result,
    uint32_t *out_consumed_until,
    st_WAV_CPMTAPE_INFO *out_info,
    st_WAV_BODY_RECOVERY *rec
) {
    if ( !seq || !out_mzf ) return WAV_ANALYZER_ERROR_INVALID_PARAM;
    *out_mzf = NULL;
    if ( out_consumed_until ) *out_consumed_until = 0;
    if ( out_header_result ) memset ( out_header_result, 0, sizeof ( *out_header_result ) );
    if ( out_body_result ) memset ( out_body_result, 0, sizeof ( *out_body_result ) );
    st_WAV_CPMTAPE_INFO info;
    memset ( &info, 0, sizeof ( info ) );

    /* === 1. hlavička: pilot, sync, flag 00h, 128 B, checksum === */
    uint8_t hdr[CPMTAPE_HEADER_SIZE];
    uint16_t h_st = 0, h_cp = 0;
    st_MRD r;
    uint32_t from = search_from;
    uint32_t start = 0;
    for ( ;; ) {
        double h;
        if ( !find_pilot ( seq, from, seq->count, &start, &h ) ) return WAV_ANALYZER_ERROR_NO_LEADER;
        memset ( &r, 0, sizeof ( r ) );
        r.seq = seq;
        r.pos = start;
        r.h = h;
        uint32_t patterns = 0;
        if ( sync_pilot ( &r, &patterns ) == 0 && read_block ( &r, WAV_CPMTAPE_FLAG_HEADER, hdr, CPMTAPE_HEADER_SIZE, &h_st, &h_cp, NULL )
                                                   == WAV_ANALYZER_OK ) {
            info.half_us = h;
            info.pilot_header = patterns;
            info.header_start_pulse = start;
            break;
        }
        from = ( r.pos > start ) ? r.pos : start + 1;   /* další kandidát (např. pilot těla bez hlavičky) */
    }
    uint32_t hdr_end = r.pos;
    info.variant = end_bit ( &r ) ? WAV_CPMTAPE_INTERCOPY : WAV_CPMTAPE_PEZIK;
    if ( info.variant == WAV_CPMTAPE_PEZIK ) {
        info.bd = ( info.half_us <= WAV_CPMTAPE_PEZIK_3200_MAX_HALF_US ) ? 3200 : 2400;
    } else {
        info.bd = cpmtape_intercopy_bd ( info.half_us );
    }

    if ( out_header_result ) {
        out_header_result->format = WAV_TAPE_FORMAT_CPM_TAPE;
        out_header_result->data = malloc ( CPMTAPE_HEADER_SIZE );
        if ( out_header_result->data ) memcpy ( out_header_result->data, hdr, CPMTAPE_HEADER_SIZE );
        out_header_result->data_size = CPMTAPE_HEADER_SIZE;
        out_header_result->crc_status = ( h_st == h_cp ) ? WAV_CRC_OK : WAV_CRC_ERROR;
        out_header_result->crc_stored = h_st;
        out_header_result->crc_computed = h_cp;
        out_header_result->pulse_start = start;
        out_header_result->pulse_end = hdr_end;
        out_header_result->is_header = 1;
    }

    st_MZF_HEADER mh;
    memcpy ( &mh, hdr, sizeof ( mh ) );
    mzf_header_swap_endianity ( &mh );   /* 11h (fname.terminator) zůstává podle pásky */
    uint32_t fsize = mh.fsize;

    /* === 2. tělo: pilot, sync, flag 01h, fsize B, checksum === */
    uint8_t *body = malloc ( fsize ? fsize : 1 );
    if ( !body ) return WAV_ANALYZER_ERROR_ALLOC;
    en_WAV_CRC_STATUS body_crc = WAV_CRC_NOT_AVAILABLE;
    uint16_t b_st = 0, b_cp = 0;
    uint32_t body_start = r.pos;
    if ( fsize ) {
        uint32_t bstart;
        double bh;
        uint32_t limit = ( r.pos > UINT32_MAX - CPMTAPE_BODY_SEARCH_MAX ) ? seq->count : r.pos + CPMTAPE_BODY_SEARCH_MAX;
        if ( !find_pilot ( seq, r.pos, limit, &bstart, &bh ) ) {
            free ( body );
            if ( out_consumed_until ) *out_consumed_until = r.pos;   /* hledat dál za hlavičkou */
            return WAV_ANALYZER_ERROR_DECODE_TAPEMARK;
        }
        memset ( &r, 0, sizeof ( r ) );
        r.seq = seq;
        r.pos = bstart;
        r.h = bh;
        body_start = bstart;
        uint32_t body_read = 0;
        if ( sync_pilot ( &r, &info.pilot_body ) != 0
             || read_block ( &r, WAV_CPMTAPE_FLAG_BODY, body, fsize, &b_st, &b_cp, &body_read ) != WAV_ANALYZER_OK ) {
            if ( !rec || !rec->enable || body_read == 0 ) {
                free ( body );
                if ( out_consumed_until ) *out_consumed_until = r.pos > bstart ? r.pos : bstart + 1;   /* za místem chyby */
                return WAV_ANALYZER_ERROR_DECODE_DATA;
            }
            /* neúplné tělo: přečtené bajty zůstanou, zbytek 00h; součet nejde ověřit */
            memset ( &body[body_read], 0, fsize - body_read );
            b_st = 0;
            b_cp = 1;
            rec->status |= WAV_RECOVERY_PARTIAL_BODY;
            rec->read_bytes = body_read;
            rec->expected_bytes = fsize;
        }
        body_crc = ( b_st == b_cp ) ? WAV_CRC_OK : WAV_CRC_ERROR;
        end_bit ( &r );   /* Intercopy: koncový bit těla */
    }
    if ( out_body_result ) {
        out_body_result->format = WAV_TAPE_FORMAT_CPM_TAPE;
        out_body_result->data = NULL;   /* tělo je v mzf->body */
        out_body_result->data_size = fsize;
        out_body_result->crc_status = body_crc;
        out_body_result->crc_stored = b_st;
        out_body_result->crc_computed = b_cp;
        out_body_result->pulse_start = body_start;
        out_body_result->pulse_end = r.pos;
        out_body_result->is_header = 0;
    }

    st_MZF *mzf = calloc ( 1, sizeof ( st_MZF ) );
    if ( !mzf ) {
        free ( body );
        return WAV_ANALYZER_ERROR_ALLOC;
    }
    memcpy ( &mzf->header, &mh, sizeof ( mh ) );
    mzf->body = body;
    mzf->body_size = fsize;
    *out_mzf = mzf;
    if ( out_consumed_until ) *out_consumed_until = r.pos;
    if ( out_info ) *out_info = info;
    return WAV_ANALYZER_OK;
}
