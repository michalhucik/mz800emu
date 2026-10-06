/*
 * test_videorec_events.c - události nahrávání, stop v pauze emulace, výstupní
 * adresář a INI modul VIDEOREC
 *
 * Testuje rozhraní lepidla videorec.c, které konzumuje UI (Task 9):
 * - strukturované události (videorec_get_event()): start, uložení, selhání
 *   i po stopu (chyba při dobíhání writeru), šev;
 * - zpracování stopu v pauze emulace (videorec_on_emulation_paused());
 * - výchozí výstupní adresář (videorec_resolve_output_dir());
 * - INI modul VIDEOREC (videorec_config_init(), volaný z cfgmain_init());
 * - souhrnný stav pro okno dálkového ovládání (videorec_get_status()): stav,
 *   čekající start (včetně cesty), segmenty, režim retake session, velikost
 *   všech AVI partů;
 * - explicitní record-pause videorec_request_pause_set() (MCP videorec_pause).
 *
 * Emulovaný snímek se simuluje posunem g_gdg.total_elapsed.screens a ručním
 * voláním hooků (stejně jako test_videorec_snapshot.c).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/cfgmain.h"
#include "emulator/emulator.h"
#include "emulator/videorec/videorec.h"
#include "emulator/videorec/videorec_config.h"
#include "hw-generic/gdg/gdg.h"
#include "libs/cfgfile/cfgelement.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/sdlapp/sdlapp.h"

extern SdlApp *g_sdlapp;

static const char *AVI = "tests/data/tmp/test_vr_events.avi";
static const char *CUTS = "tests/data/tmp/test_vr_events.cuts.json";

void setUp(void)
{
    g_mkdir_with_parents("tests/data/tmp", 0755);
    g_remove(AVI);
    g_rmdir(CUTS);
    g_remove(CUTS);
    g_videorec_settings.retake_mode = VIDEOREC_RETAKE_DISCARD;
    g_videorec_settings.default_transition = VIDEOREC_TRANS_FADE;
    g_videorec_settings.output_dir[0] = '\0';
    videorec_init();
}

void tearDown(void)
{
    videorec_exit();
    g_remove(AVI);
    g_rmdir(CUTS);
    g_remove(CUTS);
}

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/** Jeden emulovaný snímek s horizontem zvuku. */
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

/** Start nahrávání do AVI (zpracuje se na nejbližším konci snímku). */
static void rec_start(void)
{
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, AVI, sizeof(s.path));
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/** Poslední událost (musí existovat). */
static st_VIDEOREC_EVENT last_event(void)
{
    st_VIDEOREC_EVENT e;
    uint32_t seq = videorec_get_event_seq();
    TEST_ASSERT_NOT_EQUAL_UINT32(0, seq);
    TEST_ASSERT_TRUE(videorec_get_event(seq, &e));
    TEST_ASSERT_EQUAL_UINT32(seq, e.seq);
    return e;
}

/* ================================================================
 * Události
 * ================================================================ */

/* Zpracovaný start vydá událost STARTED s cestou nahrávky. */
static void test_started_event(void)
{
    uint32_t seq = videorec_get_event_seq();
    rec_start();
    TEST_ASSERT_EQUAL_UINT32(seq + 1, videorec_get_event_seq());
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_STARTED, e.kind);
    TEST_ASSERT_EQUAL_STRING(AVI, e.path);
}

/* Po stopu a dokončení zápisu writer vydá SAVED s cestou prvního partu. */
static void test_saved_event_after_stop(void)
{
    rec_start();
    emu_frames(5);
    videorec_request_stop();
    emu_frame();
    videorec_exit(); /* join writeru = všechny zprávy zpracované */
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_SAVED, e.kind);
    TEST_ASSERT_EQUAL_STRING(AVI, e.path);
    TEST_ASSERT_EQUAL_UINT64(5, e.frame);
}

/* Chyba, která nastane až při dobíhání writeru po stopu (sidecar nelze
 * zapsat - v jeho cestě je adresář), se projeví událostí FAILED. */
