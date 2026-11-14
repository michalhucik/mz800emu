/**
 * @file test_cmt_tmz_edit.c
 * @brief Testy úprav vložené pásky Virtual CMT: mazání, přesun a vložení bloků, příznak neuloženo.
 *
 * Testuje se:
 *   - invarianty po každé úpravě: index kontejneru (block_id == offset ==
 *     pozice), count_blocks == počet bloků souboru, vedlejší pole na
 *     každý blok, stav playeru nad týmž souborem, neuložený blok =>
 *     Modified; čítače výměn pásky, otevření a změn vzrostou, Modified 1,
 *     otevřený blok se zavře a otevře znovu od začátku,
 *   - pořadí bloků po mazání, přesunu a vložení,
 *   - kurzor pásky (smazání aktuálního bloku, bloku před ním
 *     a za ním; přesun aktuálního; vložení před aktuální),
 *   - příznak neuloženo (vložený ano, přesunutý a mazáním
 *     dotčený beze změny, po uložení nikdo),
 *   - pauza na švu (vložení a přesun ano, mazání ne),
 *   - hlavička TMZ 1.1 po vložení bloku 0x46 do pásky ZXTape!,
 *   - hraniční případy (smazání všech bloků, přesun na vlastní místo, na
 *     první a poslední pozici, vložení do prázdné pásky, neplatné
 *     parametry = páska beze změny a text chyby),
 *   - odmítnutí úpravy mimo STOP, při nahrávání a při záznamu
 *     a přehrávání vstupů (cmt_tape_edit_allowed()), úklid stavu CMT hacku,
 *   - vložení na konec veřejnou funkcí je úprava
 *     (čítače, hack, pozice 0), připojení záznamu čísla nemění, vložení do
 *     prázdné pásky a PLAY, vložení 0 bloků, smazání nesousedních bloků
 *     kolem kurzoru, šev přesunu u bloku před cílem, úprava pásky bez
 *     otevřeného bloku, zneplatnění délek s blokem 0x43, otisk stroje
 *     z pásky v paměti,
 *   - řídicí bloky: páska se smyčkou a skupinou po smazání začátku a konce smyčky
 *     a po přesunu bloku ven ze smyčky dohraje bez pádu a zacyklení,
 *   - značka checkpointu vrátí příznaky neuloženo,
 *   - snapshot po úpravách (pořadí, příznaky neuloženo, kurzor) a snapshot
 *     bez příznaků (starší verze).
 *   - rychlost bloku = Modified, uložení přestaví blok; import
 *     souboru jako bloků (bez pásky, doprostřed, MZT před první blok, TMZ
 *     s řídicími bloky na konec, WAV s typem WAV, chyby, odmítnutí), export
 *     celé pásky do WAV (délka, smyčka, nepřehratelný blok, chyby, mez délky,
 *     atomický zápis), řídicí bloky se skoky a voláním po úpravách.
 *
 * Testovací pásky se generují do dočasného adresáře (g_get_tmp_dir).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/cmt/cmt_request.h"
#include "emulator/device_request.h"
#include "libs/mzcmt_fm/mzcmt_fm.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/machine_hash.h"
#include "emulator/replay.h"
#include "emulator/media_writes.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Délka těla testovacích MZF v bajtech. */
#define TEST_BODY_SIZE 64

/** @brief Dočasné soubory pásek (maže tearDown). */
static char *s_paths[2] = {NULL, NULL};

/** @brief Další dočasné soubory testů importu a exportu (extra_path(), maže tearDown). */
static char *s_extra[4] = {NULL, NULL, NULL, NULL};

/**
 * @brief Vrátí cestu dočasného souboru s danou příponou (slot 0 nebo 1).
 * @param slot Číslo slotu (0, 1). @param ext Přípona bez tečky.
 * @return Cesta (vlastní ji test, uvolní tearDown).
 */
static const char *tmp_path(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_edit_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    g_free(s_paths[slot]);
    s_paths[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_paths[slot];
}

/**
 * @brief Sestaví blok 0x46 (MZ800EMU-CMT, 1:1) s MZF daného jména.
 * @param name Jméno MZF (nejvýš 16 znaků). @param pause_ms Pauza za blokem v ms.
 * @return Blok (uvolnit tmz_block_free(), data po převzetí páskou NULL).
 */
static st_TZX_BLOCK *build_mzf_block(const char *name, uint16_t pause_ms)
{
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = 0x01;
    memset(&hdr.fname, 0x0D, sizeof(hdr.fname));
    memcpy(&hdr.fname, name, strlen(name));
    hdr.fsize = TEST_BODY_SIZE;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[TEST_BODY_SIZE];
    for (int i = 0; i < TEST_BODY_SIZE; i++)
        body[i] = (uint8_t)((i * 37 + 11 + name[0]) & 0xFF);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, TEST_BODY_SIZE, CMTSPEED_1_1, NULL, pause_ms);
    TEST_ASSERT_NOT_NULL(b);
    return b;
}

/**
 * @brief Přidá do souboru blok 0x46 s MZF daného jména.
 * @param f Soubor. @param name Jméno MZF. @param pause_ms Pauza za blokem v ms.
 */
static void add_mzf_block(st_TZX_FILE *f, const char *name, uint16_t pause_ms)
{
    st_TZX_BLOCK *b = build_mzf_block(name, pause_ms);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/**
 * @brief Přidá do souboru blok s daty (kopie), např. řídicí blok TZX.
 * @param f Soubor. @param id ID bloku. @param data Data bez bajtu ID (NULL jen při len 0). @param len Délka.
 */
static void add_raw_block(st_TZX_FILE *f, uint8_t id, const uint8_t *data, uint32_t len)
{
    st_TZX_BLOCK b;
    b.id = id;
    b.length = len;
    b.data = NULL;
    if (len) {
        b.data = (uint8_t *)malloc(len);
        TEST_ASSERT_NOT_NULL(b.data);
        memcpy(b.data, data, len);
    }
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Blok 0x30 Text Description. @param f Soubor. @param text Text. */
static void add_text_block(st_TZX_FILE *f, const char *text)
{
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description(text, &b));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Vytvoří prázdný soubor s hlavičkou TMZ. @return Soubor (uvolní save_tape()). */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/**
 * @brief Uloží soubor do slotu s danou příponou a uvolní ho.
 * @param f Soubor (funkce ho uvolní). @param slot Slot. @param ext Přípona ("tmz", "tzx").
 * @return Cesta uloženého souboru.
 */
static const char *save_tape(st_TZX_FILE *f, int slot, const char *ext)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    const char *path = tmp_path(slot, ext);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    return path;
}

/**
 * @brief Otevře pásku z bloků MZF se jmény "A", "B", ... (pauza 1000 ms, poslední 0).
 * @param n Počet bloků (1 až 8).
 */
static void open_letters(int n)
{
    st_TZX_FILE *f = new_tmz();
    for (int i = 0; i < n; i++) {
        char name[2] = {(char)('A' + i), 0};
        add_mzf_block(f, name, (i + 1 < n) ? 1000 : 0);
    }
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
}

/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_count(void)
{
    if (!CMT_TEST_FILLED) return -1;
    return cmtext_container_get_count_blocks(g_cmt.ext->container);
}

/**
 * @brief Popis pořadí bloků: jména MZF a "#ID" ostatních bloků, oddělené čárkou.
 * @param[out] out Buffer. @param size Velikost bufferu.
 * @return @p out.
 */
static const char *tape_names(char *out, size_t size)
{
    out[0] = '\0';
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    const st_TZX_FILE *f = cmttmz_get_file();
    size_t len = 0;
    for (int i = 0; c && f && i < c->count_blocks; i++) {
        int n;
        if (cmtext_container_get_block_type(c, i) == CMTEXT_BLOCK_TYPE_MZF)
            n = snprintf(out + len, size - len, "%s%s", i ? "," : "", cmtext_container_get_block_fname(c, i));
        else
            n = snprintf(out + len, size - len, "%s#%02X", i ? "," : "", f->blocks[i].id);
        if (n > 0 && (size_t)n < size - len) len += (size_t)n;
    }
    return out;
}

/** @brief Ověří pořadí bloků. @param expected Očekávaný popis (tape_names()). */
#define ASSERT_NAMES(expected)                                                                                         \
    do {                                                                                                               \
        char names_buf_[256];                                                                                          \
        TEST_ASSERT_EQUAL_STRING(expected, tape_names(names_buf_, sizeof(names_buf_)));                                \
    } while (0)

/** @brief Číslo otevřeného bloku (-1 bez bloku). */
static int open_block(void)
{
    if (!CMT_TEST_FILLED || !g_cmt.ext->block) return -1;
    return cmtext_block_get_block_id(g_cmt.ext->block);
}

/**
 * @brief Ověří invarianty pásky (index, počty, vedlejší pole, otevřený blok, player, neuloženo).
 */
static void check_invariants(void)
{
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    const st_TZX_FILE *f = cmttmz_get_file();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT((int)f->block_count, c->count_blocks);
    int any_unsaved = 0;
    for (int i = 0; i < c->count_blocks; i++) {
        TEST_ASSERT_EQUAL_INT(i, c->tape->index[i].block_id);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, c->tape->index[i].offset);
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(i, NULL, NULL));
        st_CMTTMZ_BLOCK_UI ui;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_block_ui(i, &ui));
        TEST_ASSERT_EQUAL_INT(ui.unsaved, cmttmz_block_is_unsaved(i));
        if (ui.unsaved) any_unsaved = 1;
    }
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_test_get_side(c->count_blocks, NULL, NULL));
    if (any_unsaved) TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    if (g_cmt.ext->block) {
        TEST_ASSERT_TRUE(g_cmt.ext->block->block_id >= 0 && g_cmt.ext->block->block_id < c->count_blocks);
        TEST_ASSERT_EQUAL_INT(g_cmt.ext->block->block_id, cmttmz_get_open_block_id());
    } else {
        TEST_ASSERT_EQUAL_INT(-1, cmttmz_get_open_block_id());
    }
    st_TMZ_PLAYER_STATE st;
    if (EXIT_SUCCESS == cmttmz_get_player_state(&st)) {
        TEST_ASSERT_TRUE(st.file == f);
        TEST_ASSERT_TRUE(st.current_block <= f->block_count);
    }
}

