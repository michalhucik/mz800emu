/**
 * @file test_cmt_tmz_analyze.c
 * @brief Testy analýzy surového bloku pásky (0x18 CSW) a uplatnění výsledku.
 *
 * Testovací páska: blok 0x46 "PRE", surový blok 0x18 a blok 0x46 "POST".
 * Surový blok nese signál ROM dvou souborů MZF ("ONE", "TWO", mztape
 * MZ-800 1:1 při 44 100 Hz) se šumem a tichem před, mezi a za nimi -
 * stejně jako nahrávka, kterou záznam nedekódoval, nebo import WAV.
 *
 * Testuje se:
 *   - řádky analýzy jdou v pořadí bez překryvů a pokrývají blok kromě
 *     neznámých úseků kratších než CMTTMZ_REC_MIN_EDGES (časy v µs), dva
 *     soubory ADDABLE se správnými jmény, typem, velikostí a součty,
 *   - soubor s přepsanými pulzy těla v obou kopiích: STAYS_RAW, Checksum
 *     error těla; useknutý soubor bez opravy zůstane surový, s volbou
 *     opravy těla ADDABLE_REPAIRED,
 *   - uplatnění Add (bloky za surovým blokem, pořadí, data MZF, pauzy,
 *     příznaky neuloženo, Modified, čítač výměn pásky) a Replace (každý pulz
 *     řádku, který zůstává surový, je v surovém bloku v původním pořadí vůči
 *     blokům souborů; pauzy bloků souborů jako u záznamu, délka se
 *     nedoplňuje; typ WAV surových zbytků; soubory oddělené jen tichem -
 *     krátké neznámé úseky řádky nejsou a Replace je zahodí),
 *   - varování o taktu nových bloků souborů za blokem 0x43 s jiným než
 *     výchozím taktem (Add i Replace, jednou), bez varování jinak,
 *   - odmítnutí: řádek, který nejde přidat; jiná páska (tape_serial), jiný
 *     blok na stejném čísle, analýza z jiné kopie;
 *     páska se nemění,
 *   - kopie pulzů: blok jiného typu než 0x18, neexistující blok, bez pásky;
 *     prázdný blok 0x18,
 *   - histogram (součet binů = počet započtených pulzů, omezení počtu,
 *     nejdelší pulz, neplatné parametry),
 *   - obal cmt_tape_analysis_apply() mimo STOP.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/emulator.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_analyze.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmt_tmz_record.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/mzcmt_bsd/mzcmt_bsd.h"
#include "libs/mztape/mztape.h"
#include "libs/mzcmt_fm/mzcmt_fm.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "libs/wav_analyzer/wav_analyzer.h"

/** @brief Vzorkovací frekvence surového bloku testovací pásky. */
#define AN_RATE 44100u

/** @brief Velikost těla testovacích MZF. */
#define AN_BODY_SIZE 512

/** @brief Dočasný soubor pásky (maže tearDown). */
static char *s_path = NULL;

/** @brief Uložená výchozí rychlost decku. */
static en_CMTSPEED s_saved_speed;

/** @brief Stav generátoru pseudonáhodných čísel. */
static uint32_t s_rng = 1;

/** @brief Pseudonáhodné číslo 0 .. 2^31-1 (LCG, deterministické). */
static uint32_t an_rand(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 1) & 0x7FFFFFFFu;
}

/** @brief Počet jedničkových bitů (součet ROM). */
static uint32_t an_popcount(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint8_t b = d[i]; b; b >>= 1) c += b & 1u;
    return c;
}

/**
 * @brief Naplní MZF pro mztape (typ 01h, jméno, tělo AN_BODY_SIZE B ze semínka).
 * @param m Cíl. @param body Buffer těla. @param name Jméno (ASCII). @param seed Semínko těla.
 */
static void an_make_mzf(st_MZTAPE_MZF *m, uint8_t *body, const char *name, uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    uint32_t x = seed;
    for (int i = 0; i < AN_BODY_SIZE; i++) {
        x = x * 1103515245u + 12345u;
        body[i] = (uint8_t)(x >> 16);
    }
    m->header[0x00] = 0x01;
    memset(&m->header[0x01], 0x0D, 17);
    memcpy(&m->header[0x01], name, strlen(name));
    m->header[0x12] = (uint8_t)(AN_BODY_SIZE & 0xFF);
    m->header[0x13] = (uint8_t)(AN_BODY_SIZE >> 8);
    m->header[0x15] = 0x12;
    m->header[0x17] = 0x12;
    m->body = body;
    m->size = AN_BODY_SIZE;
    m->chkh = an_popcount(m->header, sizeof(m->header));
    m->chkb = an_popcount(body, AN_BODY_SIZE);
}

/** @brief Posloupnost pulzů CSW (první pulz LOW, úrovně se střídají). */
typedef struct {
    uint32_t *len;  /**< délky ve vzorcích */
    uint32_t count; /**< počet pulzů */
    uint32_t cap;   /**< kapacita */
} st_AN_SIG;

/**
 * @brief Přidá pulz dané úrovně; stejná úroveň jako poslední pulz ho prodlouží.
 * @param s Posloupnost. @param level Úroveň (0 / 1). @param len Délka ve vzorcích (> 0).
 */
static void sig_add(st_AN_SIG *s, int level, uint32_t len)
{
    if (s->count && ((int)((s->count - 1) & 1u) == level)) {
        s->len[s->count - 1] += len;
        return;
    }
    if (!s->count && level) {
        sig_add(s, 0, 1); /* první pulz CSW je LOW */
    }
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 65536;
        s->len = (uint32_t *)realloc(s->len, s->cap * sizeof(uint32_t));
        TEST_ASSERT_NOT_NULL(s->len);
    }
    s->len[s->count++] = len;
}

/** @brief Úroveň dalšího pulzu posloupnosti (střídání). */
static int sig_next_level(const st_AN_SIG *s)
{
    return (int)(s->count & 1u);
}

/** @brief Přidá šum: n pulzů 3 až 60 vzorků se střídající se úrovní. */
static void sig_noise(st_AN_SIG *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) sig_add(s, sig_next_level(s), 3u + an_rand() % 58u);
}

/** @brief Přidá ticho: jeden pulz délky ms (úroveň další v pořadí). */
static void sig_silence(st_AN_SIG *s, uint32_t ms)
{
    sig_add(s, sig_next_level(s), AN_RATE / 1000u * ms);
}

/**
 * @brief Přidá signál ROM souboru (mztape MZ-800, 1:1, AN_RATE), nejvýš max_pulses pulzů.
 * @param s Posloupnost. @param m MZF. @param max_pulses Strop počtu pulzů (UINT32_MAX = celý).
 * @return Počet přidaných pulzů signálu.
 */
static uint32_t sig_rom(st_AN_SIG *s, st_MZTAPE_MZF *m, uint32_t max_pulses)
{
    st_CMT_VSTREAM *vs = mztape_create_cmt_vstream_from_mztmzf(m, MZTAPE_FORMATSET_MZ800, CMTSPEED_1_1, AN_RATE);
    TEST_ASSERT_NOT_NULL(vs);
    uint64_t samples;
    int value;
    uint32_t n = 0;
    cmt_vstream_read_reset(vs);
    while (n < max_pulses && cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS) {
        if (!samples) continue;
        sig_add(s, value & 1, (uint32_t)samples);
        n++;
    }
    cmt_vstream_destroy(vs);
    return n;
}

/** @brief Varianta testovací pásky. */
typedef enum {
    AN_TAPE_GOOD = 0,   /**< oba soubory bez chyby */
    AN_TAPE_BAD_BODY,   /**< soubor TWO s přepsanými pulzy jednoho bajtu těla v obou kopiích */
    AN_TAPE_CUT_BODY,   /**< soubor TWO useknutý v první kopii těla (konec bloku) */
    AN_TAPE_SILENCE,    /**< ONE, ticho, TWO - bez šumu (krátké neznámé úseky) */
} en_AN_TAPE;

/** @brief Tělo souborů testovací pásky (statické, platí po celý test). */
static uint8_t s_body1[AN_BODY_SIZE], s_body2[AN_BODY_SIZE];

/** @brief MZF souborů testovací pásky. */
static st_MZTAPE_MZF s_m1, s_m2;

/**
 * @brief Sestaví signál surového bloku: šum, ONE, ticho + šum, TWO, šum (podle varianty).
 * @param s Výsledek (prázdný). @param variant Varianta.
 */
static void build_signal(st_AN_SIG *s, en_AN_TAPE variant)
{
    s_rng = 7;
    an_make_mzf(&s_m1, s_body1, "ONE", 111);
    an_make_mzf(&s_m2, s_body2, "TWO", 222);
    if (variant == AN_TAPE_SILENCE) {
        /* bez šumu: mezi soubory jen ticho (úseky kratší než mez záznamu 64 pulzů) */
        sig_rom(s, &s_m1, UINT32_MAX);
        sig_silence(s, 500);
        sig_rom(s, &s_m2, UINT32_MAX);
        return;
    }
    sig_noise(s, 700);
    sig_silence(s, 300);
    sig_rom(s, &s_m1, UINT32_MAX);
    sig_silence(s, 400);
    sig_noise(s, 300);
    sig_silence(s, 400);
    if (variant == AN_TAPE_BAD_BODY) {
        /*
         * Přepsané pulzy: signál téhož souboru s jedním bitem těla navíc a
         * součtem z původního těla. Délka signálu je stejná (bit = 2 pulzy),
         * liší se jen pulzy toho bajtu v obou kopiích těla.
         */
        st_AN_SIG good = {0}, bad = {0};
        sig_rom(&good, &s_m2, UINT32_MAX);
        static uint8_t body_bad[AN_BODY_SIZE];
        memcpy(body_bad, s_body2, AN_BODY_SIZE);
        body_bad[100] ^= 0x10;
        st_MZTAPE_MZF mb = s_m2;
        mb.body = body_bad; /* chkb zůstává z původního těla */
        sig_rom(&bad, &mb, UINT32_MAX);
        TEST_ASSERT_EQUAL_UINT32(good.count, bad.count);
        uint32_t diff = 0;
        for (uint32_t i = 0; i < good.count; i++) diff += (good.len[i] != bad.len[i]);
        TEST_ASSERT_TRUE_MESSAGE(diff > 0 && diff < 20, "corruption must overwrite only a few pulses");
        int level = sig_next_level(s);
        for (uint32_t i = 0; i < bad.count; i++) sig_add(s, (level ^ (int)(i & 1u)) & 1, bad.len[i]);
        free(good.len);
        free(bad.len);
    } else if (variant == AN_TAPE_CUT_BODY) {
        /* jen část signálu: hlavičky celé, tělo useknuté v první kopii */
        st_AN_SIG all = {0};
        sig_rom(&all, &s_m2, UINT32_MAX);
        uint32_t keep = all.count - (uint32_t)(AN_BODY_SIZE * 9 * 2) * 3 / 2;
        int level = sig_next_level(s);
        for (uint32_t i = 0; i < keep; i++) sig_add(s, (level ^ (int)(i & 1u)) & 1, all.len[i]);
        free(all.len);
        return; /* konec bloku uprostřed těla */
    } else {
        sig_rom(s, &s_m2, UINT32_MAX);
    }
    sig_silence(s, 200);
    sig_noise(s, 500);
}

/**
 * @brief Sestaví blok 0x46 (MZ800EMU-CMT, 1:1) s malým MZF daného jména.
 * @param name Jméno. @param pause_ms Pauza. @return Blok (tmz_block_free()).
 */
