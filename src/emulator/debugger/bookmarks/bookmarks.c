/*
 * File:   bookmarks.c
 *
 * Implementace storage pro pojmenované adresové záložky.
 *
 * Strategie: dynamic array (g_array) `bookmark_t` + monotonic ID
 * counter. Lookup linear (= UI rendering nepoužívá lookup po ID v
 * hot-path, jen po indexu). Persistence do JSON přes json-glib.
 *
 * Ownership: `user_input` a `comment` v každém záznamu jsou heap
 * (g_strdup), uvolňovány při remove / clear / cleanup.
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
#include "mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "bookmarks.h"
#include "debugger/symbols/sym_db.h"
#include "cfgmain.h"
#include "main.h"
#include "libs/sdlapp/sdlapp_paths.h"


/**
 * @brief Privátní storage. NULL = neinicializovaný stav.
 */
static GArray  *s_bookmarks    = NULL;

/**
 * @brief Mutex chránící storage proti race read vs. write.
 *
 * V1.D.2.B - storage byl historicky single-thread UI guarantee (= UI
 * jediný čtenář i zapisovatel), ale MCP Resource `emulator://bookmarks`
 * čte přes `bookmarks_snapshot()` z MCP I/O threadu. Mutex chrání
 * read snapshot proti souběžné mutaci z UI vlákna.
 *
 * Politika lockování: VŠECHNY mutace storage (add, remove, clear,
 * set_user_input, set_comment, set_cmd_origin, load, merge, cleanup)
 * berou tento mutex. Lookup helpery
 * (bookmarks_get_by_index, bookmarks_get_by_id, bookmarks_count,
 * bookmarks_resolve_addr) mutex NEbere - jejich kontrakt zůstává
 * single-thread UI per existující bookmarks.h:16-17. MCP read jde
 * výhradně přes thread-safe `bookmarks_snapshot()` který mutex bere.
 *
 * Init je lazy (přes g_once / atomic flag) - bookmarks_init() ho volá
 * idempotentně. Cleanup mutex neuvolňuje (statický, lazy-init).
 */
static GMutex s_bookmarks_mutex;
static gsize  s_bookmarks_mutex_init_once = 0;

/**
 * @brief Lazy init mutexu. Idempotentní, thread-safe přes
 *        g_once_init_enter/leave (po skončení mají všechna vlákna
 *        viditelný plně inicializovaný mutex).
 */
static void bookmarks_mutex_ensure ( void ) {
    if ( g_once_init_enter ( &s_bookmarks_mutex_init_once ) )
    {
        g_mutex_init ( &s_bookmarks_mutex );
        g_once_init_leave ( &s_bookmarks_mutex_init_once, 1 );
    };
}

/**
 * @brief Monotonic ID counter. Inkrementuje se pro každé add /
 *        merge insert, nikdy se nedekrementuje (= ID jsou unique).
 *
 * Začíná na 1 (= 0 je rezervováno pro "invalid / no id").
 */
static uint32_t s_next_id      = 1;

/**
 * @brief Verze storage. Inkrementuje se při každé mutaci.
 */
static unsigned s_version      = 0;


/**
 * @brief Default filepath cache (pro return v
 *        bookmarks_default_filepath() jako read-only pointer).
 *
 * Resolve probíhá při každém volání (= cesta může záviset na cfg
 * stavu), cache se realokuje. Lifetime do cleanup nebo do dalšího
 * volání.
 */
static char    *s_default_filepath_cache = NULL;


/**
 * @brief Cfg-bound stav pro [BOOKMARKS] sekci.
 *
 * Plní cfgmodule_propagate v `bookmarks_cfg_init()`. Stringy vlastní
 * cfg knihovna (= nesmí se g_free).
 */
static struct {
    char    *bookmarks_file;    /**< Per-arch default `.bookmarks` filename */
    bool     auto_save;         /**< Auto-save při cleanup */
    bool     auto_load;         /**< Auto-load při init */
} s_bm_cfg = { NULL, true, true };


/* ========================================================================= */
/*  Privátní helpery                                                         */
/* ========================================================================= */


