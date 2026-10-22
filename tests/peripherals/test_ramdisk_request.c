/**
 * @file test_ramdisk_request.c
 * @brief Zásahy GUI do RAM disků přes emu vlákno (ramdisk_request.h).
 *
 * Menu Memory Disk, výběr souboru MZ-1R18 a okno Pezik Settings dřív
 * uvolňovaly a realokovaly paměť RAM disků přímo z UI vlákna. Test
 * ověřuje, že zásahy ramdisk_request_* stav změní až v
 * device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou a že neplatné argumenty se nezařadí:
 *  - připojení a změna velikosti MZ-1R18 (typ beze změny),
 *  - připojení Pezika z menu (odpojí ostatní), konfigurace z okna,
 *  - odpojení všeho.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "hw-generic/ramdisk/ramdisk_request.h"


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
    RAMDISK_DISCONNECT_ALL();
    /* Bez záložních souborů: disk jen v paměti. */
    g_ramdisk.std.type = RAMDISK_TYPE_STD;
    g_ramdisk.std.size = RAMDISK_SIZE_64;
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].backuped = PEZIK_BACKUPED_NO;
    g_ramdisk.pezik[RAMDISK_PEZIK_68].backuped = PEZIK_BACKUPED_NO;
}

void tearDown(void)
{
    RAMDISK_DISCONNECT_ALL();
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, ramdisk_request_std_set(77, RAMDISK_REQUEST_KEEP, NULL, false));
    TEST_ASSERT_EQUAL_UINT32(0, ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, 0x55, NULL, false));
    TEST_ASSERT_EQUAL_UINT32(0, ramdisk_request_pezik_connect(5));
    TEST_ASSERT_EQUAL_UINT32(0, ramdisk_request_pezik_init(5, 1, 0xff, PEZIK_BACKUPED_NO, "", false));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Připojení a změna velikosti MZ-1R18 až v poll, typ zůstane.
 */
void test_std_connect_and_resize_in_poll(void)
{
    ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, NULL, true);
    TEST_ASSERT_FALSE(RAMDISK_TEST_STD_CONNECTED);
    TEST_ASSERT_NULL(g_ramdisk.std.memory);

    device_request_poll();
    TEST_ASSERT_TRUE(RAMDISK_TEST_STD_CONNECTED);
    TEST_ASSERT_NOT_NULL(g_ramdisk.std.memory);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_SIZE_256, NULL, false);
    TEST_ASSERT_TRUE(RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_64));
    device_request_poll();
    TEST_ASSERT_TRUE(RAMDISK_TEST_STD_SIZE(RAMDISK_SIZE_256));
    TEST_ASSERT_TRUE(RAMDISK_TEST_STD_TYPE(RAMDISK_TYPE_STD));
    /* Paměť po realokaci platí v celé nové velikosti. */
    g_ramdisk.std.memory[(RAMDISK_SIZE_256 + 1) * 0x10000 - 1] = 0x5A;
    take_one();

    ramdisk_request_disconnect_all();
    device_request_poll();
    TEST_ASSERT_TRUE(RAMDISK_TEST_NOT_CONNECTED);
    TEST_ASSERT_NULL(g_ramdisk.std.memory);
    take_one();
}


/**
 * @brief Pezik z menu odpojí MZ-1R18, okno nastavení Pezika až v poll.
 */
void test_pezik_in_poll(void)
{
    ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, NULL, true);
    ramdisk_request_pezik_connect(RAMDISK_PEZIK_68);
    TEST_ASSERT_FALSE(RAMDISK_TEST_PEZIK_68_CONNECTED);
    device_request_poll();
    TEST_ASSERT_TRUE(RAMDISK_TEST_PEZIK_68_CONNECTED);
    TEST_ASSERT_FALSE(RAMDISK_TEST_STD_CONNECTED);
    take_one();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    /* Okno: E8 připojit (odpojí MZ-1R18), 68 odpojit. */
    ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, NULL, true);
    device_request_poll();
    take_one();
    ramdisk_request_pezik_init(RAMDISK_PEZIK_E8, 1, 0x0f, PEZIK_BACKUPED_NO, "", true);
    ramdisk_request_pezik_init(RAMDISK_PEZIK_68, 0, 0xff, PEZIK_BACKUPED_NO, "", false);
    TEST_ASSERT_TRUE(RAMDISK_TEST_STD_CONNECTED);
    device_request_poll();
    TEST_ASSERT_FALSE(RAMDISK_TEST_STD_CONNECTED);
    TEST_ASSERT_TRUE(RAMDISK_TEST_PEZIK_E8_CONNECTED);
    TEST_ASSERT_EQUAL_UINT(0x0f, g_ramdisk.pezik[RAMDISK_PEZIK_E8].portmask);
    TEST_ASSERT_FALSE(RAMDISK_TEST_PEZIK_68_CONNECTED);
    take_one();
    take_one();
    g_ramdisk.pezik[RAMDISK_PEZIK_E8].portmask = 0xff;
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    g_replay_mode = REPLAY_MODE_RECORDING;
    ramdisk_request_std_set(RAMDISK_REQUEST_KEEP, RAMDISK_REQUEST_KEEP, NULL, true);
    device_request_poll();

    TEST_ASSERT_FALSE(RAMDISK_TEST_STD_CONNECTED);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_std_connect_and_resize_in_poll);
    RUN_TEST(test_pezik_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
