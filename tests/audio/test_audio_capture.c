/**
 * @file   test_audio_capture.c
 * @brief  Unit testy zachycení zvuku (audio_capture.h) nad emu core.
 *
 * Háčky se volají přímo, jako je volá zvuková cesta: audio_capture_on_frame()
 * na konci zvukového snímku, audio_capture_on_change() při změně kanálu,
 * audio_capture_on_psg_write() po zápisu do PSG a audio_capture_sdl_output()
 * s blokem SDL cesty.
 *
 * Ověřuje:
 * - naplánovaný start začne až na konci snímku, rozjezd SDL cesty se sdl.wav,
 * - obsah events.csv a psg_writes.csv (relativní takty, snímek, dekódování),
 * - hlavičky WAV a počty vzorků, bloky SDL během rozjezdu se zahodí,
 * - konec po `frames`, při skoku času zpět a stopem; capture.json
 *   (absolutní začátek a konec v taktech GDG, hodiny platformy),
 * - chyby startu (už běží, žádný výstup, nelze vytvořit složku),
 * - paměť zachycení: ringy levels a psg_writes odpovídají CSV, přetečení
 *   ringu (first_seq), segment a čitelnost po stopu, zachycení jen do
 *   paměti bez složky, souhrn snímku (hrany, čas a energie úrovně, RMS
 *   reference) a PCM ringy bajtově shodné s reference.wav a sdl.wav.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "audio_capture.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/psg/psg.h"

/** @brief Dočasná složka testu. */
static char *s_dir;
/** @brief Délka snímku v taktech GDG. */
static const uint64_t T_FRAME = (uint64_t) GDGCLK_BASE / VIDEO_SCREENS_PER_SEC;
/** @brief Začátek zachycení v testech (větší než zahřátí rendereru 100 ms). */
static const uint64_t T0 = (uint64_t) GDGCLK_BASE;


/** @brief Smaže soubory ve složce a složku. */
static void rm_dir(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *n;
    while ((n = g_dir_read_name(d)) != NULL) {
        char *p = g_build_filename(dir, n, NULL);
        g_remove(p);
        g_free(p);
    }
    g_dir_close(d);
    g_rmdir(dir);
}


/** @brief Obsah souboru ve složce zachycení (volající uvolní g_free). */
static char *read_file(const char *name, gsize *len)
{
    char *p = g_build_filename(s_dir, name, NULL);
    char *text = NULL;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(p, &text, len, NULL), name);
    g_free(p);
    return text;
}


/** @brief Počet řádků textu (bez hlavičky CSV). */
static unsigned data_lines(const char *text)
{
    unsigned n = 0;
    for (const char *c = strchr(text, '\n'); c && c[1]; c = strchr(c + 1, '\n')) n++;
    return n;
}


/** @brief Parametry s danými výstupy do s_dir. */
static st_AUDIO_CAPTURE_PARAMS params(bool sdl, uint64_t frames)
{
    st_AUDIO_CAPTURE_PARAMS p;
    audio_capture_params_init(&p);
    g_strlcpy(p.dir, s_dir, sizeof(p.dir));
    p.sdl_wav = sdl;
    p.frames = frames;
    return p;
}


