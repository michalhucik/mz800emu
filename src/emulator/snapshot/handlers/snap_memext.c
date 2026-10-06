/**
 * @file snap_memext.c
 * @brief Snapshot handler: MEMEXT — uložení a načtení stavu paměťového rozšíření
 *
 * Připojení (connection) a typ rozšíření jsou stav stroje: snapshot je
 * obnoví pro běh, ale INI ([MEMEXT] connected, type) si drží volbu
 * uživatele (snapshot_config_pin_ini_value). Volby init_mem a init_luftner
 * (čím se plní paměť při zapnutí) jsou uživatelské preference: ukládají
 * se kvůli starším verzím, při načtení se ignorují.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshot_config.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/memory/memory.h"


static en_SNAPSHOT_RESULT snap_memext_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "memext_state");

    /* Konfigurace */
    snapshot_xml_write_int(w, "connection", (int)g_memext.connection);
    snapshot_xml_write_int(w, "type", (int)g_memext.type);
    snapshot_xml_write_int(w, "init_mem", (int)g_memext.init_mem);
    snapshot_xml_write_int(w, "init_luftner", (int)g_memext.init_luftner);
    snapshot_xml_write_hex16(w, "addr_mask", g_memext.addr_mask);

    /* Mapovací tabulka (16 položek) */
    snapshot_xml_open_element(w, "map");
    for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "entry_%d", i);
        snapshot_xml_write_hex32(w, elem_name, g_memext.map[i]);
    }
    snapshot_xml_close_element(w); /* map */

    snapshot_xml_close_element(w); /* memext_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/memext.xml", xml);
    g_free(xml);
    if (res != SNAPSHOT_OK) return res;

    /* Uložení obsahu RAM a FLASH (pokud je připojeno a zapnuto v nastavení) */
    if (g_memext.connection == MEMEXT_CONNECTION_YES && g_snapshot_settings.include_memext) {
        res = snapshot_io_write_bin(ctx->io, "memory/memext_ram.bin",
                                    g_memext.RAM, MEMEXT_RAM_SIZE);
        if (res != SNAPSHOT_OK) return res;

        res = snapshot_io_write_bin(ctx->io, "memory/memext_flash.bin",
                                    g_memext.FLASH, MEMEXT_FLASH_SIZE);
        if (res != SNAPSHOT_OK) return res;
    }

    return SNAPSHOT_OK;
}


static en_SNAPSHOT_RESULT snap_memext_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* MEMEXT entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/memext.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/memext.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("memext", "Cannot load devices/memext.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("memext", "Parse error in devices/memext.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "memext_state")) {
        SNAP_ERR("memext", "Missing element memext_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    int ival;
    uint16_t hval16;

    /* Konfigurace. connection a type jen pro běh (INI hodnota se připne),
     * init_mem a init_luftner záměrně nečteme (uživatelské preference). */
    unsigned ini_connection = (unsigned)g_memext.connection;
    unsigned ini_type = (unsigned)g_memext.type;
    if (snapshot_xml_read_int(r, "connection", &ival)) g_memext.connection = (en_MEMEXT_CONNECTION)ival;
    if (snapshot_xml_read_int(r, "type", &ival)) g_memext.type = (en_MEMEXT_TYPE)ival;
    snapshot_config_pin_ini_value("MEMEXT", "connected", ini_connection);
    snapshot_config_pin_ini_value("MEMEXT", "type", ini_type);
    if (snapshot_xml_read_hex16(r, "addr_mask", &hval16)) g_memext.addr_mask = hval16;

    /* Mapovací tabulka */
    if (snapshot_xml_enter_element(r, "map")) {
        for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++) {
            char elem_name[16];
            uint32_t hval32;
            snprintf(elem_name, sizeof(elem_name), "entry_%d", i);
            if (snapshot_xml_read_hex32(r, elem_name, &hval32))
                g_memext.map[i] = hval32;
        }
        snapshot_xml_leave_element(r); /* map */
    }

    snapshot_xml_leave_element(r); /* memext_state */
    snapshot_xml_reader_free(r);

    /* Načtení obsahu RAM (pokud binární soubor existuje) */
    if (snapshot_io_entry_exists(ctx->io, "memory/memext_ram.bin")) {
        res = snapshot_io_read_bin_into(ctx->io, "memory/memext_ram.bin",
                                        g_memext.RAM, MEMEXT_RAM_SIZE);
        if (res != SNAPSHOT_OK) {
            SNAP_WARN("memext", "Cannot load memory/memext_ram.bin");
        }
    }

    /* Načtení obsahu FLASH (pokud binární soubor existuje) */
    if (snapshot_io_entry_exists(ctx->io, "memory/memext_flash.bin")) {
        res = snapshot_io_read_bin_into(ctx->io, "memory/memext_flash.bin",
                                        g_memext.FLASH, MEMEXT_FLASH_SIZE);
        if (res != SNAPSHOT_OK) {
            SNAP_WARN("memext", "Cannot load memory/memext_flash.bin");
        }
    }

    /* Přepojení paměťové mapy po načtení stavu */
    memory_reconnect_ram();

    return SNAPSHOT_OK;
}


void snap_memext_register(void)
{
    snapshot_register_component("memext",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_memext_save,
                                snap_memext_load,
                                true);
}
