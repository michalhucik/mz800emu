/**
 * @file   test_avi_writer.c
 * @brief  Standalone testy AVI zapisovače: struktura RIFF, počty snímků, truncate, plný soubor.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/avirec/avi_writer.h"

#ifdef _WIN32
#include <wchar.h>
#endif

static const char *PATH = "test_avi_writer.avi";
static const st_AVI_WRITER_PARAMS P = { 32, 16, 50, 1, 48000, 2 };

void setUp(void) {}
void tearDown(void) { remove(PATH); }

static uint8_t *slurp(size_t *n)
{
    FILE *f = fopen(PATH, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s);
    TEST_ASSERT_EQUAL_size_t((size_t)s, fread(b, 1, (size_t)s, f));
    fclose(f);
    *n = (size_t)s;
    return b;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static const uint8_t *find(const uint8_t *b, size_t n, const char *fcc)
{
    for (size_t i = 0; i + 4 <= n; i++) if (memcmp(b + i, fcc, 4) == 0) return b + i;
    return NULL;
}

static void write_n(st_AVI_WRITER *w, int n)
{
    uint8_t v[5] = { 1, 2, 3, 4, 5 };  /* lichá délka - test paddingu */
    int16_t a[960 * 2] = { 0 };
    for (int i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_write_frame(w, v, sizeof(v), i == 0, a, 960));
}

static void test_structure_and_counts(void)
{
    st_AVI_WRITER *w = avi_writer_open(PATH, &P);
    TEST_ASSERT_NOT_NULL(w);
    write_n(w, 3);
    TEST_ASSERT_EQUAL_UINT64(3, avi_writer_frame_count(w));
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_close(w));

    size_t n; uint8_t *b = slurp(&n);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", b, 4);
    TEST_ASSERT_EQUAL_UINT32(n - 8, rd32(b + 4));
    TEST_ASSERT_EQUAL_MEMORY("AVI ", b + 8, 4);
    const uint8_t *avih = find(b, n, "avih");
    TEST_ASSERT_EQUAL_UINT32(20000, rd32(avih + 8));        /* µs na snímek */
    TEST_ASSERT_EQUAL_UINT32(3, rd32(avih + 8 + 16));       /* dwTotalFrames */
    TEST_ASSERT_NOT_NULL(find(b, n, "ZMBV"));
    const uint8_t *idx = find(b, n, "idx1");
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_EQUAL_UINT32(3 * 2 * 16, rd32(idx + 4));
    TEST_ASSERT_EQUAL_HEX32(0x10, rd32(idx + 8 + 4));       /* 1. video chunk je klíčový */
    free(b);
}

static void test_truncate_then_continue(void)
{
    st_AVI_WRITER *w = avi_writer_open(PATH, &P);
    TEST_ASSERT_NOT_NULL(w);
    write_n(w, 5);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_truncate_to_frame(w, 2));
    TEST_ASSERT_EQUAL_UINT64(2, avi_writer_frame_count(w));
    write_n(w, 1);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_close(w));

    size_t n; uint8_t *b = slurp(&n);
    TEST_ASSERT_EQUAL_UINT32(n - 8, rd32(b + 4));
    const uint8_t *avih = find(b, n, "avih");
    TEST_ASSERT_EQUAL_UINT32(3, rd32(avih + 8 + 16));
    const uint8_t *idx = find(b, n, "idx1");
    TEST_ASSERT_EQUAL_UINT32(3 * 2 * 16, rd32(idx + 4));
    free(b);
}

static void test_truncate_beyond_end_is_noop(void)
{
    st_AVI_WRITER *w = avi_writer_open(PATH, &P);
    TEST_ASSERT_NOT_NULL(w);
    write_n(w, 2);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_truncate_to_frame(w, 7));
    TEST_ASSERT_EQUAL_UINT64(2, avi_writer_frame_count(w));
    avi_writer_close(w);
}

static void test_full_returns_err_full(void)
{
    st_AVI_WRITER *w = avi_writer_open(PATH, &P);
    TEST_ASSERT_NOT_NULL(w);
    /* chunk těsně nad limit se nesmí zapsat */
    static int16_t a[2] = { 0 };
    uint8_t dummy = 0;
    en_AVI_WRITER_RESULT r = avi_writer_write_frame(w, &dummy, AVI_WRITER_MAX_BYTES, true, a, 1);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_ERR_FULL, r);
    TEST_ASSERT_EQUAL_UINT64(0, avi_writer_frame_count(w));
    avi_writer_close(w);
}

