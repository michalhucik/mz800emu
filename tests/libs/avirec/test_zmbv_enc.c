/**
 * @file   test_zmbv_enc.c
 * @brief  Standalone testy ZMBV enkodéru (struktura bitstreamu, inflate klíčového snímku).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "libs/avirec/zmbv_enc.h"

#define W 40   /* záměrně ne násobek 16 - okrajové bloky */
#define H 20

static const uint32_t PAL[2] = { 0x000000u, 0xFF8040u };

void setUp(void) {}
void tearDown(void) {}

static void fill(uint8_t *px, uint8_t v) { memset(px, v, W * H); }

static void test_keyframe_header(void)
{
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 2, 0);
    TEST_ASSERT_NOT_NULL(e);
    uint8_t px[W * H];
    fill(px, 1);
    const uint8_t *out; size_t n; bool key;
    TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &out, &n, &key));
    TEST_ASSERT_TRUE(key);
    TEST_ASSERT_TRUE(n > 7);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);  /* příznak klíčového snímku */
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);  /* major verze */
    TEST_ASSERT_EQUAL_HEX8(0x01, out[2]);  /* minor verze */
    TEST_ASSERT_EQUAL_HEX8(0x01, out[3]);  /* komprese zlib */
    TEST_ASSERT_EQUAL_HEX8(0x04, out[4]);  /* formát 8bpp */
    TEST_ASSERT_EQUAL_HEX8(16, out[5]);
    TEST_ASSERT_EQUAL_HEX8(16, out[6]);
    zmbv_enc_free(e);
}

static void test_keyframe_inflates_to_palette_and_pixels(void)
{
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 2, 0);
    TEST_ASSERT_NOT_NULL(e);
    uint8_t px[W * H];
    for (int i = 0; i < W * H; i++) px[i] = (uint8_t)(i & 1);
    const uint8_t *out; size_t n; bool key;
    TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &out, &n, &key));

    uint8_t raw[768 + W * H];
    z_stream zs; memset(&zs, 0, sizeof(zs));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&zs));
    zs.next_in = (Bytef *)(out + 7); zs.avail_in = (uInt)(n - 7);
    zs.next_out = raw; zs.avail_out = sizeof(raw);
    int r = inflate(&zs, Z_SYNC_FLUSH);
    TEST_ASSERT_TRUE(r == Z_OK || r == Z_STREAM_END);
    TEST_ASSERT_EQUAL_UINT(0, zs.avail_out);
    inflateEnd(&zs);

    TEST_ASSERT_EQUAL_HEX8(0xFF, raw[3]);  /* paleta[1].R */
    TEST_ASSERT_EQUAL_HEX8(0x80, raw[4]);  /* paleta[1].G */
    TEST_ASSERT_EQUAL_HEX8(0x40, raw[5]);  /* paleta[1].B */
    TEST_ASSERT_EQUAL_HEX8(0x00, raw[6]);  /* paleta[2] černá */
    TEST_ASSERT_EQUAL_MEMORY(px, raw + 768, W * H);
    zmbv_enc_free(e);
}

static void test_unchanged_delta_is_small_and_not_key(void)
{
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 2, 100);
    TEST_ASSERT_NOT_NULL(e);
    uint8_t px[W * H]; fill(px, 1);
    const uint8_t *out; size_t n; bool key;
    zmbv_enc_frame(e, px, false, &out, &n, &key);
    TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &out, &n, &key));
    TEST_ASSERT_FALSE(key);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_TRUE(n < 64);
    zmbv_enc_free(e);
}

static void test_keyframe_interval_and_force(void)
{
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 2, 2);
    TEST_ASSERT_NOT_NULL(e);
    uint8_t px[W * H]; fill(px, 0);
    const uint8_t *out; size_t n; bool key;
    bool keys[5];
    for (int i = 0; i < 5; i++) { zmbv_enc_frame(e, px, false, &out, &n, &key); keys[i] = key; }
    TEST_ASSERT_TRUE(keys[0]); TEST_ASSERT_FALSE(keys[1]); TEST_ASSERT_FALSE(keys[2]);
    TEST_ASSERT_TRUE(keys[3]); TEST_ASSERT_FALSE(keys[4]);
    zmbv_enc_frame(e, px, true, &out, &n, &key);
    TEST_ASSERT_TRUE(key);
    zmbv_enc_free(e);
}

