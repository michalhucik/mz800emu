/*
 * mhmap_window.cpp - Memory Heatmap GUI okno.
 *
 * Phase A skeleton + Phase B top control bar a region tab bar.
 * Další fáze (grid render, filter bar, side panel, import, persistence)
 * se přidávají postupně, viz devdoc/PLAN-MemoryHeatmap.md.
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

#include "mzarch/mzarch_config.h"

#if defined(MZ800EMU_CFG_DEBUGGER_ENABLED) && ( ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 ) )

#include <cstdlib>
#include <cstring>
#include <string>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include "debugger/debugger.h"
#include "debugger/mhmap.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

#include "mhmap_window.h"
#include "mhmap_window_state.h"
#include "mhmap_window_grid.h"


/* ========================================================================= */
/*  Globální stav                                                            */
/* ========================================================================= */


extern "C"
{
    st_MHMAP_WINDOW g_mhmap_window = {};
}


/* Stav file dialogu - drží se mimo render funkce, aby dialog mohl
 * pokračovat napříč framy po zavření menu. */
static bool s_export_dialog_open = false;
static bool s_import_dialog_open = false;

/* Last import error - zobrazí se v toolu jako červený text. */
static char s_import_status[ 256 ] = { 0 };
static bool s_import_status_is_error = false;


/* ========================================================================= */
/*  State init                                                               */
/* ========================================================================= */


extern "C" void mhmap_window_state_init ( void )
{
    if ( g_mhmap_window.initialized ) return;

    g_mhmap_window.selected_region_idx  = 0;          /* první region = bus */
    g_mhmap_window.selected_cell_offset = -1;
    g_mhmap_window.color_mode           = MHMAP_WINDOW_COLOR_RGB;
    g_mhmap_window.log_scale            = true;
    g_mhmap_window.threshold            = 0;
    g_mhmap_window.zoom                 = 2;
    g_mhmap_window.side_panel_visible   = true;
    g_mhmap_window.data_source          = MHMAP_WINDOW_DATA_LIVE;
    g_mhmap_window.have_import          = false;
    g_mhmap_window.import_data          = NULL;
    /* Výchozí viditelnost: bus, ram, rom-*, vram, iorq-8bit (+ iorq-gdg na MZ-800),
     * memext. */
#if MZARCH == 800
    /* index 22 = memext (po iorq-gdg = 21) */
    g_mhmap_window.region_visibility_mask = 0x0070003Fu;  /* bus,ram,rom-lower,rom-cg,rom-upper,vram,iorq-8bit,iorq-gdg,memext */
#elif MZARCH == 1500
    /* index 9 = memext (po iorq-8bit = 8) */
    g_mhmap_window.region_visibility_mask = 0x0000031Fu;  /* bus,ram,rom,cgrom,vram,iorq-8bit,memext */
#elif MZARCH == 700
    /* index 6 = memext (po iorq-8bit = 5), 7 regionů celkem */
    g_mhmap_window.region_visibility_mask = 0x0000007Fu;  /* bus,ram,rom,cgrom,vram,iorq-8bit,memext */
#endif
    /* Default barvy: R=modrá, W=červená, X=zelená, S=cyan (V5).
     * IM_COL32 (R, G, B, A) - hodnota je AABBGGRR v paměti. */
    g_mhmap_window.color_r_rgba = IM_COL32 (   0,   0, 255, 255 );  /* blue */
    g_mhmap_window.color_w_rgba = IM_COL32 ( 255,   0,   0, 255 );  /* red */
    g_mhmap_window.color_x_rgba = IM_COL32 (   0, 255,   0, 255 );  /* green */
    g_mhmap_window.color_s_rgba = IM_COL32 (   0, 255, 255, 255 );  /* cyan (V5) */
    g_mhmap_window.show_s_category      = true;
    g_mhmap_window.initialized          = true;
}


extern "C" void mhmap_window_register_persistence ( void *cmod_void )
{
    if ( !cmod_void ) return;

    /* Defaults musí být v g_mhmap_window před voláním cfgelement_set_handlers,
     * aby propagace z .ini přepsala správné výchozí stavy (a ne BSS nuly,
     * které by zůstaly pokud .ini klíč chybí - cfgmodule pro každý element
     * propaguje default registered při volání register_new_element). */
    mhmap_window_state_init ( );

    st_CFGMODULE *cmod = (st_CFGMODULE *)cmod_void;
    st_CFGELEMENT *elm;

    /* Color mode (KEYWORD: RGB / R / W / X / S [V5]). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_color_mode",
                                           CFGENTYPE_KEYWORD, (int)MHMAP_WINDOW_COLOR_RGB,
                                           (int)MHMAP_WINDOW_COLOR_RGB, "RGB",
                                           (int)MHMAP_WINDOW_COLOR_R,   "R",
                                           (int)MHMAP_WINDOW_COLOR_W,   "W",
                                           (int)MHMAP_WINDOW_COLOR_X,   "X",
                                           (int)MHMAP_WINDOW_COLOR_S,   "S",
                                           -1 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.color_mode, (void *)&g_mhmap_window.color_mode );

    /* Log scale (BOOL). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_log_scale", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.log_scale, (void *)&g_mhmap_window.log_scale );

    /* Threshold (UNSIGNED). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_threshold",
                                           CFGENTYPE_UNSIGNED, 0, 0, 0xffffffff );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.threshold, (void *)&g_mhmap_window.threshold );

    /* Zoom (UNSIGNED, range 1-8). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_zoom",
                                           CFGENTYPE_UNSIGNED, 2, 1, 8 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.zoom, (void *)&g_mhmap_window.zoom );

    /* Side panel visible (BOOL). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_side_panel_visible", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.side_panel_visible, (void *)&g_mhmap_window.side_panel_visible );

    /* Selected region index (UNSIGNED).
     * Pozn.: plán doporučoval TEXT s region name proti přeházení enum pořadí.
     * Pro KISS používáme idx; pokud uživatel přepne architekturu nebo se změní
     * region table, idx se za běhu klampne na rozsah dostupných regionů. */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_selected_region_idx",
                                           CFGENTYPE_UNSIGNED, 0, 0, 99 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.selected_region_idx, (void *)&g_mhmap_window.selected_region_idx );

    /* Region visibility bitmask (UNSIGNED, 32-bit). Default 0xFFFFFFFF (vše
     * viditelné). Bit i = region i v export tabulce je viditelný jako tab. */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_region_visibility_mask",
                                           CFGENTYPE_UNSIGNED, 0xFFFFFFFFu, 0, 0xFFFFFFFFu );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.region_visibility_mask, (void *)&g_mhmap_window.region_visibility_mask );

    /* Custom barvy pro RGB color mode (UNSIGNED, RGBA). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_color_r_rgba",
                                           CFGENTYPE_UNSIGNED, IM_COL32 ( 0, 0, 255, 255 ), 0, 0xFFFFFFFFu );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.color_r_rgba, (void *)&g_mhmap_window.color_r_rgba );

    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_color_w_rgba",
                                           CFGENTYPE_UNSIGNED, IM_COL32 ( 255, 0, 0, 255 ), 0, 0xFFFFFFFFu );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.color_w_rgba, (void *)&g_mhmap_window.color_w_rgba );

    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_color_x_rgba",
                                           CFGENTYPE_UNSIGNED, IM_COL32 ( 0, 255, 0, 255 ), 0, 0xFFFFFFFFu );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.color_x_rgba, (void *)&g_mhmap_window.color_x_rgba );

    /* V5: color pro S kategorii (default cyan). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_color_s_rgba",
                                           CFGENTYPE_UNSIGNED, IM_COL32 ( 0, 255, 255, 255 ), 0, 0xFFFFFFFFu );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.color_s_rgba, (void *)&g_mhmap_window.color_s_rgba );

    /* V5: toggle Show S kategorie v heatmap (default ON). */
    elm = cfgmodule_register_new_element ( cmod, (char *)"mhwindow_show_s_category",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void *)&g_mhmap_window.show_s_category, (void *)&g_mhmap_window.show_s_category );
}


