/**
 * @file test_snapshot_fdc_media.c
 * @brief Načtení snapshotu a diskety vložené ze souboru (snap_fdc.c, fdc.h).
 *
 * Snapshot nese u každé mechaniky pole `mounted`, `readonly` a `filename`,
 * vložené médium je ale vlastnost hostitele: načtení snapshotu mechanikám
 * nechává to, co v nich je. Test běží nad skutečným jádrem (snapshot přes
 * buffer API, zápis sektoru přes porty řadiče, dva dočasné soubory s různým
 * obsahem) a ověřuje:
 *  - po načtení snapshotu pořízeného s jiným obrazem mechanika dál ukazuje
 *    vložený soubor, změny se ukládají do něj a soubor ze snapshotu zůstane
 *    netknutý; prázdná mechanika zůstane prázdná,
 *  - rozdíl proti snapshotu se ohlásí jednou souhrnnou zprávou, stejný
 *    rozdíl se při dalším načtení neopakuje,
 *  - ochrana proti zápisu se po načtení řídí vloženým médiem (volba
 *    uživatele, soubor chráněný na disku, sdílení), ne polem `readonly` ze
 *    snapshotu,
 *  - pojistka řadiče samostatně (uměle vyvolaná neshoda jména a obsahu, a
 *    načtení snapshotu záznamu vstupů, které jméno ze snapshotu vrací):
 *    kopie z paměti se nezapíše do jiného souboru, než ze kterého byla
 *    načtena (uložení, reset, vysunutí, ukončení),
 *  - obraz nasazený z paměti (přehrání vstupů) si bere pole ze snapshotu
 *    jako dřív a do souboru se nezapíše ani vynuceným uložením.
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
#include "hw-generic/fdc/fdc.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Stav řadiče po příkazu zápisu na chráněnou disketu (WD279x, typ II: write protect). */
#define ST_WRITE_PROTECT 0x40
/** @brief Stav řadiče při čekání na data zápisu (BUSY + DRQ). */
#define ST_BUSY_DRQ 0x03
/** @brief Bajt, kterým test plní zapisovaný sektor. */
#define FILL 0x5A
/** @brief Velikost sektoru testovacího DSK. */
#define SECTOR_BYTES 256
/** @brief Výplň dat obrazu A. */
#define FILL_A 0xA1
/** @brief Výplň dat obrazu B. */
#define FILL_B 0xB2

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Cesta obrazu A v s_tmpdir. */
static char *s_dsk_a = NULL;
/** @brief Cesta obrazu B v s_tmpdir (jiný obsah než A). */
static char *s_dsk_b = NULL;
/** @brief Uložené `connected` obou řadičů (obnova v tearDown). */
static unsigned s_saved_connected[FDC_INSTANCE_COUNT];


/**
 * @brief Zapíše malý DSK (2 stopy, 16 x 256 B) s danou výplní dat do souboru.
 * @param path Cílový soubor (přepíše se).
 * @param fill Bajt výplně sektorů.
 */
static void write_test_dsk(const char *path, uint8_t fill)
{
    st_HANDLER h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1024));
    size_t desc_size = dsk_tools_compute_description_size(1);
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *)g_malloc0(desc_size);
    desc->tracks = 2;
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description(desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, fill);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, dsk_tools_create_image(&h, desc));
    g_free(desc);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
}


/** @brief Zápis na port řadiče s inverzí sběrnice Sharp (offsety 0 až 3 invertované). */
static void port_out(st_FDC *fdc, int offset, uint8_t chip_value)
{
    uint8_t bus = (offset <= 3) ? (uint8_t)~chip_value : chip_value;
    fdc_write_byte(fdc, offset, &bus);
}


/** @brief Čtení stavového registru řadiče (hodnota čipu, po odstranění inverze). */
static uint8_t port_status(st_FDC *fdc)
{
    uint8_t bus = 0;
    fdc_read_byte(fdc, 0, &bus);
    return (uint8_t)~bus;
}


/**
 * @brief Pošle řadiči WRITE SECTOR pro stopu 0, sektor 1 dané mechaniky.
 *
 * Když řadič data přijímá (BUSY + DRQ), dodá celý sektor bajtů FILL.
 *
 * @return Stav řadiče hned po příkazu (před daty).
 */
