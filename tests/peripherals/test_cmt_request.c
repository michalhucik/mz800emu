/**
 * @file test_cmt_request.c
 * @brief Zásahy GUI do kazety přes emu vlákno (cmt_request.h).
 *
 * Menu CMT, okno Virtual CMT, seznam bloků a zadní DIP přepínač dřív
 * otevíraly pásku, měnily rychlost, pulzy, CMT hack a polaritu přímo z UI
 * vlákna. Test ověřuje, že zásahy cmt_request_* stav změní až
 * v device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou, že vlastní pulzy projdou argumenty zásahu přesně a že
 * neplatné argumenty se nezařadí. Dále ROM bez podpory CMT hacku: výběr
 * takové ROM i změna volby uživatelské ROM (soubor v dočasném adresáři) na
 * Disabled CMT Hack Patch hack vypne, uživatelská ROM se se zapnutým hackem
 * instaluje ze souboru (Default s patchem, Custom ze souboru pro CMT hack).
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
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/cmt/cmt_request.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/memory/rom_request.h"
#include "hw-generic/memory/memory.h"

/** @brief Velikost těla testovacího MZF. */
#define TEST_MZF_BODY_SIZE 256

/** @brief Cesta dvoublokové MZT pásky (dočasný soubor). */
static gchar *s_mzt_path = NULL;
/** @brief Uložená výchozí rychlost a pulzy (obnova v tearDown). */
static en_CMTSPEED s_saved_speed;
static st_MZTAPE_PULSES_LENGTH s_saved_pulses;
static unsigned s_saved_polarity;
static unsigned s_saved_fix_fname;
static unsigned s_saved_hack;
static en_CMT_CPU_BOOST s_saved_boost;
/** @brief ROM vybraná před testem; tearDown ji vrátí, i když test selže uprostřed (longjmp Unity). */
static st_ROM_CONFIG_ROW *s_saved_rom;
/**
 * @brief Nastavení uživatelské ROM před testem s uživatelskou ROM (kopie řetězců g_strdup()).
 *
 * Platí jen při s_user_rom_saved; tearDown ho vrátí do g_rom a kopie uvolní.
 */
static st_ROM_USER_DEFINED_SETTINGS s_saved_user_rom;
/** @brief true = s_saved_user_rom nese nastavení k návratu (test změnil uživatelskou ROM). */
static bool s_user_rom_saved = false;
/** @brief Dočasné soubory uživatelské ROM (hlavní, pro CMT hack); maže tearDown. */
static gchar *s_user_rom_paths[2] = {NULL, NULL};


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


/**
 * @brief Zapíše MZT pásku ze dvou stejných MZF bloků.
 * @param path Cílová cesta.
 * @return true při úspěchu.
 */
static bool write_test_mzt(const char *path)
{
    uint8_t hdr[128];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x01;
    const char name[] = "CMT REQUEST\r";
    memcpy(&hdr[1], name, sizeof(name) - 1);
    hdr[0x12] = (uint8_t)(TEST_MZF_BODY_SIZE & 0xFF);
    hdr[0x13] = (uint8_t)(TEST_MZF_BODY_SIZE >> 8);
    hdr[0x15] = 0x12; /* fstrt = 1200h */
    hdr[0x17] = 0x12; /* fexec = 1200h */

    FILE *fh = fopen(path, "wb");
    if (!fh) return false;
    bool ok = true;
    for (int b = 0; ok && b < 2; b++)
    {
        ok = (fwrite(hdr, 1, sizeof(hdr), fh) == sizeof(hdr));
        for (int i = 0; ok && (i < TEST_MZF_BODY_SIZE); i++)
        {
            ok = (fputc((i * 37 + 11) & 0xFF, fh) != EOF);
        };
    };
    if (fclose(fh) != 0) ok = false;
    return ok;
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_speed = g_cmt.mz_cmtspeed;
    s_saved_pulses = g_cmt.mz_custom_pulses;
    s_saved_polarity = g_cmt.polarity;
    s_saved_fix_fname = g_cmthack.fix_fname_terminator;
    s_saved_hack = g_cmthack.load_patch_installed;
    s_saved_boost = g_cmt.cpu_boost;
    s_saved_rom = g_rom.rom_cfg;
    if (!s_mzt_path)
    {
        gchar *name = g_strdup_printf("mztest_cmt_request_%u.mzt", (unsigned)g_random_int());
        s_mzt_path = g_build_filename(g_get_tmp_dir(), name, NULL);
        g_free(name);
    };
    TEST_ASSERT_TRUE_MESSAGE(write_test_mzt(s_mzt_path), "cannot write test MZT");
}

