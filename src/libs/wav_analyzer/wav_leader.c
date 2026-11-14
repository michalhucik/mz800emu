/**
 * @file   wav_leader.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Implementace vrstvy 2 - detekce leader tónů.
 *
 * Algoritmus detekce leader tónu:
 * 1. Sekvenční průchod pulzy od zadaného indexu
 * 2. Akumulace pulzů se shodnou délkou (v rámci tolerance od running average)
 * 3. Reset při překročení tolerance
 * 4. Nalezení leaderu při >= min_leader_pulses
 * 5. Zpřesnění statistik (průměr, směrodatná odchylka, min, max)
 *
 * Inspirováno Intercopy V10.2 a wavdec.c.
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

#include "wav_leader.h"


/** @brief Počáteční kapacita pole leader tónů. */
#define LEADER_LIST_INITIAL_CAPACITY    16


/**
 * @brief Zpřesní statistiky nalezeného leader tónu.
 *
 * Přepočítá průměr, směrodatnou odchylku, minimum a maximum
 * z finální sady pulzů leaderu.
 *
 * @param seq Sekvence pulzů.
 * @param leader Leader k zpřesnění. Musí mít platné start_index a pulse_count.
 */
static void leader_refine_stats ( const st_WAV_PULSE_SEQUENCE *seq, st_WAV_LEADER_INFO *leader ) {
    if ( leader->pulse_count == 0 ) return;

    double sum = 0.0;
    double min_val = 1e15;
    double max_val = 0.0;

    uint32_t end = leader->start_index + leader->pulse_count;
    if ( end > seq->count ) end = seq->count;

    for ( uint32_t i = leader->start_index; i < end; i++ ) {
        double d = seq->pulses[i].duration_us;
        sum += d;
        if ( d < min_val ) min_val = d;
        if ( d > max_val ) max_val = d;
    }

    leader->avg_period_us = sum / leader->pulse_count;
    leader->min_period_us = min_val;
    leader->max_period_us = max_val;

    /* směrodatná odchylka */
    double sum_sq = 0.0;
    for ( uint32_t i = leader->start_index; i < end; i++ ) {
        double diff = seq->pulses[i].duration_us - leader->avg_period_us;
        sum_sq += diff * diff;
    }
    leader->stddev_us = sqrt ( sum_sq / leader->pulse_count );
}


