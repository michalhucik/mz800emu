/**
 * @file   zxtape.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.1
 * @brief  Implementace konverze ZX Spectrum TAP bloků na CMT audio streamy.
 *
 * Obsahuje generování pilot/sync/data pulzů, bitstream a vstream path,
 * unified stream wrapper, výpočet pulzů a validaci checksumu.
 * Pulzní konstanty naměřeny z programu Intercopy na MZ-800.
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"
#include "libs/cmt_stream/cmt_stream.h"

#include "zxtape.h"

/**
 * @brief Přehled TAP formátu a pulzních parametrů.
 *
 * Hodnoty délek všech pulzů byly naměřeny z programu Intercopy:
 *
 * @verbatim
 *             |------|
 *       ______|      |
 * @endverbatim
 *
 * Typy pulzů:
 *  - PILOT: 612.075 us, 612.085 us
 *  - SYNC: 145.475, 186.085
 *  - LONG: 483.795 us, 483.805 us
 *  - SHORT: 241.895 us, 241.905 us
 *
 * Struktura:
 *  - 4032 PILOT pulzů
 *  - SYNC_HIGH
 *  - SYNC_LOW
 *  - Header Flag: 0x00
 *  - Header:
 *      uint8_t code (1 - 3)
 *      uint8_t name[10] (nevyužitá část je vyplněna mezerami 0x20)
 *      uint16_t datablock_length;
 *      uint16_t param1
 *      uint16_t param2
 *  - Header checksum (XOR všech bajtů z headeru)
 *  - následuje pauza s klidovým stavem signálu 1 (simulujeme jednou půlvlnou)
 *  - 1610 PILOT pulzů
 *  - Data Flag: 0xff
 *  - Data
 *  - Data checksum
 *  - následuje pauza s klidovým stavem signálu 1 (simulujeme jednou půlvlnou)
 *
 * Datové bity jsou posílány od nejvyššího k nejnižšímu.
 * Jako stop bit se používá jeden short!
 */


/* ========================================================================
 * Výchozí alokátor a error callback
 * ======================================================================== */

/** @brief Výchozí alokátor — obaluje malloc. */
static void* zxtape_default_alloc ( size_t size ) { return malloc ( size ); }

/** @brief Výchozí alokátor s nulováním — obaluje calloc. */
static void* zxtape_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }

/** @brief Výchozí uvolnění paměti — obaluje free. */
static void  zxtape_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Výchozí instance alokátoru (malloc/calloc/free). */
static const st_ZXTAPE_ALLOCATOR g_zxtape_default_allocator = {
    zxtape_default_alloc,
    zxtape_default_alloc0,
    zxtape_default_free,
};

/** @brief Aktuálně nastavený alokátor. */
static const st_ZXTAPE_ALLOCATOR *g_zxtape_allocator = &g_zxtape_default_allocator;


/**
 * @brief Nastaví vlastní alokátor pro knihovnu zxtape.
 * @param allocator Ukazatel na strukturu alokátoru, nebo NULL pro reset na výchozí.
 */
void zxtape_set_allocator ( const st_ZXTAPE_ALLOCATOR *allocator ) {
    g_zxtape_allocator = allocator ? allocator : &g_zxtape_default_allocator;
}


/**
 * @brief Výchozí error callback — vypisuje chybové hlášení na stderr.
 * @param func Jméno funkce, ve které chyba nastala.
 * @param line Číslo řádku, na kterém chyba nastala.
 * @param fmt Formátovací řetězec (printf styl).
 */
static void zxtape_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktuálně nastavený error callback. */
static zxtape_error_cb g_zxtape_error_cb = zxtape_default_error_cb;


/**
 * @brief Nastaví vlastní error callback pro knihovnu zxtape.
 * @param cb Ukazatel na callback funkci, nebo NULL pro reset na výchozí (fprintf stderr).
 */
void zxtape_set_error_callback ( zxtape_error_cb cb ) {
    g_zxtape_error_cb = cb ? cb : zxtape_default_error_cb;
}


