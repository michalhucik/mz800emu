/**
 * @file   tmz_mzt.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Implementace MZT a hlavičky CMTSPEED UniCMT (viz tmz_mzt.h, spec kap. 10).
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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmz_mzt.h"
#include "tmz_blocks.h"
#include "../endianity/endianity.h"
#include "../cmtspeed/cmtspeed.h"
#include "../mzf/mzf.h"
#include "tmz.h"
#include "tmz_mzfile.h"
#include "tmz_tlv.h"
#include "../mzcmt_turbo/mzcmt_turbo.h"


/** @brief Jméno hlavičky CMTSPEED na offsetu 01h (včetně ukončení 0Dh). */
static const uint8_t g_cmtspeed_name[9] = { 'C', 'M', 'T', 'S', 'P', 'E', 'E', 'D', 0x0D };

/** @brief Pulzy UniCMT 1x. */
static const uint16_t g_pulses_1x[4] = TMZ_MZT_CMTSPEED_PULSES_1X;


int tmz_mzt_is_cmtspeed ( const uint8_t *header, uint16_t pulses_us[4] ) {
    if ( !header || header[0] != 0x00 || memcmp ( header + 1, g_cmtspeed_name, sizeof ( g_cmtspeed_name ) ) != 0 ) return 0;
    if ( header[0x12] || header[0x13] ) return 0;   /* fsize musí být 0: hlavička bez těla */
    if ( pulses_us ) {
        for ( int i = 0; i < 4; i++ ) {
            pulses_us[i] = ( uint16_t ) ( header[0x30 + 2 * i] | ( header[0x31 + 2 * i] << 8 ) );
        }
    }
    return 1;
}


void tmz_mzt_build_cmtspeed ( const uint16_t pulses_us[4], uint8_t out[TMZ_MZT_HEADER_SIZE] ) {
    memset ( out, 0, TMZ_MZT_HEADER_SIZE );
    memcpy ( out + 1, g_cmtspeed_name, sizeof ( g_cmtspeed_name ) );
    out[0x20] = 0x01;   /* význam neznámý (hypotéza: verze), UniCMT ho tak zapisuje */
    for ( int i = 0; i < 4; i++ ) {
        out[0x30 + 2 * i] = ( uint8_t ) ( pulses_us[i] & 0xFF );
        out[0x31 + 2 * i] = ( uint8_t ) ( pulses_us[i] >> 8 );
    }
}


/**
 * @brief Projde MZT a spočítá, případně naplní položky.
 * @param data MZT. @param size Délka.
 * @param items Výstup (NULL = jen počítat).
 * @param[out] count Počet položek. @param[out] cmtspeed Počet hlaviček CMTSPEED.
 * @param[out] tail NULL = zbytek kratší než hlavička je chyba; jinak se
 *        čtení u něj zastaví a sem se zapíše jeho délka.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE při poškozené struktuře.
 */
static int walk ( const uint8_t *data, uint32_t size, st_TMZ_MZT_ITEM *items, uint32_t *count, uint32_t *cmtspeed,
                  uint32_t *tail ) {
    uint16_t current[4];
    memcpy ( current, g_pulses_1x, sizeof ( current ) );
    uint32_t pos = 0, n = 0, ncs = 0;
    if ( tail ) *tail = 0;
    while ( pos < size ) {
        if ( size - pos < TMZ_MZT_HEADER_SIZE ) {
            if ( !tail ) return EXIT_FAILURE;
            *tail = size - pos;   /* výplň za posledním souborem (např. CP/M na násobek 128 B) */
            break;
        }
        const uint8_t *h = data + pos;
        uint32_t fsize = ( uint32_t ) ( h[0x12] | ( h[0x13] << 8 ) );
        if ( size - pos - TMZ_MZT_HEADER_SIZE < fsize ) return EXIT_FAILURE;
        uint16_t sp[4];
        int is_cs = tmz_mzt_is_cmtspeed ( h, sp );
        if ( items ) {
            st_TMZ_MZT_ITEM *it = &items[n];
            it->offset = pos;
            it->is_cmtspeed = is_cs;
            it->header = h;
            it->body = ( is_cs || !fsize ) ? NULL : h + TMZ_MZT_HEADER_SIZE;
            it->body_size = is_cs ? 0 : fsize;
            memcpy ( it->pulses_us, is_cs ? sp : current, sizeof ( it->pulses_us ) );
        }
        if ( is_cs ) {
            memcpy ( current, sp, sizeof ( current ) );   /* platí pro další soubory */
            ncs++;
        }
        n++;
        pos += TMZ_MZT_HEADER_SIZE + fsize;
    }
    *count = n;
    *cmtspeed = ncs;
    return EXIT_SUCCESS;
}