static void test_failed_event_after_stop(void)
{
    rec_start();
    emu_frames(3);
    TEST_ASSERT_EQUAL_INT(0, g_mkdir_with_parents(CUTS, 0755));
    videorec_request_stop();
    emu_frame();
    videorec_exit();
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_FAILED, e.kind);
    TEST_ASSERT_EQUAL_STRING(AVI, e.path);
    TEST_ASSERT_TRUE(e.text[0] != '\0');
}

/* Šev (snapshot bez dat videorec) vydá SEAM s indexem snímku švu. */
static void test_seam_event_structured(void)
{
    rec_start();
    emu_frames(7);
    videorec_on_snapshot_loaded(NULL);
    emu_frame();
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_SEAM, e.kind);
    TEST_ASSERT_EQUAL_UINT64(7, e.frame);
    TEST_ASSERT_EQUAL_STRING("Recording seam (snapshot loaded)", videorec_get_last_event());
}

/* Neexistující a přepsané pořadové číslo nejsou k dispozici. */
static void test_event_ring_bounds(void)
{
    st_VIDEOREC_EVENT e;
    TEST_ASSERT_FALSE(videorec_get_event(0, &e));
    rec_start();
    for (int i = 0; i < VIDEOREC_EVENT_RING + 2; i++) {
        videorec_on_snapshot_loaded(NULL);
        emu_frame();
    }
    uint32_t seq = videorec_get_event_seq();
    TEST_ASSERT_TRUE(videorec_get_event(seq, &e));
    TEST_ASSERT_TRUE(videorec_get_event(seq - (VIDEOREC_EVENT_RING - 1), &e));
    TEST_ASSERT_FALSE(videorec_get_event(seq - VIDEOREC_EVENT_RING, &e));
    TEST_ASSERT_FALSE(videorec_get_event(seq + 1, &e));
}

/* ================================================================
 * Stop v pauze emulace
 * ================================================================ */

/* Stop požadovaný v pauze emulace se provede v paused smyčce, bez dalšího snímku. */
static void test_stop_while_emulation_paused(void)
{
    rec_start();
    emu_frames(4);
    videorec_request_stop();
    videorec_on_emulation_paused();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
    videorec_exit();
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_EVENT_SAVED, e.kind);
    TEST_ASSERT_EQUAL_UINT64(4, e.frame);
}

/* Bez stopu paused hook nic nemění (pauza nahrávání a markery čekají na snímek). */
static void test_paused_hook_without_stop(void)
{
    rec_start();
    emu_frames(2);
    videorec_request_pause_toggle();
    videorec_on_emulation_paused();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
}

/* ================================================================
 * Explicitní record-pause (videorec_request_pause_set(), pro MCP)
 * ================================================================ */

/* Bez nahrávání požadavek odmítne (false) a stav se nemění. */
static void test_pause_set_idle_rejected(void)
{
    TEST_ASSERT_FALSE(videorec_request_pause_set(true));
    TEST_ASSERT_FALSE(videorec_request_pause_set(false));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, videorec_get_state());
}

/* Opakované "paused=true" před koncem snímku se nevyruší (na rozdíl od toggle). */
static void test_pause_set_idempotent(void)
{
    rec_start();
    emu_frames(2);
    TEST_ASSERT_TRUE(videorec_request_pause_set(true));
    TEST_ASSERT_TRUE(videorec_request_pause_set(true));
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
    TEST_ASSERT_TRUE(videorec_request_pause_set(true)); /* už v pauze: no-op */
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
    TEST_ASSERT_TRUE(videorec_request_pause_set(false));
    TEST_ASSERT_TRUE(videorec_request_pause_set(false));
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
}

/* Explicitní hodnota přepíše čekající toggle (true po toggle -> pauza nastane). */
static void test_pause_set_overrides_pending_toggle(void)
{
    rec_start();
    videorec_request_pause_toggle();
    TEST_ASSERT_TRUE(videorec_request_pause_set(false));
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    videorec_request_pause_toggle();
    TEST_ASSERT_TRUE(videorec_request_pause_set(true));
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, videorec_get_state());
}

/* ================================================================
 * Souhrnný stav pro okno dálkového ovládání (videorec_get_status())
 * ================================================================ */

