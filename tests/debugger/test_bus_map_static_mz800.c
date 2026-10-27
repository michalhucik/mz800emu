/**
 * @file test_bus_map_static_mz800.c
 * @brief Statická tabulka vlastníků MZ-800 (mz800_bus_owners.c) proti větvím switche, STANDALONE.
 *
 * Referenční seznamy portů jsou opsané z větví `case` v mz800_iorq.c
 * (port_read_cb a port_write_cb, sestava mz800_config.h: FDC, IDE8,
 * RAM disk, Quick Disk, Z80 PIO, joystick a PSG zapnuté) a z mapovaných
 * portů 0E000h-0E008h v mz800_memory.c (režim MZ-700). Test hlídá: každý
 * port větve má v tabulce vlastníka ve správném směru a naopak,
 * mz800_bus_port_is_static() odpovídá pokrytí tabulky, dva statičtí
 * vlastníci stejného směru jsou jen tam, kde je switch řeší prioritou
 * (Pezik 0E8h před standardním RAM diskem na 0E9h-0EBh), každý řádek má
 * id, zkratku, zdroj, poznámku o zrcadlech a konzistentní budič; doložené
 * budiče (8255, Z80 PIO, Unicard třístavové, joystick PARTIAL 3Fh, GDG
 * status neznámý, mapper a PSG nebudí) sedí.
 *
 * Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "mzarch/mz800/mz800_bus_owners.h"


void setUp(void) { }
void tearDown(void) { }


/** @brief Rozsah referenčního seznamu. */
typedef struct { uint16_t first, last; } st_RANGE;

/** @brief Porty, které port_read_cb() v mz800_iorq.c obsluhuje (větve case, bez default). */
static const st_RANGE s_ref_read[] = {
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5b },  /* FDC1 data */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xce, 0xce },  /* GDG status */
    { 0xd0, 0xd3 },  /* 8255 (režim MZ-800) */
    { 0xd4, 0xd6 },  /* 8253 (režim MZ-800), 0D7h nelze číst */
    { 0xd8, 0xdb },  /* FDC0 data */
    { 0xe0, 0xe1 },  /* mapper: IN volí VRAM / CG-ROM, vrací latch */
    { 0xe8, 0xef },  /* Pezik 0E8h (0EAh i standardní RAM disk) */
    { 0xf0, 0xf1 },  /* joystick */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xf8, 0xf9 },  /* RAM disk std */
    { 0xfc, 0xff },  /* Z80 PIO */
};

/** @brief Porty, které port_write_cb() v mz800_iorq.c obsluhuje. */
static const st_RANGE s_ref_write[] = {
    { 0x01, 0x02 },  /* CMT hack */
    { 0x50, 0x51 },  /* Unicard */
    { 0x58, 0x5f },  /* FDC1 */
    { 0x68, 0x6f },  /* Pezik 68h */
    { 0x78, 0x7f },  /* IDE8 */
    { 0xcc, 0xcf },  /* GDG WF, RF, DMD, 16bitové registry */
    { 0xd0, 0xd3 },  /* 8255 (režim MZ-800) */
    { 0xd4, 0xd7 },  /* 8253 (režim MZ-800) */
    { 0xd8, 0xdf },  /* FDC0 */
    { 0xe0, 0xe6 },  /* mapper */
    { 0xe7, 0xe7 },  /* MemExt */
    { 0xe8, 0xef },  /* Pezik 0E8h (0E9h-0EBh i standardní RAM disk) */
    { 0xf0, 0xf0 },  /* paleta */
    { 0xf2, 0xf3 },  /* PSG, druhý PSG (stereo) */
    { 0xf4, 0xf7 },  /* Quick Disk */
    { 0xf9, 0xfa },  /* druhý PSG (stereo), RAM disk std */
    { 0xfc, 0xff },  /* Z80 PIO */
};

/** @brief Porty se dvěma statickými vlastníky téhož směru (switch řeší prioritou). */
static const st_RANGE s_ref_dual_read[]  = { { 0xea, 0xea } };  /* Pezik 0E8h před RAM diskem std */
static const st_RANGE s_ref_dual_write[] = { { 0xe9, 0xeb } };  /* Pezik 0E8h před RAM diskem std */


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
    const st_BUS_STATIC_OWNER *rows = mz800_bus_owners(&count);
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
        unsigned exp_r = IN_RANGES(s_ref_read, p) ? (IN_RANGES(s_ref_dual_read, p) ? 2 : 1) : 0;
        unsigned exp_w = IN_RANGES(s_ref_write, p) ? (IN_RANGES(s_ref_dual_write, p) ? 2 : 1) : 0;
        snprintf(msg, sizeof(msg), "port %02Xh read", p);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(exp_r, table_rows_for(p, BUS_DIR_R, false), msg);
        snprintf(msg, sizeof(msg), "port %02Xh write", p);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(exp_w, table_rows_for(p, BUS_DIR_W, false), msg);
    }
}


