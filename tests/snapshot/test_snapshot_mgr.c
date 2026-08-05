/*
 * test_snapshot_mgr.c — unit testy pro snapshot manager
 *
 * Testuje: snapshot_register_component, snapshot_save, snapshot_load,
 *          snapshot_read_metadata, snapshot_result_to_string
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/mzarch/mztvsys.h"
#include "emulator/mzarch/mzhal.h"
#include "emulator/snapshot/snapshot_io.h"
#include "emulator/snapshot/snapshot_xml.h"
#include <stdio.h>
#include "emulator/emulator.h"
#include "hw-generic/memory/memory.h"

static const char *TMP_FILE = "tests/data/tmp/test_mgr.mzs";

void setUp(void) { }
void tearDown(void)
{
    remove(TMP_FILE);
}

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* snapshot_init registruje komponenty — ověřit, že registrace proběhla */
void test_mgr_components_registered(void)
{
    /* snapshot_init() se volá v mztest_init() —
     * ověříme, že se zaregistrovalo >0 komponent */
    TEST_ASSERT_GREATER_THAN(0, snapshot_mgr_get_component_count());
}

/* snapshot_result_to_string vrací nenulový string pro každý kód */
void test_mgr_result_to_string(void)
{
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_OK));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_IO));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_ZIP));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_NOT_PAUSED));
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* save + load roundtrip — základní test celého pipeline */
void test_mgr_save_load_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* zapsat vzor do RAM */
    for (int i = 0; i < 256; i++) {
        g_memory.RAM[0x1000 + i] = (uint8_t)i;
    }

    /* uložit zálohu */
    uint8_t backup[256];
    memcpy(backup, &g_memory.RAM[0x1000], 256);

    /* emulace musí být v pauze pro save/load */
    g_emulator.paused = true;

    /* save */
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "test roundtrip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    /* zničit data v RAM */
    memset(&g_memory.RAM[0x1000], 0xFF, 256);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_memory.RAM[0x1000]);

    /* load */
    res = snapshot_load(TMP_FILE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    /* ověřit obnovení */
    TEST_ASSERT_EQUAL_MEMORY(backup, &g_memory.RAM[0x1000], 256);

    g_emulator.paused = false;
}

/* save bez pauzy → ERR_NOT_PAUSED */
void test_mgr_save_requires_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = false;
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "should fail");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_NOT_PAUSED, res);
}

/* 0019 KUS 2 - test (c): snapshot z pokračujícího BP přes dedikovaný
 * safe-point flag funguje i bez paused (= guard akceptuje snapshot_safepoint).
 * Toto je 0018 regrese-check: BP-action snapshot z continuing BP. */
void test_mgr_save_via_snapshot_safepoint(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Emulace běží (= paused false), ale BP-action otevřel safe-point. */
    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = true;

    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "continuing BP snapshot");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    g_emulator.snapshot_safepoint = false;
}

/* 0019 KUS 2 - test (b): snapshot_safepoint je samostatný kanál a NEsmí
 * ovlivnit EMULATOR_TEST_PAUSED (= běhový wait-loop ho nevidí, actual_frames
 * tak zůstane správný i během snapshot-BP). */
void test_mgr_safepoint_does_not_set_paused(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = true;

    /* Wait-loop v dispatch.c testuje EMULATOR_TEST_PAUSED (= g_emulator.paused),
     * ne snapshot_safepoint. Safe-point tedy nesmí "prosáknout" do paused. */
    TEST_ASSERT_FALSE(EMULATOR_TEST_PAUSED);
    TEST_ASSERT_TRUE(EMULATOR_TEST_SNAPSHOT_SAFEPOINT);

    g_emulator.snapshot_safepoint = false;
}

/* load bez pauzy → ERR_NOT_PAUSED */
void test_mgr_load_requires_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* nejprve vytvořit validní snapshot */
    g_emulator.paused = true;
    snapshot_save(TMP_FILE, "for load test");

    /* zkusit load bez pauzy */
    g_emulator.paused = false;
    en_SNAPSHOT_RESULT res = snapshot_load(TMP_FILE);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_NOT_PAUSED, res);
}

/* load neexistujícího souboru → chyba */
void test_mgr_load_nonexistent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_load("tests/data/tmp/neexistuje.mzs");
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, res);
    g_emulator.paused = false;
}

/* metadata — přečtení metadat z uloženého snapshotu */
void test_mgr_read_metadata(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "metadata test popis");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);
    g_emulator.paused = false;

    st_SNAPSHOT_METADATA metadata;
    memset(&metadata, 0, sizeof(metadata));

    res = snapshot_read_metadata(TMP_FILE, &metadata);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);

    /* ověřit metadata */
    TEST_ASSERT_EQUAL_STRING("metadata test popis", metadata.description);
    TEST_ASSERT_EQUAL_INT(MZARCH, metadata.architecture);
    TEST_ASSERT_EQUAL_INT(MZTVSYS, metadata.tvsys);
    TEST_ASSERT_EQUAL_INT(1, metadata.format_version);

    /* checksum nesmí být prázdný */
    TEST_ASSERT_GREATER_THAN(0, strlen(metadata.checksum));
}

/* metadata z neexistujícího souboru → chyba */
void test_mgr_read_metadata_nonexistent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_SNAPSHOT_METADATA metadata;
    en_SNAPSHOT_RESULT res = snapshot_read_metadata("tests/data/tmp/neexistuje.mzs", &metadata);
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, res);
}

