/*
 * test_videorec_snapshot.c - integrační testy retake a švu při nahrání snapshotu
 *
 * Testuje lepidlo videorec.c společně se snapshot managerem (handler
 * snap_videorec) a writer vláknem bez emu vlákna a bez GUI: emulovaný snímek
 * se simuluje posunem g_gdg.total_elapsed.screens a ručním voláním hooků
 * videorec_on_screen_done() a videorec_audio_horizon(). Snapshot se ukládá
 * a nahrává do paměti (snapshot_save_to_buffer / snapshot_load_from_buffer),
 * což vrací i časovou osu GDG - stejně jako quicksave / quickload.
 *
 * Výsledek se ověřuje na hotových souborech: počet snímků v AVI (avih
 * dwTotalFrames a počet video záznamů v idx1) a segmenty v sidecaru
 * `.cuts.json`.
 *
 * Scénáře odpovídají ručním scénářům A-D z plánu (Task 8) plus snapshot
 * uložený v době, kdy snímky ještě čekají na zvuk (okno čekajících snímků),
 * retake z opuštěné větve (finální review I1), snapshot uložený při čekajícím
 * zpracování předchozího loadu (Task 21, bod mimo linii) a rollover AVI partu
 * (finální review I2, I3): nahrávka přes více partů, selhání otevření
 * dalšího partu a retake po rolloveru. Poslední test ověřuje zdvojené
 * řádky v AVI (928x576, Task 12) rozbalením klíčových snímků přes zlib. Limit partu se pro testy zmenšuje
 * přes videorec_test_set_part_limit().
 *
 * Proměnná prostředí VIDEOREC_TEST_KEEP=1 ponechá výstupy vícepartových
 * nahrávek (tests/data/tmp/test_vr_parts.* a test_vr_parts_retake.*) pro
 * kontrolu exportu (docs/tools/videorec_export.py); jinak se po testu mažou.
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

#include "emulator/emulator.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_io.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/videorec/videorec.h"
#include "hw-generic/gdg/framebuffer.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"

static const char *AVI = "tests/data/tmp/test_vr_snap.avi";
static const char *CUTS = "tests/data/tmp/test_vr_snap.cuts.json";
/** Základ cesty vícepartové nahrávky (bez ".avi"). */
static const char *PARTS_BASE = "tests/data/tmp/test_vr_parts";
/** Základ cesty nahrávky s retake po rolloveru. */
static const char *RETAKE_BASE = "tests/data/tmp/test_vr_parts_retake";
/** Základ cesty nahrávky se selháním otevření partu. */
static const char *FAIL_BASE = "tests/data/tmp/test_vr_parts_fail";
/** Základ cesty nahrávky s retake na začátek partu a stopem (prázdný part, Task 21). */
static const char *EMPTY_BASE = "tests/data/tmp/test_vr_parts_empty";
/** Limit partu pro testy rolloveru: snímek ~ 3,9 kB (zvuk 3840 B + malý ZMBV) -> ~15 snímků na part. */
#define TEST_PART_LIMIT 60000u
/** Nejvyšší číslo partu, které testy uklízejí. */
#define TEST_MAX_PARTS 9

/** Smaže soubory (a případný adresář) vícepartové nahrávky se základem @p base. */
static void remove_parts(const char *base)
{
    for (int i = 1; i <= TEST_MAX_PARTS; i++) {
        char *pth = (i == 1) ? g_strdup_printf("%s.avi", base) : g_strdup_printf("%s_%03d.avi", base, i);
        g_remove(pth);
        g_rmdir(pth);
        g_free(pth);
    }
    char *cuts = g_strdup_printf("%s.cuts.json", base);
    g_remove(cuts);
    g_free(cuts);
}

/** Uložené snapshoty (buffer z snapshot_save_to_buffer). */
static uint8_t *s_snap[2];
static size_t s_snap_size[2];

void setUp(void)
{
    g_mkdir_with_parents("tests/data/tmp", 0755);
    g_remove(AVI);
    g_remove(CUTS);
    remove_parts("tests/data/tmp/test_vr_snap");
    videorec_test_set_part_limit(0);
    g_videorec_settings.retake_mode = VIDEOREC_RETAKE_DISCARD;
    g_videorec_settings.default_transition = VIDEOREC_TRANS_FADE;
    videorec_init();
}

void tearDown(void)
{
    videorec_exit();
    for (int i = 0; i < 2; i++) {
        g_free(s_snap[i]);
        s_snap[i] = NULL;
        s_snap_size[i] = 0;
    }
    g_emulator.paused = false;
    videorec_test_set_part_limit(0);
    remove_parts("tests/data/tmp/test_vr_snap");
    const char *keep = g_getenv("VIDEOREC_TEST_KEEP");
    if (!keep || strcmp(keep, "1") != 0) {
        remove_parts(PARTS_BASE);
        remove_parts(RETAKE_BASE);
    }
    remove_parts(FAIL_BASE);
    remove_parts(EMPTY_BASE);
}

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/** Jeden emulovaný snímek: posun času GDG + hook konce snímku (+ volitelně horizont zvuku). */
static void emu_frame(bool horizon)
{
    g_gdg.total_elapsed.screens++;
    g_gdg.total_elapsed.ticks = 0;
    videorec_on_screen_done();
    if (horizon) videorec_audio_horizon(gdg_get_total_ticks());
}

/** N snímků s horizontem zvuku na konci každého. */
static void emu_frames(int n)
{
    for (int i = 0; i < n; i++) emu_frame(true);
}

/** Start nahrávání (požadavek se zpracuje na nejbližším konci snímku, ten se nezapisuje). */
static void rec_start(void)
{
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, AVI, sizeof(s.path));
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame(true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    TEST_ASSERT_EQUAL_UINT64(0, videorec_get_frames());
}

/** Stop nahrávání a dokončení všech zápisů (join writeru). */
static void rec_stop(void)
{
    videorec_request_stop();
    emu_frame(true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit();
}

/** Quicksave do slotu @p slot (emulace v pauze jako při skutečném ukládání). */
static void snap_save(int slot)
{
    g_emulator.paused = true;
    en_SNAPSHOT_RESULT r = snapshot_save_to_buffer("videorec test", &s_snap[slot], &s_snap_size[slot]);
    g_emulator.paused = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, r, snapshot_result_to_string(r));
}