/** @brief Čítače pásky pro porovnání před a po úpravě. */
typedef struct {
    unsigned tape;   /**< cmttmz_get_tape_serial() */
    unsigned open;   /**< cmttmz_get_open_serial() */
    unsigned change; /**< cmttmz_get_change_serial() */
} st_SERIALS;

/** @brief Přečte čítače pásky. @return Čítače. */
static st_SERIALS serials(void)
{
    st_SERIALS s = {cmttmz_get_tape_serial(), cmttmz_get_open_serial(), cmttmz_get_change_serial()};
    return s;
}

/** @brief Ověří, že všechny tři čítače vzrostly. @param s Čítače před úpravou. */
static void assert_serials_grew(st_SERIALS s)
{
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(s.tape, cmttmz_get_tape_serial(), "tape serial");
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(s.open, cmttmz_get_open_serial(), "open serial");
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(s.change, cmttmz_get_change_serial(), "change serial");
}

/** @brief Ověří, že se žádný čítač nezměnil. @param s Čítače před voláním. */
static void assert_serials_same(st_SERIALS s)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.tape, cmttmz_get_tape_serial(), "tape serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.open, cmttmz_get_open_serial(), "open serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.change, cmttmz_get_change_serial(), "change serial");
}

/** @brief Pauza bloku ve vedlejším poli rychlostí. @param id Blok. @return Pauza v ms. */
static uint16_t block_pause(int id)
{
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(id, &sp, NULL));
    return sp.pause_ms;
}

/**
 * @brief Pozice pásky ve STOP (blok a takty), ověří ji.
 * @param block Očekávaný blok. @param ticks Očekávaná pozice.
 */
static void assert_position(int block, uint64_t ticks)
{
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&b, &t));
    TEST_ASSERT_EQUAL_INT(block, b);
    TEST_ASSERT_EQUAL_UINT64(ticks, t);
}

/**
 * @brief Vloží blok MZF daného jména core funkcí cmttmz_insert_blocks().
 * @param at Pozice. @param name Jméno. @param pause_ms Pauza.
 * @return Výsledek cmttmz_insert_blocks().
 */
static int insert_mzf(int at, const char *name, uint16_t pause_ms)
{
    st_TZX_BLOCK *b = build_mzf_block(name, pause_ms);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    sp.pause_ms = pause_ms;
    int rc = cmttmz_insert_blocks(at, b, &sp, 1);
    if (rc == EXIT_SUCCESS) TEST_ASSERT_NULL_MESSAGE(b->data, "data are owned by the tape after success");
    tmz_block_free(b);
    return rc;
}

/** @brief Hodnota g_cmt.autorewind před testem. */
static int s_saved_autorewind;
/** @brief Výchozí rychlost Virtual CMT před testem. */
static en_CMTSPEED s_saved_speed;
/** @brief Příznak nainstalovaného CMT hacku před testem. */
static unsigned s_saved_cmthack;

void setUp(void)
{
    g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
    g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
    s_saved_autorewind = g_cmt.autorewind;
    g_cmt.autorewind = 0;
    s_saved_cmthack = g_cmthack.load_patch_installed;
    s_saved_speed = g_cmt.mz_cmtspeed;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
}

void tearDown(void)
{
    g_cmt.autorewind = s_saved_autorewind;
    g_cmthack.load_patch_installed = s_saved_cmthack;
    g_emulator.paused = false;
    snap_cmt_set_keep_inserted_tape(false);
    snap_cmt_test_omit_unsaved(false);
    cmt_eject();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    for (int i = 0; i < 2; i++) {
        if (s_paths[i]) {
            g_remove(s_paths[i]);
            g_free(s_paths[i]);
            s_paths[i] = NULL;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (s_extra[i]) {
            g_remove(s_extra[i]);
            g_free(s_extra[i]);
            s_extra[i] = NULL;
        }
    }
    cmttmz_test_set_export_limit(0);
    cmttmz_test_fail_before_replace(0);
    g_replay_mode = REPLAY_MODE_OFF;
}

/* ================================================================
 * MAZÁNÍ
 * ================================================================ */

/*
 * Smazání bloku za aktuálním: invarianty, pořadí, čítače, Modified,
 * otevřený blok zavřený a otevřený znovu od začátku (kurzor
 * sleduje aktuální blok, čas v bloku 0).
 */
void test_remove_block_after_current(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(4);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 12345));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    st_SERIALS s = serials();
    const int ids[] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    check_invariants();
    ASSERT_NAMES("A,B,D");
    assert_serials_grew(s);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    assert_position(1, 0);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_error());
}

/* Smazání aktuálního bloku: kurzor na bloku, který zaujal jeho místo; u posledního na novém posledním. */
void test_remove_current_block_cursor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(4);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    const int b[] = {1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(b, 1));
    check_invariants();
    ASSERT_NAMES("A,C,D");
    TEST_ASSERT_EQUAL_INT(1, open_block()); /* C */

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(2));
    const int last[] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(last, 1));
    check_invariants();
    ASSERT_NAMES("A,C");
    TEST_ASSERT_EQUAL_INT(1, open_block()); /* nový poslední */
}

/* Smazání bloku před aktuálním: kurzor sleduje aktuální blok (číslo o jedna menší). */
void test_remove_block_before_current(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(4);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(2));
    const int ids[] = {0, 1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 2));
    check_invariants();
    ASSERT_NAMES("C,D");
    TEST_ASSERT_EQUAL_INT(0, open_block());
}

/*
 * Smazání aktuálního bloku, jehož místo zaujme řídicí blok (otevřít nejde):
 * kurzor na nejbližším dalším bloku se signálem.
 */
void test_remove_current_block_control_follows(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 1000);
    add_text_block(f, "text");
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    const int ids[] = {1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    check_invariants();
    ASSERT_NAMES("A,#30,C");
    TEST_ASSERT_EQUAL_INT(2, open_block());
}

/* Smazání všech bloků: prázdná páska, žádný otevřený blok, PLAY nic; pak vložení do prázdné pásky. */
void test_remove_all_blocks_then_insert(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    const int ids[] = {0, 1, 2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 3));
    check_invariants();
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_EQUAL_INT(-1, open_block());
    st_CMTTMZ_BLOCK_UI ui;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_get_block_ui(0, &ui));
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(0, "NEW", 0));
    check_invariants();
    ASSERT_NAMES("NEW");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(0));
}

/* Neplatné parametry mazání: chyba, text chyby, páska beze změny (ani Modified, ani čítače). */
void test_remove_invalid_arguments(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    st_SERIALS s = serials();
    const int unsorted[] = {2, 1};
    const int dup[] = {1, 1};
    const int out[] = {0, 3};
    const int neg[] = {-1};
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(unsorted, 2));
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(dup, 2));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(out, 2));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(neg, 1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(NULL, 1));
    ASSERT_NAMES("A,B,C");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    /* nula bloků = úspěch beze změny */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    check_invariants();
}

/* ================================================================
 * PŘESUN
 * ================================================================ */

/* Přesun na první a za poslední pozici, zpět, kurzor sleduje aktuální blok. */
void test_move_block_first_last_and_cursor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(4);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 777));
    st_SERIALS s = serials();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 3));
    check_invariants();
    ASSERT_NAMES("B,C,D,A");
    assert_serials_grew(s);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    assert_position(3, 0); /* aktuální blok A přesunutý, čas v bloku 0 */

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 0));
    check_invariants();
    ASSERT_NAMES("A,B,C,D");
    TEST_ASSERT_EQUAL_INT(0, open_block());

    /* přesun jiného bloku přes aktuální */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(2));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 3));
    check_invariants();
    ASSERT_NAMES("B,C,D,A");
    TEST_ASSERT_EQUAL_INT(1, open_block()); /* C */
}

/* Přesun na vlastní místo: úspěch beze změny (ani Modified, ani čítače); mimo rozsah chyba. */
void test_move_block_to_itself_and_out_of_range(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    st_SERIALS s = serials();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(1, 3));
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(3, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(-1, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(0, -1));
    ASSERT_NAMES("A,B,C");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    check_invariants();
}

/* ================================================================
 * VLOŽENÍ
 * ================================================================ */

/*
 * Vložení dvou bloků před aktuální blok: pořadí, kurzor sleduje aktuální
 * blok, vložené bloky neuložené (ne "vznikl záznamem"), ostatní uložené,
 * čítače, Modified; pozice mimo rozsah = chyba a data zůstanou volajícímu.
 */
void test_insert_blocks_before_current(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    st_SERIALS s = serials();
    st_TZX_BLOCK *x = build_mzf_block("X", 1000);
    st_TZX_BLOCK *y = build_mzf_block("Y", 1000);
    st_TZX_BLOCK blocks[2] = {*x, *y};
    st_CMTTMZ_BLOCK_SPEED sp[2];
    memset(sp, 0, sizeof(sp));
    for (int i = 0; i < 2; i++) {
        sp[i].origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
        sp[i].speed = CMTSPEED_1_1;
        sp[i].pause_ms = 1000;
    }
    /* mimo rozsah: chyba, data dál patří volajícímu */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_insert_blocks(4, blocks, sp, 2));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_insert_blocks(-1, blocks, sp, 2));
    TEST_ASSERT_NOT_NULL(blocks[0].data);
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');
    assert_serials_same(s);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(1, blocks, sp, 2));
    TEST_ASSERT_NULL(blocks[0].data);
    TEST_ASSERT_NULL(blocks[1].data);
    x->data = NULL;
    y->data = NULL;
    tmz_block_free(x);
    tmz_block_free(y);
    check_invariants();
    ASSERT_NAMES("A,X,Y,B,C");
    assert_serials_grew(s);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(3, open_block()); /* B */
    const int unsaved[] = {0, 1, 1, 0, 0};
    for (int i = 0; i < 5; i++) {
        st_CMTTMZ_BLOCK_UI ui;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_block_ui(i, &ui));
        TEST_ASSERT_EQUAL_INT_MESSAGE(unsaved[i], ui.unsaved, "unsaved flag");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui.recorded, "inserted block is not recorded");
    }
}

