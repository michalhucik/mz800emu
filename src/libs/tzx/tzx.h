/**
 * @file   tzx.h
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.4.0
 * @brief  Verejne API knihovny pro format TZX v1.20 - kazetovy archiv ZX Spectrum.
 *
 * Knihovna pro parsovani a generovani audio streamu ze vsech bloku TZX v1.20.
 * Generuje audio bloky 0x10-0x15, 0x18 (CSW, jen komprese RLE) a pauzu 0x20;
 * blok 0x18 umí také vytvořit. Z bloku 0x19 a z řídicích bloků 0x21-0x2B zvuk
 * negeneruje (řídicí bloky zpracuje player).
 *
 * Audio bloky generuji CMT stream (bitstream/vstream) z pulsovych definici.
 * Casovani je v Z80 T-states pri 3.5 MHz, ale lze prepocitat na libovolny
 * CPU takt pomoci prepoctoveho koeficientu.
 *
 * Tato knihovna je pouzivana knihovnou tmz pro prehravani standardnich
 * TZX bloku uvnitr TMZ souboru.
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


#ifndef TZX_H
#define TZX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../cmt_stream/cmt_stream.h"
#include "../generic_driver/generic_driver.h"


/** @defgroup tzx_file_constants Konstanty souboroveho formatu TZX/TMZ
 *  @{ */

/** @brief Signatura TZX souboru ("ZXTape!") */
#define TZX_SIGNATURE           "ZXTape!"

/** @brief Delka signatury v bajtech (bez nuloveho terminatoru) */
#define TZX_SIGNATURE_LENGTH    7

/** @brief EOF marker nasledujici za signaturou (standardni hodnota 0x1A) */
#define TZX_EOF_MARKER          0x1A

/** @brief Hlavni cislo verze TZX formatu */
#define TZX_VERSION_MAJOR       1

/** @brief Vedlejsi cislo verze TZX formatu (TZX v1.20) */
#define TZX_VERSION_MINOR       20

/** @} */


/** @defgroup tzx_block_ids Identifikatory TZX bloku
 *  @{ */

/** @brief Standardni rychlostni datovy blok (1000 T-states pilot, 2168 pilot pulzu) */
#define TZX_BLOCK_ID_STANDARD_SPEED    0x10
/** @brief Turbo rychlostni datovy blok (uzivatelsky definovane casovani) */
#define TZX_BLOCK_ID_TURBO_SPEED       0x11
/** @brief Cisty ton - sekvence pulzu stejne delky */
#define TZX_BLOCK_ID_PURE_TONE         0x12
/** @brief Sekvence pulzu ruznych delek */
#define TZX_BLOCK_ID_PULSE_SEQUENCE    0x13
/** @brief Cista data bez pilot tonu a sync pulzu */
#define TZX_BLOCK_ID_PURE_DATA         0x14
/** @brief Primy zaznam - vzorky s definovanou vzorkovaci frekvenci */
#define TZX_BLOCK_ID_DIRECT_RECORDING  0x15
/** @brief CSW zaznam - komprimovany zvukovy zaznam */
#define TZX_BLOCK_ID_CSW_RECORDING     0x18
/** @brief Generalizovany datovy blok (TZX v1.20) */
#define TZX_BLOCK_ID_GENERALIZED_DATA  0x19
/** @brief Pauza (ticho) nebo zastaveni pasky */
#define TZX_BLOCK_ID_PAUSE             0x20
/** @brief Zacatek skupiny bloku */
#define TZX_BLOCK_ID_GROUP_START       0x21
/** @brief Konec skupiny bloku */
#define TZX_BLOCK_ID_GROUP_END         0x22
/** @brief Skok na jiny blok (relativni offset) */
#define TZX_BLOCK_ID_JUMP             0x23
/** @brief Zacatek smycky (opakujici se sekvence bloku) */
#define TZX_BLOCK_ID_LOOP_START       0x24
/** @brief Konec smycky */
#define TZX_BLOCK_ID_LOOP_END         0x25
/** @brief Sekvence volani bloku (CALL) */
#define TZX_BLOCK_ID_CALL_SEQUENCE    0x26
/** @brief Navrat ze sekvence volani (RETURN) */
#define TZX_BLOCK_ID_RETURN_FROM_SEQ  0x27
/** @brief Vyber bloku z nabidky (interaktivni) */
#define TZX_BLOCK_ID_SELECT_BLOCK     0x28
/** @brief Zastaveni pasky v 48K rezimu */
#define TZX_BLOCK_ID_STOP_48K         0x2A
/** @brief Nastaveni urovne signalu (vysoka/nizka) */
#define TZX_BLOCK_ID_SET_SIGNAL_LEVEL 0x2B
/** @brief Textovy popis (informativni, neni soucasti dat) */
#define TZX_BLOCK_ID_TEXT_DESCRIPTION 0x30
/** @brief Zprava se zobrazenim (casovany text) */
#define TZX_BLOCK_ID_MESSAGE          0x31
/** @brief Archivni informace (nazev, autor, rok, ...) */
#define TZX_BLOCK_ID_ARCHIVE_INFO     0x32
/** @brief Informace o hardwaru (seznam pouziteho HW) */
#define TZX_BLOCK_ID_HARDWARE_TYPE    0x33
/** @brief Uzivatelsky definovany informacni blok */
#define TZX_BLOCK_ID_CUSTOM_INFO      0x35
/**
 * @brief Délka ASCII identifikátoru bloku 0x35 v bajtech.
 *
 * TZX spec uvádí CHAR[10] v hexadecimálním zápisu (= 16 B), verze 1.01
 * opravila délku na 16 B. Za identifikátorem následuje DWORD délka dat.
 */
#define TZX_CUSTOM_INFO_ID_LENGTH     16
/** @brief Spojovaci blok (pro spojeni vice TZX souboru) */
#define TZX_BLOCK_ID_GLUE             0x5A

/** @} */


/** @defgroup tzx_header Hlavicka TZX/TMZ souboru
 *  @{ */

    /**
     * @brief Hlavicka TZX/TMZ souboru - 10 bajtu, packed.
     *
     * Prvnich 7 bajtu obsahuje signaturu ("ZXTape!" nebo "TapeMZ!"),
     * nasleduje EOF marker (0x1A) a dvoubajtove cislo verze.
     *
     * Layout odpovida puvodnimu TZX formatu, takze soubory jsou
     * vzajemne rozpoznatelne podle signatury.
     *
     * @invariant sizeof(st_TZX_HEADER) == 10
     * @invariant eof_marker == 0x1A
     */
    typedef struct __attribute__((packed)) st_TZX_HEADER {
        uint8_t signature[TZX_SIGNATURE_LENGTH]; /**< "ZXTape!" nebo "TapeMZ!" */
        uint8_t eof_marker;                      /**< EOF marker (vzdy 0x1A) */
        uint8_t ver_major;                       /**< hlavni cislo verze */
        uint8_t ver_minor;                       /**< vedlejsi cislo verze */
    } st_TZX_HEADER;

    /* Compile-time overeni velikosti hlavicky */
