/**
 * @file test_cmt_tmz_record.c
 * @brief Testy bufferu hran záznamu Virtual CMT (cmt_tmz_record.h).
 *
 * Testuje se:
 *   - tři hrany dají dva úseky se správnými délkami a first_level,
 *     total_ticks je jejich součet, úroveň PC1 z begin,
 *   - hrana se stejnou úrovní jako předchozí (i jako PC1 při begin) se
 *     ignoruje,
 *   - nerostoucí čas hrany se odmítne (úsek délky 0 nevznikne, úroveň se
 *     nezmění), první hrana smí mít čas 0,
 *   - mez počtu položek a mez délky záznamu (zmenšené přes
 *     cmttmz_rec_test_set_limits()): limit_reached() vrací 1, další hrany se
 *     zahazují a buffer už neroste,
 *   - úsek delší než UINT32_MAX taktů se rozdělí úsekem nulové délky opačné
 *     úrovně a dělení se počítá do meze položek,
 *   - růst bufferu přes první blok kapacity (data zůstanou celá),
 *   - begin po discard (i po dosažení meze) začíná čistě, hrany bez
 *     aktivního záznamu se ignorují,
 *   - dekódování (cmttmz_rec_decode()): signál ROM z mztape s kopiemi dá
 *     blok 0x46 s bajtově shodným MZF, dva soubory dva bloky v pořadí,
 *     20 hran žádný blok, 5000 náhodných hran jeden surový blok 0x18
 *     (přehrání: délky do 1 vzorku), useknuté tělo jen surový blok,
 *     polarita surového bloku (předřazený LOW u začátku HIGH), NULL parametry,
 *   - hranice surových úseků (šum před souborem, dobrý - poškozený - dobrý,
 *     soubor a šum za ním) přesně podle header_leader_pulse a
 *     consumed_until_pulse nezávisle spuštěného analyzátoru, pauzy bloků,
 *     záloha celého záznamu jako surového bloku při selhání analýzy, úspěch
 *     bez info_out.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <stdint.h>
#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#include "hw-generic/cmt/cmt_tmz_record.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/pio8255/pio8255.h"
#include "libs/mztape/mztape.h"
#include "libs/mzf/mzf.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_player.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/cmt_stream/cmt_vstream.h"
#include "libs/wav_analyzer/wav_analyzer.h"

void setUp(void)
{
    cmttmz_rec_test_set_limits(0, 0);
    cmttmz_rec_discard();
}

void tearDown(void)
{
    cmttmz_rec_test_fail_analysis(0);
    cmttmz_rec_discard();
    cmttmz_rec_test_set_limits(0, 0);
    g_pio8255.signal_pc01 = 0;
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/*
 * PC1 = 1 při begin; hrany 0 @100, 1 @250, 0 @1000 dají úseky 150 (úroveň 0)
 * a 750 (úroveň 1). Doba od begin do první hrany úsekem není.
 */
