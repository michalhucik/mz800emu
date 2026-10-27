/**
 * @file   trace_reader.c
 * @brief  Stránkované čtení záznamů Trace Suite (viz trace_reader.h).
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "trace_reader.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "../../audio_analysis.h"
#include "../../audio_capture.h"
#include "ramlog.h"
#include "bplog.h"
#include "cputrack.h"

#include <math.h>
#include <stddef.h>

/* Dekodér audio_* čte záznamy paměti zachycení po bajtech (trace_row_audio,
 * trace_match); offsety musí sedět se strukturami v audio_capture.h. */
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_LEVEL_REC, src ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_LEVEL_REC, level ) == 13 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_PSG_REC, value ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_PSG_REC, psg ) == 14 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_PSG_REC, byte ) == 15 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_PSG_REC, channel ) == 16 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_PSG_REC, reg ) == 17 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, len_ticks ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, ch ) == 16 );
G_STATIC_ASSERT ( sizeof ( st_AUDIO_CAPTURE_FRAME_CH ) == 16 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_CH, max_level ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, ref_rms ) == 160 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, ref_peak ) == 164 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, sdl_rms ) == 168 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, sdl_peak ) == 172 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, sdl_blocks ) == 184 );
G_STATIC_ASSERT ( offsetof ( st_AUDIO_CAPTURE_FRAME_REC, flags ) == 186 );
G_STATIC_ASSERT ( AUDIO_CAPTURE_FRAME_FLAG_REF == 1u && AUDIO_CAPTURE_FRAME_FLAG_SDL == 2u );
G_STATIC_ASSERT ( AUDIO_CAPTURE_PSG_REG_TONE == 1 );
/* Dekodér ramlog čte st_RAMLOG_REC po bajtech. */
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, frame ) == 8 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, insn_pc ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, cpu_addr ) == 14 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, offset ) == 16 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, space ) == 20 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, old_value ) == 21 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, new_value ) == 22 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, src ) == 23 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, tin ) == 24 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, flags ) == 25 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, plane ) == 26 );
G_STATIC_ASSERT ( offsetof ( st_RAMLOG_REC, count ) == 28 );

/* Dekodér bplog čte st_BPLOG_REC po bajtech. */
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, frame ) == 8 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, insn_pc ) == 12 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, pc ) == 14 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, bp_id ) == 16 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, kind ) == 20 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, flags ) == 21 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, text_len ) == 22 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, name ) == 24 );
G_STATIC_ASSERT ( offsetof ( st_BPLOG_REC, text ) == 56 );
G_STATIC_ASSERT ( sizeof ( st_BPLOG_REC ) == 296 );
G_STATIC_ASSERT ( BPLOG_NAME_SIZE == sizeof ( ( (st_TRACE_FILTER *) 0 )->bp_name ) );


/* ===========================================================================
 *  Druhy kanálů a sloupce
 * =========================================================================== */

/** @brief Jména kanálů podle en_TRACE_KIND. */
static const char *const s_kind_names[ TRACE_KIND_COUNT ] = {
    "cputrack", "iorqlog", "intlog", "hwlog", "marklog", "eventlog",
    "audio_psg_writes", "audio_levels", "audio_frames", "ramlog", "bplog"
};

/** @brief Sloupce cputrack (pořadí = bity masky). Za `sym` druh záznamu
 *  a registry formátu v3 (TRACE_CPUTRACK_REG_*), ty jen na požádání. */
static const char *const s_cols_cputrack[] = { "seq", "pc", "bytes", "asm", "t_std", "wait", "t", "sym",
                                               "kind", "af", "bc", "de", "hl", "ix", "iy", "sp", "wz",
                                               "i", "r", "iff1", "iff2", "im", "halted" };
/** @brief Sloupce iorqlog. */
static const char *const s_cols_iorqlog[] = { "seq", "t", "frame", "pos", "type", "dir", "src", "addr", "value", "pulse" };
/** @brief Sloupce intlog. */
static const char *const s_cols_intlog[] = { "seq", "t", "frame", "pos", "class", "chip", "pin", "edge", "state" };
/** @brief Sloupce hwlog. */
static const char *const s_cols_hwlog[] = { "seq", "t", "frame", "pos", "chip", "sub", "payload" };
/** @brief Sloupce marklog. */
static const char *const s_cols_marklog[] = { "seq", "t", "frame", "pos", "id", "name" };
/** @brief Sloupce eventlog (ring Event Vieweru). */
static const char *const s_cols_eventlog[] = { "seq", "t", "frame", "pos", "cat", "sub", "pc", "text" };
/** @brief Sloupce audio_psg_writes (zápisy do PSG z paměti zachycení zvuku). */
static const char *const s_cols_audio_psg[] = { "seq", "t", "time_us", "frame", "psg", "byte", "ch", "reg", "value", "hz", "note" };
/** @brief Sloupce audio_levels (změny úrovní kanálů). */
static const char *const s_cols_audio_levels[] = { "seq", "t", "time_us", "frame", "channel", "level" };
/** @brief Sloupce audio_frames (souhrny zvukových snímků; pole po kanálech platformy). */
static const char *const s_cols_audio_frames[] = { "seq", "t", "time_us", "len_ticks", "edges", "on_pct", "rms_level",
                                                   "max_level", "ref_rms", "ref_peak", "sdl_rms", "sdl_peak", "sdl_blocks" };

/** @brief Sloupce ramlog (zápisy do paměti). `t` = začátek zapisující
 *  instrukce, `tin` = T-stav uvnitř instrukce v okamžiku zápisu (jen zdroj
 *  insn, jinak null). */
static const char *const s_cols_ramlog[] = { "seq", "t", "frame", "pc", "sym", "addr", "space", "offset",
                                             "plane", "old", "new", "src", "flags", "count", "tin" };

/** @brief Index sloupce `sym` u ramlog. */
#define TRACE_COL_RAMLOG_SYM 4

/** @brief Sloupce bplog (výstup akcí BP). `t`, `frame`, `insn_pc` a `pc`
 *  odpovídají `Cycle`, `Frame`, `InsnPC` a `PC` ve výrazu BP v okamžiku
 *  akce, `sym` je symbol `insn_pc`. */
static const char *const s_cols_bplog[] = { "seq", "t", "frame", "insn_pc", "sym", "pc", "bp", "name",
                                            "kind", "text", "flags" };

/** @brief Index sloupce `sym` u bplog. */
#define TRACE_COL_BPLOG_SYM 4
/** @brief Index sloupce `t` u cputrack (vynechá se bez času). */
#define TRACE_COL_CPUTRACK_T   6
/** @brief Index sloupce `sym` u cputrack. */
#define TRACE_COL_CPUTRACK_SYM 7
/** @brief Index sloupce `kind` u cputrack. */
#define TRACE_COL_CPUTRACK_KIND 8
/** @brief Výchozí sloupce cputrack (formát v2: seq .. sym). */
#define TRACE_CPUTRACK_DEFAULT_COLS_MASK ( ( 1u << ( TRACE_COL_CPUTRACK_SYM + 1 ) ) - 1u )

G_STATIC_ASSERT ( TRACE_COL_CPUTRACK_REG0 == TRACE_COL_CPUTRACK_KIND + 1 );
G_STATIC_ASSERT ( G_N_ELEMENTS ( s_cols_cputrack ) == TRACE_COL_CPUTRACK_REG0 + TRACE_CPUTRACK_REG_COUNT );
/** @brief Index sloupce `name` u marklog. */
#define TRACE_COL_MARKLOG_NAME 5
/** @brief Index sloupce `text` u eventlog. */
#define TRACE_COL_EVENTLOG_TEXT 7
/** @brief Velikost záznamu ringu Event Vieweru (st_EVENTLOG_EVENT). */
#define TRACE_EVENTLOG_RECORD_SIZE 32u
/** @brief Největší velikost záznamu čitelného kanálu (výstup akce BP, st_BPLOG_REC). */
#define TRACE_MAX_RECORD_SIZE 296u


const char *trace_kind_name ( en_TRACE_KIND kind )
{
    if ( (unsigned) kind >= TRACE_KIND_COUNT ) return NULL;
    return s_kind_names[ kind ];
}


const char *const *trace_columns ( en_TRACE_KIND kind, size_t *n )
{
    switch ( kind ) {
        case TRACE_KIND_CPUTRACK: *n = G_N_ELEMENTS ( s_cols_cputrack ); return s_cols_cputrack;
        case TRACE_KIND_IORQLOG:  *n = G_N_ELEMENTS ( s_cols_iorqlog );  return s_cols_iorqlog;
        case TRACE_KIND_INTLOG:   *n = G_N_ELEMENTS ( s_cols_intlog );   return s_cols_intlog;
        case TRACE_KIND_HWLOG:    *n = G_N_ELEMENTS ( s_cols_hwlog );    return s_cols_hwlog;
        case TRACE_KIND_MARKLOG:  *n = G_N_ELEMENTS ( s_cols_marklog );  return s_cols_marklog;
        case TRACE_KIND_EVENTLOG: *n = G_N_ELEMENTS ( s_cols_eventlog ); return s_cols_eventlog;
        case TRACE_KIND_AUDIO_PSG_WRITES: *n = G_N_ELEMENTS ( s_cols_audio_psg ); return s_cols_audio_psg;
        case TRACE_KIND_AUDIO_LEVELS: *n = G_N_ELEMENTS ( s_cols_audio_levels ); return s_cols_audio_levels;
        case TRACE_KIND_AUDIO_FRAMES: *n = G_N_ELEMENTS ( s_cols_audio_frames ); return s_cols_audio_frames;
        case TRACE_KIND_RAMLOG: *n = G_N_ELEMENTS ( s_cols_ramlog ); return s_cols_ramlog;
        case TRACE_KIND_BPLOG: *n = G_N_ELEMENTS ( s_cols_bplog ); return s_cols_bplog;
        default: *n = 0; return NULL;
    }
}


