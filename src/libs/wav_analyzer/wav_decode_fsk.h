/**
 * @file   wav_decode_fsk.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Vrstva 4e3 analyzeru - FSK dekoder (Frequency Shift Keying).
 *
 * Dekoduje FSK signal z WAV nahravky. FSK je format pouzivany programem
 * mzf2snd (mzftools) pro rychly prenos dat na kazetach Sharp MZ.
 *
 * Klicove vlastnosti FSK formatu:
 * - Kazdy bit = 1 cely cyklus (LOW+HIGH) ruzne delky
 * - Bit "1" = long cyklus (nizsi frekvence), bit "0" = short cyklus (vyssi frekvence)
 * - 8 cyklu na bajt, MSB first, bez stop bitu
 * - 7 rychlostnich urovni (speed 0-6, pomer long:short 1.50 - 2.50)
 * - Checksum neni soucasti signalu (je v Z80 loaderu)
 *
 * @par Struktura signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8L + 8H vzorku na cyklus pri 44100 Hz)
 *   [POLARITY]  1 asymetricky cyklus (4L + 2H)
 *   [SYNC]      1 kratky cyklus (2L + 2H)
 *   [DATA]      N bajtu (8 FSK cyklu na bajt, MSB first)
 *   [FADEOUT]   8 cyklu s rostouci sirkou
 * @endverbatim
 *
 * Dekoder hleda GAP (sekvenci pulzu s konstantni periodou), preskoci
 * polarity a sync, zkalibruje prah z prvnich datovych cyklu a dekoduje
 * bajty az do fadeoutu.
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


#ifndef WAV_DECODE_FSK_H
#define WAV_DECODE_FSK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /**
     * @brief Minimalni pocet po sobe jdoucich podobnych pulzu pro detekci GAPu.
     *
     * GAP ma 400 pul-period, ale degradovany signal muze mit mene.
     * 80 je bezpecny dolni limit.
     */
#define WAV_FSK_MIN_GAP_PULSES         80

    /**
     * @brief Tolerance pro shodu pulzu v GAPu (30 %).
     *
     * Pulzy v GAPu by mely mit konstantni periodu. Povolujeme
     * 30% odchylku od prumeru pro robustnost.
     */
#define WAV_FSK_GAP_TOLERANCE          0.30

    /**
     * @brief Pocet kalibracnich cyklu (celych, ne pul-period).
     *
     * Z prvnich 16 datovych cyklu (= 2 bajty) se kalibruje
     * prah pro rozliseni short/long pulzu.
     */
#define WAV_FSK_CALIBRATION_CYCLES     16

    /**
     * @brief Nasobitel GAP periody pro detekci fadeoutu.
     *
     * Fadeout zacina, kdyz je cyklus delsi nez 2x prumerna GAP perioda.
     * Fadeout pulzy jsou 32-46 vzorku pri 44100 Hz, zatimco GAP pulzy
     * jsou 8 vzorku - pomer > 4x. Prah 2x je konzervativni.
     */
#define WAV_FSK_FADEOUT_FACTOR         2.0


    /**
     * @brief Dekoduje surove bajty z FSK signalu.
     *
     * Prohledava sekvenci pulzu od pozice @p search_from_pulse,
     * najde GAP, preskoci polarity a sync, zkalibruje prah
     * z prvnich datovych cyklu a dekoduje bajty az do fadeoutu.
     *
     * Algoritmus:
     * 1. Najde GAP (>= WAV_FSK_MIN_GAP_PULSES podobnych pulzu)
     * 2. Preskoci polarity (2 pul-periody) + sync (2 pul-periody)
     * 3. Zmeri prvnich WAV_FSK_CALIBRATION_CYCLES celych cyklu
     * 4. Setridí delky, najde mezeru mezi short a long klastry
     * 5. Threshold = stred mezery
     * 6. Dekoduje bajty: cyklus >= threshold -> bit 1, jinak bit 0
     * 7. Konci pri fadeoutu (cyklus > GAP_period * FADEOUT_FACTOR)
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param search_from_pulse Pozice od ktere zacit hledat GAP.
     * @param[out] out_data Vystupni pole dekodovanych bajtu (alokuje se na heapu).
     *             Volajici uvolni pres free(). Nesmi byt NULL.
     * @param[out] out_size Pocet dekodovanych bajtu. Nesmi byt NULL.
     * @param[out] out_consumed_until Pozice za poslednim zpracovanym pulzem.
     *             Muze byt NULL.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     *
     * @post Pri uspechu *out_data ukazuje na alokovanou pamet s daty,
     *       *out_size > 0.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fsk_decode_bytes (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from_pulse,
        uint8_t **out_data,
        uint32_t *out_size,
        uint32_t *out_consumed_until
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_FSK_H */
