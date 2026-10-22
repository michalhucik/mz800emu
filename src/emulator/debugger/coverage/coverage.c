/**
 * @file   coverage.c
 * @brief  Pokrytí kódu - jádro (viz coverage.h).
 *
 * Bitmapy úrovně branch a epochy prvního vykonání jsou statická pole
 * (40 KiB + 128 KiB). Tabulka jednotek úrovně detail je hašovací tabulka
 * s otevřeným adresováním (lineární zkoušení); počet slotů je mocnina 2,
 * nejméně dvojnásobek kapacity, takže zaplnění nepřesáhne 50 %.
 *
 * @par Licence: GPLv3
 */

#include "coverage.h"

#include <stdlib.h>
#include <string.h>


int g_coverage_active = 0;


/** @brief Celý stav pokrytí (data, čítače, nastavení). */
static struct {
    bool has_data;                       /**< Data založena (coverage_start). */
    st_COVERAGE_CONFIG cfg;              /**< Nastavení dat. */

    uint8_t exec[ COVERAGE_PC_BITMAP_BYTES ];      /**< Vykonané adresy. */
    uint8_t decision[ COVERAGE_PC_BITMAP_BYTES ];  /**< Adresy s podmíněnou instrukcí. */
    uint8_t taken[ COVERAGE_PC_BITMAP_BYTES ];     /**< Podmínka splněna. */
    uint8_t not_taken[ COVERAGE_PC_BITMAP_BYTES ]; /**< Podmínka nesplněna. */
    uint8_t int_entry[ COVERAGE_PC_BITMAP_BYTES ]; /**< První instrukce obsluhy přerušení. */
    uint16_t first_epoch[ 0x10000 ];               /**< Epocha prvního vykonání adresy. */

    st_COVERAGE_UNIT *units;             /**< Sloty tabulky (detail), jinak NULL. */
    uint32_t slots;                      /**< Počet slotů (mocnina 2). */
    uint32_t used;                       /**< Obsazené sloty. */

    uint64_t dropped;                    /**< Kroky bez místa pro novou jednotku. */
    uint64_t fetch_fallback;             /**< Kroky s bajtem mimo záznam fetch. */
    uint64_t instructions;               /**< Zapsané kroky. */
    uint64_t halt_idle;                  /**< Kroky nečinného HALT. */
    bool saturated;                      /**< Některý čítač saturoval. */
    uint32_t discontinuities;            /**< Nespojitosti běhu. */
    uint32_t host_state_edits;           /**< Zásahy hostitele mezi instrukcemi. */
    uint32_t epoch;                      /**< Aktuální epocha. */
    uint32_t generation;                 /**< Generace dat (zvýší ji každé smazání dat). */

    bool prev_valid;                     /**< prev_halted a prev_pc platí (navazující krok). */
    bool prev_halted;                    /**< CPU v HALT po předchozím kroku. */
    uint16_t prev_pc;                    /**< Adresa předchozího kroku. */
} s_cov;


/**
 * @brief Nejmenší mocnina 2, která je >= n (n >= 1, n <= 2^31).
 * @param n Požadovaná hodnota.
 * @return Mocnina 2.
 */
static uint32_t cov_pow2 ( uint32_t n )
{
    uint32_t p = 1;
    while ( p < n ) p <<= 1;
    return p;
}


/**
 * @brief Smaže data a čítače dat; nespojitosti a zásahy hostitele nechá.
 */
static void cov_clear_data ( void )
{
    memset ( s_cov.exec, 0, sizeof ( s_cov.exec ) );
    memset ( s_cov.decision, 0, sizeof ( s_cov.decision ) );
    memset ( s_cov.taken, 0, sizeof ( s_cov.taken ) );
    memset ( s_cov.not_taken, 0, sizeof ( s_cov.not_taken ) );
    memset ( s_cov.int_entry, 0, sizeof ( s_cov.int_entry ) );
    memset ( s_cov.first_epoch, 0, sizeof ( s_cov.first_epoch ) );
    if ( s_cov.units ) {
        memset ( s_cov.units, 0, (size_t) s_cov.slots * sizeof ( st_COVERAGE_UNIT ) );
    };
    s_cov.used = 0;
    s_cov.dropped = 0;
    s_cov.fetch_fallback = 0;
    s_cov.instructions = 0;
    s_cov.halt_idle = 0;
    s_cov.saturated = false;
    s_cov.epoch = 0;
    s_cov.generation++;
    s_cov.prev_valid = false;
}


