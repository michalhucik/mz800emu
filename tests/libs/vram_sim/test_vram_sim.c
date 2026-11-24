/**
 * @file   test_vram_sim.c
 * @brief  Unit testy + bit-for-bit smoke identity test pro vram_sim knihovnu.
 *
 * Obsahuje:
 *  - Pure unit testy (decode_mode, hwscroll shift, metadata)
 *  - CPU read scenarios (single plane, multi-plane AND, SEARCH, no exVRAM)
 *  - CPU write scenarios (SINGLE/EXOR/OR/RESET/REPLACE/PSET, REPLACE clears)
 *  - Pixel render extraction (320@4, 320@16, 640@2, 640@4)
 *  - Locate pixel inverse
 *  - SMOKE IDENTITY: porovnání sim vs emu na matrix DMD x WF x HW scroll
 *  - HW scroll u CPU přístupu jen pro zobrazenou frame (B/A == DMD bit 0)
 *    pro celý 16 KB rozsah 0x8000-0xBFFF (byte-by-byte)
 *
 * Test typu LIB - linkuje proti emu core + headless stuby, aby identity
 * test mohl volat skutečné vramctrl_mz800_memop_read_byte().
 *
 * @par Licence: GPLv3
 */

#include "unity.h"
#include "mztest.h"

#include "libs/vram_sim/vram_sim.h"

#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#include "memory/memory.h"
#include "mzarch/mz800/memory/mz800_memory.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ========================================================================
 * Pure unit testy - decode_mode, metadata, hwscroll
 * ======================================================================== */

static void test_decode_mode_all_dmd_values(void)
{
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_320_4_A,   vram_sim_decode_mode(0x00));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_320_4_B,   vram_sim_decode_mode(0x01));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_320_16,    vram_sim_decode_mode(0x02));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_320_UNDOC, vram_sim_decode_mode(0x03));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_640_2_A,   vram_sim_decode_mode(0x04));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_640_2_B,   vram_sim_decode_mode(0x05));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_640_4,     vram_sim_decode_mode(0x06));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_640_UNDOC, vram_sim_decode_mode(0x07));

    /* Bit 3 = 1 dává MZ-700 mode bez ohledu na bity 0-2. */
    for (uint8_t i = 0x08; i <= 0x0F; i++) {
        TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_MZ700, vram_sim_decode_mode(i));
    }
    /* Vyšší bity (>0x0F) GDG ignoruje. */
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_320_4_A, vram_sim_decode_mode(0xF0));
    TEST_ASSERT_EQUAL_INT(VRAM_SIM_MODE_MZ700,   vram_sim_decode_mode(0xF8));
}

static void test_metadata_canvas_and_bpp(void)
{
    int w, h;

    vram_sim_canvas_dimensions(VRAM_SIM_MODE_320_4_A, &w, &h);
    TEST_ASSERT_EQUAL_INT(320, w);
    TEST_ASSERT_EQUAL_INT(200, h);

    vram_sim_canvas_dimensions(VRAM_SIM_MODE_640_4, &w, &h);
    TEST_ASSERT_EQUAL_INT(640, w);
    TEST_ASSERT_EQUAL_INT(200, h);

    vram_sim_canvas_dimensions(VRAM_SIM_MODE_MZ700, &w, &h);
    TEST_ASSERT_EQUAL_INT(0, w);
    TEST_ASSERT_EQUAL_INT(0, h);

    TEST_ASSERT_EQUAL_INT(2, vram_sim_bits_per_pixel(VRAM_SIM_MODE_320_4_A));
    TEST_ASSERT_EQUAL_INT(2, vram_sim_bits_per_pixel(VRAM_SIM_MODE_320_4_B));
    TEST_ASSERT_EQUAL_INT(4, vram_sim_bits_per_pixel(VRAM_SIM_MODE_320_16));
    TEST_ASSERT_EQUAL_INT(1, vram_sim_bits_per_pixel(VRAM_SIM_MODE_640_2_A));
    TEST_ASSERT_EQUAL_INT(1, vram_sim_bits_per_pixel(VRAM_SIM_MODE_640_2_B));
    TEST_ASSERT_EQUAL_INT(2, vram_sim_bits_per_pixel(VRAM_SIM_MODE_640_4));
    TEST_ASSERT_EQUAL_INT(0, vram_sim_bits_per_pixel(VRAM_SIM_MODE_MZ700));
}

static void test_hwscroll_disabled_default(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    /* Default: hwscroll_enabled = 0 → shift = identity. */
    TEST_ASSERT_EQUAL_UINT16(0x1234, vram_sim_hwscroll_shift(&state, 0x1234));
    TEST_ASSERT_EQUAL_UINT16(0x0000, vram_sim_hwscroll_shift(&state, 0x0000));
}

