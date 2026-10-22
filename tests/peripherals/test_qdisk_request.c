/**
 * @file test_qdisk_request.c
 * @brief Zásahy GUI do Quick Disku přes emu vlákno (qdisk_request.h).
 *
 * Menu Quick Disk, file chooser mountu a popup přepnutí úložiště dřív
 * měnily QD přímo z UI vlákna. Test ověřuje, že zásahy qdisk_request_*
 * stav QD změní až v device_request_poll() (emu vlákno), že se při
 * aktivním replay neprovedou a že neplatné argumenty se nezařadí:
 *  - způsob připojení, ochrana proti zápisu,
 *  - mount adresáře virtuálního QD a umount,
 *  - vytvoření obrazu .MZQ a jeho mount.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/qdisk/qdisk_request.h"

/** @brief Dočasný adresář testu (zároveň adresář virtuálního QD). */
static char *s_tmpdir = NULL;
/** @brief Uložený stav připojení a typu QD (obnova v tearDown). */
static unsigned s_saved_connected;
/** @brief Uložený typ QD. */
static unsigned s_saved_type;
/** @brief Uložená ochrana proti zápisu. */
static int s_saved_wp;


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
    s_saved_connected = g_qdisk.connected;
    s_saved_type = g_qdisk.type;
    s_saved_wp = qdisc_get_write_protected();
}

void tearDown(void)
{
    qdisk_close();
    g_qdisk.connected = s_saved_connected;
    g_qdisk.type = s_saved_type;
    qdisk_set_write_protected(s_saved_wp);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, qdisk_request_mount(NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, qdisk_request_mount("", 1));
    TEST_ASSERT_EQUAL_UINT32(0, qdisk_request_create_image(""));
    TEST_ASSERT_EQUAL_UINT32(0, qdisk_request_set_storage_mode("bogus", false));
    TEST_ASSERT_EQUAL_UINT32(0, qdisk_request_set_connection((en_QDISK_REQ_CONNECTION)9));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Ochrana proti zápisu a způsob připojení se změní až v poll.
 */
void test_wp_and_connection_in_poll(void)
{
    qdisk_set_write_protected(0);
    qdisk_request_set_write_protected(1);
    TEST_ASSERT_EQUAL_INT(0, qdisc_get_write_protected());
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(1, qdisc_get_write_protected());
    take_one();

    g_qdisk.connected = QDISK_DISCONNECTED;
    qdisk_request_set_connection(QDISK_REQ_VIRTUAL);
    TEST_ASSERT_FALSE(QDISK_TEST_CONNECTED);
    device_request_poll();
    TEST_ASSERT_TRUE(QDISK_TEST_CONNECTED);
    TEST_ASSERT_TRUE(QDISK_TEST_TYPE_VIRTUAL);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    qdisk_request_set_connection(QDISK_REQ_DISCONNECTED);
    device_request_poll();
    TEST_ASSERT_FALSE(QDISK_TEST_CONNECTED);
    take_one();
}


/**
 * @brief Mount adresáře virtuálního QD a umount proběhnou až v poll;
 *        mount souboru při typu VIRTUAL se odmítne.
 */
void test_virtual_mount_umount_in_poll(void)
{
    g_qdisk.connected = QDISK_CONNECTED;
    g_qdisk.type = QDISK_TYPE_VIRTUAL;
    qdisk_umount();

    qdisk_request_mount(s_tmpdir, 1);
    TEST_ASSERT_FALSE(QDISK_TEST_VIRT_PATH_IS_SET);
    device_request_poll();
    TEST_ASSERT_TRUE(QDISK_TEST_VIRT_PATH_IS_SET);
    TEST_ASSERT_EQUAL_STRING(s_tmpdir, qdisc_get_ui_virt_filepath());
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    qdisk_request_mount("image.mzq", 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);

    qdisk_request_umount();
    TEST_ASSERT_TRUE(QDISK_TEST_VIRT_PATH_IS_SET);
    device_request_poll();
    TEST_ASSERT_FALSE(QDISK_TEST_VIRT_PATH_IS_SET);
    take_one();
}


/**
 * @brief Vytvoření obrazu proběhne až v poll.
 */
void test_create_image_in_poll(void)
{
    char *img = g_build_filename(s_tmpdir, "new.mzq", NULL);
    g_remove(img);

    qdisk_request_create_image(img);
    TEST_ASSERT_FALSE(g_file_test(img, G_FILE_TEST_EXISTS));
    device_request_poll();
    TEST_ASSERT_TRUE(g_file_test(img, G_FILE_TEST_EXISTS));
    take_one();

    g_remove(img);
    g_free(img);
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    qdisk_set_write_protected(0);
    g_replay_mode = REPLAY_MODE_PLAYING;
    qdisk_request_set_write_protected(1);
    device_request_poll();

    TEST_ASSERT_EQUAL_INT(0, qdisc_get_write_protected());
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("qdisk_request_XXXXXX", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_wp_and_connection_in_poll);
    RUN_TEST(test_virtual_mount_umount_in_poll);
    RUN_TEST(test_create_image_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
