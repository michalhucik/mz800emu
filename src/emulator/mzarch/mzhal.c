/**
 * @file mzhal.c
 * @brief Per-EXE definice g_mzhal - compile-time designated inicializátor.
 *
 * Jediný soubor, který smí plnit g_mzhal. Kompiluje se per-EXE
 * s -DMZARCH/-DMZTVSYS daného targetu; hodnoty se berou VÝHRADNĚ
 * z per-arch maker (nikdy opisem literálů), takže MZTVSYS větvení
 * proběhne v preprocesoru správně per target a #error guardy per-arch
 * hlaviček zůstávají aktivní.
 *
 * Const inicializátor = statická inicializace v .rodata: g_mzhal platí
 * od load-time (MCP TCP vlákno startující před emu vláknem i SDL audio
 * callback čtou vždy platná data), žádné init-ordering okno neexistuje.
 *
 * Invarianty hodnot vynucují _Static_assert níže (compile-time);
 * mzhal_validate() dělá runtime cross-check proti mzarch_platform.c.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mzhal.h"
#include "mztvsys.h"
#include "mzarch_config.h"
#include "mzarch_platform.h"

/* Per-arch zdroje hodnot: clock (GDGCLK_*), video geometrie (VIDEO_*)
 * pres dispatcher hlavicky, sbernicove timingy primo. */
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/memory/memory_arch.h" /* per-arch MEMORY_SIZE_* pro mem_*_size pole (mzhal 11c-2c) */

#if MZARCH == 800
#include "mz800/gdg/mz800_bus_timing.h"
#elif MZARCH == 1500
#include "mz1500/gdg/mz1500_bus_timing.h"
#elif MZARCH == 700
#include "mz700/gdg/mz700_bus_timing.h"
#else
#error "Unknown MZARCH value"
#endif

/* ===========================================================================
 * Inicializační hodnoty polí - MZHAL_INIT_<pole>
 *
 * Přidání pole do MZHAL_FIELDS bez doplnění hodnoty zde je compile
 * error ("MZHAL_INIT_x undeclared") ve všech 4 EXE - žádná tichá nula.
 * ======================================================================== */

/* --- identita ----------------------------------------------------------- */
#define MZHAL_INIT_arch        MZARCH
#define MZHAL_INIT_tvsys       MZTVSYS
#define MZHAL_INIT_arch_name   MZARCH_NAME
#define MZHAL_INIT_tvsys_name  MZTVSYS_NAME

#if MZARCH == 800
#define MZHAL_INIT_full_name         "MZ-800"
#define MZHAL_INIT_arch_display_name "MZ-800"
#elif MZARCH == 1500
#define MZHAL_INIT_full_name         "MZ-1500"
#define MZHAL_INIT_arch_display_name "MZ-1500"
#elif MZARCH == 700
#if MZTVSYS == MZTVSYS_PAL
#define MZHAL_INIT_full_name         "MZ-700 (PAL)"
#else
#define MZHAL_INIT_full_name         "MZ-700 (NTSC)"
#endif
#define MZHAL_INIT_arch_display_name "MZ-700"
#endif

/* --- hodiny -------------------------------------------------------------- */
#define MZHAL_INIT_gdgclk_base         GDGCLK_BASE
#define MZHAL_INIT_gdgclk_real_base    GDGCLK_REAL_BASE
#define MZHAL_INIT_gdgclk2cpu_divider  GDGCLK2CPU_DIVIDER
#define MZHAL_INIT_gdgclk_ctc0_divider GDGCLK_CTC0_DIVIDER
#define MZHAL_INIT_cpu_hz              (GDGCLK_BASE / GDGCLK2CPU_DIVIDER)
#define MZHAL_INIT_ctc0_input_hz       (GDGCLK_BASE / GDGCLK_CTC0_DIVIDER)
#define MZHAL_INIT_psg_divider         (16 * GDGCLK2CPU_DIVIDER)

/* --- video geometrie ------------------------------------------------------
 * Pozn.: makro se jmenuje VIDEO_BORDER_BOTOM_HEIGHT (historický
 * pravopis) - pole v g_mzhal má správné "bottom". */
