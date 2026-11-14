/**
 * @file   mzcmt_fsk.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace FSK (Frequency Shift Keying) koderu pro Sharp MZ.
 *
 * FSK modulace koduje bity zmenou frekvence signalu:
 * - Bit "1" (long pulz): vice LOW vzorku + vice HIGH vzorku (nizsi frekvence)
 * - Bit "0" (short pulz): mene LOW vzorku + mene HIGH vzorku (vyssi frekvence)
 *
 * Pocty vzorku na pulz zavisi na rychlostni urovni (speed 0-6) a jsou
 * definovany v referencnich tabulkach pri 44100 Hz. Pro jine vzorkovaci
 * frekvence se pocty proporcionalne skaluji.
 *
 * @par Princip skalovani:
 * @verbatim
 *   vzorky_pri_cilovem_rate = round(ref_vzorky * cilovy_rate / 44100)
 * @endverbatim
 * Toto zachovava absolutni casovani signalu nezavisle na vystupni frekvenci.
 * Minimalni hodnota po skalovani je 1 vzorek (zarucuje platny signal).
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
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/endianity/endianity.h"

#include "mzcmt_fsk.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* mzcmt_fsk_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* mzcmt_fsk_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  mzcmt_fsk_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_MZCMT_FSK_ALLOCATOR g_mzcmt_fsk_default_allocator = {
    mzcmt_fsk_default_alloc,
    mzcmt_fsk_default_alloc0,
    mzcmt_fsk_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_MZCMT_FSK_ALLOCATOR *g_allocator = &g_mzcmt_fsk_default_allocator;


/** @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL. */
void mzcmt_fsk_set_allocator ( const st_MZCMT_FSK_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_mzcmt_fsk_default_allocator;
}


/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void mzcmt_fsk_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static mzcmt_fsk_error_cb g_error_cb = mzcmt_fsk_default_error_cb;


/** @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL. */
void mzcmt_fsk_set_error_callback ( mzcmt_fsk_error_cb cb ) {
    g_error_cb = cb ? cb : mzcmt_fsk_default_error_cb;
}


/* =========================================================================
 *  Referencni pulzni tabulky (pocty vzorku pri 44100 Hz)
 * ========================================================================= */

/**
 * @brief Referencni pocty vzorku jednoho FSK pulzu.
 *
 * Kazdy pulz se sklada z LOW casti (lnl vzorku) a HIGH casti (lnh vzorku).
 * Hodnoty jsou definovany pri referencni frekvenci 44100 Hz.
 */
typedef struct st_fsk_pulse_ref {
    uint8_t low;  /**< pocet LOW vzorku pri ref. frekvenci */
    uint8_t high; /**< pocet HIGH vzorku pri ref. frekvenci */
} st_fsk_pulse_ref;


/**
 * @brief Dvojice referencnich pulzu (long + short) pro jednu rychlostni uroven.
 */
typedef struct st_fsk_speed_ref {
    st_fsk_pulse_ref long_pulse;  /**< pulz pro bit "1" */
    st_fsk_pulse_ref short_pulse; /**< pulz pro bit "0" */
} st_fsk_speed_ref;


/**
 * @brief Referencni pulzni tabulky pro vsech 7 rychlostnich urovni.
 *
 * Prevzato z mzftools coder.c, funkce fsk_pulse().
 * Indexovano hodnotou en_MZCMT_FSK_SPEED.
 *
 * Pomery long:short (celkove vzorky):
 * - Speed 0: 8:4 = 2.00
 * - Speed 1: 7:4 = 1.75
 * - Speed 2: 6:4 = 1.50
 * - Speed 3: 6:3 = 2.00
 * - Speed 4: 5:2 = 2.50
 * - Speed 5: 4:2 = 2.00
 * - Speed 6: 3:2 = 1.50
 */
static const st_fsk_speed_ref g_fsk_ref[MZCMT_FSK_SPEED_COUNT] = {
    /* speed 0 */ { { 2, 6 }, { 2, 2 } },
    /* speed 1 */ { { 2, 5 }, { 2, 2 } },
    /* speed 2 */ { { 2, 4 }, { 2, 2 } },
    /* speed 3 */ { { 2, 4 }, { 2, 1 } },
    /* speed 4 */ { { 2, 3 }, { 1, 1 } },
    /* speed 5 */ { { 2, 2 }, { 1, 1 } },
    /* speed 6 */ { { 1, 2 }, { 1, 1 } },
};


/* =========================================================================
 *  Skalovani a interni typy
 * ========================================================================= */

/**
 * @brief Skalovane pocty vzorku jednoho FSK pulzu pro cilovy sample rate.
 */
typedef struct st_fsk_pulse {
    uint32_t low;  /**< pocet LOW vzorku */
    uint32_t high; /**< pocet HIGH vzorku */
} st_fsk_pulse;


/**
 * @brief Skalovane pocty vzorku pro long a short pulz.
 */
typedef struct st_fsk_pulses {
    st_fsk_pulse long_pulse;  /**< pulz pro bit "1" */
    st_fsk_pulse short_pulse; /**< pulz pro bit "0" */
} st_fsk_pulses;


