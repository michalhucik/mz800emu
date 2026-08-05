/**
 * @file   tlog_common.c
 * @brief  Implementace společného frameworku trace-suite (chunk writer +
 *         meta.json + clock domain accessory).
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "tlog_common.h"
#include "mzarch/mzhal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "emulator/mzarch/mzarch_config.h"

#if MZARCH == 800
#include "emulator/mzarch/mz800/gdg/mz800_gdg.h"
#include "emulator/mzarch/mz800/gdg/mz800_video.h"
#include "emulator/mzarch/mz800/gdg/mz800_gdgclk.h"
#elif MZARCH == 1500
#include "emulator/mzarch/mz1500/gdg/mz1500_gdg.h"
#include "emulator/mzarch/mz1500/gdg/mz1500_video.h"
#include "emulator/mzarch/mz1500/gdg/mz1500_gdgclk.h"
#elif MZARCH == 700
#include "emulator/mzarch/mz700/gdg/mz700_gdg.h"
#include "emulator/mzarch/mz700/gdg/mz700_video.h"
#include "emulator/mzarch/mz700/gdg/mz700_gdgclk.h"
#else
#error "Unsupported MZARCH for trace-suite"
#endif


/* ===========================================================================
 *  Kumulativní čítač disk-bajtů trace-suite (0019 v3 - byte backstop)
 * =========================================================================== */

/**
 * @brief Process-globální kumulativní součet disk-bajtů zapsaných trace-suite.
 *
 * Inkrementuje se při každém reálném zápisu na disk (chunk soubor, meta.json,
 * per-subsystém initial-state dumpy). Konzument (0019 byte backstop) accountuje
 * deltu. Modifikován z emu vlákna; prostý uint64 bez locku (viz Doxygen API).
 */
static uint64_t g_tlog_disk_bytes_written = 0;

/**
 * @brief Process-globální hook volaný po každém inkrementu disk-byte čítače.
 *
 * NULL = žádný hook (default, chování trace-suite beze změny). Nenulový =
 * flush-side byte backstop guard registrovaný bp_action vrstvou (0019 v3).
 * Viz @ref tlog_common_set_disk_flush_hook.
 */
static tlog_disk_flush_hook_fn g_tlog_disk_flush_hook = NULL;


void tlog_common_disk_bytes_add ( uint64_t n )
{
    if ( n == 0 ) return;
    g_tlog_disk_bytes_written += n;
    /* 0019 v3 flush-side: notifikuj registrovaný guard, ať může vyhodnotit
     * byte backstop i mezi dvěma trace_save BP fire (trace flooduje disk
     * inkrementálně po chunkách). tlog_common sám prah NEvyhodnocuje (layering:
     * nezná breakpoints/emulator), jen předá právě přičtené bajty. */
    if ( g_tlog_disk_flush_hook ) g_tlog_disk_flush_hook ( n );
}


uint64_t tlog_common_disk_bytes_total ( void )
{
    return g_tlog_disk_bytes_written;
}


void tlog_common_disk_bytes_reset ( void )
{
    g_tlog_disk_bytes_written = 0;
}


void tlog_common_set_disk_flush_hook ( tlog_disk_flush_hook_fn fn )
{
    g_tlog_disk_flush_hook = fn;
}


/* ===========================================================================
 *  Pomocné: cesty, mkdir
 * =========================================================================== */

int tlog_common_ensure_dir ( const char *dir )
{
    if ( !dir || !dir[0] ) return -1;
    /* g_mkdir_with_parents vrací 0 i pro existující adresář. */
    if ( g_mkdir_with_parents ( dir, 0755 ) != 0 ) {
        fprintf ( stderr, "[trace-suite] mkdir '%s' failed: %s\n",
                  dir, g_strerror ( errno ) );
        return -1;
    }
    return 0;
}


/* ===========================================================================
 *  Clock domain accessors
 * =========================================================================== */

uint64_t tlog_common_get_pxclk_total ( void )
{
    return gdg_get_total_ticks ( );
}

uint32_t tlog_common_get_screens_total ( void )
{
    return g_gdg.total_elapsed.screens;
}