/* ========================================================================= */
/*  Import (Phase F)                                                         */
/* ========================================================================= */


/**
 * @brief Vrátit pointer na cell array regionu uvnitř libovolné instance st_MHMAP.
 *
 * Live i imported buffer mají stejný layout (oba jsou st_MHMAP). Region
 * popis dává @c buffer = &g_mhmap.<field>; offset od základu g_mhmap je
 * stejný jako offset stejného pole v import_data. Funkce přepočítá offset
 * a vrátí příslušný pointer v cílovém bufferu.
 *
 * @param region Region popis z @c mhmap_get_export_regions.
 * @param base   Základ st_MHMAP instance (typicky @c &g_mhmap nebo @c import_data).
 * @return Pointer na první cell regionu v daném bufferu.
 */
static const st_MHMAP_CELL *cells_in_buffer ( const st_MHMAP_EXPORT_REGION *region, const void *base )
{
    if ( !region || !base ) return NULL;
    ptrdiff_t off = (const uint8_t *)region->buffer - (const uint8_t *)&g_mhmap;
    return (const st_MHMAP_CELL *)( (const uint8_t *)base + off );
}


/**
 * @brief Vrátit cell array podle aktuálního @c data_source.
 *
 * V Live módu vrací přímo @c region->buffer. V Imported módu vrátí
 * příslušnou oblast v @c g_mhmap_window.import_data.
 */
static const st_MHMAP_CELL *cells_active ( const st_MHMAP_EXPORT_REGION *region )
{
    if ( !region ) return NULL;
    if ( g_mhmap_window.data_source == MHMAP_WINDOW_DATA_IMPORTED && g_mhmap_window.have_import )
    {
        return cells_in_buffer ( region, g_mhmap_window.import_data );
    }
    return (const st_MHMAP_CELL *)region->buffer;
}


/**
 * @brief Volně tolerantní validace meta.json.
 *
 * Hledá v souboru "format_version": 2 a "mzarch": @p expected_arch.
 * Pravý JSON parser není potřeba - formát je velmi přísný (vytváří jej
 * mhmap_export sám). Pokud se formát změní (format_version=3), musí se
 * tato validace updateovat.
 *
 * @return 0 = OK, -1 = chyba (s detailním hlášením do @c s_import_status).
 */
/**
 * @brief Pomocný extraktor JSON string hodnoty z bufferu - "key": "value".
 *
 * Najde @p key v bufferu, přeskočí mezery a uvozovku, zkopíruje hodnotu
 * až do uzavírací uvozovky. Bezpečné jen pro ASCII hodnoty bez escape.
 *
 * @param buf  null-terminated buffer s meta.json
 * @param key  pattern hledaný key (např. "\"created_at\":")
 * @param out  výstupní buffer
 * @param outsz velikost @p out
 * @return @c true pokud byla hodnota nalezena.
 */
static bool extract_json_string ( const char *buf, const char *key, char *out, size_t outsz )
{
    if ( !buf || !key || !out || outsz < 2 ) return false;
    const char *p = strstr ( buf, key );
    if ( !p ) return false;
    p += strlen ( key );
    while ( *p && ( *p == ' ' || *p == '\t' ) ) p++;
    if ( *p != '"' ) return false;
    p++;
    size_t i = 0;
    while ( *p && *p != '"' && i + 1 < outsz )
    {
        out[ i++ ] = *p++;
    }
    out[ i ] = 0;
    return ( i > 0 );
}


/* Naparsovaná metadata pro UI feedback (zachycena při validaci). */
static int  s_import_meta_arch    = 0;
static char s_import_meta_created[ 32 ] = { 0 };

/* Maximum entries v regions[] - bezpečně pokrývá všechny architektury. */
#define MHMAP_IMPORT_MAX_REGIONS  40

/**
 * @brief Jeden záznam regionu načtený z meta.json pro import.
 */
struct st_IMPORT_REGION_ENTRY
{
    char name[ 64 ];      /**< symbolický název (matchuje regions[].name) */
    char file[ 192 ];     /**< relativní cesta k souboru (z regions[].file) */
};

/* Naparsované regiony z meta.json. */
static struct st_IMPORT_REGION_ENTRY s_import_regions[ MHMAP_IMPORT_MAX_REGIONS ];
static size_t s_import_regions_count = 0;


/**
 * @brief Pomocný extraktor JSON string hodnoty s rozsahem.
 *
 * Stejné chování jako @ref extract_json_string, ale hledá jen v rozsahu
 * @p start .. @p end. Vhodné pro parsování per-region bloků.
 */
static bool extract_json_string_range ( const char *start, const char *end,
                                        const char *key, char *out, size_t outsz )
{
    if ( !start || !end || !key || !out || outsz < 2 || end <= start ) return false;
    size_t klen = strlen ( key );
    const char *p = start;
    while ( p < end - klen )
    {
        if ( strncmp ( p, key, klen ) == 0 )
        {
            const char *q = p + klen;
            while ( q < end && ( *q == ' ' || *q == '\t' ) ) q++;
            if ( q < end && *q == '"' )
            {
                q++;
                size_t i = 0;
                while ( q < end && *q != '"' && i + 1 < outsz )
                {
                    out[ i++ ] = *q++;
                }
                out[ i ] = 0;
                return ( i > 0 );
            }
            return false;
        }
        p++;
    }
    return false;
}


/**
 * @brief Parsovat regions[] pole z @p buf do @ref s_import_regions.
 *
 * Najde "regions": [ ... ], pak iteruje vnořené { ... } bloky a pro každý
 * extrahuje "name" a "file". Lehký scanner - nepoužívá pravý JSON parser.
 *
 * @return Počet naparsovaných záznamů.
 */
static size_t parse_regions_from_meta ( const char *buf )
{
    s_import_regions_count = 0;
    if ( !buf ) return 0;

    const char *regs = strstr ( buf, "\"regions\":" );
    if ( !regs ) return 0;

    const char *p = strchr ( regs, '[' );
    if ( !p ) return 0;
    const char *array_end = strchr ( p, ']' );
    if ( !array_end ) array_end = buf + strlen ( buf );

    while ( p < array_end && s_import_regions_count < MHMAP_IMPORT_MAX_REGIONS )
    {
        const char *block_start = strchr ( p, '{' );
        if ( !block_start || block_start >= array_end ) break;
        const char *block_end = strchr ( block_start, '}' );
        if ( !block_end || block_end > array_end ) break;

        struct st_IMPORT_REGION_ENTRY *e = &s_import_regions[ s_import_regions_count ];
        e->name[ 0 ] = 0;
        e->file[ 0 ] = 0;

        bool ok_name = extract_json_string_range ( block_start, block_end,
                                                   "\"name\":", e->name, sizeof ( e->name ) );
        bool ok_file = extract_json_string_range ( block_start, block_end,
                                                   "\"file\":", e->file, sizeof ( e->file ) );
        if ( ok_name && ok_file )
        {
            s_import_regions_count++;
        };

        p = block_end + 1;
    }

    return s_import_regions_count;
}


