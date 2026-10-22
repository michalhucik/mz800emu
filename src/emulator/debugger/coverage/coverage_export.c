/**
 * @file   coverage_export.c
 * @brief  Pokrytí kódu: řádky, sloupce, JSON a export - implementace (viz coverage_export.h).
 *
 * @par Licence: GPLv3
 */

#include "coverage_export.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>


/** @brief Jméno příznaku jednotky pro JSON a CSV. */
typedef struct st_COV_FLAG_NAME {
    uint16_t bit;     /**< Bit COVERAGE_F_*. */
    const char *name; /**< Jméno ve výstupu. */
} st_COV_FLAG_NAME;

/** @brief Příznaky v pořadí výstupu. */
static const st_COV_FLAG_NAME s_flag_names[] = {
    { COVERAGE_F_DECISION,       "decision" },
    { COVERAGE_F_OPERAND_VARIED, "operand_varied" },
    { COVERAGE_F_INT_ENTRY,      "int_entry" },
    { COVERAGE_F_CMT_TRAP,       "cmt_trap" },
    { COVERAGE_F_FETCH_FALLBACK, "fetch_fallback" },
    { COVERAGE_F_SATURATED,      "saturated" },
    { COVERAGE_F_HALT,           "halt" },
    { COVERAGE_F_CELL_BUS,       "cell_bus" },
    { COVERAGE_F_PREFIX_SPLIT,   "prefix_split" },
    { COVERAGE_F_KEY_TRUNCATED,  "key_truncated" },
};

/** @brief Sloupce úrovně branch. */
static const char *const s_cols_branch[] = { "pc", "state", "int_entry", "first_epoch" };
/** @brief Sloupce úrovně detail. */
static const char *const s_cols_detail[] = {
    "pc", "space", "offset", "key", "len", "exec", "taken", "not_taken",
    "state", "operand", "next_pc", "first_epoch", "flags",
};


const char *coverage_level_name ( en_COVERAGE_LEVEL level )
{
    return ( level == COVERAGE_LEVEL_DETAIL ) ? "detail" : "branch";
}


const char *const *coverage_columns ( en_COVERAGE_LEVEL level, size_t *n )
{
    if ( level == COVERAGE_LEVEL_DETAIL ) {
        *n = G_N_ELEMENTS ( s_cols_detail );
        return s_cols_detail;
    };
    *n = G_N_ELEMENTS ( s_cols_branch );
    return s_cols_branch;
}


JsonObject *coverage_status_to_json ( const st_COVERAGE_STATUS *s )
{
    JsonObject *o = json_object_new ( );
    json_object_set_boolean_member ( o, "active", s->active );
    json_object_set_boolean_member ( o, "has_data", s->has_data );
    json_object_set_string_member ( o, "level", coverage_level_name ( s->level ) );
    json_object_set_string_member ( o, "key", ( s->key == COVERAGE_KEY_FULL_BYTES ) ? "full_bytes" : "opcode" );
    json_object_set_boolean_member ( o, "reset_on_discontinuity", s->reset_on_discontinuity );
    json_object_set_boolean_member ( o, "complete", s->complete );
    json_object_set_int_member ( o, "units", (gint64) s->units );
    json_object_set_int_member ( o, "capacity", (gint64) s->capacity );
    json_object_set_int_member ( o, "dropped", (gint64) s->dropped );
    json_object_set_int_member ( o, "fetch_fallback", (gint64) s->fetch_fallback );
    json_object_set_boolean_member ( o, "saturated", s->saturated );
    json_object_set_int_member ( o, "discontinuities", (gint64) s->discontinuities );
    json_object_set_int_member ( o, "host_state_edits", (gint64) s->host_state_edits );
    json_object_set_int_member ( o, "epoch", (gint64) s->epoch );
    json_object_set_int_member ( o, "generation", (gint64) s->generation );
    json_object_set_int_member ( o, "instructions", (gint64) s->instructions );
    json_object_set_int_member ( o, "halt_idle", (gint64) s->halt_idle );
    JsonObject *b = json_object_new ( );
    json_object_set_int_member ( b, "decisions", (gint64) s->decisions );
    json_object_set_int_member ( b, "both", (gint64) s->branches_both );
    json_object_set_int_member ( b, "taken_only", (gint64) s->branches_taken_only );
    json_object_set_int_member ( b, "not_taken_only", (gint64) s->branches_not_taken_only );
    json_object_set_object_member ( o, "branches", b );
    return o;
}