#define MZHAL_INIT_video_border_left_width    VIDEO_BORDER_LEFT_WIDTH
#define MZHAL_INIT_video_border_right_width   VIDEO_BORDER_RIGHT_WIDTH
#define MZHAL_INIT_video_border_top_height    VIDEO_BORDER_TOP_HEIGHT
#define MZHAL_INIT_video_border_bottom_height VIDEO_BORDER_BOTOM_HEIGHT
#define MZHAL_INIT_video_canvas_width         VIDEO_CANVAS_WIDTH
#define MZHAL_INIT_video_canvas_height        VIDEO_CANVAS_HEIGHT
#define MZHAL_INIT_video_display_width        VIDEO_DISPLAY_WIDTH
#define MZHAL_INIT_video_display_height       VIDEO_DISPLAY_HEIGHT
#define MZHAL_INIT_video_screen_width         VIDEO_SCREEN_WIDTH
#define MZHAL_INIT_video_screen_height        VIDEO_SCREEN_HEIGHT
#define MZHAL_INIT_video_screen_ticks         VIDEO_SCREEN_TICKS
#define MZHAL_INIT_video_screens_per_sec      VIDEO_SCREENS_PER_SEC
#define MZHAL_INIT_video_h_sync_ticks         VIDEO_H_SYNC_TICKS
#define MZHAL_INIT_video_h_back_porch_ticks   VIDEO_H_BACK_PORCH_TICKS
#define MZHAL_INIT_video_h_front_porch_ticks  VIDEO_H_FRONT_PORCH_TICKS

/* --- capability ----------------------------------------------------------- */
#define MZHAL_INIT_psg_count    HAVE_PSG
#define MZHAL_INIT_have_pioz80  HAVE_PIOZ80
#define MZHAL_INIT_have_fdc     CFG_HWEXT_HAVE_FDC
#define MZHAL_INIT_have_ide8    CFG_HWEXT_HAVE_IDE8
#define MZHAL_INIT_have_ramdisk CFG_HWEXT_HAVE_RAMDISK
#define MZHAL_INIT_have_qdisk   CFG_HWEXT_HAVE_QDISK
#define MZHAL_INIT_audio_src_channels (1 + 4 * HAVE_PSG)
/* CTC0 audio gate: MZ-800 a MZ-1500 hradlují beeper signálem PC00
 * (8255), MZ-700 ne - hodnota odpovídá dřívějšímu #if MZARCH != 700
 * u CTC_AUDIO_MASK v pio8255.h. */
#if MZARCH != 700
#define MZHAL_INIT_audio_ctc0_gate_pc00 1
#else
#define MZHAL_INIT_audio_ctc0_gate_pc00 0
#endif
/* Klávesa TAB (col 0, bit 3) existuje jen v matici MZ-800 - viz
 * iface_keyboard.c (bit se na MZ-700/1500 nikdy nevystavuje). */
#if MZARCH == 800
#define MZHAL_INIT_has_key_tab 1
#else
#define MZHAL_INIT_has_key_tab 0
#endif

/* Zadní DIP přepínač MZ-700 modu: MZ-800 a MZ-1500 ho mají, MZ-700
 * nativní ne (hodnota odpovídá dřívějšímu #if MZARCH != 700 u makra
 * MZARCH_TEST_REAR_DIP_SWITCH700 v mzarch.h). */
#if MZARCH != 700
#define MZHAL_INIT_has_rear_dip_switch700 1
#else
#define MZHAL_INIT_has_rear_dip_switch700 0
#endif

/* --- paměti (superset st_MEMORY, mzhal 9d) -------------------------------- */
/* Skutečné velikosti pamětí platformy; pole v st_MEMORY mají MAX
 * dimenze (memory.h), 0 = paměť na platformě neexistuje. */
#define MZHAL_INIT_mem_vram_size MEMORY_SIZE_VRAM
#if MZARCH == 800
#define MZHAL_INIT_mem_exvram_size MEMORY_SIZE_VRAM
#else
#define MZHAL_INIT_mem_exvram_size 0
#endif
#if MZARCH == 1500
#define MZHAL_INIT_mem_pcg_size MEMORY_SIZE_PCG
#else
#define MZHAL_INIT_mem_pcg_size 0
#endif

_Static_assert(MEMORY_SIZE_VRAM <= MEMORY_SIZE_VRAM_MAX,
               "MEMORY_SIZE_VRAM presahuje superset MAX dimenzi");

/* --- sběrnicové timingy ---------------------------------------------------- */
#define MZHAL_INIT_iorq_rd_ticks    IORQ_RD_TICKS
#define MZHAL_INIT_iorq_wr_ticks    IORQ_WR_TICKS
#define MZHAL_INIT_mreq_rd_m1_ticks MREQ_RD_M1_TICKS
#define MZHAL_INIT_mreq_rd_ticks    MREQ_RD_TICKS
#define MZHAL_INIT_mreq_wr_ticks    MREQ_WR_TICKS

