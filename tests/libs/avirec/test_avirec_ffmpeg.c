/**
 * @file   test_avirec_ffmpeg.c
 * @brief  Round-trip test: AVI (ZMBV + PCM) z mzlib_avirec musí ffmpeg dekódovat pixelově a vzorkově přesně.
 *
 * Cesta k ffmpeg přichází jako argv[1] (z CMake find_program); prázdná = testy ignorovány.
 *
 * Všechny soubory (AVI, dekódovaná data) vznikají v unikátním dočasném
 * adresáři (g_dir_make_tmp), který se na konci smaže - nic se nezapisuje do
 * CWD sdíleného s ostatními testy. ffmpeg se spouští přes g_spawn_sync
 * s polem argumentů (žádné parsování příkazové řádky, cesty s '\' jsou
 * v pořádku). Při selhání se vypíše výsledek spawn, exit status v hex
 * (např. 0xC0000142 = STATUS_DLL_INIT_FAILED na Windows) a stderr ffmpeg -
 * diagnostika občasného selhání (finální review I4, příčina neověřena).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs/avirec/avi_writer.h"
#include "libs/avirec/zmbv_enc.h"

#define W 48
#define H 32
#define FRAMES 12
#define SPF 960

static const char *g_ffmpeg = "";
static const uint32_t PAL[4] = { 0x000000u, 0xFF0000u, 0x00FF00u, 0x0000FFu };
/** Dočasný adresář testu (g_dir_make_tmp), NULL = nevytvořen. */
static gchar *g_tmp = NULL;
/** Jména souborů, které testy v g_tmp vytvářejí (pro úklid). */
static const char *const TMP_FILES[] = { "rt.avi", "rt_crash.avi", "rt_video.rgb", "rt_audio.raw" };

/** Cesta souboru @p name v dočasném adresáři (g_free). */
static gchar *tmp_path(const char *name) { return g_build_filename(g_tmp, name, NULL); }

void setUp(void)
{
    if (!g_ffmpeg[0]) TEST_IGNORE_MESSAGE("ffmpeg not found");
    if (!g_tmp) TEST_FAIL_MESSAGE("cannot create a temporary directory");
}
void tearDown(void) {}

/** Deterministický obsah snímku f: posouvající se pruh + šachovnice v rohu. */
static void make_frame(uint8_t *px, int f)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = (uint8_t)(((x + f) / 4) % 4);
    if (f % 3 == 0) px[(H - 1) * W + (W - 1)] = 3;
}

static void make_audio(int16_t *a, int f)
{
    for (int i = 0; i < SPF; i++) a[i * 2] = a[i * 2 + 1] = (int16_t)((f * SPF + i) * 7);
}

static void write_avi(const char *name, int frames, int keyint, bool finalize)
{
    st_AVI_WRITER_PARAMS p = { W, H, 50, 1, 48000, 2 };
    gchar *path = tmp_path(name);
    st_AVI_WRITER *w = avi_writer_open(path, &p);
    g_free(path);
    TEST_ASSERT_NOT_NULL(w);
    st_ZMBV_ENC *e = zmbv_enc_new(W, H, PAL, 4, (unsigned)keyint);
    uint8_t px[W * H]; int16_t a[SPF * 2];
    for (int f = 0; f < frames; f++) {
        make_frame(px, f); make_audio(a, f);
        const uint8_t *o; size_t n; bool k;
        TEST_ASSERT_EQUAL_INT(0, zmbv_enc_frame(e, px, false, &o, &n, &k));
        TEST_ASSERT_EQUAL_INT(AVI_WRITER_OK, avi_writer_write_frame(w, o, n, k, a, SPF));
    }
    zmbv_enc_free(e);
    if (finalize) avi_writer_close(w);
    else { /* simulace pádu: soubor jen uzavřít bez finalizace */
        /* záměrně únik w - test běží v samostatném procesu */
        fflush(NULL);
    }
}

/**
 * Spustí ffmpeg: `ffmpeg -v error -y -i <in> <out_args...> <out>`.
 * @param in       Vstupní soubor (jméno v dočasném adresáři).
 * @param out_args Výstupní volby (pole ukončené NULL).
 * @param out      Výstupní soubor (jméno v dočasném adresáři).
 * @return NULL při úspěchu, jinak popis chyby (g_free): výsledek spawn,
 *         exit status v hex a stderr ffmpeg.
 */
