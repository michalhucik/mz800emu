/**
 * @file test_coverage.c
 * @brief Testy jádra pokrytí kódu (coverage.h) nad syntetickými kroky.
 *
 * Kroky se zadávají polem bajtů a stavem po kroku (PC, F, B, HALT), bez
 * emulátoru: úroveň branch (bitmapy, stavy větví), úroveň detail (verze
 * kódu na téže adrese, buňky, klíč DD CB d op, operand, kapacita a
 * `dropped`, chybějící bajty fetch), nečinný HALT, epochy, nespojitosti
 * a vstup do obsluhy přerušení. K tomu snímek bez jednotek, stav adresy
 * a filtr řádků z coverage_export.h (MCP coverage_read a okna GUI).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Jádro se vkládá přímo (ne linkuje), aby test saturace mohl nastavit
 * čítač jednotky těsně pod 0xFFFFFFFF (stav s_cov je statický). */
#include "emulator/debugger/coverage/coverage.c"
#include "emulator/debugger/coverage/coverage_export.h"


/** @brief Kontext čtení: bajty kroku a příznak "bajt mimo záznam fetch". */
typedef struct {
    const uint8_t *bytes; /**< Bajty kroku. */
    unsigned n;           /**< Počet bajtů. */
    unsigned fetched;     /**< Kolik bajtů je "ze záznamu fetch"; další nastaví fallback. */
    bool *fallback;       /**< Kam hlásit čtení mimo záznam fetch. */
} st_TCTX;


/** @brief Čtení bajtu kroku pro jádro pokrytí. */
static uint8_t t_read ( void *ctx, unsigned index )
{
    st_TCTX *c = (st_TCTX *) ctx;
    if ( index >= c->fetched && c->fallback ) *c->fallback = true;
    return ( index < c->n ) ? c->bytes[ index ] : 0x00;
}


/**
 * @brief Zapíše jeden krok.
 *
 * @param pc      Adresa kroku.
 * @param next_pc PC po kroku.
 * @param bytes   Bajty kroku.
 * @param n       Počet bajtů.
 * @param f       F po kroku.
 * @param b       B po kroku.
 * @param halted  cpu->halted po kroku.
 * @param cell    Fyzická buňka.
 * @param flags   Původ kroku (COVERAGE_F_STEP_MASK).
 * @param fetched Bajtů ze záznamu fetch (>= n = vše).
 */
static void step_ex ( uint16_t pc, uint16_t next_pc, const uint8_t *bytes, unsigned n,
                      uint8_t f, uint8_t b, bool halted, uint32_t cell, uint16_t flags,
                      unsigned fetched )
{
    bool fb = false;
    st_TCTX c = { bytes, n, fetched, &fb };
    st_COVERAGE_STEP s = {
        .pc = pc, .next_pc = next_pc, .f = f, .b = b, .halted = halted,
        .cell = cell, .flags = flags, .read = t_read, .read_ctx = &c, .fallback = &fb,
    };
    coverage_on_step ( &s );
}


/** @brief Krok v RAM (buňka = RAM na adrese pc), bez příznaků, vše ze záznamu fetch. */
static void step ( uint16_t pc, uint16_t next_pc, const uint8_t *bytes, unsigned n, uint8_t f, uint8_t b )
{
    step_ex ( pc, next_pc, bytes, n, f, b, false, COVERAGE_CELL ( 0, pc ), 0, 255 );
}


/** @brief Spustí sběr s danou úrovní, klíčem a kapacitou, s prázdnými daty. */
static void start ( en_COVERAGE_LEVEL level, en_COVERAGE_KEY key, uint32_t capacity, bool rod )
{
    st_COVERAGE_CONFIG cfg = { level, key, capacity, rod };
    TEST_ASSERT_TRUE ( coverage_start ( &cfg, true ) );
}


