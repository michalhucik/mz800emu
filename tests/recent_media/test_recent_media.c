/**
 * @file test_recent_media.c
 * @brief Testy historie naposledy vložených médií (recent_media.h) a zápisu do ní z míst vkládání.
 *
 * Historie sama:
 *   - pořadí (naposledy vložená první), nezávislost druhů, strop RECENT_MEDIA_MAX,
 *   - opakované vložení přesune položku nahoru; na Windows i při jiné velikosti
 *     písmen a jiných oddělovačích, jinde jen při stejném textu,
 *   - odstranění podle indexu a podle cesty, hodnoty mimo rozsah,
 *   - čas vložení, kopie položky pro jiné vlákno, číslo stavu,
 *   - pozastavení zápisu, relativní cesta,
 *   - uložení do INI a nové načtení včetně cesty s diakritikou a cesty delší
 *     než 260 znaků; INI s dírami a opakovanou cestou.
 *
 * Místa vkládání (skutečné jádro):
 *   - disketa: fdc_mount_dskfile() a zásah fdc_request_mount(); neúspěšné
 *     vložení, nové otevření téhož obrazu a pozastavený zápis nic nezapíší,
 *   - páska: cmt_open_file(); tichý režim (snapshot) nic nezapíše,
 *   - Quick Disk: qdisk_mount_path() pro obraz i adresář.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "recent_media/recent_media.h"
#include "cfgmain.h"
#include "device_request.h"
#include "replay.h"
#include "libs/cfgfile/cfgroot.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/qdisk/qdisk.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Velikost těla testovacího MZF. */
#define TEST_MZF_BODY_SIZE 64

/** @brief Dočasný adresář testu (main ho vytvoří a smaže). */
static char *s_tmpdir = NULL;
/** @brief Uložené připojení řadiče FDC0 (obnova v tearDown). */
static unsigned s_saved_fdc_connected;
/** @brief Uložené připojení a typ Quick Disku (obnova v tearDown). */
static unsigned s_saved_qd_connected, s_saved_qd_type;

void setUp(void)
{
    recent_media_reset();
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_fdc_connected = g_fdc[FDC0].connected;
    s_saved_qd_connected = g_qdisk.connected;
    s_saved_qd_type = g_qdisk.type;
}

void tearDown(void)
{
    for (unsigned d = 0; d < FDC_NUM_DRIVES; d++)
        if (fdc_test_drive_id_mounted(&g_fdc[FDC0], d)) fdc_umount(&g_fdc[FDC0], d);
    g_fdc[FDC0].connected = s_saved_fdc_connected;
    if (device_state_lock_held()) device_state_unlock();
    while (cmt_quiet_active()) cmt_quiet_end();
    qdisk_close();
    g_qdisk.connected = s_saved_qd_connected;
    g_qdisk.type = s_saved_qd_type;
    device_request_clear();
    recent_media_reset();
}

/** @brief Cesta souboru v dočasném adresáři testu (uvolnit g_free()). */
static char *tmp_path(const char *name)
{
    return g_build_filename(s_tmpdir, name, NULL);
}

/** @brief Ověří cestu položky historie. */
static void assert_item(en_RECENT_MEDIA_KIND kind, unsigned index, const char *expected)
{
    const char *got = recent_media_get(kind, index);
    if (!expected) TEST_ASSERT_NULL(got);
    else TEST_ASSERT_EQUAL_STRING(expected, got);
}

/** @brief Zapíše malý DSK (2 stopy, 16 x 256 B) do souboru (jako test_fdc_request.c). */
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

/** @brief Zapíše soubor MZF s jedním krátkým programem. */
static void write_test_mzf(const char *path)
{
    uint8_t data[128 + TEST_MZF_BODY_SIZE];
    memset(data, 0, sizeof(data));
    data[0] = 0x01;
    const char name[] = "RECENT\r";
    memcpy(&data[1], name, sizeof(name) - 1);
    data[0x12] = (uint8_t)(TEST_MZF_BODY_SIZE & 0xFF);
    data[0x13] = (uint8_t)(TEST_MZF_BODY_SIZE >> 8);
    data[0x15] = 0x12; /* fstrt = 1200h */
    data[0x17] = 0x12; /* fexec = 1200h */
    for (int i = 0; i < TEST_MZF_BODY_SIZE; i++) data[128 + i] = (uint8_t)(i * 7 + 3);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)data, sizeof(data), NULL));
}

