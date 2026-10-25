/**
 * @file test_mz800_gdg_reset.c
 * @brief Stav registrů GDG MZ-800 po gdg_reset() podle HW.
 *
 * Simulace netlistu GDG: reset (/PORT i tlačítko RESET /MNRT) vynuluje
 * paletu (všechny čtyři položky IGRB 0, černá), DMD (režim MZ-800
 * 320x200) a barvu okraje. Emulátor dřív po resetu nastavoval paletu
 * 09h/0Fh/09h/0Fh, takže program, který se sám přepne do režimu 800
 * a paletu nenastaví, měl barvy tam, kde HW ukáže černou.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "mzarch/mz800/gdg/mz800_gdg.h"

#if MZARCH != 800
#error "test_mz800_gdg_reset: testovací jádro je jen pro MZARCH=800"
#endif


void setUp(void) { }

void tearDown(void)
{
    gdg_reset();
}


/**
 * @brief Po resetu je paleta černá, PALGRP 0, DMD 0, okraj 0, i když
 *        předtím byly registry nastavené jinak.
 */
void test_gdg_reset_palette_black(void)
{
    g_gdg.regPAL0 = 0x05;
    g_gdg.regPAL1 = 0x0a;
    g_gdg.regPAL2 = 0x0c;
    g_gdg.regPAL3 = 0x0f;
    g_gdg.regPALGRP = 2;
    g_gdg.regBOR = 0x07;
    gdg_debug_set_regDMD(0x08);

    gdg_reset();

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regPAL0, "PAL0");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regPAL1, "PAL1");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regPAL2, "PAL2");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regPAL3, "PAL3");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, g_gdg.regPALGRP, "PALGRP");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regDMD, "DMD");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00, g_gdg.regBOR, "BOR");
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_gdg_reset_palette_black);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
