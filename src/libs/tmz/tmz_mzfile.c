/**
 * @file   tmz_mzfile.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace bloku 0x46 MZ File (viz tmz_mzfile.h).
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
#include "tmz_tlv.h"
#include "tmz_mzfile.h"


/**
 * @brief Povolený znak ID profilu (spec 8.1).
 * @param c Znak.
 * @return Nenulové, je-li povolený.
 */
static int profile_char_ok ( uint8_t c ) {
    return ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '.' || c == '-';
}


/**
 * @brief Čtecí kurzor nad úsekem dat s kontrolou hranic.
 *
 * @invariant pos <= len
 */
typedef struct st_CURSOR {
    const uint8_t *p;   /**< začátek úseku */
    uint32_t len;       /**< délka úseku */
    uint32_t pos;       /**< aktuální pozice */
} st_CURSOR;


/**
 * @brief Ověří, že v úseku zbývá aspoň n bajtů.
 * @param c Kurzor.
 * @param n Požadovaný počet bajtů.
 * @return Nenulové, pokud zbývá.
 */
static int cur_has ( const st_CURSOR *c, uint32_t n ) {
    return c->len - c->pos >= n;
}


en_TZX_ERROR tmz_mzfile_parse ( const st_TZX_BLOCK *block, st_TMZ_MZFILE *out ) {
    if ( !block || !out ) return TZX_ERROR_NULL_INPUT;
    memset ( out, 0, sizeof ( *out ) );
    st_TMZ_PART *parts = NULL;
    uint8_t count = 0;
    if ( block->id != TMZ_BLOCK_ID_MZ_FILE || !block->data || block->length < 4 ) return TZX_ERROR_INVALID_BLOCK;
    if ( tmz_rd32 ( block->data ) != block->length - 4 ) return TZX_ERROR_INVALID_BLOCK;

    st_CURSOR c = { block->data + 4, block->length - 4, 0 };
    if ( !cur_has ( &c, 2 ) ) return TZX_ERROR_INVALID_BLOCK;
    if ( c.p[0] != TMZ_MZFILE_VERSION ) return TZX_ERROR_UNSUPPORTED;
    uint8_t n = c.p[1];
    c.pos = 2;
    if ( n > TMZ_MZFILE_PROFILE_ID_MAX || !cur_has ( &c, n ) ) return TZX_ERROR_INVALID_BLOCK;
    for ( uint8_t i = 0; i < n; i++ ) {
        if ( !profile_char_ok ( c.p[c.pos + i] ) ) {
            memset ( out, 0, sizeof ( *out ) );
            return TZX_ERROR_INVALID_BLOCK;
        }
        out->profile_id[i] = ( char ) c.p[c.pos + i];
    }
    c.pos += n;

    if ( !cur_has ( &c, TMZ_MZFILE_HEADER_SIZE + 4 ) ) goto bad;
    out->header = c.p + c.pos;
    c.pos += TMZ_MZFILE_HEADER_SIZE;
    out->body_size = tmz_rd32 ( c.p + c.pos );
    c.pos += 4;
    if ( !cur_has ( &c, out->body_size ) ) goto bad;
    out->body = c.p + c.pos;
    c.pos += out->body_size;

    if ( !cur_has ( &c, 2 ) ) goto bad;
    out->block_tlv_length = tmz_rd16 ( c.p + c.pos );
    c.pos += 2;
    if ( !cur_has ( &c, out->block_tlv_length ) ) goto bad;
    out->block_tlv = c.p + c.pos;
    if ( tmz_tlv_area_check ( out->block_tlv, out->block_tlv_length ) != EXIT_SUCCESS ) goto bad;
    c.pos += out->block_tlv_length;

    if ( !cur_has ( &c, 1 ) ) goto bad;
    count = c.p[c.pos++];
    if ( count == 0 ) goto bad;

    parts = calloc ( count, sizeof ( st_TMZ_PART ) );
    if ( !parts ) {
        memset ( out, 0, sizeof ( *out ) );
        return TZX_ERROR_ALLOC;
    }
    for ( uint8_t i = 0; i < count; i++ ) {
        st_TMZ_PART *pt = &parts[i];
        if ( !cur_has ( &c, 4 ) ) goto bad;
        uint32_t plen = tmz_rd32 ( c.p + c.pos );
        c.pos += 4;
        if ( plen < 3 || !cur_has ( &c, plen ) ) goto bad;
        st_CURSOR pc = { c.p + c.pos, plen, 3 };
        pt->role = pc.p[0];
        pt->family = pc.p[1];
        pt->source = pc.p[2];
        int known = 1;
        switch ( pt->source ) {
            case TMZ_SOURCE_MZF_HEADER:
            case TMZ_SOURCE_MZF_BODY:
                break;
            case TMZ_SOURCE_BODY_SLICE:
                if ( !cur_has ( &pc, 8 ) ) goto bad;
                pt->slice_offset = tmz_rd32 ( pc.p + pc.pos );
                pt->slice_length = tmz_rd32 ( pc.p + pc.pos + 4 );
                pc.pos += 8;
                break;
            case TMZ_SOURCE_INLINE:
                if ( !cur_has ( &pc, 4 ) ) goto bad;
                pt->inline_length = tmz_rd32 ( pc.p + pc.pos );
                pc.pos += 4;
                if ( !cur_has ( &pc, pt->inline_length ) ) goto bad;
                pt->inline_data = pc.p + pc.pos;
                pc.pos += pt->inline_length;
                break;
            default:
                known = 0;   /* délku polí nelze určit: část bez TLV, chyba se ohlásí při použití */
                break;
        }
        if ( known ) {
            pt->tlv = pc.p + pc.pos;
            pt->tlv_length = pc.len - pc.pos;
            if ( tmz_tlv_area_check ( pt->tlv, pt->tlv_length ) != EXIT_SUCCESS ) goto bad;
        }
        c.pos += plen;
    }
    if ( c.pos != c.len ) goto bad;
    out->part_count = count;
    out->parts = parts;
    return TZX_OK;

bad:
    free ( parts );
    memset ( out, 0, sizeof ( *out ) );
    return TZX_ERROR_INVALID_BLOCK;
}


