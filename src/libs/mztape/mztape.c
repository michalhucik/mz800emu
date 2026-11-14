/**
 * @file   mztape.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.4.0
 * @brief  Implementace knihovny mztape — generování CMT audio streamů z MZF souborů.
 *
 * Obsahuje logiku konverze MZF dat na CMT bitstream/vstream, výpočet pulzů,
 * definice formátů záznamu a pulzních konstant pro různé modely Sharp MZ.
 *
 * @par Popis standardního MZ-800 CMT záznamu 1200 baudů (údaj servisního manuálu):
 *
 * @verbatim
 *              ___________
 *              |         |
 *      Long:   |         |________     470µs H + 494µs L
 *
 *              ______
 *              |    |
 *      Short:  |    |____              240µs H + 278µs L
 *
 *  Read point: 379µs od náběžné hrany = 6721 GDG T
 * @endverbatim
 *
 * @par Formát záznamu z MZ-800 ROM:
 * - Long GAP: 22 000 short
 * - Long Tape Mark: 40 long + 40 short
 * - 1 Long + HDR (Header1) + CHK (2 bajty)
 * - 2 Long + 256 Short + HDR (Header2) + CHK (2 bajty) + 1 Long
 * - Short GAP: 11 000 short
 * - Short Tape Mark: 20 long + 20 short
 * - 2 Long + FILE (Data1) + CHK (2 bajty)
 * - 1 Long + 256 Short + FILE (Data2) + CHK (2 bajty) + 1 Long
 *
 * @par Datové bajty:
 * - Odesílány od nejvyššího bitu k nejnižšímu
 * - Za každým bajtem následuje 1 stop bit (long)
 * - CHK je odesílán jako big endian (nejprve horní bajt)
 *
 * @par Fyzický záznam:
 * - Log1 (i8255 PC01) má na audio CMT napěťovou úroveň < 0
 * - Log0 (i8255 PC01) má na audio CMT napěťovou úroveň >= 0
 * - MZ-800: změna polarity zadním přepínačem ovlivňuje pouze vstupní data
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní kontrola kódu a refaktorizace. Vytvořeny unit testy.
 * - 2026-11-09: Doplněny vlastní délky pulzů a hlavička CMTSPEED (UniCMT); pulzní
 *   sady převzaty z mz800emu: výchozí sada pro vstream změřená v emulátoru z výstupu
 *   Intercopy (takt GDG), sada cmt.com změřená stejně, údaj servisního manuálu jen
 *   jako záložní sada bitstreamu.
 * - 2026-11-09: kontrola rychlosti v bitstreamu (2.3.1)
 * - 2026-11-10: mztape_get_format() - definice formátové varianty jen pro čtení (2.4.0)
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
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#include "libs/generic_driver/generic_driver.h"
#include "libs/endianity/endianity.h"
#include "libs/cmt_stream/cmt_stream.h"

#include "libs/cmtspeed/cmtspeed.h"
#include "mztape.h"


/** @brief Výchozí alokace paměti (obaluje malloc). */
static void* mztape_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Výchozí alokace s nulováním (obaluje calloc). */
static void* mztape_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Výchozí uvolnění paměti (obaluje free). */
static void  mztape_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Výchozí alokátor využívající standardní knihovní funkce. */
static const st_MZTAPE_ALLOCATOR g_mztape_default_allocator = {
    mztape_default_alloc,
    mztape_default_alloc0,
    mztape_default_free,
};

/** @brief Aktuálně aktivní alokátor (výchozí = stdlib). */
static const st_MZTAPE_ALLOCATOR *g_mztape_allocator = &g_mztape_default_allocator;


/** @brief Nastaví vlastní alokátor, nebo resetuje na výchozí při NULL. */
void mztape_set_allocator ( const st_MZTAPE_ALLOCATOR *allocator ) {
    g_mztape_allocator = allocator ? allocator : &g_mztape_default_allocator;
}


/**
 * @brief Výchozí error callback — vypisuje chyby na stderr.
 * @param func Název volající funkce.
 * @param line Číslo řádku.
 * @param fmt Formátovací řetězec (printf styl).
 */
static void mztape_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktuálně aktivní error callback. */
static mztape_error_cb g_mztape_error_cb = mztape_default_error_cb;


/** @brief Nastaví vlastní error callback, nebo resetuje na výchozí při NULL. */
void mztape_set_error_callback ( mztape_error_cb cb ) {
    g_mztape_error_cb = cb ? cb : mztape_default_error_cb;
}


/** @brief Pole podporovaných rychlostí záznamu, zakončené CMTSPEED_NONE. */
const en_CMTSPEED g_mztape_speed[] = {
                                      CMTSPEED_1_1, // 1200 Bd
                                      CMTSPEED_2_1, // 2400 Bd
                                      CMTSPEED_2_1_CPM, // 2400 Bd
                                      CMTSPEED_7_3, // 2800 Bd
                                      CMTSPEED_8_3, // 3200 Bd
                                      CMTSPEED_3_1, // 3600 Bd
                                      CMTSPEED_NONE
};

/** @brief Sekvence bloků pro SANE formát (bez kopií header/data). */
en_MZTAPE_BLOCK g_mztape_format_sharp_sane[] = {
                                                MZTAPE_BLOCK_LGAP,
                                                MZTAPE_BLOCK_LTM,
                                                MZTAPE_BLOCK_2L,
                                                MZTAPE_BLOCK_HDR,
                                                MZTAPE_BLOCK_CHKH,
                                                MZTAPE_BLOCK_2L,
                                                MZTAPE_BLOCK_SGAP,
                                                MZTAPE_BLOCK_STM,
                                                MZTAPE_BLOCK_2L,
                                                MZTAPE_BLOCK_FILE,
                                                MZTAPE_BLOCK_CHKF,
                                                MZTAPE_BLOCK_2L,
                                                MZTAPE_BLOCK_STOP
};

/** @brief Sekvence bloků pro plný Sharp formát (s kopiemi header/data). */
en_MZTAPE_BLOCK g_mztape_format_sharp[] = {
                                           MZTAPE_BLOCK_LGAP,
                                           MZTAPE_BLOCK_LTM,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_HDR,
                                           MZTAPE_BLOCK_CHKH,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_256S,
                                           MZTAPE_BLOCK_HDRC,
                                           MZTAPE_BLOCK_CHKH,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_SGAP,
                                           MZTAPE_BLOCK_STM,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_FILE,
                                           MZTAPE_BLOCK_CHKF,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_256S,
                                           MZTAPE_BLOCK_FILEC,
                                           MZTAPE_BLOCK_CHKF,
                                           MZTAPE_BLOCK_2L,
                                           MZTAPE_BLOCK_STOP
};