#ifdef __cplusplus
    static_assert ( sizeof ( st_TZX_HEADER ) == 10,
                    "st_TZX_HEADER musi mit presne 10 bajtu" );
#else
    _Static_assert ( sizeof ( st_TZX_HEADER ) == 10,
                     "st_TZX_HEADER musi mit presne 10 bajtu" );
#endif

/** @} */


/** @defgroup tzx_structs Datove struktury souborove vrstvy
 *  @{ */

    /**
     * @brief Obecny blok TZX/TMZ souboru.
     *
     * Kazdy blok v souboru je identifikovan 1B ID a obsahuje
     * data, jejichz format zavisi na typu bloku.
     *
     * @par Ownership:
     * Pole data je vlastneno touto strukturou a je uvolneno
     * pri destrukci nadrazeneho st_TZX_FILE pres tzx_free().
     * Volajici nesmi uvolnovat data primo.
     */
    typedef struct st_TZX_BLOCK {
        uint8_t id;       /**< identifikator bloku (viz TZX_BLOCK_ID_*) */
        uint32_t length;  /**< delka dat za timto polem v bajtech */
        uint8_t *data;    /**< surova data bloku (vlastneno touto strukturou) */
    } st_TZX_BLOCK;


    /**
     * @brief Naparsovany TZX/TMZ soubor.
     *
     * Obsahuje hlavicku souboru a dynamicke pole vsech nactenych bloku.
     * Pole blocks je vlastneno touto strukturou a je uvolneno
     * pri volani tzx_free().
     *
     * @par Ownership:
     * Struktura a vsechna jeji data (vcetne bloku a jejich dat)
     * jsou vlastnena touto strukturou. Uvolneni probiha pres tzx_free().
     *
     * @invariant block_count odpovida poctu prvku v poli blocks
     * @invariant blocks != NULL kdyz block_count > 0
     */
    typedef struct st_TZX_FILE {
        st_TZX_HEADER header;     /**< hlavicka souboru (10 bajtu) */
        uint32_t block_count;     /**< pocet bloku v poli blocks */
        uint32_t block_capacity;  /**< alokovana kapacita pole blocks (pocet prvku) */
        st_TZX_BLOCK *blocks;     /**< pole bloku (vlastneno touto strukturou) */
        bool is_tmz;              /**< true = signatura "TapeMZ!", false = "ZXTape!" */
    } st_TZX_FILE;

/** @} */


/** @defgroup tzx_constants Konstanty formatu
 *  @{ */

/** @brief Vychozi CPU takt pro T-states prepocet (ZX Spectrum 3.5 MHz) */
#define TZX_DEFAULT_CPU_CLOCK   3500000

/** @brief Pocet T-states na vzorek pri 44100 Hz (79 T/vzorek) */
#define TZX_TSTATES_PER_SAMPLE_44100   79

/** @brief Pocet T-states na vzorek pri 22050 Hz (158 T/vzorek) */
#define TZX_TSTATES_PER_SAMPLE_22050   158

/** @} */


/** @defgroup tzx_default_timing Vychozi casovani (v T-states pri 3.5 MHz)
 *  Hodnoty ve slozenych zavorkach {} v TZX specifikaci.
 *  @{ */

/** @brief Delka pilot pulzu pro Standard Speed Data {2168} */
#define TZX_PILOT_PULSE_LENGTH      2168
/** @brief Delka 1. sync pulzu {667} */
#define TZX_SYNC1_PULSE_LENGTH      667
/** @brief Delka 2. sync pulzu {735} */
#define TZX_SYNC2_PULSE_LENGTH      735
/** @brief Delka pulzu bitu ZERO {855} */
#define TZX_ZERO_BIT_PULSE_LENGTH   855
/** @brief Delka pulzu bitu ONE {1710} */
#define TZX_ONE_BIT_PULSE_LENGTH    1710
/** @brief Pocet pilot pulzu pro header blok (flag < 128) {8063} */
#define TZX_PILOT_COUNT_HEADER      8063
/** @brief Pocet pilot pulzu pro data blok (flag >= 128) {3223} */
#define TZX_PILOT_COUNT_DATA        3223
/** @brief Vychozi pauza po bloku (ms) {1000} */
#define TZX_DEFAULT_PAUSE_MS        1000

/** @} */


/** @defgroup tzx_enums Vycty
 *  @{ */

    /**
     * @brief Chybove kody TZX operaci.
     *
     * Vsechny funkce vracejici en_TZX_ERROR vraci TZX_OK (0) pri uspechu
     * a konkretni chybovy kod pri selhani.
     */
    typedef enum en_TZX_ERROR {
        TZX_OK = 0,                 /**< operace uspesna */
        TZX_ERROR_NULL_INPUT,        /**< vstupni parametr je NULL */
        TZX_ERROR_INVALID_BLOCK,     /**< neplatna data bloku */
        TZX_ERROR_UNSUPPORTED,       /**< nepodporovany typ bloku */
        TZX_ERROR_STREAM_CREATE,     /**< chyba pri vytvareni CMT streamu */
        TZX_ERROR_ALLOC,             /**< selhani alokace pameti */
        TZX_ERROR_IO,                /**< chyba cteni/zapisu pres generic_driver */
        TZX_ERROR_INVALID_SIGNATURE, /**< signatura neni "ZXTape!" ani "TapeMZ!" */
        TZX_ERROR_INVALID_VERSION,   /**< nepodporovana verze formatu */
        TZX_ERROR_UNEXPECTED_EOF,    /**< neocekavany konec souboru pri cteni bloku */
        TZX_ERROR_UNKNOWN_BLOCK,     /**< neznamy identifikator bloku */
    } en_TZX_ERROR;

/** @} */


