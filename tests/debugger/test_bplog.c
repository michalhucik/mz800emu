/**
 * @file   test_bplog.c
 * @brief  Unit testy ringu výstupu akcí breakpointů (bplog.h) bez emulátoru.
 *
 * Ověřuje:
 * - línou alokaci a segment (od startu >= 1, první záznam ho nemění, nový
 *   po clear a free),
 * - obsah záznamu (čas, snímek, adresy, id, jméno, druh, text, délka),
 * - ořez textu a jména s příznaky,
 * - přetečení ringu (first_seq, kopie od nejstaršího čitelného),
 * - převod jména druhu.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "bplog.h"


void setUp(void) {}

void tearDown(void)
{
    bplog_free();
}


/** @brief Před prvním záznamem ring neexistuje, segment už ano; první záznam ho nemění. */
static void test_bplog_lazy_alloc_and_segment(void)
{
    st_BPLOG_STATUS st;
    bplog_get_status(&st);
    TEST_ASSERT_FALSE(st.allocated);
    TEST_ASSERT_EQUAL_UINT64(0, st.next_seq);
    TEST_ASSERT_EQUAL_UINT32(BPLOG_CAPACITY, st.capacity);
    TEST_ASSERT_TRUE(st.segment >= 1);
    unsigned seg0 = st.segment;

    bplog_clear();  /* nad nealokovaným ringem nic */
    bplog_get_status(&st);
    TEST_ASSERT_FALSE(st.allocated);
    TEST_ASSERT_EQUAL_UINT(seg0, st.segment);

    /* Kurzor z prázdného kanálu (seg0, seq 0) platí i po prvním záznamu. */
    bplog_record(BPLOG_KIND_LOG, 3, "loop", 100, 2, 0x1200, 0x1200, "A=05");
    bplog_get_status(&st);
    TEST_ASSERT_TRUE(st.allocated);
    TEST_ASSERT_EQUAL_UINT(seg0, st.segment);
    TEST_ASSERT_EQUAL_UINT64(1, st.next_seq);
    TEST_ASSERT_EQUAL_UINT64(0, st.first_seq);
    unsigned seg1 = st.segment;

    bplog_clear();
    bplog_get_status(&st);
    TEST_ASSERT_TRUE(st.allocated);
    TEST_ASSERT_TRUE(st.segment > seg1);
    TEST_ASSERT_EQUAL_UINT64(0, st.next_seq);
    unsigned seg2 = st.segment;

    /* Uvolnění zahodí data: nový segment, prázdný kanál. */
    bplog_free();
    bplog_get_status(&st);
    TEST_ASSERT_FALSE(st.allocated);
    TEST_ASSERT_TRUE(st.segment > seg2);
    TEST_ASSERT_EQUAL_UINT64(0, st.next_seq);
}


/** @brief Obsah záznamu odpovídá argumentům. */
static void test_bplog_record_content(void)
{
    bplog_record(BPLOG_KIND_STOP, 7, "wr", 123456789012ull, 42, 0x3003, 0x3006, "HL=C000");
    st_BPLOG_REC r;
    uint64_t seq = 99;
    TEST_ASSERT_EQUAL_UINT64(1, bplog_copy(0, 1, &r, &seq));
    TEST_ASSERT_EQUAL_UINT64(0, seq);
    TEST_ASSERT_EQUAL_UINT64(123456789012ull, r.t);
    TEST_ASSERT_EQUAL_UINT32(42, r.frame);
    TEST_ASSERT_EQUAL_HEX16(0x3003, r.insn_pc);
    TEST_ASSERT_EQUAL_HEX16(0x3006, r.pc);
    TEST_ASSERT_EQUAL_INT32(7, r.bp_id);
    TEST_ASSERT_EQUAL_UINT8(BPLOG_KIND_STOP, r.kind);
    TEST_ASSERT_EQUAL_UINT8(0, r.flags);
    TEST_ASSERT_EQUAL_STRING("wr", r.name);
    TEST_ASSERT_EQUAL_STRING("HL=C000", r.text);
    TEST_ASSERT_EQUAL_UINT16(7, r.text_len);
    /* Zbytek pole je vynulovaný (stejné bajty v raw_b64). */
    TEST_ASSERT_EQUAL_CHAR(0, r.text[BPLOG_TEXT_SIZE - 1]);
    TEST_ASSERT_EQUAL_CHAR(0, r.name[BPLOG_NAME_SIZE - 1]);
}


