/**
 * @file   tmz_part_manchester.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.0.0
 * @brief  Implementace převodu TLV části MANCHESTER <-> parametry (viz tmz_part_manchester.h).
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

#include <stdlib.h>
#include <string.h>

#include "tmz_part_manchester.h"


/**
 * @brief Index tagu v masce viděných tagů (kontrola opakování).
 * @param tag Číslo tagu.
 * @return 0..11 pro tagy, které se nesmí opakovat, jinak -1 (CONTEXT, neznámé).
 */
static int seen_index ( uint16_t tag ) {
    if ( tag >= TMZ_TAG_TIMEBASE && tag <= TMZ_TAG_READER ) return tag - TMZ_TAG_TIMEBASE;
    if ( tag >= TMZ_TAG_MANCH_HALVES && tag <= TMZ_TAG_MANCH_CARRY ) return 5 + ( tag - TMZ_TAG_MANCH_HALVES );
    return -1;
}


/**
 * @brief Povinnost známého tagu podle tabulek spec (kap. 5.3, 7.2).
 * @param tag Číslo tagu.
 * @return 1 = povinný, 0 = nepovinný, -1 = neznámý tag.
 */
static int known_critical ( uint16_t tag ) {
    switch ( tag ) {
        case TMZ_TAG_STOP:
        case TMZ_TAG_READER:
            return 0;
        case TMZ_TAG_TIMEBASE:
        case TMZ_TAG_LEVEL:
        case TMZ_TAG_PAUSE:
            return 1;
        default:
            return ( tag >= TMZ_TAG_MANCH_HALVES && tag <= TMZ_TAG_MANCH_CONTEXT ) ? 1 : -1;
    }
}


en_TMZ_PART_ERROR tmz_part_manchester_decode ( const uint8_t *tlv, uint32_t tlv_length, uint32_t default_timebase,
                                               st_MZCMT_MANCH_PARAMS *out, st_TMZ_PART_COMMON *common ) {
    mzcmt_manch_params_init ( out );
    memset ( common, 0, sizeof ( *common ) );
    out->timebase = default_timebase;
    int have_halves = 0;
    uint32_t seen = 0;
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    tmz_tlv_iter_init ( &it, tlv, tlv_length );
    int r;
    while ( ( r = tmz_tlv_iter_next ( &it, &t ) ) == 1 ) {
        int kc = known_critical ( t.tag );
        int crit = ( kc >= 0 ) ? kc : t.critical;
        int si = seen_index ( t.tag );
        if ( si >= 0 ) {
            if ( seen & ( 1u << si ) ) {
                if ( crit ) return TMZ_PART_ERROR_BAD_VALUE;
                continue;   /* opakovaný nepovinný tag: platí první výskyt (spec 5.1) */
            }
            seen |= 1u << si;
        }
        const uint8_t *v = t.value;
        int len_ok = 1;
        switch ( t.tag ) {
            case TMZ_TAG_TIMEBASE:
                if ( ( len_ok = ( t.length == 4 ) ) ) {
                    out->timebase = tmz_rd32 ( v );
                    if ( !out->timebase ) return TMZ_PART_ERROR_BAD_VALUE;
                }
                break;
            case TMZ_TAG_LEVEL:
                if ( ( len_ok = ( t.length == 1 ) ) ) {
                    if ( v[0] > 1 ) return TMZ_PART_ERROR_BAD_VALUE;
                    out->level = v[0];
                }
                break;
            case TMZ_TAG_PAUSE:
                if ( ( len_ok = ( t.length == 4 || ( t.length == 5 && v[4] <= 1 ) ) ) ) {
                    out->pause = tmz_rd32 ( v );
                    out->pause_level = ( t.length == 5 ) ? v[4] : 0;
                }
                break;
            case TMZ_TAG_STOP:
                if ( ( len_ok = ( t.length == 1 ) ) ) common->stop = v[0] ? 1 : 0;
                break;
            case TMZ_TAG_READER:
                if ( ( len_ok = ( t.length == 3 ) ) ) {
                    common->has_reader = 1;
                    common->reader_routine = v[0];
                    common->reader_n = tmz_rd16 ( v + 1 );
                }
                break;
            case TMZ_TAG_MANCH_HALVES:
                if ( ( len_ok = ( t.length == 16 ) ) ) {
                    out->half1[0] = tmz_rd32 ( v );
                    out->half1[1] = tmz_rd32 ( v + 4 );
                    out->half2[0] = tmz_rd32 ( v + 8 );
                    out->half2[1] = tmz_rd32 ( v + 12 );
                    have_halves = 1;
                }
                break;
            case TMZ_TAG_MANCH_PILOT:
                if ( ( len_ok = ( t.length == 6 ) ) ) {
                    out->pilot_count = tmz_rd32 ( v );
                    out->pilot_bits = v[4];
                    out->pilot_pattern = v[5];
                }
                break;
            case TMZ_TAG_MANCH_SYNC:
                if ( ( len_ok = ( t.length == 2 ) ) ) {
                    out->sync_bits = v[0];
                    out->sync_pattern = v[1];
                }
                break;
            case TMZ_TAG_MANCH_FLAG:
                if ( ( len_ok = ( t.length == 1 ) ) ) {
                    out->has_flag = 1;
                    out->flag = v[0];
                }
                break;
            case TMZ_TAG_MANCH_CHECKSUM:
                if ( ( len_ok = ( t.length == 1 || t.length == 3 ) ) ) {
                    out->checksum = v[0];
                    if ( t.length == 3 ) {
                        out->checksum_explicit = 1;
                        out->checksum_bytes[0] = v[1];
                        out->checksum_bytes[1] = v[2];
                    }
                }
                break;
            case TMZ_TAG_MANCH_END:
                if ( ( len_ok = ( t.length == 2 ) ) ) {
                    out->end_bits = v[0];
                    out->end_pattern = v[1];
                }
                break;
            case TMZ_TAG_MANCH_CARRY:
                if ( ( len_ok = ( t.length == 1 ) ) ) out->carry_adjust = ( int8_t ) v[0];
                break;
            case TMZ_TAG_MANCH_CONTEXT:
                if ( ( len_ok = ( t.length % 6 == 0 ) ) ) {
                    for ( uint32_t o = 0; o < t.length; o += 6 ) {
                        if ( out->context_count >= MZCMT_MANCH_MAX_CONTEXT ) return TMZ_PART_ERROR_BAD_VALUE;
                        st_MZCMT_MANCH_CONTEXT *c = &out->context[out->context_count++];
                        c->ctx = v[o];
                        c->bit = v[o + 1];
                        c->half2 = tmz_rd32 ( v + o + 2 );
                    }
                }
                break;
            default:
                if ( t.critical ) return TMZ_PART_ERROR_UNKNOWN_CRITICAL;
                break;
        }
        if ( !len_ok ) {
            if ( crit ) return TMZ_PART_ERROR_BAD_VALUE;
            if ( t.tag == TMZ_TAG_READER ) common->has_reader = 0;
        }
    }
    if ( r < 0 ) return TMZ_PART_ERROR_TLV_STRUCTURE;
    if ( !have_halves ) return TMZ_PART_ERROR_MISSING_PULSES;
    if ( mzcmt_manch_params_validate ( out ) != EXIT_SUCCESS ) return TMZ_PART_ERROR_BAD_VALUE;
    return TMZ_PART_OK;
}