void setUp(void)
{
    s_dir = g_dir_make_tmp("mztest-audiocap-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_dir);
}


void tearDown(void)
{
    audio_capture_stop();
    rm_dir(s_dir);
    g_free(s_dir);
}


/** @brief Start bez sdl.wav začne na konci nejbližšího snímku; CSV s relativními časy. */
void test_audio_capture_start_events_psg(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    char err[128];
    TEST_ASSERT_TRUE(audio_capture_start(&p, err, sizeof(err)));
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.pending);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_TRUE(audio_capture_wanted());
    TEST_ASSERT_FALSE(audio_capture_wants_sdl());

    /* Změna před začátkem se nezapíše. */
    audio_capture_on_change(0, 15, T0 - 10);
    audio_capture_on_frame(T0);
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT, st.events); /* výchozí úrovně */

    audio_capture_on_change(1, 13, T0 + 100);
    /* Zápis hlasitosti kanálu 1 PSG0: 1011 0010 = latch, kanál 1, attn, 2. */
    g_psg_module.psg[0].latch_cs = 1;
    g_psg_module.psg[0].latch_attn = 1;
    g_psg_module.psg[0].channel[1].attn = 2;
    audio_capture_on_psg_write(T0 + 200, PSG_CH_LEFT, 0xB2);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_change(0, 0, T0 + T_FRAME + 5);
    TEST_ASSERT_TRUE(audio_capture_stop());

    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_STRING("Stopped", st.stop_reason);
    TEST_ASSERT_EQUAL_UINT64(1, st.frames);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 2, st.events);
    TEST_ASSERT_EQUAL_UINT64(1, st.psg_writes);

    char *ev = read_file("events.csv", NULL);
    TEST_ASSERT_EQUAL_UINT(AUDIO_SRC_CHANNELS_COUNT + 2, data_lines(ev));
    TEST_ASSERT_NOT_NULL(strstr(ev, "ticks,time_us,frame,channel,level\n0,0.000,0,ctc0,0\n"));
    TEST_ASSERT_NOT_NULL(strstr(ev, "\n100,"));
    TEST_ASSERT_NOT_NULL(strstr(ev, ",0,psg0_0,13\n"));
    char line[64];
    g_snprintf(line, sizeof(line), "\n%llu,", (unsigned long long) (T_FRAME + 5));
    TEST_ASSERT_NOT_NULL(strstr(ev, line));
    TEST_ASSERT_NOT_NULL(strstr(ev, ",1,ctc0,0\n"));
    g_free(ev);

    char *ps = read_file("psg_writes.csv", NULL);
    TEST_ASSERT_NOT_NULL(strstr(ps, "ticks,time_us,frame,psg,byte,channel,reg,value\n200,"));
    TEST_ASSERT_NOT_NULL(strstr(ps, ",0,1,B2,1,volume,2\n"));
    g_free(ps);

    char *js = read_file("capture.json", NULL);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"stop_reason\": \"Stopped\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"frames\": 1,"));
    /* Absolutní časy (takty GDG): začátek = čas 0 v CSV, konec = horizont
     * posledního dokončeného snímku (konec reference.wav), poslední řádek
     * CSV po ručním stopu leží za ním. */
    TEST_ASSERT_NOT_NULL(strstr(js, "\"began\": true,"));
    g_snprintf(line, sizeof(line), "\"origin_ticks\": %llu,", (unsigned long long) T0);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, line), line);
    g_snprintf(line, sizeof(line), "\"end_ticks\": %llu,", (unsigned long long) (T0 + T_FRAME));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, line), line);
    g_snprintf(line, sizeof(line), "\"last_event_ticks\": %llu,", (unsigned long long) (T0 + T_FRAME + 5));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, line), line);
    /* Hodiny platformy se stejnými názvy jako emulator://platform/info. */
    g_snprintf(line, sizeof(line), "\"gdg_base_hz\": %llu,", (unsigned long long) GDGCLK_BASE);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, line), line);
    g_snprintf(line, sizeof(line), "\"gdg_real_base_hz\": %llu,", (unsigned long long) GDGCLK_REAL_BASE);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, line), line);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"cpu_divider\": "));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"ctc0_divider\": "));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"psg_divider\": "));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"screen_total_ticks_per_frame\": "));
    g_free(js);
}


/** @brief Stop před začátkem (start naplánovaný na konec snímku): bez časů. */
void test_audio_capture_stop_while_pending(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    TEST_ASSERT_TRUE(audio_capture_stop());

    char *js = read_file("capture.json", NULL);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"began\": false,"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"origin_ticks\": null,"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"end_ticks\": null,"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"last_event_ticks\": null,"));
    g_free(js);
}


/** @brief reference.wav: hlavička PCM 44100 Hz stereo, vzorky za celý snímek. */
void test_audio_capture_reference_wav(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 2);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    audio_capture_on_change(2, 15, T0 + 1000);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_frame(T0 + 2 * T_FRAME);  /* frames = 2 => konec */
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_STRING("Frame count reached", st.stop_reason);
    uint64_t per_frame = AUDIO_CAPTURE_WAV_RATE / VIDEO_SCREENS_PER_SEC;
    /* Renderer zapíše úplné vzorky do horizontu (případně o 1 méně kvůli zaokrouhlení hranice). */
    TEST_ASSERT_UINT64_WITHIN(1, 2 * per_frame, st.reference_samples);

    gsize len = 0;
    char *wav = read_file("reference.wav", &len);
    TEST_ASSERT_EQUAL_UINT64(44 + st.reference_samples * 4, len);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", wav, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", wav + 8, 8);
    TEST_ASSERT_EQUAL_UINT8(2, (uint8_t) wav[22]);                    /* stereo */
    uint32_t rate = (uint8_t) wav[24] | ((uint8_t) wav[25] << 8) | ((uint32_t) (uint8_t) wav[26] << 16);
    TEST_ASSERT_EQUAL_UINT32(AUDIO_CAPTURE_WAV_RATE, rate);
    uint32_t data = (uint8_t) wav[40] | ((uint8_t) wav[41] << 8) | ((uint32_t) (uint8_t) wav[42] << 16);
    TEST_ASSERT_EQUAL_UINT32(st.reference_samples * 4, data);
    /* Kanál psg0_1 na úrovni 15 => nenulový signál ve druhém snímku. */
    bool nonzero = false;
    for (gsize i = 44 + per_frame * 4; i < len; i++) if (wav[i]) { nonzero = true; break; }
    TEST_ASSERT_TRUE(nonzero);
    g_free(wav);
}


