/**
 * @file   mzcmt_manchester.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.1.0
 * @brief  Implementace generátoru rodiny MANCHESTER (spec TMZ v1.1 kap. 7.2).
 *
 * Bity se vysílají postupně; ke každému bitu volající sekce určí kontext
 * jeho druhé půlky podle toho, co následuje (pilot, sync, flag, data,
 * checksum, koncové bity). Půlky se slučují do půlvln od hrany k hraně.
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
#include <stdlib.h>
#include <string.h>

#include "mzcmt_manchester.h"


void mzcmt_manch_params_init ( st_MZCMT_MANCH_PARAMS *p ) {
    memset ( p, 0, sizeof ( *p ) );
    p->timebase = MZCMT_MANCH_DEFAULT_TIMEBASE;
    p->level = 1;
}


/**
 * @brief Najde druhou půlku bitu podle kontextu: (ctx, bit), (ctx, ANY), jinak half2[bit].
 * @param p Parametry. @param ctx Kontext. @param bit Hodnota bitu (0/1).
 * @return Druhá půlka bez úpravy přenosu.
 */
static uint32_t lookup_half2 ( const st_MZCMT_MANCH_PARAMS *p, uint8_t ctx, int bit ) {
    const st_MZCMT_MANCH_CONTEXT *any = NULL;
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        const st_MZCMT_MANCH_CONTEXT *c = &p->context[i];
        if ( c->ctx != ctx ) continue;
        if ( c->bit == ( uint8_t ) bit ) return c->half2;
        if ( c->bit == MZCMT_MANCH_BIT_ANY ) any = c;
    }
    return any ? any->half2 : p->half2[bit];
}