void test_three_edges_two_segments(void)
{
    g_pio8255.signal_pc01 = 1;
    cmttmz_rec_begin();
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT32(0, rec->count);
    TEST_ASSERT_EQUAL_INT(1, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(0, rec->total_ticks);

    cmttmz_rec_edge(100, 0);
    cmttmz_rec_edge(250, 1);
    cmttmz_rec_edge(1000, 0);

    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(2, rec->count);
    TEST_ASSERT_NOT_NULL(rec->ticks);
    TEST_ASSERT_EQUAL_UINT32(150, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(750, rec->ticks[1]);
    TEST_ASSERT_EQUAL_INT(0, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(900, rec->total_ticks);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
}

/*
 * Hrana se stejnou úrovní jako PC1 při begin i jako předchozí hrana se
 * ignoruje (nevznikne úsek, čas se nepřepíše).
 */
void test_same_level_edge_ignored(void)
{
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_get()->first_level);

    cmttmz_rec_edge(50, 0);   /* stejná jako PC1 při begin */
    cmttmz_rec_edge(100, 1);  /* první přijatá hrana */
    cmttmz_rec_edge(180, 1);  /* stejná jako předchozí */
    cmttmz_rec_edge(300, 0);
    cmttmz_rec_edge(400, 2);  /* bit 0 = 0, stejná jako předchozí */

    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(1, rec->count);
    TEST_ASSERT_EQUAL_UINT32(200, rec->ticks[0]);
    TEST_ASSERT_EQUAL_INT(1, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(200, rec->total_ticks);
}

/*
 * Čas rovný času předchozí hrany nebo menší se odmítne: úsek 0 nevznikne
 * a úroveň zůstane, takže další hrana téže úrovně jako odmítnutá platí.
 * První hrana smí mít čas 0.
 */
void test_non_increasing_time_rejected(void)
{
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    cmttmz_rec_edge(0, 1);    /* první hrana v čase 0 se přijme */
    cmttmz_rec_edge(0, 0);    /* stejný čas: odmítnuto */
    cmttmz_rec_edge(500, 0);  /* úsek 500 úrovně 1 */
    cmttmz_rec_edge(400, 1);  /* klesající čas: odmítnuto */
    cmttmz_rec_edge(500, 1);  /* stejný čas: odmítnuto */
    cmttmz_rec_edge(800, 1);  /* úsek 300 úrovně 0 */

    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(2, rec->count);
    TEST_ASSERT_EQUAL_UINT32(500, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(300, rec->ticks[1]);
    TEST_ASSERT_EQUAL_INT(1, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(800, rec->total_ticks);
    for (uint32_t i = 0; i < rec->count; i++)
        TEST_ASSERT_NOT_EQUAL(0, rec->ticks[i]);
}

/*
 * Mez položek zmenšená na 10: po 10 úsecích limit_reached() = 1, další
 * hrany se zahazují a buffer už neroste (count, total_ticks i ticks beze
 * změny).
 */
void test_edge_limit(void)
{
    cmttmz_rec_test_set_limits(10, 0);
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    uint64_t t = 1000;
    int level = 1;
    cmttmz_rec_edge(t, level); /* první hrana: bez úseku */
    for (int i = 0; i < 10; i++)
    {
        t += 100;
        level ^= 1;
        cmttmz_rec_edge(t, level);
        TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
    }
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(10, rec->count);
    const uint32_t *ticks_before = rec->ticks;

    for (int i = 0; i < 1000; i++)
    {
        t += 100;
        level ^= 1;
        cmttmz_rec_edge(t, level);
        TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
    }
    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(10, rec->count);
    TEST_ASSERT_EQUAL_UINT64(1000, rec->total_ticks);
    TEST_ASSERT_EQUAL_PTR(ticks_before, rec->ticks);
    for (uint32_t i = 0; i < rec->count; i++)
        TEST_ASSERT_EQUAL_UINT32(100, rec->ticks[i]);
}

/*
 * Mez délky záznamu zmenšená na 10 000 taktů: hrana v čase 10 000 se
 * přijme, hrana za mezí nastaví příznak a další hrany (i v čase pod mezí
 * po zpětném skoku) se zahazují.
 */
void test_time_limit(void)
{
    cmttmz_rec_test_set_limits(0, 10000);
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    cmttmz_rec_edge(1000, 1);
    cmttmz_rec_edge(10000, 0);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
    cmttmz_rec_edge(10001, 1);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
    cmttmz_rec_edge(20000, 0);

    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(1, rec->count);
    TEST_ASSERT_EQUAL_UINT32(9000, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT64(9000, rec->total_ticks);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
}

/*
 * Výchozí mez délky je CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE: hrana přesně
 * na mezi se přijme, o takt dál nastaví příznak. Úsek delší než UINT32_MAX
 * se rozdělí (UINT32_MAX, 0, zbytek) a součet zůstane přesný.
 */
void test_default_time_limit_and_split(void)
{
    const uint64_t max_ticks = (uint64_t)CMTTMZ_REC_MAX_SECONDS * GDGCLK_BASE;
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    cmttmz_rec_edge(0, 1);
    uint64_t long_len = (uint64_t)UINT32_MAX * 2u + 5u;
    cmttmz_rec_edge(long_len, 0);
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(5, rec->count);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT32(0, rec->ticks[1]);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rec->ticks[2]);
    TEST_ASSERT_EQUAL_UINT32(0, rec->ticks[3]);
    TEST_ASSERT_EQUAL_UINT32(5, rec->ticks[4]);
    TEST_ASSERT_EQUAL_INT(1, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(long_len, rec->total_ticks);

    /* úsek přesně UINT32_MAX se nedělí; další úsek má úroveň 0 (index 5 lichý) */
    cmttmz_rec_edge(long_len + UINT32_MAX, 1);
    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(6, rec->count);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rec->ticks[5]);

    cmttmz_rec_edge(max_ticks, 0);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT64(max_ticks, rec->total_ticks);
    uint32_t count = rec->count;

    cmttmz_rec_edge(max_ticks + 1, 1);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());
    TEST_ASSERT_EQUAL_UINT32(count, cmttmz_rec_get()->count);
}

/*
 * Úsek, jehož dělení se do meze položek nevejde, se zahodí celý (žádná
 * useknutá část) a nastaví příznak meze.
 */
void test_split_respects_edge_limit(void)
{
    cmttmz_rec_test_set_limits(3, 0);
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    cmttmz_rec_edge(0, 1);
    cmttmz_rec_edge(10, 0);                          /* 1 položka */
    cmttmz_rec_edge(10 + (uint64_t)UINT32_MAX + 1, 1); /* potřebuje 3 položky, místo jen na 2 */
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());

    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(1, rec->count);
    TEST_ASSERT_EQUAL_UINT32(10, rec->ticks[0]);
    TEST_ASSERT_EQUAL_UINT64(10, rec->total_ticks);
}

/*
 * Víc úseků, než má první blok kapacity: buffer se zvětší a data zůstanou
 * celá (délka úseku i = i % 1000 + 1).
 */
void test_growth_keeps_data(void)
{
    const uint32_t n = CMTTMZ_REC_INITIAL_CAPACITY * 3u + 17u;
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();

    uint64_t t = 7;
    int level = 1;
    uint64_t sum = 0;
    cmttmz_rec_edge(t, level);
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t len = (i % 1000u) + 1u;
        t += len;
        sum += len;
        level ^= 1;
        cmttmz_rec_edge(t, level);
    }
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(n, rec->count);
    TEST_ASSERT_EQUAL_UINT64(sum, rec->total_ticks);
    for (uint32_t i = 0; i < n; i++)
    {
        if (rec->ticks[i] != (i % 1000u) + 1u)
            TEST_FAIL_MESSAGE("segment length changed after buffer growth");
    }
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());
}

/*
 * begin po discard (i po dosažení meze) začíná čistě: prázdný buffer,
 * příznak meze 0, nová úroveň PC1. Bez aktivního záznamu se hrany ignorují.
 */
void test_begin_after_discard_is_clean(void)
{
    /* bez begin: hrana se ignoruje */
    cmttmz_rec_edge(100, 1);
    cmttmz_rec_edge(200, 0);
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_rec_get()->count);
    TEST_ASSERT_NULL(cmttmz_rec_get()->ticks);

    cmttmz_rec_test_set_limits(2, 0);
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();
    cmttmz_rec_edge(10, 1);
    cmttmz_rec_edge(20, 0);
    cmttmz_rec_edge(30, 1);
    cmttmz_rec_edge(40, 0);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_rec_limit_reached());

    cmttmz_rec_discard();
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(0, rec->count);
    TEST_ASSERT_NULL(rec->ticks);
    TEST_ASSERT_EQUAL_UINT64(0, rec->total_ticks);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());

    /* po discard se hrany ignorují */
    cmttmz_rec_edge(50, 1);
    cmttmz_rec_edge(60, 0);
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_rec_get()->count);

    g_pio8255.signal_pc01 = 1;
    cmttmz_rec_begin();
    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(0, rec->count);
    TEST_ASSERT_EQUAL_INT(1, rec->first_level);
    TEST_ASSERT_EQUAL_UINT64(0, rec->total_ticks);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_limit_reached());

    /* nový záznam začíná od nuly: čas první hrany nezáleží na starém záznamu */
    cmttmz_rec_edge(5, 0);
    cmttmz_rec_edge(8, 1);
    rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(1, rec->count);
    TEST_ASSERT_EQUAL_UINT32(3, rec->ticks[0]);
    TEST_ASSERT_EQUAL_INT(0, rec->first_level);

    /* begin během aktivního záznamu ho zahodí */
    g_pio8255.signal_pc01 = 0;
    cmttmz_rec_begin();
    TEST_ASSERT_EQUAL_UINT32(0, cmttmz_rec_get()->count);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_rec_get()->first_level);
}