/** Velikost souboru na disku (0 když neexistuje). */
static uint64_t file_size(const char *path)
{
    GStatBuf st;
    return (g_stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;
}

/* Bez nahrávání: IDLE, nic nečeká, nulové čítače. */
static void test_status_idle(void)
{
    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, st.state);
    TEST_ASSERT_FALSE(st.start_pending);
    TEST_ASSERT_EQUAL_UINT64(0, st.frames);
    TEST_ASSERT_EQUAL_UINT(0, st.segment);
}

/* Čekající start, nahrávání, record-pause a nový segment po obnovení;
 * režim retake je kopie ze startu (pozdější změna nastavení ho nemění). */
static void test_status_follows_session(void)
{
    g_videorec_settings.retake_mode = VIDEOREC_RETAKE_OFF;
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, AVI, sizeof(s.path));
    TEST_ASSERT_TRUE(videorec_request_start(&s));

    st_VIDEOREC_STATUS st;
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, st.state);
    TEST_ASSERT_TRUE(st.start_pending);
    TEST_ASSERT_EQUAL_STRING(AVI, st.path); /* cesta známá už při čekajícím startu */

    emu_frame();
    g_videorec_settings.retake_mode = VIDEOREC_RETAKE_DISCARD;
    emu_frames(5);
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, st.state);
    TEST_ASSERT_FALSE(st.start_pending);
    TEST_ASSERT_EQUAL_UINT64(5, st.frames);
    TEST_ASSERT_EQUAL_UINT(1, st.segment);
    TEST_ASSERT_TRUE(st.segment_open);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RETAKE_OFF, st.retake_mode);
    TEST_ASSERT_EQUAL_STRING(AVI, st.path);

    videorec_request_pause_toggle();
    emu_frame();
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_PAUSED, st.state);
    TEST_ASSERT_FALSE(st.segment_open);
    TEST_ASSERT_EQUAL_UINT(1, st.segment);

    videorec_request_pause_toggle();
    emu_frames(2);
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, st.state);
    TEST_ASSERT_TRUE(st.segment_open);
    TEST_ASSERT_EQUAL_UINT(2, st.segment);

    videorec_request_stop();
    emu_frame();
    videorec_get_status(&st);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT64(0, st.frames);
}

/* Velikost souborů: writer ji průběžně zveřejňuje; po dokončení zápisu je
 * přesně součet velikostí všech partů na disku (včetně indexů). */
static void test_status_bytes_all_parts(void)
{
    const char *base = "tests/data/tmp/test_vr_status_parts";
    char *p1 = g_strdup_printf("%s.avi", base);
    char *p2 = g_strdup_printf("%s_002.avi", base);
    char *p3 = g_strdup_printf("%s_003.avi", base);
    char *cuts = g_strdup_printf("%s.cuts.json", base);
    g_remove(p1);
    g_remove(p2);
    g_remove(p3);
    g_remove(cuts);

    videorec_test_set_part_limit(60000u);
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    g_strlcpy(s.path, p1, sizeof(s.path));
    TEST_ASSERT_TRUE(videorec_request_start(&s));
    emu_frame();
    emu_frames(40);

    /* Writer je asynchronní: počkat (max. 5 s), až zveřejní nějaká data. */
    st_VIDEOREC_STATUS st;
    for (int i = 0; i < 500; i++) {
        videorec_get_status(&st);
        if (st.bytes > 0) break;
        g_usleep(10000);
    }
    TEST_ASSERT_TRUE(st.bytes > 0);
    TEST_ASSERT_TRUE(st.parts >= 1);

    videorec_request_stop();
    emu_frame();
    videorec_exit(); /* join writeru */
    videorec_test_set_part_limit(0);
    videorec_get_status(&st);
    uint64_t disk = file_size(p1) + file_size(p2) + file_size(p3);
    TEST_ASSERT_TRUE_MESSAGE(file_size(p2) > 0, "expected at least 2 AVI parts");
    TEST_ASSERT_EQUAL_UINT64(disk, st.bytes);
    TEST_ASSERT_EQUAL_UINT((file_size(p3) > 0) ? 3 : 2, st.parts);

    g_remove(p1);
    g_remove(p2);
    g_remove(p3);
    g_remove(cuts);
    g_free(p1);
    g_free(p2);
    g_free(p3);
    g_free(cuts);
}

