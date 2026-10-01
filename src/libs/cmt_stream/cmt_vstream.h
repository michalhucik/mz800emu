/**
 * @file   cmt_vstream.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Variable-length stream (vstream) — pulzně-délkové kódování CMT signálu.
 *
 * Ukládá délku setrvání signálu v jednom stavu pomocí variabilně-šířkového
 * kódování: uint8_t (0-254), uint16_t (255-65534), uint32_t (65535+).
 * Paměťově efektivní pro dlouhé úseky konstantního signálu.
 *
 * Formát dat:
 *   uint8_t  - počet vzorků po které byl signál ve stálém stavu
 *            - pokud byl počet vzorků větší než 0xFE, uint8_t = 0xFF
 *              a následuje uint16_t
 *            - pokud byl počet vzorků větší než 0xFFFE, uint16_t = 0xFFFF
 *              a následuje uint32_t
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef CMT_VSTREAM_H
#define CMT_VSTREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/wav/wav.h"
#include "cmt_stream_all.h"


    /** @brief Minimální délka jednoho eventu v bajtech */
    typedef enum en_CMT_VSTREAM_BYTELENGTH {
        CMT_VSTREAM_BYTELENGTH8 = sizeof (uint8_t ),    /**< uint8_t minimum (1 B) */
        CMT_VSTREAM_BYTELENGTH16 = sizeof (uint16_t ),  /**< uint16_t minimum (2 B) */
        CMT_VSTREAM_BYTELENGTH32 = sizeof (uint32_t ),  /**< uint32_t minimum (4 B) */
    } en_CMT_VSTREAM_BYTELENGTH;


    /** @brief Variable-length stream — pulzně-délkové kódování */
    typedef struct st_CMT_VSTREAM {
        uint32_t rate;                          /**< vzorkovací frekvence (Hz) */
        uint32_t msticks;                       /**< počet vzorků v 1 ms */
        double scan_time;                       /**< délka jednoho vzorku = 1/rate (s) */
        double stream_length;                   /**< celková doba streamu (s) */
        en_CMT_VSTREAM_BYTELENGTH min_byte_length; /**< nejmenší velikost jednoho eventu (1|2|4 B) */
        uint32_t size;                          /**< počet bajtů datové oblasti */
        uint32_t capacity;                      /**< alokovaná kapacita datové oblasti */
        uint64_t scans;                         /**< celkový počet vzorků */
        int start_value;                        /**< počáteční hodnota signálu (0|1) */
        int last_set_value;                     /**< poslední zapsaná hodnota (0|1) */
        int last_event_byte_length;             /**< počet bajtů popisujících poslední event (1|2|4) */
        uint32_t last_read_position;            /**< pozice čtení — bajt v datové oblasti */
        uint64_t last_read_position_sample;     /**< pozice čtení — počet vzorků před aktuální pozicí */
        int last_read_value;                    /**< aktuální hodnota při čtení (0|1) */
        en_CMT_STREAM_POLARITY polarity;        /**< polarita signálu */
        uint8_t *data;                          /**< datová oblast */
    } st_CMT_VSTREAM;


    /** @brief Forward deklarace pro konverzní funkci */
    struct st_CMT_BITSTREAM;

    /* === Vytváření a rušení === */

    /**
     * @brief Vytvoří nový prázdný vstream.
     * @param rate Vzorkovací frekvence (Hz), musí být > 0
     * @param min_byte_length Minimální šířka eventu (1, 2 nebo 4)
     * @param value Počáteční hodnota signálu (0 nebo 1)
     * @param polarity Polarita signálu
     * @return Ukazatel na nový vstream, nebo NULL při nevalidních parametrech
     */
    extern st_CMT_VSTREAM* cmt_vstream_new ( uint32_t rate, en_CMT_VSTREAM_BYTELENGTH min_byte_length, int value, en_CMT_STREAM_POLARITY polarity );

    /**
     * @brief Vytvoří vstream z WAV souboru (přes handler).
     * @param h Handler na WAV data
     * @param polarity Polarita signálu
     * @return Ukazatel na nový vstream, nebo NULL při selhání
     */
    extern st_CMT_VSTREAM* cmt_vstream_new_from_wav ( st_HANDLER *h, en_CMT_STREAM_POLARITY polarity );

    /**
     * @brief Vytvoří vstream z bitstreamu (konverze bitstream -> vstream).
     * @param bitstream Zdrojový bitstream (nesmí být prázdný)
     * @param dst_rate Cílová vzorkovací frekvence (0 = použít frekvenci bitstreamu)
     * @return Ukazatel na nový vstream, nebo NULL při selhání
     */
    extern st_CMT_VSTREAM* cmt_vstream_new_from_bitstream ( struct st_CMT_BITSTREAM *bitstream, uint32_t dst_rate );

    /**
     * @brief Uvolní vstream a jeho data. Bezpečné volání s NULL.
     * @param cmt_vstream Ukazatel na vstream (NULL je bezpečné)
     */
    extern void cmt_vstream_destroy ( st_CMT_VSTREAM *cmt_vstream );

    /* === Zápis dat === */

    /**
     * @brief Přidá hodnotu do vstreamu.
     *
     * Pokud je hodnota shodná s poslední zapsanou, prodlouží aktuální event.
     * Jinak vytvoří nový event. Automaticky eskaluje šířku eventu
     * (u8->u16->u32) při přetečení.
     *
     * @param cmt_vstream Ukazatel na vstream
     * @param value Hodnota signálu (0 nebo 1)
     * @param count_samples Počet vzorků (nesmí být 0)
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int cmt_vstream_add_value ( st_CMT_VSTREAM *cmt_vstream, int value, uint32_t count_samples );

    /* === Čtení — řízení pozice === */

    /**
     * @brief Resetuje čtecí pozici na začátek streamu.
     * @param cmt_vstream Ukazatel na vstream
     */
    extern void cmt_vstream_read_reset ( st_CMT_VSTREAM *cmt_vstream );

    /* === Polarita === */

    /**
     * @brief Změní polaritu vstreamu — invertuje start_value, last_set_value a last_read_value.
     * @param stream Ukazatel na vstream
     * @param polarity Požadovaná polarita (pokud shodná s aktuální, nedělá nic)
     */
    extern void cmt_vstream_change_polarity ( st_CMT_VSTREAM *stream, en_CMT_STREAM_POLARITY polarity );

    /* === Gettery === */

    /**
     * @brief Vrátí velikost datové oblasti v bajtech.
     * @param vstream Ukazatel na vstream
     * @return Velikost v bajtech
     */
    extern uint32_t cmt_vstream_get_size ( st_CMT_VSTREAM *vstream );

    /**
     * @brief Vrátí vzorkovací frekvenci.
     * @param vstream Ukazatel na vstream
     * @return Vzorkovací frekvence v Hz
     */
    extern uint32_t cmt_vstream_get_rate ( st_CMT_VSTREAM *vstream );

    /**
     * @brief Vrátí celkovou délku streamu.
     * @param vstream Ukazatel na vstream
     * @return Délka v sekundách
     */
    extern double cmt_vstream_get_length ( st_CMT_VSTREAM *vstream );

    /**
     * @brief Vrátí celkový počet vzorků.
     * @param vstream Ukazatel na vstream
     * @return Počet vzorků
     */
    extern uint64_t cmt_vstream_get_count_scans ( st_CMT_VSTREAM *vstream );

    /**
     * @brief Vrátí délku jednoho vzorku.
     * @param vstream Ukazatel na vstream
     * @return Délka jednoho vzorku v sekundách (1/rate)
     */
    extern double cmt_vstream_get_scantime ( st_CMT_VSTREAM *vstream );

    /**
     * @brief Vrátí aktuální polaritu vstreamu.
     * @param vstream Ukazatel na vstream
     * @return Aktuální polarita signálu
     */
    extern en_CMT_STREAM_POLARITY cmt_vstream_get_polarity ( st_CMT_VSTREAM *vstream );


    /* === Pomocné inline funkce pro bezpečný nealignovaný přístup === */

    /**
     * @brief Načte uint16_t z libovolné pozice v datovém poli (bezpečně přes memcpy).
     * @param data Ukazatel na datové pole
     * @param offset Offset v bajtech
     * @return Načtená hodnota uint16_t
     */
    static inline uint16_t cmt_vstream_read_u16 ( const uint8_t *data, uint32_t offset ) {
        uint16_t val;
        memcpy ( &val, &data[offset], sizeof ( uint16_t ) );
        return val;
    }

    /**
     * @brief Načte uint32_t z libovolné pozice v datovém poli (bezpečně přes memcpy).
     * @param data Ukazatel na datové pole
     * @param offset Offset v bajtech
     * @return Načtená hodnota uint32_t
     */
    static inline uint32_t cmt_vstream_read_u32 ( const uint8_t *data, uint32_t offset ) {
        uint32_t val;
        memcpy ( &val, &data[offset], sizeof ( uint32_t ) );
        return val;
    }

    /**
     * @brief Zapíše uint16_t na libovolnou pozici v datovém poli (bezpečně přes memcpy).
     * @param data Ukazatel na datové pole
     * @param offset Offset v bajtech
     * @param val Hodnota k zápisu
     */
    static inline void cmt_vstream_write_u16 ( uint8_t *data, uint32_t offset, uint16_t val ) {
        memcpy ( &data[offset], &val, sizeof ( uint16_t ) );
    }

    /**
     * @brief Zapíše uint32_t na libovolnou pozici v datovém poli (bezpečně přes memcpy).
     * @param data Ukazatel na datové pole
     * @param offset Offset v bajtech
     * @param val Hodnota k zápisu
     */
    static inline void cmt_vstream_write_u32 ( uint8_t *data, uint32_t offset, uint32_t val ) {
        memcpy ( &data[offset], &val, sizeof ( uint32_t ) );
    }


    /**
     * @brief Přečte délku posledního čteného eventu z aktuální čtecí pozice.
     *
     * Vrací počet vzorků a nastaví *event_byte_length na počet spotřebovaných bajtů.
     *
     * @param cmt_vstream Ukazatel na vstream
     * @param event_byte_length Výstupní parametr — počet spotřebovaných bajtů
     * @return Počet vzorků v aktuálním eventu
     */
    static inline uint32_t cmt_vstream_get_last_read_event ( st_CMT_VSTREAM *cmt_vstream, int *event_byte_length ) {
        if ( cmt_vstream->min_byte_length == CMT_VSTREAM_BYTELENGTH8 ) {
            uint8_t value8 = cmt_vstream->data[cmt_vstream->last_read_position];
            *event_byte_length = 1;
            if ( value8 < 0xff ) return value8;
            uint16_t value16 = cmt_vstream_read_u16 ( cmt_vstream->data, cmt_vstream->last_read_position + 1 );
            *event_byte_length = 1 + 2;
            if ( value16 < 0xffff ) return value16;
            uint32_t value32 = cmt_vstream_read_u32 ( cmt_vstream->data, cmt_vstream->last_read_position + 1 + 2 );
            *event_byte_length = 1 + 2 + 4;
            return value32;
        } else if ( cmt_vstream->min_byte_length == CMT_VSTREAM_BYTELENGTH16 ) {
            uint16_t value16 = cmt_vstream_read_u16 ( cmt_vstream->data, cmt_vstream->last_read_position );
            *event_byte_length = 2;
            if ( value16 < 0xffff ) return value16;
            uint32_t value32 = cmt_vstream_read_u32 ( cmt_vstream->data, cmt_vstream->last_read_position + 2 );
            *event_byte_length = 2 + 4;
            return value32;
        }
        uint32_t value32 = cmt_vstream_read_u32 ( cmt_vstream->data, cmt_vstream->last_read_position );
        *event_byte_length = 4;
        return value32;
    }


    /**
     * @brief Přečte další pulz ze streamu.
     *
     * Do *samples zapíše délku pulzu (počet vzorků), do *value jeho hodnotu (0|1).
     * Posune čtecí pozici na další event.
     *
     * @param cmt_vstream Ukazatel na vstream
     * @param samples Výstupní parametr — délka pulzu (počet vzorků)
     * @param value Výstupní parametr — hodnota signálu (0 nebo 1)
     * @return EXIT_SUCCESS pokud se podařilo přečíst, EXIT_FAILURE na konci streamu
     */
    static inline int cmt_vstream_read_pulse ( st_CMT_VSTREAM *cmt_vstream, uint64_t *samples, int *value ) {
        if ( cmt_vstream->last_read_position >= cmt_vstream->size ) return EXIT_FAILURE;
        int event_byte_length = 0;
        uint64_t event_samples = cmt_vstream_get_last_read_event ( cmt_vstream, &event_byte_length );
        uint64_t read_samples = event_samples + cmt_vstream->last_read_position_sample;
        *value = cmt_vstream->last_read_value;
        *samples = event_samples;
        cmt_vstream->last_read_value = ~cmt_vstream->last_read_value & 1;
        cmt_vstream->last_read_position += event_byte_length;
        cmt_vstream->last_read_position_sample = read_samples;
        return EXIT_SUCCESS;
    }


    /**
     * @brief Vrátí hodnotu signálu (0|1) na dané vzorkové pozici.
     *
     * Postupně prochází eventy od poslední čtecí pozice — efektivní
     * pro sekvenční přístup.
     *
     * @param cmt_vstream Ukazatel na vstream
     * @param samples Vzorková pozice (absolutní od začátku streamu)
     * @param value Výstupní parametr — hodnota signálu (0 nebo 1)
     * @return EXIT_SUCCESS pokud pozice leží uvnitř streamu, EXIT_FAILURE za koncem
     */
    static inline int cmt_vstream_get_value ( st_CMT_VSTREAM *cmt_vstream, uint64_t samples, int *value ) {
        int event_byte_length = 0;
        while ( cmt_vstream->last_read_position < cmt_vstream->size ) {
            uint64_t event_samples = cmt_vstream_get_last_read_event ( cmt_vstream, &event_byte_length );
            uint64_t read_samples = event_samples + cmt_vstream->last_read_position_sample;
            if ( samples < read_samples ) {
                *value = cmt_vstream->last_read_value;
                return EXIT_SUCCESS;
            }
            cmt_vstream->last_read_value = ~cmt_vstream->last_read_value & 1;
            cmt_vstream->last_read_position += event_byte_length;
            cmt_vstream->last_read_position_sample = read_samples;
        }
        *value = cmt_vstream->last_read_value;
        return EXIT_FAILURE;
    }


#ifdef __cplusplus
}
#endif

#endif /* CMT_VSTREAM_H */
