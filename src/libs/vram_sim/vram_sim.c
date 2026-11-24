/**
 * @file vram_sim.c
 * @brief Public API implementace + capture adapter pro MZ-800 VRAM simulator.
 *
 * Tento soubor obsahuje:
 *  - Metadata helpers (decode_mode, mode_name, bits_per_pixel, atd.)
 *  - HW scroll API (shift, recompute_enabled)
 *  - Public CPU R/W wrappers - delegují na vs_read_* / vs_write_byte z modes.c
 *  - capture_from_emu adapter (kompiluje se jen pro MZARCH=800)
 *
 * Per-mode dispatch je v `vram_sim_modes.c`.
 *
 * Reference port:
 *  - hwscroll logika: `src/emulator/mzarch/mz800/gdg/mz800_hwscroll.c`
 *  - DMD flag konstanty: `src/emulator/mzarch/mz800/gdg/mz800_gdg.h`
 */

#include "vram_sim.h"
#include "vram_sim_modes.h"

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Metadata helpers
 * ------------------------------------------------------------------------- */

en_VRAM_SIM_MODE vram_sim_decode_mode(uint8_t dmd)
{
    /* Bit 3 (MZ700) má přednost - ostatní bity GDG ignoruje v MZ-700 modu. */
    if (dmd & 0x08) {
        return VRAM_SIM_MODE_MZ700;
    }
    switch (dmd & 0x07) {
        case 0x00: return VRAM_SIM_MODE_320_4_A;
        case 0x01: return VRAM_SIM_MODE_320_4_B;
        case 0x02: return VRAM_SIM_MODE_320_16;
        case 0x03: return VRAM_SIM_MODE_320_UNDOC;
        case 0x04: return VRAM_SIM_MODE_640_2_A;
        case 0x05: return VRAM_SIM_MODE_640_2_B;
        case 0x06: return VRAM_SIM_MODE_640_4;
        case 0x07: return VRAM_SIM_MODE_640_UNDOC;
        default:   return VRAM_SIM_MODE_MZ700; /* unreachable - 3 bity */
    }
}

const char *vram_sim_mode_name(en_VRAM_SIM_MODE mode)
{
    /* Anglické klíče pro _() lokalizaci ve volající UI vrstvě. */
    switch (mode) {
        case VRAM_SIM_MODE_320_4_A:   return "320x200@4 (Bank A)";
        case VRAM_SIM_MODE_320_4_B:   return "320x200@4 (Bank B)";
        case VRAM_SIM_MODE_320_16:    return "320x200@16";
        case VRAM_SIM_MODE_320_UNDOC: return "320x200 undoc (DMD 0x03)";
        case VRAM_SIM_MODE_640_2_A:   return "640x200@2 (Bank A)";
        case VRAM_SIM_MODE_640_2_B:   return "640x200@2 (Bank B)";
        case VRAM_SIM_MODE_640_4:     return "640x200@4";
        case VRAM_SIM_MODE_640_UNDOC: return "640x200 undoc (DMD 0x07)";
        case VRAM_SIM_MODE_MZ700:     return "MZ-700 mode";
        default:                      return "Unknown";
    }
}

int vram_sim_bits_per_pixel(en_VRAM_SIM_MODE mode)
{
    switch (mode) {
        case VRAM_SIM_MODE_320_4_A:
        case VRAM_SIM_MODE_320_4_B:
        case VRAM_SIM_MODE_640_4:
            return 2;
        case VRAM_SIM_MODE_320_16:
            return 4;
        case VRAM_SIM_MODE_640_2_A:
        case VRAM_SIM_MODE_640_2_B:
            return 1;
        default:
            /* undoc + MZ-700: neimplementováno v V-1d */
            return 0;
    }
}

void vram_sim_canvas_dimensions(en_VRAM_SIM_MODE mode, int *out_width, int *out_height)
{
    int w = 0, h = 0;
    switch (mode) {
        case VRAM_SIM_MODE_320_4_A:
        case VRAM_SIM_MODE_320_4_B:
        case VRAM_SIM_MODE_320_16:
        case VRAM_SIM_MODE_320_UNDOC:
            w = 320; h = 200; break;
        case VRAM_SIM_MODE_640_2_A:
        case VRAM_SIM_MODE_640_2_B:
        case VRAM_SIM_MODE_640_4:
        case VRAM_SIM_MODE_640_UNDOC:
            w = 640; h = 200; break;
        default:
            /* MZ-700: nepodporováno */
            w = 0; h = 0; break;
    }
    if (out_width)  *out_width  = w;
    if (out_height) *out_height = h;
}