/* ================================================================
 * Výstupní adresář
 * ================================================================ */

/* Prázdný output_dir = <home_dir>/videos (vytvoří se). */
static void test_resolve_output_dir_default(void)
{
    char dir[1024];
    TEST_ASSERT_TRUE(videorec_resolve_output_dir(dir, sizeof(dir)));
    char *expect = g_build_filename(g_sdlapp->paths->home_dir, "videos", NULL);
    TEST_ASSERT_EQUAL_STRING(expect, dir);
    TEST_ASSERT_TRUE(g_file_test(expect, G_FILE_TEST_IS_DIR));
    g_rmdir(expect);
    g_free(expect);
}

/* Nastavený output_dir se použije (a vytvoří, pokud chybí). */
static void test_resolve_output_dir_configured(void)
{
    const char *d = "tests/data/tmp/vr_out_dir";
    g_rmdir(d);
    g_strlcpy(g_videorec_settings.output_dir, d, sizeof(g_videorec_settings.output_dir));
    char dir[1024];
    TEST_ASSERT_TRUE(videorec_resolve_output_dir(dir, sizeof(dir)));
    TEST_ASSERT_EQUAL_STRING(d, dir);
    TEST_ASSERT_TRUE(g_file_test(d, G_FILE_TEST_IS_DIR));
    g_rmdir(d);
}

/* Výstupní adresář s diakritikou (cesty jsou v UTF-8): nahrávka s vygenerovaným
 * jménem vznikne v něm a soubor jde otevřít přes UTF-8 API (finální review I5;
 * na Windows dříve plain fopen interpretoval cestu podle kódové stránky). */
static void test_output_dir_with_diacritics(void)
{
    const char *d = "tests/data/tmp/vr_Nahr\xc3\xa1vky \xc4\x8d" "e\xc5\xa1tina"; /* "vr_Nahrávky čeština" */
    g_strlcpy(g_videorec_settings.output_dir, d, sizeof(g_videorec_settings.output_dir));
    st_VIDEOREC_START s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_TRUE_MESSAGE(videorec_request_start(&s), videorec_get_last_error());
    emu_frame();
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_RECORDING, videorec_get_state());
    emu_frames(3);
    videorec_request_stop();
    emu_frame();
    videorec_exit();
    st_VIDEOREC_EVENT e = last_event();
    TEST_ASSERT_EQUAL_INT_MESSAGE(VIDEOREC_EVENT_SAVED, e.kind, e.text);
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(e.path, d), e.path);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(e.path, G_FILE_TEST_IS_REGULAR), e.path);
    gchar *cuts = g_strdup_printf("%.*s.cuts.json", (int)(strlen(e.path) - 4), e.path);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(cuts, G_FILE_TEST_IS_REGULAR), cuts);
    g_remove(cuts);
    g_free(cuts);
    g_remove(e.path);
    g_rmdir(d);
    g_videorec_settings.output_dir[0] = '\0';
}

/* ================================================================
 * INI modul VIDEOREC
 * ================================================================ */

static st_CFGMODULE *vr_module(void)
{
    st_CFGMODULE *m = cfgroot_get_module_by_name(g_cfgmain, "VIDEOREC");
    TEST_ASSERT_NOT_NULL_MESSAGE(m, "INI module VIDEOREC is not registered");
    return m;
}