/** @brief Dlouhý text a jméno se oříznou, záznam nese příznaky. */
static void test_bplog_truncation(void)
{
    char text[BPLOG_TEXT_SIZE + 50];
    char name[BPLOG_NAME_SIZE + 5];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    bplog_record(BPLOG_KIND_LOG, 1, name, 0, 0, 0, 0, text);
    /* Přesně na mez (BPLOG_TEXT_SIZE - 1 znaků) se ještě vejde. */
    text[BPLOG_TEXT_SIZE - 1] = 0;
    bplog_record(BPLOG_KIND_LOG, 1, "ok", 0, 0, 0, 0, text);

    st_BPLOG_REC r[2];
    uint64_t seq = 0;
    TEST_ASSERT_EQUAL_UINT64(2, bplog_copy(0, 2, r, &seq));
    TEST_ASSERT_EQUAL_UINT8(BPLOG_FLAG_TEXT_TRUNCATED | BPLOG_FLAG_NAME_TRUNCATED, r[0].flags);
    TEST_ASSERT_EQUAL_UINT16(BPLOG_TEXT_SIZE - 1, r[0].text_len);
    TEST_ASSERT_EQUAL_size_t(BPLOG_TEXT_SIZE - 1, strlen(r[0].text));
    TEST_ASSERT_EQUAL_size_t(BPLOG_NAME_SIZE - 1, strlen(r[0].name));
    TEST_ASSERT_EQUAL_UINT8(0, r[1].flags);
    TEST_ASSERT_EQUAL_UINT16(BPLOG_TEXT_SIZE - 1, r[1].text_len);
}


/** @brief Přetečení ringu: first_seq roste, kopie začne u nejstaršího čitelného. */
static void test_bplog_ring_overflow(void)
{
    char t[16];
    for (unsigned i = 0; i < BPLOG_CAPACITY + 10; i++) {
        snprintf(t, sizeof(t), "%u", i);
        bplog_record(BPLOG_KIND_LOG, 1, "b", i, 0, 0, 0, t);
    }
    st_BPLOG_STATUS st;
    bplog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(BPLOG_CAPACITY + 10, st.next_seq);
    TEST_ASSERT_EQUAL_UINT64(10, st.first_seq);

    st_BPLOG_REC r[3];
    uint64_t seq = 0;
    TEST_ASSERT_EQUAL_UINT64(3, bplog_copy(0, 3, r, &seq));
    TEST_ASSERT_EQUAL_UINT64(10, seq);
    TEST_ASSERT_EQUAL_STRING("10", r[0].text);
    TEST_ASSERT_EQUAL_STRING("12", r[2].text);

    /* Konec ringu: jen to, co existuje. */
    TEST_ASSERT_EQUAL_UINT64(2, bplog_copy(BPLOG_CAPACITY + 8, 3, r, &seq));
    TEST_ASSERT_EQUAL_UINT64(BPLOG_CAPACITY + 8, seq);
    TEST_ASSERT_EQUAL_UINT64(BPLOG_CAPACITY + 9, r[1].t);
    TEST_ASSERT_EQUAL_UINT64(0, bplog_copy(BPLOG_CAPACITY + 10, 3, r, &seq));
}


/** @brief Jména druhů. */
static void test_bplog_kind_names(void)
{
    TEST_ASSERT_EQUAL_STRING("log", bplog_kind_name(BPLOG_KIND_LOG));
    TEST_ASSERT_EQUAL_STRING("stop", bplog_kind_name(BPLOG_KIND_STOP));
    TEST_ASSERT_EQUAL_STRING("?", bplog_kind_name(BPLOG_KIND_COUNT));
    TEST_ASSERT_EQUAL_INT(BPLOG_KIND_LOG, bplog_kind_from_name("log"));
    TEST_ASSERT_EQUAL_INT(BPLOG_KIND_STOP, bplog_kind_from_name("stop"));
    TEST_ASSERT_EQUAL_INT(-1, bplog_kind_from_name("mark"));
    TEST_ASSERT_EQUAL_INT(-1, bplog_kind_from_name(NULL));
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bplog_lazy_alloc_and_segment);
    RUN_TEST(test_bplog_record_content);
    RUN_TEST(test_bplog_truncation);
    RUN_TEST(test_bplog_ring_overflow);
    RUN_TEST(test_bplog_kind_names);
    return UNITY_END();
}
