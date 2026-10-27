/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_suc80.c
 * @brief Standalone testy čistého jádra 80sloupcového kitu SUC (suc80_core).
 *
 * Pokrytí:
 *  - latch: bit D4 zapsané hodnoty (28h, 50h, ostatní bity bez vlivu),
 *    návratová hodnota "režim se změnil", /RESET -> 40 sloupců;
 *  - mapování adres v 80 sloupcích: první a poslední znak, sudé a liché sloupce,
 *    bajt 0D400h se nepoužije nikdy, obě varianty granularity atributů;
 *  - kreslení řádku v 80 sloupcích proti ručně spočítaným bodům, sada 2 přes
 *    bit 7 atributu, 0D400h a 0DC00h se do obrazu nedostanou;
 *  - kreslení v 40 sloupcích proti referenčnímu přepisu
 *    framebuffer_update_MZ700_screen_row (mz700_framebuffer.c ř. 80-144);
 *  - round-trip a chyby serializace stavu.
 *
 * Standalone - nepotřebuje emulator core, jen suc80_core.c.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "emulator/mzarch/mz700/devices/suc80/suc80_core.h"


/* ===== Fixture ===== */

/**
 * @brief Barevná mapa jako c_MZ700_COLORMAP (mz700_framebuffer.c ř. 54):
 *        index 0-7 -> hodnota IGRB v pixbuf.
 */
static const uint8_t c_COLORMAP[8] = { 0, 9, 10, 11, 12, 13, 14, 15 };

static st_SUC80 g_s;
static uint8_t  g_vram[SUC80_VRAM_SIZE];
static uint8_t  g_cg[SUC80_CG_SIZE];

/**
 * @brief Vzor syntetického generátoru: v rámci jedné sady a mikrořádku má
 *        každý z 256 znaků jiný bajt, sada 2 se od sady 1 liší bitem 7.
 */
static uint8_t cg_pattern(unsigned set, unsigned code, unsigned line)
{
    return (uint8_t) (code + 29u * line + 0x80u * set);
}


void setUp(void)
{
    suc80_init(&g_s);
    memset(g_vram, 0, sizeof(g_vram));
    for (unsigned set = 0; set < 2; set++) {
        for (unsigned code = 0; code < 256; code++) {
            for (unsigned line = 0; line < 8; line++) {
                g_cg[set * 0x800 + code * 8 + line] = cg_pattern(set, code, line);
            }
        }
    }
}


void tearDown(void)
{
}


/** @brief Deterministický pseudonáhodný obsah celé VRAM (LCG). */
static void fill_vram_pseudorandom(uint32_t seed)
{
    for (unsigned i = 0; i < SUC80_VRAM_SIZE; i++) {
        seed = seed * 1103515245u + 12345u;
        g_vram[i] = (uint8_t) (seed >> 16);
    }
}


/**
 * @brief Referenční 40sloupcové kreslení - přepis framebuffer_update_MZ700_screen_row
 *        (src/emulator/mzarch/mz700/gdg/mz700_framebuffer.c ř. 80-144).
 *
 * Výstupní ukazatel tam je `pixels + beam_row * VIDEO_DISPLAY_WIDTH +
 * VIDEO_BORDER_LEFT_WIDTH` (ř. 86), tj. začátek plátna bez levého okraje;
 * `pos_Y = beam_row - VIDEO_BEAM_CANVAS_FIRST_ROW` (ř. 88) odpovídá zde @p line.
 */
