/**
 * @file test_mzarch_request.c
 * @brief Zásahy GUI do přepínačů stroje přes emu vlákno (mzarch_request.h).
 *
 * Menu Rear DIP Switch (SW1) a HW Compatibility Experiments (Allow PSG1)
 * dřív měnila stav přímo z UI vlákna. Test ověřuje, že zásahy
 * mzarch_request_* stav změní až v device_request_poll() (emu vlákno), že
 * se při aktivním replay neprovedou a že neplatná poloha SW1 se nezařadí.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>

#include "device_request.h"
#include "replay.h"
#include "mzarch/mzarch.h"
#include "mzarch/mzarch_request.h"
#include "mzarch/mz800/mz800_main.h"
#include "hw-generic/psg/psg.h"

/** @brief Uložená poloha SW1 (obnova v tearDown). */
static en_MZ800_MODE_SW s_saved_sw;
/** @brief Uložené povolení PSG1. */
static unsigned s_saved_psg1;


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
    s_saved_sw = g_mzarch_main.mode_sw;
    s_saved_psg1 = (unsigned)g_mzarch_main.mz800_hwcompat_allow_psg1;
}

void tearDown(void)
{
    mzarch_mode_sw_set(s_saved_sw);
    mz800_mz800_hwcompat_allow_psg1(s_saved_psg1);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief SW1 a PSG1 se změní až v poll.
 */
void test_mode_sw_and_psg1_in_poll(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, mzarch_request_mode_sw_set(5));

    mzarch_mode_sw_set(MZ800_MODE_SW_MZ700);
    mz800_mz800_hwcompat_allow_psg1(0);
    mzarch_request_mode_sw_set(MZ800_MODE_SW_MZ800);
    mzarch_request_allow_psg1(true);
    TEST_ASSERT_FALSE(MZARCH_TEST_MODE_SW_MZ800);
    TEST_ASSERT_FALSE(MZ800_TEST_MZ800_HWCOMPAT_ALLOW_PSG1);

    device_request_poll();
    TEST_ASSERT_TRUE(MZARCH_TEST_MODE_SW_MZ800);
    TEST_ASSERT_TRUE(MZ800_TEST_MZ800_HWCOMPAT_ALLOW_PSG1);
    TEST_ASSERT_TRUE(g_psg_module.stereo);
    TEST_ASSERT_EQUAL_STRING("mode_sw_set", take_one().name);
    TEST_ASSERT_EQUAL_STRING("allow_psg1", take_one().name);
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    mzarch_mode_sw_set(MZ800_MODE_SW_MZ700);
    g_replay_mode = REPLAY_MODE_PLAYING;
    mzarch_request_mode_sw_set(MZ800_MODE_SW_MZ800);
    device_request_poll();

    TEST_ASSERT_FALSE(MZARCH_TEST_MODE_SW_MZ800);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_mode_sw_and_psg1_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