/**
 * @brief Bumpne version counter (volat po každé mutaci).
 */
static void bookmarks_bump_version ( void ) {
    s_version++;
    if ( s_version == 0 ) s_version = 1;  /* skok přes 0 (= sentinel "neinit") */
}


/**
 * @brief Najde index záznamu podle ID, nebo -1 pokud neexistuje.
 */
static int bookmarks_find_index_by_id ( uint32_t id ) {
    if ( !s_bookmarks || id == 0 ) return -1;
    guint i;
    for ( i = 0; i < s_bookmarks->len; i++ ) {
        const bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, i );
        if ( bm->id == id ) return (int) i;
    };
    return -1;
}


/**
 * @brief Helper - copy string s normalizací prázdného -> NULL.
 */
static char* bookmarks_dup_or_null ( const char *s ) {
    if ( !s || !s[0] ) return NULL;
    return g_strdup ( s );
}


/**
 * @brief Helper - chyba do volitelného errbufu (logging nepoužíváme;
 *        save/load funkce nemají errbuf API per public API spec, takže
 *        warning rovnou na stderr).
 */
static void bookmarks_warn ( const char *fmt, ... ) {
    va_list ap;
    va_start ( ap, fmt );
    fprintf ( stderr, "BOOKMARKS WARNING: " );
    vfprintf ( stderr, fmt, ap );
    fputc ( '\n', stderr );
    va_end ( ap );
}


/* ========================================================================= */
/*  Lifecycle                                                                */
/* ========================================================================= */


void bookmarks_init ( void ) {
    if ( !s_bookmarks ) {
        s_bookmarks = g_array_new ( FALSE, TRUE, sizeof ( bookmark_t ) );
        s_next_id = 1;
        s_version = 1;
    };

    /* Cfg sekce + auto-load. cfg_init je idempotentní (= cfgmain
     * by neměl re-registrovat existující modul, ale defenzivně
     * voláme jen při prvním init). */
    bookmarks_cfg_init ( );
}


void bookmarks_cleanup ( void ) {
    if ( !s_bookmarks ) return;

    /* Auto-save před wipe storage. */
    if ( s_bm_cfg.auto_save ) {
        if ( !bookmarks_save ( NULL ) ) {
            bookmarks_warn ( "auto-save failed" );
        };
    };

    bookmarks_clear ( );
    g_array_free ( s_bookmarks, TRUE );
    s_bookmarks = NULL;
    s_next_id = 1;
    s_version = 0;

    if ( s_default_filepath_cache ) {
        g_free ( s_default_filepath_cache );
        s_default_filepath_cache = NULL;
    };
}


unsigned bookmarks_get_version ( void ) {
    return s_version;
}


/* ========================================================================= */
/*  CRUD                                                                     */
/* ========================================================================= */


uint32_t bookmarks_add ( const char *user_input, const char *comment ) {
    if ( !user_input || !user_input[0] ) return 0;
    if ( !s_bookmarks ) bookmarks_init ( );
    bookmarks_mutex_ensure ( );

    g_mutex_lock ( &s_bookmarks_mutex );
    bookmark_t entry;
    entry.id          = s_next_id++;
    entry.user_input  = g_strdup ( user_input );
    entry.comment     = bookmarks_dup_or_null ( comment );
    entry.cmd_origin  = DBGAPI_CMD_ORIGIN_USER;  /* V1.C.3 default - GUI tvorba */
    g_array_append_val ( s_bookmarks, entry );
    bookmarks_bump_version ( );
    uint32_t new_id = entry.id;
    g_mutex_unlock ( &s_bookmarks_mutex );
    return new_id;
}


bool bookmarks_remove ( uint32_t id ) {
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    int idx = bookmarks_find_index_by_id ( id );
    if ( idx < 0 ) { g_mutex_unlock ( &s_bookmarks_mutex ); return false; };

    bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
    if ( bm->user_input ) { g_free ( bm->user_input ); bm->user_input = NULL; };
    if ( bm->comment    ) { g_free ( bm->comment );    bm->comment    = NULL; };
    g_array_remove_index ( s_bookmarks, (guint) idx );
    bookmarks_bump_version ( );
    g_mutex_unlock ( &s_bookmarks_mutex );
    return true;
}