void tearDown(void)
{
    cmt_eject();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.mz_custom_pulses = s_saved_pulses;
    cmt_rear_dip_switch_cmt_inverted_polarity(s_saved_polarity);
    g_cmthack.fix_fname_terminator = s_saved_fix_fname;
    /* ROM nejdřív: návrat kompatibilní ROM, pak hack podle stavu před testem */
    if (g_rom.rom_cfg != s_saved_rom)
        rom_reinstall(s_saved_rom);
    if (s_user_rom_saved)
    {
        /* nastavení uživatelské ROM zpět (vybraná už je zase ROM před testem) */
        const st_ROM_USER_DEFINED_SETTINGS *s = &s_saved_user_rom;
        g_rom.user_defined_allinone = s->allinone;
        rom_set_user_defined_filepath(&g_rom.user_defined_allinone_fp, (char *)s->allinone_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_mz700_fp, (char *)s->mz700_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_cgrom_fp, (char *)s->cgrom_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_mz800_fp, (char *)s->mz800_fp);
        g_rom.user_defined_cmthack_type = s->cmthack_type;
        g_rom.user_defined_cmthack_allinone = s->cmthack_allinone;
        rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_allinone_fp, (char *)s->cmthack_allinone_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_mz700_fp, (char *)s->cmthack_mz700_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_cgrom_fp, (char *)s->cmthack_cgrom_fp);
        rom_set_user_defined_filepath(&g_rom.user_defined_cmthack_mz800_fp, (char *)s->cmthack_mz800_fp);
        g_rom.user_defined_rom_loaded = ROM_BOOL_NO;
        g_free((char *)s->allinone_fp);
        g_free((char *)s->mz700_fp);
        g_free((char *)s->cgrom_fp);
        g_free((char *)s->mz800_fp);
        g_free((char *)s->cmthack_allinone_fp);
        g_free((char *)s->cmthack_mz700_fp);
        g_free((char *)s->cmthack_cgrom_fp);
        g_free((char *)s->cmthack_mz800_fp);
        memset(&s_saved_user_rom, 0, sizeof(s_saved_user_rom));
        s_user_rom_saved = false;
    };
    for (int i = 0; i < 2; i++)
    {
        if (s_user_rom_paths[i])
        {
            g_remove(s_user_rom_paths[i]);
            g_free(s_user_rom_paths[i]);
            s_user_rom_paths[i] = NULL;
        };
    };
    cmthack_load_rom_patch(s_saved_hack);
    cmt_cpu_boost_set(s_saved_boost);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    if (s_mzt_path) g_remove(s_mzt_path);
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_open(NULL, false));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_open("", true));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_set_speed(CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_set_speed(CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_set_custom_pulses(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_set_block_speed(-1, CMTSPEED_3_1, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, cmt_request_set_block_speed(0, CMTSPEED_CUSTOM, true, NULL));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Volby kazety se změní až v poll; vlastní pulzy přesně.
 */
void test_options_in_poll(void)
{
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
    cmt_rear_dip_switch_cmt_inverted_polarity(0);
    g_cmthack.fix_fname_terminator = 0;

    cmt_request_set_speed(CMTSPEED_3_1);
    cmt_request_set_polarity(true);
    cmt_request_set_fix_fname_terminator(true);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, g_cmt.mz_cmtspeed);
    TEST_ASSERT_EQUAL_UINT(0, g_cmt.polarity);
    TEST_ASSERT_EQUAL_UINT(0, g_cmthack.fix_fname_terminator);

    device_request_poll();
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, g_cmt.mz_cmtspeed);
    TEST_ASSERT_EQUAL_UINT(1, g_cmt.polarity);
    TEST_ASSERT_EQUAL_UINT(1, g_cmthack.fix_fname_terminator);
    TEST_ASSERT_EQUAL_STRING("cmt_set_speed", take_one().name);
    TEST_ASSERT_EQUAL_STRING("cmt_set_polarity", take_one().name);
    TEST_ASSERT_EQUAL_STRING("cmt_set_fix_fname_terminator", take_one().name);

    /* Hodnoty, které nejdou zapsat přesně desetinně. */
    st_MZTAPE_PULSES_LENGTH p;
    p.long_pulse.high = 1.0 / 3000.0;
    p.long_pulse.low = 2.0 / 7000.0;
    p.long_pulse.total = p.long_pulse.high + p.long_pulse.low;
    p.short_pulse.high = 1.0 / 9000.0;
    p.short_pulse.low = 1.0 / 11000.0;
    p.short_pulse.total = p.short_pulse.high + p.short_pulse.low;
    cmt_request_set_custom_pulses(&p);
    device_request_poll();
    st_DEVICE_REQ_RESULT r = take_one();
    TEST_ASSERT_EQUAL_STRING("cmt_set_custom_pulses", r.name);
    TEST_ASSERT_EQUAL_INT(0, r.rc);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, g_cmt.mz_cmtspeed);
    TEST_ASSERT_EQUAL_MEMORY(&p, &g_cmt.mz_custom_pulses, sizeof(p));
}