static gchar *run_ffmpeg(const char *in, const char *const *out_args, const char *out)
{
    gchar *in_p = tmp_path(in), *out_p = tmp_path(out);
    GPtrArray *argv = g_ptr_array_new();
    const char *head[] = { g_ffmpeg, "-v", "error", "-y", "-i", in_p };
    for (size_t i = 0; i < G_N_ELEMENTS(head); i++) g_ptr_array_add(argv, (gpointer)head[i]);
    for (size_t i = 0; out_args[i]; i++) g_ptr_array_add(argv, (gpointer)out_args[i]);
    g_ptr_array_add(argv, out_p);
    g_ptr_array_add(argv, NULL);

    gchar *err = NULL;
    gint status = 0;
    GError *gerr = NULL;
    gboolean ok = g_spawn_sync(NULL, (gchar **)argv->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err, &status,
                               &gerr);
    gboolean exited_ok = ok && g_spawn_check_wait_status(status, NULL);
    gchar *res = NULL;
    if (!exited_ok) {
        res = g_strdup_printf("ffmpeg spawn %s (%s), status 0x%08x, stderr: %s", ok ? "ok" : "FAILED",
                              gerr ? gerr->message : "-", (unsigned)status, (err && err[0]) ? err : "(empty)");
    }
    g_clear_error(&gerr);
    g_free(err);
    g_ptr_array_free(argv, TRUE);
    g_free(in_p);
    g_free(out_p);
    return res;
}

/** Načte soubor @p name z dočasného adresáře. */
static gboolean tmp_get_contents(const char *name, gchar **data, gsize *len)
{
    gchar *p = tmp_path(name);
    gboolean ok = g_file_get_contents(p, data, len, NULL);
    g_free(p);
    return ok;
}

static const char *const RAW_VIDEO[] = { "-f", "rawvideo", "-pix_fmt", "rgb24", NULL };
static const char *const RAW_AUDIO[] = { "-f", "s16le", "-acodec", "pcm_s16le", NULL };

static void check_video(const char *avi, int frames)
{
    gchar *e = run_ffmpeg(avi, RAW_VIDEO, "rt_video.rgb");
    TEST_ASSERT_NULL_MESSAGE(e, e);
    gchar *data; gsize len;
    TEST_ASSERT_TRUE(tmp_get_contents("rt_video.rgb", &data, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)frames * W * H * 3, len);
    uint8_t px[W * H];
    for (int f = 0; f < frames; f++) {
        make_frame(px, f);
        for (int i = 0; i < W * H; i++) {
            uint32_t c = PAL[px[i]];
            const uint8_t *d = (const uint8_t *)data + ((size_t)f * W * H + i) * 3;
            if (d[0] != (uint8_t)(c >> 16) || d[1] != (uint8_t)(c >> 8) || d[2] != (uint8_t)c) {
                char m[64]; snprintf(m, sizeof m, "pixel mismatch frame %d px %d", f, i);
                TEST_FAIL_MESSAGE(m);
            }
        }
    }
    g_free(data);
}

static void test_video_roundtrip_with_keyframes(void)
{
    write_avi("rt.avi", FRAMES, 4, true);
    check_video("rt.avi", FRAMES);
}

static void test_audio_roundtrip(void)
{
    write_avi("rt.avi", FRAMES, 4, true);
    gchar *e = run_ffmpeg("rt.avi", RAW_AUDIO, "rt_audio.raw");
    TEST_ASSERT_NULL_MESSAGE(e, e);
    gchar *data; gsize len;
    TEST_ASSERT_TRUE(tmp_get_contents("rt_audio.raw", &data, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)FRAMES * SPF * 4, len);
    int16_t a[SPF * 2];
    for (int f = 0; f < FRAMES; f++) {
        make_audio(a, f);
        TEST_ASSERT_EQUAL_MEMORY(a, data + (size_t)f * SPF * 4, SPF * 4);
    }
    g_free(data);
}

static void test_unfinalized_file_decodes(void)
{
    write_avi("rt_crash.avi", FRAMES, 4, false);
    gchar *e = run_ffmpeg("rt_crash.avi", RAW_VIDEO, "rt_video.rgb");
    TEST_ASSERT_NULL_MESSAGE(e, e);
    gchar *data; gsize len;
    TEST_ASSERT_TRUE(tmp_get_contents("rt_video.rgb", &data, &len));
    TEST_ASSERT_TRUE_MESSAGE(len >= (gsize)(FRAMES - 1) * W * H * 3, "unfinalized AVI lost frames");
    g_free(data);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_ffmpeg = argv[1];
    g_tmp = g_dir_make_tmp("avirec_ffmpeg_XXXXXX", NULL);
    UNITY_BEGIN();
    RUN_TEST(test_video_roundtrip_with_keyframes);
    RUN_TEST(test_audio_roundtrip);
    RUN_TEST(test_unfinalized_file_decodes);
    int r = UNITY_END();
    if (g_tmp) {
        for (size_t i = 0; i < G_N_ELEMENTS(TMP_FILES); i++) {
            gchar *p = tmp_path(TMP_FILES[i]);
            (void)g_remove(p);
            g_free(p);
        }
        (void)g_rmdir(g_tmp);
        g_free(g_tmp);
    }
    return r;
}