/**
 * @brief Validovat meta.json soubor v zadané cestě.
 *
 * @param meta_path Plná cesta k @c meta.json souboru.
 * @param expected_arch Očekávaný @c mzarch (musí matchovat).
 */
static int meta_json_validate ( const char *meta_path, int expected_arch )
{
    s_import_meta_arch = 0;
    s_import_meta_created[ 0 ] = 0;

    FILE *f = fopen ( meta_path, "rb" );
    if ( !f )
    {
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "Cannot open '%s'", meta_path );
        s_import_status_is_error = true;
        return -1;
    };
    fseek ( f, 0, SEEK_END );
    long sz = ftell ( f );
    fseek ( f, 0, SEEK_SET );
    if ( sz <= 0 || sz > 100000 )
    {
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "Suspicious meta.json size: %ld", sz );
        s_import_status_is_error = true;
        fclose ( f );
        return -1;
    };
    char *buf = (char *)malloc ( (size_t)sz + 1 );
    if ( !buf )
    {
        snprintf ( s_import_status, sizeof ( s_import_status ), "OOM reading meta.json" );
        s_import_status_is_error = true;
        fclose ( f );
        return -1;
    };
    size_t n = fread ( buf, 1, (size_t)sz, f );
    buf[ n ] = 0;
    fclose ( f );

    /* Extrakce timestampu (volitelné - může v starších exportech chybět). */
    extract_json_string ( buf, "\"created_at\":", s_import_meta_created, sizeof ( s_import_meta_created ) );

    /* Naparsovat regions[] - každý záznam má name + file. Tento seznam pak
     * řídí načítání souborů (žádné odhady názvů z prefixu). */
    parse_regions_from_meta ( buf );

    int ok = 0;
    if ( strstr ( buf, "\"format_version\": 2" ) )
    {
        /* Naparsovat mzarch hodnotu z "mzarch": <number>. */
        const char *p_arch = strstr ( buf, "\"mzarch\":" );
        if ( p_arch )
        {
            p_arch += strlen ( "\"mzarch\":" );
            while ( *p_arch == ' ' || *p_arch == '\t' ) p_arch++;
            s_import_meta_arch = atoi ( p_arch );
        };

        if ( s_import_meta_arch == expected_arch )
        {
            ok = 1;
        }
        else
        {
            snprintf ( s_import_status, sizeof ( s_import_status ),
                       "Architecture mismatch (file=%d, expected=%d)",
                       s_import_meta_arch, expected_arch );
            s_import_status_is_error = true;
        };
    }
    else
    {
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "Unsupported format_version (need 2)" );
        s_import_status_is_error = true;
    };

    free ( buf );
    return ok ? 0 : -1;
}


/**
 * @brief Importovat CDL adresář do @c g_mhmap_window.import_data.
 *
 * Naalokuje (nebo zrecykluje) buffer @c import_data velikosti @c sizeof(st_MHMAP),
 * vynuluje ho, validuje meta.json proti aktuální architektuře a načte všechny
 * region soubory do bufferu. Pokud chybí jednotlivý region soubor, je tolerován
 * (zůstane vynulovaný), ale v status hlášení se zaznamená warning.
 *
 * Po úspěchu nastaví @c have_import = true a přepne @c data_source na IMPORTED.
 *
 * @param meta_path Cesta k @c meta.json souboru. Region soubory jsou hledány
 *                  v jeho parent adresáři.
 * @return 0 při úspěchu, -1 při fatální chybě (nelze validovat meta).
 */
static int mhmap_window_import ( const char *meta_path )
{
    s_import_status[ 0 ] = 0;
    s_import_status_is_error = false;

    if ( meta_json_validate ( meta_path, MZARCH ) != 0 )
    {
        return -1;
    };

    /* Region soubory leží v parent adresáři meta.json. Konkrétní názvy
     * souborů jsou v @c s_import_regions (naparsované z regions[].file). */
    char *dir = g_path_get_dirname ( meta_path );

    if ( !g_mhmap_window.import_data )
    {
        g_mhmap_window.import_data = malloc ( sizeof ( st_MHMAP ) );
        if ( !g_mhmap_window.import_data )
        {
            snprintf ( s_import_status, sizeof ( s_import_status ),
                       "OOM (need %zu bytes)", sizeof ( st_MHMAP ) );
            s_import_status_is_error = true;
            g_free ( dir );
            return -1;
        };
    };
    memset ( g_mhmap_window.import_data, 0, sizeof ( st_MHMAP ) );

    size_t count = 0;
    const st_MHMAP_EXPORT_REGION *regions = mhmap_get_export_regions ( &count );

    unsigned loaded = 0;
    unsigned skipped = 0;
    unsigned unknown = 0;

    /* Iterujeme přes regiony z JSON (autoritativní zdroj jmen souborů).
     * Pro každý matchneme jméno proti aktuální build region tabulce -
     * to dá pointer na cell array a velikost. */
    for ( size_t k = 0; k < s_import_regions_count; k++ )
    {
        const struct st_IMPORT_REGION_ENTRY *je = &s_import_regions[ k ];

        /* Najdi region s matching name v build tabulce. */
        const st_MHMAP_EXPORT_REGION *match = NULL;
        for ( size_t i = 0; i < count; i++ )
        {
            if ( strcmp ( regions[ i ].name, je->name ) == 0 )
            {
                match = &regions[ i ];
                break;
            };
        };

        if ( !match )
        {
            /* Region z JSON nemá protějšek v current build (např. import
             * ze starší verze emulátoru s odlišnou region tabulkou). */
            unknown++;
            continue;
        };

        char *fp = g_build_filename ( dir, je->file, NULL );
        FILE *f = fopen ( fp, "rb" );
        if ( !f )
        {
            skipped++;
            g_free ( fp );
            continue;
        };

        ptrdiff_t off = (const uint8_t *)match->buffer - (const uint8_t *)&g_mhmap;
        uint8_t *dst = (uint8_t *)g_mhmap_window.import_data + off;
        size_t n = fread ( dst, 1, match->size_bytes, f );
        fclose ( f );

        if ( n == match->size_bytes ) loaded++;
        else                          skipped++;

        g_free ( fp );
    };

    g_free ( dir );

    g_mhmap_window.have_import        = ( loaded > 0 );
    g_mhmap_window.selected_cell_offset = -1;
    if ( g_mhmap_window.have_import )
    {
        g_mhmap_window.data_source = MHMAP_WINDOW_DATA_IMPORTED;
        const char *ts = s_import_meta_created[ 0 ] ? s_import_meta_created : "(no timestamp)";
        char extra[ 96 ] = { 0 };
        if ( skipped > 0 || unknown > 0 )
        {
            snprintf ( extra, sizeof ( extra ), " (%u missing, %u unknown)",
                       skipped, unknown );
        };
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "Imported %u/%zu regions, mzarch=%d, created=%s%s",
                   loaded, s_import_regions_count, s_import_meta_arch, ts, extra );
        s_import_status_is_error = ( skipped > 0 || unknown > 0 );
        return 0;
    };

    if ( s_import_regions_count == 0 )
    {
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "meta.json regions[] is empty or unparseable" );
    }
    else
    {
        snprintf ( s_import_status, sizeof ( s_import_status ),
                   "No region files loaded (all %zu missing or unknown)",
                   s_import_regions_count );
    };
    s_import_status_is_error = true;
    return -1;
}


