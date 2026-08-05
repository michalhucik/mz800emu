/*
 * File:   bp_expr.c
 *
 * Implementace expression evaluatoru pro smart breakpointy (V1, fáze D.1.2).
 *
 * Tří-fázová pipeline:
 *   1. Lexer  (tokenizer)  - text -> stream tokenů
 *   2. Parser (recursive descent) - tokeny -> AST se stromovou strukturou
 *   3. Evaluator           - AST + kontext -> int32_t hodnota
 *
 * Veřejné API: viz bp_expr.h.
 *
 * Konvence číselných literálů odpovídá IASM (src/libs/iasm/iasm.c):
 *   - 0xNNNN / #NNNN  hex
 *   - %NNNN           bin
 *   - NNNN            dec
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

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <strings.h>

#include "bp_expr.h"
#include "bp_event.h"
#include "bp_vars.h"
#include "libs/cpu-z80/z80.h"
#include "hw-generic/memory/memory.h"

/* V1.6+ TODO 4.2 5b: per-arch IORQ no_se probe API. Side-effect read
 * pres regular port_read_cb(); side-effect-free pres port_read_no_se_cb().
 *
 * Pro side_effect=true volame port_read_cb() s NULL z80_t pointer + NULL
 * user_data - funkce je side-effect-aware ale nepouziva CPU register
 * state pro probe (= read jen z dispatch tabulky port -> chip).
 *
 * Pozn.: side_effect=true cesta se v expression pouziva minimalne -
 * uzivatel typicky chce probe bez mutating chipu. Pro `port[N]!` (=
 * explicit side-effect) je read fyzicky proveden = stejne jako
 * Z80 IN inst.  */
/* Kontrakt: kazda architektura implementuje ve svem mz*_iorq.c
 * IORQ callbacky se shodnymi prototypy (viz mz*_iorq.h). */
extern uint8_t port_read_no_se_cb(uint16_t addr);
extern uint8_t port_read_cb(z80_t *cpu, uint16_t addr, void *user_data);


/* ========================================================================= */
/*  Token types                                                              */
/* ========================================================================= */

/**
 * @brief Druhy tokenů produkované lexerem.
 *
 * Hodnoty stabilní v rámci buildu, neserializují se.
 */
typedef enum {
    TOK_END = 0,        /**< Konec vstupu */
    TOK_INT,            /**< Numerický literál (lex_value) */
    TOK_IDENT,          /**< Identifikátor / klíčové slovo (lex_text) */
    TOK_VAR,            /**< $name (user var, lex_text bez $) */
    TOK_LPAREN,         /**< ( */
    TOK_RPAREN,         /**< ) */
    TOK_LBRACK,         /**< [ */
    TOK_RBRACK,         /**< ] */
    TOK_LBRACE,         /**< { */
    TOK_RBRACE,         /**< } */
    TOK_COMMA,          /**< , */
    TOK_BANG,           /**< ! (logické NOT i suffix pro side-effect) */
    TOK_TILDE,          /**< ~ */
    TOK_PLUS,           /**< + */
    TOK_MINUS,          /**< - */
    TOK_STAR,           /**< * */
    TOK_SLASH,          /**< / */
    TOK_PERCENT,        /**< % (modulo - bin literal je v lexeru oddělen) */
    TOK_LSHIFT,         /**< << */
    TOK_RSHIFT,         /**< >> */
    TOK_LT,             /**< < */
    TOK_LE,             /**< <= */
    TOK_GT,             /**< > */
    TOK_GE,             /**< >= */
    TOK_EQ,             /**< == */
    TOK_NEQ,            /**< != */
    TOK_AMP,            /**< & */
    TOK_AMPAMP,         /**< && */
    TOK_CARET,          /**< ^ */
    TOK_PIPE,           /**< | */
    TOK_PIPEPIPE,       /**< || */
    TOK_ERROR           /**< Chybový token (= lexer detekoval invalid) */
} tok_kind_t;


#define BP_EXPR_IDENT_MAX  31   /**< Max délka identifier včetně apostrofu */


/**
 * @brief Jeden token z lexeru.
 *
 * Hodnota tokenu je uložená podle kind:
 *   - TOK_INT: ve value
 *   - TOK_IDENT, TOK_VAR: v text (NUL-terminated, lowercase pro IDENT
 *     - case-insensitive registers)
 *   - ostatní: jen kind
 */
typedef struct {
    tok_kind_t kind;
    int32_t    value;
    char       text[BP_EXPR_IDENT_MAX + 1];
} bp_tok_t;


/* ========================================================================= */
/*  Lexer state                                                              */
/* ========================================================================= */

/**
 * @brief Stav lexeru během tokenizace.
 *
 * Lexer je single-pass dopředný, prev_was_value drží disambiguaci pro
 * '%' (= modulo vs bin literal podle pozice ve výrazu).
 */
typedef struct {
    const char *src;        /**< Zdrojový text */
    size_t      pos;        /**< Aktuální pozice v src */
    size_t      len;        /**< Celková délka src */
    bool        prev_was_value;  /**< Předchozí token byl hodnota/identifier/RPAREN/RBRACK/RBRACE */
    char       *errbuf;     /**< Buffer pro chybu (nebo NULL) */
    size_t      errbuf_size;/**< Velikost errbuf */
} bp_lexer_t;


/**
 * @brief Zapíše chybovou hlášku do errbuf (pokud je k dispozici).
 *
 * Pokud lex->errbuf je NULL nebo size <=0, hláška se zahodí. Funkce
 * vždy vrací false (= idiom pro `return bp_err(...)`).
 */
static bool bp_err ( bp_lexer_t *lex, const char *fmt, ... ) __attribute__((format(printf,2,3)));
static bool bp_err ( bp_lexer_t *lex, const char *fmt, ... ) {
    if ( lex->errbuf && lex->errbuf_size > 0 ) {
        va_list ap;
        va_start ( ap, fmt );
        vsnprintf ( lex->errbuf, lex->errbuf_size, fmt, ap );
        va_end ( ap );
    };
    return false;
}


/* Posune pos přes whitespace. */
static void lex_skip_ws ( bp_lexer_t *lex ) {
    while ( lex->pos < lex->len && isspace ( (unsigned char) lex->src[lex->pos] ) ) {
        lex->pos++;
    };
}


/* Vrátí true pokud znak smí být součástí identifier (ne první). */
static bool lex_is_ident_cont ( char c ) {
    return isalnum ( (unsigned char) c ) || c == '_' || c == '\'';
}


