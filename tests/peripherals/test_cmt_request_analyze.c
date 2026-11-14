/**
 * @file test_cmt_request_analyze.c
 * @brief Zásahy nástrojů nad páskou přes frontu zásahů zařízení (cmt_request.h).
 *
 * Testuje se:
 *   - kopie pulzů surového bloku zásahem (cmt_request_analysis_pulses(),
 *     cmt_request_take_pulses()): kopie až v device_request_poll(),
 *     vyzvednutí jen jednou, nevyzvednutou kopii nahradí další zásah,
 *     chyby (jiný blok než 0x18, jiná páska),
 *   - uplatnění analýzy zásahem (cmt_request_analysis_apply()): Replace
 *     s blokem souboru bajtově shodným s původním MZF, Add, blok k výběru,
 *     zastaralá analýza po jiné úpravě, mimo STOP; zásah drží odkazy
 *     (čte data až při provedení) a po odmítnutí v replay data nečte;
 *     varování o taktu nového bloku za blokem 0x43 s jiným taktem v message,

 *   - zápis informací o pásce zásahem (cmt_request_tape_info_set()): kopie
 *     formuláře při zařazení, beze změny, varování, jiná páska, vypršelá kopie,
 *   - převod profilu zásahem (cmt_request_convert_block_on_tape()): převod,
 *     beze změny na tentýž profil, neznámý profil, jiná páska,
 *   - odmítnutí všech čtyř zásahů při záznamu i přehrávání vstupů
 *     (DEVICE_REQ_REJECTED_REPLAY, bez výsledku úpravy, páska beze změny),
 *   - neplatné argumenty se nezařadí.
 *
 * Surový blok vzniká stejně jako v e2e testu MCP: blok MZF z MZT se
 * exportuje do WAV (cmttmz_export_block()) a WAV se importuje na konec pásky
 * (cmt_tape_import_file()) jako jeden blok 0x18.
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
#include "hw-generic/cmt/cmt_tmz_analyze.h"
#include "hw-generic/cmt/cmt_tmz_meta.h"
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "state_rev.h"

/** @brief Velikost těla MZF testovací pásky. */
#define TEST_BODY_SIZE 64

/** @brief Počet dočasných souborů testu. */
#define TEST_FILES 6

/** @brief Dočasné soubory testu (maže tearDown). */
static gchar *s_files[TEST_FILES] = {NULL, NULL, NULL, NULL, NULL, NULL};


/**
 * @brief Vrátí cestu dočasného souboru (slot 0 .. TEST_FILES - 1) s příponou.
 * @param slot Slot. @param ext Přípona bez tečky.
 * @return Cesta (vlastní ji test, uvolní tearDown).
 */
static const char *tmp_file(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_request_analyze_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    g_free(s_files[slot]);
    s_files[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_files[slot];
}


/**
 * @brief Zapíše MZT s @p count bloky pojmenovanými "B000", "B001", ...
 * @param slot Slot dočasného souboru. @param count Počet bloků.
 * @return Cesta k souboru.
 */
static const char *write_mzt(int slot, int count)
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
        int n = snprintf(name, sizeof(name), "B%03d", b);
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


/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_count(void)
{
    if (!CMT_TEST_FILLED) return -1;
    return cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext));
}


/**
 * @brief Jméno bloku vložené pásky ("" bez jména).
 * @param i Číslo bloku. @return Jméno (platí do další změny pásky).
 */
static const char *block_name(int i)
{
    const char *n = cmtext_container_get_block_fname(cmtext_get_container(g_cmt.ext), i);
    return n ? n : "";
}


/**
 * @brief Načte celý soubor.
 * @param path Cesta. @param[out] len Délka.
 * @return Data (g_free).
 */
static gchar *read_file(const char *path, gsize *len)
{
    gchar *data = NULL;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(path, &data, len, NULL), path);
    return data;
}


/**
 * @brief Páska "B000, B001, surový blok 0x18 se signálem B000" (STOP), MZF bloku 0 ve slotu 1.
 *
 * Blok 0 se exportuje jako MZF (slot 1, vzor pro porovnání) a jako WAV
 * (slot 2); WAV se importuje na konec pásky jako blok 2. Páska je pak
 * Modified a blok 2 neuložený (na test to nemá vliv).
 */