static void ref_mz700_row40(const uint8_t *VRAM, const uint8_t *CGROM, unsigned line, uint8_t *p)
{
    const uint8_t *VRAM_CHAR = &VRAM[0x000];          /* MZ700_VRAM_CHAR, ř. 57 */
    const uint8_t *VRAM_ATTR = &VRAM[0x800];          /* MZ700_VRAM_ATTR, ř. 58 */
    unsigned vram_addr = (line / 8) * 40;             /* ř. 89, CHARS_WIDTH 40 */
    unsigned char_line = line % 8;                    /* ř. 90 */
    const uint8_t *vram_data = &VRAM_CHAR[vram_addr];
    const uint8_t *vram_attr = &VRAM_ATTR[vram_addr];

    for (unsigned char_x = 0; char_x < 40; char_x++) {
        unsigned attr_fgc = (*vram_attr >> 4) & 0x07;           /* ř. 97 */
        unsigned attr_bgc = *vram_attr & 0x07;                  /* ř. 98 */
        unsigned attr_grp = ((*vram_attr << 4) & 0x0800);       /* ř. 99 */
        unsigned cg_addr = attr_grp | (*vram_data * 8);         /* ř. 101 */
        unsigned mask = CGROM[cg_addr + char_line];             /* ř. 102 */
        vram_data++;
        vram_attr++;
        uint8_t fgc = c_COLORMAP[attr_fgc];
        uint8_t bgc = c_COLORMAP[attr_bgc];
        /* ř. 111-140: bit 0 = levý bod, každý bod zdvojený */
        for (unsigned b = 0; b < 8; b++) {
            p[2 * b] = (mask & 1) ? fgc : bgc;
            p[2 * b + 1] = p[2 * b];
            mask >>= 1;
        }
        p += 16;                                                /* ř. 142 */
    }
}


/* ===================================================================== */
/* Latch a reset                                                         */
/* ===================================================================== */

/** @brief Výchozí stav po zapnutí: 40 sloupců, atributy po znacích. */
void test_init_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
    TEST_ASSERT_EQUAL_INT(SUC80_ATTR_PER_CHAR, suc80_get_attr_mode(&g_s));
}


/** @brief 50h zapne 80 sloupců, 28h vrátí 40; návratová hodnota hlásí změnu. */
void test_latch_28h_50h(void)
{
    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0x50));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_80, suc80_get_mode(&g_s));
    TEST_ASSERT_FALSE(suc80_latch_write(&g_s, 0x50));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_80, suc80_get_mode(&g_s));

    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0x28));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
    TEST_ASSERT_FALSE(suc80_latch_write(&g_s, 0x28));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
}


/** @brief Rozhoduje jen bit 4; ostatní bity (i 0FFh / 0EFh) bez vlivu. */
void test_latch_only_bit4(void)
{
    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0x10));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_80, suc80_get_mode(&g_s));
    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0xEF));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0xFF));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_80, suc80_get_mode(&g_s));
    TEST_ASSERT_TRUE(suc80_latch_write(&g_s, 0x00));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));

    /* Všech 256 hodnot: režim = bit 4, změna hlášena právě při přechodu. */
    for (unsigned v = 0; v < 256; v++) {
        en_SUC80_MODE before = suc80_get_mode(&g_s);
        en_SUC80_MODE expect = (v & 0x10) ? SUC80_MODE_80 : SUC80_MODE_40;
        bool changed = suc80_latch_write(&g_s, (uint8_t) v);
        TEST_ASSERT_EQUAL_INT(expect, suc80_get_mode(&g_s));
        TEST_ASSERT_EQUAL(before != expect, changed);
    }
}


/** @brief /RESET vrátí 40 sloupců, volbu atributů nechá. */
void test_reset(void)
{
    TEST_ASSERT_TRUE(suc80_set_attr_mode(&g_s, SUC80_ATTR_PER_PAIR));
    suc80_latch_write(&g_s, 0x50);
    TEST_ASSERT_TRUE(suc80_reset(&g_s));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
    TEST_ASSERT_EQUAL_INT(SUC80_ATTR_PER_PAIR, suc80_get_attr_mode(&g_s));
    TEST_ASSERT_FALSE(suc80_reset(&g_s));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&g_s));
}


/** @brief Neplatná volba atributů stav nemění; NULL argumenty nepadají. */
void test_attr_mode_and_null(void)
{
    TEST_ASSERT_FALSE(suc80_set_attr_mode(&g_s, (en_SUC80_ATTR_MODE) 2));
    TEST_ASSERT_EQUAL_INT(SUC80_ATTR_PER_CHAR, suc80_get_attr_mode(&g_s));
    TEST_ASSERT_TRUE(suc80_set_attr_mode(&g_s, SUC80_ATTR_PER_PAIR));
    TEST_ASSERT_EQUAL_INT(SUC80_ATTR_PER_PAIR, suc80_get_attr_mode(&g_s));

    suc80_init(NULL);
    TEST_ASSERT_FALSE(suc80_reset(NULL));
    TEST_ASSERT_FALSE(suc80_latch_write(NULL, 0x50));
    TEST_ASSERT_FALSE(suc80_set_attr_mode(NULL, SUC80_ATTR_PER_CHAR));
}