/**
 * @brief CMT hack se přepne až v poll (je-li s vybranou ROM kompatibilní).
 */
void test_hack_in_poll(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
    {
        TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    };
    cmthack_load_rom_patch(0);
    cmt_request_set_hack(true);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    device_request_poll();
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_STRING("cmt_set_hack", take_one().name);
}


/**
 * @brief Otevření pásky a rychlost bloku se provedou až v poll.
 */
void test_open_and_block_speed_in_poll(void)
{
    cmt_eject();
    cmt_request_open("this-file-does-not-exist.mzt", false);
    device_request_poll();
    st_DEVICE_REQ_RESULT r = take_one();
    TEST_ASSERT_EQUAL_STRING("cmt_open", r.name);
    TEST_ASSERT_EQUAL_INT(-1, r.rc);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    /* Bez pásky rychlost bloku selže. */
    cmt_request_set_block_speed(1, CMTSPEED_3_1, true, NULL);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);

    cmt_request_open(s_mzt_path, false);
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    device_request_poll();
    r = take_one();
    TEST_ASSERT_EQUAL_INT(0, r.rc);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_NOT_NULL(container);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(container));

    cmt_request_set_block_speed(1, CMTSPEED_3_1, true, NULL);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 1));
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(container, 1));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, cmtext_container_get_block_cmt_speed(container, 1));

    /* Zpět na výchozí rychlost. */
    cmt_request_set_block_speed(1, CMTSPEED_NONE, true, NULL);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 1));

    /* Mimo rozsah bloků. */
    cmt_request_set_block_speed(2, CMTSPEED_3_1, true, NULL);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
}


/**
 * @brief Sdílená pravidla režimu načítání (cmt_loading_mode_apply(), okno i MCP emu_cmt_hack_set).
 *
 * Hack se mění jen ve STOP (bez pásky i s páskou), mimo STOP vrátí -1
 * a nic nezmění ani CPU Boost; požadavek beze změny hacku projde v každém
 * stavu; vypnutí hacku smaže stav hacku nad páskou, zapnutí ho nechá.
 */
