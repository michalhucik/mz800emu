/**
 * @file test_io_window_queue.c
 * @brief Změna kapacity a vyprázdnění historie I/O a reset aktivity I/O
 *        (všech portů i jednoho portu) jdou přes CMDRQ frontu, ne přímo
 *        z UI vlákna.
 *
 * Regresní test pro souběh v okně I/O Ports (io_window.cpp): UI vlákno
 * dřív volalo přímo io_history_set_capacity() (free + calloc ringu, do
 * kterého emu vlákno souběžně zapisuje přes io_history_record - zápis do
 * uvolněné paměti), io_history_clear() (head/count/overflow souběžně
 * s jejich inkrementací v emu vlákně), io_activity_reset_all() (memset
 * tabulky, kterou emu vlákno inkrementuje) a z kontextového menu portu
 * io_activity_reset_port_8bit() / io_activity_reset_port() (memset slotů
 * téže tabulky). Nově okno volá UI helpery z dbgapi_helpers.cpp, které
 * odešlou příkazy DBGAPI_CMD_IO_HISTORY_*, DBGAPI_CMD_IO_ACTIVITY_RESET
 * a DBGAPI_CMD_IO_ACTIVITY_RESET_PORT a mutaci vykoná emu vlákno.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se mutace NEprovede (helper
 *    vrátí false po timeoutu, ring/tabulka beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) mutace projde, emu vlákno vidí očekávaný příkaz a helper
 *    vrátí skutečnou (clampovanou) kapacitu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/io_history.h"
#include "debugger/io_activity.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Port, na kterém test zaznamenává aktivitu. */
#define TEST_PORT 0x00CEu

/** @brief Tentýž 8-bit port s jiným high byte (jiný slot tabulky). */
#define TEST_PORT_HB 0x12CEu

/** @brief 16-bit bus adresa (rodina 0CFh, katalog 0CF02h -> bus 02CFh). */
#define TEST_PORT16 0x02CFu

/** @brief Sousední 16-bit slot, který reset TEST_PORT16 nesmí zasáhnout. */
#define TEST_PORT16_OTHER 0x03CFu

/** @brief Počet událostí zapsaných do historie před testovanou operací. */
#define TEST_EVENTS 5u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;            /**< 1 = ukončit smyčku (atomicky). */
    gint set_cap_seen;    /**< Počet vyzvednutých IO_HISTORY_SET_CAPACITY. */
    gint clear_seen;      /**< Počet vyzvednutých IO_HISTORY_CLEAR. */
    gint act_reset_seen;  /**< Počet vyzvednutých IO_ACTIVITY_RESET. */
    gint port_reset_seen; /**< Počet vyzvednutých IO_ACTIVITY_RESET_PORT. */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna: drain fronty jako mzarch drain.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread(gpointer user_data)
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *)user_data;
    while (!g_atomic_int_get(&fe->stop))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue);
        if (!rq)
        {
            (void)dbgapi_emu_wait_for_cmd(&g_dbgapi_cmdrq_queue, 5);
            continue;
        };
        switch (rq->cmd & DBGAPI_CMD_MASK)
        {
            case DBGAPI_CMD_IO_HISTORY_SET_CAPACITY:
                g_atomic_int_inc(&fe->set_cap_seen);
                break;
            case DBGAPI_CMD_IO_HISTORY_CLEAR:
                g_atomic_int_inc(&fe->clear_seen);
                break;
            case DBGAPI_CMD_IO_ACTIVITY_RESET:
                g_atomic_int_inc(&fe->act_reset_seen);
                break;
            case DBGAPI_CMD_IO_ACTIVITY_RESET_PORT:
                g_atomic_int_inc(&fe->port_reset_seen);
                break;
            default:
                break;
        };
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/**
 * @brief Zapíše do historie TEST_EVENTS IORQ událostí.
 */
static void fill_history(void)
{
    for (unsigned i = 0; i < TEST_EVENTS; i++)
        io_history_record(false, TEST_PORT, (uint8_t)i, 0x1200, 1, 10, 20, 100u + i);
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    io_history_destroy();
    io_history_init(IO_HISTORY_DEFAULT_CAPACITY);
    io_activity_init();
}