/* ===================================================================== */
/* Mapování adres                                                        */
/* ===================================================================== */

/** @brief Pořadí z SUC 2015: 0D000h, 0D401h, 0D001h, 0D402h ...; konec 0D3E7h / 0D7E8h. */
void test_char_offset_first_last(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x000, suc80_char_offset(0, 0));
    TEST_ASSERT_EQUAL_HEX16(0x401, suc80_char_offset(0, 1));
    TEST_ASSERT_EQUAL_HEX16(0x001, suc80_char_offset(0, 2));
    TEST_ASSERT_EQUAL_HEX16(0x402, suc80_char_offset(0, 3));
    TEST_ASSERT_EQUAL_HEX16(0x027, suc80_char_offset(0, 78));
    TEST_ASSERT_EQUAL_HEX16(0x428, suc80_char_offset(0, 79));
    TEST_ASSERT_EQUAL_HEX16(0x028, suc80_char_offset(1, 0));
    TEST_ASSERT_EQUAL_HEX16(0x429, suc80_char_offset(1, 1));
    TEST_ASSERT_EQUAL_HEX16(0x3E7, suc80_char_offset(24, 78));
    TEST_ASSERT_EQUAL_HEX16(0x7E8, suc80_char_offset(24, 79));

    TEST_ASSERT_EQUAL_HEX16(SUC80_OFFSET_INVALID, suc80_char_offset(25, 0));
    TEST_ASSERT_EQUAL_HEX16(SUC80_OFFSET_INVALID, suc80_char_offset(0, 80));
}


/** @brief Blok A 0 - 3E7h a blok B 401h - 7E8h pokryté právě jednou, 400h nikdy. */
void test_char_offset_coverage_no_0x400(void)
{
    static uint8_t hits[SUC80_VRAM_SIZE];
    memset(hits, 0, sizeof(hits));
    for (unsigned r = 0; r < SUC80_TEXT_ROWS; r++) {
        for (unsigned c = 0; c < SUC80_COLUMNS_80; c++) {
            uint16_t o = suc80_char_offset(r, c);
            TEST_ASSERT_TRUE(o < SUC80_VRAM_SIZE);
            TEST_ASSERT_NOT_EQUAL(0x400, o);
            TEST_ASSERT_EQUAL((c & 1) ? 1 : 0, o >= 0x400);
            hits[o]++;
        }
    }
    for (unsigned o = 0; o < 0x800; o++) {
        bool expected = (o <= 0x3E7) || (o >= 0x401 && o <= 0x7E8);
        TEST_ASSERT_EQUAL_UINT8(expected ? 1 : 0, hits[o]);
    }
}


/** @brief Atributy po znacích: 0D800h + k / 0DC01h + k; 0DC00h nikdy. */
void test_attr_offset_per_char(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x800, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0xC01, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(0x801, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(0xBE7, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 24, 78));
    TEST_ASSERT_EQUAL_HEX16(0xFE8, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 24, 79));
    TEST_ASSERT_EQUAL_HEX16(SUC80_OFFSET_INVALID, suc80_attr_offset(SUC80_ATTR_PER_CHAR, 0, 80));
    for (unsigned r = 0; r < SUC80_TEXT_ROWS; r++) {
        for (unsigned c = 0; c < SUC80_COLUMNS_80; c++) {
            uint16_t a = suc80_attr_offset(SUC80_ATTR_PER_CHAR, r, c);
            TEST_ASSERT_EQUAL_HEX16(suc80_char_offset(r, c) + 0x800, a);
            TEST_ASSERT_NOT_EQUAL(0xC00, a);
        }
    }
}