/* ================================================================
 * DEKÓDOVÁNÍ ZÁZNAMU (cmttmz_rec_decode)
 * ================================================================ */

/** @brief Velikost těla testovacího MZF. */
#define DEC_BODY_SIZE 1024

/** @brief Frekvence bloku 0x18 záznamu. */
#define DEC_RAW_RATE (GDGCLK_BASE / 2)

/** @brief Stav generátoru pseudonáhodných čísel testu. */
static uint32_t s_rng = 1;

/** @brief Pseudonáhodné číslo 0 .. 2^31-1 (LCG, deterministické). */
static uint32_t dec_rand(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 1) & 0x7FFFFFFFu;
}

/** @brief Počet jedničkových bitů (checksum ROM). */
static uint32_t dec_popcount(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint8_t b = d[i]; b; b >>= 1) c += b & 1u;
    return c;
}

/**
 * @brief Naplní MZF pro mztape: typ 01h, jméno, tělo DEC_BODY_SIZE B (pseudonáhodné).
 * @param m Cíl. @param body Buffer těla DEC_BODY_SIZE B. @param name Jméno (ASCII, nejvýš 16 znaků).
 * @param seed Semínko obsahu těla.
 */
static void dec_make_mzf(st_MZTAPE_MZF *m, uint8_t *body, const char *name, uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    uint32_t x = seed;
    for (int i = 0; i < DEC_BODY_SIZE; i++) {
        x = x * 1103515245u + 12345u;
        body[i] = (uint8_t)(x >> 16);
    }
    m->header[0x00] = 0x01;
    memset(&m->header[0x01], 0x0D, 17);
    memcpy(&m->header[0x01], name, strlen(name));
    m->header[0x12] = (uint8_t)(DEC_BODY_SIZE & 0xFF);
    m->header[0x13] = (uint8_t)(DEC_BODY_SIZE >> 8);
    m->header[0x15] = 0x12;
    m->header[0x17] = 0x12;
    m->body = body;
    m->size = DEC_BODY_SIZE;
    m->chkh = dec_popcount(m->header, sizeof(m->header));
    m->chkb = dec_popcount(body, DEC_BODY_SIZE);
}

/** @brief Signál ROM (2x hlavička + 2x tělo) při GDGCLK_BASE. */
static st_CMT_VSTREAM *dec_rom_signal(st_MZTAPE_MZF *m)
{
    st_CMT_VSTREAM *vs = mztape_create_cmt_vstream_from_mztmzf(m, MZTAPE_FORMATSET_MZ800, CMTSPEED_1_1, GDGCLK_BASE);
    TEST_ASSERT_NOT_NULL(vs);
    return vs;
}

/** @brief Počet pulzů vstreamu s nenulovou délkou. */
static uint32_t dec_count_pulses(st_CMT_VSTREAM *vs)
{
    uint32_t n = 0;
    uint64_t samples;
    int value;
    cmt_vstream_read_reset(vs);
    while (cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS)
        if (samples) n++;
    return n;
}

/**
 * @brief Přidá do aktivního záznamu hrany pulzů vstreamu (nejvýš max_pulses pulzů).
 *
 * Každý pulz s nenulovou délkou dá hranu v čase *t s jeho úrovní; *t se
 * posune o délku pulzu. Úroveň po poslední hraně zůstává (pulz se uzavře
 * až další hranou).
 *
 * @param vs Vstream. @param t Čas v taktech (in/out). @param max_pulses Strop počtu pulzů.
 * @return Úroveň posledního přidaného pulzu.
 */
static int dec_feed(st_CMT_VSTREAM *vs, uint64_t *t, uint32_t max_pulses)
{
    uint64_t samples;
    int value = 0;
    int last = -1;
    uint32_t n = 0;
    cmt_vstream_read_reset(vs);
    while (n < max_pulses && cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS) {
        if (!samples) continue;
        cmttmz_rec_edge(*t, value);
        *t += samples;
        last = value;
        n++;
    }
    return last;
}

/** @brief Úroveň prvního pulzu vstreamu s nenulovou délkou. */
static int dec_first_level(st_CMT_VSTREAM *vs)
{
    uint64_t samples;
    int value = 0;
    cmt_vstream_read_reset(vs);
    while (cmt_vstream_read_pulse(vs, &samples, &value) == EXIT_SUCCESS)
        if (samples) return value;
    TEST_FAIL_MESSAGE("empty vstream");
    return 0;
}

