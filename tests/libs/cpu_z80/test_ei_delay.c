/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_ei_delay.c
 * @brief Regresní testy pro EI-delay v per-step variantě jádra (gun-runner).
 *
 * Reprodukuje regresi z v2.0.2: nové jádro přijímalo maskovatelné přerušení
 * IHNED po instrukci EI místo až po NÁSLEDUJÍCÍ instrukci (EI-delay). U sekvence
 * "EI; LD SP,nn" se kvůli tomu interrupt přijal před přepnutím zásobníku a push
 * návratové adresy šel na STARÝ (neplatný) zásobník -> poškození paměti
 * (hra Gun Runner se zasekla).
 *
 * Tyto testy modelují reálnou MZ cestu: cpu->external_int_handling = true
 * (z80_step interrupty NEřeší interně) + RUČNÍ "mzarch poll" mezi z80_step
 * voláními, který napodobuje mzarch_main_process_interrupt
 * ("iff1 && !ei_delay" -> z80_int + z80_process_interrupt).
 *
 * POZN: poll po KAŽDÉM kroku je zesílený model (reálný mzarch.c polluje
 * event-gated, ne striktně per-instrukci); záměr je přesně připnout
 * jednoinstrukční okno EI-delay. Nezávisí na ZEXALL (ten interrupt timing
 * netestuje).
 *
 * ZEXALL netestuje načasování přijetí přerušení, proto je tento test nutný.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_iff_change.c) ===== */

static uint8_t  g_mem[0x10000];
static uint8_t  g_int_vector;   /**< Vektor pro IM 2 (přes intread, int_vector==0). */

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

static void reset_mem(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_int_vector = 0xFF;
}

void setUp(void)
{
    reset_mem();
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
    /* Reálná MZ cesta: jádro interrupty interně neřeší, polluje je mzarch. */
    g_cpu->external_int_handling = true;
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/**
 * @brief Napodobuje mzarch_main_process_interrupt: přijme maskovatelný INT jen
 *        pokud iff1 && !ei_delay. Vrací true, pokud byl INT skutečně přijat.
 *
 * Pro IM 2 dodá vektor přes intread (g_int_vector), protože z80_int nastaví
 * int_vector=0 -> jádro si vektor přečte callbackem.
 */
static bool mzarch_poll(void)
{
    if (g_cpu->iff1 && !g_cpu->ei_delay) {
        z80_int(g_cpu);
        return z80_process_interrupt(g_cpu) > 0;
    }
    return false;
}


/* ===== Testy ===== */

/**
 * @test HLAVNÍ regrese (Gun Runner): "IN A,(0E1h); EI; LD SP,(0EC98h); RET"
 *       s pending IM2 a external_int_handling. INT se SMÍ přijmout až po LD SP,
 *       takže push jde na NOVÝ zásobník (0x9000), ne na starý (0xC000).
 *
 *       PŘED fixem: poll po EI by INT přijal (ei_delay smazán epilogem) -> push
 *       na starý SP 0xC000 -> asserty selžou (= test chytá regresi).
 */
void test_regression_ei_ldsp_pushes_to_new_stack(void)
{
    /* 0x0000: DB E1        IN A,(0E1h)
     * 0x0002: FB           EI
     * 0x0003: ED 7B 98 EC  LD SP,(0EC98h)
     * 0x0007: C9           RET */
    g_mem[0x0000] = 0xDB; g_mem[0x0001] = 0xE1;
    g_mem[0x0002] = 0xFB;
    g_mem[0x0003] = 0xED; g_mem[0x0004] = 0x7B; g_mem[0x0005] = 0x98; g_mem[0x0006] = 0xEC;
    g_mem[0x0007] = 0xC9;

    g_mem[0xEC98] = 0x00; g_mem[0xEC99] = 0x90;   /* nový SP = 0x9000 */

    /* IM 2: I=0x40, vektor 0x80 -> vec_addr (0x40<<8)|0x80=0x4080 */
    g_mem[0x4080] = 0x00; g_mem[0x4081] = 0x50;   /* ISR handler @ 0x5000 */
    g_int_vector = 0x80;

    g_cpu->pc = 0x0000;
    g_cpu->sp = 0xC000;          /* starý zásobník */
    g_cpu->im = 2;
    g_cpu->i  = 0x40;

    /* IN A,(0E1h) - iff1 ještě 0, žádný INT */
    z80_step(g_cpu);
    TEST_ASSERT_FALSE(mzarch_poll());

    /* EI - povolí INT, nastaví ei_delay. INT se NESMÍ přijmout. */
    z80_step(g_cpu);
    TEST_ASSERT_TRUE(g_cpu->ei_delay);                 /* PŘED fixem: FALSE */
    TEST_ASSERT_FALSE(mzarch_poll());                  /* PŘED fixem: TRUE (chybný accept) */
    TEST_ASSERT_EQUAL_UINT16(0xC000, g_cpu->sp);       /* žádný push */

    /* LD SP,(0EC98h) - nastaví nový zásobník, ei_delay se na vstupu kroku smaže */
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_UINT16(0x9000, g_cpu->sp);
    TEST_ASSERT_FALSE(g_cpu->ei_delay);

    /* Teprve teď se INT přijme - push na NOVÝ zásobník */
    TEST_ASSERT_TRUE(mzarch_poll());
    TEST_ASSERT_EQUAL_UINT16(0x5000, g_cpu->pc);       /* skok do ISR */
    TEST_ASSERT_EQUAL_UINT16(0x9000 - 2, g_cpu->sp);   /* push na 0x9000, NE 0xC000 */
    /* návratová adresa (= adresa RET 0x0007) uložena na novém stacku */
    TEST_ASSERT_EQUAL_UINT8(0x07, g_mem[0x8FFE]);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_mem[0x8FFF]);
}


