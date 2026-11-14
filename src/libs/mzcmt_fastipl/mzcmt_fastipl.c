/**
 * @file   mzcmt_fastipl.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.3.0
 * @brief  Implementace FASTIPL koderu pro Sharp MZ.
 *
 * FASTIPL generuje standardni dvou-blokovy MZF zaznam:
 * 1. Header blok: $BB hlavicka pri NORMAL 1:1 rychlosti
 *    LGAP(11000) + LTM(40+40) + 2L + HDR(128B) + CRC + 2L
 * 2. Body blok: datove telo pri turbo rychlosti
 *    LGAP(5500) + STM(20+20) + 2L + BODY(N B) + CRC + 2L
 *
 * Oba bloky pouzivaji standardni NORMAL FM modulaci (1 bit = 1 pulz,
 * MSB first, stop bit za kazdym bajtem). Header blok je vzdy
 * pri rychlosti 1:1, aby ho standard ROM dokazal nacist a spustit
 * loader z komentarove oblasti IPL mechanismem.
 *
 * Struktura odvozena z analyzy Intercopy 10.2 write handleru (sub_19EE):
 * header LGAP = $157C*2 = 11000 pulzu, body LGAP = $157C = 5500 pulzu,
 * body tapemark = STM (20+20), ne LTM (40+40). Mezi bloky neni pauza.
 *
 * Kod loaderu (V02/V07) je prevzat z referencni implementace Intercopy.
 * Loader kopiruje ROM $0000-$0FFF do RAM, modifikuje readpoint na $0A4B
 * a vola ROM rutinu $04F8 pro nacteni datoveho tela.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
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
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/endianity/endianity.h"

#include "mzcmt_fastipl.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

static void* fastipl_default_alloc ( size_t size ) { return malloc ( size ); }
static void* fastipl_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
static void  fastipl_default_free ( void *ptr ) { free ( ptr ); }

static const st_MZCMT_FASTIPL_ALLOCATOR g_default_allocator = {
    fastipl_default_alloc,
    fastipl_default_alloc0,
    fastipl_default_free,
};

static const st_MZCMT_FASTIPL_ALLOCATOR *g_allocator = &g_default_allocator;


void mzcmt_fastipl_set_allocator ( const st_MZCMT_FASTIPL_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_default_allocator;
}


static void fastipl_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

static mzcmt_fastipl_error_cb g_error_cb = fastipl_default_error_cb;


void mzcmt_fastipl_set_error_callback ( mzcmt_fastipl_error_cb cb ) {
    g_error_cb = cb ? cb : fastipl_default_error_cb;
}


/* =========================================================================
 *  FASTIPL loader binaries
 * ========================================================================= */

/**
 * @brief Binarni kod loaderu InterCopy V02 (110 bajtu).
 *
 * Kompletni loader od offsetu $12 v MZF hlavicce ($2139-$21A6).
 * Prvnich 6 bajtu ($12-$17) se interpretuje jako fsize=0, fstrt=$1200,
 * fexec=$1110. Intercopy patchuje offsety $18-$1F parametry.
 * Vstupni bod loaderu: offset $20 (= fexec $1110, kdyz header na $10F0).
 */
static const uint8_t g_loader_v02[MZCMT_FASTIPL_LOADER_SIZE] = {
    /* $12-$17: fsize=0, fstrt=$1200, fexec=$1110 (zaroven loader prologue) */
    0x00, 0x00, 0x00, 0x12, 0x10, 0x11,
    /* $18-$1F: patchovane parametry (vychozi: loader kod) */
    0x42, 0x53, 0x4c, 0x4c, 0x42, 0x42, 0x53, 0x53,
    /* $20-$7F: hlavni telo loaderu (96 bajtu) */
    0x3e, 0x08, 0xd3, 0xce, 0xcd, 0x3e, 0x07, 0x97,
    0x57, 0x5f, 0xcd, 0x08, 0x03, 0xcd, 0xbe, 0x02,
    0xd3, 0xe2, 0x1a, 0xd3, 0xe0, 0x12, 0x13, 0xcb,
    0x62, 0x28, 0xf5, 0x3e, 0xc3, 0x32, 0x1f, 0x06,
    0x21, 0x5c, 0x11, 0x22, 0x20, 0x06, 0x2a, 0x08,
    0x11, 0x7d, 0x32, 0x12, 0x05, 0x7c, 0x32, 0x4b,
    0x0a, 0x2a, 0x0a, 0x11, 0x22, 0x02, 0x11, 0xcd,
    0xf8, 0x04, 0xf5, 0x01, 0xcf, 0x06, 0xed, 0x59,
    0xf1, 0xd3, 0xe2, 0xda, 0xaa, 0xe9, 0x21, 0x0a,
    0x11, 0xc3, 0x08, 0xed, 0xc5, 0x3a, 0x10, 0x11,
    0xee, 0x0c, 0x32, 0x10, 0x11, 0x01, 0xcf, 0x06,
    0xed, 0x79, 0xc1, 0xc9, 0x31, 0x39, 0x38, 0x37,
};


