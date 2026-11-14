/**
 * @file   tmz_tlv.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.1
 * @brief  Implementace kódování TLV pro blok 0x46 TMZ v1.1 (viz tmz_tlv.h).
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

#include "tmz_tlv.h"


void tmz_tlv_iter_init ( st_TMZ_TLV_ITER *it, const uint8_t *area, uint32_t length ) {
    it->area = area;
    it->length = area ? length : 0;
    it->pos = 0;
}


int tmz_tlv_iter_next ( st_TMZ_TLV_ITER *it, st_TMZ_TLV *out ) {
    if ( it->pos == it->length ) return 0;
    if ( it->length - it->pos < 4 ) return -1;
    const uint8_t *p = it->area + it->pos;
    uint16_t raw = tmz_rd16 ( p );
    uint16_t len = tmz_rd16 ( p + 2 );
    if ( it->length - it->pos - 4 < len ) return -1;
    out->tag = ( uint16_t ) ( raw & TMZ_TLV_TAG_MASK );
    out->critical = ( raw & TMZ_TLV_CRITICAL ) ? 1 : 0;
    out->length = len;
    out->value = p + 4;
    it->pos += 4u + len;
    return 1;
}


int tmz_tlv_area_check ( const uint8_t *area, uint32_t length ) {
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    tmz_tlv_iter_init ( &it, area, length );
    int r;
    while ( ( r = tmz_tlv_iter_next ( &it, &t ) ) == 1 ) { }
    return ( r == 0 ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


void tmz_tlvbuf_init ( st_TMZ_TLVBUF *buf ) {
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}


void tmz_tlvbuf_free ( st_TMZ_TLVBUF *buf ) {
    if ( !buf ) return;
    free ( buf->data );
    tmz_tlvbuf_init ( buf );
}


int tmz_tlvbuf_add ( st_TMZ_TLVBUF *buf, uint16_t tag, int critical, const void *value, uint16_t length ) {
    if ( !buf || ( length && !value ) || ( tag & TMZ_TLV_CRITICAL ) ) return EXIT_FAILURE;
    if ( ( uint64_t ) buf->length + 4u + length > UINT32_MAX ) return EXIT_FAILURE;   /* délka bufferu by přetekla */
    uint32_t need = buf->length + 4u + length;
    if ( need > buf->capacity ) {
        uint64_t cap = buf->capacity ? buf->capacity : 64;
        while ( cap < need ) cap *= 2;
        if ( cap > UINT32_MAX ) cap = need;
        uint8_t *n = realloc ( buf->data, ( size_t ) cap );
        if ( !n ) return EXIT_FAILURE;
        buf->data = n;
        buf->capacity = ( uint32_t ) cap;
    }
    tmz_wr16 ( buf->data + buf->length, ( uint16_t ) ( tag | ( critical ? TMZ_TLV_CRITICAL : 0 ) ) );
    tmz_wr16 ( buf->data + buf->length + 2, length );
    if ( length ) memcpy ( buf->data + buf->length + 4, value, length );
    buf->length = need;
    return EXIT_SUCCESS;
}
