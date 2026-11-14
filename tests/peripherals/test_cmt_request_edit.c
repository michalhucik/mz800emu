/**
 * @file test_cmt_request_edit.c
 * @brief Zásahy úprav pásky přes frontu zásahů zařízení (cmt_request.h).
 *
 * Testuje se:
 *   - kódování seznamu bloků do rozsahů a zpět (cmt_request_block_ranges_encode(),
 *     _decode()) včetně hranic: 1 blok, 300 po sobě jdoucích, 300 ob jeden
 *     (nevejde se do textového argumentu zásahu -> zásah se nezařadí),
 *     neplatné texty,
 *   - smazání, přesun a import zásahem: změna až v device_request_poll(),
 *     výsledek úpravy pro okno (cmt_request_get_edit_result(): blok
 *     k výběru, počet, čítač výměn pásky po úpravě, text chyby),
 *   - neshoda čítače výměn pásky -> CMT_REQUEST_RC_TAPE_CHANGED, páska beze změny,
 *   - odmítnutí při záznamu vstupů (DEVICE_REQ_REJECTED_REPLAY, bez výsledku úpravy),
 *   - chyba mimo STOP a neplatný blok (text chyby ve výsledku úpravy),
 *   - import do prázdného decku (tape_serial 0 i aktuální čítač), import na konec,
 *     nečitelný soubor,
 *   - export celé pásky do WAV zásahem,
 *   - výsledky dvou úprav v jednom pollu jsou oba čitelné podle pořadového čísla,
 *   - device_request_current_seq() mimo zásah 0.
 *
 * Testovací pásky (MZT s bloky pojmenovanými B000, B001, ...) se generují do
 * dočasného adresáře (g_get_tmp_dir).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_container.h"

/** @brief Velikost těla MZF v testovacích páskách. */
#define TEST_BODY_SIZE 16

/** @brief Největší počet bloků testovací pásky. */
#define TEST_MAX_BLOCKS 400

/** @brief Dočasné soubory testu (maže tearDown). */
static gchar *s_files[4] = {NULL, NULL, NULL, NULL};


/**
 * @brief Vrátí cestu dočasného souboru (slot 0 .. 3) s příponou.
 * @param slot Slot. @param ext Přípona bez tečky.
 * @return Cesta (vlastní ji test, uvolní tearDown).
 */
static const char *tmp_file(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_request_edit_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    g_free(s_files[slot]);
    s_files[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_files[slot];
}


/**
 * @brief Zapíše MZT s @p count bloky pojmenovanými "<prefix><nnn>" (od @p first).
 * @param slot Slot dočasného souboru. @param prefix Předpona jména (1 znak).
 * @param first Číslo prvního bloku ve jméně. @param count Počet bloků.
 * @return Cesta k souboru.
 */
static const char *write_mzt(int slot, const char *prefix, int first, int count)
{
    const char *path = tmp_file(slot, "mzt");
    FILE *fh = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fh);
    for (int b = 0; b < count; b++)
    {
        uint8_t hdr[128];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 0x01;
        memset(&hdr[1], 0x0D, 17);
        char name[16];
        int n = snprintf(name, sizeof(name), "%s%03d", prefix, first + b);
        memcpy(&hdr[1], name, (size_t)n);
        hdr[0x12] = TEST_BODY_SIZE;
        hdr[0x15] = 0x12; /* fstrt = 1200h */
        hdr[0x17] = 0x12; /* fexec = 1200h */
        TEST_ASSERT_EQUAL_size_t(sizeof(hdr), fwrite(hdr, 1, sizeof(hdr), fh));
        for (int i = 0; i < TEST_BODY_SIZE; i++)
            TEST_ASSERT_NOT_EQUAL(EOF, fputc((i * 37 + b) & 0xFF, fh));
    };
    TEST_ASSERT_EQUAL_INT(0, fclose(fh));
    return path;
}


/** @brief Otevře testovací pásku s @p count bloky B000.. (STOP). */
static void open_tape(int count)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)write_mzt(0, "B", 0, count)));
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}


/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_count(void)
{
    if (!CMT_TEST_FILLED) return -1;
    return cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext));
}


