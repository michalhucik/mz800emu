/*
 * test_memory.c — unit testy pro paměťový systém a ROM
 *
 * Testuje: RAM přístup, ROM overlay, memory mapping, ROM integritu,
 *          bank switching, VRAM přístup
 *
 * Licence: GPLv3
 */
#include "hw-generic/memory/memory_arch.h" /* per-arch memory - dříve tranzitivně přes memory.h (mzhal 11c-2c) */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/cmt/cmthack.h"
#include "emulator/emulator.h"

/* Memory testy ověřují čistou MZ-800 paměť (g_memory.RAM, ROM overlay,
 * memory mapping). Memext (PEHU/LUFTNER) přesměrovává bus pointery
 * g_memory.memram_read/write[] do vlastních bufferů, takže zápisy přes
 * memory_load_block() a čtení přes memory_read_byte() nesouhlasí
 * s g_memory.RAM[]. Cfgmain default startuje Memext připojený
 * (PEHU = nový default po změně memext_init), proto v setUp vždy
 * Memext odpojíme - testy musí běžet na holé paměti MZ-800. */
void setUp(void)
{
    memext_disconnect();
}
void tearDown(void) { }

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* RAM — základní zápis a čtení */
void test_memory_ram_basic(void)
{
    g_memory.RAM[0x1000] = 0x42;
    TEST_ASSERT_EQUAL_HEX8(0x42, g_memory.RAM[0x1000]);

    g_memory.RAM[0x1000] = 0x00;
    TEST_ASSERT_EQUAL_HEX8(0x00, g_memory.RAM[0x1000]);
}

/* RAM — zápis/čtení přes memory_write_byte / memory_read_byte */
void test_memory_ram_access_functions(void)
{
    /* oblast 0x2000-0x7FFF je vždy RAM (bez ROM overlay) */
    memory_write_byte(0x2000, 0xAB);
    TEST_ASSERT_EQUAL_HEX8(0xAB, memory_read_byte(0x2000));
}

/* ROM — ROM data jsou načtena po inicializaci */
void test_memory_rom_loaded(void)
{
    TEST_ASSERT_NOT_NULL(g_rom.mz700rom);
    TEST_ASSERT_NOT_NULL(g_rom.cgrom);
    TEST_ASSERT_NOT_NULL(g_rom.mz800rom);
}

/* ================================================================
 * UNIT TESTY — RAM
 * ================================================================ */

/* RAM — zápis/čtení na hranicích 64KB */
void test_memory_ram_boundaries(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* první bajt */
    g_memory.RAM[0x0000] = 0x11;
    TEST_ASSERT_EQUAL_HEX8(0x11, g_memory.RAM[0x0000]);

    /* poslední bajt */
    g_memory.RAM[0xFFFF] = 0xEE;
    TEST_ASSERT_EQUAL_HEX8(0xEE, g_memory.RAM[0xFFFF]);

    /* hranice 4KB stránek */
    for (int page = 0; page < 16; page++) {
        uint16_t addr = (uint16_t)(page * 0x1000);
        g_memory.RAM[addr] = (uint8_t)(page + 0xA0);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(page + 0xA0), g_memory.RAM[addr]);
    }
}

/* RAM — vzor zápis/čtení přes celých 64KB */
void test_memory_ram_full_range(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* uložit zálohu */
    uint8_t *backup = g_malloc(MEMORY_SIZE_RAM);
    memcpy(backup, g_memory.RAM, MEMORY_SIZE_RAM);

    /* zapsat vzor */
    for (int i = 0; i < MEMORY_SIZE_RAM; i++) {
        g_memory.RAM[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }

    /* ověřit */
    for (int i = 0; i < MEMORY_SIZE_RAM; i++) {
        uint8_t expected = (uint8_t)((i * 7 + 13) & 0xFF);
        if (g_memory.RAM[i] != expected) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected, g_memory.RAM[i],
                "Nesouhlasí RAM na některé adrese");
            break;
        }
    }

    /* obnovit */
    memcpy(g_memory.RAM, backup, MEMORY_SIZE_RAM);
    g_free(backup);
}

