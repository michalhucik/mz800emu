/**
 * @file   test_videorec_pipe.c
 * @brief  Standalone testy párování obrazu a zvuku (videorec_pipe).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <string.h>

#include "emulator/videorec/videorec_pipe.h"

#define W 4u
#define H 2u
#define TPF (400ull * 10ull) /**< 10 vzorků na snímek při CLK/RATE níže */
#define CLK 19200000ull
#define RATE 48000u
#define ORIGIN 1000ull
#define MAX_GOT 80

/** @brief Záznam jednoho snímku předaného sinku. */
typedef struct {
    uint64_t index;
    uint8_t px0;
    size_t audio_frames;
    int16_t audio[2 * 10]; /**< Kopie audia (stereo, 10 vzorků). */
} st_GOT;

static st_GOT g_got[MAX_GOT];
static unsigned g_ngot;
static float LV[1][VIDEOREC_AUDIO_LEVELS];
static st_VIDEOREC_PIPE P;
static uint8_t PX[W * H];

static void sink(st_VIDEOREC_PIPE_FRAME *f, void *user)
{
    (void)user;
    if (g_ngot < MAX_GOT) {
        st_GOT *g = &g_got[g_ngot];
        g->index = f->index;
        g->px0 = f->pixels[0];
        g->audio_frames = f->audio_frames;
        if (f->audio_frames == 10) memcpy(g->audio, f->audio, sizeof(g->audio));
    }
    g_ngot++;
    g_free(f->pixels);
    g_free(f->audio);
}

void setUp(void)
{
    g_ngot = 0;
    memset(g_got, 0, sizeof(g_got));
    memset(PX, 0x01, sizeof(PX));
    for (int v = 0; v < VIDEOREC_AUDIO_LEVELS; v++) LV[0][v] = v ? 1.0f : 0.0f;
    uint8_t vals[1] = { 0 };
    videorec_pipe_init(&P, W, H, TPF, CLK, RATE, 1, (const float (*)[VIDEOREC_AUDIO_LEVELS])LV, vals, ORIGIN, sink, NULL);
}
void tearDown(void) { videorec_pipe_free(&P); }

static void test_frame_waits_for_horizon(void)
{
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_OK, videorec_pipe_frame(&P, PX, ORIGIN + TPF));
    TEST_ASSERT_EQUAL_UINT(0, g_ngot);
    videorec_pipe_horizon(&P, ORIGIN + TPF - 1);
    TEST_ASSERT_EQUAL_UINT(0, g_ngot);
    videorec_pipe_horizon(&P, ORIGIN + TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(0, g_got[0].index);
    TEST_ASSERT_EQUAL_size_t(10, g_got[0].audio_frames);
}

static void test_pixels_masked(void)
{
    PX[0] = 0xF3;
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_horizon(&P, ORIGIN + TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT8(0x03, g_got[0].px0);
}

static void test_null_frame_consumes_audio_but_not_index(void)
{
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_OK, videorec_pipe_frame(&P, NULL, ORIGIN + TPF));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_OK, videorec_pipe_frame(&P, PX, ORIGIN + 2 * TPF));
    videorec_pipe_horizon(&P, ORIGIN + 2 * TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(0, g_got[0].index);
    TEST_ASSERT_EQUAL_UINT64(1, videorec_pipe_next_index(&P));
}

static void test_discontinuity_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_ERR_DISCONTINUITY, videorec_pipe_frame(&P, PX, ORIGIN + 2 * TPF));
    videorec_pipe_horizon(&P, ORIGIN + 10 * TPF);
    TEST_ASSERT_EQUAL_UINT(0, g_ngot);
}

static void test_full_queue(void)
{
    for (unsigned i = 1; i <= VIDEOREC_PIPE_MAX_PENDING; i++)
        TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_OK, videorec_pipe_frame(&P, PX, ORIGIN + i * TPF));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_ERR_FULL,
                          videorec_pipe_frame(&P, PX, ORIGIN + (VIDEOREC_PIPE_MAX_PENDING + 1) * TPF));
}

