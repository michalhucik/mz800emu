/**
 * @file test_replay_input.c
 * @brief Unit testy cesty vstupů záznamu (replay.c) bez GUI a bez hlavní smyčky.
 *
 * Hlavní smyčku simuluje test: pauzu nastaví přímo (`g_emulator.paused`),
 * konec snímku = zvýšení `g_gdg.total_elapsed.screens` a volání
 * replay_on_frame_boundary(), paused smyčku volání replay_on_paused().
 *
 * Pokrývá:
 *   - mimo záznam sken klávesnice hostitele (i joystick z numerické
 *     klávesnice), okno VKBD, autotype a jeho prodlevy provede až emu
 *     vlákno (replay_on_paused() / hranice snímku), autotype z vlastní kopie
 *     textu; revizi stavu zvedne jen změna; průběh autotype jde číst
 *     pio8255_autotype_progress() a replay_autotype_peek_rest(),
 *   - při záznamu čeká matice z UI ("čekající matice") a reset na hranici
 *     snímku a teprve tam se převezme a zapíše do záznamu,
 *   - uložený soubor obsahuje události ve správných snímcích a polohách,
 *   - při přehrávání se živý vstup zahodí, záznam se přehraje bez rozjetí
 *     (shodný závěrečný hash) a skončí s výsledkem `ended`,
 *   - start záznamu při běžícím autotype se odmítne,
 *   - hostitelský joystick (typ JOYSTICK) se při záznamu čte jednou za
 *     snímek (mezi hranicemi stav drží), při přehrání se živý stav ignoruje
 *     a přehrání odmítne port s jiným připojením (NONE / připojený),
 *   - RTC (Unicard) při záznamu a přehrání = základ z manifestu +
 *     emulovaný čas, při přehrání stejné hodnoty.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/replay.h"
#include "emulator/replay_format.h"
#include "emulator/state_rev.h"
#include "emulator/emulator.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/joy/joy.h"
#include "hw-generic/unicard/unicard.h"
#include "mzarch/mzarch.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;

void setUp(void)
{
    if (!s_tmpdir) s_tmpdir = g_dir_make_tmp("replay_in_XXXXXX", NULL);
    g_emulator.paused = true;
}

void tearDown(void)
{
}


/** @brief Cesta souboru v dočasném adresáři (g_free). */
static char *tmp_path(const char *name)
{
    return g_build_filename(s_tmpdir, name, NULL);
}


/** @brief Konec snímku v simulované hlavní smyčce. */
static void next_frame(void)
{
    g_gdg.total_elapsed.screens++;
    replay_on_frame_boundary();
}


/** @brief Matice se všemi klávesami uvolněnými kromě (col, value). */
static void idle_matrix(uint8_t *m, int col, uint8_t value)
{
    memset(m, 0xFF, 10);
    if (col >= 0) m[col] = value;
}