/* ================================================================
 * Historie
 * ================================================================ */

/* Prázdná historie a hodnoty mimo rozsah. */
void test_empty_and_out_of_range(void)
{
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) {
        TEST_ASSERT_EQUAL_UINT(0, recent_media_count((en_RECENT_MEDIA_KIND)k));
        assert_item((en_RECENT_MEDIA_KIND)k, 0, NULL);
        TEST_ASSERT_EQUAL_INT64(0, recent_media_get_time((en_RECENT_MEDIA_KIND)k, 0));
    }
    recent_media_add(RECENT_MEDIA_KIND_COUNT, "/x/a.dsk");
    recent_media_add((en_RECENT_MEDIA_KIND)-1, "/x/a.dsk");
    recent_media_add(RECENT_MEDIA_DSK, NULL);
    recent_media_add(RECENT_MEDIA_DSK, "");
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_KIND_COUNT));
    assert_item(RECENT_MEDIA_KIND_COUNT, 0, NULL);
    TEST_ASSERT_NULL(recent_media_dup(RECENT_MEDIA_KIND_COUNT, 0, NULL));
    recent_media_remove(RECENT_MEDIA_KIND_COUNT, 0);
    recent_media_remove(RECENT_MEDIA_DSK, 0);
    TEST_ASSERT_FALSE(recent_media_remove_path(RECENT_MEDIA_DSK, NULL));
    TEST_ASSERT_FALSE(recent_media_remove_path(RECENT_MEDIA_DSK, "/x/a.dsk"));
}

/* Naposledy vložená položka je první; druhy jsou nezávislé. */
void test_order_and_kinds(void)
{
    char *a = tmp_path("a.dsk"), *b = tmp_path("b.dsk"), *c = tmp_path("c.dsk"), *t = tmp_path("t.mzf"), *q = tmp_path("q.mzq");
    recent_media_add(RECENT_MEDIA_DSK, a);
    recent_media_add(RECENT_MEDIA_DSK, b);
    recent_media_add(RECENT_MEDIA_CMT, t);
    recent_media_add(RECENT_MEDIA_DSK, c);
    recent_media_add(RECENT_MEDIA_QD, q);

    TEST_ASSERT_EQUAL_UINT(3, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, c);
    assert_item(RECENT_MEDIA_DSK, 1, b);
    assert_item(RECENT_MEDIA_DSK, 2, a);
    assert_item(RECENT_MEDIA_DSK, 3, NULL);
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, t);
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_QD));
    assert_item(RECENT_MEDIA_QD, 0, q);
    /* stejná cesta v jiném druhu je jiná položka */
    recent_media_add(RECENT_MEDIA_CMT, a);
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_CMT));
    TEST_ASSERT_EQUAL_UINT(3, recent_media_count(RECENT_MEDIA_DSK));
    g_free(a); g_free(b); g_free(c); g_free(t); g_free(q);
}

/* Strop: nejvýš RECENT_MEDIA_MAX položek, nejstarší vypadávají. */
void test_limit(void)
{
    char *paths[RECENT_MEDIA_MAX + 3];
    for (int i = 0; i < RECENT_MEDIA_MAX + 3; i++) {
        char name[32];
        g_snprintf(name, sizeof(name), "disk%02d.dsk", i);
        paths[i] = tmp_path(name);
        recent_media_add(RECENT_MEDIA_DSK, paths[i]);
    }
    TEST_ASSERT_EQUAL_UINT(RECENT_MEDIA_MAX, recent_media_count(RECENT_MEDIA_DSK));
    for (unsigned i = 0; i < RECENT_MEDIA_MAX; i++) assert_item(RECENT_MEDIA_DSK, i, paths[RECENT_MEDIA_MAX + 2 - i]);
    assert_item(RECENT_MEDIA_DSK, RECENT_MEDIA_MAX, NULL);
    /* položka z plné historie nahoru: počet se nemění a nic nevypadne */
    recent_media_add(RECENT_MEDIA_DSK, paths[3]);
    TEST_ASSERT_EQUAL_UINT(RECENT_MEDIA_MAX, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, paths[3]);
    assert_item(RECENT_MEDIA_DSK, 1, paths[RECENT_MEDIA_MAX + 2]);
    assert_item(RECENT_MEDIA_DSK, RECENT_MEDIA_MAX - 1, paths[4]);
    for (int i = 0; i < RECENT_MEDIA_MAX + 3; i++) g_free(paths[i]);
}

