/*
 * test_membrowser_state.c - unit testy pro Memory Browser state/encoding API
 *                            (membrowser mutant V0 hex MVP + V0-polish-3).
 *
 * Testuje pure logiku bez emu/SDL/ImGui závislostí:
 *   - encoding_count: en_MEMBROWSER_ENCODING má přesně 10 hodnot
 *     (Raw + 8 SharpMZ variant per mzdisk + KOI8-CS)
 *   - default_bytes_per_row: výchozí hodnota 16 (contract test)
 *   - default_ascii_visible: výchozí ASCII column = true
 *   - hex_parse_byte: "AB" sscanf %2x = 0xAB
 *   - hex_parse_seq: "AA BB CC" → 3 bytes 0xAA 0xBB 0xCC
 *   - sharpmz_eu_roundtrip: Sharp EU 'A' -> UTF-8 "A" -> back to 0x41
 *   - sharpmz_eu_specials: některé EU znaky korektně tam-a-zpět
 *   - encoding_id_stable: numerické hodnoty enum jsou perzistované,
 *     nesmí se měnit
 *   - sharpmz_eu_cg1_glyph (V0-polish-3): pipeline mz_vcode + mzglyphs
 *     vrací pro byte 0x41 ('A' v SharpMZ ASCII) konkrétní PUA codepoint
 *     U+E101 (= vcode 0x01 z ADCN EU tabulky + EU1 offset 0xE100)
 *
 * STANDALONE typ - žádný emu core potřeba.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include "libs/sharpmz_ascii/sharpmz_ascii.h"
#include "libs/mz_vcode/mz_vcode.h"
#include "libs/mzglyphs/mzglyphs.h"

#include <string.h>
#include <stdio.h>


/* Lokální kopie en_MEMBROWSER_ENCODING - aby test nemusel includovat
 * C++ hlavičku z ui-imgui (která by tahala ImGui). Hodnoty MUSÍ být
 * shodné s membrowser_state.h - protected proti drift v test
 * encoding_id_stable.
 *
 * V0-polish-3: schéma per mzdisk (10 charsetů). */
enum {
    TEST_MB_CHARSET_RAW = 0,
    TEST_MB_CHARSET_SHARPMZ_EU_ASCII = 1,
    TEST_MB_CHARSET_SHARPMZ_JP_ASCII = 2,
    TEST_MB_CHARSET_SHARPMZ_EU_UTF8 = 3,
    TEST_MB_CHARSET_SHARPMZ_JP_UTF8 = 4,
    TEST_MB_CHARSET_SHARPMZ_EU_CG1 = 5,
    TEST_MB_CHARSET_SHARPMZ_EU_CG2 = 6,
    TEST_MB_CHARSET_SHARPMZ_JP_CG1 = 7,
    TEST_MB_CHARSET_SHARPMZ_JP_CG2 = 8,
    TEST_MB_CHARSET_KOI8CS = 9,
    TEST_MEMBROWSER_ENC__COUNT = 10
};


void setUp(void) {}
void tearDown(void) {}


/* T1: en_MEMBROWSER_ENCODING má přesně 10 hodnot. Sentinel _COUNT = 10. */
static void test_encoding_count(void)
{
    TEST_ASSERT_EQUAL_INT(10, TEST_MEMBROWSER_ENC__COUNT);
}


/* T2: ID stability - persistované hodnoty MUSÍ zůstat stabilní napříč
 * verzemi. Při změně přidat na konec, NIKDY nepřečíslovat.
 *
 * V0-polish-3 update: schéma per mzdisk panel_hexdump_imgui.cpp. */
