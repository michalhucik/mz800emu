/**
 * @file test_devbus.c
 * @brief Jádro devbus: registr, připojení, dispečerské tabulky, volby a INI,
 *        události životního cyklu, sekundární video výstup.
 *
 * Testy běží v testovacím jádru MZ-800 (hw-generic/devbus se přeloží všude).
 * Adaptér platformy nahrazuje falešný st_DEVBUS_ARCH_OPS, který zaznamená
 * hooks_changed() a vrací statické porty podle MZ-700 (výběr: 01h-02h,
 * 50h-51h, 68h-6Fh, 0E0h-0E7h). Testovací zařízení je devbus_dummy.c.
 *
 * Plán D1, testy 1-4 a 7 (část reset a vout).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "hw-generic/devbus/devbus.h"
#include "devbus_dummy.h"


/** @brief Počet volání hooks_changed() falešného adaptéru. */
static unsigned s_hooks_changed;
/** @brief Součet T-stavů z cpu_wait() falešného adaptéru. */
static unsigned s_wait_sum;
/** @brief Počet volání mmio_sync() falešného adaptéru. */
static unsigned s_mmio_sync;
/** @brief Počet volání video_invalidate() falešného adaptéru. */
static unsigned s_invalidate;

static bool fake_port_is_static(uint8_t p)
{
    return (p >= 0x01 && p <= 0x02) || (p >= 0x50 && p <= 0x51) ||
           (p >= 0x68 && p <= 0x6f) || (p >= 0xe0 && p <= 0xe7);
}

static void fake_hooks_changed(void) { s_hooks_changed++; }
static void fake_cpu_wait(unsigned t) { s_wait_sum += t; }
static uint64_t fake_time_ticks(void) { return 123456789ull; }
static void fake_invalidate(void) { s_invalidate++; }
static void fake_mmio_sync(void) { s_mmio_sync++; }

/** @brief Falešný adaptér (okna MMIO jako MZ-700). */
static const st_DEVBUS_ARCH_OPS s_fake_ops = {
    DEVBUS_PLATFORM_MZ700, fake_port_is_static, fake_hooks_changed, fake_cpu_wait,
    fake_time_ticks, fake_invalidate, fake_mmio_sync,
    0xE009, 0xEFFF, 0xE000, 0xEFFF,
};

/** @brief Dummy zařízení A, B (B koliduje s A porty i MMIO). */
static st_DEVBUS_DUMMY s_a, s_b;
/** @brief Deskriptory (musí žít do konce procesu). */
static st_DEVBUS_DEVICE_DESC s_desc_a, s_desc_b, s_desc_tmp;

/** @brief Dočasný INI soubor a kořen konfigurace (test 4). */
static char *s_ini_path;
static struct st_CFGROOT *s_saved_root;
static struct st_CFGROOT *s_tmp_root;


/** @brief Nainstaluje dočasný kořen INI s obsahem @p ini (NULL = bez souboru). */
static void use_tmp_ini(const char *ini)
{
    s_ini_path = g_build_filename(g_get_tmp_dir(), "mz_devbus_test.ini", NULL);
    g_remove(s_ini_path);
    if (ini) TEST_ASSERT_TRUE(g_file_set_contents(s_ini_path, ini, -1, NULL));
    s_saved_root = g_cfgmain;
    s_tmp_root = cfgroot_new(s_ini_path);
    g_cfgmain = s_tmp_root;
}

/** @brief Vrátí původní kořen INI (po devbus_exit()). */
static void drop_tmp_ini(void)
{
    if (!s_tmp_root) return;
    g_cfgmain = s_saved_root;
    cfgroot_destroy(s_tmp_root);
    s_tmp_root = NULL;
    g_remove(s_ini_path);
    g_free(s_ini_path);
    s_ini_path = NULL;
}

void setUp(void)
{
    s_hooks_changed = s_wait_sum = s_mmio_sync = s_invalidate = 0;
    memset(&s_a, 0, sizeof(s_a));
    memset(&s_b, 0, sizeof(s_b));
    devbus_dummy_make_desc(&s_desc_a, "dummy", "DUMMY", 0);
    devbus_dummy_make_desc(&s_desc_b, "dummyb", "DUMMYB", 0);
    use_tmp_ini(NULL);
    devbus_init(&s_fake_ops);
}

void tearDown(void)
{
    devbus_exit();
    drop_tmp_ini();
}

