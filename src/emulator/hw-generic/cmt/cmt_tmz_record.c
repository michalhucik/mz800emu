/**
 * @file   cmt_tmz_record.c
 * @brief  Buffer hran záznamu Virtual CMT (implementace cmt_tmz_record.h).
 *
 * Dvě části: buffer hran (stav je jedna statická struktura modulu; z
 * globálního stavu emulátoru čte jen úroveň PC1 v cmttmz_rec_begin()
 * a časovou základnu GDGCLK_BASE pro mez délky záznamu; pole délek úseků se
 * alokuje realloc() a uvolňuje free(), bez GLib, aby selhání alokace nebylo
 * fatální) a dekódování cmttmz_rec_decode() (čistá funkce nad obsahem
 * bufferu: pulzy, wav_analyzer_analyze_pulses(), plán částí, stavba bloků
 * 0x46 a 0x18; globální stav nečte). Data bloků alokuje výchozí alokátor
 * knihoven tzx / tmz (malloc), uvolňují se free().
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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/pio8255/pio8255.h"
#include "libs/wav_analyzer/wav_analyzer.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

#include "cmt_tmz_record.h"


/**
 * @brief Stav bufferu hran.
 *
 * Invarianty: rec.count <= capacity <= účinná mez položek (kromě capacity
 * ze dřívější větší meze v testech); bez aktivního záznamu je rec.count 0
 * a rec.ticks NULL; have_edge == 0 implikuje rec.count == 0; last_level je
 * úroveň po poslední přijaté hraně (bez hrany úroveň z begin).
 *
 * Vlastnictví: rec.ticks alokuje a uvolňuje jen tento modul.
 */
typedef struct st_CMTTMZ_REC_STATE {
    st_CMTTMZ_REC rec;   /**< obsah vracený cmttmz_rec_get() */
    uint32_t capacity;   /**< alokovaná kapacita rec.ticks v položkách */
    int active;          /**< 1 = záznam běží (po begin, před discard) */
    int limit_reached;   /**< 1 = mez dosažena, další hrany se zahazují */
    int have_edge;       /**< 1 = už byla přijata aspoň jedna hrana */
    int last_level;      /**< úroveň PC1 po poslední přijaté hraně (0 / 1) */
    uint64_t last_time;  /**< čas poslední přijaté hrany (takty GDGCLK_BASE) */
#ifdef MZTEST_HEADLESS
    uint32_t test_max_edges; /**< testovací mez položek, 0 = výchozí */
    uint64_t test_max_ticks; /**< testovací mez času hrany, 0 = výchozí */
#endif
} st_CMTTMZ_REC_STATE;


/** @brief Jediná instance stavu (vlákno emulátoru). */
static st_CMTTMZ_REC_STATE g_cmttmz_rec;


/**
 * @brief Účinná mez počtu položek bufferu.
 * @return CMTTMZ_REC_MAX_EDGES, v testech případně menší hodnota z setteru.
 */
static uint32_t cmttmz_rec_max_edges ( void ) {
#ifdef MZTEST_HEADLESS
    if ( g_cmttmz_rec.test_max_edges ) return g_cmttmz_rec.test_max_edges;
#endif
    return CMTTMZ_REC_MAX_EDGES;
}


/**
 * @brief Účinná mez času hrany od začátku záznamu.
 * @return CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE taktů, v testech případně hodnota z setteru.
 */
static uint64_t cmttmz_rec_max_ticks ( void ) {
#ifdef MZTEST_HEADLESS
    if ( g_cmttmz_rec.test_max_ticks ) return g_cmttmz_rec.test_max_ticks;
#endif
    return ( uint64_t ) CMTTMZ_REC_MAX_SECONDS * ( uint64_t ) GDGCLK_BASE;
}


/**
 * @brief Zajistí kapacitu bufferu aspoň need položek.
 *
 * Kapacita roste geometricky: první blok CMTTMZ_REC_INITIAL_CAPACITY,
 * pak dvojnásobek, nejvýš účinná mez položek.
 *
 * @param need Požadovaný počet položek (need <= účinná mez, kontroluje volající).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání realloc() (buffer zůstane
 *         původní a platný).
 */
