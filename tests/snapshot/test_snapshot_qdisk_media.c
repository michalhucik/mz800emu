/**
 * @file test_snapshot_qdisk_media.c
 * @brief Načtení snapshotu a Quick Disk vložený ze souboru (snap_qdisk.c, qdisk.h).
 *
 * Snapshot nese u Quick Disku pole `status`, `image_position`, `readonly`
 * a `filename`, vložené médium je ale vlastnost hostitele: načtení snapshotu
 * mechanice nechává to, co v ní je. Test běží nad skutečným jádrem (snapshot
 * přes buffer API, zápis bajtů přes porty SIO, dva dočasné soubory s různým
 * obsahem) a ověřuje:
 *  - po načtení snapshotu pořízeného s jiným obrazem mechanika dál ukazuje
 *    vložený soubor, změny se ukládají do něj a soubor ze snapshotu zůstane
 *    netknutý (uložení, vysunutí, ukončení); prázdná mechanika zůstane
 *    prázdná a stroj ji nevidí jako připravenou,
 *  - ochrana proti zápisu se po načtení řídí vloženým médiem, ne polem
 *    `readonly` ani bitem stavu ze snapshotu,
 *  - rozdíl proti snapshotu se ohlásí řádkem v souhrnné zprávě, stejný
 *    rozdíl se při dalším načtení neopakuje,
 *  - pojistka mechaniky samostatně (uměle vyvolaná neshoda jména a obsahu,
 *    a načtení snapshotu záznamu vstupů, které jméno ze snapshotu vrací):
 *    kopie z paměti se nezapíše do jiného souboru, než ze kterého byla
 *    načtena (uložení, vypnutí motoru, vynucené uložení, vysunutí),
 *  - obraz nasazený z paměti (přehrání vstupů) si bere pole ze snapshotu
 *    jako dřív a do souboru se nezapíše ani vynuceným uložením,
 *  - snapshot s jiným druhem mechaniky (virtuální Quick Disk) obraz zavře
 *    a jeho změny uloží do jeho souboru.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/snapshot/snapshot_media.h"
#include "emulator/emulator.h"
#include "device_request.h"
#include "replay.h"
#include "hw-generic/qdisk/qdisk.h"

/** @brief Bajt, který test zapisuje na Quick Disk. */
#define FILL 0x5A
/** @brief Počet bajtů zapsaných jedním voláním emu_write_bytes(). */
#define WRITE_BYTES 16
/** @brief Výplň obrazu A. */
#define FILL_A 0xA1
/** @brief Výplň obrazu B. */
#define FILL_B 0xB2

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Cesta obrazu A v s_tmpdir. */
static char *s_mzq_a = NULL;
/** @brief Cesta obrazu B v s_tmpdir (jiný obsah než A). */
static char *s_mzq_b = NULL;
/** @brief Uložené `connected` mechaniky (obnova v tearDown). */
static unsigned s_saved_connected;
/** @brief Uložený typ mechaniky (obnova v tearDown). */
static unsigned s_saved_type;


/**
 * @brief Zapíše obraz Quick Disku plný jednoho bajtu.
 * @param path Cílový soubor (přepíše se).
 * @param fill Bajt výplně.
 */
static void write_test_mzq(const char *path, uint8_t fill)
{
    uint8_t *data = (uint8_t *)g_malloc(QDISK_FORMAT_SIZE);
    memset(data, fill, QDISK_FORMAT_SIZE);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)data, QDISK_FORMAT_SIZE, NULL));
    g_free(data);
}


/** @brief Zápis do registru SIO: výběr registru přes WR0, pak hodnota. */
static void sio_wreg(en_QDSIO_ADDR ctrl, uint8_t reg, uint8_t value)
{
    qdisk_write_byte(ctrl, reg);
    qdisk_write_byte(ctrl, value);
}


/**
 * @brief Zapíše WRITE_BYTES bajtů FILL na Quick Disk přes porty SIO.
 *
 * Zapne motor (kanál B, WR5 bit 7; vypnutý motor předtím vrátí hlavu na
 * začátek) a vysílání (kanál A, WR5 bit 3) a pošle data na datový port
 * kanálu A. Na chráněné nebo prázdné mechanice se nezapíše nic.
 */