/* Modul má všechny klíče s výchozími hodnotami podle plánu. */
static void test_ini_defaults(void)
{
    st_CFGMODULE *m = vr_module();
    TEST_ASSERT_EQUAL_STRING("", cfgmodule_get_element_text_default_value_by_name(m, "output_dir"));
    TEST_ASSERT_EQUAL_UINT(48000, cfgmodule_get_element_unsigned_default_value_by_name(m, "audio_rate"));
    TEST_ASSERT_EQUAL_UINT(1, cfgmodule_get_element_unsigned_default_value_by_name(m, "retake_mode"));
    TEST_ASSERT_EQUAL_STRING("fade", cfgmodule_get_element_text_default_value_by_name(m, "default_transition"));
    TEST_ASSERT_EQUAL_UINT(500, cfgmodule_get_element_unsigned_default_value_by_name(m, "transition_ms"));
    TEST_ASSERT_EQUAL_UINT(250, cfgmodule_get_element_unsigned_default_value_by_name(m, "keyframe_interval"));
    /* režim podle reality (tabulka V1.3 v PLAN-V1.md) */
    TEST_ASSERT_EQUAL_STRING("emulated", cfgmodule_get_element_text_default_value_by_name(m, "timebase"));
    TEST_ASSERT_EQUAL_STRING("skip", cfgmodule_get_element_text_default_value_by_name(m, "realtime_pause"));
    TEST_ASSERT_EQUAL_UINT(3, cfgmodule_get_element_unsigned_default_value_by_name(m, "realtime_pause_cap_s"));
    TEST_ASSERT_EQUAL_STRING("as_seen", cfgmodule_get_element_text_default_value_by_name(m, "realtime_speed"));
    TEST_ASSERT_EQUAL_STRING("as_heard", cfgmodule_get_element_text_default_value_by_name(m, "realtime_turbo_audio"));
    TEST_ASSERT_EQUAL_STRING("sidecar", cfgmodule_get_element_text_default_value_by_name(m, "state_marks"));
    TEST_ASSERT_EQUAL_INT(1, cfgmodule_get_element_bool_default_value_by_name(m, "auto_markers"));
    TEST_ASSERT_EQUAL_INT(0, cfgmodule_get_element_bool_default_value_by_name(m, "record_debugger_steps"));
}

