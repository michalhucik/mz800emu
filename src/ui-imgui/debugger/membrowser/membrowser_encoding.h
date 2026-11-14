/**
 * @file membrowser_encoding.h
 * @brief Encoding dropdown + per-byte ASCII column rendering.
 *
 * V0-polish-3: refactor na 10 encodings dle mzdisk panel_hexdump_imgui.cpp
 * referenční implementace + KOI8-CS jako extra (last position).
 *
 * Encoding pipeline pro CG charsets:
 *   uint8_t vcode = mz_vcode_from_ascii_dump ( byte, MZ_VCODE_EU/JP );
 *   mzglyphs_to_utf8_buf ( vcode, MZGLYPHS_EU1/EU2/JP1/JP2, buf );
 *
 * Dispatch nad sharpmz_ascii (sharpmz_to_utf8, vč. KOI8-CS) +
 * mz_vcode + mzglyphs knihovny. Core hex view kód NESmí znát konkrétní
 * charset_t enum konkrétních libs - jen integer encoding_id z
 * en_MEMBROWSER_ENCODING. Dispatch je zapouzdřen v tomto modulu.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_ENCODING_H
#define MEMBROWSER_ENCODING_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vrátí lokalizovaný název encodingu pro UI dropdown.
 *
 * @param encoding_id en_MEMBROWSER_ENCODING.
 * @return Pointer na lokalizovaný řetězec přes _() - nikdy NULL.
 *         Při neznámém ID vrátí "?".
 */
const char *membrowser_encoding_label ( int encoding_id );

/**
 * @brief Konvertuje single byte na UTF-8 řetězec dle encodingu.
 *
 * Pro Raw / ASCII varianty vrací jednoznakový řetězec ("." pro
 * netisknutelné). Pro UTF-8 a MZ-CG režimy může vrátit vícebajtový
 * UTF-8 řetězec (CG varianty vždy 3-bajtový UTF-8 z PUA U+E100-E4FF).
 *
 * @param byte         Vstupní byte (0x00-0xFF).
 * @param encoding_id  en_MEMBROWSER_ENCODING.
 * @return Pointer na (statický nebo session-stable) UTF-8 řetězec.
 *         Nikdy NULL.
 *
 * @note Funkce není reentrantní (statický buffer thread_local).
 */
const char *membrowser_encoding_byte_to_utf8 ( uint8_t byte, int encoding_id );

/**
 * @brief Konvertuje UTF-8 znak zpět na MZ byte dle encodingu.
 *
 * Použito v ASCII edit režimu - validuje, že znak je reprezentovatelný
 * v aktuálním encodingu.
 *
 * @param utf8         UTF-8 vstupní řetězec (min 1 znak).
 * @param encoding_id  en_MEMBROWSER_ENCODING.
 * @param[out] out     Výstupní byte při úspěchu.
 * @return true pokud konverze úspěšná a out je naplněno; false jinak
 *         (neznámý encoding, znak mimo encoding, ...).
 *
 * @note Pro CG encodingy V0 reverse path neexistuje (display kód -> ASCII
 *       není 1:1, tabulka mz_vcode není reversible). Vrací false.
 */
bool membrowser_encoding_utf8_to_byte ( const char *utf8, int encoding_id, uint8_t *out );

/**
 * @brief Vrátí true pokud KOI8-CS tabulka v lib není načtena.
 *
 * Status row pak ukáže warning. V0 returns false (sharpmz_ascii KOI8-CS je
 * importována jako V-1a, tabulka je vždy dostupná). Hook ponechaný
 * pro budoucí build varianty (např. minimal lib bez tabulky).
 *
 * @return true = KOI8-CS bez tabulky (warning v UI), false = OK.
 */
bool membrowser_encoding_koi8cs_table_missing ( void );


/**
 * @brief Encoding-aware case fold (uppercase → lowercase) per byte.
 *
 * Pro daný byte vrátí jeho "lowercase ekvivalent" v rámci kódování:
 *  - Raw: foldne Latin A-Z (0x41-0x5A → 0x61-0x7A); ostatní beze změny.
 *  - SharpMZ EU/JP (ASCII / UTF8 / CG1 / CG2 varianty - sdílí stejný
 *    byte layout, jen jiný render): foldne podle SharpMZ EU/JP charset.
 *  - KOI8-CS: foldne velké Czech/Slovak chars (0xE0-0xFA) na malé
 *    (0xC0-0xDA) + Latin A-Z. Per koi8cs_table mapping.
 *
 * Algoritmus: byte → UTF-8 (membrowser_encoding_byte_to_utf8) →
 * gunichar codepoint → g_unichar_tolower → UTF-8 →
 * reverse byte (membrowser_encoding_utf8_to_byte). Pokud kterýkoliv
 * krok selže (= nereprezentovatelné), vrátí původní byte.
 *
 * Internal: lazy-init per-encoding 256B tabulka při prvním volání pro
 * dané encoding_id. Subsequent volání = O(1) lookup.
 *
 * CG varianty se interně mapují na canonical _ASCII variant (mají
 * identický byte layout, jen jiný render), takže fold funguje i pro
 * uživatele kteří view nastaví na CG.
 *
 * Thread-safety: UI vlákno only (encoding lib funkce taky UI-only).
 *
 * @param byte         Vstupní byte 0x00-0xFF.
 * @param encoding_id  en_MEMBROWSER_ENCODING.
 * @return Lowercase ekvivalent v encodingu, nebo původní byte pokud
 *         fold nedává smysl (= byte není písmeno, nebo encoding nemá
 *         reverse path).
 */
uint8_t membrowser_encoding_tolower ( uint8_t byte, int encoding_id );

#ifdef __cplusplus
}
#endif

#endif /* MEMBROWSER_ENCODING_H */
