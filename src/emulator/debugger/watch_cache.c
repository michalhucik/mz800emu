/*
 * watch_cache.c - implementace per-row cache + snapshot baseline pro
 *                  Watch panel (Phase E).
 *
 * Pouziva GHashTable pro O(1) lookup po row_id (= klic int boxovany
 * pres GINT_TO_POINTER). Storage je heap-allocated struktur (drzi
 * cache).
 *
 * Threading: single-threaded UI vlakno. Bez locku.
 *
 * Licence: GPL-3.0-or-later
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>

#include "watch_cache.h"


/* Privatni heap-alokovana wrapper struktura (= hashtable value). */
typedef struct st_WATCH_CACHE_ENTRY {
    st_WATCH_CACHE_VIEW view;
    uint8_t prev_bytes[ WATCH_LENGTH_MAX_BYTES ];
} st_WATCH_CACHE_ENTRY;


/* Hashtable: int row_id -> st_WATCH_CACHE_ENTRY*. */
static GHashTable *s_cache = NULL;

/* Snapshot baseline (Phase E.2). */
static GHashTable *s_snapshot = NULL;
static bool s_snapshot_active = false;
static int s_snapshot_frame = 0;


/* ===================================================================== */
/*  Internal helpers                                                     */
/* ===================================================================== */


/**
 * @brief Allocator pro novou cache entry (zero-inicializovany view).
 */
static st_WATCH_CACHE_ENTRY *cache_entry_new ( void )
{
    st_WATCH_CACHE_ENTRY *e = g_new0 ( st_WATCH_CACHE_ENTRY, 1 );
    e->view.valid = false;
    e->view.delta_dir = WATCH_DELTA_NONE;
    return e;
}


/**
 * @brief Allocator pro snapshot entry.
 */
static st_WATCH_VALUE_SNAP *snap_new ( void )
{
    return g_new0 ( st_WATCH_VALUE_SNAP, 1 );
}


/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */


void watch_cache_init ( void )
{
    if ( s_cache ) return;
    s_cache = g_hash_table_new_full ( g_direct_hash, g_direct_equal,
                                        NULL, g_free );
    s_snapshot = g_hash_table_new_full ( g_direct_hash, g_direct_equal,
                                          NULL, g_free );
    s_snapshot_active = false;
    s_snapshot_frame = 0;
}


void watch_cache_shutdown ( void )
{
    if ( s_cache ) {
        g_hash_table_destroy ( s_cache );
        s_cache = NULL;
    };
    if ( s_snapshot ) {
        g_hash_table_destroy ( s_snapshot );
        s_snapshot = NULL;
    };
    s_snapshot_active = false;
    s_snapshot_frame = 0;
}


/* ===================================================================== */
/*  Sign-extend helper                                                   */
/* ===================================================================== */


int64_t watch_cache_value_to_signed ( en_WATCH_TYPE t, uint64_t v )
{
    switch ( t ) {
        case WATCH_TYPE_I8:
            return (int64_t) (int8_t) ( v & 0xFF );
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE:
            return (int64_t) (int16_t) ( v & 0xFFFF );
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE:
            return (int64_t) (int32_t) ( v & 0xFFFFFFFFu );
        default:
            /* Unsigned int + bit + string typy = vstupní bity. */
            return (int64_t) v;
    };
}


/* ===================================================================== */
/*  Cache update                                                         */
/* ===================================================================== */


/**
 * @brief Vytvori (nebo nahraje stávající) entry pro row_id.
 */
static st_WATCH_CACHE_ENTRY *cache_entry_lookup_or_create ( int row_id )
{
    if ( !s_cache ) return NULL;
    gpointer key = GINT_TO_POINTER ( row_id );
    st_WATCH_CACHE_ENTRY *e =
        (st_WATCH_CACHE_ENTRY *) g_hash_table_lookup ( s_cache, key );
    if ( !e ) {
        e = cache_entry_new ( );
        g_hash_table_insert ( s_cache, key, e );
    };
    return e;
}