static uint8_t emu_write_sector(st_FDC *fdc, unsigned drive)
{
    port_out(fdc, 4, (uint8_t)(0x84 | drive)); /* motor, bit 2 = přijmout číslo mechaniky, bity 0-1 mechanika */
    port_out(fdc, 5, 0);                       /* strana 0 */
    port_out(fdc, 1, 0);                       /* stopa 0 */
    port_out(fdc, 2, 1);                       /* sektor 1 */
    port_out(fdc, 0, 0xA0);                    /* WRITE SECTOR */
    uint8_t st = port_status(fdc);
    if ((st & ST_BUSY_DRQ) == ST_BUSY_DRQ)
        for (int i = 0; i < SECTOR_BYTES; i++) port_out(fdc, 3, FILL);
    return st;
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
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("fdc media", &buf, size);
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
 * @brief Vyprázdní mechaniky bez uložení a vrátí jejich volby do výchozího stavu.
 *
 * Před vysunutím přepne režim obrazu na discard: test smí skončit s kopií
 * v paměti, kterou pojistka do souboru nepustila, a úklid ji nemá zapsat.
 */
static void reset_drives(void)
{
    for (unsigned f = 0; f < FDC_INSTANCE_COUNT; f++)
        for (unsigned d = 0; d < FDC_NUM_DRIVES; d++) {
            g_fdc[f].drive[d].storage_mode = FDC_STORAGE_DISCARD;
            fdc_umount(&g_fdc[f], d);
            fdc_cfg_set_readonly(&g_fdc[f], d, 0);
            fdc_cfg_set_storage_mode(&g_fdc[f], d, "cached");
        }
}


void setUp(void)
{
    write_test_dsk(s_dsk_a, FILL_A);
    write_test_dsk(s_dsk_b, FILL_B);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    for (unsigned i = 0; i < FDC_INSTANCE_COUNT; i++) s_saved_connected[i] = g_fdc[i].connected;
    g_fdc[FDC0].connected = FDC_CONNECTED;
    g_fdc[FDC1].connected = FDC_CONNECTED;
    reset_drives();
}

void tearDown(void)
{
    reset_drives();
    for (unsigned i = 0; i < FDC_INSTANCE_COUNT; i++) g_fdc[i].connected = s_saved_connected[i];
    device_request_clear();
    g_emulator.paused = false;
}


/**
 * @brief Připraví scénář: snapshot s B.dsk ve FD0, pak FD0 = A.dsk, FD1 = B.dsk, načtení snapshotu a zápis na FD0.
 *
 * @param replay_load true = snapshot se načte jako snapshot záznamu vstupů
 *        (snapshot_media_set_replay_load()): jméno ze snapshotu se mechanice vrátí.
 * @post FD0 drží obsah A.dsk se zapsaným sektorem (neuložené změny).
 */
static void prepare_other_disk_scenario(bool replay_load)
{
    st_FDC *fdc = &g_fdc[FDC0];

    fdc_mount_dskfile(fdc, 0, s_dsk_b);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 0));
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    fdc_mount_dskfile(fdc, 1, s_dsk_b);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_STRING(s_dsk_a, fdc->drive[0].filename);

    snapshot_media_set_replay_load(replay_load);
    snap_load(snap, size);
    snapshot_media_set_replay_load(false);

    /* obsah v mechanice je v obou případech dál z A.dsk */
    TEST_ASSERT_EQUAL_HEX8(FILL_A, ((uint8_t *)fdc->drive[0].handler.spec.memspec.ptr)[fdc->drive[0].handler.spec.memspec.size - 1]);

    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));
}


/**
 * @brief Ověří, že soubory A.dsk a B.dsk mají obsah z setUp (žádný bajt druhého obrazu ani zapsaný sektor).
 * @param b_fill_a Počet bajtů FILL_A v čerstvém B.dsk (hlavička je může obsahovat).
 * @param b_fill   Počet bajtů FILL v čerstvém B.dsk.
 * @param a_fill   Počet bajtů FILL v čerstvém A.dsk.
 */
static void assert_files_untouched(unsigned b_fill_a, unsigned b_fill, unsigned a_fill)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(b_fill_a, file_count(s_dsk_b, FILL_A), "B.dsk now holds the content of A.dsk");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(b_fill, file_count(s_dsk_b, FILL), "the written sector reached B.dsk");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(a_fill, file_count(s_dsk_a, FILL), "the written sector reached A.dsk");
}


/**
 * @brief Snapshot pořízený s jinou disketou: mechanika si nechá vloženou, změny jdou do jejího souboru.
 */