/** @defgroup tzx_config Konfigurace
 *  @{ */

    /**
     * @brief Konfigurace TZX generatoru.
     *
     * Urcuje parametry pro generovani audio streamu z TZX bloku.
     * cpu_clock umoznuje prepocet T-states na jiny CPU nez ZX Spectrum 3.5 MHz.
     */
    typedef struct st_TZX_CONFIG {
        uint32_t sample_rate;            /**< vzorkovaci frekvence (Hz), 0 = CMTSTREAM_DEFAULT_RATE */
        en_CMT_STREAM_TYPE stream_type;  /**< typ vystupniho streamu (bitstream/vstream) */
        uint32_t cpu_clock;              /**< CPU takt pro T-states prepocet (Hz), 0 = TZX_DEFAULT_CPU_CLOCK */
    } st_TZX_CONFIG;

    /**
     * @brief Inicializuje konfiguraci TZX generatoru na vychozi hodnoty.
     *
     * Nastavi: sample_rate = 44100, stream_type = vstream,
     * cpu_clock = 3500000 (ZX Spectrum).
     *
     * @param config Ukazatel na konfiguraci k inicializaci.
     *
     * @pre config nesmi byt NULL.
     * @post Vsechna pole konfigurace jsou nastavena na vychozi hodnoty.
     */
    extern void tzx_config_init ( st_TZX_CONFIG *config );

/** @} */


/** @defgroup tzx_conversion Konverzni funkce
 *  Prevod T-states na sekundy a vzorky.
 *  @{ */

    /**
     * @brief Prevede T-states na sekundy pri danem CPU taktu.
     *
     * @param tstates Pocet T-states.
     * @param cpu_clock CPU takt v Hz (0 = TZX_DEFAULT_CPU_CLOCK).
     * @return Doba v sekundach.
     */
    static inline double tzx_tstates_to_seconds ( uint32_t tstates, uint32_t cpu_clock ) {
        if ( cpu_clock == 0 ) cpu_clock = TZX_DEFAULT_CPU_CLOCK;
        return (double) tstates / (double) cpu_clock;
    }

    /**
     * @brief Prevede T-states na pocet vzorku pri danem sample rate a CPU taktu.
     *
     * @param tstates Pocet T-states.
     * @param sample_rate Vzorkovaci frekvence v Hz.
     * @param cpu_clock CPU takt v Hz (0 = TZX_DEFAULT_CPU_CLOCK).
     * @return Pocet vzorku (zaokrouhleno).
     */
    static inline uint32_t tzx_tstates_to_samples ( uint32_t tstates, uint32_t sample_rate, uint32_t cpu_clock ) {
        if ( cpu_clock == 0 ) cpu_clock = TZX_DEFAULT_CPU_CLOCK;
        return (uint32_t) ( ( (double) tstates * (double) sample_rate ) / (double) cpu_clock + 0.5 );
    }

/** @} */


/** @defgroup tzx_block_structs Parsovane blokove struktury
 *  Struktury pro naparsovana data z TZX bloku.
 *  Nejsou packed - pouzivaji se az po parsovani z raw dat.
 *  @{ */

    /**
     * @brief Naparsovany blok 0x10 - Standard Speed Data.
     *
     * Data jako v .TAP souborech se standardnim casovanim ROM.
     * Pilot ton: 8063 pulzu pokud flag < 128, jinak 3223.
     */
    typedef struct st_TZX_STANDARD_SPEED {
        uint16_t pause_ms;   /**< pauza po bloku (ms) */
        uint16_t data_length; /**< delka dat */
        uint8_t *data;       /**< ukazatel na data (ukazuje do raw bloku) */
    } st_TZX_STANDARD_SPEED;

    /**
     * @brief Naparsovany blok 0x11 - Turbo Speed Data.
     *
     * Data s uzivatelsky definovanym casovanim (pilot, sync, pulzy).
     * Umoznuje zachytit libovolny loader s FM-like kodovanim.
     */
    typedef struct st_TZX_TURBO_SPEED {
        uint16_t pilot_pulse;  /**< delka PILOT pulzu (T-states) */
        uint16_t sync1_pulse;  /**< delka 1. SYNC pulzu */
        uint16_t sync2_pulse;  /**< delka 2. SYNC pulzu */
        uint16_t zero_pulse;   /**< delka pulzu bitu ZERO */
        uint16_t one_pulse;    /**< delka pulzu bitu ONE */
        uint16_t pilot_count;  /**< pocet PILOT pulzu */
        uint8_t  used_bits;    /**< pouzite bity v poslednim bajtu (1-8) */
        uint16_t pause_ms;     /**< pauza po bloku (ms) */
        uint32_t data_length;  /**< delka dat (3 bajty v bloku) */
        uint8_t *data;         /**< ukazatel na data */
    } st_TZX_TURBO_SPEED;

    /**
     * @brief Naparsovany blok 0x12 - Pure Tone.
     *
     * Sekvence pulzu stejne delky (jako pilot ton).
     */
    typedef struct st_TZX_PURE_TONE {
        uint16_t pulse_length; /**< delka jednoho pulzu (T-states) */
        uint16_t pulse_count;  /**< pocet pulzu */
    } st_TZX_PURE_TONE;

    /**
     * @brief Naparsovany blok 0x13 - Pulse Sequence.
     *
     * Sekvence pulzu ruznych delek (max 255 pulzu).
     */
    typedef struct st_TZX_PULSE_SEQUENCE {
        uint8_t  pulse_count;  /**< pocet pulzu */
        uint16_t *pulses;      /**< pole delek pulzu (T-states, ukazuje do raw bloku) */
    } st_TZX_PULSE_SEQUENCE;

    /**
     * @brief Naparsovany blok 0x14 - Pure Data.
     *
     * Data bez pilot a sync pulzu, s vlastnim casovanim bitu.
     */
    typedef struct st_TZX_PURE_DATA {
        uint16_t zero_pulse;   /**< delka pulzu bitu ZERO (T-states) */
        uint16_t one_pulse;    /**< delka pulzu bitu ONE */
        uint8_t  used_bits;    /**< pouzite bity v poslednim bajtu (1-8) */
        uint16_t pause_ms;     /**< pauza po bloku (ms) */
        uint32_t data_length;  /**< delka dat (3 bajty v bloku) */
        uint8_t *data;         /**< ukazatel na data */
    } st_TZX_PURE_DATA;

    /**
     * @brief Naparsovany blok 0x15 - Direct Recording.
     *
     * Primy zaznam vzorku - kazdy bit je jedna uroven signalu (0=low, 1=high).
     * MSb se prehraje prvni.
     */
    typedef struct st_TZX_DIRECT_RECORDING {
        uint16_t tstates_per_sample; /**< pocet T-states na vzorek */
        uint16_t pause_ms;           /**< pauza po bloku (ms) */
        uint8_t  used_bits;          /**< pouzite bity v poslednim bajtu (1-8) */
        uint32_t data_length;        /**< delka dat vzorku (3 bajty v bloku) */
        uint8_t *data;               /**< ukazatel na data vzorku */
    } st_TZX_DIRECT_RECORDING;

    /**
     * @brief Naparsovany blok 0x18 - CSW Recording.
     *
     * Komprimovany zvukovy zaznam ve formatu CSW v2: posloupnost délek pulzů
     * (půlvln) ve vzorcích při sample_rate. V kompresi RLE je každý pulz jeden
     * bajt 1-255; bajt 00h uvozuje 32bitovou délku little-endian. Tato
     * knihovna bere první pulz jako LOW a úrovně dalších střídá.
     *
     * @par Ownership:
     * data ukazuje do surových dat bloku, ze kterých struktura vznikla
     * (tzx_parse_csw_recording()), a platí, dokud platí ona.
     */
    typedef struct st_TZX_CSW_RECORDING {
        uint16_t pause_ms;           /**< pauza po bloku (ms) */
        uint32_t sample_rate;        /**< vzorkovaci frekvence (3 bajty v bloku) */
        uint8_t  compression_type;   /**< typ komprese: 0x01=RLE, 0x02=Z-RLE */
        uint32_t pulse_count;        /**< pocet ulozenych pulzu (po dekompresi) */
        uint32_t data_length;        /**< delka CSW dat */
        uint8_t *data;               /**< ukazatel na CSW data */
    } st_TZX_CSW_RECORDING;

