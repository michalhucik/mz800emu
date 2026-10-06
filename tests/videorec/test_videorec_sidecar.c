/**
 * @file   test_videorec_sidecar.c
 * @brief  Standalone testy sidecar modelu video záznamu (segmenty, markery, party, JSON).
 *
 * Validitu JSONu jednorázově ověřil Python `json.load` mimo test (viz report úlohy).
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/videorec/videorec_sidecar.h"

static st_VIDEOREC_SIDECAR *S;

void setUp(void)
{
    S = videorec_sidecar_new(928, 288, 50, 1, 48000, VIDEOREC_TRANS_FADE, 500);
}
void tearDown(void)
{
    videorec_sidecar_free(S);
    S = NULL;
}

/** @brief Vrátí kopii řetězce bez mezer a odřádkování (volající uvolní g_free). */
static char *strip_ws(const char *in)
{
    char *out = g_malloc(strlen(in) + 1);
    char *o = out;
    for (; *in; in++) {
        if (*in != ' ' && *in != '\n' && *in != '\t' && *in != '\r') *o++ = *in;
    }
    *o = '\0';
    return out;
}

/** @brief Vyrobí JSON bez mezer; volající uvolní g_free. */
static char *json_stripped(void)
{
    char *j = videorec_sidecar_to_json(S);
    TEST_ASSERT_NOT_NULL(j);
    char *s = strip_ws(j);
    g_free(j);
    return s;
}

static void assert_contains(const char *needle)
{
    char *s = json_stripped();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, needle), s);
    g_free(s);
}

static void assert_not_contains(const char *needle)
{
    char *s = json_stripped();
    TEST_ASSERT_NULL_MESSAGE(strstr(s, needle), s);
    g_free(s);
}

static void test_single_segment_json(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 100);
    assert_contains("\"segments\":[{\"start\":0,\"end\":100,\"transition_in\":\"none\"}]");
    assert_contains("\"version\":3");
    assert_contains("\"line_doubled\":false");
    assert_contains("\"default_transition\":\"fade\"");
    assert_contains("\"transition_ms\":500");
    TEST_ASSERT_FALSE(videorec_sidecar_segment_open(S));
}

/* Od verze 2: AVI se zdvojenými řádky - width/height jsou rozměry AVI, příznak line_doubled. */
static void test_line_doubled_json(void)
{
    videorec_sidecar_free(S);
    S = videorec_sidecar_new(928, 576, 50, 1, 48000, VIDEOREC_TRANS_FADE, 500);
    videorec_sidecar_set_line_doubled(S, true);
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 10);
    assert_contains("\"version\":3");
    assert_contains("\"width\":928,\"height\":576");
    assert_contains("\"line_doubled\":true");
    assert_not_contains("\"line_doubled\":false");
}

/* Verze 4: platforma, TV norma, nativní rozměry framebufferu a obdélník canvasu (MZ-1500, 60 fps). */
static void test_platform_v4_json(void)
{
    videorec_sidecar_free(S);
    S = videorec_sidecar_new(704, 464, 60, 1, 48000, VIDEOREC_TRANS_FADE, 500);
    videorec_sidecar_set_line_doubled(S, true);
    videorec_sidecar_set_platform(S, "mz1500", "ntsc", 704, 232, 32, 16, 640, 200);
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 10);
    assert_contains("\"version\":4");
    assert_contains("\"platform\":\"mz1500\",\"tv_system\":\"ntsc\"");
    assert_contains("\"width\":704,\"height\":464");
    assert_contains("\"framebuffer_width\":704,\"framebuffer_height\":232");
    assert_contains("\"canvas\":{\"x\":32,\"y\":16,\"width\":640,\"height\":200}");
    assert_contains("\"fps_num\":60,\"fps_den\":1");
    assert_contains("\"line_doubled\":true");
    assert_not_contains("\"version\":3");
}

/* Bez videorec_sidecar_set_platform() zůstává formát verze 3 (bez polí platformy). */
static void test_without_platform_stays_v3(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 10);
    assert_contains("\"version\":3");
    assert_not_contains("\"platform\"");
    assert_not_contains("\"canvas\"");
}

/* Řetězce platformy se escapují a kopírují (volající je smí hned uvolnit). */
static void test_platform_strings_copied(void)
{
    char *name = g_strdup("mz\"800");
    char *tv = g_strdup("pal");
    videorec_sidecar_set_platform(S, name, tv, 928, 288, 154, 46, 640, 200);
    g_free(name);
    g_free(tv);
    assert_contains("\"platform\":\"mz\\\"800\",\"tv_system\":\"pal\"");
    assert_contains("\"canvas\":{\"x\":154,\"y\":46,\"width\":640,\"height\":200}");
}

