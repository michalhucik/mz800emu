/**
 * @file   tmz_part_manchester.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Převod oblasti TLV části rodiny MANCHESTER na parametry generátoru a zpět.
 *
 * Dekódování uplatňuje stejná pravidla spec jako tmz_part_fm
 * (tmz-v1.1-specification.md kap. 5.1, 5.3, 7.2): neznámý povinný tag
 * je chyba, neznámý nepovinný se přeskočí, opakovaný tag (kromě CONTEXT)
 * a známý tag s chybnou délkou nebo hodnotou je chyba (nepovinný se jen
 * přeskočí, u opakování platí první výskyt), HALVES musí být. U známých
 * tagů určuje povinnost tabulka spec, ne bit 15 v souboru. READER se
 * přečte do společných údajů, u rodiny MANCHESTER se ale nekontroluje.
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

#ifndef TMZ_PART_MANCHESTER_H
#define TMZ_PART_MANCHESTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tmz_tlv.h"
#include "tmz_mzfile.h"
#include "tmz_part_fm.h"
#include "../mzcmt_manchester/mzcmt_manchester.h"


    /**
     * @brief Dekóduje oblast TLV části MANCHESTER.
     *
     * @param tlv Oblast TLV části (NULL = prázdná).
     * @param tlv_length Délka oblasti.
     * @param default_timebase Časová základna, není-li v části TIMEBASE (spec 5.3).
     * @param[out] out Parametry generátoru; platné jen při TMZ_PART_OK.
     * @param[out] common STOP a READER; platné jen při TMZ_PART_OK.
     * @return TMZ_PART_OK; TMZ_PART_ERROR_TLV_STRUCTURE (položka přesahuje
     *         oblast), TMZ_PART_ERROR_UNKNOWN_CRITICAL,
     *         TMZ_PART_ERROR_BAD_VALUE (délka nebo hodnota povinného tagu,
     *         opakovaný tag, víc než MZCMT_MANCH_MAX_CONTEXT položek,
     *         neplatné parametry podle mzcmt_manch_params_validate),
     *         TMZ_PART_ERROR_MISSING_PULSES (chybí HALVES).
     *
     * @pre out a common nesmí být NULL.
     * @post Při TMZ_PART_OK projdou parametry v out mzcmt_manch_params_validate();
     *       bez TIMEBASE je out->timebase == default_timebase.
     * @note Nic nealokuje a oblast TLV nemění.
     */
    extern en_TMZ_PART_ERROR tmz_part_manchester_decode ( const uint8_t *tlv, uint32_t tlv_length, uint32_t default_timebase,
                                                          st_MZCMT_MANCH_PARAMS *out, st_TMZ_PART_COMMON *common );

    /**
     * @brief Zakóduje parametry části MANCHESTER do TLV (připojí k bufferu).
     *
     * Pořadí a podmínky: TIMEBASE (když != default_timebase; 0 = vždy), LEVEL
     * (když != 1), PAUSE (> 0; délka 5 s bajtem úrovně jen při pause_level 1),
     * STOP (nepovinný, když stop), READER (nepovinný, když has_reader),
     * HALVES (vždy), PILOT (když pilot_count > 0), SYNC (když sync_bits > 0),
     * FLAG (když has_flag), CHECKSUM (když algoritmus != 0; s explicitními
     * bajty délka 3), END (když end_bits > 0), CARRY (když != 0), CONTEXT
     * (když jsou položky, jedna TLV).
     *
     * @param p Platné parametry.
     * @param common STOP a READER (smí být NULL = nezapisovat).
     * @param default_timebase Výchozí časová základna čtenáře.
     * @param buf Buffer, ke kterému se připojí.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při neplatných parametrech nebo
     *         selhání alokace (buffer pak může obsahovat část položek).
     *
     * @pre p a buf nesmí být NULL; buf je inicializovaný (tmz_tlvbuf_init).
     * @post Při úspěchu tmz_part_manchester_decode() nad připojenými položkami
     *       se stejnou default_timebase vrátí parametry se shodným signálem:
     *       pole, která encode nezapisuje, mají výchozí hodnotu (vzor pilotu
     *       při pilot_count 0, bity sync a konce při počtu 0, checksum_bytes
     *       bez explicitního checksumu).
     */
    extern int tmz_part_manchester_encode ( const st_MZCMT_MANCH_PARAMS *p, const st_TMZ_PART_COMMON *common,
                                            uint32_t default_timebase, st_TMZ_TLVBUF *buf );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_PART_MANCHESTER_H */