/** @brief Najde jednotku podle PC a prvního bajtu klíče; NULL = není. */
static const st_COVERAGE_UNIT *find_unit ( st_COVERAGE_UNIT *buf, uint32_t n, uint16_t pc,
                                           uint8_t key0, uint32_t cell )
{
    for ( uint32_t i = 0; i < n; i++ ) {
        if ( buf[ i ].pc == pc && buf[ i ].key[ 0 ] == key0 && buf[ i ].cell == cell ) return &buf[ i ];
    };
    return NULL;
}


static st_COVERAGE_UNIT s_units[ 64 ];


void setUp ( void )
{
}


void tearDown ( void )
{
    coverage_free ( );
}


/* ===== branch ===== */

/** @brief JR NZ oběma směry, JP Z jen skok, DJNZ jen propad; stavy a počty. */
void test_branch_states ( void )
{
    start ( COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, false );
    static const uint8_t jrnz[] = { 0x20, 0x10 };
    static const uint8_t jpz[] = { 0xCA, 0x00, 0x50 };
    static const uint8_t djnz[] = { 0x10, 0xFE };
    static const uint8_t nop[] = { 0x00 };

    step ( 0x3000, 0x3012, jrnz, 2, 0x00, 0 );   /* NZ splněno */
    step ( 0x3000, 0x3002, jrnz, 2, 0x40, 0 );   /* Z -> propad */
    step ( 0x3002, 0x5000, jpz, 3, 0x40, 0 );
    step ( 0x3005, 0x3007, djnz, 2, 0x00, 0 );   /* B = 0 -> propad */
    step ( 0x3007, 0x3008, nop, 1, 0x00, 0 );

    uint8_t exec[ COVERAGE_PC_BITMAP_BYTES ], dec[ COVERAGE_PC_BITMAP_BYTES ];
    uint8_t tk[ COVERAGE_PC_BITMAP_BYTES ], nt[ COVERAGE_PC_BITMAP_BYTES ];
    coverage_get_bitmaps ( exec, dec, tk, nt, NULL );
    TEST_ASSERT_EQUAL_HEX8 ( 0xA5, exec[ 0x3000 >> 3 ] );   /* 3000, 3002, 3005, 3007 */
    TEST_ASSERT_EQUAL_HEX8 ( 0x25, dec[ 0x3000 >> 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x05, tk[ 0x3000 >> 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x21, nt[ 0x3000 >> 3 ] );

    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_TRUE ( st.active );
    TEST_ASSERT_EQUAL_UINT32 ( 4, st.units );
    TEST_ASSERT_EQUAL_UINT64 ( 5, st.instructions );
    TEST_ASSERT_EQUAL_UINT32 ( 3, st.decisions );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.branches_both );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.branches_taken_only );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.branches_not_taken_only );
    TEST_ASSERT_TRUE ( st.complete );
}


/** @brief Nečinný HALT se nezapisuje, první vykonání HALT ano. */
void test_halt_idle_collapse ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t halt[] = { 0x76 };
    for ( int i = 0; i < 5; i++ ) {
        step_ex ( 0x2000, 0x2000, halt, 1, 0, 0, true, COVERAGE_CELL ( 0, 0x2000 ), 0, 255 );
    };
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.instructions );
    TEST_ASSERT_EQUAL_UINT64 ( 4, st.halt_idle );
    uint32_t n = coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_EQUAL_UINT32 ( 1, n );
    TEST_ASSERT_EQUAL_UINT32 ( 1, s_units[ 0 ].exec );
    TEST_ASSERT_TRUE ( s_units[ 0 ].flags & COVERAGE_F_HALT );
}


/* ===== detail ===== */

/**
 * @brief Samomodifikace: jiný operand = tatáž jednotka s OPERAND_VARIED,
 *        jiný opkód = nová jednotka; jiná buňka (banka) = nová jednotka.
 */