uint32_t trace_columns_mask ( en_TRACE_KIND kind, const char *const *names, size_t n,
                              const char **bad )
{
    size_t nc = 0;
    const char *const *cols = trace_columns ( kind, &nc );
    *bad = NULL;
    if ( !cols ) return 0;
    if ( !names || n == 0 ) {
        if ( kind == TRACE_KIND_CPUTRACK ) return TRACE_CPUTRACK_DEFAULT_COLS_MASK;
        return ( nc >= 32 ) ? 0xFFFFFFFFu : ( ( 1u << nc ) - 1u );
    }
    uint32_t mask = 0;
    for ( size_t i = 0; i < n; i++ ) {
        size_t k = 0;
        while ( k < nc && strcmp ( cols[ k ], names[ i ] ) != 0 ) k++;
        if ( k == nc ) {
            *bad = names[ i ];
            return 0;
        }
        mask |= 1u << k;
    }
    return mask;
}


int trace_cputrack_reg_index ( const char *name )
{
    if ( !name ) return -1;
    for ( unsigned i = 0; i < TRACE_CPUTRACK_REG_COUNT; i++ ) {
        if ( strcmp ( s_cols_cputrack[ TRACE_COL_CPUTRACK_REG0 + i ], name ) == 0 ) return (int) i;
    }
    return -1;
}


/* ===========================================================================
 *  Kurzor
 * =========================================================================== */

/** @brief Prefix verze kurzoru. */
#define TRACE_CURSOR_PREFIX "t1."


char *trace_cursor_encode ( const char *channel, unsigned segment, uint64_t seq )
{
    char *plain = g_strdup_printf ( "%s:%u:%" G_GUINT64_FORMAT, channel, segment, seq );
    char *b64 = g_base64_encode ( (const guchar *) plain, strlen ( plain ) );
    g_free ( plain );
    /* base64url bez '=' */
    for ( char *p = b64; *p; p++ ) {
        if ( *p == '+' ) *p = '-';
        else if ( *p == '/' ) *p = '_';
    }
    char *eq = strchr ( b64, '=' );
    if ( eq ) *eq = '\0';
    char *out = g_strconcat ( TRACE_CURSOR_PREFIX, b64, NULL );
    g_free ( b64 );
    return out;
}


bool trace_cursor_decode ( const char *cursor, const char *channel,
                           unsigned *segment, uint64_t *seq )
{
    if ( !cursor || !channel || strncmp ( cursor, TRACE_CURSOR_PREFIX, 3 ) != 0 ) return false;
    const char *body = cursor + 3;
    size_t len = strlen ( body );
    if ( len == 0 || len > 256 ) return false;
    GString *b = g_string_new ( NULL );
    for ( size_t i = 0; i < len; i++ ) {
        char c = body[ i ];
        if ( c == '-' ) c = '+';
        else if ( c == '_' ) c = '/';
        else if ( !g_ascii_isalnum ( c ) ) {
            g_string_free ( b, TRUE );
            return false;
        }
        g_string_append_c ( b, c );
    }
    while ( b->len % 4 ) g_string_append_c ( b, '=' );
    gsize dlen = 0;
    guchar *dec = g_base64_decode ( b->str, &dlen );
    g_string_free ( b, TRUE );
    if ( !dec ) return false;
    char *plain = g_strndup ( (const char *) dec, dlen );
    g_free ( dec );

    bool ok = false;
    char **parts = g_strsplit ( plain, ":", 4 );
    if ( g_strv_length ( parts ) == 3 && strcmp ( parts[ 0 ], channel ) == 0 ) {
        char *e1 = NULL, *e2 = NULL;
        guint64 sg = g_ascii_strtoull ( parts[ 1 ], &e1, 10 );
        guint64 sq = g_ascii_strtoull ( parts[ 2 ], &e2, 10 );
        if ( e1 && *e1 == '\0' && e2 && *e2 == '\0' && parts[ 1 ][ 0 ] && parts[ 2 ][ 0 ] &&
             sg <= 0xFFFFFFFFu ) {
            *segment = (unsigned) sg;
            *seq = sq;
            ok = true;
        }
    }
    g_strfreev ( parts );
    g_free ( plain );
    return ok;
}


/* ===========================================================================
 *  Čtení záznamů
 * =========================================================================== */

/**
 * @brief Najít chunk na disku, který obsahuje záznam @p seq.
 *
 * @param snap       Snímek.
 * @param seq        Záznam (< ram_first_seq).
 * @param idx        Výstup: index v snap->chunks.
 * @param start_seq  Výstup: seq prvního záznamu chunku.
 * @param end_seq    Výstup: seq za posledním záznamem chunku.
 * @return true = nalezeno.
 */
static bool trace_find_chunk ( const st_TLOG_READ_SNAPSHOT *snap, uint64_t seq,
                               unsigned *idx, uint64_t *start_seq, uint64_t *end_seq )
{
    uint64_t s = 0;
    for ( unsigned i = 0; snap->chunks && i < snap->chunks->len; i++ ) {
        const st_TLOG_CHUNK_META *m = &g_array_index ( snap->chunks, st_TLOG_CHUNK_META, i );
        uint64_t e = s + m->bytes / snap->record_size;
        if ( seq >= s && seq < e ) {
            *idx = i;
            *start_seq = s;
            *end_seq = e;
            return true;
        }
        s = e;
    }
    return false;
}


/** @brief 64bitový seek (chunk může mít až 4 GB, `long` na Windows má 32 bitů). */
static int trace_fseek64 ( FILE *fp, uint64_t offset )
{
#ifdef _WIN32
    return _fseeki64 ( fp, (__int64) offset, SEEK_SET );
#else
    return fseeko ( fp, (off_t) offset, SEEK_SET );
#endif
}


/**
 * @brief Přečíst záznamy z jednoho chunku na disku.
 *
 * @return 0 OK, -1 chyba (soubor chybí nebo je kratší).
 */
static int trace_read_chunk ( const st_TLOG_READ_SNAPSHOT *snap, unsigned idx,
                              uint64_t rec_in_chunk, uint64_t count, uint8_t *dst )
{
    const st_TLOG_CHUNK_META *m = &g_array_index ( snap->chunks, st_TLOG_CHUNK_META, idx );
    char fname[ 300 ];
    g_snprintf ( fname, sizeof ( fname ), "%s.%03u.bin", snap->name, m->index );
    char *path = g_build_filename ( snap->dir, fname, NULL );
    FILE *fp = g_fopen ( path, "rb" );
    g_free ( path );
    if ( !fp ) return -1;
    int rc = 0;
    size_t bytes = (size_t) ( count * snap->record_size );
    if ( trace_fseek64 ( fp, rec_in_chunk * snap->record_size ) != 0 ||
         fread ( dst, 1, bytes, fp ) != bytes ) {
        rc = -1;
    }
    fclose ( fp );
    return rc;
}


int trace_reader_fetch ( const st_TLOG_READ_SNAPSHOT *snap,
                         uint64_t from_seq, uint64_t count,
                         uint8_t *dst, uint64_t *out_count )
{
    *out_count = 0;
    if ( !snap || snap->record_size == 0 ) return -1;
    size_t rs = snap->record_size;
    uint64_t seq = from_seq;
    uint64_t end = snap->next_seq;
    if ( from_seq < end && count < end - from_seq ) end = from_seq + count;

    while ( seq < end ) {
        if ( seq < snap->ram_first_seq ) {
            unsigned idx;
            uint64_t cs, ce;
            if ( !trace_find_chunk ( snap, seq, &idx, &cs, &ce ) ) return -1;
            uint64_t n = ( ce < end ? ce : end ) - seq;
            if ( seq + n > snap->ram_first_seq ) n = snap->ram_first_seq - seq;
            if ( trace_read_chunk ( snap, idx, seq - cs, n, dst + ( seq - from_seq ) * rs ) != 0 ) return -1;
            seq += n;
        } else {
            /* RAM část: jen z kopie, souvisle od seq. */
            if ( !snap->ram_copy || seq < snap->ram_copy_seq ||
                 seq >= snap->ram_copy_seq + snap->ram_copy_count ) break;
            uint64_t avail = snap->ram_copy_seq + snap->ram_copy_count - seq;
            uint64_t n = end - seq;
            if ( n > avail ) n = avail;
            memcpy ( dst + ( seq - from_seq ) * rs,
                     snap->ram_copy + ( seq - snap->ram_copy_seq ) * rs, (size_t) ( n * rs ) );
            seq += n;
        }
        *out_count = seq - from_seq;
    }
    return 0;
}