/**
 * @brief Jména bloků vložené pásky oddělená čárkou.
 * @param[out] out Buffer. @param size Velikost.
 * @return @p out.
 */
static const char *tape_names(char *out, size_t size)
{
    out[0] = '\0';
    size_t len = 0;
    st_CMTEXT_CONTAINER *c = CMT_TEST_FILLED ? cmtext_get_container(g_cmt.ext) : NULL;
    for (int i = 0; c && i < cmtext_container_get_count_blocks(c); i++)
    {
        int n = snprintf(out + len, size - len, "%s%s", i ? "," : "", cmtext_container_get_block_fname(c, i));
        if (n > 0 && (size_t)n < size - len) len += (size_t)n;
    };
    return out;
}

/** @brief Ověří pořadí bloků podle jmen. */
#define ASSERT_NAMES(expected)                                                                                         \
    do {                                                                                                               \
        char names_buf_[512];                                                                                          \
        TEST_ASSERT_EQUAL_STRING(expected, tape_names(names_buf_, sizeof(names_buf_)));                                \
    } while (0)


/** @brief Vyzvedne jeden výsledek zásahu (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


/** @brief Přečte výsledek úpravy zásahu @p seq (selže bez něj). */
static st_CMT_REQUEST_EDIT_RESULT edit_result(uint32_t seq)
{
    st_CMT_REQUEST_EDIT_RESULT r;
    memset(&r, 0, sizeof(r));
    device_state_lock();
    bool ok = cmt_request_get_edit_result(seq, &r);
    device_state_unlock();
    TEST_ASSERT_TRUE_MESSAGE(ok, "edit result not found");
    TEST_ASSERT_EQUAL_UINT32(seq, r.seq);
    return r;
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    cmt_eject();
}


void tearDown(void)
{
    if (CMT_TEST_FILLED && !CMT_TEST_STOP) cmt_stop();
    cmt_eject();
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    for (int i = 0; i < 4; i++)
    {
        if (s_files[i]) g_remove(s_files[i]);
        g_free(s_files[i]);
        s_files[i] = NULL;
    };
}