/* ========================================================================
 * Pulzní konstanty
 * ======================================================================== */

/**
 * @brief Sekundové konstanty pilot pulzu.
 *
 * Přesný přepočet z naměřených GDG ticků na MZ-800 (pixel clock 17 721 600 Hz).
 * Původní GDG ticky: {10853, 10853}.
 * Hardcoded vzorkové konstanty pro rate=44100 (historický bitstream path): PILOT=27.
 */
static const st_MZTAPE_PULSE_LENGTH g_zxtape_pilot_pulse = {
    0.000612395, 0.000612395, 0.001224790
};

/**
 * @brief Sekundové konstanty sync pulzu.
 *
 * Původní GDG ticky: {2571, 3299}.
 * Hardcoded vzorkové konstanty pro rate=44100: SYNCHIGH=8, SYNCLOW=9.
 */
static const st_MZTAPE_PULSE_LENGTH g_zxtape_sync_pulse = {
    0.000145070, 0.000186161, 0.000331231
};

/**
 * @brief Sekundové konstanty datových pulzů (long a short).
 *
 * Původní GDG ticky: long {8583, 8583}, short {4291, 4291}.
 * Hardcoded vzorkové konstanty pro rate=44100: SHORT=11, LONG=22.
 */
static const st_MZTAPE_PULSES_LENGTH g_zxtape_data_pulses = {
    { 0.000484335, 0.000484335, 0.000968670 }, /* LONG */
    { 0.000242140, 0.000242140, 0.000484280 }, /* SHORT */
};


/** @brief Pole podporovaných rychlostí, zakončené CMTSPEED_NONE. */
const en_CMTSPEED g_zxtape_speed[] = {
                                      CMTSPEED_1_1, /* 1400 Bd */
                                      CMTSPEED_9_7, /* 1800 Bd */
                                      CMTSPEED_3_2, /* 2100 Bd */
                                      CMTSPEED_25_14, /* 2500 Bd */
                                      CMTSPEED_NONE
};


/* ========================================================================
 * Pomocné funkce
 * ======================================================================== */

/**
 * @brief Vrací počet pilot pulzů pro daný typ bloku.
 * @param flag Typ bloku (header/data).
 * @return Počet pilot pulzů, nebo -1 pro neznámý flag.
 */
static int zxtape_get_pilot_length ( en_ZXTAPE_BLOCK_FLAG flag ) {
    switch ( flag ) {
        case ZXTAPE_BLOCK_FLAG_HEADER:
            return ZXTAPE_HEADER_PILOT_LENGTH;
        case ZXTAPE_BLOCK_FLAG_DATA:
            return ZXTAPE_DATA_PILOT_LENGTH;
        default:
            return -1;
    }
}


/* ========================================================================
 * Výpočet celkového počtu pulzů
 * ======================================================================== */

/**
 * @brief Spočítá celkový počet long a short pulzů pro daný TAP blok.
 *
 * Zahrnuje pilot pulzy, sync, datové bity a koncovou pauzu.
 *
 * @param flag Typ bloku (header/data) — určuje počet pilot pulzů.
 * @param data Ukazatel na data TAP bloku.
 * @param data_size Velikost dat v bajtech.
 * @param[out] long_pulses Výstupní ukazatel pro počet long pulzů.
 * @param[out] short_pulses Výstupní ukazatel pro počet short pulzů.
 */