static void emu_write_bytes(void)
{
    sio_wreg(QDSIO_ADDR_CTRL_B, QDSIO_REGADDR_5, 0x80);
    sio_wreg(QDSIO_ADDR_CTRL_A, QDSIO_REGADDR_5, 0x08);
    for (int i = 0; i < WRITE_BYTES; i++) qdisk_write_byte(QDSIO_ADDR_DATA_A, FILL);
}


/** @brief Vypne motor (kanál B, WR5 bit 7 = 0): jádro při tom ukládá obraz v režimu cached. */
static void emu_motor_off(void)
{
    sio_wreg(QDSIO_ADDR_CTRL_B, QDSIO_REGADDR_5, 0x00);
}


/** @brief Stavový registr RR0 kanálu A (bit 3 = disk je přítomen, bit 5 = disk není chráněný). */
static uint8_t sio_rr0_a(void)
{
    return qdisk_read_byte(QDSIO_ADDR_CTRL_A);
}


/** @brief Počet bajtů dané hodnoty v souboru. */
static unsigned file_count(const char *path, uint8_t value)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    unsigned n = 0;
    for (gsize i = 0; i < len; i++)
        if ((uint8_t)data[i] == value) n++;
    g_free(data);
    return n;
}


/** @brief Uloží snapshot do bufferu (v pauze); volající uvolní g_free(). */
static uint8_t *snap_save(size_t *size)
{
    g_emulator.paused = true;
    uint8_t *buf = NULL;
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("qdisk media", &buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
    return buf;
}


/** @brief Načte snapshot z bufferu (v pauze) a buffer uvolní. */
static void snap_load(uint8_t *buf, size_t size)
{
    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_load_from_buffer(buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
    g_free(buf);
}


/**
 * @brief Vyprázdní mechaniku bez uložení a nastaví ji jako připojenou mechaniku s obrazem.
 *
 * Před vysunutím přepne režim obrazu na discard: test smí skončit s kopií
 * v paměti, kterou úklid nemá zapsat.
 */
static void reset_drive(void)
{
    g_qdisk.storage_mode = QDISK_STORAGE_DISCARD;
    qdisk_umount_keep_cfg();
    g_qdisk.connected = QDISK_CONNECTED;
    g_qdisk.type = QDISK_TYPE_IMAGE;
    qdisk_set_write_protected(0);
    memset(g_qdisk.channel[0].Wreg, 0, sizeof(g_qdisk.channel[0].Wreg));
    memset(g_qdisk.channel[1].Wreg, 0, sizeof(g_qdisk.channel[1].Wreg));
    g_qdisk.channel[0].REG_addr = QDSIO_REGADDR_0;
    g_qdisk.channel[1].REG_addr = QDSIO_REGADDR_0;
}


/** @brief Vloží obraz ze souboru a ověří, že je připravený. */
static void mount(const char *path)
{
    TEST_ASSERT_EQUAL_INT(0, qdisk_mount_path(path, 0));
    TEST_ASSERT_TRUE(g_qdisk.status & QDSTS_IMG_READY);
    TEST_ASSERT_EQUAL_STRING(path, g_qdisk.filename);
}


void setUp(void)
{
    write_test_mzq(s_mzq_a, FILL_A);
    write_test_mzq(s_mzq_b, FILL_B);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_connected = g_qdisk.connected;
    s_saved_type = g_qdisk.type;
    reset_drive();
}

void tearDown(void)
{
    reset_drive();
    g_qdisk.connected = s_saved_connected;
    g_qdisk.type = s_saved_type;
    device_request_clear();
    g_emulator.paused = false;
}


/**
 * @brief Připraví scénář: snapshot s B.mzq, pak vložený A.mzq, načtení snapshotu a zápis.
 * @param replay_load true = snapshot se načte jako snapshot záznamu vstupů
 *        (snapshot_media_set_replay_load()): jméno ze snapshotu se mechanice vrátí.
 * @post Mechanika drží obsah A.mzq se zapsanými bajty (neuložené změny).
 */
static void prepare_other_disk_scenario(bool replay_load)
{
    mount(s_mzq_b);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    mount(s_mzq_a);
    snapshot_media_set_replay_load(replay_load);
    snap_load(snap, size);
    snapshot_media_set_replay_load(false);

    /* obsah v mechanice je dál z A.mzq */
    TEST_ASSERT_EQUAL_INT(1, g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_HEX8(FILL_A, ((uint8_t *)g_qdisk.handler.spec.memspec.ptr)[g_qdisk.handler.spec.memspec.size - 1]);

    emu_write_bytes();
    TEST_ASSERT_TRUE(qdisk_drive_has_ram_changes());
}


/** @brief Ověří, že soubor B.mzq má obsah ze setUp (žádný bajt obrazu A ani zapsaná data). */
static void assert_b_untouched(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, file_count(s_mzq_b, FILL_A), "B.mzq now holds the content of A.mzq");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, file_count(s_mzq_b, FILL), "the written bytes reached B.mzq");
    TEST_ASSERT_EQUAL_UINT(QDISK_FORMAT_SIZE, file_count(s_mzq_b, FILL_B));
}


/**
 * @brief Snapshot pořízený s jiným Quick Diskem: mechanika si nechá vložený, uložení jde do jeho souboru.
 */
void test_other_disk_in_snapshot_keeps_inserted(void)
{
    unsigned serial0 = 0;
    snapshot_media_get_notice(&serial0);
    prepare_other_disk_scenario(false);

    TEST_ASSERT_EQUAL_STRING_MESSAGE(s_mzq_a, g_qdisk.filename, "file name taken from the snapshot");
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    /* jeden řádek v souhrnné zprávě, jména bez cest */
    unsigned serial = 0;
    TEST_ASSERT_EQUAL_STRING("Quick Disk: b.mzq (now: a.mzq)", snapshot_media_get_notice(&serial));
    TEST_ASSERT_EQUAL_UINT(serial0 + 1, serial);

    /* "Save now" uloží změny do A.mzq, B.mzq je netknutý */
    TEST_ASSERT_EQUAL_INT(1, qdisk_sync_drive());
    assert_b_untouched();
    TEST_ASSERT_FALSE(qdisk_drive_has_unsaved_changes());
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Vypnutí motoru po načtení snapshotu s jiným Quick Diskem uloží změny do vloženého souboru.
 */
void test_other_disk_in_snapshot_motor_off(void)
{
    prepare_other_disk_scenario(false);

    emu_motor_off();
    assert_b_untouched();
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Vysunutí po načtení snapshotu s jiným Quick Diskem uloží změny do vloženého souboru.
 */
void test_other_disk_in_snapshot_eject(void)
{
    prepare_other_disk_scenario(false);

    qdisk_umount();
    assert_b_untouched();
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Ukončení emulátoru po načtení snapshotu s jiným Quick Diskem cizí soubor nepřepíše.
 */
void test_other_disk_in_snapshot_exit(void)
{
    prepare_other_disk_scenario(false);

    qdisk_close(); /* tělo qdisk_exit() bez uvolnění cest pro menu */
    assert_b_untouched();
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Prázdná mechanika zůstane po načtení snapshotu s Quick Diskem prázdná a stroj ji nevidí jako připravenou.
 */
void test_empty_drive_stays_empty(void)
{
    mount(s_mzq_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    qdisk_umount();
    TEST_ASSERT_FALSE(g_qdisk.status & QDSTS_IMG_READY);

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT(0, g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", g_qdisk.filename, "file name taken from the snapshot");
    TEST_ASSERT_FALSE_MESSAGE(g_qdisk.status & QDSTS_IMG_READY, "the drive reports a disk although it is empty");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sio_rr0_a() & 0x08, "DCD reports a disk although the drive is empty");
    TEST_ASSERT_EQUAL_STRING("Quick Disk: a.mzq (now: empty)", snapshot_media_get_notice(NULL));
}


/**
 * @brief Mechanika s Quick Diskem si ho nechá i proti snapshotu s prázdnou mechanikou.
 */
void test_full_drive_stays_full(void)
{
    size_t size = 0;
    uint8_t *snap = snap_save(&size); /* prázdná mechanika */
    mount(s_mzq_b);

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_STRING(s_mzq_b, g_qdisk.filename);
    TEST_ASSERT_TRUE_MESSAGE(g_qdisk.status & QDSTS_IMG_READY, "the drive reports no disk although one is inserted");
    TEST_ASSERT_EQUAL_HEX8(0x08, sio_rr0_a() & 0x08);
    TEST_ASSERT_EQUAL_STRING("Quick Disk: empty (now: b.mzq)", snapshot_media_get_notice(NULL));
}


/**
 * @brief Snapshot z doby, kdy byl disk chráněný, nezablokuje uložení změn disku, který chráněný není.
 */
void test_readonly_from_snapshot_keeps_unsaved_changes(void)
{
    mount(s_mzq_a);
    qdisk_set_write_protected(1);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    qdisk_set_write_protected(0);

    emu_write_bytes();
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_qdisk.readonly, "readonly taken from the snapshot");
    TEST_ASSERT_FALSE_MESSAGE(g_qdisk.status & QDSTS_IMG_READONLY, "write protection taken from the snapshot");
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    qdisk_umount();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(WRITE_BYTES, file_count(s_mzq_a, FILL), "unsaved changes lost on eject");
}


/**
 * @brief Snapshot z doby, kdy disk chráněný nebyl, nesundá ochranu, kterou má vložený disk.
 */
void test_readonly_of_inserted_medium_survives(void)
{
    mount(s_mzq_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size); /* readonly = 0 */
    qdisk_set_write_protected(1);

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT(1, g_qdisk.readonly);
    TEST_ASSERT_TRUE_MESSAGE(g_qdisk.status & QDSTS_IMG_READONLY, "write protection removed by the snapshot");
    TEST_ASSERT_EQUAL_HEX8(0x00, sio_rr0_a() & 0x20);
    emu_write_bytes();
    TEST_ASSERT_FALSE(qdisk_drive_has_ram_changes());
}


/**
 * @brief Snapshot se jménem souboru, který v mechanice je, ukládání nebrání; poloha hlavy se obnoví.
 */
void test_same_name_still_saves(void)
{
    mount(s_mzq_a);
    emu_write_bytes(); /* hlava za zapsanými bajty */
    TEST_ASSERT_EQUAL_INT(1, qdisk_sync_drive());
    const unsigned pos = g_qdisk.image_position;
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, pos);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    emu_motor_off(); /* hlava zpět na začátek */
    TEST_ASSERT_EQUAL_UINT(0, g_qdisk.image_position);
    snap_load(snap, size);
    TEST_ASSERT_EQUAL_STRING(s_mzq_a, g_qdisk.filename);
    TEST_ASSERT_EQUAL_UINT(pos, g_qdisk.image_position);

    for (int i = 0; i < WRITE_BYTES; i++) qdisk_write_byte(QDSIO_ADDR_DATA_A, FILL);
    TEST_ASSERT_EQUAL_INT(1, qdisk_sync_drive());
    TEST_ASSERT_EQUAL_UINT(2 * WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Stejný rozdíl se při opakovaném načtení neohlašuje znovu; načtení bez rozdílu paměť smaže.
 */
void test_notice_not_repeated(void)
{
    mount(s_mzq_b);
    size_t size_b = 0;
    uint8_t *snap_b = snap_save(&size_b);
    uint8_t *snap_b2 = (uint8_t *)g_memdup2(snap_b, size_b);
    uint8_t *snap_b3 = (uint8_t *)g_memdup2(snap_b, size_b);
    mount(s_mzq_a);
    size_t size_a = 0;
    uint8_t *snap_a = snap_save(&size_a);

    unsigned serial0 = 0, serial = 0;
    snapshot_media_get_notice(&serial0);

    snap_load(snap_b, size_b);
    TEST_ASSERT_NOT_NULL(snapshot_media_get_notice(&serial));
    TEST_ASSERT_EQUAL_UINT(serial0 + 1, serial);

    /* Quick Load ve smyčce: stejný rozdíl, žádná další zpráva */
    snap_load(snap_b2, size_b);
    TEST_ASSERT_NOT_NULL(snapshot_media_get_notice(&serial));
    TEST_ASSERT_EQUAL_UINT(serial0 + 1, serial);

    /* načtení bez rozdílu, pak znovu s rozdílem: zpráva přijde znovu */
    snap_load(snap_a, size_a);
    TEST_ASSERT_NULL(snapshot_media_get_notice(&serial));
    TEST_ASSERT_EQUAL_UINT(serial0 + 1, serial);
    snap_load(snap_b3, size_b);
    TEST_ASSERT_NOT_NULL(snapshot_media_get_notice(&serial));
    TEST_ASSERT_EQUAL_UINT(serial0 + 2, serial);
}


/**
 * @brief Pojistka mechaniky: uměle vyvolaná neshoda jména a obsahu se do cizího souboru nezapíše.
 */
void test_guard_foreign_name(void)
{
    mount(s_mzq_a);
    emu_write_bytes();
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());
    /* neshoda: jméno ukazuje na B.mzq, obsah a identita jsou z A.mzq */
    g_strlcpy(g_qdisk.filename, s_mzq_b, sizeof(g_qdisk.filename));

    /* "Save now": chyba, nic se nezapíše, změny zůstávají v paměti */
    TEST_ASSERT_EQUAL_INT(0, qdisk_sync_drive());
    assert_b_untouched();
    TEST_ASSERT_EQUAL_UINT(0, file_count(s_mzq_a, FILL));
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    /* vypnutí motoru ukládá obraz v režimu cached */
    emu_motor_off();
    assert_b_untouched();
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    /* vynucené uložení ("Save and switch") */
    TEST_ASSERT_EQUAL_INT(0, qdisk_drive_force_save_to_file());
    assert_b_untouched();
    TEST_ASSERT_TRUE(qdisk_drive_has_ram_changes());

    /* se správným jménem se změny uloží */
    g_strlcpy(g_qdisk.filename, s_mzq_a, sizeof(g_qdisk.filename));
    TEST_ASSERT_EQUAL_INT(1, qdisk_sync_drive());
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
}


/**
 * @brief Snapshot záznamu vstupů vrací mechanice pole ze snapshotu jako dřív; vysunutí pak cizí soubor nepřepíše.
 */
void test_replay_load_keeps_old_behaviour(void)
{
    unsigned serial0 = 0, serial = 0;
    snapshot_media_get_notice(&serial0);
    prepare_other_disk_scenario(true);

    /* dosavadní chování: jméno ze snapshotu, žádná zpráva o rozdílu */
    TEST_ASSERT_EQUAL_STRING(s_mzq_b, g_qdisk.filename);
    snapshot_media_get_notice(&serial);
    TEST_ASSERT_EQUAL_UINT(serial0, serial);

    qdisk_umount();
    assert_b_untouched();
    TEST_ASSERT_EQUAL_UINT(0, file_count(s_mzq_a, FILL));
}


/**
 * @brief Prázdná mechanika si při načtení snapshotu záznamu vstupů bere stav i jméno ze snapshotu jako dřív.
 */
void test_replay_load_empty_drive_old_behaviour(void)
{
    mount(s_mzq_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    const unsigned status = g_qdisk.status;
    qdisk_umount();

    snapshot_media_set_replay_load(true);
    snap_load(snap, size);
    snapshot_media_set_replay_load(false);
    TEST_ASSERT_EQUAL_HEX(status, g_qdisk.status);
    TEST_ASSERT_EQUAL_STRING(s_mzq_a, g_qdisk.filename);
    TEST_ASSERT_EQUAL_INT(0, g_qdisk.handler_valid);
}


/**
 * @brief Obraz z paměti (přehrání vstupů): pole ze snapshotu platí dál a do souboru se nezapíše nic.
 */
void test_buffer_image_unchanged(void)
{
    mount(s_mzq_a);
    size_t img_size = 0;
    uint8_t *img = qdisk_copy_image(&img_size);
    TEST_ASSERT_NOT_NULL(img);
    qdisk_umount();

    /* snapshot s readonly = 1 a jménem skutečného souboru */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, qdisk_mount_image_buffer(img, (uint32_t)img_size, 1, s_mzq_a));
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, qdisk_mount_image_buffer(img, (uint32_t)img_size, 0, "replay"));
    emu_write_bytes();
    TEST_ASSERT_TRUE(qdisk_drive_has_ram_changes());

    unsigned serial0 = 0, serial = 0;
    snapshot_media_get_notice(&serial0);
    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT(1, g_qdisk.readonly);
    TEST_ASSERT_TRUE(g_qdisk.status & QDSTS_IMG_READONLY);
    TEST_ASSERT_EQUAL_STRING(s_mzq_a, g_qdisk.filename);
    snapshot_media_get_notice(&serial);
    TEST_ASSERT_EQUAL_UINT(serial0, serial);

    /* obraz z paměti soubor nedrží: vynucené uložení se odmítne i bez ochrany */
    g_qdisk.status &= ~QDSTS_IMG_READONLY;
    TEST_ASSERT_EQUAL_INT(0, qdisk_drive_force_save_to_file());
    qdisk_umount_keep_cfg();
    TEST_ASSERT_EQUAL_UINT(0, file_count(s_mzq_a, FILL));
    g_free(img);
}


/**
 * @brief Snapshot s virtuálním Quick Diskem: obraz se zavře, jeho změny se uloží do jeho souboru.
 */
void test_drive_kind_change_closes_image(void)
{
    g_qdisk.type = QDISK_TYPE_VIRTUAL;
    size_t size = 0;
    uint8_t *snap = snap_save(&size); /* virtuální mechanika */
    g_qdisk.type = QDISK_TYPE_IMAGE;

    mount(s_mzq_a);
    emu_write_bytes();
    TEST_ASSERT_TRUE(qdisk_drive_has_unsaved_changes());

    snap_load(snap, size);
    TEST_ASSERT_TRUE(QDISK_TEST_TYPE_VIRTUAL);
    TEST_ASSERT_EQUAL_INT(0, g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_STRING("", g_qdisk.filename);
    TEST_ASSERT_EQUAL_UINT(WRITE_BYTES, file_count(s_mzq_a, FILL));
    assert_b_untouched();
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("qd_snapmedia_XXXXXX", NULL);
    s_mzq_a = g_build_filename(s_tmpdir, "a.mzq", NULL);
    s_mzq_b = g_build_filename(s_tmpdir, "b.mzq", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_other_disk_in_snapshot_keeps_inserted);
    RUN_TEST(test_other_disk_in_snapshot_motor_off);
    RUN_TEST(test_other_disk_in_snapshot_eject);
    RUN_TEST(test_other_disk_in_snapshot_exit);
    RUN_TEST(test_empty_drive_stays_empty);
    RUN_TEST(test_full_drive_stays_full);
    RUN_TEST(test_readonly_from_snapshot_keeps_unsaved_changes);
    RUN_TEST(test_readonly_of_inserted_medium_survives);
    RUN_TEST(test_same_name_still_saves);
    RUN_TEST(test_notice_not_repeated);
    RUN_TEST(test_guard_foreign_name);
    RUN_TEST(test_replay_load_keeps_old_behaviour);
    RUN_TEST(test_replay_load_empty_drive_old_behaviour);
    RUN_TEST(test_buffer_image_unchanged);
    RUN_TEST(test_drive_kind_change_closes_image);
    int result = UNITY_END();

    g_remove(s_mzq_a);
    g_remove(s_mzq_b);
    g_free(s_mzq_a);
    g_free(s_mzq_b);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