/** @brief Typ komprese bloku 0x18: RLE (CSW v2). */
#define TZX_CSW_COMPRESSION_RLE     0x01
/** @brief Typ komprese bloku 0x18: Z-RLE (RLE + deflate, CSW v2); knihovna ho negeneruje. */
#define TZX_CSW_COMPRESSION_ZRLE    0x02
/** @brief Největší vzorkovací frekvence bloku 0x18 (pole má 3 bajty). */
#define TZX_CSW_MAX_SAMPLE_RATE     0xFFFFFFu

    /**
     * @brief Naparsovany blok 0x20 - Pause / Stop Tape.
     *
     * Ticho po zadany pocet milisekund. Hodnota 0 = zastaveni pasky.
     */
    typedef struct st_TZX_PAUSE {
        uint16_t pause_ms;   /**< doba pauzy (ms), 0 = stop */
    } st_TZX_PAUSE;

/** @} */


/** @defgroup tzx_parse Parsovani bloku
 *  Funkce pro parsovani raw TZX bloku na typovane struktury.
 *  Vsechny parse funkce pracuji zero-copy - data ukazuji do raw bloku.
 *  @{ */

    /**
     * @brief Rozparsuje blok 0x10 (Standard Speed Data) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_standard_speed ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_STANDARD_SPEED *result );

    /**
     * @brief Rozparsuje blok 0x11 (Turbo Speed Data) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_turbo_speed ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_TURBO_SPEED *result );

    /**
     * @brief Rozparsuje blok 0x12 (Pure Tone) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_pure_tone ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PURE_TONE *result );

    /**
     * @brief Rozparsuje blok 0x13 (Pulse Sequence) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_pulse_sequence ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PULSE_SEQUENCE *result );

    /**
     * @brief Rozparsuje blok 0x14 (Pure Data) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_pure_data ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PURE_DATA *result );

    /**
     * @brief Rozparsuje blok 0x15 (Direct Recording) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_direct_recording ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_DIRECT_RECORDING *result );

    /**
     * @brief Rozparsuje blok 0x18 (CSW Recording) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura (result->data ukazuje do raw_data).
     * @return TZX_OK pri uspechu, TZX_ERROR_NULL_INPUT, nebo
     *         TZX_ERROR_INVALID_BLOCK pro blok kratší než 14 B a pro DWORD
     *         délku menší než 10 nebo s daty přesahujícími raw_length.
     */
    extern en_TZX_ERROR tzx_parse_csw_recording ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_CSW_RECORDING *result );

    /**
     * @brief Rozparsuje blok 0x20 (Pause) z raw dat.
     *
     * @param raw_data Surova data bloku (za ID bajtem).
     * @param raw_length Delka surovych dat.
     * @param[out] result Vystupni struktura.
     * @return TZX_OK pri uspechu, jinak chybovy kod.
     */
    extern en_TZX_ERROR tzx_parse_pause ( const uint8_t *raw_data, uint32_t raw_length, st_TZX_PAUSE *result );

/** @} */


/**
 * @brief Úroveň signálu po pauze pro tzx_generate_block(): signál je nízko,
 *        další pulz se zapíše vysoko (vytvoří hranu) a další pauza nezačne
 *        1 ms vysoko.
 *
 * Hodnota parametru level vedle 0 (nízká) a 1 (vysoká). Pro pulzy se chová
 * jako 1, pro pauzu jako 0.
 */
#define TZX_LEVEL_AFTER_PAUSE   2