/** @brief Atribut na dvojici: oba sloupce z bloku A (0D800h + k). */
void test_attr_offset_per_pair(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x800, suc80_attr_offset(SUC80_ATTR_PER_PAIR, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0x800, suc80_attr_offset(SUC80_ATTR_PER_PAIR, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(0xBE7, suc80_attr_offset(SUC80_ATTR_PER_PAIR, 24, 78));
    TEST_ASSERT_EQUAL_HEX16(0xBE7, suc80_attr_offset(SUC80_ATTR_PER_PAIR, 24, 79));
    TEST_ASSERT_EQUAL_HEX16(SUC80_OFFSET_INVALID, suc80_attr_offset(SUC80_ATTR_PER_PAIR, 25, 0));
}


/* ===================================================================== */
/* Kreslení v 80 sloupcích                                               */
/* ===================================================================== */

/**
 * @brief První dvojice znaků mikrořádku 0, body spočítané ručně.
 *
 * 0D000h = 41h, atribut 0D800h = 71h (popředí 7 -> 15, pozadí 1 -> 9);
 * vzor sady 1 znaku 41h, mikrořádek 0 = 41h = 0100 0001b, bit 0 vlevo:
 * body 1,0,0,0,0,0,1,0.
 * 0D401h = 42h, atribut 0DC01h = 24h (popředí 2 -> 10, pozadí 4 -> 12);
 * vzor 42h = 0100 0010b: body 0,1,0,0,0,0,1,0.
 */
void test_render80_first_pair_manual(void)
{
    static const uint8_t exp_c0[8] = { 15, 9, 9, 9, 9, 9, 15, 9 };
    static const uint8_t exp_c1[8] = { 12, 10, 12, 12, 12, 12, 10, 12 };
    uint8_t out[SUC80_ROW_PIXELS];

    g_vram[0x000] = 0x41; g_vram[0x800] = 0x71;
    g_vram[0x401] = 0x42; g_vram[0xC01] = 0x24;
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 0, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp_c0, &out[0], 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp_c1, &out[8], 8);

    /* Sloupec 2 = 0D001h: znak 0, atribut 0 -> vzor 00h, vše pozadí 0. */
    for (unsigned x = 16; x < 24; x++) {
        TEST_ASSERT_EQUAL_UINT8(0, out[x]);
    }
}


/**
 * @brief Poslední dvojice (0D3E7h / 0D7E8h) na posledním obrazovém řádku 199.
 *
 * Mikrořádek 7. 0D3E7h = 10h, atribut 0DBE7h = 50h (popředí 5 -> 13, pozadí 0 -> 0);
 * vzor 10h + 29 * 7 = 219 = 0DBh = 1101 1011b: body 1,1,0,1,1,0,1,1.
 * 0D7E8h = 03h, atribut 0DFE8h = 36h (popředí 3 -> 11, pozadí 6 -> 14);
 * vzor 03h + 203 = 206 = 0CEh = 1100 1110b: body 0,1,1,1,0,0,1,1.
 */
void test_render80_last_pair_manual(void)
{
    static const uint8_t exp_c78[8] = { 13, 13, 0, 13, 13, 0, 13, 13 };
    static const uint8_t exp_c79[8] = { 14, 11, 11, 11, 14, 14, 11, 11 };
    uint8_t out[SUC80_ROW_PIXELS];

    g_vram[0x3E7] = 0x10; g_vram[0xBE7] = 0x50;
    g_vram[0x7E8] = 0x03; g_vram[0xFE8] = 0x36;
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 199, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp_c78, &out[78 * 8], 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp_c79, &out[79 * 8], 8);
}


/**
 * @brief Bit 7 atributu vybere sadu 2 (generátor + 800h), i v lichém sloupci.
 *
 * Sloupec 1, mikrořádek 3 (obrazový řádek 3): 0D401h = 41h, atribut 0DC01h =
 * 0F0h (sada 2, popředí 7 -> 15, pozadí 0 -> 0). Vzor sady 2 =
 * 41h + 87 + 80h = 118h, dolní bajt 18h = 0001 1000b: body 0,0,0,1,1,0,0,0.
 * Sada 1 by dala 98h = 1001 1000b (bod 7 = 1), test to rozliší.
 */
void test_render80_set2_bit7(void)
{
    static const uint8_t exp[8] = { 0, 0, 0, 15, 15, 0, 0, 0 };
    uint8_t out[SUC80_ROW_PIXELS];

    g_vram[0x401] = 0x41; g_vram[0xC01] = 0xF0;
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 3, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, &out[8], 8);
}


