/*
 * test_videorec_realtime.c - integrační testy režimu "podle reality" (Task 18)
 *
 * Testuje lepidlo videorec.c v časové základně realtime bez emu vlákna a bez
 * GUI: vzorkovač běží v ručním režimu (videorec_test_rt_manual()) a test volá
 * jeho tick s vlastním časem (videorec_test_rt_tick()), zobrazené snímky
 * dodává přes videorec_rt_video_tap() a výstup SDL cesty přes
 * videorec_rt_audio_output(). Emulovaný snímek se simuluje posunem
 * g_gdg.total_elapsed.screens a voláním hooků (jako ostatní testy videorec).
 * Stav emulátoru (pauza, rychlost, důvod pauzy) test nastavuje přímo
 * v g_emulator a g_customspeed.
 *
 * Ověřuje se na hotových souborech: počet snímků AVI, obsah snímků
 * (rozbalení klíčových snímků ZMBV), zvuk (PCM chunky) a sidecar verze 4
 * (segmenty, markery, události stavu).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "emulator/customspeed.h"
#include "emulator/emulator.h"
#include "emulator/videorec/videorec.h"
#include "hw-generic/gdg/framebuffer.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"

static const char *AVI = "tests/data/tmp/test_vr_rt.avi";
static const char *CUTS = "tests/data/tmp/test_vr_rt.cuts.json";

/** Zpoždění obrazu v realtime [snímky] (délka zpožďovací fronty). */
#define D videorec_rt_video_delay_ticks(VIDEO_SCREENS_PER_SEC, 1)
/** Rozměr nativního snímku. */
#define FB_SIZE ((size_t)VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT)
/** Výška snímku v AVI (zdvojené řádky). */
#define AVI_H (2u * VIDEO_DISPLAY_HEIGHT)
/** Hodnota pixelu pro snímek, jehož obsah se neověřuje (delta snímek ZMBV). */
#define PX_UNKNOWN 0xFFu
/** Vzorků zvuku na snímek při 48 kHz. */
#define SPF 960u
/** Hodnota zvuku SDL výstupu v testech (F32) a odpovídající int16. */
#define AUDIO_V 0.5f
#define AUDIO_S16_LEVEL 16384

/** Virtuální monotónní čas [us] (roste napříč testy, hodiny session se zakládají prvním tickem). */
static int64_t s_now = 1000000;
/** Hodnota pixelu, kterou má aktuální zobrazený snímek (pro kontrolu zpoždění). */
static uint8_t s_shown = 0;

void setUp(void)
{
    g_mkdir_with_parents("tests/data/tmp", 0755);
    g_remove(AVI);
    g_remove(CUTS);
    st_VIDEOREC_SETTINGS d = g_videorec_settings;
    d.retake_mode = VIDEOREC_RETAKE_DISCARD;
    d.default_transition = VIDEOREC_TRANS_FADE;
    d.audio_rate = 48000;
    d.keyframe_interval = 1; /* klíčový každý druhý snímek - obsah jde rozbalit */
    d.timebase = VIDEOREC_TIMEBASE_REALTIME;
    d.realtime_pause = VIDEOREC_RT_PAUSE_SKIP;
    d.realtime_pause_cap_s = 1;
    d.realtime_speed = VIDEOREC_RT_SPEED_AS_SEEN;
    d.realtime_turbo_audio = VIDEOREC_TURBO_AUDIO_AS_HEARD;
    d.state_marks = VIDEOREC_STATE_MARKS_SIDECAR;
    d.auto_markers = true;
    d.record_debugger_steps = false;
    g_videorec_settings = d;
    g_emulator.paused = false;
    g_emulator.max_speed = false;
    g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
    g_customspeed.speed_in_percentage = 100;
    memset(g_framebuffer.pixels, 0x0F, FB_SIZE); /* obraz v okamžiku startu */
    s_shown = 0x0F;
    videorec_init();
    videorec_test_rt_manual(true);
}

void tearDown(void)
{
    videorec_exit();
    videorec_test_rt_manual(false);
    g_emulator.paused = false;
    g_emulator.max_speed = false;
    g_emulator.pause_reason = EMU_PAUSE_REASON_NONE;
    g_customspeed.speed_in_percentage = 100;
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    g_videorec_settings.keyframe_interval = 250;
    g_remove(AVI);
    g_remove(CUTS);
}

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/** Jeden emulovaný snímek: posun času GDG + hook konce snímku + horizont zvuku. */
static void emu_frame(void)
{
    g_gdg.total_elapsed.screens++;
    g_gdg.total_elapsed.ticks = 0;
    videorec_on_screen_done();
    videorec_audio_horizon(gdg_get_total_ticks());
}

/** `n` emulovaných snímků (emu_frame()). @param n Počet snímků. */
static void emu_frames(int n)
{
    for (int i = 0; i < n; i++) emu_frame();
}

/** Zobrazí snímek s hodnotou pixelů `v` (tap jako iface_video_framebuffer_screen_done()). */
static void show(uint8_t v)
{
    static uint8_t buf[FB_SIZE];
    memset(buf, v, sizeof(buf));
    videorec_on_displayed_frame(buf);
    s_shown = v;
}

/** Výstup SDL cesty: jeden blok (jeden snímek) s konstantní hodnotou. */
static void sdl_block(float v)
{
    static float buf[VIDEOREC_RT_SDL_CHUNK_MAX * 2];
    unsigned chunk = videorec_rt_sdl_chunk(VIDEO_SCREENS_PER_SEC, 1);
    for (unsigned i = 0; i < chunk * 2; i++) buf[i] = v;
    videorec_rt_audio_output(buf, chunk);
}

/** Jeden tick vzorkovače (čas + jedna perioda snímku). */
static void tick(void)
{
    videorec_test_rt_tick(s_now);
    s_now += 20000;
}

