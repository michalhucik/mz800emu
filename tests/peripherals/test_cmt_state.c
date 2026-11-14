/**
 * @file test_cmt_state.c
 * @brief Regresní testy stavu transportu virtuálního kazetového magnetofonu (CMT).
 *
 * Testuje invariant st_CMT "bez vložené pásky je transport ve STOP" a jeho
 * udržení při načtení snapshotu:
 *   - cmt_update_output / cmt_read_data / cmt_write_data bez pásky nepadají
 *     ani při porušeném invariantu (dříve NULL dereference g_cmt.ext),
 *   - snapshot pořízený během přehrávání načtený bez vložené pásky přepne
 *     transport do STOP,
 *   - snapshot načtený s vloženou páskou zachová PLAY i playsts,
 *   - cmt_stop / cmt_eject bez pásky uvedou transport do STOP,
 *   - po vysunutí "visícího" stavu ze snapshotu přehraje nová páska
 *     (cmt_open + cmt_play) signál od začátku,
 *   - stop + play přehraje WAV pásku znovu od začátku (dokumentace MCP
 *     to doporučuje místo seeku na SINGLE pásce),
 *   - cpu_boost: snapshot s hrající páskou zapne MAX SPEED, snapshot ve
 *     STOP vypne MAX SPEED zapnutou boostem, MAX SPEED zvolenou uživatelem
 *     CMT nemění (snapshot, play/stop, pauza); snapshot neobnovuje volbu
 *     cpu_boost (uživatelská preference), MAX SPEED po načtení odpovídá
 *     aktuální volbě uživatele,
 *   - pauza uživatele a motor (hrana PC3) jsou nezávislé zdroje zastavení
 *     pásky, bez pauzy uživatele se hrana PC3 chová jako dřív; snapshot
 *     obojí obnoví,
 *   - při nahrávání jádro odmítne krok na sousední blok i výběr bloku
 *     (cmt_tape_step_block(), cmt_tape_select_block()) jako seek,
 *   - per-blok rychlost pásky (cmt_tape_set_block_cmt_speed, backing MCP
 *     cmt_tape_block_speed) se projeví při otevření bloku (dříve se
 *     ignorovala, protože blok nebyl přepnut na CMTEXT_BLOCK_SPEED_SET).
 *
 * Testovací MZF se generuje do dočasného adresáře (g_get_tmp_dir), žádná
 * cizí data se nepoužívají.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/emulator.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/machine_hash.h"
#include "emulator/audio.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/pio8255/pio8255.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Cesta k dočasnému testovacímu MZF (vytváří setUp, maže tearDown). */
static char *s_mzf_path = NULL;

/** @brief Hodnota cpu_boost před prvním testem (obnovuje ji tearDown). */
static en_CMT_CPU_BOOST s_initial_cpu_boost = CMT_CPU_BOOST_DISABLED;

/** @brief Stav ROM patche CMT hacku před testem motoru (obnoví ho hack_restore() nebo tearDown). */
static unsigned s_saved_hack = 0;

/** @brief hack_off() patch vypnul a ještě ho nikdo nevrátil (tearDown ho vrátí i po selhání asertace). */
static bool s_hack_saved = false;

/** @brief Délka těla testovacího MZF v bajtech (páska trvá několik sekund). */
#define TEST_MZF_BODY_SIZE 2048

/**
 * @brief Vytvoří testovací MZF (atribut 01h, tělo TEST_MZF_BODY_SIZE bajtů).
 *
 * @param path Cílová cesta.
 * @return true při úspěchu zápisu.
 */
static bool write_test_mzf(const char *path)
{
    uint8_t hdr[128];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x01;
    const char name[] = "CMT STATE TEST\r";
    memcpy(&hdr[1], name, sizeof(name) - 1);
    hdr[0x12] = (uint8_t)(TEST_MZF_BODY_SIZE & 0xFF);
    hdr[0x13] = (uint8_t)(TEST_MZF_BODY_SIZE >> 8);
    hdr[0x14] = 0x00; /* fstrt = 1200h */
    hdr[0x15] = 0x12;
    hdr[0x16] = 0x00; /* fexec = 1200h */
    hdr[0x17] = 0x12;

    FILE *fh = fopen(path, "wb");
    if (!fh) return false;
    bool ok = (fwrite(hdr, 1, sizeof(hdr), fh) == sizeof(hdr));
    for (int i = 0; ok && (i < TEST_MZF_BODY_SIZE); i++) {
        ok = (fputc((i * 37 + 11) & 0xFF, fh) != EOF);
    }
    if (fclose(fh) != 0) ok = false;
    return ok;
}

void setUp(void)
{
    if (!s_mzf_path) {
        gchar *name = g_strdup_printf("mztest_cmt_state_%u.mzf", (unsigned)g_random_int());
        s_mzf_path = g_build_filename(g_get_tmp_dir(), name, NULL);
        g_free(name);
    }
    TEST_ASSERT_TRUE_MESSAGE(write_test_mzf(s_mzf_path), "cannot write test MZF");
}