/**
 * @brief Pulzní konstanty pro MZ-700, MZ-80K, MZ-80A.
 *
 * LONG 464/494 µs, SHORT 240/264 µs (HIGH/LOW). Původ hodnot není
 * doložen [neověřeno]. Žádný formát mztape tuto sadu nehraje; čte ji
 * mztape_get_pulse_lengths().
 */
const st_MZTAPE_PULSES_LENGTH g_mztape_pulses_700 = {
    { 0.000464, 0.000494, 0.000958 }, // LONG PULSE
    { 0.000240, 0.000264, 0.000504 }, // SHORT PULSE
};

/**
 * @brief Pulzní konstanty pro MZ-800, MZ-1500 (bitstream path).
 *
 * Údaj servisního manuálu: LONG 470/494 µs, SHORT 240/278 µs (HIGH/LOW).
 * ROM MZ-800 zapisuje jiné délky, závislé na kontextu (změřeno v emulátoru,
 * profil ROM-800-EXACT), a jiné programy zase vlastní;
 * čtení 1:1 je tolerantní. Vstream path proto hraje sadu
 * g_mztape_pulses_800_intercopy; tato sada zůstává jen jako záložní sada
 * bitstream path a jako výstup mztape_get_pulse_lengths().
 */
const st_MZTAPE_PULSES_LENGTH g_mztape_pulses_800 = {
    { 0.000470, 0.000494, 0.000964 }, // LONG PULSE
    { 0.000240, 0.000278, 0.000518 }, // SHORT PULSE
};

/**
 * @brief Pulzní konstanty pro MZ-800 - výchozí sada vstream path.
 *
 * Změřeno v emulátoru z výstupu Intercopy 10.2 (takt GDG 17 721 600 Hz).
 * Ticky GDG (HIGH, LOW): LONG {8335, 8760}, SHORT {4356, 4930}.
 */
static const st_MZTAPE_PULSES_LENGTH g_mztape_pulses_800_intercopy = {
    { 0.000470330, 0.000494308, 0.000964638 }, /* LONG */
    { 0.000245802, 0.000278204, 0.000524006 }, /* SHORT */
};

/**
 * @brief Pulzní konstanty pro MZ-800 - CP/M cmt.com (vstream path, rychlost 2:1 CPM).
 *
 * Změřeno v emulátoru z výstupu cmt.com (takt GDG 17 721 600 Hz).
 * Ticky GDG (HIGH, LOW): LONG {9300, 8660}, SHORT {5400, 4660}.
 */
static const st_MZTAPE_PULSES_LENGTH g_mztape_pulses_800_cmtcom = {
    { 0.000524796, 0.000488665, 0.001013461 }, /* LONG */
    { 0.000304762, 0.000262935, 0.000567697 }, /* SHORT */
};

/** @brief Pulzní konstanty pro MZ-80B. */
const st_MZTAPE_PULSES_LENGTH g_mztape_pulses_80B = {
    { 0.000333, 0.000334, 0.000667 }, // LONG PULSE
    { 0.000166750, 0.000166, 0.000332750 }, // SHORT PULSE
};


/** @brief Pole ukazatelů na pulzní konstanty — indexováno podle en_MZTAPE_PULSESET. */
const st_MZTAPE_PULSES_LENGTH *g_pulses_length[] = {
                                                    &g_mztape_pulses_700,
                                                    &g_mztape_pulses_800,
                                                    &g_mztape_pulses_80B,
};


/** @brief Definice SANE formátu MZ-800 (bez kopií, kratší GAP). */
const st_MZTAPE_FORMAT g_format_mz800_sane = {
                                              MZTAPE_LGAP_LENGTH_SANE,
                                              MZTAPE_SGAP_LENGTH_SANE,
                                              MZTAPE_PULSESET_800,
                                              g_mztape_format_sharp_sane,
};

/** @brief Definice plného formátu MZ-800 (s kopiemi header/data). */
const st_MZTAPE_FORMAT g_format_mz800 = {
                                         MZTAPE_LGAP_LENGTH_DEFAULT,
                                         MZTAPE_SGAP_LENGTH,
                                         MZTAPE_PULSESET_800,
                                         g_mztape_format_sharp,
};

/** @brief Definice formátu MZ-80B (1800 Bd, GAP 10000, bez kopií). */
const st_MZTAPE_FORMAT g_format_mz80b = {
                                         MZTAPE_LGAP_LENGTH_MZ80B,
                                         MZTAPE_SGAP_LENGTH,
                                         MZTAPE_PULSESET_80B,
                                         g_mztape_format_sharp_sane,
};

/** @brief Pole formátů — indexováno podle en_MZTAPE_FORMATSET (pořadí zachovat!). */
const st_MZTAPE_FORMAT *g_formats[] = {
                                       &g_format_mz800_sane,
                                       &g_format_mz800,
                                       &g_format_mz80b,
};


/**
 * @brief Spočítá checksum bloku dat jako 32bit počet jedničkových bitů.
 * @param block Ukazatel na data bloku.
 * @param size Velikost bloku v bajtech.
 * @return Počet jedničkových bitů v celém bloku.
 */
static uint32_t mztape_compute_block_checksum_32t ( uint8_t *block, uint16_t size ) {
    uint32_t checksum = 0;
    while ( size-- ) {
        uint8_t byte = *block;
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            if ( byte & 1 ) checksum++;
            byte >>= 1;
        };
        block++;
    };
    return checksum;
}


/**
 * @brief Spočítá celkový počet long a short pulzů pro daný MZF a formát.
 * @param mztmzf MZF data.
 * @param mztape_format Formátová varianta záznamu.
 * @param[out] long_pulses Výstup — celkový počet long pulzů.
 * @param[out] short_pulses Výstup — celkový počet short pulzů.
 */
