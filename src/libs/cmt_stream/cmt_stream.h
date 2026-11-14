/**
 * @file   cmt_stream.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.0
 * @brief  Sjednocující wrapper nad dvěma typy CMT streamů (bitstream a vstream).
 *
 * Poskytuje polymorfní rozhraní — volající nemusí znát konkrétní typ
 * streamu a může používat společné funkce (get_size, get_rate, destroy...).
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


#ifndef CMT_STREAM_H
#define CMT_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libs/generic_driver/generic_driver.h"

#include "cmt_stream_all.h"

    /** @brief Výchozí vzorkovací frekvence pro WAV export (Hz) */
#define CMTSTREAM_DEFAULT_RATE 44100


    /** @brief Typ streamu */
    typedef enum en_CMT_STREAM_TYPE {
        CMT_STREAM_TYPE_BITSTREAM = 0,  /**< bitstream — 1 bit = 1 vzorek */
        CMT_STREAM_TYPE_VSTREAM,        /**< vstream — pulzně-délkové kódování */
    } en_CMT_STREAM_TYPE;


#include "cmt_bitstream.h"
#include "cmt_vstream.h"


    /** @brief Union pro uložení ukazatele na konkrétní typ streamu */
    typedef union un_CMT_STREAM {
        st_CMT_BITSTREAM *bitstream;    /**< ukazatel na bitstream */
        st_CMT_VSTREAM *vstream;        /**< ukazatel na vstream */
    } un_CMT_STREAM;


    /** @brief Wrapper nad bitstream/vstream s informací o typu */
    typedef struct st_CMT_STREAM {
        en_CMT_STREAM_TYPE stream_type; /**< typ vnitřního streamu */
        un_CMT_STREAM str;              /**< ukazatel na vnitřní stream */
    } st_CMT_STREAM;

    /* === Vytváření a rušení === */

    /**
     * @brief Uvolní stream (vnitřní data i wrapper strukturu).
     * @param stream Ukazatel na stream wrapper (NULL je bezpečné)
     */
    extern void cmt_stream_destroy ( st_CMT_STREAM *stream );

    /**
     * @brief Vytvoří nový prázdný wrapper pro daný typ streamu.
     * @param type Typ streamu (BITSTREAM nebo VSTREAM)
     * @return Ukazatel na nový wrapper, nebo NULL při selhání
     */
    extern st_CMT_STREAM* cmt_stream_new ( en_CMT_STREAM_TYPE type );

    /**
     * @brief Vytvoří stream z WAV souboru (přes handler).
     * @param h Handler na WAV data
     * @param polarity Polarita signálu
     * @param type Typ cílového streamu (BITSTREAM nebo VSTREAM)
     * @return Ukazatel na nový stream, nebo NULL při selhání
     */
    extern st_CMT_STREAM* cmt_stream_new_from_wav ( st_HANDLER *h, en_CMT_STREAM_POLARITY polarity, en_CMT_STREAM_TYPE type );

    /* === Gettery === */

    /**
     * @brief Vrátí typ streamu.
     * @param stream Ukazatel na stream wrapper
     * @return Typ streamu (BITSTREAM nebo VSTREAM)
     */
    extern en_CMT_STREAM_TYPE cmt_stream_get_stream_type ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí textový název typu streamu.
     * @param stream Ukazatel na stream wrapper
     * @return Řetězec "bitstream", "vstream" nebo "unknown"
     */
    extern const char* cmt_stream_get_stream_type_txt ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí velikost datové oblasti streamu v bajtech.
     * @param stream Ukazatel na stream wrapper
     * @return Velikost v bajtech
     */
    extern uint32_t cmt_stream_get_size ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí vzorkovací frekvenci streamu.
     * @param stream Ukazatel na stream wrapper
     * @return Vzorkovací frekvence v Hz
     */
    extern uint32_t cmt_stream_get_rate ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí celkovou délku streamu.
     * @param stream Ukazatel na stream wrapper
     * @return Délka v sekundách
     */
    extern double cmt_stream_get_length ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí celkový počet vzorků.
     * @param stream Ukazatel na stream wrapper
     * @return Počet vzorků
     */
    extern uint64_t cmt_stream_get_count_scans ( st_CMT_STREAM *stream );

    /**
     * @brief Vrátí délku jednoho vzorku.
     * @param stream Ukazatel na stream wrapper
     * @return Délka jednoho vzorku v sekundách (1/rate)
     */
    extern double cmt_stream_get_scantime ( st_CMT_STREAM *stream );

    /* === Polarita === */

    /**
     * @brief Nastaví polaritu streamu.
     * @param stream Ukazatel na stream wrapper
     * @param polarity Nová polarita signálu
     */
    extern void cmt_stream_set_polarity ( st_CMT_STREAM *stream, en_CMT_STREAM_POLARITY polarity );

    /* === Export === */

    /**
     * @brief Uloží stream jako WAV soubor.
     * @param stream Ukazatel na stream wrapper
     * @param rate Vzorkovací frekvence výstupního WAV (Hz)
     * @param filename Cesta k výstupnímu souboru
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int cmt_stream_save_wav ( st_CMT_STREAM *stream, uint32_t rate, char *filename );

    /** @brief Verze knihovny cmt_stream. */
#define CMT_STREAM_VERSION "2.1.0"

    /**
     * @brief Vrátí řetězec s verzí knihovny cmt_stream.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* cmt_stream_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* CMT_STREAM_H */