/* RAM — memory_load_block načte data do RAM */
void test_memory_load_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data[16];
    for (int i = 0; i < 16; i++) {
        data[i] = (uint8_t)(0x50 + i);
    }

    /* uložit zálohu */
    uint8_t backup[16];
    memcpy(backup, &g_memory.RAM[0x3000], 16);

    memory_load_block(data, 0x3000, 16, MEMORY_LOAD_RAMONLY);

    TEST_ASSERT_EQUAL_MEMORY(data, &g_memory.RAM[0x3000], 16);

    /* obnovit */
    memcpy(&g_memory.RAM[0x3000], backup, 16);
}

/* ================================================================
 * UNIT TESTY — ROM overlay a memory mapping
 * Pouze MZ-800 — memory mapping porty E0-E4 jsou MZ-800-specifické
 * ================================================================ */

#if MZARCH == 800

/* ROM overlay — po resetu je ROM namapovaná na 0x0000 */
void test_memory_rom_mapped_after_init(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* po memory_reset() by ROM měla být mapovaná */
    memory_reset();
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_0000);
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_E000);
}

/* ROM overlay — čtení z ROM oblasti vrací ROM data */
void test_memory_rom_read_returns_rom_data(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();

    /* přečíst z adresy 0x0000 — měla by tam být ROM */
    uint8_t rom_byte = memory_read_byte(0x0000);
    /* ROM data jsou v g_memory.ROM[] */
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[0], rom_byte);

    /* přečíst z CG-ROM oblasti 0x1000 */
    uint8_t cgrom_byte = memory_read_byte(0x1000);
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[ROM_SIZE_0000], cgrom_byte);
}

/* Memory mapping — OUT E0 odmapuje ROM 0x0000 */
void test_memory_mapping_unmap_rom_0000(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    memory_map_pwrite(MMAP_MZ800_PWRITE_E0, 0x00);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    /* po odmapování by se měla číst RAM */
    g_memory.RAM[0x0000] = 0x42;
    TEST_ASSERT_EQUAL_HEX8(0x42, memory_read_byte(0x0000));

    memory_reset(); /* úklid */
}

/* Memory mapping — OUT E1 odmapuje ROM E000 */
void test_memory_mapping_unmap_rom_e000(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_E000);

    memory_map_pwrite(MMAP_MZ800_PWRITE_E1, 0x00);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_E000);

    memory_reset(); /* úklid */
}

/* Memory mapping — OUT E2 namapuje ROM 0x0000 zpět */
void test_memory_mapping_remap_rom_0000(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();

    /* odmapovat */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E0, 0x00);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    /* namapovat zpět */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E2, 0x00);
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    memory_reset(); /* úklid */
}

/* Memory mapping — OUT E4 namapuje ROM 0x0000 + ROM E000 */
void test_memory_mapping_e4_maps_both_roms(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();

    /* odmapovat obě */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E0, 0x00);
    memory_map_pwrite(MMAP_MZ800_PWRITE_E1, 0x00);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_0000);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_E000);

    /* E4 namapuje obě */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E4, 0x00);
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_0000);
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_E000);

    memory_reset(); /* úklid */
}

/* Memory mapping — RAM pod ROM je zachována */
void test_memory_ram_under_rom_preserved(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    memory_reset();

    /* zapsat do RAM (přímo, ne přes mapping) */
    g_memory.RAM[0x0000] = 0xBE;
    g_memory.RAM[0x0001] = 0xEF;

    /* přečíst přes mapping — čte se ROM */
    TEST_ASSERT_EQUAL_HEX8(g_memory.ROM[0], memory_read_byte(0x0000));

    /* odmapovat ROM */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E0, 0x00);

    /* teď by se měla číst RAM */
    TEST_ASSERT_EQUAL_HEX8(0xBE, memory_read_byte(0x0000));
    TEST_ASSERT_EQUAL_HEX8(0xEF, memory_read_byte(0x0001));

    memory_reset(); /* úklid */
}