void mztape_compute_pulses ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, uint64_t *long_pulses, uint64_t *short_pulses ) {

    uint64_t total_long = 0;
    uint64_t total_short = 0;

    if ( mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT ) {
        g_mztape_error_cb ( __func__, __LINE__, "Invalid format index %d\n", mztape_format );
        *long_pulses = 0;
        *short_pulses = 0;
        return;
    }

    const en_MZTAPE_BLOCK *format = g_formats[mztape_format]->blocks;

    int i = 0;
    while ( format[i] != MZTAPE_BLOCK_STOP ) {

        switch ( format[i] ) {
            case MZTAPE_BLOCK_LGAP:
                total_short += g_formats[mztape_format]->lgap;
                break;

            case MZTAPE_BLOCK_SGAP:
                total_short += g_formats[mztape_format]->sgap;
                break;

            case MZTAPE_BLOCK_LTM:
                total_long += MZTAPE_LTM_LLENGTH;
                total_short += MZTAPE_LTM_SLENGTH;
                break;

            case MZTAPE_BLOCK_STM:
                total_long += MZTAPE_STM_LLENGTH;
                total_short += MZTAPE_STM_SLENGTH;
                break;

            case MZTAPE_BLOCK_2L:
                total_long += 2;
                break;

            case MZTAPE_BLOCK_256S:
                total_short += 256;
                break;

            case MZTAPE_BLOCK_HDR:
                total_long += mztmzf->chkh + sizeof ( st_MZF_HEADER ); // + stop bity
                total_short += ( sizeof ( st_MZF_HEADER ) * 8 ) - mztmzf->chkh;
                break;

            case MZTAPE_BLOCK_FILE:
                total_long += mztmzf->chkb + mztmzf->size; // + stop bity
                total_short += ( mztmzf->size * 8 ) - mztmzf->chkb;
                break;

            case MZTAPE_BLOCK_CHKH:
            {
                uint16_t chk = mztmzf->chkh;
                uint32_t chklong = mztape_compute_block_checksum_32t ( ( uint8_t* ) & chk, 2 );
                total_long += chklong + 2; // + stop bity
                total_short += 16 - chklong;
                break;
            }

            case MZTAPE_BLOCK_CHKF:
            {
                uint16_t chk = mztmzf->chkb;
                uint32_t chklong = mztape_compute_block_checksum_32t ( ( uint8_t* ) & chk, 2 );
                total_long += chklong + 2; // + stop bity
                total_short += 16 - chklong;
                break;
            }

            default:
                g_mztape_error_cb ( __func__, __LINE__, "Error: unknown block id=%d\n", format[i] );
        };
        i++;
    };

    *long_pulses = total_long;
    *short_pulses = total_short;
}


/**
 * @brief Připraví délky pulzů s ohledem na pulzní sadu a rychlost záznamu.
 *
 * Vydělí referenční délky pulzů rychlostním divisorem.
 *
 * @param[out] pulses Výstupní struktura s připravenými délkami.
 * @param pulseset Pulzní sada pro daný model.
 * @param mztape_speed Rychlost záznamu.
 */
static void mztape_prepare_pulses_length ( st_MZTAPE_PULSES_LENGTH *pulses, en_MZTAPE_PULSESET pulseset, en_CMTSPEED mztape_speed ) {
    if ( pulseset < 0 || pulseset >= MZTAPE_PULSESET_COUNT ) return;
    if ( !cmtspeed_is_valid ( mztape_speed ) ) return;
    const st_MZTAPE_PULSES_LENGTH *src = g_pulses_length[pulseset];

    pulses->long_pulse.high = src->long_pulse.high / g_cmtspeed_divisor[mztape_speed];
    pulses->long_pulse.low = src->long_pulse.low / g_cmtspeed_divisor[mztape_speed];
    //pulses->long_pulse.low = pulses->long_pulse.high;
    pulses->long_pulse.total = pulses->long_pulse.high + pulses->long_pulse.low;

    pulses->short_pulse.high = src->short_pulse.high / g_cmtspeed_divisor[mztape_speed];
    pulses->short_pulse.low = src->short_pulse.low / g_cmtspeed_divisor[mztape_speed];
    //pulses->short_pulse.low = pulses->short_pulse.high;
    pulses->short_pulse.total = pulses->short_pulse.high + pulses->short_pulse.low;
}


/**
 * @brief Skenuje jednostavový blok (GAP, 2L, 256S) pro bitstream generování.
 *
 * Vrací ukazatel na pulz pro aktuální pozici, nebo NULL pokud blok skončil.
 *
 * @param[in,out] bit_counter Čítač bitů v aktuálním bloku.
 * @param block_size Celkový počet pulzů v bloku.
 * @param block_pulse Ukazatel na délku pulzu pro tento blok.
 * @param[out] flag_next_format_phase Nastaví na 1 při dosažení konce bloku.
 * @return Ukazatel na pulz, nebo NULL.
 */
static inline st_MZTAPE_PULSE_LENGTH* mztape_scan_onestate_block ( uint32_t *bit_counter, uint32_t block_size, st_MZTAPE_PULSE_LENGTH *block_pulse, int *flag_next_format_phase ) {

    st_MZTAPE_PULSE_LENGTH *pulse = NULL;

    if ( *bit_counter < block_size ) {
        pulse = block_pulse;
    } else {
        *flag_next_format_phase = 1;
    };

    *bit_counter += 1;
    return pulse;
}


/**
 * @brief Skenuje dvoustavový blok (tapemark: long + short) pro bitstream generování.
 *
 * Nejprve vrací long pulzy, po jejich vyčerpání short pulzy.
 *
 * @param[in,out] bit_counter Čítač bitů v aktuálním bloku.
 * @param block_lsize Počet long pulzů na začátku bloku.
 * @param block_ssize Počet short pulzů za long pulzy.
 * @param block_pulses Ukazatel na dvojici délek pulzů.
 * @param[out] flag_next_format_phase Nastaví na 1 při dosažení konce bloku.
 * @return Ukazatel na pulz, nebo NULL.
 */
static inline st_MZTAPE_PULSE_LENGTH* mztape_scan_twostate_block ( uint32_t *bit_counter, uint32_t block_lsize, uint32_t block_ssize, st_MZTAPE_PULSES_LENGTH *block_pulses, int *flag_next_format_phase ) {

    st_MZTAPE_PULSE_LENGTH *pulse = NULL;

    if ( *bit_counter < block_lsize ) {
        pulse = &block_pulses->long_pulse;
    } else if ( *bit_counter < ( block_lsize + block_ssize ) ) {
        pulse = &block_pulses->short_pulse;
    } else {
        *flag_next_format_phase = 1;
    };

    *bit_counter += 1;
    return pulse;
}


/**
 * @brief Skenuje datový blok (HDR, FILE, CHK) pro bitstream generování.
 *
 * Každý bajt se odesílá od MSB k LSB, za každým bajtem stop bit (long).
 *
 * @param[in,out] bit_counter Čítač bitů v aktuálním bloku.
 * @param block_size Celkový počet bitů + stop bitů v bloku.
 * @param block_pulses Ukazatel na dvojici délek pulzů.
 * @param data Ukazatel na data bloku.
 * @param[out] flag_next_format_phase Nastaví na 1 při dosažení konce bloku.
 * @return Ukazatel na pulz, nebo NULL.
 */
static inline st_MZTAPE_PULSE_LENGTH* mztape_scan_data_block ( uint32_t *bit_counter, uint32_t block_size, st_MZTAPE_PULSES_LENGTH *block_pulses, uint8_t *data, int *flag_next_format_phase ) {

    st_MZTAPE_PULSE_LENGTH *pulse = NULL;

    if ( *bit_counter < block_size ) {

        uint32_t byte_pos = *bit_counter / 9;
        int bit = *bit_counter % 9;

        if ( bit < 8 ) {
            uint8_t byte = data[byte_pos] << bit;

            if ( byte & 0x80 ) {
                pulse = &block_pulses->long_pulse;
            } else {
                pulse = &block_pulses->short_pulse;
            };
        } else {
            // stop bit
            pulse = &block_pulses->long_pulse;
        }

    } else {
        *flag_next_format_phase = 1;
    };

    *bit_counter += 1;
    return pulse;
}