/**
 * `n` ticků živého běhu: před každým tickem nový zobrazený snímek (hodnota
 * 1..14 cyklicky) a jeden blok zvuku SDL cesty (jako 100 % s audio zařízením).
 */
static void live_ticks(int n)
{
    for (int i = 0; i < n; i++) {
        show((uint8_t)(1 + (s_shown % 14)));
        sdl_block(AUDIO_V);
        tick();
    }
}

/** Start nahrávání (zpracuje se na konci emulovaného snímku, ten se nezapisuje). */
static void rec_start_ex(uint64_t stop_after)
{
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, AVI, sizeof(s.path));
    for (int c = 0; c < VIDEOREC_AUDIO_MAX_CHANNELS; c++)
        for (int v = 0; v < VIDEOREC_AUDIO_LEVELS; v++) s.level[c][v] = 0.0f;
    s.stop_after_frames = stop_after;
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/** Start nahrávání bez limitu snímků (rec_start_ex(0)). */
static void rec_start(void)
{
    rec_start_ex(0);
}

/** Stop v realtime: požadavek zpracuje nejbližší tick vzorkovače; pak join writeru. */
static void rec_stop_rt(void)
{
    videorec_request_stop();
    tick();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit();
}

/** Stop v emulačním čase (na konci snímku); pak join writeru. */
static void rec_stop_emu(void)
{
    videorec_request_stop();
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit();
}

/** @return Aktuální stav nahrávání (videorec_get_status()). */
static st_VIDEOREC_STATUS status(void)
{
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    return st;
}

/** @return 32bit little-endian hodnota na `p`. @param p Ukazatel na 4 bajty. */
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Najde první výskyt FourCC v bufferu.
 * @param b Buffer. @param n Délka. @param fcc 4znakový kód.
 * @return Ukazatel na výskyt, nebo NULL.
 */
static const uint8_t *find4(const uint8_t *b, size_t n, const char *fcc)
{
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(b + i, fcc, 4) == 0) return b + i;
    return NULL;
}

/**
 * @brief Přečte AVI: počet snímků, levý kanál zvuku a hodnotu pixelu [0,0] klíčových snímků.
 *
 * Delta snímky ZMBV se nerozbalují (hodnota PX_UNKNOWN); při
 * keyframe_interval = 1 je klíčový každý druhý snímek.
 * @param px_out Výstup hodnot pixelu (g_free), nebo NULL.
 * @param audio_out Výstup levého kanálu (g_free), nebo NULL.
 * @param naudio Výstup počtu vzorků zvuku.
 * @return Počet snímků (ověřeno avih proti idx1).
 */
static uint32_t avi_read(uint8_t **px_out, int16_t **audio_out, size_t *naudio)
{
    gchar *b = NULL;
    gsize n = 0;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(AVI, &b, &n, NULL), AVI);
    const uint8_t *u = (const uint8_t *)b;
    const uint8_t *avih = find4(u, n, "avih");
    const uint8_t *movi = find4(u, n, "movi");
    const uint8_t *idx = find4(u, n, "idx1");
    TEST_ASSERT_NOT_NULL(avih);
    TEST_ASSERT_NOT_NULL(movi);
    TEST_ASSERT_NOT_NULL(idx);
    uint32_t frames = rd32(avih + 8 + 16);
    uint32_t entries = rd32(idx + 4) / 16, video = 0;
    uint8_t *px = g_malloc0(frames + 1);
    int16_t *au = NULL;
    size_t cnt = 0, cap = 0;
    size_t raw_size = 768 + (size_t)VIDEO_DISPLAY_WIDTH * AVI_H;
    uint8_t *raw = g_malloc(raw_size);
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = idx + 8 + 16 * i;
        const uint8_t *c = movi + rd32(e + 8);
        if (memcmp(c, e, 4) != 0) c = u + rd32(e + 8);
        uint32_t sz = rd32(e + 12);
        if (memcmp(e, "00dc", 4) == 0) {
            TEST_ASSERT_TRUE(sz > 0);
            if (sz <= 7 || (c[8] & 0x01) == 0) {
                /* delta snímek (keyframe_interval = 1: každý druhý) - obsah se neověřuje */
                if (video < frames) px[video] = PX_UNKNOWN;
                video++;
                continue;
            }
            z_stream zs;
            memset(&zs, 0, sizeof(zs));
            TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&zs));
            zs.next_in = (Bytef *)(c + 8 + 7);
            zs.avail_in = (uInt)(sz - 7);
            zs.next_out = raw;
            zs.avail_out = (uInt)raw_size;
            int r = inflate(&zs, Z_SYNC_FLUSH);
            TEST_ASSERT_TRUE(r == Z_OK || r == Z_STREAM_END);
            inflateEnd(&zs);
            if (video < frames) px[video] = raw[768];
            video++;
        } else if (memcmp(e, "01wb", 4) == 0) {
            size_t ns = sz / 4;
            if (cnt + ns > cap) {
                cap = (cnt + ns) * 2;
                au = g_realloc(au, cap * sizeof(int16_t));
            }
            for (size_t k = 0; k < ns; k++) au[cnt + k] = (int16_t)(c[8 + 4 * k] | (c[8 + 4 * k + 1] << 8));
            cnt += ns;
        }
    }
    g_free(raw);
    g_free(b);
    TEST_ASSERT_EQUAL_UINT32(frames, video);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)frames * SPF, cnt);
    if (px_out) *px_out = px; else g_free(px);
    if (audio_out) *audio_out = au; else g_free(au);
    if (naudio) *naudio = cnt;
    return frames;
}

/** @return Počet video snímků AVI (avi_read() bez výstupů). */
static uint32_t avi_frames(void)
{
    return avi_read(NULL, NULL, NULL);
}

/** Obsah sidecaru (g_free). */
static gchar *cuts_json(void)
{
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(CUTS, &j, NULL, NULL));
    return j;
}

