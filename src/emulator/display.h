/*
 * File:   display.h
 * Author: chaky
 *
 * Created on 14. června 2015, 9:44
 *
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

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

/* celkovy pocet emulovanych barev */
#define DISPLAY_MZCOLORS 16

typedef enum en_DISPLAY_COLOR_SCHEMA
{
    DISPLAY_NORMAL = 0,
    DISPLAY_GRAYSCALE,
    DISPLAY_GREEN,
    DISPLAY_COLORS_COUNT
} en_DISPLAY_COLOR_SCHEMA;

typedef enum en_DISPLAY_STARTUP_SIZE
{
    DISPLAY_STARTUP_SIZE_HALF = 0,
    DISPLAY_STARTUP_SIZE_NATIVE,
    DISPLAY_STARTUP_SIZE_NORMAL,
    DISPLAY_STARTUP_SIZE_BIGGER,
    DISPLAY_STARTUP_SIZE_BIG,
    DISPLAY_STARTUP_SIZE_FULLSCREEN,
    DISPLAY_STARTUP_SIZES_COUNT
} en_DISPLAY_STARTUP_SIZE;

typedef enum en_DISPLAY_FRAMERATE_MODE
{
    DISPLAY_FRAMERATE_MODE_ADAPTIVE = 0,
    DISPLAY_FRAMERATE_MODE_IMMEDIATE,
    DISPLAY_FRAMERATE_MODE_VSYNC,
    DISPLAY_FRAMERATE_MODE_CUSTOM,
    DISPLAY_FRAMERATE_MODE_SLAVE,
    DISPLAY_FRAMERATE_MODE_COUNT
} en_DISPLAY_FRAMERATE_MODE;


/**
 * @brief Nastavení zobrazení obrazu emulátoru v hlavním okně (sekce INI [DISPLAY]).
 *
 * Hodnoty plní cfgmain v display_init() a mění je setter funkce display_set_*()
 * z menu Display (UI vlákno). Čte je hlavně UI vlákno při vykreslování.
 *
 * @invariant scanlines_intensity je v rozsahu DISPLAY_SCANLINES_INTENSITY_MIN až
 *            DISPLAY_SCANLINES_INTENSITY_MAX (zajišťuje display_init() a
 *            display_set_scanlines_intensity()).
 */
typedef struct st_DISPLAY
{
    en_DISPLAY_COLOR_SCHEMA color_schema;
    uint32_t *color_predef[DISPLAY_COLORS_COUNT];
    int forced_full_screen_redrawing;
    int locked_window_aspect_ratio;
    int windows_forced_reset_hdr_on_return_from_fullscreen;
    en_DISPLAY_FRAMERATE_MODE framerate_mode;
    int custom_fps;
    en_DISPLAY_STARTUP_SIZE startup_window_size;
    int scanlines;           /**< Emulace scanlines CRT zapnuta (0/1, klíč INI `scanlines`). */
    int scanlines_intensity; /**< Ztmavení mezery mezi rastrovými řádky v procentech (klíč INI `scanlines_intensity`). */
} st_DISPLAY;

extern st_DISPLAY g_display;

#define DISPLAY_SCALE_HALF 0.5f
#define DISPLAY_SCALE_NATIVE 1.0f
#define DISPLAY_SCALE_NORMAL 1.5f
#define DISPLAY_SCALE_BIGGER 2.0f
#define DISPLAY_SCALE_BIG 2.5f
#define DISPLAY_SCALE_FULLSCREEN DISPLAY_SCALE_NATIVE

#define DISPLAY_CUSTOM_FPS_MIN 50
#define DISPLAY_CUSTOM_FPS_MAX 1000
#define DISPLAY_CUSTOM_FPS_DEFAULT 100

/** @brief Nejmenší povolená intenzita scanlines v procentech (slabší efekt už není vidět). */
#define DISPLAY_SCANLINES_INTENSITY_MIN 10
/** @brief Největší intenzita scanlines v procentech (100 = mezera mezi řádky úplně černá). */
#define DISPLAY_SCANLINES_INTENSITY_MAX 100
/** @brief Výchozí intenzita scanlines v procentech (zvoleno od oka, neodvozeno z měření CRT). */
#define DISPLAY_SCANLINES_INTENSITY_DEFAULT 50

#define DISPLAY_DEFAULT_COLOR_SCHEMA DISPLAY_NORMAL
#define DISPLAY_DEFAULT_STARTUP_SIZE DISPLAY_STARTUP_SIZE_NORMAL
#define DISPLAY_DEFAULT_SCALE DISPLAY_SCALE_NORMAL
#define DISPLAY_DEFAULT_FRAMERATE_MODE DISPLAY_FRAMERATE_MODE_IMMEDIATE

