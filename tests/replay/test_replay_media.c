/**
 * @file test_replay_media.c
 * @brief Unit testy médií v záznamu vstupů (replay_media.c) bez GUI.
 *
 * Hlavní smyčku simuluje test jako test_replay_input.c (pauza přímo,
 * konec snímku = zvýšení `g_gdg.total_elapsed.screens` a volání
 * replay_on_frame_boundary()).
 *
 * Pokrývá:
 *   - záznam uloží obsah nasazeného DSK z paměti (i se změnou, která ještě
 *     není v souboru) a obsah disku IDE8,
 *   - přehrání nasadí kopie ze záznamu (DSK v režimu discard, IDE8 nad
 *     dočasným souborem), zápisy přehrání se do originálů nedostanou,
 *     cesta v INI se nezmění,
 *   - mechanika bez obrazu v záznamu je při přehrání bez obrazu a po konci
 *     přehrání se vrátí obraz podle INI,
 *   - Quick Disk: obraz z paměti (discard, i u odpojené mechaniky), kopie
 *     obsahu a zavření bez zápisu do INI,
 *   - operace kazety z oken (replay_host_cmt()): mimo záznam je provede
 *     paused smyčka, při záznamu se zapíší jako události `cmt` s polohou
 *     `p` (pauza na hranici) nebo `b` (hranice snímku za běhu), přehrání
 *     vloží kopii pásky ze záznamu a operace zopakuje; operace s podmínkami
 *     (replay_host_cmt_guarded()), které při provedení neplatí, se
 *     neprovedou ani nezapíší; zahozená operace zvedne čítač varování okna
 *     (cmt_request_get_window_drop_serial()) mimo přehrávání, při něm ne,
 *   - MCP seek na blok 2 s do přehrávání (dbgapi): přehrání záznamu dá
 *     stejnou pozici v novém bloku jako živý běh (blok od začátku),
 *   - CMT hack nad páskou: záznam začatý s páskou "na konci" (hack přečetl
 *     poslední program) přehraje LOAD stejně jako při záznamu (konec pásky
 *     nese startovní snapshot),
 *   - snapshot uložený během přehrání nese cestu původní pásky, ne dočasné
 *     kopie přehrání: po konci přehrání ho jde načíst i s páskou.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/replay.h"
#include "emulator/replay_format.h"
#include "emulator/emulator.h"
#include "hw-generic/gdg/gdg.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/ide8/ide8.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/cmt/cmt.h"
#include "fs_layer.h"
#include "libs/dsk/dsk_tools.h"
#include "libs/generic_driver/memory_driver.h"
#include "emulator/media_writes.h"
#include "device_request.h"
#include "emulator/machine_hash.h"
#include "emulator/state_hash.h"
#include "emulator/state_rev.h"
#include "hw-generic/cmt/cmthack.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/replay_media.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmt_tmz_record.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "libs/mztape/mztape.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/mzf/mzf.h"
#include "libs/tmz/tmz_blocks.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#endif

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;

void setUp(void)
{
    g_emulator.paused = true;
    /* test, který selhal uprostřed záznamu nebo přehrání, nesmí shodit další */
    if (replay_is_active())
    {
        replay_request_stop();
        replay_on_paused();
    };
    replay_test_set_ckpt_record_max(0);
    replay_media_test_set_embed_limit(0);
}

void tearDown(void)
{
}


/** @brief Cesta souboru v dočasném adresáři (g_free). */
static char *tmp_path(const char *name)
{
    return g_build_filename(s_tmpdir, name, NULL);
}


/** @brief Konec snímku v simulované hlavní smyčce. */
static void next_frame(void)
{
    g_gdg.total_elapsed.screens++;
    replay_on_frame_boundary();
}


/** @brief Zapíše malý MZ-BASIC DSK (2 stopy, 16 x 256 B) do souboru. */
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


/** @brief Cesta obrazu mechaniky FDC0 v INI (klíč `wd279x_fddN_dskpath`). */
static const char *cfg_dskpath(unsigned drive)
{
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(&g_fdc[0]));
    TEST_ASSERT_NOT_NULL(cmod);
    return cfgmodule_get_element_text_value_by_name(cmod, (char *)fdc_dskpath_keyname(&g_fdc[0], drive));
}


/** @brief Ukazatel na paměťový obraz mechaniky (musí být cached / discard). */
static uint8_t *drive_mem(unsigned drive)
{
    st_FDDrive *drv = &g_fdc[0].drive[drive];
    TEST_ASSERT_TRUE(drv->mounted);
    TEST_ASSERT_EQUAL_INT(HANDLER_TYPE_MEMORY, drv->handler.type);
    return drv->handler.spec.memspec.ptr;
}


/** @brief "Zápis programu" do obrazu: bajt + příznak změny (jako WRITE SECTOR). */
static void drive_poke(unsigned drive, size_t off, uint8_t value)
{
    drive_mem(drive)[off] = value;
    g_fdc[0].drive[drive].handler.spec.memspec.updated = 1;
}


/** @brief Bajt souboru na pozici @p off. */
static uint8_t file_byte(const char *path, size_t off)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_TRUE(off < len);
    uint8_t b = (uint8_t)data[off];
    g_free(data);
    return b;
}


/** @brief Zapíše bajt na pozici @p off disku IDE8 (přes jeho otevřený soubor). */
static void ide8_poke(int drive, uint32_t off, uint8_t value)
{
    st_IDE8_DRIVE *d = &g_ide8.drive[drive];
    TEST_ASSERT_TRUE(d->connected == IDE8_STATE_CONNECTED);
    FS_LAYER_FSEEK(d->fp, off);
    unsigned wl = 0;
    TEST_ASSERT_EQUAL_INT(FS_LAYER_FR_OK, FS_LAYER_FWRITE(d->fp, &value, 1, &wl));
    FS_LAYER_FSYNC(d->fp);
}


/** @brief Bajt na pozici @p off disku IDE8. */
static uint8_t ide8_peek(int drive, uint32_t off)
{
    st_IDE8_DRIVE *d = &g_ide8.drive[drive];
    FS_LAYER_FSEEK(d->fp, off);
    uint8_t value = 0;
    unsigned rl = 0;
    TEST_ASSERT_EQUAL_INT(FS_LAYER_FR_OK, FS_LAYER_FREAD(d->fp, &value, 1, &rl));
    return value;
}


/** @brief Počet volání ui_fdc_set_dsk() pod zámkem stavu zařízení. */
static int s_fdc_ui_locked = 0;
/** @brief Počet volání ui_fdc_set_dsk() bez zámku stavu zařízení. */
static int s_fdc_ui_unlocked = 0;

/**
 * @brief Náhrada slabého ui_fdc_set_dsk() z fdc.c: počítá, zda výměna obrazu v mechanice běží pod zámkem.
 *
 * Řadič ji volá po každé změně obrazu; výměna uvolňuje paměť mechaniky,
 * kterou UI čte pod device_state_lock().
 */
void ui_fdc_set_dsk(unsigned drive_id, char *dsk_filename)
{
    (void)drive_id;
    (void)dsk_filename;
    if (device_state_lock_held()) s_fdc_ui_locked++;
    else s_fdc_ui_unlocked++;
}


/* Záznam uloží obsah DSK a IDE8, přehrání pracuje nad kopiemi. Výměna
 * disket při startu a konci přehrání běží pod zámkem stavu zařízení. */
