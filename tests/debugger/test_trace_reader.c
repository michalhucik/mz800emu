/*
 * test_trace_reader.c - unit testy čtenáře proudu záznamů (trace_read)
 *
 * Testuje trace_reader.{c,h} nad skutečným tlog writerem (chunky 1 MB
 * v dočasném adresáři), bez emulace:
 *  - kurzor: round-trip, cizí kanál, poškozený řetězec,
 *  - trace_reader_fetch přes hranici chunků na disku a disk/RAM,
 *  - stránka rows: filtr PC, limit, rozpočet znaků (pokračování přesně za
 *    posledním vráceným řádkem), sloupce, symboly přes stub resolver, asm
 *    z bajtů záznamu,
 *  - čas cputrack: kotva chunku + součet t_std + wait, přes stránky
 *    (cache) i přes hranici chunků,
 *  - iorqlog: dekódování a filtr portu, raw_b64: bajtová shoda,
 *  - cputrack v3: druh záznamu, registry po / před (i přes hranici
 *    disk/RAM), filtr podle registru, synchronizační záznam bez času,
 *    příznak neznámého stavu před; sloupec kind u v2.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "debugger/trace/tlog_common.h"
#include "debugger/trace/trace_reader.h"
#include "debugger/trace/cputrack.h"

static char *s_dir = NULL;

void setUp ( void )
{
    s_dir = g_dir_make_tmp ( "mztest-treader-XXXXXX", NULL );
    TEST_ASSERT_NOT_NULL ( s_dir );
    trace_reader_time_cache_reset ( );
}

/** @brief Smazat soubory adresáře a adresář (jen ploché). */
static void rm_dir ( const char *path )
{
    GDir *d = g_dir_open ( path, 0, NULL );
    if ( d ) {
        const gchar *n;
        while ( ( n = g_dir_read_name ( d ) ) != NULL ) {
            char *c = g_build_filename ( path, n, NULL );
            g_remove ( c );
            g_free ( c );
        }
        g_dir_close ( d );
    }
    g_rmdir ( path );
}

void tearDown ( void )
{
    if ( s_dir ) rm_dir ( s_dir );
    g_free ( s_dir );
    s_dir = NULL;
}

/** @brief Počet 12B záznamů v 1 MB chunku. */
#define CT_PER_CHUNK 87381u

/**
 * @brief Záznam cputrack v2: pc = 0x1000 + (i % 0x100), LD A,n (3E i),
 *        t_std 7, wait = i % 3.
 */
static void ct_rec ( uint8_t r[ 12 ], uint32_t i )
{
    uint16_t pc = (uint16_t) ( 0x1000 + ( i % 0x100 ) );
    memset ( r, 0, 12 );
    r[ 0 ] = pc & 0xFF;
    r[ 1 ] = pc >> 8;
    r[ 2 ] = 2;
    r[ 3 ] = 0x3E;
    r[ 4 ] = (uint8_t) i;
    r[ 7 ] = (uint8_t) ( i % 3 );
    r[ 11 ] = 7;
}

/** @brief T-stavy záznamu ct_rec(i). */
static uint64_t ct_t ( uint32_t i ) { return 7 + ( i % 3 ); }

/** @brief Zapsat n záznamů cputrack do writeru (otevřeného). */
static void ct_fill ( st_TLOG_WRITER *w, uint32_t n )
{
    uint8_t r[ 12 ];
    for ( uint32_t i = 0; i < n; i++ ) {
        ct_rec ( r, i );
        TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( w, r, 12 ) );
    }
}

/** @brief Stránka rows; vrací objekt odpovědi (g_object_unref / json_object_unref). */
static JsonObject *page ( const st_TRACE_PAGE_REQ *req, const st_TLOG_READ_SNAPSHOT *snap,
                          trace_resolve_fn res, uint64_t *next )
{
    JsonObject *o = json_object_new ( );
    TEST_ASSERT_EQUAL_INT ( 0, trace_reader_page ( req, snap, res, NULL, o, next ) );
    return o;
}

