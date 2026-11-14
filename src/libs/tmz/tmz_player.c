/**
 * @file   tmz_player.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.6.0
 * @brief  Implementace TMZ playeru - generovani CMT audio streamu z TMZ/TZX bloku.
 *
 * Player prehrava jednotlive TMZ/TZX bloky na CMT audio streamy pomoci
 * existujicich knihoven mztape (Sharp MZ formaty) a zxtape (ZX Spectrum).
 *
 * Pro MZ-specificke bloky (0x40-0x4F) se vyuziva mztape knihovna
 * s parametry z bloku. Pro standardni TZX bloky (0x10) se vyuziva zxtape.
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
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>

#include "../cmt_stream/cmt_stream.h"
#include "../mztape/mztape.h"
#include "../zxtape/zxtape.h"
#include "../tzx/tzx.h"
#include "../endianity/endianity.h"
#include "../mzcmt_turbo/mzcmt_turbo.h"
#include "../mzcmt_fastipl/mzcmt_fastipl.h"
#include "../mzcmt_fsk/mzcmt_fsk.h"
#include "../mzcmt_slow/mzcmt_slow.h"
#include "../mzcmt_direct/mzcmt_direct.h"

#include "tmz.h"
#include "tmz_blocks.h"
#include "tmz_player.h"
#include "tmz_tlv.h"
#include "tmz_mzfile.h"
#include "tmz_part_fm.h"
#include "tmz_part_manchester.h"
#include "tmz_v1conv.h"
#include "../mzcmt_fm/mzcmt_fm.h"
#include "../mzcmt_manchester/mzcmt_manchester.h"


/**
 * @brief Vychozi error callback pro tmz_player modul.
 * @param func Nazev volajici funkce.
 * @param line Cislo radku.
 * @param fmt Formatovaci retezec (printf styl).
 */
static void tmz_player_default_error_cb ( const char *func, int line, const char *fmt, ... ) {
    va_list args;
    fprintf ( stderr, "%s():%d - ", func, line );
    va_start ( args, fmt );
    vfprintf ( stderr, fmt, args );
    va_end ( args );
}

/** @brief Aktualne aktivni error callback. */
static void (*g_player_error_cb)( const char*, int, const char*, ... ) = tmz_player_default_error_cb;


/* ========================================================================= */
/*  Chybove retezce                                                          */
/* ========================================================================= */

/**
 * @brief Vrati textovy popis chyboveho kodu TMZ playeru.
 *
 * Vzdy vraci platny retezec (nikdy NULL).
 *
 * @param err Chybovy kod.
 * @return Textovy popis chyby.
 */
const char* tmz_player_error_string ( en_TMZ_PLAYER_ERROR err ) {
    switch ( err ) {
        case TMZ_PLAYER_OK:               return "OK";
        case TMZ_PLAYER_ERROR_NULL_INPUT:  return "NULL input parameter";
        case TMZ_PLAYER_ERROR_NO_BLOCKS:   return "No blocks in file";
        case TMZ_PLAYER_ERROR_UNSUPPORTED: return "Unsupported block type";
        case TMZ_PLAYER_ERROR_STREAM_CREATE: return "Failed to create CMT stream";
        case TMZ_PLAYER_ERROR_ALLOC:       return "Memory allocation failed";
        case TMZ_PLAYER_ERROR_UNREADABLE:  return "Signal not readable at this sample rate";
        default:                           return "Unknown player error";
    }
}


/* ========================================================================= */
/*  Konfigurace                                                              */
/* ========================================================================= */

/**
 * @brief Inicializuje konfiguraci playeru na vychozi hodnoty.
 *
 * Nastavi: sample_rate = 44100, stream_type = vstream,
 * default_pulseset = MZ-800, default_speed = 1:1,
 * default_formatset = MZ800_SANE, mz_cpu_clock = 0, v1_convert = 1.
 *
 * @param config Ukazatel na konfiguraci k inicializaci.
 *
 * @pre config nesmi byt NULL.
 * @post Vsechna pole konfigurace jsou nastavena na rozumne vychozi hodnoty.
 */
void tmz_player_config_init ( st_TMZ_PLAYER_CONFIG *config ) {
    if ( !config ) return;
    config->sample_rate = CMTSTREAM_DEFAULT_RATE;
    config->stream_type = CMT_STREAM_TYPE_VSTREAM;
    config->default_pulseset = MZTAPE_PULSESET_800;
    config->default_speed = CMTSPEED_1_1;
    config->default_formatset = MZTAPE_FORMATSET_MZ800_SANE;
    config->mz_cpu_clock = 0;
    config->v1_convert = 1;
}


/* ========================================================================= */
/*  Pomocne funkce                                                           */
/* ========================================================================= */

/**
 * @brief Prevede en_TMZ_PULSESET na en_MZCMT_TURBO_PULSESET.
 *
 * @param pulseset TMZ pulsni sada.
 * @return Odpovidajici TURBO encoder pulsni sada.
 */
static en_MZCMT_TURBO_PULSESET tmz_to_turbo_enc_pulseset ( uint8_t pulseset ) {
    switch ( pulseset ) {
        case TMZ_PULSESET_700: return MZCMT_TURBO_PULSESET_700;
        case TMZ_PULSESET_800: return MZCMT_TURBO_PULSESET_800;
        case TMZ_PULSESET_80B: return MZCMT_TURBO_PULSESET_80B;
        default: return MZCMT_TURBO_PULSESET_800;
    }
}


/**
 * @brief Prevede en_TMZ_PULSESET na en_MZCMT_FASTIPL_PULSESET.
 *
 * @param pulseset TMZ pulsni sada.
 * @return Odpovidajici FASTIPL encoder pulsni sada.
 */
static en_MZCMT_FASTIPL_PULSESET tmz_to_fastipl_pulseset ( uint8_t pulseset ) {
    switch ( pulseset ) {
        case TMZ_PULSESET_700: return MZCMT_FASTIPL_PULSESET_700;
        case TMZ_PULSESET_800: return MZCMT_FASTIPL_PULSESET_800;
        case TMZ_PULSESET_80B: return MZCMT_FASTIPL_PULSESET_80B;
        default: return MZCMT_FASTIPL_PULSESET_800;
    }
}


/* tmz_to_cpm_pulseset: smazano spolu s mzcmt_cpm */


/**
 * @brief Vytvori st_MZTAPE_MZF z MZF hlavicky a tela.
 *
 * Alokuje strukturu, zkopiruje hlavicku a telo, spocita checksums.
 *
 * @param mzf_header MZF hlavicka (v host byte order).
 * @param body Data tela.
 * @param body_size Velikost tela.
 * @return Nova struktura, nebo NULL pri chybe.
 *
 * @post Volajici musi uvolnit pres mztape_mztmzf_destroy().
 */
static st_MZTAPE_MZF* tmz_player_create_mztmzf ( const st_MZF_HEADER *mzf_header,
                                                   const uint8_t *body, uint16_t body_size ) {

    st_MZTAPE_MZF *mztmzf = calloc ( 1, sizeof ( st_MZTAPE_MZF ) );
    if ( !mztmzf ) return NULL;

    /* hlavicka se uklada v raw (LE) formatu pro mztape */
    memcpy ( mztmzf->header, mzf_header, sizeof ( st_MZF_HEADER ) );
    /* zpět do LE pro mztape (raw hlavička); 11h zůstává, jak ho vrátil parser bloku */
    mzf_header_swap_endianity ( (st_MZF_HEADER*) mztmzf->header );

    mztmzf->size = body_size;

    if ( body_size > 0 && body ) {
        mztmzf->body = malloc ( body_size );
        if ( !mztmzf->body ) {
            free ( mztmzf );
            return NULL;
        }
        memcpy ( mztmzf->body, body, body_size );
    }

    /* vypocet checksumu (z raw dat) */
    uint32_t chkh = 0;
    for ( uint32_t i = 0; i < sizeof ( st_MZF_HEADER ); i++ ) {
        uint8_t byte = mztmzf->header[i];
        for ( int bit = 0; bit < 8; bit++ ) {
            if ( byte & 1 ) chkh++;
            byte >>= 1;
        }
    }
    mztmzf->chkh = chkh;

    uint32_t chkb = 0;
    if ( mztmzf->body ) {
        for ( uint32_t i = 0; i < body_size; i++ ) {
            uint8_t byte = mztmzf->body[i];
            for ( int bit = 0; bit < 8; bit++ ) {
                if ( byte & 1 ) chkb++;
                byte >>= 1;
            }
        }
    }
    mztmzf->chkb = chkb;

    return mztmzf;
}


/* ========================================================================= */
/*  Prehravani jednotlivych bloku                                            */
/* ========================================================================= */

/**
 * @brief Připojí za stream bloku v1.0 pauzu pause_ms jako ticho na nízké úrovni.
 *
 * Jen u vstreamu; bitstream se nemění. Úroveň LOW odpovídá výchozí úrovni
 * pauzy převodu v1conv (pause_level 0).
 *
 * @param s Stream bloku (smí být NULL, pak nic). @param pause_ms Pauza v ms.
 * @param config Konfigurace (vzorkovací frekvence).
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE při selhání zápisu do vstreamu.
 */
static int append_v1_pause ( st_CMT_STREAM *s, uint32_t pause_ms, const st_TMZ_PLAYER_CONFIG *config ) {
    if ( !s || pause_ms == 0 || s->stream_type != CMT_STREAM_TYPE_VSTREAM ) return EXIT_SUCCESS;
    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    uint32_t n = ( uint32_t ) ( ( uint64_t ) pause_ms * rate / 1000u );
    if ( n == 0 ) return EXIT_SUCCESS;
    return cmt_vstream_add_value ( s->str.vstream, 0, n );
}


