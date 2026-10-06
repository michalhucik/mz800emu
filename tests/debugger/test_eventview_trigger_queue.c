/**
 * @file test_eventview_trigger_queue.c
 * @brief Triggery okna Events (Pause / Auto-mark on match) mění jen emu
 *        vlákno přes CMDRQ frontu; filtr předává UI vlastnicky.
 *
 * Regresní test pro souběh v okně Events (event_viewer_window.cpp): stav
 * triggerů dřív žil ve statice okna. UI vlákno při každé editaci uvolnilo
 * filtr (eventlog_filter_free) a naparsovalo nový, zatímco callback
 * v eventlog_record() na emu vlákně mohl starý filtr právě vyhodnocovat
 * (use-after-free); gate g_eventlog_*_trigger_active a ukazatele callbacků
 * zapisovalo UI; ImGui editovalo jméno markeru přímo v bufferu, který
 * callback četl (strcmp / snprintf / marklog_register).
 *
 * Nově stav vlastní eventlog_trigger.c. Okno naparsuje nový filtr do
 * nového objektu a předá ho helperem dbg_ui_eventlog_trigger_set()
 * (DBGAPI_CMD_EVENTLOG_TRIGGER_SET); emu vlákno vymění filtr, kopii jména
 * a gate a vrátí starý filtr, který helper uvolní po návratu synchronního
 * submitu. Počítadla shod maže DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se trigger nezmění, helper nový
 *    filtr uvolní (eventlog_filter_live_count) a zrušený slot emu
 *    nevyzvedne; totéž pro vymazání počítadel,
 *  - se simulovaným emu vláknem se trigger vymění, starý filtr se uvolní,
 *    callback po výměně (eventlog_record na vlákně v roli emu) používá nový
 *    filtr a nové jméno markeru; neplatný filtr / prázdné jméno handler
 *    odmítne a helper filtr uvolní.
 *
 * Samotný souběh (uvolnění filtru během vyhodnocení) test nevyvolává -
 * je vyloučen konstrukcí (výměnu i vyhodnocení dělá jedno vlákno).
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
#include "debugger/eventlog_filter.h"
#include "debugger/trace/eventlog.h"
#include "debugger/trace/eventlog_trigger.h"
#include "debugger/trace/marklog.h"
#include "emulator/emulator.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint set_seen;      /**< Počet vyzvednutých EVENTLOG_TRIGGER_SET. */
    gint clear_seen;    /**< Počet vyzvednutých EVENTLOG_TRIGGER_CLEAR_MATCHES. */
} st_FAKE_EMU;

/** @brief Výraz filtru "cat:<IORQ_OUT>" (plní setUp). */
static char s_expr_out[ 64 ];
/** @brief Výraz filtru "cat:<IORQ_IN>" (plní setUp). */
static char s_expr_in[ 64 ];


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
            case DBGAPI_CMD_EVENTLOG_TRIGGER_SET:
                g_atomic_int_inc ( &fe->set_seen );
                break;
            case DBGAPI_CMD_EVENTLOG_TRIGGER_CLEAR_MATCHES:
                g_atomic_int_inc ( &fe->clear_seen );
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
 * @brief Spustí simulované emu vlákno.
 * @param fe Stav vlákna (vynuluje se).
 * @return Handle vlákna pro emu_stop().
 */
static GThread *emu_start ( st_FAKE_EMU *fe )
{
    memset ( fe, 0, sizeof ( *fe ) );
    return g_thread_new ( "fake-emu", fake_emu_thread, fe );
}

/**
 * @brief Zastaví simulované emu vlákno a počká na něj.
 *
 * Po návratu je testovací vlákno jediné, které se stavem pracuje, a smí
 * hrát roli emu vlákna (eventlog_record, přímé eventlog_trigger_*).
 *
 * @param fe Stav vlákna.
 * @param t  Handle z emu_start().
 */
static void emu_stop ( st_FAKE_EMU *fe, GThread *t )
{
    g_atomic_int_set ( &fe->stop, 1 );
    g_thread_join ( t );
}

/**
 * @brief Zapíše jednu událost do ringu (role emu vlákna).
 * @param cat Kategorie (en_EVENTLOG_CATEGORY).
 */