/* ===========================================================================
 *  Čas cputrack
 * =========================================================================== */

/** @brief T-stavy záznamu cputrack (t_std + wait_clk; synchronizační záznam v3 nemá čas). */
static inline uint64_t cputrack_rec_t ( const uint8_t *r )
{
    if ( r[ 2 ] == CPUTRACK_INSN_LEN_SYNC ) return 0;
    uint32_t wait = (uint32_t) r[ 7 ] | ( (uint32_t) r[ 8 ] << 8 ) |
                    ( (uint32_t) r[ 9 ] << 16 ) | ( (uint32_t) r[ 10 ] << 24 );
    return (uint64_t) r[ 11 ] + wait;
}

/**
 * @brief Cache posledního známého součtu T-stavů v chunku na disku.
 *
 * Sekvenční stránkování (kurzor) pokračuje tam, kde předchozí stránka
 * skončila, takže stačí jedna položka. Chráněno s_time_lock.
 */
static struct {
    unsigned segment;     /**< 0 = prázdná. */
    uint64_t chunk_start; /**< seq začátku chunku. */
    uint64_t seq;         /**< seq, ke kterému platí prefix. */
    uint64_t prefix;      /**< Součet T-stavů záznamů [chunk_start, seq). */
} s_time_cache;
static GMutex s_time_lock;


void trace_reader_time_cache_reset ( void )
{
    g_mutex_lock ( &s_time_lock );
    memset ( &s_time_cache, 0, sizeof ( s_time_cache ) );
    g_mutex_unlock ( &s_time_lock );
}


/**
 * @brief Součet T-stavů záznamů chunku na disku od jeho začátku do @p seq.
 *
 * Použije cache, když platí pro stejný segment a chunk a není za @p seq;
 * jinak čte chunk od začátku (nejvýš velikost chunku).
 *
 * @return 0 OK, -1 chyba čtení.
 */
static int trace_disk_prefix ( const st_TLOG_READ_SNAPSHOT *snap, uint64_t chunk_start,
                               unsigned chunk_idx, uint64_t seq, uint64_t *out )
{
    uint64_t from = chunk_start, prefix = 0;
    g_mutex_lock ( &s_time_lock );
    if ( s_time_cache.segment == snap->segment && s_time_cache.chunk_start == chunk_start &&
         s_time_cache.seq <= seq ) {
        from = s_time_cache.seq;
        prefix = s_time_cache.prefix;
    }
    g_mutex_unlock ( &s_time_lock );

    enum { BATCH = 65536 };
    size_t rs = snap->record_size;
    uint8_t *buf = (uint8_t *) g_malloc ( BATCH * rs );
    int rc = 0;
    while ( from < seq ) {
        uint64_t n = seq - from;
        if ( n > BATCH ) n = BATCH;
        if ( trace_read_chunk ( snap, chunk_idx, from - chunk_start, n, buf ) != 0 ) {
            rc = -1;
            break;
        }
        for ( uint64_t k = 0; k < n; k++ ) prefix += cputrack_rec_t ( buf + k * rs );
        from += n;
    }
    g_free ( buf );
    *out = prefix;
    return rc;
}


/** @brief Uložit stav času do cache (jen chunky na disku). */
static void trace_time_cache_store ( unsigned segment, uint64_t chunk_start, uint64_t seq, uint64_t prefix )
{
    g_mutex_lock ( &s_time_lock );
    s_time_cache.segment = segment;
    s_time_cache.chunk_start = chunk_start;
    s_time_cache.seq = seq;
    s_time_cache.prefix = prefix;
    g_mutex_unlock ( &s_time_lock );
}


/**
 * @brief Stav výpočtu času cputrack během průchodu stránky.
 */
typedef struct st_TRACE_TIME
{
    bool     on;           /**< Čas se počítá. */
    bool     ok;           /**< Stav je platný pro aktuální seq. */
    bool     in_ram;       /**< Aktuální chunk je RAM buffer. */
    uint64_t chunk_start;  /**< seq začátku aktuálního chunku. */
    uint64_t chunk_end;    /**< seq za koncem aktuálního chunku. */
    uint64_t anchor_px;    /**< pxCLK total na začátku chunku. */
    uint64_t prefix;       /**< T-stavy od začátku chunku do aktuálního seq. */
} st_TRACE_TIME;


/**
 * @brief Nastavit stav času na záznam @p seq (začátek chunku nebo stránky).
 */
static void trace_time_locate ( st_TRACE_TIME *tm, const st_TLOG_READ_SNAPSHOT *snap,
                                const st_TRACE_PAGE_REQ *req, uint64_t seq )
{
    tm->ok = false;
    if ( !tm->on ) return;
    if ( seq >= snap->ram_first_seq ) {
        /* RAM chunk: báze z emu vlákna platí pro ram_copy_seq; na záznamy
         * za ní (čtení začalo o záznam dřív kvůli stavu před) se dopočítá
         * z kopie. */
        if ( !snap->live || seq < snap->ram_copy_seq ||
             seq > snap->ram_copy_seq + snap->ram_copy_count ||
             ( seq > snap->ram_copy_seq && !snap->ram_copy ) ) return;
        uint64_t prefix = req->ram_prefix_t;
        for ( uint64_t k = snap->ram_copy_seq; k < seq; k++ ) {
            prefix += cputrack_rec_t ( snap->ram_copy + ( k - snap->ram_copy_seq ) * snap->record_size );
        }
        tm->in_ram = true;
        tm->chunk_start = snap->ram_first_seq;
        tm->chunk_end = snap->next_seq;
        tm->anchor_px = snap->ram_start_pxclk;
        tm->prefix = prefix;
        tm->ok = true;
        return;
    }
    unsigned idx;
    uint64_t cs, ce;
    if ( !trace_find_chunk ( snap, seq, &idx, &cs, &ce ) ) return;
    uint64_t prefix = 0;
    if ( trace_disk_prefix ( snap, cs, idx, seq, &prefix ) != 0 ) return;
    tm->in_ram = false;
    tm->chunk_start = cs;
    tm->chunk_end = ce;
    tm->anchor_px = g_array_index ( snap->chunks, st_TLOG_CHUNK_META, idx ).start_pxclk;
    tm->prefix = prefix;
    tm->ok = true;
}


/* ===========================================================================
 *  Filtr a dekódování
 * =========================================================================== */

void trace_filter_init ( st_TRACE_FILTER *f )
{
    memset ( f, 0, sizeof ( *f ) );
    f->dir = -1;
    f->type = -1;
    f->chip = -1;
    f->sub = -1;
    f->marker = -1;
    f->reg = -1;
    f->space = -1;
    f->src = -1;
    f->bp = -1;
    f->bplog_kind = -1;
}


/** @brief uint16 LE ze záznamu. */
static inline uint16_t rd16 ( const uint8_t *p ) { return (uint16_t) ( p[ 0 ] | ( p[ 1 ] << 8 ) ); }
/** @brief uint32 LE ze záznamu. */
static inline uint32_t rd32 ( const uint8_t *p )
{
    return (uint32_t) p[ 0 ] | ( (uint32_t) p[ 1 ] << 8 ) | ( (uint32_t) p[ 2 ] << 16 ) | ( (uint32_t) p[ 3 ] << 24 );
}
/** @brief uint64 LE ze záznamu. */
static inline uint64_t rd64 ( const uint8_t *p )
{
    return (uint64_t) rd32 ( p ) | ( (uint64_t) rd32 ( p + 4 ) << 32 );
}


/**
 * @brief Vyhovuje záznam filtru?
 *
 * @param has_t  Záznam má čas (24B kanály vždy, cputrack jen s časem).
 * @param t      Čas záznamu v taktech GDG.
 */
