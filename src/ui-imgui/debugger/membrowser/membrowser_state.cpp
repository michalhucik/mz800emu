/**
 * @file membrowser_state.cpp
 * @brief State default + persist load/save Memory Browser (V0 + V3 multi-view).
 *
 * V0: jedna persist sekce [MEMBROWSER_WINDOW].
 * V3 multi-view: 5 instancí (main + #2..#5), per-instance persist sekce
 * [MEMBROWSER_WINDOW_MAIN] / [MEMBROWSER_WINDOW_2] / .._3 / .._4 / .._5.
 *
 * Legacy fallback: pokud INI obsahuje starou sekci [MEMBROWSER_WINDOW]
 * a NEobsahuje novou [MEMBROWSER_WINDOW_MAIN], hodnoty se zkopírují do
 * MAIN slotu (jednorázová migrace - po prvním save zapíše obě sekce, ale
 * legacy už nikdy nepřečte). Workflow je řízen
 * @ref membrowser_window_register_persistence; uvnitř tohoto modulu jen
 * držíme scratch slot @c MB_INSTANCE_LEGACY (= 5, nad rámec normálního
 * pole - alokujeme tedy MB_INSTANCE_COUNT+1 = 6 položek).
 *
 * Persist klíče v každé sekci (per instance):
 *   - encoding (unsigned 0..MEMBROWSER_ENC__COUNT-1)
 *   - region_kind (unsigned 0..REGION_KIND_COUNT-1)
 *   - region_sub_id (unsigned 0..255)
 *   - cursor_addr (unsigned 0..0x7FFFFFFF)
 *   - bytes_per_row (unsigned: 8/16/32)
 *   - ascii_column_visible (bool)
 *   - show_pc_marker (bool)
 *   - show_sp_marker (bool)
 *   - layers_panel_open, layer_cdl_x/r/w, layer_heatmap, layer_snapshot_delta,
 *     layer_frozen, layer_symbols (bool)
 *   - layers_panel_width (unsigned 0..2000)
 *   - regions_panel_open (bool), regions_panel_width (unsigned 0..2000)
 *   - show_origin_labels (bool)
 *
 * V0-polish-3: encoding schéma změněno (10 charsetů per mzdisk).
 * apply_persisted dělá range check + fallback na MB_CHARSET_RAW
 * pro neznámé ID.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_state.h"

#include <cstring>
#include <cstdlib>

extern "C" {
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "emulator/debugger/dbgapi_regions.h"
}

/* Scratch slot pro legacy sekci [MEMBROWSER_WINDOW] - index nad rámec
 * normálního pole 0..MB_INSTANCE_COUNT-1. Slot je čten jen pri startu
 * (apply_legacy_fallback_if_needed), nikdy se nezapisuje zpět. */
#define MB_INSTANCE_LEGACY  MB_INSTANCE_COUNT
#define MB_PERSIST_SLOTS    (MB_INSTANCE_COUNT + 1)

/* Persisted hodnoty per instance + legacy scratch slot. cfgmain library
 * čte/zapisuje přes pointery zaregistrované v cfgelement_set_handlers. */
