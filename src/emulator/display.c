/*
 * File:   display.c
 * Author: chaky
 *
 * Created on 14. června 2015, 9:41
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

#include "main.h"
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include "libs/sdlapp/sdlapp.h"
#include "display.h"
#include "iface/iface_video.h"

#include "cfgmain.h"

uint32_t g_display_predef_colors[DISPLAY_MZCOLORS] = {
    0x000000, 0x4040ac, 0xd03400, 0xb40c8c,
    0x406c00, 0x24ccff, 0xe8d430, 0xd0d0d0,
    0x848484, 0x008ce8, 0xff0000, 0xf054cc,
    0x54ff54, 0x80ffff, 0xffff28, 0xffffff};

uint32_t g_display_predef_grays[DISPLAY_MZCOLORS] = {
    0x000000, 0x545454, 0x606060, 0x6c6c6c,
    0x909090, 0x9c9c9c, 0xc0c0c0, 0xcccccc,
    0x787878, 0x848484, 0xa8a8a8, 0xb4b4b4,
    0xd8d8d8, 0xe4e4e4, 0xf0f0f0, 0xffffff};

uint32_t g_display_predef_greens[DISPLAY_MZCOLORS] = {
    0x000000, 0x005400, 0x006000, 0x006c00,
    0x009000, 0x009c00, 0x00c000, 0x00cc00,
    0x007800, 0x008400, 0x00a800, 0x00b400,
    0x00d800, 0x00e400, 0x00f000, 0x00ff00};

const char *display_color_schema_name[DISPLAY_COLORS_COUNT] = {
    "Normal",
    "Grayscale",
    "Green"};

const float display_predef_scale[DISPLAY_STARTUP_SIZES_COUNT] = {
    DISPLAY_SCALE_HALF,
    DISPLAY_SCALE_NATIVE,
    DISPLAY_SCALE_NORMAL, 
    DISPLAY_SCALE_BIGGER, 
    DISPLAY_SCALE_BIG};

const char *display_predef_scale_name[DISPLAY_STARTUP_SIZES_COUNT] = {
    "Half",
    "Native",
    "Normal",
    "Bigger",
    "Big",
    "Fullscreen"};

const char *display_predef_framerate_mode_name[DISPLAY_FRAMERATE_MODE_COUNT] = {
    "Adaptive",
    "Immediate",
    "Vsync",
    "Custom",
    "Slave"};

st_DISPLAY g_display;

static void display_print_current_settings(void)
{
    g_print("Display:\n");
    g_print("         Startup Window Size: %s\n", display_predef_scale_name[g_display.startup_window_size]);
    g_print("         Locked Aspect Ratio: %s\n", g_display.locked_window_aspect_ratio ? "Yes" : "No");
    g_print("         Color Schema:        %s\n", display_color_schema_name[g_display.color_schema]);
    g_print("         Forced Redrawing:    %s\n", g_display.forced_full_screen_redrawing ? "Yes" : "No");
    if (g_display.scanlines)
    {
        g_print("         CRT Scanlines:       Yes (%d %%)\n", g_display.scanlines_intensity);
    }
    else
    {
        g_print("         CRT Scanlines:       No\n");
    };
    g_print("         Framerate mode:      %s", display_predef_framerate_mode_name[g_display.framerate_mode]);
    if (g_display.framerate_mode == DISPLAY_FRAMERATE_MODE_CUSTOM)
    {
        g_print(" (%d Hz)\n\n", g_display.custom_fps);
    }
    else
    {
        g_print("\n\n");
    };
}

void display_init(void)
{

    en_DISPLAY_COLOR_SCHEMA i;
    for (i = 0; i < DISPLAY_COLORS_COUNT; i++)
    {
        switch (i)
        {
        case DISPLAY_NORMAL:
            g_display.color_predef[i] = g_display_predef_colors;
            break;
        case DISPLAY_GRAYSCALE:
            g_display.color_predef[i] = g_display_predef_grays;
            break;
        case DISPLAY_GREEN:
            g_display.color_predef[i] = g_display_predef_greens;
            break;
        case DISPLAY_COLORS_COUNT:
            break;
        };
    };

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "DISPLAY");

    CFGELM *elm;
    elm = cfgmodule_register_new_element(cmod, "color_schema", CFGENTYPE_KEYWORD, DISPLAY_DEFAULT_COLOR_SCHEMA,
                                         DISPLAY_NORMAL, "NORMAL",
                                         DISPLAY_GRAYSCALE, "GRAYSCALE",
                                         DISPLAY_GREEN, "GREEN",
                                         -1);
    cfgelement_set_handlers(elm, (void *)&g_display.color_schema, (void *)&g_display.color_schema);

    elm = cfgmodule_register_new_element(cmod, "framerate_mode", CFGENTYPE_KEYWORD, DISPLAY_DEFAULT_FRAMERATE_MODE,
                                         DISPLAY_FRAMERATE_MODE_ADAPTIVE, "ADAPTIVE",
                                         DISPLAY_FRAMERATE_MODE_IMMEDIATE, "IMMEDIATE",
                                         DISPLAY_FRAMERATE_MODE_VSYNC, "VSYNC",
                                         DISPLAY_FRAMERATE_MODE_CUSTOM, "CUSTOM",
                                         DISPLAY_FRAMERATE_MODE_SLAVE, "SLAVE",
                                         -1);
    cfgelement_set_handlers(elm, (void *)&g_display.framerate_mode, (void *)&g_display.framerate_mode);

    elm = cfgmodule_register_new_element(cmod, "custom_fps", CFGENTYPE_UNSIGNED, DISPLAY_CUSTOM_FPS_DEFAULT);
    cfgelement_set_handlers(elm, (void *)&g_display.custom_fps, (void *)&g_display.custom_fps);

    elm = cfgmodule_register_new_element(cmod, "forced_full_screen_redrawing", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *)&g_display.forced_full_screen_redrawing, (void *)&g_display.forced_full_screen_redrawing);

    elm = cfgmodule_register_new_element(cmod, "locked_window_aspect_ratio", CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm, (void *)&g_display.locked_window_aspect_ratio, (void *)&g_display.locked_window_aspect_ratio);

    elm = cfgmodule_register_new_element(cmod, "startup_size", CFGENTYPE_KEYWORD, DISPLAY_DEFAULT_STARTUP_SIZE,
                                         DISPLAY_STARTUP_SIZE_HALF, "HALF",
                                         DISPLAY_STARTUP_SIZE_NATIVE, "NATIVE",
                                         DISPLAY_STARTUP_SIZE_NORMAL, "NORMAL",
                                         DISPLAY_STARTUP_SIZE_BIGGER, "BIGGER",
                                         DISPLAY_STARTUP_SIZE_BIG, "BIG",
                                         DISPLAY_STARTUP_SIZE_FULLSCREEN, "FULLSCREEN",
                                         -1);
    cfgelement_set_handlers(elm, (void *)&g_display.startup_window_size, (void *)&g_display.startup_window_size);

    elm = cfgmodule_register_new_element(cmod, "windows_forced_reset_hdr_on_return_from_fullscreen", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *)&g_display.windows_forced_reset_hdr_on_return_from_fullscreen, (void *)&g_display.windows_forced_reset_hdr_on_return_from_fullscreen);

    elm = cfgmodule_register_new_element(cmod, "scanlines", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *)&g_display.scanlines, (void *)&g_display.scanlines);

    elm = cfgmodule_register_new_element(cmod, "scanlines_intensity", CFGENTYPE_UNSIGNED, DISPLAY_SCANLINES_INTENSITY_DEFAULT,
                                         DISPLAY_SCANLINES_INTENSITY_MIN, DISPLAY_SCANLINES_INTENSITY_MAX);
    cfgelement_set_handlers(elm, (void *)&g_display.scanlines_intensity, (void *)&g_display.scanlines_intensity);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    /* Hodnotu mimo min/max knihovna cfgfile nepřijme a ponechá výchozí
     * (ověřeno: scanlines_intensity = 0xC8 -> 50 %); ořez je jen pojistka. */
    display_set_scanlines_intensity(g_display.scanlines_intensity);

    if (g_iface_video_callbacks->set_colors)
    {
        g_iface_video_callbacks->set_colors(display_get_default_color_schema());
    }
    else
    {
        WARN("Display: set color schema '%s' - callback is not implemented\n", display_color_schema_name[g_display.color_schema]);
    };

    if (g_iface_video_callbacks->set_framerate_mode)
    {
        g_iface_video_callbacks->set_framerate_mode(g_display.framerate_mode, g_display.custom_fps);
    }
    else
    {
        WARN("Display: set_framerate_mode '%s' - callback is not implemented\n", display_predef_framerate_mode_name[g_display.framerate_mode]);
    };

    if ((g_display.custom_fps < DISPLAY_CUSTOM_FPS_MIN) || (g_display.custom_fps > DISPLAY_CUSTOM_FPS_MAX))
    {
        if (g_display.framerate_mode != DISPLAY_FRAMERATE_MODE_CUSTOM)
        {
            WARN("Custom FPS out of range (%d - %d), setting to default value\n", DISPLAY_CUSTOM_FPS_MIN, DISPLAY_CUSTOM_FPS_MAX);
        };
        g_display.custom_fps = DISPLAY_CUSTOM_FPS_DEFAULT;
    };

    // Nastavit velikost okna, pripadne fullscreen a zobrazit display settings info
    // Pokud je predvolen velikost okna fullscreen, tak nejprve nastavime default size a az potom fullscreen
    bool fullscreen_startup = (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_FULLSCREEN);
    if (fullscreen_startup)
    {
        if (g_iface_video_callbacks->set_window_size_by_scale)
        {
            g_iface_video_callbacks->set_window_size_by_scale(display_predef_scale[DISPLAY_STARTUP_SIZE_NORMAL]);
        }
        else
        {
            WARN("Display: set window size '%s' - callback is not implemented\n", display_predef_scale_name[DISPLAY_STARTUP_SIZE_NORMAL]);
        };
        // Vycentrovat NORMAL "restore" okno JEŠTĚ před přepnutím do fullscreenu,
        // aby okno po opuštění fullscreenu padlo na střed.
        if (g_iface_video_callbacks->set_window_centered)
            g_iface_video_callbacks->set_window_centered();
    };
    display_set_window_startup_scale(g_display.startup_window_size);

    // Okno se při vzniku vycentrovalo v base velikosti (= scale Native); pro
    // ostatní startup velikosti ho po aplikaci scale (= resize, který pozici
    // nemění) vycentrujeme znovu. Ve fullscreenu už je restore okno vycentrované
    // výše. FIFO fronta zpráv okna zajistí pořadí resize -> center.
    if (!fullscreen_startup)
    {
        if (g_iface_video_callbacks->set_window_centered)
            g_iface_video_callbacks->set_window_centered();
    };
}

