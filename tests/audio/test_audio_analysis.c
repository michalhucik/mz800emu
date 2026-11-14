/**
 * @file   test_audio_analysis.c
 * @brief  Unit testy čistých funkcí rozboru zvuku (audio_analysis.h).
 *
 * Ověřuje:
 * - výšku tónu PSG z děliče a převod na notu MIDI a jméno noty,
 * - souvislé úseky snímků, histogram intervalů a odhad CTC0 (tón / PCM),
 * - převzorkování (shoda 1:1, stejnosměrná úroveň, mono průměr) a hlavičku WAV,
 * - nalezení posunu dvou signálů a rozdíl při posunu,
 * - detektor not (začátek, změna děliče, ztišení, konec úseku).
 *
 * Takty GDG v testech jsou konstanty MZ-800 (17 721 600 Hz, dělič CPU 5),
 * modul sám platformu nezná.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "audio_analysis.h"

/** @brief Takt GDG MZ-800 [Hz]. */
#define GDG_HZ 17721600u
/** @brief Takty GDG na takt CPU MZ-800. */
#define CPU_DIV 5u


void setUp(void) {}
void tearDown(void) {}


/** @brief Dělič -> Hz -> MIDI a jména not. */
void test_audio_analysis_tone_and_notes(void)
{
    /* dělič 0 se chová jako 1024 (změřeno na HW), dělič 1 je stejnosměrný */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 17721600.0 / (32.0 * 1024.0 * 5.0),
                              audio_psg_tone_hz(0, GDG_HZ, CPU_DIV));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, audio_psg_tone_hz(1, GDG_HZ, CPU_DIV));
    double hz = audio_psg_tone_hz(254, GDG_HZ, CPU_DIV);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 17721600.0 / (32.0 * 254.0 * 5.0), hz);

    int pitch = -1, cents = 99;
    TEST_ASSERT_TRUE(audio_hz_to_midi(440.0, &pitch, &cents));
    TEST_ASSERT_EQUAL_INT(69, pitch);
    TEST_ASSERT_EQUAL_INT(0, cents);
    TEST_ASSERT_TRUE(audio_hz_to_midi(hz, &pitch, &cents));   /* 436,05 Hz */
    TEST_ASSERT_EQUAL_INT(69, pitch);
    TEST_ASSERT_EQUAL_INT(-16, cents);
    TEST_ASSERT_FALSE(audio_hz_to_midi(0.0, &pitch, &cents));
    TEST_ASSERT_EQUAL_INT(0, pitch);

    char name[8];
    audio_midi_note_name(69, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("A4", name);
    audio_midi_note_name(70, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("A#4", name);
    audio_midi_note_name(60, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("C4", name);
    audio_midi_note_name(-1, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("noise", name);
    audio_midi_note_name(128, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("?", name);

    TEST_ASSERT_TRUE(audio_psg_is_playing(0, 254, false));
    TEST_ASSERT_FALSE(audio_psg_is_playing(15, 254, false));
    TEST_ASSERT_FALSE(audio_psg_is_playing(0, 1, false));
    TEST_ASSERT_TRUE(audio_psg_is_playing(0, 0, false));
    TEST_ASSERT_TRUE(audio_psg_is_playing(3, 0, true));
}


/** @brief Úseky snímků se zvukem a s tichem. */
void test_audio_analysis_spans(void)
{
    const uint8_t f[] = { 0, 1, 1, 0, 1, 0, 0 };
    st_AUDIO_SPAN s[4];
    size_t total = 0;
    size_t n = audio_spans(f, sizeof(f), 10, true, s, 4, &total);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT(2, total);
    TEST_ASSERT_EQUAL_UINT64(11, s[0].from);
    TEST_ASSERT_EQUAL_UINT64(12, s[0].to);
    TEST_ASSERT_EQUAL_UINT64(14, s[1].from);
    TEST_ASSERT_EQUAL_UINT64(14, s[1].to);
    n = audio_spans(f, sizeof(f), 10, false, s, 1, &total);
    TEST_ASSERT_EQUAL_UINT(1, n);                      /* kapacita 1 */
    TEST_ASSERT_EQUAL_UINT(3, total);                  /* [10], [13], [15,16] */
    TEST_ASSERT_EQUAL_UINT64(10, s[0].from);
}


/** @brief Histogram intervalů a odhad: tón, PCM, ticho. */
void test_audio_analysis_ctc0_guess(void)
{
    uint32_t h[AUDIO_HIST_BUCKETS];
    memset(h, 0, sizeof(h));
    audio_hist_add(h, 0.5);
    audio_hist_add(h, 2.0);
    audio_hist_add(h, 1e12);
    TEST_ASSERT_EQUAL_UINT32(1, h[0]);
    TEST_ASSERT_EQUAL_UINT32(1, h[1]);
    TEST_ASSERT_EQUAL_UINT32(1, h[AUDIO_HIST_BUCKETS - 1]);
    TEST_ASSERT_EQUAL_UINT32(0, audio_hist_bucket_us(0));
    TEST_ASSERT_EQUAL_UINT32(1024, audio_hist_bucket_us(10));

    /* Tón 440 Hz: 880 hran/s, interval 1136 µs. */
    memset(h, 0, sizeof(h));
    for (int i = 0; i < 880; i++) audio_hist_add(h, 1136.0);
    double share = 0.0;
    TEST_ASSERT_EQUAL_STRING("tone", audio_ctc0_guess(h, 880, 1.0, &share));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, share);

    /* 1bitové PCM: násobky periody 62 µs rozprostřené přes několik košů. */
    memset(h, 0, sizeof(h));
    for (int i = 0; i < 8000; i++) audio_hist_add(h, 62.0 * (1 + (i % 9)));
    TEST_ASSERT_EQUAL_STRING("pcm", audio_ctc0_guess(h, 8000, 1.0, &share));
    TEST_ASSERT_TRUE(share < 0.8);

    memset(h, 0, sizeof(h));
    TEST_ASSERT_EQUAL_STRING("silent", audio_ctc0_guess(h, 0, 1.0, NULL));
}


/** @brief Převzorkování a hlavička WAV. */
void test_audio_analysis_resample_wav(void)
{
    int16_t in[2 * 8];
    for (int i = 0; i < 8; i++) {
        in[i * 2] = (int16_t) (1000 + i);
        in[i * 2 + 1] = (int16_t) (-1000 - i);
    }
    int16_t out[2 * 8];
    TEST_ASSERT_EQUAL_UINT(8, audio_resample_s16(in, 8, 44100, 44100, true, out));
    TEST_ASSERT_EQUAL_MEMORY(in, out, sizeof(in));

    /* 4:1 stereo: průměr čtveřic. */
    TEST_ASSERT_EQUAL_UINT(2, audio_resample_frames(8, 44100, 11025));
    TEST_ASSERT_EQUAL_UINT(2, audio_resample_s16(in, 8, 44100, 11025, true, out));
    TEST_ASSERT_EQUAL_INT16(1001, out[0]);   /* (1000+1001+1002+1003)/4 = 1001 (celočíselně) */
    TEST_ASSERT_EQUAL_INT16(-1001, out[1]);
    TEST_ASSERT_EQUAL_INT16(1005, out[2]);
    /* Mono = průměr L a R (zde 0). */
    TEST_ASSERT_EQUAL_UINT(2, audio_resample_s16(in, 8, 44100, 11025, false, out));
    TEST_ASSERT_EQUAL_INT16(0, out[0]);

    uint8_t h[44];
    audio_wav_header(h, 11025, 1, 100);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", h, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", h + 8, 8);
    TEST_ASSERT_EQUAL_MEMORY("data", h + 36, 4);
    TEST_ASSERT_EQUAL_UINT8(1, h[22]);                               /* mono */
    TEST_ASSERT_EQUAL_UINT32(11025, h[24] | (h[25] << 8) | ((uint32_t) h[26] << 16));
    TEST_ASSERT_EQUAL_UINT32(200, h[40] | (h[41] << 8));            /* 100 vzorků x 2 B */
    TEST_ASSERT_EQUAL_UINT32(236, h[4] | (h[5] << 8));              /* 36 + data */
    TEST_ASSERT_EQUAL_UINT8(2, h[32]);                               /* block align */
}


/** @brief Posun druhého signálu: nalezení a rozdíl. */
void test_audio_analysis_compare_lag(void)
{
    enum { N = 6000, LAG = 882 };
    static int16_t a[2 * N], b[2 * (N + LAG)];
    uint32_t x = 12345;
    for (int i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        int16_t v = (int16_t) ((int) ((x >> 16) & 0x7FFF) - 16384);
        a[i * 2] = v;
        a[i * 2 + 1] = v;
    }
    memset(b, 0, sizeof(b));
    memcpy(b + 2 * LAG, a, sizeof(a));

    st_AUDIO_COMPARE c;
    TEST_ASSERT_TRUE(audio_compare(a, N, b, N + LAG, 2000, 0, &c));
    TEST_ASSERT_EQUAL_INT(LAG, c.lag);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, c.diff_rms);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, c.diff_max);
    TEST_ASSERT_TRUE(c.a_rms > 0.1);
    TEST_ASSERT_EQUAL_UINT(N, c.compared);

    /* Při posunu 0 se signály liší. */
    audio_diff(a, N, b, N + LAG, 0, 0, N, &c);
    TEST_ASSERT_TRUE(c.diff_rms > 0.1);
    TEST_ASSERT_FALSE(audio_compare(a, N, b, 0, 10, 0, &c));
}