/** @brief Výchozí požadavek na stránku. */
static st_TRACE_PAGE_REQ req_default ( en_TRACE_KIND kind )
{
    st_TRACE_PAGE_REQ r;
    memset ( &r, 0, sizeof ( r ) );
    r.kind = kind;
    r.limit = TRACE_READ_DEFAULT_LIMIT;
    const char *bad = NULL;
    r.col_mask = trace_columns_mask ( kind, NULL, 0, &bad );
    trace_filter_init ( &r.filter );
    r.max_chars = TRACE_READ_MAX_MAX_CHARS;
    r.max_scan = TRACE_READ_DEFAULT_MAX_SCAN;
    r.cpu_divider = 5;
    return r;
}

/* ================================================================ */

void test_cursor_roundtrip_and_rejects ( void )
{
    char *c = trace_cursor_encode ( "cputrack", 42, G_GUINT64_CONSTANT ( 123456789012 ) );
    unsigned seg = 0;
    uint64_t seq = 0;
    TEST_ASSERT_TRUE ( trace_cursor_decode ( c, "cputrack", &seg, &seq ) );
    TEST_ASSERT_EQUAL_UINT ( 42, seg );
    TEST_ASSERT_EQUAL_UINT64 ( G_GUINT64_CONSTANT ( 123456789012 ), seq );
    TEST_ASSERT_FALSE_MESSAGE ( trace_cursor_decode ( c, "iorqlog", &seg, &seq ), "cizí kanál" );
    TEST_ASSERT_NULL ( strchr ( c, '=' ) );
    g_free ( c );
    TEST_ASSERT_FALSE ( trace_cursor_decode ( "t1.!!!", "cputrack", &seg, &seq ) );
    TEST_ASSERT_FALSE ( trace_cursor_decode ( "xx", "cputrack", &seg, &seq ) );
    TEST_ASSERT_FALSE ( trace_cursor_decode ( NULL, "cputrack", &seg, &seq ) );
}

/* fetch přes hranici chunků (uzavřený segment) a disk/RAM (živý). */
void test_fetch_across_chunks_and_ram ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    ct_fill ( &w, CT_PER_CHUNK * 2 + 50 );   /* 2 chunky na disku + 50 v RAM */

    st_TLOG_READ_SNAPSHOT snap;
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_read_snapshot ( &w, CT_PER_CHUNK * 2 - 10, 60, 1u << 20, &snap ) );
    uint8_t *buf = g_malloc ( 60 * 12 );
    uint64_t got = 0;
    /* přes hranici chunk 0/1 */
    TEST_ASSERT_EQUAL_INT ( 0, trace_reader_fetch ( &snap, CT_PER_CHUNK - 5, 10, buf, &got ) );
    TEST_ASSERT_EQUAL_UINT64 ( 10, got );
    for ( uint32_t k = 0; k < 10; k++ ) TEST_ASSERT_EQUAL_HEX8 ( (uint8_t) ( CT_PER_CHUNK - 5 + k ), buf[ k * 12 + 4 ] );
    /* přes hranici disk/RAM */
    TEST_ASSERT_EQUAL_INT ( 0, trace_reader_fetch ( &snap, CT_PER_CHUNK * 2 - 10, 60, buf, &got ) );
    TEST_ASSERT_EQUAL_UINT64 ( 60, got );
    for ( uint32_t k = 0; k < 60; k++ ) TEST_ASSERT_EQUAL_HEX8 ( (uint8_t) ( CT_PER_CHUNK * 2 - 10 + k ), buf[ k * 12 + 4 ] );
    /* RAM mimo kopii: čtení skončí */
    tlog_read_snapshot_clear ( &snap );
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );
    TEST_ASSERT_EQUAL_INT ( 0, trace_reader_fetch ( &snap, CT_PER_CHUNK * 2 - 2, 10, buf, &got ) );
    TEST_ASSERT_EQUAL_UINT64_MESSAGE ( 2, got, "bez kopie RAM se čte jen část na disku" );
    tlog_read_snapshot_clear ( &snap );
    g_free ( buf );
    tlog_writer_close ( &w, 0, 0, 0 );
}

