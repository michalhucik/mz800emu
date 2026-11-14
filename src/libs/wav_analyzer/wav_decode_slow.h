/**
 * @file   wav_decode_slow.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Vrstva 4e4 analyzeru - SLOW dekoder (kvaternalni kodovani).
 *
 * Dekoduje SLOW signal z WAV nahravky. SLOW je format pouzivany programem
 * mzf2snd (mzftools) pro kompaktni prenos dat na kazetach Sharp MZ.
 *
 * Klicove vlastnosti SLOW formatu:
 * - Kazdy symbol = 1 cely cyklus (LOW+HIGH), 4 mozne delky
 * - 2 bity na symbol (00, 01, 10, 11), 4 symboly na bajt, MSB first
 * - 5 rychlostnich urovni (speed 0-4)
 * - Checksum neni soucasti signalu (je v Z80 loaderu)
 *
 * @par Struktura signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8L + 8H vzorku na cyklus pri 44100 Hz)
 *   [POLARITY]  1 asymetricky cyklus (4L + 2H)
 *   [SYNC]      1 kratky cyklus (2L + 2H)
 *   [DATA]      N bajtu (4 SLOW cykly na bajt, MSB first, po 2 bitech)
 *   [FADEOUT]   8 cyklu s rostouci sirkou
 * @endverbatim
 *
 * Dekoder hleda GAP, preskoci polarity a sync, zkalibruje prahy
 * ze 4 klastru prvnich datovych cyklu a dekoduje bajty az do fadeoutu.
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


#ifndef WAV_DECODE_SLOW_H
#define WAV_DECODE_SLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /**
     * @brief Minimalni pocet po sobe jdoucich podobnych pulzu pro detekci GAPu.
     */
#define WAV_SLOW_MIN_GAP_PULSES        80

    /** @brief Tolerance pro shodu pulzu v GAPu (30 %). */
#define WAV_SLOW_GAP_TOLERANCE         0.30

    /**
     * @brief Pocet kalibracnich cyklu (celych, ne pul-period).
     *
     * Z prvnich 16 datovych cyklu (= 4 bajty) se kalibruje
     * 4 klastry pro rozliseni symbolu 00/01/10/11.
     */
#define WAV_SLOW_CALIBRATION_CYCLES    16

    /** @brief Nasobitel GAP periody pro detekci fadeoutu. */
#define WAV_SLOW_FADEOUT_FACTOR        2.0


    /**
     * @brief Dekoduje surove bajty ze SLOW signalu.
     *
     * Prohledava sekvenci pulzu od pozice @p search_from_pulse,
     * najde GAP, preskoci polarity a sync, zkalibruje prahy
     * ze 4 klastru a dekoduje bajty az do fadeoutu.
     *
     * Algoritmus:
     * 1. Najde GAP (>= WAV_SLOW_MIN_GAP_PULSES podobnych pulzu)
     * 2. Preskoci polarity (2 pul-periody) + sync (2 pul-periody)
     * 3. Zmeri prvnich WAV_SLOW_CALIBRATION_CYCLES celych cyklu
     * 4. Setridí delky, klasteruje do 4 skupin, urci 3 prahy
     * 5. Dekoduje bajty: 4 symboly na bajt, MSB first
     * 6. Konci pri fadeoutu (cyklus > GAP_period * FADEOUT_FACTOR)
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
    extern en_WAV_ANALYZER_ERROR wav_decode_slow_decode_bytes (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from_pulse,
        uint8_t **out_data,
        uint32_t *out_size,
        uint32_t *out_consumed_until
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_SLOW_H */