static void record ( uint8_t cat )
{
    eventlog_record ( cat, 0x01, 0x1200, 0x55u );
}

/**
 * @brief Vrátí stav triggeru.
 * @param kind Druh triggeru.
 * @return Kopie stavu.
 */
static st_EVENTLOG_TRIGGER_STATUS status ( en_EVENTLOG_TRIGGER_KIND kind )
{
    st_EVENTLOG_TRIGGER_STATUS st;
    eventlog_trigger_get_status ( kind, &st );
    return st;
}


void setUp ( void )
{
    dbgapi_init ( &g_dbgapi_cmdrq_queue );
    eventlog_init ( EVENTLOG_MIN_CAPACITY );
    g_eventlog_active = 1;   /* marklog_record zapisuje USER_MARK do ringu. */
    g_emulator.paused = false;

    snprintf ( s_expr_out, sizeof ( s_expr_out ), "cat:%s",
               eventlog_filter_cat_to_name ( EVENTLOG_CAT_IORQ_OUT ) );
    snprintf ( s_expr_in, sizeof ( s_expr_in ), "cat:%s",
               eventlog_filter_cat_to_name ( EVENTLOG_CAT_IORQ_IN ) );
}

void tearDown ( void )
{
    eventlog_trigger_shutdown ( );
    eventlog_destroy ( );
    g_emulator.paused = false;
    dbgapi_destroy ( &g_dbgapi_cmdrq_queue );
}


/**
 * @brief Bez emu vlákna se trigger nezapne a helper nový filtr uvolní.
 */
void test_set_not_applied_without_emu_thread ( void )
{
    int live0 = eventlog_filter_live_count ( );

    st_EVENTLOG_FILTER *f = eventlog_filter_parse ( s_expr_out );
    TEST_ASSERT_NOT_NULL ( f );
    TEST_ASSERT_FALSE ( dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE, f, NULL ) );

    st_EVENTLOG_FILTER *g = eventlog_filter_parse ( s_expr_out );
    TEST_ASSERT_FALSE ( dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_AUTOMARK, g, "t5c_x" ) );

    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).armed );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_AUTOMARK ).armed );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_pause_trigger_active );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_automark_trigger_active );
    /* Nový filtr neunikl. */
    TEST_ASSERT_EQUAL_INT ( live0, eventlog_filter_live_count ( ) );

    /* Shodná událost nic nespustí. */
    record ( EVENTLOG_CAT_IORQ_OUT );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Zrušené příkazy emu nevyzvedne. */
    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Bez emu vlákna se počítadla shod nevymažou.
 */
void test_clear_not_applied_without_emu_thread ( void )
{
    /* Výchozí stav připraví testovací vlákno v roli emu vlákna. */
    st_EVENTLOG_FILTER *old = NULL;
    TEST_ASSERT_TRUE ( eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE,
                                              eventlog_filter_parse ( s_expr_out ),
                                              NULL, &old ) );
    TEST_ASSERT_NULL ( old );
    record ( EVENTLOG_CAT_IORQ_OUT );
    TEST_ASSERT_TRUE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );

    TEST_ASSERT_FALSE ( dbg_ui_eventlog_trigger_clear_matches ( EVENTLOG_TRIGGER_PAUSE ) );
    TEST_ASSERT_TRUE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );

    TEST_ASSERT_NULL ( dbgapi_emu_dequeue ( &g_dbgapi_cmdrq_queue ) );
}


/**
 * @brief Pause on match: výměna filtru emu vláknem, starý se uvolní,
 *        callback po výměně používá nový filtr.
 */
