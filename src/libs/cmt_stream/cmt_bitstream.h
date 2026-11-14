/**
 * @file   cmt_bitstream.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.1
 * @brief  Bitstream — kondenzovaný WAV formát, kde 1 bit = 1 vzorek.
 *
 * Data jsou uložena jako pole uint32_t bloků (po 32 bitech).
 * Podporuje přímý přístup k libovolné pozici v O(1) — vhodný
 * pro přehrávání s náhodným přístupem k pozici v čase.
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


#ifndef CMT_BITSTREAM_H
#define CMT_BITSTREAM_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <assert.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"
#include "cmt_stream_all.h"

    /** @brief Forward deklarace — pro konverzní funkci cmt_bitstream_new_from_vstream() */
    struct st_CMT_VSTREAM;


    /** @brief Kondenzovaný WAV — 1 bit odpovídá jednomu vzorku */
    typedef struct st_CMT_BITSTREAM {
        uint32_t rate;                      /**< vzorkovací frekvence (Hz) */
        double scan_time;                   /**< délka jednoho vzorku = 1/rate (s) */
        double stream_length;               /**< celková doba streamu (s), aktualizuje se při zápisu */
        uint32_t blocks;                    /**< počet alokovaných 32-bitových bloků */
        uint32_t scans;                     /**< počet obsazených bitů (vzorků) */
        en_CMT_STREAM_POLARITY polarity;    /**< polarita signálu */
        uint32_t *data;                     /**< datové bloky */
    } st_CMT_BITSTREAM;

    /** @brief Počet bitů na jeden datový blok (32) */