static bool trace_match ( en_TRACE_KIND kind, const uint8_t *r, bool has_t, uint64_t t,
                          const st_TRACE_FILTER *f )
{
    if ( f->has_t && ( !has_t || t < f->t_from || t > f->t_to ) ) return false;
    switch ( kind ) {
        case TRACE_KIND_CPUTRACK:
            if ( f->has_pc ) {
                uint16_t pc = rd16 ( r );
                if ( pc < f->pc_from || pc > f->pc_to ) return false;
            }
            return true;
        case TRACE_KIND_IORQLOG:
            if ( f->type >= 0 && r[ 16 ] != f->type ) return false;
            if ( f->dir >= 0 && r[ 17 ] != f->dir ) return false;
            if ( f->has_port ) {
                uint16_t a = rd16 ( r + 20 );
                if ( f->port <= 0xFF ? ( ( a & 0xFF ) != f->port ) : ( a != f->port ) ) return false;
            }
            return true;
        case TRACE_KIND_HWLOG:
            if ( f->chip >= 0 && r[ 16 ] != f->chip ) return false;
            if ( f->sub >= 0 && r[ 17 ] != f->sub ) return false;
            return true;
        case TRACE_KIND_MARKLOG:
            if ( f->marker >= 0 && rd16 ( r + 16 ) != f->marker ) return false;
            return true;
        case TRACE_KIND_AUDIO_LEVELS:
            if ( f->channels && !( f->channels & ( 1u << ( r[ 12 ] & 31u ) ) ) ) return false;
            return true;
        case TRACE_KIND_RAMLOG:
            if ( !f->all && ( r[ 25 ] & ( RAMLOG_FLAG_UNCHANGED | RAMLOG_FLAG_SUPPRESSED ) ) ) return false;
            if ( f->has_pc && ( rd16 ( r + 12 ) < f->pc_from || rd16 ( r + 12 ) > f->pc_to ) ) return false;
            if ( f->has_addr ) {
                if ( r[ 25 ] & RAMLOG_FLAG_NO_CPU ) return false;
                uint16_t a = rd16 ( r + 14 );
                if ( a < f->addr_from || a > f->addr_to ) return false;
            }
            if ( f->space >= 0 && r[ 20 ] != f->space ) return false;
            if ( f->src >= 0 && r[ 23 ] != f->src ) return false;
            return true;
        case TRACE_KIND_BPLOG:
            if ( f->has_pc && ( rd16 ( r + 12 ) < f->pc_from || rd16 ( r + 12 ) > f->pc_to ) ) return false;
            if ( f->bp >= 0 && (int32_t) rd32 ( r + 16 ) != f->bp ) return false;
            if ( f->bplog_kind >= 0 && r[ 20 ] != f->bplog_kind ) return false;
            if ( f->bp_name[ 0 ] && strncmp ( (const char *) r + 24, f->bp_name, BPLOG_NAME_SIZE ) != 0 ) return false;
            return true;
        case TRACE_KIND_AUDIO_PSG_WRITES:
            if ( f->reg >= 0 && r[ 17 ] != f->reg ) return false;
            if ( f->channels ) {
                /* Kanály zasažené zápisem: PSG0 = 1 + ch, PSG1 = 5 + ch. */
                uint32_t hit = 0;
                if ( r[ 14 ] & 1u ) hit |= 1u << ( 1u + ( r[ 16 ] & 3u ) );
                if ( r[ 14 ] & 2u ) hit |= 1u << ( 5u + ( r[ 16 ] & 3u ) );
                if ( !( f->channels & hit ) ) return false;
            }
            return true;
        default:
            return true;
    }
}


/** @brief Jméno zvukového kanálu podle indexu audio logu ("ctc0", "psg0_0".."psg1_3"). */
const char *trace_audio_channel_name ( unsigned src )
{
    static const char *const names[ 9 ] = {
        "ctc0", "psg0_0", "psg0_1", "psg0_2", "psg0_3", "psg1_0", "psg1_1", "psg1_2", "psg1_3"
    };
    return ( src < 9 ) ? names[ src ] : "?";
}


/** @brief Hex řetězec n bajtů oddělených mezerou ("3E 05"). */
static char *hex_bytes ( const uint8_t *p, size_t n )
{
    GString *s = g_string_new ( NULL );
    for ( size_t i = 0; i < n; i++ ) g_string_append_printf ( s, i ? " %02X" : "%02X", p[ i ] );
    return g_string_free ( s, FALSE );
}


/** @brief Kontext read callbacku disassembleru nad bajty záznamu. */
typedef struct {
    uint16_t pc;          /**< Adresa prvního bajtu. */
    const uint8_t *bytes; /**< 4 bajty instrukce ze záznamu. */
} st_DASM_REC;

/** @brief Read callback disassembleru: bajty ze záznamu, mimo ně 0. */
static uint8_t trace_dasm_read ( uint16_t addr, void *user_data )
{
    const st_DASM_REC *d = (const st_DASM_REC *) user_data;
    uint16_t off = (uint16_t) ( addr - d->pc );
    return ( off < 4 ) ? d->bytes[ off ] : 0;
}


/** @brief Jméno typu iorqlog. */
static const char *iorq_type_name ( unsigned v )
{
    switch ( v ) {
        case 0: return "iorq";
        case 1: return "mreq_mapped";
        case 2: return "iorq_unconnected";
        case 3: return "iorq_conflict";
        default: return "unknown";
    }
}

/** @brief Jméno třídy intlog. */
static const char *int_class_name ( unsigned v )
{
    switch ( v ) {
        case 0: return "pin_edge";
        case 1: return "cpu_int_state";
        case 2: return "pio_state";
        case 3: return "irq_ack_im2";
        default: return "unknown";
    }
}

/** @brief Jméno čipu intlog (en_INTLOG_SOURCE_CHIP). */
static const char *int_chip_name ( unsigned v )
{
    static const char *const names[] = {
        "none", "ctc2", "pioz80",
        "pioz80_pa0", "pioz80_pa1", "pioz80_pa2", "pioz80_pa3",
        "pioz80_pa4", "pioz80_pa5", "pioz80_pa6", "pioz80_pa7",
        "pioz80_pb0", "pioz80_pb1", "pioz80_pb2", "pioz80_pb3",
        "pioz80_pb4", "pioz80_pb5", "pioz80_pb6", "pioz80_pb7",
        "fdc", "cpu", "pioz80_port_a", "pioz80_port_b", "vector_bus_latch"
    };
    return ( v < G_N_ELEMENTS ( names ) ) ? names[ v ] : "unknown";
}

/** @brief Jméno hrany intlog. */
static const char *int_edge_name ( unsigned v )
{
    switch ( v ) {
        case 0: return "none";
        case 1: return "rising";
        case 2: return "falling";
        default: return "unknown";
    }
}

/** @brief Jméno čipu hwlog (HWLOG_CHIP_*). */
static const char *hw_chip_name ( unsigned v )
{
    switch ( v ) {
        case 0x01: return "gdg_mode";
        case 0x02: return "gdg_banking";
        case 0x03: return "gdg_hwscroll";
        case 0x04: return "gdg_colors";
        case 0x05: return "gdg_wfrf";
        case 0x06: return "gdg_video";
        case 0x10: return "pio8255";
        case 0x11: return "ctc8253";
        case 0x12: return "pioz80";
        case 0x20: return "psg";
        case 0x30: return "qd";
        case 0x31: return "fdc";
        case 0x40: return "memext";
        case 0x41: return "rd";
        default: return "unknown";
    }
}


/** @brief Jméno kategorie Event Vieweru (en_EVENTLOG_CATEGORY). */
static const char *ev_cat_name ( unsigned v )
{
    static const char *const names[] = {
        "cpu_int", "cpu_pin_edge", "irq_ack_im2", "iorq_in", "iorq_out",
        "mmio_r", "mmio_w", "gdg_mode", "gdg_banking", "gdg_hwscroll",
        "gdg_colors", "gdg_video", "pio8255", "ctc8253", "pioz80", "psg",
        "fdc", "memext", "bp_fire", "user_mark", "cpu_ctrl", "gdg_wfrf",
        "qd", "rd", "sys"
    };
    return ( v < G_N_ELEMENTS ( names ) ) ? names[ v ] : "unknown";
}


/**
 * @brief Prošlý záznam stránky (mezi skenem a serializací).
 */
typedef struct st_TRACE_HIT
{
    uint64_t seq;        /**< seq záznamu. */
    bool     has_t;      /**< Má čas. */
    uint64_t t;          /**< Čas v taktech GDG. */
    uint8_t  rec[ TRACE_MAX_RECORD_SIZE ]; /**< Kopie záznamu (nejvýš TRACE_MAX_RECORD_SIZE B). */
    bool     v3;         /**< cputrack: záznam formátu v3 (32 B s registry). */
    bool     has_prev;   /**< cputrack v3 se stavem před: prev platí. */
    uint8_t  prev[ CPUTRACK_RECORD_SIZE_REGS ]; /**< Předchozí záznam (nese stav před tímto). */
} st_TRACE_HIT;


/**
 * @brief Hodnota registru ze záznamu cputrack v3.
 *
 * @param r    Záznam v3 (32 B).
 * @param idx  TRACE_CPUTRACK_REG_*.
 */
static unsigned cputrack_reg_value ( const uint8_t *r, unsigned idx )
{
    if ( idx <= TRACE_CPUTRACK_REG_WZ ) return rd16 ( r + CPUTRACK_V3_OFF_REGS + 2u * idx );
    uint8_t f = r[ CPUTRACK_V3_OFF_FLAGS ];
    switch ( idx ) {
        case TRACE_CPUTRACK_REG_I:      return r[ CPUTRACK_V3_OFF_I ];
        case TRACE_CPUTRACK_REG_R:      return r[ CPUTRACK_V3_OFF_R ];
        case TRACE_CPUTRACK_REG_IFF1:   return ( f & CPUTRACK_REGS_F_IFF1 ) ? 1u : 0u;
        case TRACE_CPUTRACK_REG_IFF2:   return ( f & CPUTRACK_REGS_F_IFF2 ) ? 1u : 0u;
        case TRACE_CPUTRACK_REG_IM:     return ( f & CPUTRACK_REGS_F_IM_MASK ) >> CPUTRACK_REGS_F_IM_SHIFT;
        case TRACE_CPUTRACK_REG_HALTED: return ( f & CPUTRACK_REGS_F_HALTED ) ? 1u : 0u;
        default:                        return 0;
    }
}