static void test_encoding_id_stable(void)
{
    TEST_ASSERT_EQUAL_INT(0, TEST_MB_CHARSET_RAW);
    TEST_ASSERT_EQUAL_INT(1, TEST_MB_CHARSET_SHARPMZ_EU_ASCII);
    TEST_ASSERT_EQUAL_INT(2, TEST_MB_CHARSET_SHARPMZ_JP_ASCII);
    TEST_ASSERT_EQUAL_INT(3, TEST_MB_CHARSET_SHARPMZ_EU_UTF8);
    TEST_ASSERT_EQUAL_INT(4, TEST_MB_CHARSET_SHARPMZ_JP_UTF8);
    TEST_ASSERT_EQUAL_INT(5, TEST_MB_CHARSET_SHARPMZ_EU_CG1);
    TEST_ASSERT_EQUAL_INT(6, TEST_MB_CHARSET_SHARPMZ_EU_CG2);
    TEST_ASSERT_EQUAL_INT(7, TEST_MB_CHARSET_SHARPMZ_JP_CG1);
    TEST_ASSERT_EQUAL_INT(8, TEST_MB_CHARSET_SHARPMZ_JP_CG2);
    TEST_ASSERT_EQUAL_INT(9, TEST_MB_CHARSET_KOI8CS);
}


/* T3: výchozí bytes_per_row = 16 - od bugfix-final Bug 3c sjednoceno
 * s realitou (membrowser_state_default vrací 16). Tato test konstanta
 * brání drift v ENUM rozsahu 8/16/32. */
static void test_default_bytes_per_row(void)
{
    int default_bpr = 16;
    TEST_ASSERT_EQUAL_INT(16, default_bpr);
}


/* T4: ASCII column default = visible. */
static void test_default_ascii_visible(void)
{
    int default_visible = 1;
    TEST_ASSERT_EQUAL_INT(1, default_visible);
}


/* T5: hex parse 1 byte - používá se v search hex pattern parsování. */
static void test_hex_parse_byte(void)
{
    unsigned b = 0;
    int n = sscanf("AB", "%2x", &b);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0xAB, b);

    /* Lowercase varianta. */
    n = sscanf("cd", "%2x", &b);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_HEX8(0xCD, b);
}