void test_loading_mode_apply_shared(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
    {
        TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    };
    cmt_eject();
    cmthack_load_rom_patch(0);
    cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED);

    /* bez pásky (STOP) */
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, cmt_loading_mode_apply(-1, 0, -1));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);

    /* s páskou: stav hacku nad páskou (konec pásky) zůstane při zapnutí, vypnutí ho smaže */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.end = true;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_TRUE_MESSAGE(cmthack_tape_state_get(&st), "a request without a change keeps the tape state of the hack");
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, cmt_loading_mode_apply(-1, 0, -1));
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&st), "switching the hack off forgets its tape state");

    /* mimo STOP: změna hacku odmítnutá celá (ani CPU Boost), beze změny hacku projde */
    cmt_play();
    TEST_ASSERT_FALSE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(-1, cmt_loading_mode_apply(-1, 1, 1));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_FALSE(CMT_TEST_CPU_BOOST_ENABLED);
    TEST_ASSERT_EQUAL_INT(-1, cmt_loading_mode_apply(CMT_LOADING_MODE_DIRECT, -1, -1));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_BOOST, cmt_loading_mode_apply(-1, 0, 1));
    TEST_ASSERT_TRUE(CMT_TEST_CPU_BOOST_ENABLED);
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_TAPE, cmt_loading_mode_apply(CMT_LOADING_MODE_TAPE, -1, -1));
    TEST_ASSERT_FALSE(CMT_TEST_CPU_BOOST_ENABLED);
    cmt_stop();
}


/**
 * @brief Sdílená pravidla Load directly (cmt_load_directly_apply(), okno i MCP emu_cmt_load_directly).
 *
 * Důvody odmítnutí v pořadí kontrol, úspěch (blok, Direct, CPU Boost
 * vypnutý), opakování v Direct (0, CPU Boost zůstane) a zásah okna
 * s návratem -1 při odmítnutí (kontrakt cmt_request_load_directly_on_tape()).
 */
void test_load_directly_apply_shared(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
    {
        TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    };
    cmt_eject();
    cmthack_load_rom_patch(0);
    en_CMT_LOAD_DIRECTLY_REFUSAL why = CMT_LOAD_DIRECTLY_OK;
    TEST_ASSERT_EQUAL_INT(-1, cmt_load_directly_apply(0, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_NO_TAPE, why);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    cmt_play();
    TEST_ASSERT_EQUAL_INT(-1, cmt_load_directly_apply(1, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_NOT_STOPPED, why);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(-1, cmt_load_directly_apply(2, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_NO_BLOCK, why);
    TEST_ASSERT_EQUAL_INT(-1, cmt_load_directly_apply(-1, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_NO_BLOCK, why);
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    TEST_ASSERT_EQUAL_INT(1, cmt_load_directly_apply(1, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_OK, why);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_open_block_id());
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_FALSE(CMT_TEST_CPU_BOOST_ENABLED);
    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    TEST_ASSERT_EQUAL_INT(0, cmt_load_directly_apply(0, NULL));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    TEST_ASSERT_TRUE(CMT_TEST_CPU_BOOST_ENABLED);

    /* zásah okna: odmítnutí dál vrací -1, úspěch 1 */
    cmthack_load_rom_patch(0);
    cmt_request_load_directly_on_tape(cmttmz_get_tape_serial(), 5);
    device_request_poll();
    st_DEVICE_REQ_RESULT r = take_one();
    TEST_ASSERT_EQUAL_STRING(CMT_REQUEST_LOAD_DIRECTLY_NAME, r.name);
    TEST_ASSERT_EQUAL_INT(-1, r.rc);
    cmt_request_load_directly_on_tape(cmttmz_get_tape_serial(), 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(1, take_one().rc);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
}


/**
 * @brief ROM nekompatibilní s CMT hackem hack vypne a zapomene i jeho stav nad páskou (rom.c).
 *
 * Bez úklidu by po návratu kompatibilní ROM a zapnutí hacku příští LOAD
 * navázal na starou hlavičku nebo konec pásky. Bez takové ROM v buildu se
 * test přeskočí; ROM vrátí tearDown (s_saved_rom), i když test selže uprostřed.
 */
void test_incompatible_rom_clears_hack_tape_state(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
    {
        TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    };
    st_ROM_CONFIG_ROW *incomp = NULL;
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if ((r->type == ROM_CONFIG_TYPE_ROM) && r->enabled && (r->flags & ROM_CONFIG_FLAG_CMTHACK_INCOMP)
            && !(r->flags & ROM_CONFIG_FLAG_USER_DEFINED))
        {
            incomp = r;
            break;
        };
    };
    if (!incomp)
    {
        TEST_IGNORE_MESSAGE("no predefined ROM incompatible with the CMT hack in this build");
    };
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.end = true;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));
    TEST_ASSERT_TRUE(cmthack_tape_state_get(&st));

    rom_reinstall(incomp);
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "the incompatible ROM switched the hack off");
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&st), "the incompatible ROM forgot the tape state of the hack");
}