uint32_t tlog_common_get_pxclk_in_screen ( void )
{
    return g_gdg.total_elapsed.ticks;
}

uint64_t tlog_common_get_cpuclk_total ( void )
{
    /* MZ-800/700/1500: pxCLK / 5 = CPU CLK (~3.546895 MHz). */
    return gdg_get_total_ticks ( ) / g_mzhal.gdgclk2cpu_divider;
}

const char *tlog_common_get_platform_name ( void )
{
    /* Runtime z g_mzhal (mzhal krok 7) - hodnoty identické s dřívějším
     * #if MZARCH řetězem ("MZ-800"/"MZ-700"/"MZ-1500"); jde o čtvrtý
     * jmenný styl (display name bez TV normy), persistovaný v trace
     * manifestech = zmrazený kontrakt. */
    return g_mzhal.arch_display_name;
}

uint32_t tlog_common_get_pxclk_freq ( void )
{
    /* Reálná frekvence krystalu (mz800_gdgclk.h: g_mzhal.gdgclk_real_base = 17734475).
     * Emulátor interně počítá s mírně upravenou hodnotou (g_mzhal.gdgclk_base)
     * pro celočíselný počet pxCLK / sec, ale do meta.json hlásíme reálnou. */
    return g_mzhal.gdgclk_real_base;
}

uint32_t tlog_common_get_cpu_divider ( void )
{
    return g_mzhal.gdgclk2cpu_divider;
}

uint32_t tlog_common_get_pxclk_per_screen ( void )
{
    return g_mzhal.video_screen_ticks;
}


/* ===========================================================================
 *  Chunk writer - low level disk I/O
 * =========================================================================== */

/**
 * @brief Vytvoř plnou cestu k chunk souboru:
 *        `<dir>/<name>.NNN.bin` (NNN = 3-digit zero-padded index).
 *
 * @return Heap-allocated string (caller g_free()).
 */
static char *build_chunk_path ( const st_TLOG_WRITER *w, unsigned chunk_index )
{
    char fname[ 256 ];
    g_snprintf ( fname, sizeof ( fname ), "%s.%03u.bin", w->name, chunk_index );
    return g_build_filename ( w->dir, fname, NULL );
}


/**
 * @brief Zapsat současný buffer (buffer_used bytes) na disk jako chunk.
 *
 * Implementační detail @ref tlog_writer_flush_chunk(). Po úspěchu
 * - přidá záznam do w->chunks_meta
 * - resetuje buffer_used na 0
 * - inkrementuje chunk_index
 * - aktualizuje current_chunk_start_* anchory na předané "now" hodnoty
 *
 * @return 0 OK, -1 error.
 */
static int do_flush_chunk ( st_TLOG_WRITER *w,
                            uint64_t now_pxclk, uint64_t now_cpuclk,
                            uint32_t now_screens )
{
    if ( w->buffer_used == 0 ) return 0;

    char *path = build_chunk_path ( w, w->chunk_index );
    FILE *fp = g_fopen ( path, "wb" );
    if ( !fp ) {
        fprintf ( stderr, "[trace-suite] %s: cannot open '%s' for writing: %s\n",
                  w->subsys_name, path, g_strerror ( errno ) );
        g_free ( path );
        return -1;
    }

    size_t wrote = fwrite ( w->buffer, 1, w->buffer_used, fp );
    int err = ferror ( fp );
    fclose ( fp );

    if ( wrote != w->buffer_used || err ) {
        fprintf ( stderr, "[trace-suite] %s: short write to '%s' (%zu/%zu)\n",
                  w->subsys_name, path, wrote, w->buffer_used );
        g_free ( path );
        return -1;
    }

    /* Konzolová notifikace - chunk swap je významná událost (zakuckává emu) */
    fprintf ( stderr, "[trace-suite] %s: chunk %u swap to disk (%zu B)\n",
              w->subsys_name, w->chunk_index, w->buffer_used );

    /* 0019 v3: chunk soubor reálně zapsán -> do kumulativního disk čítače
     * (byte backstop). Toto je dominantní objem disk footprintu (64 MB/chunk). */
    tlog_common_disk_bytes_add ( (uint64_t) w->buffer_used );

    /* Záznam do chunks_meta */
    st_TLOG_CHUNK_META meta;
    meta.index = w->chunk_index;
    meta.bytes = w->buffer_used;
    meta.start_pxclk = w->current_chunk_start_pxclk;
    meta.start_cpuclk = w->current_chunk_start_cpuclk;
    meta.start_screens = w->current_chunk_start_screens;
    g_array_append_val ( w->chunks_meta, meta );

    /* Reset bufferu, posun anchorů na nový chunk */
    w->buffer_used = 0;
    w->chunk_index++;
    w->current_chunk_start_pxclk = now_pxclk;
    w->current_chunk_start_cpuclk = now_cpuclk;
    w->current_chunk_start_screens = now_screens;

    g_free ( path );
    return 0;
}


