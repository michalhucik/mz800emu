/**
 * @file   tmz_v1conv.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.1
 * @brief  Implementace převodu bloků TMZ v1.0 na blok 0x46 (viz tmz_v1conv.h).
 *
 * Bloky v1.0 se parsují z kopie dat (parsery tmz_blocks mění endianitu
 * na místě) a sestaví se registrem profilů (tmz_profile_build_block).
 * Chyba registru TMZ_PROFILE_ERROR_ARGS znamená, že profil případ
 * nevyjádří (např. rychlost, kterou TurboCopy neumí): blok se nepřevede.
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

#include <stdlib.h>
#include <string.h>

#include "tmz.h"
#include "tmz_blocks.h"
#include "tmz_profile.h"
#include "tmz_v1conv.h"


void tmz_v1conv_config_init ( st_TMZ_V1CONV_CONFIG *cfg ) {
    cfg->formatset = MZTAPE_FORMATSET_MZ800_SANE;
    cfg->default_speed = CMTSPEED_1_1;
}


/**
 * @brief Kopie bloku s vlastními daty (parsery v1.0 mění endianitu na místě).
 * @param b Blok.
 * @param[out] copy Kopie struktury s ukazatelem na nová data.
 * @return Nová data (uvolnit free), nebo NULL při selhání alokace.
 */
static uint8_t* copy_block ( const st_TZX_BLOCK *b, st_TZX_BLOCK *copy ) {
    uint8_t *d = malloc ( b->length ? b->length : 1 );
    if ( !d ) return NULL;
    if ( b->length ) memcpy ( d, b->data, b->length );
    *copy = *b;
    copy->data = d;
    return d;
}


/**
 * @brief Stroj z bloku pro PROFILE_ARGS.
 * @param machine Pole bloku.
 * @return en_TMZ_MACHINE, nebo -1 pro neplatnou hodnotu (nezapisuje se).
 */
static int machine_arg ( uint8_t machine ) {
    return ( machine < TMZ_MACHINE_COUNT ) ? machine : -1;
}


/**
 * @brief Sestaví blok profilem.
 * @param id Profil. @param a Parametry. @param h Hlavička (pořadí hostitele).
 * @param body Tělo. @param size Délka těla. @param[out] out Blok.
 * @return OK; NOT_CONVERTIBLE při TMZ_PROFILE_ERROR_ARGS (profil případ nevyjádří); jinak ERROR.
 */
static en_TMZ_V1CONV_RESULT build ( const char *id, const st_TMZ_PROFILE_ARGS *a, const st_MZF_HEADER *h,
                                    const uint8_t *body, uint32_t size, st_TZX_BLOCK **out ) {
    en_TMZ_PROFILE_ERROR pe = TMZ_PROFILE_OK;
    *out = tmz_profile_build_block ( id, a, h, body, size, &pe );
    if ( *out ) return TMZ_V1CONV_OK;
    return ( pe == TMZ_PROFILE_ERROR_ARGS ) ? TMZ_V1CONV_NOT_CONVERTIBLE : TMZ_V1CONV_ERROR;
}


/**
 * @brief Převod bloku 0x40 (spec 9.2).
 * @param b Kopie bloku (data se mění). @param[out] out Výsledek.
 * @return Výsledek převodu.
 */
