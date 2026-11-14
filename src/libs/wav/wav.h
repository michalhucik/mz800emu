/**
 * @file   wav.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.1.1
 * @brief  Veřejné API knihovny pro čtení a zápis WAV souborů (RIFF WAVE).
 *
 * Knihovna pro čtení a zápis souborů ve formátu WAV (RIFF WAVE).
 * Čte PCM s 8/16/24/32/64-bit vzorky, IEEE float 32/64 bit
 * a WAVE_FORMAT_EXTENSIBLE se SubFormat PCM nebo IEEE float, mono
 * i vícekanálové.
 *
 * Klíčové vlastnosti:
 *   - robustní chunk scanning (přeskakuje neznámé chunky jako LIST, fact, bext)
 *   - čtení vzorků jako normalizovaná float hodnota i jako 1-bit kvantizace
 *   - zápis WAV souborů (PCM, 8/16-bit)
 *   - typované chybové kódy s textovým popisem
 *   - I/O přes generic_driver (soubory i paměťové buffery)
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-10-18 (2.1.0): čtení IEEE float a WAVE_FORMAT_EXTENSIBLE,
 *   pole sample_format a valid_bits_per_sample, wav_decode_sample().
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


#ifndef WAV_H
#define WAV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "libs/generic_driver/generic_driver.h"


/** @name Konstanty — RIFF chunk identifikátory
 * @{ */
/** @brief Identifikátor RIFF kontejneru */
#define WAV_TAG_RIFF    "RIFF"
/** @brief Identifikátor WAVE typu */
#define WAV_TAG_WAVE    "WAVE"
/** @brief Identifikátor formátového chunku (pozor na koncovou mezeru) */
#define WAV_TAG_FMT     "fmt "
/** @brief Identifikátor datového chunku */
#define WAV_TAG_DATA    "data"
/** @} */

/** @brief Maximální podporovaná bitová hloubka (v bitech) */
#define WAV_MAX_BITS_PER_SAMPLE 64


/** @brief Chybové kódy WAV operací */
    typedef enum en_WAV_ERROR {
        WAV_OK = 0,                         /**< úspěch */
        WAV_ERROR_IO,                       /**< chyba čtení/zápisu (generic_driver) */
        WAV_ERROR_BAD_RIFF,                 /**< chybí RIFF/WAVE identifikátor */
        WAV_ERROR_NO_FMT_CHUNK,             /**< chybí povinný "fmt " chunk */
        WAV_ERROR_NO_DATA_CHUNK,            /**< chybí povinný "data" chunk */
        WAV_ERROR_UNSUPPORTED_CODEC,        /**< nepodporovaný formátový kód (jen PCM a IEEE float, i v EXTENSIBLE) */
        WAV_ERROR_UNSUPPORTED_BPS,          /**< nepodporovaná bitová hloubka */
        WAV_ERROR_CORRUPT_HEADER,           /**< nekonzistentní hlavička (block_size=0 apod.) */
        WAV_ERROR_ALLOC,                    /**< selhání alokace paměti */
        WAV_ERROR_INVALID_PARAM,            /**< neplatný parametr (NULL, pozice mimo rozsah) */
    } en_WAV_ERROR;


