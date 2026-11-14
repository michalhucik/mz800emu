/**
 * @file   wav_decode_direct.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Vrstva 4e5 analyzeru - DIRECT dekoder (primy bitovy zapis).
 *
 * Dekoduje DIRECT signal z WAV nahravky. DIRECT je nejrychlejsi metoda
 * prenosu dat na kazetach Sharp MZ - kazdy datovy bit je 1 audio vzorek.
 *
 * Klicove vlastnosti DIRECT formatu:
 * - 1 audio vzorek = 1 bit (HIGH=1, LOW=0)
 * - 12 vzorku na bajt: 8 datovych + 4 synchronizacnich
 * - Vzor: D7 D6 ~D6 D5 D4 ~D4 D3 D2 ~D2 D1 D0 ~D0
 * - Synchro bit = opak posledniho datoveho bitu (po kazdem 2. bitu)
 * - MSB first, bez stop bitu
 * - Checksum neni soucasti signalu (je v Z80 loaderu)
 * - Nema rychlostni urovne (rychlost = sample_rate / 12 bajtu/s)
 *
 * @par Struktura signalu:
 * @verbatim
 *   [GAP]       200 cyklu (8L + 8H vzorku na cyklus pri 44100 Hz)
 *   [POLARITY]  1 asymetricky cyklus (4L + 2H)
 *   [SYNC]      1 kratky cyklus (2L + 2H)
 *   [DATA]      N bajtu (12 vzorku na bajt)
 *   [FADEOUT]   8 cyklu s rostouci sirkou
 * @endverbatim
 *
 * Dekoder pracuje na urovni vzorku (duration_samples v pulzech),
 * protoze pulse extractor slucuje po sobe jdouci vzorky stejne hodnoty
 * do jednoho pulzu. Pocet vzorku v pulzu urcuje pocet elementu.
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


#ifndef WAV_DECODE_DIRECT_H
#define WAV_DECODE_DIRECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /**
     * @brief Minimalni pocet po sobe jdoucich podobnych pulzu pro detekci GAPu.
     */
#define WAV_DIRECT_MIN_GAP_PULSES      80

    /** @brief Tolerance pro shodu pulzu v GAPu (30 %). */
#define WAV_DIRECT_GAP_TOLERANCE       0.30

    /**
     * @brief Pocet referencnich vzorku pro polarity+sync frame.
     *
     * Polarity: 4L + 2H = 6 vzorku, Sync: 2L + 2H = 4 vzorku.
     * Celkem 10 vzorku pri referencni frekvenci 44100 Hz.
     * Skaluje se na cilovy rate.
     */
#define WAV_DIRECT_FRAME_SKIP_REF      10

    /** @brief Referencni frekvence pro skalovani frame parametru. */
#define WAV_DIRECT_REF_RATE            44100

    /** @brief Pocet vzorku na bajt v DIRECT kodovani (8 dat + 4 synchro). */
#define WAV_DIRECT_SAMPLES_PER_BYTE    12


    /**
     * @brief Dekoduje surove bajty z DIRECT signalu.
     *
     * Prohledava sekvenci pulzu od pozice @p search_from_pulse,
     * najde GAP, preskoci polarity a sync (pocet vzorku dle sample rate),
     * a dekoduje bajty expanzi pulzu na jednotlive vzorky.
     *
     * Algoritmus:
     * 1. Najde GAP (>= WAV_DIRECT_MIN_GAP_PULSES podobnych pulzu)
     * 2. Spocita celkovy pocet vzorku pro polarity+sync frame
     *    a preskoci odpovidajici pocet vzorku v pulzech
     * 3. Expanduje pulzy na individualni vzorky (dle duration_samples)
     * 4. Cte vzorky po 12: D7 D6 ~D6 D5 D4 ~D4 D3 D2 ~D2 D1 D0 ~D0
     * 5. Konci pri vycerpani pulzu nebo pri fadeoutu
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
    extern en_WAV_ANALYZER_ERROR wav_decode_direct_decode_bytes (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from_pulse,
        uint8_t **out_data,
        uint32_t *out_size,
        uint32_t *out_consumed_until
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_DIRECT_H */
