/**
 * @file   eventlog_filter.c
 * @brief  Filter parser + evaluátor pro Event Viewer.
 *
 * Implementace AST + arena bump allocator. Pattern shodný s
 * @c io_history_filter.c (= bump arena, opaque struct, recursive
 * descent), ale samostatná implementace - žádné sdílení kódu.
 *
 * Datový model:
 *  - LEAF / NOT / AND / OR uzly v bump aréně (4 KB default).
 *  - Per-LEAF typ vlastní payload (mask pro cat/sub, range pro
 *    pc/frame/cycle/sline/px, exact value pro payload).
 *  - NOT collapse-uje do LEAF s @c negate flagem pro per-token "!".
 *
 * Licence: GPLv3
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "eventlog_filter.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <glib.h>

#include "symbols/sym_db.h"


/* ========================================================================= */
/*  Constants                                                                */
/* ========================================================================= */


/**
 * @brief Velikost arény pro AST uzly (= jeden malloc per filter).
 *
 * 4 KB pojme cca 50 leaf uzlů (~sizeof leaf 60 B) nebo víc binárních uzlů
 * (~24 B). Filter buffer 256 B fyzicky nepustí víc než ~30 tokenů.
 */
#define EVFILT_ARENA_CAP   4096u


/**
 * @brief Hard limit počtu AST uzlů na jeden filter.
 *
 * Pojistka proti patologickým vstupům (hluboce vnořené závorky,
 * dlouhé řetězy AND). Při překročení parser hlásí "Expression too
 * complex".
 */
#define EVFILT_MAX_NODES   64u


/**
 * @brief Velikost error message bufferu uvnitř filter struktury.
 */
#define EVFILT_ERR_LEN     96u


/**
 * @brief Globální šíře screenu pro sline/px dekódování.
 *
 * Defaults na 320 (= MZ-800 H40 / MZ-700). UI volá
 * @ref eventlog_filter_set_screen_width() při změně módu.
 */
static uint32_t g_evfilt_screen_width = 320u;


void eventlog_filter_set_screen_width ( uint32_t width )
{
    if ( width == 0 ) return;
    g_evfilt_screen_width = width;
}


/* ========================================================================= */
/*  AST node types                                                           */
/* ========================================================================= */


/**
 * @brief Druh AST uzlu.
 */
typedef enum
{
    EVFILT_NODE_LEAF = 0,  /**< Atomická podmínka per-prefix typ. */
    EVFILT_NODE_NOT  = 1,  /**< Unární negace nad podstromem. */
    EVFILT_NODE_AND  = 2,  /**< Binární AND, short-circuit. */
    EVFILT_NODE_OR   = 3,  /**< Binární OR, short-circuit. */
    EVFILT_NODE_TEMPORAL_BEFORE = 4, /**< @c before(N) sub - eventy v okně N pxclk před match. */
    EVFILT_NODE_TEMPORAL_AFTER  = 5, /**< @c after(M) sub - eventy v okně M pxclk po match. */
    EVFILT_NODE_TEMPORAL_NEAR   = 6  /**< @c near(K) sub - eventy v okně +-K pxclk kolem match. */
} en_EVFILT_NODE_KIND;


/**
 * @brief Maximální hloubka vnoření temporal node-u (Vlna 4 Commit 26).
 *
 * @c before(...) @c before(...) @c X je platná dvouúrovňová struktura.
 * Hlubší vnoření (3+) hlásí parser jako "temporal nesting too deep".
 * Důvod: per-event match temporal je O(N_ring); vnoření znásobí náklady
 * exponenciálně (O(N^depth)) - hard limit chrání UI před zaseknutím.
 */
#define EVFILT_MAX_TEMPORAL_DEPTH 2u


/**
 * @brief Druh LEAF payloadu (per prefix syntax).
 */
typedef enum
{
    EVFILT_LEAF_CAT_MASK   = 0,  /**< @c cat:NAME[,NAME]* - 64bit bitmask. */
    EVFILT_LEAF_SUB_MASK   = 1,  /**< @c sub:N[,N]* - 256bit (4x uint64) mask. */
    EVFILT_LEAF_PC_RANGE   = 2,  /**< @c pc:HEX nebo @c pc:HEX-HEX. */
    EVFILT_LEAF_FRAME_OP   = 3,  /**< @c frame:N / @c frame:>N / @c frame:<N. */
    EVFILT_LEAF_CYCLE_OP   = 4,  /**< @c cycle:N / @c cycle:>N / @c cycle:<N (s k/M). */
    EVFILT_LEAF_SLINE_RANGE = 5, /**< @c sline:N nebo @c sline:N-N. */
    EVFILT_LEAF_PX_RANGE   = 6,  /**< @c px:N nebo @c px:N-N. */
    EVFILT_LEAF_PAYLOAD_EQ = 7,  /**< @c payload:HEX - exact uint32 match. */
    EVFILT_LEAF_SYM_EXACT  = 8,  /**< @c sym:NAME - PC == addr symbolu NAME. */
    EVFILT_LEAF_SYM_PREFIX = 9,  /**< @c sym:PREFIX_* - PC == addr libovolného symbolu s prefixem. */
    EVFILT_LEAF_SYM_RANGE  = 10, /**< @c from_sym:A @c to_sym:B - PC v [A.addr, B.addr]. */
    EVFILT_LEAF_AMBIENT_BIT   = 11, /**< @c if iff1:N - jednobitový ambient match (mask = 1<<bit). */
    EVFILT_LEAF_AMBIENT_FIELD = 12  /**< @c if im/reason/banking:N - více-bitové pole ambient. */
} en_EVFILT_LEAF_KIND;


/**
 * @brief Sub-druh AMBIENT_* leaf - pro round-trip zpět na text.
 *
 * AST nezná, ze kterého fieldu (iff1/im/reason/banking) byl leaf
 * postaven - mask + expected ho jednoznačně určí jen z hlediska eval.
 * Pro @c eventlog_filter_to_string() je třeba zachovat původní field,
 * proto AMBIENT_BIT/AMBIENT_FIELD ukládají i tento tag.
 */
typedef enum
{
    EVFILT_AMBIENT_FIELD_IFF1    = 0, /**< @c if iff1:N (1 bit). */
    EVFILT_AMBIENT_FIELD_IM      = 1, /**< @c if im:N (2 bity). */
    EVFILT_AMBIENT_FIELD_REASON  = 2, /**< @c if reason:NAME (3 bity). */
    EVFILT_AMBIENT_FIELD_BANKING = 3  /**< @c if banking:NAME (3 bity). */
} en_EVFILT_AMBIENT_FIELD;


/**
 * @brief LEAF payload - tagged union dle @ref en_EVFILT_LEAF_KIND.
 *
 * Pole jsou sjednocena pro různé typy. Význam:
 *
 *  - @c CAT_MASK:  @c mask64 = bit i nastaven => kategorie i match.
 *  - @c SUB_MASK:  @c sub_mask[0..3] = 4x uint64 bitmask pro
 *                  subtype 0..255 (= subtype/64 -> word, subtype%64 -> bit).
 *  - @c *_RANGE / @c *_OP:  @c rmin..rmax inclusive (single value = min==max).
 *  - @c PAYLOAD_EQ: @c rmin = exact uint32 hodnota.
 *
 * @field kind     @ref en_EVFILT_LEAF_KIND.
 * @field negate   @c true pokud byl token uvozen @c '!' (= invertuj
 *                 raw match).
 * @field mask64   Bitmask pro @c CAT_MASK (bit per kategorie).
 * @field sub_mask 4x uint64 mask pro @c SUB_MASK (bit per subtype).
 * @field rmin     Spodní mez range / exact hodnota.
 * @field rmax     Horní mez range (= @c rmin při single match).
 * @field sym_name_a  @c SYM_*: jméno A (exact name, prefix bez @c '*',
 *                    nebo @c from_sym name). Heap-allocated (malloc),
 *                    free při AST destroy. NULL pro non-SYM leaves.
 * @field sym_name_b  @c SYM_RANGE: jméno B (= @c to_sym). Heap, free
 *                    při AST destroy. NULL jinak.
 * @field sym_cached_addrs  @c SYM_PREFIX: pre-resolved seznam matching
 *                          16-bit adres z @c sym_db v čase parse. Heap
 *                          pole (malloc). NULL pokud žádný symbol
 *                          neodpovídá prefixu (= match vždy false).
 * @field sym_cached_count  Počet položek v @c sym_cached_addrs.
 * @field sym_cached_addr_a @c SYM_EXACT / @c SYM_RANGE: pre-resolved
 *                          16-bit adresa @c sym_name_a v čase parse
 *                          (= O(1) per-event lookup místo O(N) v match).
 *                          Hodnota platná jen když @c sym_cached_valid_a
 *                          == @c true.
 * @field sym_cached_addr_b @c SYM_RANGE: pre-resolved 16-bit adresa
 *                          @c sym_name_b. Hodnota platná jen když
 *                          @c sym_cached_valid_b == @c true.
 * @field sym_cached_valid_a @c true pokud byl @c sym_name_a úspěšně
 *                          resolveř při parse (= symbol existoval v
 *                          @c sym_db). @c false = match vrací vždy
 *                          false (= žádný event nematchne).
 * @field sym_cached_valid_b @c true pokud byl @c sym_name_b úspěšně
 *                          resolveř při parse. Relevantní jen pro
 *                          @c SYM_RANGE.
 *
 * @note Symbol cache je vázána na okamžik parse a stává se stale
 *       pokud uživatel po parse modifikuje @c sym_db (= load .lbl,
 *       add user label). Akceptovatelné pro Vlna 3 MVP; UI je řešeno
 *       re-parse při edit textboxu.
 *
 * @note Commit 19 přidává pre-resolve i pro @c SYM_EXACT a @c SYM_RANGE
 *       (předtím jen @c SYM_PREFIX). Důvod: pause-on-match callback běží
 *       v hot-path a sym_db_lookup_by_name() je O(N) lineární scan po
 *       jméně - per-event lookup by zničil emu výkon při zapnutém
 *       triggeru.
 *
 * @note Vlna 4 Commit 25 přidává pole @c amb_mask, @c amb_expected
 *       a @c amb_field pro @c AMBIENT_BIT / @c AMBIENT_FIELD leafy.
 *       Match testuje @c (e->ambient & amb_mask) ==/!= amb_expected
 *       (BIT i FIELD používají stejný vzorec, BIT navíc speciálně
 *       vyhodnocuje value=1 jako "ne-nula").
 */
typedef struct
{
    uint8_t  kind;
    bool     negate;
    uint64_t mask64;
    uint64_t sub_mask[4];
    uint64_t rmin;
    uint64_t rmax;
    char    *sym_name_a;
    char    *sym_name_b;
    uint16_t *sym_cached_addrs;
    size_t   sym_cached_count;
    uint16_t sym_cached_addr_a;
    uint16_t sym_cached_addr_b;
    bool     sym_cached_valid_a;
    bool     sym_cached_valid_b;
    uint16_t amb_mask;     /**< AMBIENT_*: AND maska v poli @c ambient. */
    uint16_t amb_expected; /**< AMBIENT_*: očekávaná hodnota po AND. */
    uint8_t  amb_field;    /**< AMBIENT_*: @ref en_EVFILT_AMBIENT_FIELD pro round-trip. */
} st_EVFILT_LEAF;


/**
 * @brief Jeden AST uzel v aréně.
 *
 * @field kind   @ref en_EVFILT_NODE_KIND.
 * @field u      Union dle @c kind:
 *               - leaf: payload pro LEAF
 *               - child: pointer na potomka pro NOT
 *               - bin: pointery na potomky pro AND/OR
 *               - temporal: window + reference sub-tree pro
 *                 TEMPORAL_BEFORE/AFTER/NEAR (Vlna 4 Commit 26)
 */
typedef struct st_EVFILT_NODE
{
    uint8_t kind;
    union {
        st_EVFILT_LEAF leaf;
        struct st_EVFILT_NODE *child;
        struct { struct st_EVFILT_NODE *l, *r; } bin;
        struct {
            uint64_t window;  /**< N/M/K v pxclk (= jednotka pxclk_total). */
            struct st_EVFILT_NODE *reference; /**< sub-tree definující reference events. */
        } temporal;
    } u;
} st_EVFILT_NODE;


/**
 * @brief Bump arena pro AST.
 *
 * @field buf         Surový buffer.
 * @field used        Bytes already allocated.
 * @field cap         Total capacity.
 * @field node_count  Počet uzlů (limit @ref EVFILT_MAX_NODES).
 */
