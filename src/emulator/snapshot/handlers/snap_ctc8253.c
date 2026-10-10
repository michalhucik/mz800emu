/**
 * @file snap_ctc8253.c
 * @brief Snapshot handler: CTC 8253 — uložení a načtení stavu všech 3 čítačů
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/ctc8253/ctc8253.h"

/* Názvy elementů pro jednotlivé čítače */
static const char *counter_names[3] = { "counter_0", "counter_1", "counter_2" };


static en_SNAPSHOT_RESULT snap_ctc8253_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "ctc8253_state");

    /* Uložení všech 3 čítačů */
    for (int i = 0; i < 3; i++) {
        st_CTC8253 *ctc = &g_ctc8253[i];

        snapshot_xml_open_element(w, counter_names[i]);

        /* Stavové registry */
        snapshot_xml_write_uint(w, "out", ctc->out);
        snapshot_xml_write_uint(w, "gate", ctc->gate);
        snapshot_xml_write_uint(w, "mode", (unsigned)ctc->mode);
        snapshot_xml_write_uint(w, "bcd", ctc->bcd);
        snapshot_xml_write_uint(w, "rlf", (unsigned)ctc->rlf);
        snapshot_xml_write_uint(w, "state", (unsigned)ctc->state);
        snapshot_xml_write_uint(w, "load_done", ctc->load_done);
        snapshot_xml_write_uint(w, "load_wait_rise", ctc->load_wait_rise);
        snapshot_xml_write_uint(w, "rl_byte", ctc->rl_byte);
        snapshot_xml_write_uint(w, "latch_op", ctc->latch_op);

        /* Hodnoty čítačů — hex16 */
        snapshot_xml_write_hex16(w, "read_latch", (uint16_t)ctc->read_latch);
        snapshot_xml_write_hex16(w, "preset_value", (uint16_t)ctc->preset_value);
        snapshot_xml_write_hex16(w, "preset_latch", (uint16_t)ctc->preset_latch);
        snapshot_xml_write_hex16(w, "value", (uint16_t)ctc->value);
        snapshot_xml_write_hex16(w, "mode3_destination_value", (uint16_t)ctc->mode3_destination_value);
        snapshot_xml_write_hex16(w, "mode3_half_value", (uint16_t)ctc->mode3_half_value);

        snapshot_xml_close_element(w); /* counter_N */
    }

    snapshot_xml_close_element(w); /* ctc8253_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/ctc8253.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_ctc8253_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/ctc8253.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("ctc8253", "Cannot load hw/ctc8253.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("ctc8253", "Parse error in hw/ctc8253.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "ctc8253_state")) {
        SNAP_ERR("ctc8253", "Missing element ctc8253_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Načtení všech 3 čítačů */
    for (int i = 0; i < 3; i++) {
        if (!snapshot_xml_enter_element(r, counter_names[i])) {
            SNAP_WARN("ctc8253", "Missing element %s", counter_names[i]);
            continue;
        }

        st_CTC8253 *ctc = &g_ctc8253[i];
        unsigned uval;
        uint16_t hval;

        /* Stavové registry */
        if (snapshot_xml_read_uint(r, "out", &uval)) ctc->out = uval;
        if (snapshot_xml_read_uint(r, "gate", &uval)) ctc->gate = uval;
        if (snapshot_xml_read_uint(r, "mode", &uval)) ctc->mode = (en_CTC_MODE)uval;
        if (snapshot_xml_read_uint(r, "bcd", &uval)) ctc->bcd = uval;
        if (snapshot_xml_read_uint(r, "rlf", &uval)) ctc->rlf = (en_CTC_RLF)uval;
        if (snapshot_xml_read_uint(r, "state", &uval)) ctc->state = (en_CTC_STATE)uval;
        if (snapshot_xml_read_uint(r, "load_done", &uval)) ctc->load_done = uval;
        /* od opravy nahrání 8253 (2026-10); starší snapshot = 0 */
        ctc->load_wait_rise = 0;
        if (snapshot_xml_read_uint(r, "load_wait_rise", &uval)) ctc->load_wait_rise = uval;
        if (snapshot_xml_read_uint(r, "rl_byte", &uval)) ctc->rl_byte = uval;
        if (snapshot_xml_read_uint(r, "latch_op", &uval)) ctc->latch_op = uval;

        /* Hodnoty čítačů — hex16 */
        if (snapshot_xml_read_hex16(r, "read_latch", &hval)) ctc->read_latch = hval;
        if (snapshot_xml_read_hex16(r, "preset_value", &hval)) ctc->preset_value = hval;
        if (snapshot_xml_read_hex16(r, "preset_latch", &hval)) ctc->preset_latch = hval;
        if (snapshot_xml_read_hex16(r, "value", &hval)) ctc->value = hval;
        if (snapshot_xml_read_hex16(r, "mode3_destination_value", &hval)) ctc->mode3_destination_value = hval;
        if (snapshot_xml_read_hex16(r, "mode3_half_value", &hval)) ctc->mode3_half_value = hval;

        snapshot_xml_leave_element(r); /* counter_N */
    }

    snapshot_xml_leave_element(r); /* ctc8253_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_ctc8253_register(void)
{
    snapshot_register_component("ctc8253",
                                SNAPSHOT_PRIORITY_HW_IO,
                                snap_ctc8253_save,
                                snap_ctc8253_load,
                                false);
}
