/**
 * @file   mzcmt_turbo.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.3.0
 * @brief  Implementace TURBO koderu pro Sharp MZ.
 *
 * TURBO je standardni NORMAL FM modulace s konfigurovatelnym pulznim
 * casovanim. Kazdy bit se koduje jednim pulzem:
 * - Bit "1" (long pulz): delsi HIGH + delsi LOW (nizsi frekvence)
 * - Bit "0" (short pulz): kratsi HIGH + kratsi LOW (vyssi frekvence)
 *
 * Za kazdym bajtem nasleduje stop bit (long pulz). Bajty se odesilaji
 * od MSB k LSB (shodne s ROM rutinou).
 *
 * Časování datové části (tape API, mzcmt_turbo_create_tape_*) a čtecí
 * bod N počítá mzcmt_turbo_get_timing() z profilu zapisovače (TurboCopy
 * V1.2x) nebo z vlastních pulzů (pole *_us100, jednotka 0,1 us);
 * mzcmt_turbo_check_readpoint() ověří čitelnost ROM rutinou. Pulzní sada
 * (g_pulses_*) určuje jen předzavaděč zapisovaný 1:1.
 *
 * Experimentální API bez předzavaděče (mzcmt_turbo_create_vstream/_stream)
 * dál používá staré škálování: pulzy sady dělené poměrem rychlosti, bez
 * vazby na N a bez kontroly čitelnosti (turbo_prepare_pulses).
 *
 * @par Licence:
 * Odvozeno z projektu MZFTools v0.2.2 (mz-fuzzy@users.sf.net),
 * sireneho pod GNU General Public License v3 (GPLv3).
 *
 * Copyright (C) MZFTools authors (mz-fuzzy@users.sf.net)
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

#include "mzcmt_turbo.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* mzcmt_turbo_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* mzcmt_turbo_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  mzcmt_turbo_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_MZCMT_TURBO_ALLOCATOR g_mzcmt_turbo_default_allocator = {
    mzcmt_turbo_default_alloc,
    mzcmt_turbo_default_alloc0,
    mzcmt_turbo_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_MZCMT_TURBO_ALLOCATOR *g_allocator = &g_mzcmt_turbo_default_allocator;


/** @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL. */
void mzcmt_turbo_set_allocator ( const st_MZCMT_TURBO_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_mzcmt_turbo_default_allocator;
}


/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void mzcmt_turbo_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static mzcmt_turbo_error_cb g_error_cb = mzcmt_turbo_default_error_cb;


/** @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL. */
void mzcmt_turbo_set_error_callback ( mzcmt_turbo_error_cb cb ) {
    g_error_cb = cb ? cb : mzcmt_turbo_default_error_cb;
}


/* =========================================================================
 *  Vychozi pulzni konstanty (v sekundach)
 * ========================================================================= */

/**
 * @brief Dvojice delek pulzu (high + low cast) v sekundach.
 */
typedef struct st_turbo_pulse_length {
    double high;  /**< doba HIGH casti pulzu v sekundach */
    double low;   /**< doba LOW casti pulzu v sekundach */
} st_turbo_pulse_length;


/**
 * @brief Par delek pro long a short pulz.
 */
typedef struct st_turbo_pulses_length {
    st_turbo_pulse_length long_pulse;  /**< pulz pro bit "1" */
    st_turbo_pulse_length short_pulse; /**< pulz pro bit "0" */
} st_turbo_pulses_length;


/**
 * @brief Vychozi pulzni konstanty pro MZ-700, MZ-80K, MZ-80A.
 *
 * Symetrické hodnoty TMZ v1.0: SHORT 252 us, LONG 504 us, bez doloženého
 * zdroje [neověřeno]. ROM zapisuje pulzy asymetricky.
 */
static const st_turbo_pulses_length g_pulses_700 = {
    { 0.000504, 0.000504 },   /* long: 504 us H + 504 us L */
    { 0.000252, 0.000252 },   /* short: 252 us H + 252 us L */
};


/**
 * @brief Vychozi pulzni konstanty pro MZ-800, MZ-1500.
 *
 * Symetrické hodnoty TMZ v1.0: SHORT 249 us, LONG 498 us; v TURBO slouží
 * jen pro předzavaděč (1:1). ROM podle T-stavů píše asymetricky, asi
 * 238/259 a 469/491 us, nominál
 * dokumentace HW je 240/278 a 470/494 us (profil ROM-800-NOMINAL). Pro čtení ROM
 * 1:1 je symetrická sada čitelná (čtecí bod ~379 us).
 */
static const st_turbo_pulses_length g_pulses_800 = {
    { 0.000498, 0.000498 },         /* long: 498 us H + 498 us L */
    { 0.000249, 0.000249 },         /* short: 249 us H + 249 us L */
};


/**
 * @brief Vychozi pulzni konstanty pro MZ-80B.
 *
 * Prevzato z mztape.c (g_mztape_pulses_80B).
 */