/** @defgroup tzx_generate Generovani audio streamu
 *  Funkce pro generovani CMT vstreamu z naparsovanych TZX bloku.
 *  Kazda funkce vytvari novy vstream, ktery volajici musi uvolnit.
 *
 *  Úroveň signálu podle TZX 1.20 (kap. "Pravidla pro aktuální úroveň
 *  pulzu"): aktuální úroveň je úroveň dalšího pulzu. Každý pulz se zapíše
 *  na aktuální úrovni a úroveň se pak otočí. Pauza > 0 při vysoké úrovni
 *  nejdřív 1 ms vysoko (dokončí hranu posledního pulzu), zbytek nízko; při
 *  nízké úrovni a po pauze celá nízko. Pause 0 úroveň nemění.
 *  Po bloku 0x15 je úroveň opakem posledního vzorku (spec 0x15 neuvádí,
 *  (hypotéza): další blok tak začne hranou). Blok 0x18 se chová stejně jako
 *  0x15: úrovně určuje blok sám (první pulz LOW, pak střídání), vstupní
 *  úroveň na ně nemá vliv a po posledním pulzu je úroveň jeho opakem (spec
 *  0x18 úroveň neuvádí, převzato z 0x15).
 *
 *  Odchylka od TZX 1.20: po pauze je úroveň TZX_LEVEL_AFTER_PAUSE (signál
 *  nízko, první pulz dalšího bloku vysoko, tedy s hranou), ne nízká. TZX 1.20
 *  zapíše první pulz po pauze nízko a ten splyne s pauzou; skutečný záznam
 *  (Intercopy SINCLAIR v emulátoru) má po mezeře vidět všechny pulzy pilotu.
 *
 *  tzx_generate_block() úroveň přebírá a vrací (pro přehrávání po sobě
 *  jdoucích bloků). Ostatní funkce začínají nízkou úrovní (start
 *  emulátoru podle TZX 1.20) a výslednou úroveň nevracejí.
 *  @{ */

    /**
     * @brief Vygeneruje signál audio bloku TZX s úrovní signálu na vstupu a výstupu.
     *
     * Podporuje bloky 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18 (jen RLE) a 0x20.
     * Data bloku naparsuje příslušnou funkcí tzx_parse_*() a vygeneruje signál
     * podle pravidel úrovně (viz skupina). Pilot bloků 0x10 a 0x11 má
     * pilot_count pulzů (půlvln).
     *
     * @param block Blok (block->data jako v st_TZX_FILE, bez ID).
     * @param config Konfigurace generátoru.
     * @param[in,out] level Aktuální úroveň (0 = nízká, 1 = vysoká,
     *        TZX_LEVEL_AFTER_PAUSE = nízko po pauze, první pulz vysoko): na
     *        vstupu úroveň prvního pulzu, na výstupu aktuální úroveň po bloku
     *        (po pauze TZX_LEVEL_AFTER_PAUSE). Při chybě, u Pause 0 a u bloku
     *        bez signálu se nemění. Jiná nenulová hodnota se bere jako 1.
     * @param err Výstupní chybový kód (může být NULL): TZX_OK;
     *        TZX_ERROR_NULL_INPUT; TZX_ERROR_UNSUPPORTED (jiné ID bloku,
     *        0x18 s kompresí Z-RLE); chyba parseru nebo neplatná data CSW
     *        (TZX_ERROR_INVALID_BLOCK); TZX_ERROR_ALLOC; TZX_ERROR_STREAM_CREATE.
     * @return Nový vstream, nebo NULL: při chybě, nebo u Pause 0 (err TZX_OK,
     *         příkaz zastavení pásky vyhodnocuje přehrávač). Blok bez signálu
     *         (0x12 a 0x13 s 0 pulzy, 0x14 bez dat a s pauzou 0) dá vstream
     *         s 0 vzorky a úroveň nemění.
     *
     * @pre level nesmí být NULL (jinak TZX_ERROR_NULL_INPUT).
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_block ( const st_TZX_BLOCK *block, const st_TZX_CONFIG *config, uint8_t *level,
                                                en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x10 (Standard Speed Data).
     *
     * Generuje pilot ton (8063/3223 pulzu podle flag bajtu), sync pulzy
     * a data se standardnim ROM casovanim. Začíná nízkou úrovní.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volajici musi uvolnit vraceny vstream pres cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_standard_speed ( const st_TZX_STANDARD_SPEED *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x11 (Turbo Speed Data).
     *
     * Generuje pilot ton (pilot_count pulzu), sync pulzy a data s uzivatelsky
     * definovanym casovanim. Začíná nízkou úrovní.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volajici musi uvolnit vraceny vstream pres cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_turbo_speed ( const st_TZX_TURBO_SPEED *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x12 (Pure Tone). Začíná nízkou úrovní.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_pure_tone ( const st_TZX_PURE_TONE *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x13 (Pulse Sequence). Začíná nízkou úrovní.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_pulse_sequence ( const st_TZX_PULSE_SEQUENCE *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x14 (Pure Data). Začíná nízkou úrovní.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_pure_data ( const st_TZX_PURE_DATA *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x15 (Direct Recording).
     *
     * Kazdy bit vstupnich dat je jeden vzorek signalu (0=low, 1=high).
     * MSb se prehraje prvni. Pauza za blokem začíná úrovní opačnou
     * k poslednímu vzorku (viz skupina).
     *
     * Délka celého bloku (vzorky x T na vzorek a pauza, při cpu_clock
     * z konfigurace) nesmí přesáhnout CMT_VSTREAM_MAX_BLOCK_SECONDS; ověří
     * se předem, před alokací vstreamu.
     *
     * @param block Naparsovany blok.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL): TZX_ERROR_UNSUPPORTED
     *        pro blok delší než mez (s hlášením přes error callback).
     * @return Novy vstream, nebo NULL pri chybe.
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_direct_recording ( const st_TZX_DIRECT_RECORDING *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Generuje vstream z bloku 0x18 (CSW Recording), jen komprese RLE.
     *
     * Dekóduje přesně block->pulse_count pulzů z dat RLE (bajt 1-255 = délka,
     * bajt 00h + DWORD little-endian = delší délka). První pulz je LOW, úrovně
     * se střídají. Délky jsou ve vzorcích při block->sample_rate a převádějí se
     * na config->sample_rate kumulativně: konec k-tého pulzu je zaokrouhlený
     * čas součtu prvních k délek, takže chyba celkové délky nepřesáhne půl
     * výstupního vzorku a při shodné frekvenci vyjdou délky přesně. Pulz, který
     * by vyšel na 0 výstupních vzorků, dostane 1 vzorek (střídání úrovní se
     * nezmění); posun, který tím vznikne (nejvýš 1 vzorek za každý takový pulz
     * v řadě), vyrovná první další pulz, jehož zaokrouhlený konec leží za již
     * zapsanými vzorky. Za pulzy následuje pauza
     * pause_ms od úrovně opačné k poslednímu pulzu (viz skupina). Blok
     * s pulse_count 0 a bez dat dá jen pauzu (nebo vstream s 0 vzorky).
     *
     * @param block Naparsovaný blok (např. z tzx_parse_csw_recording()).
     * @param config Konfigurace generátoru (cpu_clock se nepoužívá).
     * @param err Výstupní chybový kód (může být NULL): TZX_OK;
     *        TZX_ERROR_NULL_INPUT (block nebo config NULL, data NULL při
     *        data_length > 0); TZX_ERROR_UNSUPPORTED (komprese Z-RLE, hlášení
     *        přes error callback; signál bloku včetně pauzy delší než
     *        CMT_VSTREAM_MAX_BLOCK_SECONDS, ověřeno tzx_csw_recording_check()
     *        před alokací); TZX_ERROR_INVALID_BLOCK (jiný typ komprese,
     *        sample_rate 0 nebo nad 24 bitů (16 777 215), useknutá 32bitová
     *        délka, délka pulzu 0, data kratší nebo delší než pulse_count
     *        pulzů; vše s hlášením přes error callback); TZX_ERROR_ALLOC;
     *        TZX_ERROR_STREAM_CREATE (selhání zápisu do vstreamu). Pulz
     *        delší než jeden event vstreamu (2^32 výstupních vzorků) se zapíše
     *        jako několik eventů (cmt_vstream_add_value_long()).
     * @return Nový vstream, nebo NULL při chybě.
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_csw_recording ( const st_TZX_CSW_RECORDING *block, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

    /**
     * @brief Ověří blok 0x18 bez generování signálu: kompresi, frekvenci, data a mez délky bloku.
     *
     * Jeden průchod daty (dekódování délek pulzů, bez alokace). Délka celého
     * bloku (součet pulzů a pauza, ve vzorcích záznamu) nesmí přesáhnout
     * CMT_VSTREAM_MAX_BLOCK_SECONDS. tzx_generate_csw_recording() ji volá
     * před alokací vstreamu; program, který chce o nepřehratelném bloku
     * vědět předem (např. při otevření pásky), ji smí volat sám.
     *
     * @param block Naparsovaný blok (tzx_parse_csw_recording()).
     * @return TZX_OK; TZX_ERROR_NULL_INPUT; TZX_ERROR_UNSUPPORTED (komprese
     *         Z-RLE, signál delší než mez bloku); TZX_ERROR_INVALID_BLOCK
     *         (jiný typ komprese, sample_rate 0 nebo nad 24 bitů, useknutá
     *         32bitová délka, pulz 0, data kratší nebo delší než pulse_count
     *         pulzů). Chyby se hlásí přes error callback.
     */
    extern en_TZX_ERROR tzx_csw_recording_check ( const st_TZX_CSW_RECORDING *block );

    /**
     * @brief Generuje vstream pauzy (ticha) od nízké úrovně (celá pauza nízko).
     *
     * @param pause_ms Doba pauzy v milisekundach.
     * @param config Konfigurace generatoru.
     * @param err Vystupni chybovy kod (muze byt NULL).
     * @return Novy vstream, nebo NULL pri chybe a pri pause_ms 0 (err TZX_OK).
     *
     * @post Volající uvolní vrácený vstream přes cmt_vstream_destroy().
     */
    extern st_CMT_VSTREAM* tzx_generate_pause ( uint16_t pause_ms, const st_TZX_CONFIG *config, en_TZX_ERROR *err );