/**
 * @brief Load directly s ROM bez podpory CMT hacku nic nezmění (CMT_LOAD_DIRECTLY_NO_HACK).
 *
 * Blok zůstane, CPU Boost zůstane zapnutý, hack vypnutý; zásah okna vrátí -1.
 * Bez takové ROM v buildu se test přeskočí; ROM vrátí tearDown.
 */
void test_load_directly_without_hack_rom(void)
{
    if (TEST_ROM_CMTHACK_INCOMP)
    {
        TEST_IGNORE_MESSAGE("current ROM is not compatible with the CMT hack");
    };
    st_ROM_CONFIG_ROW *incomp = NULL;
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if ((r->type == ROM_CONFIG_TYPE_ROM) && r->enabled && (r->flags & ROM_CONFIG_FLAG_CMTHACK_INCOMP)
            && !(r->flags & ROM_CONFIG_FLAG_USER_DEFINED))
        {
            incomp = r;
            break;
        };
    };
    if (!incomp)
    {
        TEST_IGNORE_MESSAGE("no predefined ROM incompatible with the CMT hack in this build");
    };
    cmt_eject();
    cmthack_load_rom_patch(0);
    rom_reinstall(incomp);
    TEST_ASSERT_FALSE(cmthack_rom_supported());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);

    en_CMT_LOAD_DIRECTLY_REFUSAL why = CMT_LOAD_DIRECTLY_OK;
    TEST_ASSERT_EQUAL_INT(-1, cmt_load_directly_apply(1, &why));
    TEST_ASSERT_EQUAL_INT(CMT_LOAD_DIRECTLY_NO_HACK, why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmttmz_get_open_block_id(), "the block is not selected");
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_CPU_BOOST_ENABLED, "the CPU boost stays on");
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);

    cmt_request_load_directly_on_tape(cmttmz_get_tape_serial(), 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
}


/** @brief Bajt obsahu testovací uživatelské ROM (vzor podle offsetu a semínka). */
static uint8_t user_rom_byte(uint32_t i, uint8_t seed)
{
    return (uint8_t)((i * 7u + 3u + seed) & 0xFFu);
}


/**
 * @brief Zapíše uživatelskou ROM jedním souborem (ROM_SIZE_TOTAL B, obsah user_rom_byte()).
 * @param slot Slot s_user_rom_paths (0 hlavní, 1 pro CMT hack; soubor maže tearDown).
 * @param seed Semínko vzoru (rozliší hlavní ROM a ROM pro CMT hack).
 * @return Cesta souboru.
 */
static const char *write_user_rom(int slot, uint8_t seed)
{
    gchar *name = g_strdup_printf("mztest_cmt_request_rom%d_%u.bin", slot, (unsigned)g_random_int());
    g_free(s_user_rom_paths[slot]);
    s_user_rom_paths[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    FILE *fh = fopen(s_user_rom_paths[slot], "wb");
    TEST_ASSERT_NOT_NULL(fh);
    bool ok = true;
    for (uint32_t i = 0; ok && (i < ROM_SIZE_TOTAL); i++)
    {
        ok = (fputc(user_rom_byte(i, seed), fh) != EOF);
    };
    /* zavřít i po chybě zápisu, jinak by tearDown soubor na Windows nesmazal */
    if (fclose(fh) != 0) ok = false;
    TEST_ASSERT_TRUE_MESSAGE(ok, "cannot write the test user ROM");
    return s_user_rom_paths[slot];
}


/**
 * @brief Ověří, že v paměti ROM je standardní patch CMT hacku (první bajty obou rutin).
 *
 * mz700_cmthack.c, mz800_cmthack.c i mz1500_cmthack.c zapisují na 04D8h
 * i 04F8h E5h (push HL); vzor testovací ROM tam E5h nemá (EBh, CBh).
 *
 * @param msg Zpráva selhání.
 */
static void assert_patch_in_rom(const char *msg)
{
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xE5, g_memory.ROM[0x04D8], msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xE5, g_memory.ROM[0x04F8], msg);
}