/* Mimo záznam: sken klávesnice hostitele i VKBD zapíše až emu vlákno. */
void test_off_mode_inputs_via_emu_thread(void)
{
    uint8_t m[10];
    idle_matrix(m, 3, 0xF7);
    uint64_t rev0 = g_state_rev;
    replay_host_keyboard_submit(m, -1, 0xFF);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, g_pio8255.keyboard_matrix[3], "UI vlákno matici samo nezapíše");
    TEST_ASSERT_EQUAL_UINT64(rev0, g_state_rev);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(0xF7, g_pio8255.keyboard_matrix[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[4]);
    TEST_ASSERT_TRUE_MESSAGE(g_state_rev > rev0, "změna matice zvedne revizi stavu");

    /* Stejný sken znovu: nic se nemění, revize zůstane. */
    rev0 = g_state_rev;
    replay_host_keyboard_submit(m, -1, 0xFF);
    replay_on_paused();
    TEST_ASSERT_EQUAL_UINT64(rev0, g_state_rev);

    /* Dva skeny před provedením: platí poslední, na hranici snímku. */
    idle_matrix(m, 3, 0xFE);
    replay_host_keyboard_submit(m, -1, 0xFF);
    idle_matrix(m, 8, 0xBF);
    replay_host_keyboard_submit(m, -1, 0xFF);
    next_frame();
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[3]);
    TEST_ASSERT_EQUAL_HEX8(0xBF, g_pio8255.keyboard_matrix[8]);

    /* Joystick z numerické klávesnice jde stejnou cestou. */
    uint8_t js = g_joy.dev[0].state;
    rev0 = g_state_rev;
    replay_host_keyboard_submit(m, 0, (uint8_t)(js ^ 0x01));
    TEST_ASSERT_EQUAL_HEX8(js, g_joy.dev[0].state);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(js ^ 0x01), g_joy.dev[0].state);
    TEST_ASSERT_TRUE(g_state_rev > rev0);
    replay_host_keyboard_submit(m, 0, js);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(js, g_joy.dev[0].state);

    pio8255_vkbd_matrix_reset();
    replay_host_vkbd_set(2, 1, true);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.vkbd_matrix[2]);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(0xFD, g_pio8255.vkbd_matrix[2]);
    replay_host_vkbd_set(2, 1, false);
    next_frame();
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.vkbd_matrix[2]);

    /* Stisk a uvolnění před provedením: platí poslední změna bitu. */
    replay_host_vkbd_set(5, 7, true);
    replay_host_vkbd_set(5, 7, false);
    replay_host_vkbd_set(6, 0, true);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.vkbd_matrix[5]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, g_pio8255.vkbd_matrix[6]);
    pio8255_vkbd_matrix_reset();

    TEST_ASSERT_FALSE(replay_divert_reset_request());

    idle_matrix(m, -1, 0);
    replay_host_keyboard_submit(m, -1, 0xFF);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[8]);
}


/* Záznam: vstupy z UI a reset čekají na hranici snímku. */
void test_recording_takes_inputs_at_frame_boundary(void)
{
    char *path = tmp_path("in.mzr");
    pio8255_vkbd_matrix_reset();
    pio8255_keyboard_matrix_reset();

    uint32_t seq = replay_request_record(path, 5, "unit test");
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(seq, st.request_done);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_RECORDING, st.mode);
    TEST_ASSERT_TRUE(replay_is_active());

    /* Vstup z UI se do matice nezapíše hned. */
    uint8_t m[10];
    idle_matrix(m, 4, 0x7F);
    replay_host_keyboard_submit(m, -1, 0xFF);
    replay_host_vkbd_set(1, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.vkbd_matrix[1]);
    TEST_ASSERT_TRUE(replay_divert_reset_request());

    /* Hranice snímku: reset, převzetí matice (VKBD okna se promítne do
     * keyboard_matrix), vzorek. */
    next_frame();
    TEST_ASSERT_EQUAL_HEX8(0x7F, g_pio8255.keyboard_matrix[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, g_pio8255.keyboard_matrix[1]);
    TEST_ASSERT_EQUAL_UINT32(1, replay_current_frame());

    /* V pauze na hranici snímku: konec se závěrečným hashem. */
    TEST_ASSERT_TRUE(replay_paused_at_boundary());
    seq = replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
    TEST_ASSERT_EQUAL_INT(REPLAY_OUTCOME_RECORDED, st.outcome);

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(path, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    TEST_ASSERT_EQUAL_UINT32(1, d->frames);
    TEST_ASSERT_TRUE(d->final_hash);
    TEST_ASSERT_EQUAL_UINT32(5, d->hash_interval);
    TEST_ASSERT_EQUAL_STRING("unit test", d->description);
    /* f = 0: matice + 2 joysticky; f = 1: reset, matice. */
    TEST_ASSERT_EQUAL_UINT(5, d->events->len);
    st_REPLAY_EVENT *e0 = &g_array_index(d->events, st_REPLAY_EVENT, 0);
    TEST_ASSERT_EQUAL_INT(REPLAY_EV_KBD, e0->kind);
    TEST_ASSERT_EQUAL_UINT32(0, e0->frame);
    st_REPLAY_EVENT *e3 = &g_array_index(d->events, st_REPLAY_EVENT, 3);
    TEST_ASSERT_EQUAL_INT(REPLAY_EV_RESET, e3->kind);
    TEST_ASSERT_EQUAL_UINT32(1, e3->frame);
    TEST_ASSERT_EQUAL_INT(REPLAY_AT_BOUNDARY, e3->at);
    st_REPLAY_EVENT *e4 = &g_array_index(d->events, st_REPLAY_EVENT, 4);
    TEST_ASSERT_EQUAL_INT(REPLAY_EV_KBD, e4->kind);
    TEST_ASSERT_EQUAL_HEX8(0x7F, e4->kbd[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, e4->kbd[1]);
    /* Hash f = 0 a závěrečný f = 1. */
    TEST_ASSERT_EQUAL_UINT(2, d->hashes->len);
    TEST_ASSERT_EQUAL_UINT32(1, g_array_index(d->hashes, st_REPLAY_HASH_ROW, 1).frame);
    replay_data_free(d);
    g_free(err);

    /* Po konci záznamu jde vstup zase přes emu vlákno mimo záznam. */
    idle_matrix(m, -1, 0);
    replay_host_keyboard_submit(m, -1, 0xFF);
    replay_on_paused();
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[4]);
    g_free(path);
}


/* Přehrávání: živý vstup se zahodí, záznam doběhne bez rozjetí. */
void test_playback_ignores_live_input_and_ends(void)
{
    char *path = tmp_path("in.mzr");
    uint32_t seq = replay_request_play(path, true);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(seq, st.request_done);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_PLAYING, st.mode);
    TEST_ASSERT_EQUAL_UINT32(1, st.frames);

    uint8_t m[10];
    idle_matrix(m, 6, 0x00);
    replay_host_keyboard_submit(m, -1, 0xFF);
    replay_host_vkbd_set(7, 3, true);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[6]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.vkbd_matrix[7]);

    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_EQUAL_UINT32(1, st.outcome_frame);
    TEST_ASSERT_FALSE(st.deviated);
    TEST_ASSERT_EQUAL_HEX8(0x7F, g_pio8255.keyboard_matrix[4]);
    TEST_ASSERT_EQUAL_INT(EMU_PAUSE_REASON_REPLAY_END, g_emulator.pause_reason);
    g_free(path);
}


