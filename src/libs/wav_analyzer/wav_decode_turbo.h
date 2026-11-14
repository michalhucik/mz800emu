/**
 * @file   wav_decode_turbo.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Vrstva 4b analyzéru - TURBO dekodér (tělo za předzavaděčem).
 *
 * TURBO záznam má dvě části:
 *
 * Část 1 (NORMAL 1:1): předzavaděč, který ROM načte a spustí. TurboCopy
 *   má tělo 90 B na $D400 se signaturou "[turbo]" v cmnt (wav_classify),
 *   mzftools má fsize 0 a zavaděč v cmnt.
 *
 * Část 2 (TURBO rychlost): LGAP + krátký tapemark + tělo + checksum.
 *   Předzavaděč přepne rychlost ROM a čte jen tělo, hlavička se v TURBO
 *   rychlosti nezapisuje; parametry (fsize, fstrt, fexec) nese předzavaděč.
 *
 * Dekodér přijme už dekódovaný předzavaděč (z FM dekodéru), najde část 2
 * a přečte tělo (detektor leaderu nezávislý na rychlosti, mez hledání).
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


#ifndef WAV_DECODE_TURBO_H
#define WAV_DECODE_TURBO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/mzf/mzf.h"

#include "wav_analyzer_types.h"


    /**
     * @brief Dekoduje TurboCopy TURBO body-only signal.
     *
     * TurboCopy TURBO preloader (fsize>0, fstrt=$D400) patchne ROM
     * a vola standardni CMT read rutinu $002A, ktera cte POUZE telo
     * (bez hlavicky). Metadata (fsize/fstrt/fexec) jsou extrahovana
     * z preloader body (90B loader na $D400).
     *
     * Struktura TURBO dat na pasce:
     * - LGAP (leader SHORT pulzy v TURBO rychlosti)
     * - STM tapemark (20 LONG + 20 SHORT pulzu)
     * - Sync (2 LONG hp)
     * - BODY data (fsize bajtu, standardni FM se start bity)
     * - CRC (2 bajty)
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param search_from_pulse Pozice od ktere hledat TURBO LGAP.
     * @param preloader_header Hlavička preloaderu (ftype, fname a cmnt[7..] pro výsledný MZF;
     *        cmnt[0..6] nese signaturu TurboCopy a nahradí se původními 7 B z těla
     *        preloaderu na 53h, je-li tělo dost dlouhé).
     * @param preloader_body Telo preloaderu (90B loader, obsahuje user_params).
     * @param preloader_body_size Velikost tela preloaderu.
     * @param[out] out_mzf Vystupni MZF (volajici uvolni pres mzf_free). Nesmi byt NULL.
     * @param[out] out_body_result Vysledek dekodovani tela. Muze byt NULL.
     * @param[out] out_consumed_until Pozice za poslednim zpracovanym pulzem. Muze byt NULL.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_turbo_turbocopy_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from_pulse,
        const st_MZF_HEADER *preloader_header,
        const uint8_t *preloader_body,
        uint32_t preloader_body_size,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_body_result,
        uint32_t *out_consumed_until,
        st_WAV_LEADER_INFO *out_turbo_leader,
        st_WAV_BODY_RECOVERY *rec
    );


    /**
     * @brief Dekoduje mzftools TURBO body-only signal.
     *
     * mzftools TURBO preloader (fsize=0, loader v comment) pouziva
     * stejny princip jako TurboCopy - patchne ROM a vola RDATA.
     * Metadata (fsize/fstrt/fexec) jsou v comment oblasti preloader hlavicky.
     *
     * @param seq Sekvence pulzu. Nesmi byt NULL.
     * @param search_from_pulse Pozice od ktere hledat TURBO LGAP.
     * @param preloader_header Hlavicka preloaderu (metadata v cmnt).
     * @param[out] out_mzf Vystupni MZF. Nesmi byt NULL.
     * @param[out] out_body_result Vysledek dekodovani tela. Muze byt NULL.
     * @param[out] out_consumed_until Pozice za poslednim pulzem. Muze byt NULL.
     * @param[out] out_turbo_leader Detekovany TURBO leader. Muze byt NULL.
     * @return WAV_ANALYZER_OK pri uspechu, jinak chybovy kod.
     */
    extern en_WAV_ANALYZER_ERROR wav_decode_turbo_mzftools_mzf (
        const st_WAV_PULSE_SEQUENCE *seq,
        uint32_t search_from_pulse,
        const st_MZF_HEADER *preloader_header,
        st_MZF **out_mzf,
        st_WAV_DECODE_RESULT *out_body_result,
        uint32_t *out_consumed_until,
        st_WAV_LEADER_INFO *out_turbo_leader,
        st_WAV_BODY_RECOVERY *rec
    );


#ifdef __cplusplus
}
#endif

#endif /* WAV_DECODE_TURBO_H */
