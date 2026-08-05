/**
 * @file snap_pioz80.c
 * @brief Snapshot handler: PIO Z80 — uložení a načtení stavu (jen MZ-800)
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

#include "hw-generic/pioz80/pioz80.h"

/* Názvy elementů pro porty */
static const char *port_names[PIOZ80_PORT_COUNT] = { "port_0", "port_1" };


/**
 * Uložení jednoho portu PIO Z80 do XML
 */
static void snap_pioz80_save_port(snapshot_xml_writer_t *w, const st_PIOZ80_PORT *port)
{
    snapshot_xml_write_uint(w, "mode", (unsigned)port->mode);
    snapshot_xml_write_hex8(w, "io_mask", port->io_mask);
    snapshot_xml_write_uint(w, "ctrl_expect", (unsigned)port->ctrl_expect);
    snapshot_xml_write_hex8(w, "data_output", port->data_output);
    snapshot_xml_write_hex8(w, "masked_input", port->masked_input);
    snapshot_xml_write_hex8(w, "interrupt_vector", port->interrupt_vector);

    /* Řízení přerušení */
    snapshot_xml_write_uint(w, "icfnc", (unsigned)port->icfnc);
    snapshot_xml_write_uint(w, "iclvl", (unsigned)port->iclvl);
    snapshot_xml_write_hex8(w, "icmask", port->icmask);
    snapshot_xml_write_uint(w, "icena", (unsigned)port->icena);

    /* Stav přerušení portu */
    snapshot_xml_write_uint(w, "port_int", (unsigned)port->port_int);
    snapshot_xml_write_uint(w, "last_intfnc_result", (unsigned)port->last_intfnc_result);
}


/**
 * Načtení jednoho portu PIO Z80 z XML
 */
static void snap_pioz80_load_port(snapshot_xml_reader_t *r, st_PIOZ80_PORT *port)
{
    unsigned uval;
    uint8_t bval;

    if (snapshot_xml_read_uint(r, "mode", &uval)) port->mode = (en_PIOZ80_PORT_MODE)uval;
    if (snapshot_xml_read_hex8(r, "io_mask", &bval)) port->io_mask = bval;
    if (snapshot_xml_read_uint(r, "ctrl_expect", &uval)) port->ctrl_expect = (en_PIOZ80_EXPECT)uval;
    if (snapshot_xml_read_hex8(r, "data_output", &bval)) port->data_output = bval;
    if (snapshot_xml_read_hex8(r, "masked_input", &bval)) port->masked_input = bval;
    if (snapshot_xml_read_hex8(r, "interrupt_vector", &bval)) port->interrupt_vector = bval;

    /* Řízení přerušení */
    if (snapshot_xml_read_uint(r, "icfnc", &uval)) port->icfnc = (en_PIOZ80_ICFNC)uval;
    if (snapshot_xml_read_uint(r, "iclvl", &uval)) port->iclvl = (en_PIOZ80_ICLVL)uval;
    if (snapshot_xml_read_hex8(r, "icmask", &bval)) port->icmask = bval;
    if (snapshot_xml_read_uint(r, "icena", &uval)) port->icena = (en_PIOZ80_ICENA)uval;

    /* Stav přerušení portu */
    if (snapshot_xml_read_uint(r, "port_int", &uval)) port->port_int = (en_PIOZ80_PORT_INT)uval;
    if (snapshot_xml_read_uint(r, "last_intfnc_result", &uval)) port->last_intfnc_result = (en_PIOZ80_ICFNC_RESULT)uval;
}


static en_SNAPSHOT_RESULT snap_pioz80_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "pioz80_state");

    /* Uložení obou portů (A a B) */
    for (int i = 0; i < PIOZ80_PORT_COUNT; i++) {
        snapshot_xml_open_element(w, port_names[i]);
        snap_pioz80_save_port(w, &g_pioz80.port[i]);
        snapshot_xml_close_element(w); /* port_N */
    }

    /* Globální stav přerušení */
    snapshot_xml_write_uint(w, "interrupt", (unsigned)g_pioz80.interrupt);
    snapshot_xml_write_uint(w, "interrupt_port_id", (unsigned)g_pioz80.interrupt_port_id);

    /* Odložená událost ICENA */
    snapshot_xml_open_element(w, "icena_event");
    snapshot_xml_write_int(w, "event_name", (int)g_pioz80.icena_event.event_name);
    snapshot_xml_write_uint(w, "ticks", g_pioz80.icena_event.ticks);
    snapshot_xml_write_uint(w, "port_id", (unsigned)g_pioz80.icena_event_port_id);
    snapshot_xml_close_element(w); /* icena_event */

    snapshot_xml_close_element(w); /* pioz80_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/pioz80.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_pioz80_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/pioz80.xml", &xml);
    if (res != SNAPSHOT_OK) {
        /* PIO Z80 je volitelná komponenta — chybějící soubor není chyba */
        SNAP_WARN("pioz80", "Cannot load hw/pioz80.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("pioz80", "Parse error in hw/pioz80.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "pioz80_state")) {
        SNAP_ERR("pioz80", "Missing element pioz80_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Načtení obou portů */
    for (int i = 0; i < PIOZ80_PORT_COUNT; i++) {
        if (snapshot_xml_enter_element(r, port_names[i])) {
            snap_pioz80_load_port(r, &g_pioz80.port[i]);
            snapshot_xml_leave_element(r);
        } else {
            SNAP_WARN("pioz80", "Missing element %s", port_names[i]);
        }
    }

    /* Globální stav přerušení */
    {
        unsigned uval;
        if (snapshot_xml_read_uint(r, "interrupt", &uval))
            g_pioz80.interrupt = (en_PIOZ80_INTERRUPT)uval;
        if (snapshot_xml_read_uint(r, "interrupt_port_id", &uval))
            g_pioz80.interrupt_port_id = (en_PIOZ80_PORT_ID)uval;
    }

    /* Odložená událost ICENA */
    if (snapshot_xml_enter_element(r, "icena_event")) {
        int ival;
        unsigned uval;
        if (snapshot_xml_read_int(r, "event_name", &ival))
            g_pioz80.icena_event.event_name = (en_MZEVENT)ival;
        if (snapshot_xml_read_uint(r, "ticks", &uval))
            g_pioz80.icena_event.ticks = uval;
        if (snapshot_xml_read_uint(r, "port_id", &uval))
            g_pioz80.icena_event_port_id = (en_PIOZ80_PORT_ID)uval;
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_leave_element(r); /* pioz80_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_pioz80_register(void)
{
    /* Platformy bez PIO-Z80 (MZ-700) registraci preskoci - snapshot
     * pak sekci neobsahuje (zmrazeny on-disk kontrakt, shodne
     * s drivejsim compile-time #if). */
    if (!g_mzhal.have_pioz80)
        return;

    snapshot_register_component("pioz80",
                                SNAPSHOT_PRIORITY_HW_IO,
                                snap_pioz80_save,
                                snap_pioz80_load,
                                true);
}