void tmz_mzfile_free ( st_TMZ_MZFILE *mf ) {
    if ( !mf ) return;
    free ( mf->parts );
    mf->parts = NULL;
    mf->part_count = 0;
}


/**
 * @brief Velikost polí zdroje dat části (bez délky části a bez TLV).
 * @param p Část.
 * @return Počet bajtů (uint64: inline_length blízko UINT32_MAX nesmí přetéct).
 */
static uint64_t part_source_size ( const st_TMZ_PART *p ) {
    if ( p->source == TMZ_SOURCE_BODY_SLICE ) return 8;
    if ( p->source == TMZ_SOURCE_INLINE ) return 4 + ( uint64_t ) p->inline_length;
    return 0;
}


st_TZX_BLOCK* tmz_mzfile_create_block ( const st_TMZ_MZFILE *mf, en_TZX_ERROR *err ) {
    en_TZX_ERROR local;
    if ( !err ) err = &local;
    if ( !mf || !mf->header || ( mf->body_size && !mf->body ) || mf->part_count == 0 || !mf->parts ) {
        *err = TZX_ERROR_NULL_INPUT;
        return NULL;
    }
    *err = TZX_ERROR_INVALID_BLOCK;
    size_t n = strnlen ( mf->profile_id, sizeof ( mf->profile_id ) );
    if ( n > TMZ_MZFILE_PROFILE_ID_MAX ) return NULL;
    for ( size_t i = 0; i < n; i++ ) {
        if ( !profile_char_ok ( ( uint8_t ) mf->profile_id[i] ) ) return NULL;
    }

    uint64_t total = 4 + 2 + ( uint64_t ) n + TMZ_MZFILE_HEADER_SIZE + 4 + ( uint64_t ) mf->body_size
                     + 2 + mf->block_tlv_length + 1;
    for ( uint8_t i = 0; i < mf->part_count; i++ ) {
        const st_TMZ_PART *p = &mf->parts[i];
        if ( p->source > TMZ_SOURCE_INLINE ) return NULL;
        if ( ( p->tlv_length && !p->tlv ) || ( p->inline_length && !p->inline_data ) ) return NULL;
        /* TLV musí tvořit celé položky, jinak by blok odmítl tmz_mzfile_parse() */
        if ( p->tlv_length && tmz_tlv_area_check ( p->tlv, p->tlv_length ) != EXIT_SUCCESS ) return NULL;
        total += 4 + 3 + ( uint64_t ) part_source_size ( p ) + p->tlv_length;
    }
    if ( mf->block_tlv_length && !mf->block_tlv ) return NULL;
    if ( mf->block_tlv_length && tmz_tlv_area_check ( mf->block_tlv, mf->block_tlv_length ) != EXIT_SUCCESS ) return NULL;
    if ( total > UINT32_MAX ) return NULL;

    st_TZX_BLOCK *b = calloc ( 1, sizeof ( st_TZX_BLOCK ) );
    uint8_t *d = b ? malloc ( ( size_t ) total ) : NULL;
    if ( !d ) {
        free ( b );
        *err = TZX_ERROR_ALLOC;
        return NULL;
    }
    b->id = TMZ_BLOCK_ID_MZ_FILE;
    b->length = ( uint32_t ) total;
    b->data = d;

    uint32_t o = 0;
    tmz_wr32 ( d + o, ( uint32_t ) total - 4 );
    o += 4;
    d[o++] = TMZ_MZFILE_VERSION;
    d[o++] = ( uint8_t ) n;
    memcpy ( d + o, mf->profile_id, n );
    o += ( uint32_t ) n;
    memcpy ( d + o, mf->header, TMZ_MZFILE_HEADER_SIZE );
    o += TMZ_MZFILE_HEADER_SIZE;
    tmz_wr32 ( d + o, mf->body_size );
    o += 4;
    if ( mf->body_size ) memcpy ( d + o, mf->body, mf->body_size );
    o += mf->body_size;
    tmz_wr16 ( d + o, mf->block_tlv_length );
    o += 2;
    if ( mf->block_tlv_length ) memcpy ( d + o, mf->block_tlv, mf->block_tlv_length );
    o += mf->block_tlv_length;
    d[o++] = mf->part_count;
    for ( uint8_t i = 0; i < mf->part_count; i++ ) {
        const st_TMZ_PART *p = &mf->parts[i];
        tmz_wr32 ( d + o, ( uint32_t ) ( 3 + part_source_size ( p ) + p->tlv_length ) );
        o += 4;
        d[o++] = p->role;
        d[o++] = p->family;
        d[o++] = p->source;
        if ( p->source == TMZ_SOURCE_BODY_SLICE ) {
            tmz_wr32 ( d + o, p->slice_offset );
            tmz_wr32 ( d + o + 4, p->slice_length );
            o += 8;
        } else if ( p->source == TMZ_SOURCE_INLINE ) {
            tmz_wr32 ( d + o, p->inline_length );
            o += 4;
            if ( p->inline_length ) memcpy ( d + o, p->inline_data, p->inline_length );
            o += p->inline_length;
        }
        if ( p->tlv_length ) memcpy ( d + o, p->tlv, p->tlv_length );
        o += p->tlv_length;
    }
    *err = TZX_OK;
    return b;
}


