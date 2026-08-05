/**
 * @file snap_fdc.c
 * @brief Snapshot handler: FDC floppy - uložení a načtení stavu řadiče
 *        floppy disků.
 *
 * Ukládá stav chipu WD279x (registry, state machine) a per-drive
 * informace (mount, R/O flag, filename). Mount cesty disků se primárně
 * obnovují z cfgfile (`wd279x_fddX_dskpath`); snapshot ukládá filename
 * a flagy pouze pro informaci a kontinuitu.
 *
 * ## Zpětná kompatibilita
 *
 * Historicky existovaly dvě implementační varianty FDC (_old_/_new_)
 * a snapshot na nejvyšší úrovni nesl atribut `impl="old|new"` v elementu
 * `<fdc_state>`. OLD byla odstraněna, NEW je jediná. Nové snapshoty
 * atribut `impl` nezapisují (čistší formát). Při loadu:
 *  - chybějící `impl` nebo `impl="new"` -> standardní NEW load
 *  - `impl="old"` -> log warning a skip (staré OLD snapshoty nelze
 *    obnovit, ale nepadne)
 */

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "hw-generic/fdc/fdc.h"


/* ============================================================
 * Save (always NEW)
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_save_instance(st_SNAPSHOT_CONTEXT *ctx, st_FDC *fdc,
                                                 const char *xml_entry, const char *buf_entry)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    /* Nové snapshoty nezapisují atribut `impl` - existuje jen jedna impl. */
    snapshot_xml_open_element(w, "fdc_state");

    snapshot_xml_write_uint(w, "connected", fdc->connected);

    /* Config switche (hd_patch, bus_xlate) - ukládáme pro informativní
     * účely; jsou ovládané CLI/cfgfile, nemusí se při restore aplikovat. */
    snapshot_xml_write_int(w, "hd_patch", fdc->hd_patch);
    snapshot_xml_write_int(w, "bus_xlate", (int)fdc->bus_xlate);

    /* Registry chipu (true-bus hodnoty). */
    snapshot_xml_open_element(w, "wd279x");

    snapshot_xml_write_hex8(w, "regSTATUS", fdc->wd279x.regSTATUS);
    snapshot_xml_write_hex8(w, "regCOMMAND", fdc->wd279x.regCOMMAND);
    snapshot_xml_write_hex8(w, "regTRACK", fdc->wd279x.regTRACK);
    snapshot_xml_write_hex8(w, "regSECTOR", fdc->wd279x.regSECTOR);
    snapshot_xml_write_hex8(w, "regDATA", fdc->wd279x.regDATA);

    snapshot_xml_write_hex8(w, "MOTOR", fdc->wd279x.MOTOR);
    snapshot_xml_write_hex8(w, "SIDE", fdc->wd279x.SIDE);
    snapshot_xml_write_hex8(w, "DENSITY", fdc->wd279x.DENSITY);
    snapshot_xml_write_hex8(w, "EINT", fdc->wd279x.EINT);

    snapshot_xml_write_hex16(w, "buffer_pos", fdc->wd279x.buffer_pos);
    snapshot_xml_write_hex16(w, "data_counter", fdc->wd279x.data_counter);
    snapshot_xml_write_hex16(w, "current_sector_size", fdc->wd279x.current_sector_size);

    snapshot_xml_write_int(w, "status_mode", (int)fdc->wd279x.status_mode);

    snapshot_xml_write_hex8(w, "multiblock_rw", fdc->wd279x.multiblock_rw);
    snapshot_xml_write_int(w, "direction_latch", (int)fdc->wd279x.direction_latch);
    snapshot_xml_write_hex8(w, "intrq_active", fdc->wd279x.intrq_active);
    snapshot_xml_write_hex8(w, "reading_status_counter", fdc->wd279x.reading_status_counter);
    snapshot_xml_write_hex8(w, "pending_busy_status", fdc->wd279x.pending_busy_status);
    snapshot_xml_write_hex8(w, "pending_drq", fdc->wd279x.pending_drq);
    snapshot_xml_write_hex8(w, "positioned_sector", fdc->wd279x.positioned_sector);
    snapshot_xml_write_hex8(w, "positioned_track", fdc->wd279x.positioned_track);
    snapshot_xml_write_hex8(w, "positioned_side", fdc->wd279x.positioned_side);
    snapshot_xml_write_hex8(w, "waitForInt", fdc->wd279x.waitForInt);
    snapshot_xml_write_hex8(w, "write_track_stage", fdc->wd279x.write_track_stage);
    snapshot_xml_write_hex16(w, "write_track_counter", fdc->wd279x.write_track_counter);
    snapshot_xml_write_hex8(w, "rt_sectors", fdc->wd279x.rt_sectors);
    snapshot_xml_write_hex16(w, "rt_sector_bytes", fdc->wd279x.rt_sector_bytes);
    snapshot_xml_write_hex8(w, "rt_ssize_code", fdc->wd279x.rt_ssize_code);
    snapshot_xml_write_int(w, "rt_cached_sec_idx", (int)fdc->wd279x.rt_cached_sec_idx);

    /* Mechaniky */
    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        char elem_name[16];
        snprintf(elem_name, sizeof(elem_name), "drive_%d", i);
        snapshot_xml_open_element(w, elem_name);

        snapshot_xml_write_uint(w, "mounted", fdc->drive[i].mounted);
        snapshot_xml_write_int(w, "readonly", fdc->drive[i].readonly);
        snapshot_xml_write_string(w, "filename", fdc->drive[i].filename);

        snapshot_xml_close_element(w); /* drive_N */
    }

    snapshot_xml_close_element(w); /* wd279x */
    snapshot_xml_close_element(w); /* fdc_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, xml_entry, xml);
    g_free(xml);
    if (res != SNAPSHOT_OK) return res;

    /* Buffer chipu jako binární soubor. */
    res = snapshot_io_write_bin(ctx->io, buf_entry,
                                fdc->wd279x.buffer, WD279X_BUFFER_SIZE);

    return res;
}