/**
 * @brief Záznam cputrack v3, jehož stav ukazují sloupce a filtr registrů.
 *
 * @param r        Záznam.
 * @param prev     Předchozí záznam (NULL = neznámý).
 * @param before   true = stav před záznamem (= stav v předchozím záznamu).
 * @return Záznam se stavem, nebo NULL (stav před není znám: první záznam,
 *         předchozí záznam nedostupný, příznak CPUTRACK_FLAG_NO_BEFORE).
 */
static const uint8_t *cputrack_regs_src ( const uint8_t *r, const uint8_t *prev, bool before )
{
    if ( !before ) return r;
    if ( !prev || ( r[ CPUTRACK_V3_OFF_KIND ] & CPUTRACK_FLAG_NO_BEFORE ) ) return NULL;
    return prev;
}


/** @brief Vyhovuje stav registrů @p src podmínkám filtru (NULL = neznámý = ne)? */
static bool cputrack_regs_match ( const uint8_t *src, const st_TRACE_FILTER *f )
{
    if ( f->n_regs == 0 ) return true;
    if ( !src ) return false;
    for ( unsigned i = 0; i < f->n_regs; i++ ) {
        unsigned v = cputrack_reg_value ( src, f->regs[ i ].reg );
        if ( v < f->regs[ i ].lo || v > f->regs[ i ].hi ) return false;
    }
    return true;
}


/** @brief Přidat hex16 řetězec ("0E9A") do pole. */
static void add_hex16 ( JsonArray *a, unsigned v )
{
    char b[ 8 ];
    g_snprintf ( b, sizeof ( b ), "%04X", v & 0xFFFFu );
    json_array_add_string_element ( a, b );
}


/** @brief Přidat hex8 řetězec ("5A") do pole. */
static void add_hex8 ( JsonArray *a, unsigned v )
{
    char b[ 4 ];
    g_snprintf ( b, sizeof ( b ), "%02X", v & 0xFFu );
    json_array_add_string_element ( a, b );
}


/** @brief Přidat číslo zaokrouhlené na `dec` desetinných míst. */
static void add_round ( JsonArray *a, double v, int dec )
{
    double m = pow ( 10.0, dec );
    json_array_add_double_element ( a, floor ( v * m + 0.5 ) / m );
}


/** @brief Přidat dvojici [L, R] uint16 ze záznamu, nebo null (příznak neplatí). */
static void add_lr16 ( JsonArray *a, const uint8_t *p, bool valid )
{
    if ( !valid ) {
        json_array_add_null_element ( a );
        return;
    }
    JsonArray *lr = json_array_new ( );
    json_array_add_int_element ( lr, rd16 ( p ) );
    json_array_add_int_element ( lr, rd16 ( p + 2 ) );
    json_array_add_array_element ( a, lr );
}


/**
 * @brief Řádek kanálu audio_* (paměť zachycení zvuku).
 *
 * Rozložení záznamů viz audio_capture.h (st_AUDIO_CAPTURE_*_REC): u všech
 * je na offsetu 0 absolutní čas (uint64) a na offsetu 8 snímek (uint32).
 */
static JsonArray *trace_row_audio ( const st_TRACE_PAGE_REQ *req, const st_TRACE_HIT *h )
{
    JsonArray *a = json_array_new ( );
    const uint8_t *r = h->rec;
    uint32_t mask = req->col_mask;
#define COL(i) ( mask & ( 1u << ( i ) ) )
    uint64_t t = rd64 ( r );
    if ( COL ( 0 ) ) json_array_add_int_element ( a, (gint64) h->seq );
    if ( COL ( 1 ) ) json_array_add_int_element ( a, (gint64) t );
    if ( COL ( 2 ) ) {
        double us = ( req->gdg_hz && t >= req->audio_origin )
                    ? (double) ( t - req->audio_origin ) * 1e6 / (double) req->gdg_hz : 0.0;
        add_round ( a, us, 3 );
    }
    if ( req->kind == TRACE_KIND_AUDIO_LEVELS ) {
        if ( COL ( 3 ) ) json_array_add_int_element ( a, rd32 ( r + 8 ) );
        if ( COL ( 4 ) ) json_array_add_string_element ( a, trace_audio_channel_name ( r[ 12 ] ) );
        if ( COL ( 5 ) ) json_array_add_int_element ( a, r[ 13 ] );
    } else if ( req->kind == TRACE_KIND_AUDIO_PSG_WRITES ) {
        static const char *const regs[ 4 ] = { "tone_lo", "tone", "volume", "noise" };
        unsigned reg = r[ 17 ];
        unsigned val = rd16 ( r + 12 );
        if ( COL ( 3 ) ) json_array_add_int_element ( a, rd32 ( r + 8 ) );
        if ( COL ( 4 ) ) json_array_add_int_element ( a, r[ 14 ] );
        if ( COL ( 5 ) ) add_hex8 ( a, r[ 15 ] );
        if ( COL ( 6 ) ) json_array_add_int_element ( a, r[ 16 ] );
        if ( COL ( 7 ) ) json_array_add_string_element ( a, ( reg < 4 ) ? regs[ reg ] : "?" );
        if ( COL ( 8 ) ) json_array_add_int_element ( a, val );
        /* Hz a nota jen u nového děliče tónu (stejný vzorec jako PSG Audio Scope). */
        double hz = ( reg == 1 ) ? audio_psg_tone_hz ( val, req->gdg_hz, req->cpu_divider ) : 0.0;
        if ( COL ( 9 ) ) {
            if ( hz > 0.0 ) add_round ( a, hz, 2 );
            else json_array_add_null_element ( a );
        }
        if ( COL ( 10 ) ) {
            int pitch = 0, cents = 0;
            if ( hz > 0.0 && audio_hz_to_midi ( hz, &pitch, &cents ) ) {
                char nm[ 8 ], b[ 24 ];
                audio_midi_note_name ( pitch, nm, sizeof ( nm ) );
                if ( cents ) g_snprintf ( b, sizeof ( b ), "%s%+dc", nm, cents );
                else g_strlcpy ( b, nm, sizeof ( b ) );
                json_array_add_string_element ( a, b );
            } else {
                json_array_add_null_element ( a );
            }
        }
    } else {
        /* audio_frames: t(0) frame(8) len(12) ch[9] x 16 B (16..159), ref_rms(160)
         * ref_peak(164) sdl_rms(168) sdl_peak(172) ref_samples(176) sdl_samples(180)
         * sdl_blocks(184) flags(186). */
        uint32_t len = rd32 ( r + 12 );
        unsigned nch = req->audio_channels;
        if ( nch == 0 || nch > 9 ) nch = 9;
        uint8_t flags = r[ 186 ];
        if ( COL ( 3 ) ) json_array_add_int_element ( a, len );
        for ( unsigned col = 4; col <= 7; col++ ) {
            if ( !COL ( col ) ) continue;
            JsonArray *v = json_array_new ( );
            for ( unsigned i = 0; i < nch; i++ ) {
                const uint8_t *c = r + 16 + i * 16u;
                if ( col == 4 ) json_array_add_int_element ( v, rd32 ( c ) );
                else if ( col == 5 ) add_round ( v, len ? 100.0 * rd32 ( c + 4 ) / len : 0.0, 1 );
                else if ( col == 6 ) add_round ( v, len ? sqrt ( (double) rd32 ( c + 8 ) / len ) : 0.0, 2 );
                else json_array_add_int_element ( v, c[ 12 ] );
            }
            json_array_add_array_element ( a, v );
        }
        if ( COL ( 8 ) ) add_lr16 ( a, r + 160, flags & 1u );
        if ( COL ( 9 ) ) add_lr16 ( a, r + 164, flags & 1u );
        if ( COL ( 10 ) ) add_lr16 ( a, r + 168, flags & 2u );
        if ( COL ( 11 ) ) add_lr16 ( a, r + 172, flags & 2u );
        if ( COL ( 12 ) ) {
            if ( flags & 2u ) json_array_add_int_element ( a, rd16 ( r + 184 ) );
            else json_array_add_null_element ( a );
        }
    }
#undef COL
    return a;
}


/**
 * @brief Sestavit řádek ze záznamu podle masky sloupců.
 *
 * @param req    Požadavek stránky (druh kanálu, maska sloupců, kontext audio_*).
 * @param extra  Symbol (cputrack) nebo jméno značky (marklog), NULL = "".
 */
