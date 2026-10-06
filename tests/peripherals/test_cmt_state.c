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
 *     aktuální volbě uživatele.
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
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/gdg/gdg.h"

/** @brief Cesta k dočasnému testovacímu MZF (vytváří setUp, maže tearDown). */
static char *s_mzf_path = NULL;

/** @brief Hodnota cpu_boost před prvním testem (obnovuje ji tearDown). */
static en_CMT_CPU_BOOST s_initial_cpu_boost = CMT_CPU_BOOST_DISABLED;

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
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    g_emulator.paused = false;
    /* MAX SPEED a cpu_boost zpět do výchozího stavu (testy boostu je mění). */
    g_cmt.cpu_boost = s_initial_cpu_boost;
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

    /* Nový proces nemá vloženou pásku. */
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

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

    emulator_max_speed(true); /* uživatel */
    TEST_ASSERT_FALSE(g_emulator.max_speed_boost);
    load_snapshot(buf, buf_size);
    TEST_ASSERT_EQUAL_INT(CMT_STATE_STOP, g_cmt.state);
    TEST_ASSERT_TRUE(EMULATOR_TEST_MAX_SPEED);

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

/* stop + play přehraje WAV (SINGLE container, bez seeku) znovu od začátku. */
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
    RUN_TEST(test_cmt_wav_stop_play_restarts_from_beginning);
    RUN_TEST(test_cmt_snapshot_play_with_cpu_boost_turns_max_speed_on);
    RUN_TEST(test_cmt_snapshot_stop_turns_boost_max_speed_off);
    RUN_TEST(test_cmt_user_max_speed_is_kept);
    RUN_TEST(test_cmt_cpu_boost_follows_pause_and_option);
    RUN_TEST(test_cmt_snapshot_does_not_restore_cpu_boost_on);
    RUN_TEST(test_cmt_snapshot_does_not_restore_cpu_boost_off);

    int result = UNITY_END();
    g_free(s_mzf_path);
    mztest_teardown();
    return result;
}