int tmz_mzt_parse ( const uint8_t *data, uint32_t size, st_TMZ_MZT *mzt ) {
    return tmz_mzt_parse_tail ( data, size, mzt, NULL );
}


int tmz_mzt_parse_tail ( const uint8_t *data, uint32_t size, st_TMZ_MZT *mzt, uint32_t *tail ) {
    if ( !mzt ) return EXIT_FAILURE;
    memset ( mzt, 0, sizeof ( *mzt ) );
    uint32_t n = 0, ncs = 0;
    if ( !data || size == 0 || walk ( data, size, NULL, &n, &ncs, tail ) != EXIT_SUCCESS || n == 0 ) return EXIT_FAILURE;
    st_TMZ_MZT_ITEM *items = calloc ( n, sizeof ( st_TMZ_MZT_ITEM ) );
    if ( !items ) return EXIT_FAILURE;
    walk ( data, size, items, &n, &ncs, tail );
    mzt->items = items;
    mzt->count = n;
    mzt->cmtspeed_count = ncs;
    return EXIT_SUCCESS;
}


void tmz_mzt_free ( st_TMZ_MZT *mzt ) {
    if ( !mzt ) return;
    free ( mzt->items );
    memset ( mzt, 0, sizeof ( *mzt ) );
}


int tmz_mzt_build_unicmt_blocks ( const st_TMZ_MZT *mzt, const st_TMZ_PROFILE_ARGS *base,
                                  st_TZX_BLOCK ***out, uint32_t *count, int *leading, int *trailing ) {
    if ( leading ) *leading = 0;
    if ( trailing ) *trailing = 0;
    if ( !mzt || !out || !count ) return EXIT_FAILURE;
    *out = NULL;
    *count = 0;
    uint32_t files = mzt->count - mzt->cmtspeed_count;
    st_TZX_BLOCK **blocks = files ? calloc ( files, sizeof ( st_TZX_BLOCK* ) ) : NULL;
    if ( files && !blocks ) return EXIT_FAILURE;

    st_TMZ_PROFILE_ARGS a;
    if ( base ) a = *base;
    else tmz_profile_args_init ( &a );
    uint16_t current[4];
    memcpy ( current, g_pulses_1x, sizeof ( current ) );
    int pending = 0;   /* CMTSPEED čeká na další soubor */
    uint32_t n = 0;
    for ( uint32_t i = 0; i < mzt->count; i++ ) {
        const st_TMZ_MZT_ITEM *it = &mzt->items[i];
        if ( it->is_cmtspeed ) {
            if ( i == 0 ) {
                if ( leading ) *leading = 1;   /* UniCMT hlavičku na prvním místě nepřehraje */
            } else {
                memcpy ( current, it->pulses_us, sizeof ( current ) );
                pending = 1;
            }
            continue;
        }
        pending = 0;
        /* bajty hlavičky beze změny, jen pořadí fsize/fstrt/fexec pro registr */
        st_MZF_HEADER h;
        memcpy ( &h, it->header, sizeof ( h ) );
        h.fsize = endianity_bswap16_LE ( h.fsize );
        h.fstrt = endianity_bswap16_LE ( h.fstrt );
        h.fexec = endianity_bswap16_LE ( h.fexec );
        memcpy ( a.cmtspeed, current, sizeof ( a.cmtspeed ) );
        en_TMZ_PROFILE_ERROR pe;
        blocks[n] = tmz_profile_build_block ( "UNICMT", &a, &h, it->body, it->body_size, &pe );
        if ( !blocks[n] ) {
            for ( uint32_t k = 0; k < n; k++ ) tmz_block_free ( blocks[k] );
            free ( blocks );
            return EXIT_FAILURE;
        }
        n++;
    }
    if ( trailing ) *trailing = pending;
    *out = blocks;
    *count = n;
    return EXIT_SUCCESS;
}