static en_TMZ_V1CONV_RESULT conv_standard ( st_TZX_BLOCK *b, st_TZX_BLOCK **out ) {
    uint8_t *body = NULL;
    en_TZX_ERROR e;
    st_TMZ_MZ_STANDARD_DATA *sd = tmz_block_parse_mz_standard ( b, &body, &e );
    if ( !sd ) return TMZ_V1CONV_ERROR;
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init ( &a );
    a.machine = machine_arg ( sd->machine );
    a.pause_ms = sd->pause_ms;
    st_MZF_HEADER h = sd->mzf_header;   /* 11h podle bloku (parser ho nemění) */
    if ( sd->pulseset == TMZ_PULSESET_800 ) return build ( "ROM-800-NOMINAL", &a, &h, body, sd->body_size, out );
    if ( sd->pulseset != TMZ_PULSESET_700 && sd->pulseset != TMZ_PULSESET_80B ) return TMZ_V1CONV_NOT_CONVERTIBLE;
    /* sada z bloku (v1.0 ji ignoroval a hrál MZ-800), struktura SANE, u 80B LGAP formátu MZ80B */
    a.legacy_format = TMZ_LEGACY_NORMAL;
    a.pulseset = sd->pulseset;
    a.speed = CMTSPEED_1_1;
    a.lgap = ( sd->pulseset == TMZ_PULSESET_80B ) ? MZTAPE_LGAP_LENGTH_MZ80B : MZTAPE_LGAP_LENGTH_SANE;
    a.sgap = MZTAPE_SGAP_LENGTH;
    return build ( "TAPEMZ-V1-LEGACY", &a, &h, body, sd->body_size, out );
}


/**
 * @brief Převod bloku 0x41 (spec 9.2): NORMAL 1:1 -> LEGACY, TURBO -> TC-1.21,
 *        FASTIPL -> IC-10.2-FASTIPL, CPM_TAPE -> PEZIK-2.0A.
 * @param b Kopie bloku (data se mění). @param cfg Nastavení. @param[out] out Výsledek.
 * @return Výsledek převodu.
 */
