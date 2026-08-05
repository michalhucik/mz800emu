/**
 * @file mz1500_gdg_mirror.c
 * @brief Implementace side-effect free read API pro GDG MZ-1500
 *        (gdg-panel F2).
 *
 * Viz mz1500_gdg_mirror.h pro kontrakt.
 *
 * License: GPLv3.
 */

#include "mzarch/mzarch_config.h"

#if MZARCH == 1500

#include <stddef.h>
#include <string.h>

#include "mz1500_gdg.h"
#include "mz1500_gdg_mirror.h"
#include "mzarch/gdg_raster_snapshot.h"

#include "hw-generic/memory/memory.h"
#include "mzarch/mz1500/memory/mz1500_memory.h"

/**
 * @brief Bit v PCGHI VRAM byte indikující "PCG overlay povolen na této pozici".
 *
 * Per `mz1500_framebuffer.c`: `MZ1500_ATTRPCG_ENABLED = 0x08`. Definuji
 * lokálně - hlavička framebufferu není veřejná.
 */
#define MIRROR_MZ1500_ATTRPCG_ENABLED 0x08

/**
 * @brief Offset PCGHI regionu uvnitř MZ-1500 VRAM (0x1000 B).
 *
 * Per `mz1500_framebuffer.c`: `MZ1500_VRAM_PCGHI = 0xC00`. Region zabírá
 * 0xC00-0xFFF (1024 bajtů). Definuji lokálně, hlavička framebufferu
 * není veřejná.
 */
#define MIRROR_MZ1500_VRAM_PCGHI_OFFSET 0xC00

/**
 * @brief Velikost PCGHI regionu (= attribute pro PCG enable) v bajtech.
 */
#define MIRROR_MZ1500_VRAM_PCGHI_SIZE 0x400

void gdg_mirror_snapshot(st_GDG_MIRROR_MZ1500 *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    /* DMD registr + dekód bitů. */
    out->regDMD        = (uint8_t)(g_gdg.regDMD & 0x03u);
    out->dmd_mode1500  = (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ1500_MODE) ? 1u : 0u;
    out->dmd_pmode_bfp = (g_gdg.regDMD & REGISTER_DMD_FLAG_MZ1500_PRIO) ? 1u : 0u;

    /* Border - MZ-1500 nemá border port, regBOR je vždy 0. */
    out->regBOR = (uint8_t)(g_gdg.regBOR & 0x0Fu);

    /* Paleta MZ-1500 - 8 indexu, port 0xF1. */
    for (unsigned i = 0; i < 8; ++i) {
        out->mode1500_color[i] = g_gdg.mode_color[i];
    }

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
    out->regct53g7     = (uint8_t)(g_gdg.regct53g7 & 0x01u);

    /* ---- PCG state ------------------------------------------------------ */
    /* SPEC pole z g_memory.map - která PCG banka mapuje D000-EFFF. */
    out->pcg_spec_id     = (uint8_t)MEMORY_MZ1500_SPEC_ID;
    out->pcg_spec_active = (MEMORY_MZ1500_MAP_TEST_D000_SPEC) ? 1u : 0u;
    out->pcg_bank_size   = (unsigned)MEMORY_SIZE_PCG_BANK;
    out->pcg_bank_count  = 3u;

    /* Spočítej PCG-enabled buňky v PCGHI regionu (side-effect free read). */
    unsigned active = 0;
    const uint8_t *pcghi = &g_memory.VRAM[MIRROR_MZ1500_VRAM_PCGHI_OFFSET];
    for (unsigned i = 0; i < MIRROR_MZ1500_VRAM_PCGHI_SIZE; ++i) {
        if (pcghi[i] & MIRROR_MZ1500_ATTRPCG_ENABLED) {
            ++active;
        }
    }
    out->pcg_active_cells = active;
}

/**
 * @brief MZ-1500 implementace arch-independent raster snapshot API.
 *
 * Viz `mzarch/gdg_raster_snapshot.h`. `ctc0_divider` = compile-time
 * `GDGCLK_CTC0_DIVIDER` (= 16 pro MZ-1500).
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

#endif /* MZARCH == 1500 */
