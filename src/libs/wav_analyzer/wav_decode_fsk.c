/**
 * @file   wav_decode_fsk.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace vrstvy 4e3 - FSK dekoder.
 *
 * Dekoduje FSK (Frequency Shift Keying) signal z WAV nahravky.
 *
 * Algoritmus dekodovani:
 * 1. Detekce GAPu - hledame sekvenci >= 80 pulzu s konstantni periodou
 * 2. Preskoceni polarity (2 pul-periody) + sync (2 pul-periody) za GAPem
 * 3. Kalibrace - zmereni prvnich 16 celych cyklu, setrideni,
 *    nalezeni mezery mezi short a long klastry, prah = stred mezery
 * 4. Dekodovani bajtu: cely cyklus (LOW+HIGH) >= prah -> bit 1, jinak bit 0
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

#include "wav_decode_fsk.h"


/**
 * @brief Maximalni pocet dekodovanych bajtu (ochrana proti preteceni).
 *
 * Prenosova rychlost FSK pri speed 0 a 44100 Hz [neovereno].
 * 65535 bajtu je dostatecna horni mez pro vsechny rozumne vstupy.
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
 * GAP je sekvence >= WAV_FSK_MIN_GAP_PULSES po sobe jdoucich pulzu
 * s podobnou periodou (tolerance WAV_FSK_GAP_TOLERANCE).
 * Vraci pozici prvniho pulzu za GAPem a prumernou periodu GAPu.
 *
 * @param seq Sekvence pulzu.
 * @param search_from Pozice od ktere zacit hledat.
 * @param[out] out_gap_end Index prvniho pulzu za GAPem.
 * @param[out] out_gap_period_us Prumerna perioda GAPu (us).
 * @return WAV_ANALYZER_OK pri nalezeni, WAV_ANALYZER_ERROR_NO_LEADER pokud nenalezen.
 */
static en_WAV_ANALYZER_ERROR fsk_find_gap (
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
        if ( fabs ( d - avg ) <= avg * WAV_FSK_GAP_TOLERANCE ) {
            sum += d;
            count++;
        } else {
            /* konec souvisle sekvence - zkontrolujeme zda stacila */
            if ( count >= WAV_FSK_MIN_GAP_PULSES ) {
                *out_gap_end = i;
                *out_gap_period_us = sum / count;
                return WAV_ANALYZER_OK;
            }
            /* reset - novy zacatek od aktualniho pulzu */
            sum = d;
            count = 1;
        }
    }

    /* zkontrolujeme posledni sekvenci */
    if ( count >= WAV_FSK_MIN_GAP_PULSES ) {
        *out_gap_end = seq->count;
        *out_gap_period_us = sum / count;
        return WAV_ANALYZER_OK;
    }

    return WAV_ANALYZER_ERROR_NO_LEADER;
}


/* =========================================================================
 *  Kalibrace prahu
 * ========================================================================= */

/**
 * @brief Zkalibruje prah pro rozliseni short/long FSK cyklu.
 *
 * Zmeri delky prvnich @p cal_cycles celych cyklu (kazdy = 2 pul-periody),
 * setridi je a najde nejvetsi mezeru. Prah = stred mezery.
 *
 * @param seq Sekvence pulzu.
 * @param data_start Index prvniho datoveho pulzu (za sync).
 * @param cal_cycles Pocet cyklu pro kalibraci.
 * @param[out] out_threshold Kalibrovany prah (us).
 * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
 */
static en_WAV_ANALYZER_ERROR fsk_calibrate (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t data_start,
    uint32_t cal_cycles,
    double *out_threshold
) {
    /* potrebujeme 2 * cal_cycles pul-period */
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

    /* najit nejvetsi mezeru mezi sousednimi hodnotami */
    double max_gap = 0.0;
    uint32_t max_gap_idx = 0;
    uint32_t j;
    for ( j = 0; j + 1 < cal_cycles; j++ ) {
        double gap = durations[j + 1] - durations[j];
        if ( gap > max_gap ) {
            max_gap = gap;
            max_gap_idx = j;
        }
    }

    /* prah = stred mezery */
    *out_threshold = ( durations[max_gap_idx] + durations[max_gap_idx + 1] ) / 2.0;

    free ( durations );
    return WAV_ANALYZER_OK;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

en_WAV_ANALYZER_ERROR wav_decode_fsk_decode_bytes (
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

    err = fsk_find_gap ( seq, search_from_pulse, &gap_end, &gap_period_us );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* === 2. Preskoc polarity (2 pul-periody) + sync (2 pul-periody) === */
    uint32_t data_start = gap_end + 4;
    if ( data_start >= seq->count ) {
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    /* === 3. Kalibrace === */
    double threshold;
    err = fsk_calibrate ( seq, data_start, WAV_FSK_CALIBRATION_CYCLES, &threshold );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* === 4. Dekodovani bajtu === */
    /* fadeout limit: cyklus delsi nez 2x GAP perioda (cela perioda = 2 pul-periody) */
    double fadeout_limit = gap_period_us * 2.0 * WAV_FSK_FADEOUT_FACTOR;

    /* alokace vystupniho bufferu (dynamicky roste) */
    uint32_t capacity = 1024;
    uint8_t *data = ( uint8_t* ) malloc ( capacity );
    if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

    uint32_t decoded = 0;
    uint32_t pos = data_start;

    while ( pos + 1 < seq->count && decoded < MAX_DECODED_BYTES ) {
        /* zkontroluj zda dalsi cyklus neni fadeout */
        double cycle = seq->pulses[pos].duration_us + seq->pulses[pos + 1].duration_us;
        if ( cycle > fadeout_limit ) break;

        /* dekoduj 1 bajt (8 cyklu = 16 pul-period) */
        if ( pos + 16 > seq->count ) break;

        uint8_t byte = 0;
        int valid = 1;
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            double c = seq->pulses[pos].duration_us + seq->pulses[pos + 1].duration_us;
            if ( c > fadeout_limit ) {
                valid = 0;
                break;
            }
            if ( c >= threshold ) {
                byte |= ( uint8_t ) ( 1 << ( 7 - bit ) );
            }
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