bool bookmarks_set_user_input ( uint32_t id, const char *new_input ) {
    if ( !new_input || !new_input[0] ) return false;
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    int idx = bookmarks_find_index_by_id ( id );
    if ( idx < 0 ) { g_mutex_unlock ( &s_bookmarks_mutex ); return false; };

    bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
    if ( bm->user_input ) g_free ( bm->user_input );
    bm->user_input = g_strdup ( new_input );
    bookmarks_bump_version ( );
    g_mutex_unlock ( &s_bookmarks_mutex );
    return true;
}


bool bookmarks_set_comment ( uint32_t id, const char *new_comment ) {
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    int idx = bookmarks_find_index_by_id ( id );
    if ( idx < 0 ) { g_mutex_unlock ( &s_bookmarks_mutex ); return false; };

    bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
    if ( bm->comment ) { g_free ( bm->comment ); bm->comment = NULL; };
    bm->comment = bookmarks_dup_or_null ( new_comment );
    bookmarks_bump_version ( );
    g_mutex_unlock ( &s_bookmarks_mutex );
    return true;
}


bool bookmarks_set_cmd_origin ( uint32_t id, en_DBGAPI_CMD_ORIGIN origin ) {
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    int idx = bookmarks_find_index_by_id ( id );
    if ( idx < 0 ) { g_mutex_unlock ( &s_bookmarks_mutex ); return false; };

    bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
    bm->cmd_origin = origin;
    /* bookmarks_bump_version() záměrně NEvoláme - origin je metadata
     * bez efektu na resolve ani UI cache, žádná invalidace není nutná. */
    g_mutex_unlock ( &s_bookmarks_mutex );
    return true;
}


/**
 * @brief Interní varianta bookmarks_clear bez zamykání mutexu.
 *
 * Použít POUZE z kontextu kde caller už drží s_bookmarks_mutex
 * (typicky bookmarks_load_internal). Public bookmarks_clear() je
 * tenký lock-around wrapper.
 */
static void bookmarks_clear_locked ( void ) {
    if ( !s_bookmarks ) return;
    guint i;
    for ( i = 0; i < s_bookmarks->len; i++ ) {
        bookmark_t *bm = &g_array_index ( s_bookmarks, bookmark_t, i );
        if ( bm->user_input ) { g_free ( bm->user_input ); bm->user_input = NULL; };
        if ( bm->comment    ) { g_free ( bm->comment );    bm->comment    = NULL; };
    };
    g_array_set_size ( s_bookmarks, 0 );
    bookmarks_bump_version ( );
}


void bookmarks_clear ( void ) {
    if ( !s_bookmarks ) return;
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    bookmarks_clear_locked ( );
    g_mutex_unlock ( &s_bookmarks_mutex );
}


/**
 * @brief V1.D.2.B - Thread-safe snapshot bookmark storage.
 *
 * Caller alokuje pole `out` o velikosti `capacity`. Funkce pod zámkem
 * překopíruje aktuální stav do `out` (shallow copy - char pointery
 * z g_strdup ve storage zůstávají vlastněny storage; caller je NEsmí
 * uvolnit). `out_count` = počet zapsaných záznamů, `truncated` = true
 * pokud storage > capacity. Mutex se drží jen po dobu kopírování.
 *
 * Použití z dispatch.c (DBGAPI_CMD_BOOKMARKS_LIST) - caller dál nepoužívá
 * `out[].user_input` / `comment` jako stabilní pointery (= jsou
 * platné jen dokud žádné jiné vlákno neudělá mutaci storage); typicky
 * je hned překopíruje do fixed-size st_DBGAPI_BOOKMARK_ENTRY bufferu
 * a `out` zahodí.
 *
 * @param out         Caller-allocated pole (může být NULL pokud capacity=0).
 * @param capacity    Velikost `out` (max počet zapsaných záznamů).
 * @param out_count   (OUT) Počet zapsaných záznamů. NULL = nezapsáno.
 * @param truncated   (OUT) true pokud storage > capacity. NULL = nezapsáno.
 */
