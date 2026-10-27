/**
 * @file test_devbus_debugger.c
 * @brief devbus v debuggeru: dynamická část katalogu I/O a regiony
 *        REGION_KIND_DEVICE (plán D1, test 8 a test regionů z kroku 12).
 *
 * Běží v testovacím jádru MZ-800 se zařízením devbus_dummy.c a falešným
 * adaptérem (platforma MZ-700 jen jako maska). Položky katalogu a regiony
 * zařízení jsou vidět jen s připojeným zařízením.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>

#include "debugger/io_catalog.h"
#include "debugger/dbgapi_regions.h"
#include "hw-generic/devbus/devbus.h"
#include "../peripherals/devbus_dummy.h"


static void fake_hooks_changed(void) { }

/** @brief Falešný adaptér (okna MMIO jako MZ-700, bez statických portů). */
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, NULL, fake_hooks_changed, NULL, NULL, NULL, NULL,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
};

static st_DEVBUS_DUMMY s_a;
static st_DEVBUS_DEVICE_DESC s_desc_a;

void setUp(void)
{
    memset(&s_a, 0, sizeof(s_a));
    devbus_dummy_make_desc(&s_desc_a, "dummy", "DUMMYDBG", 0);
    devbus_init(&s_fake_ops);
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    TEST_ASSERT_EQUAL_INT(0, s_a.slot);
    devbus_start();
    dbgapi_regions_refresh();
}

void tearDown(void)
{
    devbus_exit();
    dbgapi_regions_refresh();
}


/* ===================== 8. katalog I/O ===================== */

void test_io_catalog_dynamic(void)
{
    size_t base = g_io_ports_count;
    TEST_ASSERT_EQUAL_size_t(base + 2, io_catalog_count());
    TEST_ASSERT_EQUAL_PTR(&g_io_ports[0], io_catalog_at(0));
    TEST_ASSERT_TRUE(io_catalog_present(0));
    const st_IO_PORT_DESC *e = io_catalog_at(base);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("DUMMY REG", e->name);
    TEST_ASSERT_NULL(io_catalog_at(base + 2));
    TEST_ASSERT_FALSE(io_catalog_present(base + 2));

    /* Odpojené zařízení: položky nejsou přítomné, probe je nenajde. */
    TEST_ASSERT_FALSE(io_catalog_present(base));
    TEST_ASSERT_FALSE(io_catalog_present(base + 1));
    uint8_t v = 0xEE;
    int found_before = io_catalog_probe_byte(0x74, &v);

    devbus_connect(0);
    TEST_ASSERT_TRUE(io_catalog_present(base));
    TEST_ASSERT_TRUE(io_catalog_present(base + 1));
    /* Statický katalog MZ-800 port 74h nemá: najde se položka zařízení. */
    TEST_ASSERT_EQUAL_INT(0, found_before);
    TEST_ASSERT_EQUAL_INT(1, io_catalog_probe_byte(0x74, &v));
    TEST_ASSERT_EQUAL_HEX8(0, v);

    devbus_disconnect(0);
    TEST_ASSERT_FALSE(io_catalog_present(base));
}

void test_io_catalog_without_devbus(void)
{
    devbus_exit();
    TEST_ASSERT_EQUAL_size_t(g_io_ports_count, io_catalog_count());
    TEST_ASSERT_NULL(io_catalog_at(g_io_ports_count));
    devbus_init(&s_fake_ops);
}


/* ===================== regiony ===================== */

/** @brief Najde region DEVICE se sub_id @p sub v aktuálním výčtu. */
static bool find_device_region(int sub, st_REGION_DESC *out)
{
    return dbgapi_regions_find_kind(REGION_KIND_DEVICE, sub, out);
}

void test_regions_device(void)
{
    st_REGION_DESC d;
    TEST_ASSERT_FALSE(find_device_region(0, &d));
    uint32_t gen0 = dbgapi_regions_layout_gen();

    devbus_connect(0);
    dbgapi_regions_refresh();
    TEST_ASSERT_TRUE(find_device_region(0, &d));
    TEST_ASSERT_EQUAL_STRING("Dummy RAM (256 B)", d.name);
    TEST_ASSERT_EQUAL_UINT32(DUMMY_RAM_SIZE, d.size);
    TEST_ASSERT_EQUAL_INT(1, d.writable);
    TEST_ASSERT_EQUAL_INT(1, d.connected);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, d.logical_base);
    TEST_ASSERT_NOT_EQUAL(gen0, dbgapi_regions_layout_gen());

    /* Čtení */
    s_a.ram[10] = 0x5A;
    s_a.ram[11] = 0xA5;
    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(2, dbgapi_regions_read(d.id, 10, buf, 2));
    TEST_ASSERT_EQUAL_HEX8(0x5A, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, buf[1]);
    /* Ořez na konec regionu */
    TEST_ASSERT_EQUAL_INT(2, dbgapi_regions_read(d.id, DUMMY_RAM_SIZE - 2, buf, 4));

    /* Zápis + on_write */
    const uint8_t w[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT(3, dbgapi_regions_write(d.id, 100, w, 3));
    TEST_ASSERT_EQUAL_HEX8(3, s_a.ram[102]);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_region_write);

    /* Odpojení: region zmizí, staré id ze starší generace se odmítne */
    int old_id = d.id;
    uint32_t gen1 = dbgapi_regions_layout_gen();
    devbus_disconnect(0);
    dbgapi_regions_refresh();
    TEST_ASSERT_FALSE(find_device_region(0, &d));
    TEST_ASSERT_TRUE(dbgapi_regions_read_gen(old_id, gen1, 0, buf, 1) < 0);
}


/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_io_catalog_dynamic);
    RUN_TEST(test_io_catalog_without_devbus);
    RUN_TEST(test_regions_device);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