/** @return Počet (i překrývajících se) výskytů `needle` v `hay`. */
static int count_str(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) c++;
    return c;
}

/** Ověří, že `json` obsahuje `needle` (při selhání vypíše celý JSON). */
static void assert_has(const char *json, const char *needle)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, needle), json);
}

/** Text události sidecaru (formát videorec_sidecar_to_json()). */
static void assert_event(const char *json, unsigned frame, const char *kind, const char *value)
{
    char e[160];
    g_snprintf(e, sizeof(e), "{ \"frame\": %u, \"kind\": \"%s\", \"value\": \"%s\" }", frame, kind, value);
    assert_has(json, e);
}

/** Ověří segment sidecaru `{start, end, transition_in}` (formát videorec_sidecar_to_json()). */
static void assert_segment(const char *json, unsigned start, unsigned end, const char *tin)
{
    char e[160];
    g_snprintf(e, sizeof(e), "{ \"start\": %u, \"end\": %u, \"transition_in\": \"%s\" }", start, end, tin);
    assert_has(json, e);
}

/** Ověří marker sidecaru `{frame, label}` (formát videorec_sidecar_to_json()). */
static void assert_marker(const char *json, unsigned frame, const char *label)
{
    char e[160];
    g_snprintf(e, sizeof(e), "{ \"frame\": %u, \"label\": \"%s\" }", frame, label);
    assert_has(json, e);
}

/* ================================================================
 * Testy
 * ================================================================ */

/*
 * Základ: start v realtime, 20 živých ticků. Každý tick zapíše 1 snímek;
 * obraz je zpožděný o D (první snímky = obraz
 * v okamžiku startu), zvuk = výstup SDL cesty převzorkovaný na 48 kHz (první
 * 2 snímky ticho - plnění jitter bufferu na 3 bloky). Stop dopíše frontu.
 */
static void test_realtime_basic_delay_and_audio(void)
{
    rec_start();
    st_VIDEOREC_STATUS st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase_effective);

    uint8_t shown[20];
    for (int i = 0; i < 20; i++) {
        live_ticks(1);
        shown[i] = s_shown;
    }
    st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_ACTIVITY_LIVE, st.rt_activity);
    TEST_ASSERT_EQUAL_UINT64(20, st.frames);

    /* emulované snímky v realtime nic nezapisují */
    emu_frames(5);
    TEST_ASSERT_EQUAL_UINT64(20, videorec_get_frames());

    st_VIDEOREC_RT_STATS rs;
    videorec_get_rt_stats(&rs);
    TEST_ASSERT_EQUAL_UINT64(20, rs.ticks);
    TEST_ASSERT_EQUAL_UINT64(20, rs.frames_written);
    TEST_ASSERT_EQUAL_UINT64(0, rs.frames_repeated);
    TEST_ASSERT_EQUAL_UINT64(0, rs.audio.underruns);

    rec_stop_rt();
    uint8_t *px = NULL;
    int16_t *au = NULL;
    size_t na = 0;
    uint32_t frames = avi_read(&px, &au, &na);
    TEST_ASSERT_EQUAL_UINT32(20 + D, frames);
    for (unsigned i = 0; i < frames; i++) {
        uint8_t expect = (i < D) ? 0x0F : shown[i - D];
        char msg[64];
        g_snprintf(msg, sizeof(msg), "frame %u", i);
        if (px[i] != PX_UNKNOWN) TEST_ASSERT_EQUAL_HEX8_MESSAGE(expect, px[i], msg);
    }
    /* zvuk: snímky 0-1 ticho (plnění), od snímku 2 hodnota SDL výstupu */
    for (unsigned i = 0; i < 2 * SPF; i++) TEST_ASSERT_EQUAL_INT16(0, au[i]);
    for (unsigned i = 2 * SPF; i < 20 * SPF; i++) TEST_ASSERT_INT16_WITHIN(2, AUDIO_S16_LEVEL, au[i]);
    g_free(px);
    g_free(au);

    gchar *j = cuts_json();
    assert_has(j, "\"version\": 4");
    assert_has(j, "\"platform\": \"mz800\", \"tv_system\": \"pal\"");
    assert_segment(j, 0, frames, "none");
    assert_event(j, 0, "timebase", "realtime");
    assert_event(j, 0, "speed", "100");
    TEST_ASSERT_EQUAL_INT(1, count_str(j, "\"start\":"));
    g_free(j);
}

/* Bez nových zobrazených snímků se opakuje poslední obraz (statistika opakování). */
static void test_realtime_repeats_last_displayed_frame(void)
{
    rec_start();
    show(3);
    for (int i = 0; i < 10; i++) {
        sdl_block(AUDIO_V);
        tick();
    }
    st_VIDEOREC_RT_STATS rs;
    videorec_get_rt_stats(&rs);
    TEST_ASSERT_EQUAL_UINT64(10, rs.frames_written);
    TEST_ASSERT_EQUAL_UINT64(9, rs.frames_repeated);
    rec_stop_rt();
    uint8_t *px = NULL;
    uint32_t frames = avi_read(&px, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(10 + D, frames);
    for (unsigned i = D; i < frames; i++)
        if (px[i] != PX_UNKNOWN) TEST_ASSERT_EQUAL_HEX8(3, px[i]);
    g_free(px);
}

/*
 * Přepnutí časové základny za běhu: emulated -> realtime (hned na ticku)
 * a realtime -> emulated (na nejbližším konci emulovaného snímku). Každé
 * přepnutí je hranice segmentu a událost timebase.
 */
static void test_switch_timebase_segments_and_events(void)
{
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    rec_start();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, status().timebase_effective);
    live_ticks(2); /* emulační čas: ticky jen sledují stav */
    emu_frames(10);
    TEST_ASSERT_EQUAL_UINT64(10, videorec_get_frames());

    TEST_ASSERT_TRUE(videorec_request_timebase(VIDEOREC_TIMEBASE_REALTIME));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase);
    live_ticks(5);
    st_VIDEOREC_STATUS st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase_effective);
    TEST_ASSERT_EQUAL_UINT64(15, st.frames);
    TEST_ASSERT_EQUAL_UINT(2, st.segment);

    TEST_ASSERT_TRUE(videorec_request_timebase(VIDEOREC_TIMEBASE_EMULATED));
    live_ticks(1); /* vzorkovač návrat jen připraví; snímek se v něm už nezapisuje */
    st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, st.timebase_effective);
    TEST_ASSERT_EQUAL_UINT64(15, st.frames);
    emu_frame(); /* návrat: dopíše frontu (D) a zapíše tento snímek */
    TEST_ASSERT_EQUAL_UINT64(15 + D + 1, videorec_get_frames());
    emu_frames(5);
    rec_stop_emu();

    TEST_ASSERT_EQUAL_UINT32(15 + D + 6, avi_frames());
    gchar *j = cuts_json();
    assert_segment(j, 0, 10, "none");
    assert_segment(j, 10, 15 + D, "fade");
    assert_segment(j, 15 + D, 15 + D + 6, "fade");
    assert_event(j, 0, "timebase", "emulated");
    assert_event(j, 10, "timebase", "realtime");
    assert_event(j, 15 + D, "timebase", "emulated");
    g_free(j);
}