uint32_t *display_get_default_color_schema(void)
{
    return g_display.color_predef[g_display.color_schema];
}

void display_set_colors(en_DISPLAY_COLOR_SCHEMA color_schema)
{
    if (color_schema > (DISPLAY_COLORS_COUNT - 1))
    {
        color_schema = DISPLAY_DEFAULT_COLOR_SCHEMA;
    };
    g_display.color_schema = color_schema;

    if (g_iface_video_callbacks->set_colors)
    {
        g_iface_video_callbacks->set_colors(g_display.color_predef[g_display.color_schema]);
    }
    else
    {
        WARN("Display: set color schema '%s' - callback is not implemented\n", display_color_schema_name[g_display.color_schema]);
    };

    display_print_current_settings();
}

unsigned display_get_window_color_schema(void)
{
    return g_display.color_schema;
}

void display_set_window_startup_scale(en_DISPLAY_STARTUP_SIZE startup_window_size)
{
    if (startup_window_size > (DISPLAY_STARTUP_SIZES_COUNT - 1))
    {
        startup_window_size = DISPLAY_DEFAULT_STARTUP_SIZE;
    };
    g_display.startup_window_size = startup_window_size;

    if (g_display.startup_window_size == DISPLAY_STARTUP_SIZE_FULLSCREEN)
    {
        if (g_iface_video_callbacks->set_fullscreen)
        {
            g_iface_video_callbacks->set_fullscreen(true);
        }
        else
        {
            WARN("Display: set fullscreen - callback is not implemented\n");
        };
    }
    else
    {
        if (g_iface_video_callbacks->set_window_size_by_scale)
        {
            g_iface_video_callbacks->set_window_size_by_scale(display_predef_scale[startup_window_size]);
        }
        else
        {
            WARN("Display: set window size '%s' - callback is not implemented\n", display_predef_scale_name[startup_window_size]);
        };
    };

    display_print_current_settings();
}

