/**
 * @file   wav_decode_zx.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Vrstva 4 analyzeru - ZX Spectrum nativni dekoder.
 *
 * Dekoduje nativni ZX Spectrum signal z WAV nahravky. Vystupem je
 * surovy TAP blok (flag + payload + checksum), ktery jde primo
 * do TZX bloku 0x10 (Standard Speed Data).
 *
 * Klicove vlastnosti ZX Spectrum signalu:
 * - Pilot: ~612 us pul-perioda, 4032 pulzu (header) / 1610 pulzu (data)
 * - Sync: 2 asymetricke pul-periody (~145 us + ~186 us)
 * - Data: MSB first, bit 1 = 2x ~484 us, bit 0 = 2x ~242 us
 * - Checksum: XOR vsech bajtu (flag + payload + checksum) == 0
 * - Header blok: vzdy 19B (flag + 17B struct + checksum)
 * - Data blok: promenlivy pocet bajtu
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


#ifndef WAV_DECODE_ZX_H
#define WAV_DECODE_ZX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wav_analyzer_types.h"


    /** @brief Velikost ZX Spectrum header bloku v bajtech (flag + 17B header + checksum). */
#define WAV_ZX_HEADER_SIZE      19

    /** @brief Flag bajt pro ZX Spectrum header blok. */
#define WAV_ZX_FLAG_HEADER      0x00

    /** @brief Flag bajt pro ZX Spectrum datovy blok. */
#define WAV_ZX_FLAG_DATA        0xFF

    /** @brief Maximalni pocet pulzu za leaderem pro hledani sync. */
#define WAV_ZX_SYNC_SEARCH_MAX  20

    /** @brief Pocatecni kapacita dynamickeho bufferu pro data blok (bajty). */
#define WAV_ZX_DATA_INITIAL_CAPACITY    4096


    /**
     * @brief Kontext ZX Spectrum dekoderu.
     *
     * Uchovava stav dekodovani: pozici v sekvenci pulzu,
     * kalibraci prahu z pilotu a max platnou delku pul-periody.
     *
     * @par Invarianty:
     * - seq != NULL
     * - threshold_us > 0
     * - data_max_us > threshold_us
     */
    typedef struct st_WAV_ZX_DECODER {
        const st_WAV_PULSE_SEQUENCE *seq;   /**< sekvence pulzu (neni vlastnena) */
        uint32_t pos;                       /**< aktualni pozice v sekvenci */
        double pilot_avg_us;                /**< prumerna pul-perioda pilotu (us) */
        double threshold_us;                /**< prah SHORT/LONG pro datove bity (us) */
        double data_max_us;                 /**< max platna pul-perioda (nad = konec dat) */
    } st_WAV_ZX_DECODER;


    /**
     * @brief Dekoduje jeden ZX Spectrum blok z WAV signalu.
     *
     * Kompletni high-level dekodovani:
     * 1. Inicializace z leaderu (kalibrace prahu)
     * 2. Nalezeni sync pulzu (2 kratke pul-periody za pilotem)
     * 3. Cteni flag bajtu
     * 4. Pro flag==0x00: cteni presne 18 dalsich bajtu (header)
     * 5. Pro flag==0xFF nebo jiny: cteni do konce signalu (data blok)
     * 6. Verifikace XOR checksumu
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param leader Informace o leader tonu. Nesmi byt NULL.
     * @param[out] out_data Vystupni TAP data (flag + payload + checksum, heap).
     *             Volajici uvolni pres free(). Nesmi byt NULL.
     * @param[out] out_size Velikost vystupnich dat v bajtech. Nesmi byt NULL.
     * @param[out] out_crc CRC status (WAV_CRC_OK / WAV_CRC_ERROR). Nesmi byt NULL.
     * @param[out] out_consumed_until Pozice za poslednim zpracovanym pulzem.
     *             Nesmi byt NULL.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     *
     * @pre seq != NULL, leader != NULL
     * @post Pri uspechu *out_data ukazuje na alokovanou pamet,
     *       *out_size > 0, *out_crc je platna hodnota.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_zx_decode_block (
        const st_WAV_PULSE_SEQUENCE *seq,
        const st_WAV_LEADER_INFO *leader,
        uint8_t **out_data,
        uint32_t *out_size,
        en_WAV_CRC_STATUS *out_crc,
        uint32_t *out_consumed_until
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_ZX_H */