bool coverage_start ( const st_COVERAGE_CONFIG *cfg, bool reset )
{
    st_COVERAGE_CONFIG c = *cfg;
    if ( c.capacity < 1 ) c.capacity = 1;
    if ( c.capacity > COVERAGE_MAX_CAPACITY ) c.capacity = COVERAGE_MAX_CAPACITY;
    if ( c.level != COVERAGE_LEVEL_DETAIL ) {
        c.level = COVERAGE_LEVEL_BRANCH;
    };

    bool same = s_cov.has_data && s_cov.cfg.level == c.level
                && ( c.level == COVERAGE_LEVEL_BRANCH
                     || ( s_cov.cfg.key == c.key && s_cov.cfg.capacity == c.capacity ) );

    if ( !same || reset ) {
        st_COVERAGE_UNIT *units = NULL;
        uint32_t slots = 0;
        if ( c.level == COVERAGE_LEVEL_DETAIL ) {
            slots = cov_pow2 ( c.capacity * 2u );
            units = calloc ( slots, sizeof ( st_COVERAGE_UNIT ) );
            if ( !units ) {
                return false;
            };
        };
        free ( s_cov.units );
        s_cov.units = units;
        s_cov.slots = slots;
        s_cov.cfg = c;
        cov_clear_data ( );
        s_cov.discontinuities = 0;
        s_cov.host_state_edits = 0;
        s_cov.has_data = true;
    } else {
        s_cov.cfg.reset_on_discontinuity = c.reset_on_discontinuity;
    };

    /* Navazující krok po pauze sběru neplatí (mezitím mohl běžet program). */
    s_cov.prev_valid = false;
    g_coverage_active = 1;
    return true;
}


void coverage_stop ( void )
{
    g_coverage_active = 0;
}


void coverage_reset ( void )
{
    cov_clear_data ( );
    s_cov.discontinuities = 0;
    s_cov.host_state_edits = 0;
}


void coverage_free ( void )
{
    g_coverage_active = 0;
    free ( s_cov.units );
    s_cov.units = NULL;
    s_cov.slots = 0;
    s_cov.has_data = false;
    cov_clear_data ( );
    s_cov.discontinuities = 0;
    s_cov.host_state_edits = 0;
}


uint32_t coverage_mark ( void )
{
    if ( s_cov.epoch < COVERAGE_EPOCH_MAX ) s_cov.epoch++;
    return s_cov.epoch;
}


void coverage_note_discontinuity ( void )
{
    if ( !s_cov.has_data ) return;
    if ( s_cov.cfg.reset_on_discontinuity ) {
        cov_clear_data ( );
    };
    s_cov.discontinuities++;
    s_cov.prev_valid = false;
}


void coverage_note_host_state_edit ( void )
{
    if ( !s_cov.has_data ) return;
    s_cov.host_state_edits++;
    s_cov.prev_valid = false;
}


/**
 * @brief Přičte 1 k čítači se saturací.
 * @param c     Čítač.
 * @param flags Příznaky jednotky (při saturaci dostanou COVERAGE_F_SATURATED).
 */
static void cov_inc ( uint32_t *c, uint16_t *flags )
{
    if ( *c != UINT32_MAX ) {
        ( *c )++;
        if ( *c != UINT32_MAX ) return;
    };
    *flags |= COVERAGE_F_SATURATED;
    s_cov.saturated = true;
}


/**
 * @brief Haš klíče jednotky.
 * @param cell Buňka.
 * @param pc   Logická adresa.
 * @param keyw Bajty klíče složené little-endian, délka v horních bitech není.
 * @param klen Délka klíče.
 * @return Haš (32 bitů).
 */
