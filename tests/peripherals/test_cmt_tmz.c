/**
 * @file test_cmt_tmz.c
 * @brief Testy rozšíření Virtual CMT cmt_tmz (páska jako TMZ přehrávaná přes tmz_player).
 *
 * Testuje se:
 *   - index pásky: kontejner SIMPLE_TAPE, block_id = index bloku v souboru,
 *     řídicí blok (0x30) má typ CONTROL a cb_next_block / cb_previous_block
 *     ho přeskočí,
 *   - otevření bloku: stream s frekvencí GDGCLK_BASE a nenulovou délkou,
 *     pause_after 0 (pauzu nese stream),
 *   - páska bez bloku se signálem (0x30 + 0x42): otevření projde, blok
 *     otevřít nejde, cmt_play() zůstane ve STOP,
 *   - životnost: vysunutí během pauzy za blokem při PLAY uvolní blok,
 *     kontejner i soubor, další páska jde otevřít a přehrát,
 *   - registr přípon: tmz a tzx patří mezi přehrávatelné,
 *   - dotazy cmttmz_block_is_mzf() a cmttmz_block_get_mzf(),
 *   - blok MZF, který player odmítne (0x41 formát SINCLAIR): next/previous
 *     ho přeskočí, přehrávání pokračuje dalším blokem a položka indexu se
 *     nezmění,
 *   - index pásky .tap: TAPHEADER a TAPDATA se jmény jako dřív,
 *   - postupné přehrávání přes player: zastavení pásky (Pause 0,
 *     0x2A) = PLAY v pauze na bloku za příkazem, PC4 = 0; smyčka Loop
 *     Start / End přehraje blok vícekrát; skok na sebe skončí ve STOP;
 *     autorewind po konci pásky otevře první blok,
 *   - rychlost bloků: výchozí rychlost přestaví bloky DEFAULT (stream
 *     poloviční při 2:1, MZF beze změny), per-blok rychlost a vlastní pulzy,
 *     odmítnutí bloku s rychlostí ze souboru, TAP 2:1, blok MZ800EMU-CMT
 *     z TMZ, dotaz cmttmz_block_speed_changeable(),
 *   - pozice v bloku: cmt_tape_get_position() po 2 s a pauze,
 *     cmt_tape_set_position() zpět na začátek dá stejné hrany signálu,
 *     pozice ve STOP platí pro další PLAY, ořez za koncem, odmítnutí
 *     (mimo rozsah, CONTROL, PLAY bez pauzy, nahrávání, bez pásky),
 *   - snapshot pásky: stav playeru (kopie, nastavení, odmítnutí
 *     hodnot mimo soubor), snapshot ve STOP, v pauze a při PLAY obnoví
 *     pásku, blok, pozici a rychlosti bloků; postup smyčkou po načtení
 *     snapshotu (checkpoint záznamu vstupů) pokračuje jako bez něj;
 *     chybějící soubor a soubor, který neodpovídá snapshotu; režim
 *     záznamu vstupů použije vloženou kopii pásky a soubor neotevírá;
 *     ověření stavu playeru (remaining smyček, offsety volání, otevřený
 *     blok, takt CPU), blok DEFAULT s jinou výchozí rychlostí (SET), páska
 *     bez otevřeného bloku, načtení bez odpálení BP eventů CMT,
 *   - dvojklik / Play from here na aktuální blok v pauze uživatele: blok
 *     znovu od začátku, pauza zůstane (jako na jiném bloku).
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
#ifndef _WIN32
#include <unistd.h>     /* geteuid() - test souboru jen pro čtení pod rootem */
#endif

#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmt_tmz_record.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/tmz/tmz_mzt.h"
#include "device_request.h"
#include "libs/wav_analyzer/wav_analyzer.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/cmt/cmthack.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "debugger/breakpoints.h"
#include "debugger/bp_event.h"
#include "debugger/bp_vars.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_mzfile.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Délka těla testovacích MZF v bajtech. */
#define TEST_BODY_SIZE 64

/** @brief Dočasné soubory pásek (maže tearDown). */
static char *s_paths[2] = {NULL, NULL};

/**
 * @brief Vrátí cestu dočasného souboru s danou příponou (slot 0 nebo 1).
 *
 * @param slot Číslo slotu (0, 1).
 * @param ext Přípona bez tečky.
 * @return Cesta (vlastní ji test, uvolní tearDown).
 */
static const char *tmp_path(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_tmz_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    g_free(s_paths[slot]);
    s_paths[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_paths[slot];
}

/**
 * @brief Vyplní hlavičku MZF (typ 01h, tělo TEST_BODY_SIZE B na 1200h).
 *
 * @param hdr Hlavička v pořadí hostitele.
 * @param name Jméno (nejvýš 16 znaků).
 */
static void make_mzf_header(st_MZF_HEADER *hdr, const char *name)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = 0x01;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, strlen(name));
    hdr->fsize = TEST_BODY_SIZE;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
}

/**
 * @brief Přidá do souboru blok 0x46 (MZ800EMU-CMT, 1:1) s MZF daného jména.
 *
 * @param f Soubor.
 * @param name Jméno MZF.
 * @param pause_ms Pauza za blokem v ms.
 */
static void add_mzf_block(st_TZX_FILE *f, const char *name, uint16_t pause_ms)
{
    st_MZF_HEADER hdr;
    make_mzf_header(&hdr, name);
    uint8_t body[TEST_BODY_SIZE];
    for (int i = 0; i < TEST_BODY_SIZE; i++)
        body[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, TEST_BODY_SIZE, CMTSPEED_1_1, NULL, pause_ms);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/**
 * @brief Přidá do souboru blok 0x30 (Text Description).
 * @param f Soubor. @param text Text.
 */
static void add_text_block(st_TZX_FILE *f, const char *text)
{
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description(text, &b));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/**
 * @brief Vytvoří prázdný soubor s hlavičkou TMZ.
 * @return Soubor (uvolnit tzx_free()).
 */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/**
 * @brief Uloží soubor jako .tmz do slotu a uvolní ho.
 *
 * @param f Soubor (funkce ho uvolní).
 * @param slot Slot dočasného souboru.
 * @return Cesta uloženého souboru.
 */
static const char *save_tmz(st_TZX_FILE *f, int slot)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    const char *path = tmp_path(slot, "tmz");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    return path;
}

/**
 * @brief Uloží MZF (hlavička + tělo) jako .mzf do slotu.
 * @param slot Slot. @param name Jméno MZF.
 * @return Cesta souboru.
 */
static const char *save_mzf(int slot, const char *name)
{
    uint8_t raw[128 + TEST_BODY_SIZE];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x01;
    memset(&raw[1], 0x0D, 17);
    memcpy(&raw[1], name, strlen(name));
    raw[0x12] = TEST_BODY_SIZE;
    raw[0x15] = 0x12;
    raw[0x17] = 0x12;
    for (int i = 0; i < TEST_BODY_SIZE; i++)
        raw[128 + i] = (uint8_t)((i * 37 + 11) & 0xFF);
    const char *path = tmp_path(slot, "mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, sizeof(raw), NULL));
    return path;
}

/**
 * @brief Posune GDG čas o daný počet taktů.
 *
 * Takty se přičtou k ticks aktuálního snímku; gdg_get_total_ticks() je
 * jen sčítá se snímky, smyčka snímků v testu neběží.
 *
 * @param ticks Počet taktů GDG (součet za test se vejde do 32 bitů).
 */
static void advance_ticks(uint64_t ticks)
{
    g_gdg.total_elapsed.ticks += (unsigned)ticks;
}

/** @brief Hodnota g_cmt.autorewind před testem (obnovuje tearDown). */
static int s_saved_autorewind;

/** @brief Výchozí rychlost a vlastní pulzy Virtual CMT před testem (obnovuje tearDown). */
static en_CMTSPEED s_saved_speed;
static st_MZTAPE_PULSES_LENGTH s_saved_pulses;

/** @brief Příznak nainstalovaného CMT hacku před testem (obnovuje tearDown i po selhání testu). */
static unsigned s_saved_cmthack;

void setUp(void)
{
    /* advance_ticks() přičítá jen k 32bitovým ticks snímku: přenést celé
     * snímky do screens, aby součet přes všechny testy nepřetekl (čas běží
     * dál spojitě) */
    g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
    g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
    s_saved_autorewind = g_cmt.autorewind;
    g_cmt.autorewind = 0;
    s_saved_cmthack = g_cmthack.load_patch_installed;
    s_saved_speed = g_cmt.mz_cmtspeed;
    s_saved_pulses = g_cmt.mz_custom_pulses;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
}

void tearDown(void)
{
    cmttmz_rec_test_set_limits(0, 0);
    cmttmz_test_set_append_hook(NULL);
    cmttmz_test_fail_before_replace(0);
    g_cmt.autorewind = s_saved_autorewind;
    g_cmthack.load_patch_installed = s_saved_cmthack;
    g_emulator.paused = false;
    snap_cmt_set_keep_inserted_tape(false);
    snap_cmt_test_set_embed_limit(0);
    snap_cmt_test_corrupt_image(0);
    snap_cmt_test_omit_tape(false);
    /* BP z testu tichého načtení nesmí přežít ani po selhání asertace */
    breakpoints_clear_all();
    bp_vars_clear_storage();
    cmt_eject();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.mz_custom_pulses = s_saved_pulses;
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
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* Přípony tmz a tzx patří mezi přehrávatelné. */
void test_registry_extensions(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_TRUE(cmtext_is_playable_filename("a.tmz"));
    TEST_ASSERT_TRUE(cmtext_is_playable_filename("a.TZX"));
    TEST_ASSERT_TRUE(cmtext_is_playable_filename("a.mzt"));
    TEST_ASSERT_TRUE(cmtext_get_extension("a.mzf") == g_cmt_tmz_extension);
    TEST_ASSERT_TRUE(cmtext_get_extension("a.wave") == g_cmt_tmz_extension);
}

/*
 * MZF + 0x30 + MZF: tři bloky, typy MZF / CONTROL / MZF, jména z hlaviček
 * a z tmz_block_id_name(); cb_next_block z bloku 0 otevře blok 2,
 * cb_previous_block z bloku 2 blok 0.
 */
void test_index_skips_control_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_text_block(f, "between");
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE, cmtext_container_get_type(c));
    TEST_ASSERT_EQUAL_INT(3, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_CONTROL, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 2));
    TEST_ASSERT_EQUAL_STRING("PART A", cmtext_container_get_block_fname(c, 0));
    TEST_ASSERT_EQUAL_STRING(tmz_block_id_name(0x30), cmtext_container_get_block_fname(c, 1));
    TEST_ASSERT_EQUAL_STRING("PART B", cmtext_container_get_block_fname(c, 2));
    /* blok 0x46 MZ800EMU-CMT z TMZ má pevnou rychlost z PROFILE_ARGS (měnitelná, SET 1:1) */
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, cmtext_container_get_block_cmt_speed(c, 0));

    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_next_block());
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_next_block());
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_previous_block());
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    /* řídicí blok otevřít nejde, aktuální blok zůstane */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_open_block(1));
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    /* dotazy na MZF bloků */
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_mzf(0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_mzf(1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_mzf(3));
    TEST_ASSERT_NULL(cmttmz_block_get_mzf(1));
    st_MZF *mzf = cmttmz_block_get_mzf(2);
    TEST_ASSERT_NOT_NULL(mzf);
    TEST_ASSERT_EQUAL_UINT16(TEST_BODY_SIZE, mzf->header.fsize);
    TEST_ASSERT_EQUAL_UINT16(0x1200, mzf->header.fstrt);
    mzf_free(mzf);
}

/* cb_open_block(0): stream s frekvencí GDGCLK_BASE, nenulová délka, pause_after 0. */
void test_open_block_stream(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_text_block(f, "between");
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(2));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(0));
    st_CMTEXT_BLOCK *b = g_cmt.ext->block;
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(b));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_block_get_type(b));
    TEST_ASSERT_EQUAL_UINT32(GDGCLK_BASE, cmtext_block_get_rate(b));
    TEST_ASSERT_EQUAL_INT(CMT_STREAM_TYPE_VSTREAM, cmtext_block_get_stream_type(b));
    TEST_ASSERT_TRUE(cmtext_block_get_count_scans(b) > 0);
    TEST_ASSERT_EQUAL_UINT16(0, cmtext_block_get_pause_after(b));
    /* pauza 1000 ms je součástí streamu: stream je delší než 1 s */
    TEST_ASSERT_TRUE(cmtext_block_get_count_scans(b) > (uint64_t)GDGCLK_BASE);
}

/*
 * Páska jen s 0x30 a 0x42: otevření projde, blok otevřít nejde, cmt_play()
 * zůstane ve STOP.
 */
void test_tape_without_signal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "only text");
    uint8_t body[16];
    memset(body, 0xA5, sizeof(body));
    st_TZX_BLOCK *eb = tmz_block_create_mz_extra_body(TMZ_FORMAT_NORMAL, CMTSPEED_1_1, 0, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(eb);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, eb));
    eb->data = NULL;
    tmz_block_free(eb);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_CONTROL, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_CONTROL, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_NULL(g_cmt.ext->block);

    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_open_block(0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_open_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_next_block());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_previous_block());
    TEST_ASSERT_NULL(g_cmt.ext->block);

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    cmt_play_paused();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    /* dotazy bez otevřeného bloku nepadají */
    cmt_update_output();
    TEST_ASSERT_EQUAL_INT(-1, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(g_cmt.mz_cmtspeed));
    cmt_rear_dip_switch_cmt_inverted_polarity(g_cmt.polarity ^ 1);
    cmt_rear_dip_switch_cmt_inverted_polarity(g_cmt.polarity ^ 1);
}

/*
 * PLAY, v pauze za blokem 0 vysunutí, pak otevření jiné pásky a PLAY:
 * po vysunutí nezůstane blok, kontejner ani soubor.
 */
void test_eject_during_gap_then_reopen(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_text_block(f, "between");
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    uint64_t scans = cmtext_block_get_count_scans(g_cmt.ext->block);
    TEST_ASSERT_TRUE(scans > (uint64_t)GDGCLK_BASE);

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    /* půl sekundy před koncem streamu bloku 0 = uvnitř pauzy 1000 ms */
    advance_ticks(scans - GDGCLK_BASE / 2);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_NULL(g_cmt_tmz_extension->block);
    TEST_ASSERT_NULL(g_cmt_tmz_extension->container);
    TEST_ASSERT_NULL(cmttmz_get_file());

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(1, "OTHER")));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext)));
    TEST_ASSERT_EQUAL_STRING("OTHER", cmtext_container_get_block_fname(cmtext_get_container(g_cmt.ext), 0));
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    for (int i = 0; i < 100; i++) {
        advance_ticks(10007);
        (void)cmt_read_data();
    }
    /* páska doběhne do konce a zastaví se */
    advance_ticks(cmtext_block_get_count_scans(g_cmt.ext->block));
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/**
 * @brief Přidá do souboru blok 0x41 (MZ Turbo Data) ve formátu SINCLAIR.
 *
 * tmz_block_to_mzf() z něj MZF přečte, player ho odmítne
 * (TMZ_PLAYER_ERROR_UNSUPPORTED: formát SINCLAIR v bloku 0x41 nepodporuje
 * a převod v1 ho nepřevede).
 *
 * @param f Soubor. @param name Jméno MZF.
 */
static void add_unplayable_mzf_block(st_TZX_FILE *f, const char *name)
{
    st_TMZ_MZ_TURBO_DATA td;
    memset(&td, 0, sizeof(td));
    td.machine = TMZ_MACHINE_MZ800;
    td.pulseset = TMZ_PULSESET_800;
    td.format = TMZ_FORMAT_SINCLAIR;
    td.speed = CMTSPEED_1_1;
    td.pause_ms = 1000;
    make_mzf_header(&td.mzf_header, name);
    uint8_t body[TEST_BODY_SIZE];
    memset(body, 0x33, sizeof(body));
    st_TZX_BLOCK *b = tmz_block_create_mz_turbo(&td, body, TEST_BODY_SIZE);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/*
 * MZF + nepřehratelný MZF (0x41 SINCLAIR) + MZF: blok 1 má v indexu typ MZF
 * a jméno z hlavičky, next z bloku 0 ho přeskočí na blok 2, previous z 2
 * vrátí 0, přímé otevření bloku 1 selže; položka indexu bloku 1 zůstane
 * beze změny (index se po otevření pásky nepřepisuje). Při PLAY přejde
 * páska po konci bloku 0 rovnou na blok 2.
 */
void test_unplayable_block_is_skipped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_unplayable_mzf_block(f, "BAD");
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(3, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 1));
    const char *fname_before = cmtext_container_get_block_fname(c, 1);
    TEST_ASSERT_EQUAL_STRING("BAD", fname_before);
    st_CMTEXT_TAPE_INDEX before = c->tape->index[1];
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_mzf(1));

    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_next_block());
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_previous_block());
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, c->cb_open_block(1));
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    /* položka indexu nepřepsaná: stejné bajty, stejný ukazatel na jméno */
    TEST_ASSERT_EQUAL_MEMORY(&before, &c->tape->index[1], sizeof(before));
    TEST_ASSERT_TRUE(fname_before == cmtext_container_get_block_fname(c, 1));
    TEST_ASSERT_EQUAL_STRING("BAD", cmtext_container_get_block_fname(c, 1));

    /* přehrávání: po konci bloku 0 rovnou blok 2 */
    uint64_t scans = cmtext_block_get_count_scans(g_cmt.ext->block);
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    advance_ticks(scans + 1000);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c->tape->index[1], sizeof(before));
}

/**
 * @brief Přidá do bufferu blok TAP ([2 B délka LE][flag + data + checksum]).
 * @param buf Buffer. @param data Flag, data a checksum. @param len Délka.
 */