static void open_raw_tape(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)write_mzt(0, 2)));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(0, tmp_file(1, "mzf")));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(0, tmp_file(2, "wav")));
    uint32_t imported = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_import_file(s_files[2], 2, &imported));
    TEST_ASSERT_EQUAL_UINT32(1, imported);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_wav(2));
}


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


/**
 * @brief Kopie pulzů bloku zásahem (selže bez kopie).
 * @param block_id Blok. @return Kopie (uvolnit cmttmz_pulses_free()).
 */
static st_CMTTMZ_PULSES *pulses_via_request(int block_id)
{
    uint32_t seq = cmt_request_analysis_pulses(cmttmz_get_tape_serial(), block_id);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    st_CMTTMZ_PULSES *p = cmt_request_take_pulses(seq);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}


/**
 * @brief Pole příznaků "přidat každý řádek, který jde přidat".
 * @param a Analýza. @param[out] addable Počet takových řádků.
 * @return Pole (g_free), aspoň 1 položka.
 */
static uint8_t *use_addable(const st_CMTTMZ_ANALYSIS *a, uint32_t *addable)
{
    uint32_t n = cmttmz_analysis_row_count(a);
    uint8_t *use = g_new0(uint8_t, n ? n : 1);
    *addable = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        if (r->result == CMTTMZ_ANALYZE_ADDABLE || r->result == CMTTMZ_ANALYZE_ADDABLE_REPAIRED)
        {
            use[i] = 1;
            (*addable)++;
        };
    };
    return use;
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
    for (int i = 0; i < TEST_FILES; i++)
    {
        if (s_files[i]) g_remove(s_files[i]);
        g_free(s_files[i]);
        s_files[i] = NULL;
    };
}