static void test_hwscroll_recompute_enable_conditions(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));

    /* Vsechno 0 → disabled. */
    vram_sim_recompute_hwscroll_enabled(&state);
    TEST_ASSERT_EQUAL_UINT8(0, state.hwscroll_enabled);

    /* Validni kombinace: SOF=0x140, SSA=0x140, SEA=0x1F40, SW=0x1E00. */
    state.hwscroll_sof = 0x0140;
    state.hwscroll_ssa = 0x0140;
    state.hwscroll_sea = 0x1F40;
    state.hwscroll_sw  = (uint16_t)(state.hwscroll_sea - state.hwscroll_ssa);
    vram_sim_recompute_hwscroll_enabled(&state);
    TEST_ASSERT_EQUAL_UINT8(1, state.hwscroll_enabled);

    /* Porus SW > SOF: SW = SOF → disable. */
    state.hwscroll_sw = state.hwscroll_sof;
    vram_sim_recompute_hwscroll_enabled(&state);
    TEST_ASSERT_EQUAL_UINT8(0, state.hwscroll_enabled);

    /* SW != SEA - SSA scroll nevypne (OPRAVA báze 2026-11-22, jako emulátor). */
    state.hwscroll_sw = (uint16_t)(state.hwscroll_sea - state.hwscroll_ssa - 0x0040);
    vram_sim_recompute_hwscroll_enabled(&state);
    TEST_ASSERT_EQUAL_UINT8(1, state.hwscroll_enabled);
}

static void test_hwscroll_shift_up_and_down(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.hwscroll_sof = 0x0140;
    state.hwscroll_ssa = 0x0140;
    state.hwscroll_sea = 0x1F40;
    state.hwscroll_sw  = (uint16_t)(state.hwscroll_sea - state.hwscroll_ssa);
    vram_sim_recompute_hwscroll_enabled(&state);
    TEST_ASSERT_EQUAL_UINT8(1, state.hwscroll_enabled);

    /* addr < SSA → identity. */
    TEST_ASSERT_EQUAL_UINT16(0x0100, vram_sim_hwscroll_shift(&state, 0x0100));

    /* addr >= SEA → identity. */
    TEST_ASSERT_EQUAL_UINT16(0x1F50, vram_sim_hwscroll_shift(&state, 0x1F50));

    /* addr v scroll area, < SEA-SOF (= 0x1E00) → shift up (addr + SOF). */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(0x0200 + 0x0140),
                             vram_sim_hwscroll_shift(&state, 0x0200));

    /* addr v scroll area, >= SEA-SOF (= 0x1E00) → shift down (addr + SOF - SW). */
    /* SW = 0x1E00, SOF = 0x140 → shift down delta = 0x140 - 0x1E00 = -0x1CC0 */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(0x1E00 + 0x0140 - 0x1E00),
                             vram_sim_hwscroll_shift(&state, 0x1E00));
}

/**
 * @brief Pravidlo HW scrollu pro CPU přístup v simulátoru.
 *
 * Scroll se na CPU přístup uplatní jen při B/A == DMD bit 0, v HICOLOR
 * režimech (DMD bit 1 = 1) vždy. Ověřuje vram_sim_cpu_hwscroll_applies()
 * pro DMD 00h-07h a obě hodnoty B/A.
 */
static void test_cpu_hwscroll_applies_rule(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    for (uint8_t dmd = 0x00; dmd <= 0x07; dmd++) {
        for (uint8_t vbank = 0; vbank <= 1; vbank++) {
            state.dmd = dmd;
            state.wfrf_vbank = vbank;
            int expected = (dmd & 0x02) ? 1 : ((dmd & 0x01) == vbank);
            char msg[64];
            snprintf(msg, sizeof(msg), "DMD=0x%02X B/A=%u", dmd, vbank);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, vram_sim_cpu_hwscroll_applies(&state), msg);
        }
    }
}

/* ========================================================================
 * CPU Read scenarios (pure unit, bez emu)
 * ======================================================================== */

static void test_cpu_read_no_plane_selected_returns_FF(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;       /* 320x200@4 A */
    state.rf_plane = 0x00;  /* no plane */
    state.rf_search = 0;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.has_exvram = 1;

    TEST_ASSERT_EQUAL_UINT8(0xFF, vram_sim_cpu_read(&state, 0x8100));
}

static void test_cpu_read_320_4_A_single_plane(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    plane_I[0x100]  = 0xAB;
    plane_II[0x100] = 0xCD;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;
    state.rf_plane = 0x01;  /* jen plane I */
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.has_exvram = 1;

    /* Adresa 0x8100 - addr - 0x8000 = 0x100. Phy = 0x100. */
    TEST_ASSERT_EQUAL_UINT8(0xAB, vram_sim_cpu_read(&state, 0x8100));

    /* Jen plane II. */
    state.rf_plane = 0x02;
    TEST_ASSERT_EQUAL_UINT8(0xCD, vram_sim_cpu_read(&state, 0x8100));

    /* Obě plane = AND. */
    state.rf_plane = 0x03;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xAB & 0xCD), vram_sim_cpu_read(&state, 0x8100));
}

