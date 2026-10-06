/**
 * @file test_memmap_window_queue.c
 * @brief Banking a DMD z okna Memory Map jdou přes CMDRQ frontu, ne přímo
 *        z UI vlákna.
 *
 * Regresní test pro souběh v okně Memory Map (memmap_window.cpp). UI
 * vlákno dřív přímo:
 *  - přepisovalo g_memory.map (levý klik na buňku Banking, popup
 *    Mount / Umount / Inhibit / Mount All / Umount All, MZ-1500 SPEC),
 *  - zapisovalo g_gdg.regDMD (DMD roletka, MZ-800),
 *  - volalo memory_reconnect_ram() (přepojení memram_read/write a na
 *    MZ-800 přepočet RAM fast-path tabulky CPU) a refresh framebufferu,
 * zatímco emu vlákno tytéž hodnoty čte při každém přístupu CPU do paměti
 * a samo je mění (OUT / IN E0-E6, OUT CEh). Nově okno volá UI helpery
 * dbg_ui_memmap_change_map / dbg_ui_memmap_set_dmd a vše vykoná emu vlákno
 * v DBGAPI_CMD_MEMMAP_SET.
 *
 * Ověřuje se přes skutečné helpery z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se banking ani DMD nezmění
 *    a mapování stránek (memmap_query, RAM fast-path) zůstane původní
 *    (helper vrátí false po timeoutu, zrušený slot emu nevyzvedne),
 *  - se simulovaným emu vláknem změny projdou, emu vlákno vidí příkaz
 *    MEMMAP_SET, bity mimo masky zůstanou zachované a mapování stránek
 *    i fast-path tabulka odpovídají nové hodnotě (= memory_reconnect_ram
 *    proběhl),
 *  - DMD se nastaví cestou OUT CEh (gdg_debug_set_regDMD): GATE0 CTC0
 *    a latch zápisu MZ-700 se změní jako po OUT (regrese: přímý zápis
 *    g_gdg.regDMD je vynechával).
 *
 * Nepokrývá: reálný drain fronty v mzarch smyčce (simuluje se vlastním
 * vláknem), vykreslení okna (ImGui se nevolá) a platformy MZ-700 /
 * MZ-1500 - testovací jádro (mz_test_core_mz800) se zatím sestavuje jen
 * pro MZARCH=800, viz cmake/AddMzTest.cmake.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/debugger.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mzarch.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"

#if MZARCH != 800
#error "test_memmap_window_queue: testovací jádro je jen pro MZARCH=800"
#endif


/** @brief Výchozí banking: vše namapované (= stav po Mount All). */
#define TEST_MAP_ALL ( MEMORY_MZ800_MAP_FLAG_ROM_0000 \
                     | MEMORY_MZ800_MAP_FLAG_ROM_1000 \
                     | MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM \
                     | MEMORY_MZ800_MAP_FLAG_ROM_E000 )

/** @brief Výchozí DMD: režim MZ-700 (bit 3), 8000h-BFFFh je RAM. */
#define TEST_DMD_MZ700 0x08u

/** @brief DMD 320x200 @ 4 / A: 8000h-9FFFh je VRAM I (při CGRAM_VRAM). */
#define TEST_DMD_MZ800_320x4A 0x00u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy MEMMAP_SET. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;            /**< 1 = ukončit smyčku (atomicky). */
    gint memmap_seen;     /**< Počet vyzvednutých DBGAPI_CMD_MEMMAP_SET. */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna: drain fronty jako mzarch drain.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread(gpointer user_data)
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *)user_data;
    while (!g_atomic_int_get(&fe->stop))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue);
        if (!rq)
        {
            (void)dbgapi_emu_wait_for_cmd(&g_dbgapi_cmdrq_queue, 5);
            continue;
        };
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_MEMMAP_SET)
        {
            g_atomic_int_inc(&fe->memmap_seen);
        };
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/**
 * @brief Vrátí true, pokud je stránka v RAM fast-path tabulce CPU jako
 *        přímý RAM přístup (non-NULL).
 *
 * Bez MZ800EMU_CFG_RAM_FASTPATH tabulka neexistuje; pak vrací
 * @p expected, aby kontrola nic neovlivnila.
 *
 * @param page     4 KB stránka 0..15.
 * @param expected Očekávaná hodnota (vrací se bez fast-path buildu).
 */