/** @brief sdl.wav: rozjezd 6 snímků, bloky během něj se zahodí, pak se zapisují. */
void test_audio_capture_sdl_warmup(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(true, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    TEST_ASSERT_TRUE(audio_capture_wants_sdl());
    float block[2 * 10];
    for (int i = 0; i < 20; i++) block[i] = 0.5f;

    st_AUDIO_CAPTURE_STATUS st;
    for (int f = 0; f < 6; f++) {
        audio_capture_on_frame(T0 + (uint64_t) f * T_FRAME);
        audio_capture_sdl_output(block, 10);
        audio_capture_get_status(&st);
        TEST_ASSERT_TRUE(st.pending);
    }
    TEST_ASSERT_EQUAL_UINT64(0, st.sdl_blocks);
    audio_capture_on_frame(T0 + 6 * T_FRAME);
    audio_capture_sdl_output(block, 10);
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT64(1, st.sdl_blocks);
    TEST_ASSERT_EQUAL_UINT64(10, st.sdl_samples);
    TEST_ASSERT_TRUE(audio_capture_stop());
    TEST_ASSERT_FALSE(audio_capture_wants_sdl());
    audio_capture_sdl_output(block, 10);   /* po stop se nic nezapíše */

    gsize len = 0;
    char *wav = read_file("sdl.wav", &len);
    TEST_ASSERT_EQUAL_UINT64(44 + 10 * 4, len);
    int16_t s0 = (int16_t) ((uint8_t) wav[44] | ((uint8_t) wav[45] << 8));
    TEST_ASSERT_INT_WITHIN(1, 16383, s0);
    g_free(wav);
}


/** @brief Skok času zpět (reset, snapshot) zachycení ukončí. */
void test_audio_capture_timeline_restart(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_frame(1000);
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_FALSE(audio_capture_wanted());
    TEST_ASSERT_NOT_NULL(strstr(st.stop_reason, "timeline restarted"));
    TEST_ASSERT_FALSE(audio_capture_stop());
}


/** @brief Chyby startu: už běží, žádný výstup, nelze vytvořit složku. */
void test_audio_capture_start_errors(void)
{
    char err[AUDIO_CAPTURE_TEXT_MAX];
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, err, sizeof(err)));
    TEST_ASSERT_FALSE(audio_capture_start(&p, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "already running"));
    TEST_ASSERT_TRUE(audio_capture_stop());

    st_AUDIO_CAPTURE_PARAMS none = params(false, 0);
    none.events = none.psg_writes = none.reference_wav = false;
    TEST_ASSERT_FALSE(audio_capture_start(&none, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "No output"));

    /* Složka uvnitř souboru nejde vytvořit. */
    char *file = g_build_filename(s_dir, "afile", NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(file, "x", 1, NULL));
    st_AUDIO_CAPTURE_PARAMS bad = params(false, 0);
    char *sub = g_build_filename(file, "sub", NULL);
    g_strlcpy(bad.dir, sub, sizeof(bad.dir));
    TEST_ASSERT_FALSE(audio_capture_start(&bad, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "Cannot create directory"));
    TEST_ASSERT_FALSE(audio_capture_wanted());
    g_free(sub);
    g_free(file);
}


/** @brief Celý držený obsah proudu (kopie od first_seq). */
static st_AUDIO_CAPTURE_STREAM_COPY copy_all(en_AUDIO_CAPTURE_STREAM st)
{
    st_AUDIO_CAPTURE_STREAM_COPY c;
    TEST_ASSERT_TRUE(audio_capture_stream_copy(st, 0, UINT64_MAX, 64u * 1024u * 1024u, &c));
    return c;
}


