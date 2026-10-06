/**
 * @file test_freeze_window_queue.c
 * @brief Zafrození a uvolnění bajtu z Memory Browseru jde přes CMDRQ
 *        frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v kontextovém menu Memory Browseru
 * (membrowser_hexview.cpp): UI vlákno dřív volalo přímo freeze_add()
 * a freeze_remove(), které bez synchronizace přepisují pole slotu
 * a příznak in_use v tabulce, kterou emu vlákno jednou za snímek
 * prochází ve freeze_apply_all() a podle ní zapisuje do paměti. Nově
 * menu volá UI helpery dbg_ui_freeze_add / dbg_ui_freeze_remove
 * z dbgapi_helpers.cpp, které odešlou DBGAPI_CMD_FREEZE_ADD / _REMOVE
 * a operaci vykoná emu vlákno.
 *
 * Ověřuje se přes skutečné helpery:
 *  - bez vlákna, které frontu obsluhuje, se operace NEprovede (helper
 *    vrátí false po timeoutu, tabulka beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) operace projde, emu vlákno vidí očekávané příkazy
 *    a výsledek operace (plná tabulka, neexistující záznam) se vrátí UI.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/freeze/freeze.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Druh regionu použitý v testu (hodnota se jen porovnává). */
#define TEST_KIND   1

/** @brief Sub-id použité v testu. */
#define TEST_SUB    0

/** @brief Offset zafrozeného bajtu. */
#define TEST_OFFSET 0x1234u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint add_seen;      /**< Počet vyzvednutých FREEZE_ADD. */
    gint remove_seen;   /**< Počet vyzvednutých FREEZE_REMOVE. */
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
            case DBGAPI_CMD_FREEZE_ADD:
                g_atomic_int_inc(&fe->add_seen);
                break;
            case DBGAPI_CMD_FREEZE_REMOVE:
                g_atomic_int_inc(&fe->remove_seen);
                break;
            default:
                break;
        };
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    freeze_init();
}

void tearDown(void)
{
    freeze_clear();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se zafrození neprovede (tabulka zůstane prázdná).
 */
void test_add_not_applied_without_emu_thread(void)
{
    TEST_ASSERT_FALSE(dbg_ui_freeze_add(TEST_KIND, TEST_SUB, TEST_OFFSET, 0xAB));
    TEST_ASSERT_EQUAL_size_t(0, freeze_count());
    TEST_ASSERT_EQUAL_UINT(0u, g_freeze_active);
    TEST_ASSERT_FALSE(freeze_is_frozen(TEST_KIND, TEST_SUB, TEST_OFFSET, NULL));

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se uvolnění neprovede (záznam zůstane).
 */
void test_remove_not_applied_without_emu_thread(void)
{
    /* Příprava v jednovláknovém kontextu. */
    TEST_ASSERT_TRUE(freeze_add(TEST_KIND, TEST_SUB, TEST_OFFSET, 0xAB));

    TEST_ASSERT_FALSE(dbg_ui_freeze_remove(TEST_KIND, TEST_SUB, TEST_OFFSET));
    TEST_ASSERT_EQUAL_size_t(1, freeze_count());
    TEST_ASSERT_TRUE(freeze_is_frozen(TEST_KIND, TEST_SUB, TEST_OFFSET, NULL));

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projde add, update, remove i chybové
 *        stavy (plná tabulka, neexistující záznam).
 */
void test_ops_executed_by_emu_thread(void)
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    /* Přidání nového záznamu. */
    bool ok_add = dbg_ui_freeze_add(TEST_KIND, TEST_SUB, TEST_OFFSET, 0xAB);
    size_t count_after_add = freeze_count();
    uint8_t v_after_add = 0;
    bool frz_after_add = freeze_is_frozen(TEST_KIND, TEST_SUB, TEST_OFFSET,
                                          &v_after_add);

    /* Stejný klíč podruhé = aktualizace hodnoty, ne nový slot. */
    bool ok_update = dbg_ui_freeze_add(TEST_KIND, TEST_SUB, TEST_OFFSET, 0xCD);
    size_t count_after_update = freeze_count();
    uint8_t v_after_update = 0;
    (void)freeze_is_frozen(TEST_KIND, TEST_SUB, TEST_OFFSET, &v_after_update);

    /* Uvolnění; druhé uvolnění téhož klíče vrátí false (záznam už není). */
    bool ok_remove = dbg_ui_freeze_remove(TEST_KIND, TEST_SUB, TEST_OFFSET);
    size_t count_after_remove = freeze_count();
    unsigned active_after_remove = g_freeze_active;
    bool ok_remove_again = dbg_ui_freeze_remove(TEST_KIND, TEST_SUB, TEST_OFFSET);

    /* Plná tabulka: FREEZE_MAX_ENTRIES záznamů přes helper, další selže. */
    bool all_filled = true;
    for (uint32_t i = 0; i < FREEZE_MAX_ENTRIES; i++)
    {
        if (!dbg_ui_freeze_add(TEST_KIND, TEST_SUB, 0x8000u + i, (uint8_t)i))
            all_filled = false;
    };
    bool ok_overflow = dbg_ui_freeze_add(TEST_KIND, TEST_SUB, 0x0100u, 0x11);
    size_t count_full = freeze_count();

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_add);
    TEST_ASSERT_EQUAL_size_t(1, count_after_add);
    TEST_ASSERT_TRUE(frz_after_add);
    TEST_ASSERT_EQUAL_HEX8(0xAB, v_after_add);

    TEST_ASSERT_TRUE(ok_update);
    TEST_ASSERT_EQUAL_size_t(1, count_after_update);
    TEST_ASSERT_EQUAL_HEX8(0xCD, v_after_update);

    TEST_ASSERT_TRUE(ok_remove);
    TEST_ASSERT_EQUAL_size_t(0, count_after_remove);
    TEST_ASSERT_EQUAL_UINT(0u, active_after_remove);
    TEST_ASSERT_FALSE(ok_remove_again);

    TEST_ASSERT_TRUE(all_filled);
    TEST_ASSERT_FALSE(ok_overflow);
    TEST_ASSERT_EQUAL_size_t(FREEZE_MAX_ENTRIES, count_full);

    /* 2 + FREEZE_MAX_ENTRIES + 1 ADD, 2 REMOVE - každý právě jednou. */
    TEST_ASSERT_EQUAL_INT(FREEZE_MAX_ENTRIES + 3, g_atomic_int_get(&fe.add_seen));
    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.remove_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_add_not_applied_without_emu_thread);
    RUN_TEST(test_remove_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
