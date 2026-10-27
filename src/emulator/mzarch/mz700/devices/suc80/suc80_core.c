/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file suc80_core.c
 * @brief Čisté jádro modelu 80sloupcového kitu SUC pro MZ-700 - implementace.
 *
 * Latch režimu, mapování adres VRAM, kreslení obrazového řádku ve 40 i 80
 * sloupcích a serializace stavu. Bez závislostí na emulátoru; popis formátů
 * a rozhodnutí je v suc80_core.h a devdoc/suc80.md.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "suc80_core.h"


/** @brief Počet bajtů jednoho znaku v generátoru (8 mikrořádků). */
#define SUC80_CG_CHAR_BYTES   8

/** @brief Offset druhé sady znaků v generátoru (bit 7 atributu). */
#define SUC80_CG_SET2_OFFSET  0x800


void suc80_init(st_SUC80 *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->mode = SUC80_MODE_40;
    s->attr_mode = SUC80_ATTR_PER_CHAR;
}


bool suc80_reset(st_SUC80 *s)
{
    if (s == NULL) {
        return false;
    }
    bool changed = (s->mode != SUC80_MODE_40);
    s->mode = SUC80_MODE_40;
    return changed;
}


bool suc80_latch_write(st_SUC80 *s, uint8_t value)
{
    if (s == NULL) {
        return false;
    }
    /* Klopný obvod 7474: D = datový bit D4, ostatní bity se nikam nevedou. */
    uint8_t mode = (value & SUC80_LATCH_BIT) ? SUC80_MODE_80 : SUC80_MODE_40;
    bool changed = (s->mode != mode);
    s->mode = mode;
    return changed;
}


en_SUC80_MODE suc80_get_mode(const st_SUC80 *s)
{
    return (s->mode == SUC80_MODE_80) ? SUC80_MODE_80 : SUC80_MODE_40;
}


bool suc80_set_attr_mode(st_SUC80 *s, en_SUC80_ATTR_MODE attr_mode)
{
    if (s == NULL) {
        return false;
    }
    if (attr_mode != SUC80_ATTR_PER_CHAR && attr_mode != SUC80_ATTR_PER_PAIR) {
        return false;
    }
    s->attr_mode = (uint8_t) attr_mode;
    return true;
}


en_SUC80_ATTR_MODE suc80_get_attr_mode(const st_SUC80 *s)
{
    return (s->attr_mode == SUC80_ATTR_PER_PAIR) ? SUC80_ATTR_PER_PAIR : SUC80_ATTR_PER_CHAR;
}


uint16_t suc80_char_offset(unsigned row, unsigned col)
{
    if (row >= SUC80_TEXT_ROWS || col >= SUC80_COLUMNS_80) {
        return SUC80_OFFSET_INVALID;
    }
    unsigned k = SUC80_COLUMNS_40 * row + col / 2;
    return (uint16_t) ((col & 1) ? (SUC80_VRAM_BLOCK_B + k) : k);
}


uint16_t suc80_attr_offset(en_SUC80_ATTR_MODE attr_mode, unsigned row, unsigned col)
{
    if (row >= SUC80_TEXT_ROWS || col >= SUC80_COLUMNS_80) {
        return SUC80_OFFSET_INVALID;
    }
    if (attr_mode == SUC80_ATTR_PER_PAIR) {
        /* Sudý sloupec dvojice určuje atribut obou znaků. */
        col &= ~1u;
    }
    return (uint16_t) (SUC80_VRAM_ATTR_OFFSET + suc80_char_offset(row, col));
}


/**
 * @brief Nakreslí 8 bodů jednoho znaku bez zdvojení.
 * @param dst Výstup, 8 bajtů.
 * @param cg Generátor znaků 4 kB.
 * @param colormap Barevná mapa 8 hodnot.
 * @param code Kód znaku (bajt znakové VRAM).
 * @param attr Atribut (bajt atributové VRAM).
 * @param char_line Mikrořádek znaku 0 - 7.
 */
static inline void suc80_put_char8(uint8_t *dst, const uint8_t *cg, const uint8_t colormap[8],
                                   uint8_t code, uint8_t attr, unsigned char_line)
{
    unsigned cg_addr = ((attr & 0x80) ? SUC80_CG_SET2_OFFSET : 0) + code * SUC80_CG_CHAR_BYTES + char_line;
    unsigned mask = cg[cg_addr];
    uint8_t fgc = colormap[(attr >> 4) & 0x07];
    uint8_t bgc = colormap[attr & 0x07];

    /* Bit 0 vzoru = levý bod (shodně s mz700_framebuffer.c). */
    for (unsigned b = 0; b < 8; b++) {
        dst[b] = (mask & 1) ? fgc : bgc;
        mask >>= 1;
    }
}


