/**
 * @file test_dbgapi_submit_timeout.c
 * @brief Testy synchronního submitu CMDRQ při zpoždění emu vlákna.
 *
 * Regresní testy pro chybu, kdy `dbgapi_ui_submit_cmd_sync_with_origin`
 * po vypršení timeoutu opustil slot, který zůstal ve frontě:
 *  - rozpracovaný příkaz (emu ho už vyzvedlo) se provedl, ale odesílatel
 *    dostal neúspěch a emu dál pracovalo s daty odesílatele, která mezitím
 *    zanikla,
 *  - nevyzvednutý příkaz emu později vyzvedlo jako prázdný slot,
 *  - předčasné probuzení podmínky (bez zpracování) ukončilo čekání
 *    neúspěchem.
 *
 * Dále testy dbgapi_ui_submit_cmd_sync_watched(): zaseknutý rozpracovaný
 * příkaz se ohlásí callbackem v limitu, slot se přesto neopustí.
 *
 * Emu vlákno simulují pomocná GThread vlákna, která pracují přímo
 * s frontou přes `dbgapi_emu_dequeue` a `dbgapi_emu_complete` (bez
 * `dbgapi_emu_dispatch`, aby šlo řídit dobu "zpracování").
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"

#include <glib.h>


/* ========================================================================= */
/*  Pomocné simulované emu vlákno                                            */
/* ========================================================================= */

/**
 * @brief Scénář simulovaného emu vlákna.
 *
 * Vlákno čeká `delay_before_dequeue_us`, vyzvedne slot, "zpracovává" ho
 * `processing_us`, nastaví `rq->success = handler_success` a slot dokončí.
 * Pokud `spurious_signal_us` > 0, nejdřív po této době signalizuje
 * podmínku slotu 0 bez zpracování (= simulace předčasného probuzení).
 *
 * Vlastnictví: struktura žije na zásobníku testu, test na vlákno čeká
 * přes g_thread_join před návratem.
 */
typedef struct {
    st_DBGAPI_CMDRQ_QUEUE *queue;     /**< Fronta, se kterou vlákno pracuje. */
    gulong spurious_signal_us;        /**< 0 = bez falešného signálu. */
    gulong delay_before_dequeue_us;   /**< Prodleva před vyzvednutím. */
    gulong processing_us;             /**< Délka "zpracování" po vyzvednutí. */
    bool handler_success;             /**< Hodnota zapsaná do rq->success. */
    en_DBGAPI_CMD seen_cmd;           /**< Výstup: cmd vyzvednutého slotu. */
    bool dequeued;                    /**< Výstup: zda vlákno slot získalo. */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna (viz st_FAKE_EMU).
 * @param user_data ukazatel na st_FAKE_EMU
 * @return NULL
 */
static gpointer fake_emu_thread(gpointer user_data)
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *)user_data;

    if (fe->spurious_signal_us)
    {
        g_usleep(fe->spurious_signal_us);
        st_DBGAPI_CMDRQ *slot0 = &fe->queue->cmdrq[0];
        APP_MUTEX_LOCK(slot0->mutex);
        APP_COND_SIGNAL(slot0->cond);
        APP_MUTEX_UNLOCK(slot0->mutex);
    };

    g_usleep(fe->delay_before_dequeue_us);

    /* Vyzvednout první nezrušený slot (max. 1 s čekání na submit). */
    st_DBGAPI_CMDRQ *rq = NULL;
    for (int i = 0; i < 1000 && !rq; i++)
    {
        rq = dbgapi_emu_dequeue(fe->queue);
        if (!rq)
            g_usleep(1000);
    };
    if (!rq)
        return NULL;

    fe->dequeued = true;
    fe->seen_cmd = rq->cmd;
    g_usleep(fe->processing_us);
    rq->success = fe->handler_success;
    dbgapi_emu_complete(rq);
    return NULL;
}


void setUp(void) {}
void tearDown(void) {}


/* ========================================================================= */
/*  Testy                                                                    */
/* ========================================================================= */

/**
 * @brief Rozpracovaný příkaz se nesmí opustit: submit počká na dokončení.
 *
 * Emu vyzvedne slot hned, ale zpracování trvá déle než timeout. Submit
 * musí vrátit skutečný výsledek handleru (true), ne neúspěch po timeoutu.
 */
void test_inflight_command_waits_for_completion(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    st_FAKE_EMU fe = { .queue = &q, .delay_before_dequeue_us = 0,
                       .processing_us = 150000, .handler_success = true };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok = dbgapi_ui_submit_cmd_sync_with_origin(&q, DBGAPI_CMD_PAUSE,
                                                    DBGAPI_CMD_ORIGIN_MCP,
                                                    NULL, NULL, 30);
    g_thread_join(t);

    TEST_ASSERT_TRUE(fe.dequeued);
    TEST_ASSERT_TRUE(ok);

    dbgapi_destroy(&q);
}


/**
 * @brief Nevyzvednutý příkaz se po timeoutu zruší a emu ho nevyzvedne.
 */
