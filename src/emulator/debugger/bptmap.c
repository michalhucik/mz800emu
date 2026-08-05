/*
 * File:   bptmap.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 30. září 2015, 10:15
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

#include "mzarch/mzcommon_config.h"

#include <stdio.h>
#include <string.h>

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "bptmap.h"

#include "mzarch/mzarch.h"
#include "debugger.h"
#include "emulator.h"

st_BPTMAP g_bptmap;


/**
 * @brief Přepočítá agregátní flag g_bptmap.any_active.
 *
 * Po každé mutaci per_type_active[]. Hot loop testuje jen any_active,
 * detail per-typ řeší až po dispatch.
 */
static void bptmap_update_any_active ( void ) {
    int i;
    bool any = false;
    for ( i = 0; i < BPTMAP_IDX_COUNT; i++ ) {
        if ( g_bptmap.per_type_active[i] ) { any = true; break; };
    };
    g_bptmap.any_active = any;
}


void bptmap_init ( void ) {
    int i;
    /* Lazy alloc per-typ listů - alokujeme až při prvním register. */
    for ( i = 0; i < BPTMAP_IDX_COUNT; i++ ) {
        g_bptmap.per_type_lists[i] = NULL;
        g_bptmap.per_type_active[i] = false;
    };
    g_bptmap.any_active = false;
    bptmap_clear_all ( );
}


void bptmap_clear_all ( void ) {
    int i;
    memset ( g_bptmap.bpmap, BREAKPOINT_TYPE_NONE, sizeof ( g_bptmap.bpmap ) );
    g_bptmap.temporary_bpt_addr = -1;
    /* Vyprázdnit per-typ listy, zachovat alokace (= GArray->len = 0). */
    for ( i = 0; i < BPTMAP_IDX_COUNT; i++ ) {
        if ( g_bptmap.per_type_lists[i] ) {
            g_array_set_size ( g_bptmap.per_type_lists[i], 0 );
        };
        g_bptmap.per_type_active[i] = false;
    };
    g_bptmap.any_active = false;
    g_bptmap.has_enabled_bp = false;
}


void bptmap_reset_temporary_event ( void ) {
    if ( g_bptmap.temporary_bpt_addr == -1 ) return;
    printf ( "DEBUGGER - remove temporary breakpoint on addr: 0x%04x\n", g_bptmap.temporary_bpt_addr );
    if ( g_bptmap.bpmap [ g_bptmap.temporary_bpt_addr ] == BREAKPOINT_TYPE_TEMPORARY ) g_bptmap.bpmap [ g_bptmap.temporary_bpt_addr ] = BREAKPOINT_TYPE_NONE;
    g_bptmap.temporary_bpt_addr = -1;
}


void bptmap_set_temporary_event ( uint16_t addr ) {
    if ( g_bptmap.temporary_bpt_addr != -1 ) bptmap_reset_temporary_event ( );
    g_bptmap.temporary_bpt_addr = addr;
    printf ( "DEBUGGER - add temporary breakpoint on addr: 0x%04x\n", g_bptmap.temporary_bpt_addr );
    if ( g_bptmap.bpmap [ g_bptmap.temporary_bpt_addr ] == BREAKPOINT_TYPE_NONE ) g_bptmap.bpmap [ g_bptmap.temporary_bpt_addr ] = BREAKPOINT_TYPE_TEMPORARY;
}


int bptmap_event_add ( uint16_t addr, int id ) {
    if ( id <= BREAKPOINT_TYPE_NONE ) return 0;
    if ( g_bptmap.bpmap [ addr ] > BREAKPOINT_TYPE_NONE ) return g_bptmap.bpmap [ addr ];
    g_bptmap.bpmap [ addr ] = id;
    return 0;
}


int bptmap_event_clear ( uint16_t addr, int id ) {
    if ( id <= BREAKPOINT_TYPE_NONE ) return 0;
    if ( g_bptmap.bpmap [ addr ] != id ) return 0;
    g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_NONE;
    if ( addr == g_bptmap.temporary_bpt_addr ) g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_TEMPORARY;
    return 1;
}


void bptmap_event_clear_addr ( uint16_t addr ) {
    if ( g_bptmap.bpmap [ addr ] > BREAKPOINT_TYPE_NONE ) g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_NONE;
    if ( addr == g_bptmap.temporary_bpt_addr ) g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_TEMPORARY;
}


