/**
 * @file test_ramdisk_backup_path.c
 * @brief Umístění záložních souborů RAM disků (ramdisk_*_backup_file).
 *
 * Výchozí jméno zálohy MZ-1R18 `rd-<platforma>.dat` (a `pezik_*.dat`) je
 * relativní. Dřív se otevíralo vůči aktuálnímu adresáři procesu, takže
 * emulátor spuštěný testy z kořene repa při ukončení přepsal zálohu RAM
 * disku, kterou používá GUI. Test ověřuje, že relativní cesta míří do
 * konfiguračního adresáře, absolutní cesta se použije beze změny a uložený
 * obsah se načte zpět.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "main.h"
#include "libs/sdlapp/sdlapp_paths.h"
#include "hw-generic/ramdisk/ramdisk.h"


/** @brief Velikost testovací zálohy v bajtech. */
#define TEST_RD_SIZE 4096

/** @brief Relativní jméno zálohy; nesmí kolidovat s výchozím jménem. */
#define TEST_RD_NAME "ramdisk-backup-path-test.dat"

/** @brief Dočasný konfigurační adresář testu (vlastní ho test). */
static char *s_cfg_dir;

/** @brief Původní cfg_dir, obnoví se v tearDown(). */
static char *s_saved_cfg_dir;


void setUp(void)
{
    s_cfg_dir = g_dir_make_tmp("mz_rd_cfg_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_cfg_dir);
    s_saved_cfg_dir = g_sdlapp->paths->cfg_dir;
    g_sdlapp->paths->cfg_dir = s_cfg_dir;
}

void tearDown(void)
{
    g_sdlapp->paths->cfg_dir = s_saved_cfg_dir;
    char *p = g_build_filename(s_cfg_dir, TEST_RD_NAME, NULL);
    g_remove(p);
    g_free(p);
    g_rmdir(s_cfg_dir);
    g_free(s_cfg_dir);
    s_cfg_dir = NULL;
}


/**
 * @brief Relativní cesta se uloží do cfg_dir (ne do cwd) a načte zpět.
 */
void test_relative_path_goes_to_cfg_dir(void)
{
    static uint8_t out[TEST_RD_SIZE], in[TEST_RD_SIZE];
    for (unsigned i = 0; i < TEST_RD_SIZE; i++) out[i] = (uint8_t) (i * 7 + 3);
    memset(in, 0, sizeof(in));

    TEST_ASSERT_FALSE(g_file_test(TEST_RD_NAME, G_FILE_TEST_EXISTS));
    ramdisk_save_backup_file(out, TEST_RD_NAME, TEST_RD_SIZE);

    char *expected = g_build_filename(s_cfg_dir, TEST_RD_NAME, NULL);
    GStatBuf st;
    TEST_ASSERT_EQUAL_INT(0, g_stat(expected, &st));
    TEST_ASSERT_EQUAL_INT(TEST_RD_SIZE, (int) st.st_size);
    g_free(expected);
    TEST_ASSERT_FALSE(g_file_test(TEST_RD_NAME, G_FILE_TEST_EXISTS));

    ramdisk_load_backup_file(in, TEST_RD_NAME, TEST_RD_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(out, in, TEST_RD_SIZE);
}


/**
 * @brief Absolutní cesta se použije beze změny (mimo cfg_dir).
 */
void test_absolute_path_unchanged(void)
{
    static uint8_t out[TEST_RD_SIZE], in[TEST_RD_SIZE];
    memset(out, 0xA5, sizeof(out));
    memset(in, 0, sizeof(in));

    char *other = g_dir_make_tmp("mz_rd_abs_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(other);
    char *abs = g_build_filename(other, "abs.dat", NULL);

    ramdisk_save_backup_file(out, abs, TEST_RD_SIZE);
    TEST_ASSERT_TRUE(g_file_test(abs, G_FILE_TEST_IS_REGULAR));
    char *in_cfg = g_build_filename(s_cfg_dir, "abs.dat", NULL);
    TEST_ASSERT_FALSE(g_file_test(in_cfg, G_FILE_TEST_EXISTS));

    ramdisk_load_backup_file(in, abs, TEST_RD_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(out, in, TEST_RD_SIZE);

    g_remove(abs);
    g_rmdir(other);
    g_free(in_cfg);
    g_free(abs);
    g_free(other);
}


/**
 * @brief Neexistující záloha není chyba a buffer nechá beze změny.
 */
void test_missing_file_keeps_buffer(void)
{
    static uint8_t in[TEST_RD_SIZE];
    memset(in, 0x5A, sizeof(in));
    ramdisk_load_backup_file(in, TEST_RD_NAME, TEST_RD_SIZE);
    for (unsigned i = 0; i < TEST_RD_SIZE; i++) TEST_ASSERT_EQUAL_HEX8(0x5A, in[i]);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_relative_path_goes_to_cfg_dir);
    RUN_TEST(test_absolute_path_unchanged);
    RUN_TEST(test_missing_file_keeps_buffer);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