/** Quickload ze slotu @p slot. */
static void snap_load(int slot)
{
    g_emulator.paused = true;
    en_SNAPSHOT_RESULT r = snapshot_load_from_buffer(s_snap[slot], s_snap_size[slot]);
    g_emulator.paused = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, r, snapshot_result_to_string(r));
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t *find4(const uint8_t *b, size_t n, const char *fcc)
{
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(b + i, fcc, 4) == 0) return b + i;
    return NULL;
}

/** Souhrn finalizovaného AVI souboru (z avih a idx1). */
typedef struct {
    uint32_t frames;      /**< avih dwTotalFrames (ověřeno proti počtu video záznamů v idx1). */
    bool first_key;       /**< První video záznam v idx1 má příznak klíčového snímku. */
    uint64_t audio_bytes; /**< Součet velikostí audio chunků podle idx1. */
} st_AVI_INFO;

/** Přečte souhrn AVI souboru @p path (musí být finalizovaný). */
static st_AVI_INFO avi_info(const char *path)
{
    st_AVI_INFO r = { 0, false, 0 };
    gchar *b = NULL;
    gsize n = 0;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(path, &b, &n, NULL), path);
    const uint8_t *u = (const uint8_t *)b;
    const uint8_t *avih = find4(u, n, "avih");
    TEST_ASSERT_NOT_NULL(avih);
    r.frames = rd32(avih + 8 + 16);
    const uint8_t *idx = find4(u, n, "idx1");
    TEST_ASSERT_NOT_NULL_MESSAGE(idx, path);
    uint32_t entries = rd32(idx + 4) / 16, video = 0;
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = idx + 8 + 16 * i;
        if (memcmp(e, "00dc", 4) == 0) {
            if (video == 0) r.first_key = (rd32(e + 4) & 0x10u) != 0;
            video++;
        } else if (memcmp(e, "01wb", 4) == 0) {
            r.audio_bytes += rd32(e + 12);
        }
    }
    g_free(b);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.frames, video, path);
    return r;
}

/** Počet snímků v AVI: ověří shodu avih dwTotalFrames s počtem video záznamů v idx1. */
static uint32_t avi_frames(void)
{
    return avi_info(AVI).frames;
}

/** Obsah sidecaru (g_free). */
static gchar *cuts_json(void)
{
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(CUTS, &j, NULL, NULL));
    return j;
}

/** Počet výskytů podřetězce. */
static int count_str(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) c++;
    return c;
}

/** Ověří, že sidecar obsahuje přesně segment s textem @p seg (např. "\"start\": 0, \"end\": 15"). */
static void assert_segment(const char *json, const char *seg)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, seg), json);
}

/* ================================================================
 * Testy
 * ================================================================ */

/* Snapshot uložený během nahrávání nese videorec/state.bin (32 B, "VREC", verze 2, ID větve). */
static void test_state_saved_while_recording(void)
{
    rec_start();
    emu_frames(7);
    snap_save(0);

    snapshot_io_t *io = snapshot_io_open_read_buffer(s_snap[0], s_snap_size[0]);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_TRUE(snapshot_io_entry_exists(io, "videorec/state.bin"));
    uint8_t st[32];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin_into(io, "videorec/state.bin", st, sizeof(st)));
    snapshot_io_close(io);

    TEST_ASSERT_EQUAL_MEMORY("VREC", st, 4);
    TEST_ASSERT_EQUAL_UINT32(2, rd32(st + 4));
    uint64_t sid = rd32(st + 8) | ((uint64_t)rd32(st + 12) << 32);
    uint64_t frame = rd32(st + 16) | ((uint64_t)rd32(st + 20) << 32);
    uint64_t take = rd32(st + 24) | ((uint64_t)rd32(st + 28) << 32);
    TEST_ASSERT_EQUAL_UINT64(videorec_get_session_id(), sid);
    TEST_ASSERT_EQUAL_UINT64(7, frame);
    st_VIDEOREC_SNAPINFO info;
    TEST_ASSERT_TRUE(videorec_get_snapinfo(&info));
    TEST_ASSERT_NOT_EQUAL(0, take);
    TEST_ASSERT_EQUAL_UINT64(info.take_id, take);
    rec_stop();
}

/* A: retake DISCARD - 10 snímků, quicksave, 10 snímků, quickload, 5 snímků -> 15 snímků, 1 segment. */
static void test_retake_discard(void)
{
    rec_start();
    emu_frames(10);
    snap_save(0);
    emu_frames(10);
    TEST_ASSERT_EQUAL_UINT64(20, videorec_get_frames());
    uint32_t seq = videorec_get_event_seq();
    snap_load(0);
    emu_frames(5);
    TEST_ASSERT_EQUAL_UINT64(15, videorec_get_frames());
    TEST_ASSERT_NOT_EQUAL(seq, videorec_get_event_seq());
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:00:00", videorec_get_last_event());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(15, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 15, \"transition_in\": \"none\"");
    g_free(j);
}

/* Retake s časem: 61 s záznamu -> text "00:01:01". */
static void test_retake_event_time_format(void)
{
    rec_start();
    emu_frames(61 * VIDEO_SCREENS_PER_SEC);
    snap_save(0);
    emu_frames(3);
    snap_load(0);
    emu_frame(true);
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:01:01", videorec_get_last_event());
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(61 * VIDEO_SCREENS_PER_SEC + 1, avi_frames());
}

/* Retake se snímky čekajícími na zvuk (okno čekajících snímků, např. 400 %):
 * snapshot uložený, když 3 snímky čekají na horizont; load dřív, než horizont
 * přijde -> snímky před bodem snapshotu se zapíšou, zbytek zahodí. */