/** @} */


/** @defgroup tzx_error Chybove hlaseni
 *  @{ */

    /**
     * @brief Vrati textovy popis chyboveho kodu en_TZX_ERROR.
     *
     * @param err Chybovy kod.
     * @return Ukazatel na staticky retezec s popisem chyby.
     */
    extern const char* tzx_error_string ( en_TZX_ERROR err );

/** @} */


/** @defgroup tzx_error_cb Error callback
 *  Umoznuje presmerovat chybova hlaseni knihovny do vlastniho handleru.
 *  @{ */

    /**
     * @brief Typ callback funkce pro hlaseni chyb.
     *
     * @param func Jmeno funkce, ve ktere doslo k chybe.
     * @param line Cislo radku ve zdrojovem kodu.
     * @param fmt  Formatovaci retezec (printf-style).
     */
    typedef void (*tzx_error_cb)(const char *func, int line, const char *fmt, ...);

    /**
     * @brief Nastavi vlastni error callback pro knihovnu TZX.
     *
     * Callback je volan pri chybach uvnitr knihovny misto vychoziho
     * vypisu na stderr. Predani NULL resetuje na vychozi.
     *
     * @param cb Ukazatel na callback funkci, nebo NULL pro reset na vychozi.
     */
    extern void tzx_set_error_callback ( tzx_error_cb cb );

/** @} */


/** @defgroup tzx_allocator Uzivatelsky alokator
 *  Umoznuje nahradit vychozi malloc/calloc/free vlastnimi funkcemi.
 *  @{ */

    /**
     * @brief Uzivatelsky alokator pameti pro knihovnu TZX.
     *
     * Pokud neni nastaven vlastni alokator, knihovna pouziva
     * standardni systemove funkce malloc, calloc a free.
     */
    typedef struct st_TZX_ALLOCATOR {
        void* (*alloc)(size_t size);   /**< alokace pameti (jako malloc) */
        void* (*alloc0)(size_t size);  /**< alokace s nulovanim (jako calloc) */
        void  (*free)(void *ptr);      /**< uvolneni pameti (jako free) */
    } st_TZX_ALLOCATOR;

    /**
     * @brief Nastavi vlastni alokator pro knihovnu TZX.
     *
     * Predany alokator se pouzije pro vsechny nasledujici alokace
     * v ramci knihovny. Predani NULL resetuje na vychozi systemove funkce.
     *
     * @param allocator Ukazatel na strukturu alokatoru, nebo NULL pro reset na vychozi.
     */
    extern void tzx_set_allocator ( const st_TZX_ALLOCATOR *allocator );

/** @} */


/** @defgroup tzx_file_io Souborove I/O API
 *  Nacitani, ukladani a uvolnovani TZX/TMZ souboru.
 *  @{ */

    /**
     * @brief Nacte cely TZX/TMZ soubor (hlavicku + vsechny bloky) z handleru.
     *
     * Alokuje a vraci novou st_TZX_FILE strukturu na heapu.
     * Rozpozna signaturu "ZXTape!" i "TapeMZ!" a nastavi priznak is_tmz.
     * Volajici musi uvolnit pres tzx_free().
     *
     * Konec souboru je platný jen na hranici bloku. Useknutý blok (v délce
     * nebo v datech) je chyba TZX_ERROR_UNEXPECTED_EOF, délka přetékající
     * 32bit offset je TZX_ERROR_INVALID_BLOCK. Data bloku se alokují až po
     * ověření, že je soubor obsahuje. Částečně načtený soubor se nevrací.
     * Hlavička s major verzí jinou než 1 je TZX_ERROR_INVALID_VERSION
     * (TZX 1.20 i TMZ 1.0/1.1 mají major 1).
     *
     * @param h   Otevreny handler s TZX/TMZ daty
     * @param err Vystupni chybovy kod (muze byt NULL)
     * @return Ukazatel na st_TZX_FILE, nebo NULL pri chybe
     *
     * @pre h != NULL a handler musi byt otevreny (HANDLER_STATUS_READY)
     * @post Pri uspechu vraci platnou strukturu, *err == TZX_OK
     * @post Pri chybe vraci NULL, *err obsahuje konkretni chybovy kod
     */
    extern st_TZX_FILE* tzx_load ( st_HANDLER *h, en_TZX_ERROR *err );

    /**
     * @brief Zapise cely TZX/TMZ soubor (hlavicku + vsechny bloky) do handleru.
     *
     * Zapisuje bloky sekvencne v poradi, v jakem jsou ulozeny v poli blocks.
     * Signatura v hlavicce se zapisuje tak, jak je nastavena ve strukture.
     *
     * @param h    Otevreny handler pro zapis
     * @param file TZX data k ulozeni (nesmi byt NULL)
     * @return TZX_OK pri uspechu, jinak chybovy kod
     *
     * @pre h != NULL, file != NULL
     * @pre Handler musi byt otevreny pro zapis (ne READ_ONLY)
     */
    extern en_TZX_ERROR tzx_save ( st_HANDLER *h, const st_TZX_FILE *file );

    /**
     * @brief Uvolni st_TZX_FILE strukturu vcetne vsech bloku a jejich dat.
     *
     * Bezpecne volat s NULL (no-op). Uvolni vsechna data vlastnena
     * jednotlivymi bloky a nasledne pole bloku a samotnou strukturu.
     *
     * @param file Ukazatel na strukturu k uvolneni (muze byt NULL)
     */
    extern void tzx_free ( st_TZX_FILE *file );