/* max_bytes < AVI_WRITER_MAX_BYTES: plný soubor nastane podle zadaného limitu. */
static void test_custom_max_bytes(void)
{
    st_AVI_WRITER_PARAMS p = P;
    /* hlavičky 324 B; snímek = 8+5+1 (video) + 8+3840 (zvuk) = 3862 B, rezerva indexu 32 B
     * na snímek + 8 B: n snímků se vejde, když 324 + 3894*n + 8 <= 20000, tj. n = 5 */
    p.max_bytes = 20000;
    st_AVI_WRITER *w = avi_writer_open(PATH, &p);
    TEST_ASSERT_NOT_NULL(w);
    uint8_t v[5] = { 1, 2, 3, 4, 5 };
    static int16_t a[960 * 2];
    int written = 0;
    en_AVI_WRITER_RESULT r;
    while ((r = avi_writer_write_frame(w, v, sizeof(v), true, a, 960)) == AVI_WRITER_OK) written++;
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_ERR_FULL, r);
    TEST_ASSERT_EQUAL_INT(5, written);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_close(w));
    size_t n; uint8_t *b = slurp(&n);
    TEST_ASSERT_TRUE(n <= 20000);
    free(b);
}

/* Cesta v UTF-8 s diakritikou: soubor musí vzniknout se správným jménem
 * (na Windows nezávisle na kódové stránce a locale procesu). */
static void test_open_utf8_path(void)
{
    const char *utf8 = "test_avi_\xc4\x8d" "e\xc5\xa1tina.avi"; /* "test_avi_čeština.avi" v UTF-8 */
    st_AVI_WRITER *w = avi_writer_open(utf8, &P);
    TEST_ASSERT_NOT_NULL(w);
    write_n(w, 1);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_close(w));
#ifdef _WIN32
    const wchar_t *wide = L"test_avi_\x010D" L"e\x0161tina.avi";
    FILE *f = _wfopen(wide, L"rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "file with the UTF-8 name was not created");
    fclose(f);
    _wremove(wide);
#else
    FILE *f = fopen(utf8, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);
    remove(utf8);
#endif
}

/* avi_writer_bytes(): velikost souboru roste přesně o chunky snímku, truncate ji vrátí,
 * hodnota odpovídá skutečné velikosti souboru na disku (před zápisem indexu). */
static void test_bytes_tracks_file_size(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, avi_writer_bytes(NULL));
    st_AVI_WRITER *w = avi_writer_open(PATH, &P);
    TEST_ASSERT_NOT_NULL(w);
    uint64_t hdr = avi_writer_bytes(w);
    TEST_ASSERT_TRUE(hdr > 0);
    write_n(w, 1);
    /* video chunk 8 + 5 + padding 1, audio chunk 8 + 960 vzorků * 2 kanály * 2 B */
    uint64_t one = 8 + 5 + 1 + 8 + 960 * 2 * 2;
    TEST_ASSERT_EQUAL_UINT64(hdr + one, avi_writer_bytes(w));
    write_n(w, 2);
    TEST_ASSERT_EQUAL_UINT64(hdr + 3 * one, avi_writer_bytes(w));
    fflush(NULL);
    FILE *f = fopen(PATH, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long disk = ftell(f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT64(hdr + 3 * one, (uint64_t)disk);
    TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_truncate_to_frame(w, 1));
    TEST_ASSERT_EQUAL_UINT64(hdr + one, avi_writer_bytes(w));
    avi_writer_close(w);
}

static void test_open_bad_path(void)
{
    TEST_ASSERT_NULL(avi_writer_open("nonexistent_dir_xyz/x.avi", &P));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_structure_and_counts);
    RUN_TEST(test_truncate_then_continue);
    RUN_TEST(test_truncate_beyond_end_is_noop);
    RUN_TEST(test_full_returns_err_full);
    RUN_TEST(test_custom_max_bytes);
    RUN_TEST(test_open_utf8_path);
    RUN_TEST(test_open_bad_path);
    RUN_TEST(test_bytes_tracks_file_size);
    return UNITY_END();
}