/*
 * Vložení na konec veřejnou cmttmz_insert_blocks() je úprava:
 * čítač výměn pásky vzroste, stav CMT hacku se smaže, kurzor zůstane
 * na původním bloku s pozicí 0.
 */
void test_insert_at_end_is_edit(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 4321));
    st_CMTHACK_TAPE_STATE hs;
    memset(&hs, 0, sizeof(hs));
    hs.pending = true;
    hs.pending_block = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&hs));
    st_SERIALS s = serials();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(2, "END", 0));
    check_invariants();
    ASSERT_NAMES("A,B,END");
    assert_serials_grew(s);
    assert_position(1, 0);
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&hs), "insert clears the CMT hack tape state");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1)); /* šev */
}

/*
 * Připojení záznamu (cmttmz_append_blocks()) jako jediné čísla nemění:
 * čítač výměn pásky, otevřený blok i pozice zůstanou, stav CMT hacku
 * a text chyby úpravy také.
 */
void test_append_keeps_numbers(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 4321));
    cmttmz_set_edit_error("previous error");
    st_CMTHACK_TAPE_STATE hs;
    memset(&hs, 0, sizeof(hs));
    hs.pending = true;
    hs.pending_block = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&hs));
    unsigned ts = cmttmz_get_tape_serial();
    st_TZX_BLOCK *b = build_mzf_block("REC", 0);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_append_blocks(b, &sp, 1));
    tmz_block_free(b);
    check_invariants();
    ASSERT_NAMES("A,B,REC");
    TEST_ASSERT_EQUAL_UINT(ts, cmttmz_get_tape_serial());
    assert_position(1, 4321);
    TEST_ASSERT_TRUE_MESSAGE(cmthack_tape_state_get(&hs), "recording keeps the CMT hack tape state");
    TEST_ASSERT_EQUAL_STRING("previous error", cmttmz_get_edit_error());
    st_CMTTMZ_BLOCK_UI ui;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_block_ui(2, &ui));
    TEST_ASSERT_EQUAL_INT(1, ui.recorded);
    TEST_ASSERT_EQUAL_INT(1, ui.unsaved);
}

/* Vložení do nové prázdné pásky: blok 0 je otevřený a PLAY ho přehraje. */
void test_insert_into_empty_tape_then_play(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(-1, open_block());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(0, "FIRST", 0));
    check_invariants();
    TEST_ASSERT_EQUAL_INT(0, open_block());
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    cmt_stop();
}

/* Vložení 0 bloků: úspěch beze změny (ani Modified, ani čítače). */
void test_insert_zero_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    st_SERIALS s = serials();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(1, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    ASSERT_NAMES("A,B");
}

/* Smazání nesousedních bloků s kurzorem mezi nimi: kurzor sleduje aktuální blok. */
void test_remove_non_adjacent_around_cursor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(4);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    const int ids[] = {0, 2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 2));
    check_invariants();
    ASSERT_NAMES("B,D");
    TEST_ASSERT_EQUAL_INT(0, open_block());
}

/* Šev u přesunu: blok před novou pozicí (to - 1) s pauzou 0 dostane mezeru, jiné bloky ne. */
void test_move_seam_before_target(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 2));
    ASSERT_NAMES("B,C,A");
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(0));    /* B není u švu */
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1)); /* C před přesunutým A */
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(2)); /* A poslední, pauza z importu */
    check_invariants();
}

/* Úprava pásky bez otevřeného bloku (jen řídicí bloky): vložený blok se otevře převinutím. */
void test_edit_without_open_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "one");
    add_text_block(f, "two");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_INT(-1, open_block());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(1, "MID", 0));
    check_invariants();
    ASSERT_NAMES("#30,MID,#30");
    TEST_ASSERT_EQUAL_INT(1, open_block());
    /* mazání vloženého bloku: zase bez otevřeného bloku */
    const int ids[] = {1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    check_invariants();
    TEST_ASSERT_EQUAL_INT(-1, open_block());
}

/** @brief Stav délky bloku pro UI. @param id Blok. @return en_CMTTMZ_LENGTH_STATE. */
static en_CMTTMZ_LENGTH_STATE length_state(int id)
{
    st_CMTTMZ_BLOCK_UI ui;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_block_ui(id, &ui));
    return ui.length_state;
}

/*
 * Blok 0x43 (takt CPU pro bloky za ním): úprava s posunem čísel zneplatní
 * všechny známé délky (kromě znovu otevřeného bloku); bez bloku 0x43 délka
 * přesunutého bloku platí dál.
 */
void test_clock_block_invalidates_lengths(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    for (int with_clock = 0; with_clock < 2; with_clock++) {
        st_TZX_FILE *f = new_tmz();
        if (with_clock) {
            st_TZX_BLOCK *mi = tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ800, 3546895, 0);
            TEST_ASSERT_NOT_NULL(mi);
            TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, mi));
            free(mi);
        }
        add_mzf_block(f, "A", 1000);
        add_mzf_block(f, "B", 1000);
        add_mzf_block(f, "C", 0);
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
        int a = with_clock ? 1 : 0;
        cmttmz_lengths_update_budget(INT64_MAX / 2);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_LENGTH_KNOWN, length_state(a + 1));
        TEST_ASSERT_EQUAL_INT(CMTTMZ_LENGTH_KNOWN, length_state(a + 2));
        /* C před B: B se posune, data se nemění */
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(a + 2, a + 1));
        ASSERT_NAMES(with_clock ? "#43,A,C,B" : "A,C,B");
        TEST_ASSERT_EQUAL_INT_MESSAGE(with_clock ? CMTTMZ_LENGTH_UNKNOWN : CMTTMZ_LENGTH_KNOWN, length_state(a + 2),
                                      with_clock ? "with a 0x43 block the length is invalid" : "without 0x43 the length stays");
        TEST_ASSERT_EQUAL_INT(CMTTMZ_LENGTH_KNOWN, length_state(a)); /* znovu otevřený blok A */
        cmt_eject();
    }
}

/*
 * Otisk stroje (část media s obsahem) se počítá z pásky
 * v paměti - po přesunu bloku je jiný; úprava při záznamu a přehrávání
 * vstupů je odmítnutá.
 */
void test_edit_machine_hash_and_input_replay(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    char h0[STATE_HASH_HEX_LEN + 1], h1[STATE_HASH_HEX_LEN + 1], h2[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, h0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 2));
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, h1));
    TEST_ASSERT_TRUE(strcmp(h0, h1) != 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(2, 0));
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, h2));
    TEST_ASSERT_TRUE_MESSAGE(strcmp(h1, h2) != 0, "the hash follows the tape content in memory");

    for (int mode = 0; mode < 2; mode++) {
        g_replay_mode = mode ? REPLAY_MODE_PLAYING : REPLAY_MODE_RECORDING;
        TEST_ASSERT_EQUAL_INT(0, cmt_tape_edit_allowed());
        const int ids[] = {0};
        TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(ids, 1));
        TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "input replay"));
        TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(0, 1));
        g_replay_mode = REPLAY_MODE_OFF;
    }
    ASSERT_NAMES("A,B,C");
    TEST_ASSERT_EQUAL_INT(1, cmt_tape_edit_allowed());
}

/* ================================================================
 * PŘÍZNAK NEULOŽENO, PAUZA NA ŠVU, HLAVIČKA
 * ================================================================ */

/*
 * Přesun a mazání příznaky neuloženo nemění (jen Modified), uložení
 * a cmttmz_set_modified(0) je vynulují.
 */
void test_unsaved_flags_follow_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(1, "X", 1000));
    ASSERT_NAMES("A,X,B,C");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 3));
    ASSERT_NAMES("A,B,C,X");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(3));
    const int ids[] = {0};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    ASSERT_NAMES("B,C,X");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(2));
    check_invariants();

    /* přesun a mazání uložených bloků: Modified, nikdo neuložený */
    cmttmz_set_modified(0);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(i));

    /* uložení vynuluje příznaky */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(0, "Y", 1000));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tmp_path(1, "tmz")));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(i));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(4));  /* mimo rozsah */
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(-1)); /* mimo rozsah */
}

/*
 * Blok 0x46 s pauzou 0, za který se vloží nebo přesune blok, dostane
 * pauzu 1000 ms (i vložený / přesunutý blok, za kterým další blok
 * následuje); mazání pauzy nemění.
 */
void test_seam_pause_rule(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 0);
    add_mzf_block(f, "B", 0);
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(0));
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(1));

    /* mazání: B mezi A a C zmizí, A zůstane s pauzou 0 */
    const int ids[] = {1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_remove_blocks(ids, 1));
    ASSERT_NAMES("A,C");
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(0));
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(1));

    /* vložení mezi A a C: A i vložený X dostanou mezeru, C (poslední) ne */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(1, "X", 0));
    ASSERT_NAMES("A,X,C");
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(0));
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1));
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(2));

    /* přesun posledního C (pauza 0) na začátek: C dostane mezeru */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_move_block(2, 0));
    ASSERT_NAMES("C,A,X");
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(0));
    check_invariants();
}