/** @} */


/** @defgroup tzx_header_ops Operace s hlavickou
 *  Cteni, zapis a validace hlavicky TZX/TMZ souboru.
 *  @{ */

    /**
     * @brief Nacte TZX/TMZ hlavicku z handleru.
     *
     * Cte 10 bajtu od aktualni pozice (offset 0) a naplni strukturu hlavicky.
     * Validuje signaturu ("ZXTape!" nebo "TapeMZ!") a EOF marker.
     *
     * @param h      Otevreny handler s TZX/TMZ daty
     * @param header Vystupni buffer pro hlavicku
     * @return TZX_OK pri uspechu, TZX_ERROR_IO pri chybe cteni
     *
     * @pre h != NULL, header != NULL
     */
    extern en_TZX_ERROR tzx_read_header ( st_HANDLER *h, st_TZX_HEADER *header );

    /**
     * @brief Zapise TZX/TMZ hlavicku do handleru.
     *
     * Zapisuje 10 bajtu od aktualni pozice (offset 0).
     *
     * @param h      Otevreny handler pro zapis
     * @param header Hlavicka k zapisu
     * @return TZX_OK pri uspechu, TZX_ERROR_IO pri chybe zapisu
     *
     * @pre h != NULL, header != NULL
     * @pre Handler musi byt otevreny pro zapis
     */
    extern en_TZX_ERROR tzx_write_header ( st_HANDLER *h, const st_TZX_HEADER *header );

    /**
     * @brief Overi, zda hlavicka obsahuje TZX signaturu ("ZXTape!").
     *
     * @param header Ukazatel na hlavicku k overeni
     * @return true pokud signatura odpovida "ZXTape!", false jinak
     *
     * @pre header != NULL
     */
    extern bool tzx_header_is_tzx ( const st_TZX_HEADER *header );

    /**
     * @brief Inicializuje hlavicku vychozimi hodnotami TZX formatu.
     *
     * Nastavi signaturu na "ZXTape!", EOF marker na 0x1A
     * a verzi na TZX_VERSION_MAJOR.TZX_VERSION_MINOR (1.20).
     *
     * @param header Ukazatel na hlavicku k inicializaci
     *
     * @pre header != NULL
     * @post header obsahuje platnou TZX hlavicku
     */
    extern void tzx_header_init ( st_TZX_HEADER *header );

/** @} */


/** @defgroup tzx_block_names Nazvy bloku
 *  @{ */

    /**
     * @brief Vrati textovy nazev TZX bloku pro dany identifikator.
     *
     * Vraci lidsky citelny nazev bloku (napr. "Standard Speed Data").
     * Pro nezname ID vraci "Unknown". Vzdy vraci platny retezec (nikdy NULL).
     *
     * @param id Identifikator bloku (viz TZX_BLOCK_ID_*)
     * @return Ukazatel na staticky retezec s nazvem bloku
     */
    extern const char* tzx_block_id_name ( uint8_t id );

/** @} */


/** @defgroup tzx_file_manipulation Manipulace s bloky v souboru
 *  Funkce pro pridavani, odebirani a presun bloku v st_TZX_FILE.
 *  @{ */

    /**
     * @brief Odebere blok na danem indexu z pole bloku.
     *
     * Uvolni data bloku a posune zbyle bloky doleva.
     * Dekrementuje block_count.
     *
     * @param file TZX soubor.
     * @param index Index bloku k odebrani (0-based).
     * @return TZX_OK pri uspechu, TZX_ERROR_NULL_INPUT nebo TZX_ERROR_INVALID_BLOCK.
     *
     * @pre file != NULL, index < file->block_count
     * @post block_count je o 1 mensi, data odebranehp bloku jsou uvolnena
     */
    extern en_TZX_ERROR tzx_file_remove_block ( st_TZX_FILE *file, uint32_t index );

    /**
     * @brief Vlozi blok na dany index (posune ostatni doprava).
     *
     * Prebira ownership dat bloku - volajici nesmi dale uvolnovat block->data.
     * Pri nedostatku kapacity pole reallokuje na dvojnasobek.
     *
     * @param file TZX soubor.
     * @param index Pozice pro vlozeni (0 = na zacatek, block_count = na konec).
     * @param block Blok k vlozeni (obsah se zkopiruje, ownership dat predan).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre file != NULL, block != NULL, index <= file->block_count
     * @post block_count je o 1 vetsi
     */
    extern en_TZX_ERROR tzx_file_insert_block ( st_TZX_FILE *file, uint32_t index, const st_TZX_BLOCK *block );

    /**
     * @brief Prida blok na konec pole bloku.
     *
     * Ekvivalent tzx_file_insert_block(file, file->block_count, block).
     *
     * @param file TZX soubor.
     * @param block Blok k pridani (ownership dat predan).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre file != NULL, block != NULL
     */
    extern en_TZX_ERROR tzx_file_append_block ( st_TZX_FILE *file, const st_TZX_BLOCK *block );

    /**
     * @brief Presune blok z pozice from na pozici to.
     *
     * Blok se odebere z from a vlozi na to. Indexy se chovaji
     * jako: nejdrive remove(from), pak insert(to).
     *
     * @param file TZX soubor.
     * @param from Zdrojova pozice (0-based).
     * @param to Cilova pozice (0-based, po odebrani).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre file != NULL, from < block_count, to <= block_count-1
     */
    extern en_TZX_ERROR tzx_file_move_block ( st_TZX_FILE *file, uint32_t from, uint32_t to );

    /**
     * @brief Spoji bloky z src za bloky v dst.
     *
     * Vsechny bloky ze src se pripoji na konec dst.
     * Src je po operaci prazdny (block_count == 0, blocks == NULL).
     * Ownership dat je preveden na dst.
     *
     * @param dst Cilovy soubor (bloky se pridaji na konec).
     * @param src Zdrojovy soubor (bude vyprazdnen).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre dst != NULL, src != NULL
     * @post src->block_count == 0, src->blocks == NULL
     */
    extern en_TZX_ERROR tzx_file_merge ( st_TZX_FILE *dst, st_TZX_FILE *src );

/** @} */