/**
 * @brief Posun u tónu s nástupem: ustálená část druhého signálu je fázově
 *        posunutá (jako SDL cesta proti referenci), správný posun určuje nástup.
 *
 * Signál a: 2000 vzorků ticho, pak obdélník s periodou 100 vzorků. Signál
 * b: totéž o 882 vzorků později, ustálený tón ale o dalších 25 vzorků ve
 * fázi pozadu. Surový rozdíl je v ustálené části nejmenší při posunu
 * 882 + 25 - 100k, nástup sedí jen při 882.
 */
void test_audio_analysis_compare_lag_tone_onset(void)
{
    enum { N = 30000, LAG = 882, ONSET = 2000, PER = 100, PHASE = 25 };
    static int16_t a[2 * N], b[2 * (N + LAG)];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    for (int i = ONSET; i < N; i++) {
        int16_t v = (((i - ONSET) % PER) < PER / 2) ? 12000 : -12000;
        a[i * 2] = v;
        a[i * 2 + 1] = v;
        int k = i - ONSET - PHASE;
        int16_t w = (k < 0 || (k % PER) < PER / 2) ? 12000 : -12000;
        b[(i + LAG) * 2] = w;
        b[(i + LAG) * 2 + 1] = w;
    }
    st_AUDIO_COMPARE c;
    TEST_ASSERT_TRUE(audio_compare(a, N, b, N + LAG, 2000, 0, &c));
    TEST_ASSERT_INT_WITHIN(8, LAG, c.lag);
    TEST_ASSERT_FALSE(c.lag_ambiguous);

    /* Bez nástupu (jen ustálený tón) posun z dat určit nejde. */
    for (int i = 0; i < N; i++) {
        int16_t v = ((i % PER) < PER / 2) ? 12000 : -12000;
        a[i * 2] = v;
        a[i * 2 + 1] = v;
    }
    for (int i = 0; i < N + LAG; i++) {
        int16_t v = (((i + PHASE) % PER) < PER / 2) ? 12000 : -12000;
        b[i * 2] = v;
        b[i * 2 + 1] = v;
    }
    TEST_ASSERT_TRUE(audio_compare(a, N, b, N + LAG, 2000, 0, &c));
    TEST_ASSERT_TRUE(c.lag_ambiguous);
}