typedef struct
{
    uint8_t *buf;
    size_t   used;
    size_t   cap;
    size_t   node_count;
} st_EVFILT_ARENA;


/**
 * @brief Veřejná opaque struktura.
 *
 * @field root         Kořen AST. @c NULL = match all (po prázdném parse)
 *                     nebo neplatný (= drží error message).
 * @field arena        Bump arena pro uzly.
 * @field err          Error string z parsování (@c "" = no error).
 * @field has_error    @c true pokud parse selhal (= match vrací false).
 * @field sym_leaves   Lineární seznam pointerů na všechny SYM leaf-y
 *                     (heap, kapacita @c sym_leaves_cap, počet
 *                     @c sym_leaves_count). Slouží pro deterministické
 *                     free heap allocs uvnitř leaf-ů i při parse error
 *                     path (= když @c root zůstane NULL, leaf-y v aréně
 *                     by jinak unikly).
 */
struct st_EVENTLOG_FILTER
{
    st_EVFILT_NODE  *root;
    st_EVFILT_ARENA  arena;
    char             err[ EVFILT_ERR_LEN ];
    bool             has_error;
    st_EVFILT_LEAF **sym_leaves;
    size_t           sym_leaves_count;
    size_t           sym_leaves_cap;
};


/**
 * @brief Zaregistruje LEAF do seznamu sym_leaves pro pozdější free.
 *
 * Volá se z @c leaf_fill_sym / @c leaf_fill_sym_range PO úspěšné
 * heap alokaci jmen. Realloc selhání = leak heap alokací leaf-u
 * (= akceptovatelný OOM degradation, parse stejně selže).
 */
static void track_sym_leaf ( st_EVENTLOG_FILTER *f, st_EVFILT_LEAF *leaf )
{
    if ( !f || !leaf ) return;
    if ( f->sym_leaves_count >= f->sym_leaves_cap ) {
        size_t ncap = f->sym_leaves_cap ? f->sym_leaves_cap * 2 : 4;
        st_EVFILT_LEAF **nb = (st_EVFILT_LEAF **)
            realloc ( f->sym_leaves, ncap * sizeof ( *nb ) );
        if ( !nb ) return;
        f->sym_leaves = nb;
        f->sym_leaves_cap = ncap;
    }
    f->sym_leaves[ f->sym_leaves_count++ ] = leaf;
}


/* ========================================================================= */
/*  Helper: error reporting                                                  */
/* ========================================================================= */


/**
 * @brief Zapíše error message do filter struktury a označí has_error.
 */
static void set_err ( st_EVENTLOG_FILTER *f, const char *msg )
{
    if ( !f ) return;
    snprintf ( f->err, sizeof ( f->err ), "%s", msg );
    f->has_error = true;
}


/* ========================================================================= */
/*  Helper: arena allocator                                                  */
/* ========================================================================= */


/**
 * @brief Alokuj @c sz bytů z arény, 8-byte aligned. Vrátí @c NULL při OOM.
 */
static void *arena_alloc ( st_EVFILT_ARENA *a, size_t sz )
{
    if ( !a || !a->buf ) return NULL;
    size_t aligned = ( a->used + 7u ) & ~(size_t) 7u;
    if ( aligned + sz > a->cap ) return NULL;
    void *p = a->buf + aligned;
    a->used = aligned + sz;
    return p;
}


/**
 * @brief Vytvoř LEAF uzel s vynulovaným payloadem.
 */
static st_EVFILT_NODE *make_leaf ( st_EVFILT_ARENA *a )
{
    if ( !a || a->node_count >= EVFILT_MAX_NODES ) return NULL;
    st_EVFILT_NODE *n = (st_EVFILT_NODE *) arena_alloc ( a, sizeof ( *n ) );
    if ( !n ) return NULL;
    memset ( n, 0, sizeof ( *n ) );
    n->kind = EVFILT_NODE_LEAF;
    a->node_count++;
    return n;
}


/**
 * @brief Vytvoř NOT uzel nad @c child.
 */
static st_EVFILT_NODE *make_not ( st_EVFILT_ARENA *a, st_EVFILT_NODE *child )
{
    if ( !child || !a || a->node_count >= EVFILT_MAX_NODES ) return NULL;
    st_EVFILT_NODE *n = (st_EVFILT_NODE *) arena_alloc ( a, sizeof ( *n ) );
    if ( !n ) return NULL;
    memset ( n, 0, sizeof ( *n ) );
    n->kind = EVFILT_NODE_NOT;
    n->u.child = child;
    a->node_count++;
    return n;
}


/**
 * @brief Vytvoř binární AND/OR uzel.
 */
static st_EVFILT_NODE *make_bin ( st_EVFILT_ARENA *a, uint8_t kind,
                                    st_EVFILT_NODE *l, st_EVFILT_NODE *r )
{
    if ( !l || !r || !a || a->node_count >= EVFILT_MAX_NODES ) return NULL;
    st_EVFILT_NODE *n = (st_EVFILT_NODE *) arena_alloc ( a, sizeof ( *n ) );
    if ( !n ) return NULL;
    memset ( n, 0, sizeof ( *n ) );
    n->kind = kind;
    n->u.bin.l = l;
    n->u.bin.r = r;
    a->node_count++;
    return n;
}


/**
 * @brief Vytvoř TEMPORAL_* uzel s @c window + reference sub-tree.
 *
 * Volá se z parseru pro @c before(N) / @c after(M) / @c near(K).
 *
 * @param a          Arena pro alokaci uzlu.
 * @param kind       Jeden z @c EVFILT_NODE_TEMPORAL_{BEFORE,AFTER,NEAR}.
 * @param window     Velikost okna v pxclk (= sjednoceně s @c pxclk_total).
 * @param reference  AST pod-strom definující reference events. Nesmí být @c NULL.
 * @return Nový node, nebo @c NULL při OOM / node limit / NULL reference.
 */
static st_EVFILT_NODE *make_temporal ( st_EVFILT_ARENA *a, uint8_t kind,
                                         uint64_t window,
                                         st_EVFILT_NODE *reference )
{
    if ( !reference || !a || a->node_count >= EVFILT_MAX_NODES ) return NULL;
    st_EVFILT_NODE *n = (st_EVFILT_NODE *) arena_alloc ( a, sizeof ( *n ) );
    if ( !n ) return NULL;
    memset ( n, 0, sizeof ( *n ) );
    n->kind = kind;
    n->u.temporal.window = window;
    n->u.temporal.reference = reference;
    a->node_count++;
    return n;
}


/**
 * @brief Test, zda je @c kind temporal node kind.
 */
static bool is_temporal_kind ( uint8_t kind )
{
    return ( kind == EVFILT_NODE_TEMPORAL_BEFORE
             || kind == EVFILT_NODE_TEMPORAL_AFTER
             || kind == EVFILT_NODE_TEMPORAL_NEAR );
}


/* ========================================================================= */
/*  Helper: low-level lexer primitives                                       */
/* ========================================================================= */


/**
 * @brief Stop-char pro leaf token (= konec contiguous run).
 *
 * Token končí na whitespace, závorkách nebo @c '!' (= operátor).
 * Keyword @c "or" se rozpoznává po extrakci tokenu.
 */
static bool is_token_stop ( char c )
{
    if ( c == '\0' ) return true;
    if ( c == '(' || c == ')' ) return true;
    if ( c == '!' ) return true;
    if ( isspace ( (unsigned char) c ) ) return true;
    return false;
}


/**
 * @brief Posune kurzor přes whitespace.
 */
static void skip_ws ( const char **pp )
{
    const char *p = *pp;
    while ( *p && isspace ( (unsigned char) *p ) ) p++;
    *pp = p;
}


/**
 * @brief Extrahuje další contiguous token (= bez WS, závorek, '!').
 *
 * @param pp    In/out kurzor.
 * @param tok   Out: pointer na začátek tokenu.
 * @param tlen  Out: délka tokenu.
 */
static void scan_token ( const char **pp, const char **tok, size_t *tlen )
{
    skip_ws ( pp );
    const char *p = *pp;
    const char *start = p;
    while ( *p && !is_token_stop ( *p ) ) p++;
    *tok = start;
    *tlen = (size_t)( p - start );
    *pp = p;
}


/**
 * @brief Test rovnosti tokenu s string literálem (case-sensitive).
 */
static bool token_eq ( const char *tok, size_t tlen, const char *lit )
{
    size_t llen = strlen ( lit );
    if ( tlen != llen ) return false;
    return memcmp ( tok, lit, tlen ) == 0;
}


/**
 * @brief Parse hex literal (1-8 chars). Volitelný @c "0x" prefix.
 *
 * @return @c true pokud parse uspěl, @c out = hodnota.
 */
static bool parse_hex ( const char *s, size_t len, uint32_t *out )
{
    if ( len == 0 ) return false;
    if ( len >= 2 && s[ 0 ] == '0' && ( s[ 1 ] == 'x' || s[ 1 ] == 'X' ) ) {
        s += 2;
        len -= 2;
    }
    if ( len == 0 || len > 8 ) return false;
    uint32_t v = 0;
    for ( size_t i = 0; i < len; i++ ) {
        char c = s[ i ];
        uint32_t d;
        if ( c >= '0' && c <= '9' ) d = (uint32_t) ( c - '0' );
        else if ( c >= 'a' && c <= 'f' ) d = (uint32_t) ( c - 'a' + 10 );
        else if ( c >= 'A' && c <= 'F' ) d = (uint32_t) ( c - 'A' + 10 );
        else return false;
        v = ( v << 4 ) | d;
    }
    *out = v;
    return true;
}


/**
 * @brief Parse decimal literal s volitelným @c k / @c M suffixem.
 *
 * Suffix multipliers:
 *  - @c k / @c K = * 1000
 *  - @c M       = * 1000000
 *
 * @return @c true při úspěchu.
 */
static bool parse_dec_suffixed ( const char *s, size_t len, uint64_t *out )
{
    if ( len == 0 ) return false;

    uint64_t mult = 1;
    if ( s[ len - 1 ] == 'k' || s[ len - 1 ] == 'K' ) {
        mult = 1000u;
        len--;
    } else if ( s[ len - 1 ] == 'M' ) {
        mult = 1000000u;
        len--;
    }
    if ( len == 0 ) return false;

    uint64_t v = 0;
    for ( size_t i = 0; i < len; i++ ) {
        char c = s[ i ];
        if ( c < '0' || c > '9' ) return false;
        v = v * 10u + (uint64_t) ( c - '0' );
    }
    *out = v * mult;
    return true;
}


/**
 * @brief Parse decimal literal bez suffixu.
 */
static bool parse_dec ( const char *s, size_t len, uint64_t *out )
{
    if ( len == 0 ) return false;
    uint64_t v = 0;
    for ( size_t i = 0; i < len; i++ ) {
        char c = s[ i ];
        if ( c < '0' || c > '9' ) return false;
        v = v * 10u + (uint64_t) ( c - '0' );
    }
    *out = v;
    return true;
}


/* ========================================================================= */
/*  Name <-> category mapping                                                */
/* ========================================================================= */


/**
 * @brief Tabulka jména -> kategorie (statická, sjednocená s @c en_EVENTLOG_CATEGORY).
 */
static const struct {
    const char *name;
    uint8_t     cat;
} s_cat_table[] = {
    { "cpu_int",      EVENTLOG_CAT_CPU_INT      },
    { "cpu_pin_edge", EVENTLOG_CAT_CPU_PIN_EDGE },
    { "irq_ack_im2",  EVENTLOG_CAT_IRQ_ACK_IM2  },
    { "iorq_in",      EVENTLOG_CAT_IORQ_IN      },
    { "iorq_out",     EVENTLOG_CAT_IORQ_OUT     },
    { "mmio_r",       EVENTLOG_CAT_MMIO_R       },
    { "mmio_w",       EVENTLOG_CAT_MMIO_W       },
    { "gdg_mode",     EVENTLOG_CAT_GDG_MODE     },
    { "gdg_banking",  EVENTLOG_CAT_GDG_BANKING  },
    { "gdg_hwscroll", EVENTLOG_CAT_GDG_HWSCROLL },
    { "gdg_colors",   EVENTLOG_CAT_GDG_COLORS   },
    { "gdg_video",    EVENTLOG_CAT_GDG_VIDEO    },
    { "gdg_wfrf",     EVENTLOG_CAT_GDG_WFRF     },
    { "pio8255",      EVENTLOG_CAT_PIO8255      },
    { "ctc8253",      EVENTLOG_CAT_CTC8253      },
    { "pioz80",       EVENTLOG_CAT_PIOZ80       },
    { "psg",          EVENTLOG_CAT_PSG          },
    { "qd",           EVENTLOG_CAT_QD           },
    { "fdc",          EVENTLOG_CAT_FDC          },
    { "memext",       EVENTLOG_CAT_MEMEXT       },
    { "rd",           EVENTLOG_CAT_RD           },
    { "bp_fire",      EVENTLOG_CAT_BP_FIRE      },
    { "user_mark",    EVENTLOG_CAT_USER_MARK    },
    { "cpu_ctrl",     EVENTLOG_CAT_CPU_CTRL     },
    { "sys",          EVENTLOG_CAT_SYS          },
};