void test_other_disk_in_snapshot_keeps_inserted(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned b_fill_a = file_count(s_dsk_b, FILL_A);
    const unsigned b_fill = file_count(s_dsk_b, FILL);
    const unsigned a_fill = file_count(s_dsk_a, FILL);
    unsigned serial0 = 0;
    snapshot_media_get_notice(&serial0);
    prepare_other_disk_scenario(false);

    /* FD0 dál ukazuje A.dsk, FD1 dál B.dsk (ve snapshotu byla prázdná) */
    TEST_ASSERT_EQUAL_STRING(s_dsk_a, fdc->drive[0].filename);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_EQUAL_STRING(s_dsk_b, fdc->drive[1].filename);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 1));

    /* jedna souhrnná zpráva, jména bez cest */
    unsigned serial = 0;
    const char *notice = snapshot_media_get_notice(&serial);
    TEST_ASSERT_EQUAL_UINT(serial0 + 1, serial);
    TEST_ASSERT_EQUAL_STRING("FDD 0: b.dsk (now: a.dsk)\nFDD 1: empty (now: b.dsk)", notice);

    /* "Sync now" uloží změny do A.dsk, B.dsk je netknutý */
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 0));
    TEST_ASSERT_FALSE(fdc_drive_has_unsaved_changes(fdc, 0));
    TEST_ASSERT_EQUAL_UINT(a_fill + SECTOR_BYTES, file_count(s_dsk_a, FILL));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(b_fill_a, file_count(s_dsk_b, FILL_A), "B.dsk now holds the content of A.dsk");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(b_fill, file_count(s_dsk_b, FILL), "the written sector reached B.dsk");
}


/**
 * @brief Vysunutí po načtení snapshotu s jinou disketou uloží změny do vloženého souboru.
 */
void test_other_disk_in_snapshot_eject(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned b_fill_a = file_count(s_dsk_b, FILL_A);
    const unsigned b_fill = file_count(s_dsk_b, FILL);
    const unsigned a_fill = file_count(s_dsk_a, FILL);
    prepare_other_disk_scenario(false);

    fdc_umount(fdc, 0);
    TEST_ASSERT_EQUAL_UINT(a_fill + SECTOR_BYTES, file_count(s_dsk_a, FILL));
    TEST_ASSERT_EQUAL_UINT(b_fill_a, file_count(s_dsk_b, FILL_A));
    TEST_ASSERT_EQUAL_UINT(b_fill, file_count(s_dsk_b, FILL));
}


/**
 * @brief Prázdná mechanika zůstane po načtení snapshotu s disketou prázdná; plná si disketu nechá i proti prázdnému snapshotu.
 */
void test_empty_and_full_drive_kept(void)
{
    st_FDC *fdc = &g_fdc[FDC0];

    /* snapshot s A.dsk ve FD0, načtení do prázdné mechaniky */
    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    fdc_umount(fdc, 0);
    snap_load(snap, size);
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_EQUAL_STRING("", fdc->drive[0].filename);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].handler_valid);
    TEST_ASSERT_EQUAL_STRING("FDD 0: a.dsk (now: empty)", snapshot_media_get_notice(NULL));

    /* snapshot s prázdnou FD0, načtení do mechaniky s B.dsk */
    snap = snap_save(&size);
    fdc_mount_dskfile(fdc, 0, s_dsk_b);
    snap_load(snap, size);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_EQUAL_STRING(s_dsk_b, fdc->drive[0].filename);
    TEST_ASSERT_EQUAL_STRING("FDD 0: empty (now: b.dsk)", snapshot_media_get_notice(NULL));
}


/**
 * @brief Stejný rozdíl se při opakovaném načtení neohlašuje znovu; načtení bez rozdílu paměť smaže.
 */
