/**
 * @file test_bus_map.c
 * @brief Snímek Bus Map (bus_map.c) nad zařízeními devbus s falešným adaptérem.
 *
 * Běží v testovacím jádru MZ-800 (statická tabulka mz800_bus_owners.c:
 * supported true), zařízení devbus přes falešný adaptér a devbus_dummy
 * na portech bez statického vlastníka (74h-75h). Ověřuje vlastníky portů
 * a MMIO buněk, příznaky UNMAPPED / POTENTIAL_CONFLICT / CONFLICT_SEEN /
 * DIR_SPLIT, seznam zařízení (statické čipy i devbus), hlavičku (politika,
 * idle) a zkratky.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "hw-generic/devbus/devbus.h"
#include "debugger/bus_map.h"
#include "debugger/io_activity.h"
#include "../peripherals/devbus_dummy.h"


static void fake_hooks_changed(void) { }
static uint8_t s_latch = 0x3C;
static uint8_t fake_bus_latch(void) { return s_latch; }

/** @brief Falešný adaptér (okna MMIO jako MZ-700). */
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, NULL, fake_hooks_changed, NULL, NULL, NULL, NULL,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
    fake_bus_latch, NULL, NULL,
};

static st_DEVBUS_DUMMY s_a, s_b;
static st_DEVBUS_DEVICE_DESC s_desc_a, s_desc_b;
static st_BUS_MAP_SNAPSHOT s_snap;

static char *s_ini_path;
static struct st_CFGROOT *s_saved_root;
static struct st_CFGROOT *s_tmp_root;


void setUp(void)
{
    memset(&s_a, 0, sizeof(s_a));
    memset(&s_b, 0, sizeof(s_b));
    devbus_dummy_make_desc(&s_desc_a, "dummy", "DUMMY", 0);
    devbus_dummy_make_desc(&s_desc_b, "dummyb", "DUMMYB", 0);
    s_ini_path = g_build_filename(g_get_tmp_dir(), "mz_bus_map_test.ini", NULL);
    g_remove(s_ini_path);
    s_saved_root = g_cfgmain;
    s_tmp_root = cfgroot_new(s_ini_path);
    g_cfgmain = s_tmp_root;
    devbus_init(&s_fake_ops);
}

void tearDown(void)
{
    devbus_exit();
    g_cfgmain = s_saved_root;
    cfgroot_destroy(s_tmp_root);
    g_remove(s_ini_path);
    g_free(s_ini_path);
}


static const st_BUS_OWNER *find_owner(const st_BUS_PORT *p, const char *id)
{
    for (unsigned i = 0; i < p->owner_count; i++) {
        if (strcmp(p->owners[i].id, id) == 0) return &p->owners[i];
    }
    return NULL;
}


static const st_BUS_DEVICE *find_device(const char *id)
{
    for (unsigned i = 0; i < s_snap.device_count; i++) {
        if (strcmp(s_snap.devices[i].id, id) == 0) return &s_snap.devices[i];
    }
    return NULL;
}


