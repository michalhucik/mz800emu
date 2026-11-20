/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_q_register.c
 * @brief Testy interního registru Q procesoru Z80 (YF/XF po SCF a CCF).
 *
 * SCF a CCF nastavují nedokumentované příznaky YF (bit 5) a XF (bit 3) podle
 *
 *     YF/XF = ((Q xor F) or A) and 28h
 *
 * kde Q = F po instrukci, která příznaky zapisuje (i když se hodnota F
 * nezměnila), a Q = 0 po instrukci, která je nezapisuje (včetně POP AF
 * a EX AF,AF') a po přijetí přerušení. Po instrukci se zápisem příznaků se
 * tedy YF/XF berou jen z A, jinak z A or F.
 *
 * Zdroj vzorce: redcode/Z80 (sources/Z80.c), MAME (z80.lst). Čtyři základní
 * sekvence tabulky (s LD D,D / LD E,E místo NOP) dávají na skutečném MZ-800
 * 64 shod ze 64 (program syscheck, měřeno 2026-11-20).
 *
 * Sekvence začínají XOR A; CP 28h: A = 0 a F má YF/XF = 1. Jen tahle
 * kombinace odliší "jen z A" (00h) od "z A or F" (28h).
 *
 * Každá sekvence běží třikrát: krokováním (z80_step, cesta MZ emulace),
 * jednou dávkou (z80_execute) a s hranicí volání těsně před poslední
 * instrukcí. Q musí hranici volání přežít, výsledky musí být shodné.
 */

#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/** Maska nedokumentovaných příznaků YF (bit 5) a XF (bit 3). */
#define Q_YX ( Z80_FLAG_5 | Z80_FLAG_3 )


/* ===== Test paměť + IO simulace (vzor test_r_register.c) ===== */

static uint8_t g_mem[0x10000];

static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1, void *u)
{ (void)cpu; (void)m1; (void)u; return g_mem[addr]; }

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

/**
 * @brief Vytvoří čistou instanci CPU nad vynulovanou RAM (samé NOP).
 * @post PC = 0000h, SP = F000h, přerušení obsluhuje volající
 *       (external_int_handling = true, jako MZ emulace).
 */
static void make_cpu(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
    g_cpu->external_int_handling = true;
    g_cpu->pc = 0x0000;
    g_cpu->sp = 0xF000;
}

