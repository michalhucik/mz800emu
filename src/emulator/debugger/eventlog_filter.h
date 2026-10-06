/**
 * @file   eventlog_filter.h
 * @brief  Filter parser + evaluátor pro Event Viewer (Vlna 1 Commit 6).
 *
 * Samostatný filter pro Events okno (= Log tab + Strip tab v dalších
 * commitech). NEsdílí kód s @c io_history_filter (= io_window History
 * tab beze změny), jen replikuje pattern: tokenizovaná syntax s AND
 * default, OR groups přes závorky, per-token negace prefixem @c '!'.
 *
 * Filter pracuje nad @ref st_EVENTLOG_EVENT (definováno v
 * @c debugger/trace/eventlog.h). Z importu závisí výhradně tato hlavička
 * - žádné další emulator subsystémy (= filter je čistě analytická
 * vrstva nad ringem).
 *
 * @section grammar Gramatika (BNF)
 *
 * @code
 *   expr     = or_expr
 *   or_expr  = and_expr ( "or" and_expr )*       ; "or" jen uvnitř závorek
 *   and_expr = unary    ( WS unary )*            ; mezera = AND
 *   unary    = "!" unary | atom
 *   atom     = "(" expr ")" | leaf_token
 *   leaf_token =
 *       "cat:"      name_list
 *     | "sub:"      num_list
 *     | "pc:"       hex_or_range
 *     | "frame:"    dec_or_op
 *     | "cycle:"    dec_or_op_suffixed
 *     | "sline:"    dec_or_range
 *     | "px:"       dec_or_range
 *     | "payload:"  hex_value
 *     | "sym:"      ident_or_prefix
 *     | "from_sym:" ident WS "to_sym:" ident
 *     | "if"        WS ambient_atom
 *     | temporal_atom
 *
 *   temporal_atom =
 *       "before(" dec_suffixed ")" WS unary
 *     | "after("  dec_suffixed ")" WS unary
 *     | "near("   dec_suffixed ")" WS unary
 * @endcode
 *
 * @section operators Operátory v sestupné precedenci
 *
 * 1. @c "("  @c ")"  grouping
 * 2. @c "!"          unární negace (per-token nebo nad závorkou)
 * 3. whitespace      implicit AND mezi tokeny
 * 4. @c "or"         OR (jen uvnitř závorek - viz níže)
 *
 * Klíčové slovo @c "or" je rezervované JEN uvnitř explicitní závorky.
 * Na top-level je @c "or" parse error (= uživatel musí napsat
 * @c "( a or b )" pro OR group). Tento záměr zjednodušuje syntax a
 * vyhne se ambivalentní precedenci s implicit AND.
 *
 * @section leaf_syntax Per-token detaily
 *
 * | Prefix      | Hodnota                    | Příklad                  |
 * |-------------|----------------------------|--------------------------|
 * | @c cat:     | name[,name]*               | @c cat:cpu_int,gdg_mode  |
 * | @c sub:     | num[,num]* (0..255)        | @c sub:0,1               |
 * | @c pc:      | hex nebo hex-hex           | @c pc:4000-40FF          |
 * | @c frame:   | dec, @c >N nebo @c <N      | @c frame:>100            |
 * | @c cycle:   | dec/range/op, @c k/M sufix | @c cycle:>1M             |
 * | @c sline:   | dec nebo dec-dec           | @c sline:50-150          |
 * | @c px:      | dec nebo dec-dec           | @c px:160-200            |
 * | @c payload: | hex (@c 0x prefix volit.)  | @c payload:0xCE          |
 * | @c sym:     | identifier nebo prefix*    | @c sym:isr_handler       |
 * | @c sym:     | (PREFIX*)                  | @c sym:isr_*             |
 * | @c from_sym:| identifier (vyžaduje to_sym)| @c from_sym:a to_sym:b  |
 *
 * Jména kategorií jsou case-sensitive, lowercase, mapování v
 * @ref eventlog_filter_cat_from_name(). Subtype hodnoty jsou
 * surová čísla bez jména (= per-kategorie význam, UI dekóduje samo).
 *
 * @section examples Příklady
 *
 * - @c cat:cpu_int                      = jen CPU_INT eventy
 * - @c "cat:cpu_int pc:4000-40FF"       = CPU_INT a PC v range (AND)
 * - @c "( cat:cpu_int or cat:bp_fire )" = CPU_INT NEBO BP_FIRE
 * - @c !cat:gdg_video                   = vše KROMĚ GDG_VIDEO
 * - @c "frame:>100 cycle:>1M"           = po 100. snímku a 1 mil. cyklu
 * - @c sym:print_char                   = PC == addr symbolu @c print_char
 * - @c sym:isr_*                        = PC == addr jakéhokoliv @c isr_* symbolu
 * - @c "from_sym:start to_sym:end"      = PC v [start.addr, end.addr]
 * - @c !sym:isr_*                       = vše KROMĚ adres @c isr_* symbolů
 *
 * @section sym_semantics Symbol filtry - sémantika a caching
 *
 * Symboly v @c sym_db jsou bodové (= jedna adresa), ne range. @c sym:NAME
 * vrací match pouze pokud @c e->pc == @c sym.addr (= exact). Pro range
 * scan použít @c from_sym:A @c to_sym:B (= inkluzivní rozsah adres).
 *
 * Při @c parse() se pro @c sym:PREFIX_* pre-resolve seznam matching
 * adres z aktuálního @c sym_db stavu (= rychlý matcher per event). Pokud
 * uživatel později načte další .lbl / přidá user label, cache je
 * **stale** dokud uživatel re-parse filter (= edit textboxu).
 *
 * Pokud @c sym:NAME nebo @c from_sym:/to_sym: jméno v DB neexistuje,
 * leaf vrací vždy @c false (= žádný event nematchne).
 *
 * @section ownership Ownership / lifetime
 *
 * Filter alokuje volající přes @ref eventlog_filter_parse() a uvolňuje
 * přes @ref eventlog_filter_free(). Empty / @c NULL vstup vrátí "match
 * all" filter (= non-NULL handle s prázdným AST). Syntax error vrátí
 * non-NULL handle s nastaveným error stringem (volat
 * @ref eventlog_filter_get_error()).
 *
 * @section thread_safety Thread safety
 *
 * Filter struktura patří tomu, kdo ji naparsoval, dokud ji nepředá:
 * filtr Log tabu okna Events drží UI vlákno, filtry triggerů Pause /
 * Auto-mark on match převezme a uvolňuje modul @c eventlog_trigger.c
 * (emu vlákno, viz @c eventlog_trigger.h). Není thread-safe pro
 * paralelní mutaci ani pro uvolnění souběžné s vyhodnocením. Volání
 * @ref eventlog_filter_match() je read-only a může běžet z UI nebo emu
 * vlákna (matcher nemění AST).
 *
 * Licence: GPLv3
 */

