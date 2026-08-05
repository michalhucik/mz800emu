/*
 * File:   sym_db.c
 *
 * Implementace symbol DB (D.8). Storage = GArray záznamů st_SYMBOL,
 * lookup linear scan.
 *
 * Vlastnictví: name/comment/module = heap (g_strdup), uvolňováno
 * v clear/destroy/remove. Source priority enum hodnota = vyšší
 * číslo = vyšší priorita.
 *
 * Auto-detekce formátu: dle suffixu (case-insensitive). Parser
 * sám per-format soubor (sym_import_*.c).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"
#include "mzarch/mzhal.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

#include "sym_db.h"
#include "cfgmain.h"
#include "main.h"   /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"


/**
 * @brief Privátní storage. NULL = neinicializovaný stav (po destroy
 *        nebo před init).
 */
static GArray *s_syms = NULL;


/**
 * @brief Monotonní čítač změn DB pro cache invalidaci v UI vrstvě.
 *
 * Inkrementuje se interním helperem sym_db_bump_version() při každé
 * úspěšné modifikaci obsahu (clear, load, add, remove, set_comment).
 * Init/destroy ho nemění - prázdná DB má verzi 0.
 *
 * Wrap-around po 2^32 změnách teoreticky možný, prakticky bezvýznamný
 * (debugger session nikdy nedosáhne 4 mld změn).
 */
static unsigned s_version = 0;


/**
 * @brief Inkrementuje verzi DB. Volá se z modifikujících operací.
 */
static inline void sym_db_bump_version ( void ) {
    s_version++;
}


/**
 * @brief Najde index záznamu podle jména (case-sensitive).
 * @return index >= 0 nebo -1
 */
static int sym_db_find_index_by_name ( const char *name ) {
    if ( !s_syms || !name ) return -1;
    guint i;
    for ( i = 0; i < s_syms->len; i++ ) {
        const st_SYMBOL *s = &g_array_index ( s_syms, st_SYMBOL, i );
        if ( s->name && strcmp ( s->name, name ) == 0 ) {
            return (int) i;
        };
    };
    return -1;
}


/**
 * @brief Uvolní heap pole jednoho záznamu (in-place).
 */
static void sym_db_free_record_inplace ( st_SYMBOL *s ) {
    if ( !s ) return;
    g_free ( s->name );
    g_free ( s->comment );
    g_free ( s->module );
    s->name = NULL;
    s->comment = NULL;
    s->module = NULL;
}


void sym_db_init ( void ) {
    if ( s_syms ) return;
    s_syms = g_array_new ( FALSE, TRUE, sizeof ( st_SYMBOL ) );
}


void sym_db_destroy ( void ) {
    if ( !s_syms ) return;
    sym_db_clear ( );
    g_array_free ( s_syms, TRUE );
    s_syms = NULL;
}


void sym_db_clear ( void ) {
    if ( !s_syms ) return;
    guint i;
    for ( i = 0; i < s_syms->len; i++ ) {
        st_SYMBOL *s = &g_array_index ( s_syms, st_SYMBOL, i );
        sym_db_free_record_inplace ( s );
    };
    g_array_set_size ( s_syms, 0 );
    sym_db_bump_version ( );
}


/**
 * @brief Interní helper - přidá nebo aktualizuje záznam.
 *
 * Pravidla:
 *   - Pokud jméno neexistuje: insert.
 *   - Pokud jméno existuje: porovná source priority. Nový >= existující
 *     -> přepíše (= novější LBL přepíše starší NOI). Nižší priorita ->
 *     ponechá existující (= NOI nepřepíše LBL).
 *
 * @return 1 pokud přidán/přepsán, 0 pokud zachován starý (priority nižší)
 */
