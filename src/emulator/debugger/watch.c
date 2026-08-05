/*
 * File:   watch.c
 *
 * Implementace Watch panelu (Phase A = L0 Address Watch MVP + Phase B = L1 typy).
 *
 * Strategie storage: dynamic GArray `st_WATCH_ROW`. Watch řádky drží
 * pořadí přidání (= index významný pro UI - drag-reorder posunuje
 * indexy). Lookup-by-index, ne hash; typický počet < 50 řádků.
 *
 * Ownership: `name` a `expr_text` v každém záznamu jsou heap (g_strdup),
 * uvolňováno při remove / clear_storage / shutdown.
 *
 * Persistence pattern: identický jako `bp_vars` (per-arch JSON, cfg sekce
 * `[WATCH_PANEL]` s auto_load / auto_save). Phase B rozšíření schema
 * (fmt/length/bit_index) je backward compat - chybějící fieldy = defaulty.
 *
 * mzascii rendering: používá `sharpmz_to_utf8(byte, SHARPMZ_CHARSET_EU)`
 * z `libs/sharpmz_ascii/sharpmz_utf8.h`. Tabulka NENI per-arch (jeden
 * globální EU/JP volič; pro debugger watch jsme zvolili EU default,
 * harvest poznámka v `emu-experiments/knowledge/watch/types.md`).
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
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "watch.h"
#include "cfgmain.h"
#include "main.h"   /* g_sdlapp */
#include "libs/sdlapp/sdlapp_paths.h"
#include "libs/sharpmz_ascii/sharpmz_utf8.h"
#include "hw-generic/memory/memory.h"
#include "debugger/symbols/sym_db.h"
#include "debugger/bp_expr.h"
#include "debugger/breakpoints.h"  /* breakpoints_fill_global_ctx */


/**
 * @brief Privátní storage. NULL = neinicializovaný stav (po shutdown
 *        nebo před init).
 */
static GArray *s_rows = NULL;


/**
 * @brief Monotonní counter pro `st_WATCH_ROW.id`. Začíná na 1, resetuje
 *        se na 1 při `watch_init` (= po shutdown stav, nový run).
 *        Inkrementuje se v `watch_add` a `watch_add_expr`.
 */
static int s_next_id = 1;


/**
 * @brief Cfg-bound stav modulu WATCH_PANEL.
 *
 * Hodnoty plní `cfgmodule_propagate()` v `watch_cfg_init()`. Stringy
 * vlastní cfg knihovna (= nesmí se g_free).
 */
static struct {
    char     *watch_file;        /**< Per-arch default `.watch` filename */
    bool      auto_save;         /**< Auto-save při exit */
    bool      auto_load;         /**< Auto-load při start */
    bool      highlight_changes; /**< Phase E.1: color fade on value change */
    uint32_t  highlight_fade_ms; /**< Phase E.1: fade duration v ms (1..10000) */
} s_watch_cfg = { NULL, true, true, true, 500 };


/* ========================================================================= */
/*  Helpers                                                                  */
/* ========================================================================= */


/**
 * @brief Uvolní heap stringy + AST uvnitř záznamu.
 *        Nemaže záznam z arraye.
 *
 * Volá se z `watch_remove` / `watch_clear_storage` / před `watch_set_expr`
 * (= reuse pro reparse). Po návratu jsou pointery NULL.
 */
static void watch_row_free_strings ( st_WATCH_ROW *r ) {
    if ( !r ) return;
    if ( r->name ) {
        g_free ( r->name );
        r->name = NULL;
    };
    if ( r->expr_text ) {
        g_free ( r->expr_text );
        r->expr_text = NULL;
    };
    if ( r->expr_ast ) {
        bp_expr_free ( r->expr_ast );
        r->expr_ast = NULL;
    };
    if ( r->expr_error ) {
        g_free ( r->expr_error );
        r->expr_error = NULL;
    };
}


/**
 * @brief Uvolní jen expr-související fieldy (text, ast, error).
 *
 * Nepoužívá `watch_row_free_strings` aby `name` zůstal nedotčený.
 */
static void watch_row_free_expr_only ( st_WATCH_ROW *r ) {
    if ( !r ) return;
    if ( r->expr_text ) {
        g_free ( r->expr_text );
        r->expr_text = NULL;
    };
    if ( r->expr_ast ) {
        bp_expr_free ( r->expr_ast );
        r->expr_ast = NULL;
    };
    if ( r->expr_error ) {
        g_free ( r->expr_error );
        r->expr_error = NULL;
    };
}


/**
 * @brief Truncate-copy řetězce na heap, max `WATCH_NAME_MAX` znaků.
 *
 * @return Heap-allocated kopie (caller free), nebo NULL pokud
 *         input je NULL nebo prázdný.
 */
static char *watch_dup_name ( const char *s ) {
    if ( !s || !s[0] ) return NULL;
    size_t len = strlen ( s );
    if ( len > WATCH_NAME_MAX ) len = WATCH_NAME_MAX;
    char *out = (char *) g_malloc ( len + 1 );
    memcpy ( out, s, len );
    out[ len ] = '\0';
    return out;
}


/* ========================================================================= */
/*  Lifecycle                                                                */
/* ========================================================================= */


void watch_init ( void ) {
    if ( s_rows ) return;  /* idempotent */
    s_rows = g_array_new ( FALSE, TRUE, sizeof ( st_WATCH_ROW ) );
    s_next_id = 1;
}


void watch_shutdown ( void ) {
    if ( !s_rows ) return;
    watch_clear_storage ( );
    g_array_free ( s_rows, TRUE );
    s_rows = NULL;
}


void watch_clear_storage ( void ) {
    if ( !s_rows ) return;
    guint i;
    for ( i = 0; i < s_rows->len; i++ ) {
        st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, i );
        watch_row_free_strings ( r );
    };
    g_array_set_size ( s_rows, 0 );
}


/* ========================================================================= */
/*  CRUD                                                                     */
/* ========================================================================= */


int watch_add ( const char *name, uint16_t addr, int bank ) {
    if ( !s_rows ) watch_init ( );
    if ( !s_rows ) return -1;  /* alokace selhala */

    st_WATCH_ROW row;
    row.id         = s_next_id++;
    row.name       = watch_dup_name ( name );
    row.addr       = addr;
    row.bank       = bank;
    row.mode       = WATCH_MODE_ADDRESS;
    row.type       = WATCH_TYPE_U8;
    row.fmt        = WATCH_FMT_HEX;   /* default pro unsigned */
    row.length     = 0;                /* nepoužije se pro u8 */
    row.bit_index  = 0;
    row.expr_text  = NULL;
    row.expr_ast   = NULL;
    row.expr_error = NULL;
    row.cmd_origin = DBGAPI_CMD_ORIGIN_USER;  /* V1.C.3 default - GUI tvorba */

    g_array_append_val ( s_rows, row );
    return (int) ( s_rows->len - 1 );
}