/** @brief Řádek kanálu ramlog (rozložení st_RAMLOG_REC, viz ramlog.h). */
static JsonArray *trace_row_ramlog ( const st_TRACE_PAGE_REQ *req, const st_TRACE_HIT *h,
                                     const char *extra )
{
    JsonArray *a = json_array_new ( );
    const uint8_t *r = h->rec;
    uint32_t mask = req->col_mask;
    uint8_t flags = r[ 25 ];
#define COL(i) ( mask & ( 1u << ( i ) ) )
    if ( COL ( 0 ) ) json_array_add_int_element ( a, (gint64) h->seq );
    if ( COL ( 1 ) ) json_array_add_int_element ( a, (gint64) rd64 ( r ) );
    if ( COL ( 2 ) ) json_array_add_int_element ( a, rd32 ( r + 8 ) );
    if ( COL ( 3 ) ) add_hex16 ( a, rd16 ( r + 12 ) );
    if ( COL ( TRACE_COL_RAMLOG_SYM ) ) json_array_add_string_element ( a, extra ? extra : "" );
    if ( COL ( 5 ) ) {
        if ( flags & RAMLOG_FLAG_NO_CPU ) json_array_add_null_element ( a );
        else add_hex16 ( a, rd16 ( r + 14 ) );
    }
    if ( COL ( 6 ) ) json_array_add_string_element ( a, ramlog_space_name ( r[ 20 ] ) );
    if ( COL ( 7 ) ) json_array_add_int_element ( a, rd32 ( r + 16 ) );
    if ( COL ( 8 ) ) {
        if ( flags & RAMLOG_FLAG_PLANE ) json_array_add_int_element ( a, r[ 26 ] );
        else json_array_add_null_element ( a );
    }
    if ( COL ( 9 ) ) add_hex8 ( a, r[ 21 ] );
    if ( COL ( 10 ) ) add_hex8 ( a, r[ 22 ] );
    if ( COL ( 11 ) ) json_array_add_string_element ( a, ramlog_src_name ( r[ 23 ] ) );
    if ( COL ( 12 ) ) {
        static const char *const fn[] = { "unchanged", "suppressed", "summary", "plane", "no_cpu", "no_map" };
        char b[ 80 ];
        b[ 0 ] = 0;
        for ( unsigned i = 0; i < G_N_ELEMENTS ( fn ); i++ ) {
            if ( !( flags & ( 1u << i ) ) ) continue;
            if ( b[ 0 ] ) g_strlcat ( b, "|", sizeof ( b ) );
            g_strlcat ( b, fn[ i ], sizeof ( b ) );
        }
        json_array_add_string_element ( a, b );
    }
    if ( COL ( 13 ) ) {
        uint16_t c = rd16 ( r + 28 );
        json_array_add_int_element ( a, ( ( flags & RAMLOG_FLAG_SUMMARY ) && c == 0 ) ? 65536 : ( c ? c : 1 ) );
    }
    if ( COL ( 14 ) ) {
        if ( r[ 23 ] == RAMLOG_SRC_INSN ) json_array_add_int_element ( a, r[ 24 ] );
        else json_array_add_null_element ( a );
    }
#undef COL
    return a;
}


/**
 * @brief Přidá do řádku řetězec z pole záznamu pevné délky jako platné UTF-8.
 *
 * Text akce BP může obsahovat libovolné bajty (`%c`, ořez uprostřed
 * vícebajtového znaku); neplatné sekvence nahradí U+FFFD.
 */
static void add_fixed_str ( JsonArray *a, const uint8_t *p, size_t size )
{
    size_t n = 0;
    while ( n < size && p[ n ] ) n++;
    char *v = g_utf8_make_valid ( (const char *) p, (gssize) n );
    json_array_add_string_element ( a, v );
    g_free ( v );
}


/** @brief Řádek kanálu bplog (rozložení st_BPLOG_REC, viz bplog.h). */
static JsonArray *trace_row_bplog ( const st_TRACE_PAGE_REQ *req, const st_TRACE_HIT *h,
                                    const char *extra )
{
    JsonArray *a = json_array_new ( );
    const uint8_t *r = h->rec;
    uint32_t mask = req->col_mask;
    uint8_t flags = r[ 21 ];
#define COL(i) ( mask & ( 1u << ( i ) ) )
    if ( COL ( 0 ) ) json_array_add_int_element ( a, (gint64) h->seq );
    if ( COL ( 1 ) ) json_array_add_int_element ( a, (gint64) rd64 ( r ) );
    if ( COL ( 2 ) ) json_array_add_int_element ( a, rd32 ( r + 8 ) );
    if ( COL ( 3 ) ) add_hex16 ( a, rd16 ( r + 12 ) );
    if ( COL ( TRACE_COL_BPLOG_SYM ) ) json_array_add_string_element ( a, extra ? extra : "" );
    if ( COL ( 5 ) ) add_hex16 ( a, rd16 ( r + 14 ) );
    if ( COL ( 6 ) ) json_array_add_int_element ( a, (int32_t) rd32 ( r + 16 ) );
    if ( COL ( 7 ) ) add_fixed_str ( a, r + 24, BPLOG_NAME_SIZE );
    if ( COL ( 8 ) ) json_array_add_string_element ( a, bplog_kind_name ( r[ 20 ] ) );
    if ( COL ( 9 ) ) add_fixed_str ( a, r + 56, BPLOG_TEXT_SIZE );
    if ( COL ( 10 ) ) {
        const char *fl = ( flags & BPLOG_FLAG_TEXT_TRUNCATED )
                         ? ( ( flags & BPLOG_FLAG_NAME_TRUNCATED ) ? "text_truncated|name_truncated" : "text_truncated" )
                         : ( ( flags & BPLOG_FLAG_NAME_TRUNCATED ) ? "name_truncated" : "" );
        json_array_add_string_element ( a, fl );
    }
#undef COL
    return a;
}