int tmz_part_manchester_encode ( const st_MZCMT_MANCH_PARAMS *p, const st_TMZ_PART_COMMON *common,
                                 uint32_t default_timebase, st_TMZ_TLVBUF *buf ) {
    if ( !p || !buf || mzcmt_manch_params_validate ( p ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    uint8_t v[16];
    int r = EXIT_SUCCESS;
    if ( p->timebase != default_timebase ) {
        tmz_wr32 ( v, p->timebase );
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_TIMEBASE, 1, v, 4 );
    }
    if ( p->level != 1 ) {
        v[0] = p->level;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_LEVEL, 1, v, 1 );
    }
    if ( p->pause ) {
        tmz_wr32 ( v, p->pause );
        v[4] = p->pause_level;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_PAUSE, 1, v, p->pause_level ? 5 : 4 );
    }
    if ( common && common->stop ) {
        v[0] = 1;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_STOP, 0, v, 1 );
    }
    if ( common && common->has_reader ) {
        v[0] = common->reader_routine;
        tmz_wr16 ( v + 1, common->reader_n );
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_READER, 0, v, 3 );
    }
    tmz_wr32 ( v, p->half1[0] );
    tmz_wr32 ( v + 4, p->half1[1] );
    tmz_wr32 ( v + 8, p->half2[0] );
    tmz_wr32 ( v + 12, p->half2[1] );
    r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_HALVES, 1, v, 16 );
    if ( p->pilot_count ) {
        tmz_wr32 ( v, p->pilot_count );
        v[4] = p->pilot_bits;
        v[5] = p->pilot_pattern;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_PILOT, 1, v, 6 );
    }
    if ( p->sync_bits ) {
        v[0] = p->sync_bits;
        v[1] = p->sync_pattern;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_SYNC, 1, v, 2 );
    }
    if ( p->has_flag ) {
        v[0] = p->flag;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_FLAG, 1, v, 1 );
    }
    if ( p->checksum ) {
        v[0] = p->checksum;
        v[1] = p->checksum_bytes[0];
        v[2] = p->checksum_bytes[1];
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_CHECKSUM, 1, v, p->checksum_explicit ? 3 : 1 );
    }
    if ( p->end_bits ) {
        v[0] = p->end_bits;
        v[1] = p->end_pattern;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_END, 1, v, 2 );
    }
    if ( p->carry_adjust ) {
        v[0] = ( uint8_t ) p->carry_adjust;
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_CARRY, 1, v, 1 );
    }
    if ( p->context_count ) {
        uint8_t c[MZCMT_MANCH_MAX_CONTEXT * 6];
        for ( uint32_t i = 0; i < p->context_count; i++ ) {
            c[i * 6] = p->context[i].ctx;
            c[i * 6 + 1] = p->context[i].bit;
            tmz_wr32 ( c + i * 6 + 2, p->context[i].half2 );
        }
        r |= tmz_tlvbuf_add ( buf, TMZ_TAG_MANCH_CONTEXT, 1, c, ( uint16_t ) ( p->context_count * 6u ) );
    }
    return r ? EXIT_FAILURE : EXIT_SUCCESS;
}