/**
 * @brief Binarni kod loaderu InterCopy V07 (110 bajtu).
 *
 * Pouzivaji verze InterCopy v7, v7.2, v8, v8.2, v10.1, v10.2.
 * Jediny rozdil oproti V02: instrukce LD (HL),$01 na offsetu $27
 * (nastaveni priznaku v pameti).
 */
static const uint8_t g_loader_v07[MZCMT_FASTIPL_LOADER_SIZE] = {
    /* $12-$17: fsize=0, fstrt=$1200, fexec=$1110 */
    0x00, 0x00, 0x00, 0x12, 0x10, 0x11,
    /* $18-$1F: patchovane parametry */
    0x42, 0x53, 0x4c, 0x4c, 0x42, 0x42, 0x53, 0x53,
    /* $20-$7F: hlavni telo loaderu */
    0x3e, 0x08, 0xd3, 0xce, 0xcd, 0x3e, 0x07, 0x36,
    0x01, 0x97, 0x57, 0x5f, 0xcd, 0x08, 0x03, 0xcd,
    0xbe, 0x02, 0xd3, 0xe2, 0x1a, 0xd3, 0xe0, 0x12,
    0x13, 0xcb, 0x62, 0x28, 0xf5, 0x3e, 0xc3, 0x32,
    0x1f, 0x06, 0x21, 0x5c, 0x11, 0x22, 0x20, 0x06,
    0x2a, 0x08, 0x11, 0x7d, 0x32, 0x12, 0x05, 0x7c,
    0x32, 0x4b, 0x0a, 0x2a, 0x0a, 0x11, 0x22, 0x02,
    0x11, 0xcd, 0xf8, 0x04, 0x01, 0xcf, 0x06, 0xed,
    0x71, 0xd3, 0xe2, 0xda, 0xaa, 0xe9, 0x21, 0x0a,
    0x11, 0xc3, 0x08, 0xed, 0xc5, 0x3a, 0x10, 0x11,
    0xee, 0x0c, 0x32, 0x10, 0x11, 0x01, 0xcf, 0x06,
    0xed, 0x79, 0xc1, 0xc9, 0x31, 0x39, 0x38, 0x37,
};


/**
 * @brief Ukazatele na loader binaries indexovane en_MZCMT_FASTIPL_VERSION.
 */
static const uint8_t *g_loaders[MZCMT_FASTIPL_VERSION_COUNT] = {
    g_loader_v02,
    g_loader_v07,
};


/* =========================================================================
 *  Vychozi pulzni konstanty (shodne s mzcmt_turbo)
 * ========================================================================= */

/** @brief Dvojice delek pulzu (high + low) v sekundach. */
typedef struct st_fastipl_pulse_length {
    double high;
    double low;
} st_fastipl_pulse_length;

/** @brief Par delek pro long a short pulz. */
typedef struct st_fastipl_pulses_length {
    st_fastipl_pulse_length long_pulse;
    st_fastipl_pulse_length short_pulse;
} st_fastipl_pulses_length;

/**
 * @brief MZ-700 pulzy.
 *
 * Symetricke hodnoty TMZ v1.0 bez dolozeneho zdroje [neovereno].
 * Shodne s mzcmt_turbo g_pulses_700.
 */
