/**
 * @file   wav_pulse.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace vrstvy 1 - extrakce pulzů ze signálu.
 *
 * Obsahuje dva režimy extrakce pulzů:
 *   - Zero-crossing: detekce přechodů přes nulovou úroveň
 *   - Schmitt trigger: hysterezní přepínání s horním a dolním prahem
 *
 * Výstupem je sekvence pulzů (půl-period) s délkami v mikrosekundách.
 * Inspirováno algoritmem wavdec.c (C port Intercopy).
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

#include "wav_pulse.h"


/** @brief Počáteční kapacita pole pulzů. */
#define PULSE_INITIAL_CAPACITY  4096

/** @brief Faktor růstu kapacity pole pulzů při realokaci. */
#define PULSE_GROW_FACTOR       2


/**
 * @brief Přidá pulz do sekvence s automatickým růstem kapacity.
 *
 * Při nedostatku místa zdvojnásobí kapacitu pole.
 *
 * @param seq Sekvence pulzů. Nesmí být NULL.
 * @param duration_samples Délka pulzu ve vzorcích.
 * @param sample_rate Vzorkovací frekvence (Hz).
 * @param level Úroveň signálu (0 nebo 1).
 * @return WAV_ANALYZER_OK při úspěchu, WAV_ANALYZER_ERROR_ALLOC při selhání.
 */
