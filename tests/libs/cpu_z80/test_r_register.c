/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_r_register.c
 * @brief Regresní testy inkrementu registru R během HALT a při přijetí INT/NMI.
 *
 * Reprodukuje regresi z v2.0.2 (výměna z80ex -> cpu-z80): jádro nezvyšovalo R
 * při opakovaném fetchi HALT ani při přijetí maskovatelného přerušení a NMI.
 * Loader hry Interkarate (ochrana "CHRANNY SYSTEM ADOLF") dešifruje kód klíčem
 * z LD A,R po HALT čekajícím na VBLN přerušení; s chybným R vyšel jiný klíč
 * a loader se zacyklil.
 *
 * Očekávané chování (báze mz800-knowledge, cpu/z80/09-r-register.md a
 * 07-interrupts.md; z80ex v2.0.1):
 * - každý opakovaný fetch HALT (po 4 T) je M1 cyklus -> R += 1,
 * - přijetí INT (IM 0/1/2) i NMI -> R += 1,
 * - inkrementuje se jen dolních 7 bitů, bit 7 se zachová.
 *
 * Testy používají reálnou MZ cestu (external_int_handling = true, z80_step
 * + z80_int/z80_nmi + z80_process_interrupt) i dávkovou cestu z80_execute.
 *
 * Osamocený prefix DD/FD (bajt za ním není instrukce s IX/IY) je jeden M1
 * cyklus: DD 00 = R + 2, DD DD 00 = R + 3 (báze cpu/z80/09-r-register.md).
 * Jádro dřív bajt za prefixem vrátilo a načetlo znovu (R + 3 u DD 00,
 * dvojí M1 čtení téhož bajtu). Prefix a instrukce jsou jeden krok CPU.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_ei_delay.c) ===== */

static uint8_t g_mem[0x10000];
static uint16_t g_m1_reads[0x10000];   /**< Počet M1 čtení podle adresy. */

static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1, void *u)
{
    (void)cpu; (void)u;
    if (m1) g_m1_reads[addr]++;
    return g_mem[addr];
}

static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t v, void *u)
{ (void)cpu; (void)u; g_mem[addr] = v; }

static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *u)
{ (void)cpu; (void)port; (void)u; return 0xFF; }

static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t v, void *u)
{ (void)cpu; (void)port; (void)v; (void)u; }

static uint8_t test_intread(struct z80_s *cpu, void *u)
{ (void)cpu; (void)u; return 0xFF; }


/* ===== setUp / tearDown ===== */

static z80_t *g_cpu;

void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_m1_reads, 0, sizeof(g_m1_reads));
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/**
 * @brief Připraví "HALT" na 0x0100, IM 1, EI stav (iff1 = iff2 = 1), R = r0.
 */
static void prepare_halt(uint8_t r0)
{
    g_mem[0x0100] = 0x76;   /* HALT */
    g_cpu->pc  = 0x0100;
    g_cpu->sp  = 0x8000;
    g_cpu->im  = 1;
    g_cpu->iff1 = 1;
    g_cpu->iff2 = 1;
    g_cpu->r   = r0;
}


/* ===== Testy ===== */

/**
 * @test HALT: fetch HALT + každé opakování (4 T) zvýší R o 1 (per-step cesta).
 */
void test_halt_refetch_increments_r_step(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x10);

    z80_step(g_cpu);                      /* fetch HALT: R = 0x11 */
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX8(0x11, g_cpu->r);

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(4, z80_step(g_cpu));
    }
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX8(0x11 + 10, g_cpu->r);
}

/**
 * @test HALT: dávková cesta z80_execute - R roste o 1 za každé 4 T v HALT.
 */
void test_halt_refetch_increments_r_batch(void)
{
    g_cpu->external_int_handling = true;  /* bez INT, jen čekání v HALT */
    prepare_halt(0x20);

    int cycles = 0;
    while (cycles < 4 + 40) {
        cycles += z80_execute(g_cpu, 4);
    }
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_INT(44, cycles);
    TEST_ASSERT_EQUAL_HEX8(0x20 + 1 + 10, g_cpu->r);
}

/**
 * @test HALT: inkrement přeteče jen v dolních 7 bitech, bit 7 zůstane.
 */
void test_halt_refetch_keeps_bit7(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0xFE);

    z80_step(g_cpu);                      /* HALT: 0xFE -> 0xFF */
    z80_step(g_cpu);                      /* 0xFF -> 0x80 (bit 7 drží) */
    TEST_ASSERT_EQUAL_HEX8(0x80, g_cpu->r);
}