/**
 * @brief Je v paměti ROM obsah testovací uživatelské ROM?
 * @param seed Semínko vzoru. @param skip_patch true = vynechat oblast patche CMT hacku (04D0h-050Fh).
 * @return Offset prvního rozdílu, -1 = shoda.
 */
static int user_rom_diff(uint8_t seed, bool skip_patch)
{
    for (uint32_t i = 0; i < ROM_SIZE_TOTAL; i++)
    {
        if (skip_patch && (i >= 0x04D0) && (i < 0x0510)) continue;
        if (g_memory.ROM[i] != user_rom_byte(i, seed)) return (int)i;
    };
    return -1;
}


/**
 * @brief Zařadí a provede nastavení uživatelské ROM jedním souborem (jako okno User Defined ROM Settings).
 * @param fp Soubor hlavní ROM. @param type Varianta patche CMT hacku. @param hack_fp Soubor ROM pro CMT hack (CUSTOM), jinak NULL.
 */
static void apply_user_rom(const char *fp, en_ROM_CMTHACK type, const char *hack_fp)
{
    st_ROM_USER_DEFINED_SETTINGS s;
    memset(&s, 0, sizeof(s));
    s.allinone = ROM_BOOL_YES;
    s.allinone_fp = fp;
    s.mz700_fp = "";
    s.cgrom_fp = "";
    s.mz800_fp = "";
    s.cmthack_type = type;
    s.cmthack_allinone = ROM_BOOL_YES;
    s.cmthack_allinone_fp = hack_fp ? hack_fp : "";
    s.cmthack_mz700_fp = "";
    s.cmthack_cgrom_fp = "";
    s.cmthack_mz800_fp = "";
    TEST_ASSERT_TRUE(rom_request_set_user_defined(&s) > 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, take_one().status);
}


/**
 * @brief Uloží nastavení uživatelské ROM pro tearDown a vybere uživatelskou ROM.
 * @return Řádek uživatelské ROM; NULL = build ji nemá (test se pak přeskočí).
 */
static st_ROM_CONFIG_ROW *user_rom_begin(void)
{
    st_ROM_CONFIG_ROW *user = NULL;
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if ((r->type == ROM_CONFIG_TYPE_ROM) && r->enabled && (r->flags & ROM_CONFIG_FLAG_USER_DEFINED))
        {
            user = r;
            break;
        };
    };
    if (!user) return NULL;
    st_ROM_USER_DEFINED_SETTINGS *s = &s_saved_user_rom;
    s->allinone = g_rom.user_defined_allinone;
    s->allinone_fp = g_strdup(g_rom.user_defined_allinone_fp);
    s->mz700_fp = g_strdup(g_rom.user_defined_mz700_fp);
    s->cgrom_fp = g_strdup(g_rom.user_defined_cgrom_fp);
    s->mz800_fp = g_strdup(g_rom.user_defined_mz800_fp);
    s->cmthack_type = g_rom.user_defined_cmthack_type;
    s->cmthack_allinone = g_rom.user_defined_cmthack_allinone;
    s->cmthack_allinone_fp = g_strdup(g_rom.user_defined_cmthack_allinone_fp);
    s->cmthack_mz700_fp = g_strdup(g_rom.user_defined_cmthack_mz700_fp);
    s->cmthack_cgrom_fp = g_strdup(g_rom.user_defined_cmthack_cgrom_fp);
    s->cmthack_mz800_fp = g_strdup(g_rom.user_defined_cmthack_mz800_fp);
    s_user_rom_saved = true;
    return user;
}