static void test_retake_pending_at_load(void)
{
    rec_start();
    emu_frames(10);
    emu_frame(false);
    emu_frame(false);
    emu_frame(false); /* 3 snímky čekají na zvuk */
    snap_save(0);     /* bod snapshotu = 13 (počítají se i čekající snímky) */
    emu_frame(false);
    emu_frame(false); /* čeká 10..14 */
    snap_load(0);
    emu_frames(5);
    TEST_ASSERT_EQUAL_UINT64(18, videorec_get_frames());
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(18, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 18,");
    g_free(j);
}

/* Varianta: snapshot uložený s čekajícími snímky, load až po zapsání snímků
 * za bodem snapshotu (a s dalším čekajícím snímkem) -> truncate. */
static void test_retake_pending_at_save(void)
{
    rec_start();
    emu_frames(10);
    emu_frame(false);
    emu_frame(false);
    emu_frame(false); /* 3 snímky čekají na zvuk */
    snap_save(0);     /* bod snapshotu = 13 */
    emu_frames(6);    /* zapsáno 0..18 */
    emu_frame(false); /* čeká 19 */
    snap_load(0);
    emu_frames(5);
    TEST_ASSERT_EQUAL_UINT64(18, videorec_get_frames());
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(18, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 18,");
    g_free(j);
}

/* Opakovaný retake na stejný snapshot (hráč zkouší místo několikrát). */
static void test_retake_repeated(void)
{
    rec_start();
    emu_frames(10);
    snap_save(0);
    for (int i = 0; i < 3; i++) {
        emu_frames(7);
        snap_load(0);
    }
    emu_frames(4);
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(14, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    g_free(j);
}

/* B: retake OFF -> šev: 25 snímků, 2 segmenty, druhý s výchozím přechodem. */
static void test_retake_off_seam(void)
{
    g_videorec_settings.retake_mode = VIDEOREC_RETAKE_OFF;
    rec_start();
    emu_frames(10);
    snap_save(0);
    emu_frames(10);
    snap_load(0);
    emu_frames(5);
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(25, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 20, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 20, \"end\": 25, \"transition_in\": \"fade\"");
    g_free(j);
}

/* C: cizí snapshot (uložený před startem nahrávání, bez videorec dat) -> šev. */
static void test_foreign_snapshot_seam(void)
{
    emu_frames(3);
    snap_save(1); /* nenahrává se: snapshot bez videorec/state.bin */
    rec_start();
    emu_frames(10);
    snap_load(1);
    emu_frames(5);
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(15, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 10, \"end\": 15, \"transition_in\": \"fade\"");
    g_free(j);
}

/* Snapshot z jiné (dřívější) session nahrávání -> šev, ne retake. */
static void test_other_session_seam(void)
{
    rec_start();
    emu_frames(5);
    snap_save(0);
    rec_stop();
    videorec_init();
    g_remove(AVI);
    g_remove(CUTS);

    rec_start();
    emu_frames(10);
    snap_load(0);
    emu_frames(5);
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(15, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    g_free(j);
}

/* D: record-pause, quicksave během pauzy, resume, quickload.
 * Během record-pause emulace běží, takže stav snapshotu nenavazuje na poslední
 * zapsaný snímek před pauzou: po retake musí na tom místě zůstat šev
 * (segment začínající bodem snapshotu). */
static void test_pause_snapshot_retake(void)
{
    rec_start();
    emu_frames(10);
    videorec_request_pause_toggle();
    emu_frame(true); /* pauza: segment 0..10 uzavřen */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
    emu_frames(5);
    snap_save(0); /* během pauzy, bod = 10 */
    emu_frames(5);
    videorec_request_pause_toggle();
    emu_frame(true); /* resume: segment od 10, tento snímek se už zapíše */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    emu_frames(6);
    TEST_ASSERT_EQUAL_UINT64(17, videorec_get_frames());
    snap_load(0);
    emu_frames(4);
    TEST_ASSERT_EQUAL_UINT64(14, videorec_get_frames());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(14, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 10, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 10, \"end\": 14, \"transition_in\": \"fade\"");
    g_free(j);
}

/* D2: quickload během record-pause (snapshot z doby nahrávání) -> zahodí snímky
 * po bodu snapshotu, pauza trvá; po resume nový segment. */
static void test_load_during_pause(void)
{
    rec_start();
    emu_frames(8);
    snap_save(0); /* bod = 8 */
    emu_frames(4);
    videorec_request_pause_toggle();
    emu_frame(true);
    emu_frames(3);
    snap_load(0);
    emu_frames(2);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
    TEST_ASSERT_EQUAL_UINT64(8, videorec_get_frames());
    videorec_request_pause_toggle();
    emu_frame(true);
    emu_frames(4);
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(13, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 8, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 8, \"end\": 13, \"transition_in\": \"fade\"");
    g_free(j);
}

/* D3: quickload během record-pause přesune začátek pauzy na bod snapshotu;
 * snapshot uložený v téže pauze a nahraný po resume musí šev na tom místě zachovat. */
static void test_load_during_pause_then_retake(void)
{
    rec_start();
    emu_frames(8);
    snap_save(0); /* bod = 8 */
    emu_frames(4);
    videorec_request_pause_toggle();
    emu_frame(true); /* pauza od 12 */
    snap_load(0);    /* zpět na 8, pauza trvá -> pauza nyní od 8 */
    emu_frames(3);
    snap_save(1); /* během pauzy, bod = 8 */
    emu_frames(2);
    videorec_request_pause_toggle();
    emu_frame(true); /* resume: segment od 8 */
    emu_frames(5);   /* 14 */
    snap_load(1);
    emu_frames(3); /* 11 */
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(11, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 8, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 8, \"end\": 11, \"transition_in\": \"fade\"");
    g_free(j);
}

/* I1: retake na snapshot z opuštěné větve -> šev. A uložen na 10, B na 20,
 * hra do 24, load A (retake na 10), nová verze do 25, load B: B patří větvi,
 * jejíž snímky 10..19 byly retakem zahozené -> nesmí se zkrátit na 20 a
 * navázat starým stavem (neviditelný skok), musí vzniknout šev. */
static void test_retake_abandoned_branch_seam(void)
{
    rec_start();
    emu_frames(10);
    snap_save(0); /* A: bod 10 */
    emu_frames(10);
    snap_save(1); /* B: bod 20, stejná větev jako A */
    emu_frames(4);
    snap_load(0); /* retake na 10 -> nová větev */
    emu_frame(true);
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:00:00", videorec_get_last_event());
    emu_frames(14); /* 25 */
    TEST_ASSERT_EQUAL_UINT64(25, videorec_get_frames());
    snap_load(1); /* B z opuštěné větve */
    emu_frames(5);
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    TEST_ASSERT_EQUAL_UINT64(30, videorec_get_frames());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(30, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 25, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 25, \"end\": 30, \"transition_in\": \"fade\"");
    g_free(j);
}

/* I1: snapshot ze starší části aktuální linie zůstává platný i přes šev:
 * A na 5, cizí snapshot na 15 (šev, nová větev), load A -> retake na 5
 * (snímky 0..4 jsou z větve A, šev za bodem snapshotu se zahodí). */
static void test_retake_across_seam_in_lineage(void)
{
    emu_frames(2);
    snap_save(1); /* cizí: bez nahrávání */
    rec_start();
    emu_frames(5);
    snap_save(0); /* A: bod 5 */
    emu_frames(10);
    snap_load(1); /* šev na 15 */
    emu_frames(5);
    TEST_ASSERT_EQUAL_UINT64(20, videorec_get_frames());
    snap_load(0);
    emu_frames(3);
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:00:00", videorec_get_last_event());
    TEST_ASSERT_EQUAL_UINT64(8, videorec_get_frames());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(8, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 8, \"transition_in\": \"none\"");
    g_free(j);
}

/* Task 21 (2a): snapshot uložený, když zpracování předchozího loadu ještě čeká
 * na konec snímku. Emulace v pauze: load cizího F (šev se provede až na konci
 * dalšího snímku), v téže pauze save S - S nese stav F, ne stav větve, ve
 * které se nahrávalo. Resume -> šev na 10. Pozdější load S nesmí udělat retake
 * na 10 (zkrátil by segment za švem a stav F by navázal na starou hru bez
 * švu), musí vzniknout další šev a segment 10..15 zůstat. */
static void test_save_while_load_pending_seam(void)
{
    emu_frames(2);
    snap_save(1); /* F: cizí, bez nahrávání */
    rec_start();
    emu_frames(10);
    /* pauza emulace: žádný konec snímku mezi loadem a uložením */
    snap_load(1); /* F: čeká na konec snímku */
    snap_save(0); /* S: uložen při čekajícím loadu */
    emu_frame(true); /* resume: šev na 10 */
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    emu_frames(4); /* 15 */
    TEST_ASSERT_EQUAL_UINT64(15, videorec_get_frames());
    uint32_t seq = videorec_get_event_seq();
    snap_load(0);
    emu_frames(5);
    TEST_ASSERT_NOT_EQUAL(seq, videorec_get_event_seq());
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    TEST_ASSERT_EQUAL_UINT64(20, videorec_get_frames());
    rec_stop();

    TEST_ASSERT_EQUAL_UINT32(20, avi_frames());
    gchar *j = cuts_json();
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 10, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 10, \"end\": 15, \"transition_in\": \"fade\"");
    assert_segment(j, "\"start\": 15, \"end\": 20, \"transition_in\": \"fade\"");
    g_free(j);
}

/* Task 21 (2a): snapshot uložený při čekajícím loadu nese ve state.bin nulové
 * ID větve (bod neplatný pro retake); session a snímek zůstávají. */
static void test_save_while_load_pending_take_zero(void)
{
    rec_start();
    emu_frames(6);
    snap_save(0);
    snap_load(0); /* čeká na konec snímku */
    st_VIDEOREC_SNAPINFO info;
    TEST_ASSERT_TRUE(videorec_get_snapinfo(&info));
    TEST_ASSERT_EQUAL_UINT64(videorec_get_session_id(), info.session_id);
    TEST_ASSERT_EQUAL_UINT64(6, info.frame);
    TEST_ASSERT_EQUAL_UINT64(0, info.take_id);
    emu_frame(true); /* zpracování loadu: retake na 6 */
    TEST_ASSERT_TRUE(videorec_get_snapinfo(&info));
    TEST_ASSERT_NOT_EQUAL(0, info.take_id);
    rec_stop();
}

/* Nahrání snapshotu bez nahrávání nic nezmění a nevytvoří událost. */
static void test_load_when_idle(void)
{
    emu_frames(2);
    snap_save(0);
    uint32_t seq = videorec_get_event_seq();
    snap_load(0);
    emu_frames(2);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    TEST_ASSERT_EQUAL_UINT32(seq, videorec_get_event_seq());
}


/* ================================================================
 * Rollover AVI partu (finální review I2, I3)
 * ================================================================ */

/** Cesta partu @p n (1 = první) se základem @p base (g_free). */
static char *part_path(const char *base, int n)
{
    return (n == 1) ? g_strdup_printf("%s.avi", base) : g_strdup_printf("%s_%03d.avi", base, n);
}

/** Start nahrávání do @p path s malým limitem partu a slyšitelnou úrovní kanálu 0. */
static void rec_start_parts(const char *path)
{
    videorec_test_set_part_limit(TEST_PART_LIMIT);
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, path, sizeof(s.path));
    s.level[0][15] = 0.5f; /* kanál 0, hodnota 15 = slyšitelný puls */
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame(true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/**
 * Snímek se značkou pro kontrolu A/V synchronizace: každý 10. snímek (index % 10 == 5)
 * je celý bílý a zároveň po celou dobu snímku zní puls (kanál 0 = 15); ostatní
 * snímky jsou černé a tiché. Export pak musí mít bílé snímky a pulsy na stejném čase.
 */
static void emu_frame_marked(uint64_t index)
{
    bool flash = (index % 10) == 5;
    memset(g_framebuffer.pixels, flash ? 15 : 0, (size_t)VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT);
    uint64_t start = (uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS;
    if (flash) {
        videorec_audio_tap(0, 15, start);
        videorec_audio_tap(0, 0, start + VIDEO_SCREEN_TICKS - 1);
    }
    emu_frame(true);
}

/** Kolik partů nahrávka se základem @p base má (souvislá řada souborů). */
static int count_parts(const char *base)
{
    int n = 0;
    for (int i = 1; i <= TEST_MAX_PARTS; i++) {
        char *pth = part_path(base, i);
        bool ok = g_file_test(pth, G_FILE_TEST_IS_REGULAR);
        g_free(pth);
        if (!ok) break;
        n++;
    }
    return n;
}

/* I3 (a): nahrávka přes několik partů. Každý part začíná klíčovým snímkem,
 * součet snímků i zvuku sedí, sidecar obsahuje všechny party se správným
 * first_frame a jeden segment přes celou nahrávku. */
static void test_rollover_multi_part(void)
{
    remove_parts(PARTS_BASE);
    char *avi = g_strdup_printf("%s.avi", PARTS_BASE);
    char *cuts = g_strdup_printf("%s.cuts.json", PARTS_BASE);
    rec_start_parts(avi);
    const uint64_t N = 50;
    for (uint64_t i = 0; i < N; i++) emu_frame_marked(i);
    TEST_ASSERT_EQUAL_UINT64(N, videorec_get_frames());
    rec_stop();

    int parts = count_parts(PARTS_BASE);
    TEST_ASSERT_TRUE_MESSAGE(parts >= 3, "expected at least 3 AVI parts");
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(cuts, &j, NULL, NULL));
    uint64_t first = 0;
    for (int i = 1; i <= parts; i++) {
        char *pth = part_path(PARTS_BASE, i);
        st_AVI_INFO inf = avi_info(pth);
        TEST_ASSERT_TRUE_MESSAGE(inf.frames > 0, pth);
        TEST_ASSERT_TRUE_MESSAGE(inf.first_key, pth);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)inf.frames * 960 * 4, inf.audio_bytes, pth);
        char *base = g_path_get_basename(pth);
        char *entry = g_strdup_printf("{ \"file\": \"%s\", \"first_frame\": %" G_GUINT64_FORMAT " }", base, first);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, entry), j);
        g_free(entry);
        g_free(base);
        g_free(pth);
        first += inf.frames;
    }
    TEST_ASSERT_EQUAL_UINT64(N, first);
    TEST_ASSERT_EQUAL_INT_MESSAGE(parts, count_str(j, "\"file\""), j);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 50, \"transition_in\": \"none\"");
    g_free(j);
    g_free(cuts);
    g_free(avi);
}