/* ============================================================
 * Load (NEW)
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_load_instance(st_SNAPSHOT_CONTEXT *ctx,
                                                 snapshot_xml_reader_t *r,
                                                 st_FDC *fdc, const char *buf_entry)
{
    /* Předpoklad: caller už vstoupil do <fdc_state>. */

    snapshot_xml_read_uint(r, "connected", &fdc->connected);

    /* hd_patch / bus_xlate jsou informativní - aktivní hodnoty jsou
     * z cfgfile/CLI, ne ze snapshotu. */

    if (snapshot_xml_enter_element(r, "wd279x")) {
        int ival;
        uint8_t hval8;
        uint16_t hval16;

        if (snapshot_xml_read_hex8(r, "regSTATUS", &hval8)) fdc->wd279x.regSTATUS = hval8;
        if (snapshot_xml_read_hex8(r, "regCOMMAND", &hval8)) fdc->wd279x.regCOMMAND = hval8;
        if (snapshot_xml_read_hex8(r, "regTRACK", &hval8)) fdc->wd279x.regTRACK = hval8;
        if (snapshot_xml_read_hex8(r, "regSECTOR", &hval8)) fdc->wd279x.regSECTOR = hval8;
        if (snapshot_xml_read_hex8(r, "regDATA", &hval8)) fdc->wd279x.regDATA = hval8;

        if (snapshot_xml_read_hex8(r, "MOTOR", &hval8)) fdc->wd279x.MOTOR = hval8;
        if (snapshot_xml_read_hex8(r, "SIDE", &hval8)) fdc->wd279x.SIDE = hval8;
        if (snapshot_xml_read_hex8(r, "DENSITY", &hval8)) fdc->wd279x.DENSITY = hval8;
        if (snapshot_xml_read_hex8(r, "EINT", &hval8)) fdc->wd279x.EINT = hval8;

        if (snapshot_xml_read_hex16(r, "buffer_pos", &hval16)) fdc->wd279x.buffer_pos = hval16;
        if (snapshot_xml_read_hex16(r, "data_counter", &hval16)) fdc->wd279x.data_counter = hval16;
        if (snapshot_xml_read_hex16(r, "current_sector_size", &hval16)) fdc->wd279x.current_sector_size = hval16;

        if (snapshot_xml_read_int(r, "status_mode", &ival)) {
            fdc->wd279x.status_mode = (en_WD279X_STATUS_MODE)ival;
        }

        if (snapshot_xml_read_hex8(r, "multiblock_rw", &hval8)) fdc->wd279x.multiblock_rw = hval8;
        if (snapshot_xml_read_int(r, "direction_latch", &ival)) fdc->wd279x.direction_latch = (int8_t)ival;
        if (snapshot_xml_read_hex8(r, "intrq_active", &hval8)) fdc->wd279x.intrq_active = hval8;
        if (snapshot_xml_read_hex8(r, "reading_status_counter", &hval8)) fdc->wd279x.reading_status_counter = hval8;
        if (snapshot_xml_read_hex8(r, "pending_busy_status", &hval8)) fdc->wd279x.pending_busy_status = hval8;
        if (snapshot_xml_read_hex8(r, "pending_drq", &hval8)) fdc->wd279x.pending_drq = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_sector", &hval8)) fdc->wd279x.positioned_sector = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_track", &hval8)) fdc->wd279x.positioned_track = hval8;
        if (snapshot_xml_read_hex8(r, "positioned_side", &hval8)) fdc->wd279x.positioned_side = hval8;
        if (snapshot_xml_read_hex8(r, "waitForInt", &hval8)) fdc->wd279x.waitForInt = hval8;
        if (snapshot_xml_read_hex8(r, "write_track_stage", &hval8)) fdc->wd279x.write_track_stage = hval8;
        if (snapshot_xml_read_hex16(r, "write_track_counter", &hval16)) fdc->wd279x.write_track_counter = hval16;
        if (snapshot_xml_read_hex8(r, "rt_sectors", &hval8)) fdc->wd279x.rt_sectors = hval8;
        if (snapshot_xml_read_hex16(r, "rt_sector_bytes", &hval16)) fdc->wd279x.rt_sector_bytes = hval16;
        if (snapshot_xml_read_hex8(r, "rt_ssize_code", &hval8)) fdc->wd279x.rt_ssize_code = hval8;
        if (snapshot_xml_read_int(r, "rt_cached_sec_idx", &ival)) fdc->wd279x.rt_cached_sec_idx = (int8_t)ival;

        for (int i = 0; i < FDC_NUM_DRIVES; i++) {
            char elem_name[16];
            snprintf(elem_name, sizeof(elem_name), "drive_%d", i);

            if (snapshot_xml_enter_element(r, elem_name)) {
                /* Drive filename: pokud byl drive mounted v snapshotu,
                 * cesta je v cfgfile - mount se obnoví standardní init
                 * cestou. Zde pouze obnovíme readonly flag a filename
                 * jako informaci pro UI. */
                unsigned uval;
                int rdo;
                if (snapshot_xml_read_uint(r, "mounted", &uval)) {
                    fdc->drive[i].mounted = uval;
                }
                if (snapshot_xml_read_int(r, "readonly", &rdo)) {
                    fdc->drive[i].readonly = rdo;
                }

                char *fname = NULL;
                if (snapshot_xml_read_string(r, "filename", &fname)) {
                    if (fname && fname[0] != '\0') {
                        strncpy(fdc->drive[i].filename, fname,
                                sizeof(fdc->drive[i].filename) - 1);
                        fdc->drive[i].filename[sizeof(fdc->drive[i].filename) - 1] = '\0';
                    }
                    g_free(fname);
                }

                snapshot_xml_leave_element(r); /* drive_N */
            }
        }

        snapshot_xml_leave_element(r); /* wd279x */
    }

    /* Buffer chipu - load do fdc->wd279x.buffer. */
    if (snapshot_io_entry_exists(ctx->io, buf_entry)) {
        en_SNAPSHOT_RESULT res = snapshot_io_read_bin_into(
            ctx->io, buf_entry,
            fdc->wd279x.buffer, WD279X_BUFFER_SIZE);
        if (res != SNAPSHOT_OK) {
            SNAP_WARN("fdc", "Cannot load FDC buffer (size mismatch or I/O error)");
        }
    }

    return SNAPSHOT_OK;
}


