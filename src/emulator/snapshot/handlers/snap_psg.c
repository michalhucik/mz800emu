/**
 * @file snap_psg.c
 * @brief Snapshot handler: PSG — uložení a načtení zvukového generátoru (mono/stereo)
 *
 * Pro HAVE_PSG=0 (MZ-700) je handler zaregistrovan jako no-op,
 * snapshot pak neobsahuje hw/psg.xml.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/psg/psg.h"
#include "mzarch/mzcommon_config.h"

#include "mzarch/mzhal.h"

/* TU se kompiluje bezpodminecne na vsech platformach (mzhal krok 8);
 * pritomnost sekce ve snapshotu ridi runtime guard v _register(). */

/* Názvy elementů pro PSG instance a kanály */
static const char *psg_names[PSG_MAX_COUNT] = { "psg_0", "psg_1" };
static const char *ch_names[PSG_CHANNELS_COUNT] = { "channel_0", "channel_1", "channel_2", "channel_3" };


/**
 * Uložení jedné PSG instance do XML
 */
static void snap_psg_save_instance(snapshot_xml_writer_t *w, const st_PSG *psg)
{
    snapshot_xml_write_uint(w, "latch_cs", psg->latch_cs);
    snapshot_xml_write_uint(w, "latch_attn", psg->latch_attn);

    for (int ch = 0; ch < PSG_CHANNELS_COUNT; ch++) {
        const st_PSG_CHANNEL *channel = &psg->channel[ch];

        snapshot_xml_open_element(w, ch_names[ch]);

        snapshot_xml_write_uint(w, "type", (unsigned)channel->type);
        snapshot_xml_write_uint(w, "timer", channel->timer);
        snapshot_xml_write_uint(w, "attn", (unsigned)channel->attn);

        /* Tónový generátor */
        snapshot_xml_write_uint(w, "tone_divider", channel->tone.divider);
        snapshot_xml_write_uint(w, "tone_latch_divider", channel->tone.latch_divider);

        /* Šumový generátor */
        snapshot_xml_write_uint(w, "noise_div_type", (unsigned)channel->noise.div_type);
        snapshot_xml_write_uint(w, "noise_type", (unsigned)channel->noise.type);
        snapshot_xml_write_uint(w, "noise_last_type", (unsigned)channel->noise.last_noise_type);
        snapshot_xml_write_hex16(w, "noise_shiftregister", channel->noise.shiftregister);

        snapshot_xml_write_uint(w, "output_signal", channel->output_signal);

        snapshot_xml_close_element(w); /* channel_N */
    }
}


/**
 * Načtení jedné PSG instance z XML
 */
static void snap_psg_load_instance(snapshot_xml_reader_t *r, st_PSG *psg)
{
    unsigned uval;

    snapshot_xml_read_uint(r, "latch_cs", &psg->latch_cs);
    if (snapshot_xml_read_uint(r, "latch_attn", &uval)) psg->latch_attn = uval;

    for (int ch = 0; ch < PSG_CHANNELS_COUNT; ch++) {
        if (!snapshot_xml_enter_element(r, ch_names[ch])) {
            SNAP_WARN("psg", "Missing element %s", ch_names[ch]);
            continue;
        }

        st_PSG_CHANNEL *channel = &psg->channel[ch];
        uint16_t hval;

        if (snapshot_xml_read_uint(r, "type", &uval)) channel->type = (en_PSG_CHTYPE)uval;
        snapshot_xml_read_uint(r, "timer", &channel->timer);
        if (snapshot_xml_read_uint(r, "attn", &uval)) channel->attn = (en_ATTENUATOR)uval;

        /* Tónový generátor */
        snapshot_xml_read_uint(r, "tone_divider", &channel->tone.divider);
        snapshot_xml_read_uint(r, "tone_latch_divider", &channel->tone.latch_divider);

        /* Šumový generátor */
        if (snapshot_xml_read_uint(r, "noise_div_type", &uval)) channel->noise.div_type = (en_NOISE_DIV_TYPE)uval;
        if (snapshot_xml_read_uint(r, "noise_type", &uval)) channel->noise.type = (en_NOISE_TYPE)uval;
        if (snapshot_xml_read_uint(r, "noise_last_type", &uval)) channel->noise.last_noise_type = (en_NOISE_TYPE)uval;
        if (snapshot_xml_read_hex16(r, "noise_shiftregister", &hval)) channel->noise.shiftregister = hval;

        snapshot_xml_read_uint(r, "output_signal", &channel->output_signal);

        snapshot_xml_leave_element(r); /* channel_N */
    }
}


static en_SNAPSHOT_RESULT snap_psg_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "psg_state");

    /* Příznak stereo režimu */
    snapshot_xml_write_bool(w, "stereo", g_psg_module.stereo);

    /* Počet PSG instancí závisí na stereo režimu */
    int psg_count = g_psg_module.stereo ? 2 : 1;

    for (int i = 0; i < psg_count; i++) {
        snapshot_xml_open_element(w, psg_names[i]);
        snap_psg_save_instance(w, &g_psg_module.psg[i]);
        snapshot_xml_close_element(w); /* psg_N */
    }

    snapshot_xml_close_element(w); /* psg_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/psg.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_psg_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/psg.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("psg", "Cannot load hw/psg.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("psg", "Parse error in hw/psg.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "psg_state")) {
        SNAP_ERR("psg", "Missing element psg_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Přečtení stereo příznaku ze snapshotu */
    bool stereo = false;
    snapshot_xml_read_bool(r, "stereo", &stereo);

    /* Načtení PSG0 (levý / mono) — vždy přítomen */
    if (snapshot_xml_enter_element(r, psg_names[0])) {
        snap_psg_load_instance(r, &g_psg_module.psg[0]);
        snapshot_xml_leave_element(r);
    }

    /* Načtení PSG1 (pravý) — pouze pokud snapshot obsahuje stereo data */
    if (stereo && snapshot_xml_enter_element(r, psg_names[1])) {
        snap_psg_load_instance(r, &g_psg_module.psg[1]);
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_leave_element(r); /* psg_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_psg_register(void)
{
    /* Platformy bez PSG (MZ-700) registraci preskoci - snapshot pak
     * neobsahuje hw/psg.xml (zmrazeny on-disk kontrakt, shodne
     * s drivejsim compile-time #if). */
    if (g_mzhal.psg_count == 0)
        return;

    snapshot_register_component("psg",
                                SNAPSHOT_PRIORITY_HW_IO,
                                snap_psg_save,
                                snap_psg_load,
                                false);
}


