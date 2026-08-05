/*
 * File:   watch_emu_cache.c
 *
 * Implementace EMU-side thread-safe zrcadla Watch panel stavů pro MCP
 * dispatch. Viz watch_emu_cache.h pro architekturu, threading kontrakt
 * a scope omezení.
 *
 * Storage:
 *   - GHashTable<int row_id, st_WATCH_EMU_SNAPSHOT*> (= heap copies)
 *   - GMutex chrání všechny operace (publish, get, remove, shutdown)
 *
 * Lifetime:
 *   - init je idempotentní (= g_once_init_enter)
 *   - shutdown uvolní vše a vynuluje pointer
 *
 * Licence: GPL-3.0-or-later
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>

#include "watch_emu_cache.h"
#include "debugger/watch.h"        /* en_WATCH_TYPE */
#include "debugger/watch_cache.h"  /* watch_cache_value_to_signed */


/* ===================================================================== */
/*  Privátní stav modulu                                                 */
/* ===================================================================== */


/**
 * @brief Hash table mapující row_id -> heap-alokovaný snapshot.
 *
 * Klíč je `GINT_TO_POINTER(row_id)`. Value je `st_WATCH_EMU_SNAPSHOT*`
 * vlastněný hash table (= destroy callback g_free).
 *
 * Invariant: chráněno `s_mutex` při všech operacích po init.
 */
static GHashTable *s_mirror = NULL;


/**
 * @brief Mutex chránící `s_mirror`.
 *
 * Inicializovaný v `watch_emu_cache_init` přes `g_once_init_enter`.
 * Životnost stejná jako modulu (= clear v shutdown).
 */
static GMutex s_mutex;


/**
 * @brief Inicializační flag pro `g_once_init_enter` (= idempotentní init).
 */
static gsize s_init_once = 0;


/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */


/**
 * @brief Lazy init mutexu + hash table při prvním volání init.
 */
void watch_emu_cache_init ( void ) {
    if ( g_once_init_enter ( &s_init_once ) ) {
        g_mutex_init ( &s_mutex );
        s_mirror = g_hash_table_new_full ( g_direct_hash, g_direct_equal,
                                           NULL, g_free );
        g_once_init_leave ( &s_init_once, 1 );
        return;
    };

    /* Pokud už inicializováno, ale shutdown smazal storage, znovu
     * alokujeme hash table (= mutex zůstává). */
    g_mutex_lock ( &s_mutex );
    if ( !s_mirror ) {
        s_mirror = g_hash_table_new_full ( g_direct_hash, g_direct_equal,
                                           NULL, g_free );
    };
    g_mutex_unlock ( &s_mutex );
}


void watch_emu_cache_shutdown ( void ) {
    if ( !s_init_once ) return;

    g_mutex_lock ( &s_mutex );
    if ( s_mirror ) {
        g_hash_table_destroy ( s_mirror );  /* destroy callback uvolní values */
        s_mirror = NULL;
    };
    g_mutex_unlock ( &s_mutex );
}


/* ===================================================================== */
/*  Publish / Get / Remove                                               */
/* ===================================================================== */


/**
 * @brief Spočítá sign-extended int hodnotu ze st_WATCH_VALUE_SNAP.
 *
 * Pro int typy vrací `watch_cache_value_to_signed(type, value_int)`.
 * Pro string/bytes vrací 0 (= nepoužitelné).
 *
 * @param snap  Snap value (může být NULL = vrací 0).
 * @param type_fallback  Typ použít pokud snap->type je 0 (= empty).
 */
static int64_t snap_to_signed ( const st_WATCH_VALUE_SNAP *snap, int type_fallback ) {
    if ( !snap ) return 0;
    int t = ( snap->type != 0 ) ? snap->type : type_fallback;
    return watch_cache_value_to_signed ( (en_WATCH_TYPE) t, snap->value_int );
}


