/**
 * @file snap_ide8.c
 * @brief Snapshot handler: IDE8 HDD — uložení a načtení stavu IDE řadiče
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/ide8/ide8.h"


static en_SNAPSHOT_RESULT snap_ide8_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "ide8_state");

    /* Společné registry řadiče */
    snapshot_xml_write_int(w, "selected", (int)g_ide8.selected);
    snapshot_xml_write_hex8(w, "regSECTOR_COUNT", g_ide8.regSECTOR_COUNT);
    snapshot_xml_write_hex8(w, "regSECTOR", g_ide8.regSECTOR);
    snapshot_xml_write_hex16(w, "regCYLINDER", g_ide8.regCYLINDER);
    snapshot_xml_write_hex8(w, "regHEAD", g_ide8.regHEAD);
    snapshot_xml_write_hex8(w, "regFEATURES", g_ide8.regFEATURES);

    /* Jednotlivé disky */
    for (int i = 0; i < IDE8_DRIVES_COUNT; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "drive_%d", i);
        snapshot_xml_open_element(w, elem_name);

        const st_IDE8_DRIVE *drv = &g_ide8.drive[i];

        snapshot_xml_write_int(w, "connected", (int)drv->connected);
        snapshot_xml_write_int(w, "geo_c", drv->geo_c);
        snapshot_xml_write_int(w, "geo_h", drv->geo_h);
        snapshot_xml_write_int(w, "geo_s", drv->geo_s);
        snapshot_xml_write_int(w, "data_pos", drv->data_pos);
        snapshot_xml_write_int(w, "sector_count", drv->sector_count);
        snapshot_xml_write_int(w, "cmd", (int)drv->cmd);
        snapshot_xml_write_int(w, "addressing", (int)drv->addressing);
        snapshot_xml_write_int(w, "busmode", (int)drv->busmode);
        snapshot_xml_write_hex8(w, "status", drv->status);
        snapshot_xml_write_hex32(w, "block", drv->block);
        snapshot_xml_write_hex32(w, "total_blocks", drv->total_blocks);

        /* Cesta k image souboru (referenční) */
        snapshot_xml_write_string(w, "filepath", drv->filepath ? drv->filepath : "");

        snapshot_xml_close_element(w); /* drive_N */
    }

    snapshot_xml_close_element(w); /* ide8_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/ide8.xml", xml);
    g_free(xml);
    if (res != SNAPSHOT_OK) return res;

    /* Uložení cache obou disků jako jeden binární soubor */
    uint8_t cache_combined[IDE8_SECTOR_SIZE * IDE8_DRIVES_COUNT];
    for (int i = 0; i < IDE8_DRIVES_COUNT; i++) {
        memcpy(cache_combined + (i * IDE8_SECTOR_SIZE),
               g_ide8.drive[i].cache, IDE8_SECTOR_SIZE);
    }
    res = snapshot_io_write_bin(ctx->io, "devices/ide8_cache.bin",
                                cache_combined, sizeof(cache_combined));

    return res;
}


static en_SNAPSHOT_RESULT snap_ide8_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* IDE8 entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/ide8.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/ide8.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("ide8", "Cannot load devices/ide8.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("ide8", "Parse error in devices/ide8.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "ide8_state")) {
        SNAP_ERR("ide8", "Missing element ide8_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    int ival;
    uint8_t hval8;
    uint16_t hval16;
    uint32_t hval32;

    /* Společné registry řadiče */
    if (snapshot_xml_read_int(r, "selected", &ival)) g_ide8.selected = (en_IDE8_DRIVE)ival;
    if (snapshot_xml_read_hex8(r, "regSECTOR_COUNT", &hval8)) g_ide8.regSECTOR_COUNT = hval8;
    if (snapshot_xml_read_hex8(r, "regSECTOR", &hval8)) g_ide8.regSECTOR = hval8;
    if (snapshot_xml_read_hex16(r, "regCYLINDER", &hval16)) g_ide8.regCYLINDER = hval16;
    if (snapshot_xml_read_hex8(r, "regHEAD", &hval8)) g_ide8.regHEAD = hval8;
    if (snapshot_xml_read_hex8(r, "regFEATURES", &hval8)) g_ide8.regFEATURES = hval8;

    /* Jednotlivé disky */
    for (int i = 0; i < IDE8_DRIVES_COUNT; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "drive_%d", i);

        if (snapshot_xml_enter_element(r, elem_name)) {
            st_IDE8_DRIVE *drv = &g_ide8.drive[i];

            if (snapshot_xml_read_int(r, "connected", &ival)) drv->connected = (en_IDE8_STATE)ival;
            snapshot_xml_read_int(r, "geo_c", &drv->geo_c);
            snapshot_xml_read_int(r, "geo_h", &drv->geo_h);
            snapshot_xml_read_int(r, "geo_s", &drv->geo_s);
            snapshot_xml_read_int(r, "data_pos", &drv->data_pos);
            snapshot_xml_read_int(r, "sector_count", &drv->sector_count);
            if (snapshot_xml_read_int(r, "cmd", &ival)) drv->cmd = (en_IDE8_CMD)ival;
            if (snapshot_xml_read_int(r, "addressing", &ival)) drv->addressing = (en_IDE8_ADDRESSING)ival;
            if (snapshot_xml_read_int(r, "busmode", &ival)) drv->busmode = (en_IDE8_BUSMODE)ival;
            if (snapshot_xml_read_hex8(r, "status", &hval8)) drv->status = hval8;
            if (snapshot_xml_read_hex32(r, "block", &hval32)) drv->block = hval32;
            if (snapshot_xml_read_hex32(r, "total_blocks", &hval32)) drv->total_blocks = hval32;

            /* Cesta k image souboru (informativní) */
            char *fpath = NULL;
            if (snapshot_xml_read_string(r, "filepath", &fpath)) {
                if (fpath && fpath[0] != '\0') {
                    SNAP_WARN("ide8", "Disk reference file %d: '%s'", i, fpath);
                }
                g_free(fpath);
            }

            snapshot_xml_leave_element(r); /* drive_N */
        }
    }

    snapshot_xml_leave_element(r); /* ide8_state */
    snapshot_xml_reader_free(r);

    /* Načtení cache obou disků */
    if (snapshot_io_entry_exists(ctx->io, "devices/ide8_cache.bin")) {
        uint8_t cache_combined[IDE8_SECTOR_SIZE * IDE8_DRIVES_COUNT];
        res = snapshot_io_read_bin_into(ctx->io, "devices/ide8_cache.bin",
                                        cache_combined, sizeof(cache_combined));
        if (res == SNAPSHOT_OK) {
            for (int i = 0; i < IDE8_DRIVES_COUNT; i++) {
                memcpy(g_ide8.drive[i].cache,
                       cache_combined + (i * IDE8_SECTOR_SIZE), IDE8_SECTOR_SIZE);
            }
        } else {
            SNAP_WARN("ide8", "Cannot load devices/ide8_cache.bin");
        }
    }

    return SNAPSHOT_OK;
}


void snap_ide8_register(void)
{
    /* Platformy bez HW registraci preskoci - snapshot pak sekci
     * neobsahuje (zmrazeny on-disk kontrakt, shodne s drivejsim
     * compile-time #if). */
    if (!g_mzhal.have_ide8)
        return;

    snapshot_register_component("ide8",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_ide8_save,
                                snap_ide8_load,
                                true);
}


