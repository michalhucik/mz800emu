/**
 * @file   mzcmt_fm.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 1.2.0
 * @brief  Implementace obecného generátoru FM (viz mzcmt_fm.h).
 *
 * Pulz se vypouští až ve chvíli, kdy je známý druh následujícího pulzu
 * (kontextové délky na něm mohou záviset), proto generátor drží jeden
 * čekající pulz. Délky pro všechny kombinace (kontext, druh, další druh)
 * se předpočítají jednou na začátku.
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
#include <stdlib.h>
#include <string.h>

#include "mzcmt_fm.h"


/** @brief Rodič kontextu (spec 6.3); MZCMT_FM_CTX_COUNT = bez rodiče. */
static const uint8_t g_parent[MZCMT_FM_CTX_COUNT] = {
    [MZCMT_FM_CTX_TONE] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_TONE_LAST] = MZCMT_FM_CTX_TONE,
    [MZCMT_FM_CTX_TM_L] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_TM_L_LAST] = MZCMT_FM_CTX_TM_L,
    [MZCMT_FM_CTX_TM_S] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_TM_S_LAST] = MZCMT_FM_CTX_TM_S,
    [MZCMT_FM_CTX_TM_END_L] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_START] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_BIT] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_BIT_LAST] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_BIT_LAST_DATA_END] = MZCMT_FM_CTX_BIT_LAST,
    [MZCMT_FM_CTX_CK_START] = MZCMT_FM_CTX_START,
    [MZCMT_FM_CTX_CK_BIT] = MZCMT_FM_CTX_BIT,
    [MZCMT_FM_CTX_CK_BIT_LAST] = MZCMT_FM_CTX_BIT_LAST,
    [MZCMT_FM_CTX_CK_BIT_END] = MZCMT_FM_CTX_CK_BIT_LAST,
    [MZCMT_FM_CTX_END_L] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_TAIL] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_TAIL_LAST] = MZCMT_FM_CTX_TAIL,
    [MZCMT_FM_CTX_COPY_SEP] = MZCMT_FM_CTX_COUNT,
    [MZCMT_FM_CTX_COPY_SEP_LAST] = MZCMT_FM_CTX_COPY_SEP,
};

/** @brief Index "dalšího druhu" v tabulce délek: 0 = S, 1 = L, 2 = žádný (konec části). */
#define NEXT_NONE 2


/**
 * @brief Stav generátoru: předpočítané délky a pulz čekající na znalost dalšího druhu.
 *
 * @invariant have_pending == 0, nebo pending_ctx < MZCMT_FM_CTX_COUNT a pending_kind <= 1
 */
typedef struct st_GEN {
    uint32_t table[MZCMT_FM_CTX_COUNT][2][3][2];  /**< [kontext][druh][další][půlvlna] */
    int have_pending;                             /**< čeká pulz na výstup */
    uint8_t pending_ctx;                          /**< kontext čekajícího pulzu */
    uint8_t pending_kind;                         /**< druh čekajícího pulzu */
    int level;                                    /**< úroveň první půlvlny */
    mzcmt_fm_half_cb cb;                          /**< výstup */
    void *user;                                   /**< data výstupu */
} st_GEN;


void mzcmt_fm_params_init ( st_MZCMT_FM_PARAMS *p ) {
    memset ( p, 0, sizeof ( *p ) );
    p->timebase = MZCMT_FM_DEFAULT_TIMEBASE;
    p->level = 1;
    p->start_bits = 1;
}