void tearDown(void)
{
    /* Každý test končí bez pásky ve STOP (cmt_eject nad vloženou páskou). */
    cmt_eject();
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.user_paused = 0;
    g_cmt.motor_off = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_emulator.paused = false;
    /* patch CMT hacku vypnutý testem motoru zpět (Unity při selhání přeskočí hack_restore()) */
    if (s_hack_saved)
    {
        g_cmthack.load_patch_installed = s_saved_hack;
        s_hack_saved = false;
    }
    /* MAX SPEED a cpu_boost zpět do výchozího stavu (testy boostu je mění). */
    g_cmt.cpu_boost = s_initial_cpu_boost;
    g_cmt.echo = 0;
    emulator_max_speed(false);
    if (s_mzf_path) g_remove(s_mzf_path);
}

/**
 * @brief Posune GDG čas o zadaný počet celých snímků.
 *
 * @param screens Počet snímků.
 */
static void advance_screens(unsigned screens)
{
    g_gdg.total_elapsed.screens += screens;
}

/**
 * @brief Otevře testovací MZF a spustí přehrávání (bez pauzy).
 */
static void open_and_play(void)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_mzf_path));
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* Porušený invariant (PLAY/RECORD bez pásky) nesmí vést k NULL dereferenci. */
void test_cmt_no_tape_play_state_does_not_crash(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_cmt.output = 1;

    cmt_update_output();
    TEST_ASSERT_EQUAL_INT(1, cmt_read_data());

    g_cmt.state = CMT_STATE_RECORD;
    cmt_write_data(1);

    g_cmt.state = CMT_STATE_STOP;
}

/* Snapshot z přehrávání načtený bez pásky -> transport STOP (dříve pád). */
void test_cmt_snapshot_play_loaded_without_tape_is_stopped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    open_and_play();
    advance_screens(25);
    cmt_update_output();
    /* Snapshot "uprostřed" pauzy motoru: paused_time nese pozici pásky. */
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);

    g_emulator.paused = true;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmt play", &buf, &buf_size));

    /* Nový proces nemá vloženou pásku a soubor pásky ze snapshotu chybí
     * (jinak by ho load otevřel). */
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    g_remove(s_mzf_path);

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, buf_size));
    g_free(buf);

    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_STOP, g_cmt.playsts);
    TEST_ASSERT_EQUAL_UINT64(0, g_cmt.paused_time);

    /* Volání z mzarch při opuštění pauzy i čtení PC5 nesmí padnout. */
    cmt_update_output();
    TEST_ASSERT_EQUAL_INT(0, cmt_read_data());
}

/* Snapshot načtený s vloženou páskou zachová PLAY a obnoví playsts. */
void test_cmt_snapshot_with_tape_keeps_play_and_playsts(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    open_and_play();
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_BODY, g_cmt.playsts);

    g_emulator.paused = true;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmt play", &buf, &buf_size));

    /* Stop nastaví playsts = STOP; load ho musí vrátit na BODY. */
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_STOP, g_cmt.playsts);

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, buf_size));
    g_free(buf);

    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_BODY, g_cmt.playsts);
}

/* cmt_sanitize_state: STOP bez pásky se srovná, aktivní stav bez pásky hlásí reset. */
void test_cmt_sanitize_state(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.paused = 1;
    g_cmt.paused_time = 1000;
    TEST_ASSERT_TRUE(cmt_sanitize_state());
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    TEST_ASSERT_EQUAL_UINT64(0, g_cmt.paused_time);

    /* Už klidový stav -> bez resetu transportu. */
    TEST_ASSERT_FALSE(cmt_sanitize_state());

    /* S vloženou páskou zůstává PLAY. */
    open_and_play();
    TEST_ASSERT_FALSE(cmt_sanitize_state());
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
}

/* cmt_stop bez pásky přepne stav PLAY (např. ze starého snapshotu) do STOP. */
void test_cmt_stop_without_tape_stops(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.paused = 1;
    g_cmt.paused_time = 5000;

    cmt_stop();

    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_STOP, g_cmt.playsts);
    TEST_ASSERT_EQUAL_UINT64(0, g_cmt.paused_time);
}

/*
 * Scénář "zamrzlé CPU": stav PLAY + pauza s pozicí za koncem pásky bez
 * vložené pásky (tak ho dřív zanechal snapshot). cmt_open (eject + open)
 * + cmt_play musí začít přehrávat novou pásku od začátku a signál se musí
 * měnit (hrany přicházejí).
 */
void test_cmt_open_after_stale_play_starts_from_beginning(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    g_cmt.state = CMT_STATE_PLAY;
    g_cmt.paused = 1;
    /* 36,19 s jako v hlášení: 641374545 GDG tiků. */
    g_cmt.paused_time = 641374545ULL;
    g_cmt.start_time = 127090360ULL;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;

    open_and_play();

    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_PLAYSTS_BODY, g_cmt.playsts);
    TEST_ASSERT_EQUAL_UINT64(gdg_get_total_ticks(), g_cmt.start_time);
    TEST_ASSERT_TRUE(cmt_get_playtime() < 0.001);

    /* Motor programem vypnut a zapnut (cmt_pause 1/0) - pozice se drží. */
    cmt_pause(1);
    cmt_pause(0);
    TEST_ASSERT_TRUE(cmt_get_playtime() < 0.001);

    /* Během první sekundy musí signál z pásky aspoň několikrát změnit úroveň. */
    int edges = 0;
    int prev = cmt_read_data();
    for (int i = 0; i < 2000; i++) {
        g_gdg.total_elapsed.ticks += 500;
        int cur = cmt_read_data();
        if (cur != prev) edges++;
        prev = cur;
    }
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_GREATER_THAN_INT(10, edges);
}

