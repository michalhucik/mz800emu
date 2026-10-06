/**
 * @file test_membrowser_io_queue.c
 * @brief Zápis Memory Browseru jde přes CMDRQ frontu, ne přímo z UI vlákna.
 *
 * Regresní test pro souběh: emu_backend_write (membrowser_io.cpp) dřív
 * volal dbgapi_regions_write() přímo z UI vlákna, souběžně s emu vláknem
 * (u regionu LOGICAL přes memory_write_byte: MEM_W breakpointy, 8255/8253/
 * GDG na E000-E008, VRAM přes GDG write-format). Nově zápis odešle
 * příkaz DBGAPI_CMD_REGIONS_WRITE a vykoná ho emu vlákno.
 *
 * Ověřuje se přes skutečný backend z membrowser_io.cpp:
 *  - bez emu vlákna, které frontu obsluhuje, se zápis NEprovede (paměť
 *    beze změny, write_bytes vrátí -1 po timeoutu),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch +
 *    complete) zápis projde, emu vlákno vidí příkaz REGIONS_WRITE
 *    a data jsou v paměti.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/dbgapi_regions.h"
#include "../../src/ui-imgui/debugger/membrowser/membrowser_io.h"


/** @brief Offset v regionu RAM, kam test zapisuje (běžná RAM). */
#define TEST_OFFSET 0x4321u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a zapamatuje si
 * vyzvednuté příkazy. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;                  /**< 1 = ukončit smyčku (atomicky). */
    gint regions_write_seen;    /**< Počet vyzvednutých REGIONS_WRITE. */
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
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_REGIONS_WRITE)
            g_atomic_int_inc(&fe->regions_write_seen);
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/**
 * @brief Najde region RAM a připraví na něj backend Memory Browseru.
 * @param[out] be Backend.
 * @return region_id (test selže, pokud region chybí).
 */
static int make_ram_backend(st_HEX_VIEW_BACKEND *be)
{
    st_MEMBROWSER_REGION_SNAPSHOT snap;
    membrowser_io_refresh_regions(&snap);
    int rid = membrowser_io_find_region(&snap, REGION_KIND_RAM, 0);
    TEST_ASSERT_TRUE_MESSAGE(rid >= 0, "RAM region not found");
    membrowser_io_make_emu_backend(be, rid, true);
    TEST_ASSERT_NOT_NULL(be->write_bytes);
    return rid;
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
 * @brief Bez emu vlákna se zápis neprovede (žádný přímý zápis z UI).
 */
void test_write_not_applied_without_emu_thread(void)
{
    st_HEX_VIEW_BACKEND be;
    int rid = make_ram_backend(&be);

    uint8_t before = 0;
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(rid, TEST_OFFSET, &before, 1));
    uint8_t v = (uint8_t)(before ^ 0xA5);

    int wr = be.write_bytes(be.ctx, TEST_OFFSET, &v, 1);
    TEST_ASSERT_EQUAL_INT(-1, wr);

    uint8_t after = 0;
    TEST_ASSERT_EQUAL_INT(1, dbgapi_regions_read(rid, TEST_OFFSET, &after, 1));
    TEST_ASSERT_EQUAL_HEX8(before, after);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem zápis projde přes REGIONS_WRITE.
 */
void test_write_executed_by_emu_thread(void)
{
    st_HEX_VIEW_BACKEND be;
    int rid = make_ram_backend(&be);

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    uint8_t data[3] = { 0x11, 0x22, 0x33 };
    int wr = be.write_bytes(be.ctx, TEST_OFFSET, data, sizeof(data));

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_EQUAL_INT(3, wr);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&fe.regions_write_seen));

    uint8_t back[3] = { 0 };
    TEST_ASSERT_EQUAL_INT(3, dbgapi_regions_read(rid, TEST_OFFSET, back, 3));
    TEST_ASSERT_EQUAL_MEMORY(data, back, 3);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_write_not_applied_without_emu_thread);
    RUN_TEST(test_write_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