/**
 * @brief Pomocné - parsuje text výrazu a uloží AST + případnou chybu
 *        do dané řádky.
 *
 * Caller je zodpovědný za to že `r->expr_text` je již alokovaný (= tato
 * funkce ho ANI nemění ANI nealokuje). Předchozí `expr_ast` a `expr_error`
 * se uvolní.
 *
 * @param r  Řádek (non-NULL, expr_text non-NULL).
 */
static void watch_reparse_expr ( st_WATCH_ROW *r ) {
    if ( !r || !r->expr_text ) return;
    if ( r->expr_ast ) {
        bp_expr_free ( r->expr_ast );
        r->expr_ast = NULL;
    };
    if ( r->expr_error ) {
        g_free ( r->expr_error );
        r->expr_error = NULL;
    };
    char errbuf[ 256 ];
    errbuf[0] = '\0';
    r->expr_ast = bp_expr_parse ( r->expr_text, errbuf, sizeof ( errbuf ) );
    if ( !r->expr_ast && errbuf[0] ) {
        r->expr_error = g_strdup ( errbuf );
    };
}


int watch_add_expr ( const char *name, const char *expr_text,
                      en_WATCH_MODE mode, en_WATCH_TYPE type ) {
    if ( !s_rows ) watch_init ( );
    if ( !s_rows ) return -1;
    if ( !expr_text || !expr_text[0] ) return -1;

    /* Defenzivní: pokud caller předá ADDRESS, downgrade a zalogovat. */
    if ( mode == WATCH_MODE_ADDRESS ) {
        fprintf ( stderr, "WATCH WARNING: watch_add_expr called with "
                          "WATCH_MODE_ADDRESS, using watch_add() semantics\n" );
        int idx = watch_add ( name, 0, -1 );
        if ( idx >= 0 ) watch_set_type ( idx, type );
        return idx;
    };

    st_WATCH_ROW row;
    row.id         = s_next_id++;
    row.name       = watch_dup_name ( name );
    row.addr       = 0;
    row.bank       = -1;
    row.mode       = mode;
    row.type       = type;
    row.fmt        = watch_type_default_fmt ( type );
    row.length     = watch_type_has_length ( type ) ? WATCH_LENGTH_MIN : 0;
    row.bit_index  = 0;
    row.expr_text  = g_strdup ( expr_text );
    row.expr_ast   = NULL;
    row.expr_error = NULL;
    row.cmd_origin = DBGAPI_CMD_ORIGIN_USER;  /* V1.C.3 default - GUI tvorba */
    watch_reparse_expr ( &row );

    g_array_append_val ( s_rows, row );
    return (int) ( s_rows->len - 1 );
}


bool watch_set_expr ( int row_index, const char *expr_text,
                       en_WATCH_MODE mode ) {
    if ( !s_rows ) return false;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return false;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );

    if ( mode == WATCH_MODE_ADDRESS ) {
        /* Přepnutí zpět na address mód = vyčistit expr fieldy. */
        watch_row_free_expr_only ( r );
        r->mode = WATCH_MODE_ADDRESS;
        return true;
    };

    if ( !expr_text || !expr_text[0] ) return false;

    /* Reuse expr fieldy - uvolnit staré, alokovat nové, reparse. */
    watch_row_free_expr_only ( r );
    r->mode      = mode;
    r->expr_text = g_strdup ( expr_text );
    watch_reparse_expr ( r );
    return true;
}


const char *watch_get_expr_error ( const st_WATCH_ROW *row ) {
    if ( !row ) return NULL;
    if ( row->mode == WATCH_MODE_ADDRESS ) return NULL;
    return row->expr_error;
}


/**
 * @brief Helper - vyhodnotí AST řádky se standardním Watch ctx.
 *
 * Sestaví ctx přes `bp_expr_ctx_zero` + `breakpoints_fill_global_ctx`
 * (= cpu + Cycle/Frame/Scanline + BankPC). Pole závislá na BP hit typu
 * (Address/Value/IsRead/...) zůstanou na nule - Watch je čte mimo
 * kontext BP hooku.
 *
 * Side effects: vnitřně `bp_read_byte_no_se` cesta (side_effect=false).
 *
 * @return Hodnota výrazu, nebo 0 pokud AST není parsed.
 */
static int32_t watch_eval_internal ( const st_WATCH_ROW *row ) {
    if ( !row ) return 0;
    if ( row->mode == WATCH_MODE_ADDRESS ) return 0;
    if ( !row->expr_ast ) return 0;

    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    breakpoints_fill_global_ctx ( &ctx );
    /* self_id = -1 (= ne BP enforcement kontext, viz bp_expr_ctx_t.self_id). */
    ctx.self_id = -1;
    return bp_expr_eval ( row->expr_ast, &ctx );
}


int32_t watch_eval_expr ( const st_WATCH_ROW *row ) {
    return watch_eval_internal ( row );
}


uint16_t watch_eval_expr_as_addr ( const st_WATCH_ROW *row ) {
    int32_t v = watch_eval_internal ( row );
    return (uint16_t) ( v & 0xFFFF );
}


void watch_remove ( int row_index ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;

    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    watch_row_free_strings ( r );
    g_array_remove_index ( s_rows, (guint) row_index );
}


void watch_rename ( int row_index, const char *new_name ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;

    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    if ( r->name ) {
        g_free ( r->name );
        r->name = NULL;
    };
    r->name = watch_dup_name ( new_name );  /* NULL safe */
}


void watch_move ( int row_index, int new_index ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    if ( new_index < 0 || (guint) new_index >= s_rows->len ) return;
    if ( row_index == new_index ) return;

    /* Bezpečný posun přes copy / remove / insert. Záznam je POD - kopie
     * pointerů je OK, ownership přechází na nový index. */
    st_WATCH_ROW tmp = g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    g_array_remove_index ( s_rows, (guint) row_index );
    g_array_insert_val ( s_rows, (guint) new_index, tmp );
}


void watch_set_type ( int row_index, en_WATCH_TYPE type ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );

    r->type = type;
    r->fmt  = watch_type_default_fmt ( type );

    if ( watch_type_has_length ( type ) ) {
        if ( r->length == 0 ) r->length = WATCH_LENGTH_MIN;
        uint16_t maxl = watch_type_max_length ( type );
        if ( r->length > maxl ) r->length = maxl;
    } else {
        r->length = 0;
    };

    if ( type != WATCH_TYPE_BIT ) {
        r->bit_index = 0;
    };
}