static int cmttmz_rec_reserve ( uint32_t need ) {
    if ( need <= g_cmttmz_rec.capacity ) return EXIT_SUCCESS;
    uint64_t limit = cmttmz_rec_max_edges ( );
    uint64_t cap = g_cmttmz_rec.capacity ? ( uint64_t ) g_cmttmz_rec.capacity * 2u : CMTTMZ_REC_INITIAL_CAPACITY;
    while ( cap < need ) cap *= 2u;
    if ( cap > limit ) cap = limit;
    if ( cap < need ) cap = need;
    uint32_t *p = realloc ( g_cmttmz_rec.rec.ticks, ( size_t ) cap * sizeof ( uint32_t ) );
    if ( !p ) return EXIT_FAILURE;
    g_cmttmz_rec.rec.ticks = p;
    g_cmttmz_rec.capacity = ( uint32_t ) cap;
    return EXIT_SUCCESS;
}


void cmttmz_rec_discard ( void ) {
    free ( g_cmttmz_rec.rec.ticks );
    g_cmttmz_rec.rec.ticks = NULL;
    g_cmttmz_rec.rec.count = 0;
    g_cmttmz_rec.rec.first_level = 0;
    g_cmttmz_rec.rec.total_ticks = 0;
    g_cmttmz_rec.capacity = 0;
    g_cmttmz_rec.active = 0;
    g_cmttmz_rec.limit_reached = 0;
    g_cmttmz_rec.have_edge = 0;
    g_cmttmz_rec.last_level = 0;
    g_cmttmz_rec.last_time = 0;
}


void cmttmz_rec_begin ( void ) {
    cmttmz_rec_discard ( );
    int level = pio8255_pc1_get ( ) & 1;
    g_cmttmz_rec.rec.first_level = level;
    g_cmttmz_rec.last_level = level;
    g_cmttmz_rec.active = 1;
}


void cmttmz_rec_edge ( uint64_t play_ticks, int level_after ) {
    if ( ( !g_cmttmz_rec.active ) || g_cmttmz_rec.limit_reached ) return;

    int level = level_after & 1;
    if ( level == g_cmttmz_rec.last_level ) return;
    /* nerostoucí čas: úsek délky 0 nevznikne a úroveň zůstane */
    if ( g_cmttmz_rec.have_edge && ( play_ticks <= g_cmttmz_rec.last_time ) ) return;

    if ( play_ticks > cmttmz_rec_max_ticks ( ) ) {
        g_cmttmz_rec.limit_reached = 1;
        return;
    }

    if ( !g_cmttmz_rec.have_edge ) {
        /* první hrana jen zahájí první úsek */
        g_cmttmz_rec.have_edge = 1;
        g_cmttmz_rec.last_time = play_ticks;
        g_cmttmz_rec.last_level = level;
        g_cmttmz_rec.rec.first_level = level;
        return;
    }

    /* úsek delší než UINT32_MAX: q oddělovačů (UINT32_MAX + úsek 0 opačné úrovně), zbytek 1..UINT32_MAX */
    uint64_t len = play_ticks - g_cmttmz_rec.last_time;
    uint64_t q = ( len - 1u ) / UINT32_MAX;
    uint64_t entries = 2u * q + 1u;
    uint64_t need = ( uint64_t ) g_cmttmz_rec.rec.count + entries;
    if ( need > cmttmz_rec_max_edges ( ) ) {
        g_cmttmz_rec.limit_reached = 1;
        return;
    }
    if ( EXIT_SUCCESS != cmttmz_rec_reserve ( ( uint32_t ) need ) ) {
        fprintf ( stderr, "Virtual CMT: out of memory for the recording buffer (%u segments), further signal is discarded\n",
                  ( unsigned ) g_cmttmz_rec.rec.count );
        g_cmttmz_rec.limit_reached = 1;
        return;
    }

    uint32_t *t = g_cmttmz_rec.rec.ticks + g_cmttmz_rec.rec.count;
    for ( uint64_t i = 0; i < q; i++ ) {
        *t++ = UINT32_MAX;
        *t++ = 0;
    }
    *t = ( uint32_t ) ( len - q * UINT32_MAX );
    g_cmttmz_rec.rec.count = ( uint32_t ) need;
    g_cmttmz_rec.rec.total_ticks += len;
    g_cmttmz_rec.last_time = play_ticks;
    g_cmttmz_rec.last_level = level;
}


int cmttmz_rec_last_edge ( uint64_t *play_ticks ) {
    int have = g_cmttmz_rec.active && g_cmttmz_rec.have_edge;
    if ( play_ticks ) *play_ticks = have ? g_cmttmz_rec.last_time : 0;
    return have;
}


int cmttmz_rec_limit_reached ( void ) {
    return g_cmttmz_rec.limit_reached;
}


