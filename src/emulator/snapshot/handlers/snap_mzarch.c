/**
 * @file snap_mzarch.c
 * @brief Snapshot handler: hlavní stav mzarch_main (řídicí struktura emulátoru)
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "mzarch/mzhal.h"
#include "snapshot/snapshot_xml.h"
#include "mzarch/mzarch.h"

static en_SNAPSHOT_RESULT snap_mzarch_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "mzarch_state");

    /* Hlavní stav emulátoru */
    snapshot_xml_write_uint(w, "cursor_timer", g_mzarch_main.cursor_timer);
    snapshot_xml_write_hex16(w, "instruction_addr", g_mzarch_main.instruction_addr);
    snapshot_xml_write_int(w, "instruction_tstates", g_mzarch_main.instruction_tstates);
    snapshot_xml_write_int(w, "instruction_insideop_sync_ticks",
                           g_mzarch_main.instruction_insideop_sync_ticks);

    /* Datová sběrnice a PIO */
    snapshot_xml_write_hex8(w, "regDBUS_latch", g_mzarch_main.regDBUS_latch);
    snapshot_xml_write_int(w, "pio8255_ct53g7", g_mzarch_main.pio8255_ct53g7);

    /* Event */
    snapshot_xml_open_element(w, "event");
    snapshot_xml_write_int(w, "event_name", (int)g_mzarch_main.event.event_name);
    snapshot_xml_write_uint(w, "ticks", g_mzarch_main.event.ticks);
    snapshot_xml_close_element(w); /* event */

    /* Přerušení */
    snapshot_xml_write_uint(w, "interrupt", g_mzarch_main.interrupt);

    /* HW kompatibilita - pouze MZ-800 ma konfigurovatelne HW experimenty */
    /* Runtime dle g_mzhal (mzhal 11e), klice per arch zmrazene. */
    if (g_mzhal.arch == 800) {
    snapshot_xml_open_element(w, "hw_compatibility");
    snapshot_xml_write_int(w, "mz800_hwcompat_allow_psg1",
                           (int)g_mzarch_main.mz800_hwcompat_allow_psg1);
    snapshot_xml_close_element(w); /* hw_compatibility */
    }

    /* MZ-700 kompatibilní přepínač - jen MZ-800/MZ-1500;
     * MZ-700 nativní field switch700 nemá. */
    if (g_mzhal.has_rear_dip_switch700) {
        snapshot_xml_write_int(w, "switch700",
                               (int)g_mzarch_main.switch700);
    }

    snapshot_xml_close_element(w); /* mzarch_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/mzarch.xml", xml);
    g_free(xml);

    return res;
}

static en_SNAPSHOT_RESULT snap_mzarch_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/mzarch.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("mzarch", "Cannot load hw/mzarch.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("mzarch", "Parse error in hw/mzarch.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "mzarch_state")) {
        SNAP_ERR("mzarch", "Missing element mzarch_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Hlavní stav emulátoru */
    snapshot_xml_read_uint(r, "cursor_timer", &g_mzarch_main.cursor_timer);

    {
        uint16_t addr;
        if (snapshot_xml_read_hex16(r, "instruction_addr", &addr))
            g_mzarch_main.instruction_addr = addr;
    }

    snapshot_xml_read_int(r, "instruction_tstates", &g_mzarch_main.instruction_tstates);
    snapshot_xml_read_int(r, "instruction_insideop_sync_ticks",
                          &g_mzarch_main.instruction_insideop_sync_ticks);

    /* Datová sběrnice a PIO */
    {
        uint8_t dbus;
        if (snapshot_xml_read_hex8(r, "regDBUS_latch", &dbus))
            g_mzarch_main.regDBUS_latch = dbus;
    }

    snapshot_xml_read_int(r, "pio8255_ct53g7", &g_mzarch_main.pio8255_ct53g7);

    /* Event */
    if (snapshot_xml_enter_element(r, "event")) {
        int ev_name;
        if (snapshot_xml_read_int(r, "event_name", &ev_name))
            g_mzarch_main.event.event_name = (en_MZEVENT)ev_name;
        snapshot_xml_read_uint(r, "ticks", &g_mzarch_main.event.ticks);
        snapshot_xml_leave_element(r);
    }

    /* Přerušení */
    snapshot_xml_read_uint(r, "interrupt", &g_mzarch_main.interrupt);

    /* HW kompatibilita - pouze MZ-800 ma konfigurovatelne HW experimenty.
     * Klice mz800_hwcompat_mz700_pal_timing a mz800_hwcompat_mz700_fixed_e008
     * jsou v starsich snapshotech ignorovany (read_int vraci false). */
    if (g_mzhal.arch == 800) {
        if (snapshot_xml_enter_element(r, "hw_compatibility")) {
            int val;
            if (snapshot_xml_read_int(r, "mz800_hwcompat_allow_psg1", &val))
                g_mzarch_main.mz800_hwcompat_allow_psg1 = (en_MZ800_HWCOMPAT_ALLOW_PSG1)val;
            snapshot_xml_leave_element(r);
        }
    }

    /* MZ-700 kompatibilní přepínač - viz save block. */
    if (g_mzhal.has_rear_dip_switch700) {
        int val;
        if (snapshot_xml_read_int(r, "switch700", &val))
            g_mzarch_main.switch700 = (en_SWITCH700)val;
    }

    snapshot_xml_leave_element(r); /* mzarch_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}

void snap_mzarch_register(void)
{
    snapshot_register_component("mzarch",
                                SNAPSHOT_PRIORITY_HW_CORE,
                                snap_mzarch_save,
                                snap_mzarch_load,
                                false);
}
