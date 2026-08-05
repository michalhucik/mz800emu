/**
 * @file membrowser_encoding.cpp
 * @brief Encoding dropdown + per-byte ASCII column rendering - implementace.
 *
 * V0-polish-3: dispatch dle mzdisk panel_hexdump_imgui.cpp.
 *
 * CG charset pipeline (= cíl - identický výstup s živým emu displayem):
 *   uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_EU/JP );
 *   mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU1/EU2/JP1/JP2, buf );
 *
 * UTF-8 / ASCII charsety přes sharpmz_ascii lib:
 *   sharpmz_eu_convert_to_UTF8 ( byte, ... ) / sharpmz_jp_convert_to_UTF8
 *   sharpmz_convert_to_ASCII / sharpmz_jp_convert_to_ASCII
 *
 * KOI8-CS přes sharpmz_koi8cs lib (mzkoi8cs_to_utf8).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_encoding.h"
#include "membrowser_state.h"

#include "i18n.h"

/* V4.1+: encoding-aware tolower využívá GLib Unicode helpers
 * (g_unichar_tolower, g_utf8_get_char_validated, g_unichar_to_utf8). */
#include <glib.h>

extern "C" {
#include "libs/sharpmz_ascii/sharpmz_ascii.h"  /* sharpmz_cnv_from / cnv_to */
#include "libs/sharpmz_ascii/sharpmz_utf8.h"   /* sharpmz_to_utf8 / from_utf8 */
#include "libs/sharpmz_koi8cs/sharpmz_koi8cs.h"
#include "libs/mz_vcode/mz_vcode.h"
#include "libs/mzglyphs/mzglyphs.h"
}

#include <cstring>


extern "C" const char *membrowser_encoding_label ( int encoding_id )
{
    switch ( encoding_id ) {
        case MB_CHARSET_RAW:                return _( "Raw" );
        case MB_CHARSET_SHARPMZ_EU_ASCII:   return _( "SharpMZ-EU -> ASCII" );
        case MB_CHARSET_SHARPMZ_JP_ASCII:   return _( "SharpMZ-JP -> ASCII" );
        case MB_CHARSET_SHARPMZ_EU_UTF8:    return _( "SharpMZ-EU -> UTF-8" );
        case MB_CHARSET_SHARPMZ_JP_UTF8:    return _( "SharpMZ-JP -> UTF-8" );
        case MB_CHARSET_SHARPMZ_EU_CG1:     return _( "SharpMZ-EU -> MZ-CG1" );
        case MB_CHARSET_SHARPMZ_EU_CG2:     return _( "SharpMZ-EU -> MZ-CG2" );
        case MB_CHARSET_SHARPMZ_JP_CG1:     return _( "SharpMZ-JP -> MZ-CG1" );
        case MB_CHARSET_SHARPMZ_JP_CG2:     return _( "SharpMZ-JP -> MZ-CG2" );
        case MB_CHARSET_KOI8CS:             return _( "KOI8-CS" );
        default:                            return "?";
    }
}


/* Statický buffer pro single byte rendering. Max 7 bajtů + NUL stačí na
 * libovolný UTF-8 znak (max 4 bajty) i pro CG variantu (3 bajty PUA). */
static thread_local char s_byte_buf[8];

/* UTF-8 replacement character U+FFFD - sharpmz_to_utf8 vrací pro znaky
 * bez Unicode ekvivalentu (graphics chars). V "UTF-8" encoding modech
 * je akceptovaný (zobrazí jako "?"); v "ASCII" modech filtrujeme na "." */
#define MB_UTF8_REPLACEMENT "\xEF\xBF\xBD"

/* Helper: vrátí true pokud UTF-8 řetězec je 1-byte čisté tisknutelné ASCII. */
static inline int is_single_printable_ascii ( const char *s )
{
    if ( !s || !s[0] ) return 0;
    unsigned char c = ( unsigned char ) s[0];
    if ( c < 0x20 || c > 0x7E ) return 0;
    return s[1] == '\0';
}