void test_notice_not_repeated(void)
{
    st_FDC *fdc = &g_fdc[FDC0];

    fdc_mount_dskfile(fdc, 0, s_dsk_b);
    size_t size_b = 0;
    uint8_t *snap_b = snap_save(&size_b);
    uint8_t *snap_b2 = (uint8_t *)g_memdup2(snap_b, size_b);
    uint8_t *snap_b3 = (uint8_t *)g_memdup2(snap_b, size_b);
    fdc_mount_dskfile(fdc, 0, s_dsk_a);
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
 * @brief Pojistka řadiče: uměle vyvolaná neshoda jména a obsahu se do cizího souboru nezapíše (uložení, reset, vynucené uložení).
 */
void test_guard_foreign_name_sync_and_reset(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned b_fill_a = file_count(s_dsk_b, FILL_A);
    const unsigned b_fill = file_count(s_dsk_b, FILL);
    const unsigned a_fill = file_count(s_dsk_a, FILL);

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));
    /* neshoda: jméno ukazuje na B.dsk, obsah a identita jsou z A.dsk */
    g_strlcpy(fdc->drive[0].filename, s_dsk_b, sizeof(fdc->drive[0].filename));

    /* "Sync now": chyba, nic se nezapíše, změny zůstávají v paměti */
    TEST_ASSERT_EQUAL_INT(0, fdc_sync_drive(fdc, 0));
    assert_files_untouched(b_fill_a, b_fill, a_fill);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    /* reset stroje ukládá cached obrazy */
    fdc_reset();
    assert_files_untouched(b_fill_a, b_fill, a_fill);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    /* vynucené uložení ("Save and switch") */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_force_save_to_file(fdc, 0));
    assert_files_untouched(b_fill_a, b_fill, a_fill);
    TEST_ASSERT_TRUE(fdc_drive_has_ram_changes(fdc, 0));
}


/**
 * @brief Snapshot záznamu vstupů vrací mechanice jméno ze snapshotu jako dřív; vysunutí pak cizí soubor nepřepíše.
 */
void test_replay_load_keeps_old_behaviour_eject(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned b_fill_a = file_count(s_dsk_b, FILL_A);
    const unsigned b_fill = file_count(s_dsk_b, FILL);
    const unsigned a_fill = file_count(s_dsk_a, FILL);
    unsigned serial0 = 0, serial = 0;
    snapshot_media_get_notice(&serial0);
    prepare_other_disk_scenario(true);

    /* dosavadní chování: jméno ze snapshotu, žádná zpráva o rozdílu */
    TEST_ASSERT_EQUAL_STRING(s_dsk_b, fdc->drive[0].filename);
    snapshot_media_get_notice(&serial);
    TEST_ASSERT_EQUAL_UINT(serial0, serial);

    fdc_umount(fdc, 0);
    assert_files_untouched(b_fill_a, b_fill, a_fill);
}


/**
 * @brief Ukončení emulátoru při neshodě jména a obsahu (snapshot záznamu vstupů) cizí soubor nepřepíše.
 */
void test_replay_load_keeps_old_behaviour_exit(void)
{
    const unsigned b_fill_a = file_count(s_dsk_b, FILL_A);
    const unsigned b_fill = file_count(s_dsk_b, FILL);
    const unsigned a_fill = file_count(s_dsk_a, FILL);
    prepare_other_disk_scenario(true);

    fdc_exit();
    assert_files_untouched(b_fill_a, b_fill, a_fill);
}


/**
 * @brief Prázdná mechanika si při načtení snapshotu záznamu vstupů bere pole ze snapshotu jako dřív.
 */
void test_replay_load_empty_drive_old_behaviour(void)
{
    st_FDC *fdc = &g_fdc[FDC0];

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    fdc_umount(fdc, 0);

    snapshot_media_set_replay_load(true);
    snap_load(snap, size);
    snapshot_media_set_replay_load(false);
    TEST_ASSERT_EQUAL_UINT(1, fdc->drive[0].mounted);
    TEST_ASSERT_EQUAL_STRING(s_dsk_a, fdc->drive[0].filename);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].handler_valid);

    /* úklid stavu, který test vyrobil mimo běžné cesty */
    fdc->drive[0].mounted = 0;
    fdc->drive[0].filename[0] = 0;
}


/**
 * @brief Snapshot se jménem souboru, který v mechanice je, ukládání nebrání.
 */
void test_same_name_still_saves(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned a_fill = file_count(s_dsk_a, FILL);

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    snap_load(snap, size);
    TEST_ASSERT_EQUAL_STRING(s_dsk_a, fdc->drive[0].filename);

    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 0));
    TEST_ASSERT_FALSE(fdc_drive_has_unsaved_changes(fdc, 0));
    TEST_ASSERT_EQUAL_UINT(a_fill + SECTOR_BYTES, file_count(s_dsk_a, FILL));
}


/**
 * @brief `readonly = 1` ze snapshotu nezablokuje uložení změn mechaniky, která smí zapisovat.
 */
