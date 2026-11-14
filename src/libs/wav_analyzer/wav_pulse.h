/**
 * @file   wav_pulse.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Vrstva 1 analyzéru - extrakce pulzů ze signálu.
 *
 * Převádí předspracovaný float buffer na sekvenci pulzů (půl-period).
 * Každý pulz je definován délkou trvání v mikrosekundách a úrovní signálu.
 *
 * Dva režimy detekce:
 *   - Zero-crossing: jednoduchý, přesný pro čisté signály
 *   - Schmitt trigger: odolnější proti šumu díky hystereznímu přepínání
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


#ifndef WAV_PULSE_H
#define WAV_PULSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /**
     * @brief Extrahuje pulzy ze signálu metodou zero-crossing.
     *
     * Detekuje přechody signálu přes nulovou úroveň. Měří čas
     * (v počtu vzorků) mezi dvěma po sobě jdoucími přechody.
     * Každý takový interval je jeden pulz (půl-perioda).
     *
     * @param samples Float buffer se vzorky. Nesmí být NULL.
     * @param sample_count Počet vzorků. Musí být > 0.
     * @param sample_rate Vzorkovací frekvence zdrojového WAV (Hz).
     * @param polarity Polarita signálu.
     * @param[out] out_seq Výstupní sekvence pulzů. Nesmí být NULL.
     *             Struktura musí být nainicializovaná na nulu.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre samples obsahuje platná float data.
     * @post out_seq obsahuje extrahované pulzy s délkami v us i vzorcích.
     */
    extern en_WAV_ANALYZER_ERROR wav_pulse_extract_zero_crossing (
        const double *samples,
        uint32_t sample_count,
        uint32_t sample_rate,
        en_WAV_SIGNAL_POLARITY polarity,
        st_WAV_PULSE_SEQUENCE *out_seq
    );


    /**
     * @brief Extrahuje pulzy ze signálu metodou Schmitt trigger.
     *
     * Používá dva prahové hodnoty (horní a dolní) pro hysterezní
     * přepínání mezi stavy HIGH a LOW. Signál se přepne na HIGH
     * až po překročení horního prahu, na LOW až po klesnutí pod dolní.
     *
     * Odolnější proti šumu než zero-crossing díky hysterezní mezeře.
     *
     * @param samples Float buffer se vzorky. Nesmí být NULL.
     * @param sample_count Počet vzorků. Musí být > 0.
     * @param sample_rate Vzorkovací frekvence zdrojového WAV (Hz).
     * @param polarity Polarita signálu.
     * @param threshold_high Horní práh (typicky +0.1).
     * @param threshold_low Dolní práh (typicky -0.1).
     * @param[out] out_seq Výstupní sekvence pulzů. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     *
     * @pre threshold_high > threshold_low
     * @post out_seq obsahuje extrahované pulzy.
     */
    extern en_WAV_ANALYZER_ERROR wav_pulse_extract_schmitt (
        const double *samples,
        uint32_t sample_count,
        uint32_t sample_rate,
        en_WAV_SIGNAL_POLARITY polarity,
        double threshold_high,
        double threshold_low,
        st_WAV_PULSE_SEQUENCE *out_seq
    );


    /**
     * @brief Extrahuje pulzy z předspracovaného signálu dle konfigurace.
     *
     * Vybere metodu detekce (zero-crossing nebo Schmitt trigger)
     * podle nastavení v konfiguraci a zavolá příslušnou funkci.
     *
     * @param samples Float buffer se vzorky. Nesmí být NULL.
     * @param sample_count Počet vzorků. Musí být > 0.
     * @param sample_rate Vzorkovací frekvence (Hz).
     * @param config Konfigurace analyzéru. Nesmí být NULL.
     * @param[out] out_seq Výstupní sekvence pulzů. Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu, jinak chybový kód.
     */
    extern en_WAV_ANALYZER_ERROR wav_pulse_extract (
        const double *samples,
        uint32_t sample_count,
        uint32_t sample_rate,
        const st_WAV_ANALYZER_CONFIG *config,
        st_WAV_PULSE_SEQUENCE *out_seq
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_PULSE_H */