/* Kódování rozsahů: tvar, hranice, 300 po sobě jdoucích se vejde, 300 ob jeden ne. */
void test_ranges_encode_decode(void)
{
    char text[DEVICE_REQUEST_STR_MAX];
    int out[TEST_MAX_BLOCKS];

    const int one[] = {5};
    TEST_ASSERT_TRUE(cmt_request_block_ranges_encode(one, 1, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("5", text);
    TEST_ASSERT_EQUAL_INT(1, cmt_request_block_ranges_decode(text, out, TEST_MAX_BLOCKS));
    TEST_ASSERT_EQUAL_INT(5, out[0]);

    const int mix[] = {0, 1, 2, 3, 7, 9, 10, 11, 12, 14, 15};
    TEST_ASSERT_TRUE(cmt_request_block_ranges_encode(mix, G_N_ELEMENTS(mix), text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("0-3,7,9-12,14-15", text);
    TEST_ASSERT_EQUAL_INT((int)G_N_ELEMENTS(mix), cmt_request_block_ranges_decode(text, out, TEST_MAX_BLOCKS));
    TEST_ASSERT_EQUAL_INT_ARRAY(mix, out, G_N_ELEMENTS(mix));

    int run[300];
    for (int i = 0; i < 300; i++) run[i] = i;
    TEST_ASSERT_TRUE(cmt_request_block_ranges_encode(run, 300, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("0-299", text);
    TEST_ASSERT_EQUAL_INT(300, cmt_request_block_ranges_decode(text, out, TEST_MAX_BLOCKS));
    TEST_ASSERT_EQUAL_INT_ARRAY(run, out, 300);
    /* kapacita menší než rozsah */
    TEST_ASSERT_EQUAL_INT(-1, cmt_request_block_ranges_decode(text, out, 299));

    int every_other[300];
    for (int i = 0; i < 300; i++) every_other[i] = 2 * i;
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(every_other, 300, text, sizeof(text)));
    /* 150 ob jeden se vejde */
    TEST_ASSERT_TRUE(cmt_request_block_ranges_encode(every_other, 150, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(150, cmt_request_block_ranges_decode(text, out, TEST_MAX_BLOCKS));
    TEST_ASSERT_EQUAL_INT_ARRAY(every_other, out, 150);

    /* neplatné vstupy kodéru */
    const int desc[] = {3, 1};
    const int dup[] = {1, 1};
    const int neg[] = {-1, 2};
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(desc, 2, text, sizeof(text)));
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(dup, 2, text, sizeof(text)));
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(neg, 2, text, sizeof(text)));
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(one, 0, text, sizeof(text)));
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(NULL, 1, text, sizeof(text)));
    TEST_ASSERT_FALSE(cmt_request_block_ranges_encode(mix, G_N_ELEMENTS(mix), text, 4));

    /* neplatné texty dekodéru */
    const char *bad[] = {"", ",", "1,", ",1", "1,,2", "3-1", "2-2", "1,1", "2,1", "1-3,2", "a", "1-", "-2",
                         "1 ,2", "1-2-3", "99999999999", "0-2147483647",
                         /* úvodní nuly: kodér je nikdy nezapíše */
                         "01", "0-01", "00", "1,02", "007-9"};
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, cmt_request_block_ranges_decode(bad[i], out, TEST_MAX_BLOCKS), bad[i]);
}


/* Neplatné argumenty se nezařadí; device_request_current_seq() mimo zásah je 0. */
void test_invalid_args_not_queued(void)
{
    const int ids[] = {1, 2};
    const int dup[] = {2, 1, 2};
    const int neg[] = {0, -3};
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_remove_blocks_on_tape(1, NULL, 1));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_remove_blocks_on_tape(1, ids, 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_remove_blocks_on_tape(1, dup, 3));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_remove_blocks_on_tape(1, neg, 2));
    int every_other[300];
    for (int i = 0; i < 300; i++) every_other[i] = 2 * i;
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_remove_blocks_on_tape(1, every_other, 300));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_move_block_on_tape(1, -1, 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_move_block_on_tape(1, 0, -1));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_import_on_tape(1, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_import_on_tape(1, "", 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_export_tape_on_tape(1, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_export_tape_on_tape(1, ""));
    char long_path[DEVICE_REQUEST_STR_MAX + 8];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_import_on_tape(1, long_path, 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_export_tape_on_tape(1, long_path));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_EQUAL_UINT32(0, device_request_current_seq());
    st_CMT_REQUEST_EDIT_RESULT r;
    TEST_ASSERT_FALSE(cmt_request_get_edit_result(0, &r));
}


/* Smazání zásahem: až v poll, libovolné pořadí čísel, výsledek pro okno. */
void test_remove_in_poll(void)
{
    open_tape(5);
    unsigned serial = cmttmz_get_tape_serial();
    const int ids[] = {3, 1};
    uint32_t seq = cmt_request_remove_blocks_on_tape(serial, ids, 2);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    ASSERT_NAMES("B000,B001,B002,B003,B004");
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_REMOVE_BLOCKS_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    ASSERT_NAMES("B000,B002,B004");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_REMOVE, r.op);
    TEST_ASSERT_EQUAL_INT(0, r.rc);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);
    TEST_ASSERT_EQUAL_INT(1, r.select_block); /* na místě prvního smazaného (1) je B002 */
    TEST_ASSERT_EQUAL_UINT32(3, r.block_count);
    TEST_ASSERT_EQUAL_UINT32(cmttmz_get_tape_serial(), r.tape_serial);
    TEST_ASSERT_NOT_EQUAL_UINT32(serial, r.tape_serial);
    TEST_ASSERT_EQUAL_STRING("", r.message);

    /* smazání posledního bloku: vybraný je nový poslední */
    const int last[] = {2};
    seq = cmt_request_remove_blocks_on_tape(r.tape_serial, last, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("B000,B002");
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(1, r.select_block);

    /* smazání všech: prázdná páska, žádný blok k výběru */
    const int all[] = {0, 1};
    seq = cmt_request_remove_blocks_on_tape(r.tape_serial, all, 2);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(-1, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(0, r.block_count);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);
}