/**
 * @brief Uvolní strukturu st_MZTAPE_MZF včetně alokovaného těla.
 * @param mztmzf Ukazatel na strukturu k uvolnění (NULL je bezpečné).
 */
void mztape_mztmzf_destroy ( st_MZTAPE_MZF *mztmzf ) {
    if ( !mztmzf ) return;
    if ( mztmzf->body ) g_mztape_allocator->free ( mztmzf->body );
    g_mztape_allocator->free ( mztmzf );
}


/**
 * @brief Vytvoří st_MZTAPE_MZF načtením MZF hlavičky a těla z handleru.
 *
 * Alokuje strukturu, načte hlavičku a tělo, spočítá checksums.
 *
 * @param mzf_handler Handler otevřeného MZF souboru.
 * @param offset Offset v handleru, odkud začíná MZF hlavička.
 * @return Ukazatel na novou strukturu, nebo NULL při chybě.
 */
st_MZTAPE_MZF* mztape_create_mztapemzf ( st_HANDLER *mzf_handler, uint32_t offset ) {

    st_HANDLER *h = mzf_handler;

    st_MZTAPE_MZF *mztmzf = g_mztape_allocator->alloc0 ( sizeof ( st_MZTAPE_MZF ) );

    if ( !mztmzf ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not create mztape mzf\n" );
        return NULL;
    };

    st_MZF_HEADER mzfhdr;

    if ( EXIT_SUCCESS != mzf_read_header_on_offset ( h, offset, &mzfhdr ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not read MZF header\n" );
        mztape_mztmzf_destroy ( mztmzf );
        return NULL;
    };

    mztmzf->size = mzfhdr.fsize;

    if ( EXIT_SUCCESS != generic_driver_read ( h, offset, mztmzf->header, sizeof ( st_MZF_HEADER ) ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not read MZF header\n" );
        mztape_mztmzf_destroy ( mztmzf );
        return NULL;
    };

    mztmzf->body = g_mztape_allocator->alloc ( mztmzf->size );

    if ( EXIT_SUCCESS != generic_driver_read ( h, offset + sizeof ( st_MZF_HEADER ), mztmzf->body, mzfhdr.fsize ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not read MZF body\n" );
        mztape_mztmzf_destroy ( mztmzf );
        return NULL;
    };

    mztmzf->chkh = mztape_compute_block_checksum_32t ( mztmzf->header, sizeof ( st_MZF_HEADER ) );
    mztmzf->chkb = mztape_compute_block_checksum_32t ( mztmzf->body, mztmzf->size );

    return mztmzf;
}


/**
 * @brief Vytvoří CMT bitstream přímou generací vzorků z MZF dat.
 *
 * Méně přesný path — zaokrouhlovací chyba se akumuluje pulz po pulzu.
 * Při vyšších rychlostech (3600 Bd) se časování rozjede natolik, že ROM
 * rutina přestane pulzy rozpoznávat. Pro spolehlivý výsledek použít
 * vstream path (mztape_create_cmt_vstream_from_mztmzf).
 *
 * @param mztmzf MZF data.
 * @param mztape_format Formátová varianta záznamu.
 * @param mztape_speed Rychlost záznamu (platný poměr, cmtspeed_is_valid()).
 * @param sample_rate Vzorkovací frekvence výstupního bitstreamu (Hz).
 * @return Ukazatel na nový bitstream (vlastník je volající), nebo NULL při
 *         chybě. Chybou je i neplatný formát a neplatná rychlost
 *         (CMTSPEED_NONE, CMTSPEED_CUSTOM).
 */
st_CMT_BITSTREAM* mztape_create_cmt_bitstream_from_mztmzf ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, uint32_t sample_rate ) {

    /* CMTSPEED_CUSTOM a neplatná rychlost nemají násobitel, délky pulzů by zůstaly neinicializované */
    if ( mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT || !cmtspeed_is_valid ( mztape_speed ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Invalid format %d or speed %d\n", mztape_format, mztape_speed );
        return NULL;
    };

    double sample_length = (double) 1 / sample_rate;

    /*
     * Zjisteni poctu pulzu a priprava jejich delky
     */

    uint64_t long_pulses;
    uint64_t short_pulses;

    mztape_compute_pulses ( mztmzf, mztape_format, &long_pulses, &short_pulses );

    st_MZTAPE_PULSES_LENGTH pulses;

    mztape_prepare_pulses_length ( &pulses, g_formats[mztape_format]->pulseset, mztape_speed );

    double stream_length = ( long_pulses * pulses.long_pulse.total ) + ( short_pulses * pulses.short_pulse.total );

    uint32_t data_bitsize = stream_length / sample_length;

    /*
     * Vytvoreni CMT_STREAM
     */
    uint32_t blocks = cmt_bitstream_compute_required_blocks_from_scans ( data_bitsize );

    st_CMT_BITSTREAM *bitstream = cmt_bitstream_new ( sample_rate, blocks, CMT_STREAM_POLARITY_NORMAL );
    if ( !bitstream ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not create cmt bitstream\n" );
        return NULL;
    };

    uint32_t sample_position = 0;
    uint32_t bit_counter = 0;
    double scan_time = 0;
    en_MZTAPE_BLOCK *format = g_formats[mztape_format]->blocks;
    int format_phase = 0;
    double pulse_time = 0;

    st_MZTAPE_PULSE_LENGTH *pulse = NULL;

    while ( scan_time <= stream_length ) {

        if ( ( pulse == NULL ) || ( pulse_time >= pulse->total ) ) {

            if ( pulse != NULL ) {
                pulse_time -= pulse->total;
            };

            int flag_skip_this_pulse = 0;

            do {

                int flag_next_format_phase;

                do {

                    flag_next_format_phase = 0;

                    switch ( format[format_phase] ) {

                        case MZTAPE_BLOCK_LGAP:
                            pulse = mztape_scan_onestate_block ( &bit_counter, g_formats[mztape_format]->lgap, &pulses.short_pulse, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_SGAP:
                            pulse = mztape_scan_onestate_block ( &bit_counter, g_formats[mztape_format]->sgap, &pulses.short_pulse, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_LTM:
                            pulse = mztape_scan_twostate_block ( &bit_counter, MZTAPE_LTM_LLENGTH, MZTAPE_LTM_SLENGTH, &pulses, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_STM:
                            pulse = mztape_scan_twostate_block ( &bit_counter, MZTAPE_STM_LLENGTH, MZTAPE_STM_SLENGTH, &pulses, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_2L:
                            pulse = mztape_scan_onestate_block ( &bit_counter, 2, &pulses.long_pulse, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_256S:
                            pulse = mztape_scan_onestate_block ( &bit_counter, 256, &pulses.short_pulse, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_HDR:
                        case MZTAPE_BLOCK_HDRC:
                            pulse = mztape_scan_data_block ( &bit_counter, ( sizeof ( st_MZF_HEADER ) * 8 ) + sizeof ( st_MZF_HEADER ), &pulses, mztmzf->header, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_FILE:
                        case MZTAPE_BLOCK_FILEC:
                            pulse = mztape_scan_data_block ( &bit_counter, ( mztmzf->size * 8 ) + mztmzf->size, &pulses, mztmzf->body, &flag_next_format_phase );
                            break;

                        case MZTAPE_BLOCK_CHKH:
                        {
                            uint16_t chk = endianity_bswap16_BE ( mztmzf->chkh );
                            pulse = mztape_scan_data_block ( &bit_counter, 16 + 2, &pulses, ( uint8_t* ) & chk, &flag_next_format_phase );
                            break;
                        }

                        case MZTAPE_BLOCK_CHKF:
                        {
                            uint16_t chk = endianity_bswap16_BE ( mztmzf->chkb );
                            pulse = mztape_scan_data_block ( &bit_counter, 16 + 2, &pulses, ( uint8_t* ) & chk, &flag_next_format_phase );
                            break;
                        }

                        case MZTAPE_BLOCK_STOP:
                        default:
                            pulse = NULL;
                            break;
                    };

                    if ( flag_next_format_phase != 0 ) {
                        pulse = NULL;
                        bit_counter = 0;
                        format_phase++;
                    };

                } while ( flag_next_format_phase != 0 );


                if ( pulse != NULL ) {
                    if ( pulse_time >= pulse->total ) {
                        pulse_time -= pulse->total;
                        flag_skip_this_pulse = 1;
                    }
                } else {
                    flag_skip_this_pulse = 0;
                }

            } while ( flag_skip_this_pulse != 0 );

        };

        if ( pulse != NULL ) {
            int sample_value = ( pulse_time < pulse->high ) ? 1 : 0;
            cmt_bitstream_set_value_on_position ( bitstream, sample_position++, sample_value );
            pulse_time += sample_length;
        };

        scan_time += sample_length;
    }

    return bitstream;
}


/** @brief Počet vzorků jednoho pulzu (high + low část) pro vstream generování. */
typedef struct st_MZTAPE_PULSE_SAMPLES {
    uint32_t high; /**< počet vzorků HIGH části */
    uint32_t low;  /**< počet vzorků LOW části */
} st_MZTAPE_PULSE_SAMPLES;


/** @brief Počty vzorků pro long a short pulz (vstream generování). */
typedef struct st_MZTAPE_PULSES_SAMPLES {
    st_MZTAPE_PULSE_SAMPLES long_pulse;  /**< vzorky long pulzu */
    st_MZTAPE_PULSE_SAMPLES short_pulse; /**< vzorky short pulzu */
} st_MZTAPE_PULSES_SAMPLES;


/**
 * @brief Přidá jednostavový blok pulzů do vstreamu (GAP, tapemark části, 2L, 256S).
 * @param vstream Cílový vstream.
 * @param gpulse Ukazatel na vzorky pulzu (high + low).
 * @param count Počet pulzů k přidání.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static inline int mztape_add_cmt_vstream_onestate_block ( st_CMT_VSTREAM* vstream, st_MZTAPE_PULSE_SAMPLES *gpulse, int count ) {
    int i;
    for ( i = 0; i < count; i++ ) {
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 1, gpulse->high ) ) return EXIT_FAILURE;
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpulse->low ) ) return EXIT_FAILURE;
    };
    return EXIT_SUCCESS;
}


/**
 * @brief Přidá datový blok do vstreamu (bajty od MSB, za každým bajtem stop bit).
 * @param vstream Cílový vstream.
 * @param gpulses Ukazatel na vzorky long/short pulzu.
 * @param data Ukazatel na data bloku.
 * @param size Velikost dat v bajtech.
 * @return EXIT_SUCCESS při úspěchu, EXIT_FAILURE při chybě.
 */
static inline int mztape_add_cmt_vstream_data_block ( st_CMT_VSTREAM* vstream, st_MZTAPE_PULSES_SAMPLES *gpulses, uint8_t *data, uint16_t size ) {
    int i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            if ( byte & 0x80 ) {
                if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 1, gpulses->long_pulse.high ) ) return EXIT_FAILURE;
                if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpulses->long_pulse.low ) ) return EXIT_FAILURE;
            } else {
                if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 1, gpulses->short_pulse.high ) ) return EXIT_FAILURE;
                if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpulses->short_pulse.low ) ) return EXIT_FAILURE;
            };
            byte = byte << 1;
        };
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 1, gpulses->long_pulse.high ) ) return EXIT_FAILURE;
        if ( EXIT_FAILURE == cmt_vstream_add_value ( vstream, 0, gpulses->long_pulse.low ) ) return EXIT_FAILURE;
    };
    return EXIT_SUCCESS;
}