static const st_fastipl_pulses_length g_pulses_700 = {
    { 0.000504, 0.000504 },   /* long: 504 us H + 504 us L */
    { 0.000252, 0.000252 },   /* short: 252 us H + 252 us L */
};

/**
 * @brief MZ-800 pulzy.
 *
 * Asymetricke hodnoty odvozene z nahravky Intercopy z emulatoru (kvantovane
 * na vzorky 44100 Hz: SHORT = 10+12=22 smp, LONG = 21+22=43 smp); nejde
 * o mereni hardwaru. Casovani Intercopy v T-stavech je v profilu IC-10.2-FASTIPL.
 *
 * Asymetricky pulseset je nutny pro spravne skalovani turbo rychlosti:
 * pri 2:1 dava 5+6=11 smp (referencni), symetricke 249/249 dava 5+5=10.
 * Readpoint hodnoty v lookup tabulce g_fastipl_readpoints jsou konzistentni
 * s timto pulsesetem.
 */
static const st_fastipl_pulses_length g_pulses_800 = {
    { 0.000476, 0.000499 },   /* long: 476 us H (21 smp) + 499 us L (22 smp) = 43 smp */
    { 0.000227, 0.000272 },   /* short: 227 us H (10 smp) + 272 us L (12 smp) = 22 smp */
};

/** @brief MZ-80B pulzy. */
static const st_fastipl_pulses_length g_pulses_80B = {
    { 0.000333, 0.000334 },
    { 0.000166750, 0.000166 },
};

static const st_fastipl_pulses_length *g_pulses[] = {
    &g_pulses_700,
    &g_pulses_800,
    &g_pulses_80B,
};


/* =========================================================================
 *  Interni typy a pomocne funkce pro NORMAL FM kodovani
 * ========================================================================= */

/** @brief Pocet vzorku jednoho pulzu. */
typedef struct st_fastipl_pulse_samples {
    uint32_t high;
    uint32_t low;
} st_fastipl_pulse_samples;

/** @brief Pocty vzorku pro long a short pulz. */
typedef struct st_fastipl_pulses_samples {
    st_fastipl_pulse_samples long_pulse;
    st_fastipl_pulse_samples short_pulse;
} st_fastipl_pulses_samples;


/**
 * @brief Pripravi skalovane pulzy z pulsesetu a rychlosti.
 *
 * Pokud jsou explicitni casovani nenulove, pouzije je.
 * Jinak pouzije vychozi z pulsesetu skalovane divisorem.
 *
 * Skalovani: HIGH a LOW se zaokrouhluji zvlast z vychozich
 * pulsesetu skalovanim divisorem. Shodne s mzcmt_turbo.
 *
 * @param[out] pulses Vystupni skalovane pulzy.
 * @param pulseset Pulzni sada.
 * @param speed Rychlost.
 * @param long_high_us100 Explicitni long HIGH (0 = default).
 * @param long_low_us100 Explicitni long LOW (0 = default).
 * @param short_high_us100 Explicitni short HIGH (0 = default).
 * @param short_low_us100 Explicitni short LOW (0 = default).
 * @param rate Cilovy sample rate (Hz).
 */