static en_TMZ_V1CONV_RESULT conv_turbo ( st_TZX_BLOCK *b, const st_TMZ_V1CONV_CONFIG *cfg, st_TZX_BLOCK **out ) {
    uint8_t *body = NULL;
    en_TZX_ERROR e;
    st_TMZ_MZ_TURBO_DATA *td = tmz_block_parse_mz_turbo ( b, &body, &e );
    if ( !td ) return TMZ_V1CONV_ERROR;
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init ( &a );
    a.machine = machine_arg ( td->machine );
    a.pause_ms = td->pause_ms;
    en_CMTSPEED speed = cmtspeed_is_valid ( ( en_CMTSPEED ) td->speed ) ? ( en_CMTSPEED ) td->speed : cfg->default_speed;
    a.speed = speed;
    uint16_t pul[4] = { td->long_high, td->long_low, td->short_high, td->short_low };
    int any = pul[0] || pul[1] || pul[2] || pul[3];
    int all = pul[0] && pul[1] && pul[2] && pul[3];
    st_MZF_HEADER h = td->mzf_header;

    switch ( td->format ) {
        case TMZ_FORMAT_NORMAL:
            /*
             * Kopie (formát MZ800), pulzy cmt.com a částečné vlastní pulzy model nevyjádří.
             * Rychlost nad 1:1 se nepřevádí: zavedení přes ROM
             * ověřit nejde a převod mění délku záznamu systematicky (2:1 +8,2 %).
             */
            if ( cfg->formatset == MZTAPE_FORMATSET_MZ800 || speed != CMTSPEED_1_1 || ( any && !all ) ) {
                return TMZ_V1CONV_NOT_CONVERTIBLE;
            }
            if ( cfg->formatset != MZTAPE_FORMATSET_MZ800_SANE && cfg->formatset != MZTAPE_FORMATSET_MZ80B ) {
                return TMZ_V1CONV_NOT_CONVERTIBLE;
            }
            a.legacy_format = TMZ_LEGACY_NORMAL;
            /* v1.0 bral sadu i délky tónů z formátu playeru, pole bloku ignoroval */
            if ( cfg->formatset == MZTAPE_FORMATSET_MZ80B ) {
                a.pulseset = TMZ_PULSESET_80B;
                a.lgap = MZTAPE_LGAP_LENGTH_MZ80B;
            } else {
                a.pulseset = TMZ_PULSESET_800;
                a.lgap = MZTAPE_LGAP_LENGTH_SANE;
            }
            a.sgap = MZTAPE_SGAP_LENGTH;
            if ( all ) memcpy ( a.pulses, pul, sizeof ( a.pulses ) );
            return build ( "TAPEMZ-V1-LEGACY", &a, &h, body, td->body_size, out );

        case TMZ_FORMAT_TURBO:
            /* TurboCopy má pevné tóny a pulzy (změřeno v emulátoru): vlastní pulzy, kopie a sadu 80B nevyjádří */
            if ( any || td->flags || td->pulseset == TMZ_PULSESET_80B ) return TMZ_V1CONV_NOT_CONVERTIBLE;
            a.loader = 1;
            return build ( "TC-1.21", &a, &h, body, td->body_size, out );

        case TMZ_FORMAT_FASTIPL: {
            /*
             * Intercopy V10.2 FAST IPL změřený v emulátoru místo pulzů
             * v1.0 (převod na LEGACY se nezavedl). Jen rychlosti, které Intercopy
             * má, bez vlastních pulzů a s výchozím tónem těla; záměrná změna výstupu.
             */
            static const struct { en_CMTSPEED speed; uint16_t bd; } map[] = {
                { CMTSPEED_1_1, 1200 }, { CMTSPEED_2_1, 2400 }, { CMTSPEED_7_3, 2800 }, { CMTSPEED_8_3, 3200 },
            };
            uint16_t bd = 0;
            for ( size_t k = 0; k < sizeof ( map ) / sizeof ( map[0] ); k++ ) {
                if ( map[k].speed == speed ) bd = map[k].bd;
            }
            if ( !bd || any || ( td->lgap_length && td->lgap_length != 5500 ) || td->pulseset == TMZ_PULSESET_80B
                 || td->pulseset >= TMZ_PULSESET_COUNT ) {
                return TMZ_V1CONV_NOT_CONVERTIBLE;
            }
            a.bd = bd;
            a.speed = CMTSPEED_NONE;   /* profily Intercopy berou rychlost jen jako bd */
            return build ( "IC-10.2-FASTIPL", &a, &h, body, td->body_size, out );
        }

        case TMZ_FORMAT_CPM_TAPE:
            /*
             * CPM-TAPE nemá cestu v1.0; blok se vždy
             * převede na PEZIK-2.0A (spec 9.2): úroveň 2 = 3200, jinak 2400
             * (úroveň 0 "1200" neexistuje, neplatnou hrál player v1.0 jako 2400).
             * Vlastní pulzy, lgap a flags player v1.0 ignoroval, převod také.
             */
            a.bd = ( td->speed == 2 ) ? 3200 : 2400;
            a.speed = CMTSPEED_NONE;
            return build ( "PEZIK-2.0A", &a, &h, body, td->body_size, out );

        default:
            return TMZ_V1CONV_NOT_CONVERTIBLE;
    }
}


/**
 * @brief Převod bloku 0x45 (spec 9.2).
 * @param b Kopie bloku (data se mění). @param[out] out Výsledek.
 * @return Výsledek převodu.
 */
static en_TMZ_V1CONV_RESULT conv_basic ( st_TZX_BLOCK *b, st_TZX_BLOCK **out ) {
    uint8_t *chunks = NULL;
    en_TZX_ERROR e;
    st_TMZ_MZ_BASIC_DATA *bd = tmz_block_parse_mz_basic_data ( b, &chunks, &e );
    if ( !bd ) return TMZ_V1CONV_ERROR;
    /* blok 0x46 má nejvýš 255 částí: hlavička + 254 kusů; víc se nepřehraje */
    if ( bd->chunk_count == 0 || bd->chunk_count > 254 ) return TMZ_V1CONV_ERROR;
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init ( &a );
    a.machine = machine_arg ( bd->machine );
    a.pause_ms = bd->pause_ms;
    /* pulzní sada bloku se neuplatní: časování BASICu 1Z-016 (záměrná změna, spec 9.2) */
    st_MZF_HEADER h = bd->mzf_header;
    return build ( "BASIC-1Z016", &a, &h, chunks, ( uint32_t ) bd->chunk_count * TMZ_BASIC_CHUNK_SIZE, out );
}