/**
 * @brief Prehaje blok 0x40 (MZ Standard Data) na CMT stream.
 *
 * Naparsuje blok, vytvori st_MZTAPE_MZF a pouzije mztape knihovnu
 * pro generovani CMT streamu ve standardni rychlosti 1:1. Za signál
 * připojí pause_ms bloku jako ticho na nízké úrovni (jen vstream).
 *
 * @param block TMZ blok s ID 0x40.
 * @param config Konfigurace playeru.
 * @param err Vystupni chybovy kod (muze byt NULL).
 * @return Novy CMT stream, nebo NULL pri chybe.
 *
 * @pre block nesmi byt NULL a musi mit id == TMZ_BLOCK_ID_MZ_STANDARD_DATA.
 * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* tmz_player_play_mz_standard ( const st_TZX_BLOCK *block,
                                              const st_TMZ_PLAYER_CONFIG *config,
                                              en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }

    /* parsovani bloku */
    en_TZX_ERROR parse_err;
    uint8_t *body_data = NULL;

    /* musime pracovat s kopii bloku, protoze parse funkce modifikuji data (endianita) */
    st_TZX_BLOCK block_copy = *block;
    uint8_t *data_copy = malloc ( block->length );
    if ( !data_copy ) {
        *err = TMZ_PLAYER_ERROR_ALLOC;
        return NULL;
    }
    memcpy ( data_copy, block->data, block->length );
    block_copy.data = data_copy;

    st_TMZ_MZ_STANDARD_DATA *std_data = tmz_block_parse_mz_standard ( &block_copy, &body_data, &parse_err );
    if ( !std_data ) {
        g_player_error_cb ( __func__, __LINE__, "Failed to parse MZ Standard Data block\n" );
        free ( data_copy );
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;   /* neplatný blok: trvalá chyba */
        return NULL;
    }

    uint16_t pause_ms = std_data->pause_ms;   /* std_data ukazuje do data_copy */

    /* vytvoreni mztape MZF */
    st_MZTAPE_MZF *mztmzf = tmz_player_create_mztmzf ( &std_data->mzf_header, body_data, std_data->body_size );
    free ( data_copy );

    if ( !mztmzf ) {
        *err = TMZ_PLAYER_ERROR_ALLOC;
        return NULL;
    }

    /* generovani CMT streamu */
    st_CMT_STREAM *stream = mztape_create_stream_from_mztapemzf (
        mztmzf, CMTSPEED_1_1, config->stream_type,
        config->default_formatset, config->sample_rate );

    mztape_mztmzf_destroy ( mztmzf );

    if ( !stream || append_v1_pause ( stream, pause_ms, config ) != EXIT_SUCCESS ) {
        g_player_error_cb ( __func__, __LINE__, "Failed to create CMT stream from MZ Standard Data\n" );
        cmt_stream_destroy ( stream );
        *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
        return NULL;
    }

    *err = TMZ_PLAYER_OK;
    return stream;
}


/* ========================================================================= */
/*  Format-specificke helpery pro blok 0x41                                  */
/* ========================================================================= */

/**
 * @brief Prehaje blok 0x41 s formatem NORMAL (standardni FM).
 *
 * Pouzije mztape knihovnu se zadanou rychlosti. Odpovida puvodnimu
 * chovani playeru, kdy format pole bylo ignorovano.
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_normal ( const st_TMZ_MZ_TURBO_DATA *td,
                                               const uint8_t *body_data,
                                               const st_TMZ_PLAYER_CONFIG *config ) {

    en_CMTSPEED speed = (en_CMTSPEED) td->speed;
    if ( !cmtspeed_is_valid ( speed ) ) speed = config->default_speed;

    st_MZTAPE_MZF *mztmzf = tmz_player_create_mztmzf ( &td->mzf_header, body_data, td->body_size );
    if ( !mztmzf ) return NULL;

    st_CMT_STREAM *stream = mztape_create_stream_from_mztapemzf_ex (
        mztmzf, speed, config->stream_type,
        config->default_formatset, config->sample_rate,
        td->long_high, td->long_low,
        td->short_high, td->short_low );

    mztape_mztmzf_destroy ( mztmzf );
    return stream;
}


/**
 * @brief Prehaje blok 0x41 s formatem TURBO (FM s konfigurovatelnym casovanim).
 *
 * Pouzije mzcmt_turbo koder s kompletnim paskovym signalem vcetne
 * TURBO loaderu v NORMAL FM 1:1 (loader modifikuje readpoint a cte
 * uzivatelska data pri TURBO rychlosti).
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_turbo ( const st_TMZ_MZ_TURBO_DATA *td,
                                              const uint8_t *body_data,
                                              const st_TMZ_PLAYER_CONFIG *config ) {

    /* konfigurace TURBO koderu z parametru bloku */
    st_MZCMT_TURBO_CONFIG tcfg;
    memset ( &tcfg, 0, sizeof ( tcfg ) );
    tcfg.pulseset = tmz_to_turbo_enc_pulseset ( td->pulseset );
    tcfg.speed = (en_CMTSPEED) td->speed;
    if ( !cmtspeed_is_valid ( tcfg.speed ) ) tcfg.speed = config->default_speed;
    tcfg.lgap_length = td->lgap_length;
    tcfg.sgap_length = td->sgap_length;
    tcfg.long_high_us100 = td->long_high;
    tcfg.long_low_us100 = td->long_low;
    tcfg.short_high_us100 = td->short_high;
    tcfg.short_low_us100 = td->short_low;
    tcfg.flags = td->flags;

    return mzcmt_turbo_create_tape_stream (
        &td->mzf_header, body_data, td->body_size,
        &tcfg, config->stream_type, config->sample_rate );
}


/**
 * @brief Prehaje blok 0x41 s formatem FASTIPL ($BB prefix, Intercopy).
 *
 * Pouzije mzcmt_fastipl koder. Generuje dvoudilny signal:
 * $BB hlavicka pri 1:1 rychlosti, pauza, datove telo pri turbo rychlosti.
 * Verze loaderu je V07 (InterCopy v7+), pokud neni v bloku specifikovano jinak.
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_fastipl ( const st_TMZ_MZ_TURBO_DATA *td,
                                                const uint8_t *body_data,
                                                const st_TMZ_PLAYER_CONFIG *config ) {

    /* konfigurace FASTIPL koderu */
    st_MZCMT_FASTIPL_CONFIG fcfg;
    memset ( &fcfg, 0, sizeof ( fcfg ) );
    fcfg.version = MZCMT_FASTIPL_VERSION_V07;
    fcfg.pulseset = tmz_to_fastipl_pulseset ( td->pulseset );
    fcfg.speed = (en_CMTSPEED) td->speed;
    if ( !cmtspeed_is_valid ( fcfg.speed ) ) fcfg.speed = config->default_speed;
    fcfg.lgap_length = td->lgap_length;
    fcfg.long_high_us100 = td->long_high;
    fcfg.long_low_us100 = td->long_low;
    fcfg.short_high_us100 = td->short_high;
    fcfg.short_low_us100 = td->short_low;

    /* FASTIPL ocekava MZF hlavicku v host byte order */
    return mzcmt_fastipl_create_stream (
        &td->mzf_header, body_data, td->body_size,
        &fcfg, config->stream_type, config->sample_rate );
}


/**
 * @brief Prehaje blok 0x41 s formatem FSK (Frequency Shift Keying).
 *
 * Pouzije mzcmt_fsk koder s kompletnim paskovym signalem: preloader
 * v NORMAL FM 1:1 + FSK loader body v NORMAL FM + uzivatelska data
 * v FSK formatu.
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_fsk ( const st_TMZ_MZ_TURBO_DATA *td,
                                            const uint8_t *body_data,
                                            const st_TMZ_PLAYER_CONFIG *config ) {

    /* konfigurace FSK tape koderu */
    st_MZCMT_FSK_TAPE_CONFIG fcfg;
    memset ( &fcfg, 0, sizeof ( fcfg ) );
    fcfg.pulseset = (en_MZCMT_FSK_PULSESET) tmz_to_turbo_enc_pulseset ( td->pulseset );
    fcfg.speed = (en_MZCMT_FSK_SPEED) td->speed;
    if ( fcfg.speed >= MZCMT_FSK_SPEED_COUNT ) fcfg.speed = MZCMT_FSK_SPEED_0;
    fcfg.loader_speed = 0; /* standardni ROM rychlost pro nacteni FSK loaderu */

    return mzcmt_fsk_create_tape_stream (
        &td->mzf_header, body_data, td->body_size,
        &fcfg, config->stream_type, config->sample_rate );
}


/**
 * @brief Prehaje blok 0x41 s formatem SLOW (2 bity na pulz).
 *
 * Pouzije mzcmt_slow koder s kompletnim paskovym signalem: preloader
 * v NORMAL FM 1:1 + SLOW loader body v NORMAL FM + uzivatelska data
 * v SLOW formatu.
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_slow ( const st_TMZ_MZ_TURBO_DATA *td,
                                             const uint8_t *body_data,
                                             const st_TMZ_PLAYER_CONFIG *config ) {

    /* konfigurace SLOW tape koderu */
    st_MZCMT_SLOW_TAPE_CONFIG scfg;
    memset ( &scfg, 0, sizeof ( scfg ) );
    scfg.pulseset = (en_MZCMT_SLOW_PULSESET) tmz_to_turbo_enc_pulseset ( td->pulseset );
    scfg.speed = (en_MZCMT_SLOW_SPEED) td->speed;
    if ( scfg.speed >= MZCMT_SLOW_SPEED_COUNT ) scfg.speed = MZCMT_SLOW_SPEED_0;
    scfg.loader_speed = 0;

    return mzcmt_slow_create_tape_stream (
        &td->mzf_header, body_data, td->body_size,
        &scfg, config->stream_type, config->sample_rate );
}