/** Pomocník pauzy: 10 živých ticků, `paused` ticků v pauze emulace, 10 živých, stop. */
static uint32_t run_pause_scenario(int paused, bool dbg_step)
{
    rec_start();
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    tick(); /* první tick pauzy: událost pause_start */
    if (dbg_step) videorec_on_debugger_step();
    for (int i = 1; i < paused; i++) tick(); /* v pauze SDL cesta nic nevyrábí (zařízení stojí) */
    g_emulator.paused = false;
    live_ticks(10);
    rec_stop_rt();
    return avi_frames();
}

/* skip: pauza se nezapisuje; události pause_start / pause_end a auto marker "Pause". */
static void test_pause_skip(void)
{
    uint32_t frames = run_pause_scenario(80, false);
    TEST_ASSERT_EQUAL_UINT32(20 + D, frames);
    gchar *j = cuts_json();
    /* pozice = snímky + zpožďovací fronta (obraz v okamžiku pauzy se objeví až po ní) */
    assert_event(j, 10 + D, "pause_start", "user");
    assert_event(j, 10 + D, "pause_end", "");
    assert_marker(j, 10 + D, "Pause");
    TEST_ASSERT_EQUAL_INT(1, count_str(j, "\"start\":")); /* pauza emulace není hranice segmentu */
    g_free(j);
}

/* freeze: zapisuje se celá pauza (zamrzlý obraz, ticho). */
static void test_pause_freeze(void)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    uint32_t frames = run_pause_scenario(80, false);
    TEST_ASSERT_EQUAL_UINT32(100 + D, frames);
}

/* freeze_capped: zamrzlý obraz nejvýš realtime_pause_cap_s (1 s = 50 snímků), pak skip. */
static void test_pause_freeze_capped(void)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE_CAPPED;
    g_videorec_settings.realtime_pause_cap_s = 1;
    uint32_t frames = run_pause_scenario(80, false);
    TEST_ASSERT_EQUAL_UINT32(20 + 50 + D, frames);
}

/* Krokování debuggeru (krok během pauzy) se při record_debugger_steps = false nezapisuje ani při freeze. */
static void test_debugger_steps_not_recorded(void)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    g_videorec_settings.record_debugger_steps = false;
    uint32_t frames = run_pause_scenario(80, true);
    TEST_ASSERT_EQUAL_UINT32(20 + 1 + D, frames); /* 1 tick pauzy před krokem */
}

/* record_debugger_steps = true: krokování se zapisuje i při skip. */
static void test_debugger_steps_recorded(void)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_SKIP;
    g_videorec_settings.record_debugger_steps = true;
    uint32_t frames = run_pause_scenario(80, true);
    TEST_ASSERT_EQUAL_UINT32(20 + 79 + D, frames); /* od kroku dál */
}

/* Pauza breakpointem = debugger: value "breakpoint" a zápis podle record_debugger_steps. */
static void test_breakpoint_pause(void)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    g_videorec_settings.record_debugger_steps = false;
    rec_start();
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_BREAKPOINT;
    for (int i = 0; i < 30; i++) tick();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_ACTIVITY_SKIPPING, status().rt_activity);
    g_emulator.paused = false;
    live_ticks(5);
    rec_stop_rt();
    TEST_ASSERT_EQUAL_UINT32(15 + D, avi_frames());
    gchar *j = cuts_json();
    assert_event(j, 10 + D, "pause_start", "breakpoint");
    g_free(j);
}

/*
 * emulated_when_fast: při rychlosti != 100 % se nahrává v emulačním čase
 * (každý emulovaný snímek), po návratu na 100 % zase podle reality.
 * Změny rychlosti jsou události; auto marker rychlosti vzniká jen
 * v efektivní realtime - tady se obě změny pozorují v emulačním čase
 * (final review I2), takže markery nevzniknou.
 */
