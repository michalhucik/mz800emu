/**
 * @file   devbus_dummy.c
 * @brief  Testovací zařízení devbus (kontrakt v devbus_dummy.h).
 *
 * @par Licence: GPLv3
 */

#include "devbus_dummy.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_xml.h"


/** @brief Klíčová slova volby "mode". */
static const char *const s_mode_words[] = { "off", "rising", "level", NULL };

/** @brief Volby dummy zařízení (indexy DUMMY_OPT_*). */
static const st_DEVBUS_OPTION s_options[] = {
    { "flag", "Flag", DEVBUS_OPT_BOOL, 0, 0, 0, NULL, DEVBUS_APPLY_LIVE },
    { "mode", "Mode", DEVBUS_OPT_KEYWORD, 1, 0, 0, s_mode_words, DEVBUS_APPLY_LIVE },
    { "fill", "RAM fill after power on", DEVBUS_OPT_UNSIGNED, 0xAA, 0, 255, NULL, DEVBUS_APPLY_CONNECT },
};


static uint8_t dummy_port_read(void *ctx, uint16_t port, uint8_t *driven)
{
    st_DEVBUS_DUMMY *d = ctx;
    if (d->driven_override_valid) *driven = d->driven_override;
    if ((port & 0xff) == 0x75) return ++d->reads;
    return d->reg;
}

static uint8_t dummy_port_peek(void *ctx, uint16_t port, uint8_t *driven)
{
    st_DEVBUS_DUMMY *d = ctx;
    if (d->driven_override_valid) *driven = d->driven_override;
    if ((port & 0xff) == 0x75) return d->reads;
    return d->reg;
}

static void dummy_port_write(void *ctx, uint16_t port, uint8_t v)
{
    st_DEVBUS_DUMMY *d = ctx;
    if ((port & 0xff) == 0x74) {
        d->reg = v;
        if (d->wait_on_write) devbus_cpu_wait(d->wait_on_write);
    }
}

/** @brief Porty dummy zařízení: 74h-75h (třístavový budič). */
static const st_DEVBUS_PORT_RANGE s_ports[] = {
    { .first = 0x74, .last = 0x75, .drive = DEVBUS_DRIVE_TRISTATE,
      .read = dummy_port_read, .peek = dummy_port_peek, .write = dummy_port_write },
};


static void dummy_mmio_write(void *ctx, uint16_t addr, uint8_t value, unsigned flags)
{
    st_DEVBUS_DUMMY *d = ctx;
    d->n_mmio++;
    d->mmio_addr = addr;
    d->mmio_value = value;
    d->mmio_flags = flags;
    d->mmio_dbg = devbus_access_is_debugger();
    if (d->wait_on_write) devbus_cpu_wait(d->wait_on_write);
}

/** @brief MMIO dummy zařízení: 0E013h (MAPPED), 0E014h (DRAM); jen snoop zápisu. */
static const st_DEVBUS_MMIO_RANGE s_mmio[] = {
    { .first = 0xE013, .last = 0xE013, .when_default = DEVBUS_MMIO_MAPPED, .drive = DEVBUS_DRIVE_NONE, .write = dummy_mmio_write },
    { .first = 0xE014, .last = 0xE014, .when_default = DEVBUS_MMIO_DRAM, .drive = DEVBUS_DRIVE_NONE, .write = dummy_mmio_write },
};


static const uint8_t *dummy_glyph(void *ctx)
{
    static uint8_t cg[4096];
    (void) ctx;
    return cg;
}

static void dummy_row(void *ctx, unsigned canvas_y, const uint8_t *cg, uint8_t *dst)
{
    st_DEVBUS_DUMMY *d = ctx;
    (void) canvas_y;
    (void) cg;
    (void) dst;
    d->n_row++;
}

/** @brief Video háčky: jen zdroj glyfů. */
static const st_DEVBUS_VIDEO_OPS s_video_glyph = { dummy_glyph, NULL };
/** @brief Video háčky: jen kreslení řádku. */
static const st_DEVBUS_VIDEO_OPS s_video_row = { NULL, dummy_row };


static uint8_t *dummy_ram(void *ctx)
{
    return ((st_DEVBUS_DUMMY *) ctx)->ram;
}

static void dummy_ram_written(void *ctx, uint32_t off, uint32_t len)
{
    (void) off;
    (void) len;
    ((st_DEVBUS_DUMMY *) ctx)->n_region_write++;
}

/** @brief Region dummy zařízení: RAM 256 B. */
static const st_DEVBUS_REGION s_regions[] = {
    { "Dummy RAM (256 B)", DUMMY_RAM_SIZE, 1, dummy_ram, dummy_ram_written },
};


static uint8_t dummy_cat_reg(void)
{
    return 0;
}

/**
 * @brief Katalog I/O dummy zařízení.
 *
 * Maska všech platforem: zařízení se registruje do testovacího jádra
 * MZ-800 (falešný adaptér MZ-700 je jen maska oken MMIO) a
 * io_catalog_probe_byte() bere jen položky platformy jádra.
 */
