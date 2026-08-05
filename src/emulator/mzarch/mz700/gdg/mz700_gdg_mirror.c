/**
 * @file mz700_gdg_mirror.c
 * @brief Implementace side-effect free read API pro GDG MZ-700
 *        (gdg-panel F2).
 *
 * Viz mz700_gdg_mirror.h pro kontrakt.
 *
 * License: GPLv3.
 */

#include "mzarch/mzarch_config.h"

#if MZARCH == 700

#include <stddef.h>
#include <string.h>

#include "mz700_gdg.h"
#include "mz700_gdg_mirror.h"
#include "mzarch/gdg_raster_snapshot.h"

void gdg_mirror_snapshot(st_GDG_MIRROR_MZ700 *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    /* Reálný MZ-700 DMD registr nemá (porty 0xF0/0xF1 nedekóduje) -
     * regDMD je trvale 0, mirror ukazuje klidové hodnoty. */
    out->regDMD        = 0u;
    out->dmd_mode700   = 0u;
    out->dmd_pmode_bfp = 0u;

    /* Border - MZ-700 nema border port, regBOR je vzdy 0, ale kopie pro
     * symetrii s MZ-800 mirror. */
    out->regBOR = (uint8_t)(g_gdg.regBOR & 0x0Fu);

    /* Paleta MZ-700 - 8 indexu, port 0xF1. */
    for (unsigned i = 0; i < 8; ++i) {
        out->mode700_color[i] = g_gdg.mode_color[i];
    }

    /* Raster pozice. */
    out->beam_row     = g_gdg.beam_row;
    out->screen_ticks = g_gdg.total_elapsed.ticks;
    out->screens_done = g_gdg.total_elapsed.screens;

    out->hbln      = (uint8_t)(g_gdg.hbln      & 0x01u);
    out->vbln      = (uint8_t)(g_gdg.vbln      & 0x01u);
    out->sts_hsync = (uint8_t)(g_gdg.sts_hsync & 0x01u);
    out->sts_vsync = (uint8_t)(g_gdg.sts_vsync & 0x01u);

    /* Ostatni. */
    out->tempo         = (uint8_t)(g_gdg.tempo & 0x01u);
    out->tempo_divider = g_gdg.tempo_divider;
    out->regct53g7     = (uint8_t)(g_gdg.regct53g7 & 0x01u);
}

/**
 * @brief MZ-700 implementace arch-independent raster snapshot API.
 *
 * Viz `mzarch/gdg_raster_snapshot.h`. `ctc0_divider` čerpá z compile-time
 * `GDGCLK_CTC0_DIVIDER` (= 13 pro NTSC build, 16 pro PAL build - výběr
 * v `mz700_gdgclk.h` přes makro VIDEO_MZ700_*).
 */
void gdg_mirror_raster_snapshot(st_GDG_RASTER_SNAPSHOT *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->beam_row     = (uint32_t)g_gdg.beam_row;
    out->screen_ticks = (uint32_t)g_gdg.total_elapsed.ticks;
    out->screens_done = (uint64_t)g_gdg.total_elapsed.screens;

    out->hbln      = (uint8_t)(g_gdg.hbln      & 0x01u);
    out->vbln      = (uint8_t)(g_gdg.vbln      & 0x01u);
    out->sts_hsync = (uint8_t)(g_gdg.sts_hsync & 0x01u);
    out->sts_vsync = (uint8_t)(g_gdg.sts_vsync & 0x01u);

    out->tempo         = (uint8_t)(g_gdg.tempo & 0x01u);
    out->tempo_divider = (uint32_t)g_gdg.tempo_divider;
    out->ctc0_divider  = (uint32_t)GDGCLK_CTC0_DIVIDER;
}

#endif /* MZARCH == 700 */