/**
 * @brief Otevřít file dialog pro výběr meta.json souboru CDL exportu.
 *
 * Filtr na *.json (typicky meta.json). Region soubory jsou hledány v parent
 * adresáři vybraného souboru. Default cesta je @c g_debugger.cdl_export_dir.
 */
static void open_import_dialog ( void )
{
    s_import_dialog_open = true;
    IGFD::FileDialogConfig config;
    config.path = ( g_debugger.cdl_export_dir && g_debugger.cdl_export_dir[ 0 ] )
                  ? g_debugger.cdl_export_dir : ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton;
    /* Filtr na JSON soubory; uživatel může vybrat libovolný .json (typicky
     * meta.json), validace pak ověří format_version + mzarch. */
    ImGuiFileDialog::Instance ( )->OpenDialog ( "MhmapImportDir",
                                                _( "Select CDL meta.json file" ),
                                                ".json", config );
}


/**
 * @brief Render dialog pro výběr importního adresáře.
 */
static void render_import_dialog ( void )
{
    if ( !s_import_dialog_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "MhmapImportDir" ) )
    {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) )
        {
            /* GetFilePathName vrátí plnou cestu k vybranému souboru
             * (typicky .../meta.json). Region soubory jsou hledány
             * v jeho parent adresáři. */
            std::string file_path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            mhmap_window_import ( file_path.c_str ( ) );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
        s_import_dialog_open = false;
    };
}


/* ========================================================================= */
/*  Top control bar                                                          */
/* ========================================================================= */


/**
 * @brief Render Mode radio (Off / Window / Always).
 *
 * Klik volá @c dbg_ui_mhmap_set_mode: zapíše @c g_debugger.mhmap_mode
 * a swap CPU callbacků deleguje na emu vlákno (RECOMPUTE přes frontu dbgapi).
 */
static void render_mode_radio ( void )
{
    int mode = (int)g_debugger.mhmap_mode;
    ImGui::TextUnformatted ( _( "Mode:" ) );
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "Off##mhmap_mode" ), &mode, (int)DEBUGGER_MHMAP_MODE_OFF ) )
    {
        dbg_ui_mhmap_set_mode ( DEBUGGER_MHMAP_MODE_OFF );
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
                            _( "Do not record. Hot path stays bit-identical with vanilla emulator." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "With Window##mhmap_mode" ), &mode, (int)DEBUGGER_MHMAP_MODE_WITH_WINDOW ) )
    {
        dbg_ui_mhmap_set_mode ( DEBUGGER_MHMAP_MODE_WITH_WINDOW );
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
                            _( "Record only when the main debugger window is open\n"
                               "(NOT this Memory Heatmap window)." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "Always##mhmap_mode" ), &mode, (int)DEBUGGER_MHMAP_MODE_ALWAYS ) )
    {
        dbg_ui_mhmap_set_mode ( DEBUGGER_MHMAP_MODE_ALWAYS );
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s",
                            _( "Always record while the emulator is running\n"
                               "(independent of any debugger window)." ) );
    };
}


/**
 * @brief Otevřít file dialog pro výběr meta.json souboru pro export.
 *
 * Default jméno se sestaví z @c cdl_export_name + ".json", default cesta
 * z @c cdl_export_dir. Po výběru se zavolá @c mhmap_export s plnou cestou.
 */
static void open_export_dialog ( void )
{
    s_export_dialog_open = true;
    IGFD::FileDialogConfig config;
    config.path = ( g_debugger.cdl_export_dir && g_debugger.cdl_export_dir[ 0 ] )
                  ? g_debugger.cdl_export_dir : ".";

    /* Default filename = cdl_export_name + ".json". Pokud už cdl_export_name
     * končí .json (uživatel dal např. "mzdos.json" jako --cdl-name), nepřidávat
     * další extenzi - jinak by dialog ukázal "mzdos.json.json". */
    static char default_fname[ 256 ];
    if ( g_debugger.cdl_export_name && g_debugger.cdl_export_name[ 0 ] )
    {
        const char *nm = g_debugger.cdl_export_name;
        size_t nlen = strlen ( nm );
        bool has_ext = ( nlen >= 5 &&
                         g_ascii_strcasecmp ( nm + nlen - 5, ".json" ) == 0 );
        if ( has_ext )
        {
            snprintf ( default_fname, sizeof ( default_fname ), "%s", nm );
        }
        else
        {
            snprintf ( default_fname, sizeof ( default_fname ), "%s.json", nm );
        };
    }
    else
    {
        snprintf ( default_fname, sizeof ( default_fname ), "cdl-export.json" );
    };
    config.fileName = default_fname;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton |
                   ImGuiFileDialogFlags_ConfirmOverwrite;
    ImGuiFileDialog::Instance ( )->OpenDialog ( "MhmapExportDir",
                                                _( "Save CDL meta.json file" ),
                                                ".json", config );
}


/**
 * @brief Render dialog pro výběr exportního souboru.
 *
 * Dialogy musí být renderovány na top-levelu okna, ne uvnitř menu, aby
 * pokračovaly napříč framy. Po výběru se sestaví plná cesta a spustí
 * @c mhmap_export. Cesta a basename se uloží do @c g_debugger pro
 * persistenci.
 */
static void render_export_dialog ( void )
{
    if ( !s_export_dialog_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "MhmapExportDir" ) )
    {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) )
        {
            std::string file_path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            mhmap_export ( file_path.c_str ( ) );

            /* Aktualizovat persistované hodnoty pro příští dialog. */
            char *dir  = g_path_get_dirname  ( file_path.c_str ( ) );
            char *base = g_path_get_basename ( file_path.c_str ( ) );
            char *dot  = strrchr ( base, '.' );
            if ( dot && g_ascii_strcasecmp ( dot, ".json" ) == 0 ) *dot = 0;

            /* dir + base alokujeme přes malloc protože cfgmodule dělá free. */
            char *new_dir = (char *)malloc ( strlen ( dir ) + 1 );
            if ( new_dir )
            {
                strcpy ( new_dir, dir );
                free ( g_debugger.cdl_export_dir );
                g_debugger.cdl_export_dir = new_dir;
            };
            char *new_name = (char *)malloc ( strlen ( base ) + 1 );
            if ( new_name )
            {
                strcpy ( new_name, base );
                free ( g_debugger.cdl_export_name );
                g_debugger.cdl_export_name = new_name;
            };
            g_free ( dir );
            g_free ( base );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
        s_export_dialog_open = false;
    };
}


/**
 * @brief Render top control bar.
 *
 * Layout: Mode radio | [ ] Export on Exit | [Reset] [Export...] [Import...]
 * Pod tím status řádek s informací o aktuálním režimu a aktivitě recordingu.
 */
