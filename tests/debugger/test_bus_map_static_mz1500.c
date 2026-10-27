/**
 * @file test_bus_map_static_mz1500.c
 * @brief Statická tabulka vlastníků MZ-1500 (mz1500_bus_owners.c) proti větvím switche, STANDALONE.
 *
 * Referenční seznamy portů jsou opsané z větví `case` v mz1500_iorq.c
 * (port_read_cb a port_write_cb, sestava mz1500_config.h: FDC, IDE8,
 * RAM disk, Quick Disk, Z80 PIO a PSG zapnuté; joystick ani Pezik 0E8h
 * ve switchi nejsou) a z mapovaných portů 0E000h-0E008h v mz1500_memory.c.
 * Test hlídá: každý port větve má v tabulce vlastníka ve správném směru
 * a naopak, mz1500_bus_port_is_static() odpovídá pokrytí tabulky, žádný
 * port nemá dva statické vlastníky stejného směru, každý řádek má id,
 * zkratku, zdroj, poznámku o zrcadlech a konzistentní budič; PSG na 0E9h,
 * 0F2h, 0F3h a GDG na 0F0h-0F1h jen zápis bez budiče.
 *
 * Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "mzarch/mz1500/mz1500_bus_owners.h"


void setUp(void) { }
void tearDown(void) { }


/** @brief Rozsah referenčního seznamu. */
typedef struct { uint16_t first, last; } st_RANGE;

/** @brief Porty, které port_read_cb() v mz1500_iorq.c obsluhuje (větve case, bez default). */
static const st_RANGE s_ref_read[] = {
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5b },  /* FDC1 data */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xd8, 0xdb },  /* FDC0 data */
    { 0xea, 0xea },  /* RAM disk std */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xf8, 0xf9 },  /* RAM disk std */
    { 0xfc, 0xff },  /* Z80 PIO */
};

/** @brief Porty, které port_write_cb() v mz1500_iorq.c obsluhuje. */
static const st_RANGE s_ref_write[] = {
    { 0x01, 0x02 },  /* CMT hack */
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5f },  /* FDC1 */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xd8, 0xdf },  /* FDC0 */
    { 0xe0, 0xe6 },  /* mapper */
    { 0xe7, 0xe7 },  /* MemExt */
    { 0xe9, 0xe9 },  /* PSG (oba) */
    { 0xea, 0xeb },  /* RAM disk std */
    { 0xf0, 0xf1 },  /* DMD / paleta */
    { 0xf2, 0xf3 },  /* PSG levý, pravý */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xfa, 0xfa },  /* RAM disk std */
    { 0xfc, 0xff },  /* Z80 PIO */
};


static bool in_ranges(const st_RANGE *r, unsigned n, unsigned p)
{
    for (unsigned i = 0; i < n; i++) {
        if (p >= r[i].first && p <= r[i].last) return true;
    }
    return false;
}

#define IN_RANGES(arr, p) in_ranges((arr), (unsigned) (sizeof(arr) / sizeof((arr)[0])), (p))

/** @brief Počet řádků tabulky pro adresu @p p se směrem @p dir. */
static unsigned table_rows_for(unsigned p, unsigned dir, bool mmio)
{
    unsigned n = 0;
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz1500_bus_owners(&count);
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_STATIC_OWNER *r = &rows[i];
        if ((r->mmio != 0) != mmio) continue;
        if (p < r->first || p > r->last) continue;
        if (r->dirs & dir) n++;
    }
    return n;
}


void test_ports_match_switch_cases(void)
{
    for (unsigned p = 0; p < 256; p++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "port %02Xh read", p);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(IN_RANGES(s_ref_read, p) ? 1 : 0, table_rows_for(p, BUS_DIR_R, false), msg);
        snprintf(msg, sizeof(msg), "port %02Xh write", p);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(IN_RANGES(s_ref_write, p) ? 1 : 0, table_rows_for(p, BUS_DIR_W, false), msg);
    }
}


void test_port_is_static_matches_table(void)
{
    for (unsigned p = 0; p < 256; p++) {
        bool covered = table_rows_for(p, BUS_DIR_R | BUS_DIR_W, false) > 0;
        bool in_switch = IN_RANGES(s_ref_read, p) || IN_RANGES(s_ref_write, p);
        char msg[64];
        snprintf(msg, sizeof(msg), "port %02Xh is_static", p);
        TEST_ASSERT_EQUAL_MESSAGE(covered, mz1500_bus_port_is_static((uint8_t) p), msg);
        TEST_ASSERT_EQUAL_MESSAGE(in_switch, mz1500_bus_port_is_static((uint8_t) p), msg);
    }
    /* Porty MZ-800, které MZ-1500 nedekóduje: GDG 0CCh-0CFh, 8255 / 8253 na 0D0h-0D7h, Pezik 0E8h. */
    for (unsigned p = 0xcc; p <= 0xd7; p++) TEST_ASSERT_FALSE(mz1500_bus_port_is_static((uint8_t) p));
    TEST_ASSERT_FALSE(mz1500_bus_port_is_static(0xe8));
}