static void fastipl_prepare_pulses (
    st_fastipl_pulses_samples *pulses,
    en_MZCMT_FASTIPL_PULSESET pulseset,
    en_CMTSPEED speed,
    uint16_t long_high_us100,
    uint16_t long_low_us100,
    uint16_t short_high_us100,
    uint16_t short_low_us100,
    uint32_t rate
) {
    double divisor = cmtspeed_get_divisor ( speed );
    const st_fastipl_pulses_length *src = g_pulses[pulseset];

    if ( long_high_us100 != 0 ) {
        pulses->long_pulse.high = ( uint32_t ) round ( ( long_high_us100 / 10000000.0 ) * rate );
    } else {
        pulses->long_pulse.high = ( uint32_t ) round ( src->long_pulse.high * rate / divisor );
    }

    if ( long_low_us100 != 0 ) {
        pulses->long_pulse.low = ( uint32_t ) round ( ( long_low_us100 / 10000000.0 ) * rate );
    } else {
        pulses->long_pulse.low = ( uint32_t ) round ( src->long_pulse.low * rate / divisor );
    }

    if ( short_high_us100 != 0 ) {
        pulses->short_pulse.high = ( uint32_t ) round ( ( short_high_us100 / 10000000.0 ) * rate );
    } else {
        pulses->short_pulse.high = ( uint32_t ) round ( src->short_pulse.high * rate / divisor );
    }

    if ( short_low_us100 != 0 ) {
        pulses->short_pulse.low = ( uint32_t ) round ( ( short_low_us100 / 10000000.0 ) * rate );
    } else {
        pulses->short_pulse.low = ( uint32_t ) round ( src->short_pulse.low * rate / divisor );
    }

    if ( pulses->long_pulse.high < 1 ) pulses->long_pulse.high = 1;
    if ( pulses->long_pulse.low < 1 ) pulses->long_pulse.low = 1;
    if ( pulses->short_pulse.high < 1 ) pulses->short_pulse.high = 1;
    if ( pulses->short_pulse.low < 1 ) pulses->short_pulse.low = 1;
}


/**
 * @brief Prida blok identickych pulzu do vstreamu.
 */
