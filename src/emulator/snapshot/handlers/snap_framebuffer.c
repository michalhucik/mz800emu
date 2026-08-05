/**
 * @file snap_framebuffer.c
 * @brief Snapshot handler: framebuffer — pixel buffery pro zobrazení
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/gdg/framebuffer_state.h"
#include "mzarch/mzhal.h"

/**
 * @brief Velikost jednoho pixel bufferu v hw/framebuffer.bin.
 *
 * Zmrazený on-disk kontrakt: blok obsahuje GDG_FRAMEBUFFER_PIXBUF_COUNT
 * bufferů o skutečné display ploše architektury (mz800 267264 B,
 * mz700/mz1500 163328 B) za sebou - NE superset rozměr pixbuff pole
 * (GDG_FRAMEBUFFER_PIXBUF_SIZE_MAX). V paměti buffery leží se superset
 * stridem, proto se při save/load přeskládávají přes dočasný buffer.
 */
static uint32_t snap_framebuffer_pixbuf_size(void)
{
    return (uint32_t)(g_mzhal.video_display_width * g_mzhal.video_display_height);
}

static en_SNAPSHOT_RESULT snap_framebuffer_save(st_SNAPSHOT_CONTEXT *ctx)
{
    en_SNAPSHOT_RESULT res;

    /* Uložení pixel bufferů jako binární data (skutečná velikost, viz
     * snap_framebuffer_pixbuf_size) */
    const uint32_t fb_size = snap_framebuffer_pixbuf_size();
    uint8_t *packed = g_malloc((gsize)fb_size * GDG_FRAMEBUFFER_PIXBUF_COUNT);
    for (int i = 0; i < GDG_FRAMEBUFFER_PIXBUF_COUNT; i++) {
        memcpy(packed + (gsize)i * fb_size, g_framebuffer.pixbuff[i], fb_size);
    }
    res = snapshot_io_write_bin(ctx->io, "hw/framebuffer.bin",
                                packed,
                                (gsize)fb_size * GDG_FRAMEBUFFER_PIXBUF_COUNT);
    g_free(packed);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("framebuffer", "Cannot write hw/framebuffer.bin");
        return res;
    }

    /* Uložení stavu (pixels_id) jako XML */
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "framebuffer_state");
    snapshot_xml_write_int(w, "pixels_id", g_framebuffer.pixels_id);
    snapshot_xml_close_element(w);

    char *xml = snapshot_xml_writer_finish(w);
    res = snapshot_io_write_xml(ctx->io, "hw/framebuffer_state.xml", xml);
    g_free(xml);

    return res;
}

static en_SNAPSHOT_RESULT snap_framebuffer_load(st_SNAPSHOT_CONTEXT *ctx)
{
    en_SNAPSHOT_RESULT res;

    /* Načtení pixel bufferů (formát viz snap_framebuffer_pixbuf_size) */
    const uint32_t fb_size = snap_framebuffer_pixbuf_size();
    uint8_t *packed = g_malloc((gsize)fb_size * GDG_FRAMEBUFFER_PIXBUF_COUNT);
    res = snapshot_io_read_bin_into(ctx->io, "hw/framebuffer.bin",
                                    packed,
                                    (gsize)fb_size * GDG_FRAMEBUFFER_PIXBUF_COUNT);
    if (res != SNAPSHOT_OK) {
        g_free(packed);
        SNAP_ERR("framebuffer", "Cannot load hw/framebuffer.bin");
        return res;
    }
    for (int i = 0; i < GDG_FRAMEBUFFER_PIXBUF_COUNT; i++) {
        memcpy(g_framebuffer.pixbuff[i], packed + (gsize)i * fb_size, fb_size);
    }
    g_free(packed);

    /* Načtení stavu */
    char *xml = NULL;
    res = snapshot_io_read_xml(ctx->io, "hw/framebuffer_state.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("framebuffer", "Cannot load hw/framebuffer_state.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("framebuffer", "Parse error in hw/framebuffer_state.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (snapshot_xml_enter_element(r, "framebuffer_state")) {
        snapshot_xml_read_int(r, "pixels_id", &g_framebuffer.pixels_id);
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_reader_free(r);

    /* Nastavení ukazatele pixels na správný buffer dle pixels_id */
    if (g_framebuffer.pixels_id < 0 || g_framebuffer.pixels_id >= GDG_FRAMEBUFFER_PIXBUF_COUNT) {
        SNAP_WARN("framebuffer", "Invalid pixels_id %d, resetting to 0",
                  g_framebuffer.pixels_id);
        g_framebuffer.pixels_id = 0;
    }
    g_framebuffer.pixels = g_framebuffer.pixbuff[g_framebuffer.pixels_id];

    return SNAPSHOT_OK;
}

void snap_framebuffer_register(void)
{
    snapshot_register_component("framebuffer",
                                SNAPSHOT_PRIORITY_FRAMEBUFFER,
                                snap_framebuffer_save,
                                snap_framebuffer_load,
                                false);
}