/** @brief Zahájí záznam s PC1 opačnou k úrovni první hrany (první hrana se přijme). */
static void dec_begin(int first_edge_level)
{
    g_pio8255.signal_pc01 = (unsigned)(first_edge_level ^ 1);
    cmttmz_rec_begin();
}

/** @brief Dekóduje aktuální záznam; ověří EXIT_SUCCESS a soulad počtů v reportu. */
static uint32_t dec_run(st_TZX_BLOCK **blocks, st_CMTTMZ_REC_BLOCK_INFO **kinds, st_CMTTMZ_REC_REPORT *rep)
{
    uint32_t count = 12345;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_rec_decode(cmttmz_rec_get(), NULL, blocks, kinds, &count, rep));
    TEST_ASSERT_EQUAL_UINT32(count, rep->mzf_blocks + rep->raw_blocks);
    TEST_ASSERT_TRUE(strlen(rep->text) > 0);
    return count;
}

/** @brief Ověří, že blok 0x46 nese MZF bajtově shodný s m. */
static void dec_check_mzf_block(const st_TZX_BLOCK *b, const st_MZTAPE_MZF *m)
{
    TEST_ASSERT_EQUAL_UINT8(TMZ_BLOCK_ID_MZ_FILE, b->id);
    st_MZF *mzf = tmz_block_to_mzf(b, NULL);
    TEST_ASSERT_NOT_NULL(mzf);
    st_MZF_HEADER hdr;
    memcpy(&hdr, &mzf->header, sizeof(hdr));
    mzf_header_swap_endianity(&hdr); /* host -> soubor */
    TEST_ASSERT_EQUAL_MEMORY(m->header, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(m->size, mzf->header.fsize);
    TEST_ASSERT_EQUAL_MEMORY(m->body, mzf->body, m->size);
    mzf_free(mzf);
}

/**
 * @brief Přehraje blok při DEC_RAW_RATE a vrátí pulzy s nenulovou délkou (sousední stejné úrovně sloučené).
 * @param b Blok. @param[out] lens Délky (vzorky). @param[out] levels Úrovně. @param cap Kapacita polí.
 * @return Počet přečtených pulzů (nejvýš cap).
 */
static uint32_t dec_play(const st_TZX_BLOCK *b, uint64_t *lens, int *levels, uint32_t cap)
{
    st_TMZ_PLAYER_CONFIG cfg;
    tmz_player_config_init(&cfg);
    cfg.sample_rate = DEC_RAW_RATE;
    cfg.stream_type = CMT_STREAM_TYPE_VSTREAM;
    en_TMZ_PLAYER_ERROR perr = TMZ_PLAYER_OK;
    st_CMT_STREAM *s = tmz_player_play_block(b, &cfg, &perr);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, tmz_player_error_string(perr));
    st_CMT_VSTREAM *v = s->str.vstream;
    cmt_vstream_read_reset(v);
    uint32_t n = 0;
    uint64_t samples;
    int value;
    while (cmt_vstream_read_pulse(v, &samples, &value) == EXIT_SUCCESS) {
        if (!samples) continue;
        /* vstream dělí běh delší než CMT_VSTREAM_MAX_EVENT na dva eventy téže úrovně */
        if (n && levels[n - 1] == value) {
            lens[n - 1] += samples;
            continue;
        }
        if (n >= cap) break;
        lens[n] = samples;
        levels[n] = value;
        n++;
    }
    cmt_stream_destroy(s);
    return n;
}

/**
 * @brief Záznam z náhodných hran: první hrana s úrovní first, pak count - 1 úseků 20..20 000 taktů.
 * @param first Úroveň po první hraně. @param count Počet hran. @param[out] seg Délky úseků (count - 1).
 */
static void dec_random_record(int first, uint32_t count, uint32_t *seg)
{
    dec_begin(first);
    uint64_t t = 5000;
    int level = first;
    cmttmz_rec_edge(t, level);
    for (uint32_t i = 0; i + 1 < count; i++) {
        uint32_t len = 20u + dec_rand() % 19981u;
        seg[i] = len;
        t += len;
        level ^= 1;
        cmttmz_rec_edge(t, level);
    }
}

/*
 * Signál ROM s kopiemi (MZF 1 KB, jméno "TEST") jako hrany záznamu při
 * GDGCLK_BASE: jeden blok 0x46, MZF z bloku bajtově shodný se vstupem.
 */
void test_decode_good_record(void)
{
    static uint8_t body[DEC_BODY_SIZE];
    st_MZTAPE_MZF m;
    dec_make_mzf(&m, body, "TEST", 777);
    st_CMT_VSTREAM *vs = dec_rom_signal(&m);
    dec_begin(dec_first_level(vs));
    uint64_t t = 10000;
    int last = dec_feed(vs, &t, UINT32_MAX);
    cmttmz_rec_edge(t, last ^ 1); /* uzavře poslední pulz */
    cmt_vstream_destroy(vs);

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
    TEST_ASSERT_EQUAL_UINT32(1, rep.mzf_blocks);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[0].kind);
    TEST_ASSERT_EQUAL_UINT16(0, kinds[0].pause_ms); /* poslední blok */
    dec_check_mzf_block(&blocks[0], &m);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rep.text, "TEST"), rep.text);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/*
 * Dva soubory ROM za sebou (1 s ticha mezi nimi): dva bloky 0x46 ve správném
 * pořadí, žádný surový blok.
 */