/* 300 po sobě jdoucích bloků jedním zásahem (rozsah "0-299" tam i zpět přes frontu). */
void test_remove_300_consecutive(void)
{
    open_tape(302);
    int ids[300];
    for (int i = 0; i < 300; i++) ids[i] = 299 - i; /* sestupně: zásah seřadí */
    uint32_t seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), ids, 300);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("B300,B301");
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(300, r.count);
    TEST_ASSERT_EQUAL_INT(0, r.select_block);
}


/* Jiný čítač výměn pásky: nic se nezmění, CMT_REQUEST_RC_TAPE_CHANGED (i ve výsledku úpravy). */
void test_tape_changed(void)
{
    open_tape(3);
    unsigned serial = cmttmz_get_tape_serial();
    const int ids[] = {0};
    uint32_t s1 = cmt_request_remove_blocks_on_tape(serial + 1, ids, 1);
    uint32_t s2 = cmt_request_move_block_on_tape(serial + 1, 0, 2);
    uint32_t s3 = cmt_request_import_on_tape(serial + 1, write_mzt(1, "I", 0, 1), 0);
    device_request_poll();
    for (int i = 0; i < 3; i++)
    {
        st_DEVICE_REQ_RESULT res = take_one();
        TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, res.status);
        TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, res.rc);
    };
    ASSERT_NAMES("B000,B001,B002");
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    const uint32_t seqs[] = {s1, s2, s3};
    for (int i = 0; i < 3; i++)
    {
        st_CMT_REQUEST_EDIT_RESULT r = edit_result(seqs[i]);
        TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, r.rc);
        TEST_ASSERT_EQUAL_INT(-1, r.select_block);
        TEST_ASSERT_EQUAL_UINT32(0, r.count);
        TEST_ASSERT_EQUAL_UINT32(serial, r.tape_serial);
    };
}


/* Záznam i přehrávání vstupů: fronta zásahy odmítne, páska beze změny, výsledek úpravy není. */
void test_rejected_while_replay_active(void)
{
    open_tape(3);
    unsigned serial = cmttmz_get_tape_serial();
    const en_REPLAY_MODE modes[] = {REPLAY_MODE_RECORDING, REPLAY_MODE_PLAYING};
    for (int m = 0; m < 2; m++)
    {
        g_replay_mode = modes[m];
        const int ids[] = {1};
        uint32_t s1 = cmt_request_remove_blocks_on_tape(serial, ids, 1);
        uint32_t s2 = cmt_request_move_block_on_tape(serial, 0, 2);
        uint32_t s3 = cmt_request_import_on_tape(serial, write_mzt(1, "I", 0, 1), 0);
        uint32_t s4 = cmt_request_export_tape_on_tape(serial, tmp_file(2, "wav"));
        device_request_poll();
        for (int i = 0; i < 4; i++)
            TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
        g_replay_mode = REPLAY_MODE_OFF;
        ASSERT_NAMES("B000,B001,B002");
        TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
        TEST_ASSERT_FALSE(g_file_test(s_files[2], G_FILE_TEST_EXISTS));
        const uint32_t seqs[] = {s1, s2, s3};
        st_CMT_REQUEST_EDIT_RESULT r;
        device_state_lock();
        for (int i = 0; i < 3; i++)
            TEST_ASSERT_FALSE(cmt_request_get_edit_result(seqs[i], &r));
        device_state_unlock();
        (void)s4;
    };
}


/* Mimo STOP a s neplatným blokem: chyba s textem, páska beze změny. */
void test_errors_with_message(void)
{
    open_tape(3);
    unsigned serial = cmttmz_get_tape_serial();
    const int bad[] = {7};
    uint32_t seq = cmt_request_remove_blocks_on_tape(serial, bad, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(-1, r.rc);
    TEST_ASSERT_EQUAL_STRING("Block 7 does not exist", r.message);
    TEST_ASSERT_EQUAL_INT(-1, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(3, r.block_count);

    seq = cmt_request_move_block_on_tape(serial, 1, 5);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "out of range"));

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    const int ids[] = {0};
    uint32_t s1 = cmt_request_remove_blocks_on_tape(serial, ids, 1);
    uint32_t s2 = cmt_request_move_block_on_tape(serial, 0, 1);
    uint32_t s3 = cmt_request_import_on_tape(serial, write_mzt(1, "I", 0, 1), 0);
    device_request_poll();
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    const uint32_t seqs[] = {s1, s2, s3};
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL_STRING("The tape can be edited only when the deck is stopped", edit_result(seqs[i]).message);
    cmt_stop();
    ASSERT_NAMES("B000,B001,B002");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}


