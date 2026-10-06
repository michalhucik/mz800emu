/**
 * @file test_callstack_window_queue.c
 * @brief Zapnutí / vypnutí a vyprázdnění callstacku z panelu Callstack
 *        jde přes CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v panelu Callstack debuggeru
 * (dbg_callstack.cpp): UI vlákno dřív volalo přímo callstack_set_active()
 * (registrace Z80 CALL/RET hooků běžícího CPU + vynulování shadow stacku)
 * a callstack_reset() (g_depth = 0 souběžně s push/pop v CALL/RET hoocích
 * na emu vlákně). Nově panel volá UI helpery dbg_ui_callstack_set_active /
 * dbg_ui_callstack_reset z dbgapi_helpers.cpp, které odešlou
 * DBGAPI_CMD_CALLSTACK_SET_ACTIVE / _RESET a operaci vykoná emu vlákno.
 *
 * Ověřuje se přes skutečné helpery:
 *  - bez vlákna, které frontu obsluhuje, se operace NEprovede (helper
 *    vrátí false po timeoutu, stav beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) operace projde a emu vlákno vidí očekávané příkazy.
 *
 * Shadow stack se plní přímým voláním registrovaného hooku cpu->call_cb
 * (vzor test_callstack.c), vždy jen když simulované emu vlákno neběží.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/callstack.h"
#include "libs/cpu-z80/z80.h"
#include "mzarch/mzarch.h"
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
    gint active_seen;   /**< Počet vyzvednutých CALLSTACK_SET_ACTIVE. */
    gint reset_seen;    /**< Počet vyzvednutých CALLSTACK_RESET. */
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
            case DBGAPI_CMD_CALLSTACK_SET_ACTIVE:
                g_atomic_int_inc(&fe->active_seen);
                break;
            case DBGAPI_CMD_CALLSTACK_RESET:
                g_atomic_int_inc(&fe->reset_seen);
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
 * @brief Přidá do shadow stacku @p n rámců přes registrovaný CALL hook.
 *
 * @param n Počet rámců.
 * @pre Callstack je aktivní (cpu->call_cb zaregistrován), simulované emu
 *      vlákno neběží.
 */
static void push_frames(int n)
{
    z80_t *cpu = g_mzarch_main.cpu;
    TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_NOT_NULL(cpu->call_cb);
    for (int i = 0; i < n; i++)
    {
        uint16_t sp_after_push = (uint16_t)(0xF000 - 2 * i);
        cpu->sp = (uint16_t)(sp_after_push + 2);
        cpu->call_cb(cpu, (uint16_t)(0x1000 + i), (uint16_t)(0x2000 + i),
                     (uint16_t)(0x1003 + i), cpu->call_data);
    };
}


/** @brief Aktuální hloubka shadow stacku (přes callstack_get_stats). */
static int current_depth(void)
{
    st_CALLSTACK_STATS s;
    callstack_get_stats(&s);
    return s.current_depth;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    callstack_set_active(false);
    callstack_reset();
}

void tearDown(void)
{
    callstack_set_active(false);
    callstack_reset();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se zapnutí neprovede (hooky nezaregistrovány).
 */
void test_set_active_not_applied_without_emu_thread(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(dbg_ui_callstack_set_active(true, &out));
    TEST_ASSERT_TRUE(out);  /* Při neúspěchu se výstup nemění. */
    TEST_ASSERT_EQUAL_UINT8(0, g_callstack_active);
    TEST_ASSERT_NULL(g_mzarch_main.cpu->call_cb);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se vyprázdnění neprovede (hloubka zůstane).
 */
void test_reset_not_applied_without_emu_thread(void)
{
    callstack_set_active(true);
    push_frames(3);
    TEST_ASSERT_EQUAL_INT(3, current_depth());

    TEST_ASSERT_FALSE(dbg_ui_callstack_reset());
    TEST_ASSERT_EQUAL_INT(3, current_depth());

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projde zapnutí, vyprázdnění i vypnutí.
 */
void test_ops_executed_by_emu_thread(void)
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    /* Zapnutí: hooky zaregistrovány, flag 1. */
    bool active_after_on = false;
    bool ok_on = dbg_ui_callstack_set_active(true, &active_after_on);
    bool hook_after_on = (g_mzarch_main.cpu->call_cb != NULL);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    /* Naplnění shadow stacku bez běžícího simulovaného emu vlákna. */
    push_frames(4);
    int depth_before_reset = current_depth();

    g_atomic_int_set(&fe.stop, 0);
    t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok_reset = dbg_ui_callstack_reset();
    int depth_after_reset = current_depth();

    /* Vypnutí: hooky odregistrovány, flag 0. */
    bool active_after_off = true;
    bool ok_off = dbg_ui_callstack_set_active(false, &active_after_off);
    bool hook_after_off = (g_mzarch_main.cpu->call_cb != NULL);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_on);
    TEST_ASSERT_TRUE(active_after_on);
    TEST_ASSERT_TRUE(hook_after_on);

    TEST_ASSERT_EQUAL_INT(4, depth_before_reset);
    TEST_ASSERT_TRUE(ok_reset);
    TEST_ASSERT_EQUAL_INT(0, depth_after_reset);

    TEST_ASSERT_TRUE(ok_off);
    TEST_ASSERT_FALSE(active_after_off);
    TEST_ASSERT_FALSE(hook_after_off);

    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.active_seen));
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.reset_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_set_active_not_applied_without_emu_thread);
    RUN_TEST(test_reset_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
