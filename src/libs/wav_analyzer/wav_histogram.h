/**
 * @file   wav_histogram.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Vrstva 2b analyzéru - histogramová analýza délek pulzů.
 *
 * Poskytuje diagnostiku a automatické určení prahů z distribuce
 * délek pulzů. Histogram slouží jako pomocný klasifikátor formátu
 * a jako diagnostický nástroj (--histogram flag v CLI).
 *
 * Postup:
 * 1. Sestavení histogramu délek pulzů (bins po konfigurovatelných us)
 * 2. Detekce peaků (lokálních maxim) v histogramu
 * 3. Detekce údolí (automatické prahy mezi peaky)
 * 4. Klasifikace typu modulace z počtu peaků:
 *    - 2 peaky (SHORT/LONG) = FM modulace
 *    - 2 peaky (různé frekvence) = FSK
 *    - 4 peaky = SLOW (kvaternální kódování)
 *    - 1 peak = DIRECT nebo leader tón
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


#ifndef WAV_HISTOGRAM_H
#define WAV_HISTOGRAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>

#include "wav_analyzer_types.h"


    /** @brief Maximální počet peaků detekovatelných v histogramu. */
#define WAV_HISTOGRAM_MAX_PEAKS     8

    /** @brief Typ modulace detekovaný z histogramu. */
    typedef enum en_WAV_MODULATION_TYPE {
        WAV_MODULATION_UNKNOWN = 0, /**< nerozpoznaný typ */
        WAV_MODULATION_FM,          /**< FM - 2 peaky (SHORT/LONG), poměr ~1:1.86 */
        WAV_MODULATION_FSK,         /**< FSK - 2 peaky (různé frekvence) */
        WAV_MODULATION_SLOW,        /**< SLOW - 4 peaky (kvaternální symboly) */
        WAV_MODULATION_DIRECT,      /**< DIRECT - 1 peak (uniformní) */
    } en_WAV_MODULATION_TYPE;


    /**
     * @brief Informace o detekovaném peaku v histogramu.
     */
    typedef struct st_WAV_HISTOGRAM_PEAK {
        double center_us;       /**< střed peaku (us) */
        uint32_t count;         /**< počet pulzů v peaku */
        uint32_t bin_index;     /**< index binu s maximem */
    } st_WAV_HISTOGRAM_PEAK;


    /**
     * @brief Výsledek histogramové analýzy.
     *
     * Obsahuje histogram, detekované peaky, prahy mezi nimi
     * a klasifikaci typu modulace.
     */
    typedef struct st_WAV_HISTOGRAM {
        uint32_t *bins;             /**< pole binů (počty pulzů, alokováno na heapu) */
        uint32_t bin_count;         /**< počet binů */
        double bin_width_us;        /**< šířka jednoho binu (us) */
        double min_pulse_us;        /**< minimální délka pulzu v analyzovaném úseku */
        double max_pulse_us;        /**< maximální délka pulzu v analyzovaném úseku */

        st_WAV_HISTOGRAM_PEAK peaks[WAV_HISTOGRAM_MAX_PEAKS]; /**< nalezené peaky */
        uint32_t peak_count;        /**< počet nalezených peaků */

        double thresholds[WAV_HISTOGRAM_MAX_PEAKS - 1]; /**< prahy mezi peaky (us) */
        uint32_t threshold_count;   /**< počet prahů */

        en_WAV_MODULATION_TYPE modulation; /**< detekovaný typ modulace */
    } st_WAV_HISTOGRAM;


    /**
     * @brief Sestaví histogram délek pulzů z úseku sekvence.
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param from_index Počáteční index v sekvenci.
     * @param count Počet pulzů k analýze (0 = všechny od from_index).
     * @param bin_width_us Šířka binu v mikrosekundách (typicky 5.0).
     * @param[out] out_hist Výstupní histogram. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post Volající musí uvolnit out_hist přes wav_histogram_destroy().
     */
    extern en_WAV_ANALYZER_ERROR wav_histogram_build (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t from_index,
        uint32_t count,
        double bin_width_us,
        st_WAV_HISTOGRAM *out_hist
    );


    /**
     * @brief Detekuje peaky (lokální maxima) v histogramu.
     *
     * Hledá lokální maxima s minimální prominencí (výškou nad okolím).
     * Výsledky zapíše do out_hist->peaks a out_hist->peak_count.
     *
     * @param hist Histogram (musí mít sestavené bins). Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre hist->bins != NULL, hist->bin_count > 0
     * @post hist->peaks a hist->peak_count jsou aktualizovány.
     */
    extern en_WAV_ANALYZER_ERROR wav_histogram_find_peaks (
        st_WAV_HISTOGRAM *hist
    );


    /**
     * @brief Najde prahy (údolí) mezi detekovanými peaky.
     *
     * Pro každý pár sousedních peaků najde minimum histogramu
     * mezi nimi - toto minimum je práh pro rozlišení pulzů.
     *
     * @param hist Histogram s detekovanými peaky. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre hist->peak_count >= 2
     * @post hist->thresholds a hist->threshold_count jsou aktualizovány.
     */
    extern en_WAV_ANALYZER_ERROR wav_histogram_find_thresholds (
        st_WAV_HISTOGRAM *hist
    );


    /**
     * @brief Klasifikuje typ modulace z počtu a poměru peaků.
     *
     * @param hist Histogram s detekovanými peaky. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @post hist->modulation je nastaven.
     */
    extern en_WAV_ANALYZER_ERROR wav_histogram_classify_modulation (
        st_WAV_HISTOGRAM *hist
    );


    /**
     * @brief Provede kompletní histogramovou analýzu.
     *
     * Sestaví histogram, najde peaky, prahy a klasifikuje modulaci.
     *
     * @param seq Sekvence pulzů.
     * @param from_index Počáteční index.
     * @param count Počet pulzů (0 = všechny).
     * @param bin_width_us Šířka binu (us).
     * @param[out] out_hist Výstupní histogram.
     * @return WAV_ANALYZER_OK při úspěchu.
     */
    extern en_WAV_ANALYZER_ERROR wav_histogram_analyze (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t from_index,
        uint32_t count,
        double bin_width_us,
        st_WAV_HISTOGRAM *out_hist
    );


    /**
     * @brief Vypíše histogram na standardní výstup (diagnostika).
     *
     * Textová vizualizace histogramu s peaky a prahy.
     *
     * @param hist Histogram k vypsání.
     * @param stream Výstupní proud (typicky stderr).
     */
    extern void wav_histogram_print (
        const st_WAV_HISTOGRAM *hist,
        FILE *stream
    );


    /**
     * @brief Uvolní paměť alokovanou histogramem.
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param hist Histogram k uvolnění (může být NULL).
     */
    extern void wav_histogram_destroy ( st_WAV_HISTOGRAM *hist );


#ifdef __cplusplus
}
#endif

#endif /* WAV_HISTOGRAM_H */