/**
 * @brief Uživatelská ROM s patchem Default: zapnutý hack přežije přeinstalování, volba Disabled ho vypne.
 *
 * Dřív: (1) přeinstalování uživatelské ROM se zapnutým hackem (Default)
 * nainstalovalo neinicializovanou ROM pro CMT hack místo souboru; (2) po
 * změně volby na Disabled zůstal hack "zapnutý" (cmthack_load_rom_patch(0)
 * s typem Disabled nic neudělá), okno ani menu ho pak nevypnuly a v paměti
 * byla zase neinicializovaná ROM. Nyní: obsah ROM = soubor (mimo patch),
 * po Disabled hack vypnutý, stav hacku nad páskou prázdný a celá ROM = soubor.
 */
void test_user_rom_default_patch_then_disabled(void)
{
    st_ROM_CONFIG_ROW *user = user_rom_begin();
    if (!user)
    {
        TEST_IGNORE_MESSAGE("no user defined ROM row in this build");
    };
    const char *fp = write_user_rom(0, 0);
    cmt_eject();
    cmthack_load_rom_patch(0);
    apply_user_rom(fp, ROM_CMTHACK_DEFAULT, NULL);
    TEST_ASSERT_TRUE(rom_request_select(user) > 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, take_one().status);
    TEST_ASSERT_TRUE_MESSAGE(g_rom.rom_cfg == user, "the user defined ROM is selected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, false), "the user defined ROM is in memory");
    TEST_ASSERT_TRUE(cmthack_rom_supported());

    /* hack zapnutý nad páskou se stavem (konec pásky) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, true), "the patch changes only its own bytes");
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.end = true;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));

    /* totéž nastavení znovu: ROM se přeinstaluje, hack i jeho stav zůstanou */
    apply_user_rom(fp, ROM_CMTHACK_DEFAULT, NULL);
    TEST_ASSERT_TRUE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "the hack survives a reinstall of a supporting ROM");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, true), "a reinstall with the hack on installs the ROM file");
    assert_patch_in_rom("a reinstall with the hack on applies the patch");
    TEST_ASSERT_TRUE_MESSAGE(cmthack_tape_state_get(&st), "the hack keeps its tape state");

    /* výběr jiné ROM a zpět v menu ROM se zapnutým hackem: zase soubor ROM s patchem */
    TEST_ASSERT_TRUE(rom_request_select(s_saved_rom) > 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, take_one().status);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_TRUE(rom_request_select(user) > 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, take_one().status);
    TEST_ASSERT_TRUE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "the hack survives the ROM change");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, true), "selecting the user ROM with the hack on installs the ROM file");
    assert_patch_in_rom("selecting the user ROM with the hack on applies the patch");

    /* volba Disabled: hack vypnutý, stav nad páskou zapomenutý, ROM = soubor */
    apply_user_rom(fp, ROM_CMTHACK_DISABLED, NULL);
    TEST_ASSERT_FALSE(cmthack_rom_supported());
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "Disabled CMT Hack Patch switches the hack off");
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&st), "the hack forgets its tape state");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, false), "the unpatched ROM file is in memory");
    /* zapnutí s takovou ROM nic nezmění (okno a menu ho nenabízejí) */
    TEST_ASSERT_NOT_EQUAL(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
}


/**
 * @brief Uživatelská ROM s vlastní ROM pro CMT hack (Custom): hack nainstaluje ROM ze souboru, volba Disabled ho vypne.
 *
 * Kroky: Custom + hack -> soubor pro hack; hack vypnutý -> hlavní soubor;
 * hack znovu zapnutý, změna volby na Default se zapnutým hackem -> hlavní
 * soubor s patchem (dřív nenačtená data); zpět Custom -> soubor pro hack;
 * Disabled -> hack vypnutý, stav hacku nad páskou zapomenutý, hlavní soubor.
 */