static void append_tap_block(GByteArray *buf, const uint8_t *data, uint16_t len)
{
    uint8_t l[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    g_byte_array_append(buf, l, 2);
    g_byte_array_append(buf, data, len);
}

/*
 * Páska .tap (hlavička + data): index TAPHEADER se jménem 10 znaků z hlavičky
 * a kódem typu, TAPDATA s prázdným jménem a fsize bez flagu a checksumu;
 * rychlost bloků pevná 1:1 (SET), 1400 Bd.
 */
void test_tap_index_types_and_names(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t hdr[19];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x00;                 /* flag hlavičky */
    hdr[1] = 0x03;                 /* kód: File Header */
    memcpy(&hdr[2], "TAPNAME   ", 10);
    hdr[12] = 8;                   /* délka dat */
    uint8_t data[10];
    data[0] = 0xFF;                /* flag dat */
    for (int i = 1; i < 10; i++)
        data[i] = (uint8_t)(i * 11);
    GByteArray *buf = g_byte_array_new();
    append_tap_block(buf, hdr, sizeof(hdr));
    append_tap_block(buf, data, sizeof(data));
    const char *path = tmp_path(0, "tap");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)buf->data, buf->len, NULL));
    g_byte_array_free(buf, TRUE);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TAPHEADER, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_STRING("TAPNAME   ", cmtext_container_get_block_fname(c, 0));
    TEST_ASSERT_EQUAL_INT(0x03, cmtext_container_get_block_ftype(c, 0));
    TEST_ASSERT_EQUAL_STRING("File Header", cmttmz_tap_code_txt(cmtext_container_get_block_ftype(c, 0)));
    TEST_ASSERT_EQUAL_INT(17, cmtext_container_get_block_fsize(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TAPDATA, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_block_fname(c, 1));
    TEST_ASSERT_EQUAL_INT(8, cmtext_container_get_block_fsize(c, 1));
    TEST_ASSERT_EQUAL_STRING("Data", cmttmz_tap_code_txt(cmtext_container_get_block_ftype(c, 1)));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, cmtext_container_get_block_cmt_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TAPHEADER, cmtext_block_get_type(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_UINT16(1400, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
}

/**
 * @brief Přidá do souboru blok s daty (kopie), např. řídicí blok TZX.
 *
 * @param f Soubor.
 * @param id ID bloku.
 * @param data Data bloku podle TZX 1.20 (bez bajtu ID); NULL jen při len 0.
 * @param len Délka dat.
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

/** @brief Blok 0x20 Pause s pauzou ms (0 = zastavení pásky). @param f Soubor. @param ms Pauza. */
static void add_pause_block(st_TZX_FILE *f, uint16_t ms)
{
    const uint8_t d[2] = {(uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_PAUSE, d, 2);
}

/** @brief Blok 0x2A Stop the Tape if 48K (DWORD délka 0). @param f Soubor. */
static void add_stop48k_block(st_TZX_FILE *f)
{
    const uint8_t d[4] = {0, 0, 0, 0};
    add_raw_block(f, TZX_BLOCK_ID_STOP_48K, d, 4);
}

/** @brief Blok 0x24 Loop Start s počtem opakování. @param f Soubor. @param n Počet. */
static void add_loop_start_block(st_TZX_FILE *f, uint16_t n)
{
    const uint8_t d[2] = {(uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, d, 2);
}

/** @brief Blok 0x25 Loop End (bez dat). @param f Soubor. */
static void add_loop_end_block(st_TZX_FILE *f)
{
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
}

/** @brief Blok 0x23 Jump s relativním posunem. @param f Soubor. @param off Posun (0 = na sebe). */
static void add_jump_block(st_TZX_FILE *f, int16_t off)
{
    const uint16_t u = (uint16_t)off;
    const uint8_t d[2] = {(uint8_t)(u & 0xFF), (uint8_t)(u >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_JUMP, d, 2);
}

/**
 * @brief Blok 0x12 Pure Tone (délka pulzu v T-stavech 3,5 MHz, počet pulzů).
 * @param f Soubor. @param tstates Délka pulzu. @param count Počet pulzů.
 */
static void add_tone_block(st_TZX_FILE *f, uint16_t tstates, uint16_t count)
{
    const uint8_t d[4] = {(uint8_t)(tstates & 0xFF), (uint8_t)(tstates >> 8), (uint8_t)(count & 0xFF), (uint8_t)(count >> 8)};
    add_raw_block(f, TZX_BLOCK_ID_PURE_TONE, d, 4);
}

/**
 * @brief Nechá dohrát otevřený blok: posune čas za konec jeho streamu a přečte výstup.
 *
 * cmt_read_data() -> cmt_update_output() zjistí konec bloku a zavolá
 * přechod na další blok (cmttmz_advance()).
 */
static void play_to_block_end(void)
{
    advance_ticks(cmtext_block_get_count_scans(g_cmt.ext->block) + 1000);
    (void)cmt_read_data();
}

/**
 * @brief Zastavení pásky blokem mezi dvěma MZF.
 *
 * Po prvním bloku: PLAY v pauze, aktuální blok 2 (za příkazem), PC4 = 0.
 * Po pokračování (cmt_pause(0), nebo náběžná hrana PC3 zápisem do PPI)
 * hraje blok 2 od začátku, po něm STOP.
 *
 * @param stop48k 0 = Pause 0, 1 = blok 0x2A.
 * @param by_motor 0 = pokračuje cmt_pause(0), 1 = náběžná hrana PC3 (pio8255_write(), Bit Set/Reset).
 */
static void check_stop_tape(int stop48k, int by_motor)
{
    /* PC4 bez CMT hacku odvozuje Virtual CMT ze stavu transportu (pio8255_pc4_get()); tearDown příznak vrátí */
    g_cmthack.load_patch_installed = 0;
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    if (stop48k)
        add_stop48k_block(f);
    else
        add_pause_block(f, 0);
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_CONTROL, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 1));

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, cmt_get_playtime());
    /* blok zastavení pásky = zastavení motoru, ne pauza uživatele */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cmt.user_paused, "a tape-stop block is not the user's pause");
    bool motor_known = false, motor_running = true;
    cmt_motor_get_state(&motor_known, &motor_running);
    TEST_ASSERT_TRUE(motor_known);
    TEST_ASSERT_FALSE_MESSAGE(motor_running, "a tape-stop block stops the motor");

    /* v pauze páska stojí */
    advance_ticks(5 * GDGCLK_BASE);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));

    if (by_motor) {
        /* motor: PC3 = 0 a pak 1 přes Bit Set/Reset řídicího slova 8255 (port 3) */
        pio8255_write(3, 0x06);
        TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
        pio8255_write(3, 0x07);
    } else {
        cmt_pause(0);
    }
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());
    advance_ticks(GDGCLK_BASE / 10);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
}

/*
 * Krok bloku (Next / Previous) i dvojklik na jiný blok v pauze uživatele
 * se stojícím motorem zachovají oba zdroje pauzy.
 */
void test_step_and_select_keep_pause_sources(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    g_cmthack.load_patch_installed = 0;
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_mzf_block(f, "PART B", 1000);
    add_mzf_block(f, "PART C", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    cmt_pause(1);
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07); /* hrana PC3: motor stojí, pauza uživatele trvá */
    bool known = false, on = true;
    cmt_motor_get_state(&known, &on);
    TEST_ASSERT_TRUE(known);
    TEST_ASSERT_FALSE(on);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cmt.user_paused, "Next keeps the user's pause");
    cmt_motor_get_state(&known, &on);
    TEST_ASSERT_FALSE_MESSAGE(on, "Next keeps the motor state");

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_step_block(0));
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cmt.user_paused, "Previous keeps the user's pause");
    cmt_motor_get_state(&known, &on);
    TEST_ASSERT_FALSE(on);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_select_block(2, 1));
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cmt.user_paused, "double-click on another block keeps the user's pause");
    cmt_motor_get_state(&known, &on);
    TEST_ASSERT_FALSE(on);

    /* krok, který nejde (za posledním blokem), v pauze: blok, PLAY i zdroje zůstanou */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);

    /* krok, který nejde, při přehrávání: přehrávání pokračuje na původním místě */
    cmt_pause(0);
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07); /* motor zase běží, páska jede */
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    advance_ticks(GDGCLK_BASE / 10);
    (void)cmt_read_data();
    int blk = -1;
    uint64_t before = 0, after = 0;
    cmt_pause(1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&blk, &before));
    cmt_pause(0);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(1));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    cmt_pause(1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&blk, &after));
    TEST_ASSERT_EQUAL_INT(2, blk);
    TEST_ASSERT_TRUE_MESSAGE(after >= before && after - before < GDGCLK_BASE / 100, "a failed Next keeps the place in the block");
}

/*
 * Dvojklik / Play from here na aktuální blok v pauze uživatele: blok znovu
 * od začátku a pauza zůstane, stejně jako na jiném bloku; nepokračuje
 * z místa pauzy. Bez pauzy uživatele (čekání
 * na motor) dvojklik pásku rozjede jako dřív.
 */
void test_select_same_block_user_paused_restarts(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    g_cmthack.load_patch_installed = 0;
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_mzf_block(f, "PART B", 1000);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    advance_ticks(GDGCLK_BASE / 10);
    (void)cmt_read_data();
    cmt_pause(1);
    int blk = -1;
    uint64_t pos = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&blk, &pos));
    TEST_ASSERT_EQUAL_INT(0, blk);
    TEST_ASSERT_TRUE_MESSAGE(pos > 0, "the tape moved before the pause");

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_select_block(0, 1));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cmt.user_paused, "the user's pause stays, as on another block");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&blk, &pos));
    TEST_ASSERT_EQUAL_INT(0, blk);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, pos, "the current block starts from its beginning, not from the pause position");

    /* čekání na motor bez pauzy uživatele: dvojklik ho rozjede (beze změny) */
    cmt_pause(0);
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07); /* hrana PC3: motor stojí */
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_select_block(0, 1));
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "a tape waiting only for the motor starts");
}

/* Pause 0 zastaví pásku. */
void test_stop_tape_pause0(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_stop_tape(0, 0);
}

/* Blok 0x2A zastaví pásku stejně jako Pause 0. */
void test_stop_tape_0x2a(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_stop_tape(1, 0);
}

/* Pauzu po zastavení pásky ukončí náběžná hrana motoru PC3 zápisem do PPI. */
void test_stop_tape_resume_by_motor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    check_stop_tape(0, 1);
}

/*
 * Zastavení pásky, za kterým už není blok se signálem: konec pásky (STOP,
 * ne pauza). S autorewind se pak otevře první blok.
 */
void test_stop_tape_without_following_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_mzf_block(f, "PART B", 0);
    add_pause_block(f, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    /* blok 1 přímo (player za ním: Pause 0 a konec pásky) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));

    g_cmt.autorewind = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
}


/*
 * Loop Start 3x, tón, Loop End: tón (blok 1) se přehraje třikrát, pak STOP
 * (následnost bloků určuje player).
 */
void test_loop_plays_block_three_times(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_loop_start_block(f, 3);
    add_tone_block(f, 2168, 400);
    add_loop_end_block(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));

    g_cmt.paused = 0;
    cmt_play();
    int plays = 0;
    while (CMT_TEST_PLAY && plays < 10) {
        TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
        plays++;
        play_to_block_end();
    }
    TEST_ASSERT_EQUAL_INT(3, plays);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/*
 * Tón a skok 0x23 s posunem 0 (na sebe): player cyklus utne pojistkou
 * iterací a vrátí chybu; emulátor skončí ve STOP do 1 s emulovaného času
 * po konci tónu. Vypíše, jak dlouho přechod trval ve skutečném čase.
 */
void test_self_jump_ends_in_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_tone_block(f, 2168, 400);
    add_jump_block(f, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(cmtext_block_get_count_scans(g_cmt.ext->block) - 1000);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);

    gint64 t0 = g_get_monotonic_time();
    uint64_t elapsed = 0;
    while (CMT_TEST_PLAY && elapsed < GDGCLK_BASE) {
        advance_ticks(GDGCLK_BASE / 100);
        elapsed += GDGCLK_BASE / 100;
        (void)cmt_read_data();
    }
    gint64 t1 = g_get_monotonic_time();
    char msg[128];
    snprintf(msg, sizeof(msg), "self-jump: STOP after %.3f s of emulated time, %.3f ms of wall-clock time",
             (double)elapsed / GDGCLK_BASE, (double)(t1 - t0) / 1000.0);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_TRUE(elapsed < GDGCLK_BASE);
}

/*
 * Chyba postupu (skok na sebe) se opakuje: tón A, tón B, skok 0. Z bloku 1
 * skončí PLAY chybou ve STOP na bloku 1; druhé PLAY skončí stejně (stav
 * playeru se po chybě vrátil), ne koncem pásky - ani s autorewind se
 * nepřevine na blok 0.
 */
void test_self_jump_error_repeats_without_rewind(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_cmt.autorewind = 1;
    st_TZX_FILE *f = new_tmz();
    add_tone_block(f, 2168, 400);
    add_tone_block(f, 2168, 300);
    add_jump_block(f, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));

    for (int round = 0; round < 2; round++) {
        g_cmt.paused = 0;
        cmt_play();
        TEST_ASSERT_TRUE(CMT_TEST_PLAY);
        TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
        play_to_block_end();
        TEST_ASSERT_TRUE(CMT_TEST_STOP);
        TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    }
}

/**
 * @brief Přidá do souboru blok 0x18 (CSW) s kompresí Z-RLE.
 *
 * Player Z-RLE nepodporuje a vrátí TMZ_PLAYER_ERROR_STREAM_CREATE
 * (tmz_player.h, tmz_player_play_tzx_block()) - chyba, která blok
 * neoznačí k trvalému přeskočení. Blok se sestaví knihovnou (RLE)
 * a změní se mu bajt komprese (offset 09h dat, tzx_parse_csw_recording()).
 *
 * @param f Soubor.
 */
static void add_zrle_csw_block(st_TZX_FILE *f)
{
    uint32_t pulses[4] = {100, 100, 100, 100};
    st_TZX_BLOCK b;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(44100, 0, pulses, 4, &b));
    TEST_ASSERT_TRUE(b.length > 9);
    b.data[9] = TZX_CSW_COMPRESSION_ZRLE;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/*
 * CSW Z-RLE mezi dvěma MZF: player ho hlásí jako blok, který přehrát nejde
 * (TMZ_PLAYER_ERROR_UNSUPPORTED), blok dostane příznak přeskočení a PLAY
 * pokračuje za ním blokem 2.
 */
void test_zrle_block_in_the_middle_is_passed_over(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_zrle_csw_block(f);
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 1));

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    /* přímé otevření bloku 1 selže (blok je přeskakovaný) */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
}

/* Páska, jejíž první blok je CSW Z-RLE: otevře se na přehratelném bloku 1 a hraje. */
void test_zrle_first_block_opens_on_next(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_zrle_csw_block(f);
    add_mzf_block(f, "PART A", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP); /* konec pásky po bloku 1 */
}

/**
 * @brief Přidá blok 0x46 s jednou částí FM, jejíž půlvlna (1 jednotka při
 *        TIMEBASE 4 GHz) nedá ani jeden výstupní vzorek: platný blok, který
 *        generátor nepřevede (TMZ_PLAYER_ERROR_STREAM_CREATE, přechodná chyba).
 * @param f Soubor.
 */
static void add_unconvertible_fm_block(st_TZX_FILE *f)
{
    static const uint8_t tlv[] = { 0x01, 0x80, 0x04, 0x00, 0x00, 0x28, 0x6B, 0xEE,   /* TIMEBASE 4 000 000 000 */
                                   0x01, 0x81, 0x10, 0x00, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0 };
    static uint8_t hdr[128];
    static uint8_t body[1] = { 0x00 };
    st_TMZ_PART part = { TMZ_PART_ROLE_BODY, TMZ_FAMILY_FM, TMZ_SOURCE_MZF_BODY, 0, 0, NULL, 0, tlv, sizeof ( tlv ) };
    st_TMZ_MZFILE w = { "X-T", hdr, body, 1, NULL, 0, 1, &part };
    st_TZX_BLOCK *b = tmz_mzfile_create_block(&w, NULL);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/*
 * Přechodná chyba playeru při PLAY (blok 0x46, jehož signál generátor
 * nevyrobí, mezi dvěma MZF): páska skončí ve STOP na bloku 0, blok 1 se
 * neoznačí k přeskočení - druhé PLAY skončí stejně, místo aby pokračovalo
 * blokem 2.
 */
void test_transient_player_error_stops_without_skip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_unconvertible_fm_block(f);
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    for (int round = 0; round < 2; round++) {
        g_cmt.paused = 0;
        cmt_play();
        TEST_ASSERT_TRUE(CMT_TEST_PLAY);
        TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
        play_to_block_end();
        TEST_ASSERT_TRUE(CMT_TEST_STOP);
        TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    }
    /* přímé otevření bloku 1 selže také (zkusí se znovu, ne jako přeskočený) */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
}


/* autorewind: po konci pásky je aktuální blok 0 a stav STOP. */
void test_autorewind(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_cmt.autorewind = 1;
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    /* bez autorewind zůstane poslední blok */
    g_cmt.autorewind = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_next_block());
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
}

/**
 * @brief Uloží MZT se zadaným počtem MZF (jména "P0", "P1", ...) do slotu.
 * @param slot Slot. @param count Počet MZF (nejvýš 8).
 * @return Cesta souboru.
 */
static const char *save_mzt(int slot, int count)
{
    uint8_t raw[8 * (128 + TEST_BODY_SIZE)];
    memset(raw, 0, sizeof(raw));
    for (int b = 0; b < count; b++) {
        uint8_t *m = raw + b * (128 + TEST_BODY_SIZE);
        m[0] = 0x01;
        memset(&m[1], 0x0D, 17);
        m[1] = 'P';
        m[2] = (uint8_t)('0' + b);
        m[0x12] = TEST_BODY_SIZE;
        m[0x15] = 0x12;
        m[0x17] = 0x12;
        for (int i = 0; i < TEST_BODY_SIZE; i++)
            m[128 + i] = (uint8_t)((i * 37 + 11 + b) & 0xFF);
    }
    const char *path = tmp_path(slot, "mzt");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, count * (128 + TEST_BODY_SIZE), NULL));
    return path;
}

/**
 * @brief Kopie dat bloku vložené pásky (pro porovnání před a po změně).
 * @param id Index bloku. @param[out] len Délka.
 * @return Kopie (g_free()).
 */
static uint8_t *dup_block_data(int id, uint32_t *len)
{
    const st_TZX_FILE *f = cmttmz_get_file();
    TEST_ASSERT_NOT_NULL(f);
    *len = f->blocks[id].length;
    return (uint8_t *)g_memdup2(f->blocks[id].data, *len);
}

/**
 * @brief Zjistí, zda má blok vložené pásky stejná data jako kopie.
 * @param id Index bloku. @param data Kopie. @param len Délka kopie.
 * @return true = shodná data.
 */
static bool block_data_equal(int id, const uint8_t *data, uint32_t len)
{
    const st_TZX_FILE *f = cmttmz_get_file();
    return (f->blocks[id].length == len) && (0 == memcmp(f->blocks[id].data, data, len));
}

/* MZF 1:1, výchozí rychlost 2:1 -> stream poloviční (na 1 %), MZF bloku beze změny. */
void test_default_speed_halves_mzf_stream(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(0, "HALF")));
    uint64_t s1 = cmtext_block_get_count_scans(g_cmt.ext->block);
    st_MZF *m1 = cmttmz_block_get_mzf(0);
    TEST_ASSERT_NOT_NULL(m1);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    uint64_t s2 = cmtext_block_get_count_scans(g_cmt.ext->block);
    double ratio = (double)s2 / (double)s1;
    TEST_ASSERT_DOUBLE_WITHIN(0.005, 0.5, ratio);
    TEST_ASSERT_EQUAL_UINT16(2400, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));

    st_MZF *m2 = cmttmz_block_get_mzf(0);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_MEMORY(&m1->header, &m2->header, sizeof(m1->header));
    TEST_ASSERT_EQUAL_UINT32(m1->body_size, m2->body_size);
    TEST_ASSERT_EQUAL_MEMORY(m1->body, m2->body, m1->body_size);
    mzf_free(m1);
    mzf_free(m2);
}

/*
 * Výchozí rychlost přestaví v souboru hned všechny bloky DEFAULT (i ty, které
 * nejsou otevřené), blok SET nechá: MZT se třemi bloky, blok 1
 * SET 3:1, změna výchozí rychlosti na 2:1 změní data bloků 0 a 2, blok 1 ne.
 */
void test_block_speed_set_and_default_change(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 3)));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 2));

    uint32_t l0, l1, l2;
    uint8_t *d0 = dup_block_data(0, &l0);
    uint8_t *d1 = dup_block_data(1, &l1);
    uint8_t *d2 = dup_block_data(2, &l2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    TEST_ASSERT_FALSE(block_data_equal(0, d0, l0));
    TEST_ASSERT_TRUE(block_data_equal(1, d1, l1));
    TEST_ASSERT_FALSE(block_data_equal(2, d2, l2));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 1));
    g_free(d0);
    g_free(d1);
    g_free(d2);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(1));
    TEST_ASSERT_EQUAL_UINT16(3600, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(2));
    TEST_ASSERT_EQUAL_UINT16(2400, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
}

/* Vlastní pulzy bloku 235/247/120/139 µs se vrátí beze změny. */
void test_block_pulses_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    st_MZTAPE_PULSES_LENGTH p, got;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 139));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_pulses(1, &p));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_get_block_pulses(c, 1, &got));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 235e-6, got.long_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 247e-6, got.long_pulse.low);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 120e-6, got.short_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 139e-6, got.short_pulse.low);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(c, 1));
}