static void test_cpu_read_no_exvram_returns_FF_for_plane_III(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x01;          /* 320x200@4 B → vyžaduje plane III/IV */
    state.rf_plane = 0x04;     /* plane III select */
    state.wfrf_vbank = 1;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.plane_data[2] = NULL;  /* žádná exVRAM */
    state.plane_data[3] = NULL;
    state.has_exvram = 0;

    /* Plane III bez exVRAM → 0xFF. */
    TEST_ASSERT_EQUAL_UINT8(0xFF, vram_sim_cpu_read(&state, 0x8000));
}

static void test_cpu_read_search_mode_match(void)
{
    /* 320@16, search mode. Pattern: plane I=1, plane II=0, III=0, IV=0
     * znamená hledáme pixely kde plane I = 1 AND ostatní = 0.
     * Naplníme plane I = 0xF0, ostatní 0x00. → search výsledek = 0xF0. */
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    uint8_t plane_III[VRAM_SIM_PLANE_SIZE]= {0};
    uint8_t plane_IV[VRAM_SIM_PLANE_SIZE] = {0};
    plane_I[0x50] = 0xF0;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x02;       /* 320x200@16 */
    state.rf_plane = 0x01;  /* match: I=1, II=0, III=0, IV=0 */
    state.rf_search = 1;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.plane_data[2] = plane_III;
    state.plane_data[3] = plane_IV;
    state.has_exvram = 1;

    TEST_ASSERT_EQUAL_UINT8(0xF0, vram_sim_cpu_read(&state, 0x8050));
}

/* ========================================================================
 * CPU Write scenarios
 * ======================================================================== */

static void test_cpu_write_single_mode_320_4_A(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;
    state.wf_plane = 0x01;
    state.wf_mode = VRAM_SIM_WF_SINGLE;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.plane_data_writable[0] = plane_I;
    state.plane_data_writable[1] = plane_II;
    state.has_exvram = 1;

    vram_sim_cpu_write(&state, 0x8100, 0x55);
    TEST_ASSERT_EQUAL_UINT8(0x55, plane_I[0x100]);
    TEST_ASSERT_EQUAL_UINT8(0x00, plane_II[0x100]);  /* SINGLE nezasáhl */
}

static void test_cpu_write_replace_clears_other_planes(void)
{
    /* REPLACE: vybrané plane dostanou value, ostatní dostupné = 0. */
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    plane_I[0x100]  = 0x55;
    plane_II[0x100] = 0xAA;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;          /* 320@4 A: plane I+II both available */
    state.wf_plane = 0x01;     /* jen I vybrán */
    state.wf_mode = VRAM_SIM_WF_REPLACE;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.plane_data_writable[0] = plane_I;
    state.plane_data_writable[1] = plane_II;
    state.has_exvram = 1;

    vram_sim_cpu_write(&state, 0x8100, 0xFF);
    TEST_ASSERT_EQUAL_UINT8(0xFF, plane_I[0x100]);   /* vybran - dostal value */
    TEST_ASSERT_EQUAL_UINT8(0x00, plane_II[0x100]);  /* nevybran - vynulovan */
}

static void test_cpu_write_exor_mode(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    plane_I[0x100] = 0xF0;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;
    state.wf_plane = 0x01;
    state.wf_mode = VRAM_SIM_WF_EXOR;
    state.plane_data[0] = plane_I;
    state.plane_data_writable[0] = plane_I;
    state.has_exvram = 1;

    vram_sim_cpu_write(&state, 0x8100, 0xAA);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xF0 ^ 0xAA), plane_I[0x100]);
}

static void test_cpu_write_readonly_plane_silent_skip(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    plane_I[0x100] = 0x42;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;
    state.wf_plane = 0x01;
    state.wf_mode = VRAM_SIM_WF_SINGLE;
    state.plane_data[0] = plane_I;
    state.plane_data_writable[0] = NULL;  /* read-only! */
    state.has_exvram = 1;

    vram_sim_cpu_write(&state, 0x8100, 0xFF);
    TEST_ASSERT_EQUAL_UINT8(0x42, plane_I[0x100]);  /* beze zmeny */
}

/* ========================================================================
 * Pixel render extraction
 * ======================================================================== */

