/**
 * @file membrowser_search.cpp
 * @brief V4 search engine pro Memory Browser - implementace.
 *
 * Viz @ref membrowser_search.h pro high-level kontrakt.
 *
 * Implementační poznámky:
 *
 *   1. Pattern compile dispatch per type:
 *      - BYTE_SEQ: parse "AA BB CC" -> bytes, mask = vse 0xFF
 *      - ASCII:    per-char via membrowser_encoding_utf8_to_byte; pokud
 *                  case_sensitive == false, lower-case ASCII bajty (0x41-0x5A
 *                  -> 0x61-0x7A) v patternu i v match buffer wrapperu
 *      - WORD_LE/BE: parse jedno cislo (hex 0x.. nebo dec), 2 bajty
 *      - MASKED:   parse "AA ?? BB" -> ?? = mask[i]=0
 *      - REGEX:    std::regex parsovat na regex_handle; match per byte window
 *      - EXPRESSION: bp_expr_parse; eval per byte
 *
 *   2. Engine state machine:
 *      START -> RUNNING -> [DONE | CANCELED | ERROR]
 *
 *      STEP cyklus per frame:
 *      while (chunk_budget > 0 && state == RUNNING):
 *          read_bytes(region, scan_pos, buffer, min(budget, region_end-scan_pos))
 *          for each candidate position in buffer:
 *              if (match_at): handle per mode (NEXT/PREV: set cursor, DONE;
 *                                              ALL: append to results)
 *          scan_pos += read_len - (pattern_len - 1)  // overlap
 *          if scan_pos >= region_end:
 *              if scope == ALL_REGIONS: advance to next region
 *              else: DONE
 *          budget -= read_len
 *
 *   3. PREV mode: skenuje zpět od cursor-1 v opačném smeru. Chunked je
 *      tady taky - čteme overlapping chunky pozpátku. V4 implementace
 *      použije naivní zpětný scan po chunku 64 KB - akceptovatelný pro
 *      drtivou většinu regionů.
 *
 * Threading: vše single-thread UI. dbgapi_regions_* je no-side-effect,
 * lze volat kdykoliv mezi emu cykly bez mutexu.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_search.h"
#include "membrowser_encoding.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <regex>
#include <new>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/debugger/bp_expr.h"
}


/* ---- Pomocné funkce --------------------------------------------------- */


/**
 * @brief Parse jednoho hex páru z pozice p. Vrátí počet zkonzumovaných znaků.
 *
 * Akceptuje "AA", "Aa", "aA", "aa". Při méně než 2 hex znacích vrací 0.
 */
static int parse_hex_byte ( const char *p, uint8_t *out )
{
    int v = 0;
    int n = 0;
    for ( int i = 0; i < 2 && p[i] != '\0'; i++ ) {
        char c = p[i];
        int d = -1;
        if ( c >= '0' && c <= '9' ) d = c - '0';
        else if ( c >= 'a' && c <= 'f' ) d = 10 + ( c - 'a' );
        else if ( c >= 'A' && c <= 'F' ) d = 10 + ( c - 'A' );
        if ( d < 0 ) break;
        v = ( v << 4 ) | d;
        n++;
    }
    if ( n == 0 ) return 0;
    *out = ( uint8_t ) v;
    return n;
}


/**
 * @brief Parse byte sequence "AA BB CC" - whitespace separated hex pairs.
 *
 * Returns počet vyparsovaných bajtů, max max_len. -1 pri chybě (neplatný hex).
 */
static int parse_byte_seq ( const char *src, uint8_t *out_bytes,
                            uint8_t *out_mask, int max_len )
{
    int count = 0;
    const char *p = src;
    while ( *p && count < max_len ) {
        while ( *p == ' ' || *p == '\t' || *p == ',' ) p++;
        if ( !*p ) break;
        uint8_t b = 0;
        int n = parse_hex_byte ( p, &b );
        if ( n == 0 ) return -1;
        out_bytes[count] = b;
        out_mask[count] = 0xFF;
        count++;
        p += n;
    }
    return count;
}


/* V4.1+: Public Pattern Builder API helpers. Wrappery nad parse_byte_seq
 * (decode) a inline format loop (encode). UI v window.cpp je volá pro
 * dual HEX+ASCII fields sync logiku. */
extern "C" int membrowser_search_decode_hex_bytes ( const char *hex_str,
                                                      uint8_t *out_bytes,
                                                      int max_bytes )
{
    if ( !hex_str || !out_bytes || max_bytes <= 0 ) return 0;
    /* Reuse parse_byte_seq - vyžaduje out_mask buffer (= dummy). */
    uint8_t dummy_mask[MB_SEARCH_MAX_PATTERN_BYTES];
    if ( max_bytes > MB_SEARCH_MAX_PATTERN_BYTES ) {
        max_bytes = MB_SEARCH_MAX_PATTERN_BYTES;
    }
    return parse_byte_seq ( hex_str, out_bytes, dummy_mask, max_bytes );
}