static uint32_t cov_hash ( uint32_t cell, uint16_t pc, uint32_t keyw, uint8_t klen )
{
    uint32_t h = cell * 0x9E3779B1u;
    h ^= ( (uint32_t) pc | ( (uint32_t) klen << 16 ) ) * 0x85EBCA77u;
    h ^= keyw * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}


/**
 * @brief Zapíše krok do tabulky jednotek (úroveň detail).
 * @param s       Krok.
 * @param insn    Dekódovaný krok.
 * @param outcome Výsledek podmínky.
 */
static void cov_detail ( const st_COVERAGE_STEP *s, const st_COVERAGE_INSN *insn,
                         en_COVERAGE_OUTCOME outcome )
{
    uint8_t key[ 4 ] = { 0, 0, 0, 0 };
    uint8_t klen = 0;
    uint16_t flags = (uint16_t) ( s->flags & COVERAGE_F_STEP_MASK );
    unsigned len = insn->len;

    if ( s_cov.cfg.key == COVERAGE_KEY_FULL_BYTES ) {
        unsigned n = ( len < 4 ) ? len : 4;
        for ( unsigned i = 0; i < n; i++ ) key[ i ] = s->read ( s->read_ctx, i );
        klen = (uint8_t) n;
        if ( len > 4 ) flags |= COVERAGE_F_KEY_TRUNCATED;
    } else {
        unsigned want = insn->op_len + ( insn->op_last ? 1u : 0u );
        unsigned lead = insn->op_len;
        if ( want > 4 ) {
            flags |= COVERAGE_F_KEY_TRUNCATED;
            lead = insn->op_last ? 3u : 4u;
        };
        for ( unsigned i = 0; i < lead; i++ ) key[ i ] = s->read ( s->read_ctx, i );
        klen = (uint8_t) lead;
        if ( insn->op_last ) key[ klen++ ] = s->read ( s->read_ctx, len - 1 );
    };

    /* Operand: první dva bajty za opkódem (u DD/FD CB d op jen d). */
    uint16_t operand = 0;
    bool has_operand = false;
    unsigned op_end = insn->op_last ? len - 1u : len;
    if ( !insn->prefix_split && op_end > insn->op_len ) {
        has_operand = true;
        operand = s->read ( s->read_ctx, insn->op_len );
        if ( op_end > insn->op_len + 1u ) {
            operand |= (uint16_t) ( s->read ( s->read_ctx, insn->op_len + 1u ) << 8 );
        };
    };

    if ( insn->cond != COVERAGE_COND_NONE ) flags |= COVERAGE_F_DECISION;
    if ( insn->halt ) flags |= COVERAGE_F_HALT;
    if ( insn->prefix_split ) flags |= COVERAGE_F_PREFIX_SPLIT;
    if ( s->fallback && *s->fallback ) {
        flags |= COVERAGE_F_FETCH_FALLBACK;
        s_cov.fetch_fallback++;
    };

    uint32_t keyw = (uint32_t) key[ 0 ] | ( (uint32_t) key[ 1 ] << 8 )
                    | ( (uint32_t) key[ 2 ] << 16 ) | ( (uint32_t) key[ 3 ] << 24 );
    uint32_t mask = s_cov.slots - 1u;
    uint32_t i = cov_hash ( s->cell, s->pc, keyw, klen ) & mask;
    st_COVERAGE_UNIT *u;
    for ( ;; ) {
        u = &s_cov.units[ i ];
        if ( u->insn_len == 0 ) {
            if ( s_cov.used >= s_cov.cfg.capacity ) {
                s_cov.dropped++;
                return;
            };
            memset ( u, 0, sizeof ( *u ) );
            u->cell = s->cell;
            u->pc = s->pc;
            u->key_len = klen;
            memcpy ( u->key, key, sizeof ( u->key ) );
            u->insn_len = (uint8_t) len;
            u->operand_last = operand;
            u->first_epoch = (uint16_t) s_cov.epoch;
            s_cov.used++;
            break;
        };
        if ( u->cell == s->cell && u->pc == s->pc && u->key_len == klen
             && memcmp ( u->key, key, sizeof ( key ) ) == 0 ) {
            if ( has_operand && u->operand_last != operand ) {
                u->flags |= COVERAGE_F_OPERAND_VARIED;
                u->operand_last = operand;
            };
            break;
        };
        i = ( i + 1u ) & mask;
    };

    u->flags |= flags;
    u->next_pc_last = s->next_pc;
    cov_inc ( &u->exec, &u->flags );
    if ( outcome == COVERAGE_OUTCOME_TAKEN ) {
        cov_inc ( &u->taken, &u->flags );
    } else if ( outcome == COVERAGE_OUTCOME_NOT_TAKEN ) {
        cov_inc ( &u->not_taken, &u->flags );
    };
}


