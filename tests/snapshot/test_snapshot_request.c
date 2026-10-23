/**
 * @file test_snapshot_request.c
 * @brief Fronta požadavků na snapshot z GUI (snapshot_request.h).
 *
 * Ověřuje kontrakt modulu, přes který GUI (Load / Save Snapshot, Quick
 * Load / Quick Save) nechává snapshot vyřídit emu vlákno v pauze:
 *  - požadavek bez cesty se nezařadí,
 *  - zařazení samo nic neprovede, až snapshot_request_poll(),
 *  - poll mimo pauzu vrátí SNAPSHOT_ERR_NOT_PAUSED (pauzu hlídá snapshot
 *    vrstva), v pauze uloží i načte,
 *  - výsledky přicházejí v pořadí zařazení se shodným seq a cestou,
 *  - snapshot_request_clear() zahodí čekající požadavky i výsledky.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "emulator/emulator.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshot_request.h"
#include "state_rev.h"


/** @brief Dočasný snapshot (relativně k pracovnímu adresáři testu). */
#define TEST_SNAP_FILE "test_snapshot_request.mzs"


void setUp(void)
{
    snapshot_request_clear();
    g_remove(TEST_SNAP_FILE);
    g_emulator.paused = true;
}

void tearDown(void)
{
    snapshot_request_clear();
    g_remove(TEST_SNAP_FILE);
    g_emulator.paused = false;
}


/**
 * @brief SAVE a LOAD bez cesty se nezařadí, Quick požadavky cestu nepotřebují.
 */
void test_submit_requires_path(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, snapshot_request_submit(SNAPSHOT_REQ_SAVE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, snapshot_request_submit(SNAPSHOT_REQ_LOAD, "", NULL));
    TEST_ASSERT_EQUAL_INT(0, snapshot_request_pending());

    TEST_ASSERT_NOT_EQUAL_UINT32(0, snapshot_request_submit(SNAPSHOT_REQ_QUICKLOAD, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, snapshot_request_pending());
}


/**
 * @brief Zařazení nic neprovede; poll mimo pauzu vrátí NOT_PAUSED.
 */
void test_poll_executes_and_checks_pause(void)
{
    g_emulator.paused = false;
    uint32_t seq = snapshot_request_submit(SNAPSHOT_REQ_SAVE, TEST_SNAP_FILE, "desc");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);

    st_SNAPSHOT_REQ_RESULT r;
    TEST_ASSERT_FALSE(snapshot_request_take_result(&r));
    TEST_ASSERT_FALSE(g_file_test(TEST_SNAP_FILE, G_FILE_TEST_EXISTS));

    snapshot_request_poll();
    TEST_ASSERT_EQUAL_INT(0, snapshot_request_pending());
    TEST_ASSERT_TRUE(snapshot_request_take_result(&r));
    TEST_ASSERT_EQUAL_UINT32(seq, r.seq);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_REQ_SAVE, r.kind);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_NOT_PAUSED, r.result);
    TEST_ASSERT_EQUAL_STRING(TEST_SNAP_FILE, r.path);
    TEST_ASSERT_FALSE(g_file_test(TEST_SNAP_FILE, G_FILE_TEST_EXISTS));
}


/**
 * @brief V pauze se uloží i načte, výsledky v pořadí zařazení.
 */
void test_save_then_load_in_order(void)
{
    uint32_t s1 = snapshot_request_submit(SNAPSHOT_REQ_SAVE, TEST_SNAP_FILE, NULL);
    uint32_t s2 = snapshot_request_submit(SNAPSHOT_REQ_LOAD, TEST_SNAP_FILE, NULL);
    TEST_ASSERT_TRUE(s2 > s1);
    TEST_ASSERT_EQUAL_INT(2, snapshot_request_pending());

    snapshot_request_poll();
    TEST_ASSERT_EQUAL_INT(0, snapshot_request_pending());
    TEST_ASSERT_TRUE(g_file_test(TEST_SNAP_FILE, G_FILE_TEST_EXISTS));

    st_SNAPSHOT_REQ_RESULT r;
    TEST_ASSERT_TRUE(snapshot_request_take_result(&r));
    TEST_ASSERT_EQUAL_UINT32(s1, r.seq);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_REQ_SAVE, r.kind);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, r.result);

    TEST_ASSERT_TRUE(snapshot_request_take_result(&r));
    TEST_ASSERT_EQUAL_UINT32(s2, r.seq);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_REQ_LOAD, r.kind);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, r.result);

    TEST_ASSERT_FALSE(snapshot_request_take_result(&r));
}


/**
 * @brief Revize stavu: uložení snapshotu ji nezvedne, načtení ano.
 */
void test_load_bumps_state_rev(void)
{
    g_emulator.paused = true;
    snapshot_request_submit(SNAPSHOT_REQ_SAVE, TEST_SNAP_FILE, NULL);
    uint64_t rev0 = g_state_rev;
    snapshot_request_poll();
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(rev0, g_state_rev, "uložení snapshotu stav nemění");
    snapshot_request_submit(SNAPSHOT_REQ_LOAD, TEST_SNAP_FILE, NULL);
    snapshot_request_poll();
    TEST_ASSERT_TRUE_MESSAGE(g_state_rev > rev0, "načtení snapshotu zvedne revizi stavu");
    st_SNAPSHOT_REQ_RESULT r;
    while (snapshot_request_take_result(&r)) TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, r.result);
}


/**
 * @brief Clear zahodí čekající požadavky i nevyzvednuté výsledky.
 */
void test_clear_drops_everything(void)
{
    (void)snapshot_request_submit(SNAPSHOT_REQ_SAVE, TEST_SNAP_FILE, NULL);
    snapshot_request_poll();
    (void)snapshot_request_submit(SNAPSHOT_REQ_QUICKSAVE, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(1, snapshot_request_pending());

    snapshot_request_clear();
    TEST_ASSERT_EQUAL_INT(0, snapshot_request_pending());
    st_SNAPSHOT_REQ_RESULT r;
    TEST_ASSERT_FALSE(snapshot_request_take_result(&r));

    /* Poll po clear nic neprovede. */
    snapshot_request_poll();
    TEST_ASSERT_FALSE(snapshot_request_take_result(&r));
}


int main(void)
{
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_submit_requires_path);
    RUN_TEST(test_poll_executes_and_checks_pause);
    RUN_TEST(test_save_then_load_in_order);
    RUN_TEST(test_load_bumps_state_rev);
    RUN_TEST(test_clear_drops_everything);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