/* Neplatné argumenty se nezařadí. */
void test_invalid_args_not_queued(void)
{
    st_CMTTMZ_TAPE_INFO info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_analysis_pulses(1, -1));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_analysis_apply(NULL, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_tape_info_set(1, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, -1, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, 0, NULL, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, 0, "", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, 0, "ROM-800-NOMINAL", (en_CMTSPEED)-1));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, 0, "ROM-800-NOMINAL",
                                                                  (en_CMTSPEED)(CMTSPEED_CUSTOM + 1)));
    char longid[DEVICE_REQUEST_STR_MAX + 8];
    memset(longid, 'A', sizeof(longid) - 1);
    longid[sizeof(longid) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_convert_block_on_tape(1, 0, longid, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
    TEST_ASSERT_NULL(cmt_request_take_pulses(0));
}


/* Kopie pulzů: až v poll, vyzvednutí jen jednou, výsledek úpravy nese blok a počet pulzů. */
void test_pulses_request(void)
{
    open_raw_tape();
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t seq = cmt_request_analysis_pulses(serial, 2);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    TEST_ASSERT_NULL(cmt_request_take_pulses(seq));
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_ANALYSIS_PULSES_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_ANALYSIS_PULSES, r.op);
    TEST_ASSERT_EQUAL_INT(0, r.rc);
    TEST_ASSERT_EQUAL_INT(2, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(serial, r.tape_serial);
    TEST_ASSERT_EQUAL_UINT32(3, r.block_count);
    st_CMTTMZ_PULSES *p = cmt_request_take_pulses(seq);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(2, p->block_id);
    TEST_ASSERT_EQUAL_UINT(serial, p->tape_serial);
    TEST_ASSERT_TRUE(p->count > 1000);
    TEST_ASSERT_EQUAL_UINT32(p->count, r.count);
    TEST_ASSERT_EQUAL_INT(1, p->wav_type);
    /* vyzvednout jde jen jednou */
    TEST_ASSERT_NULL(cmt_request_take_pulses(seq));
    cmttmz_pulses_free(p);
    /* páska se nezměnila */
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
}


/* Nevyzvednutou kopii nahradí další zásah kopie pulzů; chyby kopie. */
void test_pulses_unclaimed_and_errors(void)
{
    open_raw_tape();
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t a = cmt_request_analysis_pulses(serial, 2);
    uint32_t b = cmt_request_analysis_pulses(serial, 2);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_NULL(cmt_request_take_pulses(a));
    st_CMTTMZ_PULSES *p = cmt_request_take_pulses(b);
    TEST_ASSERT_NOT_NULL(p);
    cmttmz_pulses_free(p);

    /* blok 0x46 */
    uint32_t seq = cmt_request_analysis_pulses(serial, 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(-1, r.rc);
    TEST_ASSERT_EQUAL_INT(-1, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_STRING("Block 0 is not a raw signal block (CSW recording)", r.message);
    TEST_ASSERT_NULL(cmt_request_take_pulses(seq));

    /* jiná páska */
    seq = cmt_request_analysis_pulses(serial + 1, 2);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, edit_result(seq).rc);
    TEST_ASSERT_NULL(cmt_request_take_pulses(seq));
}


/* Replace zásahem: blok souboru na místě surového, MZF bajtově shodné, blok k výběru. */
void test_apply_replace(void)
{
    open_raw_tape();
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_analysis_error(a));
    uint32_t addable = 0;
    uint8_t *use = use_addable(a, &addable);
    TEST_ASSERT_EQUAL_UINT32(1, addable);

    uint32_t seq = cmt_request_analysis_apply(p, a, use, 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_ANALYSIS_APPLY_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_ANALYSIS_APPLY, r.op);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)tape_count(), r.block_count);
    TEST_ASSERT_TRUE(r.select_block >= 2);
    TEST_ASSERT_EQUAL_STRING("B000", block_name(r.select_block));
    TEST_ASSERT_EQUAL_UINT(cmttmz_get_tape_serial(), r.tape_serial);

    /* MZF nového bloku bajtově shodné s původním */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_export_block(r.select_block, tmp_file(3, "mzf")));
    gsize la = 0, lb = 0;
    gchar *da = read_file(s_files[1], &la);
    gchar *db = read_file(s_files[3], &lb);
    TEST_ASSERT_EQUAL_size_t(la, lb);
    TEST_ASSERT_EQUAL_MEMORY(da, db, la);
    g_free(da);
    g_free(db);

    /* tatáž analýza podruhé: páska se mezitím změnila */
    seq = cmt_request_analysis_apply(p, a, use, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, edit_result(seq).rc);

    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/*
 * Add zásahem; zásah drží odkazy (výběr změněný po zařazení platí), blok
 * k výběru je za surovým blokem; výběr bez řádku = úspěch beze změny.
 */
void test_apply_add_holds_references(void)
{
    open_raw_tape();
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    uint32_t addable = 0;
    uint8_t *want = use_addable(a, &addable);
    TEST_ASSERT_EQUAL_UINT32(1, addable);
    uint32_t n = cmttmz_analysis_row_count(a);
    uint8_t *use = g_new0(uint8_t, n);

    /* nic vybraného: úspěch beze změny */
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t seq = cmt_request_analysis_apply(p, a, use, 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_INT(-1, r.select_block);
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());

    /* zařadit s prázdným výběrem, výběr doplnit až před provedením */
    seq = cmt_request_analysis_apply(p, a, use, 0);
    memcpy(use, want, n);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_INT(3, r.select_block);
    TEST_ASSERT_EQUAL_INT(4, tape_count());
    TEST_ASSERT_EQUAL_STRING("B000", block_name(3));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_wav(2));
    /* páska bez bloku 0x43: žádné varování o taktu */
    TEST_ASSERT_EQUAL_STRING("", r.message);

    g_free(want);
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/*
 * Blok 0x43 s taktem 4 MHz na začátku pásky (před surovým
 * blokem) - výsledek zásahu Add nese varování o taktu nového bloku souboru
 * (okno ho ukáže v řádku zpráv).
 */