void zxtape_compute_pulses ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, uint64_t *long_pulses, uint64_t *short_pulses ) {

    uint64_t total_long = 0;
    uint64_t total_short = 0;

    if ( !long_pulses || !short_pulses ) return;

    *long_pulses = 0;
    *short_pulses = 0;

    int pilot_length = zxtape_get_pilot_length ( flag );
    if ( pilot_length < 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Unknown ZXtape block flag '0x%02x'\n", flag );
        return;
    }

    /* pilot pulzy (symetrické — 2 půlvlny na pulz) */
    total_long += pilot_length; /* pilot pulzy mají délku long pulzu */

    /* sync — 1 pulz (2 půlvlny s různou délkou) */
    total_short += 1; /* sync je kratší než long */

    /* datové bity */
    if ( data ) {
        for ( unsigned i = 0; i < data_size; i++ ) {
            uint8_t byte = data[i];
            for ( int j = 0; j < 8; j++ ) {
                if ( byte & 0x80 ) {
                    total_long++;
                } else {
                    total_short++;
                }
                byte <<= 1;
            }
        }
    }

    /* koncová pauza — 1 pilot pulz */
    total_long += 1;

    *long_pulses = total_long;
    *short_pulses = total_short;
}


/* ========================================================================
 * Validace checksumu
 * ======================================================================== */

/**
 * @brief Validuje XOR checksum TAP bloku.
 *
 * TAP checksum: XOR všech bajtů bloku (včetně flag bytu).
 * Poslední bajt v data[] je samotný checksum.
 * Validace: XOR všech bajtů (včetně checksumu) musí být 0.
 *
 * @param data Ukazatel na data bloku (flag + payload + checksum).
 * @param data_size Celková velikost dat v bajtech (včetně checksumu).
 * @return 0 = checksum OK, -1 = chyba (NULL data, příliš krátký blok nebo checksum nesedí).
 */
int zxtape_validate_checksum ( uint8_t *data, uint16_t data_size ) {

    if ( !data || data_size < 2 ) return -1;

    /*
     * TAP checksum: XOR všech bajtů bloku (včetně flag bytu, ale ten je
     * součástí data[]).
     * Poslední bajt v data[] je samotný checksum.
     * Validace: XOR všech bajtů (včetně checksumu) musí být 0.
     */
    uint8_t xor = 0;
    for ( uint16_t i = 0; i < data_size; i++ ) {
        xor ^= data[i];
    }

    return ( xor == 0 ) ? 0 : -1;
}


/* ========================================================================
 * Bitstream path (přímý)
 * ======================================================================== */

/**
 * @brief Přidá jednu půlvlnu do bitstreamu a invertuje polaritu.
 * @param bitstream Cílový bitstream.
 * @param sample_position Ukazatel na aktuální pozici vzorku (inkrementuje se).
 * @param samples Počet vzorků pro tuto půlvlnu.
 * @param polarity Ukazatel na aktuální polaritu (0/1, po volání se invertuje).
 */
