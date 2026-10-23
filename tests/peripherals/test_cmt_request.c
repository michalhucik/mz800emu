/**
 * @file test_cmt_request.c
 * @brief Zásahy GUI do kazety přes emu vlákno (cmt_request.h).
 *
 * Menu CMT, okno Virtual CMT, seznam bloků a zadní DIP přepínač dřív
 * otevíraly pásku, měnily rychlost, pulzy, CMT hack a polaritu přímo z UI
 * vlákna. Test ověřuje, že zásahy cmt_request_* stav změní až
 * v device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou, že vlastní pulzy projdou argumenty zásahu přesně a že
 * neplatné argumenty se nezařadí.
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
#include "hw-generic/cmt/cmtext.h"
#include "hw-generic/cmt/cmtext_container.h"
#include "hw-generic/memory/rom.h"

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
    cmthack_load_rom_patch(s_saved_hack);
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
    int result = UNITY_END();

    g_free(s_mzt_path);
    mztest_teardown();
    return result;
}
