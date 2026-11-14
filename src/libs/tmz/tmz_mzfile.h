/**
 * @file   tmz_mzfile.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Blok 0x46 MZ File (TMZ v1.1): čtení, validace a zápis.
 *
 * Blok nese jeden MZF soubor tak, jak ho program uložil, a seznam částí,
 * které tvoří jeho záznam na pásce (tmz-v1.1-specification.md kap. 4).
 * Každá část má roli, rodinu modulace, zdroj dat a oblast TLV s parametry.
 *
 * Parser na rozdíl od starších tmz_block_parse_*() data bloku nemění:
 * vrací pohledy (ukazatele) do st_TZX_BLOCK.data a alokuje jen pole částí.
 * Zápis skládá blok z téže struktury, takže parse + create dá bajtově
 * shodný blok.
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


#ifndef TMZ_MZFILE_H
#define TMZ_MZFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../tzx/tzx.h"


/** @brief Podporovaná verze bloku 0x46 (spec 4.1). */
#define TMZ_MZFILE_VERSION         1
/** @brief Největší délka ID profilu ve znacích (spec 8.1). */
#define TMZ_MZFILE_PROFILE_ID_MAX  32
/** @brief Velikost MZF hlavičky v bloku. */
#define TMZ_MZFILE_HEADER_SIZE     128


    /**
     * @brief Role části (spec 4.3.1). Player ji nepoužívá; slouží nástrojům.
     */
    typedef enum en_TMZ_PART_ROLE {
        TMZ_PART_ROLE_OTHER = 0,          /**< jiná nebo neznámá část */
        TMZ_PART_ROLE_HEADER = 1,         /**< MZF hlavička programu */
        TMZ_PART_ROLE_BODY = 2,           /**< tělo programu nebo jeho úsek */
        TMZ_PART_ROLE_LOADER_HEADER = 3,  /**< hlavička předzavaděče */
        TMZ_PART_ROLE_LOADER = 4,         /**< předzavaděč nebo zavaděč */
        TMZ_PART_ROLE_BSD_CHUNK = 5,      /**< chunk souboru BASIC (BSD/BRD) */
    } en_TMZ_PART_ROLE;


    /**
     * @brief Rodina modulace části (spec 4.3.2).
     */
    typedef enum en_TMZ_FAMILY {
        TMZ_FAMILY_NONE = 0,         /**< neplatná */
        TMZ_FAMILY_FM = 1,           /**< FM: bit délkou pulzu (kap. 6) */
        TMZ_FAMILY_FSK = 2,          /**< rezervovaná */
        TMZ_FAMILY_SLOW = 3,         /**< rezervovaná */
        TMZ_FAMILY_DIRECT = 4,       /**< rezervovaná */
        TMZ_FAMILY_MANCHESTER = 5,   /**< rezervovaná */
        TMZ_FAMILY_PILOT = 6,        /**< rezervovaná */
    } en_TMZ_FAMILY;


    /**
     * @brief Zdroj bajtů části (spec 4.3.3).
     */
    typedef enum en_TMZ_DATA_SOURCE {
        TMZ_SOURCE_MZF_HEADER = 0,   /**< 128 B MZF hlavičky bloku */
        TMZ_SOURCE_MZF_BODY = 1,     /**< celé tělo bloku */
        TMZ_SOURCE_BODY_SLICE = 2,   /**< výřez těla [offset, offset+délka) */
        TMZ_SOURCE_INLINE = 3,       /**< bajty uložené v části */
    } en_TMZ_DATA_SOURCE;


    /**
     * @brief Chyba části (spec 4.4). Část s chybou se nepřehraje, blok
     *        a ostatní části zůstávají platné.
     */
    typedef enum en_TMZ_PART_ERROR {
        TMZ_PART_OK = 0,                    /**< bez chyby */
        TMZ_PART_ERROR_TLV_STRUCTURE,       /**< položka TLV přesahuje oblast části */
        TMZ_PART_ERROR_UNKNOWN_CRITICAL,    /**< neznámý povinný tag */
        TMZ_PART_ERROR_BAD_VALUE,           /**< délka nebo hodnota povinného tagu, opakovaný tag */
        TMZ_PART_ERROR_MISSING_PULSES,      /**< část FM bez PULSES */
        TMZ_PART_ERROR_UNSUPPORTED_FAMILY,  /**< neznámá nebo rezervovaná rodina */
        TMZ_PART_ERROR_BAD_SOURCE,          /**< neznámý zdroj dat, výřez mimo tělo */
    } en_TMZ_PART_ERROR;


    /**
     * @brief Jedna část bloku 0x46.
     *
     * @par Vlastnictví:
     * Po tmz_mzfile_parse() ukazují inline_data a tlv do dat bloku a žijí
     * s ním. Při zápisu (tmz_mzfile_create_block) je vlastní volající.
     *
     * @invariant source == TMZ_SOURCE_BODY_SLICE: slice_* platné, jinak 0
     * @invariant source == TMZ_SOURCE_INLINE: inline_data ukazuje na inline_length B
     * @invariant tlv == NULL a tlv_length == 0 u neznámého zdroje (pole nelze určit)
     */
    typedef struct st_TMZ_PART {
        uint8_t role;                 /**< en_TMZ_PART_ROLE (neznámá hodnota = OTHER) */
        uint8_t family;               /**< en_TMZ_FAMILY */
        uint8_t source;               /**< en_TMZ_DATA_SOURCE */
        uint32_t slice_offset;        /**< začátek výřezu (jen BODY_SLICE) */
        uint32_t slice_length;        /**< délka výřezu (jen BODY_SLICE) */
        const uint8_t *inline_data;   /**< inline bajty (jen INLINE) */
        uint32_t inline_length;       /**< délka inline bajtů */
        const uint8_t *tlv;           /**< oblast TLV části */
        uint32_t tlv_length;          /**< délka oblasti TLV */
    } st_TMZ_PART;


    /**
     * @brief Obsah bloku 0x46 (popis pro čtení i zápis).
     *
     * @par Vlastnictví:
     * Po tmz_mzfile_parse() ukazují header, body a block_tlv do dat bloku
     * (žijí s blokem) a parts je alokováno parserem (uvolní tmz_mzfile_free).
     * Pro tmz_mzfile_create_block() vlastní všechna pole volající.
     *
     * @invariant profile_id je ukončeno nulou, nejvýš 32 znaků A-Z 0-9 . -
     * @invariant header ukazuje na 128 B, body na body_size B
     * @invariant part_count >= 1 a parts má part_count prvků
     */
    typedef struct st_TMZ_MZFILE {
        char profile_id[TMZ_MZFILE_PROFILE_ID_MAX + 1];  /**< ID profilu ("" = bez ID) */
        const uint8_t *header;        /**< MZF hlavička (128 B) */
        const uint8_t *body;          /**< tělo */
        uint32_t body_size;           /**< délka těla */
        const uint8_t *block_tlv;     /**< blokové TLV */
        uint16_t block_tlv_length;    /**< délka blokových TLV */
        uint8_t part_count;           /**< počet částí */
        st_TMZ_PART *parts;           /**< části */
    } st_TMZ_MZFILE;


    /**
     * @brief Naparsuje a zvaliduje blok 0x46.
     *
     * Kontroluje strukturu podle spec 4.1 a 4.4: vnitřní DWORD délky
     * odpovídá délce bloku, verze, délka a znaky ID profilu, všechna pole
     * uvnitř bloku, alespoň jedna část, délky částí, inline data uvnitř
     * části, oblasti TLV tvořené celými položkami. Význam TLV nekontroluje
     * (to dělá modul rodiny, např. tmz_part_fm). Část s neznámým zdrojem
     * dat se přijme bez TLV; chyba se ohlásí až při jejím použití.
     *
     * @param block Blok s ID 0x46 (data včetně 4B délky).
     * @param[out] out Výsledek; při chybě vynulován.
     * @return TZX_OK; TZX_ERROR_NULL_INPUT při NULL parametru;
     *         TZX_ERROR_UNSUPPORTED při verzi bloku jiné než 1;
     *         TZX_ERROR_ALLOC při selhání alokace; jinak TZX_ERROR_INVALID_BLOCK.
     *
     * @post Při TZX_OK volající uvolní out přes tmz_mzfile_free() a nesmí
     *       uvolnit blok, dokud out používá.
     * @note Data bloku se nemění.
     */
    extern en_TZX_ERROR tmz_mzfile_parse ( const st_TZX_BLOCK *block, st_TMZ_MZFILE *out );

    /**
     * @brief Uvolní pole částí alokované tmz_mzfile_parse().
     * @param mf Struktura (NULL se ignoruje).
     * @post mf->parts == NULL, mf->part_count == 0; ostatní pole beze změny.
     */
    extern void tmz_mzfile_free ( st_TMZ_MZFILE *mf );

    /**
     * @brief Vytvoří blok 0x46 z popisu.
     *
     * @param mf Popis bloku (pole viz st_TMZ_MZFILE; TLV se zapíší tak, jak jsou,
     *        jen se ověří, že tvoří celé položky).
     * @param[out] err Chybový kód (smí být NULL): TZX_ERROR_NULL_INPUT,
     *        TZX_ERROR_INVALID_BLOCK (neplatné ID profilu, neznámý zdroj dat,
     *        TLV části nebo bloku, které nekončí na hranici položky, blok delší
     *        než 4 GB), TZX_ERROR_ALLOC.
     * @return Nový blok, nebo NULL při chybě.
     *
     * @pre Pole a délky v mf si odpovídají (ukazatel != NULL u nenulové délky).
     * @post Volající vlastní blok a uvolní ho tmz_block_free(). TLV bloku
     *       i částí tvoří celé položky, jak vyžaduje tmz_mzfile_parse().
     */
    extern st_TZX_BLOCK* tmz_mzfile_create_block ( const st_TMZ_MZFILE *mf, en_TZX_ERROR *err );

    /**
     * @brief Vrátí bajty části podle jejího zdroje dat.
     *
     * @param mf Blok (po parse nebo popis pro zápis).
     * @param part Část bloku.
     * @param[out] size Počet bajtů (smí být NULL); při chybě 0.
     * @param[out] err TMZ_PART_OK, nebo TMZ_PART_ERROR_BAD_SOURCE při
     *        neznámém zdroji a výřezu mimo tělo (smí být NULL).
     * @return Ukazatel na bajty (do bloku), nebo NULL při chybě. U prázdných
     *         dat může být i NULL s TMZ_PART_OK a size 0.
     *
     * @pre mf a part nesmí být NULL; part patří k mf (zdroje MZF_HEADER,
     *      MZF_BODY a BODY_SLICE čtou mf->header a mf->body).
     * @post Vrácená data patří bloku (mf->header, mf->body nebo
     *       part->inline_data) a platí, dokud žije blok; nic se nealokuje.
     */
    extern const uint8_t* tmz_mzfile_part_data ( const st_TMZ_MZFILE *mf, const st_TMZ_PART *part,
                                                 uint32_t *size, en_TMZ_PART_ERROR *err );

    /**
     * @brief Název role části (anglicky, pro výpisy nástrojů).
     * @param role Hodnota en_TMZ_PART_ROLE; neznámá hodnota se vykládá jako OTHER.
     * @return Statický řetězec ("other", "header", "body", "loader header", "loader", "BSD chunk").
     */
    extern const char* tmz_part_role_name ( uint8_t role );

    /**
     * @brief Název rodiny modulace (anglicky, pro výpisy nástrojů).
     * @param family Hodnota en_TMZ_FAMILY.
     * @return Statický řetězec ("FM", "FSK", "SLOW", "DIRECT", "Manchester", "PILOT"), jinak "unknown".
     */
    extern const char* tmz_family_name ( uint8_t family );

    /**
     * @brief Text chyby části (anglicky, pro výpisy nástrojů).
     * @param err Kód chyby.
     * @return Statický řetězec.
     */
    extern const char* tmz_part_error_string ( en_TMZ_PART_ERROR err );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_MZFILE_H */
