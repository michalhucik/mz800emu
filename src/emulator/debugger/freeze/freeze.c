/*
 * File:   freeze.c
 *
 * Implementace Freeze Bytes - viz freeze.h pro popis modulu.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>

#include "freeze.h"
#include "emulator/debugger/dbgapi_regions.h"


/**
 * @brief Statické úložiště - BSS alokace, žádný malloc.
 *
 * Layout: pole FREEZE_MAX_ENTRIES slotů. Slot je obsazen pokud in_use = true.
 * add() hledá první volný slot (linear), remove() jen vynuluje in_use.
 */
static st_FREEZE_ENTRY s_entries[FREEZE_MAX_ENTRIES];


/**
 * @brief Počet obsazených slotů - dovolí O(1) skip v apply path.
 */
static size_t s_count = 0;


/**
 * @brief Hot path zkratka. 0 = žádné entries, apply vrátí okamžitě.
 *
 * Sync s s_count v každé mutate funkci. Public extern pro test v hot path
 * bez funkčního volání.
 */
unsigned g_freeze_active = 0;


/**
 * @brief Pomocný buffer pro region enumerate v apply path.
 *
 * Static aby šel reusovat bez stack alokace per call.
 */
#define FREEZE_REGIONS_MAX 512
static st_REGION_DESC s_apply_regions[FREEZE_REGIONS_MAX];


void freeze_init ( void )
{
    memset ( s_entries, 0, sizeof ( s_entries ) );
    s_count = 0;
    g_freeze_active = 0;
}


void freeze_clear ( void )
{
    freeze_init ( );
}


/**
 * @brief Najde slot pro danou (kind, sub_id, offset) trojici.
 *
 * @return Index slotu (0..FREEZE_MAX_ENTRIES-1) nebo -1 pokud nenalezen.
 */
static int find_slot ( int region_kind, int sub_id, uint32_t offset )
{
    for ( int i = 0; i < FREEZE_MAX_ENTRIES; i++ ) {
        if ( s_entries[i].in_use
             && s_entries[i].region_kind == region_kind
             && s_entries[i].sub_id == sub_id
             && s_entries[i].offset == offset ) {
            return i;
        }
    }
    return -1;
}


/**
 * @brief Najde první volný slot.
 *
 * @return Index volného slotu nebo -1 pokud je storage plný.
 */
static int find_free_slot ( void )
{
    for ( int i = 0; i < FREEZE_MAX_ENTRIES; i++ ) {
        if ( !s_entries[i].in_use ) return i;
    }
    return -1;
}


bool freeze_add ( int region_kind, int sub_id, uint32_t offset, uint8_t value )
{
    int idx = find_slot ( region_kind, sub_id, offset );
    if ( idx >= 0 ) {
        /* Update existing entry. */
        s_entries[idx].value = value;
        return true;
    }

    idx = find_free_slot ( );
    if ( idx < 0 ) return false;  /* Storage plný. */

    s_entries[idx].region_kind = region_kind;
    s_entries[idx].sub_id = sub_id;
    s_entries[idx].offset = offset;
    s_entries[idx].value = value;
    s_entries[idx].in_use = true;
    s_count++;
    g_freeze_active = ( s_count > 0 ) ? 1u : 0u;
    return true;
}


bool freeze_remove ( int region_kind, int sub_id, uint32_t offset )
{
    int idx = find_slot ( region_kind, sub_id, offset );
    if ( idx < 0 ) return false;

    s_entries[idx].in_use = false;
    if ( s_count > 0 ) s_count--;
    g_freeze_active = ( s_count > 0 ) ? 1u : 0u;
    return true;
}


bool freeze_is_frozen ( int region_kind, int sub_id, uint32_t offset,
                        uint8_t *out_value )
{
    int idx = find_slot ( region_kind, sub_id, offset );
    if ( idx < 0 ) return false;
    if ( out_value ) *out_value = s_entries[idx].value;
    return true;
}


size_t freeze_count ( void )
{
    return s_count;
}


const st_FREEZE_ENTRY *freeze_get_slot ( size_t idx )
{
    if ( idx >= FREEZE_MAX_ENTRIES ) return NULL;
    if ( !s_entries[idx].in_use ) return NULL;
    return &s_entries[idx];
}


void freeze_apply_all ( void )
{
    /* Hot path skip: žádné frozen byty = okamžitý return. Branch
     * predictor v default OFF stavu naučí "vždy false". */
    if ( g_freeze_active == 0 ) return;

    /* Enumerate aktuální regiony jednou per call (banking-aware snapshot).
     * Pro každý entry hledáme region_id podle (kind, sub_id). */
    int n = dbgapi_regions_enumerate ( s_apply_regions, FREEZE_REGIONS_MAX );
    if ( n <= 0 ) return;

    for ( int i = 0; i < FREEZE_MAX_ENTRIES; i++ ) {
        if ( !s_entries[i].in_use ) continue;

        /* Najdi region_id v aktuálním snapshotu. */
        int region_id = -1;
        for ( int j = 0; j < n; j++ ) {
            if ( (int) s_apply_regions[j].kind == s_entries[i].region_kind
                 && s_apply_regions[j].sub_id == s_entries[i].sub_id ) {
                region_id = s_apply_regions[j].id;
                /* Skip pokud region disconnected nebo R-only - soft tolerance. */
                if ( !s_apply_regions[j].connected
                     || !s_apply_regions[j].writable ) {
                    region_id = -1;
                }
                break;
            }
        }
        if ( region_id < 0 ) continue;

        /* Apply - 1-byte write přes dbgapi (no side-effect na chip state). */
        dbgapi_regions_write ( region_id, s_entries[i].offset,
                               &s_entries[i].value, 1 );
    }
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