/* ===========================================================================
 *  Meta.json writer
 * =========================================================================== */

/**
 * @brief Sestavit a zapsat meta.json pro daný writer.
 *
 * Formát (V1):
 * @code
 * {
 *   "subsys": "cputrack",
 *   "platform": "MZ-800",
 *   "pxclk_freq": 17734475,
 *   "cpu_divider": 5,
 *   "pxclk_per_screen": 97344,
 *   "start_pxclk": 12345,
 *   "start_cpuclk": 2469,
 *   "start_screens": 0,
 *   "start_pxclk_in_screen": 12345,
 *   "truncated": false,
 *   "truncated_reason": "",
 *   "chunks": [ { "index": 0, "file": "name.000.bin",
 *                 "bytes": 67108864, "start_pxclk": ...,
 *                 "start_cpuclk": ..., "start_screens": ... }, ... ],
 *   "subsys_header": { ... per-subsystém specifický fragment ... }
 * }
 * @endcode
 *
 * Per-subsystém pole se předává jako pre-formatted JSON string fragment
 * (obsahuje vnitřek objektu, např. `{"initial_regs":{...},"initial_ram_file":"..."}`),
 * tlog_common ho jen zabudovává pod klíč "subsys_header".
 */
int tlog_writer_update_meta ( st_TLOG_WRITER *w, const char *header_extra )
{
    /* Cache header_extra v writeru aby ho mohl použít auto-update_meta v
     * flush_chunk a close. Caller předává buď nový header (= update cache),
     * nebo NULL = "použij cached" (zachová existující). */
    if ( header_extra ) {
        g_free ( w->subsys_header_cache );
        w->subsys_header_cache = g_strdup ( header_extra );
    }
    /* Pokud caller poslal NULL ale máme cached, použij cache. */
    const char *effective_header = header_extra ? header_extra : w->subsys_header_cache;


    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "subsys" );
    json_builder_add_string_value ( b, w->subsys_name );

    json_builder_set_member_name ( b, "platform" );
    json_builder_add_string_value ( b, tlog_common_get_platform_name ( ) );

    json_builder_set_member_name ( b, "pxclk_freq" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_freq ( ) );

    json_builder_set_member_name ( b, "cpu_divider" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_cpu_divider ( ) );

    json_builder_set_member_name ( b, "pxclk_per_screen" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_per_screen ( ) );

    json_builder_set_member_name ( b, "truncated" );
    json_builder_add_boolean_value ( b, w->truncated ? TRUE : FALSE );

    json_builder_set_member_name ( b, "truncated_reason" );
    json_builder_add_string_value ( b, w->truncated ? "max_total_mb" : "" );

    json_builder_set_member_name ( b, "chunks" );
    json_builder_begin_array ( b );
    for ( unsigned i = 0; i < w->chunks_meta->len; i++ ) {
        st_TLOG_CHUNK_META *m = &g_array_index ( w->chunks_meta, st_TLOG_CHUNK_META, i );

        json_builder_begin_object ( b );

        json_builder_set_member_name ( b, "index" );
        json_builder_add_int_value ( b, m->index );

        char *chunk_file = g_strdup_printf ( "%s.%03u.bin", w->name, m->index );
        json_builder_set_member_name ( b, "file" );
        json_builder_add_string_value ( b, chunk_file );
        g_free ( chunk_file );

        json_builder_set_member_name ( b, "bytes" );
        json_builder_add_int_value ( b, (gint64) m->bytes );

        json_builder_set_member_name ( b, "start_pxclk" );
        json_builder_add_int_value ( b, (gint64) m->start_pxclk );

        json_builder_set_member_name ( b, "start_cpuclk" );
        json_builder_add_int_value ( b, (gint64) m->start_cpuclk );

        json_builder_set_member_name ( b, "start_screens" );
        json_builder_add_int_value ( b, m->start_screens );

        json_builder_end_object ( b );
    }
    json_builder_end_array ( b );

    /* Per-subsystém header je předáván jako pre-formatted JSON string fragment
     * (kontrakt zůstává - producenti hwlog/cputrack/intlog/iorqlog stále vrací
     * char*). Parsujeme ho přes JsonParser a embedujeme jako JsonNode pod
     * klíč "subsys_header". Tím konzument JSON vidí strukturovaný objekt,
     * ne string. */
    if ( effective_header ) {
        JsonParser *parser = json_parser_new ( );
        GError *perr = NULL;
        if ( !json_parser_load_from_data ( parser, effective_header, -1, &perr ) ) {
            fprintf ( stderr, "[trace-suite] %s: invalid subsys_header JSON: %s\n",
                      w->subsys_name,
                      perr ? perr->message : "unknown" );
            if ( perr ) g_error_free ( perr );
            g_object_unref ( parser );
            g_object_unref ( b );
            return -1;
        }
        json_builder_set_member_name ( b, "subsys_header" );
        /* json_node_copy aby si JsonBuilder vlastnil svou kopii. Parser pak
         * uvolníme bez vlivu na builder. */
        JsonNode *hdr = json_node_copy ( json_parser_get_root ( parser ) );
        json_builder_add_value ( b, hdr );  /* builder přebírá ownership */
        g_object_unref ( parser );
    }

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    /* json_builder_get_root() vrací JsonNode vlastněný builderem - NEsmí
     * se volat json_node_free(), uvolní se s g_object_unref(builder). */
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    char fname[ 256 ];
    g_snprintf ( fname, sizeof ( fname ), "%s.json", w->name );
    char *path = g_build_filename ( w->dir, fname, NULL );

    /* Místo json_generator_to_file() (jiné chování UTF-8/path na Windows
     * dle nasazené json-glib verze) generujeme do paměti a sami zapisujeme
     * binární. */
    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );
    int rc = 0;
    FILE *fp = g_fopen ( path, "wb" );
    if ( !fp ) {
        fprintf ( stderr, "[trace-suite] %s: cannot open meta '%s' for writing: %s\n",
                  w->subsys_name, path, g_strerror ( errno ) );
        rc = -1;
    } else {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) rc = -1;
        if ( ferror ( fp ) ) rc = -1;
        fclose ( fp );
        /* 0019 v3: meta.json reálně zapsán -> kumulativní disk čítač. */
        if ( rc == 0 ) tlog_common_disk_bytes_add ( (uint64_t) wrote );
    }
    g_free ( out );
    g_free ( path );
    g_object_unref ( gen );
    g_object_unref ( b );
    return rc;
}