/** @name Binární struktury RIFF WAVE (packed, pro přímé mapování na data)
 * @{ */

    /** @brief RIFF hlavička (12 bajtů) */
    typedef struct __attribute__((packed)) st_WAV_RIFF_HEADER {
        uint8_t riff_tag[4];        /**< "RIFF" */
        uint32_t overall_size;      /**< velikost souboru - 8 (little-endian) */
        uint8_t wave_tag[4];        /**< "WAVE" */
    } st_WAV_RIFF_HEADER;

    /** @brief Obecná hlavička chunku (8 bajtů) */
    typedef struct __attribute__((packed)) st_WAV_CHUNK_HEADER {
        uint8_t chunk_tag[4];       /**< identifikátor chunku ("fmt ", "data", ...) */
        uint32_t chunk_size;        /**< velikost dat chunku v bajtech (little-endian) */
    } st_WAV_CHUNK_HEADER;

    /** @brief Formátový kód (format_code v fmt chunku) */
    typedef enum en_WAVE_FORMAT_CODE {
        WAVE_FORMAT_CODE_PCM = 0x0001,          /**< PCM — pulzně kódovaná modulace */
        WAVE_FORMAT_CODE_IEEE_FLOAT = 0x0003,   /**< IEEE float */
        WAVE_FORMAT_CODE_ALAW = 0x0006,         /**< 8-bit ITU-T G.711 A-law */
        WAVE_FORMAT_CODE_MULAW = 0x0007,        /**< 8-bit ITU-T G.711 µ-law */
        WAVE_FORMAT_CODE_EXTENSIBLE = 0xfffe,   /**< určeno hodnotou SubFormat */
    } en_WAVE_FORMAT_CODE;

    /** @brief Formátový chunk — minimální varianta, 16 bajtů (PCM) */
    typedef struct __attribute__((packed)) st_WAV_FMT16 {
        uint16_t format_code;       /**< formátový kód (viz en_WAVE_FORMAT_CODE) */
        uint16_t channels;          /**< počet kanálů (1=mono, 2=stereo, ...) */
        uint32_t sample_rate;       /**< vzorkovací frekvence v Hz (44100, 48000, ...) */
        uint32_t bytes_per_sec;     /**< (sample_rate * channels * bits_per_sample) / 8 */
        uint16_t block_size;        /**< velikost jednoho rámce: (channels * bits_per_sample) / 8 */
        uint16_t bits_per_sample;   /**< bitová hloubka: 8, 16, 24, 32, 64 */
    } st_WAV_FMT16;

    /**
     * @brief Rozšíření formátového chunku WAVE_FORMAT_EXTENSIBLE (bajty 16..39).
     *
     * Následuje za st_WAV_FMT16, chunk "fmt " má pak 40 B a cb_size 22.
     * Zdroj rozložení: mmreg.h (WAVEFORMATEXTENSIBLE), ksmedia.h (GUID
     * KSDATAFORMAT_SUBTYPE_PCM a _IEEE_FLOAT, na disku little-endian:
     * Data1 1 nebo 3, pak 00 00 10 00 80 00 00 AA 00 38 9B 71).
     */
    typedef struct __attribute__((packed)) st_WAV_FMT_EXTENSIBLE_TAIL {
        uint16_t cb_size;               /**< délka rozšíření (pro EXTENSIBLE aspoň 22) */
        uint16_t valid_bits_per_sample; /**< platné bity vzorku (<= bits_per_sample, 0 = celý kontejner) */
        uint32_t channel_mask;          /**< maska reproduktorů (knihovna nepoužívá) */
        uint8_t sub_format[16];         /**< GUID formátu dat (Data1 LE + 12 B ocas) */
    } st_WAV_FMT_EXTENSIBLE_TAIL;

/** @} */


/**
 * @brief Zjednodušená hlavička WAV — výsledek parsování RIFF struktury.
 *
 * Obsahuje všechny informace potřebné pro čtení vzorků.
 *
 * @invariant sample_format je PCM nebo IEEE float; u PCM bits_per_sample
 *            8, 16, 24, 32 nebo 64, u float 32 nebo 64.
 */
    typedef struct st_WAV_SIMPLE_HEADER {
        uint16_t format_code;       /**< formátový kód (host byte-order) */
        uint16_t channels;          /**< počet kanálů */
        uint32_t sample_rate;       /**< vzorkovací frekvence (Hz) */
        uint32_t bytes_per_sec;     /**< datový tok (B/s) */
        uint16_t block_size;        /**< velikost jednoho rámce v bajtech */
        uint32_t blocks;            /**< celkový počet rámců (vzorků) */
        uint16_t bits_per_sample;   /**< bitová hloubka */
        uint32_t real_data_offset;  /**< offset datové oblasti v souboru (bajty) */
        uint32_t data_size;         /**< velikost datové oblasti (bajty) */
        double count_sec;           /**< délka záznamu v sekundách */
        uint16_t sample_format;     /**< skutečné kódování vzorků: WAVE_FORMAT_CODE_PCM nebo
                                         WAVE_FORMAT_CODE_IEEE_FLOAT (u EXTENSIBLE ze SubFormat);
                                         format_code zůstává surový tag ze souboru */
        uint16_t valid_bits_per_sample; /**< platné bity vzorku (bez EXTENSIBLE = bits_per_sample) */
    } st_WAV_SIMPLE_HEADER;

    /** @brief Polarita při 1-bit kvantizaci */
    typedef enum en_WAV_POLARITY {
        WAV_POLARITY_NORMAL = 0,    /**< kladné vzorky → 0, záporné → 1 */
        WAV_POLARITY_INVERTED,      /**< kladné vzorky → 1, záporné → 0 */
    } en_WAV_POLARITY;