static unsigned s_persist_encoding[MB_PERSIST_SLOTS]            = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_region_kind[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_region_sub_id[MB_PERSIST_SLOTS]       = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_cursor_addr[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
/* Bugfix-final Bug 3c: default bytes_per_row sníženo z 32 na 16
 * (kompaktnější výchozí layout pro užší okno). */
static unsigned s_persist_bytes_per_row[MB_PERSIST_SLOTS]       = { 16, 16, 16, 16, 16, 16 };
static unsigned s_persist_ascii_column_visible[MB_PERSIST_SLOTS] = { 1, 1, 1, 1, 1, 1 };
static unsigned s_persist_show_pc_marker[MB_PERSIST_SLOTS]      = { 1, 1, 1, 1, 1, 1 };
static unsigned s_persist_show_sp_marker[MB_PERSIST_SLOTS]      = { 1, 1, 1, 1, 1, 1 };

/* V1 Layers - default all OFF. */
static unsigned s_persist_layers_panel_open[MB_PERSIST_SLOTS]   = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_cdl_x[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_cdl_r[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_cdl_w[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_heatmap[MB_PERSIST_SLOTS]       = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_snapshot_delta[MB_PERSIST_SLOTS] = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_frozen[MB_PERSIST_SLOTS]        = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layer_symbols[MB_PERSIST_SLOTS]       = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_layers_panel_width[MB_PERSIST_SLOTS]  = { 0, 0, 0, 0, 0, 0 };

/* V2 Regions sidebar - default OFF od bugfix-final Bug 3c
 * (původně ON; uživatel většinou pracuje jen v Logical Z80 view a
 * sidebar zabíral horizontální prostor). Lze zapnout přes toolbar. */
static unsigned s_persist_regions_panel_open[MB_PERSIST_SLOTS]  = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_regions_panel_width[MB_PERSIST_SLOTS] = { 0, 0, 0, 0, 0, 0 };

/* V2 Origin labels - default OFF. */
static unsigned s_persist_show_origin_labels[MB_PERSIST_SLOTS]  = { 0, 0, 0, 0, 0, 0 };

/* V4 Search engine persist - type/scope/case + last pattern.
 *
 * Persistuje se:
 *   - search_type (UNSIGNED enum)
 *   - search_scope (UNSIGNED enum)
 *   - search_case_sensitive (BOOL)
 *   - search_last_pattern (TEXT) - jen poslední pattern; full history je
 *     in-memory only (10 slots v search_history[]) - persist celého 2D
 *     pole stringů by vyžadoval N×TEXT elementů a komplikoval cfgmain.
 *     V4.1+ pokud bude potřeba full history persist, lze rozšířit.
 *
 * CFGENTYPE_TEXT vyžaduje char** binding (knihovna interně realloc/free).
 * Držíme char* per slot - inicializace na default v register_persistence
 * (strdup ""), update přes save_to_persisted (free + strdup z aktuálního
 * stringu). */
static unsigned s_persist_search_type[MB_PERSIST_SLOTS]         = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_search_scope[MB_PERSIST_SLOTS]        = { 0, 0, 0, 0, 0, 0 };
static unsigned s_persist_search_case_sensitive[MB_PERSIST_SLOTS] = { 1, 1, 1, 1, 1, 1 };
static char    *s_persist_search_last_pattern[MB_PERSIST_SLOTS] = { NULL, NULL, NULL, NULL, NULL, NULL };


/* Pomocný safety clamp - vrátí validní slot index 0..MB_PERSIST_SLOTS-1
 * nebo 0 pokud je vstup mimo. */
static int clamp_slot ( int idx )
{
    if ( idx < 0 || idx >= MB_PERSIST_SLOTS ) return 0;
    return idx;
}


extern "C" st_MEMBROWSER_STATE membrowser_state_default ( int instance_idx )
{
    st_MEMBROWSER_STATE st;
    std::memset ( &st, 0, sizeof ( st ) );

    /* Clamp instance_idx do rozsahu 0..MB_INSTANCE_COUNT-1 (legacy slot
     * nepatří do reálné instance state). */
    if ( instance_idx < 0 || instance_idx >= MB_INSTANCE_COUNT ) {
        instance_idx = MB_INSTANCE_MAIN;
    }
    st.instance_idx = instance_idx;

    st.current_key.kind = REGION_KIND_LOGICAL;
    st.current_key.sub_id = 0;
    st.current_region_id = -1;     /* Resolved v render path. */
    st.current_encoding = MB_CHARSET_RAW;  /* V0-polish-3 default: Raw - bezpečný start, uživatel vybere přes dropdown. */

    st.cursor_addr = 0;
    st.scroll_top_addr = 0;
    st.bytes_per_row = 16;  /* Bugfix-final Bug 3c: default 16 (kompaktnější). */
    st.ascii_column_visible = true;
    st.show_pc_marker = true;
    st.show_sp_marker = true;

    st.edit_enabled = false;  /* OFF default per stará GTK reference (safety). */
    st.edit_mode = MEMBROWSER_EDIT_HEX;
    st.edit_nibble = 0;
    st.edit_input[0] = '\0';

    st.search_panel_open = false;
    /* V4.1+: nový default je MEMBROWSER_SEARCH_BYTES - Pattern Builder
     * s dual HEX+ASCII fields. Legacy BYTE_SEQ + ASCII zachované v enum
     * pro persisted state z dřívějška, ale nový default je BYTES. */
    st.search_type = MEMBROWSER_SEARCH_BYTES;
    st.search_pattern[0] = '\0';
    st.last_match_addr = 0;
    st.last_match_valid = false;

    /* V4: search engine defaults. */
    st.search_scope = MEMBROWSER_SCOPE_CURRENT;
    st.search_case_sensitive = true;
    st.search_state = MEMBROWSER_SEARCH_STATE_IDLE;
    st.search_mode = MEMBROWSER_SEARCH_MODE_NEXT;
    st.search_engine_region_id = -1;
    st.search_engine_region_kind = 0;
    st.search_engine_region_sub_id = 0;
    st.search_engine_scope_idx = 0;
    st.search_scan_pos = 0;
    st.search_scan_end = 0;
    st.search_total_scanned = 0;
    st.search_total_bytes = 0;
    st.search_error[0] = '\0';
    st.search_results_count = 0;
    st.search_results_visible = 0;
    /* search_results[] - memset zajistilo nulování. */
    st.search_history_count = 0;
    for ( int i = 0; i < MB_SEARCH_HISTORY_SIZE; i++ ) {
        st.search_history[i][0] = '\0';
        st.search_history_types[i] = MEMBROWSER_SEARCH_BYTE_SEQ;
    }

    st.goto_input[0] = '\0';

    /* V1 Layers - default OFF. */
    st.layers_panel_open = false;
    st.layer_cdl_x = false;
    st.layer_cdl_r = false;
    st.layer_cdl_w = false;
    st.layer_heatmap = false;
    st.layer_snapshot_delta = false;
    st.layer_frozen = false;
    st.layer_symbols = false;

    st.layers_panel_width = 0;  /* 0 = auto-compute při příštím renderu. */

    /* V2 Regions sidebar - default OFF od bugfix-final Bug 3c
     * (původně ON). User most-of-the-time pracuje v Logical Z80 view;
     * sidebar lze zapnout přes toolbar. */
    st.regions_panel_open = false;
    st.regions_panel_width = 0;

    /* V2 Origin labels - default OFF (opt-in feature). */
    st.show_origin_labels = false;

    /* Edit semantics: sentinel -1 = "uninitialized" - první render po
     * create state si zapamatuje aktuální region key bez clear. */
    st.last_region_kind = -1;
    st.last_region_sub_id = -1;

    return st;
}


extern "C" void membrowser_state_register_persistence ( void *cmod_void,
                                                         int instance_idx )
{
    CFGMOD *cmod = ( CFGMOD * ) cmod_void;
    if ( !cmod ) return;
    if ( instance_idx < 0 || instance_idx >= MB_PERSIST_SLOTS ) return;

    int s = instance_idx;
    CFGELM *elm;

    /* Per cfgmodule.h: CFGENTYPE_UNSIGNED má (int default, int min, int max). */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "encoding",
                                           CFGENTYPE_UNSIGNED,
                                           ( int ) MB_CHARSET_RAW,
                                           0, ( int ) ( MEMBROWSER_ENC__COUNT - 1 ) );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_encoding[s],
                                    ( void * ) &s_persist_encoding[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "region_kind",
                                           CFGENTYPE_UNSIGNED,
                                           ( int ) REGION_KIND_LOGICAL,
                                           0, ( int ) ( REGION_KIND_COUNT - 1 ) );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_region_kind[s],
                                    ( void * ) &s_persist_region_kind[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "region_sub_id",
                                           CFGENTYPE_UNSIGNED,
                                           0, 0, 0xFF );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_region_sub_id[s],
                                    ( void * ) &s_persist_region_sub_id[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "cursor_addr",
                                           CFGENTYPE_UNSIGNED,
                                           0, 0, 0x7FFFFFFF );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_cursor_addr[s],
                                    ( void * ) &s_persist_cursor_addr[s] );

    /* Bugfix-final Bug 3c: default bytes_per_row 32 -> 16. */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "bytes_per_row",
                                           CFGENTYPE_UNSIGNED,
                                           16, 8, 32 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_bytes_per_row[s],
                                    ( void * ) &s_persist_bytes_per_row[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "ascii_column_visible",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_ascii_column_visible[s],
                                    ( void * ) &s_persist_ascii_column_visible[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "show_pc_marker",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_show_pc_marker[s],
                                    ( void * ) &s_persist_show_pc_marker[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "show_sp_marker",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_show_sp_marker[s],
                                    ( void * ) &s_persist_show_sp_marker[s] );

    /* V1 Layers persist - default 0 (OFF). */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layers_panel_open",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layers_panel_open[s],
                                    ( void * ) &s_persist_layers_panel_open[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_cdl_x",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_cdl_x[s],
                                    ( void * ) &s_persist_layer_cdl_x[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_cdl_r",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_cdl_r[s],
                                    ( void * ) &s_persist_layer_cdl_r[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_cdl_w",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_cdl_w[s],
                                    ( void * ) &s_persist_layer_cdl_w[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_heatmap",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_heatmap[s],
                                    ( void * ) &s_persist_layer_heatmap[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_snapshot_delta",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_snapshot_delta[s],
                                    ( void * ) &s_persist_layer_snapshot_delta[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_frozen",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_frozen[s],
                                    ( void * ) &s_persist_layer_frozen[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layer_symbols",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layer_symbols[s],
                                    ( void * ) &s_persist_layer_symbols[s] );

    /* Šířka Layers panelu. Default 0 = auto-compute. */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "layers_panel_width",
                                           CFGENTYPE_UNSIGNED,
                                           0, 0, 2000 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_layers_panel_width[s],
                                    ( void * ) &s_persist_layers_panel_width[s] );

    /* V2 Regions sidebar - default 0 (OFF) od bugfix-final Bug 3c,
     * šířka 0 (auto). Změna ovlivní jen fresh install (žádná hodnota
     * v INI); existující INI s regions_panel_open=1 zůstane zapnutý. */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "regions_panel_open",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_regions_panel_open[s],
                                    ( void * ) &s_persist_regions_panel_open[s] );
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "regions_panel_width",
                                           CFGENTYPE_UNSIGNED,
                                           0, 0, 2000 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_regions_panel_width[s],
                                    ( void * ) &s_persist_regions_panel_width[s] );

    /* V2 Origin labels - default 0 (OFF). */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "show_origin_labels",
                                           CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_show_origin_labels[s],
                                    ( void * ) &s_persist_show_origin_labels[s] );

    /* V4 Search engine persist: type/scope/case + last pattern. */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "search_type",
                                           CFGENTYPE_UNSIGNED,
                                           ( int ) MEMBROWSER_SEARCH_BYTE_SEQ,
                                           0, ( int ) ( MEMBROWSER_SEARCH__COUNT - 1 ) );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_search_type[s],
                                    ( void * ) &s_persist_search_type[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "search_scope",
                                           CFGENTYPE_UNSIGNED,
                                           ( int ) MEMBROWSER_SCOPE_CURRENT,
                                           0, ( int ) ( MEMBROWSER_SCOPE__COUNT - 1 ) );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_search_scope[s],
                                    ( void * ) &s_persist_search_scope[s] );

    elm = cfgmodule_register_new_element ( cmod, ( char * ) "search_case_sensitive",
                                           CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, ( void * ) &s_persist_search_case_sensitive[s],
                                    ( void * ) &s_persist_search_case_sensitive[s] );

    /* TEXT element pro last pattern - cfgelement_bind dělá strdup/free
     * pres char* pointer (NESmí být NULL na vstupu, knihovna realokuje). */
    elm = cfgmodule_register_new_element ( cmod, ( char * ) "search_last_pattern",
                                           CFGENTYPE_TEXT, ( char * ) "" );
    cfgelement_bind ( elm, ( void * ) &s_persist_search_last_pattern[s] );
}


extern "C" void membrowser_state_apply_persisted ( st_MEMBROWSER_STATE *st )
{
    if ( !st ) return;
    int s = clamp_slot ( st->instance_idx );

    /* Graceful fallback na RAW pro neznámé encoding ID. */
    if ( s_persist_encoding[s] < ( unsigned ) MEMBROWSER_ENC__COUNT ) {
        st->current_encoding = ( int ) s_persist_encoding[s];
    } else {
        st->current_encoding = MB_CHARSET_RAW;
    }
    if ( s_persist_region_kind[s] < ( unsigned ) REGION_KIND_COUNT ) {
        st->current_key.kind = ( int ) s_persist_region_kind[s];
    }
    st->current_key.sub_id = ( int ) s_persist_region_sub_id[s];
    st->cursor_addr = s_persist_cursor_addr[s];
    if ( s_persist_bytes_per_row[s] == 8u || s_persist_bytes_per_row[s] == 16u
            || s_persist_bytes_per_row[s] == 32u ) {
        st->bytes_per_row = ( int ) s_persist_bytes_per_row[s];
    }
    st->ascii_column_visible = ( s_persist_ascii_column_visible[s] != 0 );
    st->show_pc_marker = ( s_persist_show_pc_marker[s] != 0 );
    st->show_sp_marker = ( s_persist_show_sp_marker[s] != 0 );

    /* V1 Layers - apply persisted bool hodnoty. */
    st->layers_panel_open = ( s_persist_layers_panel_open[s] != 0 );
    st->layer_cdl_x = ( s_persist_layer_cdl_x[s] != 0 );
    st->layer_cdl_r = ( s_persist_layer_cdl_r[s] != 0 );
    st->layer_cdl_w = ( s_persist_layer_cdl_w[s] != 0 );
    st->layer_heatmap = ( s_persist_layer_heatmap[s] != 0 );
    st->layer_snapshot_delta = ( s_persist_layer_snapshot_delta[s] != 0 );
    st->layer_frozen = ( s_persist_layer_frozen[s] != 0 );
    st->layer_symbols = ( s_persist_layer_symbols[s] != 0 );

    /* Layers panel width - akceptuj 0 (= auto) nebo 200..600. */
    if ( s_persist_layers_panel_width[s] == 0
            || ( s_persist_layers_panel_width[s] >= 200u
                  && s_persist_layers_panel_width[s] <= 600u ) ) {
        st->layers_panel_width = ( int ) s_persist_layers_panel_width[s];
    } else {
        st->layers_panel_width = 0;
    }

    /* V2 Regions sidebar - bool open + width clamp (0 nebo 180..420). */
    st->regions_panel_open = ( s_persist_regions_panel_open[s] != 0 );
    if ( s_persist_regions_panel_width[s] == 0
            || ( s_persist_regions_panel_width[s] >= 180u
                  && s_persist_regions_panel_width[s] <= 420u ) ) {
        st->regions_panel_width = ( int ) s_persist_regions_panel_width[s];
    } else {
        st->regions_panel_width = 0;
    }

    /* V2 Origin labels - jednoduchý bool. */
    st->show_origin_labels = ( s_persist_show_origin_labels[s] != 0 );

    /* V4 Search engine - type/scope clamp, case_sensitive bool, last pattern. */
    if ( s_persist_search_type[s] < ( unsigned ) MEMBROWSER_SEARCH__COUNT ) {
        st->search_type = ( int ) s_persist_search_type[s];
    }
    if ( s_persist_search_scope[s] < ( unsigned ) MEMBROWSER_SCOPE__COUNT ) {
        st->search_scope = ( int ) s_persist_search_scope[s];
    }
    st->search_case_sensitive = ( s_persist_search_case_sensitive[s] != 0 );

    if ( s_persist_search_last_pattern[s] != NULL
         && s_persist_search_last_pattern[s][0] != '\0' ) {
        std::strncpy ( st->search_pattern,
                       s_persist_search_last_pattern[s],
                       sizeof ( st->search_pattern ) - 1 );
        st->search_pattern[sizeof ( st->search_pattern ) - 1] = '\0';
        /* Naplnit history slot 0 stejnou hodnotou (= "minulý běh"). */
        std::strncpy ( st->search_history[0], st->search_pattern,
                       MB_SEARCH_PATTERN_SIZE - 1 );
        st->search_history[0][MB_SEARCH_PATTERN_SIZE - 1] = '\0';
        st->search_history_types[0] = st->search_type;
        st->search_history_count = 1;
    }
}


extern "C" void membrowser_state_save_to_persisted ( const st_MEMBROWSER_STATE *st )
{
    if ( !st ) return;
    int s = clamp_slot ( st->instance_idx );

    s_persist_encoding[s] = ( unsigned ) st->current_encoding;
    s_persist_region_kind[s] = ( unsigned ) st->current_key.kind;
    s_persist_region_sub_id[s] = ( unsigned ) st->current_key.sub_id;
    s_persist_cursor_addr[s] = st->cursor_addr;
    s_persist_bytes_per_row[s] = ( unsigned ) st->bytes_per_row;
    s_persist_ascii_column_visible[s] = st->ascii_column_visible ? 1u : 0u;
    s_persist_show_pc_marker[s] = st->show_pc_marker ? 1u : 0u;
    s_persist_show_sp_marker[s] = st->show_sp_marker ? 1u : 0u;

    s_persist_layers_panel_open[s] = st->layers_panel_open ? 1u : 0u;
    s_persist_layer_cdl_x[s] = st->layer_cdl_x ? 1u : 0u;
    s_persist_layer_cdl_r[s] = st->layer_cdl_r ? 1u : 0u;
    s_persist_layer_cdl_w[s] = st->layer_cdl_w ? 1u : 0u;
    s_persist_layer_heatmap[s] = st->layer_heatmap ? 1u : 0u;
    s_persist_layer_snapshot_delta[s] = st->layer_snapshot_delta ? 1u : 0u;
    s_persist_layer_frozen[s] = st->layer_frozen ? 1u : 0u;
    s_persist_layer_symbols[s] = st->layer_symbols ? 1u : 0u;
    s_persist_layers_panel_width[s] = ( unsigned ) st->layers_panel_width;

    s_persist_regions_panel_open[s] = st->regions_panel_open ? 1u : 0u;
    s_persist_regions_panel_width[s] = ( unsigned ) st->regions_panel_width;

    s_persist_show_origin_labels[s] = st->show_origin_labels ? 1u : 0u;

    /* V4: search engine - type/scope/case + last pattern.
     *
     * TEXT element vyžaduje char** binding - knihovna interně realloc/free.
     * Při save bookmark pattern: pokud byl předtím alokovaný, free; pak
     * strdup z aktuálního search_pattern (nebo "" pokud prázdný). */
    s_persist_search_type[s] = ( unsigned ) st->search_type;
    s_persist_search_scope[s] = ( unsigned ) st->search_scope;
    s_persist_search_case_sensitive[s] = st->search_case_sensitive ? 1u : 0u;

    /* Pouze update text pointeru pokud máme co uložit - jinak ponecháme
     * existující (může to být default "" alokovaný cfgmodule_register). */
    const char *pat = ( st->search_history_count > 0 )
                          ? st->search_history[0]
                          : st->search_pattern;
    if ( pat != NULL ) {
        char *new_dup = NULL;
        size_t plen = std::strlen ( pat );
        if ( plen > 0 ) {
            /* Jednoduchá malloc + memcpy - zabráníme závislosti na g_strdup
             * (GLib) v tomto souboru. cfg knihovna interně používá free,
             * takže malloc je kompatibilní (oba ze stejné CRT). */
            new_dup = ( char * ) std::malloc ( plen + 1 );
            if ( new_dup ) {
                std::memcpy ( new_dup, pat, plen + 1 );
            }
        } else {
            new_dup = ( char * ) std::malloc ( 1 );
            if ( new_dup ) new_dup[0] = '\0';
        }
        if ( new_dup ) {
            if ( s_persist_search_last_pattern[s] ) {
                std::free ( s_persist_search_last_pattern[s] );
            }
            s_persist_search_last_pattern[s] = new_dup;
        }
    }
}


extern "C" void membrowser_state_apply_legacy_fallback_if_needed (
        bool have_main_section_in_ini )
{
    /* Pokud INI obsahovala MAIN sekci, ignoruj legacy (MAIN má přednost
     * + zápis při shutdownu by stejně přepsal legacy nepoužitelně). */
    if ( have_main_section_in_ini ) return;

    /* Zkopíruj legacy scratch slot do MAIN slotu. */
    int src = MB_INSTANCE_LEGACY;
    int dst = MB_INSTANCE_MAIN;

    s_persist_encoding[dst]             = s_persist_encoding[src];
    s_persist_region_kind[dst]          = s_persist_region_kind[src];
    s_persist_region_sub_id[dst]        = s_persist_region_sub_id[src];
    s_persist_cursor_addr[dst]          = s_persist_cursor_addr[src];
    s_persist_bytes_per_row[dst]        = s_persist_bytes_per_row[src];
    s_persist_ascii_column_visible[dst] = s_persist_ascii_column_visible[src];
    s_persist_show_pc_marker[dst]       = s_persist_show_pc_marker[src];
    s_persist_show_sp_marker[dst]       = s_persist_show_sp_marker[src];

    s_persist_layers_panel_open[dst]    = s_persist_layers_panel_open[src];
    s_persist_layer_cdl_x[dst]          = s_persist_layer_cdl_x[src];
    s_persist_layer_cdl_r[dst]          = s_persist_layer_cdl_r[src];
    s_persist_layer_cdl_w[dst]          = s_persist_layer_cdl_w[src];
    s_persist_layer_heatmap[dst]        = s_persist_layer_heatmap[src];
    s_persist_layer_snapshot_delta[dst] = s_persist_layer_snapshot_delta[src];
    s_persist_layer_frozen[dst]         = s_persist_layer_frozen[src];
    s_persist_layer_symbols[dst]        = s_persist_layer_symbols[src];
    s_persist_layers_panel_width[dst]   = s_persist_layers_panel_width[src];

    s_persist_regions_panel_open[dst]   = s_persist_regions_panel_open[src];
    s_persist_regions_panel_width[dst]  = s_persist_regions_panel_width[src];

    s_persist_show_origin_labels[dst]   = s_persist_show_origin_labels[src];

    /* V4 search engine - clone numerické, last_pattern jen pokud src má co. */
    s_persist_search_type[dst]            = s_persist_search_type[src];
    s_persist_search_scope[dst]           = s_persist_search_scope[src];
    s_persist_search_case_sensitive[dst]  = s_persist_search_case_sensitive[src];

    if ( s_persist_search_last_pattern[src] != NULL ) {
        size_t plen = std::strlen ( s_persist_search_last_pattern[src] );
        char *dup = ( char * ) std::malloc ( plen + 1 );
        if ( dup ) {
            std::memcpy ( dup, s_persist_search_last_pattern[src], plen + 1 );
            if ( s_persist_search_last_pattern[dst] ) {
                std::free ( s_persist_search_last_pattern[dst] );
            }
            s_persist_search_last_pattern[dst] = dup;
        }
    }
}


/* ---- Region group helpers (toolbar Region dropdown) ------------------- */
/*
 * Grupa = logická kategorie regionu pro toolbar dropdown. Per-bank výběr
 * pro Memext/Ramdisk se řeší dynamicky sub-controls (Type combo + Bank
 * combo) - nikoli per-bank items v hlavním dropdown jak tomu bylo dříve.
 *
 * Mapping (kind, sub_id) → grupa je many-to-one (např. všechny Memext RAM
 * banky → MB_RG_MEMEXT). Inverzní mapping grupa+selection → (kind, sub_id)
 * je naopak 1:1 jednoznačný (sub_id se dopočítá z (type_idx, bank_idx)).
 *
 * Sidebar Regions tree (membrowser_regions.cpp) tuto grupu nepoužívá -
 * tam zůstává per-bank zobrazení.
 */

extern "C" en_MEMBROWSER_REGION_GROUP membrowser_region_kind_to_group (
    int region_kind, int sub_id )
{
    switch ( region_kind ) {
        case REGION_KIND_LOGICAL:        return MB_RG_LOGICAL;
        case REGION_KIND_RAM:            return MB_RG_RAM;
        case REGION_KIND_ROM_LOWER:      return MB_RG_ROM_LOWER;
        case REGION_KIND_ROM_UPPER:      return MB_RG_ROM_UPPER;
        case REGION_KIND_CGROM:          return MB_RG_CGROM;
        case REGION_KIND_CGRAM_700:      return MB_RG_CGRAM_700;
        case REGION_KIND_VRAM_700_CHAR:  return MB_RG_VRAM_700_CHAR;
        case REGION_KIND_VRAM_700_ATTR:  return MB_RG_VRAM_700_ATTR;
        case REGION_KIND_PROHIBITED_SHADOW: return MB_RG_PROHIBITED_SHADOW;

        case REGION_KIND_VRAM_PHYS_PLANE:
            switch ( sub_id ) {
                case 0:  return MB_RG_VRAM_PLANE_I;
                case 1:  return MB_RG_VRAM_PLANE_II;
                case 2:  return MB_RG_VRAM_PLANE_III;
                case 3:  return MB_RG_VRAM_PLANE_IV;
                default: return MB_RG_VRAM_PLANE_I;
            }

        case REGION_KIND_PCG_1500:
            switch ( sub_id ) {
                case 0:  return MB_RG_PCG_1;
                case 1:  return MB_RG_PCG_2;
                case 2:  return MB_RG_PCG_3;
                default: return MB_RG_PCG_1;
            }

        case REGION_KIND_MEMEXT_RAM:     return MB_RG_MEMEXT;
        case REGION_KIND_MEMEXT_FLASH:   return MB_RG_MEMEXT;

        case REGION_KIND_RAMDISK_STD:    return MB_RG_RAMDISK_STD;

        case REGION_KIND_RAMDISK_PEZIK:
            /* sub_id = pezik_instance * 8 + bank_idx; 0..7 = port 0x68,
             * 8..15 = port 0xE8 (per dbgapi_regions.h doc). */
            return ( sub_id < 8 ) ? MB_RG_RAMDISK_PEZIK_68
                                  : MB_RG_RAMDISK_PEZIK_E8;

        default:
            return MB_RG_LOGICAL;
    }
}


extern "C" void membrowser_group_to_region (
    en_MEMBROWSER_REGION_GROUP group, int type_idx, int bank_idx,
    int *out_kind, int *out_sub )
{
    if ( !out_kind || !out_sub ) return;

    /* Defaultní výstup - bezpečný fallback pro neznámé grupy. */
    *out_kind = REGION_KIND_LOGICAL;
    *out_sub  = 0;

    switch ( group ) {
        case MB_RG_LOGICAL:        *out_kind = REGION_KIND_LOGICAL;        break;
        case MB_RG_RAM:            *out_kind = REGION_KIND_RAM;            break;
        case MB_RG_ROM_LOWER:      *out_kind = REGION_KIND_ROM_LOWER;      break;
        case MB_RG_ROM_UPPER:      *out_kind = REGION_KIND_ROM_UPPER;      break;
        case MB_RG_CGROM:          *out_kind = REGION_KIND_CGROM;          break;
        case MB_RG_CGRAM_700:      *out_kind = REGION_KIND_CGRAM_700;      break;
        case MB_RG_VRAM_700_CHAR:  *out_kind = REGION_KIND_VRAM_700_CHAR;  break;
        case MB_RG_VRAM_700_ATTR:  *out_kind = REGION_KIND_VRAM_700_ATTR;  break;
        case MB_RG_PROHIBITED_SHADOW: *out_kind = REGION_KIND_PROHIBITED_SHADOW; break;

        case MB_RG_VRAM_PLANE_I:
            *out_kind = REGION_KIND_VRAM_PHYS_PLANE; *out_sub = 0; break;
        case MB_RG_VRAM_PLANE_II:
            *out_kind = REGION_KIND_VRAM_PHYS_PLANE; *out_sub = 1; break;
        case MB_RG_VRAM_PLANE_III:
            *out_kind = REGION_KIND_VRAM_PHYS_PLANE; *out_sub = 2; break;
        case MB_RG_VRAM_PLANE_IV:
            *out_kind = REGION_KIND_VRAM_PHYS_PLANE; *out_sub = 3; break;

        case MB_RG_PCG_1:
            *out_kind = REGION_KIND_PCG_1500; *out_sub = 0; break;
        case MB_RG_PCG_2:
            *out_kind = REGION_KIND_PCG_1500; *out_sub = 1; break;
        case MB_RG_PCG_3:
            *out_kind = REGION_KIND_PCG_1500; *out_sub = 2; break;

        case MB_RG_MEMEXT:
            /* type_idx 0 = RAM, 1 = FLASH. Bank je raw bank index dle dbgapi
             * konvence: RAM 0..0x7F (Luftner) nebo 0..0x3F (PEHU), FLASH
             * 0x80..0xFF (Luftner only). */
            *out_kind = ( type_idx == 1 ) ? REGION_KIND_MEMEXT_FLASH
                                          : REGION_KIND_MEMEXT_RAM;
            *out_sub  = bank_idx;
            break;

        case MB_RG_RAMDISK_STD:
            *out_kind = REGION_KIND_RAMDISK_STD;
            *out_sub  = bank_idx;
            break;

        case MB_RG_RAMDISK_PEZIK_68:
            /* Port 0x68 = pezik instance 0 → sub_id = 0..7. */
            *out_kind = REGION_KIND_RAMDISK_PEZIK;
            *out_sub  = ( bank_idx & 7 );
            break;

        case MB_RG_RAMDISK_PEZIK_E8:
            /* Port 0xE8 = pezik instance 1 → sub_id = 8..15. */
            *out_kind = REGION_KIND_RAMDISK_PEZIK;
            *out_sub  = 8 + ( bank_idx & 7 );
            break;

        default:
            break;
    }
}


extern "C" const char *membrowser_group_label_en (
    en_MEMBROWSER_REGION_GROUP group )
{
    /* Anglické zdrojové stringy - caller obalí _L() pro stabilní ImGui ID.
     * Bez ###StableID suffixu: ID v dropdown řeší ImGui::Selectable pres
     * iteraci PushID/PopID v render loop. */
    switch ( group ) {
        case MB_RG_LOGICAL:             return "Logical Z80";
        case MB_RG_RAM:                 return "User RAM";
        case MB_RG_ROM_LOWER:           return "ROM lower";
        case MB_RG_ROM_UPPER:           return "ROM upper";
        case MB_RG_CGROM:               return "CG-ROM";
        case MB_RG_CGRAM_700:           return "CG-RAM 700";
        case MB_RG_VRAM_PLANE_I:        return "VRAM Plane I";
        case MB_RG_VRAM_PLANE_II:       return "VRAM Plane II";
        case MB_RG_VRAM_PLANE_III:      return "VRAM Plane III";
        case MB_RG_VRAM_PLANE_IV:       return "VRAM Plane IV";
        case MB_RG_VRAM_700_CHAR:       return "VRAM 700 char";
        case MB_RG_VRAM_700_ATTR:       return "VRAM 700 attr";
        case MB_RG_PCG_1:               return "PCG bank 1";
        case MB_RG_PCG_2:               return "PCG bank 2";
        case MB_RG_PCG_3:               return "PCG bank 3";
        case MB_RG_MEMEXT:              return "Memext";
        case MB_RG_RAMDISK_STD:         return "Ramdisk STD";
        case MB_RG_RAMDISK_PEZIK_68:    return "Ramdisk PEZIK 68";
        case MB_RG_RAMDISK_PEZIK_E8:    return "Ramdisk PEZIK E8";
        case MB_RG_PROHIBITED_SHADOW:   return "PROHIBITED shadow";
        default:                        return "?";
    }
}


extern "C" bool membrowser_group_has_subcontrols (
    en_MEMBROWSER_REGION_GROUP group )
{
    switch ( group ) {
        case MB_RG_MEMEXT:
        case MB_RG_RAMDISK_STD:
        case MB_RG_RAMDISK_PEZIK_68:
        case MB_RG_RAMDISK_PEZIK_E8:
            return true;
        default:
            return false;
    }
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