void test_pause_swapped_by_emu_thread ( void )
{
    int live0 = eventlog_filter_live_count ( );
    st_FAKE_EMU fe;

    /* 1) Zapnout s filtrem na IORQ_OUT. */
    GThread *t = emu_start ( &fe );
    bool ok_out = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE,
                                                eventlog_filter_parse ( s_expr_out ), NULL );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_out );
    TEST_ASSERT_TRUE ( status ( EVENTLOG_TRIGGER_PAUSE ).armed );
    TEST_ASSERT_EQUAL_INT ( 1, g_eventlog_pause_trigger_active );
    TEST_ASSERT_EQUAL_INT ( live0 + 1, eventlog_filter_live_count ( ) );
    TEST_ASSERT_EQUAL_INT ( 1, g_atomic_int_get ( &fe.set_seen ) );

    record ( EVENTLOG_CAT_IORQ_IN );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    record ( EVENTLOG_CAT_IORQ_OUT );
    st_EVENTLOG_TRIGGER_STATUS st = status ( EVENTLOG_TRIGGER_PAUSE );
    TEST_ASSERT_TRUE ( st.has_match );
    TEST_ASSERT_EQUAL_INT64 ( (int64_t) g_eventlog.count - 1, st.last_match_event_idx );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    g_emulator.paused = false;

    /* 2) Vyměnit na IORQ_IN + vymazat počítadla; neplatné filtry odmítnout. */
    t = emu_start ( &fe );
    bool ok_in = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE,
                                               eventlog_filter_parse ( s_expr_in ), NULL );
    bool ok_clear = dbg_ui_eventlog_trigger_clear_matches ( EVENTLOG_TRIGGER_PAUSE );
    bool ok_syntax = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE,
                                                   eventlog_filter_parse ( "cat:" ), NULL );
    char temporal[ 96 ];
    snprintf ( temporal, sizeof ( temporal ), "after(5) %s", s_expr_out );
    st_EVENTLOG_FILTER *ft = eventlog_filter_parse ( temporal );
    bool temporal_has = eventlog_filter_has_temporal ( ft );
    bool ok_temporal = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE, ft, NULL );
    bool ok_bad_kind = dbg_ui_eventlog_trigger_clear_matches ( 7u );
    emu_stop ( &fe, t );

    TEST_ASSERT_TRUE ( ok_in );
    TEST_ASSERT_TRUE ( ok_clear );
    TEST_ASSERT_FALSE ( ok_syntax );
    TEST_ASSERT_TRUE ( temporal_has );
    TEST_ASSERT_FALSE ( ok_temporal );
    TEST_ASSERT_FALSE ( ok_bad_kind );
    TEST_ASSERT_EQUAL_INT ( 3, g_atomic_int_get ( &fe.set_seen ) );
    TEST_ASSERT_EQUAL_INT ( 2, g_atomic_int_get ( &fe.clear_seen ) );
    /* Starý filtr i odmítnuté filtry uvolněny, drží se jen nový. */
    TEST_ASSERT_EQUAL_INT ( live0 + 1, eventlog_filter_live_count ( ) );
    TEST_ASSERT_TRUE ( status ( EVENTLOG_TRIGGER_PAUSE ).armed );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );
    TEST_ASSERT_EQUAL_INT64 ( -1, status ( EVENTLOG_TRIGGER_PAUSE ).last_match_event_idx );

    record ( EVENTLOG_CAT_IORQ_OUT );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    record ( EVENTLOG_CAT_IORQ_IN );
    TEST_ASSERT_TRUE ( status ( EVENTLOG_TRIGGER_PAUSE ).has_match );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    g_emulator.paused = false;

    /* 3) Vypnout: gate 0, filtr uvolněn. */
    t = emu_start ( &fe );
    bool ok_off = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_PAUSE, NULL, NULL );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_off );
    TEST_ASSERT_FALSE ( status ( EVENTLOG_TRIGGER_PAUSE ).armed );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_pause_trigger_active );
    TEST_ASSERT_EQUAL_INT ( live0, eventlog_filter_live_count ( ) );
}


/**
 * @brief Auto-mark on match: emu vlákno převezme kopii jména, změna jména
 *        zneplatní marker, callback po výměně registruje nové jméno.
 */