/* Blok 0x46 ROM-800-NOMINAL z TMZ - změna rychlosti EXIT_FAILURE, blok beze změny. */
void test_file_speed_block_rejected(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZF_HEADER hdr;
    make_mzf_header(&hdr, "ROM");
    uint8_t body[TEST_BODY_SIZE];
    memset(body, 0x5A, sizeof(body));
    en_TMZ_PROFILE_ERROR perr = TMZ_PROFILE_OK;
    st_TZX_BLOCK *b = tmz_profile_build_block("ROM-800-NOMINAL", NULL, &hdr, body, TEST_BODY_SIZE, &perr);
    TEST_ASSERT_NOT_NULL(b);
    st_TZX_FILE *f = new_tmz();
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    uint32_t l0;
    uint8_t *d0 = dup_block_data(0, &l0);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(0, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_change_block_speed(0, CMTSPEED_3_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    TEST_ASSERT_TRUE(block_data_equal(0, d0, l0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_NONE, cmtext_container_get_block_speed(cmtext_get_container(g_cmt.ext), 0));
    g_free(d0);
}

/* TAP 2:1 -> stream poloviční (na 1 %), blok zůstává 0x11, 2800 Bd. */
void test_tap_speed_2_1(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data[40];
    data[0] = 0xFF;
    for (int i = 1; i < 40; i++)
        data[i] = (uint8_t)(i * 13);
    GByteArray *buf = g_byte_array_new();
    append_tap_block(buf, data, sizeof(data));
    const char *path = tmp_path(0, "tap");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)buf->data, buf->len, NULL));
    g_byte_array_free(buf, TRUE);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));

    uint64_t s1 = cmtext_block_get_count_scans(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_UINT16(1400, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_speed_changeable(0, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(0, CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(0, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(0));
    uint64_t s2 = cmtext_block_get_count_scans(g_cmt.ext->block);
    TEST_ASSERT_DOUBLE_WITHIN(0.005, 0.5, (double)s2 / (double)s1);
    TEST_ASSERT_EQUAL_HEX8(TZX_BLOCK_ID_TURBO_SPEED, cmttmz_get_file()->blocks[0].id);
    TEST_ASSERT_EQUAL_UINT16(2800, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));

    /* zpět na 1:1 přes seznam bloků (rychlosti ZX) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(0, CMTSPEED_1_1, false, NULL));
    TEST_ASSERT_EQUAL_UINT64(s1, cmtext_block_get_count_scans(g_cmt.ext->block));
}

/*
 * Blok 0x46 MZ800EMU-CMT z TMZ: rychlost z PROFILE_ARGS (SET 1:1), jde
 * změnit (3:1 = 3600 Bd po otevření).
 */
void test_tmz_emu_profile_block_speed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "EMU", 0);
    add_text_block(f, "info");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_UINT16(1200, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_speed_changeable(0, CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(2, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(-1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, c->cb_open_block(0));
    TEST_ASSERT_EQUAL_UINT16(3600, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
}

/** @brief Krok vzorkování výstupu v testech pozice (takty GDG). */
#define POS_STEP_TICKS 50

/**
 * @brief Zaznamená časy prvních n hran výstupu pásky od aktuálního okamžiku.
 *
 * Posouvá čas po POS_STEP_TICKS taktech a čte cmt_read_data(); čas hrany
 * je počet taktů od začátku záznamu (kvantovaný krokem).
 *
 * @param[out] edges Časy hran (n položek).
 * @param n Počet hran.
 */
static void record_edges(uint32_t *edges, int n)
{
    uint32_t t = 0;
    int prev = cmt_read_data();
    for (int i = 0; i < n;) {
        advance_ticks(POS_STEP_TICKS);
        t += POS_STEP_TICKS;
        int v = cmt_read_data();
        TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PLAY, "tape stopped before enough edges");
        if (v != prev) {
            edges[i++] = t;
            prev = v;
        }
    }
}

/*
 * MZF, PLAY, 2 s, pauza -> pozice blok 0 a 2 s (na 1 ms);
 * cmt_tape_set_position(0, 0) a pokračování -> prvních 1000 hran stejně
 * jako od začátku bloku.
 */
void test_position_get_and_rewind_in_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static uint32_t ref[1000], got[1000];
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(0, "POS")));
    g_cmt.paused = 0;
    cmt_play();
    record_edges(ref, 1000);
    uint64_t spent = ref[999];
    advance_ticks(2ull * GDGCLK_BASE - spent);
    (void)cmt_read_data();
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);

    int block_id = -1;
    uint64_t ticks = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_UINT64_WITHIN(GDGCLK_BASE / 1000, 2ull * GDGCLK_BASE, ticks);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(0, ticks);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    cmt_pause(0);
    record_edges(got, 1000);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(ref, got, 1000);
}

/*
 * Pozice nastavená ve STOP platí pro další PLAY (start odtud, hrany jako
 * při průchodu tímtéž místem); po STOP a PLAY bez nastavení se hraje od
 * začátku bloku.
 */
void test_position_set_in_stop_applies_to_play(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static uint32_t ref[200], got[200];
    const uint64_t at = GDGCLK_BASE / 2;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(0, "POS")));
    /* reference: od začátku až do místa at, pak hrany */
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(at);
    (void)cmt_read_data();
    record_edges(ref, 200);
    cmt_stop();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, at));
    int block_id = -1;
    uint64_t ticks = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(at, ticks);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    cmt_play();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(at, ticks);
    record_edges(got, 200);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(ref, got, 200);

    /* STOP a PLAY bez nastavení: od začátku bloku */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(0, ticks);
    cmt_play();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(0, ticks);
}

/*
 * Pozice za koncem = konec bloku; blok mimo rozsah nebo CONTROL
 * a volání během PLAY bez pauzy = EXIT_FAILURE; bez pásky a při nahrávání
 * EXIT_FAILURE; jiný blok ve STOP a v pauze (i po zastavení pásky) se otevře.
 */
void test_position_clamp_and_errors(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    int block_id = -1;
    uint64_t ticks = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(0, 0));

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_text_block(f, "between");
    add_pause_block(f, 0);
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    uint64_t end0 = cmtext_block_get_count_scans(g_cmt.ext->block); /* vstream s frekvencí GDGCLK_BASE */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 100ull * GDGCLK_BASE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(end0, ticks);

    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(4, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(-1, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(1, 0)); /* text = CONTROL */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(2, 0)); /* Pause 0 = CONTROL */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(end0, ticks);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(3, 1000));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(3, block_id);
    TEST_ASSERT_EQUAL_UINT64(1000, ticks);
    TEST_ASSERT_EQUAL_INT(3, cmtext_block_get_block_id(g_cmt.ext->block));

    /* PLAY bez pauzy: get jde, set ne */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 0));
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(5000);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(5000, ticks);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(0, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(3, 0));

    /* zastavení pásky (Pause 0): pauza na bloku 3, set na blok 0 jde */
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(3, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 777));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(777, ticks);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);

    /* nahrávání (do nové prázdné pásky po vysunutí): get i set odmítne
     * kontrola stavu RECORD. */
    cmt_stop();
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(tmp_path(1, "wav")));
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(0, 0));
    cmt_stop();
}

/*
 * Změna rychlosti otevřeného bloku zruší pozici nastavenou ve STOP:
 * MZT se dvěma bloky, pozice daleko za polovinou bloku 0, výchozí
 * rychlost 2:1 (blok 0 je DEFAULT, nový stream je kratší než pozice),
 * PLAY -> hraje blok 0 od začátku, ne hned další blok.
 */
void test_position_reset_by_speed_change(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    uint64_t len0 = cmtext_block_get_count_scans(g_cmt.ext->block);
    uint64_t at = len0 * 9 / 10;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, at));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    TEST_ASSERT_TRUE(cmtext_block_get_count_scans(g_cmt.ext->block) < at);

    int block_id = -1;
    uint64_t ticks = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(0, ticks);

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_INT(0, block_id);
    TEST_ASSERT_EQUAL_UINT64(0, ticks);
    advance_ticks(1000);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&block_id, &ticks));
    TEST_ASSERT_EQUAL_UINT64(1000, ticks);
}

/* Pozice na bloku, který player odmítá (příznak přeskočení), se odmítne. */
void test_position_refused_on_skipped_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "PART A", 1000);
    add_unplayable_mzf_block(f, "BAD");
    add_mzf_block(f, "PART B", 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));

    /* první pokus: player blok odmítne (UNSUPPORTED) a označí ho k přeskočení */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(1, 0));
    /* druhý pokus: blok s příznakem přeskočení */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(1, 0));
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    g_cmt.paused = 0;
    cmt_play();
    cmt_pause(1);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_set_position(1, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(2, 0));
}

/*
 * Přestavění bloků DEFAULT selže (vlastní pulz 0,2 µs se zaokrouhlí na 0 µs,
 * cmttmz_build_mzf_block() ho odmítne): cmt_change_custom_pulses() i
 * cmt_change_speed(CMTSPEED_CUSTOM) vrátí EXIT_FAILURE, všechny bloky
 * souboru jsou bajtově beze změny, výchozí rychlost i pulzy se nemění.
 */
void test_default_speed_rebuild_failure_is_atomic(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 3)));
    uint32_t len[3];
    uint8_t *data[3];
    for (int i = 0; i < 3; i++)
        data[i] = dup_block_data(i, &len[i]);
    st_MZTAPE_PULSES_LENGTH good = g_cmt.mz_custom_pulses;

    st_MZTAPE_PULSES_LENGTH bad;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&bad, 470, 494, 240, 278));
    bad.long_pulse.high = 0.2e-6;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_change_custom_pulses(&bad));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, g_cmt.mz_cmtspeed);
    TEST_ASSERT_EQUAL_MEMORY(&good, &g_cmt.mz_custom_pulses, sizeof(good));

    g_cmt.mz_custom_pulses = bad;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_change_speed(CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, g_cmt.mz_cmtspeed);
    g_cmt.mz_custom_pulses = good;

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(block_data_equal(i, data[i], len[i]));
        g_free(data[i]);
    }
}

/**
 * @brief Najde v otevřeném bloku prostředek prvního běhu dané úrovně dlouhého aspoň 4 takty.
 * @param level Hledaná úroveň (0 nebo 1).
 * @return Pozice v taktech GDGCLK_BASE (stream vstream s frekvencí GDGCLK_BASE).
 */
static uint64_t find_run_middle(int level)
{
    st_CMT_VSTREAM *v = g_cmt.ext->block->stream->str.vstream;
    TEST_ASSERT_EQUAL_UINT32(GDGCLK_BASE, cmt_vstream_get_rate(v));
    cmt_vstream_read_reset(v);
    uint64_t pos = 0, samples = 0;
    int value = 0;
    while (EXIT_SUCCESS == cmt_vstream_read_pulse(v, &samples, &value)) {
        if (value == level && samples >= 4) {
            cmt_vstream_read_reset(v);
            return pos + samples / 2;
        }
        pos += samples;
    }
    TEST_FAIL_MESSAGE("no run of the requested level");
    return 0;
}

/* V pauze dá cmt_tape_set_position() do g_cmt.output úroveň streamu v daném místě (HIGH i LOW). */
void test_position_output_in_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(0, "LEVEL")));
    uint64_t mid_high = find_run_middle(1);
    uint64_t mid_low = find_run_middle(0);
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(GDGCLK_BASE / 5);
    (void)cmt_read_data();
    cmt_pause(1);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, mid_high));
    TEST_ASSERT_EQUAL_INT(1, g_cmt.output);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, mid_low));
    TEST_ASSERT_EQUAL_INT(0, g_cmt.output);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, mid_high));
    TEST_ASSERT_EQUAL_INT(1, g_cmt.output);
}

/**
 * @brief Páska se smyčkou: Loop Start 3x, tón (blok 1), Loop End, tón (blok 3).
 * @return Cesta uloženého souboru (slot 0).
 */
static const char *save_loop_tape(void)
{
    st_TZX_FILE *f = new_tmz();
    add_loop_start_block(f, 3);
    add_tone_block(f, 2168, 300);
    add_loop_end_block(f);
    add_tone_block(f, 1500, 200);
    return save_tmz(f, 0);
}

/* Stav playeru: kopie, nastavení zpět, odmítnutí hodnot mimo soubor a mezí. */
void test_player_state_accessors(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TMZ_PLAYER_STATE st;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_get_player_state(&st)); /* bez pásky */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&st));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_loop_tape()));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end(); /* 2. průchod smyčkou */
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&st));
    TEST_ASSERT_EQUAL_INT(1, st.loop_depth);
    TEST_ASSERT_EQUAL_UINT32(1, st.loop_stack[0].start_block);
    TEST_ASSERT_EQUAL_UINT32(2, st.current_block);
    TEST_ASSERT_EQUAL_UINT32(1, st.last_played_block);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_set_player_state(&st));
    st_TMZ_PLAYER_STATE back;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&back));
    TEST_ASSERT_EQUAL_UINT32(st.current_block, back.current_block);
    TEST_ASSERT_EQUAL_INT(st.loop_depth, back.loop_depth);
    TEST_ASSERT_EQUAL_UINT32(st.loop_stack[0].remaining, back.loop_stack[0].remaining);

    /* hodnoty mimo soubor nebo meze knihovny se odmítnou a stav se nezmění */
    st_TMZ_PLAYER_STATE bad = st;
    bad.current_block = 5; /* blok_count = 4 */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.last_played_block = 4;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.loop_depth = TMZ_PLAYER_MAX_LOOP_DEPTH + 1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.loop_depth = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.loop_stack[0].start_block = 3; /* před blokem 3 není Loop Start */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.loop_stack[0].remaining = 0; /* Loop End by podtekl */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.loop_stack[0].remaining = 4; /* Loop Start má 3 opakování */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.last_played_block = 2; /* otevřený je blok 1 */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.config.mz_cpu_clock = 12345; /* páska nemá blok 0x43 */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.call_depth = 1;
    bad.call_stack[0].call_block = 1; /* blok 1 není Call Sequence */
    bad.call_stack[0].total_offsets = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.call_depth = TMZ_PLAYER_MAX_CALL_DEPTH + 1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.signal_level = 7;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.stopped = true;
    bad.finished = true; /* zastavení a konec zároveň nejde */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&back));
    TEST_ASSERT_EQUAL_UINT32(st.current_block, back.current_block);
    TEST_ASSERT_EQUAL_INT(st.loop_depth, back.loop_depth);
    TEST_ASSERT_EQUAL_UINT32(st.loop_stack[0].remaining, back.loop_stack[0].remaining);
    TEST_ASSERT_EQUAL_UINT32(st.last_played_block, back.last_played_block);
    TEST_ASSERT_EQUAL_UINT32(st.config.mz_cpu_clock, back.config.mz_cpu_clock);
}

/*
 * Stav playeru uvnitř Call Sequence: total_offsets musí být počet offsetů
 * bloku 0x26 a current_offset_idx menší; jinak odmítnuto, stav beze změny.
 */
void test_player_state_call_validation(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* 0 Call Sequence (1 offset, +2) / 1 tón / 2 tón (volaný) / 3 Return */
    st_TZX_FILE *f = new_tmz();
    const uint8_t call[4] = {1, 0, 2, 0};
    add_raw_block(f, TZX_BLOCK_ID_CALL_SEQUENCE, call, 4);
    add_tone_block(f, 2168, 300);
    add_tone_block(f, 1500, 200);
    add_raw_block(f, TZX_BLOCK_ID_RETURN_FROM_SEQ, NULL, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));

    st_TMZ_PLAYER_STATE st;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&st));
    TEST_ASSERT_EQUAL_INT(1, st.call_depth);
    TEST_ASSERT_EQUAL_UINT32(0, st.call_stack[0].call_block);
    TEST_ASSERT_EQUAL_UINT16(1, st.call_stack[0].total_offsets);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_set_player_state(&st));

    st_TMZ_PLAYER_STATE bad = st;
    bad.call_stack[0].total_offsets = 2; /* blok 0x26 má 1 offset */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = st;
    bad.call_stack[0].current_offset_idx = 1; /* >= total_offsets */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));

    st_TMZ_PLAYER_STATE back;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&back));
    TEST_ASSERT_EQUAL_INT(1, back.call_depth);
    TEST_ASSERT_EQUAL_UINT16(1, back.call_stack[0].total_offsets);
    TEST_ASSERT_EQUAL_UINT16(0, back.call_stack[0].current_offset_idx);
}

/**
 * @brief Uloží snapshot stroje do paměti (emulace v pauze).
 * @param[out] buf Buffer (g_free()). @param[out] size Velikost.
 */
static void snap_save(uint8_t **buf, size_t *size)
{
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmt tape", buf, size));
}

/**
 * @brief Načte snapshot z paměti a uvolní buffer.
 * @param buf Buffer. @param size Velikost.
 */
static void snap_load(uint8_t *buf, size_t size)
{
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
}

/**
 * @brief Ověří pozici pásky.
 * @param block Očekávaný blok. @param ticks Očekávaná pozice v taktech.
 */
static void assert_position(int block, uint64_t ticks)
{
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&b, &t));
    TEST_ASSERT_EQUAL_INT(block, b);
    TEST_ASSERT_EQUAL_UINT64(ticks, t);
}

/*
 * Snapshot ve STOP s pozicí nastavenou v bloku 1 a bloky s vlastní
 * rychlostí: po vysunutí a načtení je páska vložená, blok 1, stejná
 * pozice, STOP a rychlosti bloků jako při uložení.
 */
void test_snapshot_tape_stop_position_and_speeds(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 235, 247, 120, 139));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_pulses(2, &p));
    /* Změna rychlosti bloku pásku označí Modified a snapshot by ji vložil
     * jako obraz bez cesty; test ověřuje obnovu rychlostí z XML nad souborem
     * z cesty (rychlosti nastavené např. snapshotem starší verze) */
    cmttmz_set_modified(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 123456));

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    snap_load(buf, size);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    assert_position(1, 123456);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, cmtext_container_get_block_cmt_speed(c, 2));
    st_MZTAPE_PULSES_LENGTH got;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_get_block_pulses(c, 2, &got));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 235e-6, got.long_pulse.high);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 139e-6, got.short_pulse.low);
    TEST_ASSERT_EQUAL_UINT16(3600, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
}

/* Snapshot v pauze (blok 1, 1 s do bloku): po načtení pauza, blok 1, pozice 1 s. */
void test_snapshot_tape_pause_position(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    advance_ticks(GDGCLK_BASE);
    (void)cmt_read_data();
    cmt_pause(1);
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&b, &t));
    int out = g_cmt.output;

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    assert_position(1, t);
    TEST_ASSERT_EQUAL_INT(out, g_cmt.output);

    /* pokračování hraje odtud: stejné hrany jako v průběhu bez snapshotu */
    static uint32_t ref[200], got[200];
    snap_save(&buf, &size);
    cmt_pause(0);
    record_edges(ref, 200);
    cmt_eject();
    snap_load(buf, size);
    cmt_pause(0);
    record_edges(got, 200);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(ref, got, 200);
}

/*
 * Snapshot při PLAY (blok 1, 1 s do bloku): po načtení PLAY, blok 1, stejná
 * pozice; hrany signálu od načtení stejné jako bez snapshotu.
 */
void test_snapshot_tape_play_position(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static uint32_t ref[300], got[300];
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    advance_ticks(GDGCLK_BASE);
    (void)cmt_read_data();
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&b, &t));
    TEST_ASSERT_EQUAL_INT(1, b);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    record_edges(ref, 300);
    cmt_eject();
    snap_load(buf, size);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    assert_position(1, t);
    record_edges(got, 300);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(ref, got, 300);
}

/**
 * @brief Dohraje pásku do STOP a zapíše pořadí přehraných bloků.
 * @param[out] seq Čísla bloků. @param max Kapacita. @return Počet.
 */
static int play_sequence(int *seq, int max)
{
    int n = 0;
    while (CMT_TEST_PLAY && n < max) {
        seq[n++] = cmtext_block_get_block_id(g_cmt.ext->block);
        play_to_block_end();
    }
    return n;
}

/*
 * Checkpoint záznamu vstupů uvnitř smyčky: replay_ckpt_load() (replay.c)
 * otevře blok checkpointu přímo, je-li jiný než aktuální, a pak načte
 * snapshot stroje (snap_cmt.c). Po načtení musí páska pokračovat smyčkou
 * jako bez přerušení: zbylé průchody blokem 1, pak blok 3.
 */
static void assert_player_equal(const st_TMZ_PLAYER_STATE *a, const st_TMZ_PLAYER_STATE *b);

/**
 * @brief Checkpoint uvnitř smyčky (viz test_snapshot_checkpoint_inside_loop()).
 *
 * Kromě pořadí bloků po načtení ověří, že stav playeru po načtení (včetně
 * obsahu zásobníku smyček) je týž jako při uložení.
 *
 * @param keep true = načíst jako replay_snapshot_load() (snap_cmt_set_keep_inserted_tape()).
 */
static void checkpoint_inside_loop(bool keep)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_loop_tape()));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end(); /* 2. průchod blokem 1 */
    advance_ticks(1000);
    (void)cmt_read_data();
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));

    st_TMZ_PLAYER_STATE at_save, after_load;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&at_save));
    TEST_ASSERT_EQUAL_INT(1, at_save.loop_depth); /* uvnitř smyčky */
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    int ref[10], got[10];
    int nref = play_sequence(ref, 10);
    TEST_ASSERT_EQUAL_INT(3, nref); /* 1 (2. průchod), 1 (3. průchod), 3 */

    /* jako replay_ckpt_load(): jiný blok -> přímé otevření, pak snapshot */
    TEST_ASSERT_EQUAL_INT(3, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
    snap_cmt_set_keep_inserted_tape(keep);
    snap_load(buf, size);
    snap_cmt_set_keep_inserted_tape(false);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&after_load));
    assert_player_equal(&at_save, &after_load);
    int ngot = play_sequence(got, 10);
    TEST_ASSERT_EQUAL_INT(nref, ngot);
    TEST_ASSERT_EQUAL_INT_ARRAY(ref, got, nref);
}

void test_snapshot_checkpoint_inside_loop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    checkpoint_inside_loop(false);
}

/* Totéž v režimu záznamu vstupů (replay_snapshot_load() v replay.c). */
void test_snapshot_checkpoint_inside_loop_replay_mode(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    checkpoint_inside_loop(true);
}