int cmttmz_rec_check_time ( uint64_t play_ticks ) {
    if ( !g_cmttmz_rec.active ) return 0;
    if ( play_ticks > cmttmz_rec_max_ticks ( ) ) g_cmttmz_rec.limit_reached = 1;
    return g_cmttmz_rec.limit_reached;
}


int cmttmz_rec_is_active ( void ) {
    return g_cmttmz_rec.active;
}


const st_CMTTMZ_REC* cmttmz_rec_get ( void ) {
    return &g_cmttmz_rec.rec;
}


/** @brief Kopie stavu záznamu (st_CMTTMZ_REC_COPY): stav modulu s vlastním polem úseků. */
struct st_CMTTMZ_REC_COPY {
    st_CMTTMZ_REC_STATE st; /**< stav; st.rec.ticks je vlastní pole kopie (count položek), capacity = count */
};


st_CMTTMZ_REC_COPY* cmttmz_rec_copy_take ( size_t max_bytes, int *too_big ) {
    if ( too_big ) *too_big = 0;
    if ( !g_cmttmz_rec.active ) return NULL;
    size_t bytes = ( size_t ) g_cmttmz_rec.rec.count * sizeof ( uint32_t );
    if ( bytes > max_bytes ) {
        if ( too_big ) *too_big = 1;
        return NULL;
    }
    st_CMTTMZ_REC_COPY *c = ( st_CMTTMZ_REC_COPY* ) calloc ( 1, sizeof ( *c ) );
    if ( !c ) return NULL;
    c->st = g_cmttmz_rec;
    c->st.rec.ticks = NULL;
    c->st.capacity = 0;
    if ( g_cmttmz_rec.rec.count ) {
        c->st.rec.ticks = ( uint32_t* ) malloc ( bytes );
        if ( !c->st.rec.ticks ) {
            free ( c );
            return NULL;
        }
        memcpy ( c->st.rec.ticks, g_cmttmz_rec.rec.ticks, bytes );
        c->st.capacity = g_cmttmz_rec.rec.count;
    }
    return c;
}


int cmttmz_rec_copy_install ( const st_CMTTMZ_REC_COPY *copy ) {
    cmttmz_rec_discard ( );
    if ( !copy ) return EXIT_FAILURE;
    st_CMTTMZ_REC_STATE st = copy->st;
    st.rec.ticks = NULL;
    st.capacity = 0;
    if ( copy->st.rec.count ) {
        size_t bytes = ( size_t ) copy->st.rec.count * sizeof ( uint32_t );
        st.rec.ticks = ( uint32_t* ) malloc ( bytes );
        if ( !st.rec.ticks ) return EXIT_FAILURE;
        memcpy ( st.rec.ticks, copy->st.rec.ticks, bytes );
        st.capacity = copy->st.rec.count;
    }
#ifdef MZTEST_HEADLESS
    /* testovací meze patří modulu, ne kopii */
    st.test_max_edges = g_cmttmz_rec.test_max_edges;
    st.test_max_ticks = g_cmttmz_rec.test_max_ticks;
#endif
    g_cmttmz_rec = st;
    g_cmttmz_rec.active = 1;
    return EXIT_SUCCESS;
}


size_t cmttmz_rec_copy_bytes ( const st_CMTTMZ_REC_COPY *copy ) {
    return copy ? ( size_t ) copy->st.rec.count * sizeof ( uint32_t ) : 0;
}


void cmttmz_rec_copy_free ( st_CMTTMZ_REC_COPY *copy ) {
    if ( !copy ) return;
    free ( copy->st.rec.ticks );
    free ( copy );
}


/* ------------------------------------------------------------------------- */
/*  Dekódování záznamu                                                       */
/* ------------------------------------------------------------------------- */


/**
 * @brief Iterátor pulzů záznamu: úseky rozdělené kvůli délce sloučí zpět.
 *
 * Pulz k má úroveň first_level ^ (k & 1): sloučení spotřebuje vždy úsek
 * nulové délky a pokračování téže úrovně, parita se tedy zachová.
 */
typedef struct st_CMTTMZ_REC_PULSE_IT {
    const st_CMTTMZ_REC *rec; /**< záznam (jen čtení) */
    uint32_t pos;             /**< další položka rec->ticks */
} st_CMTTMZ_REC_PULSE_IT;


/**
 * @brief Vrátí délku dalšího pulzu záznamu v taktech.
 * @param it Iterátor. @param[out] len Délka pulzu (sloučené úseky, může přesáhnout UINT32_MAX).
 * @return 1 = pulz vrácen, 0 = konec záznamu.
 */