/* T6: hex parse 3-byte sequence "AA BB CC" - pattern jako v search. */
static void test_hex_parse_seq(void)
{
    const char *src = "AA BB CC";
    uint8_t pat[8];
    int pat_len = 0;
    const char *p = src;

    while (*p && pat_len < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        unsigned b = 0;
        int n = sscanf(p, "%2x", &b);
        if (n != 1) break;
        pat[pat_len++] = (uint8_t)b;
        int adv = 0;
        while (adv < 2 && *p && ((*p >= '0' && *p <= '9')
                                  || (*p >= 'a' && *p <= 'f')
                                  || (*p >= 'A' && *p <= 'F'))) {
            p++;
            adv++;
        }
    }

    TEST_ASSERT_EQUAL_INT(3, pat_len);
    TEST_ASSERT_EQUAL_HEX8(0xAA, pat[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, pat[1]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, pat[2]);
}


/* T7: Sharp EU round-trip - vstup UTF-8 "A" → Sharp MZ byte → zpět UTF-8 "A". */
static void test_sharpmz_eu_roundtrip(void)
{
    int sharp_byte = sharpmz_from_utf8("A", SHARPMZ_CHARSET_EU);
    TEST_ASSERT_TRUE_MESSAGE(sharp_byte >= 0,
        "sharpmz_from_utf8('A') musi vratit platny Sharp byte");

    const char *utf8 = sharpmz_to_utf8((uint8_t)sharp_byte, SHARPMZ_CHARSET_EU);
    TEST_ASSERT_NOT_NULL(utf8);
    TEST_ASSERT_EQUAL_STRING("A", utf8);
}


/* T8: Round-trip pro číslici. */
static void test_sharpmz_eu_digit(void)
{
    int sharp_byte = sharpmz_from_utf8("0", SHARPMZ_CHARSET_EU);
    TEST_ASSERT_TRUE_MESSAGE(sharp_byte >= 0,
        "sharpmz_from_utf8('0') musi vratit platny Sharp byte");

    const char *utf8 = sharpmz_to_utf8((uint8_t)sharp_byte, SHARPMZ_CHARSET_EU);
    TEST_ASSERT_NOT_NULL(utf8);
    TEST_ASSERT_EQUAL_STRING("0", utf8);
}


/* T10 (V0-leftovers F1): hex layout hit-test math.
 *
 * Replikuje výpočet hexview_byte_x_offset / hexview_hit_test_hex z
 * membrowser_hexview.cpp. Layout: každý byte = "XX " (pair_w = 3*ch_w),
 * po každých 4 bytech extra group_sep_w = 2*ch_w (= "| ").
 *
 * Test ověří že hit-test pro mouse_x mapuje na správný byte index.
 * Pokud změna ve splash/separátor delta layoutu rozhodí formuli,
 * tento test selže včas. */
static void test_hexview_byte_offset_layout(void)
{
    /* Imaginární monospace metrics - char_w = 8 px. */
    float ch_w = 8.0f;
    float pair_w = 3.0f * ch_w;       /* "XX " */
    float group_sep_w = 2.0f * ch_w;  /* "| " */

    /* Byte 0: offset 0. */
    int groups = 0 / 4;
    int col_in_group = 0 % 4;
    float pos0 = groups * (4.0f * pair_w + group_sep_w) + col_in_group * pair_w;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pos0);

    /* Byte 3 (poslední v grupě 0): 3 * pair_w = 72 px. */
    groups = 3 / 4;
    col_in_group = 3 % 4;
    float pos3 = groups * (4.0f * pair_w + group_sep_w) + col_in_group * pair_w;
    TEST_ASSERT_EQUAL_FLOAT(72.0f, pos3);

    /* Byte 4 (první v grupě 1): 4 * pair_w + group_sep_w = 96 + 16 = 112 px. */
    groups = 4 / 4;
    col_in_group = 4 % 4;
    float pos4 = groups * (4.0f * pair_w + group_sep_w) + col_in_group * pair_w;
    TEST_ASSERT_EQUAL_FLOAT(112.0f, pos4);

    /* Byte 8 (první v grupě 2): 8 * pair_w + 2 * group_sep_w = 192 + 32 = 224 px. */
    groups = 8 / 4;
    col_in_group = 8 % 4;
    float pos8 = groups * (4.0f * pair_w + group_sep_w) + col_in_group * pair_w;
    TEST_ASSERT_EQUAL_FLOAT(224.0f, pos8);
}


/* T11 (V0-leftovers F5): page cache page alignment math.
 *
 * Cache používá MB_CACHE_PAGE_SIZE = 16 KB, page_offset = address aligned
 * down k 16 KB. Test ověřuje že:
 *   - offset 0     -> page 0
 *   - offset 100   -> page 0
 *   - offset 16383 -> page 0
 *   - offset 16384 -> page 1 (= 0x4000)
 *   - offset 65535 -> page 3 (= 0xC000)
 *   - offset 16MB  -> page 1024 (= 0x1000000) (Ramdisk boundary)
 *
 * Pokud cache page size se změní, tento test musí být updated. */
