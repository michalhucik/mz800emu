/**
 * @file   wav_decode_cpmtape.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.2.0
 * @brief  Vrstva 4e2 - dekodér CPM-TAPE (Manchester: Pezik/MarVan TAPE.COM, Intercopy "CPM").
 *
 * Dekóduje záznam CPM-TAPE podle disassembly TAPE.COM a měření Intercopy
 * v emulátoru:
 *
 * @verbatim
 *   bit 1 = první půlka HIGH, druhá LOW; bit 0 obráceně (Manchester)
 *   bajt  = 8 bitů LSB první, bez start a stop bitu
 *
 *   [HLAVIČKA]  pilot "011" x N, sync = bit 1, flag 00h, 128 B MZF hlavičky,
 *               checksum L, H (16bitový součet dat bez flagu)
 *               Intercopy: + koncový bit 1, pauza; Pezik: rovnou pilot těla
 *   [TĚLO]      pilot "011" x N, sync, flag 01h, fsize B, checksum L, H
 *               (Intercopy + koncový bit 1)
 * @endverbatim
 *
 * Pilot: Pezik 2134/1067 vzorů, Intercopy 2048/1024. Dekodér počty
 * neověřuje (začátek pilotu se v nahrávce může ztratit), stačí
 * WAV_CPMTAPE_MIN_PATTERNS vzorů. Polarita WAV se pozná z pilotu (vzor
 * 1,0,0 = obrácený signál, typicky export z emulátoru).
 *
 * @par Changelog:
 * - 2026-10-18 (2.0.0): přepis podle disassembly a měření: čistý
 *   Manchester pilot, 8 bitů posledního bajtu, checksum bez
 *   flagu, rozpoznání varianty Pezik / Intercopy a rychlosti.
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


#ifndef WAV_DECODE_CPMTAPE_H
#define WAV_DECODE_CPMTAPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"


    /** @brief Nejméně vzorů "011" za sebou, aby šlo o pilot CPM-TAPE. */
#define WAV_CPMTAPE_MIN_PATTERNS       64

    /**
     * @brief Hranice půlky bitu (µs) mezi Pezik 2400 a 3200.
     *
     * Pezik 2400: půlka bitu 754 T = 212,6 µs, 3200: 559 T = 157,6 µs
     * (výpočet z T-stavů TAPE.COM); hranice uprostřed.
     */
#define WAV_CPMTAPE_PEZIK_3200_MAX_HALF_US  183.0

    /** @brief Očekávaný flag bajt hlavičkového bloku. */
#define WAV_CPMTAPE_FLAG_HEADER        0x00

    /** @brief Očekávaný flag bajt bloku těla. */
#define WAV_CPMTAPE_FLAG_BODY          0x01


    /**
     * @brief Varianta záznamu CPM-TAPE.
     */
    typedef enum en_WAV_CPMTAPE_VARIANT {
        WAV_CPMTAPE_NONE = 0,         /**< nerozpoznáno (jiný formát) */
        WAV_CPMTAPE_PEZIK,            /**< Pezik/MarVan TAPE.COM: bez koncového bitu, tělo hned za hlavičkou */
        WAV_CPMTAPE_INTERCOPY,        /**< Intercopy V10.2 "CPM": koncový bit 1 a pauza za hlavičkou */
    } en_WAV_CPMTAPE_VARIANT;


    /**
     * @brief Údaje o dekódovaném záznamu CPM-TAPE.
     */
    typedef struct st_WAV_CPMTAPE_INFO {
        en_WAV_CPMTAPE_VARIANT variant;   /**< varianta podle koncového bitu za checksumem hlavičky */
        uint16_t bd;                      /**< rychlost podle půlky bitu: Pezik 2400/3200, Intercopy
                                               1500/2400/2800/3200/3600 (nejbližší změřená půlka bitu) */
        double half_us;                   /**< průměrná půlka bitu z pilotu hlavičky (µs) */
        uint32_t pilot_header;            /**< počet přečtených vzorů pilotu hlavičky */
        uint32_t pilot_body;              /**< počet přečtených vzorů pilotu těla (0 bez těla) */
        uint32_t header_start_pulse;      /**< index prvního pulzu pilotu hlavičky */
    } st_WAV_CPMTAPE_INFO;


    /**
     * @brief Najde a dekóduje jeden záznam CPM-TAPE (hlavička a tělo).
     *
     * Od pozice search_from hledá pilot (běhy 2:1:1:2 půlek bitu, aspoň
     * WAV_CPMTAPE_MIN_PATTERNS vzorů), z něj určí půlku bitu, zarovnání
     * a polaritu. Přečte sync, flag 00h, 128 B hlavičky a checksum. Bit 1
     * za checksumem následovaný koncem signálu = Intercopy, jinak Pezik.
     * Délku těla vezme z hlavičky (offset 12h, LE); při fsize > 0 najde
     * pilot těla a přečte flag 01h, tělo a checksum.
     *
     * @param seq Sekvence pulzů. Nesmí být NULL.
     * @param search_from Pozice, od které se hledá pilot.
     * @param[out] out_mzf Výstupní MZF (volající uvolní mzf_free()). Nesmí být NULL.
     * @param[out] out_header_result Výsledek hlavičky (data alokuje funkce). Může být NULL.
     * @param[out] out_body_result Výsledek těla (data = NULL, tělo je v mzf). Může být NULL.
     * @param[out] out_consumed_until Pozice za posledním zpracovaným pulzem; při chybě
     *             těla (pilot nenalezen, porušený Manchester, chybný flag) pozice místa
     *             chyby, odkud může volající hledat další záznam. Může být NULL.
     * @param[out] out_info Varianta, rychlost, počty vzorů. Může být NULL.
     * @param[in,out] rec Obnova neúplného těla: s enable se ponechá tělo, ze kterého se
     *             přečetl aspoň 1 bajt, zbytek je 00h a CRC těla WAV_CRC_ERROR. Může být NULL.
     * @return WAV_ANALYZER_OK (i při chybě CRC, ta je v crc_status);
     *         WAV_ANALYZER_ERROR_NO_LEADER (pilot hlavičky nenalezen);
     *         WAV_ANALYZER_ERROR_DECODE_TAPEMARK (pilot těla nenalezen);
     *         WAV_ANALYZER_ERROR_DECODE_DATA (porušený Manchester, chybný flag);
     *         WAV_ANALYZER_ERROR_ALLOC; WAV_ANALYZER_ERROR_INVALID_PARAM.
     *
     * @post Při úspěchu *out_mzf obsahuje dekódovaný soubor; header_crc i
     *       body_crc mají stav (bez těla body WAV_CRC_NOT_AVAILABLE).
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_cpmtape_decode_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_header_result,
        st_WAV_DECODE_RESULT *out_body_result,
        uint32_t *out_consumed_until,
        st_WAV_CPMTAPE_INFO *out_info,
        st_WAV_BODY_RECOVERY *rec
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_CPMTAPE_H */