void test_apply_clock_warning(void)
{
    open_raw_tape();
    st_TZX_BLOCK *mi = tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ800, 4000000u, 0);
    TEST_ASSERT_NOT_NULL(mi);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(0, mi, &sp, 1));
    mi->data = NULL; /* data převzala páska */
    tmz_block_free(mi);
    TEST_ASSERT_EQUAL_INT(4, tape_count());
    st_CMTTMZ_PULSES *p = pulses_via_request(3);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    uint32_t addable = 0;
    uint8_t *use = use_addable(a, &addable);
    TEST_ASSERT_EQUAL_UINT32(1, addable);
    uint32_t seq = cmt_request_analysis_apply(p, a, use, 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_STRING(CMTTMZ_CLOCK_WARNING, r.message);
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/* Uplatnění mimo STOP: chyba s textem jádra, páska beze změny; jiná úprava mezi kopií a uplatněním. */
void test_apply_refused(void)
{
    open_raw_tape();
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    uint32_t addable = 0;
    uint8_t *use = use_addable(a, &addable);
    unsigned serial = cmttmz_get_tape_serial();

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    uint32_t seq = cmt_request_analysis_apply(p, a, use, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_STRING("The tape can be edited only when the deck is stopped", edit_result(seq).message);
    cmt_stop();
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(3, tape_count());

    /* jiná úprava (přesun) mezi kopií a uplatněním: jiná páska */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    seq = cmt_request_analysis_apply(p, a, use, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_EQUAL_INT(3, tape_count());

    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/* Zápis informací o pásce: kopie formuláře při zařazení, změna, beze změny, varování, jiná páska. */
void test_tape_info_set(void)
{
    open_raw_tape();
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    g_strlcpy(info.title, "Test tape", sizeof(info.title));
    uint32_t seq = cmt_request_tape_info_set(info.tape_serial, &info);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    /* formulář se zkopíroval při zařazení */
    g_strlcpy(info.title, "Changed later", sizeof(info.title));
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_TAPE_INFO_SET_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_TAPE_INFO, r.op);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_STRING("", r.message);
    TEST_ASSERT_EQUAL_INT(-1, r.select_block);
    TEST_ASSERT_EQUAL_UINT32(4, r.block_count);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("Test tape", info.title);
    TEST_ASSERT_EQUAL_UINT(r.tape_serial, info.tape_serial);

    /* beze změny */
    seq = cmt_request_tape_info_set(info.tape_serial, &info);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_UINT(info.tape_serial, cmttmz_get_tape_serial());

    /* varování (znak mimo ISO 8859-1) */
    g_strlcpy(info.author, "\xC4\x8C" "apek", sizeof(info.author));
    seq = cmt_request_tape_info_set(info.tape_serial, &info);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Characters outside ISO 8859-1"));

    /* jiná páska (formulář před poslední úpravou) */
    unsigned now = cmttmz_get_tape_serial();
    seq = cmt_request_tape_info_set(info.tape_serial, &info);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, edit_result(seq).rc);
    TEST_ASSERT_EQUAL_UINT(now, cmttmz_get_tape_serial());
}


/* Čekající kopie formuláře: novější zápisy o CMT_REQUEST_TAPE_INFO_SLOTS dál ji uvolní (vypršení). */
void test_tape_info_expired(void)
{
    open_raw_tape();
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    unsigned serial = cmttmz_get_tape_serial();
    /* první zápis odmítnutý v replay: jeho kopie zůstane, dokud ji nevytlačí další */
    g_replay_mode = REPLAY_MODE_RECORDING;
    uint32_t first = cmt_request_tape_info_set(serial, &info);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
    g_replay_mode = REPLAY_MODE_OFF;
    (void)first;
    /* CMT_REQUEST_TAPE_INFO_SLOTS + 1 zápisů v jednom pollu: nejstarší vypršel */
    uint32_t seqs[CMT_REQUEST_TAPE_INFO_SLOTS + 1];
    for (int i = 0; i < CMT_REQUEST_TAPE_INFO_SLOTS + 1; i++)
        seqs[i] = cmt_request_tape_info_set(serial, &info);
    device_request_poll();
    for (int i = 0; i < CMT_REQUEST_TAPE_INFO_SLOTS + 1; i++)
        (void)take_one();
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seqs[0]);
    TEST_ASSERT_EQUAL_INT(-1, r.rc);
    TEST_ASSERT_EQUAL_STRING("The tape information request expired", r.message);
    for (int i = 1; i < CMT_REQUEST_TAPE_INFO_SLOTS + 1; i++)
    {
        r = edit_result(seqs[i]);
        TEST_ASSERT_EQUAL_INT(0, r.rc);
        TEST_ASSERT_EQUAL_UINT32(0, r.count);
    };
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
}


/* Převod profilu zásahem: změna, tentýž profil beze změny, neznámý profil, jiná páska. */
void test_convert_block(void)
{
    open_raw_tape();
    char id[64];
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_profile(0, id, sizeof(id)));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("MZ800EMU-CMT", id);

    unsigned serial = cmttmz_get_tape_serial();
    uint32_t seq = cmt_request_convert_block_on_tape(serial, 0, "ROM-800-NOMINAL", CMTSPEED_NONE);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_CONVERT_BLOCK_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(0, res.rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_EDIT_CONVERT, r.op);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_INT(0, r.select_block);
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_profile(0, id, sizeof(id)));
    device_state_unlock();
    TEST_ASSERT_EQUAL_STRING("ROM-800-NOMINAL", id);

    /* tentýž profil bez rychlosti: beze změny */
    serial = cmttmz_get_tape_serial();
    seq = cmt_request_convert_block_on_tape(serial, 0, "ROM-800-NOMINAL", CMTSPEED_NONE);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_STRING("", r.message);
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());

    /* neznámý profil */
    seq = cmt_request_convert_block_on_tape(serial, 0, "NO-SUCH-PROFILE", CMTSPEED_NONE);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_STRING("Unknown profile: NO-SUCH-PROFILE", edit_result(seq).message);

    /* jiná páska */
    seq = cmt_request_convert_block_on_tape(serial + 5, 0, "MZ800EMU-CMT", CMTSPEED_NONE);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMT_REQUEST_RC_TAPE_CHANGED, take_one().rc);
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
}