static int fastipl_add_pulses ( st_CMT_VSTREAM *vstream, const st_fastipl_pulse_samples *pulse, uint32_t count ) {
    uint32_t i;
    for ( i = 0; i < count; i++ ) {
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida tapemark (long_count long + short_count short pulzu).
 */
static int fastipl_add_tapemark ( st_CMT_VSTREAM *vstream, const st_fastipl_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje datovy blok NORMAL FM modulaci (MSB first, stop bit).
 */
static int fastipl_encode_data ( st_CMT_VSTREAM *vstream, const st_fastipl_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            const st_fastipl_pulse_samples *pulse = ( byte & 0x80 )
                ? &pulses->long_pulse : &pulses->short_pulse;
            if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
            if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
            byte <<= 1;
        }
        /* stop bit (long) */
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulses->long_pulse.high ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulses->long_pulse.low ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje 2B checksum (big-endian) do vstreamu.
 */
static int fastipl_encode_checksum ( st_CMT_VSTREAM *vstream, const st_fastipl_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be = endianity_bswap16_BE ( checksum );
    return fastipl_encode_data ( vstream, pulses, ( const uint8_t* ) &be, 2 );
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

/**
 * @brief Spocita NORMAL FM checksum (pocet jednickovych bitu).
 */
uint16_t mzcmt_fastipl_compute_checksum ( const uint8_t *data, uint16_t size ) {
    if ( !data || size == 0 ) return 0;
    uint16_t checksum = 0;
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            if ( byte & 1 ) checksum++;
            byte >>= 1;
        }
    }
    return checksum;
}


/**
 * @brief Readpoint hodnoty pro jednotlive rychlosti.
 *
 * Hodnoty pro 1:1, 2:1, 7:3 a 8:3 jsou z tabulky programu Intercopy V10.2
 * (shodne s dekodovanymi $BB hlavickami). Readpoint ridi ROM delay smycku na $0A4B -
 * musi byt konzistentni s pulznimi sirkami body bloku.
 *
 * Intercopy readpoint NENI jednoduchy vzorec 82/divisor.
 * Pocita se pres sub_201C -> sub_1E09 ze sloziteho timing modelu.
 * Proto pouzivame lookup tabulku.
 */
static const uint8_t g_fastipl_readpoints[CMTSPEED_COUNT] = {
    0,    /* CMTSPEED_NONE */
    77,   /* CMTSPEED_1_1    - 1200 Bd */
    32,   /* CMTSPEED_2_1    - 2400 Bd */
    32,   /* CMTSPEED_2_1_CPM - 2400 Bd (shodne s 2:1) */
    13,   /* CMTSPEED_3_1    - 3600 Bd (odhad: 52507/(3*577)-14) */
    48,   /* CMTSPEED_3_2    - 1800 Bd (odhad: 52507/(1.5*577)-14) */
    22,   /* CMTSPEED_7_3    - 2800 Bd */
    17,   /* CMTSPEED_8_3    - 3200 Bd */
    55,   /* CMTSPEED_9_7    - ~1543 Bd (odhad) */
    40,   /* CMTSPEED_25_14  - ~2143 Bd (odhad) */
    0,    /* CMTSPEED_CUSTOM - vlastní délky pulzů, readpoint z poměru nelze odvodit */
};


/**
 * @brief Vrati vychozi readpoint pro danou rychlost.
 *
 * Pouziva lookup tabulku (Intercopy 10.2 pro 1:1, 2:1, 7:3, 8:3, jinak odhad).
 */
uint8_t mzcmt_fastipl_default_readpoint ( en_CMTSPEED speed ) {
    if ( !cmtspeed_is_valid ( speed ) ) return MZCMT_FASTIPL_READPOINT_DEFAULT;
    return g_fastipl_readpoints[speed];
}


/**
 * @brief Sestavi $BB FASTIPL hlavicku.
 *
 * Vyplni 128B buffer kompletni $BB hlavickou:
 * - ftype=$BB, fname z originalu, fsize=0, fstrt=$1200, fexec=$1110
 * - komentarova oblast: blcount, readpoint, skutecne fsize/fstrt/fexec, loader
 */
void mzcmt_fastipl_build_header (
    uint8_t *out_header,
    const st_MZF_HEADER *original,
    const st_MZCMT_FASTIPL_CONFIG *config
) {
    if ( !out_header || !original || !config ) return;

    /* vynulovat cely header */
    memset ( out_header, 0, sizeof ( st_MZF_HEADER ) );

    /* ftype = $BB */
    out_header[0] = MZCMT_FASTIPL_FTYPE;

    /* fname z originalu (17 bajtu, offset 1-17) */
    memcpy ( &out_header[1], &original->fname, sizeof ( original->fname ) );

    /*
     * Loader binary od offsetu $12 (110 bajtu).
     * Prvnich 6 bajtu ($12-$17) se zaroven interpretuje jako
     * fsize=0, fstrt=$1200, fexec=$1110 pro ROM.
     * Offsety $18-$1F obsahuji vychozi loader kod, ktery se
     * nasledne patchuje parametry (shodne s Intercopy sub_2035).
     */
    en_MZCMT_FASTIPL_VERSION ver = config->version;
    if ( ver >= MZCMT_FASTIPL_VERSION_COUNT ) ver = MZCMT_FASTIPL_VERSION_V07;
    memcpy ( &out_header[MZCMT_FASTIPL_OFF_LOADER], g_loaders[ver], MZCMT_FASTIPL_LOADER_SIZE );

    /* patchovani parametru uvnitr loaderu (shodne s Intercopy sub_2035) */

    /* blcount na offsetu $18 */
    uint8_t blcount = config->blcount > 0 ? config->blcount : 1;
    out_header[MZCMT_FASTIPL_OFF_BLCOUNT] = blcount;

    /* readpoint na offsetu $19 */
    uint8_t readpoint = config->readpoint > 0
        ? config->readpoint
        : mzcmt_fastipl_default_readpoint ( config->speed );
    out_header[MZCMT_FASTIPL_OFF_READPOINT] = readpoint;

    /* skutecne fsize na offsetu $1A (LE) */
    out_header[MZCMT_FASTIPL_OFF_FSIZE]     = ( original->fsize ) & 0xFF;
    out_header[MZCMT_FASTIPL_OFF_FSIZE + 1] = ( original->fsize >> 8 ) & 0xFF;

    /* skutecne fstrt na offsetu $1C (LE) */
    out_header[MZCMT_FASTIPL_OFF_FSTRT]     = ( original->fstrt ) & 0xFF;
    out_header[MZCMT_FASTIPL_OFF_FSTRT + 1] = ( original->fstrt >> 8 ) & 0xFF;

    /* skutecne fexec na offsetu $1E (LE) */
    out_header[MZCMT_FASTIPL_OFF_FEXEC]     = ( original->fexec ) & 0xFF;
    out_header[MZCMT_FASTIPL_OFF_FEXEC + 1] = ( original->fexec >> 8 ) & 0xFF;
}


/**
 * @brief Vytvori CMT vstream z MZF dat FASTIPL kodovanim.
 *
 * Generuje standardni dvou-blokovy MZF zaznam (shodne s Intercopy 10.2):
 *
 * Header blok (NORMAL 1:1 rychlost):
 *   LGAP(11000) + LTM(40L+40S) + 2L + HDR(128B) + CRC + 2L
 *
 * Body blok (turbo rychlost, primo za header blokem bez pauzy):
 *   LGAP(5500) + STM(20L+20S) + 2L + BODY(N B) + CRC + 2L
 *
 * Intercopy sub_19EE pise kazdy blok zvlast:
 * - header: LGAP=$157C*2=11000 pulzu, LTM(40+40)
 * - body: LGAP=$157C=5500 pulzu, STM(20+20)
 * Mezi bloky neni zadna pauza ani SGAP.
 */
st_CMT_VSTREAM* mzcmt_fastipl_create_vstream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_FASTIPL_CONFIG *config,
    uint32_t rate
) {
    /* validace */
    if ( !original ) {
        g_error_cb ( __func__, __LINE__, "Original header is NULL\n" );
        return NULL;
    }

    if ( body_size > 0 && !body ) {
        g_error_cb ( __func__, __LINE__, "Body is NULL but body_size=%u\n", body_size );
        return NULL;
    }

    if ( !config ) {
        g_error_cb ( __func__, __LINE__, "Config is NULL\n" );
        return NULL;
    }

    if ( config->version >= MZCMT_FASTIPL_VERSION_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid loader version %d\n", config->version );
        return NULL;
    }

    if ( config->pulseset >= MZCMT_FASTIPL_PULSESET_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid pulseset %d\n", config->pulseset );
        return NULL;
    }

    if ( !cmtspeed_is_valid ( config->speed ) ) {
        g_error_cb ( __func__, __LINE__, "Invalid speed %d\n", config->speed );
        return NULL;
    }

    if ( rate == 0 ) {
        g_error_cb ( __func__, __LINE__, "Sample rate is 0\n" );
        return NULL;
    }

    /* sestaveni $BB hlavicky */
    uint8_t bb_header[sizeof ( st_MZF_HEADER )];
    mzcmt_fastipl_build_header ( bb_header, original, config );

    /* pulzy pro header blok (1:1 rychlost) */
    st_fastipl_pulses_samples hdr_pulses;
    fastipl_prepare_pulses ( &hdr_pulses, config->pulseset, CMTSPEED_1_1,
                             0, 0, 0, 0, rate );

    /* pulzy pro body blok (turbo rychlost) */
    st_fastipl_pulses_samples body_pulses;
    fastipl_prepare_pulses ( &body_pulses, config->pulseset, config->speed,
                             config->long_high_us100, config->long_low_us100,
                             config->short_high_us100, config->short_low_us100,
                             rate );

    /* checksums */
    uint16_t chk_bb_header = mzcmt_fastipl_compute_checksum ( bb_header, sizeof ( st_MZF_HEADER ) );
    uint16_t chk_body = mzcmt_fastipl_compute_checksum ( body, ( uint16_t ) body_size );

    /* body LGAP delka */
    uint32_t body_lgap = config->lgap_length > 0
                         ? config->lgap_length
                         : MZCMT_FASTIPL_BODY_LGAP_DEFAULT;

    /* signal zacina v HIGH stavu (shodne s mztape) */
    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* ===================================================================
     *  Header blok: $BB hlavicka pri NORMAL 1:1 rychlosti
     * =================================================================== */

    /* LGAP (11000 SHORT pulzu) */
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &hdr_pulses.short_pulse, MZCMT_FASTIPL_HEADER_LGAP_DEFAULT ) ) goto error;

    /* LTM (40 LONG + 40 SHORT) */
    if ( EXIT_SUCCESS != fastipl_add_tapemark ( vstream, &hdr_pulses, MZCMT_FASTIPL_LTM_LONG, MZCMT_FASTIPL_LTM_SHORT ) ) goto error;

    /* 2L sync + hlavicka(128B) + CRC + 2L terminator */
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != fastipl_encode_data ( vstream, &hdr_pulses, bb_header, sizeof ( st_MZF_HEADER ) ) ) goto error;
    if ( EXIT_SUCCESS != fastipl_encode_checksum ( vstream, &hdr_pulses, chk_bb_header ) ) goto error;
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /* ===================================================================
     *  Body blok: datove telo pri turbo rychlosti
     *  (primo za header blokem bez pauzy - shodne s Intercopy)
     * =================================================================== */

    /* LGAP (5500 SHORT pulzu pri turbo rychlosti) */
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &body_pulses.short_pulse, body_lgap ) ) goto error;

    /* STM (20 LONG + 20 SHORT) - Intercopy pise STM pro body, ne LTM */
    if ( EXIT_SUCCESS != fastipl_add_tapemark ( vstream, &body_pulses, MZCMT_FASTIPL_STM_LONG, MZCMT_FASTIPL_STM_SHORT ) ) goto error;

    /* 2L sync + body data + CRC + 2L terminator */
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &body_pulses.long_pulse, 2 ) ) goto error;
    if ( body_size > 0 ) {
        if ( EXIT_SUCCESS != fastipl_encode_data ( vstream, &body_pulses, body, ( uint16_t ) body_size ) ) goto error;
    }
    if ( EXIT_SUCCESS != fastipl_encode_checksum ( vstream, &body_pulses, chk_body ) ) goto error;
    if ( EXIT_SUCCESS != fastipl_add_pulses ( vstream, &body_pulses.long_pulse, 2 ) ) goto error;

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during FASTIPL signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z MZF dat FASTIPL kodovanim.
 *
 * Pro bitstream interno vytvori vstream a konvertuje.
 */