static void test_emulated_when_fast(void)
{
    g_videorec_settings.realtime_speed = VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST;
    rec_start();
    live_ticks(10);
    g_customspeed.speed_in_percentage = 200;
    live_ticks(1); /* rozhodnutí o návratu */
    emu_frame();   /* návrat: fronta (D) + tento snímek */
    st_VIDEOREC_STATUS st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, st.timebase_effective);
    emu_frames(20);
    TEST_ASSERT_EQUAL_UINT64(10 + D + 21, videorec_get_frames());
    g_customspeed.speed_in_percentage = 100;
    emu_frame();   /* změna rychlosti zaznamenaná na konci snímku */
    live_ticks(1); /* zpět do realtime */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, status().timebase_effective);
    live_ticks(9);
    rec_stop_rt();
    TEST_ASSERT_EQUAL_UINT32(10 + D + 22 + 9 + 1 + D, avi_frames());
    gchar *j = cuts_json();
    assert_event(j, 10 + D, "speed", "200");
    assert_event(j, 10 + D, "timebase", "emulated");
    assert_event(j, 10 + D + 21, "speed", "100");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_str(j, "\"label\": \"Speed"), j);
    assert_event(j, 10 + D + 22, "timebase", "realtime");
    assert_segment(j, 0, 10 + D, "none");
    assert_segment(j, 10 + D, 10 + D + 22, "fade");
    g_free(j);
}

/*
 * Změna rychlosti v efektivní realtime (as_seen) je událost i auto marker -
 * video ji ukazuje zrychleně. Po přepnutí do emulačního času další změna
 * rychlosti marker nedostane (video hraje normální rychlostí), událost ano.
 */
static void test_speed_marker_only_in_realtime(void)
{
    rec_start();
    live_ticks(5);
    g_customspeed.speed_in_percentage = 200;
    live_ticks(5);
    videorec_request_timebase(VIDEOREC_TIMEBASE_EMULATED);
    live_ticks(1);
    emu_frames(3); /* návrat (fronta D) + 3 snímky */
    g_customspeed.speed_in_percentage = 100;
    emu_frames(2);
    rec_stop_emu();
    gchar *j = cuts_json();
    assert_event(j, 5 + D, "speed", "200");
    assert_marker(j, 5 + D, "Speed 200%");
    assert_event(j, 10 + D + 3, "speed", "100");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_str(j, "\"Speed 100%\""), j);
    g_free(j);
}

/** Pomocník turbo zvuku: MAX SPEED v as_seen, vrátí průměr |zvuku| snímků 5..14. */
static int turbo_audio_level(en_VIDEOREC_TURBO_AUDIO mode)
{
    g_videorec_settings.realtime_turbo_audio = mode;
    rec_start();
    g_emulator.max_speed = true;
    live_ticks(15);
    rec_stop_rt();
    int16_t *au = NULL;
    size_t na = 0;
    (void)avi_read(NULL, &au, &na);
    long sum = 0;
    for (unsigned i = 5 * SPF; i < 15 * SPF; i++) sum += abs(au[i]);
    g_free(au);
    g_emulator.max_speed = false;
    return (int)(sum / (10 * SPF));
}

/* realtime_turbo_audio při MAX SPEED: as_heard = beze změny, silence = ticho, attenuate = 0,25. */
static void test_turbo_audio_modes(void)
{
    TEST_ASSERT_INT_WITHIN(2, AUDIO_S16_LEVEL, turbo_audio_level(VIDEOREC_TURBO_AUDIO_AS_HEARD));
    tearDown();
    setUp();
    TEST_ASSERT_EQUAL_INT(0, turbo_audio_level(VIDEOREC_TURBO_AUDIO_SILENCE));
    tearDown();
    setUp();
    TEST_ASSERT_INT_WITHIN(2, AUDIO_S16_LEVEL / 4, turbo_audio_level(VIDEOREC_TURBO_AUDIO_ATTENUATE));
}

/* Nahrání snapshotu v realtime = šev (nový segment, událost, marker), nikdy retake. */
static void test_snapshot_in_realtime_is_seam(void)
{
    rec_start();
    live_ticks(10);
    uint32_t seq = videorec_get_event_seq();
    videorec_on_snapshot_loaded(NULL);
    live_ticks(10);
    TEST_ASSERT_EQUAL_UINT32(seq + 1, videorec_get_event_seq());
    st_VIDEOREC_EVENT e;
    TEST_ASSERT_TRUE(videorec_get_event(seq + 1, &e));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_SEAM, e.kind);
    rec_stop_rt();
    gchar *j = cuts_json();
    assert_segment(j, 0, 10 + D, "none");
    assert_segment(j, 10 + D, 20 + D, "fade");
    assert_event(j, 10 + D, "snapshot", "seam");
    assert_marker(j, 10 + D, "Snapshot loaded");
    g_free(j);
}

/*
 * Snapshot uložený v realtime úseku a nahraný po návratu do emulačního
 * času nevede na retake (realtime větev), i při retake_mode DISCARD.
 */
static void test_realtime_take_never_retake(void)
{
    rec_start();
    live_ticks(10);
    st_VIDEOREC_SNAPINFO info;
    TEST_ASSERT_TRUE(videorec_get_snapinfo(&info));
    videorec_request_timebase(VIDEOREC_TIMEBASE_EMULATED);
    live_ticks(1);
    emu_frames(5);
    uint32_t seq = videorec_get_event_seq();
    videorec_on_snapshot_loaded(&info);
    emu_frame();
    st_VIDEOREC_EVENT e;
    TEST_ASSERT_TRUE(videorec_get_event(seq + 1, &e));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_SEAM, e.kind);
    rec_stop_emu();
    gchar *j = cuts_json();
    assert_has(j, "\"value\": \"seam\""); /* událost zůstává */
    /* šev v emulačním čase bez auto markeru (final review I2) */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_str(j, "\"Snapshot loaded\""), j);
    g_free(j);
}

/*
 * Retake v emulačním čase zahodí události za bodem snapshotu; stav platný od
 * bodu snapshotu (časová základna, rychlost) se musí zapsat znovu - jinak by
 * export po pozdějším přepnutí do realtime neukázal rychlost (Task 19 review).
 */