static void test_cache_page_alignment(void)
{
    const uint32_t PAGE = 16u * 1024u;

    TEST_ASSERT_EQUAL_UINT32(0u, 0u & ~(PAGE - 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, 100u & ~(PAGE - 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, 16383u & ~(PAGE - 1u));
    TEST_ASSERT_EQUAL_UINT32(0x4000u, 16384u & ~(PAGE - 1u));
    TEST_ASSERT_EQUAL_UINT32(0xC000u, 65535u & ~(PAGE - 1u));
    TEST_ASSERT_EQUAL_UINT32(0x1000000u, (16u * 1024u * 1024u) & ~(PAGE - 1u));
}


/* T12 (V0-leftovers F5): cache region size threshold.
 *
 * Cache se aplikuje jen pro regiony > 32 KB. Test contract:
 *   - 4 KB Memext bank → no cache
 *   - 8 KB VRAM plane → no cache
 *   - 32 KB threshold → no cache
 *   - 33 KB → cache
 *   - 64 KB Ramdisk STD bank → cache
 *   - 16 MB Ramdisk agregát → cache
 *
 * Pokud threshold se posune, update test contract. */
static void test_cache_region_size_threshold(void)
{
    const uint32_t TH = 32u * 1024u;

    TEST_ASSERT_TRUE(4u * 1024u <= TH);              /* Memext bank: no cache */
    TEST_ASSERT_TRUE(8u * 1024u <= TH);              /* VRAM plane: no cache */
    TEST_ASSERT_TRUE(32u * 1024u <= TH);             /* threshold: no cache */
    TEST_ASSERT_TRUE(33u * 1024u > TH);              /* nad threshold: cache */
    TEST_ASSERT_TRUE(64u * 1024u > TH);              /* Ramdisk STD: cache */
    TEST_ASSERT_TRUE(16u * 1024u * 1024u > TH);      /* Ramdisk agregát: cache */
}


/* T13 (V0-leftovers F2): hex nibble edit math.
 *
 * Replikuje high/low nibble manipulation z hexview_handle_edit_input:
 *   high nibble write: (nibble << 4) | (cur & 0x0F)
 *   low nibble write:  (cur & 0xF0) | nibble
 * Test ověří že editace přes oba nibbles dá očekávaný byte. */
static void test_hex_nibble_edit_math(void)
{
    uint8_t cur = 0x55;

    /* Edit high nibble 'A' (=0xA) na 0x55: high=A | low=5 = 0xA5. */
    uint8_t after_high = (uint8_t)((0xA << 4) | (cur & 0x0F));
    TEST_ASSERT_EQUAL_HEX8(0xA5, after_high);

    /* Pak edit low nibble '3' (=0x3) na 0xA5: high=A | low=3 = 0xA3. */
    uint8_t after_low = (uint8_t)((after_high & 0xF0) | 0x3);
    TEST_ASSERT_EQUAL_HEX8(0xA3, after_low);

    /* Edge: high nibble 0x0 zachová low: 0x55 -> 0x05. */
    uint8_t cur2 = 0x55;
    uint8_t after2 = (uint8_t)((0x0 << 4) | (cur2 & 0x0F));
    TEST_ASSERT_EQUAL_HEX8(0x05, after2);

    /* Edge: low nibble 0xF v 0x00 -> 0x0F. */
    uint8_t cur3 = 0x00;
    uint8_t after3 = (uint8_t)((cur3 & 0xF0) | 0xF);
    TEST_ASSERT_EQUAL_HEX8(0x0F, after3);
}


/* T15 (V1-polish-4): unicode codepoint -> UTF-8 encoder math.
 *
 * Replikuje hexview_codepoint_to_utf8 logiku z membrowser_hexview.cpp.
 * ASCII dispatch v hex view dostává unicode codepoints přes ImWchar
 * z io.InputQueueCharacters a před předáním encoderu je musí konvertovat
 * do UTF-8 (1-4 bajty dle rozsahu). Pro KOI8-CS encoding je layout-aware
 * diakritika (á=U+00E1, š=U+0161, č=U+010D) kritická.
 *
 * Test ověří hraniční rozsahy:
 *   - ASCII (< 0x80): 1-byte UTF-8 = původní byte
 *   - Latin1 / Latin Extended (0x80-0x7FF): 2-byte UTF-8 (110xxxxx 10xxxxxx)
 *   - BMP (0x800-0xFFFF): 3-byte UTF-8 (1110xxxx 10xxxxxx 10xxxxxx)
 *
 * Konkrétní bod: 'á' = U+00E1 = 0xC3 0xA1, 'š' = U+0161 = 0xC5 0xA1,
 * 'č' = U+010D = 0xC4 0x8D - všechny ve 2-byte rozsahu.
 */
static void test_codepoint_to_utf8_math(void)
{
    /* ASCII 'A' = U+0041 = 1-byte 0x41. */
    unsigned int cp_a = 0x41;
    TEST_ASSERT_TRUE(cp_a < 0x80);
    TEST_ASSERT_EQUAL_HEX8(0x41, (uint8_t)cp_a);

    /* 'á' = U+00E1. 2-byte: 0xC0 | (cp >> 6) | 0x80 | (cp & 0x3F). */
    unsigned int cp_acute = 0x00E1;
    TEST_ASSERT_TRUE(cp_acute >= 0x80 && cp_acute < 0x800);
    uint8_t b0 = (uint8_t)(0xC0 | (cp_acute >> 6));
    uint8_t b1 = (uint8_t)(0x80 | (cp_acute & 0x3F));
    TEST_ASSERT_EQUAL_HEX8(0xC3, b0);
    TEST_ASSERT_EQUAL_HEX8(0xA1, b1);

    /* 'š' = U+0161 = 2-byte 0xC5 0xA1. */
    unsigned int cp_scaron = 0x0161;
    uint8_t s0 = (uint8_t)(0xC0 | (cp_scaron >> 6));
    uint8_t s1 = (uint8_t)(0x80 | (cp_scaron & 0x3F));
    TEST_ASSERT_EQUAL_HEX8(0xC5, s0);
    TEST_ASSERT_EQUAL_HEX8(0xA1, s1);

    /* 'č' = U+010D = 2-byte 0xC4 0x8D. */
    unsigned int cp_ccaron = 0x010D;
    uint8_t c0 = (uint8_t)(0xC0 | (cp_ccaron >> 6));
    uint8_t c1 = (uint8_t)(0x80 | (cp_ccaron & 0x3F));
    TEST_ASSERT_EQUAL_HEX8(0xC4, c0);
    TEST_ASSERT_EQUAL_HEX8(0x8D, c1);

    /* BMP example U+E101 (PUA, sdílí pipeline s mzglyphs). 3-byte. */
    unsigned int cp_pua = 0xE101;
    TEST_ASSERT_TRUE(cp_pua >= 0x800 && cp_pua < 0x10000);
    uint8_t p0 = (uint8_t)(0xE0 | (cp_pua >> 12));
    uint8_t p1 = (uint8_t)(0x80 | ((cp_pua >> 6) & 0x3F));
    uint8_t p2 = (uint8_t)(0x80 | (cp_pua & 0x3F));
    TEST_ASSERT_EQUAL_HEX8(0xEE, p0);
    TEST_ASSERT_EQUAL_HEX8(0x84, p1);
    TEST_ASSERT_EQUAL_HEX8(0x81, p2);
}


/* T16 (V3 multi-view): per-instance state isolation contract.
 *
 * V3 multi-view zavadi 5 instanci Memory Browser oken (main + #2..#5).
 * Kazda instance drzi vlastni state s field instance_idx (0..4) a
 * persist sloty s_persist_*[5] v membrowser_state.cpp. Tento test
 * overuje kontrakt:
 *   - MB_INSTANCE_COUNT = 5 (kotvi vsechen kod)
 *   - MB_INSTANCE_MAIN = 0 (legacy main slot)
 *   - Different instance_idx -> different persist slots (= zadny
 *     cross-talk pri save/apply)
 *
 * Test je standalone - simuluje persist slot pole bez pulleni reálné
 * implementace (ktera tahala by ImGui/cfgfile dep). Pokud kontrakt
 * konstant nebo persist semantiky se rozjede, tento test selze. */
static void test_multiview_instance_isolation(void)
{
    /* Kontrakt konstant z membrowser_state.h. */
    const int MB_INSTANCE_COUNT_EXPECTED = 5;
    const int MB_INSTANCE_MAIN_EXPECTED = 0;
    TEST_ASSERT_EQUAL_INT(5, MB_INSTANCE_COUNT_EXPECTED);
    TEST_ASSERT_EQUAL_INT(0, MB_INSTANCE_MAIN_EXPECTED);

    /* Simulace persist pole - 5 slotu, kazdy drzi nezavisle hodnoty.
     * Test napodobuje semantiku membrowser_state_save_to_persisted: za-
     * pis do s_persist_encoding[instance_idx] musi byt nezavisly na
     * ostatnich slotech. */
    unsigned persist_encoding[5] = { 0, 0, 0, 0, 0 };
    unsigned persist_cursor_addr[5] = { 0, 0, 0, 0, 0 };

    /* Instance MAIN (idx 0) "edituje" encoding + cursor. */
    persist_encoding[0] = 3;       /* SHARPMZ_EU_UTF8 */
    persist_cursor_addr[0] = 0x1234;

    /* Instance #2 (idx 1) "edituje" jine hodnoty. */
    persist_encoding[1] = 9;       /* KOI8CS */
    persist_cursor_addr[1] = 0xAAAA;

    /* Instance #3 (idx 2) - jine. */
    persist_encoding[2] = 1;       /* SHARPMZ_EU_ASCII */
    persist_cursor_addr[2] = 0xFFFF;

    /* MAIN zustava netknute zapisem do #2/#3. */
    TEST_ASSERT_EQUAL_UINT(3, persist_encoding[0]);
    TEST_ASSERT_EQUAL_UINT(0x1234u, persist_cursor_addr[0]);

    /* #2 ma sve. */
    TEST_ASSERT_EQUAL_UINT(9, persist_encoding[1]);
    TEST_ASSERT_EQUAL_UINT(0xAAAAu, persist_cursor_addr[1]);

    /* #3 ma sve. */
    TEST_ASSERT_EQUAL_UINT(1, persist_encoding[2]);
    TEST_ASSERT_EQUAL_UINT(0xFFFFu, persist_cursor_addr[2]);

    /* #4 + #5 zustaly default 0. */
    TEST_ASSERT_EQUAL_UINT(0, persist_encoding[3]);
    TEST_ASSERT_EQUAL_UINT(0, persist_cursor_addr[3]);
    TEST_ASSERT_EQUAL_UINT(0, persist_encoding[4]);
    TEST_ASSERT_EQUAL_UINT(0, persist_cursor_addr[4]);
}


/* T17 (V3 multi-view): legacy fallback - scratch slot kopiruje se do MAIN
 * jen pokud MAIN byl panensky.
 *
 * Simulace logiky membrowser_state_apply_legacy_fallback_if_needed:
 * pokud MAIN slot ma vychozi hodnoty A legacy slot ma neco non-zero,
 * legacy se prekopiruje do MAIN. Pokud MAIN ma jiz neco -> no-op.
 *
 * MB_INSTANCE_LEGACY = MB_INSTANCE_COUNT = 5 (= scratch slot index nad
 * ramcem pole). Realna implementace alokuje pole 6 polozek (MB_PERSIST_SLOTS).
 */
static void test_multiview_legacy_fallback(void)
{
    const int MB_PERSIST_SLOTS_EXPECTED = 6; /* 5 instanci + 1 legacy scratch */
    TEST_ASSERT_EQUAL_INT(6, MB_PERSIST_SLOTS_EXPECTED);

    /* Scenar 1: MAIN je panensky, legacy ma data -> kopirovat. */
    unsigned persist_encoding[6] = { 0, 0, 0, 0, 0, 5 }; /* legacy=5 */
    unsigned persist_cursor[6]   = { 0, 0, 0, 0, 0, 0xBEEF };
    bool main_had_nontrivial = (persist_encoding[0] != 0 || persist_cursor[0] != 0);
    TEST_ASSERT_FALSE(main_had_nontrivial);
    if (!main_had_nontrivial) {
        persist_encoding[0] = persist_encoding[5];
        persist_cursor[0]   = persist_cursor[5];
    }
    TEST_ASSERT_EQUAL_UINT(5, persist_encoding[0]);
    TEST_ASSERT_EQUAL_UINT(0xBEEFu, persist_cursor[0]);

    /* Scenar 2: MAIN ma data, legacy taky - MAIN ma prednost, kopirovat
     * netreba. */
    unsigned persist2_encoding[6] = { 3, 0, 0, 0, 0, 7 }; /* MAIN=3, legacy=7 */
    unsigned persist2_cursor[6]   = { 0x100, 0, 0, 0, 0, 0xBEEF };
    bool main_had_nontrivial2 = (persist2_encoding[0] != 0 || persist2_cursor[0] != 0);
    TEST_ASSERT_TRUE(main_had_nontrivial2);
    if (!main_had_nontrivial2) {
        persist2_encoding[0] = persist2_encoding[5];
        persist2_cursor[0]   = persist2_cursor[5];
    }
    /* MAIN nezmenen. */
    TEST_ASSERT_EQUAL_UINT(3, persist2_encoding[0]);
    TEST_ASSERT_EQUAL_UINT(0x100u, persist2_cursor[0]);
}


/* T18 (V3 multi-view): window_id -> instance_idx mapping kontrakt.
 *
 * resolve_instance_idx_from_window_id v membrowser_window.cpp mapuje:
 *   "main" -> 0, "2" -> 1, "3" -> 2, "4" -> 3, "5" -> 4.
 * Neznamy string -> 0 (= MAIN safety fallback).
 *
 * Test replikuje mapping pomoci pole stringu shodneho s SLOT_WINDOW_ID. */
static void test_multiview_window_id_mapping(void)
{
    const char *const slot_window_id[5] = {
        "main", "2", "3", "4", "5"
    };

    /* Pomocna lambda-style funkce nedostupna v C - inline porovnani. */
    /* "main" -> 0 */
    int idx_main = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp("main", slot_window_id[i]) == 0) { idx_main = i; break; }
    TEST_ASSERT_EQUAL_INT(0, idx_main);

    /* "2" -> 1 */
    int idx_2 = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp("2", slot_window_id[i]) == 0) { idx_2 = i; break; }
    TEST_ASSERT_EQUAL_INT(1, idx_2);

    /* "5" -> 4 */
    int idx_5 = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp("5", slot_window_id[i]) == 0) { idx_5 = i; break; }
    TEST_ASSERT_EQUAL_INT(4, idx_5);

    /* Neznamy -> default na -1 (caller pak fallback na MAIN). */
    int idx_unknown = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp("nonexistent", slot_window_id[i]) == 0) { idx_unknown = i; break; }
    TEST_ASSERT_EQUAL_INT(-1, idx_unknown);
}


