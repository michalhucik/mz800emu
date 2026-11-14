/**
 * @file   tmz_part_fm.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.1
 * @brief  Převod oblasti TLV části rodiny FM na parametry generátoru a zpět.
 *
 * Dekódování uplatňuje pravidla spec (tmz-v1.1-specification.md kap.
 * 5.1, 5.3, 6.1): neznámý povinný tag je chyba, neznámý nepovinný se
 * přeskočí, opakovaný tag (kromě CONTEXT) a známý tag s chybnou délkou
 * nebo hodnotou je chyba (nepovinný se jen přeskočí, u opakování platí
 * první výskyt), PULSES musí být. U známých tagů určuje povinnost tabulka
 * spec, ne bit 15 v souboru.
 * Výsledek ověří mzcmt_fm_params_validate().
 *
 * Kódování zapisuje jen tagy s nevýchozí hodnotou v pevném pořadí, takže
 * dekódování a nové zakódování dá bajtově shodnou oblast (testovací
 * vektory tstdata/tmz-v2).
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
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


#ifndef TMZ_PART_FM_H
#define TMZ_PART_FM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tmz_tlv.h"
#include "tmz_mzfile.h"
#include "../mzcmt_fm/mzcmt_fm.h"


    /**
     * @brief Společné nepovinné údaje části, které generátor nepotřebuje
     *        (spec 5.3: STOP, READER). TIMEBASE, LEVEL a PAUSE jsou
     *        v st_MZCMT_FM_PARAMS.
     *
     * @invariant has_reader == 0: reader_routine a reader_n nemají význam
     */
    typedef struct st_TMZ_PART_COMMON {
        uint8_t stop;            /**< 1 = po části (a pauze) zastavit pásku */
        uint8_t has_reader;      /**< 1 = READER přítomen a platný */
        uint8_t reader_routine;  /**< READER: čtecí rutina (spec 5.6) */
        uint16_t reader_n;       /**< READER: konstanta N */
    } st_TMZ_PART_COMMON;


    /**
     * @brief Dekóduje oblast TLV části FM.
     *
     * @param tlv Oblast TLV části (NULL = prázdná).
     * @param tlv_length Délka oblasti.
     * @param default_timebase Časová základna, není-li v části TIMEBASE (spec 5.3).
     * @param[out] out Parametry generátoru; platné jen při TMZ_PART_OK.
     * @param[out] common STOP a READER; platné jen při TMZ_PART_OK.
     * @return TMZ_PART_OK; TMZ_PART_ERROR_TLV_STRUCTURE (položka přesahuje
     *         oblast), TMZ_PART_ERROR_UNKNOWN_CRITICAL,
     *         TMZ_PART_ERROR_BAD_VALUE (délka nebo hodnota povinného tagu,
     *         opakovaný tag, neplatné parametry podle mzcmt_fm_params_validate),
     *         TMZ_PART_ERROR_MISSING_PULSES.
     *
     * @pre out a common nesmí být NULL.
     * @post Při TMZ_PART_OK projdou parametry v out mzcmt_fm_params_validate();
     *       bez TIMEBASE je out->timebase == default_timebase.
     * @note Nic nealokuje a oblast TLV nemění.
     */
    extern en_TMZ_PART_ERROR tmz_part_fm_decode ( const uint8_t *tlv, uint32_t tlv_length, uint32_t default_timebase,
                                                  st_MZCMT_FM_PARAMS *out, st_TMZ_PART_COMMON *common );

    /**
     * @brief Zakóduje parametry části FM do TLV (připojí k bufferu).
     *
     * Pořadí a podmínky: TIMEBASE (když != default_timebase; 0 = vždy), LEVEL (když
     * != 1), PAUSE (> 0; délka 5 s bajtem úrovně jen při pause_level 1), STOP (nepovinný, když stop), READER (nepovinný,
     * když has_reader), PULSES (vždy), TONE (> 0), TAPEMARK (když některé
     * číslo > 0), FRAMING (když != 1 start bit, MSB), CHECKSUM (když
     * algoritmus != 0; s explicitními bajty délka 3), END (když některé
     * číslo > 0), COPIES (> 0), CONTEXT (když jsou položky, jedna TLV).
     *
     * @param p Platné parametry.
     * @param common STOP a READER (smí být NULL = nezapisovat).
     * @param default_timebase Výchozí časová základna čtenáře.
     * @param buf Buffer, ke kterému se připojí.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při neplatných parametrech nebo
     *         selhání alokace (buffer pak může obsahovat část položek).
     *
     * @pre p a buf nesmí být NULL; buf je inicializovaný (tmz_tlvbuf_init).
     * @post Při úspěchu tmz_part_fm_decode() nad připojenými položkami se
     *       stejnou default_timebase vrátí shodné parametry.
     */
    extern int tmz_part_fm_encode ( const st_MZCMT_FM_PARAMS *p, const st_TMZ_PART_COMMON *common,
                                    uint32_t default_timebase, st_TMZ_TLVBUF *buf );

    /** @brief Nejmenší přípustná rezerva čtecího bodu v µs. */