void test_media_recorded_and_played_on_copies(void)
{
    char *dsk = tmp_path("a.dsk");
    char *dsk2 = tmp_path("b.dsk");
    char *hdd = tmp_path("hdd.img");
    char *mzr = tmp_path("media.mzr");
    write_test_dsk(dsk);
    write_test_dsk(dsk2);
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_TRUE(g_file_set_contents(hdd, (const char *)zeros, sizeof(zeros), NULL));

    const size_t off = 0x200; /* uvnitř obrazu, mimo hlavičku disku */
    fdc_cfg_set_storage_mode(&g_fdc[0], 1, "cached");
    fdc_mount_dskfile(&g_fdc[0], 1, dsk);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 1));
    uint8_t orig = drive_mem(1)[off];
    /* Změna jen v paměti (cached, ještě ne v souboru). */
    drive_poke(1, off, (uint8_t)(orig ^ 0x5A));

    /* Disk připojený podle INI (hdd0_filepath), jako při startu emulátoru. */
    st_IDE8_DRIVE *hd = &g_ide8.drive[0];
    char *saved_filepath = hd->filepath;
    hd->filepath = g_strdup(hdd);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, ide8_drive_open_image(hd, hd->filepath));
    ide8_poke(0, 100, 0x11);

    /* --- záznam --- */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    /* "Zápis programu" během záznamu - do záznamu se už nedostane. */
    drive_poke(1, off, 0xC3);
    ide8_poke(0, 100, 0x22);
    next_frame();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(mzr, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    const st_REPLAY_MEDIA *m = replay_data_find_media(d, "fdc0.1");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(m->data);
    TEST_ASSERT_EQUAL_HEX8(orig ^ 0x5A, m->data[off]);
    TEST_ASSERT_EQUAL_STRING(dsk, m->name);
    m = replay_data_find_media(d, "ide8.0");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT64(sizeof(zeros), m->size);
    TEST_ASSERT_EQUAL_HEX8(0x11, m->data[100]);
    TEST_ASSERT_NULL(replay_data_find_media(d, "fdc0.2"));
    replay_data_free(d);
    g_free(err);

    /* Před přehráním: do mechaniky 2 jiný obraz (v záznamu nebyl). */
    fdc_mount_dskfile(&g_fdc[0], 2, dsk2);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 2));

    /* --- přehrání --- */
    s_fdc_ui_locked = s_fdc_ui_unlocked = 0;
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    /* nasazení kopií (mechanika 1) a odpojení (mechanika 2) pod zámkem */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, s_fdc_ui_locked);
    TEST_ASSERT_EQUAL_INT(0, s_fdc_ui_unlocked);
    TEST_ASSERT_FALSE(device_state_lock_held());
    /* Obsah ze záznamu (stav při startu), discard, INI beze změny. */
    TEST_ASSERT_EQUAL_HEX8(orig ^ 0x5A, drive_mem(1)[off]);
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_DISCARD, g_fdc[0].drive[1].storage_mode);
    TEST_ASSERT_EQUAL_STRING(dsk, cfg_dskpath(1));
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[0], 2));
    TEST_ASSERT_EQUAL_HEX8(0x11, ide8_peek(0, 100));

    /* Zápisy přehrání. */
    drive_poke(1, off, 0x99);
    ide8_poke(0, 100, 0x33);
    s_fdc_ui_locked = s_fdc_ui_unlocked = 0;
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    /* návrat disket uživatele pod zámkem */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, s_fdc_ui_locked);
    TEST_ASSERT_EQUAL_INT(0, s_fdc_ui_unlocked);
    TEST_ASSERT_FALSE(device_state_lock_held());

    /* Po konci: originály podle INI, zápisy přehrání v nich nejsou. Změny
     * ze záznamu (cached) se při odpojení před přehráním zapsaly. */
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 1));
    TEST_ASSERT_EQUAL_STRING(dsk, g_fdc[0].drive[1].filename);
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_CACHED, g_fdc[0].drive[1].storage_mode);
    TEST_ASSERT_EQUAL_HEX8(0xC3, file_byte(dsk, off));
    TEST_ASSERT_EQUAL_HEX8(0xC3, drive_mem(1)[off]);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 2));
    TEST_ASSERT_EQUAL_STRING(dsk2, g_fdc[0].drive[2].filename);
    TEST_ASSERT_EQUAL_HEX8(0x22, ide8_peek(0, 100));
    TEST_ASSERT_EQUAL_HEX8(0x22, file_byte(hdd, 100));

    fdc_umount(&g_fdc[0], 1);
    fdc_umount(&g_fdc[0], 2);
    ide8_drive_close_image(hd);
    g_free(hd->filepath);
    hd->filepath = saved_filepath;
    g_remove(dsk);
    g_remove(dsk2);
    g_remove(hdd);
    g_remove(mzr);
    g_free(dsk);
    g_free(dsk2);
    g_free(hdd);
    g_free(mzr);
}


/* Zápis sektoru IDE8 přes registry zvýší počítadlo zápisů na média
 * (checkpoint pak nejde použít); čtení ho nezmění. */
void test_media_writes_counted_ide8(void)
{
    char *hdd = tmp_path("cnt.img");
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_TRUE(g_file_set_contents(hdd, (const char *)zeros, sizeof(zeros), NULL));
    st_IDE8_DRIVE *hd = &g_ide8.drive[0];
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, ide8_drive_open_image(hd, hdd));

    /* Sektor 1 adresovaný LBA, jeden, master. */
    ide8_write_byte(IDE8_ADDR_HEAD, (uint8_t)(0xA0 | IDE8_HEAD_BUSMODEMASK));
    ide8_write_byte(IDE8_ADDR_SCOUNT, 1);
    ide8_write_byte(IDE8_ADDR_SECTOR, 1);
    ide8_write_byte(IDE8_ADDR_LCYL, 0);
    ide8_write_byte(IDE8_ADDR_HCYL, 0);
    uint32_t before = g_media_writes;
    ide8_write_byte(IDE8_ADDR_COMMAND, IDE8_CMD_SECTOR_READ);
    for (int i = 0; i < IDE8_SECTOR_SIZE; i++) (void)ide8_read_byte(IDE8_ADDR_DATA);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, g_media_writes, "reading must not count");

    ide8_write_byte(IDE8_ADDR_SCOUNT, 1);
    ide8_write_byte(IDE8_ADDR_SECTOR, 1);
    ide8_write_byte(IDE8_ADDR_COMMAND, IDE8_CMD_SECTOR_WRITE);
    for (int i = 0; i < IDE8_SECTOR_SIZE && hd->cmd == IDE8_CMD_SECTOR_WRITE; i++)
    {
        ide8_write_byte(IDE8_ADDR_DATA, 0x5A);
    };
    TEST_ASSERT_EQUAL_UINT32(before + 1, g_media_writes);
    /* Sektor 1 v LBA = blok 0 (ide8_drive_assign_registers odečte 1). */
    TEST_ASSERT_EQUAL_HEX8(0x5A, ide8_peek(0, 0));

    ide8_drive_close_image(hd);
    g_remove(hdd);
    g_free(hdd);
}


/* Quick Disk: nasazení z paměti, kopie obsahu a zavření bez INI. */
void test_qdisk_buffer_mount(void)
{
    uint8_t img[300];
    for (int i = 0; i < 300; i++) img[i] = (uint8_t)(i * 3);
    unsigned saved_connected = g_qdisk.connected;
    unsigned saved_type = g_qdisk.type;
    g_qdisk.type = QDISK_TYPE_IMAGE;

    /* Funguje i s odpojenou mechanikou (přehrání ji nasazuje před snapshotem). */
    g_qdisk.connected = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, qdisk_mount_image_buffer(img, sizeof(img), 1, "x.mzq"));
    TEST_ASSERT_TRUE(g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_INT(QDISK_STORAGE_DISCARD, g_qdisk.storage_mode);
    TEST_ASSERT_TRUE(g_qdisk.readonly);
    TEST_ASSERT_TRUE(g_qdisk.status & QDSTS_IMG_READY);

    size_t size = 0;
    uint8_t *copy = qdisk_copy_image(&size);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_size_t(sizeof(img), size);
    TEST_ASSERT_EQUAL_MEMORY(img, copy, sizeof(img));
    g_free(copy);

    qdisk_umount_keep_cfg();
    TEST_ASSERT_FALSE(g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_UINT(QDSTS_NO_DISC, g_qdisk.status);
    TEST_ASSERT_NULL(qdisk_copy_image(&size));

    g_qdisk.connected = saved_connected;
    g_qdisk.type = saved_type;
}


/** @brief Zapíše malé MZF (tělo 64 B na 1200h). */
static void write_test_mzf(const char *path)
{
    uint8_t buf[128 + 64];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;
    memcpy(buf + 1, "TEST\r", 5);
    buf[0x12] = 64;
    buf[0x15] = 0x12;
    buf[0x17] = 0x12;
    for (int i = 0; i < 64; i++) buf[128 + i] = (uint8_t)i;
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)buf, sizeof(buf), NULL));
}


