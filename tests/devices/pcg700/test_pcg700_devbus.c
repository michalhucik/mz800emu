/**
 * @file   test_pcg700_devbus.c
 * @brief  Testy lepidla PCG-700 pro devbus (pcg700_devbus.c).
 *
 * Testuje napojení modelu pcg700_core na device API:
 *   - registraci (deskriptor, volby, výchozí hodnoty, INI),
 *   - MMIO 0E010h-0E012h: sekvence z manuálu přes devbus_mmio_dispatch(),
 *     dekód jen s mapovanou horní oblastí, volba decode_dram (maska DRAM),
 *     0E013h se nedekóduje,
 *   - zdroj glyfů (efektivní generátor vs. CG-ROM podle bitu 3, HSW, GSW),
 *   - reset (latche, RAM zůstává), rom_changed, region PCG RAM a jeho
 *     on_write, katalog I/O,
 *   - snapshot round-trip komponenty devbus včetně voleb.
 *
 * Běží v testovacím jádru MZ-800 s falešným adaptérem (okna MMIO jako
 * MZ-700); lepidlo závisí jen na arch-neutrálním devbus a na g_rom.cgrom,
 * kterou test nahrazuje syntetickou CG-ROM.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "emulator.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshot_mgr.h"
#include "hw-generic/memory/rom.h"
#include "hw-generic/devbus/devbus.h"
#include "mzarch/mz700/devices/pcg700/pcg700_devbus.h"


/** @brief Počet volání video_invalidate adaptéru. */
static unsigned s_invalidate;

static void fake_hooks_changed(void) { }
static void fake_invalidate(void) { s_invalidate++; }

/** @brief Falešný adaptér (okna MMIO jako MZ-700, bez statických portů). */
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, NULL, fake_hooks_changed, NULL, NULL, fake_invalidate, NULL,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
};

/** @brief Syntetická CG-ROM (každý bajt jiný) a druhá pro rom_changed. */
static uint8_t s_cgrom[PCG700_CGROM_SIZE];
static uint8_t s_cgrom2[PCG700_CGROM_SIZE];
static uint8_t *s_saved_cgrom;

static char *s_ini_path;
static struct st_CFGROOT *s_saved_root;
static struct st_CFGROOT *s_tmp_root;
static int s_slot;

/** @brief Vzor z BASIC příkladu manuálu (formát PCG-700, bit 7 = levý bod). */
static const uint8_t c_GLYPH[8] = { 16, 126, 20, 62, 85, 73, 50, 0 };


/** @brief Zápis CPU s mapovanou horní oblastí. */
static void wr(uint16_t addr, uint8_t v)
{
    devbus_mmio_dispatch(addr, v, DEVBUS_MMIO_MAPPED | DEVBUS_ACC_CPU);
}

/** @brief Zápis CPU s DRAM na 0E000h (po OUT 0E1h). */
static void wr_dram(uint16_t addr, uint8_t v)
{
    devbus_mmio_dispatch(addr, v, DEVBUS_MMIO_DRAM | DEVBUS_ACC_CPU);
}

/** @brief Zapíše jeden bajt postupem BASIC příkladu manuálu (uvolnění zápisem 0). */
static void basic_write(unsigned a, uint8_t data)
{
    wr(0xE010, data);
    wr(0xE011, (uint8_t) (a & 0xFF));
    wr(0xE012, (uint8_t) ((a >> 8) | 0x10));
    wr(0xE012, 0x00);
}

/** @brief Index bajtu v generátoru ve formátu CG-ROM pro kód @p code sady @p set. */
static unsigned cg_index(unsigned set, unsigned code, unsigned row)
{
    return (set ? 0x800u : 0u) + code * 8u + row;
}

/** @brief Spustí devbus s PCG-700 a znovu zaregistruje komponenty snapshotu. */
static void start_devbus(void)
{
    devbus_init(&s_fake_ops);
    s_slot = pcg700_devbus_register();
    TEST_ASSERT_EQUAL_INT(0, s_slot);
    devbus_start();
    snapshot_init();
}

static void stop_devbus(void)
{
    devbus_exit();
    snapshot_init();
}

