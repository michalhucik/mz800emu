/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_im2_vector.c
 * @brief Testy adresy záznamu tabulky vektorů v IM 2 (bit 0 vektoru).
 *
 * Z80 při přijetí přerušení v IM 2 čte cílovou adresu ze záznamu
 * (I << 8) | vektor. Zilog uvádí, že bit 0 je vždy 0, ale procesor ho
 * nemaskuje (Sean Young, báze cpu/z80/07-interrupts.md). Na MZ-800 přichází
 * lichý vektor jako duch sběrnice, když vektor nedodá nikdo (CTC2, /EXINT).
 * Jádro do 2026-11-26 bit 0 maskovalo (bod I1 v bázi
 * mz800emu/02-rozdily-proti-hw.md); test na tom stavu selže.
 *
 * Model odpovídá reálné MZ cestě: external_int_handling = true a přijetí
 * přes z80_int + z80_process_interrupt jako v mzarch_main_process_interrupt;
 * vektor dodává intread callback.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_ei_delay.c) ===== */

static uint8_t g_mem[0x10000];
static uint8_t g_int_vector;   /**< Bajt, který CPU přečte ze sběrnice při INTACK. */

static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1, void *u)
{ (void)cpu; (void)m1; (void)u; return g_mem[addr]; }

static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t v, void *u)
{ (void)cpu; (void)u; g_mem[addr] = v; }

static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *u)
{ (void)cpu; (void)port; (void)u; return 0xFF; }

static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t v, void *u)
{ (void)cpu; (void)port; (void)v; (void)u; }

static uint8_t test_intread(struct z80_s *cpu, void *u)
{ (void)cpu; (void)u; return g_int_vector; }


/* ===== setUp / tearDown ===== */

static z80_t *g_cpu;

void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_int_vector = 0x00;
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
    g_cpu->external_int_handling = true;
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/**
 * @brief Připraví CPU v IM 2 s povolenými přerušeními a přijme jedno INT.
 *
 * @param i_reg   Hodnota registru I.
 * @param vector  Bajt, který CPU přečte jako vektor.
 * @return Počet T-stavů přijetí (0 = nepřijato).
 * @post Při přijetí je PC = slovo přečtené ze záznamu tabulky.
 */
static int accept_im2(uint8_t i_reg, uint8_t vector)
{
    g_cpu->pc   = 0x0100;
    g_cpu->sp   = 0xC000;
    g_cpu->im   = 2;
    g_cpu->i    = i_reg;
    g_cpu->iff1 = 1;
    g_cpu->iff2 = 1;
    g_int_vector = vector;
    z80_int(g_cpu);
    return z80_process_interrupt(g_cpu);
}


/* ===== Testy ===== */

/**
 * @test Lichý vektor: záznam se čte z (I << 8) | vektor, ne z vektor & 0FEh.
 *       Záznamy 39FEh a 39FFh se překrývají; 39FEh = 7C00h, 39FFh = A57Ch.
 */
void test_odd_vector_uses_odd_entry(void)
{
    g_mem[0x39FE] = 0x00;
    g_mem[0x39FF] = 0x7C;
    g_mem[0x3A00] = 0xA5;

    TEST_ASSERT_EQUAL_INT(19, accept_im2(0x39, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0xA57C, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX16(0xA57C, g_cpu->wz.w);
}

/**
 * @test Sudý vektor beze změny: záznam (I << 8) | vektor.
 */
void test_even_vector_unchanged(void)
{
    g_mem[0x4080] = 0x00;
    g_mem[0x4081] = 0x50;

    TEST_ASSERT_EQUAL_INT(19, accept_im2(0x40, 0x80));
    TEST_ASSERT_EQUAL_HEX16(0x5000, g_cpu->pc);
}

/**
 * @test Tabulka 257 bajtů vyplněná jedním bajtem (recept báze
 *       firmware/interrupts.md) vede na tutéž obsluhu pro libovolný vektor,
 *       sudý i lichý.
 */
void test_filled_table_any_vector(void)
{
    for (unsigned v = 0; v < 256; v++) {
        memset(&g_mem[0x2000], 0x22, 257);
        TEST_ASSERT_EQUAL_INT(19, accept_im2(0x20, (uint8_t)v));
        TEST_ASSERT_EQUAL_HEX16(0x2222, g_cpu->pc);
    }
}

/**
 * @test Návratová adresa se uloží na zásobník i při lichém vektoru.
 */
void test_odd_vector_pushes_return_address(void)
{
    g_mem[0x3901] = 0x34;
    g_mem[0x3902] = 0x12;

    TEST_ASSERT_EQUAL_INT(19, accept_im2(0x39, 0x01));
    TEST_ASSERT_EQUAL_HEX16(0x1234, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX16(0xBFFE, g_cpu->sp);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_mem[0xBFFE]);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_mem[0xBFFF]);
}


/* ===== Unity main ===== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_odd_vector_uses_odd_entry);
    RUN_TEST(test_even_vector_unchanged);
    RUN_TEST(test_filled_table_any_vector);
    RUN_TEST(test_odd_vector_pushes_return_address);
    return UNITY_END();
}