/* Vrátí true pokud znak smí být první znak identifikátoru. */
static bool lex_is_ident_start ( char c ) {
    return isalpha ( (unsigned char) c ) || c == '_';
}


/**
 * @brief Načte hex literál začínající od src[pos] (po '0x' nebo '#' prefixu).
 *
 * Spotřebuje co nejvíc hex číslic (= [0-9a-fA-F]), výsledek do *out.
 * @return Počet spotřebovaných hex znaků (0 = chyba).
 */
static int lex_read_hex ( bp_lexer_t *lex, int32_t *out ) {
    int32_t v = 0;
    int n = 0;
    while ( lex->pos < lex->len ) {
        char c = lex->src[lex->pos];
        int d;
        if ( c >= '0' && c <= '9' ) d = c - '0';
        else if ( c >= 'a' && c <= 'f' ) d = c - 'a' + 10;
        else if ( c >= 'A' && c <= 'F' ) d = c - 'A' + 10;
        else break;
        v = (int32_t)( ( (uint32_t) v << 4 ) | (uint32_t) d );
        lex->pos++;
        n++;
    };
    *out = v;
    return n;
}


/**
 * @brief Načte binární literál po '%' prefixu.
 *
 * Spotřebuje co nejvíc 0/1 znaků.
 * @return Počet spotřebovaných znaků.
 */
static int lex_read_bin ( bp_lexer_t *lex, int32_t *out ) {
    int32_t v = 0;
    int n = 0;
    while ( lex->pos < lex->len ) {
        char c = lex->src[lex->pos];
        if ( c != '0' && c != '1' ) break;
        v = (int32_t)( ( (uint32_t) v << 1 ) | (uint32_t) ( c - '0' ) );
        lex->pos++;
        n++;
    };
    *out = v;
    return n;
}


/**
 * @brief Načte dekadický literál.
 *
 * @return Počet spotřebovaných číslic.
 */
static int lex_read_dec ( bp_lexer_t *lex, int32_t *out ) {
    int32_t v = 0;
    int n = 0;
    while ( lex->pos < lex->len ) {
        char c = lex->src[lex->pos];
        if ( c < '0' || c > '9' ) break;
        v = v * 10 + ( c - '0' );
        lex->pos++;
        n++;
    };
    *out = v;
    return n;
}


/**
 * @brief Načte další token ze vstupu.
 *
 * @param lex State lexeru (mutable).
 * @param tok Cíl, naplní se kind + hodnotou (value/text dle kind).
 * @return true při úspěchu (i TOK_END), false při lexer chybě (= tok->kind
 *         se nastaví na TOK_ERROR a errbuf se zapíše).
 *
 * Disambig '%':
 *   - prev_was_value == false (= start nebo po operátoru/(/[)  → bin literal
 *   - prev_was_value == true  (= po hodnotě/RPAREN/...)         → modulo
 */
static bool lex_next ( bp_lexer_t *lex, bp_tok_t *tok ) {
    lex_skip_ws ( lex );
    memset ( tok, 0, sizeof ( *tok ) );

    if ( lex->pos >= lex->len ) {
        tok->kind = TOK_END;
        return true;
    };

    char c = lex->src[lex->pos];

    /* Numerické literály - hex 0x / 0X */
    if ( c == '0' && lex->pos + 1 < lex->len &&
         ( lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X' ) ) {
        lex->pos += 2;
        int n = lex_read_hex ( lex, &tok->value );
        if ( n == 0 ) {
            tok->kind = TOK_ERROR;
            return bp_err ( lex, "expected hex digits after 0x" );
        };
        tok->kind = TOK_INT;
        lex->prev_was_value = true;
        return true;
    };

    /* Numerický literál - hex s prefixem '#' */
    if ( c == '#' ) {
        lex->pos++;
        int n = lex_read_hex ( lex, &tok->value );
        if ( n == 0 ) {
            tok->kind = TOK_ERROR;
            return bp_err ( lex, "expected hex digits after #" );
        };
        tok->kind = TOK_INT;
        lex->prev_was_value = true;
        return true;
    };

    /* '%' - bin literal vs modulo (per kontextu) */
    if ( c == '%' ) {
        if ( !lex->prev_was_value ) {
            /* Bin literal */
            lex->pos++;
            int n = lex_read_bin ( lex, &tok->value );
            if ( n == 0 ) {
                tok->kind = TOK_ERROR;
                return bp_err ( lex, "expected binary digits after %%" );
            };
            tok->kind = TOK_INT;
            lex->prev_was_value = true;
            return true;
        } else {
            /* Modulo operátor */
            lex->pos++;
            tok->kind = TOK_PERCENT;
            lex->prev_was_value = false;
            return true;
        };
    };

    /* Dekadický literál */
    if ( c >= '0' && c <= '9' ) {
        int n = lex_read_dec ( lex, &tok->value );
        if ( n == 0 ) {
            tok->kind = TOK_ERROR;
            return bp_err ( lex, "internal: dec read 0 chars" );
        };
        tok->kind = TOK_INT;
        lex->prev_was_value = true;
        return true;
    };

    /* User var $name */
    if ( c == '$' ) {
        lex->pos++;
        if ( lex->pos >= lex->len || !lex_is_ident_start ( lex->src[lex->pos] ) ) {
            tok->kind = TOK_ERROR;
            return bp_err ( lex, "expected identifier after $" );
        };
        size_t i = 0;
        while ( lex->pos < lex->len && lex_is_ident_cont ( lex->src[lex->pos] ) ) {
            if ( i < BP_EXPR_IDENT_MAX ) {
                tok->text[i++] = lex->src[lex->pos];
            };
            lex->pos++;
        };
        tok->text[i] = '\0';
        tok->kind = TOK_VAR;
        lex->prev_was_value = true;
        return true;
    };

    /* Identifikátor / klíčové slovo */
    if ( lex_is_ident_start ( c ) ) {
        size_t i = 0;
        while ( lex->pos < lex->len && lex_is_ident_cont ( lex->src[lex->pos] ) ) {
            if ( i < BP_EXPR_IDENT_MAX ) {
                tok->text[i++] = lex->src[lex->pos];
            };
            lex->pos++;
        };
        tok->text[i] = '\0';
        tok->kind = TOK_IDENT;
        lex->prev_was_value = true;
        return true;
    };

    /* Operátory */
    lex->pos++;  /* spotřebuj první znak */
    switch ( c ) {
        case '(': tok->kind = TOK_LPAREN; lex->prev_was_value = false; return true;
        case ')': tok->kind = TOK_RPAREN; lex->prev_was_value = true;  return true;
        case '[': tok->kind = TOK_LBRACK; lex->prev_was_value = false; return true;
        case ']': tok->kind = TOK_RBRACK; lex->prev_was_value = true;  return true;
        case '{': tok->kind = TOK_LBRACE; lex->prev_was_value = false; return true;
        case '}': tok->kind = TOK_RBRACE; lex->prev_was_value = true;  return true;
        case ',': tok->kind = TOK_COMMA;  lex->prev_was_value = false; return true;
        case '~': tok->kind = TOK_TILDE;  lex->prev_was_value = false; return true;
        case '+': tok->kind = TOK_PLUS;   lex->prev_was_value = false; return true;
        case '-': tok->kind = TOK_MINUS;  lex->prev_was_value = false; return true;
        case '*': tok->kind = TOK_STAR;   lex->prev_was_value = false; return true;
        case '/': tok->kind = TOK_SLASH;  lex->prev_was_value = false; return true;
        case '^': tok->kind = TOK_CARET;  lex->prev_was_value = false; return true;
        case '!':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '=' ) {
                lex->pos++;
                tok->kind = TOK_NEQ;
                lex->prev_was_value = false;
            } else {
                tok->kind = TOK_BANG;
                /* prev_was_value zachováme - postfix '!' suffix po ']'
                 * znamená side-effect; prefix '!' znamená logical NOT. */
                /* Pro jednoduchost: TOK_BANG samo o sobě - parser
                 * rozezná použití podle pozice. Po BANG = false. */
                lex->prev_was_value = false;
            };
            return true;
        case '<':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '<' ) {
                lex->pos++; tok->kind = TOK_LSHIFT;
            } else if ( lex->pos < lex->len && lex->src[lex->pos] == '=' ) {
                lex->pos++; tok->kind = TOK_LE;
            } else {
                tok->kind = TOK_LT;
            };
            lex->prev_was_value = false;
            return true;
        case '>':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '>' ) {
                lex->pos++; tok->kind = TOK_RSHIFT;
            } else if ( lex->pos < lex->len && lex->src[lex->pos] == '=' ) {
                lex->pos++; tok->kind = TOK_GE;
            } else {
                tok->kind = TOK_GT;
            };
            lex->prev_was_value = false;
            return true;
        case '=':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '=' ) {
                lex->pos++; tok->kind = TOK_EQ;
                lex->prev_was_value = false;
                return true;
            };
            tok->kind = TOK_ERROR;
            return bp_err ( lex, "expected '==' (single '=' is not assignment)" );
        case '&':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '&' ) {
                lex->pos++; tok->kind = TOK_AMPAMP;
            } else {
                tok->kind = TOK_AMP;
            };
            lex->prev_was_value = false;
            return true;
        case '|':
            if ( lex->pos < lex->len && lex->src[lex->pos] == '|' ) {
                lex->pos++; tok->kind = TOK_PIPEPIPE;
            } else {
                tok->kind = TOK_PIPE;
            };
            lex->prev_was_value = false;
            return true;
    };

    tok->kind = TOK_ERROR;
    return bp_err ( lex, "unexpected character '%c'", c );
}