/** @brief Počet a poslední událost `cmt` v souboru záznamu. */
static int cmt_events(const char *mzr, const st_REPLAY_EVENT **last, st_REPLAY_DATA **out)
{
    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(mzr, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    g_free(err);
    int n = 0;
    for (guint i = 0; i < d->events->len; i++)
    {
        const st_REPLAY_EVENT *e = &g_array_index(d->events, st_REPLAY_EVENT, i);
        if (e->kind != REPLAY_EV_CMT) continue;
        n++;
        if (last) *last = e;
    };
    *out = d;
    return n;
}


/* Operace kazety z oken: mimo záznam provede paused smyčka, při záznamu
 * události cmt (p / b), přehrání pásku vloží z kopie a operace zopakuje. */
void test_cmt_window_ops_recorded(void)
{
    char *mzf = tmp_path("t.mzf");
    char *mzr = tmp_path("cmt.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));

    /* Mimo záznam: provede až emu vlákno (paused smyčka). */
    replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    /* Záznam: play v pauze na hranici (p), pauza za běhu na hranici (b). */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    g_emulator.paused = false;
    replay_host_cmt(REPLAY_CMT_PAUSE, 1, NULL);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    const st_REPLAY_EVENT *last = NULL;
    st_REPLAY_DATA *d = NULL;
    TEST_ASSERT_EQUAL_INT(2, cmt_events(mzr, &last, &d));
    TEST_ASSERT_EQUAL_STRING("pause", last->cmd);
    TEST_ASSERT_EQUAL_INT32(1, last->arg);
    TEST_ASSERT_EQUAL_INT(REPLAY_AT_BOUNDARY, last->at);
    TEST_ASSERT_EQUAL_UINT32(1, last->frame);
    const st_REPLAY_MEDIA *m = replay_data_find_media(d, "cmt");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(m->data);
    TEST_ASSERT_EQUAL_UINT64(128 + 64, m->size);
    TEST_ASSERT_NOT_NULL(d->cmt_json);
    replay_data_free(d);

    /* Přehrání bez vložené pásky: páska z kopie, stav podle záznamu. */
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING("t.mzf", cmt_get_ui_base_filename());
    /* Okna jsou při přehrávání zamčená; operace z UI se zahodí. */
    replay_host_cmt(REPLAY_CMT_EJECT, 0, NULL);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_FALSE(st.deviated);
    /* Po konci: před přehráním nebyla vložená žádná páska. */
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


/* Operace okna s podmínkami (replay_host_cmt_guarded()), které při provedení
 * neplatí, se při záznamu nezapíší: pauza zadaná při PLAY a provedená ve
 * STOP ani SEEK s čítačem jiné pásky; platná operace se zapíše jako dřív
 * (bez podmínek, přehrání je nepotřebuje). */
void test_cmt_guarded_op_dropped_not_recorded(void)
{
    char *mzf = tmp_path("g.mzf");
    char *mzr = tmp_path("guard.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    st_REPLAY_CMT_GUARD g;
    g.flags = REPLAY_CMT_GUARD_NOT_STOP;
    g.tape_serial = 0u;
    replay_host_cmt_guarded(REPLAY_CMT_PAUSE, 1, &g); /* ve STOP: zahodí se */
    g.flags = REPLAY_CMT_GUARD_TAPE;
    g.tape_serial = cmttmz_get_tape_serial() + 1u;
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g);  /* jiná páska: zahodí se */
    g.tape_serial = cmttmz_get_tape_serial();
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g);  /* tatáž páska: provede se */
    replay_on_paused();
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_STOP, "the dropped pause did not start the tape");
    next_frame();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    const st_REPLAY_EVENT *last = NULL;
    st_REPLAY_DATA *d = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmt_events(mzr, &last, &d), "only the operation that ran is recorded");
    TEST_ASSERT_EQUAL_STRING("seek", last->cmd);
    TEST_ASSERT_EQUAL_INT32(0, last->arg);
    replay_data_free(d);
    cmt_eject();
    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


/* Zahozená operace okna se počítá pro varování okna (cmt_request_note_window_drop())
 * jen mimo přehrávání záznamu vstupů: mimo přehrávání a při záznamu ano,
 * při přehrávání (zařazení odmítnuté i operace zařazená těsně před startem
 * přehrávání a vyřízená během něj) ne. */
void test_cmt_guarded_drop_counted_not_in_playback(void)
{
    char *mzf = tmp_path("d.mzf");
    char *mzr = tmp_path("drop.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
    st_REPLAY_CMT_GUARD g;
    g.flags = REPLAY_CMT_GUARD_TAPE;
    g.tape_serial = cmttmz_get_tape_serial() + 1u;

    /* mimo záznam vstupů: počítá se */
    unsigned d0 = cmt_request_get_window_drop_serial();
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g);
    replay_on_paused();
    TEST_ASSERT_EQUAL_UINT(d0 + 1u, cmt_request_get_window_drop_serial());

    /* při záznamu: počítá se (a nezapíše se, viz test_cmt_guarded_op_dropped_not_recorded) */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g);
    replay_on_paused();
    TEST_ASSERT_EQUAL_UINT(d0 + 2u, cmt_request_get_window_drop_serial());
    next_frame();
    next_frame();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    /* při přehrávání: nepočítá se */
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_PLAYING, g_atomic_int_get(&g_replay_mode));
    g.tape_serial = cmttmz_get_tape_serial() + 1u;
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g); /* zařazení při přehrávání odmítnuto */
    /* operace zařazená UI vláknem těsně před startem přehrávání (UI ještě
     * četlo režim OFF), vyřízená až během přehrávání */
    g_atomic_int_set(&g_replay_mode, REPLAY_MODE_OFF);
    replay_host_cmt_guarded(REPLAY_CMT_SEEK, 0, &g);
    g_atomic_int_set(&g_replay_mode, REPLAY_MODE_PLAYING);
    next_frame();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(d0 + 2u, cmt_request_get_window_drop_serial(), "operations dropped during input replay playback are not reported");
    while (replay_test_take_ui_cmt(NULL, NULL)) {}
    for (int i = 0; i < 4 && g_atomic_int_get(&g_replay_mode) == REPLAY_MODE_PLAYING; i++) next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    cmt_eject();
    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

/** @brief Pozice pásky (blok, takty v bloku); selhání = chyba testu. */
static void tape_position(int *block_id, uint64_t *ticks)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(block_id, ticks));
}


/* MCP seek (dbgapi DBGAPI_CMD_CMT_TAPE_SEEK) 2 s do přehrávání bloku 0:
 * živě blok 1 hraje od začátku; záznam ho uloží jako událost cmt "seek"
 * a přehrání musí dát tutéž pozici v bloku 1 (dříve přehrání volalo
 * cb_open_block přímo a blok 1 se četl od času uplynulého v bloku 0). */
void test_cmt_mcp_seek_while_playing_replayed(void)
{
    char *mzf = tmp_path("seek.mzf");
    char *mzt = tmp_path("seek.mzt");
    char *mzr = tmp_path("seek.mzr");
    write_test_mzf(mzf);
    gchar *one = NULL;
    gsize one_len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(mzf, &one, &one_len, NULL));
    GByteArray *two = g_byte_array_new();
    g_byte_array_append(two, (const guint8 *)one, (guint)one_len);
    g_byte_array_append(two, (const guint8 *)one, (guint)one_len);
    TEST_ASSERT_TRUE(g_file_set_contents(mzt, (const gchar *)two->data, (gssize)two->len, NULL));
    g_byte_array_free(two, TRUE);
    g_free(one);

    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzt));

    /* Živý běh: záznam, PLAY, 100 snímků (2 s) v bloku 0. */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    g_emulator.paused = false;
    for (int i = 0; i < 100; i++) next_frame();
    g_emulator.paused = true;
    int blk = -1;
    uint64_t pos = 0;
    tape_position(&blk, &pos);
    TEST_ASSERT_EQUAL_INT(0, blk);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)100 * VIDEO_SCREEN_TICKS, pos);

    /* MCP seek na blok 1 v pauze na hranici snímku. */
    st_DBGAPI_CMT_TAPE_SEEK_PARAM sp;
    memset(&sp, 0, sizeof(sp));
    sp.block_id = 1;
    st_DBGAPI_CMDRQ rq;
    memset(&rq, 0, sizeof(rq));
    rq.cmd = DBGAPI_CMD_CMT_TAPE_SEEK;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = &sp;
    dbgapi_emu_dispatch(&rq);
    TEST_ASSERT_TRUE(rq.success);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    tape_position(&blk, &pos);
    TEST_ASSERT_EQUAL_INT(1, blk);
    TEST_ASSERT_EQUAL_UINT64(0, pos);

    g_emulator.paused = false;
    for (int i = 0; i < 10; i++) next_frame();
    g_emulator.paused = true;
    int live_blk = -1;
    uint64_t live_pos = 0;
    tape_position(&live_blk, &live_pos);
    TEST_ASSERT_EQUAL_INT(1, live_blk);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)10 * VIDEO_SCREEN_TICKS, live_pos);
    for (int i = 0; i < 5; i++) next_frame();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    const st_REPLAY_EVENT *last = NULL;
    st_REPLAY_DATA *d = NULL;
    TEST_ASSERT_EQUAL_INT(2, cmt_events(mzr, &last, &d));
    TEST_ASSERT_EQUAL_STRING("seek", last->cmd);
    TEST_ASSERT_EQUAL_INT32(1, last->arg);
    replay_data_free(d);

    /* Přehrání: po 110 snímcích stejná pozice jako živě. */
    cmt_eject();
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    for (int i = 0; i < 110; i++) next_frame();
    replay_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.deviated, st.outcome_message);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    tape_position(&blk, &pos);
    TEST_ASSERT_EQUAL_INT(live_blk, blk);
    TEST_ASSERT_EQUAL_UINT64(live_pos, pos);
    for (int i = 0; i < 10; i++) next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    cmt_eject();
    g_remove(mzf);
    g_remove(mzt);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzt);
    g_free(mzr);
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */


/**
 * @brief Past hlavičky CMT hacku (OUT 01h) s HL = 2000h.
 * @return AF po pasti (CY = 1, A = 2: Break).
 */
static uint16_t hack_header(void)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, 0x2000);
    cmthack_load_file();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}


/**
 * @brief Past těla CMT hacku (OUT 02h) s HL = 3000h a BC = @p bc.
 * @param bc Délka. @return AF po pasti.
 */
static uint16_t hack_body(uint16_t bc)
{
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, 0x3000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, bc);
    cmthack_read_mzf_body();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}


/*
 * Scénář (b): hack přečetl jediný program z pásky (konec pásky),
 * pak začne záznam a LOAD při záznamu skončí jako bez pásky (headless
 * Break, zapsaný do záznamu). Přehrání musí LOAD skončit stejně: konec
 * pásky nese startovní snapshot. Dřív ho načtení startovního snapshotu
 * zahodilo a LOAD při přehrání vzal program z pásky.
 */