static inline void zxtape_bitstream_add_wave ( st_CMT_BITSTREAM *bitstream, uint32_t *sample_position, int samples, int *polarity ) {
    while ( samples ) {
        cmt_bitstream_set_value_on_position ( bitstream, *sample_position, *polarity );
        *sample_position += 1;
        samples--;
    };
    *polarity = ( ~*polarity ) & 1;
}


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
st_CMT_BITSTREAM* zxtape_create_cmt_bitstream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate ) {

    /* validace vstupů */
    if ( !data && data_size > 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "NULL data pointer with data_size=%u\n", data_size );
        return NULL;
    }

    if ( !cmtspeed_is_valid ( cmtspeed ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Invalid cmtspeed '%d'\n", cmtspeed );
        return NULL;
    }

    if ( rate == 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Invalid sample rate 0\n" );
        return NULL;
    }

    double divisor = g_cmtspeed_divisor[cmtspeed];

    /* délky pulzů ve vzorcích — přepočet ze sekundových konstant */
    uint32_t pilot_pulse_length = round ( g_zxtape_pilot_pulse.high * rate / divisor );
    uint32_t synchigh_pulse_length = round ( g_zxtape_sync_pulse.high * rate / divisor );
    uint32_t synclow_pulse_length = round ( g_zxtape_sync_pulse.low * rate / divisor );
    uint32_t short_pulse_length = round ( g_zxtape_data_pulses.short_pulse.high * rate / divisor );
    uint32_t long_pulse_length = round ( g_zxtape_data_pulses.long_pulse.high * rate / divisor );

    uint32_t count_pilot_pulses = 0;
    uint32_t count_long_pulses = 0;
    uint32_t count_short_pulses = 0;

    unsigned i;
    for ( i = 0; i < data_size; i++ ) {
        uint8_t byte = data[i];
        int j;
        for ( j = 0; j < 8; j++ ) {
            if ( byte & 0x80 ) {
                count_long_pulses++;
            } else {
                count_short_pulses++;
            };
            byte <<= 1;
        };
    };

    int pilot_length = zxtape_get_pilot_length ( flag );
    if ( pilot_length < 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Unknown ZXtape block flag '0x%02x'\n", flag );
        return NULL;
    }
    count_pilot_pulses = (uint32_t) pilot_length;

    uint32_t stream_bitsize = ( count_pilot_pulses * pilot_pulse_length * 2 ) + synchigh_pulse_length + synclow_pulse_length + ( count_long_pulses * long_pulse_length * 2 ) + ( count_short_pulses * short_pulse_length * 2 ) + pilot_pulse_length;

    /*
     * Vytvoreni CMT_STREAM
     */
    uint32_t blocks = cmt_bitstream_compute_required_blocks_from_scans ( stream_bitsize );

    st_CMT_BITSTREAM *bitstream = cmt_bitstream_new ( rate, blocks, CMT_STREAM_POLARITY_NORMAL );
    if ( !bitstream ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Could not create cmt bitstream\n" );
        return NULL;
    };

    uint32_t sample_position = 0;
    int polarity = 0;

    /*
     * Pilot
     */
    for ( i = 0; i < count_pilot_pulses; i++ ) {
        zxtape_bitstream_add_wave ( bitstream, &sample_position, pilot_pulse_length, &polarity );
        zxtape_bitstream_add_wave ( bitstream, &sample_position, pilot_pulse_length, &polarity );
    };

    /*
     * Sync
     */
    zxtape_bitstream_add_wave ( bitstream, &sample_position, synchigh_pulse_length, &polarity );
    zxtape_bitstream_add_wave ( bitstream, &sample_position, synclow_pulse_length, &polarity );

    /*
     * Data
     */
    for ( i = 0; i < data_size; i++ ) {
        uint8_t byte = data[i];
        int j;
        for ( j = 0; j < 8; j++ ) {

            int wave_length;
            if ( byte & 0x80 ) {
                wave_length = long_pulse_length;
            } else {
                wave_length = short_pulse_length;
            };

            zxtape_bitstream_add_wave ( bitstream, &sample_position, wave_length, &polarity );
            zxtape_bitstream_add_wave ( bitstream, &sample_position, wave_length, &polarity );

            byte <<= 1;
        };
    };

    zxtape_bitstream_add_wave ( bitstream, &sample_position, pilot_pulse_length, &polarity );

    return bitstream;
}


/* ========================================================================
 * Vstream path
 * ======================================================================== */

/**
 * @brief Struktura pro počet vzorků jednoho pulzu (high + low půlvlna).
 */
typedef struct st_ZXTAPE_PULSE_SAMPLES {
    uint32_t high; /**< Počet vzorků high půlvlny */
    uint32_t low;  /**< Počet vzorků low půlvlny */
} st_ZXTAPE_PULSE_SAMPLES;


/**
 * @brief Struktura pro počty vzorků long a short pulzu.
 */
typedef struct st_ZXTAPE_PULSES_SAMPLES {
    st_ZXTAPE_PULSE_SAMPLES long_pulse;  /**< Vzorky long pulzu (bit "1") */
    st_ZXTAPE_PULSE_SAMPLES short_pulse; /**< Vzorky short pulzu (bit "0") */
} st_ZXTAPE_PULSES_SAMPLES;