static void test_retake_reemits_state(void)
{
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    rec_start();
    emu_frames(10);
    st_VIDEOREC_SNAPINFO info;
    TEST_ASSERT_TRUE(videorec_get_snapinfo(&info));
    TEST_ASSERT_EQUAL_UINT64(10, info.frame);
    emu_frames(5);
    g_customspeed.speed_in_percentage = 400; /* změna rychlosti v zahozené budoucnosti */
    emu_frames(5);
    videorec_on_snapshot_loaded(&info);
    emu_frame(); /* retake na snímek 10 */
    uint64_t after = videorec_get_frames(); /* 10 + snímek, na jehož konci se retake zpracoval */
    TEST_ASSERT_TRUE(after >= 10 && after <= 11);
    emu_frames(5);
    videorec_request_timebase(VIDEOREC_TIMEBASE_REALTIME);
    live_ticks(5);
    rec_stop_rt();
    g_customspeed.speed_in_percentage = 100;

    gchar *j = cuts_json();
    assert_event(j, 10, "snapshot", "retake");
    assert_event(j, 10, "timebase", "emulated");
    assert_event(j, 10, "speed", "400");
    assert_event(j, (unsigned)after + 5, "timebase", "realtime");
    /* retake ani změna rychlosti v emulačním čase auto markery nevkládají
     * (final review I2), opětovný zápis stavu také ne */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_str(j, "\"Speed 400%\""), j);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_str(j, "\"Snapshot loaded\""), j);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"value\": \"400\""), j);
    g_free(j);
}

/* Reset: událost + marker v realtime (tick) i v emulačním čase (konec snímku). */
static void test_reset_event(void)
{
    rec_start();
    live_ticks(5);
    videorec_on_reset();
    live_ticks(5);
    videorec_request_timebase(VIDEOREC_TIMEBASE_EMULATED);
    live_ticks(1);
    emu_frames(3); /* 10 + D (fronta) + 3 */
    videorec_on_reset();
    emu_frame();
    rec_stop_emu();
    gchar *j = cuts_json();
    assert_event(j, 5 + D, "reset", "");
    assert_marker(j, 5 + D, "Reset");
    assert_event(j, 10 + D + 3, "reset", "");
    assert_marker(j, 10 + D + 3, "Reset");
    g_free(j);
}

/* Markery v realtime: pozice obrazu za zpožďovací frontou. */
static void test_marker_position_realtime(void)
{
    rec_start();
    live_ticks(7);
    videorec_request_marker("M");
    live_ticks(3);
    rec_stop_rt();
    gchar *j = cuts_json();
    assert_marker(j, 7 + D, "M");
    g_free(j);
}

/* Record-pause v realtime: fronta se dopíše, segment se uzavře, po obnovení nový segment. */
static void test_record_pause_realtime(void)
{
    rec_start();
    live_ticks(10);
    TEST_ASSERT_TRUE(videorec_request_pause_set(true));
    live_ticks(20);
    st_VIDEOREC_STATUS st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, st.state);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_ACTIVITY_SKIPPING, st.rt_activity);
    TEST_ASSERT_EQUAL_UINT64(10 + D, st.frames);
    TEST_ASSERT_TRUE(videorec_request_pause_set(false));
    live_ticks(10);
    rec_stop_rt();
    TEST_ASSERT_EQUAL_UINT32(10 + D + 10 + D, avi_frames());
    gchar *j = cuts_json();
    assert_segment(j, 0, 10 + D, "none");
    assert_segment(j, 10 + D, 20 + 2 * D, "fade");
    g_free(j);
}

/*
 * Record-pause zadaná i zrušená v pauze emulace (final review M1): pauza
 * nahrávání frontu obrazu dopíše a v pauze emulace se fronta nedoplňuje.
 * Obnovení ji musí znovu naplnit na D, jinak by
 * obraz po obnovení nebyl zpožděný (zvuk ano) - zvuk by trvale zaostával.
 */
