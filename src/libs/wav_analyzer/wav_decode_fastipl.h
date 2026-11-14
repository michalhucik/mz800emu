/**
 * @file   wav_decode_fastipl.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.5.0
 * @brief  Vrstva 4c analyzeru - FASTIPL dekoder ($BB prefix, Intercopy).
 *
 * Dekoduje FASTIPL format z WAV signalu. FASTIPL signal se sklada ze dvou casti:
 *
 * Cast 1 ($BB hlavicka, standardni rychlost 1:1):
 *   LGAP + LTM + 2L + BB_HDR(128B, ftype=$BB, fsize=0) + CHKH + 2L
 *   + SGAP + STM + 2L + CHKB(=0) + 2L
 *   $BB hlavicka obsahuje loader a realne parametry:
 *   - offset 0x1A-0x1B: skutecny fsize (LE)
 *   - offset 0x1C-0x1D: skutecny fstrt (LE)
 *   - offset 0x1E-0x1F: skutecny fexec (LE)
 *
 * Pauza (typicky 1000 ms ticha)
 *
 * Cast 2 (datove telo, turbo rychlost):
 *   LGAP + LTM + 2L + BODY(N bajtu) + CHKB + 2L
 *   Pouze surova data - bez hlavicky.
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


#ifndef WAV_DECODE_FASTIPL_H
#define WAV_DECODE_FASTIPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"


    /** @brief Offset skutecneho fsize v $BB hlavicce (od zacatku hlavicky). */
#define WAV_FASTIPL_OFF_FSIZE   0x1A
    /** @brief Offset skutecneho fstrt v $BB hlavicce. */
#define WAV_FASTIPL_OFF_FSTRT   0x1C
    /** @brief Offset skutecneho fexec v $BB hlavicce. */
#define WAV_FASTIPL_OFF_FEXEC   0x1E
    /** @brief Ftype identifikator $BB hlavicky. */
#define WAV_FASTIPL_FTYPE       0xBB
    /** @brief Vychozi ftype pro rekonstruovany MZF (OBJ/program). */
#define WAV_FASTIPL_DEFAULT_FTYPE   0x01


    /**
     * @brief Dekoduje kompletni MZF soubor z FASTIPL signalu.
     *
     * Predpoklada, ze FM dekoder jiz dekodoval $BB hlavicku.
     * Tato funkce:
     * 1. Extrahuje realne parametry z $BB hlavicky
     * 2. Najde body LGAP leader za header CRC
     * 3. Dekoduje telo z body bloku (STM + data + CRC)
     * 4. Sestavi MZF z realnych parametru a dekodovaneho tela
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param bb_header_raw 128B surova $BB hlavicka (pred endianity korekci).
     *        Nesmi byt NULL.
     * @param search_from_pulse Pozice od ktere hledat body LGAP
     *        (typicky hdr_res.pulse_end z FM dekoderu).
     * @param[out] out_mzf Vystupni MZF struktura (volajici uvolni pres mzf_free).
     *             Nesmi byt NULL.
     * @param[out] out_body_result Vysledek dekodovani tela. Muze byt NULL.
     * @param[out] out_consumed_until Pozice za poslednim zpracovanym pulzem.
     *             Muze byt NULL.
     * @param[out] out_data_leader Informace o body LGAP leaderu (rychlost, pozice).
     *             Muze byt NULL.
     * @param[in,out] rec Obnova neúplného těla (wav_decode_fm_read_body()). Může být NULL.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     *
     * @pre FM dekoder jiz dekodoval $BB hlavicku a klasifikator urcil
     *      format jako WAV_TAPE_FORMAT_FASTIPL.
     * @post Pri uspechu *out_mzf obsahuje rekonstruovany MZF soubor
     *       s realnym fsize/fstrt/fexec a dekodovanym telem.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_fastipl_decode_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        const uint8_t *bb_header_raw,
        uint32_t search_from_pulse,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_body_result,
        uint32_t *out_consumed_until,
        st_WAV_LEADER_INFO *out_data_leader,
        st_WAV_BODY_RECOVERY *rec
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_FASTIPL_H */
