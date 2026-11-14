/**
 * @file   cmt_tmz_import.c
 * @brief  Import pásky do TMZ v paměti (implementace cmt_tmz_import.h).
 *
 * Modul nepoužívá globální stav emulátoru. Jediný sdílený prostředek je
 * paměťový driver g_memory_driver_static (knihovna generic_driver), přes
 * který cmttmz_import_file() načítá soubor stejně jako dosavadní
 * rozšíření CMT a přes který import TMZ/TZX a WAV čte buffer volajícího
 * (handler bez kopie, cmttmz_borrow_handler()).
 *
 * Soubor st_TZX_FILE se alokuje calloc() a bloky z registru profilů mají
 * data z malloc(): modul předpokládá výchozí alokátory knihoven tzx
 * a tmz_blocks (malloc/free), stejně jako tmz_v1conv_file(). Emulátor
 * vlastní alokátor (tzx_set_allocator) nenastavuje.
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

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_mzt.h"
#include "libs/tmz/tmz_mzfile.h"
#include "libs/tmz/tmz_tlv.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/wav/wav.h"
#include "libs/zxtape/zxtape.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

#include "cmt_tmz_import.h"


/**
 * @brief Přiřazení přípony souboru ke zdroji importu.
 */
typedef struct st_CMTTMZ_EXT {
    const char *ext;           /**< přípona malými písmeny, bez tečky */
    en_CMTTMZ_SOURCE source;   /**< zdroj */
} st_CMTTMZ_EXT;


/** @brief Tabulka přípon (porovnává se bez ohledu na velikost písmen). */
static const st_CMTTMZ_EXT g_cmttmz_ext[] = {
    { "tmz", CMTTMZ_SOURCE_TMZ },
    { "tzx", CMTTMZ_SOURCE_TMZ },
    { "mzf", CMTTMZ_SOURCE_MZF },
    { "m12", CMTTMZ_SOURCE_MZF },
    { "mzt", CMTTMZ_SOURCE_MZT },
    { "tap", CMTTMZ_SOURCE_TAP },
    { "wav", CMTTMZ_SOURCE_WAV },
    { "wave", CMTTMZ_SOURCE_WAV },
};


/**
 * @brief Vynuluje report a nastaví zdroj.
 * @param rep Report (NULL se ignoruje).
 * @param source Zdroj.
 */
static void cmttmz_rep_reset ( st_CMTTMZ_IMPORT_REPORT *rep, en_CMTTMZ_SOURCE source ) {
    if ( !rep ) return;
    memset ( rep, 0, sizeof ( *rep ) );
    rep->source = source;
}


/**
 * @brief Zapíše do reportu chybu (přepíše předchozí).
 * @param rep Report (NULL = nic se nezapíše).
 * @param fmt Formát printf.
 * @post rep->error je ukončené nulou, dlouhý text se ořízne.
 */
static void cmttmz_rep_error ( st_CMTTMZ_IMPORT_REPORT *rep, const char *fmt, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );

static void cmttmz_rep_error ( st_CMTTMZ_IMPORT_REPORT *rep, const char *fmt, ... ) {
    if ( !rep ) return;
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( rep->error, sizeof ( rep->error ), fmt, ap );
    va_end ( ap );
}


void cmttmz_report_add_warning ( st_CMTTMZ_IMPORT_REPORT *rep, const char *text ) {
    if ( !rep || !text ) return;
    size_t len = strnlen ( rep->warning, sizeof ( rep->warning ) );
    if ( len >= sizeof ( rep->warning ) ) return;   /* neukončený text se nerozšiřuje */
    if ( len ) {
        /* bez místa na oddělovač a aspoň jeden znak se další varování nepřidá vůbec */
        if ( len + 3 >= sizeof ( rep->warning ) ) return;
        memcpy ( &rep->warning[len], "; ", 3 );
        len += 2;
    }
    snprintf ( &rep->warning[len], sizeof ( rep->warning ) - len, "%s", text );
}


/**
 * @brief Přidá do reportu formátované varování (cmttmz_report_add_warning()).
 * @param rep Report (NULL = nic se nezapíše).
 * @param fmt Formát printf.
 * @post Jako cmttmz_report_add_warning(); text delší než 255 znaků se ořízne.
 */
static void cmttmz_rep_warning ( st_CMTTMZ_IMPORT_REPORT *rep, const char *fmt, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );

static void cmttmz_rep_warning ( st_CMTTMZ_IMPORT_REPORT *rep, const char *fmt, ... ) {
    if ( !rep ) return;
    char text[sizeof ( rep->warning )];
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( text, sizeof ( text ), fmt, ap );
    va_end ( ap );
    cmttmz_report_add_warning ( rep, text );
}


/**
 * @brief Nastaví výchozí volby importu.
 *
 * Výchozí vlastní pulzy jsou pulzy UniCMT 1x 470 / 494 / 240 / 278 µs
 * (stejné výchozí hodnoty jako nastavení vlastních pulzů Virtual CMT).
 *
 * @param opts Volby (NULL se ignoruje).
 */
void cmttmz_import_opts_init ( st_CMTTMZ_IMPORT_OPTS *opts ) {
    if ( !opts ) return;
    memset ( opts, 0, sizeof ( *opts ) );
    opts->default_speed = CMTSPEED_1_1;
    ( void ) mztape_pulses_set_us ( &opts->custom_pulses, 470.0, 494.0, 240.0, 278.0 );
    opts->wav_polarity = CMT_STREAM_POLARITY_NORMAL;
    opts->pause_ms = 1000;
}


/**
 * @brief Porovná dva řetězce ASCII bez ohledu na velikost písmen.
 * @param a První řetězec. @param b Druhý řetězec.
 * @return 1 = shodné, 0 = různé.
 */
static int cmttmz_streq_nocase ( const char *a, const char *b ) {
    while ( *a && *b ) {
        if ( tolower ( ( unsigned char ) *a ) != tolower ( ( unsigned char ) *b ) ) return 0;
        a++;
        b++;
    }
    return ( *a == *b );
}


int cmttmz_source_from_filename ( const char *filename, en_CMTTMZ_SOURCE *source ) {
    if ( !filename ) return EXIT_FAILURE;
    /* přípona jen v posledním prvku cesty */
    const char *base = filename;
    for ( const char *p = filename; *p; p++ ) {
        if ( *p == '/' || *p == '\\' ) base = p + 1;
    }
    const char *dot = strrchr ( base, '.' );
    if ( !dot || dot[1] == '\0' ) return EXIT_FAILURE;
    for ( size_t i = 0; i < sizeof ( g_cmttmz_ext ) / sizeof ( g_cmttmz_ext[0] ); i++ ) {
        if ( cmttmz_streq_nocase ( dot + 1, g_cmttmz_ext[i].ext ) ) {
            if ( source ) *source = g_cmttmz_ext[i].source;
            return EXIT_SUCCESS;
        }
    }
    return EXIT_FAILURE;
}