int mzcmt_fm_params_validate ( const st_MZCMT_FM_PARAMS *p ) {
    if ( !p || !p->timebase ) return EXIT_FAILURE;
    if ( !p->short_first || !p->short_second || !p->long_first || !p->long_second ) return EXIT_FAILURE;
    const uint64_t max_half = ( uint64_t ) MZCMT_FM_MAX_HALF_SECONDS * p->timebase;
    if ( p->short_first > max_half || p->short_second > max_half || p->long_first > max_half
         || p->long_second > max_half ) return EXIT_FAILURE;
    if ( p->tone > MZCMT_FM_MAX_TONE ) return EXIT_FAILURE;
    if ( p->pause > ( uint64_t ) MZCMT_FM_MAX_PART_SECONDS * p->timebase ) return EXIT_FAILURE;
    if ( p->level > 1 || p->lsb_first > 1 || p->pause_level > 1 ) return EXIT_FAILURE;
    if ( p->checksum > MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE ) return EXIT_FAILURE;
    if ( p->checksum_explicit && p->checksum != MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE ) return EXIT_FAILURE;
    if ( p->context_count > MZCMT_FM_MAX_CONTEXT ) return EXIT_FAILURE;
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        const st_MZCMT_FM_CONTEXT *c = &p->context[i];
        if ( c->ctx >= MZCMT_FM_CTX_COUNT || c->kind > 1 ) return EXIT_FAILURE;
        if ( c->next > 1 && c->next != MZCMT_FM_NEXT_ANY ) return EXIT_FAILURE;
        if ( !c->first || !c->second || c->first > max_half || c->second > max_half ) return EXIT_FAILURE;
        for ( uint32_t j = 0; j < i; j++ ) {
            const st_MZCMT_FM_CONTEXT *d = &p->context[j];
            if ( d->ctx == c->ctx && d->kind == c->kind && d->next == c->next ) return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


int mzcmt_fm_part_bounds ( const st_MZCMT_FM_PARAMS *p, uint32_t size, uint64_t *pulses_out, uint64_t *units_out ) {
    if ( mzcmt_fm_params_validate ( p ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    uint64_t ck_len = ( p->checksum == MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE ) ? 2 : 0;
    uint64_t section = ( ( uint64_t ) size + ck_len ) * ( 8u + p->start_bits ) + p->end_long + p->tail_short;
    uint64_t pulses = ( uint64_t ) p->tone + p->tm_long + p->tm_short + p->tm_end_long
                      + ( 1u + ( uint64_t ) p->copies ) * section + ( uint64_t ) p->copies * p->copy_sep;
    if ( pulses > MZCMT_FM_MAX_PULSES ) return EXIT_FAILURE;
    uint64_t pair = ( uint64_t ) p->short_first + p->short_second;
    if ( ( uint64_t ) p->long_first + p->long_second > pair ) pair = ( uint64_t ) p->long_first + p->long_second;
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        /* kontext mění jen jednu dvojici; horní odhad: obě půlvlny nejdelší z kontextu */
        uint64_t c = 2 * ( uint64_t ) ( ( p->context[i].first > p->context[i].second ) ? p->context[i].first : p->context[i].second );
        if ( c > pair ) pair = c;
    }
    /* pulses <= 1,6e7, pair <= 2 s x timebase (< 2^33): součin se do 64 bitů vejde */
    uint64_t units = pulses * pair + p->pause;
    if ( units > ( uint64_t ) MZCMT_FM_MAX_PART_SECONDS * p->timebase ) return EXIT_FAILURE;
    if ( pulses_out ) *pulses_out = pulses;
    if ( units_out ) *units_out = units;
    return EXIT_SUCCESS;
}


int mzcmt_fm_check_size ( const st_MZCMT_FM_PARAMS *p, uint32_t size ) {
    return mzcmt_fm_part_bounds ( p, size, NULL, NULL );
}


/**
 * @brief Najde položku kontextu.
 * @param p Parametry. @param ctx Kontext. @param kind Druh. @param next Další druh nebo MZCMT_FM_NEXT_ANY.
 * @return Položka, nebo NULL.
 */
static const st_MZCMT_FM_CONTEXT* find_entry ( const st_MZCMT_FM_PARAMS *p, uint8_t ctx, uint8_t kind, uint8_t next ) {
    for ( uint32_t i = 0; i < p->context_count; i++ ) {
        const st_MZCMT_FM_CONTEXT *c = &p->context[i];
        if ( c->ctx == ctx && c->kind == kind && c->next == next ) return c;
    }
    return NULL;
}


/**
 * @brief Předpočítá tabulku délek podle vyhodnocení spec 6.3.
 * @param g Stav generátoru. @param p Platné parametry.
 */
static void build_table ( st_GEN *g, const st_MZCMT_FM_PARAMS *p ) {
    for ( uint8_t ctx = 0; ctx < MZCMT_FM_CTX_COUNT; ctx++ ) {
        for ( uint8_t kind = 0; kind < 2; kind++ ) {
            for ( uint8_t nx = 0; nx < 3; nx++ ) {
                const st_MZCMT_FM_CONTEXT *e = NULL;
                for ( uint8_t c = ctx; c < MZCMT_FM_CTX_COUNT && !e; c = g_parent[c] ) {
                    if ( nx != NEXT_NONE ) e = find_entry ( p, c, kind, nx );
                    if ( !e ) e = find_entry ( p, c, kind, MZCMT_FM_NEXT_ANY );
                }
                g->table[ctx][kind][nx][0] = e ? e->first : ( kind ? p->long_first : p->short_first );
                g->table[ctx][kind][nx][1] = e ? e->second : ( kind ? p->long_second : p->short_second );
            }
        }
    }
}


/**
 * @brief Vypustí čekající pulz.
 * @param g Stav. @param next Druh následujícího pulzu (0, 1) nebo NEXT_NONE.
 * @return EXIT_SUCCESS, nebo chyba z výstupu.
 */
static int flush ( st_GEN *g, uint8_t next ) {
    if ( !g->have_pending ) return EXIT_SUCCESS;
    g->have_pending = 0;
    const uint32_t *d = g->table[g->pending_ctx][g->pending_kind][next];
    if ( g->cb ( g->user, g->level, d[0] ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    return g->cb ( g->user, !g->level, d[1] );
}


/**
 * @brief Přidá pulz; předchozí se vypustí, protože teď známe jeho další druh.
 * @param g Stav. @param ctx Kontext. @param kind Druh.
 * @return EXIT_SUCCESS, nebo chyba z výstupu.
 */
static int push ( st_GEN *g, uint8_t ctx, uint8_t kind ) {
    if ( flush ( g, kind ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    g->have_pending = 1;
    g->pending_ctx = ctx;
    g->pending_kind = kind;
    return EXIT_SUCCESS;
}


/**
 * @brief n pulzů téhož druhu; poslední dostane kontext last.
 * @param g Stav. @param ctx Kontext. @param last Kontext posledního (MZCMT_FM_CTX_COUNT = stejný).
 * @param kind Druh. @param n Počet.
 * @return EXIT_SUCCESS, nebo chyba z výstupu.
 */
static int push_run ( st_GEN *g, uint8_t ctx, uint8_t last, uint8_t kind, uint32_t n ) {
    for ( uint32_t i = 0; i < n; i++ ) {
        uint8_t c = ( i + 1 == n && last < MZCMT_FM_CTX_COUNT ) ? last : ctx;
        if ( push ( g, c, kind ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Bajty jedné posloupnosti (data nebo checksum) se start bity.
 * @param g Stav. @param p Parametry. @param b Bajty. @param n Počet.
 * @param c_start Kontext start bitu. @param c_bit Kontext pozic 0-6.
 * @param c_last Kontext pozice 7. @param c_final Kontext pozice 7 posledního bajtu.
 * @return EXIT_SUCCESS, nebo chyba z výstupu.
 */
static int push_bytes ( st_GEN *g, const st_MZCMT_FM_PARAMS *p, const uint8_t *b, uint32_t n,
                        uint8_t c_start, uint8_t c_bit, uint8_t c_last, uint8_t c_final ) {
    for ( uint32_t i = 0; i < n; i++ ) {
        for ( uint8_t s = 0; s < p->start_bits; s++ ) {
            if ( push ( g, c_start, MZCMT_FM_KIND_LONG ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        }
        for ( uint8_t q = 0; q < 8; q++ ) {
            uint8_t bit = p->lsb_first ? ( ( b[i] >> q ) & 1 ) : ( ( b[i] >> ( 7 - q ) ) & 1 );
            uint8_t c = ( q < 7 ) ? c_bit : ( ( i + 1 == n ) ? c_final : c_last );
            if ( push ( g, c, bit ) != EXIT_SUCCESS ) return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}


/**
 * @brief Datová sekce (spec 6.2 bod 3): data, checksum, END L, TAIL S.
 * @param g Stav. @param p Parametry. @param data Bajty. @param size Počet.
 * @param ck Bajty checksumu. @param ck_len Jejich počet.
 * @return EXIT_SUCCESS, nebo chyba z výstupu.
 */
static int push_section ( st_GEN *g, const st_MZCMT_FM_PARAMS *p, const uint8_t *data, uint32_t size,
                          const uint8_t *ck, uint32_t ck_len ) {
    if ( push_bytes ( g, p, data, size, MZCMT_FM_CTX_START, MZCMT_FM_CTX_BIT,
                      MZCMT_FM_CTX_BIT_LAST, MZCMT_FM_CTX_BIT_LAST_DATA_END ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    if ( push_bytes ( g, p, ck, ck_len, MZCMT_FM_CTX_CK_START, MZCMT_FM_CTX_CK_BIT,
                      MZCMT_FM_CTX_CK_BIT_LAST, MZCMT_FM_CTX_CK_BIT_END ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    if ( push_run ( g, MZCMT_FM_CTX_END_L, MZCMT_FM_CTX_COUNT, MZCMT_FM_KIND_LONG, p->end_long ) != EXIT_SUCCESS ) return EXIT_FAILURE;
    return push_run ( g, MZCMT_FM_CTX_TAIL, MZCMT_FM_CTX_TAIL_LAST, MZCMT_FM_KIND_SHORT, p->tail_short );
}


int mzcmt_fm_generate ( const st_MZCMT_FM_PARAMS *p, const uint8_t *data, uint32_t size,
                        mzcmt_fm_half_cb cb, void *user ) {
    if ( !cb || ( size && !data ) || mzcmt_fm_check_size ( p, size ) != EXIT_SUCCESS ) return EXIT_FAILURE;

    st_GEN *g = calloc ( 1, sizeof ( st_GEN ) );
    if ( !g ) return EXIT_FAILURE;
    build_table ( g, p );
    g->level = p->level;
    g->cb = cb;
    g->user = user;

    uint8_t ck[2];
    uint32_t ck_len = 0;
    if ( p->checksum == MZCMT_FM_CHECKSUM_MZ_POPCOUNT16_BE ) {
        if ( p->checksum_explicit ) {
            ck[0] = p->checksum_bytes[0];
            ck[1] = p->checksum_bytes[1];
        } else {
            uint32_t ones = 0;
            for ( uint32_t i = 0; i < size; i++ ) {
                for ( uint8_t v = data[i]; v; v &= ( uint8_t ) ( v - 1 ) ) ones++;
            }
            ck[0] = ( uint8_t ) ( ones >> 8 );
            ck[1] = ( uint8_t ) ones;
        }
        ck_len = 2;
    }

    int r = push_run ( g, MZCMT_FM_CTX_TONE, MZCMT_FM_CTX_TONE_LAST, MZCMT_FM_KIND_SHORT, p->tone );
    if ( r == EXIT_SUCCESS ) r = push_run ( g, MZCMT_FM_CTX_TM_L, MZCMT_FM_CTX_TM_L_LAST, MZCMT_FM_KIND_LONG, p->tm_long );
    if ( r == EXIT_SUCCESS ) r = push_run ( g, MZCMT_FM_CTX_TM_S, MZCMT_FM_CTX_TM_S_LAST, MZCMT_FM_KIND_SHORT, p->tm_short );
    if ( r == EXIT_SUCCESS ) r = push_run ( g, MZCMT_FM_CTX_TM_END_L, MZCMT_FM_CTX_COUNT, MZCMT_FM_KIND_LONG, p->tm_end_long );
    if ( r == EXIT_SUCCESS ) r = push_section ( g, p, data, size, ck, ck_len );
    for ( uint8_t c = 0; r == EXIT_SUCCESS && c < p->copies; c++ ) {
        r = push_run ( g, MZCMT_FM_CTX_COPY_SEP, MZCMT_FM_CTX_COPY_SEP_LAST, MZCMT_FM_KIND_SHORT, p->copy_sep );
        if ( r == EXIT_SUCCESS ) r = push_section ( g, p, data, size, ck, ck_len );
    }
    if ( r == EXIT_SUCCESS ) r = flush ( g, NEXT_NONE );
    if ( r == EXIT_SUCCESS && p->pause ) r = cb ( user, p->pause_level, p->pause );
    free ( g );
    return r;
}


/**
 * @brief Stav převodu půlvln na vzorky (akumulovaný čas od začátku části).
 */
typedef struct st_VSOUT {
    st_CMT_VSTREAM *vs;   /**< cílový vstream */
    uint64_t rate;        /**< vzorkovací frekvence */
    uint64_t timebase;    /**< časová základna části */
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


int mzcmt_fm_append_vstream ( st_CMT_VSTREAM *vs, const st_MZCMT_FM_PARAMS *p, const uint8_t *data, uint32_t size ) {
    if ( !vs || !p ) return EXIT_FAILURE;
    st_VSOUT o = { vs, cmt_vstream_get_rate ( vs ), p->timebase, 0, 0 };
    if ( !o.rate || !o.timebase ) return EXIT_FAILURE;
    /*
     * Pauza je poslední půlvlna. Generuje se zvlášť: když po zaokrouhlení
     * akumulovaného času nedá ani jeden vzorek, vynechá se místo
     * chyby "half-wave does not fit". Ostatní půlvlny se tím nemění.
     */
    st_MZCMT_FM_PARAMS *q = malloc ( sizeof ( *q ) );
    if ( !q ) return EXIT_FAILURE;
    *q = *p;
    q->pause = 0;
    int r = mzcmt_fm_generate ( q, data, size, vs_out, &o );
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


st_CMT_VSTREAM* mzcmt_fm_create_vstream ( const st_MZCMT_FM_PARAMS *p, const uint8_t *data, uint32_t size, uint32_t rate ) {
    if ( !p ) return NULL;
    st_CMT_VSTREAM *vs = cmt_vstream_new ( rate, CMT_VSTREAM_BYTELENGTH8, p->level ? 1 : 0, CMT_STREAM_POLARITY_NORMAL );
    if ( !vs ) return NULL;
    if ( mzcmt_fm_append_vstream ( vs, p, data, size ) != EXIT_SUCCESS ) {
        cmt_vstream_destroy ( vs );
        return NULL;
    }
    return vs;
}


const char* mzcmt_fm_version ( void ) {
    return MZCMT_FM_VERSION;
}