void setUp(void)
{
    for (unsigned i = 0; i < PCG700_CGROM_SIZE; i++) {
        s_cgrom[i] = (uint8_t) ((i * 7u) ^ (i >> 8) ^ 0x5Au);
        s_cgrom2[i] = (uint8_t) ~s_cgrom[i];
    }
    s_saved_cgrom = g_rom.cgrom;
    g_rom.cgrom = s_cgrom;
    s_invalidate = 0;
    s_ini_path = g_build_filename(g_get_tmp_dir(), "mz_pcg700_devbus_test.ini", NULL);
    g_remove(s_ini_path);
    s_saved_root = g_cfgmain;
    s_tmp_root = cfgroot_new(s_ini_path);
    g_cfgmain = s_tmp_root;
    g_emulator.paused = true;
    start_devbus();
}

void tearDown(void)
{
    stop_devbus();
    g_cfgmain = s_saved_root;
    cfgroot_destroy(s_tmp_root);
    g_remove(s_ini_path);
    g_free(s_ini_path);
    g_rom.cgrom = s_saved_cgrom;
    g_emulator.paused = false;
}


/* ===================== registrace ===================== */

void test_register_descriptor_and_defaults(void)
{
    const st_DEVBUS_DEVICE_DESC *d = devbus_desc(s_slot);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("pcg700", d->id);
    TEST_ASSERT_EQUAL_STRING("PCG700", d->ini_section);
    TEST_ASSERT_EQUAL_UINT(PCG700_OPT_COUNT, d->option_count);
    TEST_ASSERT_FALSE(devbus_is_connected(s_slot));

    st_PCG700_CONFIG cfg, def;
    pcg700_devbus_config_from_options(s_slot, &cfg);
    pcg700_core_config_default(&def);
    TEST_ASSERT_EQUAL_MEMORY(&def, &cfg, sizeof(cfg));
    TEST_ASSERT_EQUAL_UINT(0, devbus_option_get(s_slot, PCG700_OPT_DECODE_DRAM));
    TEST_ASSERT_EQUAL_UINT(DEVBUS_MMIO_MAPPED, devbus_mmio_get_when(s_slot, 0));

    /* Klíče INI a klíčová slova */
    TEST_ASSERT_EQUAL_INT(PCG700_OPT_STROBE, devbus_option_find(s_slot, "strobe"));
    unsigned v = 0;
    TEST_ASSERT_TRUE(devbus_option_parse(s_slot, PCG700_OPT_HSW, "nom", &v));
    TEST_ASSERT_EQUAL_UINT(PCG700_HSW_NOM, v);
    TEST_ASSERT_TRUE(devbus_option_parse(s_slot, PCG700_OPT_STROBE, "falling", &v));
    TEST_ASSERT_EQUAL_UINT(PCG700_STROBE_FALLING, v);
    TEST_ASSERT_TRUE(devbus_option_parse(s_slot, PCG700_OPT_SSWOFF, "force_off", &v));
    TEST_ASSERT_EQUAL_UINT(PCG700_SSWOFF_FORCE_OFF, v);
    TEST_ASSERT_TRUE(devbus_option_parse(s_slot, PCG700_OPT_POWERON_RAM, "cgrom", &v));
    TEST_ASSERT_EQUAL_UINT(PCG700_POWERON_RAM_CGROM, v);

    /* Odpojené zařízení nemá háčky */
    TEST_ASSERT_NULL(g_devbus_hooks.glyph);
    TEST_ASSERT_FALSE(g_devbus_hooks.mmio);
}

void test_connect_installs_hooks_display_off(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    TEST_ASSERT_NOT_NULL(g_devbus_hooks.glyph);
    TEST_ASSERT_TRUE(g_devbus_hooks.mmio);
    TEST_ASSERT_FALSE(g_devbus_hooks.ports);
    /* Po zapnutí je bit 3 = 1: kreslí se z CG-ROM */
    TEST_ASSERT_EQUAL_PTR(s_cgrom, g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx));
    TEST_ASSERT_EQUAL_HEX8(PCG700_CTRL_RESET_VALUE, pcg700_devbus_instance()->ctrl);
}


/* ===================== MMIO ===================== */