void test_readonly_from_snapshot_keeps_unsaved_changes(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned a_fill = file_count(s_dsk_a, FILL);

    /* snapshot z doby, kdy byla mechanika chráněná volbou uživatele */
    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 0, NULL));

    /* mechanika zapisuje a má neuložené změny */
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fdc->drive[0].readonly, "readonly taken from the snapshot");
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    fdc_umount(fdc, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(a_fill + SECTOR_BYTES, file_count(s_dsk_a, FILL), "unsaved changes lost on eject");
}


/**
 * @brief `readonly = 0` ze snapshotu nesundá ochranu, kterou má vložené médium.
 */
void test_readonly_of_inserted_medium_survives(void)
{
    st_FDC *fdc = &g_fdc[FDC0];

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    size_t size = 0;
    uint8_t *snap = snap_save(&size); /* readonly = 0 */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 1, NULL));

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(fdc, 0));
}


/**
 * @brief Sdílený obraz: načtení snapshotu s prohozenými příznaky nesesadí mechaniku, která zapisuje.
 */
void test_shared_writer_keeps_role(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned a_fill = file_count(s_dsk_a, FILL);

    /* snapshot: FD0 chráněná volbou uživatele, FD1 se zápisem */
    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 1, NULL));
    fdc_mount_dskfile(fdc, 1, s_dsk_a);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    /* teď: FD0 zapisuje, FD1 chráněná kvůli sdílení */
    fdc_umount(fdc, 1);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 0, NULL));
    fdc_mount_dskfile(fdc, 1, s_dsk_a);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fdc->drive[0].readonly, "the writing drive lost its role");
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));

    fdc_umount(fdc, 0);
    TEST_ASSERT_EQUAL_UINT(a_fill + SECTOR_BYTES, file_count(s_dsk_a, FILL));
}


/**
 * @brief Obraz z paměti (přehrání vstupů): `readonly` ze snapshotu platí dál a do souboru se nezapíše nic.
 */
void test_buffer_image_unchanged(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned a_fill = file_count(s_dsk_a, FILL);

    fdc_mount_dskfile(fdc, 0, s_dsk_a);
    size_t img_size = 0;
    uint8_t *img = fdc_drive_copy_image(fdc, 0, &img_size);
    TEST_ASSERT_NOT_NULL(img);
    fdc_umount(fdc, 0);

    /* snapshot s readonly = 1 a jménem skutečného souboru */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, fdc_mount_dsk_buffer(fdc, 0, img, img_size, 1, s_dsk_a));
    size_t size = 0;
    uint8_t *snap = snap_save(&size);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, fdc_mount_dsk_buffer(fdc, 0, img, img_size, 0, "replay"));
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_ram_changes(fdc, 0));

    snap_load(snap, size);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_STRING(s_dsk_a, fdc->drive[0].filename);

    /* obraz z paměti soubor nedrží: vynucené uložení se odmítne */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_force_save_to_file(fdc, 0));
    fdc_umount_keep_cfg(fdc, 0);
    TEST_ASSERT_EQUAL_UINT(a_fill, file_count(s_dsk_a, FILL));
    g_free(img);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("fdc_snapmedia_XXXXXX", NULL);
    s_dsk_a = g_build_filename(s_tmpdir, "a.dsk", NULL);
    s_dsk_b = g_build_filename(s_tmpdir, "b.dsk", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_other_disk_in_snapshot_keeps_inserted);
    RUN_TEST(test_other_disk_in_snapshot_eject);
    RUN_TEST(test_empty_and_full_drive_kept);
    RUN_TEST(test_notice_not_repeated);
    RUN_TEST(test_guard_foreign_name_sync_and_reset);
    RUN_TEST(test_replay_load_keeps_old_behaviour_eject);
    RUN_TEST(test_replay_load_keeps_old_behaviour_exit);
    RUN_TEST(test_replay_load_empty_drive_old_behaviour);
    RUN_TEST(test_same_name_still_saves);
    RUN_TEST(test_readonly_from_snapshot_keeps_unsaved_changes);
    RUN_TEST(test_readonly_of_inserted_medium_survives);
    RUN_TEST(test_shared_writer_keeps_role);
    RUN_TEST(test_buffer_image_unchanged);
    int result = UNITY_END();

    g_remove(s_dsk_a);
    g_remove(s_dsk_b);
    g_free(s_dsk_a);
    g_free(s_dsk_b);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