static const st_IO_PORT_DESC s_catalog[] = {
    { 0x0074, "DUMMY REG", "Dummy register", IO_PORT_DIR_RW, NULL, 0, dummy_cat_reg, NULL, MZ_AVAIL_ALL, IO_CROSS_NONE },
    { 0xE013, "DUMMY MMIO", "Dummy MMIO", IO_PORT_DIR_W, NULL, 0, NULL, NULL, MZ_AVAIL_ALL, IO_CROSS_NONE },
};


static en_SNAPSHOT_RESULT dummy_save(void *ctx, st_DEVBUS_SNAP *s)
{
    st_DEVBUS_DUMMY *d = ctx;
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);
    snapshot_xml_open_element(w, "dummy");
    snapshot_xml_write_hex8(w, "reg", d->reg);
    snapshot_xml_write_hex8(w, "reads", d->reads);
    snapshot_xml_write_hex32(w, "extra", d->extra);
    snapshot_xml_close_element(w);
    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT r = devbus_snap_write_xml(s, "state.xml", xml);
    g_free(xml);
    if (r != SNAPSHOT_OK) return r;
    return devbus_snap_write_bin(s, "ram.bin", d->ram, sizeof(d->ram));
}

static en_SNAPSHOT_RESULT dummy_load(void *ctx, st_DEVBUS_SNAP *s, uint32_t version)
{
    st_DEVBUS_DUMMY *d = ctx;
    char *xml = NULL;
    en_SNAPSHOT_RESULT r = devbus_snap_read_xml(s, "state.xml", &xml);
    if (r != SNAPSHOT_OK) return r;
    snapshot_xml_reader_t *rd = snapshot_xml_reader_new(xml);
    g_free(xml);
    if (!rd) return SNAPSHOT_ERR_XML_PARSE;
    if (snapshot_xml_enter_element(rd, "dummy")) {
        snapshot_xml_read_hex8(rd, "reg", &d->reg);
        snapshot_xml_read_hex8(rd, "reads", &d->reads);
        d->extra = 0x5A; /* verze 1 pole nemá */
        if (version >= 2) snapshot_xml_read_hex32(rd, "extra", &d->extra);
        snapshot_xml_leave_element(rd);
    }
    snapshot_xml_reader_free(rd);
    return devbus_snap_read_bin_into(s, "ram.bin", d->ram, sizeof(d->ram));
}


static int dummy_init(void *ctx)
{
    st_DEVBUS_DUMMY *d = ctx;
    d->n_init++;
    return d->fail_init ? -1 : 0;
}

static int dummy_connect(void *ctx)
{
    st_DEVBUS_DUMMY *d = ctx;
    d->n_connect++;
    if (d->fail_connect) return -1;
    memset(d->ram, (int) devbus_option_get(d->slot, DUMMY_OPT_FILL), sizeof(d->ram));
    return 0;
}

static void dummy_reset(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_reset++;
}

static void dummy_disconnect(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_disconnect++;
}

static void dummy_destroy(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_destroy++;
}

static void dummy_option_changed(void *ctx, unsigned opt, unsigned value)
{
    st_DEVBUS_DUMMY *d = ctx;
    d->n_option_changed++;
    d->last_opt = opt;
    d->last_opt_value = value;
}

static void dummy_frame_done(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_frame_done++;
}

static void dummy_after_load(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_after_load++;
}

static void dummy_rom_changed(void *ctx)
{
    ((st_DEVBUS_DUMMY *) ctx)->n_rom_changed++;
}

/** @brief Životní cyklus dummy zařízení. */
static const st_DEVBUS_LIFECYCLE s_ops = {
    dummy_init, dummy_connect, dummy_reset, dummy_disconnect, dummy_destroy,
    dummy_option_changed, dummy_frame_done, dummy_after_load, dummy_rom_changed,
};


void devbus_dummy_make_desc(st_DEVBUS_DEVICE_DESC *out, const char *id, const char *section, int video)
{
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->name = "Dummy test device";
    out->ini_section = section;
    out->platforms = DEVBUS_PLATFORM_MZ700;
    out->options = s_options;
    out->option_count = G_N_ELEMENTS(s_options);
    out->ports = s_ports;
    out->port_count = G_N_ELEMENTS(s_ports);
    out->mmio = s_mmio;
    out->mmio_count = G_N_ELEMENTS(s_mmio);
    out->video = (video == 1) ? &s_video_glyph : (video == 2) ? &s_video_row : NULL;
    out->regions = s_regions;
    out->region_count = G_N_ELEMENTS(s_regions);
    out->io_catalog = s_catalog;
    out->io_catalog_count = G_N_ELEMENTS(s_catalog);
    out->snap.version = 2;
    out->snap.save = dummy_save;
    out->snap.load = dummy_load;
    out->ops = &s_ops;
}


void devbus_dummy_reset_counters(st_DEVBUS_DUMMY *d)
{
    int slot = d->slot;
    bool fc = d->fail_connect, fi = d->fail_init;
    bool dov = d->driven_override_valid;
    uint8_t dom = d->driven_override;
    memset(d, 0, sizeof(*d));
    d->fail_connect = fc;
    d->fail_init = fi;
    d->slot = slot;
    d->driven_override_valid = dov;
    d->driven_override = dom;
}