void test_manual_sequence_writes_glyph_and_enables(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    /* Kód 80h sady 1, adresa = řádek */
    for (unsigned r = 0; r < 8; r++) basic_write(r, c_GLYPH[r]);
    const uint8_t *ram = devbus_region(s_slot, 0)->data(devbus_ctx(s_slot));
    for (unsigned r = 0; r < 8; r++) {
        TEST_ASSERT_EQUAL_HEX8(pcg700_core_bitrev(c_GLYPH[r]), ram[r]);
    }
    /* Uvolnění zápisem 0 nechalo bit 3 = 0: PCG zapnuté (SSW ON) */
    const uint8_t *cg = g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx);
    TEST_ASSERT_TRUE(cg != s_cgrom);
    for (unsigned r = 0; r < 8; r++) {
        TEST_ASSERT_EQUAL_HEX8(pcg700_core_bitrev(c_GLYPH[r]), cg[cg_index(0, 0x80, r)]);
    }
    /* Kódy 00h-7Fh a nepřepsané glyfy zůstávají z CG-ROM / RAM */
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(0, 0x41, 3)], cg[cg_index(0, 0x41, 3)]);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(1, 0x10, 5)], cg[cg_index(1, 0x10, 5)]);
    TEST_ASSERT_EQUAL_HEX8(0x00, cg[cg_index(1, 0x80, 0)]);
    TEST_ASSERT_TRUE(s_invalidate > 0);

    /* POKE 0E012h,8 vypne */
    wr(0xE012, 0x08);
    TEST_ASSERT_EQUAL_PTR(s_cgrom, g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx));
}

void test_copy_from_rom_set2(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    /* Sada 2, kód 0C1h, řádek 2: adresa 400h + 41h * 8 + 2 */
    unsigned a = 0x400 + 0x41 * 8 + 2;
    wr(0xE011, (uint8_t) (a & 0xFF));
    wr(0xE012, (uint8_t) ((a >> 8) | 0x30));
    wr(0xE012, 0x00);
    const uint8_t *cg = g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(1, 0xC1, 2)], cg[cg_index(1, 0xC1, 2)]);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(1, 0xC1, 2)], pcg700_devbus_instance()->ram[a]);
}

void test_dram_state_ignored_unless_option(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    wr_dram(0xE010, 0xAA);
    wr_dram(0xE012, 0x10);
    TEST_ASSERT_EQUAL_HEX8(0x00, pcg700_devbus_instance()->data);
    TEST_ASSERT_EQUAL_HEX8(PCG700_CTRL_RESET_VALUE, pcg700_devbus_instance()->ctrl);

    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_DECODE_DRAM, 1));
    TEST_ASSERT_EQUAL_UINT(DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM, devbus_mmio_get_when(s_slot, 0));
    wr_dram(0xE010, 0xAA);
    TEST_ASSERT_EQUAL_HEX8(0xAA, pcg700_devbus_instance()->data);
    wr(0xE010, 0x55);
    TEST_ASSERT_EQUAL_HEX8(0x55, pcg700_devbus_instance()->data);

    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_DECODE_DRAM, 0));
    TEST_ASSERT_EQUAL_UINT(DEVBUS_MMIO_MAPPED, devbus_mmio_get_when(s_slot, 0));
}

void test_only_three_addresses_decoded(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    wr(0xE013, 0x00);  /* zrcadlo 0E012h by PCG zapnulo */
    wr(0xE00F, 0x00);
    TEST_ASSERT_EQUAL_HEX8(PCG700_CTRL_RESET_VALUE, pcg700_devbus_instance()->ctrl);
    TEST_ASSERT_TRUE(devbus_mmio_claims(0xE010));
    TEST_ASSERT_TRUE(devbus_mmio_claims(0xE012));
    TEST_ASSERT_FALSE(devbus_mmio_claims(0xE013));
}

void test_debugger_write_acts_like_bus(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    devbus_mmio_dispatch(0xE012, 0x00, DEVBUS_MMIO_MAPPED | DEVBUS_ACC_DEBUGGER);
    TEST_ASSERT_EQUAL_HEX8(0x00, pcg700_devbus_instance()->ctrl);
}


/* ===================== volby ===================== */

