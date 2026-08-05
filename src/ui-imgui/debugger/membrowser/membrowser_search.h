/**
 * @file membrowser_search.h
 * @brief V4 search engine pro Memory Browser - public API.
 *
 * Vyčleněno z membrowser_window.cpp (V0 měl naivní inline search v
 * render_search_panel). V4 přidává:
 *   - 7 search typů (byte seq, ASCII, word LE/BE, masked, regex, expression)
 *   - region scope (current / all regions current arch)
 *   - case sensitivity (ASCII)
 *   - chunked / multi-frame scan s progress + cancel
 *   - Find Next / Find Prev / Find All s result listem (max 256)
 *
 * Threading: UI-thread only. Engine drží state v st_MEMBROWSER_STATE
 * (search_state, search_scan_pos, search_results[]). Per frame se volá
 * @ref membrowser_search_step, který spotřebuje chunk_budget bajtů a
 * vrátí. Po dokončení nastaví search_state = DONE.
 *
 * Backend access: přes membrowser_io_make_emu_backend + read_bytes; žádný
 * direct dbgapi_regions_* call.
 *
 * Pattern compile: ASCII/byte_seq/masked/word jsou parsované do byte
 * sekvence + mask. Regex přes std::regex (POSIX ERE = extended grammar)
 * nad raw bajtovou sekvencí (bytes 0..0xFF, regex matchuje bytes ne unicode).
 * Expression přes bp_expr - eval per byte s ctx.Value = aktuální byte,
 * ctx.Address = offset.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_SEARCH_H
#define MEMBROWSER_SEARCH_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stddef.h>             /* size_t (Linux: stdint.h nezahrnuje tranzitivně) */
#include <stdint.h>
#include <stdbool.h>

#include "membrowser_state.h"
#include "membrowser_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximální délka literálního byte patternu (byte_seq/ASCII/masked/word).
 *
 * Regex a expression nepoužívají toto omezení - parser je drží jako
 * std::regex / bp_expr_t* handles. Pro literální patterny je 256 bajtů
 * víc než dostačující (search_pattern je 256 znaků = max ~128 hex párů).
 */
#define MB_SEARCH_MAX_PATTERN_BYTES 256

/**
 * @brief Kompilovaný pattern - intermediate forma po validaci search_pattern.
 *
 * Vyrobeno @ref membrowser_search_compile, freed @ref membrowser_search_compiled_free.
 * Engine drží jeden aktivní compiled pattern během search_state == RUNNING.
 *
 * Pro literal patterny (byte_seq/ASCII/word/masked): bytes + mask + len.
 *   mask[i] != 0 znamená "porovnej bytes[i]"; mask[i] == 0 znamená wildcard.
 * Pro regex: opaque pointer na std::regex (drženy přes void*).
 * Pro expression: opaque pointer na bp_expr_t.
 */
typedef struct st_MB_SEARCH_COMPILED
{
    int type;                                       /**< en_MEMBROWSER_SEARCH_TYPE. */
    uint8_t bytes[MB_SEARCH_MAX_PATTERN_BYTES];     /**< Literal byte sekvence (po per-char encoding). */
    uint8_t mask[MB_SEARCH_MAX_PATTERN_BYTES];      /**< Per-byte mask (0xFF = porovnej, 0x00 = wildcard). */
    int len;                                        /**< Délka bytes/mask (0 = invalid). */
    bool case_sensitive;                            /**< Pro ASCII/BYTES typy honoring přes encoding-aware tolower. */
    int encoding_id;                                /**< V4.1+: en_MEMBROWSER_ENCODING pro encoding-aware tolower v match (CG/KOI8 fold). */
    void *regex_handle;                             /**< std::regex* (NULL pokud type != REGEX). */
    void *expr_handle;                              /**< bp_expr_t* (NULL pokud type != EXPRESSION). */
} st_MB_SEARCH_COMPILED;

/**
 * @brief Skompiluje search_pattern + search_type ze state do st_MB_SEARCH_COMPILED.
 *
 * @param st     Source state (musí být non-NULL). Čte search_pattern,
 *               search_type, search_case_sensitive, current_encoding.
 * @param out    Cílový compiled pattern (NESmí být NULL, ownership patří caller).
 * @param errbuf Buffer pro chybovou hlášku (může být NULL).
 * @param errbuf_size Velikost errbuf.
 * @return true při úspěchu, false pokud pattern je prázdný / invalid /
 *         regex/expr parser selhal (errbuf naplněn).
 *
 * @note Caller MUSÍ volat @ref membrowser_search_compiled_free i pri úspěchu.
 */
bool membrowser_search_compile ( const st_MEMBROWSER_STATE *st,
                                 st_MB_SEARCH_COMPILED *out,
                                 char *errbuf, size_t errbuf_size );

/**
 * @brief Uvolní alokované resources (regex/expr handles) v compiled patternu.
 *
 * Bezpečné volat i na čerstvě vyplněnou strukturu (skipy NULL handles).
 *
 * @param c Pointer na compiled pattern (NULL = no-op).
 */
void membrowser_search_compiled_free ( st_MB_SEARCH_COMPILED *c );

