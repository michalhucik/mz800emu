/**
 * @file test_devbus_snapshot.c
 * @brief devbus: komponenta snapshotu, kompatibilita hashe periph, zásahy
 *        GUI přes device_request (plán D1, testy 5, 6 a 7 - request).
 *
 * Běží v testovacím jádru MZ-800 se zařízením devbus_dummy.c a falešným
 * adaptérem. Komponenta "devbus" se registruje jen při inicializovaném
 * devbus, proto testy po devbus_init() volají snapshot_init() znovu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "emulator.h"
#include "cfgmain.h"
#include "device_request.h"
#include "replay.h"
#include "libs/cfgfile/cfgroot.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshot_io.h"
#include "snapshot/snapshot_mgr.h"
#include "hw-generic/devbus/devbus.h"
#include "hw-generic/devbus/devbus_request.h"
#include "devbus_dummy.h"


static void fake_hooks_changed(void) { }

/** @brief Falešný adaptér (okna MMIO jako MZ-700, bez statických portů). */
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, NULL, fake_hooks_changed, NULL, NULL, NULL, NULL,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
};

static st_DEVBUS_DUMMY s_a;
static st_DEVBUS_DEVICE_DESC s_desc_a, s_desc_other, s_desc_v1;

static char *s_ini_path;
static struct st_CFGROOT *s_saved_root;
static struct st_CFGROOT *s_tmp_root;


/** @brief Spustí devbus s deskriptorem @p d (kontext s_a) a znovu zaregistruje komponenty snapshotu. */
static void start_with(const st_DEVBUS_DEVICE_DESC *d)
{
    devbus_init(&s_fake_ops);
    s_a.slot = devbus_register(d, &s_a);
    TEST_ASSERT_EQUAL_INT(0, s_a.slot);
    devbus_start();
    snapshot_init();
}

/** @brief Ukončí devbus a odregistruje komponentu "devbus". */
static void stop_devbus(void)
{
    devbus_exit();
    snapshot_init();
}

void setUp(void)
{
    memset(&s_a, 0, sizeof(s_a));
    devbus_dummy_make_desc(&s_desc_a, "dummy", "DUMMY", 0);
    devbus_dummy_make_desc(&s_desc_other, "other", "DUMMY", 0);
    devbus_dummy_make_desc(&s_desc_v1, "dummy", "DUMMY", 0);
    s_desc_v1.snap.version = 1;
    s_ini_path = g_build_filename(g_get_tmp_dir(), "mz_devbus_snap_test.ini", NULL);
    g_remove(s_ini_path);
    s_saved_root = g_cfgmain;
    s_tmp_root = cfgroot_new(s_ini_path);
    g_cfgmain = s_tmp_root;
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    g_emulator.paused = true;
    start_with(&s_desc_a);
}

void tearDown(void)
{
    stop_devbus();
    g_cfgmain = s_saved_root;
    cfgroot_destroy(s_tmp_root);
    g_remove(s_ini_path);
    g_free(s_ini_path);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    g_emulator.paused = false;
}

