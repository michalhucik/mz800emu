/**
 * @file   mzcmt_slow.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace SLOW koderu pro Sharp MZ (2 bity na pulz).
 *
 * SLOW modulace koduje 2 bity jednim pulzem. Kazdy ze 4 symbolu
 * (00, 01, 10, 11) ma vlastni delku LOW a HIGH casti. Bajt se koduje
 * ve 4 krocich po 2 bitech (MSB first).
 *
 * Pocty vzorku na pulz zavisi na rychlostni urovni (speed 0-4) a jsou
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
 * @par Referencni tabulky:
 * Prevzato z mzftools coder.c, funkce slow_encode_data_byte().
 * Rychlostni index v tabulce je invertovany: table_index = 4 - speed.
 * Speed 0 = nejpomalejsi (nejdelsi pulzy, tabulkovy radek 4),
 * speed 4 = nejrychlejsi (nejkratsi pulzy, tabulkovy radek 0).
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

#include "mzcmt_slow.h"


/* =========================================================================
 *  Alokator a error callback
 * ========================================================================= */

/** @brief Vychozi alokace pameti (obaluje malloc). */
static void* mzcmt_slow_default_alloc ( size_t size ) { return malloc ( size ); }
/** @brief Vychozi alokace s nulovanim (obaluje calloc). */
static void* mzcmt_slow_default_alloc0 ( size_t size ) { return calloc ( 1, size ); }
/** @brief Vychozi uvolneni pameti (obaluje free). */
static void  mzcmt_slow_default_free ( void *ptr ) { free ( ptr ); }

/** @brief Vychozi alokator vyuzivajici standardni knihovni funkce. */
static const st_MZCMT_SLOW_ALLOCATOR g_mzcmt_slow_default_allocator = {
    mzcmt_slow_default_alloc,
    mzcmt_slow_default_alloc0,
    mzcmt_slow_default_free,
};

/** @brief Aktualne aktivni alokator (vychozi = stdlib). */
static const st_MZCMT_SLOW_ALLOCATOR *g_allocator = &g_mzcmt_slow_default_allocator;


/** @brief Nastavi vlastni alokator, nebo resetuje na vychozi pri NULL. */
void mzcmt_slow_set_allocator ( const st_MZCMT_SLOW_ALLOCATOR *allocator ) {
    g_allocator = allocator ? allocator : &g_mzcmt_slow_default_allocator;
}


/**
 * @brief Vychozi error callback - vypisuje chyby na stderr.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void mzcmt_slow_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static mzcmt_slow_error_cb g_error_cb = mzcmt_slow_default_error_cb;


/** @brief Nastavi vlastni error callback, nebo resetuje na vychozi pri NULL. */
void mzcmt_slow_set_error_callback ( mzcmt_slow_error_cb cb ) {
    g_error_cb = cb ? cb : mzcmt_slow_default_error_cb;
}


/* =========================================================================
 *  Referencni pulzni tabulky (pocty vzorku pri 44100 Hz)
 * ========================================================================= */

/**
 * @brief Referencni pocty vzorku pro 4 symboly jedne rychlostni urovne.
 *
 * Pro kazdy symbol (0-3) je definovan pocet LOW a HIGH vzorku.
 * Hodnoty jsou definovany pri referencni frekvenci 44100 Hz.
 *
 * Tabulka je indexovana internim tabulkovym indexem (0 = nejrychlejsi,
 * 4 = nejpomalejsi). Mapovani z verejne rychlosti: table_index = 4 - speed.
 */
typedef struct st_slow_symbol_ref {
    uint8_t low;  /**< pocet LOW vzorku pri ref. frekvenci */
    uint8_t high; /**< pocet HIGH vzorku pri ref. frekvenci */
} st_slow_symbol_ref;


/**
 * @brief Referencni tabulka LOW vzorku pro vsech 5 urovni a 4 symboly.
 *
 * Prevzato z mzftools coder.c, pole lowln[5][4].
 * Radek = tabulkovy index (0 = nejrychlejsi, 4 = nejpomalejsi).
 * Sloupec = symbol (00=0, 01=1, 10=2, 11=3).
 *
 * Mapovani z verejne rychlosti: radek = 4 - speed.
 * Tj. speed 0 (nejpomalejsi) cte radek 4, speed 4 (nejrychlejsi) cte radek 0.
 */
static const uint8_t g_slow_low_ref[MZCMT_SLOW_SPEED_COUNT][MZCMT_SLOW_SYMBOL_COUNT] = {
    /* table index 0 (speed 4, nejrychlejsi) */ { 1, 2, 3, 4 },
    /* table index 1 (speed 3)               */ { 1, 2, 3, 4 },
    /* table index 2 (speed 2)               */ { 1, 3, 4, 6 },
    /* table index 3 (speed 1)               */ { 3, 5, 7, 9 },
    /* table index 4 (speed 0, nejpomalejsi) */ { 3, 6, 9, 12 },
};