static st_TZX_BLOCK *build_small_mzf(const char *name, uint16_t pause_ms)
{
    st_MZF_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ftype = 0x01;
    memset(&hdr.fname, 0x0D, sizeof(hdr.fname));
    memcpy(&hdr.fname, name, strlen(name));
    hdr.fsize = 32;
    hdr.fstrt = 0x1200;
    hdr.fexec = 0x1200;
    uint8_t body[32];
    for (int i = 0; i < 32; i++) body[i] = (uint8_t)(i * 7 + name[0]);
    st_TZX_BLOCK *b = cmttmz_build_mzf_block(&hdr, body, 32, CMTSPEED_1_1, NULL, pause_ms);
    TEST_ASSERT_NOT_NULL(b);
    return b;
}

/** @brief Signál surového bloku poslední otevřené testovací pásky (kopie pro porovnání). */
static st_AN_SIG s_sig;

/** @brief Pauza surového bloku testovací pásky v ms. */
#define AN_RAW_PAUSE_MS 250

/** @brief Pauza surového bloku příští otevřené pásky v ms (výchozí AN_RAW_PAUSE_MS, vrací tearDown). */
static uint16_t s_raw_pause = AN_RAW_PAUSE_MS;

/** @brief 1 = příští otevřená páska nemá blok POST (surový blok je poslední; vrací tearDown). */
static int s_no_post = 0;

/**
 * @brief Takt bloku 0x43 místo bloku PRE příští otevřené pásky v Hz (0 = blok PRE; vrací tearDown).
 *
 * Blok 0x43 (MZ Machine Info, stroj MZ-800) před surovým blokem: takt CPU
 * platí pro bloky souborů za ním.
 */
static uint32_t s_pre_clock = 0;

/**
 * @brief Uloží a otevře pásku PRE, surový blok (s_sig), POST.
 * @param variant Varianta signálu. @param raw_override Data surového bloku místo signálu (NULL = signál).
 */
static void open_tape_ex(en_AN_TAPE variant, const st_TZX_BLOCK *raw_override)
{
    free(s_sig.len);
    memset(&s_sig, 0, sizeof(s_sig));
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;

    st_TZX_BLOCK *pre = s_pre_clock ? tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ800, s_pre_clock, 0)
                                    : build_small_mzf("PRE", 1000);
    TEST_ASSERT_NOT_NULL(pre);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, pre));
    pre->data = NULL;
    tmz_block_free(pre);

    st_TZX_BLOCK raw;
    if (raw_override) {
        raw = *raw_override;
    } else {
        build_signal(&s_sig, variant);
        TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(AN_RATE, s_raw_pause, s_sig.len, s_sig.count, &raw));
    }
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &raw));

    if (!s_no_post) {
        st_TZX_BLOCK *post = build_small_mzf("POST", 0);
        TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, post));
        post->data = NULL;
        tmz_block_free(post);
    }

    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    g_free(s_path);
    gchar *name = g_strdup_printf("mztest_cmt_analyze_%u.tmz", (unsigned)g_random_int());
    s_path = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    TEST_ASSERT_TRUE(g_file_set_contents(s_path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(s_path));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}

/** @brief Otevře testovací pásku dané varianty. */
static void open_tape(en_AN_TAPE variant)
{
    open_tape_ex(variant, NULL);
}

/** @brief Počet bloků vložené pásky. */
static int tape_count(void)
{
    return CMT_TEST_FILLED ? cmtext_container_get_count_blocks(g_cmt.ext->container) : -1;
}

/** @brief Popis pořadí bloků: jména MZF, ostatní "#ID" (oddělené čárkou). */
static const char *tape_names(char *out, size_t size)
{
    out[0] = '\0';
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    const st_TZX_FILE *f = cmttmz_get_file();
    size_t len = 0;
    for (int i = 0; c && f && i < c->count_blocks; i++) {
        int n;
        if (cmtext_container_get_block_type(c, i) == CMTEXT_BLOCK_TYPE_MZF)
            n = snprintf(out + len, size - len, "%s%s", i ? "," : "", cmtext_container_get_block_fname(c, i));
        else
            n = snprintf(out + len, size - len, "%s#%02X", i ? "," : "", f->blocks[i].id);
        if (n > 0 && (size_t)n < size - len) len += (size_t)n;
    }
    return out;
}

/** @brief Ověří pořadí bloků. */
#define ASSERT_NAMES(expected)                                                                                         \
    do {                                                                                                               \
        char names_buf_[256];                                                                                          \
        TEST_ASSERT_EQUAL_STRING(expected, tape_names(names_buf_, sizeof(names_buf_)));                                \
    } while (0)

/** @brief Kopie pulzů a analýza bloku 1 výchozími volbami. */
static st_CMTTMZ_ANALYSIS *analyze_block1(st_CMTTMZ_PULSES **p, const st_CMTTMZ_ANALYZE_OPTS *opts)
{
    *p = cmttmz_analyze_copy_pulses(1);
    TEST_ASSERT_NOT_NULL_MESSAGE(*p, cmttmz_get_edit_error());
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(*p, opts);
    TEST_ASSERT_NOT_NULL(a);
    return a;
}

/** @brief Index prvního řádku souboru daného jména (-1 = není). */
static int row_by_name(const st_CMTTMZ_ANALYSIS *a, const char *name)
{
    for (uint32_t i = 0; i < cmttmz_analysis_row_count(a); i++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        if (r->kind == CMTTMZ_ANALYZE_FILE && 0 == strcmp(r->name, name)) return (int)i;
    }
    return -1;
}

/** @brief Zaokrouhlený převod vzorků na µs (jako jádro). */
static uint64_t an_us(uint64_t samples, uint32_t rate)
{
    return (samples / rate) * 1000000u + ((samples % rate) * 1000000u + rate / 2u) / rate;
}

/**
 * @brief Ověří rozložení řádků: pořadí bez překryvů, mezery (i na krajích) kratší než
 *        CMTTMZ_REC_MIN_EDGES pulzů, neznámé úseky aspoň CMTTMZ_REC_MIN_EDGES pulzů, časy.
 * @param p Kopie. @param a Analýza.
 * @return Počet vynechaných mezer (úseků mezi řádky a na krajích s aspoň 1 pulzem).
 */
static uint32_t assert_rows_cover(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a)
{
    uint32_t n = cmttmz_analysis_row_count(a);
    TEST_ASSERT_TRUE(n > 0);
    uint32_t pulse = 0;
    uint64_t cum = 0;
    uint32_t gaps = 0;
    for (uint32_t i = 0; i < n; i++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE_MESSAGE(r->first_pulse >= pulse, "rows must not overlap");
        TEST_ASSERT_TRUE_MESSAGE(r->first_pulse - pulse < CMTTMZ_REC_MIN_EDGES, "only short unknown parts may be left out");
        if (r->first_pulse > pulse) gaps++;
        /* prázdný rozsah jen u souboru celého uvnitř dřívějšího (překryv) */
        if (r->kind == CMTTMZ_ANALYZE_UNKNOWN) TEST_ASSERT_TRUE(r->end_pulse > r->first_pulse);
        else TEST_ASSERT_TRUE(r->end_pulse >= r->first_pulse);
        for (; pulse < r->first_pulse; pulse++) cum += p->lengths[pulse];
        TEST_ASSERT_EQUAL_UINT64(an_us(cum, p->sample_rate), r->start_us);
        for (; pulse < r->end_pulse; pulse++) cum += p->lengths[pulse];
        TEST_ASSERT_EQUAL_UINT64(an_us(cum, p->sample_rate) - r->start_us, r->length_us);
        if (r->kind == CMTTMZ_ANALYZE_UNKNOWN) {
            TEST_ASSERT_TRUE_MESSAGE(r->end_pulse - r->first_pulse >= CMTTMZ_REC_MIN_EDGES,
                                     "unknown parts shorter than the recording threshold are not rows");
            TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_STAYS_RAW, r->result);
            TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_NO_FILE, r->reason);
            TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE, r->header_checksum);
            TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_NOT_AVAILABLE, r->body_checksum);
            TEST_ASSERT_EQUAL_STRING("", r->name);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(p->count - pulse < CMTTMZ_REC_MIN_EDGES, "only a short unknown part may be left out at the end");
    if (p->count > pulse) gaps++;
    TEST_ASSERT_NULL(cmttmz_analysis_row(a, n));
    return gaps;
}

/** @brief Ověří, že blok 0x46 na pásce nese MZF bajtově shodný s m. */
static void assert_block_mzf(int id, const st_MZTAPE_MZF *m)
{
    st_MZF *mzf = cmttmz_block_get_mzf(id);
    TEST_ASSERT_NOT_NULL(mzf);
    st_MZF_HEADER hdr;
    memcpy(&hdr, &mzf->header, sizeof(hdr));
    mzf_header_swap_endianity(&hdr); /* host -> soubor */
    TEST_ASSERT_EQUAL_MEMORY(m->header, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(m->size, mzf->header.fsize);
    TEST_ASSERT_EQUAL_MEMORY(m->body, mzf->body, m->size);
    mzf_free(mzf);
}

/** @brief Pauza bloku ve vedlejším poli rychlostí v ms. */
static uint16_t block_pause(int id)
{
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(id, &sp, NULL));
    return sp.pause_ms;
}

/** @brief Délka bloku pásky v µs přehráním playerem při GDGCLK_BASE (včetně pauzy). */
static double block_length_us(int id)
{
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = GDGCLK_BASE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *s = tmz_player_play_block(&cmttmz_get_file()->blocks[id], &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, tmz_player_error_string(perr));
    double len = cmt_stream_get_length(s) * 1e6;
    cmt_stream_destroy(s);
    return len;
}

/**
 * @brief Rozbalí pulzy bloku 0x18 pásky.
 * @param id Blok. @param[out] count Počet pulzů. @param[out] rate Frekvence. @param[out] pause Pauza.
 * @return Pole délek (free()).
 */
static uint32_t *block_csw_pulses(int id, uint32_t *count, uint32_t *rate, uint16_t *pause)
{
    const st_TZX_BLOCK *b = &cmttmz_get_file()->blocks[id];
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, b->id);
    st_TZX_CSW_RECORDING csw;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_parse_csw_recording(b->data, b->length, &csw));
    uint32_t *out = (uint32_t *)malloc((csw.pulse_count ? csw.pulse_count : 1) * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(out);
    uint32_t pos = 0;
    for (uint32_t i = 0; i < csw.pulse_count; i++) {
        uint32_t v = csw.data[pos++];
        if (!v) {
            v = csw.data[pos] | (csw.data[pos + 1] << 8) | (csw.data[pos + 2] << 16) | ((uint32_t)csw.data[pos + 3] << 24);
            pos += 4;
        }
        out[i] = v;
    }
    TEST_ASSERT_EQUAL_UINT32(csw.data_length, pos);
    *count = csw.pulse_count;
    *rate = csw.sample_rate;
    *pause = csw.pause_ms;
    return out;
}

/** @brief Čítače pásky. */
typedef struct {
    unsigned tape;   /**< cmttmz_get_tape_serial() */
    unsigned change; /**< cmttmz_get_change_serial() */
    int count;       /**< počet bloků */
    int modified;    /**< cmttmz_is_modified() */
} st_AN_STATE;

/** @brief Přečte čítače pásky. */
static st_AN_STATE tape_state(void)
{
    st_AN_STATE s = {cmttmz_get_tape_serial(), cmttmz_get_change_serial(), tape_count(), cmttmz_is_modified()};
    return s;
}

/** @brief Ověří, že se páska nezměnila (čítače, počet bloků, Modified, pořadí). */
static void assert_tape_same(st_AN_STATE s)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.tape, cmttmz_get_tape_serial(), "tape serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.change, cmttmz_get_change_serial(), "change serial");
    TEST_ASSERT_EQUAL_INT(s.count, tape_count());
    TEST_ASSERT_EQUAL_INT(s.modified, cmttmz_is_modified());
    ASSERT_NAMES("PRE,#18,POST");
}