static uint8_t *save(size_t *size)
{
    uint8_t *buf = NULL;
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("devbus", &buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
    return buf;
}

static void load(uint8_t *buf, size_t size)
{
    en_SNAPSHOT_RESULT res = snapshot_load_from_buffer(buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
}

/** @brief Je v archivu @p buf soubor @p name? */
static bool has_entry(const uint8_t *buf, size_t size, const char *name)
{
    snapshot_io_t *io = snapshot_io_open_read_buffer(buf, size);
    TEST_ASSERT_NOT_NULL(io);
    bool r = snapshot_io_entry_exists(io, name);
    snapshot_io_close(io);
    return r;
}


/* ===================== 5. snapshot ===================== */

void test_component_registered_only_when_initialized(void)
{
    bool found = false;
    for (int i = 0; i < snapshot_mgr_get_component_count(); i++) {
        if (strcmp(snapshot_mgr_get_component_name(i), "devbus") == 0) found = true;
    }
    TEST_ASSERT_TRUE(found);
    stop_devbus();
    for (int i = 0; i < snapshot_mgr_get_component_count(); i++) {
        TEST_ASSERT_TRUE(strcmp(snapshot_mgr_get_component_name(i), "devbus") != 0);
    }
    start_with(&s_desc_a);
}

void test_save_without_device_writes_nothing(void)
{
    size_t size;
    uint8_t *buf = save(&size);
    TEST_ASSERT_FALSE(has_entry(buf, size, "devices/devbus.xml"));
    g_free(buf);
}

void test_roundtrip(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    s_a.reg = 0x12;
    s_a.extra = 0xCAFE;
    for (int i = 0; i < DUMMY_RAM_SIZE; i++) s_a.ram[i] = (uint8_t) i;
    devbus_option_set(0, DUMMY_OPT_MODE, 2);
    devbus_option_set(0, DUMMY_OPT_FILL, 0x10);
    size_t size;
    uint8_t *buf = save(&size);
    TEST_ASSERT_TRUE(has_entry(buf, size, "devices/devbus.xml"));
    TEST_ASSERT_TRUE(has_entry(buf, size, "devices/devbus/dummy/state.xml"));
    TEST_ASSERT_TRUE(has_entry(buf, size, "devices/devbus/dummy/ram.bin"));

    s_a.reg = 0;
    s_a.extra = 0;
    memset(s_a.ram, 0, sizeof(s_a.ram));
    devbus_option_set(0, DUMMY_OPT_MODE, 0);
    devbus_option_set(0, DUMMY_OPT_FILL, 0x99);
    unsigned oc = s_a.n_option_changed;
    load(buf, size);
    g_free(buf);

    TEST_ASSERT_TRUE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_HEX8(0x12, s_a.reg);
    TEST_ASSERT_EQUAL_HEX32(0xCAFE, s_a.extra);
    TEST_ASSERT_EQUAL_HEX8(200, s_a.ram[200]);
    TEST_ASSERT_EQUAL_UINT(2, devbus_option_get(0, DUMMY_OPT_MODE));
    TEST_ASSERT_EQUAL_UINT(0x10, devbus_option_get(0, DUMMY_OPT_FILL));
    TEST_ASSERT_EQUAL_UINT(oc, s_a.n_option_changed); /* load option_changed nevolá */
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_after_load);
}

void test_load_connects_disconnected_device(void)
{
    devbus_connect(0);
    s_a.reg = 0x77;
    size_t size;
    uint8_t *buf = save(&size);
    devbus_disconnect(0);
    s_a.reg = 0;
    unsigned nc = s_a.n_connect;
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(nc + 1, s_a.n_connect);
    TEST_ASSERT_EQUAL_HEX8(0x77, s_a.reg);
}

void test_load_without_block_disconnects(void)
{
    size_t size;
    uint8_t *buf = save(&size);       /* bez připojeného zařízení */
    devbus_connect(0);
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_disconnect);
}

void test_load_unknown_id_warns(void)
{
    devbus_connect(0);
    size_t size;
    uint8_t *buf = save(&size);
    stop_devbus();
    memset(&s_a, 0, sizeof(s_a));
    start_with(&s_desc_other);
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(0, s_a.n_connect);
}

void test_load_higher_version_warns(void)
{
    devbus_connect(0);
    size_t size;
    uint8_t *buf = save(&size);       /* state_version 2 */
    stop_devbus();
    memset(&s_a, 0, sizeof(s_a));
    start_with(&s_desc_v1);           /* umí jen verzi 1 */
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_FALSE(devbus_is_connected(0));
}