float display_get_window_startup_scale(void)
{
    return display_predef_scale[g_display.startup_window_size];
}

void display_set_forced_redrawing(int value)
{
    g_display.forced_full_screen_redrawing = value;
    display_print_current_settings();
}

void display_set_locked_aspect_ratio(int value)
{
    g_display.locked_window_aspect_ratio = value;
    display_print_current_settings();
}

void display_set_framerate_mode(en_DISPLAY_FRAMERATE_MODE mode)
{
    if (mode > (DISPLAY_FRAMERATE_MODE_COUNT - 1))
    {
        mode = DISPLAY_DEFAULT_FRAMERATE_MODE;
    };
    en_DISPLAY_FRAMERATE_MODE old_mode = g_display.framerate_mode;
    g_display.framerate_mode = mode;

    if ((g_display.custom_fps < DISPLAY_CUSTOM_FPS_MIN) || (g_display.custom_fps > DISPLAY_CUSTOM_FPS_MAX))
    {
        if (g_display.framerate_mode != DISPLAY_FRAMERATE_MODE_CUSTOM)
        {
            WARN("Custom FPS out of range (%d - %d), setting to default value\n", DISPLAY_CUSTOM_FPS_MIN, DISPLAY_CUSTOM_FPS_MAX);
        };
        g_display.custom_fps = DISPLAY_CUSTOM_FPS_DEFAULT;
    };

    if (g_iface_video_callbacks->set_framerate_mode)
    {
        g_iface_video_callbacks->set_framerate_mode(g_display.framerate_mode, g_display.custom_fps);
    }
    else
    {
        WARN("Display: set_framerate_mode '%s' - callback is not implemented\n", display_predef_framerate_mode_name[g_display.framerate_mode]);
    };

    if (old_mode != mode)
    {
        display_print_current_settings();
    }
    else if (mode == DISPLAY_FRAMERATE_MODE_CUSTOM)
    {
        g_print("Display custom FPS: %d Hz\n", g_display.custom_fps);
    };
}
void display_set_scanlines(int value)
{
    g_display.scanlines = (value) ? 1 : 0;
    display_print_current_settings();
}

void display_set_scanlines_intensity(int percent)
{
    if (percent < DISPLAY_SCANLINES_INTENSITY_MIN)
    {
        percent = DISPLAY_SCANLINES_INTENSITY_MIN;
    }
    else if (percent > DISPLAY_SCANLINES_INTENSITY_MAX)
    {
        percent = DISPLAY_SCANLINES_INTENSITY_MAX;
    };
    g_display.scanlines_intensity = percent;
}