static void test_record_pause_resume_keeps_video_delay(void)
{
    rec_start();
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    tick(); /* pauza emulace (skip) */
    TEST_ASSERT_TRUE(videorec_request_pause_set(true));
    tick(); /* pauza nahrávání: fronta (D) se dopíše */
    TEST_ASSERT_EQUAL_UINT64(10 + D, status().frames);
    TEST_ASSERT_TRUE(videorec_request_pause_set(false));
    tick(); /* obnovení stále v pauze emulace */
    uint8_t frozen = s_shown;
    g_emulator.paused = false;
    uint8_t shown[10];
    for (int i = 0; i < 10; i++) {
        live_ticks(1);
        shown[i] = s_shown;
    }
    rec_stop_rt();
    uint8_t *px = NULL;
    uint32_t frames = avi_read(&px, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(10 + D + D + 10, frames);
    for (unsigned i = 10 + D; i < frames; i++) {
        unsigned k = i - (10 + D);
        uint8_t expect = (k < D) ? frozen : shown[k - D];
        char msg[64];
        g_snprintf(msg, sizeof(msg), "frame %u", i);
        if (px[i] != PX_UNKNOWN) TEST_ASSERT_EQUAL_HEX8_MESSAGE(expect, px[i], msg);
    }
    g_free(px);
    gchar *j = cuts_json();
    assert_segment(j, 10 + D, frames, "fade");
    g_free(j);
}

/* stop_after_frames v realtime: přesně N snímků (fronta se dopíše jen do limitu). */
static void test_stop_after_frames_realtime(void)
{
    rec_start_ex(30);
    for (int i = 0; i < 60 && videorec_get_state() != VIDEOREC_STATE_IDLE; i++) live_ticks(1);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit();
    TEST_ASSERT_EQUAL_UINT32(30, avi_frames());
}

/* state_marks = none a auto_markers = false: žádné události ani automatické markery. */
static void test_state_marks_none(void)
{
    g_videorec_settings.state_marks = VIDEOREC_STATE_MARKS_NONE;
    g_videorec_settings.auto_markers = false;
    rec_start();
    live_ticks(5);
    g_emulator.paused = true;
    tick();
    g_emulator.paused = false;
    videorec_on_reset();
    live_ticks(5);
    rec_stop_rt();
    gchar *j = cuts_json();
    assert_has(j, "\"events\": []");
    assert_has(j, "\"markers\": []");
    g_free(j);
}

/*
 * Emulační čas: pauzu sleduje emu vlákno - začátek v paused smyčce
 * (videorec_on_emulation_paused()), konec na prvním konci snímku po
 * pokračování. Pozice jsou přesné i když vzorkovač mezitím tickne nebo
 * neticne vůbec (pauza kratší než jeden snímek, např. MCP run frames při MAX SPEED).
 */
static void test_emulated_pause_positions_exact(void)
{
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    rec_start();
    emu_frames(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_FRAMES;
    videorec_on_emulation_paused();
    tick();
    tick();
    g_emulator.paused = false;
    emu_frames(3);
    tick(); /* vzorkovač v emulačním čase pauzu nesleduje - konec už je zapsaný na 10 */
    emu_frames(2);
    /* krátká pauza bez ticku vzorkovače */
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_paused();
    g_emulator.paused = false;
    emu_frames(4);
    rec_stop_emu();
    gchar *j = cuts_json();
    assert_event(j, 10, "pause_start", "frames");
    assert_event(j, 10, "pause_end", "");
    assert_event(j, 15, "pause_start", "user");
    assert_event(j, 15, "pause_end", "");
    TEST_ASSERT_EQUAL_INT(2, count_str(j, "\"pause_start\""));
    TEST_ASSERT_EQUAL_INT(2, count_str(j, "\"pause_end\""));
    assert_has(j, "\"markers\": []"); /* v emulačním čase bez markeru Pause */
    g_free(j);
}

/**
 * Pomocník step over / run to cursor v realtime: pauza uživatele, spuštění
 * běhu k dočasnému breakpointu (krátce zruší pauzu), pauza na breakpointu
 * (pause_reason se nenastaví), pak ruční pokračování.
 */
static uint32_t run_step_over_scenario(gchar **json)
{
    g_videorec_settings.realtime_pause = VIDEOREC_RT_PAUSE_FREEZE;
    rec_start();
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    tick();                      /* pauza uživatele (freeze: 1 snímek) */
    videorec_on_debugger_run();  /* step over: mzarch_run_to_temporary_breakpoint() */
    g_emulator.paused = false;
    for (int i = 0; i < 3; i++) tick(); /* běh k dočasnému breakpointu */
    g_emulator.paused = true;    /* dočasný breakpoint; pause_reason zůstává MANUAL */
    tick();                      /* vzorkovač vidí pauzu dřív než paused smyčka */
    videorec_on_emulation_stopped(); /* vstup do paused smyčky: běh k dočasnému BP skončil */
    videorec_on_emulation_paused();
    for (int i = 0; i < 4; i++) tick();
    g_emulator.paused = false;   /* ruční pokračování (bez akce debuggeru) */
    live_ticks(10);
    rec_stop_rt();
    *json = cuts_json();
    return avi_frames();
}

/* Step over bez record_debugger_steps: běh i pauza na dočasném breakpointu se nezapisují, bez markeru. */
static void test_step_over_is_debugger(void)
{
    g_videorec_settings.record_debugger_steps = false;
    gchar *j = NULL;
    uint32_t frames = run_step_over_scenario(&j);
    TEST_ASSERT_EQUAL_UINT32(10 + 1 + 10 + D, frames);
    assert_event(j, 10 + D, "pause_start", "user");
    assert_event(j, 11 + D, "pause_start", "debugger");
    TEST_ASSERT_EQUAL_INT(1, count_str(j, "\"label\": \"Pause\""));
    g_free(j);
}

/* Step over s record_debugger_steps: běh i pauza krokování se zapisují. */
static void test_step_over_recorded(void)
{
    g_videorec_settings.record_debugger_steps = true;
    gchar *j = NULL;
    uint32_t frames = run_step_over_scenario(&j);
    TEST_ASSERT_EQUAL_UINT32(10 + 1 + 3 + 5 + 10 + D, frames);
    TEST_ASSERT_EQUAL_INT(1, count_str(j, "\"label\": \"Pause\""));
    g_free(j);
}

/*
 * Krátký step over (dočasný breakpoint dosažen dřív, než vzorkovač tickne):
 * vzorkovač vidí jednu nepřerušenou pauzu. Ruční pokračování po dokončeném
 * kroku je normální běh uživatele - zapisuje se (i bez record_debugger_steps)
 * a další pauza je "user" s markerem "Pause".
 */
static void test_short_step_over_then_manual_resume(void)
{
    g_videorec_settings.record_debugger_steps = false;
    rec_start();
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_paused();
    tick();                       /* pauza uživatele (skip) */
    videorec_on_debugger_run();   /* step over */
    g_emulator.paused = false;
    g_emulator.paused = true;     /* dočasný breakpoint dosažen bez ticku vzorkovače */
    videorec_on_emulation_stopped(); /* vstup do paused smyčky po zastavení na dočasném BP */
    videorec_on_emulation_paused();
    for (int i = 0; i < 3; i++) tick();
    g_emulator.paused = false;    /* ruční pokračování (F5 / MCP run) */
    live_ticks(10);
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_paused();
    tick();
    g_emulator.paused = false;
    rec_stop_rt();
    TEST_ASSERT_EQUAL_UINT32(10 + 10 + D, avi_frames());
    gchar *j = cuts_json();
    assert_event(j, 20 + D, "pause_start", "user");
    assert_marker(j, 20 + D, "Pause");
    TEST_ASSERT_EQUAL_INT(2, count_str(j, "\"label\": \"Pause\""));
    g_free(j);
}

/*
 * Step over a hned pokračování v téže iteraci paused smyčky (MCP `step_over`
 * a hned `run`, final review I1): po zastavení na dočasném breakpointu
 * paused smyčka nejdřív zpracuje frontu dbgapi (pokračování zruší pauzu)
 * a teprve pak volá videorec_on_emulation_paused() - už s
 * g_emulator.paused == false. Příznak běhu k dočasnému breakpointu se proto
 * musí zrušit při vstupu do paused smyčky, jinak by se celý následující
 * volný běh v realtime bez record_debugger_steps nezapisoval. Pauza na
 * dočasném breakpointu přitom nebyla pozorovaná (vzorkovač ji neviděl), takže
 * `dbg_pause` musí zrušit už pozorovaný volný běh - další pauza uživatele je
 * "user" s markerem "Pause".
 */
static void test_step_over_then_immediate_run(void)
{
    g_videorec_settings.record_debugger_steps = false;
    rec_start();
    live_ticks(10);
    /* pauza uživatele: vstup do paused smyčky + první iterace */
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_stopped();
    videorec_on_emulation_paused();
    /* další iterace: drain dbgapi - step over (mzarch_run_to_temporary_breakpoint()) */
    videorec_on_debugger_run();
    g_emulator.paused = false;
    videorec_on_emulation_paused(); /* hook na konci iterace, emulace už běží */
    for (int i = 0; i < 2; i++) tick(); /* běh k dočasnému breakpointu se nezapisuje */
    /* dočasný breakpoint: emulator_pause(true), vstup do paused smyčky */
    g_emulator.paused = true;
    videorec_on_emulation_stopped();
    /* první iterace: drain dbgapi zpracuje MCP run dřív než hook */
    g_emulator.paused = false;
    videorec_on_emulation_paused();
    live_ticks(10); /* volný běh uživatele - musí se zapsat */
    /* další pauza uživatele: nepozorovaná pauza na dočasném BP nesmí přežít jako "debugger" */
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_stopped();
    videorec_on_emulation_paused();
    tick();
    g_emulator.paused = false;
    rec_stop_rt();
    TEST_ASSERT_EQUAL_UINT32(10 + 10 + D, avi_frames());
    gchar *j = cuts_json();
    assert_event(j, 20 + D, "pause_start", "user");
    assert_marker(j, 20 + D, "Pause");
    TEST_ASSERT_EQUAL_INT(1, count_str(j, "\"label\": \"Pause\""));
    g_free(j);
}

/* Pauza začatá během čekání na návrat do emulačního času (rt_leaving) se zaznamená. */
static void test_pause_during_rt_leaving(void)
{
    rec_start();
    live_ticks(10);
    videorec_request_timebase(VIDEOREC_TIMEBASE_EMULATED);
    tick(); /* návrat čeká na konec emulovaného snímku */
    g_emulator.paused = true;
    g_emulator.pause_reason = EMU_PAUSE_REASON_MANUAL;
    videorec_on_emulation_paused();
    tick();
    g_emulator.paused = false;
    emu_frames(3); /* návrat (fronta D) + 3 snímky */
    rec_stop_emu();
    gchar *j = cuts_json();
    assert_event(j, 10 + D, "pause_start", "user");
    assert_event(j, 10 + D, "pause_end", "");
    g_free(j);
}

/* Headless producent: zvuk se vyrábí jen v realtime a jen když buffer potřebuje data. */
static void test_headless_wants_output(void)
{
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_output());
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    rec_start();
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_output());
    videorec_request_timebase(VIDEOREC_TIMEBASE_REALTIME);
    tick();
    TEST_ASSERT_TRUE(videorec_rt_audio_wants_output());
    for (int i = 0; i < 4; i++) sdl_block(AUDIO_V); /* cíl 3 bloky + 1 */
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_output());
    rec_stop_rt();
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_output());
}

