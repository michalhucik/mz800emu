/**
 * @file   wav_decode_slow.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace vrstvy 4e4 - SLOW dekoder.
 *
 * Dekoduje SLOW (kvaternalni) signal z WAV nahravky.
 *
 * Algoritmus dekodovani:
 * 1. Detekce GAPu - hledame sekvenci >= 80 pulzu s konstantni periodou
 * 2. Preskoceni polarity (2 pul-periody) + sync (2 pul-periody) za GAPem
 * 3. Kalibrace - zmereni prvnich 16 celych cyklu, klasterovani
 *    do 4 skupin, urceni 3 prahu
 * 4. Dekodovani bajtu: klasifikace cyklu na symbol 0-3 (2 bity),
 *    4 symboly na bajt, MSB first
 * 5. Konec pri fadeoutu (cyklus > 2x GAP perioda)
 *
 * @par Licence:
 * Odvozeno z projektu MZFTools v0.2.2 (mz-fuzzy@users.sf.net),
 * sireneho pod GNU General Public License v3 (GPLv3).
 *
 * Copyright (C) MZFTools authors (mz-fuzzy@users.sf.net)
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
#include <math.h>

#include "wav_decode_slow.h"


/**
 * @brief Maximalni pocet dekodovanych bajtu (ochrana proti preteceni).
 */
#define MAX_DECODED_BYTES  65535


/* =========================================================================
 *  Pomocna funkce - porovnani double pro qsort
 * ========================================================================= */

/**
 * @brief Komparator pro qsort na poli double.
 * @param a Ukazatel na prvni prvek.
 * @param b Ukazatel na druhy prvek.
 * @return < 0 pokud a < b, 0 pokud a == b, > 0 pokud a > b.
 */
static int cmp_double ( const void *a, const void *b ) {
    double da = *( const double* ) a;
    double db = *( const double* ) b;
    if ( da < db ) return -1;
    if ( da > db ) return 1;
    return 0;
}


/* =========================================================================
 *  Detekce GAPu
 * ========================================================================= */

/**
 * @brief Najde GAP (leader ton) v sekvenci pulzu.
 *
 * GAP je sekvence >= WAV_SLOW_MIN_GAP_PULSES po sobe jdoucich pulzu
 * s podobnou periodou (tolerance WAV_SLOW_GAP_TOLERANCE).
 *
 * @param seq Sekvence pulzu.
 * @param search_from Pozice od ktere zacit hledat.
 * @param[out] out_gap_end Index prvniho pulzu za GAPem.
 * @param[out] out_gap_period_us Prumerna perioda GAPu (us).
 * @return WAV_ANALYZER_OK pri nalezeni, WAV_ANALYZER_ERROR_NO_LEADER pokud nenalezen.
 */
static en_WAV_ANALYZER_ERROR slow_find_gap (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from,
    uint32_t *out_gap_end,
    double *out_gap_period_us
) {
    double sum = 0.0;
    uint32_t count = 0;
    uint32_t i;

    for ( i = search_from; i < seq->count; i++ ) {
        double d = seq->pulses[i].duration_us;

        if ( count == 0 ) {
            sum = d;
            count = 1;
            continue;
        }

        double avg = sum / count;
        if ( fabs ( d - avg ) <= avg * WAV_SLOW_GAP_TOLERANCE ) {
            sum += d;
            count++;
        } else {
            if ( count >= WAV_SLOW_MIN_GAP_PULSES ) {
                *out_gap_end = i;
                *out_gap_period_us = sum / count;
                return WAV_ANALYZER_OK;
            }
            sum = d;
            count = 1;
        }
    }

    if ( count >= WAV_SLOW_MIN_GAP_PULSES ) {
        *out_gap_end = seq->count;
        *out_gap_period_us = sum / count;
        return WAV_ANALYZER_OK;
    }

    return WAV_ANALYZER_ERROR_NO_LEADER;
}


/* =========================================================================
 *  Kalibrace prahu (4 klastry)
 * ========================================================================= */

/**
 * @brief Zkalibruje 3 prahy pro rozliseni 4 symbolu SLOW cyklu.
 *
 * Zmeri delky prvnich @p cal_cycles celych cyklu, setridi je
 * a najde 3 nejvetsi mezery. Prahy = stredy mezer.
 *
 * @param seq Sekvence pulzu.
 * @param data_start Index prvniho datoveho pulzu (za sync).
 * @param cal_cycles Pocet cyklu pro kalibraci.
 * @param[out] out_thresholds Pole 3 prahu (us), vzestupne.
 * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
 */
