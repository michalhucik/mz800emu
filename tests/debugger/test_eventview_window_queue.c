/**
 * @file test_eventview_window_queue.c
 * @brief Režim záznamu, maska kategorií a import souboru okna Events jdou
 *        přes CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v okně Events (event_viewer_window.cpp): UI
 * vlákno dřív přímo zapisovalo g_eventlog_config.mode a volalo
 * eventlog_recompute_active() (příznak g_eventlog_active čte gate každého
 * zápisu), read-modify-write g_eventlog_active_mask (maska kategorií,
 * kterou může souběžně nastavit MCP eventlog_set_mask) a hlavně
 * eventlog_import_from_file() - ta může ring realokovat
 * (eventlog_set_capacity = free + calloc) a čte záznamy přímo do ringu,
 * do kterého emu vlákno souběžně zapisuje v eventlog_record(). Nově okno
 * volá UI helpery dbg_ui_eventlog_* z dbgapi_helpers.cpp, které odešlou
 * DBGAPI_CMD_EVENTLOG_SET_MODE, _SET_MASK a _IMPORT_FILE a operaci vykoná
 * emu vlákno.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se nic neprovede (helper vrátí
 *    false po timeoutu, režim / maska / ring beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) operace projdou, emu vlákno vidí očekávané příkazy, import
 *    zvětší ring a výsledky (aktivita, rc, počet) se vrátí volajícímu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/trace/eventlog.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/**
 * @brief Počet záznamů v exportovaném souboru.
 *
 * O 1 víc než EVENTLOG_MIN_CAPACITY: import do ringu s minimální kapacitou
 * proto musí ring realokovat (eventlog_set_capacity = free + calloc), což
 * je nejrizikovější část dřívějšího přímého volání z UI vlákna.
 */
#define TEST_FILE_EVENTS ( EVENTLOG_MIN_CAPACITY + 1u )

/** @brief Počet událostí v ringu před testovaným importem. */
#define TEST_RING_EVENTS 3u

/** @brief Kategorie, jejíž bit test v masce přepíná. */
#define TEST_CAT_BIT ( UINT64_C ( 1 ) << EVENTLOG_CAT_IORQ_OUT )


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint mode_seen;     /**< Počet vyzvednutých EVENTLOG_SET_MODE. */
    gint mask_seen;     /**< Počet vyzvednutých EVENTLOG_SET_MASK. */
    gint import_seen;   /**< Počet vyzvednutých EVENTLOG_IMPORT_FILE. */
} st_FAKE_EMU;


/** @brief Cesta k exportovanému souboru (vytváří setUp, maže tearDown). */
static char s_file_path[ 512 ];


/**
 * @brief Tělo simulovaného emu vlákna: drain fronty jako mzarch drain.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread ( gpointer user_data )
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *) user_data;
    while ( !g_atomic_int_get ( &fe->stop ) )
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue );
        if ( !rq )
        {
            (void) dbgapi_emu_wait_for_cmd ( &g_dbgapi_cmdrq_queue, 5 );
            continue;
        };
        switch ( rq->cmd & DBGAPI_CMD_MASK )
        {
            case DBGAPI_CMD_EVENTLOG_SET_MODE:
                g_atomic_int_inc ( &fe->mode_seen );
                break;
            case DBGAPI_CMD_EVENTLOG_SET_MASK:
                g_atomic_int_inc ( &fe->mask_seen );
                break;
            case DBGAPI_CMD_EVENTLOG_IMPORT_FILE:
                g_atomic_int_inc ( &fe->import_seen );
                break;
            default:
                break;
        };
        dbgapi_emu_dispatch ( rq );
        dbgapi_emu_complete ( rq );
    };
    return NULL;
}


/**
 * @brief Zapíše do ringu @p n IORQ_OUT událostí s payloadem = pořadí.
 * @param n Počet událostí.
 */
static void record_events ( unsigned n )
{
    for ( unsigned i = 0; i < n; i++ )
        eventlog_record ( EVENTLOG_CAT_IORQ_OUT, 0x01, 0x1200, (uint32_t) i );
}