/** @brief Ringy levels a psg_writes nesou totéž co events.csv a psg_writes.csv. */
void test_audio_capture_memory_matches_csv(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    audio_capture_on_change(1, 13, T0 + 100);
    g_psg_module.psg[0].latch_cs = 1;
    g_psg_module.psg[0].latch_attn = 1;
    g_psg_module.psg[0].channel[1].attn = 2;
    audio_capture_on_psg_write(T0 + 200, PSG_CH_LEFT, 0xB2);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_change(0, 15, T0 + T_FRAME + 5);
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.memory);
    TEST_ASSERT_EQUAL_UINT(1, st.streams[AUDIO_CAPTURE_STREAM_LEVELS].present);
    TEST_ASSERT_FALSE(st.streams[AUDIO_CAPTURE_STREAM_PCM_SDL].present);
    TEST_ASSERT_EQUAL_UINT64(st.events, st.streams[AUDIO_CAPTURE_STREAM_LEVELS].next_seq);
    TEST_ASSERT_TRUE(audio_capture_stop());

    st_AUDIO_CAPTURE_STREAM_COPY lv = copy_all(AUDIO_CAPTURE_STREAM_LEVELS);
    TEST_ASSERT_FALSE(lv.live);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 2, lv.data_count);
    TEST_ASSERT_EQUAL_UINT64(T0, lv.origin_ticks);
    const st_AUDIO_CAPTURE_LEVEL_REC *l = (const st_AUDIO_CAPTURE_LEVEL_REC *) lv.data;
    /* Řádky "100,...,0,psg0_0,13" a "T_FRAME+5,...,1,ctc0,15" (absolutní čas v ringu). */
    const st_AUDIO_CAPTURE_LEVEL_REC *a = &l[AUDIO_SRC_CHANNELS_COUNT];
    TEST_ASSERT_EQUAL_UINT64(T0 + 100, a->t);
    TEST_ASSERT_EQUAL_UINT32(0, a->frame);
    TEST_ASSERT_EQUAL_UINT8(1, a->src);
    TEST_ASSERT_EQUAL_UINT8(13, a->level);
    TEST_ASSERT_EQUAL_UINT64(T0 + T_FRAME + 5, a[1].t);
    TEST_ASSERT_EQUAL_UINT32(1, a[1].frame);
    TEST_ASSERT_EQUAL_UINT8(0, a[1].src);
    TEST_ASSERT_EQUAL_UINT8(15, a[1].level);
    audio_capture_stream_copy_clear(&lv);

    st_AUDIO_CAPTURE_STREAM_COPY ps = copy_all(AUDIO_CAPTURE_STREAM_PSG_WRITES);
    TEST_ASSERT_EQUAL_UINT64(1, ps.data_count);
    const st_AUDIO_CAPTURE_PSG_REC *w = (const st_AUDIO_CAPTURE_PSG_REC *) ps.data;
    /* psg_writes.csv: "200,...,0,1,B2,1,volume,2" */
    TEST_ASSERT_EQUAL_UINT64(T0 + 200, w->t);
    TEST_ASSERT_EQUAL_UINT8(PSG_CH_LEFT, w->psg);
    TEST_ASSERT_EQUAL_UINT8(0xB2, w->byte);
    TEST_ASSERT_EQUAL_UINT8(1, w->channel);
    TEST_ASSERT_EQUAL_UINT8(AUDIO_CAPTURE_PSG_REG_VOLUME, w->reg);
    TEST_ASSERT_EQUAL_UINT16(2, w->value);
    audio_capture_stream_copy_clear(&ps);
    char *csv = read_file("psg_writes.csv", NULL);
    TEST_ASSERT_NOT_NULL(strstr(csv, ",0,1,B2,1,volume,2\n"));
    g_free(csv);
}