extern "C" int membrowser_search_encode_hex_bytes ( const uint8_t *bytes,
                                                      int len, char *out,
                                                      size_t out_size )
{
    if ( !out || out_size == 0 ) return 0;
    out[0] = '\0';
    if ( !bytes || len <= 0 ) return 0;

    int written = 0;
    size_t pos = 0;
    for ( int i = 0; i < len; i++ ) {
        /* Per byte = "XX" (2 char) + optional " " (1 char) + NUL.
         * Min vejde se: pos + 3 < out_size (= "XX\0"); s mezerou pak 4. */
        const char *sep = ( i > 0 ) ? " " : "";
        int need = ( i > 0 ) ? 3 : 2;  /* mezera + 2 hex digity. */
        if ( pos + ( size_t ) need + 1 > out_size ) break;
        int n = std::snprintf ( out + pos, out_size - pos, "%s%02X",
                                 sep, bytes[i] );
        if ( n < 0 ) break;
        pos += ( size_t ) n;
        written++;
    }
    return written;
}


/**
 * @brief Parse masked sequence "AA ?? BB" - ?? = wildcard (mask=0).
 *
 * Akceptuje i jeden '?' (= jeden nibble wildcard? -> NE, V4 jen celý byte:
 * dvojice znaků '??'). Jiné kombinace ('?A', 'A?') = error.
 */
static int parse_masked_seq ( const char *src, uint8_t *out_bytes,
                              uint8_t *out_mask, int max_len )
{
    int count = 0;
    const char *p = src;
    while ( *p && count < max_len ) {
        while ( *p == ' ' || *p == '\t' || *p == ',' ) p++;
        if ( !*p ) break;
        if ( p[0] == '?' && p[1] == '?' ) {
            out_bytes[count] = 0x00;
            out_mask[count] = 0x00;
            count++;
            p += 2;
        } else if ( p[0] == '?' || p[1] == '?' ) {
            /* Nibble wildcard nepodporujeme. */
            return -1;
        } else {
            uint8_t b = 0;
            int n = parse_hex_byte ( p, &b );
            if ( n == 0 ) return -1;
            out_bytes[count] = b;
            out_mask[count] = 0xFF;
            count++;
            p += n;
        }
    }
    return count;
}


/**
 * @brief Parse number literal - hex (0x..), bin (0b..), nebo decimal.
 *
 * Vrací true a naplní out_value pri úspěchu, false jinak.
 */
static bool parse_number ( const char *src, uint32_t *out_value )
{
    if ( !src || !*src ) return false;
    const char *p = src;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( !*p ) return false;

    char *endp = nullptr;
    unsigned long v = 0;
    if ( p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) ) {
        v = std::strtoul ( p + 2, &endp, 16 );
    } else if ( p[0] == '0' && ( p[1] == 'b' || p[1] == 'B' ) ) {
        v = std::strtoul ( p + 2, &endp, 2 );
    } else {
        v = std::strtoul ( p, &endp, 10 );
    }
    if ( endp == p ) return false;
    *out_value = ( uint32_t ) v;
    return true;
}


/**
 * @brief ASCII to lowercase pro case-insensitive porovnání.
 *
 * Pouze 0x41-0x5A -> 0x61-0x7A. Ostatní bajty beze změny.
 * (NE-ASCII jako ISO-8859 / KOI8 by potřebovala lokalizovanou tabulku;
 * V4 omezení na čistý ASCII case folding.)
 */
static uint8_t ascii_tolower ( uint8_t b )
{
    if ( b >= 0x41 && b <= 0x5A ) return b + 0x20;
    return b;
}


/* ---- Compile / free --------------------------------------------------- */