/* ===========================================================================
 *  Public API: open / append / flush / close
 * =========================================================================== */

int tlog_writer_open ( st_TLOG_WRITER *w,
                       const char *subsys_name,
                       const char *dir, const char *name,
                       unsigned chunk_mb, unsigned max_total_mb,
                       uint64_t init_pxclk, uint64_t init_cpuclk,
                       uint32_t init_screens )
{
    memset ( w, 0, sizeof ( *w ) );

    if ( !subsys_name || !subsys_name[0] || !dir || !dir[0] || !name || !name[0] ) {
        fprintf ( stderr, "[trace-suite] tlog_writer_open: empty subsys/dir/name\n" );
        return -1;
    }

    if ( chunk_mb == 0 ) chunk_mb = TLOG_DEFAULT_CHUNK_MB;

    if ( tlog_common_ensure_dir ( dir ) != 0 ) {
        return -1;
    }

    w->subsys_name = subsys_name;
    w->dir = g_strdup ( dir );
    w->name = g_strdup ( name );
    w->chunk_size_bytes = (size_t)chunk_mb * 1024u * 1024u;
    w->max_total_bytes = (size_t)max_total_mb * 1024u * 1024u;

    w->buffer = (uint8_t *) g_try_malloc ( w->chunk_size_bytes );
    if ( !w->buffer ) {
        fprintf ( stderr, "[trace-suite] %s: failed to allocate %zu B chunk buffer\n",
                  subsys_name, w->chunk_size_bytes );
        g_free ( w->dir );
        g_free ( w->name );
        memset ( w, 0, sizeof ( *w ) );
        return -1;
    }
    w->buffer_size = w->chunk_size_bytes;
    w->buffer_used = 0;
    w->chunk_index = 0;
    w->total_bytes_written = 0;
    w->truncated = 0;

    w->current_chunk_start_pxclk = init_pxclk;
    w->current_chunk_start_cpuclk = init_cpuclk;
    w->current_chunk_start_screens = init_screens;

    w->chunks_meta = g_array_new ( FALSE, FALSE, sizeof ( st_TLOG_CHUNK_META ) );
    w->subsys_header_cache = NULL;

    return 0;
}