/** @brief Porovnání jednotek pro řazení řádků: PC, buňka, délka klíče, klíč. */
static int cov_unit_cmp ( const void *a, const void *b )
{
    const st_COVERAGE_UNIT *x = (const st_COVERAGE_UNIT *) a, *y = (const st_COVERAGE_UNIT *) b;
    if ( x->pc != y->pc ) return ( x->pc < y->pc ) ? -1 : 1;
    if ( x->cell != y->cell ) return ( x->cell < y->cell ) ? -1 : 1;
    if ( x->key_len != y->key_len ) return ( x->key_len < y->key_len ) ? -1 : 1;
    return memcmp ( x->key, y->key, sizeof ( x->key ) );
}


st_COVERAGE_ROW *coverage_rows_build ( st_COVERAGE_SNAPSHOT *s, size_t *n )
{
    st_COVERAGE_ROW *rows;
    *n = 0;
    if ( s->status.level == COVERAGE_LEVEL_DETAIL ) {
        if ( s->n_units ) qsort ( s->units, s->n_units, sizeof ( st_COVERAGE_UNIT ), cov_unit_cmp );
        rows = g_new0 ( st_COVERAGE_ROW, s->n_units ? s->n_units : 1 );
        for ( uint32_t i = 0; i < s->n_units; i++ ) {
            st_COVERAGE_ROW *r = &rows[ ( *n )++ ];
            r->pc = s->units[ i ].pc;
            r->unit = &s->units[ i ];
            r->k = ( i > 0 && s->units[ i - 1 ].pc == r->pc ) ? (uint16_t) ( rows[ *n - 2 ].k + 1 ) : 0;
        };
    } else {
        rows = g_new0 ( st_COVERAGE_ROW, 0x10000 );
        for ( uint32_t pc = 0; pc < 0x10000; pc++ ) {
            if ( s->exec[ pc >> 3 ] & ( 1u << ( pc & 7u ) ) ) {
                rows[ ( *n )++ ].pc = (uint16_t) pc;
            };
        };
    };
    return rows;
}


/** @brief Bit adresy v bitmapě 8 KiB. */
static bool cov_bit ( const uint8_t *bm, uint16_t pc )
{
    return ( bm[ pc >> 3 ] >> ( pc & 7u ) ) & 1u;
}


const char *coverage_row_state ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r )
{
    if ( r->unit ) {
        return coverage_branch_state_name ( ( r->unit->flags & COVERAGE_F_DECISION ) != 0,
                                            r->unit->taken, r->unit->not_taken );
    };
    return coverage_branch_state_name ( cov_bit ( s->decision, r->pc ), cov_bit ( s->taken, r->pc ),
                                        cov_bit ( s->not_taken, r->pc ) );
}


uint16_t coverage_row_flags ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r )
{
    if ( r->unit ) return r->unit->flags;
    uint16_t f = 0;
    if ( cov_bit ( s->decision, r->pc ) ) f |= COVERAGE_F_DECISION;
    if ( cov_bit ( s->int_entry, r->pc ) ) f |= COVERAGE_F_INT_ENTRY;
    return f;
}


uint32_t coverage_row_epoch ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r )
{
    return r->unit ? r->unit->first_epoch : s->first_epoch[ r->pc ];
}


unsigned coverage_pc_state ( const st_COVERAGE_SNAPSHOT *s, uint16_t pc )
{
    if ( !cov_bit ( s->exec, pc ) ) return 0;
    return coverage_state_bits ( coverage_branch_state_name ( cov_bit ( s->decision, pc ),
                                                              cov_bit ( s->taken, pc ),
                                                              cov_bit ( s->not_taken, pc ) ) );
}


void coverage_filter_init ( st_COVERAGE_FILTER *f )
{
    memset ( f, 0, sizeof ( *f ) );
    f->pc_to = 0xFFFF;
}


bool coverage_row_match ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r,
                          const st_COVERAGE_FILTER *f )
{
    if ( r->pc < f->pc_from || r->pc > f->pc_to ) return false;
    if ( f->has_space && ( !r->unit || COVERAGE_CELL_SPACE ( r->unit->cell ) != f->space ) ) return false;
    if ( f->has_epoch && coverage_row_epoch ( s, r ) < f->epoch_from ) return false;
    if ( f->state_mask && !( f->state_mask & coverage_state_bits ( coverage_row_state ( s, r ) ) ) ) return false;
    if ( f->flags_any && !( f->flags_any & coverage_row_flags ( s, r ) ) ) return false;
    return true;
}