extern "C" bool membrowser_search_compile ( const st_MEMBROWSER_STATE *st,
                                            st_MB_SEARCH_COMPILED *out,
                                            char *errbuf, size_t errbuf_size )
{
    if ( !st || !out ) return false;

    /* Reset out struktura - bez memset (drží non-trivial members teoreticky;
     * tady jen POD-like, ale explicitní init je bezpečnější). */
    out->type = st->search_type;
    out->len = 0;
    out->case_sensitive = st->search_case_sensitive;
    out->encoding_id = st->current_encoding;  /* V4.1+: pro encoding-aware tolower. */
    out->regex_handle = nullptr;
    out->expr_handle = nullptr;
    std::memset ( out->bytes, 0, sizeof ( out->bytes ) );
    std::memset ( out->mask, 0, sizeof ( out->mask ) );

    const char *pat = st->search_pattern;
    if ( !pat || pat[0] == '\0' ) {
        if ( errbuf && errbuf_size > 0 ) {
            std::snprintf ( errbuf, errbuf_size, "empty pattern" );
        }
        return false;
    }

    switch ( st->search_type ) {

    case MEMBROWSER_SEARCH_BYTE_SEQ:
    case MEMBROWSER_SEARCH_BYTES: {
        /* V4.1+: BYTES type používá stejný parser jako BYTE_SEQ -
         * canonical storage v search_pattern[] je hex string "AA BB CC".
         * Pattern Builder UI (HEX + ASCII synced fields) edituje tento
         * hex string; ASCII field je per-frame re-render z těchto bytů
         * přes current_encoding. Pro compile path je rozdíl mezi BYTE_SEQ
         * a BYTES jen kosmetický (= UI dispatch).
         *
         * Case sensitivity: pokud !case_sensitive, lowercase ASCII bytes
         * (0x41-0x5A → 0x61-0x7A). Match cyklus stejně lowercase'uje
         * buffer. Pro non-ASCII byty (control, high) tolower no-op, takže
         * jen Latin A-Z se foldne. */
        int n = parse_byte_seq ( pat, out->bytes, out->mask,
                                  MB_SEARCH_MAX_PATTERN_BYTES );
        if ( n <= 0 ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "invalid hex byte sequence" );
            }
            return false;
        }
        if ( !st->search_case_sensitive ) {
            /* V4.1+: encoding-aware fold (KOI8-CS Ž→ž, SharpMZ accent
             * pairs, Latin A-Z). Pro Raw a encodingy bez fold páru
             * vrátí byte beze změny. */
            for ( int i = 0; i < n; i++ ) {
                out->bytes[i] = membrowser_encoding_tolower (
                    out->bytes[i], st->current_encoding );
            }
        }
        out->len = n;
        return true;
    }

    case MEMBROWSER_SEARCH_ASCII: {
        /* Per-char encoding lookup; pokud case_insensitive, encoding-aware
         * tolower v out->bytes (match cyklus stejně tolower'uje buffer). */
        const char *src = pat;
        int n = 0;
        while ( *src && n < MB_SEARCH_MAX_PATTERN_BYTES ) {
            uint8_t b = 0;
            if ( membrowser_encoding_utf8_to_byte ( src, st->current_encoding, &b ) ) {
                if ( !st->search_case_sensitive ) {
                    /* V4.1+: encoding-aware tolower místo plain ASCII fold. */
                    b = membrowser_encoding_tolower ( b, st->current_encoding );
                }
                out->bytes[n] = b;
                out->mask[n] = 0xFF;
                n++;
            }
            /* Advance UTF-8 codepoint. */
            unsigned char c = ( unsigned char ) *src;
            if ( c < 0x80 ) src += 1;
            else if ( ( c & 0xE0 ) == 0xC0 ) src += 2;
            else if ( ( c & 0xF0 ) == 0xE0 ) src += 3;
            else if ( ( c & 0xF8 ) == 0xF0 ) src += 4;
            else src += 1;
        }
        if ( n <= 0 ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "empty / unencodable ASCII pattern" );
            }
            return false;
        }
        out->len = n;
        return true;
    }

    case MEMBROWSER_SEARCH_WORD_LE:
    case MEMBROWSER_SEARCH_WORD_BE: {
        uint32_t val = 0;
        if ( !parse_number ( pat, &val ) ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "invalid numeric literal (hex 0xNN, bin 0bNN, dec NNN)" );
            }
            return false;
        }
        /* 16-bit clamp pro V4 - vyšší rozsahy by potřebovaly WORD32 typ. */
        if ( val > 0xFFFFu ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "value out of 16-bit range (max 0xFFFF)" );
            }
            return false;
        }
        uint8_t lo = ( uint8_t ) ( val & 0xFF );
        uint8_t hi = ( uint8_t ) ( ( val >> 8 ) & 0xFF );
        if ( st->search_type == MEMBROWSER_SEARCH_WORD_LE ) {
            out->bytes[0] = lo;
            out->bytes[1] = hi;
        } else {
            out->bytes[0] = hi;
            out->bytes[1] = lo;
        }
        out->mask[0] = 0xFF;
        out->mask[1] = 0xFF;
        out->len = 2;
        return true;
    }

    case MEMBROWSER_SEARCH_MASKED: {
        int n = parse_masked_seq ( pat, out->bytes, out->mask,
                                    MB_SEARCH_MAX_PATTERN_BYTES );
        if ( n <= 0 ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "invalid masked sequence (use AA ?? BB)" );
            }
            return false;
        }
        out->len = n;
        return true;
    }

    case MEMBROWSER_SEARCH_REGEX: {
        try {
            std::regex::flag_type flags = std::regex::ECMAScript;
            /* V4: ERE (extended) i ECMAScript bohužel std::regex::extended
             * v MinGW libc++ mívá horší support pro \xNN escape. Použijeme
             * default ECMAScript (POSIX-like ERE features + běžně podporované
             * escapes). */
            if ( !st->search_case_sensitive ) flags |= std::regex::icase;
            std::regex *re = new ( std::nothrow ) std::regex ( pat, flags );
            if ( !re ) {
                if ( errbuf && errbuf_size > 0 ) {
                    std::snprintf ( errbuf, errbuf_size, "regex alloc failed" );
                }
                return false;
            }
            out->regex_handle = ( void * ) re;
            /* Pro regex je len = 1 (= matching window posouvá po 1 bajtu);
             * skutečná match délka je dynamická per match. */
            out->len = 1;
            return true;
        } catch ( const std::regex_error &e ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "regex parse error: %s", e.what ( ) );
            }
            return false;
        } catch ( ... ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size, "regex unknown error" );
            }
            return false;
        }
    }

    case MEMBROWSER_SEARCH_EXPRESSION: {
        char local_err[128];
        local_err[0] = '\0';
        bp_expr_t *e = bp_expr_parse ( pat, local_err, sizeof ( local_err ) );
        if ( !e ) {
            if ( errbuf && errbuf_size > 0 ) {
                std::snprintf ( errbuf, errbuf_size,
                                "expression parse error: %s",
                                local_err[0] ? local_err : "unknown" );
            }
            return false;
        }
        out->expr_handle = ( void * ) e;
        out->len = 1;  /* Expression eval per byte. */
        return true;
    }

    default:
        if ( errbuf && errbuf_size > 0 ) {
            std::snprintf ( errbuf, errbuf_size, "unknown search type" );
        }
        return false;
    }
}