/* ============================================================
 * Top-level dispatch
 * ============================================================ */

static en_SNAPSHOT_RESULT snap_fdc_save(st_SNAPSHOT_CONTEXT *ctx)
{
    /* FDC0 - historické entry devices/fdc.xml (zpětná kompatibilita 2A). */
    en_SNAPSHOT_RESULT res = snap_fdc_save_instance(
        ctx, &g_fdc[FDC0], "devices/fdc.xml", "devices/fdc_buffer.bin");
    if (res != SNAPSHOT_OK) return res;

    /* FDC1 - nové entry devices/fdc1.xml + devices/fdc1_buffer.bin. */
    res = snap_fdc_save_instance(
        ctx, &g_fdc[FDC1], "devices/fdc1.xml", "devices/fdc1_buffer.bin");
    return res;
}


/**
 * @brief Načte stav jedné instance FDC z daného XML entry.
 *
 * Entry je volitelný - když chybí (např. starý snapshot bez FDC1), vrátí
 * OK a instance zůstane v default stavu po init.
 *
 * @param ctx       snapshot kontext.
 * @param fdc       cílová instance FDC.
 * @param xml_entry název XML entry (devices/fdc.xml / devices/fdc1.xml).
 * @param buf_entry název binárního buffer entry.
 * @return SNAPSHOT_OK nebo chyba parse/IO.
 */