/* ========================================================================= */
/*  AST                                                                      */
/* ========================================================================= */

/**
 * @brief Druhy AST uzlů.
 */
typedef enum {
    NODE_INT,           /**< Konstantní int literál */
    NODE_IDENT,         /**< Identifikátor (registr / kontext / built-in const) */
    NODE_VAR,           /**< User $var */
    NODE_UNARY,         /**< Unární operátor */
    NODE_BINOP,         /**< Binární operátor */
    NODE_LOGAND,        /**< && (special - short-circuit) */
    NODE_LOGOR,         /**< || (special - short-circuit) */
    NODE_MEM,           /**< [addr] / [addr]! - byte memory deref */
    NODE_MEM_W,         /**< {addr} - word memory deref */
    NODE_PORT,          /**< port[N] / port[N]! - I/O port */
    NODE_FNCALL         /**< Built-in funkce */
} node_kind_t;


/**
 * @brief Druhy unárních operátorů.
 */
typedef enum {
    UN_PLUS,
    UN_MINUS,
    UN_NOT,             /**< logické ! */
    UN_BNOT             /**< bitové ~ */
} un_op_t;


/**
 * @brief Druhy binárních operátorů (kromě logických - viz NODE_LOGAND/OR).
 */
typedef enum {
    OP_MUL, OP_DIV, OP_MOD,
    OP_ADD, OP_SUB,
    OP_SHL, OP_SHR,
    OP_LT, OP_LE, OP_GT, OP_GE,
    OP_EQ, OP_NEQ,
    OP_AND, OP_XOR, OP_OR
} bin_op_t;


/**
 * @brief Built-in funkce.
 */
typedef enum {
    FN_MIN, FN_MAX, FN_ABS, FN_IF,
    FN_BIT, FN_S8, FN_S16, FN_S32,
    FN_INVALID
} fn_kind_t;


#define BP_EXPR_FN_MAX_ARGS  3


/**
 * @brief Tagged union AST uzlu.
 *
 * Vlastnictví: děti uzlů jsou heap-allocated (malloc), uzel je vlastní.
 * bp_node_free() rekurzivně uvolní celý strom.
 */
typedef struct bp_node_s {
    node_kind_t kind;
    union {
        struct { int32_t value; } i;
        struct { char name[BP_EXPR_IDENT_MAX + 1]; } id;
        struct { char name[BP_EXPR_IDENT_MAX + 1]; } var;
        struct { un_op_t op; struct bp_node_s *child; } un;
        struct { bin_op_t op; struct bp_node_s *l, *r; } bin;
        struct { struct bp_node_s *l, *r; } logical;
        struct { struct bp_node_s *addr; bool side_effect; } mem;
        struct { struct bp_node_s *port; bool side_effect; } port;
        struct {
            fn_kind_t fn;
            int n_args;
            struct bp_node_s *args[BP_EXPR_FN_MAX_ARGS];
        } fn;
    };
} bp_node_t;


struct st_BP_EXPR {
    bp_node_t *root;
};


/* Forward decl. */
static void bp_node_free ( bp_node_t *n );


