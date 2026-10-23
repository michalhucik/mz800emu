/*
 * File:   bp_expr.h
 *
 * Expression evaluator pro smart breakpointy (V1, fáze D.1.2).
 *
 * Vlastní mini-jazyk pro condition pole BP a budoucí action argumenty
 * (D.1.3). Syntax C-like s precedencí + short-circuit && / ||, sdílený
 * mezi BP/Watch/Trace.
 *
 * Hodnotová doména: signed 32-bit int (dostatečné pro Z80 16-bit values
 * + bank context + signed math). Bez floating point.
 *
 * Lifecycle:
 *   bp_expr_t *e = bp_expr_parse(src, errbuf, sizeof(errbuf));
 *   if (!e) { ... fprintf(stderr, "%s\n", errbuf); ... }
 *   int32_t v = bp_expr_eval(e, &ctx);
 *   bp_expr_free(e);
 *
 * Numerické literály (= IASM-konzistentní):
 *   - hex: 0xNNNN / 0XNNNN / #NNNN
 *   - bin: %NNNN  (lexer disambig vůči modulo - viz bp_expr.c)
 *   - dec: NNNN
 *
 * Operátory (priorita zhora dolů):
 *   1.  ()                                  závorky
 *   2.  + - ~ !                             unární
 *   3.  * / %                               multiplikativní
 *   4.  + -                                 aditivní
 *   5.  << >>                               shift
 *   6.  < <= > >=                           relační
 *   7.  == !=                               equality
 *   8.  &                                   bitové AND
 *   9.  ^                                   bitové XOR
 *  10.  |                                   bitové OR
 *  11.  &&                                  logické AND (short-circuit)
 *  12.  ||                                  logické OR (short-circuit)
 *
 * Memory access (per kontextové proměnné):
 *   [addr]   = byte z paměti, default no side-effect (= '@' implicit)
 *   {addr}   = word LE z paměti
 *   [addr]!  = byte s side-effect (simuluje CPU read)
 *   port[N]  = I/O port byte (no side-effect)
 *   port[N]! = I/O read s side-effect
 *
 * V1 implementační omezení: side-effect kontrola je naznačena v AST,
 * ale dispatch v evaluatoru pro non-se a se variantu volá stejný
 * read_byte / port_in (TODO: V1.5 vyrobit no_se variantu).
 *
 * Identifikátory (case-insensitive):
 *   Z80 registry: A, F, B, C, D, E, H, L, BC, DE, HL, AF, IX, IY,
 *                 SP, PC, I, R, IM, IFF1, IFF2
 *   Shadow:       AF', BC', DE', HL'
 *   Z80 flagy:    Cf (carry), Zf, Sf, Pf, Hf, Nf  (= 0/1)
 *                 (POZOR: 'C' samo o sobě = registr C, ne carry flag)
 *   Kontext BP:   Address, Value, IsRead, IsWrite, IsExec, IsPort,
 *                 BankPC, BankAddr
 *   Globální:     Cycle, Frame, Scanline
 *   User vars:    $name (D.1.3 - propojeno s bp_vars storage,
 *                        neexistující jméno → 0)
 *
 * Built-in funkce:
 *   min(a,b), max(a,b), abs(x), if(cond,a,b),
 *   bit(value,n), s8(x), s16(x), s32(x),
 *   lastwriter(addr) - PC posledního zápisu do buňky podle ramlogu (-1 = neznámý)
 *
 * ----------------------------- License -------------------------------------
 * GPL-3.0-or-later, see breakpoints.h header for full notice.
 * ---------------------------------------------------------------------------
 */