extern "C" void membrowser_search_compiled_free ( st_MB_SEARCH_COMPILED *c )
{
    if ( !c ) return;
    if ( c->regex_handle ) {
        delete ( std::regex * ) c->regex_handle;
        c->regex_handle = nullptr;
    }
    if ( c->expr_handle ) {
        bp_expr_free ( ( bp_expr_t * ) c->expr_handle );
        c->expr_handle = nullptr;
    }
    c->len = 0;
}


/* ---- Match per pozice -------------------------------------------------- */


extern "C" bool membrowser_search_match_at ( const st_MB_SEARCH_COMPILED *c,
                                             const uint8_t *buffer,
                                             uint32_t avail,
                                             uint64_t abs_addr )
{
    if ( !c || !buffer ) return false;

    switch ( c->type ) {

    case MEMBROWSER_SEARCH_BYTE_SEQ:
    case MEMBROWSER_SEARCH_BYTES: {
        /* V4.1+: encoding-aware case fold (KOI8-CS, SharpMZ EU/JP,
         * Latin A-Z). Compile už foldlo c->bytes pokud !case_sensitive;
         * tady jen foldujeme buffer. */
        if ( ( uint32_t ) c->len > avail ) return false;
        if ( c->case_sensitive ) {
            for ( int i = 0; i < c->len; i++ ) {
                if ( buffer[i] != c->bytes[i] ) return false;
            }
        } else {
            for ( int i = 0; i < c->len; i++ ) {
                uint8_t bb = membrowser_encoding_tolower ( buffer[i],
                                                            c->encoding_id );
                if ( bb != c->bytes[i] ) return false;
            }
        }
        return true;
    }

    case MEMBROWSER_SEARCH_WORD_LE:
    case MEMBROWSER_SEARCH_WORD_BE:
    case MEMBROWSER_SEARCH_MASKED: {
        /* Word/Masked nemají case sens koncept - byte-exact match s mask. */
        if ( ( uint32_t ) c->len > avail ) return false;
        for ( int i = 0; i < c->len; i++ ) {
            if ( c->mask[i] == 0 ) continue;
            if ( ( buffer[i] & c->mask[i] ) != ( c->bytes[i] & c->mask[i] ) ) {
                return false;
            }
        }
        return true;
    }

    case MEMBROWSER_SEARCH_ASCII: {
        /* V4.1+: encoding-aware tolower místo plain ASCII fold. */
        if ( ( uint32_t ) c->len > avail ) return false;
        if ( c->case_sensitive ) {
            for ( int i = 0; i < c->len; i++ ) {
                if ( buffer[i] != c->bytes[i] ) return false;
            }
        } else {
            for ( int i = 0; i < c->len; i++ ) {
                uint8_t bb = membrowser_encoding_tolower ( buffer[i],
                                                            c->encoding_id );
                if ( bb != c->bytes[i] ) return false;
            }
        }
        return true;
    }

    case MEMBROWSER_SEARCH_REGEX: {
        if ( !c->regex_handle || avail == 0 ) return false;
        try {
            std::regex *re = ( std::regex * ) c->regex_handle;
            /* Match na pevné pozici 0 v okně. avail je horní limit
             * - regex by jinak mohl skočit za konec bufferu. Match start
             * je fixed na buffer[0] (= scanner posouvá pozici sám). */
            std::cmatch m;
            const char *bs = ( const char * ) buffer;
            const char *be = bs + avail;
            /* regex_search s ECMAScript ^ anchor by vyžadoval flag - místo
             * toho match jen pokud match je na pozici 0. */
            if ( std::regex_search ( bs, be, m, *re ) ) {
                if ( m.position ( 0 ) == 0 ) return true;
            }
            return false;
        } catch ( ... ) {
            return false;
        }
    }

    case MEMBROWSER_SEARCH_EXPRESSION: {
        if ( !c->expr_handle || avail == 0 ) return false;
        bp_expr_ctx_t ctx;
        bp_expr_ctx_zero ( &ctx );
        ctx.Value = ( int32_t ) buffer[0];
        ctx.Address = ( int32_t ) ( abs_addr & 0x7FFFFFFF );
        ctx.IsRead = true;
        int32_t v = bp_expr_eval ( ( bp_expr_t * ) c->expr_handle, &ctx );
        return ( v != 0 );
    }

    default:
        return false;
    }
}