void watch_set_fmt ( int row_index, en_WATCH_FMT fmt ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    if ( !watch_type_has_format ( r->type ) ) return;
    r->fmt = fmt;
}


void watch_set_length ( int row_index, uint16_t length ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    if ( !watch_type_has_length ( r->type ) ) return;

    uint16_t maxl = watch_type_max_length ( r->type );
    if ( length < WATCH_LENGTH_MIN ) length = WATCH_LENGTH_MIN;
    if ( length > maxl ) length = maxl;
    r->length = length;
}


void watch_set_bit_index ( int row_index, uint8_t bit_index ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    if ( r->type != WATCH_TYPE_BIT ) return;
    if ( bit_index > 7 ) bit_index = 7;
    r->bit_index = bit_index;
}


void watch_set_cmd_origin ( int row_index, en_DBGAPI_CMD_ORIGIN origin ) {
    if ( !s_rows ) return;
    if ( row_index < 0 || (guint) row_index >= s_rows->len ) return;
    st_WATCH_ROW *r = &g_array_index ( s_rows, st_WATCH_ROW, (guint) row_index );
    r->cmd_origin = origin;
}


/* ========================================================================= */
/*  Enumerace                                                                */
/* ========================================================================= */


size_t watch_count ( void ) {
    if ( !s_rows ) return 0;
    return (size_t) s_rows->len;
}


const st_WATCH_ROW *watch_get ( size_t index ) {
    if ( !s_rows ) return NULL;
    if ( index >= (size_t) s_rows->len ) return NULL;
    return &g_array_index ( s_rows, st_WATCH_ROW, (guint) index );
}


/* ========================================================================= */
/*  Type metadata                                                            */
/* ========================================================================= */


size_t watch_type_size ( en_WATCH_TYPE t, uint16_t length ) {
    switch ( t ) {
        case WATCH_TYPE_U8:
        case WATCH_TYPE_I8:
        case WATCH_TYPE_BIT:
            return 1;
        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_U16BE:
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE:
            return 2;
        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_U32BE:
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE:
            return 4;
        case WATCH_TYPE_ASCII:
        case WATCH_TYPE_MZASCII:
        case WATCH_TYPE_BYTES:
            return (size_t) length;
    };
    return 0;
}


bool watch_type_has_length ( en_WATCH_TYPE t ) {
    return ( t == WATCH_TYPE_ASCII || t == WATCH_TYPE_MZASCII ||
             t == WATCH_TYPE_BYTES );
}


bool watch_type_has_format ( en_WATCH_TYPE t ) {
    switch ( t ) {
        case WATCH_TYPE_U8:
        case WATCH_TYPE_I8:
        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_U16BE:
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE:
        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_U32BE:
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE:
            return true;
        default:
            return false;
    };
}


en_WATCH_FMT watch_type_default_fmt ( en_WATCH_TYPE t ) {
    switch ( t ) {
        case WATCH_TYPE_I8:
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE:
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE:
            return WATCH_FMT_DEC;
        case WATCH_TYPE_U8:
        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_U16BE:
        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_U32BE:
            return WATCH_FMT_HEX;
        default:
            return WATCH_FMT_HEX;  /* placeholder pro typy bez fmt */
    };
}


uint16_t watch_type_max_length ( en_WATCH_TYPE t ) {
    switch ( t ) {
        case WATCH_TYPE_ASCII:
        case WATCH_TYPE_MZASCII:
            return WATCH_LENGTH_MAX_STRING;
        case WATCH_TYPE_BYTES:
            return WATCH_LENGTH_MAX_BYTES;
        default:
            return 0;
    };
}


/* ========================================================================= */
/*  Polling read                                                             */
/* ========================================================================= */


uint8_t watch_read_u8 ( const st_WATCH_ROW *row ) {
    if ( !row ) return 0;
    return memory_read_byte_no_se ( row->addr );
}


/**
 * @brief Pomocné - 16-bit wrap-around čtení n bajtů od adresy.
 */
static void watch_read_n ( uint16_t addr, size_t n, uint8_t *out ) {
    for ( size_t i = 0; i < n; i++ ) {
        out[ i ] = memory_read_byte_no_se ( (uint16_t) ( addr + i ) );
    };
}


uint64_t watch_read_int ( const st_WATCH_ROW *row ) {
    if ( !row ) return 0;

    /* EXPR_SCALAR: hodnota = výsledek eval, type určuje jen šířku display.
     * Pro non-int typy vrátíme 0 (= caller by neměl pro string/bytes
     * v scalar módu volat). */
    if ( row->mode == WATCH_MODE_EXPR_SCALAR ) {
        if ( !watch_type_has_format ( row->type ) &&
              row->type != WATCH_TYPE_BIT ) {
            return 0;
        };
        int32_t v = watch_eval_internal ( row );
        if ( row->type == WATCH_TYPE_BIT ) {
            return (uint64_t) ( ( ( (uint32_t) v ) >> row->bit_index ) & 1u );
        };
        return (uint64_t) (uint32_t) v;
    };

    /* EXPR_DEREF: nejdřív eval adresu, pak čti analogicky ADDRESS. */
    uint16_t eff_addr;
    if ( row->mode == WATCH_MODE_EXPR_DEREF ) {
        eff_addr = watch_eval_expr_as_addr ( row );
    } else {
        eff_addr = row->addr;
    };

    uint8_t buf[4] = { 0, 0, 0, 0 };
    switch ( row->type ) {

        case WATCH_TYPE_U8:
        case WATCH_TYPE_I8:
            return (uint64_t) memory_read_byte_no_se ( eff_addr );

        case WATCH_TYPE_BIT: {
            uint8_t b = memory_read_byte_no_se ( eff_addr );
            return (uint64_t) ( ( b >> row->bit_index ) & 1u );
        }

        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_I16LE:
            watch_read_n ( eff_addr, 2, buf );
            return (uint64_t) ( (uint16_t) buf[0] | ( (uint16_t) buf[1] << 8 ) );

        case WATCH_TYPE_U16BE:
        case WATCH_TYPE_I16BE:
            watch_read_n ( eff_addr, 2, buf );
            return (uint64_t) ( ( (uint16_t) buf[0] << 8 ) | (uint16_t) buf[1] );

        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_I32LE:
            watch_read_n ( eff_addr, 4, buf );
            return (uint64_t) ( (uint32_t) buf[0]
                                | ( (uint32_t) buf[1] << 8 )
                                | ( (uint32_t) buf[2] << 16 )
                                | ( (uint32_t) buf[3] << 24 ) );

        case WATCH_TYPE_U32BE:
        case WATCH_TYPE_I32BE:
            watch_read_n ( eff_addr, 4, buf );
            return (uint64_t) ( ( (uint32_t) buf[0] << 24 )
                                | ( (uint32_t) buf[1] << 16 )
                                | ( (uint32_t) buf[2] << 8 )
                                | (uint32_t) buf[3] );

        case WATCH_TYPE_ASCII:
        case WATCH_TYPE_MZASCII:
        case WATCH_TYPE_BYTES:
        default:
            return 0;
    };
}