void test_cmthack_tape_end_replayed(void)
{
    char *mzf = tmp_path("h.mzf");
    char *mzr = tmp_path("hack.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    cmthack_reset();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
    TEST_ASSERT_EQUAL_HEX16(0, hack_header() & 0x01);
    TEST_ASSERT_EQUAL_HEX16(0, hack_body(64) & 0x01);

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    uint16_t af = hack_header();
    TEST_ASSERT_EQUAL_HEX16(0x0201, af & 0xFF01); /* Break: konec pásky */
    next_frame();
    next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    /* stav hacku mezi záznamem a přehráním jinak: program znovu z pásky */
    cmthack_reset();
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    af = hack_header();
    TEST_ASSERT_EQUAL_HEX16(0x0201, af & 0xFF01); /* jako při záznamu */
    next_frame();
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_FALSE(st.deviated);

    cmt_eject();
    cmthack_reset();
    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


/*
 * Snapshot uložený uživatelem během přehrání: páska v mechanice je kopie
 * v dočasném adresáři přehrání (po konci se smaže). Snapshot musí nést
 * cestu původního souboru, jinak ho po konci přehrání nejde načíst
 * s páskou (dřív "Snapshot tape file not found").
 */
void test_snapshot_during_playback_stores_original_tape(void)
{
    char *mzf = tmp_path("orig.mzf");
    char *mzr = tmp_path("snap.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    cmthack_reset();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    next_frame();
    next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();

    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE_MESSAGE(0 != strcmp(mzf, g_cmt.last_filename), "the replay inserted a copy of the tape");
    TEST_ASSERT_EQUAL_STRING(mzf, replay_media_cmt_snapshot_path(g_cmt.last_filename));
    uint8_t *buf = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("during replay", &buf, &size));
    next_frame();
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    /* po konci přehrání: mechanika bez pásky, snapshot pásku otevře z původní cesty */
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING(mzf, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING(mzf, replay_media_cmt_snapshot_path(g_cmt.last_filename));

    cmt_eject();
    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


/* ================================================================
 * Nahrávání do pásky v záznamu vstupů
 * ================================================================ */

/** @brief Tělo MZF, které "program" nahraje (64 B, deterministické). */
static uint8_t s_rec_body[64];

/** @brief Posune čas o @p t taktů; přes hranici snímku zavolá next_frame(). */
static void prog_advance(uint64_t t)
{
    while (t)
    {
        uint64_t room = VIDEO_SCREEN_TICKS - g_gdg.total_elapsed.ticks;
        if (t < room)
        {
            g_gdg.total_elapsed.ticks += (unsigned)t;
            return;
        };
        t -= room;
        g_gdg.total_elapsed.ticks = 0;
        next_frame();
    };
}

/** @brief Dokončí rozběhnutý snímek (čas na hranici, next_frame()). */
static void prog_finish_frame(void)
{
    if (g_gdg.total_elapsed.ticks) prog_advance(VIDEO_SCREEN_TICKS - g_gdg.total_elapsed.ticks);
}

/** @brief Počet jedničkových bitů (checksum ROM). */
static uint32_t rec_popcount(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint8_t b = d[i]; b; b >>= 1) c += b & 1u;
    return c;
}

/** @brief MZF pro mztape (typ 01h, jméno, tělo s_rec_body). */
static void rec_make_mzf(st_MZTAPE_MZF *m, const char *name)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < (int)sizeof(s_rec_body); i++) s_rec_body[i] = (uint8_t)((i * 53 + 7) & 0xFF);
    m->header[0x00] = 0x01;
    memset(&m->header[0x01], 0x0D, 17);
    memcpy(&m->header[0x01], name, strlen(name));
    m->header[0x12] = (uint8_t)sizeof(s_rec_body);
    m->header[0x15] = 0x12;
    m->header[0x17] = 0x12;
    m->body = s_rec_body;
    m->size = sizeof(s_rec_body);
    m->chkh = rec_popcount(m->header, sizeof(m->header));
    m->chkb = rec_popcount(s_rec_body, sizeof(s_rec_body));
}

/** @brief "Program" zapíše signál ROM SAVE přes cmt_write_data(), čas běží přes hranice snímků. */
static void prog_write_rom(st_MZTAPE_MZF *m)
{
    st_CMT_VSTREAM *vs = mztape_create_cmt_vstream_from_mztmzf(m, MZTAPE_FORMATSET_MZ800, CMTSPEED_1_1, GDGCLK_BASE);
    TEST_ASSERT_NOT_NULL(vs);
    uint64_t samples;
    int value, last = 0;
    cmt_vstream_read_reset(vs);
    while (cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS)
    {
        if (!samples) continue;
        cmt_write_data(value);
        prog_advance(samples);
        last = value;
    };
    cmt_write_data(last ^ 1);
    cmt_vstream_destroy(vs);
}

/** @brief Ověří, že blok vložené pásky nese MZF m (hlavička a tělo). */
static void check_block_mzf(int block_id, const st_MZTAPE_MZF *m)
{
    st_MZF *mzf = cmttmz_block_get_mzf(block_id);
    TEST_ASSERT_NOT_NULL(mzf);
    st_MZF_HEADER hdr;
    memcpy(&hdr, &mzf->header, sizeof(hdr));
    mzf_header_swap_endianity(&hdr);
    TEST_ASSERT_EQUAL_MEMORY(m->header, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_MEMORY(m->body, mzf->body, m->size);
    mzf_free(mzf);
}

/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_count(void)
{
    if (!CMT_TEST_FILLED) return -1;
    return cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext));
}

/** @brief Průběh "programu" testu: RECORD (p), motor, signál ROM, STOP (b), konec snímku. */
static void prog_record_session(st_MZTAPE_MZF *m, bool live)
{
    g_pio8255.signal_pc01 = 0;
    if (live)
    {
        replay_host_cmt(REPLAY_CMT_RECORD, 0, NULL);
        replay_on_paused();
        replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
        replay_on_paused();
    };
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    g_emulator.paused = false;
    prog_write_rom(m);
    prog_finish_frame();
    if (live) replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    g_emulator.paused = true;
}

/*
 * Záznam vstupů s nahráváním na kazetu (RECORD bez cesty,
 * program zapíše SAVE ROM, STOP) přehraný dvakrát dá pokaždé stejnou
 * pásku jako živý běh: 1 blok, stejné bajty MZF, Modified, bez cesty
 * (nahrávání bez cesty se přehraje bez cesty, ne do dočasného WAV).
 */
void test_cmt_record_replayed_same_tape(void)
{
    char *mzr = tmp_path("rec.mzr");
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "REPLAYED");
    cmt_eject();

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    prog_record_session(&m, true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
    check_block_mzf(0, &m);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    for (int run = 0; run < 2; run++)
    {
        cmt_eject();
        replay_request_play(mzr, true);
        replay_on_paused();
        replay_get_status(&st);
        TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
        prog_record_session(&m, false);
        replay_get_status(&st);
        TEST_ASSERT_FALSE_MESSAGE(st.deviated, st.outcome_message);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
        check_block_mzf(0, &m);
        TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
        TEST_ASSERT_EQUAL_STRING("", replay_media_cmt_snapshot_path(cmtext_container_get_filepath(cmtext_get_container(g_cmt.ext))));
        g_emulator.paused = false;
        for (int i = 0; i < 4; i++) next_frame();
        g_emulator.paused = true;
        replay_get_status(&st);
        TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    };

    cmt_eject();
    g_remove(mzr);
    g_free(mzr);
}

/*
 * V záznamu vstupů: neúspěšné otevření pásky během
 * nahrávání (chybějící a poškozený soubor) pásku ani nahrávání nezmění
 * a nepočítá se jako zápis na médium (checkpointy ho nemusí odmítat);
 * záznam se pak přehraje beze změny (bez odchýlení, stejný blok).
 * Otevření z MCP a z UI se při záznamu vstupů odmítá (FORBIDDEN, zásahy
 * fronty zařízení), do záznamu se tedy nikdy nedostane - test volá jádro
 * přímo.
 */
void test_cmt_failed_open_during_recording_keeps_tape(void)
{
    char *mzr = tmp_path("fo.mzr");
    char *bad = tmp_path("bad.tmz");
    char *missing = tmp_path("missing.mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(bad, "not a tape", -1, NULL));
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "KEPT");
    cmt_eject();

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    g_pio8255.signal_pc01 = 0;
    replay_host_cmt(REPLAY_CMT_RECORD, 0, NULL);
    replay_on_paused();
    replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    uint32_t writes = g_media_writes;
    device_state_lock();
    bool discarded = true;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_open_file(missing, &discarded));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_open_file(bad, &discarded));
    device_state_unlock();
    TEST_ASSERT_FALSE(discarded);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_UINT32(writes, g_media_writes);

    g_emulator.paused = false;
    prog_write_rom(&m);
    prog_finish_frame();
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
    check_block_mzf(0, &m);
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    cmt_eject();
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    prog_record_session(&m, false);
    replay_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.deviated, st.outcome_message);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
    check_block_mzf(0, &m);
    g_emulator.paused = false;
    for (int i = 0; i < 4; i++) next_frame();
    g_emulator.paused = true;
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    cmt_eject();
    g_remove(mzr);
    g_remove(bad);
    g_free(mzr);
    g_free(bad);
    g_free(missing);
}