#define TMZ_READER_MIN_MARGIN_US 10.0

    /**
     * @brief Rezervy čtecího bodu části FM (spec 5.6), v µs.
     *
     * Kladná hodnota = podmínka splněna s danou rezervou.
     */
    typedef struct st_TMZ_READER_MARGINS {
        double short_high_us;     /**< (86+14N) - nejdelší první půlvlna SHORT */
        double long_high_us;      /**< nejkratší první půlvlna LONG - (138+14N) */
        double short_period_us;   /**< nejkratší perioda SHORT - (256+14N) */
    } st_TMZ_READER_MARGINS;

    /**
     * @brief Výsledek kontroly čtecího bodu (tmz_part_fm_check_reader).
     */
    typedef enum en_TMZ_READER_CHECK {
        TMZ_READER_SKIPPED = -1,     /**< nekontrolováno: bez READER, neznámá rutina, NULL parametry */
        TMZ_READER_UNREADABLE = 0,   /**< některá rezerva < 0: čtecí rutina signál nepřečte (chyba) */
        TMZ_READER_LOW_MARGIN = 1,   /**< všechny rezervy >= 0, některá < TMZ_READER_MIN_MARGIN_US (varování) */
        TMZ_READER_OK = 2,           /**< všechny rezervy >= TMZ_READER_MIN_MARGIN_US */
    } en_TMZ_READER_CHECK;

    /**
     * @brief Zkontroluje rezervy čtecího bodu podle READER části.
     *
     * Model čtecí rutiny ROM (spec 5.6, mzcmt_turbo_check_readpoint):
     * vzorek 86+14N+(0..52) T po hraně; SHORT musí být přečten jako 0
     * i v nejdřívějším vzorku, LONG jako 1 i v nejpozdějším, perioda SHORT
     * musí přesáhnout 256+14N T. Rutina 0 = ROM s N = 82, rutina 1 = táž
     * rutina s N z READER, rutina 2 = rutina 1 se zavaděčem FAST IPL V07
     * (Intercopy), který po každé náběžné hraně přidá 86 T (všechny meze
     * o 86 T dál, disassembly Intercopy 10.2). Délky se převedou na T taktu MZ-800
     * (3 546 895 Hz) a vyhodnotí se nejhorší případ přes PULSES a všechny
     * kontextové položky téhož druhu.
     *
     * Při rate > 0 se délky nejdřív převedou na celé vzorky s nejhorším
     * zaokrouhlením. Player klade hrany na vzorek zaokrouhlením
     * akumulovaného času (spec 5.4), takže úsek x jednotek má floor nebo
     * ceil(x·rate/timebase) vzorků: první půlvlna SHORT se počítá s ceil,
     * první půlvlna LONG a perioda SHORT s floor.
     *
     * @param p Parametry části (platné).
     * @param c STOP a READER části.
     * @param rate Výstupní vzorkovací frekvence v Hz; 0 = nominální délky
     *        bez převodu na vzorky (tape lint).
     * @param[out] m Rezervy (smí být NULL); platné jen při návratu jiném
     *        než TMZ_READER_SKIPPED.
     * @return Výsledek kontroly; rezerva přesně 0 se počítá jako čitelná
     *         (TMZ_READER_LOW_MARGIN).
     *
     * @pre p a c nesmí být NULL; p prošly mzcmt_fm_params_validate()
     *      (např. z tmz_part_fm_decode()).
     * @post Nic nealokuje, parametry nemění.
     */
    extern en_TMZ_READER_CHECK tmz_part_fm_check_reader ( const st_MZCMT_FM_PARAMS *p, const st_TMZ_PART_COMMON *c,
                                                          uint32_t rate, st_TMZ_READER_MARGINS *m );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_PART_FM_H */