static int sym_db_upsert ( const char *name, uint32_t addr, uint8_t bank_id,
                           en_SYM_SOURCE source, const char *comment,
                           const char *module ) {
    if ( !s_syms || !name || !*name ) return 0;
    int idx = sym_db_find_index_by_name ( name );
    if ( idx < 0 ) {
        /* nový záznam */
        st_SYMBOL rec;
        memset ( &rec, 0, sizeof ( rec ) );
        rec.name    = g_strdup ( name );
        rec.addr    = addr;
        rec.bank_id = bank_id;
        rec.source  = source;
        rec.comment = ( comment && *comment ) ? g_strdup ( comment ) : NULL;
        rec.module  = ( module  && *module  ) ? g_strdup ( module  ) : NULL;
        g_array_append_val ( s_syms, rec );
        return 1;
    };
    /* existující záznam - rozhodnutí dle priority */
    st_SYMBOL *rec = &g_array_index ( s_syms, st_SYMBOL, idx );
    if ( (int) source < (int) rec->source ) {
        /* novější má nižší prioritu, ponecháme existující */
        return 0;
    };
    /* novější je >= existující priority, přepíšeme */
    rec->addr    = addr;
    rec->bank_id = bank_id;
    rec->source  = source;
    g_free ( rec->comment );
    rec->comment = ( comment && *comment ) ? g_strdup ( comment ) : NULL;
    g_free ( rec->module );
    rec->module  = ( module  && *module  ) ? g_strdup ( module  ) : NULL;
    return 1;
}


/**
 * @brief Veřejný "internal API" pro parsery - jen wrapper nad upsert.
 *
 * Není v hlavičce (= jen pro parsery v same translation unit / přes
 * extern declaration v sym_import_*.c).
 */
int sym_db_add_internal ( const char *name, uint32_t addr, uint8_t bank_id,
                          en_SYM_SOURCE source, const char *comment,
                          const char *module ) {
    int changed = sym_db_upsert ( name, addr, bank_id, source, comment, module );
    if ( changed ) sym_db_bump_version ( );
    return changed;
}


const st_SYMBOL *sym_db_lookup_by_name ( const char *name ) {
    int idx = sym_db_find_index_by_name ( name );
    if ( idx < 0 ) return NULL;
    return &g_array_index ( s_syms, st_SYMBOL, idx );
}


const st_SYMBOL *sym_db_lookup_by_addr ( uint32_t addr, uint8_t bank_id ) {
    if ( !s_syms ) return NULL;
    const st_SYMBOL *best = NULL;
    const st_SYMBOL *fallback = NULL;
    guint i;
    for ( i = 0; i < s_syms->len; i++ ) {
        const st_SYMBOL *s = &g_array_index ( s_syms, st_SYMBOL, i );
        if ( s->addr != addr ) continue;
        if ( s->bank_id == bank_id ) {
            if ( !best || (int) s->source > (int) best->source ) {
                best = s;
            };
        }
        else if ( s->bank_id == 0 && bank_id != 0 ) {
            /* fallback: globální (CPU view) symbol pokud chybí explicit bank match */
            if ( !fallback || (int) s->source > (int) fallback->source ) {
                fallback = s;
            };
        };
    };
    return best ? best : fallback;
}


size_t sym_db_count ( void ) {
    if ( !s_syms ) return 0;
    return s_syms->len;
}


const st_SYMBOL *sym_db_get_by_index ( size_t idx ) {
    if ( !s_syms || idx >= s_syms->len ) return NULL;
    return &g_array_index ( s_syms, st_SYMBOL, idx );
}


void sym_db_iter_init ( st_SYM_DB_ITER *iter ) {
    if ( !iter ) return;
    iter->next_idx = 0;
}


const st_SYMBOL *sym_db_iter_next ( st_SYM_DB_ITER *iter ) {
    if ( !iter || !s_syms ) return NULL;
    if ( iter->next_idx >= s_syms->len ) return NULL;
    const st_SYMBOL *s = &g_array_index ( s_syms, st_SYMBOL, iter->next_idx );
    iter->next_idx++;
    return s;
}


