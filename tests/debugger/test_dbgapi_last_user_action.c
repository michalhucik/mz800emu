/**
 * @file test_dbgapi_last_user_action.c
 * @brief Testy záznamu poslední akce uživatele (last_user_action).
 *
 * dbgapi_get_last_user_action() má hlásit poslední příkaz z GUI, který
 * není čtením: okna debuggeru čtou stav (registry, paměť) periodicky
 * a záznam by se jinak měnil bez akce člověka. Testy ověřují:
 *  - čtení (druh DBGAPI_CMD_KIND_READ) s původem USER záznam nemění,
 *  - u příkazu s operací v parametru (COVERAGE, PROFILER_PC, RAMLOG)
 *    čtecí operace záznam nemění, měnicí ho přepíše,
 *  - řízení běhu a změna stavu s původem USER ho přepíší,
 *  - příkaz s původem MCP ho nemění.
 *
 * Příkazy se zařazují přes dbgapi_ui_submit_cmd_sync_ex() s krátkým
 * timeoutem bez emu vlákna: záznam vzniká při zařazení, výsledek čekání
 * (timeout) test nezajímá.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"

#include <glib.h>
#include <string.h>


/** @brief Fronta testu (inicializuje setUp). */
static st_DBGAPI_CMDRQ_QUEUE s_queue;


void setUp(void)
{
    dbgapi_init(&s_queue);
}


void tearDown(void)
{
    dbgapi_destroy(&s_queue);
}


/**
 * @brief Zařadí příkaz a nečeká na vyřízení (emu vlákno v testu neběží).
 * @param cmd Příkaz.
 * @param origin Původ příkazu.
 */
static void submit(en_DBGAPI_CMD cmd, en_DBGAPI_CMD_ORIGIN origin)
{
    (void) dbgapi_ui_submit_cmd_sync_ex(&s_queue, cmd, origin, NULL, NULL, 1);
}


/**
 * @brief Zařadí příkaz s parametrem a nečeká na vyřízení.
 * @param cmd Příkaz.
 * @param data Parametr příkazu; musí žít do tearDown() (slot zůstane ve
 *             frontě nevyřízený), proto ho testy drží ve statické proměnné.
 */
static void submit_param(en_DBGAPI_CMD cmd, void *data)
{
    (void) dbgapi_ui_submit_cmd_sync_ex(&s_queue, cmd, DBGAPI_CMD_ORIGIN_USER, data, NULL, 1);
}


/**
 * @brief Vrátí zaznamenaný příkaz (selže test, když záznam chybí).
 * @return Poslední zaznamenaný příkaz.
 */
static en_DBGAPI_CMD last_cmd(void)
{
    en_DBGAPI_CMD cmd = DBGAPI_CMD_NONE;
    uint64_t ts = 0;
    TEST_ASSERT_TRUE(dbgapi_get_last_user_action(&cmd, &ts));
    return cmd;
}


void test_reads_do_not_overwrite_last_action(void)
{
    submit(DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PAUSE, last_cmd());

    /* Periodická čtení oken debuggeru. */
    submit(DBGAPI_CMD_GET_ALL_REGS, DBGAPI_CMD_ORIGIN_USER);
    submit(DBGAPI_CMD_MEM_READ, DBGAPI_CMD_ORIGIN_USER);
    submit(DBGAPI_CMD_DASM, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PAUSE, last_cmd());
}


void test_user_mutation_overwrites_last_action(void)
{
    submit(DBGAPI_CMD_RUN, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN, last_cmd());

    submit(DBGAPI_CMD_MEM_WRITE, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_MEM_WRITE, last_cmd());

    /* Konfigurace debuggeru (BP) je také akce člověka. */
    submit(DBGAPI_CMD_BP_ADD, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_BP_ADD, last_cmd());
}


void test_read_operations_do_not_overwrite_last_action(void)
{
    static st_DBGAPI_COVERAGE_PARAM cov;
    static st_DBGAPI_PROFILER_PC_PARAM prof;
    static st_DBGAPI_RAMLOG_PARAM ramlog;

    submit(DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_USER);

    /* Okna pokrytí, profileru podle PC a RAM Write Log se obnovují samy. */
    memset(&cov, 0, sizeof(cov));
    cov.op = DBGAPI_COVERAGE_OP_SNAPSHOT;
    submit_param(DBGAPI_CMD_COVERAGE, &cov);
    memset(&prof, 0, sizeof(prof));
    prof.op = DBGAPI_PROFILER_PC_OP_GET;
    submit_param(DBGAPI_CMD_PROFILER_PC, &prof);
    memset(&ramlog, 0, sizeof(ramlog));
    ramlog.op = DBGAPI_RAMLOG_OP_RECENT;
    submit_param(DBGAPI_CMD_RAMLOG, &ramlog);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_PAUSE, last_cmd());
}


void test_config_operation_overwrites_last_action(void)
{
    static st_DBGAPI_COVERAGE_PARAM cov;

    submit(DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_USER);

    memset(&cov, 0, sizeof(cov));
    cov.op = DBGAPI_COVERAGE_OP_START;
    submit_param(DBGAPI_CMD_COVERAGE, &cov);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_COVERAGE, last_cmd());
}


void test_mcp_origin_is_ignored(void)
{
    submit(DBGAPI_CMD_STEP_INTO, DBGAPI_CMD_ORIGIN_USER);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STEP_INTO, last_cmd());

    submit(DBGAPI_CMD_MEM_WRITE, DBGAPI_CMD_ORIGIN_MCP);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_STEP_INTO, last_cmd());
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_reads_do_not_overwrite_last_action);
    RUN_TEST(test_user_mutation_overwrites_last_action);
    RUN_TEST(test_read_operations_do_not_overwrite_last_action);
    RUN_TEST(test_config_operation_overwrites_last_action);
    RUN_TEST(test_mcp_origin_is_ignored);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