/**
 * @brief Atribut na dvojici: lichý sloupec bere barvy i sadu z bloku A.
 *
 * Řádek 0: 0D000h = 41h, 0D401h = 42h, 0D800h = 71h, 0DC01h = 24h (ignoruje se).
 * Sloupec 1 tedy kreslí vzor 42h barvami 15 / 9.
 */
void test_render80_attr_per_pair(void)
{
    static const uint8_t exp_c1[8] = { 9, 15, 9, 9, 9, 9, 15, 9 };
    uint8_t out[SUC80_ROW_PIXELS];

    g_vram[0x000] = 0x41; g_vram[0x800] = 0x71;
    g_vram[0x401] = 0x42; g_vram[0xC01] = 0x24;
    suc80_set_attr_mode(&g_s, SUC80_ATTR_PER_PAIR);
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 0, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp_c1, &out[8], 8);

    /* Bit 7 atributu bloku A vybere sadu i pro lichý znak. */
    g_vram[0x800] = 0xF1;  /* sada 2, popředí 7, pozadí 1 */
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 0, out));
    uint8_t mask = cg_pattern(1, 0x42, 0);
    for (unsigned b = 0; b < 8; b++) {
        TEST_ASSERT_EQUAL_UINT8(((mask >> b) & 1) ? 15 : 9, out[8 + b]);
    }
}


/**
 * @brief Celý obraz proti nezávislému výpočtu podle báze (hw/28): sudé c z
 *        0D000h + k, liché z 0D401h + k, atributy 0D800h + k / 0DC01h + k.
 */
void test_render80_full_screen_formula(void)
{
    uint8_t out[SUC80_ROW_PIXELS];
    fill_vram_pseudorandom(0x5EC80u);

    for (unsigned mode = 0; mode < 2; mode++) {
        suc80_set_attr_mode(&g_s, (en_SUC80_ATTR_MODE) mode);
        for (unsigned line = 0; line < SUC80_SCREEN_LINES; line++) {
            TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, line, out));
            for (unsigned c = 0; c < 80; c++) {
                unsigned k = 40 * (line / 8) + c / 2;
                unsigned ch = (c & 1) ? g_vram[0x401 + k] : g_vram[k];
                unsigned at;
                if (mode == SUC80_ATTR_PER_PAIR) {
                    at = g_vram[0x800 + k];
                } else {
                    at = (c & 1) ? g_vram[0xC01 + k] : g_vram[0x800 + k];
                }
                uint8_t mask = cg_pattern((at >> 7) & 1, ch, line % 8);
                for (unsigned b = 0; b < 8; b++) {
                    uint8_t exp = ((mask >> b) & 1) ? c_COLORMAP[(at >> 4) & 7] : c_COLORMAP[at & 7];
                    TEST_ASSERT_EQUAL_UINT8(exp, out[c * 8 + b]);
                }
            }
        }
    }
}


/** @brief Obsah 0D400h a 0DC00h (ani zbytek 0D3E8h - 0D3FFh) neovlivní obraz. */
void test_render80_ignores_0x400(void)
{
    static uint8_t out_a[SUC80_SCREEN_LINES][SUC80_ROW_PIXELS];
    static uint8_t out_b[SUC80_ROW_PIXELS];
    fill_vram_pseudorandom(1234u);

    for (unsigned mode = 0; mode < 2; mode++) {
        suc80_set_attr_mode(&g_s, (en_SUC80_ATTR_MODE) mode);
        g_vram[0x400] = 0x00; g_vram[0xC00] = 0x00;
        for (unsigned line = 0; line < SUC80_SCREEN_LINES; line++) {
            suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, line, out_a[line]);
        }
        g_vram[0x400] = 0xFF; g_vram[0xC00] = 0xF7;
        for (unsigned o = 0x3E8; o < 0x400; o++) {
            g_vram[o] ^= 0xFF;
            g_vram[0x800 + o] ^= 0xFF;
        }
        for (unsigned line = 0; line < SUC80_SCREEN_LINES; line++) {
            suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, line, out_b);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(out_a[line], out_b, SUC80_ROW_PIXELS);
        }
    }
}