/**
 * @brief Referencni tabulka HIGH vzorku pro vsech 5 urovni a 4 symboly.
 *
 * Prevzato z mzftools coder.c, pole highln[5][4].
 * Indexovani shodne s g_slow_low_ref.
 */
static const uint8_t g_slow_high_ref[MZCMT_SLOW_SPEED_COUNT][MZCMT_SLOW_SYMBOL_COUNT] = {
    /* table index 0 (speed 4, nejrychlejsi) */ { 1, 2, 3, 4 },
    /* table index 1 (speed 3)               */ { 1, 3, 5, 7 },
    /* table index 2 (speed 2)               */ { 2, 3, 5, 6 },
    /* table index 3 (speed 1)               */ { 3, 5, 7, 9 },
    /* table index 4 (speed 0, nejpomalejsi) */ { 3, 6, 9, 12 },
};


/* =========================================================================
 *  Skalovani a interni typy
 * ========================================================================= */

/**
 * @brief Skalovane pocty vzorku pro 4 symboly.
 *
 * Obsahuje LOW a HIGH pocty vzorku pro kazdy symbol (0-3)
 * jiz preskalovane na cilovy sample rate.
 */
typedef struct st_slow_symbol_pulses {
    uint32_t low[MZCMT_SLOW_SYMBOL_COUNT];  /**< pocet LOW vzorku pro symbol 0-3 */
    uint32_t high[MZCMT_SLOW_SYMBOL_COUNT]; /**< pocet HIGH vzorku pro symbol 0-3 */
} st_slow_symbol_pulses;


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
static inline uint32_t slow_scale ( uint32_t ref_samples, uint32_t rate ) {
    uint32_t s = ( uint32_t ) round ( ( double ) ref_samples * rate / MZCMT_SLOW_REF_RATE );
    return s > 0 ? s : 1;
}


/**
 * @brief Pripravi skalovane symbolove pulzy pro danou rychlost a sample rate.
 *
 * Prevede referencni tabulky na skalovane hodnoty pro cilovy rate.
 * Mapovani rychlosti: table_index = 4 - speed (inverze).
 *
 * @param[out] pulses Vystupni struktura se skalovanymi pulzy.
 * @param speed Rychlostni uroven (0-4).
 * @param rate Cilovy sample rate (Hz).
 */
static void slow_prepare_pulses ( st_slow_symbol_pulses *pulses, en_MZCMT_SLOW_SPEED speed, uint32_t rate ) {
    int table_idx = ( MZCMT_SLOW_SPEED_COUNT - 1 ) - ( int ) speed;
    int sym;
    for ( sym = 0; sym < MZCMT_SLOW_SYMBOL_COUNT; sym++ ) {
        pulses->low[sym] = slow_scale ( g_slow_low_ref[table_idx][sym], rate );
        pulses->high[sym] = slow_scale ( g_slow_high_ref[table_idx][sym], rate );
    }
}


/* =========================================================================
 *  Interni pomocne funkce pro generovani signalu
 * ========================================================================= */