void coverage_on_step ( const st_COVERAGE_STEP *s )
{
    if ( s_cov.prev_valid && s_cov.prev_halted && s->halted && s->pc == s_cov.prev_pc ) {
        /* Opakovaný fetch HALT - CPU nic nevykonává. */
        s_cov.halt_idle++;
        return;
    };
    s_cov.prev_valid = true;
    s_cov.prev_halted = s->halted;
    s_cov.prev_pc = s->pc;

    st_COVERAGE_INSN insn;
    coverage_decode ( s->read, s->read_ctx, &insn );
    en_COVERAGE_OUTCOME outcome = coverage_decode_outcome ( &insn, s->pc, s->next_pc, s->f, s->b );
    s_cov.instructions++;

    unsigned byte = s->pc >> 3;
    uint8_t bit = (uint8_t) ( 1u << ( s->pc & 7u ) );
    if ( !( s_cov.exec[ byte ] & bit ) ) {
        s_cov.exec[ byte ] |= bit;
        s_cov.first_epoch[ s->pc ] = (uint16_t) s_cov.epoch;
    };
    if ( insn.cond != COVERAGE_COND_NONE ) {
        s_cov.decision[ byte ] |= bit;
        if ( outcome == COVERAGE_OUTCOME_TAKEN ) {
            s_cov.taken[ byte ] |= bit;
        } else {
            s_cov.not_taken[ byte ] |= bit;
        };
    };
    if ( s->flags & COVERAGE_F_INT_ENTRY ) {
        s_cov.int_entry[ byte ] |= bit;
    };

    if ( s_cov.cfg.level == COVERAGE_LEVEL_DETAIL && s_cov.units ) {
        cov_detail ( s, &insn, outcome );
    };
}


/**
 * @brief Počet nastavených bitů v bajtu.
 * @param v Bajt.
 * @return 0-8.
 */
static unsigned cov_popcount8 ( uint8_t v )
{
    unsigned n = 0;
    for ( ; v; v &= (uint8_t) ( v - 1u ) ) n++;
    return n;
}


void coverage_get_status ( st_COVERAGE_STATUS *out )
{
    memset ( out, 0, sizeof ( *out ) );
    out->active = g_coverage_active != 0;
    out->has_data = s_cov.has_data;
    out->level = s_cov.cfg.level;
    out->key = s_cov.cfg.key;
    out->reset_on_discontinuity = s_cov.cfg.reset_on_discontinuity;
    out->dropped = s_cov.dropped;
    out->fetch_fallback = s_cov.fetch_fallback;
    out->saturated = s_cov.saturated;
    out->discontinuities = s_cov.discontinuities;
    out->host_state_edits = s_cov.host_state_edits;
    out->epoch = s_cov.epoch;
    out->generation = s_cov.generation;
    out->instructions = s_cov.instructions;
    out->halt_idle = s_cov.halt_idle;
    out->complete = s_cov.dropped == 0 && s_cov.fetch_fallback == 0 && !s_cov.saturated;

    uint32_t executed = 0;
    for ( unsigned i = 0; i < COVERAGE_PC_BITMAP_BYTES; i++ ) {
        executed += cov_popcount8 ( s_cov.exec[ i ] );
        uint8_t d = s_cov.decision[ i ];
        if ( !d ) continue;
        uint8_t t = s_cov.taken[ i ] & d, n = s_cov.not_taken[ i ] & d;
        out->decisions += cov_popcount8 ( d );
        out->branches_both += cov_popcount8 ( t & n );
        out->branches_taken_only += cov_popcount8 ( (uint8_t) ( t & ~n ) );
        out->branches_not_taken_only += cov_popcount8 ( (uint8_t) ( n & ~t ) );
    };

    if ( s_cov.cfg.level == COVERAGE_LEVEL_DETAIL && s_cov.units ) {
        out->units = s_cov.used;
        out->capacity = s_cov.cfg.capacity;
    } else {
        out->units = executed;
    };
}