/**
 * @brief Vybere referenční délky pulzů (1:1) pro pulzní sadu formátu a režim rychlosti.
 *
 * Pro MZ-800 je to sada změřená v emulátoru z výstupu Intercopy, pro 2:1
 * CP/M sada cmt.com (jiný tvar pulzu, ne jen jiný poměr).
 *
 * @param mztape_format Formátová varianta záznamu (platná, ověřuje volající).
 * @param mztape_speed Rychlost záznamu.
 * @return Ukazatel na statické konstanty (nikdy NULL).
 */
static const st_MZTAPE_PULSES_LENGTH* mztape_get_src_pulses ( en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed ) {
    switch ( g_formats[mztape_format]->pulseset ) {
        case MZTAPE_PULSESET_800:
            return ( mztape_speed == CMTSPEED_2_1_CPM )
                ? &g_mztape_pulses_800_cmtcom : &g_mztape_pulses_800_intercopy;
        case MZTAPE_PULSESET_700:
            return &g_mztape_pulses_700;
        case MZTAPE_PULSESET_80B:
            return &g_mztape_pulses_80B;
        default:
            return &g_mztape_pulses_800_intercopy;
    }
}


/**
 * @brief Vytvoří vstream záznamu z již spočítaných počtů vzorků pulzů.
 *
 * Společné jádro pro rychlost danou poměrem i pro vlastní délky pulzů:
 * projde bloky formátu (GAP, tapemark, hlavička, tělo, checksum) a přidá
 * je do vstreamu.
 *
 * @param mztmzf MZF data.
 * @param mztape_format Formátová varianta záznamu (platná, ověřuje volající).
 * @param gpulses Počty vzorků high/low částí dlouhého a krátkého pulzu
 *                (všechny nenulové, jinak cmt_vstream_add_value() selže).
 * @param rate Vzorkovací frekvence výstupního vstreamu (Hz).
 * @return Ukazatel na nový vstream (vlastník je volající), nebo NULL při chybě.
 */