static int cmttmz_rec_pulse_next ( st_CMTTMZ_REC_PULSE_IT *it, uint64_t *len ) {
    const st_CMTTMZ_REC *rec = it->rec;
    if ( it->pos >= rec->count ) return 0;
    uint64_t l = rec->ticks[it->pos++];
    /* úsek 0 opačné úrovně + pokračování = tentýž pulz */
    while ( ( it->pos + 1 < rec->count ) && ( rec->ticks[it->pos] == 0 ) ) {
        l += rec->ticks[it->pos + 1];
        it->pos += 2;
    }
    *len = l;
    return 1;
}


/**
 * @brief Přidá do textu reportu formátovaný řetězec (ořízne na velikost bufferu).
 * @param rep Report (smí být NULL). @param fmt Formát printf.
 */
static void cmttmz_rec_rep_append ( st_CMTTMZ_REC_REPORT *rep, const char *fmt, ... ) {
    if ( !rep ) return;
    size_t used = strlen ( rep->text );
    if ( used + 1 >= sizeof ( rep->text ) ) return;
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( rep->text + used, sizeof ( rep->text ) - used, fmt, ap );
    va_end ( ap );
}


int cmttmz_rec_file_range_valid ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses ) {
    return ( f->header_leader_pulse < f->consumed_until_pulse ) && ( f->consumed_until_pulse <= npulses );
}


int cmttmz_rec_file_body_complete ( const st_WAV_ANALYZER_FILE_RESULT *f ) {
    if ( !f->mzf ) return 0;
    return ( f->mzf->header.fsize == 0 ) || ( f->mzf->body && ( f->mzf->body_size >= f->mzf->header.fsize ) );
}


int cmttmz_rec_file_accepted ( const st_WAV_ANALYZER_FILE_RESULT *f, uint32_t npulses ) {
    if ( f->format != WAV_TAPE_FORMAT_NORMAL ) return 0;
    if ( ( f->header_crc != WAV_CRC_OK ) || ( f->body_crc != WAV_CRC_OK ) ) return 0;
    if ( f->recovery_status != WAV_RECOVERY_NONE ) return 0;
    if ( !f->mzf ) return 0;
    if ( !cmttmz_rec_file_body_complete ( f ) ) return 0;
    if ( !cmttmz_rec_file_range_valid ( f, npulses ) ) return 0;
    return 1;
}


/**
 * @brief Sestaví blok 0x18 (CSW, GDGCLK_BASE / 2) z pulzů záznamu first_pulse .. end_pulse - 1.
 *
 * Konce pulzů se počítají ze součtu taktů od začátku úseku (zaokrouhlení
 * polovinou nahoru), pulz má nejméně 1 vzorek. Úsek začínající HIGH dostane
 * předřazený pulz LOW 1 vzorek. Pulz nad UINT32_MAX vzorků se zkrátí
 * a o zkrácení se posunou konce dalších pulzů. Nejvýš
 * TMZ_PLAYER_MAX_BLOCK_PULSES pulzů a CMT_VSTREAM_MAX_BLOCK_SECONDS
 * sekund; zbytek úseku se vynechá.
 *
 * @param rec Záznam. @param first_pulse První pulz. @param end_pulse Pulz za úsekem (> first_pulse).
 * @param[out] block Blok (data vlastní volající, free()).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace nebo stavby bloku.
 */
