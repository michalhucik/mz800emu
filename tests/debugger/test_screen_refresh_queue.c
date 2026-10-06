/**
 * @file test_screen_refresh_queue.c
 * @brief Vynucený refresh obrazovky z debuggeru (Ctrl+R, menu) jde přes
 *        CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v okně debuggeru (debugger_window.cpp, Ctrl+R)
 * a v menu Emulation -> Forced Full Screen Refresh (dbg_topmenu.cpp): UI
 * vlákno dřív volalo přímo debugger_forced_screen_update(), tedy
 * mzarch_forced_full_screen_refresh() - přegenerování celého framebufferu
 * a framebuffer_screen_done() (výměna g_framebuffer.pixels / pixels_id),
 * a to bez kontroly pauzy, souběžně s emu vláknem, které framebuffer plní
 * a snímky dokončuje. Nově UI volá helper dbg_ui_screen_refresh()
 * z dbgapi_helpers.cpp, který odešle DBGAPI_CMD_SCREEN_REFRESH a refresh
 * vykoná emu vlákno.
 *
 * Pozorovatelný efekt refreshe: framebuffer_screen_done() posune
 * g_framebuffer.pixels_id na další buffer.
 *
 * Ověřuje se přes skutečný helper:
 *  - bez vlákna, které frontu obsluhuje, se refresh NEprovede (helper
 *    vrátí false po timeoutu, pixels_id beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) refresh projde a emu vlákno vidí očekávaný příkaz.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "hw-generic/gdg/framebuffer.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy SCREEN_REFRESH. Žije na zásobníku testu, test na vlákno čeká
 * přes g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint refresh_seen;  /**< Počet vyzvednutých SCREEN_REFRESH. */
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
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_SCREEN_REFRESH)
            g_atomic_int_inc(&fe->refresh_seen);
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
}

void tearDown(void)
{
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se refresh neprovede (framebuffer beze změny).
 */
void test_refresh_not_applied_without_emu_thread(void)
{
    int id_before = g_framebuffer.pixels_id;

    TEST_ASSERT_FALSE(dbg_ui_screen_refresh());
    TEST_ASSERT_EQUAL_INT(id_before, g_framebuffer.pixels_id);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem refresh projde (dokončí snímek).
 */
void test_refresh_executed_by_emu_thread(void)
{
    int id_before = g_framebuffer.pixels_id;

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok = dbg_ui_screen_refresh();
    int id_after = g_framebuffer.pixels_id;
    en_FBSTATE state_after = g_framebuffer.framebuffer_state;

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT((id_before + 1) % GDG_FRAMEBUFFER_PIXBUF_COUNT, id_after);
    TEST_ASSERT_EQUAL_INT(FB_STATE_NOT_CHANGED, state_after);
    TEST_ASSERT_TRUE(g_framebuffer.pixels == g_framebuffer.pixbuff[id_after]);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.refresh_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_refresh_not_applied_without_emu_thread);
    RUN_TEST(test_refresh_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
