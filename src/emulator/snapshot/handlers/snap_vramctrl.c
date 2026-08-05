/**
 * @file snap_vramctrl.c
 * @brief Snapshot handler: VRAM Control — platformově specifický řadič VRAM
 */
#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"

#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_vramctrl.h"
#endif

static en_SNAPSHOT_RESULT snap_vramctrl_save(st_SNAPSHOT_CONTEXT *ctx)
{
#if MZARCH == 800
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "vramctrl_state");

    /* MZ-800: WF/RF registry a banky */
    snapshot_xml_write_uint(w, "regWF_PLANE", g_vramctrl.regWF_PLANE);
    snapshot_xml_write_uint(w, "regWF_MODE", (unsigned)g_vramctrl.regWF_MODE);
    snapshot_xml_write_uint(w, "regRF_PLANE", g_vramctrl.regRF_PLANE);
    snapshot_xml_write_uint(w, "regRF_SEARCH", g_vramctrl.regRF_SEARCH);
    snapshot_xml_write_uint(w, "regWFRF_VBANK", g_vramctrl.regWFRF_VBANK);
    snapshot_xml_write_uint(w, "mz700_wr_latch_is_used", g_vramctrl.mz700_wr_latch_is_used);

    snapshot_xml_close_element(w); /* vramctrl_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/vramctrl.xml", xml);
    g_free(xml);

    return res;

#elif MZARCH == 1500
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "vramctrl_state");

    /* MZ-1500: pouze latch */
    snapshot_xml_write_uint(w, "mz700_wr_latch_is_used", g_vramctrl.mz700_wr_latch_is_used);

    snapshot_xml_close_element(w); /* vramctrl_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/vramctrl.xml", xml);
    g_free(xml);

    return res;

#else
    /* MZ-700: prázdný handler */
    (void)ctx;
    return SNAPSHOT_OK;
#endif
}

static en_SNAPSHOT_RESULT snap_vramctrl_load(st_SNAPSHOT_CONTEXT *ctx)
{
#if MZARCH == 800 || MZARCH == 1500
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/vramctrl.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("vramctrl", "Cannot load hw/vramctrl.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("vramctrl", "Parse error in hw/vramctrl.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "vramctrl_state")) {
        SNAP_ERR("vramctrl", "Missing element vramctrl_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

#if MZARCH == 800
    /* MZ-800: WF/RF registry a banky */
    snapshot_xml_read_uint(r, "regWF_PLANE", &g_vramctrl.regWF_PLANE);

    {
        unsigned wf_mode;
        if (snapshot_xml_read_uint(r, "regWF_MODE", &wf_mode))
            g_vramctrl.regWF_MODE = (WFR_MODE)wf_mode;
    }

    snapshot_xml_read_uint(r, "regRF_PLANE", &g_vramctrl.regRF_PLANE);
    snapshot_xml_read_uint(r, "regRF_SEARCH", &g_vramctrl.regRF_SEARCH);
    snapshot_xml_read_uint(r, "regWFRF_VBANK", &g_vramctrl.regWFRF_VBANK);
#endif

    /* Společný pro MZ-800 i MZ-1500 */
    snapshot_xml_read_uint(r, "mz700_wr_latch_is_used", &g_vramctrl.mz700_wr_latch_is_used);

    snapshot_xml_leave_element(r); /* vramctrl_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;

#else
    /* MZ-700: prázdný handler */
    (void)ctx;
    return SNAPSHOT_OK;
#endif
}

void snap_vramctrl_register(void)
{
    snapshot_register_component("vramctrl",
                                SNAPSHOT_PRIORITY_HW_CORE,
                                snap_vramctrl_save,
                                snap_vramctrl_load,
                                false);
}
