/**
 * @file   wav_histogram.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace vrstvy 2b - histogramová analýza délek pulzů.
 *
 * Sestavuje histogram délek pulzů, detekuje peaky (lokální maxima),
 * nachází prahy (údolí mezi peaky) a klasifikuje typ modulace.
 *
 * Inspirováno přístupy Explorer 85 data recovery a Taper.
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

#include "wav_histogram.h"


/**
 * @brief Minimální prominence peaku (jako podíl z celkového počtu pulzů).
 *
 * Peak musí mít alespoň tuto frakci celkového počtu pulzů,
 * jinak je ignorován jako šum.
 */
#define PEAK_MIN_PROMINENCE_FRACTION    0.02

/** @brief Minimální šířka okolí (binů) pro hledání lokálního maxima. */
#define PEAK_WINDOW_BINS    3

/** @brief FM poměr LONG/SHORT - horní mez pro klasifikaci. */
#define FM_RATIO_MAX    2.5

/** @brief FM poměr LONG/SHORT - dolní mez pro klasifikaci. */
#define FM_RATIO_MIN    1.4


en_WAV_ANALYZER_ERROR wav_histogram_build (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t from_index,
    uint32_t count,
    double bin_width_us,
    st_WAV_HISTOGRAM *out_hist
) {
    if ( !seq || !seq->pulses || !out_hist ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    if ( bin_width_us <= 0.0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    memset ( out_hist, 0, sizeof ( *out_hist ) );
    out_hist->bin_width_us = bin_width_us;

    /* určíme rozsah analýzy */
    uint32_t end_index = ( count == 0 ) ? seq->count : from_index + count;
    if ( end_index > seq->count ) end_index = seq->count;

    if ( from_index >= end_index ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    /* najdeme min a max délku pulzu */
    double min_us = 1e15;
    double max_us = 0.0;

    for ( uint32_t i = from_index; i < end_index; i++ ) {
        double d = seq->pulses[i].duration_us;
        if ( d < min_us ) min_us = d;
        if ( d > max_us ) max_us = d;
    }

    out_hist->min_pulse_us = min_us;
    out_hist->max_pulse_us = max_us;

    /*
     * počet binů: omezit ještě v double, převod na uint32 mimo rozsah
     * (velmi dlouhý pulz nebo malá šířka binu, NaN) by byl nedefinovaný;
     * pro hodnoty v rozsahu je výsledek stejný jako dřív
     */
    double bins_d = ceil ( ( max_us - min_us ) / bin_width_us ) + 1.0;
    uint32_t num_bins = ( bins_d >= 1.0 && bins_d <= WAV_ANALYZER_MAX_HISTOGRAM_BINS )
                        ? ( uint32_t ) bins_d : WAV_ANALYZER_MAX_HISTOGRAM_BINS;

    out_hist->bins = ( uint32_t* ) calloc ( num_bins, sizeof ( uint32_t ) );
    if ( !out_hist->bins ) {
        return WAV_ANALYZER_ERROR_ALLOC;
    }
    out_hist->bin_count = num_bins;

    /* naplníme biny */
    for ( uint32_t i = from_index; i < end_index; i++ ) {
        double d = seq->pulses[i].duration_us;
        double bin_d = ( d - min_us ) / bin_width_us;
        uint32_t bin = ( bin_d >= 0.0 && bin_d < ( double ) num_bins ) ? ( uint32_t ) bin_d : num_bins - 1;
        out_hist->bins[bin]++;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_histogram_find_peaks (
    st_WAV_HISTOGRAM *hist
) {
    if ( !hist || !hist->bins || hist->bin_count == 0 ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    hist->peak_count = 0;

    /* celkový počet pulzů pro výpočet minimální prominence */
    uint32_t total = 0;
    for ( uint32_t i = 0; i < hist->bin_count; i++ ) {
        total += hist->bins[i];
    }

    uint32_t min_count = ( uint32_t ) ( total * PEAK_MIN_PROMINENCE_FRACTION );
    if ( min_count < 2 ) min_count = 2;

    /*
     * Hledání lokálních maxim:
     * Bin je peak, pokud je větší než všechny biny v okolí (PEAK_WINDOW_BINS)
     * a má alespoň min_count pulzů.
     */
    for ( uint32_t i = 0; i < hist->bin_count && hist->peak_count < WAV_HISTOGRAM_MAX_PEAKS; i++ ) {
        if ( hist->bins[i] < min_count ) continue;

        int is_peak = 1;

        /* kontrola okolí vlevo */
        uint32_t left = ( i >= PEAK_WINDOW_BINS ) ? i - PEAK_WINDOW_BINS : 0;
        for ( uint32_t j = left; j < i; j++ ) {
            if ( hist->bins[j] > hist->bins[i] ) {
                is_peak = 0;
                break;
            }
        }

        if ( !is_peak ) continue;

        /* kontrola okolí vpravo */
        uint32_t right = ( i + PEAK_WINDOW_BINS < hist->bin_count ) ? i + PEAK_WINDOW_BINS : hist->bin_count - 1;
        for ( uint32_t j = i + 1; j <= right; j++ ) {
            if ( hist->bins[j] > hist->bins[i] ) {
                is_peak = 0;
                break;
            }
        }

        if ( is_peak ) {
            st_WAV_HISTOGRAM_PEAK *p = &hist->peaks[hist->peak_count];
            p->bin_index = i;
            p->count = hist->bins[i];
            p->center_us = hist->min_pulse_us + ( i + 0.5 ) * hist->bin_width_us;
            hist->peak_count++;
        }
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_histogram_find_thresholds (
    st_WAV_HISTOGRAM *hist
) {
    if ( !hist || !hist->bins ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    hist->threshold_count = 0;

    if ( hist->peak_count < 2 ) {
        return WAV_ANALYZER_OK;
    }

    /* pro každý pár sousedních peaků najdeme minimum (údolí) mezi nimi */
    for ( uint32_t p = 0; p + 1 < hist->peak_count && hist->threshold_count < WAV_HISTOGRAM_MAX_PEAKS - 1; p++ ) {
        uint32_t from_bin = hist->peaks[p].bin_index;
        uint32_t to_bin = hist->peaks[p + 1].bin_index;

        uint32_t min_bin = from_bin;
        uint32_t min_val = hist->bins[from_bin];

        for ( uint32_t b = from_bin + 1; b <= to_bin; b++ ) {
            if ( hist->bins[b] < min_val ) {
                min_val = hist->bins[b];
                min_bin = b;
            }
        }

        hist->thresholds[hist->threshold_count] = hist->min_pulse_us + ( min_bin + 0.5 ) * hist->bin_width_us;
        hist->threshold_count++;
    }

    return WAV_ANALYZER_OK;
}


/**
 * @brief Shlukne blízké peaky do klastrů.
 *
 * Dva peaky patří do stejného klastru, pokud je vzdálenost
 * mezi jejich centry menší než cluster_gap_us.
 *
 * @param hist Histogram s detekovanými peaky.
 * @param cluster_gap_us Minimální mezera mezi klastry (us).
 * @param[out] cluster_count Počet nalezených klastrů.
 * @param[out] cluster_centers Pole středů klastrů (vážený průměr).
 * @param max_clusters Maximální počet klastrů.
 */
static void cluster_peaks (
    const st_WAV_HISTOGRAM *hist,
    double cluster_gap_us,
    uint32_t *cluster_count,
    double *cluster_centers,
    uint32_t max_clusters
) {
    *cluster_count = 0;
    if ( hist->peak_count == 0 ) return;

    double sum_center = hist->peaks[0].center_us * hist->peaks[0].count;
    uint32_t sum_count = hist->peaks[0].count;
    uint32_t clusters = 0;

    for ( uint32_t i = 1; i < hist->peak_count; i++ ) {
        double gap = hist->peaks[i].center_us - hist->peaks[i - 1].center_us;

        if ( gap > cluster_gap_us ) {
            /* nový klastr - uzavřeme předchozí */
            if ( clusters < max_clusters ) {
                cluster_centers[clusters] = sum_center / sum_count;
            }
            clusters++;
            sum_center = hist->peaks[i].center_us * hist->peaks[i].count;
            sum_count = hist->peaks[i].count;
        } else {
            /* rozšíříme aktuální klastr */
            sum_center += hist->peaks[i].center_us * hist->peaks[i].count;
            sum_count += hist->peaks[i].count;
        }
    }

    /* poslední klastr */
    if ( clusters < max_clusters ) {
        cluster_centers[clusters] = sum_center / sum_count;
    }
    clusters++;

    *cluster_count = clusters;
}


en_WAV_ANALYZER_ERROR wav_histogram_classify_modulation (
    st_WAV_HISTOGRAM *hist
) {
    if ( !hist ) {
        return WAV_ANALYZER_ERROR_INVALID_PARAM;
    }

    hist->modulation = WAV_MODULATION_UNKNOWN;

    if ( hist->peak_count == 0 ) {
        return WAV_ANALYZER_OK;
    }

    if ( hist->peak_count == 1 ) {
        hist->modulation = WAV_MODULATION_DIRECT;
        return WAV_ANALYZER_OK;
    }

    /*
     * FM signál může mít 2 nebo 4 peaky:
     * - 2 peaky: SHORT a LONG mají symetrické HIGH/LOW fáze
     * - 4 peaky: SHORT-HIGH, SHORT-LOW, LONG-HIGH, LONG-LOW
     *   (HIGH a LOW fáze mají mírně odlišnou délku)
     *
     * Shluknutím blízkých peaků (< 50 us mezera) zjistíme
     * skutečný počet klastrů. 2 klastry = FM.
     */
    double cluster_centers[WAV_HISTOGRAM_MAX_PEAKS];
    uint32_t cluster_count = 0;

    /*
     * Mezera pro klastrovani: 40% z centru prvniho peaku.
     * Vyssi hodnota (40% misto 30%) spolehliveji shlukne
     * asymetricke HIGH/LOW faze FM pulzu (napr. SHORT-HIGH 150 us
     * a SHORT-LOW 190 us maji mezeru 40 us = 27% z 150, ale < 40%).
     * Minimum 40 us zajisti spravne klastrovani i pro rychle signaly.
     */
    double cluster_gap = hist->peaks[0].center_us * 0.40;
    if ( cluster_gap < 40.0 ) cluster_gap = 40.0;

    cluster_peaks ( hist, cluster_gap, &cluster_count, cluster_centers, WAV_HISTOGRAM_MAX_PEAKS );

    if ( cluster_count == 1 ) {
        hist->modulation = WAV_MODULATION_DIRECT;
    } else if ( cluster_count == 2 ) {
        /* 2 klastry - FM nebo FSK */
        double ratio = cluster_centers[1] / cluster_centers[0];
        if ( ratio >= FM_RATIO_MIN && ratio <= FM_RATIO_MAX ) {
            hist->modulation = WAV_MODULATION_FM;
        } else {
            hist->modulation = WAV_MODULATION_FSK;
        }
    } else if ( cluster_count >= 3 && cluster_count <= 5 ) {
        hist->modulation = WAV_MODULATION_SLOW;
    }

    return WAV_ANALYZER_OK;
}


en_WAV_ANALYZER_ERROR wav_histogram_analyze (
    const st_WAV_PULSE_SEQUENCE *seq,
    uint32_t from_index,
    uint32_t count,
    double bin_width_us,
    st_WAV_HISTOGRAM *out_hist
) {
    en_WAV_ANALYZER_ERROR err;

    err = wav_histogram_build ( seq, from_index, count, bin_width_us, out_hist );
    if ( err != WAV_ANALYZER_OK ) return err;

    err = wav_histogram_find_peaks ( out_hist );
    if ( err != WAV_ANALYZER_OK ) return err;

    err = wav_histogram_find_thresholds ( out_hist );
    if ( err != WAV_ANALYZER_OK ) return err;

    err = wav_histogram_classify_modulation ( out_hist );
    return err;
}


void wav_histogram_print (
    const st_WAV_HISTOGRAM *hist,
    FILE *stream
) {
    if ( !hist || !hist->bins || !stream ) return;

    fprintf ( stream, "=== Pulse Histogram ===\n" );
    fprintf ( stream, "Range: %.1f - %.1f us, bins: %u (%.1f us each)\n",
              hist->min_pulse_us, hist->max_pulse_us,
              hist->bin_count, hist->bin_width_us );

    /* najdeme maximum pro škálování */
    uint32_t max_count = 0;
    for ( uint32_t i = 0; i < hist->bin_count; i++ ) {
        if ( hist->bins[i] > max_count ) max_count = hist->bins[i];
    }

    if ( max_count == 0 ) return;

    /* vykreslíme histogram */
    for ( uint32_t i = 0; i < hist->bin_count; i++ ) {
        if ( hist->bins[i] == 0 ) continue;

        double bin_center = hist->min_pulse_us + ( i + 0.5 ) * hist->bin_width_us;
        int bar_len = ( int ) ( ( ( double ) hist->bins[i] / max_count ) * 50.0 );
        if ( bar_len < 1 && hist->bins[i] > 0 ) bar_len = 1;

        fprintf ( stream, "%7.1f us [%6u] ", bin_center, hist->bins[i] );
        for ( int j = 0; j < bar_len; j++ ) {
            fputc ( '#', stream );
        }
        fputc ( '\n', stream );
    }

    /* vypíšeme peaky */
    if ( hist->peak_count > 0 ) {
        fprintf ( stream, "\nPeaks (%u):\n", hist->peak_count );
        for ( uint32_t i = 0; i < hist->peak_count; i++ ) {
            fprintf ( stream, "  #%u: %.1f us (%u pulses)\n",
                      i + 1, hist->peaks[i].center_us, hist->peaks[i].count );
        }
    }

    /* vypíšeme prahy */
    if ( hist->threshold_count > 0 ) {
        fprintf ( stream, "Thresholds (%u):\n", hist->threshold_count );
        for ( uint32_t i = 0; i < hist->threshold_count; i++ ) {
            fprintf ( stream, "  %.1f us\n", hist->thresholds[i] );
        }
    }

    /* typ modulace */
    const char *mod_names[] = { "UNKNOWN", "FM", "FSK", "SLOW", "DIRECT" };
    fprintf ( stream, "Modulation: %s\n", mod_names[hist->modulation] );
}


void wav_histogram_destroy ( st_WAV_HISTOGRAM *hist ) {
    if ( !hist ) return;
    free ( hist->bins );
    hist->bins = NULL;
    hist->bin_count = 0;
}