void test_decode_two_files(void)
{
    static uint8_t body1[DEC_BODY_SIZE], body2[DEC_BODY_SIZE];
    st_MZTAPE_MZF m1, m2;
    dec_make_mzf(&m1, body1, "ONE", 111);
    dec_make_mzf(&m2, body2, "TWO", 222);
    st_CMT_VSTREAM *v1 = dec_rom_signal(&m1);
    st_CMT_VSTREAM *v2 = dec_rom_signal(&m2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dec_first_level(v2), "test assumes the ROM signal starts HIGH");
    dec_begin(dec_first_level(v1));
    uint64_t t = 10000;
    dec_feed(v1, &t, UINT32_MAX);
    cmttmz_rec_edge(t, 0);    /* ticho LOW (je-li poslední pulz LOW, jen ho prodlouží) */
    t += GDGCLK_BASE;         /* 1 s */
    int last = dec_feed(v2, &t, UINT32_MAX);
    cmttmz_rec_edge(t, last ^ 1);
    cmt_vstream_destroy(v1);
    cmt_vstream_destroy(v2);

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, n, rep.text);
    TEST_ASSERT_EQUAL_UINT32(2, rep.mzf_blocks);
    TEST_ASSERT_EQUAL_UINT32(0, rep.raw_blocks);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[0].kind);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[1].kind);
    TEST_ASSERT_EQUAL_UINT16(1000, kinds[0].pause_ms); /* za ním další 0x46 */
    TEST_ASSERT_EQUAL_UINT16(0, kinds[1].pause_ms);
    dec_check_mzf_block(&blocks[0], &m1);
    dec_check_mzf_block(&blocks[1], &m2);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/* 20 hran (19 úseků, méně než CMTTMZ_REC_MIN_EDGES): žádný blok. */
void test_decode_too_short(void)
{
    uint32_t seg[20];
    s_rng = 5;
    dec_random_record(1, 20, seg);
    st_TZX_BLOCK *blocks = (st_TZX_BLOCK *)1;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = (st_CMTTMZ_REC_BLOCK_INFO *)1;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32(0, n);
    TEST_ASSERT_NULL(blocks);
    TEST_ASSERT_NULL(kinds);
}

/*
 * 5000 náhodných hran: jeden surový blok 0x18, jehož přehrání dá tytéž
 * délky úseků s chybou nejvýš 1 vzorek bloku (záznam začíná LOW, bez
 * předřazeného pulzu).
 */
void test_decode_random_raw(void)
{
    enum { N = 5000 };
    static uint32_t seg[N];
    static uint64_t lens[N + 8];
    static int levels[N + 8];
    s_rng = 42;
    dec_random_record(0, N, seg);

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
    TEST_ASSERT_EQUAL_UINT32(1, rep.raw_blocks);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, blocks[0].id);

    uint32_t played = dec_play(&blocks[0], lens, levels, N + 8);
    TEST_ASSERT_TRUE(played >= N - 1);
    for (uint32_t i = 0; i < N - 1; i++) {
        double want = seg[i] / 2.0;
        double diff = (double)lens[i] - want;
        if (diff > 1.0 || diff < -1.0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "segment %u: %llu samples, want %.1f", (unsigned)i,
                     (unsigned long long)lens[i], want);
            TEST_FAIL_MESSAGE(msg);
        }
        TEST_ASSERT_EQUAL_INT((int)(i & 1u), levels[i]);
    }
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/*
 * Signál ROM useknutý v půlce první kopie těla (hlavička dobrá): jeden
 * surový blok s celým záznamem, žádný blok 0x46.
 */
void test_decode_truncated_body(void)
{
    static uint8_t body[DEC_BODY_SIZE];
    st_MZTAPE_MZF m;
    dec_make_mzf(&m, body, "CUT", 333);
    st_CMT_VSTREAM *vs = dec_rom_signal(&m);
    uint32_t total = dec_count_pulses(vs);
    /* od konce: 2. kopie těla s checksumem (~18 500 pulzů), mezera 256 SHORT
     * (512 pulzů) a polovina 1. kopie (~9 200 pulzů) */
    TEST_ASSERT_TRUE(total > 60000);
    uint32_t keep = total - 28000u;
    dec_begin(dec_first_level(vs));
    uint64_t t = 10000;
    int last = dec_feed(vs, &t, keep);
    cmttmz_rec_edge(t, last ^ 1);
    cmt_vstream_destroy(vs);

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
    TEST_ASSERT_EQUAL_UINT32(0, rep.mzf_blocks);
    TEST_ASSERT_EQUAL_UINT32(1, rep.raw_blocks);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, blocks[0].id);
    TEST_ASSERT_EQUAL_STRING("Recording could not be decoded, stored as a raw block", rep.text);

    /* surový blok nese celý záznam: všechny úseky (+ případný předřazený LOW) */
    static uint64_t lens[90000];
    static int levels[90000];
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_TRUE(rec->count + 1u < 90000u);
    uint32_t played = dec_play(&blocks[0], lens, levels, 90000);
    uint32_t extra = (rec->first_level == 1) ? 1u : 0u;
    TEST_ASSERT_EQUAL_UINT32(rec->count + extra, played);
    uint64_t sum = 0;
    for (uint32_t i = extra; i < played; i++) sum += lens[i];
    double want = rec->total_ticks / 2.0;
    TEST_ASSERT_TRUE(sum + 1 >= want && sum <= want + 1);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/*
 * Polarita surového bloku: záznam začínající HIGH dostane předřazený pulz
 * LOW 1 vzorek a další pulzy mají úrovně záznamu (HIGH, LOW, ...); záznam
 * začínající LOW předřazený pulz nemá.
 */
