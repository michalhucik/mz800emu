/*
 * test_snapshot_config.c - INI sekce [SNAPSHOT]
 *
 * Ověřuje, že snapshot_config_init() načte hodnoty z INI souboru a propaguje
 * je do g_snapshot_settings. Globální cfgroot_propagate se v emulátoru
 * nevolá, takže každý modul musí sekci načíst a propagovat sám.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/cfgmain.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_config.h"
#include "libs/cfgfile/cfgroot.h"

void setUp(void) { }
void tearDown(void) { }

/* Načte INI se zadaným obsahem přes snapshot_config_init() a vrátí výsledná
 * nastavení; původní globální stav (g_cfgmain, g_snapshot_settings) obnoví. */
static st_SNAPSHOT_SETTINGS load_from_ini(const char *content)
{
    const char *ini = "tests/data/tmp/test_snapshot_config.ini";
    if (content) {
        TEST_ASSERT_TRUE(g_file_set_contents(ini, content, -1, NULL));
    } else {
        g_remove(ini);
    }

    st_SNAPSHOT_SETTINGS saved = g_snapshot_settings;
    struct st_CFGROOT *saved_root = g_cfgmain;
    g_cfgmain = cfgroot_new(ini);
    snapshot_config_init();
    st_SNAPSHOT_SETTINGS loaded = g_snapshot_settings;
    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;
    g_snapshot_settings = saved;
    g_remove(ini);
    return loaded;
}

/* Nevýchozí hodnoty z INI se po init propagují do nastavení. */
static void test_ini_loaded_from_file(void)
{
    st_SNAPSHOT_SETTINGS s = load_from_ini(
        "[SNAPSHOT]\n"
        "include_ramdisk = 1\n"
        "include_memext = 0\n"
        "compression_level = 0x09\n"
        "default_directory = D:/snaps\n"
        "quicksave_filename = myquick\n"
        "quicksave_mode = 0x02\n"
        "quicksave_max_slots = 0x0c\n"
        "load_resume_mode = 0x00\n"
        "quickload_resume_mode = 0x02\n");

    TEST_ASSERT_TRUE(s.include_ramdisk);
    TEST_ASSERT_FALSE(s.include_memext);
    TEST_ASSERT_EQUAL_INT(9, s.compression_level);
    TEST_ASSERT_EQUAL_STRING("D:/snaps", s.default_directory);
    TEST_ASSERT_EQUAL_STRING("myquick", s.quicksave_filename);
    TEST_ASSERT_EQUAL_INT(2, s.quicksave_mode);
    TEST_ASSERT_EQUAL_INT(12, s.quicksave_max_slots);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_RESUME_ALWAYS_RUN, s.load_resume_mode);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_RESUME_PREVIOUS, s.quickload_resume_mode);
}

/* Bez INI souboru se propagují výchozí hodnoty. */
static void test_ini_missing_gives_defaults(void)
{
    st_SNAPSHOT_SETTINGS s = load_from_ini(NULL);

    TEST_ASSERT_FALSE(s.include_ramdisk);
    TEST_ASSERT_TRUE(s.include_memext);
    TEST_ASSERT_EQUAL_INT(6, s.compression_level);
    TEST_ASSERT_EQUAL_STRING("", s.default_directory);
    TEST_ASSERT_EQUAL_STRING("quicksave", s.quicksave_filename);
    TEST_ASSERT_EQUAL_INT(0, s.quicksave_mode);
    TEST_ASSERT_EQUAL_INT(5, s.quicksave_max_slots);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_RESUME_ALWAYS_PAUSE, s.load_resume_mode);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_RESUME_ALWAYS_PAUSE, s.quickload_resume_mode);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_ini_loaded_from_file);
    RUN_TEST(test_ini_missing_gives_defaults);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
