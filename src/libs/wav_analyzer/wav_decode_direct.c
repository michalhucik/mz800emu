/**
 * @file   wav_decode_direct.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace vrstvy 4e5 - DIRECT dekoder.
 *
 * Dekoduje DIRECT (primy bitovy zapis) signal z WAV nahravky.
 *
 * Algoritmus dekodovani:
 * 1. Detekce GAPu - hledame sekvenci >= 80 pulzu s konstantni periodou
 * 2. Preskoceni polarity + sync frame pomoci poctu vzorku
 *    (10 vzorku pri ref. 44100 Hz, skalovano na cilovy rate)
 * 3. Expanze pulzu na individualni vzorky (dle duration_samples)
 * 4. Cteni po 12 vzorcich na bajt: D7 D6 ~D6 D5 D4 ~D4 D3 D2 ~D2 D1 D0 ~D0
 * 5. Konec pri vycerpani pulzu
 *
 * DIRECT je odlisny od FSK a SLOW - pracuje na urovni vzorku,
 * ne celych cyklu (LOW+HIGH). Kazdy datovy element je 1 audio vzorek.
 * Pulse extractor slucuje po sobe jdouci stejne vzorky do jednoho pulzu,
 * proto dekoder pouziva duration_samples pro expanzi zpet na vzorky.
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
#include <math.h>

#include "wav_decode_direct.h"


/**
 * @brief Maximalni pocet dekodovanych bajtu (ochrana proti preteceni).
 */
#define MAX_DECODED_BYTES  65535


/* =========================================================================
 *  Interni kontext dekoderu
 * ========================================================================= */

/**
 * @brief Kontext DIRECT dekoderu pro prochazeni vzorku v pulzech.
 *
 * Sleduje aktualni pozici v pulzni sekvenci a offset vzorku uvnitr
 * aktualniho pulzu. Umoznuje cist individualni vzorky z RLE-kodovanych
 * pulzu (kde duration_samples > 1 znamena vice po sobe jdoucich
 * vzorku se stejnou hodnotou).
 *
 * Obsahuje bit1_level - uroven pulzu odpovídající bitu 1 (HIGH).
 * Tato hodnota se urcuje z polarity pulzu za GAPem a umoznuje
 * spravne dekodovani i pri invertovane polarite WAV signalu.
 *
 * @par Invarianty:
 * - pulse_idx < seq->count (nebo na konci)
 * - sample_offset < aktualni pulz duration_samples
 * - bit1_level je 0 nebo 1
 */
typedef struct {
    const st_WAV_PULSE_SEQUENCE *seq;   /**< zdrojova sekvence pulzu */
    uint32_t pulse_idx;                 /**< aktualni index pulzu */
    uint32_t sample_offset;             /**< offset vzorku v aktualnim pulzu */
    int bit1_level;                     /**< uroven pulzu odpovídající bitu 1 (HIGH);
                                             1 = normalni polarita, 0 = invertovana */
} st_DIRECT_DEC;


/* =========================================================================
 *  Pomocne funkce - cteni vzorku
 * ========================================================================= */

/**
 * @brief Precte jeden vzorek (bit) a posune pozici o 1 vzorek.
 *
 * Vrati level aktualniho vzorku (0 = LOW, 1 = HIGH) a posune
 * interni pozici o 1 vzorek. Pokud aktualni pulz obsahuje vice
 * vzorku (duration_samples > 1), zustaname v nem a jen zvysime offset.
 * Pokud se vycerpa, presuneme se na dalsi pulz.
 *
 * @param dec Kontext dekoderu.
 * @return 0 nebo 1 pri uspechu, -1 na konci sekvence.
 */
