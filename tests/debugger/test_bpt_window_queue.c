/**
 * @file test_bpt_window_queue.c
 * @brief Smazání všech breakpointů a jejich načtení ze souboru jdou přes
 *        CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh v okně Breakpoints (bpt_window.cpp): UI vlákno
 * dřív volalo přímo breakpoints_clear_all() (Delete All) a
 * breakpoints_load_from_file() / breakpoints_load_from_filepath()
 * (File -> Load, Load Breakpoints From...). Obě uvolní stringy a AST BP,
 * zkrátí pole g_breakpoints a vyčistí bptmap, které emu vlákno souběžně
 * čte při vyhodnocení BP. Nově okno volá UI helpery z dbgapi_helpers.cpp,
 * které odešlou DBGAPI_CMD_BP_CLEAR_ALL / DBGAPI_CMD_BP_LOAD_FILE
 * a operaci vykoná emu vlákno.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se operace NEprovede (helper
 *    vrátí false po timeoutu, BP i bptmap beze změny, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) operace projde a emu vlákno vidí očekávaný příkaz.
 *
 * Navíc (ui-thread-writes T6b) reset počítadla zásahů z editačního panelu
 * BP (bpt_edit_panel.cpp, tlačítko Reset): dřív breakpoints_reset_hits()
 * přímo z UI vlákna souběžně s hits++ na emu vlákně, nově
 * dbg_ui_bp_reset_hits() -> DBGAPI_CMD_BP_RESET_HITS.
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
#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Adresa BP, který je uložen v testovacím souboru. */
#define TEST_ADDR_FILE  0x1234u

/** @brief Adresa BP, který v souboru není (přidá se až po uložení). */
#define TEST_ADDR_EXTRA 0x5678u

/** @brief Dočasný soubor s breakpointy (relativně k pracovnímu adresáři testu). */
#define TEST_BPT_FILE "test_bpt_window_queue.bpt"


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy podle druhu. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint clear_seen;    /**< Počet vyzvednutých BP_CLEAR_ALL. */
    gint load_seen;     /**< Počet vyzvednutých BP_LOAD_FILE. */
    gint reset_seen;    /**< Počet vyzvednutých BP_RESET_HITS. */
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
            case DBGAPI_CMD_BP_CLEAR_ALL:
                g_atomic_int_inc(&fe->clear_seen);
                break;
            case DBGAPI_CMD_BP_LOAD_FILE:
                g_atomic_int_inc(&fe->load_seen);
                break;
            case DBGAPI_CMD_BP_RESET_HITS:
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
 * @brief Připraví stav: soubor s jedním BP (TEST_ADDR_FILE) a v paměti
 *        dva BP (TEST_ADDR_FILE ze souboru + TEST_ADDR_EXTRA navíc).
 *
 * Volá se před startem simulovaného emu vlákna (přímé volání
 * breakpoints_* je tu jednovláknové).
 */
static void prepare_file_and_extra_bp(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL(1, breakpoints_add(TEST_ADDR_FILE, "FromFile", -1));
    breakpoints_save_to_filepath(TEST_BPT_FILE);
    TEST_ASSERT_GREATER_OR_EQUAL(1, breakpoints_add(TEST_ADDR_EXTRA, "Extra", -1));
    TEST_ASSERT_EQUAL_UINT(2, breakpoints_count());
    TEST_ASSERT_TRUE(g_bptmap.bpmap[TEST_ADDR_EXTRA] != BREAKPOINT_TYPE_NONE);
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    breakpoints_clear_all();
    g_breakpoints.next_id = 1;
}