/** @brief Zaregistruje A (a volitelně B) a uloží sloty do kontextů. */
static void reg_ab(bool with_b)
{
    s_a.slot = devbus_register(&s_desc_a, &s_a);
    TEST_ASSERT_EQUAL_INT(0, s_a.slot);
    if (with_b) {
        s_b.slot = devbus_register(&s_desc_b, &s_b);
        TEST_ASSERT_EQUAL_INT(1, s_b.slot);
    }
}


/* ===================== 1. registr ===================== */

void test_registry_basic(void)
{
    TEST_ASSERT_TRUE(devbus_is_initialized());
    reg_ab(false);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_init);
    TEST_ASSERT_EQUAL_UINT(1, devbus_count());
    TEST_ASSERT_EQUAL_INT(0, devbus_find("dummy"));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_find("nothere"));
    TEST_ASSERT_EQUAL_PTR(&s_desc_a, devbus_desc(0));
    TEST_ASSERT_EQUAL_PTR(&s_a, devbus_ctx(0));
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(0, s_hooks_changed);
    /* Duplicitní id */
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_EXISTS, devbus_register(&s_desc_a, &s_a));
}

void test_registry_uninitialized_and_after_start(void)
{
    devbus_exit();
    TEST_ASSERT_FALSE(devbus_is_initialized());
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_UNSUPPORTED, devbus_register(&s_desc_a, &s_a));
    devbus_init(&s_fake_ops);
    devbus_start();
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_a, &s_a));
}

void test_registry_full(void)
{
    static st_DEVBUS_DEVICE_DESC descs[DEVBUS_MAX_DEVICES + 1];
    static st_DEVBUS_DUMMY ctx[DEVBUS_MAX_DEVICES + 1];
    static char ids[DEVBUS_MAX_DEVICES + 1][8], secs[DEVBUS_MAX_DEVICES + 1][8];
    for (int i = 0; i <= DEVBUS_MAX_DEVICES; i++) {
        g_snprintf(ids[i], sizeof(ids[i]), "dev%d", i);
        g_snprintf(secs[i], sizeof(secs[i]), "DEV%d", i);
        devbus_dummy_make_desc(&descs[i], ids[i], secs[i], 0);
        int rc = devbus_register(&descs[i], &ctx[i]);
        if (i < DEVBUS_MAX_DEVICES) TEST_ASSERT_EQUAL_INT(i, rc);
        else TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_FULL, rc);
    }
}

void test_registry_invalid_desc(void)
{
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(NULL, NULL));

    s_desc_tmp = s_desc_a;
    s_desc_tmp.id = "Bad-Id";
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));

    s_desc_tmp = s_desc_a;
    s_desc_tmp.ini_section = "lower";
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));

    s_desc_tmp = s_desc_a;
    s_desc_tmp.platforms = DEVBUS_PLATFORM_MZ800;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_UNSUPPORTED, devbus_register(&s_desc_tmp, &s_a));

    s_desc_tmp = s_desc_a;
    s_desc_tmp.snap.version = 0;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));

    /* Volba s rezervovaným klíčem a s neplatnou výchozí hodnotou */
    static const st_DEVBUS_OPTION bad_key[] = {
        { "connected", "C", DEVBUS_OPT_BOOL, 0, 0, 0, NULL, DEVBUS_APPLY_LIVE } };
    static const st_DEVBUS_OPTION bad_def[] = {
        { "num", "N", DEVBUS_OPT_UNSIGNED, 9, 0, 5, NULL, DEVBUS_APPLY_LIVE } };
    s_desc_tmp = s_desc_a;
    s_desc_tmp.options = bad_key;
    s_desc_tmp.option_count = 1;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    s_desc_tmp.options = bad_def;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));

    /* Popisky hodnot: jen u KEYWORD a pro každé klíčové slovo */
    static const char *const words[] = { "a", "b", NULL };
    static const char *const labels_short[] = { "A", NULL };
    static const char *const labels_ok[] = { "A", "B" };
    static const st_DEVBUS_OPTION bad_labels_count[] = {
        { "kw", "K", DEVBUS_OPT_KEYWORD, 0, 0, 0, words, DEVBUS_APPLY_LIVE, labels_short } };
    static const st_DEVBUS_OPTION bad_labels_type[] = {
        { "flag", "F", DEVBUS_OPT_BOOL, 0, 0, 0, NULL, DEVBUS_APPLY_LIVE, labels_ok } };
    static const st_DEVBUS_OPTION good_labels[] = {
        { "kw", "K", DEVBUS_OPT_KEYWORD, 1, 0, 0, words, DEVBUS_APPLY_LIVE, labels_ok } };
    s_desc_tmp.options = bad_labels_count;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    s_desc_tmp.options = bad_labels_type;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    s_desc_tmp.options = good_labels;
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&s_desc_tmp, &s_a));
    devbus_exit();
    devbus_init(&s_fake_ops);

    /* Prázdné video háčky */
    static const st_DEVBUS_VIDEO_OPS empty_video = { NULL, NULL };
    s_desc_tmp = s_desc_a;
    s_desc_tmp.video = &empty_video;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));

    /* init() selže: slot se uvolní */
    s_a.fail_init = true;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_DEVICE, devbus_register(&s_desc_a, &s_a));
    TEST_ASSERT_EQUAL_UINT(0, devbus_count());
    s_a.fail_init = false;
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&s_desc_a, &s_a));
}