void test_detail_versions_and_cells ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t ld1[] = { 0x3E, 0x01 };
    static const uint8_t ld5[] = { 0x3E, 0x05 };
    static const uint8_t ldb[] = { 0x06, 0x01 };

    step ( 0x3037, 0x3039, ld1, 2, 0, 0 );
    step ( 0x3037, 0x3039, ld5, 2, 0, 0 );
    step ( 0x3037, 0x3039, ldb, 2, 0, 0 );
    /* Stejné PC, ale ROM (prostor 4) místo RAM. */
    step_ex ( 0x3037, 0x3039, ld1, 2, 0, 0, false, COVERAGE_CELL ( 4, 0x3037 ), 0, 255 );

    uint32_t n = coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_EQUAL_UINT32 ( 3, n );
    const st_COVERAGE_UNIT *a = find_unit ( s_units, n, 0x3037, 0x3E, COVERAGE_CELL ( 0, 0x3037 ) );
    TEST_ASSERT_NOT_NULL ( a );
    TEST_ASSERT_EQUAL_UINT32 ( 2, a->exec );
    TEST_ASSERT_EQUAL_UINT8 ( 1, a->key_len );
    TEST_ASSERT_EQUAL_UINT8 ( 2, a->insn_len );
    TEST_ASSERT_TRUE ( a->flags & COVERAGE_F_OPERAND_VARIED );
    TEST_ASSERT_EQUAL_HEX16 ( 0x0005, a->operand_last );
    TEST_ASSERT_NOT_NULL ( find_unit ( s_units, n, 0x3037, 0x06, COVERAGE_CELL ( 0, 0x3037 ) ) );
    const st_COVERAGE_UNIT *r = find_unit ( s_units, n, 0x3037, 0x3E, COVERAGE_CELL ( 4, 0x3037 ) );
    TEST_ASSERT_NOT_NULL ( r );
    TEST_ASSERT_FALSE ( r->flags & COVERAGE_F_OPERAND_VARIED );

    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT32 ( 3, st.units );
    TEST_ASSERT_EQUAL_UINT32 ( 16, st.capacity );
}


/** @brief Klíč full_bytes: jiný operand = jiná jednotka. */
void test_detail_full_bytes_key ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_FULL_BYTES, 16, false );
    static const uint8_t ld1[] = { 0x3E, 0x01 };
    static const uint8_t ld5[] = { 0x3E, 0x05 };
    step ( 0x3037, 0x3039, ld1, 2, 0, 0 );
    step ( 0x3037, 0x3039, ld5, 2, 0, 0 );
    TEST_ASSERT_EQUAL_UINT32 ( 2, coverage_copy_units ( s_units, 64 ) );
    TEST_ASSERT_EQUAL_UINT8 ( 2, s_units[ 0 ].key_len );
}


/** @brief DD CB d op: klíč DD CB op, operand d; jiné d = tatáž jednotka. */
void test_detail_ddcb_key ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t bit0[] = { 0xDD, 0xCB, 0x05, 0x46 };
    static const uint8_t bit0d[] = { 0xDD, 0xCB, 0x07, 0x46 };
    static const uint8_t bit1[] = { 0xDD, 0xCB, 0x05, 0x4E };
    step ( 0x4000, 0x4004, bit0, 4, 0, 0 );
    step ( 0x4000, 0x4004, bit0d, 4, 0, 0 );
    step ( 0x4000, 0x4004, bit1, 4, 0, 0 );
    uint32_t n = coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_EQUAL_UINT32 ( 2, n );
    for ( uint32_t i = 0; i < n; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( 3, s_units[ i ].key_len );
        TEST_ASSERT_EQUAL_HEX8 ( 0xDD, s_units[ i ].key[ 0 ] );
        TEST_ASSERT_EQUAL_HEX8 ( 0xCB, s_units[ i ].key[ 1 ] );
        if ( s_units[ i ].key[ 2 ] == 0x46 ) {
            TEST_ASSERT_EQUAL_UINT32 ( 2, s_units[ i ].exec );
            TEST_ASSERT_TRUE ( s_units[ i ].flags & COVERAGE_F_OPERAND_VARIED );
            TEST_ASSERT_EQUAL_HEX16 ( 0x0007, s_units[ i ].operand_last );
        } else {
            TEST_ASSERT_EQUAL_HEX8 ( 0x4E, s_units[ i ].key[ 2 ] );
        };
    };
}