/* Soubor pásky ze snapshotu chybí: transport STOP bez pásky. */
void test_snapshot_tape_file_missing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    g_cmt.paused = 0;
    cmt_play();
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    g_remove(path);
    snap_load(buf, size);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
}

/*
 * Soubor se po uložení snapshotu změnil (méně bloků): páska vložená, STOP,
 * blok 0, bez pádu a bez obnovy pozice.
 */
void test_snapshot_tape_file_changed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 3);
    gchar *keep = g_strdup(path);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    /* snapshot jen s cestou (bez obrazu), test ověřuje soubor změněný pod cestou */
    cmttmz_set_modified(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(2, 5000));
    g_cmt.paused = 0;
    cmt_play();
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();

    /* stejná cesta, jen 1 blok */
    uint8_t raw[128 + TEST_BODY_SIZE];
    gchar *contents = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(keep, &contents, &len, NULL));
    memcpy(raw, contents, sizeof(raw));
    g_free(contents);
    TEST_ASSERT_TRUE(g_file_set_contents(keep, (const gchar *)raw, sizeof(raw), NULL));

    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(cmtext_get_container(g_cmt.ext)));
    assert_position(0, 0);
    /* blok 0 odpovídá druhem i indexem: rychlost se obnoví */
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(cmtext_get_container(g_cmt.ext), 0));
    g_free(keep);
}

/*
 * Režim záznamu vstupů (snap_cmt_set_keep_inserted_tape): vložená kopie
 * pásky s jinou cestou se použije (blok a pozice ze snapshotu), soubor
 * z cesty ve snapshotu se neotevře - ani když vložená páska chybí.
 */
void test_snapshot_tape_keep_inserted(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    gchar *orig = g_strdup(save_mzt(0, 2));
    const char *copy = save_mzt(1, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(orig));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 4321));
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    uint8_t *buf2 = g_memdup2(buf, size);

    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)copy));
    snap_cmt_set_keep_inserted_tape(true);
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING(copy, g_cmt.last_filename);
    assert_position(1, 4321);

    /* bez vložené pásky se originál neotevře */
    cmt_eject();
    snap_load(buf2, size);
    snap_cmt_set_keep_inserted_tape(false);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    g_free(orig);
}

/*
 * Blok DEFAULT, jehož výchozí rychlost ve snapshotu (1:1) je jiná než
 * aktuální volba (3:1): po načtení hraje rychlostí ze snapshotu jako SET,
 * volba výchozí rychlosti zůstane 3:1.
 */
void test_snapshot_tape_default_speed_differs(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    en_CMTSPEED saved_default = g_cmt.mz_cmtspeed;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 1));
    uint16_t bd_saved = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 0));
    bd_saved = g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    g_cmt.mz_cmtspeed = CMTSPEED_3_1; /* uživatel mezitím změnil výchozí rychlost */
    snap_load(buf, size);

    c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, g_cmt.mz_cmtspeed);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, cmtext_container_get_block_cmt_speed(c, 1));
    assert_position(1, 0);
    TEST_ASSERT_EQUAL_UINT16(bd_saved, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));
    g_cmt.mz_cmtspeed = saved_default;
}

/*
 * Páska dohraná do konce bez převinutí: CMTTMZ_ADVANCE_END jen zastaví
 * transport (cmt_update_output()), poslední blok zůstane otevřený, takže
 * snapshot nese jeho číslo a pozici (ne block_id -1). Obnoví se ve STOP
 * na tomtéž místě, ne jako neshoda.
 */
void test_snapshot_tape_finished_without_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&b, &t));
    TEST_ASSERT_EQUAL_INT(0, b);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    assert_position(b, t); /* páska po konci stojí na posledním bloku */
}

/*
 * Snapshot ve stavu PLAY bez elementu tape (starý formát nebo prázdná cesta)
 * načtený přes pásku bez otevřeného bloku (jen řídicí bloky): transport
 * musí skončit ve STOP; dřív zůstal PLAY a cmt_update_output() četl blok
 * NULL. Pak pár snímků emulace pásky.
 */
void test_old_snapshot_play_over_tape_without_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_cmt_test_omit_tape(true); /* snapshot bez elementu tape (jako 2.0.6) */
    snap_save(&buf, &size);
    snap_cmt_test_omit_tape(false);

    cmt_eject();

    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "only text");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 1)));
    TEST_ASSERT_NULL(g_cmt.ext->block);
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    for (int i = 0; i < 5; i++) {
        advance_ticks(VIDEO_SCREEN_TICKS);
        cmt_screen_done_period();
        (void)cmt_read_data();
    }
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/**
 * @brief Posune čas GDG o libovolný počet taktů (i přes 2^32) celými snímky.
 * @param ticks Takty GDG.
 */
static void advance_long(uint64_t ticks)
{
    g_gdg.total_elapsed.screens += (unsigned)(ticks / VIDEO_SCREEN_TICKS);
    advance_ticks(ticks % VIDEO_SCREEN_TICKS);
}

/*
 * WAV s dlouhým tichem po burstu (kratší soubor): 8 kHz mono
 * 8 bit, 1 s burst a 300 s ticha. Ticho je při 17 721 600 Hz delší než
 * jeden event vstreamu (2^32 vzorků, 242 s): páska se otevře, blok má
 * správnou délku a dohraje se do STOP.
 */
void test_wav_long_silence_plays_to_end(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const uint32_t rate = 8000, burst = rate, silence = 300u * rate, frames = burst + silence;
    const uint32_t size = 44 + frames;
    uint8_t *w = (uint8_t *)calloc(1, size);
    TEST_ASSERT_NOT_NULL(w);
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
    const char *path = tmp_path(0, "wav");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)w, size, NULL));
    free(w);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    uint64_t scans = cmtext_block_get_count_scans(g_cmt.ext->block);
    uint64_t want = (uint64_t)frames * GDGCLK_BASE / rate;
    TEST_ASSERT_TRUE_MESSAGE(scans > 0xFFFFFFFFull, "block longer than one vstream event");
    TEST_ASSERT_TRUE_MESSAGE(scans + GDGCLK_BASE / 1000 >= want && scans <= want + GDGCLK_BASE / 1000,
                             "block length equals the WAV length (within 1 ms)");

    g_cmt.paused = 0;
    cmt_play();
    advance_long(want / 2);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.output == 1 ? 1 : 0); /* uprostřed ticha nízká úroveň */
    advance_long(want / 2 + GDGCLK_BASE);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_STOP); /* dohráno do konce */
}

/**
 * @brief Cesta k existujícímu souboru, která začíná dvěma lomítky.
 *
 * Windows: prefix zařízení \\?\ před absolutní cestou (_fullpath(); dočasný
 * adresář testů bývá i "/tmp", tedy kořen aktuálního disku). Jinde:
 * "/" před absolutní cestou ("//tmp/..." je v POSIX tentýž soubor).
 *
 * @param path Cesta k souboru (jinde než na Windows absolutní).
 * @return Nový řetězec (g_free()), nebo NULL, když absolutní cestu nejde zjistit.
 */
static gchar *device_path_of(const char *path)
{
#ifdef _WIN32
    char full[_MAX_PATH];
    if (!_fullpath(full, path, sizeof(full)))
        return NULL;
    gchar *p = g_strconcat("\\\\?\\", full, NULL);
    for (gchar *c = p; *c; c++)
        if (*c == '/') *c = '\\';
    return p;
#else
    return (path[0] == '/') ? g_strconcat("/", path, NULL) : NULL;
#endif
}

/*
 * Síťová cesta a cesta zařízení ve snapshotu se automaticky neotevře: cesta
 * \\?\C:\...\x.mzt k existujícímu souboru (g_file_test() by ho našel)
 * skončí jako chybějící soubor - kontrola předchází testu souboru, síť by se
 * tedy nekontaktovala. Je-li vložená páska s přesně touto cestou, použije se.
 */
void test_snapshot_network_path_not_opened(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 2);
    gchar *dev = device_path_of(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(dev, path);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(dev, G_FILE_TEST_IS_REGULAR), dev);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    g_free(g_cmt.last_filename);
    g_cmt.last_filename = g_strdup(dev); /* snapshot ponese cestu zařízení */
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    uint8_t *buf2 = g_memdup2(buf, size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    /* páska vložená s přesně touto cestou se použije */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(dev));
    TEST_ASSERT_EQUAL_STRING(dev, g_cmt.last_filename);
    snap_load(buf2, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING(dev, g_cmt.last_filename);
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    g_free(dev);

    /* UNC cesty obou tvarů: páska se neotevře, transport STOP */
    static const char *const unc[] = {"\\\\server\\share\\x.mzt", "//server/share/x.mzt"};
    for (unsigned i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
        g_free(g_cmt.last_filename);
        g_cmt.last_filename = g_strdup(unc[i]);
        snap_save(&buf, &size);
        cmt_eject();
        snap_load(buf, size);
        TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_FILLED, unc[i]);
        TEST_ASSERT_TRUE(CMT_TEST_STOP);
    }
}

/** @brief Počet volání kontroly velikosti MZF ve stubu GUI (tests/framework/stubs/app_stubs.c). */
extern int g_stub_cmt_mzfsize_check_calls;

/*
 * Načtení snapshotu znovu otevře MZF s tělem delším, než říká hlavička:
 * kontrola velikosti MZF (v GUI dialog opravy) při načtení neproběhne,
 * při ručním otevření ano.
 */
void test_snapshot_reopen_mzf_no_size_dialog(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t raw[128 + TEST_BODY_SIZE + 16];
    memset(raw, 0x55, sizeof(raw));
    raw[0] = 0x01;
    memset(&raw[1], 0x0D, 17);
    raw[1] = 'L';
    raw[0x12] = TEST_BODY_SIZE; raw[0x13] = 0;
    raw[0x14] = 0x00; raw[0x15] = 0x12;
    raw[0x16] = 0x00; raw[0x17] = 0x12;
    const char *path = tmp_path(0, "mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, sizeof(raw), NULL));

    int saved_check = g_cmt.mzfsize_check;
    g_cmt.mzfsize_check = CMT_MZFSIZE_CHECK_ENABLED;
    g_stub_cmt_mzfsize_check_calls = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_EQUAL_INT(1, g_stub_cmt_mzfsize_check_calls); /* ruční otevření kontroluje */
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, g_stub_cmt_mzfsize_check_calls); /* načtení snapshotu ne */
    g_cmt.mzfsize_check = saved_check;
}

/*
 * Varování otevření (MCP emu_cmt_open, pole warning): bloky, které player
 * neumí (0x19, 0x18 Z-RLE), a páska bez přehratelného bloku. Páska bez
 * takových bloků varování nemá.
 */
void test_open_warning_reports_unplayable(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *mzt = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)mzt));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_open_warning());
    g_remove(mzt); /* slot 0 se níže použije znovu (páska je v paměti) */

    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "intro");
    const uint8_t gdb[4] = {0, 0, 0, 0};
    add_raw_block(f, 0x19, gdb, sizeof(gdb));
    add_tone_block(f, 2168, 100);
    add_zrle_csw_block(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 1)));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_STRING("Blocks the player cannot play (skipped): 1 (ID 0x19), 3 (ID 0x18)",
                             cmttmz_get_open_warning());

    f = new_tmz();
    add_text_block(f, "only text");
    add_raw_block(f, 0x19, gdb, sizeof(gdb));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_STRING("Blocks the player cannot play (skipped): 1 (ID 0x19); The tape has no playable block",
                             cmttmz_get_open_warning());
}

/*
 * Vnořené prázdné smyčky kolem bloku, který player neumí (0x19): každý
 * průchod vnější smyčky stojí player přes 130 000 iterací a blok 0x19 se
 * přeskočí, celkem přes 8 miliard iterací. Jeden rozpočet iterací na celé
 * hledání bloku (cmttmz_seq_next()) to ukončí chybou "control flow does not
 * reach a playable block" rychle; páska zůstane vložená bez otevřeného bloku.
 */
void test_nested_empty_loops_around_unplayable_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_loop_start_block(f, 65535);
    add_loop_start_block(f, 65535);
    add_loop_end_block(f);
    const uint8_t gdb[4] = {0, 0, 0, 0};
    add_raw_block(f, 0x19, gdb, sizeof(gdb));
    add_loop_end_block(f);
    add_tone_block(f, 2168, 100);
    gint64 t0 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    gint64 us = g_get_monotonic_time() - t0;
    printf("nested empty loops: open took %.3f s\n", (double)us / 1e6);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_NULL(g_cmt.ext->block);
    TEST_ASSERT_TRUE_MESSAGE(us < 10 * G_USEC_PER_SEC, "open finishes within 10 s");
}

/*
 * Blok 0x18 přes mez délky bloku (frekvence 1 Hz, pulzy 0xFFFFFFFF = 136
 * let; při 17 721 600 Hz by generátor žádal gigabajty): je ve varování
 * otevření, při přehrávání se rychle přeskočí a páska hraje další blok.
 */
void test_csw_block_over_limit_is_passed_over(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_tone_block(f, 2168, 100);
    static const uint32_t huge[] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    st_TZX_BLOCK csw;
    memset(&csw, 0, sizeof(csw));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(1, 0, huge, 2, &csw));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &csw));
    add_tone_block(f, 2168, 100);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    TEST_ASSERT_EQUAL_STRING("Blocks the player cannot play (skipped): 1 (ID 0x18)", cmttmz_get_open_warning());
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));

    g_cmt.paused = 0;
    cmt_play();
    gint64 t0 = g_get_monotonic_time();
    play_to_block_end();
    gint64 us = g_get_monotonic_time() - t0;
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block)); /* blok 1 přeskočen */
    TEST_ASSERT_TRUE_MESSAGE(us < 5 * G_USEC_PER_SEC, "the block over the limit is passed over quickly");
}

/* Páska jen s řídicími bloky (bez přehratelného bloku): STOP, vložená, bez pádu. */
void test_snapshot_tape_control_blocks_only(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_text_block(f, "only text");
    add_pause_block(f, 0);
    const char *path = save_tmz(f, 0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    int b = -1;
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_get_position(&b, &t)); /* bez otevřeného bloku */

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
}

/*
 * Načtení snapshotu s nastraženými BP cmt:state_change a cmt:mstate je
 * neodpálí (vysunutí, otevření a zastavení pásky během načtení nejsou
 * přechody emulovaného stroje); skutečný přechod po načtení je odpálí.
 * Cache hrany cmt:mstate se srovná v obou směrech: snapshot z PLAY přes
 * zastavenou pásku (0 -> 1) i snapshot ze STOP přes hrající pásku (1 -> 0).
 * BP uklízí tearDown().
 */
void test_snapshot_load_does_not_fire_cmt_breakpoints(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_cmthack.load_patch_installed = 0; /* s CMT hackem odpaluje mstate pio8255 (tearDown vrátí) */

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    uint8_t *buf_stop = NULL, *buf_play = NULL;
    size_t size_stop = 0, size_play = 0;
    snap_save(&buf_stop, &size_stop); /* STOP s vloženou páskou */
    g_cmt.paused = 0;
    cmt_play();
    snap_save(&buf_play, &size_play); /* PLAY */
    cmt_eject();                      /* load pásku otevře (vnitřně vysunutí + otevření), STOP -> PLAY */

    breakpoints_clear_all();
    bp_vars_clear_storage();
    int id1 = breakpoints_add(0, "T", -1);
    breakpoints_set_type(id1, BPT_TYPE_HW_EVENT);
    breakpoints_set_event_name(id1, "cmt:state_change");
    breakpoints_set_action(id1, "$sc += 1");
    int id2 = breakpoints_add(0, "T", -1);
    breakpoints_set_type(id2, BPT_TYPE_HW_EVENT);
    breakpoints_set_event_name(id2, "cmt:mstate");
    breakpoints_set_action(id2, "$ms += 1");
    /* CHANGED: bez srovnání cache hrany po načtení by skutečný přechod
     * zpět na předchozí úroveň hranu nevyvolal */
    TEST_ASSERT_TRUE(breakpoints_set_event_trigger(id2, BP_EVT_TRIG_CHANGED));
    TEST_ASSERT_TRUE(g_bp_event_active[BP_EVENT_CMT_STATE_CHANGE]);
    g_bp_event_state[BP_EVENT_CMT_MSTATE] = 0; /* cache hrany: motor stojí (STOP před načtením) */

    /* snapshot z PLAY přes vysunutou pásku: tiše, cache 0 -> 1 */
    snap_load(buf_play, size_play);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT32(0, bp_var_get("sc"));
    TEST_ASSERT_EQUAL_INT32(0, bp_var_get("ms"));
    TEST_ASSERT_FALSE(cmt_quiet_active());
    TEST_ASSERT_EQUAL_UINT8(1, g_bp_event_state[BP_EVENT_CMT_MSTATE]);

    /* skutečný přechod PLAY -> STOP: oba BP (mstate hrana 1 -> 0) */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("sc"));
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("ms"));

    /* druhý směr: páska hraje (skutečný přechod), snapshot ze STOP přes ni */
    cmt_play();
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("sc"));
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("ms"));
    snap_load(buf_stop, size_stop);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("sc"));
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("ms"));
    TEST_ASSERT_EQUAL_UINT8(0, g_bp_event_state[BP_EVENT_CMT_MSTATE]);

    /* skutečný cmt_play() odpálí oba BP právě jednou (mstate hrana 0 -> 1) */
    cmt_play();
    TEST_ASSERT_EQUAL_INT32(3, bp_var_get("sc"));
    TEST_ASSERT_EQUAL_INT32(3, bp_var_get("ms"));
}

/*
 * Nahrávání se zapnutým CMT hackem přepíná zdroj PC4 (podvržený bit hacku
 * <-> stav decku). Hrana cmt:mstate (BP s podmínkou CHANGED i LOW) se
 * odpálí právě tehdy, když se tím změní hodnota, kterou program čte:
 * podvržený bit 1 -> Record (PC4 0) = hrana; motor programu (PC4 1) =
 * hrana; Stop s podvrženým bitem 1 = beze změny, bez hrany; Record,
 * motor a Stop s podvrženým bitem 0 = hrana jen při Stop (1 -> 0). BP
 * LOW nedostane zásah navíc, když se hodnota nemění. BP uklízí tearDown().
 */
void test_record_with_hack_fires_mstate_on_pc4_source_switch(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_cmthack.load_patch_installed = 1; /* jen příznak (tearDown vrátí) */
    g_pio8255.signal_pc04 = 1;          /* podvržený bit: motor "běží" */

    breakpoints_clear_all();
    bp_vars_clear_storage();
    int id1 = breakpoints_add(0, "T", -1);
    breakpoints_set_type(id1, BPT_TYPE_HW_EVENT);
    breakpoints_set_event_name(id1, "cmt:mstate");
    breakpoints_set_action(id1, "$ch += 1");
    TEST_ASSERT_TRUE(breakpoints_set_event_trigger(id1, BP_EVT_TRIG_CHANGED));
    int id2 = breakpoints_add(0, "T", -1);
    breakpoints_set_type(id2, BPT_TYPE_HW_EVENT);
    breakpoints_set_event_name(id2, "cmt:mstate");
    breakpoints_set_action(id2, "$lo += 1");
    TEST_ASSERT_TRUE(breakpoints_set_event_trigger(id2, BP_EVT_TRIG_LOW));
    TEST_ASSERT_TRUE(g_bp_event_active[BP_EVENT_CMT_MSTATE]);
    g_bp_event_state[BP_EVENT_CMT_MSTATE] = 1; /* cache: program čte podvržený bit 1 */

    /* Record: PC4 čte stav decku (0) -> hrana 1 -> 0 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("ch"));
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("lo"));

    /* motor programu rozjede nahrávání: 0 -> 1 */
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07);
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("ch"));
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("lo"));

    /* Stop: PC4 čte zase podvržený bit 1 = beze změny, bez hrany */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("ch"));
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("lo"));

    /* podvržený bit 0: Record beze změny (0 -> 0, BP LOW bez zásahu navíc) */
    g_pio8255.signal_pc04 = 0;
    g_bp_event_state[BP_EVENT_CMT_MSTATE] = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("ch"));
    TEST_ASSERT_EQUAL_INT32(1, bp_var_get("lo"));
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07);
    TEST_ASSERT_EQUAL_INT32(3, bp_var_get("ch"));
    /* Stop: deck běžel (1), podvržený bit 0 -> hrana 1 -> 0 */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    TEST_ASSERT_EQUAL_INT32(4, bp_var_get("ch"));
    TEST_ASSERT_EQUAL_INT32(2, bp_var_get("lo"));
}

/**
 * @brief Páska tón (0), tón (1), Pause 0 (2): po dohrání konec za blokem zastavení.
 * @return Cesta uloženého souboru (slot 0).
 */
