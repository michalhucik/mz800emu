/**
 * @file   tmz_tlv.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Kódování TLV (tag, délka, hodnota) pro blok 0x46 TMZ v1.1.
 *
 * Položka TLV má tvar WORD tag, WORD délka, hodnota; čísla jsou LE.
 * Bit 15 tagu znamená "povinné k pochopení" (tmz-v1.1-specification.md
 * kap. 5.1). Modul nemá závislosti mimo standardní knihovnu: čte oblast TLV
 * bez kopírování (iterátor) a skládá novou oblast do rostoucího bufferu.
 * Význam jednotlivých tagů řeší moduly nad ním (tmz_part_fm).
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


#ifndef TMZ_TLV_H
#define TMZ_TLV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>


/** @brief Bit 15 tagu: položka je povinná k pochopení (neznámá = chyba části). */
#define TMZ_TLV_CRITICAL  0x8000u
/** @brief Maska čísla tagu (bity 0-14). */
#define TMZ_TLV_TAG_MASK  0x7FFFu

/** @defgroup tmz_tlv_tags Čísla tagů (spec kap. 5.2-5.5, 6.1)
 *  Hodnoty bez bitu 15. Povinnost určuje specifikace u každého tagu.
 *  @{ */
/** @brief Společný tag části: časová základna, DWORD Hz (povinný). */
#define TMZ_TAG_TIMEBASE      0x0001u
/** @brief Společný tag části: úroveň první půlvlny, BYTE 0/1 (povinný). */
#define TMZ_TAG_LEVEL         0x0002u
/** @brief Společný tag části: ticho za částí, DWORD v jednotkách časové základny (povinný). */
#define TMZ_TAG_PAUSE         0x0003u
/** @brief Společný tag části: po části zastavit pásku, BYTE (nepovinný). */
#define TMZ_TAG_STOP          0x0004u
/** @brief Společný tag části: kontrola čtecího bodu, BYTE rutina + WORD N (nepovinný). */
#define TMZ_TAG_READER        0x0005u
/** @brief Blokový tag: parametry profilu, ASCII (nepovinný). */
#define TMZ_TAG_PROFILE_ARGS  0x0040u
/** @brief FM: 4x DWORD délky půlvln SHORT/LONG (povinný). */
#define TMZ_TAG_FM_PULSES     0x0101u
/** @brief FM: DWORD počet SHORT v tónu (povinný). */
#define TMZ_TAG_FM_TONE       0x0102u
/** @brief FM: WORD L, WORD S, BYTE L za tapemarkem (povinný). */
#define TMZ_TAG_FM_TAPEMARK   0x0103u
/** @brief FM: BYTE start bity, BYTE pořadí bitů (povinný). */
#define TMZ_TAG_FM_FRAMING    0x0104u
/** @brief FM: BYTE algoritmus checksumu, volitelně 2 B explicitní hodnoty (povinný). */
#define TMZ_TAG_FM_CHECKSUM   0x0105u
/** @brief FM: BYTE L za checksumem, WORD S na konci (povinný). */
#define TMZ_TAG_FM_END        0x0106u
/** @brief FM: BYTE počet kopií, WORD S oddělovače (povinný). */
#define TMZ_TAG_FM_COPIES     0x0107u
/** @brief FM: n x 11 B kontextových délek, smí se opakovat (povinný). */
#define TMZ_TAG_FM_CONTEXT    0x0108u
/** @brief MANCHESTER: 4x DWORD první půlka bitu 0/1, druhá půlka bitu 0/1 (povinný). */
#define TMZ_TAG_MANCH_HALVES   0x0501u
/** @brief MANCHESTER: DWORD počet vzorů, BYTE délka vzoru, BYTE vzor (povinný). */
#define TMZ_TAG_MANCH_PILOT    0x0502u
/** @brief MANCHESTER: BYTE počet bitů sync, BYTE bity (povinný). */
#define TMZ_TAG_MANCH_SYNC     0x0503u
/** @brief MANCHESTER: BYTE hodnota flagu; přítomnost = flag se vysílá (povinný). */
#define TMZ_TAG_MANCH_FLAG     0x0504u
/** @brief MANCHESTER: BYTE algoritmus checksumu, volitelně 2 B explicitní hodnoty (povinný). */
#define TMZ_TAG_MANCH_CHECKSUM 0x0505u
/** @brief MANCHESTER: BYTE počet koncových bitů, BYTE bity (povinný). */
#define TMZ_TAG_MANCH_END      0x0506u
/** @brief MANCHESTER: BYTE se znaménkem, úprava při přenosu součtu (povinný). */
#define TMZ_TAG_MANCH_CARRY    0x0507u
/** @brief MANCHESTER: n x 6 B kontextových druhých půlek, smí se opakovat (povinný). */
#define TMZ_TAG_MANCH_CONTEXT  0x0508u
/** @} */


    /**
     * @brief Jedna položka TLV přečtená iterátorem.
     *
     * @par Vlastnictví:
     * value ukazuje do oblasti, nad kterou iterátor běží, a žije s ní.
     *
     * @invariant tag <= TMZ_TLV_TAG_MASK
     * @invariant value ukazuje na length platných bajtů
     */
    typedef struct st_TMZ_TLV {
        uint16_t tag;          /**< číslo tagu bez bitu 15 */
        int critical;          /**< 1 = bit 15 nastaven (povinná položka) */
        uint16_t length;       /**< délka hodnoty v bajtech */
        const uint8_t *value;  /**< hodnota (ukazuje do oblasti TLV) */
    } st_TMZ_TLV;


    /**
     * @brief Iterátor nad oblastí TLV (bez kopírování, nemění data).
     *
     * @invariant pos <= length
     */
    typedef struct st_TMZ_TLV_ITER {
        const uint8_t *area;   /**< začátek oblasti (může být NULL při délce 0) */
        uint32_t length;       /**< délka oblasti */
        uint32_t pos;          /**< pozice další položky */
    } st_TMZ_TLV_ITER;


    /**
     * @brief Rostoucí buffer pro skládání oblasti TLV.
     *
     * @par Vlastnictví:
     * data vlastní buffer; uvolní tmz_tlvbuf_free().
     *
     * @invariant length <= capacity
     * @invariant data == NULL právě když capacity == 0
     */
    typedef struct st_TMZ_TLVBUF {
        uint8_t *data;         /**< obsah (vlastněný) */
        uint32_t length;       /**< zapsané bajty */
        uint32_t capacity;     /**< alokovaná velikost */
    } st_TMZ_TLVBUF;


    /**
     * @brief Přečte WORD LE.
     * @param p Ukazatel na 2 bajty.
     * @return Hodnota.
     * @pre p ukazuje na aspoň 2 čitelné bajty (meze nekontroluje).
     */
    static inline uint16_t tmz_rd16 ( const uint8_t *p ) {
        return ( uint16_t ) ( p[0] | ( p[1] << 8 ) );
    }

    /**
     * @brief Přečte DWORD LE.
     * @param p Ukazatel na 4 bajty.
     * @return Hodnota.
     * @pre p ukazuje na aspoň 4 čitelné bajty (meze nekontroluje).
     */
    static inline uint32_t tmz_rd32 ( const uint8_t *p ) {
        return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8 ) | ( ( uint32_t ) p[2] << 16 ) | ( ( uint32_t ) p[3] << 24 );
    }

    /**
     * @brief Zapíše WORD LE.
     * @param p Cíl (2 bajty).
     * @param v Hodnota.
     * @pre p ukazuje na aspoň 2 zapisovatelné bajty (meze nekontroluje).
     */
    static inline void tmz_wr16 ( uint8_t *p, uint16_t v ) {
        p[0] = ( uint8_t ) v;
        p[1] = ( uint8_t ) ( v >> 8 );
    }

    /**
     * @brief Zapíše DWORD LE.
     * @param p Cíl (4 bajty).
     * @param v Hodnota.
     * @pre p ukazuje na aspoň 4 zapisovatelné bajty (meze nekontroluje).
     */
    static inline void tmz_wr32 ( uint8_t *p, uint32_t v ) {
        p[0] = ( uint8_t ) v;
        p[1] = ( uint8_t ) ( v >> 8 );
        p[2] = ( uint8_t ) ( v >> 16 );
        p[3] = ( uint8_t ) ( v >> 24 );
    }


    /**
     * @brief Připraví iterátor nad oblastí TLV.
     *
     * @param it Iterátor (nesmí být NULL).
     * @param area Začátek oblasti; NULL znamená prázdnou oblast.
     * @param length Délka oblasti v bajtech.
     *
     * @pre it != NULL; area (není-li NULL) má aspoň length čitelných bajtů
     *      a žije, dokud se iterátor používá.
     * @post První volání tmz_tlv_iter_next() vrátí první položku.
     */
    extern void tmz_tlv_iter_init ( st_TMZ_TLV_ITER *it, const uint8_t *area, uint32_t length );

    /**
     * @brief Přečte další položku TLV.
     *
     * @param it Iterátor připravený tmz_tlv_iter_init().
     * @param[out] out Položka; platná jen při návratu 1.
     * @return 1 = položka přečtena, 0 = konec oblasti, -1 = položka přesahuje
     *         oblast (hlavička nebo hodnota). Po -1 se iterátor neposune.
     *
     * @pre it připravil tmz_tlv_iter_init(), out != NULL.
     * @post Při návratu 1 ukazuje out->value do oblasti (platí, dokud žije
     *       oblast) a iterátor stojí za položkou.
     */
    extern int tmz_tlv_iter_next ( st_TMZ_TLV_ITER *it, st_TMZ_TLV *out );

    /**
     * @brief Ověří, že oblast tvoří celé položky TLV bez přesahu.
     *
     * @param area Začátek oblasti (NULL = prázdná).
     * @param length Délka oblasti.
     * @return EXIT_SUCCESS když všechny položky leží uvnitř a končí přesně na
     *         konci oblasti, jinak EXIT_FAILURE. Význam tagů se nekontroluje.
     */
    extern int tmz_tlv_area_check ( const uint8_t *area, uint32_t length );

    /**
     * @brief Inicializuje prázdný buffer.
     * @param buf Buffer (nesmí být NULL).
     * @post buf->data == NULL, length == capacity == 0.
     */
    extern void tmz_tlvbuf_init ( st_TMZ_TLVBUF *buf );

    /**
     * @brief Uvolní obsah bufferu a vrátí ho do prázdného stavu.
     * @param buf Buffer (NULL se ignoruje).
     * @pre buf je inicializovaný (tmz_tlvbuf_init) nebo NULL.
     * @post buf->data == NULL, length == capacity == 0; buffer lze znovu plnit.
     */
    extern void tmz_tlvbuf_free ( st_TMZ_TLVBUF *buf );

    /**
     * @brief Připojí položku TLV na konec bufferu.
     *
     * @param buf Buffer.
     * @param tag Číslo tagu bez bitu 15 (tag s bitem 15 je chyba).
     * @param critical Nenulové = nastavit bit 15 (povinná položka).
     * @param value Hodnota (smí být NULL jen při length == 0).
     * @param length Délka hodnoty.
     * @return EXIT_SUCCESS, nebo EXIT_FAILURE při chybném parametru, délce
     *         bufferu, která by přesáhla UINT32_MAX, nebo selhání alokace.
     *         Při chybě se buffer nemění.
     * @pre buf je inicializovaný (tmz_tlvbuf_init) nebo obsahuje dřívější položky.
     * @post Při úspěchu buf->length vzrostl o 4 + length a položka je na konci.
     */
    extern int tmz_tlvbuf_add ( st_TMZ_TLVBUF *buf, uint16_t tag, int critical, const void *value, uint16_t length );

#ifdef __cplusplus
}
#endif

#endif /* TMZ_TLV_H */