/* vícenásobný save→load */
void test_mgr_multiple_save_load(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_FULL);

    g_emulator.paused = true;

    for (int round = 0; round < 5; round++) {
        /* zapsat unikátní vzor */
        uint8_t pattern = (uint8_t)(round * 37 + 11);
        for (int i = 0; i < 64; i++) {
            g_memory.RAM[0x2000 + i] = pattern;
        }

        TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save(TMP_FILE, "multi"));

        /* zničit */
        memset(&g_memory.RAM[0x2000], 0, 64);

        /* obnovit */
        TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load(TMP_FILE));

        /* ověřit */
        TEST_ASSERT_EQUAL_HEX8(pattern, g_memory.RAM[0x2000]);
        TEST_ASSERT_EQUAL_HEX8(pattern, g_memory.RAM[0x2000 + 63]);
    }

    g_emulator.paused = false;
}

/* === Kompatibilita arch/tvsys (mzhal krok 14) ============================ */

/**
 * @brief Zapíše minimální .mzs obsahující jen manifest.xml.
 *
 * @param path  Cílový soubor.
 * @param arch  Hodnota elementu <architecture>.
 * @param tvsys Hodnota elementu <tvsys>; 0 = element vynechat (legacy
 *              snapshot z doby před zavedením tvsys pole).
 */
static void write_minimal_manifest(const char *path, int arch, int tvsys)
{
    snapshot_io_t *io = snapshot_io_open_write(path, 1);
    TEST_ASSERT_NOT_NULL(io);

    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);
    char ver_str[16];
    snprintf(ver_str, sizeof(ver_str), "%d", SNAPSHOT_FORMAT_VERSION);
    snapshot_xml_open_element_attr(w, "mzs_snapshot", "version", ver_str);
    snapshot_xml_write_int(w, "architecture", arch);
    if (tvsys != 0) {
        snapshot_xml_write_int(w, "tvsys", tvsys);
    }
    snapshot_xml_close_element(w);
    char *xml = snapshot_xml_writer_finish(w);

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK,
                          snapshot_io_write_xml(io, "manifest.xml", xml));
    g_free(xml);
    snapshot_io_close(io);
}

#define REJECT_FILE "tests/data/tmp/test_mgr_reject.mzs"

/* snapshot cizí architektury -> SNAPSHOT_ERR_ARCHITECTURE */
void test_mgr_load_reject_wrong_architecture(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const int foreign_arch = (g_mzhal.arch == 800) ? 1500 : 800;
    write_minimal_manifest(REJECT_FILE, foreign_arch, (int)g_mzhal.tvsys);

    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_ARCHITECTURE,
                          snapshot_load(REJECT_FILE));
    g_emulator.paused = false;
}

/* správná arch, cizí TV systém -> SNAPSHOT_ERR_TVSYS */
void test_mgr_load_reject_wrong_tvsys(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const int foreign_tvsys =
        ((int)g_mzhal.tvsys == MZTVSYS_PAL) ? MZTVSYS_NTSC : MZTVSYS_PAL;
    write_minimal_manifest(REJECT_FILE, (int)g_mzhal.arch, foreign_tvsys);

    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_TVSYS, snapshot_load(REJECT_FILE));
    g_emulator.paused = false;
}

/* legacy snapshot bez <tvsys> - posuzuje se historickým defaultem
 * architektury (700/800 = PAL, 1500 = NTSC; rozhodnutí 2026-07-30) */
void test_mgr_load_legacy_tvsys_policy(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const int legacy_default =
        (g_mzhal.arch == 1500) ? MZTVSYS_NTSC : MZTVSYS_PAL;
    write_minimal_manifest(REJECT_FILE, (int)g_mzhal.arch, 0);

    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_load(REJECT_FILE);
    g_emulator.paused = false;

    if (legacy_default == (int)g_mzhal.tvsys) {
        /* Legacy default sedí (mz800, mz700-pal, mz1500): tvsys check
         * musí projít; load pak selže až na chybějících komponentách,
         * nikdy ne na tvsys/arch. */
        TEST_ASSERT_NOT_EQUAL(SNAPSHOT_ERR_TVSYS, res);
        TEST_ASSERT_NOT_EQUAL(SNAPSHOT_ERR_ARCHITECTURE, res);
    } else {
        /* mz700-ntsc: legacy default PAL != NTSC -> reject. */
        TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_TVSYS, res);
    }
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_mgr_components_registered);
    RUN_TEST(test_mgr_result_to_string);

    /* unit */
    RUN_TEST(test_mgr_save_load_roundtrip);
    RUN_TEST(test_mgr_save_requires_pause);
    RUN_TEST(test_mgr_save_via_snapshot_safepoint);
    RUN_TEST(test_mgr_safepoint_does_not_set_paused);
    RUN_TEST(test_mgr_load_requires_pause);
    RUN_TEST(test_mgr_load_nonexistent);
    RUN_TEST(test_mgr_read_metadata);
    RUN_TEST(test_mgr_read_metadata_nonexistent);
    RUN_TEST(test_mgr_load_reject_wrong_architecture);
    RUN_TEST(test_mgr_load_reject_wrong_tvsys);
    RUN_TEST(test_mgr_load_legacy_tvsys_policy);

    /* full */
    RUN_TEST(test_mgr_multiple_save_load);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
