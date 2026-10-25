/**
 * @file test_snapshot_quicksave_dir.c
 * @brief Adresář Quick Save a Quick Load vůči pracovnímu adresáři (N96).
 *
 * `[SNAPSHOT] default_directory` prázdný = pracovní adresář (--work-dir,
 * g_sdlapp->paths->work_dir), relativní = vůči němu, absolutní beze změny.
 * Test podstrčí pracovní adresář do dočasného adresáře, uloží Quick Save
 * (mód Basic), ověří umístění souboru a že ho Quick Load najde.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_quicksave.h"
#include "emulator/emulator.h"


/** @brief Dočasný pracovní adresář testu (vytvoří setUp). */
static char *s_work = NULL;
/** @brief Původní pracovní adresář aplikace (vrátí tearDown). */
static char *s_saved_work = NULL;
/** @brief Původní nastavení snapshotů (vrátí tearDown). */
static st_SNAPSHOT_SETTINGS s_saved_settings;


void setUp(void)
{
    s_work = g_dir_make_tmp("mz_qs_dir_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_work);
    s_saved_work = g_sdlapp->paths->work_dir;
    g_sdlapp->paths->work_dir = g_strdup(s_work);
    s_saved_settings = g_snapshot_settings;
    g_emulator.paused = true; /* snapshot_save() vyžaduje pauzu */
    g_snapshot_settings.quicksave_mode = 0; /* Basic */
    g_strlcpy(g_snapshot_settings.quicksave_filename, "qs_n96",
              sizeof(g_snapshot_settings.quicksave_filename));
}


/**
 * @brief Smaže soubor Quick Save (Basic) v adresáři a adresář, pokud je prázdný.
 * @param dir Adresář.
 */
static void remove_qs(const char *dir)
{
    char *name = g_strdup_printf("qs_n96_mz%d.mzs", MZARCH);
    char *path = g_build_filename(dir, name, NULL);
    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(name);
}


void tearDown(void)
{
    char *sub = g_build_filename(s_work, "sub", NULL);
    remove_qs(sub);
    remove_qs(s_work);
    g_free(sub);
    g_free(g_sdlapp->paths->work_dir);
    g_sdlapp->paths->work_dir = s_saved_work;
    g_snapshot_settings = s_saved_settings;
    g_emulator.paused = false;
    g_free(s_work);
    s_work = NULL;
}


/**
 * @brief Existuje soubor Quick Save (Basic) v adresáři?
 * @param dir Adresář.
 * @return true, pokud soubor existuje.
 */
static bool qs_exists(const char *dir)
{
    char *name = g_strdup_printf("qs_n96_mz%d.mzs", MZARCH);
    char *path = g_build_filename(dir, name, NULL);
    bool ok = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    g_free(name);
    return ok;
}


void test_empty_directory_uses_work_dir(void)
{
    g_snapshot_settings.default_directory[0] = '\0';
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_quicksave());
    TEST_ASSERT_TRUE(qs_exists(s_work));
    TEST_ASSERT_FALSE(qs_exists("."));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_quickload());
}


void test_relative_directory_is_under_work_dir(void)
{
    g_strlcpy(g_snapshot_settings.default_directory, "sub",
              sizeof(g_snapshot_settings.default_directory));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_quicksave());
    char *sub = g_build_filename(s_work, "sub", NULL);
    TEST_ASSERT_TRUE(qs_exists(sub));
    g_free(sub);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_quickload());
}


void test_absolute_directory_is_kept(void)
{
    char *abs = g_build_filename(s_work, "sub", NULL);
    g_strlcpy(g_snapshot_settings.default_directory, abs,
              sizeof(g_snapshot_settings.default_directory));
    /* Pracovní adresář jinam: absolutní cesta na něm nesmí záviset. */
    g_free(g_sdlapp->paths->work_dir);
    g_sdlapp->paths->work_dir = g_strdup("does-not-matter");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_quicksave());
    TEST_ASSERT_TRUE(qs_exists(abs));
    g_free(abs);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_empty_directory_uses_work_dir);
    RUN_TEST(test_relative_directory_is_under_work_dir);
    RUN_TEST(test_absolute_directory_is_kept);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