/**
 * @brief Prehaje blok 0x41 s formatem DIRECT (primy bitovy zapis).
 *
 * Pouzije mzcmt_direct koder s kompletnim paskovym signalem: preloader
 * v NORMAL FM 1:1 + DIRECT loader body v NORMAL FM + uzivatelska data
 * v DIRECT formatu.
 *
 * @param td         Rozparsovana turbo data (host byte order).
 * @param body_data  Datove telo.
 * @param config     Konfigurace playeru.
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* play_turbo_fmt_direct ( const st_TMZ_MZ_TURBO_DATA *td,
                                               const uint8_t *body_data,
                                               const st_TMZ_PLAYER_CONFIG *config ) {

    /* konfigurace DIRECT tape koderu */
    st_MZCMT_DIRECT_TAPE_CONFIG dcfg;
    memset ( &dcfg, 0, sizeof ( dcfg ) );
    dcfg.pulseset = (en_MZCMT_DIRECT_PULSESET) tmz_to_turbo_enc_pulseset ( td->pulseset );
    dcfg.loader_speed = 0;

    return mzcmt_direct_create_tape_stream (
        &td->mzf_header, body_data, td->body_size,
        &dcfg, config->stream_type, config->sample_rate );
}


/* ========================================================================= */
/*  Prehravani jednotlivych bloku                                            */
/* ========================================================================= */

/** @brief Úroveň v g_fsl_bad platná pro všechny úrovně (DIRECT nemá úrovně rychlosti). */
#define FSL_ANY_LEVEL 0xFF

/**
 * @brief Vzorkovací frekvence, na kterých se FSK/SLOW/DIRECT měřily v emulátoru.
 *
 * Zdroj: měření zavedení v emulátoru (mz800emu 2.0.6, emulace MZ-800).
 */
static const uint32_t g_fsl_measured_rates[] = { 22050, 32000, 44100, 48000, 88200, 96000 };

/**
 * @brief Kombinace FSK/SLOW/DIRECT, které zavaděč v emulátoru nepřečetl (TIMEOUT/FAIL).
 *
 * Zdroj: měření v emulátoru. level je pole speed bloku
 * 0x41, FSL_ANY_LEVEL = libovolná úroveň. Při 44100 Hz prošly všechny
 * kombinace.
 */
static const struct {
    uint8_t format;   /**< en_TMZ_FORMAT */
    uint8_t level;    /**< úroveň rychlosti (pole speed), nebo FSL_ANY_LEVEL */
    uint32_t rate;    /**< vzorkovací frekvence (Hz) */
} g_fsl_bad[] = {
    { TMZ_FORMAT_FSK, 2, 48000 },
    { TMZ_FORMAT_FSK, 4, 22050 },
    { TMZ_FORMAT_FSK, 5, 22050 },
    { TMZ_FORMAT_FSK, 5, 32000 },
    { TMZ_FORMAT_FSK, 6, 22050 },
    { TMZ_FORMAT_FSK, 6, 32000 },
    { TMZ_FORMAT_FSK, 6, 48000 },
    { TMZ_FORMAT_FSK, 6, 88200 },
    { TMZ_FORMAT_FSK, 6, 96000 },
    { TMZ_FORMAT_SLOW, 0, 22050 },
    { TMZ_FORMAT_SLOW, 1, 22050 },
    { TMZ_FORMAT_SLOW, 1, 88200 },
    { TMZ_FORMAT_SLOW, 1, 96000 },
    { TMZ_FORMAT_SLOW, 2, 22050 },
    { TMZ_FORMAT_SLOW, 2, 32000 },
    { TMZ_FORMAT_SLOW, 3, 22050 },
    { TMZ_FORMAT_SLOW, 4, 22050 },
    { TMZ_FORMAT_SLOW, 4, 32000 },
    { TMZ_FORMAT_SLOW, 4, 88200 },
    { TMZ_FORMAT_SLOW, 4, 96000 },
    { TMZ_FORMAT_DIRECT, FSL_ANY_LEVEL, 22050 },
    { TMZ_FORMAT_DIRECT, FSL_ANY_LEVEL, 32000 },
    { TMZ_FORMAT_DIRECT, FSL_ANY_LEVEL, 48000 },
    { TMZ_FORMAT_DIRECT, FSL_ANY_LEVEL, 88200 },
    { TMZ_FORMAT_DIRECT, FSL_ANY_LEVEL, 96000 },
};


/**
 * @brief Ověří, zda zavaděč FSK/SLOW/DIRECT přečte signál při dané frekvenci.
 * @param format en_TMZ_FORMAT. @param level Úroveň (pole speed bloku). @param rate Frekvence (Hz).
 * @return 1 = změřeno PASS, nebo jiný formát; 0 = změřeno TIMEOUT/FAIL;
 *         -1 = frekvence nezměřená (přehrát s varováním).
 */
static int fsl_rate_ok ( uint8_t format, uint8_t level, uint32_t rate ) {
    if ( format != TMZ_FORMAT_FSK && format != TMZ_FORMAT_SLOW && format != TMZ_FORMAT_DIRECT ) return 1;
    for ( size_t i = 0; i < sizeof ( g_fsl_bad ) / sizeof ( g_fsl_bad[0] ); i++ ) {
        if ( g_fsl_bad[i].format == format && g_fsl_bad[i].rate == rate
             && ( g_fsl_bad[i].level == FSL_ANY_LEVEL || g_fsl_bad[i].level == level ) ) return 0;
    }
    for ( size_t i = 0; i < sizeof ( g_fsl_measured_rates ) / sizeof ( g_fsl_measured_rates[0] ); i++ ) {
        if ( g_fsl_measured_rates[i] == rate ) return 1;
    }
    return -1;
}


/**
 * @brief Prehaje blok 0x41 (MZ Turbo Data) na CMT stream.
 *
 * Dispatcher - podle pole format v bloku zvoli prislusny koder:
 * - NORMAL: standardni FM pres mztape (puvodni chovani)
 * - TURBO: NORMAL FM se zmenenym casovanim (mzcmt_turbo)
 * - FASTIPL: $BB prefix kodovani s dvojdilnym signalem (mzcmt_fastipl)
 * - FSK: Frequency Shift Keying (mzcmt_fsk)
 * - SLOW: 2 bity na pulz (mzcmt_slow)
 * - DIRECT: primy bitovy zapis (mzcmt_direct)
 * - CPM-TAPE: vždy převod na 0x46 PEZIK-2.0A (tmz_v1conv) a přehrání jako 0x46
 * - SINCLAIR: nepodporovano (vyzaduje TZX blokovou generaci)
 *
 * Za signál připojí pause_ms bloku jako ticho na nízké úrovni (jen vstream).
 *
 * FSK, SLOW a DIRECT při vzorkovací frekvenci, kterou emulátor změřil jako
 * nečitelnou (g_fsl_bad), vrátí
 * TMZ_PLAYER_ERROR_UNREADABLE; při nezměřené frekvenci varují přes error
 * callback a přehrají se.
 *
 * @param block TMZ blok s ID 0x41.
 * @param config Konfigurace playeru.
 * @param err Vystupni chybovy kod (muze byt NULL).
 * @return Novy CMT stream, nebo NULL pri chybe.
 *
 * @pre block nesmi byt NULL a musi mit id == TMZ_BLOCK_ID_MZ_TURBO_DATA.
 * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* tmz_player_play_mz_turbo ( const st_TZX_BLOCK *block,
                                           const st_TMZ_PLAYER_CONFIG *config,
                                           en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }

    /* parsovani bloku (kopie kvuli endianite) */
    en_TZX_ERROR parse_err;
    uint8_t *body_data = NULL;

    st_TZX_BLOCK block_copy = *block;
    uint8_t *data_copy = malloc ( block->length );
    if ( !data_copy ) {
        *err = TMZ_PLAYER_ERROR_ALLOC;
        return NULL;
    }
    memcpy ( data_copy, block->data, block->length );
    block_copy.data = data_copy;

    st_TMZ_MZ_TURBO_DATA *turbo_data = tmz_block_parse_mz_turbo ( &block_copy, &body_data, &parse_err );
    if ( !turbo_data ) {
        g_player_error_cb ( __func__, __LINE__, "Failed to parse MZ Turbo Data block\n" );
        free ( data_copy );
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;   /* neplatný blok: trvalá chyba */
        return NULL;
    }

    /*
     * CPM-TAPE nemá cestu v1.0. Blok se převede na 0x46 PEZIK-2.0A i při v1_convert = 0;
     * pauza bloku je v něm jako pauza poslední části.
     */
    if ( turbo_data->format == TMZ_FORMAT_CPM_TAPE ) {
        free ( data_copy );
        st_TMZ_V1CONV_CONFIG vc;
        tmz_v1conv_config_init ( &vc );
        st_TZX_BLOCK *conv = NULL;
        if ( tmz_v1conv_block ( block, &vc, &conv ) != TMZ_V1CONV_OK ) {
            g_player_error_cb ( __func__, __LINE__, "CPM-TAPE block cannot be converted to PEZIK-2.0A\n" );
            *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
            return NULL;
        }
        st_CMT_STREAM *cs = tmz_player_play_mz_file ( conv, config, err );
        tmz_block_free ( conv );
        return cs;
    }

    /* FSK/SLOW/DIRECT: čitelnost podle měření v emulátoru */
    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    int rok = fsl_rate_ok ( turbo_data->format, turbo_data->speed, rate );
    if ( rok <= 0 ) {
        const char *fname = ( turbo_data->format == TMZ_FORMAT_FSK ) ? "FSK"
                            : ( turbo_data->format == TMZ_FORMAT_SLOW ) ? "SLOW" : "DIRECT";
        if ( rok == 0 ) {
            g_player_error_cb ( __func__, __LINE__, "Format %s level %u is not readable at %u Hz (measured in the emulator)\n",
                                fname, turbo_data->speed, rate );
            free ( data_copy );
            *err = TMZ_PLAYER_ERROR_UNREADABLE;
            return NULL;
        }
        g_player_error_cb ( __func__, __LINE__, "Warning: format %s level %u is not verified at %u Hz\n",
                            fname, turbo_data->speed, rate );
    }

    /* dispatching podle formatu zaznamu */
    st_CMT_STREAM *stream = NULL;

    switch ( turbo_data->format ) {

        case TMZ_FORMAT_NORMAL:
            stream = play_turbo_fmt_normal ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_TURBO:
            stream = play_turbo_fmt_turbo ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_FASTIPL:
            stream = play_turbo_fmt_fastipl ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_FSK:
            stream = play_turbo_fmt_fsk ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_SLOW:
            stream = play_turbo_fmt_slow ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_DIRECT:
            stream = play_turbo_fmt_direct ( turbo_data, body_data, config );
            break;

        case TMZ_FORMAT_SINCLAIR:
            g_player_error_cb ( __func__, __LINE__,
                "SINCLAIR format in MZ Turbo Data block not yet supported\n" );
            break;

        default:
            g_player_error_cb ( __func__, __LINE__,
                "Unknown format %d in MZ Turbo Data block\n", turbo_data->format );
            break;
    }

    uint16_t pause_ms = turbo_data->pause_ms;   /* turbo_data ukazuje do data_copy */
    free ( data_copy );

    if ( !stream ) {
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
        return NULL;
    }
    if ( append_v1_pause ( stream, pause_ms, config ) != EXIT_SUCCESS ) {
        cmt_stream_destroy ( stream );
        *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
        return NULL;
    }

    *err = TMZ_PLAYER_OK;
    return stream;
}


