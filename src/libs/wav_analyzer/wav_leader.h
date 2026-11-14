/**
 * @file   wav_leader.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Vrstva 2 analyzéru - detekce leader tónů v sekvenci pulzů.
 *
 * Leader tón je dlouhá sekvence pulzů přibližně konstantní délky,
 * která předchází každému datovému bloku na Sharp MZ pásce.
 * Typické délky: 22000 pulzů (hlavička), 11000 pulzů (tělo).
 *
 * Algoritmus detekce je inspirován Intercopy V10.2 (adaptivní kalibrace
 * z 131 pulzů, konzistence 6 T-states) a wavdec.c (running average
 * s 35% tolerancí, min. 2000 pulzů).
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


#ifndef WAV_LEADER_H
#define WAV_LEADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /**
     * @brief Najde jeden leader tón počínaje daným indexem.
     *
     * Prochází sekvenci pulzů od from_index a hledá dostatečně
     * dlouhý úsek pulzů se shodnou délkou (v rámci tolerance).
     * Tolerance je relativní vzhledem k running average.
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param from_index Index pulzu, od kterého začít hledání.
     * @param min_pulses Minimální počet pulzů pro detekci leaderu.
     * @param tolerance Relativní tolerance (0.02-0.35).
     * @param[out] out_leader Výstupní informace o nalezeném leaderu.
     *             Nesmí být NULL.
     * @return WAV_ANALYZER_OK pokud nalezen leader,
     *         WAV_ANALYZER_ERROR_NO_LEADER pokud nenalezen,
     *         jiný kód při chybě.
     *
     * @pre seq->count > 0, seq->pulses != NULL
     * @post Při úspěchu out_leader obsahuje platné statistiky.
     */
    extern en_WAV_ANALYZER_ERROR wav_leader_detect (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t from_index,
        uint32_t min_pulses,
        double tolerance,
        st_WAV_LEADER_INFO *out_leader
    );


    /**
     * @brief Najde leader tón podle celých period (dvojic sousedních půlvln).
     *
     * Stejný kontrakt jako wav_leader_detect(), ale s průměrem běhu se
     * porovnává součet každých dvou sousedních půlvln (perioda), ne
     * jednotlivá půlvlna. Asymetrické pulzy s jitterem kvantizace
     * (Intercopy 2800/3200 Bd při 44100 Hz: půlvlny 3 až 6 vzorků kolem
     * průměru 4,3) mají periody stálé, takže leader najde i tam, kde
     * wav_leader_detect() běh pořád nuluje. Počty a statistiky
     * ve výsledku jsou v půlvlnách jako u wav_leader_detect().
     *
     * Používá se jen v tělech FASTIPL a TURBO: globální náhrada by ve
     * skutečných nahrávkách násobila falešné leadery (měřeno).
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param from_index Index pulzu, od kterého začít hledání.
     * @param min_pulses Minimální počet půlvln leaderu.
     * @param tolerance Relativní tolerance periody (0.02-0.35).
     * @param[out] out_leader Výstupní informace o leaderu. Nesmí být NULL.
     * @return WAV_ANALYZER_OK pokud nalezen leader,
     *         WAV_ANALYZER_ERROR_NO_LEADER pokud nenalezen,
     *         WAV_ANALYZER_ERROR_INVALID_PARAM při neplatném vstupu.
     *
     * @pre seq->count > 0, seq->pulses != NULL
     * @post Při úspěchu out_leader obsahuje platné statistiky.
     */
    extern en_WAV_ANALYZER_ERROR wav_leader_detect_period (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t from_index,
        uint32_t min_pulses,
        double tolerance,
        st_WAV_LEADER_INFO *out_leader
    );


    /**
     * @brief Najde všechny leader tóny v sekvenci pulzů.
     *
     * Opakovaně volá wav_leader_detect() od konce předchozího
     * leaderu dokud nejsou prohledány všechny pulzy. Na každém místě zkusí
     * i wav_leader_detect_period(); leader po periodách s aspoň
     * 2 * min_pulses půlvlnami se použije, když leader po půlvlnách chybí,
     * začíná až za ním, nebo začíná na stejném místě a je nejvýš poloviční
     * (tón s nesouměrnými půlvlnami, např. generátor Intercopy NORMAL
     * 2800/3200 podle kódu programu).
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param min_pulses Minimální počet pulzů pro detekci leaderu.
     * @param tolerance Relativní tolerance (0.02-0.35).
     * @param[out] out_list Seznam nalezených leader tónů.
     *             Struktura musí být nainicializovaná na nulu.
     *             Nesmí být NULL.
     * @return WAV_ANALYZER_OK při úspěchu (i 0 nalezených),
     *         jiný kód při chybě.
     *
     * @post out_list->count obsahuje počet nalezených leader tónů.
     */
    extern en_WAV_ANALYZER_ERROR wav_leader_find_all (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t min_pulses,
        double tolerance,
        st_WAV_LEADER_LIST *out_list
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_LEADER_H */