static bp_node_t* bp_node_new ( node_kind_t kind ) {
    bp_node_t *n = (bp_node_t *) calloc ( 1, sizeof ( bp_node_t ) );
    if ( n ) n->kind = kind;
    return n;
}


static void bp_node_free ( bp_node_t *n ) {
    if ( !n ) return;
    switch ( n->kind ) {
        case NODE_UNARY:
            bp_node_free ( n->un.child );
            break;
        case NODE_BINOP:
            bp_node_free ( n->bin.l );
            bp_node_free ( n->bin.r );
            break;
        case NODE_LOGAND:
        case NODE_LOGOR:
            bp_node_free ( n->logical.l );
            bp_node_free ( n->logical.r );
            break;
        case NODE_MEM:
        case NODE_MEM_W:
            bp_node_free ( n->mem.addr );
            break;
        case NODE_PORT:
            bp_node_free ( n->port.port );
            break;
        case NODE_FNCALL:
            for ( int i = 0; i < n->fn.n_args; i++ ) {
                bp_node_free ( n->fn.args[i] );
            };
            break;
        case NODE_INT:
        case NODE_IDENT:
        case NODE_VAR:
            break;
    };
    free ( n );
}


/* ========================================================================= */
/*  Parser                                                                   */
/* ========================================================================= */

/**
 * @brief Stav parseru.
 *
 * Udržuje lexer a aktuální 1-token lookahead.
 */
typedef struct {
    bp_lexer_t lex;
    bp_tok_t   cur;
    bool       had_error;
} bp_parser_t;


/* Forward decl. */
static bp_node_t* parse_or ( bp_parser_t *p );
static bp_node_t* parse_unary ( bp_parser_t *p );
static bp_node_t* parse_primary ( bp_parser_t *p );


/* Posune cur token (při chybě nastaví had_error). */
static void parser_advance ( bp_parser_t *p ) {
    if ( !lex_next ( &p->lex, &p->cur ) ) {
        p->had_error = true;
    };
}


/* Strict eat - pokud aktuální není kind, error. */
static bool parser_eat ( bp_parser_t *p, tok_kind_t kind, const char *what ) {
    if ( p->cur.kind != kind ) {
        bp_err ( &p->lex, "expected %s", what );
        p->had_error = true;
        return false;
    };
    parser_advance ( p );
    return true;
}


/**
 * @brief Lookup built-in funkce podle jména.
 */
static fn_kind_t parser_lookup_fn ( const char *name ) {
    /* Case-insensitive */
    char lc[BP_EXPR_IDENT_MAX + 1];
    size_t i;
    for ( i = 0; name[i] && i < BP_EXPR_IDENT_MAX; i++ ) {
        lc[i] = (char) tolower ( (unsigned char) name[i] );
    };
    lc[i] = '\0';

    if ( strcmp ( lc, "min" ) == 0 ) return FN_MIN;
    if ( strcmp ( lc, "max" ) == 0 ) return FN_MAX;
    if ( strcmp ( lc, "abs" ) == 0 ) return FN_ABS;
    if ( strcmp ( lc, "if" )  == 0 ) return FN_IF;
    if ( strcmp ( lc, "bit" ) == 0 ) return FN_BIT;
    if ( strcmp ( lc, "s8" )  == 0 ) return FN_S8;
    if ( strcmp ( lc, "s16" ) == 0 ) return FN_S16;
    if ( strcmp ( lc, "s32" ) == 0 ) return FN_S32;
    return FN_INVALID;
}


/**
 * @brief Očekávané počty argumentů per funkce.
 */
static int parser_fn_arity ( fn_kind_t fn ) {
    switch ( fn ) {
        case FN_MIN: case FN_MAX: case FN_BIT: return 2;
        case FN_ABS: case FN_S8: case FN_S16: case FN_S32: return 1;
        case FN_IF: return 3;
        default: return -1;
    };
}


/* primary := INT | $var | IDENT | IDENT(args) | port[expr] | port[expr]!
 *          | [expr] | [expr]! | {expr} | (expr) */
