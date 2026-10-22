/**
 * @file test_printer_request.c
 * @brief Zásahy GUI do tiskárny přes emu vlákno (printer_request.h).
 *
 * Menu Printer a zadní DIP přepínač dřív měnily standard tiskárny,
 * zachytávání a zavíraly soubor zachytávání přímo z UI vlákna. Test
 * ověřuje, že zásahy printer_request_* stav změní až v
 * device_request_poll() (emu vlákno), že se při aktivním replay
 * neprovedou a že neplatný standard se nezařadí.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>

#include "device_request.h"
#include "replay.h"
#include "hw-generic/printer/printer.h"
#include "hw-generic/printer/printer_request.h"
#include "hw-generic/pioz80/pioz80.h"

/** @brief Uložený standard tiskárny (obnova v tearDown). */
static en_PIOZ80_PRINTER_STD s_saved_std;
/** @brief Uložený stav zachytávání. */
static bool s_saved_active;


/** @brief Vyzvedne jeden výsledek (selže bez výsledku). */
static st_DEVICE_REQ_RESULT take_one(void)
{
    st_DEVICE_REQ_RESULT res;
    TEST_ASSERT_TRUE(device_request_take_result(&res));
    return res;
}


void setUp(void)
{
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
    s_saved_std = pioz80_get_printer_std();
    s_saved_active = printer_get_active();
}

void tearDown(void)
{
    pioz80_set_printer_std(s_saved_std);
    printer_set_active(s_saved_active);
    device_request_clear();
    g_replay_mode = REPLAY_MODE_OFF;
}


/**
 * @brief Standard tiskárny a zachytávání se změní až v poll.
 */
void test_std_and_active_in_poll(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, printer_request_set_std(7));

    pioz80_set_printer_std(PIOZ80_PRINTER_STD_CENTRONICS);
    printer_set_active(false);
    printer_request_set_std(PIOZ80_PRINTER_STD_MZ);
    printer_request_set_active(true);
    printer_request_close_file();
    TEST_ASSERT_EQUAL_INT(PIOZ80_PRINTER_STD_CENTRONICS, pioz80_get_printer_std());
    TEST_ASSERT_FALSE(printer_get_active());

    device_request_poll();
    TEST_ASSERT_EQUAL_INT(PIOZ80_PRINTER_STD_MZ, pioz80_get_printer_std());
    TEST_ASSERT_TRUE(printer_get_active());
    TEST_ASSERT_NULL(g_printer.fp);
    TEST_ASSERT_EQUAL_STRING("printer_set_std", take_one().name);
    TEST_ASSERT_EQUAL_STRING("printer_set_active", take_one().name);
    TEST_ASSERT_EQUAL_STRING("printer_close_file", take_one().name);
}


/**
 * @brief Při aktivním replay se zásah neprovede.
 */
void test_rejected_while_replay_active(void)
{
    pioz80_set_printer_std(PIOZ80_PRINTER_STD_CENTRONICS);
    g_replay_mode = REPLAY_MODE_RECORDING;
    printer_request_set_std(PIOZ80_PRINTER_STD_MZ);
    device_request_poll();

    TEST_ASSERT_EQUAL_INT(PIOZ80_PRINTER_STD_CENTRONICS, pioz80_get_printer_std());
    TEST_ASSERT_EQUAL_INT(DEVICE_REQ_REJECTED_REPLAY, take_one().status);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_std_and_active_in_poll);
    RUN_TEST(test_rejected_while_replay_active);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