void test_header_without_devices(void)
{
    bus_map_snapshot(&s_snap, BUS_WANT_ALL);
    TEST_ASSERT_EQUAL_UINT(800, s_snap.platform);
    TEST_ASSERT_TRUE(s_snap.supported);
    TEST_ASSERT_TRUE(s_snap.devbus_initialized);
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_REFUSE, s_snap.conflict_policy);
    TEST_ASSERT_EQUAL_INT(DEVBUS_IDLE_LATCH, s_snap.bus_idle);
    TEST_ASSERT_EQUAL_HEX8(0x3C, s_snap.idle_byte);
    TEST_ASSERT_EQUAL_UINT(0, s_snap.devbus_registered);
    /* Statické čipy MZ-800 jsou v seznamu zařízení i bez devbus. */
    TEST_ASSERT_TRUE(s_snap.device_count > 5);
    TEST_ASSERT_NOT_NULL(find_device("gdg"));
    TEST_ASSERT_NOT_NULL(find_device("mapper"));
    TEST_ASSERT_EQUAL_UINT(BUS_OWNER_STATIC, find_device("mapper")->kind);
    TEST_ASSERT_EQUAL_INT(-1, find_device("mapper")->devbus_slot);
    /* Porty bez větve ve switchi MZ-800: žádný vlastník, UNMAPPED. */
    for (unsigned p = 0x00; p <= 0x4F; p++) {
        if (p == 0x01 || p == 0x02) continue; /* CMT hack */
        TEST_ASSERT_EQUAL_UINT(0, s_snap.ports[p].owner_count);
        TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED, s_snap.ports[p].flags);
    }
    for (unsigned p = 0x74; p <= 0x77; p++) {
        TEST_ASSERT_EQUAL_UINT(0, s_snap.ports[p].owner_count);
        TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED, s_snap.ports[p].flags);
    }
    /* Statická tabulka: 0E0h mapper (IN i OUT, nebudí), 0F0h paleta OUT
     * a joystick IN (jiný čip = DIR_SPLIT, PARTIAL 3Fh), 0CEh DMD OUT
     * a stav IN (týž čip GDG: bez DIR_SPLIT). */
    const st_BUS_OWNER *mp = find_owner(&s_snap.ports[0xE0], "mapper");
    TEST_ASSERT_NOT_NULL(mp);
    TEST_ASSERT_EQUAL_UINT(BUS_OWNER_STATIC, mp->kind);
    TEST_ASSERT_EQUAL_UINT(BUS_DIR_RW, mp->dirs);
    TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, mp->drive);
    TEST_ASSERT_EQUAL_UINT(1, mp->connected);
    TEST_ASSERT_EQUAL_HEX8(0, s_snap.ports[0xE0].flags & BUS_PORT_UNMAPPED);
    const st_BUS_PORT *pf0 = &s_snap.ports[0xF0];
    TEST_ASSERT_EQUAL_UINT(2, pf0->owner_count);
    const st_BUS_OWNER *joy = find_owner(pf0, "joy0");
    TEST_ASSERT_NOT_NULL(joy);
    TEST_ASSERT_EQUAL(DEVBUS_DRIVE_PARTIAL, joy->drive);
    TEST_ASSERT_EQUAL_HEX8(0x3F, joy->drive_mask);
    TEST_ASSERT_EQUAL_UINT(BUS_DIR_R, joy->dirs);
    TEST_ASSERT_EQUAL_UINT(BUS_DIR_W, find_owner(pf0, "gdg")->dirs);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_DIR_SPLIT, pf0->flags);
    const st_BUS_PORT *pce = &s_snap.ports[0xCE];
    TEST_ASSERT_EQUAL_UINT(2, pce->owner_count);
    TEST_ASSERT_EQUAL_STRING("gdg", pce->owners[0].id);
    TEST_ASSERT_EQUAL_STRING("gdg", pce->owners[1].id);
    TEST_ASSERT_EQUAL_HEX8(0, pce->flags);
    TEST_ASSERT_EQUAL_HEX16(0xE013, s_snap.mmio[0x13].addr);
    TEST_ASSERT_EQUAL_UINT(0, s_snap.mmio[0x13].owner_count);
    TEST_ASSERT_EQUAL_UINT(1, s_snap.mmio[0x00].owner_count);
    TEST_ASSERT_EQUAL_STRING("ppi8255", s_snap.mmio[0x00].owners[0].id);
    TEST_ASSERT_TRUE(bus_map_supported());
}