/**
 * @brief Rychlost NORMAL v1.0 jako pulzy UniCMT: floor(1x / dělitel) (UniCMT 2x a 3x = 1x / 2 a / 3 ořezané).
 * @param speed Rychlost (platná, ne 2:1 CPM). @param[out] us Pulzy.
 */
static void normal_speed_pulses ( en_CMTSPEED speed, uint16_t us[4] ) {
    double div = cmtspeed_get_divisor ( speed );
    for ( int i = 0; i < 4; i++ ) us[i] = ( uint16_t ) floor ( g_pulses_1x[i] / div );
}


/**
 * @brief PROFILE_ARGS bloku 0x46.
 * @param mf Blok. @param[out] len Délka textu.
 * @return Text (bez ukončení nulou), nebo NULL.
 */
static const char* mzfile_args ( const st_TMZ_MZFILE *mf, uint16_t *len ) {
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    *len = 0;
    tmz_tlv_iter_init ( &it, mf->block_tlv, mf->block_tlv_length );
    while ( tmz_tlv_iter_next ( &it, &t ) == 1 ) {
        if ( t.tag == TMZ_TAG_PROFILE_ARGS ) {
            *len = t.length;
            return ( const char* ) t.value;
        }
    }
    return NULL;
}


/**
 * @brief Bloky v1.0 0x40 a 0x41 (parsuje se kopie dat).
 * @param block Blok. @param info Výstup (owned = kopie).
 * @return 1, nebo -1 při chybě.
 */
static int info_v1 ( const st_TZX_BLOCK *block, st_TMZ_MZT_BLOCK_INFO *info ) {
    info->owned = malloc ( block->length ? block->length : 1 );
    if ( !info->owned ) return -1;
    if ( block->length ) memcpy ( info->owned, block->data, block->length );
    st_TZX_BLOCK copy = *block;
    copy.data = info->owned;
    en_TZX_ERROR e;
    uint8_t *body = NULL;
    st_MZF_HEADER h;
    uint32_t size;
    if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA ) {
        st_TMZ_MZ_STANDARD_DATA *sd = tmz_block_parse_mz_standard ( &copy, &body, &e );
        if ( !sd ) return -1;
        h = sd->mzf_header;
        size = sd->body_size;
        info->rom_structure = 1;
    } else {
        st_TMZ_MZ_TURBO_DATA *td = tmz_block_parse_mz_turbo ( &copy, &body, &e );
        if ( !td ) return -1;
        h = td->mzf_header;
        size = td->body_size;
        en_CMTSPEED speed = cmtspeed_is_valid ( ( en_CMTSPEED ) td->speed ) ? ( en_CMTSPEED ) td->speed : CMTSPEED_1_1;
        /* NORMAL je záznam ROM; 2:1 CPM má jiné pulzy (cmt.com), ostatní formáty jinou strukturu */
        info->rom_structure = ( td->format == TMZ_FORMAT_NORMAL && speed != CMTSPEED_2_1_CPM );
        if ( info->rom_structure ) normal_speed_pulses ( speed, info->pulses_us );
    }
    if ( h.fsize != size ) return -1;
    mzf_header_swap_endianity ( &h );   /* zpět do pořadí souboru (11h beze změny) */
    memcpy ( info->header, &h, TMZ_MZT_HEADER_SIZE );
    info->body = size ? body : NULL;
    info->body_size = size;
    return 1;
}