void test_mmio_cells(void)
{
    for (unsigned a = 0xE000; a <= 0xE008; a++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "mmio %04Xh", a);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(a == 0xE007 ? 0 : 1, table_rows_for(a, BUS_DIR_R, true), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, table_rows_for(a, BUS_DIR_W, true), msg);
    }
    for (unsigned a = 0xE009; a <= 0xE0FF; a++) {
        TEST_ASSERT_EQUAL_UINT(0, table_rows_for(a, BUS_DIR_R | BUS_DIR_W, true));
    }
}


void test_rows_well_formed(void)
{
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz1500_bus_owners(&count);
    TEST_ASSERT_TRUE(count > 15);
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_STATIC_OWNER *r = &rows[i];
        char msg[64];
        snprintf(msg, sizeof(msg), "row %u (%04Xh)", i, r->first);
        TEST_ASSERT_TRUE_MESSAGE(r->first <= r->last, msg);
        if (!r->mmio) TEST_ASSERT_TRUE_MESSAGE(r->last <= 0xFF, msg);
        else TEST_ASSERT_TRUE_MESSAGE(r->first >= 0xE000 && r->last <= 0xE0FF, msg);
        TEST_ASSERT_TRUE_MESSAGE(r->dirs != 0 && (r->dirs & ~BUS_DIR_RW) == 0, msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(r->id, msg);
        TEST_ASSERT_TRUE_MESSAGE(r->id[0] >= 'a' && r->id[0] <= 'z', msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(r->name, msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(r->abbrev, msg);
        TEST_ASSERT_TRUE_MESSAGE(strlen(r->abbrev) >= 1 && strlen(r->abbrev) <= 2, msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(r->source_note, msg);
        if (!r->mmio) TEST_ASSERT_NOT_NULL_MESSAGE(r->mirror_note, msg);
        else TEST_ASSERT_NULL_MESSAGE(r->mirror_note, msg);
        if (!(r->dirs & BUS_DIR_R)) TEST_ASSERT_EQUAL_MESSAGE(DEVBUS_DRIVE_NONE, r->drive, msg);
        else TEST_ASSERT_TRUE_MESSAGE(r->drive != DEVBUS_DRIVE_NONE, msg);
        if (r->drive == DEVBUS_DRIVE_PARTIAL) TEST_ASSERT_TRUE_MESSAGE(r->drive_mask != 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(r->present_kind < MZ1500_BUS_PRESENT_COUNT, msg);
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(rows[j].id, r->id) == 0) TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[j].present_kind, r->present_kind, msg);
        }
    }
}


void test_documented_drives(void)
{
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz1500_bus_owners(&count);
    unsigned psg_rows = 0;
    bool seen_pio = false, seen_qd = false;
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_STATIC_OWNER *r = &rows[i];
        if (strcmp(r->id, "unicard") == 0) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive);
        if (strcmp(r->id, "ppi8255") == 0) {
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive);
            TEST_ASSERT_TRUE(r->mmio);
            TEST_ASSERT_EQUAL_UINT(MZ1500_BUS_PRESENT_ALWAYS, r->present_kind);
        }
        if (strcmp(r->id, "pioz80") == 0) { seen_pio = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive); }
        if (strcmp(r->id, "psg") == 0) {
            psg_rows++;
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
            TEST_ASSERT_EQUAL_UINT(BUS_DIR_W, r->dirs);
            TEST_ASSERT_EQUAL_UINT(MZ1500_BUS_PRESENT_ALWAYS, r->present_kind);
        }
        if (strcmp(r->id, "gdg") == 0 && !r->mmio) {
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
            TEST_ASSERT_EQUAL_UINT(BUS_DIR_W, r->dirs);
        }
        if (strcmp(r->id, "qdisk") == 0) { seen_qd = true; TEST_ASSERT_NOT_NULL(strstr(r->source_note, "74h-77h")); }
        if (strcmp(r->id, "mapper") == 0) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
        if (r->drive == DEVBUS_DRIVE_TRISTATE) {
            TEST_ASSERT_TRUE(strstr(r->source_note, "datasheet") || strstr(r->source_note, "VHDL"));
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, psg_rows);
    TEST_ASSERT_TRUE(seen_pio && seen_qd);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ports_match_switch_cases);
    RUN_TEST(test_port_is_static_matches_table);
    RUN_TEST(test_mmio_cells);
    RUN_TEST(test_rows_well_formed);
    RUN_TEST(test_documented_drives);
    return UNITY_END();
}