static void test_pixel_render_320_4_A_extracts_bit0(void)
{
    /* 320x200@4 A: pal_idx = (II_bit << 1) | I_bit. Bit ordering LSB-first.
     * Naplníme plane I[0] = 0x01 (bit 0 = 1) a plane II[0] = 0x00. Pixel (0,0)
     * by měl mít pal_idx = (0 << 1) | 1 = 1. */
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    plane_I[0] = 0x01;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.has_exvram = 1;

    TEST_ASSERT_EQUAL_INT(1, vram_sim_get_pixel_palette_index(&state, 0, 0));
    /* Pixel (1, 0) by měl mít pal_idx = 0 (bit 1 v 0x01 = 0). */
    TEST_ASSERT_EQUAL_INT(0, vram_sim_get_pixel_palette_index(&state, 1, 0));
}

static void test_pixel_render_320_16_extracts_4_planes(void)
{
    uint8_t plane_I[VRAM_SIM_PLANE_SIZE]  = {0};
    uint8_t plane_II[VRAM_SIM_PLANE_SIZE] = {0};
    uint8_t plane_III[VRAM_SIM_PLANE_SIZE]= {0};
    uint8_t plane_IV[VRAM_SIM_PLANE_SIZE] = {0};
    /* Bit 0 ve všech 4 planes = 1 → pal_idx = 0xF. */
    plane_I[0] = 0x01;
    plane_II[0] = 0x01;
    plane_III[0] = 0x01;
    plane_IV[0] = 0x01;

    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x02;
    state.plane_data[0] = plane_I;
    state.plane_data[1] = plane_II;
    state.plane_data[2] = plane_III;
    state.plane_data[3] = plane_IV;
    state.has_exvram = 1;

    TEST_ASSERT_EQUAL_INT(0xF, vram_sim_get_pixel_palette_index(&state, 0, 0));
}

static void test_pixel_render_out_of_range_returns_minus1(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;

    TEST_ASSERT_EQUAL_INT(-1, vram_sim_get_pixel_palette_index(&state, -1, 0));
    TEST_ASSERT_EQUAL_INT(-1, vram_sim_get_pixel_palette_index(&state, 320, 0));
    TEST_ASSERT_EQUAL_INT(-1, vram_sim_get_pixel_palette_index(&state, 0, 200));
}

static void test_pixel_render_mz700_mode_returns_minus1(void)
{
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x08;  /* MZ700 */
    TEST_ASSERT_EQUAL_INT(-1, vram_sim_get_pixel_palette_index(&state, 0, 0));
}

static void test_locate_pixel_320_4_A_two_planes(void)
{
    st_VRAM_PIXEL_LOC locs[VRAM_SIM_MAX_PIXEL_LOCATIONS];
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x00;

    int n = vram_sim_locate_pixel(&state, 0, 0, locs);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, locs[0].plane_index);   /* I */
    TEST_ASSERT_EQUAL_INT(1, locs[1].plane_index);   /* II */
    TEST_ASSERT_EQUAL_UINT16(0, locs[0].byte_offset);
    TEST_ASSERT_EQUAL_UINT8(0, locs[0].bit_in_byte);
}

static void test_locate_pixel_320_16_four_planes(void)
{
    st_VRAM_PIXEL_LOC locs[VRAM_SIM_MAX_PIXEL_LOCATIONS];
    st_VRAM_SIM_STATE state;
    memset(&state, 0, sizeof(state));
    state.dmd = 0x02;

    int n = vram_sim_locate_pixel(&state, 7, 0, locs);
    TEST_ASSERT_EQUAL_INT(4, n);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(i, locs[i].plane_index);
        TEST_ASSERT_EQUAL_UINT16(0, locs[i].byte_offset);
        TEST_ASSERT_EQUAL_UINT8(7, locs[i].bit_in_byte);
    }
}

/* ========================================================================
 * SMOKE IDENTITY TEST - sim vs emu pro celý 16 KB rozsah
 *
 * Matrix:
 *   DMD = 0x00..0x07 (8 modes)
 *   WF mode = SINGLE/EXOR/OR/RESET/REPLACE/PSET (6 modes - jen relevantní pro write)
 *   HW scroll = on/off
 *
 * Pro read test: pro každou DMD kombinaci nastavíme RF a porovnáme
 * vramctrl_mz800_memop_read_byte(addr) vs vram_sim_cpu_read(state, addr)
 * pro celý 0x8000..0xBFFF rozsah.
 *
 * Pro write test: zapíšeme sekvenci přes oba kanály na separované plane
 * kopie a porovnáme bajt-po-bajtu.
 * ======================================================================== */

static void fill_plane_pattern(uint8_t *plane, uint32_t seed)
{
    /* Deterministic pattern: kombinace seed + index. */
    for (int i = 0; i < VRAM_SIM_PLANE_SIZE; i++) {
        plane[i] = (uint8_t)((i * 31 + seed) & 0xFF);
    }
}