void bptmap_event_clear_id ( int id ) {
    if ( id <= BREAKPOINT_TYPE_NONE ) return;
    unsigned addr;
    for ( addr = 0; addr <= 0xffff; addr++ ) {
        if ( g_bptmap.bpmap [ addr ] == id ) {
            g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_NONE;
            if ( addr == (unsigned)g_bptmap.temporary_bpt_addr ) g_bptmap.bpmap [ addr ] = BREAKPOINT_TYPE_TEMPORARY;
            return;
        };
    };
}


int bptmap_event_get_addr_by_id ( int id ) {
    if ( id <= BREAKPOINT_TYPE_NONE ) return -1;
    unsigned addr;
    for ( addr = 0; addr <= 0xffff; addr++ ) {
        if ( g_bptmap.bpmap [ addr ] == id ) {
            return addr;
        };
    };
    return -1;
}


int bptmap_event_get_id_by_addr ( uint16_t addr ) {
    return g_bptmap.bpmap [ addr ];
}


void bptmap_activate_event ( void ) {
    int breakpoint_id = g_bptmap.bpmap [ g_mzarch_main.instruction_addr ];
    if ( breakpoint_id == BREAKPOINT_TYPE_NONE ) return;
    if ( breakpoint_id > BREAKPOINT_TYPE_NONE ) {
        printf ( "DEBUGGER - activated breakpoint on addr: 0x%04x\n", g_mzarch_main.instruction_addr );
        emulator_pause ( true );
        debugger_show_main_window ( );
    };

    if ( !EMULATOR_TEST_PAUSED ) {
        if ( g_mzarch_main.instruction_addr == g_bptmap.temporary_bpt_addr ) {
            printf ( "DEBUGGER - activated temporary breakpoint on addr: 0x%04x\n", g_mzarch_main.instruction_addr );
            emulator_pause ( true );
            debugger_show_main_window ( );
        };
    };
}


/* ========================================================================= */
/*  D.2 per-typ tabulky                                                       */
/* ========================================================================= */


void bptmap_register ( int bp_id, en_BPTMAP_TYPE_IDX idx ) {
    if ( bp_id < 0 ) return;
    if ( idx < 0 || idx >= BPTMAP_IDX_COUNT ) return;

    /* Lazy alokace listu při prvním použití. */
    if ( !g_bptmap.per_type_lists[idx] ) {
        g_bptmap.per_type_lists[idx] = g_array_new ( FALSE, FALSE, sizeof ( int ) );
    };

    GArray *list = g_bptmap.per_type_lists[idx];

    /* Idempotence - pokud už je tam, neduplikuj. */
    unsigned i;
    for ( i = 0; i < list->len; i++ ) {
        if ( g_array_index ( list, int, i ) == bp_id ) return;
    };

    g_array_append_val ( list, bp_id );
    g_bptmap.per_type_active[idx] = true;
    g_bptmap.any_active = true;
}


void bptmap_unregister ( int bp_id, en_BPTMAP_TYPE_IDX idx ) {
    if ( bp_id < 0 ) return;
    if ( idx < 0 || idx >= BPTMAP_IDX_COUNT ) return;

    GArray *list = g_bptmap.per_type_lists[idx];
    if ( !list ) return;

    unsigned i;
    for ( i = 0; i < list->len; i++ ) {
        if ( g_array_index ( list, int, i ) == bp_id ) {
            g_array_remove_index ( list, i );
            break;
        };
    };

    if ( list->len == 0 ) {
        g_bptmap.per_type_active[idx] = false;
        bptmap_update_any_active ( );
    };
}


void bptmap_unregister_all ( int bp_id ) {
    int i;
    for ( i = 0; i < BPTMAP_IDX_COUNT; i++ ) {
        bptmap_unregister ( bp_id, (en_BPTMAP_TYPE_IDX) i );
    };
}


GArray* bptmap_get_list ( en_BPTMAP_TYPE_IDX idx ) {
    if ( idx < 0 || idx >= BPTMAP_IDX_COUNT ) return NULL;
    return g_bptmap.per_type_lists[idx];
}

#endif
