/**
 * @file   tmz_part_fm.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.1
 * @brief  Implementace převodu TLV části FM <-> parametry (viz tmz_part_fm.h).
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
#include "tmz_part_fm.h"


/**
 * @brief Index tagu v masce viděných tagů (kontrola opakování).
 * @param tag Číslo tagu.
 * @return 0..11 pro tagy, které se nesmí opakovat, jinak -1.
 */
static int seen_index ( uint16_t tag ) {
    if ( tag >= TMZ_TAG_TIMEBASE && tag <= TMZ_TAG_READER ) return tag - TMZ_TAG_TIMEBASE;
    if ( tag >= TMZ_TAG_FM_PULSES && tag <= TMZ_TAG_FM_COPIES ) return 5 + ( tag - TMZ_TAG_FM_PULSES );
    return -1;
}


/**
 * @brief Povinnost známého tagu podle tabulek spec (kap. 5.3, 6.1).
 *
 * U známého tagu rozhoduje tabulka, ne bit 15 v souboru: tag zapsaný se
 * špatným bitem a chybnou hodnotou nesmí tiše změnit význam části
 * (např. TIMEBASE by jinak spadl na výchozí hodnotu).
 *
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
            return ( tag >= TMZ_TAG_FM_PULSES && tag <= TMZ_TAG_FM_CONTEXT ) ? 1 : -1;
    }
}


en_TMZ_PART_ERROR tmz_part_fm_decode ( const uint8_t *tlv, uint32_t tlv_length, uint32_t default_timebase,
                                       st_MZCMT_FM_PARAMS *out, st_TMZ_PART_COMMON *common ) {
    mzcmt_fm_params_init ( out );
    memset ( common, 0, sizeof ( *common ) );
    out->timebase = default_timebase;
    int have_pulses = 0;
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
                /* DWORD délka, volitelně BYTE úroveň (0 LOW, 1 HIGH) */
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
            case TMZ_TAG_FM_PULSES:
                if ( ( len_ok = ( t.length == 16 ) ) ) {
                    out->short_first = tmz_rd32 ( v );
                    out->short_second = tmz_rd32 ( v + 4 );
                    out->long_first = tmz_rd32 ( v + 8 );
                    out->long_second = tmz_rd32 ( v + 12 );
                    have_pulses = 1;
                }
                break;
            case TMZ_TAG_FM_TONE:
                if ( ( len_ok = ( t.length == 4 ) ) ) out->tone = tmz_rd32 ( v );
                break;
            case TMZ_TAG_FM_TAPEMARK:
                if ( ( len_ok = ( t.length == 5 ) ) ) {
                    out->tm_long = tmz_rd16 ( v );
                    out->tm_short = tmz_rd16 ( v + 2 );
                    out->tm_end_long = v[4];
                }
                break;
            case TMZ_TAG_FM_FRAMING:
                if ( ( len_ok = ( t.length == 2 ) ) ) {
                    if ( v[1] > 1 ) return TMZ_PART_ERROR_BAD_VALUE;
                    out->start_bits = v[0];
                    out->lsb_first = v[1];
                }
                break;
            case TMZ_TAG_FM_CHECKSUM:
                if ( ( len_ok = ( t.length == 1 || t.length == 3 ) ) ) {
                    out->checksum = v[0];
                    if ( t.length == 3 ) {
                        out->checksum_explicit = 1;
                        out->checksum_bytes[0] = v[1];
                        out->checksum_bytes[1] = v[2];
                    }
                }
                break;
            case TMZ_TAG_FM_END:
                if ( ( len_ok = ( t.length == 3 ) ) ) {
                    out->end_long = v[0];
                    out->tail_short = tmz_rd16 ( v + 1 );
                }
                break;
            case TMZ_TAG_FM_COPIES:
                if ( ( len_ok = ( t.length == 3 ) ) ) {
                    out->copies = v[0];
                    out->copy_sep = tmz_rd16 ( v + 1 );
                }
                break;
            case TMZ_TAG_FM_CONTEXT:
                if ( ( len_ok = ( t.length % 11 == 0 ) ) ) {
                    for ( uint32_t o = 0; o < t.length; o += 11 ) {
                        if ( out->context_count >= MZCMT_FM_MAX_CONTEXT ) return TMZ_PART_ERROR_BAD_VALUE;
                        st_MZCMT_FM_CONTEXT *c = &out->context[out->context_count++];
                        c->ctx = v[o];
                        c->kind = v[o + 1];
                        c->next = v[o + 2];
                        c->first = tmz_rd32 ( v + o + 3 );
                        c->second = tmz_rd32 ( v + o + 7 );
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
    if ( !have_pulses ) return TMZ_PART_ERROR_MISSING_PULSES;
    if ( mzcmt_fm_params_validate ( out ) != EXIT_SUCCESS ) return TMZ_PART_ERROR_BAD_VALUE;
    return TMZ_PART_OK;
}


/**
 * @brief Připojí položku TLV.
 * @param b Buffer. @param tag Tag. @param critical Povinná. @param v Hodnota. @param len Délka.
 * @return EXIT_SUCCESS / EXIT_FAILURE.
 */
static int put ( st_TMZ_TLVBUF *b, uint16_t tag, int critical, const uint8_t *v, uint16_t len ) {
    return tmz_tlvbuf_add ( b, tag, critical, v, len );
}


int tmz_part_fm_encode ( const st_MZCMT_FM_PARAMS *p, const st_TMZ_PART_COMMON *common,
                         uint32_t default_timebase, st_TMZ_TLVBUF *buf ) {
    if ( !p || !buf || mzcmt_fm_params_validate ( p ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    uint8_t v[16];
    int r = EXIT_SUCCESS;
    if ( p->timebase != default_timebase ) {
        tmz_wr32 ( v, p->timebase );
        r |= put ( buf, TMZ_TAG_TIMEBASE, 1, v, 4 );
    }
    if ( p->level != 1 ) {
        v[0] = p->level;
        r |= put ( buf, TMZ_TAG_LEVEL, 1, v, 1 );
    }
    if ( p->pause ) {
        tmz_wr32 ( v, p->pause );
        v[4] = p->pause_level;
        r |= put ( buf, TMZ_TAG_PAUSE, 1, v, p->pause_level ? 5 : 4 );
    }
    if ( common && common->stop ) {
        v[0] = 1;
        r |= put ( buf, TMZ_TAG_STOP, 0, v, 1 );
    }
    if ( common && common->has_reader ) {
        v[0] = common->reader_routine;
        tmz_wr16 ( v + 1, common->reader_n );
        r |= put ( buf, TMZ_TAG_READER, 0, v, 3 );
    }
    tmz_wr32 ( v, p->short_first );
    tmz_wr32 ( v + 4, p->short_second );
    tmz_wr32 ( v + 8, p->long_first );
    tmz_wr32 ( v + 12, p->long_second );
    r |= put ( buf, TMZ_TAG_FM_PULSES, 1, v, 16 );
    if ( p->tone ) {
        tmz_wr32 ( v, p->tone );
        r |= put ( buf, TMZ_TAG_FM_TONE, 1, v, 4 );
    }
    if ( p->tm_long || p->tm_short || p->tm_end_long ) {
        tmz_wr16 ( v, p->tm_long );
        tmz_wr16 ( v + 2, p->tm_short );
        v[4] = p->tm_end_long;
        r |= put ( buf, TMZ_TAG_FM_TAPEMARK, 1, v, 5 );
    }
    if ( p->start_bits != 1 || p->lsb_first ) {
        v[0] = p->start_bits;
        v[1] = p->lsb_first;
        r |= put ( buf, TMZ_TAG_FM_FRAMING, 1, v, 2 );
    }
    if ( p->checksum ) {
        v[0] = p->checksum;
        v[1] = p->checksum_bytes[0];
        v[2] = p->checksum_bytes[1];
        r |= put ( buf, TMZ_TAG_FM_CHECKSUM, 1, v, p->checksum_explicit ? 3 : 1 );
    }
    if ( p->end_long || p->tail_short ) {
        v[0] = p->end_long;
        tmz_wr16 ( v + 1, p->tail_short );
        r |= put ( buf, TMZ_TAG_FM_END, 1, v, 3 );
    }
    if ( p->copies ) {
        v[0] = p->copies;
        tmz_wr16 ( v + 1, p->copy_sep );
        r |= put ( buf, TMZ_TAG_FM_COPIES, 1, v, 3 );
    }
    if ( p->context_count ) {
        uint8_t *c = malloc ( p->context_count * 11u );
        if ( !c ) return EXIT_FAILURE;
        for ( uint32_t i = 0; i < p->context_count; i++ ) {
            const st_MZCMT_FM_CONTEXT *e = &p->context[i];
            c[i * 11] = e->ctx;
            c[i * 11 + 1] = e->kind;
            c[i * 11 + 2] = e->next;
            tmz_wr32 ( c + i * 11 + 3, e->first );
            tmz_wr32 ( c + i * 11 + 7, e->second );
        }
        r |= put ( buf, TMZ_TAG_FM_CONTEXT, 1, c, ( uint16_t ) ( p->context_count * 11u ) );
        free ( c );
    }
    return r ? EXIT_FAILURE : EXIT_SUCCESS;
}


/** @brief Takt CPU MZ-800 pro model čtecí rutiny (T-stavy/s). */
#define READER_CPU_HZ 3546895.0


/**
 * @brief Délka úseku v T-stavech MZ-800, případně po převodu na vzorky.
 *
 * @param x Délka v jednotkách časové základny.
 * @param timebase Časová základna (Hz, nenulová).
 * @param rate Vzorkovací frekvence (Hz); 0 = bez převodu na vzorky.
 * @param up 1 = zaokrouhlit počet vzorků nahoru, 0 = dolů.
 * @return Délka v T-stavech.
 */
static double reader_len_t ( uint64_t x, uint32_t timebase, uint32_t rate, int up ) {
    if ( !rate ) return ( double ) x * READER_CPU_HZ / timebase;
    uint64_t num = x * rate;
    uint64_t samples = up ? ( num + timebase - 1 ) / timebase : num / timebase;
    return ( double ) samples * READER_CPU_HZ / rate;
}


en_TMZ_READER_CHECK tmz_part_fm_check_reader ( const st_MZCMT_FM_PARAMS *p, const st_TMZ_PART_COMMON *c,
                                               uint32_t rate, st_TMZ_READER_MARGINS *m ) {
    if ( !p || !c || !c->has_reader || !p->timebase ) return TMZ_READER_SKIPPED;
    unsigned n;
    double hook = 0;   /* zpoždění za náběžnou hranou navíc (rutina 2: háček zavaděče FAST IPL V07, disassembly Intercopy 10.2) */
    if ( c->reader_routine == 0 ) n = 82;
    else if ( c->reader_routine == 1 ) n = c->reader_n;
    else if ( c->reader_routine == 2 ) {
        n = c->reader_n;
        hook = 86.0;
    } else return TMZ_READER_SKIPPED;

    /*
     * Nejhorší případ přes základ a kontextové položky (jednotky časové
     * základny). Floor i ceil jsou monotónní, takže stačí zaokrouhlit
     * extrém.
     */
    uint64_t sh = p->short_first, lh = p->long_first, per = ( uint64_t ) p->short_first + p->short_second;
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        const st_MZCMT_FM_CONTEXT *e = &p->context[i];
        if ( e->kind == MZCMT_FM_KIND_SHORT ) {
            if ( e->first > sh ) sh = e->first;
            if ( ( uint64_t ) e->first + e->second < per ) per = ( uint64_t ) e->first + e->second;
        } else {
            if ( e->first < lh ) lh = e->first;
        }
    }
    double us = READER_CPU_HZ / 1e6;   /* T na µs */
    st_TMZ_READER_MARGINS r;
    r.short_high_us = ( ( 86.0 + hook + 14.0 * n ) - reader_len_t ( sh, p->timebase, rate, 1 ) ) / us;
    r.long_high_us = ( reader_len_t ( lh, p->timebase, rate, 0 ) - ( 138.0 + hook + 14.0 * n ) ) / us;
    r.short_period_us = ( reader_len_t ( per, p->timebase, rate, 0 ) - ( 256.0 + hook + 14.0 * n ) ) / us;
    if ( m ) *m = r;
    if ( r.short_high_us < 0 || r.long_high_us < 0 || r.short_period_us < 0 ) return TMZ_READER_UNREADABLE;
    if ( r.short_high_us < TMZ_READER_MIN_MARGIN_US || r.long_high_us < TMZ_READER_MIN_MARGIN_US
         || r.short_period_us < TMZ_READER_MIN_MARGIN_US ) return TMZ_READER_LOW_MARGIN;
    return TMZ_READER_OK;
}
