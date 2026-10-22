/**
 * @file test_fdc_request.c
 * @brief Zásahy GUI do FDC přes emu vlákno (fdc_request.h).
 *
 * Menu FD Controller, Alt+1..4 a file chooser mountu dřív měnily FDC
 * přímo z UI vlákna. Test ověřuje, že každý zásah fdc_request_* jen
 * zařadí požadavek a stav FDC změní až device_request_poll() (emu
 * vlákno), že se při aktivním replay neprovede a že neplatné argumenty
 * se nezařadí:
 *  - připojení řadiče, HD Patch, BUS translation,
 *  - mount a umount DSK obrazu, mount do odpojeného řadiče,
 *  - read-only a režim úložiště s remountem, sync.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#include "libs/dsk/dsk_tools.h"
#include "generic_driver/memory_driver.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Cesta testovacího DSK v s_tmpdir. */
static char *s_dsk = NULL;
/** @brief Uložené `connected` obou řadičů (obnova v tearDown). */
static unsigned s_saved_connected[FDC_INSTANCE_COUNT];


/** @brief Zapíše malý DSK (2 stopy, 16 x 256 B) do souboru. */
static void write_test_dsk(const char *path)
{
    st_HANDLER h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1024));
    size_t desc_size = dsk_tools_compute_description_size(1);
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *)g_malloc0(desc_size);
    desc->tracks = 2;
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description(desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, dsk_tools_create_image(&h, desc));
    g_free(desc);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
}


/** @brief Vyzvedne jeden výsledek a vrátí jeho stav (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


void setUp(void)
{
    if (!g_file_test(s_dsk, G_FILE_TEST_EXISTS)) write_test_dsk(s_dsk);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    for (unsigned i = 0; i < FDC_INSTANCE_COUNT; i++) s_saved_connected[i] = g_fdc[i].connected;
    g_fdc[FDC0].connected = FDC_CONNECTED;
}

void tearDown(void)
{
    for (unsigned d = 0; d < FDC_NUM_DRIVES; d++)
        if (fdc_test_drive_id_mounted(&g_fdc[FDC0], d)) fdc_umount(&g_fdc[FDC0], d);
    for (unsigned i = 0; i < FDC_INSTANCE_COUNT; i++) g_fdc[i].connected = s_saved_connected[i];
    g_fdc[FDC0].hd_patch = 1;
    g_fdc[FDC0].bus_xlate = FDC_BUS_XLATE_INVERT;
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Připojení řadiče, HD Patch a BUS translation se změní až v poll.
 */
void test_flags_change_only_in_poll(void)
{
    TEST_ASSERT_NOT_EQUAL_UINT32(0, fdc_request_set_connected(FDC1, 1));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, fdc_request_set_hd_patch(FDC0, 0));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, fdc_request_set_bus_xlate(FDC0, FDC_BUS_XLATE_PASSTHROUGH));
    g_fdc[FDC1].connected = FDC_DISCONNECTED;

    TEST_ASSERT_EQUAL_UINT(FDC_DISCONNECTED, g_fdc[FDC1].connected);
    TEST_ASSERT_EQUAL_INT(1, g_fdc[FDC0].hd_patch);
    TEST_ASSERT_EQUAL_INT(FDC_BUS_XLATE_INVERT, g_fdc[FDC0].bus_xlate);

    device_request_poll();
    TEST_ASSERT_EQUAL_UINT(FDC_CONNECTED, g_fdc[FDC1].connected);
    TEST_ASSERT_EQUAL_INT(0, g_fdc[FDC0].hd_patch);
    TEST_ASSERT_EQUAL_INT(FDC_BUS_XLATE_PASSTHROUGH, g_fdc[FDC0].bus_xlate);
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_mount(FDC_INSTANCE_COUNT, 0, s_dsk));
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_mount(FDC0, FDC_NUM_DRIVES, s_dsk));
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_mount(FDC0, 0, ""));
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_set_storage_mode(FDC0, 0, "bogus", false));
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_set_bus_xlate(FDC0, (en_FDC_BUS_XLATE)7));

    /* Cesta delší než argument zásahu se nesmí tiše oříznout a nasadit. */
    char long_path[DEVICE_REQUEST_STR_MAX + 8];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT32(0, fdc_request_mount(FDC0, 0, long_path));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Mount a umount proběhnou až v poll; do odpojeného řadiče mount selže.
 */
void test_mount_umount_in_poll(void)
{
    fdc_request_mount(FDC0, 1, s_dsk);
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));

    device_request_poll();
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    TEST_ASSERT_EQUAL_STRING(s_dsk, fdc_get_dsk_filepath(&g_fdc[FDC0], 1));
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.rc);

    fdc_request_umount(FDC0, 1);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    device_request_poll();
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    take_one();

    g_fdc[FDC0].connected = FDC_DISCONNECTED;
    fdc_request_mount(FDC0, 2, s_dsk);
    device_request_poll();
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 2));
    res = take_one();
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
}


/**
 * @brief Read-only a režim úložiště zapíše a remountne emu vlákno.
 */
void test_readonly_and_storage_mode_remount(void)
{
    fdc_mount_dskfile(&g_fdc[FDC0], 0, s_dsk);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0));
    fdc_cfg_set_readonly(&g_fdc[FDC0], 0, 0);
    fdc_cfg_set_storage_mode(&g_fdc[FDC0], 0, "cached");

    fdc_request_set_readonly(FDC0, 0, 1);
    fdc_request_set_storage_mode(FDC0, 0, "discard", false);
    TEST_ASSERT_EQUAL_INT(0, fdc_cfg_get_readonly(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_STRING("cached", fdc_cfg_get_storage_mode(&g_fdc[FDC0], 0));

    device_request_poll();
    TEST_ASSERT_EQUAL_INT(1, fdc_cfg_get_readonly(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_STRING("discard", fdc_cfg_get_storage_mode(&g_fdc[FDC0], 0));
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_DISCARD, g_fdc[FDC0].drive[0].storage_mode);

    fdc_request_sync(FDC0, 0);
    device_request_poll();
    take_one();
    take_one();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING("fdc_sync", res.name);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);

    fdc_cfg_set_readonly(&g_fdc[FDC0], 0, 0);
    fdc_cfg_set_storage_mode(&g_fdc[FDC0], 0, "cached");
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    g_replay_mode = REPLAY_MODE_RECORDING;
    fdc_request_mount(FDC0, 3, s_dsk);
    fdc_request_set_hd_patch(FDC0, 0);
    device_request_poll();

    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 3));
    TEST_ASSERT_EQUAL_INT(1, g_fdc[FDC0].hd_patch);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("fdc_request_XXXXXX", NULL);
    s_dsk = g_build_filename(s_tmpdir, "a.dsk", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_flags_change_only_in_poll);
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_mount_umount_in_poll);
    RUN_TEST(test_readonly_and_storage_mode_remount);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    g_remove(s_dsk);
    g_free(s_dsk);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