void bookmarks_snapshot ( bookmark_t *out, size_t capacity,
                          size_t *out_count, bool *truncated ) {
    if ( out_count ) *out_count = 0;
    if ( truncated ) *truncated = false;
    bookmarks_mutex_ensure ( );
    g_mutex_lock ( &s_bookmarks_mutex );
    size_t total = s_bookmarks ? (size_t) s_bookmarks->len : 0;
    size_t n     = ( total > capacity ) ? capacity : total;
    if ( out && capacity > 0 ) {
        for ( size_t i = 0; i < n; i++ ) {
            out[ i ] = g_array_index ( s_bookmarks, bookmark_t, (guint) i );
        };
    };
    if ( out_count ) *out_count = n;
    if ( truncated ) *truncated = ( total > capacity );
    g_mutex_unlock ( &s_bookmarks_mutex );
}


/* ========================================================================= */
/*  Iterace / lookup                                                         */
/* ========================================================================= */


size_t bookmarks_count ( void ) {
    if ( !s_bookmarks ) return 0;
    return (size_t) s_bookmarks->len;
}


const bookmark_t *bookmarks_get_by_index ( size_t idx ) {
    if ( !s_bookmarks ) return NULL;
    if ( idx >= (size_t) s_bookmarks->len ) return NULL;
    return &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
}


const bookmark_t *bookmarks_get_by_id ( uint32_t id ) {
    int idx = bookmarks_find_index_by_id ( id );
    if ( idx < 0 ) return NULL;
    return &g_array_index ( s_bookmarks, bookmark_t, (guint) idx );
}


/* ========================================================================= */
/*  Resolve helper                                                           */
/* ========================================================================= */


/**
 * @brief Trim whitespace na obou koncích, výsledek do bufferu.
 *
 * @param src      Vstupní string.
 * @param outbuf   Cílový buffer.
 * @param outsize  Velikost bufferu.
 * @return Počet zkopírovaných znaků (bez terminátoru) nebo -1 při
 *         přetečení (out je v takovém případě prázdný).
 */
static int bookmarks_trim_copy ( const char *src, char *outbuf, size_t outsize ) {
    if ( !src || outsize == 0 ) {
        if ( outbuf && outsize ) outbuf[0] = '\0';
        return 0;
    };
    while ( *src && isspace ( (unsigned char) *src ) ) src++;
    const char *end = src + strlen ( src );
    while ( end > src && isspace ( (unsigned char) *( end - 1 ) ) ) end--;
    size_t len = (size_t) ( end - src );
    if ( len + 1 > outsize ) {
        outbuf[0] = '\0';
        return -1;
    };
    memcpy ( outbuf, src, len );
    outbuf[len] = '\0';
    return (int) len;
}


/**
 * @brief Helper - parse hex digit, vrací -1 pro non-hex.
 */
static int bookmarks_hex_digit ( char c ) {
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return 10 + ( c - 'a' );
    if ( c >= 'A' && c <= 'F' ) return 10 + ( c - 'A' );
    return -1;
}


/**
 * @brief Pokus o parse hex literálu z trimnutého stringu.
 *
 * Akceptované formáty (case-insensitive):
 *   - "0x1234", "0X1234"
 *   - "#1234"
 *   - "$1234"
 *   - "1234h", "01234h"
 *   - holé "1234" (max 4 znaky, všechny hex digit)
 *
 * @return true při úspěchu (out_val obsahuje 16-bit hodnotu).
 */