unsigned coverage_state_bits ( const char *name )
{
    if ( g_strcmp0 ( name, "none" ) == 0 ) return COVERAGE_STATE_NONE;
    if ( g_strcmp0 ( name, "unobserved" ) == 0 ) return COVERAGE_STATE_UNOBSERVED;
    if ( g_strcmp0 ( name, "taken_only" ) == 0 ) return COVERAGE_STATE_TAKEN_ONLY;
    if ( g_strcmp0 ( name, "not_taken_only" ) == 0 ) return COVERAGE_STATE_NOT_TAKEN_ONLY;
    if ( g_strcmp0 ( name, "both" ) == 0 ) return COVERAGE_STATE_BOTH;
    if ( g_strcmp0 ( name, "partial" ) == 0 ) return COVERAGE_STATE_TAKEN_ONLY | COVERAGE_STATE_NOT_TAKEN_ONLY;
    if ( g_strcmp0 ( name, "decision" ) == 0 ) {
        return COVERAGE_STATE_UNOBSERVED | COVERAGE_STATE_TAKEN_ONLY
               | COVERAGE_STATE_NOT_TAKEN_ONLY | COVERAGE_STATE_BOTH;
    };
    return 0;
}


uint16_t coverage_flag_from_name ( const char *name )
{
    for ( size_t k = 0; k < G_N_ELEMENTS ( s_flag_names ); k++ ) {
        if ( g_strcmp0 ( name, s_flag_names[ k ].name ) == 0 ) return s_flag_names[ k ].bit;
    };
    return 0;
}


/** @brief Bajty klíče jednotky jako hex řetězec (např. "DDCB46"); g_free. */
static char *cov_key_hex ( const st_COVERAGE_UNIT *u )
{
    GString *g = g_string_new ( NULL );
    for ( unsigned i = 0; i < u->key_len && i < 4; i++ ) g_string_append_printf ( g, "%02X", u->key[ i ] );
    return g_string_free ( g, FALSE );
}


/** @brief Operand má smysl (krok má bajty za klíčem). */
static bool cov_has_operand ( const st_COVERAGE_UNIT *u )
{
    return u->insn_len > u->key_len && !( u->flags & COVERAGE_F_PREFIX_SPLIT );
}


/** @brief Pole jmen příznaků. */
static JsonArray *cov_flags_array ( uint16_t flags )
{
    JsonArray *a = json_array_new ( );
    for ( size_t k = 0; k < G_N_ELEMENTS ( s_flag_names ); k++ ) {
        if ( flags & s_flag_names[ k ].bit ) json_array_add_string_element ( a, s_flag_names[ k ].name );
    };
    return a;
}


JsonNode *coverage_row_column ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r, const char *col )
{
    const st_COVERAGE_UNIT *u = r->unit;
    if ( strcmp ( col, "flags" ) == 0 ) {
        JsonNode *n = json_node_new ( JSON_NODE_ARRAY );
        json_node_take_array ( n, cov_flags_array ( coverage_row_flags ( s, r ) ) );
        return n;
    };
    if ( u && strcmp ( col, "operand" ) == 0 && !cov_has_operand ( u ) ) {
        return json_node_new ( JSON_NODE_NULL );
    };
    JsonNode *n = json_node_new ( JSON_NODE_VALUE );
    if ( strcmp ( col, "pc" ) == 0 ) json_node_set_int ( n, r->pc );
    else if ( strcmp ( col, "state" ) == 0 ) json_node_set_string ( n, coverage_row_state ( s, r ) );
    else if ( strcmp ( col, "int_entry" ) == 0 ) json_node_set_boolean ( n, ( coverage_row_flags ( s, r ) & COVERAGE_F_INT_ENTRY ) != 0 );
    else if ( strcmp ( col, "first_epoch" ) == 0 ) json_node_set_int ( n, coverage_row_epoch ( s, r ) );
    else if ( u && strcmp ( col, "space" ) == 0 ) json_node_set_string ( n, coverage_space_name ( COVERAGE_CELL_SPACE ( u->cell ) ) );
    else if ( u && strcmp ( col, "offset" ) == 0 ) json_node_set_int ( n, COVERAGE_CELL_OFFSET ( u->cell ) );
    else if ( u && strcmp ( col, "key" ) == 0 ) {
        char *h = cov_key_hex ( u );
        json_node_set_string ( n, h );
        g_free ( h );
    }
    else if ( u && strcmp ( col, "len" ) == 0 ) json_node_set_int ( n, u->insn_len );
    else if ( u && strcmp ( col, "exec" ) == 0 ) json_node_set_int ( n, u->exec );
    else if ( u && strcmp ( col, "taken" ) == 0 ) json_node_set_int ( n, u->taken );
    else if ( u && strcmp ( col, "not_taken" ) == 0 ) json_node_set_int ( n, u->not_taken );
    else if ( u && strcmp ( col, "operand" ) == 0 ) json_node_set_int ( n, u->operand_last );
    else if ( u && strcmp ( col, "next_pc" ) == 0 ) json_node_set_int ( n, u->next_pc_last );
    else {
        json_node_free ( n );
        return json_node_new ( JSON_NODE_NULL );
    };
    return n;
}


