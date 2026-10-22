/**
 * @file test_ide8_unicard_request.c
 * @brief Zásahy GUI do IDE8 a Unicardu přes emu vlákno
 *        (ide8_request.h, unicard_request.h).
 *
 * Menu IDE8 a Unicard dřív otevíraly a zavíraly obrazy disků a soubory
 * SD přímo z UI vlákna. Test ověřuje, že zásahy stav změní až
 * v device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou a že neplatné argumenty se nezařadí:
 *  - IDE8: nasazení obrazu (cesta se zapíše, původní se uvolní),
 *    neexistující obraz cestu nezmění, připojení a odpojení,
 *  - Unicard: jen čtení SD, spravovaný FDC.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/ide8/ide8.h"
#include "hw-generic/ide8/ide8_request.h"
#include "hw-generic/unicard/unicard.h"
#include "hw-generic/unicard/unicard_request.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Obraz disku IDE8 (4 KB nul). */
static char *s_img = NULL;


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


void setUp(void)
{
    if (!g_file_test(s_img, G_FILE_TEST_EXISTS))
    {
        char zeros[4096];
        memset(zeros, 0, sizeof(zeros));
        TEST_ASSERT_TRUE(g_file_set_contents(s_img, zeros, sizeof(zeros), NULL));
    }
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}

void tearDown(void)
{
    if (IDE8_TEST_SLAVE_CONNECTED) ide8_drive_close_image(&g_ide8.drive[IDE8_DRIVE_SLAVE]);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, ide8_request_set_connected(IDE8_DRIVES_COUNT, 1));
    TEST_ASSERT_EQUAL_UINT32(0, ide8_request_set_image(IDE8_DRIVE_SLAVE, ""));
    TEST_ASSERT_EQUAL_UINT32(0, unicard_request_set_fw(9));
    TEST_ASSERT_EQUAL_UINT32(0, unicard_request_set_managed_fdc(9));
    TEST_ASSERT_EQUAL_UINT32(0, unicard_request_set_sd_root(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, unicard_request_reinit("bogus"));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Nasazení obrazu IDE8 a odpojení až v poll; chybný obraz cestu nemění.
 */
void test_ide8_image_in_poll(void)
{
    ide8_request_set_image(IDE8_DRIVE_SLAVE, s_img);
    TEST_ASSERT_FALSE(IDE8_TEST_SLAVE_CONNECTED);
    device_request_poll();
    TEST_ASSERT_TRUE(IDE8_TEST_SLAVE_CONNECTED);
    TEST_ASSERT_EQUAL_STRING(s_img, ide8_drive_get_filepath(IDE8_DRIVE_SLAVE));
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    ide8_request_set_connected(IDE8_DRIVE_SLAVE, 0);
    TEST_ASSERT_TRUE(IDE8_TEST_SLAVE_CONNECTED);
    device_request_poll();
    TEST_ASSERT_FALSE(IDE8_TEST_SLAVE_CONNECTED);
    take_one();

    char *missing = g_build_filename(s_tmpdir, "missing", "hdd.img", NULL);
    ide8_request_set_image(IDE8_DRIVE_SLAVE, missing);
    device_request_poll();
    TEST_ASSERT_FALSE(IDE8_TEST_SLAVE_CONNECTED);
    TEST_ASSERT_EQUAL_STRING(s_img, ide8_drive_get_filepath(IDE8_DRIVE_SLAVE));
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    g_free(missing);

    ide8_request_set_connected(IDE8_DRIVE_SLAVE, 1);
    device_request_poll();
    TEST_ASSERT_TRUE(IDE8_TEST_SLAVE_CONNECTED);
    take_one();
}


/**
 * @brief Volby Unicardu (jen čtení SD, spravovaný FDC) se změní až v poll.
 */
void test_unicard_options_in_poll(void)
{
    int saved_ro = unicard_test_readonly();
    en_UNICARD_MANAGED_FDC saved_fdc = unicard_get_managed_fdc();

    unicard_set_readonly(0);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    unicard_request_set_readonly(1);
    unicard_request_set_managed_fdc(UNICARD_MANAGED_FDC_FDC1);
    TEST_ASSERT_EQUAL_INT(0, unicard_test_readonly());
    TEST_ASSERT_EQUAL_INT(UNICARD_MANAGED_FDC_FDC0, unicard_get_managed_fdc());

    device_request_poll();
    TEST_ASSERT_NOT_EQUAL_INT(0, unicard_test_readonly());
    TEST_ASSERT_EQUAL_INT(UNICARD_MANAGED_FDC_FDC1, unicard_get_managed_fdc());
    take_one();
    take_one();

    unicard_set_readonly(saved_ro);
    unicard_set_managed_fdc(saved_fdc);
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    g_replay_mode = REPLAY_MODE_PLAYING;
    ide8_request_set_image(IDE8_DRIVE_SLAVE, s_img);
    device_request_poll();

    TEST_ASSERT_FALSE(IDE8_TEST_SLAVE_CONNECTED);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("ide8_unicard_request_XXXXXX", NULL);
    s_img = g_build_filename(s_tmpdir, "hdd.img", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_ide8_image_in_poll);
    RUN_TEST(test_unicard_options_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    g_remove(s_img);
    g_free(s_img);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