static int compare_emu_vs_sim_for_dmd(uint8_t dmd, uint8_t rf, int hwscroll_on,
                                       int *out_first_diff_addr,
                                       uint8_t *out_emu_byte, uint8_t *out_sim_byte)
{
    /* Nastavit emu state. */
    g_gdg.regDMD = dmd;

    /* RF: posíláme přímo přes set_wf_rf_reg(1, value) - bit 7 = search, bit 4 = vbank,
     * bity 0-3 = plane select. */
    vramctrl_mz800_set_wf_rf_reg(1, rf);

    if (hwscroll_on) {
        /* Aktivuj scroll: SOF=0x140, SSA=0x140, SEA=0x1F40, SW=0x1E00.
         * SSA = 5<<6, SEA = 0x7d<<6, SW = 0x78<<6 (= SEA-SSA). SOF = 0x28<<3. */
        hwscroll_set_reg(0x01, 0x28);  /* SOF_LO: 0x28 << 3 = 0x140 */
        hwscroll_set_reg(0x02, 0x00);  /* SOF_HI: 0 */
        hwscroll_set_reg(0x03, 0x78);  /* SW: 0x78 << 6 = 0x1E00 */
        hwscroll_set_reg(0x04, 0x05);  /* SSA: 5 << 6 = 0x140 */
        hwscroll_set_reg(0x05, 0x7D);  /* SEA: 0x7D << 6 = 0x1F40 */
    } else {
        hwscroll_set_reg(0x01, 0x00);
        hwscroll_set_reg(0x02, 0x00);
    }

    /* Capture sim state z emu. */
    st_VRAM_SIM_STATE state;
    vram_sim_capture_from_emu(&state);

    /* Porovnat read na celém rozsahu. V 320 modu addr = 0x8000..0x9FFF (8 KB),
     * v 640 modu 0x8000..0xBFFF (16 KB). */
    uint16_t addr_end = (dmd & 0x04) ? 0xBFFF : 0x9FFF;

    for (uint16_t addr = 0x8000; addr <= addr_end; addr++) {
        /* Emu vramctrl read očekává masked offset 0x0000..0x3FFF (volaný z
         * MEMORY_VRAM_MZ800_READ_BYTE makra s `addr & 0x3FFF`). Sim API
         * akceptuje plný Z80 addr - maskuje interně. */
        uint8_t emu_b = vramctrl_mz800_memop_read_byte((uint16_t)(addr & 0x3FFF));
        uint8_t sim_b = vram_sim_cpu_read(&state, addr);
        if (emu_b != sim_b) {
            if (out_first_diff_addr) *out_first_diff_addr = addr;
            if (out_emu_byte) *out_emu_byte = emu_b;
            if (out_sim_byte) *out_sim_byte = sim_b;
            return 0; /* fail */
        }
    }
    return 1; /* PASS */
}

static void test_smoke_identity_read_matrix(void)
{
    /* Naplnit VRAM patternem. */
    fill_plane_pattern(g_memoryVRAM_I,   0x11);
    fill_plane_pattern(g_memoryVRAM_II,  0x22);
    fill_plane_pattern(g_memoryVRAM_III, 0x33);
    fill_plane_pattern(g_memoryVRAM_IV,  0x44);

    int total_combos = 0;
    int passed = 0;

    /* Matrix: DMD 0x00..0x07 (8 hodnot, jen 800 mode), RF plane selects,
     * search on/off, hwscroll on/off. */
    for (uint8_t dmd = 0x00; dmd <= 0x07; dmd++) {
        for (uint8_t rf_planes = 0x00; rf_planes <= 0x0F; rf_planes++) {
            for (int search = 0; search <= 1; search++) {
                for (int vbank = 0; vbank <= 1; vbank++) {
                    for (int hwscroll_on = 0; hwscroll_on <= 1; hwscroll_on++) {
                        uint8_t rf = (uint8_t)(rf_planes | (vbank << 4) | (search << 7));
                        total_combos++;
                        int diff_addr = 0;
                        uint8_t emu_b = 0, sim_b = 0;
                        int ok = compare_emu_vs_sim_for_dmd(dmd, rf, hwscroll_on,
                                                             &diff_addr, &emu_b, &sim_b);
                        if (ok) {
                            passed++;
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "SMOKE FAIL: DMD=0x%02X RF=0x%02X hwscrl=%d "
                                     "addr=0x%04X emu=0x%02X sim=0x%02X",
                                     dmd, rf, hwscroll_on, diff_addr, emu_b, sim_b);
                            TEST_FAIL_MESSAGE(msg);
                        }
                    }
                }
            }
        }
    }

    /* Pokud sem dorazíme, all PASS. */
    TEST_ASSERT_EQUAL_INT(total_combos, passed);
}

