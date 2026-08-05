/**
 * @file membrowser_snapshot.cpp
 * @brief Implementace per-region snapshot pro Δ layer (V1).
 *
 * Storage: fixní pole MEMBROWSER_SNAPSHOT_MAX_REGIONS slotů; každý slot drží
 * region_id + heap buffer + size. Lineární scan při lookup je OK pro
 * očekávaný počet snapshotů (V1 typicky 1-5).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_snapshot.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
}

/**
 * @brief Max počet současně držených snapshotů.
 *
 * Realisticky 1 instance × ~1 snapshot. 16 dává bezpečnou rezervu pro V3
 * multi-instance setup.
 */
#define MEMBROWSER_SNAPSHOT_MAX_REGIONS 16

/**
 * @brief Slot v snapshot storage.
 *
 * region_id = -1 znamená volný slot. data == NULL znamená "ještě nebyl
 * alokován" - obvykle stejné jako region_id = -1.
 */
struct st_SNAPSHOT_SLOT
{
    int       region_id;   /**< -1 = volný, jinak ID z dbgapi_regions_enumerate. */
    uint8_t  *data;        /**< Heap buffer s obsahem. */
    uint32_t  size;        /**< Velikost data v bajtech. */
};

static st_SNAPSHOT_SLOT s_slots[MEMBROWSER_SNAPSHOT_MAX_REGIONS];
static bool             s_init_done = false;


static void ensure_init ( void )
{
    if ( s_init_done ) return;
    for ( int i = 0; i < MEMBROWSER_SNAPSHOT_MAX_REGIONS; i++ ) {
        s_slots[i].region_id = -1;
        s_slots[i].data = nullptr;
        s_slots[i].size = 0;
    }
    s_init_done = true;
}


/**
 * @brief Najde slot pro daný region_id, nebo -1.
 */
static int find_slot ( int region_id )
{
    if ( region_id < 0 ) return -1;
    for ( int i = 0; i < MEMBROWSER_SNAPSHOT_MAX_REGIONS; i++ ) {
        if ( s_slots[i].region_id == region_id ) return i;
    }
    return -1;
}


static int find_free_slot ( void )
{
    for ( int i = 0; i < MEMBROWSER_SNAPSHOT_MAX_REGIONS; i++ ) {
        if ( s_slots[i].region_id < 0 ) return i;
    }
    return -1;
}


/**
 * @brief Uvolní heap buffer slotu a označí ho jako volný.
 */
static void free_slot ( int idx )
{
    if ( idx < 0 || idx >= MEMBROWSER_SNAPSHOT_MAX_REGIONS ) return;
    if ( s_slots[idx].data ) {
        std::free ( s_slots[idx].data );
        s_slots[idx].data = nullptr;
    }
    s_slots[idx].region_id = -1;
    s_slots[idx].size = 0;
}


extern "C" bool membrowser_snapshot_take ( int region_id )
{
    ensure_init ( );
    if ( region_id < 0 ) return false;

    /* Enumerate aktuální regiony pro získání size. dbgapi_regions_read
     * sám clamp-uje na region size, ale potřebujeme předem znát kolik
     * alokovat. */
    static st_REGION_DESC tmp_regions[512];
    int n = dbgapi_regions_enumerate ( tmp_regions, 512 );
    if ( n <= 0 ) return false;

    uint32_t region_size = 0;
    for ( int i = 0; i < n; i++ ) {
        if ( tmp_regions[i].id == region_id ) {
            region_size = tmp_regions[i].size;
            if ( !tmp_regions[i].connected ) return false;
            break;
        }
    }
    if ( region_size == 0 ) return false;

    uint32_t take_size = region_size;
    if ( take_size > MEMBROWSER_SNAPSHOT_MAX_BYTES ) {
        take_size = MEMBROWSER_SNAPSHOT_MAX_BYTES;
    }

    /* Najdi existující slot nebo alokuj nový. */
    int idx = find_slot ( region_id );
    if ( idx < 0 ) {
        idx = find_free_slot ( );
        if ( idx < 0 ) return false;  /* Storage plný. */
    } else {
        /* Realloc pokud se velikost změnila (např. Memext bank přepínání). */
        if ( s_slots[idx].size != take_size ) {
            std::free ( s_slots[idx].data );
            s_slots[idx].data = nullptr;
            s_slots[idx].size = 0;
        }
    }

    if ( !s_slots[idx].data ) {
        s_slots[idx].data = ( uint8_t * ) std::malloc ( take_size );
        if ( !s_slots[idx].data ) {
            s_slots[idx].region_id = -1;
            s_slots[idx].size = 0;
            return false;
        }
        s_slots[idx].size = take_size;
    }

    /* Read aktuální obsah. dbgapi_regions_read může vrátit méně bajtů
     * (clamp), v takovém případě zbytek vyplníme 0. */
    int got = dbgapi_regions_read ( region_id, 0, s_slots[idx].data, take_size );
    if ( got < 0 ) {
        free_slot ( idx );
        return false;
    }
    for ( int i = got; i < ( int ) take_size; i++ ) {
        s_slots[idx].data[i] = 0;
    }

    s_slots[idx].region_id = region_id;
    return true;
}


extern "C" bool membrowser_snapshot_reset ( int region_id )
{
    ensure_init ( );
    int idx = find_slot ( region_id );
    if ( idx < 0 ) return false;
    free_slot ( idx );
    return true;
}


extern "C" bool membrowser_snapshot_has ( int region_id )
{
    ensure_init ( );
    return find_slot ( region_id ) >= 0;
}


extern "C" bool membrowser_snapshot_byte ( int region_id, uint32_t offset,
                                            uint8_t *out )
{
    ensure_init ( );
    int idx = find_slot ( region_id );
    if ( idx < 0 ) return false;
    if ( offset >= s_slots[idx].size ) return false;
    if ( out ) *out = s_slots[idx].data[offset];
    return true;
}


extern "C" uint32_t membrowser_snapshot_size ( int region_id )
{
    ensure_init ( );
    int idx = find_slot ( region_id );
    if ( idx < 0 ) return 0;
    return s_slots[idx].size;
}


extern "C" void membrowser_snapshot_cleanup_all ( void )
{
    if ( !s_init_done ) return;
    for ( int i = 0; i < MEMBROWSER_SNAPSHOT_MAX_REGIONS; i++ ) {
        free_slot ( i );
    }
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