/** @brief Stub resolveru: symbol "SYM_<adresa>" s offsetem 1, značka "M<id>", text "EV<cat>". */
static bool stub_resolve ( void *user, st_TRACE_RESOLVE_REQ *rr )
{
    (void) user;
    for ( size_t i = 0; i < rr->n_addrs; i++ ) {
        g_snprintf ( rr->syms + i * TRACE_RESOLVE_NAME_SIZE, TRACE_RESOLVE_NAME_SIZE, "SYM_%04X", rr->addrs[ i ] );
        rr->sym_offs[ i ] = 1;
    }
    for ( size_t i = 0; i < rr->n_markers; i++ ) {
        g_snprintf ( rr->marker_names + i * TRACE_RESOLVE_NAME_SIZE, TRACE_RESOLVE_NAME_SIZE, "M%u", rr->markers[ i ] );
    }
    for ( size_t i = 0; i < rr->n_events; i++ ) {
        g_snprintf ( rr->event_texts + i * TRACE_RESOLVE_TEXT_SIZE, TRACE_RESOLVE_TEXT_SIZE, "EV%u",
                     rr->events[ i * 32 + 16 ] );
    }
    return true;
}

/* rows: filtr, limit, sloupce, asm, symboly, pokračování po rozpočtu znaků. */
void test_page_rows_filter_budget_and_symbols ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    ct_fill ( &w, 2000 );
    tlog_writer_close ( &w, 0, 0, 0 );
    st_TLOG_READ_SNAPSHOT snap;
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );

    /* filtr PC 1010h-1011h: v každém cyklu 256 záznamů 2 shody */
    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_CPUTRACK );
    r.filter.has_pc = true;
    r.filter.pc_from = 0x1010;
    r.filter.pc_to = 0x1011;
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, stub_resolve, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    /* shody i % 256 == 10h, 11h pro i < 2000: 8 cyklů (poslední 1808, 1809) */
    TEST_ASSERT_EQUAL_UINT ( 16, json_array_get_length ( rows ) );
    JsonArray *row0 = json_array_get_array_element ( rows, 0 );
    TEST_ASSERT_EQUAL_INT64 ( 0x10, json_array_get_int_element ( row0, 0 ) );
    TEST_ASSERT_EQUAL_STRING ( "1010", json_array_get_string_element ( row0, 1 ) );
    TEST_ASSERT_EQUAL_STRING ( "3E 10", json_array_get_string_element ( row0, 2 ) );
    TEST_ASSERT_TRUE_MESSAGE ( g_str_has_prefix ( json_array_get_string_element ( row0, 3 ), "LD A," ),
                               "asm z bajtů záznamu" );
    TEST_ASSERT_EQUAL_STRING ( "SYM_1010+1", json_array_get_string_element ( row0, 7 ) );
    TEST_ASSERT_TRUE ( json_object_get_boolean_member ( o, "end_of_data" ) );
    TEST_ASSERT_EQUAL_UINT64 ( 2000, next );
    json_object_unref ( o );

    /* sloupce a malý rozpočet: stránky navazují bez mezer a duplicit */
    const char *names[] = { "seq", "pc" };
    const char *bad = NULL;
    r = req_default ( TRACE_KIND_CPUTRACK );
    r.col_mask = trace_columns_mask ( TRACE_KIND_CPUTRACK, names, 2, &bad );
    TEST_ASSERT_NULL ( bad );
    r.max_chars = 1200;      /* hlavička 900 + pár řádků */
    r.limit = 5000;
    uint64_t expect = 0;
    unsigned pages = 0;
    while ( expect < 2000 && pages < 1000 ) {
        o = page ( &r, &snap, NULL, &next );
        rows = json_object_get_array_member ( o, "rows" );
        TEST_ASSERT_TRUE ( json_array_get_length ( rows ) > 0 );
        TEST_ASSERT_EQUAL_UINT ( 2, json_array_get_length ( json_object_get_array_member ( o, "columns" ) ) );
        for ( guint k = 0; k < json_array_get_length ( rows ); k++ ) {
            JsonArray *rw = json_array_get_array_element ( rows, k );
            TEST_ASSERT_EQUAL_UINT ( 2, json_array_get_length ( rw ) );
            TEST_ASSERT_EQUAL_INT64 ( (gint64) expect, json_array_get_int_element ( rw, 0 ) );
            expect++;
        }
        TEST_ASSERT_EQUAL_UINT64 ( expect, next );
        json_object_unref ( o );
        r.from_seq = next;
        pages++;
    }
    TEST_ASSERT_EQUAL_UINT64 ( 2000, expect );
    TEST_ASSERT_TRUE ( pages > 1 );

    /* neznámý sloupec */
    const char *wrong[] = { "nope" };
    TEST_ASSERT_EQUAL_UINT32 ( 0, trace_columns_mask ( TRACE_KIND_CPUTRACK, wrong, 1, &bad ) );
    TEST_ASSERT_EQUAL_STRING ( "nope", bad );
    tlog_read_snapshot_clear ( &snap );
}