int sym_db_set_comment ( const char *name, const char *new_comment ) {
    int idx = sym_db_find_index_by_name ( name );
    if ( idx < 0 ) return -1;
    st_SYMBOL *rec = &g_array_index ( s_syms, st_SYMBOL, idx );
    g_free ( rec->comment );
    rec->comment = ( new_comment && *new_comment ) ? g_strdup ( new_comment ) : NULL;
    /* edit komentáře = promote na LBL (write-back) */
    rec->source = SYM_SOURCE_LBL;
    sym_db_bump_version ( );
    return 0;
}


int sym_db_add_user_label ( uint32_t addr, const char *name,
                            const char *comment ) {
    if ( !name || !*name ) return -1;
    /* přepíše existující bez ohledu na priority (= user explicit add) */
    int idx = sym_db_find_index_by_name ( name );
    if ( idx >= 0 ) {
        st_SYMBOL *rec = &g_array_index ( s_syms, st_SYMBOL, idx );
        rec->addr    = addr;
        rec->bank_id = 0;
        rec->source  = SYM_SOURCE_LBL;
        g_free ( rec->comment );
        rec->comment = ( comment && *comment ) ? g_strdup ( comment ) : NULL;
        g_free ( rec->module );
        rec->module = NULL;
        sym_db_bump_version ( );
        return 0;
    };
    int ok = sym_db_upsert ( name, addr, 0, SYM_SOURCE_LBL, comment, NULL );
    if ( ok ) sym_db_bump_version ( );
    return ok ? 0 : -1;
}


int sym_db_remove_user_label ( const char *name ) {
    int idx = sym_db_find_index_by_name ( name );
    if ( idx < 0 ) return -1;
    st_SYMBOL *rec = &g_array_index ( s_syms, st_SYMBOL, idx );
    sym_db_free_record_inplace ( rec );
    g_array_remove_index ( s_syms, idx );
    sym_db_bump_version ( );
    return 0;
}


int sym_db_set_cmd_origin ( const char *name, en_DBGAPI_CMD_ORIGIN origin ) {
    if ( !name || !*name ) return -1;
    int idx = sym_db_find_index_by_name ( name );
    if ( idx < 0 ) return -1;
    st_SYMBOL *rec = &g_array_index ( s_syms, st_SYMBOL, idx );
    rec->cmd_origin = origin;
    /* sym_db_bump_version() záměrně NEvoláme - origin je metadata bez
     * dopadu na lookup ani UI cache, žádná invalidace není nutná. */
    return 0;
}


unsigned sym_db_get_version ( void ) {
    return s_version;
}


/**
 * @brief Auto-detekce formátu dle suffixu (case-insensitive).
 *
 * Suffix se hledá od posledního '.'. Pokud chybí, vrací -1.
 */
int sym_db_load_auto ( const char *path ) {
    if ( !path ) return -1;
    const char *dot = strrchr ( path, '.' );
    if ( !dot ) return -1;
    /* case-insensitive porovnání */
    char ext [ 8 ];
    size_t i = 0;
    for ( const char *p = dot + 1; *p && i < sizeof ( ext ) - 1; p++, i++ ) {
        ext [ i ] = (char) tolower ( (unsigned char) *p );
    };
    ext [ i ] = '\0';
    if      ( strcmp ( ext, "noi" ) == 0 ) return sym_db_load_noi ( path );
    else if ( strcmp ( ext, "map" ) == 0 ) return sym_db_load_map ( path );
    else if ( strcmp ( ext, "sym" ) == 0 ) return sym_db_load_sym ( path );
    else if ( strcmp ( ext, "lbl" ) == 0 ) return sym_db_load_lbl ( path );
    return -1;
}


/* ===================================================================== */
/*  Default .lbl persistence (V1.7+ 3.2, debugger-fixes-5)                */
/* ===================================================================== */

/**
 * @brief Cfg state pro auto-persist default .lbl souboru.
 *
 * default_file: cfg-owned string (bind přes cfgelement_bind), nikdy
 *               manually free - lifetime = do exit cfg modulu.
 * auto_load/auto_save: bool jako unsigned (CFGENTYPE_BOOL handler).
 * registered: idempotency guard pro sym_db_cfg_init.
 */