static en_WAV_ANALYZER_ERROR slow_calibrate (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t data_start,
    uint32_t cal_cycles,
    double out_thresholds[3]
) {
    if ( data_start + 2 * cal_cycles > seq->count ) {
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    double *durations = ( double* ) malloc ( cal_cycles * sizeof ( double ) );
    if ( !durations ) return WAV_ANALYZER_ERROR_ALLOC;

    uint32_t pos = data_start;
    uint32_t c;
    for ( c = 0; c < cal_cycles; c++ ) {
        durations[c] = seq->pulses[pos].duration_us + seq->pulses[pos + 1].duration_us;
        pos += 2;
    }

    /* setridit vzestupne */
    qsort ( durations, cal_cycles, sizeof ( double ), cmp_double );

    /*
     * Najit 3 nejvetsi mezery mezi sousednimi hodnotami.
     * Kazda mezera definuje hranici mezi dvema sousednimi klastry.
     */
    typedef struct {
        double gap;
        uint32_t idx;
    } st_gap;

    st_gap gaps[3] = { { 0.0, 0 }, { 0.0, 0 }, { 0.0, 0 } };
    uint32_t j;
    for ( j = 0; j + 1 < cal_cycles; j++ ) {
        double g = durations[j + 1] - durations[j];
        /* vlozit do top-3 pokud je dost velka */
        int k;
        for ( k = 0; k < 3; k++ ) {
            if ( g > gaps[k].gap ) {
                /* posunout mensi dolu */
                int m;
                for ( m = 2; m > k; m-- ) {
                    gaps[m] = gaps[m - 1];
                }
                gaps[k].gap = g;
                gaps[k].idx = j;
                break;
            }
        }
    }

    /* setridit 3 mezery podle indexu (vzestupne) pro spravne poradi prahu */
    int a, b;
    for ( a = 0; a < 2; a++ ) {
        for ( b = a + 1; b < 3; b++ ) {
            if ( gaps[b].idx < gaps[a].idx ) {
                st_gap tmp = gaps[a];
                gaps[a] = gaps[b];
                gaps[b] = tmp;
            }
        }
    }

    /* prahy = stredy mezer */
    int t;
    for ( t = 0; t < 3; t++ ) {
        uint32_t idx = gaps[t].idx;
        out_thresholds[t] = ( durations[idx] + durations[idx + 1] ) / 2.0;
    }

    free ( durations );
    return WAV_ANALYZER_OK;
}


/**
 * @brief Klasifikuje delku cyklu na symbol 0-3 pomoci 3 prahu.
 *
 * @param cycle_us Delka celeho cyklu (us).
 * @param thresholds Pole 3 prahu (vzestupne).
 * @return Symbol 0-3.
 */
static int slow_classify_symbol ( double cycle_us, const double thresholds[3] ) {
    if ( cycle_us < thresholds[0] ) return 0;
    if ( cycle_us < thresholds[1] ) return 1;
    if ( cycle_us < thresholds[2] ) return 2;
    return 3;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

en_WAV_ANALYZER_ERROR wav_decode_slow_decode_bytes (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t search_from_pulse,
    uint8_t **out_data,
    uint32_t *out_size,
    uint32_t *out_consumed_until
) {
    if ( !seq || !out_data || !out_size ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    *out_data = NULL;
    *out_size = 0;
    if ( out_consumed_until ) *out_consumed_until = 0;

    /* === 1. Najdi GAP === */
    uint32_t gap_end;
    double gap_period_us;
    en_WAV_ANALYZER_ERROR err;

    err = slow_find_gap ( seq, search_from_pulse, &gap_end, &gap_period_us );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* === 2. Preskoc polarity (2 pul-periody) + sync (2 pul-periody) === */
    uint32_t data_start = gap_end + 4;
    if ( data_start >= seq->count ) {
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    /* === 3. Kalibrace (4 klastry, 3 prahy) === */
    double thresholds[3];
    err = slow_calibrate ( seq, data_start, WAV_SLOW_CALIBRATION_CYCLES, thresholds );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* === 4. Dekodovani bajtu === */
    double fadeout_limit = gap_period_us * 2.0 * WAV_SLOW_FADEOUT_FACTOR;

    uint32_t capacity = 1024;
    uint8_t *data = ( uint8_t* ) malloc ( capacity );
    if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

    uint32_t decoded = 0;
    uint32_t pos = data_start;

    while ( pos + 1 < seq->count && decoded < MAX_DECODED_BYTES ) {
        /* zkontroluj fadeout na prvnim cyklu bajtu */
        double cycle = seq->pulses[pos].duration_us + seq->pulses[pos + 1].duration_us;
        if ( cycle > fadeout_limit ) break;

        /* dekoduj 1 bajt (4 cykly = 8 pul-period) */
        if ( pos + 8 > seq->count ) break;

        uint8_t byte = 0;
        int valid = 1;
        int sym;
        for ( sym = 0; sym < 4; sym++ ) {
            double c = seq->pulses[pos].duration_us + seq->pulses[pos + 1].duration_us;
            if ( c > fadeout_limit ) {
                valid = 0;
                break;
            }
            int symbol = slow_classify_symbol ( c, thresholds );
            byte |= ( uint8_t ) ( symbol << ( 6 - sym * 2 ) );
            pos += 2;
        }

        if ( !valid ) break;

        /* uloz bajt */
        if ( decoded >= capacity ) {
            capacity *= 2;
            uint8_t *tmp = ( uint8_t* ) realloc ( data, capacity );
            if ( !tmp ) {
                free ( data );
                return WAV_ANALYZER_ERROR_ALLOC;
            }
            data = tmp;
        }
        data[decoded++] = byte;
    }

    if ( decoded == 0 ) {
        free ( data );
        return WAV_ANALYZER_ERROR_DECODE_DATA;
    }

    *out_data = data;
    *out_size = decoded;
    if ( out_consumed_until ) *out_consumed_until = pos;

    return WAV_ANALYZER_OK;
}