extern "C" const char *membrowser_encoding_byte_to_utf8 ( uint8_t byte, int encoding_id )
{
    switch ( encoding_id ) {

        case MB_CHARSET_RAW:
            if ( byte >= 0x20 && byte <= 0x7E ) {
                s_byte_buf[0] = ( char ) byte;
                s_byte_buf[1] = '\0';
                return s_byte_buf;
            }
            return ".";

        case MB_CHARSET_SHARPMZ_EU_ASCII: {
            /* SharpMZ-EU -> ASCII (jednobajtová konverze).
             * sharpmz_cnv_from je dedikovaný 1-byte EU helper z lib. */
            uint8_t c = sharpmz_cnv_from ( byte );
            if ( c >= 0x20 && c <= 0x7E ) {
                s_byte_buf[0] = ( char ) c;
                s_byte_buf[1] = '\0';
                return s_byte_buf;
            }
            return ".";
        }

        case MB_CHARSET_SHARPMZ_JP_ASCII: {
            /* SharpMZ-JP -> ASCII: derivováno z sharpmz_to_utf8 JP -
             * akceptujeme pouze pokud výsledek je jednobajtové
             * tisknutelné ASCII (jinak grafický znak / katakana). */
            const char *utf = sharpmz_to_utf8 ( byte, SHARPMZ_CHARSET_JP );
            if ( is_single_printable_ascii ( utf ) ) {
                s_byte_buf[0] = utf[0];
                s_byte_buf[1] = '\0';
                return s_byte_buf;
            }
            return ".";
        }

        case MB_CHARSET_SHARPMZ_EU_UTF8: {
            /* Plná UTF-8 konverze EU. Pro znaky bez Unicode mapping
             * (graphics) lib vrátí U+FFFD - filtrujeme na "." */
            const char *utf = sharpmz_to_utf8 ( byte, SHARPMZ_CHARSET_EU );
            if ( !utf || std::strcmp ( utf, MB_UTF8_REPLACEMENT ) == 0 ) {
                return ".";
            }
            std::strncpy ( s_byte_buf, utf, 7 );
            s_byte_buf[7] = '\0';
            return s_byte_buf;
        }

        case MB_CHARSET_SHARPMZ_JP_UTF8: {
            const char *utf = sharpmz_to_utf8 ( byte, SHARPMZ_CHARSET_JP );
            if ( !utf || std::strcmp ( utf, MB_UTF8_REPLACEMENT ) == 0 ) {
                return ".";
            }
            std::strncpy ( s_byte_buf, utf, 7 );
            s_byte_buf[7] = '\0';
            return s_byte_buf;
        }

        case MB_CHARSET_SHARPMZ_EU_CG1: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_EU );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU1, s_byte_buf );
            return s_byte_buf;
        }

        case MB_CHARSET_SHARPMZ_EU_CG2: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_EU );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU2, s_byte_buf );
            return s_byte_buf;
        }

        case MB_CHARSET_SHARPMZ_JP_CG1: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_JP );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_JP1, s_byte_buf );
            return s_byte_buf;
        }

        case MB_CHARSET_SHARPMZ_JP_CG2: {
            uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_JP );
            mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_JP2, s_byte_buf );
            return s_byte_buf;
        }

        case MB_CHARSET_KOI8CS:
            return mzkoi8cs_to_utf8 ( byte, MZKOI8CS_CHARSET_KOI8CS );

        default:
            return ".";
    }
}