/* Vložení bloku 0x46 do pásky ZXTape! dá hlavičku TMZ 1.1. */
void test_insert_mz_block_into_zxtape_makes_tmz(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    memcpy(f->header.signature, TZX_SIGNATURE, TZX_SIGNATURE_LENGTH);
    f->header.ver_major = 1;
    f->header.ver_minor = 20;
    f->is_tmz = false;
    add_text_block(f, "zx tape");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tzx")));
    TEST_ASSERT_FALSE(cmttmz_get_file()->is_tmz);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(0, "MZ", 0));
    const st_TZX_FILE *t = cmttmz_get_file();
    TEST_ASSERT_TRUE(t->is_tmz);
    TEST_ASSERT_EQUAL_MEMORY("TapeMZ!", t->header.signature, 7);
    TEST_ASSERT_EQUAL_UINT8(1, t->header.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, t->header.ver_minor);
    ASSERT_NAMES("MZ,#30");
    check_invariants();
}

/* ================================================================
 * ODMÍTNUTÍ MIMO STOP, PŘI NAHRÁVÁNÍ; CMT HACK
 * ================================================================ */

/* Úprava jen ve STOP s vloženou páskou a bez nahrávání; jinak chyba s textem a páska beze změny. */
void test_edit_refused_outside_stop_and_recording(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_edit_allowed()); /* bez pásky */
    const int ids[] = {0};
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(ids, 1));
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');

    open_letters(3);
    TEST_ASSERT_EQUAL_INT(1, cmt_tape_edit_allowed());
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_edit_allowed());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_remove_blocks(ids, 1));
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(0, 2));
    ASSERT_NAMES("A,B,C");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    cmt_stop();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_edit_allowed());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(0, 2));
    ASSERT_NAMES("A,B,C");
    cmt_stop();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, cmt_tape_edit_allowed());
}

/* Úprava smaže stav CMT hacku nad páskou (hlavička načtená bez těla). */
void test_edit_clears_cmthack_tape_state(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    st_CMTHACK_TAPE_STATE hs;
    memset(&hs, 0, sizeof(hs));
    hs.pending = true;
    hs.pending_block = 0;
    hs.pending_moved = false;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&hs));
    TEST_ASSERT_TRUE(cmthack_tape_state_get(&hs));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(2, 1));
    TEST_ASSERT_FALSE(cmthack_tape_state_get(&hs));
}

/* ================================================================
 * ŘÍDICÍ BLOKY
 * ================================================================ */

/**
 * @brief Otevře pásku A, Group Start, Loop Start (2x), B, Loop End, Group End, C.
 *
 * Indexy: 0 A, 1 #21, 2 #24, 3 B, 4 #25, 5 #22, 6 C.
 */
static void open_loop_group_tape(void)
{
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t grp[] = {1, 'G'};
    add_raw_block(f, TZX_BLOCK_ID_GROUP_START, grp, sizeof(grp));
    const uint8_t loop[] = {2, 0};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, loop, sizeof(loop));
    add_mzf_block(f, "B", 1000);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    add_raw_block(f, TZX_BLOCK_ID_GROUP_END, NULL, 0);
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    ASSERT_NAMES("A,#21,#24,B,#25,#22,C");
}

/**
 * @brief Přehraje celou pásku postupným přehráváním (převinutí + cmttmz_advance()) s mezí kroků.
 * @param[out] seq Čísla otevřených bloků v pořadí (nejvýš @p max).
 * @param max Mez kroků.
 * @return Počet otevřených bloků; -1 = mez vyčerpána (zacyklení).
 */
static int play_whole_tape(int *seq, int max)
{
    if (EXIT_SUCCESS != cmttmz_rewind()) return 0;
    int n = 0;
    seq[n++] = open_block();
    while (n < max) {
        en_CMTTMZ_ADVANCE r = cmttmz_advance();
        if (r == CMTTMZ_ADVANCE_END || r == CMTTMZ_ADVANCE_ERROR) return n;
        seq[n++] = open_block();
    }
    return -1;
}

/** @brief Ověří, že páska dohraje v mezi a dá očekávanou posloupnost jmen. @param expected Jména bloků. */
static void assert_plays(const char *expected)
{
    int seq[64];
    int n = play_whole_tape(seq, 64);
    TEST_ASSERT_TRUE_MESSAGE(n >= 0, "playback did not finish within the step limit");
    char got[256];
    size_t len = 0;
    got[0] = '\0';
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    for (int i = 0; i < n; i++) {
        int k = snprintf(got + len, sizeof(got) - len, "%s%s", i ? "," : "", cmtext_container_get_block_fname(c, seq[i]));
        if (k > 0 && (size_t)k < sizeof(got) - len) len += (size_t)k;
    }
    TEST_ASSERT_EQUAL_STRING(expected, got);
    check_invariants();
}

/* Smazání začátku smyčky (Loop End bez Loop Start se přeskočí): B jednou. */
void test_z5_remove_loop_start(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_loop_group_tape();
    assert_plays("A,B,B,C");
    const int ids[] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    assert_plays("A,B,C");
}

/* Smazání konce smyčky (Loop Start bez Loop End): páska dohraje. */
void test_z5_remove_loop_end(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_loop_group_tape();
    const int ids[] = {4};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    assert_plays("A,B,C");
}

/* Přesun bloku ven ze smyčky (prázdná smyčka) a konce smyčky před začátek. */
void test_z5_move_out_of_loop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_loop_group_tape();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 6));
    ASSERT_NAMES("A,#21,#24,#25,#22,C,B");
    assert_plays("A,C,B");
    /* Loop End před Loop Start: smyčka bez konce */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 1));
    ASSERT_NAMES("A,#25,#21,#24,#22,C,B");
    assert_plays("A,C,B");
    /* smazání začátku skupiny */
    const int ids[] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ids, 1));
    assert_plays("A,C,B");
}

/* ================================================================
 * ZNAČKA CHECKPOINTU A SNAPSHOT
 * ================================================================ */

/*
 * Značka checkpointu nese příznaky neuloženo: vložený blok uprostřed
 * (neuložený) a připojení za značkou; po uložení (všechny uložené) návrat
 * na značku vrátí neuložený blok uprostřed.
 */
void test_tape_mark_restores_unsaved_flags(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(1, "X", 1000));
    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    TEST_ASSERT_EQUAL_UINT32(4, mark.block_count);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(4, "R", 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tmp_path(1, "tmz")));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    ASSERT_NAMES("A,X,B,C");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    const int unsaved[] = {0, 1, 0, 0};
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT(unsaved[i], cmttmz_block_is_unsaved(i));
    check_invariants();
}

/**
 * @brief Uloží snapshot stroje do paměti (emulace v pauze).
 * @param[out] buf Buffer (g_free()). @param[out] size Velikost.
 */
static void snap_save(uint8_t **buf, size_t *size)
{
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmt edit", buf, size));
}

/** @brief Načte snapshot z paměti a uvolní buffer. @param buf Buffer. @param size Velikost. */
static void snap_load(uint8_t *buf, size_t size)
{
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
}

/**
 * @brief Připraví upravenou pásku pro snapshot: soubor A,B,C; X vložený na 1, C přesunutý na 0, kurzor na B.
 *
 * Výsledek: C,A,X,B; neuložený jen X (index 2); otevřený blok 3 (B).
 */
static void prepare_edited_tape(void)
{
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(1, "X", 1000));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(3));
    ASSERT_NAMES("C,A,X,B");
}

/*
 * Snapshot po úpravách se stejnou páskou v kazetofonu
 * (Quick Save -> Quick Load): stejné bloky ve stejném pořadí, stejné
 * příznaky neuloženo, stejný kurzor, Modified. Bez vložené pásky
 * (cizí relace) je páska z obrazu bez cesty a celá neuložená.
 */
void test_snapshot_after_edits_keeps_order_flags_cursor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    prepare_edited_tape();
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    uint8_t *buf2 = g_memdup2(buf, size);

    snap_load(buf, size);
    ASSERT_NAMES("C,A,X,B");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    const int unsaved[] = {0, 0, 1, 0};
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT_MESSAGE(unsaved[i], cmttmz_block_is_unsaved(i), "unsaved flag");
    TEST_ASSERT_EQUAL_INT(3, open_block());
    check_invariants();

    cmt_eject();
    snap_load(buf2, size);
    ASSERT_NAMES("C,A,X,B");
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmttmz_block_is_unsaved(i), "tape without file");
    TEST_ASSERT_EQUAL_INT(3, open_block());
    check_invariants();
}

/*
 * Snapshot bez příznaků neuloženo (zápis jako starší emulátor): načte se, se
 * stejnou páskou v kazetofonu platí dosavadní pravidlo (obraz s cestou =
 * všechny bloky uložené).
 */
void test_snapshot_without_unsaved_flags_loads(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    prepare_edited_tape();
    snap_cmt_test_omit_unsaved(true);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    snap_cmt_test_omit_unsaved(false);
    snap_load(buf, size);
    ASSERT_NAMES("C,A,X,B");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(i));
    TEST_ASSERT_EQUAL_INT(3, open_block());
    check_invariants();
}

/* ================================================================
 * RYCHLOST BLOKU A ULOŽENÍ
 * ================================================================ */

/*
 * Změna rychlosti neotevřeného bloku nastaví Modified a uložení do
 * TMZ zapíše blok s novou rychlostí (po novém otevření má blok 1 rychlost
 * 2:1). Test před opravou chybu potvrdil: Modified zůstal
 * 0 a znovu otevřený blok měl rychlost 1:1. Táž rychlost znovu Modified
 * nemění.
 */
void test_z7_block_speed_marks_modified_and_saves(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1, true, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmttmz_is_modified(), "block speed change marks the tape modified");
    /* táž rychlost znovu: Modified se nemění (uložením vynulovaný zůstane 0) */
    cmttmz_set_modified(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    cmttmz_set_modified(1);
    const char *out = tmp_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(out));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)out));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_2_1, cmtext_container_get_block_cmt_speed(c, 1), "saved block keeps its speed");
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, cmtext_container_get_block_cmt_speed(c, 0));
}