void watch_cache_update ( int row_id,
                           en_WATCH_TYPE type,
                           en_WATCH_MODE mode,
                           uint64_t value_int,
                           const uint8_t *bytes,
                           size_t len,
                           int frame_no )
{
    if ( row_id < 0 || !s_cache ) return;

    st_WATCH_CACHE_ENTRY *e = cache_entry_lookup_or_create ( row_id );
    if ( !e ) return;

    /* Reset prev pri type/mode change. */
    if ( e->view.valid &&
         ( e->view.type_snap != (int) type ||
           e->view.mode_snap != (int) mode ) ) {
        e->view.valid = false;
        e->view.min_max_valid = false;
        e->view.change_count = 0;
        e->view.delta_dir = WATCH_DELTA_NONE;
    };

    bool is_int_type = ( type != WATCH_TYPE_ASCII &&
                          type != WATCH_TYPE_MZASCII &&
                          type != WATCH_TYPE_BYTES );

    if ( is_int_type ) {
        int64_t signed_v = watch_cache_value_to_signed ( type, value_int );

        if ( !e->view.valid ) {
            e->view.prev_int = value_int;
            e->view.delta_dir = WATCH_DELTA_NONE;
            e->view.last_change_frame = frame_no;
            e->view.min_int = signed_v;
            e->view.max_int = signed_v;
            e->view.change_count = 0;
            e->view.min_max_valid = true;
            e->view.prev_len = 0;
        } else {
            int64_t prev_signed =
                watch_cache_value_to_signed ( (en_WATCH_TYPE) e->view.type_snap,
                                                e->view.prev_int );
            if ( value_int != e->view.prev_int ) {
                e->view.delta_dir =
                    ( signed_v > prev_signed ) ? WATCH_DELTA_UP : WATCH_DELTA_DOWN;
                e->view.last_change_frame = frame_no;
                e->view.change_count++;
                e->view.prev_int = value_int;
            };
            if ( signed_v < e->view.min_int ) e->view.min_int = signed_v;
            if ( signed_v > e->view.max_int ) e->view.max_int = signed_v;
        };
    } else {
        /* String / bytes. */
        size_t to_copy = ( len > WATCH_LENGTH_MAX_BYTES )
                          ? WATCH_LENGTH_MAX_BYTES : len;

        if ( !e->view.valid ) {
            if ( bytes && to_copy > 0 ) {
                memcpy ( e->prev_bytes, bytes, to_copy );
            };
            e->view.prev_len = to_copy;
            e->view.delta_dir = WATCH_DELTA_NONE;
            e->view.last_change_frame = frame_no;
            e->view.change_count = 0;
            e->view.min_max_valid = false;
        } else {
            bool changed = ( to_copy != e->view.prev_len ) ||
                            ( to_copy > 0 && bytes &&
                              memcmp ( bytes, e->prev_bytes, to_copy ) != 0 );
            if ( changed ) {
                if ( bytes && to_copy > 0 ) {
                    memcpy ( e->prev_bytes, bytes, to_copy );
                };
                e->view.prev_len = to_copy;
                e->view.last_change_frame = frame_no;
                e->view.delta_dir = WATCH_DELTA_STRING;
                e->view.change_count++;
            };
        };
    };

    e->view.type_snap = (int) type;
    e->view.mode_snap = (int) mode;
    e->view.valid = true;
}


const st_WATCH_CACHE_VIEW *watch_cache_get ( int row_id )
{
    if ( !s_cache || row_id < 0 ) return NULL;
    st_WATCH_CACHE_ENTRY *e =
        (st_WATCH_CACHE_ENTRY *) g_hash_table_lookup ( s_cache,
                                                         GINT_TO_POINTER ( row_id ) );
    return e ? &e->view : NULL;
}


/**
 * @brief Vrati read access do prev_bytes pole entry (pro tests + UI
 *        ktere by chtelo srovnat byty mimo update).
 *
 * Lifetime stejny jako watch_cache_get.
 */
const uint8_t *watch_cache_get_prev_bytes ( int row_id, size_t *out_len )
{
    if ( !s_cache || row_id < 0 ) {
        if ( out_len ) *out_len = 0;
        return NULL;
    };
    st_WATCH_CACHE_ENTRY *e =
        (st_WATCH_CACHE_ENTRY *) g_hash_table_lookup ( s_cache,
                                                         GINT_TO_POINTER ( row_id ) );
    if ( !e ) {
        if ( out_len ) *out_len = 0;
        return NULL;
    };
    if ( out_len ) *out_len = e->view.prev_len;
    return e->prev_bytes;
}


void watch_cache_reset_one ( int row_id )
{
    if ( !s_cache || row_id < 0 ) return;
    st_WATCH_CACHE_ENTRY *e =
        (st_WATCH_CACHE_ENTRY *) g_hash_table_lookup ( s_cache,
                                                         GINT_TO_POINTER ( row_id ) );
    if ( !e ) return;
    e->view.valid = false;
    e->view.min_max_valid = false;
    e->view.change_count = 0;
    e->view.delta_dir = WATCH_DELTA_NONE;
    e->view.prev_len = 0;
}