static void test_pause_creates_boundary(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 100);
    videorec_sidecar_segment_begin(S, 100, VIDEOREC_TRANS_FADE);
    TEST_ASSERT_TRUE(videorec_sidecar_segment_open(S));
    videorec_sidecar_segment_end(S, 250);
    assert_contains("\"segments\":[{\"start\":0,\"end\":100,\"transition_in\":\"none\"},"
                    "{\"start\":100,\"end\":250,\"transition_in\":\"fade\"}]");
}

/* Počet segmentů: roste se segment_begin, prázdný segment se nepočítá, truncate ho sníží. */
static void test_segment_count(void)
{
    TEST_ASSERT_EQUAL_UINT(0, videorec_sidecar_segment_count(S));
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    TEST_ASSERT_EQUAL_UINT(1, videorec_sidecar_segment_count(S));
    videorec_sidecar_segment_end(S, 100);
    videorec_sidecar_segment_begin(S, 100, VIDEOREC_TRANS_FADE);
    TEST_ASSERT_EQUAL_UINT(2, videorec_sidecar_segment_count(S));
    videorec_sidecar_segment_end(S, 100); /* prázdný - zahozen */
    TEST_ASSERT_EQUAL_UINT(1, videorec_sidecar_segment_count(S));
    videorec_sidecar_segment_begin(S, 100, VIDEOREC_TRANS_FADE);
    videorec_sidecar_segment_end(S, 200);
    TEST_ASSERT_EQUAL_UINT(2, videorec_sidecar_segment_count(S));
    videorec_sidecar_truncate(S, 50);
    TEST_ASSERT_EQUAL_UINT(1, videorec_sidecar_segment_count(S));
}

static void test_empty_segment_dropped(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 0);
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_FADE);
    videorec_sidecar_segment_end(S, 10);
    assert_contains("\"segments\":[{\"start\":0,\"end\":10,\"transition_in\":\"none\"}]");
    assert_not_contains("\"fade\"},");
}

static void test_begin_closes_open_segment(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_begin(S, 40, VIDEOREC_TRANS_CUT);
    videorec_sidecar_segment_end(S, 60);
    assert_contains("\"segments\":[{\"start\":0,\"end\":40,\"transition_in\":\"none\"},"
                    "{\"start\":40,\"end\":60,\"transition_in\":\"cut\"}]");
}

static void test_truncate_cuts_segments_and_markers(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 100);
    videorec_sidecar_segment_begin(S, 100, VIDEOREC_TRANS_FADE);
    videorec_sidecar_segment_end(S, 250);
    videorec_sidecar_add_marker(S, 50, "A");
    videorec_sidecar_add_marker(S, 200, "B");
    videorec_sidecar_truncate(S, 80);
    TEST_ASSERT_TRUE(videorec_sidecar_segment_open(S));
    assert_contains("\"segments\":[{\"start\":0,\"end\":80,\"transition_in\":\"none\"}]");
    assert_contains("\"markers\":[{\"frame\":50,\"label\":\"A\"}]");
    assert_not_contains("\"B\"");
}

static void test_truncate_drops_later_parts(void)
{
    videorec_sidecar_add_part(S, "a.avi", 0);
    videorec_sidecar_add_part(S, "b.avi", 1000);
    videorec_sidecar_truncate(S, 500);
    assert_contains("\"parts\":[{\"file\":\"a.avi\",\"first_frame\":0}]");
    assert_not_contains("b.avi");
}

static void test_label_escaping(void)
{
    videorec_sidecar_add_marker(S, 1, "a\"b\\c\n");
    char *j = videorec_sidecar_to_json(S);
    TEST_ASSERT_NOT_NULL(strstr(j, "a\\\"b\\\\c\\n"));
    g_free(j);
}

static void test_transition_names_roundtrip(void)
{
    for (int t = VIDEOREC_TRANS_NONE; t <= VIDEOREC_TRANS_CARD; t++) {
        en_VIDEOREC_TRANSITION back = VIDEOREC_TRANS_NONE;
        TEST_ASSERT_TRUE(videorec_transition_from_name(videorec_transition_name((en_VIDEOREC_TRANSITION)t), &back));
        TEST_ASSERT_EQUAL_INT(t, back);
    }
    en_VIDEOREC_TRANSITION x;
    TEST_ASSERT_FALSE(videorec_transition_from_name("x", &x));
}

/* Verze 3: prázdné pole událostí je vždy přítomné. */
static void test_events_empty_array(void)
{
    assert_contains("\"events\":[]");
}