/* Přesun zásahem: cílová pozice je blok k výběru; dva zásahy v jednom pollu mají oba výsledky. */
void test_move_in_poll(void)
{
    open_tape(4);
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t seq = cmt_request_move_block_on_tape(serial, 0, 3);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    ASSERT_NAMES("B000,B001,B002,B003");
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_MOVE_BLOCK_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    ASSERT_NAMES("B001,B002,B003,B000");
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_MOVE, r.op);
    TEST_ASSERT_EQUAL_INT(3, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_UINT32(4, r.block_count);

    /* dva přesuny v jednom pollu: druhý s čítačem po prvním (okno ho zná z výsledku až potom,
     * tady ho test zná předem: každá úprava zvedne čítač o 1) */
    uint32_t a = cmt_request_move_block_on_tape(r.tape_serial, 3, 0);
    uint32_t b = cmt_request_move_block_on_tape(r.tape_serial + 1, 1, 2);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("B000,B002,B001,B003");
    TEST_ASSERT_EQUAL_INT(0, edit_result(a).select_block);
    TEST_ASSERT_EQUAL_INT(2, edit_result(b).select_block);
    TEST_ASSERT_EQUAL_UINT32(edit_result(a).tape_serial + 1, edit_result(b).tape_serial);
}


/* Import zásahem: doprostřed, na konec (záporné at), do prázdného decku, nečitelný soubor. */
void test_import_in_poll(void)
{
    open_tape(3);
    uint32_t seq = cmt_request_import_on_tape(cmttmz_get_tape_serial(), write_mzt(1, "I", 0, 2), 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_IMPORT_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    ASSERT_NAMES("B000,I000,I001,B001,B002");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(3));
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_IMPORT, r.op);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);
    TEST_ASSERT_EQUAL_INT(1, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(5, r.block_count);
    TEST_ASSERT_FALSE(r.new_tape);

    /* na konec */
    seq = cmt_request_import_on_tape(r.tape_serial, write_mzt(2, "E", 0, 1), -1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("B000,I000,I001,B001,B002,E000");
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(5, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);

    /* nečitelný soubor: chyba, páska beze změny */
    gchar *missing = g_build_filename(g_get_tmp_dir(), "mztest_cmt_request_edit_missing.mzf", NULL);
    seq = cmt_request_import_on_tape(r.tape_serial, missing, 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Can't import"));
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    ASSERT_NAMES("B000,I000,I001,B001,B002,E000");

    /* prázdný deck: tape_serial 0 = jen do prázdného decku */
    cmt_eject();
    seq = cmt_request_import_on_tape(0, write_mzt(1, "N", 0, 2), -1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("N000,N001");
    r = edit_result(seq);
    TEST_ASSERT_TRUE(r.new_tape);
    TEST_ASSERT_EQUAL_INT(0, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);

    /* tape_serial 0, ale páska vložená: jiná páska */
    seq = cmt_request_import_on_tape(0, write_mzt(1, "X", 0, 1), 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    ASSERT_NAMES("N000,N001");

    /* prázdný deck a aktuální čítač výměn (okno vidělo prázdný deck) */
    cmt_eject();
    unsigned serial = cmttmz_get_tape_serial();
    seq = cmt_request_import_on_tape(serial, write_mzt(1, "M", 0, 1), -1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    ASSERT_NAMES("M000");
    TEST_ASSERT_TRUE(edit_result(seq).new_tape);

    /* prázdný deck, ale čítač z doby před vysunutím: jiná páska, deck zůstane prázdný */
    cmt_eject();
    seq = cmt_request_import_on_tape(serial, write_mzt(1, "Y", 0, 1), -1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    /* nečitelný soubor do prázdného decku: deck zůstane prázdný */
    seq = cmt_request_import_on_tape(0, missing, -1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_FALSE(edit_result(seq).new_tape);
    TEST_ASSERT_NOT_NULL(strstr(edit_result(seq).message, "Can't import"));

    /* pozice bez pásky: chyba "no tape in the deck", deck zůstane prázdný */
    seq = cmt_request_import_on_tape(0, write_mzt(1, "Z", 0, 1), 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING("no tape in the deck", edit_result(seq).message);
    g_free(missing);
}


/* Export celé pásky zásahem: soubor WAV, páska beze změny; chyba přípony. */
void test_export_tape_in_poll(void)
{
    open_tape(2);
    unsigned serial = cmttmz_get_tape_serial();
    const char *wav = tmp_file(2, "wav");
    uint32_t seq = cmt_request_export_tape_on_tape(serial, wav);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    TEST_ASSERT_FALSE(g_file_test(wav, G_FILE_TEST_EXISTS));
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_EXPORT_TAPE_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    GStatBuf st;
    TEST_ASSERT_EQUAL_INT(0, g_stat(wav, &st));
    TEST_ASSERT_TRUE(st.st_size > 44);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "Tape exported"));
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    cmt_request_export_tape_on_tape(cmttmz_get_tape_serial(), tmp_file(3, "tmz"));
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_FALSE(g_file_test(s_files[3], G_FILE_TEST_EXISTS));
}


/*
 * Kruh výsledků - 33 úprav v jednom pollu: výsledek první
 * přepíše 33. (pořadová čísla o 32 dál), getter u první vrátí false, nikdy
 * cizí výsledek; poslední 32 se najdou.
 */
void test_edit_result_ring_overflow(void)
{
    open_tape(2);
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t seqs[CMT_REQUEST_EDIT_RESULTS_MAX + 1];
    for (int i = 0; i < CMT_REQUEST_EDIT_RESULTS_MAX + 1; i++)
    {
        /* přesun na vlastní místo: úspěch beze změny, čítač pásky zůstane */
        seqs[i] = cmt_request_move_block_on_tape(serial, 0, 0);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, seqs[i]);
    };
    device_request_poll();
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res)) {}
    st_CMT_REQUEST_EDIT_RESULT r;
    device_state_lock();
    TEST_ASSERT_FALSE_MESSAGE(cmt_request_get_edit_result(seqs[0], &r), "the oldest result is overwritten");
    for (int i = 1; i < CMT_REQUEST_EDIT_RESULTS_MAX + 1; i++)
    {
        TEST_ASSERT_TRUE(cmt_request_get_edit_result(seqs[i], &r));
        TEST_ASSERT_EQUAL_UINT32(seqs[i], r.seq);
    };
    device_state_unlock();
}


/*
 * Přesun a import odmítnuté v pauze a při nahrávání
 * (text jádra), páska beze změny.
 */
void test_move_import_refused_in_pause_and_record(void)
{
    open_tape(3);
    unsigned serial = cmttmz_get_tape_serial();
    for (int state = 0; state < 2; state++)
    {
        if (state == 0)
        {
            g_cmt.paused = 0;
            cmt_play();
            cmt_pause(1);
            TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
        }
        else
        {
            TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
            TEST_ASSERT_TRUE(CMT_TEST_RECORD);
        };
        uint32_t s1 = cmt_request_move_block_on_tape(serial, 0, 1);
        uint32_t s2 = cmt_request_import_on_tape(serial, write_mzt(1, "I", 0, 1), 0);
        device_request_poll();
        TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
        TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
        const char *expect = state ? "The tape cannot be edited while recording"
                                   : "The tape can be edited only when the deck is stopped";
        TEST_ASSERT_EQUAL_STRING(expect, edit_result(s1).message);
        TEST_ASSERT_EQUAL_STRING(expect, edit_result(s2).message);
        cmt_stop();
        ASSERT_NAMES("B000,B001,B002");
    };
}


/* Počet bloků k smazání větší než páska se odmítne před alokací. */
void test_remove_count_capped_by_tape(void)
{
    open_tape(3);
    unsigned serial = cmttmz_get_tape_serial();
    /* 0-99 je jeden krátký rozsah, ale páska má 3 bloky: první neexistující je 3
     * (stejný text jako bez meze počtu, bez alokace podle počtu) */
    int ids[100];
    for (int i = 0; i < 100; i++) ids[i] = i;
    uint32_t seq = cmt_request_remove_blocks_on_tape(serial, ids, 100);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_STRING("Block 3 does not exist", edit_result(seq).message);
    ASSERT_NAMES("B000,B001,B002");
    /* rozsah 5-99: první neexistující je 5 */
    seq = cmt_request_remove_blocks_on_tape(serial, &ids[5], 95);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_STRING("Block 5 does not exist", edit_result(seq).message);
    /* stav decku se hlásí dřív než neexistující blok (pořadí jako bez meze) */
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    seq = cmt_request_remove_blocks_on_tape(serial, ids, 100);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_STRING("The tape can be edited only when the deck is stopped", edit_result(seq).message);
    cmt_stop();
    ASSERT_NAMES("B000,B001,B002");
}


/* Výsledek smazání nese první smazaný blok (číslo před smazáním), u chyby -1. */
void test_remove_result_removed_block(void)
{
    open_tape(4);
    const int one[] = {2};
    uint32_t seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), one, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_INT(2, r.removed_block);
    /* poslední blok: k výběru předposlední, smazaný je pořád ten poslední */
    const int last[] = {2};
    seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), last, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(2, r.removed_block);
    TEST_ASSERT_EQUAL_INT(1, r.select_block);
    const int bad[] = {9};
    seq = cmt_request_remove_blocks_on_tape(cmttmz_get_tape_serial(), bad, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_INT(-1, edit_result(seq).removed_block);
}


/*
 * Export celé pásky s čítačem výměn pásky. Jiný čítač (páska
 * mezitím upravená) = CMT_REQUEST_RC_TAPE_CHANGED, soubor nevznikne a zpráva
 * exportu zůstane; stejný čítač = export.
 */
void test_export_tape_on_tape(void)
{
    open_tape(2);
    unsigned serial = cmttmz_get_tape_serial();
    const char *wav = tmp_file(2, "wav");
    /* úprava pásky před provedením (MCP) */
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    device_state_unlock();
    TEST_ASSERT_NOT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    char before[256];
    g_strlcpy(before, cmttmz_get_export_message(), sizeof(before));
    uint32_t seq = cmt_request_export_tape_on_tape(serial, wav);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_EXPORT_TAPE_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, res.rc);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(wav, G_FILE_TEST_EXISTS), "nothing is written for another tape");
    TEST_ASSERT_EQUAL_STRING(before, cmttmz_get_export_message());
    /* vysunutá páska */
    unsigned now = cmttmz_get_tape_serial();
    cmt_eject();
    cmt_request_export_tape_on_tape(now, wav);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_FALSE(g_file_test(wav, G_FILE_TEST_EXISTS));
    /* tatáž páska */
    open_tape(2);
    cmt_request_export_tape_on_tape(cmttmz_get_tape_serial(), wav);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_export_message(), "Tape exported"));
}