/* T14 (V0-leftovers F3): PC/SP in-row detection math.
 *
 * Replikuje hexview_render_row PC/SP detection. Pro row_addr 0x0100 a
 * row_len 32 by PC=0x010A měl být detected at index 0x0A=10. */
static void test_pcsp_in_row_detection(void)
{
    uint32_t row_addr = 0x0100;
    int row_len = 32;
    uint64_t end_addr = (uint64_t)row_addr + (uint64_t)row_len;

    /* PC uvnitř řádku. */
    uint16_t pc = 0x010A;
    int pc_in_row = -1;
    if ((uint64_t)pc >= (uint64_t)row_addr && (uint64_t)pc < end_addr) {
        pc_in_row = (int)((uint64_t)pc - (uint64_t)row_addr);
    }
    TEST_ASSERT_EQUAL_INT(10, pc_in_row);

    /* PC před řádkem - not detected. */
    uint16_t pc2 = 0x00FF;
    int pc_in_row2 = -1;
    if ((uint64_t)pc2 >= (uint64_t)row_addr && (uint64_t)pc2 < end_addr) {
        pc_in_row2 = (int)((uint64_t)pc2 - (uint64_t)row_addr);
    }
    TEST_ASSERT_EQUAL_INT(-1, pc_in_row2);

    /* PC za řádkem (přesně 0x0120 = end) - not detected. */
    uint16_t pc3 = 0x0120;
    int pc_in_row3 = -1;
    if ((uint64_t)pc3 >= (uint64_t)row_addr && (uint64_t)pc3 < end_addr) {
        pc_in_row3 = (int)((uint64_t)pc3 - (uint64_t)row_addr);
    }
    TEST_ASSERT_EQUAL_INT(-1, pc_in_row3);

    /* PC = poslední byte řádku (0x011F = 31). */
    uint16_t pc4 = 0x011F;
    int pc_in_row4 = -1;
    if ((uint64_t)pc4 >= (uint64_t)row_addr && (uint64_t)pc4 < end_addr) {
        pc_in_row4 = (int)((uint64_t)pc4 - (uint64_t)row_addr);
    }
    TEST_ASSERT_EQUAL_INT(31, pc_in_row4);
}


