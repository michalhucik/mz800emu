/**
 * @file test_mz800_bootstrap_dmd.c
 * @brief Bootstrap `--run-mzf` (MZ-800) nastavuje DMD cestou OUT CEh.
 *
 * ]GOPGM v ROM (ED02h) dělá skutečný OUT (0CEh),0. Bootstrap ho dřív
 * nahrazoval přímým zápisem g_gdg.regDMD = 0, takže po --run-mzf v 800
 * módu zůstal GATE0 CTC0 = 0 (gdg_reset ho v 700 módu nastaví podle
 * regct53g7 = 0) a latch zápisu MZ-700 nebyl vynulován. Nově jde DMD přes
 * gdg_write_byte(0x00CE, ...) jako paleta a border v téže funkci.
 *
 * Ověřuje se přímým voláním mzarch_platform_bootstrap_post_header():
 *  - větev MZ-800 módu: DMD = 0, GATE0 CTC0 = 1, latch = 0,
 *  - větev MZ-700 módu: DMD = 08h, GATE0 CTC0 beze změny (zápis 08h při
 *    DMD = 08h je bez účinku jako OUT se stejnou hodnotou).
 *
 * Nepokrývá: celé mzarch_bootstrap_run_mzf() (čtení MZF, cmthack) - to
 * pokrývá e2e test tests/mcp/test_run_mzf_state.py.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "mzarch/mzarch.h"
#include "mzarch/bootstrap.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "hw-generic/ctc8253/ctc8253.h"

#if MZARCH != 800
#error "test_mz800_bootstrap_dmd: testovací jádro je jen pro MZARCH=800"
#endif


/** @brief Uložená poloha zadního přepínače (obnoví se v tearDown). */
static en_MZ800_MODE_SW s_saved_mode_sw;


/**
 * @brief Stav jako po resetu v 700 módu: DMD 08h, regct53g7 = 0,
 *        GATE0 CTC0 = 0, latch zápisu MZ-700 použitý.
 */
void setUp(void)
{
    s_saved_mode_sw = g_mzarch_main.mode_sw;
    gdg_reset();
    g_gdg.regct53g7 = 0;
    g_ctc8253[0].gate = 0;
    g_vramctrl.mz700_wr_latch_is_used = 1;
}

void tearDown(void)
{
    g_mzarch_main.mode_sw = s_saved_mode_sw;
    gdg_reset();
}


/**
 * @brief Větev MZ-800 módu (mode_sw = MZ800_MODE_SW_MZ800): DMD 00h s vedlejšími efekty.
 */
void test_post_header_mz800_mode_sets_dmd_like_out(void)
{
    g_mzarch_main.mode_sw = MZ800_MODE_SW_MZ800;
    TEST_ASSERT_EQUAL_HEX32(0x08, g_gdg.regDMD);

    mzarch_platform_bootstrap_post_header(0x1200);

    TEST_ASSERT_EQUAL_HEX32(0x00, g_gdg.regDMD);
    TEST_ASSERT_EQUAL_UINT(1u, g_ctc8253[0].gate);
    TEST_ASSERT_EQUAL_UINT(0u, g_vramctrl.mz700_wr_latch_is_used);
}


/**
 * @brief Větev MZ-700 módu (mode_sw = MZ800_MODE_SW_MZ700): DMD zůstane 08h, GATE0
 *        i latch beze změny.
 */
void test_post_header_mz700_mode_keeps_state(void)
{
    g_mzarch_main.mode_sw = MZ800_MODE_SW_MZ700;

    mzarch_platform_bootstrap_post_header(0x1200);

    TEST_ASSERT_EQUAL_HEX32(0x08, g_gdg.regDMD);
    TEST_ASSERT_EQUAL_UINT(0u, g_ctc8253[0].gate);
    TEST_ASSERT_EQUAL_UINT(1u, g_vramctrl.mz700_wr_latch_is_used);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_post_header_mz800_mode_sets_dmd_like_out);
    RUN_TEST(test_post_header_mz700_mode_keeps_state);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