#ifndef EVENTLOG_FILTER_H
#define EVENTLOG_FILTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "trace/eventlog.h"

#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief Opaque handle držící AST + arenu + error string.
 *
 * Layout v @c eventlog_filter.c. Caller pracuje výhradně přes API
 * funkce níže.
 *
 * Invariant: po úspěšném @ref eventlog_filter_parse() handle drží
 * platný AST a @ref eventlog_filter_get_error() vrací @c NULL. Po
 * chybě parse vrátí NEnull handle s nastaveným error stringem (=
 * @c parse vrací NULL při OOM, jinak handle).
 */
typedef struct st_EVENTLOG_FILTER st_EVENTLOG_FILTER;


/**
 * @brief Parse filter expression do AST.
 *
 * Při @c NULL nebo prázdném @c expr vrátí valid handle reprezentující
 * "match all" (= @ref eventlog_filter_match() vždy @c true).
 *
 * Při syntax chybě vrátí NEnull handle, ale @ref eventlog_filter_match()
 * vrací vždy @c false (= žádný event neprojde). Error popis je
 * dostupný přes @ref eventlog_filter_get_error().
 *
 * @c NULL je vrácen pouze při OOM nebo když volající poslal příliš
 * dlouhý filter string (limit @c EVENTLOG_FILTER_MAX_EXPR_LEN).
 *
 * @param expr  Filter expression (@c NULL nebo @c "" = match all).
 * @return Nová filter struktura. Volající musí zavolat
 *         @ref eventlog_filter_free(). @c NULL při OOM.
 */
st_EVENTLOG_FILTER *eventlog_filter_parse ( const char *expr );


/**
 * @brief Uvolnit filter strukturu.
 *
 * No-op pokud @c f je @c NULL.
 *
 * @param f  Filter z @ref eventlog_filter_parse() (může být @c NULL).
 */
void eventlog_filter_free ( st_EVENTLOG_FILTER *f );


/**
 * @brief Počet živých filtrů v procesu (úspěšné parse minus free).
 *
 * Diagnostika pro testy vlastnictví filtrů předávaných mezi vlákny
 * (triggery okna Events): po dokončené operaci musí počet odpovídat
 * filtrům, které někdo legitimně drží. Čítač se mění atomicky.
 *
 * @return Aktuální počet živých filtrů (>= 0 při korektním použití).
 *
 * Smí volat libovolné vlákno.
 */
int eventlog_filter_live_count ( void );