/**
 * @brief Skaluje referencni pocet vzorku na cilovy sample rate.
 *
 * Pouziva proporcionalni skalovani: result = round(ref * rate / 44100).
 * Minimalni navratova hodnota je 1 (zarucuje platny vstream event).
 *
 * @param ref_samples Pocet vzorku pri referencni frekvenci 44100 Hz.
 * @param rate Cilova vzorkovaci frekvence (Hz).
 * @return Skalovany pocet vzorku (minimalne 1).
 */
static inline uint32_t fsk_scale ( uint32_t ref_samples, uint32_t rate ) {
    uint32_t s = ( uint32_t ) round ( ( double ) ref_samples * rate / MZCMT_FSK_REF_RATE );
    return s > 0 ? s : 1;
}


/**
 * @brief Pripravi skalovane pulzni sirky pro danou rychlost a sample rate.
 *
 * @param[out] pulses Vystupni struktura se skalovanymi pulzy.
 * @param speed Rychlostni uroven (0-6).
 * @param rate Cilovy sample rate (Hz).
 */
static void fsk_prepare_pulses ( st_fsk_pulses *pulses, en_MZCMT_FSK_SPEED speed, uint32_t rate ) {
    const st_fsk_speed_ref *ref = &g_fsk_ref[speed];
    pulses->long_pulse.low = fsk_scale ( ref->long_pulse.low, rate );
    pulses->long_pulse.high = fsk_scale ( ref->long_pulse.high, rate );
    pulses->short_pulse.low = fsk_scale ( ref->short_pulse.low, rate );
    pulses->short_pulse.high = fsk_scale ( ref->short_pulse.high, rate );
}


/* =========================================================================
 *  Interni pomocne funkce pro generovani signalu
 * ========================================================================= */

/**
 * @brief Prida GAP (leader) sekvenci do vstreamu.
 *
 * GAP slouzi jako leader ton pred daty (dekoder na Z80 ma pevny prah z tabulky).
 * Dekoder meri sirku GAP pulzu a nastavi prahovou hodnotu pro
 * rozliseni short/long datovych pulzu.
 *
 * Generuje 200 cyklu, kazdy cyklus = 8 LOW + 8 HIGH vzorku
 * (pri ref. frekvenci, skalovano na cilovy rate).
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_add_gap ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = fsk_scale ( MZCMT_FSK_GAP_LOW_REF, rate );
    uint32_t high = fsk_scale ( MZCMT_FSK_GAP_HIGH_REF, rate );
    int i;
    for ( i = 0; i < MZCMT_FSK_GAP_CYCLES; i++ ) {
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida polaritni pulz do vstreamu.
 *
 * Kratky asymetricky pulz (4 LOW + 2 HIGH pri ref. frekvenci).
 * Dekoder jej pouziva k detekci polarity signalu a pripadne
 * modifikuje skokove instrukce pro spravne rozpoznavani hran.
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu po GAP).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_add_polarity ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = fsk_scale ( MZCMT_FSK_POLARITY_LOW_REF, rate );
    uint32_t high = fsk_scale ( MZCMT_FSK_POLARITY_HIGH_REF, rate );
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Prida synchronizacni kod do vstreamu.
 *
 * Sync kod (2 LOW + 2 HIGH vzorky pri ref. frekvenci) oznacuje konec
 * leader sekvence a zacatek datoveho toku. Dekoder po jeho detekci
 * prechazi do rezimu cteni bajtu.
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu po polarity).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_add_sync ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = fsk_scale ( MZCMT_FSK_SYNC_LOW_REF, rate );
    uint32_t high = fsk_scale ( MZCMT_FSK_SYNC_HIGH_REF, rate );
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje jeden datovy bajt do vstreamu FSK modulaci.
 *
 * Bajt se koduje od MSB k LSB (8 pulzu celkem). Kazdy bit urcuje
 * vyber long (bit=1) nebo short (bit=0) pulzu. Za bajtem NENASLEDUJE
 * stop bit (na rozdil od NORMAL FM formatu).
 *
 * Dekoder na Z80 detekuje konec bajtu pomoci shift registru:
 * bajt je inicializovan na 0xFE a po kazdem prijmem bitu se rotuje.
 * Kdyz nula vypadne z registru, bajt je kompletni.
 *
 * @param vstream Cilovy vstream.
 * @param byte Bajt k zakodovani.
 * @param pulses Skalovane pulzni sirky.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_encode_byte ( st_CMT_VSTREAM *vstream, uint8_t byte, const st_fsk_pulses *pulses ) {
    int bit;
    for ( bit = 0; bit < 8; bit++ ) {
        const st_fsk_pulse *pulse = ( byte & 0x80 ) ? &pulses->long_pulse : &pulses->short_pulse;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
        byte <<= 1;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida fade-out sekvenci do vstreamu.
 *
 * Postupne prodluzujici se pulzy (od 32 do 46 vzorku pri ref. frekvenci,
 * s krokem 2). Dekoder detekuje konec datoveho toku podle "extremne
 * dlouheho pulzu" (cp $C0 v Z80 kodu) a ukonci prijem.
 *
 * @param vstream Cilovy vstream.
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_add_fadeout ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    int w;
    for ( w = MZCMT_FSK_FADEOUT_START_REF; w < MZCMT_FSK_FADEOUT_END_REF; w += MZCMT_FSK_FADEOUT_STEP_REF ) {
        uint32_t s = fsk_scale ( ( uint32_t ) w, rate );
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, s ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, s ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

/**
 * @brief Vytvori CMT vstream z datoveho bloku FSK kodovanim.
 *
 * Generuje kompletni FSK signal:
 * 1. GAP (200 cyklu leader tonu)
 * 2. Polaritni pulz (detekce polarity signalu)
 * 3. Sync kod (oznaceni zacatku dat)
 * 4. Datove bajty (8 FSK pulzu na bajt, MSB first, bez stop bitu)
 * 5. Fade-out (rostouci pulzy pro detekci konce streamu)
 *
 * Signal zacina v LOW stavu. Pulzni sirky se skaluji z referencni
 * frekvence 44100 Hz na cilovy @p rate proporcionalne (zachovava
 * absolutni casovani).
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param speed     Rychlostni uroven (0-6).
 * @param rate      Vzorkovaci frekvence vystupniho vstreamu (Hz).
 * @return Ukazatel na novy vstream, nebo NULL pri chybe.
 *         Volajici vlastni vstream a musi jej uvolnit pres cmt_vstream_destroy().
 */