/** @brief Podmíněná instrukce v detailu: čítače taken a not_taken, next_pc_last. */
void test_detail_branch_counters ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t retz[] = { 0xC8 };
    step ( 0x1000, 0x1234, retz, 1, 0x40, 0 );
    step ( 0x1000, 0x1234, retz, 1, 0x40, 0 );
    step ( 0x1000, 0x1001, retz, 1, 0x00, 0 );
    TEST_ASSERT_EQUAL_UINT32 ( 1, coverage_copy_units ( s_units, 64 ) );
    TEST_ASSERT_EQUAL_UINT32 ( 3, s_units[ 0 ].exec );
    TEST_ASSERT_EQUAL_UINT32 ( 2, s_units[ 0 ].taken );
    TEST_ASSERT_EQUAL_UINT32 ( 1, s_units[ 0 ].not_taken );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1001, s_units[ 0 ].next_pc_last );
    TEST_ASSERT_TRUE ( s_units[ 0 ].flags & COVERAGE_F_DECISION );
    TEST_ASSERT_EQUAL_STRING ( "both", coverage_branch_state_name ( true, 2, 1 ) );
}


/** @brief Plná tabulka: nové jednotky se zahodí (dropped), staré se počítají dál. */
void test_detail_capacity_dropped ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 2, false );
    static const uint8_t nop[] = { 0x00 };
    step ( 0x0100, 0x0101, nop, 1, 0, 0 );
    step ( 0x0101, 0x0102, nop, 1, 0, 0 );
    step ( 0x0102, 0x0103, nop, 1, 0, 0 );   /* nevejde se */
    step ( 0x0100, 0x0101, nop, 1, 0, 0 );

    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT32 ( 2, st.units );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.dropped );
    TEST_ASSERT_FALSE ( st.complete );
    TEST_ASSERT_EQUAL_UINT32 ( 2, coverage_copy_units ( s_units, 64 ) );
    const st_COVERAGE_UNIT *u = find_unit ( s_units, 2, 0x0100, 0x00, COVERAGE_CELL ( 0, 0x0100 ) );
    TEST_ASSERT_NOT_NULL ( u );
    TEST_ASSERT_EQUAL_UINT32 ( 2, u->exec );
    /* Bitmapa branch zapisuje i krok bez místa v tabulce. */
    uint8_t exec[ COVERAGE_PC_BITMAP_BYTES ];
    coverage_get_bitmaps ( exec, NULL, NULL, NULL, NULL );
    TEST_ASSERT_EQUAL_HEX8 ( 0x07, exec[ 0x0100 >> 3 ] );
}


/**
 * @brief Saturace: čítač na 0xFFFFFFFF zůstane, jednotka dostane SATURATED,
 *        stav saturated a complete = false.
 */
void test_detail_saturation ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t djnz[] = { 0x10, 0xFE };
    step ( 0x6000, 0x6000, djnz, 2, 0, 1 );      /* skok */
    TEST_ASSERT_EQUAL_UINT32 ( 1, s_cov.used );
    st_COVERAGE_UNIT *u = NULL;
    for ( uint32_t i = 0; i < s_cov.slots; i++ ) {
        if ( s_cov.units[ i ].insn_len ) u = &s_cov.units[ i ];
    };
    TEST_ASSERT_NOT_NULL ( u );
    u->exec = UINT32_MAX - 1;
    u->taken = UINT32_MAX - 1;
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_TRUE ( st.complete );

    step ( 0x6000, 0x6000, djnz, 2, 0, 1 );      /* exec a taken dosáhnou maxima */
    step ( 0x6000, 0x6000, djnz, 2, 0, 1 );      /* zůstanou na maximu */
    TEST_ASSERT_EQUAL_UINT32 ( UINT32_MAX, u->exec );
    TEST_ASSERT_EQUAL_UINT32 ( UINT32_MAX, u->taken );
    TEST_ASSERT_TRUE ( u->flags & COVERAGE_F_SATURATED );
    coverage_get_status ( &st );
    TEST_ASSERT_TRUE ( st.saturated );
    TEST_ASSERT_FALSE ( st.complete );
}