static struct {
    char    *default_file;
    unsigned auto_load;
    unsigned auto_save;
    int      registered;
} s_sym_cfg = { NULL, 1, 1, 0 };


/**
 * @brief Per-arch default .lbl filename ("mz800.lbl", ...). Per-arch
 *        separace umožňuje přepínání mezi mz800emu/mz1500emu/mz700emu
 *        se samostatnou LBL sadou v jednom config dir. Od mzhal kroku 7
 *        runtime z g_mzhal.arch_name (hodnoty beze změny).
 *
 * @return Jméno souboru; statický buffer platný po celou dobu běhu,
 *         volající ho NEuvolňuje.
 */
static const char *sym_db_default_lbl_file ( void ) {
    static char fname[32];
    if ( fname[0] == 0x00 ) {
        snprintf ( fname, sizeof ( fname ), "%s.lbl", g_mzhal.arch_name );
    };
    return fname;
}


void sym_db_cfg_init ( void ) {
    if ( s_sym_cfg.registered ) return;
    s_sym_cfg.registered = 1;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "SYMBOLS" );
    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "lbl_file", CFGENTYPE_TEXT,
                                            sym_db_default_lbl_file ( ) );
    cfgelement_bind ( elm, (void*) &s_sym_cfg.default_file );

    elm = cfgmodule_register_new_element ( cmod, "auto_load", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_sym_cfg.auto_load );

    elm = cfgmodule_register_new_element ( cmod, "auto_save", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void*) &s_sym_cfg.auto_save );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

    /* Auto-load default .lbl pokud uživatel nezakázal. Mlčky tolerantní
     * k chybějícímu souboru - typické při prvním spuštění. */
    if ( s_sym_cfg.auto_load ) {
        (void) sym_db_load_default_lbl ( );
    };
}


void sym_db_cfg_auto_save ( void ) {
    if ( !s_sym_cfg.registered ) return;
    if ( !s_sym_cfg.auto_save ) return;
    (void) sym_db_save_default_lbl ( );
}


const char *sym_db_get_default_lbl_file ( void ) {
    return s_sym_cfg.default_file;
}


void sym_db_set_default_lbl_file ( const char *path ) {
    if ( !path || !*path ) return;
    /* default_file je cfg-bind (vlastněn cfgelement) - g_free + g_strdup
     * je legitní (cfgelement_bind ukládá pointer adresu, nepouští).
     * Toto je stejný pattern jako v breakpoints.c po user "Default BPT
     * file..." dialogu. */
    if ( s_sym_cfg.default_file ) g_free ( s_sym_cfg.default_file );
    s_sym_cfg.default_file = g_strdup ( path );
}


int sym_db_get_auto_load_lbl ( void ) {
    return s_sym_cfg.auto_load ? 1 : 0;
}


void sym_db_set_auto_load_lbl ( int enable ) {
    s_sym_cfg.auto_load = enable ? 1u : 0u;
}


int sym_db_get_auto_save_lbl ( void ) {
    return s_sym_cfg.auto_save ? 1 : 0;
}


void sym_db_set_auto_save_lbl ( int enable ) {
    s_sym_cfg.auto_save = enable ? 1u : 0u;
}


int sym_db_load_default_lbl ( void ) {
    if ( !s_sym_cfg.default_file || !s_sym_cfg.default_file[ 0 ] ) return -1;
    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths,
                                                 s_sym_cfg.default_file );
    int n = sym_db_load_lbl ( resolved );
    g_free ( resolved );
    return n;
}


int sym_db_save_default_lbl ( void ) {
    if ( !s_sym_cfg.default_file || !s_sym_cfg.default_file[ 0 ] ) return -1;
    char *resolved = sdlapp_paths_resolve_cfg ( g_sdlapp->paths,
                                                 s_sym_cfg.default_file );
    int n = sym_db_save_lbl ( resolved );
    g_free ( resolved );
    return n;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