void test_decode_raw_polarity(void)
{
    enum { N = 100 };
    static uint32_t seg[N];
    static uint64_t lens[N + 8];
    static int levels[N + 8];

    for (int first = 0; first <= 1; first++) {
        s_rng = 99u + (uint32_t)first;
        dec_random_record(first, N, seg);
        TEST_ASSERT_EQUAL_INT(first, cmttmz_rec_get()->first_level);
        st_TZX_BLOCK *blocks = NULL;
        st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
        st_CMTTMZ_REC_REPORT rep;
        uint32_t n = dec_run(&blocks, &kinds, &rep);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
        uint32_t played = dec_play(&blocks[0], lens, levels, N + 8);
        uint32_t off = 0;
        if (first) {
            TEST_ASSERT_EQUAL_INT(0, levels[0]);
            TEST_ASSERT_EQUAL_UINT64(1, lens[0]);
            off = 1;
        }
        TEST_ASSERT_TRUE(played >= off + N - 1);
        for (uint32_t i = 0; i < N - 1; i++) {
            TEST_ASSERT_EQUAL_INT(first ^ (int)(i & 1u), levels[off + i]);
            double diff = (double)lens[off + i] - seg[i] / 2.0;
            TEST_ASSERT_TRUE(diff <= 1.0 && diff >= -1.0);
        }
        cmttmz_rec_blocks_free(blocks, n);
        free(kinds);
    }
}

/** @brief Hranice přijatého souboru podle analyzátoru (index pulzů). */
typedef struct st_DEC_SPAN {
    uint32_t start; /**< header_leader_pulse */
    uint32_t end;   /**< consumed_until_pulse */
} st_DEC_SPAN;

/**
 * @brief Nezávisle spustí analyzátor nad aktuálním záznamem (bez rozdělených úseků)
 *        a vrátí hranice souborů NORMAL s oběma CRC v pořádku, seřazené.
 * @param[out] spans Pole hranic. @param cap Kapacita. @return Počet souborů.
 */
static uint32_t dec_good_spans(st_DEC_SPAN *spans, uint32_t cap)
{
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    st_WAV_PULSE_SEQUENCE seq;
    memset(&seq, 0, sizeof(seq));
    seq.pulses = (st_WAV_PULSE *)malloc(rec->count * sizeof(st_WAV_PULSE));
    TEST_ASSERT_NOT_NULL(seq.pulses);
    seq.count = seq.capacity = rec->count;
    seq.sample_rate = GDGCLK_BASE;
    for (uint32_t k = 0; k < rec->count; k++) {
        TEST_ASSERT_NOT_EQUAL(0, rec->ticks[k]); /* tyto testy nemají rozdělené úseky */
        seq.pulses[k].duration_samples = rec->ticks[k];
        seq.pulses[k].duration_us = rec->ticks[k] * 1e6 / GDGCLK_BASE;
        seq.pulses[k].level = rec->first_level ^ (int)(k & 1u);
    }
    st_WAV_ANALYZER_CONFIG cfg;
    wav_analyzer_config_default(&cfg);
    st_WAV_ANALYZER_RESULT res;
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL_INT(WAV_ANALYZER_OK, wav_analyzer_analyze_pulses(&seq, &cfg, &res));
    free(seq.pulses);
    uint32_t n = 0;
    for (uint32_t i = 0; i < res.file_count && n < cap; i++) {
        const st_WAV_ANALYZER_FILE_RESULT *f = &res.files[i];
        if (f->format != WAV_TAPE_FORMAT_NORMAL || f->header_crc != WAV_CRC_OK || f->body_crc != WAV_CRC_OK) continue;
        spans[n].start = f->header_leader_pulse;
        spans[n].end = f->consumed_until_pulse;
        n++;
    }
    wav_analyzer_result_destroy(&res);
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && spans[j - 1].start > spans[j].start; j--) {
            st_DEC_SPAN x = spans[j];
            spans[j] = spans[j - 1];
            spans[j - 1] = x;
        }
    return n;
}

/**
 * @brief Ověří, že surový blok nese přesně úseky start .. end - 1 bufferu.
 *
 * Počet pulzů, úroveň prvního pulzu (s předřazeným LOW u začátku HIGH)
 * a celková délka do 1 vzorku proti součtu taktů / 2; posun hranice o jeden
 * pulz tedy test zachytí.
 *
 * @param b Blok 0x18. @param start První úsek. @param end Úsek za částí.
 */