static bp_node_t* parse_primary ( bp_parser_t *p ) {
    if ( p->cur.kind == TOK_INT ) {
        bp_node_t *n = bp_node_new ( NODE_INT );
        if ( !n ) { p->had_error = true; return NULL; };
        n->i.value = p->cur.value;
        parser_advance ( p );
        return n;
    };
    if ( p->cur.kind == TOK_VAR ) {
        bp_node_t *n = bp_node_new ( NODE_VAR );
        if ( !n ) { p->had_error = true; return NULL; };
        strncpy ( n->var.name, p->cur.text, BP_EXPR_IDENT_MAX );
        parser_advance ( p );
        return n;
    };
    if ( p->cur.kind == TOK_LPAREN ) {
        parser_advance ( p );
        bp_node_t *e = parse_or ( p );
        if ( !e || !parser_eat ( p, TOK_RPAREN, "')'" ) ) {
            bp_node_free ( e );
            return NULL;
        };
        return e;
    };
    if ( p->cur.kind == TOK_LBRACK ) {
        parser_advance ( p );
        bp_node_t *addr = parse_or ( p );
        if ( !addr || !parser_eat ( p, TOK_RBRACK, "']'" ) ) {
            bp_node_free ( addr );
            return NULL;
        };
        bool se = false;
        if ( p->cur.kind == TOK_BANG ) {
            se = true;
            parser_advance ( p );
        };
        bp_node_t *n = bp_node_new ( NODE_MEM );
        if ( !n ) { bp_node_free ( addr ); p->had_error = true; return NULL; };
        n->mem.addr = addr;
        n->mem.side_effect = se;
        return n;
    };
    if ( p->cur.kind == TOK_LBRACE ) {
        parser_advance ( p );
        bp_node_t *addr = parse_or ( p );
        if ( !addr || !parser_eat ( p, TOK_RBRACE, "'}'" ) ) {
            bp_node_free ( addr );
            return NULL;
        };
        bp_node_t *n = bp_node_new ( NODE_MEM_W );
        if ( !n ) { bp_node_free ( addr ); p->had_error = true; return NULL; };
        n->mem.addr = addr;
        n->mem.side_effect = false;
        return n;
    };
    if ( p->cur.kind == TOK_IDENT ) {
        char name[BP_EXPR_IDENT_MAX + 1];
        strncpy ( name, p->cur.text, BP_EXPR_IDENT_MAX );
        name[BP_EXPR_IDENT_MAX] = '\0';
        parser_advance ( p );

        /* port[N] / port[N]! - speciální keyword */
        bool is_port_kw = ( strcasecmp ( name, "port" ) == 0 );

        if ( is_port_kw && p->cur.kind == TOK_LBRACK ) {
            parser_advance ( p );
            bp_node_t *pn = parse_or ( p );
            if ( !pn || !parser_eat ( p, TOK_RBRACK, "']'" ) ) {
                bp_node_free ( pn );
                return NULL;
            };
            bool se = false;
            if ( p->cur.kind == TOK_BANG ) {
                se = true;
                parser_advance ( p );
            };
            bp_node_t *n = bp_node_new ( NODE_PORT );
            if ( !n ) { bp_node_free ( pn ); p->had_error = true; return NULL; };
            n->port.port = pn;
            n->port.side_effect = se;
            return n;
        };

        /* Funkční volání? */
        if ( p->cur.kind == TOK_LPAREN ) {
            fn_kind_t fn = parser_lookup_fn ( name );
            if ( fn == FN_INVALID ) {
                bp_err ( &p->lex, "unknown function '%s'", name );
                p->had_error = true;
                return NULL;
            };
            int expected = parser_fn_arity ( fn );
            parser_advance ( p );  /* '(' */

            bp_node_t *n = bp_node_new ( NODE_FNCALL );
            if ( !n ) { p->had_error = true; return NULL; };
            n->fn.fn = fn;
            n->fn.n_args = 0;

            if ( p->cur.kind != TOK_RPAREN ) {
                while ( 1 ) {
                    if ( n->fn.n_args >= BP_EXPR_FN_MAX_ARGS ) {
                        bp_err ( &p->lex, "too many arguments to '%s'", name );
                        p->had_error = true;
                        bp_node_free ( n );
                        return NULL;
                    };
                    bp_node_t *a = parse_or ( p );
                    if ( !a ) { bp_node_free ( n ); return NULL; };
                    n->fn.args[n->fn.n_args++] = a;
                    if ( p->cur.kind == TOK_COMMA ) {
                        parser_advance ( p );
                        continue;
                    };
                    break;
                };
            };
            if ( !parser_eat ( p, TOK_RPAREN, "')'" ) ) {
                bp_node_free ( n );
                return NULL;
            };
            if ( n->fn.n_args != expected ) {
                bp_err ( &p->lex, "function '%s' expects %d args (got %d)",
                         name, expected, n->fn.n_args );
                p->had_error = true;
                bp_node_free ( n );
                return NULL;
            };
            return n;
        };

        /* Plain identifier */
        bp_node_t *n = bp_node_new ( NODE_IDENT );
        if ( !n ) { p->had_error = true; return NULL; };
        strncpy ( n->id.name, name, BP_EXPR_IDENT_MAX );
        return n;
    };

    bp_err ( &p->lex, "unexpected token in primary expression" );
    p->had_error = true;
    return NULL;
}


/* unary := ('+' | '-' | '!' | '~')* primary */
static bp_node_t* parse_unary ( bp_parser_t *p ) {
    if ( p->cur.kind == TOK_PLUS || p->cur.kind == TOK_MINUS ||
         p->cur.kind == TOK_BANG || p->cur.kind == TOK_TILDE ) {
        un_op_t op = UN_PLUS;
        switch ( p->cur.kind ) {
            case TOK_PLUS:  op = UN_PLUS;  break;
            case TOK_MINUS: op = UN_MINUS; break;
            case TOK_BANG:  op = UN_NOT;   break;
            case TOK_TILDE: op = UN_BNOT;  break;
            default: break;
        };
        parser_advance ( p );
        bp_node_t *child = parse_unary ( p );
        if ( !child ) return NULL;
        bp_node_t *n = bp_node_new ( NODE_UNARY );
        if ( !n ) { bp_node_free ( child ); p->had_error = true; return NULL; };
        n->un.op = op;
        n->un.child = child;
        return n;
    };
    return parse_primary ( p );
}


/* Pomůcka pro left-associative binary operators. */
static bp_node_t* parse_binop_left ( bp_parser_t *p,
                                      bp_node_t *(*next) ( bp_parser_t * ),
                                      const tok_kind_t *toks,
                                      const bin_op_t *ops,
                                      int n_ops ) {
    bp_node_t *l = next ( p );
    if ( !l ) return NULL;
    while ( 1 ) {
        int found = -1;
        for ( int i = 0; i < n_ops; i++ ) {
            if ( p->cur.kind == toks[i] ) { found = i; break; };
        };
        if ( found < 0 ) return l;
        bin_op_t op = ops[found];
        parser_advance ( p );
        bp_node_t *r = next ( p );
        if ( !r ) { bp_node_free ( l ); return NULL; };
        bp_node_t *n = bp_node_new ( NODE_BINOP );
        if ( !n ) { bp_node_free ( l ); bp_node_free ( r ); p->had_error = true; return NULL; };
        n->bin.op = op;
        n->bin.l = l;
        n->bin.r = r;
        l = n;
    };
}


static bp_node_t* parse_mul ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_STAR, TOK_SLASH, TOK_PERCENT };
    static const bin_op_t  ops[]  = { OP_MUL, OP_DIV, OP_MOD };
    return parse_binop_left ( p, parse_unary, toks, ops, 3 );
}


static bp_node_t* parse_add ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_PLUS, TOK_MINUS };
    static const bin_op_t  ops[]  = { OP_ADD, OP_SUB };
    return parse_binop_left ( p, parse_mul, toks, ops, 2 );
}


static bp_node_t* parse_shift ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_LSHIFT, TOK_RSHIFT };
    static const bin_op_t  ops[]  = { OP_SHL, OP_SHR };
    return parse_binop_left ( p, parse_add, toks, ops, 2 );
}


static bp_node_t* parse_rel ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_LT, TOK_LE, TOK_GT, TOK_GE };
    static const bin_op_t  ops[]  = { OP_LT, OP_LE, OP_GT, OP_GE };
    return parse_binop_left ( p, parse_shift, toks, ops, 4 );
}


static bp_node_t* parse_eq ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_EQ, TOK_NEQ };
    static const bin_op_t  ops[]  = { OP_EQ, OP_NEQ };
    return parse_binop_left ( p, parse_rel, toks, ops, 2 );
}


static bp_node_t* parse_band ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_AMP };
    static const bin_op_t  ops[]  = { OP_AND };
    return parse_binop_left ( p, parse_eq, toks, ops, 1 );
}


