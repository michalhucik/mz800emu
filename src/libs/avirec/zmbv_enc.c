/**
 * @file   zmbv_enc.c
 * @brief  Implementace ZMBV enkodéru (8bpp, bloky 16×16, nulové pohybové vektory).
 *
 * Rozložení snímku:
 * - bajt 0: příznaky (bit 0 = klíčový snímek, bit 1 = delta palety - nepoužíváme),
 * - klíčový snímek pokračuje 6bajtovou hlavičkou: verze 0.1, komprese 1 (zlib),
 *   formát 4 (8bpp), šířka a výška bloku,
 * - následují data komprimovaná zlibem (stream spojitý přes snímky, flush Z_SYNC_FLUSH).
 * Nekomprimovaný obsah klíčového snímku: paleta 256×RGB + pixely.
 * Nekomprimovaný obsah delta snímku: tabulka bloků (2 bajty na blok:
 * (dx << 1) | xor_příznak, dy << 1; zarovnáno na 4 bajty) + XOR data změněných bloků.
 *
 * @par Licence: GPLv3
 */

#include "zmbv_enc.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/** Bit 0 příznakového bajtu snímku: snímek je klíčový. */
#define ZMBV_FLAG_KEYFRAME     0x01
/** Hlavní verze formátu v hlavičce klíčového snímku. */
#define ZMBV_VERSION_MAJOR     0
/** Vedlejší verze formátu v hlavičce klíčového snímku. */
#define ZMBV_VERSION_MINOR     1
/** Identifikátor komprese zlib v hlavičce klíčového snímku. */
#define ZMBV_COMPRESSION_ZLIB  1
/** Identifikátor pixelového formátu 8bpp (paletový) v hlavičce klíčového snímku. */
#define ZMBV_FORMAT_8BPP       4
/** Velikost nekomprimované hlavičky klíčového snímku: příznaky + 6 bajtů. */
#define ZMBV_KEY_HEADER_SIZE   7

/**
 * @brief Stav enkodéru.
 *
 * Invarianty: prev má width*height bajtů; has_prev == false jen před prvním snímkem;
 * zs je inicializovaný po celou dobu života; payload_cap pokrývá největší možný
 * nekomprimovaný snímek a out_cap jeho deflateBound + hlavičku.
 */
struct st_ZMBV_ENC {
    unsigned width, height;     /**< rozměr snímku v pixelech (> 0) */
    unsigned blocks_x, blocks_y; /**< rozměr v blocích, zaokrouhleno nahoru (okrajové bloky jsou částečné) */
    unsigned keyint;            /**< max. delta snímků mezi klíčovými (0 = nikdy) */
    unsigned since_key;         /**< delta snímků od posledního klíčového */
    uint8_t palette[ZMBV_PALETTE_ENTRIES * 3]; /**< paleta RGB (3 B na položku), neuvedené položky černé */
    uint8_t *prev;              /**< referenční (předchozí) snímek */
    bool has_prev;              /**< prev obsahuje platnou referenci (po prvním snímku) */
    uint8_t *payload;           /**< nekomprimovaný obsah snímku */
    size_t payload_cap;         /**< kapacita payload v bajtech */
    uint8_t *out;               /**< výstup: hlavička + komprimovaná data */
    size_t out_cap;             /**< kapacita out v bajtech */
    z_stream zs;                /**< deflate stream, spojitý přes snímky, reset na klíčovém snímku */
};