/* ---- Helper: naplň hit záznam ------------------------------------------ */


/**
 * @brief Vyplní jeden hit záznam (přečte preview z backendu).
 */
static void fill_hit ( st_MB_SEARCH_HIT *hit, int region_id,
                       int region_kind, int region_sub_id, uint64_t addr )
{
    hit->region_id = region_id;
    hit->region_kind = region_kind;
    hit->region_sub_id = region_sub_id;
    hit->addr = addr;
    std::memset ( hit->preview, 0, sizeof ( hit->preview ) );
    hit->preview_len = 0;

    /* Read 16 bajtů preview přes dbgapi (region_id stabilní v aktuálním
     * frame snapshotu, který právě engine zpracovává). */
    int got = dbgapi_regions_read ( region_id, ( uint32_t ) addr,
                                    hit->preview, ( uint32_t ) sizeof ( hit->preview ) );
    if ( got > 0 ) hit->preview_len = got;
}


/* ---- Engine: start / step / cancel ------------------------------------- */


/**
 * @brief Najde index regionu v snap dle (kind, sub_id). -1 = not found.
 */
static int find_region_index_by_key ( const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                      int kind, int sub_id )
{
    if ( !snap ) return -1;
    for ( int i = 0; i < snap->count; i++ ) {
        if ( ( int ) snap->items[i].kind == kind
             && snap->items[i].sub_id == sub_id ) {
            return i;
        }
    }
    return -1;
}


/**
 * @brief Initial scan pos podle mode + cursor.
 *
 * NEXT: cursor + 1 (wrap-around přes region boundary není - V4 nezačíná
 *       skenovat zpátku).
 * PREV: 0 (scanner pak iteruje od cursor-1 zpět; pro V4 chunked
 *       implementace prevu = sken celého regionu od 0 a vybere nejbližší
 *       match před cursor).
 * ALL:  0 (celý region / všechny regiony).
 */
static uint64_t compute_initial_scan_pos ( const st_MEMBROWSER_STATE *st,
                                           int mode )
{
    switch ( mode ) {
    case MEMBROWSER_SEARCH_MODE_NEXT:
        return ( uint64_t ) st->cursor_addr + 1;
    case MEMBROWSER_SEARCH_MODE_PREV:
    case MEMBROWSER_SEARCH_MODE_ALL:
    default:
        return 0;
    }
}


