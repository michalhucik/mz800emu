/**
 * @file test_mhmap_window_queue.c
 * @brief Přepnutí režimu Memory Heatmap a mutace jejích counterů jdou přes
 *        CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v okně Memory Heatmap (mhmap_window.cpp)
 * a v menu Debugger -> Settings -> CDL (dbg_topmenu.cpp). UI vlákno dřív
 * volalo přímo:
 *  - mhmap_set_mode() - zápis režimu + swap CPU callbacků
 *    (mzarch_platform_fn_debugger_state_changed) souběžně s CPU smyčkou,
 *  - mhmap_reset() - memset celé g_mhmap,
 *  - reset_single_region() - memset jednoho regionu g_mhmap,
 *  - Add / Sub importovaných dat - smyčka přes celou g_mhmap,
 * zatímco emu vlákno do g_mhmap inkrementuje countery. Nově okno i menu
 * volají UI helpery z dbgapi_helpers.cpp; swap callbacků (RECOMPUTE) i
 * mutace counterů vykoná emu vlákno.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se callbacky CPU nepřepnou
 *    a countery se nezmění (helper vrátí false po timeoutu, zrušený slot
 *    emu nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) operace projdou a emu vlákno vidí očekávané příkazy.
 *
 * Nepokrývá: reálný drain fronty v mzarch smyčce (simuluje se vlastním
 * vláknem), vykreslení okna ani menu (ImGui se nevolá); zápis
 * g_debugger.mhmap_mode se podle vzoru cpuhist děje dál na UI vlákně
 * (bajtový flag), test ověřuje jen, že swap callbacků proběhne až na
 * emu vlákně.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Offset buňky v regionu "bus" (index 0), kterou test plní. */
#define TEST_BUS_OFFSET 0x1234u

/** @brief Offset buňky v regionu "ram" (index 1), kterou test plní. */
#define TEST_RAM_OFFSET 0x0042u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;            /**< 1 = ukončit smyčku (atomicky). */
    gint recompute_seen;  /**< Počet vyzvednutých DEBUGGER_STATE_RECOMPUTE. */
    gint reset_seen;      /**< Počet vyzvednutých CDL_RESET. */
    gint region_seen;     /**< Počet vyzvednutých MHMAP_RESET_REGION. */
    gint merge_seen;      /**< Počet vyzvednutých MHMAP_MERGE. */
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
            case DBGAPI_CMD_DEBUGGER_STATE_RECOMPUTE:
                g_atomic_int_inc(&fe->recompute_seen);
                break;
            case DBGAPI_CMD_CDL_RESET:
                g_atomic_int_inc(&fe->reset_seen);
                break;
            case DBGAPI_CMD_MHMAP_RESET_REGION:
                g_atomic_int_inc(&fe->region_seen);
                break;
            case DBGAPI_CMD_MHMAP_MERGE:
                g_atomic_int_inc(&fe->merge_seen);
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
 * @brief Vrátí true, pokud CPU právě používá logging (pomalé) callbacky.
 *
 * Swap provádí mzarch_platform_fn_debugger_state_changed; čtecí callback
 * paměti je jeho spolehlivý indikátor.
 */
static bool cpu_on_logging_callbacks(void)
{
    return g_mzarch_main.cpu->mread_cb == memory_read_with_logging_cb;
}


/**
 * @brief Naplní dvě buňky ve dvou různých regionech (bus a ram).
 *
 * Volá se před startem simulovaného emu vlákna (jednovláknově).
 */
static void fill_counters(void)
{
    g_mhmap.bus[TEST_BUS_OFFSET].x = 7;
    g_mhmap.ram[TEST_RAM_OFFSET].w = 5;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    mhmap_reset();
    /* Výchozí stav: debugger okno zavřené, heatmapa vypnutá, rychlé callbacky. */
    g_debugger.active = 0;
    g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_OFF;
    mzarch_platform_fn_debugger_state_changed(false);
    TEST_ASSERT_FALSE(cpu_on_logging_callbacks());
}