/**
 * @brief Počet událostí kanálu CTC0 aktuálního zvukového logu.
 * @return Počet událostí (0 bez logu).
 */
static unsigned echo_log_events(void)
{
    return g_audio.log ? g_audio.log->src[0]->samples_count : 0;
}

/*
 * Echo (g_cmt.echo): signál čtený programem je stejný jako bez echa, hrany
 * pásky jdou do zvukového logu i bez čtení PC5 (jen cmt_echo_frame()) a po
 * STOP se úroveň echa vrátí do klidu. Bez echa zvukový log hrany nedostane.
 */
void test_cmt_echo_reports_edges_and_keeps_output(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (!g_audio.log) TEST_IGNORE_MESSAGE("no audio log in this build");
    enum { STEPS = 400, STEP_TICKS = 2500 };
    static int ref[STEPS];

    /* bez echa: referenční průběh, zvukový log beze změny */
    g_cmt.echo = 0;
    cmt_eject();
    open_and_play();
    unsigned ev0 = echo_log_events();
    int edges = 0;
    for (int i = 0; i < STEPS; i++) {
        g_gdg.total_elapsed.ticks += STEP_TICKS;
        cmt_on_frame_audio_event();
        ref[i] = cmt_read_data();
        if (i && ref[i] != ref[i - 1]) edges++;
    }
    TEST_ASSERT_GREATER_THAN_INT(10, edges);
    TEST_ASSERT_EQUAL_UINT(ev0, echo_log_events());
    TEST_ASSERT_EQUAL_UINT(0, g_audio.cmt_echo);

    /* s echem: stejný průběh, hrany v logu */
    cmt_eject();
    g_cmt.echo = 1;
    open_and_play();
    ev0 = echo_log_events();
    for (int i = 0; i < STEPS; i++) {
        g_gdg.total_elapsed.ticks += STEP_TICKS;
        cmt_on_frame_audio_event();
        TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i], cmt_read_data(), "echo must not change the tape signal");
    }
    unsigned ev_polled = echo_log_events() - ev0;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT((unsigned)edges, ev_polled);

    /* s echem bez čtení PC5: hrany doručí jen cmt_echo_frame() */
    cmt_eject();
    cmt_on_frame_audio_event(); /* STOP: úroveň echa do klidu jako na začátku */
    TEST_ASSERT_EQUAL_UINT(0, g_audio.cmt_echo);
    open_and_play();
    ev0 = echo_log_events();
    g_gdg.total_elapsed.ticks += (uint64_t)STEPS * STEP_TICKS;
    cmt_on_frame_audio_event();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT((unsigned)edges, echo_log_events() - ev0);
    /* pozice streamu je tam, kde by byla i bez echa */
    TEST_ASSERT_EQUAL_INT(ref[STEPS - 1], cmt_read_data());

    /* STOP: úroveň echa do klidu */
    cmt_stop();
    cmt_on_frame_audio_event();
    TEST_ASSERT_EQUAL_UINT(0, g_audio.cmt_echo);
    g_cmt.echo = 0;
}

/* cmt_play_paused bez pásky nesmí nechat STOP + paused (invariant st_CMT). */
void test_cmt_play_paused_without_tape_keeps_invariant(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    cmt_play_paused();
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);

    /* S páskou: start do PLAY v pauze. */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_mzf_path));
    cmt_play_paused();
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.paused);

    /* Už hrající transport: play_paused je no-op, pauza se nevnutí. */
    cmt_pause(0);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    cmt_play_paused();
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
}

/**
 * @brief Uloží aktuální stav emulátoru do paměťového snapshotu.
 *
 * @param[out] buf      Alokovaný buffer (uvolní volající přes g_free).
 * @param[out] buf_size Velikost bufferu.
 */
static void save_snapshot(uint8_t **buf, size_t *buf_size)
{
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmt boost", buf, buf_size));
}

/**
 * @brief Načte paměťový snapshot a uvolní jeho buffer.
 *
 * @param buf      Buffer ze save_snapshot (po návratu uvolněn).
 * @param buf_size Velikost bufferu.
 */
static void load_snapshot(uint8_t *buf, size_t buf_size)
{
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, buf_size));
    g_free(buf);
}

/*
 * Snapshot z přehrávání s cpu_boost načtený do procesu, kde MAX SPEED neběží:
 * páska po načtení hraje, MAX SPEED musí běžet (dříve se cpu_boost jen
 * obnovil, rychlost zůstala normální).
 */