/* Opakované vložení položku přesune nahoru; jiná velikost písmen jen na Windows. */
void test_duplicates(void)
{
    char *a = tmp_path("Alpha.dsk"), *b = tmp_path("beta.dsk");
    recent_media_add(RECENT_MEDIA_DSK, a);
    recent_media_add(RECENT_MEDIA_DSK, b);
    recent_media_add(RECENT_MEDIA_DSK, a);
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, a);
    assert_item(RECENT_MEDIA_DSK, 1, b);

    /* jiná velikost písmen (i u znaku s diakritikou) a jiné oddělovače */
    char *lower = g_build_filename(s_tmpdir, "\xC5\x98" "eka.DSK", NULL); /* Řeka.DSK */
    char *upper_dir = g_utf8_strup(s_tmpdir, -1);
    char *other = g_strconcat(upper_dir, "/", "\xC5\x99" "EKA.dsk", NULL); /* řEKA.dsk */
    recent_media_add(RECENT_MEDIA_DSK, lower);
    recent_media_add(RECENT_MEDIA_DSK, b);
    recent_media_add(RECENT_MEDIA_DSK, other);
#ifdef G_OS_WIN32
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3, recent_media_count(RECENT_MEDIA_DSK), "paths differing only in case are one item on Windows");
    /* položka zůstává v původním zápisu */
    assert_item(RECENT_MEDIA_DSK, 0, lower);
    assert_item(RECENT_MEDIA_DSK, 1, b);
    assert_item(RECENT_MEDIA_DSK, 2, a);
    TEST_ASSERT_TRUE(recent_media_remove_path(RECENT_MEDIA_DSK, other));
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
#else
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4, recent_media_count(RECENT_MEDIA_DSK), "paths differing in case are different items");
    assert_item(RECENT_MEDIA_DSK, 0, other);
    assert_item(RECENT_MEDIA_DSK, 1, b);
    assert_item(RECENT_MEDIA_DSK, 2, lower);
#endif
    g_free(a); g_free(b); g_free(lower); g_free(upper_dir); g_free(other);
}

/* Odstranění podle indexu a podle cesty. */
void test_remove(void)
{
    char *a = tmp_path("a.mzf"), *b = tmp_path("b.mzf"), *c = tmp_path("c.mzf"), *d = tmp_path("d.mzf");
    recent_media_add(RECENT_MEDIA_CMT, a);
    recent_media_add(RECENT_MEDIA_CMT, b);
    recent_media_add(RECENT_MEDIA_CMT, c);
    recent_media_add(RECENT_MEDIA_CMT, d);
    recent_media_remove(RECENT_MEDIA_CMT, 1); /* c */
    TEST_ASSERT_EQUAL_UINT(3, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, d);
    assert_item(RECENT_MEDIA_CMT, 1, b);
    assert_item(RECENT_MEDIA_CMT, 2, a);
    recent_media_remove(RECENT_MEDIA_CMT, 7);
    TEST_ASSERT_EQUAL_UINT(3, recent_media_count(RECENT_MEDIA_CMT));
    TEST_ASSERT_TRUE(recent_media_remove_path(RECENT_MEDIA_CMT, a));
    TEST_ASSERT_FALSE(recent_media_remove_path(RECENT_MEDIA_CMT, a));
    TEST_ASSERT_FALSE(recent_media_remove_path(RECENT_MEDIA_DSK, d));
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_CMT));
    recent_media_remove(RECENT_MEDIA_CMT, 0);
    recent_media_remove(RECENT_MEDIA_CMT, 0);
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, NULL);
    /* po vyprázdnění jde historie znovu plnit */
    recent_media_add(RECENT_MEDIA_CMT, c);
    assert_item(RECENT_MEDIA_CMT, 0, c);
    g_free(a); g_free(b); g_free(c); g_free(d);
}