#define DISPLAY_TEST_STARTUP_SIZE_HALF (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_HALF)
#define DISPLAY_TEST_STARTUP_SIZE_NATIVE (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_NATIVE)
#define DISPLAY_TEST_STARTUP_SIZE_NORMAL (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_NORMAL)
#define DISPLAY_TEST_STARTUP_SIZE_BIGGER (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_BIGGER)
#define DISPLAY_TEST_STARTUP_SIZE_BIG (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_BIG)
#define DISPLAY_TEST_STARTUP_SIZE_FULLSCREEN (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_FULLSCREEN)

#define DISPLAY_TEST_FORCED_RESET_HDR (g_display.windows_forced_reset_hdr_on_return_from_fullscreen)
#define DISPLAY_SET_FORCED_RESET_HDR(x) (g_display.windows_forced_reset_hdr_on_return_from_fullscreen = (x))

#define DISPLAY_TEST_COLOR_NORMAL (g_display.color_schema == DISPLAY_NORMAL)
#define DISPLAY_TEST_COLOR_GRAYSCALE (g_display.color_schema == DISPLAY_GRAYSCALE)
#define DISPLAY_TEST_COLOR_GREEN (g_display.color_schema == DISPLAY_GREEN)

#define DISPLAY_TEST_LOCKED_ASPECT_RATIO (g_display.locked_window_aspect_ratio)
#define DISPLAY_TEST_FORCED_FULL_SCREEN_REDRAWING (g_display.forced_full_screen_redrawing)
/** @brief Nenulové, pokud je zapnutá emulace scanlines CRT. */
#define DISPLAY_TEST_SCANLINES (g_display.scanlines)

#define DISPLAY_TEST_FRAMERATE_MODE_ADAPTIVE (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_ADAPTIVE)
#define DISPLAY_TEST_FRAMERATE_MODE_IMMEDIATE (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_IMMEDIATE)
#define DISPLAY_TEST_FRAMERATE_MODE_VSYNC (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_VSYNC)
#define DISPLAY_TEST_FRAMERATE_MODE_CUSTOM (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_CUSTOM)
#define DISPLAY_TEST_FRAMERATE_MODE_SLAVE (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_SLAVE)

extern uint32_t g_display_predef_colors[DISPLAY_MZCOLORS];
extern uint32_t g_display_predef_grays[DISPLAY_MZCOLORS];
extern uint32_t g_display_predef_greens[DISPLAY_MZCOLORS];

#ifdef __cplusplus
extern "C"
{
#endif

    void display_init(void);
    uint32_t *display_get_default_color_schema(void);
    void display_set_colors(en_DISPLAY_COLOR_SCHEMA color_schema);
    unsigned display_get_window_color_schema(void);
    void display_set_window_startup_scale(en_DISPLAY_STARTUP_SIZE startup_window_size);
    float display_get_window_startup_scale(void);
    void display_set_forced_redrawing(int value);
    void display_set_locked_aspect_ratio(int value);
    void display_set_framerate_mode(en_DISPLAY_FRAMERATE_MODE mode);

    /**
     * @brief Zapne nebo vypne emulaci scanlines CRT v hlavním okně.
     *
     * Efekt je jen vrstva nad obrazem v hlavním okně; framebuffer emulátoru,
     * snímky obrazovky ani nahrávání videa se nemění. Hodnota se uloží do INI
     * spolu s ostatním nastavením [DISPLAY].
     *
     * @param value Nenulové = zapnout, 0 = vypnout.
     * @pre display_init() už proběhl.
     * @post g_display.scanlines je 0 nebo 1; aktuální nastavení vypsáno na stdout.
     * @par Vlákna Volá UI vlákno (menu Display).
     */
    void display_set_scanlines(int value);

    /**
     * @brief Nastaví intenzitu scanlines CRT (ztmavení mezery mezi rastrovými řádky).
     *
     * @param percent Intenzita v procentech; hodnota mimo rozsah
     *                DISPLAY_SCANLINES_INTENSITY_MIN až DISPLAY_SCANLINES_INTENSITY_MAX
     *                se ořízne na nejbližší mez.
     * @pre display_init() už proběhl.
     * @post g_display.scanlines_intensity je v povoleném rozsahu. Na stdout nic
     *       nevypisuje (volá se při každém posunu posuvníku).
     * @par Vlákna Volá UI vlákno (menu Display).
     */
    void display_set_scanlines_intensity(int percent);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
