/*
 * File:   bp_vars.c
 *
 * Implementace globálního storage pro user `$name` proměnné smart BP
 * (V1, fáze D.1.3).
 *
 * Strategie: dynamic array (g_array) `bp_var_t`. Lookup linear
 * (case-sensitive). Typický počet vars per session < 50, lineární
 * lookup je dostatečně rychlý a kód jednoduchý. Hash table by byla
 * over-engineering pro tuto velikost.
 *
 * Ownership: `name` v každém záznamu je heap (g_strdup), uvolňováno
 * při remove / clear_storage / destroy.
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
#include "mzarch/mzhal.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "bp_vars.h"
#include "cfgmain.h"
#include "main.h"   /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"


/**
 * @brief Privátní storage. NULL = neinicializovaný stav (po destroy
 *        nebo před init).
 */
static GArray *s_vars = NULL;


/**
 * @brief Vrátí index záznamu podle jména, nebo -1 pokud neexistuje.
 */
static int bp_vars_find_index ( const char *name ) {
    if ( !s_vars || !name ) return -1;
    guint i;
    for ( i = 0; i < s_vars->len; i++ ) {
        const bp_var_t *v = &g_array_index ( s_vars, bp_var_t, i );
        if ( v->name && strcmp ( v->name, name ) == 0 ) {
            return (int) i;
        };
    };
    return -1;
}


void bp_vars_init ( void ) {
    if ( s_vars ) return;  /* idempotent */
    s_vars = g_array_new ( FALSE, TRUE, sizeof ( bp_var_t ) );
}


void bp_vars_destroy ( void ) {
    if ( !s_vars ) return;
    bp_vars_clear_storage ( );
    g_array_free ( s_vars, TRUE );
    s_vars = NULL;
}


int32_t bp_var_get ( const char *name ) {
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return 0;
    return g_array_index ( s_vars, bp_var_t, (guint) idx ).value;
}


bool bp_var_name_is_valid ( const char *name ) {
    if ( !name || !*name ) return false;

    /* Délkový limit (= konzistentní s BP_VAR_NAME_MAX). */
    size_t len = strlen ( name );
    if ( len > BP_VAR_NAME_MAX ) return false;

    /* První znak: písmeno nebo underscore. */
    char c = name[0];
    if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' ) ) {
        return false;
    };

    /* Pokračování: písmeno, číslice nebo underscore. */
    for ( size_t i = 1; i < len; i++ ) {
        c = name[i];
        if ( !( ( c >= 'a' && c <= 'z' ) ||
                ( c >= 'A' && c <= 'Z' ) ||
                ( c >= '0' && c <= '9' ) ||
                c == '_' ) ) {
            return false;
        };
    };

    /* Žádné reserved keywords (per Michal 2026-05-06 - `$` prefix
     * je distinktivní v action/expression jazyce). */
    return true;
}


void bp_var_set ( const char *name, int32_t value ) {
    if ( !name || !*name ) return;

    /* Defenzivní validace - parser by měl jméno už zkontrolovat,
     * ale belt-and-suspenders pro UI / RPC / ostatní callsites. */
    if ( !bp_var_name_is_valid ( name ) ) {
        fprintf ( stderr, "BP_VARS WARNING: rejecting invalid var name '%s'\n", name );
        return;
    };

    if ( !s_vars ) bp_vars_init ( );

    int idx = bp_vars_find_index ( name );
    if ( idx >= 0 ) {
        /* Update existing - mění se jen value. Comment a persist_value
         * zůstanou beze změny (action `set $x = ...` nemá ambici
         * resetovat user metadata). */
        g_array_index ( s_vars, bp_var_t, (guint) idx ).value = value;
        return;
    };

    /* Nový záznam - default comment NULL, persist_value true. */
    bp_var_t entry;
    entry.name = g_strdup ( name );
    entry.value = value;
    entry.comment = NULL;
    entry.persist_value = true;
    g_array_append_val ( s_vars, entry );
}


void bp_var_set_full ( const char *name,
                       int32_t value,
                       const char *comment,
                       bool persist_value ) {
    if ( !name || !*name ) return;
    if ( !bp_var_name_is_valid ( name ) ) {
        fprintf ( stderr, "BP_VARS WARNING: rejecting invalid var name '%s'\n", name );
        return;
    };

    if ( !s_vars ) bp_vars_init ( );

    int idx = bp_vars_find_index ( name );
    if ( idx >= 0 ) {
        bp_var_t *v = &g_array_index ( s_vars, bp_var_t, (guint) idx );
        v->value = value;
        if ( v->comment ) {
            g_free ( v->comment );
            v->comment = NULL;
        };
        if ( comment && comment[0] ) {
            v->comment = g_strdup ( comment );
        };
        v->persist_value = persist_value;
        return;
    };

    bp_var_t entry;
    entry.name = g_strdup ( name );
    entry.value = value;
    entry.comment = ( comment && comment[0] ) ? g_strdup ( comment ) : NULL;
    entry.persist_value = persist_value;
    g_array_append_val ( s_vars, entry );
}