/**
 * @brief Přehraje blok 0x45 (MZ BASIC Data) na CMT stream.
 *
 * Generátor v1.0 (mzcmt_bsd) psal tón jen před první kus a BASIC soubor za
 * prvním kusem nepřečetl. Blok se proto vždy převede
 * na 0x46 s profilem BASIC-1Z016 (tmz_v1conv_block, spec 9.2) a přehraje
 * jako 0x46; pauza bloku je pauzou poslední části. Platí i při
 * v1_convert = 0.
 *
 * @param block TMZ blok s ID 0x45.
 * @param config Konfigurace playeru.
 * @param err Výstupní chybový kód (může být NULL): TMZ_PLAYER_ERROR_UNSUPPORTED,
 *        když blok nejde převést (poškozený, žádný kus nebo víc než 254 kusů),
 *        jinak jako tmz_player_play_mz_file().
 * @return Nový CMT stream, nebo NULL při chybě.
 *
 * @pre block nesmí být NULL a musí mít id == TMZ_BLOCK_ID_MZ_BASIC_DATA.
 * @post Volající je vlastníkem vráceného streamu a musí ho uvolnit přes cmt_stream_destroy().
 */
st_CMT_STREAM* tmz_player_play_mz_basic ( const st_TZX_BLOCK *block,
                                            const st_TMZ_PLAYER_CONFIG *config,
                                            en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }

    st_TMZ_V1CONV_CONFIG vc;
    tmz_v1conv_config_init ( &vc );
    st_TZX_BLOCK *conv = NULL;
    if ( tmz_v1conv_block ( block, &vc, &conv ) != TMZ_V1CONV_OK ) {
        g_player_error_cb ( __func__, __LINE__,
                            "Block 0x45 cannot be converted to BASIC-1Z016 (invalid block, no chunk or more than 254 chunks)\n" );
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
        return NULL;
    }
    st_CMT_STREAM *stream = tmz_player_play_mz_file ( conv, config, err );
    tmz_block_free ( conv );
    return stream;
}


/**
 * @brief Prevede konfiguraci TMZ playeru na konfiguraci TZX generatoru.
 *
 * @param config TMZ player konfigurace.
 * @return TZX konfigurace.
 */
static st_TZX_CONFIG tmz_player_to_tzx_config ( const st_TMZ_PLAYER_CONFIG *config ) {
    st_TZX_CONFIG tzx_cfg;
    tzx_config_init ( &tzx_cfg );
    tzx_cfg.sample_rate = config->sample_rate;
    tzx_cfg.stream_type = config->stream_type;
    /* cpu_clock zustava na vychozich 3.5 MHz pro TZX bloky */
    return tzx_cfg;
}


/**
 * @brief Zabali vstream do CMT stream wrapperu.
 *
 * @param vs Vstream k zabaleni (vlastnictvi se prenasi).
 * @return Novy CMT stream, nebo NULL pri chybe.
 */
static st_CMT_STREAM* tmz_player_wrap_vstream ( st_CMT_VSTREAM *vs ) {
    st_CMT_STREAM *stream = cmt_stream_new ( CMT_STREAM_TYPE_VSTREAM );
    if ( !stream ) {
        cmt_vstream_destroy ( vs );
        return NULL;
    }
    stream->str.vstream = vs;
    return stream;
}