/**
 * @brief Přidá blok opakujících se pulzů stejného typu do vstreamu.
 * @param vstream Cílový vstream.
 * @param gpulse Ukazatel na parametry pulzu (high + low vzorky).
 * @param count Počet pulzů k přidání.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static inline int zxtape_add_cmt_vstream_onestate_block ( st_CMT_VSTREAM* vstream, st_ZXTAPE_PULSE_SAMPLES *gpulse, int count ) {
    int i;
    for ( i = 0; i < count; i++ ) {
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpulse->low ) ) return EXIT_FAILURE;
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 1, gpulse->high ) ) return EXIT_FAILURE;
    };
    return EXIT_SUCCESS;
}


/**
 * @brief Přidá datové bajty jako sekvenci long/short pulzů do vstreamu.
 *
 * Bity se odesílají od MSB k LSB. Bit "1" = long pulz, bit "0" = short pulz.
 *
 * @param cmt_vstream Cílový vstream.
 * @param gpulses Ukazatel na parametry long a short pulzů.
 * @param data Ukazatel na datové bajty.
 * @param size Počet bajtů k zakódování.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static inline int zxtape_add_cmt_vstream_data_block ( st_CMT_VSTREAM* cmt_vstream, st_ZXTAPE_PULSES_SAMPLES *gpulses, uint8_t *data, uint16_t size ) {
    int i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            if ( byte & 0x80 ) {
                if ( EXIT_FAILURE == cmt_vstream_add_value ( cmt_vstream, 0, gpulses->long_pulse.low ) ) return EXIT_FAILURE;
                if ( EXIT_FAILURE == cmt_vstream_add_value ( cmt_vstream, 1, gpulses->long_pulse.high ) ) return EXIT_FAILURE;
            } else {
                if ( EXIT_FAILURE == cmt_vstream_add_value ( cmt_vstream, 0, gpulses->short_pulse.low ) ) return EXIT_FAILURE;
                if ( EXIT_FAILURE == cmt_vstream_add_value ( cmt_vstream, 1, gpulses->short_pulse.high ) ) return EXIT_FAILURE;
            };
            byte = byte << 1;
        };
    };
    return EXIT_SUCCESS;
}


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
st_CMT_VSTREAM* zxtape_create_cmt_vstream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate ) {

    /* validace vstupů */
    if ( !data && data_size > 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "NULL data pointer with data_size=%u\n", data_size );
        return NULL;
    }

    if ( !cmtspeed_is_valid ( cmtspeed ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Invalid cmtspeed '%d'\n", cmtspeed );
        return NULL;
    }

    if ( rate == 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Invalid sample rate 0\n" );
        return NULL;
    }

    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH16, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Could not create cmt vstream\n" );
        return NULL;
    };

    /* konverze sekundových konstant na počet vzorků */
    double divisor = g_cmtspeed_divisor[cmtspeed];

    st_ZXTAPE_PULSE_SAMPLES gpilot;
    gpilot.high = round ( g_zxtape_pilot_pulse.high * rate / divisor );
    gpilot.low = round ( g_zxtape_pilot_pulse.low * rate / divisor );

    st_ZXTAPE_PULSE_SAMPLES gsync;
    gsync.high = round ( g_zxtape_sync_pulse.high * rate / divisor );
    gsync.low = round ( g_zxtape_sync_pulse.low * rate / divisor );

    st_ZXTAPE_PULSES_SAMPLES gpulses;
    gpulses.long_pulse.high = round ( g_zxtape_data_pulses.long_pulse.high * rate / divisor );
    gpulses.long_pulse.low = round ( g_zxtape_data_pulses.long_pulse.low * rate / divisor );
    gpulses.short_pulse.high = round ( g_zxtape_data_pulses.short_pulse.high * rate / divisor );
    gpulses.short_pulse.low = round ( g_zxtape_data_pulses.short_pulse.low * rate / divisor );

    int pilot_length = zxtape_get_pilot_length ( flag );
    if ( pilot_length < 0 ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Unknown ZXtape block flag '0x%02x'\n", flag );
        cmt_vstream_destroy ( vstream );
        return NULL;
    }

    if ( EXIT_FAILURE == zxtape_add_cmt_vstream_onestate_block ( vstream, &gpilot, pilot_length ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Error: can't create cmt vstream\n" );
        cmt_vstream_destroy ( vstream );
        return NULL;
    };

    if ( EXIT_FAILURE == zxtape_add_cmt_vstream_onestate_block ( vstream, &gsync, 1 ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Error: can't create cmt vstream\n" );
        cmt_vstream_destroy ( vstream );
        return NULL;
    };

    if ( EXIT_FAILURE == zxtape_add_cmt_vstream_data_block ( vstream, &gpulses, data, data_size ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Error: can't create cmt vstream\n" );
        cmt_vstream_destroy ( vstream );
        return NULL;
    };

    if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpilot.low ) ) {
        g_zxtape_error_cb ( __func__, __LINE__, "Error: can't create cmt vstream\n" );
        cmt_vstream_destroy ( vstream );
        return NULL;
    };

    return vstream;
}


