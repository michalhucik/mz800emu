/**
 * @file   zxtape.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.1
 * @brief  Veřejné API knihovny zxtape pro konverzi ZX Spectrum TAP bloků na CMT streamy.
 *
 * Poskytuje typy, enumy, alokátor, error callback a funkce pro generování
 * CMT audio streamů (bitstream/vstream) z TAP bloků ZX Spectra.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
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


#ifndef ZXTAPE_H
#define ZXTAPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"


    /** @brief Výchozí baudová rychlost TAP formátu (1400 Bd). */
#define ZXTAPE_DEFAULT_BDSPEED 1400

    /** @brief Počet pilot pulzů pro header blok. */
#define ZXTAPE_HEADER_PILOT_LENGTH 4032

    /** @brief Počet pilot pulzů pro data blok. */
#define ZXTAPE_DATA_PILOT_LENGTH 1610

    /** @brief Pole podporovaných rychlostí, zakončené CMTSPEED_NONE. */
    extern const en_CMTSPEED g_zxtape_speed[];


    /**
     * @brief Typ bloku v TAP souboru.
     *
     * Určuje, zda jde o header (0x00) nebo datový blok (0xFF).
     * Ovlivňuje počet pilot pulzů při generování audio streamu.
     */
    typedef enum en_ZXTAPE_BLOCK_FLAG {
        ZXTAPE_BLOCK_FLAG_HEADER = 0x00, /**< Header blok (4032 pilot pulzů) */
        ZXTAPE_BLOCK_FLAG_DATA = 0xff,   /**< Datový blok (1610 pilot pulzů) */
    } en_ZXTAPE_BLOCK_FLAG;


    /**
     * @brief Alokátor — umožňuje nahradit výchozí malloc/calloc/free.
     *
     * Pokud není nastaven vlastní alokátor, používají se standardní
     * systémové funkce malloc, calloc a free.
     */
    typedef struct st_ZXTAPE_ALLOCATOR {
        void* (*alloc)(size_t size);  /**< Alokace paměti (jako malloc) */
        void* (*alloc0)(size_t size); /**< Alokace s nulováním (jako calloc) */
        void  (*free)(void *ptr);     /**< Uvolnění paměti (jako free) */
    } st_ZXTAPE_ALLOCATOR;

    /**
     * @brief Nastaví vlastní alokátor pro knihovnu zxtape.
     * @param allocator Ukazatel na strukturu alokátoru, nebo NULL pro reset na výchozí.
     */
    extern void zxtape_set_allocator ( const st_ZXTAPE_ALLOCATOR *allocator );


    /**
     * @brief Typ error callbacku pro chybové hlášení.
     *
     * Formát odpovídá fprintf(stderr, "%s():%d - ...", func, line, ...).
     */
    typedef void (*zxtape_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastaví vlastní error callback pro knihovnu zxtape.
     * @param cb Ukazatel na callback funkci, nebo NULL pro reset na výchozí (fprintf stderr).
     */
    extern void zxtape_set_error_callback ( zxtape_error_cb cb );


    /**
     * @brief Vytvoří CMT bitstream přímo z TAP bloku.
     *
     * Přímá generace bitstreamu — alternativní path, který akumuluje
     * zaokrouhlovací chybu při vyšších rychlostech.
     *
     * @param flag Typ bloku (header/data) — určuje počet pilot pulzů.
     * @param data Ukazatel na data TAP bloku (flag + payload + checksum).
     * @param data_size Velikost dat v bajtech.
     * @param cmtspeed Rychlostní poměr CMT pásky.
     * @param rate Vzorkovací frekvence v Hz (např. 44100).
     * @return Nově alokovaný bitstream, nebo NULL při chybě.
     */
    extern st_CMT_BITSTREAM* zxtape_create_cmt_bitstream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate );

    /**
     * @brief Vytvoří CMT vstream z TAP bloku.
     *
     * Doporučený path — přesnější než přímý bitstream díky nezávislému
     * zaokrouhlování každého pulzu (RLE kódování).
     *
     * @param flag Typ bloku (header/data) — určuje počet pilot pulzů.
     * @param data Ukazatel na data TAP bloku (flag + payload + checksum).
     * @param data_size Velikost dat v bajtech.
     * @param cmtspeed Rychlostní poměr CMT pásky.
     * @param rate Vzorkovací frekvence v Hz (např. 44100).
     * @return Nově alokovaný vstream, nebo NULL při chybě.
     */
    extern st_CMT_VSTREAM* zxtape_create_cmt_vstream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate );

    /**
     * @brief Vytvoří CMT stream zvoleného typu z TAP bloku (unified wrapper).
     *
     * Volí vstream nebo bitstream podle parametru type. Pro bitstream
     * interně používá vstream -> bitstream konverzi (přesnější).
     *
     * @param flag Typ bloku (header/data) — určuje počet pilot pulzů.
     * @param data Ukazatel na data TAP bloku (flag + payload + checksum).
     * @param data_size Velikost dat v bajtech.
     * @param cmtspeed Rychlostní poměr CMT pásky.
     * @param rate Vzorkovací frekvence v Hz (např. 44100).
     * @param type Typ výstupního streamu (bitstream/vstream).
     * @return Nově alokovaný stream, nebo NULL při chybě.
     */
    extern st_CMT_STREAM* zxtape_create_stream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate, en_CMT_STREAM_TYPE type );


    /**
     * @brief Spočítá celkový počet long a short pulzů pro daný TAP blok.
     *
     * Užitečné pro odhad délky streamu bez jeho generování.
     * Zahrnuje pilot pulzy, sync, datové bity a koncovou pauzu.
     *
     * @param flag Typ bloku (header/data) — určuje počet pilot pulzů.
     * @param data Ukazatel na data TAP bloku.
     * @param data_size Velikost dat v bajtech.
     * @param[out] long_pulses Výstupní ukazatel pro počet long pulzů.
     * @param[out] short_pulses Výstupní ukazatel pro počet short pulzů.
     */
    extern void zxtape_compute_pulses ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, uint64_t *long_pulses, uint64_t *short_pulses );


    /**
     * @brief Validuje XOR checksum TAP bloku.
     *
     * Data musí obsahovat celý blok (flag + payload + checksum).
     * Validace: XOR všech bajtů (včetně checksumu) musí být 0.
     *
     * @param data Ukazatel na data bloku.
     * @param data_size Celková velikost dat v bajtech (včetně checksumu).
     * @return 0 = checksum OK, -1 = chyba (neplatná data nebo checksum nesedí).
     */
    extern int zxtape_validate_checksum ( uint8_t *data, uint16_t data_size );


    /** @brief Verze knihovny zxtape. */
#define ZXTAPE_VERSION "2.0.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny zxtape.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* zxtape_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* ZXTAPE_H */