static const char *save_tape_ending_with_stop(void)
{
    st_TZX_FILE *f = new_tmz();
    add_tone_block(f, 2168, 300);
    add_tone_block(f, 1500, 200);
    add_pause_block(f, 0);
    return save_tmz(f, 0);
}

/**
 * @brief Porovná stav playeru (bez ukazatele file a config kromě mz_cpu_clock).
 *
 * Porovnává i obsah zásobníku smyček (start_block, remaining) a volání
 * (call_block, current_offset_idx, total_offsets) do aktuální hloubky.
 *
 * @param a Očekávaný stav. @param b Skutečný stav.
 */
static void assert_player_equal(const st_TMZ_PLAYER_STATE *a, const st_TMZ_PLAYER_STATE *b)
{
    TEST_ASSERT_EQUAL_UINT32(a->current_block, b->current_block);
    TEST_ASSERT_EQUAL_UINT32(a->last_played_block, b->last_played_block);
    TEST_ASSERT_EQUAL_INT(a->loop_depth, b->loop_depth);
    for (int i = 0; i < a->loop_depth && i < TMZ_PLAYER_MAX_LOOP_DEPTH; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->loop_stack[i].start_block, b->loop_stack[i].start_block, "loop start_block");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->loop_stack[i].remaining, b->loop_stack[i].remaining, "loop remaining");
    }
    TEST_ASSERT_EQUAL_INT(a->call_depth, b->call_depth);
    for (int i = 0; i < a->call_depth && i < TMZ_PLAYER_MAX_CALL_DEPTH; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->call_stack[i].call_block, b->call_stack[i].call_block, "call_block");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(a->call_stack[i].current_offset_idx, b->call_stack[i].current_offset_idx,
                                         "call current_offset_idx");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(a->call_stack[i].total_offsets, b->call_stack[i].total_offsets,
                                         "call total_offsets");
    }
    TEST_ASSERT_EQUAL(a->stopped, b->stopped);
    TEST_ASSERT_EQUAL(a->finished, b->finished);
    TEST_ASSERT_EQUAL_UINT8(a->signal_level, b->signal_level);
    TEST_ASSERT_EQUAL_UINT32(a->config.mz_cpu_clock, b->config.mz_cpu_clock);
}

/*
 * Páska končí blokem zastavení (Pause 0) a dohraje se do konce:
 * cmttmz_advance() vrátí END, otevřený zůstane blok 1, player je finished
 * s last_played_block 2 (blok zastavení). Snapshot se načte bez
 * odmítnutí stavu playeru (stav po načtení stejný, ne přestavěný na blok).
 */
void test_snapshot_tape_ended_after_stop_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tape_ending_with_stop()));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    st_TMZ_PLAYER_STATE before;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&before));
    TEST_ASSERT_TRUE(before.finished);
    TEST_ASSERT_EQUAL_UINT32(2, before.last_played_block);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_set_player_state(&before)); /* stav po dohrání je platný */

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    st_TMZ_PLAYER_STATE after;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&after));
    assert_player_equal(&before, &after);

    /* bez finished je last_played_block jiný než otevřený blok neplatný */
    st_TMZ_PLAYER_STATE bad = before;
    bad.finished = false;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
    bad = before;
    bad.last_played_block = 3; /* >= počet bloků */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_set_player_state(&bad));
}

/*
 * Snapshot v pauze po bloku zastavení uprostřed pásky (tón, Pause 0, tón,
 * tón): páska čeká na bloku 2 (za příkazem), last_played_block 2. Po
 * načtení stejný stav playeru a po pokračování hraje blok 2 a pak blok 3
 * se stejnými hranami jako bez snapshotu.
 */
void test_snapshot_stop_the_tape_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_tone_block(f, 2168, 300);
    add_pause_block(f, 0);
    add_tone_block(f, 1500, 200);
    add_tone_block(f, 900, 250);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_tmz(f, 0)));
    g_cmt.paused = 0;
    cmt_play();
    play_to_block_end();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    st_TMZ_PLAYER_STATE before;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&before));
    TEST_ASSERT_EQUAL_UINT32(2, before.last_played_block);
    TEST_ASSERT_FALSE(before.stopped);
    TEST_ASSERT_FALSE(before.finished);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);

    /* referenční průběh bez snapshotu */
    static uint32_t ref[300], got[300];
    int ref_seq[4], got_seq[4];
    cmt_pause(0);
    record_edges(ref, 300);
    int nref = play_sequence(ref_seq, 4);

    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    st_TMZ_PLAYER_STATE after;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&after));
    assert_player_equal(&before, &after);

    cmt_pause(0);
    record_edges(got, 300);
    int ngot = play_sequence(got_seq, 4);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(ref, got, 300);
    TEST_ASSERT_EQUAL_INT(nref, ngot);
    TEST_ASSERT_EQUAL_INT_ARRAY(ref_seq, got_seq, nref);
}

/* Otevření pásky, která neexistuje, selže a nic nenechá vložené. */
void test_open_missing_file_fails(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = tmp_path(0, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_NULL(cmttmz_get_file());
    TEST_ASSERT_NULL(g_cmt_tmz_extension->container);
}

/* ================================================================
 * ZÁZNAM DO PÁSKY (cmt_record(), cmt_stop(), cmttmz_append_blocks())
 * ================================================================ */

/** @brief Délka těla MZF, které "program" při záznamu uloží. */
#define REC_BODY_SIZE 256

/** @brief Tělo MZF pro záznam (plní rec_make_mzf()). */
static uint8_t s_rec_body[REC_BODY_SIZE];

/** @brief Počet jedničkových bitů (checksum ROM). */
static uint32_t rec_popcount(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint8_t b = d[i]; b; b >>= 1) c += b & 1u;
    return c;
}

/** @brief MZF pro mztape: typ 01h, jméno, tělo REC_BODY_SIZE B (deterministické). */
static void rec_make_mzf(st_MZTAPE_MZF *m, const char *name)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < REC_BODY_SIZE; i++) s_rec_body[i] = (uint8_t)((i * 53 + 7) & 0xFF);
    m->header[0x00] = 0x01;
    memset(&m->header[0x01], 0x0D, 17);
    memcpy(&m->header[0x01], name, strlen(name));
    m->header[0x12] = (uint8_t)(REC_BODY_SIZE & 0xFF);
    m->header[0x13] = (uint8_t)(REC_BODY_SIZE >> 8);
    m->header[0x15] = 0x12;
    m->header[0x17] = 0x12;
    m->body = s_rec_body;
    m->size = REC_BODY_SIZE;
    m->chkh = rec_popcount(m->header, sizeof(m->header));
    m->chkb = rec_popcount(s_rec_body, REC_BODY_SIZE);
}

/**
 * @brief "Program" zapíše signál ROM (2x hlavička + 2x tělo) přes cmt_write_data().
 *
 * Každý pulz signálu mztape (GDGCLK_BASE) = zápis úrovně a posun času;
 * poslední pulz uzavře zápis opačné úrovně. PC1 při begin musí být opačná
 * k první úrovni (nastaví rec_start()).
 *
 * @param m MZF. @return Celkové takty mezi první a poslední hranou.
 */
static uint64_t rec_write_rom(st_MZTAPE_MZF *m)
{
    st_CMT_VSTREAM *vs = mztape_create_cmt_vstream_from_mztmzf(m, MZTAPE_FORMATSET_MZ800, CMTSPEED_1_1, GDGCLK_BASE);
    TEST_ASSERT_NOT_NULL(vs);
    uint64_t samples, total = 0;
    int value, last = 0;
    cmt_vstream_read_reset(vs);
    while (cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS) {
        if (!samples) continue;
        cmt_write_data(value);
        advance_ticks(samples);
        total += samples;
        last = value;
    }
    cmt_write_data(last ^ 1);
    cmt_vstream_destroy(vs);
    return total;
}

/** @brief Zahájí záznam (PC1 LOW, první hrana ROM je HIGH) a rozběhne motor (zruší pauzu). */
static void rec_start(void)
{
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    cmt_pause(0);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
}

/** @brief Zapíše count hran s úseky 100..10 000 taktů (deterministicky), začne HIGH. */
static void rec_write_noise(int count)
{
    uint32_t x = 99;
    for (int i = 0; i < count; i++) {
        cmt_write_data((i & 1) ? 0 : 1);
        x = x * 1103515245u + 12345u;
        advance_ticks(100u + (x >> 16) % 9901u);
    }
}

/** @brief Ověří, že blok vložené pásky nese MZF m (hlavička i tělo bajtově). */
static void rec_check_block_mzf(int block_id, const st_MZTAPE_MZF *m)
{
    st_MZF *mzf = cmttmz_block_get_mzf(block_id);
    TEST_ASSERT_NOT_NULL(mzf);
    st_MZF_HEADER hdr;
    memcpy(&hdr, &mzf->header, sizeof(hdr));
    mzf_header_swap_endianity(&hdr);
    TEST_ASSERT_EQUAL_MEMORY(m->header, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(m->size, mzf->header.fsize);
    TEST_ASSERT_EQUAL_MEMORY(m->body, mzf->body, m->size);
    mzf_free(mzf);
}

/*
 * Záznam do prázdné kazety (bez pásky): cmt_record() vloží
 * novou pásku, signál ROM přes cmt_write_data(), cmt_stop() -> 1 blok
 * MZF (DEFAULT), páska Modified, otevřený blok 0, PLAY ho hraje; zpráva
 * nese jméno. Nová páska bez záznamu zůstane prázdná a ne Modified.
 */
void test_record_into_empty_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* bez záznamu: prázdná páska, ne Modified */
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    rec_start();
    cmt_stop();
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, cmtext_container_get_count_blocks(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_STRING("Recording too short, nothing stored", cmttmz_get_record_message());
    cmt_eject();

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "RECORDED");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();

    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmtext_container_get_count_blocks(c), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(1, (int)cmttmz_get_file()->block_count);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    rec_check_block_mzf(0, &m);
    TEST_ASSERT_EQUAL_STRING("Recorded \"RECORDED\", 256 B", cmttmz_get_record_message());

    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_EQUAL_INT(0, cmtext_block_get_block_id(g_cmt.ext->block));
    cmt_stop();
}

/*
 * Páska MZT se dvěma bloky, otevřený blok 1 ->
 * po záznamu 3 bloky, index 3 položky, nový blok DEFAULT, otevřený blok 2;
 * stav playeru jde přečíst a znovu nastavit; dosavadní poslední blok 1
 * (pauza 0) dostal pauzu 1 s (stream o 1 s delší); Modified.
 */
void test_record_appends_to_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    uint64_t len1 = cmtext_block_get_count_scans(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    const st_TZX_FILE *f_before = cmttmz_get_file();

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "THIRD");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();

    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, cmtext_container_get_count_blocks(c), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_PTR(f_before, cmttmz_get_file());
    TEST_ASSERT_EQUAL_INT(3, (int)cmttmz_get_file()->block_count);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 2));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 2));
    TEST_ASSERT_EQUAL_STRING("THIRD", cmtext_container_get_block_fname(c, 2));
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    rec_check_block_mzf(2, &m);

    st_TMZ_PLAYER_STATE st;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&st));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_set_player_state(&st));

    /* blok 1 (dřív poslední, pauza 0) má teď za sebou 1 s ticha */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    uint64_t len1b = cmtext_block_get_count_scans(g_cmt.ext->block);
    TEST_ASSERT_UINT64_WITHIN(GDGCLK_BASE / 1000, len1 + GDGCLK_BASE, len1b);
    /* blok 0 (pauza 1000 z importu) se nezměnil */
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0));
}

/*
 * Surový blok ze záznamu (šum) má v indexu typ TZX (ne WAV: přepínač
 * polarity na něj nepůsobí) a rychlost NONE.
 */
void test_record_raw_block_is_tzx_type(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(500);
    cmt_stop();
    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmtext_container_get_count_blocks(c), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, cmttmz_get_file()->blocks[0].id);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_NONE, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_STRING("Recording could not be decoded, stored as a raw block", cmttmz_get_record_message());
}

/*
 * Hrany při stojícím motoru (pauza) se nezaznamenají a čas
 * stání se do záznamu nepočítá.
 */
void test_record_motor_stopped_time_excluded(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    /* pauza od začátku: hrany se zahodí */
    cmt_write_data(1);
    advance_ticks(1000);
    cmt_write_data(0);
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_rec_get()->count);

    cmt_pause(0);
    cmt_write_data(1);     /* první hrana */
    advance_ticks(500);
    cmt_write_data(0);     /* úsek 500 */
    advance_ticks(300);
    cmt_pause(1);          /* motor stojí: 300 taktů úseku už uplynulo */
    advance_ticks(1000000);
    cmt_write_data(1);     /* zahozeno */
    cmt_write_data(0);
    cmt_pause(0);
    advance_ticks(200);
    cmt_write_data(1);     /* úsek 300 + 200, stání se nepočítá */
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(2, rec->count);
    TEST_ASSERT_EQUAL_UINT32(500, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(500, rec->ticks[1]);
    TEST_ASSERT_EQUAL_UINT64(1000, rec->total_ticks);
    cmt_stop();
}

/** @brief Přečte z WAV vzorkovací frekvenci a počet vzorků (RIFF, chunk fmt a data). */
static void rec_wav_info(const char *path, uint32_t *rate, uint32_t *samples)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_TRUE(len >= 12 && 0 == memcmp(data, "RIFF", 4) && 0 == memcmp(data + 8, "WAVE", 4));
    const uint8_t *d = (const uint8_t *)data;
    uint32_t block_align = 0, data_size = 0;
    *rate = 0;
    for (gsize pos = 12; pos + 8 <= len;) {
        uint32_t sz = d[pos + 4] | (d[pos + 5] << 8) | (d[pos + 6] << 16) | ((uint32_t)d[pos + 7] << 24);
        if (0 == memcmp(d + pos, "fmt ", 4) && pos + 8 + 16 <= len) {
            *rate = d[pos + 12] | (d[pos + 13] << 8) | (d[pos + 14] << 16) | ((uint32_t)d[pos + 15] << 24);
            block_align = d[pos + 20] | (d[pos + 21] << 8);
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

/*
 * cmt_record_to_file("x.wav") -> po STOP soubor existuje,
 * 44 100 Hz, počet vzorků = takty záznamu převedené na 44 100 Hz (± 1);
 * páska má navíc blok. Nepodporovaná přípona = chyba, soubor nevznikne.
 */
void test_record_to_wav_file(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *bad = tmp_path(1, "xyz");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_record_to_file(bad));
    TEST_ASSERT_FALSE(g_file_test(bad, G_FILE_TEST_EXISTS));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    const char *wav = tmp_path(1, "wav");
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "TOWAV");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS)); /* zkouška zapisovatelnosti nechá prázdný soubor */
    cmt_pause(0);
    uint64_t total = rec_write_rom(&m);
    cmt_stop();

    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    uint32_t rate = 0, samples = 0;
    rec_wav_info(wav, &rate, &samples);
    TEST_ASSERT_EQUAL_UINT32(44100, rate);
    uint64_t want = (total * 44100u + GDGCLK_BASE / 2) / GDGCLK_BASE;
    TEST_ASSERT_UINT64_WITHIN(1, want, samples);
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(g_cmt.ext->container));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_record_message(), "WAV saved"));
}

/*
 * Mez bufferu (zmenšená na 100 položek) ->
 * snímek emulace (cmt_screen_done_period()) záznam sám ukončí jako STOP,
 * zpráva začíná "Recording stopped: length limit reached", záznam je na pásce.
 */
/*
 * cmt_record_to_file() s příponou pásky (.tmz): zkouška zapisovatelnosti
 * soubor nenechá; po STOP se celá páska uloží (cmttmz_save()): soubor
 * existuje, cesta pásky je on, Modified 0, otevření dá týž blok.
 */
void test_record_to_tape_file(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *tmz = tmp_path(1, "tmz");
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "TOTAPE");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(tmz));
    TEST_ASSERT_FALSE(g_file_test(tmz, G_FILE_TEST_EXISTS));
    cmt_pause(0);
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(tmz, G_FILE_TEST_EXISTS), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_STRING(tmz, cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_record_message(), "Tape saved"));
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)tmz));
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(g_cmt.ext->container));
    rec_check_block_mzf(0, &m);
}

void test_record_limit_stops(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmttmz_rec_test_set_limits(100, 0);
    rec_start();
    rec_write_noise(300);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
    TEST_ASSERT_EQUAL_UINT32(100, cmttmz_rec_get()->count);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_screen_done_period();
    cmttmz_rec_test_set_limits(0, 0);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    const char *msg = cmttmz_get_record_message();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(msg, "Recording stopped: length limit reached", 39), msg);
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_is_active());
}


/* ================================================================
 * ULOŽENÍ PÁSKY A EXPORT BLOKU (cmttmz_save(), cmttmz_export_block())
 * ================================================================ */

/** @brief Dočasná cesta mimo sloty s_paths (g_free()); soubor smaže volající. */
static char *b4_path(const char *name)
{
    gchar *n = g_strdup_printf("mztest_cmt_b4_%u_%s", (unsigned)g_random_int(), name);
    char *p = g_build_filename(g_get_tmp_dir(), n, NULL);
    g_free(n);
    return p;
}

/** @brief Kopie MZF všech bloků vložené pásky (pole count položek, uvolnit b4_mzfs_free()). */
static st_MZF **b4_mzfs(int *count)
{
    *count = (int)cmttmz_get_file()->block_count;
    st_MZF **a = g_new0(st_MZF *, *count ? *count : 1);
    for (int i = 0; i < *count; i++) a[i] = cmttmz_block_get_mzf(i);
    return a;
}

/** @brief Uvolní pole z b4_mzfs(). */
static void b4_mzfs_free(st_MZF **a, int count)
{
    for (int i = 0; i < count; i++) if (a[i]) mzf_free(a[i]);
    g_free(a);
}

/** @brief Ověří, že MZF jsou shodná (hlavička i tělo). */
static void b4_mzf_equal(const st_MZF *a, const st_MZF *b)
{
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_MEMORY(&a->header, &b->header, sizeof(a->header));
    TEST_ASSERT_EQUAL_UINT32(a->header.fsize, b->header.fsize);
    TEST_ASSERT_EQUAL_MEMORY(a->body, b->body, a->header.fsize);
}

/*
 * Import MZT -> cmttmz_save("a.tmz") -> Modified 0, cesta pásky
 * je a.tmz -> otevření a.tmz: stejné bloky, MZF každého shodné.
 */
void test_save_tmz_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 3)));
    int n = 0;
    st_MZF **before = b4_mzfs(&n);
    TEST_ASSERT_EQUAL_INT(3, n);
    cmttmz_set_modified(1);

    char *tmz = b4_path("a.tmz");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_save(tmz), cmttmz_get_save_message());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_STRING(tmz, cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_STRING(tmz, g_cmt.last_filename);

    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(tmz));
    int m = 0;
    st_MZF **after = b4_mzfs(&m);
    TEST_ASSERT_EQUAL_INT(n, m);
    for (int i = 0; i < n; i++) b4_mzf_equal(before[i], after[i]);
    b4_mzfs_free(before, n);
    b4_mzfs_free(after, m);

    /* NULL = dosavadní cesta; nová páska bez cesty = chyba */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(NULL));
    cmt_eject();
    g_remove(tmz);
    g_free(tmz);
    rec_start();
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(NULL));
    TEST_ASSERT_EQUAL_STRING("Tape has no file name", cmttmz_get_save_message());
}

/*
 * Surový blok ze záznamu (0x18) má i po uložení jako TMZ a novém otevření
 * typ TZX (rozhodnutí řízení: nese úroveň PC1, ne WAV). Uložení s příponou
 * .tzx s bloky 0x4x nese varování.
 */
void test_save_raw_record_reopens_as_tzx_type(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "MIXREC");
    rec_start();
    rec_write_noise(300);
    rec_write_rom(&m);
    cmt_stop();
    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cmtext_container_get_count_blocks(c), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 1));

    char *tmz = b4_path("raw.tmz");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_save(tmz), cmttmz_get_save_message());
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(tmz));
    c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(c, 0));
    rec_check_block_mzf(1, &m);

    char *tzx = b4_path("raw.tzx");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(tzx));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_save_message(),
                                        "Tape contains MZ blocks; file saved in TMZ format with .tzx extension"),
                                 cmttmz_get_save_message());
    cmt_eject();
    g_remove(tmz);
    g_remove(tzx);
    g_free(tmz);
    g_free(tzx);
}