uint8_t vram_sim_display_planes(const st_VRAM_SIM_STATE *state)
{
    en_VRAM_SIM_MODE mode = vram_sim_decode_mode(state->dmd);
    switch (mode) {
        case VRAM_SIM_MODE_320_4_A: return VS_PLANE1 | VS_PLANE2;
        case VRAM_SIM_MODE_320_4_B: return VS_PLANE3 | VS_PLANE4;
        case VRAM_SIM_MODE_320_16:
        case VRAM_SIM_MODE_320_UNDOC:
        case VRAM_SIM_MODE_640_UNDOC:
        case VRAM_SIM_MODE_640_4:   return VS_PLANE_ALL;
        case VRAM_SIM_MODE_640_2_A: return VS_PLANE1 | VS_PLANE2;
        case VRAM_SIM_MODE_640_2_B: return VS_PLANE3 | VS_PLANE4;
        default: return 0;
    }
}

uint8_t vram_sim_cpu_planes(const st_VRAM_SIM_STATE *state)
{
    /*
     * Port z mz800_vramctrl.c:702-708 (avlb_plane logika).
     *
     * V HICOLOR rezimech jsou vsechny plane "available".
     * VBANK doplni dvojici plane (I+II pro A, III+IV pro B).
     * V 640 modu se zachova jen "licha strana" (I+III) pro sudy bajt.
     *
     * Pozn: vram_sim_cpu_planes vraci "avlb_plane" - tedy availability
     * pro REPLACE/PSET. SINGLE/EXOR/OR/RESET pouziva 'avlb_plane_s' =
     * (SCRW640 ? I+III : ALL). Pokud konzument potrebuje obe, musi si je
     * spocitat sam.
     */
    uint8_t avlb_plane = VS_DMD_TEST_HICOLOR(state) ? VS_PLANE_ALL : 0;
    avlb_plane |= state->wfrf_vbank ? (VS_PLANE3 | VS_PLANE4) : (VS_PLANE1 | VS_PLANE2);
    avlb_plane &= VS_DMD_TEST_SCRW640(state) ? (VS_PLANE1 | VS_PLANE3) : VS_PLANE_ALL;
    return avlb_plane;
}

/* ---------------------------------------------------------------------------
 * HW scroll API
 * ------------------------------------------------------------------------- */

void vram_sim_recompute_hwscroll_enabled(st_VRAM_SIM_STATE *state)
{
    /* Port z mz800_hwscroll.c (hwscroll_regs_changed); rovnost SW == SEA - SSA
     * podmínkou není (OPRAVA báze 2026-11-22, viz vram_sim.h). */
    if ((state->hwscroll_sof > 0) &&
        (state->hwscroll_ssa <= 0x1E00) &&
        (state->hwscroll_sea >= 0x0140) &&
        (state->hwscroll_sw > state->hwscroll_sof) &&
        (state->hwscroll_sea > state->hwscroll_ssa))
    {
        state->hwscroll_enabled = 1;
    } else {
        state->hwscroll_enabled = 0;
    }
}

uint16_t vram_sim_hwscroll_shift(const st_VRAM_SIM_STATE *state, uint16_t phy_offset)
{
    /* Port z mz800_hwscroll.h:70-89 (hwscroll_shift_addr). */
    if (!state->hwscroll_enabled) {
        return phy_offset;
    }
    /* Mimo scroll area - beze změny. */
    if (phy_offset < state->hwscroll_ssa || phy_offset >= state->hwscroll_sea) {
        return phy_offset;
    }
    /* Uvnitř scroll area - rozhodnout up/down podle pozice. */
    if (phy_offset >= (uint16_t)(state->hwscroll_sea - state->hwscroll_sof)) {
        /* Shift down; přetečení v 13 bitech jako HWSCRL_ADDR_MASK emulátoru. */
        return (uint16_t)((phy_offset + state->hwscroll_sof - state->hwscroll_sw) & 0x1FFFu);
    }
    /* Shift up. */
    return (uint16_t)((phy_offset + state->hwscroll_sof) & 0x1FFFu);
}