static en_WAV_ANALYZER_ERROR pulse_seq_add (
    st_WAV_PULSE_SEQUENCE *seq,
    uint32_t duration_samples,
    uint32_t sample_rate,
    int level
) {
    if ( seq->count >= seq->capacity ) {
        uint32_t new_capacity = ( seq->capacity == 0 ) ? PULSE_INITIAL_CAPACITY : seq->capacity * PULSE_GROW_FACTOR;
        st_WAV_PULSE *new_pulses = ( st_WAV_PULSE* ) realloc ( seq->pulses, new_capacity * sizeof ( st_WAV_PULSE ) );
        if ( !new_pulses ) return WAV_ANALYZER_ERROR_ALLOC;
        seq->pulses = new_pulses;
        seq->capacity = new_capacity;
    }

    st_WAV_PULSE *p = &seq->pulses[seq->count];
    p->duration_samples = duration_samples;
    p->duration_us = ( ( double ) duration_samples * 1000000.0 ) / sample_rate;
    p->level = level;
    seq->count++;

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_pulse_extract_zero_crossing (
    const double *samples,
    uint32_t sample_count,
    uint32_t sample_rate,
    en_WAV_SIGNAL_POLARITY polarity,
    st_WAV_PULSE_SEQUENCE *out_seq
) {
    if ( !samples || sample_count == 0 || sample_rate == 0 || !out_seq ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    memset ( out_seq, 0, sizeof ( *out_seq ) );
    out_seq->sample_rate = sample_rate;

    /*
     * Najdeme první přechod přes nulu, aby se ustálil stav.
     * Počáteční stav je dán znaménkem prvního vzorku.
     */
    int current_level;
    if ( polarity == WAV_SIGNAL_POLARITY_INVERTED ) {
        current_level = ( samples[0] <= 0.0 ) ? 1 : 0;
    } else {
        current_level = ( samples[0] >= 0.0 ) ? 1 : 0;
    }

    uint32_t pulse_start = 0;

    for ( uint32_t i = 1; i < sample_count; i++ ) {
        int new_level;
        if ( polarity == WAV_SIGNAL_POLARITY_INVERTED ) {
            new_level = ( samples[i] <= 0.0 ) ? 1 : 0;
        } else {
            new_level = ( samples[i] >= 0.0 ) ? 1 : 0;
        }

        if ( new_level != current_level ) {
            /* přechod - zapíšeme pulz */
            uint32_t duration = i - pulse_start;
            if ( duration > 0 ) {
                en_WAV_ANALYZER_ERROR err = pulse_seq_add ( out_seq, duration, sample_rate, current_level );
                if ( err != WAV_ANALYZER_OK ) {
                    wav_pulse_sequence_destroy ( out_seq );
                    return err;
                }
            }
            pulse_start = i;
            current_level = new_level;
        }
    }

    /* zapíšeme poslední nedokončený pulz */
    if ( pulse_start < sample_count ) {
        uint32_t duration = sample_count - pulse_start;
        if ( duration > 0 ) {
            en_WAV_ANALYZER_ERROR err = pulse_seq_add ( out_seq, duration, sample_rate, current_level );
            if ( err != WAV_ANALYZER_OK ) {
                wav_pulse_sequence_destroy ( out_seq );
                return err;
            }
        }
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_pulse_extract_schmitt (
    const double *samples,
    uint32_t sample_count,
    uint32_t sample_rate,
    en_WAV_SIGNAL_POLARITY polarity,
    double threshold_high,
    double threshold_low,
    st_WAV_PULSE_SEQUENCE *out_seq
) {
    if ( !samples || sample_count == 0 || sample_rate == 0 || !out_seq ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( threshold_high <= threshold_low ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    memset ( out_seq, 0, sizeof ( *out_seq ) );
    out_seq->sample_rate = sample_rate;

    /* při invertované polaritě prohodíme prahy */
    double high_th = threshold_high;
    double low_th = threshold_low;
    if ( polarity == WAV_SIGNAL_POLARITY_INVERTED ) {
        high_th = -threshold_low;
        low_th = -threshold_high;
    }

    /*
     * Najdeme počáteční stav - projdeme vzorky
     * dokud signál nepřekročí některý z prahů.
     */
    int current_level = 0;
    uint32_t start_i = 0;

    for ( uint32_t i = 0; i < sample_count; i++ ) {
        if ( samples[i] >= high_th ) {
            current_level = 1;
            start_i = i;
            break;
        } else if ( samples[i] <= low_th ) {
            current_level = 0;
            start_i = i;
            break;
        }
    }

    uint32_t pulse_start = start_i;

    for ( uint32_t i = start_i + 1; i < sample_count; i++ ) {
        int transition = 0;

        if ( current_level == 1 && samples[i] <= low_th ) {
            /* přechod z HIGH do LOW */
            transition = 1;
        } else if ( current_level == 0 && samples[i] >= high_th ) {
            /* přechod z LOW do HIGH */
            transition = 1;
        }

        if ( transition ) {
            uint32_t duration = i - pulse_start;
            if ( duration > 0 ) {
                en_WAV_ANALYZER_ERROR err = pulse_seq_add ( out_seq, duration, sample_rate, current_level );
                if ( err != WAV_ANALYZER_OK ) {
                    wav_pulse_sequence_destroy ( out_seq );
                    return err;
                }
            }
            pulse_start = i;
            current_level = !current_level;
        }
    }

    /* poslední pulz */
    if ( pulse_start < sample_count ) {
        uint32_t duration = sample_count - pulse_start;
        if ( duration > 0 ) {
            pulse_seq_add ( out_seq, duration, sample_rate, current_level );
        }
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_pulse_extract (
    const double *samples,
    uint32_t sample_count,
    uint32_t sample_rate,
    const st_WAV_ANALYZER_CONFIG *config,
    st_WAV_PULSE_SEQUENCE *out_seq
) {
    if ( !samples || !config || !out_seq ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    switch ( config->pulse_mode ) {
        case WAV_PULSE_MODE_ZERO_CROSSING:
            return wav_pulse_extract_zero_crossing (
                       samples, sample_count, sample_rate,
                       config->polarity, out_seq
                   );

        case WAV_PULSE_MODE_SCHMITT_TRIGGER:
            return wav_pulse_extract_schmitt (
                       samples, sample_count, sample_rate,
                       config->polarity,
                       config->schmitt_high, config->schmitt_low,
                       out_seq
                   );

        default:
            return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }
}