/** @copydoc tmz_player_play_mz_file */
st_CMT_STREAM* tmz_player_play_mz_file ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config,
                                         en_TMZ_PLAYER_ERROR *err ) {
    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;
    if ( !block || !config ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }
    st_TMZ_MZFILE mf;
    if ( tmz_mzfile_parse ( block, &mf ) != TZX_OK ) {
        g_player_error_cb ( __func__, __LINE__, "Invalid MZ File block 0x46\n" );
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
        return NULL;
    }
    uint32_t rate = config->sample_rate ? config->sample_rate : CMTSTREAM_DEFAULT_RATE;
    uint32_t tb = config->mz_cpu_clock ? config->mz_cpu_clock : MZCMT_FM_DEFAULT_TIMEBASE;
    st_MZCMT_FM_PARAMS *p = malloc ( sizeof ( st_MZCMT_FM_PARAMS ) );
    st_MZCMT_MANCH_PARAMS *mp = malloc ( sizeof ( st_MZCMT_MANCH_PARAMS ) );
    if ( !p || !mp ) {
        free ( p );
        free ( mp );
        tmz_mzfile_free ( &mf );
        *err = TMZ_PLAYER_ERROR_ALLOC;
        return NULL;
    }
    /*
     * Mez celého bloku předem (před alokací vstreamu): součet horních
     * odhadů délky přehratelných částí <= CMT_VSTREAM_MAX_BLOCK_SECONDS
     * a součet pulzů FM a bitů MANCHESTER <= TMZ_PLAYER_MAX_BLOCK_PULSES.
     * Délka v µs: odhad části <= 3600 s x timebase, převod dělením (bez přetečení).
     */
    {
        uint64_t block_us = 0, block_pulses = 0;
        for ( uint8_t i = 0; i < mf.part_count; i++ ) {
            const st_TMZ_PART *pt = &mf.parts[i];
            if ( pt->family != TMZ_FAMILY_FM && pt->family != TMZ_FAMILY_MANCHESTER ) continue;
            uint32_t size = 0;
            en_TMZ_PART_ERROR pe = TMZ_PART_OK;
            const uint8_t *data = tmz_mzfile_part_data ( &mf, pt, &size, &pe );
            ( void ) data;
            st_TMZ_PART_COMMON c;
            uint64_t n = 0, units = 0;
            uint32_t part_tb = 0;
            if ( pt->family == TMZ_FAMILY_MANCHESTER ) {
                if ( pe == TMZ_PART_OK ) pe = tmz_part_manchester_decode ( pt->tlv, pt->tlv_length, tb, mp, &c );
                if ( pe != TMZ_PART_OK || mzcmt_manch_part_bounds ( mp, size, &n, &units ) != EXIT_SUCCESS ) continue;
                part_tb = mp->timebase;
            } else {
                if ( pe == TMZ_PART_OK ) pe = tmz_part_fm_decode ( pt->tlv, pt->tlv_length, tb, p, &c );
                if ( pe != TMZ_PART_OK || mzcmt_fm_part_bounds ( p, size, &n, &units ) != EXIT_SUCCESS ) continue;
                part_tb = p->timebase;
            }
            /* neplatnou část přeskočí i hlavní průchod; počítají se jen přehratelné */
            block_pulses += n;
            block_us += units / part_tb * 1000000u + ( units % part_tb ) * 1000000u / part_tb;
        }
        if ( block_pulses > TMZ_PLAYER_MAX_BLOCK_PULSES
             || block_us > ( uint64_t ) CMT_VSTREAM_MAX_BLOCK_SECONDS * 1000000u ) {
            g_player_error_cb ( __func__, __LINE__, "MZ File block: signal exceeds the block limits "
                                "(%llu pulses, %llu s; at most %u pulses, %u s)\n",
                                ( unsigned long long ) block_pulses, ( unsigned long long ) ( block_us / 1000000u ),
                                TMZ_PLAYER_MAX_BLOCK_PULSES, CMT_VSTREAM_MAX_BLOCK_SECONDS );
            free ( p );
            free ( mp );
            tmz_mzfile_free ( &mf );
            *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
            return NULL;
        }
    }

    st_CMT_VSTREAM *vs = NULL;
    int played = 0;
    for ( uint8_t i = 0; i < mf.part_count; i++ ) {
        const st_TMZ_PART *pt = &mf.parts[i];
        if ( pt->family != TMZ_FAMILY_FM && pt->family != TMZ_FAMILY_MANCHESTER ) {
            g_player_error_cb ( __func__, __LINE__, "Part %u: %s (family %u)\n", i,
                                tmz_part_error_string ( TMZ_PART_ERROR_UNSUPPORTED_FAMILY ), pt->family );
            continue;
        }
        uint32_t size = 0;
        en_TMZ_PART_ERROR pe = TMZ_PART_OK;
        const uint8_t *data = tmz_mzfile_part_data ( &mf, pt, &size, &pe );
        st_TMZ_PART_COMMON c;
        if ( pt->family == TMZ_FAMILY_MANCHESTER ) {
            /* rodina MANCHESTER: READER se nekontroluje (spec 7.2) */
            if ( pe == TMZ_PART_OK ) pe = tmz_part_manchester_decode ( pt->tlv, pt->tlv_length, tb, mp, &c );
            if ( pe != TMZ_PART_OK ) {
                g_player_error_cb ( __func__, __LINE__, "Part %u: %s\n", i, tmz_part_error_string ( pe ) );
                continue;
            }
            if ( mzcmt_manch_check_size ( mp, size ) != EXIT_SUCCESS ) {
                /* signál mimo meze knihovny (počet bitů, délka): neplatná část */
                g_player_error_cb ( __func__, __LINE__, "Part %u: signal exceeds the generator limits\n", i );
                continue;
            }
            if ( !vs ) vs = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, mzcmt_manch_first_level ( mp, data, size ),
                                              CMT_STREAM_POLARITY_NORMAL );
            if ( !vs || mzcmt_manch_append_vstream ( vs, mp, data, size ) != EXIT_SUCCESS ) {
                g_player_error_cb ( __func__, __LINE__, "Part %u: signal generation failed\n", i );
                if ( vs ) cmt_vstream_destroy ( vs );
                free ( p );
                free ( mp );
                tmz_mzfile_free ( &mf );
                *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
                return NULL;
            }
            played++;
            continue;
        }
        if ( pe == TMZ_PART_OK ) pe = tmz_part_fm_decode ( pt->tlv, pt->tlv_length, tb, p, &c );
        if ( pe != TMZ_PART_OK ) {
            g_player_error_cb ( __func__, __LINE__, "Part %u: %s\n", i, tmz_part_error_string ( pe ) );
            continue;
        }
        if ( mzcmt_fm_check_size ( p, size ) != EXIT_SUCCESS ) {
            /* signál mimo meze knihovny (počet pulzů, délka): neplatná část */
            g_player_error_cb ( __func__, __LINE__, "Part %u: signal exceeds the generator limits\n", i );
            continue;
        }
        /* čtecí bod po převodu na vzorky: pod 0 nečitelné = chyba bloku, pod 10 µs varování */
        st_TMZ_READER_MARGINS m;
        en_TMZ_READER_CHECK rc = tmz_part_fm_check_reader ( p, &c, rate, &m );
        if ( rc == TMZ_READER_UNREADABLE ) {
            g_player_error_cb ( __func__, __LINE__, "Part %u: not readable at %u Hz (margins %.1f/%.1f/%.1f us)\n", i,
                                rate, m.short_high_us, m.long_high_us, m.short_period_us );
            if ( vs ) cmt_vstream_destroy ( vs );
            free ( p );
            free ( mp );
            tmz_mzfile_free ( &mf );
            *err = TMZ_PLAYER_ERROR_UNREADABLE;
            return NULL;
        }
        if ( rc == TMZ_READER_LOW_MARGIN ) {
            g_player_error_cb ( __func__, __LINE__, "Warning: part %u: low reader margin at %u Hz (%.1f/%.1f/%.1f us, min %.1f)\n",
                                i, rate, m.short_high_us, m.long_high_us, m.short_period_us, TMZ_READER_MIN_MARGIN_US );
        }
        if ( !vs ) vs = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, p->level ? 1 : 0, CMT_STREAM_POLARITY_NORMAL );
        if ( !vs || mzcmt_fm_append_vstream ( vs, p, data, size ) != EXIT_SUCCESS ) {
            g_player_error_cb ( __func__, __LINE__, "Part %u: signal generation failed\n", i );
            if ( vs ) cmt_vstream_destroy ( vs );
            free ( p );
            free ( mp );
            tmz_mzfile_free ( &mf );
            *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
            return NULL;
        }
        played++;
    }
    free ( p );
    free ( mp );
    tmz_mzfile_free ( &mf );
    if ( !played ) {
        g_player_error_cb ( __func__, __LINE__, "MZ File block has no playable part\n" );
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
        return NULL;
    }

    st_CMT_STREAM *stream = cmt_stream_new ( config->stream_type );
    if ( !stream ) {
        cmt_vstream_destroy ( vs );
        *err = TMZ_PLAYER_ERROR_ALLOC;
        return NULL;
    }
    if ( stream->stream_type == CMT_STREAM_TYPE_BITSTREAM ) {
        st_CMT_BITSTREAM *bs = cmt_bitstream_new_from_vstream ( vs, rate );
        cmt_vstream_destroy ( vs );
        if ( !bs ) {
            cmt_stream_destroy ( stream );
            *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
            return NULL;
        }
        stream->str.bitstream = bs;
    } else {
        stream->str.vstream = vs;
    }
    *err = TMZ_PLAYER_OK;
    return stream;
}


/**
 * @brief Zjistí, zda jde o audio blok TZX, který generuje tzx_generate_block().
 * @param id ID bloku.
 * @return 1 = 0x10-0x15, 0x18 nebo 0x20, jinak 0.
 */
static int is_tzx_audio ( uint8_t id ) {
    return id == TZX_BLOCK_ID_STANDARD_SPEED || id == TZX_BLOCK_ID_TURBO_SPEED || id == TZX_BLOCK_ID_PURE_TONE
           || id == TZX_BLOCK_ID_PULSE_SEQUENCE || id == TZX_BLOCK_ID_PURE_DATA || id == TZX_BLOCK_ID_DIRECT_RECORDING
           || id == TZX_BLOCK_ID_CSW_RECORDING || id == TZX_BLOCK_ID_PAUSE;
}


/**
 * @brief Zjistí, zda signál bloku MZ končí tichem (pauzou).
 *
 * Podle dat bloku, ne podle signálu (ticho nejde od dlouhé půlvlny spolehlivě
 * odlišit): 0x40, 0x41 a 0x45 končí tichem při pause_ms > 0 (pauzu na konec
 * dává převod na 0x46 i cesta v1.0); 0x46 při pause > 0 poslední části
 * rodiny FM nebo MANCHESTER, kterou jde dekódovat (tu player přehraje poslední). Pauza
 * kratší než vzorek, kterou generátor FM vynechá, se počítá také.
 *
 * @param block Blok MZ.
 * @param config Konfigurace (mz_cpu_clock = výchozí časová základna částí 0x46).
 * @return 1 = končí tichem, 0 = nekončí, jiný blok nebo chyba (kopie, parse).
 */
static int mz_block_ends_with_pause ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config ) {
    if ( !block->data || !block->length ) return 0;
    if ( block->id == TMZ_BLOCK_ID_MZ_FILE ) {
        st_TMZ_MZFILE mf;
        if ( tmz_mzfile_parse ( block, &mf ) != TZX_OK ) return 0;
        st_MZCMT_FM_PARAMS *p = malloc ( sizeof ( st_MZCMT_FM_PARAMS ) );
        st_MZCMT_MANCH_PARAMS *mp = malloc ( sizeof ( st_MZCMT_MANCH_PARAMS ) );
        int ends = 0;
        uint32_t tb = config->mz_cpu_clock ? config->mz_cpu_clock : MZCMT_FM_DEFAULT_TIMEBASE;
        for ( int i = ( int ) mf.part_count - 1; p && mp && i >= 0; i-- ) {
            const st_TMZ_PART *pt = &mf.parts[i];
            st_TMZ_PART_COMMON c;
            if ( pt->family == TMZ_FAMILY_FM && tmz_part_fm_decode ( pt->tlv, pt->tlv_length, tb, p, &c ) == TMZ_PART_OK ) {
                ends = ( p->pause > 0 );
                break;
            }
            if ( pt->family == TMZ_FAMILY_MANCHESTER
                 && tmz_part_manchester_decode ( pt->tlv, pt->tlv_length, tb, mp, &c ) == TMZ_PART_OK ) {
                ends = ( mp->pause > 0 );
                break;
            }
        }
        free ( p );
        free ( mp );
        tmz_mzfile_free ( &mf );
        return ends;
    }
    if ( block->id != TMZ_BLOCK_ID_MZ_STANDARD_DATA && block->id != TMZ_BLOCK_ID_MZ_TURBO_DATA
         && block->id != TMZ_BLOCK_ID_MZ_BASIC_DATA ) return 0;

    /* parse bloků v1.0 mění data (endianita), proto kopie */
    st_TZX_BLOCK copy = *block;
    uint8_t *data = malloc ( block->length );
    if ( !data ) return 0;
    memcpy ( data, block->data, block->length );
    copy.data = data;
    en_TZX_ERROR pe;
    uint8_t *payload = NULL;
    uint32_t pause_ms = 0;
    if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) {
        st_TMZ_MZ_STANDARD_DATA *d = tmz_block_parse_mz_standard ( &copy, &payload, &pe );
        if ( d ) pause_ms = d->pause_ms;
    } else if ( block->id == TMZ_BLOCK_ID_MZ_TURBO_DATA ) {
        st_TMZ_MZ_TURBO_DATA *d = tmz_block_parse_mz_turbo ( &copy, &payload, &pe );
        if ( d ) pause_ms = d->pause_ms;
    } else {
        st_TMZ_MZ_BASIC_DATA *d = tmz_block_parse_mz_basic_data ( &copy, &payload, &pe );
        if ( d ) pause_ms = d->pause_ms;
    }
    free ( data );
    return pause_ms > 0;
}