void test_automark_swapped_by_emu_thread ( void )
{
    int live0 = eventlog_filter_live_count ( );
    st_FAKE_EMU fe;

    /* Jméno předává UI v bufferu, který po návratu přepíše (jako ImGui). */
    char ui_name[ EVENTLOG_TRIGGER_NAME_MAX ];
    snprintf ( ui_name, sizeof ( ui_name ), "%s", "t5c_mark_a" );

    GThread *t = emu_start ( &fe );
    bool ok_a = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_AUTOMARK,
                                              eventlog_filter_parse ( s_expr_out ), ui_name );
    bool ok_empty = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_AUTOMARK,
                                                  eventlog_filter_parse ( s_expr_out ), "" );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_a );
    TEST_ASSERT_FALSE ( ok_empty );
    TEST_ASSERT_EQUAL_INT ( 2, g_atomic_int_get ( &fe.set_seen ) );
    TEST_ASSERT_EQUAL_INT ( live0 + 1, eventlog_filter_live_count ( ) );
    TEST_ASSERT_EQUAL_INT ( 1, g_eventlog_automark_trigger_active );

    /* UI buffer se mění - emu vlákno má vlastní kopii. */
    snprintf ( ui_name, sizeof ( ui_name ), "%s", "garbage_edit" );

    size_t count0 = g_eventlog.count;
    record ( EVENTLOG_CAT_IORQ_IN );
    TEST_ASSERT_EQUAL_UINT16 ( MARKLOG_INVALID_ID, status ( EVENTLOG_TRIGGER_AUTOMARK ).marker_id );
    record ( EVENTLOG_CAT_IORQ_OUT );
    st_EVENTLOG_TRIGGER_STATUS st = status ( EVENTLOG_TRIGGER_AUTOMARK );
    TEST_ASSERT_TRUE ( st.has_match );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.total_marks );
    TEST_ASSERT_NOT_EQUAL ( MARKLOG_INVALID_ID, st.marker_id );
    TEST_ASSERT_EQUAL_STRING ( "t5c_mark_a", marklog_get_name ( st.marker_id ) );
    /* IN + OUT + USER_MARK (bez rekurze). */
    TEST_ASSERT_EQUAL_size_t ( count0 + 3u, g_eventlog.count );
    const st_EVENTLOG_EVENT *last = eventlog_get_event ( g_eventlog.count - 1u );
    TEST_ASSERT_NOT_NULL ( last );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_USER_MARK, last->category );
    uint16_t id_a = st.marker_id;

    /* Nové jméno + vymazání počítadel. */
    snprintf ( ui_name, sizeof ( ui_name ), "%s", "t5c_mark_b" );
    t = emu_start ( &fe );
    bool ok_b = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_AUTOMARK,
                                              eventlog_filter_parse ( s_expr_out ), ui_name );
    bool ok_clear = dbg_ui_eventlog_trigger_clear_matches ( EVENTLOG_TRIGGER_AUTOMARK );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_b );
    TEST_ASSERT_TRUE ( ok_clear );
    TEST_ASSERT_EQUAL_INT ( live0 + 1, eventlog_filter_live_count ( ) );
    st = status ( EVENTLOG_TRIGGER_AUTOMARK );
    TEST_ASSERT_EQUAL_UINT16 ( MARKLOG_INVALID_ID, st.marker_id );
    TEST_ASSERT_FALSE ( st.has_match );
    TEST_ASSERT_EQUAL_UINT64 ( 0, st.total_marks );

    record ( EVENTLOG_CAT_IORQ_OUT );
    st = status ( EVENTLOG_TRIGGER_AUTOMARK );
    TEST_ASSERT_EQUAL_UINT64 ( 1, st.total_marks );
    TEST_ASSERT_NOT_EQUAL ( id_a, st.marker_id );
    TEST_ASSERT_EQUAL_STRING ( "t5c_mark_b", marklog_get_name ( st.marker_id ) );

    /* Vypnout. */
    t = emu_start ( &fe );
    bool ok_off = dbg_ui_eventlog_trigger_set ( EVENTLOG_TRIGGER_AUTOMARK, NULL, NULL );
    emu_stop ( &fe, t );
    TEST_ASSERT_TRUE ( ok_off );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_automark_trigger_active );
    TEST_ASSERT_EQUAL_INT ( live0, eventlog_filter_live_count ( ) );
    TEST_ASSERT_EQUAL_INT ( 1, g_atomic_int_get ( &fe.set_seen ) );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_set_not_applied_without_emu_thread );
    RUN_TEST ( test_clear_not_applied_without_emu_thread );
    RUN_TEST ( test_pause_swapped_by_emu_thread );
    RUN_TEST ( test_automark_swapped_by_emu_thread );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