void test_cmt_snapshot_play_with_cpu_boost_turns_max_speed_on(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    open_and_play();
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    TEST_ASSERT_TRUE(g_emulator.max_speed_boost);

    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);

    cmt_stop();
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);

    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    TEST_ASSERT_TRUE(g_emulator.max_speed_boost);
}

/*
 * Snapshot ve STOP načtený během přehrávání s cpu_boost: MAX SPEED zapnutá
 * boostem musí zhasnout (dříve zůstala viset, ačkoli páska stojí).
 */
void test_cmt_snapshot_stop_turns_boost_max_speed_off(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);

    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);

    open_and_play();
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);

    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
    TEST_ASSERT_FALSE(g_emulator.max_speed_boost);
}

/*
 * Snapshot z přehrávání načtený bez pásky (-> STOP) s MAX SPEED zvolenou
 * uživatelem: rychlost se nemění. Uživatelská MAX SPEED přežije i play/stop
 * s cpu_boost a pauzu pásky s vypnutým cpu_boost (dříve ji obojí vypnulo).
 */
void test_cmt_user_max_speed_is_kept(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    open_and_play();
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);
    cmt_eject();
    g_remove(s_mzf_path); /* soubor ze snapshotu chybí -> load skončí ve STOP bez pásky */

    emulator_max_speed(true); /* uživatel */
    TEST_ASSERT_FALSE(g_emulator.max_speed_boost);
    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);

    TEST_ASSERT_TRUE(write_test_mzf(s_mzf_path));
    open_and_play();
    cmt_stop();
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);

    cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED);
    open_and_play();
    cmt_pause(1);
    cmt_pause(0);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
}

/*
 * Pauza a obnovení pásky s cpu_boost: MAX SPEED od boostu v pauze zhasne
 * a po obnovení se vrátí; vypnutí volby za běhu ji vypne.
 */
void test_cmt_cpu_boost_follows_pause_and_option(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    open_and_play();
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    cmt_pause(1);
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
    cmt_pause(0);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED);
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    cmt_stop();
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
}

/*
 * Snapshot uložený s cpu_boost zapnutým, načtený, když má uživatel cpu_boost
 * vypnutý: volba zůstane vypnutá (snapshot ji neobnovuje - dříve ji přepsal
 * a cfg element CMT/cpu_boost, který ukazuje na g_cmt.cpu_boost, ji pak
 * uložil do INI). Páska ze snapshotu hraje, ale MAX SPEED neběží.
 */
void test_cmt_snapshot_does_not_restore_cpu_boost_on(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED);
    open_and_play();
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);

    cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED); /* uživatel volbu vypne */
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);

    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_DISABLED, g_cmt.cpu_boost);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
    TEST_ASSERT_FALSE(g_emulator.max_speed_boost);
}

/*
 * Opačný směr: snapshot s cpu_boost vypnutým, načtený, když má uživatel
 * cpu_boost zapnutý. Volba zůstane zapnutá a hrající páska ze snapshotu
 * podle ní zapne MAX SPEED (cmt_cpu_boost_apply po načtení).
 */
void test_cmt_snapshot_does_not_restore_cpu_boost_off(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_cpu_boost_set(CMT_CPU_BOOST_DISABLED);
    open_and_play();
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED);
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);

    cmt_stop();
    cmt_cpu_boost_set(CMT_CPU_BOOST_ENABLED); /* uživatel volbu zapne */
    TEST_ASSERT_FALSE(EMULATOR_TEST_MAX_SPEED); /* páska stojí */

    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_CPU_BOOST_ENABLED, g_cmt.cpu_boost);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);
    TEST_ASSERT_TRUE(g_emulator.max_speed_boost);
}

/**
 * @brief Vytvoří PCM WAV (8 bit, mono, 44100 Hz, 0,5 s) s obdélníkem proměnné periody.
 *
 * @param path Cílová cesta.
 * @return true při úspěchu zápisu.
 */
static bool write_test_wav(const char *path)
{
    const uint32_t rate = 44100;
    const uint32_t samples = rate / 2;
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(&hdr[0], "RIFF", 4);
    uint32_t v = 36 + samples;
    memcpy(&hdr[4], &v, 4);
    memcpy(&hdr[8], "WAVEfmt ", 8);
    v = 16; memcpy(&hdr[16], &v, 4);
    uint16_t w = 1; memcpy(&hdr[20], &w, 2);   /* PCM */
    w = 1; memcpy(&hdr[22], &w, 2);            /* mono */
    memcpy(&hdr[24], &rate, 4);
    v = rate; memcpy(&hdr[28], &v, 4);         /* byte rate */
    w = 1; memcpy(&hdr[32], &w, 2);            /* block align */
    w = 8; memcpy(&hdr[34], &w, 2);            /* bits */
    memcpy(&hdr[36], "data", 4);
    memcpy(&hdr[40], &samples, 4);

    FILE *fh = fopen(path, "wb");
    if (!fh) return false;
    bool ok = (fwrite(hdr, 1, sizeof(hdr), fh) == sizeof(hdr));
    uint32_t period = 20, pos = 0;
    int level = 0;
    for (uint32_t i = 0; ok && (i < samples); i++) {
        if (++pos >= period) {
            pos = 0;
            level = !level;
            period = 20 + (i % 37);
        }
        ok = (fputc(level ? 0xE0 : 0x20, fh) != EOF);
    }
    if (fclose(fh) != 0) ok = false;
    return ok;
}