extern "C" void membrowser_search_start ( st_MEMBROWSER_STATE *st,
                                          const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                          int mode )
{
    if ( !st || !snap ) return;

    /* Reset výsledků + chyby. */
    st->search_results_count = 0;
    st->search_results_visible = 0;
    st->search_error[0] = '\0';
    st->search_total_scanned = 0;
    st->search_total_bytes = 0;
    st->last_match_valid = false;

    /* Compile do file-static engine storage (drží se mezi _start a _step
     * vlastněn enginem; přeprava přes getter v step). */
    extern st_MB_SEARCH_COMPILED *membrowser_search__compiled_get ( void );
    extern void membrowser_search__compiled_reset ( void );
    membrowser_search__compiled_reset ( );
    char err[128];
    err[0] = '\0';
    if ( !membrowser_search_compile ( st, membrowser_search__compiled_get ( ),
                                       err, sizeof ( err ) ) ) {
        std::snprintf ( st->search_error, sizeof ( st->search_error ), "%s", err );
        st->search_state = MEMBROWSER_SEARCH_STATE_ERROR;
        return;
    }

    /* Setup scope. */
    st->search_mode = mode;
    int start_idx = -1;
    if ( st->search_scope == MEMBROWSER_SCOPE_ALL_REGIONS ) {
        start_idx = 0;
        /* Plánovaný total = suma sizes všech connected regionů. */
        uint64_t total = 0;
        for ( int i = 0; i < snap->count; i++ ) {
            if ( snap->items[i].connected ) total += snap->items[i].size;
        }
        st->search_total_bytes = total;
    } else {
        /* CURRENT scope. */
        start_idx = find_region_index_by_key ( snap,
                                                st->current_key.kind,
                                                st->current_key.sub_id );
        if ( start_idx < 0 ) {
            std::snprintf ( st->search_error, sizeof ( st->search_error ),
                            "current region not found" );
            st->search_state = MEMBROWSER_SEARCH_STATE_ERROR;
            return;
        }
        st->search_total_bytes = snap->items[start_idx].size;
    }
    if ( start_idx < 0 || start_idx >= snap->count ) {
        std::snprintf ( st->search_error, sizeof ( st->search_error ),
                        "no regions to search" );
        st->search_state = MEMBROWSER_SEARCH_STATE_ERROR;
        return;
    }

    st->search_engine_scope_idx = start_idx;
    st->search_engine_region_id = snap->items[start_idx].id;
    st->search_engine_region_kind = ( int ) snap->items[start_idx].kind;
    st->search_engine_region_sub_id = snap->items[start_idx].sub_id;
    st->search_scan_end = snap->items[start_idx].size;
    /* Initial scan_pos jen pro CURRENT + první region SCOPE_ALL_REGIONS = 0.
     * Pro NEXT mode v CURRENT scope začneme od cursor+1; v ALL_REGIONS
     * začneme od 0 (search je tu "find all matches", scope-wide). */
    if ( st->search_scope == MEMBROWSER_SCOPE_CURRENT ) {
        st->search_scan_pos = compute_initial_scan_pos ( st, mode );
        if ( st->search_scan_pos >= st->search_scan_end ) {
            st->search_scan_pos = 0;
        }
    } else {
        st->search_scan_pos = 0;
    }
    st->search_state = MEMBROWSER_SEARCH_STATE_RUNNING;
}


extern "C" void membrowser_search_cancel ( st_MEMBROWSER_STATE *st )
{
    if ( !st ) return;
    if ( st->search_state == MEMBROWSER_SEARCH_STATE_RUNNING ) {
        st->search_state = MEMBROWSER_SEARCH_STATE_CANCELED;
    }
}


/* Engine helper - advance to next region v SCOPE_ALL_REGIONS. */
static bool advance_to_next_region ( st_MEMBROWSER_STATE *st,
                                     const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    int idx = st->search_engine_scope_idx + 1;
    while ( idx < snap->count ) {
        if ( snap->items[idx].connected ) {
            st->search_engine_scope_idx = idx;
            st->search_engine_region_id = snap->items[idx].id;
            st->search_engine_region_kind = ( int ) snap->items[idx].kind;
            st->search_engine_region_sub_id = snap->items[idx].sub_id;
            st->search_scan_pos = 0;
            st->search_scan_end = snap->items[idx].size;
            return true;
        }
        idx++;
    }
    return false;
}