/** @brief Bajt mimo záznam fetch: příznak jednotky, čítač, complete = false. */
void test_detail_fetch_fallback ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t ldhl[] = { 0x21, 0x34, 0x12 };
    step_ex ( 0x5000, 0x5003, ldhl, 3, 0, 0, false, COVERAGE_CELL ( 0, 0x5000 ), 0, 2 );
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.fetch_fallback );
    TEST_ASSERT_FALSE ( st.complete );
    coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_TRUE ( s_units[ 0 ].flags & COVERAGE_F_FETCH_FALLBACK );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1234, s_units[ 0 ].operand_last );
}


/* ===== epochy, nespojitosti, původ ===== */

/** @brief coverage_mark(): nové adresy a jednotky dostanou novou epochu. */
void test_epoch_mark ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t nop[] = { 0x00 };
    step ( 0x0200, 0x0201, nop, 1, 0, 0 );
    TEST_ASSERT_EQUAL_UINT32 ( 1, coverage_mark ( ) );
    step ( 0x0200, 0x0201, nop, 1, 0, 0 );
    step ( 0x0201, 0x0202, nop, 1, 0, 0 );
    TEST_ASSERT_EQUAL_UINT16 ( 0, coverage_pc_first_epoch ( 0x0200 ) );
    TEST_ASSERT_EQUAL_UINT16 ( 1, coverage_pc_first_epoch ( 0x0201 ) );
    uint32_t n = coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_EQUAL_UINT16 ( 0, find_unit ( s_units, n, 0x0200, 0x00, COVERAGE_CELL ( 0, 0x0200 ) )->first_epoch );
    TEST_ASSERT_EQUAL_UINT16 ( 1, find_unit ( s_units, n, 0x0201, 0x00, COVERAGE_CELL ( 0, 0x0201 ) )->first_epoch );
}


/** @brief Nespojitost: bez volby jen čítač, s reset_on_discontinuity smaže data. */
void test_discontinuity ( void )
{
    static const uint8_t nop[] = { 0x00 };
    start ( COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, false );
    step ( 0x0300, 0x0301, nop, 1, 0, 0 );
    coverage_note_discontinuity ( );
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.discontinuities );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.units );

    start ( COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, true );
    step ( 0x0300, 0x0301, nop, 1, 0, 0 );
    coverage_note_discontinuity ( );
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT32 ( 1, st.discontinuities );
    TEST_ASSERT_EQUAL_UINT32 ( 0, st.units );
    TEST_ASSERT_EQUAL_UINT64 ( 0, st.instructions );
}


/** @brief Start bez resetu se stejnou úrovní pokračuje, se změnou úrovně začne znovu. */
void test_start_continue_or_reset ( void )
{
    static const uint8_t nop[] = { 0x00 };
    start ( COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, false );
    step ( 0x0400, 0x0401, nop, 1, 0, 0 );
    coverage_stop ( );
    st_COVERAGE_CONFIG cfg = { COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, false };
    TEST_ASSERT_TRUE ( coverage_start ( &cfg, false ) );
    st_COVERAGE_STATUS st;
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.instructions );

    cfg.level = COVERAGE_LEVEL_DETAIL;
    cfg.capacity = 8;
    TEST_ASSERT_TRUE ( coverage_start ( &cfg, false ) );
    coverage_get_status ( &st );
    TEST_ASSERT_EQUAL_UINT64 ( 0, st.instructions );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_LEVEL_DETAIL, st.level );
}


