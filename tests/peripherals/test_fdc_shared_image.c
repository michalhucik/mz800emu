/**
 * @file test_fdc_shared_image.c
 * @brief Tentýž soubor obrazu diskety ve více mechanikách (fdc.h, fdc_fileid.h).
 *
 * Invariant: do jednoho souboru obrazu smí v každém okamžiku zapisovat
 * nejvýš jedna mechanika (přes oba řadiče). Test ověřuje:
 *  - druhé vložení téhož souboru dostane ochranu kvůli sdílení a upozornění,
 *  - zápis sektoru z chráněné mechaniky skončí stavem "write protect" jako
 *    u chráněné diskety a do souboru se nedostane ani při vysunutí,
 *  - sundání ochrany je odmítnuté, dokud první mechanika smí zapisovat,
 *    a jde až po jejím vysunutí nebo po zapnutí její ochrany (bez tichého
 *    odemčení),
 *  - shodu souboru přes cestu s `..`, s jinou velikostí písmen (Windows)
 *    a přes pevný odkaz,
 *  - sdílení přes oba řadiče a číslování mechanik 0 až 7,
 *  - že jediná mechanika, různé soubory a obraz nasazený z paměti se
 *    chovají jako dřív,
 *  - že vynucené uložení ("Save and switch") nepřepíše soubor, do kterého
 *    smí zapisovat jiná mechanika,
 *  - že vložení a vysunutí správcem Unicard běží pod zámkem stavu zařízení
 *    a zámek, který volající už drží, podruhé nebere,
 *  - pojistku invariantu po přepsání pole `readonly`
 *    (fdc_enforce_single_writer(); samotné načtení snapshotu ověřuje
 *    tests/snapshot/test_snapshot_fdc_media.c).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "device_request.h"
#include "replay.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_fileid.h"
#include "hw-generic/fdc/fdc_request.h"
#include "hw-generic/unicard/unicard.h"
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

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Cesta testovacího DSK v s_tmpdir. */
static char *s_dsk = NULL;
/** @brief Cesta druhého (jiného) DSK v s_tmpdir. */
static char *s_dsk2 = NULL;
/** @brief Uložené `connected` obou řadičů (obnova v tearDown). */
static unsigned s_saved_connected[FDC_INSTANCE_COUNT];


/** @brief Zapíše malý DSK (2 stopy, 16 x 256 B, výplň 0xFF) do souboru. */
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


/** @brief Počet bajtů FILL v souboru (data čerstvého DSK žádný nemají, hlavička může). */
static unsigned file_fill_count(const char *path)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    unsigned n = 0;
    for (gsize i = 0; i < len; i++)
        if ((uint8_t)data[i] == FILL) n++;
    g_free(data);
    return n;
}


/** @brief Vyřídí frontu zásahů a vrátí návratovou hodnotu posledního. */
static int poll_last_rc(void)
{
    device_request_poll();
    st_DEVICE_REQ_RESULT res, last;
    memset(&last, 0, sizeof(last));
    bool any = false;
    while (device_request_take_result(&res)) { last = res; any = true; }
    TEST_ASSERT_TRUE(any);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, last.status);
    return last.rc;
}


/** @brief Vysune vše a vrátí volby mechanik obou řadičů do výchozího stavu. */
static void reset_drives(void)
{
    for (unsigned f = 0; f < FDC_INSTANCE_COUNT; f++)
        for (unsigned d = 0; d < FDC_NUM_DRIVES; d++) {
            fdc_umount(&g_fdc[f], d);
            fdc_cfg_set_readonly(&g_fdc[f], d, 0);
            fdc_cfg_set_storage_mode(&g_fdc[f], d, "cached");
        }
}


void setUp(void)
{
    write_test_dsk(s_dsk);
    write_test_dsk(s_dsk2);
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
}


/**
 * @brief Jediná mechanika a různé soubory: žádná ochrana, žádné upozornění.
 */