/** @brief Malý ring: přepisuje nejstarší, first_seq roste; kopie začne od first_seq. */
void test_audio_capture_memory_ring_overflow(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.events = p.psg_writes = p.reference_wav = false;
    p.memory = true;
    p.ring_levels = 4;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);                        /* výchozí úrovně = prvních N záznamů */
    for (unsigned i = 0; i < 10; i++) audio_capture_on_change(0, (uint8_t) ((i & 1) ? 0 : 15), T0 + 10 + i);
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    uint64_t total = AUDIO_SRC_CHANNELS_COUNT + 10;
    TEST_ASSERT_EQUAL_UINT64(total, st.streams[AUDIO_CAPTURE_STREAM_LEVELS].next_seq);
    TEST_ASSERT_EQUAL_UINT64(total - 4, st.streams[AUDIO_CAPTURE_STREAM_LEVELS].first_seq);
    TEST_ASSERT_EQUAL_UINT64(4, st.streams[AUDIO_CAPTURE_STREAM_LEVELS].capacity);

    st_AUDIO_CAPTURE_STREAM_COPY c;
    TEST_ASSERT_TRUE(audio_capture_stream_copy(AUDIO_CAPTURE_STREAM_LEVELS, 0, 100, 1u << 20, &c));
    TEST_ASSERT_TRUE(c.live);
    TEST_ASSERT_EQUAL_UINT64(total - 4, c.data_seq);
    TEST_ASSERT_EQUAL_UINT64(4, c.data_count);
    const st_AUDIO_CAPTURE_LEVEL_REC *l = (const st_AUDIO_CAPTURE_LEVEL_REC *) c.data;
    TEST_ASSERT_EQUAL_UINT64(T0 + 16, l[0].t);         /* 7. změna (i = 6) */
    TEST_ASSERT_EQUAL_UINT64(T0 + 19, l[3].t);
    audio_capture_stream_copy_clear(&c);

    /* Strop bajtů: jen 2 záznamy. */
    TEST_ASSERT_TRUE(audio_capture_stream_copy(AUDIO_CAPTURE_STREAM_LEVELS, total - 3, 100,
                                               2 * sizeof(st_AUDIO_CAPTURE_LEVEL_REC), &c));
    TEST_ASSERT_EQUAL_UINT64(total - 3, c.data_seq);
    TEST_ASSERT_EQUAL_UINT64(2, c.data_count);
    audio_capture_stream_copy_clear(&c);
}


/** @brief Segment roste se startem, data předchozího zachycení platí do dalšího startu. */
void test_audio_capture_memory_segment(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    unsigned seg = st.segment;
    TEST_ASSERT_TRUE(seg > 0);
    audio_capture_on_frame(T0);
    audio_capture_on_change(0, 15, T0 + 1);
    TEST_ASSERT_TRUE(audio_capture_stop());

    st_AUDIO_CAPTURE_STREAM_COPY c = copy_all(AUDIO_CAPTURE_STREAM_LEVELS);
    TEST_ASSERT_EQUAL_UINT(seg, c.segment);
    TEST_ASSERT_FALSE(c.live);
    TEST_ASSERT_TRUE(c.present);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 1, c.next_seq);
    audio_capture_stream_copy_clear(&c);

    /* Nový start bez paměti: segment + 1, ringy uvolněné. */
    st_AUDIO_CAPTURE_PARAMS q = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&q, NULL, 0));
    c = copy_all(AUDIO_CAPTURE_STREAM_LEVELS);
    TEST_ASSERT_EQUAL_UINT(seg + 1, c.segment);
    TEST_ASSERT_FALSE(c.present);
    TEST_ASSERT_EQUAL_UINT64(0, c.next_seq);
    TEST_ASSERT_NULL(c.data);
    audio_capture_stream_copy_clear(&c);
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.memory);
    TEST_ASSERT_EQUAL_UINT64(0, st.memory_bytes);
}


/** @brief Zachycení jen do paměti: bez složky a capture.json. */
void test_audio_capture_memory_only(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 3);
    p.events = p.psg_writes = p.reference_wav = false;
    p.memory = true;
    char *sub = g_build_filename(s_dir, "not_created", NULL);
    g_strlcpy(p.dir, sub, sizeof(p.dir));
    char err[AUDIO_CAPTURE_TEXT_MAX];
    TEST_ASSERT_TRUE_MESSAGE(audio_capture_start(&p, err, sizeof(err)), err);
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_EQUAL_STRING("", st.dir);
    TEST_ASSERT_TRUE(st.memory_bytes > 0);
    for (unsigned f = 0; f <= 3; f++) audio_capture_on_frame(T0 + f * T_FRAME);
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_STRING("Frame count reached", st.stop_reason);
    TEST_ASSERT_EQUAL_UINT64(3, st.streams[AUDIO_CAPTURE_STREAM_FRAMES].next_seq);
    TEST_ASSERT_FALSE(g_file_test(sub, G_FILE_TEST_EXISTS));
    g_free(sub);
}