/**
 * @brief Přehraje blok s úrovní signálu na vstupu a výstupu.
 *
 * Audio bloky TZX (0x10-0x15, 0x18, 0x20) generuje tzx_generate_block() od úrovně
 * *level a vrátí úroveň po bloku (pravidla TZX 1.20, po pauze ale
 * TZX_LEVEL_AFTER_PAUSE: další pulz s hranou jako na ZX). Vzorkované bloky
 * 0x15 a 0x18 mají úrovně pulzů dané blokem (0x18: první pulz LOW), vstupní
 * úroveň platí jen pro pauzu bloku bez vzorků; po nich je úroveň opakem
 * posledního pulzu. Ostatní bloky přehraje
 * tmz_player_play_block(); vrátí-li vstream, nastaví *level na opak poslední
 * hodnoty, aby další blok TZX začal hranou; po bloku MZ, který končí tichem
 * na nízké úrovni (mz_block_ends_with_pause()), na TZX_LEVEL_AFTER_PAUSE
 * jako po pauze TZX.
 * U bitstreamu a bloků bez streamu se úroveň nemění.
 *
 * @param block Blok.
 * @param config Konfigurace playeru.
 * @param[in,out] level Aktuální úroveň signálu (0|1|TZX_LEVEL_AFTER_PAUSE).
 * @param[out] err TMZ_PLAYER_OK; Pause 0 vrátí NULL s OK. U bloků TZX blok,
 *        který přehrát nejde (TZX_ERROR_UNSUPPORTED - např. CSW Z-RLE,
 *        TZX_ERROR_INVALID_BLOCK - neplatná data) = TMZ_PLAYER_ERROR_UNSUPPORTED
 *        (hlášení vypíše knihovna tzx, player nic dalšího), jiná chyba
 *        generátoru = TMZ_PLAYER_ERROR_STREAM_CREATE, selhání obalení streamu
 *        = TMZ_PLAYER_ERROR_ALLOC; jinak kód z tmz_player_play_block().
 * @return Nový stream (uvolnit cmt_stream_destroy()), nebo NULL.
 *
 * @pre block, config, level a err nesmí být NULL.
 */
static st_CMT_STREAM* play_with_level ( const st_TZX_BLOCK *block, const st_TMZ_PLAYER_CONFIG *config, uint8_t *level,
                                        en_TMZ_PLAYER_ERROR *err ) {
    if ( is_tzx_audio ( block->id ) ) {
        st_TZX_CONFIG tzx_cfg = tmz_player_to_tzx_config ( config );
        en_TZX_ERROR tzx_err = TZX_OK;
        st_CMT_VSTREAM *vs = tzx_generate_block ( block, &tzx_cfg, level, &tzx_err );
        if ( !vs ) {
            if ( tzx_err == TZX_OK ) {   /* Pause 0: žádný signál */
                *err = TMZ_PLAYER_OK;
                return NULL;
            }
            if ( tzx_err == TZX_ERROR_UNSUPPORTED || tzx_err == TZX_ERROR_INVALID_BLOCK ) {
                /* blok tak, jak je, přehrát nejde (Z-RLE, neplatná data): trvalá chyba bloku,
                 * ne přechodná. Hlášení s důvodem už vypsala knihovna tzx. */
                *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
                return NULL;
            }
            g_player_error_cb ( __func__, __LINE__, "TZX block 0x%02X: failed to generate audio\n", block->id );
            *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
            return NULL;
        }
        st_CMT_STREAM *stream = tmz_player_wrap_vstream ( vs );
        *err = stream ? TMZ_PLAYER_OK : TMZ_PLAYER_ERROR_ALLOC;
        return stream;
    }
    st_CMT_STREAM *stream = tmz_player_play_block ( block, config, err );
    if ( stream && stream->stream_type == CMT_STREAM_TYPE_VSTREAM && stream->str.vstream ) {
        /*
         * Další blok TZX začne hranou: opak poslední úrovně MZ streamu. Ticho na konci
         * na nízké úrovni = jako pauza TZX (TZX_LEVEL_AFTER_PAUSE: další pauza bez 1 ms vysoko).
         */
        uint8_t last = stream->str.vstream->last_set_value ? 1 : 0;
        *level = ( mz_block_ends_with_pause ( block, config ) && !last ) ? TZX_LEVEL_AFTER_PAUSE : ( uint8_t ) !last;
    }
    return stream;
}


/**
 * @brief Prehaje libovolny TZX audio blok (0x10-0x15, 0x18, 0x20) na CMT stream.
 *
 * Pouzije TZX knihovnu (tzx_generate_block) pro parsovani bloku a generovani
 * vstreamu. Podporuje bloky: Standard Speed (0x10), Turbo Speed (0x11),
 * Pure Tone (0x12), Pulse Sequence (0x13), Pure Data (0x14),
 * Direct Recording (0x15), CSW Recording (0x18, jen komprese RLE; Z-RLE
 * = TMZ_PLAYER_ERROR_UNSUPPORTED) a Pause (0x20). Vstupní úroveň je
 * TZX_LEVEL_AFTER_PAUSE (signál nízko): u bloků 0x10-0x14 je proto první
 * pulz vysoko, s hranou (jako ZX); vzorkované bloky 0x15 a 0x18 mají úrovně
 * pulzů dané blokem (0x18: první pulz LOW), vstupní úroveň u nich platí jen
 * pro pauzu bloku bez vzorků. Úroveň mezi bloky drží jen tmz_player_play_next().
 *
 * @param block TMZ blok s TZX audio ID.
 * @param config Konfigurace playeru.
 * @param err Vystupni chybovy kod (muze byt NULL): TMZ_PLAYER_OK (i Pause 0,
 *        kdy se vrátí NULL); TMZ_PLAYER_ERROR_NULL_INPUT;
 *        TMZ_PLAYER_ERROR_UNSUPPORTED (jiné ID, blok, který přehrát nejde:
 *        CSW Z-RLE, neplatná data); TMZ_PLAYER_ERROR_STREAM_CREATE (jiná chyba
 *        generátoru); TMZ_PLAYER_ERROR_ALLOC.
 * @return Novy CMT stream, nebo NULL pri chybe / Pause 0.
 *
 * @pre block nesmi byt NULL.
 * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* tmz_player_play_tzx_block ( const st_TZX_BLOCK *block,
                                            const st_TMZ_PLAYER_CONFIG *config,
                                            en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config || !block->data ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }
    if ( !is_tzx_audio ( block->id ) ) {
        *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
        return NULL;
    }

    uint8_t lv = TZX_LEVEL_AFTER_PAUSE;   /* start: signál nízko, první pulz s hranou */
    return play_with_level ( block, config, &lv, err );
}


/**
 * @brief Prehaje jeden libovolny blok na CMT stream.
 *
 * Dispatcher - podle ID bloku zavola prislusnou specializovanou funkci.
 * Ridici bloky (pause, group, loop) jsou zpracovany, ale nevytvareji stream
 * (vraci NULL s TMZ_PLAYER_OK).
 *
 * Bloky v1.0 0x40, 0x41 a 0x45 se při config->v1_convert převedou v paměti
 * na 0x46 (tmz_v1conv_block, spec 9.1, 9.2) a přehrají jako 0x46. Blok,
 * který převést nejde (nebo při v1_convert = 0), se přehraje cestou v1.0;
 * výjimkou jsou 0x41 CPM_TAPE a 0x45, které cestu v1.0 nemají a převádějí
 * se vždy.
 * Cestou v1.0 se přehraje i převedený blok, jehož signál generátor FM
 * nevyrobí; výjimkou je TMZ_PLAYER_ERROR_UNREADABLE, ta se vrátí.
 * Bloky 0x42 a 0x44 se přeskočí s varováním přes error callback (spec 9.3):
 * NULL a TMZ_PLAYER_OK. Stejně rezervované bloky 0x47-0x4F. Pause 0
 * vrátí NULL a TMZ_PLAYER_OK (signál zastavení dává jen tmz_player_play_next()).
 *
 * @param block TMZ blok k prehrani.
 * @param config Konfigurace playeru.
 * @param err Vystupni chybovy kod (muze byt NULL).
 * @return Novy CMT stream, nebo NULL pokud blok negeneruje audio (ridici bloky)
 *         nebo pri chybe.
 *
 * @pre block nesmi byt NULL.
 * @post Volajici je vlastnikem vraceneho streamu a musi ho uvolnit pres cmt_stream_destroy().
 */