static bool fastpath_page_is_ram(int page, bool expected)
{
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    (void)expected;
    return g_mzarch_main.cpu->ram_fp_read[page] != NULL;
#else
    (void)page;
    return expected;
#endif
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    /* Testy bez emu vlákna refresh nepotřebují; vláknový test ho zapíná. */
    g_debugger.screen_refresh_on_edit = 0;
    g_memory.map = TEST_MAP_ALL;
    g_gdg.regDMD = TEST_DMD_MZ700;
    memory_reconnect_ram();
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_ROM_LOW, memmap_query(0x0));
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_RAM, memmap_query(0x8));
}

void tearDown(void)
{
    g_debugger.screen_refresh_on_edit = 0;
    g_memory.map = TEST_MAP_ALL;
    g_gdg.regDMD = TEST_DMD_MZ700;
    memory_reconnect_ram();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se banking (Umount ROM $0000) nezmění.
 */
void test_map_change_not_applied_without_emu_thread(void)
{
    uint8_t after = 0xAA;
    TEST_ASSERT_FALSE(dbg_ui_memmap_change_map(MEMORY_MZ800_MAP_FLAG_ROM_0000,
                                               0, &after));
    TEST_ASSERT_EQUAL_HEX8(TEST_MAP_ALL, g_memory.map);
    TEST_ASSERT_EQUAL_HEX8(0xAA, after);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_ROM_LOW, memmap_query(0x0));
    TEST_ASSERT_FALSE(fastpath_page_is_ram(0x0, false));

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Bez emu vlákna se DMD nezmění a 8000h zůstane RAM.
 */
void test_dmd_not_applied_without_emu_thread(void)
{
    TEST_ASSERT_FALSE(dbg_ui_memmap_set_dmd(TEST_DMD_MZ800_320x4A));
    TEST_ASSERT_EQUAL_HEX32(TEST_DMD_MZ700, g_gdg.regDMD);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_RAM, memmap_query(0x8));
    TEST_ASSERT_TRUE(fastpath_page_is_ram(0x8, true));

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projdou banking i DMD a mapování
 *        stránek je po každém příkazu konzistentní.
 *
 * Se zapnutým "Auto refresh on edit" (refresh běží na emu vlákně; jeho
 * výsledek se nekontroluje).
 *
 * Pořadí: Umount ROM $0000 (PROHIBITED nastavené "OUT E5" předem musí
 * zůstat), DMD na 320x200 (8000h přejde z RAM na VRAM I), Umount All,
 * Mount All a návrat DMD.
 */
void test_ops_executed_by_emu_thread(void)
{
    /* Bit, který by mezitím nastavilo emu vlákno (OUT E5); UI se ho
     * nedotýká a nesmí ho přepsat. */
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    /* Handler volá i debugger_screen_refresh_if_enabled(); se zapnutým
     * "Auto refresh on edit" proběhne vynucený refresh framebufferu na
     * (simulovaném) emu vlákně. Ověřuje se jen, že cesta projde. */
    g_debugger.screen_refresh_on_edit = 1;

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    uint8_t after_umount0 = 0;
    bool ok_umount0 = dbg_ui_memmap_change_map(MEMORY_MZ800_MAP_FLAG_ROM_0000,
                                               0, &after_umount0);
    uint8_t map_umount0 = g_memory.map;
    int kind0_umount0 = memmap_query(0x0);
    bool fp0_umount0 = fastpath_page_is_ram(0x0, true);

    bool ok_dmd = dbg_ui_memmap_set_dmd(TEST_DMD_MZ800_320x4A);
    unsigned dmd_after = g_gdg.regDMD;
    int kind8_dmd = memmap_query(0x8);
    bool fp8_dmd = fastpath_page_is_ram(0x8, false);

    uint8_t after_all_off = 0xAA;
    bool ok_all_off = dbg_ui_memmap_change_map(0xFF, 0, &after_all_off);
    int kind8_all_off = memmap_query(0x8);
    bool fp8_all_off = fastpath_page_is_ram(0x8, true);

    bool ok_all_on = dbg_ui_memmap_change_map(0xFF, TEST_MAP_ALL, NULL);
    int kind0_all_on = memmap_query(0x0);

    bool ok_dmd_back = dbg_ui_memmap_set_dmd(TEST_DMD_MZ700);
    int kind8_back = memmap_query(0x8);
    bool fp8_back = fastpath_page_is_ram(0x8, true);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_umount0);
    TEST_ASSERT_EQUAL_HEX8(( TEST_MAP_ALL | MEMORY_MZ800_MAP_FLAG_PROHIBITED )
                           & ~MEMORY_MZ800_MAP_FLAG_ROM_0000, map_umount0);
    TEST_ASSERT_EQUAL_HEX8(map_umount0, after_umount0);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_RAM, kind0_umount0);
    TEST_ASSERT_TRUE(fp0_umount0);

    TEST_ASSERT_TRUE(ok_dmd);
    TEST_ASSERT_EQUAL_HEX32(TEST_DMD_MZ800_320x4A, dmd_after);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_VRAM_I, kind8_dmd);
    TEST_ASSERT_FALSE(fp8_dmd);

    TEST_ASSERT_TRUE(ok_all_off);
    TEST_ASSERT_EQUAL_HEX8(0x00, after_all_off);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_RAM, kind8_all_off);
    TEST_ASSERT_TRUE(fp8_all_off);

    TEST_ASSERT_TRUE(ok_all_on);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_ROM_LOW, kind0_all_on);

    TEST_ASSERT_TRUE(ok_dmd_back);
    TEST_ASSERT_EQUAL_INT(MEMMAP_KIND_RAM, kind8_back);
    TEST_ASSERT_TRUE(fp8_back);

    TEST_ASSERT_EQUAL_INT(5, g_atomic_int_get(&fe.memmap_seen));
}