static int cmttmz_rec_build_raw ( const st_CMTTMZ_REC *rec, uint32_t first_pulse, uint32_t end_pulse, st_TZX_BLOCK *block ) {
    const uint32_t rate = ( uint32_t ) ( GDGCLK_BASE / 2 );
    const uint64_t max_samples = ( uint64_t ) CMT_VSTREAM_MAX_BLOCK_SECONDS * rate;
    uint64_t n = ( uint64_t ) ( end_pulse - first_pulse ) + 1u;
    if ( n > TMZ_PLAYER_MAX_BLOCK_PULSES ) n = TMZ_PLAYER_MAX_BLOCK_PULSES;
    uint32_t *pulses = ( uint32_t* ) malloc ( ( size_t ) n * sizeof ( uint32_t ) );
    if ( !pulses ) return EXIT_FAILURE;

    st_CMTTMZ_REC_PULSE_IT it = { rec, 0 };
    uint64_t len = 0;
    for ( uint32_t k = 0; k < first_pulse; k++ ) ( void ) cmttmz_rec_pulse_next ( &it, &len );

    uint32_t count = 0;
    uint64_t prepend = 0;
    if ( ( rec->first_level ^ ( int ) ( first_pulse & 1u ) ) & 1 ) {
        /* první pulz CSW je LOW: úsek začínající HIGH dostane LOW 1 vzorek */
        pulses[count++] = 1;
        prepend = 1;
    }
    uint64_t cum = 0;       /* takty od začátku úseku */
    uint64_t shift = 0;     /* vzorky vynechané zkrácením dlouhých pulzů */
    uint64_t prev_end = prepend;
    for ( uint32_t k = first_pulse; k < end_pulse && count < n; k++ ) {
        if ( !cmttmz_rec_pulse_next ( &it, &len ) ) break;
        cum += len;
        uint64_t end = prepend + ( cum + 1u ) / 2u - shift;
        if ( end <= prev_end ) end = prev_end + 1u;
        uint64_t d = end - prev_end;
        if ( d > UINT32_MAX ) {
            shift += d - UINT32_MAX;
            d = UINT32_MAX;
        }
        if ( prev_end + d > max_samples ) {
            d = ( max_samples > prev_end ) ? max_samples - prev_end : 0;
            if ( d ) pulses[count++] = ( uint32_t ) d;
            break;
        }
        pulses[count++] = ( uint32_t ) d;
        prev_end += d;
    }
    en_TZX_ERROR terr = TZX_ERROR_NULL_INPUT;
    if ( count ) terr = tzx_block_create_csw_recording ( rate, 0, pulses, count, block );
    free ( pulses );
    return ( terr == TZX_OK ) ? EXIT_SUCCESS : EXIT_FAILURE;
}


uint16_t cmttmz_rec_mzf_pause ( const st_CMTTMZ_IMPORT_OPTS *opts, int next_is_mzf ) {
    return ( next_is_mzf && opts ) ? opts->pause_ms : 0;
}


int cmttmz_rec_build_mzf ( const st_MZF *mzf, const st_CMTTMZ_IMPORT_OPTS *opts, uint16_t pause_ms, st_TZX_BLOCK *block ) {
    if ( !mzf || !opts || !block ) return EXIT_FAILURE;
    st_TZX_BLOCK *nb = cmttmz_build_mzf_block ( &mzf->header, mzf->body, mzf->header.fsize, opts->default_speed,
                                                &opts->custom_pulses, pause_ms );
    if ( !nb ) return EXIT_FAILURE;
    *block = *nb;
    nb->data = NULL;   /* data převzal volající */
    tmz_block_free ( nb );
    return EXIT_SUCCESS;
}


void cmttmz_rec_blocks_free ( st_TZX_BLOCK *blocks, uint32_t count ) {
    if ( !blocks ) return;
    for ( uint32_t i = 0; i < count; i++ ) free ( blocks[i].data );
    free ( blocks );
}


#ifdef MZTEST_HEADLESS
/** @brief Jen pro testy: 1 = cmttmz_rec_decode() se chová, jako by analýza selhala. */
static int s_test_fail_analysis = 0;
#endif


/**
 * @brief Spustí analýzu pulzů záznamu.
 *
 * @param rec Záznam. @param npulses Počet pulzů po sloučení rozdělených úseků.
 * @param[out] res Výsledek (při úspěchu uvolnit wav_analyzer_result_destroy()).
 * @return EXIT_SUCCESS; EXIT_FAILURE při selhání alokace nebo analýzy (res je pak vynulovaný).
 */