static void render_top_bar ( void )
{
    render_mode_radio ( );

    ImGui::SameLine ( );
    ImGui::Spacing ( );
    ImGui::SameLine ( );

    bool export_on_exit = ( g_debugger.cdl_export_on_exit != 0 );
    if ( ImGui::Checkbox ( _L( "Export on Exit##mhmap_top" ), &export_on_exit ) )
    {
        g_debugger.cdl_export_on_exit = export_on_exit ? 1 : 0;
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Reset##mhmap_top" ) ) )
    {
        /* Nulování counterů vykoná emu vlákno (DBGAPI_CMD_CDL_RESET). */
        dbg_ui_mhmap_reset ( );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Export...##mhmap_top" ) ) )
    {
        open_export_dialog ( );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Import...##mhmap_top" ) ) )
    {
        open_import_dialog ( );
    };

    /* Side panel toggle - zarovnat na úplný pravý okraj content area.
     * CalcTextSize s hide_text_after_double_hash=true nepočítá ##suffix do šířky.
     * GetContentRegionMax().x dá pravý okraj v cursor coords; tam minus
     * šířka tlačítka = SetCursorPosX. */
    ImGui::SameLine ( );
    {
        const char *btn_label = g_mhmap_window.side_panel_visible
                                ? _L( "Hide panel##mhmap_top" )
                                : _L( "Show panel##mhmap_top" );
        ImVec2 lbl_size = ImGui::CalcTextSize ( btn_label, NULL, true );
        float btn_w = lbl_size.x + ImGui::GetStyle ( ).FramePadding.x * 2.0f;
        float right_x = ImGui::GetWindowContentRegionMax ( ).x;
        float pos_x = right_x - btn_w;
        if ( pos_x > ImGui::GetCursorPosX ( ) )
        {
            ImGui::SetCursorPosX ( pos_x );
        };
        if ( ImGui::Button ( btn_label ) )
        {
            g_mhmap_window.side_panel_visible = !g_mhmap_window.side_panel_visible;
        };
    }

    /* Status řádek - co se právě děje. Text přes _() (i18n); hodnota
     * mhmap_mode je technický identifikátor režimu, nepřekládá se. */
    const char *mode_str = "OFF";
    switch ( g_debugger.mhmap_mode )
    {
        case DEBUGGER_MHMAP_MODE_OFF:         mode_str = "OFF";         break;
        case DEBUGGER_MHMAP_MODE_WITH_WINDOW: mode_str = "WITH_WINDOW"; break;
        case DEBUGGER_MHMAP_MODE_ALWAYS:      mode_str = "ALWAYS";      break;
    }

    if ( TEST_DEBUGGER_MHMAP_ACTIVE )
    {
        ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ),
                             _( "Status: Recording active (mhmap_mode=%s)" ), mode_str );
    }
    else
    {
        ImGui::TextDisabled ( _( "Status: Recording inactive (mhmap_mode=%s)" ), mode_str );
    };

    /* Import status (poslední pokus o import) - barevný feedback. */
    if ( s_import_status[ 0 ] )
    {
        if ( s_import_status_is_error )
        {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "Import: %s", s_import_status );
        }
        else
        {
            ImGui::TextColored ( ImVec4 ( 0.4f, 1.0f, 0.4f, 1.0f ), "Import: %s", s_import_status );
        };
    };
}


/* ========================================================================= */
/*  Side panel                                                               */
/* ========================================================================= */


/**
 * @brief Vrátit v procentech podíl @p v / @p max (0 .. 100).
 *
 * Používá se v side panelu pro zobrazení podílu counter / region max.
 */
static float percent_of ( uint32_t v, uint32_t max_v )
{
    if ( max_v == 0 ) return 0.0f;
    return ( (float)v / (float)max_v ) * 100.0f;
}


/**
 * @brief Pro region @c "bus" vrátí addr = offset, jinak vrátí @c -1.
 *
 * Detekce přes název regionu (offset v "bus" odpovídá CPU adrese 1:1).
 * Pro ostatní regiony je offset specifický (rom-cg má offset v 4 KB ROM
 * imagu, ne CPU adresa) a smysluplnou bus adresu nelze odvodit.
 */
static int try_get_bus_addr ( const st_MHMAP_EXPORT_REGION *region, int offset )
{
    if ( !region || offset < 0 ) return -1;
    if ( strcmp ( region->name, "bus" ) == 0 ) return offset;
    return -1;
}


/**
 * @brief Render side panel s detailem selection a region statistikami.
 *
 * Volá se uvnitř BeginChild dedikovaného pro panel. Layout:
 *  - Hlavička "Selected cell" + addr/region/offset/R/W/X
 *  - Hlavička "Region statistics" + active_cells/totals/max
 *  - Tlačítko "Reset region only"
 *
 * Mutace živých counterů (Add / Sub importovaných dat, Reset region only)
 * vykoná emu vlákno přes frontu dbgapi (dbg_ui_mhmap_*); UI je jen odešle.
 *
 * @param region       Popis zobrazeného regionu z @c mhmap_get_export_regions.
 * @param region_index Index @p region v téže tabulce (pro Reset region only).
 * @param stats        Statistiky regionu spočtené pro aktuální snímek.
 */
static void render_side_panel ( const st_MHMAP_EXPORT_REGION *region,
                                size_t region_index,
                                const st_MHMAP_REGION_STATS *stats )
{
    if ( !region || !stats ) return;

    /* Show: Live / Imported toggle (jen pokud máme načtená imported data). */
    ImGui::SeparatorText ( _( "Show" ) );
    int ds = (int)g_mhmap_window.data_source;
    if ( ImGui::RadioButton ( _L( "Live##mhmap_show" ), &ds, (int)MHMAP_WINDOW_DATA_LIVE ) )
    {
        g_mhmap_window.data_source = MHMAP_WINDOW_DATA_LIVE;
    };
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( !g_mhmap_window.have_import );
    if ( ImGui::RadioButton ( _L( "Imported##mhmap_show" ), &ds, (int)MHMAP_WINDOW_DATA_IMPORTED ) )
    {
        g_mhmap_window.data_source = MHMAP_WINDOW_DATA_IMPORTED;
    };
    ImGui::EndDisabled ( );
    if ( !g_mhmap_window.have_import )
    {
        ImGui::TextDisabled ( "%s", _( "(no import loaded)" ) );
    }
    else
    {
        /* Operace nad live counter pomocí imported snapshot (item 3, 4).
         * Tlačítka jsou krátká (Drop / Add / Sub), úplný popis ve tooltipu. */
        ImGui::Spacing ( );
        if ( ImGui::Button ( _L( "Drop##mhmap_imp" ) ) )
        {
            free ( g_mhmap_window.import_data );
            g_mhmap_window.import_data = NULL;
            g_mhmap_window.have_import = false;
            g_mhmap_window.data_source = MHMAP_WINDOW_DATA_LIVE;
            s_import_status[ 0 ] = 0;
        };
        if ( ImGui::IsItemHovered ( ) )
        {
            ImGui::SetTooltip ( "%s", _( "Drop imported data (free buffer, switch back to Live)" ) );
        };

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L( "Add##mhmap_imp" ) ) )
        {
            /* Per-cell live += imported se saturací na UINT32_MAX; vykoná
             * emu vlákno (mhmap_merge), import_data čte jen do návratu. */
            dbg_ui_mhmap_merge ( (const st_MHMAP *)g_mhmap_window.import_data, MHMAP_MERGE_ADD );
        };
        if ( ImGui::IsItemHovered ( ) )
        {
            ImGui::SetTooltip ( "%s", _( "Add imported counters to Live (saturates at UINT32_MAX)" ) );
        };

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L( "Sub##mhmap_imp" ) ) )
        {
            /* Per-cell live -= imported s ořezem na 0; vykoná emu vlákno
             * (mhmap_merge), import_data čte jen do návratu. */
            dbg_ui_mhmap_merge ( (const st_MHMAP *)g_mhmap_window.import_data, MHMAP_MERGE_SUB );
        };
        if ( ImGui::IsItemHovered ( ) )
        {
            ImGui::SetTooltip ( "%s", _( "Subtract imported counters from Live (clamps at 0)" ) );
        };
    };

    ImGui::SeparatorText ( _( "Selected cell" ) );

    if ( g_mhmap_window.selected_cell_offset < 0 )
    {
        ImGui::TextDisabled ( "%s", _( "(none - click a cell)" ) );
    }
    else
    {
        int sel = g_mhmap_window.selected_cell_offset;
        if ( sel >= (int)region->size_cells )
        {
            ImGui::TextDisabled ( "%s", _( "(out of range)" ) );
        }
        else
        {
            const st_MHMAP_CELL *cells = cells_active ( region );
            const st_MHMAP_CELL &c = cells[ sel ];

            int bus_addr = try_get_bus_addr ( region, sel );
            if ( bus_addr >= 0 )
            {
                ImGui::Text ( "Addr:    0x%04X", bus_addr );
            }
            else
            {
                ImGui::TextDisabled ( "Addr:    (n/a)" );
            };
            ImGui::Text     ( "Region:  %s",         region->name );
            ImGui::Text     ( "Offset:  0x%04X (%d)", sel, sel );
            ImGui::Spacing ( );
            ImGui::Text     ( "R = %u    (%.2f%% of max)", c.r, percent_of ( c.r, stats->max_r ) );
            ImGui::Text     ( "W = %u    (%.2f%%)",         c.w, percent_of ( c.w, stats->max_w ) );
            ImGui::Text     ( "X = %u    (%.2f%%)",         c.x, percent_of ( c.x, stats->max_x ) );
            ImGui::Text     ( "S = %u    (%.2f%%)",         c.s, percent_of ( c.s, stats->max_s ) );
        };
    };

    ImGui::SeparatorText ( _( "Region statistics" ) );

    float active_pct = 0.0f;
    if ( region->size_cells > 0 )
    {
        active_pct = ( (float)stats->active_cells / (float)region->size_cells ) * 100.0f;
    };

    ImGui::Text ( "Active cells:  %u / %u (%.1f%%)",
                  stats->active_cells, region->size_cells, active_pct );
    ImGui::Spacing ( );
    ImGui::Text ( "Total R:  %llu", (unsigned long long)stats->total_r );
    ImGui::Text ( "Total W:  %llu", (unsigned long long)stats->total_w );
    ImGui::Text ( "Total X:  %llu", (unsigned long long)stats->total_x );
    ImGui::Text ( "Total S:  %llu", (unsigned long long)stats->total_s );
    ImGui::Spacing ( );
    ImGui::Text ( "Max R:    %u", stats->max_r );
    ImGui::Text ( "Max W:    %u", stats->max_w );
    ImGui::Text ( "Max X:    %u", stats->max_x );
    ImGui::Text ( "Max S:    %u", stats->max_s );

    ImGui::Spacing ( );
    ImGui::Separator ( );

    /* Reset region operuje vždy nad live bufferem (region->buffer); v Imported
     * módu nemá smysl (importovaná data jsou snapshot, nemodifikujeme je). */
    bool can_reset = ( g_mhmap_window.data_source == MHMAP_WINDOW_DATA_LIVE );
    ImGui::BeginDisabled ( !can_reset );
    if ( ImGui::Button ( _L( "Reset region only##mhmap_side" ) ) )
    {
        /* Nulování regionu vykoná emu vlákno; výběr se ruší jen po úspěchu. */
        if ( dbg_ui_mhmap_reset_region ( (uint32_t)region_index ) )
        {
            g_mhmap_window.selected_cell_offset = -1;
        };
    };
    ImGui::EndDisabled ( );
}