void test_registry_static_ports_rejected(void)
{
    static const st_DEVBUS_PORT_RANGE p_static[] = { { 0x4f, 0x50, NULL, NULL, NULL } };
    s_desc_tmp = s_desc_a;
    s_desc_tmp.ports = p_static;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    /* Překryv rozsahů téhož zařízení */
    static const st_DEVBUS_PORT_RANGE p_overlap[] = {
        { 0x70, 0x72, NULL, NULL, NULL }, { 0x72, 0x73, NULL, NULL, NULL } };
    s_desc_tmp.ports = p_overlap;
    s_desc_tmp.port_count = 2;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
}

static void nop_mmio(void *c, uint16_t a, uint8_t v, unsigned f) { (void) c; (void) a; (void) v; (void) f; }

void test_registry_mmio_window(void)
{
    /* MAPPED nesmí zasahovat do 0E000h-0E008h (8255, 8253, GDG) */
    static const st_DEVBUS_MMIO_RANGE m_low[] = { { 0xE008, 0xE009, DEVBUS_MMIO_MAPPED, nop_mmio } };
    /* DRAM smí od 0E000h */
    static const st_DEVBUS_MMIO_RANGE m_dram[] = { { 0xE000, 0xE001, DEVBUS_MMIO_DRAM, nop_mmio } };
    /* Mimo stránku 0E000h */
    static const st_DEVBUS_MMIO_RANGE m_out[] = { { 0xEFFF, 0xF000, DEVBUS_MMIO_DRAM, nop_mmio } };
    s_desc_tmp = s_desc_a;
    s_desc_tmp.mmio_count = 1;
    s_desc_tmp.mmio = m_low;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    s_desc_tmp.mmio = m_out;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    s_desc_tmp.mmio = m_dram;
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&s_desc_tmp, &s_a));
}


/* ===================== 2. připojení ===================== */

void test_connect_disconnect(void)
{
    reg_ab(false);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(1, devbus_connected_count());
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_connect);
    TEST_ASSERT_EQUAL_UINT(1, s_hooks_changed);
    TEST_ASSERT_TRUE(g_devbus_hooks.ports);
    TEST_ASSERT_TRUE(g_devbus_hooks.mmio);
    TEST_ASSERT_EQUAL_PTR(&s_desc_a.ports[0], g_devbus_ports[0x74].range);
    TEST_ASSERT_EQUAL_PTR(&s_a, g_devbus_ports[0x75].ctx);
    TEST_ASSERT_NULL(g_devbus_ports[0x76].range);
    /* RAM podle volby "fill" (výchozí 0AAh) */
    TEST_ASSERT_EQUAL_HEX8(0xAA, s_a.ram[17]);

    /* Opakované připojení nic nemění */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_connect);
    TEST_ASSERT_EQUAL_UINT(1, s_hooks_changed);

    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_disconnect(0));
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_disconnect);
    TEST_ASSERT_EQUAL_UINT(2, s_hooks_changed);
    TEST_ASSERT_FALSE(g_devbus_hooks.ports);
    TEST_ASSERT_FALSE(g_devbus_hooks.mmio);
    TEST_ASSERT_NULL(g_devbus_ports[0x74].range);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_disconnect(0));
    TEST_ASSERT_EQUAL_UINT(2, s_hooks_changed);

    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_connect(5));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_disconnect(-1));
}

void test_connect_conflicts(void)
{
    reg_ab(true);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    /* B má stejné porty i MMIO */
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_CONFLICT, devbus_connect(1));
    TEST_ASSERT_EQUAL_UINT(0, s_b.n_connect);
    TEST_ASSERT_EQUAL_UINT(1, s_hooks_changed);
    devbus_disconnect(0);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(1));
    TEST_ASSERT_EQUAL_PTR(&s_b, g_devbus_ports[0x74].ctx);
}