/**
 * @brief Test jestli compiled pattern matchuje na dané pozici v bufferu.
 *
 * Pro literal patterns: porovnání bytes vs buffer (per-byte mask).
 * Pro regex: std::regex_search jednoznakové okno.
 * Pro expression: bp_expr_eval s ctx.Value = buffer[0], ctx.Address = abs_addr.
 *
 * @param c        Compiled pattern.
 * @param buffer   Pointer na začátek matching window.
 * @param avail    Kolik bajtů je dostupných od buffer (= overlap pro literal len).
 * @param abs_addr Absolutní adresa v rámci regionu (pro expression Address ctx).
 * @return true pokud match, false jinak.
 */
bool membrowser_search_match_at ( const st_MB_SEARCH_COMPILED *c,
                                  const uint8_t *buffer,
                                  uint32_t avail,
                                  uint64_t abs_addr );

/**
 * @brief Spustí nové hledání - reset engine state a začne scan.
 *
 * Setupuje search_state na RUNNING, naplní search_engine_region_* podle
 * scope, search_scan_pos na (cursor + 1) pro NEXT / (cursor - 1) pro PREV.
 * Pro ALL spustí scan od začátku regionu(ů) a sbírá do search_results[].
 *
 * @param st     State - bude modifikován (search_state, scan_pos, results).
 * @param snap   Aktuální regions snapshot (pro scope ALL_REGIONS iteration).
 * @param mode   en_MEMBROWSER_SEARCH_MODE.
 *
 * @pre snap != NULL, st != NULL.
 * @post Pokud search_state == ERROR, search_error obsahuje hlášku.
 *       Pokud DONE rovnou (např. nalezeno hned v prvním chunku), engine
 *       nezůstává v RUNNING.
 */
void membrowser_search_start ( st_MEMBROWSER_STATE *st,
                               const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                               int mode );

/**
 * @brief Pokračuje v běhícím hledání - chunk per frame.
 *
 * Měl by být volán každý frame pokud search_state == RUNNING. Spotřebuje
 * až chunk_budget bajtů z aktuálního regionu, posune scan_pos. Pokud
 * narazí na konec regionu a scope == ALL_REGIONS, přeskočí na další.
 *
 * Pro mode NEXT/PREV: po prvním matchi nastaví cursor na found_addr +
 * last_match_addr/valid a přejde do DONE.
 * Pro mode ALL: sbírá do search_results[] až do limit MB_SEARCH_MAX_RESULTS,
 * pak DONE (status: max results reached).
 *
 * @param st           State.
 * @param snap         Regions snapshot.
 * @param chunk_budget Maximální počet bajtů zpracovaných v tomto volání
 *                     (typicky 256*1024 = 256 KB).
 *
 * @note Engine při canceled / done je no-op (vrátí bez práce).
 */
void membrowser_search_step ( st_MEMBROWSER_STATE *st,
                              const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                              uint32_t chunk_budget );

/**
 * @brief Zrušení běžícího hledání - nastaví CANCELED.
 *
 * Bezpečné volat i pri IDLE / DONE / ERROR (no-op).
 *
 * @param st State.
 */
void membrowser_search_cancel ( st_MEMBROWSER_STATE *st );

/**
 * @brief V4.1+ Pattern Builder helper: parsuje hex string ("AA BB CC")
 *        do byte pole. Akceptuje volitelné mezery, taby, čárky mezi byty.
 *
 * Reuse interní parse_byte_seq logiky. Vrací -1 při syntax chybě (lichý
 * počet nibblů, invalid hex char).
 *
 * @param hex_str   NUL-terminated hex string.
 * @param out_bytes Cílové pole (NESmí být NULL).
 * @param max_bytes Max počet bytů které out_bytes pojme.
 * @return Počet parsovaných bytů (0..max_bytes), nebo -1 chyba.
 */
int membrowser_search_decode_hex_bytes ( const char *hex_str,
                                          uint8_t *out_bytes,
                                          int max_bytes );


/**
 * @brief V4.1+ Pattern Builder helper: enkóduje byte pole do hex string
 *        "AA BB CC" (mezera mezi byty, žádná na začátku/konci).
 *
 * @param bytes    Pole bytů.
 * @param len      Počet bytů.
 * @param out      Cílový buffer (NUL-terminated, NESmí být NULL).
 * @param out_size Velikost cílového bufferu.
 * @return Počet bytů zapsaných do out (= len pokud out_size byl dostatečný,
 *         menší při truncation).
 */
int membrowser_search_encode_hex_bytes ( const uint8_t *bytes, int len,
                                          char *out, size_t out_size );


/**
 * @brief Push pattern do history (most-recent first), deduplikace + rotation.
 *
 * Pokud pattern už v history je, posune na index 0 (= most-recent).
 * Jinak rotuje vše o jedno a vloží na 0.
 *
 * @param st       State (search_history modifikován).
 * @param pattern  Pattern string (NULL nebo prázdný = no-op).
 * @param type     Typ patternu (en_MEMBROWSER_SEARCH_TYPE).
 */
void membrowser_search_history_push ( st_MEMBROWSER_STATE *st,
                                      const char *pattern, int type );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* MEMBROWSER_SEARCH_H */