/* Názvy voleb režimu podle reality: obousměrný převod a odmítnutí neznámého názvu. */
static void test_rt_option_names_roundtrip(void)
{
    for (int i = VIDEOREC_TIMEBASE_EMULATED; i <= VIDEOREC_TIMEBASE_REALTIME; i++) {
        en_VIDEOREC_TIMEBASE v;
        TEST_ASSERT_TRUE(videorec_timebase_from_name(videorec_timebase_name((en_VIDEOREC_TIMEBASE)i), &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    for (int i = VIDEOREC_RT_PAUSE_SKIP; i <= VIDEOREC_RT_PAUSE_FREEZE_CAPPED; i++) {
        en_VIDEOREC_RT_PAUSE v;
        TEST_ASSERT_TRUE(videorec_rt_pause_from_name(videorec_rt_pause_name((en_VIDEOREC_RT_PAUSE)i), &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    for (int i = VIDEOREC_RT_SPEED_AS_SEEN; i <= VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST; i++) {
        en_VIDEOREC_RT_SPEED v;
        TEST_ASSERT_TRUE(videorec_rt_speed_from_name(videorec_rt_speed_name((en_VIDEOREC_RT_SPEED)i), &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    for (int i = VIDEOREC_TURBO_AUDIO_AS_HEARD; i <= VIDEOREC_TURBO_AUDIO_ATTENUATE; i++) {
        en_VIDEOREC_TURBO_AUDIO v;
        TEST_ASSERT_TRUE(videorec_turbo_audio_from_name(videorec_turbo_audio_name((en_VIDEOREC_TURBO_AUDIO)i), &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    for (int i = VIDEOREC_STATE_MARKS_NONE; i <= VIDEOREC_STATE_MARKS_SIDECAR; i++) {
        en_VIDEOREC_STATE_MARKS v;
        TEST_ASSERT_TRUE(videorec_state_marks_from_name(videorec_state_marks_name((en_VIDEOREC_STATE_MARKS)i), &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    TEST_ASSERT_EQUAL_STRING("realtime", videorec_timebase_name(VIDEOREC_TIMEBASE_REALTIME));
    TEST_ASSERT_EQUAL_STRING("freeze_capped", videorec_rt_pause_name(VIDEOREC_RT_PAUSE_FREEZE_CAPPED));
    TEST_ASSERT_EQUAL_STRING("emulated_when_fast", videorec_rt_speed_name(VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST));
    TEST_ASSERT_EQUAL_STRING("attenuate", videorec_turbo_audio_name(VIDEOREC_TURBO_AUDIO_ATTENUATE));
    TEST_ASSERT_EQUAL_STRING("none", videorec_state_marks_name(VIDEOREC_STATE_MARKS_NONE));
    en_VIDEOREC_TIMEBASE tb = VIDEOREC_TIMEBASE_REALTIME;
    TEST_ASSERT_FALSE(videorec_timebase_from_name("bogus", &tb));
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, tb); /* při neúspěchu beze změny */
}

/* Propagace z INI do g_videorec_settings, včetně náhrady neplatných hodnot. */
static void test_ini_propagate(void)
{
    st_CFGMODULE *m = vr_module();
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "output_dir"), "C:/videos");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "audio_rate"), 44100);
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "retake_mode"), 2);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "default_transition"), "crossfade");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "transition_ms"), 1200);
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "keyframe_interval"), 100);
    cfgroot_propagate(g_cfgmain);
    TEST_ASSERT_EQUAL_STRING("C:/videos", g_videorec_settings.output_dir);
    TEST_ASSERT_EQUAL_UINT(44100, g_videorec_settings.audio_rate);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RETAKE_SEAM, g_videorec_settings.retake_mode);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TRANS_CROSSFADE, g_videorec_settings.default_transition);
    TEST_ASSERT_EQUAL_UINT(1200, g_videorec_settings.transition_ms);
    TEST_ASSERT_EQUAL_UINT(100, g_videorec_settings.keyframe_interval);

    /* režim podle reality */
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "timebase"), "realtime");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_pause"), "freeze_capped");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "realtime_pause_cap_s"), 7);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_speed"), "emulated_when_fast");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_turbo_audio"), "attenuate");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "state_marks"), "none");
    cfgelement_set_bool_value(cfgmodule_get_element_by_name(m, "auto_markers"), 0);
    cfgelement_set_bool_value(cfgmodule_get_element_by_name(m, "record_debugger_steps"), 1);
    cfgroot_propagate(g_cfgmain);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, g_videorec_settings.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_FREEZE_CAPPED, g_videorec_settings.realtime_pause);
    TEST_ASSERT_EQUAL_UINT(7, g_videorec_settings.realtime_pause_cap_s);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST, g_videorec_settings.realtime_speed);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TURBO_AUDIO_ATTENUATE, g_videorec_settings.realtime_turbo_audio);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_MARKS_NONE, g_videorec_settings.state_marks);
    TEST_ASSERT_FALSE(g_videorec_settings.auto_markers);
    TEST_ASSERT_TRUE(g_videorec_settings.record_debugger_steps);

    /* neznámé názvy -> výchozí hodnoty */
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "timebase"), "bogus");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_pause"), "bogus");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_speed"), "bogus");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_turbo_audio"), "bogus");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "state_marks"), "bogus");
    cfgroot_propagate(g_cfgmain);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_EMULATED, g_videorec_settings.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_SKIP, g_videorec_settings.realtime_pause);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_SPEED_AS_SEEN, g_videorec_settings.realtime_speed);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TURBO_AUDIO_AS_HEARD, g_videorec_settings.realtime_turbo_audio);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_MARKS_SIDECAR, g_videorec_settings.state_marks);

    /* neplatný přechod a "none" -> fade */
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "default_transition"), "bogus");
    cfgroot_propagate(g_cfgmain);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TRANS_FADE, g_videorec_settings.default_transition);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "default_transition"), "none");
    cfgroot_propagate(g_cfgmain);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TRANS_FADE, g_videorec_settings.default_transition);

    /* zpět na výchozí, ať další testy běží s výchozím nastavením */
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "output_dir"), "");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "audio_rate"), 48000);
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "retake_mode"), 1);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "default_transition"), "fade");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "transition_ms"), 500);
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "keyframe_interval"), 250);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "timebase"), "emulated");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_pause"), "skip");
    cfgelement_set_unsigned_value(cfgmodule_get_element_by_name(m, "realtime_pause_cap_s"), 3);
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_speed"), "as_seen");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "realtime_turbo_audio"), "as_heard");
    cfgelement_set_text_value(cfgmodule_get_element_by_name(m, "state_marks"), "sidecar");
    cfgelement_set_bool_value(cfgmodule_get_element_by_name(m, "auto_markers"), 1);
    cfgelement_set_bool_value(cfgmodule_get_element_by_name(m, "record_debugger_steps"), 0);
    cfgroot_propagate(g_cfgmain);
}