static st_CMT_VSTREAM* mztape_create_cmt_vstream_from_samples ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, st_MZTAPE_PULSES_SAMPLES *gpulses, uint32_t rate ) {

    st_CMT_VSTREAM* vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_mztape_error_cb ( __func__, __LINE__, "Could not create cmt vstream\n" );
        return NULL;
    };

    const en_MZTAPE_BLOCK *format = g_formats[mztape_format]->blocks;

    int i = 0;
    int ret;
    while ( format[i] != MZTAPE_BLOCK_STOP ) {

        switch ( format[i] ) {
            case MZTAPE_BLOCK_LGAP:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->short_pulse, g_formats[mztape_format]->lgap );
                break;

            case MZTAPE_BLOCK_SGAP:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->short_pulse, g_formats[mztape_format]->sgap );
                break;

            case MZTAPE_BLOCK_LTM:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->long_pulse, MZTAPE_LTM_LLENGTH );
                if ( ret != EXIT_FAILURE ) {
                    ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->short_pulse, MZTAPE_LTM_SLENGTH );
                };
                break;

            case MZTAPE_BLOCK_STM:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->long_pulse, MZTAPE_STM_LLENGTH );
                if ( ret != EXIT_FAILURE ) {
                    ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->short_pulse, MZTAPE_STM_SLENGTH );
                };
                break;

            case MZTAPE_BLOCK_2L:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->long_pulse, 2 );
                break;

            case MZTAPE_BLOCK_256S:
                ret = mztape_add_cmt_vstream_onestate_block ( vstream, &gpulses->short_pulse, 256 );
                break;

            case MZTAPE_BLOCK_HDR:
            case MZTAPE_BLOCK_HDRC:
                ret = mztape_add_cmt_vstream_data_block ( vstream, gpulses, mztmzf->header, sizeof ( st_MZF_HEADER ) );
                break;

            case MZTAPE_BLOCK_FILE:
            case MZTAPE_BLOCK_FILEC:
                ret = mztape_add_cmt_vstream_data_block ( vstream, gpulses, mztmzf->body, mztmzf->size );
                break;

            case MZTAPE_BLOCK_CHKH:
            {
                uint16_t chk = endianity_bswap16_BE ( mztmzf->chkh );
                ret = mztape_add_cmt_vstream_data_block ( vstream, gpulses, ( uint8_t* ) & chk, 2 );
                break;
            }

            case MZTAPE_BLOCK_CHKF:
            {
                uint16_t chk = endianity_bswap16_BE ( mztmzf->chkb );
                ret = mztape_add_cmt_vstream_data_block ( vstream, gpulses, ( uint8_t* ) & chk, 2 );
                break;
            }

            default:
                g_mztape_error_cb ( __func__, __LINE__, "Error: unknown block id=%d\n", format[i] );
                ret = EXIT_FAILURE;
        };

        if ( ret == EXIT_FAILURE ) {
            g_mztape_error_cb ( __func__, __LINE__, "Error: can't create cmt vstream\n" );
            cmt_vstream_destroy ( vstream );
            return NULL;
        };

        i++;
    };

    return vstream;
}


/**
 * @brief Vytvoří CMT vstream z MZF dat (RLE kódování pulzů).
 *
 * Přesnější path - zaokrouhlení probíhá nezávisle pro každý pulz, takže
 * se chyba neakumuluje. Doporučený způsob generování CMT streamu.
 *
 * Počet vzorků se počítá jako round(délka 1:1 * rate / poměr); tento
 * výpočet se při zavedení vlastních délek pulzů záměrně nezměnil, aby
 * záznam v poměru zůstal bit po bitu stejný.
 *
 * @param mztmzf MZF data.
 * @param mztape_format Formátová varianta záznamu.
 * @param mztape_speed Rychlost záznamu (platný poměr, cmtspeed_is_valid()).
 * @param rate Vzorkovací frekvence výstupního vstreamu (Hz).
 * @return Ukazatel na nový vstream (vlastník je volající), nebo NULL při
 *         chybě. Chybou je i neplatný formát a neplatná rychlost
 *         (CMTSPEED_NONE, CMTSPEED_CUSTOM).
 */