int eventlog_filter_cat_from_name ( const char *name )
{
    if ( !name || !name[ 0 ] ) return -1;
    for ( size_t i = 0; i < sizeof ( s_cat_table ) / sizeof ( s_cat_table[ 0 ] ); i++ ) {
        if ( strcmp ( name, s_cat_table[ i ].name ) == 0 ) {
            return (int) s_cat_table[ i ].cat;
        }
    }
    return -1;
}


const char *eventlog_filter_cat_to_name ( uint8_t cat )
{
    for ( size_t i = 0; i < sizeof ( s_cat_table ) / sizeof ( s_cat_table[ 0 ] ); i++ ) {
        if ( s_cat_table[ i ].cat == cat ) return s_cat_table[ i ].name;
    }
    return NULL;
}


/* ========================================================================= */
/*  Leaf builders per prefix                                                 */
/* ========================================================================= */


/**
 * @brief Parse @c cat:NAME[,NAME]* hodnotu do bitmasky.
 */
static bool leaf_fill_cat ( const char *val, size_t vlen,
                              st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_CAT_MASK;
    leaf->mask64 = 0;

    const char *p = val;
    const char *end = val + vlen;
    char name_buf[ 32 ];

    while ( p < end ) {
        const char *comma = NULL;
        for ( const char *q = p; q < end; q++ ) {
            if ( *q == ',' ) { comma = q; break; }
        }
        const char *name_end = comma ? comma : end;
        size_t nlen = (size_t) ( name_end - p );
        if ( nlen == 0 || nlen >= sizeof ( name_buf ) ) {
            set_err ( f, "Invalid cat name" );
            return false;
        }
        memcpy ( name_buf, p, nlen );
        name_buf[ nlen ] = '\0';
        int cat = eventlog_filter_cat_from_name ( name_buf );
        if ( cat < 0 ) {
            set_err ( f, "Unknown cat name" );
            return false;
        }
        leaf->mask64 |= ( 1ULL << (unsigned) cat );
        if ( !comma ) break;
        p = comma + 1;
    }

    if ( leaf->mask64 == 0 ) {
        set_err ( f, "Empty cat value" );
        return false;
    }
    return true;
}


/**
 * @brief Parse @c sub:NUM[,NUM]* hodnotu do 256-bit bitmasky.
 */
static bool leaf_fill_sub ( const char *val, size_t vlen,
                              st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_SUB_MASK;
    memset ( leaf->sub_mask, 0, sizeof ( leaf->sub_mask ) );

    const char *p = val;
    const char *end = val + vlen;
    bool any = false;

    while ( p < end ) {
        const char *comma = NULL;
        for ( const char *q = p; q < end; q++ ) {
            if ( *q == ',' ) { comma = q; break; }
        }
        const char *num_end = comma ? comma : end;
        size_t nlen = (size_t) ( num_end - p );
        uint64_t v;
        if ( !parse_dec ( p, nlen, &v ) || v > 255 ) {
            set_err ( f, "Invalid sub value" );
            return false;
        }
        leaf->sub_mask[ v >> 6 ] |= ( 1ULL << ( v & 63u ) );
        any = true;
        if ( !comma ) break;
        p = comma + 1;
    }

    if ( !any ) {
        set_err ( f, "Empty sub value" );
        return false;
    }
    return true;
}


/**
 * @brief Parse @c pc:HEX nebo @c pc:HEX-HEX (16-bit).
 */
static bool leaf_fill_pc ( const char *val, size_t vlen,
                            st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_PC_RANGE;

    const char *dash = NULL;
    /* Dash detekce - skip-uj 0x prefix aby "-" v "0x..-0x.." nepletlo. */
    size_t scan_from = 0;
    if ( vlen >= 2 && val[ 0 ] == '0' && ( val[ 1 ] == 'x' || val[ 1 ] == 'X' ) ) {
        scan_from = 2;
    }
    for ( size_t i = scan_from; i < vlen; i++ ) {
        if ( val[ i ] == '-' ) { dash = &val[ i ]; break; }
    }

    if ( dash ) {
        size_t lo_len = (size_t) ( dash - val );
        size_t hi_len = vlen - lo_len - 1;
        uint32_t lo, hi;
        if ( !parse_hex ( val, lo_len, &lo ) || !parse_hex ( dash + 1, hi_len, &hi )
             || lo > 0xFFFFu || hi > 0xFFFFu || lo > hi ) {
            set_err ( f, "Invalid pc range" );
            return false;
        }
        leaf->rmin = lo;
        leaf->rmax = hi;
        return true;
    }

    uint32_t v;
    if ( !parse_hex ( val, vlen, &v ) || v > 0xFFFFu ) {
        set_err ( f, "Invalid pc hex" );
        return false;
    }
    leaf->rmin = v;
    leaf->rmax = v;
    return true;
}


/**
 * @brief Parse @c frame:N / @c frame:>N / @c frame:<N (dec, no suffix).
 */
static bool leaf_fill_frame ( const char *val, size_t vlen,
                                st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_FRAME_OP;

    if ( vlen == 0 ) {
        set_err ( f, "Empty frame value" );
        return false;
    }

    if ( val[ 0 ] == '>' ) {
        uint64_t n;
        if ( !parse_dec ( val + 1, vlen - 1, &n ) ) {
            set_err ( f, "Invalid frame:>N" );
            return false;
        }
        leaf->rmin = ( n < UINT64_MAX ) ? n + 1u : UINT64_MAX;
        leaf->rmax = UINT64_MAX;
        return true;
    }

    if ( val[ 0 ] == '<' ) {
        uint64_t n;
        if ( !parse_dec ( val + 1, vlen - 1, &n ) ) {
            set_err ( f, "Invalid frame:<N" );
            return false;
        }
        leaf->rmin = 0;
        leaf->rmax = ( n > 0 ) ? n - 1u : 0;
        return true;
    }

    uint64_t n;
    if ( !parse_dec ( val, vlen, &n ) ) {
        set_err ( f, "Invalid frame value" );
        return false;
    }
    leaf->rmin = n;
    leaf->rmax = n;
    return true;
}


/**
 * @brief Parse @c cycle:N / @c cycle:>N / @c cycle:<N s k/M suffixem.
 */
static bool leaf_fill_cycle ( const char *val, size_t vlen,
                                st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_CYCLE_OP;

    if ( vlen == 0 ) {
        set_err ( f, "Empty cycle value" );
        return false;
    }

    if ( val[ 0 ] == '>' ) {
        uint64_t n;
        if ( !parse_dec_suffixed ( val + 1, vlen - 1, &n ) ) {
            set_err ( f, "Invalid cycle:>N" );
            return false;
        }
        leaf->rmin = ( n < UINT64_MAX ) ? n + 1u : UINT64_MAX;
        leaf->rmax = UINT64_MAX;
        return true;
    }

    if ( val[ 0 ] == '<' ) {
        uint64_t n;
        if ( !parse_dec_suffixed ( val + 1, vlen - 1, &n ) ) {
            set_err ( f, "Invalid cycle:<N" );
            return false;
        }
        leaf->rmin = 0;
        leaf->rmax = ( n > 0 ) ? n - 1u : 0;
        return true;
    }

    /* Range cycle:A-B (oba s podporou suffixu) */
    const char *dash = NULL;
    for ( size_t i = 0; i < vlen; i++ ) {
        if ( val[ i ] == '-' ) { dash = &val[ i ]; break; }
    }
    if ( dash ) {
        size_t lo_len = (size_t) ( dash - val );
        size_t hi_len = vlen - lo_len - 1;
        uint64_t lo, hi;
        if ( !parse_dec_suffixed ( val, lo_len, &lo )
             || !parse_dec_suffixed ( dash + 1, hi_len, &hi ) || lo > hi ) {
            set_err ( f, "Invalid cycle range" );
            return false;
        }
        leaf->rmin = lo;
        leaf->rmax = hi;
        return true;
    }

    uint64_t n;
    if ( !parse_dec_suffixed ( val, vlen, &n ) ) {
        set_err ( f, "Invalid cycle value" );
        return false;
    }
    leaf->rmin = n;
    leaf->rmax = n;
    return true;
}


/**
 * @brief Parse @c sline:N nebo @c sline:N-N (dec).
 */
static bool leaf_fill_sline ( const char *val, size_t vlen,
                                st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_SLINE_RANGE;

    const char *dash = NULL;
    for ( size_t i = 0; i < vlen; i++ ) {
        if ( val[ i ] == '-' ) { dash = &val[ i ]; break; }
    }
    if ( dash ) {
        size_t lo_len = (size_t) ( dash - val );
        size_t hi_len = vlen - lo_len - 1;
        uint64_t lo, hi;
        if ( !parse_dec ( val, lo_len, &lo ) || !parse_dec ( dash + 1, hi_len, &hi )
             || lo > hi ) {
            set_err ( f, "Invalid sline range" );
            return false;
        }
        leaf->rmin = lo;
        leaf->rmax = hi;
        return true;
    }

    uint64_t n;
    if ( !parse_dec ( val, vlen, &n ) ) {
        set_err ( f, "Invalid sline value" );
        return false;
    }
    leaf->rmin = n;
    leaf->rmax = n;
    return true;
}


/**
 * @brief Parse @c px:N nebo @c px:N-N (dec).
 */
static bool leaf_fill_px ( const char *val, size_t vlen,
                            st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_PX_RANGE;

    const char *dash = NULL;
    for ( size_t i = 0; i < vlen; i++ ) {
        if ( val[ i ] == '-' ) { dash = &val[ i ]; break; }
    }
    if ( dash ) {
        size_t lo_len = (size_t) ( dash - val );
        size_t hi_len = vlen - lo_len - 1;
        uint64_t lo, hi;
        if ( !parse_dec ( val, lo_len, &lo ) || !parse_dec ( dash + 1, hi_len, &hi )
             || lo > hi ) {
            set_err ( f, "Invalid px range" );
            return false;
        }
        leaf->rmin = lo;
        leaf->rmax = hi;
        return true;
    }

    uint64_t n;
    if ( !parse_dec ( val, vlen, &n ) ) {
        set_err ( f, "Invalid px value" );
        return false;
    }
    leaf->rmin = n;
    leaf->rmax = n;
    return true;
}


/**
 * @brief Parse @c payload:HEX (uint32 exact match).
 */
static bool leaf_fill_payload ( const char *val, size_t vlen,
                                  st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_PAYLOAD_EQ;
    uint32_t v;
    if ( !parse_hex ( val, vlen, &v ) ) {
        set_err ( f, "Invalid payload hex" );
        return false;
    }
    leaf->rmin = v;
    leaf->rmax = v;
    return true;
}


/* ========================================================================= */
/*  Ambient field lookup tables (Vlna 4 Commit 25)                           */
/* ========================================================================= */


/**
 * @brief Mapování @c reason jména -> 3-bit kód (= @ref en_EVENTLOG_AMBIENT_REASON).
 *
 * @param name  Lowercase identifier (např. @c "int_ack", @c "none").
 * @return Hodnota 0..7 nebo @c -1 při neznámém jméně / @c NULL.
 */