bool suc80_render_line80(const st_SUC80 *s, const uint8_t *vram, const uint8_t *cg,
                         const uint8_t colormap[8], unsigned line, uint8_t *dst)
{
    if (s == NULL || vram == NULL || cg == NULL || colormap == NULL || dst == NULL
        || line >= SUC80_SCREEN_LINES) {
        return false;
    }

    unsigned k0 = (line / 8) * SUC80_COLUMNS_40;
    unsigned char_line = line % 8;
    const uint8_t *char_a = &vram[k0];
    const uint8_t *char_b = &vram[SUC80_VRAM_BLOCK_B + k0];
    const uint8_t *attr_a = &vram[SUC80_VRAM_ATTR_OFFSET + k0];
    /* Atribut lichého znaku: blok B (po znacích), nebo totéž co sudý (dvojice). */
    const uint8_t *attr_b = (s->attr_mode == SUC80_ATTR_PER_PAIR)
                                ? attr_a
                                : &vram[SUC80_VRAM_ATTR_OFFSET + SUC80_VRAM_BLOCK_B + k0];

    for (unsigned i = 0; i < SUC80_COLUMNS_40; i++) {
        suc80_put_char8(dst, cg, colormap, char_a[i], attr_a[i], char_line);
        suc80_put_char8(dst + 8, cg, colormap, char_b[i], attr_b[i], char_line);
        dst += 16;
    }
    return true;
}


bool suc80_render_line40(const uint8_t *vram, const uint8_t *cg,
                         const uint8_t colormap[8], unsigned line, uint8_t *dst)
{
    if (vram == NULL || cg == NULL || colormap == NULL || dst == NULL
        || line >= SUC80_SCREEN_LINES) {
        return false;
    }

    unsigned k0 = (line / 8) * SUC80_COLUMNS_40;
    unsigned char_line = line % 8;
    const uint8_t *chars = &vram[k0];
    const uint8_t *attrs = &vram[SUC80_VRAM_ATTR_OFFSET + k0];

    for (unsigned i = 0; i < SUC80_COLUMNS_40; i++) {
        uint8_t attr = attrs[i];
        unsigned cg_addr = ((attr & 0x80) ? SUC80_CG_SET2_OFFSET : 0) + chars[i] * SUC80_CG_CHAR_BYTES + char_line;
        unsigned mask = cg[cg_addr];
        uint8_t fgc = colormap[(attr >> 4) & 0x07];
        uint8_t bgc = colormap[attr & 0x07];

        /* 8 bodů vzoru zdvojených na 16 (pixel doubling jako MZ-700). */
        for (unsigned b = 0; b < 8; b++) {
            uint8_t color = (mask & 1) ? fgc : bgc;
            dst[0] = color;
            dst[1] = color;
            dst += 2;
            mask >>= 1;
        }
    }
    return true;
}


bool suc80_render_line(const st_SUC80 *s, const uint8_t *vram, const uint8_t *cg,
                       const uint8_t colormap[8], unsigned line, uint8_t *dst)
{
    if (s == NULL) {
        return false;
    }
    if (s->mode == SUC80_MODE_80) {
        return suc80_render_line80(s, vram, cg, colormap, line, dst);
    }
    return suc80_render_line40(vram, cg, colormap, line, dst);
}


size_t suc80_state_save(const st_SUC80 *s, uint8_t *buf, size_t size)
{
    if (s == NULL || buf == NULL || size < SUC80_STATE_SIZE) {
        return 0;
    }
    buf[0] = (uint8_t) (SUC80_STATE_VERSION & 0xff);
    buf[1] = (uint8_t) ((SUC80_STATE_VERSION >> 8) & 0xff);
    buf[2] = s->mode;
    buf[3] = s->attr_mode;
    return SUC80_STATE_SIZE;
}


int suc80_state_load(st_SUC80 *s, const uint8_t *buf, size_t size)
{
    if (s == NULL || buf == NULL) {
        return SUC80_ERR_INVALID;
    }
    if (size < 2) {
        return SUC80_ERR_SIZE;
    }
    unsigned version = (unsigned) buf[0] | ((unsigned) buf[1] << 8);
    if (version != SUC80_STATE_VERSION) {
        return SUC80_ERR_VERSION;
    }
    if (size < SUC80_STATE_SIZE) {
        return SUC80_ERR_SIZE;
    }
    uint8_t mode = buf[2];
    uint8_t attr_mode = buf[3];
    if (mode > SUC80_MODE_80 || attr_mode > SUC80_ATTR_PER_PAIR) {
        return SUC80_ERR_VALUE;
    }
    s->mode = mode;
    s->attr_mode = attr_mode;
    return SUC80_OK;
}