/**
 * @brief Hodnota sloupce jako text pro CSV (příznaky oddělené `|`, null prázdně).
 * @return Nový řetězec (g_free).
 */
static char *cov_column_csv ( const st_COVERAGE_SNAPSHOT *s, const st_COVERAGE_ROW *r, const char *col )
{
    if ( strcmp ( col, "flags" ) == 0 ) {
        GString *g = g_string_new ( NULL );
        uint16_t fl = coverage_row_flags ( s, r );
        for ( size_t k = 0; k < G_N_ELEMENTS ( s_flag_names ); k++ ) {
            if ( fl & s_flag_names[ k ].bit ) {
                if ( g->len ) g_string_append_c ( g, '|' );
                g_string_append ( g, s_flag_names[ k ].name );
            };
        };
        return g_string_free ( g, FALSE );
    };
    JsonNode *n = coverage_row_column ( s, r, col );
    char *t;
    if ( JSON_NODE_HOLDS_NULL ( n ) ) t = g_strdup ( "" );
    else if ( json_node_get_value_type ( n ) == G_TYPE_STRING ) t = g_strdup ( json_node_get_string ( n ) );
    else if ( json_node_get_value_type ( n ) == G_TYPE_BOOLEAN ) t = g_strdup ( json_node_get_boolean ( n ) ? "1" : "0" );
    else t = g_strdup_printf ( "%" G_GINT64_FORMAT, json_node_get_int ( n ) );
    json_node_free ( n );
    return t;
}


bool coverage_export_write ( st_COVERAGE_SNAPSHOT *s, const char *path, bool csv,
                             size_t *out_rows, char **out_err )
{
    size_t n = 0, n_cols = 0;
    st_COVERAGE_ROW *rows = coverage_rows_build ( s, &n );
    const char *const *cols = coverage_columns ( s->status.level, &n_cols );
    gchar *text;
    if ( out_err ) *out_err = NULL;

    if ( csv ) {
        GString *g = g_string_new ( NULL );
        for ( size_t c = 0; c < n_cols; c++ ) g_string_append_printf ( g, "%s%s", c ? "," : "", cols[ c ] );
        g_string_append_c ( g, '\n' );
        for ( size_t i = 0; i < n; i++ ) {
            for ( size_t c = 0; c < n_cols; c++ ) {
                char *v = cov_column_csv ( s, &rows[ i ], cols[ c ] );
                g_string_append_printf ( g, "%s%s", c ? "," : "", v );
                g_free ( v );
            };
            g_string_append_c ( g, '\n' );
        };
        text = g_string_free ( g, FALSE );
    } else {
        JsonObject *root = json_object_new ( );
        json_object_set_string_member ( root, "format", "mz800emu-coverage/1" );
        json_object_set_object_member ( root, "status", coverage_status_to_json ( &s->status ) );
        JsonArray *ca = json_array_new ( );
        for ( size_t c = 0; c < n_cols; c++ ) json_array_add_string_element ( ca, cols[ c ] );
        json_object_set_array_member ( root, "columns", ca );
        JsonArray *ra = json_array_new ( );
        for ( size_t i = 0; i < n; i++ ) {
            JsonObject *ro = json_object_new ( );
            for ( size_t c = 0; c < n_cols; c++ ) {
                json_object_set_member ( ro, cols[ c ], coverage_row_column ( s, &rows[ i ], cols[ c ] ) );
            };
            json_array_add_object_element ( ra, ro );
        };
        json_object_set_array_member ( root, "rows", ra );
        JsonNode *rn = json_node_new ( JSON_NODE_OBJECT );
        json_node_take_object ( rn, root );
        text = json_to_string ( rn, TRUE );
        json_node_free ( rn );
    };
    g_free ( rows );

    GError *err = NULL;
    gboolean ok = g_file_set_contents ( path, text, -1, &err );
    g_free ( text );
    if ( !ok ) {
        if ( out_err ) *out_err = g_strdup_printf ( "cannot write %s: %s", path, err ? err->message : "unknown error" );
        if ( err ) g_error_free ( err );
        return false;
    };
    if ( out_rows ) *out_rows = n;
    return true;
}