void tearDown(void)
{
    g_debugger.mhmap_mode = DEBUGGER_MHMAP_MODE_OFF;
    mzarch_platform_fn_debugger_state_changed(false);
    mhmap_reset();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se callbacky CPU při zapnutí heatmapy nepřepnou.
 *
 * Režim (flag) UI zapíše hned (vzor cpuhist); swap callbacků se smí
 * provést jen na emu vlákně, které tu neběží.
 */
void test_set_mode_no_swap_without_emu_thread(void)
{
    TEST_ASSERT_FALSE(dbg_ui_mhmap_set_mode(DEBUGGER_MHMAP_MODE_ALWAYS));
    TEST_ASSERT_FALSE(cpu_on_logging_callbacks());

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se Reset (všechny countery) neprovede.
 */
void test_reset_not_applied_without_emu_thread(void)
{
    fill_counters();

    TEST_ASSERT_FALSE(dbg_ui_mhmap_reset());
    TEST_ASSERT_EQUAL_UINT32(7, g_mhmap.bus[TEST_BUS_OFFSET].x);
    TEST_ASSERT_EQUAL_UINT32(5, g_mhmap.ram[TEST_RAM_OFFSET].w);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se Reset region only neprovede.
 */
void test_reset_region_not_applied_without_emu_thread(void)
{
    fill_counters();

    TEST_ASSERT_FALSE(dbg_ui_mhmap_reset_region(0));
    TEST_ASSERT_EQUAL_UINT32(7, g_mhmap.bus[TEST_BUS_OFFSET].x);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se Add importovaných dat neprovede.
 */
void test_merge_not_applied_without_emu_thread(void)
{
    fill_counters();
    st_MHMAP *imp = (st_MHMAP *)calloc(1, sizeof(st_MHMAP));
    TEST_ASSERT_NOT_NULL(imp);
    imp->bus[TEST_BUS_OFFSET].x = 3;

    bool ok = dbg_ui_mhmap_merge(imp, MHMAP_MERGE_ADD);
    free(imp);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT32(7, g_mhmap.bus[TEST_BUS_OFFSET].x);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projdou všechny operace přes frontu.
 *
 * Pořadí: zapnutí (swap na logging callbacky), Add a Sub importu, reset
 * jen regionu "bus" (ram zůstane), reset všeho, vypnutí (swap zpět).
 */
void test_ops_executed_by_emu_thread(void)
{
    fill_counters();
    st_MHMAP *imp = (st_MHMAP *)calloc(1, sizeof(st_MHMAP));
    TEST_ASSERT_NOT_NULL(imp);
    imp->bus[TEST_BUS_OFFSET].x = 3;
    imp->ram[TEST_RAM_OFFSET].w = 9;    /* Sub: 5 + 9 - 9 = 5, Add sám 14 */

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok_on = dbg_ui_mhmap_set_mode(DEBUGGER_MHMAP_MODE_ALWAYS);
    bool logging_after_on = cpu_on_logging_callbacks();

    bool ok_add = dbg_ui_mhmap_merge(imp, MHMAP_MERGE_ADD);
    uint32_t bus_after_add = g_mhmap.bus[TEST_BUS_OFFSET].x;
    uint32_t ram_after_add = g_mhmap.ram[TEST_RAM_OFFSET].w;

    bool ok_sub = dbg_ui_mhmap_merge(imp, MHMAP_MERGE_SUB);
    uint32_t bus_after_sub = g_mhmap.bus[TEST_BUS_OFFSET].x;

    bool ok_region = dbg_ui_mhmap_reset_region(0);     /* region "bus" */
    uint32_t bus_after_region = g_mhmap.bus[TEST_BUS_OFFSET].x;
    uint32_t ram_after_region = g_mhmap.ram[TEST_RAM_OFFSET].w;

    bool ok_bad_region = dbg_ui_mhmap_reset_region(0xFFFFu);

    bool ok_reset = dbg_ui_mhmap_reset();
    uint32_t ram_after_reset = g_mhmap.ram[TEST_RAM_OFFSET].w;

    bool ok_off = dbg_ui_mhmap_set_mode(DEBUGGER_MHMAP_MODE_OFF);
    bool logging_after_off = cpu_on_logging_callbacks();

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);
    free(imp);

    TEST_ASSERT_TRUE(ok_on);
    TEST_ASSERT_TRUE(logging_after_on);

    TEST_ASSERT_TRUE(ok_add);
    TEST_ASSERT_EQUAL_UINT32(10, bus_after_add);
    TEST_ASSERT_EQUAL_UINT32(14, ram_after_add);
    TEST_ASSERT_TRUE(ok_sub);
    TEST_ASSERT_EQUAL_UINT32(7, bus_after_sub);

    TEST_ASSERT_TRUE(ok_region);
    TEST_ASSERT_EQUAL_UINT32(0, bus_after_region);
    TEST_ASSERT_EQUAL_UINT32(5, ram_after_region);
    TEST_ASSERT_FALSE(ok_bad_region);

    TEST_ASSERT_TRUE(ok_reset);
    TEST_ASSERT_EQUAL_UINT32(0, ram_after_reset);

    TEST_ASSERT_TRUE(ok_off);
    TEST_ASSERT_FALSE(logging_after_off);

    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.recompute_seen));
    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.merge_seen));
    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.region_seen));
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.reset_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_set_mode_no_swap_without_emu_thread);
    RUN_TEST(test_reset_not_applied_without_emu_thread);
    RUN_TEST(test_reset_region_not_applied_without_emu_thread);
    RUN_TEST(test_merge_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