static bool bookmarks_parse_hex ( const char *trimmed, uint16_t *out_val ) {
    if ( !trimmed || !trimmed[0] ) return false;

    size_t len = strlen ( trimmed );
    const char *digits = NULL;
    size_t      ndigits = 0;

    /* Prefix variants */
    if ( len >= 3 && ( trimmed[0] == '0' ) &&
         ( trimmed[1] == 'x' || trimmed[1] == 'X' ) ) {
        digits = trimmed + 2;
        ndigits = len - 2;
    } else if ( len >= 2 && ( trimmed[0] == '#' || trimmed[0] == '$' ) ) {
        digits = trimmed + 1;
        ndigits = len - 1;
    }
    /* Suffix `h` / `H` */
    else if ( len >= 2 && ( trimmed[len - 1] == 'h' || trimmed[len - 1] == 'H' ) ) {
        digits = trimmed;
        ndigits = len - 1;
    }
    /* Holé hex digity (max 4 znaky) */
    else if ( len >= 1 && len <= 4 ) {
        digits = trimmed;
        ndigits = len;
    } else {
        return false;
    };

    if ( ndigits == 0 || ndigits > 8 ) return false;  /* max 8 hex digits, ořežeme na 16-bit */

    uint32_t val = 0;
    for ( size_t i = 0; i < ndigits; i++ ) {
        int d = bookmarks_hex_digit ( digits[i] );
        if ( d < 0 ) return false;
        val = ( val << 4 ) | (uint32_t) d;
    };

    if ( out_val ) *out_val = (uint16_t) ( val & 0xFFFF );
    return true;
}


bool bookmarks_resolve_addr ( const char *user_input, uint16_t *addr_out ) {
    if ( !user_input || !user_input[0] ) return false;

    char trimmed[BOOKMARK_INPUT_MAX + 1];
    int n = bookmarks_trim_copy ( user_input, trimmed, sizeof ( trimmed ) );
    if ( n <= 0 ) return false;

    /* Hex parse */
    uint16_t v = 0;
    if ( bookmarks_parse_hex ( trimmed, &v ) ) {
        if ( addr_out ) *addr_out = v;
        return true;
    };

    /* Symbol fallback */
    const st_SYMBOL *sym = sym_db_lookup_by_name ( trimmed );
    if ( !sym ) return false;
    if ( addr_out ) *addr_out = (uint16_t) ( sym->addr & 0xFFFF );
    return true;
}


/* ========================================================================= */
/*  Persistence - JSON                                                       */
/* ========================================================================= */


/**
 * @brief Resolve filepath - NULL = default per-arch, jinak relativní
 *        vůči cfg dir (absolutní pass-through).
 *
 * Caller je zodpovědný za g_free.
 */
static char* bookmarks_resolve_path ( const char *path ) {
    if ( path && path[0] ) {
        if ( !g_sdlapp || !g_sdlapp->paths ) return g_strdup ( path );
        return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, path );
    };
    /* Default per-arch - runtime z g_mzhal (mzhal krok 7), jméno
     * identické s dřívějším #if MZARCH řetězem ("mz800.bookmarks", ...).
     * Statický buffer: plní se při prvním volání, g_mzhal je const. */
    static char default_fname[32];
    if ( default_fname[0] == 0x00 ) {
        snprintf ( default_fname, sizeof ( default_fname ), "%s.bookmarks", g_mzhal.arch_name );
    };
    const char *fname = default_fname;
    /* Pokud cfg má override, použij ten */
    const char *use = ( s_bm_cfg.bookmarks_file && s_bm_cfg.bookmarks_file[0] )
                      ? s_bm_cfg.bookmarks_file : fname;
    if ( !g_sdlapp || !g_sdlapp->paths ) return g_strdup ( use );
    return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, use );
}


const char *bookmarks_default_filepath ( void ) {
    if ( s_default_filepath_cache ) {
        g_free ( s_default_filepath_cache );
        s_default_filepath_cache = NULL;
    };
    s_default_filepath_cache = bookmarks_resolve_path ( NULL );
    return s_default_filepath_cache;
}


/**
 * @brief Helpery pro JSON readout.
 */
static const char* bm_json_str_or ( JsonObject *obj, const char *key, const char *def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_string ( node );
}


static gint64 bm_json_int_or ( JsonObject *obj, const char *key, gint64 def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_int ( node );
}


