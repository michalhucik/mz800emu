/**
 * @file mz800_gdg_mirror.c
 * @brief Implementace side-effect free read API pro GDG MZ-800
 *        (gdg-panel F2).
 *
 * Viz `mz800_gdg_mirror.h` pro popis kontraktu. Implementace je jen
 * mechanická kopie fieldů `g_gdg` do `st_GDG_MIRROR_MZ800`.
 *
 * License: GPLv3.
 */

#include "mzarch/mzarch_config.h"

#if MZARCH == 800

#include <stddef.h>
#include <string.h>

#include "mz800_gdg.h"
#include "mz800_gdg_mirror.h"
#include "mz800_vramctrl.h"
#include "mz800_hwscroll.h"
#include "mzarch/gdg_raster_snapshot.h"

void gdg_mirror_snapshot(st_GDG_MIRROR_MZ800 *out)
{
    if (out == NULL) {
        /* Defensive guard - kontrakt sice zakazuje NULL, ale segfault by
         * pri debugovani UI byl zbytecne nepekny. */
        return;
    }

    /* Vynulování celé struktury - sjednotí ABI pro budoucí přidané fieldy
     * (caller dostane definované hodnoty i pro nevyplněné fieldy v starých
     * verzích). */
    memset(out, 0, sizeof(*out));

    /* DMD registr + dekód jednotlivých bitů. Pojmenování bitů viz makra
     * REGISTER_DMD_FLAG_* v mz800_gdg.h. */
    out->regDMD      = (uint8_t)(g_gdg.regDMD & 0x0Fu);
    out->dmd_mz700   = (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ700)   ? 1u : 0u;
    out->dmd_scrw640 = (g_gdg.regDMD & REGISTER_DMD_FLAG_SCRW640) ? 1u : 0u;
    out->dmd_hicolor = (g_gdg.regDMD & REGISTER_DMD_FLAG_HICOLOR) ? 1u : 0u;
    out->dmd_vbank   = (g_gdg.regDMD & REGISTER_DMD_FLAG_VBANK)   ? 1u : 0u;

    /* Border + palette registry. */
    out->regBOR    = (uint8_t)(g_gdg.regBOR    & 0x0Fu);
    out->regPALGRP = (uint8_t)(g_gdg.regPALGRP & 0x03u);
    out->regPAL0   = (uint8_t)(g_gdg.regPAL0   & 0x0Fu);
    out->regPAL1   = (uint8_t)(g_gdg.regPAL1   & 0x0Fu);
    out->regPAL2   = (uint8_t)(g_gdg.regPAL2   & 0x0Fu);
    out->regPAL3   = (uint8_t)(g_gdg.regPAL3   & 0x0Fu);

    /* Raster pozice. */
    out->beam_row     = g_gdg.beam_row;
    out->screen_ticks = g_gdg.total_elapsed.ticks;
    out->screens_done = g_gdg.total_elapsed.screens;

    out->hbln      = (uint8_t)(g_gdg.hbln      & 0x01u);
    out->vbln      = (uint8_t)(g_gdg.vbln      & 0x01u);
    out->sts_hsync = (uint8_t)(g_gdg.sts_hsync & 0x01u);
    out->sts_vsync = (uint8_t)(g_gdg.sts_vsync & 0x01u);

    /* Ostatní. */
    out->tempo         = (uint8_t)(g_gdg.tempo & 0x01u);
    out->tempo_divider = g_gdg.tempo_divider;
    out->cksw          = (uint8_t)(g_gdg.cksw & 0x01u);
    out->regct53g7     = (uint8_t)(g_gdg.regct53g7 & 0x01u);

    /* VRAM controller registry. WF/RF plane masky drží jen low 4 bity
     * (plane I=bit0, II=bit1, III=bit2, IV=bit3). vc_wf_mode je raw enum
     * WFR_MODE - dekód na lidský string řeší panel přes label tabulku. */
    out->vc_wf_plane               = (uint8_t)(g_vramctrl.regWF_PLANE & 0x0Fu);
    out->vc_wf_mode                = (uint8_t)g_vramctrl.regWF_MODE;
    out->vc_rf_plane               = (uint8_t)(g_vramctrl.regRF_PLANE & 0x0Fu);
    out->vc_rf_search              = (uint8_t)(g_vramctrl.regRF_SEARCH & 0x0Fu);
    out->vc_wfrf_vbank             = (uint8_t)(g_vramctrl.regWFRF_VBANK & 0x01u);
    out->vc_vram_wrff_set          = (uint8_t)(g_gdg.vram_wrff_release > gdg_get_vramwait_ticks());

    /* HW scroll registry. SSA/SEA/SW/SOF jsou adresy/velikosti v bajtech
     * plane (= max 0x4000 pro 16 KB plane). enabled je cached flag,
     * který emulátor udržuje pro rychlý test ve render smyčce. */
    out->hs_ssa     = g_hwscroll.regSSA;
    out->hs_sea     = g_hwscroll.regSEA;
    out->hs_sw      = g_hwscroll.regSW;
    out->hs_sof     = g_hwscroll.regSOF;
    out->hs_enabled = (uint8_t)(g_hwscroll.enabled & 0x01u);
}

/**
 * @brief MZ-800 implementace arch-independent raster snapshot API.
 *
 * Viz `mzarch/gdg_raster_snapshot.h` pro kontrakt. Plní fieldy z `g_gdg`,
 * `ctc0_divider` z compile-time `GDGCLK_CTC0_DIVIDER` (= 16 pro MZ-800).
 */
void gdg_mirror_raster_snapshot(st_GDG_RASTER_SNAPSHOT *out)
{
    if (out == NULL) {
        return;
    }

    /* Vynulování pro forward-compat při budoucím přidání fieldů. */
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
    out->line_ticks    = (uint16_t)(VIDEO_SCREEN_WIDTH - g_gdg.row_shrink);
}

#endif /* MZARCH == 800 */