static en_SNAPSHOT_RESULT snap_fdc_load_one(st_SNAPSHOT_CONTEXT *ctx, st_FDC *fdc,
                                            const char *xml_entry, const char *buf_entry)
{
    /* FDC entry je volitelný */
    if (!snapshot_io_entry_exists(ctx->io, xml_entry)) {
        return SNAPSHOT_OK;
    }

    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, xml_entry, &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("fdc", "Cannot load FDC xml entry");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("fdc", "Parse error in FDC xml entry");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "fdc_state")) {
        SNAP_ERR("fdc", "Missing element fdc_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Zpětná kompatibilita: starší snapshoty (před odstraněním OLD impl)
     * měly v <fdc_state> atribut `impl="old|new"`. Nové snapshoty atribut
     * nezapisují (čistší formát). Při loadu:
     *  - chybějící atribut nebo `impl="new"` -> standardní load
     *  - `impl="old"` -> log warning a skip (= staré OLD snapshoty nelze
     *    obnovit, ale neshazujeme load celého snapshotu) */
    char *impl_attr = NULL;
    if (snapshot_xml_read_attr(r, "impl", &impl_attr) && impl_attr) {
        if (strcmp(impl_attr, "old") == 0) {
            SNAP_WARN("fdc",
                "Snapshot FDC impl='old' is no longer supported - skipping FDC restore");
            g_free(impl_attr);
            snapshot_xml_leave_element(r); /* fdc_state */
            snapshot_xml_reader_free(r);
            return SNAPSHOT_OK;
        }
        g_free(impl_attr);
    }

    res = snap_fdc_load_instance(ctx, r, fdc, buf_entry);

    snapshot_xml_leave_element(r); /* fdc_state */
    snapshot_xml_reader_free(r);

    return res;
}


static en_SNAPSHOT_RESULT snap_fdc_load(st_SNAPSHOT_CONTEXT *ctx)
{
    /* FDC0 - historické entry. */
    en_SNAPSHOT_RESULT res = snap_fdc_load_one(
        ctx, &g_fdc[FDC0], "devices/fdc.xml", "devices/fdc_buffer.bin");
    if (res != SNAPSHOT_OK) return res;

    /* FDC1 - nové entry; starý snapshot ho nemá -> FDC1 zůstane default. */
    res = snap_fdc_load_one(
        ctx, &g_fdc[FDC1], "devices/fdc1.xml", "devices/fdc1_buffer.bin");
    return res;
}


void snap_fdc_register(void)
{
    /* Platformy bez HW registraci preskoci - snapshot pak sekci
     * neobsahuje (zmrazeny on-disk kontrakt, shodne s drivejsim
     * compile-time #if). */
    if (!g_mzhal.have_fdc)
        return;

    snapshot_register_component("fdc",
                                SNAPSHOT_PRIORITY_DEVICE,
                                snap_fdc_save,
                                snap_fdc_load,
                                true);
}