static int direct_read_sample ( st_DIRECT_DEC *dec ) {
    if ( dec->pulse_idx >= dec->seq->count ) return -1;

    int raw_level = dec->seq->pulses[dec->pulse_idx].level;
    dec->sample_offset++;

    if ( dec->sample_offset >= dec->seq->pulses[dec->pulse_idx].duration_samples ) {
        dec->pulse_idx++;
        dec->sample_offset = 0;
    }

    /* prevedeme raw uroven na logickou hodnotu s ohledem na polaritu */
    return ( raw_level == dec->bit1_level ) ? 1 : 0;
}


/**
 * @brief Preskoci N vzorku v sekvenci pulzu.
 *
 * Posune interni pozici dekoderu o @p count vzorku dopredu.
 * Konzumuje pulzy dle jejich duration_samples.
 *
 * @param dec Kontext dekoderu.
 * @param count Pocet vzorku k preskoceni.
 */
static void direct_skip_samples ( st_DIRECT_DEC *dec, uint32_t count ) {
    while ( count > 0 && dec->pulse_idx < dec->seq->count ) {
        uint32_t remaining = dec->seq->pulses[dec->pulse_idx].duration_samples - dec->sample_offset;
        if ( count < remaining ) {
            dec->sample_offset += count;
            return;
        }
        count -= remaining;
        dec->pulse_idx++;
        dec->sample_offset = 0;
    }
}


/* =========================================================================
 *  Detekce GAPu
 * ========================================================================= */

/**
 * @brief Najde GAP (leader ton) v sekvenci pulzu.
 *
 * Shodny algoritmus s FSK/SLOW - hleda sekvenci >= WAV_DIRECT_MIN_GAP_PULSES
 * po sobe jdoucich pulzu s podobnou periodou.
 *
 * @param seq Sekvence pulzu.
 * @param search_from Pozice od ktere zacit hledat.
 * @param[out] out_gap_end Index prvniho pulzu za GAPem.
 * @param[out] out_gap_period_us Prumerna perioda GAPu (us).
 * @return WAV_ANALYZER_OK pri nalezeni, WAV_ANALYZER_ERROR_NO_LEADER pokud nenalezen.
 */
static en_WAV_ANALYZER_ERROR direct_find_gap (
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
        if ( fabs ( d - avg ) <= avg * WAV_DIRECT_GAP_TOLERANCE ) {
            sum += d;
            count++;
        } else {
            if ( count >= WAV_DIRECT_MIN_GAP_PULSES ) {
                *out_gap_end = i;
                *out_gap_period_us = sum / count;
                return WAV_ANALYZER_OK;
            }
            sum = d;
            count = 1;
        }
    }

    if ( count >= WAV_DIRECT_MIN_GAP_PULSES ) {
        *out_gap_end = seq->count;
        *out_gap_period_us = sum / count;
        return WAV_ANALYZER_OK;
    }

    return WAV_ANALYZER_ERROR_NO_LEADER;
}


/* =========================================================================
 *  Dekodovani bajtu
 * ========================================================================= */

/**
 * @brief Dekoduje jeden bajt ze 12 vzorku DIRECT kodovani.
 *
 * Cte 12 po sobe jdoucich vzorku a extrahuje 8 datovych bitu:
 * @verbatim
 *   pozice:  0   1   2   3   4   5   6   7   8   9  10  11
 *   obsah:  D7  D6 ~D6  D5  D4 ~D4  D3  D2 ~D2  D1  D0 ~D0
 * @endverbatim
 *
 * Datove bity jsou na pozicich 0, 1, 3, 4, 6, 7, 9, 10.
 * Synchro bity na pozicich 2, 5, 8, 11 se ignoruji (slouzi pouze
 * pro resynchronizaci Z80 dekoderu).
 *
 * @param dec Kontext dekoderu.
 * @param[out] byte Dekodovany bajt.
 * @return WAV_ANALYZER_OK pri uspechu,
 *         WAV_ANALYZER_ERROR_DECODE_INCOMPLETE na konci sekvence.
 */