void test_timed_out_command_is_not_dequeued(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    bool ok = dbgapi_ui_submit_cmd_sync_with_origin(&q, DBGAPI_CMD_PAUSE,
                                                    DBGAPI_CMD_ORIGIN_MCP,
                                                    NULL, NULL, 20);
    TEST_ASSERT_FALSE(ok);

    /* Zrušený slot emu přeskočí - fronta je z pohledu emu prázdná. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&q));
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&q));

    dbgapi_destroy(&q);
}


/**
 * @brief Po zrušeném slotu projde další příkaz normálně.
 */
void test_command_after_cancelled_slot_is_processed(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    bool ok = dbgapi_ui_submit_cmd_sync_with_origin(&q, DBGAPI_CMD_PAUSE,
                                                    DBGAPI_CMD_ORIGIN_MCP,
                                                    NULL, NULL, 20);
    TEST_ASSERT_FALSE(ok);

    st_FAKE_EMU fe = { .queue = &q, .delay_before_dequeue_us = 20000,
                       .processing_us = 0, .handler_success = true };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);
    ok = dbgapi_ui_submit_cmd_sync_with_origin(&q, DBGAPI_CMD_RUN,
                                               DBGAPI_CMD_ORIGIN_MCP,
                                               NULL, NULL, 1000);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMD_RUN, fe.seen_cmd);

    dbgapi_destroy(&q);
}


/**
 * @brief Předčasné probuzení (signál bez zpracování) neukončí čekání.
 */
void test_spurious_wakeup_keeps_waiting(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    st_FAKE_EMU fe = { .queue = &q, .spurious_signal_us = 10000,
                       .delay_before_dequeue_us = 40000,
                       .processing_us = 0, .handler_success = true };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok = dbgapi_ui_submit_cmd_sync_with_origin(&q, DBGAPI_CMD_PAUSE,
                                                    DBGAPI_CMD_ORIGIN_MCP,
                                                    NULL, NULL, 1000);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok);

    dbgapi_destroy(&q);
}


/**
 * @brief Kontext callbacku zaseknutí pro testy _watched varianty.
 *
 * Vlastnictví: žije na zásobníku testu po celou dobu submitu.
 */
typedef struct {
    int calls;                 /**< Kolikrát byl callback zavolán. */
    gint64 at_us;              /**< Čas (monotonic) posledního volání. */
} st_STALL_CTX;


/**
 * @brief Callback zaseknutí: zaznamená volání do st_STALL_CTX.
 * @param user_data st_STALL_CTX*
 */
static void stall_cb(void *user_data)
{
    st_STALL_CTX *c = (st_STALL_CTX *)user_data;
    c->calls++;
    c->at_us = g_get_monotonic_time();
}


/**
 * @brief Zaseknutý rozpracovaný příkaz: callback přijde v limitu, submit
 *        ale dál čeká a vrátí skutečný výsledek (slot se neopustí).
 */
void test_watched_reports_stall_and_still_completes(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    st_FAKE_EMU fe = { .queue = &q, .delay_before_dequeue_us = 0,
                       .processing_us = 400000, .handler_success = true };
    st_STALL_CTX ctx = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    gint64 t0 = g_get_monotonic_time();
    en_DBGAPI_SUBMIT_STATUS st = dbgapi_ui_submit_cmd_sync_watched(
        &q, DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_MCP, NULL, NULL,
        30, 50, stall_cb, &ctx);
    gint64 t_end = g_get_monotonic_time();
    g_thread_join(t);

    TEST_ASSERT_TRUE(fe.dequeued);
    TEST_ASSERT_EQUAL_INT(DBGAPI_SUBMIT_OK, st);
    TEST_ASSERT_EQUAL_INT(1, ctx.calls);
    /* Callback po timeout + stall (80 ms), ale dřív než dokončení (400 ms). */
    TEST_ASSERT_TRUE(ctx.at_us - t0 >= 70000);
    TEST_ASSERT_TRUE(ctx.at_us - t0 < 350000);
    /* Submit vrátil až po dokončení handleru. */
    TEST_ASSERT_TRUE(t_end - t0 >= 390000);

    dbgapi_destroy(&q);
}


/**
 * @brief Rozpracovaný příkaz dokončený v dodatečném limitu callback nevolá.
 */
void test_watched_no_stall_when_completed_in_time(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    st_FAKE_EMU fe = { .queue = &q, .delay_before_dequeue_us = 0,
                       .processing_us = 60000, .handler_success = false };
    st_STALL_CTX ctx = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    en_DBGAPI_SUBMIT_STATUS st = dbgapi_ui_submit_cmd_sync_watched(
        &q, DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_MCP, NULL, NULL,
        20, 1000, stall_cb, &ctx);
    g_thread_join(t);

    TEST_ASSERT_EQUAL_INT(DBGAPI_SUBMIT_FAILED, st);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);

    dbgapi_destroy(&q);
}


/**
 * @brief Nevyzvednutý příkaz se zruší (TIMEOUT) a callback se nevolá.
 */
void test_watched_not_dequeued_times_out_without_stall(void) {
    st_DBGAPI_CMDRQ_QUEUE q;
    dbgapi_init(&q);

    st_STALL_CTX ctx = { 0 };
    en_DBGAPI_SUBMIT_STATUS st = dbgapi_ui_submit_cmd_sync_watched(
        &q, DBGAPI_CMD_PAUSE, DBGAPI_CMD_ORIGIN_MCP, NULL, NULL,
        20, 20, stall_cb, &ctx);

    TEST_ASSERT_EQUAL_INT(DBGAPI_SUBMIT_TIMEOUT, st);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&q));

    dbgapi_destroy(&q);
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */

int main(int argc, char *argv[]) {
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_inflight_command_waits_for_completion);
    RUN_TEST(test_timed_out_command_is_not_dequeued);
    RUN_TEST(test_command_after_cancelled_slot_is_processed);
    RUN_TEST(test_spurious_wakeup_keeps_waiting);
    RUN_TEST(test_watched_reports_stall_and_still_completes);
    RUN_TEST(test_watched_no_stall_when_completed_in_time);
    RUN_TEST(test_watched_not_dequeued_times_out_without_stall);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