/* I3 (b): další part nejde otevřít (v cestě je adresář) -> nahrávání skončí
 * s chybou (stav IDLE, událost FAILED), první part je finalizovaný a čitelný. */
static void test_rollover_part_open_failure(void)
{
    remove_parts(FAIL_BASE);
    char *avi = g_strdup_printf("%s.avi", FAIL_BASE);
    char *blocker = part_path(FAIL_BASE, 2);
    TEST_ASSERT_EQUAL_INT(0, g_mkdir_with_parents(blocker, 0755));
    rec_start_parts(avi);
    int i = 0;
    for (; i < 200 && videorec_get_state() != VIDEOREC_STATE_IDLE; i++) emu_frame_marked((uint64_t)i);
    TEST_ASSERT_EQUAL_INT_MESSAGE(VIDEOREC_STATE_IDLE, videorec_get_state(), "recording did not stop after the part error");
    videorec_exit(); /* join writeru: událost FAILED vzniká při zavření */

    st_VIDEOREC_EVENT e;
    TEST_ASSERT_TRUE(videorec_get_event(videorec_get_event_seq(), &e));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_FAILED, e.kind);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.text, "Cannot create video file part"), e.text);
    TEST_ASSERT_TRUE_MESSAGE(strstr(videorec_get_last_error(), "Cannot create video file part") != NULL,
                             videorec_get_last_error());
    st_AVI_INFO inf = avi_info(avi);
    TEST_ASSERT_TRUE(inf.frames > 0);
    TEST_ASSERT_TRUE(inf.first_key);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)inf.frames * 960 * 4, inf.audio_bytes);
    g_free(blocker);
    g_free(avi);
}

