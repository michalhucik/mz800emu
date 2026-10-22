/**
 * @file test_coverage_window_queue.c
 * @brief Data pokrytí pro okna GUI (dbg_coverage_data) jdou přes CMDRQ
 *        frontu (DBGAPI_CMD_COVERAGE), ne přímo z UI vlákna.
 *
 * Záložka Coverage okna CPU Profiler a značky v oknech Disassembly čtou
 * pokrytí z kopie, kterou si UI vlákno vyžádá přes helper dbg_ui_coverage
 * (dbgapi_helpers.cpp); start, stop a značku vykoná emu vlákno.
 *
 * Ověřuje se přes skutečný modul dbg_coverage_data.cpp:
 *  - bez vlákna, které frontu obsluhuje, se start neprovede (chyba
 *    "did not respond", sběr neběží),
 *  - se simulovaným emu vláknem projde start, lehká kopie (stav adresy
 *    pro značky, bez jednotek), plná kopie (řádky, řádky jedné adresy),
 *    omezení četnosti obnovy, značka, export JSON a stop.
 *
 * Kroky CPU se zapisují přímo jádrem pokrytí (coverage_on_step), vždy jen
 * když simulované emu vlákno neběží.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/coverage/coverage.h"
#include "debugger/coverage/coverage_export.h"
#include "../../src/ui-imgui/debugger/sections/dbg_coverage_data.h"


/** @brief Stav simulovaného emu vlákna (vzor test_ramlog_window_queue.c). */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint coverage_seen; /**< Počet vyzvednutých DBGAPI_CMD_COVERAGE. */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna: drain fronty jako mzarch drain.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread ( gpointer user_data )
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *) user_data;
    while ( !g_atomic_int_get ( &fe->stop ) ) {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue );
        if ( !rq ) {
            (void) dbgapi_emu_wait_for_cmd ( &g_dbgapi_cmdrq_queue, 5 );
            continue;
        };
        if ( ( rq->cmd & DBGAPI_CMD_MASK ) == DBGAPI_CMD_COVERAGE ) {
            g_atomic_int_inc ( &fe->coverage_seen );
        };
        dbgapi_emu_dispatch ( rq );
        dbgapi_emu_complete ( rq );
    };
    return NULL;
}


/** @brief Bajty jednoho kroku pro jádro pokrytí. */
typedef struct {
    const uint8_t *bytes; /**< Bajty kroku. */
    unsigned n;           /**< Počet bajtů. */
} st_TSTEP;


/** @brief Čtení bajtu kroku (coverage_decode_read_fn). */
static uint8_t t_read ( void *ctx, unsigned index )
{
    const st_TSTEP *c = (const st_TSTEP *) ctx;
    return ( index < c->n ) ? c->bytes[ index ] : 0x00;
}


/** @brief Zapíše krok v RAM (emu vlákno neběží). */
static void step ( uint16_t pc, uint16_t next_pc, const uint8_t *bytes, unsigned n, uint8_t f )
{
    st_TSTEP c = { bytes, n };
    st_COVERAGE_STEP s;
    memset ( &s, 0, sizeof ( s ) );
    s.pc = pc;
    s.next_pc = next_pc;
    s.f = f;
    s.cell = COVERAGE_CELL ( 0, pc );
    s.read = t_read;
    s.read_ctx = &c;
    coverage_on_step ( &s );
}


/** @brief Spustí simulované emu vlákno. */
static GThread *emu_start ( st_FAKE_EMU *fe )
{
    g_atomic_int_set ( &fe->stop, 0 );
    return g_thread_new ( "fake-emu", fake_emu_thread, fe );
}


/** @brief Zastaví simulované emu vlákno. */
static void emu_stop ( st_FAKE_EMU *fe, GThread *t )
{
    g_atomic_int_set ( &fe->stop, 1 );
    g_thread_join ( t );
}


void setUp ( void )
{
    dbgapi_init ( &g_dbgapi_cmdrq_queue );
    dbg_cov_data_invalidate ( );
}