void coverage_get_bitmaps ( uint8_t *exec, uint8_t *decision, uint8_t *taken,
                            uint8_t *not_taken, uint8_t *int_entry )
{
    if ( exec ) memcpy ( exec, s_cov.exec, COVERAGE_PC_BITMAP_BYTES );
    if ( decision ) memcpy ( decision, s_cov.decision, COVERAGE_PC_BITMAP_BYTES );
    if ( taken ) memcpy ( taken, s_cov.taken, COVERAGE_PC_BITMAP_BYTES );
    if ( not_taken ) memcpy ( not_taken, s_cov.not_taken, COVERAGE_PC_BITMAP_BYTES );
    if ( int_entry ) memcpy ( int_entry, s_cov.int_entry, COVERAGE_PC_BITMAP_BYTES );
}


uint16_t coverage_pc_first_epoch ( uint16_t pc )
{
    return s_cov.first_epoch[ pc ];
}


uint32_t coverage_copy_units ( st_COVERAGE_UNIT *out, uint32_t max )
{
    if ( !s_cov.units ) return 0;
    uint32_t n = 0;
    for ( uint32_t i = 0; i < s_cov.slots; i++ ) {
        if ( s_cov.units[ i ].insn_len == 0 ) continue;
        if ( out && n < max ) out[ n ] = s_cov.units[ i ];
        n++;
    };
    return n;
}


const char *coverage_branch_state_name ( bool decision, uint32_t taken, uint32_t not_taken )
{
    if ( !decision ) return "none";
    if ( taken && not_taken ) return "both";
    if ( taken ) return "taken_only";
    if ( not_taken ) return "not_taken_only";
    return "unobserved";
}


void coverage_get_first_epochs ( uint16_t *out )
{
    memcpy ( out, s_cov.first_epoch, sizeof ( s_cov.first_epoch ) );
}


/** @brief Jména prostorů v pořadí en_RAMLOG_SPACE. */
static const char *const s_space_names[] = { "ram", "vram", "pcg", "memext", "rom" };


const char *coverage_space_name ( uint8_t space )
{
    if ( space == COVERAGE_SPACE_BUS ) return "bus";
    if ( space < sizeof ( s_space_names ) / sizeof ( s_space_names[ 0 ] ) ) return s_space_names[ space ];
    return "unknown";
}


bool coverage_space_parse ( const char *name, uint8_t *out )
{
    if ( !name ) return false;
    if ( strcmp ( name, "bus" ) == 0 ) {
        *out = COVERAGE_SPACE_BUS;
        return true;
    };
    for ( unsigned i = 0; i < sizeof ( s_space_names ) / sizeof ( s_space_names[ 0 ] ); i++ ) {
        if ( strcmp ( name, s_space_names[ i ] ) == 0 ) {
            *out = (uint8_t) i;
            return true;
        };
    };
    return false;
}


bool coverage_snapshot_take ( st_COVERAGE_SNAPSHOT *out, bool with_units )
{
    coverage_get_status ( &out->status );
    coverage_get_bitmaps ( out->exec, out->decision, out->taken, out->not_taken, out->int_entry );
    coverage_get_first_epochs ( out->first_epoch );
    out->units = NULL;
    out->n_units = with_units ? coverage_copy_units ( NULL, 0 ) : 0;
    if ( out->n_units == 0 ) return true;
    out->units = malloc ( (size_t) out->n_units * sizeof ( st_COVERAGE_UNIT ) );
    if ( !out->units ) {
        out->n_units = 0;
        return false;
    };
    out->n_units = coverage_copy_units ( out->units, out->n_units );
    return true;
}


void coverage_snapshot_clear ( st_COVERAGE_SNAPSHOT *s )
{
    if ( !s ) return;
    free ( s->units );
    s->units = NULL;
    s->n_units = 0;
}