static en_WAV_ANALYZER_ERROR direct_decode_byte ( st_DIRECT_DEC *dec, uint8_t *byte ) {
    int elements[WAV_DIRECT_SAMPLES_PER_BYTE];
    int i;

    /* precteme 12 vzorku */
    for ( i = 0; i < WAV_DIRECT_SAMPLES_PER_BYTE; i++ ) {
        elements[i] = direct_read_sample ( dec );
        if ( elements[i] < 0 ) {
            return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
        }
    }

    /* extrahujeme datove bity (pozice 0,1,3,4,6,7,9,10) */
    static const int data_pos[8] = { 0, 1, 3, 4, 6, 7, 9, 10 };
    uint8_t value = 0;
    for ( i = 0; i < 8; i++ ) {
        if ( elements[data_pos[i]] ) {
            value |= ( uint8_t ) ( 1 << ( 7 - i ) );
        }
    }

    *byte = value;
    return WAV_ANALYZER_OK;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

en_WAV_ANALYZER_ERROR wav_decode_direct_decode_bytes (
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

    err = direct_find_gap ( seq, search_from_pulse, &gap_end, &gap_period_us );
    if ( err != WAV_ANALYZER_OK ) return err;

    /* === 2. Detekce polarity z polarity pulzu === */

    /*
     * Za GAPem nasleduje polarity pulz: LOW(4 ref) + HIGH(2 ref).
     * LOW cast ma vic vzorku nez HIGH cast. Delsi polovina odpovida
     * logicke urovni LOW (bit 0). Kratsi odpovida logicke HIGH (bit 1).
     *
     * To umoznuje spravne dekodovat i pri invertovane polarite WAV signalu.
     */
    int bit1_level = 1; /* vychozi: normalni polarita */

    if ( gap_end + 1 < seq->count ) {
        uint32_t d0 = seq->pulses[gap_end].duration_samples;
        uint32_t d1 = seq->pulses[gap_end + 1].duration_samples;
        int lev0 = seq->pulses[gap_end].level;
        int lev1 = seq->pulses[gap_end + 1].level;

        if ( d0 > d1 ) {
            /* pulse[gap_end] je delsi = LOW (polarity LOW), opacny level je HIGH */
            bit1_level = lev1;
        } else {
            /* pulse[gap_end+1] je delsi = LOW, opacny level je HIGH */
            bit1_level = lev0;
        }
    }

    /* === 3. Inicializuj dekoder na pozici za GAPem === */
    st_DIRECT_DEC dec;
    dec.seq = seq;
    dec.pulse_idx = gap_end;
    dec.sample_offset = 0;
    dec.bit1_level = bit1_level;

    /*
     * Preskoceni polarity + sync frame.
     * Polarity: 4L + 2H = 6 vzorku pri 44100 Hz
     * Sync: 2L + 2H = 4 vzorku pri 44100 Hz
     * Celkem 10 vzorku pri ref rate, skalovano na cilovy rate.
     */
    uint32_t frame_skip = ( uint32_t ) round (
                               ( double ) WAV_DIRECT_FRAME_SKIP_REF * seq->sample_rate / WAV_DIRECT_REF_RATE
                           );
    if ( frame_skip < 1 ) frame_skip = 1;

    direct_skip_samples ( &dec, frame_skip );

    if ( dec.pulse_idx >= seq->count ) {
        return WAV_ANALYZER_ERROR_DECODE_INCOMPLETE;
    }

    /* === 3. Dekodovani bajtu === */
    uint32_t capacity = 1024;
    uint8_t *data = ( uint8_t* ) malloc ( capacity );
    if ( !data ) return WAV_ANALYZER_ERROR_ALLOC;

    uint32_t decoded = 0;

    while ( dec.pulse_idx < seq->count && decoded < MAX_DECODED_BYTES ) {
        uint8_t byte;
        err = direct_decode_byte ( &dec, &byte );
        if ( err != WAV_ANALYZER_OK ) break;

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
    if ( out_consumed_until ) *out_consumed_until = dec.pulse_idx;

    return WAV_ANALYZER_OK;
}
