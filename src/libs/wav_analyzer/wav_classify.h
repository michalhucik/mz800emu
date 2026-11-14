/**
 * @file   wav_classify.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Vrstva 3 analyzéru - klasifikace formátu kazetového záznamu.
 *
 * Dvoustupňová detekce formátu:
 *
 * Stupeň A - z leader tónu (rychlostní třída):
 *   Průměrná půl-perioda leader tónu určí rychlostní třídu
 *   (DIRECT/FAST/MEDIUM/NORMAL/ERROR).
 *
 * Stupeň B - z obsahu hlavičky (format variant):
 *   Po dekódování hlavičky jako NORMAL FM se testují signatury:
 *   - header[0] == 0xBB -> FASTIPL
 *   - předzavaděč TurboCopy na $D400 (90 B, signatura "[turbo]" v cmnt) -> TURBO
 *   - typ=0x04, fsize=0, fstrt=0, fexec=0 -> BSD
 *
 * Tabulkový přístup z wavdec.c. MZ-80B se automaticky nerozpoznává.
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


#ifndef WAV_CLASSIFY_H
#define WAV_CLASSIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"
#include "wav_histogram.h"


    /**
     * @brief Určí rychlostní třídu z průměrné půl-periody leader tónu.
     *
     * Tabulkový přístup (z wavdec.c):
     *   < 70 us -> DIRECT
     *   70-155 us -> FAST (CPM, FSK vyšší rychlosti)
     *   155-220 us -> MEDIUM (SINCLAIR; MZ-80B 1800 Bd by sem patřil, autodetekce není)
     *   220-500 us -> NORMAL (1200 Bd, TURBO, FASTIPL, BSD, SLOW)
     *   500-1100 us -> ZX (nativní formát ZX Spectra; pilot 2168 T = 619 us
     *                  při 3,5 MHz, Intercopy SINCLAIR 900 Bd 930 us)
     *   > 1100 us -> ERROR
     *
     * @param avg_period_us Průměrná půl-perioda leader tónu (us).
     * @return Rychlostní třída.
     */
    extern en_WAV_SPEED_CLASS wav_classify_speed_class (
        double avg_period_us
    );


    /**
     * @brief Stupeň A - klasifikace formátu z leader tónu.
     *
     * Kombinuje rychlostní třídu z leader tónu s volitelnou
     * histogramovou analýzou pro zpřesnění.
     *
     * @param leader Informace o leader tónu. Nesmí být NULL.
     * @param hist Volitelný histogram datového úseku (může být NULL).
     * @return Předběžný detekovaný formát.
     */
    extern en_WAV_TAPE_FORMAT wav_classify_from_leader (
        const st_WAV_LEADER_INFO *leader,
        const st_WAV_HISTOGRAM *hist
    );


    /**
     * @brief Stupeň B - zpřesnění formátu z obsahu dekódované hlavičky.
     *
     * Po dekódování hlavičky jako NORMAL FM testuje signatury:
     * - header[0] == 0xBB -> FASTIPL
     * - fstrt = fexec = $D400, fsize 90 a cmnt[0..6] = 5B 96 A5 9D 9A B7 5D -> TURBO
     *   (jiný program na $D400 zůstane u předběžného formátu)
     * - turbo_loader vzor + cmnt[3..4]=0xD400 + cmnt[1..2]=191 -> FSK
     * - turbo_loader vzor + cmnt[3..4]=0xD400 + cmnt[1..2]=249 -> SLOW
     * - turbo_loader vzor + cmnt[3..4]=0xD400 + cmnt[1..2]=360 -> DIRECT
     * - turbo_loader vzor (jinak) -> TURBO
     * - ftype=0x04, fsize=0, fstrt=0, fexec=0 -> BSD
     * - ftype=0x22 -> CPM-CMT
     * - jinak -> ponechá formát z stupně A
     *
     * @param header Dekódovaná MZF hlavička. Nesmí být NULL.
     * @param preliminary_format Předběžný formát ze stupně A.
     * @return Zpřesněný formát.
     */
    extern en_WAV_TAPE_FORMAT wav_classify_from_header (
        const st_MZF_HEADER *header,
        en_WAV_TAPE_FORMAT preliminary_format
    );


    /**
     * @brief Vrátí textový název rychlostní třídy.
     *
     * @param speed_class Rychlostní třída.
     * @return Textový název (anglicky).
     */
    extern const char* wav_speed_class_name (
        en_WAV_SPEED_CLASS speed_class
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_CLASSIFY_H */
