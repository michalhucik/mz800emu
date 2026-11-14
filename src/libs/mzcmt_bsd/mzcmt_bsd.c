/**
 * @file   mzcmt_bsd.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.1
 * @brief  Implementace BSD/BRD kodéru TMZ v1.0 (jen pro TAPEMZ-V1-LEGACY, viz mzcmt_bsd.h).
 *
 * BSD/BRD je chunkovaný datový soubor BASICu (BASIC 1Z-016 WOPEN#/PRINT#).
 * Používá NORMAL FM modulaci (1 bit na pulz, MSB first, start
 * bit LONG před každým bajtem). Struktura v1.0 (tón jen před prvním kusem)
 * BASIC za prvním kusem nepřečte; player používá profil
 * BASIC-1Z016.
 *
 * Hlavicka se koduje stejne jako u standardniho MZF zaznamu
 * (LGAP + LTM + 2L + HDR + CHKH + 2L). Telo je rozdeleno do N
 * chunku, kazdy chunk (258 B = 2B ID + 256B data) je na pasce
 * ulozen jako samostatny body blok s kratkym tapemarkem
 * (STM + 2L + CHUNK + CHK + 2L).
 *
 * Pulzni casovani se odvozuje z pulsesetu (MZ-700/800/80B) a
 * rychlostniho divisoru (typicky 1:1 pro standardni rychlost).
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
#include <stdarg.h>
#include <math.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/endianity/endianity.h"

#include "mzcmt_bsd.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* mzcmt_bsd_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* mzcmt_bsd_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  mzcmt_bsd_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_MZCMT_BSD_ALLOCATOR g_mzcmt_bsd_default_allocator = {
    mzcmt_bsd_default_alloc,
    mzcmt_bsd_default_alloc0,
    mzcmt_bsd_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_MZCMT_BSD_ALLOCATOR *g_allocator = &g_mzcmt_bsd_default_allocator;


/** @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL. */
void mzcmt_bsd_set_allocator ( const st_MZCMT_BSD_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_mzcmt_bsd_default_allocator;
}


/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void mzcmt_bsd_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static mzcmt_bsd_error_cb g_error_cb = mzcmt_bsd_default_error_cb;


/** @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL. */
void mzcmt_bsd_set_error_callback ( mzcmt_bsd_error_cb cb ) {
    g_error_cb = cb ? cb : mzcmt_bsd_default_error_cb;
}


/* =========================================================================
 *  Vychozi pulzni konstanty (v sekundach)
 * ========================================================================= */

/**
 * @brief Dvojice delek pulzu (high + low cast) v sekundach.
 */
typedef struct st_bsd_pulse_length {
    double high;  /**< doba HIGH casti pulzu v sekundach */
    double low;   /**< doba LOW casti pulzu v sekundach */
} st_bsd_pulse_length;


/**
 * @brief Par delek pro long a short pulz.
 */
typedef struct st_bsd_pulses_length {
    st_bsd_pulse_length long_pulse;  /**< pulz pro bit "1" */
    st_bsd_pulse_length short_pulse; /**< pulz pro bit "0" */
} st_bsd_pulses_length;


/**
 * @brief Vychozi pulzni konstanty pro MZ-700, MZ-80K, MZ-80A.
 *
 * Symetrické hodnoty v1.0 bez doloženého zdroje - shodné
 * s mzcmt_turbo g_pulses_700.
 */
static const st_bsd_pulses_length g_pulses_700 = {
    { 0.000504, 0.000504 },   /* long: 504 us H + 504 us L */
    { 0.000252, 0.000252 },   /* short: 252 us H + 252 us L */
};


/**
 * @brief Vychozi pulzni konstanty pro MZ-800, MZ-1500.
 *
 * Symetrické hodnoty v1.0 - shodné s mzcmt_turbo g_pulses_800. ROM
 * MZ-800 symetrické pulzy nezapisuje (ROM SAVE 844/920 a 1664/1750 T,
 * změřeno v emulátoru).
 */
static const st_bsd_pulses_length g_pulses_800 = {
    { 0.000498, 0.000498 },   /* long: 498 us H + 498 us L */
    { 0.000249, 0.000249 },   /* short: 249 us H + 249 us L */
};


/**
 * @brief Vychozi pulzni konstanty pro MZ-80B.
 *
 * Prevzato z mztape.c (g_mztape_pulses_80B).
 */