st_CMT_STREAM* tmz_player_play_block ( const st_TZX_BLOCK *block,
                                        const st_TMZ_PLAYER_CONFIG *config,
                                        en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !block || !config ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }

    switch ( block->id ) {

        /* MZ-specificke bloky */
        case TMZ_BLOCK_ID_MZ_STANDARD_DATA:
        case TMZ_BLOCK_ID_MZ_TURBO_DATA:
        case TMZ_BLOCK_ID_MZ_BASIC_DATA: {
            /* spec 9.1: převod v paměti na 0x46; co převést nejde, hraje cestou v1.0 */
            if ( config->v1_convert ) {
                st_TMZ_V1CONV_CONFIG vc;
                vc.formatset = config->default_formatset;
                vc.default_speed = config->default_speed;
                st_TZX_BLOCK *conv = NULL;
                if ( tmz_v1conv_block ( block, &vc, &conv ) == TMZ_V1CONV_OK ) {
                    st_CMT_STREAM *s = tmz_player_play_mz_file ( conv, config, err );
                    tmz_block_free ( conv );
                    /*
                     * Nečitelnost je chyba; jiné selhání generátoru FM (např. půlvlna
                     * kratší než vzorek při nízké frekvenci) hraje cestou v1.0, která
                     * půlvlny zaokrouhluje nejméně na 1 vzorek.
                     */
                    if ( s || *err == TMZ_PLAYER_ERROR_UNREADABLE ) return s;
                    g_player_error_cb ( __func__, __LINE__, "Block 0x%02X: converted signal failed, using the v1.0 path\n",
                                        block->id );
                }
            }
            if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) return tmz_player_play_mz_standard ( block, config, err );
            /* BSD/BRD: vždy převod na BASIC-1Z016 */
            if ( block->id == TMZ_BLOCK_ID_MZ_BASIC_DATA ) return tmz_player_play_mz_basic ( block, config, err );
            return tmz_player_play_mz_turbo ( block, config, err );
        }

        /* TMZ v1.1: MZF + seznam částí */
        case TMZ_BLOCK_ID_MZ_FILE:
            return tmz_player_play_mz_file ( block, config, err );

        /* spec 9.3: ve v1.1 nepodporované, přeskočí se s varováním */
        case TMZ_BLOCK_ID_MZ_EXTRA_BODY:
        case TMZ_BLOCK_ID_MZ_LOADER:
            g_player_error_cb ( __func__, __LINE__, "Warning: block 0x%02X is not supported, skipped\n", block->id );
            *err = TMZ_PLAYER_OK;
            return NULL;

        /* TZX audio bloky - delegovano na TZX knihovnu */
        case TZX_BLOCK_ID_STANDARD_SPEED:
        case TZX_BLOCK_ID_TURBO_SPEED:
        case TZX_BLOCK_ID_PURE_TONE:
        case TZX_BLOCK_ID_PULSE_SEQUENCE:
        case TZX_BLOCK_ID_PURE_DATA:
        case TZX_BLOCK_ID_DIRECT_RECORDING:
        case TZX_BLOCK_ID_CSW_RECORDING:
        case TZX_BLOCK_ID_PAUSE:
            return tmz_player_play_tzx_block ( block, config, err );

        /* ridici bloky - negeneruji audio */
        case TZX_BLOCK_ID_GROUP_START:
        case TZX_BLOCK_ID_GROUP_END:
        case TZX_BLOCK_ID_LOOP_START:
        case TZX_BLOCK_ID_LOOP_END:
        case TZX_BLOCK_ID_JUMP:
        case TZX_BLOCK_ID_SELECT_BLOCK:
        case TZX_BLOCK_ID_STOP_48K:
        case TZX_BLOCK_ID_SET_SIGNAL_LEVEL:
        case TZX_BLOCK_ID_TEXT_DESCRIPTION:
        case TZX_BLOCK_ID_MESSAGE:
        case TZX_BLOCK_ID_ARCHIVE_INFO:
        case TZX_BLOCK_ID_HARDWARE_TYPE:
        case TZX_BLOCK_ID_CUSTOM_INFO:
        case TZX_BLOCK_ID_GLUE:
        case TMZ_BLOCK_ID_MZ_MACHINE_INFO:
            *err = TMZ_PLAYER_OK;
            return NULL;

        /* nepodporované bloky (Generalized Data, ...); rezervované MZ bloky se přeskočí */
        default:
            if ( block->id >= TMZ_BLOCK_ID_RESERVED_FIRST && block->id <= TMZ_BLOCK_ID_RESERVED_LAST ) {
                g_player_error_cb ( __func__, __LINE__, "Warning: reserved block 0x%02X skipped\n", block->id );
                *err = TMZ_PLAYER_OK;
                return NULL;
            }
            g_player_error_cb ( __func__, __LINE__, "Unsupported block ID 0x%02X for playback\n", block->id );
            *err = TMZ_PLAYER_ERROR_UNSUPPORTED;
            return NULL;
    }
}


/* ========================================================================= */
/*  Stavovy automat pro sekvencni prehravani                                 */
/* ========================================================================= */


/**
 * @brief Inicializuje stav playeru pro prehrani daneho souboru.
 *
 * Nastavi pocatecni pozici na blok 0, prazdne zasobniky,
 * signal_level = TZX_LEVEL_AFTER_PAUSE, finished = false.
 *
 * @param state   Stav k inicializaci.
 * @param file    TMZ/TZX soubor (musi zustat platny po celou dobu prehravani).
 * @param config  Konfigurace playeru (zkopiruje se do stavu).
 *
 * @pre state, file, config nesmi byt NULL.
 * @post state je pripraven pro prvni volani tmz_player_play_next().
 */
void tmz_player_state_init ( st_TMZ_PLAYER_STATE *state,
                             const st_TZX_FILE *file,
                             const st_TMZ_PLAYER_CONFIG *config ) {
    if ( !state || !file || !config ) return;
    memset ( state, 0, sizeof ( *state ) );
    state->file = file;
    state->config = *config;
    state->signal_level = TZX_LEVEL_AFTER_PAUSE; /* signál nízko, první pulz TZX vytvoří hranu (jako ZX) */
    state->finished = ( file->block_count == 0 );
}


/**
 * @brief Zjisti, zda je prehravani dokonceno.
 *
 * @param state Stav playeru.
 * @return true pokud vsechny bloky byly zpracovany, false jinak.
 */
bool tmz_player_state_finished ( const st_TMZ_PLAYER_STATE *state ) {
    if ( !state ) return true;
    return state->finished;
}


/** @copydoc tmz_player_state_stopped */
bool tmz_player_state_stopped ( const st_TMZ_PLAYER_STATE *state ) {
    return state ? state->stopped : false;
}


/**
 * @brief Zpracuje blok Loop Start (0x24).
 *
 * Nacte pocet opakovani z dat bloku (2B LE), ulozi zaznam
 * na zasobnik smycek a posune current_block za Loop Start.
 * Pokud je zasobnik plny nebo repeat_count == 0, blok se preskoci.
 *
 * @param state Stav playeru.
 * @param block Blok 0x24.
 */
static void handle_loop_start ( st_TMZ_PLAYER_STATE *state, const st_TZX_BLOCK *block ) {
    state->current_block++;

    if ( state->loop_depth >= TMZ_PLAYER_MAX_LOOP_DEPTH ) {
        g_player_error_cb ( __func__, __LINE__,
            "Loop nesting depth exceeded (max %d)\n", TMZ_PLAYER_MAX_LOOP_DEPTH );
        return;
    }

    uint16_t repeat_count = 0;
    if ( block->data && block->length >= 2 ) {
        repeat_count = (uint16_t) ( block->data[0] | ( block->data[1] << 8 ) );
    }

    if ( repeat_count == 0 ) return; /* nulovy repeat = preskocit */

    st_TMZ_PLAYER_LOOP_ENTRY *entry = &state->loop_stack[state->loop_depth];
    entry->start_block = state->current_block; /* blok hned za Loop Start */
    entry->remaining = repeat_count;
    state->loop_depth++;
}


/**
 * @brief Zpracuje blok Loop End (0x25).
 *
 * Dekrementuje pocitadlo na vrcholu zasobniku smycek.
 * Pokud je remaining > 1, skoci zpet na start_block.
 * Pokud je remaining == 1, smycka je hotova - pop ze zasobniku a pokracuje dal.
 * Pokud je zasobnik prazdny, blok se preskoci.
 *
 * @param state Stav playeru.
 */
static void handle_loop_end ( st_TMZ_PLAYER_STATE *state ) {
    if ( state->loop_depth <= 0 ) {
        /* Loop End bez Loop Start - preskocit */
        state->current_block++;
        return;
    }

    st_TMZ_PLAYER_LOOP_ENTRY *entry = &state->loop_stack[state->loop_depth - 1];
    entry->remaining--;

    if ( entry->remaining > 0 ) {
        /* dalsi iterace - skok zpet na zacatek smycky */
        state->current_block = entry->start_block;
    } else {
        /* smycka hotova - pop a pokracovat za Loop End */
        state->loop_depth--;
        state->current_block++;
    }
}


/**
 * @brief Zpracuje blok Jump (0x23).
 *
 * Nacte relativni offset z dat bloku (2B LE signed)
 * a upravi current_block. Offset je relativni vuci aktualnimu bloku.
 *
 * @param state Stav playeru.
 * @param block Blok 0x23.
 */
static void handle_jump ( st_TMZ_PLAYER_STATE *state, const st_TZX_BLOCK *block ) {
    int16_t offset = 0;
    if ( block->data && block->length >= 2 ) {
        offset = (int16_t) ( block->data[0] | ( block->data[1] << 8 ) );
    }

    int64_t target = (int64_t) state->current_block + offset;

    if ( target < 0 || target >= (int64_t) state->file->block_count ) {
        g_player_error_cb ( __func__, __LINE__,
            "Jump target out of range: block %u + offset %d = %lld\n",
            state->current_block, offset, (long long) target );
        state->finished = true;
        return;
    }

    state->current_block = (uint32_t) target;
}


/**
 * @brief Zpracuje blok Call Sequence (0x26).
 *
 * Nacte pocet offsetu a skoci na prvni z nich. Ulozi navratovy
 * zaznam na zasobnik volani. Offsety jsou relativni vuci Call bloku.
 *
 * @param state Stav playeru.
 * @param block Blok 0x26.
 */