st_ZMBV_ENC *zmbv_enc_new(unsigned width, unsigned height, const uint32_t *palette_rgb,
                          unsigned palette_count, unsigned keyframe_interval)
{
    if (width == 0 || height == 0 || !palette_rgb || palette_count == 0 ||
        palette_count > ZMBV_PALETTE_ENTRIES)
        return NULL;

    st_ZMBV_ENC *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->width = width;
    e->height = height;
    e->blocks_x = (width + ZMBV_BLOCK_W - 1) / ZMBV_BLOCK_W;
    e->blocks_y = (height + ZMBV_BLOCK_H - 1) / ZMBV_BLOCK_H;
    e->keyint = keyframe_interval;
    for (unsigned i = 0; i < palette_count; i++) {
        e->palette[i * 3 + 0] = (uint8_t)(palette_rgb[i] >> 16);
        e->palette[i * 3 + 1] = (uint8_t)(palette_rgb[i] >> 8);
        e->palette[i * 3 + 2] = (uint8_t)(palette_rgb[i]);
    }

    size_t pixels = (size_t)width * height;
    size_t table = ((size_t)e->blocks_x * e->blocks_y * 2 + 3) & ~(size_t)3;
    size_t key_payload = sizeof(e->palette) + pixels;
    size_t delta_payload = table + pixels;
    e->payload_cap = key_payload > delta_payload ? key_payload : delta_payload;

    e->prev = malloc(pixels);
    e->payload = malloc(e->payload_cap);
    if (!e->prev || !e->payload || deflateInit(&e->zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(e->prev); free(e->payload); free(e);
        return NULL;
    }
    /* rezerva pro Z_SYNC_FLUSH značky */
    e->out_cap = ZMBV_KEY_HEADER_SIZE + deflateBound(&e->zs, (uLong)e->payload_cap) + 64;
    e->out = malloc(e->out_cap);
    if (!e->out) {
        deflateEnd(&e->zs); free(e->prev); free(e->payload); free(e);
        return NULL;
    }
    return e;
}

/**
 * @brief Sestaví nekomprimovaný obsah delta snímku do e->payload.
 *
 * Tabulka bloků (2 B na blok, zarovnaná na 4 B) následovaná XOR daty změněných
 * bloků v pořadí po řádcích; okrajové bloky mají jen bw*bh bajtů.
 *
 * @param e   Enkodér.
 * @param px  Aktuální snímek, width*height bajtů.
 * @return Délka nekomprimovaného obsahu v bajtech (<= e->payload_cap).
 * @pre e->has_prev je true (prev je platná reference); px != NULL.
 * @note Nemění e->prev (ten se aktualizuje až po úspěšném deflate).
 */
static size_t zmbv_build_delta(st_ZMBV_ENC *e, const uint8_t *px)
{
    size_t nblocks = (size_t)e->blocks_x * e->blocks_y;
    size_t table = (nblocks * 2 + 3) & ~(size_t)3;
    memset(e->payload, 0, table);
    size_t pos = table;

    for (unsigned by = 0; by < e->blocks_y; by++) {
        unsigned y0 = by * ZMBV_BLOCK_H;
        unsigned bh = (e->height - y0) < ZMBV_BLOCK_H ? (e->height - y0) : ZMBV_BLOCK_H;
        for (unsigned bx = 0; bx < e->blocks_x; bx++) {
            unsigned x0 = bx * ZMBV_BLOCK_W;
            unsigned bw = (e->width - x0) < ZMBV_BLOCK_W ? (e->width - x0) : ZMBV_BLOCK_W;
            bool changed = false;
            for (unsigned y = 0; y < bh && !changed; y++) {
                size_t off = (size_t)(y0 + y) * e->width + x0;
                changed = memcmp(px + off, e->prev + off, bw) != 0;
            }
            if (!changed) continue;
            size_t bi = (size_t)by * e->blocks_x + bx;
            e->payload[bi * 2] = 0x01;      /* dx = 0, XOR data následují */
            e->payload[bi * 2 + 1] = 0x00;  /* dy = 0 */
            for (unsigned y = 0; y < bh; y++) {
                size_t off = (size_t)(y0 + y) * e->width + x0;
                for (unsigned x = 0; x < bw; x++)
                    e->payload[pos++] = px[off + x] ^ e->prev[off + x];
            }
        }
    }
    return pos;
}

int zmbv_enc_frame(st_ZMBV_ENC *e, const uint8_t *px, bool force_keyframe,
                   const uint8_t **out, size_t *out_size, bool *is_keyframe)
{
    size_t pixels = (size_t)e->width * e->height;
    bool key = force_keyframe || !e->has_prev || (e->keyint > 0 && e->since_key >= e->keyint);
    size_t hdr, plen;

    if (key) {
        e->out[0] = ZMBV_FLAG_KEYFRAME;
        e->out[1] = ZMBV_VERSION_MAJOR;
        e->out[2] = ZMBV_VERSION_MINOR;
        e->out[3] = ZMBV_COMPRESSION_ZLIB;
        e->out[4] = ZMBV_FORMAT_8BPP;
        e->out[5] = ZMBV_BLOCK_W;
        e->out[6] = ZMBV_BLOCK_H;
        hdr = ZMBV_KEY_HEADER_SIZE;
        memcpy(e->payload, e->palette, sizeof(e->palette));
        memcpy(e->payload + sizeof(e->palette), px, pixels);
        plen = sizeof(e->palette) + pixels;
        if (deflateReset(&e->zs) != Z_OK) return -1;
        e->since_key = 0;
    } else {
        e->out[0] = 0x00;
        hdr = 1;
        plen = zmbv_build_delta(e, px);
        e->since_key++;
    }

    e->zs.next_in = e->payload;
    e->zs.avail_in = (uInt)plen;
    e->zs.next_out = e->out + hdr;
    e->zs.avail_out = (uInt)(e->out_cap - hdr);
    int r = deflate(&e->zs, Z_SYNC_FLUSH);
    if (r != Z_OK || e->zs.avail_in != 0 || e->zs.avail_out == 0)
        return -1;

    memcpy(e->prev, px, pixels);
    e->has_prev = true;
    *out = e->out;
    *out_size = e->out_cap - e->zs.avail_out;
    *is_keyframe = key;
    return 0;
}

void zmbv_enc_free(st_ZMBV_ENC *e)
{
    if (!e) return;
    deflateEnd(&e->zs);
    free(e->prev);
    free(e->payload);
    free(e->out);
    free(e);
}