static const st_bsd_pulses_length g_pulses_80B = {
    { 0.000333, 0.000334 },        /* long: 333 us H + 334 us L */
    { 0.000166750, 0.000166 },      /* short: 167 us H + 166 us L */
};


/**
 * @brief Pole ukazatelu na pulzni konstanty - indexovano en_MZCMT_BSD_PULSESET.
 */
static const st_bsd_pulses_length *g_pulses[] = {
    &g_pulses_700,
    &g_pulses_800,
    &g_pulses_80B,
};


/* =========================================================================
 *  Interni typy pro skalovane pulzy (pocty vzorku)
 * ========================================================================= */

/**
 * @brief Pocet vzorku jednoho pulzu (high + low cast).
 */
typedef struct st_bsd_pulse_samples {
    uint32_t high; /**< pocet vzorku HIGH casti */
    uint32_t low;  /**< pocet vzorku LOW casti */
} st_bsd_pulse_samples;


/**
 * @brief Skalovane pocty vzorku pro long a short pulz.
 */
typedef struct st_bsd_pulses_samples {
    st_bsd_pulse_samples long_pulse;  /**< vzorky long pulzu */
    st_bsd_pulse_samples short_pulse; /**< vzorky short pulzu */
} st_bsd_pulses_samples;


/**
 * @brief Pripravi skalovane pulzni sirky z konfigurace.
 *
 * Vychozi pulzni konstanty z pulsesetu se skali rychlostnim divisorem
 * a prevadi na pocty vzorku pri danem sample ratu.
 *
 * @param[out] pulses Vystupni struktura se skalovanymi pulzy.
 * @param config Konfigurace koderu.
 * @param rate Cilovy sample rate (Hz).
 */
static void bsd_prepare_pulses ( st_bsd_pulses_samples *pulses, const st_MZCMT_BSD_CONFIG *config, uint32_t rate ) {

    double divisor = cmtspeed_get_divisor ( config->speed );
    const st_bsd_pulses_length *src = g_pulses[config->pulseset];

    pulses->long_pulse.high = ( uint32_t ) round ( src->long_pulse.high * rate / divisor );
    pulses->long_pulse.low  = ( uint32_t ) round ( src->long_pulse.low  * rate / divisor );
    pulses->short_pulse.high = ( uint32_t ) round ( src->short_pulse.high * rate / divisor );
    pulses->short_pulse.low  = ( uint32_t ) round ( src->short_pulse.low  * rate / divisor );

    /* minimalne 1 vzorek na kazdy segment */
    if ( pulses->long_pulse.high < 1 ) pulses->long_pulse.high = 1;
    if ( pulses->long_pulse.low < 1 ) pulses->long_pulse.low = 1;
    if ( pulses->short_pulse.high < 1 ) pulses->short_pulse.high = 1;
    if ( pulses->short_pulse.low < 1 ) pulses->short_pulse.low = 1;
}


/* =========================================================================
 *  Interni pomocne funkce pro generovani signalu
 * ========================================================================= */