/* Čas vložení, kopie položky a číslo stavu. */
void test_time_dup_serial(void)
{
    char *a = tmp_path("a.dsk"), *b = tmp_path("b.dsk");
    const gint64 before = g_get_real_time() / G_USEC_PER_SEC;
    const unsigned s0 = recent_media_serial();
    recent_media_add(RECENT_MEDIA_DSK, a);
    const unsigned s1 = recent_media_serial();
    TEST_ASSERT_TRUE(s1 != s0);
    recent_media_add(RECENT_MEDIA_DSK, b);
    const gint64 after = g_get_real_time() / G_USEC_PER_SEC;
    for (unsigned i = 0; i < 2; i++) {
        const gint64 t = recent_media_get_time(RECENT_MEDIA_DSK, i);
        TEST_ASSERT_TRUE(t >= before && t <= after);
    }
    TEST_ASSERT_EQUAL_INT64(0, recent_media_get_time(RECENT_MEDIA_DSK, 2));

    gint64 t = -1;
    char *copy = recent_media_dup(RECENT_MEDIA_DSK, 1, &t);
    TEST_ASSERT_EQUAL_STRING(a, copy);
    TEST_ASSERT_TRUE(copy != recent_media_get(RECENT_MEDIA_DSK, 1));
    TEST_ASSERT_EQUAL_INT64(recent_media_get_time(RECENT_MEDIA_DSK, 1), t);
    g_free(copy);
    t = -1;
    TEST_ASSERT_NULL(recent_media_dup(RECENT_MEDIA_DSK, 2, &t));
    TEST_ASSERT_EQUAL_INT64(0, t);

    /* čtení stav nemění, odstranění ano */
    const unsigned s2 = recent_media_serial();
    (void)recent_media_count(RECENT_MEDIA_DSK);
    TEST_ASSERT_EQUAL_UINT(s2, recent_media_serial());
    recent_media_remove(RECENT_MEDIA_DSK, 0);
    TEST_ASSERT_TRUE(recent_media_serial() != s2);
    g_free(a); g_free(b);
}

/* Pozastavený zápis historii nemění; pozastavení se počítají. Relativní cesta se uloží jako absolutní. */
void test_suspend_and_relative(void)
{
    char *a = tmp_path("a.dsk"), *b = tmp_path("b.dsk");
    recent_media_suspend();
    recent_media_suspend();
    recent_media_add(RECENT_MEDIA_DSK, a);
    recent_media_resume();
    recent_media_add(RECENT_MEDIA_DSK, a);
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
    recent_media_resume();
    recent_media_resume(); /* volání navíc se ignoruje */
    recent_media_add(RECENT_MEDIA_DSK, b);
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, b);

    recent_media_add(RECENT_MEDIA_QD, "relative-dir/disk.mzq");
    const char *stored = recent_media_get(RECENT_MEDIA_QD, 0);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_TRUE_MESSAGE(g_path_is_absolute(stored), stored);
    TEST_ASSERT_TRUE(g_str_has_suffix(stored, "disk.mzq"));
    g_free(a); g_free(b);
}