static void dec_check_raw_span(const st_TZX_BLOCK *b, uint32_t start, uint32_t end)
{
    static uint64_t lens[140000];
    static int levels[140000];
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT8(TZX_BLOCK_ID_CSW_RECORDING, b->id);
    TEST_ASSERT_TRUE(end > start && end - start + 2u < 140000u);
    uint32_t played = dec_play(b, lens, levels, 140000);
    int first = rec->first_level ^ (int)(start & 1u);
    uint32_t off = first ? 1u : 0u;
    if (first) {
        TEST_ASSERT_EQUAL_INT(0, levels[0]);
        TEST_ASSERT_EQUAL_UINT64(1, lens[0]);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(end - start + off, played, "raw block pulse count");
    TEST_ASSERT_EQUAL_INT(first, levels[off]);
    uint64_t ticks = 0, samples = 0;
    for (uint32_t k = start; k < end; k++) ticks += rec->ticks[k];
    for (uint32_t i = off; i < played; i++) samples += lens[i];
    double want = ticks / 2.0;
    char msg[96];
    snprintf(msg, sizeof(msg), "raw block length %llu samples, want %.1f", (unsigned long long)samples, want);
    TEST_ASSERT_TRUE_MESSAGE((double)samples <= want + 1.0 && (double)samples + 1.0 >= want, msg);
}

/** @brief Přidá count náhodných hran (úseky 20..20 000 taktů) od úrovně level; vrátí úroveň poslední hrany. */
static int dec_noise(uint64_t *t, uint32_t count, int level)
{
    for (uint32_t i = 0; i < count; i++) {
        cmttmz_rec_edge(*t, level);
        *t += 20u + dec_rand() % 19981u;
        level ^= 1;
    }
    return level ^ 1;
}

/*
 * Smíšený záznam: dobrý soubor ROM, 1 s ticha a 5000 náhodných hran =
 * blok 0x46 (pauza 0, za ním surový blok) a surový blok přesně od
 * consumed_until_pulse do konce; s jen 10 hranami za souborem (méně než
 * CMTTMZ_REC_MIN_EDGES) jen blok 0x46.
 */
void test_decode_mixed(void)
{
    static uint8_t body[DEC_BODY_SIZE];
    st_MZTAPE_MZF m;
    dec_make_mzf(&m, body, "MIXED", 444);
    st_CMT_VSTREAM *vs = dec_rom_signal(&m);

    for (int tail = 0; tail < 2; tail++) {
        uint32_t edges = tail ? 10u : 5000u;
        dec_begin(dec_first_level(vs));
        uint64_t t = 10000;
        dec_feed(vs, &t, UINT32_MAX);
        cmttmz_rec_edge(t, 0);
        t += GDGCLK_BASE;
        s_rng = 7;
        int last = dec_noise(&t, edges, 1);
        cmttmz_rec_edge(t, last ^ 1);

        st_DEC_SPAN spans[4];
        TEST_ASSERT_EQUAL_UINT32(1, dec_good_spans(spans, 4));
        st_TZX_BLOCK *blocks = NULL;
        st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
        st_CMTTMZ_REC_REPORT rep;
        uint32_t n = dec_run(&blocks, &kinds, &rep);
        TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[0].kind);
        TEST_ASSERT_EQUAL_UINT16(0, kinds[0].pause_ms);
        dec_check_mzf_block(&blocks[0], &m);
        if (tail) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, n, rep.text);
            TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[1].kind);
            TEST_ASSERT_EQUAL_UINT16(0, kinds[1].pause_ms);
            dec_check_raw_span(&blocks[1], spans[0].end, cmttmz_rec_get()->count);
            TEST_ASSERT_EQUAL_STRING("Recorded \"MIXED\", 1024 B; an undecoded part stored as a raw block", rep.text);
        }
        cmttmz_rec_blocks_free(blocks, n);
        free(kinds);
    }
    cmt_vstream_destroy(vs);
}

/*
 * Šum (500 hran) před dobrým souborem: surový blok přesně od začátku
 * záznamu do header_leader_pulse souboru, pak blok 0x46 (pauza 0).
 */
void test_decode_noise_then_file(void)
{
    static uint8_t body[DEC_BODY_SIZE];
    st_MZTAPE_MZF m;
    dec_make_mzf(&m, body, "AFTER", 555);
    st_CMT_VSTREAM *vs = dec_rom_signal(&m);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dec_first_level(vs), "test assumes the ROM signal starts HIGH");

    dec_begin(0);
    uint64_t t = 10000;
    s_rng = 9;
    int last = dec_noise(&t, 500, 0);
    if (last) cmttmz_rec_edge(t, 0); /* ticho LOW před souborem */
    t += GDGCLK_BASE;
    last = dec_feed(vs, &t, UINT32_MAX);
    cmttmz_rec_edge(t, last ^ 1);
    cmt_vstream_destroy(vs);

    st_DEC_SPAN spans[4];
    TEST_ASSERT_EQUAL_UINT32(1, dec_good_spans(spans, 4));
    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, n, rep.text);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[1].kind);
    TEST_ASSERT_EQUAL_UINT16(0, kinds[0].pause_ms);
    TEST_ASSERT_EQUAL_UINT16(0, kinds[1].pause_ms);
    dec_check_raw_span(&blocks[0], 0, spans[0].start);
    dec_check_mzf_block(&blocks[1], &m);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/*
 * Dobrý soubor, poškozený soubor (useknutý v těle), dobrý soubor: 0x46,
 * surový blok přesně od consumed_until_pulse prvního do header_leader_pulse
 * třetího, 0x46. Pauzy 0 (žádný 0x46 nemá za sebou 0x46).
 */
void test_decode_good_bad_good(void)
{
    static uint8_t b1[DEC_BODY_SIZE], b2[DEC_BODY_SIZE], b3[DEC_BODY_SIZE];
    st_MZTAPE_MZF m1, m2, m3;
    dec_make_mzf(&m1, b1, "FIRST", 1);
    dec_make_mzf(&m2, b2, "BROKEN", 2);
    dec_make_mzf(&m3, b3, "THIRD", 3);
    st_CMT_VSTREAM *v1 = dec_rom_signal(&m1);
    st_CMT_VSTREAM *v2 = dec_rom_signal(&m2);
    st_CMT_VSTREAM *v3 = dec_rom_signal(&m3);
    TEST_ASSERT_EQUAL_INT(1, dec_first_level(v2));
    TEST_ASSERT_EQUAL_INT(1, dec_first_level(v3));

    dec_begin(dec_first_level(v1));
    uint64_t t = 10000;
    dec_feed(v1, &t, UINT32_MAX);
    cmttmz_rec_edge(t, 0);
    t += GDGCLK_BASE;
    dec_feed(v2, &t, dec_count_pulses(v2) - 28000u); /* useknuto v 1. kopii těla */
    cmttmz_rec_edge(t, 0);
    t += GDGCLK_BASE;
    int last = dec_feed(v3, &t, UINT32_MAX);
    cmttmz_rec_edge(t, last ^ 1);
    cmt_vstream_destroy(v1);
    cmt_vstream_destroy(v2);
    cmt_vstream_destroy(v3);

    st_DEC_SPAN spans[4];
    TEST_ASSERT_EQUAL_UINT32(2, dec_good_spans(spans, 4));
    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, n, rep.text);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[0].kind);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[1].kind);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_MZF, kinds[2].kind);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_UINT16(0, kinds[i].pause_ms);
    dec_check_mzf_block(&blocks[0], &m1);
    dec_check_raw_span(&blocks[1], spans[0].end, spans[1].start);
    dec_check_mzf_block(&blocks[2], &m3);
    TEST_ASSERT_EQUAL_STRING("Recorded \"FIRST\", 1024 B; \"THIRD\", 1024 B; an undecoded part stored as a raw block",
                             rep.text);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/*
 * Selhání analýzy (testovací přepínač): záznam se neztratí, celý se uloží
 * jako jeden surový blok; text to říká. Úspěch i s info_out == NULL.
 */