/**
 * @brief Ring kontext pro temporal eval (Vlna 4 Commit 26).
 *
 * Temporal filtry (@c before(N) / @c after(M) / @c near(K)) potřebují
 * pro vyhodnocení match-u jednoho eventu **přístup ke všem ostatním
 * eventům v ringu**, aby vyhledaly reference events v zadaném časovém
 * okně. Tento kontext nabízí volajícímu prostor předat ring snapshot
 * do matcheru.
 *
 * Politika lookupu eventů:
 *  - @c events != @c NULL: matcher iteruje přes pole
 *    @c events[0..count-1] (= caller poskytl vlastní snapshot).
 *  - @c events == @c NULL: matcher volá @ref eventlog_get_event() pro
 *    indexy @c [0..count-1] (= UI typický případ, ring je read-only
 *    z UI vlákna).
 *
 * @field events  Volitelné pole eventů pro matching scope. @c NULL
 *                znamená použít @ref eventlog_get_event() přes
 *                veřejné API.
 * @field count   Počet eventů ve scopu. Pro UI typicky
 *                @ref eventlog_get_count().
 *
 * @note Filter s temporal node-em volaný přes
 *       @ref eventlog_filter_match() bez ctx (= shim s ctx=NULL)
 *       vrací vždy @c false (= temporal vyžaduje kontext).
 *       Non-temporal filtry shimu fungují beze změny.
 */
typedef struct
{
    const st_EVENTLOG_EVENT *events;
    size_t                   count;
} st_EVENTLOG_FILTER_CTX;


/**
 * @brief Test zda event splňuje filter podmínku (s ring kontextem).
 *
 * Rekurzivní eval AST se short-circuit AND/OR. Pokud filter obsahuje
 * temporal node, musí být poskytnut @c ctx != @c NULL s validním
 * scope - jinak match selže (= vrátí @c false). Pro non-temporal
 * filtry je @c ctx ignorován.
 *
 * Návratová hodnota:
 *  - @c true při match (= zobraz event v UI).
 *  - @c false při no-match, syntax error, nebo temporal bez ctx.
 *  - @c false pokud @c f nebo @c e je @c NULL.
 *
 * @param f    Parsovaný filter (@c NULL = false).
 * @param e    Event z ringu (@c NULL = false).
 * @param ctx  Volitelný ring kontext pro temporal eval (@c NULL =
 *             bez kontextu, temporal vrací false).
 * @return @c true / @c false podle pravidel výše.
 */
bool eventlog_filter_match_ctx ( const st_EVENTLOG_FILTER *f,
                                  const st_EVENTLOG_EVENT *e,
                                  const st_EVENTLOG_FILTER_CTX *ctx );


/**
 * @brief Test zda event splňuje filter podmínku (bez kontextu).
 *
 * Backward-compat shim nad @ref eventlog_filter_match_ctx() s
 * @c ctx=NULL. Pro non-temporal filtry funguje shodně s legacy
 * API. Pro filter s temporal node-em vrací vždy @c false (= caller
 * musí použít @ref eventlog_filter_match_ctx() s validním ctx).
 *
 * @param f  Parsovaný filter (@c NULL = false).
 * @param e  Event z ringu (@c NULL = false).
 * @return @c true při match, @c false jinak.
 */
bool eventlog_filter_match ( const st_EVENTLOG_FILTER *f,
                              const st_EVENTLOG_EVENT *e );


/**
 * @brief Zjisti, zda filter obsahuje temporal node (= vyžaduje ctx).
 *
 * UI helper pro detekci, kdy je nutné poskytnout
 * @ref st_EVENTLOG_FILTER_CTX. Vrací @c false pro NULL filter,
 * filter s parse error, nebo prázdný (match-all) filter.
 *
 * @param f  Parsovaný filter (@c NULL = false).
 * @return @c true pokud strom obsahuje aspoň jeden temporal node.
 */
bool eventlog_filter_has_temporal ( const st_EVENTLOG_FILTER *f );


/**
 * @brief Vrátí error message z poslední @ref eventlog_filter_parse().
 *
 * @param f  Filter handle (@c NULL = vrátí @c NULL).
 * @return Pointer na statický error string vlastněný filterem
 *         (lifetime stejný jako @c f), nebo @c NULL pokud parse
 *         proběhl bez chyby.
 */
const char *eventlog_filter_get_error ( const st_EVENTLOG_FILTER *f );


/**
 * @brief Zpětný dump AST na string (= "save preset" pro UI).
 *
 * Generuje canonical string reprezentaci AST. Round-trip platí:
 * @c parse(to_string(parse(X))) má sémanticky shodné chování s
 * @c parse(X) (= match-equivalent na všech eventech). Textová podoba
 * MŮŽE být jiná (= závorky kolem všech OR groups jsou explicitní,
 * AND je vždy mezerou, negace je @c '!' bez mezery).
 *
 * @param f  Filter handle (@c NULL = vrátí @c NULL).
 * @return Heap-alokovaný null-terminated string vlastněný volajícím
 *         (musí volat @c free()). @c NULL při OOM nebo @c f == @c NULL
 *         nebo filter s chybou parsování.
 */