#endif /* MZARCH == 800 */

/* ================================================================
 * UNIT TESTY — ROM integrita
 * ================================================================ */

/* ROM — velikost ROM bufferu */
void test_memory_rom_size(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* ROM buffer musí mít ROM_SIZE_TOTAL bajtů */
    TEST_ASSERT_EQUAL_INT(ROM_SIZE_0000 + ROM_SIZE_CGROM + ROM_SIZE_E000,
                          ROM_SIZE_TOTAL);
    TEST_ASSERT_EQUAL_INT(0x4000, ROM_SIZE_TOTAL);
}

/* ROM — ROM není prázdná (samé nuly) */
void test_memory_rom_not_empty(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* alespoň jeden bajt v ROM 0x0000 musí být nenulový */
    bool rom0_nonzero = false;
    for (int i = 0; i < ROM_SIZE_0000; i++) {
        if (g_memory.ROM[i] != 0x00) {
            rom0_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(rom0_nonzero, "ROM 0x0000 je celá nulová");

    /* totéž pro CGROM */
    bool cgrom_nonzero = false;
    for (int i = 0; i < ROM_SIZE_CGROM; i++) {
        if (g_memory.ROM[ROM_SIZE_0000 + i] != 0x00) {
            cgrom_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(cgrom_nonzero, "CG-ROM je celá nulová");

    /* totéž pro ROM E000 */
    bool rome_nonzero = false;
    for (int i = 0; i < ROM_SIZE_E000; i++) {
        if (g_memory.ROM[ROM_SIZE_0000 + ROM_SIZE_CGROM + i] != 0x00) {
            rome_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(rome_nonzero, "ROM E000 je celá nulová");
}

/* ROM — g_rom pointery ukazují na zdrojová ROM data, g_memory.ROM obsahuje kopii */
void test_memory_rom_pointers(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* g_rom.mz700rom ukazuje na zdrojová ROM data (c_ROM_MZ800_0000 apod.) */
    TEST_ASSERT_NOT_NULL(g_rom.mz700rom);
    TEST_ASSERT_NOT_NULL(g_rom.cgrom);
    TEST_ASSERT_NOT_NULL(g_rom.mz800rom);

    /* g_memory.ROM obsahuje kopii dat z g_rom pointerů.
     * Pozor: pokud je CMTHACK nainstalován, kopie v g_memory.ROM
     * je patchovaná (RHEAD/RDATA přepis) a neodpovídá originálu. */
    if (!g_cmthack.load_patch_installed) {
        TEST_ASSERT_EQUAL_MEMORY(g_rom.mz700rom, &g_memory.ROM[0], ROM_SIZE_0000);
    }
    TEST_ASSERT_EQUAL_MEMORY(g_rom.cgrom, &g_memory.ROM[ROM_SIZE_0000], ROM_SIZE_CGROM);
    TEST_ASSERT_EQUAL_MEMORY(g_rom.mz800rom, &g_memory.ROM[ROM_SIZE_0000 + ROM_SIZE_CGROM], ROM_SIZE_E000);
}

/* ROM — ROM konfigurace existuje */
void test_memory_rom_config_exists(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_NOT_NULL(g_rom.rom_cfg);
    TEST_ASSERT_NOT_NULL(g_rom.rom_cfg->name);
}

/* ================================================================
 * UNIT TESTY — VRAM
 * ================================================================ */

/* VRAM — přímý přístup k VRAM bufferu */
void test_memory_vram_direct_access(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* uložit zálohu */
    uint8_t backup = g_memory.VRAM[0];

    g_memory.VRAM[0] = 0xCD;
    TEST_ASSERT_EQUAL_HEX8(0xCD, g_memory.VRAM[0]);

    g_memory.VRAM[MEMORY_SIZE_VRAM - 1] = 0xEF;
    TEST_ASSERT_EQUAL_HEX8(0xEF, g_memory.VRAM[MEMORY_SIZE_VRAM - 1]);

    /* obnovit */
    g_memory.VRAM[0] = backup;
}

#if MZARCH == 800
/* VRAM — EXVRAM existuje (pouze MZ-800) */
void test_memory_exvram_exists(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t backup = g_memory.EXVRAM[0];

    g_memory.EXVRAM[0] = 0xAB;
    TEST_ASSERT_EQUAL_HEX8(0xAB, g_memory.EXVRAM[0]);

    g_memory.EXVRAM[0] = backup;
}

/* VRAM — banky I-IV pointery jsou nastaveny (pouze MZ-800) */
void test_memory_vram_bank_pointers(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_NOT_NULL(g_memoryVRAM_I);
    TEST_ASSERT_NOT_NULL(g_memoryVRAM_II);
    TEST_ASSERT_NOT_NULL(g_memoryVRAM_III);
    TEST_ASSERT_NOT_NULL(g_memoryVRAM_IV);
}
#endif /* MZARCH == 800 */

/* ================================================================
 * FULL TESTY
 * ================================================================ */

#if MZARCH == 800
/* Memory reset — po resetu je ROM namapovaná, RAM zachovaná */
void test_memory_reset_preserves_ram(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_FULL);

    /* zapsat vzor do RAM */
    g_memory.RAM[0x4000] = 0xDE;
    g_memory.RAM[0x4001] = 0xAD;

    /* odmapovat ROM */
    memory_map_pwrite(MMAP_MZ800_PWRITE_E0, 0x00);
    TEST_ASSERT_FALSE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    /* reset */
    memory_reset();

    /* ROM by měla být zpět namapovaná */
    TEST_ASSERT_TRUE(MEMORY_MZ800_MAP_TEST_ROM_0000);

    /* RAM by měla být zachovaná (teplý reset nemění RAM) */
    TEST_ASSERT_EQUAL_HEX8(0xDE, g_memory.RAM[0x4000]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, g_memory.RAM[0x4001]);
}
#endif /* MZARCH == 800 */

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_memory_ram_basic);
    RUN_TEST(test_memory_ram_access_functions);
    RUN_TEST(test_memory_rom_loaded);

    /* unit — RAM */
    RUN_TEST(test_memory_ram_boundaries);
    RUN_TEST(test_memory_ram_full_range);
    RUN_TEST(test_memory_load_block);

    /* unit — ROM overlay a mapping (pouze MZ-800) */
#if MZARCH == 800
    RUN_TEST(test_memory_rom_mapped_after_init);
    RUN_TEST(test_memory_rom_read_returns_rom_data);
    RUN_TEST(test_memory_mapping_unmap_rom_0000);
    RUN_TEST(test_memory_mapping_unmap_rom_e000);
    RUN_TEST(test_memory_mapping_remap_rom_0000);
    RUN_TEST(test_memory_mapping_e4_maps_both_roms);
    RUN_TEST(test_memory_ram_under_rom_preserved);
#endif

    /* unit — ROM integrita */
    RUN_TEST(test_memory_rom_size);
    RUN_TEST(test_memory_rom_not_empty);
    RUN_TEST(test_memory_rom_pointers);
    RUN_TEST(test_memory_rom_config_exists);

    /* unit — VRAM */
    RUN_TEST(test_memory_vram_direct_access);
#if MZARCH == 800
    RUN_TEST(test_memory_exvram_exists);
    RUN_TEST(test_memory_vram_bank_pointers);
#endif

    /* full (pouze MZ-800) */
#if MZARCH == 800
    RUN_TEST(test_memory_reset_preserves_ram);
#endif

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