st_CMT_VSTREAM* mzcmt_fsk_create_vstream (
    const uint8_t *data,
    uint32_t data_size,
    en_MZCMT_FSK_SPEED speed,
    uint32_t rate
) {
    if ( !data ) {
        g_error_cb ( __func__, __LINE__, "Input data is NULL\n" );
        return NULL;
    }

    if ( data_size == 0 ) {
        g_error_cb ( __func__, __LINE__, "Data size is 0\n" );
        return NULL;
    }

    if ( speed >= MZCMT_FSK_SPEED_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid speed level %d (max %d)\n", speed, MZCMT_FSK_SPEED_COUNT - 1 );
        return NULL;
    }

    if ( rate == 0 ) {
        g_error_cb ( __func__, __LINE__, "Sample rate is 0\n" );
        return NULL;
    }

    /* signal zacina v LOW stavu */
    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 0, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* priprava skalovanych pulznich sirek */
    st_fsk_pulses pulses;
    fsk_prepare_pulses ( &pulses, speed, rate );

    /* 1. GAP (leader ton) */
    if ( EXIT_SUCCESS != fsk_add_gap ( vstream, rate ) ) goto error;

    /* 2. polaritni pulz */
    if ( EXIT_SUCCESS != fsk_add_polarity ( vstream, rate ) ) goto error;

    /* 3. sync kod */
    if ( EXIT_SUCCESS != fsk_add_sync ( vstream, rate ) ) goto error;

    /* 4. datove bajty */
    uint32_t i;
    for ( i = 0; i < data_size; i++ ) {
        if ( EXIT_SUCCESS != fsk_encode_byte ( vstream, data[i], &pulses ) ) goto error;
    }

    /* 5. fade-out */
    if ( EXIT_SUCCESS != fsk_add_fadeout ( vstream, rate ) ) goto error;

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during FSK signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z datoveho bloku FSK kodovanim.
 *
 * Pro typ CMT_STREAM_TYPE_VSTREAM primo pouzije mzcmt_fsk_create_vstream().
 * Pro typ CMT_STREAM_TYPE_BITSTREAM vytvori vstream a konvertuje jej na
 * bitstream (presnejsi nez prima generace bitstreamu - viz mztape princip).
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param speed     Rychlostni uroven (0-6).
 * @param type      Typ vystupniho streamu (bitstream/vstream).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Ukazatel na novy stream, nebo NULL pri chybe.
 *         Volajici vlastni stream a musi jej uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* mzcmt_fsk_create_stream (
    const uint8_t *data,
    uint32_t data_size,
    en_MZCMT_FSK_SPEED speed,
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
            st_CMT_VSTREAM *vstream = mzcmt_fsk_create_vstream ( data, data_size, speed, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_fsk_create_vstream ( data, data_size, speed, rate );
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


/**
 * @brief Spocita XOR checksum datoveho bloku.
 *
 * FSK dekoder na Z80 prubezne XORuje kazdy prijaty bajt do akumulatoru
 * (instrukce XOR B v Z80 kodu). Pocatecni hodnota akumulatoru je 0.
 * Po prijeti vsech bajtu porovnava vysledek s predpocitanou hodnotou
 * ulozenou v kodu loaderu (self-modifying code na adrese checksumx).
 *
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
 */
uint8_t mzcmt_fsk_compute_checksum ( const uint8_t *data, uint32_t size ) {
    if ( !data || size == 0 ) return 0;
    uint8_t cs = 0;
    uint32_t i;
    for ( i = 0; i < size; i++ ) {
        cs ^= data[i];
    }
    return cs;
}


/* =========================================================================
 *  Embedded loader binarky pro FSK tape
 * ========================================================================= */

/**
 * @brief Binarni kod preloaderu (loader_turbo z mzftools, 128 bajtu).
 *
 * Preloader je MZF hlavicka s fsize=0 a Z80 kodem v komentarove oblasti.
 * ROM ho nacte standardnim NORMAL FM 1:1, IPL mechanismus spusti kod
 * v komentari, ktery modifikuje readpoint a vola ROM cteci rutinu
 * pro nacteni FSK loaderu.
 */
static const uint8_t g_fsk_preloader[128] = {
    0x01,0x3E,0x11,0x09,0x11,0x0B,0x11,0x0D,0x11,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,
    0x0D,0x0D,0x0D,0x00,0x00,0x00,0x12,0x10,0x11,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x3E,0x08,0xD3,0xCE,0xCD,0x3E,0x07,0x36,0x01,0x97,0x57,0x5F,0xCD,
    0xBE,0x02,0xD3,0xE2,0x1A,0xD3,0xE0,0x12,0x13,0xCB,0x62,0x28,0xF5,0x3E,0xC3,
    0x32,0x1F,0x06,0x21,0x60,0x11,0x22,0x20,0x06,0x21,0x12,0x05,0x36,0x01,0x21,
    0x4B,0x0A,0x36,0x00,0x2A,0x09,0x11,0x22,0x02,0x11,0x3E,0xC9,0x32,0x9F,0x06,
    0x32,0x00,0x07,0xCD,0xF8,0x04,0x01,0xCF,0x06,0xED,0x71,0xD3,0xE2,0xDA,0xAA,
    0xE9,0x21,0x09,0x11,0xC3,0x08,0xED,0xC5,0x3A,0x10,0x11,0xEE,0x0F,0x32,0x10,
    0x11,0x01,0xCF,0x06,0xED,0x79,0xC1,0xC9
};


/** @brief FSK loader - Z80 telo (191 bajtu). */
static const uint8_t g_fsk_loader_body[191] = {
    0x3E,0x08,0xD3,0xCE,0x21,0x00,0x00,0x22,0x02,0x11,0x21,0x00,0x00,0x22,0x06,
    0x11,0xD3,0xE0,0x21,0x00,0xD4,0x11,0x55,0x01,0x01,0xB5,0x00,0xED,0xB0,0xCD,
    0x7E,0x01,0x21,0x00,0x00,0xD9,0xD3,0xE4,0xC3,0x9A,0xE9,0x11,0x00,0x12,0xD9,
    0x01,0xCF,0x06,0xD9,0x08,0xAF,0x08,0xF3,0x21,0x02,0xE0,0x01,0xC2,0x10,0xAF,
    0x3C,0xCB,0x6E,0xCA,0x91,0x01,0x85,0xCB,0x6E,0xC2,0x97,0x01,0xFE,0x52,0xCB,
    0x10,0xFE,0x36,0x30,0xEB,0x04,0x20,0xE8,0xFE,0x22,0x79,0x30,0x12,0xCB,0x6E,
    0xCA,0xAD,0x01,0xCB,0x6E,0xC2,0xB2,0x01,0xAF,0x95,0x47,0x4F,0x7C,0xC3,0xEA,
    0x01,0xCB,0x6E,0xCA,0xBF,0x01,0xEE,0x08,0x32,0xE2,0x01,0x32,0xB4,0x01,0xEE,
    0x08,0x32,0xED,0x01,0x32,0xAF,0x01,0xC3,0xAD,0x01,0x08,0xA8,0x08,0xEB,0x70,
    0xEB,0x13,0x41,0x3D,0xCB,0x6E,0xC2,0xDF,0x01,0xFE,0x00,0x7C,0xCB,0x10,0x3D,
    0xCB,0x6E,0xCA,0xEA,0x01,0xD2,0xD7,0x01,0xD9,0xED,0x79,0x00,0x00,0xD9,0xFE,
    0xC0,0xD2,0xDF,0x01,0xFB,0xD9,0xAF,0xED,0x79,0x08,0xFE,0x00,0x28,0x01,0x37,
    0xC9,0xB0,0xD4,0x91,0xD4,0x05,0xD4,0x21,0xD4,0x0B,0xD4
};


/* =========================================================================
 *  Patch offsety pro preloader a FSK loader
 * ========================================================================= */

/** @brief Offset fsize v comment casti preloaderu (od bajtu 24). */
#define FSK_PRELOADER_OFF_SIZE    1
/** @brief Offset fstrt v comment casti preloaderu (od bajtu 24). */
#define FSK_PRELOADER_OFF_FROM    3
/** @brief Offset fexec v comment casti preloaderu (od bajtu 24). */
#define FSK_PRELOADER_OFF_EXEC   5
/** @brief Offset readpoint delay v comment casti preloaderu (od bajtu 24). */
#define FSK_PRELOADER_OFF_DELAY  54

/** @brief Offset fsize v body FSK loaderu (191B). */
#define FSK_LOADER_OFF_SIZE       5
/** @brief Offset fexec v body FSK loaderu (191B). */
#define FSK_LOADER_OFF_EXEC       11
/** @brief Offset fstrt v body FSK loaderu (191B). */
#define FSK_LOADER_OFF_FROM       33
/** @brief Offset readpoint delay v body FSK loaderu (191B). */
#define FSK_LOADER_OFF_DELAY      145
/** @brief Offset XOR checksum v body FSK loaderu (191B). */
#define FSK_LOADER_OFF_CHECKSUM   176

/** @brief Velikost body FSK loaderu v bajtech. */
#define FSK_LOADER_BODY_SIZE  191
/** @brief Startovni adresa FSK loaderu v pameti Z80. */
#define FSK_LOADER_FSTRT      0xD400
/** @brief Exec adresa FSK loaderu v pameti Z80. */
#define FSK_LOADER_FEXEC      0xD400


/* =========================================================================
 *  Delay tabulky pro ROM a FSK loader
 * ========================================================================= */

/** @brief ROM delay tabulka pro 44100 Hz (speed 0-6, pro nacteni FSK loaderu). */
static const uint8_t g_rom_delay_44k[] = { 76, 29, 17, 9, 4, 2, 1 };
/** @brief ROM delay tabulka pro 48000 Hz (speed 0-6, pro nacteni FSK loaderu). */
static const uint8_t g_rom_delay_48k[] = { 76, 29, 17, 7, 2, 1, 1 };

/** @brief FSK delay tabulka pro 44100 Hz (speed 0-6, pro nacteni dat ve FSK). */
static const uint8_t g_fsk_delay_44k[] = { 0xD0, 0xD1, 0xD3, 0xD5, 0xD8, 0xDA, 0xDB };
/** @brief FSK delay tabulka pro 48000 Hz (speed 0-6, pro nacteni dat ve FSK). */
static const uint8_t g_fsk_delay_48k[] = { 0xCF, 0xD0, 0xD2, 0xD4, 0xD7, 0xD9, 0xDA };


/**
 * @brief Vrati ROM delay pro danou rychlost a vzorkovaci frekvenci.
 *
 * Delay urcuje prodlevu v ROM cteci smycce pri nacitani FSK loaderu.
 * Pro frekvence >= 46050 Hz pouziva tabulku pro 48000 Hz, jinak 44100 Hz.
 *
 * @param speed Rychlost ROM (0-6). Hodnoty > 6 se oriznou na 6.
 * @param rate Vzorkovaci frekvence (Hz).
 * @return Hodnota delay pro patchovani preloaderu.
 */
static uint8_t fsk_tape_get_rom_delay ( uint8_t speed, uint32_t rate ) {
    if ( speed > 6 ) speed = 6;
    if ( rate >= 46050 ) return g_rom_delay_48k[speed];
    return g_rom_delay_44k[speed];
}


/**
 * @brief Vrati FSK delay pro danou rychlost a vzorkovaci frekvenci.
 *
 * Delay urcuje casovaci konstantu FSK dekoderu pri cteni uzivatelskeho
 * datoveho bloku. Pro frekvence >= 46050 Hz pouziva tabulku pro 48000 Hz.
 *
 * @param speed Rychlost FSK (0-6). Hodnoty > 6 se oriznou na 6.
 * @param rate Vzorkovaci frekvence (Hz).
 * @return Hodnota delay pro patchovani FSK loaderu.
 */
static uint8_t fsk_tape_get_fsk_delay ( uint8_t speed, uint32_t rate ) {
    if ( speed > 6 ) speed = 6;
    if ( rate >= 46050 ) return g_fsk_delay_48k[speed];
    return g_fsk_delay_44k[speed];
}


/* =========================================================================
 *  NORMAL FM pulzni struktury a tabulky pro tape cast
 * ========================================================================= */

/** @brief Dvojice delek pulzu (high + low) v sekundach. */
typedef struct st_fsk_tape_pulse_length {
    double high; /**< delka HIGH casti pulzu (s) */
    double low;  /**< delka LOW casti pulzu (s) */
} st_fsk_tape_pulse_length;

/** @brief Par delek pro long a short pulz. */
typedef struct st_fsk_tape_pulses_length {
    st_fsk_tape_pulse_length long_pulse;  /**< delka long pulzu (bit "1") */
    st_fsk_tape_pulse_length short_pulse; /**< delka short pulzu (bit "0") */
} st_fsk_tape_pulses_length;

/** @brief MZ-700 pulzy. */
static const st_fsk_tape_pulses_length g_tape_pulses_700 = {
    { 0.000464, 0.000494 },
    { 0.000240, 0.000264 },
};

/** @brief MZ-800 pulzy (Intercopy mereni). */
static const st_fsk_tape_pulses_length g_tape_pulses_800 = {
    { 0.000470330, 0.000494308 },
    { 0.000245802, 0.000278204 },
};

/** @brief MZ-80B pulzy. */
static const st_fsk_tape_pulses_length g_tape_pulses_80B = {
    { 0.000333, 0.000334 },
    { 0.000166750, 0.000166 },
};

/** @brief Pole ukazatelu na pulzni sady indexovane en_MZCMT_FSK_PULSESET. */
static const st_fsk_tape_pulses_length *g_tape_pulses[] = {
    &g_tape_pulses_700,
    &g_tape_pulses_800,
    &g_tape_pulses_80B,
};


/** @brief Pocet vzorku jednoho NORMAL FM pulzu (skalovany na cilovy rate). */
typedef struct st_fsk_tape_pulse_samples {
    uint32_t high; /**< pocet HIGH vzorku */
    uint32_t low;  /**< pocet LOW vzorku */
} st_fsk_tape_pulse_samples;

/** @brief Pocty vzorku pro long a short pulz (skalovane). */
typedef struct st_fsk_tape_pulses_samples {
    st_fsk_tape_pulse_samples long_pulse;  /**< skalovany long pulz */
    st_fsk_tape_pulse_samples short_pulse; /**< skalovany short pulz */
} st_fsk_tape_pulses_samples;


/**
 * @brief Pripravi skalovane NORMAL FM pulzy z pulsesetu a divisoru.
 *
 * Pouziva vychozi casovani z dane pulzni sady, skalovane divisorem
 * (pro vyssi rychlosti se pulzy zkracuji). Minimalni hodnota je 1 vzorek.
 *
 * @param[out] pulses Vystupni skalovane pulzy.
 * @param pulseset Pulzni sada (700/800/80B).
 * @param divisor Delitel rychlosti (1.0 = standardni, vyssi = rychlejsi).
 * @param rate Cilovy sample rate (Hz).
 */
static void fsk_tape_prepare_pulses ( st_fsk_tape_pulses_samples *pulses, en_MZCMT_FSK_PULSESET pulseset, double divisor, uint32_t rate ) {
    const st_fsk_tape_pulses_length *src = g_tape_pulses[pulseset];

    pulses->long_pulse.high = ( uint32_t ) round ( src->long_pulse.high * rate / divisor );
    pulses->long_pulse.low = ( uint32_t ) round ( src->long_pulse.low * rate / divisor );
    pulses->short_pulse.high = ( uint32_t ) round ( src->short_pulse.high * rate / divisor );
    pulses->short_pulse.low = ( uint32_t ) round ( src->short_pulse.low * rate / divisor );

    if ( pulses->long_pulse.high < 1 ) pulses->long_pulse.high = 1;
    if ( pulses->long_pulse.low < 1 ) pulses->long_pulse.low = 1;
    if ( pulses->short_pulse.high < 1 ) pulses->short_pulse.high = 1;
    if ( pulses->short_pulse.low < 1 ) pulses->short_pulse.low = 1;
}


/**
 * @brief Prida blok identickych NORMAL FM pulzu do vstreamu.
 *
 * Kazdy pulz se sklada z HIGH + LOW casti (shodne s ROM formatem).
 *
 * @param vstream Cilovy vstream.
 * @param pulse Skalovane pulzni sirky.
 * @param count Pocet pulzu k pridani.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_tape_add_pulses ( st_CMT_VSTREAM *vstream, const st_fsk_tape_pulse_samples *pulse, uint32_t count ) {
    uint32_t i;
    for ( i = 0; i < count; i++ ) {
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulse->high ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulse->low ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida tapemark (long_count long + short_count short pulzu).
 *
 * Tapemark je synchronizacni sekvence na zacatku bloku. Dlouhy tapemark
 * (40+40) uvozuje hlavickovy blok, kratky tapemark (20+20) body blok.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param long_count Pocet long pulzu.
 * @param short_count Pocet short pulzu.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_tape_add_tapemark ( st_CMT_VSTREAM *vstream, const st_fsk_tape_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje datovy blok NORMAL FM modulaci (MSB first, stop bit).
 *
 * Kazdy bajt se koduje jako 8 datovych bitu + 1 stop bit (long pulz).
 * Bit "1" = long pulz, bit "0" = short pulz. Poradi od MSB k LSB.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_tape_encode_data ( st_CMT_VSTREAM *vstream, const st_fsk_tape_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            const st_fsk_tape_pulse_samples *pulse = ( byte & 0x80 )
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
 * @brief Zakoduje 2B checksum (big-endian) do vstreamu NORMAL FM modulaci.
 *
 * Checksum se koduje jako 2 bajty v big-endian poradi (horni bajt prvni),
 * shodne s ROM formatem Sharp MZ.
 *
 * @param vstream Cilovy vstream.
 * @param pulses Skalovane pulzni sirky.
 * @param checksum 16-bitovy checksum.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int fsk_tape_encode_checksum ( st_CMT_VSTREAM *vstream, const st_fsk_tape_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be = endianity_bswap16_BE ( checksum );
    return fsk_tape_encode_data ( vstream, pulses, ( const uint8_t* ) &be, 2 );
}


/**
 * @brief Spocita NORMAL FM checksum (pocet jednickovych bitu).
 *
 * Shodny algoritmus s ROM a mzcmt_fastipl_compute_checksum() -
 * pocet jednickovych bitu ve vsech bajtech datoveho bloku.
 *
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return Pocet jednickovych bitu. Vraci 0 pro NULL nebo size==0.
 */
static uint16_t fsk_tape_compute_ones_checksum ( const uint8_t *data, uint16_t size ) {
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


/* =========================================================================
 *  Verejne tape API
 * ========================================================================= */

/**
 * @brief Vytvori CMT vstream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Generuje trojdilny signal:
 * 1. Preloader header v NORMAL FM 1:1 (ROM ho nacte standardne, fsize=0)
 * 2. FSK loader body v NORMAL FM pri loader_speed (preloader ho nacte)
 * 3. Uzivatelska data v FSK formatu (FSK loader je nacte)
 *
 * Preloader je patchovan s parametry FSK loaderu (velikost, adresy, delay).
 * FSK loader je patchovan s parametry uzivatelskeho programu (velikost,
 * adresy, FSK delay, XOR checksum).
 *
 * Signal zacina v HIGH stavu (shodne s ROM formatem).
 *
 * @param original  Originalni MZF hlavicka (zdrojovy program).
 * @param body      Ukazatel na datove telo k zakodovani.
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace (pulseset, speed, loader_speed).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy vstream s kompletnim signalem, nebo NULL pri chybe.
 */
st_CMT_VSTREAM* mzcmt_fsk_create_tape_vstream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_FSK_TAPE_CONFIG *config,
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

    if ( config->pulseset >= MZCMT_FSK_PULSESET_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid pulseset %d\n", config->pulseset );
        return NULL;
    }

    if ( config->speed >= MZCMT_FSK_SPEED_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid FSK speed %d\n", config->speed );
        return NULL;
    }

    if ( config->loader_speed > 6 ) {
        g_error_cb ( __func__, __LINE__, "Invalid loader speed %d (max 6)\n", config->loader_speed );
        return NULL;
    }

    if ( rate == 0 ) {
        g_error_cb ( __func__, __LINE__, "Sample rate is 0\n" );
        return NULL;
    }

    /* ===== A. Priprava patchovaneho preloaderu ===== */

    uint8_t preloader[128];
    memcpy ( preloader, g_fsk_preloader, 128 );

    /* kopirovat fname z originalu */
    memcpy ( &preloader[1], &original->fname, sizeof ( original->fname ) );

    /* patch comment cast (od bajtu 24): fsize, fstrt, fexec FSK loaderu */
    uint16_t loader_size_le = FSK_LOADER_BODY_SIZE;
    uint16_t loader_from_le = FSK_LOADER_FSTRT;
    uint16_t loader_exec_le = FSK_LOADER_FEXEC;
    memcpy ( &preloader[24 + FSK_PRELOADER_OFF_SIZE], &loader_size_le, 2 );
    memcpy ( &preloader[24 + FSK_PRELOADER_OFF_FROM], &loader_from_le, 2 );
    memcpy ( &preloader[24 + FSK_PRELOADER_OFF_EXEC], &loader_exec_le, 2 );

    /* patch delay pro ROM speed (rychlost nacteni FSK loaderu) */
    preloader[24 + FSK_PRELOADER_OFF_DELAY] = fsk_tape_get_rom_delay ( config->loader_speed, rate );

    /* ===== B. Priprava patchovaneho FSK loader body ===== */

    uint8_t fsk_loader[FSK_LOADER_BODY_SIZE];
    memcpy ( fsk_loader, g_fsk_loader_body, FSK_LOADER_BODY_SIZE );

    /* patch: uzivatelska velikost, adresy */
    uint16_t user_size = ( uint16_t ) body_size;
    uint16_t user_from = original->fstrt;
    uint16_t user_exec = original->fexec;
    memcpy ( &fsk_loader[FSK_LOADER_OFF_SIZE], &user_size, 2 );
    memcpy ( &fsk_loader[FSK_LOADER_OFF_FROM], &user_from, 2 );
    memcpy ( &fsk_loader[FSK_LOADER_OFF_EXEC], &user_exec, 2 );

    /* patch: FSK delay */
    fsk_loader[FSK_LOADER_OFF_DELAY] = fsk_tape_get_fsk_delay ( config->speed, rate );

    /* patch: XOR checksum uzivatelskeho body */
    fsk_loader[FSK_LOADER_OFF_CHECKSUM] = mzcmt_fsk_compute_checksum ( body, body_size );

    /* ===== C. Pulzy pro NORMAL FM ===== */

    /* pulzy pro preloader header (1:1 rychlost) */
    st_fsk_tape_pulses_samples hdr_pulses;
    fsk_tape_prepare_pulses ( &hdr_pulses, config->pulseset, 1.0, rate );

    /* pulzy pro loader body (loader_speed) */
    st_fsk_tape_pulses_samples ldr_pulses;
    if ( config->loader_speed == 0 ) {
        /* standardni ROM rychlost = 1:1 */
        fsk_tape_prepare_pulses ( &ldr_pulses, config->pulseset, 1.0, rate );
    } else {
        /* loader_speed 1-6 mapovat na cmtspeed enum */
        en_CMTSPEED ldr_cmtspeed = ( en_CMTSPEED ) ( config->loader_speed + 1 );
        if ( !cmtspeed_is_valid ( ldr_cmtspeed ) ) ldr_cmtspeed = CMTSPEED_1_1;
        double ldr_divisor = cmtspeed_get_divisor ( ldr_cmtspeed );
        fsk_tape_prepare_pulses ( &ldr_pulses, config->pulseset, ldr_divisor, rate );
    }

    /* ===== D. Checksums ===== */

    uint16_t chk_preloader_hdr = fsk_tape_compute_ones_checksum ( preloader, 128 );
    uint16_t chk_loader_body = fsk_tape_compute_ones_checksum ( fsk_loader, FSK_LOADER_BODY_SIZE );

    /* ===== E. GAP delky ===== */

    /* loader body SGAP dle loader_speed */
    uint32_t ldr_sgap;
    if ( config->loader_speed == 0 )
        ldr_sgap = 4000;
    else
        ldr_sgap = 2000 * ( config->loader_speed + 1 );

    /* ===== F. Vytvoreni vstreamu ===== */

    /* signal zacina v HIGH stavu (shodne s ROM formatem a mztape) */
    st_CMT_VSTREAM *vstream = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, 1, CMT_STREAM_POLARITY_NORMAL );
    if ( !vstream ) {
        g_error_cb ( __func__, __LINE__, "Can't create vstream\n" );
        return NULL;
    }

    /* ----- Cast 1: Preloader header v NORMAL 1:1 ----- */

    /* LGAP (22000 kratkych pulzu) */
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &hdr_pulses.short_pulse, 22000 ) ) goto error;

    /* dlouhy tapemark (40 long + 40 short) */
    if ( EXIT_SUCCESS != fsk_tape_add_tapemark ( vstream, &hdr_pulses, 40, 40 ) ) goto error;

    /* 2 long + header(128B) + checksum + 2 long */
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_encode_data ( vstream, &hdr_pulses, preloader, 128 ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_encode_checksum ( vstream, &hdr_pulses, chk_preloader_hdr ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /* protoze fsize=0, body blok preloaderu se neposila (ROM ho necte) */
    /* preloader modifikuje ROM: nastavi size/from/exec a readpoint, pak vola
       ROM body-cteci rutinu - ta cte primo nasledujici body blok z pasky */

    /* ----- Cast 2: FSK loader body v NORMAL pri loader_speed ----- */
    /* FSK loader header se na pasku NEPISE - preloader patchuje ROM primo
       s parametry loaderu (size=191, from=$D400, exec=$D400) a cte jen body */

    /* SGAP */
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &ldr_pulses.short_pulse, ldr_sgap ) ) goto error;

    /* kratky tapemark (20 long + 20 short) */
    if ( EXIT_SUCCESS != fsk_tape_add_tapemark ( vstream, &ldr_pulses, 20, 20 ) ) goto error;

    /* 2 long + body(191B) + checksum + 2 long */
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_encode_data ( vstream, &ldr_pulses, fsk_loader, FSK_LOADER_BODY_SIZE ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_encode_checksum ( vstream, &ldr_pulses, chk_loader_body ) ) goto error;
    if ( EXIT_SUCCESS != fsk_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;

    /* ----- Cast 3: Uzivatelska data v FSK formatu ----- */

    if ( body_size > 0 ) {
        /* priprava skalovanych FSK pulznich sirek */
        st_fsk_pulses fsk_data_pulses;
        fsk_prepare_pulses ( &fsk_data_pulses, config->speed, rate );

        /* GAP (leader ton) */
        if ( EXIT_SUCCESS != fsk_add_gap ( vstream, rate ) ) goto error;

        /* polaritni pulz */
        if ( EXIT_SUCCESS != fsk_add_polarity ( vstream, rate ) ) goto error;

        /* sync kod */
        if ( EXIT_SUCCESS != fsk_add_sync ( vstream, rate ) ) goto error;

        /* datove bajty */
        uint32_t i;
        for ( i = 0; i < body_size; i++ ) {
            if ( EXIT_SUCCESS != fsk_encode_byte ( vstream, body[i], &fsk_data_pulses ) ) goto error;
        }

        /* fade-out */
        if ( EXIT_SUCCESS != fsk_add_fadeout ( vstream, rate ) ) goto error;
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during FSK tape signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Obaluje mzcmt_fsk_create_tape_vstream() do polymorfniho st_CMT_STREAM.
 * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
 *
 * @param original  Originalni MZF hlavicka (zdrojovy program).
 * @param body      Ukazatel na datove telo k zakodovani.
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace (pulseset, speed, loader_speed).
 * @param type      Typ vystupniho streamu (bitstream/vstream).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy stream, nebo NULL pri chybe.
 */
st_CMT_STREAM* mzcmt_fsk_create_tape_stream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_FSK_TAPE_CONFIG *config,
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
            st_CMT_VSTREAM *vstream = mzcmt_fsk_create_tape_vstream ( original, body, body_size, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_fsk_create_tape_vstream ( original, body, body_size, config, rate );
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


const char* mzcmt_fsk_version ( void ) {
    return MZCMT_FSK_VERSION;
}