int tlog_writer_append ( st_TLOG_WRITER *w, const void *data, size_t n )
{
    if ( w->truncated ) return -1;
    if ( !w->buffer ) return -1;
    if ( n == 0 ) return 0;

    /* Check max_total_bytes */
    if ( w->max_total_bytes > 0 ) {
        if ( w->total_bytes_written + n > w->max_total_bytes ) {
            w->truncated = 1;
            fprintf ( stderr, "[trace-suite] %s: max-total-mb=%zu reached, recording stopped\n",
                      w->subsys_name, w->max_total_bytes / ( 1024u * 1024u ) );
            return -1;
        }
    }

    /* Pokud by append přetekl chunk, nejdřív flush. */
    if ( w->buffer_used + n > w->buffer_size ) {
        uint64_t now_px = tlog_common_get_pxclk_total ( );
        uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
        uint32_t now_sc = tlog_common_get_screens_total ( );
        if ( do_flush_chunk ( w, now_px, now_cpu, now_sc ) != 0 ) {
            return -1;
        }
        /* Po flush update meta. */
        tlog_writer_update_meta ( w, NULL );
    }

    /* Edge: jeden event větší než chunk. V V1 zakázáno - subsystémy mají
     * fixed event size (12-24 B). Pojistka: pokud by se to stalo, reportujeme
     * a fail (caller chyba v konfiguraci). */
    if ( n > w->buffer_size ) {
        fprintf ( stderr, "[trace-suite] %s: event size %zu > chunk size %zu, skipping\n",
                  w->subsys_name, n, w->buffer_size );
        return -1;
    }

    memcpy ( w->buffer + w->buffer_used, data, n );
    w->buffer_used += n;
    w->total_bytes_written += n;
    return 0;
}


int tlog_writer_flush_chunk ( st_TLOG_WRITER *w,
                              uint64_t now_pxclk, uint64_t now_cpuclk,
                              uint32_t now_screens )
{
    if ( !w->buffer ) return -1;
    if ( w->buffer_used == 0 ) return 0;
    int rc = do_flush_chunk ( w, now_pxclk, now_cpuclk, now_screens );
    /* Update meta po každém flush. */
    tlog_writer_update_meta ( w, NULL );
    return rc;
}


void tlog_writer_close ( st_TLOG_WRITER *w,
                         uint64_t now_pxclk, uint64_t now_cpuclk,
                         uint32_t now_screens )
{
    if ( !w->buffer ) return;

    /* Final flush zbytku */
    if ( w->buffer_used > 0 ) {
        do_flush_chunk ( w, now_pxclk, now_cpuclk, now_screens );
    }
    /* Final meta update (s aktuálními chunks). */
    tlog_writer_update_meta ( w, NULL );

    g_free ( w->buffer );
    g_free ( w->dir );
    g_free ( w->name );
    g_free ( w->subsys_header_cache );
    if ( w->chunks_meta ) {
        g_array_free ( w->chunks_meta, TRUE );
    }
    memset ( w, 0, sizeof ( *w ) );
}