/* Historie přežije uložení INI a nové načtení (jako restart emulátoru), včetně dlouhé cesty a diakritiky. */
void test_ini_roundtrip(void)
{
    char *ini = tmp_path("recent.ini");
    st_CFGROOT *saved_root = g_cfgmain;

    /* cesta s diakritikou (včetně mezery) a cesta přes 260 znaků */
    char *diac = g_build_filename(s_tmpdir, "Příliš žluťoučký kůň", "úpěl ďábelské ódy.dsk", NULL);
    GString *long_name = g_string_new(NULL);
    while (long_name->len < 300) g_string_append(long_name, "dlouhy-adresar-0123456789/");
    g_string_append(long_name, "disk.dsk");
    char *lng = g_build_filename(s_tmpdir, long_name->str, NULL);
    g_string_free(long_name, TRUE);
    TEST_ASSERT_TRUE(strlen(lng) > 260);
    char *tape = tmp_path("tape one.mzf"), *qd = tmp_path("qd.mzq"), *plain = tmp_path("plain.dsk");

    g_cfgmain = cfgroot_new(ini);
    recent_media_config_init();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, recent_media_count(RECENT_MEDIA_DSK), "a missing INI gives an empty history");
    recent_media_add(RECENT_MEDIA_DSK, plain);
    recent_media_add(RECENT_MEDIA_DSK, lng);
    recent_media_add(RECENT_MEDIA_DSK, diac);
    recent_media_add(RECENT_MEDIA_CMT, tape);
    recent_media_add(RECENT_MEDIA_QD, qd);
    const gint64 t_diac = recent_media_get_time(RECENT_MEDIA_DSK, 0);
    const gint64 t_tape = recent_media_get_time(RECENT_MEDIA_CMT, 0);
    TEST_ASSERT_TRUE(t_diac > 0);
    cfgroot_save(g_cfgmain);
    cfgroot_destroy(g_cfgmain);

    /* soubor má sekci a klíče */
    gchar *text = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(ini, &text, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(text, "[RECENT_MEDIA]"));
    TEST_ASSERT_NOT_NULL(strstr(text, "dsk_1_time"));
    TEST_ASSERT_NOT_NULL(strstr(text, "cmt_1"));
    TEST_ASSERT_NOT_NULL(strstr(text, "qd_12"));
    g_free(text);

    /* "restart": prázdná paměť, nové načtení ze stejného INI */
    recent_media_reset();
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
    g_cfgmain = cfgroot_new(ini);
    recent_media_config_init();
    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;

    TEST_ASSERT_EQUAL_UINT(3, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, diac);
    assert_item(RECENT_MEDIA_DSK, 1, lng);
    assert_item(RECENT_MEDIA_DSK, 2, plain);
    TEST_ASSERT_EQUAL_INT64(t_diac, recent_media_get_time(RECENT_MEDIA_DSK, 0));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, tape);
    TEST_ASSERT_EQUAL_INT64(t_tape, recent_media_get_time(RECENT_MEDIA_CMT, 0));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_QD));
    assert_item(RECENT_MEDIA_QD, 0, qd);

    g_remove(ini);
    g_free(ini); g_free(diac); g_free(lng); g_free(tape); g_free(qd); g_free(plain);
}

/* Ručně upravené INI: díry, opakovaná cesta, čas bez cesty a nečitelný čas. */
void test_ini_hand_edited(void)
{
    char *ini = tmp_path("edited.ini");
    st_CFGROOT *saved_root = g_cfgmain;
    static const char text[] = "[RECENT_MEDIA]\n"
                               "dsk_2 = C:/disks/b.dsk\n"
                               "dsk_2_time = 1700000000\n"
                               "dsk_3_time = 1700000005\n"
                               "dsk_5 = C:/disks/c.dsk\n"
                               "dsk_5_time = not-a-number\n"
                               "dsk_7 = C:/disks/b.dsk\n"
                               "dsk_7_time = 1600000000\n"
                               "dsk_99 = C:/disks/ignored.dsk\n"
                               "cmt_12 = C:/tapes/last.mzf\n"
                               "cmt_12_time = -5\n";
    TEST_ASSERT_TRUE(g_file_set_contents(ini, text, -1, NULL));
    recent_media_add(RECENT_MEDIA_QD, "C:/old/in-memory.mzq");

    g_cfgmain = cfgroot_new(ini);
    recent_media_config_init();
    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;

    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, "C:/disks/b.dsk");
    TEST_ASSERT_EQUAL_INT64(1700000000, recent_media_get_time(RECENT_MEDIA_DSK, 0));
    assert_item(RECENT_MEDIA_DSK, 1, "C:/disks/c.dsk");
    TEST_ASSERT_EQUAL_INT64(0, recent_media_get_time(RECENT_MEDIA_DSK, 1));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, "C:/tapes/last.mzf");
    TEST_ASSERT_EQUAL_INT64(0, recent_media_get_time(RECENT_MEDIA_CMT, 0));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, recent_media_count(RECENT_MEDIA_QD), "loading replaces the history in memory");
    g_remove(ini);
    g_free(ini);
}