static const st_turbo_pulses_length g_pulses_80B = {
    { 0.000333, 0.000334 },        /* long: 333 us H + 334 us L */
    { 0.000166750, 0.000166 },      /* short: 167 us H + 166 us L */
};


/**
 * @brief Pole ukazatelu na pulzni konstanty - indexovano en_MZCMT_TURBO_PULSESET.
 */
static const st_turbo_pulses_length *g_pulses[] = {
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
typedef struct st_turbo_pulse_samples {
    uint32_t high; /**< pocet vzorku HIGH casti */
    uint32_t low;  /**< pocet vzorku LOW casti */
} st_turbo_pulse_samples;


/**
 * @brief Skalovane pocty vzorku pro long a short pulz.
 */
typedef struct st_turbo_pulses_samples {
    st_turbo_pulse_samples long_pulse;  /**< vzorky long pulzu */
    st_turbo_pulse_samples short_pulse; /**< vzorky short pulzu */
} st_turbo_pulses_samples;


/**
 * @brief Pripravi skalovane pulzni sirky z konfigurace.
 *
 * Pokud jsou explicitni casovani v konfiguraci nenulove, pouzije je
 * (konverze z 0,1 us na počty vzorků). Jinak použije výchozí pulzní
 * konstanty skalovane rychlostnim divisorem.
 *
 * @param[out] pulses Vystupni struktura se skalovanymi pulzy.
 * @param config Konfigurace koderu.
 * @param rate Cilovy sample rate (Hz).
 */
static void turbo_prepare_pulses ( st_turbo_pulses_samples *pulses, const st_MZCMT_TURBO_CONFIG *config, uint32_t rate ) {

    double divisor = cmtspeed_get_divisor ( config->speed );
    const st_turbo_pulses_length *src = g_pulses[config->pulseset];

    if ( config->long_high_us100 != 0 ) {
        /* explicitní časování: 0,1 us -> sekundy -> vzorky */
        double sec = config->long_high_us100 / 10000000.0;
        pulses->long_pulse.high = ( uint32_t ) round ( sec * rate );
    } else {
        pulses->long_pulse.high = ( uint32_t ) round ( src->long_pulse.high * rate / divisor );
    }

    if ( config->long_low_us100 != 0 ) {
        double sec = config->long_low_us100 / 10000000.0;
        pulses->long_pulse.low = ( uint32_t ) round ( sec * rate );
    } else {
        pulses->long_pulse.low = ( uint32_t ) round ( src->long_pulse.low * rate / divisor );
    }

    if ( config->short_high_us100 != 0 ) {
        double sec = config->short_high_us100 / 10000000.0;
        pulses->short_pulse.high = ( uint32_t ) round ( sec * rate );
    } else {
        pulses->short_pulse.high = ( uint32_t ) round ( src->short_pulse.high * rate / divisor );
    }

    if ( config->short_low_us100 != 0 ) {
        double sec = config->short_low_us100 / 10000000.0;
        pulses->short_pulse.low = ( uint32_t ) round ( sec * rate );
    } else {
        pulses->short_pulse.low = ( uint32_t ) round ( src->short_pulse.low * rate / divisor );
    }

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
static int turbo_add_pulses ( st_CMT_VSTREAM *vstream, const st_turbo_pulse_samples *pulse, uint32_t count ) {
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
static int turbo_add_tapemark ( st_CMT_VSTREAM *vstream, const st_turbo_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
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
static int turbo_encode_data ( st_CMT_VSTREAM *vstream, const st_turbo_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        /* 8 datovych bitu, MSB first */
        for ( bit = 0; bit < 8; bit++ ) {
            const st_turbo_pulse_samples *pulse = ( byte & 0x80 )
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
static int turbo_encode_checksum ( st_CMT_VSTREAM *vstream, const st_turbo_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be_checksum = endianity_bswap16_BE ( checksum );
    return turbo_encode_data ( vstream, pulses, ( const uint8_t* ) &be_checksum, 2 );
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
uint16_t mzcmt_turbo_compute_checksum ( const uint8_t *data, uint16_t size ) {
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
 * @brief Vytvori CMT vstream z MZF dat TURBO kodovanim.
 *
 * Generuje kompletni paskovy ramec:
 * 1. LGAP (kratke pulzy, leader ton)
 * 2. Dlouhy tapemark (40 long + 40 short)
 * 3. 2 long + hlavicka (128 B) + checksum hlavicky + 2 long
 * 4. [kopie hlavicky: 256 short + hlavicka + checksum + 2 long]
 * 5. SGAP (kratke pulzy)
 * 6. Kratky tapemark (20 long + 20 short)
 * 7. 2 long + telo (N B) + checksum tela + 2 long
 * 8. [kopie tela: 256 short + telo + checksum + 2 long]
 *
 * Signal zacina v HIGH stavu. Pulzni casovani se bere z konfigurace
 * (explicitne hodnoty nebo vychozi z pulsesetu + rychlosti).
 *
 * @param header    128B MZF hlavicka v originalni endianite.
 * @param body      Datove telo (muze byt NULL pokud body_size == 0).
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace koderu.
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy vstream, nebo NULL pri chybe.
 */
st_CMT_VSTREAM* mzcmt_turbo_create_vstream (
    const uint8_t *header,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_TURBO_CONFIG *config,
    uint32_t rate
) {
    /* validace vstupu */
    if ( !header ) {
        g_error_cb ( __func__, __LINE__, "Header is NULL\n" );
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

    if ( config->pulseset >= MZCMT_TURBO_PULSESET_COUNT ) {
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
    st_turbo_pulses_samples pulses;
    turbo_prepare_pulses ( &pulses, config, rate );

    /* efektivni GAP delky */
    uint32_t lgap = config->lgap_length > 0 ? config->lgap_length : MZCMT_TURBO_LGAP_DEFAULT;
    uint32_t sgap = config->sgap_length > 0 ? config->sgap_length : MZCMT_TURBO_SGAP_DEFAULT;

    /* checksums (pocet jednickovych bitu) */
    uint16_t chk_header = mzcmt_turbo_compute_checksum ( header, sizeof ( st_MZF_HEADER ) );
    uint16_t chk_body = mzcmt_turbo_compute_checksum ( body, ( uint16_t ) body_size );

    /* signal zacina v HIGH stavu (shodne s mztape) */
    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* === 1. LGAP (kratke pulzy) === */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.short_pulse, lgap ) ) goto error;

    /* === 2. Dlouhy tapemark (40 long + 40 short) === */
    if ( EXIT_SUCCESS != turbo_add_tapemark ( vstream, &pulses, MZCMT_TURBO_LTM_LONG, MZCMT_TURBO_LTM_SHORT ) ) goto error;

    /* === 3. 2 long + hlavicka + checksum + 2 long === */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &pulses, header, sizeof ( st_MZF_HEADER ) ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &pulses, chk_header ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;

    /* === 4. Kopie hlavicky (pokud flag) === */
    if ( config->flags & MZCMT_TURBO_FLAG_HEADER_COPY ) {
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.short_pulse, MZCMT_TURBO_COPY_SEP ) ) goto error;
        if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &pulses, header, sizeof ( st_MZF_HEADER ) ) ) goto error;
        if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &pulses, chk_header ) ) goto error;
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    }

    /* === 5. SGAP (kratke pulzy) === */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.short_pulse, sgap ) ) goto error;

    /* === 6. Kratky tapemark (20 long + 20 short) === */
    if ( EXIT_SUCCESS != turbo_add_tapemark ( vstream, &pulses, MZCMT_TURBO_STM_LONG, MZCMT_TURBO_STM_SHORT ) ) goto error;

    /* === 7. 2 long + telo + checksum + 2 long === */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    if ( body_size > 0 ) {
        if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &pulses, body, ( uint16_t ) body_size ) ) goto error;
    }
    if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &pulses, chk_body ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;

    /* === 8. Kopie tela (pokud flag) === */
    if ( config->flags & MZCMT_TURBO_FLAG_BODY_COPY ) {
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.short_pulse, MZCMT_TURBO_COPY_SEP ) ) goto error;
        if ( body_size > 0 ) {
            if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &pulses, body, ( uint16_t ) body_size ) ) goto error;
        }
        if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &pulses, chk_body ) ) goto error;
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &pulses.long_pulse, 2 ) ) goto error;
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during TURBO signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z MZF dat TURBO kodovanim.
 *
 * Pro typ CMT_STREAM_TYPE_VSTREAM primo pouzije mzcmt_turbo_create_vstream().
 * Pro typ CMT_STREAM_TYPE_BITSTREAM vytvori vstream a konvertuje jej na
 * bitstream (presnejsi nez prima generace - viz mztape princip).
 *
 * @param header    128B MZF hlavicka v originalni endianite.
 * @param body      Datove telo (muze byt NULL pokud body_size == 0).
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace koderu.
 * @param type      Typ vystupniho streamu.
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy stream, nebo NULL pri chybe.
 */
