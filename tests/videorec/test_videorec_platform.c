/**
 * @file   test_videorec_platform.c
 * @brief  Standalone testy parametrů platformy video záznamu (všechny čtyři exe).
 *
 * Hodnoty se berou ze sond (videorec_platform_probe.c) přeložených
 * s per-arch makry jednotlivých platforem, takže test hlídá skutečné
 * VIDEO_* / GDGCLK_BASE / HAVE_PSG každé platformy, ne jejich kopii.
 * Očekávané hodnoty odpovídají hlavičkám src/emulator/mzarch/<arch>/gdg/
 * (*_video*.h, *_gdgclk.h) a <arch>_config.h (HAVE_PSG).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "emulator/videorec/videorec_platform.h"

/** Sondy přeložené pro jednotlivé platformy (viz tests/videorec/CMakeLists.txt). */
void vr_probe_mz800(st_VIDEOREC_PLATFORM *out);
void vr_probe_mz1500(st_VIDEOREC_PLATFORM *out);
void vr_probe_mz700pal(st_VIDEOREC_PLATFORM *out);
void vr_probe_mz700ntsc(st_VIDEOREC_PLATFORM *out);

void setUp(void) {}
void tearDown(void) {}

/** Ověří jednu platformu proti očekávaným hodnotám a kontrole pro 44,1 i 48 kHz. */
static void check(const st_VIDEOREC_PLATFORM *p, const char *name, const char *tv, unsigned w, unsigned h,
                  unsigned cx, unsigned cy, uint64_t tpf, uint64_t clk, unsigned fps, unsigned ch, unsigned psg,
                  unsigned spf48, unsigned spf44)
{
    TEST_ASSERT_EQUAL_STRING(name, p->name);
    TEST_ASSERT_EQUAL_STRING(tv, p->tv_system);
    TEST_ASSERT_EQUAL_UINT(w, p->fb_width);
    TEST_ASSERT_EQUAL_UINT(h, p->fb_height);
    TEST_ASSERT_EQUAL_UINT(cx, p->canvas_x);
    TEST_ASSERT_EQUAL_UINT(cy, p->canvas_y);
    TEST_ASSERT_EQUAL_UINT(640, p->canvas_w);
    TEST_ASSERT_EQUAL_UINT(200, p->canvas_h);
    TEST_ASSERT_EQUAL_UINT64(tpf, p->ticks_per_frame);
    TEST_ASSERT_EQUAL_UINT64(clk, p->clk_hz);
    TEST_ASSERT_EQUAL_UINT(fps, p->fps_num);
    TEST_ASSERT_EQUAL_UINT(1, p->fps_den);
    TEST_ASSERT_EQUAL_UINT(ch, p->audio_channels);
    TEST_ASSERT_EQUAL_UINT(psg, p->psg_count);
    char err[128] = "";
    TEST_ASSERT_TRUE_MESSAGE(videorec_platform_check(p, 48000, err, sizeof(err)), err);
    TEST_ASSERT_TRUE_MESSAGE(videorec_platform_check(p, 44100, err, sizeof(err)), err);
    TEST_ASSERT_EQUAL_UINT(spf48, videorec_platform_samples_per_frame(p, 48000));
    TEST_ASSERT_EQUAL_UINT(spf44, videorec_platform_samples_per_frame(p, 44100));
}

/* MZ-800: PAL 50 Hz, framebuffer 928x288 (border 154/134/46/42), 9 kanálů (HAVE_PSG 2 kvůli allow_psg1). */
static void test_mz800(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz800(&p);
    check(&p, "mz800", "pal", 928, 288, 154, 46, 354432, 17721600, 50, 9, 2, 960, 882);
}

/* MZ-1500: NTSC 60 Hz, framebuffer 704x232 (border 32/16), dva PSG = 9 kanálů. */
static void test_mz1500(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz1500(&p);
    check(&p, "mz1500", "ntsc", 704, 232, 32, 16, 238944, 14336640, 60, 9, 2, 800, 735);
}

/* MZ-700 PAL: 50 Hz, řádek 1136 taktů x 312 řádků, bez PSG (jen CTC0). */
static void test_mz700_pal(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz700pal(&p);
    check(&p, "mz700", "pal", 704, 232, 32, 16, 354432, 17721600, 50, 1, 0, 960, 882);
}

/* MZ-700 NTSC: 60 Hz, řádek 912 taktů x 262 řádků, bez PSG. */
static void test_mz700_ntsc(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz700ntsc(&p);
    check(&p, "mz700", "ntsc", 704, 232, 32, 16, 238944, 14336640, 60, 1, 0, 800, 735);
}

/* Frekvence, při které vzorků na snímek nevychází celé číslo, se odmítne. */
static void test_rate_not_divisible(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz1500(&p);
    char err[128] = "";
    TEST_ASSERT_FALSE(videorec_platform_check(&p, 22050, err, sizeof(err))); /* 22050 / 60 = 367,5 */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "22050"), err);
    TEST_ASSERT_TRUE(videorec_platform_check(&p, 0, NULL, 0)); /* 0 = jen platforma */
}

/* Porušené invarianty platformy. */
static void test_invalid_platform(void)
{
    st_VIDEOREC_PLATFORM p;
    vr_probe_mz800(&p);
    st_VIDEOREC_PLATFORM q = p;
    q.clk_hz += 1; /* snímek by netrval celý počet taktů */
    TEST_ASSERT_FALSE(videorec_platform_check(&q, 48000, NULL, 0));
    q = p;
    q.canvas_x = p.fb_width - 100; /* canvas přesahuje framebuffer */
    TEST_ASSERT_FALSE(videorec_platform_check(&q, 48000, NULL, 0));
    q = p;
    q.audio_channels = 5; /* nesouhlasí s psg_count */
    TEST_ASSERT_FALSE(videorec_platform_check(&q, 48000, NULL, 0));
    q = p;
    q.fps_num = 0;
    char err[8];
    TEST_ASSERT_FALSE(videorec_platform_check(&q, 48000, err, sizeof(err))); /* krátký buffer: zkrácený text */
    TEST_ASSERT_EQUAL_size_t(sizeof(err) - 1, strlen(err));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mz800);
    RUN_TEST(test_mz1500);
    RUN_TEST(test_mz700_pal);
    RUN_TEST(test_mz700_ntsc);
    RUN_TEST(test_rate_not_divisible);
    RUN_TEST(test_invalid_platform);
    return UNITY_END();
}