en_TMZ_V1CONV_RESULT tmz_v1conv_block ( const st_TZX_BLOCK *block, const st_TMZ_V1CONV_CONFIG *cfg,
                                        st_TZX_BLOCK **out ) {
    if ( !out ) return TMZ_V1CONV_ERROR;
    *out = NULL;
    if ( !block || ( block->length && !block->data ) ) return TMZ_V1CONV_ERROR;
    if ( block->id != TMZ_BLOCK_ID_MZ_STANDARD_DATA && block->id != TMZ_BLOCK_ID_MZ_TURBO_DATA
         && block->id != TMZ_BLOCK_ID_MZ_BASIC_DATA ) {
        return TMZ_V1CONV_NOT_CONVERTIBLE;
    }
    st_TMZ_V1CONV_CONFIG local;
    if ( !cfg ) {
        tmz_v1conv_config_init ( &local );
        cfg = &local;
    }
    st_TZX_BLOCK copy;
    uint8_t *data = copy_block ( block, &copy );
    if ( !data ) return TMZ_V1CONV_ERROR;
    en_TMZ_V1CONV_RESULT r;
    if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) r = conv_standard ( &copy, out );
    else if ( block->id == TMZ_BLOCK_ID_MZ_TURBO_DATA ) r = conv_turbo ( &copy, cfg, out );
    else r = conv_basic ( &copy, out );
    free ( data );
    if ( r != TMZ_V1CONV_OK && *out ) {
        tmz_block_free ( *out );
        *out = NULL;
    }
    return r;
}


int tmz_v1conv_file ( st_TZX_FILE *file, const st_TMZ_V1CONV_CONFIG *cfg, st_TMZ_V1CONV_REPORT *rep ) {
    st_TMZ_V1CONV_REPORT local;
    if ( !rep ) rep = &local;
    memset ( rep, 0, sizeof ( *rep ) );
    if ( !file ) return EXIT_FAILURE;
    for ( uint32_t i = 0; i < file->block_count; i++ ) {
        st_TZX_BLOCK *b = &file->blocks[i];
        if ( b->id == TMZ_BLOCK_ID_MZ_EXTRA_BODY || b->id == TMZ_BLOCK_ID_MZ_LOADER ) {
            rep->kept++;   /* spec 9.3: ve v1.1 nepodporované, ponechají se */
            continue;
        }
        if ( b->id != TMZ_BLOCK_ID_MZ_STANDARD_DATA && b->id != TMZ_BLOCK_ID_MZ_TURBO_DATA
             && b->id != TMZ_BLOCK_ID_MZ_BASIC_DATA ) {
            continue;
        }
        st_TZX_BLOCK *nb = NULL;
        en_TMZ_V1CONV_RESULT r = tmz_v1conv_block ( b, cfg, &nb );
        if ( r == TMZ_V1CONV_OK ) {
            free ( b->data );
            *b = *nb;
            free ( nb );   /* data převzal soubor; jen obal (tmz_block_free by uvolnil i data) */
            rep->converted++;
        } else if ( r == TMZ_V1CONV_NOT_CONVERTIBLE ) {
            rep->kept++;
        } else {
            rep->errors++;
        }
    }
    return EXIT_SUCCESS;
}


const char* tmz_v1conv_result_string ( en_TMZ_V1CONV_RESULT r ) {
    switch ( r ) {
        case TMZ_V1CONV_OK:               return "Converted";
        case TMZ_V1CONV_NOT_CONVERTIBLE:  return "Not convertible, kept as v1.0";
        case TMZ_V1CONV_ERROR:            return "Conversion error";
    }
    return "Unknown conversion result";
}