/**
 * @brief Prida GAP (leader) sekvenci do vstreamu.
 *
 * GAP slouzi jako leader ton pred daty (dekoder na Z80 ma pevny prah z tabulky).
 * Dekoder meri sirku GAP pulzu, detekuje synchronizacni sekvenci
 * a pripravuje lookup tabulku pro rozpoznavani symbolu.
 *
 * Generuje 200 cyklu, kazdy cyklus = 8 LOW + 8 HIGH vzorku
 * (pri ref. frekvenci, skalovano na cilovy rate).
 * Struktura GAP je shodna s FSK koderem (spolecny frame format).
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int slow_add_gap ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = slow_scale ( MZCMT_SLOW_GAP_LOW_REF, rate );
    uint32_t high = slow_scale ( MZCMT_SLOW_GAP_HIGH_REF, rate );
    int i;
    for ( i = 0; i < MZCMT_SLOW_GAP_CYCLES; i++ ) {
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
 * Struktura je shodna s FSK koderem.
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu po GAP).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int slow_add_polarity ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = slow_scale ( MZCMT_SLOW_POLARITY_LOW_REF, rate );
    uint32_t high = slow_scale ( MZCMT_SLOW_POLARITY_HIGH_REF, rate );
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Prida synchronizacni kod do vstreamu.
 *
 * Sync kod (2 LOW + 2 HIGH vzorky pri ref. frekvenci) oznacuje konec
 * leader sekvence a zacatek datoveho toku. Dekoder po jeho detekci
 * inicializuje registr B na $FE (shift registr pro 2-bitove symboly),
 * nastavi ukazatel DE na lookup tabulku a prechazi do rezimu cteni.
 * Struktura je shodna s FSK koderem.
 *
 * @param vstream Cilovy vstream (signal musi byt v LOW stavu po polarity).
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int slow_add_sync ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    uint32_t low = slow_scale ( MZCMT_SLOW_SYNC_LOW_REF, rate );
    uint32_t high = slow_scale ( MZCMT_SLOW_SYNC_HIGH_REF, rate );
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, low ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, high ) ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


/**
 * @brief Zakoduje jeden datovy bajt do vstreamu SLOW modulaci.
 *
 * Bajt se rozlozi na 4 dvoubitove symboly (MSB first). Pro kazdy
 * symbol se z tabulky vybere pocet LOW a HIGH vzorku a zapise
 * jako jeden pulz do vstreamu.
 *
 * Dekoder na Z80 meri sirku pulzu pomoci citace v registru E
 * a indexuje do lookup tabulky na adrese $0300. Tabulka obsahuje
 * masky pro rekonstrukci 2-bitovych symbolu, ktere se postupne
 * skladaji v registru B pomoci RLCA + RLA (dvojity levy posun).
 *
 * @param vstream Cilovy vstream.
 * @param byte Bajt k zakodovani.
 * @param pulses Skalovane symbolove pulzy.
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int slow_encode_byte ( st_CMT_VSTREAM *vstream, uint8_t byte, const st_slow_symbol_pulses *pulses ) {
    int i;
    for ( i = 0; i < 8; i += 2 ) {
        int symbol = ( byte >> ( 6 - i ) ) & 0x03;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, pulses->low[symbol] ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, pulses->high[symbol] ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Prida fade-out sekvenci do vstreamu.
 *
 * Postupne prodluzujici se pulzy (od 32 do 46 vzorku pri ref. frekvenci,
 * s krokem 2). Dekoder detekuje konec datoveho toku podle "extremne
 * dlouheho pulzu" (cp $30 v SLOW Z80 kodu - prahova hodnota citace
 * v registru E) a ukonci prijem.
 * Struktura je shodna s FSK koderem.
 *
 * @param vstream Cilovy vstream.
 * @param rate Cilovy sample rate (Hz).
 * @return EXIT_SUCCESS pri uspechu, EXIT_FAILURE pri chybe.
 */
