/**
 * @file   iorqlog.c
 * @brief  Implementace IORQ Log subsystému trace-suite.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "iorqlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "emulator/debugger/trace/reclife.h"

st_IORQLOG_CONFIG g_iorqlog_config;
int g_iorqlog_active = 0;

uint8_t g_tracelog_iorq_unconnected = 0;
uint8_t g_tracelog_iorq_conflict = 0;

static st_TLOG_WRITER s_writer;
static int s_writer_open = 0;

void iorqlog_record ( en_IORQLOG_EVENT_TYPE etype,
                      en_IORQLOG_DIRECTION dir,
                      uint16_t source_addr, uint16_t port_or_addr,
                      uint8_t value, uint8_t pulse_duration_pxclk )
{
    if ( !s_writer_open ) return;

    uint64_t pxclk_total = tlog_common_get_pxclk_total ( );
    uint32_t screens = tlog_common_get_screens_total ( );
    uint32_t pxclk_in = tlog_common_get_pxclk_in_screen ( );

    uint8_t buf[ 24 ];
    /* pxclk_total LE */
    for ( int i = 0; i < 8; i++ ) buf[ i ] = (uint8_t)( pxclk_total >> ( i * 8 ) );
    /* screens_total LE */
    for ( int i = 0; i < 4; i++ ) buf[ 8 + i ] = (uint8_t)( screens >> ( i * 8 ) );
    /* pxclk_in_screen LE */
    for ( int i = 0; i < 4; i++ ) buf[ 12 + i ] = (uint8_t)( pxclk_in >> ( i * 8 ) );
    buf[ 16 ] = (uint8_t) etype;
    buf[ 17 ] = (uint8_t) dir;
    buf[ 18 ] = (uint8_t)( source_addr & 0xFF );
    buf[ 19 ] = (uint8_t)( ( source_addr >> 8 ) & 0xFF );
    buf[ 20 ] = (uint8_t)( port_or_addr & 0xFF );
    buf[ 21 ] = (uint8_t)( ( port_or_addr >> 8 ) & 0xFF );
    buf[ 22 ] = value;
    buf[ 23 ] = pulse_duration_pxclk;

    tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
}


/* ===========================================================================
 *  Header / lifecycle
 * =========================================================================== */

static char *build_subsys_header_json ( void )
{
    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "start_pxclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_total ( ) );

    json_builder_set_member_name ( b, "start_pxclk_in_screen" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_in_screen ( ) );

    json_builder_set_member_name ( b, "start_screens" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_screens_total ( ) );

    json_builder_set_member_name ( b, "start_cpuclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_cpuclk_total ( ) );

    json_builder_set_member_name ( b, "event_record_size" );
    json_builder_add_int_value ( b, 24 );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    g_object_unref ( gen );
    if ( root ) json_node_unref ( root );
    g_object_unref ( b );

    return out;
}


int iorqlog_start ( void )
{
    if ( s_writer_open ) return 0;
    if ( !g_iorqlog_config.dir || !g_iorqlog_config.dir[0]
         || !g_iorqlog_config.name || !g_iorqlog_config.name[0] ) {
        fprintf ( stderr, "[trace-suite] iorqlog: empty dir or name\n" );
        return -1;
    }
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths,
                                                     g_iorqlog_config.dir );
    uint64_t init_px = tlog_common_get_pxclk_total ( );
    uint64_t init_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t init_sc = tlog_common_get_screens_total ( );
    if ( tlog_writer_open ( &s_writer, "iorqlog",
                            resolved_dir, g_iorqlog_config.name,
                            g_iorqlog_config.chunk_mb, g_iorqlog_config.max_total_mb,
                            init_px, init_cpu, init_sc ) != 0 ) {
        g_free ( resolved_dir );
        return -1;
    }
    s_writer_open = 1;
    tlog_writer_set_record_size ( &s_writer, TLOG_RECORD_SIZE_EVENT24 );
    char *hdr = build_subsys_header_json ( );
    tlog_writer_update_meta ( &s_writer, hdr );
    g_free ( hdr );
    g_free ( resolved_dir );
    fprintf ( stderr, "[trace-suite] iorqlog: recording started (dir='%s', name='%s')\n",
              g_iorqlog_config.dir, g_iorqlog_config.name );
    return 0;
}


void iorqlog_stop ( void )
{
    if ( !s_writer_open ) return;
    uint64_t now_px = tlog_common_get_pxclk_total ( );
    uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t now_sc = tlog_common_get_screens_total ( );
    tlog_writer_close ( &s_writer, now_px, now_cpu, now_sc );
    s_writer_open = 0;
    fprintf ( stderr, "[trace-suite] iorqlog: recording stopped\n" );
}


void iorqlog_finalize ( void )
{
    if ( g_iorqlog_config.save_on_exit && s_writer_open ) {
        iorqlog_stop ( );
    }
    g_free ( g_iorqlog_config.dir );
    g_free ( g_iorqlog_config.name );
    g_iorqlog_config.dir = NULL;
    g_iorqlog_config.name = NULL;
}


int iorqlog_is_truncated ( void )
{
    /* Pokud writer není otevřený, recording neexistuje, tedy ani
     * "truncated" stav nedává smysl. */
    if ( !s_writer_open ) return 0;
    return tlog_writer_is_truncated ( &s_writer );
}