/* I3 (c) + I2: retake na snapshot uložený v předchozím partu -> šev (truncate přes
 * hranici partu V1 neumí); snapshot se nahraje hned po rolloveru, kdy writer
 * může být ještě pozadu (rozhoduje se až po bariéře). Retake na snapshot
 * uložený v aktuálním partu zůstává retakem. */
static void test_retake_after_rollover(void)
{
    remove_parts(RETAKE_BASE);
    char *avi = g_strdup_printf("%s.avi", RETAKE_BASE);
    char *cuts = g_strdup_printf("%s.cuts.json", RETAKE_BASE);
    rec_start_parts(avi);
    uint64_t idx = 0;
    for (; idx < 5; idx++) emu_frame_marked(idx);
    snap_save(0); /* bod 5, part 1 */
    /* Těsně za první rollover (part 1 má ~15 snímků): nahrát hned, bez čekání na writer. */
    for (; idx < 18; idx++) emu_frame_marked(idx);
    snap_load(0);
    emu_frame_marked(idx++);
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
    TEST_ASSERT_EQUAL_UINT64(19, videorec_get_frames());
    for (; idx < 22; idx++) emu_frame_marked(idx);
    snap_save(1); /* bod 22, aktuální part (part 2 začíná na 15 při TEST_PART_LIMIT) */
    for (; idx < 26; idx++) emu_frame_marked(idx);
    snap_load(1);
    emu_frame_marked(idx++);
    TEST_ASSERT_EQUAL_STRING("Retake: rewound to 00:00:00", videorec_get_last_event());
    TEST_ASSERT_EQUAL_UINT64(23, videorec_get_frames());
    rec_stop();

    int parts = count_parts(RETAKE_BASE);
    TEST_ASSERT_TRUE(parts >= 2);
    uint64_t total = 0;
    for (int i = 1; i <= parts; i++) {
        char *pth = part_path(RETAKE_BASE, i);
        st_AVI_INFO inf = avi_info(pth);
        TEST_ASSERT_TRUE_MESSAGE(inf.first_key, pth);
        total += inf.frames;
        g_free(pth);
    }
    TEST_ASSERT_EQUAL_UINT64(23, total);
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(cuts, &j, NULL, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_str(j, "\"start\""), j);
    assert_segment(j, "\"start\": 0, \"end\": 18, \"transition_in\": \"none\"");
    assert_segment(j, "\"start\": 18, \"end\": 23, \"transition_in\": \"fade\"");
    g_free(j);
    g_free(cuts);
    g_free(avi);
}

/** Počká (max. ~5 s), až writer ohlásí aspoň @p n partů; vrátí poslední zjištěný počet. */
static unsigned wait_writer_parts(unsigned n)
{
    st_VIDEOREC_STATUS st;
    for (int i = 0; i < 500; i++) {
        videorec_get_status(&st);
        if (st.parts >= n) return st.parts;
        g_usleep(10000);
    }
    return st.parts;
}