void setUp(void)
{
    g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
    g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
    s_saved_speed = g_cmt.mz_cmtspeed;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
}

void tearDown(void)
{
    cmt_eject();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    if (s_path) {
        g_remove(s_path);
        g_free(s_path);
        s_path = NULL;
    }
    free(s_sig.len);
    memset(&s_sig, 0, sizeof(s_sig));
    s_raw_pause = AN_RAW_PAUSE_MS;
    s_no_post = 0;
    s_pre_clock = 0;
    cmttmz_analysis_test_set_max_pulses(0);
}

/* ================================================================
 * VOLBY A KOPIE PULZŮ
 * ================================================================ */

/* Výchozí volby: tolerance 0,20, opravy vypnuté. */
void test_opts_default(void)
{
    st_CMTTMZ_ANALYZE_OPTS o;
    memset(&o, 0x55, sizeof(o));
    cmttmz_analyze_opts_default(&o);
    TEST_ASSERT_EQUAL_DOUBLE(0.20, o.tolerance);
    TEST_ASSERT_EQUAL_INT(0, o.repair_header);
    TEST_ASSERT_EQUAL_INT(0, o.repair_body);
    TEST_ASSERT_EQUAL_INT(0, o.repair_bsd);
    cmttmz_analyze_opts_default(NULL);
}

/* Kopie pulzů: shoda se signálem bloku, údaje pásky; blok jiného typu, mimo rozsah a bez pásky = NULL a text. */
void test_copy_pulses(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_NULL(cmttmz_analyze_copy_pulses(0));
    TEST_ASSERT_EQUAL_STRING("No tape inserted", cmttmz_get_edit_error());

    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = cmttmz_analyze_copy_pulses(1);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(AN_RATE, p->sample_rate);
    TEST_ASSERT_EQUAL_UINT32(s_sig.count, p->count);
    TEST_ASSERT_EQUAL_MEMORY(s_sig.len, p->lengths, s_sig.count * sizeof(uint32_t));
    TEST_ASSERT_EQUAL_INT(0, p->first_level);
    TEST_ASSERT_EQUAL_UINT16(AN_RAW_PAUSE_MS, p->pause_ms);
    TEST_ASSERT_EQUAL_UINT(cmttmz_get_tape_serial(), p->tape_serial);
    TEST_ASSERT_EQUAL_INT(1, p->block_id);
    TEST_ASSERT_EQUAL_INT(0, p->unsaved);
    TEST_ASSERT_EQUAL_INT(0, p->wav_type);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < s_sig.count; i++) sum += s_sig.len[i];
    TEST_ASSERT_EQUAL_UINT64((sum * 1000000u + AN_RATE / 2) / AN_RATE, p->total_us);
    cmttmz_pulses_free(p);
    cmttmz_pulses_free(NULL);

    TEST_ASSERT_NULL(cmttmz_analyze_copy_pulses(0));
    TEST_ASSERT_EQUAL_STRING("Block 0 is not a raw signal block (CSW recording)", cmttmz_get_edit_error());
    TEST_ASSERT_NULL(cmttmz_analyze_copy_pulses(3));
    TEST_ASSERT_EQUAL_STRING("Block 3 does not exist", cmttmz_get_edit_error());
    TEST_ASSERT_NULL(cmttmz_analyze_copy_pulses(-1));
    TEST_ASSERT_EQUAL_STRING("Block -1 does not exist", cmttmz_get_edit_error());
}

/* ================================================================
 * ANALÝZA
 * ================================================================ */

/* Dva bezchybné soubory se šumem: řádky pokrývají blok, ONE a TWO ADDABLE se jmény, typem a velikostí. */
void test_analyze_rows_cover_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_analysis_error(a));
    assert_rows_cover(p, a);

    uint32_t files = 0;
    for (uint32_t i = 0; i < cmttmz_analysis_row_count(a); i++)
        if (cmttmz_analysis_row(a, i)->kind == CMTTMZ_ANALYZE_FILE) files++;
    TEST_ASSERT_EQUAL_UINT32(2, files);

    int i1 = row_by_name(a, "ONE"), i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i1 >= 0 && i2 > i1);
    /* šum před ONE, mezi soubory a za TWO je neznámý úsek */
    TEST_ASSERT_TRUE(i1 >= 1);
    TEST_ASSERT_TRUE(i2 >= i1 + 2);
    TEST_ASSERT_TRUE((uint32_t)i2 + 1 < cmttmz_analysis_row_count(a));
    for (int k = 0; k < 2; k++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)(k ? i2 : i1));
        TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE, r->result);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_NONE, r->reason);
        TEST_ASSERT_EQUAL_STRING("NORMAL", r->format_name);
        TEST_ASSERT_EQUAL_UINT8(0x01, r->ftype);
        TEST_ASSERT_EQUAL_UINT32(AN_BODY_SIZE, r->size);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_OK, r->header_checksum);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_OK, r->body_checksum);
        TEST_ASSERT_EQUAL_UINT32(0, r->recovery_status);
        TEST_ASSERT_EQUAL_STRING("complete", r->recovery_text);
        TEST_ASSERT_EQUAL_UINT8(3, r->name_sharp_len);
        TEST_ASSERT_EQUAL_MEMORY(k ? "TWO" : "ONE", r->name_sharp, 3);
        TEST_ASSERT_TRUE(r->short_pulse_us > 0 && r->long_pulse_us > r->short_pulse_us);
        TEST_ASSERT_TRUE(r->length_us > 1000000u); /* signál ROM 512 B trvá několik sekund */
    }
    /* páska se analýzou nemění */
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
    cmttmz_analysis_free(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_analysis_row_count(NULL));
    TEST_ASSERT_NULL(cmttmz_analysis_row(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_analysis_error(NULL));
    TEST_ASSERT_NULL(cmttmz_analyze_pulses(NULL, NULL));
}

/* Přepsané pulzy těla TWO v obou kopiích: TWO STAYS_RAW s Checksum error těla, ONE ADDABLE. */
void test_analyze_corrupted_body_stays_raw(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_BAD_BODY);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    assert_rows_cover(p, a);
    int i1 = row_by_name(a, "ONE"), i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i1 >= 0);
    TEST_ASSERT_TRUE_MESSAGE(i2 > i1, "the damaged file must still be listed");
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE, cmttmz_analysis_row(a, (uint32_t)i1)->result);
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)i2);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_STAYS_RAW, r->result);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_BODY_CHECKSUM, r->reason);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_OK, r->header_checksum);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CHECKSUM_ERROR, r->body_checksum);
    TEST_ASSERT_EQUAL_STRING("NORMAL", r->format_name);
    TEST_ASSERT_EQUAL_STRING("Checksum error in the body", cmttmz_analyze_reason_text(r->reason));
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Useknuté tělo TWO: bez opravy se nepřidá, s volbou opravy těla ADDABLE_REPAIRED (PARTIAL_BODY) a jde přidat. */
void test_analyze_repair_body(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_CUT_BODY);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    assert_rows_cover(p, a);
    int i2 = row_by_name(a, "TWO");
    if (i2 >= 0) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)i2);
        TEST_ASSERT_NOT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE, r->result);
        TEST_ASSERT_NOT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE_REPAIRED, r->result);
    }
    cmttmz_analysis_free(a);

    st_CMTTMZ_ANALYZE_OPTS o;
    cmttmz_analyze_opts_default(&o);
    o.repair_body = 1;
    a = cmttmz_analyze_pulses(p, &o);
    TEST_ASSERT_NOT_NULL(a);
    assert_rows_cover(p, a);
    i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i2 >= 0);
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)i2);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE_REPAIRED, r->result);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_REPAIRED, r->reason);
    TEST_ASSERT_TRUE(r->recovery_status & WAV_RECOVERY_PARTIAL_BODY);
    TEST_ASSERT_EQUAL_UINT32(AN_BODY_SIZE, r->size);

    /* opravený soubor jde přidat: tělo doplněné 00h do fsize */
    uint8_t *use = (uint8_t *)calloc(cmttmz_analysis_row_count(a), 1);
    use[i2] = 1;
    uint32_t added = 99;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(1, added);
    ASSERT_NAMES("PRE,#18,TWO,POST");
    st_MZF *mzf = cmttmz_block_get_mzf(2);
    TEST_ASSERT_NOT_NULL(mzf);
    TEST_ASSERT_EQUAL_UINT32(AN_BODY_SIZE, mzf->header.fsize);
    TEST_ASSERT_EQUAL_MEMORY(s_body2, mzf->body, 16); /* začátek těla je přečtený */
    mzf_free(mzf);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Prázdný blok 0x18 (0 pulzů): kopie s 0 pulzy, analýza bez řádků s chybou, uplatnění nic nezmění. */
void test_empty_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    /* data bloku 0x18: [DWORD 10][WORD pauza][3B frekvence][1B komprese RLE][DWORD 0 pulzů] */
    static const uint8_t raw_data[14] = {10, 0, 0, 0, 0, 0, 0x44, 0xAC, 0x00, 0x01, 0, 0, 0, 0};
    st_TZX_BLOCK raw;
    raw.id = TZX_BLOCK_ID_CSW_RECORDING;
    raw.length = sizeof(raw_data);
    raw.data = (uint8_t *)malloc(sizeof(raw_data));
    TEST_ASSERT_NOT_NULL(raw.data);
    memcpy(raw.data, raw_data, sizeof(raw_data));
    open_tape_ex(AN_TAPE_GOOD, &raw);

    st_CMTTMZ_PULSES *p = cmttmz_analyze_copy_pulses(1);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(0, p->count);
    TEST_ASSERT_NULL(p->lengths);
    TEST_ASSERT_EQUAL_UINT64(0, p->total_us);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analyze_pulses(p, NULL);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_analysis_row_count(a));
    TEST_ASSERT_EQUAL_STRING("The block contains no signal", cmttmz_analysis_error(a));
    st_AN_STATE s = tape_state();
    uint32_t added = 5;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, NULL, 1, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    assert_tape_same(s);
    uint32_t bins[4];
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(p, 0, 0, 5.0, bins, 4, NULL));
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* ================================================================
 * UPLATNĚNÍ
 * ================================================================ */

/** @brief Pole use se všemi řádky, které jdou přidat. @return Pole (free()), *n vybraných. */
static uint8_t *use_all_addable(const st_CMTTMZ_ANALYSIS *a, uint32_t *n)
{
    uint32_t rows = cmttmz_analysis_row_count(a);
    uint8_t *use = (uint8_t *)calloc(rows ? rows : 1, 1);
    TEST_ASSERT_NOT_NULL(use);
    *n = 0;
    for (uint32_t i = 0; i < rows; i++) {
        en_CMTTMZ_ANALYZE_RESULT r = cmttmz_analysis_row(a, i)->result;
        if (r == CMTTMZ_ANALYZE_ADDABLE || r == CMTTMZ_ANALYZE_ADDABLE_REPAIRED) {
            use[i] = 1;
            (*n)++;
        }
    }
    return use;
}

/**
 * @brief Blok vytvořený analýzou nemá příznak importu (st_CMTTMZ_BLOCK_UI::imported).
 * @param id Číslo bloku.
 */
static void assert_not_imported(int id)
{
    st_CMTTMZ_BLOCK_UI ui;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_get_block_ui(id, &ui));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui.imported, "a block made by the analysis is not an imported block");
}