void watch_read_bytes ( const st_WATCH_ROW *row,
                         uint8_t *out_buf, size_t buf_size,
                         size_t *out_len ) {
    if ( out_len ) *out_len = 0;
    if ( !row || !out_buf || buf_size == 0 ) return;
    if ( !watch_type_has_length ( row->type ) ) return;

    /* EXPR_SCALAR pro string typy nemá smysl - vrátíme 0 bajtů. */
    if ( row->mode == WATCH_MODE_EXPR_SCALAR ) return;

    uint16_t eff_addr = ( row->mode == WATCH_MODE_EXPR_DEREF )
                        ? watch_eval_expr_as_addr ( row )
                        : row->addr;

    size_t want = watch_type_size ( row->type, row->length );
    if ( want > buf_size ) want = buf_size;
    watch_read_n ( eff_addr, want, out_buf );
    if ( out_len ) *out_len = want;
}


/* ========================================================================= */
/*  Formátování                                                              */
/* ========================================================================= */


/**
 * @brief Pomocné - bin format s prefixem `0b`, šířka přesně N bitů.
 *
 * @param val   Hodnota.
 * @param bits  Počet bitů (8 / 16 / 32).
 * @param out   Výstup.
 * @param sz    Velikost.
 */
static void watch_fmt_bin ( uint64_t val, int bits, char *out, size_t sz ) {
    if ( sz < (size_t) ( bits + 3 ) ) {
        if ( sz > 0 ) out[0] = '\0';
        return;
    };
    out[0] = '0';
    out[1] = 'b';
    for ( int i = 0; i < bits; i++ ) {
        int bit = bits - 1 - i;
        out[ 2 + i ] = ( ( val >> bit ) & 1u ) ? '1' : '0';
    };
    out[ 2 + bits ] = '\0';
}


/**
 * @brief Sign-extend zarovnání na 64-bit pro signed typy.
 */
static int64_t watch_sign_extend ( uint64_t v, int bits ) {
    uint64_t mask  = ( bits >= 64 ) ? ~(uint64_t) 0 : ( ( (uint64_t) 1 << bits ) - 1 );
    uint64_t value = v & mask;
    uint64_t sign  = (uint64_t) 1 << ( bits - 1 );
    if ( value & sign ) {
        return (int64_t) ( value | ~mask );
    };
    return (int64_t) value;
}


void watch_format_int ( const st_WATCH_ROW *row, uint64_t value,
                         char *out, size_t out_size ) {
    if ( !out || out_size == 0 ) return;
    out[0] = '\0';
    if ( !row ) return;

    if ( row->type == WATCH_TYPE_BIT ) {
        snprintf ( out, out_size, "%u", (unsigned) ( value & 1u ) );
        return;
    };

    /* Typové šířky pro mask + bin display. */
    int bits = 0;
    bool is_signed = false;
    switch ( row->type ) {
        case WATCH_TYPE_U8:    bits = 8;  is_signed = false; break;
        case WATCH_TYPE_I8:    bits = 8;  is_signed = true;  break;
        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_U16BE: bits = 16; is_signed = false; break;
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE: bits = 16; is_signed = true;  break;
        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_U32BE: bits = 32; is_signed = false; break;
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE: bits = 32; is_signed = true;  break;
        default:
            /* Non-int typ -> caller chyba; bezpečný fallback. */
            return;
    };

    uint64_t mask = ( bits >= 64 ) ? ~(uint64_t) 0
                                     : ( ( (uint64_t) 1 << bits ) - 1 );
    uint64_t v    = value & mask;

    switch ( row->fmt ) {
        case WATCH_FMT_DEC:
            if ( is_signed ) {
                int64_t sv = watch_sign_extend ( v, bits );
                snprintf ( out, out_size, "%" G_GINT64_FORMAT, (gint64) sv );
            } else {
                snprintf ( out, out_size, "%" G_GUINT64_FORMAT, (guint64) v );
            };
            break;

        case WATCH_FMT_HEX: {
            int width = bits / 4;
            snprintf ( out, out_size, "0x%0*" G_GINT64_MODIFIER "X",
                       width, (guint64) v );
            break;
        }

        case WATCH_FMT_BIN:
            watch_fmt_bin ( v, bits, out, out_size );
            break;

        case WATCH_FMT_CHAR:
            if ( row->type == WATCH_TYPE_U8 || row->type == WATCH_TYPE_I8 ) {
                uint8_t b = (uint8_t) ( v & 0xFF );
                if ( b >= 0x20 && b < 0x7F ) {
                    snprintf ( out, out_size, "'%c'", (char) b );
                } else {
                    /* Non-printable -> hex fallback. */
                    snprintf ( out, out_size, "0x%02X", b );
                };
            } else {
                /* Multibyte char fmt nedává smysl -> fallback na hex. */
                int width = bits / 4;
                snprintf ( out, out_size, "0x%0*" G_GINT64_MODIFIER "X",
                           width, (guint64) v );
            };
            break;
    };
}


void watch_format_ascii ( const uint8_t *buf, size_t len,
                           char *out, size_t out_size,
                           bool sharp_translation ) {
    if ( !out || out_size == 0 ) return;
    out[0] = '\0';
    if ( !buf || len == 0 ) return;

    size_t pos = 0;
    for ( size_t i = 0; i < len; i++ ) {
        uint8_t b = buf[ i ];

        if ( sharp_translation ) {
            /* Sharp MZ EU charset -> UTF-8 řetězec (1-4 bajty). Pokud znak
             * nemá Unicode ekvivalent, vrací U+FFFD (3 bajty UTF-8). */
            const char *utf = sharpmz_to_utf8 ( b, SHARPMZ_CHARSET_EU );
            size_t l = strlen ( utf );
            if ( pos + l + 1 >= out_size ) break;
            memcpy ( out + pos, utf, l );
            pos += l;
        } else {
            /* Plain ASCII: printable přímo, jinak `\xNN` escape, `\` -> `\\`. */
            if ( b == '\\' ) {
                if ( pos + 2 + 1 >= out_size ) break;
                out[ pos++ ] = '\\';
                out[ pos++ ] = '\\';
            } else if ( b >= 0x20 && b < 0x7F ) {
                if ( pos + 1 + 1 >= out_size ) break;
                out[ pos++ ] = (char) b;
            } else {
                if ( pos + 4 + 1 >= out_size ) break;
                int n = snprintf ( out + pos, out_size - pos, "\\x%02X", b );
                if ( n < 0 ) break;
                pos += (size_t) n;
            };
        };
    };
    out[ pos ] = '\0';
}