/** @brief Souhrn snímku: obdélník na psg0_1 -> hrany, čas a energie úrovně; RMS reference. */
void test_audio_capture_memory_frame_summary(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.events = p.psg_writes = p.reference_wav = false;
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    /* 10 změn po 1000 taktech: 15 v [1000,2000), [3000,4000), ... = 5x 1000 taktů. */
    for (unsigned k = 1; k <= 10; k++) {
        audio_capture_on_change(2, (uint8_t) ((k & 1) ? 15 : 0), T0 + k * 1000u);
    }
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_frame(T0 + 2 * T_FRAME);           /* ticho */
    TEST_ASSERT_TRUE(audio_capture_stop());

    st_AUDIO_CAPTURE_STREAM_COPY c = copy_all(AUDIO_CAPTURE_STREAM_FRAMES);
    TEST_ASSERT_EQUAL_UINT64(2, c.data_count);
    const st_AUDIO_CAPTURE_FRAME_REC *f = (const st_AUDIO_CAPTURE_FRAME_REC *) c.data;
    TEST_ASSERT_EQUAL_UINT64(T0, f[0].t);
    TEST_ASSERT_EQUAL_UINT32(0, f[0].frame);
    TEST_ASSERT_EQUAL_UINT32(T_FRAME, f[0].len_ticks);
    TEST_ASSERT_EQUAL_UINT32(10, f[0].ch[2].edges);
    TEST_ASSERT_EQUAL_UINT32(5000, f[0].ch[2].on_ticks);
    TEST_ASSERT_EQUAL_UINT32(225u * 5000u, f[0].ch[2].energy);
    TEST_ASSERT_EQUAL_UINT8(15, f[0].ch[2].max_level);
    TEST_ASSERT_EQUAL_UINT8(0, f[0].ch[2].end_level);
    TEST_ASSERT_EQUAL_UINT32(0, f[0].ch[1].edges);
    TEST_ASSERT_TRUE(f[0].flags & AUDIO_CAPTURE_FRAME_FLAG_REF);
    TEST_ASSERT_FALSE(f[0].flags & AUDIO_CAPTURE_FRAME_FLAG_SDL);
    TEST_ASSERT_TRUE(f[0].ref_samples > 0);
    TEST_ASSERT_TRUE(f[0].ref_rms[0] > 0);             /* obdélník je slyšet */
    TEST_ASSERT_EQUAL_UINT32(1, f[1].frame);
    TEST_ASSERT_EQUAL_UINT64(T0 + T_FRAME, f[1].t);
    TEST_ASSERT_EQUAL_UINT32(0, f[1].ch[2].edges);
    TEST_ASSERT_EQUAL_UINT32(0, f[1].ch[2].on_ticks);
    audio_capture_stream_copy_clear(&c);
}


/**
 * @brief Poloviční výchylka (tón PSG s děličem 001h, hodnoty 16..30): ring levels
 *        nese hodnotu zvukového logu, souhrn snímku stupeň hlasitosti a energii x 6 / 25.
 */
void test_audio_capture_memory_half_level(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.psg_writes = p.reference_wav = false;
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    /* hodnota 25 = poloviční výchylka stupně 10, v [1000,2000), [3000,4000), ... */
    for (unsigned k = 1; k <= 10; k++) {
        audio_capture_on_change(2, (uint8_t) ((k & 1) ? 25 : 0), T0 + k * 1000u);
    }
    audio_capture_on_frame(T0 + T_FRAME);
    TEST_ASSERT_TRUE(audio_capture_stop());

    gsize len = 0;
    char *ev = read_file("events.csv", &len);
    TEST_ASSERT_NOT_NULL(strstr(ev, ",psg0_1,25\n"));
    g_free(ev);

    st_AUDIO_CAPTURE_STREAM_COPY lv = copy_all(AUDIO_CAPTURE_STREAM_LEVELS);
    const st_AUDIO_CAPTURE_LEVEL_REC *l = (const st_AUDIO_CAPTURE_LEVEL_REC *) lv.data;
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 10, lv.data_count);
    TEST_ASSERT_EQUAL_UINT8(25, l[AUDIO_SRC_CHANNELS_COUNT].level);
    TEST_ASSERT_EQUAL_UINT8(0, l[AUDIO_SRC_CHANNELS_COUNT + 1].level);
    audio_capture_stream_copy_clear(&lv);

    st_AUDIO_CAPTURE_STREAM_COPY c = copy_all(AUDIO_CAPTURE_STREAM_FRAMES);
    TEST_ASSERT_EQUAL_UINT64(1, c.data_count);
    const st_AUDIO_CAPTURE_FRAME_REC *f = (const st_AUDIO_CAPTURE_FRAME_REC *) c.data;
    TEST_ASSERT_EQUAL_UINT32(10, f[0].ch[2].edges);
    TEST_ASSERT_EQUAL_UINT32(5000, f[0].ch[2].on_ticks);
    TEST_ASSERT_EQUAL_UINT32(100u * 5000u * 6u / 25u, f[0].ch[2].energy);
    TEST_ASSERT_EQUAL_UINT8(10, f[0].ch[2].max_level);
    TEST_ASSERT_EQUAL_UINT8(0, f[0].ch[2].end_level);
    TEST_ASSERT_TRUE(f[0].ref_rms[0] > 0);
    audio_capture_stream_copy_clear(&c);
}


