/**
 * @file vram_sim_emu_capture.c
 * @brief Adapter mezi vram_sim knihovnou a globálním emu state (MZ-800 only).
 *
 * Tento soubor obsahuje implementaci `vram_sim_capture_from_emu()` a
 * `vram_sim_attach_emu_planes()` deklarovaných v `libs/vram_sim/vram_sim.h`.
 * Je odděleně od lib core proto, že potřebuje linkovat proti emu globals
 * (`g_vramctrl`, `g_gdg`, `g_hwscroll`, `g_memoryVRAM_*`) - ty existují
 * jen v mz800emu executable.
 *
 * Kompiluje se per-arch sběrem z `mz_add_emulator()` v
 * `cmake/AddMzEmu.cmake` - pouze pro mz800emu target. Pro mz700/mz1500
 * adapter neexistuje a Memory Browser jako MZ-800-only feature tyto
 * symboly nevolá.
 */

#include "libs/vram_sim/vram_sim.h"

#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#include "memory/memory.h"
#include "mzarch/mz800/memory/mz800_memory.h"

void vram_sim_capture_from_emu(st_VRAM_SIM_STATE *state)
{
    state->dmd          = (uint8_t)(g_gdg.regDMD & 0x0F);
    state->rf_plane     = (uint8_t)(g_vramctrl.regRF_PLANE & 0x0F);
    state->rf_search    = (uint8_t)(g_vramctrl.regRF_SEARCH & 0x01);
    state->wf_plane     = (uint8_t)(g_vramctrl.regWF_PLANE & 0x0F);
    state->wf_mode      = (uint8_t)(g_vramctrl.regWF_MODE);
    state->wfrf_vbank   = (uint8_t)(g_vramctrl.regWFRF_VBANK & 0x01);

    state->hwscroll_sof     = (uint16_t)(g_hwscroll.regSOF);
    state->hwscroll_sw      = (uint16_t)(g_hwscroll.regSW);
    state->hwscroll_ssa     = (uint16_t)(g_hwscroll.regSSA);
    state->hwscroll_sea     = (uint16_t)(g_hwscroll.regSEA);
    state->hwscroll_enabled = (uint8_t)(g_hwscroll.enabled);

    /* Rozšíření VRAM MZ-1R25 jde odpojit volbou [MZ800] exvram
     * (mz800_memory_set_exvram()). */
    state->has_exvram = (uint8_t) ( g_mz800_exvram_installed ? 1 : 0 );

    vram_sim_attach_emu_planes(state);
}

void vram_sim_attach_emu_planes(st_VRAM_SIM_STATE *state)
{
    state->plane_data[0] = g_memoryVRAM_I;
    state->plane_data[1] = g_memoryVRAM_II;
    state->plane_data[2] = g_memoryVRAM_III;
    state->plane_data[3] = g_memoryVRAM_IV;
    state->plane_data_writable[0] = g_memoryVRAM_I;
    state->plane_data_writable[1] = g_memoryVRAM_II;
    /* Bez rozšíření ukazují čtecí ukazatele III a IV na buffer samých 0FFh;
     * zápis jde přes *_WR (odkládací buffer), takže čtení nepřepíše. */
    state->plane_data_writable[2] = g_memoryVRAM_III_WR;
    state->plane_data_writable[3] = g_memoryVRAM_IV_WR;
}