/** @brief Neplatný řádek nebo NULL: false a výstup beze změny. */
void test_render_invalid_args(void)
{
    uint8_t out[SUC80_ROW_PIXELS];
    uint8_t ref[SUC80_ROW_PIXELS];
    memset(out, 0xAA, sizeof(out));
    memcpy(ref, out, sizeof(out));

    TEST_ASSERT_FALSE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 200, out));
    TEST_ASSERT_FALSE(suc80_render_line40(g_vram, g_cg, c_COLORMAP, 200, out));
    TEST_ASSERT_FALSE(suc80_render_line(&g_s, g_vram, g_cg, c_COLORMAP, 200, out));
    TEST_ASSERT_FALSE(suc80_render_line80(&g_s, NULL, g_cg, c_COLORMAP, 0, out));
    TEST_ASSERT_FALSE(suc80_render_line80(&g_s, g_vram, NULL, c_COLORMAP, 0, out));
    TEST_ASSERT_FALSE(suc80_render_line80(&g_s, g_vram, g_cg, NULL, 0, out));
    TEST_ASSERT_FALSE(suc80_render_line80(NULL, g_vram, g_cg, c_COLORMAP, 0, out));
    TEST_ASSERT_FALSE(suc80_render_line40(g_vram, g_cg, c_COLORMAP, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, sizeof(out));
}


/* ===================================================================== */
/* Kreslení v 40 sloupcích a přepínání                                   */
/* ===================================================================== */

/** @brief 40 sloupců bajtově shodné s přepisem mz700_framebuffer.c ř. 80-144. */
void test_render40_matches_mz700_reference(void)
{
    uint8_t out[SUC80_ROW_PIXELS];
    uint8_t ref[SUC80_ROW_PIXELS];

    for (uint32_t seed = 1; seed <= 3; seed++) {
        fill_vram_pseudorandom(seed * 7919u);
        for (unsigned line = 0; line < SUC80_SCREEN_LINES; line++) {
            memset(out, 0xAA, sizeof(out));
            memset(ref, 0x55, sizeof(ref));
            TEST_ASSERT_TRUE(suc80_render_line40(g_vram, g_cg, c_COLORMAP, line, out));
            ref_mz700_row40(g_vram, g_cg, line, ref);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, SUC80_ROW_PIXELS);
        }
    }
}


/**
 * @brief Šířka výstupu sedí: 40 x 16 = 80 x 8 = 640 bodů, nic za koncem řádku.
 *
 * Znak 40sloupcového obrazu zdvojený na 16 bodů zabírá stejné místo jako
 * dvojice znaků 80 sloupců. Test hlídá, že žádná cesta nezapíše za 640. bod.
 */
void test_render_width_640(void)
{
    uint8_t buf[SUC80_ROW_PIXELS + 16];
    fill_vram_pseudorandom(42u);

    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 17, buf));
    for (unsigned i = SUC80_ROW_PIXELS; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, buf[i]);
    }
    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_TRUE(suc80_render_line40(g_vram, g_cg, c_COLORMAP, 17, buf));
    for (unsigned i = SUC80_ROW_PIXELS; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, buf[i]);
    }
}


/** @brief suc80_render_line kreslí podle latche; změna platí od příštího volání. */
void test_render_line_dispatch(void)
{
    uint8_t out[SUC80_ROW_PIXELS];
    uint8_t ref[SUC80_ROW_PIXELS];
    fill_vram_pseudorandom(99u);

    TEST_ASSERT_TRUE(suc80_render_line(&g_s, g_vram, g_cg, c_COLORMAP, 50, out));
    ref_mz700_row40(g_vram, g_cg, 50, ref);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, SUC80_ROW_PIXELS);

    suc80_latch_write(&g_s, 0x50);
    TEST_ASSERT_TRUE(suc80_render_line(&g_s, g_vram, g_cg, c_COLORMAP, 50, out));
    TEST_ASSERT_TRUE(suc80_render_line80(&g_s, g_vram, g_cg, c_COLORMAP, 50, ref));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, SUC80_ROW_PIXELS);
}


/* ===================================================================== */
/* Serializace stavu                                                     */
/* ===================================================================== */

