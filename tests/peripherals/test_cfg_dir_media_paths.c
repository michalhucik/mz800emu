/**
 * @file test_cfg_dir_media_paths.c
 * @brief Kořen SD karty Unicard a FLASH MemExt Luftner vůči cfg_dir.
 *
 * Výchozí kořen SD `SD-<platforma>` a výchozí soubor FLASH `flash.dat`
 * jsou relativní. Dřív se otevíraly vůči aktuálnímu adresáři procesu,
 * takže emulátor spuštěný odjinud než z adresáře programu založil prázdnou
 * SD kartu v cizím adresáři a FLASH nenačetl. Test ověřuje, že relativní
 * cesta míří do konfiguračního adresáře (ne do cwd) a absolutní cesta se
 * použije beze změny.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "main.h"
#include "libs/sdlapp/sdlapp_paths.h"
#include "hw-generic/unicard/unicard.h"
#include "hw-generic/memory/memext.h"


/** @brief Relativní kořen SD; nesmí kolidovat s výchozím jménem. */
#define TEST_SD_NAME "cfg-dir-sd-path-test"

/** @brief Relativní soubor FLASH; nesmí kolidovat s výchozím jménem. */
#define TEST_FLASH_NAME "cfg-dir-flash-path-test.dat"

/** @brief Velikost testovacího obsahu FLASH v bajtech (začátek FLASH). */
#define TEST_FLASH_LEN 4096

/** @brief Dočasný konfigurační adresář testu (vlastní ho test). */
static char *s_cfg_dir;

/** @brief Původní cfg_dir, obnoví se v tearDown(). */
static char *s_saved_cfg_dir;

/** @brief Kořen SD před testem (převedený), obnoví se v tearDown(). */
static char *s_saved_sd_root;


/**
 * @brief Rekurzivně smaže adresář nebo soubor.
 * @param path Cesta.
 */
static void rmtree(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const char *name;
            while ((name = g_dir_read_name(d)) != NULL) {
                char *child = g_build_filename(path, name, NULL);
                rmtree(child);
                g_free(child);
            }
            g_dir_close(d);
        }
        g_rmdir(path);
    } else {
        g_remove(path);
    }
}


void setUp(void)
{
    s_cfg_dir = g_dir_make_tmp("mz_cfgpath_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_cfg_dir);
    s_saved_cfg_dir = g_sdlapp->paths->cfg_dir;
    g_sdlapp->paths->cfg_dir = s_cfg_dir;
    s_saved_sd_root = g_strdup(unicard_get_sd_root_dirpath());
}

void tearDown(void)
{
    unicard_set_connected(UNICARD_CONNECTION_DISCONNECTED);
    unicard_set_sd_root_dirpath(s_saved_sd_root);
    g_free(s_saved_sd_root);
    s_saved_sd_root = NULL;
    g_sdlapp->paths->cfg_dir = s_saved_cfg_dir;
    rmtree(s_cfg_dir);
    g_free(s_cfg_dir);
    s_cfg_dir = NULL;
}


/**
 * @brief Relativní kořen SD se převede vůči cfg_dir a připojení ho
 *        založí tam, ne v aktuálním adresáři.
 */
void test_unicard_relative_sd_root_goes_to_cfg_dir(void)
{
    TEST_ASSERT_FALSE(g_file_test(TEST_SD_NAME, G_FILE_TEST_EXISTS));
    unicard_set_sd_root_dirpath(TEST_SD_NAME);

    char *expected = g_build_filename(s_cfg_dir, TEST_SD_NAME, NULL);
    TEST_ASSERT_EQUAL_STRING(expected, unicard_get_sd_root_dirpath());

    unicard_set_connected(UNICARD_CONNECTION_DISCONNECTED);
    unicard_set_connected(UNICARD_CONNECTION_CONNECTED);
    TEST_ASSERT_TRUE(g_file_test(expected, G_FILE_TEST_IS_DIR));
    TEST_ASSERT_FALSE(g_file_test(TEST_SD_NAME, G_FILE_TEST_EXISTS));
    g_free(expected);
}


/**
 * @brief Absolutní kořen SD platí beze změny.
 */
void test_unicard_absolute_sd_root_unchanged(void)
{
    char *abs = g_build_filename(s_cfg_dir, "abs-sd", NULL);
    unicard_set_sd_root_dirpath(abs);
    TEST_ASSERT_EQUAL_STRING(abs, unicard_get_sd_root_dirpath());
    g_free(abs);
}


/**
 * @brief Relativní soubor FLASH Luftner se načte z cfg_dir.
 */
void test_memext_flash_relative_path_from_cfg_dir(void)
{
    static uint8_t data[TEST_FLASH_LEN];
    for (unsigned i = 0; i < TEST_FLASH_LEN; i++) data[i] = (uint8_t) (i * 13 + 5);
    char *path = g_build_filename(s_cfg_dir, TEST_FLASH_NAME, NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *) data, TEST_FLASH_LEN, NULL));
    g_free(path);
    TEST_ASSERT_FALSE(g_file_test(TEST_FLASH_NAME, G_FILE_TEST_EXISTS));

    en_MEMEXT_TYPE saved_type = g_memext.type;
    en_MEMEXT_CONNECTION saved_conn = g_memext.connection;
    char *saved_fp = g_strdup(g_memext.flash_filepath);

    memext_connect(MEMEXT_TYPE_LUFTNER);
    memset(g_memext.FLASH, 0xFF, TEST_FLASH_LEN);
    memext_luftner_set_flash_filepath(TEST_FLASH_NAME);
    TEST_ASSERT_EQUAL_MEMORY(data, g_memext.FLASH, TEST_FLASH_LEN);

    if (saved_fp) memext_luftner_set_flash_filepath(saved_fp);
    g_free(saved_fp);
    if (saved_conn == MEMEXT_CONNECTION_YES) {
        memext_connect(saved_type);
    } else {
        g_memext.type = saved_type;
        memext_disconnect();
    }
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_unicard_relative_sd_root_goes_to_cfg_dir);
    RUN_TEST(test_unicard_absolute_sd_root_unchanged);
    RUN_TEST(test_memext_flash_relative_path_from_cfg_dir);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