/* Bez nahrávání request_timebase změní jen nastavení pro příští start. */
static void test_request_timebase_idle(void)
{
    g_videorec_settings.timebase = VIDEOREC_TIMEBASE_EMULATED;
    TEST_ASSERT_FALSE(videorec_request_timebase(VIDEOREC_TIMEBASE_REALTIME));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase);
    st_VIDEOREC_STATUS st = status();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, st.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, st.timebase_effective);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_ACTIVITY_OFF, st.rt_activity);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_realtime_basic_delay_and_audio);
    RUN_TEST(test_realtime_repeats_last_displayed_frame);
    RUN_TEST(test_switch_timebase_segments_and_events);
    RUN_TEST(test_pause_skip);
    RUN_TEST(test_pause_freeze);
    RUN_TEST(test_pause_freeze_capped);
    RUN_TEST(test_debugger_steps_not_recorded);
    RUN_TEST(test_debugger_steps_recorded);
    RUN_TEST(test_breakpoint_pause);
    RUN_TEST(test_emulated_when_fast);
    RUN_TEST(test_speed_marker_only_in_realtime);
    RUN_TEST(test_turbo_audio_modes);
    RUN_TEST(test_snapshot_in_realtime_is_seam);
    RUN_TEST(test_realtime_take_never_retake);
    RUN_TEST(test_retake_reemits_state);
    RUN_TEST(test_reset_event);
    RUN_TEST(test_marker_position_realtime);
    RUN_TEST(test_record_pause_realtime);
    RUN_TEST(test_record_pause_resume_keeps_video_delay);
    RUN_TEST(test_stop_after_frames_realtime);
    RUN_TEST(test_state_marks_none);
    RUN_TEST(test_emulated_pause_positions_exact);
    RUN_TEST(test_step_over_is_debugger);
    RUN_TEST(test_step_over_recorded);
    RUN_TEST(test_short_step_over_then_manual_resume);
    RUN_TEST(test_step_over_then_immediate_run);
    RUN_TEST(test_pause_during_rt_leaving);
    RUN_TEST(test_headless_wants_output);
    RUN_TEST(test_request_timebase_idle);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