/**
 * @brief Převede délku pulzu v sekundách na celé µs pro PROFILE_ARGS.
 *
 * Zaokrouhlení lround ( sekundy * 1e6 ) jako u převodu pulzů bloku na µs
 * v knihovně tmz (tmz_mzt_block_info). Pulzy z hlavičky CMTSPEED jsou celé
 * µs, převod tam a zpět je u nich přesný.
 *
 * @param sec Délka v sekundách.
 * @param[out] us Délka v µs.
 * @return EXIT_SUCCESS; EXIT_FAILURE, když výsledek není v rozsahu 1 až 65535 (i NaN).
 */
static int cmttmz_pulse_to_us ( double sec, uint16_t *us ) {
    double v = sec * 1e6;
    if ( !( v >= 0.5 && v < 65535.5 ) ) return EXIT_FAILURE;
    *us = ( uint16_t ) lround ( v );
    return EXIT_SUCCESS;
}


/**
 * @brief cmttmz_build_mzf_block() s kódem chyby registru profilů.
 *
 * @param hdr Hlavička MZF v pořadí hostitele.
 * @param body Tělo (NULL jen při body_size 0).
 * @param body_size Délka těla (== hdr->fsize).
 * @param speed Rychlost bloku.
 * @param custom_pulses Vlastní pulzy (jen při CMTSPEED_CUSTOM).
 * @param pause_ms Pauza za blokem v ms.
 * @param[out] perr Kód chyby; TMZ_PROFILE_ERROR_ARGS při neplatných
 *        parametrech zjištěných zde, jinak z tmz_profile_build_block().
 * @return Nový blok (uvolnit tmz_block_free()), nebo NULL.
 */
static st_TZX_BLOCK* cmttmz_build_mzf_block_err ( const st_MZF_HEADER *hdr, const uint8_t *body, uint32_t body_size,
                                                  en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *custom_pulses,
                                                  uint16_t pause_ms, en_TMZ_PROFILE_ERROR *perr ) {
    *perr = TMZ_PROFILE_ERROR_ARGS;
    if ( !hdr || body_size != hdr->fsize || ( !body && body_size ) ) return NULL;

    st_TMZ_PROFILE_ARGS args;
    tmz_profile_args_init ( &args );
    args.pause_ms = pause_ms;

    if ( cmtspeed_is_custom ( speed ) ) {
        if ( !custom_pulses ) return NULL;
        const double sec[4] = {
                               custom_pulses->long_pulse.high, custom_pulses->long_pulse.low,
                               custom_pulses->short_pulse.high, custom_pulses->short_pulse.low
        };
        for ( int i = 0; i < 4; i++ ) {
            if ( EXIT_SUCCESS != cmttmz_pulse_to_us ( sec[i], &args.cmtspeed[i] ) ) return NULL;
        }
        args.speed = CMTSPEED_NONE;
    } else if ( speed == CMTSPEED_NONE || cmtspeed_is_valid ( speed ) ) {
        args.speed = speed;
    } else {
        return NULL;
    }

    *perr = TMZ_PROFILE_OK;
    return tmz_profile_build_block ( TMZ_PROFILE_ID_MZ800EMU_CMT, &args, hdr, body, body_size, perr );
}


st_TZX_BLOCK* cmttmz_build_mzf_block ( const st_MZF_HEADER *hdr, const uint8_t *body, uint32_t body_size,
                                       en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *custom_pulses,
                                       uint16_t pause_ms ) {
    en_TMZ_PROFILE_ERROR perr;
    return cmttmz_build_mzf_block_err ( hdr, body, body_size, speed, custom_pulses, pause_ms, &perr );
}


/**
 * @brief Vytvoří prázdný soubor s hlavičkou TMZ.
 * @return Nový soubor (uvolnit tzx_free()), nebo NULL při selhání alokace.
 */
static st_TZX_FILE* cmttmz_file_new ( void ) {
    st_TZX_FILE *file = ( st_TZX_FILE* ) calloc ( 1, sizeof ( st_TZX_FILE ) );
    if ( !file ) return NULL;
    tmz_header_init ( &file->header );
    file->is_tmz = true;
    return file;
}


/**
 * @brief Přidá blok na konec souboru a převezme jeho data.
 *
 * @param file Soubor.
 * @param block Blok z tmz_profile_build_block() (nesmí být NULL).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace pole bloků.
 * @post Při úspěchu i neúspěchu je struktura block uvolněná; data bloku
 *       vlastní při úspěchu file, při neúspěchu jsou uvolněná.
 */
static int cmttmz_file_take_block ( st_TZX_FILE *file, st_TZX_BLOCK *block ) {
    if ( TZX_OK != tzx_file_append_block ( file, block ) ) {
        tmz_block_free ( block );
        return EXIT_FAILURE;
    }
    block->data = NULL;   /* data převzal soubor */
    tmz_block_free ( block );
    return EXIT_SUCCESS;
}


/**
 * @brief Sestaví blok z MZF a přidá ho do souboru; chybu zapíše do reportu.
 *
 * @param file Soubor.
 * @param raw_header 128 B hlavičky v pořadí souboru.
 * @param body Tělo (NULL jen při body_size 0).
 * @param body_size Délka těla (== fsize hlavičky).
 * @param speed Rychlost bloku.
 * @param custom_pulses Vlastní pulzy (při CMTSPEED_CUSTOM).
 * @param pause_ms Pauza za blokem v ms.
 * @param rep Report (smí být NULL).
 * @return EXIT_SUCCESS; EXIT_FAILURE (chyba v rep->error, file beze změny).
 */