void test_connect_conflict_mmio_only(void)
{
    s_desc_b.ports = NULL;
    s_desc_b.port_count = 0;
    reg_ab(true);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_CONFLICT, devbus_connect(1));
}

void test_connect_conflict_video(void)
{
    /* Dva render_row bez portů a MMIO: kolize; glyph + row spolu smějí. */
    static st_DEVBUS_DEVICE_DESC r1, r2, g1;
    static st_DEVBUS_DUMMY c1, c2, c3;
    devbus_dummy_make_desc(&r1, "row1", "ROW1", 2);
    devbus_dummy_make_desc(&r2, "row2", "ROW2", 2);
    devbus_dummy_make_desc(&g1, "glyph1", "GLYPH1", 1);
    st_DEVBUS_DEVICE_DESC *all[] = { &r1, &r2, &g1 };
    for (int i = 0; i < 3; i++) {
        all[i]->ports = NULL; all[i]->port_count = 0;
        all[i]->mmio = NULL; all[i]->mmio_count = 0;
    }
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&r1, &c1));
    TEST_ASSERT_EQUAL_INT(1, devbus_register(&r2, &c2));
    TEST_ASSERT_EQUAL_INT(2, devbus_register(&g1, &c3));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(0));
    TEST_ASSERT_NOT_NULL(g_devbus_hooks.row);
    TEST_ASSERT_EQUAL_PTR(&c1, g_devbus_hooks.row_ctx);
    TEST_ASSERT_NULL(g_devbus_hooks.glyph);
    TEST_ASSERT_FALSE(g_devbus_hooks.ports);
    TEST_ASSERT_FALSE(g_devbus_hooks.mmio);
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_CONFLICT, devbus_connect(1));
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_connect(2));
    TEST_ASSERT_NOT_NULL(g_devbus_hooks.glyph);
    TEST_ASSERT_EQUAL_PTR(&c3, g_devbus_hooks.glyph_ctx);
}

void test_connect_failure_keeps_tables_clean(void)
{
    reg_ab(false);
    s_a.fail_connect = true;
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_DEVICE, devbus_connect(0));
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    TEST_ASSERT_NULL(g_devbus_ports[0x74].range);
    TEST_ASSERT_FALSE(g_devbus_hooks.ports);
    TEST_ASSERT_EQUAL_UINT(0, s_hooks_changed);
    TEST_ASSERT_EQUAL_UINT(0, devbus_connected_count());
}


/* ===================== 3. dispečink ===================== */

void test_dispatch_ports(void)
{
    reg_ab(false);
    devbus_connect(0);
    const st_DEVBUS_PORT_SLOT *ps = &g_devbus_ports[0x74];
    ps->range->write(ps->ctx, 0x1274, 0x5C);
    TEST_ASSERT_EQUAL_HEX8(0x5C, s_a.reg);
    TEST_ASSERT_EQUAL_HEX8(0x5C, ps->range->read(ps->ctx, 0x0074));
    /* peek bez vedlejšího efektu, read s ním */
    uint8_t v = 0;
    TEST_ASSERT_TRUE(devbus_port_peek(0x0075, &v));
    TEST_ASSERT_EQUAL_HEX8(0, v);
    ps = &g_devbus_ports[0x75];
    TEST_ASSERT_EQUAL_HEX8(1, ps->range->read(ps->ctx, 0x75));
    TEST_ASSERT_TRUE(devbus_port_peek(0xAB75, &v));
    TEST_ASSERT_EQUAL_HEX8(1, v);
    TEST_ASSERT_FALSE(devbus_port_peek(0x0076, &v));
    /* WAIT přes adaptér */
    s_a.wait_on_write = 3;
    g_devbus_ports[0x74].range->write(&s_a, 0x74, 1);
    TEST_ASSERT_EQUAL_UINT(3, s_wait_sum);
    TEST_ASSERT_EQUAL_UINT64(123456789ull, devbus_time_ticks());
    devbus_video_invalidate();
    TEST_ASSERT_EQUAL_UINT(1, s_invalidate);
}