static void test_flush_renders_pending(void)
{
    for (unsigned i = 1; i <= 3; i++) videorec_pipe_frame(&P, PX, ORIGIN + i * TPF);
    videorec_pipe_flush(&P);
    TEST_ASSERT_EQUAL_UINT(3, g_ngot);
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT64(i, g_got[i].index);
        TEST_ASSERT_EQUAL_size_t(10, g_got[i].audio_frames);
    }
}

static void test_rebase_drops_pending_and_continues(void)
{
    uint8_t vals[1] = { 0 };
    videorec_pipe_set_next_index(&P, 7);
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_rebase(&P, 5000, vals);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_PIPE_OK, videorec_pipe_frame(&P, PX, 5000 + TPF));
    videorec_pipe_horizon(&P, 5000 + TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(7, g_got[0].index);
    TEST_ASSERT_EQUAL_UINT64(8, videorec_pipe_next_index(&P));
}

static void test_rebase_without_set_index_starts_from_current(void)
{
    uint8_t vals[1] = { 0 };
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_rebase(&P, 5000, vals);
    videorec_pipe_frame(&P, PX, 5000 + TPF);
    videorec_pipe_horizon(&P, 5000 + TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(0, g_got[0].index);
}

static void test_audio_lands_in_right_frame(void)
{
    videorec_pipe_audio_event(&P, 0, 15, ORIGIN + TPF + TPF / 2);
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_frame(&P, PX, ORIGIN + 2 * TPF);
    videorec_pipe_horizon(&P, ORIGIN + 2 * TPF);
    TEST_ASSERT_EQUAL_UINT(2, g_ngot);
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL_INT16(0, g_got[0].audio[i]);
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_INT16(0, g_got[1].audio[i * 2]);
    for (int i = 5; i < 10; i++) TEST_ASSERT_TRUE(g_got[1].audio[i * 2] > 0);
}

static void test_flush_until_index_keeps_frames_before_snapshot(void)
{
    /* Retake: snímky 0..4 čekají na zvuk, snapshot byl uložen na snímku 3.
     * Snímky 0..2 se musí vydat (platná minulost), 3..4 zahodit při rebase. */
    for (unsigned i = 1; i <= 5; i++) videorec_pipe_frame(&P, PX, ORIGIN + i * TPF);
    videorec_pipe_flush_until_index(&P, 3);
    TEST_ASSERT_EQUAL_UINT(3, g_ngot);
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT64(i, g_got[i].index);
        TEST_ASSERT_EQUAL_size_t(10, g_got[i].audio_frames);
    }
    TEST_ASSERT_EQUAL_UINT64(3, videorec_pipe_next_index(&P));
    TEST_ASSERT_EQUAL_UINT(2, P.count);
    uint8_t vals[1] = { 0 };
    videorec_pipe_rebase(&P, 9000, vals);
    videorec_pipe_frame(&P, PX, 9000 + TPF);
    videorec_pipe_horizon(&P, 9000 + TPF);
    TEST_ASSERT_EQUAL_UINT(4, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(3, g_got[3].index);
}

static void test_flush_until_index_skips_pause_frames(void)
{
    videorec_pipe_frame(&P, NULL, ORIGIN + TPF);
    videorec_pipe_frame(&P, PX, ORIGIN + 2 * TPF);
    videorec_pipe_frame(&P, PX, ORIGIN + 3 * TPF);
    videorec_pipe_flush_until_index(&P, 1);
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
    TEST_ASSERT_EQUAL_UINT64(1, videorec_pipe_next_index(&P));
    TEST_ASSERT_EQUAL_UINT(1, P.count);
    videorec_pipe_flush_until_index(&P, 0); /* už dosaženo - no-op */
    TEST_ASSERT_EQUAL_UINT(1, g_ngot);
}

/** @brief Zachycené stavy (callback zachycení). */
static uint64_t g_cap_tag[8];
static st_VIDEOREC_AUDIO_STATE g_cap_state[8];
static unsigned g_ncap;

static void capture(uint64_t tag, const st_VIDEOREC_AUDIO_STATE *st, void *user)
{
    (void)user;
    if (g_ncap < 8) { g_cap_tag[g_ncap] = tag; g_cap_state[g_ncap] = *st; }
    g_ncap++;
}

/* Bez čekajících snímků se stav zachytí hned (renderer stojí na konci posledního snímku). */
static void test_capture_immediate_when_nothing_pending(void)
{
    g_ncap = 0;
    videorec_pipe_set_capture_cb(&P, capture, NULL);
    videorec_pipe_audio_event(&P, 0, 15, ORIGIN + 100);
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_horizon(&P, ORIGIN + TPF);
    videorec_pipe_request_capture(&P, 7);
    TEST_ASSERT_EQUAL_UINT(1, g_ncap);
    TEST_ASSERT_EQUAL_UINT64(7, g_cap_tag[0]);
    TEST_ASSERT_EQUAL_UINT8(15, g_cap_state[0].value[0]);
    TEST_ASSERT_TRUE(g_cap_state[0].ch[0].ag > 0.0f); /* filtry už reagovaly */
}

/* Čeká-li snímek na zvuk, stav se zachytí až po jeho vyrenderování - ne dřív, ne s pozdějšími událostmi. */
static void test_capture_deferred_until_frame_rendered(void)
{
    g_ncap = 0;
    videorec_pipe_set_capture_cb(&P, capture, NULL);
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_request_capture(&P, 3);
    TEST_ASSERT_EQUAL_UINT(0, g_ncap);
    videorec_pipe_audio_event(&P, 0, 15, ORIGIN + TPF + 10); /* událost až v dalším snímku */
    videorec_pipe_frame(&P, PX, ORIGIN + 2 * TPF);
    videorec_pipe_horizon(&P, ORIGIN + 2 * TPF);
    TEST_ASSERT_EQUAL_UINT(1, g_ncap);
    TEST_ASSERT_EQUAL_UINT64(3, g_cap_tag[0]);
    TEST_ASSERT_EQUAL_UINT8(0, g_cap_state[0].value[0]); /* stav na konci 1. snímku */
    TEST_ASSERT_TRUE(g_cap_state[0].ch[0].ag == 0.0f);
}

/* Rebase zahodí nevyřízené požadavky (jejich snímky se nevyrenderují). */
static void test_capture_dropped_by_rebase(void)
{
    g_ncap = 0;
    videorec_pipe_set_capture_cb(&P, capture, NULL);
    videorec_pipe_frame(&P, PX, ORIGIN + TPF);
    videorec_pipe_request_capture(&P, 5);
    videorec_pipe_rebase(&P, ORIGIN + 10 * TPF, NULL);
    videorec_pipe_frame(&P, PX, ORIGIN + 11 * TPF);
    videorec_pipe_horizon(&P, ORIGIN + 11 * TPF);
    TEST_ASSERT_EQUAL_UINT(0, g_ncap);
    TEST_ASSERT_EQUAL_UINT(0, P.ncapture);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_waits_for_horizon);
    RUN_TEST(test_pixels_masked);
    RUN_TEST(test_null_frame_consumes_audio_but_not_index);
    RUN_TEST(test_discontinuity_rejected);
    RUN_TEST(test_full_queue);
    RUN_TEST(test_flush_renders_pending);
    RUN_TEST(test_rebase_drops_pending_and_continues);
    RUN_TEST(test_rebase_without_set_index_starts_from_current);
    RUN_TEST(test_audio_lands_in_right_frame);
    RUN_TEST(test_flush_until_index_keeps_frames_before_snapshot);
    RUN_TEST(test_flush_until_index_skips_pause_frames);
    RUN_TEST(test_capture_immediate_when_nothing_pending);
    RUN_TEST(test_capture_deferred_until_frame_rendered);
    RUN_TEST(test_capture_dropped_by_rebase);
    return UNITY_END();
}