/** @name API — parsování WAV hlavičky
 * @{ */

    /**
     * @brief Vytvoří zjednodušenou hlavičku z WAV dat v handleru.
     *
     * Alokuje novou st_WAV_SIMPLE_HEADER na heapu. Robustně prochází chunky
     * — přeskakuje neznámé (LIST, fact, bext, ...) dokud nenajde "fmt " a "data".
     * Respektuje RIFF padding (zarovnání na 2 B).
     *
     * Volající musí uvolnit přes wav_simple_header_destroy().
     *
     * @param h   Otevřený handler s WAV daty
     * @param err Výstupní chybový kód (může být NULL)
     * @return Ukazatel na hlavičku, nebo NULL při chybě
     */
    extern st_WAV_SIMPLE_HEADER* wav_simple_header_new_from_handler ( st_HANDLER *h, en_WAV_ERROR *err );

    /**
     * @brief Uvolní st_WAV_SIMPLE_HEADER alokovanou přes wav_simple_header_new_from_handler().
     *
     * Bezpečné volání s NULL (no-op).
     *
     * @param simple_header Ukazatel na hlavičku k uvolnění (může být NULL)
     */
    extern void wav_simple_header_destroy ( st_WAV_SIMPLE_HEADER *simple_header );

    /**
     * @brief Vrátí textový popis chybového kódu.
     *
     * Vždy vrací platný řetězec (nikdy NULL). Pro neznámé kódy vrací "neznámá chyba".
     *
     * @param err Chybový kód
     * @return Textový popis chybového kódu
     */
    extern const char* wav_error_string ( en_WAV_ERROR err );

/** @} */


/** @name API — čtení vzorků
 * @{ */

    /**
     * @brief Přečte vzorek na dané pozici a vrátí jeho normalizovanou hodnotu [-1.0, 1.0].
     *
     * Pro 8-bit unsigned PCM: 0→-1.0, 128→0.0, 255→+1.0.
     * Pro 16/24/32/64-bit signed PCM: normalizace rozsahem datového typu.
     * Pro IEEE float 32/64: hodnota beze změny (může ležet mimo ±1.0),
     * NaN = 0.0, ±Inf = ±1.0. Převod dělá wav_decode_sample().
     *
     * @param h               Otevřený handler s WAV daty
     * @param sh              Parsovaná hlavička
     * @param sample_position Index rámce (0-based, musí být < sh->blocks)
     * @param channel         Index kanálu (0 = první/levý, musí být < sh->channels)
     * @param value           Výstupní normalizovaná hodnota
     * @return WAV_OK při úspěchu, jinak chybový kód
     */
    extern en_WAV_ERROR wav_get_sample_float ( st_HANDLER *h, const st_WAV_SIMPLE_HEADER *sh,
                                               uint32_t sample_position, uint16_t channel,
                                               double *value );

    /**
     * @brief Převede jeden vzorek z bajtů souboru na normalizovanou hodnotu.
     *
     * Jediný převod vzorků knihovny (používá ho wav_get_sample_float()
     * i hromadné čtení v wav_analyzer). PCM: 8 bit unsigned, 16/24/32/64
     * bit signed little-endian, normalizace rozsahem kontejneru. IEEE
     * float 32/64 little-endian: hodnota beze změny, NaN = 0.0, ±Inf = ±1.0
     * (NaN by jinak rozbil filtry předzpracování).
     *
     * @param sh  Hlavička (rozhoduje sample_format a bits_per_sample)
     * @param raw Bajty vzorku (bits_per_sample / 8 bajtů)
     * @param value Výstupní hodnota
     * @return WAV_OK; WAV_ERROR_INVALID_PARAM (NULL); WAV_ERROR_UNSUPPORTED_BPS
     *         (bitová hloubka nepatří ke kódování)
     */
    extern en_WAV_ERROR wav_decode_sample ( const st_WAV_SIMPLE_HEADER *sh, const uint8_t *raw, double *value );

    /**
     * @brief Přečte vzorek a provede 1-bit kvantizaci (prahování kolem nuly).
     *
     * Vždy čte kanál 0 (první). U vícekanálových WAV ignoruje ostatní kanály.
     *
     * @param h               Otevřený handler s WAV daty
     * @param sh              Parsovaná hlavička
     * @param sample_position Index rámce (0-based)
     * @param polarity        Režim kvantizace (WAV_POLARITY_NORMAL/WAV_POLARITY_INVERTED)
     * @param bit_value       Výstup — 0 nebo 1
     * @return WAV_OK při úspěchu, jinak chybový kód
     */
    extern en_WAV_ERROR wav_get_bit_value_of_sample ( st_HANDLER *h, const st_WAV_SIMPLE_HEADER *sh,
                                                      uint32_t sample_position, en_WAV_POLARITY polarity,
                                                      int *bit_value );