void test_devbus_owners_and_flags(void)
{
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    s_b.slot = devbus_register(&s_desc_b, &s_b);
    TEST_ASSERT_EQUAL_INT(0, s_a.slot);
    TEST_ASSERT_EQUAL_INT(1, s_b.slot);
    devbus_connect(0);
    bus_map_snapshot(&s_snap, BUS_WANT_ALL);

    /* 74h: A připojené (TRISTATE), B registrované odpojené -> potenciální kolize */
    const st_BUS_PORT *p74 = &s_snap.ports[0x74];
    TEST_ASSERT_EQUAL_UINT(2, p74->owner_count);
    const st_BUS_OWNER *a = find_owner(p74, "dummy");
    const st_BUS_OWNER *b = find_owner(p74, "dummyb");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT(BUS_OWNER_DEVBUS, a->kind);
    TEST_ASSERT_EQUAL_INT(0, a->devbus_slot);
    TEST_ASSERT_EQUAL_UINT(1, a->connected);
    TEST_ASSERT_EQUAL_UINT(0, b->connected);
    TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, a->drive);
    TEST_ASSERT_EQUAL_HEX8(0xFF, a->drive_mask);
    TEST_ASSERT_EQUAL_UINT(BUS_DIR_RW, a->dirs);
    TEST_ASSERT_EQUAL_HEX16(0x74, a->range_first);
    TEST_ASSERT_EQUAL_HEX16(0x75, a->range_last);
    TEST_ASSERT_EQUAL_STRING("Du", a->abbrev);
    TEST_ASSERT_EQUAL_STRING("Dummy test device", a->name);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_POTENTIAL_CONFLICT, p74->flags);
    TEST_ASSERT_EQUAL_UINT(0, s_snap.ports[0x76].owner_count);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED, s_snap.ports[0x76].flags);

    /* MMIO 0E013h: A i B (NONE), maska MAPPED */
    const st_BUS_MMIO_CELL *c = &s_snap.mmio[0x13];
    TEST_ASSERT_EQUAL_UINT(2, c->owner_count);
    TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, c->owners[0].drive);
    TEST_ASSERT_EQUAL_UINT(BUS_DIR_W, c->owners[0].dirs);
    TEST_ASSERT_EQUAL_HEX8(DEVBUS_MMIO_MAPPED, c->when_mask);
    TEST_ASSERT_EQUAL_HEX8(0, c->flags); /* dva pozorovatelé: bez kolize */
    TEST_ASSERT_EQUAL_HEX8(DEVBUS_MMIO_DRAM, s_snap.mmio[0x14].when_mask);
    TEST_ASSERT_EQUAL_UINT(0, s_snap.mmio[0x15].owner_count);

    /* Zařízení: statické čipy MZ-800 a za nimi obě devbus */
    unsigned static_count = s_snap.device_count;
    const st_BUS_DEVICE *da = find_device("dummy");
    const st_BUS_DEVICE *db = find_device("dummyb");
    TEST_ASSERT_NOT_NULL(da);
    TEST_ASSERT_NOT_NULL(db);
    TEST_ASSERT_EQUAL_UINT(BUS_OWNER_DEVBUS, da->kind);
    TEST_ASSERT_EQUAL_INT(0, da->devbus_slot);
    TEST_ASSERT_EQUAL_UINT(1, da->connected);
    TEST_ASSERT_EQUAL_UINT(2, da->port_count);
    TEST_ASSERT_EQUAL_UINT(2, da->mmio_count);
    TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, da->drive);
    TEST_ASSERT_EQUAL_UINT(1, da->region_count);
    TEST_ASSERT_EQUAL_UINT(0, db->connected);
    TEST_ASSERT_EQUAL_UINT(2, s_snap.devbus_registered);
    TEST_ASSERT_EQUAL_UINT(1, s_snap.devbus_connected);

    /* Jen hlavička + zařízení: porty prázdné */
    bus_map_snapshot(&s_snap, BUS_WANT_DEVICES);
    TEST_ASSERT_EQUAL_UINT(0, s_snap.ports[0x74].owner_count);
    TEST_ASSERT_EQUAL_UINT(static_count, s_snap.device_count);

    /* Odpojení: 74h má jen registrované odpojené -> UNMAPPED i potenciální kolize */
    devbus_disconnect(0);
    bus_map_snapshot(&s_snap, BUS_WANT_PORTS);
    TEST_ASSERT_EQUAL_UINT(2, s_snap.ports[0x74].owner_count);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED | BUS_PORT_POTENTIAL_CONFLICT, s_snap.ports[0x74].flags);
}


void test_conflict_seen_from_devbus_counter(void)
{
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    s_b.slot = devbus_register(&s_desc_b, &s_b);
    devbus_set_conflict_policy(DEVBUS_CONFLICT_AND);
    devbus_connect(0);
    devbus_connect(1);
    bus_map_snapshot(&s_snap, BUS_WANT_PORTS);
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_AND, s_snap.conflict_policy);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_POTENTIAL_CONFLICT, s_snap.ports[0x74].flags);
    TEST_ASSERT_EQUAL_UINT64(0, s_snap.ports[0x74].conflict_hits);
    /* čtení s kolizí: čítač devbus bez sledování aktivity */
    uint8_t driven;
    (void) devbus_port_read_resolve(0x0074, 0, false, &driven);
    bus_map_snapshot(&s_snap, BUS_WANT_PORTS);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_POTENTIAL_CONFLICT | BUS_PORT_CONFLICT_SEEN, s_snap.ports[0x74].flags);
    TEST_ASSERT_EQUAL_UINT64(1, s_snap.ports[0x74].conflict_hits);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_POTENTIAL_CONFLICT, s_snap.ports[0x75].flags);
}