/** @brief Očekávaný čas záznamu seq v chunku s kotvou anchor_px (5 taktů GDG na T). */
static uint64_t expect_t ( uint64_t anchor_px, uint32_t chunk_start, uint32_t seq )
{
    uint64_t sum = 0;
    for ( uint32_t i = chunk_start; i < seq; i++ ) sum += ct_t ( i );
    return anchor_px + sum * 5;
}

/* Čas cputrack: první stránka mid-chunk, navazující stránky (cache),
 * přechod přes hranici chunků a do RAM. */
void test_page_cputrack_time ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct", 1, 0, 1000, 200, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    ct_fill ( &w, CT_PER_CHUNK + 300 );   /* chunk 0 na disku, 300 v RAM */

    st_TLOG_READ_SNAPSHOT snap;
    uint64_t from = CT_PER_CHUNK - 100;
    tlog_writer_read_snapshot ( &w, from, 400, 1u << 20, &snap );
    uint64_t a0 = g_array_index ( snap.chunks, st_TLOG_CHUNK_META, 0 ).start_pxclk;
    TEST_ASSERT_EQUAL_UINT64 ( 1000, a0 );

    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_CPUTRACK );
    r.cputrack_time = true;
    r.from_seq = from;
    r.limit = 50;
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, NULL, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 50, json_array_get_length ( rows ) );
    for ( guint k = 0; k < 50; k++ ) {
        JsonArray *rw = json_array_get_array_element ( rows, k );
        TEST_ASSERT_EQUAL_UINT64 ( expect_t ( a0, 0, (uint32_t) ( from + k ) ),
                                   (uint64_t) json_array_get_int_element ( rw, 6 ) );
    }
    json_object_unref ( o );

    /* Navazující stránka přes hranici disk/RAM: kotva RAM chunku. */
    r.from_seq = next;
    r.limit = 100;
    tlog_read_snapshot_clear ( &snap );
    tlog_writer_read_snapshot ( &w, next, 100, 1u << 20, &snap );
    r.ram_prefix_t = 0;   /* kopie začíná na ram_first_seq */
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 100, json_array_get_length ( rows ) );
    for ( guint k = 0; k < 100; k++ ) {
        JsonArray *rw = json_array_get_array_element ( rows, k );
        uint64_t s = (uint64_t) json_array_get_int_element ( rw, 0 );
        uint64_t t = (uint64_t) json_array_get_int_element ( rw, 6 );
        if ( s < CT_PER_CHUNK ) TEST_ASSERT_EQUAL_UINT64 ( expect_t ( a0, 0, (uint32_t) s ), t );
        else TEST_ASSERT_EQUAL_UINT64 ( expect_t ( snap.ram_start_pxclk, CT_PER_CHUNK, (uint32_t) s ), t );
    }
    json_object_unref ( o );

    /* Bez času: sloupec t je null. */
    r = req_default ( TRACE_KIND_CPUTRACK );
    r.limit = 1;
    o = page ( &r, &snap, NULL, &next );
    JsonArray *rw = json_array_get_array_element ( json_object_get_array_member ( o, "rows" ), 0 );
    TEST_ASSERT_TRUE ( JSON_NODE_HOLDS_NULL ( json_array_get_element ( rw, 6 ) ) );
    json_object_unref ( o );

    tlog_read_snapshot_clear ( &snap );
    tlog_writer_close ( &w, 0, 0, 0 );
}

/** @brief Počet 32B záznamů (v3) v 1 MB chunku. */
#define CT3_PER_CHUNK 32768u

/**
 * @brief Záznam cputrack v3: jako ct_rec, HL po instrukci = i; seq 0 je
 *        synchronizace START, seq 10 synchronizace WRITE (bez času), seq 20
 *        má příznak neznámého stavu před.
 */
