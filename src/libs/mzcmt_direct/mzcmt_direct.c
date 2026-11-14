/**
 * @file   mzcmt_direct.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace DIRECT koderu pro Sharp MZ (primy bitovy zapis).
 *
 * DIRECT je nejrychlejsi metoda prenosu dat na kazetach Sharp MZ.
 * Kazdy datovy bit se zapise primo jako jeden audio vzorek (HIGH/LOW)
 * bez frekvencni modulace. Po kazdem druhem datovem bitu nasleduje
 * synchronizacni bit (opacna hodnota posledniho datoveho bitu), ktery
 * dekoderu umoznuje resynchronizaci timingu.
 *
 * @par Kodovani jednoho bajtu (12 vzorku):
 * @verbatim
 *   bit index:  0   1   S   2   3   S   4   5   S   6   7   S
 *   obsah:      D7  D6 ~D6  D5  D4 ~D4  D3  D2 ~D2  D1  D0 ~D0
 * @endverbatim
 * kde S = synchro bit (opak posledniho datoveho bitu).
 *
 * @par Rozdil od FSK a SLOW:
 * FSK a SLOW kodery pouzivaji pulzy ruznych delek (LOW+HIGH cyklus)
 * pro kodovani 1 resp. 2 bitu. DIRECT zapisuje kazdy bit jako
 * samostatny vzorek - to eliminuje frekvencni modulaci a dosahuje
 * nejvyssi mozne prenosove rychlosti (omezene pouze vzorkovaci
 * frekvenci a schopnosti Z80 dekoderu).
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
#include <string.h>
#include <math.h>

#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmtspeed/cmtspeed.h"
#include "libs/endianity/endianity.h"

#include "mzcmt_direct.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* mzcmt_direct_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* mzcmt_direct_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  mzcmt_direct_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_MZCMT_DIRECT_ALLOCATOR g_mzcmt_direct_default_allocator = {
    mzcmt_direct_default_alloc,
    mzcmt_direct_default_alloc0,
    mzcmt_direct_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_MZCMT_DIRECT_ALLOCATOR *g_allocator = &g_mzcmt_direct_default_allocator;


/** @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL. */
void mzcmt_direct_set_allocator ( const st_MZCMT_DIRECT_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_mzcmt_direct_default_allocator;
}


/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void mzcmt_direct_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static mzcmt_direct_error_cb g_error_cb = mzcmt_direct_default_error_cb;


/** @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL. */
void mzcmt_direct_set_error_callback ( mzcmt_direct_error_cb cb ) {
    g_error_cb = cb ? cb : mzcmt_direct_default_error_cb;
}


/* =========================================================================
 *  Skalovani pro frame strukturu
 * ========================================================================= */

/**
 * @brief Skaluje referencni pocet vzorku na cilovy sample rate.
 *
 * Pouziva proporcionalni skalovani: result = round(ref * rate / 44100).
 * Minimalni navratova hodnota je 1 (zarucuje platny vstream event).
 *
 * Toto skalovani se pouziva pouze pro frame strukturu (GAP, polarity,
 * sync, fadeout). Datove a synchro bity se neskaluji - jsou vzdy 1 vzorek.
 *
 * @param ref_samples Pocet vzorku pri referencni frekvenci 44100 Hz.
 * @param rate Cilova vzorkovaci frekvence (Hz).
 * @return Skalovany pocet vzorku (minimalne 1).
 */
static inline uint32_t direct_scale ( uint32_t ref_samples, uint32_t rate ) {
    uint32_t s = ( uint32_t ) round ( ( double ) ref_samples * rate / MZCMT_DIRECT_REF_RATE );
    return s > 0 ? s : 1;
}


/* =========================================================================
 *  Interni pomocne funkce pro generovani signalu
 * ========================================================================= */

