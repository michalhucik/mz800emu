/**
 * @file test_cmt_testdata.c
 * @brief Generátor a kontrola testovacích pásek v tests/data/cmt/ (pro tests/mcp/test_cmt_tmz.py).
 *
 * Testovací data vznikají z malých syntetických programů MZF (žádné cizí
 * programy) přes knihovnu tmz (tmz_profile_build_block() s profilem
 * ROM-800-NOMINAL, tzx_save()). Test soubory sestaví v paměti a porovná je
 * s uloženými v tests/data/cmt/; s proměnnou prostředí
 * MZTEST_REGEN_CMT_DATA=1 je místo porovnání zapíše (přegenerování po změně
 * knihovny nebo programů).
 *
 * Soubory:
 *   - prog1.mzf, prog2.mzf: program na 1200h (fstrt = fexec), který zapíše
 *     11h / 22h na 1300h a zůstane ve smyčce; zbytek těla do 64 B je vzorek,
 *   - two_programs.tmz: bloky 0x46 ROM-800-NOMINAL s prog1 a prog2,
 *   - stop_between.tmz: prog1, Pause 0 (zastavení pásky), prog2.
 */

#include "mztest.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "libs/mzf/mzf.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

#ifndef MZTEST_CMT_DATA_DIR
#error "MZTEST_CMT_DATA_DIR musí nastavit tests/peripherals/CMakeLists.txt"
#endif

/** @brief Délka těla testovacích programů. */
#define PROG_SIZE 64

/**
 * @brief Sestaví MZF programu, který zapíše @p marker na 1300h.
 *
 * Kód na 1200h: LD A,marker / LD (1300h),A / JR $ (3E mm 32 00 13 18 FE),
 * zbytek těla vzorek (i * 29 + marker).
 *
 * @param name Jméno MZF (ASCII, nejvýš 16 znaků).
 * @param marker Bajt zapsaný na 1300h.
 * @param[out] hdr Hlavička (pořadí hostitele).
 * @param[out] body Tělo PROG_SIZE bajtů.
 */
static void make_program(const char *name, uint8_t marker, st_MZF_HEADER *hdr, uint8_t body[PROG_SIZE])
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = 0x01;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, strlen(name));
    hdr->fsize = PROG_SIZE;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
    const uint8_t code[] = {0x3E, marker, 0x32, 0x00, 0x13, 0x18, 0xFE};
    for (int i = 0; i < PROG_SIZE; i++)
        body[i] = (uint8_t)(i * 29 + marker);
    memcpy(body, code, sizeof(code));
}

/**
 * @brief Porovná obsah s uloženým souborem, nebo ho zapíše (MZTEST_REGEN_CMT_DATA=1).
 * @param name Jméno souboru v tests/data/cmt/. @param data Obsah. @param size Délka.
 */
static void check_or_write(const char *name, const uint8_t *data, size_t size)
{
    gchar *path = g_build_filename(MZTEST_CMT_DATA_DIR, name, NULL);
    const char *regen = g_getenv("MZTEST_REGEN_CMT_DATA");
    if (regen && 0 == strcmp(regen, "1")) {
        g_mkdir_with_parents(MZTEST_CMT_DATA_DIR, 0755);
        TEST_ASSERT_TRUE_MESSAGE(g_file_set_contents(path, (const gchar *)data, (gssize)size, NULL), path);
    } else {
        gchar *disk = NULL;
        gsize len = 0;
        TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(path, &disk, &len, NULL), path);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(size, len, path);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(data, disk, size, path);
        g_free(disk);
    }
    g_free(path);
}

/**
 * @brief MZF soubor (hlavička v pořadí souboru + tělo).
 * @param name Jméno souboru. @param hdr Hlavička. @param body Tělo.
 */
static void check_mzf(const char *name, const st_MZF_HEADER *hdr, const uint8_t body[PROG_SIZE])
{
    uint8_t raw[sizeof(st_MZF_HEADER) + PROG_SIZE];
    st_MZF_HEADER h = *hdr;
    mzf_header_swap_endianity(&h);
    memcpy(raw, &h, sizeof(h));
    memcpy(raw + sizeof(h), body, PROG_SIZE);
    check_or_write(name, raw, sizeof(raw));
}

/**
 * @brief Připojí blok 0x46 ROM-800-NOMINAL s programem.
 * @param f Soubor. @param hdr Hlavička. @param body Tělo.
 */
static void add_program(st_TZX_FILE *f, const st_MZF_HEADER *hdr, const uint8_t body[PROG_SIZE])
{
    en_TMZ_PROFILE_ERROR err = TMZ_PROFILE_OK;
    st_TZX_BLOCK *b = tmz_profile_build_block("ROM-800-NOMINAL", NULL, hdr, body, PROG_SIZE, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, tmz_profile_error_string(err));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/** @brief Připojí blok 0x20 Pause 0 (zastavení pásky). @param f Soubor. */
static void add_stop(st_TZX_FILE *f)
{
    st_TZX_BLOCK b;
    b.id = TZX_BLOCK_ID_PAUSE;
    b.length = 2;
    b.data = (uint8_t *)calloc(1, 2);
    TEST_ASSERT_NOT_NULL(b.data);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/**
 * @brief Uloží TMZ do paměti a porovná / zapíše; soubor uvolní.
 * @param name Jméno souboru. @param f Soubor.
 */
static void check_tmz(const char *name, st_TZX_FILE *f)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    check_or_write(name, (const uint8_t *)h.spec.memspec.ptr, h.spec.memspec.size);
    generic_driver_close(&h);
    tzx_free(f);
}

/** @brief Nový prázdný TMZ. @return Soubor. */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

void setUp(void) {}
void tearDown(void) {}

/* Testovací pásky v tests/data/cmt/ odpovídají generátoru. */
void test_cmt_testdata_up_to_date(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZF_HEADER h1, h2;
    uint8_t b1[PROG_SIZE], b2[PROG_SIZE];
    make_program("TMZ PROG ONE", 0x11, &h1, b1);
    make_program("TMZ PROG TWO", 0x22, &h2, b2);
    check_mzf("prog1.mzf", &h1, b1);
    check_mzf("prog2.mzf", &h2, b2);

    st_TZX_FILE *f = new_tmz();
    add_program(f, &h1, b1);
    add_program(f, &h2, b2);
    check_tmz("two_programs.tmz", f);

    f = new_tmz();
    add_program(f, &h1, b1);
    add_stop(f);
    add_program(f, &h2, b2);
    check_tmz("stop_between.tmz", f);
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_cmt_testdata_up_to_date);
    return UNITY_END();
}