void test_dispatch_mmio_map_state(void)
{
    reg_ab(false);
    devbus_connect(0);
    /* 0E013h reaguje jen při MAPPED */
    devbus_mmio_dispatch(0xE013, 0x11, DEVBUS_MMIO_DRAM | DEVBUS_ACC_CPU);
    TEST_ASSERT_EQUAL_UINT(0, s_a.n_mmio);
    TEST_ASSERT_EQUAL_UINT(0, s_mmio_sync);
    devbus_mmio_dispatch(0xE013, 0x22, DEVBUS_MMIO_MAPPED | DEVBUS_ACC_CPU | DEVBUS_ACC_PROHIBITED);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_mmio);
    TEST_ASSERT_EQUAL_HEX16(0xE013, s_a.mmio_addr);
    TEST_ASSERT_EQUAL_HEX8(0x22, s_a.mmio_value);
    TEST_ASSERT_EQUAL_HEX(DEVBUS_MMIO_MAPPED | DEVBUS_ACC_CPU | DEVBUS_ACC_PROHIBITED, s_a.mmio_flags);
    TEST_ASSERT_EQUAL_UINT(1, s_mmio_sync);
    /* Zápis debuggeru bez synchronizace */
    devbus_mmio_dispatch(0xE013, 0x33, DEVBUS_MMIO_MAPPED | DEVBUS_ACC_DEBUGGER);
    TEST_ASSERT_EQUAL_UINT(2, s_a.n_mmio);
    TEST_ASSERT_EQUAL_UINT(1, s_mmio_sync);
    /* 0E014h reaguje jen při DRAM */
    devbus_mmio_dispatch(0xE014, 0x44, DEVBUS_MMIO_MAPPED | DEVBUS_ACC_CPU);
    TEST_ASSERT_EQUAL_UINT(2, s_a.n_mmio);
    devbus_mmio_dispatch(0xE014, 0x44, DEVBUS_MMIO_DRAM | DEVBUS_ACC_CPU);
    TEST_ASSERT_EQUAL_UINT(3, s_a.n_mmio);
    /* Mimo rozsah */
    devbus_mmio_dispatch(0xE015, 0x55, DEVBUS_MMIO_DRAM | DEVBUS_MMIO_MAPPED | DEVBUS_ACC_CPU);
    TEST_ASSERT_EQUAL_UINT(3, s_a.n_mmio);
    TEST_ASSERT_TRUE(devbus_mmio_claims(0xE013));
    TEST_ASSERT_TRUE(devbus_mmio_claims(0xE014));
    TEST_ASSERT_FALSE(devbus_mmio_claims(0xE015));
}

void test_dispatch_mmio_set_when(void)
{
    reg_ab(false);
    devbus_connect(0);
    unsigned h0 = s_hooks_changed;
    /* 0E013h i v DRAM: obálka už je potřeba, hooks_changed se nevolá */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_mmio_set_when(0, 0, DEVBUS_MMIO_MAPPED | DEVBUS_MMIO_DRAM));
    TEST_ASSERT_EQUAL_UINT(h0, s_hooks_changed);
    devbus_mmio_dispatch(0xE013, 1, DEVBUS_MMIO_DRAM | DEVBUS_ACC_CPU);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_mmio);
    /* Oba rozsahy vypnout: potřeba obálky zaniká -> hooks_changed */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_mmio_set_when(0, 0, 0));
    TEST_ASSERT_EQUAL_UINT(h0, s_hooks_changed);
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_mmio_set_when(0, 1, 0));
    TEST_ASSERT_EQUAL_UINT(h0 + 1, s_hooks_changed);
    TEST_ASSERT_FALSE(g_devbus_hooks.mmio);
    TEST_ASSERT_EQUAL_UINT(0, devbus_mmio_get_when(0, 1));
    /* Sledování v debuggeru nezávisí na masce */
    TEST_ASSERT_TRUE(devbus_mmio_claims(0xE013));
    /* Neplatné: neexistující rozsah, neznámý bit masky */
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_mmio_set_when(0, 2, DEVBUS_MMIO_DRAM));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_mmio_set_when(0, 0, 0x80));
}

void test_dispatch_mmio_when_window(void)
{
    /* Rozsah 0E000h smí jen DRAM; přepnutí do MAPPED je mimo okno */
    static const st_DEVBUS_MMIO_RANGE m[] = { { 0xE000, 0xE000, DEVBUS_MMIO_DRAM, nop_mmio } };
    s_desc_tmp = s_desc_a;
    s_desc_tmp.mmio = m;
    s_desc_tmp.mmio_count = 1;
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&s_desc_tmp, &s_a));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_mmio_set_when(0, 0, DEVBUS_MMIO_MAPPED));
}


/* ===================== 4. volby a INI ===================== */