/** @brief Živý stav podvrženého hostitelského joysticku (oba porty). */
static uint8_t s_fake_joy = 0xFF;

/** @brief Náhrada g_joy.host_scan: vrací s_fake_joy. */
static uint8_t fake_joy_scan(en_JOY_DEVID joy_devid)
{
    (void)joy_devid;
    return s_fake_joy;
}


/* Hostitelský joystick: záznam vzorkuje jednou za snímek, přehrání ignoruje
 * živý stav, jiné připojení portu přehrání odmítne. */
void test_host_joystick_sampled_per_frame(void)
{
    char *path = tmp_path("joy.mzr");
    en_JOY_TYPE saved_type = g_joy.dev[0].type;
    joy_host_read_fn saved_scan = g_joy.host_scan;
    g_joy.dev[0].type = JOY_TYPE_JOYSTICK;
    g_joy.host_scan = fake_joy_scan;
    pio8255_vkbd_matrix_reset();
    pio8255_keyboard_matrix_reset();

    /* Mimo záznam: živé čtení při každém IN. */
    s_fake_joy = 0xF0;
    TEST_ASSERT_EQUAL_HEX8(0xF0, joy_read_byte(JOY_DEVID_0));

    s_fake_joy = 0xFE;
    replay_request_record(path, 1, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_HEX8(0xFE, joy_read_byte(JOY_DEVID_0));

    /* Změna mezi hranicemi se neprojeví, až na hranici snímku. */
    s_fake_joy = 0xFB;
    TEST_ASSERT_EQUAL_HEX8(0xFE, joy_read_byte(JOY_DEVID_0));
    next_frame();
    TEST_ASSERT_EQUAL_HEX8(0xFB, joy_read_byte(JOY_DEVID_0));
    s_fake_joy = 0xEF;
    next_frame();
    next_frame();

    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    /* Po konci zase živě. */
    s_fake_joy = 0x3F;
    TEST_ASSERT_EQUAL_HEX8(0x3F, joy_read_byte(JOY_DEVID_0));

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(path, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    TEST_ASSERT_EQUAL_INT(JOY_TYPE_JOYSTICK, d->joy_type[0]);
    for (guint i = 0; i < d->warnings->len; i++)
    {
        TEST_ASSERT_NULL(strstr((const char *)g_ptr_array_index(d->warnings, i), "joystick"));
    };
    uint8_t seen[3];
    int n = 0;
    for (guint i = 0; i < d->events->len && n < 3; i++)
    {
        st_REPLAY_EVENT *e = &g_array_index(d->events, st_REPLAY_EVENT, i);
        if (e->kind == REPLAY_EV_JOY && e->joy_port == 0) seen[n++] = e->joy_state;
    };
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX8(0xFE, seen[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFB, seen[1]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, seen[2]);
    replay_data_free(d);
    g_free(err);

    /* Přehrání: živý joystick se ignoruje, záznam doběhne bez rozjetí. */
    s_fake_joy = 0x00;
    replay_request_play(path, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_HEX8(0xFE, joy_read_byte(JOY_DEVID_0));
    next_frame();
    TEST_ASSERT_EQUAL_HEX8(0xFB, joy_read_byte(JOY_DEVID_0));
    next_frame();
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_EQUAL_HEX8(0x00, joy_read_byte(JOY_DEVID_0));

    /* Port bez joysticku přehrání odmítne. */
    g_joy.dev[0].type = JOY_TYPE_NONE;
    replay_request_play(path, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_FALSE(st.request_ok);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(st.request_message, "joystick port 1"), st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);

    g_joy.dev[0].type = saved_type;
    g_joy.host_scan = saved_scan;
    g_joy.dev[0].state = 0xFF;
    g_remove(path);
    g_free(path);
}


/** @brief RTC Unicardu jako sekundy od roku 0 (jen pro rozdíly v testu). */
static gint64 rtc_seconds(void)
{
    st_UNICARD_RTC r;
    unicard_read_rtc(&r);
    GDateTime *t = g_date_time_new_utc(r.year, r.month, r.day, r.hour, r.min, r.sec);
    gint64 v = g_date_time_to_unix(t);
    g_date_time_unref(t);
    return v;
}


/* RTC: při záznamu a přehrání ze základu záznamu + emulovaného času. */
void test_rtc_follows_emulated_time(void)
{
    char *path = tmp_path("rtc.mzr");
    replay_request_record(path, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    gint64 t0 = rtc_seconds();
    for (int i = 0; i < VIDEO_SCREENS_PER_SEC; i++) next_frame();
    gint64 t1 = rtc_seconds();
    TEST_ASSERT_EQUAL_INT64(1, t1 - t0);
    /* Další sekunda, aby přehrání bylo na snímku t1 ještě aktivní. */
    for (int i = 0; i < VIDEO_SCREENS_PER_SEC; i++) next_frame();

    replay_request_stop();
    replay_on_paused();

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(path, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    TEST_ASSERT_NOT_NULL(d->rtc_base);
    TEST_ASSERT_EQUAL_size_t(19, strlen(d->rtc_base));
    replay_data_free(d);
    g_free(err);

    /* Přehrání vidí stejné hodiny, i když hostitelský čas mezitím běžel. */
    replay_request_play(path, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT64(t0, rtc_seconds());
    for (int i = 0; i < VIDEO_SCREENS_PER_SEC; i++) next_frame();
    TEST_ASSERT_EQUAL_INT64(t1, rtc_seconds());
    for (int i = 0; i < VIDEO_SCREENS_PER_SEC; i++) next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    g_remove(path);
    g_free(path);
}


/* Mimo záznam: autotype a prodlevy spustí emu vlákno z vlastní kopie textu. */
void test_off_mode_autotype_via_emu_thread(void)
{
    char text[8] = "AB";
    int gen0, gen, done_gen, typed;
    pio8255_autotype_progress(&gen0, NULL, NULL);
    pio8255_autotype_set_key_down_ms(20);
    pio8255_autotype_set_key_up_ms(20);

    replay_host_autotype(text, 33.0, 44.0);
    TEST_ASSERT_NULL(g_pio8255.vkbd_autotype);
    uint64_t rev0 = g_state_rev;
    replay_on_paused();
    TEST_ASSERT_TRUE_MESSAGE(g_state_rev > rev0, "autotype z UI mimo záznam zvedne revizi stavu");
    rev0 = g_state_rev;
    replay_on_paused();
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(rev0, g_state_rev, "pauza bez vstupu z UI revizi nezvedne");
    TEST_ASSERT_NOT_NULL(g_pio8255.vkbd_autotype);
    TEST_ASSERT_TRUE(g_pio8255.vkbd_autotype != text);
    TEST_ASSERT_EQUAL_DOUBLE(33.0, g_pio8255.vkbd_autotype_kd_ms);
    TEST_ASSERT_EQUAL_DOUBLE(44.0, g_pio8255.vkbd_autotype_ku_ms);
    pio8255_autotype_progress(&gen, &done_gen, &typed);
    TEST_ASSERT_EQUAL_INT(gen0 + 1, gen);
    TEST_ASSERT_NOT_EQUAL(gen, done_gen);
    TEST_ASSERT_EQUAL_INT(0, typed);

    /* Buffer volajícího se smí hned změnit. */
    g_strlcpy(text, "XY", sizeof(text));
    char rest[16];
    TEST_ASSERT_TRUE(replay_autotype_peek_rest(rest, sizeof(rest)));
    TEST_ASSERT_EQUAL_STRING("AB", rest);

    /* Prodlevy za běhu nastaví až emu vlákno. */
    replay_host_autotype_delays(55.0, 66.0);
    TEST_ASSERT_EQUAL_DOUBLE(33.0, g_pio8255.vkbd_autotype_kd_ms);
    next_frame();
    TEST_ASSERT_EQUAL_DOUBLE(55.0, g_pio8255.vkbd_autotype_kd_ms);
    TEST_ASSERT_EQUAL_DOUBLE(66.0, g_pio8255.vkbd_autotype_ku_ms);

    /* Zastavení: generace dokončená, zbytek prázdný. */
    replay_host_autotype(NULL, 20.0, 20.0);
    replay_on_paused();
    TEST_ASSERT_NULL(g_pio8255.vkbd_autotype);
    pio8255_autotype_progress(&gen, &done_gen, NULL);
    TEST_ASSERT_EQUAL_INT(gen, done_gen);
    TEST_ASSERT_FALSE(replay_autotype_peek_rest(rest, sizeof(rest)));
    TEST_ASSERT_EQUAL_STRING("", rest);

    pio8255_autotype_set_key_down_ms(20);
    pio8255_autotype_set_key_up_ms(20);
}


/* Start záznamu při běžícím autotype se odmítne. */
void test_record_refused_while_autotype_runs(void)
{
    static char text[] = "A";
    char *path = tmp_path("no.mzr");
    pio8255_set_autotype(text);
    uint32_t seq = replay_request_record(path, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(seq, st.request_done);
    TEST_ASSERT_FALSE(st.request_ok);
    TEST_ASSERT_NOT_NULL(strstr(st.request_message, "autotype"));
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_OFF, st.mode);
    TEST_ASSERT_FALSE(g_file_test(path, G_FILE_TEST_EXISTS));
    pio8255_set_autotype(NULL);
    g_free(path);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    replay_init();

    UNITY_BEGIN();
    RUN_TEST(test_off_mode_inputs_via_emu_thread);
    RUN_TEST(test_off_mode_autotype_via_emu_thread);
    RUN_TEST(test_recording_takes_inputs_at_frame_boundary);
    RUN_TEST(test_playback_ignores_live_input_and_ends);
    RUN_TEST(test_record_refused_while_autotype_runs);
    RUN_TEST(test_host_joystick_sampled_per_frame);
    RUN_TEST(test_rtc_follows_emulated_time);
    int result = UNITY_END();

    char *p = tmp_path("in.mzr");
    g_remove(p);
    g_free(p);
    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