bool bp_var_set_comment ( const char *name, const char *comment ) {
    if ( !name || !*name ) return false;
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return false;

    bp_var_t *v = &g_array_index ( s_vars, bp_var_t, (guint) idx );
    if ( v->comment ) {
        g_free ( v->comment );
        v->comment = NULL;
    };
    if ( comment && comment[0] ) {
        v->comment = g_strdup ( comment );
    };
    return true;
}


const char* bp_var_get_comment ( const char *name ) {
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return NULL;
    return g_array_index ( s_vars, bp_var_t, (guint) idx ).comment;
}


bool bp_var_set_persist ( const char *name, bool persist ) {
    if ( !name || !*name ) return false;
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return false;

    g_array_index ( s_vars, bp_var_t, (guint) idx ).persist_value = persist;
    return true;
}


bool bp_var_get_persist ( const char *name ) {
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return true;  /* default sémantika "ulož value" */
    return g_array_index ( s_vars, bp_var_t, (guint) idx ).persist_value;
}


bool bp_var_unset ( const char *name ) {
    if ( !name || !*name ) return false;
    int idx = bp_vars_find_index ( name );
    if ( idx < 0 ) return false;

    bp_var_t *v = &g_array_index ( s_vars, bp_var_t, (guint) idx );
    if ( v->name ) {
        g_free ( v->name );
        v->name = NULL;
    };
    if ( v->comment ) {
        g_free ( v->comment );
        v->comment = NULL;
    };
    /* Odstranění z GArray - posune ostatní záznamy. To invaliduje
     * pointery získané z bp_vars_get_by_index před voláním unset. */
    g_array_remove_index ( s_vars, (guint) idx );
    return true;
}


void bp_vars_clear_all ( void ) {
    if ( !s_vars ) return;
    guint i;
    for ( i = 0; i < s_vars->len; i++ ) {
        g_array_index ( s_vars, bp_var_t, i ).value = 0;
    };
}


void bp_vars_clear_storage ( void ) {
    if ( !s_vars ) return;
    guint i;
    for ( i = 0; i < s_vars->len; i++ ) {
        bp_var_t *v = &g_array_index ( s_vars, bp_var_t, i );
        if ( v->name ) {
            g_free ( v->name );
            v->name = NULL;
        };
        if ( v->comment ) {
            g_free ( v->comment );
            v->comment = NULL;
        };
    };
    g_array_set_size ( s_vars, 0 );
}


size_t bp_vars_count ( void ) {
    if ( !s_vars ) return 0;
    return (size_t) s_vars->len;
}


const bp_var_t* bp_vars_get_by_index ( size_t idx ) {
    if ( !s_vars ) return NULL;
    if ( idx >= (size_t) s_vars->len ) return NULL;
    return &g_array_index ( s_vars, bp_var_t, (guint) idx );
}


/* ========================================================================= */
/*  V1.5.B per-arch .vars file - cfg state                                   */
/* ========================================================================= */

/**
 * @brief Cfg-bound stav modulu BP_VARS.
 *
 * Hodnoty plní `cfgmodule_propagate()` v `bp_vars_cfg_init()`. Stringy
 * vlastní cfg knihovna (= nesmí se g_free).
 */
static struct {
    char    *vars_file;     /**< Per-arch default `.vars` filename (relativní k cfg dir) */
    bool     auto_save;     /**< Auto-save při exit do `vars_file` */
    bool     auto_load;     /**< Auto-load při start ze `vars_file` */
} s_vars_cfg = { NULL, true, true };


/* ========================================================================= */
/*  V1.5.B per-arch .vars file - file ops                                    */
/* ========================================================================= */


/**
 * @brief Helper - emit jeden var jako JSON object do builderu.
 *
 * Per-záznam pole:
 *   - "name" (povinné)
 *   - "value" (jen pokud persist_value=true)
 *   - "comment" (jen pokud non-NULL & non-empty)
 *   - "persist_value" (vždy, explicit flag)
 */