void test_single_drive_unchanged(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned base = file_fill_count(s_dsk);
    fdc_mount_dskfile(fdc, 0, s_dsk);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 0));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_shared_with(fdc, 0));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 0));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 0));

    /* jiný soubor ve druhé mechanice: obě smějí zapisovat */
    fdc_mount_dskfile(fdc, 1, s_dsk2);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 1));

    /* nové otevření téhož obrazu v téže mechanice (změna volby) ochranu nepřidá */
    fdc_mount_dskfile(fdc, 0, s_dsk);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 0));

    /* volba read-only jedné mechaniky tam i zpět jako dřív */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_cfg_get_readonly(fdc, 0));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_cfg_get_readonly(fdc, 0));

    /* zápis sektoru projde a po uložení je v souboru */
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 0));
    TEST_ASSERT_EQUAL_UINT(base + SECTOR_BYTES, file_fill_count(s_dsk));
}


/**
 * @brief Druhé vložení téhož souboru je chráněné a nese upozornění.
 */
void test_second_insert_is_protected(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);
    fdc_mount_dskfile(fdc, 1, s_dsk);

    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_with(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_write_blocker(fdc, 1));
    /* volba uživatele v INI se nemění: ochrana kvůli sdílení je stav běhu */
    TEST_ASSERT_EQUAL_INT(0, fdc_cfg_get_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].user_readonly);

    /* upozornění jednou, první mechanika žádné nemá */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_take_share_notice(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 0));

    /* první mechanika zápis má a nic ji neblokuje */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 0));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 0));

    /* třetí mechanika dopadne stejně a odkazuje na tu se zápisem */
    fdc_mount_dskfile(fdc, 2, s_dsk);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 2));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_with(fdc, 2));

    /* zásah z GUI (okno se zprávou nahrazuje v testu stub) projde s rc 0 */
    fdc_request_mount(FDC0, 3, s_dsk);
    TEST_ASSERT_EQUAL_INT(0, poll_last_rc());
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 3));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_take_share_notice(fdc, 3)); /* vyzvedl ho zásah */
}


/**
 * @brief Zápis z chráněné mechaniky selže jako u chráněné diskety a soubor nepřepíše ani vysunutí.
 */
void test_write_from_second_drive_fails(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned base = file_fill_count(s_dsk);
    fdc_mount_dskfile(fdc, 0, s_dsk);
    fdc_mount_dskfile(fdc, 1, s_dsk);

    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(fdc, 1));
    TEST_ASSERT_FALSE(fdc_drive_has_unsaved_changes(fdc, 1));
    TEST_ASSERT_FALSE(fdc_drive_has_ram_changes(fdc, 1));

    /* stejný stav jako u diskety chráněné volbou uživatele */
    fdc_cfg_set_readonly(fdc, 2, 1);
    fdc_mount_dskfile(fdc, 2, s_dsk2);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 2));
    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(fdc, 2));

    /* první mechanika zapíše a uloží */
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 0) & ST_BUSY_DRQ);
    TEST_ASSERT_TRUE(fdc_drive_has_unsaved_changes(fdc, 0));
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 0));
    TEST_ASSERT_EQUAL_UINT(base + SECTOR_BYTES, file_fill_count(s_dsk));

    /* chráněná mechanika drží starší kopii: uložení, vynucené uložení ani vysunutí soubor nepřepíší */
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 1));
    fdc->drive[1].handler.spec.memspec.updated = 1; /* jako po opravě hlavičky DSK při vložení */
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_force_save_to_file(fdc, 1));
    fdc_umount(fdc, 1);
    TEST_ASSERT_EQUAL_UINT(base + SECTOR_BYTES, file_fill_count(s_dsk));
}


/**
 * @brief Ochranu nejde sundat, dokud první mechanika smí zapisovat; po jejím vysunutí ano, ale ne sama.
 */