/** @brief Hrany "programu" ve snímku f (definice níže). @param f Číslo snímku. */
static void prog_noise_frame(uint32_t f);

/* ================================================================
 * Checkpoint pásky, otisk stroje
 * ================================================================ */

/** @brief Čítač výsledků nahrávání (cmttmz_get_record_result()). */
static unsigned b6_rec_serial(void)
{
    unsigned s = 0;
    cmttmz_get_record_result(NULL, NULL, &s);
    return s;
}

/**
 * @brief Průběh "programu" jako prog_record_session(), s volitelnou cestou záznamu a novou páskou.
 * @param m MZF. @param live true = operace zadává test (záznam), false = přehrání.
 * @param path Cesta záznamu (NULL = jen do pásky). @param new_tape true = nejdřív nová páska.
 */
static void b6_session(st_MZTAPE_MZF *m, bool live, const char *path, bool new_tape)
{
    g_pio8255.signal_pc01 = 0;
    if (live)
    {
        if (new_tape)
        {
            replay_host_cmt(REPLAY_CMT_NEW_TAPE, 0, NULL);
            replay_on_paused();
        };
        replay_host_cmt(REPLAY_CMT_RECORD, 0, path);
        replay_on_paused();
        replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
        replay_on_paused();
    };
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    g_emulator.paused = false;
    prog_write_rom(m);
    prog_finish_frame();
    if (live) replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    g_emulator.paused = true;
}

/**
 * @brief Otisk částí media (s obsahem médií) a cmt.
 *
 * Počítadlo zápisů na média je globální pro proces (živý běh a přehrání
 * v jednom testu ho mají různé), proto se po dobu výpočtu nastaví na 0.
 *
 * @param[out] out Buffer aspoň 2 * STATE_HASH_HEX_LEN + 1 znaků.
 */
static void b6_hash(char *out)
{
    uint32_t mw = g_media_writes;
    g_media_writes = 0;
    char a[STATE_HASH_HEX_LEN + 1] = "", b[STATE_HASH_HEX_LEN + 1] = "";
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, a));
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_CMT, 0, b));
    g_media_writes = mw;
    g_snprintf(out, 2 * STATE_HASH_HEX_LEN + 1, "%s%s", a, b);
}

/*
 * Checkpoint bez vložené pásky (start záznamu) - návrat na něj po
 * nahrávání do nové pásky pásku vysune; opakované nahrávání dá zase jeden
 * blok. Přehrání: seek na snímek 0 a dohrání dá tutéž pásku.
 */
void test_cmt_rewind_before_new_tape_recording(void)
{
    char *mzr = tmp_path("c1.mzr");
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "C1");
    cmt_eject();
    /* signál ROM trvá stovky snímků: checkpoint snímku 0 musí v kruhu zůstat */
    st_REPLAY_CKPT_CFG cfg = { 50, 64 };
    replay_request_record_ex(mzr, 0, "c1", &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    prog_record_session(&m, true);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;

    replay_get_status(&st);
    unsigned serial = b6_rec_serial();
    replay_request_rewind(st.frame);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_UINT32(0, st.frame);
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_FILLED, "checkpoint without a tape must eject the recorded tape");
    TEST_ASSERT_EQUAL_UINT(serial, b6_rec_serial());

    prog_record_session(&m, true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), "re-recording after the rewind must give one block");
    check_block_mzf(0, &m);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    cmt_eject();
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    prog_record_session(&m, false);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    g_emulator.paused = false;
    next_frame();
    g_emulator.paused = true;
    replay_request_seek(0);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_UINT32(0, st.frame);
    /* snímek 0 přehrání: start (bez pásky), nebo už po RECORD (prázdná páska) */
    TEST_ASSERT_TRUE(!CMT_TEST_FILLED || tape_count() == 0);
    if (!CMT_TEST_RECORD) prog_record_session(&m, false);
    else
    {
        g_emulator.paused = false;
        prog_write_rom(&m);
        prog_finish_frame();
        next_frame();
        g_emulator.paused = true;
    };
    replay_get_status(&st);
    TEST_ASSERT_FALSE_MESSAGE(st.deviated, st.outcome_message);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    check_block_mzf(0, &m);

    replay_request_stop();
    replay_on_paused();
    cmt_eject();
    g_remove(mzr);
    g_free(mzr);
}

/*
 * Nahrávání s cestou pásky (.tmz) v záznamu vstupů - checkpoint
 * uprostřed nahrávání, STOP pásku uloží (cesta a Modified se změní), návrat
 * na checkpoint vrátí i cestu a Modified a nahrávání pokračuje bez
 * ohlášení zahození (čítač výsledků se nezmění).
 */
void test_cmt_rewind_into_recording_with_tape_path(void)
{
    char *mzr = tmp_path("i2.mzr");
    char *out = tmp_path("i2.tmz");
    cmt_eject();
    st_REPLAY_CKPT_CFG cfg = { 2, 20 };
    replay_request_record_ex(mzr, 0, "i2", &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    g_pio8255.signal_pc01 = 0;
    replay_host_cmt(REPLAY_CMT_RECORD, 0, out);
    replay_on_paused();
    replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    g_emulator.paused = false;
    for (uint32_t f = 1; f <= 10; f++) prog_noise_frame(f);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    next_frame();
    g_emulator.paused = true;
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_STRING(out, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    replay_get_status(&st);
    unsigned serial = b6_rec_serial();
    replay_request_rewind(st.frame - 6);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(st.frame >= 2 && st.frame <= 6);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", g_cmt.last_filename, "the tape path must go back with the checkpoint");
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(cmtext_get_container(g_cmt.ext)));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(serial, b6_rec_serial(), "a checkpoint restore must not report a discarded recording");

    /* dokončení: STOP pásku znovu uloží */
    g_emulator.paused = false;
    for (uint32_t f = st.frame + 1; f <= 10; f++) prog_noise_frame(f);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    next_frame();
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_STRING(out, g_cmt.last_filename);

    replay_request_stop();
    replay_on_paused();
    cmt_eject();
    g_remove(mzr);
    g_remove(out);
    g_free(mzr);
    g_free(out);
}

/** @brief Scénář otisku živě proti přehrání. */
typedef enum en_B6_HASH_CASE
{
    B6_HASH_NO_PATH = 0, /**< nahrávání bez cesty, bez pásky na startu */
    B6_HASH_WAV,         /**< nahrávání s cestou .wav */
    B6_HASH_TMZ,         /**< nahrávání s cestou .tmz (STOP pásku uloží) */
    B6_HASH_NEW_TAPE,    /**< páska ze souboru, v záznamu nová páska a nahrávání */
    B6_HASH_MODIFIED,    /**< páska ze souboru se změnami už na startu, nahrávání */
    B6_HASH_EMPTY_START, /**< nová prázdná páska (bez cesty, bez změn) už na startu, nahrávání */
    B6_HASH_COUNT        /**< počet scénářů */
} en_B6_HASH_CASE;

/** @brief Zapíše malý soubor MZF (pro pásku ze souboru) a vrátí cestu (g_free). */
static char *b6_tape_file(void)
{
    char *p = tmp_path("b6src.mzf");
    write_test_mzf(p);
    return p;
}

/*
 * Otisk stroje (část media s obsahem a část cmt) je po nahrávání
 * v živém běhu a v přehrání stejný: páska bez cesty, s cestou .wav,
 * s cestou .tmz, nová páska, páska Modified s cestou, prázdná páska bez
 * cesty už na startu (přehrání ji vkládá z dočasného souboru).
 */
void test_cmt_machine_hash_live_equals_replay(void)
{
    for (int k = 0; k < B6_HASH_COUNT; k++)
    {
        char *mzr = tmp_path("i3.mzr");
        char *wav = tmp_path("i3.wav");
        char *tmz = tmp_path("i3.tmz");
        char *src = b6_tape_file();
        const char *path = (k == B6_HASH_WAV) ? wav : (k == B6_HASH_TMZ) ? tmz : NULL;
        st_MZTAPE_MZF m;
        rec_make_mzf(&m, "HASH");
        cmt_eject();
        if (k == B6_HASH_NEW_TAPE || k == B6_HASH_MODIFIED)
        {
            TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(src));
        };
        if (k == B6_HASH_EMPTY_START)
        {
            TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
            TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
        };
        if (k == B6_HASH_MODIFIED)
        {
            /* změna pásky před startem záznamu (mimo záznam vstupů) */
            g_pio8255.signal_pc01 = 0;
            TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
            cmt_pause(0);
            g_emulator.paused = false;
            prog_write_rom(&m);
            prog_finish_frame();
            cmt_stop();
            g_emulator.paused = true;
            TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
        };

        replay_request_record(mzr, 0, NULL);
        replay_on_paused();
        st_REPLAY_STATUS st;
        replay_get_status(&st);
        TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
        b6_session(&m, true, path, k == B6_HASH_NEW_TAPE);
        char live[2 * STATE_HASH_HEX_LEN + 1];
        b6_hash(live);
        g_emulator.paused = false;
        for (int i = 0; i < 3; i++) next_frame();
        g_emulator.paused = true;
        replay_request_stop();
        replay_on_paused();
        replay_get_status(&st);
        TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

        replay_request_play(mzr, true);
        replay_on_paused();
        replay_get_status(&st);
        TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
        b6_session(&m, false, path, k == B6_HASH_NEW_TAPE);
        char played[2 * STATE_HASH_HEX_LEN + 1];
        b6_hash(played);
        char msg[64];
        g_snprintf(msg, sizeof(msg), "machine hash case %d", k);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(live, played, msg);
        g_emulator.paused = false;
        for (int i = 0; i < 4; i++) next_frame();
        g_emulator.paused = true;
        replay_get_status(&st);
        TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

        cmt_eject();
        g_remove(mzr);
        g_remove(wav);
        g_remove(tmz);
        g_remove(src);
        g_free(mzr);
        g_free(wav);
        g_free(tmz);
        g_free(src);
    };
}