/**
 * @brief Zapíše zásahem autora se znakem mimo ISO 8859-1 (varování) a ověří varování ve výsledku.
 */
static void make_info_warning(void)
{
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    /* pokaždé jiný text, aby se páska skutečně změnila */
    static int n = 0;
    snprintf(info.author, sizeof(info.author), "\xC5\x98%d", ++n);
    uint32_t seq = cmt_request_tape_info_set(info.tape_serial, &info);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_NOT_NULL(strstr(edit_result(seq).message, "Characters outside ISO 8859-1"));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "Characters outside ISO 8859-1"));
}


/*
 * Kanál varování: varování zápisu informací
 * nepatří žádné pozdější operaci - úspěšná i odmítnutá úprava, změna
 * rychlosti bloku, vysunutí a otevření pásky ho vymažou; další zásah bez
 * varování má ve výsledku prázdnou zprávu.
 */
void test_edit_warning_does_not_leak(void)
{
    open_raw_tape();

    /* úspěšný převod bez varování: výsledek bez zprávy (kontrola výsledku úpravy; převod
     * maže varování sám, opravu kanálu v jádře pojišťují přímé kontroly níže) */
    make_info_warning();
    uint32_t seq = cmt_request_convert_block_on_tape(cmttmz_get_tape_serial(), 1, "MZ800EMU-CMT", CMTSPEED_2_1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_STRING("", r.message);

    /* přesun bloku (úprava pásky v jádře) */
    make_info_warning();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(0, 1));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());

    /* odmítnutá úprava (PLAY) */
    make_info_warning();
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_move_block(0, 1));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    cmt_stop();

    /* změna rychlosti bloku */
    make_info_warning();
    /* blok 0 je teď Archive Info (0x32): blok MZF najít podle jména */
    int mzf = -1;
    for (int i = 0; i < tape_count(); i++)
        if (!strcmp(block_name(i), "B000")) mzf = i;
    TEST_ASSERT_TRUE(mzf >= 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(mzf, CMTSPEED_3_1, true, NULL));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());

    /* vysunutí a otevření pásky */
    make_info_warning();
    cmt_eject();
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    open_raw_tape();
    make_info_warning();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)s_files[0]));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
}