/* ===========================================================================
 * Compile-time invarianty (selftest hodnot)
 * ======================================================================== */

/* Simulovaná GDG frekvence je odvozený invariant rasteru - záměna
 * s fyzickým krystalem (gdgclk_real_base) by prošla vším ostatním. */
_Static_assert(GDGCLK_BASE == (uint32_t)VIDEO_SCREEN_TICKS * VIDEO_SCREENS_PER_SEC,
               "gdgclk_base != screen_ticks * screens_per_sec");

/* TV norma je jedna osa: MZTVSYS (50/60) musí sedět s počtem obrazovek. */
_Static_assert(VIDEO_SCREENS_PER_SEC == MZTVSYS,
               "video_screens_per_sec != MZTVSYS");

/* Display = border + canvas (obě osy). */
_Static_assert(VIDEO_DISPLAY_WIDTH ==
               VIDEO_BORDER_LEFT_WIDTH + VIDEO_CANVAS_WIDTH + VIDEO_BORDER_RIGHT_WIDTH,
               "display_width != left + canvas + right");
_Static_assert(VIDEO_DISPLAY_HEIGHT ==
               VIDEO_BORDER_TOP_HEIGHT + VIDEO_CANVAS_HEIGHT + VIDEO_BORDER_BOTOM_HEIGHT,
               "display_height != top + canvas + bottom");

/* Identitní sanity per platforma: dvojice deliček je vázaná na
 * (arch, tvsys) - klíčování jen podle MZARCH by dalo mz700emu-ntsc
 * tiše PAL hodnoty. */
#if MZARCH == 700
#if MZTVSYS == MZTVSYS_PAL
_Static_assert(GDGCLK2CPU_DIVIDER == 5 && GDGCLK_CTC0_DIVIDER == 16,
               "MZ-700 PAL expects dividers 5/16");
#else
_Static_assert(GDGCLK2CPU_DIVIDER == 4 && GDGCLK_CTC0_DIVIDER == 13,
               "MZ-700 NTSC expects dividers 4/13");
#endif
#elif MZARCH == 800
_Static_assert(GDGCLK2CPU_DIVIDER == 5 && GDGCLK_CTC0_DIVIDER == 16,
               "MZ-800 expects dividers 5/16");
#elif MZARCH == 1500
_Static_assert(GDGCLK2CPU_DIVIDER == 4 && GDGCLK_CTC0_DIVIDER == 16,
               "MZ-1500 expects dividers 4/16");
#endif

/* ===========================================================================
 * Definice g_mzhal
 * ======================================================================== */

const st_MZHAL g_mzhal = {
#define MZHAL_FIELD_INIT(type, name) .name = MZHAL_INIT_##name,
    MZHAL_FIELDS(MZHAL_FIELD_INIT)
#undef MZHAL_FIELD_INIT
};


void mzhal_validate(void)
{
    /* Cross-check proti druhému zdroji platformní identity
     * (mzarch_platform.c) - oba se plní ze stejných maker, ale jinými
     * cestami; rozjetí = chyba refaktoru. */
    int ok = 1;

    if (g_mzhal.arch != g_mzarch_platform_numeric)
    {
        fprintf(stderr, "mzhal_validate: arch %d != g_mzarch_platform_numeric %d\n",
                g_mzhal.arch, g_mzarch_platform_numeric);
        ok = 0;
    };
    if (g_mzhal.gdgclk_base != g_mzarch_platform_pxclk)
    {
        fprintf(stderr, "mzhal_validate: gdgclk_base %u != g_mzarch_platform_pxclk %u\n",
                (unsigned)g_mzhal.gdgclk_base, (unsigned)g_mzarch_platform_pxclk);
        ok = 0;
    };
    if (strcmp(g_mzhal.arch_name, g_mzarch_platform_name) != 0)
    {
        fprintf(stderr, "mzhal_validate: arch_name '%s' != g_mzarch_platform_name '%s'\n",
                g_mzhal.arch_name, g_mzarch_platform_name);
        ok = 0;
    };
    if (strcmp(g_mzhal.full_name, g_mzarch_full_name) != 0)
    {
        fprintf(stderr, "mzhal_validate: full_name '%s' != g_mzarch_full_name '%s'\n",
                g_mzhal.full_name, g_mzarch_full_name);
        ok = 0;
    };

    if (!ok)
    {
        fprintf(stderr, "mzhal_validate: inconsistent HW layer specification - aborting.\n");
        abort();
    };
}
