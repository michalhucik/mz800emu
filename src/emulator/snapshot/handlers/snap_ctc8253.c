/**
 * @file snap_ctc8253.c
 * @brief Snapshot handler: CTC 8253 — uložení a načtení stavu všech 3 čítačů
 *
 * U CTC0 v rychlé cestě (MZ800EMU_CFG_CLK1M1_FAST) ukládá i naplánovanou
 * událost a bázi líného dopočtu hodnoty; bez nich by po loadu CTC0 čítal
 * podle času stroje před loadem.
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
        snapshot_xml_write_uint(w, "wr_half", ctc->wr_half);
        snapshot_xml_write_uint(w, "rl_byte", ctc->rl_byte);
        snapshot_xml_write_uint(w, "latch_op", ctc->latch_op);

        /* Hodnoty čítačů — hex16 */
        snapshot_xml_write_hex16(w, "read_latch", (uint16_t)ctc->read_latch);
        snapshot_xml_write_hex16(w, "preset_value", (uint16_t)ctc->preset_value);
        snapshot_xml_write_hex16(w, "preset_latch", (uint16_t)ctc->preset_latch);
        snapshot_xml_write_hex16(w, "value", (uint16_t)ctc->value);
        snapshot_xml_write_hex16(w, "mode3_destination_value", (uint16_t)ctc->mode3_destination_value);
        snapshot_xml_write_hex16(w, "mode3_half_value", (uint16_t)ctc->mode3_half_value);

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        /* CTC0 rychlá cesta: naplánovaná událost a báze líného dopočtu
         * value (value je uložená k této bázi, ne dopočtená). Obojí je
         * v čase GDG, který snapshot obnovuje (total_elapsed). */
        if (i == CTC_CS0) {
            snapshot_xml_write_uint(w, "clk1m1_event_ticks", ctc->clk1m1_event.ticks);
            snapshot_xml_write_uint(w, "clk1m1_last_event_total_ticks",
                                    ctc->clk1m1_last_event_total_ticks);
        }
#endif

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
        /* od opravy 8253 podle měření (2026-11); starší snapshot = 0 */
        ctc->wr_half = 0;
        if (snapshot_xml_read_uint(r, "wr_half", &uval)) ctc->wr_half = uval;
        if (snapshot_xml_read_uint(r, "latch_op", &uval)) ctc->latch_op = uval;

        /* Hodnoty čítačů — hex16 */
        if (snapshot_xml_read_hex16(r, "read_latch", &hval)) ctc->read_latch = hval;
        if (snapshot_xml_read_hex16(r, "preset_value", &hval)) ctc->preset_value = hval;
        if (snapshot_xml_read_hex16(r, "preset_latch", &hval)) ctc->preset_latch = hval;
        if (snapshot_xml_read_hex16(r, "value", &hval)) ctc->value = hval;
        if (snapshot_xml_read_hex16(r, "mode3_destination_value", &hval)) ctc->mode3_destination_value = hval;
        if (snapshot_xml_read_hex16(r, "mode3_half_value", &hval)) ctc->mode3_half_value = hval;

        /* Hodnoty se ukládají v 16 bitech: dělič 0 (65 536, v BCD 10 000)
         * a režim 3 s děličem 1 (65 537) mají interně 17 bitů. */
        if (ctc->preset_value == 0) ctc->preset_value = 0x10000;
        if ((ctc->mode == CTC_MODE3) && (ctc->preset_value == 1)) ctc->preset_value = 0x10001;
        if ((ctc->value == 0) && (ctc->state == CTC_STATE_COUNTDOWN)) ctc->value = 0x10000;

#ifdef MZ800EMU_CFG_CLK1M1_FAST
        /* CTC0 rychlá cesta; snapshot bez události (starší verze) ji
         * naplánuje na nejbližší hranu CLK0 (GDG je už obnovený).
         * Snapshot MZ-800 uložený před změnou fáze CLK0 (do 883abab8, hrana
         * při total_ticks % 16 == 0) pokračuje ve staré fázi, dokud se
         * událost nepřeplánuje (nahrání děliče, hrana GATE0); násilné
         * přeplánování by mohlo ztratit hrany od poslední zpracované do
         * okamžiku uložení. Rozdíl nejvýš 10 tiků, jen u takových snapshotů. */
        if (i == CTC_CS0) {
            unsigned ev_ticks, base;
            if (snapshot_xml_read_uint(r, "clk1m1_event_ticks", &ev_ticks)
                && snapshot_xml_read_uint(r, "clk1m1_last_event_total_ticks", &base)) {
                ctc->clk1m1_event.event_name = MZEVENT_CTC0;
                ctc->clk1m1_event.ticks = ev_ticks;
                ctc->clk1m1_last_event_total_ticks = base;
            } else {
                ctc8253_ctc0_restart_event();
            }
        }
#endif

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