void setUp(void)
{
    make_cpu();
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/* ===== Tabulka sekvencí ===== */

/**
 * @brief Jedna sekvence instrukcí od adresy 0000h.
 *
 * Invariant: tstates je součet T-stavů všech instrukcí sekvence a poslední
 * instrukce (SCF nebo CCF, případně s prefixem) trvá aspoň 4 T.
 */
typedef struct {
    const char *name;      /**< Čitelný zápis sekvence. */
    uint8_t code[8];       /**< Strojový kód, nevyužité bajty jsou NOP. */
    int instructions;      /**< Počet kroků z80_step() na celou sekvenci. */
    int tstates;           /**< Součet T-stavů sekvence (pro dávku). */
    uint8_t expected_yx;   /**< Očekávané F and 28h. */
} st_Q_CASE;

static const st_Q_CASE s_q_cases[] = {
    { "XOR A; CP 28h; SCF",              { 0xAF, 0xFE, 0x28, 0x37 },             3, 15, 0x00 },
    { "XOR A; CP 28h; NOP; SCF",         { 0xAF, 0xFE, 0x28, 0x00, 0x37 },       4, 19, Q_YX },
    { "XOR A; CP 28h; CCF",              { 0xAF, 0xFE, 0x28, 0x3F },             3, 15, 0x00 },
    { "XOR A; CP 28h; NOP; CCF",         { 0xAF, 0xFE, 0x28, 0x00, 0x3F },       4, 19, Q_YX },
    /* Druhé CP zapíše do F stejnou hodnotu: Q se přesto musí nastavit. */
    { "XOR A; CP 28h; CP 28h; SCF",      { 0xAF, 0xFE, 0x28, 0xFE, 0x28, 0x37 }, 4, 22, 0x00 },
    /* První SCF/CCF vidí Q = 0 (po NOP) a nastaví YF/XF, druhé vidí Q = F. */
    { "XOR A; CP 28h; NOP; SCF; SCF",    { 0xAF, 0xFE, 0x28, 0x00, 0x37, 0x37 }, 5, 23, 0x00 },
    { "XOR A; CP 28h; NOP; CCF; CCF",    { 0xAF, 0xFE, 0x28, 0x00, 0x3F, 0x3F }, 5, 23, 0x00 },
    /* Osamocený prefix DD nuluje Q; chování převzaté z dřívější implementace,
     * na reálném procesoru neověřeno. */
    { "XOR A; CP 28h; DD SCF",           { 0xAF, 0xFE, 0x28, 0xDD, 0x37 },       3, 19, Q_YX },
};

/** Způsob průchodu sekvencí. */
typedef enum {
    Q_RUN_STEP,      /**< Každá instrukce vlastním voláním z80_step(). */
    Q_RUN_BATCH,     /**< Celá sekvence jedním voláním z80_execute(). */
    Q_RUN_BOUNDARY,  /**< Dávka bez posledních 4 T, pak jeden z80_step(). */
} en_Q_RUN;

/**
 * @brief Provede všechny sekvence tabulky zvoleným způsobem a ověří YF/XF.
 * @param mode Způsob průchodu.
 * @post g_cpu je čerstvá instance po poslední sekvenci (uvolní ji tearDown).
 */
static void check_q_cases(en_Q_RUN mode)
{
    for (size_t i = 0; i < sizeof(s_q_cases) / sizeof(s_q_cases[0]); i++) {
        const st_Q_CASE *c = &s_q_cases[i];
        char msg[80];
        snprintf(msg, sizeof(msg), "%s (mode %d)", c->name, (int)mode);

        z80_destroy(g_cpu);
        make_cpu();
        memcpy(g_mem, c->code, sizeof(c->code));

        switch (mode) {
            case Q_RUN_STEP:
                for (int n = 0; n < c->instructions; n++) z80_step(g_cpu);
                break;
            case Q_RUN_BATCH:
                z80_execute(g_cpu, c->tstates);
                break;
            case Q_RUN_BOUNDARY:
                z80_execute(g_cpu, c->tstates - 4);
                z80_step(g_cpu);
                break;
        }
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(c->expected_yx, g_cpu->af.l & Q_YX, msg);
    }
}

/** @test Tabulka sekvencí, krokování (per-step jádro). */
void test_q_sequences_step(void)     { check_q_cases(Q_RUN_STEP); }

/** @test Tabulka sekvencí, jedna dávka (dávkové jádro). */
void test_q_sequences_batch(void)    { check_q_cases(Q_RUN_BATCH); }

/** @test Tabulka sekvencí, hranice volání před poslední instrukcí. */
void test_q_sequences_boundary(void) { check_q_cases(Q_RUN_BOUNDARY); }


/* ===== Jednotlivé případy ===== */

/** @test SCF bere YF/XF z A i po instrukci se zápisem příznaků. */
void test_scf_takes_yx_from_a(void)
{
    g_cpu->af.h = 0x28;
    g_mem[0] = 0xB7; g_mem[1] = 0x37;         /* OR A; SCF */
    z80_step(g_cpu); z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
    TEST_ASSERT_TRUE(g_cpu->af.l & Z80_FLAG_C);
}

/** @test SCF s A = 0 a F = 0 nechá YF/XF nulové. */
void test_scf_zero_a_zero_f(void)
{
    g_cpu->af.w = 0x0000;
    g_mem[0] = 0x00; g_mem[1] = 0x37;         /* NOP; SCF */
    z80_step(g_cpu); z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_cpu->af.l & Q_YX);
}

/** @test Po POP AF je Q = 0: SCF bere YF/XF z A or F. */
void test_scf_after_pop_af(void)
{
    g_cpu->sp = 0xFFF0;
    g_mem[0xFFF0] = 0x28; g_mem[0xFFF1] = 0x00;   /* F = 28h, A = 00h */
    g_mem[0] = 0xF1; g_mem[1] = 0x37;         /* POP AF; SCF */
    z80_step(g_cpu); z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_cpu->af.h);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/** @test Po EX AF,AF' je Q = 0: SCF bere YF/XF z A or F. */