/* Události stavu {frame, kind, value} se zapíšou v pořadí snímků, value s escapováním. */
static void test_events_json(void)
{
    videorec_sidecar_add_event(S, 10, VIDEOREC_SC_EVENT_PAUSE_START, "user");
    videorec_sidecar_add_event(S, 0, VIDEOREC_SC_EVENT_TIMEBASE, "realtime");
    videorec_sidecar_add_event(S, 25, VIDEOREC_SC_EVENT_SPEED, "max");
    videorec_sidecar_add_event(S, 10, VIDEOREC_SC_EVENT_PAUSE_END, "");
    videorec_sidecar_add_event(S, 30, VIDEOREC_SC_EVENT_RESET, "a\"b");
    assert_contains("\"events\":[{\"frame\":0,\"kind\":\"timebase\",\"value\":\"realtime\"},"
                    "{\"frame\":10,\"kind\":\"pause_start\",\"value\":\"user\"},"
                    "{\"frame\":10,\"kind\":\"pause_end\",\"value\":\"\"},"
                    "{\"frame\":25,\"kind\":\"speed\",\"value\":\"max\"},"
                    "{\"frame\":30,\"kind\":\"reset\",\"value\":\"a\\\"b\"}]");
    TEST_ASSERT_EQUAL_UINT(5, videorec_sidecar_event_count(S));
}

/* Truncate zahodí události s frame >= hranice (i při přidání mimo pořadí). */
static void test_truncate_drops_events(void)
{
    videorec_sidecar_add_event(S, 50, VIDEOREC_SC_EVENT_SPEED, "200");
    videorec_sidecar_add_event(S, 5, VIDEOREC_SC_EVENT_SNAPSHOT, "seam");
    videorec_sidecar_add_event(S, 80, VIDEOREC_SC_EVENT_RESET, "");
    videorec_sidecar_truncate(S, 50);
    assert_contains("\"events\":[{\"frame\":5,\"kind\":\"snapshot\",\"value\":\"seam\"}]");
    TEST_ASSERT_EQUAL_UINT(1, videorec_sidecar_event_count(S));
}

/* Markery přidané mimo pořadí snímků se řadí (stabilně), truncate pak nezahodí platné. */
static void test_markers_sorted_insert(void)
{
    videorec_sidecar_add_marker(S, 20, "B");
    videorec_sidecar_add_marker(S, 10, "A");
    videorec_sidecar_add_marker(S, 20, "C");
    videorec_sidecar_add_marker(S, 30, "D");
    assert_contains("\"markers\":[{\"frame\":10,\"label\":\"A\"},{\"frame\":20,\"label\":\"B\"},"
                    "{\"frame\":20,\"label\":\"C\"},{\"frame\":30,\"label\":\"D\"}]");
    videorec_sidecar_truncate(S, 25);
    assert_contains("\"markers\":[{\"frame\":10,\"label\":\"A\"},{\"frame\":20,\"label\":\"B\"},"
                    "{\"frame\":20,\"label\":\"C\"}]");
}

static void test_save_writes_file(void)
{
    videorec_sidecar_segment_begin(S, 0, VIDEOREC_TRANS_NONE);
    videorec_sidecar_segment_end(S, 7);
    char *path = g_build_filename(g_get_tmp_dir(), "videorec_sidecar_test.cuts.json", NULL);
    GError *err = NULL;
    TEST_ASSERT_TRUE(videorec_sidecar_save(S, path, &err));
    TEST_ASSERT_NULL(err);
    char *content = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &content, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(content, "\"end\": 7"));
    g_free(content);
    g_remove(path);
    g_free(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_segment_json);
    RUN_TEST(test_line_doubled_json);
    RUN_TEST(test_pause_creates_boundary);
    RUN_TEST(test_empty_segment_dropped);
    RUN_TEST(test_segment_count);
    RUN_TEST(test_begin_closes_open_segment);
    RUN_TEST(test_truncate_cuts_segments_and_markers);
    RUN_TEST(test_truncate_drops_later_parts);
    RUN_TEST(test_label_escaping);
    RUN_TEST(test_transition_names_roundtrip);
    RUN_TEST(test_events_empty_array);
    RUN_TEST(test_events_json);
    RUN_TEST(test_truncate_drops_events);
    RUN_TEST(test_markers_sorted_insert);
    RUN_TEST(test_save_writes_file);
    RUN_TEST(test_platform_v4_json);
    RUN_TEST(test_without_platform_stays_v3);
    RUN_TEST(test_platform_strings_copied);
    return UNITY_END();
}