st_CMT_STREAM* mzcmt_turbo_create_stream (
    const uint8_t *header,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_TURBO_CONFIG *config,
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
            st_CMT_VSTREAM *vstream = mzcmt_turbo_create_vstream ( header, body, body_size, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_turbo_create_vstream ( header, body, body_size, config, rate );
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


/* =========================================================================
 *  Embedded TurboCopy TURBO loader pro tape generovani
 * ========================================================================= */

/**
 * @brief Genericka cast TurboCopy TURBO loaderu (75 bajtu, $D400-$D44A).
 *
 * Loader se nahraje jako body preloaderu na adresu $D400 a spusti.
 * Funkce: zkopiruje ROM do RAM, patchne rychlostni parametry,
 * zkopiruje metadata programu (fsize/fstrt/fexec) do $1102,
 * a zavola ROM rutinu $002A pro nacteni TURBO dat.
 *
 * Struktura kompletniho 90B loaderu:
 * - bajty 0x00-0x4A: genericka cast (75B, vzdy stejna) = toto pole
 * - bajt 0x4B: čtecí bod N (předzavaděč ho zapíše na $0A4B)
 * - bajt 0x4C: read_param (vzdy $01, pro $0512)
 * - bajty 0x4D-0x4E: user fsize (2B LE)
 * - bajty 0x4F-0x50: user fstrt (2B LE)
 * - bajty 0x51-0x52: user fexec (2B LE)
 * - bajty 0x53-0x59: extra ROM parametry (7B, z cmnt[0..6] originalu)
 *
 * Odvozeno z reverse engineeringu TurboCopy V1.21 (Michal Kreidl).
 */
static const uint8_t g_tc_loader_generic[75] = {
    0x3E,0x08,0xD3,0xCE,0xE5,0x21,0x00,0x00,0xD3,0xE4,0x7E,0xD3,0xE0,0x77,0x23,
    0x7C,0xFE,0x10,0x20,0xF4,0x3A,0x4B,0xD4,0x32,0x4B,0x0A,0x3A,0x4C,0xD4,0x32,
    0x12,0x05,0x21,0x4D,0xD4,0x11,0x02,0x11,0x01,0x0D,0x00,0xED,0xB0,0xE1,0x7C,
    0xFE,0xD4,0x28,0x12,0x2A,0x04,0x11,0xD9,0x21,0x00,0x12,0x22,0x04,0x11,0xCD,
    0x2A,0x00,0xD3,0xE4,0xC3,0x9A,0xE9,0xCD,0x2A,0x00,0xD3,0xE4,0xC3,0x24,0x01
};


/** @brief Velikost genericke casti TurboCopy loaderu (bajty). */
#define TC_LOADER_GENERIC_SIZE  75

/** @brief Celkova velikost TurboCopy loader body (bajty), viz MZCMT_TURBO_PRELOADER_SIZE. */
#define TC_LOADER_BODY_SIZE     MZCMT_TURBO_PRELOADER_SIZE

/** @brief Load/exec adresa TurboCopy loaderu v pameti Z80. */
#define TC_LOADER_ADDR          0xD400

/** @brief Offset čtecího bodu N v loader body (1B, předzavaděč ho zapíše na $0A4B). */
#define TC_OFF_SPEED            0x4B

/** @brief Offset read_param v loader body (1B, vzdy $01, pro $0512). */
#define TC_OFF_RDPARAM          0x4C

/** @brief Offset user fsize v loader body (2B LE). */
#define TC_OFF_FSIZE            0x4D

/** @brief Offset user fstrt v loader body (2B LE). */
#define TC_OFF_FSTRT            0x4F

/** @brief Offset user fexec v loader body (2B LE). */
#define TC_OFF_FEXEC            0x51

/** @brief Offset extra ROM parametru v loader body (7B, z cmnt[0..6]). */
#define TC_OFF_ROMPARAMS        0x53

/**
 * @brief TurboCopy identifikacni signatura (7 bajtu).
 *
 * TurboCopy zapisuje tuto signaturu do cmnt[0..6] preloader hlavicky.
 * Intercopy 10.2 ji porovnava s hodnotou na $18CA pro detekci
 * TURBO formatu. Signatura nahradi puvodni cmnt[0..6], ktere se
 * ulozi do loader body na offsetu $53 (extra ROM parametry).
 */
static const uint8_t g_tc_signature[7] = {
    0x5B, 0x96, 0xA5, 0x9D, 0x9A, 0xB7, 0x5D
};


int mzcmt_turbo_build_preloader ( const st_MZF_HEADER *original, uint32_t body_size, uint8_t readpoint,
                                  uint8_t header_out[128], uint8_t body_out[MZCMT_TURBO_PRELOADER_SIZE] ) {
    if ( !original || !header_out || !body_out || body_size > 0xFFFF ) return EXIT_FAILURE;

    /* hlavička: fsize/fstrt/fexec loaderu, signatura v cmnt[0..6], cmnt[7..103] z originálu */
    st_MZF_HEADER preloader_hdr;
    memcpy ( &preloader_hdr, original, sizeof ( st_MZF_HEADER ) );
    preloader_hdr.fsize = TC_LOADER_BODY_SIZE;
    preloader_hdr.fstrt = TC_LOADER_ADDR;
    preloader_hdr.fexec = TC_LOADER_ADDR;
    memcpy ( &preloader_hdr.cmnt[0], g_tc_signature, 7 );
    mzf_header_swap_endianity ( &preloader_hdr );   /* -> LE, jak jde na pásku; 11h z originálu */
    memcpy ( header_out, &preloader_hdr, sizeof ( st_MZF_HEADER ) );

    /* tělo: generická část + čtecí bod N + read_param + metadata programu + cmnt[0..6] originálu */
    memset ( body_out, 0, TC_LOADER_BODY_SIZE );
    memcpy ( body_out, g_tc_loader_generic, TC_LOADER_GENERIC_SIZE );
    body_out[TC_OFF_SPEED] = readpoint;
    body_out[TC_OFF_RDPARAM] = 0x01;
    uint16_t u_size = endianity_bswap16_LE ( ( uint16_t ) body_size );
    uint16_t u_strt = endianity_bswap16_LE ( original->fstrt );
    uint16_t u_exec = endianity_bswap16_LE ( original->fexec );
    memcpy ( &body_out[TC_OFF_FSIZE], &u_size, 2 );
    memcpy ( &body_out[TC_OFF_FSTRT], &u_strt, 2 );
    memcpy ( &body_out[TC_OFF_FEXEC], &u_exec, 2 );
    memcpy ( &body_out[TC_OFF_ROMPARAMS], &original->cmnt[0], 7 );
    return EXIT_SUCCESS;
}


/* =========================================================================
 *  Čtecí bod ROM a časování datové části (profil zapisovače)
 * ========================================================================= */

/** @brief Takt CPU MZ-800 pro výpočet T-stavů čtecí rutiny (Hz). */
#define TURBO_CPU_HZ        3546895.0


int mzcmt_turbo_check_readpoint ( const st_MZCMT_TURBO_TIMING *timing, uint32_t rate, st_MZCMT_TURBO_MARGINS *margins ) {
    st_MZCMT_TURBO_MARGINS local;
    if ( !margins ) margins = &local;
    memset ( margins, 0, sizeof ( *margins ) );
    if ( !timing || rate == 0 ) return EXIT_FAILURE;

    double t_per_sample = TURBO_CPU_HZ / rate;
    double us_per_t = 1000000.0 / TURBO_CPU_HZ;
    double n = timing->readpoint;
    double sh = timing->short_high * t_per_sample;
    double sl = timing->short_low * t_per_sample;
    double lh = timing->long_high * t_per_sample;

    margins->short_high_us = ( ( 86.0 + 14.0 * n ) - sh ) * us_per_t;
    margins->long_high_us = ( lh - ( 138.0 + 14.0 * n ) ) * us_per_t;
    margins->short_period_us = ( ( sh + sl ) - ( 256.0 + 14.0 * n ) ) * us_per_t;

    if ( margins->short_high_us < MZCMT_TURBO_MIN_MARGIN_US ) return EXIT_FAILURE;
    if ( margins->long_high_us < MZCMT_TURBO_MIN_MARGIN_US ) return EXIT_FAILURE;
    if ( margins->short_period_us < MZCMT_TURBO_MIN_MARGIN_US ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/** @brief Takt čítače CTC0 (8253) MZ-800 (Hz) - zdroj tiků S-modelu TurboCopy. */
#define TURBO_CTC0_HZ       1108405.0


/**
 * @brief Převede en_CMTSPEED na poměr a:b s číslicemi 1-9 (zápis TurboCopy).
 *
 * @param speed Rychlost.
 * @param[out] a Čitatel (1-9).
 * @param[out] b Jmenovatel (1-9).
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE pro poměr, který TurboCopy zadat
 *         neumí (25:14, 2:1 CP/M) nebo neplatnou hodnotu.
 */
int mzcmt_turbo_speed_to_ratio ( en_CMTSPEED speed, unsigned *a, unsigned *b ) {
    switch ( speed ) {
        case CMTSPEED_1_1: *a = 1; *b = 1; return EXIT_SUCCESS;
        case CMTSPEED_2_1: *a = 2; *b = 1; return EXIT_SUCCESS;
        case CMTSPEED_3_1: *a = 3; *b = 1; return EXIT_SUCCESS;
        case CMTSPEED_3_2: *a = 3; *b = 2; return EXIT_SUCCESS;
        case CMTSPEED_7_3: *a = 7; *b = 3; return EXIT_SUCCESS;
        case CMTSPEED_8_3: *a = 8; *b = 3; return EXIT_SUCCESS;
        case CMTSPEED_9_7: *a = 9; *b = 7; return EXIT_SUCCESS;
        default: return EXIT_FAILURE;
    }
}


int mzcmt_turbo_speed_is_supported ( en_MZCMT_TURBO_PROFILE profile, en_CMTSPEED speed ) {
    unsigned a, b;
    if ( profile != MZCMT_TURBO_PROFILE_TURBOCOPY_V12X ) return 0;
    return mzcmt_turbo_speed_to_ratio ( speed, &a, &b ) == EXIT_SUCCESS;
}


/**
 * @brief Najde čtecí bod N s největší rezervou pro dané pulzy.
 *
 * Přípustné N splňuje (v T-stavech) sh < 86+14N, lh > 138+14N
 * a sh+sl > 256+14N. Vrací střed přípustného intervalu v 1..255.
 *
 * @param t Časování ve vzorcích (readpoint se nečte).
 * @param rate Vzorkovací frekvence (Hz, > 0).
 * @return N, nebo 0 pokud je přípustný interval prázdný.
 */
static uint8_t turbo_readpoint_from_pulses ( const st_MZCMT_TURBO_TIMING *t, uint32_t rate ) {
    double tps = TURBO_CPU_HZ / rate;
    double sh = t->short_high * tps;
    double sl = t->short_low * tps;
    double lh = t->long_high * tps;
    long n_min = ( long ) floor ( ( sh - 86.0 ) / 14.0 ) + 1;
    double hi1 = ( lh - 138.0 ) / 14.0;
    double hi2 = ( sh + sl - 256.0 ) / 14.0;
    long n_max = ( long ) ceil ( hi1 < hi2 ? hi1 : hi2 ) - 1;
    if ( n_min < 1 ) n_min = 1;
    if ( n_max > 255 ) n_max = 255;
    if ( n_max < n_min ) return 0;
    return ( uint8_t ) ( ( n_min + n_max ) / 2 );
}


int mzcmt_turbo_get_timing ( const st_MZCMT_TURBO_CONFIG *config, uint32_t rate, st_MZCMT_TURBO_TIMING *timing ) {
    if ( !timing ) return EXIT_FAILURE;
    memset ( timing, 0, sizeof ( *timing ) );
    if ( !config || rate == 0 ) return EXIT_FAILURE;

    if ( config->profile != MZCMT_TURBO_PROFILE_TURBOCOPY_V12X ) {
        g_error_cb ( __func__, __LINE__, "Unknown TURBO profile %d\n", config->profile );
        return EXIT_FAILURE;
    }

    int custom = config->long_high_us100 || config->long_low_us100 || config->short_high_us100 || config->short_low_us100;

    if ( !custom ) {
        unsigned a, b;
        if ( EXIT_SUCCESS != mzcmt_turbo_speed_to_ratio ( config->speed, &a, &b ) ) {
            g_error_cb ( __func__, __LINE__, "Speed %s is not supported by the TurboCopy profile\n", cmtspeed_get_ratiotxt ( config->speed ) );
            return EXIT_FAILURE;
        }
        /* S-model TurboCopy (hypotéza): S tiků CTC0, SHORT = S/2, LONG = S na půlvlnu */
        unsigned s_ticks = 71 + ( 480 * b ) / a;
        double tick = ( double ) rate / TURBO_CTC0_HZ;
        timing->readpoint = ( uint8_t ) ( ( 82 * b ) / a ); /* celočíselně jako TurboCopy */
        timing->short_high = ( uint32_t ) lround ( s_ticks / 2.0 * tick );
        timing->short_low = timing->short_high;
        timing->long_high = ( uint32_t ) lround ( s_ticks * tick );
        timing->long_low = timing->long_high;
    } else {
        /* vlastní pulzy (0,1 us); chybějící půlvlna = druhá půlvlna téhož pulzu */
        double sh = config->short_high_us100, sl = config->short_low_us100;
        double lh = config->long_high_us100, ll = config->long_low_us100;
        if ( ( sh == 0 && sl == 0 ) || ( lh == 0 && ll == 0 ) ) {
            g_error_cb ( __func__, __LINE__, "Incomplete custom pulses (both halves of a pulse are zero)\n" );
            return EXIT_FAILURE;
        }
        if ( sh == 0 ) sh = sl;
        if ( sl == 0 ) sl = sh;
        if ( lh == 0 ) lh = ll;
        if ( ll == 0 ) ll = lh;
        timing->short_high = ( uint32_t ) lround ( sh / 1e7 * rate );
        timing->short_low = ( uint32_t ) lround ( sl / 1e7 * rate );
        timing->long_high = ( uint32_t ) lround ( lh / 1e7 * rate );
        timing->long_low = ( uint32_t ) lround ( ll / 1e7 * rate );
    }

    if ( timing->short_high < 1 ) timing->short_high = 1;
    if ( timing->short_low < 1 ) timing->short_low = 1;
    if ( timing->long_high < 1 ) timing->long_high = 1;
    if ( timing->long_low < 1 ) timing->long_low = 1;

    if ( custom ) {
        timing->readpoint = turbo_readpoint_from_pulses ( timing, rate );
        if ( timing->readpoint == 0 ) {
            g_error_cb ( __func__, __LINE__, "Custom pulses admit no ROM read point\n" );
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


/* =========================================================================
 *  Verejne tape API
 * ========================================================================= */

/**
 * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Generuje dvoudilny signal v TurboCopy kompatibilnim formatu:
 *
 * Cast 1 - preloader v NORMAL FM 1:1:
 *   LGAP(22000) + LTM(40L+40S) + 2L + HDR(128B, fsize=90) + CHKH + 2L
 *   + SGAP + STM(20L+20S) + 2L + BODY(90B TurboCopy loader) + CHKB + 2L
 *
 * Cast 2 - uzivatelska data v TURBO rychlosti (body-only):
 *   LGAP + STM(20L+20S) + 2L + BODY + CHKB + 2L + [kopie]
 *
 * TurboCopy loader (90B na $D400) patchne ROM:
 * - $0A4B = čtecí bod N z mzcmt_turbo_get_timing()
 * - $0512 = $01 (počet čtených kopií bloku, H v @RBLOK)
 * Pak vola $002A (CMT read), ktera cte pouze telo v TURBO rychlosti.
 * Metadata (fsize/fstrt/fexec) jsou v loader body na offsetu $4D.
 *
 * @param original  Originalni MZF hlavicka (v host byte-order).
 * @param body      Datove telo.
 * @param body_size Velikost datoveho tela.
 * @param config    Konfigurace: pulseset (jen předzavaděč; 80B je chyba),
 *                  speed a profile, nebo vlastní pulzy v 0,1 us (pak se
 *                  N počítá z pulzů), délky GAP, flags.
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy vstream, nebo NULL pri chybe: neplatný vstup, sada 80B,
 *         rychlost nepodporovaná profilem, časování nečitelné ROM s
 *         rezervou MZCMT_TURBO_MIN_MARGIN_US, chyba vstreamu.
 */
st_CMT_VSTREAM* mzcmt_turbo_create_tape_vstream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_TURBO_CONFIG *config,
    uint32_t rate
) {
    /* validace vstupu */
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

    if ( config->pulseset >= MZCMT_TURBO_PULSESET_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid pulseset %d\n", config->pulseset );
        return NULL;
    }

    if ( config->pulseset == MZCMT_TURBO_PULSESET_80B ) {
        /* TurboCopy je program pro MZ-700/800; pulzy 80B jsou pro ROM 1:1 nečitelné */
        g_error_cb ( __func__, __LINE__, "TURBO format is not supported for MZ-80B pulseset\n" );
        return NULL;
    }

    if ( rate == 0 ) {
        g_error_cb ( __func__, __LINE__, "Sample rate is 0\n" );
        return NULL;
    }

    /*
     * Časování datové části a čtecí bod N z profilu (nebo z vlastních
     * pulzů). Rychlost se validuje zde. Signál, který ROM se zapsaným N
     * nepřečte s rezervou MZCMT_TURBO_MIN_MARGIN_US, se negeneruje.
     */
    st_MZCMT_TURBO_TIMING timing;
    if ( EXIT_SUCCESS != mzcmt_turbo_get_timing ( config, rate, &timing ) ) {
        return NULL;
    }
    {
        st_MZCMT_TURBO_MARGINS m;
        if ( EXIT_SUCCESS != mzcmt_turbo_check_readpoint ( &timing, rate, &m ) ) {
            g_error_cb ( __func__, __LINE__,
                         "TURBO timing not readable at %u Hz (N=%u, margins %.1f/%.1f/%.1f us, min %.1f)\n",
                         rate, timing.readpoint, m.short_high_us, m.long_high_us, m.short_period_us,
                         MZCMT_TURBO_MIN_MARGIN_US );
            return NULL;
        }
    }

    /* ===== A, B. Hlavička a tělo předzavaděče TurboCopy ===== */

    uint8_t preloader_raw[sizeof ( st_MZF_HEADER )];
    uint8_t tc_body[TC_LOADER_BODY_SIZE];
    if ( EXIT_SUCCESS != mzcmt_turbo_build_preloader ( original, body_size, timing.readpoint, preloader_raw, tc_body ) ) {
        g_error_cb ( __func__, __LINE__, "Can't build TurboCopy preloader\n" );
        return NULL;
    }
    /* LE bajty hlavičky, jak je kóduje signál */
    st_MZF_HEADER preloader_hdr;
    memcpy ( &preloader_hdr, preloader_raw, sizeof ( st_MZF_HEADER ) );

    /* ===== C. Pulzy pro preloader (1:1 rychlost) ===== */

    st_MZCMT_TURBO_CONFIG loader_config = {
        .pulseset = config->pulseset,
        .speed = CMTSPEED_1_1,
        .lgap_length = 0,
        .sgap_length = 0,
        .long_high_us100 = 0,
        .long_low_us100 = 0,
        .short_high_us100 = 0,
        .short_low_us100 = 0,
        .flags = 0,
    };

    st_turbo_pulses_samples hdr_pulses;
    turbo_prepare_pulses ( &hdr_pulses, &loader_config, rate );

    /* checksums preloaderu */
    uint16_t chk_preloader_hdr = mzcmt_turbo_compute_checksum (
                                     ( const uint8_t* ) &preloader_hdr, sizeof ( st_MZF_HEADER ) );
    uint16_t chk_tc_body = mzcmt_turbo_compute_checksum (
                               tc_body, TC_LOADER_BODY_SIZE );

    /*
     * Parametry datové části (část 2) - počítají se před prvním skokem
     * na návěští error, aby goto nepřeskakovalo inicializace.
     */
    st_turbo_pulses_samples data_pulses;
    data_pulses.short_pulse.high = timing.short_high;
    data_pulses.short_pulse.low = timing.short_low;
    data_pulses.long_pulse.high = timing.long_high;
    data_pulses.long_pulse.low = timing.long_low;

    uint32_t lgap = config->lgap_length > 0 ? config->lgap_length : MZCMT_TURBO_LGAP_DEFAULT;

    uint16_t chk_body = mzcmt_turbo_compute_checksum ( body, ( uint16_t ) body_size );

    /* ===== D. Vytvoreni vstreamu ===== */

    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* ----- Cast 1: TurboCopy preloader v NORMAL 1:1 ----- */

    /* LGAP (22000 kratkych pulzu) */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.short_pulse, 22000 ) ) goto error;

    /* dlouhy tapemark (40 long + 40 short) */
    if ( EXIT_SUCCESS != turbo_add_tapemark ( vstream, &hdr_pulses, MZCMT_TURBO_LTM_LONG, MZCMT_TURBO_LTM_SHORT ) ) goto error;

    /* 2 long + hlavicka(128B) + checksum + 2 long */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &hdr_pulses, ( const uint8_t* ) &preloader_hdr, sizeof ( st_MZF_HEADER ) ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &hdr_pulses, chk_preloader_hdr ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /* SGAP + kratky tapemark + body(90B TurboCopy loader) */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.short_pulse, MZCMT_TURBO_SGAP_DEFAULT ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_tapemark ( vstream, &hdr_pulses, MZCMT_TURBO_STM_LONG, MZCMT_TURBO_STM_SHORT ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &hdr_pulses, tc_body, TC_LOADER_BODY_SIZE ) ) goto error;
    if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &hdr_pulses, chk_tc_body ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /*
     * ----- Cast 2: Uzivatelska data v TURBO formatu -----
     *
     * Předzavaděč TurboCopy (tc_body, mzcmt_turbo_build_preloader) patchne ROM ($0512=$01 počet kopií, $0A4B=čtecí bod N)
     * a vola $04F8 (RDATA), ktera cte POUZE telo (body-only).
     * Proto generujeme pouze:
     *   LGAP + STM(20+20) + 2L + BODY + CRC + 2L + [kopie]
     *
     * Hlavicka se NEPOSILA - metadata (fsize/fstrt/fexec) jsou
     * ulozena v loader comment oblasti preloaderu.
     *
     * data_pulses, lgap a chk_body jsou spočítané před částí 1.
     */

    /* LGAP */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &data_pulses.short_pulse, lgap ) ) goto error;

    /* kratky tapemark (20 long + 20 short) - ROM RDATA s $0512=$01 */
    if ( EXIT_SUCCESS != turbo_add_tapemark ( vstream, &data_pulses, MZCMT_TURBO_STM_LONG, MZCMT_TURBO_STM_SHORT ) ) goto error;

    /* 2 long + telo + checksum + 2 long */
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &data_pulses.long_pulse, 2 ) ) goto error;
    if ( body_size > 0 ) {
        if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &data_pulses, body, ( uint16_t ) body_size ) ) goto error;
    }
    if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &data_pulses, chk_body ) ) goto error;
    if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &data_pulses.long_pulse, 2 ) ) goto error;

    /* kopie tela */
    if ( config->flags & MZCMT_TURBO_FLAG_BODY_COPY ) {
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &data_pulses.short_pulse, MZCMT_TURBO_COPY_SEP ) ) goto error;
        if ( body_size > 0 ) {
            if ( EXIT_SUCCESS != turbo_encode_data ( vstream, &data_pulses, body, ( uint16_t ) body_size ) ) goto error;
        }
        if ( EXIT_SUCCESS != turbo_encode_checksum ( vstream, &data_pulses, chk_body ) ) goto error;
        if ( EXIT_SUCCESS != turbo_add_pulses ( vstream, &data_pulses.long_pulse, 2 ) ) goto error;
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during TURBO tape signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Obaluje mzcmt_turbo_create_tape_vstream() do polymorfniho st_CMT_STREAM.
 *
 * @param original  Originalni MZF hlavicka.
 * @param body      Datove telo.
 * @param body_size Velikost datoveho tela.
 * @param config    Konfigurace TURBO koderu.
 * @param type      Typ vystupniho streamu.
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy stream, nebo NULL pri chybe.
 */
st_CMT_STREAM* mzcmt_turbo_create_tape_stream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_TURBO_CONFIG *config,
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
            st_CMT_VSTREAM *vstream = mzcmt_turbo_create_tape_vstream ( original, body, body_size, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_turbo_create_tape_vstream ( original, body, body_size, config, rate );
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


const char* mzcmt_turbo_version ( void ) {
    return MZCMT_TURBO_VERSION;
}