extern "C" bool membrowser_encoding_utf8_to_byte ( const char *utf8, int encoding_id, uint8_t *out )
{
    if ( !utf8 || !out ) return false;

    switch ( encoding_id ) {
        case MB_CHARSET_RAW: {
            /* Raw - jen tisknutelné ASCII přímo. */
            unsigned char c = ( unsigned char ) utf8[0];
            if ( c >= 0x20 && c <= 0x7E ) {
                *out = c;
                return true;
            }
            return false;
        }

        case MB_CHARSET_SHARPMZ_EU_ASCII:
        case MB_CHARSET_SHARPMZ_EU_UTF8: {
            int r = sharpmz_from_utf8 ( utf8, SHARPMZ_CHARSET_EU );
            if ( r < 0 ) return false;
            *out = ( uint8_t ) r;
            return true;
        }

        case MB_CHARSET_SHARPMZ_JP_ASCII:
        case MB_CHARSET_SHARPMZ_JP_UTF8: {
            int r = sharpmz_from_utf8 ( utf8, SHARPMZ_CHARSET_JP );
            if ( r < 0 ) return false;
            *out = ( uint8_t ) r;
            return true;
        }

        case MB_CHARSET_SHARPMZ_EU_CG1:
        case MB_CHARSET_SHARPMZ_EU_CG2:
        case MB_CHARSET_SHARPMZ_JP_CG1:
        case MB_CHARSET_SHARPMZ_JP_CG2:
            /* CG (display kód) reverse path: mz_vcode tabulka není
             * jednoznačně reverzibilní (více ASCII -> stejný vcode),
             * navíc PUA UTF-8 znaky nemají platnou reverse cestu na byte
             * paměti. V0 nepodporujeme edit přes ASCII column v CG
             * encodingu - uživatel musí přepnout na hex edit. */
            return false;

        case MB_CHARSET_KOI8CS: {
            /* mzkoi8cs_utf8_to_koi8cs vrací 0x20 pro neznámé znaky -
             * to není chyba detekovatelná z návratu, ale pro V0 přijímáme. */
            uint8_t k = mzkoi8cs_utf8_to_koi8cs ( utf8 );
            *out = k;
            return true;
        }

        default:
            return false;
    }
}


extern "C" bool membrowser_encoding_koi8cs_table_missing ( void )
{
    /* V0-leftovers F6: runtime sanity check tabulky. KOI8-CS byte 0xC1
     * by měl mapovat na 'á' (U+00E1, dvoubajtový UTF-8 "\xC3\xA1"). Pokud
     * lib vrátí identity (1 byte 0xC1 jako single-byte string nebo
     * netisknutelné), tabulka je nenačtená / poškozená.
     *
     * Cache výsledku - test stačí jednou per session, opakované volání
     * pak vrací cached flag (lib state se za běhu nemění). */
    static bool s_tested = false;
    static bool s_missing = false;
    if ( s_tested ) return s_missing;

    int converted = 0, printable = 0;
    const char *s = mzkoi8cs_koi8cs_to_utf8 ( 0xC1, &converted, &printable );
    /* Validní mapping: dvoubajtový UTF-8 začínající 0xC3 (= U+00xx range). */
    if ( !s || !converted || ( unsigned char ) s[0] != 0xC3 ) {
        s_missing = true;
    } else {
        s_missing = false;
    }
    s_tested = true;
    return s_missing;
}

/* V4.1+: encoding-aware tolower per-encoding tabulky. Lazy-init při
 * prvním volání pro dané encoding_id. Subsequent volání = O(1) lookup.
 *
 * Tabulka mapuje byte 0x00-0xFF na jeho lowercase ekvivalent v rámci
 * encodingu. Pro byte které není písmeno nebo nemá uppercase/lowercase
 * pair v daném encodingu, tabulka[byte] == byte (= identity = no fold).
 *
 * Build algoritmus per byte b:
 *   1. UTF-8 = byte_to_utf8(b, encoding)
 *   2. gunichar cp = utf8 → unicode
 *   3. cp_lo = g_unichar_tolower(cp)
 *   4. utf8_lo = cp_lo → utf8
 *   5. byte_lo = utf8_to_byte(utf8_lo, encoding)
 *   6. tabulka[b] = byte_lo (nebo b při selhání) */