en_WAV_ANALYZER_ERROR wav_leader_detect (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t from_index,
    uint32_t min_pulses,
    double tolerance,
    st_WAV_LEADER_INFO *out_leader
) {
    if ( !seq || !seq->pulses || seq->count == 0 || !out_leader ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( from_index >= seq->count ) {
        return WAV_ANALYZER_ERROR_NO_LEADER;
    }

    memset ( out_leader, 0, sizeof ( *out_leader ) );

    /*
     * Algoritmus:
     * - udržujeme running average z akumulovaných pulzů
     * - každý nový pulz porovnáme s running average
     * - pokud je v toleranci, přidáme do akumulátoru
     * - pokud není, resetujeme a začneme znovu
     * - leader nalezen, když akumulátor dosáhne min_pulses
     *
     * Kvantizační podlaha: pro rychlé signály (málo vzorků na
     * půl-periodu) je kvantizační chyba ±1 vzorek dominantní.
     * Podlaha zajistí, že max_diff nebude menší než 1 vzorek.
     * Při vysoké frekvenci (přesné pulzy z emulátoru, 17 721 600 Hz)
     * je podlaha zanedbatelná a rozhoduje relativní tolerance; spodní
     * mez v us není potřeba (přesné pulzy kvantizační chybu nemají).
     * Asymetrické pulzy Intercopy 2800/3200 Bd při 44100 Hz ale
     * nezvládne (půlvlny 3-6 vzorků, odchylka nad 0,4 x průměr): pro
     * ně je wav_leader_detect_period().
     *
     * Bootstrap fáze: prvních BOOTSTRAP_COUNT pulzů používá
     * dvojnásobnou toleranci, protože running average je
     * nestabilní s málo vzorky (jeden odlehlý pulz driftuje
     * průměr příliš daleko).
     */
    double quant_floor_us = 1000000.0 / ( double ) seq->sample_rate;

    uint32_t run_start = from_index;
    double run_sum = seq->pulses[from_index].duration_us;
    uint32_t run_count = 1;

    /*
     * Bootstrap: pocet pulzu s dvojnasobnou toleranci.
     * Realne nahravky maji asymetricky duty cycle (HIGH != LOW
     * pul-perioda), takze prvnich min_pulses pulzu pouziva sirsi
     * toleranci. Pro asymetrické pulzy s jitterem kvantizace
     * (Intercopy 2800/3200 Bd při 44100 Hz) to nestačí, viz
     * wav_leader_detect_period().
     * Po dosazeni min_pulses se running average stabilizuje
     * a rozsirovaci smycka pouziva standardni toleranci
     * s kvantizacni podlahou.
     */

    for ( uint32_t i = from_index + 1; i < seq->count; i++ ) {
        double pulse_us = seq->pulses[i].duration_us;
        double avg = run_sum / run_count;

        /* kontrola tolerance s kvantizační podlahou a bootstrap fází */
        double diff = fabs ( pulse_us - avg );
        double eff_tol = ( run_count < min_pulses )
                         ? tolerance * 2.0
                         : tolerance;
        double max_diff = avg * eff_tol;
        if ( max_diff < quant_floor_us ) max_diff = quant_floor_us;

        if ( diff <= max_diff ) {
            /* pulz je v toleranci - přidáme */
            run_sum += pulse_us;
            run_count++;

            /* zkontrolujeme, zda jsme dosáhli minimálního počtu */
            if ( run_count >= min_pulses ) {
                out_leader->start_index = run_start;
                out_leader->pulse_count = run_count;

                /*
                 * Pokračujeme dokud pulzy odpovídají - rozšíříme leader
                 * na maximální délku.
                 */
                for ( uint32_t j = i + 1; j < seq->count; j++ ) {
                    double next_us = seq->pulses[j].duration_us;
                    double cur_avg = run_sum / run_count;
                    double next_diff = fabs ( next_us - cur_avg );
                    double next_max = cur_avg * tolerance;
                    if ( next_max < quant_floor_us ) next_max = quant_floor_us;

                    if ( next_diff <= next_max ) {
                        run_sum += next_us;
                        run_count++;
                        out_leader->pulse_count = run_count;
                    } else {
                        break;
                    }
                }

                /* zpřesníme statistiky */
                leader_refine_stats ( seq, out_leader );
                return WAV_ANALYZER_OK;
            }
        } else {
            /* mimo toleranci - resetujeme */
            run_start = i;
            run_sum = pulse_us;
            run_count = 1;
        }
    }

    return WAV_ANALYZER_ERROR_NO_LEADER;
}


en_WAV_ANALYZER_ERROR wav_leader_find_all (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t min_pulses,
    double tolerance,
    st_WAV_LEADER_LIST *out_list
) {
    if ( !seq || !out_list ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    memset ( out_list, 0, sizeof ( *out_list ) );

    if ( seq->count == 0 ) {
        return WAV_ANALYZER_OK;
    }

    uint32_t search_from = 0;

    while ( search_from < seq->count ) {
        st_WAV_LEADER_INFO leader;
        en_WAV_ANALYZER_ERROR err = wav_leader_detect ( seq, search_from, min_pulses, tolerance, &leader );

        if ( err != WAV_ANALYZER_OK && err != WAV_ANALYZER_ERROR_NO_LEADER ) {
            wav_leader_list_destroy ( out_list );
            return err;
        }

        /*
         * Tón s nesouměrnými půlvlnami (generátor Intercopy NORMAL 2800/3200 podle
         * kódu programu, krátká půlvlna 3-6 vzorků při 44,1 kHz) detektor po půlvlnách
         * rozseká hned za fází s dvojnásobnou tolerancí, nebo nenajde vůbec. Leader
         * po periodách se použije, když začíná dřív (detektor po půlvlnách ho
         * minul), nebo na stejném místě a je aspoň dvakrát delší. Musí mít aspoň
         * 2 * min_pulses půlvln: v datech (FM, Manchester) detektor po periodách
         * projde fází s dvojnásobnou tolerancí a skončí přesně na min_pulses
         * (změřeno na nahrávkách z emulátoru), skutečný tón je delší. Kratší
         * kandidáty přeskočí a hledá dál (tón dalšího souboru za daty). Souměrné tóny
         * najdou oba detektory stejně, výsledek se pro ně nemění.
         */
        st_WAV_LEADER_INFO by_period;
        int have_period = 0;
        /* krátké kandidáty (data) přeskočit a hledat dál, nejvýš do začátku leaderu po půlvlnách */
        for ( uint32_t from = search_from; from < seq->count; ) {
            if ( wav_leader_detect_period ( seq, from, min_pulses, tolerance, &by_period ) != WAV_ANALYZER_OK ) break;
            if ( err == WAV_ANALYZER_OK && by_period.start_index > leader.start_index ) break;
            if ( by_period.pulse_count >= 2 * min_pulses ) {
                have_period = 1;
                break;
            }
            from = by_period.start_index + by_period.pulse_count;
        }
        if ( have_period ) {
            if ( err == WAV_ANALYZER_ERROR_NO_LEADER
                 || by_period.start_index + by_period.pulse_count <= leader.start_index
                 || ( by_period.start_index <= leader.start_index
                      && by_period.start_index + by_period.pulse_count >= leader.start_index + 2 * leader.pulse_count ) ) {
                leader = by_period;
                err = WAV_ANALYZER_OK;
            }
        }

        if ( err == WAV_ANALYZER_ERROR_NO_LEADER ) {
            /* žádný další leader nenalezen - konec */
            break;
        }

        /* přidáme leader do seznamu */
        if ( out_list->count >= out_list->capacity ) {
            uint32_t new_cap = ( out_list->capacity == 0 ) ? LEADER_LIST_INITIAL_CAPACITY : out_list->capacity * 2;
            st_WAV_LEADER_INFO *new_leaders = ( st_WAV_LEADER_INFO* ) realloc (
                                                  out_list->leaders, new_cap * sizeof ( st_WAV_LEADER_INFO )
                                              );
            if ( !new_leaders ) {
                wav_leader_list_destroy ( out_list );
                return WAV_ANALYZER_ERROR_ALLOC;
            }
            out_list->leaders = new_leaders;
            out_list->capacity = new_cap;
        }

        out_list->leaders[out_list->count] = leader;
        out_list->count++;

        /* pokračujeme za koncem nalezeného leaderu */
        search_from = leader.start_index + leader.pulse_count;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_leader_detect_period (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t from_index,
    uint32_t min_pulses,
    double tolerance,
    st_WAV_LEADER_INFO *out_leader
) {
    if ( !seq || !seq->pulses || seq->count == 0 || !out_leader ) return WAV_ANALYZER_ERROR_INVALID_PARAM;
    if ( from_index >= seq->count ) return WAV_ANALYZER_ERROR_NO_LEADER;

    memset ( out_leader, 0, sizeof ( *out_leader ) );

    /* porovnávají se celé periody (součty sousedních půlvln) s průměrem period běhu */
    double quant_floor_us = 1000000.0 / ( double ) seq->sample_rate;

    uint32_t run_start = from_index;
    uint32_t run_count = 1;        /* půlvlny v běhu */
    double pair_sum = 0.0;         /* součet period v běhu */
    uint32_t pair_n = 0;           /* počet period v běhu */

    for ( uint32_t i = from_index + 1; i < seq->count; i++ ) {
        double pair_us = seq->pulses[i - 1].duration_us + seq->pulses[i].duration_us;
        int ok = 1;
        if ( pair_n > 0 ) {
            double avg = pair_sum / pair_n;
            /* bootstrap: do min_pulses dvojnásobná tolerance (jako wav_leader_detect) */
            double eff_tol = ( run_count < min_pulses ) ? tolerance * 2.0 : tolerance;
            double max_diff = avg * eff_tol;
            if ( max_diff < quant_floor_us ) max_diff = quant_floor_us;
            ok = ( fabs ( pair_us - avg ) <= max_diff );
        }
        if ( ok ) {
            pair_sum += pair_us;
            pair_n++;
            run_count++;
            if ( run_count >= min_pulses ) {
                out_leader->start_index = run_start;
                out_leader->pulse_count = run_count;
                /* rozšíření se standardní tolerancí */
                for ( uint32_t j = i + 1; j < seq->count; j++ ) {
                    double next_pair = seq->pulses[j - 1].duration_us + seq->pulses[j].duration_us;
                    double cur_avg = pair_sum / pair_n;
                    double next_max = cur_avg * tolerance;
                    if ( next_max < quant_floor_us ) next_max = quant_floor_us;
                    if ( fabs ( next_pair - cur_avg ) > next_max ) break;
                    pair_sum += next_pair;
                    pair_n++;
                    run_count++;
                    out_leader->pulse_count = run_count;
                }
                leader_refine_stats ( seq, out_leader );
                return WAV_ANALYZER_OK;
            }
        } else {
            run_start = i;
            run_count = 1;
            pair_sum = 0.0;
            pair_n = 0;
        }
    }

    return WAV_ANALYZER_ERROR_NO_LEADER;
}
