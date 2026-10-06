/*
 * File:   mhmap_window_state.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 30. dubna 2026
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

/**
 * @file mhmap_window_state.h
 * @brief Stav Memory Heatmap GUI okna - perzistentní napříč framy.
 *
 * Drží uživatelem zvolené volby (selected region tab, color mode, threshold,
 * zoom, side panel visibility) a buffer pro importovaná CDL data. Stav je
 * jediný globální (žádné multi-instance windows). Persistence vybraných
 * položek do mz800emu.ini je přidána v Phase G.
 */

#ifndef MHMAP_WINDOW_STATE_H
#define MHMAP_WINDOW_STATE_H

#include "mzarch/mzarch_config.h"

#if defined(MZ800EMU_CFG_DEBUGGER_ENABLED) && ( ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 ) )

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>


    /**
     * @brief Mód barvy heatmap cell.
     *
     * V5: přidána varianta S (stack write) - viz @ref en_MHMAP_ACCESS.
     * "RGB" mód kombinuje R + W + X + S do aditivní směsi přes uživatelské
     * barvy color_r_rgba / color_w_rgba / color_x_rgba / color_s_rgba.
     */
    typedef enum en_MHMAP_WINDOW_COLOR_MODE
    {
        MHMAP_WINDOW_COLOR_RGB = 0,    /**< Aditivní směs R + W + X + S přes user barvy */
        MHMAP_WINDOW_COLOR_R,          /**< jen R counter, monochrome palette */
        MHMAP_WINDOW_COLOR_W,          /**< jen W counter */
        MHMAP_WINDOW_COLOR_X,          /**< jen X counter */
        MHMAP_WINDOW_COLOR_S,          /**< jen S counter (V5, stack write) */
    } en_MHMAP_WINDOW_COLOR_MODE;


    /**
     * @brief Zdroj dat pro vizualizaci.
     */
    typedef enum en_MHMAP_WINDOW_DATA_SOURCE
    {
        MHMAP_WINDOW_DATA_LIVE = 0,    /**< @c g_mhmap (live emulator data) */
        MHMAP_WINDOW_DATA_IMPORTED,    /**< @c g_mhmap_window.import_data (loaded z disku) */
    } en_MHMAP_WINDOW_DATA_SOURCE;


    /**
     * @brief Stav Memory Heatmap GUI okna.
     *
     * Globální instance @ref g_mhmap_window drží UI volby napříč framy.
     * Reset na default při startu emulátoru (BSS = 0 + post-init pravidlo
     * v @c mhmap_window_state_init).
     */
    typedef struct st_MHMAP_WINDOW
    {
        bool initialized;                    /**< @c true po prvním renderu */
        int  selected_region_idx;            /**< index v @c mhmap_get_export_regions tabulce */
        int  selected_cell_offset;           /**< -1 = žádná selekce */
        en_MHMAP_WINDOW_COLOR_MODE color_mode;  /**< barevný režim heatmap */
        bool log_scale;                      /**< @c true = log normalizace, @c false = linear */
        unsigned threshold;                  /**< cells pod tímto max(R,W,X) se nezobrazí */
        unsigned zoom;                       /**< 1, 2, 4, 8, 16, 32 - pixel size per cell */
        bool side_panel_visible;             /**< viditelnost pravého panelu s detailem */
        en_MHMAP_WINDOW_DATA_SOURCE data_source;/**< Live vs. Imported */
        bool have_import;                    /**< @c true = @c import_data je validní */
        /* import_data je velký buffer (sizeof(st_MHMAP), pro MZ-800 13 508 608 B,
         * viz mhmap.h), alokuje se dynamicky při Import. */
        void *import_data;                   /**< malloc'd kopie st_MHMAP po importu */
        /**
         * @brief Bitmaska viditelnosti regionů pro tab bar.
         *
         * Bit @c i = region @c i v @c mhmap_get_export_regions je viditelný
         * jako tab. MZ-800 má 22, MZ-1500 9 - 32-bit pojme oboje.
         * Default: bus, ram, rom-*, vram, iorq-8bit (+iorq-gdg na MZ-800).
         * Persistuje do @c .ini.
         */
        unsigned region_visibility_mask;
        /**
         * @brief Uživatelsky nastavitelné barvy pro RGB color mode.
         *
         * RGBA hodnoty (0xAABBGGRR formát, ImU32). Default:
         *  - color_r_rgba = modrá  (R counter → modrý kanál)
         *  - color_w_rgba = červená (W counter → červený kanál)
         *  - color_x_rgba = zelená  (X counter → zelený kanál)
         *  - color_s_rgba = cyan   (V5: S counter → stack write)
         *
         * Cell color v RGB módu = aditivní směs intensit jednotlivých
         * counterů × jejich barev (clamp na 0..255 per channel).
         * Persistuje do @c .ini jako 4× UNSIGNED.
         */
        unsigned color_r_rgba;
        unsigned color_w_rgba;
        unsigned color_x_rgba;
        unsigned color_s_rgba;        /**< V5: barva pro S kategorii v RGB mode */
        /**
         * @brief V5 toggle: zobrazit S kategorii v heatmap.
         *
         * Default true. Pokud false, S counter se ignoruje v RGB mode
         * (= cell s pouze S accessem zůstane černá / threshold-skrytá)
         * i v single-channel modes nevyfiltruje S za R/W/X. Implementační
         * detail v cell_color a max compute v mhmap_window_grid.cpp.
         */
        bool show_s_category;
    } st_MHMAP_WINDOW;


    extern st_MHMAP_WINDOW g_mhmap_window;


    /**
     * @brief Inicializace state na default hodnoty (1× při prvním otevření).
     *
     * Volá se z @ref mhmap_window_render při prvním vstupu do okna. Idempotentní.
     */
    extern void mhmap_window_state_init ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED && (MZARCH == 800 || MZARCH == 1500) */

#endif /* MHMAP_WINDOW_STATE_H */