static JsonArray *trace_row ( const st_TRACE_PAGE_REQ *req, const st_TRACE_HIT *h,
                              const char *extra )
{
    en_TRACE_KIND kind = req->kind;
    uint32_t mask = req->col_mask;
    if ( kind == TRACE_KIND_RAMLOG ) return trace_row_ramlog ( req, h, extra );
    if ( kind == TRACE_KIND_BPLOG ) return trace_row_bplog ( req, h, extra );
    if ( kind >= TRACE_KIND_AUDIO_PSG_WRITES ) return trace_row_audio ( req, h );
    JsonArray *a = json_array_new ( );
    const uint8_t *r = h->rec;
#define COL(i) ( mask & ( 1u << ( i ) ) )
    if ( kind == TRACE_KIND_CPUTRACK ) {
        uint8_t len = r[ 2 ];
        /* v2: insn_length 0 = přijetí přerušení (pc = adresa obsluhy);
         * v3: 0xFE = synchronizační záznam (bajty 3-10 = alternativní sada) */
        bool irq = ( len == 0 );
        bool sync = ( len == CPUTRACK_INSN_LEN_SYNC );
        bool v3 = h->v3;
        if ( sync ) len = 0;
        if ( len > 4 ) len = 4;
        if ( COL ( 0 ) ) json_array_add_int_element ( a, (gint64) h->seq );
        if ( COL ( 1 ) ) add_hex16 ( a, rd16 ( r ) );
        if ( COL ( 2 ) ) {
            char *hx = hex_bytes ( r + 3, len );
            json_array_add_string_element ( a, hx );
            g_free ( hx );
        }
        if ( COL ( 3 ) && irq ) {
            json_array_add_string_element ( a, "INT ACK" );
        } else if ( COL ( 3 ) && sync ) {
            static const char *const reasons[] = { "SYNC start", "SYNC pc_range", "SYNC write" };
            unsigned why = ( r[ CPUTRACK_V3_OFF_KIND ] & CPUTRACK_SYNC_REASON_MASK ) >> CPUTRACK_SYNC_REASON_SHIFT;
            json_array_add_string_element ( a, ( why < G_N_ELEMENTS ( reasons ) ) ? reasons[ why ] : "SYNC" );
        } else if ( COL ( 3 ) ) {
            st_DASM_REC d = { rd16 ( r ), r + 3 };
            z80_dasm_inst_t inst;
            char buf[ 64 ];
            z80_dasm ( &inst, trace_dasm_read, &d, d.pc );
            z80_dasm_to_str ( buf, (int) sizeof ( buf ), &inst, NULL );
            json_array_add_string_element ( a, buf );
        }
        if ( COL ( 4 ) ) json_array_add_int_element ( a, sync ? 0 : r[ 11 ] );
        if ( COL ( 5 ) ) json_array_add_int_element ( a, sync ? 0 : (gint64) rd32 ( r + 7 ) );
        if ( COL ( TRACE_COL_CPUTRACK_T ) ) {
            if ( h->has_t ) json_array_add_int_element ( a, (gint64) h->t );
            else json_array_add_null_element ( a );
        }
        if ( COL ( TRACE_COL_CPUTRACK_SYM ) ) json_array_add_string_element ( a, extra ? extra : "" );
        if ( COL ( TRACE_COL_CPUTRACK_KIND ) ) {
            static const char *const kinds[] = { "insn", "loop", "int", "sync" };
            unsigned k;
            if ( v3 ) k = r[ CPUTRACK_V3_OFF_KIND ] & CPUTRACK_KIND_MASK;
            else k = irq ? CPUTRACK_KIND_INT : ( r[ 11 ] == 0 ? CPUTRACK_KIND_LOOP : CPUTRACK_KIND_INSN );
            json_array_add_string_element ( a, ( k < G_N_ELEMENTS ( kinds ) ) ? kinds[ k ] : "?" );
        }
        if ( mask & TRACE_CPUTRACK_REG_COLS_MASK ) {
            const uint8_t *src = v3 ? cputrack_regs_src ( r, h->has_prev ? h->prev : NULL, req->regs_before ) : NULL;
            for ( unsigned i = 0; i < TRACE_CPUTRACK_REG_COUNT; i++ ) {
                if ( !COL ( TRACE_COL_CPUTRACK_REG0 + i ) ) continue;
                if ( !src ) {
                    json_array_add_null_element ( a );
                    continue;
                }
                unsigned v = cputrack_reg_value ( src, i );
                if ( i <= TRACE_CPUTRACK_REG_WZ ) add_hex16 ( a, v );
                else if ( i <= TRACE_CPUTRACK_REG_R ) add_hex8 ( a, v );
                else json_array_add_int_element ( a, (gint64) v );
            }
        }
        return a;
    }
    /* 24B kanály: seq, t, frame, pos společné. */
    if ( COL ( 0 ) ) json_array_add_int_element ( a, (gint64) h->seq );
    if ( COL ( 1 ) ) json_array_add_int_element ( a, (gint64) rd64 ( r ) );
    if ( COL ( 2 ) ) json_array_add_int_element ( a, rd32 ( r + 8 ) );
    if ( COL ( 3 ) ) json_array_add_int_element ( a, rd32 ( r + 12 ) );
    switch ( kind ) {
        case TRACE_KIND_IORQLOG:
            if ( COL ( 4 ) ) json_array_add_string_element ( a, iorq_type_name ( r[ 16 ] ) );
            if ( COL ( 5 ) ) json_array_add_string_element ( a, r[ 17 ] ? "out" : "in" );
            if ( COL ( 6 ) ) add_hex16 ( a, rd16 ( r + 18 ) );
            if ( COL ( 7 ) ) add_hex16 ( a, rd16 ( r + 20 ) );
            if ( COL ( 8 ) ) add_hex8 ( a, r[ 22 ] );
            if ( COL ( 9 ) ) json_array_add_int_element ( a, r[ 23 ] );
            break;
        case TRACE_KIND_INTLOG:
            if ( COL ( 4 ) ) json_array_add_string_element ( a, int_class_name ( r[ 16 ] ) );
            if ( COL ( 5 ) ) json_array_add_string_element ( a, int_chip_name ( r[ 17 ] ) );
            if ( COL ( 6 ) ) json_array_add_int_element ( a, r[ 18 ] );
            if ( COL ( 7 ) ) json_array_add_string_element ( a, int_edge_name ( r[ 19 ] ) );
            if ( COL ( 8 ) ) {
                char b[ 12 ];
                g_snprintf ( b, sizeof ( b ), "%08X", rd32 ( r + 20 ) );
                json_array_add_string_element ( a, b );
            }
            break;
        case TRACE_KIND_HWLOG:
            if ( COL ( 4 ) ) json_array_add_string_element ( a, hw_chip_name ( r[ 16 ] ) );
            if ( COL ( 5 ) ) json_array_add_int_element ( a, r[ 17 ] );
            if ( COL ( 6 ) ) {
                char *hx = hex_bytes ( r + 18, 6 );
                json_array_add_string_element ( a, hx );
                g_free ( hx );
            }
            break;
        case TRACE_KIND_MARKLOG:
            if ( COL ( 4 ) ) json_array_add_int_element ( a, rd16 ( r + 16 ) );
            if ( COL ( TRACE_COL_MARKLOG_NAME ) ) json_array_add_string_element ( a, extra ? extra : "" );
            break;
        case TRACE_KIND_EVENTLOG:
            if ( COL ( 4 ) ) json_array_add_string_element ( a, ev_cat_name ( r[ 16 ] ) );
            if ( COL ( 5 ) ) json_array_add_int_element ( a, r[ 17 ] );
            if ( COL ( 6 ) ) add_hex16 ( a, rd16 ( r + 18 ) );
            if ( COL ( TRACE_COL_EVENTLOG_TEXT ) ) json_array_add_string_element ( a, extra ? extra : "" );
            break;
        default:
            break;
    }
#undef COL
    return a;
}


/** @brief Délka JSON serializace pole (kompaktní). */
static size_t json_array_chars ( JsonArray *a )
{
    JsonNode *n = json_node_new ( JSON_NODE_ARRAY );
    json_node_set_array ( n, a );
    char *s = json_to_string ( n, FALSE );
    size_t len = strlen ( s );
    g_free ( s );
    json_node_unref ( n );
    return len;
}


/** @brief Porovnání uint16 pro qsort a bsearch. */
static int cmp_u16 ( const void *a, const void *b )
{
    return (int) *(const uint16_t *) a - (int) *(const uint16_t *) b;
}

/** @brief Rezerva znaků na hlavičku odpovědi (stav kanálu, kurzor, sloupce). */
#define TRACE_PAGE_HEADER_CHARS 900u