void tearDown ( void )
{
    coverage_free ( );
    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/**
 * @brief Bez emu vlákna se start neprovede (timeout, sběr neběží).
 */
void test_start_not_applied_without_emu_thread ( void )
{
    st_COVERAGE_CONFIG cfg = { COVERAGE_LEVEL_BRANCH, COVERAGE_KEY_OPCODE, 0, false };
    char err[ 128 ];
    TEST_ASSERT_FALSE ( dbg_cov_data_op ( DBGAPI_COVERAGE_OP_START, &cfg, false, err, sizeof ( err ) ) );
    TEST_ASSERT_TRUE ( err[ 0 ] != '\0' );
    TEST_ASSERT_EQUAL_INT ( 0, g_coverage_active );
    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Se simulovaným emu vláknem: start, lehká a plná kopie, omezení
 *        obnovy, značka, export a stop.
 */
void test_data_through_emu_thread ( void )
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = emu_start ( &fe );
    st_COVERAGE_CONFIG cfg = { COVERAGE_LEVEL_DETAIL, COVERAGE_KEY_OPCODE, 64, false };
    char err[ 128 ];
    bool ok_start = dbg_cov_data_op ( DBGAPI_COVERAGE_OP_START, &cfg, false, err, sizeof ( err ) );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_start );
    TEST_ASSERT_EQUAL_STRING ( "", err );

    static const uint8_t jrnz[] = { 0x20, 0x10 };
    static const uint8_t nop[] = { 0x00 };
    step ( 0x3000, 0x3012, jrnz, 2, 0x00 );   /* skok */
    step ( 0x3000, 0x3002, jrnz, 2, 0x40 );   /* propad */
    step ( 0x3002, 0x3003, nop, 1, 0x00 );

    t = emu_start ( &fe );
    /* Lehká kopie: stav adres pro značky, bez jednotek. */
    dbg_cov_data_poll ( false );
    TEST_ASSERT_TRUE ( dbg_cov_data_valid ( ) );
    TEST_ASSERT_TRUE ( dbg_cov_data_status ( )->has_data );
    TEST_ASSERT_TRUE ( dbg_cov_data_status ( )->active );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_BOTH, dbg_cov_data_pc_state ( 0x3000 ) );
    TEST_ASSERT_EQUAL_UINT ( COVERAGE_STATE_NONE, dbg_cov_data_pc_state ( 0x3002 ) );
    TEST_ASSERT_EQUAL_UINT ( 0, dbg_cov_data_pc_state ( 0x3001 ) );
    TEST_ASSERT_NULL ( dbg_cov_data_full ( ) );
    int seen_light = g_atomic_int_get ( &fe.coverage_seen );

    /* Plná kopie: řádky a řádky jedné adresy. */
    dbg_cov_data_poll ( true );
    TEST_ASSERT_EQUAL_INT ( seen_light + 1, g_atomic_int_get ( &fe.coverage_seen ) );
    TEST_ASSERT_NOT_NULL ( dbg_cov_data_full ( ) );
    size_t n = 0;
    const st_COVERAGE_ROW *rows = dbg_cov_data_rows ( &n );
    TEST_ASSERT_NOT_NULL ( rows );
    TEST_ASSERT_EQUAL_size_t ( 2, n );
    size_t first = 99;
    TEST_ASSERT_EQUAL_size_t ( 1, dbg_cov_data_rows_for_pc ( 0x3002, &first ) );
    TEST_ASSERT_EQUAL_size_t ( 1, first );
    TEST_ASSERT_EQUAL_UINT32 ( 1, rows[ first ].unit->exec );
    TEST_ASSERT_EQUAL_size_t ( 0, dbg_cov_data_rows_for_pc ( 0x4000, &first ) );
    uint32_t serial = dbg_cov_data_full_serial ( );
    TEST_ASSERT_TRUE ( serial > 0 );

    /* Bez uplynulého intervalu se nic nežádá. */
    int seen = g_atomic_int_get ( &fe.coverage_seen );
    dbg_cov_data_poll ( true );
    TEST_ASSERT_EQUAL_INT ( seen, g_atomic_int_get ( &fe.coverage_seen ) );

    /* Značka vynutí obnovu obou kopií (epocha 1). */
    TEST_ASSERT_TRUE ( dbg_cov_data_op ( DBGAPI_COVERAGE_OP_MARK, NULL, false, err, sizeof ( err ) ) );
    dbg_cov_data_poll ( true );
    TEST_ASSERT_EQUAL_UINT32 ( 1, dbg_cov_data_status ( )->epoch );
    TEST_ASSERT_TRUE ( dbg_cov_data_full_serial ( ) > serial );

    /* Export JSON emulátorovým formátem. */
    gchar *path = g_build_filename ( g_get_tmp_dir ( ), "mz800emu_test_coverage_export.json", NULL );
    size_t exported = 0;
    bool ok_export = dbg_cov_data_export ( path, false, &exported, err, sizeof ( err ) );

    bool ok_stop = dbg_cov_data_op ( DBGAPI_COVERAGE_OP_STOP, NULL, false, err, sizeof ( err ) );
    dbg_cov_data_poll ( false );
    bool active_after = dbg_cov_data_status ( )->active;
    emu_stop ( &fe, t );

    TEST_ASSERT_TRUE ( ok_export );
    TEST_ASSERT_EQUAL_size_t ( 2, exported );
    gchar *text = NULL;
    TEST_ASSERT_TRUE ( g_file_get_contents ( path, &text, NULL, NULL ) );
    TEST_ASSERT_NOT_NULL ( strstr ( text, "mz800emu-coverage/1" ) );
    g_free ( text );
    g_remove ( path );
    g_free ( path );

    TEST_ASSERT_TRUE ( ok_stop );
    TEST_ASSERT_FALSE ( active_after );
}


int main ( int argc, char **argv )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );
    UNITY_BEGIN ( );

    RUN_TEST ( test_start_not_applied_without_emu_thread );
    RUN_TEST ( test_data_through_emu_thread );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