/** @brief Počet vzorků výstupu v test_cmt_wav_stop_play_restarts_from_beginning. */
#define WAV_SAMPLES 300

/**
 * @brief Navzorkuje výstup pásky v pevných krocích GDG času od aktuálního okamžiku.
 *
 * @param out Výstupní pole WAV_SAMPLES hodnot.
 */
static void sample_output(int *out)
{
    for (int i = 0; i < WAV_SAMPLES; i++) {
        g_gdg.total_elapsed.ticks += 997;
        out[i] = cmt_read_data();
    }
}

/* stop + play přehraje WAV (páska s jedním blokem, bez seeku) znovu od začátku. */
void test_cmt_wav_stop_play_restarts_from_beginning(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    gchar *wav = g_strconcat(s_mzf_path, ".wav", NULL);
    TEST_ASSERT_TRUE_MESSAGE(write_test_wav(wav), "cannot write test WAV");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(wav));

    static int first[WAV_SAMPLES], second[WAV_SAMPLES];
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    sample_output(first);

    cmt_stop();
    g_gdg.total_elapsed.screens += 7; /* mezi tím uběhne čas */
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    sample_output(second);

    int changes = 0;
    for (int i = 1; i < WAV_SAMPLES; i++) {
        if (first[i] != first[i - 1]) changes++;
    }
    TEST_ASSERT_GREATER_THAN_INT(10, changes);
    TEST_ASSERT_EQUAL_INT_ARRAY(first, second, WAV_SAMPLES);

    cmt_eject();
    g_remove(wav);
    g_free(wav);
}

/* === MAIN === */

/**
 * @brief Zapíše dvoublokovou MZT pásku (dvakrát testovací MZF za sebou).
 *
 * @param mzt_path Cílová cesta MZT.
 * @return true při úspěchu.
 */
static bool write_test_mzt(const char *mzt_path)
{
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(s_mzf_path, &data, &len, NULL)) return false;
    FILE *fh = fopen(mzt_path, "wb");
    if (!fh) { g_free(data); return false; }
    bool ok = (fwrite(data, 1, len, fh) == len) && (fwrite(data, 1, len, fh) == len);
    if (fclose(fh) != 0) ok = false;
    g_free(data);
    return ok;
}

/*
 * Per-blok rychlost (MCP cmt_tape_block_speed): blok s nastavenou rychlostí
 * se otevře v ní, i když globální rychlost je 1:1. Dříve se nastavila jen
 * cmtspeed bez blspeed = SET a otevření bloku MZT vzalo globální 1:1.
 */
void test_cmt_tape_block_speed_applies_on_block_open(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    gchar *mzt = g_strconcat(s_mzf_path, ".mzt", NULL);
    TEST_ASSERT_TRUE_MESSAGE(write_test_mzt(mzt), "cannot write test MZT");

    en_CMTSPEED saved_speed = g_cmt.mz_cmtspeed;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzt));
    st_CMTEXT_CONTAINER *container = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_NOT_NULL(container);
    TEST_ASSERT_EQUAL_INT(2, cmtext_container_get_count_blocks(container));

    /* chybové případy nic nemění */
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_cmt_speed(2, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_cmt_speed(-1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(-2, cmt_tape_set_block_cmt_speed(1, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(container, 1));

    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(1, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(container, 1));

    /* přechod na blok 1 jako při přehrávání: blok se otevře v 3:1 = 3600 Bd */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, g_cmt.ext->container->cb_next_block());
    TEST_ASSERT_EQUAL_INT(1, cmtext_block_get_block_id(g_cmt.ext->block));
    TEST_ASSERT_EQUAL_UINT16(3600, g_cmt.ext->block->cb_get_bdspeed(g_cmt.ext));

    cmt_eject();
    g_cmt.mz_cmtspeed = saved_speed;
    g_remove(mzt);
    g_free(mzt);
}

/**
 * @brief Uloží TMZ s jedním blokem 0x46 profilu ROM-800-NOMINAL (MZF z testu).
 *
 * @param path Cílová cesta (.tmz).
 */
static void write_rom_profile_tmz(const char *path)
{
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = 0x01;
    memset(&hdr.fname, 0x0D, sizeof(hdr.fname));
    memcpy(&hdr.fname, "ROM PROFILE", 11);
    hdr.fsize = 16;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[16];
    memset(body, 0x5A, sizeof(body));
    en_TMZ_PROFILE_ERROR perr = TMZ_PROFILE_OK;
    st_TZX_BLOCK *b = tmz_profile_build_block("ROM-800-NOMINAL", NULL, &hdr, body, sizeof(body), &perr);
    TEST_ASSERT_NOT_NULL(b);

    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);

    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
}