/**
 * První snímek partu 2 nahrávky z emu_frame_marked() při TEST_PART_LIMIT.
 *
 * Hranice partu závisí na velikosti zakódovaných snímků; obsah je
 * deterministický, proto ji zjistí referenční nahrávka se stejnými snímky
 * (do @p base, po zjištění smazaná).
 */
static uint64_t part2_first_frame(const char *base)
{
    remove_parts(base);
    char *avi = g_strdup_printf("%s.avi", base);
    char *cuts = g_strdup_printf("%s.cuts.json", base);
    rec_start_parts(avi);
    for (uint64_t i = 0; i < 40; i++) emu_frame_marked(i);
    rec_stop();
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(cuts, &j, NULL, NULL));
    const char *p = strstr(j, "_002.avi\", \"first_frame\": ");
    TEST_ASSERT_NOT_NULL_MESSAGE(p, j);
    uint64_t first = g_ascii_strtoull(p + strlen("_002.avi\", \"first_frame\": "), NULL, 10);
    TEST_ASSERT_TRUE_MESSAGE(first > 1 && first < 40, j);
    g_free(j);
    g_free(cuts);
    g_free(avi);
    remove_parts(base);
    videorec_init(); /* rec_stop() modul ukončil */
    return first;
}

/* Task 21 (2b): retake přesně na první snímek partu 2 a stop ve stejném snímku
 * -> part 2 by zůstal s 0 snímky. Writer ho nesmí nechat: soubor se smaže
 * a ze sidecaru zmizí, nahrávka je jen part 1. */
static void test_retake_at_part_first_then_stop(void)
{
    const uint64_t first = part2_first_frame(EMPTY_BASE);
    char *avi = g_strdup_printf("%s.avi", EMPTY_BASE);
    char *cuts = g_strdup_printf("%s.cuts.json", EMPTY_BASE);
    rec_start_parts(avi);
    uint64_t idx = 0;
    for (; idx < first; idx++) emu_frame_marked(idx);
    snap_save(0); /* bod = první snímek partu 2 */
    for (; idx < first + 5; idx++) emu_frame_marked(idx);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2, wait_writer_parts(2), "part 2 was not created");
    snap_load(0);
    videorec_request_stop();
    emu_frame_marked(idx); /* retake na začátek partu 2, hned stop: part 2 zkrácen na 0 snímků */
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit(); /* join writeru */

    st_VIDEOREC_EVENT e;
    TEST_ASSERT_TRUE(videorec_get_event(videorec_get_event_seq(), &e));
    TEST_ASSERT_EQUAL_INT_MESSAGE(VIDEOREC_EVENT_SAVED, e.kind, e.text);
    TEST_ASSERT_TRUE(videorec_get_event(videorec_get_event_seq() - 1, &e));
    TEST_ASSERT_EQUAL_INT_MESSAGE(VIDEOREC_EVENT_RETAKE, e.kind, e.text);
    TEST_ASSERT_EQUAL_INT(1, count_parts(EMPTY_BASE));
    char *p2 = part_path(EMPTY_BASE, 2);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(p2, G_FILE_TEST_EXISTS), p2);
    st_AVI_INFO inf = avi_info(avi);
    TEST_ASSERT_EQUAL_UINT64(first, inf.frames);
    TEST_ASSERT_EQUAL_UINT64(first * 960 * 4, inf.audio_bytes);
    gchar *j = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(cuts, &j, NULL, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_str(j, "\"file\""), j);
    TEST_ASSERT_NULL_MESSAGE(strstr(j, "_002.avi"), j);
    char *seg = g_strdup_printf("\"start\": 0, \"end\": %" G_GUINT64_FORMAT ", \"transition_in\": \"none\"", first);
    assert_segment(j, seg);
    g_free(seg);
    g_free(j);
    g_free(p2);
    g_free(cuts);
    g_free(avi);
}

/* ================================================================
 * Zdvojené řádky (Task 12): AVI má poměr stran okna emulátoru
 * ================================================================ */

/** Výška AVI snímku: každý řádek framebufferu dvakrát. */
#define TEST_AVI_H (2 * VIDEO_DISPLAY_HEIGHT)

/** Vzorek framebufferu: index barvy závislý na řádku, sloupci i čísle snímku (každý řádek jiný). */
static uint8_t pattern_px(unsigned x, unsigned y, unsigned f)
{
    return (uint8_t)((x / 7 + y * 3 + f * 5) & 0x0F);
}

/**
 * Rozbalí payload klíčového ZMBV snímku (hlavička 7 B + zlib: paleta 768 B + pixely).
 * @param chunk Data chunku `00dc`.
 * @param n     Velikost chunku.
 * @param out   Výstup, 768 + VIDEO_DISPLAY_WIDTH * TEST_AVI_H bajtů.
 */
static void inflate_keyframe(const uint8_t *chunk, uint32_t n, uint8_t *out, size_t out_size)
{
    TEST_ASSERT_TRUE_MESSAGE(n > 7, "video chunk too short");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, chunk[0], "expected a keyframe");
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&zs));
    zs.next_in = (Bytef *)(chunk + 7);
    zs.avail_in = (uInt)(n - 7);
    zs.next_out = out;
    zs.avail_out = (uInt)out_size;
    int r = inflate(&zs, Z_SYNC_FLUSH);
    TEST_ASSERT_TRUE(r == Z_OK || r == Z_STREAM_END);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, zs.avail_out, "keyframe payload is not palette + 928x576 pixels");
    inflateEnd(&zs);
}

/* AVI ze záznamu má rozměry 928x576 (avih, strf, sidecar v2 s line_doubled) a řádky
 * 2k i 2k+1 klíčového snímku jsou shodné s řádkem k framebufferu. Kontrola přes
 * vlastní rozbalení payloadu klíčových snímků (snímky 0 a 2 při intervalu 1),
 * takže je ověřeno i přepsání sdíleného bufferu zdvojení dalším snímkem. */