/**
 * @brief V1.D.2.C - en_DBGAPI_CMD_ORIGIN -> stable persist token.
 *
 * Tokeny "user" / "mcp" / "test" / "internal" jsou stabilní napříč
 * verzemi schématu - loader je tolerantní k unknown tokenům
 * (default = USER).
 */
static const char *_bookmarks_origin_to_str ( en_DBGAPI_CMD_ORIGIN o ) {
    switch ( o ) {
        case DBGAPI_CMD_ORIGIN_USER:     return "user";
        case DBGAPI_CMD_ORIGIN_MCP:      return "mcp";
        case DBGAPI_CMD_ORIGIN_TEST:     return "test";
        case DBGAPI_CMD_ORIGIN_INTERNAL: return "internal";
    }
    return "user";
}


/**
 * @brief V1.D.2.C - parse stable persist token -> en_DBGAPI_CMD_ORIGIN.
 *
 * Tolerantní k neznámým tokenům: fallback na USER (= GUI default).
 */
static en_DBGAPI_CMD_ORIGIN _bookmarks_origin_from_str ( const char *s ) {
    if ( !s || !s[0] ) return DBGAPI_CMD_ORIGIN_USER;
    if ( strcmp ( s, "mcp" )      == 0 ) return DBGAPI_CMD_ORIGIN_MCP;
    if ( strcmp ( s, "test" )     == 0 ) return DBGAPI_CMD_ORIGIN_TEST;
    if ( strcmp ( s, "internal" ) == 0 ) return DBGAPI_CMD_ORIGIN_INTERNAL;
    return DBGAPI_CMD_ORIGIN_USER;
}