/* Při záznamu i přehrávání vstupů: všechny čtyři zásahy odmítnuté, bez výsledku úpravy, páska beze změny. */
void test_refused_in_replay(void)
{
    open_raw_tape();
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    uint32_t addable = 0;
    uint8_t *use = use_addable(a, &addable);
    st_CMTTMZ_TAPE_INFO info;
    device_state_lock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    device_state_unlock();
    g_strlcpy(info.title, "Replay", sizeof(info.title));
    unsigned serial = cmttmz_get_tape_serial();

    const en_REPLAY_MODE modes[] = {REPLAY_MODE_RECORDING, REPLAY_MODE_PLAYING};
    for (int m = 0; m < 2; m++)
    {
        g_replay_mode = modes[m];
        uint32_t s[4];
        s[0] = cmt_request_analysis_pulses(serial, 2);
        s[1] = cmt_request_analysis_apply(p, a, use, 1);
        s[2] = cmt_request_tape_info_set(serial, &info);
        s[3] = cmt_request_convert_block_on_tape(serial, 0, "ROM-800-NOMINAL", CMTSPEED_NONE);
        device_request_poll();
        for (int i = 0; i < 4; i++)
            TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
        g_replay_mode = REPLAY_MODE_OFF;
        st_CMT_REQUEST_EDIT_RESULT r;
        device_state_lock();
        for (int i = 0; i < 4; i++)
            TEST_ASSERT_FALSE(cmt_request_get_edit_result(s[i], &r));
        device_state_unlock();
        TEST_ASSERT_NULL(cmt_request_take_pulses(s[0]));
        TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
        TEST_ASSERT_EQUAL_INT(3, tape_count());
    };

    /* po výsledku zásahu (i odmítnutého) smí volající data uvolnit */
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
}


/*
 * Zrušení zásahu uplatnění: před provedením vrátí true, data
 * jdou hned uvolnit (zásah je nepřečte a skončí chybou, páska beze změny);
 * po výsledku, dvakrát, s 0 a s číslem jiného zásahu false.
 */
