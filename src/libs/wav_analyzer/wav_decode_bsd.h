/**
 * @file   wav_decode_bsd.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.3.0
 * @brief  Vrstva 4d analyzeru - BSD dekoder (chunkovany BASIC datovy format).
 *
 * Dekoduje BSD/BRD format z WAV signalu. BSD signal se sklada z:
 *
 * Hlavicka:
 *   LGAP + LTM + 2L + HDR(128B, ftype=0x03/0x04, fsize=0) + CHKH + 2L
 *   + [kopie] + SGAP
 *
 * Chunky (opakovane):
 *   STM + 2L + CHUNK(258B = 2B ID + 256B data) + CHK + 2L
 *
 * Chunk ID:
 * - Prvni chunk: 0x0000
 * - Nasledujici: inkrementuje se (0x0001, 0x0002, ...)
 * - Posledni chunk: 0xFFFF (ukoncujici marker)
 *
 * Vsechny chunky pouzivaji standardni NORMAL FM modulaci.
 *
 * BASIC 1Z-016 (změřeno v emulátoru) zapisuje před
 * každý kus vlastní tón a za kus kopii za oddělovačem 256 S. Dekodér ověří
 * CRC každého kusu a při chybě použije kopii s platným checksumem
 * (wav_decode_fm_try_copy2); nahrávky bez kopií se čtou stejně.
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


#ifndef WAV_DECODE_BSD_H
#define WAV_DECODE_BSD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"


    /** @brief Velikost jednoho BSD chunku (2B ID + 256B data). */
#define WAV_BSD_CHUNK_SIZE          258

    /** @brief Velikost datove casti jednoho chunku. */
#define WAV_BSD_CHUNK_DATA_SIZE     256

    /** @brief Chunk ID posledniho chunku (ukoncujici marker). */
#define WAV_BSD_LAST_CHUNK_ID       0xFFFF

    /** @brief Maximalni pocet chunku (ochrana proti nekonecne smycce). */
#define WAV_BSD_MAX_CHUNKS          256

/**
 * @brief Největší skok ID kusu, který se s allow_partial považuje za chybějící kusy.
 *
 * Větší skok znamená cizí data za koncem souboru.
 */
#define WAV_BSD_MAX_GAP             32

/** @brief Kolik bitů se smí lišit ID kusu s chybou CRC, aby se zařadil na očekávané místo. */
#define WAV_BSD_ID_MAX_FLIPS        1


    /**
     * @brief Údaje o dekódovaném souboru BSD.
     *
     * @invariant crc_errors + from_copy <= chunks
     */
    typedef struct st_WAV_BSD_INFO {
        uint32_t chunks;      /**< počet přečtených kusů */
        uint32_t crc_errors;  /**< kusy bez platné kopie (chyba CRC v 1. kopii i v kopii, nebo kopie chybí) */
        uint32_t from_copy;   /**< kusy, jejichž data jsou z kopie (1. kopie s chybou CRC, kopie OK) */
        uint32_t gap_chunks;  /**< chybějící kusy doplněné nulami (jen s allow_partial) */
    } st_WAV_BSD_INFO;


    /**
     * @brief Dekoduje kompletni MZF soubor z BSD signalu.
     *
     * Predpoklada, ze FM dekoder jiz dekodoval hlavicku
     * (ftype=0x03/0x04, fsize=0). Tato funkce:
     * 1. Najde prvni chunk (STM za SGAP)
     * 2. Iterativne dekoduje chunky (258B = 2B ID + 256B data)
     * 3. Konci pri chunk ID == 0xFFFF nebo pri chybe cteni (pokud allow_partial)
     * 4. Sestavi MZF z hlavicky a zretezenych dat chunku
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param leader Informace o leader tonu hlavicky (pro kalibraci prahu).
     *        Nesmi byt NULL.
     * @param header Dekodovana MZF hlavicka (po endianity korekci).
     *        Nesmi byt NULL.
     * @param search_from_pulse Pozice od ktere hledat chunky
     *        (typicky hdr_res.pulse_end z FM dekoderu).
     * @param allow_partial 1 = pri chybe cteni chunku zachovat dosavadni data
     *        (recovery mod), 0 = pri chybe vratit chybu a zahodit vse. S 1 se navíc
     *        kus s platným CRC a ID o nejvýš WAV_BSD_MAX_GAP vyšším bere jako chybějící
     *        kusy (nuly na jejich místě, WAV_RECOVERY_BSD_GAPS) a kus s chybou CRC, jehož
     *        ID se liší nejvýš o WAV_BSD_ID_MAX_FLIPS bitů, se zařadí na očekávané místo.
     * @param[out] out_mzf Vystupni MZF struktura (volajici uvolni pres mzf_free).
     *             Nesmi byt NULL.
     * @param[out] out_body_result Výsledek dekódování těla: crc_status WAV_CRC_ERROR,
     *             když některý kus nemá platnou kopii (crc_stored/crc_computed
     *             prvního takového kusu), jinak WAV_CRC_OK. Může být NULL.
     * @param[out] out_consumed_until Pozice za poslednim zpracovanym pulzem.
     *             Muze byt NULL.
     * @param[out] out_recovery_status Stav obnovy (bitove OR z en_WAV_RECOVERY_STATUS).
     *             Muze byt NULL. Nastaveno i bez allow_partial (informacni).
     * @param[out] out_info Počty kusů, chyb CRC a použitých kopií.
     *             Může být NULL; při chybě vynulované.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     *
     * @pre FM dekoder jiz dekodoval hlavicku a klasifikator urcil
     *      format jako WAV_TAPE_FORMAT_BSD.
     * @post Pri uspechu *out_mzf obsahuje MZF s hlavickou a zretezenymi daty.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_bsd_decode_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        const st_WAV_LEADER_INFO *leader,
        const st_MZF_HEADER *header,
        uint32_t search_from_pulse,
        int allow_partial,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_body_result,
        uint32_t *out_consumed_until,
        uint32_t *out_recovery_status,
        st_WAV_BSD_INFO *out_info
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_BSD_H */
