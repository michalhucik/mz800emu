/**
 * @file snap_ramdisk.c
 * @brief Snapshot handler: RAM disk — uložení a načtení stavu RAM disku (STD + Pezik)
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/ramdisk/ramdisk.h"
#include "snapshot/snapshot.h"


static en_SNAPSHOT_RESULT snap_ramdisk_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "ramdisk_state");

    /* STD ramdisk */
    snapshot_xml_open_element(w, "std");
    snapshot_xml_write_uint(w, "connected", g_ramdisk.std.connected);
    snapshot_xml_write_int(w, "type", (int)g_ramdisk.std.type);
    snapshot_xml_write_int(w, "size", (int)g_ramdisk.std.size);
    snapshot_xml_write_hex16(w, "offset", g_ramdisk.std.offset);
    snapshot_xml_write_hex8(w, "bank", g_ramdisk.std.bank);
    snapshot_xml_close_element(w); /* std */

    /* Pezik ramdisk — oba kanály */
    for (int i = 0; i < 2; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "pezik_%d", i);
        snapshot_xml_open_element(w, elem_name);
        snapshot_xml_write_uint(w, "connected", g_ramdisk.pezik[i].connected);
        snapshot_xml_write_hex16(w, "latch", g_ramdisk.pezik[i].latch);
        snapshot_xml_write_uint(w, "portmask", g_ramdisk.pezik[i].portmask);
        snapshot_xml_write_uint(w, "backuped", g_ramdisk.pezik[i].backuped);
        snapshot_xml_close_element(w); /* pezik_N */
    }

    snapshot_xml_close_element(w); /* ramdisk_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/ramdisk.xml", xml);
    g_free(xml);
    if (res != SNAPSHOT_OK) return res;

    /* Uložení obsahu STD ramdisku (pouze volatile a pokud je zapnuto v nastavení) */
    if (g_ramdisk.std.connected && g_ramdisk.std.type == RAMDISK_TYPE_STD
        && g_snapshot_settings.include_ramdisk && g_ramdisk.std.memory != NULL) {
        size_t mem_size = ((size_t)g_ramdisk.std.size + 1) * 0x10000;
        res = snapshot_io_write_bin(ctx->io, "devices/ramdisk_std.bin",
                                    g_ramdisk.std.memory, mem_size);
        if (res != SNAPSHOT_OK) return res;
    }

    return SNAPSHOT_OK;
}


static en_SNAPSHOT_RESULT snap_ramdisk_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Ramdisk entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/ramdisk.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/ramdisk.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("ramdisk", "Cannot load devices/ramdisk.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("ramdisk", "Parse error in devices/ramdisk.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "ramdisk_state")) {
        SNAP_ERR("ramdisk", "Missing element ramdisk_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* STD ramdisk */
    if (snapshot_xml_enter_element(r, "std")) {
        int ival;
        uint16_t hval16;
        uint8_t hval8;

        snapshot_xml_read_uint(r, "connected", &g_ramdisk.std.connected);
        if (snapshot_xml_read_int(r, "type", &ival)) g_ramdisk.std.type = (en_RAMDISK_TYPE)ival;
        if (snapshot_xml_read_int(r, "size", &ival)) g_ramdisk.std.size = (en_RAMDISK_BANKMASK)ival;
        if (snapshot_xml_read_hex16(r, "offset", &hval16)) g_ramdisk.std.offset = hval16;
        if (snapshot_xml_read_hex8(r, "bank", &hval8)) g_ramdisk.std.bank = hval8;

        snapshot_xml_leave_element(r); /* std */
    }

    /* Pezik ramdisk — oba kanály */
    for (int i = 0; i < 2; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "pezik_%d", i);

        if (snapshot_xml_enter_element(r, elem_name)) {
            uint16_t hval16;

            snapshot_xml_read_uint(r, "connected", &g_ramdisk.pezik[i].connected);
            if (snapshot_xml_read_hex16(r, "latch", &hval16)) g_ramdisk.pezik[i].latch = hval16;
            snapshot_xml_read_uint(r, "portmask", &g_ramdisk.pezik[i].portmask);
            snapshot_xml_read_uint(r, "backuped", &g_ramdisk.pezik[i].backuped);

            snapshot_xml_leave_element(r); /* pezik_N */
        }
    }

    snapshot_xml_leave_element(r); /* ramdisk_state */
    snapshot_xml_reader_free(r);

    /* Načtení obsahu STD ramdisku (pokud binární soubor existuje) */
    if (g_ramdisk.std.connected && g_ramdisk.std.memory != NULL
        && snapshot_io_entry_exists(ctx->io, "devices/ramdisk_std.bin")) {
        size_t mem_size = ((size_t)g_ramdisk.std.size + 1) * 0x10000;
        res = snapshot_io_read_bin_into(ctx->io, "devices/ramdisk_std.bin",
                                        g_ramdisk.std.memory, mem_size);
        if (res != SNAPSHOT_OK) {
            SNAP_WARN("ramdisk", "Cannot load devices/ramdisk_std.bin");
        }
    }

    return SNAPSHOT_OK;
}


void snap_ramdisk_register(void)
{
    /* Platformy bez HW registraci preskoci - snapshot pak sekci
     * neobsahuje (zmrazeny on-disk kontrakt, shodne s drivejsim
     * compile-time #if). */
    if (!g_mzhal.have_ramdisk)
        return;

    snapshot_register_component("ramdisk",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_ramdisk_save,
                                snap_ramdisk_load,
                                true);
}