static int cmttmz_file_add_mzf ( st_TZX_FILE *file, const uint8_t *raw_header, const uint8_t *body, uint32_t body_size,
                                 en_CMTSPEED speed, const st_MZTAPE_PULSES_LENGTH *custom_pulses, uint16_t pause_ms,
                                 st_CMTTMZ_IMPORT_REPORT *rep ) {
    /* bajty hlavičky beze změny (i 11h), jen pořadí fsize/fstrt/fexec pro registr */
    st_MZF_HEADER hdr;
    memcpy ( &hdr, raw_header, sizeof ( hdr ) );
    mzf_header_swap_endianity ( &hdr );

    en_TMZ_PROFILE_ERROR perr = TMZ_PROFILE_OK;
    st_TZX_BLOCK *block = cmttmz_build_mzf_block_err ( &hdr, body, body_size, speed, custom_pulses, pause_ms, &perr );
    if ( !block ) {
        cmttmz_rep_error ( rep, "Can't create MZF tape block: %s", tmz_profile_error_string ( perr ) );
        return EXIT_FAILURE;
    }
    if ( EXIT_SUCCESS != cmttmz_file_take_block ( file, block ) ) {
        cmttmz_rep_error ( rep, "Not enough memory" );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Import MZF: jeden blok rychlostí opts->default_speed, bez pauzy za blokem.
 *
 * Blok je poslední (jediný) blok pásky, proto má pauzu 0: páska po něm
 * skončí hned jako u dřívějšího rozšíření MZF (pause_after 0; rozšíření už neexistuje).
 *
 * @param data Data (NULL jen při size 0). @param size Délka dat.
 * @param opts Volby (nesmí být NULL). @param rep Report (smí být NULL).
 * @return Nový soubor, nebo NULL (chyba v rep->error).
 */
static st_TZX_FILE* cmttmz_import_mzf ( const uint8_t *data, uint32_t size, const st_CMTTMZ_IMPORT_OPTS *opts,
                                        st_CMTTMZ_IMPORT_REPORT *rep ) {
    if ( !data || size < MZF_HEADER_SIZE ) {
        cmttmz_rep_error ( rep, "MZF file is shorter than MZF header" );
        return NULL;
    }
    uint32_t fsize = ( uint32_t ) ( data[0x12] | ( data[0x13] << 8 ) );
    uint32_t avail = size - MZF_HEADER_SIZE;
    if ( avail < fsize ) {
        cmttmz_rep_error ( rep, "MZF body is shorter than header size" );
        return NULL;
    }

    st_TZX_FILE *file = cmttmz_file_new ( );
    if ( !file ) {
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }
    const uint8_t *body = fsize ? data + MZF_HEADER_SIZE : NULL;
    if ( EXIT_SUCCESS != cmttmz_file_add_mzf ( file, data, body, fsize, opts->default_speed, &opts->custom_pulses,
                                               0, rep ) ) {
        tzx_free ( file );
        return NULL;
    }
    if ( avail > fsize ) cmttmz_rep_warning ( rep, "MZF file is longer than header size, extra data ignored" );
    return file;
}


/**
 * @brief Import MZT: blok na každý MZF, rychlost z hlaviček CMTSPEED UniCMT.
 *
 * Hlavičky CMTSPEED rozpoznává a čte mztape_unicmt_speed_marker() stejně
 * jako dřívější rozšíření MZT (už neexistuje), varování mají jeho texty.
 * Bloky mají pauzu opts->pause_ms, poslední blok 0 (jako pause_after
 * posledního bloku dřívějšího rozšíření MZT).
 *
 * Při speeds != NULL zapíše do *speeds nové pole (malloc) s rychlostí
 * každého výsledného bloku (cmttmz_import_buffer_speeds()); při chybě tam
 * nechá NULL.
 *
 * @param data Data (NULL jen při size 0). @param size Délka dat.
 * @param opts Volby (nesmí být NULL). @param rep Report (smí být NULL).
 * @param[out] speeds Pole rychlostí bloků (uvolnit free()), nebo NULL = nevytvářet.
 * @return Nový soubor, nebo NULL (chyba v rep->error).
 */
static st_TZX_FILE* cmttmz_import_mzt ( const uint8_t *data, uint32_t size, const st_CMTTMZ_IMPORT_OPTS *opts,
                                        st_CMTTMZ_IMPORT_REPORT *rep, st_CMTTMZ_BLOCK_SPEED **speeds ) {
    if ( speeds ) *speeds = NULL;
    st_TMZ_MZT mzt;
    uint32_t tail = 0;
    /* zbytek kratší než hlavička na konci (výplň po CP/M) se ignoruje */
    if ( EXIT_SUCCESS != tmz_mzt_parse_tail ( data, size, &mzt, &tail ) ) {
        cmttmz_rep_error ( rep, "Can't parse MZT file" );
        return NULL;
    }
    st_TZX_FILE *file = cmttmz_file_new ( );
    /* nejvýš jeden blok na položku MZT (hlavičky CMTSPEED blok nedávají) */
    st_CMTTMZ_BLOCK_SPEED *sp = NULL;
    if ( file && speeds && mzt.count ) {
        sp = ( st_CMTTMZ_BLOCK_SPEED* ) calloc ( mzt.count, sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
        if ( !sp ) {
            tzx_free ( file );
            file = NULL;
        }
    }
    if ( !file ) {
        tmz_mzt_free ( &mzt );
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }

    /* index posledního MZF (dostane pauzu 0) */
    uint32_t last_mzf = 0;
    for ( uint32_t i = 0; i < mzt.count; i++ ) {
        if ( !mzt.items[i].is_cmtspeed ) last_mzf = i;
    }

    /* rychlost z poslední platné hlavičky CMTSPEED */
    int marker_active = 0;
    st_MZTAPE_PULSES_LENGTH marker_pulses;
    memset ( &marker_pulses, 0x00, sizeof ( marker_pulses ) );

    for ( uint32_t i = 0; i < mzt.count; i++ ) {
        const st_TMZ_MZT_ITEM *item = &mzt.items[i];
        if ( item->is_cmtspeed ) {
            st_MZTAPE_PULSES_LENGTH pulses;
            if ( MZTAPE_UNICMT_MARKER_VALID == mztape_unicmt_speed_marker ( item->header, &pulses ) ) {
                marker_active = 1;
                marker_pulses = pulses;
                if ( file->block_count == 0 ) {
                    /* HW UniCMT takovou pásku nepřehraje (báze hw/25-unicmt.md), emulátor ji hraje */
                    cmttmz_rep_warning ( rep, "note: CMTSPEED header is the first block; the real UniCMT does not play such a tape" );
                }
            } else {
                cmttmz_rep_warning ( rep, "warning: CMTSPEED header at offset 0x%x has a zero pulse length, ignored",
                                     ( unsigned ) item->offset );
            }
            continue;
        }
        en_CMTSPEED speed = marker_active ? CMTSPEED_CUSTOM : opts->default_speed;
        const st_MZTAPE_PULSES_LENGTH *pulses = marker_active ? &marker_pulses : &opts->custom_pulses;
        uint16_t pause_ms = ( i == last_mzf ) ? 0 : opts->pause_ms;
        if ( EXIT_SUCCESS != cmttmz_file_add_mzf ( file, item->header, item->body, item->body_size, speed, pulses,
                                                   pause_ms, rep ) ) {
            tmz_mzt_free ( &mzt );
            tzx_free ( file );
            free ( sp );
            return NULL;
        }
        if ( sp ) {
            st_CMTTMZ_BLOCK_SPEED *s = &sp[file->block_count - 1];
            s->origin = marker_active ? CMTTMZ_SPEED_ORIGIN_FIXED : CMTTMZ_SPEED_ORIGIN_DEFAULT;
            s->speed = speed;
            if ( cmtspeed_is_custom ( speed ) ) s->pulses = *pulses;
            s->pause_ms = pause_ms;
        }
    }
    tmz_mzt_free ( &mzt );

    if ( file->block_count == 0 ) {
        cmttmz_rep_error ( rep, "No playable block in MZT file" );
        tzx_free ( file );
        free ( sp );
        return NULL;
    }
    if ( speeds ) *speeds = sp;
    return file;
}


/** @brief Pauza za blokem TAP s hlavičkou v ms (jako dřívější rozšíření TAP, CMTTAP_DEFAULT_PAUSE_AFTER_HEADER). */
#define CMTTMZ_TAP_PAUSE_HEADER_MS 350

/** @brief Pauza za datovým blokem TAP v ms (jako dřívější rozšíření TAP, CMTTAP_DEFAULT_PAUSE_AFTER_DATA). */
#define CMTTMZ_TAP_PAUSE_DATA_MS 1000

/** @brief Časová základna délek pulzů bloku 0x11 v Hz (takt ZX Spectrum, TZX_DEFAULT_CPU_CLOCK). */
#define CMTTMZ_TAP_TSTATE_HZ 3500000.0

/*
 * Délky půlvln dnešního zxtape při rychlosti 1:1 v sekundách. Knihovna je
 * nevystavuje (statické konstanty v src/libs/zxtape/zxtape.c), proto jsou
 * zde opsané:
 *   pilot          g_zxtape_pilot_pulse.high/.low       zxtape.c:156-158
 *   sync 1 (L)     g_zxtape_sync_pulse.low              zxtape.c:166-168
 *   sync 2 (H)     g_zxtape_sync_pulse.high             zxtape.c:166-168
 *   bit 0          g_zxtape_data_pulses.short_pulse     zxtape.c:176-179
 *   bit 1          g_zxtape_data_pulses.long_pulse      zxtape.c:176-179
 * Pořadí sync: zxtape zapisuje každý pulz jako L a pak H
 * (zxtape_add_cmt_vstream_onestate_block, zxtape.c:480-487), takže první
 * sync je .low a druhý .high.
 */

/** @brief Půlvlna pilotu zxtape v sekundách (zxtape.c:156-158). */
#define CMTTMZ_ZXTAPE_PILOT_SEC 0.000612395

/** @brief První synchronizační půlvlna zxtape (.low) v sekundách (zxtape.c:166-168). */
#define CMTTMZ_ZXTAPE_SYNC1_SEC 0.000186161

/** @brief Druhá synchronizační půlvlna zxtape (.high) v sekundách (zxtape.c:166-168). */
#define CMTTMZ_ZXTAPE_SYNC2_SEC 0.000145070

/** @brief Půlvlna bitu 0 zxtape v sekundách (zxtape.c:176-179, short). */
#define CMTTMZ_ZXTAPE_ZERO_SEC 0.000242140

/** @brief Půlvlna bitu 1 zxtape v sekundách (zxtape.c:176-179, long). */
#define CMTTMZ_ZXTAPE_ONE_SEC 0.000484335

/** @brief Počáteční kapacita pole pulzů WAV (roste zdvojnásobením podle počtu hran). */
#define CMTTMZ_WAV_PULSES_INITIAL 4096


/**
 * @brief Nastaví paměťový handler, který jen čte buffer volajícího (bez kopie).
 *
 * Handler typu HANDLER_TYPE_MEMORY s driverem g_memory_driver_static
 * ukazuje přímo do data a má stav HANDLER_STATUS_READY |
 * HANDLER_STATUS_READ_ONLY, takže zápis driver odmítne. Opírá se o vnitřek
 * paměťového driveru (memory_driver.c: čtení a příprava kontrolují jen typ,
 * stav READY, ptr a size). Nahrazuje generic_driver_open_memory(), která
 * by celý buffer zkopírovala (u velkého WAV stovky MB).
 *
 * @param h Handler (obsah se přepíše).
 * @param data Data (smí být NULL, pak každé čtení selže).
 * @param size Délka dat.
 * @post Handler se NESMÍ zavřít generic_driver_close(): close callback by
 *       uvolnil buffer volajícího. Stačí ho zahodit; data musí žít, dokud
 *       se handler používá.
 * @pre Volá se z vlákna emulátoru (operace zapisují pole err sdíleného
 *      g_memory_driver_static).
 */
static void cmttmz_borrow_handler ( st_HANDLER *h, const uint8_t *data, uint32_t size ) {
    generic_driver_register_handler ( h, HANDLER_TYPE_MEMORY );
    h->driver = &g_memory_driver_static;
    h->spec.memspec.ptr = ( uint8_t* ) data;   /* jen čtení, viz READ_ONLY */
    h->spec.memspec.open_size = size;
    h->spec.memspec.size = size;
    h->status = HANDLER_STATUS_READY | HANDLER_STATUS_READ_ONLY;
}


/**
 * @brief Import TMZ/TZX: tzx_load() nad bufferem, hlavička souboru zůstává.
 *
 * @param data Data (NULL jen při size 0). @param size Délka dat.
 * @param rep Report (smí být NULL).
 * @return Načtený soubor (signatura TapeMZ! i ZXTape! zůstává), nebo NULL
 *         s textem tzx_error_string() v rep->error; soubor bez bloků dá
 *         chybu "No playable block in TMZ file".
 */
static st_TZX_FILE* cmttmz_import_tmz ( const uint8_t *data, uint32_t size, st_CMTTMZ_IMPORT_REPORT *rep ) {
    st_HANDLER h;
    cmttmz_borrow_handler ( &h, data, size );
    en_TZX_ERROR err = TZX_OK;
    st_TZX_FILE *file = tzx_load ( &h, &err );
    if ( !file ) {
        cmttmz_rep_error ( rep, "%s", tzx_error_string ( err ) );
        return NULL;
    }
    if ( file->block_count == 0 ) {
        tzx_free ( file );
        cmttmz_rep_error ( rep, "No playable block in TMZ file" );
        return NULL;
    }
    return file;
}


/**
 * @brief Převede délku půlvlny zxtape v sekundách na T-stavy bloku 0x11 při dané rychlosti.
 *
 * Stejně jako zxtape škáluje délky pulzů rychlostí (délka / dělitel,
 * zxtape_create_cmt_vstream_from_tapblock(), zxtape.c:559-573), jen
 * v T-stavech při 3,5 MHz místo ve vzorcích.
 *
 * @param sec Délka v sekundách při 1:1.
 * @param divisor Dělitel rychlosti (g_cmtspeed_divisor[], > 0).
 * @param[out] ts Délka v T-stavech.
 * @return EXIT_SUCCESS; EXIT_FAILURE, když výsledek není 1 až 65535.
 */
static int cmttmz_tap_tstates ( double sec, double divisor, uint16_t *ts ) {
    double v = sec * CMTTMZ_TAP_TSTATE_HZ / divisor;
    if ( !( v >= 0.5 && v < 65535.5 ) ) return EXIT_FAILURE;
    *ts = ( uint16_t ) lround ( v );
    return EXIT_SUCCESS;
}


st_TZX_BLOCK* cmttmz_build_tap_block ( const uint8_t *tap_data, uint16_t len, en_CMTSPEED speed, uint16_t pause_ms ) {
    if ( !tap_data || len == 0 ) return NULL;
    if ( speed == CMTSPEED_NONE ) speed = CMTSPEED_1_1;
    if ( !cmtspeed_is_valid ( speed ) ) return NULL;
    double divisor = g_cmtspeed_divisor[speed];
    if ( !( divisor > 0.0 ) ) return NULL;

    const double sec[5] = {
                           CMTTMZ_ZXTAPE_PILOT_SEC, CMTTMZ_ZXTAPE_SYNC1_SEC, CMTTMZ_ZXTAPE_SYNC2_SEC,
                           CMTTMZ_ZXTAPE_ZERO_SEC, CMTTMZ_ZXTAPE_ONE_SEC
    };
    uint16_t w[6];
    for ( int i = 0; i < 5; i++ ) {
        if ( EXIT_SUCCESS != cmttmz_tap_tstates ( sec[i], divisor, &w[i] ) ) return NULL;
    }
    /* počet půlvln pilotu rychlost nemění (zxtape_get_pilot_length(), zxtape.c:200-208) */
    w[5] = ( uint16_t ) ( 2 * ( ( tap_data[0] < 0x80 ) ? ZXTAPE_HEADER_PILOT_LENGTH : ZXTAPE_DATA_PILOT_LENGTH ) );

    st_TZX_BLOCK *block = ( st_TZX_BLOCK* ) malloc ( sizeof ( st_TZX_BLOCK ) );
    if ( !block ) return NULL;
    block->id = TZX_BLOCK_ID_TURBO_SPEED;
    block->length = 0x12u + len;
    block->data = ( uint8_t* ) malloc ( block->length );
    if ( !block->data ) {
        free ( block );
        return NULL;
    }
    for ( int i = 0; i < 6; i++ ) {
        block->data[2 * i] = ( uint8_t ) ( w[i] & 0xFF );
        block->data[2 * i + 1] = ( uint8_t ) ( w[i] >> 8 );
    }
    block->data[0x0C] = 8;
    block->data[0x0D] = ( uint8_t ) ( pause_ms & 0xFF );
    block->data[0x0E] = ( uint8_t ) ( pause_ms >> 8 );
    block->data[0x0F] = ( uint8_t ) ( len & 0xFF );
    block->data[0x10] = ( uint8_t ) ( len >> 8 );
    block->data[0x11] = 0;
    memcpy ( block->data + 0x12, tap_data, len );
    return block;
}


/**
 * @brief Přidá do souboru blok 0x11 s daty bloku TAP a časováním zxtape při 1:1.
 *
 * Blok sestaví cmttmz_build_tap_block() rychlostí CMTSPEED_1_1.
 *
 * @param file Soubor.
 * @param tap_data Data bloku TAP (flag + data + checksum).
 * @param len Délka dat (1 až 65535).
 * @param pause_ms Pauza za blokem v ms.
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (soubor beze změny).
 */
static int cmttmz_file_add_tap_block ( st_TZX_FILE *file, const uint8_t *tap_data, uint16_t len, uint16_t pause_ms ) {
    st_TZX_BLOCK *block = cmttmz_build_tap_block ( tap_data, len, CMTSPEED_1_1, pause_ms );
    if ( !block ) return EXIT_FAILURE;
    return cmttmz_file_take_block ( file, block );
}


/**
 * @brief Výsledek čtení jednoho bloku TAP (cmttmz_tap_next()).
 */
typedef enum en_CMTTMZ_TAP_ITEM {
    CMTTMZ_TAP_END = 0,       /**< konec dat */
    CMTTMZ_TAP_BLOCK,         /**< celý blok s nenulovou délkou */
    CMTTMZ_TAP_EMPTY,         /**< blok délky 0 (přeskočit) */
    CMTTMZ_TAP_DAMAGED,       /**< neúplná délka nebo data za koncem (čtení končí) */
} en_CMTTMZ_TAP_ITEM;


/**
 * @brief Přečte další blok TAP ([2 B délka LE][data]).
 *
 * @param data Data TAP (NULL = hned konec). @param size Délka dat.
 * @param[in,out] pos Pozice dalšího bloku; posune se za blok (i za blok délky 0).
 * @param[out] block Začátek dat bloku (jen při CMTTMZ_TAP_BLOCK).
 * @param[out] len Délka dat bloku (jen při CMTTMZ_TAP_BLOCK).
 * @return en_CMTTMZ_TAP_ITEM; při CMTTMZ_TAP_DAMAGED a CMTTMZ_TAP_END se *pos nemění.
 */
static en_CMTTMZ_TAP_ITEM cmttmz_tap_next ( const uint8_t *data, uint32_t size, uint32_t *pos, const uint8_t **block,
                                            uint16_t *len ) {
    uint32_t p = *pos;
    if ( !data || p >= size ) return CMTTMZ_TAP_END;
    if ( size - p < 2 ) return CMTTMZ_TAP_DAMAGED;
    uint16_t l = ( uint16_t ) ( data[p] | ( data[p + 1] << 8 ) );
    p += 2;
    if ( l == 0 ) {
        *pos = p;
        return CMTTMZ_TAP_EMPTY;
    }
    if ( size - p < l ) return CMTTMZ_TAP_DAMAGED;
    *block = data + p;
    *len = l;
    *pos = p + l;
    return CMTTMZ_TAP_BLOCK;
}


/**
 * @brief Import TAP: každý blok TAP jeden blok 0x11 s časováním dnešního zxtape.
 *
 * Pauzy jako dřívější rozšíření TAP (už neexistuje): za blokem
 * s flagem hlavičky (< 80h) CMTTMZ_TAP_PAUSE_HEADER_MS, za datovým blokem
 * CMTTMZ_TAP_PAUSE_DATA_MS, poslední blok pásky 0; poslední blok se najde
 * předem prvním průchodem
 * (cmttmz_tap_next()), stejně jako poslední MZF u MZT. Blok TAP s délkou 0
 * se přeskočí s varováním; neúplná délka nebo data za koncem souboru
 * ukončí čtení s varováním a načtené bloky zůstanou. Odchylky signálu od
 * zxtape (opačná úroveň, chybějící závěrečná L) viz
 * cmttmz_file_add_tap_block().
 *
 * Při speeds != NULL zapíše do *speeds nové pole (malloc) s rychlostí
 * každého bloku: CMTTMZ_SPEED_ORIGIN_FIXED, CMTSPEED_1_1 a pauza bloku.
 *
 * @param data Data (NULL jen při size 0). @param size Délka dat.
 * @param rep Report (smí být NULL).
 * @param[out] speeds Pole rychlostí bloků (uvolnit free()), nebo NULL = nevytvářet.
 * @return Nový soubor, nebo NULL (chyba v rep->error).
 */
static st_TZX_FILE* cmttmz_import_tap ( const uint8_t *data, uint32_t size, st_CMTTMZ_IMPORT_REPORT *rep,
                                        st_CMTTMZ_BLOCK_SPEED **speeds ) {
    if ( speeds ) *speeds = NULL;
    /* 1. průchod: pozice posledního celého bloku (dostane pauzu 0) a počet bloků */
    uint32_t pos = 0, last_pos = UINT32_MAX, count = 0;
    const uint8_t *block = NULL;
    uint16_t len = 0;
    en_CMTTMZ_TAP_ITEM it;
    while ( ( it = cmttmz_tap_next ( data, size, &pos, &block, &len ) ) == CMTTMZ_TAP_BLOCK || it == CMTTMZ_TAP_EMPTY ) {
        if ( it == CMTTMZ_TAP_BLOCK ) {
            last_pos = ( uint32_t ) ( block - data );
            count++;
        }
    }
    if ( last_pos == UINT32_MAX ) {
        cmttmz_rep_error ( rep, "No playable block in TAP file" );
        return NULL;
    }

    st_TZX_FILE *file = cmttmz_file_new ( );
    st_CMTTMZ_BLOCK_SPEED *sp = NULL;
    if ( file && speeds ) {
        sp = ( st_CMTTMZ_BLOCK_SPEED* ) calloc ( count, sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
        if ( !sp ) {
            tzx_free ( file );
            file = NULL;
        }
    }
    if ( !file ) {
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }
    /* 2. průchod: bloky a varování */
    pos = 0;
    while ( ( it = cmttmz_tap_next ( data, size, &pos, &block, &len ) ) != CMTTMZ_TAP_END ) {
        if ( it == CMTTMZ_TAP_EMPTY ) {
            cmttmz_rep_warning ( rep, "TAP file contains an empty block, ignored" );
            continue;
        }
        if ( it == CMTTMZ_TAP_DAMAGED ) {
            cmttmz_rep_warning ( rep, "TAP file has a damaged end, incomplete block ignored" );
            break;
        }
        uint16_t pause_ms = 0;
        if ( ( uint32_t ) ( block - data ) != last_pos ) {
            pause_ms = ( block[0] < 0x80 ) ? CMTTMZ_TAP_PAUSE_HEADER_MS : CMTTMZ_TAP_PAUSE_DATA_MS;
        }
        if ( EXIT_SUCCESS != cmttmz_file_add_tap_block ( file, block, len, pause_ms ) ) {
            tzx_free ( file );
            free ( sp );
            cmttmz_rep_error ( rep, "Not enough memory" );
            return NULL;
        }
        if ( sp ) {
            /* 2. průchod nesmí dát víc bloků než 1. (stejné čtení dat); jinak chyba, ne krátké pole */
            if ( file->block_count > count ) {
                tzx_free ( file );
                free ( sp );
                cmttmz_rep_error ( rep, "Internal error: TAP block count mismatch" );
                return NULL;
            }
            st_CMTTMZ_BLOCK_SPEED *s = &sp[file->block_count - 1];
            s->origin = CMTTMZ_SPEED_ORIGIN_FIXED;
            s->speed = CMTSPEED_1_1;
            s->pause_ms = pause_ms;
        }
    }
    if ( speeds ) *speeds = sp;
    return file;
}


/**
 * @brief Pole délek pulzů WAV, které roste s počtem hran.
 *
 * @invariant count <= capacity; pulses == NULL právě když capacity == 0.
 * @par Vlastnictví: pulses vlastní struktura, uvolní se free().
 */
typedef struct st_CMTTMZ_PULSES {
    uint32_t *pulses;     /**< délky pulzů ve vzorcích */
    uint32_t count;       /**< počet uložených pulzů */
    uint32_t capacity;    /**< kapacita pole */
} st_CMTTMZ_PULSES;


/**
 * @brief Přidá pulz na konec pole (při plném poli ho zdvojnásobí).
 * @param p Pole. @param length Délka pulzu ve vzorcích (>= 1).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace (pole beze změny).
 */
static int cmttmz_pulses_push ( st_CMTTMZ_PULSES *p, uint32_t length ) {
    if ( p->count == p->capacity ) {
        uint32_t cap = p->capacity ? p->capacity * 2 : CMTTMZ_WAV_PULSES_INITIAL;
        if ( cap <= p->capacity ) return EXIT_FAILURE;
        uint32_t *n = ( uint32_t* ) realloc ( p->pulses, ( size_t ) cap * sizeof ( uint32_t ) );
        if ( !n ) return EXIT_FAILURE;
        p->pulses = n;
        p->capacity = cap;
    }
    p->pulses[p->count++] = length;
    return EXIT_SUCCESS;
}


/**
 * @brief Prahování vzorku na 1 bit jako dřívější rozšíření WAV (už neexistuje).
 *
 * Stejné pravidlo jako wav_get_bit_value_of_sample() (wav.c), které volá
 * cmt_bitstream_new_from_wav() pro rozšíření WAV: normální polarita
 * = záporný vzorek je 1, obrácená = kladný vzorek je 1; nula je vždy 0.
 *
 * @param value Normalizovaný vzorek (wav_decode_sample()).
 * @param polarity Polarita.
 * @return 0 (LOW) nebo 1 (HIGH).
 */
static int cmttmz_wav_bit ( double value, en_CMT_STREAM_POLARITY polarity ) {
    if ( polarity == CMT_STREAM_POLARITY_NORMAL ) return ( value < 0.0 ) ? 1 : 0;
    return ( value > 0.0 ) ? 1 : 0;
}


/**
 * @brief Import WAV: první kanál, prahování na 1 bit, jeden blok 0x18 (CSW RLE).
 *
 * Hlavičku čte knihovna wav (PCM, IEEE float, EXTENSIBLE) přes handler,
 * který jen čte buffer volajícího (bez kopie). Vzorky se dekódují
 * wav_decode_sample() přímo z bufferu a prahují cmttmz_wav_bit(); do paměti
 * se ukládají jen délky běhů (pole roste s počtem hran). Signál, který
 * začíná HIGH, dostane na začátek pulz LOW délky 1 vzorek (první pulz CSW
 * je LOW). Poslední běh do konce dat je také pulz. Blok má frekvenci
 * souboru a pauzu 0 (jediný, tedy poslední blok pásky).
 *
 * Data chunk delší než soubor: přečtou se celé rámce, které v souboru
 * jsou, s varováním "WAV data chunk is longer than file, missing samples ignored".
 * Hlavička s počtem kanálů 0 dá chybu "WAV header is corrupted".
 *
 * @param data Data (NULL jen při size 0). @param size Délka dat.
 * @param polarity Polarita prahování.
 * @param rep Report (smí být NULL).
 * @return Nový soubor, nebo NULL: "Can't read WAV file: " + wav_error_string(),
 *         "WAV header is corrupted" (0 kanálů),
 *         "WAV contains no signal" (žádná hrana), "Can't create CSW block: "
 *         + tzx_error_string(), "Not enough memory".
 */
static st_TZX_FILE* cmttmz_import_wav ( const uint8_t *data, uint32_t size, en_CMT_STREAM_POLARITY polarity,
                                        st_CMTTMZ_IMPORT_REPORT *rep ) {
    st_HANDLER h;
    cmttmz_borrow_handler ( &h, data, size );
    en_WAV_ERROR werr = WAV_OK;
    st_WAV_SIMPLE_HEADER *sh = wav_simple_header_new_from_handler ( &h, &werr );
    if ( !sh ) {
        cmttmz_rep_error ( rep, "Can't read WAV file: %s", wav_error_string ( werr ) );
        return NULL;
    }
    if ( sh->channels == 0 ) {
        cmttmz_rep_error ( rep, "WAV header is corrupted" );
        wav_simple_header_destroy ( sh );
        return NULL;
    }
    if ( sh->block_size < sh->bits_per_sample / 8 ) {
        cmttmz_rep_error ( rep, "Can't read WAV file: %s", wav_error_string ( WAV_ERROR_CORRUPT_HEADER ) );
        wav_simple_header_destroy ( sh );
        return NULL;
    }
    uint32_t frames = sh->blocks;
    uint32_t offset = sh->real_data_offset;
    uint32_t fit = ( offset < size ) ? ( size - offset ) / sh->block_size : 0;
    if ( fit < frames ) {
        frames = fit;
        cmttmz_rep_warning ( rep, "WAV data chunk is longer than file, missing samples ignored" );
    }

    st_CMTTMZ_PULSES p = { NULL, 0, 0 };
    int ok = 1;
    int level = 0;
    uint32_t run = 0;
    uint32_t edges = 0;
    for ( uint32_t i = 0; ok && i < frames; i++ ) {
        double v = 0.0;
        en_WAV_ERROR derr = wav_decode_sample ( sh, data + offset + ( size_t ) i * sh->block_size, &v );
        if ( WAV_OK != derr ) {
            cmttmz_rep_error ( rep, "Can't read WAV file: %s", wav_error_string ( derr ) );
            free ( p.pulses );
            wav_simple_header_destroy ( sh );
            return NULL;
        }
        int bit = cmttmz_wav_bit ( v, polarity );
        if ( i == 0 ) {
            level = bit;
            /* první pulz CSW je LOW: signál začínající HIGH dostane LOW 1 vzorek */
            if ( bit ) ok = ( EXIT_SUCCESS == cmttmz_pulses_push ( &p, 1 ) );
        } else if ( bit != level ) {
            ok = ( EXIT_SUCCESS == cmttmz_pulses_push ( &p, run ) );
            level = bit;
            run = 0;
            edges++;
        }
        run++;
    }
    uint32_t rate = sh->sample_rate;
    wav_simple_header_destroy ( sh );
    if ( ok && run ) ok = ( EXIT_SUCCESS == cmttmz_pulses_push ( &p, run ) );
    if ( !ok ) {
        free ( p.pulses );
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }
    if ( edges == 0 ) {
        free ( p.pulses );
        cmttmz_rep_error ( rep, "WAV contains no signal" );
        return NULL;
    }

    st_TZX_BLOCK block;
    en_TZX_ERROR terr = tzx_block_create_csw_recording ( rate, 0, p.pulses, p.count, &block );
    free ( p.pulses );
    if ( terr != TZX_OK ) {
        cmttmz_rep_error ( rep, "Can't create CSW block: %s", tzx_error_string ( terr ) );
        return NULL;
    }
    st_TZX_FILE *file = cmttmz_file_new ( );
    if ( !file || TZX_OK != tzx_file_append_block ( file, &block ) ) {
        free ( block.data );
        tzx_free ( file );
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }
    return file;
}


/**
 * @brief Vytvoří pole rychlostí pro soubor, jehož bloky mají všechny stejný záznam.
 *
 * @param count Počet bloků (> 0).
 * @param proto Záznam, který dostane každý blok.
 * @param rep Report (smí být NULL); při selhání alokace dostane chybu "Not enough memory".
 * @return Nové pole (uvolnit free()), nebo NULL při selhání alokace.
 */
static st_CMTTMZ_BLOCK_SPEED* cmttmz_speeds_uniform ( uint32_t count, const st_CMTTMZ_BLOCK_SPEED *proto,
                                                     st_CMTTMZ_IMPORT_REPORT *rep ) {
    st_CMTTMZ_BLOCK_SPEED *sp = ( st_CMTTMZ_BLOCK_SPEED* ) calloc ( count ? count : 1, sizeof ( st_CMTTMZ_BLOCK_SPEED ) );
    if ( !sp ) {
        cmttmz_rep_error ( rep, "Not enough memory" );
        return NULL;
    }
    for ( uint32_t i = 0; i < count; i++ ) sp[i] = *proto;
    return sp;
}


/**
 * @brief Rychlost bloku 0x46 s profilem MZ800EMU-CMT načteného z TMZ.
 *
 * Z bloku přečte ID profilu (tmz_mzfile_parse()) a text PROFILE_ARGS
 * (blokové TLV, tag TMZ_TAG_PROFILE_ARGS) a převede ho
 * tmz_profile_args_parse(). Profil MZ800EMU-CMT zapisuje
 * "speed=a:b,pause=N" nebo "cmtspeed=LH/LL/SH/SL,pause=N" (µs). Při
 * úspěchu nastaví záznam na CMTTMZ_SPEED_ORIGIN_FIXED s touto rychlostí
 * (vlastní pulzy jako CMTSPEED_CUSTOM) a pauzou; jiný blok, jiný profil,
 * nečitelné PROFILE_ARGS nebo pauza nad 65535 ms záznam nezmění.
 *
 * PROFILE_ARGS se berou jako pravdivé: neporovnávají se s částmi bloku
 * (pulzy, pauzy ve skutečných částech). Přestavění takového bloku jinou
 * rychlostí ho vygeneruje znovu jen z MZF (hlavička a tělo) a profilu
 * MZ800EMU-CMT; ostatní data bloku (další bloková TLV, odlišné části) se
 * tím zahodí.
 *
 * @param block Blok souboru.
 * @param[in,out] sp Záznam rychlosti bloku (na vstupu ORIGIN_NONE).
 * @note Data bloku se nemění.
 */
static void cmttmz_speed_from_emu_block ( const st_TZX_BLOCK *block, st_CMTTMZ_BLOCK_SPEED *sp ) {
    if ( block->id != TMZ_BLOCK_ID_MZ_FILE ) return;
    st_TMZ_MZFILE mf;
    if ( TZX_OK != tmz_mzfile_parse ( block, &mf ) ) return;
    int ok = ( 0 == strcmp ( mf.profile_id, TMZ_PROFILE_ID_MZ800EMU_CMT ) );
    const char *text = NULL;
    uint16_t text_len = 0;
    if ( ok ) {
        st_TMZ_TLV_ITER it;
        st_TMZ_TLV t;
        tmz_tlv_iter_init ( &it, mf.block_tlv, mf.block_tlv_length );
        while ( tmz_tlv_iter_next ( &it, &t ) == 1 ) {
            if ( t.tag == TMZ_TAG_PROFILE_ARGS ) {
                text = ( const char* ) t.value;
                text_len = t.length;
                break;
            }
        }
    }
    st_TMZ_PROFILE_ARGS args;
    ok = ok && text && ( EXIT_SUCCESS == tmz_profile_args_parse ( text, text_len, &args ) ) && ( args.pause_ms <= 0xFFFF );
    tmz_mzfile_free ( &mf );
    if ( !ok ) return;

    st_CMTTMZ_BLOCK_SPEED r;
    memset ( &r, 0, sizeof ( r ) );
    r.origin = CMTTMZ_SPEED_ORIGIN_FIXED;
    r.pause_ms = ( uint16_t ) args.pause_ms;
    if ( args.cmtspeed[0] || args.cmtspeed[1] || args.cmtspeed[2] || args.cmtspeed[3] ) {
        if ( EXIT_SUCCESS != mztape_pulses_set_us ( &r.pulses, args.cmtspeed[0], args.cmtspeed[1], args.cmtspeed[2],
                                                    args.cmtspeed[3] ) ) return;
        r.speed = CMTSPEED_CUSTOM;
    } else {
        r.speed = ( args.speed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : args.speed;
        if ( !cmtspeed_is_valid ( r.speed ) ) return;
    }
    *sp = r;
}


st_TZX_FILE* cmttmz_import_buffer_speeds ( en_CMTTMZ_SOURCE source, const uint8_t *data, uint32_t size,
                                           const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep,
                                           st_CMTTMZ_BLOCK_SPEED **speeds ) {
    if ( speeds ) *speeds = NULL;
    cmttmz_rep_reset ( rep, source );
    st_CMTTMZ_IMPORT_OPTS defaults;
    if ( !opts ) {
        cmttmz_import_opts_init ( &defaults );
        opts = &defaults;
    }

    st_TZX_FILE *file = NULL;
    st_CMTTMZ_BLOCK_SPEED proto;
    memset ( &proto, 0, sizeof ( proto ) );
    switch ( source ) {
        case CMTTMZ_SOURCE_MZF:
            file = cmttmz_import_mzf ( data, size, opts, rep );
            /* jediný blok výchozí rychlostí, pauza 0 (cmttmz_import_mzf()) */
            proto.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
            proto.speed = opts->default_speed;
            if ( cmtspeed_is_custom ( opts->default_speed ) ) proto.pulses = opts->custom_pulses;
            break;
        case CMTTMZ_SOURCE_MZT:
            return cmttmz_import_mzt ( data, size, opts, rep, speeds );
        case CMTTMZ_SOURCE_TMZ:
            file = cmttmz_import_tmz ( data, size, rep );
            break;
        case CMTTMZ_SOURCE_TAP:
            return cmttmz_import_tap ( data, size, rep, speeds );
        case CMTTMZ_SOURCE_WAV:
            file = cmttmz_import_wav ( data, size, opts->wav_polarity, rep );
            break;
        default:
            cmttmz_rep_error ( rep, "Unknown tape source" );
            return NULL;
    }
    if ( !file || !speeds ) return file;
    *speeds = cmttmz_speeds_uniform ( file->block_count, &proto, rep );
    if ( !*speeds ) {
        tzx_free ( file );
        return NULL;
    }
    if ( source == CMTTMZ_SOURCE_TMZ ) {
        for ( uint32_t i = 0; i < file->block_count; i++ ) cmttmz_speed_from_emu_block ( &file->blocks[i], &( *speeds )[i] );
    }
    return file;
}


st_TZX_FILE* cmttmz_import_buffer ( en_CMTTMZ_SOURCE source, const uint8_t *data, uint32_t size,
                                    const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep ) {
    return cmttmz_import_buffer_speeds ( source, data, size, opts, rep, NULL );
}


st_TZX_FILE* cmttmz_import_file ( const char *path, const st_CMTTMZ_IMPORT_OPTS *opts, st_CMTTMZ_IMPORT_REPORT *rep ) {
    return cmttmz_import_file_speeds ( path, opts, rep, NULL );
}


st_TZX_FILE* cmttmz_import_file_speeds ( const char *path, const st_CMTTMZ_IMPORT_OPTS *opts,
                                         st_CMTTMZ_IMPORT_REPORT *rep, st_CMTTMZ_BLOCK_SPEED **speeds ) {
    if ( speeds ) *speeds = NULL;
    en_CMTTMZ_SOURCE source = CMTTMZ_SOURCE_TMZ;
    cmttmz_rep_reset ( rep, source );
    if ( EXIT_SUCCESS != cmttmz_source_from_filename ( path, &source ) ) {
        cmttmz_rep_error ( rep, "Unsupported tape file extension" );
        return NULL;
    }
    if ( rep ) rep->source = source;

    /* soubor nad 4 GB by délka bufferu (uint32_t) tiše usekla: odmítnout před načtením */
    GStatBuf stat_buf;
    if ( 0 == g_stat ( path, &stat_buf ) && ( uint64_t ) stat_buf.st_size > UINT32_MAX ) {
        cmttmz_rep_error ( rep, "Tape file is too large" );
        return NULL;
    }

    /* celý soubor do paměti jako dřívější rozšíření CMT pro MZF, handler na zásobníku */
    st_HANDLER handler;
    st_HANDLER *h = generic_driver_open_memory_from_file ( &handler, &g_memory_driver_static, path );
    if ( !h || h->err || !h->spec.memspec.ptr ) {
        if ( h ) generic_driver_close ( h );
        cmttmz_rep_error ( rep, "Can't open file %s", path );
        return NULL;
    }

    if ( ( uint64_t ) h->spec.memspec.size > UINT32_MAX ) {
        generic_driver_close ( h );
        cmttmz_rep_error ( rep, "Tape file is too large" );
        return NULL;
    }
    st_TZX_FILE *file = cmttmz_import_buffer_speeds ( source, h->spec.memspec.ptr, ( uint32_t ) h->spec.memspec.size,
                                                      opts, rep, speeds );
    generic_driver_close ( h );
    return file;
}