int mzcmt_manch_params_validate ( const st_MZCMT_MANCH_PARAMS *p ) {
    if ( !p || !p->timebase || p->level > 1 || p->pause_level > 1 ) return EXIT_FAILURE;
    if ( !p->half1[0] || !p->half1[1] || !p->half2[0] || !p->half2[1] ) return EXIT_FAILURE;
    const uint64_t max_half = ( uint64_t ) MZCMT_MANCH_MAX_HALF_SECONDS * p->timebase;
    if ( p->half1[0] > max_half || p->half1[1] > max_half || p->half2[0] > max_half || p->half2[1] > max_half ) {
        return EXIT_FAILURE;
    }
    if ( p->pilot_count > MZCMT_MANCH_MAX_PILOT ) return EXIT_FAILURE;
    if ( p->pause > ( uint64_t ) MZCMT_MANCH_MAX_PART_SECONDS * p->timebase ) return EXIT_FAILURE;
    if ( p->pilot_count && ( p->pilot_bits < 1 || p->pilot_bits > MZCMT_MANCH_MAX_PATTERN_BITS ) ) return EXIT_FAILURE;
    if ( p->sync_bits > MZCMT_MANCH_MAX_PATTERN_BITS || p->end_bits > MZCMT_MANCH_MAX_PATTERN_BITS ) return EXIT_FAILURE;
    if ( p->has_flag > 1 || p->checksum > MZCMT_MANCH_CHECKSUM_SUM16_LE || p->checksum_explicit > 1 ) return EXIT_FAILURE;
    if ( p->checksum_explicit && p->checksum != MZCMT_MANCH_CHECKSUM_SUM16_LE ) return EXIT_FAILURE;
    if ( p->context_count > MZCMT_MANCH_MAX_CONTEXT ) return EXIT_FAILURE;
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        const st_MZCMT_MANCH_CONTEXT *c = &p->context[i];
        if ( c->ctx >= MZCMT_MANCH_CTX_COUNT || !c->half2 || c->half2 > max_half ) return EXIT_FAILURE;
        if ( c->bit > 1 && c->bit != MZCMT_MANCH_BIT_ANY ) return EXIT_FAILURE;
        for ( uint32_t j = 0; j < i; j++ ) {
            if ( p->context[j].ctx == c->ctx && p->context[j].bit == c->bit ) return EXIT_FAILURE;
        }
    }
    /* úprava přenosu nesmí dát nulovou ani zápornou půlku */
    static const uint8_t carry_ctx[2] = { MZCMT_MANCH_CTX_DATA_FIRST, MZCMT_MANCH_CTX_DATA_BYTE };
    for ( int k = 0; k < 2; k++ ) {
        for ( int b = 0; b < 2; b++ ) {
            if ( ( int64_t ) lookup_half2 ( p, carry_ctx[k], b ) + p->carry_adjust <= 0 ) return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


int mzcmt_manch_part_bounds ( const st_MZCMT_MANCH_PARAMS *p, uint32_t size, uint64_t *bits_out, uint64_t *units_out ) {
    if ( mzcmt_manch_params_validate ( p ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    uint64_t bits = ( uint64_t ) p->pilot_count * p->pilot_bits + p->sync_bits + ( p->has_flag ? 8u : 0u )
                    + ( uint64_t ) size * 8u + ( p->checksum == MZCMT_MANCH_CHECKSUM_SUM16_LE ? 16u : 0u ) + p->end_bits;
    if ( bits > MZCMT_MANCH_MAX_BITS ) return EXIT_FAILURE;
    uint64_t h1 = ( p->half1[0] > p->half1[1] ) ? p->half1[0] : p->half1[1];
    uint64_t h2 = ( p->half2[0] > p->half2[1] ) ? p->half2[0] : p->half2[1];
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        if ( p->context[i].half2 > h2 ) h2 = p->context[i].half2;
    }
    if ( p->carry_adjust > 0 ) h2 += ( uint64_t ) p->carry_adjust;
    /* bits <= 1,6e7, h1 + h2 < 2^34: součin se do 64 bitů vejde */
    uint64_t units = bits * ( h1 + h2 ) + p->pause;
    if ( units > ( uint64_t ) MZCMT_MANCH_MAX_PART_SECONDS * p->timebase ) return EXIT_FAILURE;
    if ( bits_out ) *bits_out = bits;
    if ( units_out ) *units_out = units;
    return EXIT_SUCCESS;
}


int mzcmt_manch_check_size ( const st_MZCMT_MANCH_PARAMS *p, uint32_t size ) {
    return mzcmt_manch_part_bounds ( p, size, NULL, NULL );
}


uint16_t mzcmt_manch_checksum ( const uint8_t *data, uint32_t size ) {
    uint16_t sum = 0;
    for ( uint32_t i = 0; i < size; i++ ) sum = ( uint16_t ) ( sum + data[i] );
    return sum;
}


/**
 * @brief Stav generátoru: rozpracovaná půlvlna a výstup.
 */
typedef struct st_GEN {
    const st_MZCMT_MANCH_PARAMS *p;   /**< parametry */
    mzcmt_manch_half_cb cb;           /**< výstup */
    void *user;                       /**< data výstupu */
    int cur_level;                    /**< úroveň rozpracované půlvlny */
    uint64_t cur_dur;                 /**< délka rozpracované půlvlny (0 = žádná) */
} st_GEN;


/**
 * @brief Přidá půlku; stejnou úroveň přičte k rozpracované půlvlně.
 * @param g Stav. @param level Úroveň. @param dur Délka.
 * @return EXIT_SUCCESS, nebo chyba z cb (také při půlvlně přes 32 bitů).
 */
static int emit ( st_GEN *g, int level, uint64_t dur ) {
    if ( g->cur_dur && g->cur_level == level ) {
        g->cur_dur += dur;
        return EXIT_SUCCESS;
    }
    if ( g->cur_dur ) {
        if ( g->cur_dur > UINT32_MAX ) return EXIT_FAILURE;
        if ( g->cb ( g->user, g->cur_level, ( uint32_t ) g->cur_dur ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    }
    g->cur_level = level;
    g->cur_dur = dur;
    return EXIT_SUCCESS;
}


/**
 * @brief Vyšle jeden bit.
 * @param g Stav. @param bit Hodnota (0/1). @param ctx Kontext druhé půlky. @param carry 1 = přičíst carry_adjust.
 * @return Výsledek emit().
 */
static int push_bit ( st_GEN *g, int bit, uint8_t ctx, int carry ) {
    const st_MZCMT_MANCH_PARAMS *p = g->p;
    int first = p->level ? bit : !bit;
    int64_t h2 = ( int64_t ) lookup_half2 ( p, ctx, bit ) + ( carry ? p->carry_adjust : 0 );
    if ( emit ( g, first, p->half1[bit] ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    return emit ( g, !first, ( uint64_t ) h2 );
}


/**
 * @brief Vyšle bity vzoru (sync, koncové bity).
 * @param g Stav. @param bits Počet. @param pattern Bity (bit 0 první). @param in Kontext uvnitř. @param last Kontext posledního.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE.
 */
static int push_pattern ( st_GEN *g, uint8_t bits, uint8_t pattern, uint8_t in, uint8_t last ) {
    for ( uint8_t i = 0; i < bits; i++ ) {
        if ( push_bit ( g, ( pattern >> i ) & 1, ( uint8_t ) ( i + 1 < bits ? in : last ), 0 ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Vyšle bajt LSB první; bit 7 dostane kontext last.
 * @param g Stav. @param v Bajt. @param last Kontext bitu 7. @param carry Úprava přenosu u bitu 7.
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE.
 */
static int push_byte ( st_GEN *g, uint8_t v, uint8_t last, int carry ) {
    for ( int i = 0; i < 8; i++ ) {
        int b = ( v >> i ) & 1;
        if ( push_bit ( g, b, ( uint8_t ) ( i < 7 ? MZCMT_MANCH_CTX_BIT : last ), i == 7 && carry ) != EXIT_SUCCESS ) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


int mzcmt_manch_generate ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size,
                           mzcmt_manch_half_cb cb, void *user ) {
    if ( !cb || mzcmt_manch_check_size ( p, size ) != EXIT_SUCCESS || ( size && !data ) ) return EXIT_FAILURE;
    st_GEN g = { p, cb, user, 0, 0 };

    /* pilot: vzor x počet */
    for ( uint32_t k = 0; k < p->pilot_count; k++ ) {
        for ( uint8_t i = 0; i < p->pilot_bits; i++ ) {
            uint8_t ctx = MZCMT_MANCH_CTX_PILOT_IN;
            if ( i + 1 == p->pilot_bits ) {
                ctx = ( k + 1 < p->pilot_count ) ? MZCMT_MANCH_CTX_PILOT_NEXT : MZCMT_MANCH_CTX_PILOT_END;
            }
            if ( push_bit ( &g, ( p->pilot_pattern >> i ) & 1, ctx, 0 ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        }
    }
    if ( push_pattern ( &g, p->sync_bits, p->sync_pattern, MZCMT_MANCH_CTX_SYNC_IN, MZCMT_MANCH_CTX_SYNC_END ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }
    if ( p->has_flag && push_byte ( &g, p->flag, MZCMT_MANCH_CTX_FLAG_END, 0 ) != EXIT_SUCCESS ) return EXIT_FAILURE;

    /* data: bit 7 podle pozice bajtu, přenos podle nižšího bajtu průběžného součtu */
    uint16_t sum = size ? data[0] : 0;
    for ( uint32_t k = 0; k < size; k++ ) {
        uint8_t ctx;
        int carry = 0;
        if ( size == 1 ) ctx = MZCMT_MANCH_CTX_DATA_SINGLE_END;
        else if ( k + 1 == size ) ctx = MZCMT_MANCH_CTX_DATA_END;
        else {
            ctx = k ? MZCMT_MANCH_CTX_DATA_BYTE : MZCMT_MANCH_CTX_DATA_FIRST;
            carry = ( ( sum & 0xFF ) + data[k + 1] ) > 0xFF;
        }
        if ( push_byte ( &g, data[k], ctx, carry ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        if ( k + 1 < size ) sum = ( uint16_t ) ( sum + data[k + 1] );
    }

    if ( p->checksum == MZCMT_MANCH_CHECKSUM_SUM16_LE ) {
        uint8_t ck[2];
        if ( p->checksum_explicit ) {
            ck[0] = p->checksum_bytes[0];
            ck[1] = p->checksum_bytes[1];
        } else {
            ck[0] = ( uint8_t ) ( sum & 0xFF );
            ck[1] = ( uint8_t ) ( sum >> 8 );
        }
        if ( push_byte ( &g, ck[0], MZCMT_MANCH_CTX_CK_MID, 0 ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        if ( push_byte ( &g, ck[1], MZCMT_MANCH_CTX_CK_END, 0 ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    }
    if ( push_pattern ( &g, p->end_bits, p->end_pattern, MZCMT_MANCH_CTX_END_IN, MZCMT_MANCH_CTX_END_LAST ) != EXIT_SUCCESS ) {
        return EXIT_FAILURE;
    }

    /* dokončit rozpracovanou půlvlnu, pak pauza jako samostatná půlvlna */
    if ( g.cur_dur ) {
        if ( g.cur_dur > UINT32_MAX || cb ( user, g.cur_level, ( uint32_t ) g.cur_dur ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    }
    if ( p->pause && cb ( user, p->pause_level, p->pause ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}


int mzcmt_manch_first_level ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size ) {
    int bit;
    if ( p->pilot_count && p->pilot_bits ) bit = p->pilot_pattern & 1;
    else if ( p->sync_bits ) bit = p->sync_pattern & 1;
    else if ( p->has_flag ) bit = p->flag & 1;
    else if ( size && data ) bit = data[0] & 1;
    else if ( p->checksum == MZCMT_MANCH_CHECKSUM_SUM16_LE ) bit = p->checksum_explicit ? ( p->checksum_bytes[0] & 1 ) : 0;
    else if ( p->end_bits ) bit = p->end_pattern & 1;
    else return p->pause_level ? 1 : 0;
    return p->level ? bit : !bit;
}


/**
 * @brief Stav převodu na vzorky (akumulovaný čas části).
 */
typedef struct st_VSOUT {
    st_CMT_VSTREAM *vs;   /**< cílový vstream */
    uint32_t rate;        /**< vzorkovací frekvence */
    uint32_t timebase;    /**< časová základna části */
    uint64_t cum;         /**< akumulovaný čas v jednotkách */
    uint64_t emitted;     /**< již zapsané vzorky od začátku části */
} st_VSOUT;


/**
 * @brief Výstup generátoru: zapíše půlvlnu jako vzorky podle akumulovaného času.
 * @param user st_VSOUT. @param level Úroveň. @param duration Délka (jednotky timebase).
 * @return EXIT_SUCCESS, nebo EXIT_FAILURE (půlvlna bez vzorku, chyba vstreamu).
 */
static int vs_out ( void *user, int level, uint32_t duration ) {
    st_VSOUT *o = user;
    o->cum += duration;
    /* cum x rate by při vysoké časové základně přetekl 64 bitů: převod dělením */
    uint64_t target = cmt_vstream_scale_time ( o->cum, ( uint32_t ) o->timebase, ( uint32_t ) o->rate );
    uint64_t n = ( target > o->emitted && target != UINT64_MAX ) ? target - o->emitted : 0;
    if ( n == 0 || n > UINT32_MAX ) {
        fprintf ( stderr, "%s():%d: Error: half-wave of %u units does not fit the sample rate.\n", __func__, __LINE__, duration );
        return EXIT_FAILURE;
    }
    o->emitted = target;
    return cmt_vstream_add_value ( o->vs, level, ( uint32_t ) n );
}


int mzcmt_manch_append_vstream ( st_CMT_VSTREAM *vs, const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size ) {
    if ( !vs || !p ) return EXIT_FAILURE;
    st_VSOUT o = { vs, cmt_vstream_get_rate ( vs ), p->timebase, 0, 0 };
    if ( !o.rate || !o.timebase ) return EXIT_FAILURE;
    /* pauza se generuje zvlášť: když nedá ani jeden vzorek, vynechá se (jako mzcmt_fm) */
    st_MZCMT_MANCH_PARAMS *q = malloc ( sizeof ( *q ) );
    if ( !q ) return EXIT_FAILURE;
    *q = *p;
    q->pause = 0;
    int r = mzcmt_manch_generate ( q, data, size, vs_out, &o );
    free ( q );
    if ( r != EXIT_SUCCESS || !p->pause ) return r;
    o.cum += p->pause;
    uint64_t target = cmt_vstream_scale_time ( o.cum, ( uint32_t ) o.timebase, ( uint32_t ) o.rate );
    if ( target == UINT64_MAX ) return EXIT_FAILURE;
    /* akumulovaný čas neklesá, target tedy také; pojistka proti podtečení */
    uint64_t n = ( target > o.emitted ) ? target - o.emitted : 0;
    if ( n == 0 ) return EXIT_SUCCESS;
    o.emitted = target;
    /* pauza smí být delší než jeden event vstreamu (2^32 vzorků) */
    return cmt_vstream_add_value_long ( vs, p->pause_level, n );
}


st_CMT_VSTREAM* mzcmt_manch_create_vstream ( const st_MZCMT_MANCH_PARAMS *p, const uint8_t *data, uint32_t size, uint32_t rate ) {
    if ( !p || mzcmt_manch_params_validate ( p ) != EXIT_SUCCESS ) return NULL;
    st_CMT_VSTREAM *vs = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, mzcmt_manch_first_level ( p, data, size ),
                                           CMT_STREAM_POLARITY_NORMAL );
    if ( !vs ) return NULL;
    if ( mzcmt_manch_append_vstream ( vs, p, data, size ) != EXIT_SUCCESS ) {
        cmt_vstream_destroy ( vs );
        return NULL;
    }
    return vs;
}


const char* mzcmt_manch_version ( void ) {
    return MZCMT_MANCH_VERSION;
}