/*
 * Checkpoint během nahrávání s bufferem nad mezí se vynechá - návrat
 * skočí na checkpoint před nahráváním (zde start bez pásky: páska pryč).
 */
void test_cmt_ckpt_skipped_over_record_limit(void)
{
    char *mzr = tmp_path("lim.mzr");
    cmt_eject();
    replay_test_set_ckpt_record_max(64);
    st_REPLAY_CKPT_CFG cfg = { 2, 20 };
    replay_request_record_ex(mzr, 0, "limit", &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_pio8255.signal_pc01 = 0;
    replay_host_cmt(REPLAY_CMT_RECORD, 0, NULL);
    replay_on_paused();
    replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
    replay_on_paused();
    g_emulator.paused = false;
    for (uint32_t f = 1; f <= 8; f++) prog_noise_frame(f);
    g_emulator.paused = true;
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);

    replay_request_rewind(0);
    replay_on_paused();
    replay_get_status(&st);
    replay_test_set_ckpt_record_max(0);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, st.frame, "checkpoints during the over-limit recording must be skipped");
    TEST_ASSERT_FALSE(CMT_TEST_RECORD);

    replay_request_stop();
    replay_on_paused();
    cmt_eject();
    g_remove(mzr);
    g_free(mzr);
}

/*
 * Starší záznam vstupů bez klíče rec_autostop_s v manifestu kazety
 * se přehraje s hodnotou 0; po konci přehrání se vrátí hodnota uživatele.
 * Takový záznam nemá ani klíč direct_save (= vznikl bez Direct Save,
 * bere se jako vypnuto): se zapnutým CMT hackem se přehraje jen
 * s vypnutým Direct Save, se zapnutým se odmítne.
 */
void test_cmt_old_manifest_without_autostop(void)
{
    unsigned saved_ds = g_cmthack.direct_save;
    unsigned saved_hack = g_cmthack.load_patch_installed;
    st_REPLAY_DATA *d = replay_data_new();
    d->media_known = true;
    d->cmt_json = g_strdup("{\"speed\": 1, \"polarity\": 0, \"block\": -1}");
    char err[256];
    const char *e;
    /* kontrola klíče direct_save platí jen se zapnutým hackem: zapnout */
    cmthack_load_rom_patch(1);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    cmthack_direct_save_set(1);
    e = replay_media_prepare(d, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "a record without direct_save (off) must be refused with Direct Save on");
    TEST_ASSERT_NOT_NULL(strstr(e, "Direct Save of the CMT hack was off when recording"));
    cmthack_direct_save_set(0);
    g_cmt.rec_autostop_s = 5;
    e = replay_media_prepare(d, err, sizeof(err));
    TEST_ASSERT_NULL_MESSAGE(e, e);
    TEST_ASSERT_EQUAL_UINT(0, g_cmt.rec_autostop_s);
    replay_media_restore();
    TEST_ASSERT_EQUAL_UINT(5, g_cmt.rec_autostop_s);
    g_cmt.rec_autostop_s = 0;
    replay_data_free(d);
    cmthack_direct_save_set(saved_ds);
    cmthack_load_rom_patch(saved_hack);
}

/** @brief Hrany "programu" ve snímku f: 60 hran s délkami podle f (deterministicky), pak konec snímku. */
static void prog_noise_frame(uint32_t f)
{
    for (int i = 0; i < 60; i++)
    {
        cmt_write_data(i & 1 ? 0 : 1);
        prog_advance(1000u + ((f * 60u + (uint32_t)i) * 37u) % 3000u);
    };
    prog_finish_frame();
}

/*
 * Rewind záznamu vstupů přes nahrávání: checkpoint během RECORD nese kopii
 * bufferu hran, návrat na něj pásku zkrátí o blok připojený STOPem
 * a obnoví nahrávání; opakovaný průběh dá stejný blok jako poprvé.
 */
void test_cmt_rewind_over_recording(void)
{
    char *mzr = tmp_path("rw.mzr");
    cmt_eject();
    st_REPLAY_CKPT_CFG cfg = { 2, 20 };
    replay_request_record_ex(mzr, 0, "tape rewind", &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    g_pio8255.signal_pc01 = 0;
    replay_host_cmt(REPLAY_CMT_RECORD, 0, NULL);
    replay_on_paused();
    replay_host_cmt(REPLAY_CMT_PAUSE, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    g_emulator.paused = false;
    for (uint32_t f = 1; f <= 10; f++) prog_noise_frame(f);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame(); /* STOP na hranici 11 */
    next_frame();
    g_emulator.paused = true;
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
    const st_TZX_FILE *f1 = cmttmz_get_file();
    uint32_t len = f1->blocks[0].length;
    uint8_t *data = g_memdup2(f1->blocks[0].data, len);

    /* zpět na checkpoint uprostřed nahrávání (snímek <= 6) */
    replay_get_status(&st);
    uint32_t frame_end = st.frame;
    replay_request_rewind(frame_end - 6);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(st.frame <= 6);
    TEST_ASSERT_TRUE(st.frame >= 2);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* stejný průběh od checkpointu: stejný blok */
    g_emulator.paused = false;
    for (uint32_t f = st.frame + 1; f <= 10; f++) prog_noise_frame(f);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    next_frame();
    next_frame();
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    const st_TZX_FILE *f2 = cmttmz_get_file();
    TEST_ASSERT_EQUAL_UINT32(len, f2->blocks[0].length);
    TEST_ASSERT_EQUAL_MEMORY(data, f2->blocks[0].data, len);
    g_free(data);

    replay_request_stop();
    replay_on_paused();
    cmt_eject();
    g_remove(mzr);
    g_free(mzr);
}


/* ================================================================
 * Záznam do pásky a záznam vstupů: další případy
 * ================================================================ */

/** @brief Zapíše krátký WAV (8 kHz mono 8 bit, 0,2 s obdélník, 0,1 s ticha). @param path Cesta. */
static void ff_write_wav(const char *path)
{
    const uint32_t rate = 8000, burst = rate / 5, frames = burst + rate / 10;
    const uint32_t size = 44 + frames;
    uint8_t *w = (uint8_t *)g_malloc0(size);
    uint32_t v32;
    uint16_t v16;
    memcpy(w, "RIFF", 4);
    v32 = size - 8; memcpy(w + 4, &v32, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    v32 = 16; memcpy(w + 16, &v32, 4);
    v16 = 1; memcpy(w + 20, &v16, 2);
    memcpy(w + 22, &v16, 2);
    memcpy(w + 24, &rate, 4);
    memcpy(w + 28, &rate, 4);
    memcpy(w + 32, &v16, 2);
    v16 = 8; memcpy(w + 34, &v16, 2);
    memcpy(w + 36, "data", 4);
    v32 = frames; memcpy(w + 40, &v32, 4);
    for (uint32_t i = 0; i < frames; i++)
        w[44 + i] = (i < burst) ? (((i / 4) & 1) ? 0x20 : 0xE0) : 0x80;
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)w, size, NULL));
    g_free(w);
}

/*
 * V záznamu vstupů: páska WAV s neuloženou změnou jde do záznamu jako
 * obraz; blok 0x18 z WAV má v manifestu příznak "wav" a přehrání ho vloží
 * jako blok WAV (přepínač polarity na něj působí jako při záznamu). Po
 * přehrání se vrátí páska uživatele (obraz) také s blokem WAV.
 */
void test_cmt_wav_origin_in_replay(void)
{
    char *wav = tmp_path("origin.wav");
    char *mzr = tmp_path("origin.mzr");
    ff_write_wav(wav);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(wav));
    cmttmz_set_modified(1); /* neuložená změna: do záznamu jako obraz */
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 0));

    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(mzr, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    TEST_ASSERT_NOT_NULL(d->cmt_json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(d->cmt_json, "\"wav\":true"), d->cmt_json);
    replay_data_free(d);

    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 0));
    g_emulator.paused = false;
    for (int i = 0; i < 4; i++) next_frame();
    g_emulator.paused = true;
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_FALSE(st.deviated);

    /* páska uživatele zpět z obrazu: blok WAV, Modified */
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    cmt_eject();
    g_remove(mzr);
    g_remove(wav);
    g_free(mzr);
    g_free(wav);
}