/** @defgroup tzx_block_create Vytvareni TZX info bloku
 *  Funkce pro vytvareni standardnich TZX informacnich bloku.
 *  @{ */

    /**
     * @brief Zaznam Archive Info bloku (0x32).
     *
     * Pouziva se jako vstup pro tzx_block_create_archive_info().
     * Typ zaznamu urcuje vyznam textu (nazev, autor, rok, ...).
     */
    typedef struct st_TZX_ARCHIVE_ENTRY {
        uint8_t type_id;     /**< typ zaznamu (0x00=Title, 0x01=Publisher, 0x02=Author, 0x03=Year, 0xFF=Comment) */
        const char *text;    /**< ASCII text zaznamu (nesmi byt NULL) */
    } st_TZX_ARCHIVE_ENTRY;

    /**
     * @brief Vytvori blok 0x30 (Text Description).
     *
     * Alokuje data bloku: [1B delka textu][text].
     *
     * @param text Textovy retezec (max 255 znaku).
     * @param[out] block Vystupni blok (id a data budou naplneny).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre text != NULL, block != NULL
     * @post block->data je vlastneno volajicim (uvolnit pres free)
     */
    extern en_TZX_ERROR tzx_block_create_text_description ( const char *text, st_TZX_BLOCK *block );

    /**
     * @brief Vytvori blok 0x31 (Message Block).
     *
     * Alokuje data bloku: [1B cas][1B delka textu][text].
     *
     * @param text Textovy retezec (max 255 znaku).
     * @param time_seconds Cas zobrazeni zpravy v sekundach.
     * @param[out] block Vystupni blok (id a data budou naplneny).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre text != NULL, block != NULL
     * @post block->data je vlastneno volajicim (uvolnit pres free)
     */
    extern en_TZX_ERROR tzx_block_create_message ( const char *text, uint8_t time_seconds, st_TZX_BLOCK *block );

    /**
     * @brief Vytvori blok 0x32 (Archive Info).
     *
     * Alokuje data bloku: [2B celkova delka][1B pocet][N * (1B typ, 1B delka, text)].
     *
     * @param entries Pole zaznamu.
     * @param count Pocet zaznamu.
     * @param[out] block Vystupni blok (id a data budou naplneny).
     * @return TZX_OK při úspěchu; TZX_ERROR_NULL_INPUT (NULL vstup, count 0,
     *         text NULL); TZX_ERROR_INVALID_BLOCK, když tělo (počet + záznamy)
     *         přesáhne 65535 B, které unese WORD délky; TZX_ERROR_ALLOC.
     *         Při chybě se block nemění.
     *
     * @pre entries != NULL, count > 0, block != NULL
     * @post block->data je vlastneno volajicim (uvolnit pres free)
     */
    extern en_TZX_ERROR tzx_block_create_archive_info ( const st_TZX_ARCHIVE_ENTRY *entries, uint8_t count, st_TZX_BLOCK *block );

    /**
     * @brief Vytvori blok 0x15 (Direct Recording).
     *
     * Serializuje parametry Direct Recording do binarniho formatu TZX bloku:
     * [2B tstates_per_sample LE][2B pause_ms LE][1B used_bits][3B data_length LE][data].
     * Data jsou zkopirovana - volajici muze uvolnit sve.
     *
     * @param tstates_per_sample Pocet T-states na jeden vzorek.
     * @param pause_ms Pauza po bloku v milisekundach.
     * @param used_bits Pouzite bity v poslednim bajtu (1-8).
     * @param data Bitova data zaznamu. Nesmi byt NULL pokud data_length > 0.
     * @param data_length Delka dat v bajtech.
     * @param[out] block Vystupni blok (id a data budou naplneny).
     * @return TZX_OK pri uspechu, chybovy kod jinak.
     *
     * @pre block != NULL
     * @post block->data je vlastneno volajicim (uvolnit pres free)
     */
    extern en_TZX_ERROR tzx_block_create_direct_recording (
        uint16_t tstates_per_sample,
        uint16_t pause_ms,
        uint8_t used_bits,
        const uint8_t *data,
        uint32_t data_length,
        st_TZX_BLOCK *block
    );

    /**
     * @brief Vytvoří blok 0x18 (CSW Recording) s kompresí RLE.
     *
     * Data bloku mají tvar jako v st_TZX_FILE (tzx_load()): [DWORD délka za
     * tímto polem = 10 + N][WORD pause_ms][3B sample_rate][1B komprese = 01h]
     * [DWORD pulse_count][N B dat RLE]. V RLE je pulz 1-255 vzorků jeden bajt,
     * delší pulz bajt 00h a DWORD délky little-endian (5 B). Z-RLE se nezapisuje.
     *
     * Úrovně: první pulz je LOW, další se střídají. Signál, který začíná
     * úrovní HIGH, zapíše volající s předřazeným pulzem LOW délky 1 vzorek,
     * jinak by se přehrál s obrácenou polaritou.
     *
     * @param sample_rate Vzorkovací frekvence záznamu v Hz (1 až
     *        TZX_CSW_MAX_SAMPLE_RATE).
     * @param pause_ms Pauza po bloku v milisekundách.
     * @param pulses Délky pulzů ve vzorcích při sample_rate; každá >= 1 (nula
     *        je v RLE únikový znak). Pole se jen čte.
     * @param pulse_count Počet pulzů (>= 1).
     * @param[out] block Výstupní blok (id, length, data).
     * @return TZX_OK při úspěchu; TZX_ERROR_NULL_INPUT (pulses nebo block
     *         NULL, pulse_count 0); TZX_ERROR_INVALID_BLOCK (sample_rate 0 nebo
     *         nad 24 bitů, pulz délky 0, blok by přesáhl 0xFFFFFFFF B; s hlášením
     *         přes error callback); TZX_ERROR_ALLOC. Při chybě se block nemění.
     *
     * @pre pulses ukazuje na nejméně pulse_count prvků.
     * @post block->data je vlastněno volajícím a alokováno alokátorem knihovny
     *       (výchozí malloc, uvolnit free nebo přes tzx_free() po vložení do
     *       souboru).
     */
    extern en_TZX_ERROR tzx_block_create_csw_recording ( uint32_t sample_rate, uint16_t pause_ms, const uint32_t *pulses,
                                                         uint32_t pulse_count, st_TZX_BLOCK *block );

/** @} */


    /** @brief Verze knihovny tzx. */
#define TZX_VERSION "1.4.0"

    /** @brief Verze formátu TZX jako řetězec ("1.20"). */
#define TZX_FORMAT_VERSION "1.20"

    /**
     * @brief Vrátí řetězec s verzí knihovny tzx.
     * @return Statický řetězec s verzí (např. "1.0.0").
     */
    extern const char* tzx_version ( void );

    /**
     * @brief Vrátí řetězec s verzí formátu TZX.
     * @return Statický řetězec s verzí formátu (např. "1.20").
     */
    extern const char* tzx_format_version ( void );

#ifdef __cplusplus
}
#endif

#endif /* TZX_H */