/** @brief Formát verze 1 (LE) a round-trip obou polí. */
void test_state_roundtrip(void)
{
    uint8_t buf[8];
    st_SUC80 loaded;

    suc80_latch_write(&g_s, 0x50);
    suc80_set_attr_mode(&g_s, SUC80_ATTR_PER_PAIR);
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(SUC80_STATE_SIZE, suc80_state_save(&g_s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, buf[4]);

    suc80_init(&loaded);
    TEST_ASSERT_EQUAL_INT(SUC80_OK, suc80_state_load(&loaded, buf, SUC80_STATE_SIZE));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_80, suc80_get_mode(&loaded));
    TEST_ASSERT_EQUAL_INT(SUC80_ATTR_PER_PAIR, suc80_get_attr_mode(&loaded));

    /* Výchozí stav také projde (a bajty za koncem formátu se ignorují). */
    suc80_init(&g_s);
    TEST_ASSERT_EQUAL_UINT(SUC80_STATE_SIZE, suc80_state_save(&g_s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(SUC80_OK, suc80_state_load(&loaded, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(&g_s, &loaded, sizeof(st_SUC80));
}


/** @brief Chyby načtení a ukládání; při chybě se stav nemění. */
void test_state_errors(void)
{
    uint8_t buf[SUC80_STATE_SIZE];
    uint8_t small[SUC80_STATE_SIZE - 1];
    st_SUC80 dst;

    TEST_ASSERT_EQUAL_UINT(0, suc80_state_save(&g_s, small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT(0, suc80_state_save(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(0, suc80_state_save(&g_s, NULL, sizeof(buf)));

    suc80_init(&dst);
    suc80_latch_write(&dst, 0x50);
    st_SUC80 before = dst;

    static const uint8_t bad_version[] = { 0x02, 0x00, 0x00, 0x00 };
    static const uint8_t bad_version_hi[] = { 0x01, 0x01, 0x00, 0x00 };
    static const uint8_t bad_mode[] = { 0x01, 0x00, 0x02, 0x00 };
    static const uint8_t bad_attr[] = { 0x01, 0x00, 0x00, 0x02 };
    static const uint8_t ok[] = { 0x01, 0x00, 0x00, 0x00 };

    TEST_ASSERT_EQUAL_INT(SUC80_ERR_INVALID, suc80_state_load(NULL, ok, sizeof(ok)));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_INVALID, suc80_state_load(&dst, NULL, sizeof(ok)));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_SIZE, suc80_state_load(&dst, ok, 1));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_SIZE, suc80_state_load(&dst, ok, SUC80_STATE_SIZE - 1));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_VERSION, suc80_state_load(&dst, bad_version, sizeof(bad_version)));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_VERSION, suc80_state_load(&dst, bad_version_hi, sizeof(bad_version_hi)));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_VALUE, suc80_state_load(&dst, bad_mode, sizeof(bad_mode)));
    TEST_ASSERT_EQUAL_INT(SUC80_ERR_VALUE, suc80_state_load(&dst, bad_attr, sizeof(bad_attr)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &dst, sizeof(st_SUC80));

    TEST_ASSERT_EQUAL_INT(SUC80_OK, suc80_state_load(&dst, ok, sizeof(ok)));
    TEST_ASSERT_EQUAL_INT(SUC80_MODE_40, suc80_get_mode(&dst));
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults);
    RUN_TEST(test_latch_28h_50h);
    RUN_TEST(test_latch_only_bit4);
    RUN_TEST(test_reset);
    RUN_TEST(test_attr_mode_and_null);
    RUN_TEST(test_char_offset_first_last);
    RUN_TEST(test_char_offset_coverage_no_0x400);
    RUN_TEST(test_attr_offset_per_char);
    RUN_TEST(test_attr_offset_per_pair);
    RUN_TEST(test_render80_first_pair_manual);
    RUN_TEST(test_render80_last_pair_manual);
    RUN_TEST(test_render80_set2_bit7);
    RUN_TEST(test_render80_attr_per_pair);
    RUN_TEST(test_render80_full_screen_formula);
    RUN_TEST(test_render80_ignores_0x400);
    RUN_TEST(test_render_invalid_args);
    RUN_TEST(test_render40_matches_mz700_reference);
    RUN_TEST(test_render_width_640);
    RUN_TEST(test_render_line_dispatch);
    RUN_TEST(test_state_roundtrip);
    RUN_TEST(test_state_errors);
    return UNITY_END();
}