bool watch_format_bytes ( const uint8_t *buf, size_t len,
                           char *out, size_t out_size,
                           size_t max_inline ) {
    bool truncated = false;
    if ( !out || out_size == 0 ) return false;
    out[0] = '\0';
    if ( !buf || len == 0 ) return false;

    size_t shown = len;
    if ( max_inline > 0 && shown > max_inline ) {
        shown = max_inline;
        truncated = true;
    };

    size_t pos = 0;
    for ( size_t i = 0; i < shown; i++ ) {
        /* `NN ` = 3 znaky + místo pro NUL */
        if ( pos + 4 >= out_size ) {
            truncated = true;
            break;
        };
        int n;
        if ( i == 0 ) {
            n = snprintf ( out + pos, out_size - pos, "%02X", buf[ i ] );
        } else {
            n = snprintf ( out + pos, out_size - pos, " %02X", buf[ i ] );
        };
        if ( n < 0 ) break;
        pos += (size_t) n;
    };

    if ( truncated && pos + 5 < out_size ) {
        snprintf ( out + pos, out_size - pos, " ..." );
    };

    return truncated;
}


/* ========================================================================= */
/*  Type / fmt string mapping                                                */
/* ========================================================================= */


const char *watch_type_to_str ( en_WATCH_TYPE t ) {
    switch ( t ) {
        case WATCH_TYPE_U8:      return "u8";
        case WATCH_TYPE_I8:      return "i8";
        case WATCH_TYPE_U16LE:   return "u16le";
        case WATCH_TYPE_U16BE:   return "u16be";
        case WATCH_TYPE_I16LE:   return "i16le";
        case WATCH_TYPE_I16BE:   return "i16be";
        case WATCH_TYPE_U32LE:   return "u32le";
        case WATCH_TYPE_U32BE:   return "u32be";
        case WATCH_TYPE_I32LE:   return "i32le";
        case WATCH_TYPE_I32BE:   return "i32be";
        case WATCH_TYPE_BIT:     return "bit";
        case WATCH_TYPE_ASCII:   return "ascii";
        case WATCH_TYPE_MZASCII: return "mzascii";
        case WATCH_TYPE_BYTES:   return "bytes";
    };
    return "u8";  /* defenzivní fallback */
}


bool watch_type_from_str ( const char *s, en_WATCH_TYPE *out ) {
    if ( !s || !out ) return false;
    if ( strcmp ( s, "u8"      ) == 0 ) { *out = WATCH_TYPE_U8;      return true; };
    if ( strcmp ( s, "i8"      ) == 0 ) { *out = WATCH_TYPE_I8;      return true; };
    if ( strcmp ( s, "u16le"   ) == 0 ) { *out = WATCH_TYPE_U16LE;   return true; };
    if ( strcmp ( s, "u16be"   ) == 0 ) { *out = WATCH_TYPE_U16BE;   return true; };
    if ( strcmp ( s, "i16le"   ) == 0 ) { *out = WATCH_TYPE_I16LE;   return true; };
    if ( strcmp ( s, "i16be"   ) == 0 ) { *out = WATCH_TYPE_I16BE;   return true; };
    if ( strcmp ( s, "u32le"   ) == 0 ) { *out = WATCH_TYPE_U32LE;   return true; };
    if ( strcmp ( s, "u32be"   ) == 0 ) { *out = WATCH_TYPE_U32BE;   return true; };
    if ( strcmp ( s, "i32le"   ) == 0 ) { *out = WATCH_TYPE_I32LE;   return true; };
    if ( strcmp ( s, "i32be"   ) == 0 ) { *out = WATCH_TYPE_I32BE;   return true; };
    if ( strcmp ( s, "bit"     ) == 0 ) { *out = WATCH_TYPE_BIT;     return true; };
    if ( strcmp ( s, "ascii"   ) == 0 ) { *out = WATCH_TYPE_ASCII;   return true; };
    if ( strcmp ( s, "mzascii" ) == 0 ) { *out = WATCH_TYPE_MZASCII; return true; };
    if ( strcmp ( s, "bytes"   ) == 0 ) { *out = WATCH_TYPE_BYTES;   return true; };
    return false;
}


const char *watch_fmt_to_str ( en_WATCH_FMT f ) {
    switch ( f ) {
        case WATCH_FMT_DEC:  return "dec";
        case WATCH_FMT_HEX:  return "hex";
        case WATCH_FMT_BIN:  return "bin";
        case WATCH_FMT_CHAR: return "char";
    };
    return "hex";
}


bool watch_fmt_from_str ( const char *s, en_WATCH_FMT *out ) {
    if ( !s || !out ) return false;
    if ( strcmp ( s, "dec"  ) == 0 ) { *out = WATCH_FMT_DEC;  return true; };
    if ( strcmp ( s, "hex"  ) == 0 ) { *out = WATCH_FMT_HEX;  return true; };
    if ( strcmp ( s, "bin"  ) == 0 ) { *out = WATCH_FMT_BIN;  return true; };
    if ( strcmp ( s, "char" ) == 0 ) { *out = WATCH_FMT_CHAR; return true; };
    return false;
}


const char *watch_mode_to_str ( en_WATCH_MODE m ) {
    switch ( m ) {
        case WATCH_MODE_ADDRESS:     return "address";
        case WATCH_MODE_EXPR_SCALAR: return "expr_scalar";
        case WATCH_MODE_EXPR_DEREF:  return "expr_deref";
    };
    return "address";
}


bool watch_mode_from_str ( const char *s, en_WATCH_MODE *out ) {
    if ( !s || !out ) return false;
    if ( strcmp ( s, "address"     ) == 0 ) { *out = WATCH_MODE_ADDRESS;     return true; };
    if ( strcmp ( s, "expr_scalar" ) == 0 ) { *out = WATCH_MODE_EXPR_SCALAR; return true; };
    if ( strcmp ( s, "expr_deref"  ) == 0 ) { *out = WATCH_MODE_EXPR_DEREF;  return true; };
    return false;
}


/* ========================================================================= */
/*  Parse addr                                                               */
/* ========================================================================= */


/**
 * @brief Pomocná funkce - skip leading whitespace.
 */