static int slow_add_fadeout ( st_CMT_VSTREAM *vstream, uint32_t rate ) {
    int w;
    for ( w = MZCMT_SLOW_FADEOUT_START_REF; w < MZCMT_SLOW_FADEOUT_END_REF; w += MZCMT_SLOW_FADEOUT_STEP_REF ) {
        uint32_t s = slow_scale ( ( uint32_t ) w, rate );
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 0, s ) ) return EXIT_FAILURE;
        if ( EXIT_SUCCESS != cmt_vstream_add_value ( vstream, 1, s ) ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/* =========================================================================
 *  Verejne API
 * ========================================================================= */

/**
 * @brief Vytvori CMT vstream z datoveho bloku SLOW kodovanim.
 *
 * Generuje kompletni SLOW signal:
 * 1. GAP (200 cyklu leader tonu)
 * 2. Polaritni pulz (detekce polarity signalu)
 * 3. Sync kod (oznaceni zacatku dat)
 * 4. Datove bajty (4 SLOW pulzy na bajt, po 2 bitech, MSB first)
 * 5. Fade-out (rostouci pulzy pro detekci konce streamu)
 *
 * Signal zacina v LOW stavu. Pulzni sirky se skaluji z referencni
 * frekvence 44100 Hz na cilovy @p rate proporcionalne (zachovava
 * absolutni casovani).
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param speed     Rychlostni uroven (0-4).
 * @param rate      Vzorkovaci frekvence vystupniho vstreamu (Hz).
 * @return Ukazatel na novy vstream, nebo NULL pri chybe.
 *         Volajici vlastni vstream a musi jej uvolnit pres cmt_vstream_destroy().
 */
st_CMT_VSTREAM* mzcmt_slow_create_vstream (
    const uint8_t *data,
    uint32_t data_size,
    en_MZCMT_SLOW_SPEED speed,
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

    if ( speed >= MZCMT_SLOW_SPEED_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid speed level %d (max %d)\n", speed, MZCMT_SLOW_SPEED_COUNT - 1 );
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

    /* priprava skalovanych symbolovych pulzu */
    st_slow_symbol_pulses pulses;
    slow_prepare_pulses ( &pulses, speed, rate );

    /* 1. GAP (leader ton) */
    if ( EXIT_SUCCESS != slow_add_gap ( vstream, rate ) ) goto error;

    /* 2. polaritni pulz */
    if ( EXIT_SUCCESS != slow_add_polarity ( vstream, rate ) ) goto error;

    /* 3. sync kod */
    if ( EXIT_SUCCESS != slow_add_sync ( vstream, rate ) ) goto error;

    /* 4. datove bajty */
    uint32_t i;
    for ( i = 0; i < data_size; i++ ) {
        if ( EXIT_SUCCESS != slow_encode_byte ( vstream, data[i], &pulses ) ) goto error;
    }

    /* 5. fade-out */
    if ( EXIT_SUCCESS != slow_add_fadeout ( vstream, rate ) ) goto error;

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during SLOW signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream z datoveho bloku SLOW kodovanim.
 *
 * Pro typ CMT_STREAM_TYPE_VSTREAM primo pouzije mzcmt_slow_create_vstream().
 * Pro typ CMT_STREAM_TYPE_BITSTREAM vytvori vstream a konvertuje jej na
 * bitstream (presnejsi nez prima generace bitstreamu - viz mztape princip).
 *
 * @param data      Ukazatel na datovy blok k zakodovani.
 * @param data_size Velikost datoveho bloku v bajtech.
 * @param speed     Rychlostni uroven (0-4).
 * @param type      Typ vystupniho streamu (bitstream/vstream).
 * @param rate      Vzorkovaci frekvence (Hz).
 * @return Ukazatel na novy stream, nebo NULL pri chybe.
 *         Volajici vlastni stream a musi jej uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* mzcmt_slow_create_stream (
    const uint8_t *data,
    uint32_t data_size,
    en_MZCMT_SLOW_SPEED speed,
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
            st_CMT_VSTREAM *vstream = mzcmt_slow_create_vstream ( data, data_size, speed, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_slow_create_vstream ( data, data_size, speed, rate );
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
 * SLOW dekoder na Z80 pouziva registr IXL jako akumulator. Kazdy
 * prijaty bajt je XORovan s aktualni hodnotou IXL (instrukce XOR IXL).
 * Po prijeti vsech bajtu porovnava vysledek s predpocitanou hodnotou
 * ulozenou v kodu loaderu (instrukce CP na adrese checksumx v slow.asm).
 *
 * @param data Ukazatel na datovy blok.
 * @param size Velikost datoveho bloku v bajtech.
 * @return XOR vsech bajtu v bloku. Vraci 0 pro NULL nebo size==0.
 */
uint8_t mzcmt_slow_compute_checksum ( const uint8_t *data, uint32_t size ) {
    if ( !data || size == 0 ) return 0;
    uint8_t cs = 0;
    uint32_t i;
    for ( i = 0; i < size; i++ ) {
        cs ^= data[i];
    }
    return cs;
}


/* =========================================================================
 *  Embedded loader binarky pro SLOW tape
 * ========================================================================= */

/**
 * @brief Binarni kod preloaderu (loader_turbo z mzftools, 128 bajtu).
 *
 * Preloader je MZF hlavicka s fsize=0 a Z80 kodem v komentarove oblasti.
 * ROM ho nacte standardnim NORMAL FM 1:1, IPL mechanismus spusti kod
 * v komentari, ktery modifikuje readpoint a vola ROM cteci rutinu
 * pro nacteni SLOW loaderu.
 *
 * Shodny s preloaderem v mzcmt_fsk - pouziva se stejny loader_turbo.
 */
static const uint8_t g_slow_preloader[128] = {
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
 * @brief SLOW loader - Z80 telo (249 bajtu).
 *
 * Extrahovano z loader_slow (377B celkem) v loaders.h - prvnich 128B je
 * MZF hlavicka, zbytek (249B) je body. Loader inicializuje lookup tabulku
 * na adrese $0300, cte SLOW modulovany signal a dekoduje 2-bitove symboly.
 *
 * Parametry loaderu (fsize=$00F9, fstrt=$D400, fexec=$D400).
 */
static const uint8_t g_slow_loader_body[249] = {
    0x3E,0x08,0xD3,0xCE,0x21,0x00,0x00,0x22,0x02,0x11,0xD9,0x21,0x00,0x00,0x22,
    0x06,0x11,0xD3,0xE0,0x3E,0xFC,0x21,0x00,0x03,0x11,0x01,0x03,0x77,0x06,0x00,
    0x3A,0xEE,0xD4,0x4F,0xED,0xB0,0x3E,0xFD,0x77,0x3A,0xEE,0xD4,0x4F,0xED,0xB0,
    0x3E,0xFE,0x77,0x3A,0xEE,0xD4,0x4F,0xED,0xB0,0x3E,0xFF,0x77,0x0E,0x20,0xED,
    0xB0,0x21,0x00,0xD4,0x11,0x55,0x01,0x01,0xEF,0x00,0xED,0xB0,0xCD,0xA9,0x01,
    0x21,0x00,0x00,0xD9,0xD3,0xE4,0xC3,0x9A,0xE9,0xD9,0x11,0x00,0x12,0x01,0xCF,
    0x06,0xD9,0xF3,0x21,0x02,0xE0,0x01,0xC2,0x10,0xAF,0x3C,0xCB,0x6E,0xCA,0xB9,
    0x01,0x85,0xCB,0x6E,0xC2,0xBF,0x01,0xFE,0x52,0xCB,0x10,0xFE,0x36,0x30,0xEB,
    0x04,0x20,0xE8,0xFE,0x22,0x79,0x30,0x18,0xAF,0xDD,0x6F,0xCB,0x6E,0xCA,0xD8,
    0x01,0xCB,0x6E,0xC2,0xDD,0x01,0x11,0x00,0x03,0x06,0xFE,0x78,0x4F,0x37,0xC3,
    0x1D,0x02,0xCB,0x6E,0xCA,0xED,0x01,0xEE,0x08,0x32,0x13,0x02,0x32,0xDF,0x01,
    0xEE,0x08,0x32,0x20,0x02,0x32,0xDA,0x01,0xC3,0xD5,0x01,0xA1,0xD9,0x12,0xDD,
    0xAD,0xDD,0x6F,0x13,0xD9,0x06,0xFE,0x1C,0xCB,0x6E,0xC2,0x10,0x02,0x1A,0x4F,
    0x1E,0x00,0x78,0x07,0x17,0x1C,0xCB,0x6E,0xCA,0x1D,0x02,0xD2,0x05,0x02,0xD9,
    0xED,0x79,0xD9,0xA1,0x47,0x7B,0xFE,0x30,0xDA,0x10,0x02,0xD9,0xAF,0xED,0x79,
    0xD9,0xDD,0x7D,0x37,0x3F,0xFE,0x00,0xCA,0x41,0x02,0x37,0xC9,0x00,0x00,0xE7,
    0xD4,0xEE,0xD4,0x05,0xD4,0x4C,0xD4,0x0C,0xD4
};


/* =========================================================================
 *  Patch offsety pro preloader a SLOW loader
 * ========================================================================= */

/** @brief Offset fsize v comment casti preloaderu (od bajtu 24). */
#define SLOW_PRELOADER_OFF_SIZE    1
/** @brief Offset fstrt v comment casti preloaderu (od bajtu 24). */
#define SLOW_PRELOADER_OFF_FROM    3
/** @brief Offset fexec v comment casti preloaderu (od bajtu 24). */
#define SLOW_PRELOADER_OFF_EXEC   5
/** @brief Offset readpoint delay v comment casti preloaderu (od bajtu 24). */
#define SLOW_PRELOADER_OFF_DELAY  54

/** @brief Offset fsize v body SLOW loaderu (249B). */
#define SLOW_LOADER_OFF_SIZE       5
/** @brief Offset fexec v body SLOW loaderu (249B). */
#define SLOW_LOADER_OFF_EXEC       12
/** @brief Offset fstrt v body SLOW loaderu (249B). */
#define SLOW_LOADER_OFF_FROM       76
/** @brief Offset readpoint delay v body SLOW loaderu (249B). */
#define SLOW_LOADER_OFF_DELAY      238
/** @brief Offset XOR checksum v body SLOW loaderu (249B). */
#define SLOW_LOADER_OFF_CHECKSUM   231

/** @brief Velikost body SLOW loaderu v bajtech. */
#define SLOW_LOADER_BODY_SIZE  249
/** @brief Startovni adresa SLOW loaderu v pameti Z80. */
#define SLOW_LOADER_FSTRT      0xD400
/** @brief Exec adresa SLOW loaderu v pameti Z80. */
#define SLOW_LOADER_FEXEC      0xD400


/* =========================================================================
 *  Delay tabulky pro ROM a SLOW loader
 * ========================================================================= */

/** @brief ROM delay tabulka pro 44100 Hz (speed 0-6, pro nacteni SLOW loaderu). */
static const uint8_t g_rom_delay_44k[] = { 76, 29, 17, 9, 4, 2, 1 };
/** @brief ROM delay tabulka pro 48000 Hz (speed 0-6, pro nacteni SLOW loaderu). */
static const uint8_t g_rom_delay_48k[] = { 76, 29, 17, 7, 2, 1, 1 };

/** @brief SLOW delay tabulka pro 44100 Hz (speed 0-4, pro nacteni dat ve SLOW). */
static const uint8_t g_slow_delay_44k[] = { 22, 17, 11, 9, 6 };
/** @brief SLOW delay tabulka pro 48000 Hz (speed 0-4, pro nacteni dat ve SLOW). */
static const uint8_t g_slow_delay_48k[] = { 20, 15, 10, 8, 5 };


/**
 * @brief Vrati ROM delay pro danou rychlost a vzorkovaci frekvenci.
 *
 * Delay urcuje prodlevu v ROM cteci smycce pri nacitani SLOW loaderu.
 * Pro frekvence >= 46050 Hz pouziva tabulku pro 48000 Hz, jinak 44100 Hz.
 *
 * @param speed Rychlost ROM (0-6). Hodnoty > 6 se oriznou na 6.
 * @param rate Vzorkovaci frekvence (Hz).
 * @return Hodnota delay pro patchovani preloaderu.
 */
static uint8_t slow_tape_get_rom_delay ( uint8_t speed, uint32_t rate ) {
    if ( speed > 6 ) speed = 6;
    if ( rate >= 46050 ) return g_rom_delay_48k[speed];
    return g_rom_delay_44k[speed];
}


/**
 * @brief Vrati SLOW delay pro danou rychlost a vzorkovaci frekvenci.
 *
 * Delay urcuje casovaci konstantu SLOW dekoderu pri cteni uzivatelskeho
 * datoveho bloku. Pro frekvence >= 46050 Hz pouziva tabulku pro 48000 Hz.
 *
 * @param speed Rychlost SLOW (0-4). Hodnoty > 4 se oriznou na 4.
 * @param rate Vzorkovaci frekvence (Hz).
 * @return Hodnota delay pro patchovani SLOW loaderu.
 */
static uint8_t slow_tape_get_slow_delay ( uint8_t speed, uint32_t rate ) {
    if ( speed > 4 ) speed = 4;
    if ( rate >= 46050 ) return g_slow_delay_48k[speed];
    return g_slow_delay_44k[speed];
}


/* =========================================================================
 *  NORMAL FM pulzni struktury a tabulky pro tape cast
 * ========================================================================= */

/** @brief Dvojice delek pulzu (high + low) v sekundach. */
typedef struct st_slow_tape_pulse_length {
    double high; /**< delka HIGH casti pulzu (s) */
    double low;  /**< delka LOW casti pulzu (s) */
} st_slow_tape_pulse_length;

/** @brief Par delek pro long a short pulz. */
typedef struct st_slow_tape_pulses_length {
    st_slow_tape_pulse_length long_pulse;  /**< delka long pulzu (bit "1") */
    st_slow_tape_pulse_length short_pulse; /**< delka short pulzu (bit "0") */
} st_slow_tape_pulses_length;

/** @brief MZ-700 pulzy. */
static const st_slow_tape_pulses_length g_tape_pulses_700 = {
    { 0.000464, 0.000494 },
    { 0.000240, 0.000264 },
};

/** @brief MZ-800 pulzy (Intercopy mereni). */
static const st_slow_tape_pulses_length g_tape_pulses_800 = {
    { 0.000470330, 0.000494308 },
    { 0.000245802, 0.000278204 },
};

/** @brief MZ-80B pulzy. */
static const st_slow_tape_pulses_length g_tape_pulses_80B = {
    { 0.000333, 0.000334 },
    { 0.000166750, 0.000166 },
};

/** @brief Pole ukazatelu na pulzni sady indexovane en_MZCMT_SLOW_PULSESET. */
static const st_slow_tape_pulses_length *g_tape_pulses[] = {
    &g_tape_pulses_700,
    &g_tape_pulses_800,
    &g_tape_pulses_80B,
};


/** @brief Pocet vzorku jednoho NORMAL FM pulzu (skalovany na cilovy rate). */
typedef struct st_slow_tape_pulse_samples {
    uint32_t high; /**< pocet HIGH vzorku */
    uint32_t low;  /**< pocet LOW vzorku */
} st_slow_tape_pulse_samples;

/** @brief Pocty vzorku pro long a short pulz (skalovane). */
typedef struct st_slow_tape_pulses_samples {
    st_slow_tape_pulse_samples long_pulse;  /**< skalovany long pulz */
    st_slow_tape_pulse_samples short_pulse; /**< skalovany short pulz */
} st_slow_tape_pulses_samples;


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
static void slow_tape_prepare_pulses ( st_slow_tape_pulses_samples *pulses, en_MZCMT_SLOW_PULSESET pulseset, double divisor, uint32_t rate ) {
    const st_slow_tape_pulses_length *src = g_tape_pulses[pulseset];

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
static int slow_tape_add_pulses ( st_CMT_VSTREAM *vstream, const st_slow_tape_pulse_samples *pulse, uint32_t count ) {
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
static int slow_tape_add_tapemark ( st_CMT_VSTREAM *vstream, const st_slow_tape_pulses_samples *pulses, uint32_t long_count, uint32_t short_count ) {
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &pulses->long_pulse, long_count ) ) return EXIT_FAILURE;
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &pulses->short_pulse, short_count ) ) return EXIT_FAILURE;
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
static int slow_tape_encode_data ( st_CMT_VSTREAM *vstream, const st_slow_tape_pulses_samples *pulses, const uint8_t *data, uint16_t size ) {
    uint16_t i;
    for ( i = 0; i < size; i++ ) {
        uint8_t byte = data[i];
        int bit;
        for ( bit = 0; bit < 8; bit++ ) {
            const st_slow_tape_pulse_samples *pulse = ( byte & 0x80 )
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
static int slow_tape_encode_checksum ( st_CMT_VSTREAM *vstream, const st_slow_tape_pulses_samples *pulses, uint16_t checksum ) {
    uint16_t be = endianity_bswap16_BE ( checksum );
    return slow_tape_encode_data ( vstream, pulses, ( const uint8_t* ) &be, 2 );
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
static uint16_t slow_tape_compute_ones_checksum ( const uint8_t *data, uint16_t size ) {
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
 * 2. SLOW loader body v NORMAL FM pri loader_speed (preloader ho nacte)
 * 3. Uzivatelska data v SLOW formatu (SLOW loader je nacte)
 *
 * Preloader je patchovan s parametry SLOW loaderu (velikost, adresy, delay).
 * SLOW loader je patchovan s parametry uzivatelskeho programu (velikost,
 * adresy, SLOW delay, XOR checksum).
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
st_CMT_VSTREAM* mzcmt_slow_create_tape_vstream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_SLOW_TAPE_CONFIG *config,
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

    if ( config->pulseset >= MZCMT_SLOW_PULSESET_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid pulseset %d\n", config->pulseset );
        return NULL;
    }

    if ( config->speed >= MZCMT_SLOW_SPEED_COUNT ) {
        g_error_cb ( __func__, __LINE__, "Invalid SLOW speed %d\n", config->speed );
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
    memcpy ( preloader, g_slow_preloader, 128 );

    /* kopirovat fname z originalu */
    memcpy ( &preloader[1], &original->fname, sizeof ( original->fname ) );

    /* patch comment cast (od bajtu 24): fsize, fstrt, fexec SLOW loaderu */
    uint16_t loader_size_le = SLOW_LOADER_BODY_SIZE;
    uint16_t loader_from_le = SLOW_LOADER_FSTRT;
    uint16_t loader_exec_le = SLOW_LOADER_FEXEC;
    memcpy ( &preloader[24 + SLOW_PRELOADER_OFF_SIZE], &loader_size_le, 2 );
    memcpy ( &preloader[24 + SLOW_PRELOADER_OFF_FROM], &loader_from_le, 2 );
    memcpy ( &preloader[24 + SLOW_PRELOADER_OFF_EXEC], &loader_exec_le, 2 );

    /* patch delay pro ROM speed (rychlost nacteni SLOW loaderu) */
    preloader[24 + SLOW_PRELOADER_OFF_DELAY] = slow_tape_get_rom_delay ( config->loader_speed, rate );

    /* ===== B. Priprava patchovaneho SLOW loader body ===== */

    uint8_t slow_loader[SLOW_LOADER_BODY_SIZE];
    memcpy ( slow_loader, g_slow_loader_body, SLOW_LOADER_BODY_SIZE );

    /* patch: uzivatelska velikost, adresy */
    uint16_t user_size = ( uint16_t ) body_size;
    uint16_t user_from = original->fstrt;
    uint16_t user_exec = original->fexec;
    memcpy ( &slow_loader[SLOW_LOADER_OFF_SIZE], &user_size, 2 );
    memcpy ( &slow_loader[SLOW_LOADER_OFF_FROM], &user_from, 2 );
    memcpy ( &slow_loader[SLOW_LOADER_OFF_EXEC], &user_exec, 2 );

    /* patch: SLOW delay */
    slow_loader[SLOW_LOADER_OFF_DELAY] = slow_tape_get_slow_delay ( config->speed, rate );

    /* patch: XOR checksum uzivatelskeho body */
    slow_loader[SLOW_LOADER_OFF_CHECKSUM] = mzcmt_slow_compute_checksum ( body, body_size );

    /* ===== C. Pulzy pro NORMAL FM ===== */

    /* pulzy pro preloader header (1:1 rychlost) */
    st_slow_tape_pulses_samples hdr_pulses;
    slow_tape_prepare_pulses ( &hdr_pulses, config->pulseset, 1.0, rate );

    /* pulzy pro loader body (loader_speed) */
    st_slow_tape_pulses_samples ldr_pulses;
    if ( config->loader_speed == 0 ) {
        /* standardni ROM rychlost = 1:1 */
        slow_tape_prepare_pulses ( &ldr_pulses, config->pulseset, 1.0, rate );
    } else {
        /* loader_speed 1-6 mapovat na cmtspeed enum */
        en_CMTSPEED ldr_cmtspeed = ( en_CMTSPEED ) ( config->loader_speed + 1 );
        if ( !cmtspeed_is_valid ( ldr_cmtspeed ) ) ldr_cmtspeed = CMTSPEED_1_1;
        double ldr_divisor = cmtspeed_get_divisor ( ldr_cmtspeed );
        slow_tape_prepare_pulses ( &ldr_pulses, config->pulseset, ldr_divisor, rate );
    }

    /* ===== D. Checksums ===== */

    uint16_t chk_preloader_hdr = slow_tape_compute_ones_checksum ( preloader, 128 );
    uint16_t chk_loader_body = slow_tape_compute_ones_checksum ( slow_loader, SLOW_LOADER_BODY_SIZE );

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
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &hdr_pulses.short_pulse, 22000 ) ) goto error;

    /* dlouhy tapemark (40 long + 40 short) */
    if ( EXIT_SUCCESS != slow_tape_add_tapemark ( vstream, &hdr_pulses, 40, 40 ) ) goto error;

    /* 2 long + header(128B) + checksum + 2 long */
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_encode_data ( vstream, &hdr_pulses, preloader, 128 ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_encode_checksum ( vstream, &hdr_pulses, chk_preloader_hdr ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &hdr_pulses.long_pulse, 2 ) ) goto error;

    /* protoze fsize=0, body blok preloaderu se neposila (ROM ho necte) */
    /* preloader modifikuje ROM: nastavi size/from/exec a readpoint, pak vola
       ROM body-cteci rutinu - ta cte primo nasledujici body blok z pasky */

    /* ----- Cast 2: SLOW loader body v NORMAL pri loader_speed ----- */
    /* SLOW loader header se na pasku NEPISE - preloader patchuje ROM primo
       s parametry loaderu (size=249, from=$D400, exec=$D400) a cte jen body */

    /* SGAP */
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &ldr_pulses.short_pulse, ldr_sgap ) ) goto error;

    /* kratky tapemark (20 long + 20 short) */
    if ( EXIT_SUCCESS != slow_tape_add_tapemark ( vstream, &ldr_pulses, 20, 20 ) ) goto error;

    /* 2 long + body(249B) + checksum + 2 long */
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_encode_data ( vstream, &ldr_pulses, slow_loader, SLOW_LOADER_BODY_SIZE ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_encode_checksum ( vstream, &ldr_pulses, chk_loader_body ) ) goto error;
    if ( EXIT_SUCCESS != slow_tape_add_pulses ( vstream, &ldr_pulses.long_pulse, 2 ) ) goto error;

    /* ----- Cast 3: Uzivatelska data v SLOW formatu ----- */

    if ( body_size > 0 ) {
        /* priprava skalovanych SLOW symbolovych pulzu */
        st_slow_symbol_pulses slow_data_pulses;
        slow_prepare_pulses ( &slow_data_pulses, config->speed, rate );

        /* GAP (leader ton) */
        if ( EXIT_SUCCESS != slow_add_gap ( vstream, rate ) ) goto error;

        /* polaritni pulz */
        if ( EXIT_SUCCESS != slow_add_polarity ( vstream, rate ) ) goto error;

        /* sync kod */
        if ( EXIT_SUCCESS != slow_add_sync ( vstream, rate ) ) goto error;

        /* datove bajty */
        uint32_t i;
        for ( i = 0; i < body_size; i++ ) {
            if ( EXIT_SUCCESS != slow_encode_byte ( vstream, body[i], &slow_data_pulses ) ) goto error;
        }

        /* fade-out */
        if ( EXIT_SUCCESS != slow_add_fadeout ( vstream, rate ) ) goto error;
    }

    return vstream;

error:
    g_error_cb ( __func__, __LINE__, "Error during SLOW tape signal generation\n" );
    cmt_vstream_destroy ( vstream );
    return NULL;
}


/**
 * @brief Vytvori jednotny CMT stream s kompletnim paskovym signalem vcetne loaderu.
 *
 * Obaluje mzcmt_slow_create_tape_vstream() do polymorfniho st_CMT_STREAM.
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
st_CMT_STREAM* mzcmt_slow_create_tape_stream (
    const st_MZF_HEADER *original,
    const uint8_t *body,
    uint32_t body_size,
    const st_MZCMT_SLOW_TAPE_CONFIG *config,
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
            st_CMT_VSTREAM *vstream = mzcmt_slow_create_tape_vstream ( original, body, body_size, config, rate );
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
            st_CMT_VSTREAM *vstream = mzcmt_slow_create_tape_vstream ( original, body, body_size, config, rate );
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


const char* mzcmt_slow_version ( void ) {
    return MZCMT_SLOW_VERSION;
}