/**
 * @test Přijetí INT (IM 1) z HALT zvýší R o 1 navíc k opakovaným fetchům.
 */
void test_int_ack_increments_r(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x30);

    z80_step(g_cpu);                      /* HALT: R = 0x31 */
    z80_step(g_cpu);                      /* opakování: R = 0x32 */
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_FALSE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX16(0x0038, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x33, g_cpu->r);
}

/**
 * @test Přijetí INT mimo HALT (IM 2) zvýší R o 1.
 */
void test_int_ack_im2_increments_r(void)
{
    g_cpu->external_int_handling = true;
    g_mem[0x0200] = 0x00;                 /* NOP */
    /* intread vrací vektor 0FFh; Z80 bit 0 nemaskuje, záznam je 40FFh. */
    g_mem[0x40FF] = 0x00; g_mem[0x4100] = 0x50;
    g_cpu->pc = 0x0200; g_cpu->sp = 0x8000;
    g_cpu->im = 2; g_cpu->i = 0x40;
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    g_cpu->r = 0x45;

    z80_step(g_cpu);                      /* NOP: R = 0x46 */
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x5000, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x47, g_cpu->r);
}

/**
 * @test Přijetí NMI zvýší R o 1.
 */
void test_nmi_ack_increments_r(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x7F);

    z80_step(g_cpu);                      /* HALT: 0x7F -> 0x00 */
    z80_nmi(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x0066, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_cpu->r);
}

/**
 * @test Interní zpracování přerušení (batch, external_int_handling = false)
 *       zvýší R při přijetí INT o 1 (z80_execute pak vykoná ještě NOP
 *       na 0038h, ten přidá další 1).
 */
void test_int_ack_internal_handling_increments_r(void)
{
    g_cpu->external_int_handling = false;
    g_mem[0x0200] = 0x00;                 /* NOP */
    g_cpu->pc = 0x0200; g_cpu->sp = 0x8000;
    g_cpu->im = 1;
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    g_cpu->r = 0x10;

    z80_int(g_cpu);
    z80_execute(g_cpu, 1);                /* vstupní check přijme INT, pak NOP na 0038h */
    TEST_ASSERT_EQUAL_HEX16(0x0039, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x12, g_cpu->r);   /* +1 INT ack, +1 NOP */
}


/* ===== Osamocený prefix DD/FD ===== */

/**
 * @brief Jeden krok per-step (z80_step) nebo dávkové varianty (z80_execute, cíl 1 T).
 */
static int run_one(bool batch)
{
    return batch ? z80_execute(g_cpu, 1) : z80_step(g_cpu);
}

/**
 * @brief Uloží bajty programu na 0x0100, nastaví PC, R a vynuluje čítače M1.
 */
static void put_prog(const uint8_t *bytes, size_t n, uint8_t r0)
{
    memcpy(&g_mem[0x0100], bytes, n);
    memset(g_m1_reads, 0, sizeof(g_m1_reads));
    g_cpu->external_int_handling = true;
    g_cpu->pc = 0x0100;
    g_cpu->sp = 0x8000;
    g_cpu->r = r0;
}

/**
 * @test DD 00: prefix + NOP v jednom kroku, 8 T, R + 2, bajt 00h načtený
 *       jedním M1 cyklem.
 */
void test_stray_dd_nop(void)
{
    static const uint8_t prog[] = { 0xDD, 0x00 };
    for (int batch = 0; batch < 2; batch++) {
        put_prog(prog, sizeof(prog), 0x10);
        TEST_ASSERT_EQUAL_INT(8, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0x0102, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x12, g_cpu->r);
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0100]);
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0101]);
    }
}

/**
 * @test DD DD 00 a FD 3E 55 (LD A,n za ignorovaným prefixem): R podle počtu
 *       M1 cyklů, každý bajt načtený jednou, jeden krok.
 */
void test_stray_prefix_chain(void)
{
    static const uint8_t prog1[] = { 0xDD, 0xDD, 0x00 };
    static const uint8_t prog2[] = { 0xFD, 0x3E, 0x55 };
    for (int batch = 0; batch < 2; batch++) {
        put_prog(prog1, sizeof(prog1), 0x7E);
        TEST_ASSERT_EQUAL_INT(12, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0x0103, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x01, g_cpu->r);      /* 7Eh + 3, bit 7 drží 0 */
        for (uint16_t a = 0x0100; a < 0x0103; a++) {
            TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[a]);
        }

        put_prog(prog2, sizeof(prog2), 0x80);
        TEST_ASSERT_EQUAL_INT(11, run_one(batch));   /* 4 + LD A,n 7 */
        TEST_ASSERT_EQUAL_HEX16(0x0103, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x55, g_cpu->af.h);
        TEST_ASSERT_EQUAL_HEX8(0x82, g_cpu->r);      /* bit 7 drží 1 */
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0101]);
        TEST_ASSERT_EQUAL_UINT16(0, g_m1_reads[0x0102]);  /* operand, ne M1 */
    }
}