static void handle_call_sequence ( st_TMZ_PLAYER_STATE *state, const st_TZX_BLOCK *block ) {
    if ( state->call_depth >= TMZ_PLAYER_MAX_CALL_DEPTH ) {
        g_player_error_cb ( __func__, __LINE__,
            "Call nesting depth exceeded (max %d)\n", TMZ_PLAYER_MAX_CALL_DEPTH );
        state->current_block++;
        return;
    }

    uint16_t count = 0;
    if ( block->data && block->length >= 2 ) {
        count = (uint16_t) ( block->data[0] | ( block->data[1] << 8 ) );
    }

    if ( count == 0 || block->length < 2 + (uint32_t) count * 2 ) {
        /* prazdna sekvence nebo nedostatek dat - preskocit */
        state->current_block++;
        return;
    }

    /* nacist prvni offset a skocit na nej */
    int16_t first_offset = (int16_t) ( block->data[2] | ( block->data[3] << 8 ) );
    int64_t target = (int64_t) state->current_block + first_offset;

    if ( target < 0 || target >= (int64_t) state->file->block_count ) {
        g_player_error_cb ( __func__, __LINE__,
            "Call target out of range: block %u + offset %d = %lld\n",
            state->current_block, first_offset, (long long) target );
        state->current_block++;
        return;
    }

    /* ulozit na zasobnik */
    st_TMZ_PLAYER_CALL_ENTRY *entry = &state->call_stack[state->call_depth];
    entry->call_block = state->current_block;
    entry->current_offset_idx = 0;
    entry->total_offsets = count;
    state->call_depth++;

    state->current_block = (uint32_t) target;
}


/**
 * @brief Zpracuje blok Return from Sequence (0x27).
 *
 * Posune na dalsi offset v aktualnim Call Sequence.
 * Pokud jsou vsechny offsety zpracovany, pop ze zasobniku
 * a pokracuje blokem za Call Sequence.
 *
 * @param state Stav playeru.
 */
static void handle_return ( st_TMZ_PLAYER_STATE *state ) {
    if ( state->call_depth <= 0 ) {
        /* Return bez Call - preskocit */
        state->current_block++;
        return;
    }

    st_TMZ_PLAYER_CALL_ENTRY *entry = &state->call_stack[state->call_depth - 1];
    entry->current_offset_idx++;

    if ( entry->current_offset_idx < entry->total_offsets ) {
        /* dalsi volani v sekvenci */
        const st_TZX_BLOCK *call_block = &state->file->blocks[entry->call_block];
        uint16_t idx = entry->current_offset_idx;

        if ( call_block->data && call_block->length >= 2 + (uint32_t) ( idx + 1 ) * 2 ) {
            int16_t offset = (int16_t) ( call_block->data[2 + idx * 2] |
                                          ( call_block->data[3 + idx * 2] << 8 ) );
            int64_t target = (int64_t) entry->call_block + offset;

            if ( target >= 0 && target < (int64_t) state->file->block_count ) {
                state->current_block = (uint32_t) target;
                return;
            }
        }

        /* chybny offset - ukoncit call sekvenci */
        g_player_error_cb ( __func__, __LINE__,
            "Call sequence offset %u out of range\n", idx );
    }

    /* vsechny offsety zpracovany nebo chyba - pop a pokracovat za Call blokem */
    state->call_depth--;
    state->current_block = entry->call_block + 1;
}


/**
 * @brief Zpracuje blok Set Signal Level (0x2B).
 *
 * Nastavi uroven signalu (0=LOW, 1=HIGH) pro nasledujici audio blok;
 * hodnota různá od 0 je vysoká. Blok kratší než 5 B úroveň nemění.
 *
 * @param state Stav playeru.
 * @param block Blok 0x2B.
 */
static void handle_set_signal_level ( st_TMZ_PLAYER_STATE *state, const st_TZX_BLOCK *block ) {
    if ( block->data && block->length >= 5 ) {
        state->signal_level = ( block->data[4] != 0 );   /* jiná hodnota než 0 = vysoká (otáčení úrovně počítá s 0|1) */
    }
    state->current_block++;
}


/**
 * @brief Prehaje dalsi audio blok v sekvenci.
 *
 * Interně zpracovava ridici bloky (Loop Start/End, Jump,
 * Call Sequence/Return, Set Signal Level, Group Start/End)
 * a vraci CMT stream az pri narazeni na audio blok. Úroveň signálu
 * drží mezi bloky ve state->signal_level (play_with_level).
 *
 * Pause 0 (0x20 s délkou 0) a 0x2A zastaví pásku: funkce vrátí NULL
 * s TMZ_PLAYER_OK, nastaví state->stopped a last_played_block na blok
 * zastavení; další volání pokračuje za ním a stopped vynuluje.
 *
 * Ochrana proti nekonecne smycce: TMZ_PLAYER_MAX_ITERATIONS iteraci
 * vnitrni smycky (tmz_player_play_next_budget() s vlastnim rozpoctem).
 *
 * @param state Stav playeru.
 * @param err   Vystupni chybovy kod (muze byt NULL).
 * @return Novy CMT stream, nebo NULL pokud konec / chyba.
 *
 * @pre state nesmi byt NULL a musi byt inicializovany.
 * @post Volajici je vlastnikem vraceneho streamu.
 */
st_CMT_STREAM* tmz_player_play_next ( st_TMZ_PLAYER_STATE *state,
                                       en_TMZ_PLAYER_ERROR *err ) {
    uint32_t budget = TMZ_PLAYER_MAX_ITERATIONS;
    return tmz_player_play_next_budget ( state, &budget, err );
}


st_CMT_STREAM* tmz_player_play_next_budget ( st_TMZ_PLAYER_STATE *state, uint32_t *budget,
                                              en_TMZ_PLAYER_ERROR *err ) {

    en_TMZ_PLAYER_ERROR local_err = TMZ_PLAYER_OK;
    if ( !err ) err = &local_err;

    if ( !state || !state->file || !budget ) {
        *err = TMZ_PLAYER_ERROR_NULL_INPUT;
        return NULL;
    }

    state->stopped = false;

    /* ochrana proti nekonecne smycce: kazda iterace stoji jednotku rozpoctu */
    while ( !state->finished && *budget > 0 ) {
        ( *budget )--;

        if ( state->current_block >= state->file->block_count ) {
            state->finished = true;
            *err = TMZ_PLAYER_OK;
            return NULL;
        }

        const st_TZX_BLOCK *block = &state->file->blocks[state->current_block];

        switch ( block->id ) {

            /* ridici bloky - zpracovat a pokracovat */
            case TZX_BLOCK_ID_LOOP_START:
                handle_loop_start ( state, block );
                continue;

            case TZX_BLOCK_ID_LOOP_END:
                handle_loop_end ( state );
                continue;

            case TZX_BLOCK_ID_JUMP:
                handle_jump ( state, block );
                continue;

            case TZX_BLOCK_ID_CALL_SEQUENCE:
                handle_call_sequence ( state, block );
                continue;

            case TZX_BLOCK_ID_RETURN_FROM_SEQ:
                handle_return ( state );
                continue;

            case TZX_BLOCK_ID_SET_SIGNAL_LEVEL:
                handle_set_signal_level ( state, block );
                continue;

            /* informacni/metadatove bloky - preskocit */
            case TZX_BLOCK_ID_GROUP_START:
            case TZX_BLOCK_ID_GROUP_END:
            case TZX_BLOCK_ID_SELECT_BLOCK:
            case TZX_BLOCK_ID_TEXT_DESCRIPTION:
            case TZX_BLOCK_ID_MESSAGE:
            case TZX_BLOCK_ID_ARCHIVE_INFO:
            case TZX_BLOCK_ID_HARDWARE_TYPE:
            case TZX_BLOCK_ID_CUSTOM_INFO:
            case TZX_BLOCK_ID_GLUE:
                state->current_block++;
                continue;

            /* zastavení pásky: 0x2A vždy (Sharp MZ nemá režim 128K) */
            case TZX_BLOCK_ID_STOP_48K:
                state->last_played_block = state->current_block;
                state->current_block++;
                state->stopped = true;
                *err = TMZ_PLAYER_OK;
                return NULL;

            /* takt CPU = výchozí časová základna dalších bloků 0x46 (spec 5.3); blok se nemění */
            case TMZ_BLOCK_ID_MZ_MACHINE_INFO:
                if ( block->data && block->length >= 4 + 1 + 4 ) {
                    state->config.mz_cpu_clock = tmz_rd32 ( block->data + 4 + 1 );
                }
                state->current_block++;
                continue;

            /* audio a datove bloky - prehrat a vratit */
            default:
            {
                state->last_played_block = state->current_block;
                /* Pause 0 = zastavení pásky, úroveň se nemění */
                if ( block->id == TZX_BLOCK_ID_PAUSE && block->data && block->length >= 2 && tmz_rd16 ( block->data ) == 0 ) {
                    state->current_block++;
                    state->stopped = true;
                    *err = TMZ_PLAYER_OK;
                    return NULL;
                }
                st_CMT_STREAM *stream = play_with_level ( block, &state->config, &state->signal_level, err );
                state->current_block++;

                if ( stream ) {
                    return stream;
                }

                /* blok nevygeneroval audio (neznamy typ, chyba) */
                if ( *err != TMZ_PLAYER_OK ) {
                    return NULL;
                }
                /* err == OK ale stream == NULL: neaudio blok - pokracovat */
                continue;
            }
        }
    }

    if ( !state->finished ) {
        g_player_error_cb ( __func__, __LINE__,
            "Iteration budget exhausted - possible infinite loop\n" );
        state->finished = true;
        *err = TMZ_PLAYER_ERROR_STREAM_CREATE;
        return NULL;
    }

    *err = TMZ_PLAYER_OK;
    return NULL;
}