void test_live_options_switches(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    basic_write(0, 0xFF);
    TEST_ASSERT_TRUE(g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) != s_cgrom);

    /* HSW NOM: kreslí se z CG-ROM, zápisy dál fungují */
    unsigned inv = s_invalidate;
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_HSW, PCG700_HSW_NOM));
    TEST_ASSERT_TRUE(s_invalidate > inv);
    TEST_ASSERT_EQUAL_PTR(s_cgrom, g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx));
    basic_write(1, 0x81);
    TEST_ASSERT_EQUAL_HEX8(0x81, pcg700_devbus_instance()->ram[1]);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_HSW, PCG700_HSW_PCG));
    TEST_ASSERT_TRUE(g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) != s_cgrom);

    /* SSW OFF + force_off: vypnuto i s bitem 3 = 0 */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_SSW, PCG700_SSW_OFF));
    TEST_ASSERT_TRUE(g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) != s_cgrom);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_SSWOFF, PCG700_SSWOFF_FORCE_OFF));
    TEST_ASSERT_EQUAL_PTR(s_cgrom, g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_SSW, PCG700_SSW_ON));

    /* GSW OFF: zápisy se ignorují celé */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_GSW, PCG700_GSW_OFF));
    wr(0xE012, 0x08);
    TEST_ASSERT_EQUAL_HEX8(0x00, pcg700_devbus_instance()->ctrl);

    /* Strobe úrovní: opakovaný zápis 0E012h s bitem 4 zapisuje znovu */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_GSW, PCG700_GSW_ON));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_STROBE, PCG700_STROBE_LEVEL));
    TEST_ASSERT_EQUAL_INT(PCG700_STROBE_LEVEL, pcg700_devbus_instance()->cfg.strobe);
}

void test_poweron_ram_applies_on_connect(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_POWERON_RAM, PCG700_POWERON_RAM_CGROM));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(0, 0x80, 0)], pcg700_devbus_instance()->ram[0]);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(1, 0xFF, 7)], pcg700_devbus_instance()->ram[0x7FF]);

    /* Změna za připojení se uplatní až při dalším připojení */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_POWERON_RAM, PCG700_POWERON_RAM_ZERO));
    TEST_ASSERT_EQUAL_HEX8(s_cgrom[cg_index(0, 0x80, 0)], pcg700_devbus_instance()->ram[0]);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_disconnect(s_slot));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    TEST_ASSERT_EQUAL_HEX8(0x00, pcg700_devbus_instance()->ram[0]);
}


/* ===================== reset, ROM, region, katalog ===================== */

void test_reset_keeps_ram_disables_display(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    basic_write(5, 0x0F);
    TEST_ASSERT_TRUE(g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) != s_cgrom);
    unsigned inv = s_invalidate;
    devbus_reset_all();
    TEST_ASSERT_TRUE(s_invalidate > inv);
    TEST_ASSERT_EQUAL_HEX8(PCG700_CTRL_RESET_VALUE, pcg700_devbus_instance()->ctrl);
    TEST_ASSERT_EQUAL_PTR(s_cgrom, g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx));
    TEST_ASSERT_EQUAL_HEX8(0xF0, pcg700_devbus_instance()->ram[5]);
}

void test_rom_changed_rebuilds_generator(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    wr(0xE012, 0x00);
    g_rom.cgrom = s_cgrom2;
    devbus_rom_changed();
    const uint8_t *cg = g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom2[cg_index(0, 0x41, 1)], cg[cg_index(0, 0x41, 1)]);
    TEST_ASSERT_EQUAL_HEX8(s_cgrom2[cg_index(1, 0x7F, 7)], cg[cg_index(1, 0x7F, 7)]);
    TEST_ASSERT_EQUAL_HEX8(0x00, cg[cg_index(0, 0x80, 0)]);
}

