/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_read16_order.c
 * @brief Unit testy pořadí a T-stavu čtení 16bitové hodnoty z paměti.
 *
 * Zilog UM0080 (Z80 CPU User Manual): LD HL,(nn), LD dd,(nn), LD IX,(nn)
 * čtou nejdřív nižší bajt z nn, pak vyšší z nn+1; POP, RET, RETI, RETN
 * čtou nejdřív nižší bajt z SP, pak vyšší z SP+1; EX (SP),HL čte nižší
 * bajt z SP a vyšší z SP+1 a teprve pak zapisuje.
 *
 * Každé čtení je samostatný M-cyklus o 3 T: druhé čtení musí vidět
 * cpu->op_tstate o 3 vyšší než první. Na tom závisí synchronizace s GDG
 * (čekací takty u VRAM, čas čtení mapovaných portů). Jádro dřív skládalo
 * hodnotu výrazem RD(a) | (RD(a + 1) << 8), který měnil op_tstate dvakrát
 * bez určeného pořadí; GCC 16.2 (-O0 i -O2) provedl obě přičtení před
 * prvním čtením, takže obě čtení viděla stejný op_tstate (pořadí adres
 * bylo správné). Zjištěno při srovnání čekacích taktů s měřením na MZ-800
 * (emu-experiments gdg-fixes).
 *
 * Pokrytí (pořadí čtení a jejich op_tstate z callbacku paměti, výsledná
 * hodnota):
 *   LD HL,(nn), LD BC,(nn) (ED 4B), LD SP,(nn) (ED 7B), LD IX,(nn),
 *   POP BC, POP IX, RET, RET NZ (splněno), RETI, EX (SP),HL.
 *
 * Standalone test - cpu-z80 bez emulátoru, paměť simulovaná v testu.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/** Plná 64 KB paměť testu. */
static uint8_t g_mem[0x10000];

/** Začátek datové oblasti; čtení pod ní (kód) se nezaznamenává. */
#define DATA_BASE 0x4000

/** Největší počet zaznamenaných čtení. */
#define MAX_READS 16

/** Adresy datových čtení v pořadí, jak je CPU provedlo. */
static uint16_t g_reads[MAX_READS];
/** Hodnota cpu->op_tstate v okamžiku každého datového čtení. */
static int g_read_tstate[MAX_READS];
static int g_nreads;


static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1_state, void *user_data)
{
    (void)m1_state;
    (void)user_data;
    if (addr >= DATA_BASE) {
        if (g_nreads < MAX_READS) {
            g_reads[g_nreads] = addr;
            g_read_tstate[g_nreads] = cpu->op_tstate;
        }
        g_nreads++;
    }
    return g_mem[addr];
}


static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    (void)cpu;
    (void)user_data;
    g_mem[addr] = value;
}


static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *user_data)
{
    (void)cpu;
    (void)port;
    (void)user_data;
    return 0xFF;
}


static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t value, void *user_data)
{
    (void)cpu;
    (void)port;
    (void)value;
    (void)user_data;
}


static uint8_t test_intread(struct z80_s *cpu, void *user_data)
{
    (void)cpu;
    (void)user_data;
    return 0xFF;
}


static z80_t *g_cpu;


void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_nreads = 0;
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL, test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
}


void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/**
 * @brief Provede jednu instrukci z adresy 0100h a ověří pořadí datových čtení.
 *
 * Na @p addr uloží hodnotu 0BEEFh (nižší bajt na addr), SP nastaví na
 * @p addr, instrukci zkopíruje na 0100h a krokuje.
 *
 * @param code Bajty instrukce.
 * @param len  Délka instrukce.
 * @param addr Adresa čtené 16bitové hodnoty (>= DATA_BASE).
 * @post Proběhla právě dvě datová čtení: addr, pak addr + 1, druhé o 3 T
 *       později než první.
 */