/** @brief Detektor not: začátek, změna děliče, ztišení, konec úseku. */
void test_audio_analysis_note_track(void)
{
    st_AUDIO_NOTE_TRACK t;
    st_AUDIO_NOTE n;
    audio_note_track_init(&t, 254, 15, false, 0);      /* ticho */
    TEST_ASSERT_FALSE(t.playing);
    TEST_ASSERT_FALSE(audio_note_track_update(&t, 254, 2, false, 3, &n));   /* hraje od 3 */
    TEST_ASSERT_TRUE(t.playing);
    TEST_ASSERT_FALSE(audio_note_track_update(&t, 254, 0, false, 4, &n));   /* hlasitěji, stejná nota */
    TEST_ASSERT_TRUE(audio_note_track_update(&t, 300, 0, false, 8, &n));    /* jiný dělič */
    TEST_ASSERT_EQUAL_UINT(254, n.divider);
    TEST_ASSERT_EQUAL_UINT64(3, n.from_frame);
    TEST_ASSERT_EQUAL_UINT64(8, n.to_frame);
    TEST_ASSERT_EQUAL_UINT(15, n.max_level);
    TEST_ASSERT_TRUE(audio_note_track_update(&t, 300, 15, false, 10, &n));  /* ztišení */
    TEST_ASSERT_EQUAL_UINT(300, n.divider);
    TEST_ASSERT_EQUAL_UINT64(8, n.from_frame);
    TEST_ASSERT_EQUAL_UINT64(10, n.to_frame);
    TEST_ASSERT_FALSE(audio_note_track_finish(&t, 20, &n));

    audio_note_track_init(&t, 0, 5, true, 2);           /* šum hraje od začátku */
    TEST_ASSERT_TRUE(audio_note_track_finish(&t, 20, &n));
    TEST_ASSERT_TRUE(n.noise);
    TEST_ASSERT_EQUAL_UINT(0, n.divider);
    TEST_ASSERT_EQUAL_UINT(10, n.max_level);
    TEST_ASSERT_EQUAL_UINT64(2, n.from_frame);
    TEST_ASSERT_EQUAL_UINT64(20, n.to_frame);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_audio_analysis_tone_and_notes);
    RUN_TEST(test_audio_analysis_spans);
    RUN_TEST(test_audio_analysis_ctc0_guess);
    RUN_TEST(test_audio_analysis_resample_wav);
    RUN_TEST(test_audio_analysis_compare_lag);
    RUN_TEST(test_audio_analysis_compare_lag_tone_onset);
    RUN_TEST(test_audio_analysis_note_track);
    return UNITY_END();
}