/* ========================================================================= */
/*  Filter bar                                                               */
/* ========================================================================= */


/**
 * @brief Render filter bar pro aktivní region.
 *
 * Layout: Color: [RGB] [R] [W] [X]   Scale: [Linear] [Log]   Threshold: [N]   Zoom: [N×]
 *
 * Volá se uvnitř každé tab, mezi statistikou a grid renderem. Změny se aplikují
 * okamžitě (live - každý frame se grid překreslí podle aktuálních hodnot).
 */
/**
 * @brief Pomocná konverze IM_COL32 (uint) → ImVec4 a zpět pro ColorEdit3.
 *
 * IM_COL32 ukládá AABBGGRR v little-endian. ImGui::ColorConvertU32ToFloat4
 * správně rozseká.
 */
static void render_color_settings_popup ( void )
{
    if ( !ImGui::BeginPopup ( "mhmap_color_popup" ) ) return;

    ImGui::TextDisabled ( "%s", _( "RGB mode color mapping" ) );
    ImGui::Separator ( );

    /* R counter color. */
    {
        ImVec4 col = ImGui::ColorConvertU32ToFloat4 ( g_mhmap_window.color_r_rgba );
        if ( ImGui::ColorEdit3 ( _L( "R counter##mhmap_col" ), &col.x,
                                 ImGuiColorEditFlags_NoInputs ) )
        {
            g_mhmap_window.color_r_rgba = ImGui::ColorConvertFloat4ToU32 ( col );
        };
    }
    /* W counter color. */
    {
        ImVec4 col = ImGui::ColorConvertU32ToFloat4 ( g_mhmap_window.color_w_rgba );
        if ( ImGui::ColorEdit3 ( _L( "W counter##mhmap_col" ), &col.x,
                                 ImGuiColorEditFlags_NoInputs ) )
        {
            g_mhmap_window.color_w_rgba = ImGui::ColorConvertFloat4ToU32 ( col );
        };
    }
    /* X counter color. */
    {
        ImVec4 col = ImGui::ColorConvertU32ToFloat4 ( g_mhmap_window.color_x_rgba );
        if ( ImGui::ColorEdit3 ( _L( "X counter##mhmap_col" ), &col.x,
                                 ImGuiColorEditFlags_NoInputs ) )
        {
            g_mhmap_window.color_x_rgba = ImGui::ColorConvertFloat4ToU32 ( col );
        };
    }
    /* V5: S counter color (stack write). */
    {
        ImVec4 col = ImGui::ColorConvertU32ToFloat4 ( g_mhmap_window.color_s_rgba );
        if ( ImGui::ColorEdit3 ( _L( "S counter##mhmap_col" ), &col.x,
                                 ImGuiColorEditFlags_NoInputs ) )
        {
            g_mhmap_window.color_s_rgba = ImGui::ColorConvertFloat4ToU32 ( col );
        };
    }

    ImGui::Separator ( );
    if ( ImGui::Button ( _L( "Reset to defaults##mhmap_col" ) ) )
    {
        g_mhmap_window.color_r_rgba = IM_COL32 (   0,   0, 255, 255 );
        g_mhmap_window.color_w_rgba = IM_COL32 ( 255,   0,   0, 255 );
        g_mhmap_window.color_x_rgba = IM_COL32 (   0, 255,   0, 255 );
        g_mhmap_window.color_s_rgba = IM_COL32 (   0, 255, 255, 255 );
    };

    ImGui::EndPopup ( );
}