static uint8_t s_tolower_tables[MEMBROWSER_ENC__COUNT][256];
static bool s_tolower_tables_built[MEMBROWSER_ENC__COUNT] = { false };


/* CG varianty a UTF8 varianty SharpMZ encodingů mají identický byte
 * layout jako jejich _ASCII counterpart - liší se jen render path
 * (= UTF-8 multibyte vs CG-ROM PUA glyph vs ASCII). Pro tolower (=
 * byte-level operace) je canonical encoding ten _ASCII. Tato funkce
 * aliasuje varianty na canonical pro tolower lookup; pro encodingy
 * bez varianty vrací sám encoding_id. */
static int tolower_canonical_encoding ( int encoding_id )
{
    switch ( encoding_id ) {
        case MB_CHARSET_SHARPMZ_EU_UTF8:
        case MB_CHARSET_SHARPMZ_EU_CG1:
        case MB_CHARSET_SHARPMZ_EU_CG2:
            return MB_CHARSET_SHARPMZ_EU_ASCII;
        case MB_CHARSET_SHARPMZ_JP_UTF8:
        case MB_CHARSET_SHARPMZ_JP_CG1:
        case MB_CHARSET_SHARPMZ_JP_CG2:
            return MB_CHARSET_SHARPMZ_JP_ASCII;
        default:
            return encoding_id;
    }
}


static void build_tolower_table ( int encoding_id )
{
    if ( encoding_id < 0 || encoding_id >= MEMBROWSER_ENC__COUNT ) return;

    uint8_t *tbl = s_tolower_tables[encoding_id];

    for ( int b = 0; b < 256; b++ ) {
        tbl[b] = ( uint8_t ) b;  /* Identity default. */

        const char *u = membrowser_encoding_byte_to_utf8 ( ( uint8_t ) b,
                                                            encoding_id );
        if ( !u || !u[0] ) continue;

        /* Parse UTF-8 codepoint. g_utf8_get_char_validated vrátí
         * (gunichar)-1 nebo -2 při invalid UTF-8. */
        gunichar cp = g_utf8_get_char_validated ( u, -1 );
        if ( cp == ( gunichar ) -1 || cp == ( gunichar ) -2 ) continue;

        /* Skip non-letter codepoints - g_unichar_tolower je no-op pro ně,
         * ale explicit early-out šetří jeden encoding round-trip. */
        if ( !g_unichar_isupper ( cp ) ) continue;

        gunichar cp_lo = g_unichar_tolower ( cp );
        if ( cp_lo == cp ) continue;

        /* Convert lowercase codepoint zpět na UTF-8 string. */
        char utf8_lo[8];
        gint n = g_unichar_to_utf8 ( cp_lo, utf8_lo );
        if ( n <= 0 || n >= ( gint ) sizeof ( utf8_lo ) ) continue;
        utf8_lo[n] = '\0';

        /* Reverse-lookup byte. Pro CG variants encoding_utf8_to_byte
         * vrátí false - v tomto kódu nedosažitelný case, protože jsme
         * v build_tolower_table který se volá s canonical encoding_id. */
        uint8_t b_lo = 0;
        if ( membrowser_encoding_utf8_to_byte ( utf8_lo, encoding_id, &b_lo ) ) {
            tbl[b] = b_lo;
        }
    }
    s_tolower_tables_built[encoding_id] = true;
}


extern "C" uint8_t membrowser_encoding_tolower ( uint8_t byte, int encoding_id )
{
    /* CG/UTF8 varianty: pro byte-level operace canonical = _ASCII counterpart. */
    int enc = tolower_canonical_encoding ( encoding_id );
    if ( enc < 0 || enc >= MEMBROWSER_ENC__COUNT ) return byte;
    if ( !s_tolower_tables_built[enc] ) {
        build_tolower_table ( enc );
    }
    return s_tolower_tables[enc][byte];
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