static bp_node_t* parse_bxor ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_CARET };
    static const bin_op_t  ops[]  = { OP_XOR };
    return parse_binop_left ( p, parse_band, toks, ops, 1 );
}


static bp_node_t* parse_bor ( bp_parser_t *p ) {
    static const tok_kind_t toks[] = { TOK_PIPE };
    static const bin_op_t  ops[]  = { OP_OR };
    return parse_binop_left ( p, parse_bxor, toks, ops, 1 );
}


static bp_node_t* parse_land ( bp_parser_t *p ) {
    bp_node_t *l = parse_bor ( p );
    if ( !l ) return NULL;
    while ( p->cur.kind == TOK_AMPAMP ) {
        parser_advance ( p );
        bp_node_t *r = parse_bor ( p );
        if ( !r ) { bp_node_free ( l ); return NULL; };
        bp_node_t *n = bp_node_new ( NODE_LOGAND );
        if ( !n ) { bp_node_free ( l ); bp_node_free ( r ); p->had_error = true; return NULL; };
        n->logical.l = l;
        n->logical.r = r;
        l = n;
    };
    return l;
}


static bp_node_t* parse_or ( bp_parser_t *p ) {
    bp_node_t *l = parse_land ( p );
    if ( !l ) return NULL;
    while ( p->cur.kind == TOK_PIPEPIPE ) {
        parser_advance ( p );
        bp_node_t *r = parse_land ( p );
        if ( !r ) { bp_node_free ( l ); return NULL; };
        bp_node_t *n = bp_node_new ( NODE_LOGOR );
        if ( !n ) { bp_node_free ( l ); bp_node_free ( r ); p->had_error = true; return NULL; };
        n->logical.l = l;
        n->logical.r = r;
        l = n;
    };
    return l;
}


/* ========================================================================= */
/*  Identifier resolution                                                    */
/* ========================================================================= */

/**
 * @brief Resolve identifier na hodnotu.
 *
 * Priorita: kontextová pole BP > Z80 register > flag > globální emu state.
 * Neznámé jméno → 0.
 *
 * Identifier matching je case-insensitive (kromě shadow apostrofů).
 */
static int32_t resolve_ident ( const char *raw_name, const bp_expr_ctx_t *ctx ) {
    if ( !raw_name || !*raw_name ) return 0;

    char name[BP_EXPR_IDENT_MAX + 1];
    size_t i;
    for ( i = 0; raw_name[i] && i < BP_EXPR_IDENT_MAX; i++ ) {
        name[i] = (char) tolower ( (unsigned char) raw_name[i] );
    };
    name[i] = '\0';

    /* Kontextová BP hit data - v UI typicky CamelCase ("Address", "Value", ...).
     * Lookup case-insensitive po lowercase. */
    if ( strcmp ( name, "address" )  == 0 ) return ctx->Address;
    if ( strcmp ( name, "value" )    == 0 ) return ctx->Value;
    if ( strcmp ( name, "isread" )   == 0 ) return ctx->IsRead  ? 1 : 0;
    if ( strcmp ( name, "iswrite" )  == 0 ) return ctx->IsWrite ? 1 : 0;
    if ( strcmp ( name, "isexec" )   == 0 ) return ctx->IsExec  ? 1 : 0;
    if ( strcmp ( name, "isport" )   == 0 ) return ctx->IsPort  ? 1 : 0;
    if ( strcmp ( name, "bankpc" )   == 0 ) return ctx->BankPC;
    if ( strcmp ( name, "bankaddr" ) == 0 ) return ctx->BankAddr;
    if ( strcmp ( name, "cycle" )    == 0 ) return (int32_t) ctx->Cycle;
    if ( strcmp ( name, "frame" )    == 0 ) return (int32_t) ctx->Frame;
    if ( strcmp ( name, "scanline" ) == 0 ) return ctx->Scanline;

    /* Reason - ambient global pro IFF*_CHANGE eventy (V1.7+ 1.5).
     * Hodnota platí jen v rámci aktivního bp_event_fire() stacku (mzarch
     * IFF callback nastaví těsně před fire a reset po). Mimo to čte
     * BP_REASON_NONE (= 0xFF). DSL: `Reason == nmi_ack` apod. */
    if ( strcmp ( name, "reason" )   == 0 ) return (int32_t) g_bp_fire_reason;
    /* Symbolic constants pro porovnání. Hodnoty matchují en_BP_FIRE_REASON
     * (= odpovídají z80_iff_change_reason_t v cpu-z80/z80.h). */
    if ( strcmp ( name, "reset" )    == 0 ) return (int32_t) BP_REASON_IFF_RESET;
    if ( strcmp ( name, "ei" )       == 0 ) return (int32_t) BP_REASON_IFF_EI;
    if ( strcmp ( name, "di" )       == 0 ) return (int32_t) BP_REASON_IFF_DI;
    if ( strcmp ( name, "int_ack" )  == 0 ) return (int32_t) BP_REASON_IFF_INT_ACK;
    if ( strcmp ( name, "nmi_ack" )  == 0 ) return (int32_t) BP_REASON_IFF_NMI_ACK;
    if ( strcmp ( name, "reti" )     == 0 ) return (int32_t) BP_REASON_IFF_RETI;
    if ( strcmp ( name, "retn" )     == 0 ) return (int32_t) BP_REASON_IFF_RETN;
    if ( strcmp ( name, "none" )     == 0 ) return (int32_t) BP_REASON_NONE;

    /* Z80 register / flag */
    if ( ctx->cpu ) {
        z80_t *cpu = ctx->cpu;

        /* 8-bit a 16-bit registry */
        if ( strcmp ( name, "a" ) == 0 )  return cpu->af.h;
        if ( strcmp ( name, "f" ) == 0 )  return cpu->af.l;
        if ( strcmp ( name, "b" ) == 0 )  return cpu->bc.h;
        if ( strcmp ( name, "c" ) == 0 )  return cpu->bc.l;
        if ( strcmp ( name, "d" ) == 0 )  return cpu->de.h;
        if ( strcmp ( name, "e" ) == 0 )  return cpu->de.l;
        if ( strcmp ( name, "h" ) == 0 )  return cpu->hl.h;
        if ( strcmp ( name, "l" ) == 0 )  return cpu->hl.l;
        if ( strcmp ( name, "af" ) == 0 ) return cpu->af.w;
        if ( strcmp ( name, "bc" ) == 0 ) return cpu->bc.w;
        if ( strcmp ( name, "de" ) == 0 ) return cpu->de.w;
        if ( strcmp ( name, "hl" ) == 0 ) return cpu->hl.w;
        if ( strcmp ( name, "ix" ) == 0 ) return cpu->ix.w;
        if ( strcmp ( name, "iy" ) == 0 ) return cpu->iy.w;
        /* 8-bit půlené index registry (high/low byte IX/IY). */
        if ( strcmp ( name, "ixh" ) == 0 ) return cpu->ix.h;
        if ( strcmp ( name, "ixl" ) == 0 ) return cpu->ix.l;
        if ( strcmp ( name, "iyh" ) == 0 ) return cpu->iy.h;
        if ( strcmp ( name, "iyl" ) == 0 ) return cpu->iy.l;
        if ( strcmp ( name, "sp" ) == 0 ) return cpu->sp;
        if ( strcmp ( name, "pc" ) == 0 ) return cpu->pc;
        if ( strcmp ( name, "i" )  == 0 ) return cpu->i;
        if ( strcmp ( name, "r" )  == 0 ) return cpu->r;
        if ( strcmp ( name, "im" ) == 0 ) return cpu->im;
        if ( strcmp ( name, "iff1" ) == 0 ) return cpu->iff1;
        if ( strcmp ( name, "iff2" ) == 0 ) return cpu->iff2;

        /* Shadow registry - apostrof zachovaný v lowercase */
        if ( strcmp ( name, "af'" ) == 0 ) return cpu->af2.w;
        if ( strcmp ( name, "bc'" ) == 0 ) return cpu->bc2.w;
        if ( strcmp ( name, "de'" ) == 0 ) return cpu->de2.w;
        if ( strcmp ( name, "hl'" ) == 0 ) return cpu->hl2.w;

        /* Z80 flagy (Cf, Zf, Sf, Pf, Hf, Nf - postfix 'f' aby
         * nekolidovaly s registry C, F. Bity dle Z80 standardu:
         * S=7, Z=6, H=4, P/V=2, N=1, C=0. */
        uint8_t f = cpu->af.l;
        if ( strcmp ( name, "cf" ) == 0 ) return ( f >> 0 ) & 1;
        if ( strcmp ( name, "nf" ) == 0 ) return ( f >> 1 ) & 1;
        if ( strcmp ( name, "pf" ) == 0 ) return ( f >> 2 ) & 1;
        if ( strcmp ( name, "hf" ) == 0 ) return ( f >> 4 ) & 1;
        if ( strcmp ( name, "zf" ) == 0 ) return ( f >> 6 ) & 1;
        if ( strcmp ( name, "sf" ) == 0 ) return ( f >> 7 ) & 1;
    };

    /* Neznámé jméno = 0 (viz docstring v bp_expr.h - eval je tolerant). */
    return 0;
}