#define CMT_BITSTREAM_BLOCK_SIZE ( sizeof ( uint32_t ) * 8 )

    /* === Vytváření a rušení === */

    /**
     * @brief Vytvoří nový prázdný bitstream.
     * @param rate Vzorkovací frekvence (Hz)
     * @param blocks Počet 32-bitových bloků (0 = bez datové oblasti)
     * @param polarity Polarita signálu
     * @return Ukazatel na nový bitstream, nebo NULL při selhání alokace
     */
    extern st_CMT_BITSTREAM* cmt_bitstream_new ( uint32_t rate, uint32_t blocks, en_CMT_STREAM_POLARITY polarity );

    /**
     * @brief Vytvoří bitstream z WAV souboru (přes handler).
     * @param wav_handler Handler na WAV data
     * @param polarity Polarita signálu
     * @return Ukazatel na nový bitstream, nebo NULL při selhání
     */
    extern st_CMT_BITSTREAM* cmt_bitstream_new_from_wav ( st_HANDLER *wav_handler, en_CMT_STREAM_POLARITY polarity );

    /**
     * @brief Vytvoří bitstream převzorkováním dat z vstreamu.
     * @param vstream Zdrojový vstream
     * @param dst_rate Cílová vzorkovací frekvence (0 = použít frekvenci vstreamu)
     * @return Ukazatel na nový bitstream, nebo NULL při selhání
     */
    extern st_CMT_BITSTREAM* cmt_bitstream_new_from_vstream ( struct st_CMT_VSTREAM *vstream, uint32_t dst_rate );

    /**
     * @brief Uvolní bitstream a jeho data. Bezpečné volání s NULL.
     * @param stream Ukazatel na bitstream (NULL je bezpečné)
     */
    extern void cmt_bitstream_destroy ( st_CMT_BITSTREAM *stream );

    /* === Výpočty velikosti === */

    /**
     * @brief Spočítá minimální počet 32-bitových bloků pro daný počet vzorků.
     * @param scans Počet vzorků (uint64_t pro bezpečnou práci s velkými streamy)
     * @return Potřebný počet bloků (UINT32_MAX při přetečení)
     */
    extern uint32_t cmt_bitstream_compute_required_blocks_from_scans ( uint64_t scans );

    /* === Export do WAV === */

    /**
     * @brief Vytvoří WAV data z bitstreamu a zapíše je přes handler.
     * @param wav_handler Handler pro zápis WAV dat
     * @param stream Zdrojový bitstream
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int cmt_bitstream_create_wav ( st_HANDLER *wav_handler, st_CMT_BITSTREAM *stream );

    /* === Polarita === */

    /**
     * @brief Změní polaritu bitstreamu — invertuje všechny bity.
     * @param stream Ukazatel na bitstream
     * @param polarity Požadovaná polarita (pokud shodná s aktuální, nedělá nic)
     */
    extern void cmt_bitstream_change_polarity ( st_CMT_BITSTREAM *stream, en_CMT_STREAM_POLARITY polarity );

    /* === Gettery === */

    /**
     * @brief Vrátí velikost datové oblasti v bajtech.
     * @param bitstream Ukazatel na bitstream
     * @return Velikost v bajtech
     */
    extern uint32_t cmt_bitstream_get_size ( st_CMT_BITSTREAM *bitstream );

    /**
     * @brief Vrátí vzorkovací frekvenci.
     * @param bitstream Ukazatel na bitstream
     * @return Vzorkovací frekvence v Hz
     */
    extern uint32_t cmt_bitstream_get_rate ( st_CMT_BITSTREAM *bitstream );

    /**
     * @brief Vrátí celkovou délku streamu.
     * @param bitstream Ukazatel na bitstream
     * @return Délka v sekundách
     */
    extern double cmt_bitstream_get_length ( st_CMT_BITSTREAM *bitstream );

    /**
     * @brief Vrátí celkový počet vzorků.
     * @param bitstream Ukazatel na bitstream
     * @return Počet vzorků
     */
    extern uint64_t cmt_bitstream_get_count_scans ( st_CMT_BITSTREAM *bitstream );

    /**
     * @brief Vrátí délku jednoho vzorku.
     * @param bitstream Ukazatel na bitstream
     * @return Délka jednoho vzorku v sekundách (1/rate)
     */
    extern double cmt_bitstream_get_scantime ( st_CMT_BITSTREAM *bitstream );


    /**
     * @brief Převede čas (v sekundách) na pozici ve streamu (index bitu).
     * @param stream Ukazatel na bitstream
     * @param scan_time Čas v sekundách
     * @return Index bitu odpovídající danému času
     */
    static inline uint32_t cmt_bitstream_get_position_by_time ( st_CMT_BITSTREAM *stream, double scan_time ) {
        uint32_t position = scan_time / stream->scan_time;
        return position;
    }


    /**
     * @brief Vrátí hodnotu bitu (0|1) na dané pozici.
     * @param stream Ukazatel na bitstream
     * @param position Index bitu (musí být < scans)
     * @return Hodnota bitu (0 nebo 1)
     */
    static inline int cmt_bitstream_get_value_on_position ( st_CMT_BITSTREAM *stream, uint32_t position ) {
        assert ( position < stream->scans );
        assert ( stream->data != NULL );
        uint32_t block = position / CMT_BITSTREAM_BLOCK_SIZE;
        assert ( block < stream->blocks );
        return ( stream->data[block] >> ( position % CMT_BITSTREAM_BLOCK_SIZE ) ) & 1;
    }


    /**
     * @brief Vrátí hodnotu bitu (0|1) na dané časové pozici.
     * @param stream Ukazatel na bitstream
     * @param scan_time Čas v sekundách
     * @return Hodnota bitu (0 nebo 1)
     */
    static inline int cmt_bitstream_get_value_on_time ( st_CMT_BITSTREAM *stream, double scan_time ) {
        uint32_t position = cmt_bitstream_get_position_by_time ( stream, scan_time );
        return cmt_bitstream_get_value_on_position ( stream, position );
    }


    /**
     * @brief Nastaví bit na dané pozici na hodnotu value (0|1).
     *
     * Pokud je pozice za koncem dosavadních dat, rozšíří scans a stream_length.
     *
     * @param stream Ukazatel na bitstream
     * @param position Index bitu (blok musí být < blocks)
     * @param value Hodnota bitu (0 nebo 1)
     */
    static inline void cmt_bitstream_set_value_on_position ( st_CMT_BITSTREAM *stream, uint32_t position, int value ) {
        uint32_t block = position / CMT_BITSTREAM_BLOCK_SIZE;
        assert ( block < stream->blocks );
        assert ( stream->data != NULL );

        if ( value ) {
            stream->data[block] |= (uint32_t) 1 << ( position % CMT_BITSTREAM_BLOCK_SIZE );
        } else {
            stream->data[block] &= ~( (uint32_t) 1 << ( position % CMT_BITSTREAM_BLOCK_SIZE ) );
        }

        if ( !( position < stream->scans ) ) {
            stream->scans = position + 1;
            stream->stream_length = stream->scan_time * stream->scans;
        }
    }


    /**
     * @brief Nastaví bit na dané časové pozici na hodnotu value (0|1).
     * @param stream Ukazatel na bitstream
     * @param scan_time Čas v sekundách
     * @param value Hodnota bitu (0 nebo 1)
     */
    static inline void cmt_bitstream_set_value_on_time ( st_CMT_BITSTREAM *stream, double scan_time, int value ) {
        uint32_t position = cmt_bitstream_get_position_by_time ( stream, scan_time );
        cmt_bitstream_set_value_on_position ( stream, position, value );
    }


#ifdef __cplusplus
}
#endif

#endif /* CMT_BITSTREAM_H */