/* videorec_config_init() načte hodnoty z INI souboru a propaguje je (bez
 * globálního cfgroot_propagate - ten se v emulátoru nevolá). */
static void test_ini_loaded_from_file(void)
{
    const char *ini = "tests/data/tmp/test_vr_config.ini";
    const char *content = "[VIDEOREC]\n"
                          "output_dir = D:/rec\n"
                          "audio_rate = 0xac44\n" /* UNSIGNED je v INI vždy hex (44100) */
                          "retake_mode = 0x00\n"
                          "default_transition = card\n"
                          "transition_ms = 0x64\n"
                          "keyframe_interval = 0x0a\n"
                          "timebase = realtime\n"
                          "realtime_pause = freeze\n"
                          "realtime_pause_cap_s = 0x0c\n"
                          "realtime_speed = emulated_when_fast\n"
                          "realtime_turbo_audio = silence\n"
                          "state_marks = none\n"
                          "auto_markers = 0\n"
                          "record_debugger_steps = 1\n";
    TEST_ASSERT_TRUE(g_file_set_contents(ini, content, -1, NULL));

    st_VIDEOREC_SETTINGS saved = g_videorec_settings;
    struct st_CFGROOT *saved_root = g_cfgmain;
    g_cfgmain = cfgroot_new(ini);
    videorec_config_init();
    st_VIDEOREC_SETTINGS loaded = g_videorec_settings;
    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;
    g_videorec_settings = saved;
    g_remove(ini);

    TEST_ASSERT_EQUAL_STRING("D:/rec", loaded.output_dir);
    TEST_ASSERT_EQUAL_UINT(44100, loaded.audio_rate);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RETAKE_OFF, loaded.retake_mode);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TRANS_CARD, loaded.default_transition);
    TEST_ASSERT_EQUAL_UINT(100, loaded.transition_ms);
    TEST_ASSERT_EQUAL_UINT(10, loaded.keyframe_interval);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TIMEBASE_REALTIME, loaded.timebase);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_PAUSE_FREEZE, loaded.realtime_pause);
    TEST_ASSERT_EQUAL_UINT(12, loaded.realtime_pause_cap_s);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_SPEED_EMULATED_WHEN_FAST, loaded.realtime_speed);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_TURBO_AUDIO_SILENCE, loaded.realtime_turbo_audio);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_STATE_MARKS_NONE, loaded.state_marks);
    TEST_ASSERT_FALSE(loaded.auto_markers);
    TEST_ASSERT_TRUE(loaded.record_debugger_steps);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_started_event);
    RUN_TEST(test_saved_event_after_stop);
    RUN_TEST(test_failed_event_after_stop);
    RUN_TEST(test_seam_event_structured);
    RUN_TEST(test_event_ring_bounds);
    RUN_TEST(test_stop_while_emulation_paused);
    RUN_TEST(test_paused_hook_without_stop);
    RUN_TEST(test_pause_set_idle_rejected);
    RUN_TEST(test_pause_set_idempotent);
    RUN_TEST(test_pause_set_overrides_pending_toggle);
    RUN_TEST(test_status_idle);
    RUN_TEST(test_status_follows_session);
    RUN_TEST(test_status_bytes_all_parts);
    RUN_TEST(test_resolve_output_dir_default);
    RUN_TEST(test_resolve_output_dir_configured);
    RUN_TEST(test_output_dir_with_diacritics);
    RUN_TEST(test_ini_defaults);
    RUN_TEST(test_ini_propagate);
    RUN_TEST(test_rt_option_names_roundtrip);
    RUN_TEST(test_ini_loaded_from_file);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