void test_activity_and_ghost(void)
{
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    devbus_connect(0);
    io_activity_init();
    g_io_window_tracking_active = 1;
    io_activity_record_hit(0x0174, 0x5A, true, 7);
    io_activity_record_hit(0x0074, 0x5B, false, 8);
    io_activity_record_ghost(0x0076);
    io_activity_record_ghost(0x1276);
    io_activity_record_conflict(0x0074);
    bus_map_snapshot(&s_snap, BUS_WANT_PORTS | BUS_WANT_ACTIVITY);
    TEST_ASSERT_TRUE(s_snap.tracking_active);
    TEST_ASSERT_EQUAL_UINT64(1, s_snap.ports[0x74].total_in);
    TEST_ASSERT_EQUAL_UINT64(1, s_snap.ports[0x74].total_out);
    TEST_ASSERT_EQUAL_UINT(1, s_snap.ports[0x74].last_read_valid);
    TEST_ASSERT_EQUAL_HEX8(0x5A, s_snap.ports[0x74].last_read_value);
    TEST_ASSERT_EQUAL_HEX8(0x5B, s_snap.ports[0x74].last_write_value);
    TEST_ASSERT_EQUAL_UINT64(1, s_snap.ports[0x74].conflict_hits);
    TEST_ASSERT_TRUE(s_snap.ports[0x74].flags & BUS_PORT_CONFLICT_SEEN);
    TEST_ASSERT_EQUAL_UINT64(2, s_snap.ports[0x76].ghost_hits);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED | BUS_PORT_GHOST_SEEN, s_snap.ports[0x76].flags);
    /* bez BUS_WANT_ACTIVITY: aktivita nulová, kolize z čítače devbus 0 */
    bus_map_snapshot(&s_snap, BUS_WANT_PORTS);
    TEST_ASSERT_EQUAL_UINT64(0, s_snap.ports[0x76].ghost_hits);
    TEST_ASSERT_EQUAL_HEX8(BUS_PORT_UNMAPPED, s_snap.ports[0x76].flags);
    g_io_window_tracking_active = 0;
    io_activity_reset_all();
}


void test_tracking_clients(void)
{
    g_io_window_tracking_active = 0;
    TEST_ASSERT_TRUE(io_activity_tracking_set_client(IO_TRACKING_CLIENT_IO_WINDOW, true));
    TEST_ASSERT_EQUAL_UINT8(1, g_io_window_tracking_active);
    TEST_ASSERT_FALSE(io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, true));
    TEST_ASSERT_TRUE(io_activity_tracking_client_active(IO_TRACKING_CLIENT_BUS_MAP));
    /* zavření jednoho okna sledování nevypne */
    TEST_ASSERT_FALSE(io_activity_tracking_set_client(IO_TRACKING_CLIENT_IO_WINDOW, false));
    TEST_ASSERT_EQUAL_UINT8(1, g_io_window_tracking_active);
    TEST_ASSERT_TRUE(io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, false));
    TEST_ASSERT_EQUAL_UINT8(0, g_io_window_tracking_active);
    TEST_ASSERT_FALSE(io_activity_tracking_set_client(IO_TRACKING_CLIENT_BUS_MAP, false));
    TEST_ASSERT_FALSE(io_activity_tracking_set_client(99, true));
}


void test_abbrev_and_flag_names(void)
{
    char ab[4];
    bus_map_abbrev_from_id("kp80", ab);
    TEST_ASSERT_EQUAL_STRING("K8", ab);
    bus_map_abbrev_from_id("pcg700", ab);
    TEST_ASSERT_EQUAL_STRING("P7", ab);
    bus_map_abbrev_from_id("suc80", ab);
    TEST_ASSERT_EQUAL_STRING("S8", ab);
    bus_map_abbrev_from_id("dummy", ab);
    TEST_ASSERT_EQUAL_STRING("Du", ab);
    bus_map_abbrev_from_id("x", ab);
    TEST_ASSERT_EQUAL_STRING("X", ab);
    bus_map_abbrev_from_id(NULL, ab);
    TEST_ASSERT_EQUAL_STRING("?", ab);
    TEST_ASSERT_EQUAL_STRING("unmapped", bus_map_flag_name(BUS_PORT_UNMAPPED));
    TEST_ASSERT_EQUAL_STRING("dir_split", bus_map_flag_name(BUS_PORT_DIR_SPLIT));
    TEST_ASSERT_EQUAL_STRING("potential_conflict", bus_map_flag_name(BUS_PORT_POTENTIAL_CONFLICT));
    TEST_ASSERT_EQUAL_STRING("conflict_seen", bus_map_flag_name(BUS_PORT_CONFLICT_SEEN));
    TEST_ASSERT_EQUAL_STRING("ghost_seen", bus_map_flag_name(BUS_PORT_GHOST_SEEN));
    TEST_ASSERT_NULL(bus_map_flag_name(0x80));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_header_without_devices);
    RUN_TEST(test_devbus_owners_and_flags);
    RUN_TEST(test_conflict_seen_from_devbus_counter);
    RUN_TEST(test_activity_and_ghost);
    RUN_TEST(test_tracking_clients);
    RUN_TEST(test_abbrev_and_flag_names);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