/** @brief Analyzuje WAV soubor a vrátí výsledek (uvolnit wav_analyzer_result_destroy()). */
static void b4_analyze_wav(const char *path, st_WAV_ANALYZER_RESULT *res)
{
    st_HANDLER h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory_from_file(&h, &g_memory_driver_realloc, path));
    st_WAV_ANALYZER_CONFIG cfg;
    wav_analyzer_config_default(&cfg);
    memset(res, 0, sizeof(*res));
    TEST_ASSERT_EQUAL_INT(WAV_ANALYZER_OK, wav_analyzer_analyze(&h, &cfg, res));
    generic_driver_close(&h);
}

/*
 * Páska TZX (ZXTape!) s připojeným záznamem 0x46 dostane hlavičku TMZ 1.1;
 * uložení jako .tzx varuje a soubor je TMZ.
 */
void test_record_into_tzx_makes_tmz(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    memcpy(f->header.signature, TZX_SIGNATURE, TZX_SIGNATURE_LENGTH);
    f->header.ver_major = 1;
    f->header.ver_minor = 20;
    f->is_tmz = false;
    add_text_block(f, "zx tape");
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    const char *tzx = tmp_path(0, "tzx");
    TEST_ASSERT_TRUE(g_file_set_contents(tzx, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)tzx));
    TEST_ASSERT_FALSE(cmttmz_get_file()->is_tmz);
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "TOZX");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    const st_TZX_FILE *t = cmttmz_get_file();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)t->block_count, cmttmz_get_record_message());
    TEST_ASSERT_TRUE(t->is_tmz);
    TEST_ASSERT_EQUAL_MEMORY("TapeMZ!", t->header.signature, 7);
    TEST_ASSERT_EQUAL_UINT8(1, t->header.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, t->header.ver_minor);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(NULL));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_save_message(), "saved in TMZ format with .tzx extension"));
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(tzx, &data, &len, NULL));
    TEST_ASSERT_TRUE(len > 7);
    TEST_ASSERT_EQUAL_MEMORY("TapeMZ!", data, 7);
    g_free(data);
}

/*
 * Blok po záznamu exportovaný jako .mzf je bajtově shodný
 * se vstupním MZF; export .wav jde otevřít a wav_analyzer_analyze() z něj
 * dekóduje NORMAL se stejným MZF. Modified a cesta se nemění; blok bez
 * MZF jako .mzf = chyba.
 */
void test_export_block_mzf_and_wav(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "EXPORT");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    char *mzf = b4_path("e.mzf");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_block(0, mzf), cmttmz_get_save_message());
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(mzf, &data, &len, NULL));
    TEST_ASSERT_EQUAL_size_t(128 + REC_BODY_SIZE, len);
    TEST_ASSERT_EQUAL_MEMORY(m.header, data, 128);
    TEST_ASSERT_EQUAL_MEMORY(m.body, data + 128, REC_BODY_SIZE);
    g_free(data);

    char *wav = b4_path("e.wav");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_export_block(0, wav), cmttmz_get_save_message());
    st_WAV_ANALYZER_RESULT res;
    b4_analyze_wav(wav, &res);
    TEST_ASSERT_EQUAL_UINT32(1, res.file_count);
    TEST_ASSERT_EQUAL_INT(WAV_TAPE_FORMAT_NORMAL, res.files[0].format);
    TEST_ASSERT_EQUAL_INT(WAV_CRC_OK, res.files[0].header_crc);
    TEST_ASSERT_EQUAL_INT(WAV_CRC_OK, res.files[0].body_crc);
    st_MZF_HEADER hdr;
    memcpy(&hdr, &res.files[0].mzf->header, sizeof(hdr));
    mzf_header_swap_endianity(&hdr);
    TEST_ASSERT_EQUAL_MEMORY(m.header, &hdr, 128);
    TEST_ASSERT_EQUAL_MEMORY(m.body, res.files[0].mzf->body, REC_BODY_SIZE);
    wav_analyzer_result_destroy(&res);

    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));

    /* surový blok nejde exportovat jako MZF */
    rec_start();
    rec_write_noise(300);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(g_cmt.ext->container));
    char *bad = b4_path("raw.mzf");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_export_block(1, bad));
    TEST_ASSERT_FALSE(g_file_test(bad, G_FILE_TEST_EXISTS));

    g_remove(mzf);
    g_remove(wav);
    g_free(mzf);
    g_free(wav);
    g_free(bad);
}

/*
 * Páska se surovým blokem jako .mzt = chyba, soubor nevznikne;
 * páska se dvěma bloky jako .mzf = chyba; páska s jedním MZF jako .mzf =
 * bajtově týž soubor.
 */
void test_save_mzt_and_mzf_rules(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(300);
    cmt_stop();
    char *mzt = b4_path("raw.mzt");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(mzt));
    TEST_ASSERT_EQUAL_STRING("Tape contains blocks that cannot be stored as MZT", cmttmz_get_save_message());
    TEST_ASSERT_FALSE(g_file_test(mzt, G_FILE_TEST_EXISTS));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    cmt_eject();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    char *mzf = b4_path("two.mzf");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(mzf));
    TEST_ASSERT_EQUAL_STRING("Only a tape with a single MZF block can be saved as MZF", cmttmz_get_save_message());
    TEST_ASSERT_FALSE(g_file_test(mzf, G_FILE_TEST_EXISTS));
    char *unk = b4_path("tape.xyz");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(unk));
    TEST_ASSERT_FALSE(g_file_test(unk, G_FILE_TEST_EXISTS));

    const char *src = save_mzf(1, "ONE");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)src));
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_save(mzf), cmttmz_get_save_message());
    gchar *a = NULL, *b = NULL;
    gsize la = 0, lb = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(src, &a, &la, NULL));
    TEST_ASSERT_TRUE(g_file_get_contents(mzf, &b, &lb, NULL));
    TEST_ASSERT_EQUAL_size_t(la, lb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, la);
    g_free(a);
    g_free(b);
    cmt_eject();
    g_remove(mzf);
    g_free(mzt);
    g_free(mzf);
    g_free(unk);
}

/*
 * MZT kruh s rychlostí (UniCMT): MZT [A][CMTSPEED 2x][B][C] -> import ->
 * uložit .mzt -> soubor bajtově shodný se vstupem; po novém importu blok A
 * DEFAULT, B a C SET s pulzy 2x.
 */
void test_save_mzt_roundtrip_with_cmtspeed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *one = save_mzt(0, 3);   /* tři MZF P0, P1, P2 */
    gchar *raw = NULL;
    gsize rlen = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(one, &raw, &rlen, NULL));
    const size_t rec = 128 + TEST_BODY_SIZE;
    TEST_ASSERT_EQUAL_size_t(3 * rec, rlen);
    uint16_t two[4] = { 235, 247, 120, 139 };
    uint8_t hdr[TMZ_MZT_HEADER_SIZE];
    tmz_mzt_build_cmtspeed(two, hdr);
    GByteArray *in = g_byte_array_new();
    g_byte_array_append(in, (const guint8 *)raw, (guint)rec);
    g_byte_array_append(in, hdr, TMZ_MZT_HEADER_SIZE);
    g_byte_array_append(in, (const guint8 *)raw + rec, (guint)(2 * rec));
    g_free(raw);
    char *src = b4_path("speed_in.mzt");
    TEST_ASSERT_TRUE(g_file_set_contents(src, (const gchar *)in->data, (gssize)in->len, NULL));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(src));
    char *out = b4_path("speed_out.mzt");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_save(out), cmttmz_get_save_message());
    gchar *got = NULL;
    gsize glen = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(out, &got, &glen, NULL));
    TEST_ASSERT_EQUAL_size_t(in->len, glen);
    TEST_ASSERT_EQUAL_MEMORY(in->data, got, glen);
    g_free(got);

    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(out));
    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT(3, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(c, 0));
    for (int i = 1; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, i));
        st_MZTAPE_PULSES_LENGTH p;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_container_get_block_pulses(c, i, &p));
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, 235e-6, p.long_pulse.high);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, 139e-6, p.short_pulse.low);
    }
    cmt_eject();
    g_byte_array_free(in, TRUE);
    g_remove(src);
    g_remove(out);
    g_free(src);
    g_free(out);
}

/*
 * Uložení do neexistujícího adresáře = chyba,
 * Modified zůstane 1, cesta pásky se nezmění; cíl je adresář stejného jména
 * = chyba, adresář zůstane; cíl je soubor jen pro čtení = chyba a soubor má
 * původní obsah. Část se souborem jen pro čtení se mimo Windows pod rootem
 * vynechá (root smí zapisovat i do souboru s právy 0444).
 */
void test_save_failure_keeps_original(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(300);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    char *dir = b4_path("nodir");
    gchar *in_missing = g_build_filename(dir, "x.tmz", NULL);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(in_missing));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));

    char *asdir = b4_path("isdir.tmz");
    TEST_ASSERT_EQUAL_INT(0, g_mkdir(asdir, 0700));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_save(asdir));
    TEST_ASSERT_TRUE(g_file_test(asdir, G_FILE_TEST_IS_DIR));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    char *ro = b4_path("readonly.tmz");
    bool ro_enforced = true;
#ifndef _WIN32
    ro_enforced = (geteuid() != 0);
#endif
    if (ro_enforced) {
        TEST_ASSERT_TRUE(g_file_set_contents(ro, "ORIGINAL", 8, NULL));
        TEST_ASSERT_EQUAL_INT(0, g_chmod(ro, 0444));
        int rc = cmttmz_save(ro);
        gchar *data = NULL;
        gsize len = 0;
        TEST_ASSERT_TRUE(g_file_get_contents(ro, &data, &len, NULL));
        g_chmod(ro, 0666);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_FAILURE, rc, "saving over a read-only file must fail");
        TEST_ASSERT_EQUAL_size_t(8, len);
        TEST_ASSERT_EQUAL_MEMORY("ORIGINAL", data, 8);
        TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
        g_free(data);
        g_remove(ro);
    }

    g_rmdir(asdir);
    g_free(dir);
    g_free(in_missing);
    g_free(asdir);
    g_free(ro);
}


/* ================================================================
 * ZÁZNAM A ULOŽENÍ: připojení uprostřed, zámek, atomický zápis, MZT
 * ================================================================ */

/** @brief Počet bloků, který viděl čtenář indexu ve druhém vlákně (-1 = ještě nečetl). */
static gint s_reader_count = -1;
/** @brief 1 = čtenář získal zámek stavu zařízení. */
static gint s_reader_locked = 0;
/** @brief Vlákno čtenáře spuštěné háčkem připojení. */
static GThread *s_reader_thread = NULL;
/** @brief 1 = čtenář nezískal zámek během výměny indexu (háček to ověřil). */
static gint s_reader_blocked_in_hook = 0;

/** @brief Čtenář indexu jako UI: zámek stavu zařízení, počet bloků, odemčení. */
static gpointer fr_reader(gpointer data)
{
    (void)data;
    device_state_lock();
    g_atomic_int_set(&s_reader_locked, 1);
    g_atomic_int_set(&s_reader_count, cmtext_container_get_count_blocks(g_cmt.ext->container));
    device_state_unlock();
    return NULL;
}

/** @brief Háček v cmttmz_append_blocks() po výměně indexu: spustí čtenáře a ověří, že čeká na zámek. */
static void fr_append_hook(void)
{
    if (s_reader_thread) return;
    s_reader_thread = g_thread_new("fr-reader", fr_reader, NULL);
    g_usleep(100000);
    if (!g_atomic_int_get(&s_reader_locked)) g_atomic_int_set(&s_reader_blocked_in_hook, 1);
}

/** @brief Počká na příznak (nejvýš 2 s). @return true = nastaven. */
static bool fr_wait_flag(gint *flag)
{
    for (int i = 0; i < 200; i++) {
        if (g_atomic_int_get(flag)) return true;
        g_usleep(10000);
    }
    return false;
}

/** @brief Vlákno, které jen vezme a pustí zámek stavu zařízení. */
static gpointer fr_lock_once(gpointer data)
{
    device_state_lock();
    device_state_unlock();
    g_atomic_int_set((gint *)data, 1);
    return NULL;
}

/** @brief Ověří, že zámek stavu zařízení je volný (jiné vlákno ho do 2 s získá). */
static void fr_assert_lock_free(void)
{
    static gint done;
    g_atomic_int_set(&done, 0);
    GThread *t = g_thread_new("fr-lock", fr_lock_once, &done);
    TEST_ASSERT_TRUE_MESSAGE(fr_wait_flag(&done), "device state lock is still held");
    g_thread_join(t);
}

/*
 * Páska přehraná doprostřed bloku a pozastavená (RECORD
 * v pauze PLAY nejde), zastavená s pozicí uprostřed bloku -> záznam:
 * index a vedlejší pole (rychlosti, skip) všech bloků, stav playeru;
 * čtenář indexu pod zámkem (druhé vlákno) při výměně čeká a vidí nový
 * index; pak 300 bloků najednou (přes každou počáteční kapacitu) a zpětné
 * čtení každé položky.
 */
void test_record_append_mid_block_side_arrays(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 3)));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    uint64_t len1 = cmtext_block_get_count_scans(g_cmt.ext->block);
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(len1 / 2);
    (void)cmt_read_data();
    cmt_pause(1);
    int blk = -1;
    uint64_t pos = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_get_position(&blk, &pos));
    TEST_ASSERT_EQUAL_INT(1, blk);
    TEST_ASSERT_TRUE(pos > 0 && pos < len1);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_record()); /* RECORD jen ve STOP */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, len1 / 2));
    st_TMZ_PLAYER_STATE before;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&before));

    s_reader_thread = NULL;
    g_atomic_int_set(&s_reader_locked, 0);
    g_atomic_int_set(&s_reader_count, -1);
    g_atomic_int_set(&s_reader_blocked_in_hook, 0);
    cmttmz_test_set_append_hook(fr_append_hook);
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "MIDDLE");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    cmttmz_test_set_append_hook(NULL);
    TEST_ASSERT_NOT_NULL(s_reader_thread);
    g_thread_join(s_reader_thread);
    s_reader_thread = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_atomic_int_get(&s_reader_blocked_in_hook), "reader got the lock during the index swap");
    TEST_ASSERT_EQUAL_INT(4, g_atomic_int_get(&s_reader_count));

    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT(4, cmtext_container_get_count_blocks(c));
    TEST_ASSERT_EQUAL_INT(3, cmtext_block_get_block_id(g_cmt.ext->block));
    {
        /* jeden blok navíc: _msize() vrací přesně požadovanou velikost, bez realloc() by pole měla 3 položky */
        long long sp_b = 0, sk_b = 0, ix_b = 0;
        if (EXIT_SUCCESS == cmttmz_test_get_side_alloc(&sp_b, &sk_b, &ix_b)) {
            TEST_ASSERT_TRUE(sp_b >= (long long)(4 * sizeof(st_CMTTMZ_BLOCK_SPEED)));
            TEST_ASSERT_TRUE(sk_b >= 4);
            TEST_ASSERT_TRUE(ix_b >= (long long)(4 * sizeof(st_CMTEXT_TAPE_INDEX)));
        }
    }
    for (int i = 0; i < 4; i++) {
        st_CMTTMZ_BLOCK_SPEED sp;
        int skip = -1;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(i, &sp, &skip));
        TEST_ASSERT_EQUAL_INT(0, skip);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp.origin);
        TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, sp.speed);
        /* bloky 0, 1 z importu 1000 ms, blok 2 dostal mezeru 1000 ms, nový poslední 0 */
        TEST_ASSERT_EQUAL_UINT16((i < 3) ? 1000 : 0, sp.pause_ms);
        TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, i));
    }
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_test_get_side(4, NULL, NULL));
    st_TMZ_PLAYER_STATE st;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_player_state(&st));
    TEST_ASSERT_EQUAL_PTR(before.file, st.file);
    TEST_ASSERT_EQUAL_UINT32(3, st.last_played_block);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_set_player_state(&st));

    /* 300 bloků najednou, rozlišené pauzou; zpětné čtení všech položek */
    enum { MANY = 300 };
    st_TZX_BLOCK *blocks = g_new0(st_TZX_BLOCK, MANY);
    st_CMTTMZ_BLOCK_SPEED *speeds = g_new0(st_CMTTMZ_BLOCK_SPEED, MANY);
    for (int i = 0; i < MANY; i++) {
        st_MZF_HEADER hdr;
        char name[8];
        snprintf(name, sizeof(name), "B%03d", i);
        make_mzf_header(&hdr, name);
        uint8_t body[TEST_BODY_SIZE];
        memset(body, i & 0xFF, sizeof(body));
        speeds[i].origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
        speeds[i].speed = CMTSPEED_1_1;
        speeds[i].pause_ms = (uint16_t)(100 + i);
        st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, TEST_BODY_SIZE, CMTSPEED_1_1, NULL, speeds[i].pause_ms);
        TEST_ASSERT_NOT_NULL(b);
        blocks[i] = *b;
        b->data = NULL;
        tmz_block_free(b);
    }
    device_state_lock();
    int rc = cmttmz_append_blocks(blocks, speeds, MANY);
    device_state_unlock();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, rc);
    for (int i = 0; i < MANY; i++) TEST_ASSERT_NULL(blocks[i].data);
    g_free(blocks);
    g_free(speeds);
    TEST_ASSERT_EQUAL_INT(4 + MANY, cmtext_container_get_count_blocks(c));
    /* skutečné alokace (z alokátoru, ne z počtu bloků): chybějící realloc()
     * by tu selhal deterministicky - pole importu měla 3 položky */
    long long sp_bytes = 0, sk_bytes = 0, ix_bytes = 0;
    if (EXIT_SUCCESS == cmttmz_test_get_side_alloc(&sp_bytes, &sk_bytes, &ix_bytes)) {
        TEST_ASSERT_TRUE_MESSAGE(sp_bytes >= (long long)((4 + MANY) * sizeof(st_CMTTMZ_BLOCK_SPEED)), "speeds array not grown");
        TEST_ASSERT_TRUE_MESSAGE(sk_bytes >= (long long)(4 + MANY), "skip array not grown");
        TEST_ASSERT_TRUE_MESSAGE(ix_bytes >= (long long)((4 + MANY) * sizeof(st_CMTEXT_TAPE_INDEX)), "index not grown");
    } else {
#if defined(_WIN32) || defined(__GLIBC__)
        TEST_FAIL_MESSAGE("allocation size not available");
#endif
    }
    for (int i = 0; i < 4 + MANY; i++) {
        st_CMTTMZ_BLOCK_SPEED sp;
        int skip = -1;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(i, &sp, &skip));
        TEST_ASSERT_EQUAL_INT(0, skip);
        if (i >= 4) {
            TEST_ASSERT_EQUAL_UINT16(100 + (i - 4), sp.pause_ms);
            char name[8];
            snprintf(name, sizeof(name), "B%03d", i - 4);
            TEST_ASSERT_EQUAL_STRING(name, cmtext_container_get_block_fname(c, i));
        }
    }
    /* dosavadní poslední blok (záznam, pauza 0) dostal mezeru */
    st_CMTTMZ_BLOCK_SPEED sp3;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(3, &sp3, NULL));
    TEST_ASSERT_EQUAL_UINT16(1000, sp3.pause_ms);
}

/*
 * Zámek stavu zařízení už drží volající (GUI přes frontu zásahů, MCP přes
 * dbgapi): cmt_record() s novou páskou, cmt_stop() v RECORD a
 * cmt_record_to_file("x.tmz") s uložením po STOP nezamrznou a nechají
 * zámek držený právě jednou.
 */
void test_record_with_lock_already_held(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    device_state_lock();
    rec_start();               /* cmt_record() vloží novou pásku pod zámkem */
    rec_write_noise(200);
    cmt_stop();                /* připojení pod zámkem */
    TEST_ASSERT_EQUAL_INT(1, cmtext_container_get_count_blocks(g_cmt.ext->container));
    const char *tmz = tmp_path(1, "tmz");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(tmz));
    cmt_pause(0);
    rec_write_noise(200);
    cmt_stop();                /* připojení a uložení pásky pod zámkem */
    TEST_ASSERT_TRUE(device_state_lock_held());
    device_state_unlock();
    TEST_ASSERT_FALSE(device_state_lock_held());
    fr_assert_lock_free();
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(g_cmt.ext->container));
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(tmz, G_FILE_TEST_EXISTS), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}

/*
 * Selhání mezi zápisem dočasného souboru a nahrazením cíle
 * (testovací háček) nechá původní soubor bajtově shodný a žádný dočasný
 * soubor; Modified zůstane.
 */
