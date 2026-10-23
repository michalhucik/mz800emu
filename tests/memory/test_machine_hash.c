/*
 * File:   test_machine_hash.c
 *
 * Obsahový otisk stroje po částech (machine_hash.c): jména částí, skladba
 * celkového otisku, co která část vidí (mapa paměti, ROM, vstupy, kazeta,
 * média, RAM disk) a že čtení otisku stav nemění.
 */

/**
 * @file test_machine_hash.c
 * @brief Testy machine_hash_part(), machine_hash_compute() a jmen částí.
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/machine_hash.h"
#include "emulator/state_hash.h"
#include "emulator/media_writes.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/ramdisk/ramdisk.h"
#include "hw-generic/cmt/cmt.h"
#include "mzarch/mzarch.h"
#if HAVE_JOY
#include "hw-generic/joy/joy.h"
#endif


void setUp(void)
{
    memext_disconnect();
}

void tearDown(void)
{
    memext_disconnect();
}


/** @brief Hash části s assertem, že se spočítal. */
static void part_hex(en_MACHINE_HASH_PART part, unsigned flags, char *out)
{
    TEST_ASSERT_TRUE_MESSAGE(machine_hash_part(part, flags, out), machine_hash_part_name(part));
    TEST_ASSERT_EQUAL_size_t(STATE_HASH_HEX_LEN, strlen(out));
}


/** @brief Jména částí v pořadí výčtu a převod zpět. */
void test_machine_hash_part_names(void)
{
    static const char *const names[] = {
        "cpu", "ram", "memext", "vram", "pcg", "periph",
        "memmap", "rom", "media", "inputs", "ramdisk", "cmt",
    };
    TEST_ASSERT_EQUAL_INT(MACHINE_HASH_PART_COUNT, (int)G_N_ELEMENTS(names));
    for (int i = 0; i < MACHINE_HASH_PART_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_STRING(names[i], machine_hash_part_name((en_MACHINE_HASH_PART)i));
        TEST_ASSERT_EQUAL_INT(i, machine_hash_part_from_name(names[i]));
    };
    TEST_ASSERT_EQUAL_STRING("unknown", machine_hash_part_name(MACHINE_HASH_PART_COUNT));
    TEST_ASSERT_EQUAL_INT(-1, machine_hash_part_from_name("screen"));
    TEST_ASSERT_EQUAL_INT(-1, machine_hash_part_from_name(NULL));
}


/** @brief Části ze state_hash jsou stejné jako state_hash_part(). */
void test_machine_hash_state_hash_parts(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_RAM, 0, a);
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_RAM, b));
    TEST_ASSERT_EQUAL_STRING(b, a);
    part_hex(MACHINE_HASH_VRAM, 0, a);
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_VRAM, b));
    TEST_ASSERT_EQUAL_STRING(b, a);
    TEST_ASSERT_FALSE(machine_hash_part(MACHINE_HASH_MEMEXT, 0, a));
#if MZARCH != 1500
    TEST_ASSERT_FALSE(machine_hash_part(MACHINE_HASH_PCG, 0, a));
#endif
}


/** @brief Celkový otisk = SHA-256 řádků `jméno:hex` (chybějící část `-`). */
void test_machine_hash_compute_total(void)
{
    st_MACHINE_HASH h;
    const uint32_t mask = (1u << MACHINE_HASH_RAM) | (1u << MACHINE_HASH_MEMEXT) | (1u << MACHINE_HASH_ROM);
    machine_hash_compute(mask | 0x80000000u, 0, &h);
    TEST_ASSERT_EQUAL_UINT32(mask, h.requested);
    TEST_ASSERT_EQUAL_UINT32((1u << MACHINE_HASH_RAM) | (1u << MACHINE_HASH_ROM), h.present);
    TEST_ASSERT_EQUAL_STRING("", h.part[MACHINE_HASH_MEMEXT]);
    TEST_ASSERT_EQUAL_STRING("", h.part[MACHINE_HASH_CPU]);

    char *text = g_strdup_printf("ram:%s\nmemext:-\nrom:%s\n", h.part[MACHINE_HASH_RAM], h.part[MACHINE_HASH_ROM]);
    char ref[STATE_HASH_HEX_LEN + 1];
    state_hash_buffer(text, strlen(text), ref);
    g_free(text);
    TEST_ASSERT_EQUAL_STRING(ref, h.total);

    /* Všechny části: bez MemExt chybí jen memext (a pcg mimo MZ-1500). */
    machine_hash_compute(MACHINE_HASH_ALL_PARTS, 0, &h);
    uint32_t absent = 1u << MACHINE_HASH_MEMEXT;
#if MZARCH != 1500
    absent |= 1u << MACHINE_HASH_PCG;
#endif
    TEST_ASSERT_EQUAL_UINT32(MACHINE_HASH_ALL_PARTS & ~absent, h.present);

    /* Opakovaný výpočet bez změny stavu = stejný otisk. */
    st_MACHINE_HASH h2;
    machine_hash_compute(MACHINE_HASH_ALL_PARTS, 0, &h2);
    TEST_ASSERT_EQUAL_STRING(h.total, h2.total);
}