/**
 * @test FD DD 21 34 12: platí poslední prefix, LD IX,1234h; IY beze změny,
 *       R + 3, 4 + 14 T.
 */
void test_stray_prefix_last_wins(void)
{
    static const uint8_t prog[] = { 0xFD, 0xDD, 0x21, 0x34, 0x12 };
    for (int batch = 0; batch < 2; batch++) {
        put_prog(prog, sizeof(prog), 0x20);
        g_cpu->ix.w = 0x0000;
        g_cpu->iy.w = 0xBEEF;
        TEST_ASSERT_EQUAL_INT(18, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0x0105, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX16(0x1234, g_cpu->ix.w);
        TEST_ASSERT_EQUAL_HEX16(0xBEEF, g_cpu->iy.w);
        TEST_ASSERT_EQUAL_HEX8(0x23, g_cpu->r);
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0101]);
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0102]);
    }
}

/**
 * @test DD ED 44 (NEG): ED prefix DD ruší, R + 3, 4 + 8 T.
 */
void test_stray_prefix_before_ed(void)
{
    static const uint8_t prog[] = { 0xDD, 0xED, 0x44 };
    for (int batch = 0; batch < 2; batch++) {
        put_prog(prog, sizeof(prog), 0x00);
        g_cpu->af.h = 0x01;
        TEST_ASSERT_EQUAL_INT(12, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0x0103, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0xFF, g_cpu->af.h);
        TEST_ASSERT_EQUAL_HEX8(0x03, g_cpu->r);
        TEST_ASSERT_EQUAL_UINT16(1, g_m1_reads[0x0101]);
    }
}

/**
 * @test Přerušení se mezi osamoceným prefixem a instrukcí nepřijme: INT
 *       čekající před krokem DD 00 se přijme až za NOP (návratová adresa
 *       na zásobníku je 0102h).
 */
void test_stray_prefix_no_int_between(void)
{
    static const uint8_t prog[] = { 0xDD, 0x00, 0x00 };
    put_prog(prog, sizeof(prog), 0x00);
    g_cpu->im = 1;
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    TEST_ASSERT_EQUAL_INT(8, z80_step(g_cpu));
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x0038, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x02, g_mem[0x7FFE]);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_mem[0x7FFF]);
}

/**
 * @test Dlouhý řetěz: 100 x DD a NOP. Krok pojme nejvýš 64 prefixů, pak se
 *       rozdělí; R a T-stavy celkem odpovídají 101 M1 cyklům (R se při
 *       rozdělení nezdvojí), řetěz skončí za NOP.
 */
void test_stray_prefix_long_chain_splits(void)
{
    uint8_t prog[101];
    memset(prog, 0xDD, 100);
    prog[100] = 0x00;
    for (int batch = 0; batch < 2; batch++) {
        put_prog(prog, sizeof(prog), 0x00);
        int steps = 0, cycles = 0;
        while (g_cpu->pc != 0x0100 + 101 && steps < 10) {
            cycles += run_one(batch);
            steps++;
        }
        TEST_ASSERT_EQUAL_HEX16(0x0100 + 101, g_cpu->pc);
        TEST_ASSERT_EQUAL_INT(2, steps);
        TEST_ASSERT_EQUAL_INT(101 * 4, cycles);
        TEST_ASSERT_EQUAL_HEX8(101 & 0x7F, g_cpu->r);
    }
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_halt_refetch_increments_r_step);
    RUN_TEST(test_halt_refetch_increments_r_batch);
    RUN_TEST(test_halt_refetch_keeps_bit7);
    RUN_TEST(test_int_ack_increments_r);
    RUN_TEST(test_int_ack_im2_increments_r);
    RUN_TEST(test_nmi_ack_increments_r);
    RUN_TEST(test_int_ack_internal_handling_increments_r);
    RUN_TEST(test_stray_dd_nop);
    RUN_TEST(test_stray_prefix_chain);
    RUN_TEST(test_stray_prefix_last_wins);
    RUN_TEST(test_stray_prefix_before_ed);
    RUN_TEST(test_stray_prefix_no_int_between);
    RUN_TEST(test_stray_prefix_long_chain_splits);
    return UNITY_END();
}