int trace_reader_page ( const st_TRACE_PAGE_REQ *req,
                        const st_TLOG_READ_SNAPSHOT *snap,
                        trace_resolve_fn resolve, void *user,
                        JsonObject *out, uint64_t *out_next )
{
    size_t rs = snap->record_size;
    uint64_t seq = req->from_seq;
    if ( seq > snap->next_seq ) seq = snap->next_seq;
    *out_next = seq;
    if ( rs == 0 || rs > TRACE_MAX_RECORD_SIZE ) return -1;

    /* --- raw_b64: souvislé surové záznamy bez filtru --- */
    if ( req->raw ) {
        uint64_t want = req->limit ? req->limit : 1;
        uint64_t cap = TRACE_READ_MAX_RAW_BYTES / rs;
        if ( want > cap ) want = cap;
        uint8_t *buf = (uint8_t *) g_malloc ( (size_t) ( want * rs ) + 1 );
        uint64_t got = 0;
        if ( trace_reader_fetch ( snap, seq, want, buf, &got ) != 0 ) {
            g_free ( buf );
            return -1;
        }
        char *b64 = g_base64_encode ( buf, (gsize) ( got * rs ) );
        json_object_set_int_member ( out, "record_size", (gint64) rs );
        json_object_set_int_member ( out, "from_seq", (gint64) seq );
        json_object_set_int_member ( out, "count", (gint64) got );
        json_object_set_string_member ( out, "data_b64", b64 );
        json_object_set_int_member ( out, "scanned", (gint64) got );
        json_object_set_int_member ( out, "matched", (gint64) got );
        json_object_set_boolean_member ( out, "scan_exhausted", FALSE );
        json_object_set_boolean_member ( out, "page_full", seq + got < snap->next_seq && got == want );
        json_object_set_boolean_member ( out, "end_of_data", seq + got >= snap->next_seq );
        g_free ( b64 );
        g_free ( buf );
        *out_next = seq + got;
        return 0;
    }

    /* --- rows: sken s filtrem, pak symboly, pak rozpočet znaků --- */
    unsigned limit = req->limit ? req->limit : 1;
    st_TRACE_HIT *hits = (st_TRACE_HIT *) g_malloc ( limit * sizeof ( *hits ) );
    size_t nhits = 0;
    uint64_t scanned = 0;
    bool data_gap = false;   /* další záznamy nejsou ve snímku (kopie RAM došla) */
    st_TRACE_TIME tm = { 0 };
    tm.on = ( req->kind == TRACE_KIND_CPUTRACK ) && req->cputrack_time;
    if ( tm.on ) trace_time_locate ( &tm, snap, req, seq );

    /* cputrack v3 se stavem před: předchozí záznam (i ten před stránkou). */
    bool track_prev = ( req->kind == TRACE_KIND_CPUTRACK ) && ( rs == CPUTRACK_RECORD_SIZE_REGS ) &&
                      req->regs_before;
    uint8_t prev[ CPUTRACK_RECORD_SIZE_REGS ];
    bool has_prev = false;
    if ( track_prev && seq > snap->first_seq ) {
        uint64_t got_prev = 0;
        has_prev = ( trace_reader_fetch ( snap, seq - 1, 1, prev, &got_prev ) == 0 ) && got_prev == 1;
    }

    enum { BATCH = 4096 };
    uint8_t *buf = (uint8_t *) g_malloc ( BATCH * rs );
    int rc = 0;
    while ( seq < snap->next_seq && nhits < limit && scanned < req->max_scan ) {
        uint64_t want = snap->next_seq - seq;
        if ( want > BATCH ) want = BATCH;
        if ( want > req->max_scan - scanned ) want = req->max_scan - scanned;
        uint64_t got = 0;
        if ( trace_reader_fetch ( snap, seq, want, buf, &got ) != 0 ) {
            rc = -1;
            break;
        }
        if ( got == 0 ) {
            data_gap = true;
            break;
        }
        uint64_t k = 0;
        for ( ; k < got && nhits < limit; k++ ) {
            const uint8_t *r = buf + k * rs;
            uint64_t s = seq + k;
            bool has_t = false;
            uint64_t t = 0;
            if ( req->kind == TRACE_KIND_CPUTRACK ) {
                if ( tm.on ) {
                    if ( tm.ok && s >= tm.chunk_end ) trace_time_locate ( &tm, snap, req, s );
                    if ( tm.ok ) {
                        has_t = true;
                        t = tm.anchor_px + tm.prefix * req->cpu_divider;
                        tm.prefix += cputrack_rec_t ( r );
                    }
                }
            } else {
                has_t = true;
                t = rd64 ( r );
            }
            bool ok = trace_match ( req->kind, r, has_t, t, &req->filter );
            if ( ok && req->filter.n_regs ) {
                const uint8_t *src = ( rs == CPUTRACK_RECORD_SIZE_REGS )
                                     ? cputrack_regs_src ( r, has_prev ? prev : NULL, req->regs_before ) : NULL;
                ok = cputrack_regs_match ( src, &req->filter );
            }
            if ( ok ) {
                st_TRACE_HIT *h = &hits[ nhits++ ];
                h->seq = s;
                h->has_t = has_t;
                h->t = t;
                memset ( h->rec, 0, sizeof ( h->rec ) );
                memcpy ( h->rec, r, rs );
                h->v3 = ( req->kind == TRACE_KIND_CPUTRACK ) && ( rs == CPUTRACK_RECORD_SIZE_REGS );
                h->has_prev = track_prev && has_prev;
                if ( h->has_prev ) memcpy ( h->prev, prev, sizeof ( prev ) );
            }
            if ( track_prev ) {
                memcpy ( prev, r, sizeof ( prev ) );
                has_prev = true;
            }
        }
        scanned += k;
        seq += k;
    }
    g_free ( buf );
    if ( rc != 0 ) {
        g_free ( hits );
        return -1;
    }

    /* Symboly (cputrack), jména značek (marklog) a texty událostí
     * (eventlog) - smí je číst jen emu vlákno, doplní je resolver. */
    bool want_sym = ( ( req->kind == TRACE_KIND_CPUTRACK ) && ( req->col_mask & ( 1u << TRACE_COL_CPUTRACK_SYM ) ) )
                    || ( ( req->kind == TRACE_KIND_RAMLOG ) && ( req->col_mask & ( 1u << TRACE_COL_RAMLOG_SYM ) ) )
                    || ( ( req->kind == TRACE_KIND_BPLOG ) && ( req->col_mask & ( 1u << TRACE_COL_BPLOG_SYM ) ) );
    /* Offset adresy pro symbol: cputrack PC na 0, ramlog a bplog insn_pc na 12. */
    size_t sym_off = ( req->kind == TRACE_KIND_RAMLOG || req->kind == TRACE_KIND_BPLOG ) ? 12u : 0u;
    bool want_mark = ( req->kind == TRACE_KIND_MARKLOG ) && ( req->col_mask & ( 1u << TRACE_COL_MARKLOG_NAME ) );
    bool want_text = ( req->kind == TRACE_KIND_EVENTLOG ) && ( req->col_mask & ( 1u << TRACE_COL_EVENTLOG_TEXT ) );
    uint16_t *keys = NULL;
    size_t nkeys = 0;
    char *names = NULL;
    uint32_t *offs = NULL;
    uint8_t *events = NULL;
    char *texts = NULL;
    if ( ( want_sym || want_mark || want_text ) && nhits > 0 && resolve ) {
        st_TRACE_RESOLVE_REQ rr;
        memset ( &rr, 0, sizeof ( rr ) );
        if ( want_sym || want_mark ) {
            keys = (uint16_t *) g_malloc ( nhits * sizeof ( uint16_t ) );
            for ( size_t i = 0; i < nhits; i++ ) keys[ i ] = want_sym ? rd16 ( hits[ i ].rec + sym_off ) : rd16 ( hits[ i ].rec + 16 );
            qsort ( keys, nhits, sizeof ( uint16_t ), cmp_u16 );
            for ( size_t i = 0; i < nhits; i++ ) {
                if ( nkeys == 0 || keys[ nkeys - 1 ] != keys[ i ] ) keys[ nkeys++ ] = keys[ i ];
            }
            names = (char *) g_malloc0 ( nkeys * TRACE_RESOLVE_NAME_SIZE );
            offs = (uint32_t *) g_malloc0 ( nkeys * sizeof ( uint32_t ) );
            if ( want_sym ) {
                rr.addrs = keys; rr.n_addrs = nkeys; rr.syms = names; rr.sym_offs = offs;
            } else {
                rr.markers = keys; rr.n_markers = nkeys; rr.marker_names = names;
            }
        } else {
            events = (uint8_t *) g_malloc ( nhits * TRACE_EVENTLOG_RECORD_SIZE );
            for ( size_t i = 0; i < nhits; i++ ) memcpy ( events + i * TRACE_EVENTLOG_RECORD_SIZE, hits[ i ].rec, TRACE_EVENTLOG_RECORD_SIZE );
            texts = (char *) g_malloc0 ( nhits * TRACE_RESOLVE_TEXT_SIZE );
            rr.events = events; rr.n_events = nhits; rr.event_texts = texts;
        }
        if ( !resolve ( user, &rr ) ) {
            if ( names ) memset ( names, 0, nkeys * TRACE_RESOLVE_NAME_SIZE );
            if ( texts ) memset ( texts, 0, nhits * TRACE_RESOLVE_TEXT_SIZE );
        }
    }

    /* Serializace s rozpočtem znaků. */
    JsonArray *rows = json_array_new ( );
    size_t budget = req->max_chars;
    size_t used = TRACE_PAGE_HEADER_CHARS;
    size_t emitted = 0;
    for ( ; emitted < nhits; emitted++ ) {
        const st_TRACE_HIT *h = &hits[ emitted ];
        char extra[ TRACE_RESOLVE_NAME_SIZE + 16 ];
        const char *ex = NULL;
        if ( names ) {
            uint16_t key = want_sym ? rd16 ( h->rec + sym_off ) : rd16 ( h->rec + 16 );
            uint16_t *f = (uint16_t *) bsearch ( &key, keys, nkeys, sizeof ( uint16_t ), cmp_u16 );
            if ( f ) {
                size_t ki = (size_t) ( f - keys );
                const char *nm = names + ki * TRACE_RESOLVE_NAME_SIZE;
                if ( want_sym && nm[ 0 ] && offs[ ki ] ) {
                    g_snprintf ( extra, sizeof ( extra ), "%s+%u", nm, (unsigned) offs[ ki ] );
                    ex = extra;
                } else {
                    ex = nm;
                }
            }
        } else if ( texts ) {
            ex = texts + emitted * TRACE_RESOLVE_TEXT_SIZE;
        }
        JsonArray *row = trace_row ( req, h, ex );
        size_t len = json_array_chars ( row ) + 1;
        if ( emitted > 0 && used + len > budget ) {
            json_array_unref ( row );
            break;
        }
        used += len;
        json_array_add_array_element ( rows, row );
    }

    /* Sloupce v pořadí masky (bez t u cputrack bez času). */
    JsonArray *cols = json_array_new ( );
    size_t nc = 0;
    const char *const *cn = trace_columns ( req->kind, &nc );
    for ( size_t i = 0; i < nc; i++ ) {
        if ( req->col_mask & ( 1u << i ) ) json_array_add_string_element ( cols, cn[ i ] );
    }
    json_object_set_array_member ( out, "columns", cols );
    json_object_set_array_member ( out, "rows", rows );

    bool chars_cut = emitted < nhits;
    uint64_t next = chars_cut ? hits[ emitted ].seq : seq;

    /* Cache času pro další stránku (jen chunky na disku): stav na `next`. */
    if ( tm.on && tm.ok && !tm.in_ram ) {
        if ( !chars_cut ) {
            trace_time_cache_store ( snap->segment, tm.chunk_start, seq, tm.prefix );
        } else if ( hits[ emitted ].has_t && next >= tm.chunk_start && next < tm.chunk_end &&
                    req->cpu_divider ) {
            trace_time_cache_store ( snap->segment, tm.chunk_start, next,
                                     ( hits[ emitted ].t - tm.anchor_px ) / req->cpu_divider );
        }
    }
    json_object_set_int_member ( out, "scanned", (gint64) scanned );
    json_object_set_int_member ( out, "matched", (gint64) emitted );
    json_object_set_boolean_member ( out, "scan_exhausted",
                                     !chars_cut && nhits < limit && !data_gap &&
                                     seq < snap->next_seq && scanned >= req->max_scan );
    json_object_set_boolean_member ( out, "page_full", chars_cut || ( nhits >= limit && next < snap->next_seq ) || data_gap );
    json_object_set_boolean_member ( out, "end_of_data", next >= snap->next_seq );

    g_free ( keys );
    g_free ( names );
    g_free ( offs );
    g_free ( events );
    g_free ( texts );
    g_free ( hits );
    *out_next = next;
    return 0;
}