static void ct3_rec ( uint8_t r[ 32 ], uint32_t i )
{
    memset ( r, 0, 32 );
    ct_rec ( r, i );
    r[ 18 ] = (uint8_t) i;                /* HL (offset 12 + 3 * 2) */
    r[ 19 ] = (uint8_t) ( i >> 8 );
    r[ 30 ] = CPUTRACK_REGS_F_IFF1 | ( 1u << CPUTRACK_REGS_F_IM_SHIFT );
    r[ 31 ] = CPUTRACK_KIND_INSN;
    if ( i == 0 || i == 10 ) {
        r[ 2 ] = CPUTRACK_INSN_LEN_SYNC;
        for ( unsigned k = 3; k <= 10; k++ ) r[ k ] = 0xA0 + k;   /* alternativní sada, ne čas */
        r[ 11 ] = 0;
        r[ 31 ] = CPUTRACK_KIND_SYNC | ( ( i == 0 ? CPUTRACK_SYNC_START : CPUTRACK_SYNC_WRITE )
                                         << CPUTRACK_SYNC_REASON_SHIFT );
    } else if ( i == 20 ) {
        r[ 31 ] = CPUTRACK_KIND_INSN | CPUTRACK_FLAG_NO_BEFORE;
    }
}

/** @brief T-stavy záznamu ct3_rec(i) (synchronizace 0). */
static uint64_t ct3_t ( uint32_t i ) { return ( i == 0 || i == 10 ) ? 0 : ct_t ( i ); }

/** @brief Zapsat n záznamů v3. */
static void ct3_fill ( st_TLOG_WRITER *w, uint32_t n )
{
    uint8_t r[ 32 ];
    for ( uint32_t i = 0; i < n; i++ ) {
        ct3_rec ( r, i );
        TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( w, r, 32 ) );
    }
}

/** @brief Maska sloupců cputrack podle jmen. */
static uint32_t ct_cols ( const char *const *names, size_t n )
{
    const char *bad = NULL;
    uint32_t m = trace_columns_mask ( TRACE_KIND_CPUTRACK, names, n, &bad );
    TEST_ASSERT_NULL ( bad );
    return m;
}

/* cputrack v3: druh, registry po / před (i přes hranici disk/RAM), filtr
 * podle registru, synchronizace bez času, příznak neznámého stavu před. */