static const char *skip_ws ( const char *p ) {
    while ( *p && ( *p == ' ' || *p == '\t' ) ) p++;
    return p;
}


/**
 * @brief Pokusí se rozpoznat čistě číselný literál (hex / dec / IASM hex).
 *
 * @return true při úspěšném parse + naplnění `out_addr`, false jinak.
 */
static bool watch_try_parse_numeric ( const char *input, uint16_t *out_addr ) {
    if ( !input || !input[0] ) return false;

    /* Skopíruj do bufferu se striped whitespace pro snazší práci. */
    char buf[64];
    size_t i = 0;
    const char *p = skip_ws ( input );
    while ( *p && *p != ' ' && *p != '\t' && i + 1 < sizeof ( buf ) ) {
        buf[ i++ ] = *p++;
    };
    buf[ i ] = '\0';
    p = skip_ws ( p );
    if ( *p ) return false;        /* nečekané znaky za hodnotou */
    if ( i == 0 ) return false;

    int base = 10;
    const char *body = buf;
    size_t body_len = i;

    /* C-style hex prefix. */
    if ( i >= 2 && buf[0] == '0' && ( buf[1] == 'x' || buf[1] == 'X' ) ) {
        base = 16;
        body = buf + 2;
        body_len = i - 2;
    }
    /* IASM hex suffix `h` / `H`. */
    else if ( i >= 1 && ( buf[ i - 1 ] == 'h' || buf[ i - 1 ] == 'H' ) ) {
        /* Musí začínat číslicí (= rozliš `Ch` register vs `0Ch` hex). */
        if ( !( buf[0] >= '0' && buf[0] <= '9' ) ) return false;
        base = 16;
        body_len = i - 1;
        /* Body je buf, jen s ohraničenou délkou (přidat \0 do bufferu). */
        buf[ body_len ] = '\0';
        body = buf;
    };
    if ( body_len == 0 ) return false;

    /* Strict validace znaků těla. */
    for ( size_t j = 0; j < body_len; j++ ) {
        char c = body[ j ];
        bool ok;
        if ( base == 10 ) {
            ok = ( c >= '0' && c <= '9' );
        } else {
            ok = ( ( c >= '0' && c <= '9' ) ||
                   ( c >= 'a' && c <= 'f' ) ||
                   ( c >= 'A' && c <= 'F' ) );
        };
        if ( !ok ) return false;
    };

    /* Parse přes strtoul. */
    char tmp[32];
    if ( body_len + 1 > sizeof ( tmp ) ) return false;
    memcpy ( tmp, body, body_len );
    tmp[ body_len ] = '\0';

    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul ( tmp, &end, base );
    if ( errno == ERANGE ) return false;
    if ( end == tmp ) return false;
    if ( *end ) return false;
    if ( v > 0xFFFFUL ) return false;

    *out_addr = (uint16_t) v;
    return true;
}


bool watch_parse_address ( const char *input, uint16_t *out_addr ) {
    if ( !input || !out_addr ) return false;

    /* 1. Pokus o numerický parse (hex / dec / IASM hex). */
    if ( watch_try_parse_numeric ( input, out_addr ) ) {
        return true;
    };

    /* 2. Pokus o symbol lookup. Vstup musí být striped od whitespace. */
    char sym[ 128 ];
    size_t i = 0;
    const char *p = skip_ws ( input );
    while ( *p && *p != ' ' && *p != '\t' && i + 1 < sizeof ( sym ) ) {
        sym[ i++ ] = *p++;
    };
    sym[ i ] = '\0';
    p = skip_ws ( p );
    if ( *p || i == 0 ) return false;

    const st_SYMBOL *s = sym_db_lookup_by_name ( sym );
    if ( !s ) return false;
    if ( s->addr > 0xFFFFu ) return false;  /* physical/banked - L0 nepodporuje */

    *out_addr = (uint16_t) s->addr;
    return true;
}


/* ========================================================================= */
/*  Persistence                                                              */
/* ========================================================================= */


/**
 * @brief Resolve path - NULL = default per-arch file v cfg dir.
 *
 * @return Heap-allocated string (caller g_free), nebo NULL pokud nelze
 *         resolveovat (= cfg dir nedostupný a path je NULL).
 */
static char *watch_resolve_path ( const char *path ) {
    if ( path && path[0] ) {
        if ( !g_sdlapp || !g_sdlapp->paths ) {
            return g_strdup ( path );
        };
        return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, path );
    };
    return watch_default_filepath ( );
}


char *watch_default_filepath ( void ) {
    /* Jmeno souboru per arch = zmrazeny on-disk kontrakt, runtime
     * z g_mzhal (mzhal 11g). */
    char fname[32];
    g_snprintf ( fname, sizeof ( fname ), "%s.watch", g_mzhal.arch_name );
    const char *use = ( s_watch_cfg.watch_file && s_watch_cfg.watch_file[0] )
                      ? s_watch_cfg.watch_file : fname;
    if ( !g_sdlapp || !g_sdlapp->paths ) {
        return g_strdup ( use );
    };
    return sdlapp_paths_resolve_cfg ( g_sdlapp->paths, use );
}


/**
 * @brief V1.E.6.B - en_DBGAPI_CMD_ORIGIN -> stable persist token.
 *
 * Stable tokeny "user" / "mcp" / "test" / "internal" napříč verzemi
 * schématu - loader je tolerantní k unknown tokenům (default = USER).
 * Identický pattern jako v breakpoints.c a bookmarks.c.
 */
static const char *_watch_origin_to_str ( en_DBGAPI_CMD_ORIGIN o ) {
    switch ( o ) {
        case DBGAPI_CMD_ORIGIN_USER:     return "user";
        case DBGAPI_CMD_ORIGIN_MCP:      return "mcp";
        case DBGAPI_CMD_ORIGIN_TEST:     return "test";
        case DBGAPI_CMD_ORIGIN_INTERNAL: return "internal";
    }
    return "user";
}


/**
 * @brief V1.E.6.B - parse stable persist token -> en_DBGAPI_CMD_ORIGIN.
 *
 * Tolerantní k missing / unknown tokenům: fallback na USER (= GUI default).
 * Stejné chování jako v breakpoints.c a bookmarks.c.
 */
static en_DBGAPI_CMD_ORIGIN _watch_origin_from_str ( const char *s ) {
    if ( !s || !s[0] ) return DBGAPI_CMD_ORIGIN_USER;
    if ( strcmp ( s, "mcp" )      == 0 ) return DBGAPI_CMD_ORIGIN_MCP;
    if ( strcmp ( s, "test" )     == 0 ) return DBGAPI_CMD_ORIGIN_TEST;
    if ( strcmp ( s, "internal" ) == 0 ) return DBGAPI_CMD_ORIGIN_INTERNAL;
    return DBGAPI_CMD_ORIGIN_USER;
}