void test_decode_analysis_failure_fallback(void)
{
    static uint8_t body[DEC_BODY_SIZE];
    st_MZTAPE_MZF m;
    dec_make_mzf(&m, body, "LOST", 666);
    st_CMT_VSTREAM *vs = dec_rom_signal(&m);
    dec_begin(dec_first_level(vs));
    uint64_t t = 10000;
    int last = dec_feed(vs, &t, UINT32_MAX);
    cmttmz_rec_edge(t, last ^ 1);
    cmt_vstream_destroy(vs);

    cmttmz_rec_test_fail_analysis(1);
    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    cmttmz_rec_test_fail_analysis(0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
    TEST_ASSERT_EQUAL_STRING("Recording analysis failed, stored as a raw block", rep.text);
    dec_check_raw_span(&blocks[0], 0, cmttmz_rec_get()->count);
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);

    /* bez info_out: úspěch, blok 0x46 */
    uint32_t count = 0;
    blocks = NULL;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_rec_decode(cmttmz_rec_get(), NULL, &blocks, NULL, &count, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    dec_check_mzf_block(&blocks[0], &m);
    cmttmz_rec_blocks_free(blocks, count);
}

/*
 * Ticho delší než UINT32_MAX taktů uprostřed nedekódovaného záznamu: úseky
 * rozdělené bufferem se sloučí do jednoho pulzu, ten se v bloku 0x18 zkrátí
 * na UINT32_MAX vzorků a délky pulzů za ním zůstanou správné.
 */
void test_decode_long_silence(void)
{
    enum { N = 200 };
    static uint64_t lens[N + 8];
    static int levels[N + 8];
    const uint64_t silence = (uint64_t)UINT32_MAX * 3u;
    dec_begin(0);
    uint64_t t = 1000;
    int level = 0;
    s_rng = 3;
    uint32_t seg[N];
    for (uint32_t i = 0; i < N; i++) {
        cmttmz_rec_edge(t, level);
        seg[i] = (i == N / 2) ? 0u : 20u + dec_rand() % 19981u;
        t += (i == N / 2) ? silence : seg[i];
        level ^= 1;
    }
    cmttmz_rec_edge(t, level);
    const st_CMTTMZ_REC *rec = cmttmz_rec_get();
    TEST_ASSERT_EQUAL_UINT32(N + 4u, rec->count); /* ticho = 5 položek (MAX, 0, MAX, 0, MAX) */

    st_TZX_BLOCK *blocks = NULL;
    st_CMTTMZ_REC_BLOCK_INFO *kinds = NULL;
    st_CMTTMZ_REC_REPORT rep;
    uint32_t n = dec_run(&blocks, &kinds, &rep);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n, rep.text);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_REC_BLOCK_RAW, kinds[0].kind);
    uint32_t played = dec_play(&blocks[0], lens, levels, N + 8);
    TEST_ASSERT_EQUAL_UINT32(N, played);
    for (uint32_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_INT((int)(i & 1u), levels[i]);
        if (i == N / 2) {
            TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, lens[i]);
        } else {
            double diff = (double)lens[i] - seg[i] / 2.0;
            TEST_ASSERT_TRUE(diff <= 1.0 && diff >= -1.0);
        }
    }
    cmttmz_rec_blocks_free(blocks, n);
    free(kinds);
}

/* NULL parametry: EXIT_FAILURE. */
void test_decode_null_args(void)
{
    st_TZX_BLOCK *blocks = NULL;
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_rec_decode(NULL, NULL, &blocks, NULL, &count, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_rec_decode(cmttmz_rec_get(), NULL, NULL, NULL, &count, NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_rec_decode(cmttmz_rec_get(), NULL, &blocks, NULL, NULL, NULL));
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_three_edges_two_segments);
    RUN_TEST(test_same_level_edge_ignored);
    RUN_TEST(test_non_increasing_time_rejected);
    RUN_TEST(test_edge_limit);
    RUN_TEST(test_time_limit);
    RUN_TEST(test_default_time_limit_and_split);
    RUN_TEST(test_split_respects_edge_limit);
    RUN_TEST(test_growth_keeps_data);
    RUN_TEST(test_begin_after_discard_is_clean);
    RUN_TEST(test_decode_good_record);
    RUN_TEST(test_decode_two_files);
    RUN_TEST(test_decode_too_short);
    RUN_TEST(test_decode_random_raw);
    RUN_TEST(test_decode_truncated_body);
    RUN_TEST(test_decode_raw_polarity);
    RUN_TEST(test_decode_mixed);
    RUN_TEST(test_decode_noise_then_file);
    RUN_TEST(test_decode_good_bad_good);
    RUN_TEST(test_decode_analysis_failure_fallback);
    RUN_TEST(test_decode_long_silence);
    RUN_TEST(test_decode_null_args);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