/**
 * @brief DMD z okna má vedlejší efekty GDG jako OUT (CEh).
 *
 * Regrese: handler MEMMAP_SET dřív zapisoval g_gdg.regDMD přímo, takže
 * přechod do 800 módu nenastavil GATE0 CTC0 na 1
 * (ctc82530_on_regDMD_changed) ani nevynuloval latch zápisu MZ-700
 * (g_vramctrl.mz700_wr_latch_is_used). Nově handler volá
 * gdg_debug_set_regDMD() = stejná cesta jako OUT CEh.
 *
 * Výchozí stav: 700 mód, E008h GATE0 = 0 (regct53g7), GATE0 CTC0 = 0,
 * latch použitý. Po DMD 00h: GATE0 = 1, latch 0. Po návratu na 08h:
 * GATE0 zpět podle regct53g7 = 0.
 */
void test_dmd_side_effects_like_out_ceh(void)
{
    g_gdg.regct53g7 = 0;
    g_ctc8253[0].gate = 0;
    g_vramctrl.mz700_wr_latch_is_used = 1;

    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    bool ok_800 = dbg_ui_memmap_set_dmd(TEST_DMD_MZ800_320x4A);
    unsigned gate_800 = g_ctc8253[0].gate;
    unsigned latch_800 = g_vramctrl.mz700_wr_latch_is_used;

    bool ok_700 = dbg_ui_memmap_set_dmd(TEST_DMD_MZ700);
    unsigned gate_700 = g_ctc8253[0].gate;

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_800);
    TEST_ASSERT_EQUAL_UINT(1u, gate_800);
    TEST_ASSERT_EQUAL_UINT(0u, latch_800);

    TEST_ASSERT_TRUE(ok_700);
    TEST_ASSERT_EQUAL_HEX32(TEST_DMD_MZ700, g_gdg.regDMD);
    TEST_ASSERT_EQUAL_UINT(0u, gate_700);

    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.memmap_seen));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_map_change_not_applied_without_emu_thread);
    RUN_TEST(test_dmd_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);
    RUN_TEST(test_dmd_side_effects_like_out_ceh);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