/**
 * @brief Emit jeden řádek jako JSON object.
 *
 * Volitelné fieldy (fmt/length/bit_index) se emitují jen pro typy, kde
 * mají smysl - to udržuje JSON minimální a BC s Phase A formátem.
 * V1.E.6.B - emituje navíc "origin" pole (stable token "user"/"mcp"/
 * "test"/"internal").
 */
static void watch_emit_one ( JsonBuilder *b, const st_WATCH_ROW *r ) {
    json_builder_begin_object ( b );

    if ( r->name && r->name[0] ) {
        json_builder_set_member_name ( b, "name" );
        json_builder_add_string_value ( b, r->name );
    };
    json_builder_set_member_name ( b, "addr" );
    json_builder_add_int_value ( b, (gint64) r->addr );
    json_builder_set_member_name ( b, "bank" );
    json_builder_add_int_value ( b, (gint64) r->bank );
    json_builder_set_member_name ( b, "type" );
    json_builder_add_string_value ( b, watch_type_to_str ( r->type ) );

    /* Phase C: mode + expr. Emitujeme jen pokud != ADDRESS (= JSON
     * minimální + BC kompat s Phase A/B soubory). */
    if ( r->mode != WATCH_MODE_ADDRESS ) {
        json_builder_set_member_name ( b, "mode" );
        json_builder_add_string_value ( b, watch_mode_to_str ( r->mode ) );
        if ( r->expr_text ) {
            json_builder_set_member_name ( b, "expr" );
            json_builder_add_string_value ( b, r->expr_text );
        };
    };

    if ( watch_type_has_format ( r->type ) ) {
        json_builder_set_member_name ( b, "fmt" );
        json_builder_add_string_value ( b, watch_fmt_to_str ( r->fmt ) );
    };
    if ( watch_type_has_length ( r->type ) ) {
        json_builder_set_member_name ( b, "length" );
        json_builder_add_int_value ( b, (gint64) r->length );
    };
    if ( r->type == WATCH_TYPE_BIT ) {
        json_builder_set_member_name ( b, "bit_index" );
        json_builder_add_int_value ( b, (gint64) r->bit_index );
    };

    /* V1.E.6.B - persist cmd_origin pro audit trail. Stable token
     * "user"/"mcp"/"test"/"internal"; load tolerantní k missing klíči
     * (default USER) a unknown tokenům (= forward compat). */
    json_builder_set_member_name ( b, "origin" );
    json_builder_add_string_value ( b, _watch_origin_to_str ( r->cmd_origin ) );

    json_builder_end_object ( b );
}


bool watch_save_to_filepath ( const char *path ) {
    char *resolved = watch_resolve_path ( path );
    if ( !resolved ) return false;

    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "watch" );
    json_builder_begin_array ( b );
    {
        size_t n = watch_count ( );
        for ( size_t i = 0; i < n; i++ ) {
            const st_WATCH_ROW *r = watch_get ( i );
            if ( !r ) continue;
            watch_emit_one ( b, r );
        };
    };
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
        ok = false;
    } else {
        size_t wrote = fwrite ( out, 1, len, fp );
        if ( wrote != len ) ok = false;
        fclose ( fp );
    };

    g_free ( out );
    g_object_unref ( gen );
    g_object_unref ( b );
    g_free ( resolved );
    return ok;
}


/**
 * @brief Helper - vrátí string field nebo default (s NULL safety).
 */
static const char *watch_json_str_or ( JsonObject *obj, const char *key,
                                        const char *def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_string ( node );
}


/**
 * @brief Helper - vrátí int field nebo default.
 */
static gint64 watch_json_int_or ( JsonObject *obj, const char *key, gint64 def ) {
    if ( !json_object_has_member ( obj, key ) ) return def;
    JsonNode *node = json_object_get_member ( obj, key );
    if ( !node || json_node_is_null ( node ) ) return def;
    if ( json_node_get_node_type ( node ) != JSON_NODE_VALUE ) return def;
    return json_node_get_int ( node );
}