/*
 * Zpráva exportu nepřepíše zprávu uložení, kterou okno
 * ještě nečetlo (Save a hned Export v jednom pollu).
 */
void test_export_keeps_save_message(void)
{
    open_tape(2);
    const char *tmz = tmp_file(3, "tmz");
    cmt_request_save(tmz);
    cmt_request_export_tape_on_tape(cmttmz_get_tape_serial(), tmp_file(2, "wav"));
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_save_message(), "Tape saved"), cmttmz_get_save_message());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_export_message(), "Tape exported"), cmttmz_get_export_message());
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_ranges_encode_decode);
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_remove_in_poll);
    RUN_TEST(test_remove_300_consecutive);
    RUN_TEST(test_tape_changed);
    RUN_TEST(test_rejected_while_replay_active);
    RUN_TEST(test_errors_with_message);
    RUN_TEST(test_move_in_poll);
    RUN_TEST(test_import_in_poll);
    RUN_TEST(test_export_tape_in_poll);
    RUN_TEST(test_edit_result_ring_overflow);
    RUN_TEST(test_move_import_refused_in_pause_and_record);
    RUN_TEST(test_remove_count_capped_by_tape);
    RUN_TEST(test_remove_result_removed_block);
    RUN_TEST(test_export_tape_on_tape);
    RUN_TEST(test_export_keeps_save_message);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