void test_port_is_static_matches_table(void)
{
    for (unsigned p = 0; p < 256; p++) {
        bool covered = table_rows_for(p, BUS_DIR_R | BUS_DIR_W, false) > 0;
        bool in_switch = IN_RANGES(s_ref_read, p) || IN_RANGES(s_ref_write, p);
        char msg[64];
        snprintf(msg, sizeof(msg), "port %02Xh is_static", p);
        TEST_ASSERT_EQUAL_MESSAGE(covered, mz800_bus_port_is_static((uint8_t) p), msg);
        TEST_ASSERT_EQUAL_MESSAGE(in_switch, mz800_bus_port_is_static((uint8_t) p), msg);
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
    /* Mapované porty jen v režimu MZ-700. */
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz800_bus_owners(&count);
    for (unsigned i = 0; i < count; i++) {
        if (rows[i].mmio) TEST_ASSERT_EQUAL_UINT(MZ800_BUS_PRESENT_MODE700, rows[i].present_kind);
    }
}


void test_rows_well_formed(void)
{
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz800_bus_owners(&count);
    TEST_ASSERT_TRUE(count > 20);
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
        /* porty: poznámka o zrcadlech vždy (model emulátoru, HW neměřeno); MMIO bez ní */
        if (!r->mmio) TEST_ASSERT_NOT_NULL_MESSAGE(r->mirror_note, msg);
        else TEST_ASSERT_NULL_MESSAGE(r->mirror_note, msg);
        /* budič jen u řádku se čtením; řádek se čtením má budič nebo UNKNOWN,
         * výjimka: mapper IN 0E0h / 0E1h (dekódované čtení bez budiče) */
        if (!(r->dirs & BUS_DIR_R)) TEST_ASSERT_EQUAL_MESSAGE(DEVBUS_DRIVE_NONE, r->drive, msg);
        else if (strcmp(r->id, "mapper") == 0) TEST_ASSERT_EQUAL_MESSAGE(DEVBUS_DRIVE_NONE, r->drive, msg);
        else TEST_ASSERT_TRUE_MESSAGE(r->drive != DEVBUS_DRIVE_NONE, msg);
        if (r->drive == DEVBUS_DRIVE_PARTIAL) TEST_ASSERT_TRUE_MESSAGE(r->drive_mask != 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(r->present_kind < MZ800_BUS_PRESENT_COUNT, msg);
        /* stejné id = stejný druh přítomnosti, kromě čipů přítomných podle režimu DMD
         * (8255, 8253: porty v režimu MZ-800, mapované adresy v režimu MZ-700; GDG vždy
         * na portech a jen v režimu MZ-700 na 0E008h) */
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(rows[j].id, r->id) != 0) continue;
            if (rows[j].mmio != r->mmio) continue;
            TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[j].present_kind, r->present_kind, msg);
        }
    }
}


void test_documented_drives(void)
{
    unsigned count = 0;
    const st_BUS_STATIC_OWNER *rows = mz800_bus_owners(&count);
    bool seen_joy = false, seen_status = false, seen_pio = false, seen_psg = false, seen_ppi_port = false;
    for (unsigned i = 0; i < count; i++) {
        const st_BUS_STATIC_OWNER *r = &rows[i];
        if (strcmp(r->id, "unicard") == 0) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive);
        if (strcmp(r->id, "ppi8255") == 0) {
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive);
            if (!r->mmio) { seen_ppi_port = true; TEST_ASSERT_EQUAL_UINT(MZ800_BUS_PRESENT_MODE800, r->present_kind); }
        }
        if (strcmp(r->id, "pioz80") == 0) { seen_pio = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_TRISTATE, r->drive); }
        if (strcmp(r->id, "joy0") == 0 || strcmp(r->id, "joy1") == 0) {
            seen_joy = true;
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_PARTIAL, r->drive);
            TEST_ASSERT_EQUAL_HEX8(0x3F, r->drive_mask);
            TEST_ASSERT_EQUAL_UINT(BUS_DIR_R, r->dirs);
            TEST_ASSERT_NOT_NULL(strstr(r->source_note, "74LS365"));
        }
        if (strcmp(r->id, "gdg") == 0) {
            if (r->dirs & BUS_DIR_R) { seen_status = true; TEST_ASSERT_EQUAL(DEVBUS_DRIVE_UNKNOWN, r->drive); }
            else TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
        }
        if (strcmp(r->id, "psg") == 0 || strcmp(r->id, "psg_stereo") == 0) {
            seen_psg = true;
            TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
            TEST_ASSERT_EQUAL_UINT(BUS_DIR_W, r->dirs);
        }
        if (strcmp(r->id, "psg_stereo") == 0) TEST_ASSERT_EQUAL_UINT(MZ800_BUS_PRESENT_PSG_STEREO, r->present_kind);
        if (strcmp(r->id, "mapper") == 0) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_NONE, r->drive);
        if (strcmp(r->id, "fdc0") == 0 && (r->dirs & BUS_DIR_R)) TEST_ASSERT_EQUAL(DEVBUS_DRIVE_UNKNOWN, r->drive);
        /* žádný řádek neodhaduje: TRISTATE jen s poznámkou o datasheetu / VHDL */
        if (r->drive == DEVBUS_DRIVE_TRISTATE) {
            TEST_ASSERT_TRUE(strstr(r->source_note, "datasheet") || strstr(r->source_note, "VHDL"));
        }
    }
    TEST_ASSERT_TRUE(seen_joy && seen_status && seen_pio && seen_psg && seen_ppi_port);
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