void test_options_defaults_and_set(void)
{
    reg_ab(false);
    TEST_ASSERT_EQUAL_UINT(0, devbus_option_get(0, DUMMY_OPT_FLAG));
    TEST_ASSERT_EQUAL_UINT(1, devbus_option_get(0, DUMMY_OPT_MODE));
    TEST_ASSERT_EQUAL_UINT(0xAA, devbus_option_get(0, DUMMY_OPT_FILL));
    TEST_ASSERT_EQUAL_INT(DUMMY_OPT_MODE, devbus_option_find(0, "mode"));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_option_find(0, "nope"));

    /* Odpojené: LIVE se jen uloží */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(0, DUMMY_OPT_FLAG, 1));
    TEST_ASSERT_EQUAL_UINT(0, s_a.n_option_changed);
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_option_set(0, DUMMY_OPT_FLAG, 2));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_option_set(0, DUMMY_OPT_MODE, 3));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_option_set(0, DUMMY_OPT_FILL, 256));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_option_set(0, 3, 0));

    devbus_connect(0);
    /* LIVE připojeného zařízení -> option_changed */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(0, DUMMY_OPT_MODE, 2));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_option_changed);
    TEST_ASSERT_EQUAL_UINT(DUMMY_OPT_MODE, s_a.last_opt);
    TEST_ASSERT_EQUAL_UINT(2, s_a.last_opt_value);
    /* Stejná hodnota: bez volání */
    devbus_option_set(0, DUMMY_OPT_MODE, 2);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_option_changed);
    /* CONNECT: bez volání, projeví se až při příštím připojení */
    TEST_ASSERT_EQUAL_INT(DEVBUS_OK, devbus_option_set(0, DUMMY_OPT_FILL, 0x11));
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_option_changed);
    TEST_ASSERT_EQUAL_HEX8(0xAA, s_a.ram[0]);
    devbus_disconnect(0);
    devbus_connect(0);
    TEST_ASSERT_EQUAL_HEX8(0x11, s_a.ram[0]);
}

void test_options_parse_format(void)
{
    reg_ab(false);
    unsigned v = 99;
    char buf[16];
    TEST_ASSERT_TRUE(devbus_option_parse(0, DUMMY_OPT_MODE, "level", &v));
    TEST_ASSERT_EQUAL_UINT(2, v);
    TEST_ASSERT_FALSE(devbus_option_parse(0, DUMMY_OPT_MODE, "bogus", &v));
    TEST_ASSERT_TRUE(devbus_option_parse(0, DUMMY_OPT_FLAG, "true", &v));
    TEST_ASSERT_EQUAL_UINT(1, v);
    TEST_ASSERT_FALSE(devbus_option_parse(0, DUMMY_OPT_FLAG, "2", &v));
    TEST_ASSERT_TRUE(devbus_option_parse(0, DUMMY_OPT_FILL, "0x20", &v));
    TEST_ASSERT_EQUAL_UINT(0x20, v);
    TEST_ASSERT_FALSE(devbus_option_parse(0, DUMMY_OPT_FILL, "300", &v));
    TEST_ASSERT_EQUAL_STRING("rising", devbus_option_format(0, DUMMY_OPT_MODE, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("7", devbus_option_format(0, DUMMY_OPT_FILL, 7, buf, sizeof(buf)));
    unsigned opt = 9;
    TEST_ASSERT_EQUAL_INT(0, devbus_option_key("dummy", "MODE", &opt));
    TEST_ASSERT_EQUAL_UINT(DUMMY_OPT_MODE, opt);
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_option_key("DUMMY", "connected", &opt));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_option_key("DUMMY", "nope", &opt));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_NOT_FOUND, devbus_option_key("CFGMAIN", "mode", &opt));
}

void test_ini_load_and_start_connects(void)
{
    devbus_exit();
    drop_tmp_ini();
    use_tmp_ini("[DUMMY]\nconnected = 1\nflag = 1\nmode = level\nfill = 0x33\n\n");
    devbus_init(&s_fake_ops);
    reg_ab(false);
    TEST_ASSERT_EQUAL_UINT(1, devbus_option_get(0, DUMMY_OPT_FLAG));
    TEST_ASSERT_EQUAL_UINT(2, devbus_option_get(0, DUMMY_OPT_MODE));
    TEST_ASSERT_EQUAL_UINT(0x33, devbus_option_get(0, DUMMY_OPT_FILL));
    TEST_ASSERT_FALSE(devbus_is_connected(0));
    devbus_start();
    TEST_ASSERT_TRUE(devbus_is_connected(0));
    TEST_ASSERT_EQUAL_HEX8(0x33, s_a.ram[5]);
}

