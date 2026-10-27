/**
 * @file test_devbus_resolve.c
 * @brief Rezoluce čtení sdílené sběrnice (devbus_resolve.h), STANDALONE.
 *
 * Tabulkové případy pravidel 1-5: ignorování nebudících, jediný budič
 * s doplněním z idle, částečný budič (PARTIAL), otevřený kolektor
 * (wired-AND), kolize dvou třístavových budičů s politikou AND / FIRST /
 * REFUSE, smíšená kolize OC + třístavový, maska kolize, jména a parsování.
 *
 * Licence: GPLv3
 */

#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "hw-generic/devbus/devbus_resolve.h"


void setUp(void) { }
void tearDown(void) { }


/** @brief Jeden tabulkový případ rezoluce. */
typedef struct {
    const char *name;
    st_DEVBUS_CONTRIB c[4];
    unsigned n;
    uint8_t idle;
    en_DEVBUS_CONFLICT policy;
    uint8_t exp_value, exp_driven, exp_conflict;
} st_CASE;

static const st_CASE s_cases[] = {
    /* 1. nikdo nebudí: idle */
    { "nobody", { { DEVBUS_DRIVE_NONE, 0x12, 0xFF } }, 1, 0x5A, DEVBUS_CONFLICT_REFUSE, 0x5A, 0x00, 0x00 },
    { "empty", { { 0 } }, 0, 0xA5, DEVBUS_CONFLICT_REFUSE, 0xA5, 0x00, 0x00 },
    { "tristate driven 0 = nobody", { { DEVBUS_DRIVE_TRISTATE, 0x12, 0x00 } }, 1, 0x5A, DEVBUS_CONFLICT_REFUSE, 0x5A, 0x00, 0x00 },
    { "unknown ignored", { { DEVBUS_DRIVE_UNKNOWN, 0x12, 0xFF } }, 1, 0x5A, DEVBUS_CONFLICT_REFUSE, 0x5A, 0x00, 0x00 },
    /* 3. jediný budič */
    { "single tristate", { { DEVBUS_DRIVE_TRISTATE, 0x12, 0xFF } }, 1, 0x5A, DEVBUS_CONFLICT_REFUSE, 0x12, 0xFF, 0x00 },
    { "tristate after none", { { DEVBUS_DRIVE_NONE, 0xEE, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0x34, 0xFF } }, 2, 0x5A, DEVBUS_CONFLICT_REFUSE, 0x34, 0xFF, 0x00 },
    /* PARTIAL: 3Fh od budiče, bity 6-7 z idle (joystick LS365) */
    { "partial 3F idle FF", { { DEVBUS_DRIVE_PARTIAL, 0x00, 0x3F } }, 1, 0xFF, DEVBUS_CONFLICT_REFUSE, 0xC0, 0x3F, 0x00 },
    { "partial 3F idle 00", { { DEVBUS_DRIVE_PARTIAL, 0xFF, 0x3F } }, 1, 0x00, DEVBUS_CONFLICT_REFUSE, 0x3F, 0x3F, 0x00 },
    /* dva částeční s disjunktními maskami: bez kolize */
    { "partial disjoint", { { DEVBUS_DRIVE_PARTIAL, 0x0F, 0x0F }, { DEVBUS_DRIVE_PARTIAL, 0xA0, 0xF0 } }, 2, 0x55, DEVBUS_CONFLICT_REFUSE, 0xAF, 0xFF, 0x00 },
    /* tristate, který snížil masku (K&P R31): idle */
    { "tristate lowered to 0", { { DEVBUS_DRIVE_TRISTATE, 0x00, 0x00 }, { DEVBUS_DRIVE_NONE, 0, 0 } }, 2, 0x78, DEVBUS_CONFLICT_REFUSE, 0x78, 0x00, 0x00 },
    /* 2. jen otevřené kolektory: wired-AND, klid 1 bez ohledu na idle */
    { "oc single", { { DEVBUS_DRIVE_OPEN_COLLECTOR, 0xF0, 0xFF } }, 1, 0x00, DEVBUS_CONFLICT_REFUSE, 0xF0, 0xFF, 0x00 },
    { "oc two and", { { DEVBUS_DRIVE_OPEN_COLLECTOR, 0xF0, 0xFF }, { DEVBUS_DRIVE_OPEN_COLLECTOR, 0x3C, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_REFUSE, 0x30, 0xFF, 0x00 },
    { "oc partial pulls only its bits", { { DEVBUS_DRIVE_OPEN_COLLECTOR, 0x00, 0x01 } }, 1, 0x00, DEVBUS_CONFLICT_REFUSE, 0x00, 0x01, 0x00 },
    { "oc partial idle elsewhere", { { DEVBUS_DRIVE_OPEN_COLLECTOR, 0x00, 0x01 } }, 1, 0xFE, DEVBUS_CONFLICT_REFUSE, 0xFE, 0x01, 0x00 },
    /* 4. kolize dvou třístavových */
    { "conflict AND", { { DEVBUS_DRIVE_TRISTATE, 0xF0, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0x3C, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_AND, 0x30, 0xFF, 0xFF },
    { "conflict FIRST", { { DEVBUS_DRIVE_TRISTATE, 0xF0, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0x3C, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_FIRST, 0xF0, 0xFF, 0xFF },
    { "conflict REFUSE acts as FIRST", { { DEVBUS_DRIVE_TRISTATE, 0xF0, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0x3C, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_REFUSE, 0xF0, 0xFF, 0xFF },
    /* částečná kolize: jen společné bity */
    { "partial conflict AND", { { DEVBUS_DRIVE_PARTIAL, 0x0F, 0x0F }, { DEVBUS_DRIVE_PARTIAL, 0x30, 0x3C } }, 2, 0x80, DEVBUS_CONFLICT_AND, 0xB3, 0x3F, 0x0C },
    { "partial conflict FIRST", { { DEVBUS_DRIVE_PARTIAL, 0x0F, 0x0F }, { DEVBUS_DRIVE_PARTIAL, 0x30, 0x3C } }, 2, 0x80, DEVBUS_CONFLICT_FIRST, 0xBF, 0x3F, 0x0C },
    /* tři budiče */
    { "three AND", { { DEVBUS_DRIVE_TRISTATE, 0xFF, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0xF0, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0x3C, 0xFF } }, 3, 0x00, DEVBUS_CONFLICT_AND, 0x30, 0xFF, 0xFF },
    /* smíšené: OC proti třístavovému */
    { "oc vs tristate AND", { { DEVBUS_DRIVE_TRISTATE, 0xFF, 0xFF }, { DEVBUS_DRIVE_OPEN_COLLECTOR, 0x0F, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_AND, 0x0F, 0xFF, 0xFF },
    { "oc vs tristate FIRST (tristate wins)", { { DEVBUS_DRIVE_OPEN_COLLECTOR, 0x0F, 0xFF }, { DEVBUS_DRIVE_TRISTATE, 0xFF, 0xFF } }, 2, 0x00, DEVBUS_CONFLICT_FIRST, 0xFF, 0xFF, 0xFF },
    /* deska (TRISTATE) + zařízení NONE: deska */
    { "board + snoop", { { DEVBUS_DRIVE_TRISTATE, 0x42, 0xFF }, { DEVBUS_DRIVE_NONE, 0x00, 0x00 } }, 2, 0x00, DEVBUS_CONFLICT_REFUSE, 0x42, 0xFF, 0x00 },
};


void test_resolve_table(void)
{
    for (unsigned i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const st_CASE *k = &s_cases[i];
        st_DEVBUS_RESOLVED r;
        memset(&r, 0xEE, sizeof(r));
        devbus_resolve(k->c, k->n, k->idle, k->policy, &r);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: value", k->name);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(k->exp_value, r.value, msg);
        snprintf(msg, sizeof(msg), "%s: driven", k->name);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(k->exp_driven, r.driven, msg);
        snprintf(msg, sizeof(msg), "%s: conflict", k->name);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(k->exp_conflict, r.conflict, msg);
    }
}


void test_resolve_null_contrib_with_zero_count(void)
{
    st_DEVBUS_RESOLVED r;
    devbus_resolve(NULL, 0, 0x33, DEVBUS_CONFLICT_AND, &r);
    TEST_ASSERT_EQUAL_HEX8(0x33, r.value);
    TEST_ASSERT_EQUAL_HEX8(0, r.driven);
    TEST_ASSERT_EQUAL_HEX8(0, r.conflict);
}


void test_drive_mask(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00, devbus_drive_mask(DEVBUS_DRIVE_NONE, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(0xFF, devbus_drive_mask(DEVBUS_DRIVE_TRISTATE, 0x00));
    TEST_ASSERT_EQUAL_HEX8(0x3F, devbus_drive_mask(DEVBUS_DRIVE_PARTIAL, 0x3F));
    TEST_ASSERT_EQUAL_HEX8(0xFF, devbus_drive_mask(DEVBUS_DRIVE_OPEN_COLLECTOR, 0x01));
    TEST_ASSERT_EQUAL_HEX8(0x00, devbus_drive_mask(DEVBUS_DRIVE_UNKNOWN, 0xFF));
}


void test_drive_overlaps(void)
{
    /* dva třístavové: kolize */
    TEST_ASSERT_EQUAL_INT(1, devbus_drive_overlaps(DEVBUS_DRIVE_TRISTATE, 0, DEVBUS_DRIVE_TRISTATE, 0));
    /* budič + pozorovatel: ne */
    TEST_ASSERT_EQUAL_INT(0, devbus_drive_overlaps(DEVBUS_DRIVE_TRISTATE, 0, DEVBUS_DRIVE_NONE, 0));
    TEST_ASSERT_EQUAL_INT(0, devbus_drive_overlaps(DEVBUS_DRIVE_NONE, 0, DEVBUS_DRIVE_NONE, 0));
    /* částečné s disjunktními maskami: ne; s překryvem: ano */
    TEST_ASSERT_EQUAL_INT(0, devbus_drive_overlaps(DEVBUS_DRIVE_PARTIAL, 0x0F, DEVBUS_DRIVE_PARTIAL, 0xF0));
    TEST_ASSERT_EQUAL_INT(1, devbus_drive_overlaps(DEVBUS_DRIVE_PARTIAL, 0x0F, DEVBUS_DRIVE_PARTIAL, 0x08));
    TEST_ASSERT_EQUAL_INT(1, devbus_drive_overlaps(DEVBUS_DRIVE_PARTIAL, 0x3F, DEVBUS_DRIVE_TRISTATE, 0));
    /* dva otevřené kolektory: wired-AND, ne kolize; OC + třístavový: kolize */
    TEST_ASSERT_EQUAL_INT(0, devbus_drive_overlaps(DEVBUS_DRIVE_OPEN_COLLECTOR, 0, DEVBUS_DRIVE_OPEN_COLLECTOR, 0));
    TEST_ASSERT_EQUAL_INT(1, devbus_drive_overlaps(DEVBUS_DRIVE_OPEN_COLLECTOR, 0, DEVBUS_DRIVE_TRISTATE, 0));
    /* UNKNOWN nebudí (jen statická tabulka) */
    TEST_ASSERT_EQUAL_INT(0, devbus_drive_overlaps(DEVBUS_DRIVE_UNKNOWN, 0, DEVBUS_DRIVE_TRISTATE, 0));
}


void test_names_and_parse(void)
{
    TEST_ASSERT_EQUAL_STRING("none", devbus_drive_name(DEVBUS_DRIVE_NONE));
    TEST_ASSERT_EQUAL_STRING("tristate", devbus_drive_name(DEVBUS_DRIVE_TRISTATE));
    TEST_ASSERT_EQUAL_STRING("partial", devbus_drive_name(DEVBUS_DRIVE_PARTIAL));
    TEST_ASSERT_EQUAL_STRING("open_collector", devbus_drive_name(DEVBUS_DRIVE_OPEN_COLLECTOR));
    TEST_ASSERT_EQUAL_STRING("unknown", devbus_drive_name(DEVBUS_DRIVE_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("unknown", devbus_drive_name((en_DEVBUS_DRIVE) 99));
    TEST_ASSERT_EQUAL_STRING("refuse", devbus_conflict_name(DEVBUS_CONFLICT_REFUSE));
    TEST_ASSERT_EQUAL_STRING("and", devbus_conflict_name(DEVBUS_CONFLICT_AND));
    TEST_ASSERT_EQUAL_STRING("first", devbus_conflict_name(DEVBUS_CONFLICT_FIRST));
    en_DEVBUS_CONFLICT p = DEVBUS_CONFLICT_REFUSE;
    TEST_ASSERT_EQUAL_INT(1, devbus_conflict_parse("AND", &p));
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_AND, p);
    TEST_ASSERT_EQUAL_INT(1, devbus_conflict_parse("first", &p));
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_FIRST, p);
    TEST_ASSERT_EQUAL_INT(1, devbus_conflict_parse("Refuse", &p));
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_REFUSE, p);
    TEST_ASSERT_EQUAL_INT(0, devbus_conflict_parse("or", &p));
    TEST_ASSERT_EQUAL_INT(0, devbus_conflict_parse("", &p));
    TEST_ASSERT_EQUAL_INT(0, devbus_conflict_parse(NULL, &p));
    TEST_ASSERT_EQUAL_INT(DEVBUS_CONFLICT_REFUSE, p);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resolve_table);
    RUN_TEST(test_resolve_null_contrib_with_zero_count);
    RUN_TEST(test_drive_mask);
    RUN_TEST(test_drive_overlaps);
    RUN_TEST(test_names_and_parse);
    return UNITY_END();
}