/*
 * Per-blok rychlost: bez pásky se odmítne. Samostatné MZF je od přechodu na
 * TMZ (rozšíření cmt_tmz) páska SIMPLE_TAPE s jedním blokem,
 * takže rychlost jeho bloku 0 nastavit jde (dříve SINGLE bez indexu: -1).
 * Blok, jehož rychlost dává soubor (TMZ s blokem ROM-800-NOMINAL, blspeed
 * NONE), se odmítne hodnotou -1.
 */
void test_cmt_tape_block_speed_single_mzf_and_file_speed(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1)); /* bez pásky */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_mzf_path));
    TEST_ASSERT_EQUAL_INT(0, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    cmt_eject();

    gchar *tmz = g_strconcat(s_mzf_path, ".tmz", NULL);
    write_rom_profile_tmz(tmz);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(tmz));
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, 0));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_NONE, cmtext_container_get_block_speed(c, 0));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_cmt_speed(0, CMTSPEED_3_1));
    st_MZTAPE_PULSES_LENGTH p;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, mztape_pulses_set_us(&p, 470, 494, 240, 278));
    TEST_ASSERT_EQUAL_INT(-1, cmt_tape_set_block_pulses(0, &p));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_change_block_speed(0, CMTSPEED_3_1, true, NULL));
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_NONE, cmtext_container_get_block_speed(c, 0));
    cmt_eject();
    g_remove(tmz);
    g_free(tmz);
}

/**
 * @brief Jedna náběžná hrana PC3 (BSR přes řídicí registr 8255: PC3 = 0, pak PC3 = 1).
 *
 * Hodnoty BSR 06h / 07h podle báze reference/agent/hw/05-pio-8255.md
 * (bit 7 = 0, bity 3-1 = číslo bitu, bit 0 = hodnota).
 */
static void motor_edge(void)
{
    pio8255_write(3, 0x06);
    pio8255_write(3, 0x07);
}

/**
 * @brief Vypne ROM patch CMT hacku (hrana PC3 pak řídí transport, ne PC4 hacku).
 *
 * Testovací prostředí může mít hack zapnutý podle výchozí konfigurace.
 */
static void hack_off(void)
{
    if (!s_hack_saved)
        s_saved_hack = g_cmthack.load_patch_installed;
    s_hack_saved = true;
    g_cmthack.load_patch_installed = 0;
}

/** @brief Vrátí stav ROM patche CMT hacku uložený hack_off(). */
static void hack_restore(void)
{
    if (s_hack_saved)
        g_cmthack.load_patch_installed = s_saved_hack;
    s_hack_saved = false;
}

/**
 * @brief Stav motoru podle cmt_motor_get_state(); test selže, není-li znám.
 * @return true = motor běží.
 */
static bool motor_on(void)
{
    bool known = false, on = false;
    cmt_motor_get_state(&known, &on);
    TEST_ASSERT_TRUE_MESSAGE(known, "motor state must be known in PLAY / RECORD");
    return on;
}

/*
 * Pauza uživatele a motor jsou dva nezávislé zdroje (D-G2-3): bez pauzy
 * uživatele hrana PC3 přepíná pohyb pásky přesně jako dřív; s pauzou
 * uživatele přepíná jen motor a páska stojí; pokračování pauzu zruší
 * a motor nechá, jak ho nastavil program; bez
 * pauzy uživatele pokračování motor rozběhne (dosavadní význam). Pauza ve
 * STOP = PLAY čekající na motor.
 */
void test_cmt_user_pause_and_motor_are_independent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    open_and_play();
    TEST_ASSERT_TRUE(motor_on());
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);

    /* bez pauzy uživatele: hrana = zastavení / rozběh pásky (dřívější chování) */
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(motor_on());
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());

    /* pauza uživatele: motor běží dál, páska stojí */
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    TEST_ASSERT_TRUE(motor_on());

    /* hrany PC3 při pauze uživatele přepínají jen motor */
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE(motor_on());

    /* pokračování s pauzou uživatele: zruší jen ji, motor stojí dál, páska čeká na motor */
    cmt_pause(0);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(motor_on());

    /* pauza uživatele s běžícím motorem a pokračování: páska jede */
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    cmt_pause(0);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_TRUE(motor_on());

    /* čekání na motor, pauza a pokračování: páska čeká dál (MCP pause true + pause false) */
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    cmt_pause(1);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    cmt_pause(0);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());

    /* bez pauzy uživatele pokračování motor rozběhne (rozjede čekající pásku) */
    cmt_pause(0);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(motor_on());

    /* STOP oba zdroje nuluje */
    cmt_pause(1);
    cmt_stop();
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.motor_off);

    /* cmt_play_paused i cmt_pause(1) ve STOP = PLAY čekající na motor, první hrana PC3 pásku rozjede */
    cmt_play_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    cmt_stop();
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY && CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    motor_edge();
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "pause in STOP + first PC3 edge starts the tape (ROM LOAD)");
    TEST_ASSERT_EQUAL_INT(1, pio8255_pc4_get());
    hack_restore();
}