static void bp_vars_emit_one ( JsonBuilder *b, const bp_var_t *v ) {
    json_builder_begin_object ( b );
    json_builder_set_member_name ( b, "name" );
    json_builder_add_string_value ( b, v->name );
    if ( v->persist_value ) {
        json_builder_set_member_name ( b, "value" );
        json_builder_add_int_value ( b, (gint64) v->value );
    };
    if ( v->comment && v->comment[0] ) {
        json_builder_set_member_name ( b, "comment" );
        json_builder_add_string_value ( b, v->comment );
    };
    json_builder_set_member_name ( b, "persist_value" );
    json_builder_add_boolean_value ( b, v->persist_value );
    json_builder_end_object ( b );
}


/**
 * @brief Resolve path - NULL = default per-arch file v cfg dir.
 *
 * Caller je zodpovědný za g_free.
 */
static char* bp_vars_resolve_path ( const char *path ) {
    if ( path && path[0] ) {
        /* Explicit path - resolveme relativní vůči cfg dir, absolutní pass-through. */
        return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, path );
    };
    return bp_vars_default_filepath ( );
}


/**
 * @brief Helper pro chybovou zprávu do volitelného errbufu.
 */
static void bp_vars_set_err ( char *errbuf, size_t errbuf_size, const char *fmt, ... ) {
    if ( !errbuf || errbuf_size == 0 ) return;
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( errbuf, errbuf_size, fmt, ap );
    va_end ( ap );
}


char* bp_vars_default_filepath ( void ) {
    /* Jmeno souboru per arch = zmrazeny on-disk kontrakt (mzhal 11g). */
    char fname[32];
    g_snprintf ( fname, sizeof ( fname ), "%s.vars", g_mzhal.arch_name );
    /* Pokud je v cfg `vars_file` explicit nastavený, použij ho. */
    const char *use = ( s_vars_cfg.vars_file && s_vars_cfg.vars_file[0] )
                      ? s_vars_cfg.vars_file : fname;
    if ( !g_sdlapp || !g_sdlapp->paths ) {
        return g_strdup ( use );
    };
    return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, use );
}


bool bp_vars_save_to_filepath ( const char *path, char *errbuf, size_t errbuf_size ) {
    char *resolved = bp_vars_resolve_path ( path );
    if ( !resolved ) {
        bp_vars_set_err ( errbuf, errbuf_size, "cannot resolve .vars filepath" );
        return false;
    };

    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "vars" );
    json_builder_begin_array ( b );
    {
        size_t n = bp_vars_count ( );
        for ( size_t i = 0; i < n; i++ ) {
            const bp_var_t *v = bp_vars_get_by_index ( i );
            if ( !v || !v->name ) continue;
            bp_vars_emit_one ( b, v );
        };
    }
    json_builder_end_array ( b );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    bool ok = true;
    FILE *fp = g_fopen ( resolved, "wb" );
    if ( !fp ) {
        bp_vars_set_err ( errbuf, errbuf_size, "cannot open '%s' for writing", resolved );
        ok = false;
    } else {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) {
            bp_vars_set_err ( errbuf, errbuf_size,
                              "short write to '%s' (%zu of %zu)",
                              resolved, wrote, (size_t) len );
            ok = false;
        };
        fclose ( fp );
    };

    g_free ( out );
    g_object_unref ( gen );
    g_object_unref ( b );
    g_free ( resolved );
    return ok;
}


/**
 * @brief Helper pro string field z JSON objectu (NULL pokud chybí / null).
 */
static const char* bp_vars_json_str_or ( JsonObject *obj, const char *key, const char *def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_string ( node );
}


/**
 * @brief Helper pro int field.
 */
static gint64 bp_vars_json_int_or ( JsonObject *obj, const char *key, gint64 def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_int ( node );
}


/**
 * @brief Helper pro bool field.
 */
static gboolean bp_vars_json_bool_or ( JsonObject *obj, const char *key, gboolean def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_boolean ( node );
}