/**
 * @brief Prida GAP (leader) sekvenci do vstreamu.
 *
 * GAP slouzi jako leader ton pro synchronizaci DIRECT dekoderu na Z80.
 * Dekoder detekuje pilot ton ctenim portu $E002 bit 5, meri delku
 * pulzu a kalibruje prahy pro detekci sync sekvence.
 *
 * Generuje 200 cyklu, kazdy cyklus = 8 LOW + 8 HIGH vzorku
 * (pri ref. frekvenci, skalovano na cilovy rate).
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int direct_add_gap ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = direct_scale ( MZCMT_DIRECT_GAP_LOW_REF, rate );
    uint32_t high = direct_scale ( MZCMT_DIRECT_GAP_HIGH_REF, rate );
    int i;
    for ( i = 0; i < MZCMT_DIRECT_GAP_CYCLES; i++ ) {
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
 * modifikuje skokove instrukce (JP Z / JP NZ) pro spravne
 * rozpoznavani hran v datovem toku.
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu po GAP).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int direct_add_polarity ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = direct_scale ( MZCMT_DIRECT_POLARITY_LOW_REF, rate );
    uint32_t high = direct_scale ( MZCMT_DIRECT_POLARITY_HIGH_REF, rate );
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
static int direct_add_sync ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = direct_scale ( MZCMT_DIRECT_SYNC_LOW_REF, rate );
    uint32_t high = direct_scale ( MZCMT_DIRECT_SYNC_HIGH_REF, rate );
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje jeden datovy bajt do vstreamu DIRECT kodovanim.
 *
 * Bajt se koduje od MSB k LSB. Pro kazdy bit se zapise 1 vzorek
 * (HIGH pro bit=1, LOW pro bit=0). Po kazdem sudem bitu (indexy 1, 3, 5, 7)
 * nasleduje synchronizacni bit s opacnou hodnotou.
 *
 * Sekvence pro bajt B = b7 b6 b5 b4 b3 b2 b1 b0:
 * @verbatim
 *   b7  b6  ~b6  b5  b4  ~b4  b3  b2  ~b2  b1  b0  ~b0
 * @endverbatim
 *
 * Celkem 12 vzorku na bajt (8 datovych + 4 synchronizacnich).
 *
 * Vstream automaticky spojuje po sobe jdouci vzorky se stejnou
 * hodnotou do jednoho eventu (RLE), takze neni nutne rucne bufferovat.
 *
 * @param vstream Cilovy vstream.
 * @param byte Bajt k zakodovani.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int direct_encode_byte ( st_CMT_VSTREAM *vstream, uint8_t byte ) {
    int i;
    for ( i = 0; i < 8; i++ ) {
        int bit = ( byte >> ( 7 - i ) ) & 0x01;

        /* datovy bit */
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, bit, 1 ) ) return EXIT_FAILURE;

        /* synchro bit po kazdem sudem bitu (indexy 1, 3, 5, 7) */
        if ( ( i % 2 ) == 1 ) {
            if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, !bit, 1 ) ) return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida fade-out sekvenci do vstreamu.
 *
 * Postupne prodluzujici se pulzy (od 32 do 46 vzorku pri ref. frekvenci,
 * s krokem 2). Dekoder na Z80 detekuje konec datoveho toku podle extremne
 * dlouheho intervalu mezi prechodymi signalu a ukonci prijem.
 *
 * @param vstream Cilovy vstream.
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int direct_add_fadeout ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    int w;
    for ( w = MZCMT_DIRECT_FADEOUT_START_REF; w < MZCMT_DIRECT_FADEOUT_END_REF; w += MZCMT_DIRECT_FADEOUT_STEP_REF ) {
        uint32_t s = direct_scale ( ( uint32_t ) w, rate );
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, s ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, s ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

/**
 * @brief Vytvori CMT vstream z datoveho bloku DIRECT kodovanim.
 *
 * Generuje kompletni DIRECT signal:
 * 1. GAP (200 cyklu leader tonu pro synchronizaci dekoderu)
 * 2. Polaritni pulz (detekce polarity signalu)
 * 3. Sync kod (oznaceni zacatku dat)
 * 4. Datove bajty (12 vzorku na bajt: 8 dat + 4 synchro, MSB first)
 * 5. Fade-out (rostouci pulzy pro detekci konce streamu)
 *
 * Signal zacina v LOW stavu. Frame struktura (GAP, polarity, sync, fadeout)
 * je shodna s FSK a SLOW kodery a skaluje se z referencni frekvence 44100 Hz
 * na cilovy @p rate. Datove a synchro bity jsou vzdy 1 vzorek.
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param rate      Vzorkovaci frekvence vystupniho vstreamu (Hz).
 * @return Ukazatel na novy vstream, nebo NULL pri chybe.
 *         Volajici vlastni vstream a musi jej uvolnit pres cmt_vstream_destroy().
 */
st_CMT_VSTREAM* mzcmt_direct_create_vstream (
    const uint8_t *data,
    uint32_t data_size,
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

    /* 1. GAP (leader ton) */
    if ( EXIT_SUCCESS != direct_add_gap ( vstream, rate ) ) goto error;

    /* 2. polaritni pulz */
    if ( EXIT_SUCCESS != direct_add_polarity ( vstream, rate ) ) goto error;

    /* 3. sync kod */
    if ( EXIT_SUCCESS != direct_add_sync ( vstream, rate ) ) goto error;

    /* 4. datove bajty */
    uint32_t i;
    for ( i = 0; i < data_size; i++ ) {
        if ( EXIT_SUCCESS != direct_encode_byte ( vstream, data[i] ) ) goto error;
    }

    /* 5. fade-out */
    if ( EXIT_SUCCESS != direct_add_fadeout ( vstream, rate ) ) goto error;

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during DIRECT signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z datoveho bloku DIRECT kodovanim.
 *
 * Pro typ CMT_STREAM_TYPE_VSTREAM primo pouzije mzcmt_direct_create_vstream().
 * Pro typ CMT_STREAM_TYPE_BITSTREAM vytvori vstream a konvertuje jej na
 * bitstream (presnejsi nez prima generace bitstreamu - viz mztape princip).
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param type      Typ vystupniho streamu (bitstream/vstream).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Ukazatel na novy stream, nebo NULL pri chybe.
 *         Volajici vlastni stream a musi jej uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* mzcmt_direct_create_stream (
    const uint8_t *data,
    uint32_t data_size,
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
            st_CMT_VSTREAM *vstream = mzcmt_direct_create_vstream ( data, data_size, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_direct_create_vstream ( data, data_size, rate );
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
 * DIRECT dekoder na Z80 pouziva alternativni akumulator (A') pro
 * prubezny XOR checksum. Po kazdem prijmutem bajtu provede:
 *   ex af,af'
 *   xor c
 *   ex af,af'
 * Po prijeti vsech bajtu porovnava vysledek s predpocitanou hodnotou
 * na adrese checksumx (instrukce CP $xx).
 *
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
 */
uint8_t mzcmt_direct_compute_checksum ( const uint8_t *data, uint32_t size ) {
    if ( !data || size == 0 ) return 0;
    uint8_t cs = 0;
    uint32_t i;
    for ( i = 0; i < size; i++ ) {
        cs ^= data[i];
    }
    return cs;
}


/* =========================================================================
 *  Embedded loader binarky pro DIRECT tape
 * ========================================================================= */

/**
 * @brief Binarni kod preloaderu (loader_turbo z mzftools, 128 bajtu).
 *
 * Preloader je MZF hlavicka s fsize=0 a Z80 kodem v komentarove oblasti.
 * ROM ho nacte standardnim NORMAL FM 1:1, IPL mechanismus spusti kod
 * v komentari, ktery modifikuje readpoint a vola ROM cteci rutinu
 * pro nacteni DIRECT loaderu.
 */
static const uint8_t g_direct_preloader[128] = {
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


/**
 * @brief DIRECT loader - Z80 telo (360 bajtu).
 *
 * Druha cast loader_direct z mzftools (bajty 128-487).
 * Obsahuje Z80 kod pro primy bitovy prijem dat z kazetoveho portu.
 * Dekoder cte signal primo z PPI portu ($E002, bit 5), kazdy datovy
 * element je 1 audio vzorek. Po kazdem druhem bitu se resynchronizuje
 * pomoci synchro bitu.
 */
static const uint8_t g_direct_loader_body[360] = {
    0x3E,0x08,0xD3,0xCE,0x21,0x00,0x00,0x22,0x02,0x11,0xD9,0x21,0x00,0x00,0x22,
    0x06,0x11,0xD3,0xE0,0x21,0x00,0xD4,0x11,0x55,0x01,0x01,0x5E,0x01,0xED,0xB0,
    0xCD,0x7F,0x01,0x21,0x00,0x00,0xD9,0xD3,0xE4,0xC3,0x9A,0xE9,0x11,0x00,0x12,
    0xD9,0x01,0xCF,0x06,0xD9,0xF3,0x21,0x02,0xE0,0x01,0xC2,0x10,0xAF,0x3C,0xCB,
    0x6E,0xCA,0x8F,0x01,0x85,0xCB,0x6E,0xC2,0x95,0x01,0xFE,0x52,0xCB,0x10,0xFE,
    0x36,0x30,0xEB,0x04,0x20,0xE8,0xFE,0x22,0x79,0x30,0x16,0x06,0x20,0xAF,0x08,
    0xCB,0x6E,0xCA,0xAF,0x01,0x3E,0x0A,0x3D,0xC2,0xB6,0x01,0x00,0x0E,0x00,0x00,
    0xC3,0xE0,0x01,0xCB,0x6E,0xCA,0xC1,0x01,0xEE,0x08,0xEE,0x08,0x32,0xB1,0x01,
    0xC3,0xAB,0x01,0x79,0x12,0x13,0x08,0xA9,0x08,0xD9,0x2B,0x7C,0xB5,0xD9,0xCA,
    0xA2,0x02,0x0E,0x00,0x7E,0xA0,0x07,0x07,0xB1,0x4F,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x7E,0xA0,0xCA,0xFD,0x01,0xCB,0x6E,0xC2,0xF5,0x01,
    0xC3,0x05,0x02,0xCB,0x6E,0xCA,0xFD,0x01,0xC3,0x05,0x02,0x07,0x00,0x00,0x00,
    0xB1,0x4F,0x3E,0x04,0x3D,0xC2,0x0D,0x02,0x00,0x7E,0xA0,0x00,0x00,0x00,0xB1,
    0x4F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0xA0,0xCA,0x2F,0x02,
    0xCB,0x6E,0xC2,0x27,0x02,0xC3,0x37,0x02,0xCB,0x6E,0xCA,0x2F,0x02,0xC3,0x37,
    0x02,0x0F,0x00,0xB1,0x4F,0x3E,0x05,0x3D,0xC2,0x3D,0x02,0x7E,0xA0,0x0F,0x0F,
    0xB1,0x4F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0xA0,0xCA,
    0x5E,0x02,0xCB,0x6E,0xC2,0x56,0x02,0xC3,0x66,0x02,0xCB,0x6E,0xCA,0x5E,0x02,
    0xC3,0x66,0x02,0x0F,0x0F,0x0F,0xB1,0x4F,0x3E,0x04,0x3D,0xC2,0x6D,0x02,0x00,
    0x00,0x7E,0xA0,0x0F,0x0F,0x0F,0x0F,0xB1,0x4F,0xD9,0xED,0x79,0xD9,0x00,0x00,
    0x00,0x00,0x00,0x00,0x7E,0xA0,0xCA,0x92,0x02,0xCB,0x6E,0xC2,0x8A,0x02,0xC3,
    0x9A,0x02,0xCB,0x6E,0xCA,0x92,0x02,0xC3,0x9A,0x02,0x07,0x07,0x07,0xB1,0x4F,
    0xC3,0xD0,0x01,0xD9,0xAF,0xED,0x79,0xD9,0x08,0x37,0x3F,0xFE,0x00,0xCA,0xB0,
    0x02,0x37,0xC9,0x00,0x00,0x56,0xD5,0x5D,0xD5,0x05,0xD4,0x22,0xD4,0x0C,0xD4
};


/* =========================================================================
 *  Patch offsety pro preloader a DIRECT loader
 * ========================================================================= */

/** @brief Offset fsize v comment casti preloaderu (od bajtu 24). */
#define DIRECT_PRELOADER_OFF_SIZE    1
/** @brief Offset fstrt v comment casti preloaderu (od bajtu 24). */
#define DIRECT_PRELOADER_OFF_FROM    3
/** @brief Offset fexec v comment casti preloaderu (od bajtu 24). */
#define DIRECT_PRELOADER_OFF_EXEC    5
/** @brief Offset readpoint delay v comment casti preloaderu (od bajtu 24). */
#define DIRECT_PRELOADER_OFF_DELAY   54

/** @brief Offset fsize v body DIRECT loaderu (360B). */
#define DIRECT_LOADER_OFF_SIZE       5
/** @brief Offset fexec v body DIRECT loaderu (360B). */
#define DIRECT_LOADER_OFF_EXEC       12
/** @brief Offset fstrt v body DIRECT loaderu (360B). */
#define DIRECT_LOADER_OFF_FROM       34
/** @brief Offset readpoint delay v body DIRECT loaderu (360B). */
#define DIRECT_LOADER_OFF_DELAY      349
/** @brief Offset XOR checksum v body DIRECT loaderu (360B). */
#define DIRECT_LOADER_OFF_CHECKSUM   342

/** @brief Velikost body DIRECT loaderu v bajtech. */
#define DIRECT_LOADER_BODY_SIZE  360
/** @brief Startovni adresa DIRECT loaderu v pameti Z80. */
#define DIRECT_LOADER_FSTRT      0xD400
/** @brief Exec adresa DIRECT loaderu v pameti Z80. */
#define DIRECT_LOADER_FEXEC      0xD400


/* =========================================================================
 *  Delay tabulky pro ROM a DIRECT loader
 * ========================================================================= */

/** @brief ROM delay tabulka pro 44100 Hz (speed 0-6, pro nacteni DIRECT loaderu). */
static const uint8_t g_rom_delay_44k[] = { 76, 29, 17, 9, 4, 2, 1 };
/** @brief ROM delay tabulka pro 48000 Hz (speed 0-6, pro nacteni DIRECT loaderu). */
static const uint8_t g_rom_delay_48k[] = { 76, 29, 17, 7, 2, 1, 1 };

/**
 * @brief Konstantni delay hodnota pro DIRECT loader.
 *
 * DIRECT pouziva konstantni delay 0x06 nezavisle na rychlosti a vzorkovaci
 * frekvenci. Timing je dany poctem T-states v instrukci smycce dekoderu
 * a rychlost prenosu je primo umerna vzorkovaci frekvenci.
 */
#define DIRECT_LOADER_DELAY  0x06


/**
 * @brief Vrati ROM delay pro danou rychlost a vzorkovaci frekvenci.
 *
 * Delay urcuje prodlevu v ROM cteci smycce pri nacitani DIRECT loaderu.
 * Pro frekvence >= 46050 Hz pouziva tabulku pro 48000 Hz, jinak 44100 Hz.
 *
 * @param speed Rychlost ROM (0-6). Hodnoty > 6 se oriznou na 6.
 * @param rate Vzorkovaci frekvence (Hz).
 * @return Hodnota delay pro patchovani preloaderu.
 */
static uint8_t direct_tape_get_rom_delay ( uint8_t speed, uint32_t rate ) {
    if ( speed > 6 ) speed = 6;
    if ( rate >= 46050 ) return g_rom_delay_48k[speed];
    return g_rom_delay_44k[speed];
}


/* =========================================================================
 *  NORMAL FM pulzni struktury a tabulky pro tape cast
 * ========================================================================= */

/** @brief Dvojice delek pulzu (high + low) v sekundach. */
typedef struct st_direct_tape_pulse_length {
    double high; /**< delka HIGH casti pulzu (s) */
    double low;  /**< delka LOW casti pulzu (s) */
} st_direct_tape_pulse_length;

/** @brief Par delek pro long a short pulz. */
typedef struct st_direct_tape_pulses_length {
    st_direct_tape_pulse_length long_pulse;  /**< delka long pulzu (bit "1") */
    st_direct_tape_pulse_length short_pulse; /**< delka short pulzu (bit "0") */
} st_direct_tape_pulses_length;

/** @brief MZ-700 pulzy. */
static const st_direct_tape_pulses_length g_tape_pulses_700 = {
    { 0.000464, 0.000494 },
    { 0.000240, 0.000264 },
};

/** @brief MZ-800 pulzy (Intercopy mereni). */
static const st_direct_tape_pulses_length g_tape_pulses_800 = {
    { 0.000470330, 0.000494308 },
    { 0.000245802, 0.000278204 },
};

/** @brief MZ-80B pulzy. */
static const st_direct_tape_pulses_length g_tape_pulses_80B = {
    { 0.000333, 0.000334 },
    { 0.000166750, 0.000166 },
};

/** @brief Pole ukazatelu na pulzni sady indexovane en_MZCMT_DIRECT_PULSESET. */
static const st_direct_tape_pulses_length *g_tape_pulses[] = {
    &g_tape_pulses_700,
    &g_tape_pulses_800,
    &g_tape_pulses_80B,
};


/** @brief Pocet vzorku jednoho NORMAL FM pulzu (skalovany na cilovy rate). */
typedef struct st_direct_tape_pulse_samples {
    uint32_t high; /**< pocet HIGH vzorku */
    uint32_t low;  /**< pocet LOW vzorku */
} st_direct_tape_pulse_samples;

/** @brief Pocty vzorku pro long a short pulz (skalovane). */
typedef struct st_direct_tape_pulses_samples {
    st_direct_tape_pulse_samples long_pulse;  /**< skalovany long pulz */
    st_direct_tape_pulse_samples short_pulse; /**< skalovany short pulz */
} st_direct_tape_pulses_samples;


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
static void direct_tape_prepare_pulses ( st_direct_tape_pulses_samples *pulses, en_MZCMT_DIRECT_PULSESET pulseset, double divisor, uint32_t rate ) {
    const st_direct_tape_pulses_length *src = g_tape_pulses[pulseset];

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
static int direct_tape_add_pulses ( st_CMT_VSTREAM *vstream, const st_direct_tape_pulse_samples *pulse, uint32_t count ) {
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
static int direct_tape_add_tapemark ( st_CMT_VSTREAM *vstream, const st_direct_tape_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
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
static int direct_tape_encode_data ( st_CMT_VSTREAM *vstream, const st_direct_tape_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            const st_direct_tape_pulse_samples *pulse = ( byte & 0x80 )
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
static int direct_tape_encode_checksum ( st_CMT_VSTREAM *vstream, const st_direct_tape_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be = endianity_bswap16_BE ( checksum );
    return direct_tape_encode_data ( vstream, pulses, ( const uint8_t* ) &be, 2 );
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
static uint16_t direct_tape_compute_ones_checksum ( const uint8_t *data, uint16_t size ) {
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
 * 2. DIRECT loader body v NORMAL FM pri loader_speed (preloader ho nacte)
 * 3. Uzivatelska data v DIRECT formatu (DIRECT loader je nacte)
 *
 * Preloader je patchovan s parametry DIRECT loaderu (velikost, adresy, delay).
 * DIRECT loader je patchovan s parametry uzivatelskeho programu (velikost,
 * adresy, delay, XOR checksum).
 *
 * Signal zacina v HIGH stavu (shodne s ROM formatem).
 *
 * @param original  Originalni MZF hlavicka (zdrojovy program).
 * @param body      Ukazatel na datove telo k zakodovani.
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace (pulseset, loader_speed).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy vstream s kompletnim signalem, nebo NULL pri chybe.
 */
st_CMT_VSTREAM* mzcmt_direct_create_tape_vstream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_DIRECT_TAPE_CONFIG *config,
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

    if ( config->pulseset >= MZCMT_DIRECT_PULSESET_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid pulseset %d\n", config->pulseset );
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
    memcpy ( preloader, g_direct_preloader, 128 );

    /* kopirovat fname z originalu */
    memcpy ( &preloader[1], &original->fname, sizeof ( original->fname ) );

    /* patch comment cast (od bajtu 24): fsize, fstrt, fexec DIRECT loaderu */
    uint16_t loader_size_le = DIRECT_LOADER_BODY_SIZE;
    uint16_t loader_from_le = DIRECT_LOADER_FSTRT;
    uint16_t loader_exec_le = DIRECT_LOADER_FEXEC;
    memcpy ( &preloader[24 + DIRECT_PRELOADER_OFF_SIZE], &loader_size_le, 2 );
    memcpy ( &preloader[24 + DIRECT_PRELOADER_OFF_FROM], &loader_from_le, 2 );
    memcpy ( &preloader[24 + DIRECT_PRELOADER_OFF_EXEC], &loader_exec_le, 2 );

    /* patch delay pro ROM speed (rychlost nacteni DIRECT loaderu) */
    preloader[24 + DIRECT_PRELOADER_OFF_DELAY] = direct_tape_get_rom_delay ( config->loader_speed, rate );

    /* ===== B. Priprava patchovaneho DIRECT loader body ===== */

    uint8_t direct_loader[DIRECT_LOADER_BODY_SIZE];
    memcpy ( direct_loader, g_direct_loader_body, DIRECT_LOADER_BODY_SIZE );

    /* patch: uzivatelska velikost, adresy */
    uint16_t user_size = ( uint16_t ) body_size;
    uint16_t user_from = original->fstrt;
    uint16_t user_exec = original->fexec;
    memcpy ( &direct_loader[DIRECT_LOADER_OFF_SIZE], &user_size, 2 );
    memcpy ( &direct_loader[DIRECT_LOADER_OFF_FROM], &user_from, 2 );
    memcpy ( &direct_loader[DIRECT_LOADER_OFF_EXEC], &user_exec, 2 );

    /* patch: DIRECT delay (konstantni 0x06) */
    direct_loader[DIRECT_LOADER_OFF_DELAY] = DIRECT_LOADER_DELAY;

    /* patch: XOR checksum uzivatelskeho body */
    direct_loader[DIRECT_LOADER_OFF_CHECKSUM] = mzcmt_direct_compute_checksum ( body, body_size );

    /* ===== C. Pulzy pro NORMAL FM ===== */

    /* pulzy pro preloader header (1:1 rychlost) */
    st_direct_tape_pulses_samples hdr_pulses;
    direct_tape_prepare_pulses ( &hdr_pulses, config->pulseset, 1.0, rate );

    /* pulzy pro loader body (loader_speed) */
    st_direct_tape_pulses_samples ldr_pulses;
    if ( config->loader_speed == 0 ) {
        /* standardni ROM rychlost = 1:1 */
        direct_tape_prepare_pulses ( &ldr_pulses, config->pulseset, 1.0, rate );
    } else {
        /* loader_speed 1-6 mapovat na cmtspeed enum */
        en_CMTSPEED ldr_cmtspeed = ( en_CMTSPEED ) ( config->loader_speed + 1 );
        if ( !cmtspeed_is_valid ( ldr_cmtspeed ) ) ldr_cmtspeed = CMTSPEED_1_1;
        double ldr_divisor = cmtspeed_get_divisor ( ldr_cmtspeed );
        direct_tape_prepare_pulses ( &ldr_pulses, config->pulseset, ldr_divisor, rate );
    }

    /* ===== D. Checksums ===== */

    uint16_t chk_preloader_hdr = direct_tape_compute_ones_checksum ( preloader, 128 );
    uint16_t chk_loader_body = direct_tape_compute_ones_checksum ( direct_loader, DIRECT_LOADER_BODY_SIZE );

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
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &hdr_pulses.short_pulse, 22000 ) ) goto error;

    /* dlouhy tapemark (40 long + 40 short) */
    if ( EXIT_SUCCESS != direct_tape_add_tapemark ( vstream, &hdr_pulses, 40, 40 ) ) goto error;

    /* 2 long + header(128B) + checksum + 2 long */
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_encode_data ( vstream, &hdr_pulses, preloader, 128 ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_encode_checksum ( vstream, &hdr_pulses, chk_preloader_hdr ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /* protoze fsize=0, body blok preloaderu se neposila (ROM ho necte) */
    /* preloader modifikuje ROM: nastavi size/from/exec a readpoint, pak vola
       ROM body-cteci rutinu - ta cte primo nasledujici body blok z pasky */

    /* ----- Cast 2: DIRECT loader body v NORMAL pri loader_speed ----- */
    /* DIRECT loader header se na pasku NEPISE - preloader patchuje ROM primo
       s parametry loaderu (size=360, from=$D400, exec=$D400) a cte jen body */

    /* SGAP */
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &ldr_pulses.short_pulse, ldr_sgap ) ) goto error;

    /* kratky tapemark (20 long + 20 short) */
    if ( EXIT_SUCCESS != direct_tape_add_tapemark ( vstream, &ldr_pulses, 20, 20 ) ) goto error;

    /* 2 long + body(360B) + checksum + 2 long */
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_encode_data ( vstream, &ldr_pulses, direct_loader, DIRECT_LOADER_BODY_SIZE ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_encode_checksum ( vstream, &ldr_pulses, chk_loader_body ) ) goto error;
    if ( EXIT_SUCCESS != direct_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;

    /* ----- Cast 3: Uzivatelska data v DIRECT formatu ----- */

    if ( body_size > 0 ) {
        /* GAP (leader ton) */
        if ( EXIT_SUCCESS != direct_add_gap ( vstream, rate ) ) goto error;

        /* polaritni pulz */
        if ( EXIT_SUCCESS != direct_add_polarity ( vstream, rate ) ) goto error;

        /* sync kod */
        if ( EXIT_SUCCESS != direct_add_sync ( vstream, rate ) ) goto error;

        /* datove bajty */
        uint32_t i;
        for ( i = 0; i < body_size; i++ ) {
            if ( EXIT_SUCCESS != direct_encode_byte ( vstream, body[i] ) ) goto error;
        }

        /* fade-out */
        if ( EXIT_SUCCESS != direct_add_fadeout ( vstream, rate ) ) goto error;
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during DIRECT tape signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Obaluje mzcmt_direct_create_tape_vstream() do polymorfniho st_CMT_STREAM.
 * Pro typ CMT_STREAM_TYPE_BITSTREAM interno vytvori vstream a konvertuje.
 *
 * @param original  Originalni MZF hlavicka (zdrojovy program).
 * @param body      Ukazatel na datove telo k zakodovani.
 * @param body_size Velikost datoveho tela v bajtech.
 * @param config    Konfigurace (pulseset, loader_speed).
 * @param type      Typ vystupniho streamu (bitstream/vstream).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Novy stream, nebo NULL pri chybe.
 */
st_CMT_STREAM* mzcmt_direct_create_tape_stream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_DIRECT_TAPE_CONFIG *config,
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
            st_CMT_VSTREAM *vstream = mzcmt_direct_create_tape_vstream ( original, body, body_size, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_direct_create_tape_vstream ( original, body, body_size, config, rate );
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


const char* mzcmt_direct_version ( void ) {
    return MZCMT_DIRECT_VERSION;
}