static void test_smoke_identity_write_matrix(void)
{
    /* Pro write side: pro každou kombinaci DMD x WF_MODE:
     *  1. Naplnit emu VRAM patternem A.
     *  2. Naplnit sim plane data patternem A (kopie).
     *  3. Provést sekvenci 32 zápisů přes oba kanály.
     *  4. Porovnat emu plane data vs sim plane data byte-by-byte.
     */

    uint8_t sim_plane[4][VRAM_SIM_PLANE_SIZE];
    int total_combos = 0;
    int passed = 0;

    /* WF modes k testu: SINGLE(0), EXOR(1), OR(2), RESET(3), REPLACE(4), PSET(6).
     * Pro REPLACE/PSET emu maskuje bit 0 v WM2=1, takze hodnoty 5 a 7 jsou neplatne. */
    uint8_t wf_modes[] = { VRAM_SIM_WF_SINGLE, VRAM_SIM_WF_EXOR, VRAM_SIM_WF_OR,
                           VRAM_SIM_WF_RESET, VRAM_SIM_WF_REPLACE, VRAM_SIM_WF_PSET };

    for (uint8_t dmd = 0x00; dmd <= 0x07; dmd++) {
        for (int wf_idx = 0; wf_idx < (int)(sizeof(wf_modes)/sizeof(wf_modes[0])); wf_idx++) {
            for (uint8_t wf_planes = 0x01; wf_planes <= 0x0F; wf_planes++) {
                for (int vbank = 0; vbank <= 1; vbank++) {
                  for (int hwscroll_on = 0; hwscroll_on <= 1; hwscroll_on++) {
                    total_combos++;

                    /* Reset patternu. */
                    fill_plane_pattern(g_memoryVRAM_I,   0x11);
                    fill_plane_pattern(g_memoryVRAM_II,  0x22);
                    fill_plane_pattern(g_memoryVRAM_III, 0x33);
                    fill_plane_pattern(g_memoryVRAM_IV,  0x44);
                    memcpy(sim_plane[0], g_memoryVRAM_I,   VRAM_SIM_PLANE_SIZE);
                    memcpy(sim_plane[1], g_memoryVRAM_II,  VRAM_SIM_PLANE_SIZE);
                    memcpy(sim_plane[2], g_memoryVRAM_III, VRAM_SIM_PLANE_SIZE);
                    memcpy(sim_plane[3], g_memoryVRAM_IV,  VRAM_SIM_PLANE_SIZE);

                    /* Setup emu DMD + WF; hwscroll_on = posun o 1 řádek (SOF=0x28)
                     * přes celou obrazovku, zapisované adresy 0x8000.. tak leží
                     * ve scroll oblasti. */
                    g_gdg.regDMD = dmd;
                    /* WF byte: bit 7 = WM2, bity 5-6 = WM1, WM0; bit 4 = vbank; bity 0-3 = plane. */
                    uint8_t wf_byte;
                    if (wf_modes[wf_idx] == VRAM_SIM_WF_REPLACE) {
                        wf_byte = (uint8_t)(0x80 | (vbank << 4) | wf_planes); /* WM2=1, WM1=0, WM0=0 */
                    } else if (wf_modes[wf_idx] == VRAM_SIM_WF_PSET) {
                        wf_byte = (uint8_t)(0xC0 | (vbank << 4) | wf_planes); /* WM2=1, WM1=1 */
                    } else {
                        wf_byte = (uint8_t)((wf_modes[wf_idx] << 5) | (vbank << 4) | wf_planes);
                    }
                    vramctrl_mz800_set_wf_rf_reg(0, wf_byte);
                    hwscroll_set_reg(0x01, hwscroll_on ? 0x05 : 0); hwscroll_set_reg(0x02, 0);
                    hwscroll_set_reg(0x03, 0x7D); hwscroll_set_reg(0x04, 0); hwscroll_set_reg(0x05, 0x7D);

                    /* Sim state setup - capture po RF/WF + ručně použít sim_plane buffery. */
                    st_VRAM_SIM_STATE state;
                    vram_sim_capture_from_emu(&state);
                    state.plane_data[0] = sim_plane[0];
                    state.plane_data[1] = sim_plane[1];
                    state.plane_data[2] = sim_plane[2];
                    state.plane_data[3] = sim_plane[3];
                    state.plane_data_writable[0] = sim_plane[0];
                    state.plane_data_writable[1] = sim_plane[1];
                    state.plane_data_writable[2] = sim_plane[2];
                    state.plane_data_writable[3] = sim_plane[3];

                    /* Sekvence 32 zápisů na adresy 0x8000, 0x8001, ... 0x801F. */
                    uint16_t addr_max_one_past = (uint8_t)(dmd & 0x04) ? 0xC000 : 0xA000;
                    (void)addr_max_one_past;
                    for (int i = 0; i < 32; i++) {
                        uint16_t addr = (uint16_t)(0x8000 + i);
                        uint8_t value = (uint8_t)((i * 13 + 7) & 0xFF);
                        /* Emu write také očekává masked offset. */
                        vramctrl_mz800_memop_write_byte((uint16_t)(addr & 0x3FFF), value);
                        vram_sim_cpu_write(&state, addr, value);
                    }

                    /* Porovnat plane data. */
                    int diff_found = 0;
                    int diff_plane = -1, diff_offset = -1;
                    uint8_t diff_emu = 0, diff_sim = 0;
                    const uint8_t *emu_planes[4] = {
                        g_memoryVRAM_I, g_memoryVRAM_II,
                        g_memoryVRAM_III, g_memoryVRAM_IV
                    };
                    for (int p = 0; p < 4 && !diff_found; p++) {
                        for (int o = 0; o < VRAM_SIM_PLANE_SIZE; o++) {
                            if (emu_planes[p][o] != sim_plane[p][o]) {
                                diff_found = 1;
                                diff_plane = p;
                                diff_offset = o;
                                diff_emu = emu_planes[p][o];
                                diff_sim = sim_plane[p][o];
                                break;
                            }
                        }
                    }

                    if (!diff_found) {
                        passed++;
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "SMOKE WRITE FAIL: DMD=0x%02X WF=0x%02X wf_mode=%d vbank=%d "
                                 "hwscrl=%d plane=%d offset=0x%04X emu=0x%02X sim=0x%02X",
                                 dmd, wf_byte, wf_modes[wf_idx], vbank, hwscroll_on,
                                 diff_plane, diff_offset, diff_emu, diff_sim);
                        TEST_FAIL_MESSAGE(msg);
                    }
                  }
                }
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(total_combos, passed);
}

/* ========================================================================
 * HW scroll a CPU přístup do nezobrazené frame (emu vramctrl)
 *
 * GDG přičítá SOF k adrese CPU přístupu jen při B/A == DMD bit 0 (HICOLOR
 * vždy). Scénář odpovídá testu, který na reálném MZ-800 kreslí pruhovaný
 * sloupec: zápis 0FFh na spodní řádek (offset 199*40+20 = 0x1F2C) při
 * posunu o jeden řádek (SOF = 5 << 3 = 0x28, SSA = 0, SEA = SW = 0x1F40).
 * Posunutá fyzická adresa: 0x1F2C >= SEA - SOF, tedy 0x1F2C + 0x28 - 0x1F40
 * = 0x0014.
 * ======================================================================== */

/** @brief Fyzická adresa zápisu na spodní řádek bez posunu. */
#define SCRL_FRAME_OFFSET     0x1F2C
/** @brief Fyzická adresa téhož zápisu po posunu o jeden řádek. */
#define SCRL_FRAME_SHIFTED    0x0014

/**
 * @brief Připraví emu: DMD, WF (SINGLE, roviny I-IV) a RF (rovina I) se
 *        zadaným B/A, scroll o 1 řádek, vynulované roviny.
 */
static void scrl_frame_setup(uint8_t dmd, uint8_t ba)
{
    memset(g_memoryVRAM_I,   0, VRAM_SIM_PLANE_SIZE);
    memset(g_memoryVRAM_II,  0, VRAM_SIM_PLANE_SIZE);
    memset(g_memoryVRAM_III, 0, VRAM_SIM_PLANE_SIZE);
    memset(g_memoryVRAM_IV,  0, VRAM_SIM_PLANE_SIZE);
    g_gdg.regDMD = dmd;
    vramctrl_mz800_set_wf_rf_reg(0, (uint8_t)((ba << 4) | 0x0F));
    vramctrl_mz800_set_wf_rf_reg(1, (uint8_t)((ba << 4) | 0x01));
    hwscroll_set_reg(0x03, 0x7D); hwscroll_set_reg(0x04, 0x00); hwscroll_set_reg(0x05, 0x7D);
    hwscroll_set_reg(0x02, 0x00); hwscroll_set_reg(0x01, 0x05);
}

/**
 * @brief Zápis i čtení CPU jdou na posunutou adresu jen při B/A == DMD bit 0.
 *
 * 320x200@4 (DMD 00h/01h) a 640x200@2 (DMD 04h/05h; CPU offset se dělí 2,
 * proto se zapisuje na 2*offset). Pro B/A != DMD bit 0 musí zápis skončit na
 * neposunuté adrese a posunutá zůstat netknutá; čtení z téhož CPU offsetu
 * musí vrátit zapsanou hodnotu.
 */
static void test_emu_cpu_scroll_only_for_displayed_frame(void)
{
    static const uint8_t bases[] = { 0x00, 0x04 };
    for (unsigned b = 0; b < sizeof(bases); b++) {
        for (uint8_t d0 = 0; d0 <= 1; d0++) {
            for (uint8_t ba = 0; ba <= 1; ba++) {
                uint8_t dmd = (uint8_t)(bases[b] | d0);
                uint16_t cpu_off = (dmd & 0x04) ? (uint16_t)(SCRL_FRAME_OFFSET * 2) : SCRL_FRAME_OFFSET;
                scrl_frame_setup(dmd, ba);
                vramctrl_mz800_memop_write_byte(cpu_off, 0xFF);

                int scrolled = (d0 == ba);
                uint16_t hit  = scrolled ? SCRL_FRAME_SHIFTED : SCRL_FRAME_OFFSET;
                uint16_t miss = scrolled ? SCRL_FRAME_OFFSET : SCRL_FRAME_SHIFTED;
                /* B/A vybírá v 640x200@2 rovinu I (A) nebo III (B) */
                const uint8_t *plane = ((dmd & 0x04) && ba) ? g_memoryVRAM_III : g_memoryVRAM_I;
                char msg[80];
                snprintf(msg, sizeof(msg), "DMD=0x%02X B/A=%u", dmd, ba);
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, plane[hit], msg);
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, plane[miss], msg);
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, vramctrl_mz800_memop_read_byte(cpu_off), msg);
            }
        }
    }
}