void test_unprotect_refused_until_writer_leaves(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);
    fdc_mount_dskfile(fdc, 1, s_dsk);

    int blocker = 99;
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_set_write_protect(fdc, 1, 0, &blocker));
    TEST_ASSERT_EQUAL_INT(0, blocker);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));

    /* stejně dopadne zásah z GUI (menu, overlay) */
    fdc_request_set_readonly(FDC0, 1, 0);
    TEST_ASSERT_EQUAL_INT(-1, poll_last_rc());
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);

    /* změna režimu úložiště obraz znovu otevře, ochrana zůstává */
    fdc_request_set_storage_mode(FDC0, 1, "direct", false);
    poll_last_rc();
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_DIRECT, fdc->drive[1].storage_mode);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(fdc, 1));
    fdc_request_set_storage_mode(FDC0, 1, "cached", false);
    poll_last_rc();
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));

    /* první mechanika soubor vysune: omezení zmizí, ochrana ne (žádné tiché odemčení) */
    fdc_umount(fdc, 0);
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_shared_with(fdc, 1));
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    /* ani nové otevření obrazu ji nesundá */
    fdc_request_set_storage_mode(FDC0, 1, "discard", false);
    poll_last_rc();
    fdc_request_set_storage_mode(FDC0, 1, "cached", false);
    poll_last_rc();
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);

    /* uživatel ji sundá */
    fdc_request_set_readonly(FDC0, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, poll_last_rc());
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 1) & ST_BUSY_DRQ);

    /* jiný obraz v mechanice ochranu kvůli sdílení nedědí */
    fdc_mount_dskfile(fdc, 0, s_dsk);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 0));
    fdc_mount_dskfile(fdc, 0, s_dsk2);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 0));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
}


/**
 * @brief Opačný směr: první mechanika dostane ochranu, druhá zápis; první je pak zamčená.
 */
void test_writer_can_be_swapped(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);
    fdc_mount_dskfile(fdc, 1, s_dsk);

    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 0, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    /* druhá se sama neodemkne */
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_with(fdc, 1));

    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);

    int blocker = -1;
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_set_write_protect(fdc, 0, 0, &blocker));
    TEST_ASSERT_EQUAL_INT(1, blocker);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_cfg_get_readonly(fdc, 0)); /* odmítnutí nic nezměnilo */

    /* nové otevření první mechaniky invariant neporuší */
    fdc_request_set_storage_mode(FDC0, 0, "direct", false);
    poll_last_rc();
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
}


/**
 * @brief Tentýž soubor se pozná přes cestu s `..`, jinou velikost písmen (Windows) a pevný odkaz.
 */
void test_same_file_through_different_paths(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);

    /* cesta s ".." */
    char *sub = g_build_filename(s_tmpdir, "sub", NULL);
    TEST_ASSERT_EQUAL_INT(0, g_mkdir_with_parents(sub, 0700));
    char *dotdot = g_build_filename(s_tmpdir, "sub", "..", "a.dsk", NULL);
    fdc_mount_dskfile(fdc, 1, dotdot);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 1));
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    fdc_umount(fdc, 1);

#ifdef G_OS_WIN32
    /* jiná velikost písmen a obrácená lomítka */
    char *upper = g_build_filename(s_tmpdir, "A.DSK", NULL);
    for (char *p = upper; *p; p++)
        if (*p == '/') *p = '\\';
    fdc_mount_dskfile(fdc, 1, upper);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 1));
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
    fdc_umount(fdc, 1);
    g_free(upper);
#endif

    /* pevný odkaz: jiné jméno, tentýž soubor (číslo souboru v souborovém systému) */
    char *link_path = g_build_filename(s_tmpdir, "link.dsk", NULL);
    g_remove(link_path);
#ifdef G_OS_WIN32
    wchar_t *wlink = (wchar_t *)g_utf8_to_utf16(link_path, -1, NULL, NULL, NULL);
    wchar_t *wsrc = (wchar_t *)g_utf8_to_utf16(s_dsk, -1, NULL, NULL, NULL);
    bool linked = wlink && wsrc && CreateHardLinkW(wlink, wsrc, NULL);
    g_free(wlink);
    g_free(wsrc);