void test_user_rom_custom_patch_then_disabled(void)
{
    st_ROM_CONFIG_ROW *user = user_rom_begin();
    if (!user)
    {
        TEST_IGNORE_MESSAGE("no user defined ROM row in this build");
    };
    const char *fp = write_user_rom(0, 0);
    const char *hack_fp = write_user_rom(1, 0x55);
    cmt_eject();
    cmthack_load_rom_patch(0);
    apply_user_rom(fp, ROM_CMTHACK_CUSTOM, hack_fp);
    TEST_ASSERT_TRUE(rom_request_select(user) > 0);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_DONE, take_one().status);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, false), "the user defined ROM is in memory");

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0x55, false), "the custom CMT hack ROM is in memory");

    /* vypnutí hacku: hlavní soubor */
    TEST_ASSERT_NOT_EQUAL(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 0, -1));
    TEST_ASSERT_FALSE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, false), "switching the hack off installs the main ROM file");

    /* Custom -> Default se zapnutým hackem: hlavní soubor s patchem */
    TEST_ASSERT_EQUAL_INT(CMT_LOADING_MODE_DIRECT, cmt_loading_mode_apply(-1, 1, -1));
    TEST_ASSERT_EQUAL_INT(-1, user_rom_diff(0x55, false));
    apply_user_rom(fp, ROM_CMTHACK_DEFAULT, NULL);
    TEST_ASSERT_TRUE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "Custom -> Default keeps the hack on");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, true), "Custom -> Default with the hack on installs the main ROM file");
    assert_patch_in_rom("Custom -> Default with the hack on applies the patch");

    /* zpět Custom se zapnutým hackem: soubor pro hack */
    apply_user_rom(fp, ROM_CMTHACK_CUSTOM, hack_fp);
    TEST_ASSERT_TRUE(CMTHACK_TEST_IS_INSTALLED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0x55, false), "Default -> Custom with the hack on installs the CMT hack ROM file");

    /* Disabled: hack vypnutý, stav nad páskou zapomenutý */
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.end = true;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));
    TEST_ASSERT_TRUE(cmthack_tape_state_get(&st));
    apply_user_rom(fp, ROM_CMTHACK_DISABLED, NULL);
    TEST_ASSERT_FALSE_MESSAGE(CMTHACK_TEST_IS_INSTALLED, "Disabled CMT Hack Patch switches the hack off");
    TEST_ASSERT_FALSE_MESSAGE(cmthack_tape_state_get(&st), "the hack forgets its tape state");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, user_rom_diff(0, false), "the main ROM file is in memory");
}


/**
 * @brief Stav bloku a první přehratelný blok (cmt_tape_block_status(), cmt_tape_first_playable_block()).
 */
void test_block_status_and_first_playable(void)
{
    cmt_eject();
    en_CMT_BLOCK_STATUS st = CMT_BLOCK_STATUS_CONTROL;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_block_status(0, &st));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_first_playable_block());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file(s_mzt_path, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_block_status(1, &st));
    TEST_ASSERT_EQUAL_INT(CMT_BLOCK_STATUS_OK, st);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_block_status(2, &st));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_block_status(-1, &st));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_first_playable_block());
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    cmt_eject();
    g_replay_mode = REPLAY_MODE_RECORDING;
    cmt_request_open(s_mzt_path, true);
    device_request_poll();

    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_options_in_poll);
    RUN_TEST(test_hack_in_poll);
    RUN_TEST(test_open_and_block_speed_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    RUN_TEST(test_loading_mode_apply_shared);
    RUN_TEST(test_load_directly_apply_shared);
    RUN_TEST(test_block_status_and_first_playable);
    RUN_TEST(test_incompatible_rom_clears_hack_tape_state);
    RUN_TEST(test_load_directly_without_hack_rom);
    RUN_TEST(test_user_rom_default_patch_then_disabled);
    RUN_TEST(test_user_rom_custom_patch_then_disabled);
    int result = UNITY_END();

    g_free(s_mzt_path);
    mztest_teardown();
    return result;
}