/**
 * @brief Blok 0x46: MZF bajty bloku, rychlost podle profilu.
 *
 * Jako záznam ROM se berou ROM-800-*, UNICMT, MZ800EMU-CMT (cmtspeed, nebo
 * poměr jako TAPEMZ-V1-LEGACY normal), TAPEMZ-V1-LEGACY normal,
 * IC-10.2-NORMAL (rychlost z bd) a TC-1.21 bez předzavaděče (loader=N:
 * hlavička a tělo rychlostí a/b, pulzy z S tiků CTC0, 1:1 jako 1x); ostatní profily mají
 * rom_structure = 0.
 * @param block Blok. @param info Výstup (body ukazuje do dat bloku).
 * @return 1, nebo -1 při chybě.
 */
static int info_mzfile ( const st_TZX_BLOCK *block, st_TMZ_MZT_BLOCK_INFO *info ) {
    st_TMZ_MZFILE mf;
    if ( tmz_mzfile_parse ( block, &mf ) != TZX_OK ) return -1;
    uint32_t fsize = ( uint32_t ) ( mf.header[0x12] | ( mf.header[0x13] << 8 ) );
    if ( fsize != mf.body_size ) {
        tmz_mzfile_free ( &mf );
        return -1;
    }
    memcpy ( info->header, mf.header, TMZ_MZT_HEADER_SIZE );
    info->body = mf.body_size ? mf.body : NULL;
    info->body_size = mf.body_size;
    uint16_t len = 0;
    const char *text = mzfile_args ( &mf, &len );
    st_TMZ_PROFILE_ARGS a;
    int args_ok = ( tmz_profile_args_parse ( text, len, &a ) == EXIT_SUCCESS );
    if ( strcmp ( mf.profile_id, "ROM-800-NOMINAL" ) == 0 || strcmp ( mf.profile_id, "ROM-800-EXACT" ) == 0 ) {
        info->rom_structure = 1;
    } else if ( strcmp ( mf.profile_id, "UNICMT" ) == 0 && args_ok ) {
        info->rom_structure = 1;
        if ( a.cmtspeed[0] && a.cmtspeed[1] && a.cmtspeed[2] && a.cmtspeed[3] ) {
            memcpy ( info->pulses_us, a.cmtspeed, sizeof ( info->pulses_us ) );
        }
    } else if ( strcmp ( mf.profile_id, TMZ_PROFILE_ID_MZ800EMU_CMT ) == 0 && args_ok ) {
        /*
         * Signál mztape MZ800_SANE: hlavička a tělo s tónem a tapemarkem bez kopií.
         * Vlastní pulzy = cmtspeed; poměr jako 0x41 NORMAL (floor(1x / dělitel)),
         * 2:1 CPM ne (pulzy cmt.com, poměr by je nepopsal).
         */
        if ( a.cmtspeed[0] && a.cmtspeed[1] && a.cmtspeed[2] && a.cmtspeed[3] ) {
            info->rom_structure = 1;
            memcpy ( info->pulses_us, a.cmtspeed, sizeof ( info->pulses_us ) );
        } else {
            en_CMTSPEED speed = ( a.speed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : a.speed;
            info->rom_structure = ( speed != CMTSPEED_2_1_CPM && cmtspeed_is_valid ( speed ) );
            if ( info->rom_structure ) normal_speed_pulses ( speed, info->pulses_us );
        }
    } else if ( strcmp ( mf.profile_id, "TAPEMZ-V1-LEGACY" ) == 0 && args_ok && a.legacy_format == TMZ_LEGACY_NORMAL ) {
        en_CMTSPEED speed = ( a.speed == CMTSPEED_NONE ) ? CMTSPEED_1_1 : a.speed;
        info->rom_structure = ( speed != CMTSPEED_2_1_CPM && cmtspeed_is_valid ( speed ) );
        if ( info->rom_structure ) normal_speed_pulses ( speed, info->pulses_us );
    } else if ( strcmp ( mf.profile_id, "IC-10.2-NORMAL" ) == 0 && args_ok ) {
        /*
         * Intercopy NORMAL zapisuje hlavičku a tělo jako ROM. Rychlost
         * UniCMT z bd: pulzy 1x krát 1200/bd zaokrouhlené dolů, při 1200 tedy 1x
         * (skutečné pulzy Intercopy 1200 se od 1x liší o jednotky µs).
         */
        uint32_t bd = a.bd ? a.bd : 1200;
        info->rom_structure = ( bd == 600 || bd == 1200 || bd == 2400 || bd == 2800 || bd == 3200 );
        for ( int i = 0; info->rom_structure && i < 4; i++ ) info->pulses_us[i] = ( uint16_t ) ( ( uint32_t ) g_pulses_1x[i] * 1200u / bd );
    } else if ( strcmp ( mf.profile_id, "TC-1.21" ) == 0 && args_ok && !a.loader ) {
        /*
         * TurboCopy Loader N: hlavička a tělo s tónem a tapemarkem jako ROM, rychlostí
         * a/b. Pulzy UniCMT z S = 71 + floor(480·b/a) tiků CTC0 (1 108 405 Hz):
         * LONG S + S, SHORT (S+1)/2 + S/2 tiků, zaokrouhleno na µs. Poměr 1:1
         * (S 551, perioda 994 µs, ROM ho přečte) zůstává rychlostí 1x, jako
         * dřív blok 0x40 1:1; UniCMT navíc vyžaduje první soubor 1x.
         */
        unsigned ra = a.tc_a, rb = a.tc_b;
        if ( !ra && mzcmt_turbo_speed_to_ratio ( ( a.speed == CMTSPEED_NONE ) ? CMTSPEED_2_1 : a.speed, &ra, &rb ) != EXIT_SUCCESS ) {
            ra = 0;
        }
        info->rom_structure = ( ra != 0 && rb != 0 );
        if ( info->rom_structure && ra != rb ) {
            unsigned s = 71 + ( 480 * rb ) / ra;
            const unsigned ticks[4] = { s, s, ( s + 1 ) / 2, s / 2 };
            for ( int i = 0; i < 4; i++ ) info->pulses_us[i] = ( uint16_t ) lround ( ticks[i] * 1e6 / 1108405.0 );
        }
    }
    tmz_mzfile_free ( &mf );   /* uvolní jen pole částí, header a body ukazují do bloku */
    return 1;
}


int tmz_mzt_block_info ( const st_TZX_BLOCK *block, st_TMZ_MZT_BLOCK_INFO *info ) {
    if ( !block || !info ) return -1;
    memset ( info, 0, sizeof ( *info ) );
    memcpy ( info->pulses_us, g_pulses_1x, sizeof ( info->pulses_us ) );
    int r;
    if ( block->id == TMZ_BLOCK_ID_MZ_STANDARD_DATA || block->id == TMZ_BLOCK_ID_MZ_TURBO_DATA ) {
        r = info_v1 ( block, info );
    } else if ( block->id == TMZ_BLOCK_ID_MZ_FILE ) {
        r = info_mzfile ( block, info );
    } else {
        return 0;
    }
    if ( r != 1 ) tmz_mzt_block_info_free ( info );
    return r;
}


void tmz_mzt_block_info_free ( st_TMZ_MZT_BLOCK_INFO *info ) {
    if ( !info ) return;
    free ( info->owned );
    info->owned = NULL;
    info->body = NULL;
}


/**
 * @brief Připojí bajty k rostoucímu bufferu.
 * @param buf Buffer. @param size Obsazeno. @param cap Kapacita. @param d Data. @param n Délka.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE při selhání alokace nebo přetečení.
 */
static int buf_add ( uint8_t **buf, uint32_t *size, uint32_t *cap, const uint8_t *d, uint32_t n ) {
    if ( n > UINT32_MAX - *size ) return EXIT_FAILURE;
    if ( *size + n > *cap ) {
        uint64_t c = *cap ? *cap : 4096;
        while ( c < ( uint64_t ) *size + n ) c *= 2;
        if ( c > UINT32_MAX ) c = UINT32_MAX;
        uint8_t *nb = realloc ( *buf, ( size_t ) c );
        if ( !nb ) return EXIT_FAILURE;
        *buf = nb;
        *cap = ( uint32_t ) c;
    }
    if ( n ) memcpy ( *buf + *size, d, n );
    *size += n;
    return EXIT_SUCCESS;
}


int tmz_mzt_export ( const st_TZX_FILE *file, en_TMZ_MZT_MODE mode, uint8_t **out_data, uint32_t *out_size,
                     tmz_mzt_warn_cb warn, void *user ) {
    if ( !out_data || !out_size ) return EXIT_FAILURE;
    *out_data = NULL;
    *out_size = 0;
    if ( !file ) return EXIT_FAILURE;
    uint8_t *buf = NULL;
    uint32_t size = 0, cap = 0, files = 0;
    uint16_t prev[4];
    memcpy ( prev, g_pulses_1x, sizeof ( prev ) );
    int ok = 1;
    for ( uint32_t i = 0; ok && i < file->block_count; i++ ) {
        st_TMZ_MZT_BLOCK_INFO info;
        int r = tmz_mzt_block_info ( &file->blocks[i], &info );
        if ( r == 0 ) continue;
        if ( r < 0 ) {
            if ( warn ) warn ( user, i, "damaged block or MZF size differs from the body" );
            ok = 0;
            break;
        }
        int is_1x = ( memcmp ( info.pulses_us, g_pulses_1x, sizeof ( prev ) ) == 0 );
        if ( mode == TMZ_MZT_UNICMT ) {
            if ( !info.rom_structure ) {
                if ( warn ) warn ( user, i, "not a ROM header + body recording, cannot be written as UniCMT MZT" );
                ok = 0;
            } else if ( files == 0 && !is_1x ) {
                if ( warn ) warn ( user, i, "the first file must be 1x (UniCMT does not apply a leading CMTSPEED)" );
                ok = 0;
            } else if ( memcmp ( info.pulses_us, prev, sizeof ( prev ) ) != 0 ) {
                uint8_t cs[TMZ_MZT_HEADER_SIZE];
                tmz_mzt_build_cmtspeed ( info.pulses_us, cs );
                ok = ( buf_add ( &buf, &size, &cap, cs, sizeof ( cs ) ) == EXIT_SUCCESS );
                memcpy ( prev, info.pulses_us, sizeof ( prev ) );
            }
        } else if ( warn ) {
            if ( !info.rom_structure ) warn ( user, i, "not a ROM header + body recording, timing lost in classic MZT" );
            else if ( !is_1x ) warn ( user, i, "timing not 1x, lost in classic MZT" );
        }
        if ( ok ) {
            ok = ( buf_add ( &buf, &size, &cap, info.header, TMZ_MZT_HEADER_SIZE ) == EXIT_SUCCESS
                   && buf_add ( &buf, &size, &cap, info.body, info.body_size ) == EXIT_SUCCESS );
            files++;
        }
        tmz_mzt_block_info_free ( &info );
    }
    if ( !ok || files == 0 ) {
        free ( buf );
        return EXIT_FAILURE;
    }
    *out_data = buf;
    *out_size = size;
    return EXIT_SUCCESS;
}