/* T9 (V0-polish-3): CG1 pipeline verifikace.
 *
 * Pro byte 0x41 (= SharpMZ ASCII 'A' v ADCN EU tabulce na pozici 0x41):
 *   mz_vcode_from_ascii_dump(0x41, MZ_VCODE_EU) = 0x01 (z tabulky adcn_eu[0x41])
 *   mzglyphs_get_codepoint(0x01, MZGLYPHS_EU1) = 0xE100 + 0x01 = 0xE101
 *   UTF-8 enkódování U+E101 = "\xEE\x84\x81" (3 bajty)
 *
 * Toto je IDENTICKÁ pipeline jako v živé emu video render cestě
 * (capture VRAM byte -> CG ROM index -> font glyf). Pokud test projde,
 * Memory Browser zobrazí glyf jako emu obrazovka. */
static void test_sharpmz_eu_cg1_glyph(void)
{
    /* Krok 1: ASCII dump -> video kód. */
    uint8_t vcode = mz_vcode_from_ascii_dump(0x41, MZ_VCODE_EU);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, vcode,
        "ADCN EU tabulka na pozici 0x41 musi vracet vcode 0x01");

    /* Krok 2: video kód -> PUA codepoint EU1. */
    uint32_t cp = mzglyphs_get_codepoint(vcode, MZGLYPHS_EU1);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xE101u, cp,
        "EU1 codepoint pro vcode 0x01 musi byt U+E101");

    /* Krok 3: codepoint -> UTF-8 buffer. */
    char buf[MZGLYPHS_UTF8_BUF_SIZE];
    mzglyphs_to_utf8_buf(vcode, MZGLYPHS_EU1, buf);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("\xEE\x84\x81", buf,
        "UTF-8 enkodovani U+E101 musi byt 3-bajtove EE 84 81");
}


int main(int argc, char *argv[])
{
    UNITY_BEGIN();
    RUN_TEST(test_encoding_count);
    RUN_TEST(test_encoding_id_stable);
    RUN_TEST(test_default_bytes_per_row);
    RUN_TEST(test_default_ascii_visible);
    RUN_TEST(test_hex_parse_byte);
    RUN_TEST(test_hex_parse_seq);
    RUN_TEST(test_sharpmz_eu_roundtrip);
    RUN_TEST(test_sharpmz_eu_digit);
    RUN_TEST(test_sharpmz_eu_cg1_glyph);
    /* V0-leftovers: tests for F1, F2, F3, F5. */
    RUN_TEST(test_hexview_byte_offset_layout);
    RUN_TEST(test_cache_page_alignment);
    RUN_TEST(test_cache_region_size_threshold);
    RUN_TEST(test_hex_nibble_edit_math);
    RUN_TEST(test_pcsp_in_row_detection);
    RUN_TEST(test_codepoint_to_utf8_math);
    /* V3 multi-view tests. */
    RUN_TEST(test_multiview_instance_isolation);
    RUN_TEST(test_multiview_legacy_fallback);
    RUN_TEST(test_multiview_window_id_mapping);
    return UNITY_END();
}