const uint8_t* tmz_mzfile_part_data ( const st_TMZ_MZFILE *mf, const st_TMZ_PART *part,
                                      uint32_t *size, en_TMZ_PART_ERROR *err ) {
    en_TMZ_PART_ERROR local;
    if ( !err ) err = &local;
    *err = TMZ_PART_ERROR_BAD_SOURCE;
    if ( size ) *size = 0;
    if ( !mf || !part ) return NULL;
    const uint8_t *p = NULL;
    uint32_t s = 0;
    switch ( part->source ) {
        case TMZ_SOURCE_MZF_HEADER:
            p = mf->header;
            s = TMZ_MZFILE_HEADER_SIZE;
            break;
        case TMZ_SOURCE_MZF_BODY:
            p = mf->body;
            s = mf->body_size;
            break;
        case TMZ_SOURCE_BODY_SLICE:
            if ( part->slice_offset > mf->body_size || part->slice_length > mf->body_size - part->slice_offset ) return NULL;
            p = mf->body + part->slice_offset;
            s = part->slice_length;
            break;
        case TMZ_SOURCE_INLINE:
            p = part->inline_data;
            s = part->inline_length;
            break;
        default:
            return NULL;
    }
    if ( size ) *size = s;
    *err = TMZ_PART_OK;
    return p;
}


const char* tmz_part_role_name ( uint8_t role ) {
    switch ( role ) {
        case TMZ_PART_ROLE_HEADER:        return "header";
        case TMZ_PART_ROLE_BODY:          return "body";
        case TMZ_PART_ROLE_LOADER_HEADER: return "loader header";
        case TMZ_PART_ROLE_LOADER:        return "loader";
        case TMZ_PART_ROLE_BSD_CHUNK:     return "BSD chunk";
        default:                          return "other";
    }
}


const char* tmz_family_name ( uint8_t family ) {
    switch ( family ) {
        case TMZ_FAMILY_FM:         return "FM";
        case TMZ_FAMILY_FSK:        return "FSK";
        case TMZ_FAMILY_SLOW:       return "SLOW";
        case TMZ_FAMILY_DIRECT:     return "DIRECT";
        case TMZ_FAMILY_MANCHESTER: return "Manchester";
        case TMZ_FAMILY_PILOT:      return "PILOT";
        default:                    return "unknown";
    }
}


const char* tmz_part_error_string ( en_TMZ_PART_ERROR err ) {
    switch ( err ) {
        case TMZ_PART_OK:                       return "OK";
        case TMZ_PART_ERROR_TLV_STRUCTURE:      return "Malformed TLV area";
        case TMZ_PART_ERROR_UNKNOWN_CRITICAL:   return "Unknown critical tag";
        case TMZ_PART_ERROR_BAD_VALUE:          return "Invalid tag value";
        case TMZ_PART_ERROR_MISSING_PULSES:     return "FM part without PULSES";
        case TMZ_PART_ERROR_UNSUPPORTED_FAMILY: return "Unsupported modulation family";
        case TMZ_PART_ERROR_BAD_SOURCE:         return "Invalid data source";
    }
    return "Unknown part error";
}