/* ========================================================================
 * Unified stream wrapper
 * ======================================================================== */

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
st_CMT_STREAM* zxtape_create_stream_from_tapblock ( en_ZXTAPE_BLOCK_FLAG flag, uint8_t *data, uint16_t data_size, en_CMTSPEED cmtspeed, uint32_t rate, en_CMT_STREAM_TYPE type ) {

    st_CMT_STREAM *stream = cmt_stream_new ( type );
    if ( !stream ) {
        return NULL;
    };

    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
        {
#if 0
            /*
             * Přímý bitstream path — ponechán jako kompilační alternativa k budoucímu prověření.
             *
             * Princip bitstream:
             * Při daném sample rate se každý pulz kvantizuje na celý počet vzorků.
             * Zaokrouhlovací chyba se akumuluje pulz po pulzu. Při vyšších rychlostech
             * (větší divisor) se pulzy zkracují → relativní chyba roste → časování
             * se rozjede natolik, že přehrávací rutina přestane pulzy rozpoznávat.
             *
             * Princip vstream:
             * Ukládá přesný počet vzorků pro každý pulz samostatně (RLE kódování).
             * Zaokrouhlení probíhá nezávisle pro každý pulz → chyba se neakumuluje.
             * Konverze vstream → bitstream pak produkuje přesnější výsledek, protože
             * každý pulz má korektně zaokrouhlený počet vzorků.
             *
             * Závěr: přímý bitstream path ponechán jako kompilační alternativa
             * (#if 0/#if 1) k budoucímu prověření a případné opravě
             * (např. kompenzací akumulované chyby).
             */
            st_CMT_BITSTREAM *bitstream = zxtape_create_cmt_bitstream_from_tapblock ( flag, data, data_size, cmtspeed, rate );
            if ( !bitstream ) {
                g_zxtape_error_cb ( __func__, __LINE__, "Can't create bitstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            };
#else
            st_CMT_VSTREAM *vstream = zxtape_create_cmt_vstream_from_tapblock ( flag, data, data_size, cmtspeed, rate );
            if ( !vstream ) {
                g_zxtape_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            };

            st_CMT_BITSTREAM *bitstream = cmt_bitstream_new_from_vstream ( vstream, 0 );
            cmt_vstream_destroy ( vstream );

            if ( !bitstream ) {
                g_zxtape_error_cb ( __func__, __LINE__, "Can't create bitstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            };
#endif
            stream->str.bitstream = bitstream;
            break;
        }

        case CMT_STREAM_TYPE_VSTREAM:
        {
            st_CMT_VSTREAM *vstream = zxtape_create_cmt_vstream_from_tapblock ( flag, data, data_size, cmtspeed, rate );
            if ( !vstream ) {
                g_zxtape_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            };
            stream->str.vstream = vstream;
            break;
        }

        default:
            g_zxtape_error_cb ( __func__, __LINE__, "Unknown stream type '%d'\n", stream->stream_type );
            cmt_stream_destroy ( stream );
            return NULL;
    };

    return stream;
}


const char* zxtape_version ( void ) {
    return ZXTAPE_VERSION;
}