/* Add: ONE a TWO za surovým blokem, data MZF shodná, pauzy, neuloženo jen nové bloky, Modified, čítač pásky. */
void test_apply_add(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(2, added);
    ASSERT_NAMES("PRE,#18,ONE,TWO,POST");
    assert_block_mzf(2, &s_m1);
    assert_block_mzf(3, &s_m2);
    /* surový blok zůstal beze změny */
    uint32_t cnt, rate;
    uint16_t pause;
    uint32_t *pl = block_csw_pulses(1, &cnt, &rate, &pause);
    TEST_ASSERT_EQUAL_UINT32(s_sig.count, cnt);
    TEST_ASSERT_EQUAL_MEMORY(s_sig.len, pl, cnt * sizeof(uint32_t));
    free(pl);
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(2)); /* mezi dvěma vloženými */
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(3)); /* šev: za ním POST */
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(3));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(4));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_NOT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    /* rychlost bloků jako u záznamu: výchozí rychlost decku */
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(2, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_1_1, sp.speed);
    /* bloky analýzy nejsou importované (Source "Imported" jen u importu souboru) */
    assert_not_imported(2);
    assert_not_imported(3);

    /* stejná analýza podruhé: páska se mezitím změnila */
    st_AN_STATE s = {cmttmz_get_tape_serial(), cmttmz_get_change_serial(), tape_count(), cmttmz_is_modified()};
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_analysis_apply(p, a, use, 0, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    TEST_ASSERT_EQUAL_STRING("The tape changed since the analysis; analyze the block again", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT(s.tape, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(s.count, tape_count());
    ASSERT_NAMES("PRE,#18,ONE,TWO,POST");
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/**
 * @brief Počet výskytů věty CMTTMZ_CLOCK_WARNING v textu.
 * @param text Text (nesmí být NULL). @return Počet.
 */
static int clock_warning_count(const char *text)
{
    int n = 0;
    for (const char *s = strstr(text, CMTTMZ_CLOCK_WARNING); s; s = strstr(s + 1, CMTTMZ_CLOCK_WARNING)) n++;
    return n;
}

/*
 * Blok 0x43 s jiným než výchozím taktem před surovým blokem
 * - nové bloky souborů by hrály tímto taktem: Add i Replace dají varování
 * o taktu jako import, právě jednou; blok 0x43 s výchozím taktem a páska bez
 * bloku 0x43 varování nedají.
 */
void test_apply_clock_warning(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint32_t clocks[3] = {4000000u, MZCMT_FM_DEFAULT_TIMEBASE, 0u};
    for (int c = 0; c < 3; c++) {
        for (int replace = 0; replace < 2; replace++) {
            s_pre_clock = clocks[c];
            open_tape(AN_TAPE_GOOD);
            st_CMTTMZ_PULSES *p = NULL;
            st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
            uint32_t n = 0;
            uint8_t *use = use_all_addable(a, &n);
            TEST_ASSERT_EQUAL_UINT32(2, n);
            uint32_t added = 0;
            TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, replace, &added), cmttmz_get_edit_error());
            TEST_ASSERT_EQUAL_UINT32(2, added);
            const char *w = cmttmz_get_edit_warning();
            char msg[160];
            snprintf(msg, sizeof(msg), "clock %u, %s: '%s'", (unsigned)clocks[c], replace ? "Replace" : "Add", w);
            if (c == 0) TEST_ASSERT_EQUAL_INT_MESSAGE(1, clock_warning_count(w), msg);
            else TEST_ASSERT_EQUAL_STRING_MESSAGE("", w, msg);
            free(use);
            cmttmz_analysis_free(a);
            cmttmz_pulses_free(p);
            tearDown();
            setUp();
        }
    }
}

/**
 * @brief Ověří po Replace obsah a pořadí: surové zbytky nesou původní pulzy, bloky souborů pauzu jako záznam.
 *
 * Bloky 1 .. n - 2 nahradily surový blok a projdou se spolu s řádky: vybraný
 * řádek = blok MZF s pauzou jako u záznamu (1000 ms před dalším blokem
 * souboru, jinak 0; poslední nový blok pauzu původního bloku), běh
 * nevybraných řádků = blok 0x18 s pulzy od prvního do posledního řádku běhu.
 * Délka se nedoplňuje: blok souboru (signál a pauza) je o víc než 5 s
 * kratší než jeho úsek v nahrávce ROM (dvě kopie, plný GAP).
 *
 * @param p Kopie pulzů. @param a Analýza. @param use Výběr. @param expected Očekávané pořadí bloků.
 * @param expect_wav 1 = surové zbytky mají typ WAV.
 * @param others_unsaved Očekávaný příznak neuloženo bloků PRE a POST (nemění se).
 * @param rom_rows 1 = řádky souborů jsou nahrávka ROM (ověří se, že blok souboru není doplněný
 *        na délku úseku a páska je kratší); 0 = syntetické rozsahy řádků (jen obsah a pořadí).
 */
static void check_replace_ex(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use, const char *expected,
                             int expect_wav, int others_unsaved, int rom_rows)
{
    ASSERT_NAMES(expected);
    st_CMTEXT_CONTAINER *c = cmtext_get_container(g_cmt.ext);
    int n = tape_count();
    int post = s_no_post ? 0 : 1; /* blok POST za novými bloky */
    int id = 1;
    uint32_t rows = cmttmz_analysis_row_count(a);
    uint32_t i = 0;
    double sum_us = 0;
    while (i < rows) {
        TEST_ASSERT_TRUE(id < n - post);
        TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(id));
        assert_not_imported(id);
        if (use[i]) {
            const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
            TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_MZF, cmtext_container_get_block_type(c, id));
            /* poslední nový blok: pauza nahrazeného, s blokem za sebou aspoň 1000 ms (soubor nikdy bez mezery) */
            int last_new = (id == n - post - 1);
            uint16_t want = last_new ? ((post && p->pause_ms < 1000) ? 1000 : p->pause_ms) : (use[i + 1] ? 1000 : 0);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(want, block_pause(id), "file block pause like a recording");
            double len = block_length_us(id);
            if (rom_rows)
                TEST_ASSERT_TRUE_MESSAGE(len + 5e6 < (double)r->length_us, "a file block must not be padded to its original length");
            sum_us += len;
            i++;
            id++;
            continue;
        }
        /* běh nevybraných řádků = jeden blok 0x18 s původními pulzy v původním pořadí */
        uint32_t first = cmttmz_analysis_row(a, i)->first_pulse;
        while (i < rows && !use[i]) i++;
        uint32_t end = cmttmz_analysis_row(a, i - 1)->end_pulse;
        if (end == first) continue; /* běh jen z prázdného řádku souboru: surový blok nevznikne */
        uint32_t cnt, rate;
        uint16_t pause;
        uint32_t *pl = block_csw_pulses(id, &cnt, &rate, &pause);
        TEST_ASSERT_EQUAL_UINT32(p->sample_rate, rate);
        uint32_t off = 0;
        if ((p->first_level ^ (int)(first & 1u)) & 1) {
            /* úsek začíná HIGH: předřazený pulz LOW 1 vzorek */
            TEST_ASSERT_EQUAL_UINT32(1, pl[0]);
            off = 1;
        }
        TEST_ASSERT_EQUAL_UINT32(end - first + off, cnt);
        TEST_ASSERT_EQUAL_MEMORY(&p->lengths[first], pl + off, (end - first) * sizeof(uint32_t));
        TEST_ASSERT_EQUAL_UINT16(i == rows ? p->pause_ms : 0, pause);
        TEST_ASSERT_EQUAL_INT(expect_wav ? CMTEXT_BLOCK_TYPE_WAV : CMTEXT_BLOCK_TYPE_TZX,
                              cmtext_container_get_block_type(c, id));
        sum_us += block_length_us(id);
        free(pl);
        id++;
    }
    TEST_ASSERT_EQUAL_INT(n - post, id);
    /* páska je kratší než původní blok (pulzy + pauza) */
    double orig_us = (double)p->total_us + p->pause_ms * 1000.0;
    printf("replace length %.0f us, original %.0f us\n", sum_us, orig_us);
    if (rom_rows) TEST_ASSERT_TRUE(sum_us < orig_us);
    TEST_ASSERT_EQUAL_INT(others_unsaved, cmttmz_block_is_unsaved(0));
    if (post) TEST_ASSERT_EQUAL_INT(others_unsaved, cmttmz_block_is_unsaved(n - 1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
}

/** @brief check_replace_ex() pro řádky z nahrávky ROM (rom_rows 1). */
static void check_replace(const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYSIS *a, const uint8_t *use, const char *expected,
                          int expect_wav, int others_unsaved)
{
    check_replace_ex(p, a, use, expected, expect_wav, others_unsaved, 1);
}

/* Replace se všemi soubory: PRE, šum, ONE, šum, TWO, šum, POST; pulzy surových úseků zachované, délka nedoplněná. */
void test_apply_replace_keeps_every_pulse(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    /* otevřený je surový blok: po nahrazení první nový blok */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_seek_block(1));
    unsigned serial = cmttmz_get_tape_serial();
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(2, added);
    check_replace(p, a, use, "PRE,#18,ONE,#18,TWO,#18,POST", 0, 0);
    assert_block_mzf(2, &s_m1);
    assert_block_mzf(4, &s_m2);
    TEST_ASSERT_NOT_EQUAL_UINT(serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_get_open_block_id());
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Replace jen s TWO: ONE zůstane v surovém běhu spolu se šumem kolem; blok z WAV dá zbytky typu WAV. */
void test_apply_replace_partial_wav(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    /* táž páska jako obraz s příznakem původu WAV u bloku 1 */
    uint8_t *img = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&img, &len));
    const uint8_t wav_flags[3] = {0, 1, 0};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_image(img, len, "", wav_flags, 3));
    g_free(img);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_TYPE_WAV, cmtext_container_get_block_type(cmtext_get_container(g_cmt.ext), 1));

    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    TEST_ASSERT_EQUAL_INT(1, p->wav_type);
    TEST_ASSERT_EQUAL_INT(1, p->unsaved); /* obraz bez cesty = vše neuložené */
    uint8_t *use = (uint8_t *)calloc(cmttmz_analysis_row_count(a), 1);
    int i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i2 >= 0);
    use[i2] = 1;
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(1, added);
    check_replace(p, a, use, "PRE,#18,TWO,#18,POST", 1, 1);
    assert_block_mzf(2, &s_m2);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/*
 * Soubory oddělené jen tichem: neznámé úseky kratší než mez záznamu
 * (CMTTMZ_REC_MIN_EDGES) řádky nejsou a Replace je jako záznam zahodí -
 * páska PRE, ONE, TWO, POST bez surových zbytků mezi soubory.
 */
