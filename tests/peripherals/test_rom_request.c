/**
 * @file test_rom_request.c
 * @brief Zásahy GUI do ROM přes emu vlákno (rom_request.h).
 *
 * Menu ROM a okno User Defined ROM Settings dřív volaly rom_reinstall()
 * a přepisovaly cesty v g_rom přímo z UI vlákna. Test ověřuje, že výběr
 * ROM a nastavení uživatelské ROM se projeví až v device_request_poll()
 * (emu vlákno), že se při aktivním replay neprovedou, že neplatný řádek se
 * nezařadí a že z několika nastavení před provedením platí poslední.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/memory/rom_request.h"

/** @brief ROM vybraná před testem (obnova v tearDown). */
static st_ROM_CONFIG_ROW *s_saved_row;


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


/** @brief Najde povolený předdefinovaný řádek ROM různý od @p other. */
static st_ROM_CONFIG_ROW *find_predefined_row(const st_ROM_CONFIG_ROW *other)
{
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if (r->type != ROM_CONFIG_TYPE_ROM || !r->enabled) continue;
        if (r->flags & ROM_CONFIG_FLAG_USER_DEFINED) continue;
        if (r == other) continue;
        return r;
    };
    return NULL;
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_row = g_rom.rom_cfg;
}

void tearDown(void)
{
    rom_reinstall(s_saved_row);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Výběr ROM se provede až v poll; neplatný řádek se nezařadí.
 */
void test_select_in_poll(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, rom_request_select(NULL));
    /* Oddíl (ne ROM) se nezařadí. */
    for (st_ROM_CONFIG_ROW *r = g_rom_config->roms; r->type != ROM_CONFIG_TYPE_NONE; r++)
    {
        if (r->type == ROM_CONFIG_TYPE_SECTION)
        {
            TEST_ASSERT_EQUAL_UINT32(0, rom_request_select(r));
            break;
        };
    };

    st_ROM_CONFIG_ROW *a = find_predefined_row(NULL);
    TEST_ASSERT_NOT_NULL(a);
    st_ROM_CONFIG_ROW *b = find_predefined_row(a);
    rom_reinstall(a);
    if (!b)
    {
        TEST_IGNORE_MESSAGE("only one predefined ROM in this build");
    };

    rom_request_select(b);
    TEST_ASSERT_EQUAL_PTR(a, g_rom.rom_cfg);
    device_request_poll();
    TEST_ASSERT_EQUAL_PTR(b, g_rom.rom_cfg);
    TEST_ASSERT_EQUAL_STRING("rom_select", take_one().name);
}


/**
 * @brief Nastavení uživatelské ROM se přepíše až v poll, platí poslední.
 */
void test_user_defined_settings_in_poll(void)
{
    /* Předdefinovaná ROM: poll jen přepíše nastavení, soubory nenačítá. */
    rom_reinstall(find_predefined_row(NULL));

    st_ROM_USER_DEFINED_SETTINGS s;
    memset(&s, 0, sizeof(s));
    s.allinone = ROM_BOOL_NO;
    s.mz700_fp = "first-0000.rom";
    s.cmthack_type = ROM_CMTHACK_DEFAULT;
    TEST_ASSERT_NOT_EQUAL_UINT32(0, rom_request_set_user_defined(&s));

    char buf[64];
    g_strlcpy(buf, "second-0000.rom", sizeof(buf));
    s.mz700_fp = buf;
    s.cgrom_fp = "second-cg.rom";
    s.cmthack_type = ROM_CMTHACK_DISABLED;
    rom_request_set_user_defined(&s);
    /* Kopie: změna bufferu volajícího po zařazení se neprojeví. */
    g_strlcpy(buf, "changed", sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, rom_request_set_user_defined(NULL));

    g_rom.user_defined_rom_loaded = ROM_BOOL_YES;
    device_request_poll();
    TEST_ASSERT_EQUAL_STRING("second-0000.rom", g_rom.user_defined_mz700_fp);
    TEST_ASSERT_EQUAL_STRING("second-cg.rom", g_rom.user_defined_cgrom_fp);
    TEST_ASSERT_EQUAL_STRING("", g_rom.user_defined_allinone_fp);
    TEST_ASSERT_EQUAL_INT(ROM_CMTHACK_DISABLED, g_rom.user_defined_cmthack_type);
    TEST_ASSERT_EQUAL_INT(ROM_BOOL_NO, g_rom.user_defined_rom_loaded);
    TEST_ASSERT_EQUAL_STRING("rom_set_user_defined", take_one().name);
    TEST_ASSERT_EQUAL_STRING("rom_set_user_defined", take_one().name);
}


/**
 * @brief Při aktivním replay se výběr ROM neprovede.
 */
void test_rejected_while_replay_active(void)
{
    st_ROM_CONFIG_ROW *a = find_predefined_row(NULL);
    st_ROM_CONFIG_ROW *b = find_predefined_row(a);
    rom_reinstall(a);
    if (!b)
    {
        TEST_IGNORE_MESSAGE("only one predefined ROM in this build");
    };
    g_replay_mode = REPLAY_MODE_RECORDING;
    rom_request_select(b);
    device_request_poll();

    TEST_ASSERT_EQUAL_PTR(a, g_rom.rom_cfg);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_select_in_poll);
    RUN_TEST(test_user_defined_settings_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