void test_page_cputrack_v3_regs ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct3", 1, 0, 1000, 0, 0 );
    tlog_writer_set_record_size ( &w, 32 );
    ct3_fill ( &w, CT3_PER_CHUNK + 100 );   /* chunk 0 na disku, 100 v RAM */

    /* Výchozí sloupce bez registrů a druhu (jako v2). */
    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_CPUTRACK );
    TEST_ASSERT_EQUAL_UINT32 ( 0, r.col_mask & TRACE_CPUTRACK_REG_COLS_MASK );
    TEST_ASSERT_EQUAL_INT ( TRACE_CPUTRACK_REG_HL, trace_cputrack_reg_index ( "hl" ) );
    TEST_ASSERT_EQUAL_INT ( -1, trace_cputrack_reg_index ( "seq" ) );

    st_TLOG_READ_SNAPSHOT snap;
    tlog_writer_read_snapshot ( &w, 0, 64, 1u << 20, &snap );
    const char *names[] = { "seq", "asm", "t_std", "wait", "t", "kind", "hl", "iff1", "im" };
    r.col_mask = ct_cols ( names, G_N_ELEMENTS ( names ) );
    r.cputrack_time = true;
    r.limit = 25;
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, NULL, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 25, json_array_get_length ( rows ) );
    JsonArray *s0 = json_array_get_array_element ( rows, 0 );
    TEST_ASSERT_EQUAL_STRING ( "SYNC start", json_array_get_string_element ( s0, 1 ) );
    TEST_ASSERT_EQUAL_INT64 ( 0, json_array_get_int_element ( s0, 3 ) );
    TEST_ASSERT_EQUAL_STRING ( "sync", json_array_get_string_element ( s0, 5 ) );
    JsonArray *s10 = json_array_get_array_element ( rows, 10 );
    TEST_ASSERT_EQUAL_STRING ( "SYNC write", json_array_get_string_element ( s10, 1 ) );
    JsonArray *r5 = json_array_get_array_element ( rows, 5 );
    TEST_ASSERT_EQUAL_STRING ( "insn", json_array_get_string_element ( r5, 5 ) );
    TEST_ASSERT_EQUAL_STRING ( "0005", json_array_get_string_element ( r5, 6 ) );
    TEST_ASSERT_EQUAL_INT64 ( 1, json_array_get_int_element ( r5, 7 ) );
    TEST_ASSERT_EQUAL_INT64 ( 1, json_array_get_int_element ( r5, 8 ) );
    /* čas: synchronizační záznamy se nezapočítají */
    for ( guint k = 0; k < 25; k++ ) {
        uint64_t sum = 0;
        for ( uint32_t i = 0; i < k; i++ ) sum += ct3_t ( i );
        JsonArray *rw = json_array_get_array_element ( rows, k );
        TEST_ASSERT_EQUAL_UINT64 ( 1000 + sum * 5, (uint64_t) json_array_get_int_element ( rw, 4 ) );
    }
    json_object_unref ( o );

    /* Stav před: řádek 5 nese HL ze záznamu 4, záznam 0 a 20 nemá stav před. */
    r.regs_before = true;
    r.cputrack_time = false;
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_TRUE ( JSON_NODE_HOLDS_NULL ( json_array_get_element ( json_array_get_array_element ( rows, 0 ), 6 ) ) );
    TEST_ASSERT_EQUAL_STRING ( "0004", json_array_get_string_element ( json_array_get_array_element ( rows, 5 ), 6 ) );
    TEST_ASSERT_TRUE ( JSON_NODE_HOLDS_NULL ( json_array_get_element ( json_array_get_array_element ( rows, 20 ), 6 ) ) );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );

    /* Stav před přes hranici disk/RAM: první řádek v RAM, předchozí na disku. */
    tlog_writer_read_snapshot ( &w, CT3_PER_CHUNK, 10, 1u << 20, &snap );
    r.from_seq = CT3_PER_CHUNK;
    r.limit = 3;
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_STRING ( "7FFF", json_array_get_string_element ( json_array_get_array_element ( rows, 0 ), 6 ) );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
    /* ... a uvnitř RAM, když kopie začíná o záznam dřív (jako dispatch). */
    tlog_writer_read_snapshot ( &w, CT3_PER_CHUNK + 4, 10, 1u << 20, &snap );
    r.from_seq = CT3_PER_CHUNK + 5;
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_STRING ( "8004", json_array_get_string_element ( json_array_get_array_element ( rows, 0 ), 6 ) );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );

    /* Filtr podle registru: HL = 0123h po záznamu = seq 123h, před = seq 124h. */
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );
    r = req_default ( TRACE_KIND_CPUTRACK );
    r.filter.n_regs = 1;
    r.filter.regs[ 0 ].reg = TRACE_CPUTRACK_REG_HL;
    r.filter.regs[ 0 ].lo = 0x0123;
    r.filter.regs[ 0 ].hi = 0x0123;
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 1, json_array_get_length ( rows ) );
    TEST_ASSERT_EQUAL_INT64 ( 0x123, json_array_get_int_element ( json_array_get_array_element ( rows, 0 ), 0 ) );
    json_object_unref ( o );
    r.regs_before = true;
    o = page ( &r, &snap, NULL, &next );
    rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 1, json_array_get_length ( rows ) );
    TEST_ASSERT_EQUAL_INT64 ( 0x124, json_array_get_int_element ( json_array_get_array_element ( rows, 0 ), 0 ) );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
    tlog_writer_close ( &w, 0, 0, 0 );
}