void test_short_gaps_dropped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_SILENCE);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t gaps = assert_rows_cover(p, a);
    TEST_ASSERT_TRUE_MESSAGE(gaps >= 1, "test needs a left-out unknown part shorter than the threshold");
    uint32_t rows = cmttmz_analysis_row_count(a);
    char expected[128] = "PRE";
    uint8_t *use = (uint8_t *)calloc(rows, 1);
    for (uint32_t i = 0; i < rows; i++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        if (r->kind == CMTTMZ_ANALYZE_UNKNOWN) {
            strcat(expected, ",#18");
        } else {
            TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE, r->result);
            use[i] = 1;
            strcat(expected, ",");
            strcat(expected, r->name);
        }
    }
    strcat(expected, ",POST");
    int i1 = row_by_name(a, "ONE"), i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i1 >= 0 && i2 == i1 + 1); /* ticho mezi soubory řádek nedá */
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(2, added);
    check_replace(p, a, use, expected, 0, 0); /* ONE a TWO za sebou: pauza ONE 1000 ms */
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Řádek, který nejde přidat (neznámý úsek, soubor s chybou), je chyba a páska se nemění. */
void test_apply_refuses_unaddable_row(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_BAD_BODY);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t rows = cmttmz_analysis_row_count(a);
    uint8_t *use = (uint8_t *)calloc(rows, 1);
    int i1 = row_by_name(a, "ONE"), i2 = row_by_name(a, "TWO");
    TEST_ASSERT_TRUE(i1 >= 0 && i2 >= 0);
    st_AN_STATE s = tape_state();
    char want[64];

    use[i1] = 1;
    use[i2] = 1; /* STAYS_RAW */
    uint32_t added = 7;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 0, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    snprintf(want, sizeof(want), "Row %d cannot be added", i2);
    TEST_ASSERT_EQUAL_STRING(want, cmttmz_get_edit_error());
    assert_tape_same(s);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 1, &added));
    assert_tape_same(s);

    memset(use, 0, rows);
    use[0] = 1; /* šum na začátku: neznámý úsek */
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_UNKNOWN, cmttmz_analysis_row(a, 0)->kind);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 1, &added));
    TEST_ASSERT_EQUAL_STRING("Row 0 cannot be added", cmttmz_get_edit_error());
    assert_tape_same(s);

    /* nic nevybráno: úspěch beze změny */
    memset(use, 0, rows);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    assert_tape_same(s);

    /* use NULL při nenulovém počtu řádků je chyba */
    added = 9;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, NULL, 1, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    TEST_ASSERT_EQUAL_STRING("The row selection is missing", cmttmz_get_edit_error());
    assert_tape_same(s);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Jiný tape_serial, jiný blok na stejném čísle, analýza z jiné kopie = chyba, páska beze změny. */
void test_apply_refuses_stale_result(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    st_AN_STATE s = tape_state();
    uint32_t added = 0;

    /* jiný tape_serial (páska se mezitím vyměnila) */
    p->tape_serial++;
    st_CMTTMZ_ANALYSIS *a2 = cmttmz_analyze_pulses(p, NULL);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a2, use, 1, &added));
    TEST_ASSERT_EQUAL_STRING("The tape changed since the analysis; analyze the block again", cmttmz_get_edit_error());
    assert_tape_same(s);
    cmttmz_analysis_free(a2);
    p->tape_serial--;

    /* jiný blok na stejném čísle: kopie hlásí blok 2 (POST, 0x46) */
    p->block_id = 2;
    a2 = cmttmz_analyze_pulses(p, NULL);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a2, use, 0, &added));
    TEST_ASSERT_EQUAL_STRING("Block 2 changed since the analysis; analyze the block again", cmttmz_get_edit_error());
    assert_tape_same(s);
    cmttmz_analysis_free(a2);
    p->block_id = 1;

    /* blok 0x18 s jiným počtem pulzů */
    p->count--;
    a2 = cmttmz_analyze_pulses(p, NULL);
    uint8_t *use2 = use_all_addable(a2, &n);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a2, use2, 0, &added));
    TEST_ASSERT_EQUAL_STRING("Block 1 changed since the analysis; analyze the block again", cmttmz_get_edit_error());
    assert_tape_same(s);
    free(use2);
    cmttmz_analysis_free(a2);
    p->count++;

    /* analýza z jiné kopie (jiný počet pulzů než kopie předaná uplatnění) */
    p->count--;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 0, &added));
    TEST_ASSERT_EQUAL_STRING("The analysis does not belong to these pulses", cmttmz_get_edit_error());
    p->count++;
    assert_tape_same(s);

    /* bez pásky */
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 0, &added));
    TEST_ASSERT_EQUAL_STRING("No tape inserted", cmttmz_get_edit_error());
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* Obal cmt_tape_analysis_apply(): mimo STOP chyba s textem, páska beze změny. */
void test_wrapper_refused_outside_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    st_AN_STATE s = tape_state();
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    uint32_t added = 3;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_analysis_apply(p, a, use, 0, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    TEST_ASSERT_EQUAL_STRING("The tape can be edited only when the deck is stopped", cmttmz_get_edit_error());
    cmt_stop();
    assert_tape_same(s);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/* ================================================================
 * CHYBY KOPIE, OTISK, PAUZY A SYNTETICKÉ VÝSLEDKY
 * ================================================================ */

/**
 * @brief Sestaví data bloku 0x18 (formát st_TZX_FILE) z ručně zadaných bajtů RLE.
 * @param rate Frekvence. @param comp Typ komprese. @param count Počet pulzů v hlavičce.
 * @param data Data RLE. @param n Délka dat.
 * @return Blok (data převezme soubor v open_tape_ex()).
 */
static st_TZX_BLOCK raw_csw(uint32_t rate, uint8_t comp, uint32_t count, const uint8_t *data, uint32_t n)
{
    st_TZX_BLOCK b;
    b.id = TZX_BLOCK_ID_CSW_RECORDING;
    b.length = 14 + n;
    b.data = (uint8_t *)malloc(b.length);
    TEST_ASSERT_NOT_NULL(b.data);
    uint32_t l = 10 + n;
    b.data[0] = (uint8_t)l;
    b.data[1] = (uint8_t)(l >> 8);
    b.data[2] = (uint8_t)(l >> 16);
    b.data[3] = (uint8_t)(l >> 24);
    b.data[4] = 0;
    b.data[5] = 0;
    b.data[6] = (uint8_t)rate;
    b.data[7] = (uint8_t)(rate >> 8);
    b.data[8] = (uint8_t)(rate >> 16);
    b.data[9] = comp;
    b.data[10] = (uint8_t)count;
    b.data[11] = (uint8_t)(count >> 8);
    b.data[12] = (uint8_t)(count >> 16);
    b.data[13] = (uint8_t)(count >> 24);
    if (n) memcpy(b.data + 14, data, n);
    return b;
}

/** @brief Otevře pásku s ručně zadaným blokem 0x18 a ověří, že kopie selže s daným textem. */
static void expect_copy_error(st_TZX_BLOCK raw, const char *text)
{
    open_tape_ex(AN_TAPE_GOOD, &raw);
    TEST_ASSERT_NULL(cmttmz_analyze_copy_pulses(1));
    TEST_ASSERT_EQUAL_STRING(text, cmttmz_get_edit_error());
    cmt_eject();
}

/* Chyby kopie pulzů: Z-RLE, jiná komprese, useknutá DWORD, data navíc, pulz 0, frekvence 0, počet nad délkou dat. */
void test_copy_errors(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint8_t one[1] = {5}, two[2] = {5, 6}, cut[3] = {0, 1, 2}, zero[5] = {0, 0, 0, 0, 0};
    const char *bad = "Block 1 contains invalid signal data";
    expect_copy_error(raw_csw(AN_RATE, TZX_CSW_COMPRESSION_ZRLE, 1, one, 1),
                      "Block 1 uses Z-RLE compression, which is not supported");
    expect_copy_error(raw_csw(AN_RATE, 3, 1, one, 1), bad);
    expect_copy_error(raw_csw(AN_RATE, TZX_CSW_COMPRESSION_RLE, 1, cut, 3), bad);
    expect_copy_error(raw_csw(AN_RATE, TZX_CSW_COMPRESSION_RLE, 1, two, 2), bad);
    expect_copy_error(raw_csw(AN_RATE, TZX_CSW_COMPRESSION_RLE, 1, zero, 5), bad);
    expect_copy_error(raw_csw(0, TZX_CSW_COMPRESSION_RLE, 1, one, 1), bad);
    /* počet pulzů z hlavičky nad délkou dat: chyba před alokací (ne 16 GiB) */
    expect_copy_error(raw_csw(AN_RATE, TZX_CSW_COMPRESSION_RLE, 0xFFFFFFFFu, two, 2), bad);
    /* bez chyby: dva pulzy */
    st_TZX_BLOCK ok = raw_csw(AN_RATE, TZX_CSW_COMPRESSION_RLE, 2, two, 2);
    open_tape_ex(AN_TAPE_GOOD, &ok);
    st_CMTTMZ_PULSES *p = cmttmz_analyze_copy_pulses(1);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(2, p->count);
    TEST_ASSERT_EQUAL_UINT32(16, p->block_length);
    cmttmz_pulses_free(p);
}

/* Data bloku změněná bez změny čítače pásky a počtu pulzů - uplatnění odmítne podle otisku dat. */
void test_apply_refuses_changed_data(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    st_AN_STATE s = tape_state();
    /* první pulz RLE (šum 3 až 60 vzorků, jeden bajt) o vzorek delší: počet pulzů i čítač stejné */
    st_TZX_FILE *f = (st_TZX_FILE *)cmttmz_get_file();
    uint8_t *d = f->blocks[1].data;
    TEST_ASSERT_TRUE(d[14] > 0 && d[14] < 255);
    d[14]++;
    uint32_t added = 4;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 1, &added));
    TEST_ASSERT_EQUAL_UINT32(0, added);
    TEST_ASSERT_EQUAL_STRING("Block 1 changed since the analysis; analyze the block again", cmttmz_get_edit_error());
    assert_tape_same(s);
    d[14]--;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(2, added);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/**
 * @brief Replace všech souborů nad páskou AN_TAPE_SILENCE (ONE, TWO bez surových zbytků).
 * @param expected Očekávané pořadí bloků.
 * @return Pauza bloku TWO (blok 2) v ms.
 */
static uint16_t replace_silence(const char *expected)
{
    open_tape(AN_TAPE_SILENCE);
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    uint32_t n = 0;
    uint8_t *use = use_all_addable(a, &n);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, cmttmz_analysis_row_count(a), "test needs TWO as the last row");
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    check_replace(p, a, use, expected, 0, 0);
    uint16_t pause = block_pause(2);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
    return pause;
}

/*
 * Replace: poslední nový blok souboru má pauzu nahrazeného bloku, s blokem za sebou
 * aspoň 1000 ms (pauza 0 -> 1000, 250 -> 1000, 2500 -> 2500); bez bloku za sebou pauzu
 * nahrazeného (0).
 */
void test_replace_last_file_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    s_raw_pause = 0;
    TEST_ASSERT_EQUAL_UINT16(1000, replace_silence("PRE,ONE,TWO,POST"));
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1)); /* ONE před dalším souborem */
    cmt_eject();
    s_raw_pause = 250; /* kratší než 1000 ms: zvýší se (šev sám jen pauzu 0) */
    TEST_ASSERT_EQUAL_UINT16(1000, replace_silence("PRE,ONE,TWO,POST"));
    cmt_eject();
    s_raw_pause = 2500;
    TEST_ASSERT_EQUAL_UINT16(2500, replace_silence("PRE,ONE,TWO,POST"));
    cmt_eject();
    s_raw_pause = 0;
    s_no_post = 1;
    TEST_ASSERT_EQUAL_UINT16(0, replace_silence("PRE,ONE,TWO"));
}

/* Add: poslední vložený soubor s blokem za sebou 1000 ms (šev), bez bloku za sebou 0. */
void test_add_last_file_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    for (int no_post = 0; no_post < 2; no_post++) {
        s_no_post = no_post;
        open_tape(AN_TAPE_SILENCE);
        st_CMTTMZ_PULSES *p = NULL;
        st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
        uint32_t n = 0;
        uint8_t *use = use_all_addable(a, &n);
        uint32_t added = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
        if (no_post) ASSERT_NAMES("PRE,#18,ONE,TWO");
        else ASSERT_NAMES("PRE,#18,ONE,TWO,POST");
        TEST_ASSERT_EQUAL_UINT16(1000, block_pause(2));
        TEST_ASSERT_EQUAL_UINT16(no_post ? 0 : 1000, block_pause(3));
        free(use);
        cmttmz_analysis_free(a);
        cmttmz_pulses_free(p);
        cmt_eject();
    }
}

