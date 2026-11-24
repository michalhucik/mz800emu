/**
 * @file test_hwscroll.c
 * @brief Testy hardwarového scrollu MZ-800 (registry SOF, SW, SSA, SEA).
 *
 * Kauzalita vůči paprsku: zápis do registru scrollu uprostřed obrazového
 * řádku nesmí změnit pixely, které paprsek už vykreslil. Framebuffer se
 * dokresluje líně (framebuffer_MZ800_screen_changed() doplní řádek až
 * k aktuálnímu sloupci paprsku), proto se musí dokreslit se STARÝM
 * mapováním adres, tedy před změnou registru - stejně jako u palety a DMD
 * v mz800_gdg.c.
 *
 * Test nastaví stav GDG a VRAM přímo (bez běhu Z80): režim 320x200 4/A,
 * scroll zapnutý (SSA = 0, SEA = SW = 125, SOF = 1), paprsek uprostřed
 * řádku. VRAM plane I obsahuje vzor, ve kterém se barva bajtu mění po
 * 8 a 2048 bajtech, takže posun o SOF je v pixelech vidět.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#include "mzarch/mz800/gdg/mz800_framebuffer.h"
#include "mzarch/mz800/gdg/mz800_video.h"
#include "mzarch/mz800/memory/mz800_memory.h"

#if MZARCH != 800
#error "test_hwscroll je jen pro MZARCH=800"
#endif

/** @brief Řádek plátna, na kterém test pracuje (relativně k prvnímu řádku plátna). */
#define TEST_ROW            20
/** @brief Sloupec paprsku v pixelech plátna, kde proběhne zápis registru. */
#define TEST_BEAM_PIXEL     200
/** @brief Počet celých bajtů VRAM (po 16 pixelech) před paprskem, které se kontrolují. */
#define TEST_CHECK_BYTES    ( TEST_BEAM_PIXEL / 16 )


/**
 * @brief Barva (index PAL) bajtu VRAM ve vzoru testu.
 *
 * Mění se po 8 bajtech (posun o SOF_LO) a obrací se po 2048 bajtech
 * (posun o SOF_HI), aby test rozlišil změnu obou bajtů SOF.
 */
static unsigned pattern_color ( unsigned real_addr )
{
    return ( ( real_addr >> 3 ) ^ ( real_addr >> 11 ) ) & 1;
}


/**
 * @brief Připraví GDG, VRAM a scroll: 320x200 4/A, scroll SOF = 1 zapnutý,
 *        paprsek na řádku TEST_ROW a pixelu TEST_BEAM_PIXEL, řádek
 *        zatím nevykreslený.
 */
void setUp ( void )
{
    framebuffer_init ( );

    g_gdg.regDMD = 0x00;
    g_gdg.regPAL0 = 0;
    g_gdg.regPAL1 = 1;
    g_gdg.regPAL2 = 2;
    g_gdg.regPAL3 = 3;

    for ( unsigned addr = 0; addr < 0x2000; addr++ ) {
        g_memoryVRAM_I [ addr ] = pattern_color ( addr ) ? 0xff : 0x00;
        g_memoryVRAM_II [ addr ] = 0x00;
    };

    hwscroll_reset ( );
    hwscroll_set_reg ( 0x03, 125 );     /* SW */
    hwscroll_set_reg ( 0x04, 0 );       /* SSA */
    hwscroll_set_reg ( 0x05, 125 );     /* SEA */
    hwscroll_set_reg ( 0x02, 0 );       /* SOF_HI */
    hwscroll_set_reg ( 0x01, 1 );       /* SOF_LO -> SOF = 1 (8 bajtů) */
    TEST_ASSERT_TRUE ( g_hwscroll.enabled );

    g_gdg.beam_row = VIDEO_BEAM_CANVAS_FIRST_ROW + TEST_ROW;
    g_gdg.total_elapsed.ticks = g_gdg.beam_row * VIDEO_SCREEN_WIDTH
                              + VIDEO_BEAM_CANVAS_FIRST_COLUMN + TEST_BEAM_PIXEL;
    g_gdg.screen_need_update_from = 0;
}


void tearDown ( void ) { }


/**
 * @brief Spočítá očekávané barvy bajtů před paprskem s aktuálním mapováním.
 *
 * @param out Pole TEST_CHECK_BYTES barev (index PAL 0/1).
 * @pre Volat PŘED zápisem testovaného registru (mapování "staré").
 */
static void expected_colors ( unsigned *out )
{
    unsigned row_addr = TEST_ROW * 40;
    for ( unsigned i = 0; i < TEST_CHECK_BYTES; i++ ) {
        out [ i ] = pattern_color ( hwscroll_shift_addr ( row_addr + i ) );
    };
}