/**
 * @brief Prida blok jednostavovych pulzu do vstreamu.
 *
 * Generuje sekvenci identickych pulzu (napr. GAP - same short,
 * nebo tapemark long cast - same long).
 *
 * @param vstream Cilovy vstream.
 * @param pulse Ukazatel na vzorky pulzu.
 * @param count Pocet pulzu k pridani.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int bsd_add_pulses ( st_CMT_VSTREAM *vstream, const st_bsd_pulse_samples *pulse, uint32_t count ) {
    uint32_t i;
    for ( i = 0; i < count; i++ ) {
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida tapemark sekvenci do vstreamu.
 *
 * Tapemark se sklada z long_count long pulzu nasledovanych
 * short_count short pulzy.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param long_count Pocet long pulzu.
 * @param short_count Pocet short pulzu.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int bsd_add_tapemark ( st_CMT_VSTREAM *vstream, const st_bsd_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje datovy blok do vstreamu NORMAL FM modulaci.
 *
 * Kazdy bajt se odesila od MSB k LSB (8 bitu). Za kazdym bajtem
 * nasleduje 1 stop bit (long pulz). Bit "1" = long pulz,
 * bit "0" = short pulz.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param data Ukazatel na data k zakodovani.
 * @param size Velikost dat v bajtech.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int bsd_encode_data ( st_CMT_VSTREAM *vstream, const st_bsd_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        /* 8 datovych bitu, MSB first */
        for ( bit = 0; bit < 8; bit++ ) {
            const st_bsd_pulse_samples *pulse = ( byte & 0x80 )
                ? &pulses->long_pulse : &pulses->short_pulse;
            if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
            if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
            byte <<= 1;
        }
        /* stop bit (vzdy long) */
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulses->long_pulse.high ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulses->long_pulse.low ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje 2B checksum do vstreamu.
 *
 * Checksum (pocet jednickovych bitu) se odesila big-endian
 * (horni bajt prvni), se stop bitem za kazdym bajtem.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param checksum 16bitovy checksum k zakodovani.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int bsd_encode_checksum ( st_CMT_VSTREAM *vstream, const st_bsd_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be_checksum = endianity_bswap16_BE ( checksum );
    return bsd_encode_data ( vstream, pulses, ( const uint8_t* ) &be_checksum, 2 );
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

/**
 * @brief Spocita NORMAL FM checksum datoveho bloku.
 *
 * Suma jednickovych bitu pres vsechny bajty bloku. Shodny s ROM
 * algoritmem pouzitym pri nacitani z pasky.
 *
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return Pocet jednickovych bitu (0 pro NULL nebo size==0).
 */
uint16_t mzcmt_bsd_compute_checksum ( const uint8_t *data, uint16_t size ) {
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
 * @brief Vytvori CMT vstream z BSD/BRD dat chunkovym kodovanim.
 *
 * Generuje kompletni paskovy ramec pro BSD/BRD zaznam:
 * 1. LGAP (kratke pulzy, leader ton)
 * 2. Dlouhy tapemark (40 long + 40 short)
 * 3. 2 long + hlavicka (128 B) + checksum hlavicky + 2 long
 * 4. [kopie hlavicky: 256 short + hlavicka + checksum + 2 long]
 * 5. SGAP (kratke pulzy)
 * 6. Pro kazdy chunk:
 *    a. Kratky tapemark (20 long + 20 short)
 *    b. 2 long + chunk (258 B) + checksum chunku + 2 long
 *
 * Signal zacina v HIGH stavu. Pulzni casovani se bere z konfigurace
 * (vychozi z pulsesetu + rychlosti).
 *
 * @param header      128B MZF hlavicka v originalni endianite.
 * @param chunks_data Chunkova data (chunk_count * 258 B).
 * @param chunk_count Pocet chunku (musi byt >= 1).
 * @param config      Konfigurace koderu.
 * @param rate        Vzorkovaci frekvence (Hz).
 * @return Novy vstream, nebo NULL pri chybe.
 */
st_CMT_VSTREAM* mzcmt_bsd_create_vstream (
    const uint8_t *header,
    const uint8_t *chunks_data,
    uint16_t chunk_count,
    const st_MZCMT_BSD_CONFIG *config,
    uint32_t rate
) {
    /* validace vstupu */
    if ( !header ) {
        g_error_cb ( __func__, __LINE__, "Header is NULL\n" );
        return NULL;
    }

    if ( chunk_count == 0 ) {
        g_error_cb ( __func__, __LINE__, "Chunk count is 0\n" );
        return NULL;
    }

    if ( !chunks_data ) {
        g_error_cb ( __func__, __LINE__, "Chunks data is NULL\n" );
        return NULL;
    }

    if ( !config ) {
        g_error_cb ( __func__, __LINE__, "Config is NULL\n" );
        return NULL;
    }

    if ( config->pulseset >= MZCMT_BSD_PULSESET_COUNT ) {
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

    /* priprava skalovanych pulzu */
    st_bsd_pulses_samples pulses;
    bsd_prepare_pulses ( &pulses, config, rate );

    /* efektivni GAP delky */
    uint32_t lgap = config->lgap_length > 0 ? config->lgap_length : MZCMT_BSD_LGAP_DEFAULT;
    uint32_t sgap = config->sgap_length > 0 ? config->sgap_length : MZCMT_BSD_SGAP_DEFAULT;

    /* checksum hlavicky (pocet jednickovych bitu) */
    uint16_t chk_header = mzcmt_bsd_compute_checksum ( header, sizeof ( st_MZF_HEADER ) );

    /* signal zacina v HIGH stavu (shodne s mztape) */
    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* === 1. LGAP (kratke pulzy) === */
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.short_pulse, lgap ) ) goto error;

    /* === 2. Dlouhy tapemark (40 long + 40 short) === */
    if ( EXIT_SUCCESS != bsd_add_tapemark ( vstream, &pulses, MZCMT_BSD_LTM_LONG, MZCMT_BSD_LTM_SHORT ) ) goto error;

    /* === 3. 2 long + hlavicka + checksum + 2 long === */
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != bsd_encode_data ( vstream, &pulses, header, sizeof ( st_MZF_HEADER ) ) ) goto error;
    if ( EXIT_SUCCESS != bsd_encode_checksum ( vstream, &pulses, chk_header ) ) goto error;
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;

    /* === 4. Kopie hlavicky (pokud flag) === */
    if ( config->flags & MZCMT_BSD_FLAG_HEADER_COPY ) {
        if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.short_pulse, MZCMT_BSD_COPY_SEP ) ) goto error;
        if ( EXIT_SUCCESS != bsd_encode_data ( vstream, &pulses, header, sizeof ( st_MZF_HEADER ) ) ) goto error;
        if ( EXIT_SUCCESS != bsd_encode_checksum ( vstream, &pulses, chk_header ) ) goto error;
        if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    }

    /* === 5. SGAP (kratke pulzy) === */
    if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.short_pulse, sgap ) ) goto error;

    /* === 6. Chunky - kazdy chunk jako samostatny body blok === */
    {
        uint16_t i;
        for ( i = 0; i < chunk_count; i++ ) {

            const uint8_t *chunk = chunks_data + ( uint32_t ) i * MZCMT_BSD_CHUNK_SIZE;
            uint16_t chk_chunk = mzcmt_bsd_compute_checksum ( chunk, MZCMT_BSD_CHUNK_SIZE );

            /* kratky tapemark (20 long + 20 short) */
            if ( EXIT_SUCCESS != bsd_add_tapemark ( vstream, &pulses, MZCMT_BSD_STM_LONG, MZCMT_BSD_STM_SHORT ) ) goto error;

            /* 2 long + chunk data (258 B) + checksum + 2 long */
            if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
            if ( EXIT_SUCCESS != bsd_encode_data ( vstream, &pulses, chunk, MZCMT_BSD_CHUNK_SIZE ) ) goto error;
            if ( EXIT_SUCCESS != bsd_encode_checksum ( vstream, &pulses, chk_chunk ) ) goto error;
            if ( EXIT_SUCCESS != bsd_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
        }
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during BSD signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z BSD/BRD dat chunkovym kodovanim.
 *
 * Pro typ CMT_STREAM_TYPE_VSTREAM primo pouzije mzcmt_bsd_create_vstream().
 * Pro typ CMT_STREAM_TYPE_BITSTREAM vytvori vstream a konvertuje jej na
 * bitstream (presnejsi nez prima generace - viz mztape princip).
 *
 * @param header      128B MZF hlavicka v originalni endianite.
 * @param chunks_data Chunkova data (chunk_count * 258 B).
 * @param chunk_count Pocet chunku (musi byt >= 1).
 * @param config      Konfigurace koderu.
 * @param type        Typ vystupniho streamu.
 * @param rate        Vzorkovaci frekvence (Hz).
 * @return Novy stream, nebo NULL pri chybe.
 */
st_CMT_STREAM* mzcmt_bsd_create_stream (
    const uint8_t *header,
    const uint8_t *chunks_data,
    uint16_t chunk_count,
    const st_MZCMT_BSD_CONFIG *config,
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
            /* vstream -> bitstream konverze (presnejsi) */
            st_CMT_VSTREAM *vstream = mzcmt_bsd_create_vstream ( header, chunks_data, chunk_count, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_bsd_create_vstream ( header, chunks_data, chunk_count, config, rate );
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


const char* mzcmt_bsd_version ( void ) {
    return MZCMT_BSD_VERSION;
}

int mzcmt_bsd_get_pulse_lengths ( en_MZCMT_BSD_PULSESET pulseset, double out[4] ) {
    if ( !out || ( int ) pulseset < 0 || pulseset >= MZCMT_BSD_PULSESET_COUNT ) return EXIT_FAILURE;
    const st_bsd_pulses_length *p = g_pulses[pulseset];
    out[0] = p->long_pulse.high;
    out[1] = p->long_pulse.low;
    out[2] = p->short_pulse.high;
    out[3] = p->short_pulse.low;
    return EXIT_SUCCESS;
}