static void test_avi_line_doubled(void)
{
    unsigned saved_keyint = g_videorec_settings.keyframe_interval;
    g_videorec_settings.keyframe_interval = 1; /* klíčové snímky 0, 2 */
    rec_start();
    for (unsigned f = 0; f < 3; f++) {
        for (unsigned y = 0; y < VIDEO_DISPLAY_HEIGHT; y++)
            for (unsigned x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
                g_framebuffer.pixels[y * VIDEO_DISPLAY_WIDTH + x] = pattern_px(x, y, f);
        emu_frame(true);
    }
    rec_stop();
    g_videorec_settings.keyframe_interval = saved_keyint;
    TEST_ASSERT_EQUAL_UINT32(3, avi_frames());

    gchar *b = NULL;
    gsize n = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(AVI, &b, &n, NULL));
    const uint8_t *u = (const uint8_t *)b;
    const uint8_t *avih = find4(u, n, "avih");
    TEST_ASSERT_NOT_NULL(avih);
    TEST_ASSERT_EQUAL_UINT32(VIDEO_DISPLAY_WIDTH, rd32(avih + 8 + 32));
    TEST_ASSERT_EQUAL_UINT32(TEST_AVI_H, rd32(avih + 8 + 36));
    const uint8_t *strf = find4(u, n, "strf"); /* první strf = video BITMAPINFOHEADER */
    TEST_ASSERT_NOT_NULL(strf);
    TEST_ASSERT_EQUAL_UINT32(VIDEO_DISPLAY_WIDTH, rd32(strf + 8 + 4));
    TEST_ASSERT_EQUAL_UINT32(TEST_AVI_H, rd32(strf + 8 + 8));

    /* Průchod chunky LIST movi: '00dc' / '01wb', data zarovnaná na sudý počet bajtů. */
    const uint8_t *movi = find4(u, n, "movi");
    TEST_ASSERT_NOT_NULL(movi);
    const uint8_t *idx = find4(u, n, "idx1");
    TEST_ASSERT_NOT_NULL(idx);
    size_t raw_size = 768 + (size_t)VIDEO_DISPLAY_WIDTH * TEST_AVI_H;
    uint8_t *raw = g_malloc(raw_size);
    unsigned video = 0, checked = 0;
    for (const uint8_t *c = movi + 4; c + 8 <= idx; ) {
        uint32_t sz = rd32(c + 4);
        if (memcmp(c, "00dc", 4) == 0) {
            if (video == 0 || video == 2) {
                inflate_keyframe(c + 8, sz, raw, raw_size);
                const uint8_t *px = raw + 768;
                for (unsigned y = 0; y < TEST_AVI_H; y++) {
                    for (unsigned x = 0; x < VIDEO_DISPLAY_WIDTH; x++) {
                        if (px[y * VIDEO_DISPLAY_WIDTH + x] != pattern_px(x, y / 2, video)) {
                            char msg[96];
                            g_snprintf(msg, sizeof(msg), "frame %u: AVI row %u col %u != framebuffer row %u", video, y, x,
                                       y / 2);
                            TEST_FAIL_MESSAGE(msg);
                        }
                    }
                }
                checked++;
            }
            video++;
        }
        c += 8 + sz + (sz & 1u);
    }
    g_free(raw);
    g_free(b);
    TEST_ASSERT_EQUAL_UINT(3, video);
    TEST_ASSERT_EQUAL_UINT(2, checked);

    gchar *j = cuts_json();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"version\": 4"), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"framebuffer_width\": 928, \"framebuffer_height\": 288"), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"canvas\": { \"x\": 154, \"y\": 46, \"width\": 640, \"height\": 200 }"), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"fps_num\": 50, \"fps_den\": 1"), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"width\": 928, \"height\": 576"), j);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(j, "\"line_doubled\": true"), j);
    g_free(j);
}

/* ================================================================
 * Plynulý retake zvuku (Task 14)
 * ================================================================ */

/** Cesta nepřerušené referenční nahrávky pro porovnání zvuku. */
static const char *REF_AVI = "tests/data/tmp/test_vr_snap_ref.avi";
static const char *REF_CUTS = "tests/data/tmp/test_vr_snap_ref.cuts.json";

/** Takt začátku nahrávky (konec prvního, nezapisovaného snímku); vzor zvuku je relativní k němu. */
static uint64_t s_audio_base;

/** Hodnota kanálu deterministického vzoru v relativním čase `rt` (CTC0 obdélník ~1 kHz, PSG1 ~440 Hz). */
static uint8_t audio_pattern(unsigned ch, uint64_t rt)
{
    if (ch == 0) return ((rt / 8864) & 1u) ? 0 : 15;
    return ((rt / 20160) & 1u) ? 0 : 12;
}

/**
 * Jeden snímek se zvukem: tapy všech změn vzoru v intervalu snímku, pak konec snímku.
 * Vzor závisí jen na čase relativně k s_audio_base, takže po loadu snapshotu
 * (návrat času GDG) vzniknou stejné události jako v nepřerušeném běhu.
 */
static void emu_frame_audio(bool horizon)
{
    uint64_t f0 = (uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS;
    uint64_t f1 = f0 + VIDEO_SCREEN_TICKS;
    for (unsigned ch = 0; ch < 2; ch++) {
        uint64_t half = ch ? 20160 : 8864;
        uint64_t rt0 = (f0 > s_audio_base) ? f0 - s_audio_base : 0;
        for (uint64_t rt = (rt0 + half - 1) / half * half; s_audio_base + rt < f1; rt += half)
            videorec_audio_tap(ch, audio_pattern(ch, rt), s_audio_base + rt);
    }
    emu_frame(horizon);
}

/** Start nahrávání do `path` se slyšitelnými úrovněmi kanálů 0 a 1; nastaví s_audio_base. */
static void rec_start_audio(const char *path)
{
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, path, sizeof(s.path));
    for (int v = 1; v < VIDEOREC_AUDIO_LEVELS; v++) {
        s.level[0][v] = 0.5f;
        s.level[1][v] = (float)v / 15.0f * 0.25f;
    }
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame(true);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    s_audio_base = (uint64_t)g_gdg.total_elapsed.screens * VIDEO_SCREEN_TICKS;
}