st_CMT_STREAM* mzcmt_fastipl_create_stream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_FASTIPL_CONFIG *config,
    en_CMT_STREAM_TYPE type,
    uint32_t rate
) {
    st_CMT_STREAM *stream = cmt_stream_new ( type );
    if ( !stream ) {
        g_error_cb ( __func__, __LINE__, "Can't create CMT stream\n" );
        return NULL;
    }

    switch ( stream->stream_type ) {
        case CMT_STREAM_TYPE_BITSTREAM:
        {
            st_CMT_VSTREAM *vstream = mzcmt_fastipl_create_vstream ( original, body, body_size, config, rate );
            if ( !vstream ) {
                cmt_stream_destroy ( stream );
                return NULL;
            }

            st_CMT_BITSTREAM *bitstream = cmt_bitstream_new_from_vstream ( vstream, rate );
            cmt_vstream_destroy ( vstream );

            if ( !bitstream ) {
                g_error_cb ( __func__, __LINE__, "Can't convert vstream to bitstream\n" );
                cmt_stream_destroy ( stream );
                return NULL;
            }
            stream->str.bitstream = bitstream;
            break;
        }

        case CMT_STREAM_TYPE_VSTREAM:
        {
            st_CMT_VSTREAM *vstream = mzcmt_fastipl_create_vstream ( original, body, body_size, config, rate );
            if ( !vstream ) {
                cmt_stream_destroy ( stream );
                return NULL;
            }
            stream->str.vstream = vstream;
            break;
        }

        default:
            g_error_cb ( __func__, __LINE__, "Unknown stream type '%d'\n", stream->stream_type );
            cmt_stream_destroy ( stream );
            return NULL;
    }

    return stream;
}


const char* mzcmt_fastipl_version ( void ) {
    return MZCMT_FASTIPL_VERSION;
}

int mzcmt_fastipl_get_pulse_lengths ( en_MZCMT_FASTIPL_PULSESET pulseset, double out[4] ) {
    if ( !out || ( int ) pulseset < 0 || pulseset >= MZCMT_FASTIPL_PULSESET_COUNT ) return EXIT_FAILURE;
    const st_fastipl_pulses_length *p = g_pulses[pulseset];
    out[0] = p->long_pulse.high;
    out[1] = p->long_pulse.low;
    out[2] = p->short_pulse.high;
    out[3] = p->short_pulse.low;
    return EXIT_SUCCESS;
}