/*
 * Páska s neuloženými změnami větší než limit vložených médií
 * (pro test 16 B) by v záznamu měla jen hash a přehrát by nešla; start
 * záznamu se proto odmítne s jasnou chybou a nic nezmění. Páska ze
 * souboru beze změn nad limitem se zaznamená (hash, varování) jako dřív.
 */
void test_cmt_unsaved_tape_over_limit_refuses_recording(void)
{
    char *mzf = tmp_path("big.mzf");
    char *mzr = tmp_path("big.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
    cmttmz_set_modified(1);
    replay_media_test_set_embed_limit(16);

    /* odmítnutí přijde dřív, než se uloží a znovu načte startovní snapshot
     * (načtení snapshotu by připsalo zdroj revize STATE_REV_SRC_SNAPSHOT) */
    g_state_rev_src = 0;
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_FALSE(st.request_ok);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, g_state_rev_src & STATE_REV_SRC_SNAPSHOT, "no start snapshot was loaded");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(st.request_message, "Tape with unsaved changes is too large for an input recording; "
                                                            "save the tape first"), st.request_message);
    TEST_ASSERT_FALSE(replay_is_active());
    TEST_ASSERT_FALSE(g_file_test(mzr, G_FILE_TEST_EXISTS));
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    /* páska beze změn: soubor nad limitem jde do záznamu jako hash */
    cmttmz_set_modified(0);
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_request_stop();
    replay_on_paused();
    replay_media_test_set_embed_limit(0);

    cmt_eject();
    g_remove(mzr);
    g_remove(mzf);
    g_free(mzr);
    g_free(mzf);
}


/*
 * Ukončení během přehrávání: páska uživatele s neuloženými
 * změnami je po dobu přehrávání uschovaná; zastavení přehrávání (cesta,
 * kterou UI volá před dotazem na ukončení) ji vrátí vloženou a Modified,
 * takže dotaz Virtual CMT ji vidí (cmt_has_unsaved_changes()).
 */
void test_cmt_stop_playback_returns_stashed_modified_tape(void)
{
    char *mzf = tmp_path("stash.mzf");
    char *mzr = tmp_path("stash.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();

    /* páska uživatele s neuloženými změnami */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
    cmttmz_set_modified(1);
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_PLAYING, st.mode);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED); /* záznam začal bez pásky */

    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_TRUE(cmt_has_unsaved_changes());
    TEST_ASSERT_EQUAL_STRING(mzf, g_cmt.last_filename);

    cmt_eject();
    g_remove(mzr);
    g_remove(mzf);
    g_free(mzr);
    g_free(mzf);
}


/*
 * Páska uživatele s blokem vloženým úpravou (neuložený,
 * jeho data jinde nejsou) se po přehrání záznamu vstupů vrátí i s příznaky
 * neuloženo (obraz je nenese, uschovají se zvlášť).
 */
void test_cmt_stop_playback_returns_unsaved_flags(void)
{
    char *mzf = tmp_path("unsaved.mzf");
    char *mzr = tmp_path("unsaved.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();

    /* páska ze souboru (blok 0 uložený) a blok vložený na konec (neuložený) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
    st_MZF *m = cmttmz_block_get_mzf(0);
    TEST_ASSERT_NOT_NULL(m);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&m->header, m->body, m->header.fsize, CMTSPEED_1_1, NULL, 0);
    mzf_free(m);
    TEST_ASSERT_NOT_NULL(b);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(1, b, &sp, 1));
    tmz_block_free(b);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(1));

    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING(mzf, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext)));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmttmz_block_is_unsaved(1), "the inserted block is unsaved again");

    cmt_eject();
    g_remove(mzr);
    g_remove(mzf);
    g_free(mzr);
    g_free(mzf);
}


/*
 * Páska uživatele se změněnou rychlostí bloku se po
 * přehrání záznamu vstupů vrátí s rychlostí bloku i Modified jako před
 * přehráním - u pásky Modified (z obrazu) i u pásky Modified 0 (ze
 * souboru, rychlost např. ze snapshotu starší verze).
 */
void test_cmt_stop_playback_returns_block_speeds(void)
{
    char *mzf = tmp_path("speeds.mzf");
    char *mzr = tmp_path("speeds.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();

    for (int modified = 1; modified >= 0; modified--) {
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));
        TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
        TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
        if (!modified) cmttmz_set_modified(0);

        replay_request_play(mzr, true);
        replay_on_paused();
        replay_get_status(&st);
        TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
        replay_request_stop();
        replay_on_paused();
        replay_get_status(&st);
        TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
        TEST_ASSERT_TRUE(CMT_TEST_FILLED);
        st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0), "block speed mode back");
        TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 0), "block speed back");
        TEST_ASSERT_EQUAL_INT_MESSAGE(modified, cmttmz_is_modified(), "Modified as before the playback");
        TEST_ASSERT_EQUAL_UINT16(cmtspeed_get_bdspeed(CMTSPEED_3_1, 1200), g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
        cmt_eject();
    }

    g_remove(mzr);
    g_remove(mzf);
    g_free(mzr);
    g_free(mzf);
}



/** @brief Zapíše MZT s @p count bloky MZF (jména M0, M1, ...; tělo 64 B na 1200h, bloky DEFAULT rychlosti). */
static void write_test_mzt(const char *path, int count)
{
    GByteArray *ba = g_byte_array_new();
    for (int b = 0; b < count; b++)
    {
        uint8_t m[128 + 64];
        memset(m, 0, sizeof(m));
        m[0] = 0x01;
        memset(&m[1], 0x0D, 17);
        m[1] = 'M';
        m[2] = (uint8_t)('0' + b);
        m[0x12] = 64;
        m[0x15] = 0x12;
        m[0x17] = 0x12;
        for (int i = 0; i < 64; i++) m[128 + i] = (uint8_t)(i + b);
        g_byte_array_append(ba, m, sizeof(m));
    };
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)ba->data, ba->len, NULL));
    g_byte_array_free(ba, TRUE);
}


/** @brief Zapíše TAP se dvěma bloky (hlavička ZX a data). */
static void write_test_tap(const char *path)
{
    GByteArray *ba = g_byte_array_new();
    uint8_t hdr[19];
    memset(hdr, 0, sizeof(hdr));
    hdr[1] = 3;
    memcpy(&hdr[2], "TAPTEST   ", 10);
    hdr[12] = 4;
    hdr[15] = 0x80;
    uint8_t x = 0;
    for (int i = 0; i < 18; i++) x ^= hdr[i];
    hdr[18] = x;
    uint8_t data[6] = {0xFF, 1, 2, 3, 4, 0};
    x = 0;
    for (int i = 0; i < 5; i++) x ^= data[i];
    data[5] = x;
    const uint8_t l1[2] = {sizeof(hdr), 0}, l2[2] = {sizeof(data), 0};
    g_byte_array_append(ba, l1, 2);
    g_byte_array_append(ba, hdr, sizeof(hdr));
    g_byte_array_append(ba, l2, 2);
    g_byte_array_append(ba, data, sizeof(data));
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)ba->data, ba->len, NULL));
    g_byte_array_free(ba, TRUE);
}


/** @brief Nahraje krátký záznam vstupů bez pásky (3 snímky) do @p mzr. */
static void record_short_replay(const char *mzr)
{
    cmt_eject();
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
}


/**
 * @brief Přehraje @p mzr a hned ho zastaví (páska uživatele se vrátí).
 * @param mzr Záznam. @param during Volá se během přehrávání (NULL = nic).
 */
static void play_and_stop(const char *mzr, void (*during)(void))
{
    st_REPLAY_STATUS st;
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    if (during) during();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
}


/*
 * Po přehrání záznamu vstupů se vrátí rychlost každého
 * bloku: otevřený blok DEFAULT (obraz ho nese jako pevnou rychlost),
 * neotevřený blok s vlastními pulzy, neotevřený blok s pevnou rychlostí
 * a blok TAP.
 */