#else
    bool linked = (link(s_dsk, link_path) == 0);
#endif
    if (linked) {
        st_FDC_FILEID a, b;
        fdc_fileid_init(&a, s_dsk);
        fdc_fileid_init(&b, link_path);
        const bool ids = a.has_id && b.has_id;
        if (ids) TEST_ASSERT_TRUE(fdc_fileid_same(&a, &b));
        fdc_fileid_clear(&a);
        fdc_fileid_clear(&b);
        if (ids) {
            fdc_mount_dskfile(fdc, 1, link_path);
            TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));
            fdc_umount(fdc, 1);
        } else {
            TEST_MESSAGE("file system gives no file id, hard link not recognized (documented gap)");
        }
        g_remove(link_path);
    } else {
        TEST_MESSAGE("hard link cannot be created here, check skipped");
    }
    g_free(link_path);

    /* jiný soubor se stejným obsahem sdílený není */
    fdc_mount_dskfile(fdc, 1, s_dsk2);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 1));

    /* prázdná identita se neshoduje s ničím */
    st_FDC_FILEID e1, e2;
    fdc_fileid_init(&e1, "");
    fdc_fileid_init(&e2, NULL);
    TEST_ASSERT_FALSE(fdc_fileid_same(&e1, &e2));
    TEST_ASSERT_FALSE(fdc_fileid_same(&e1, &fdc->drive[0].fileid));

    g_rmdir(sub);
    g_free(sub);
    g_free(dotdot);
}


/**
 * @brief Sdílení přes oba řadiče; mechaniky se číslují 0 až 7.
 */
void test_two_controllers(void)
{
    fdc_mount_dskfile(&g_fdc[FDC1], 2, s_dsk);
    TEST_ASSERT_EQUAL_INT(0, g_fdc[FDC1].drive[2].readonly);

    fdc_mount_dskfile(&g_fdc[FDC0], 0, s_dsk);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_INT(6, fdc_drive_shared_with(&g_fdc[FDC0], 0));
    TEST_ASSERT_EQUAL_INT(6, fdc_drive_take_share_notice(&g_fdc[FDC0], 0));

    int blocker = -1;
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_set_write_protect(&g_fdc[FDC0], 0, 0, &blocker));
    TEST_ASSERT_EQUAL_INT(6, blocker);

    /* odpojený řadič obraz dál drží (při ukončení ho ukládá): omezení trvá */
    g_fdc[FDC1].connected = FDC_DISCONNECTED;
    TEST_ASSERT_EQUAL_INT(6, fdc_drive_write_blocker(&g_fdc[FDC0], 0));
    g_fdc[FDC1].connected = FDC_CONNECTED;

    fdc_umount(&g_fdc[FDC1], 2);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(&g_fdc[FDC0], 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_fdc[FDC0].drive[0].readonly);

    /* a obráceně: zápis drží FDC0 FD0, FDC1 FD1 je mechanika 5 */
    fdc_mount_dskfile(&g_fdc[FDC1], 1, s_dsk);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(&g_fdc[FDC1], 1));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_with(&g_fdc[FDC1], 1));
    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(&g_fdc[FDC1], 1));
}


/**
 * @brief Obraz nasazený z paměti (přehrání vstupů) soubor nedrží a pravidlo se ho netýká.
 */
void test_buffer_mount_is_not_shared(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);
    size_t size = 0;
    uint8_t *img = fdc_drive_copy_image(fdc, 0, &size);
    TEST_ASSERT_NOT_NULL(img);

    /* jmenovka shodná s cestou souboru v mechanice 0: stejně se nesdílí */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, fdc_mount_dsk_buffer(fdc, 1, img, size, 0, s_dsk));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 1));
    TEST_ASSERT_EQUAL_INT(-1, fdc_drive_write_blocker(fdc, 0));

    /* soubor vložený vedle obrazu z paměti se stejnou jmenovkou zápis má */
    fdc_umount(fdc, 0);
    fdc_mount_dskfile(fdc, 2, s_dsk);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[2].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 2));

    g_free(img);
    fdc_umount_keep_cfg(fdc, 1);
}