#ifndef BP_EXPR_H
#define BP_EXPR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "libs/cpu-z80/z80.h"


    /**
     * @brief Opaque handle reprezentující parsovaný AST výrazu.
     *
     * Vlastněný caller-em, freed přes bp_expr_free(). Threading: AST je
     * read-only po parse, lze sdílet mezi vlákny pro eval (pokud ctx
     * není sdílený mutable).
     */
    typedef struct st_BP_EXPR bp_expr_t;


    /**
     * @brief Kontext pro vyhodnocování výrazu.
     *
     * Naplňuje caller před voláním bp_expr_eval(). Pole nepoužitá daným
     * výrazem se ignorují.
     *
     * @invariant cpu může být NULL - pak registry resolve na 0.
     * @invariant vars rezervováno pro D.1.3 ($name storage). V1.2 = NULL.
     */
    typedef struct bp_expr_ctx_s {
        /* Kontextová BP hit data (= naplněno BP enforcement vrstvou). */
        int32_t  Address;       /**< Aktuální memory adresa accessu */
        int32_t  Value;         /**< Hodnota čtená/zapisovaná */
        bool     IsRead;        /**< true při memory read accessu */
        bool     IsWrite;       /**< true při memory write accessu */
        bool     IsExec;        /**< true při PC execution */
        bool     IsPort;        /**< true při IORQ accessu */
        uint8_t  BankPC;        /**< bank kontext PC */
        uint8_t  BankAddr;      /**< bank kontext target adresy */
        /* Globální emu state (= naplnit z g_mzarch_main / g_gdg). */
        int32_t  InsnPC;        /**< Adresa právě vykonávané instrukce (g_mzarch_main.instruction_addr);
                                     u MEM_R/MEM_W/IORQ je živé PC už za ní. */
        int64_t  Cycle;         /**< Takty GDG od startu emulátoru na začátku instrukce (gdg_get_total_ticks; reset stroje čítač nenuluje);
                                     ve výrazu oříznuté na int32 (přeteče asi po 121 s PAL). */
        int64_t  Frame;         /**< Počet dokončených snímků od startu emulátoru (reset ho nenuluje). */
        int32_t  FrameCycle;    /**< Takty GDG od začátku aktuálního snímku (0 .. takty snímku - 1),
                                     na začátku instrukce; nepřetéká. */
        int32_t  Scanline;      /**< GDG raster line */
        /* CPU stav (NULL = registry resolve na 0). */
        z80_t   *cpu;
        /* User vars storage - placeholder pro D.1.3. */
        void    *vars;
        /* ID právě firujícího BP (D.2). Použito akcí disable_self pro
         * vypnutí "vlastního" BP. -1 = ctx vznikl mimo BP enforcement
         * (= test, eval bez self-reference); disable_self pak loguje
         * warning a no-op. */
        int      self_id;
        /* true = znak `!` se ignoruje: `[addr]!` / `{addr}!` čte jako
         * `[addr]` (bez MEM_R BP), `port[N]!` jako sonda `port[N]`. Pro
         * vyhodnocení, které stav stroje měnit nesmí: Test Eval editačního
         * panelu BP (DBGAPI_CMD_BP_TEST_EVAL), Watch (okno, watch_list,
         * watch_eval) a hledání výrazem v Memory Browseru;
         * bp_expr_ctx_zero() nastaví false (= `!` platí, akce a podmínky BP). */
        bool     no_side_effects;
    } bp_expr_ctx_t;


    /**
     * @brief Naplní bp_expr_ctx_t nulovými / neutrálními hodnotami.
     *
     * Pomůcka pro testy a callery, kteří chtějí jen evaluovat
     * výraz nezávislý na kontextu (např. konstantní podmínka).
     *
     * @param ctx Cíl (nesmí být NULL).
     */
    extern void bp_expr_ctx_zero ( bp_expr_ctx_t *ctx );


    /**
     * @brief Parse zdrojový text výrazu na AST.
     *
     * @param source Zdrojový text výrazu (UTF-8, NULL-terminated).
     *               NULL nebo prázdný řetězec → chyba.
     * @param errbuf Buffer pro chybovou hlášku (může být NULL).
     * @param errbuf_size Velikost errbuf v bajtech.
     * @return Vlastněný AST handle, nebo NULL při chybě (pak je v errbuf
     *         krátký technický popis problému).
     *
     * Ownership: caller musí volat bp_expr_free() na vrácený pointer
     * (kromě NULL).
     *
     * Side effects: žádné. Funkce je čistá.
     */
    extern bp_expr_t* bp_expr_parse ( const char *source,
                                       char *errbuf, size_t errbuf_size );


    /**
     * @brief Vyhodnotí parsovaný výraz proti kontextu.
     *
     * @param expr Parsovaný AST (musí být non-NULL, vrácený z bp_expr_parse).
     * @param ctx  Kontext pro resolve identifikátorů a memory accesů
     *             (musí být non-NULL).
     * @return Signed 32-bit hodnota výrazu. Pro BP condition platí:
     *         0 = false, ne-nula = true.
     *
     * Side effects: pokud výraz obsahuje memory deref s '!' suffixem,
     * provede se přes regulární CPU read path (= simuluje side-effect).
     * V1.2 implementuje rozdíl naznačeně, ale skutečné no_se varianty
     * čekají na V1.5 (TODO).
     *
     * Při nesplnitelné situaci (= neznámý identifikátor, dělení nulou,
     * NULL cpu pro register access) vrací 0 a pokračuje, NEvyhazuje
     * fatální chybu.
     */
    extern int32_t bp_expr_eval ( const bp_expr_t *expr,
                                   const bp_expr_ctx_t *ctx );


    /**
     * @brief Uvolní paměť asociovanou s AST.
     *
     * @param expr AST k uvolnění (NULL = no-op).
     *
     * Postcondition: expr pointer je po návratu invalidní.
     */
    extern void bp_expr_free ( bp_expr_t *expr );


    /**
     * @brief Pomůcka: parse + eval + free v jednom volání.
     *
     * Použití pro one-shot vyhodnocení (typicky v testech). Pro
     * BP condition je kontraproduktivní (parse je drahý, cache se ztrácí).
     *
     * @param source Zdrojový text.
     * @param ctx    Kontext pro eval.
     * @param out    Výstupní hodnota (zapíše se i při chybě = 0).
     * @param errbuf Buffer pro chybovou hlášku (může být NULL).
     * @param errbuf_size Velikost errbuf.
     * @return true při úspěchu, false při parser chybě.
     */
    extern bool bp_expr_parse_eval ( const char *source,
                                      const bp_expr_ctx_t *ctx,
                                      int32_t *out,
                                      char *errbuf, size_t errbuf_size );


#ifdef __cplusplus
}
#endif

#endif /* BP_EXPR_H */