/* ================================================================
 * Místa vkládání
 * ================================================================ */

/* Clear recent media: vyprázdní všechny druhy, změní číslo stavu a pozastavení zápisu nechá být. */
void test_clear(void)
{
    recent_media_add(RECENT_MEDIA_DSK, "/x/a.dsk");
    recent_media_add(RECENT_MEDIA_CMT, "/x/a.mzf");
    recent_media_add(RECENT_MEDIA_QD, "/x/a.mzq");
    const unsigned serial = recent_media_serial();
    recent_media_suspend();
    recent_media_clear();
    TEST_ASSERT_TRUE(recent_media_serial() != serial);
    for (int k = 0; k < (int)RECENT_MEDIA_KIND_COUNT; k++) TEST_ASSERT_EQUAL_UINT(0, recent_media_count((en_RECENT_MEDIA_KIND)k));
    recent_media_add(RECENT_MEDIA_DSK, "/x/b.dsk");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, recent_media_count(RECENT_MEDIA_DSK), "clearing keeps the write suspension");
    recent_media_resume();
    recent_media_add(RECENT_MEDIA_DSK, "/x/b.dsk");
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));
    /* prázdná historie: další vyprázdnění nic nerozbije */
    recent_media_clear();
    recent_media_clear();
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
}

/* Disketa: úspěšné vložení zapíše, neúspěšné, opakované otevření a pozastavený zápis ne. */
void test_fdc_mount_records(void)
{
    char *d1 = tmp_path("one.dsk"), *d2 = tmp_path("two.dsk"), *missing = tmp_path("missing.dsk");
    write_test_dsk(d1);
    write_test_dsk(d2);
    g_fdc[FDC0].connected = FDC_CONNECTED;

    fdc_mount_dskfile(&g_fdc[FDC0], 0, d1);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, d1);

    /* soubor, který nejde otevřít: mechanika zůstane prázdná a historie beze změny */
    fdc_mount_dskfile(&g_fdc[FDC0], 1, missing);
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));

    /* zásah z UI (dialog souboru, overlay menu): zapíše až emu vlákno po úspěšném vložení */
    TEST_ASSERT_TRUE(fdc_request_mount(FDC0, 1, d2) != 0);
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_DSK));
    device_request_poll();
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, d2);
    assert_item(RECENT_MEDIA_DSK, 1, d1);

    /* nové otevření téhož obrazu (změna volby read-only): pořadí se nemění */
    const unsigned serial = recent_media_serial();
    fdc_request_set_readonly(FDC0, 0, 1);
    device_request_poll();
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(serial, recent_media_serial(), "a remount of the same image is not a new insert");
    assert_item(RECENT_MEDIA_DSK, 0, d2);
    fdc_request_set_readonly(FDC0, 0, 0);
    device_request_poll();

    /* výměna diskety v obsazené mechanice: dříve vložený obraz jde nahoru */
    fdc_mount_dskfile(&g_fdc[FDC0], 1, d1);
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_DSK));
    assert_item(RECENT_MEDIA_DSK, 0, d1);

    /* pozastavený zápis (návrat médií po přehrání záznamu vstupů) */
    fdc_umount(&g_fdc[FDC0], 1);
    recent_media_suspend();
    fdc_mount_dskfile(&g_fdc[FDC0], 1, d2);
    recent_media_resume();
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[FDC0], 1));
    assert_item(RECENT_MEDIA_DSK, 0, d1);

    for (unsigned d = 0; d < 2; d++) fdc_umount(&g_fdc[FDC0], d);
    g_remove(d1);
    g_remove(d2);
    g_free(d1); g_free(d2); g_free(missing);
}