static int eventlog_filter_reason_name_to_code ( const char *name )
{
    if ( !name ) return -1;
    if ( strcmp ( name, "reset"   ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_RESET;
    if ( strcmp ( name, "ei"      ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_EI;
    if ( strcmp ( name, "di"      ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_DI;
    if ( strcmp ( name, "int_ack" ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_INT_ACK;
    if ( strcmp ( name, "nmi_ack" ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_NMI_ACK;
    if ( strcmp ( name, "reti"    ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_RETI;
    if ( strcmp ( name, "retn"    ) == 0 ) return EVENTLOG_AMBIENT_REASON_IFF_RETN;
    if ( strcmp ( name, "none"    ) == 0 ) return EVENTLOG_AMBIENT_REASON_NONE;
    return -1;
}


/**
 * @brief Inverzní mapování @c reason kódu -> jméno (pro round-trip).
 *
 * @param code  Hodnota 0..7.
 * @return Statický lowercase string nebo @c NULL při invalid kódu.
 */
static const char *eventlog_filter_reason_code_to_name ( uint8_t code )
{
    switch ( code ) {
        case EVENTLOG_AMBIENT_REASON_IFF_RESET:   return "reset";
        case EVENTLOG_AMBIENT_REASON_IFF_EI:      return "ei";
        case EVENTLOG_AMBIENT_REASON_IFF_DI:      return "di";
        case EVENTLOG_AMBIENT_REASON_IFF_INT_ACK: return "int_ack";
        case EVENTLOG_AMBIENT_REASON_IFF_NMI_ACK: return "nmi_ack";
        case EVENTLOG_AMBIENT_REASON_IFF_RETI:    return "reti";
        case EVENTLOG_AMBIENT_REASON_IFF_RETN:    return "retn";
        case EVENTLOG_AMBIENT_REASON_NONE:        return "none";
        default: return NULL;
    }
}


/**
 * @brief Mapování @c banking jména -> 3-bit kód (= @ref en_EVENTLOG_AMBIENT_BANKING).
 *
 * Pojmenování odpovídá enum hodnotám z @c trace/eventlog.h. Banking kódy
 * jsou per-arch coded (= jeden enum, různá sémantika 800/700/1500), ale
 * lookup tabulka je společná. Některá jména mají v dané arch význam
 * @c "(nepoužito)" - filter to akceptuje, jenom match vrací @c false na
 * eventech které takové banking nikdy neemitují.
 *
 * Synonyma pro UX: @c "ram" je alias pro @c "all_ram", @c "default" je
 * alias pro @c "default" (existuje jenom kvůli explicitní lookup).
 *
 * @param name  Lowercase identifier (např. @c "ram", @c "rom_low_off").
 * @return Hodnota 0..7 nebo @c -1 při neznámém / @c NULL.
 */
static int eventlog_filter_banking_name_to_code ( const char *name )
{
    if ( !name ) return -1;
    /* Kanonická jména (1:1 s enum). */
    if ( strcmp ( name, "default"      ) == 0 ) return EVENTLOG_AMBIENT_BANKING_DEFAULT;
    if ( strcmp ( name, "all_ram"      ) == 0 ) return EVENTLOG_AMBIENT_BANKING_ALL_RAM;
    if ( strcmp ( name, "rom_low_off"  ) == 0 ) return EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF;
    if ( strcmp ( name, "rom_high_off" ) == 0 ) return EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF;
    if ( strcmp ( name, "cgrom"        ) == 0 ) return EVENTLOG_AMBIENT_BANKING_CGROM;
    if ( strcmp ( name, "vram_640"     ) == 0 ) return EVENTLOG_AMBIENT_BANKING_VRAM_640;
    if ( strcmp ( name, "pcg_high"     ) == 0 ) return EVENTLOG_AMBIENT_BANKING_PCG_HIGH;
    if ( strcmp ( name, "other"        ) == 0 ) return EVENTLOG_AMBIENT_BANKING_OTHER;

    /* UX synonyma (= zachovává konvence z mutant spec). */
    if ( strcmp ( name, "ram"          ) == 0 ) return EVENTLOG_AMBIENT_BANKING_ALL_RAM;
    if ( strcmp ( name, "vram"         ) == 0 ) return EVENTLOG_AMBIENT_BANKING_VRAM_640;
    if ( strcmp ( name, "vram_low"     ) == 0 ) return EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF;
    if ( strcmp ( name, "pcg_1"        ) == 0 ) return EVENTLOG_AMBIENT_BANKING_VRAM_640;
    if ( strcmp ( name, "pcg_2"        ) == 0 ) return EVENTLOG_AMBIENT_BANKING_PCG_HIGH;
    if ( strcmp ( name, "pcg_3"        ) == 0 ) return EVENTLOG_AMBIENT_BANKING_PCG_HIGH;
    return -1;
}


/**
 * @brief Inverzní mapování @c banking kódu -> kanonické jméno.
 *
 * Vrací kanonické (= 1:1 s enum) jméno; synonyma se nepoužívají pro
 * round-trip - parser je akceptuje, ale @c to_string emituje kanonickou
 * formu.
 *
 * @param code  Hodnota 0..7.
 * @return Statický string nebo @c NULL při invalid kódu.
 */
static const char *eventlog_filter_banking_code_to_name ( uint8_t code )
{
    switch ( code ) {
        case EVENTLOG_AMBIENT_BANKING_DEFAULT:      return "default";
        case EVENTLOG_AMBIENT_BANKING_ALL_RAM:      return "all_ram";
        case EVENTLOG_AMBIENT_BANKING_ROM_LOW_OFF:  return "rom_low_off";
        case EVENTLOG_AMBIENT_BANKING_ROM_HIGH_OFF: return "rom_high_off";
        case EVENTLOG_AMBIENT_BANKING_CGROM:        return "cgrom";
        case EVENTLOG_AMBIENT_BANKING_VRAM_640:     return "vram_640";
        case EVENTLOG_AMBIENT_BANKING_PCG_HIGH:     return "pcg_high";
        case EVENTLOG_AMBIENT_BANKING_OTHER:        return "other";
        default: return NULL;
    }
}


/**
 * @brief Postaví ambient leaf z field jména + value tokenu.
 *
 * Field jména: @c iff1 / @c im / @c reason / @c banking. Value sémantika
 * závislá na fieldu:
 *
 *  - @c iff1:0 / @c iff1:1 - 1 bit, AMBIENT_BIT.
 *  - @c im:0 / @c im:1 / @c im:2 - 2 bity, AMBIENT_FIELD.
 *  - @c reason:NAME - 3 bity, lookup table.
 *  - @c banking:NAME - 3 bity, lookup table.
 *
 * @param fname  Pointer na jméno fieldu (NEnull-terminated).
 * @param flen   Délka jména fieldu.
 * @param vname  Pointer na hodnotu po dvojtečce (NEnull-terminated).
 * @param vlen   Délka hodnoty.
 * @param leaf   Out: vyplněný leaf.
 * @param f      Filter handle pro error reporting.
 * @return @c true při úspěchu, @c false při syntax/neznámém jméně.
 */
static bool leaf_fill_ambient ( const char *fname, size_t flen,
                                  const char *vname, size_t vlen,
                                  st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    if ( flen == 0 || vlen == 0 ) {
        set_err ( f, "Empty if field or value" );
        return false;
    }

    /* Buffery na nul-terminated kopie (= jména jsou krátká). */
    char fbuf[ 16 ];
    char vbuf[ 32 ];
    if ( flen >= sizeof ( fbuf ) || vlen >= sizeof ( vbuf ) ) {
        set_err ( f, "if field/value too long" );
        return false;
    }
    memcpy ( fbuf, fname, flen ); fbuf[ flen ] = '\0';
    memcpy ( vbuf, vname, vlen ); vbuf[ vlen ] = '\0';

    if ( strcmp ( fbuf, "iff1" ) == 0 ) {
        uint64_t n;
        if ( !parse_dec ( vname, vlen, &n ) || n > 1u ) {
            set_err ( f, "Invalid if iff1 value (0 or 1)" );
            return false;
        }
        leaf->kind         = EVFILT_LEAF_AMBIENT_BIT;
        leaf->amb_mask     = (uint16_t) EVENTLOG_AMBIENT_IFF1;
        leaf->amb_expected = ( n != 0 ) ? (uint16_t) EVENTLOG_AMBIENT_IFF1 : 0u;
        leaf->amb_field    = EVFILT_AMBIENT_FIELD_IFF1;
        return true;
    }

    if ( strcmp ( fbuf, "im" ) == 0 ) {
        uint64_t n;
        if ( !parse_dec ( vname, vlen, &n ) || n > 2u ) {
            set_err ( f, "Invalid if im value (0/1/2)" );
            return false;
        }
        leaf->kind         = EVFILT_LEAF_AMBIENT_FIELD;
        leaf->amb_mask     = (uint16_t) EVENTLOG_AMBIENT_IM_MASK;
        leaf->amb_expected = (uint16_t) ( n << EVENTLOG_AMBIENT_IM_SHIFT );
        leaf->amb_field    = EVFILT_AMBIENT_FIELD_IM;
        return true;
    }

    if ( strcmp ( fbuf, "reason" ) == 0 ) {
        int code = eventlog_filter_reason_name_to_code ( vbuf );
        if ( code < 0 ) {
            set_err ( f, "Unknown if reason name" );
            return false;
        }
        leaf->kind         = EVFILT_LEAF_AMBIENT_FIELD;
        leaf->amb_mask     = (uint16_t) EVENTLOG_AMBIENT_REASON_MASK;
        leaf->amb_expected = (uint16_t) ( (unsigned) code << EVENTLOG_AMBIENT_REASON_SHIFT );
        leaf->amb_field    = EVFILT_AMBIENT_FIELD_REASON;
        return true;
    }

    if ( strcmp ( fbuf, "banking" ) == 0 ) {
        int code = eventlog_filter_banking_name_to_code ( vbuf );
        if ( code < 0 ) {
            set_err ( f, "Unknown if banking name" );
            return false;
        }
        leaf->kind         = EVFILT_LEAF_AMBIENT_FIELD;
        leaf->amb_mask     = (uint16_t) EVENTLOG_AMBIENT_BANKING_MASK;
        leaf->amb_expected = (uint16_t) ( (unsigned) code << EVENTLOG_AMBIENT_BANKING_SHIFT );
        leaf->amb_field    = EVFILT_AMBIENT_FIELD_BANKING;
        return true;
    }

    set_err ( f, "Unknown if field (iff1/im/reason/banking)" );
    return false;
}


/**
 * @brief Test platnosti identifier znaku (lowercase, digit, underscore).
 *
 * Identifier symbolu = [a-zA-Z0-9_]+ (case-sensitive). Sym_db povoluje
 * širší, ale pro filter token držíme alfanumeric+underscore aby zápis
 * neinterferoval s lexer stop-chary.
 */
static bool is_ident_char ( char c )
{
    if ( c >= 'a' && c <= 'z' ) return true;
    if ( c >= 'A' && c <= 'Z' ) return true;
    if ( c >= '0' && c <= '9' ) return true;
    if ( c == '_' ) return true;
    return false;
}


/**
 * @brief Validuje, že @c val délky @c vlen je identifier (s volitelným
 *        @c '*' suffixem pro prefix match).
 *
 * @param val          Hodnota tokenu.
 * @param vlen         Délka.
 * @param[out] is_pref @c true pokud končí @c '*' (= prefix).
 * @param[out] ilen    Délka bez koncového @c '*' (pokud has_star).
 * @return @c true pokud platný identifier; @c false jinak.
 */
static bool validate_sym_ident ( const char *val, size_t vlen,
                                   bool *is_pref, size_t *ilen )
{
    if ( vlen == 0 ) return false;
    *is_pref = false;
    *ilen = vlen;
    if ( val[ vlen - 1 ] == '*' ) {
        if ( vlen == 1 ) return false;  /* sám hvězdička není identifier */
        *is_pref = true;
        *ilen = vlen - 1;
    }
    for ( size_t i = 0; i < *ilen; i++ ) {
        if ( !is_ident_char ( val[ i ] ) ) return false;
    }
    return true;
}


/**
 * @brief Pre-resolve cache adres pro @c sym:PREFIX_* leaf.
 *
 * Scanne @c sym_db a vybere všechny záznamy s @c bank_id == 0, jejichž
 * jméno začíná @c prefix. Adresa je oříznuta na 16-bit (= PC v eventu
 * je 16-bit, sym_db drží 32-bit pro physical V1.5).
 *
 * Při OOM ponechá @c sym_cached_addrs = NULL, @c sym_cached_count = 0
 * (= match vrátí vždy false). Pokud žádný symbol neodpovídá, taktéž
 * NULL + 0.
 *
 * @return @c true vždy (= chybový stav je matchování false, ne parse fail).
 */
static bool resolve_sym_prefix ( const char *prefix, size_t plen,
                                   st_EVFILT_LEAF *leaf )
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Dvouprůchodový scan: count + collect, ať alokujeme přesně. */
    size_t count = 0;
    st_SYM_DB_ITER it;
    sym_db_iter_init ( &it );
    const st_SYMBOL *s;
    while ( ( s = sym_db_iter_next ( &it ) ) != NULL ) {
        if ( s->bank_id != 0 ) continue;
        if ( !s->name ) continue;
        if ( strncmp ( s->name, prefix, plen ) == 0 ) {
            count++;
        }
    }
    if ( count == 0 ) {
        leaf->sym_cached_addrs = NULL;
        leaf->sym_cached_count = 0;
        return true;
    }
    leaf->sym_cached_addrs = (uint16_t *) malloc ( count * sizeof ( uint16_t ) );
    if ( !leaf->sym_cached_addrs ) {
        leaf->sym_cached_count = 0;
        return true;
    }
    size_t idx = 0;
    sym_db_iter_init ( &it );
    while ( ( s = sym_db_iter_next ( &it ) ) != NULL ) {
        if ( s->bank_id != 0 ) continue;
        if ( !s->name ) continue;
        if ( strncmp ( s->name, prefix, plen ) == 0 ) {
            if ( idx < count ) {
                leaf->sym_cached_addrs[ idx++ ] = (uint16_t) ( s->addr & 0xFFFFu );
            }
        }
    }
    leaf->sym_cached_count = idx;
    return true;
#else
    (void) prefix; (void) plen;
    leaf->sym_cached_addrs = NULL;
    leaf->sym_cached_count = 0;
    return true;
#endif
}


/**
 * @brief Parse @c sym:NAME nebo @c sym:PREFIX_* hodnotu.
 *
 * Plný build leaf-u SYM_EXACT/SYM_PREFIX + uložení jména +
 * pre-resolve cache pro PREFIX.
 */
static bool leaf_fill_sym ( const char *val, size_t vlen,
                              st_EVFILT_LEAF *leaf, st_EVENTLOG_FILTER *f )
{
    bool is_pref = false;
    size_t ilen = 0;
    if ( !validate_sym_ident ( val, vlen, &is_pref, &ilen ) ) {
        set_err ( f, "Invalid sym identifier" );
        return false;
    }

    /* Identifier kopie do heap (NULL-terminated). */
    char *name = (char *) malloc ( ilen + 1 );
    if ( !name ) {
        set_err ( f, "OOM allocating sym name" );
        return false;
    }
    memcpy ( name, val, ilen );
    name[ ilen ] = '\0';

    leaf->sym_name_a = name;
    leaf->sym_name_b = NULL;
    leaf->sym_cached_addrs = NULL;
    leaf->sym_cached_count = 0;
    leaf->sym_cached_addr_a = 0;
    leaf->sym_cached_addr_b = 0;
    leaf->sym_cached_valid_a = false;
    leaf->sym_cached_valid_b = false;

    if ( is_pref ) {
        leaf->kind = EVFILT_LEAF_SYM_PREFIX;
        resolve_sym_prefix ( name, ilen, leaf );
    } else {
        leaf->kind = EVFILT_LEAF_SYM_EXACT;
        /* Pre-resolve jméno na adresu v čase parse (Commit 19).
         * Eval se pak nemusí volat sym_db_lookup_by_name() per event,
         * což je kritické pro pause-on-match hot-path. Stale cache po
         * sym_db change je akceptovatelná - UI musí re-parse filter. */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        const st_SYMBOL *s = sym_db_lookup_by_name ( name );
        if ( s ) {
            leaf->sym_cached_addr_a = (uint16_t) ( s->addr & 0xFFFFu );
            leaf->sym_cached_valid_a = true;
        }
#endif
    }
    track_sym_leaf ( f, leaf );
    return true;
}


/**
 * @brief Build leaf SYM_RANGE z dvou jmen (= @c from_sym:A @c to_sym:B).
 *
 * Volá se z parseru po extrakci dvou tokenů. Obě jména jsou heap-kopie.
 * Adresy se NEresolvují při parse - @c eval_leaf je lookup-uje při
 * každém matchi (sym_db lookup je O(N) lineární, ale typicky <500
 * položek = OK). Důvod: cache by se musela invalidovat při sym_db
 * change, což zatím nehlídáme.
 */
static bool leaf_fill_sym_range ( const char *name_a, size_t alen,
                                    const char *name_b, size_t blen,
                                    st_EVFILT_LEAF *leaf,
                                    st_EVENTLOG_FILTER *f )
{
    leaf->kind = EVFILT_LEAF_SYM_RANGE;

    /* Validace - jména musí být holý identifier (= bez '*'). */
    for ( size_t i = 0; i < alen; i++ ) {
        if ( !is_ident_char ( name_a[ i ] ) ) {
            set_err ( f, "Invalid from_sym name" );
            return false;
        }
    }
    for ( size_t i = 0; i < blen; i++ ) {
        if ( !is_ident_char ( name_b[ i ] ) ) {
            set_err ( f, "Invalid to_sym name" );
            return false;
        }
    }

    char *a = (char *) malloc ( alen + 1 );
    char *b = (char *) malloc ( blen + 1 );
    if ( !a || !b ) {
        free ( a );
        free ( b );
        set_err ( f, "OOM allocating sym names" );
        return false;
    }
    memcpy ( a, name_a, alen ); a[ alen ] = '\0';
    memcpy ( b, name_b, blen ); b[ blen ] = '\0';

    leaf->sym_name_a = a;
    leaf->sym_name_b = b;
    leaf->sym_cached_addrs = NULL;
    leaf->sym_cached_count = 0;
    leaf->sym_cached_addr_a = 0;
    leaf->sym_cached_addr_b = 0;
    leaf->sym_cached_valid_a = false;
    leaf->sym_cached_valid_b = false;

    /* Pre-resolve both names při parse (Commit 19) - viz leaf_fill_sym
     * komentář. Range eval pak používá cached_addr_a/b a vyhne se O(N)
     * sym_db_lookup_by_name() per event. */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    const st_SYMBOL *sa = sym_db_lookup_by_name ( a );
    const st_SYMBOL *sb = sym_db_lookup_by_name ( b );
    if ( sa ) {
        leaf->sym_cached_addr_a = (uint16_t) ( sa->addr & 0xFFFFu );
        leaf->sym_cached_valid_a = true;
    }
    if ( sb ) {
        leaf->sym_cached_addr_b = (uint16_t) ( sb->addr & 0xFFFFu );
        leaf->sym_cached_valid_b = true;
    }
#endif

    track_sym_leaf ( f, leaf );
    return true;
}


/* ========================================================================= */
/*  Token -> LEAF parser                                                     */
/* ========================================================================= */


/**
 * @brief Postaví LEAF uzel z jednoho atomického tokenu (= prefix:value).
 *
 * Podporované prefixy: cat / sub / pc / frame / cycle / sline / px / payload.
 * Neznámý prefix nebo missing ':' = chyba.
 */
static st_EVFILT_NODE *parse_token_to_leaf ( st_EVENTLOG_FILTER *f,
                                               const char *tok, size_t tlen )
{
    /* Detekuj prefix:value tvar. */
    const char *colon = NULL;
    for ( size_t i = 0; i < tlen; i++ ) {
        if ( tok[ i ] == ':' ) { colon = &tok[ i ]; break; }
    }
    if ( !colon ) {
        set_err ( f, "Token missing ':' prefix" );
        return NULL;
    }

    size_t plen = (size_t) ( colon - tok );
    const char *val = colon + 1;
    size_t vlen = tlen - plen - 1;

    if ( vlen == 0 ) {
        set_err ( f, "Empty value after prefix" );
        return NULL;
    }

    st_EVFILT_NODE *node = make_leaf ( &f->arena );
    if ( !node ) {
        set_err ( f, "Expression too complex" );
        return NULL;
    }
    st_EVFILT_LEAF *leaf = &node->u.leaf;

    if ( plen == 3 && memcmp ( tok, "cat", 3 ) == 0 ) {
        if ( !leaf_fill_cat ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 3 && memcmp ( tok, "sub", 3 ) == 0 ) {
        if ( !leaf_fill_sub ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 2 && memcmp ( tok, "pc", 2 ) == 0 ) {
        if ( !leaf_fill_pc ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 5 && memcmp ( tok, "frame", 5 ) == 0 ) {
        if ( !leaf_fill_frame ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 5 && memcmp ( tok, "cycle", 5 ) == 0 ) {
        if ( !leaf_fill_cycle ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 5 && memcmp ( tok, "sline", 5 ) == 0 ) {
        if ( !leaf_fill_sline ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 2 && memcmp ( tok, "px", 2 ) == 0 ) {
        if ( !leaf_fill_px ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 7 && memcmp ( tok, "payload", 7 ) == 0 ) {
        if ( !leaf_fill_payload ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    if ( plen == 3 && memcmp ( tok, "sym", 3 ) == 0 ) {
        if ( !leaf_fill_sym ( val, vlen, leaf, f ) ) return NULL;
        return node;
    }
    /* from_sym: a to_sym: jsou rozpoznané v parse_leaf_from_stream
     * (= dvojice tokenů). Sole "from_sym:" / "to_sym:" zde znamená
     * chybu (parser ji ošetří dřív, ale fallback safety net). */
    if ( plen == 8 && memcmp ( tok, "from_sym", 8 ) == 0 ) {
        set_err ( f, "'from_sym:' must be followed by 'to_sym:'" );
        return NULL;
    }
    if ( plen == 6 && memcmp ( tok, "to_sym", 6 ) == 0 ) {
        set_err ( f, "'to_sym:' must follow 'from_sym:'" );
        return NULL;
    }

    set_err ( f, "Unknown filter prefix" );
    return NULL;
}


/* ========================================================================= */
/*  Parser - recursive descent                                               */
/* ========================================================================= */


/**
 * @brief Peek - co následuje na kurzoru po WS?
 */
typedef enum
{
    PEEK_END    = 0,
    PEEK_LPAREN = 1,
    PEEK_RPAREN = 2,
    PEEK_BANG   = 3,
    PEEK_OR     = 4,   /**< Bare token @c "or" (lowercase, jen v závorce). */
    PEEK_LEAF   = 5
} en_PEEK;


static en_PEEK peek_next ( const char *p )
{
    while ( *p && isspace ( (unsigned char) *p ) ) p++;
    if ( *p == '\0' ) return PEEK_END;
    if ( *p == '(' )  return PEEK_LPAREN;
    if ( *p == ')' )  return PEEK_RPAREN;
    if ( *p == '!' )  return PEEK_BANG;

    /* Test na bare token "or". */
    const char *t = p;
    while ( *t && !is_token_stop ( *t ) ) t++;
    size_t tlen = (size_t) ( t - p );
    if ( tlen == 2 && p[ 0 ] == 'o' && p[ 1 ] == 'r' ) return PEEK_OR;

    return PEEK_LEAF;
}


/* Forward decls */
static st_EVFILT_NODE *parse_or_expr ( st_EVENTLOG_FILTER *f,
                                        const char **pp, bool inside_parens,
                                        unsigned temporal_depth );
static st_EVFILT_NODE *parse_and_expr ( st_EVENTLOG_FILTER *f,
                                         const char **pp, bool inside_parens,
                                         unsigned temporal_depth );
static st_EVFILT_NODE *parse_unary ( st_EVENTLOG_FILTER *f,
                                      const char **pp, bool inside_parens,
                                      unsigned temporal_depth );


/**
 * @brief Pokus o detekci a parsování @c before/after/near token.
 *
 * Test, zda kurzor začíná @c "before(" / @c "after(" / @c "near(". Pokud
 * ano, zkonzumuje keyword + window value + @c ')' + parsne reference
 * sub-expression (rekurzivně přes @c parse_unary), a vytvoří TEMPORAL_*
 * node.
 *
 * @param f               Filter handle (error reporting).
 * @param pp              Kurzor v expression bufferu.
 * @param inside_parens   Forwarded do @c parse_unary pro reference.
 * @param temporal_depth  Aktuální hloubka vnoření; pokud >=
 *                        @c EVFILT_MAX_TEMPORAL_DEPTH, parser hlásí
 *                        chybu "temporal nesting too deep".
 * @param[out] handled    @c true pokud kurzor začínal temporal keywordem
 *                        (úspěch nebo chyba). @c false pokud kurzor není
 *                        temporal (= volající má pokračovat standardním
 *                        parsem).
 * @return Vytvořený TEMPORAL_* node při úspěchu, @c NULL při chybě.
 *         @c handled rozlišuje "není temporal" vs "je temporal + chyba".
 */
static st_EVFILT_NODE *try_parse_temporal ( st_EVENTLOG_FILTER *f,
                                              const char **pp,
                                              bool inside_parens,
                                              unsigned temporal_depth,
                                              bool *handled )
{
    *handled = false;

    /* Skip WS, peek na začátek tokenu (ne přes scan_token, aby šlo
     * detekovat tvar 'before(' bez konzumace). */
    skip_ws ( pp );
    const char *p = *pp;
    if ( !p || !*p ) return NULL;

    uint8_t kind = 0;
    size_t kw_len = 0;
    if ( strncmp ( p, "before(", 7 ) == 0 ) {
        kind = EVFILT_NODE_TEMPORAL_BEFORE;
        kw_len = 7;
    } else if ( strncmp ( p, "after(", 6 ) == 0 ) {
        kind = EVFILT_NODE_TEMPORAL_AFTER;
        kw_len = 6;
    } else if ( strncmp ( p, "near(", 5 ) == 0 ) {
        kind = EVFILT_NODE_TEMPORAL_NEAR;
        kw_len = 5;
    } else {
        return NULL;  /* Not temporal. */
    }

    *handled = true;

    if ( temporal_depth >= EVFILT_MAX_TEMPORAL_DEPTH ) {
        set_err ( f, "temporal nesting too deep" );
        return NULL;
    }

    /* Konzumuj keyword + '('. */
    *pp += kw_len;

    /* Window hodnota = decimal s volitelným k/M suffixem, do ')'. */
    const char *win_start = *pp;
    const char *q = win_start;
    while ( *q && *q != ')' ) q++;
    if ( *q != ')' ) {
        set_err ( f, "temporal missing ')'" );
        return NULL;
    }
    size_t win_len = (size_t) ( q - win_start );
    if ( win_len == 0 ) {
        set_err ( f, "temporal empty window" );
        return NULL;
    }
    uint64_t window;
    if ( !parse_dec_suffixed ( win_start, win_len, &window ) ) {
        set_err ( f, "temporal invalid window value" );
        return NULL;
    }
    /* Posuň kurzor za ')'. */
    *pp = q + 1;

    /* Reference sub-expression = unary (= jeden atom / negace / vnořený
     * temporal / závorkový OR). AND chain za temporal vyhodnotí parent
     * parse_and_expr. */
    st_EVFILT_NODE *ref = parse_unary ( f, pp, inside_parens, temporal_depth + 1u );
    if ( !ref ) {
        if ( !f->has_error ) set_err ( f, "temporal missing reference" );
        return NULL;
    }

    st_EVFILT_NODE *n = make_temporal ( &f->arena, kind, window, ref );
    if ( !n ) {
        set_err ( f, "Expression too complex" );
        return NULL;
    }
    return n;
}


/**
 * @brief Parse leaf token z kurzoru (= scan + parse_token_to_leaf).
 *
 * Pro @c from_sym: speciální path: konzumuje **dva** sousední tokeny
 * @c "from_sym:A" a @c "to_sym:B" a vrátí jeden SYM_RANGE leaf.
 */
static st_EVFILT_NODE *parse_leaf_from_stream ( st_EVENTLOG_FILTER *f,
                                                  const char **pp )
{
    const char *tok;
    size_t tlen;
    scan_token ( pp, &tok, &tlen );
    if ( tlen == 0 ) {
        set_err ( f, "Expected leaf token" );
        return NULL;
    }

    /* Detekce dvojice "if" + "FIELD:VALUE" (Vlna 4 Commit 25).
     * Token "if" je samostatný keyword bez dvojtečky; musí být následován
     * druhým tokenem ve tvaru FIELD:VALUE. Speciální cesta nutná protože
     * standardní parse_token_to_leaf vyžaduje ':' uvnitř tokenu, což "if"
     * nesplňuje. */
    if ( tlen == 2 && memcmp ( tok, "if", 2 ) == 0 ) {
        const char *tok2;
        size_t tlen2;
        scan_token ( pp, &tok2, &tlen2 );
        if ( tlen2 == 0 ) {
            set_err ( f, "'if' must be followed by FIELD:VALUE" );
            return NULL;
        }
        /* Druhý token musí obsahovat ':'. */
        const char *colon = NULL;
        for ( size_t i = 0; i < tlen2; i++ ) {
            if ( tok2[ i ] == ':' ) { colon = &tok2[ i ]; break; }
        }
        if ( !colon ) {
            set_err ( f, "'if' atom missing ':'" );
            return NULL;
        }
        size_t flen = (size_t) ( colon - tok2 );
        const char *vname = colon + 1;
        size_t vlen = tlen2 - flen - 1;
        if ( flen == 0 || vlen == 0 ) {
            set_err ( f, "Empty if field or value" );
            return NULL;
        }

        st_EVFILT_NODE *node = make_leaf ( &f->arena );
        if ( !node ) {
            set_err ( f, "Expression too complex" );
            return NULL;
        }
        if ( !leaf_fill_ambient ( tok2, flen, vname, vlen,
                                    &node->u.leaf, f ) ) {
            return NULL;
        }
        return node;
    }

    /* Detekce dvojice from_sym:A to_sym:B. */
    if ( tlen > 9 && memcmp ( tok, "from_sym:", 9 ) == 0 ) {
        const char *name_a = tok + 9;
        size_t alen = tlen - 9;
        if ( alen == 0 ) {
            set_err ( f, "Empty from_sym name" );
            return NULL;
        }

        /* Sezeň druhý token. */
        const char *tok2;
        size_t tlen2;
        scan_token ( pp, &tok2, &tlen2 );
        if ( tlen2 == 0 || tlen2 < 8 || memcmp ( tok2, "to_sym:", 7 ) != 0 ) {
            set_err ( f, "'from_sym:' must be followed by 'to_sym:'" );
            return NULL;
        }
        const char *name_b = tok2 + 7;
        size_t blen = tlen2 - 7;
        if ( blen == 0 ) {
            set_err ( f, "Empty to_sym name" );
            return NULL;
        }

        st_EVFILT_NODE *node = make_leaf ( &f->arena );
        if ( !node ) {
            set_err ( f, "Expression too complex" );
            return NULL;
        }
        if ( !leaf_fill_sym_range ( name_a, alen, name_b, blen,
                                     &node->u.leaf, f ) ) {
            return NULL;
        }
        return node;
    }

    return parse_token_to_leaf ( f, tok, tlen );
}


/**
 * @brief Parse atom = "(" expr ")" | leaf_token.
 *
 * @param temporal_depth  Aktuální hloubka vnoření temporal node-ů
 *                        - propaguje se do případného temporal atomu
 *                        při parse leaf-u.
 */
static st_EVFILT_NODE *parse_atom ( st_EVENTLOG_FILTER *f,
                                      const char **pp, bool inside_parens,
                                      unsigned temporal_depth )
{
    (void) inside_parens;
    en_PEEK k = peek_next ( *pp );

    if ( k == PEEK_LPAREN ) {
        skip_ws ( pp );
        (*pp)++;                /* '(' */

        en_PEEK inner = peek_next ( *pp );
        if ( inner == PEEK_RPAREN ) {
            set_err ( f, "Empty expression in parens" );
            return NULL;
        }
        if ( inner == PEEK_END ) {
            set_err ( f, "Expected ')'" );
            return NULL;
        }

        st_EVFILT_NODE *inside = parse_or_expr ( f, pp, true, temporal_depth );
        if ( !inside ) return NULL;

        en_PEEK after = peek_next ( *pp );
        if ( after != PEEK_RPAREN ) {
            set_err ( f, "Expected ')'" );
            return NULL;
        }
        skip_ws ( pp );
        (*pp)++;                /* ')' */
        return inside;
    }

    if ( k == PEEK_LEAF ) {
        /* Pokus o detekci temporal před standardním leaf parserem.
         * before(N) / after(M) / near(K) jsou rozpoznané jako prefix
         * tokenu s '(' uvnitř - to standardní scan_token nepokryje,
         * proto own peek. */
        bool tem_handled = false;
        st_EVFILT_NODE *t = try_parse_temporal ( f, pp, inside_parens,
                                                  temporal_depth, &tem_handled );
        if ( tem_handled ) return t;  /* Pokud handled, t buď node, nebo NULL = chyba. */
        return parse_leaf_from_stream ( f, pp );
    }

    if ( k == PEEK_OR ) {
        set_err ( f, "'or' only inside parens" );
    } else if ( k == PEEK_END ) {
        set_err ( f, "Empty expression" );
    } else if ( k == PEEK_RPAREN ) {
        set_err ( f, "Unexpected ')'" );
    } else {
        set_err ( f, "Unexpected token" );
    }
    return NULL;
}


/**
 * @brief Parse unary = "!" unary | atom.
 *
 * Per-token "!" před LEAF se collapse-uje do LEAF s @c negate=true
 * pro semantickou ekvivalenci (= simplification). NOT nad temporal
 * node-em (= @c !before(...)) zachovává explicit NOT wrapper.
 */
static st_EVFILT_NODE *parse_unary ( st_EVENTLOG_FILTER *f,
                                      const char **pp, bool inside_parens,
                                      unsigned temporal_depth )
{
    en_PEEK k = peek_next ( *pp );
    if ( k == PEEK_BANG ) {
        skip_ws ( pp );
        (*pp)++;                /* '!' */

        en_PEEK after = peek_next ( *pp );
        if ( after != PEEK_LEAF && after != PEEK_LPAREN && after != PEEK_BANG ) {
            set_err ( f, "Empty token after '!'" );
            return NULL;
        }

        st_EVFILT_NODE *child = parse_unary ( f, pp, inside_parens, temporal_depth );
        if ( !child ) return NULL;

        /* Collapse !LEAF -> LEAF s flipnutým negate. */
        if ( child->kind == EVFILT_NODE_LEAF ) {
            child->u.leaf.negate = !child->u.leaf.negate;
            return child;
        }

        st_EVFILT_NODE *n = make_not ( &f->arena, child );
        if ( !n ) {
            set_err ( f, "Expression too complex" );
            return NULL;
        }
        return n;
    }
    return parse_atom ( f, pp, inside_parens, temporal_depth );
}


/**
 * @brief Parse and_expr = unary ( WS unary )*.
 *
 * Implicit AND mezi tokeny. Konec and_expr: EOF, RPAREN, "or" keyword
 * (jen pokud @c inside_parens).
 */
static st_EVFILT_NODE *parse_and_expr ( st_EVENTLOG_FILTER *f,
                                         const char **pp, bool inside_parens,
                                         unsigned temporal_depth )
{
    st_EVFILT_NODE *lhs = parse_unary ( f, pp, inside_parens, temporal_depth );
    if ( !lhs ) return NULL;

    for ( ;; ) {
        en_PEEK nxt = peek_next ( *pp );
        if ( nxt == PEEK_END || nxt == PEEK_RPAREN ) return lhs;
        if ( nxt == PEEK_OR ) {
            if ( !inside_parens ) {
                set_err ( f, "'or' only inside parens" );
                return NULL;
            }
            return lhs;
        }
        /* PEEK_LEAF / PEEK_LPAREN / PEEK_BANG = implicit AND. */
        if ( nxt != PEEK_LEAF && nxt != PEEK_LPAREN && nxt != PEEK_BANG ) {
            set_err ( f, "Unexpected token in AND chain" );
            return NULL;
        }

        st_EVFILT_NODE *rhs = parse_unary ( f, pp, inside_parens, temporal_depth );
        if ( !rhs ) return NULL;

        st_EVFILT_NODE *combined = make_bin ( &f->arena, EVFILT_NODE_AND, lhs, rhs );
        if ( !combined ) {
            set_err ( f, "Expression too complex" );
            return NULL;
        }
        lhs = combined;
    }
}


/**
 * @brief Parse or_expr = and_expr ( "or" and_expr )*.
 *
 * "or" keyword je validní jen pokud @c inside_parens. Na top-level
 * je "or" parse error (= chyba zachycena v parse_and_expr nad rámec).
 */
static st_EVFILT_NODE *parse_or_expr ( st_EVENTLOG_FILTER *f,
                                        const char **pp, bool inside_parens,
                                        unsigned temporal_depth )
{
    st_EVFILT_NODE *lhs = parse_and_expr ( f, pp, inside_parens, temporal_depth );
    if ( !lhs ) return NULL;

    for ( ;; ) {
        en_PEEK nxt = peek_next ( *pp );
        if ( nxt == PEEK_END || nxt == PEEK_RPAREN ) return lhs;
        if ( nxt != PEEK_OR ) {
            set_err ( f, "Expected 'or' or ')'" );
            return NULL;
        }
        if ( !inside_parens ) {
            set_err ( f, "'or' only inside parens" );
            return NULL;
        }

        /* Konzumuj "or" keyword (2 chars). */
        skip_ws ( pp );
        *pp += 2;

        en_PEEK after = peek_next ( *pp );
        if ( after == PEEK_END || after == PEEK_RPAREN ) {
            set_err ( f, "Empty expression after 'or'" );
            return NULL;
        }

        st_EVFILT_NODE *rhs = parse_and_expr ( f, pp, inside_parens, temporal_depth );
        if ( !rhs ) return NULL;

        st_EVFILT_NODE *combined = make_bin ( &f->arena, EVFILT_NODE_OR, lhs, rhs );
        if ( !combined ) {
            set_err ( f, "Expression too complex" );
            return NULL;
        }
        lhs = combined;
    }
}


/* ========================================================================= */
/*  Evaluator                                                                */
/* ========================================================================= */


/**
 * @brief Eval LEAF proti eventu.
 */
static bool eval_leaf ( const st_EVFILT_LEAF *leaf,
                         const st_EVENTLOG_EVENT *e )
{
    bool raw = false;

    switch ( (en_EVFILT_LEAF_KIND) leaf->kind ) {
        case EVFILT_LEAF_CAT_MASK:
            if ( e->category < 64 ) {
                raw = ( leaf->mask64 & ( 1ULL << e->category ) ) != 0;
            }
            break;
        case EVFILT_LEAF_SUB_MASK:
            raw = ( leaf->sub_mask[ e->subtype >> 6 ]
                    & ( 1ULL << ( e->subtype & 63u ) ) ) != 0;
            break;
        case EVFILT_LEAF_PC_RANGE:
            raw = ( e->pc >= leaf->rmin && e->pc <= leaf->rmax );
            break;
        case EVFILT_LEAF_FRAME_OP:
            raw = ( e->screens_total >= leaf->rmin
                    && e->screens_total <= leaf->rmax );
            break;
        case EVFILT_LEAF_CYCLE_OP:
            raw = ( e->pxclk_total >= leaf->rmin
                    && e->pxclk_total <= leaf->rmax );
            break;
        case EVFILT_LEAF_SLINE_RANGE: {
            uint32_t w = g_evfilt_screen_width ? g_evfilt_screen_width : 320u;
            uint64_t sline = e->pxclk_in_screen / w;
            raw = ( sline >= leaf->rmin && sline <= leaf->rmax );
            break;
        }
        case EVFILT_LEAF_PX_RANGE: {
            uint32_t w = g_evfilt_screen_width ? g_evfilt_screen_width : 320u;
            uint64_t px = e->pxclk_in_screen % w;
            raw = ( px >= leaf->rmin && px <= leaf->rmax );
            break;
        }
        case EVFILT_LEAF_PAYLOAD_EQ:
            raw = ( (uint64_t) e->payload == leaf->rmin );
            break;
        case EVFILT_LEAF_SYM_EXACT: {
            /* Pre-resolved cache (Commit 19) - O(1) compare. Pokud byl
             * symbol v parse stale (= jméno neexistuje v sym_db),
             * cached_valid_a == false a match vrací vždy false. */
            if ( leaf->sym_cached_valid_a ) {
                raw = ( (uint16_t) e->pc == leaf->sym_cached_addr_a );
            }
            break;
        }
        case EVFILT_LEAF_SYM_PREFIX: {
            /* Cache byla pre-resolved při parse. NULL/0 = no match. */
            if ( leaf->sym_cached_addrs && leaf->sym_cached_count > 0 ) {
                for ( size_t i = 0; i < leaf->sym_cached_count; i++ ) {
                    if ( (uint16_t) e->pc == leaf->sym_cached_addrs[ i ] ) {
                        raw = true;
                        break;
                    }
                }
            }
            break;
        }
        case EVFILT_LEAF_SYM_RANGE: {
            /* Pre-resolved cache (Commit 19) - O(1) range compare.
             * Vyžaduje obě jména rozpoznaná v parse. */
            if ( leaf->sym_cached_valid_a && leaf->sym_cached_valid_b ) {
                uint16_t lo = leaf->sym_cached_addr_a;
                uint16_t hi = leaf->sym_cached_addr_b;
                if ( lo > hi ) {
                    /* Tolerance: prohození pokud user zadal opačně. */
                    uint16_t t = lo; lo = hi; hi = t;
                }
                raw = ( e->pc >= lo && e->pc <= hi );
            }
            break;
        }
        case EVFILT_LEAF_AMBIENT_BIT: {
            /* @c if iff1:N - jednobitový test. amb_expected == 0 znamená
             * "bit musí být cleared", != 0 znamená "bit musí být set". */
            if ( leaf->amb_expected != 0 ) {
                raw = ( ( e->ambient & leaf->amb_mask ) != 0 );
            } else {
                raw = ( ( e->ambient & leaf->amb_mask ) == 0 );
            }
            break;
        }
        case EVFILT_LEAF_AMBIENT_FIELD: {
            /* @c if im/reason/banking:N - více-bitový field. */
            raw = ( ( e->ambient & leaf->amb_mask ) == leaf->amb_expected );
            break;
        }
    }

    return leaf->negate ? !raw : raw;
}


/**
 * @brief Helper: vrátí ukazatel na N-tý event ve scope kontextu.
 *
 * Pokud @c ctx->events je @c NULL, deleguje na @ref eventlog_get_event().
 * Jinak indexuje pole @c ctx->events. Při @c idx >= @c ctx->count vrátí
 * @c NULL.
 */
static const st_EVENTLOG_EVENT *ctx_event_at ( const st_EVENTLOG_FILTER_CTX *ctx,
                                                 size_t idx )
{
    if ( !ctx || idx >= ctx->count ) return NULL;
    if ( ctx->events ) return &ctx->events[ idx ];
    return eventlog_get_event ( idx );
}


/* Forward declaration pro vzájemnou rekurzi temporal <-> eval_node. */
static bool eval_node ( const st_EVFILT_NODE *n,
                          const st_EVENTLOG_EVENT *e,
                          const st_EVENTLOG_FILTER_CTX *ctx );


/**
 * @brief Eval TEMPORAL_BEFORE/AFTER/NEAR proti eventu @c e.
 *
 * @c e splňuje @c "before(W) ref" pokud existuje reference event @c r
 * v @c ctx splňující @c ref filter, kde:
 *   - BEFORE: @c e.pxclk_total <= r.pxclk_total a (r-e) <= W
 *   - AFTER:  @c e.pxclk_total >= r.pxclk_total a (e-r) <= W
 *   - NEAR:   |e.pxclk_total - r.pxclk_total| <= W
 *
 * Komplexita: O(N) per event přes scope @c ctx (N = @c ctx->count). Per
 * UI render frame s N display events a R reference matches je celkem
 * O(N*R). Pro N=R=5000 / W=1000 pxclk to znamená ~25M ops na frame, což
 * není při 60 fps udržitelné. Realistický load (display po předfiltru
 * cat:, R~100) dává ~500k ops/frame = jednotky ms. Optimalizace přes
 * pre-resolved reference timestamp cache je follow-up commit.
 *
 * Pokud @c ctx == @c NULL, vrátí false (= temporal vyžaduje kontext).
 */
static bool eval_temporal ( const st_EVFILT_NODE *n,
                              const st_EVENTLOG_EVENT *e,
                              const st_EVENTLOG_FILTER_CTX *ctx )
{
    if ( !n || !e ) return false;
    if ( !ctx ) return false;

    uint64_t W = n->u.temporal.window;
    const st_EVFILT_NODE *ref = n->u.temporal.reference;

    for ( size_t j = 0; j < ctx->count; j++ ) {
        const st_EVENTLOG_EVENT *r = ctx_event_at ( ctx, j );
        if ( !r ) continue;

        uint64_t et = e->pxclk_total;
        uint64_t rt = r->pxclk_total;
        uint64_t diff;
        bool in_window = false;

        switch ( (en_EVFILT_NODE_KIND) n->kind ) {
            case EVFILT_NODE_TEMPORAL_BEFORE:
                /* e předchází (nebo se rovná) r v čase. */
                if ( et > rt ) continue;
                diff = rt - et;
                in_window = ( diff <= W );
                break;
            case EVFILT_NODE_TEMPORAL_AFTER:
                /* e následuje (nebo se rovná) r v čase. */
                if ( et < rt ) continue;
                diff = et - rt;
                in_window = ( diff <= W );
                break;
            case EVFILT_NODE_TEMPORAL_NEAR:
                diff = ( et >= rt ) ? ( et - rt ) : ( rt - et );
                in_window = ( diff <= W );
                break;
            default:
                return false;
        }
        if ( !in_window ) continue;
        if ( eval_node ( ref, r, ctx ) ) return true;
    }
    return false;
}


/**
 * @brief Rekurzivní eval AST s volitelným ring kontextem.
 *
 * Pro non-temporal node-y je @c ctx ignorován. Pro temporal node-y
 * vyžaduje @c ctx != @c NULL (jinak vrací @c false).
 */
static bool eval_node ( const st_EVFILT_NODE *n,
                          const st_EVENTLOG_EVENT *e,
                          const st_EVENTLOG_FILTER_CTX *ctx )
{
    if ( !n ) return true;
    switch ( (en_EVFILT_NODE_KIND) n->kind ) {
        case EVFILT_NODE_LEAF: return eval_leaf ( &n->u.leaf, e );
        case EVFILT_NODE_NOT:  return !eval_node ( n->u.child, e, ctx );
        case EVFILT_NODE_AND:
            if ( !eval_node ( n->u.bin.l, e, ctx ) ) return false;
            return eval_node ( n->u.bin.r, e, ctx );
        case EVFILT_NODE_OR:
            if ( eval_node ( n->u.bin.l, e, ctx ) ) return true;
            return eval_node ( n->u.bin.r, e, ctx );
        case EVFILT_NODE_TEMPORAL_BEFORE:
        case EVFILT_NODE_TEMPORAL_AFTER:
        case EVFILT_NODE_TEMPORAL_NEAR:
            return eval_temporal ( n, e, ctx );
    }
    return false;
}


/**
 * @brief Rekurzivní detekce, zda AST obsahuje temporal node.
 */
static bool node_has_temporal ( const st_EVFILT_NODE *n )
{
    if ( !n ) return false;
    if ( is_temporal_kind ( n->kind ) ) return true;
    switch ( (en_EVFILT_NODE_KIND) n->kind ) {
        case EVFILT_NODE_LEAF: return false;
        case EVFILT_NODE_NOT:  return node_has_temporal ( n->u.child );
        case EVFILT_NODE_AND:
        case EVFILT_NODE_OR:
            return node_has_temporal ( n->u.bin.l )
                   || node_has_temporal ( n->u.bin.r );
        case EVFILT_NODE_TEMPORAL_BEFORE:
        case EVFILT_NODE_TEMPORAL_AFTER:
        case EVFILT_NODE_TEMPORAL_NEAR:
            /* Už zachyceno v is_temporal_kind() check výše. Sub-tree může
             * obsahovat další temporal - true se vrátí už nahoře. */
            return true;
    }
    return false;
}


/* ========================================================================= */
/*  AST -> string (round trip)                                               */
/* ========================================================================= */


/**
 * @brief Dynamický string buffer pro to_string.
 */
typedef struct
{
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} st_EVFILT_STR;


static void str_init ( st_EVFILT_STR *s )
{
    s->cap = 64;
    s->len = 0;
    s->oom = false;
    s->buf = (char *) malloc ( s->cap );
    if ( !s->buf ) { s->oom = true; s->cap = 0; return; }
    s->buf[ 0 ] = '\0';
}


static void str_append ( st_EVFILT_STR *s, const char *t )
{
    if ( s->oom || !t ) return;
    size_t tlen = strlen ( t );
    if ( s->len + tlen + 1 > s->cap ) {
        size_t ncap = s->cap;
        while ( s->len + tlen + 1 > ncap ) ncap *= 2;
        char *nb = (char *) realloc ( s->buf, ncap );
        if ( !nb ) { s->oom = true; return; }
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy ( s->buf + s->len, t, tlen );
    s->len += tlen;
    s->buf[ s->len ] = '\0';
}


static void str_appendf ( st_EVFILT_STR *s, const char *fmt, ... )
{
    char tmp[ 64 ];
    va_list ap;
    va_start ( ap, fmt );
    vsnprintf ( tmp, sizeof ( tmp ), fmt, ap );
    va_end ( ap );
    str_append ( s, tmp );
}


/**
 * @brief Dump LEAF do textové reprezentace.
 */
static void dump_leaf ( const st_EVFILT_LEAF *leaf, st_EVFILT_STR *s )
{
    if ( leaf->negate ) str_append ( s, "!" );

    switch ( (en_EVFILT_LEAF_KIND) leaf->kind ) {
        case EVFILT_LEAF_CAT_MASK: {
            str_append ( s, "cat:" );
            bool first = true;
            for ( unsigned i = 0; i < 64; i++ ) {
                if ( leaf->mask64 & ( 1ULL << i ) ) {
                    const char *nm = eventlog_filter_cat_to_name ( (uint8_t) i );
                    if ( !nm ) continue;
                    if ( !first ) str_append ( s, "," );
                    str_append ( s, nm );
                    first = false;
                }
            }
            break;
        }
        case EVFILT_LEAF_SUB_MASK: {
            str_append ( s, "sub:" );
            bool first = true;
            for ( unsigned i = 0; i < 256; i++ ) {
                if ( leaf->sub_mask[ i >> 6 ] & ( 1ULL << ( i & 63u ) ) ) {
                    if ( !first ) str_append ( s, "," );
                    str_appendf ( s, "%u", i );
                    first = false;
                }
            }
            break;
        }
        case EVFILT_LEAF_PC_RANGE:
            if ( leaf->rmin == leaf->rmax ) {
                str_appendf ( s, "pc:%lX", (unsigned long) leaf->rmin );
            } else {
                str_appendf ( s, "pc:%lX-%lX",
                              (unsigned long) leaf->rmin,
                              (unsigned long) leaf->rmax );
            }
            break;
        case EVFILT_LEAF_FRAME_OP:
            if ( leaf->rmax == UINT64_MAX ) {
                /* >N: rmin = N+1 -> dump jako >N. */
                if ( leaf->rmin == 0 ) {
                    str_append ( s, "frame:>0" );
                } else {
                    str_appendf ( s, "frame:>%llu",
                                  (unsigned long long) ( leaf->rmin - 1 ) );
                }
            } else if ( leaf->rmin == 0 && leaf->rmax + 1 != 0 ) {
                /* <N: rmin=0, rmax=N-1 -> rmax+1 = N. Single 0 case
                 * (rmax=0) dumpneme jako "<1" pro round-trip. */
                str_appendf ( s, "frame:<%llu",
                              (unsigned long long) ( leaf->rmax + 1 ) );
            } else if ( leaf->rmin == leaf->rmax ) {
                str_appendf ( s, "frame:%llu",
                              (unsigned long long) leaf->rmin );
            } else {
                /* Range A-B - dump jako N-N (parser to nepodporuje
                 * pro frame, ale pro symetrii spec.). Fallback: pokud
                 * by to byla anomálie, použij explicit hodnoty. */
                str_appendf ( s, "frame:%llu",
                              (unsigned long long) leaf->rmin );
            }
            break;
        case EVFILT_LEAF_CYCLE_OP:
            if ( leaf->rmax == UINT64_MAX ) {
                str_appendf ( s, "cycle:>%llu",
                              (unsigned long long) ( leaf->rmin - 1 ) );
            } else if ( leaf->rmin == 0 && leaf->rmax + 1 != 0 ) {
                str_appendf ( s, "cycle:<%llu",
                              (unsigned long long) ( leaf->rmax + 1 ) );
            } else if ( leaf->rmin == leaf->rmax ) {
                str_appendf ( s, "cycle:%llu",
                              (unsigned long long) leaf->rmin );
            } else {
                str_appendf ( s, "cycle:%llu-%llu",
                              (unsigned long long) leaf->rmin,
                              (unsigned long long) leaf->rmax );
            }
            break;
        case EVFILT_LEAF_SLINE_RANGE:
            if ( leaf->rmin == leaf->rmax ) {
                str_appendf ( s, "sline:%llu",
                              (unsigned long long) leaf->rmin );
            } else {
                str_appendf ( s, "sline:%llu-%llu",
                              (unsigned long long) leaf->rmin,
                              (unsigned long long) leaf->rmax );
            }
            break;
        case EVFILT_LEAF_PX_RANGE:
            if ( leaf->rmin == leaf->rmax ) {
                str_appendf ( s, "px:%llu",
                              (unsigned long long) leaf->rmin );
            } else {
                str_appendf ( s, "px:%llu-%llu",
                              (unsigned long long) leaf->rmin,
                              (unsigned long long) leaf->rmax );
            }
            break;
        case EVFILT_LEAF_PAYLOAD_EQ:
            str_appendf ( s, "payload:%lX", (unsigned long) leaf->rmin );
            break;
        case EVFILT_LEAF_SYM_EXACT:
            str_append ( s, "sym:" );
            if ( leaf->sym_name_a ) str_append ( s, leaf->sym_name_a );
            break;
        case EVFILT_LEAF_SYM_PREFIX:
            str_append ( s, "sym:" );
            if ( leaf->sym_name_a ) str_append ( s, leaf->sym_name_a );
            str_append ( s, "*" );
            break;
        case EVFILT_LEAF_SYM_RANGE:
            str_append ( s, "from_sym:" );
            if ( leaf->sym_name_a ) str_append ( s, leaf->sym_name_a );
            str_append ( s, " to_sym:" );
            if ( leaf->sym_name_b ) str_append ( s, leaf->sym_name_b );
            break;
        case EVFILT_LEAF_AMBIENT_BIT: {
            /* if iff1:0 / if iff1:1 - jediný field v BIT kategorii. */
            unsigned bit = ( leaf->amb_expected != 0 ) ? 1u : 0u;
            if ( leaf->amb_field == EVFILT_AMBIENT_FIELD_IFF1 ) {
                str_appendf ( s, "if iff1:%u", bit );
            }
            break;
        }
        case EVFILT_LEAF_AMBIENT_FIELD: {
            switch ( leaf->amb_field ) {
                case EVFILT_AMBIENT_FIELD_IM: {
                    unsigned im = ( leaf->amb_expected & EVENTLOG_AMBIENT_IM_MASK )
                                  >> EVENTLOG_AMBIENT_IM_SHIFT;
                    str_appendf ( s, "if im:%u", im );
                    break;
                }
                case EVFILT_AMBIENT_FIELD_REASON: {
                    unsigned code = ( leaf->amb_expected & EVENTLOG_AMBIENT_REASON_MASK )
                                    >> EVENTLOG_AMBIENT_REASON_SHIFT;
                    const char *nm = eventlog_filter_reason_code_to_name ( (uint8_t) code );
                    str_appendf ( s, "if reason:%s", nm ? nm : "?" );
                    break;
                }
                case EVFILT_AMBIENT_FIELD_BANKING: {
                    unsigned code = ( leaf->amb_expected & EVENTLOG_AMBIENT_BANKING_MASK )
                                    >> EVENTLOG_AMBIENT_BANKING_SHIFT;
                    const char *nm = eventlog_filter_banking_code_to_name ( (uint8_t) code );
                    str_appendf ( s, "if banking:%s", nm ? nm : "?" );
                    break;
                }
                default: break;
            }
            break;
        }
    }
}


/**
 * @brief Dump AST rekurzivně.
 *
 * Binární operátory jsou v závorkách pro jednoznačnost (= round-trip).
 */
static void dump_node ( const st_EVFILT_NODE *n, st_EVFILT_STR *s )
{
    if ( !n ) return;
    switch ( (en_EVFILT_NODE_KIND) n->kind ) {
        case EVFILT_NODE_LEAF:
            dump_leaf ( &n->u.leaf, s );
            break;
        case EVFILT_NODE_NOT:
            str_append ( s, "!(" );
            dump_node ( n->u.child, s );
            str_append ( s, ")" );
            break;
        case EVFILT_NODE_AND:
            str_append ( s, "( " );
            dump_node ( n->u.bin.l, s );
            str_append ( s, " " );
            dump_node ( n->u.bin.r, s );
            str_append ( s, " )" );
            break;
        case EVFILT_NODE_OR:
            str_append ( s, "( " );
            dump_node ( n->u.bin.l, s );
            str_append ( s, " or " );
            dump_node ( n->u.bin.r, s );
            str_append ( s, " )" );
            break;
        case EVFILT_NODE_TEMPORAL_BEFORE:
        case EVFILT_NODE_TEMPORAL_AFTER:
        case EVFILT_NODE_TEMPORAL_NEAR: {
            const char *kw = "before";
            if ( n->kind == EVFILT_NODE_TEMPORAL_AFTER ) kw = "after";
            else if ( n->kind == EVFILT_NODE_TEMPORAL_NEAR ) kw = "near";
            str_appendf ( s, "%s(%llu) ", kw,
                          (unsigned long long) n->u.temporal.window );
            dump_node ( n->u.temporal.reference, s );
            break;
        }
    }
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */


/**
 * @brief Počet živých filtrů (parse bez odpovídajícího free).
 *
 * Diagnostika pro testy vlastnictví (@ref eventlog_filter_live_count).
 * Mění se atomicky, parse / free smí běžet na různých vláknech.
 */
static gint s_live_count = 0;


int eventlog_filter_live_count ( void )
{
    return (int) g_atomic_int_get ( &s_live_count );
}


st_EVENTLOG_FILTER *eventlog_filter_parse ( const char *expr )
{
    /* Délka overflow check. */
    if ( expr ) {
        size_t elen = strnlen ( expr, EVENTLOG_FILTER_MAX_EXPR_LEN + 1 );
        if ( elen > EVENTLOG_FILTER_MAX_EXPR_LEN ) return NULL;
    }

    st_EVENTLOG_FILTER *f = (st_EVENTLOG_FILTER *) malloc ( sizeof ( *f ) );
    if ( !f ) return NULL;
    f->root = NULL;
    f->err[ 0 ] = '\0';
    f->has_error = false;
    f->sym_leaves = NULL;
    f->sym_leaves_count = 0;
    f->sym_leaves_cap = 0;
    f->arena.buf = (uint8_t *) malloc ( EVFILT_ARENA_CAP );
    if ( !f->arena.buf ) {
        free ( f );
        return NULL;
    }
    f->arena.used = 0;
    f->arena.cap = EVFILT_ARENA_CAP;
    f->arena.node_count = 0;
    g_atomic_int_inc ( &s_live_count );

    if ( !expr || !expr[ 0 ] ) return f;  /* match all */

    const char *p = expr;
    skip_ws ( &p );
    if ( *p == '\0' ) return f;  /* whitespace-only = match all */

    st_EVFILT_NODE *root = parse_or_expr ( f, &p, false, 0u );
    if ( !root ) {
        /* Error již nastaven setterem. */
        return f;
    }

    skip_ws ( &p );
    if ( *p != '\0' ) {
        if ( *p == ')' ) {
            set_err ( f, "Unexpected ')'" );
        } else {
            set_err ( f, "Unexpected trailing token" );
        }
        return f;
    }

    f->root = root;
    return f;
}


void eventlog_filter_free ( st_EVENTLOG_FILTER *f )
{
    if ( !f ) return;
    /* Uvolnit heap allocs uvnitř SYM leaf-ů (pole jmen + cache). Iterace
     * jde přes tracked list, ne AST - pokrýváme i leaf-y, na něž root
     * neukazuje (parse error path). */
    for ( size_t i = 0; i < f->sym_leaves_count; i++ ) {
        st_EVFILT_LEAF *leaf = f->sym_leaves[ i ];
        if ( !leaf ) continue;
        free ( leaf->sym_name_a );
        free ( leaf->sym_name_b );
        free ( leaf->sym_cached_addrs );
        leaf->sym_name_a = NULL;
        leaf->sym_name_b = NULL;
        leaf->sym_cached_addrs = NULL;
    }
    free ( f->sym_leaves );
    free ( f->arena.buf );
    free ( f );
    g_atomic_int_add ( &s_live_count, -1 );
}


bool eventlog_filter_match_ctx ( const st_EVENTLOG_FILTER *f,
                                  const st_EVENTLOG_EVENT *e,
                                  const st_EVENTLOG_FILTER_CTX *ctx )
{
    if ( !f || !e ) return false;
    if ( f->has_error ) return false;
    return eval_node ( f->root, e, ctx );
}


bool eventlog_filter_match ( const st_EVENTLOG_FILTER *f,
                              const st_EVENTLOG_EVENT *e )
{
    /* Shim s ctx=NULL. Non-temporal filtry fungují shodně s legacy API.
     * Temporal filter bez ctx vrací false (= eval_temporal selže na
     * NULL ctx check). */
    return eventlog_filter_match_ctx ( f, e, NULL );
}


bool eventlog_filter_has_temporal ( const st_EVENTLOG_FILTER *f )
{
    if ( !f || f->has_error ) return false;
    return node_has_temporal ( f->root );
}


const char *eventlog_filter_get_error ( const st_EVENTLOG_FILTER *f )
{
    if ( !f ) return NULL;
    if ( !f->has_error ) return NULL;
    return f->err;
}


char *eventlog_filter_to_string ( const st_EVENTLOG_FILTER *f )
{
    if ( !f || f->has_error ) return NULL;
    st_EVFILT_STR s;
    str_init ( &s );
    if ( s.oom ) return NULL;
    if ( f->root ) {
        dump_node ( f->root, &s );
    }
    if ( s.oom ) {
        free ( s.buf );
        return NULL;
    }
    return s.buf;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