char *eventlog_filter_to_string ( const st_EVENTLOG_FILTER *f );


/**
 * @brief Mapování názvu kategorie -> @ref en_EVENTLOG_CATEGORY hodnota.
 *
 * Helper pro UI (= label dropdown) i parser. Jména jsou lowercase
 * "snake_case" odpovídající enum hodnotám:
 *
 *  - @c "cpu_int"     -> @ref EVENTLOG_CAT_CPU_INT
 *  - @c "cpu_pin_edge"-> @ref EVENTLOG_CAT_CPU_PIN_EDGE
 *  - @c "irq_ack_im2" -> @ref EVENTLOG_CAT_IRQ_ACK_IM2
 *  - @c "iorq_in"     -> @ref EVENTLOG_CAT_IORQ_IN
 *  - @c "iorq_out"    -> @ref EVENTLOG_CAT_IORQ_OUT
 *  - @c "mmio_r"      -> @ref EVENTLOG_CAT_MMIO_R
 *  - @c "mmio_w"      -> @ref EVENTLOG_CAT_MMIO_W
 *  - @c "gdg_mode"    -> @ref EVENTLOG_CAT_GDG_MODE
 *  - @c "gdg_banking" -> @ref EVENTLOG_CAT_GDG_BANKING
 *  - @c "gdg_hwscroll"-> @ref EVENTLOG_CAT_GDG_HWSCROLL
 *  - @c "gdg_colors"  -> @ref EVENTLOG_CAT_GDG_COLORS
 *  - @c "gdg_video"   -> @ref EVENTLOG_CAT_GDG_VIDEO
 *  - @c "gdg_wfrf"    -> @ref EVENTLOG_CAT_GDG_WFRF
 *  - @c "pio8255"     -> @ref EVENTLOG_CAT_PIO8255
 *  - @c "ctc8253"     -> @ref EVENTLOG_CAT_CTC8253
 *  - @c "pioz80"      -> @ref EVENTLOG_CAT_PIOZ80
 *  - @c "psg"         -> @ref EVENTLOG_CAT_PSG
 *  - @c "qd"          -> @ref EVENTLOG_CAT_QD
 *  - @c "fdc"         -> @ref EVENTLOG_CAT_FDC
 *  - @c "memext"      -> @ref EVENTLOG_CAT_MEMEXT
 *  - @c "rd"          -> @ref EVENTLOG_CAT_RD
 *  - @c "bp_fire"     -> @ref EVENTLOG_CAT_BP_FIRE
 *  - @c "user_mark"   -> @ref EVENTLOG_CAT_USER_MARK
 *  - @c "cpu_ctrl"    -> @ref EVENTLOG_CAT_CPU_CTRL
 *
 * @param name  Lowercase identifier kategorie (@c NULL -> -1).
 * @return Hodnota kategorie @c [0..EVENTLOG_CAT_COUNT-1] nebo @c -1
 *         při neznámém / @c NULL jméně.
 */
int eventlog_filter_cat_from_name ( const char *name );


/**
 * @brief Inverzní mapování @ref en_EVENTLOG_CATEGORY -> lowercase jméno.
 *
 * Stabilní string konstanty (statické literály). Při neplatném @c cat
 * vrací @c NULL.
 *
 * @param cat  Hodnota kategorie (cast z @ref en_EVENTLOG_CATEGORY).
 * @return Lowercase string nebo @c NULL.
 */
const char *eventlog_filter_cat_to_name ( uint8_t cat );


/**
 * @brief Nastaví šíři logického screenu pro dekódování sline/px.
 *
 * Filter používá @c pxclk_in_screen z eventu a dělí ho šíří screenu
 * pro získání scanline (@c pxclk_in_screen / @c width) a pixel column
 * (@c pxclk_in_screen % @c width). Hodnota je per-arch (MZ-800 H40 =
 * 320, H80 = 640, MZ-700 = 320).
 *
 * Default po startu = @c 320 (= MZ-800 H40 nebo MZ-700 textový). UI
 * nastavuje při změně video módu nebo arch přepnutí. Hodnota
 * @c 0 je ignorována (= ponechá předchozí).
 *
 * @param width  Šíře screenu v pixel clocks (typicky 320 nebo 640).
 */
void eventlog_filter_set_screen_width ( uint32_t width );


/**
 * @brief Maximální délka filter expression (input do parse).
 *
 * Dlouhé filtery jsou parse error (= UI overflow guard). 256 byte
 * stačí na ~30 tokenů s plnými prefixy + závorky.
 */
#define EVENTLOG_FILTER_MAX_EXPR_LEN  256u


#ifdef __cplusplus
}
#endif

#endif /* EVENTLOG_FILTER_H */