/* Páska: úspěšné otevření zapíše; neúspěšné a tichý režim (načtení snapshotu) ne. */
void test_cmt_open_records(void)
{
    char *t1 = tmp_path("first.mzf"), *t2 = tmp_path("second.mzf"), *bad = tmp_path("missing.mzf"), *unknown = tmp_path("tape.xyz");
    write_test_mzf(t1);
    write_test_mzf(t2);

    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(t1, NULL));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, t1);

    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_open_file(bad, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_open_file(unknown, NULL));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_CMT));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(t2));
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_CMT));
    assert_item(RECENT_MEDIA_CMT, 0, t2);

    /* tichý režim: páska se vloží, historie se nemění */
    cmt_quiet_begin();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(t1, NULL));
    cmt_quiet_end();
    assert_item(RECENT_MEDIA_CMT, 0, t2);
    assert_item(RECENT_MEDIA_CMT, 1, t1);

    cmt_eject();
    device_state_unlock();
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_DSK));
    g_remove(t1);
    g_remove(t2);
    g_free(t1); g_free(t2); g_free(bad); g_free(unknown);
}

/* Quick Disk: obraz i adresář virtuálního Quick Disku; nesouhlasící typ mechaniky nic nezapíše. */
void test_qdisk_mount_records(void)
{
    char *img = tmp_path("disk.mzq");
    g_qdisk.connected = QDISK_CONNECTED;
    g_qdisk.type = QDISK_TYPE_IMAGE;
    qdisk_umount();

    /* neexistující obraz se při vložení vytvoří (jako v klasickém menu) */
    TEST_ASSERT_EQUAL_INT(0, qdisk_mount_path(img, 0));
    TEST_ASSERT_TRUE(g_file_test(img, G_FILE_TEST_IS_REGULAR));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_QD));
    assert_item(RECENT_MEDIA_QD, 0, img);

    /* adresář do mechaniky typu obraz: odmítnuto, historie beze změny */
    TEST_ASSERT_EQUAL_INT(-1, qdisk_mount_path(s_tmpdir, 1));
    TEST_ASSERT_EQUAL_UINT(1, recent_media_count(RECENT_MEDIA_QD));
    qdisk_umount();

    g_qdisk.type = QDISK_TYPE_VIRTUAL;
    qdisk_umount();
    TEST_ASSERT_EQUAL_INT(0, qdisk_mount_path(s_tmpdir, 1));
    TEST_ASSERT_EQUAL_UINT(2, recent_media_count(RECENT_MEDIA_QD));
    assert_item(RECENT_MEDIA_QD, 0, s_tmpdir);
    assert_item(RECENT_MEDIA_QD, 1, img);
    qdisk_umount();

    /* odpojený Quick Disk: nic se nevloží ani nezapíše */
    g_qdisk.connected = QDISK_DISCONNECTED;
    recent_media_reset();
    TEST_ASSERT_EQUAL_INT(0, qdisk_mount_path(s_tmpdir, 1));
    TEST_ASSERT_EQUAL_UINT(0, recent_media_count(RECENT_MEDIA_QD));

    g_remove(img);
    g_free(img);
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    GError *err = NULL;
    s_tmpdir = g_dir_make_tmp("mztest_recent_XXXXXX", &err);
    if (!s_tmpdir) {
        fprintf(stderr, "g_dir_make_tmp: %s\n", err ? err->message : "?");
        mztest_teardown();
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_empty_and_out_of_range);
    RUN_TEST(test_order_and_kinds);
    RUN_TEST(test_limit);
    RUN_TEST(test_duplicates);
    RUN_TEST(test_remove);
    RUN_TEST(test_time_dup_serial);
    RUN_TEST(test_suspend_and_relative);
    RUN_TEST(test_clear);
    RUN_TEST(test_ini_roundtrip);
    RUN_TEST(test_ini_hand_edited);
    RUN_TEST(test_fdc_mount_records);
    RUN_TEST(test_cmt_open_records);
    RUN_TEST(test_qdisk_mount_records);
    int result = UNITY_END();

    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