static void step_and_check(const uint8_t *code, int len, uint16_t addr)
{
    memcpy(&g_mem[0x0100], code, (size_t)len);
    g_mem[addr] = 0xEF;
    g_mem[(uint16_t)(addr + 1)] = 0xBE;
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0100);
    z80_set_reg(g_cpu, Z80_REG_SP, addr);
    g_nreads = 0;
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_nreads, "expected exactly two data reads");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(addr, g_reads[0], "first read must be the low byte");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE((uint16_t)(addr + 1), g_reads[1], "second read must be the high byte");
    TEST_ASSERT_EQUAL_INT_MESSAGE(g_read_tstate[0] + 3, g_read_tstate[1],
                                  "each read must see its own T-state (second read 3 T after the first)");
}


/** @test LD HL,(nn): nižší bajt z nn dřív. */
void test_ld_hl_nn_low_byte_first(void)
{
    const uint8_t code[] = { 0x2A, 0x00, 0x50 };
    step_and_check(code, 3, 0x5000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_HL));
}


/** @test LD BC,(nn) (ED 4B): nižší bajt z nn dřív. */
void test_ld_bc_nn_low_byte_first(void)
{
    const uint8_t code[] = { 0xED, 0x4B, 0x00, 0x50 };
    step_and_check(code, 4, 0x5000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_BC));
}


/** @test LD SP,(nn) (ED 7B): nižší bajt z nn dřív. */
void test_ld_sp_nn_low_byte_first(void)
{
    const uint8_t code[] = { 0xED, 0x7B, 0x00, 0x50 };
    step_and_check(code, 4, 0x5000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_SP));
}


/** @test LD IX,(nn): nižší bajt z nn dřív. */
void test_ld_ix_nn_low_byte_first(void)
{
    const uint8_t code[] = { 0xDD, 0x2A, 0x00, 0x50 };
    step_and_check(code, 4, 0x5000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_IX));
}


/** @test POP BC: nižší bajt z SP dřív. */
void test_pop_bc_low_byte_first(void)
{
    const uint8_t code[] = { 0xC1 };
    step_and_check(code, 1, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_BC));
    TEST_ASSERT_EQUAL_HEX16(0x6002, z80_get_reg(g_cpu, Z80_REG_SP));
}


/** @test POP IX: nižší bajt z SP dřív. */
void test_pop_ix_low_byte_first(void)
{
    const uint8_t code[] = { 0xDD, 0xE1 };
    step_and_check(code, 2, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_IX));
}


/** @test RET: nižší bajt návratové adresy z SP dřív. */
void test_ret_low_byte_first(void)
{
    const uint8_t code[] = { 0xC9 };
    step_and_check(code, 1, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_PC));
}


/** @test RET NZ (splněno): nižší bajt z SP dřív. */
void test_ret_cc_low_byte_first(void)
{
    const uint8_t code[] = { 0xC0 };
    z80_set_reg(g_cpu, Z80_REG_AF, 0x0000); /* Z = 0 */
    step_and_check(code, 1, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_PC));
}


/** @test RETI: nižší bajt z SP dřív. */
void test_reti_low_byte_first(void)
{
    const uint8_t code[] = { 0xED, 0x4D };
    step_and_check(code, 2, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_PC));
}


/** @test EX (SP),HL: čte nižší bajt z SP, pak vyšší z SP+1. */
void test_ex_sp_hl_low_byte_first(void)
{
    const uint8_t code[] = { 0xE3 };
    z80_set_reg(g_cpu, Z80_REG_HL, 0x1234);
    step_and_check(code, 1, 0x6000);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, z80_get_reg(g_cpu, Z80_REG_HL));
    TEST_ASSERT_EQUAL_HEX8(0x34, g_mem[0x6000]);
    TEST_ASSERT_EQUAL_HEX8(0x12, g_mem[0x6001]);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ld_hl_nn_low_byte_first);
    RUN_TEST(test_ld_bc_nn_low_byte_first);
    RUN_TEST(test_ld_sp_nn_low_byte_first);
    RUN_TEST(test_ld_ix_nn_low_byte_first);
    RUN_TEST(test_pop_bc_low_byte_first);
    RUN_TEST(test_pop_ix_low_byte_first);
    RUN_TEST(test_ret_low_byte_first);
    RUN_TEST(test_ret_cc_low_byte_first);
    RUN_TEST(test_reti_low_byte_first);
    RUN_TEST(test_ex_sp_hl_low_byte_first);
    return UNITY_END();
}