/* ================================================================
 * IMPORT SOUBORU JAKO BLOKŮ
 * ================================================================ */

/**
 * @brief Cesta dalšího dočasného souboru (slot 0 až 3).
 * @param slot Slot. @param ext Přípona bez tečky. @return Cesta (uvolní tearDown).
 */
static const char *extra_path(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_edit_x_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    g_free(s_extra[slot]);
    s_extra[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_extra[slot];
}

/**
 * @brief Zapíše MZF (hlavička + tělo TEST_BODY_SIZE B) do souboru.
 * @param slot Slot extra_path(). @param name Jméno MZF. @param ext Přípona ("mzf").
 * @return Cesta.
 */
static const char *write_mzf(int slot, const char *name, const char *ext)
{
    uint8_t raw[128 + TEST_BODY_SIZE];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x01;
    memset(&raw[1], 0x0D, 17);
    memcpy(&raw[1], name, strlen(name));
    raw[0x12] = TEST_BODY_SIZE;
    raw[0x15] = 0x12;
    raw[0x17] = 0x12;
    for (int i = 0; i < TEST_BODY_SIZE; i++) raw[128 + i] = (uint8_t)((i * 37 + 11 + name[0]) & 0xFF);
    const char *path = extra_path(slot, ext);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, sizeof(raw), NULL));
    return path;
}

/**
 * @brief Zapíše MZT ze dvou MZF se jmény "M0", "M1".
 * @param slot Slot extra_path(). @return Cesta.
 */
static const char *write_mzt2(int slot)
{
    uint8_t raw[2 * (128 + TEST_BODY_SIZE)];
    memset(raw, 0, sizeof(raw));
    for (int b = 0; b < 2; b++) {
        uint8_t *m = raw + b * (128 + TEST_BODY_SIZE);
        m[0] = 0x01;
        memset(&m[1], 0x0D, 17);
        m[1] = 'M';
        m[2] = (uint8_t)('0' + b);
        m[0x12] = TEST_BODY_SIZE;
        m[0x15] = 0x12;
        m[0x17] = 0x12;
        for (int i = 0; i < TEST_BODY_SIZE; i++) m[128 + i] = (uint8_t)((i * 37 + 11 + b) & 0xFF);
    }
    const char *path = extra_path(slot, "mzt");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, sizeof(raw), NULL));
    return path;
}

/** @brief Ověří příznaky neuloženo všech bloků. @param flags Očekávané hodnoty. @param n Počet bloků. */
static void assert_unsaved(const int *flags, int n)
{
    TEST_ASSERT_EQUAL_INT(n, tape_count());
    for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_INT_MESSAGE(flags[i], cmttmz_block_is_unsaved(i), "unsaved flag");
}

/* Import MZF bez vložené pásky: nová páska, blok 0 otevřený, neuložený, Modified, PLAY hraje. */
void test_import_mzf_without_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    uint32_t n = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(write_mzf(0, "NEWPROG", "mzf"), 5, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    check_invariants();
    ASSERT_NAMES("NEWPROG");
    TEST_ASSERT_EQUAL_INT(0, open_block());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    const int u[] = {1};
    assert_unsaved(u, 1);
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    cmt_stop();
}

/* Import MZF doprostřed: pořadí, kurzor sleduje aktuální blok, příznaky, šev, čítače. */
void test_import_mzf_into_middle(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    st_SERIALS s = serials();
    uint32_t n = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(write_mzf(0, "MID", "mzf"), 1, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    check_invariants();
    ASSERT_NAMES("A,MID,B,C");
    assert_serials_grew(s);
    TEST_ASSERT_EQUAL_INT(2, open_block());
    const int u[] = {0, 1, 0, 0};
    assert_unsaved(u, 4);
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1)); /* import dal MZF pauzu 0, šev ji zvětšil */
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp.origin);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_import_warning());
}

/* Import MZT se dvěma soubory před první blok. */
void test_import_mzt_before_first(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    uint32_t n = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(write_mzt2(0), 0, &n));
    TEST_ASSERT_EQUAL_UINT32(2, n);
    check_invariants();
    ASSERT_NAMES("M0,M1,A,B");
    TEST_ASSERT_EQUAL_INT(2, open_block());
    const int u[] = {1, 1, 0, 0};
    assert_unsaved(u, 4);
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1)); /* poslední importovaný před A */
}

/* Import TMZ s řídicími bloky na konec (vložení na konec je úprava). */
void test_import_tmz_with_control_blocks_at_end(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "imported");
    const uint8_t loop[] = {2, 0};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, loop, sizeof(loop));
    add_mzf_block(f, "L", 0);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    const char *tmz = save_tape(f, 1, "tmz");
    open_letters(2);
    unsigned ts = cmttmz_get_tape_serial();
    uint32_t n = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(tmz, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(4, n);
    check_invariants();
    ASSERT_NAMES("A,B,#30,#24,L,#25");
    TEST_ASSERT_NOT_EQUAL_UINT(ts, cmttmz_get_tape_serial());
    const int u[] = {0, 0, 1, 1, 1, 1};
    assert_unsaved(u, 6);
    /* blok 0x46 MZ800EMU-CMT z TMZ má měnitelnou rychlost (PROFILE_ARGS) */
    TEST_ASSERT_TRUE(cmttmz_block_speed_changeable(4, CMTSPEED_2_1));
}

/*
 * Chyby importu: soubor bez bloků, neexistující soubor, nepodporovaná
 * přípona, pozice mimo rozsah - chyba, text chyby (i důvod jako u
 * otevření), páska beze změny; bez pásky deck zůstane bez pásky.
 */
void test_import_errors_keep_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *empty = new_tmz();
    const char *empty_path = save_tape(empty, 1, "tmz");
    const char *txt = extra_path(1, "txt");
    TEST_ASSERT_TRUE(g_file_set_contents(txt, "hello", 5, NULL));
    gchar *missing = g_build_filename(g_get_tmp_dir(), "mztest_cmt_edit_missing_file.mzf", NULL);
    g_remove(missing);

    open_letters(2);
    st_SERIALS s = serials();
    const char *bad[] = {empty_path, missing, txt};
    for (int i = 0; i < 3; i++) {
        uint32_t n = 99;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_FAILURE, cmt_tape_import_file(bad[i], 1, &n), bad[i]);
        TEST_ASSERT_EQUAL_UINT32(0, n);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_edit_error(), "Can't import"), cmttmz_get_edit_error());
        TEST_ASSERT_TRUE(cmttmz_get_prepare_error()[0] != '\0');
        ASSERT_NAMES("A,B");
        TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
        assert_serials_same(s);
    }
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(write_mzf(0, "X", "mzf"), 3, NULL));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "out of range"));
    ASSERT_NAMES("A,B");
    check_invariants();

    /* bez pásky: chyba importu nechá deck bez pásky */
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(missing, 0, NULL));
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "Can't import"));
    g_free(missing);
}

/* Import odmítnutý mimo STOP a při záznamu vstupů. */
void test_import_refused_outside_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *mzf = write_mzf(0, "X", "mzf");
    open_letters(2);
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(mzf, 0, NULL));
    ASSERT_NAMES("A,B");
    cmt_stop();
    g_replay_mode = REPLAY_MODE_RECORDING;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(mzf, 0, NULL));
    g_replay_mode = REPLAY_MODE_OFF;
    ASSERT_NAMES("A,B");
    cmt_eject();
    g_replay_mode = REPLAY_MODE_PLAYING;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(mzf, 0, NULL));
    g_replay_mode = REPLAY_MODE_OFF;
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
}

/* Import WAV: blok 0x18 má v indexu typ WAV (přepínač polarity na něj působí) jako po otevření WAV. */
void test_import_wav_block_type(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(1);
    const char *wav = extra_path(2, "wav");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(0, wav));
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(wav, 1, NULL));
    check_invariants();
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, cmttmz_get_file()->blocks[1].id);
    /* ostatní bloky typ podle obsahu */
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 0));
}

/*
 * Ověření tvrzení dokumentace: blok WAV importovaný do
 * pásky má typ WAV (přepínač polarity na něj působí, blok má callback
 * polarity), po uložení do TMZ a novém otevření je to obyčejný blok 0x18
 * typu TZX bez callbacku polarity.
 */
void test_final_imported_wav_polarity_until_reopen(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(1);
    const char *wav = extra_path(2, "wav");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(0, wav));
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(wav, 1, NULL));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.ext->block->block_id);
    TEST_ASSERT_NOT_NULL_MESSAGE((void *)g_cmt.ext->block->cb_set_polarity, "the imported WAV block follows the polarity switch");

    const char *tmz = extra_path(3, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tmz));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)tmz));
    c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, cmttmz_get_file()->blocks[1].id);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_NULL_MESSAGE((void *)g_cmt.ext->block->cb_set_polarity, "after Save and Open the block no longer follows the switch");
}

/* ================================================================
 * EXPORT CELÉ PÁSKY DO WAV
 * ================================================================ */

/**
 * @brief Přečte vzorkovací frekvenci a počet vzorků WAV (data / block_align).
 * @param path Soubor. @param[out] rate Frekvence. @param[out] samples Počet vzorků.
 * @param[out] bits Bity na vzorek. @param[out] channels Počet kanálů.
 */