bool bp_vars_load_from_filepath ( const char *path,
                                   en_BP_VARS_LOAD_MODE mode,
                                   char *errbuf, size_t errbuf_size ) {
    char *resolved = bp_vars_resolve_path ( path );
    if ( !resolved ) {
        bp_vars_set_err ( errbuf, errbuf_size, "cannot resolve .vars filepath" );
        return false;
    };

    /* Soubor neexistuje - REPLACE wipne, MERGE_* nedělá nic (= success). */
    if ( !g_file_test ( resolved, G_FILE_TEST_EXISTS ) ) {
        if ( mode == BP_VARS_LOAD_REPLACE ) {
            bp_vars_clear_storage ( );
        };
        g_free ( resolved );
        return true;
    };

    JsonParser *parser = json_parser_new ( );
    GError *err = NULL;
    if ( !json_parser_load_from_file ( parser, resolved, &err ) ) {
        bp_vars_set_err ( errbuf, errbuf_size, "parse error: %s",
                          err ? err->message : "unknown" );
        if ( err ) g_error_free ( err );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonNode *root = json_parser_get_root ( parser );
    if ( !root || json_node_get_node_type ( root ) != JSON_NODE_OBJECT ) {
        bp_vars_set_err ( errbuf, errbuf_size, "root is not a JSON object" );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonObject *root_obj = json_node_get_object ( root );

    if ( mode == BP_VARS_LOAD_REPLACE ) {
        bp_vars_clear_storage ( );
    };

    if ( !json_object_has_member ( root_obj, "vars" ) ) {
        /* Soubor bez "vars" pole je tolerantně OK (= prázdný stav). */
        g_object_unref ( parser );
        g_free ( resolved );
        return true;
    };

    JsonNode *vn = json_object_get_member ( root_obj, "vars" );
    if ( !vn || json_node_get_node_type ( vn ) != JSON_NODE_ARRAY ) {
        bp_vars_set_err ( errbuf, errbuf_size, "'vars' is not an array" );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonArray *varr = json_node_get_array ( vn );
    guint n = json_array_get_length ( varr );
    for ( guint i = 0; i < n; i++ ) {
        JsonNode *en = json_array_get_element ( varr, i );
        if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
        JsonObject *vo = json_node_get_object ( en );

        const char *vname = bp_vars_json_str_or ( vo, "name", NULL );
        if ( !vname || !vname[0] ) continue;
        if ( !bp_var_name_is_valid ( vname ) ) {
            fprintf ( stderr, "BP_VARS WARNING: skipping invalid var name '%s' in '%s'\n",
                      vname, resolved );
            continue;
        };

        /* Merge mode rozhodnutí. */
        if ( mode == BP_VARS_LOAD_MERGE_SKIP ) {
            int existing = bp_vars_find_index ( vname );
            if ( existing >= 0 ) continue;
        };

        bool persist = bp_vars_json_bool_or ( vo, "persist_value", TRUE ) ? true : false;
        int32_t value = 0;
        if ( persist ) {
            value = (int32_t) bp_vars_json_int_or ( vo, "value", 0 );
        } else if ( json_object_has_member ( vo, "value" ) ) {
            fprintf ( stderr, "BP_VARS WARNING: var '%s' has persist_value=false "
                              "but 'value' present in '%s' - resetting to 0\n",
                      vname, resolved );
        };
        const char *comment = bp_vars_json_str_or ( vo, "comment", NULL );

        /* MERGE_OVERWRITE i REPLACE: bp_var_set_full přepíše existující. */
        bp_var_set_full ( vname, value, comment, persist );
    };

    g_object_unref ( parser );
    g_free ( resolved );
    return true;
}


/* ========================================================================= */
/*  V1.5.B cfg sekce [BP_VARS]                                               */
/* ========================================================================= */


void bp_vars_cfg_init ( void ) {
    static char default_vars_file[32];
    g_snprintf ( default_vars_file, sizeof ( default_vars_file ),
                 "%s.vars", g_mzhal.arch_name );
#define DEFAULT_VARS_FILE default_vars_file

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "BP_VARS" );
    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "vars_file", CFGENTYPE_TEXT, DEFAULT_VARS_FILE );
    cfgelement_bind ( elm, (void*) &s_vars_cfg.vars_file );

    elm = cfgmodule_register_new_element ( cmod, "auto_save", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_vars_cfg.auto_save );

    elm = cfgmodule_register_new_element ( cmod, "auto_load", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_vars_cfg.auto_load );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

#undef DEFAULT_VARS_FILE

    /* Auto-load: REPLACE mode (= soubor je single source of truth pro start).
     * Pokud soubor neexistuje, storage zůstane prázdný (= žádná chyba). */
    if ( s_vars_cfg.auto_load ) {
        char errbuf[256] = {0};
        if ( !bp_vars_load_from_filepath ( NULL, BP_VARS_LOAD_REPLACE, errbuf, sizeof ( errbuf ) ) ) {
            fprintf ( stderr, "BP_VARS WARNING: auto-load failed: %s\n", errbuf );
        };
    };
}


void bp_vars_cfg_auto_save ( void ) {
    if ( !s_vars_cfg.auto_save ) return;
    char errbuf[256] = {0};
    if ( !bp_vars_save_to_filepath ( NULL, errbuf, sizeof ( errbuf ) ) ) {
        fprintf ( stderr, "BP_VARS WARNING: auto-save failed: %s\n", errbuf );
    };
}


/* ========================================================================= */
/*  Privátní helper - vrací index find pro merge_skip (interní expose)       */
/* ========================================================================= */

/* Pozn.: bp_vars_find_index je static, ale potřebujeme ho v load. Definice
 * výše v souboru ji vidí - žádný extra helper nutný. */


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