/* Sloupec kind u v2: přijetí přerušení (délka 0), sbalená smyčka (t_std 0), instrukce. */
void test_page_cputrack_v2_kind ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    uint8_t rec[ 12 ];
    ct_rec ( rec, 0 );
    tlog_writer_append ( &w, rec, 12 );
    rec[ 11 ] = 0;                                   /* sbalená smyčka */
    tlog_writer_append ( &w, rec, 12 );
    memset ( rec, 0, 12 );
    rec[ 0 ] = 0x38; rec[ 11 ] = 13;                 /* přijetí přerušení */
    tlog_writer_append ( &w, rec, 12 );
    tlog_writer_close ( &w, 0, 0, 0 );
    st_TLOG_READ_SNAPSHOT snap;
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );
    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_CPUTRACK );
    const char *names[] = { "kind", "hl" };
    r.col_mask = ct_cols ( names, 2 );
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, NULL, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_STRING ( "insn", json_array_get_string_element ( json_array_get_array_element ( rows, 0 ), 0 ) );
    TEST_ASSERT_EQUAL_STRING ( "loop", json_array_get_string_element ( json_array_get_array_element ( rows, 1 ), 0 ) );
    TEST_ASSERT_EQUAL_STRING ( "int", json_array_get_string_element ( json_array_get_array_element ( rows, 2 ), 0 ) );
    TEST_ASSERT_TRUE_MESSAGE ( JSON_NODE_HOLDS_NULL ( json_array_get_element ( json_array_get_array_element ( rows, 0 ), 1 ) ),
                               "v2 nemá registry" );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
}

/** @brief Záznam iorqlog: t = 100 * i, port 10h + (i % 4), OUT pro sudé i. */
static void io_rec ( uint8_t r[ 24 ], uint32_t i )
{
    memset ( r, 0, 24 );
    uint64_t t = 100u * i;
    memcpy ( r, &t, 8 );
    r[ 16 ] = 0;
    r[ 17 ] = ( i % 2 ) ? 0 : 1;
    r[ 18 ] = 0x34; r[ 19 ] = 0x12;
    r[ 20 ] = (uint8_t) ( 0x10 + ( i % 4 ) );
    r[ 22 ] = (uint8_t) i;
}

/* iorqlog: dekódování, filtr port + dir + čas; raw_b64 = bajty souboru. */
void test_page_iorqlog_and_raw ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "iorqlog", s_dir, "io", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 24 );
    uint8_t r[ 24 ];
    for ( uint32_t i = 0; i < 1000; i++ ) {
        io_rec ( r, i );
        tlog_writer_append ( &w, r, 24 );
    }
    tlog_writer_close ( &w, 0, 0, 0 );
    st_TLOG_READ_SNAPSHOT snap;
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );

    st_TRACE_PAGE_REQ q = req_default ( TRACE_KIND_IORQLOG );
    q.filter.has_port = true;
    q.filter.port = 0x12;      /* i % 4 == 2 -> sudé -> out */
    q.filter.dir = 1;
    q.filter.has_t = true;
    q.filter.t_from = 0;
    q.filter.t_to = 100u * 99;
    uint64_t next = 0;
    JsonObject *o = page ( &q, &snap, NULL, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 25, json_array_get_length ( rows ) );
    JsonArray *r0 = json_array_get_array_element ( rows, 0 );
    TEST_ASSERT_EQUAL_INT64 ( 2, json_array_get_int_element ( r0, 0 ) );
    TEST_ASSERT_EQUAL_INT64 ( 200, json_array_get_int_element ( r0, 1 ) );
    TEST_ASSERT_EQUAL_STRING ( "iorq", json_array_get_string_element ( r0, 4 ) );
    TEST_ASSERT_EQUAL_STRING ( "out", json_array_get_string_element ( r0, 5 ) );
    TEST_ASSERT_EQUAL_STRING ( "1234", json_array_get_string_element ( r0, 6 ) );
    TEST_ASSERT_EQUAL_STRING ( "0012", json_array_get_string_element ( r0, 7 ) );
    TEST_ASSERT_EQUAL_STRING ( "02", json_array_get_string_element ( r0, 8 ) );
    json_object_unref ( o );

    /* raw: 1000 záznamů = soubor io.000.bin bajt po bajtu */
    q = req_default ( TRACE_KIND_IORQLOG );
    q.raw = true;
    q.limit = 1000;
    o = page ( &q, &snap, NULL, &next );
    gsize len = 0;
    guchar *data = g_base64_decode ( json_object_get_string_member ( o, "data_b64" ), &len );
    char *path = g_build_filename ( s_dir, "io.000.bin", NULL );
    gchar *file = NULL;
    gsize flen = 0;
    TEST_ASSERT_TRUE ( g_file_get_contents ( path, &file, &flen, NULL ) );
    TEST_ASSERT_EQUAL_size_t ( flen, len );
    TEST_ASSERT_EQUAL_MEMORY ( file, data, len );
    TEST_ASSERT_EQUAL_UINT64 ( 1000, next );
    g_free ( file );
    g_free ( path );
    g_free ( data );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
}