int vram_sim_cpu_hwscroll_applies(const st_VRAM_SIM_STATE *state)
{
    /* Port z mz800_vramctrl.c (vramctrl_mz800_cpu_scroll_applies). */
    if (VS_DMD_TEST_HICOLOR(state)) {
        return 1;
    }
    return ((state->dmd & 0x01) ? 1 : 0) == (state->wfrf_vbank ? 1 : 0);
}

/* ---------------------------------------------------------------------------
 * CPU R/W public wrappers
 * ------------------------------------------------------------------------- */

uint8_t vram_sim_cpu_read(const st_VRAM_SIM_STATE *state, uint16_t addr)
{
    /* MZ-700 mode není v scope vram_sim - vracíme 0xFF (sentinel). */
    if (VS_DMD_TEST_MZ700(state)) {
        return 0xFF;
    }

    /*
     * Vstupní addr může být v Z80 prostoru (0x8000..0xBFFF) nebo už maskovaný
     * offset (0x0000..0x3FFF). Emu vramctrl_mz800_memop_read_byte je voláno
     * přes makro MEMORY_VRAM_MZ800_READ_BYTE s `addr & 0x3FFF`, tj. dostává
     * masked offset. Sim aplikuje stejnou masku pro konzistenci - caller
     * může předat plný Z80 addr a vyjde to správně.
     */
    addr = (uint16_t)(addr & 0x3FFF);

    /* Výpočet phy_vram_addr - port z vramctrl_mz800_memop_read_byte_internal(). */
    uint16_t phy = addr;
    if (VS_DMD_TEST_SCRW640(state)) {
        phy = (uint16_t)(phy >> 1);
    }
    if (vram_sim_cpu_hwscroll_applies(state)) {
        phy = vram_sim_hwscroll_shift(state, phy);
    }

    /* Maska na 13 bitů pro safe plane[] access. Emu kód maskování nedělá -
     * pro běžné adresy s rozumným HW scroll se nikdy nedostane mimo 0x1FFF,
     * ale extrémní edge case by mohl. Zde maskujeme aby sim byl
     * deterministicky safe; pro běžné scénáře je výsledek identický s emu. */
    phy = (uint16_t)(phy & 0x1FFF);

    if (state->rf_search) {
        return vs_read_search(state, addr, phy);
    }
    return vs_read_normal(state, addr, phy);
}

void vram_sim_cpu_write(st_VRAM_SIM_STATE *state, uint16_t addr, uint8_t value)
{
    if (VS_DMD_TEST_MZ700(state)) {
        return; /* MZ-700 mode mimo scope */
    }

    addr = (uint16_t)(addr & 0x3FFF);

    uint16_t phy = addr;
    if (VS_DMD_TEST_SCRW640(state)) {
        phy = (uint16_t)(phy >> 1);
    }
    if (vram_sim_cpu_hwscroll_applies(state)) {
        phy = vram_sim_hwscroll_shift(state, phy);
    }
    phy = (uint16_t)(phy & 0x1FFF);

    vs_write_byte(state, addr, phy, value);
}

/* ---------------------------------------------------------------------------
 * capture_from_emu adapter
 * ---------------------------------------------------------------------------
 *
 * Pozn: capture_from_emu() a attach_emu_planes() jsou implementovány v
 * per-arch souboru `src/emulator/mzarch/mz800/vram_sim_emu_capture.c`,
 * který se kompiluje pouze do mz800emu executable (díky per-arch sběru
 * v `mz_add_emulator()`). Lib vram_sim sama je arch-independent a
 * neobsahuje žádné emu závislosti.
 *
 * Pro 700/1500 build jsou tyto symboly absent. Memory Browser je
 * MZ-800-only feature, takže linker chyba nehrozí pokud se dodrží
 * konvence "capture API volat jen z MZ-800 specific kódu".
 */
