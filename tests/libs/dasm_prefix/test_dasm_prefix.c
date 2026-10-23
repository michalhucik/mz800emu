/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_dasm_prefix.c
 * @brief Unit testy zobrazení ignorovaného prefixu DD/FD a neplatné ED.
 *
 * Prefix DD nebo FD, za kterým následuje další prefix (DD, FD) nebo ED,
 * CPU vykoná (4 T) a další bajt zpracuje jako novou instrukci; jádro
 * emulátoru obojí provede v jednom kroku. Disassembler ho ukazuje jako
 * jednobajtovou instrukci `DD*` / `FD*` (dřív `NOP*`), nedefinovaná
 * `ED xx` zůstává `NOP*` (2 bajty, 8 T).
 *
 * Test scénáře:
 *
 *   1) DD DD 21 34 12: první řádek DD* (1 B, 4 T), další DD 21 = LD IX,1234h
 *   2) FD DD, FD FD, DD FD, DD ED, FD ED: DD* / FD*
 *   3) ED 00 (nedefinovaná): NOP*, 2 B, 8 T
 *   4) metadata z80meta: DD*, FD* a NOP* mají popis
 *
 * Standalone test - linkuje mzlib_dasm_z80, mzlib_z80meta a Unity.
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "libs/dasm-z80/z80_dasm.h"
#include "libs/z80meta/z80_meta.h"


/** @brief Kontext read callbacku - čtení z bufferu od adresy 0. */
typedef struct {
    const uint8_t *bytes;  /**< Buffer instrukcí (čte se od adresy 0). */
    size_t len;            /**< Velikost bufferu. */
} test_ctx_t;


/**
 * @brief Read callback pro z80_dasm (mimo buffer 0x00 = NOP).
 */
static uint8_t test_read(uint16_t addr, void *user_data)
{
    const test_ctx_t *ctx = (const test_ctx_t *)user_data;
    return (addr < ctx->len) ? ctx->bytes[addr] : 0x00;
}


void setUp(void) {}
void tearDown(void) {}


/**
 * @brief Dekóduje instrukci na adrese @p addr a vyformátuje ji (velká písmena).
 */
static void decode(const uint8_t *bytes, size_t len, uint16_t addr,
                   z80_dasm_inst_t *inst, char *out, int out_sz)
{
    test_ctx_t ctx = { .bytes = bytes, .len = len };
    z80_dasm(inst, test_read, &ctx, addr);
    z80_dasm_format_t fmt;
    z80_dasm_format_default(&fmt);
    fmt.show_addr = 0;
    fmt.show_bytes = 0;
    z80_dasm_to_str(out, out_sz, inst, &fmt);
}


/** @brief DD DD 21 34 12: DD* (1 B, 4 T), pak LD IX,1234h. */
void test_dd_dd_chain(void)
{
    const uint8_t bytes[] = { 0xDD, 0xDD, 0x21, 0x34, 0x12 };
    z80_dasm_inst_t inst;
    char out[64];
    decode(bytes, sizeof bytes, 0, &inst, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(1, inst.length);
    TEST_ASSERT_EQUAL_INT(4, inst.t_states);
    TEST_ASSERT_EQUAL_INT(Z80_CLASS_INVALID, inst.cls);
    TEST_ASSERT_EQUAL_STRING("DD*", inst.mnemonic);
    TEST_ASSERT_EQUAL_STRING("DD*", out);

    decode(bytes, sizeof bytes, 1, &inst, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(4, inst.length);
    TEST_ASSERT_EQUAL_STRING("LD", inst.mnemonic);
}


/** @brief Ostatní kombinace ignorovaného prefixu. */
void test_other_ignored_prefixes(void)
{
    static const struct { uint8_t b0, b1; const char *want; } cases[] = {
        { 0xFD, 0xDD, "FD*" }, { 0xFD, 0xFD, "FD*" }, { 0xDD, 0xFD, "DD*" },
        { 0xDD, 0xED, "DD*" }, { 0xFD, 0xED, "FD*" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const uint8_t bytes[] = { cases[i].b0, cases[i].b1, 0x00, 0x00 };
        z80_dasm_inst_t inst;
        char out[64];
        decode(bytes, sizeof bytes, 0, &inst, out, sizeof out);
        TEST_ASSERT_EQUAL_INT(1, inst.length);
        TEST_ASSERT_EQUAL_STRING(cases[i].want, inst.mnemonic);
        TEST_ASSERT_EQUAL_STRING(cases[i].want, out);
    }
}


/** @brief Nedefinovaná ED 00 zůstává NOP* (2 B, 8 T). */
void test_invalid_ed_stays_nop(void)
{
    const uint8_t bytes[] = { 0xED, 0x00 };
    z80_dasm_inst_t inst;
    char out[64];
    decode(bytes, sizeof bytes, 0, &inst, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(2, inst.length);
    TEST_ASSERT_EQUAL_INT(8, inst.t_states);
    TEST_ASSERT_EQUAL_STRING("NOP*", inst.mnemonic);
    TEST_ASSERT_EQUAL_STRING("NOP*", out);
}


/** @brief z80meta zná popis DD*, FD* i NOP*. */
void test_meta_descriptions(void)
{
    static const uint8_t seqs[][2] = { { 0xDD, 0xDD }, { 0xFD, 0xED }, { 0xED, 0x00 } };
    static const char *const prefix[] = { "DD*:", "FD*:", "NOP*:" };
    for (size_t i = 0; i < 3; i++) {
        z80_dasm_inst_t inst;
        char out[64];
        decode(seqs[i], 2, 0, &inst, out, sizeof out);
        const z80_meta_t *m = z80_meta_lookup(&inst);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_EQUAL_INT(0, strncmp(m->description, prefix[i], strlen(prefix[i])));
    }
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dd_dd_chain);
    RUN_TEST(test_other_ignored_prefixes);
    RUN_TEST(test_invalid_ed_stays_nop);
    RUN_TEST(test_meta_descriptions);
    return UNITY_END();
}