void test_save_atomic_failure_before_replace(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(300);
    cmt_stop();
    gchar *dir = g_dir_make_tmp("mztest_cmt_atomic_XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(dir);
    gchar *path = g_build_filename(dir, "orig.tmz", NULL);
    static const char orig[] = "ORIGINAL CONTENT 0123456789";
    TEST_ASSERT_TRUE(g_file_set_contents(path, orig, sizeof(orig), NULL));

    cmttmz_test_fail_before_replace(1);
    int rc = cmttmz_save(path);
    cmttmz_test_fail_before_replace(0);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, rc);
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_EQUAL_size_t(sizeof(orig), len);
    TEST_ASSERT_EQUAL_MEMORY(orig, data, len);
    g_free(data);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    GDir *d = g_dir_open(dir, 0, NULL);
    TEST_ASSERT_NOT_NULL(d);
    int files = 0;
    const gchar *n;
    while ((n = g_dir_read_name(d))) files++;
    g_dir_close(d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, files, "a temporary file was left behind");

    /* bez háčku nahrazení projde */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(path));
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_EQUAL_MEMORY("TapeMZ!", data, 7);
    g_free(data);
    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

/* Mez bufferu dosažena, pak se zastaví motor (pauza): snímek záznam stejně ukončí. */
void test_record_limit_then_motor_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmttmz_rec_test_set_limits(100, 0);
    rec_start();
    rec_write_noise(300);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    cmt_screen_done_period();
    cmttmz_rec_test_set_limits(0, 0);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, strncmp(cmttmz_get_record_message(), "Recording stopped: length limit reached", 39));
}

/*
 * Cesta souboru záznamu nepřežije záznam ukončený bez cmt_record_finish()
 * (snapshot: cmt_sanitize_state() bez bufferu): další cmt_record() WAV
 * nezapíše. cmt_record_to_file() s příponou pásky bez záznamu nic neuloží.
 */
void test_record_path_not_reused(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *wav = tmp_path(1, "wav");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    /* konec záznamu bez cmt_record_finish(): buffer pryč, sanitize vrátí STOP */
    cmttmz_rec_discard();
    TEST_ASSERT_TRUE(cmt_sanitize_state());
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "NOPATH");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    gchar *data = NULL;
    gsize len = 99;
    TEST_ASSERT_TRUE(g_file_get_contents(wav, &data, &len, NULL));
    TEST_ASSERT_EQUAL_size_t(0, len); /* jen prázdný soubor ze zkoušky */
    g_free(data);
    TEST_ASSERT_NULL(strstr(cmttmz_get_record_message(), "WAV saved"));

    const char *tmz = tmp_path(0, "tmz");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(tmz));
    cmt_pause(0);
    cmt_stop();
    TEST_ASSERT_FALSE(g_file_test(tmz, G_FILE_TEST_EXISTS));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_record_message(), "nothing recorded"));
}

/* ================================================================
 * SNAPSHOT NEULOŽENÉ PÁSKY A NAHRÁVÁNÍ
 * ================================================================ */

/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_count(void)
{
    if (!CMT_TEST_FILLED) return -1;
    return cmtext_container_get_count_blocks(g_cmt.ext->container);
}

/*
 * Záznam do nové pásky -> snapshot -> nová páska -> načtení:
 * páska má zaznamenaný blok (MZF bajtově, rychlost DEFAULT jako před
 * snapshotem), je Modified a nemá cestu. Páska ze souboru s připojeným
 * záznamem: obraz má přednost před souborem (ten má o blok méně),
 * Modified 1; cestu ze snapshotu načteného uživatelem páska nedostane,
 * jméno souboru je jen ve zprávě.
 */
void test_snapshot_embeds_unsaved_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "EMBED");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_count(), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(g_cmt.ext->container, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(g_cmt.ext->container, 0));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    rec_check_block_mzf(0, &m);

    /* páska ze souboru (2 bloky) + záznam = 3 bloky, soubor beze změny */
    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    snap_save(&buf, &size);
    cmt_eject();
    snap_cmt_test_clear_warnings();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    gchar *base = g_path_get_basename(path);
    gchar *want = g_strdup_printf("Unsaved tape restored from the snapshot (was: %s)", base);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(snap_cmt_test_warnings(), want), snap_cmt_test_warnings());
    g_free(want);
    g_free(base);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(g_cmt.ext->container, 2));
    rec_check_block_mzf(2, &m);

    /* páska ze souboru bez změn: jen cesta (soubor se otevře, ne Modified) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}

/*
 * Mez vloženého obrazu (pro test zmenšená): větší páska se
 * nevloží (varování při uložení), načtení se zachová jako u chybějícího
 * souboru - bez pásky, STOP. Poškozený obraz (znak mimo base64, platný
 * base64, který není páska) totéž, bez pádu.
 */
void test_snapshot_embed_limit_and_invalid_image(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "LIMIT");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(1, tape_count());

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_cmt_test_set_embed_limit(64);
    snap_save(&buf, &size);
    snap_cmt_test_set_embed_limit(0);
    snap_load(buf, size);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    for (int mode = 1; mode <= 2; mode++) {
        rec_start();
        rec_write_rom(&m);
        cmt_stop();
        TEST_ASSERT_EQUAL_INT(1, tape_count());
        snap_cmt_test_corrupt_image(mode);
        snap_save(&buf, &size);
        snap_cmt_test_corrupt_image(0);
        snap_load(buf, size);
        TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_FILLED, mode == 1 ? "bad base64" : "bad image");
        TEST_ASSERT_TRUE(CMT_TEST_STOP);
    }
}

/*
 * Snapshot během nahrávání: buffer hran soubor nenese, načtení dá STOP
 * bez aktivního záznamu (páska z obrazu, bez zaznamenaného bloku).
 * Načtení snapshotu během nahrávání nahrávání zahodí (nic se nepřipojí).
 */
void test_snapshot_during_record(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "DURING");
    rec_start();
    rec_write_noise(500);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    uint8_t *buf2 = g_memdup2(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);

    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(cmttmz_rec_is_active());
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (a snapshot was loaded)", cmttmz_get_record_message());

    /* znovu nahrávat a načíst: rozpracovaný záznam zmizí, páska bez bloků */
    rec_start();
    rec_write_rom(&m);
    TEST_ASSERT_TRUE(cmttmz_rec_is_active());
    snap_load(buf2, size);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(0, tape_count());
}

/*
 * Značka pásky (checkpoint záznamu vstupů): připojení záznamu se vrátí
 * zkrácením - počet bloků, pauza posledního bloku (připojení jí dává
 * 1000 ms), Modified; otevřený blok za značkou se zavře.
 */
void test_tape_mark_restore_truncates(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    st_CMTTMZ_BLOCK_SPEED before;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &before, NULL));
    uint32_t len1 = cmttmz_get_file()->blocks[1].length;
    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    TEST_ASSERT_EQUAL_UINT32(2, mark.block_count);

    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "MARK");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_INT(2, cmtext_block_get_block_id(g_cmt.ext->block));
    st_CMTTMZ_BLOCK_SPEED after;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &after, NULL));
    TEST_ASSERT_NOT_EQUAL(before.pause_ms, after.pause_ms);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_UINT32(2, cmttmz_get_file()->block_count);
    TEST_ASSERT_NULL(g_cmt.ext->block);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &after, NULL));
    TEST_ASSERT_EQUAL_UINT16(before.pause_ms, after.pause_ms);
    TEST_ASSERT_EQUAL_UINT32(len1, cmttmz_get_file()->blocks[1].length);

    /* opakované nahrávání dá zase 3 bloky se stejným MZF */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    rec_check_block_mzf(2, &m);

    /* značka delší než páska = jiná páska */
    mark.block_count = 7;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
}

/* ================================================================
 * AUTOMATICKÉ UKONČENÍ ZÁZNAMU (INI [CMT] rec_autostop_s)
 * ================================================================ */

/**
 * @brief Nechá uplynout @p ms ms emulovaného času po 10 ms, každý krok cmt_screen_done_period().
 * @param ms Doba v ms.
 */
static void run_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms / 10u; i++) {
        advance_ticks((uint64_t)GDGCLK_BASE / 100u);
        g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
        g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
        cmt_screen_done_period();
    }
}

/*
 * S rec_autostop_s 2 se záznam ukončí (jako STOP: dekódování,
 * připojení bloku) po 2 s emulovaného času bez hrany při běžícím motoru
 * a jen po aspoň jedné hraně; při stojícím motoru čas neběží; s hodnotou
 * 0 se neukončí nikdy.
 */
void test_record_autostop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    unsigned saved = g_cmt.rec_autostop_s;
    g_cmt.rec_autostop_s = 2;

    /* bez hrany se neukončí ani po 3 s */
    rec_start();
    run_ms(3000);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);

    /* po hranách: 1,9 s ticha ještě ne, 2 s ano */
    rec_write_noise(200);
    run_ms(1900);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    run_ms(200);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_STRING_LEN("Recording stopped automatically after 2 s of silence", cmttmz_get_record_message(),
                                 strlen("Recording stopped automatically after 2 s of silence"));

    /* stojící motor (pauza) se do ticha nepočítá: 1 s + 10 s v pauze + 0,9 s = RECORD */
    rec_start();
    rec_write_noise(200);
    run_ms(1000);
    cmt_pause(1);
    run_ms(10000);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_pause(0);
    run_ms(900);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    run_ms(200);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(2, tape_count());

    /* 0 = nikdy */
    g_cmt.rec_autostop_s = 0;
    rec_start();
    rec_write_noise(200);
    run_ms(20000);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_NULL(strstr(cmttmz_get_record_message(), "automatically"));

    g_cmt.rec_autostop_s = saved;
}

/** @brief Uloží vloženou pásku jako .mzt a vrátí obsah souboru (g_free()). */
static gchar *fr_save_mzt(gsize *len)
{
    char *mzt = b4_path("speed.mzt");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_save(mzt), cmttmz_get_save_message());
    gchar *data = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(mzt, &data, len, NULL));
    g_remove(mzt);
    g_free(mzt);
    return data;
}

/*
 * MZT jako tmz_mzt_export(): blok 2:1 CPM a první blok s rychlostí jinou
 * než 1x se zapíší bez hlavičky CMTSPEED a uložení projde s varováním
 * "Speed of block N cannot be stored in MZT".
 */
void test_save_mzt_unstorable_speeds(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *src = save_mzt(0, 2);
    gchar *plain = NULL;
    gsize plen = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(src, &plain, &plen, NULL));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)src));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_2_1_CPM, true, NULL));
    gsize len = 0;
    gchar *out = fr_save_mzt(&len);
    TEST_ASSERT_EQUAL_STRING("Speed of block 1 cannot be stored in MZT", cmttmz_get_save_warning());
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_save_message(), "Speed of block 1 cannot be stored in MZT"));
    TEST_ASSERT_EQUAL_size_t(plen, len);
    TEST_ASSERT_EQUAL_MEMORY(plain, out, len);
    g_free(out);
    cmt_eject();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)src));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(0, CMTSPEED_2_1, true, NULL));
    out = fr_save_mzt(&len);
    TEST_ASSERT_EQUAL_STRING("Speed of block 0 cannot be stored in MZT", cmttmz_get_save_warning());
    TEST_ASSERT_EQUAL_size_t(plen, len);
    TEST_ASSERT_EQUAL_MEMORY(plain, out, len);
    g_free(out);
    g_free(plain);
}


/** @brief Čítač výsledků záznamu (cmttmz_get_record_result()). */
static unsigned rec_serial(void)
{
    unsigned s = 0;
    cmttmz_get_record_result(NULL, NULL, &s);
    return s;
}

/** @brief Počet bloků vložené pásky (-1 bez pásky). */
static int tape_blocks(void)
{
    return CMT_TEST_FILLED ? cmtext_container_get_count_blocks(g_cmt.ext->container) : -1;
}

/*
 * Otevření pásky nejdřív načte a ověří nový soubor.
 * Neexistující, poškozený, zkrácený a nepodporovaný soubor vrátí chybu
 * a vloženou pásku (Modified), její soubor v paměti i běžící nahrávání
 * nechá beze změny; discarded = false. Úspěšné otevření během nahrávání
 * záznam zahodí jednou s důvodem "another tape was opened" a ohlásí
 * discarded = true.
 */
void test_open_failure_keeps_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* páska Modified: nová páska se surovým blokem */
    rec_start();
    rec_write_noise(500);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(1, tape_blocks());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    const st_TZX_FILE *file = cmttmz_get_file();

    gchar *missing = g_build_filename(g_get_tmp_dir(), "mztest_cmt_tmz_no_such_tape.tmz", NULL);
    g_remove(missing);
    const char *bad_tmz = tmp_path(0, "tmz");
    TEST_ASSERT_TRUE(g_file_set_contents(bad_tmz, "this is not a tape file, just text", -1, NULL));
    const char *short_mzf = tmp_path(1, "mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(short_mzf, "\x01" "SHORT", 6, NULL));
    const char *bad[] = {missing, bad_tmz, short_mzf, "tape.xyz"};
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        bool discarded = true;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_FAILURE, cmt_open_file(bad[i], &discarded), bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(discarded, bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_FILLED, bad[i]);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(file, cmttmz_get_file(), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, tape_blocks(), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, cmttmz_is_modified(), bad[i]);
    }

    /* běžící nahrávání neúspěšné otevření přežije */
    rec_start();
    rec_write_noise(200);
    unsigned serial = rec_serial();
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        bool discarded = true;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_FAILURE, cmt_open_file(bad[i], &discarded), bad[i]);
        TEST_ASSERT_FALSE(discarded);
        TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_RECORD, bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(cmttmz_rec_is_active(), bad[i]);
    }
    TEST_ASSERT_EQUAL_UINT(serial, rec_serial());
    rec_write_noise(200);

    /* úspěšné otevření záznam zahodí (jednou) a ohlásí to */
    const char *good = save_mzf(0, "GOOD");
    bool discarded = false;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(good, &discarded));
    TEST_ASSERT_TRUE(discarded);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_UINT(serial + 1, rec_serial());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_record_discarded());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (another tape was opened)", cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(1, tape_blocks());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* nezměněná páska: otevření nic nezahodí */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(good, &discarded));
    TEST_ASSERT_FALSE(discarded);
    g_free(missing);
}

/*
 * Nahrávání se zahodí jednou - vysunutí po
 * cmt_record_discard() (dřívější pořadí v cmt_exit()) už druhou zprávu
 * nepřidá; samotné vysunutí zahodí jednou s důvodem "the tape was ejected".
 * Dokončený záznam (STOP) příznak zahození nemá.
 */
void test_record_discarded_once(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(100);
    unsigned serial = rec_serial();
    cmt_record_discard("emulator exit");
    cmt_eject();
    TEST_ASSERT_EQUAL_UINT(serial + 1, rec_serial());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (emulator exit)", cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_record_discarded());

    rec_start();
    rec_write_noise(100);
    serial = rec_serial();
    cmt_eject();
    TEST_ASSERT_EQUAL_UINT(serial + 1, rec_serial());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (the tape was ejected)", cmttmz_get_record_message());

    rec_start();
    rec_write_noise(100);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_record_discarded());

    rec_start();
    rec_write_noise(100);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (a new tape was inserted)", cmttmz_get_record_message());
}

/*
 * cmttmz_save_fits() - Save tape v UI podle ní volí
 * Save As. Páska z jednoho MZF se vejde do .mzf, .mzt, .tmz, .tzx; .wav
 * a .tap ne; po nahraném surovém bloku už ne do .mzf ani .mzt.
 */
void test_save_fits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("x.tmz")); /* bez pásky */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(0, "FITS")));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.mzf"));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.M12"));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.mzt"));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.tmz"));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.tzx"));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("game.wav"));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("game.tap"));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("game"));

    rec_start();
    rec_write_noise(500);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(2, tape_blocks());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("game.mzf"));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_save_fits("game.mzt"));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_save_fits("game.tmz"));
}


/* ================================================================
 * SNAPSHOT A CHECKPOINT PÁSKY: další případy
 * ================================================================ */

/** @brief Nahraje do vložené pásky (bez pásky do nové) jeden soubor ROM se jménem @p name. */
static void b6_record_rom(const char *name)
{
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, name);
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
}

/*
 * Checkpoint záznamu vstupů (snapshot bez vloženého obrazu) pásky bez
 * cesty nese <tape>: blok, pozice a rychlost bloku se při načtení
 * s vloženou páskou (keep-inserted) obnoví; páska zůstane táž.
 */
void test_ckpt_snapshot_no_path_tape_keeps_state(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    b6_record_rom("FIRST");
    b6_record_rom("SECOND");
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(0, CMTSPEED_2_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 54321));
    const st_TZX_FILE *file = cmttmz_get_file();

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_cmt_set_embed_tape(false);
    snap_save(&buf, &size);
    snap_cmt_set_embed_tape(true);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(0, 7));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(0, CMTSPEED_3_1, true, NULL));
    snap_cmt_set_keep_inserted_tape(true);
    snap_load(buf, size);
    snap_cmt_set_keep_inserted_tape(false);

    TEST_ASSERT_EQUAL_PTR(file, cmttmz_get_file());
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    assert_position(1, 54321);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1, cmtext_container_get_block_cmt_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
}

/*
 * Snapshot s vloženým obrazem a síťovou cestou (z cizího zdroje
 * i s modified = false): cesta se zahodí s varováním, páska z obrazu je
 * vždy Modified. Síť se nekontaktuje (cesta se nikde neotevírá).
 */
void test_snapshot_image_network_path_dropped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    b6_record_rom("NET");
    uint8_t *img = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&img, &len));
    const char *unc = "\\\\host\\share\\tape.tmz";
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(img, len, unc, NULL, 0));
    g_free(img);
    TEST_ASSERT_EQUAL_STRING(unc, g_cmt.last_filename);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_cmt_test_corrupt_image(3); /* XML s modified = false */
    snap_save(&buf, &size);
    snap_cmt_test_corrupt_image(0);
    cmt_eject();
    snap_cmt_test_clear_warnings();
    snap_load(buf, size);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_NOT_NULL(strstr(snap_cmt_test_warnings(), "Snapshot tape path on a network location was dropped"));

    /* místní cesta s modified = false v XML: páska z obrazu je také Modified
     * a bez cesty (cesta z nedůvěryhodného snapshotu se nepřebírá) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&img, &len));
    const char *local = tmp_path(0, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(img, len, local, NULL, 0));
    g_free(img);
    snap_cmt_test_corrupt_image(3);
    snap_save(&buf, &size);
    snap_cmt_test_corrupt_image(0);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
}

/*
 * Návrat na značku vrátí dosavadní poslední blok bajtově (připojení
 * záznamu mu přestavbou dalo pauzu 1000 ms); značka nese a vrací i cestu
 * pásky a příznak Modified.
 */
void test_tape_mark_restore_exact_bytes_and_path(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    const st_TZX_BLOCK *b1 = &cmttmz_get_file()->blocks[1];
    uint32_t len1 = b1->length;
    uint8_t id1 = b1->id;
    uint8_t *data1 = g_memdup2(b1->data, len1);
    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));

    b6_record_rom("EXACT");
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_FALSE(cmttmz_get_file()->blocks[1].length == len1
                      && 0 == memcmp(cmttmz_get_file()->blocks[1].data, data1, len1));
    /* uložení změní cestu a Modified; značka je vrátí */
    const char *saved = tmp_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(saved));
    TEST_ASSERT_EQUAL_STRING(saved, g_cmt.last_filename);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_UINT8(id1, cmttmz_get_file()->blocks[1].id);
    TEST_ASSERT_EQUAL_UINT32(len1, cmttmz_get_file()->blocks[1].length);
    TEST_ASSERT_EQUAL_MEMORY(data1, cmttmz_get_file()->blocks[1].data, len1);
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING(path, cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    g_free(data1);
}

/*
 * Prázdný obraz ZXTape! (jen hlavička) se po načtení snapshotu vrátí
 * s hlavičkou, jak byla (ne TMZ 1.1); varování importu vloženého obrazu
 * jde do varování snapshotu.
 */
void test_snapshot_empty_zxtape_image_keeps_header(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const uint8_t hdr[10] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(hdr, sizeof(hdr), "", NULL, 0));
    TEST_ASSERT_FALSE(cmttmz_get_file()->is_tmz);
    TEST_ASSERT_EQUAL_UINT8(1, cmttmz_get_file()->header.ver_major);
    TEST_ASSERT_EQUAL_UINT8(20, cmttmz_get_file()->header.ver_minor);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_FALSE(cmttmz_get_file()->is_tmz);
    TEST_ASSERT_EQUAL_UINT8(20, cmttmz_get_file()->header.ver_minor);
}

/*
 * Snapshot během nahrávání - přesný text varování; SET rychlost
 * a pozice projdou vloženým obrazem.
 */