static int cmttmz_rec_analyze ( const st_CMTTMZ_REC *rec, uint32_t npulses, st_WAV_ANALYZER_RESULT *res ) {
    memset ( res, 0, sizeof ( *res ) );
#ifdef MZTEST_HEADLESS
    if ( s_test_fail_analysis ) return EXIT_FAILURE;
#endif
    st_WAV_PULSE_SEQUENCE seq;
    memset ( &seq, 0, sizeof ( seq ) );
    seq.pulses = ( st_WAV_PULSE* ) malloc ( ( size_t ) npulses * sizeof ( st_WAV_PULSE ) );
    if ( !seq.pulses ) return EXIT_FAILURE;
    seq.count = seq.capacity = npulses;
    seq.sample_rate = GDGCLK_BASE;
    /* pulzy pro analyzátor: sloučené úseky, úroveň PC1, délka v µs přesně */
    st_CMTTMZ_REC_PULSE_IT it = { rec, 0 };
    uint64_t len = 0;
    for ( uint32_t k = 0; k < npulses; k++ ) {
        ( void ) cmttmz_rec_pulse_next ( &it, &len );
        st_WAV_PULSE *p = &seq.pulses[k];
        p->duration_samples = ( len > UINT32_MAX ) ? UINT32_MAX : ( uint32_t ) len;
        p->duration_us = ( double ) len * 1000000.0 / ( double ) GDGCLK_BASE;
        p->level = ( rec->first_level ^ ( int ) ( k & 1u ) ) & 1;
    }
    st_WAV_ANALYZER_CONFIG cfg;
    wav_analyzer_config_default ( &cfg );   /* keep_unknown 0, recover_* 0: beze změny */
    en_WAV_ANALYZER_ERROR aerr = wav_analyzer_analyze_pulses ( &seq, &cfg, res );
    free ( seq.pulses );
    if ( aerr != WAV_ANALYZER_OK ) {
        wav_analyzer_result_destroy ( res );
        memset ( res, 0, sizeof ( *res ) );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


uint32_t cmttmz_rec_file_start ( const st_WAV_ANALYZER_FILE_RESULT *f ) {
    /* výsledky ZX / SINCLAIR header_leader_pulse nenastavují (wav_analyzer.c, větve ZX a fallback ZX);
     * u ostatních formátů je leader datový leader (TURBO: za preloaderem), náhrada by posunula začátek
     * souboru na pulzu 0 */
    if ( f->header_leader_pulse == 0
         && ( f->format == WAV_TAPE_FORMAT_ZX_SPECTRUM || f->format == WAV_TAPE_FORMAT_SINCLAIR ) ) return f->leader.start_index;
    return f->header_leader_pulse;
}


/**
 * @brief Plán v režimu všech souborů (analýza surového bloku, cmttmz_rec_plan() s all_files 1).
 *
 * Soubory s platným rozsahem (cmttmz_rec_file_start() < consumed_until_pulse
 * <= npulses) v pořadí začátku (při shodě v pořadí výsledku); žádný se
 * nevynechá. Soubor, který začíná před koncem předchozího, dostane rozsah
 * od konce předchozího (překryv patří dřívějšímu souboru); soubor celý
 * uvnitř dřívějšího má prázdný rozsah (first_pulse == end_pulse). Úseky bez
 * souboru s aspoň min_gap pulzy dají část RAW.
 *
 * @param res Výsledek analýzy. @param npulses Počet pulzů. @param min_gap Mez úseku RAW (>= 1).
 * @param[out] parts Pole nejméně 2 * res->file_count + 1 částí. @return Počet částí.
 */
static uint32_t cmttmz_rec_plan_all ( const st_WAV_ANALYZER_RESULT *res, uint32_t npulses, uint32_t min_gap,
                                      st_CMTTMZ_REC_PART *parts ) {
    uint32_t nparts = 0;
    uint32_t cursor = 0;
    uint32_t last_start = 0, last_idx = 0;
    int have_last = 0;
    for ( ;; ) {
        /* další soubor podle klíče (začátek, index) za naposledy vybraným */
        uint32_t best = UINT32_MAX, best_start = 0;
        for ( uint32_t i = 0; i < res->file_count; i++ ) {
            const st_WAV_ANALYZER_FILE_RESULT *f = &res->files[i];
            uint32_t st = cmttmz_rec_file_start ( f );
            if ( ( st >= f->consumed_until_pulse ) || ( f->consumed_until_pulse > npulses ) ) continue;
            if ( have_last && ( ( st < last_start ) || ( ( st == last_start ) && ( i <= last_idx ) ) ) ) continue;
            if ( ( best == UINT32_MAX ) || ( st < best_start ) ) {
                best = i;
                best_start = st;
            }
        }
        uint32_t first = ( best == UINT32_MAX ) ? npulses : ( ( best_start > cursor ) ? best_start : cursor );
        if ( first - cursor >= min_gap ) {
            parts[nparts].kind = CMTTMZ_REC_BLOCK_RAW;
            parts[nparts].first_pulse = cursor;
            parts[nparts].end_pulse = first;
            parts[nparts].file = 0;
            nparts++;
        }
        if ( best == UINT32_MAX ) break;
        uint32_t end = res->files[best].consumed_until_pulse;
        if ( end < first ) end = first;   /* soubor celý uvnitř dřívějšího */
        parts[nparts].kind = CMTTMZ_REC_BLOCK_MZF;
        parts[nparts].first_pulse = first;
        parts[nparts].end_pulse = end;
        parts[nparts].file = best;
        nparts++;
        cursor = end;
        have_last = 1;
        last_start = best_start;
        last_idx = best;
    }
    return nparts;
}


uint32_t cmttmz_rec_plan ( const st_WAV_ANALYZER_RESULT *res, uint32_t npulses, int all_files, uint32_t min_gap,
                           st_CMTTMZ_REC_PART *parts ) {
    if ( min_gap == 0 ) min_gap = 1;   /* prázdný úsek část nedá nikdy */
    if ( all_files ) return cmttmz_rec_plan_all ( res, npulses, min_gap, parts );
    uint32_t nparts = 0;
    uint32_t cursor = 0;
    for ( ;; ) {
        /* další přijatý soubor za kurzorem (nejmenší header_leader_pulse >= cursor) */
        uint32_t best = UINT32_MAX;
        for ( uint32_t i = 0; i < res->file_count; i++ ) {
            const st_WAV_ANALYZER_FILE_RESULT *f = &res->files[i];
            if ( !cmttmz_rec_file_accepted ( f, npulses ) || ( f->header_leader_pulse < cursor ) ) continue;
            if ( ( best == UINT32_MAX ) || ( f->header_leader_pulse < res->files[best].header_leader_pulse ) ) best = i;
        }
        uint32_t gap_end = ( best == UINT32_MAX ) ? npulses : res->files[best].header_leader_pulse;
        if ( gap_end - cursor >= min_gap ) {
            parts[nparts].kind = CMTTMZ_REC_BLOCK_RAW;
            parts[nparts].first_pulse = cursor;
            parts[nparts].end_pulse = gap_end;
            parts[nparts].file = 0;
            nparts++;
        }
        if ( best == UINT32_MAX ) break;
        parts[nparts].kind = CMTTMZ_REC_BLOCK_MZF;
        parts[nparts].first_pulse = res->files[best].header_leader_pulse;
        parts[nparts].end_pulse = res->files[best].consumed_until_pulse;
        parts[nparts].file = best;
        nparts++;
        cursor = res->files[best].consumed_until_pulse;
    }
    return nparts;
}


int cmttmz_rec_decode ( const st_CMTTMZ_REC *rec, const st_CMTTMZ_IMPORT_OPTS *opts, st_TZX_BLOCK **blocks_out,
                        st_CMTTMZ_REC_BLOCK_INFO **info_out, uint32_t *count_out, st_CMTTMZ_REC_REPORT *rep ) {
    if ( rep ) memset ( rep, 0, sizeof ( *rep ) );
    if ( blocks_out ) *blocks_out = NULL;
    if ( info_out ) *info_out = NULL;
    if ( count_out ) *count_out = 0;
    if ( ( !rec ) || ( !blocks_out ) || ( !count_out ) ) {
        cmttmz_rec_rep_append ( rep, "Recording decoder: invalid parameter" );
        return EXIT_FAILURE;
    }

    /* mez "málo hran" na pulzech po sloučení rozdělených úseků (jako u úseků RAW) */
    uint32_t npulses = 0;
    if ( rec->ticks ) {
        st_CMTTMZ_REC_PULSE_IT it = { rec, 0 };
        uint64_t len = 0;
        while ( cmttmz_rec_pulse_next ( &it, &len ) ) npulses++;
    }
    if ( npulses < CMTTMZ_REC_MIN_EDGES ) {
        cmttmz_rec_rep_append ( rep, "Recording too short, nothing stored" );
        return EXIT_SUCCESS;
    }

    st_CMTTMZ_IMPORT_OPTS def_opts;
    if ( !opts ) {
        cmttmz_import_opts_init ( &def_opts );
        opts = &def_opts;
    }

    st_WAV_ANALYZER_RESULT res;
    int analyzed = ( EXIT_SUCCESS == cmttmz_rec_analyze ( rec, npulses, &res ) );

    /* plán; bez analýzy celý záznam jako jeden surový blok (záznam se neztratí) */
    size_t cap = analyzed ? ( ( size_t ) res.file_count * 2u + 1u ) : 1u;
    st_CMTTMZ_REC_PART *parts = ( st_CMTTMZ_REC_PART* ) malloc ( cap * sizeof ( st_CMTTMZ_REC_PART ) );
    if ( !parts ) {
        if ( analyzed ) wav_analyzer_result_destroy ( &res );
        cmttmz_rec_rep_append ( rep, "Not enough memory" );
        return EXIT_FAILURE;
    }
    uint32_t nparts;
    if ( analyzed ) {
        nparts = cmttmz_rec_plan ( &res, npulses, 0, CMTTMZ_REC_MIN_EDGES, parts );
    } else {
        parts[0].kind = CMTTMZ_REC_BLOCK_RAW;
        parts[0].first_pulse = 0;
        parts[0].end_pulse = npulses;
        parts[0].file = 0;
        nparts = 1;
    }

    if ( nparts == 0 ) {
        free ( parts );
        if ( analyzed ) wav_analyzer_result_destroy ( &res );
        cmttmz_rec_rep_append ( rep, "Nothing to store" );
        return EXIT_SUCCESS;
    }

    st_TZX_BLOCK *blocks = ( st_TZX_BLOCK* ) calloc ( nparts, sizeof ( st_TZX_BLOCK ) );
    st_CMTTMZ_REC_BLOCK_INFO *info = ( st_CMTTMZ_REC_BLOCK_INFO* ) calloc ( nparts, sizeof ( st_CMTTMZ_REC_BLOCK_INFO ) );
    int ok = ( blocks && info );
    uint32_t built = 0;
    uint32_t nmzf = 0, nraw = 0;
    for ( uint32_t i = 0; ok && i < nparts; i++ ) {
        const st_CMTTMZ_REC_PART *pt = &parts[i];
        info[i].kind = pt->kind;
        if ( pt->kind == CMTTMZ_REC_BLOCK_RAW ) {
            info[i].pause_ms = 0;   /* časování včetně ticha nese signál bloku */
            ok = ( EXIT_SUCCESS == cmttmz_rec_build_raw ( rec, pt->first_pulse, pt->end_pulse, &blocks[i] ) );
            if ( ok ) nraw++;
        } else {
            /* pauza jen mezi dvěma bloky MZF; před surovým blokem a na konci 0 */
            int next_mzf = ( i + 1 < nparts ) && ( parts[i + 1].kind == CMTTMZ_REC_BLOCK_MZF );
            info[i].pause_ms = cmttmz_rec_mzf_pause ( opts, next_mzf );
            ok = ( EXIT_SUCCESS == cmttmz_rec_build_mzf ( res.files[pt->file].mzf, opts, info[i].pause_ms, &blocks[i] ) );
            if ( ok ) nmzf++;
        }
        if ( ok ) built++;
    }
    if ( !ok ) {
        if ( blocks ) cmttmz_rec_blocks_free ( blocks, built );
        free ( info );
        free ( parts );
        if ( analyzed ) wav_analyzer_result_destroy ( &res );
        cmttmz_rec_rep_append ( rep, "Can't build a tape block from the recording" );
        return EXIT_FAILURE;
    }

    /* text pro uživatele */
    if ( rep ) {
        rep->mzf_blocks = nmzf;
        rep->raw_blocks = nraw;
        if ( !analyzed ) {
            cmttmz_rec_rep_append ( rep, "Recording analysis failed, stored as a raw block" );
        } else if ( nmzf == 0 ) {
            if ( nraw == 1 ) cmttmz_rec_rep_append ( rep, "Recording could not be decoded, stored as a raw block" );
            else cmttmz_rec_rep_append ( rep, "Recording could not be decoded, stored as %u raw blocks", ( unsigned ) nraw );
        } else {
            cmttmz_rec_rep_append ( rep, "Recorded " );
            uint32_t done = 0;
            for ( uint32_t i = 0; i < nparts; i++ ) {
                if ( parts[i].kind != CMTTMZ_REC_BLOCK_MZF ) continue;
                const st_MZF *mzf = res.files[parts[i].file].mzf;
                char name[MZF_FNAME_FULL_LENGTH];
                mzf_tools_get_fname ( &mzf->header, name );
                cmttmz_rec_rep_append ( rep, "%s\"%s\", %u B", done ? "; " : "", name, ( unsigned ) mzf->header.fsize );
                done++;
            }
            if ( nraw == 1 ) cmttmz_rec_rep_append ( rep, "; an undecoded part stored as a raw block" );
            else if ( nraw > 1 ) cmttmz_rec_rep_append ( rep, "; %u undecoded parts stored as raw blocks", ( unsigned ) nraw );
        }
    }

    free ( parts );
    if ( analyzed ) wav_analyzer_result_destroy ( &res );
    *blocks_out = blocks;
    if ( info_out ) *info_out = info;
    else free ( info );
    *count_out = nparts;
    return EXIT_SUCCESS;
}


#ifdef MZTEST_HEADLESS

void cmttmz_rec_test_set_limits ( uint32_t max_edges, uint64_t max_ticks ) {
    g_cmttmz_rec.test_max_edges = max_edges;
    g_cmttmz_rec.test_max_ticks = max_ticks;
}


void cmttmz_rec_test_fail_analysis ( int fail ) {
    s_test_fail_analysis = fail ? 1 : 0;
}

#endif