static void wav_info(const char *path, uint32_t *rate, uint32_t *samples, unsigned *bits, unsigned *channels)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_TRUE(len >= 12 && 0 == memcmp(data, "RIFF", 4) && 0 == memcmp(data + 8, "WAVE", 4));
    const uint8_t *d = (const uint8_t *)data;
    uint32_t block_align = 0, data_size = 0;
    *rate = 0;
    *bits = 0;
    *channels = 0;
    for (gsize pos = 12; pos + 8 <= len;) {
        uint32_t sz = d[pos + 4] | (d[pos + 5] << 8) | (d[pos + 6] << 16) | ((uint32_t)d[pos + 7] << 24);
        if (0 == memcmp(d + pos, "fmt ", 4) && pos + 8 + 16 <= len) {
            *channels = d[pos + 10] | (d[pos + 11] << 8);
            *rate = d[pos + 12] | (d[pos + 13] << 8) | (d[pos + 14] << 16) | ((uint32_t)d[pos + 15] << 24);
            block_align = d[pos + 20] | (d[pos + 21] << 8);
            *bits = d[pos + 22] | (d[pos + 23] << 8);
        } else if (0 == memcmp(d + pos, "data", 4)) {
            data_size = sz;
            break;
        }
        pos += 8 + sz + (sz & 1u);
    }
    TEST_ASSERT_TRUE(block_align > 0);
    *samples = data_size / block_align;
    g_free(data);
}

/** @brief Počet vzorků WAV exportu jednoho bloku (cmttmz_export_block()). @param id Blok. @return Vzorky. */
static uint32_t block_wav_samples(int id)
{
    const char *p = extra_path(3, "wav");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(id, p));
    uint32_t rate, samples;
    unsigned bits, ch;
    wav_info(p, &rate, &samples, &bits, &ch);
    return samples;
}

/*
 * Dva programy s pauzou: WAV 44 100 Hz, 8 bit mono, délka = součet
 * exportů jednotlivých bloků (tolerance 1 vzorek na blok), zpráva
 * výsledku; páska ani Modified se nemění.
 */
void test_export_tape_two_programs(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(2);
    uint32_t expect = block_wav_samples(0) + block_wav_samples(1);
    st_SERIALS s = serials();
    const char *out = extra_path(0, "wav");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(out), cmttmz_get_export_message());
    uint32_t rate, samples;
    unsigned bits, ch;
    wav_info(out, &rate, &samples, &bits, &ch);
    TEST_ASSERT_EQUAL_UINT32(44100, rate);
    TEST_ASSERT_EQUAL_UINT(8, bits);
    TEST_ASSERT_EQUAL_UINT(1, ch);
    TEST_ASSERT_UINT32_WITHIN(2, expect, samples);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "Tape exported"));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_export_warning());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    assert_serials_same(s);
    ASSERT_NAMES("A,B");
}

/* Páska se smyčkou: blok ve smyčce 2x (délka A + 2 B + C, tolerance 1 vzorek na blok). */
void test_export_tape_with_loop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t loop[] = {2, 0};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, loop, sizeof(loop));
    add_mzf_block(f, "B", 1000);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    uint32_t a = block_wav_samples(0), b = block_wav_samples(2), c = block_wav_samples(4);
    const char *out = extra_path(0, "wav");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(out), cmttmz_get_export_message());
    uint32_t rate, samples;
    unsigned bits, ch;
    wav_info(out, &rate, &samples, &bits, &ch);
    TEST_ASSERT_UINT32_WITHIN(4, a + 2 * b + c, samples);
}

/* Nepřehratelný blok se přeskočí s varováním; zbytek pásky se exportuje. */
void test_export_tape_skips_unplayable(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t junk[] = {1, 0, 0, 0, 0xAA};
    add_raw_block(f, 0x16, junk, sizeof(junk)); /* C64 ROM type data: player ho nepodporuje */
    add_mzf_block(f, "C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    uint32_t expect = block_wav_samples(0) + block_wav_samples(2);
    const char *out = extra_path(0, "wav");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(out), cmttmz_get_export_message());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_warning(), "cannot be played were skipped: 1"), cmttmz_get_export_warning());
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "skipped"));
    uint32_t rate, samples;
    unsigned bits, ch;
    wav_info(out, &rate, &samples, &bits, &ch);
    TEST_ASSERT_UINT32_WITHIN(2, expect, samples);
}

/*
 * Chyby exportu: prázdná páska, jiná přípona, páska bez signálu, mez
 * délky (smyčka přes blok se signálem, mez zmenšená testovacím
 * nastavením), selhání zápisu - existující soubor zůstane.
 */
void test_export_tape_errors_and_atomic(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *out = extra_path(0, "wav");
    TEST_ASSERT_TRUE(g_file_set_contents(out, "keep", 4, NULL));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(out));
    TEST_ASSERT_EQUAL_STRING("The tape is empty", cmttmz_get_export_message());

    open_letters(1);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(extra_path(1, "tmz")));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "Unsupported"));

    /* selhání před nahrazením cíle: původní obsah zůstane */
    cmttmz_test_fail_before_replace(1);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(out));
    cmttmz_test_fail_before_replace(0);
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(out, &data, &len, NULL));
    TEST_ASSERT_EQUAL_UINT(4, len);
    TEST_ASSERT_EQUAL_MEMORY("keep", data, 4);
    g_free(data);

    /* páska jen z řídicích bloků: bez signálu */
    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "nothing");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(out));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "no signal"));

    /* nekonečná smyčka přes blok se signálem (skok zpět): mez délky */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t back[] = {0xFF, 0xFF}; /* skok -1 */
    add_raw_block(f, TZX_BLOCK_ID_JUMP, back, sizeof(back));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    cmttmz_test_set_export_limit(60);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_message(), "longer than"), cmttmz_get_export_message());
    cmttmz_test_set_export_limit(0);
    TEST_ASSERT_TRUE(g_file_get_contents(out, &data, &len, NULL));
    TEST_ASSERT_EQUAL_UINT(4, len);
    g_free(data);
}

/* ================================================================
 * ŘÍDICÍ BLOKY: SKOKY A VOLÁNÍ PO ÚPRAVĚ
 * ================================================================ */

/** @brief Blok 0x23 Jump s relativním posunem. @param f Soubor. @param off Posun. */
static void add_jump_block(st_TZX_FILE *f, int16_t off)
{
    const uint16_t u = (uint16_t)off;
    const uint8_t d[2] = {(uint8_t)(u & 0xFF), (uint8_t)(u >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_JUMP, d, 2);
}

/** @brief Blok 0x26 Call Sequence s jedním posunem. @param f Soubor. @param off Posun od bloku volání. */
static void add_call_block(st_TZX_FILE *f, int16_t off)
{
    const uint16_t u = (uint16_t)off;
    const uint8_t d[4] = {1, 0, (uint8_t)(u & 0xFF), (uint8_t)(u >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_CALL_SEQUENCE, d, 4);
}

/**
 * @brief Otevře pásku se skokem a voláním: A, Jump +2, B, Call +2, C, D, Return.
 *
 * Indexy: 0 A, 1 #23, 2 B, 3 #26, 4 C, 5 D, 6 #27. Průchod: A, skok na 3,
 * volání D (5) a návrat za volání, C, D, Return bez volání se přeskočí.
 */
static void open_jump_call_tape(void)
{
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_jump_block(f, 2);
    add_mzf_block(f, "B", 1000);
    add_call_block(f, 2);
    add_mzf_block(f, "C", 1000);
    add_mzf_block(f, "D", 0);
    add_raw_block(f, TZX_BLOCK_ID_RETURN_FROM_SEQ, NULL, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    ASSERT_NAMES("A,#23,B,#26,C,D,#27");
}

/*
 * Řídicí bloky se skoky a voláním: po smazání a přesunu, které pošlou cíl skoku
 * a volání mimo pásku nebo jinam, a po vložení bloku mezi skok a jeho cíl
 * páska dohraje bez pádu a zacyklení (průchod playerem s mezí kroků);
 * export celé pásky také skončí. Žádnou úpravu není třeba odmítnout.
 */
void test_z5_jump_and_call_after_edits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *out = extra_path(0, "wav");

    open_jump_call_tape();
    assert_plays("A,D,C,D");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));

    /* smazání cíle skoku i volání: skok míří za konec pásky */
    const int tail[] = {2, 3, 4, 5, 6};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(tail, 5));
    ASSERT_NAMES("A,#23");
    assert_plays("A");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));

    /* skok přesunutý na konec: cíl mimo pásku, volání míří na D */
    open_jump_call_tape();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 6));
    ASSERT_NAMES("A,B,#26,C,D,#27,#23");
    assert_plays("A,B,D,C,D");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));

    /* smazaný Return: volání se nevrátí, přehraje se dál do konce */
    open_jump_call_tape();
    const int ret[] = {6};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(ret, 1));
    assert_plays("A,D");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));

    /* volání přesunuté před skok, Return na začátek: posuny míří jinam */
    open_jump_call_tape();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(6, 1));
    ASSERT_NAMES("#26,#27,A,#23,B,C,D");
    int seq[64];
    TEST_ASSERT_TRUE_MESSAGE(play_whole_tape(seq, 64) >= 0, "playback finishes");
    check_invariants();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));

    /* vložení bloku mezi skok a cíl: skok teď míří na vložený blok */
    open_jump_call_tape();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, insert_mzf(2, "X", 1000));
    ASSERT_NAMES("A,#23,X,B,#26,C,D,#27");
    assert_plays("A,B,D,C,D");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_tape_wav(out));
}

/* ================================================================
 * IMPORT A RYCHLOSTI: DALŠÍ PŘÍPADY
 * ================================================================ */

/*
 * Páska se změněnou rychlostí bloku je Modified, snapshot ji vloží
 * jako obraz; po vysunutí a načtení je bez cesty, Modified a blok má
 * rychlost ze snapshotu.
 */
void test_h2fix_speed_change_snapshot_embeds_image(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", g_cmt.last_filename, "the tape comes from the embedded image");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 1));
    check_invariants();
}

/*
 * Obnova rychlostí ze snapshotu (soubor z cesty) a z checkpointu
 * záznamu vstupů (vložená páska, bez obrazu) Modified nenastaví.
 */