/** Levý kanál zvuku AVI (přes idx1); vrací počet vzorků, `*out` g_free. */
static size_t avi_audio_left(const char *path, int16_t **out)
{
    gchar *b = NULL;
    gsize n = 0;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(path, &b, &n, NULL), path);
    const uint8_t *u = (const uint8_t *)b;
    const uint8_t *movi = find4(u, n, "movi");
    const uint8_t *idx = find4(u, n, "idx1");
    TEST_ASSERT_NOT_NULL(movi);
    TEST_ASSERT_NOT_NULL(idx);
    uint32_t entries = rd32(idx + 4) / 16;
    size_t cap = 0, cnt = 0;
    int16_t *res = NULL;
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = idx + 8 + 16 * i;
        if (memcmp(e, "01wb", 4) != 0) continue;
        const uint8_t *c = movi + rd32(e + 8);              /* offset relativně k 'movi' */
        if (memcmp(c, "01wb", 4) != 0) c = u + rd32(e + 8); /* nebo absolutně */
        TEST_ASSERT_EQUAL_MEMORY("01wb", c, 4);
        uint32_t sz = rd32(e + 12);
        size_t ns = sz / 4;
        if (cnt + ns > cap) {
            cap = (cnt + ns) * 2;
            res = g_realloc(res, cap * sizeof(int16_t));
        }
        for (size_t k = 0; k < ns; k++) res[cnt + k] = (int16_t)(c[8 + 4 * k] | (c[8 + 4 * k + 1] << 8));
        cnt += ns;
    }
    g_free(b);
    *out = res;
    return cnt;
}

/** Nepřerušená referenční nahrávka `frames` snímků se zvukovým vzorem do REF_AVI. */
static void record_reference(int frames)
{
    g_remove(REF_AVI);
    g_remove(REF_CUTS);
    rec_start_audio(REF_AVI);
    for (int i = 0; i < frames; i++) emu_frame_audio(true);
    rec_stop();
    videorec_init();
}

/**
 * Porovná zvuk nahrávky AVI s referencí od snímku `from` dál: vrátí max. |rozdíl|
 * a do `first` rozdíl prvního vzorku snímku `from`.
 */
static int audio_diff_from(int from, int frames, int *first)
{
    int16_t *a = NULL, *r = NULL;
    size_t na = avi_audio_left(AVI, &a), nr = avi_audio_left(REF_AVI, &r);
    size_t spf = na / (size_t)frames;
    TEST_ASSERT_EQUAL_size_t(nr, na);
    TEST_ASSERT_EQUAL_size_t((size_t)frames * spf, na);
    int mx = 0;
    for (size_t i = (size_t)from * spf; i < na; i++) {
        int d = abs((int)a[i] - (int)r[i]);
        if (d > mx) mx = d;
    }
    *first = abs((int)a[from * spf] - (int)r[from * spf]);
    /* kontrola, že zvuk v referenci opravdu hraje (test neporovnává ticho) */
    int peak = 0;
    for (size_t i = (size_t)from * spf; i < nr; i++) if (abs(r[i]) > peak) peak = abs(r[i]);
    TEST_ASSERT_TRUE_MESSAGE(peak > 8000, "reference audio is silent");
    g_free(a);
    g_free(r);
    return mx;
}

/*
 * Retake bez skoku ve zvuku: 15 snímků, quicksave, 12 snímků "zahozené
 * budoucnosti", quickload, 25 snímků. Zvuk od bodu retake musí být shodný
 * s nepřerušenou nahrávkou (stav rendereru z bodu snapshotu, události
 * prvního snímku po loadu se nezahodí).
 */
static void test_retake_audio_continuity(void)
{
    record_reference(40);
    rec_start_audio(AVI);
    for (int i = 0; i < 15; i++) emu_frame_audio(true);
    snap_save(0);
    for (int i = 0; i < 12; i++) emu_frame_audio(true);
    snap_load(0);
    for (int i = 0; i < 25; i++) emu_frame_audio(true);
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(40, avi_frames());
    int first = 0;
    int mx = audio_diff_from(15, 40, &first);
    printf("retake audio: first sample diff %d, max diff from retake point %d\n", first, mx);
    TEST_ASSERT_EQUAL_INT(0, first);
    TEST_ASSERT_EQUAL_INT(0, mx);
    g_remove(REF_AVI);
    g_remove(REF_CUTS);
}

/*
 * Totéž se snímky čekajícími na zvuk při uložení i při loadu (bez horizontu):
 * stav rendereru pro bod snapshotu se zachytí až při vyrenderování snímku.
 */
static void test_retake_audio_continuity_pending(void)
{
    record_reference(40);
    rec_start_audio(AVI);
    for (int i = 0; i < 12; i++) emu_frame_audio(true);
    for (int i = 0; i < 3; i++) emu_frame_audio(false); /* snímky 12-14 čekají */
    snap_save(0);
    for (int i = 0; i < 2; i++) emu_frame_audio(false); /* load dřív, než čekající snímky dostanou zvuk */
    snap_load(0);
    for (int i = 0; i < 25; i++) emu_frame_audio(true);
    rec_stop();
    TEST_ASSERT_EQUAL_UINT32(40, avi_frames());
    int first = 0;
    int mx = audio_diff_from(15, 40, &first);
    printf("retake audio (pending): first sample diff %d, max diff from retake point %d\n", first, mx);
    TEST_ASSERT_EQUAL_INT(0, first);
    TEST_ASSERT_EQUAL_INT(0, mx);
    g_remove(REF_AVI);
    g_remove(REF_CUTS);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_state_saved_while_recording);
    RUN_TEST(test_retake_discard);
    RUN_TEST(test_retake_event_time_format);
    RUN_TEST(test_retake_pending_at_load);
    RUN_TEST(test_retake_pending_at_save);
    RUN_TEST(test_retake_repeated);
    RUN_TEST(test_retake_off_seam);
    RUN_TEST(test_foreign_snapshot_seam);
    RUN_TEST(test_other_session_seam);
    RUN_TEST(test_pause_snapshot_retake);
    RUN_TEST(test_load_during_pause);
    RUN_TEST(test_load_during_pause_then_retake);
    RUN_TEST(test_load_when_idle);
    RUN_TEST(test_retake_abandoned_branch_seam);
    RUN_TEST(test_retake_across_seam_in_lineage);
    RUN_TEST(test_save_while_load_pending_seam);
    RUN_TEST(test_save_while_load_pending_take_zero);
    RUN_TEST(test_rollover_multi_part);
    RUN_TEST(test_rollover_part_open_failure);
    RUN_TEST(test_retake_after_rollover);
    RUN_TEST(test_retake_at_part_first_then_stop);
    RUN_TEST(test_avi_line_doubled);
    RUN_TEST(test_retake_audio_continuity);
    RUN_TEST(test_retake_audio_continuity_pending);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
