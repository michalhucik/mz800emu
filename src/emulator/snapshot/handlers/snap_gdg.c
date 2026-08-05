/**
 * @file snap_gdg.c
 * @brief Snapshot handler: GDG — zobrazovací řadič
 *
 * Load provádí rozsahovou validaci polí, která jádro používá jako indexy
 * nebo v ukazatelové aritmetice (mzhal dávka 9b) - poškozený nebo ručně
 * upravený snapshot dřív mohl zavést out-of-bounds přístup (např.
 * last_updated_border_pixel > šířka display podteče unsigned rozdíl
 * v memset, mz700_framebuffer.c). Meze se berou runtime z g_mzhal.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/gdg/gdg_state.h"
#include "mzarch/mzhal.h"

static en_SNAPSHOT_RESULT snap_gdg_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "gdg_state");

    /* Časové razítko */
    snapshot_xml_open_element(w, "total_elapsed");
    snapshot_xml_write_uint(w, "screens", g_gdg.total_elapsed.screens);
    snapshot_xml_write_uint(w, "ticks", g_gdg.total_elapsed.ticks);
    snapshot_xml_close_element(w);

    /* Pozice paprsku */
    snapshot_xml_write_uint(w, "beam_row", g_gdg.beam_row);

    /* Registry */
    snapshot_xml_open_element(w, "registers");
    snapshot_xml_write_uint(w, "regDMD", g_gdg.regDMD);
    snapshot_xml_write_uint(w, "regBOR", g_gdg.regBOR);
    /* Runtime dle g_mzhal.arch (mzhal 11e): množina XML klíčů per arch
     * je zmrazený on-disk kontrakt - MZ-800 klíče se na 700/1500 nesmí
     * objevit (superset pole tam existují, ale nikdo je neplní). */
    if (g_mzhal.arch == 800) {
        snapshot_xml_write_uint(w, "regPALGRP", g_gdg.regPALGRP);
        snapshot_xml_write_uint(w, "regPAL0", g_gdg.regPAL0);
        snapshot_xml_write_uint(w, "regPAL1", g_gdg.regPAL1);
        snapshot_xml_write_uint(w, "regPAL2", g_gdg.regPAL2);
        snapshot_xml_write_uint(w, "regPAL3", g_gdg.regPAL3);
        snapshot_xml_write_uint(w, "cksw", g_gdg.cksw);
    }
    snapshot_xml_write_uint(w, "regct53g7", g_gdg.regct53g7);
    if (g_mzhal.arch == 800) {
        /* HDL-presny WAIT model 800 grafickych rezimu - stav horke faze.
         * Starsi snapshoty bez techto klicu se pri loadu defaultne
         * zinicializuji na 0 (= zadna horka faze aktivni). */
        snapshot_xml_write_uint64(w, "vram800_hot_phase_end_total_ticks",
                                  g_gdg.vram800_hot_phase_end_total_ticks);
        snapshot_xml_write_uint(w, "vram800_hot_phase_clk0_phase",
                                g_gdg.vram800_hot_phase_clk0_phase);
    }
    snapshot_xml_close_element(w); /* registers */

    /* Tempo */
    snapshot_xml_write_uint(w, "tempo", g_gdg.tempo);
    snapshot_xml_write_uint(w, "tempo_divider", g_gdg.tempo_divider);

    /* Synchronizační signály */
    snapshot_xml_open_element(w, "sync_signals");
    snapshot_xml_write_uint(w, "sts_vsync", g_gdg.sts_vsync);
    snapshot_xml_write_uint(w, "sts_hsync", g_gdg.sts_hsync);
    snapshot_xml_write_uint(w, "hbln", g_gdg.hbln);
    snapshot_xml_write_uint(w, "vbln", g_gdg.vbln);
    snapshot_xml_close_element(w); /* sync_signals */

    /* Stav renderování */
    snapshot_xml_write_uint(w, "screen_is_already_rendered_at_beam_pos",
                            g_gdg.screen_is_already_rendered_at_beam_pos);
    snapshot_xml_write_uint(w, "screen_need_update_from",
                            g_gdg.screen_need_update_from);
    snapshot_xml_write_uint(w, "last_updated_border_pixel",
                            g_gdg.last_updated_border_pixel);

    /* Paleta 700/1500: XML klíče zůstávají per-arch (zmrazený kontrakt),
     * jen větvení je runtime (mzhal 11e). */
    if ((g_mzhal.arch == 1500) || (g_mzhal.arch == 700)) {
        const char *el = (g_mzhal.arch == 1500) ? "mode1500_colors"
                                                : "mode700_colors";
        const char *key_fmt = (g_mzhal.arch == 1500) ? "mode1500_color_%d"
                                                     : "mode700_color_%d";
        char key[24];
        snapshot_xml_open_element(w, el);
        for (int i = 0; i < 8; i++) {
            snprintf(key, sizeof(key), key_fmt, i);
            snapshot_xml_write_int(w, key, g_gdg.mode_color[i]);
        }
        snapshot_xml_close_element(w); /* mode700/1500_colors */
    }

    snapshot_xml_close_element(w); /* gdg_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/gdg.xml", xml);
    g_free(xml);

    return res;
}

static en_SNAPSHOT_RESULT snap_gdg_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/gdg.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("gdg", "Cannot load hw/gdg.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("gdg", "Parse error in hw/gdg.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "gdg_state")) {
        SNAP_ERR("gdg", "Missing element gdg_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Časové razítko */
    if (snapshot_xml_enter_element(r, "total_elapsed")) {
        snapshot_xml_read_uint(r, "screens", &g_gdg.total_elapsed.screens);
        snapshot_xml_read_uint(r, "ticks", &g_gdg.total_elapsed.ticks);
        snapshot_xml_leave_element(r);
    }

    /* Pozice paprsku */
    snapshot_xml_read_uint(r, "beam_row", &g_gdg.beam_row);

    /* Registry */
    if (snapshot_xml_enter_element(r, "registers")) {
        snapshot_xml_read_uint(r, "regDMD", &g_gdg.regDMD);
        snapshot_xml_read_uint(r, "regBOR", &g_gdg.regBOR);
        /* Runtime dle g_mzhal.arch (mzhal 11e) - viz save. */
        if (g_mzhal.arch == 800) {
            snapshot_xml_read_uint(r, "regPALGRP", &g_gdg.regPALGRP);
            snapshot_xml_read_uint(r, "regPAL0", &g_gdg.regPAL0);
            snapshot_xml_read_uint(r, "regPAL1", &g_gdg.regPAL1);
            snapshot_xml_read_uint(r, "regPAL2", &g_gdg.regPAL2);
            snapshot_xml_read_uint(r, "regPAL3", &g_gdg.regPAL3);
            /* cksw - novy klic, starsi snapshoty ho nemaji -> default 0
             * (snapshot_xml_read_uint nesnaha pri chybejicim klici). */
            snapshot_xml_read_uint(r, "cksw", &g_gdg.cksw);
        }
        snapshot_xml_read_uint(r, "regct53g7", &g_gdg.regct53g7);
        if (g_mzhal.arch == 800) {
            /* HDL-presny WAIT model - novy klic, default 0 pro starsi
             * snapshoty (= zadna horka faze aktivni, prvni VRAM pristup po
             * loadu se chova jako bez predchoziho WRITE). */
            g_gdg.vram800_hot_phase_end_total_ticks = 0;
            g_gdg.vram800_hot_phase_clk0_phase = 0;
            snapshot_xml_read_uint64(r, "vram800_hot_phase_end_total_ticks",
                                     &g_gdg.vram800_hot_phase_end_total_ticks);
            snapshot_xml_read_uint(r, "vram800_hot_phase_clk0_phase",
                                   &g_gdg.vram800_hot_phase_clk0_phase);
        }
        snapshot_xml_leave_element(r);
    }

    /* Tempo */
    snapshot_xml_read_uint(r, "tempo", &g_gdg.tempo);
    snapshot_xml_read_uint(r, "tempo_divider", &g_gdg.tempo_divider);

    /* Synchronizační signály */
    if (snapshot_xml_enter_element(r, "sync_signals")) {
        snapshot_xml_read_uint(r, "sts_vsync", &g_gdg.sts_vsync);
        snapshot_xml_read_uint(r, "sts_hsync", &g_gdg.sts_hsync);
        snapshot_xml_read_uint(r, "hbln", &g_gdg.hbln);
        snapshot_xml_read_uint(r, "vbln", &g_gdg.vbln);
        snapshot_xml_leave_element(r);
    }

    /* Stav renderování */
    snapshot_xml_read_uint(r, "screen_is_already_rendered_at_beam_pos",
                            &g_gdg.screen_is_already_rendered_at_beam_pos);
    snapshot_xml_read_uint(r, "screen_need_update_from",
                            &g_gdg.screen_need_update_from);
    snapshot_xml_read_uint(r, "last_updated_border_pixel",
                            &g_gdg.last_updated_border_pixel);

    /* Paleta 700/1500 runtime dle g_mzhal.arch (mzhal 11e); XML klice
     * per arch zmrazene - viz save. */
    if ((g_mzhal.arch == 1500) || (g_mzhal.arch == 700)) {
        const char *el = (g_mzhal.arch == 1500) ? "mode1500_colors"
                                                : "mode700_colors";
        const char *key_fmt = (g_mzhal.arch == 1500) ? "mode1500_color_%d"
                                                     : "mode700_color_%d";
        char key[24];
        if (snapshot_xml_enter_element(r, el)) {
            for (int i = 0; i < 8; i++) {
                snprintf(key, sizeof(key), key_fmt, i);
                snapshot_xml_read_int(r, key, &g_gdg.mode_color[i]);
            }
            snapshot_xml_leave_element(r);
        }
    }

    snapshot_xml_leave_element(r); /* gdg_state */
    snapshot_xml_reader_free(r);

    /* Rozsahová validace načtených hodnot (OOB kanály jádra):
     *  - beam_row: jádro drží 0..VIDEO_SCREEN_HEIGHT-1
     *    (mz*_gdg_event.c inkrement s wrapem)
     *  - total_elapsed.ticks: pixel clock v aktuálním snímku,
     *    0..VIDEO_SCREEN_TICKS-1 (kontrakt gdg_raster_snapshot.h)
     *  - screen_is_already_rendered_at_beam_pos: tick pozice, přebírá
     *    hodnoty total_elapsed.ticks (mzarch.c)
     *  - screen_need_update_from: pixel v canvas řádku, vstupuje do
     *    ukazatelové aritmetiky framebufferu (mz800_framebuffer.c
     *    framebuffer_MZ800_screen_row_fill)
     *  - last_updated_border_pixel: 0..VIDEO_DISPLAY_WIDTH; větší
     *    hodnota podteče unsigned délku memset (mz700_framebuffer.c)
     *  - paleta mode700/1500_color: index do c_MZ700_COLORMAP[8],
     *    HW zápis maskuje na 0-7 (mz700_gdg.c port 0xF1) */
    if (g_gdg.beam_row >= g_mzhal.video_screen_height) {
        SNAP_ERR("gdg", "beam_row out of range: %u (max %u)",
                 g_gdg.beam_row, g_mzhal.video_screen_height - 1);
        return SNAPSHOT_ERR_CORRUPTED;
    }
    if (g_gdg.total_elapsed.ticks >= g_mzhal.video_screen_ticks) {
        SNAP_ERR("gdg", "total_elapsed.ticks out of range: %u (max %u)",
                 g_gdg.total_elapsed.ticks, g_mzhal.video_screen_ticks - 1);
        return SNAPSHOT_ERR_CORRUPTED;
    }
    if (g_gdg.screen_is_already_rendered_at_beam_pos > g_mzhal.video_screen_ticks) {
        SNAP_ERR("gdg", "screen_is_already_rendered_at_beam_pos out of range: %u (max %u)",
                 g_gdg.screen_is_already_rendered_at_beam_pos,
                 g_mzhal.video_screen_ticks);
        return SNAPSHOT_ERR_CORRUPTED;
    }
    if (g_gdg.screen_need_update_from > g_mzhal.video_canvas_width) {
        SNAP_ERR("gdg", "screen_need_update_from out of range: %u (max %u)",
                 g_gdg.screen_need_update_from, g_mzhal.video_canvas_width);
        return SNAPSHOT_ERR_CORRUPTED;
    }
    if (g_gdg.last_updated_border_pixel > g_mzhal.video_display_width) {
        SNAP_ERR("gdg", "last_updated_border_pixel out of range: %u (max %u)",
                 g_gdg.last_updated_border_pixel, g_mzhal.video_display_width);
        return SNAPSHOT_ERR_CORRUPTED;
    }
    if ((g_mzhal.arch == 1500) || (g_mzhal.arch == 700)) {
        for (int i = 0; i < 8; i++) {
            if ((g_gdg.mode_color[i] < 0) || (g_gdg.mode_color[i] > 7)) {
                SNAP_ERR("gdg", "mode%d_color[%d] out of range: %d",
                         g_mzhal.arch, i, g_gdg.mode_color[i]);
                return SNAPSHOT_ERR_CORRUPTED;
            }
        }
    }

    /* Redzone kontrola (mzhal 9c-3): load zapisuje jen pojmenovaná pole,
     * redzone přepsat nemůže - nález tedy znamená dřívější korupci paměti
     * emulátoru, ne vadná snapshot data. Proto jen hlasitý log, load
     * nepadá. */
    if (!gdg_redzone_check(&g_gdg)) {
        SNAP_ERR("gdg", "st_GDG redzone corrupted (pre-existing memory corruption, not snapshot data)");
    }

    return SNAPSHOT_OK;
}

void snap_gdg_register(void)
{
    snapshot_register_component("gdg",
                                SNAPSHOT_PRIORITY_HW_CORE,
                                snap_gdg_save,
                                snap_gdg_load,
                                false);
}