/**
 * @brief Ověří, že pixely před paprskem odpovídají očekávaným barvám.
 */
static void assert_row_prefix ( const unsigned *expected, const char *msg )
{
    const uint8_t *row = g_framebuffer.pixels
                       + g_gdg.beam_row * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH;
    for ( unsigned i = 0; i < TEST_CHECK_BYTES; i++ ) {
        for ( unsigned px = 0; px < 16; px++ ) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE ( expected [ i ], row [ i * 16 + px ], msg );
        };
    };
}


/** Zápis registru přes port (hwscroll_set_reg) nechá uplynulou část řádku. */
static void check_port_write ( unsigned reg, uint8_t value, const char *msg )
{
    unsigned expected [ TEST_CHECK_BYTES ];
    setUp ( );
    expected_colors ( expected );
    hwscroll_set_reg ( reg, value );
    framebuffer_MZ800_screen_changed ( );
    assert_row_prefix ( expected, msg );
}


/** SOF_LO: změna posunu uprostřed řádku. */
void test_sof_lo_change_keeps_elapsed_pixels ( void )
{
    check_port_write ( 0x01, 2, "SOF_LO 1 -> 2" );
}


/** SOF_LO = 0 vypne scroll; uplynulá část zůstane posunutá. */
void test_scroll_disable_keeps_elapsed_pixels ( void )
{
    check_port_write ( 0x01, 0, "SOF_LO 1 -> 0 (scroll off)" );
}


/** SOF_HI, SW, SSA, SEA: změna oblasti scrollu uprostřed řádku. */
void test_band_registers_keep_elapsed_pixels ( void )
{
    check_port_write ( 0x02, 1, "SOF_HI 0 -> 1" );
    /* SW, SSA a SEA porušující SW = SEA - SSA scroll na MZ-800 nevypnou
     * (měření U4, mz800-knowledge hw/10c-hw-scroll.md OPRAVA 2026-11-22). */
    check_port_write ( 0x03, 100, "SW 125 -> 100 (scroll stays on)" );
    check_port_write ( 0x04, 1, "SSA 0 -> 1 (scroll stays on)" );
    check_port_write ( 0x05, 100, "SEA 125 -> 100 (scroll stays on)" );
}


/** Zapnutí scrollu uprostřed řádku: uplynulá část zůstane nescrollovaná. */
void test_scroll_enable_keeps_elapsed_pixels ( void )
{
    unsigned expected [ TEST_CHECK_BYTES ];
    hwscroll_set_reg ( 0x01, 0 );
    TEST_ASSERT_FALSE ( g_hwscroll.enabled );
    g_gdg.screen_need_update_from = 0;
    expected_colors ( expected );
    hwscroll_set_reg ( 0x01, 1 );
    framebuffer_MZ800_screen_changed ( );
    assert_row_prefix ( expected, "SOF_LO 0 -> 1 (scroll on)" );
}


/** Ladicí settery a gettery používají stejné jednotky jako zápis portem. */
void test_debug_setters_match_port_units ( void )
{
    hwscroll_set_ssa ( 5 );
    TEST_ASSERT_EQUAL_UINT ( 5u << 6, g_hwscroll.regSSA );
    TEST_ASSERT_EQUAL_UINT ( 5, hwscroll_get_ssa ( ) );

    hwscroll_set_sea ( 100 );
    TEST_ASSERT_EQUAL_UINT ( 100, hwscroll_get_sea ( ) );
    hwscroll_set_sw ( 95 );
    TEST_ASSERT_EQUAL_UINT ( 95, hwscroll_get_sw ( ) );
    hwscroll_set_sof ( 0x123 );
    TEST_ASSERT_EQUAL_UINT ( 0x123, hwscroll_get_sof ( ) );
}


/** Ladicí setter uprostřed řádku také nechá uplynulou část. */
void test_debug_setter_keeps_elapsed_pixels ( void )
{
    unsigned expected [ TEST_CHECK_BYTES ];
    expected_colors ( expected );
    hwscroll_set_sof ( 2 );
    framebuffer_MZ800_screen_changed ( );
    assert_row_prefix ( expected, "hwscroll_set_sof 1 -> 2" );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_sof_lo_change_keeps_elapsed_pixels );
    RUN_TEST ( test_scroll_disable_keeps_elapsed_pixels );
    RUN_TEST ( test_band_registers_keep_elapsed_pixels );
    RUN_TEST ( test_scroll_enable_keeps_elapsed_pixels );
    RUN_TEST ( test_debug_setters_match_port_units );
    RUN_TEST ( test_debug_setter_keeps_elapsed_pixels );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