st_CMT_VSTREAM* mztape_create_cmt_vstream_from_mztmzf ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, uint32_t rate ) {

    /* CMTSPEED_CUSTOM a neplatná rychlost nemají násobitel, dělení by bylo nulou */
    if ( mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT || !cmtspeed_is_valid ( mztape_speed ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Invalid format %d or speed %d\n", mztape_format, mztape_speed );
        return NULL;
    };

    /* výběr sekundových konstant podle pulsesetu formátu a režimu rychlosti */
    const st_MZTAPE_PULSES_LENGTH *srcpulses = mztape_get_src_pulses ( mztape_format, mztape_speed );

    /* konverze sekund → počet vzorků */
    double divisor = g_cmtspeed_divisor[mztape_speed];
    st_MZTAPE_PULSES_SAMPLES gpulses;
    gpulses.long_pulse.high = round ( srcpulses->long_pulse.high * rate / divisor );
    gpulses.long_pulse.low = round ( srcpulses->long_pulse.low * rate / divisor );
    gpulses.short_pulse.high = round ( srcpulses->short_pulse.high * rate / divisor );
    gpulses.short_pulse.low = round ( srcpulses->short_pulse.low * rate / divisor );

    return mztape_create_cmt_vstream_from_samples ( mztmzf, mztape_format, &gpulses, rate );
}


/*
 * Vlastní délky pulzů: každá část pulzu se zaokrouhlí na celé vzorky samostatně.
 * Kontrakt viz mztape.h.
 */
st_CMT_VSTREAM* mztape_create_cmt_vstream_from_mztmzf_pulses ( st_MZTAPE_MZF *mztmzf, en_MZTAPE_FORMATSET mztape_format, const st_MZTAPE_PULSES_LENGTH *pulses, uint32_t rate ) {

    if ( mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT || !pulses ) {
        g_mztape_error_cb ( __func__, __LINE__, "Invalid format %d or pulses\n", mztape_format );
        return NULL;
    };

    st_MZTAPE_PULSES_SAMPLES gpulses;
    gpulses.long_pulse.high = round ( pulses->long_pulse.high * rate );
    gpulses.long_pulse.low = round ( pulses->long_pulse.low * rate );
    gpulses.short_pulse.high = round ( pulses->short_pulse.high * rate );
    gpulses.short_pulse.low = round ( pulses->short_pulse.low * rate );

    /* část pulzu kratší než půl vzorku by ze záznamu zmizela */
    if ( !gpulses.long_pulse.high || !gpulses.long_pulse.low || !gpulses.short_pulse.high || !gpulses.short_pulse.low ) {
        g_mztape_error_cb ( __func__, __LINE__, "Pulse shorter than one sample at %u Hz\n", rate );
        return NULL;
    };

    return mztape_create_cmt_vstream_from_samples ( mztmzf, mztape_format, &gpulses, rate );
}


/**
 * @brief Zabalí hotový vstream do CMT streamu požadovaného typu.
 *
 * Pro vstream ho přímo převezme, pro bitstream ho převede (každá část
 * pulzu je už zaokrouhlená na celé vzorky) a vstream uvolní.
 *
 * @param vstream Vstream (funkce přebírá vlastnictví; NULL = chyba volajícího).
 * @param type Typ výstupního streamu.
 * @param rate Vzorkovací frekvence (Hz).
 * @return Nový stream (vlastník je volající), nebo NULL při chybě (vstream
 *         je pak uvolněný).
 */
static st_CMT_STREAM* mztape_wrap_vstream ( st_CMT_VSTREAM *vstream, en_CMT_STREAM_TYPE type, uint32_t rate ) {

    if ( !vstream ) {
        g_mztape_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    };

    st_CMT_STREAM *stream = cmt_stream_new ( type );
    if ( !stream ) {
        cmt_vstream_destroy ( vstream );
        return NULL;
    };

    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
        {
            st_CMT_BITSTREAM *bitstream = cmt_bitstream_new_from_vstream ( vstream, rate );
            cmt_vstream_destroy ( vstream );

            if ( !bitstream ) {
                g_mztape_error_cb ( __func__, __LINE__, "Can't create bitstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            };
            stream->str.bitstream = bitstream;
            break;
        }

        case CMT_STREAM_TYPE_VSTREAM:
            stream->str.vstream = vstream;
            break;

        default:
            g_mztape_error_cb ( __func__, __LINE__, "Unknown stream type '%d'\n", stream->stream_type );
            cmt_vstream_destroy ( vstream );
            cmt_stream_destroy ( stream );
            return NULL;
    };

    return stream;
}


/**
 * @brief Jednotné API pro vytvoření CMT streamu (bitstream nebo vstream).
 *
 * Pro bitstream interně vytváří vstream a konvertuje (přesnější výsledek).
 *
 * @param mztmzf MZF data.
 * @param cmtspeed Rychlost záznamu.
 * @param type Typ výstupního streamu (bitstream/vstream).
 * @param mztape_fset Formátová varianta záznamu.
 * @param rate Vzorkovací frekvence (Hz).
 * @return Ukazatel na nový stream (vlastník je volající), nebo NULL při chybě.
 */
st_CMT_STREAM* mztape_create_stream_from_mztapemzf ( st_MZTAPE_MZF *mztmzf, en_CMTSPEED cmtspeed, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate ) {

#if 0
    /*
     * Dvě cesty k bitstreamu, obě kvantizují hrany na mřížku sample_rate:
     *
     * Přímý bitstream (#if 0, mztape_create_cmt_bitstream_from_mztmzf):
     * ideální průběh pulzů se navzorkuje po vzorcích; zbytek času se
     * přenáší do dalšího pulzu (pulse_time -= pulse->total), takže poloha
     * hran se neposouvá, ale každá hrana padne na nejbližší další vzorek
     * a šířky jednotlivých pulzů kolísají o 1 vzorek.
     *
     * Přes vstream (#else, výchozí): každý půlpulz se zaokrouhlí na celý
     * počet vzorků samostatně (round); šířky jsou stálé, ale zkreslené.
     * Při 3:1 a 44,1 kHz z krátkého pulzu 81,9/92,7 us vznikne
     * 90,7/90,7 us (dlouhý 156,8/164,8 -> 158,7/158,7 us).
     *
     * Ani jedna cesta není přesná. Zavaděče s pevným okamžikem vzorkování
     * bitu (Interkarate, díly 2-3 při 3:1) na 44,1 kHz načítají
     * nespolehlivě. Emulátor proto MZF přehrává jako vstream s frekvencí
     * taktu GDG; bitstream zůstává pro WAV.
     */
    if ( type == CMT_STREAM_TYPE_BITSTREAM ) {
        st_CMT_STREAM *stream = cmt_stream_new ( type );
        if ( !stream ) {
            return NULL;
        };
        st_CMT_BITSTREAM *bitstream = mztape_create_cmt_bitstream_from_mztmzf ( mztmzf, mztape_fset, cmtspeed, rate );
        if ( !bitstream ) {
            g_mztape_error_cb ( __func__, __LINE__, "Can't create bitstream\n" );
            cmt_stream_destroy ( stream );
            return NULL;
        };
        stream->str.bitstream = bitstream;
        return stream;
    };
#endif

    return mztape_wrap_vstream ( mztape_create_cmt_vstream_from_mztmzf ( mztmzf, mztape_fset, cmtspeed, rate ), type, rate );
}


/* Kontrakt viz mztape.h. */
st_CMT_STREAM* mztape_create_stream_from_mztapemzf_pulses ( st_MZTAPE_MZF *mztmzf, const st_MZTAPE_PULSES_LENGTH *pulses, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate ) {
    return mztape_wrap_vstream ( mztape_create_cmt_vstream_from_mztmzf_pulses ( mztmzf, mztape_fset, pulses, rate ), type, rate );
}


/**
 * @brief Rozšířená verze s volitelným přepisem délek pulzů.
 *
 * Všechny čtyři hodnoty nulové = mztape_create_stream_from_mztapemzf().
 * Jinak se hodnoty v jednotkách 0,1 µs převedou na µs (hodnota / 10)
 * a záznam vznikne přes mztape_create_stream_from_mztapemzf_pulses();
 * počet vzorků každé části pulzu je round(hodnota / 1e7 * rate).
 *
 * @param mztmzf MZF data.
 * @param cmtspeed Rychlost záznamu (ignorována, pokud některá délka != 0).
 * @param type Typ výstupního streamu (bitstream/vstream).
 * @param mztape_fset Formátová varianta záznamu.
 * @param rate Vzorkovací frekvence (Hz).
 * @param long_high_us100  Délka HIGH části dlouhého pulzu (0,1 µs, 0 = výchozí).
 * @param long_low_us100   Délka LOW části dlouhého pulzu (0,1 µs, 0 = výchozí).
 * @param short_high_us100 Délka HIGH části krátkého pulzu (0,1 µs, 0 = výchozí).
 * @param short_low_us100  Délka LOW části krátkého pulzu (0,1 µs, 0 = výchozí).
 * @return Ukazatel na nový stream (vlastník je volající), nebo NULL při
 *         chybě (mimo jiné částečně nulové délky nebo část pulzu kratší
 *         než půl vzorku).
 */
st_CMT_STREAM* mztape_create_stream_from_mztapemzf_ex ( st_MZTAPE_MZF *mztmzf, en_CMTSPEED cmtspeed, en_CMT_STREAM_TYPE type, en_MZTAPE_FORMATSET mztape_fset, uint32_t rate,
                                                          uint16_t long_high_us100, uint16_t long_low_us100,
                                                          uint16_t short_high_us100, uint16_t short_low_us100 ) {

    /* všechny vlastní délky nulové = délky z pulzní sady a rychlosti */
    if ( long_high_us100 == 0 && long_low_us100 == 0 &&
         short_high_us100 == 0 && short_low_us100 == 0 ) {
        return mztape_create_stream_from_mztapemzf (
            mztmzf, cmtspeed, type, mztape_fset, rate );
    }

    /* 0,1 µs -> µs; nulová část (částečně vyplněné délky) je chyba */
    st_MZTAPE_PULSES_LENGTH pulses;
    if ( EXIT_SUCCESS != mztape_pulses_set_us ( &pulses, long_high_us100 / 10.0, long_low_us100 / 10.0,
                                                short_high_us100 / 10.0, short_low_us100 / 10.0 ) ) {
        g_mztape_error_cb ( __func__, __LINE__, "Invalid custom pulses %u/%u/%u/%u\n",
                            long_high_us100, long_low_us100, short_high_us100, short_low_us100 );
        return NULL;
    };

    return mztape_create_stream_from_mztapemzf_pulses ( mztmzf, &pulses, type, mztape_fset, rate );
}


/* Kontrakt viz mztape.h. */
int mztape_pulses_set_us ( st_MZTAPE_PULSES_LENGTH *pulses, double long_high_us, double long_low_us, double short_high_us, double short_low_us ) {
    const double v[4] = { long_high_us, long_low_us, short_high_us, short_low_us };
    int i;
    for ( i = 0; i < 4; i++ ) {
        /* !(v > 0) zachytí i NaN */
        if ( !( v[i] > 0 ) || ( v[i] > MZTAPE_PULSE_US_MAX ) ) return EXIT_FAILURE;
    };
    pulses->long_pulse.high = long_high_us / 1000000.0;
    pulses->long_pulse.low = long_low_us / 1000000.0;
    pulses->long_pulse.total = pulses->long_pulse.high + pulses->long_pulse.low;
    pulses->short_pulse.high = short_high_us / 1000000.0;
    pulses->short_pulse.low = short_low_us / 1000000.0;
    pulses->short_pulse.total = pulses->short_pulse.high + pulses->short_pulse.low;
    return EXIT_SUCCESS;
}


/* Kontrakt viz mztape.h. */
int mztape_get_speed_pulses ( en_MZTAPE_FORMATSET mztape_format, en_CMTSPEED mztape_speed, st_MZTAPE_PULSES_LENGTH *pulses ) {
    if ( mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT || !cmtspeed_is_valid ( mztape_speed ) ) return EXIT_FAILURE;
    const st_MZTAPE_PULSES_LENGTH *src = mztape_get_src_pulses ( mztape_format, mztape_speed );
    double divisor = g_cmtspeed_divisor[mztape_speed];
    pulses->long_pulse.high = src->long_pulse.high / divisor;
    pulses->long_pulse.low = src->long_pulse.low / divisor;
    pulses->long_pulse.total = pulses->long_pulse.high + pulses->long_pulse.low;
    pulses->short_pulse.high = src->short_pulse.high / divisor;
    pulses->short_pulse.low = src->short_pulse.low / divisor;
    pulses->short_pulse.total = pulses->short_pulse.high + pulses->short_pulse.low;
    return EXIT_SUCCESS;
}


/* Kontrakt viz mztape.h. */
double mztape_pulses_get_ratio ( en_MZTAPE_FORMATSET mztape_format, const st_MZTAPE_PULSES_LENGTH *pulses ) {
    st_MZTAPE_PULSES_LENGTH nominal;
    if ( !pulses || EXIT_SUCCESS != mztape_get_speed_pulses ( mztape_format, CMTSPEED_1_1, &nominal ) ) return 0;
    double custom = pulses->long_pulse.high + pulses->long_pulse.low + pulses->short_pulse.high + pulses->short_pulse.low;
    if ( !( custom > 0 ) ) return 0;
    return ( nominal.long_pulse.total + nominal.short_pulse.total ) / custom;
}


/* Kontrakt viz mztape.h. */
en_MZTAPE_UNICMT_MARKER mztape_unicmt_speed_marker ( const uint8_t *header, st_MZTAPE_PULSES_LENGTH *pulses ) {
    static const char name[] = "CMTSPEED\r";
    /* typ 00h, jméno (offset 01h), délka těla 0 (offset 12h, LE) */
    if ( header[0x00] != 0x00 ) return MZTAPE_UNICMT_MARKER_NONE;
    if ( memcmp ( &header[0x01], name, sizeof ( name ) - 1 ) != 0 ) return MZTAPE_UNICMT_MARKER_NONE;
    if ( header[0x12] != 0x00 || header[0x13] != 0x00 ) return MZTAPE_UNICMT_MARKER_NONE;

    const uint8_t *p = &header[MZTAPE_UNICMT_PULSES_OFFSET];
    uint16_t us[4];
    int i;
    for ( i = 0; i < 4; i++ ) {
        us[i] = (uint16_t) ( p[i * 2] | ( p[i * 2 + 1] << 8 ) );
    };

    st_MZTAPE_PULSES_LENGTH tmp;
    if ( EXIT_SUCCESS != mztape_pulses_set_us ( &tmp, us[0], us[1], us[2], us[3] ) ) return MZTAPE_UNICMT_MARKER_INVALID;
    if ( pulses ) *pulses = tmp;
    return MZTAPE_UNICMT_MARKER_VALID;
}


const char* mztape_version ( void ) {
    return MZTAPE_VERSION;
}


int mztape_get_pulse_lengths ( en_MZTAPE_PULSESET pulseset, double out[4] ) {
    if ( !out || ( int ) pulseset < 0 || pulseset >= MZTAPE_PULSESET_COUNT ) return EXIT_FAILURE;
    const st_MZTAPE_PULSES_LENGTH *p = g_pulses_length[pulseset];
    out[0] = p->long_pulse.high;
    out[1] = p->long_pulse.low;
    out[2] = p->short_pulse.high;
    out[3] = p->short_pulse.low;
    return EXIT_SUCCESS;
}


/* Kontrakt viz mztape.h. */
const st_MZTAPE_FORMAT* mztape_get_format ( en_MZTAPE_FORMATSET mztape_format ) {
    if ( ( int ) mztape_format < 0 || mztape_format >= MZTAPE_FORMATSET_COUNT ) return NULL;
    return g_formats[mztape_format];
}