/* max_scan: sken skončí a pokračuje kurzorem. */
void test_page_max_scan ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_dir, "ct", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    ct_fill ( &w, 3000 );
    tlog_writer_close ( &w, 0, 0, 0 );
    st_TLOG_READ_SNAPSHOT snap;
    tlog_writer_read_snapshot ( &w, 0, 0, 0, &snap );
    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_CPUTRACK );
    r.filter.has_pc = true;
    r.filter.pc_from = 0xF000;   /* nic nevyhoví */
    r.filter.pc_to = 0xF000;
    r.max_scan = 1000;
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, NULL, &next );
    TEST_ASSERT_EQUAL_UINT ( 0, json_array_get_length ( json_object_get_array_member ( o, "rows" ) ) );
    TEST_ASSERT_TRUE ( json_object_get_boolean_member ( o, "scan_exhausted" ) );
    TEST_ASSERT_FALSE ( json_object_get_boolean_member ( o, "end_of_data" ) );
    TEST_ASSERT_EQUAL_UINT64 ( 1000, next );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
}

/* eventlog: 32B záznamy jen v kopii RAM od first_seq, kategorie a text z resolveru. */
void test_page_eventlog ( void )
{
    st_TLOG_READ_SNAPSHOT snap;
    memset ( &snap, 0, sizeof ( snap ) );
    snap.segment = 5;
    snap.live = 1;
    snap.record_size = 32;
    snap.first_seq = 100;
    snap.next_seq = 110;
    snap.ram_first_seq = 100;
    snap.chunks = g_array_new ( FALSE, FALSE, sizeof ( st_TLOG_CHUNK_META ) );
    snap.ram_copy = g_malloc0 ( 10 * 32 );
    snap.ram_copy_seq = 100;
    snap.ram_copy_count = 10;
    for ( unsigned i = 0; i < 10; i++ ) {
        uint8_t *r = snap.ram_copy + i * 32;
        uint64_t t = 1000u + i;
        memcpy ( r, &t, 8 );
        r[ 16 ] = 4;            /* iorq_out */
        r[ 17 ] = 1;
        r[ 18 ] = 0x34; r[ 19 ] = 0x12;
    }
    st_TRACE_PAGE_REQ r = req_default ( TRACE_KIND_EVENTLOG );
    r.from_seq = 100;
    uint64_t next = 0;
    JsonObject *o = page ( &r, &snap, stub_resolve, &next );
    JsonArray *rows = json_object_get_array_member ( o, "rows" );
    TEST_ASSERT_EQUAL_UINT ( 10, json_array_get_length ( rows ) );
    JsonArray *r0 = json_array_get_array_element ( rows, 0 );
    TEST_ASSERT_EQUAL_INT64 ( 100, json_array_get_int_element ( r0, 0 ) );
    TEST_ASSERT_EQUAL_INT64 ( 1000, json_array_get_int_element ( r0, 1 ) );
    TEST_ASSERT_EQUAL_STRING ( "iorq_out", json_array_get_string_element ( r0, 4 ) );
    TEST_ASSERT_EQUAL_STRING ( "1234", json_array_get_string_element ( r0, 6 ) );
    TEST_ASSERT_EQUAL_STRING ( "EV4", json_array_get_string_element ( r0, 7 ) );
    TEST_ASSERT_EQUAL_UINT64 ( 110, next );
    json_object_unref ( o );
    tlog_read_snapshot_clear ( &snap );
}

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );
    UNITY_BEGIN ( );
    RUN_TEST ( test_cursor_roundtrip_and_rejects );
    RUN_TEST ( test_fetch_across_chunks_and_ram );
    RUN_TEST ( test_page_rows_filter_budget_and_symbols );
    RUN_TEST ( test_page_cputrack_time );
    RUN_TEST ( test_page_cputrack_v3_regs );
    RUN_TEST ( test_page_cputrack_v2_kind );
    RUN_TEST ( test_page_iorqlog_and_raw );
    RUN_TEST ( test_page_max_scan );
    RUN_TEST ( test_page_eventlog );
    int result = UNITY_END ( );
    mztest_teardown ( );
    return result;
}