void test_snapshot_record_warning_and_image_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(300);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    snap_cmt_test_clear_warnings();
    snap_load(buf, size);
    TEST_ASSERT_NOT_NULL(strstr(snap_cmt_test_warnings(),
                                "Snapshot was taken while recording; the recording was not restored"));
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    b6_record_rom("ONE");
    b6_record_rom("TWO");
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(0, CMTSPEED_2_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_set_position(1, 4321));
    snap_save(&buf, &size);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    assert_position(1, 4321);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1, cmtext_container_get_block_cmt_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
}


/* ================================================================
 * ZÁZNAM DO PÁSKY: DALŠÍ PŘÍPADY (jádro)
 * ================================================================ */

/**
 * @brief Zapíše krátký WAV (8 kHz mono 8 bit: 0,2 s obdélník a 0,1 s ticha) do slotu 0.
 * @return Cesta souboru.
 */
static const char *ff_make_wav(void)
{
    const uint32_t rate = 8000, burst = rate / 5, frames = burst + rate / 10;
    const uint32_t size = 44 + frames;
    uint8_t *w = (uint8_t *)calloc(1, size);
    TEST_ASSERT_NOT_NULL(w);
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
    const char *path = tmp_path(0, "wav");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)w, size, NULL));
    free(w);
    return path;
}

/*
 * Blok 0x18 z importu WAV si typ WAV (přepínač polarity) nese i přes
 * obraz pásky: snapshot pásky Modified (WAV + nahraný surový blok) se načte
 * s blokem 0 typu WAV a bez varování o neshodě; cmttmz_get_wav_blocks()
 * vrací příznaky po blocích. Obraz bez příznaků (starší snapshot nebo
 * manifest) dá blok TZX.
 */
void test_wav_origin_survives_image_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)ff_make_wav()));
    st_CMTEXT_CONTAINER *c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(c, 0));
    rec_start();
    rec_write_noise(500);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, tape_count(), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(g_cmt.ext->container, 1));

    uint32_t n = 0;
    uint8_t *wav = cmttmz_get_wav_blocks(&n);
    TEST_ASSERT_NOT_NULL(wav);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, wav[0]);
    TEST_ASSERT_EQUAL_UINT8(0, wav[1]);
    g_free(wav);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    snap_cmt_test_clear_warnings();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    c = g_cmt.ext->container;
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(c, 1));
    TEST_ASSERT_NULL_MESSAGE(strstr(snap_cmt_test_warnings(), "does not match"), snap_cmt_test_warnings());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(0));
    TEST_ASSERT_NOT_NULL(g_cmt.ext->block->cb_set_polarity);

    /* obraz bez příznaků: blok 0x18 je TZX (přepínač na něj nepůsobí) */
    uint8_t *img = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&img, &len));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(img, len, "", NULL, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_TZX, cmtext_container_get_block_type(g_cmt.ext->container, 0));
    TEST_ASSERT_NULL(cmttmz_get_wav_blocks(&n));
    TEST_ASSERT_EQUAL_UINT32(0, n);
    g_free(img);
}

/*
 * Značka checkpointu kopíruje data posledního bloku jen u bloku,
 * který připojení záznamu může přepsat (0x46 MZ800EMU-CMT s pauzou 0).
 * Poslední blok 0x18 (páska WAV) se nekopíruje, návrat na značku ho nechá
 * beze změny; poslední blok 0x46 s pauzou 0 se kopíruje a vrátí bajtově.
 */
void test_tape_mark_copies_only_rewritable_last_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* páska WAV: jeden velký blok 0x18 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)ff_make_wav()));
    const st_TZX_BLOCK *b0 = &cmttmz_get_file()->blocks[0];
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, b0->id);
    uint32_t len0 = b0->length;
    uint8_t *data0 = g_memdup2(b0->data, len0);
    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    TEST_ASSERT_EQUAL_UINT32(1, mark.block_count);
    TEST_ASSERT_NULL(mark.last_data);
    TEST_ASSERT_EQUAL_UINT32(len0, mark.last_len);
    rec_start();
    rec_write_noise(500);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    TEST_ASSERT_EQUAL_INT(1, tape_count());
    TEST_ASSERT_EQUAL_UINT32(len0, cmttmz_get_file()->blocks[0].length);
    TEST_ASSERT_EQUAL_MEMORY(data0, cmttmz_get_file()->blocks[0].data, len0);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    g_free(data0);

    /* páska MZT: poslední blok 0x46 s pauzou 0 se kopíruje a vrátí bajtově */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    const st_TZX_BLOCK *b1 = &cmttmz_get_file()->blocks[1];
    uint32_t len1 = b1->length;
    uint8_t *data1 = g_memdup2(b1->data, len1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    TEST_ASSERT_NOT_NULL(mark.last_data);
    b6_record_rom("COPY");
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_UINT32(len1, cmttmz_get_file()->blocks[1].length);
    TEST_ASSERT_EQUAL_MEMORY(data1, cmttmz_get_file()->blocks[1].data, len1);
    g_free(data1);
}

/*
 * Mez délky záznamu (pro test 0,5 s) platí i bez hran: s běžícím
 * motorem a bez zápisu programu snímek záznam ukončí jako mez bufferu.
 */
void test_record_time_limit_without_edges(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmttmz_rec_test_set_limits(0, (uint64_t)GDGCLK_BASE / 2);
    rec_start();
    run_ms(400);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    run_ms(200);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    const char *msg = cmttmz_get_record_message();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(msg, "Recording stopped: length limit reached", 39), msg);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_is_active());

    /* stojící motor (pauza) se do času nepočítá */
    rec_start();
    run_ms(300);
    cmt_pause(1);
    run_ms(2000);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_pause(0);
    run_ms(100);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    run_ms(200);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/*
 * Nahrávání s příponou pásky, které nic nepřipojí (méně než 64 hran),
 * pásku neuloží: soubor nevznikne, zpráva bez "Tape saved".
 */
void test_record_to_tape_file_nothing_appended(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *tmz = tmp_path(1, "tmz");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(tmz));
    cmt_pause(0);
    rec_write_noise(20);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(tmz, G_FILE_TEST_EXISTS), cmttmz_get_record_message());
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_record_message(), "Tape saved"), cmttmz_get_record_message());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_record_message(), "not written"), cmttmz_get_record_message());
    TEST_ASSERT_EQUAL_STRING("", cmtext_container_get_filepath(g_cmt.ext->container));
}

/*
 * Zkouška zapisovatelnosti WAV nenechá prázdný soubor, když záznam
 * nic nezapsal: zahození (vysunutí, otevření, snapshot, konec) i STOP bez
 * hran soubor ze zkoušky smaže. Soubor, který existoval předem, zůstane.
 */
void test_record_wav_probe_file_removed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *wav = tmp_path(1, "wav");
    g_pio8255.signal_pc01 = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    cmt_pause(0);
    rec_write_noise(100);
    cmt_eject();
    TEST_ASSERT_FALSE(g_file_test(wav, G_FILE_TEST_EXISTS));

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    TEST_ASSERT_TRUE(g_file_test(wav, G_FILE_TEST_EXISTS));
    cmt_pause(0);
    cmt_stop();
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(wav, G_FILE_TEST_EXISTS), cmttmz_get_record_message());
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_record_message(), "not written"));

    /* soubor uživatele (existoval předem) zahození nesmaže */
    TEST_ASSERT_TRUE(g_file_set_contents(wav, "keep", 4, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    cmt_pause(0);
    rec_write_noise(100);
    cmt_eject();
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(wav, &data, &len, NULL));
    TEST_ASSERT_EQUAL_size_t(4, len);
    g_free(data);
    g_remove(wav);
}

/*
 * Komponenta CMT je volitelná, vadný devices/cmt.xml
 * (chybí cmt_state) načtení stroje nezastaví. Nahrávání z dosavadní časové
 * osy se proto zahodí hned na začátku handleru a transport skončí ve STOP;
 * nic se nepřipojí (dřív nahrávání běželo dál nad načteným strojem
 * a skončilo "length limit reached" s hranami z doby před načtením).
 */
void test_snapshot_invalid_cmt_xml_drops_recording(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    rec_write_noise(200);
    uint8_t *bad = NULL;
    size_t bad_size = 0;
    snap_cmt_test_corrupt_image(4);
    snap_save(&bad, &bad_size);
    snap_cmt_test_corrupt_image(0);
    rec_write_noise(200); /* čas běží dál: snapshot je starší */
    unsigned serial = rec_serial();

    snap_load(bad, bad_size);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_UINT(serial + 1, rec_serial());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (a snapshot was loaded)", cmttmz_get_record_message());
    run_ms(100);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, tape_count());
    TEST_ASSERT_EQUAL_UINT(serial + 1, rec_serial());
}

/*
 * Snapshot bez komponenty CMT (devices/cmt.xml chybí): nahrávání se
 * zahodí, přehrávání se zastaví (časová základna patří dosavadní ose),
 * páska zůstane vložená.
 */
void test_snapshot_without_cmt_component_rests_deck(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t *buf = NULL;
    size_t size = 0;
    snap_cmt_test_corrupt_image(5);
    snap_save(&buf, &size);
    snap_cmt_test_corrupt_image(0);

    rec_start();
    rec_write_noise(200);
    snap_load(g_memdup2(buf, size), size);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_FALSE(cmttmz_rec_is_active());
    TEST_ASSERT_EQUAL_STRING("Recording discarded (a snapshot was loaded)", cmttmz_get_record_message());

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(0, 2)));
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
}

/*
 * Čas záznamu s start_time z jiné časové osy (pozdější než teď)
 * nepodteče: snímek záznam neukončí mezí délky.
 */
void test_record_time_with_future_start_time(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    rec_start();
    g_cmt.start_time = gdg_get_total_ticks() + (uint64_t)GDGCLK_BASE;
    run_ms(50);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
    cmt_stop();
}


/*
 * Výsledek posledního nahrávání (cmttmz_get_last_recording(),
 * MCP last_recording) přežije vložení pásky, které nahrávání zahodilo
 * (otevření jiné pásky, nová páska, snapshot), i samotné vložení další.
 */
void test_last_recording_survives_insert(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unsigned serial = 0;
    uint32_t mzf = 9, raw = 9;
    int discarded = 0;
    rec_start();
    rec_write_noise(100);
    bool dropped = false;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(save_mzf(0, "LAST"), &dropped));
    TEST_ASSERT_TRUE(dropped);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_last_recording(&serial, &mzf, &raw, &discarded));
    TEST_ASSERT_EQUAL_UINT(rec_serial(), serial);
    TEST_ASSERT_EQUAL_INT(1, discarded);
    TEST_ASSERT_EQUAL_STRING("Recording discarded (another tape was opened)", cmttmz_get_record_message());

    rec_start();
    rec_write_noise(100);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_last_recording(&serial, NULL, NULL, &discarded));
    TEST_ASSERT_EQUAL_UINT(rec_serial(), serial);
    TEST_ASSERT_EQUAL_INT(1, discarded);

    /* vložení jiné pásky bez nahrávání výsledek nesmaže */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(1, 2)));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_last_recording(&serial, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(rec_serial(), serial);
}


/*
 * Páska z vloženého obrazu si cestu ze snapshotu
 * ponechá, když je při načtení v kazetofonu páska se stejnou cestou
 * (snapshot patří této relaci: Quick Save -> Quick Load); jinak je bez
 * jména (vysunutá páska, jiná páska).
 */
void test_snapshot_image_keeps_path_of_same_inserted_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    b6_record_rom("SAME");
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    uint8_t *buf2 = g_memdup2(buf, size);

    /* stejná páska vložená: cesta zůstane, Modified */
    snap_cmt_test_clear_warnings();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_STRING(path, g_cmt.last_filename);
    TEST_ASSERT_EQUAL_STRING(path, cmtext_container_get_filepath(g_cmt.ext->container));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_NULL_MESSAGE(strstr(snap_cmt_test_warnings(), "Unsaved tape restored"), snap_cmt_test_warnings());

    /* jiná páska vložená: bez jména */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzf(1, "OTHER")));
    snap_load(buf2, size);
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_STRING("", g_cmt.last_filename);
}


/*
 * Čítač výměn pásky cmttmz_get_tape_serial(), podle
 * kterého okno Virtual CMT ruší výběr řádku a odložené akce s blokem
 * (export, rychlost bloku). Mění se při vysunutí, otevření souboru, obrazu
 * (cmt_open_image()), načtení snapshotu, který pásku otevře znovu, nové
 * pásce a zkrácení návratem na značku; NEmění se při připojení záznamu,
 * výměně otevřeného bloku (Next, seek), změně rychlosti bloku i výchozí
 * rychlosti a při uložení pásky.
 */
void test_tape_serial_changes(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unsigned s = cmttmz_get_tape_serial();
    /* otevření souboru */
    const char *path = save_mzt(0, 2);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());
    s = cmttmz_get_tape_serial();

    /* výměna bloku, rychlost bloku, výchozí rychlost: beze změny */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(0));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_change_block_speed(1, CMTSPEED_3_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_change_speed(CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s, cmttmz_get_tape_serial(), "block change and speed keep the tape serial");

    /* značka, připojení záznamu: beze změny */
    st_CMTTMZ_TAPE_MARK mark;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_mark_get(&mark));
    st_MZTAPE_MZF m;
    rec_make_mzf(&m, "SERIAL");
    rec_start();
    rec_write_rom(&m);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(3, tape_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s, cmttmz_get_tape_serial(), "recording append keeps the tape serial");

    /* uložení: beze změny */
    const char *out = tmp_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(out));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s, cmttmz_get_tape_serial(), "save keeps the tape serial");

    /* zkrácení návratem na značku: změna */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_mark_restore(&mark));
    cmttmz_tape_mark_free(&mark);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());
    s = cmttmz_get_tape_serial();

    /* obraz pásky: změna */
    uint8_t *img = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&img, &len));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(img, len, NULL, NULL, 0));
    g_free(img);
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());
    s = cmttmz_get_tape_serial();

    /* nová páska: změna */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());
    s = cmttmz_get_tape_serial();

    /* vysunutí: změna */
    cmt_eject();
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());

    /* snapshot, který pásku otevře znovu (při načtení není vložená): změna */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmt_eject();
    s = cmttmz_get_tape_serial();
    snap_load(buf, size);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_NOT_EQUAL_UINT(s, cmttmz_get_tape_serial());
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_registry_extensions);
    RUN_TEST(test_index_skips_control_block);
    RUN_TEST(test_open_block_stream);
    RUN_TEST(test_tape_without_signal);
    RUN_TEST(test_eject_during_gap_then_reopen);
    RUN_TEST(test_open_missing_file_fails);
    RUN_TEST(test_unplayable_block_is_skipped);
    RUN_TEST(test_tap_index_types_and_names);
    RUN_TEST(test_step_and_select_keep_pause_sources);
    RUN_TEST(test_select_same_block_user_paused_restarts);
    RUN_TEST(test_stop_tape_pause0);
    RUN_TEST(test_stop_tape_0x2a);
    RUN_TEST(test_stop_tape_resume_by_motor);
    RUN_TEST(test_stop_tape_without_following_block);
    RUN_TEST(test_loop_plays_block_three_times);
    RUN_TEST(test_self_jump_ends_in_stop);
    RUN_TEST(test_self_jump_error_repeats_without_rewind);
    RUN_TEST(test_zrle_block_in_the_middle_is_passed_over);
    RUN_TEST(test_zrle_first_block_opens_on_next);
    RUN_TEST(test_transient_player_error_stops_without_skip);
    RUN_TEST(test_autorewind);
    RUN_TEST(test_default_speed_halves_mzf_stream);
    RUN_TEST(test_block_speed_set_and_default_change);
    RUN_TEST(test_block_pulses_roundtrip);
    RUN_TEST(test_file_speed_block_rejected);
    RUN_TEST(test_tap_speed_2_1);
    RUN_TEST(test_tmz_emu_profile_block_speed);
    RUN_TEST(test_position_get_and_rewind_in_pause);
    RUN_TEST(test_position_set_in_stop_applies_to_play);
    RUN_TEST(test_position_clamp_and_errors);
    RUN_TEST(test_position_reset_by_speed_change);
    RUN_TEST(test_position_refused_on_skipped_block);
    RUN_TEST(test_default_speed_rebuild_failure_is_atomic);
    RUN_TEST(test_position_output_in_pause);
    RUN_TEST(test_player_state_accessors);
    RUN_TEST(test_player_state_call_validation);
    RUN_TEST(test_snapshot_tape_stop_position_and_speeds);
    RUN_TEST(test_snapshot_tape_pause_position);
    RUN_TEST(test_snapshot_tape_play_position);
    RUN_TEST(test_snapshot_checkpoint_inside_loop);
    RUN_TEST(test_snapshot_checkpoint_inside_loop_replay_mode);
    RUN_TEST(test_snapshot_tape_file_missing);
    RUN_TEST(test_snapshot_tape_file_changed);
    RUN_TEST(test_snapshot_tape_keep_inserted);
    RUN_TEST(test_snapshot_tape_default_speed_differs);
    RUN_TEST(test_snapshot_tape_finished_without_block);
    RUN_TEST(test_snapshot_tape_control_blocks_only);
    RUN_TEST(test_old_snapshot_play_over_tape_without_block);
    RUN_TEST(test_wav_long_silence_plays_to_end);
    RUN_TEST(test_snapshot_network_path_not_opened);
    RUN_TEST(test_snapshot_reopen_mzf_no_size_dialog);
    RUN_TEST(test_open_warning_reports_unplayable);
    RUN_TEST(test_nested_empty_loops_around_unplayable_block);
    RUN_TEST(test_csw_block_over_limit_is_passed_over);
    RUN_TEST(test_snapshot_load_does_not_fire_cmt_breakpoints);
    RUN_TEST(test_record_with_hack_fires_mstate_on_pc4_source_switch);
    RUN_TEST(test_snapshot_tape_ended_after_stop_block);
    RUN_TEST(test_snapshot_stop_the_tape_pause);
    RUN_TEST(test_record_into_empty_tape);
    RUN_TEST(test_record_appends_to_tape);
    RUN_TEST(test_record_raw_block_is_tzx_type);
    RUN_TEST(test_record_motor_stopped_time_excluded);
    RUN_TEST(test_record_to_wav_file);
    RUN_TEST(test_record_limit_stops);
    RUN_TEST(test_record_to_tape_file);
    RUN_TEST(test_save_tmz_roundtrip);
    RUN_TEST(test_save_raw_record_reopens_as_tzx_type);
    RUN_TEST(test_export_block_mzf_and_wav);
    RUN_TEST(test_record_into_tzx_makes_tmz);
    RUN_TEST(test_save_mzt_and_mzf_rules);
    RUN_TEST(test_save_mzt_roundtrip_with_cmtspeed);
    RUN_TEST(test_save_failure_keeps_original);
    RUN_TEST(test_record_append_mid_block_side_arrays);
    RUN_TEST(test_record_with_lock_already_held);
    RUN_TEST(test_save_atomic_failure_before_replace);
    RUN_TEST(test_record_limit_then_motor_stop);
    RUN_TEST(test_record_path_not_reused);
    RUN_TEST(test_snapshot_embeds_unsaved_tape);
    RUN_TEST(test_snapshot_embed_limit_and_invalid_image);
    RUN_TEST(test_snapshot_during_record);
    RUN_TEST(test_tape_mark_restore_truncates);
    RUN_TEST(test_record_autostop);
    RUN_TEST(test_save_mzt_unstorable_speeds);
    RUN_TEST(test_open_failure_keeps_tape);
    RUN_TEST(test_record_discarded_once);
    RUN_TEST(test_save_fits);
    RUN_TEST(test_ckpt_snapshot_no_path_tape_keeps_state);
    RUN_TEST(test_snapshot_image_network_path_dropped);
    RUN_TEST(test_tape_mark_restore_exact_bytes_and_path);
    RUN_TEST(test_snapshot_empty_zxtape_image_keeps_header);
    RUN_TEST(test_snapshot_record_warning_and_image_roundtrip);
    RUN_TEST(test_wav_origin_survives_image_roundtrip);
    RUN_TEST(test_tape_mark_copies_only_rewritable_last_block);
    RUN_TEST(test_record_time_limit_without_edges);
    RUN_TEST(test_record_to_tape_file_nothing_appended);
    RUN_TEST(test_record_wav_probe_file_removed);
    RUN_TEST(test_snapshot_invalid_cmt_xml_drops_recording);
    RUN_TEST(test_snapshot_without_cmt_component_rests_deck);
    RUN_TEST(test_record_time_with_future_start_time);
    RUN_TEST(test_last_recording_survives_insert);
    RUN_TEST(test_snapshot_image_keeps_path_of_same_inserted_tape);
    RUN_TEST(test_tape_serial_changes);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