/** @brief `memmap` vidí mapu paměti a mapu MemExt, `ram` je nevidí. */
void test_machine_hash_memmap(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    char ra[STATE_HASH_HEX_LEN + 1], rb[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_MEMMAP, 0, a);
    part_hex(MACHINE_HASH_RAM, 0, ra);
    uint8_t map = g_memory.map;
    g_memory.map ^= 0x01;
    part_hex(MACHINE_HASH_MEMMAP, 0, b);
    part_hex(MACHINE_HASH_RAM, 0, rb);
    g_memory.map = map;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    TEST_ASSERT_EQUAL_STRING(ra, rb);

    uint32_t m0 = g_memext.map[3];
    g_memext.map[3] ^= 0x05;
    part_hex(MACHINE_HASH_MEMMAP, 0, b);
    g_memext.map[3] = m0;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    memext_connect(MEMEXT_TYPE_PEHU);
    part_hex(MACHINE_HASH_MEMMAP, 0, b);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}


/** @brief `rom` vidí změnu obsahu ROM (patch CMT hacku). */
void test_machine_hash_rom(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_ROM, 0, a);
    g_memory.ROM[0x10] ^= 0xFF;
    part_hex(MACHINE_HASH_ROM, 0, b);
    g_memory.ROM[0x10] ^= 0xFF;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    part_hex(MACHINE_HASH_ROM, 0, b);
    TEST_ASSERT_EQUAL_STRING(a, b);
}


/** @brief `inputs` vidí výslednou matici klávesnice (host i VKBD) a joystick. */
void test_machine_hash_inputs(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_INPUTS, 0, a);

    uint8_t k = g_pio8255.keyboard_matrix[4];
    g_pio8255.keyboard_matrix[4] = (uint8_t)(k & ~0x10);
    part_hex(MACHINE_HASH_INPUTS, 0, b);
    g_pio8255.keyboard_matrix[4] = k;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    uint8_t v = g_pio8255.vkbd_matrix[2];
    g_pio8255.vkbd_matrix[2] = (uint8_t)(v & ~0x01);
    part_hex(MACHINE_HASH_INPUTS, 0, b);
    g_pio8255.vkbd_matrix[2] = v;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

#if HAVE_JOY
    uint8_t js = g_joy.dev[0].state;
    g_joy.dev[0].state = (uint8_t)(js ^ 0x01);
    part_hex(MACHINE_HASH_INPUTS, 0, b);
    g_joy.dev[0].state = js;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
#endif

    part_hex(MACHINE_HASH_INPUTS, 0, b);
    TEST_ASSERT_EQUAL_STRING(a, b);
}


/** @brief `cmt` vidí stav transportu, `media` počítadlo zápisů a příznak obsahu. */
void test_machine_hash_cmt_media(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_CMT, 0, a);
    int out = g_cmt.output;
    g_cmt.output = !out;
    part_hex(MACHINE_HASH_CMT, 0, b);
    g_cmt.output = out;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    part_hex(MACHINE_HASH_MEDIA, 0, a);
    uint32_t w = g_media_writes;
    MEDIA_WRITES_NOTE();
    part_hex(MACHINE_HASH_MEDIA, 0, b);
    g_media_writes = w;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    /* FLASH MemExt jen s příznakem obsahu. */
    memext_connect(MEMEXT_TYPE_PEHU);
    char c0[STATE_HASH_HEX_LEN + 1], c1[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_MEDIA, 0, a);
    part_hex(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, c0);
    g_memext.FLASH[100] ^= 0xFF;
    part_hex(MACHINE_HASH_MEDIA, 0, b);
    part_hex(MACHINE_HASH_MEDIA, MACHINE_HASH_FLAG_INCLUDE_MEDIA, c1);
    g_memext.FLASH[100] ^= 0xFF;
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_TRUE(strcmp(c0, c1) != 0);
}


/** @brief `ramdisk` vidí stav STD RAM disku. */
void test_machine_hash_ramdisk(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    part_hex(MACHINE_HASH_RAMDISK, 0, a);
    uint8_t bank = g_ramdisk.std.bank;
    g_ramdisk.std.bank = (uint8_t)(bank ^ 0x01);
    part_hex(MACHINE_HASH_RAMDISK, 0, b);
    g_ramdisk.std.bank = bank;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_machine_hash_part_names);
    RUN_TEST(test_machine_hash_state_hash_parts);
    RUN_TEST(test_machine_hash_compute_total);
    RUN_TEST(test_machine_hash_memmap);
    RUN_TEST(test_machine_hash_rom);
    RUN_TEST(test_machine_hash_inputs);
    RUN_TEST(test_machine_hash_cmt_media);
    RUN_TEST(test_machine_hash_ramdisk);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
