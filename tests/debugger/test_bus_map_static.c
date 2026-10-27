/**
 * @file test_bus_map_static.c
 * @brief Statická tabulka vlastníků MZ-700 (mz700_bus_owners.c) proti větvím switche, STANDALONE.
 *
 * Referenční seznamy portů jsou opsané z větví `case` v mz700_iorq.c
 * (port_read_cb a port_write_cb, sestava mz700_config.h: FDC, IDE8,
 * RAM disk, Quick Disk zapnuté, Z80 PIO a PSG vypnuté) a z mapovaných
 * portů 0E000h-0E008h v mz700_memory.c. Test hlídá: každý port větve má
 * v tabulce vlastníka ve správném směru a naopak, mz700_bus_port_is_static()
 * odpovídá pokrytí tabulky (= dřívější ruční seznam adaptéru), žádný port
 * nemá dva statické vlastníky stejného směru, každý řádek má id, zkratku,
 * zdroj a konzistentní budič.
 *
 * Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "mzarch/mz700/mz700_bus_owners.h"


void setUp(void) { }
void tearDown(void) { }


/** @brief Rozsah referenčního seznamu. */
typedef struct { uint16_t first, last; } st_RANGE;

/** @brief Porty, které port_read_cb() v mz700_iorq.c obsluhuje (větve case, bez default). */
static const st_RANGE s_ref_read[] = {
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5b },  /* FDC1 data */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xd8, 0xdb },  /* FDC0 data */
    { 0xea, 0xea },  /* RAM disk std */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xf8, 0xf9 },  /* RAM disk std */
    /* 0FCh-0FFh Z80 PIO: HAVE_PIOZ80 = 0 v mz700_config.h */
};

/** @brief Porty, které port_write_cb() v mz700_iorq.c obsluhuje. */
static const st_RANGE s_ref_write[] = {
    { 0x01, 0x02 },  /* CMT hack */
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5f },  /* FDC1 */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xd8, 0xdf },  /* FDC0 */
    { 0xe0, 0xe6 },  /* mapper */
    { 0xe7, 0xe7 },  /* MemExt */
    { 0xea, 0xeb },  /* RAM disk std */
    { 0xf0, 0xf1 },  /* DMD / paleta */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xfa, 0xfa },  /* RAM disk std */
};

/** @brief Dřívější ruční seznam mz700_devbus_port_is_static() (D1-D5). */
static const st_RANGE s_ref_static_old[] = {
    { 0x01, 0x02 }, { 0x50, 0x51 }, { 0x68, 0x6f }, { 0xea, 0xeb }, { 0xf8, 0xfa },
    { 0x78, 0x7f }, { 0xd8, 0xdf }, { 0x58, 0x5f }, { 0xe0, 0xe7 }, { 0xf0, 0xf1 },
    { 0xf4, 0xf7 },
};


static bool in_ranges(const st_RANGE *r, unsigned n, unsigned p)
{
    for (unsigned i = 0; i < n; i++) {
        if (p >= r[i].first && p <= r[i].last) return true;
    }
    return false;
}

/** @brief Má tabulka pro port @p p řádek se směrem @p dir? Vrátí počet takových řádků. */
static unsigned table_rows_for(unsigned p, unsigned dir, bool mmio)
{
    unsigned n = 0;
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz700_bus_owners(&count);
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
        TEST_ASSERT_EQUAL_UINT_MESSAGE(in_ranges(s_ref_read, sizeof(s_ref_read) / sizeof(s_ref_read[0]), p) ? 1 : 0,
                                       table_rows_for(p, BUS_DIR_R, false), msg);
        snprintf(msg, sizeof(msg), "port %02Xh write", p);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(in_ranges(s_ref_write, sizeof(s_ref_write) / sizeof(s_ref_write[0]), p) ? 1 : 0,
                                       table_rows_for(p, BUS_DIR_W, false), msg);
    }
}


void test_port_is_static_matches_old_list_and_table(void)
{
    for (unsigned p = 0; p < 256; p++) {
        bool old = in_ranges(s_ref_static_old, sizeof(s_ref_static_old) / sizeof(s_ref_static_old[0]), p);
        bool covered = table_rows_for(p, BUS_DIR_R | BUS_DIR_W, false) > 0;
        char msg[64];
        snprintf(msg, sizeof(msg), "port %02Xh is_static", p);
        TEST_ASSERT_EQUAL_MESSAGE(old, mz700_bus_port_is_static((uint8_t) p), msg);
        TEST_ASSERT_EQUAL_MESSAGE(covered, mz700_bus_port_is_static((uint8_t) p), msg);
    }
}


void test_mmio_cells(void)
{
    /* 0E000h-0E006h a 0E008h čitelné, 0E007h (řídicí slovo 8253) ne; zápis všude 0E000h-0E008h. */
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
    const st_BUS_STATIC_OWNER *rows = mz700_bus_owners(&count);
    TEST_ASSERT_TRUE(count > 10);
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
        /* budič jen u řádku se čtením; řádek se čtením má budič nebo UNKNOWN */
        if (!(r->dirs & BUS_DIR_R)) TEST_ASSERT_EQUAL_MESSAGE(DEVBUS_DRIVE_NONE, r->drive, msg);
        else TEST_ASSERT_TRUE_MESSAGE(r->drive != DEVBUS_DRIVE_NONE, msg);
        if (r->drive == DEVBUS_DRIVE_PARTIAL) TEST_ASSERT_TRUE_MESSAGE(r->drive_mask != 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(r->present_kind < MZ700_BUS_PRESENT_COUNT, msg);
        /* stejné id = stejný druh přítomnosti */
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(rows[j].id, r->id) == 0) TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[j].present_kind, r->present_kind, msg);
        }
    }
}


void test_documented_drives(void)
{
    /* Doložené budiče: Unicard (VHDL) a 8255 (datasheet) třístavové; 8253 třístavový
     * s poznámkou; mapper a MemExt jen zápis; FDC, IDE8, RAM disk, QD neznámé. */
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz700_bus_owners(&count);
    bool seen_unicard = false, seen_ppi = false, seen_fdc = false;
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_STATIC_OWNER *r = &rows[i];
        if (strcmp(r->id, "unicard") == 0) { seen_unicard = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive); }
        if (strcmp(r->id, "ppi8255") == 0) { seen_ppi = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive); }
        if (strcmp(r->id, "fdc0") == 0 && (r->dirs & BUS_DIR_R)) { seen_fdc = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_UNKNOWN, r->drive); }
        if (strcmp(r->id, "mapper") == 0) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
        /* žádný řádek neodhaduje: TRISTATE jen s poznámkou o datasheetu / VHDL */
        if (r->drive == DEVBUS_DRIVE_TRISTATE) {
            TEST_ASSERT_TRUE(strstr(r->source_note, "datasheet") || strstr(r->source_note, "VHDL")
                             || strstr(r->source_note, "KOMPENDIUM"));
        }
    }
    TEST_ASSERT_TRUE(seen_unicard && seen_ppi && seen_fdc);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ports_match_switch_cases);
    RUN_TEST(test_port_is_static_matches_old_list_and_table);
    RUN_TEST(test_mmio_cells);
    RUN_TEST(test_rows_well_formed);
    RUN_TEST(test_documented_drives);
    return UNITY_END();
}