void test_h2fix_speed_restore_keeps_modified(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    /* snapshot s rychlostmi nad souborem z cesty (jako starší verze, Modified 0) */
    open_letters(3);
    const char *path = s_paths[0];
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    cmttmz_set_modified(0);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(cmtext_get_container(g_cmt.ext), 1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_is_modified(), "restoring speeds from a snapshot does not set Modified");

    /* checkpoint: snapshot bez obrazu nad vloženou páskou (keep-inserted) */
    snap_cmt_set_embed_tape(false);
    snap_save(&buf, &size);
    snap_cmt_set_embed_tape(true);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_1_1));
    cmttmz_set_modified(0);
    snap_cmt_set_keep_inserted_tape(true);
    snap_load(buf, size);
    snap_cmt_set_keep_inserted_tape(false);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(cmtext_get_container(g_cmt.ext), 1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_is_modified(), "restoring speeds from a checkpoint does not set Modified");
}

/**
 * @brief Zapíše TAP se dvěma bloky (hlavička ZX a data).
 * @param slot Slot extra_path(). @return Cesta.
 */
static const char *write_tap(int slot)
{
    GByteArray *ba = g_byte_array_new();
    uint8_t hdr[19];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x00; /* flag hlavičky */
    hdr[1] = 3;    /* code */
    memcpy(&hdr[2], "TAPTEST   ", 10);
    hdr[12] = 4;   /* délka dat */
    hdr[14] = 0x00;
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
    const char *path = extra_path(slot, "tap");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)ba->data, ba->len, NULL));
    g_byte_array_free(ba, TRUE);
    return path;
}

/* Změna rychlosti bloku TAP (0x11 z importu TAP) nastaví Modified. */
void test_h2fix_tap_block_speed_marks_modified(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)write_tap(0)));
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_TRUE(cmttmz_block_speed_changeable(1, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1, false, NULL));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
}

/**
 * @brief Sestaví a otevře pásku s blokem 0x43 (takt @p clock) na pozici @p clock_at mezi bloky MZF "A".."C".
 * @param clock_at Pozice bloku 0x43 (-1 = bez něj). @param clock Takt v Hz.
 * @param slot Slot dočasného souboru. @param ext "tmz".
 * @return Cesta uloženého souboru (nevkládá se, když @p open 0).
 */
static const char *clock_tape(int clock_at, uint32_t clock, int slot, int open)
{
    st_TZX_FILE *f = new_tmz();
    const char *names[] = {"A", "B", "C"};
    for (int i = 0; i <= 3; i++) {
        if (i == clock_at) {
            st_TZX_BLOCK *mi = tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ700, clock, 0);
            TEST_ASSERT_NOT_NULL(mi);
            TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, mi));
            free(mi);
        }
        if (i < 3) add_mzf_block(f, names[i], (i < 2) ? 1000 : 0);
    }
    const char *p = save_tape(f, slot, "tmz");
    if (open) TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)p));
    return p;
}

/*
 * Import souboru s blokem 0x43 jiného taktu doprostřed změní
 * takt dosavadních bloků 0x46 za ním, import souboru bez 0x43 za blok 0x43
 * pásky jiného taktu změní takt importovaných bloků - obojí s varováním.
 * Bez varování: import bez vlivu na takt, blok 0x43 s výchozím taktem
 * (přehrává se stejně jako bez něj) a posun bloků, které takt nepoužívají
 * (TAP).
 */
void test_h2fix_import_clock_warning(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *clock_word = "The tape clock setting now applies to different blocks; check their speed.";
    const uint32_t other = 4000000u; /* jiný než výchozí takt playeru (MZCMT_FM_DEFAULT_TIMEBASE) */
    /* soubor s 0x43 na začátku -> doprostřed pásky bez 0x43 */
    const char *with_clock = clock_tape(0, other, 1, 0);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(with_clock, 1, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());

    /* soubor bez 0x43 -> za blok 0x43 pásky */
    clock_tape(0, other, 0, 1);
    const char *plain = write_mzf(0, "PLAIN", "mzf");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(plain, 2, NULL));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());

    /* soubor bez 0x43 před blok 0x43 (takt nikde jiný): bez varování */
    clock_tape(1, other, 0, 1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(plain, 0, NULL));
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());

    /* soubor s 0x43 na konec pásky bez 0x43: takt importovaných jako ve vlastním souboru, bez varování */
    open_letters(2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(with_clock, 2, NULL));
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());

    /* 0x43 s výchozím taktem doprostřed pásky bez 0x43: hraje se stejně, bez varování */
    const char *default_clock = clock_tape(0, 3546895u, 1, 0);
    open_letters(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(default_clock, 1, NULL));
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());

    /* blok TAP (0x11) za blok 0x43 jiného taktu: takt nepoužívá, bez varování */
    clock_tape(0, other, 0, 1);
    const char *tap = write_tap(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(tap, 4, NULL));
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_import_warning(), clock_word), cmttmz_get_import_warning());
}

/*
 * Mazání a přesun, po kterém blok 0x46 hraje jiným
 * účinným taktem CPU (blok 0x43 smazaný nebo přesunutý, program přesunutý
 * přes něj), hlásí stejné varování jako import (cmttmz_get_edit_warning(),
 * zpráva výsledku úpravy fronty); bez změny přehrávání žádné varování.
 */
void test_final_edit_clock_warning(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *clock_word = "The tape clock setting now applies to different blocks; check their speed.";
    const uint32_t other = 4000000u;
    /* [0x43, A, B, C] -> [A, 0x43, B, C]: A hraje výchozím taktem místo 4 MHz */
    clock_tape(0, other, 0, 1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(1, 0));
    TEST_ASSERT_EQUAL_STRING(clock_word, cmttmz_get_edit_warning());
    /* B a C za blokem 0x43 mezi sebou: takt se nemění, varování se nuluje */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(2, 3));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    /* smazání bloku 0x43: C a B hrají výchozím taktem */
    const int del_clock[1] = {1};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(del_clock, 1));
    TEST_ASSERT_EQUAL_STRING(clock_word, cmttmz_get_edit_warning());

    /* [A, 0x43 výchozí takt, B, C]: přesun ani smazání nic nemění */
    clock_tape(1, MZCMT_FM_DEFAULT_TIMEBASE, 0, 1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(3, 0));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    const int del_default[1] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(del_default, 1));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());

    /* smazání programu za blokem 0x43 jiného taktu: ostatní hrají stejně */
    clock_tape(0, other, 0, 1);
    const int del_prog[1] = {2};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_remove_blocks(del_prog, 1));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());

    /* fronta zásahů (okno): zpráva výsledku přesunu i mazání nese varování */
    clock_tape(0, other, 0, 1);
    st_DEVICE_REQ_RESULT res;
    st_CMT_REQUEST_EDIT_RESULT r;
    uint32_t seq = cmt_request_move_block_on_tape(cmttmz_get_tape_serial(), 3, 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    device_state_lock();
    TEST_ASSERT_TRUE(cmt_request_get_edit_result(seq, &r));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING(clock_word, r.message);
    /* [C, 0x43, A, B]: smazání bloku 0x43 */
    const int del_req[1] = {1};
    seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), del_req, 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    device_state_lock();
    TEST_ASSERT_TRUE(cmt_request_get_edit_result(seq, &r));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING(clock_word, r.message);
    /* bez změny taktu prázdná zpráva */
    const int del_plain[1] = {0};
    seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), del_plain, 1);
    device_request_poll();
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    device_state_lock();
    TEST_ASSERT_TRUE(cmt_request_get_edit_result(seq, &r));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("", r.message);
}

/* Neúspěšný import do prázdného decku nezanechá stopu (páska nevznikne, žádný zápis na médium). */
void test_h2fix_failed_import_into_empty_deck_leaves_no_trace(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cmt_eject();
    unsigned ts = cmttmz_get_tape_serial();
    uint32_t writes = g_media_writes;
    gchar *missing = g_build_filename(g_get_tmp_dir(), "mztest_cmt_edit_missing_h2fix.mzf", NULL);
    g_remove(missing);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(missing, 0, NULL));
    g_free(missing);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_NULL(cmttmz_get_file());
    TEST_ASSERT_EQUAL_UINT32(writes, g_media_writes);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(ts, cmttmz_get_tape_serial(), "no tape was created and ejected");
}

/*
 * Signal speed decku Modified nenastaví; uložení do TMZ zapíše bloky
 * DEFAULT rychlostí, kterou hrají (i když se výchozí rychlost změnila za
 * PLAY a bloky v souboru jsou sestavené starou).
 */
void test_h2fix_deck_speed_save_writes_playing_speed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)write_mzt2(1))); /* bloky DEFAULT */
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_3_1));
    cmt_stop();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_is_modified(), "the deck speed does not set Modified");
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &sp, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTSPEED_1_1, sp.speed, "block 1 is still built at the old speed");
    const char *out = tmp_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(out));
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)out));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 1));
}

/* Chyba importu s dlouhou cestou nese celý důvod (zkrátí se cesta). */
void test_h2fix_import_error_keeps_reason(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(1);
    GString *p = g_string_new(g_get_tmp_dir());
    g_string_append(p, G_DIR_SEPARATOR_S);
    for (int i = 0; i < 40; i++) g_string_append(p, "long_directory_");
    g_string_append(p, ".mzf");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(p->str, 0, NULL));
    const char *reason = cmttmz_get_prepare_error();
    TEST_ASSERT_TRUE(reason[0] != '\0');
    const char *err = cmttmz_get_edit_error();
    TEST_ASSERT_TRUE(strlen(err) < CMTTMZ_EDIT_ERROR_SIZE);
    /* důvod (u chybějícího souboru obsahuje i cestu) se vejde aspoň začátkem: věta končí důvodem z prepare_error */
    size_t rl = strlen(reason), el = strlen(err);
    if (rl < CMTTMZ_EDIT_ERROR_SIZE - 32) {
        TEST_ASSERT_TRUE_MESSAGE(el >= rl && 0 == strcmp(err + el - rl, reason), err);
    }
    TEST_ASSERT_TRUE(0 == strncmp(err, "Can't import", 12));
    g_string_free(p, TRUE);
}