/* Šev při nahrazení (cmttmz_replace_block()) - blok 0x46 s pauzou 0 a blokem za sebou dostane 1000 ms. */
void test_replace_seam_branch(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    sp.origin = CMTTMZ_SPEED_ORIGIN_DEFAULT;
    sp.speed = CMTSPEED_1_1;
    sp.pause_ms = 0;
    const char *names[2] = {"NEW", "LAST"};
    for (int k = 0; k < 2; k++) {
        st_TZX_BLOCK *nb = build_small_mzf(names[k], 0);
        st_TZX_BLOCK blocks[1] = {*nb};
        /* k 0: surový blok 1 (za ním POST); k 1: poslední blok POST (za ním nic) */
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_replace_block(1 + k, blocks, &sp, 1, 0), cmttmz_get_edit_error());
        TEST_ASSERT_NULL(blocks[0].data);
        nb->data = NULL;
        tmz_block_free(nb);
    }
    ASSERT_NAMES("PRE,NEW,LAST");
    TEST_ASSERT_EQUAL_UINT16(1000, block_pause(1));
    TEST_ASSERT_EQUAL_UINT16(0, block_pause(2));
}

/** @brief MZF pro syntetický výsledek analyzátoru (hlavička v pořadí hostitele, vlastní tělo). */
static st_MZF *syn_mzf(const st_MZTAPE_MZF *m)
{
    st_MZF *z = (st_MZF *)calloc(1, sizeof(st_MZF));
    TEST_ASSERT_NOT_NULL(z);
    memcpy(&z->header, m->header, sizeof(z->header));
    mzf_header_swap_endianity(&z->header); /* soubor -> hostitel */
    z->body = (uint8_t *)malloc(m->size);
    TEST_ASSERT_NOT_NULL(z->body);
    memcpy(z->body, m->body, m->size);
    z->body_size = m->size;
    return z;
}

/**
 * @brief Přidá soubor do syntetického výsledku analyzátoru (součty OK, bez opravy).
 * @param r Výsledek. @param fmt Formát. @param hlp header_leader_pulse (0 jako u ZX).
 * @param leader_start leader.start_index. @param consumed consumed_until_pulse.
 * @param mzf MZF (převezme výsledek), NULL = záznam ZX s daty TAP.
 */
static void syn_add(st_WAV_ANALYZER_RESULT *r, en_WAV_TAPE_FORMAT fmt, uint32_t hlp, uint32_t leader_start, uint32_t consumed,
                    st_MZF *mzf)
{
    r->files = (st_WAV_ANALYZER_FILE_RESULT *)realloc(r->files, (r->file_count + 1) * sizeof(st_WAV_ANALYZER_FILE_RESULT));
    TEST_ASSERT_NOT_NULL(r->files);
    st_WAV_ANALYZER_FILE_RESULT *f = &r->files[r->file_count++];
    r->file_capacity = r->file_count;
    memset(f, 0, sizeof(*f));
    f->format = fmt;
    f->header_leader_pulse = hlp;
    f->leader.start_index = leader_start;
    f->consumed_until_pulse = consumed;
    f->header_crc = WAV_CRC_OK;
    f->body_crc = WAV_CRC_OK;
    f->mzf = mzf;
    if (!mzf) {
        f->tap_data = (uint8_t *)calloc(1, 4);
        f->tap_data_size = 4;
        f->body_crc = WAV_CRC_NOT_AVAILABLE;
    }
}

/** @brief Ověří řádek: druh, rozsah, výsledek a jméno formátu. */
static void assert_row(const st_CMTTMZ_ANALYSIS *a, uint32_t i, en_CMTTMZ_ANALYZE_ITEM kind, uint32_t first, uint32_t end,
                       en_CMTTMZ_ANALYZE_RESULT result, const char *format)
{
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(kind, r->kind);
    TEST_ASSERT_EQUAL_UINT32(first, r->first_pulse);
    TEST_ASSERT_EQUAL_UINT32(end, r->end_pulse);
    TEST_ASSERT_EQUAL_INT(result, r->result);
    TEST_ASSERT_EQUAL_STRING(format, r->format_name);
}

/**
 * @brief Otevře pásku AN_TAPE_GOOD, zkopíruje blok 1 a vrátí analýzu ze syntetického výsledku.
 * @param p Kopie (výstup). @param res Výsledek (funkce ho spotřebuje).
 */
static st_CMTTMZ_ANALYSIS *syn_analysis(st_CMTTMZ_PULSES **p, st_WAV_ANALYZER_RESULT *res)
{
    *p = cmttmz_analyze_copy_pulses(1);
    TEST_ASSERT_NOT_NULL(*p);
    st_CMTTMZ_ANALYSIS *a = cmttmz_analysis_test_from_result(*p, NULL, res);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_analysis_error(a));
    assert_rows_cover(*p, a);
    return a;
}

/*
 * Syntetický výsledek analyzátoru: záznam ZX bez header_leader_pulse (jako vrací analyzátor)
 * leží za souborem Sharp - řádek ZX pokryje jen svůj rozsah, soubor Sharp před ním má svůj řádek
 * a jde přidat.
 */
void test_rows_zx_does_not_hide_files(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    syn_add(&res, WAV_TAPE_FORMAT_ZX_SPECTRUM, 0, 100000, 120000, NULL); /* pořadí jako u analyzátoru */
    syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 5000, 5000, 60000, syn_mzf(&s_m1));
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
    TEST_ASSERT_EQUAL_UINT32(5, cmttmz_analysis_row_count(a));
    assert_row(a, 0, CMTTMZ_ANALYZE_UNKNOWN, 0, 5000, CMTTMZ_ANALYZE_STAYS_RAW, "UNKNOWN");
    assert_row(a, 1, CMTTMZ_ANALYZE_FILE, 5000, 60000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
    assert_row(a, 2, CMTTMZ_ANALYZE_UNKNOWN, 60000, 100000, CMTTMZ_ANALYZE_STAYS_RAW, "UNKNOWN");
    assert_row(a, 3, CMTTMZ_ANALYZE_FILE, 100000, 120000, CMTTMZ_ANALYZE_CANNOT_ADD, "ZX SPECTRUM");
    assert_row(a, 4, CMTTMZ_ANALYZE_UNKNOWN, 120000, p->count, CMTTMZ_ANALYZE_STAYS_RAW, "UNKNOWN");
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_FORMAT, cmttmz_analysis_row(a, 3)->reason);
    TEST_ASSERT_EQUAL_UINT32(4, cmttmz_analysis_row(a, 3)->size);
    uint8_t use[5] = {0, 1, 0, 0, 0};
    uint32_t added = 0;
    /* první přidaný blok souboru: před ONE zůstane surový blok z řádku 0 */
    TEST_ASSERT_EQUAL_INT(2, cmttmz_analysis_first_file_block(a, use, 1));
    TEST_ASSERT_EQUAL_INT(2, cmttmz_analysis_first_file_block(a, use, 0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    check_replace_ex(p, a, use, "PRE,#18,ONE,#18,POST", 0, 0, 0);
    assert_block_mzf(2, &s_m1);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/*
 * Soubor TURBO na pulzu 0 (header_leader_pulse 0, leader = datový leader za
 * preloaderem) začíná na pulzu 0 - náhradní začátek leader.start_index platí jen pro ZX / SINCLAIR.
 */
void test_rows_turbo_at_pulse_zero(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    syn_add(&res, WAV_TAPE_FORMAT_TURBO, 0, 3000, 50000, syn_mzf(&s_m1));
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
    TEST_ASSERT_EQUAL_UINT32(2, cmttmz_analysis_row_count(a));
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, 0);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_FILE, r->kind);
    TEST_ASSERT_EQUAL_UINT32(0, r->first_pulse);
    TEST_ASSERT_EQUAL_UINT32(50000, r->end_pulse);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_UNKNOWN, cmttmz_analysis_row(a, 1)->kind);
    TEST_ASSERT_EQUAL_UINT32(50000, cmttmz_analysis_row(a, 1)->first_pulse);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/*
 * Syntetický výsledek: soubor B začíná před koncem A, soubor M leží celý uvnitř B.
 * Řádky: A, B od konce A, M s prázdným rozsahem; B jde přidat sám (A zůstane surový i s
 * překryvem), A a B spolu (z překryvu nic surového), M sám (A a B surové).
 */
void test_rows_overlap_kept(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    /* čtvrtý případ: jen A - překryv a B, M zůstanou v jednom surovém bloku za A */
    static const uint8_t sel[4][5] = {{0, 0, 1, 0, 0}, {0, 1, 1, 0, 0}, {0, 0, 0, 1, 0}, {0, 1, 0, 0, 0}};
    static const char *names[4] = {"PRE,#18,TWO,#18,POST", "PRE,#18,ONE,TWO,#18,POST", "PRE,#18,ONE,#18,POST",
                                   "PRE,#18,ONE,#18,POST"};
    for (int k = 0; k < 4; k++) {
        open_tape(AN_TAPE_GOOD);
        st_WAV_ANALYZER_RESULT res;
        memset(&res, 0, sizeof(res));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 2000, 2000, 50000, syn_mzf(&s_m1));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 49900, 49900, 90000, syn_mzf(&s_m2));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 60000, 60000, 70000, syn_mzf(&s_m1));
        st_CMTTMZ_PULSES *p = NULL;
        st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
        TEST_ASSERT_EQUAL_UINT32(5, cmttmz_analysis_row_count(a));
        assert_row(a, 0, CMTTMZ_ANALYZE_UNKNOWN, 0, 2000, CMTTMZ_ANALYZE_STAYS_RAW, "UNKNOWN");
        assert_row(a, 1, CMTTMZ_ANALYZE_FILE, 2000, 50000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
        assert_row(a, 2, CMTTMZ_ANALYZE_FILE, 50000, 90000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
        assert_row(a, 3, CMTTMZ_ANALYZE_FILE, 90000, 90000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
        assert_row(a, 4, CMTTMZ_ANALYZE_UNKNOWN, 90000, p->count, CMTTMZ_ANALYZE_STAYS_RAW, "UNKNOWN");
        TEST_ASSERT_EQUAL_UINT64(cmttmz_analysis_row(a, 3)->start_us + 0, cmttmz_analysis_row(a, 4)->start_us);
        TEST_ASSERT_EQUAL_UINT64(0, cmttmz_analysis_row(a, 3)->length_us);
        const int first = cmttmz_analysis_first_file_block(a, sel[k], 1);
        uint32_t added = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, sel[k], 1, &added), cmttmz_get_edit_error());
        check_replace_ex(p, a, sel[k], names[k], 0, 0, 0);
        /* první přidaný blok souboru je blok souboru za surovým blokem s řádkem 0 */
        TEST_ASSERT_EQUAL_INT(2, first);
        assert_block_mzf(first, (k == 0) ? &s_m2 : &s_m1);
        if (k == 3) {
            /* jen A: surový blok za A začíná koncem A (pulz 50 000), překryv [49 900, 50 000) zastupuje A */
            const st_TZX_BLOCK *rb = &cmttmz_get_file()->blocks[3];
            TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, rb->id);
        }
        cmttmz_analysis_free(a);
        cmttmz_pulses_free(p);
        cmt_eject();
    }
}

/*
 * cmttmz_analysis_first_file_block(): soubor na začátku bloku
 * vybraný = Replace začne blokem souboru (bez surového bloku před ním), Add
 * za surovým blokem; bez výběru -1; jen druhý soubor = za surovým zbytkem.
 */