/** @brief Příznaky původu: vstup do obsluhy přerušení (bitmapa i jednotka), CMT past. */
void test_step_origin_flags ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t push[] = { 0xF5 };
    static const uint8_t out[] = { 0xD3, 0x01 };
    step_ex ( 0x1038, 0x1039, push, 1, 0, 0, false, COVERAGE_CELL ( 0, 0x1038 ), COVERAGE_F_INT_ENTRY, 255 );
    step_ex ( 0x2000, 0x2002, out, 2, 0, 0, false, COVERAGE_CELL ( 0, 0x2000 ), COVERAGE_F_CMT_TRAP, 255 );
    uint8_t ie[ COVERAGE_PC_BITMAP_BYTES ];
    coverage_get_bitmaps ( NULL, NULL, NULL, NULL, ie );
    TEST_ASSERT_EQUAL_HEX8 ( 0x01, ie[ 0x1038 >> 3 ] );
    uint32_t n = coverage_copy_units ( s_units, 64 );
    TEST_ASSERT_TRUE ( find_unit ( s_units, n, 0x1038, 0xF5, COVERAGE_CELL ( 0, 0x1038 ) )->flags & COVERAGE_F_INT_ENTRY );
    TEST_ASSERT_TRUE ( find_unit ( s_units, n, 0x2000, 0xD3, COVERAGE_CELL ( 0, 0x2000 ) )->flags & COVERAGE_F_CMT_TRAP );
}


/* ===== snímek a filtr (coverage_export.h, MCP i GUI) ===== */

static st_COVERAGE_SNAPSHOT s_snap;


/** @brief Snímek bez jednotek nese bitmapy a stav; stav adresy podle bitmap. */
void test_snapshot_without_units_pc_state ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t jrnz[] = { 0x20, 0x10 };
    static const uint8_t jpz[] = { 0xCA, 0x00, 0x50 };
    static const uint8_t retnc[] = { 0xD0 };
    static const uint8_t nop[] = { 0x00 };
    step ( 0x3000, 0x3012, jrnz, 2, 0x00, 0 );   /* skok */
    step ( 0x3000, 0x3002, jrnz, 2, 0x40, 0 );   /* propad */
    step ( 0x3002, 0x5000, jpz, 3, 0x40, 0 );    /* jen skok */
    step ( 0x3005, 0x3006, retnc, 1, 0x01, 0 );  /* C -> jen propad */
    step ( 0x3006, 0x3007, nop, 1, 0x00, 0 );

    TEST_ASSERT_TRUE ( coverage_snapshot_take ( &s_snap, false ) );
    TEST_ASSERT_NULL ( s_snap.units );
    TEST_ASSERT_EQUAL_UINT32 ( 0, s_snap.n_units );
    TEST_ASSERT_EQUAL_UINT32 ( 4, s_snap.status.units );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_BOTH, coverage_pc_state ( &s_snap, 0x3000 ) );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_TAKEN_ONLY, coverage_pc_state ( &s_snap, 0x3002 ) );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_NOT_TAKEN_ONLY, coverage_pc_state ( &s_snap, 0x3005 ) );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_NONE, coverage_pc_state ( &s_snap, 0x3006 ) );
    TEST_ASSERT_EQUAL_UINT ( 0, coverage_pc_state ( &s_snap, 0x3001 ) );
    TEST_ASSERT_EQUAL_UINT ( 0, coverage_pc_state ( &s_snap, 0x5000 ) );

    TEST_ASSERT_TRUE ( coverage_snapshot_take ( &s_snap, true ) );
    TEST_ASSERT_NOT_NULL ( s_snap.units );
    TEST_ASSERT_EQUAL_UINT32 ( 4, s_snap.n_units );
    coverage_snapshot_clear ( &s_snap );
}