void test_region_write_updates_generator(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    wr(0xE012, 0x00);
    const st_DEVBUS_REGION *r = devbus_region(s_slot, 0);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT32(PCG700_RAM_SIZE, r->size);
    TEST_ASSERT_TRUE(r->writable);
    TEST_ASSERT_NOT_NULL(strstr(r->name, "PCG-700"));
    uint8_t *ram = r->data(devbus_ctx(s_slot));
    ram[0x400 + 3] = 0x3C;  /* sada 2, kód 80h, řádek 3 */
    unsigned inv = s_invalidate;
    r->on_write(devbus_ctx(s_slot), 0x403, 1);
    TEST_ASSERT_TRUE(s_invalidate > inv);
    const uint8_t *cg = g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx);
    TEST_ASSERT_EQUAL_HEX8(0x3C, cg[cg_index(1, 0x80, 3)]);
}

void test_io_catalog_entries(void)
{
    TEST_ASSERT_EQUAL_UINT(3, devbus_io_catalog_total());
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    wr(0xE011, 0x42);
    for (size_t i = 0; i < 3; i++) {
        int owner = -1;
        const st_IO_PORT_DESC *e = devbus_io_catalog_at(i, &owner);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_INT(s_slot, owner);
        TEST_ASSERT_EQUAL_UINT16(0xE010 + i, e->addr);
        TEST_ASSERT_EQUAL_INT(0, strncmp(e->name, "Device - ", 9));
        TEST_ASSERT_NOT_NULL(e->read_value);
    }
    TEST_ASSERT_EQUAL_HEX8(0x42, devbus_io_catalog_at(1, NULL)->read_value());
}


/* ===================== snapshot ===================== */

void test_snapshot_round_trip_with_options(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_STROBE, PCG700_STROBE_LEVEL));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_DECODE_DRAM, 1));
    for (unsigned r = 0; r < 8; r++) basic_write(0x400 + r, c_GLYPH[r]);
    st_PCG700 saved = *pcg700_devbus_instance();

    uint8_t *buf = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("pcg700", &buf, &size));

    /* Rozházet stav i volby, pak načíst */
    wr(0xE012, 0x08);
    basic_write(0x401, 0x00);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_STROBE, PCG700_STROBE_RISING));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(s_slot, PCG700_OPT_DECODE_DRAM, 0));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_disconnect(s_slot));

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);

    TEST_ASSERT_TRUE(devbus_is_connected(s_slot));
    const st_PCG700 *p = pcg700_devbus_instance();
    TEST_ASSERT_EQUAL_HEX8(saved.ctrl, p->ctrl);
    TEST_ASSERT_EQUAL_HEX8(saved.data, p->data);
    TEST_ASSERT_EQUAL_HEX8(saved.addr_lo, p->addr_lo);
    TEST_ASSERT_EQUAL_MEMORY(saved.ram, p->ram, PCG700_RAM_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(saved.effective, p->effective, PCG700_CGROM_SIZE);
    TEST_ASSERT_EQUAL_INT(PCG700_STROBE_LEVEL, p->cfg.strobe);
    TEST_ASSERT_EQUAL_UINT(DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM, devbus_mmio_get_when(s_slot, 0));
    TEST_ASSERT_TRUE(g_devbus_hooks.glyph(g_devbus_hooks.glyph_ctx) != s_cgrom);
}

void test_snapshot_without_device_disconnects(void)
{
    uint8_t *buf = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("nodev", &buf, &size));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(s_slot));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
    TEST_ASSERT_FALSE(devbus_is_connected(s_slot));
    TEST_ASSERT_NULL(g_devbus_hooks.glyph);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_register_descriptor_and_defaults);
    RUN_TEST(test_connect_installs_hooks_display_off);
    RUN_TEST(test_manual_sequence_writes_glyph_and_enables);
    RUN_TEST(test_copy_from_rom_set2);
    RUN_TEST(test_dram_state_ignored_unless_option);
    RUN_TEST(test_only_three_addresses_decoded);
    RUN_TEST(test_debugger_write_acts_like_bus);
    RUN_TEST(test_live_options_switches);
    RUN_TEST(test_poweron_ram_applies_on_connect);
    RUN_TEST(test_reset_keeps_ram_disables_display);
    RUN_TEST(test_rom_changed_rebuilds_generator);
    RUN_TEST(test_region_write_updates_generator);
    RUN_TEST(test_io_catalog_entries);
    RUN_TEST(test_snapshot_round_trip_with_options);
    RUN_TEST(test_snapshot_without_device_disconnects);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