bool watch_load_from_filepath ( const char *path ) {
    char *resolved = watch_resolve_path ( path );
    if ( !resolved ) return false;

    /* Soubor neexistuje - storage se vyčistí (REPLACE sémantika) a vrátí
     * success. Stejný pattern jako bp_vars. */
    if ( !g_file_test ( resolved, G_FILE_TEST_EXISTS ) ) {
        watch_clear_storage ( );
        g_free ( resolved );
        return true;
    };

    JsonParser *parser = json_parser_new ( );
    GError *err = NULL;
    if ( !json_parser_load_from_file ( parser, resolved, &err ) ) {
        fprintf ( stderr, "WATCH WARNING: parse error in '%s': %s\n",
                  resolved, err ? err->message : "unknown" );
        if ( err ) g_error_free ( err );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonNode *root = json_parser_get_root ( parser );
    if ( !root || json_node_get_node_type ( root ) != JSON_NODE_OBJECT ) {
        fprintf ( stderr, "WATCH WARNING: root is not a JSON object in '%s'\n",
                  resolved );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonObject *root_obj = json_node_get_object ( root );

    /* REPLACE mode - wipne před loadem. */
    watch_clear_storage ( );

    if ( !json_object_has_member ( root_obj, "watch" ) ) {
        /* Soubor bez "watch" pole je tolerantně OK (= prázdný stav). */
        g_object_unref ( parser );
        g_free ( resolved );
        return true;
    };

    JsonNode *wn = json_object_get_member ( root_obj, "watch" );
    if ( !wn || json_node_get_node_type ( wn ) != JSON_NODE_ARRAY ) {
        fprintf ( stderr, "WATCH WARNING: 'watch' is not an array in '%s'\n",
                  resolved );
        g_object_unref ( parser );
        g_free ( resolved );
        return false;
    };

    JsonArray *warr = json_node_get_array ( wn );
    guint n = json_array_get_length ( warr );
    for ( guint i = 0; i < n; i++ ) {
        JsonNode *en = json_array_get_element ( warr, i );
        if ( !en || json_node_get_node_type ( en ) != JSON_NODE_OBJECT ) continue;
        JsonObject *o = json_node_get_object ( en );

        /* addr je povinné. */
        if ( !json_object_has_member ( o, "addr" ) ) continue;
        gint64 addr64 = watch_json_int_or ( o, "addr", -1 );
        if ( addr64 < 0 || addr64 > 0xFFFF ) {
            fprintf ( stderr, "WATCH WARNING: addr out of range (%" G_GINT64_FORMAT
                              ") in '%s' (row %u)\n",
                      addr64, resolved, i );
            continue;
        };

        /* type - default "u8" (BC s Phase A). Neznámý = skip s warning. */
        const char *tstr = watch_json_str_or ( o, "type", "u8" );
        en_WATCH_TYPE t;
        if ( !watch_type_from_str ( tstr, &t ) ) {
            fprintf ( stderr, "WATCH WARNING: unknown type '%s' in '%s' "
                              "(row %u), skipping\n",
                      tstr, resolved, i );
            continue;
        };

        const char *name = watch_json_str_or ( o, "name", NULL );
        gint64 bank64 = watch_json_int_or ( o, "bank", -1 );

        int idx = watch_add ( name, (uint16_t) addr64, (int) bank64 );
        if ( idx < 0 ) continue;

        /* Aplikuj type (nastaví fmt default + length default). */
        watch_set_type ( idx, t );

        /* fmt - volitelné, BC s Phase A (chybí = default per typ). */
        if ( watch_type_has_format ( t ) ) {
            const char *fstr = watch_json_str_or ( o, "fmt", NULL );
            if ( fstr ) {
                en_WATCH_FMT f;
                if ( watch_fmt_from_str ( fstr, &f ) ) {
                    watch_set_fmt ( idx, f );
                } else {
                    fprintf ( stderr, "WATCH WARNING: unknown fmt '%s' in '%s' "
                                      "(row %u), using default\n",
                              fstr, resolved, i );
                };
            };
        };

        /* length - volitelné pro ascii/mzascii/bytes. */
        if ( watch_type_has_length ( t ) ) {
            gint64 len64 = watch_json_int_or ( o, "length", -1 );
            if ( len64 > 0 && len64 <= (gint64) watch_type_max_length ( t ) ) {
                watch_set_length ( idx, (uint16_t) len64 );
            };
            /* jinak default už nastaven přes watch_set_type */
        };

        /* bit_index - volitelné pro WATCH_TYPE_BIT. */
        if ( t == WATCH_TYPE_BIT ) {
            gint64 bi64 = watch_json_int_or ( o, "bit_index", 0 );
            if ( bi64 < 0 ) bi64 = 0;
            if ( bi64 > 7 ) bi64 = 7;
            watch_set_bit_index ( idx, (uint8_t) bi64 );
        };

        /* Phase C: mode + expr. Volitelné, default ADDRESS = BC s Phase A/B. */
        const char *mstr = watch_json_str_or ( o, "mode", NULL );
        if ( mstr ) {
            en_WATCH_MODE m;
            if ( watch_mode_from_str ( mstr, &m ) ) {
                if ( m != WATCH_MODE_ADDRESS ) {
                    const char *expr = watch_json_str_or ( o, "expr", NULL );
                    if ( expr && expr[0] ) {
                        watch_set_expr ( idx, expr, m );
                    } else {
                        fprintf ( stderr, "WATCH WARNING: mode '%s' without "
                                          "expr in '%s' (row %u), keeping "
                                          "address mode\n",
                                  mstr, resolved, i );
                    };
                };
            } else {
                fprintf ( stderr, "WATCH WARNING: unknown mode '%s' in '%s' "
                                  "(row %u), using address\n",
                          mstr, resolved, i );
            };
        };

        /* V1.E.6.B - load cmd_origin tolerantně (= chybějící klíč defaultuje
         * na USER, neznámé tokeny taky na USER). Vlastník je tedy zachován
         * přes save/load cyklus pro audit trail. */
        const char *origin_str = watch_json_str_or ( o, "origin", NULL );
        watch_set_cmd_origin ( idx, _watch_origin_from_str ( origin_str ) );
    };

    g_object_unref ( parser );
    g_free ( resolved );
    return true;
}


/* ========================================================================= */
/*  Cfg sekce [WATCH_PANEL]                                                  */
/* ========================================================================= */


void watch_cfg_init ( void ) {
    static char default_watch_file[32];
    g_snprintf ( default_watch_file, sizeof ( default_watch_file ),
                 "%s.watch", g_mzhal.arch_name );
#define DEFAULT_WATCH_FILE default_watch_file

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "WATCH_PANEL" );
    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, "watch_file", CFGENTYPE_TEXT,
                                            DEFAULT_WATCH_FILE );
    cfgelement_bind ( elm, (void *) &s_watch_cfg.watch_file );

    elm = cfgmodule_register_new_element ( cmod, "auto_save", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void *) &s_watch_cfg.auto_save );

    elm = cfgmodule_register_new_element ( cmod, "auto_load", CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void *) &s_watch_cfg.auto_load );

    /* Phase E.1: highlight changes toggle + fade duration. */
    elm = cfgmodule_register_new_element ( cmod, "highlight_changes",
                                            CFGENTYPE_BOOL, 1 );
    cfgelement_bind ( elm, (void *) &s_watch_cfg.highlight_changes );

    elm = cfgmodule_register_new_element ( cmod, "highlight_fade_ms",
                                            CFGENTYPE_UNSIGNED, 500, 1, 10000 );
    cfgelement_bind ( elm, (void *) &s_watch_cfg.highlight_fade_ms );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );

#undef DEFAULT_WATCH_FILE

    if ( s_watch_cfg.auto_load ) {
        if ( !watch_load_from_filepath ( NULL ) ) {
            fprintf ( stderr, "WATCH WARNING: auto-load failed\n" );
        };
    };
}


void watch_cfg_auto_save ( void ) {
    if ( !s_watch_cfg.auto_save ) return;
    if ( !watch_save_to_filepath ( NULL ) ) {
        fprintf ( stderr, "WATCH WARNING: auto-save failed\n" );
    };
}


bool watch_cfg_get_auto_load ( void ) {
    return s_watch_cfg.auto_load;
}


void watch_cfg_set_auto_load ( bool enable ) {
    s_watch_cfg.auto_load = enable;
}


bool watch_cfg_get_auto_save ( void ) {
    return s_watch_cfg.auto_save;
}


void watch_cfg_set_auto_save ( bool enable ) {
    s_watch_cfg.auto_save = enable;
}


bool watch_cfg_get_highlight_changes ( void ) {
    return s_watch_cfg.highlight_changes;
}


void watch_cfg_set_highlight_changes ( bool enable ) {
    s_watch_cfg.highlight_changes = enable;
}


int watch_cfg_get_highlight_fade_ms ( void ) {
    return (int) s_watch_cfg.highlight_fade_ms;
}


void watch_cfg_set_highlight_fade_ms ( int ms ) {
    if ( ms < 1 ) ms = 1;
    s_watch_cfg.highlight_fade_ms = (uint32_t) ms;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