static void test_delta_xor_payload_partial_blocks(void)
{
    /* 40x20 => 3x2 bloků; pravý okraj bw=8, spodní okraj bh=4 */
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 2, 100);
    TEST_ASSERT_NOT_NULL(e);
    uint8_t px[W * H]; fill(px, 0);
    const uint8_t *out; size_t n; bool key;
    TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &out, &n, &key));

    /* jeden spojitý inflate stream: nejdřív klíčový snímek */
    uint8_t raw[768 + W * H];
    z_stream zs; memset(&zs, 0, sizeof(zs));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&zs));
    zs.next_in = (Bytef *)(out + 7); zs.avail_in = (uInt)(n - 7);
    zs.next_out = raw; zs.avail_out = sizeof(raw);
    int r = inflate(&zs, Z_SYNC_FLUSH);
    TEST_ASSERT_TRUE(r == Z_OK || r == Z_STREAM_END);

    px[2 * W + 35] = 0x05;   /* x=35,y=2  => blok (2,0), index 2, 8x16 */
    px[17 * W + 5] = 0x03;   /* x=5,y=17  => blok (0,1), index 3, 16x4 */
    TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &out, &n, &key));
    TEST_ASSERT_FALSE(key);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);

    enum { TABLE = 12, BLK2 = 8 * 16, BLK3 = 16 * 4, TOTAL = TABLE + BLK2 + BLK3 };
    uint8_t d[TOTAL + 64];
    zs.next_in = (Bytef *)(out + 1); zs.avail_in = (uInt)(n - 1);
    zs.next_out = d; zs.avail_out = sizeof(d);
    r = inflate(&zs, Z_SYNC_FLUSH);
    TEST_ASSERT_TRUE(r == Z_OK || r == Z_STREAM_END);
    TEST_ASSERT_EQUAL_UINT(TOTAL, sizeof(d) - zs.avail_out);
    inflateEnd(&zs);

    for (int b = 0; b < 6; b++) {
        uint8_t flag = (b == 2 || b == 3) ? 0x01 : 0x00;
        TEST_ASSERT_EQUAL_HEX8(flag, d[b * 2]);
        TEST_ASSERT_EQUAL_HEX8(0x00, d[b * 2 + 1]);
    }
    /* tabulka je 12 B (6*2, již zarovnáno na 4); XOR data následují */
    const uint8_t *x2 = d + TABLE;
    for (int i = 0; i < BLK2; i++) {
        /* řádek y=2 bloku => offset 2*8 + (35-32) */
        uint8_t exp = (i == 2 * 8 + 3) ? 0x05 : 0x00;
        TEST_ASSERT_EQUAL_HEX8(exp, x2[i]);
    }
    const uint8_t *x3 = d + TABLE + BLK2;
    for (int i = 0; i < BLK3; i++) {
        /* řádek y=17 => řádek 1 bloku, sloupec 5 */
        uint8_t exp = (i == 1 * 16 + 5) ? 0x03 : 0x00;
        TEST_ASSERT_EQUAL_HEX8(exp, x3[i]);
    }
    zmbv_enc_free(e);
}

static void test_invalid_params(void)
{
    TEST_ASSERT_NULL(zmbv_enc_new(0, H, PAL, 2, 0));
    TEST_ASSERT_NULL(zmbv_enc_new(W, H, PAL, 0, 0));
    TEST_ASSERT_NULL(zmbv_enc_new(W, H, PAL, 257, 0));
    TEST_ASSERT_NULL(zmbv_enc_new(W, H, NULL, 2, 0));
    zmbv_enc_free(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_keyframe_header);
    RUN_TEST(test_keyframe_inflates_to_palette_and_pixels);
    RUN_TEST(test_unchanged_delta_is_small_and_not_key);
    RUN_TEST(test_keyframe_interval_and_force);
    RUN_TEST(test_delta_xor_payload_partial_blocks);
    RUN_TEST(test_invalid_params);
    return UNITY_END();
}