/*
 * Previous / Next, které selže (jednobloková páska), nechá hrající pásku
 * v PLAY na původním bloku a v pauze uživatele i se stojícím motorem.
 * Dřív krok pásku zastavil.
 */
void test_cmt_failed_step_keeps_play_and_pause_sources(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    open_and_play();
    for (int forward = 0; forward <= 1; forward++)
    {
        TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(forward));
        TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PLAY, "a failed step keeps playing");
        TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
        TEST_ASSERT_TRUE(motor_on());
    }

    /* pauza uživatele se stojícím motorem */
    cmt_pause(1);
    motor_edge();
    TEST_ASSERT_FALSE(motor_on());
    for (int forward = 0; forward <= 1; forward++)
    {
        TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(forward));
        TEST_ASSERT_TRUE(CMT_TEST_PLAY);
        TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cmt.user_paused, "a failed step keeps the user's pause");
        TEST_ASSERT_FALSE_MESSAGE(motor_on(), "a failed step keeps the motor state");
    }
    /* pokračování: motor stojí dál, hrana ho rozběhne */
    cmt_pause(0);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    hack_restore();
}

/*
 * Otisk stroje (část cmt) rozliší pauzu uživatele a stav motoru za ní;
 * bez pauzy uživatele motor_off otisk nemění (odvozuje se z paused, otisk
 * je stejný jako před rozdělením zdrojů).
 */
void test_cmt_user_pause_machine_hash(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    open_and_play();
    motor_edge(); /* čeká na motor, bez pauzy uživatele */
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    char h_wait[STATE_HASH_HEX_LEN + 1], h_x[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_CMT, 0, h_wait));
    /* bez pauzy uživatele: motor_off do otisku nejde */
    int saved_motor_off = g_cmt.motor_off;
    g_cmt.motor_off = !saved_motor_off;
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_CMT, 0, h_x));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(h_wait, h_x, "motor_off without a user's pause is not hashed");
    g_cmt.motor_off = saved_motor_off;

    /* stejné paused a časy, jen zdroje pauzy jinak: otisk se liší */
    g_cmt.user_paused = 1;
    g_cmt.motor_off = 1;
    char h_user_off[STATE_HASH_HEX_LEN + 1], h_user_on[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_CMT, 0, h_user_off));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(h_wait, h_user_off), "the user's pause is hashed");
    g_cmt.motor_off = 0;
    TEST_ASSERT_TRUE(machine_hash_part(MACHINE_HASH_CMT, 0, h_user_on));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(h_user_off, h_user_on), "the motor behind the user's pause is hashed");
    g_cmt.user_paused = 0;
    g_cmt.motor_off = saved_motor_off;
    hack_restore();
}

/*
 * RECORD s pauzou uživatele: hrany PC3 přepínají jen motor, pokračování
 * zruší pauzu a nechá motor podle programu (stojí-li, nahrávání čeká na
 * motor). Nahrávání bez pauzy uživatele čeká na motor jako dřív.
 */
void test_cmt_record_user_pause_and_motor(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "the motor starts the recording");

    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PAUSED, "a PC3 edge does not end the user's pause while recording");
    TEST_ASSERT_TRUE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE(motor_on());
    cmt_pause(0);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PAUSED, "resume leaves the recording waiting for the motor");
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_stop();
    hack_restore();
}

/**
 * @brief Zapne příznak ROM patche CMT hacku (jen příznak, ROM se nemění).
 *
 * Hrana PC3 a čtení PC4 (pio8255.c) se řídí jen příznakem
 * CMTHACK_TEST_IS_INSTALLED; stav vrátí hack_restore() nebo tearDown.
 */
static void hack_on(void)
{
    if (!s_hack_saved)
        s_saved_hack = g_cmthack.load_patch_installed;
    s_hack_saved = true;
    g_cmthack.load_patch_installed = 1;
}

/*
 * Nahrávání se zapnutým CMT hackem: hrana PC3 programu řídí deck stejně
 * jako bez hacku (cmt_motor_toggle()) a PC4 čte skutečný stav decku, takže
 * @MGON ROM (069Fh: PC4 = 0 -> hrana, PC4 = 1 -> motor běží) nahrávání
 * rozjede a @MGOFF (0700h) zastaví; podvržený bit PC4 hacku se při tom
 * nemění. Při přehrávání (PLAY) se hackem nic nemění: hrana jen přepne
 * podvržený bit PC4, deck dál čeká na motor.
 */