void test_wave_cmt_playback_returns_all_block_speeds(void)
{
    char *mzr = tmp_path("allspeeds.mzr");
    char *mzt = tmp_path("allspeeds.mzt");
    char *tap = tmp_path("allspeeds.tap");
    write_test_mzt(mzt, 3);
    write_test_tap(tap);
    record_short_replay(mzr);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzt));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(tap, 3, NULL));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(5, cmtext_container_get_count_blocks(c));
    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 400.0, 420.0, 200.0, 230.0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_CUSTOM, true, &p));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_block_cmt_speed(2, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(3, CMTSPEED_2_1, false, NULL));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0), "block 0 uses the deck speed");
    en_CMTEXT_BLOCK_SPEED mode[5];
    en_CMTSPEED speed[5];
    for (int i = 0; i < 5; i++)
    {
        mode[i] = cmtext_container_get_block_speed(c, i);
        speed[i] = cmtext_container_get_block_cmt_speed(c, i);
    };
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    play_and_stop(mzr, NULL);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(5, cmtext_container_get_count_blocks(c));
    for (int i = 0; i < 5; i++)
    {
        char msg[48];
        snprintf(msg, sizeof(msg), "block %d speed mode", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(mode[i], cmtext_container_get_block_speed(c, i), msg);
        snprintf(msg, sizeof(msg), "block %d speed", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(speed[i], cmtext_container_get_block_cmt_speed(c, i), msg);
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0), "DEFAULT is back, not a fixed speed");
    st_MZTAPE_PULSES_LENGTH q;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_get_block_pulses(c, 1, &q));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&p, &q, sizeof(p), "custom pulses of block 1 are back");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    cmt_eject();
    g_remove(mzr);
    g_remove(mzt);
    g_remove(tap);
    g_free(mzr);
    g_free(mzt);
    g_free(tap);
}


/** @brief Cesta pásky, kterou přepíše wave_overwrite_tape() během přehrání. */
static char *s_wave_tape = NULL;
/** @brief Soubor, jehož obsahem se páska během přehrání přepíše. */
static char *s_wave_other = NULL;


/** @brief Během přehrání: soubor pásky uživatele se na disku změní (stejný počet bloků, jiné typy). */
static void wave_overwrite_tape(void)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(s_wave_other, &data, &len, NULL));
    TEST_ASSERT_TRUE(g_file_set_contents(s_wave_tape, data, (gssize)len, NULL));
    g_free(data);
}


/*
 * Soubor pásky (Modified 0) se během přehrání změnil na
 * disku na pásku se stejným počtem bloků jiného typu - rychlosti se
 * nezapíšou na cizí bloky a okno dostane varování.
 */
void test_wave_cmt_playback_tape_file_changed(void)
{
    char *mzr = tmp_path("changed.mzr");
    char *mzt = tmp_path("changed.mzt");
    char *tap = tmp_path("changed.tap");
    s_wave_tape = tmp_path("changed_a.tmz");
    s_wave_other = tmp_path("changed_b.tmz");
    write_test_mzt(mzt, 2);
    write_test_tap(tap);
    record_short_replay(mzr);
    /* jiná páska se dvěma bloky TAP a její rychlosti po otevření */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(tap));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(s_wave_other));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_wave_other));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(c));
    en_CMTEXT_BLOCK_TYPE other_type = cmtext_container_get_block_type(c, 0);
    en_CMTEXT_BLOCK_SPEED other_mode = cmtext_container_get_block_speed(c, 0);
    en_CMTSPEED other_speed = cmtext_container_get_block_cmt_speed(c, 0);
    /* páska uživatele: soubor TMZ se dvěma bloky MZF, rychlost bloku 0 změněná, Modified 0 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzt));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(s_wave_tape));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_wave_tape));
    c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_NOT_EQUAL_INT(other_type, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    cmttmz_set_modified(0);
    unsigned w0 = 0;
    (void)replay_media_get_cmt_warning(&w0);

    play_and_stop(mzr, wave_overwrite_tape);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(other_type, cmtext_container_get_block_type(c, 0), "the changed file was opened");
    TEST_ASSERT_EQUAL_INT_MESSAGE(other_mode, cmtext_container_get_block_speed(c, 0), "no speed written onto another block");
    TEST_ASSERT_EQUAL_INT(other_speed, cmtext_container_get_block_cmt_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    unsigned w1 = 0;
    const char *w = replay_media_get_cmt_warning(&w1);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(w0 + 1u, w1, "one warning");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(w, "block speeds were not restored"), w);

    cmt_eject();
    g_remove(mzr);
    g_remove(mzt);
    g_remove(tap);
    g_remove(s_wave_tape);
    g_remove(s_wave_other);
    g_free(mzr);
    g_free(mzt);
    g_free(tap);
    g_free(s_wave_tape);
    g_free(s_wave_other);
    s_wave_tape = s_wave_other = NULL;
}



/*
 * Hypotéza: poslední blok je v okamžiku značky
 * checkpointu sestavený jinou rychlostí, než má hrát (rychlost změněná
 * v indexu u neotevřeného bloku). Po značce se blok přestaví (otevřením)
 * a připojení záznamu mu přidá pauzu; návrat na značku musí vrátit data
 * bloku i jeho záznam rychlosti (speed, pulses) do stavu ve značce, jinak
 * záznam rychlosti říká "sestaveno 2:1" nad daty 1:1 a blok se už
 * nepřestaví (přehrání záznamu vstupů ho přestaví = rozejití).
 */
void test_final_tape_mark_restores_last_block_speed(void)
{
    char *mzt = tmp_path("markspeed.mzt");
    write_test_mzt(mzt, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzt));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    /* rychlost neotevřeného posledního bloku jen v indexu: data zůstanou 1:1 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1, true, NULL));
    st_CMTTMZ_BLOCK_SPEED at_mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &at_mark, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_1_1, at_mark.speed, "precondition: the block is built at 1:1");
    const st_TZX_BLOCK *lb = &cmttmz_get_file()->blocks[1];
    uint32_t mark_len = lb->length;
    uint8_t *mark_data = (uint8_t *)g_memdup2(lb->data, mark_len);

    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));

    /* po značce: otevření bloku ho přestaví rychlostí 2:1, záznam připojí blok */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    st_CMTTMZ_BLOCK_SPEED after;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &after, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_2_1, after.speed, "precondition: opening rebuilt the block at 2:1");
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = 0x01;
    memset(&hdr.fname, 0x0D, sizeof(hdr.fname));
    memcpy(&hdr.fname, "REC", 3);
    hdr.fsize = 16;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[16];
    memset(body, 0x5A, sizeof(body));
    st_TZX_BLOCK *rb = cmttmz_build_mzf_block(&hdr, body, sizeof(body), CMTSPEED_1_1, NULL, 0);
    TEST_ASSERT_NOT_NULL(rb);
    st_CMTTMZ_BLOCK_SPEED rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    rsp.speed = CMTSPEED_1_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_append_blocks(rb, &rsp, 1));
    tmz_block_free(rb);
    TEST_ASSERT_EQUAL_INT(3, (int)cmttmz_get_file()->block_count);

    /* návrat na značku: data i záznam rychlosti jako ve značce */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    TEST_ASSERT_EQUAL_INT(2, (int)cmttmz_get_file()->block_count);
    lb = &cmttmz_get_file()->blocks[1];
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(mark_len, lb->length, "block data length as at the mark");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(mark_data, lb->data, mark_len, "block data as at the mark");
    st_CMTTMZ_BLOCK_SPEED back;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &back, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(at_mark.speed, back.speed, "speed record as at the mark (the data is built at it)");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&at_mark.pulses, &back.pulses, sizeof(back.pulses), "pulses as at the mark");
    TEST_ASSERT_EQUAL_UINT16(at_mark.pause_ms, back.pause_ms);
    /* blok hraje rychlostí z indexu: další otevření ho přestaví jako po značce při přehrání */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &back, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_2_1, back.speed, "the restored block is rebuilt when opened");
    g_free(mark_data);
    cmt_eject();
    g_free(mzt);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    replay_init();
    s_tmpdir = g_dir_make_tmp("replay_media_XXXXXX", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_media_recorded_and_played_on_copies);
    RUN_TEST(test_qdisk_buffer_mount);
    RUN_TEST(test_media_writes_counted_ide8);
    RUN_TEST(test_cmt_window_ops_recorded);
    RUN_TEST(test_cmt_guarded_op_dropped_not_recorded);
    RUN_TEST(test_cmt_guarded_drop_counted_not_in_playback);
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    RUN_TEST(test_cmt_mcp_seek_while_playing_replayed);
#endif
    RUN_TEST(test_cmthack_tape_end_replayed);
    RUN_TEST(test_snapshot_during_playback_stores_original_tape);
    RUN_TEST(test_cmt_record_replayed_same_tape);
    RUN_TEST(test_cmt_rewind_over_recording);
    RUN_TEST(test_cmt_failed_open_during_recording_keeps_tape);
    RUN_TEST(test_cmt_rewind_before_new_tape_recording);
    RUN_TEST(test_cmt_rewind_into_recording_with_tape_path);
    RUN_TEST(test_cmt_machine_hash_live_equals_replay);
    RUN_TEST(test_cmt_ckpt_skipped_over_record_limit);
    RUN_TEST(test_cmt_old_manifest_without_autostop);
    RUN_TEST(test_cmt_wav_origin_in_replay);
    RUN_TEST(test_cmt_unsaved_tape_over_limit_refuses_recording);
    RUN_TEST(test_cmt_stop_playback_returns_stashed_modified_tape);
    RUN_TEST(test_cmt_stop_playback_returns_unsaved_flags);
    RUN_TEST(test_cmt_stop_playback_returns_block_speeds);
    RUN_TEST(test_wave_cmt_playback_returns_all_block_speeds);
    RUN_TEST(test_wave_cmt_playback_tape_file_changed);
    RUN_TEST(test_final_tape_mark_restores_last_block_speed);
    int result = UNITY_END();

    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