void tearDown(void)
{
    io_history_destroy();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se změna kapacity neprovede (ring se neuvolní).
 */
void test_set_capacity_not_applied_without_emu_thread(void)
{
    fill_history();
    st_IO_HISTORY_EVENT *events_before = g_io_history.events;

    uint32_t after = 0xDEADBEEFu;
    bool ok = dbg_ui_io_history_set_capacity(IO_HISTORY_MIN_CAPACITY, &after);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, after);

    TEST_ASSERT_TRUE(g_io_history.events == events_before);
    TEST_ASSERT_EQUAL_size_t(IO_HISTORY_DEFAULT_CAPACITY, g_io_history.capacity);
    TEST_ASSERT_EQUAL_size_t(TEST_EVENTS, g_io_history.count);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se historie nevyprázdní.
 */
void test_clear_not_applied_without_emu_thread(void)
{
    fill_history();

    TEST_ASSERT_FALSE(dbg_ui_io_history_clear());
    TEST_ASSERT_EQUAL_size_t(TEST_EVENTS, g_io_history.count);
    TEST_ASSERT_EQUAL_size_t(TEST_EVENTS, g_io_history.head);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se čítače aktivity nevynulují.
 */
void test_activity_reset_not_applied_without_emu_thread(void)
{
    io_activity_record_hit(TEST_PORT, 0x42, false, 7);
    TEST_ASSERT_EQUAL_UINT32(1, g_io_activity[TEST_PORT].total_hits_out);

    TEST_ASSERT_FALSE(dbg_ui_io_activity_reset_all());
    TEST_ASSERT_EQUAL_UINT32(1, g_io_activity[TEST_PORT].total_hits_out);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se čítače jednoho portu nevynulují (8bit i 16bit).
 */
void test_port_reset_not_applied_without_emu_thread(void)
{
    io_activity_record_hit(TEST_PORT_HB, 0x42, false, 7);
    io_activity_record_hit(TEST_PORT16, 0x43, false, 7);

    TEST_ASSERT_FALSE(dbg_ui_io_activity_reset_port(TEST_PORT & 0xFFu, true));
    TEST_ASSERT_EQUAL_UINT32(1, g_io_activity[TEST_PORT_HB].total_hits_out);
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));

    TEST_ASSERT_FALSE(dbg_ui_io_activity_reset_port(TEST_PORT16, false));
    TEST_ASSERT_EQUAL_UINT32(1, g_io_activity[TEST_PORT16].total_hits_out);
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projde reset jednoho portu (8bit i 16bit).
 *
 * 8-bit varianta vynuluje všechny high-byte sloty daného low byte
 * (TEST_PORT i TEST_PORT_HB), 16-bit varianta jen svůj slot (sousední
 * TEST_PORT16_OTHER zůstane).
 */
void test_port_reset_executed_by_emu_thread(void)
{
    io_activity_record_hit(TEST_PORT, 0x41, false, 7);
    io_activity_record_hit(TEST_PORT_HB, 0x42, false, 7);
    io_activity_record_hit(TEST_PORT16, 0x43, false, 7);
    io_activity_record_hit(TEST_PORT16_OTHER, 0x44, false, 7);

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok8 = dbg_ui_io_activity_reset_port(TEST_PORT & 0xFFu, true);
    uint32_t hits_hb_after8 = g_io_activity[TEST_PORT_HB].total_hits_out;
    uint32_t hits16_after8 = g_io_activity[TEST_PORT16].total_hits_out;
    bool ok16 = dbg_ui_io_activity_reset_port(TEST_PORT16, false);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok8);
    TEST_ASSERT_EQUAL_UINT32(0, g_io_activity[TEST_PORT].total_hits_out);
    TEST_ASSERT_EQUAL_UINT32(0, hits_hb_after8);
    TEST_ASSERT_EQUAL_UINT32(1, hits16_after8);

    TEST_ASSERT_TRUE(ok16);
    TEST_ASSERT_EQUAL_UINT32(0, g_io_activity[TEST_PORT16].total_hits_out);
    TEST_ASSERT_EQUAL_UINT32(1, g_io_activity[TEST_PORT16_OTHER].total_hits_out);

    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.port_reset_seen));
    TEST_ASSERT_EQUAL_INT(0, g_atomic_int_get(&fe.act_reset_seen));
}


/**
 * @brief Se simulovaným emu vláknem projdou všechny tři operace přes frontu.
 *
 * Kapacita se vrací z výsledku příkazu včetně clampu (požadavek pod
 * minimem dá IO_HISTORY_MIN_CAPACITY).
 */
void test_ops_executed_by_emu_thread(void)
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    /* Změna kapacity (s clampem) zahodí data. */
    fill_history();
    uint32_t after = 0;
    bool ok_cap = dbg_ui_io_history_set_capacity(1u, &after);

    /* Clear. */
    fill_history();
    size_t count_before_clear = g_io_history.count;
    bool ok_clear = dbg_ui_io_history_clear();
    size_t count_after_clear = g_io_history.count;

    /* Reset aktivity. */
    io_activity_record_hit(TEST_PORT, 0x42, false, 7);
    bool ok_reset = dbg_ui_io_activity_reset_all();

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_cap);
    TEST_ASSERT_EQUAL_UINT32(IO_HISTORY_MIN_CAPACITY, after);
    TEST_ASSERT_EQUAL_size_t(IO_HISTORY_MIN_CAPACITY, g_io_history.capacity);
    TEST_ASSERT_NOT_NULL(g_io_history.events);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.set_cap_seen));

    TEST_ASSERT_EQUAL_size_t(TEST_EVENTS, count_before_clear);
    TEST_ASSERT_TRUE(ok_clear);
    TEST_ASSERT_EQUAL_size_t(0, count_after_clear);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.clear_seen));

    TEST_ASSERT_TRUE(ok_reset);
    TEST_ASSERT_EQUAL_UINT32(0, g_io_activity[TEST_PORT].total_hits_out);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.act_reset_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_set_capacity_not_applied_without_emu_thread);
    RUN_TEST(test_clear_not_applied_without_emu_thread);
    RUN_TEST(test_activity_reset_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);
    RUN_TEST(test_port_reset_not_applied_without_emu_thread);
    RUN_TEST(test_port_reset_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