void watch_emu_cache_publish ( int row_id,
                                const st_WATCH_CACHE_VIEW *view,
                                const char *name,
                                const st_WATCH_VALUE_SNAP *snap,
                                int type ) {
    /* Defenzivně - pokud init nebyl volán, no-op. */
    if ( !s_init_once || !s_mirror ) return;
    if ( row_id < 1 ) return;

    /* Remove path: view nebo name chybí -> smazat ze zrcadla. */
    if ( !view || !name || !name[0] ) {
        watch_emu_cache_remove ( row_id );
        return;
    };

    g_mutex_lock ( &s_mutex );

    if ( !s_mirror ) {
        g_mutex_unlock ( &s_mutex );
        return;
    };

    st_WATCH_EMU_SNAPSHOT *e = (st_WATCH_EMU_SNAPSHOT *)
        g_hash_table_lookup ( s_mirror, GINT_TO_POINTER ( row_id ) );

    if ( !e ) {
        e = g_new0 ( st_WATCH_EMU_SNAPSHOT, 1 );
        e->row_id = row_id;
        g_hash_table_insert ( s_mirror, GINT_TO_POINTER ( row_id ), e );
    };

    /* Copy name (truncate na buffer). */
    g_strlcpy ( e->name, name, sizeof ( e->name ) );

    /* View statistiky. */
    e->cur_int        = watch_cache_value_to_signed (
                            (en_WATCH_TYPE) view->type_snap,
                            view->prev_int );
    e->min_int        = view->min_int;
    e->max_int        = view->max_int;
    e->change_count   = view->change_count;
    e->min_max_valid  = view->min_max_valid;
    e->type_snap      = view->type_snap;

    /* Snapshot baseline. */
    if ( snap ) {
        e->snapshot_active = true;
        e->snap_int        = snap_to_signed ( snap, type );
        e->delta_int       = e->cur_int - e->snap_int;
    } else {
        e->snapshot_active = false;
        e->snap_int        = 0;
        e->delta_int       = 0;
    };

    g_mutex_unlock ( &s_mutex );
}


void watch_emu_cache_remove ( int row_id ) {
    if ( !s_init_once || !s_mirror ) return;
    if ( row_id < 1 ) return;

    g_mutex_lock ( &s_mutex );
    if ( s_mirror ) {
        g_hash_table_remove ( s_mirror, GINT_TO_POINTER ( row_id ) );
    };
    g_mutex_unlock ( &s_mutex );
}


void watch_emu_cache_prune_stale ( const int *live_ids, size_t n ) {
    if ( !s_init_once || !s_mirror ) return;

    g_mutex_lock ( &s_mutex );

    if ( !s_mirror ) {
        g_mutex_unlock ( &s_mutex );
        return;
    };

    if ( n == 0 || !live_ids ) {
        g_hash_table_remove_all ( s_mirror );
        g_mutex_unlock ( &s_mutex );
        return;
    };

    /* Postavíme set platných ID a smažeme entries, které v něm nejsou. */
    GHashTable *live = g_hash_table_new ( g_direct_hash, g_direct_equal );
    for ( size_t i = 0; i < n; i++ ) {
        g_hash_table_add ( live, GINT_TO_POINTER ( live_ids[i] ) );
    };

    GHashTableIter it;
    gpointer       key, val;
    GSList        *to_remove = NULL;
    g_hash_table_iter_init ( &it, s_mirror );
    while ( g_hash_table_iter_next ( &it, &key, &val ) ) {
        if ( !g_hash_table_contains ( live, key ) ) {
            to_remove = g_slist_prepend ( to_remove, key );
        };
    };

    for ( GSList *l = to_remove; l; l = l->next ) {
        g_hash_table_remove ( s_mirror, l->data );
    };
    g_slist_free ( to_remove );
    g_hash_table_destroy ( live );

    g_mutex_unlock ( &s_mutex );
}


bool watch_emu_cache_get_by_name ( const char *name,
                                    st_WATCH_EMU_SNAPSHOT *out ) {
    if ( !out ) return false;
    if ( !name || !name[0] ) return false;
    if ( !s_init_once || !s_mirror ) return false;

    bool found = false;

    g_mutex_lock ( &s_mutex );

    if ( s_mirror ) {
        /* Linear scan přes values - v praxi desítky řádků, OK. */
        GHashTableIter it;
        gpointer       key, val;
        g_hash_table_iter_init ( &it, s_mirror );
        while ( g_hash_table_iter_next ( &it, &key, &val ) ) {
            const st_WATCH_EMU_SNAPSHOT *e = (const st_WATCH_EMU_SNAPSHOT *) val;
            if ( !e ) continue;
            if ( strcmp ( e->name, name ) == 0 ) {
                memcpy ( out, e, sizeof ( *out ) );
                found = true;
                break;
            };
        };
    };

    g_mutex_unlock ( &s_mutex );

    return found;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
