/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * @file test_mhmap_resolve.c
 * @brief Testy resolveru CDL / Memory Heatmap pro horní paměť (stránky 0Eh, 0Fh).
 *
 * Resolver @c mhmap_resolve_mem() rozhoduje, do kterého fyzického regionu se
 * zaznamená přístup na danou sběrnicovou adresu. Musí odpovídat tomu, odkud
 * CPU v témž stavu skutečně čte - jinak heatmapa ukáže přístup, který se
 * fyzicky nestal.
 *
 * Kritická je shoda pořadí testů příznaků s čtecí cestou v
 * @c mz800_memory.c. Čtecí makra @c memory_internal_read_e000_efff a
 * @c memory_internal_read_f000_ffff testují @c PROHIBITED jako PRVNÍ
 * podmínku - resolver proto musí dělat totéž. Jinak vzniká rozpor ve stavu
 * "Prohibited aktivní, ROM_E000 neaktivní": CPU vrátí poslední bajt na sběrnici a z
 * paměti RAM nečte nic, ale resolver by přístup zaznamenal do regionu RAM.
 *
 * Pokrytí:
 *   - prohibited_without_rom_e000: Prohibited bez ROM_E000 = SKIP (regrese)
 *   - prohibited_with_rom_e000: Prohibited s ROM_E000 = SKIP
 *   - rom_e000_without_prohibited: běžné mapování ROM = DIRECT do ROM_UPPER
 *   - no_rom_no_prohibited: čistá RAM = DIRECT do RAM
 *   - prohibited_covers_mapped_ports: i adresy mapovaných portů = SKIP
 *
 * Test framework je MZARCH=800 only (viz cmake/AddMzTest.cmake).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "hw-generic/memory/memory.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#include "debugger/mhmap.h"


/* setUp: deterministický výchozí stav - oba příznaky shozené. */
void setUp(void)
{
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_E000;
}


void tearDown(void)
{
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    g_memory.map &= ~MEMORY_MZ800_MAP_FLAG_ROM_E000;
}


/*
 * Regresní test.
 *
 * Ve stavu "Prohibited aktivní, ROM_E000 neaktivní" čte CPU poslední bajt na sběrnici
 * (viz memory_internal_read_e000_efff - PROHIBITED je první podmínka) a k
 * paměti RAM se vůbec nedostane. Resolver proto nesmí hlásit fyzický cíl.
 *
 * Před opravou pořadí podmínek v mhmap_resolve_mem() tento test selhal:
 * řízení propadlo na závěrečné MH_RESOLVE_RETURN_RAM a vrátilo DIRECT do
 * regionu RAM.
 */
void test_mhmap_prohibited_without_rom_e000(void)
{
    en_MHMAP_REGION region = MHMAP_REGION_RAM;
    unsigned offset = 0;

    g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    /* ROM_E000 zůstává shozený (viz setUp). */

    /* Stránka 0Eh. */
    en_MHMAP_RESOLVE_RESULT r = mhmap_resolve_mem(0xE100, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);

    /* Stránka 0Fh. */
    r = mhmap_resolve_mem(0xF200, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);
}


/*
 * Prohibited se zároveň mapovanou horní ROM. Tuhle konfiguraci resolver
 * ošetřoval správně i před opravou - test hlídá, že se nerozbila.
 */
void test_mhmap_prohibited_with_rom_e000(void)
{
    en_MHMAP_REGION region = MHMAP_REGION_RAM;
    unsigned offset = 0;

    g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;

    en_MHMAP_RESOLVE_RESULT r = mhmap_resolve_mem(0xE100, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);

    r = mhmap_resolve_mem(0xF200, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);
}


/*
 * Prohibited zastiňuje CELÉ $E000-$FFFF včetně oblasti mapovaných portů
 * $E000-$E008. Empiricky ověřeno na reálném stroji (banking-e800 v0.5,
 * test T4: $E001 nevrací klávesnicový scan, ale poslední bajt na sběrnici).
 */
void test_mhmap_prohibited_covers_mapped_ports(void)
{
    en_MHMAP_REGION region = MHMAP_REGION_RAM;
    unsigned offset = 0;

    g_memory.map |= MEMORY_MZ800_MAP_FLAG_PROHIBITED;
    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;

    /* $E001 = PIO port B, $E008 = GDG DMD status. */
    en_MHMAP_RESOLVE_RESULT r = mhmap_resolve_mem(0xE001, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);

    r = mhmap_resolve_mem(0xE008, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_SKIP, r);
}


/*
 * Běžné mapování horní ROM bez Prohibited: resolver má vrátit fyzický cíl
 * v regionu ROM_UPPER. Kontrola, že oprava nerozbila hlavní cestu.
 */
void test_mhmap_rom_e000_without_prohibited(void)
{
    en_MHMAP_REGION region = MHMAP_REGION_RAM;
    unsigned offset = 0;

    g_memory.map |= MEMORY_MZ800_MAP_FLAG_ROM_E000;
    /* PROHIBITED zůstává shozený. */

    /* Adresa mimo oblast mapovaných portů, aby test nezávisel na režimu DMD. */
    en_MHMAP_RESOLVE_RESULT r = mhmap_resolve_mem(0xF200, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_DIRECT, r);
    TEST_ASSERT_EQUAL_INT(MHMAP_REGION_ROM_UPPER, region);
    TEST_ASSERT_EQUAL_UINT(0xF200 - 0xE000, offset);
}


/*
 * Bez obou příznaků je v horní oblasti vidět RAM. Kontrola, že se oprava
 * nedotkla výchozí cesty.
 */
void test_mhmap_no_rom_no_prohibited(void)
{
    en_MHMAP_REGION region = MHMAP_REGION_ROM_UPPER;
    unsigned offset = 0;

    en_MHMAP_RESOLVE_RESULT r = mhmap_resolve_mem(0xE100, &region, &offset);
    TEST_ASSERT_EQUAL_INT(MHMAP_RESOLVE_DIRECT, r);
    TEST_ASSERT_EQUAL_INT(MHMAP_REGION_RAM, region);
    TEST_ASSERT_EQUAL_UINT(0xE100, offset);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mhmap_prohibited_without_rom_e000);
    RUN_TEST(test_mhmap_prohibited_with_rom_e000);
    RUN_TEST(test_mhmap_prohibited_covers_mapped_ports);
    RUN_TEST(test_mhmap_rom_e000_without_prohibited);
    RUN_TEST(test_mhmap_no_rom_no_prohibited);
    return UNITY_END();
}