void tearDown(void)
{
    breakpoints_clear_all();
    remove(TEST_BPT_FILE);
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se Delete All neprovede (BP ani bptmap se nemažou).
 */
void test_clear_all_not_applied_without_emu_thread(void)
{
    prepare_file_and_extra_bp();

    TEST_ASSERT_FALSE(dbg_ui_bp_clear_all());
    TEST_ASSERT_EQUAL_UINT(2, breakpoints_count());
    TEST_ASSERT_TRUE(g_bptmap.bpmap[TEST_ADDR_EXTRA] != BREAKPOINT_TYPE_NONE);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se načtení ze souboru neprovede (BP beze změny).
 */
void test_load_not_applied_without_emu_thread(void)
{
    prepare_file_and_extra_bp();

    TEST_ASSERT_FALSE(dbg_ui_bp_load_from_file(TEST_BPT_FILE));
    TEST_ASSERT_EQUAL_UINT(2, breakpoints_count());
    TEST_ASSERT_NOT_NULL(breakpoints_find_by_addr(TEST_ADDR_EXTRA));
    TEST_ASSERT_TRUE(g_bptmap.bpmap[TEST_ADDR_EXTRA] != BREAKPOINT_TYPE_NONE);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projde načtení i smazání přes frontu.
 *
 * Načtení nahradí oba BP v paměti jediným BP ze souboru (BP navíc zmizí
 * z pole i z bptmap), následné Delete All smaže vše.
 */
void test_ops_executed_by_emu_thread(void)
{
    prepare_file_and_extra_bp();

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok_load = dbg_ui_bp_load_from_file(TEST_BPT_FILE);
    unsigned count_after_load = breakpoints_count();
    bool file_bp_after_load = ( breakpoints_find_by_addr(TEST_ADDR_FILE) != NULL );
    bool extra_bp_after_load = ( breakpoints_find_by_addr(TEST_ADDR_EXTRA) != NULL );
    int extra_map_after_load = g_bptmap.bpmap[TEST_ADDR_EXTRA];
    int file_map_after_load = g_bptmap.bpmap[TEST_ADDR_FILE];

    bool ok_clear = dbg_ui_bp_clear_all();
    unsigned count_after_clear = breakpoints_count();
    int file_map_after_clear = g_bptmap.bpmap[TEST_ADDR_FILE];

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_load);
    TEST_ASSERT_EQUAL_UINT(1, count_after_load);
    TEST_ASSERT_TRUE(file_bp_after_load);
    TEST_ASSERT_FALSE(extra_bp_after_load);
    TEST_ASSERT_TRUE(extra_map_after_load == BREAKPOINT_TYPE_NONE);
    TEST_ASSERT_TRUE(file_map_after_load != BREAKPOINT_TYPE_NONE);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.load_seen));

    TEST_ASSERT_TRUE(ok_clear);
    TEST_ASSERT_EQUAL_UINT(0, count_after_clear);
    TEST_ASSERT_TRUE(file_map_after_clear == BREAKPOINT_TYPE_NONE);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.clear_seen));
}


/**
 * @brief Bez emu vlákna se reset počítadla zásahů neprovede (hits zůstane).
 */
void test_reset_hits_not_applied_without_emu_thread(void)
{
    int id = breakpoints_add(TEST_ADDR_FILE, "Hits", -1);
    TEST_ASSERT_GREATER_OR_EQUAL(1, id);
    breakpoints_increment_hits(id);
    breakpoints_increment_hits(id);

    TEST_ASSERT_FALSE(dbg_ui_bp_reset_hits(id));
    TEST_ASSERT_EQUAL_UINT64(2, breakpoints_find_by_id(id)->hits);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem reset projde; neexistující ID selže.
 */
void test_reset_hits_executed_by_emu_thread(void)
{
    int id = breakpoints_add(TEST_ADDR_FILE, "Hits", -1);
    TEST_ASSERT_GREATER_OR_EQUAL(1, id);
    breakpoints_increment_hits(id);
    breakpoints_increment_hits(id);
    breakpoints_increment_hits(id);

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok_reset = dbg_ui_bp_reset_hits(id);
    uint64_t hits_after = breakpoints_find_by_id(id)->hits;
    bool ok_unknown = dbg_ui_bp_reset_hits(id + 1000);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_reset);
    TEST_ASSERT_EQUAL_UINT64(0, hits_after);
    TEST_ASSERT_FALSE(ok_unknown);
    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.reset_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_clear_all_not_applied_without_emu_thread);
    RUN_TEST(test_load_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);
    RUN_TEST(test_reset_hits_not_applied_without_emu_thread);
    RUN_TEST(test_reset_hits_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