/* ========================================================================= */
/*  Memory / port access                                                     */
/* ========================================================================= */

/**
 * @brief Čtení byte z paměti.
 *
 * V1.6+ TODO 4.2 5a: side_effect=false → memory_read_byte_no_se()
 * (= bez MEM_R BP fire); side_effect=true → memory_read_byte()
 * (= s plným side-effecty vč. MEM_R BP fire). MZ-800 memory subsystem
 * jine side-effecty mimo BP fire nema (= tape state v memory_read_byte
 * neni triggered, je v separate cmt_update_output v iorq path).
 */
static uint8_t bp_read_byte ( uint16_t addr, bool side_effect ) {
    if ( side_effect ) {
        return memory_read_byte ( addr );
    }
    return memory_read_byte_no_se ( addr );
}


/**
 * @brief Čtení word LE z paměti.
 */
static uint16_t bp_read_word ( uint16_t addr, bool side_effect ) {
    uint8_t lo = bp_read_byte ( addr, side_effect );
    uint8_t hi = bp_read_byte ( (uint16_t) ( addr + 1 ), side_effect );
    return (uint16_t) ( lo | ( (uint16_t) hi << 8 ) );
}


/**
 * @brief Čtení byte z I/O portu.
 *
 * V1.6+ TODO 4.2 5b ✅: castecna implementace per-chip probe.
 *
 *  - `port[N]` (side_effect=false): probe pres `port_read_no_se_cb`.
 *    Joystick (MZ-800 jen) vraci real read = cista funkce.
 *    Side-effect-heavy chipy (PIO/CTC/GDG/FDC/PIO Z80) vraci
 *    bus latch (`regDBUS_latch`) jako safer fallback.
 *  - `port[N]!` (side_effect=true): real `port_read_cb` = stejne
 *    jako CPU IN instr. Volat jen explicit user request.
 *
 * Per-chip plne no_se refactor je odlozen V1.7+ (= per-chip dispatcher
 * rozdvojeni mezi probe a real path).
 */
static uint8_t bp_read_port ( uint16_t port, bool side_effect ) {
    if ( side_effect ) {
        /* Real read - cesta jako pri CPU IN instr. CPU pointer je NULL
         * (= pripustny pro port_read_cb na MZ-800 i 1500, dispatcher
         * cpu argument neuziva). */
        return port_read_cb ( NULL, port, NULL );
    }
    /* Side-effect-free probe. */
    return port_read_no_se_cb ( port );
}


/* ========================================================================= */
/*  Evaluator                                                                */
/* ========================================================================= */

static int32_t eval_node ( const bp_node_t *n, const bp_expr_ctx_t *ctx );


static int32_t eval_unary ( const bp_node_t *n, const bp_expr_ctx_t *ctx ) {
    int32_t v = eval_node ( n->un.child, ctx );
    switch ( n->un.op ) {
        case UN_PLUS:  return v;
        case UN_MINUS: return -v;
        case UN_NOT:   return v ? 0 : 1;
        case UN_BNOT:  return ~v;
    };
    return 0;
}


static int32_t eval_binop ( const bp_node_t *n, const bp_expr_ctx_t *ctx ) {
    int32_t l = eval_node ( n->bin.l, ctx );
    int32_t r = eval_node ( n->bin.r, ctx );
    switch ( n->bin.op ) {
        case OP_MUL: return l * r;
        case OP_DIV: return ( r == 0 ) ? 0 : l / r;  /* div by zero → 0 (tolerant) */
        case OP_MOD: return ( r == 0 ) ? 0 : l % r;
        case OP_ADD: return l + r;
        case OP_SUB: return l - r;
        case OP_SHL: return (int32_t) ( (uint32_t) l << ( r & 31 ) );
        case OP_SHR: return (int32_t) ( (uint32_t) l >> ( r & 31 ) );
        case OP_LT:  return l <  r ? 1 : 0;
        case OP_LE:  return l <= r ? 1 : 0;
        case OP_GT:  return l >  r ? 1 : 0;
        case OP_GE:  return l >= r ? 1 : 0;
        case OP_EQ:  return l == r ? 1 : 0;
        case OP_NEQ: return l != r ? 1 : 0;
        case OP_AND: return l & r;
        case OP_XOR: return l ^ r;
        case OP_OR:  return l | r;
    };
    return 0;
}