bool bookmarks_save ( const char *filepath ) {
    char *resolved = bookmarks_resolve_path ( filepath );
    if ( !resolved ) {
        bookmarks_warn ( "cannot resolve .bookmarks filepath" );
        return false;
    };

    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "version" );
    json_builder_add_int_value ( b, 1 );

    json_builder_set_member_name ( b, "bookmarks" );
    json_builder_begin_array ( b );
    {
        size_t n = bookmarks_count ( );
        for ( size_t i = 0; i < n; i++ ) {
            const bookmark_t *bm = bookmarks_get_by_index ( i );
            if ( !bm || !bm->user_input ) continue;
            json_builder_begin_object ( b );
            json_builder_set_member_name ( b, "id" );
            json_builder_add_int_value ( b, (gint64) bm->id );
            json_builder_set_member_name ( b, "input" );
            json_builder_add_string_value ( b, bm->user_input );
            json_builder_set_member_name ( b, "comment" );
            json_builder_add_string_value ( b, bm->comment ? bm->comment : "" );
            /* V1.D.2.C - persist cmd_origin pro audit trail. Tolerantní
             * load: chybějící klíč defaultuje na "user" (= viz parse_entry
             * a load_internal). Origin slouží pro forensic analýzu kde
             * záložka vznikla (= GUI klik vs MCP klient vs test). */
            json_builder_set_member_name ( b, "origin" );
            json_builder_add_string_value ( b,
                _bookmarks_origin_to_str ( bm->cmd_origin ) );
            json_builder_end_object ( b );
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
        bookmarks_warn ( "cannot open '%s' for writing", resolved );
        ok = false;
    } else {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) {
            bookmarks_warn ( "short write to '%s' (%zu of %zu)",
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
 * @brief Interní helper pro load - parse jeden JSON object jako bookmark.
 *        Nově alokovaný záznam je předán callerem (caller vlastní paměť).
 *        ID, user_input a comment jsou výsledné parsed hodnoty.
 *
 * @return true pokud byl entry úspěšně extrahován (= mandatory fields).
 */
static bool bookmarks_parse_entry ( JsonObject *obj,
                                    uint32_t *out_id,
                                    char **out_input,
                                    char **out_comment,
                                    en_DBGAPI_CMD_ORIGIN *out_origin ) {
    *out_id      = (uint32_t) bm_json_int_or ( obj, "id", 0 );
    const char *input   = bm_json_str_or ( obj, "input", NULL );
    const char *comment = bm_json_str_or ( obj, "comment", NULL );
    if ( !input || !input[0] ) return false;
    *out_input = g_strdup ( input );
    *out_comment = ( comment && comment[0] ) ? g_strdup ( comment ) : NULL;
    /* V1.D.2.C - tolerantní origin load: chybějící klíč -> USER. */
    if ( out_origin ) {
        const char *origin_str = bm_json_str_or ( obj, "origin", NULL );
        *out_origin = _bookmarks_origin_from_str ( origin_str );
    };
    return true;
}


/**
 * @brief Interní load helper. mode_replace=true = wipe storage před plněním.
 *
 * Pravidla pro merge:
 *   - duplicate ID s identickým user_input + comment -> skip
 *   - duplicate ID s odlišným obsahem -> nový ID
 *   - ID v souboru <= s_next_id -> nový ID (= renumber pro jistotu)
 *   - po loadu se s_next_id posune nad maximum existujícího ID
 */
static bool bookmarks_load_internal ( const char *filepath, bool mode_replace ) {
    if ( !s_bookmarks ) bookmarks_init ( );
    bookmarks_mutex_ensure ( );

    char *resolved = bookmarks_resolve_path ( filepath );
    if ( !resolved ) {
        bookmarks_warn ( "cannot resolve .bookmarks filepath" );
        return false;
    };

    if ( !g_file_test ( resolved, G_FILE_TEST_EXISTS ) ) {
        if ( mode_replace ) {
            bookmarks_clear ( );
        };
        g_free ( resolved );
        return true;
    };

    JsonParser *parser = json_parser_new ( );
    GError *err = NULL;
    if ( !json_parser_load_from_file ( parser, resolved, &err ) ) {
        bookmarks_warn ( "parse error in '%s': %s",
                         resolved, err ? err->message : "unknown" );
        if ( err ) g_error_free ( err );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonNode *root = json_parser_get_root ( parser );
    if ( !root || json_node_get_node_type ( root ) != JSON_NODE_OBJECT ) {
        bookmarks_warn ( "root is not a JSON object in '%s'", resolved );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonObject *root_obj = json_node_get_object ( root );

    /* Storage mutation sekce - držíme s_bookmarks_mutex od clear až
     * po bump_version + s_next_id update, aby snapshot z jiného threadu
     * neviděl částečný stav. */
    g_mutex_lock ( &s_bookmarks_mutex );

    if ( mode_replace ) {
        bookmarks_clear_locked ( );
        s_next_id = 1;
    };

    if ( !json_object_has_member ( root_obj, "bookmarks" ) ) {
        g_mutex_unlock ( &s_bookmarks_mutex );
        g_object_unref ( parser );
        g_free ( resolved );
        return true;  /* tolerantně OK - prázdný stav */
    };

    JsonNode *vn = json_object_get_member ( root_obj, "bookmarks" );
    if ( !vn || json_node_get_node_type ( vn ) != JSON_NODE_ARRAY ) {
        g_mutex_unlock ( &s_bookmarks_mutex );
        bookmarks_warn ( "'bookmarks' is not an array in '%s'", resolved );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonArray *arr = json_node_get_array ( vn );
    guint n = json_array_get_length ( arr );
    uint32_t max_id_seen = 0;

    for ( guint i = 0; i < n; i++ ) {
        JsonNode *en = json_array_get_element ( arr, i );
        if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
        JsonObject *eo = json_node_get_object ( en );

        uint32_t  file_id   = 0;
        char     *file_in   = NULL;
        char     *file_cmnt = NULL;
        en_DBGAPI_CMD_ORIGIN file_origin = DBGAPI_CMD_ORIGIN_USER;
        if ( !bookmarks_parse_entry ( eo, &file_id, &file_in, &file_cmnt, &file_origin ) ) {
            continue;
        };

        if ( mode_replace ) {
            /* REPLACE mode: zachovat ID ze souboru. */
            bookmark_t entry;
            entry.id         = file_id ? file_id : s_next_id++;
            entry.user_input = file_in;
            entry.comment    = file_cmnt;
            /* V1.D.2.C - origin persistujeme, default USER při missing klíči. */
            entry.cmd_origin = file_origin;
            g_array_append_val ( s_bookmarks, entry );
            if ( entry.id > max_id_seen ) max_id_seen = entry.id;
        } else {
            /* MERGE mode: skip-duplicate identical, jinak nové ID. */
            bool skip = false;
            int existing_idx = bookmarks_find_index_by_id ( file_id );
            if ( existing_idx >= 0 ) {
                const bookmark_t *ex = &g_array_index ( s_bookmarks, bookmark_t, (guint) existing_idx );
                bool same_in   = ( ex->user_input && file_in && strcmp ( ex->user_input, file_in ) == 0 );
                bool same_cmnt = ( ( !ex->comment && !file_cmnt ) ||
                                   ( ex->comment && file_cmnt && strcmp ( ex->comment, file_cmnt ) == 0 ) );
                if ( same_in && same_cmnt ) skip = true;
            };
            if ( skip ) {
                g_free ( file_in );
                if ( file_cmnt ) g_free ( file_cmnt );
                continue;
            };

            bookmark_t entry;
            /* Pokud ID koliduje (existing + odlišný), nebo je <= s_next_id-1,
             * dáme nové ID. Jinak respektuj file_id. */
            if ( file_id == 0 || existing_idx >= 0 || file_id < s_next_id ) {
                entry.id = s_next_id++;
            } else {
                entry.id = file_id;
            };
            entry.user_input = file_in;
            entry.comment    = file_cmnt;
            /* V1.D.2.C - origin persistujeme i v merge módu. */
            entry.cmd_origin = file_origin;
            g_array_append_val ( s_bookmarks, entry );
            if ( entry.id > max_id_seen ) max_id_seen = entry.id;
        };
    };

    /* Posuneme s_next_id nad maximum (= nikdy nerecyklujeme ID). */
    if ( max_id_seen >= s_next_id ) {
        s_next_id = max_id_seen + 1;
    };

    bookmarks_bump_version ( );
    g_mutex_unlock ( &s_bookmarks_mutex );
    g_object_unref ( parser );
    g_free ( resolved );
    return true;
}


bool bookmarks_load ( const char *filepath ) {
    return bookmarks_load_internal ( filepath, true );
}


bool bookmarks_merge ( const char *filepath ) {
    return bookmarks_load_internal ( filepath, false );
}


/* ========================================================================= */
/*  Cfg sekce [BOOKMARKS]                                                    */
/* ========================================================================= */


/**
 * @brief Idempotence flag - cfg modul lze registrovat jen jednou per
 *        process lifetime (cfgmain by re-registraci odmítl).
 */
static bool s_cfg_registered = false;


void bookmarks_cfg_init ( void ) {
    if ( s_cfg_registered ) return;
    s_cfg_registered = true;

    /* Default per-arch - runtime z g_mzhal (mzhal krok 7). */
    static char default_bookmarks_file[32];
    snprintf ( default_bookmarks_file, sizeof ( default_bookmarks_file ),
               "%s.bookmarks", g_mzhal.arch_name );

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "BOOKMARKS" );
    if ( !cmod ) {
        /* Neselháváme, jen běh bez cfg perzistence. */
        return;
    };

    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "bookmarks_file", CFGENTYPE_TEXT, default_bookmarks_file );
    cfgelement_bind ( elm, (void*) &s_bm_cfg.bookmarks_file );

    elm = cfgmodule_register_new_element ( cmod, "bookmarks_auto_save", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_bm_cfg.auto_save );

    elm = cfgmodule_register_new_element ( cmod, "bookmarks_auto_load", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_bm_cfg.auto_load );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );


    /* Auto-load po propagate (= cesta v s_bm_cfg.bookmarks_file je
     * platná). REPLACE mode = soubor je SSOT pro start. */
    if ( s_bm_cfg.auto_load ) {
        if ( !bookmarks_load_internal ( NULL, true ) ) {
            bookmarks_warn ( "auto-load failed" );
        };
    };
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