extern "C" void membrowser_search_step ( st_MEMBROWSER_STATE *st,
                                         const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                         uint32_t chunk_budget )
{
    if ( !st || !snap ) return;
    if ( st->search_state != MEMBROWSER_SEARCH_STATE_RUNNING ) return;

    /* Re-acquire compiled pattern z file-static engine storage. */
    extern st_MB_SEARCH_COMPILED *membrowser_search__compiled_get ( void );
    st_MB_SEARCH_COMPILED *c = membrowser_search__compiled_get ( );
    if ( !c || c->len <= 0 ) {
        st->search_state = MEMBROWSER_SEARCH_STATE_ERROR;
        std::snprintf ( st->search_error, sizeof ( st->search_error ),
                        "engine internal: no compiled pattern" );
        return;
    }

    /* Engine smyčka: zpracuj chunks dokud máme budget. */
    uint8_t chunk[64 * 1024];
    while ( chunk_budget > 0
            && st->search_state == MEMBROWSER_SEARCH_STATE_RUNNING ) {

        if ( st->search_scan_pos >= st->search_scan_end ) {
            /* Konec regionu - posuň na další (jen v SCOPE_ALL_REGIONS). */
            if ( st->search_scope == MEMBROWSER_SCOPE_ALL_REGIONS
                 && advance_to_next_region ( st, snap ) ) {
                continue;
            }
            /* Konec scanu. */
            st->search_state = MEMBROWSER_SEARCH_STATE_DONE;
            break;
        }

        uint64_t remain = st->search_scan_end - st->search_scan_pos;
        uint32_t read_len = ( remain > sizeof ( chunk ) )
                                ? ( uint32_t ) sizeof ( chunk )
                                : ( uint32_t ) remain;
        if ( read_len > chunk_budget ) read_len = chunk_budget;

        int got = dbgapi_regions_read ( st->search_engine_region_id,
                                         ( uint32_t ) st->search_scan_pos,
                                         chunk, read_len );
        if ( got <= 0 ) {
            /* Read fail - region disconnected mid-scan; přeskočit. */
            if ( st->search_scope == MEMBROWSER_SCOPE_ALL_REGIONS
                 && advance_to_next_region ( st, snap ) ) {
                continue;
            }
            st->search_state = MEMBROWSER_SEARCH_STATE_DONE;
            break;
        }

        /* Match scan - per byte position (i) v chunku. */
        int max_i = got - c->len + 1;
        if ( c->type == MEMBROWSER_SEARCH_REGEX
             || c->type == MEMBROWSER_SEARCH_EXPRESSION ) {
            /* Pro regex/expr c->len = 1 - skenujeme každý byte (regex_search
             * sám projde delší match). */
            max_i = got;
        }
        if ( max_i < 0 ) max_i = 0;

        for ( int i = 0; i < max_i; i++ ) {
            uint64_t abs_pos = st->search_scan_pos + ( uint64_t ) i;
            uint32_t avail = ( uint32_t ) ( got - i );
            if ( !membrowser_search_match_at ( c, chunk + i, avail, abs_pos ) ) {
                continue;
            }

            /* Match! */
            switch ( st->search_mode ) {
            case MEMBROWSER_SEARCH_MODE_NEXT: {
                /* Set cursor + uloz do results jako single hit + DONE. */
                st->cursor_addr = ( uint32_t ) abs_pos;
                st->last_match_addr = ( uint32_t ) abs_pos;
                st->last_match_valid = true;
                /* Pokud match je v jiném regionu (ALL_REGIONS scope),
                 * přepneme region. */
                if ( st->search_engine_region_kind != st->current_key.kind
                     || st->search_engine_region_sub_id != st->current_key.sub_id ) {
                    st->current_key.kind = st->search_engine_region_kind;
                    st->current_key.sub_id = st->search_engine_region_sub_id;
                }
                if ( st->search_results_count < MB_SEARCH_MAX_RESULTS ) {
                    fill_hit ( &st->search_results[st->search_results_count],
                                st->search_engine_region_id,
                                st->search_engine_region_kind,
                                st->search_engine_region_sub_id,
                                abs_pos );
                    st->search_results_count++;
                }
                st->search_state = MEMBROWSER_SEARCH_STATE_DONE;
                /* Posuň scan_pos pro případný další step (pro pravidelnost
                 * stavů). */
                st->search_scan_pos = abs_pos + 1;
                goto chunk_done;  /* Vyskoč ze for + ven z while. */
            }

            case MEMBROWSER_SEARCH_MODE_PREV: {
                /* Sbírej všechny matches do limitu; po dokončení scanu
                 * vyber poslední match před cursor. Pre PREV mode se
                 * full scan stejně provede - cursor < scan_end už platí. */
                if ( abs_pos < ( uint64_t ) st->cursor_addr ) {
                    if ( st->search_results_count < MB_SEARCH_MAX_RESULTS ) {
                        fill_hit ( &st->search_results[st->search_results_count],
                                    st->search_engine_region_id,
                                    st->search_engine_region_kind,
                                    st->search_engine_region_sub_id,
                                    abs_pos );
                        st->search_results_count++;
                    }
                }
                break;
            }

            case MEMBROWSER_SEARCH_MODE_ALL: {
                if ( st->search_results_count < MB_SEARCH_MAX_RESULTS ) {
                    fill_hit ( &st->search_results[st->search_results_count],
                                st->search_engine_region_id,
                                st->search_engine_region_kind,
                                st->search_engine_region_sub_id,
                                abs_pos );
                    st->search_results_count++;
                    if ( st->search_results_count >= MB_SEARCH_MAX_RESULTS ) {
                        st->search_state = MEMBROWSER_SEARCH_STATE_DONE;
                        st->search_results_visible = 1;
                        goto chunk_done;
                    }
                }
                break;
            }
            } /* switch mode */
        }

        /* Posun scan_pos: read_len bajtů minus overlap (pattern_len - 1)
         * - jen pokud read_len byl plný chunk. Pro poslední okno (read_len
         * menší než chunk) bez overlapu. */
        uint64_t advance = ( uint64_t ) got;
        if ( ( uint64_t ) c->len > 1 && advance > ( uint64_t ) ( c->len - 1 ) ) {
            advance -= ( uint64_t ) ( c->len - 1 );
        }
        if ( advance == 0 ) advance = 1;
        st->search_scan_pos += advance;
        st->search_total_scanned += ( uint64_t ) got;

        if ( chunk_budget >= ( uint32_t ) got ) chunk_budget -= ( uint32_t ) got;
        else chunk_budget = 0;
    }

chunk_done:
    /* Po dokončení (DONE) post-process pro PREV - vyber nejbližší match
     * před cursor (= max addr v results, který < cursor). */
    if ( st->search_state == MEMBROWSER_SEARCH_STATE_DONE
         && st->search_mode == MEMBROWSER_SEARCH_MODE_PREV
         && st->search_results_count > 0 ) {
        uint64_t best = 0;
        int best_idx = -1;
        for ( int i = 0; i < st->search_results_count; i++ ) {
            if ( st->search_results[i].addr < ( uint64_t ) st->cursor_addr
                 && ( best_idx < 0 || st->search_results[i].addr > best ) ) {
                best = st->search_results[i].addr;
                best_idx = i;
            }
        }
        if ( best_idx >= 0 ) {
            st->cursor_addr = ( uint32_t ) best;
            st->last_match_addr = ( uint32_t ) best;
            st->last_match_valid = true;
        }
        /* PREV nereportuje result list (= UX: jen jeden hit). */
        st->search_results_count = 0;
    }

    /* Pro ALL nastav results_visible když je co zobrazit. */
    if ( st->search_state == MEMBROWSER_SEARCH_STATE_DONE
         && st->search_mode == MEMBROWSER_SEARCH_MODE_ALL
         && st->search_results_count > 0 ) {
        st->search_results_visible = 1;
    }
}