void test_load_lower_version_defaults(void)
{
    stop_devbus();
    memset(&s_a, 0, sizeof(s_a));
    start_with(&s_desc_v1);
    devbus_connect(0);
    s_a.extra = 0x1234;               /* verze 1 ho sice uloží, loader v1 ho ignoruje */
    size_t size;
    uint8_t *buf = save(&size);
    stop_devbus();
    memset(&s_a, 0, sizeof(s_a));
    start_with(&s_desc_a);            /* verze 2 */
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_HEX32(0x5A, s_a.extra);
}

void test_load_pins_ini(void)
{
    /* Uživatel má zařízení odpojené a fill = 0AAh; snapshot ho připojí s fill 10h. */
    devbus_connect(0);
    devbus_option_set(0, DUMMY_OPT_FILL, 0x10);
    size_t size;
    uint8_t *buf = save(&size);
    devbus_disconnect(0);
    devbus_option_set(0, DUMMY_OPT_FILL, 0xAA);
    load(buf, size);
    g_free(buf);
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    cfgroot_save(g_cfgmain);
    char *txt = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(s_ini_path, &txt, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(txt, "connected = 0"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "fill = 0xaa"));
    g_free(txt);
}


/* ===================== 6. hash periph ===================== */

void test_hash_compat(void)
{
    size_t n = 0;
    const char *const *base = replay_periph_components(&n);
    const char *with[32];
    TEST_ASSERT_TRUE(n < G_N_ELEMENTS(with));
    for (size_t i = 0; i < n; i++) with[i] = base[i];
    with[n] = "devbus";

    char h_base[65], h_with[65], h_conn[65];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(base, n, h_base, sizeof(h_base)));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(with, n + 1, h_with, sizeof(h_with)));
    TEST_ASSERT_EQUAL_STRING(h_base, h_with);

    devbus_connect(0);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(with, n + 1, h_conn, sizeof(h_conn)));
    TEST_ASSERT_TRUE(strcmp(h_base, h_conn) != 0);
}


/* ===================== 7. zásahy ===================== */

void test_request_connect_and_option(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, devbus_request_connect("nope", true));
    TEST_ASSERT_EQUAL_UINT32(0, devbus_request_option("dummy", 9, 0));
    TEST_ASSERT_EQUAL_UINT32(0, devbus_request_option("dummy", DUMMY_OPT_FLAG, 2));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());

    uint32_t seq = devbus_request_connect("dummy", true);
    TEST_ASSERT_NOT_EQUAL(0, seq);
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    device_request_poll();
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_UINT32(seq, res.seq);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, res.rc);

    TEST_ASSERT_NOT_EQUAL(0, devbus_request_option("dummy", DUMMY_OPT_MODE, 0));
    device_request_poll();
    TEST_ASSERT_EQUAL_UINT(0, devbus_option_get(0, DUMMY_OPT_MODE));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_option_changed);
    TEST_ASSERT_TRUE(device_request_take_result(&res));
}

void test_request_rejected_during_replay(void)
{
    devbus_request_connect("dummy", true);
    g_replay_mode = REPLAY_MODE_PLAYING;
    device_request_poll();
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, res.status);
}

void test_request_uninitialized(void)
{
    stop_devbus();
    TEST_ASSERT_EQUAL_UINT32(0, devbus_request_connect("dummy", true));
    TEST_ASSERT_EQUAL_UINT32(0, devbus_request_option("dummy", 0, 0));
    start_with(&s_desc_a);
}


/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_component_registered_only_when_initialized);
    RUN_TEST(test_save_without_device_writes_nothing);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_load_connects_disconnected_device);
    RUN_TEST(test_load_without_block_disconnects);
    RUN_TEST(test_load_unknown_id_warns);
    RUN_TEST(test_load_higher_version_warns);
    RUN_TEST(test_load_lower_version_defaults);
    RUN_TEST(test_load_pins_ini);
    RUN_TEST(test_hash_compat);
    RUN_TEST(test_request_connect_and_option);
    RUN_TEST(test_request_rejected_during_replay);
    RUN_TEST(test_request_uninitialized);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