void test_first_file_block(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint8_t sel[2][4] = {{1, 0, 1, 0}, {0, 0, 1, 0}};
    static const int first_replace[2] = {1, 2};
    for (int k = 0; k < 2; k++) {
        open_tape(AN_TAPE_GOOD);
        st_WAV_ANALYZER_RESULT res;
        memset(&res, 0, sizeof(res));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 0, 0, 50000, syn_mzf(&s_m1));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 60000, 60000, 90000, syn_mzf(&s_m2));
        st_CMTTMZ_PULSES *p = NULL;
        st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
        TEST_ASSERT_EQUAL_UINT32(4, cmttmz_analysis_row_count(a));
        assert_row(a, 0, CMTTMZ_ANALYZE_FILE, 0, 50000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
        assert_row(a, 2, CMTTMZ_ANALYZE_FILE, 60000, 90000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
        static const uint8_t none[4] = {0, 0, 0, 0};
        TEST_ASSERT_EQUAL_INT(-1, cmttmz_analysis_first_file_block(a, none, 1));
        TEST_ASSERT_EQUAL_INT(-1, cmttmz_analysis_first_file_block(a, NULL, 1));
        TEST_ASSERT_EQUAL_INT(2, cmttmz_analysis_first_file_block(a, sel[k], 0));
        const int first = cmttmz_analysis_first_file_block(a, sel[k], 1);
        TEST_ASSERT_EQUAL_INT(first_replace[k], first);
        uint32_t added = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, sel[k], 1, &added), cmttmz_get_edit_error());
        assert_block_mzf(first, (k == 0) ? &s_m1 : &s_m2);
        cmttmz_analysis_free(a);
        cmttmz_pulses_free(p);
        cmt_eject();
    }
}

/*
 * Syntetický výsledek: krátký vynechaný úsek mezi dvěma řádky, které oba
 * zůstávají surové, zůstane uvnitř jejich společného surového bloku.
 */
void test_raw_run_keeps_inner_gap(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 1000, 1000, 30000, syn_mzf(&s_m1));
    syn_add(&res, WAV_TAPE_FORMAT_TURBO, 30010, 30010, 60000, syn_mzf(&s_m2)); /* mezera 10 pulzů */
    syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 60000, 60000, 90000, syn_mzf(&s_m2));
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
    TEST_ASSERT_EQUAL_UINT32(5, cmttmz_analysis_row_count(a));
    assert_row(a, 1, CMTTMZ_ANALYZE_FILE, 1000, 30000, CMTTMZ_ANALYZE_ADDABLE, "NORMAL");
    assert_row(a, 2, CMTTMZ_ANALYZE_FILE, 30010, 60000, CMTTMZ_ANALYZE_ADDABLE, "TURBO");
    TEST_ASSERT_EQUAL_STRING("MZ Turbo Data", cmttmz_analysis_row(a, 2)->writer_profile);
    uint8_t use[5] = {0, 0, 0, 1, 0};
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
    /* první surový blok = pulzy 0 .. 60000 včetně mezery 30000 .. 30010 */
    check_replace_ex(p, a, use, "PRE,#18,TWO,#18,POST", 0, 0, 0);
    uint32_t cnt, rate;
    uint16_t pause;
    uint32_t *pl = block_csw_pulses(1, &cnt, &rate, &pause);
    TEST_ASSERT_EQUAL_UINT32(60000, cnt);
    free(pl);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/*
 * Syntetický výsledek, mez snížená testovacím háčkem: surový zbytek začínající HIGH
 * s předřazeným pulzem nesmí přesáhnout mez playeru - na hranici o 1 pulz Replace odmítne
 * s textem a páska se nezmění; s mezí o 1 vyšší projde.
 */
void test_replace_raw_limit(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    for (int k = 0; k < 2; k++) {
        open_tape(AN_TAPE_GOOD);
        st_WAV_ANALYZER_RESULT res;
        memset(&res, 0, sizeof(res));
        syn_add(&res, WAV_TAPE_FORMAT_NORMAL, 5000, 5000, 60001, syn_mzf(&s_m1)); /* zbytek od lichého pulzu = HIGH */
        st_CMTTMZ_PULSES *p = NULL;
        st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
        uint32_t tail = p->count - 60001; /* pulzy zbytku bez předřazeného */
        cmttmz_analysis_test_set_max_pulses(k ? tail + 1 : tail);
        uint8_t use[3] = {0, 1, 0};
        st_AN_STATE s = tape_state();
        uint32_t added = 0;
        if (!k) {
            TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_apply(p, a, use, 1, &added));
            TEST_ASSERT_EQUAL_STRING("Block 1 cannot be split: a raw part would be longer than a playable block",
                                     cmttmz_get_edit_error());
            assert_tape_same(s);
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_analysis_apply(p, a, use, 1, &added), cmttmz_get_edit_error());
            check_replace_ex(p, a, use, "PRE,#18,ONE,#18,POST", 0, 0, 0);
        }
        cmttmz_analysis_free(a);
        cmttmz_pulses_free(p);
        cmt_eject();
    }
}

/* ================================================================
 * HISTOGRAM
 * ================================================================ */

/* Histogram: součet binů = počet započtených pulzů, omezení počtu, nejdelší pulz, neplatné parametry. */
void test_histogram(void)
{
    enum { N = 300000 };
    st_CMTTMZ_PULSES p;
    memset(&p, 0, sizeof(p));
    p.sample_rate = 1000000; /* 1 vzorek = 1 µs */
    p.count = N;
    p.lengths = (uint32_t *)malloc(N * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(p.lengths);
    for (uint32_t i = 0; i < N; i++) p.lengths[i] = (i & 1u) ? 21 : 11;
    p.lengths[N - 1] = 5000;

    uint32_t bins[8];
    double max_us = 0;
    /* malý rozsah: všechny pulzy */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_histogram(&p, 10, 110, 5.0, bins, 8, &max_us));
    uint64_t sum = 0;
    for (int k = 0; k < 8; k++) sum += bins[k];
    TEST_ASSERT_EQUAL_UINT64(100, sum);
    TEST_ASSERT_EQUAL_UINT32(50, bins[2]); /* 11 µs */
    TEST_ASSERT_EQUAL_UINT32(50, bins[4]); /* 21 µs */
    TEST_ASSERT_EQUAL_DOUBLE(21.0, max_us);

    /* celý blok: nejvýš CMTTMZ_ANALYZE_HIST_MAX_PULSES pulzů, dlouhý pulz do posledního binu */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_histogram(&p, 0, N, 5.0, bins, 8, &max_us));
    sum = 0;
    for (int k = 0; k < 8; k++) sum += bins[k];
    TEST_ASSERT_EQUAL_UINT64(CMTTMZ_ANALYZE_HIST_MAX_PULSES, sum);
    TEST_ASSERT_TRUE(bins[2] > 90000 && bins[4] > 90000);
    /* výběr j * n / max pro j < max nezahrne poslední pulz, nejdelší pulz je ale přesně z celého rozsahu */
    TEST_ASSERT_EQUAL_UINT32(0, bins[7]);
    TEST_ASSERT_EQUAL_DOUBLE(5000.0, max_us);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_histogram(&p, N - 2, N, 5.0, bins, 8, &max_us));
    TEST_ASSERT_EQUAL_UINT32(1, bins[7]);
    TEST_ASSERT_EQUAL_DOUBLE(5000.0, max_us);

    /* jen nejdelší pulz */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_analysis_histogram(&p, 0, 100, 5.0, NULL, 0, &max_us));
    TEST_ASSERT_EQUAL_DOUBLE(21.0, max_us);

    /* neplatné parametry */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(NULL, 0, 1, 5.0, bins, 8, &max_us));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(&p, 5, 5, 5.0, bins, 8, &max_us));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(&p, 0, N + 1, 5.0, bins, 8, &max_us));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(&p, 0, 10, 0.0, bins, 8, &max_us));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_analysis_histogram(&p, 0, 10, 5.0, NULL, 8, &max_us));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, max_us);
    free(p.lengths);
}


/* ================================================================
 * JINÉ FORMÁTY NEŽ NORMAL (TURBO, FASTIPL, BSD, CP/M)
 * ================================================================ */

/** @brief Délka těla souboru v testech jiných formátů (B). */
#define FMT_BODY_SIZE 700

/**
 * @brief Zahraje blok playerem při AN_RATE a vrátí jeho signál jako blok 0x18 s tichem před a za.
 *
 * První pulz CSW je LOW: začíná-li signál úrovní HIGH, ticho před ním je
 * právě ten pulz LOW; jinak se ticho k prvnímu pulzu přičte.
 *
 * @param b Blok se signálem. @return Blok 0x18 (data převezme soubor v open_tape_ex()).
 */
static st_TZX_BLOCK fmt_block_to_raw(const st_TZX_BLOCK *b)
{
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = AN_RATE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *st = tmz_player_play_block(b, &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(st, tmz_player_error_string(perr));
    st_CMT_VSTREAM *vs = st->str.vstream;
    cmt_vstream_read_reset(vs);
    st_AN_SIG sig;
    memset(&sig, 0, sizeof(sig));
    sig_add(&sig, 0, AN_RATE / 2); /* 0,5 s ticha */
    uint64_t samples = 0;
    int value = 0;
    while (EXIT_SUCCESS == cmt_vstream_read_pulse(vs, &samples, &value)) {
        while (samples > 0) {
            uint32_t n = (samples > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (uint32_t)samples;
            sig_add(&sig, value & 1, n);
            samples -= n;
        }
    }
    sig_add(&sig, sig_next_level(&sig), AN_RATE / 2);
    cmt_stream_destroy(st);
    st_TZX_BLOCK raw;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_csw_recording(AN_RATE, AN_RAW_PAUSE_MS, sig.len, sig.count, &raw));
    free(sig.len);
    return raw;
}

/**
 * @brief Hlavička a tělo testovacího souboru jiného formátu.
 * @param[out] hdr Hlavička v pořadí hostitele. @param[out] body Tělo FMT_BODY_SIZE bajtů.
 * @param name Jméno. @param ftype Typ souboru.
 */
static void fmt_make_file(st_MZF_HEADER *hdr, uint8_t *body, const char *name, uint8_t ftype)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = ftype;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, strlen(name));
    hdr->fsize = FMT_BODY_SIZE;
    hdr->fstrt = 0x2000;
    hdr->fexec = 0x2000;
    uint32_t x = 0x1234u + (uint32_t)name[0];
    for (int i = 0; i < FMT_BODY_SIZE; i++) {
        x = x * 1103515245u + 12345u;
        body[i] = (uint8_t)(x >> 16);
    }
}

/**
 * @brief Kruh jednoho formátu: blok profilu zahraný do surového bloku analýza najde a přidá se stejným obsahem.
 *
 * @param profile ID profilu, kterým se sestaví zdrojový blok. @param args Parametry profilu.
 * @param name Jméno souboru. @param format Očekávané jméno formátu řádku.
 * @param writer Očekávané st_CMTTMZ_ANALYZE_ROW::writer_profile.
 */