static int32_t eval_fncall ( const bp_node_t *n, const bp_expr_ctx_t *ctx ) {
    /* Eval všech argumentů eagerly (kromě FN_IF, kde děláme short-circuit). */
    if ( n->fn.fn == FN_IF ) {
        /* if(cond, a, b) - eval cond, pak jen vybranou větev. */
        int32_t cond = eval_node ( n->fn.args[0], ctx );
        if ( cond ) return eval_node ( n->fn.args[1], ctx );
        return eval_node ( n->fn.args[2], ctx );
    };

    int32_t a[BP_EXPR_FN_MAX_ARGS] = {0};
    for ( int i = 0; i < n->fn.n_args; i++ ) {
        a[i] = eval_node ( n->fn.args[i], ctx );
    };

    switch ( n->fn.fn ) {
        case FN_MIN: return a[0] < a[1] ? a[0] : a[1];
        case FN_MAX: return a[0] > a[1] ? a[0] : a[1];
        case FN_ABS: return a[0] < 0 ? -a[0] : a[0];
        case FN_BIT: {
            int n_bit = a[1] & 31;
            return ( (uint32_t) a[0] >> n_bit ) & 1;
        };
        case FN_S8:  return (int32_t) (int8_t)  ( a[0] & 0xFF );
        case FN_S16: return (int32_t) (int16_t) ( a[0] & 0xFFFF );
        case FN_S32: return a[0];  /* už je 32b signed */
        case FN_IF:  /* handled above */
        case FN_INVALID: break;
    };
    return 0;
}


static int32_t eval_node ( const bp_node_t *n, const bp_expr_ctx_t *ctx ) {
    if ( !n ) return 0;
    switch ( n->kind ) {
        case NODE_INT:    return n->i.value;
        case NODE_IDENT:  return resolve_ident ( n->id.name, ctx );
        case NODE_VAR:
            /* D.1.3: $vars storage propojený s bp_vars modulem.
             * Neexistující var = 0 (default). */
            return bp_var_get ( n->var.name );
        case NODE_UNARY:  return eval_unary ( n, ctx );
        case NODE_BINOP:  return eval_binop ( n, ctx );
        case NODE_LOGAND: {
            int32_t l = eval_node ( n->logical.l, ctx );
            if ( !l ) return 0;  /* short-circuit */
            int32_t r = eval_node ( n->logical.r, ctx );
            return r ? 1 : 0;
        };
        case NODE_LOGOR: {
            int32_t l = eval_node ( n->logical.l, ctx );
            if ( l ) return 1;   /* short-circuit */
            int32_t r = eval_node ( n->logical.r, ctx );
            return r ? 1 : 0;
        };
        case NODE_MEM: {
            int32_t addr = eval_node ( n->mem.addr, ctx );
            return bp_read_byte ( (uint16_t) addr, n->mem.side_effect );
        };
        case NODE_MEM_W: {
            int32_t addr = eval_node ( n->mem.addr, ctx );
            return bp_read_word ( (uint16_t) addr, n->mem.side_effect );
        };
        case NODE_PORT: {
            int32_t port = eval_node ( n->port.port, ctx );
            return bp_read_port ( (uint16_t) port, n->port.side_effect );
        };
        case NODE_FNCALL: return eval_fncall ( n, ctx );
    };
    return 0;
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void bp_expr_ctx_zero ( bp_expr_ctx_t *ctx ) {
    if ( !ctx ) return;
    memset ( ctx, 0, sizeof ( *ctx ) );
    /* self_id default -1 (= ctx vznikl mimo BP enforcement, např. v testu). */
    ctx->self_id = -1;
}


bp_expr_t* bp_expr_parse ( const char *source, char *errbuf, size_t errbuf_size ) {
    if ( errbuf && errbuf_size > 0 ) errbuf[0] = '\0';
    if ( !source || !*source ) {
        if ( errbuf && errbuf_size > 0 ) {
            snprintf ( errbuf, errbuf_size, "empty expression" );
        };
        return NULL;
    };

    bp_parser_t p;
    memset ( &p, 0, sizeof ( p ) );
    p.lex.src = source;
    p.lex.pos = 0;
    p.lex.len = strlen ( source );
    p.lex.prev_was_value = false;
    p.lex.errbuf = errbuf;
    p.lex.errbuf_size = errbuf_size;

    parser_advance ( &p );  /* prime cur */
    if ( p.had_error ) return NULL;

    bp_node_t *root = parse_or ( &p );
    if ( !root || p.had_error ) {
        bp_node_free ( root );
        return NULL;
    };

    if ( p.cur.kind != TOK_END ) {
        bp_err ( &p.lex, "trailing tokens after expression" );
        bp_node_free ( root );
        return NULL;
    };

    bp_expr_t *e = (bp_expr_t *) calloc ( 1, sizeof ( bp_expr_t ) );
    if ( !e ) {
        bp_node_free ( root );
        if ( errbuf && errbuf_size > 0 ) snprintf ( errbuf, errbuf_size, "out of memory" );
        return NULL;
    };
    e->root = root;
    return e;
}


int32_t bp_expr_eval ( const bp_expr_t *expr, const bp_expr_ctx_t *ctx ) {
    if ( !expr || !ctx ) return 0;
    return eval_node ( expr->root, ctx );
}


void bp_expr_free ( bp_expr_t *expr ) {
    if ( !expr ) return;
    bp_node_free ( expr->root );
    free ( expr );
}


bool bp_expr_parse_eval ( const char *source,
                           const bp_expr_ctx_t *ctx,
                           int32_t *out,
                           char *errbuf, size_t errbuf_size ) {
    if ( out ) *out = 0;
    bp_expr_t *e = bp_expr_parse ( source, errbuf, errbuf_size );
    if ( !e ) return false;
    int32_t v = bp_expr_eval ( e, ctx );
    if ( out ) *out = v;
    bp_expr_free ( e );
    return true;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