/**
 * @brief CTC0 v režimu 3 s děličem 2 (hodnota 31): ring levels a events.csv nesou
 *        hodnotu zvukového logu, souhrn snímku stupeň 15 a energii x 41 / 100.
 */
void test_audio_capture_memory_ctc0_div2_level(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.psg_writes = p.reference_wav = false;
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    /* hodnota 31 v [1000,2000), [3000,4000), ... */
    for (unsigned k = 1; k <= 10; k++) {
        audio_capture_on_change(0, (uint8_t) ((k & 1) ? AUDIO_LEVEL_CTC0_DIV2 : 0), T0 + k * 1000u);
    }
    audio_capture_on_frame(T0 + T_FRAME);
    TEST_ASSERT_TRUE(audio_capture_stop());

    gsize len = 0;
    char *ev = read_file("events.csv", &len);
    TEST_ASSERT_NOT_NULL(strstr(ev, ",ctc0,31\n"));
    g_free(ev);

    st_AUDIO_CAPTURE_STREAM_COPY lv = copy_all(AUDIO_CAPTURE_STREAM_LEVELS);
    const st_AUDIO_CAPTURE_LEVEL_REC *l = (const st_AUDIO_CAPTURE_LEVEL_REC *) lv.data;
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 10, lv.data_count);
    TEST_ASSERT_EQUAL_UINT8(31, l[AUDIO_SRC_CHANNELS_COUNT].level);
    TEST_ASSERT_EQUAL_UINT8(0, l[AUDIO_SRC_CHANNELS_COUNT + 1].level);
    audio_capture_stream_copy_clear(&lv);

    st_AUDIO_CAPTURE_STREAM_COPY c = copy_all(AUDIO_CAPTURE_STREAM_FRAMES);
    TEST_ASSERT_EQUAL_UINT64(1, c.data_count);
    const st_AUDIO_CAPTURE_FRAME_REC *f = (const st_AUDIO_CAPTURE_FRAME_REC *) c.data;
    TEST_ASSERT_EQUAL_UINT32(10, f[0].ch[0].edges);
    TEST_ASSERT_EQUAL_UINT32(5000, f[0].ch[0].on_ticks);
    /* součet po úsecích, každý zaokrouhlený dolů: 5 x (225 x 1000 x 41 / 100) */
    TEST_ASSERT_EQUAL_UINT32(5u * (225u * 1000u * 41u / 100u), f[0].ch[0].energy);
    TEST_ASSERT_EQUAL_UINT8(15, f[0].ch[0].max_level);
    TEST_ASSERT_EQUAL_UINT8(0, f[0].ch[0].end_level);
    TEST_ASSERT_TRUE(f[0].ref_rms[0] > 0);
    audio_capture_stream_copy_clear(&c);
}