/**
 * @brief V HICOLOR režimech (320x200@16, 640x200@4) se scroll uplatní vždy.
 */
static void test_emu_cpu_scroll_always_in_hicolor(void)
{
    static const uint8_t bases[] = { 0x02, 0x06 };
    for (unsigned b = 0; b < sizeof(bases); b++) {
        for (uint8_t d0 = 0; d0 <= 1; d0++) {
            for (uint8_t ba = 0; ba <= 1; ba++) {
                uint8_t dmd = (uint8_t)(bases[b] | d0);
                uint16_t cpu_off = (dmd & 0x04) ? (uint16_t)(SCRL_FRAME_OFFSET * 2) : SCRL_FRAME_OFFSET;
                scrl_frame_setup(dmd, ba);
                vramctrl_mz800_memop_write_byte(cpu_off, 0xFF);
                char msg[80];
                snprintf(msg, sizeof(msg), "DMD=0x%02X B/A=%u", dmd, ba);
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, g_memoryVRAM_I[SCRL_FRAME_SHIFTED], msg);
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, g_memoryVRAM_I[SCRL_FRAME_OFFSET], msg);
            }
        }
    }
}

/* ========================================================================
 * Main runner
 * ======================================================================== */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    mztest_init();

    UNITY_BEGIN();

    /* Pure unit testy */
    RUN_TEST(test_decode_mode_all_dmd_values);
    RUN_TEST(test_metadata_canvas_and_bpp);
    RUN_TEST(test_hwscroll_disabled_default);
    RUN_TEST(test_hwscroll_recompute_enable_conditions);
    RUN_TEST(test_hwscroll_shift_up_and_down);
    RUN_TEST(test_cpu_hwscroll_applies_rule);

    /* CPU R/W unit */
    RUN_TEST(test_cpu_read_no_plane_selected_returns_FF);
    RUN_TEST(test_cpu_read_320_4_A_single_plane);
    RUN_TEST(test_cpu_read_no_exvram_returns_FF_for_plane_III);
    RUN_TEST(test_cpu_read_search_mode_match);

    RUN_TEST(test_cpu_write_single_mode_320_4_A);
    RUN_TEST(test_cpu_write_replace_clears_other_planes);
    RUN_TEST(test_cpu_write_exor_mode);
    RUN_TEST(test_cpu_write_readonly_plane_silent_skip);

    /* Pixel render */
    RUN_TEST(test_pixel_render_320_4_A_extracts_bit0);
    RUN_TEST(test_pixel_render_320_16_extracts_4_planes);
    RUN_TEST(test_pixel_render_out_of_range_returns_minus1);
    RUN_TEST(test_pixel_render_mz700_mode_returns_minus1);
    RUN_TEST(test_locate_pixel_320_4_A_two_planes);
    RUN_TEST(test_locate_pixel_320_16_four_planes);

    /* SMOKE IDENTITY - sim vs emu */
    RUN_TEST(test_smoke_identity_read_matrix);
    RUN_TEST(test_smoke_identity_write_matrix);

    /* HW scroll a CPU přístup do nezobrazené frame */
    RUN_TEST(test_emu_cpu_scroll_only_for_displayed_frame);
    RUN_TEST(test_emu_cpu_scroll_always_in_hicolor);

    return UNITY_END();
}
