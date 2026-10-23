/**
 * @file test_joy_request.c
 * @brief Zásahy GUI do joysticků přes emu vlákno (joy_request.h).
 *
 * Okno Joystick Setup dřív přepisovalo typ zařízení přímo z UI vlákna.
 * Test ověřuje, že joy_request_set_type() typ změní až
 * v device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovede a že neplatné zařízení nebo typ se nezařadí.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/joy/joy.h"
#include "hw-generic/joy/joy_request.h"

/** @brief Uložené typy zařízení (obnova v tearDown). */
static en_JOY_TYPE s_saved_type[JOY_DEVID_COUNT];


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    for (int i = 0; i < JOY_DEVID_COUNT; i++) s_saved_type[i] = g_joy.dev[i].type;
}

void tearDown(void)
{
    for (int i = 0; i < JOY_DEVID_COUNT; i++) g_joy.dev[i].type = s_saved_type[i];
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Typ se změní až v poll; neplatné argumenty se nezařadí.
 */
void test_set_type_in_poll(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, joy_request_set_type(-1, JOY_TYPE_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, joy_request_set_type(JOY_DEVID_COUNT, JOY_TYPE_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, joy_request_set_type(JOY_DEVID_0, 99));

    g_joy.dev[JOY_DEVID_1].type = JOY_TYPE_NONE;
    joy_request_set_type(JOY_DEVID_1, JOY_TYPE_NUM_KEYPAD);
    TEST_ASSERT_EQUAL_INT(JOY_TYPE_NONE, g_joy.dev[JOY_DEVID_1].type);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(JOY_TYPE_NUM_KEYPAD, g_joy.dev[JOY_DEVID_1].type);
    TEST_ASSERT_EQUAL_STRING("joy_set_type", take_one().name);
}


/**
 * @brief Při aktivním replay se změna typu neprovede.
 */
void test_rejected_while_replay_active(void)
{
    g_joy.dev[JOY_DEVID_0].type = JOY_TYPE_NONE;
    g_replay_mode = REPLAY_MODE_RECORDING;
    joy_request_set_type(JOY_DEVID_0, JOY_TYPE_NUM_KEYPAD);
    device_request_poll();

    TEST_ASSERT_EQUAL_INT(JOY_TYPE_NONE, g_joy.dev[JOY_DEVID_0].type);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_set_type_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