void test_ini_save(void)
{
    reg_ab(false);
    devbus_connect(0);
    devbus_option_set(0, DUMMY_OPT_MODE, 0);
    cfgroot_save(g_cfgmain);
    char *txt = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(s_ini_path, &txt, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(txt, "[DUMMY]"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "connected = 1"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "mode = off"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "fill = 0xaa"));
    g_free(txt);
}

void test_ini_section_collision(void)
{
    s_desc_tmp = s_desc_a;
    s_desc_tmp.ini_section = "CFGMAIN";
    TEST_ASSERT_NOT_NULL(cfgroot_register_new_module(g_cfgmain, "CFGMAIN"));
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_a));
    /* Dvě zařízení se stejnou sekcí */
    reg_ab(false);
    s_desc_tmp = s_desc_b;
    s_desc_tmp.ini_section = "DUMMY";
    TEST_ASSERT_EQUAL_INT(DEVBUS_ERR_INVALID, devbus_register(&s_desc_tmp, &s_b));
}

void test_ini_reregister_after_exit(void)
{
    reg_ab(false);
    devbus_option_set(0, DUMMY_OPT_FILL, 0x44);
    devbus_exit();
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_destroy);
    devbus_init(&s_fake_ops);
    /* Sekce už existuje (založil ji devbus): znovu použita, hodnoty z INI (soubor neexistuje = výchozí) */
    TEST_ASSERT_EQUAL_INT(0, devbus_register(&s_desc_a, &s_a));
    TEST_ASSERT_EQUAL_UINT(0xAA, devbus_option_get(0, DUMMY_OPT_FILL));
}


/* ===================== 7. události, výstup ===================== */

void test_events_only_connected(void)
{
    reg_ab(true);
    devbus_reset_all();
    devbus_frame_done();
    devbus_rom_changed();
    devbus_after_load();
    TEST_ASSERT_EQUAL_UINT(0, s_a.n_reset + s_a.n_frame_done + s_a.n_rom_changed + s_a.n_after_load);
    devbus_connect(0);
    devbus_reset_all();
    devbus_frame_done();
    devbus_frame_done();
    devbus_rom_changed();
    devbus_after_load();
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_reset);
    TEST_ASSERT_EQUAL_UINT(2, s_a.n_frame_done);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_rom_changed);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_after_load);
    TEST_ASSERT_EQUAL_UINT(0, s_b.n_reset + s_b.n_frame_done);
}

void test_exit_disconnects_and_destroys(void)
{
    reg_ab(false);
    devbus_connect(0);
    devbus_exit();
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_disconnect);
    TEST_ASSERT_EQUAL_UINT(1, s_a.n_destroy);
    TEST_ASSERT_FALSE(devbus_is_initialized());
    TEST_ASSERT_NULL(g_devbus_ports[0x74].range);
    /* Bez inicializace okamžitý návrat */
    devbus_rom_changed();
    devbus_after_load();
    devbus_init(&s_fake_ops);
}

/** @brief Paleta testovacího výstupu. */
static const uint32_t s_pal[2] = { 0x000000ffu, 0xffffffffu };
/** @brief Popis testovacího výstupu 8 x 4. */
static const st_DEVBUS_VOUT_DESC s_vout_desc = { "dummyout", "Dummy output", 8, 4, 1.0f, s_pal, 2 };