void test_apply_cancel(void)
{
    open_raw_tape();
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    uint32_t addable = 0;
    uint8_t *use = use_addable(a, &addable);
    unsigned serial = cmttmz_get_tape_serial();

    uint32_t seq = cmt_request_analysis_apply(p, a, use, 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    TEST_ASSERT_TRUE(cmt_request_analysis_cancel(seq));
    TEST_ASSERT_FALSE_MESSAGE(cmt_request_analysis_cancel(seq), "second cancel");
    /* data volající hned uvolní; zásah na ně nesmí sáhnout */
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
    device_request_poll();
    st_DEVICE_REQ_RESULT res = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_ANALYSIS_APPLY_NAME, res.name);
    TEST_ASSERT_EQUAL_INT(-1, res.rc);
    st_CMT_REQUEST_EDIT_RESULT r = edit_result(seq);
    TEST_ASSERT_EQUAL_STRING("The analysis request was cancelled", r.message);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(3, tape_count());

    /* po výsledku zrušit nejde; 0 a číslo jiného zásahu nejsou zásah uplatnění */
    p = pulses_via_request(2);
    a = cmttmz_analyze_pulses(p, NULL);
    use = use_addable(a, &addable);
    seq = cmt_request_analysis_apply(p, a, use, 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_FALSE(cmt_request_analysis_cancel(seq));
    TEST_ASSERT_FALSE(cmt_request_analysis_cancel(0));
    uint32_t other = cmt_request_analysis_pulses(cmttmz_get_tape_serial(), 2);
    TEST_ASSERT_FALSE(cmt_request_analysis_cancel(other));
    device_request_poll();
    (void)take_one();
    cmttmz_pulses_free(cmt_request_take_pulses(other));
    TEST_ASSERT_EQUAL_INT(4, tape_count());
    /* páska se změnila: nová kopie a analýza pro další část */
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
    p = pulses_via_request(2);
    a = cmttmz_analyze_pulses(p, NULL);
    use = use_addable(a, &addable);

    /* registr má CMT_REQUEST_EDIT_RESULTS_MAX míst: zařazení na místo neprovedeného
     * zásahu ten zásah zneplatní (skončí chybou, data nepřečte) */
    uint32_t seqs[CMT_REQUEST_EDIT_RESULTS_MAX + 1];
    for (int i = 0; i <= CMT_REQUEST_EDIT_RESULTS_MAX; i++)
    {
        seqs[i] = cmt_request_analysis_apply(p, a, use, 0);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, seqs[i]);
    };
    TEST_ASSERT_FALSE_MESSAGE(cmt_request_analysis_cancel(seqs[0]), "slot taken by a newer request");
    for (int i = 1; i < CMT_REQUEST_EDIT_RESULTS_MAX; i++)
        TEST_ASSERT_TRUE(cmt_request_analysis_cancel(seqs[i]));
    /* nejstarší (zneplatněný) a zrušené skončí chybou, poslední (nezrušený) přidá soubor */
    device_request_poll();
    for (int i = 0; i <= CMT_REQUEST_EDIT_RESULTS_MAX; i++)
    {
        st_DEVICE_REQ_RESULT rr;
        if (!device_request_take_result(&rr)) break;
    };
    TEST_ASSERT_EQUAL_STRING("The analysis request was cancelled", edit_result(seqs[1]).message);
    TEST_ASSERT_EQUAL_INT(0, edit_result(seqs[CMT_REQUEST_EDIT_RESULTS_MAX]).rc);
    TEST_ASSERT_EQUAL_UINT32(1, edit_result(seqs[CMT_REQUEST_EDIT_RESULTS_MAX]).count);
    TEST_ASSERT_EQUAL_INT(5, tape_count());
    g_free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/* Kopie pulzů pro okno stav stroje nemění: nezvedá revizi stavu. */
void test_pulses_no_state_rev(void)
{
    open_raw_tape();
    const uint64_t rev = g_state_rev;
    st_CMTTMZ_PULSES *p = pulses_via_request(2);
    TEST_ASSERT_EQUAL_UINT64(rev, g_state_rev);
    cmttmz_pulses_free(p);
}


/*
 * MCP analýza (dbgapi DBGAPI_CMD_CMT_TAPE_ANALYZE_APPLY) běží bez zámku stavu
 * zařízení: háček zaznamená stav zámku těsně před analýzou;
 * kopie a uplatnění proběhnou a po příkazu zámek nikdo nedrží.
 */
void test_mcp_analysis_without_device_lock(void)
{
    open_raw_tape();
    dbgapi_test_cmt_analyze_lock_reset();
    st_DBGAPI_CMT_TAPE_ANALYZE_PARAM *prm = g_new0(st_DBGAPI_CMT_TAPE_ANALYZE_PARAM, 1);
    prm->block_id = 2;
    prm->tolerance = 0.20;
    prm->apply = DBGAPI_CMT_ANALYZE_APPLY_REPLACE;
    st_DBGAPI_CMDRQ rq;
    memset(&rq, 0, sizeof(rq));
    rq.cmd = DBGAPI_CMD_CMT_TAPE_ANALYZE_APPLY;
    rq.cmd_origin = DBGAPI_CMD_ORIGIN_MCP;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = prm;
    dbgapi_emu_dispatch(&rq);
    TEST_ASSERT_TRUE_MESSAGE(rq.success, prm->out_message);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dbgapi_test_cmt_analyze_lock_state(), "analysis ran without the device state lock");
    TEST_ASSERT_FALSE(device_state_lock_held());
    TEST_ASSERT_EQUAL_UINT32(1, prm->out_applied);
    TEST_ASSERT_TRUE(prm->out_first_added >= 2);
    TEST_ASSERT_EQUAL_STRING("B000", block_name(prm->out_first_added));
    TEST_ASSERT_EQUAL_INT(tape_count(), prm->out_count);
    g_free(prm->out_rows);
    g_free(prm);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_pulses_request);
    RUN_TEST(test_pulses_unclaimed_and_errors);
    RUN_TEST(test_apply_replace);
    RUN_TEST(test_apply_add_holds_references);
    RUN_TEST(test_apply_clock_warning);
    RUN_TEST(test_apply_refused);
    RUN_TEST(test_tape_info_set);
    RUN_TEST(test_tape_info_expired);
    RUN_TEST(test_convert_block);
    RUN_TEST(test_refused_in_replay);
    RUN_TEST(test_edit_warning_does_not_leak);
    RUN_TEST(test_apply_cancel);
    RUN_TEST(test_pulses_no_state_rev);
    RUN_TEST(test_mcp_analysis_without_device_lock);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