/**
 * @brief Po přímém přepsání pole `readonly` pojistka fdc_enforce_single_writer() invariant obnoví.
 *
 * Načtení snapshotu pole `readonly` mechaniky s obrazem ze souboru už
 * nepřepisuje; test ověřuje funkci samotnou.
 */
void test_enforce_after_snapshot_load(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    fdc_mount_dskfile(fdc, 0, s_dsk);
    fdc_mount_dskfile(fdc, 1, s_dsk);
    fdc_mount_dskfile(fdc, 2, s_dsk2);

    /* sdílená mechanika přišla o readonly */
    fdc->drive[1].readonly = 0;
    fdc_enforce_single_writer();
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[2].readonly); /* nesdílená mechanika beze změny */
    TEST_ASSERT_EQUAL_HEX8(ST_WRITE_PROTECT, emu_write_sector(fdc, 1));

    /* dvě mechaniky se zápisem na tentýž soubor: zápis zůstane první v pořadí */
    fdc->drive[1].shared_readonly = 0;
    fdc->drive[1].readonly = 0;
    fdc_enforce_single_writer();
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_shared_readonly(fdc, 1));

    /* mechaniky bez sdíleného souboru se pojistka netýká */
    fdc->drive[2].readonly = 1;
    fdc_enforce_single_writer();
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[2].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 2));
}


/**
 * @brief Vynucené uložení nepřepíše soubor, který drží se zápisem jiná mechanika.
 *
 * Mechanika 0 je chráněná volbou uživatele (ne sdílením) a v režimu discard
 * má starou kopii; mechanika 1 do téhož souboru zapisuje. "Save and switch"
 * z mechaniky 0 by soubor přepsal starou kopií.
 */
void test_force_save_respects_other_writer(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    const unsigned base = file_fill_count(s_dsk);

    fdc_cfg_set_storage_mode(fdc, 0, "discard");
    fdc_cfg_set_readonly(fdc, 0, 1);
    fdc_mount_dskfile(fdc, 0, s_dsk);
    TEST_ASSERT_EQUAL_INT(1, fdc->drive[0].readonly);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_shared_readonly(fdc, 0));

    /* druhá mechanika: ochranu kvůli sdílení jde sundat, první nezapisuje */
    fdc_mount_dskfile(fdc, 1, s_dsk);
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_set_write_protect(fdc, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, fdc->drive[1].readonly);
    TEST_ASSERT_EQUAL_HEX8(ST_BUSY_DRQ, emu_write_sector(fdc, 1) & ST_BUSY_DRQ);
    TEST_ASSERT_EQUAL_INT(1, fdc_sync_drive(fdc, 1));
    TEST_ASSERT_EQUAL_UINT(base + SECTOR_BYTES, file_fill_count(s_dsk));

    /* stará kopie mechaniky 0 se do souboru nedostane */
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_write_blocker(fdc, 0));
    TEST_ASSERT_EQUAL_INT(0, fdc_drive_force_save_to_file(fdc, 0));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(base + SECTOR_BYTES, file_fill_count(s_dsk), "stale copy overwrote the file");

    /* jakmile druhá mechanika soubor nedrží, vynucené uložení projde jako dřív */
    fdc_umount(fdc, 1);
    TEST_ASSERT_EQUAL_INT(1, fdc_drive_force_save_to_file(fdc, 0));
    TEST_ASSERT_EQUAL_UINT(base, file_fill_count(s_dsk));
}


