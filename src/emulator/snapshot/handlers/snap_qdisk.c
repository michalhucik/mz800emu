/**
 * @file snap_qdisk.c
 * @brief Snapshot handler: Quick Disk - uložení a načtení stavu Quick Disku
 *
 * Snapshot ukládá chip kontinuitu (SIO kanály, image_position, status, crc) +
 * informativní info (readonly effective, filename). Mount cesta image se
 * primárně obnovuje z cfgfile (`mz1f11_std_filepath`) - mount probíhá v
 * `qdisk_init()` PŘED snapshot loadem, takže snapshot už jen dopisuje chip
 * stav na mountnutý handler.
 *
 * Snapshot NEUKLÁDÁ user-pref pole `storage_mode`, `user_readonly` - ta jsou
 * v INI a obnovují se při init. `fs_readonly` se taky neukládá (runtime detect
 * při mount). Memory buffer image obsahu se neukládá (dirty RAM změny se
 * snapshot save+load ztrácí, stejné chování jako FDC).
 *
 * Backward compat: pole `readonly` a `filename` jsou nová (Fáze 4). Starý
 * snapshot bez těchto klíčů projde loadem - `snapshot_xml_read_*` vrátí
 * false a runtime defaulty z init/mount se nezmění.
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/qdisk/qdisk.h"


static en_SNAPSHOT_RESULT snap_qdisk_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "qdisk_state");

    /* Základní stav */
    snapshot_xml_write_uint(w, "connected", g_qdisk.connected);
    snapshot_xml_write_uint(w, "type", g_qdisk.type);
    snapshot_xml_write_uint(w, "status", g_qdisk.status);
    snapshot_xml_write_uint(w, "image_position", g_qdisk.image_position);
    snapshot_xml_write_hex16(w, "out_crc16", g_qdisk.out_crc16);

    /* Fáze 4: informativní pole (vzor snap_fdc.c). Mount user pref
     * (storage_mode, user_readonly) se neukládá - SSOT je INI.
     * fs_readonly se neukládá - runtime detect při mount. */
    snapshot_xml_write_int(w, "readonly", g_qdisk.readonly);
    snapshot_xml_write_string(w, "filename", g_qdisk.filename);

    /* SIO kanály */
    for (int ch = 0; ch < 2; ch++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "channel_%d", ch);
        snapshot_xml_open_element(w, elem_name);

        /* Zápisové registry (8 ks) */
        for (int j = 0; j < 8; j++) {
            char reg_name[16];
            snprintf(reg_name, sizeof(reg_name), "Wreg_%d", j);
            snapshot_xml_write_hex8(w, reg_name, g_qdisk.channel[ch].Wreg[j]);
        }

        /* Čtecí registry (3 ks) */
        for (int j = 0; j < 3; j++) {
            char reg_name[16];
            snprintf(reg_name, sizeof(reg_name), "Rreg_%d", j);
            snapshot_xml_write_hex8(w, reg_name, g_qdisk.channel[ch].Rreg[j]);
        }

        /* Adresa registru */
        snapshot_xml_write_uint(w, "REG_addr", (unsigned)g_qdisk.channel[ch].REG_addr);

        snapshot_xml_close_element(w); /* channel_N */
    }

    snapshot_xml_close_element(w); /* qdisk_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "devices/qdisk.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_qdisk_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* Quick Disk entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, "devices/qdisk.xml")) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "devices/qdisk.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("qdisk", "Cannot load devices/qdisk.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("qdisk", "Parse error in devices/qdisk.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "qdisk_state")) {
        SNAP_ERR("qdisk", "Missing element qdisk_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Základní stav */
    snapshot_xml_read_uint(r, "connected", &g_qdisk.connected);
    snapshot_xml_read_uint(r, "type", &g_qdisk.type);
    snapshot_xml_read_uint(r, "status", &g_qdisk.status);
    snapshot_xml_read_uint(r, "image_position", &g_qdisk.image_position);

    uint16_t hval16;
    if (snapshot_xml_read_hex16(r, "out_crc16", &hval16)) g_qdisk.out_crc16 = hval16;

    /* Fáze 4: informativní pole. Mount už proběhl v qdisk_init() z INI,
     * snapshot tyto pole jen přepíše hodnotou v čase snapshotu (pokud
     * je přítomná); pokud chybí (starý snapshot), runtime hodnoty z
     * mountu zůstanou neměnné. `readonly` = effective (= user_readonly
     * || fs_readonly v čase snapshotu); user_readonly a fs_readonly se
     * neukládají (jsou v INI / runtime detect). */
    int rdo;
    if (snapshot_xml_read_int(r, "readonly", &rdo)) {
        g_qdisk.readonly = rdo;
    }
    char *fname = NULL;
    if (snapshot_xml_read_string(r, "filename", &fname)) {
        if (fname && fname[0] != '\0') {
            g_strlcpy(g_qdisk.filename, fname, sizeof(g_qdisk.filename));
        }
        g_free(fname);
    }

    /* SIO kanály */
    for (int ch = 0; ch < 2; ch++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "channel_%d", ch);

        if (snapshot_xml_enter_element(r, elem_name)) {
            uint8_t hval8;

            /* Zápisové registry */
            for (int j = 0; j < 8; j++) {
                char reg_name[16];
                snprintf(reg_name, sizeof(reg_name), "Wreg_%d", j);
                if (snapshot_xml_read_hex8(r, reg_name, &hval8))
                    g_qdisk.channel[ch].Wreg[j] = hval8;
            }

            /* Čtecí registry */
            for (int j = 0; j < 3; j++) {
                char reg_name[16];
                snprintf(reg_name, sizeof(reg_name), "Rreg_%d", j);
                if (snapshot_xml_read_hex8(r, reg_name, &hval8))
                    g_qdisk.channel[ch].Rreg[j] = hval8;
            }

            /* Adresa registru */
            unsigned uval;
            if (snapshot_xml_read_uint(r, "REG_addr", &uval))
                g_qdisk.channel[ch].REG_addr = (en_QDSIO_REGADRR)uval;

            snapshot_xml_leave_element(r); /* channel_N */
        }
    }

    snapshot_xml_leave_element(r); /* qdisk_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_qdisk_register(void)
{
    /* Platformy bez HW registraci preskoci - snapshot pak sekci
     * neobsahuje (zmrazeny on-disk kontrakt, shodne s drivejsim
     * compile-time #if). */
    if (!g_mzhal.have_qdisk)
        return;

    snapshot_register_component("qdisk",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_qdisk_save,
                                snap_qdisk_load,
                                true);
}