/**
 * @brief Připraví ring s minimální kapacitou a TEST_RING_EVENTS událostmi.
 *
 * Volá se přímo (bez fronty) - simuluje stav před akcí uživatele.
 */
static void prepare_small_ring ( void )
{
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY );
    record_events ( TEST_RING_EVENTS );
}


void setUp ( void )
{
    dbgapi_init ( &g_dbgapi_cmdrq_queue );

    /* Soubor s TEST_FILE_EVENTS záznamy: naplnit ring s výchozí kapacitou
     * a exportovat (exportní formát je kontrakt importu). */
    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
    g_eventlog_active = 0;
    g_eventlog_config.mode = EVENTLOG_MODE_OFF;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );
    g_eventlog_config.categories_mask = g_eventlog_active_mask;
    record_events ( TEST_FILE_EVENTS );

    static int counter = 0;
    counter++;
    snprintf ( s_file_path, sizeof ( s_file_path ), "%s/evw_queue_test_%lld_%d.evlog",
               g_get_tmp_dir ( ), (long long) g_get_monotonic_time ( ), counter );
    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( s_file_path ) );

    eventlog_clear ( );
}

void tearDown ( void )
{
    remove ( s_file_path );
    eventlog_destroy ( );
    g_eventlog_config.mode = EVENTLOG_MODE_OFF;
    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/**
 * @brief Bez emu vlákna se režim nezmění a záznam se nespustí.
 */
void test_set_mode_not_applied_without_emu_thread ( void )
{
    bool active = false;
    TEST_ASSERT_FALSE ( dbg_ui_eventlog_set_mode ( EVENTLOG_MODE_ALWAYS, &active ) );

    TEST_ASSERT_EQUAL_INT ( EVENTLOG_MODE_OFF, g_eventlog_config.mode );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Bez emu vlákna se maska kategorií nezmění.
 */
void test_set_mask_not_applied_without_emu_thread ( void )
{
    uint64_t want = g_eventlog_active_mask & ~TEST_CAT_BIT;

    TEST_ASSERT_FALSE ( dbg_ui_eventlog_set_mask ( want ) );
    TEST_ASSERT_TRUE ( ( g_eventlog_active_mask & TEST_CAT_BIT ) != 0 );
    TEST_ASSERT_TRUE ( ( g_eventlog_config.categories_mask & TEST_CAT_BIT ) != 0 );

    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Bez emu vlákna import ring nerealokuje ani nepřepíše.
 */
void test_import_not_applied_without_emu_thread ( void )
{
    prepare_small_ring ( );
    st_EVENTLOG_EVENT *events_before = g_eventlog.events;

    int rc = 12345;
    uint32_t count_after = 0xDEADBEEFu;
    TEST_ASSERT_FALSE ( dbg_ui_eventlog_import_file ( s_file_path, &rc, &count_after ) );

    /* Neprovedený příkaz výstupy nemění. */
    TEST_ASSERT_EQUAL_INT ( 12345, rc );
    TEST_ASSERT_EQUAL_UINT32 ( 0xDEADBEEFu, count_after );

    TEST_ASSERT_TRUE ( g_eventlog.events == events_before );
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.capacity );
    TEST_ASSERT_EQUAL_size_t ( TEST_RING_EVENTS, g_eventlog.count );

    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Se simulovaným emu vláknem projdou režim, maska i import.
 *
 * Režim: ALWAYS zapne záznam, OFF vypne, neplatná hodnota se odmítne.
 * Import souboru s víc záznamy, než je kapacita ringu, ring zvětší;
 * import neexistujícího souboru vrátí rc = -1 a ring nechá beze změny.
 */
void test_ops_executed_by_emu_thread ( void )
{
    prepare_small_ring ( );

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new ( "fake-emu", fake_emu_thread, &fe );

    /* Režim. */
    bool active_always = false;
    bool ok_always = dbg_ui_eventlog_set_mode ( EVENTLOG_MODE_ALWAYS, &active_always );
    int mode_after_always = (int) g_eventlog_config.mode;
    bool active_off = true;
    bool ok_off = dbg_ui_eventlog_set_mode ( EVENTLOG_MODE_OFF, &active_off );
    bool ok_invalid = dbg_ui_eventlog_set_mode ( 3u, NULL );
    int mode_after_invalid = (int) g_eventlog_config.mode;

    /* Maska. */
    uint64_t want_mask = g_eventlog_active_mask & ~TEST_CAT_BIT;
    bool ok_mask = dbg_ui_eventlog_set_mask ( want_mask );

    /* Import s realokací ringu. */
    int rc = 12345;
    uint32_t count_after = 0;
    bool ok_import = dbg_ui_eventlog_import_file ( s_file_path, &rc, &count_after );
    size_t cap_after_import = g_eventlog.capacity;
    size_t count_after_import = g_eventlog.count;
    const st_EVENTLOG_EVENT *last = eventlog_get_event ( TEST_FILE_EVENTS - 1u );
    uint32_t last_payload = last ? last->payload : 0xFFFFFFFFu;

    /* Import neexistujícího souboru: provede se, ale selže. */
    char missing[ 600 ];
    snprintf ( missing, sizeof ( missing ), "%s.missing", s_file_path );
    int rc_missing = 12345;
    uint32_t count_missing = 0;
    bool ok_missing = dbg_ui_eventlog_import_file ( missing, &rc_missing, &count_missing );

    g_atomic_int_set ( &fe.stop, 1 );
    g_thread_join ( t );

    TEST_ASSERT_TRUE ( ok_always );
    TEST_ASSERT_TRUE ( active_always );
    TEST_ASSERT_EQUAL_INT ( EVENTLOG_MODE_ALWAYS, mode_after_always );
    TEST_ASSERT_TRUE ( ok_off );
    TEST_ASSERT_FALSE ( active_off );
    TEST_ASSERT_FALSE ( ok_invalid );
    TEST_ASSERT_EQUAL_INT ( EVENTLOG_MODE_OFF, mode_after_invalid );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );
    TEST_ASSERT_EQUAL_INT ( 3, g_atomic_int_get ( &fe.mode_seen ) );

    TEST_ASSERT_TRUE ( ok_mask );
    TEST_ASSERT_TRUE ( g_eventlog_active_mask == want_mask );
    TEST_ASSERT_TRUE ( g_eventlog_config.categories_mask == want_mask );
    TEST_ASSERT_EQUAL_INT ( 1, g_atomic_int_get ( &fe.mask_seen ) );

    TEST_ASSERT_TRUE ( ok_import );
    TEST_ASSERT_EQUAL_INT ( 0, rc );
    TEST_ASSERT_EQUAL_UINT32 ( TEST_FILE_EVENTS, count_after );
    TEST_ASSERT_EQUAL_size_t ( TEST_FILE_EVENTS, cap_after_import );
    TEST_ASSERT_EQUAL_size_t ( TEST_FILE_EVENTS, count_after_import );
    TEST_ASSERT_EQUAL_UINT32 ( TEST_FILE_EVENTS - 1u, last_payload );

    TEST_ASSERT_FALSE ( ok_missing );
    TEST_ASSERT_EQUAL_INT ( -1, rc_missing );
    TEST_ASSERT_EQUAL_UINT32 ( TEST_FILE_EVENTS, count_missing );
    TEST_ASSERT_EQUAL_size_t ( TEST_FILE_EVENTS, g_eventlog.count );
    TEST_ASSERT_EQUAL_INT ( 2, g_atomic_int_get ( &fe.import_seen ) );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_set_mode_not_applied_without_emu_thread );
    RUN_TEST ( test_set_mask_not_applied_without_emu_thread );
    RUN_TEST ( test_import_not_applied_without_emu_thread );
    RUN_TEST ( test_ops_executed_by_emu_thread );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