/** @brief PCM ringy obsahují bajtově totéž co reference.wav a sdl.wav. */
void test_audio_capture_memory_pcm_matches_wav(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(true, 0);
    p.memory = true;
    p.pcm_reference = true;
    p.pcm_sdl = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    float block[2 * 300];
    for (int i = 0; i < 600; i++) block[i] = (float) ((i % 7) - 3) / 8.0f;
    for (unsigned f = 0; f < 10; f++) {
        if (f == 7) audio_capture_on_change(3, 12, T0 + f * T_FRAME + 500);
        audio_capture_on_frame(T0 + f * T_FRAME);
        audio_capture_sdl_output(block, 300);
    }
    TEST_ASSERT_TRUE(audio_capture_stop());

    const char *names[2] = { "reference.wav", "sdl.wav" };
    const en_AUDIO_CAPTURE_STREAM streams[2] = { AUDIO_CAPTURE_STREAM_PCM_REFERENCE, AUDIO_CAPTURE_STREAM_PCM_SDL };
    for (int k = 0; k < 2; k++) {
        gsize len = 0;
        char *wav = read_file(names[k], &len);
        st_AUDIO_CAPTURE_STREAM_COPY c = copy_all(streams[k]);
        TEST_ASSERT_TRUE_MESSAGE(c.data_count > 0, names[k]);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(len - 44, c.data_count * 4, names[k]);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(wav + 44, c.data, (size_t) (len - 44), names[k]);
        audio_capture_stream_copy_clear(&c);
        g_free(wav);
    }
    st_AUDIO_CAPTURE_STREAM_COPY fr = copy_all(AUDIO_CAPTURE_STREAM_FRAMES);
    const st_AUDIO_CAPTURE_FRAME_REC *f = (const st_AUDIO_CAPTURE_FRAME_REC *) fr.data;
    TEST_ASSERT_TRUE(fr.data_count >= 3);
    TEST_ASSERT_TRUE(f[fr.data_count - 1].flags & AUDIO_CAPTURE_FRAME_FLAG_SDL);
    TEST_ASSERT_EQUAL_UINT16(1, f[fr.data_count - 1].sdl_blocks);
    TEST_ASSERT_EQUAL_UINT32(300, f[fr.data_count - 1].sdl_samples);
    TEST_ASSERT_TRUE(f[fr.data_count - 1].sdl_peak[0] > 0);
    audio_capture_stream_copy_clear(&fr);
}


/**
 * @brief Doba kopie 4 MiB z ringu (strop jedné kopie trace_read a PCM výřezu).
 *
 * Kopie drží zámek modulu, na který čeká i vlákno SDL zvuku. Test čas jen
 * měří a vypíše (bez meze, čas závisí na stroji); ověří úplnost kopie.
 */
void test_audio_capture_memory_copy_time(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    p.events = p.psg_writes = p.reference_wav = false;
    p.memory = true;
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    const uint32_t n = (4u << 20) / sizeof(st_AUDIO_CAPTURE_LEVEL_REC);
    for (uint32_t i = 0; i < n; i++) audio_capture_on_change(0, (uint8_t) ((i & 1) ? 15 : 0), T0 + 1 + i);
    st_AUDIO_CAPTURE_STREAM_COPY c;
    gint64 best = G_MAXINT64, worst = 0;
    for (int k = 0; k < 5; k++) {
        gint64 t0 = g_get_monotonic_time();
        TEST_ASSERT_TRUE(audio_capture_stream_copy(AUDIO_CAPTURE_STREAM_LEVELS, 0, UINT64_MAX, 4u << 20, &c));
        gint64 dt = g_get_monotonic_time() - t0;
        TEST_ASSERT_EQUAL_UINT64(n, c.data_count);
        audio_capture_stream_copy_clear(&c);
        if (dt < best) best = dt;
        if (dt > worst) worst = dt;
    }
    printf("audio_capture_stream_copy 4 MiB: best of 5 = %lld us, worst = %lld us\n",
           (long long) best, (long long) worst);
}


/** @brief Paměť jako jediný výstup je platný výběr; bez výstupu chyba zmiňuje memory. */
void test_audio_capture_memory_start_errors(void)
{
    char err[AUDIO_CAPTURE_TEXT_MAX];
    st_AUDIO_CAPTURE_PARAMS none = params(false, 0);
    none.events = none.psg_writes = none.reference_wav = false;
    TEST_ASSERT_FALSE(audio_capture_start(&none, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "memory"));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_audio_capture_start_events_psg);
    RUN_TEST(test_audio_capture_stop_while_pending);
    RUN_TEST(test_audio_capture_reference_wav);
    RUN_TEST(test_audio_capture_sdl_warmup);
    RUN_TEST(test_audio_capture_timeline_restart);
    RUN_TEST(test_audio_capture_start_errors);
    RUN_TEST(test_audio_capture_memory_matches_csv);
    RUN_TEST(test_audio_capture_memory_ring_overflow);
    RUN_TEST(test_audio_capture_memory_segment);
    RUN_TEST(test_audio_capture_memory_only);
    RUN_TEST(test_audio_capture_memory_frame_summary);
    RUN_TEST(test_audio_capture_memory_half_level);
    RUN_TEST(test_audio_capture_memory_ctc0_div2_level);
    RUN_TEST(test_audio_capture_memory_pcm_matches_wav);
    RUN_TEST(test_audio_capture_memory_start_errors);
    RUN_TEST(test_audio_capture_memory_copy_time);
    return UNITY_END();
}