/** @} */


/** @name API — zápis WAV souborů
 * @{ */

    /**
     * @brief Zapíše kompletní WAV soubor (hlavičku + data) do handleru.
     *
     * Generuje RIFF WAVE s jedním "fmt " a jedním "data" chunkem.
     * Data musí být v nativním formátu odpovídajícím bits_per_sample:
     *   - 8-bit:  uint8_t pole (unsigned, střed 128)
     *   - 16-bit: int16_t pole (signed, little-endian)
     *
     * @param h               Otevřený handler pro zápis
     * @param sample_rate     Vzorkovací frekvence (Hz), nesmí být 0
     * @param channels        Počet kanálů (1=mono, 2=stereo), nesmí být 0
     * @param bits_per_sample Bitová hloubka (8 nebo 16)
     * @param data            Ukazatel na vzorková data, nesmí být NULL
     * @param data_size       Velikost dat v bajtech
     * @return WAV_OK při úspěchu, jinak chybový kód
     */
    extern en_WAV_ERROR wav_write ( st_HANDLER *h, uint32_t sample_rate, uint16_t channels,
                                    uint16_t bits_per_sample, const void *data, uint32_t data_size );

/** @} */


/** @name Zpětně kompatibilní API (wrappery)
 * @{ */

    /**
     * @brief Zpětně kompatibilní varianta wav_simple_header_new_from_handler().
     *
     * Neposkytuje chybový kód — použijte novou variantu s en_WAV_ERROR*.
     *
     * @param h Otevřený handler s WAV daty
     * @return Ukazatel na hlavičku, nebo NULL při chybě
     */
    extern st_WAV_SIMPLE_HEADER* wav_simple_header_new_from_handler_compat ( st_HANDLER *h );

    /**
     * @brief Zpětně kompatibilní varianta wav_get_bit_value_of_sample().
     *
     * Vrací EXIT_SUCCESS/EXIT_FAILURE místo en_WAV_ERROR.
     *
     * @param h               Otevřený handler s WAV daty
     * @param sh              Parsovaná hlavička
     * @param sample_position Index rámce (0-based)
     * @param polarity        Režim kvantizace
     * @param bit_value       Výstup — 0 nebo 1
     * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě
     */
    extern int wav_get_bit_value_of_sample_compat ( st_HANDLER *h, st_WAV_SIMPLE_HEADER *sh,
                                                    uint32_t sample_position, en_WAV_POLARITY polarity,
                                                    int *bit_value );

/** @} */


    /** @brief Verze knihovny wav. */
#define WAV_VERSION "2.1.1"

    /**
     * @brief Vrátí řetězec s verzí knihovny wav.
     * @return Statický řetězec s verzí (např. "2.0.0").
     */
    extern const char* wav_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* WAV_H */