/* ---- Engine storage (file-static) -------------------------------------- */


/**
 * @brief File-static storage compiled patternu sdílená mezi _start a _step.
 *
 * Engine je single-thread / single-active-search; jedna instance stačí.
 * Lifetime: alokace v _start, free v _start nebo při shutdownu (TODO -
 * pro V4 se spoléháme na free v rámci dalšího _start volání).
 */
static st_MB_SEARCH_COMPILED s_engine_compiled_storage;

/**
 * @brief Getter na file-static engine storage (cross-function access).
 */
extern "C" st_MB_SEARCH_COMPILED *membrowser_search__compiled_get ( void )
{
    return &s_engine_compiled_storage;
}

/**
 * @brief Reset engine storage - free regex/expr handles, prepare for reuse.
 */
extern "C" void membrowser_search__compiled_reset ( void )
{
    membrowser_search_compiled_free ( &s_engine_compiled_storage );
}


/* ---- History helpers --------------------------------------------------- */


extern "C" void membrowser_search_history_push ( st_MEMBROWSER_STATE *st,
                                                 const char *pattern, int type )
{
    if ( !st || !pattern || pattern[0] == '\0' ) return;

    /* Find existing duplicate. */
    int existing = -1;
    for ( int i = 0; i < st->search_history_count; i++ ) {
        if ( std::strncmp ( st->search_history[i], pattern,
                            MB_SEARCH_PATTERN_SIZE ) == 0
             && st->search_history_types[i] == type ) {
            existing = i;
            break;
        }
    }

    if ( existing == 0 ) return;  /* Už je top, nemá smysl posouvat. */

    /* Vyhoď existing (= posuň zbytek o jedno doleva). */
    if ( existing > 0 ) {
        for ( int i = existing; i < st->search_history_count - 1; i++ ) {
            std::memcpy ( st->search_history[i], st->search_history[i + 1],
                          MB_SEARCH_PATTERN_SIZE );
            st->search_history_types[i] = st->search_history_types[i + 1];
        }
        st->search_history_count--;
    }

    /* Posuň [0..count-1] o jedno doprava (rotuj), klampuj na MAX-1. */
    int new_count = st->search_history_count + 1;
    if ( new_count > MB_SEARCH_HISTORY_SIZE ) new_count = MB_SEARCH_HISTORY_SIZE;
    /* Šift od konce. */
    for ( int i = new_count - 1; i > 0; i-- ) {
        std::memcpy ( st->search_history[i], st->search_history[i - 1],
                      MB_SEARCH_PATTERN_SIZE );
        st->search_history_types[i] = st->search_history_types[i - 1];
    }
    /* Vlož na 0. */
    std::strncpy ( st->search_history[0], pattern, MB_SEARCH_PATTERN_SIZE - 1 );
    st->search_history[0][MB_SEARCH_PATTERN_SIZE - 1] = '\0';
    st->search_history_types[0] = type;
    st->search_history_count = new_count;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
