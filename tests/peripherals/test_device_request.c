/**
 * @file test_device_request.c
 * @brief Fronta zásahů GUI do zařízení (device_request.h).
 *
 * Ověřuje kontrakt modulu, přes který menu a okna zařízení nechávají
 * zásahy provést emu vlákno:
 *  - zásah bez funkce se nezařadí,
 *  - zařazení samo nic neprovede, až device_request_poll(),
 *  - argumenty se při zařazení zkopírují (text včetně oříznutí),
 *  - výsledky přicházejí v pořadí zařazení se shodným seq a návratem,
 *  - při aktivním replay se zásah neprovede (DEVICE_REQ_REJECTED_REPLAY),
 *  - když UI drží zámek stavu zařízení, poll nic neprovede a zásahy počkají,
 *  - nevyzvednutých výsledků je nejvýš DEVICE_REQUEST_RESULTS_MAX,
 *  - device_request_clear() zahodí čekající zásahy i výsledky.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "state_rev.h"


/** @brief Počet volání testovací funkce zásahu. */
static int s_calls;
/** @brief Hodnota args->i[0] posledních volání v pořadí (nejvýš 8). */
static int s_order[8];
/** @brief Kopie textového argumentu posledního volání. */
static char s_last_str[DEVICE_REQUEST_STR_MAX];


/**
 * @brief Testovací zásah: zapíše pořadí a text, vrátí args->i[1].
 * @param args Argumenty zásahu.
 * @return args->i[1].
 */
static int test_fn(const st_DEVICE_REQ_ARGS *args)
{
    if (s_calls < 8) s_order[s_calls] = args->i[0];
    s_calls++;
    g_strlcpy(s_last_str, args->str, sizeof(s_last_str));
    return args->i[1];
}


void setUp(void)
{
    device_request_clear();
    s_calls = 0;
    memset(s_order, 0, sizeof(s_order));
    s_last_str[0] = '\0';
    g_replay_mode = REPLAY_MODE_OFF;
}

void tearDown(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Bez funkce se zásah nezařadí; se zařazením nic neproběhne.
 */
void test_submit_does_not_execute(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, device_request_submit("none", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());

    TEST_ASSERT_NOT_EQUAL_UINT32(0, device_request_submit("a", test_fn, NULL));
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    TEST_ASSERT_EQUAL_INT(0, s_calls);

    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_FALSE(device_request_take_result(&res));
}


/**
 * @brief Poll provede zásahy v pořadí zařazení, výsledky nesou seq a návrat.
 */
void test_poll_executes_in_order(void)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = 11; a.i[1] = 0;
    uint32_t s1 = device_request_submit("first", test_fn, &a);
    a.i[0] = 22; a.i[1] = -5;
    uint32_t s2 = device_request_submit("second", test_fn, &a);

    uint64_t rev0 = g_state_rev;
    device_request_poll();
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(rev0 + 2, g_state_rev, "každý provedený zásah zvedne revizi stavu");
    TEST_ASSERT_EQUAL_INT(2, s_calls);
    TEST_ASSERT_EQUAL_INT(11, s_order[0]);
    TEST_ASSERT_EQUAL_INT(22, s_order[1]);
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());

    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_UINT32(s1, res.seq);
    TEST_ASSERT_EQUAL_STRING("first", res.name);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_UINT32(s2, res.seq);
    TEST_ASSERT_EQUAL_INT(-5, res.rc);
    TEST_ASSERT_FALSE(device_request_take_result(&res));
}


/**
 * @brief Argumenty se kopírují při zařazení; dlouhý text se ořízne.
 */
void test_args_are_copied(void)
{
    st_DEVICE_REQ_ARGS a;
    TEST_ASSERT_TRUE(device_request_args_set_str(&a, "disk.dsk"));
    device_request_submit("copy", test_fn, &a);
    g_strlcpy(a.str, "changed", sizeof(a.str));

    device_request_poll();
    TEST_ASSERT_EQUAL_STRING("disk.dsk", s_last_str);

    char long_str[DEVICE_REQUEST_STR_MAX + 16];
    memset(long_str, 'x', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';
    TEST_ASSERT_FALSE(device_request_args_set_str(&a, long_str));
    TEST_ASSERT_EQUAL_size_t(DEVICE_REQUEST_STR_MAX - 1, strlen(a.str));
}


/**
 * @brief Při záznamu i přehrávání vstupů se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    uint64_t rev0 = g_state_rev;
    g_replay_mode = REPLAY_MODE_RECORDING;
    device_request_submit("rec", test_fn, NULL);
    device_request_poll();
    g_replay_mode = REPLAY_MODE_PLAYING;
    device_request_submit("play", test_fn, NULL);
    device_request_poll();

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(rev0, g_state_rev, "odmítnutý zásah revizi nezvedne");
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());

    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
}


/**
 * @brief Zámek stavu zařízení držený UI odloží zásahy na další poll.
 */
void test_state_lock_defers_execution(void)
{
    device_request_submit("locked", test_fn, NULL);

    device_state_lock();
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_INT(1, device_request_pending());
    device_state_unlock();

    device_request_poll();
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Nevyzvednuté výsledky jsou omezené, starší se zahodí.
 */
void test_results_are_capped(void)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    for (int n = 0; n < DEVICE_REQUEST_RESULTS_MAX + 5; n++)
    {
        a.i[1] = n;
        device_request_submit("many", test_fn, &a);
    }
    device_request_poll();

    int count = 0;
    int first_rc = -1;
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res))
    {
        if (count == 0) first_rc = res.rc;
        count++;
    }
    TEST_ASSERT_EQUAL_INT(DEVICE_REQUEST_RESULTS_MAX, count);
    TEST_ASSERT_EQUAL_INT(5, first_rc);
}


/**
 * @brief device_request_clear() zahodí čekající zásahy i výsledky.
 */
void test_clear_drops_everything(void)
{
    device_request_submit("done", test_fn, NULL);
    device_request_poll();
    device_request_submit("waiting", test_fn, NULL);
    device_request_clear();

    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_FALSE(device_request_take_result(&res));
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(1, s_calls);
}


int main(void)
{
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_submit_does_not_execute);
    RUN_TEST(test_poll_executes_in_order);
    RUN_TEST(test_args_are_copied);
    RUN_TEST(test_rejected_while_replay_active);
    RUN_TEST(test_state_lock_defers_execution);
    RUN_TEST(test_results_are_capped);
    RUN_TEST(test_clear_drops_everything);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