static void fmt_roundtrip(const char *profile, const st_TMZ_PROFILE_ARGS *args, const char *name, const char *format,
                          const char *writer)
{
    st_MZF_HEADER hdr;
    uint8_t body[FMT_BODY_SIZE];
    fmt_make_file(&hdr, body, name, 0x01);
    en_TMZ_PROFILE_ERROR pe = TMZ_PROFILE_OK;
    st_TZX_BLOCK *src = tmz_profile_build_block(profile, args, &hdr, body, FMT_BODY_SIZE, &pe);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, tmz_profile_error_string(pe));
    st_TZX_BLOCK raw = fmt_block_to_raw(src);
    tmz_block_free(src);
    open_tape_ex(AN_TAPE_GOOD, &raw);

    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", cmttmz_analysis_error(a), profile);
    int row = -1;
    uint32_t rows = cmttmz_analysis_row_count(a);
    for (uint32_t i = 0; i < rows; i++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        if (r->kind == CMTTMZ_ANALYZE_FILE && 0 == strcmp(r->format_name, format)) row = (int)i;
    }
    TEST_ASSERT_TRUE_MESSAGE(row >= 0, profile);
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)row);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(name, r->name, profile);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTTMZ_ANALYZE_ADDABLE, r->result, cmttmz_analyze_reason_text(r->reason));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(writer, r->writer_profile, profile);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FMT_BODY_SIZE, r->size, profile);

    uint8_t *use = (uint8_t *)calloc(rows, 1);
    TEST_ASSERT_NOT_NULL(use);
    use[row] = 1;
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(1, added);
    TEST_ASSERT_EQUAL_INT(4, tape_count());
    /* nový blok za surovým nese stejný soubor */
    st_MZF *mzf = cmttmz_block_get_mzf(2);
    TEST_ASSERT_NOT_NULL_MESSAGE(mzf, profile);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(hdr.ftype, mzf->header.ftype, profile);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&hdr.fname, &mzf->header.fname, sizeof(hdr.fname), profile);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(hdr.fsize, mzf->header.fsize, profile);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(hdr.fstrt, mzf->header.fstrt, profile);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(hdr.fexec, mzf->header.fexec, profile);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(body, mzf->body, FMT_BODY_SIZE, profile);
    mzf_free(mzf);
    /* a jde přehrát */
    TEST_ASSERT_TRUE_MESSAGE(block_length_us(2) > 100000.0, profile);
    assert_not_imported(2);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/**
 * @brief Kruh datového souboru BASIC (BSD): tři kusy po 256 B, blok profilu BASIC-1Z016.
 */
static void fmt_roundtrip_bsd(void)
{
    enum { CHUNKS = 3 };
    st_MZF_HEADER hdr;
    uint8_t body[FMT_BODY_SIZE];
    fmt_make_file(&hdr, body, "BSDDATA", 0x04);
    hdr.fsize = 0;
    hdr.fstrt = 0;
    hdr.fexec = 0;
    uint8_t chunks[CHUNKS * MZCMT_BSD_CHUNK_SIZE];
    memset(chunks, 0, sizeof(chunks));
    for (int i = 0; i < CHUNKS; i++) {
        uint16_t id = (i + 1 == CHUNKS) ? 0xFFFFu : (uint16_t)i;
        chunks[i * MZCMT_BSD_CHUNK_SIZE] = (uint8_t)(id & 0xFF);
        chunks[i * MZCMT_BSD_CHUNK_SIZE + 1] = (uint8_t)(id >> 8);
        memcpy(&chunks[i * MZCMT_BSD_CHUNK_SIZE + 2], &body[(i * 97) % 400], MZCMT_BSD_CHUNK_DATA_SIZE);
    }
    en_TMZ_PROFILE_ERROR pe = TMZ_PROFILE_OK;
    st_TZX_BLOCK *src = tmz_profile_build_block("BASIC-1Z016", NULL, &hdr, chunks, sizeof(chunks), &pe);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, tmz_profile_error_string(pe));
    st_TZX_BLOCK raw = fmt_block_to_raw(src);
    tmz_block_free(src);
    open_tape_ex(AN_TAPE_GOOD, &raw);

    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = analyze_block1(&p, NULL);
    int row = row_by_name(a, "BSDDATA");
    TEST_ASSERT_TRUE_MESSAGE(row >= 0, "BSD file found");
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)row);
    TEST_ASSERT_EQUAL_STRING("BSD", r->format_name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CMTTMZ_ANALYZE_ADDABLE, r->result, cmttmz_analyze_reason_text(r->reason));
    TEST_ASSERT_EQUAL_STRING("BASIC-1Z016", r->writer_profile);
    uint32_t rows = cmttmz_analysis_row_count(a);
    uint8_t *use = (uint8_t *)calloc(rows, 1);
    TEST_ASSERT_NOT_NULL(use);
    use[row] = 1;
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(1, added);
    /* nový blok 0x46 nese všechny kusy beze změny (ID i data) a jde přehrát */
    const st_TZX_BLOCK *nb = &cmttmz_get_file()->blocks[2];
    TEST_ASSERT_EQUAL_UINT8(TMZ_BLOCK_ID_MZ_FILE, nb->id);
    for (int i = 0; i < CHUNKS; i++) {
        const uint8_t *chunk = &chunks[i * MZCMT_BSD_CHUNK_SIZE];
        int found = 0;
        for (uint32_t o = 0; !found && o + MZCMT_BSD_CHUNK_SIZE <= nb->length; o++) {
            found = (0 == memcmp(nb->data + o, chunk, MZCMT_BSD_CHUNK_SIZE));
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "BSD chunk kept in the new block");
    }
    TEST_ASSERT_TRUE(block_length_us(2) > 100000.0);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

/*
 * Soubory TURBO, FASTIPL, CP/M a BSD z analýzy jdou přidat jako bloky:
 * blok profilu zapisovače zahraný do surového záznamu analýza najde, pozná
 * zapisovač a přidá blok se stejným souborem.
 */
void test_formats_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TMZ_PROFILE_ARGS a;

    tmz_profile_args_init(&a);
    a.speed = CMTSPEED_2_1;
    fmt_roundtrip("TC-1.21", &a, "TCTURBO", "TURBO", "TC-1.21");

    tmz_profile_args_init(&a);
    a.bd = 2400;
    fmt_roundtrip("IC-10.2-TURBO", &a, "ICTURBO", "TURBO", "IC-10.2-TURBO");

    tmz_profile_args_init(&a);
    a.bd = 2400;
    fmt_roundtrip("IC-10.2-FASTIPL", &a, "ICFAST", "FASTIPL", "IC-10.2-FASTIPL");

    tmz_profile_args_init(&a);
    a.bd = 2400;
    fmt_roundtrip("PEZIK-2.0A", &a, "PEZIK", "CPM-TAPE", "PEZIK-2.0A");

    tmz_profile_args_init(&a);
    a.bd = 2400;
    fmt_roundtrip("IC-10.2-CPM", &a, "ICCPM", "CPM-TAPE", "IC-10.2-CPM");

    fmt_roundtrip_bsd();
}

/*
 * Formát, který jako blok sestavit nejde (ZX Spectrum, FSK), zůstává
 * CANNOT_ADD s důvodem FORMAT a bez profilu; soubor CP/M Pezik s tělem nad
 * 16 KB profil odmítne (CANNOT_ADD, důvod PROFILE).
 */
void test_formats_not_buildable(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    syn_add(&res, WAV_TAPE_FORMAT_FSK, 1000, 1000, 30000, syn_mzf(&s_m1));
    syn_add(&res, WAV_TAPE_FORMAT_CPM_TAPE, 40000, 40000, 70000, syn_mzf(&s_m2));
    /* tělo Pezik nad 16 KB */
    st_MZF *big = res.files[1].mzf;
    free(big->body);
    big->body = (uint8_t *)calloc(1, 20000);
    TEST_ASSERT_NOT_NULL(big->body);
    big->body_size = 20000;
    big->header.fsize = 20000;
    res.files[1].cpmtape_variant = WAV_CPMTAPE_PEZIK;
    res.files[1].cpmtape_bd = 2400;
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
    int fsk = -1, cpm = -1;
    for (uint32_t i = 0; i < cmttmz_analysis_row_count(a); i++) {
        const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, i);
        if (r->kind != CMTTMZ_ANALYZE_FILE) continue;
        if (0 == strcmp(r->format_name, "FSK")) fsk = (int)i;
        if (0 == strcmp(r->format_name, "CPM-TAPE")) cpm = (int)i;
    }
    TEST_ASSERT_TRUE(fsk >= 0 && cpm >= 0);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CANNOT_ADD, cmttmz_analysis_row(a, (uint32_t)fsk)->result);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_FORMAT, cmttmz_analysis_row(a, (uint32_t)fsk)->reason);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_analysis_row(a, (uint32_t)fsk)->writer_profile);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_CANNOT_ADD, cmttmz_analysis_row(a, (uint32_t)cpm)->result);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_REASON_PROFILE, cmttmz_analysis_row(a, (uint32_t)cpm)->reason);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}


/*
 * CPM-CMT (standardní formát při 2400 Bd, CMT.COM): soubor jde přidat jako
 * blok MZ800EMU-CMT s pevnou rychlostí 2:1 CP/M.
 */
void test_formats_cpm_cmt(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_tape(AN_TAPE_GOOD);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    syn_add(&res, WAV_TAPE_FORMAT_CPM_CMT, 1000, 1000, 30000, syn_mzf(&s_m1));
    st_CMTTMZ_PULSES *p = NULL;
    st_CMTTMZ_ANALYSIS *a = syn_analysis(&p, &res);
    int row = -1;
    for (uint32_t i = 0; i < cmttmz_analysis_row_count(a); i++) {
        if (cmttmz_analysis_row(a, i)->kind == CMTTMZ_ANALYZE_FILE) row = (int)i;
    }
    TEST_ASSERT_TRUE(row >= 0);
    const st_CMTTMZ_ANALYZE_ROW *r = cmttmz_analysis_row(a, (uint32_t)row);
    TEST_ASSERT_EQUAL_STRING("CPM-CMT", r->format_name);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_ANALYZE_ADDABLE, r->result);
    TEST_ASSERT_EQUAL_STRING("MZ800EMU-CMT", r->writer_profile);
    uint8_t *use = (uint8_t *)calloc(cmttmz_analysis_row_count(a), 1);
    TEST_ASSERT_NOT_NULL(use);
    use[row] = 1;
    uint32_t added = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmt_tape_analysis_apply(p, a, use, 0, &added), cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_UINT32(1, added);
    assert_block_mzf(2, &s_m1);
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(2, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1_CPM, sp.speed);
    free(use);
    cmttmz_analysis_free(a);
    cmttmz_pulses_free(p);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_opts_default);
    RUN_TEST(test_copy_pulses);
    RUN_TEST(test_analyze_rows_cover_block);
    RUN_TEST(test_analyze_corrupted_body_stays_raw);
    RUN_TEST(test_analyze_repair_body);
    RUN_TEST(test_empty_block);
    RUN_TEST(test_apply_add);
    RUN_TEST(test_apply_clock_warning);
    RUN_TEST(test_apply_replace_keeps_every_pulse);
    RUN_TEST(test_apply_replace_partial_wav);
    RUN_TEST(test_short_gaps_dropped);
    RUN_TEST(test_apply_refuses_unaddable_row);
    RUN_TEST(test_apply_refuses_stale_result);
    RUN_TEST(test_wrapper_refused_outside_stop);
    RUN_TEST(test_copy_errors);
    RUN_TEST(test_apply_refuses_changed_data);
    RUN_TEST(test_replace_last_file_pause);
    RUN_TEST(test_add_last_file_pause);
    RUN_TEST(test_replace_seam_branch);
    RUN_TEST(test_rows_zx_does_not_hide_files);
    RUN_TEST(test_rows_turbo_at_pulse_zero);
    RUN_TEST(test_rows_overlap_kept);
    RUN_TEST(test_first_file_block);
    RUN_TEST(test_raw_run_keeps_inner_gap);
    RUN_TEST(test_replace_raw_limit);
    RUN_TEST(test_histogram);
    RUN_TEST(test_formats_roundtrip);
    RUN_TEST(test_formats_not_buildable);
    RUN_TEST(test_formats_cpm_cmt);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