static void render_filter_bar ( void )
{
    /* "Color:" label jako tlačítko - otevírá popup s nastavením barev. */
    if ( ImGui::Button ( _L( "Color:##mhmap_color_btn" ) ) )
    {
        ImGui::OpenPopup ( "mhmap_color_popup" );
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s", _( "Click to customize R/W/X/S colors used in RGB mode" ) );
    };
    render_color_settings_popup ( );

    /* Color mode (4 radio). */
    int cm = (int)g_mhmap_window.color_mode;
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "RGB##mhmap_color" ), &cm, (int)MHMAP_WINDOW_COLOR_RGB ) )
    {
        g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_RGB;
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "R##mhmap_color" ), &cm, (int)MHMAP_WINDOW_COLOR_R ) )
    {
        g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_R;
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "W##mhmap_color" ), &cm, (int)MHMAP_WINDOW_COLOR_W ) )
    {
        g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_W;
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "X##mhmap_color" ), &cm, (int)MHMAP_WINDOW_COLOR_X ) )
    {
        g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_X;
    };
    /* V5: S radio button (Stack write). Disabled pokud show_s_category = OFF
     * (= nemá smysl jen-S monochrome když kategorie není viditelná). */
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( !g_mhmap_window.show_s_category );
    if ( ImGui::RadioButton ( _L( "S##mhmap_color" ), &cm, (int)MHMAP_WINDOW_COLOR_S ) )
    {
        g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_S;
    };
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s", _( "Stack write counter (V5). Disabled when 'Show S' is off." ) );
    };

    /* V5: Show S category toggle. */
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "  " );
    ImGui::SameLine ( );
    bool show_s = g_mhmap_window.show_s_category;
    if ( ImGui::Checkbox ( _L( "Show S##mhmap_show_s" ), &show_s ) )
    {
        g_mhmap_window.show_s_category = show_s;
        /* Pokud byl aktivní S-only mode a uživatel vypnul Show S,
         * spadnout zpět na RGB (= konzistentní stav). */
        if ( !show_s && g_mhmap_window.color_mode == MHMAP_WINDOW_COLOR_S )
        {
            g_mhmap_window.color_mode = MHMAP_WINDOW_COLOR_RGB;
        }
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s", _( "Toggle visibility of stack write (S) category in RGB blending and as a standalone color mode." ) );
    };

    /* Scale (linear / log). */
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "  " );
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Scale:" ) );
    ImGui::SameLine ( );
    int sc = g_mhmap_window.log_scale ? 1 : 0;
    if ( ImGui::RadioButton ( _L( "Linear##mhmap_scale" ), &sc, 0 ) )
    {
        g_mhmap_window.log_scale = false;
    };
    ImGui::SameLine ( );
    if ( ImGui::RadioButton ( _L( "Log##mhmap_scale" ), &sc, 1 ) )
    {
        g_mhmap_window.log_scale = true;
    };

    /* Threshold (integer input). */
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "  " );
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Threshold:" ) );
    ImGui::SameLine ( );
    int th = (int)g_mhmap_window.threshold;
    ImGui::SetNextItemWidth ( 180.0f );
    if ( ImGui::InputInt ( "##mhmap_threshold", &th, 1, 10 ) )
    {
        if ( th < 0 ) th = 0;
        g_mhmap_window.threshold = (unsigned)th;
    };

    /* Zoom (combo 1×/2×/4×/8×/16×/32×). */
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "  " );
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Zoom:" ) );
    ImGui::SameLine ( );
    static const char *zoom_labels[ ] = { "1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97", "16\xC3\x97", "32\xC3\x97" };  /* UTF-8 × */
    static const unsigned zoom_vals [ ] = { 1, 2, 4, 8, 16, 32 };
    static const int zoom_count = (int)( sizeof ( zoom_vals ) / sizeof ( zoom_vals[ 0 ] ) );
    int z_idx = 0;
    for ( int i = 0; i < zoom_count; i++ )
    {
        if ( zoom_vals[ i ] == g_mhmap_window.zoom ) { z_idx = i; break; };
    }
    ImGui::SetNextItemWidth ( 80.0f );
    if ( ImGui::Combo ( "##mhmap_zoom", &z_idx, zoom_labels, zoom_count ) )
    {
        g_mhmap_window.zoom = zoom_vals[ z_idx ];
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s", _( "Cell pixel size. Hint: Ctrl + mouse wheel over the grid to zoom in/out." ) );
    };
}




/* ========================================================================= */
/*  Region tab bar                                                           */
/* ========================================================================= */


/**
 * @brief Test viditelnosti regionu @p i v @c region_visibility_mask.
 */
static inline bool region_is_visible ( int i )
{
    if ( i < 0 || i >= 32 ) return false;
    return ( g_mhmap_window.region_visibility_mask >> i ) & 1u;
}


/**
 * @brief Nastavit viditelnost regionu @p i v @c region_visibility_mask.
 */
static inline void region_set_visible ( int i, bool v )
{
    if ( i < 0 || i >= 32 ) return;
    if ( v ) g_mhmap_window.region_visibility_mask |=  ( 1u << i );
    else     g_mhmap_window.region_visibility_mask &= ~( 1u << i );
}


/**
 * @brief Helper: aplikovat skupinový výběr přes prefix názvu regionu.
 *
 * Pro každý region nastaví bit ve viditelnosti podle toho, zda jeho
 * @c name začíná zadaným prefixem (nebo se mu přesně rovná).
 *
 * @param mode 0 = odznačit ostatní, ponechat jen match;
 *             1 = jen přidat match k existujícím;
 *             2 = jen odebrat match z existujících.
 */
static void region_group_apply ( const char *prefix, int mode )
{
    size_t count = 0;
    const st_MHMAP_EXPORT_REGION *regions = mhmap_get_export_regions ( &count );
    size_t plen = strlen ( prefix );

    for ( size_t i = 0; i < count; i++ )
    {
        bool match = ( strncmp ( regions[ i ].name, prefix, plen ) == 0 );
        if ( mode == 0 )       region_set_visible ( (int)i, match );
        else if ( mode == 1 && match ) region_set_visible ( (int)i, true );
        else if ( mode == 2 && match ) region_set_visible ( (int)i, false );
    }
}


/**
 * @brief Render popup menu pro výběr viditelných regionů (item 8).
 *
 * Vyvoláno tlačítkem "≡" před prvním tabem. Obsahuje:
 *  - "Show all" / "Hide all"
 *  - Předdefinované skupiny (ROM only, VRAM only, ...)
 *  - Per-region checkbox seznam
 */