void watch_cache_reset_all ( void )
{
    if ( !s_cache ) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init ( &it, s_cache );
    while ( g_hash_table_iter_next ( &it, &key, &val ) ) {
        st_WATCH_CACHE_ENTRY *e = (st_WATCH_CACHE_ENTRY *) val;
        e->view.valid = false;
        e->view.min_max_valid = false;
        e->view.change_count = 0;
        e->view.delta_dir = WATCH_DELTA_NONE;
        e->view.prev_len = 0;
    };
}


void watch_cache_prune_stale ( const int *live_ids, size_t n )
{
    if ( !s_cache ) return;
    /* Vytvor live set pro O(n+m) iteraci. */
    GHashTable *live = g_hash_table_new ( g_direct_hash, g_direct_equal );
    for ( size_t i = 0; i < n; i++ ) {
        g_hash_table_add ( live, GINT_TO_POINTER ( live_ids[i] ) );
    };

    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init ( &it, s_cache );
    while ( g_hash_table_iter_next ( &it, &key, &val ) ) {
        if ( !g_hash_table_contains ( live, key ) ) {
            g_hash_table_iter_remove ( &it );
        };
    };
    g_hash_table_destroy ( live );

    /* Stejne pro snapshot. */
    if ( s_snapshot ) {
        GHashTable *live2 = g_hash_table_new ( g_direct_hash, g_direct_equal );
        for ( size_t i = 0; i < n; i++ ) {
            g_hash_table_add ( live2, GINT_TO_POINTER ( live_ids[i] ) );
        };
        GHashTableIter it2;
        gpointer k2, v2;
        g_hash_table_iter_init ( &it2, s_snapshot );
        while ( g_hash_table_iter_next ( &it2, &k2, &v2 ) ) {
            if ( !g_hash_table_contains ( live2, k2 ) ) {
                g_hash_table_iter_remove ( &it2 );
            };
        };
        g_hash_table_destroy ( live2 );
        if ( g_hash_table_size ( s_snapshot ) == 0 ) {
            s_snapshot_active = false;
        };
    };
}


/* ===================================================================== */
/*  Snapshot baseline (Phase E.2)                                        */
/* ===================================================================== */


void watch_snapshot_begin ( int frame_no )
{
    if ( !s_snapshot ) return;
    g_hash_table_remove_all ( s_snapshot );
    s_snapshot_active = true;
    s_snapshot_frame = frame_no;
}


void watch_snapshot_put ( int row_id,
                           en_WATCH_TYPE type,
                           uint64_t value_int,
                           const uint8_t *bytes,
                           size_t len )
{
    if ( !s_snapshot || row_id < 0 ) return;
    st_WATCH_VALUE_SNAP *snap = snap_new ( );
    snap->type = type;
    snap->value_int = value_int;
    size_t to_copy = ( len > WATCH_LENGTH_MAX_BYTES )
                      ? WATCH_LENGTH_MAX_BYTES : len;
    if ( bytes && to_copy > 0 ) {
        memcpy ( snap->bytes, bytes, to_copy );
    };
    snap->len = to_copy;
    g_hash_table_insert ( s_snapshot, GINT_TO_POINTER ( row_id ), snap );
    s_snapshot_active = true;
}


const st_WATCH_VALUE_SNAP *watch_snapshot_get ( int row_id )
{
    if ( !s_snapshot || row_id < 0 ) return NULL;
    return (const st_WATCH_VALUE_SNAP *)
        g_hash_table_lookup ( s_snapshot, GINT_TO_POINTER ( row_id ) );
}


void watch_snapshot_clear ( void )
{
    if ( !s_snapshot ) return;
    g_hash_table_remove_all ( s_snapshot );
    s_snapshot_active = false;
    s_snapshot_frame = 0;
}


void watch_snapshot_remove ( int row_id )
{
    if ( !s_snapshot || row_id < 0 ) return;
    g_hash_table_remove ( s_snapshot, GINT_TO_POINTER ( row_id ) );
    if ( g_hash_table_size ( s_snapshot ) == 0 ) {
        s_snapshot_active = false;
    };
}


bool watch_snapshot_active ( void )
{
    return s_snapshot_active;
}


int watch_snapshot_frame ( void )
{
    return s_snapshot_frame;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