void test_cmt_record_motor_with_hack(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_on();
    unsigned fake = g_pio8255.signal_pc04 & 1;

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pio8255_pc4_get(), "PC4 = 0: the recording waits for the motor");
    TEST_ASSERT_EQUAL_HEX8(0x00, pio8255_peek(2 /* Port C */) & 0x10);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "with the CMT hack the motor command starts the recording");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, pio8255_pc4_get(), "PC4 = 1: the motor runs");
    TEST_ASSERT_EQUAL_HEX8(0x10, pio8255_peek(2 /* Port C */) & 0x10);
    TEST_ASSERT_TRUE(motor_on());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake, g_pio8255.signal_pc04 & 1, "the fake PC4 bit of the hack is not toggled while recording");
    motor_edge();
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PAUSED, "the next motor command stops the recording");
    TEST_ASSERT_EQUAL_INT(0, pio8255_pc4_get());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_stop();

    /* přehrávání s hackem beze změny: hrana přepne jen podvržený PC4 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_mzf_path));
    cmt_play_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    fake = g_pio8255.signal_pc04 & 1;
    motor_edge();
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_PAUSED, "with the CMT hack a motor command does not start playback");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake ^ 1u, g_pio8255.signal_pc04 & 1, "the motor command toggles the fake PC4 bit");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(fake ^ 1u), pio8255_pc4_get(), "PC4 reads the fake bit in PLAY");
    cmt_stop();
    hack_restore();
}

/*
 * Při nahrávání (RECORD) jádro odmítne krok na sousední blok i výběr bloku
 * (i dvojklik, forced) jako seek: nahrávání a jeho
 * stav zůstanou, otevřený blok se nezmění. Okno tyto akce při nahrávání
 * zakazuje; operace ze staršího snímku nebo jiné cesty (záznam vstupů)
 * tak nahrávání nerozbije.
 */
void test_cmt_step_and_select_refused_while_recording(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_mzf_path));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    const st_CMTEXT_BLOCK *block = g_cmt.ext->block;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_step_block(0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_select_block(0, 0));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_select_block(0, 1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_seek_block(0));
    TEST_ASSERT_TRUE_MESSAGE(CMT_TEST_RECORD, "the recording goes on");
    TEST_ASSERT_FALSE_MESSAGE(CMT_TEST_PAUSED, "the recording is not paused");
    TEST_ASSERT_TRUE_MESSAGE(g_cmt.ext->block == block, "the open block does not change");
    cmt_stop();
    /* po STOP výběr i krok zase jdou (výběr ve STOP) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_select_block(0, 0));
    hack_restore();
}

/*
 * Snapshot nese pauzu uživatele i stav motoru za ní; snapshot bez pauzy
 * uživatele ji nemá (starší snapshot se načte jako bez pauzy uživatele)
 * a motor odvodí z paused.
 */
void test_cmt_user_pause_snapshot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    hack_off();

    open_and_play();
    cmt_pause(1);
    motor_edge(); /* motor stojí, pauza uživatele trvá */
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    save_snapshot(&buf, &buf_size);
    cmt_pause(0);
    motor_edge(); /* jiný stav v paměti: bez pauzy uživatele, páska jede */
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_PLAY, g_cmt.state);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(1, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_TRUE(motor_on());

    /* bez pauzy uživatele: stojící motor ze snapshotu, hrana pásku rozjede */
    cmt_pause(0);
    motor_edge();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    save_snapshot(&buf, &buf_size);
    cmt_pause(1);
    load_snapshot(buf, buf_size);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.user_paused);
    TEST_ASSERT_FALSE(motor_on());
    motor_edge();
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    hack_restore();
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    s_initial_cpu_boost = g_cmt.cpu_boost;
    emulator_max_speed(false);

    UNITY_BEGIN();

    RUN_TEST(test_cmt_no_tape_play_state_does_not_crash);
    RUN_TEST(test_cmt_snapshot_play_loaded_without_tape_is_stopped);
    RUN_TEST(test_cmt_snapshot_with_tape_keeps_play_and_playsts);
    RUN_TEST(test_cmt_sanitize_state);
    RUN_TEST(test_cmt_stop_without_tape_stops);
    RUN_TEST(test_cmt_open_after_stale_play_starts_from_beginning);
    RUN_TEST(test_cmt_play_paused_without_tape_keeps_invariant);
    RUN_TEST(test_cmt_echo_reports_edges_and_keeps_output);
    RUN_TEST(test_cmt_wav_stop_play_restarts_from_beginning);
    RUN_TEST(test_cmt_snapshot_play_with_cpu_boost_turns_max_speed_on);
    RUN_TEST(test_cmt_snapshot_stop_turns_boost_max_speed_off);
    RUN_TEST(test_cmt_user_max_speed_is_kept);
    RUN_TEST(test_cmt_cpu_boost_follows_pause_and_option);
    RUN_TEST(test_cmt_snapshot_does_not_restore_cpu_boost_on);
    RUN_TEST(test_cmt_snapshot_does_not_restore_cpu_boost_off);
    RUN_TEST(test_cmt_tape_block_speed_applies_on_block_open);
    RUN_TEST(test_cmt_tape_block_speed_single_mzf_and_file_speed);
    RUN_TEST(test_cmt_user_pause_and_motor_are_independent);
    RUN_TEST(test_cmt_user_pause_snapshot);
    RUN_TEST(test_cmt_failed_step_keeps_play_and_pause_sources);
    RUN_TEST(test_cmt_user_pause_machine_hash);
    RUN_TEST(test_cmt_record_user_pause_and_motor);
    RUN_TEST(test_cmt_record_motor_with_hack);
    RUN_TEST(test_cmt_step_and_select_refused_while_recording);

    int result = UNITY_END();
    g_free(s_mzf_path);
    mztest_teardown();
    return result;
}