/**
 * @test Přímý invariant (model-nezávislý, nejsilnější stráž): po EI drží
 *       ei_delay, po následující instrukci je smazán.
 *       PŘED fixem: po EI je ei_delay v per-step už FALSE.
 */
void test_invariant_ei_delay_lifetime(void)
{
    g_mem[0x0000] = 0xFB; /* EI */
    g_mem[0x0001] = 0x00; /* NOP */
    g_cpu->pc = 0x0000;

    z80_step(g_cpu); /* EI */
    TEST_ASSERT_TRUE(g_cpu->ei_delay);
    z80_step(g_cpu); /* NOP */
    TEST_ASSERT_FALSE(g_cpu->ei_delay);
}


/**
 * @test Řetězení EI;EI: delay drží přes obě EI, smaže se až po instrukci po nich.
 */
void test_ei_ei_chain(void)
{
    g_mem[0x0000] = 0xFB; /* EI */
    g_mem[0x0001] = 0xFB; /* EI */
    g_mem[0x0002] = 0x00; /* NOP */
    g_cpu->pc = 0x0000;

    z80_step(g_cpu); /* EI */
    TEST_ASSERT_TRUE(g_cpu->ei_delay);
    z80_step(g_cpu); /* EI */
    TEST_ASSERT_TRUE(g_cpu->ei_delay);
    z80_step(g_cpu); /* NOP */
    TEST_ASSERT_FALSE(g_cpu->ei_delay);
}


/**
 * @test EI;HALT: po EI se INT nepřijme; HALT (instrukce po EI) se vykoná a
 *       CPU se zastaví; teprve pak poll INT přijme a probudí HALT.
 */
void test_ei_halt(void)
{
    g_mem[0x0000] = 0xFB; /* EI */
    g_mem[0x0001] = 0x76; /* HALT */
    g_mem[0x0038] = 0x76; /* IM1 handler = HALT (PC zůstane na 0x0038) */
    g_cpu->pc = 0x0000;
    g_cpu->sp = 0xFFFE;
    g_cpu->im = 1;

    z80_step(g_cpu); /* EI */
    TEST_ASSERT_TRUE(g_cpu->ei_delay);
    TEST_ASSERT_FALSE(mzarch_poll());          /* INT deferován */

    z80_step(g_cpu); /* HALT */
    TEST_ASSERT_TRUE(z80_is_halted(g_cpu));
    TEST_ASSERT_FALSE(g_cpu->ei_delay);

    TEST_ASSERT_TRUE(mzarch_poll());           /* probuzení z HALT */
    TEST_ASSERT_EQUAL_UINT16(0x0038, g_cpu->pc);
}


/**
 * @test NEGATIVNÍ batch protitest (záměr): ve veřejné batch variantě
 *       (z80_execute) si epilog clear ei_delay PONECHÁVÁ, takže po EI uvnitř
 *       dávky je ei_delay false. Dokumentuje, že fix je čistě per-step a batch
 *       je beze změny.
 */
void test_batch_keeps_epilog_clear(void)
{
    g_cpu->external_int_handling = false; /* batch interní cesta */
    g_mem[0x0000] = 0xFB; /* EI */
    g_mem[0x0001] = 0x00; /* NOP */
    g_cpu->pc = 0x0000;

    z80_execute(g_cpu, 4); /* batch: vykoná jen EI (4 T) */
    TEST_ASSERT_FALSE(g_cpu->ei_delay);
}


/**
 * @test Interní cesta (external_int_handling=false) per-step: lifecycle ei_delay
 *       je stejný jako u externí -> existující interní INT cesta není regresí
 *       dotčena.
 */
void test_internal_per_step_lifetime(void)
{
    g_cpu->external_int_handling = false;
    g_mem[0x0000] = 0xFB; /* EI */
    g_mem[0x0001] = 0x00; /* NOP */
    g_cpu->pc = 0x0000;

    z80_step(g_cpu); /* EI */
    TEST_ASSERT_TRUE(g_cpu->ei_delay);
    z80_step(g_cpu); /* NOP */
    TEST_ASSERT_FALSE(g_cpu->ei_delay);
}


/* ===== Unity main ===== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_regression_ei_ldsp_pushes_to_new_stack);
    RUN_TEST(test_invariant_ei_delay_lifetime);
    RUN_TEST(test_ei_ei_chain);
    RUN_TEST(test_ei_halt);
    RUN_TEST(test_batch_keeps_epilog_clear);
    RUN_TEST(test_internal_per_step_lifetime);
    return UNITY_END();
}
