/**
 * @file test_memext_request.c
 * @brief Zásahy GUI do MemExt přes emu vlákno (memext_request.h).
 *
 * Menu MemExt a dialog "Load MemExt RAM From File" dřív přepojovaly
 * paměť a četly soubor přímo do `g_memext.RAM` z UI vlákna. Test
 * ověřuje, že zásahy memext_request_* stav změní až v
 * device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou a že neplatné argumenty se nezařadí:
 *  - připojení a odpojení MemExt,
 *  - načtení úseku souboru do RAM (offset v souboru i v paměti),
 *    chybějící soubor a kratší čtení vrátí chybu.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/memory/memext_request.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;
/** @brief Testovací soubor (16 bajtů 00h-0Fh). */
static char *s_file = NULL;
/** @brief Uložené připojení MemExt (obnova v tearDown). */
static int s_saved_connected;
/** @brief Uložený typ MemExt. */
static en_MEMEXT_TYPE s_saved_type;


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


void setUp(void)
{
    if (!g_file_test(s_file, G_FILE_TEST_EXISTS))
    {
        char data[16];
        for (int i = 0; i < 16; i++) data[i] = (char)i;
        TEST_ASSERT_TRUE(g_file_set_contents(s_file, data, sizeof(data), NULL));
    }
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_connected = MEMEXT_TEST_CONNECTED;
    s_saved_type = g_memext.type;
}

void tearDown(void)
{
    if (s_saved_connected) memext_connect(s_saved_type);
    else memext_disconnect();
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Neplatné argumenty se nezařadí.
 */
void test_invalid_args_not_queued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, memext_request_set_connection(42));
    TEST_ASSERT_EQUAL_UINT32(0, memext_request_set_flash_filepath(""));
    TEST_ASSERT_EQUAL_UINT32(0, memext_request_load(false, "", 0, 0, 1));
    TEST_ASSERT_EQUAL_UINT32(0, memext_request_load(false, s_file, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, memext_request_load(false, s_file, 0, MEMEXT_RAM_SIZE - 4, 8));
    TEST_ASSERT_EQUAL_INT(0, device_request_pending());
}


/**
 * @brief Připojení a odpojení až v poll.
 */
void test_connection_in_poll(void)
{
    memext_disconnect();
    memext_request_set_connection(MEMEXT_TYPE_PEHU);
    TEST_ASSERT_FALSE(MEMEXT_TEST_CONNECTED);
    device_request_poll();
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED_PEHU);
    take_one();

    memext_request_set_connection(MEMEXT_REQUEST_DISCONNECT);
    TEST_ASSERT_TRUE(MEMEXT_TEST_CONNECTED);
    device_request_poll();
    TEST_ASSERT_FALSE(MEMEXT_TEST_CONNECTED);
    take_one();
}


/**
 * @brief Načtení úseku souboru do RAM až v poll; chyby vrátí -1.
 */
void test_load_in_poll(void)
{
    memset(&g_memext.RAM[0x100], 0xEE, 16);
    memext_request_load(false, s_file, 4, 0x100, 8);
    TEST_ASSERT_EQUAL_HEX8(0xEE, g_memext.RAM[0x100]);

    device_request_poll();
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_HEX8(4 + i, g_memext.RAM[0x100 + i]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, g_memext.RAM[0x108]);
    TEST_ASSERT_EQUAL_INT(0, take_one().rc);

    /* Kratší soubor než požadovaná velikost. */
    memext_request_load(false, s_file, 8, 0x200, 16);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);

    char *missing = g_build_filename(s_tmpdir, "missing.bin", NULL);
    memext_request_load(false, missing, 0, 0, 1);
    device_request_poll();
    TEST_ASSERT_EQUAL_INT(-1, take_one().rc);
    g_free(missing);
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    memset(&g_memext.RAM[0x300], 0xEE, 4);
    g_replay_mode = REPLAY_MODE_RECORDING;
    memext_request_load(false, s_file, 0, 0x300, 4);
    device_request_poll();

    TEST_ASSERT_EQUAL_HEX8(0xEE, g_memext.RAM[0x300]);
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    s_tmpdir = g_dir_make_tmp("memext_request_XXXXXX", NULL);
    s_file = g_build_filename(s_tmpdir, "data.bin", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_invalid_args_not_queued);
    RUN_TEST(test_connection_in_poll);
    RUN_TEST(test_load_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    g_remove(s_file);
    g_free(s_file);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