/*
 * Zkrácená dlouhá cesta s diakritikou nezačne půlkou znaku
 * UTF-8. Řez se počítá od konce cesty: dvě cesty, jejichž konec se liší
 * o bajt (ASCII znak za dvoubajtovými znaky), takže řez padne jednou na
 * hranici, jednou doprostřed dvoubajtového znaku (bez opravy text není
 * platné UTF-8).
 */
void test_wave_import_error_path_utf8(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(1);
    for (int extra = 0; extra < 2; extra++) {
        GString *p = g_string_new(g_get_tmp_dir());
        g_string_append(p, G_DIR_SEPARATOR_S);
        for (int i = 0; i < 140; i++) g_string_append(p, "\xC5\xBE"); /* "ž" */
        if (extra) g_string_append_c(p, 'x');
        g_string_append(p, ".xyz");
        TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_import_file(p->str, 0, NULL));
        const char *err = cmttmz_get_edit_error();
        TEST_ASSERT_TRUE_MESSAGE(strstr(err, "...") != NULL, "the path is shortened");
        TEST_ASSERT_TRUE_MESSAGE(g_utf8_validate(err, -1, NULL), err);
        TEST_ASSERT_NOT_NULL(strstr(err, "Unsupported tape file extension"));
        g_string_free(p, TRUE);
    }
}

/*
 * Export - vyčerpaný rozpočet iterací (skok na sebe za blokem A)
 * export ukončí s varováním a A se exportuje; pokračování za Pause 0;
 * blok, který nejde přestavět rychlostí, kterou hraje: export i uložení
 * do TMZ selžou s textem, soubor ani páska se nemění.
 */
void test_h2fix_export_budget_pause_and_rebuild_failure(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const char *out = extra_path(0, "wav");

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_jump_block(f, 0); /* skok na sebe: řídicí bloky bez signálu */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    uint32_t a = block_wav_samples(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(out), cmttmz_get_export_message());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_warning(), "Control flow does not reach"), cmttmz_get_export_warning());
    uint32_t rate, samples;
    unsigned bits, ch;
    wav_info(out, &rate, &samples, &bits, &ch);
    TEST_ASSERT_UINT32_WITHIN(1, a, samples);

    /* Pause 0 (zastavení pásky) export nepřeruší */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t p0[2] = {0, 0};
    add_raw_block(f, TZX_BLOCK_ID_PAUSE, p0, 2);
    add_mzf_block(f, "B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    uint32_t expect = block_wav_samples(0) + block_wav_samples(2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(out), cmttmz_get_export_message());
    wav_info(out, &rate, &samples, &bits, &ch);
    TEST_ASSERT_UINT32_WITHIN(2, expect, samples);

    /* přestavba selže: vlastní pulzy, které generátor nesestaví (0,1 s) */
    open_letters(2);
    st_MZTAPE_PULSES_LENGTH bad;
    memset(&bad, 0, sizeof(bad));
    bad.long_pulse.high = bad.long_pulse.low = 0.1;
    bad.long_pulse.total = 0.2;
    bad.short_pulse.high = bad.short_pulse.low = 0.1;
    bad.short_pulse.total = 0.2;
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_pulses(1, &bad));
    TEST_ASSERT_TRUE(g_file_set_contents(out, "keep", 4, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_tape_wav(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_message(), "cannot be rebuilt"), cmttmz_get_export_message());
    const char *tmz = extra_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(tmz));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_save_message(), "cannot be rebuilt"), cmttmz_get_save_message());
    TEST_ASSERT_FALSE(g_file_test(tmz, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(out, &data, &len, NULL));
    TEST_ASSERT_EQUAL_UINT(4, len);
    g_free(data);
}

/* Blok přeskočený ve smyčce se ve varování exportu objeví jednou. */
void test_h2fix_export_skipped_block_listed_once(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    const uint8_t loop[] = {3, 0};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, loop, sizeof(loop));
    add_mzf_block(f, "A", 1000);
    const uint8_t junk[] = {1, 0, 0, 0, 0xAA};
    add_raw_block(f, 0x16, junk, sizeof(junk));
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape(f, 0, "tmz")));
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_tape_wav(extra_path(0, "wav")), cmttmz_get_export_message());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_warning(), "were skipped: 2"), cmttmz_get_export_warning());
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_export_warning(), "2, 2"), cmttmz_get_export_warning());
}

/*
 * Zpráva exportu nezávisí na locale procesu: délka s desetinnou tečkou
 * i v locale s desetinnou čárkou (čeština), když ho prostředí umí
 * nastavit.
 */
void test_h2fix_export_message_decimal_point(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_letters(1);
    const char *names[] = {"cs-CZ", "Czech_Czechia.1250", "Czech", "cs_CZ.UTF-8", "cs_CZ"};
    char *saved = g_strdup(setlocale(LC_NUMERIC, NULL));
    int comma_locale = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(names) && !comma_locale; i++) {
        if (!setlocale(LC_NUMERIC, names[i])) continue;
        char probe[16];
        snprintf(probe, sizeof(probe), "%.1f", 1.5);
        comma_locale = (strchr(probe, ',') != NULL);
    }
    if (!comma_locale) TEST_MESSAGE("no locale with a decimal comma available, checking the C locale only");
    int rc = cmttmz_export_tape_wav(extra_path(0, "wav"));
    char msg[CMTTMZ_SAVE_MESSAGE_SIZE];
    g_strlcpy(msg, cmttmz_get_export_message(), sizeof(msg));
    setlocale(LC_NUMERIC, saved);
    g_free(saved);
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, rc, msg);
    /* "..., 11.4 s": číslice, tečka, číslice, " s" na konci */
    size_t l = strlen(msg);
    TEST_ASSERT_TRUE_MESSAGE(l > 6 && 0 == strcmp(msg + l - 2, " s"), msg);
    TEST_ASSERT_TRUE_MESSAGE(msg[l - 4] == '.' && g_ascii_isdigit(msg[l - 3]) && g_ascii_isdigit(msg[l - 5]), msg);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_remove_block_after_current);
    RUN_TEST(test_remove_current_block_cursor);
    RUN_TEST(test_remove_block_before_current);
    RUN_TEST(test_remove_current_block_control_follows);
    RUN_TEST(test_remove_all_blocks_then_insert);
    RUN_TEST(test_remove_invalid_arguments);
    RUN_TEST(test_move_block_first_last_and_cursor);
    RUN_TEST(test_move_block_to_itself_and_out_of_range);
    RUN_TEST(test_insert_blocks_before_current);
    RUN_TEST(test_insert_at_end_is_edit);
    RUN_TEST(test_append_keeps_numbers);
    RUN_TEST(test_insert_into_empty_tape_then_play);
    RUN_TEST(test_insert_zero_blocks);
    RUN_TEST(test_remove_non_adjacent_around_cursor);
    RUN_TEST(test_move_seam_before_target);
    RUN_TEST(test_edit_without_open_block);
    RUN_TEST(test_clock_block_invalidates_lengths);
    RUN_TEST(test_edit_machine_hash_and_input_replay);
    RUN_TEST(test_unsaved_flags_follow_blocks);
    RUN_TEST(test_seam_pause_rule);
    RUN_TEST(test_insert_mz_block_into_zxtape_makes_tmz);
    RUN_TEST(test_edit_refused_outside_stop_and_recording);
    RUN_TEST(test_edit_clears_cmthack_tape_state);
    RUN_TEST(test_z5_remove_loop_start);
    RUN_TEST(test_z5_remove_loop_end);
    RUN_TEST(test_z5_move_out_of_loop);
    RUN_TEST(test_tape_mark_restores_unsaved_flags);
    RUN_TEST(test_snapshot_after_edits_keeps_order_flags_cursor);
    RUN_TEST(test_snapshot_without_unsaved_flags_loads);
    RUN_TEST(test_z7_block_speed_marks_modified_and_saves);
    RUN_TEST(test_import_mzf_without_tape);
    RUN_TEST(test_import_mzf_into_middle);
    RUN_TEST(test_import_mzt_before_first);
    RUN_TEST(test_import_tmz_with_control_blocks_at_end);
    RUN_TEST(test_import_errors_keep_tape);
    RUN_TEST(test_import_refused_outside_stop);
    RUN_TEST(test_import_wav_block_type);
    RUN_TEST(test_final_imported_wav_polarity_until_reopen);
    RUN_TEST(test_export_tape_two_programs);
    RUN_TEST(test_export_tape_with_loop);
    RUN_TEST(test_export_tape_skips_unplayable);
    RUN_TEST(test_export_tape_errors_and_atomic);
    RUN_TEST(test_z5_jump_and_call_after_edits);
    RUN_TEST(test_h2fix_speed_change_snapshot_embeds_image);
    RUN_TEST(test_h2fix_speed_restore_keeps_modified);
    RUN_TEST(test_h2fix_tap_block_speed_marks_modified);
    RUN_TEST(test_h2fix_import_clock_warning);
    RUN_TEST(test_final_edit_clock_warning);
    RUN_TEST(test_h2fix_failed_import_into_empty_deck_leaves_no_trace);
    RUN_TEST(test_h2fix_deck_speed_save_writes_playing_speed);
    RUN_TEST(test_h2fix_import_error_keeps_reason);
    RUN_TEST(test_wave_import_error_path_utf8);
    RUN_TEST(test_h2fix_export_budget_pause_and_rebuild_failure);
    RUN_TEST(test_h2fix_export_skipped_block_listed_once);
    RUN_TEST(test_h2fix_export_message_decimal_point);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