void test_vout_present_fetch(void)
{
    reg_ab(false);
    st_DEVBUS_VOUT *o = devbus_vout_create(0, &s_vout_desc);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT(1, devbus_vout_count());
    TEST_ASSERT_EQUAL_PTR(o, devbus_vout_at(0));
    TEST_ASSERT_NULL(devbus_vout_at(1));
    TEST_ASSERT_EQUAL_PTR(&s_vout_desc, devbus_vout_desc(o));
    TEST_ASSERT_EQUAL_INT(0, devbus_vout_slot(o));

    uint8_t dst[32];
    uint32_t seen = 0;
    /* Zatím žádný snímek */
    TEST_ASSERT_FALSE(devbus_vout_fetch(o, dst, sizeof(dst), &seen));
    TEST_ASSERT_FALSE(devbus_vout_read_last(o, dst, sizeof(dst), &seen));
    uint8_t *p = devbus_vout_draw_buffer(o);
    memset(p, 1, 32);
    devbus_vout_present(o);
    /* Nový buffer kreslení má obsah předaného snímku */
    uint8_t *p2 = devbus_vout_draw_buffer(o);
    TEST_ASSERT_TRUE(p2 != p);
    TEST_ASSERT_EQUAL_HEX8(1, p2[31]);
    TEST_ASSERT_TRUE(devbus_vout_fetch(o, dst, sizeof(dst), &seen));
    TEST_ASSERT_EQUAL_UINT32(1, seen);
    TEST_ASSERT_EQUAL_HEX8(1, dst[0]);
    /* Bez nového snímku nic */
    TEST_ASSERT_FALSE(devbus_vout_fetch(o, dst, sizeof(dst), &seen));
    /* Dva snímky bez čtení: čtenář dostane poslední */
    p2[0] = 2;
    devbus_vout_present(o);
    devbus_vout_draw_buffer(o)[0] = 3;
    devbus_vout_present(o);
    TEST_ASSERT_TRUE(devbus_vout_fetch(o, dst, sizeof(dst), &seen));
    TEST_ASSERT_EQUAL_UINT32(3, seen);
    TEST_ASSERT_EQUAL_HEX8(3, dst[0]);
    /* Čtení v emu vlákně (read_last) vidí poslední snímek a čtenáři fetch()
     * nový snímek neubere */
    devbus_vout_draw_buffer(o)[0] = 4;
    devbus_vout_present(o);
    uint8_t last[32];
    uint32_t lseq = 0;
    TEST_ASSERT_TRUE(devbus_vout_read_last(o, last, sizeof(last), &lseq));
    TEST_ASSERT_EQUAL_UINT32(4, lseq);
    TEST_ASSERT_EQUAL_HEX8(4, last[0]);
    TEST_ASSERT_EQUAL_HEX8(1, last[31]);
    TEST_ASSERT_TRUE(devbus_vout_read_last(o, last, sizeof(last), NULL));
    TEST_ASSERT_TRUE(devbus_vout_fetch(o, dst, sizeof(dst), &seen));
    TEST_ASSERT_EQUAL_UINT32(4, seen);
    TEST_ASSERT_EQUAL_HEX8(4, dst[0]);
    /* Po fetch() (výměna bufferů) a dalším snímku dál poslední */
    devbus_vout_draw_buffer(o)[0] = 5;
    devbus_vout_present(o);
    TEST_ASSERT_TRUE(devbus_vout_read_last(o, last, sizeof(last), &lseq));
    TEST_ASSERT_EQUAL_UINT32(5, lseq);
    TEST_ASSERT_EQUAL_HEX8(5, last[0]);
    /* Malý cíl */
    seen = 0;
    TEST_ASSERT_FALSE(devbus_vout_fetch(o, dst, 31, &seen));
    seen = 3;
    TEST_ASSERT_FALSE(devbus_vout_read_last(o, dst, 31, &seen));
    TEST_ASSERT_FALSE(devbus_vout_read_last(NULL, dst, sizeof(dst), &seen));
    TEST_ASSERT_EQUAL_UINT32(3, seen);
    /* Neplatný popis */
    static const st_DEVBUS_VOUT_DESC bad = { "Bad", "x", 8, 4, 1.0f, s_pal, 2 };
    TEST_ASSERT_NULL(devbus_vout_create(0, &bad));
    /* Po startu už nelze */
    devbus_start();
    TEST_ASSERT_NULL(devbus_vout_create(0, &s_vout_desc));
}


/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_registry_basic);
    RUN_TEST(test_registry_uninitialized_and_after_start);
    RUN_TEST(test_registry_full);
    RUN_TEST(test_registry_invalid_desc);
    RUN_TEST(test_registry_static_ports_rejected);
    RUN_TEST(test_registry_mmio_window);
    RUN_TEST(test_connect_disconnect);
    RUN_TEST(test_connect_conflicts);
    RUN_TEST(test_connect_conflict_mmio_only);
    RUN_TEST(test_connect_conflict_video);
    RUN_TEST(test_connect_failure_keeps_tables_clean);
    RUN_TEST(test_dispatch_ports);
    RUN_TEST(test_dispatch_mmio_map_state);
    RUN_TEST(test_dispatch_mmio_set_when);
    RUN_TEST(test_dispatch_mmio_when_window);
    RUN_TEST(test_options_defaults_and_set);
    RUN_TEST(test_options_parse_format);
    RUN_TEST(test_ini_load_and_start_connects);
    RUN_TEST(test_ini_save);
    RUN_TEST(test_ini_section_collision);
    RUN_TEST(test_ini_reregister_after_exit);
    RUN_TEST(test_events_only_connected);
    RUN_TEST(test_exit_disconnects_and_destroys);
    RUN_TEST(test_vout_present_fetch);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