void test_scf_after_ex_af(void)
{
    g_cpu->af.w = 0x0000;
    g_cpu->af2.w = 0x0028;
    g_mem[0] = 0x08; g_mem[1] = 0x37;         /* EX AF,AF'; SCF */
    z80_step(g_cpu); z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_cpu->af.h);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/**
 * @test Přijetí INT nuluje Q, cesta MZ emulace (external_int_handling = true,
 *       z80_process_interrupt). Bez nulování by SCF na 0038h vidělo Q = F
 *       po CP a vzalo YF/XF jen z A.
 */
void test_int_ack_clears_q_external(void)
{
    g_cpu->im = 1;
    g_mem[0] = 0xAF; g_mem[1] = 0xFE; g_mem[2] = 0x28;   /* XOR A; CP 28h */
    g_mem[0x0038] = 0x37;                     /* SCF jako první instrukce obsluhy */
    z80_step(g_cpu); z80_step(g_cpu);
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x0038, g_cpu->pc);
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/**
 * @test Přijetí INT nuluje Q, interní zpracování přerušení v dávkovém jádře
 *       (external_int_handling = false).
 */
void test_int_ack_clears_q_internal(void)
{
    g_cpu->external_int_handling = false;
    g_cpu->im = 1;
    g_mem[0] = 0xAF; g_mem[1] = 0xFE; g_mem[2] = 0x28;   /* XOR A; CP 28h */
    g_mem[0x0038] = 0x37;                     /* SCF */
    z80_execute(g_cpu, 11);                   /* XOR A (4 T) + CP 28h (7 T) */
    TEST_ASSERT_EQUAL_HEX16(0x0003, g_cpu->pc);
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    z80_int(g_cpu);
    z80_execute(g_cpu, 1);                    /* vstupní check přijme INT, pak SCF */
    TEST_ASSERT_EQUAL_HEX16(0x0039, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/** @test Přijetí NMI nuluje Q. */
void test_nmi_ack_clears_q(void)
{
    g_mem[0] = 0xAF; g_mem[1] = 0xFE; g_mem[2] = 0x28;   /* XOR A; CP 28h */
    g_mem[0x0066] = 0x37;                     /* SCF jako první instrukce obsluhy */
    z80_step(g_cpu); z80_step(g_cpu);
    z80_nmi(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x0066, g_cpu->pc);
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/** @test Zápis AF přes z80_set_reg() nuluje Q (jako POP AF). */
void test_set_reg_af_clears_q(void)
{
    g_mem[0] = 0xAF; g_mem[1] = 0xFE; g_mem[2] = 0x28;   /* XOR A; CP 28h */
    g_mem[3] = 0x37;                          /* SCF */
    z80_step(g_cpu); z80_step(g_cpu);
    z80_set_reg(g_cpu, Z80_REG_AF, 0x0028);
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->af.l & Q_YX);
}

/**
 * @test Hodnota cpu->q po instrukci: F po zápisu příznaků, 0 jinak.
 *       cpu->q čte debugger, snapshot i otisk stavu stroje.
 */
void test_q_value_visible_in_cpu(void)
{
    g_mem[0] = 0xAF; g_mem[1] = 0xFE; g_mem[2] = 0x28;   /* XOR A; CP 28h */
    g_mem[3] = 0x00;                          /* NOP */
    z80_step(g_cpu); z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(g_cpu->af.l, g_cpu->q);
    TEST_ASSERT_EQUAL_HEX8(Q_YX, g_cpu->q & Q_YX);
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_cpu->q);
}


/* ===== Runner ===== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_q_sequences_step);
    RUN_TEST(test_q_sequences_batch);
    RUN_TEST(test_q_sequences_boundary);
    RUN_TEST(test_scf_takes_yx_from_a);
    RUN_TEST(test_scf_zero_a_zero_f);
    RUN_TEST(test_scf_after_pop_af);
    RUN_TEST(test_scf_after_ex_af);
    RUN_TEST(test_int_ack_clears_q_external);
    RUN_TEST(test_int_ack_clears_q_internal);
    RUN_TEST(test_nmi_ack_clears_q);
    RUN_TEST(test_set_reg_af_clears_q);
    RUN_TEST(test_q_value_visible_in_cpu);
    return UNITY_END();
}