static void render_regions_menu ( void )
{
    if ( !ImGui::BeginPopup ( "mhmap_regions_menu" ) ) return;

    size_t count = 0;
    const st_MHMAP_EXPORT_REGION *regions = mhmap_get_export_regions ( &count );

    if ( ImGui::MenuItem ( _L( "Show all##mhmap_menu" ) ) )
    {
        g_mhmap_window.region_visibility_mask = 0xFFFFFFFFu;
    };
    if ( ImGui::MenuItem ( _L( "Hide all##mhmap_menu" ) ) )
    {
        g_mhmap_window.region_visibility_mask = 0;
    };

    ImGui::Separator ( );

    if ( ImGui::BeginMenu ( _L( "Groups##mhmap_menu" ) ) )
    {
        if ( ImGui::MenuItem ( _L( "Only bus + ram##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "bus", 1 );
            region_group_apply ( "ram", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only ROM##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "rom",   1 );
            region_group_apply ( "cgrom", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only VRAM (all)##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "vram", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only VRAM 320 modes##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "vram800-320", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only VRAM 640 modes##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "vram800-640", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only PCG (MZ-1500)##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "pcg", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only IORQ##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "iorq", 1 );
        };
        if ( ImGui::MenuItem ( _L( "Only Memext##mhmap_grp" ) ) )
        {
            g_mhmap_window.region_visibility_mask = 0;
            region_group_apply ( "memext", 1 );
        };
        ImGui::EndMenu ( );
    };

    ImGui::Separator ( );

    /* Per-region checkbox toggle. */
    for ( size_t i = 0; i < count; i++ )
    {
        bool vis = region_is_visible ( (int)i );
        char label[ 96 ];
        snprintf ( label, sizeof ( label ), "%s##mhmap_chk_%zu", regions[ i ].label, i );
        if ( ImGui::Checkbox ( label, &vis ) )
        {
            region_set_visible ( (int)i, vis );
        };
    };

    ImGui::EndPopup ( );
}


/**
 * @brief Render tab bar přes všechny dostupné regiony.
 *
 * Používá veřejné API @c mhmap_get_export_regions. Per-arch zobrazí jen
 * relevantní (MZ-800 = 22, MZ-1500 = 9). Selekce uložena v
 * @c g_mhmap_window.selected_region_idx. Viditelné regiony jsou určeny
 * @c region_visibility_mask, ovládá se buď tlačítkem "Hide" (X) na tabu
 * nebo přes menu "≡" před tabbarem.
 */
static void render_region_tabs ( void )
{
    size_t count = 0;
    const st_MHMAP_EXPORT_REGION *regions = mhmap_get_export_regions ( &count );

    if ( count == 0 )
    {
        ImGui::TextDisabled ( "%s", _( "(no regions available for this architecture)" ) );
        return;
    };

    /* Tlačítko otvírající menu pro show/hide regionů (item 8). */
    if ( ImGui::Button ( _L( "...##mhmap_menu_btn" ) ) )
    {
        ImGui::OpenPopup ( "mhmap_regions_menu" );
    };
    if ( ImGui::IsItemHovered ( ) )
    {
        ImGui::SetTooltip ( "%s", _( "Show / hide regions, predefined groups" ) );
    };
    render_regions_menu ( );
    ImGui::SameLine ( );

    /* Bold/highlighted active tab pro lepší viditelnost (item 2).
     * ImGui nemá bold font, ale TabActive barvu lze pushnout - dáme jasně
     * odlišnou (např. azurová) + světlejší text. */
    ImGui::PushStyleColor ( ImGuiCol_TabActive,        IM_COL32 (  60, 140, 220, 255 ) );
    ImGui::PushStyleColor ( ImGuiCol_TabHovered,       IM_COL32 (  90, 170, 240, 255 ) );

    ImGuiTabBarFlags tab_flags = ImGuiTabBarFlags_Reorderable |
                                 ImGuiTabBarFlags_FittingPolicyScroll;

    if ( !ImGui::BeginTabBar ( "##mhmap_regions", tab_flags ) )
    {
        ImGui::PopStyleColor ( 2 );
        return;
    };

    for ( size_t i = 0; i < count; i++ )
    {
        if ( !region_is_visible ( (int)i ) ) continue;

        /* Stable ID = pad i do labelu kvůli ImGui tab uniqueness. */
        char label[ 96 ];
        snprintf ( label, sizeof ( label ), "%s##mhmap_tab_%zu", regions[ i ].label, i );

        /* Bez X tlačítka na tabu - schovávání řešíme přes "..." menu
         * s checkboxy (region_visibility_mask). */
        if ( ImGui::BeginTabItem ( label ) )
        {
            if ( g_mhmap_window.selected_region_idx != (int)i )
            {
                g_mhmap_window.selected_region_idx  = (int)i;
                g_mhmap_window.selected_cell_offset = -1;
            };

            /* Render heatmap grid pro aktivní region.
             * cells = Live (region->buffer) nebo Imported (offset v import_data). */
            const st_MHMAP_CELL *cells = cells_active ( &regions[ i ] );
            st_MHMAP_REGION_STATS stats = mhmap_grid_compute_stats ( cells, regions[ i ].size_cells );

            const char *src_label = ( g_mhmap_window.data_source == MHMAP_WINDOW_DATA_IMPORTED
                                      && g_mhmap_window.have_import )
                                    ? "imported" : "live";
            ImGui::TextDisabled ( "%s [%s]: %u cells, max R=%u W=%u X=%u, active=%u",
                                  regions[ i ].name, src_label,
                                  regions[ i ].size_cells,
                                  stats.max_r, stats.max_w, stats.max_x,
                                  stats.active_cells );

            render_filter_bar ( );

            ImGui::Separator ( );

            /* Horizontal split: scrollable grid vlevo + side panel vpravo (toggle).
             * Pomocí ImGui Table s Resizable + BordersInnerV dostaneme draggable
             * splitter zdarma. Šířka pravého sloupce se ukládá do imgui.ini
             * (NoSavedSettings je vypnuté), takže si ji ImGui pamatuje. */
            if ( g_mhmap_window.side_panel_visible )
            {
                ImGuiTableFlags tflags = ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_NoPadOuterX;

                if ( ImGui::BeginTable ( "##mhmap_split", 2, tflags ) )
                {
                    ImGui::TableSetupColumn ( "##grid",  ImGuiTableColumnFlags_WidthStretch );
                    ImGui::TableSetupColumn ( "##panel", ImGuiTableColumnFlags_WidthFixed, 280.0f );

                    ImGui::TableNextRow ( );

                    ImGui::TableNextColumn ( );
                    ImGui::BeginChild ( "##mhmap_grid_scroll", ImVec2 ( 0, 0 ),
                                        ImGuiChildFlags_Borders,
                                        ImGuiWindowFlags_HorizontalScrollbar );
                    mhmap_grid_render ( &regions[ i ], cells, &stats );
                    ImGui::EndChild ( );

                    ImGui::TableNextColumn ( );
                    ImGui::BeginChild ( "##mhmap_side_panel", ImVec2 ( 0, 0 ),
                                        ImGuiChildFlags_Borders );
                    render_side_panel ( &regions[ i ], i, &stats );
                    ImGui::EndChild ( );

                    ImGui::EndTable ( );
                };
            }
            else
            {
                /* Side panel skrytý - grid přes celou šířku content area. */
                ImGui::BeginChild ( "##mhmap_grid_scroll", ImVec2 ( 0, 0 ),
                                    ImGuiChildFlags_Borders,
                                    ImGuiWindowFlags_HorizontalScrollbar );
                mhmap_grid_render ( &regions[ i ], cells, &stats );
                ImGui::EndChild ( );
            };

            ImGui::EndTabItem ( );
        };
    }

    ImGui::EndTabBar ( );
    ImGui::PopStyleColor ( 2 );
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */


extern "C"
{


void mhmap_window_show_hide ( void )
{
    if ( g_gui ) g_gui->showMemoryHeatmapWindow = !g_gui->showMemoryHeatmapWindow;
}


void mhmap_window_render ( bool *p_open )
{
    /* Focus-on-open: detekce false -> true transition okna. Po otevření
     * jednorázově nastavíme focus, aby se okno vykreslilo nad ostatními.
     * Standardní z-order = klik na jiné okno funguje normálně. */
    static bool s_prev_open = false;
    bool focus_on_open = ( *p_open && !s_prev_open );
    s_prev_open = *p_open;

    if ( !*p_open ) return;

    mhmap_window_state_init ( );

    ImGui::SetNextWindowSize ( ImVec2 ( 1202, 850 ), ImGuiCond_FirstUseEver );

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;

    if ( focus_on_open )
        ImGui::SetNextWindowFocus ( );
    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *mhmap_title = _L( "Memory Heatmap##mhmap_main" );
    auto_layout_first_use_portrait ( mhmap_title, 1202.0f, 850.0f );
    if ( !ImGui::Begin ( mhmap_title, p_open, flags ) )
    {
        ImGui::End ( );
        return;
    };

    render_top_bar ( );
    ImGui::Separator ( );
    render_region_tabs ( );

    ImGui::End ( );

    /* Dialogy mimo Begin/End - musí běžet napříč framy. */
    render_export_dialog ( );
    render_import_dialog ( );
}


} /* extern "C" */

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED && (MZARCH == 800 || MZARCH == 1500) */