/** @brief Filtr řádků: rozsah, stav, epocha, příznak, prostor buňky. */
void test_row_filter ( void )
{
    start ( COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 16, false );
    static const uint8_t jrnz[] = { 0x20, 0x10 };
    static const uint8_t jpz[] = { 0xCA, 0x00, 0x50 };
    static const uint8_t nop[] = { 0x00 };
    step ( 0x3000, 0x3012, jrnz, 2, 0x00, 0 );
    step ( 0x3002, 0x5000, jpz, 3, 0x40, 0 );
    coverage_mark ( );
    step_ex ( 0x0038, 0x0039, nop, 1, 0, 0, false, COVERAGE_CELL ( 4, 0x0038 ), COVERAGE_F_INT_ENTRY, 255 );

    TEST_ASSERT_TRUE ( coverage_snapshot_take ( &s_snap, true ) );
    size_t n = 0;
    st_COVERAGE_ROW *rows = coverage_rows_build ( &s_snap, &n );
    TEST_ASSERT_EQUAL_size_t ( 3, n );

    st_COVERAGE_FILTER f;
    unsigned hits;
    coverage_filter_init ( &f );
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) hits += coverage_row_match ( &s_snap, &rows[ i ], &f );
    TEST_ASSERT_EQUAL_UINT ( 3, hits );

    /* Neúplné větve (partial). */
    f.state_mask = coverage_state_bits ( "partial" );
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) hits += coverage_row_match ( &s_snap, &rows[ i ], &f );
    TEST_ASSERT_EQUAL_UINT ( 2, hits );

    /* Rozsah 3001h-FFFFh a stav partial: jen JP Z na 3002h. */
    f.pc_from = 0x3001;
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) {
        if ( coverage_row_match ( &s_snap, &rows[ i ], &f ) ) {
            hits++;
            TEST_ASSERT_EQUAL_HEX16 ( 0x3002, rows[ i ].pc );
        };
    };
    TEST_ASSERT_EQUAL_UINT ( 1, hits );

    /* Nové od značky (epocha 1): jen obsluha přerušení. */
    coverage_filter_init ( &f );
    f.has_epoch = true;
    f.epoch_from = 1;
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) {
        if ( coverage_row_match ( &s_snap, &rows[ i ], &f ) ) {
            hits++;
            TEST_ASSERT_EQUAL_HEX16 ( 0x0038, rows[ i ].pc );
        };
    };
    TEST_ASSERT_EQUAL_UINT ( 1, hits );

    /* Příznak a prostor buňky. */
    coverage_filter_init ( &f );
    f.flags_any = COVERAGE_F_INT_ENTRY;
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) hits += coverage_row_match ( &s_snap, &rows[ i ], &f );
    TEST_ASSERT_EQUAL_UINT ( 1, hits );
    coverage_filter_init ( &f );
    f.has_space = true;
    f.space = 4;
    hits = 0;
    for ( size_t i = 0; i < n; i++ ) hits += coverage_row_match ( &s_snap, &rows[ i ], &f );
    TEST_ASSERT_EQUAL_UINT ( 1, hits );

    g_free ( rows );
    coverage_snapshot_clear ( &s_snap );
}


int main ( void )
{
    UNITY_BEGIN ( );

    RUN_TEST ( test_branch_states );
    RUN_TEST ( test_halt_idle_collapse );
    RUN_TEST ( test_detail_versions_and_cells );
    RUN_TEST ( test_detail_full_bytes_key );
    RUN_TEST ( test_detail_ddcb_key );
    RUN_TEST ( test_detail_branch_counters );
    RUN_TEST ( test_detail_capacity_dropped );
    RUN_TEST ( test_detail_saturation );
    RUN_TEST ( test_detail_fetch_fallback );
    RUN_TEST ( test_epoch_mark );
    RUN_TEST ( test_discontinuity );
    RUN_TEST ( test_start_continue_or_reset );
    RUN_TEST ( test_step_origin_flags );
    RUN_TEST ( test_snapshot_without_units_pc_state );
    RUN_TEST ( test_row_filter );

    return UNITY_END ( );
}