int iorqlog_save_segment ( const char *path )
{
    /* Uzavřít aktuální segment + volitelně přesměrovat na path. Vzor sdílený
     * s cputrack_save_segment - viz tam pro detaily semantiky. */
    int was_active = ( g_iorqlog_active != 0 );

    /* Prázdná cesta = "force save now": flush rozdělaného segmentu BEZ
     * uzavření/reopenu (segment pokračuje do téhož manifestu). Viz
     * cputrack_save_segment + mzdos 0022 pro detaily. */
    if ( !( path && path[ 0 ] ) ) {
        if ( s_writer_open ) {
            uint64_t now_px = tlog_common_get_pxclk_total ( );
            uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
            uint32_t now_sc = tlog_common_get_screens_total ( );
            tlog_writer_flush_chunk ( &s_writer, now_px, now_cpu, now_sc );
        }
        return 0;
    }

    if ( s_writer_open ) {
        iorqlog_stop ( );
    }
    reclife_redirect_path ( &g_iorqlog_config.dir, &g_iorqlog_config.name, path );
    if ( was_active ) {
        return iorqlog_start ( );
    }
    return 0;
}


/* Sdílený lifecycle deskriptor (reclife) - viz cputrack.c pro detaily vzoru. */
static const st_RECLIFE_DESC s_iorqlog_reclife = {
    .subsys_name = "iorqlog",
    .mode_ptr    = (int *) &g_iorqlog_config.mode,
    .active_ptr  = &g_iorqlog_active,
    .fn_start    = iorqlog_start,
    .fn_stop     = iorqlog_stop,
    .fn_reset    = NULL,
    .fn_save     = iorqlog_save_segment,
};


void iorqlog_recompute_active ( int debugger_active )
{
    reclife_recompute_active ( &s_iorqlog_reclife, debugger_active );
}


/* ===========================================================================
 *  Init / config / CLI options
 * =========================================================================== */

void iorqlog_init ( void )
{
    memset ( &g_iorqlog_config, 0, sizeof ( g_iorqlog_config ) );
    g_iorqlog_config.mode = TLOG_MODE_OFF;
    g_iorqlog_config.chunk_mb = TLOG_DEFAULT_CHUNK_MB;
    g_iorqlog_config.max_total_mb = TLOG_DEFAULT_MAX_TOTAL_MB;
    g_iorqlog_config.save_on_exit = 1;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "TRACE_IORQLOG" );
    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, TLOG_MODE_OFF,
                                           TLOG_MODE_OFF,         "OFF",
                                           TLOG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           TLOG_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_iorqlog_config.mode, (void*) &g_iorqlog_config.mode );
    elm = cfgmodule_register_new_element ( cmod, "dir", CFGENTYPE_TEXT, "trace-suite" );
    cfgelement_bind ( elm, (void*) &g_iorqlog_config.dir );
    elm = cfgmodule_register_new_element ( cmod, "name", CFGENTYPE_TEXT, "iorqlog" );
    cfgelement_bind ( elm, (void*) &g_iorqlog_config.name );
    elm = cfgmodule_register_new_element ( cmod, "chunk_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_CHUNK_MB, 1, 4096 );
    cfgelement_set_handlers ( elm, (void*) &g_iorqlog_config.chunk_mb, (void*) &g_iorqlog_config.chunk_mb );
    elm = cfgmodule_register_new_element ( cmod, "max_total_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_MAX_TOTAL_MB, 0, 1048576 );
    cfgelement_set_handlers ( elm, (void*) &g_iorqlog_config.max_total_mb, (void*) &g_iorqlog_config.max_total_mb );
    elm = cfgmodule_register_new_element ( cmod, "save_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_iorqlog_config.save_on_exit, (void*) &g_iorqlog_config.save_on_exit );

    /* Propagace .ini / default hodnot do g_iorqlog_config (jinak by dir/name
     * zůstaly NULL). Viz analogický komentář v cputrack_init(). */
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void iorqlog_apply_cli_options ( void )
{
    const char *v;
    v = sdlapp_option_value ( "--iorqlog-mode" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_iorqlog_config.mode = TLOG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "window" ) == 0 ) g_iorqlog_config.mode = TLOG_MODE_WITH_WINDOW;
        else if ( g_ascii_strcasecmp ( v, "always" ) == 0 ) g_iorqlog_config.mode = TLOG_MODE_ALWAYS;
        else fprintf ( stderr, "Invalid --iorqlog-mode value: %s\n", v );
    }
    v = sdlapp_option_value ( "--iorqlog-dir" );
    if ( v ) { g_free ( g_iorqlog_config.dir ); g_iorqlog_config.dir = g_strdup ( v ); }
    v = sdlapp_option_value ( "--iorqlog-name" );
    if ( v ) { g_free ( g_iorqlog_config.name ); g_iorqlog_config.name = g_strdup ( v ); }
    v = sdlapp_option_value ( "--iorqlog-chunk-mb" );
    if ( v ) g_iorqlog_config.chunk_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--iorqlog-max-total-mb" );
    if ( v ) g_iorqlog_config.max_total_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--iorqlog-save-on-exit" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_iorqlog_config.save_on_exit = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_iorqlog_config.save_on_exit = 0;
    }
}


const st_TLOG_WRITER *iorqlog_get_writer ( void )
{
    return &s_writer;
}