/** @brief Počet volání ui_fdc_set_dsk(), při kterých volající vlákno drželo zámek stavu zařízení. */
static int s_ui_calls_locked = 0;
/** @brief Počet volání ui_fdc_set_dsk() bez zámku stavu zařízení. */
static int s_ui_calls_unlocked = 0;

/**
 * @brief Náhrada slabého ui_fdc_set_dsk() z fdc.c: počítá, zda změna mechaniky běží pod zámkem.
 *
 * Řadič ji volá po každé změně obrazu v mechanice, tedy uvnitř úseku,
 * který má být zamčený.
 */
void ui_fdc_set_dsk(unsigned drive_id, char *dsk_filename)
{
    (void)drive_id;
    (void)dsk_filename;
    if (device_state_lock_held()) s_ui_calls_locked++;
    else s_ui_calls_unlocked++;
}


/**
 * @brief Správce Unicard vkládá a vysouvá obraz pod zámkem stavu zařízení.
 *
 * Bez zámku by emu vlákno uvolnilo jméno a identitu souboru mechaniky,
 * které UI čte pod zámkem. Zámek není rekurzivní: drží-li ho volající,
 * funkce ho nebere ani neodemyká.
 */
void test_unicard_mount_takes_device_lock(void)
{
    st_FDC *fdc = &g_fdc[FDC0];
    char *saved_root = g_strdup(unicard_get_sd_root_dirpath());
    const en_UNICARD_MANAGED_FDC saved_managed = unicard_get_managed_fdc();
    unicard_set_sd_root_dirpath(s_tmpdir);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);

    /* volající zámek nedrží: funkce si ho vezme a zase pustí */
    s_ui_calls_locked = s_ui_calls_unlocked = 0;
    TEST_ASSERT_FALSE(device_state_lock_held());
    unicard_fdc_mount(0, (char *)"a.dsk");
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_FALSE(device_state_lock_held());
    TEST_ASSERT_GREATER_THAN_INT(0, s_ui_calls_locked);
    TEST_ASSERT_EQUAL_INT(0, s_ui_calls_unlocked);

    unicard_fdc_mount(0, (char *)"");
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(fdc, 0));
    TEST_ASSERT_FALSE(device_state_lock_held());
    TEST_ASSERT_EQUAL_INT(0, s_ui_calls_unlocked);

    /* volající zámek drží (zásah GUI, příkaz MCP): žádné druhé zamčení, zámek zůstane volajícímu */
    device_state_lock();
    unicard_fdc_mount(1, (char *)"a.dsk");
    TEST_ASSERT_TRUE(device_state_lock_held());
    unicard_fdc_mount(1, (char *)"");
    TEST_ASSERT_TRUE(device_state_lock_held());
    device_state_unlock();
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(fdc, 1));
    TEST_ASSERT_EQUAL_INT(0, s_ui_calls_unlocked);

    unicard_set_managed_fdc(saved_managed);
    unicard_set_sd_root_dirpath(saved_root);
    g_free(saved_root);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("fdc_shared_XXXXXX", NULL);
    s_dsk = g_build_filename(s_tmpdir, "a.dsk", NULL);
    s_dsk2 = g_build_filename(s_tmpdir, "b.dsk", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_single_drive_unchanged);
    RUN_TEST(test_second_insert_is_protected);
    RUN_TEST(test_write_from_second_drive_fails);
    RUN_TEST(test_unprotect_refused_until_writer_leaves);
    RUN_TEST(test_writer_can_be_swapped);
    RUN_TEST(test_same_file_through_different_paths);
    RUN_TEST(test_two_controllers);
    RUN_TEST(test_buffer_mount_is_not_shared);
    RUN_TEST(test_enforce_after_snapshot_load);
    RUN_TEST(test_force_save_respects_other_writer);
    RUN_TEST(test_unicard_mount_takes_device_lock);
    int result = UNITY_END();

    g_remove(s_dsk);
    g_remove(s_dsk2);
    g_free(s_dsk);
    g_free(s_dsk2);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
