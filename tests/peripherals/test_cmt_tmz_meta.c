/**
 * @file test_cmt_tmz_meta.c
 * @brief Testy nástrojů nad vloženou páskou: informace o pásce, kontrola pásky, převod profilu.
 *
 * Testuje se:
 *   - informace o pásce: páska bez metadat (prázdné texty,
 *     machine -1), zápis vloží 0x32 jako blok 0 a 0x43 za něj, čtení vrátí
 *     totéž i po uložení do TMZ a novém otevření, zápis beze změny pásku
 *     nedotkne (čítače, Modified), cizí položky 0x32 a druhý blok 0x32
 *     přežijí změnu jiné položky, bajty mimo ASCII
 *     nezměněné položky zůstanou, změněný text mimo ASCII dá '?' a varování,
 *     prázdný formulář bloky odstraní, změna stroje nechá takt a verzi ROM,
 *     odstranění bloku 0x43 s jiným taktem varuje, storable 0 u pásky z MZF,
 *     obal mimo STOP odmítne,
 *   - kontrola pásky: vyvážená páska bez nálezů; smyčka bez
 *     konce, konec bez začátku, skupina neuzavřená, skok a volání mimo pásku,
 *     hlavička s neplatným typem, délka těla jiná než v hlavičce, neznámé ID
 *     - každý případ jeden nález se správným číslem bloku; kontrola nic
 *     nemění a nezávisí na otevřeném bloku,
 *   - převod profilu: MZ800EMU-CMT -> ROM-800-NOMINAL a zpět,
 *     MZF bajtově shodné, délka přehrání podle profilu, rychlost měnitelná
 *     jen u MZ800EMU-CMT, výchozí rychlost decku (DEFAULT), pauza z
 *     PROFILE_ARGS, převod na každý profil registru, převod beze změny,
 *     chyby (neznámý profil, nepřevoditelný blok, rychlost u profilu bez
 *     rychlosti) s páskou beze změny, obal mimo STOP.
 *
 * Testovací pásky se generují do dočasného adresáře (g_get_tmp_dir).
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
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/cmt/cmt_tmz.h"
#include "hw-generic/cmt/cmt_tmz_import.h"
#include "hw-generic/cmt/cmt_tmz_meta.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_profile.h"
#include "libs/tmz/tmz_mzfile.h"
#include "libs/tmz/tmz_tlv.h"
#include "libs/mzf/mzf.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"

/** @brief Délka těla testovacích MZF v bajtech. */
#define TEST_BODY_SIZE 64

/** @brief Dočasné soubory pásek (maže tearDown). */
static char *s_paths[3] = {NULL, NULL, NULL};

/**
 * @brief Vrátí cestu dočasného souboru s danou příponou (dosavadní soubor slotu smaže).
 * @param slot Číslo slotu (0 až 2). @param ext Přípona bez tečky.
 * @return Cesta (vlastní ji test, uvolní tearDown).
 */
static const char *tmp_path(int slot, const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmt_meta_%u_%d.%s", (unsigned)g_random_int(), slot, ext);
    if (s_paths[slot]) g_remove(s_paths[slot]);
    g_free(s_paths[slot]);
    s_paths[slot] = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_paths[slot];
}

/**
 * @brief Hlavička testovacího MZF.
 * @param[out] hdr Hlavička. @param name Jméno (nejvýš 16 znaků). @param ftype Typ. @param fsize Velikost těla v hlavičce.
 */
static void make_header(st_MZF_HEADER *hdr, const char *name, uint8_t ftype, uint16_t fsize)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = ftype;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    memcpy(&hdr->fname, name, strlen(name));
    hdr->fsize = fsize;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
}

/** @brief Tělo testovacího MZF. @param[out] body Buffer TEST_BODY_SIZE B. @param seed Odlišení obsahu. */
static void make_body(uint8_t *body, char seed)
{
    for (int i = 0; i < TEST_BODY_SIZE; i++)
        body[i] = (uint8_t)((i * 37 + 11 + seed) & 0xFF);
}

/** @brief Převezme data bloku do souboru a uvolní strukturu. @param f Soubor. @param b Blok (tmz_block_free()). */
static void append_owned(st_TZX_FILE *f, st_TZX_BLOCK *b)
{
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL; /* data převzal soubor */
    tmz_block_free(b);
}

/**
 * @brief Přidá do souboru blok 0x46 (MZ800EMU-CMT, 1:1) s MZF daného jména.
 * @param f Soubor. @param name Jméno MZF. @param pause_ms Pauza za blokem v ms.
 */
static void add_mzf_block(st_TZX_FILE *f, const char *name, uint16_t pause_ms)
{
    st_MZF_HEADER hdr;
    uint8_t body[TEST_BODY_SIZE];
    make_header(&hdr, name, 0x01, TEST_BODY_SIZE);
    make_body(body, name[0]);
    append_owned(f, cmttmz_build_mzf_block(&hdr, body, TEST_BODY_SIZE, CMTSPEED_1_1, NULL, pause_ms));
}

/**
 * @brief Přidá do souboru blok s daty (kopie), např. řídicí blok TZX.
 * @param f Soubor. @param id ID bloku. @param data Data bez bajtu ID (NULL jen při len 0). @param len Délka.
 */
static void add_raw_block(st_TZX_FILE *f, uint8_t id, const uint8_t *data, uint32_t len)
{
    st_TZX_BLOCK b;
    b.id = id;
    b.length = len;
    b.data = NULL;
    if (len) {
        b.data = (uint8_t *)malloc(len);
        TEST_ASSERT_NOT_NULL(b.data);
        memcpy(b.data, data, len);
    }
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/** @brief Položka bloku 0x32 pro sestavení v testu (i bajty mimo ASCII). */
typedef struct {
    uint8_t type;     /**< ID textu */
    const char *text; /**< bajty textu */
} st_ENTRY;

/**
 * @brief Sestaví data bloku 0x32 bajt po bajtu (bez knihovny, i cizí ID a bajty mimo ASCII).
 * @param e Položky. @param n Počet. @param[out] out Buffer. @param size Velikost bufferu.
 * @return Délka dat.
 */
static uint32_t archive_data(const st_ENTRY *e, int n, uint8_t *out, size_t size)
{
    uint32_t pos = 3;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(e[i].text);
        TEST_ASSERT_TRUE(pos + 2 + len <= size);
        out[pos++] = e[i].type;
        out[pos++] = (uint8_t)len;
        memcpy(out + pos, e[i].text, len);
        pos += (uint32_t)len;
    }
    out[0] = (uint8_t)((pos - 2) & 0xFF);
    out[1] = (uint8_t)((pos - 2) >> 8);
    out[2] = (uint8_t)n;
    return pos;
}

/** @brief Přidá blok 0x32 z položek. @param f Soubor. @param e Položky. @param n Počet. */
static void add_archive(st_TZX_FILE *f, const st_ENTRY *e, int n)
{
    uint8_t buf[2048];
    uint32_t len = archive_data(e, n, buf, sizeof(buf));
    add_raw_block(f, TZX_BLOCK_ID_ARCHIVE_INFO, buf, len);
}

/** @brief Vytvoří prázdný soubor s hlavičkou TMZ. @return Soubor (uvolní save_tape()). */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/**
 * @brief Uloží soubor do slotu s danou příponou, uvolní ho a otevře jako pásku.
 * @param f Soubor (funkce ho uvolní). @param ext Přípona ("tmz", "tzx").
 */
static void open_tape(st_TZX_FILE *f, const char *ext)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    const char *path = tmp_path(0, ext);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
}

/** @brief Otevře pásku z bloků MZF "A", "B" (pauza 1000 ms, poslední 0). */
static void open_ab(void)
{
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
}

/** @brief Počet bloků vložené pásky. */
static int tape_count(void)
{
    const st_TZX_FILE *f = cmttmz_get_file();
    return f ? (int)f->block_count : -1;
}

/** @brief Blok vložené pásky. @param i Číslo bloku. @return Blok. */
static const st_TZX_BLOCK *tape_block(int i)
{
    const st_TZX_FILE *f = cmttmz_get_file();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(i >= 0 && (uint32_t)i < f->block_count);
    return &f->blocks[i];
}

/**
 * @brief Popis pořadí bloků: jména MZF a "#ID" ostatních bloků, oddělené čárkou.
 * @param[out] out Buffer. @param size Velikost bufferu.
 * @return @p out.
 */
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

/** @brief Ověří pořadí bloků. @param expected Očekávaný popis (tape_names()). */
#define ASSERT_NAMES(expected)                                                                                         \
    do {                                                                                                               \
        char names_buf_[256];                                                                                          \
        TEST_ASSERT_EQUAL_STRING(expected, tape_names(names_buf_, sizeof(names_buf_)));                                \
    } while (0)

/** @brief Čítače pásky pro porovnání před a po volání. */
typedef struct {
    unsigned tape;   /**< cmttmz_get_tape_serial() */
    unsigned open;   /**< cmttmz_get_open_serial() */
    unsigned change; /**< cmttmz_get_change_serial() */
} st_SERIALS;

/** @brief Přečte čítače pásky. @return Čítače. */
static st_SERIALS serials(void)
{
    st_SERIALS s = {cmttmz_get_tape_serial(), cmttmz_get_open_serial(), cmttmz_get_change_serial()};
    return s;
}

/** @brief Ověří, že se žádný čítač nezměnil. @param s Čítače před voláním. */
static void assert_serials_same(st_SERIALS s)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.tape, cmttmz_get_tape_serial(), "tape serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.open, cmttmz_get_open_serial(), "open serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s.change, cmttmz_get_change_serial(), "change serial");
}

/** @brief Kopie bajtů celé pásky (obraz) pro porovnání "beze změny". @param[out] size Délka. @return Obraz (g_free). */
static uint8_t *tape_image(size_t *size)
{
    uint8_t *data = NULL;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save_image(&data, size));
    return data;
}

/** @brief Ověří, že obraz pásky je stejný jako @p before (a uvolní ho). @param before Obraz. @param size Délka. */
static void assert_image_same(uint8_t *before, size_t size)
{
    size_t now_size = 0;
    uint8_t *now = tape_image(&now_size);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)size, (uint64_t)now_size, "tape image size");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, now, size, "tape image bytes");
    g_free(now);
    g_free(before);
}

/** @brief Hodnota g_cmt.autorewind před testem. */
static int s_saved_autorewind;
/** @brief Výchozí rychlost Virtual CMT před testem. */
static en_CMTSPEED s_saved_speed;

void setUp(void)
{
    g_gdg.total_elapsed.screens += g_gdg.total_elapsed.ticks / VIDEO_SCREEN_TICKS;
    g_gdg.total_elapsed.ticks %= VIDEO_SCREEN_TICKS;
    s_saved_autorewind = g_cmt.autorewind;
    g_cmt.autorewind = 0;
    s_saved_speed = g_cmt.mz_cmtspeed;
    g_cmt.mz_cmtspeed = CMTSPEED_1_1;
}

void tearDown(void)
{
    g_cmt.autorewind = s_saved_autorewind;
    cmt_eject();
    g_cmt.mz_cmtspeed = s_saved_speed;
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    g_cmt.playsts = CMTEXT_BLOCK_PLAYSTS_STOP;
    for (int i = 0; i < 3; i++) {
        if (s_paths[i]) {
            g_remove(s_paths[i]);
            g_free(s_paths[i]);
            s_paths[i] = NULL;
        }
    }
}

/* ================================================================
 * INFORMACE O PÁSCE
 * ================================================================ */

/** @brief Ověří, že všechny texty informací jsou prázdné. @param info Informace. */
static void assert_texts_empty(st_CMTTMZ_TAPE_INFO *info)
{
    for (uint32_t i = 0; i < CMTTMZ_TAPE_INFO_FIELDS; i++)
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", cmttmz_tape_info_text(info, i), cmttmz_tape_info_key(i));
}

/* Páska bez metadat: prázdné texty, machine -1, bez cizích položek; bez pásky chyba. */
void test_info_get_without_metadata(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(-1, info.machine);

    open_ab();
    memset(&info, 0x55, sizeof(info));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    assert_texts_empty(&info);
    TEST_ASSERT_EQUAL_INT(-1, info.machine);
    TEST_ASSERT_EQUAL_INT(1, info.storable);
    TEST_ASSERT_EQUAL_UINT32(0, info.other_entries);

    /* klíče a buffery textů */
    TEST_ASSERT_EQUAL_STRING("title", cmttmz_tape_info_key(0));
    TEST_ASSERT_EQUAL_STRING("comment", cmttmz_tape_info_key(9));
    TEST_ASSERT_NULL(cmttmz_tape_info_key(10));
    TEST_ASSERT_TRUE(cmttmz_tape_info_text(&info, 0) == info.title);
    TEST_ASSERT_TRUE(cmttmz_tape_info_text(&info, 3) == info.year);
    TEST_ASSERT_TRUE(cmttmz_tape_info_text(&info, 9) == info.comment);
    TEST_ASSERT_NULL(cmttmz_tape_info_text(&info, 10));
}

/*
 * Zápis vloží 0x32 jako blok 0 a 0x43 hned za něj; čtení vrátí totéž;
 * po uložení do TMZ a novém otevření zase totéž.
 */
void test_info_set_inserts_blocks_and_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    st_CMTTMZ_TAPE_INFO in;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&in));
    strcpy(in.title, "Space Game");
    strcpy(in.author, "Somebody");
    strcpy(in.year, "1986");
    strcpy(in.comment, "Dumped from tape");
    in.machine = TMZ_MACHINE_MZ800;
    unsigned tape_serial = cmttmz_get_tape_serial();
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&in, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    ASSERT_NAMES("#32,#43,A,B");
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_NOT_EQUAL_UINT(tape_serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(2, cmttmz_get_open_block_id());

    /* blok 0x43: stroj, takt 3 546 895 Hz (výchozí takt playeru), ROM 0 */
    const st_TZX_BLOCK *mi = tape_block(1);
    TEST_ASSERT_EQUAL_UINT32(4 + 6, mi->length);
    TEST_ASSERT_EQUAL_UINT8(TMZ_MACHINE_MZ800, mi->data[4]);
    uint32_t clock = (uint32_t)mi->data[5] | ((uint32_t)mi->data[6] << 8) | ((uint32_t)mi->data[7] << 16) |
                     ((uint32_t)mi->data[8] << 24);
    TEST_ASSERT_EQUAL_UINT32(3546895u, clock);
    TEST_ASSERT_EQUAL_UINT8(0, mi->data[9]);

    /* blok 0x32: položky v pořadí ID */
    const st_TZX_BLOCK *ai = tape_block(0);
    const uint8_t expect[] = {0, 0, 4, 0x00, 10, 'S', 'p', 'a', 'c', 'e', ' ', 'G', 'a', 'm', 'e', 0x02, 8, 'S', 'o', 'm',
                              'e', 'b', 'o', 'd', 'y', 0x03, 4, '1', '9', '8', '6', 0xFF, 16, 'D', 'u', 'm', 'p', 'e',
                              'd', ' ', 'f', 'r', 'o', 'm', ' ', 't', 'a', 'p', 'e'};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expect), ai->length);
    TEST_ASSERT_EQUAL_UINT8(sizeof(expect) - 2, ai->data[0]);
    TEST_ASSERT_EQUAL_MEMORY(expect + 2, ai->data + 2, sizeof(expect) - 2);

    st_CMTTMZ_TAPE_INFO out;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&out));
    TEST_ASSERT_EQUAL_STRING("Space Game", out.title);
    TEST_ASSERT_EQUAL_STRING("", out.publisher);
    TEST_ASSERT_EQUAL_STRING("Somebody", out.author);
    TEST_ASSERT_EQUAL_STRING("1986", out.year);
    TEST_ASSERT_EQUAL_STRING("Dumped from tape", out.comment);
    TEST_ASSERT_EQUAL_INT(TMZ_MACHINE_MZ800, out.machine);
    TEST_ASSERT_EQUAL_UINT32(0, out.other_entries);

    /* uložit do TMZ a znovu otevřít */
    const char *path = tmp_path(1, "tmz");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_save(path));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    ASSERT_NAMES("#32,#43,A,B");
    st_CMTTMZ_TAPE_INFO again;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&again));
    TEST_ASSERT_NOT_EQUAL_UINT(out.tape_serial, again.tape_serial); /* jiná páska (nové otevření) */
    out.tape_serial = again.tape_serial;
    TEST_ASSERT_EQUAL_MEMORY(&out, &again, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* zápis beze změny: páska nedotčená, Modified 0 */
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&again, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));

    /* změna jen stroje: blok 0x32 se nemění */
    img = NULL;
    again.machine = TMZ_MACHINE_MZ1500;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&again, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    ASSERT_NAMES("#32,#43,A,B");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_UINT8(TMZ_MACHINE_MZ1500, tape_block(1)->data[4]);
}

/*
 * Cizí položky bloku 0x32 (neznámé ID, druhý komentář),
 * bajty mimo ASCII nezměněné položky, druhý blok 0x32 a blok 0x30 přežijí
 * změnu jiné položky; zápis beze změny pásku nedotkne.
 */
void test_info_foreign_entries_survive(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    const st_ENTRY first[] = {
        {0x00, "Old title"}, {0x10, "X-Unknown"}, {0xFF, "c1"}, {0x01, "\xE9" "dition"}, {0xFF, "c2"}, {0x03, "1985"},
    };
    add_archive(f, first, 6);
    st_TZX_BLOCK td;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description("side A", &td));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &td));
    const st_ENTRY second[] = {{0x00, "Second archive"}};
    add_archive(f, second, 1);
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tzx");
    ASSERT_NAMES("#32,#30,#32,A,B");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("Old title", info.title);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9" "dition", info.publisher); /* ISO 8859-1 E9h = U+00E9 */
    TEST_ASSERT_EQUAL_STRING("1985", info.year);
    TEST_ASSERT_EQUAL_STRING("c1", info.comment);
    TEST_ASSERT_EQUAL_UINT32(2, info.other_entries);
    TEST_ASSERT_EQUAL_INT(-1, info.machine);
    TEST_ASSERT_EQUAL_INT(1, info.storable);

    /* OK beze změny: páska nedotčená */
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* změna roku: ostatní položky bajtově a v původním pořadí */
    strcpy(info.year, "1986");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    ASSERT_NAMES("#32,#30,#32,A,B");
    const st_ENTRY expect[] = {
        {0x00, "Old title"}, {0x10, "X-Unknown"}, {0xFF, "c1"}, {0x01, "\xE9" "dition"}, {0xFF, "c2"}, {0x03, "1986"},
    };
    uint8_t buf[512];
    uint32_t len = archive_data(expect, 6, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);
    /* druhý blok 0x32 a blok 0x30 beze změny */
    len = archive_data(second, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(2)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(2)->data, len);
    TEST_ASSERT_EQUAL_UINT32(7, tape_block(1)->length);
    TEST_ASSERT_EQUAL_MEMORY("\x06side A", tape_block(1)->data, 7);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(2));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(0));

    /* změněný text mimo ISO 8859-1: '?' a varování; vymazaný komentář smaže obě položky FFh */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info)); /* formulář po změně pásky znovu */
    strcpy(info.title, "\xC5\xBDlu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88"); /* "Žluťoučký kůň" v UTF-8 */
    info.comment[0] = '\0';
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "'?'"));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "title"));
    TEST_ASSERT_NULL(strstr(cmttmz_get_edit_warning(), "comment"));
    st_CMTTMZ_TAPE_INFO out;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&out));
    TEST_ASSERT_EQUAL_STRING("?lu?ou?k\xC3\xBD k??", out.title); /* ý (U+00FD) je v ISO 8859-1 */
    TEST_ASSERT_EQUAL_STRING("", out.comment);                   /* obě položky FFh pryč */
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9" "dition", out.publisher);
    TEST_ASSERT_EQUAL_UINT32(1, out.other_entries);
    const st_ENTRY expect2[] = {
        {0x00, "?lu?ou?k\xFD k??"}, {0x10, "X-Unknown"}, {0x01, "\xE9" "dition"}, {0x03, "1986"},
    };
    len = archive_data(expect2, 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);

    /* prázdný formulář: cizí položka 0x10 zůstane, blok 0x32 tedy také */
    st_CMTTMZ_TAPE_INFO empty;
    memset(&empty, 0, sizeof(empty));
    empty.machine = -1;
    empty.tape_serial = cmttmz_get_tape_serial();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&empty, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    ASSERT_NAMES("#32,#30,#32,A,B");
    const st_ENTRY expect3[] = {{0x10, "X-Unknown"}};
    len = archive_data(expect3, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);
}

/* Prázdný formulář odstraní blok 0x32 i 0x43 (bez cizích položek). */
void test_info_empty_form_removes_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    const st_ENTRY e[] = {{0x00, "Title"}, {0xFF, "Comment"}};
    add_archive(f, e, 2);
    append_owned(f, tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ700, 3546895u, 0));
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    ASSERT_NAMES("#32,#43,A,B");
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("Title", info.title);
    TEST_ASSERT_EQUAL_INT(TMZ_MACHINE_MZ700, info.machine);

    st_CMTTMZ_TAPE_INFO empty;
    memset(&empty, 0, sizeof(empty));
    empty.machine = -1;
    empty.tape_serial = info.tape_serial;
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&empty, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    ASSERT_NAMES("A,B");
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning()); /* takt 0x43 byl výchozí */
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    assert_texts_empty(&info);
    TEST_ASSERT_EQUAL_INT(-1, info.machine);
}

/*
 * Změna stroje nechá takt a verzi ROM bloku 0x43; odstranění bloku 0x43
 * s jiným než výchozím taktem varuje (takt bloků za ním se změní).
 * Nový blok 0x43 bez bloku 0x32 je blok 0.
 */
void test_info_machine_keeps_clock_and_warns(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    append_owned(f, tmz_block_create_mz_machine_info(TMZ_MACHINE_MZ700, 3500000u, 7));
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(TMZ_MACHINE_MZ700, info.machine);
    uint8_t before[10];
    TEST_ASSERT_EQUAL_UINT32(10, tape_block(0)->length);
    memcpy(before, tape_block(0)->data, 10);

    info.machine = TMZ_MACHINE_MZ80B;
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    ASSERT_NAMES("#43,A,B");
    before[4] = TMZ_MACHINE_MZ80B;
    TEST_ASSERT_EQUAL_MEMORY(before, tape_block(0)->data, 10);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info)); /* formulář po změně pásky znovu */

    /* neplatný stroj: chyba, páska beze změny */
    st_SERIALS s = serials();
    info.machine = TMZ_MACHINE_COUNT;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "Invalid machine"));
    assert_serials_same(s);

    /* text bez ukončovací nuly v bufferu: chyba */
    info.machine = TMZ_MACHINE_MZ80B;
    memset(info.title, 'x', sizeof(info.title));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "title"));
    assert_serials_same(s);
    info.title[0] = '\0';

    /* odstranění bloku 0x43 s taktem 3,5 MHz: varování o taktu */
    info.machine = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    ASSERT_NAMES("A,B");
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "clock"));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));

    /* nový blok 0x43 bez bloku 0x32 = blok 0, bez varování (výchozí takt) */
    info.machine = TMZ_MACHINE_GENERIC;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    ASSERT_NAMES("#43,A,B");
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(TMZ_MACHINE_GENERIC, info.machine);

    /* titul doplněný k existujícímu 0x43: 0x32 jako blok 0 */
    strcpy(info.title, "T");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    ASSERT_NAMES("#32,#43,A,B");
}

/* storable 0 u pásky otevřené z MZF; zápis do paměti přesto projde. */
void test_info_storable_mzf(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_MZF_HEADER hdr;
    make_header(&hdr, "MZF", 0x01, TEST_BODY_SIZE);
    uint8_t file[sizeof(st_MZF_HEADER) + TEST_BODY_SIZE];
    memcpy(file, &hdr, sizeof(hdr));
    make_body(file + sizeof(hdr), 'M');
    const char *path = tmp_path(0, "mzf");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)file, sizeof(file), NULL));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(0, info.storable);
    strcpy(info.title, "In memory");
    int changed = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT(2, tape_count());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(0, info.storable);

    /* nová páska bez souboru: storable 1 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_new_tape());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_INT(1, info.storable);
}

/* Obal mimo STOP odmítne, páska beze změny. */
void test_info_wrapper_refused_outside_stop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    strcpy(info.title, "X");
    g_cmt.paused = 0;
    cmt_play();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_TRUE(cmttmz_get_edit_error()[0] != '\0');
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_convert_block(0, "ROM-800-NOMINAL", CMTSPEED_NONE));
    cmt_stop();
    ASSERT_NAMES("A,B");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}

/* ================================================================
 * KONTROLA PÁSKY
 * ================================================================ */

/** @brief Výsledek kontroly pásky v testu. */
typedef struct {
    st_CMTTMZ_CHECK_ITEM *items; /**< nálezy (g_free) */
    uint32_t count;              /**< počet nálezů */
    uint32_t blocks;             /**< zkontrolované bloky */
} st_CHECK;

/** @brief Spustí kontrolu pásky. @return Výsledek (uvolnit g_free(items)). */
static st_CHECK run_check(void)
{
    st_CHECK c = {NULL, 12345, 0};
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_check(&c.items, &c.count, &c.blocks));
    return c;
}

/**
 * @brief Ověří, že kontrola dá právě jeden nález.
 * @param block Číslo bloku. @param severity Závažnost. @param text Podřetězec textu.
 */
static void assert_single_finding(int block, int severity, const char *text)
{
    st_CHECK c = run_check();
    for (uint32_t i = 0; i < c.count; i++)
        fprintf(stderr, "finding: block %d sev %d: %s\n", c.items[i].block_id, c.items[i].severity, c.items[i].text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, c.count, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(block, c.items[0].block_id, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(severity, c.items[0].severity, text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c.items[0].text, text), c.items[0].text);
    g_free(c.items);
}

/** @brief Přidá řídicí blok s 16bitovým parametrem (LE). @param f Soubor. @param id ID. @param v Hodnota. */
static void add_word_block(st_TZX_FILE *f, uint8_t id, int16_t v)
{
    const uint8_t d[2] = {(uint8_t)((uint16_t)v & 0xFF), (uint8_t)((uint16_t)v >> 8)};
    add_raw_block(f, id, d, 2);
}

/*
 * Vyvážená páska (skupina, smyčka, skok, volání s návratem, informační
 * bloky) bez nálezů; kontrola nic nemění a nezávisí na otevřeném bloku.
 */
void test_check_balanced_tape_no_findings(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    uint32_t count = 7;
    st_CMTTMZ_CHECK_ITEM *items = (st_CMTTMZ_CHECK_ITEM *)1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_check(&items, &count, NULL));
    TEST_ASSERT_NULL(items);
    TEST_ASSERT_EQUAL_UINT32(0, count);

    st_TZX_FILE *f = new_tmz();
    const st_ENTRY e[] = {{0x00, "Balanced"}};
    add_archive(f, e, 1);                                       /* 0 */
    const uint8_t grp[] = {1, 'G'};
    add_raw_block(f, TZX_BLOCK_ID_GROUP_START, grp, 2);         /* 1 */
    add_mzf_block(f, "A", 1000);                                /* 2 */
    add_raw_block(f, TZX_BLOCK_ID_GROUP_END, NULL, 0);          /* 3 */
    add_word_block(f, TZX_BLOCK_ID_LOOP_START, 2);              /* 4 */
    add_mzf_block(f, "B", 1000);                                /* 5 */
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);           /* 6 */
    add_word_block(f, TZX_BLOCK_ID_JUMP, 2);                    /* 7 -> 9 */
    add_mzf_block(f, "C", 1000);                                /* 8 */
    const uint8_t call[] = {1, 0, 3, 0};
    add_raw_block(f, TZX_BLOCK_ID_CALL_SEQUENCE, call, 4);      /* 9 -> 12 */
    add_word_block(f, TZX_BLOCK_ID_JUMP, 4);                    /* 10 -> 14 */
    add_word_block(f, TZX_BLOCK_ID_JUMP, -10);                  /* 11 -> 1 */
    add_mzf_block(f, "D", 1000);                                /* 12 */
    add_raw_block(f, TZX_BLOCK_ID_RETURN_FROM_SEQ, NULL, 0);    /* 13 */
    add_mzf_block(f, "E", 0);                                   /* 14 */
    open_tape(f, "tmz");
    ASSERT_NAMES("#32,#21,A,#22,#24,B,#25,#23,C,#26,#23,#23,D,#27,E");

    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    st_CHECK c = run_check();
    TEST_ASSERT_EQUAL_UINT32(0, c.count);
    TEST_ASSERT_NULL(c.items);
    TEST_ASSERT_EQUAL_UINT32(15, c.blocks);
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* jiný otevřený blok: stejný výsledek */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_select_block(12, 0));
    c = run_check();
    TEST_ASSERT_EQUAL_UINT32(0, c.count);
    TEST_ASSERT_EQUAL_UINT32(15, c.blocks);
}

/* Každá vada pásky jeden nález se správným číslem bloku. */
void test_check_findings(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f;

    /* smyčka bez konce */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_word_block(f, TZX_BLOCK_ID_LOOP_START, 2);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Loop start without a loop end");

    /* konec bez začátku */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 1000);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    add_mzf_block(f, "C", 0);
    open_tape(f, "tmz");
    assert_single_finding(2, CMTTMZ_CHECK_WARNING, "Loop end without a loop start");

    /* skupina neuzavřená */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t grp[] = {1, 'G'};
    add_raw_block(f, TZX_BLOCK_ID_GROUP_START, grp, 2);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Group start without a group end");

    /* skok mimo pásku */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 1000);
    add_word_block(f, TZX_BLOCK_ID_JUMP, 5);
    add_mzf_block(f, "C", 0);
    open_tape(f, "tmz");
    assert_single_finding(2, CMTTMZ_CHECK_WARNING, "Jump to block 7 outside the tape");

    /* volání mimo pásku (s návratem na pásce) */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t call[] = {1, 0, 0xFE, 0xFF}; /* -2 */
    add_raw_block(f, TZX_BLOCK_ID_CALL_SEQUENCE, call, 4);
    add_raw_block(f, TZX_BLOCK_ID_RETURN_FROM_SEQ, NULL, 0);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Call to block -1 outside the tape");

    /* hlavička s neplatným typem */
    f = new_tmz();
    add_mzf_block(f, "A", 1000);
    st_MZF_HEADER hdr;
    uint8_t body[TEST_BODY_SIZE];
    make_header(&hdr, "BAD", 0x77, TEST_BODY_SIZE);
    make_body(body, 'X');
    append_owned(f, cmttmz_build_mzf_block(&hdr, body, TEST_BODY_SIZE, CMTSPEED_1_1, NULL, 0));
    open_tape(f, "tmz");
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Unknown file type 0x77 in the header");

    /* délka těla jiná než v hlavičce */
    f = new_tmz();
    make_header(&hdr, "LEN", 0x01, 100);
    make_body(body, 'L');
    en_TMZ_PROFILE_ERROR pe;
    append_owned(f, tmz_profile_build_block(TMZ_PROFILE_ID_MZ800EMU_CMT, NULL, &hdr, body, TEST_BODY_SIZE, &pe));
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    assert_single_finding(0, CMTTMZ_CHECK_WARNING, "Body length 64 B differs from the header size 100 B");

    /* neznámé ID bloku (vložené přímo do pásky v paměti) */
    open_ab();
    st_TZX_BLOCK unk;
    unk.id = 0x99;
    unk.length = 4;
    unk.data = (uint8_t *)calloc(1, 4);
    TEST_ASSERT_NOT_NULL(unk.data);
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(1, &unk, &sp, 1));
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Unknown block ID 0x99");
}

/* ================================================================
 * PŘEVOD PROFILU
 * ================================================================ */

/** @brief Kopie MZF bloku (hlavička a tělo) pro bajtové porovnání. */
typedef struct {
    uint8_t header[sizeof(st_MZF_HEADER)]; /**< hlavička v pořadí hostitele */
    uint8_t body[TEST_BODY_SIZE];          /**< tělo */
    uint32_t body_size;                    /**< délka těla */
} st_MZF_COPY;

/** @brief Přečte MZF bloku. @param id Blok. @return Kopie. */
static st_MZF_COPY block_mzf(int id)
{
    st_MZF_COPY c;
    memset(&c, 0, sizeof(c));
    st_MZF *mzf = cmttmz_block_get_mzf(id);
    TEST_ASSERT_NOT_NULL(mzf);
    memcpy(c.header, &mzf->header, sizeof(c.header));
    TEST_ASSERT_TRUE(mzf->body_size <= TEST_BODY_SIZE);
    memcpy(c.body, mzf->body, mzf->body_size);
    c.body_size = mzf->body_size;
    mzf_free(mzf);
    return c;
}

/** @brief Ověří shodu MZF bloku s kopií. @param id Blok. @param c Kopie. */
static void assert_block_mzf(int id, const st_MZF_COPY *c)
{
    st_MZF_COPY now = block_mzf(id);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(c->header, now.header, sizeof(c->header), "MZF header");
    TEST_ASSERT_EQUAL_UINT32(c->body_size, now.body_size);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(c->body, now.body, c->body_size, "MZF body");
}

/** @brief Délka přehrání bloku generováním streamu. @param id Blok. @return Délka v s. */
static double block_seconds(int id)
{
    int cheap_known = 0, stream_known = 0;
    double cheap_s = 0, stream_s = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_length_paths(id, &cheap_known, &cheap_s, &stream_known, &stream_s));
    TEST_ASSERT_EQUAL_INT(1, stream_known);
    return stream_s;
}

/** @brief ID profilu bloku. @param id Blok. @param[out] buf Buffer 64 B. @return @p buf. */
static const char *block_profile(int id, char *buf)
{
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_profile(id, buf, 64));
    return buf;
}

/*
 * MZ800EMU-CMT -> ROM-800-NOMINAL a zpět: MZF bajtově shodné, délka podle
 * profilu, rychlost měnitelná jen u MZ800EMU-CMT, pauza z PROFILE_ARGS.
 */
void test_convert_emu_to_rom_and_back(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    char prof[64];
    TEST_ASSERT_EQUAL_STRING(TMZ_PROFILE_ID_MZ800EMU_CMT, block_profile(0, prof));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_profile_convertible(0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_speed_changeable(0, CMTSPEED_2_1));
    st_MZF_COPY mzf = block_mzf(0);
    double len_emu = block_seconds(0);
    unsigned tape_serial = cmttmz_get_tape_serial();

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("ROM-800-NOMINAL", block_profile(0, prof));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning()); /* nic se neztratilo */
    assert_block_mzf(0, &mzf);
    ASSERT_NAMES("A,B");
    double len_rom = block_seconds(0);
    /* délka podle profilu: naměřeno ROM-800-NOMINAL 14,1 s proti 11,4 s MZ800EMU-CMT 1:1 (64 B) */
    TEST_ASSERT_TRUE_MESSAGE(len_rom > len_emu + 1.0, "ROM profile plays longer");
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(0, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_speed_changeable(0, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_is_unsaved(0));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(1));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_is_modified());
    TEST_ASSERT_NOT_EQUAL_UINT(tape_serial, cmttmz_get_tape_serial());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_get_open_block_id());
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(0, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_NONE, sp.origin);

    /* zpět na MZ800EMU-CMT s pevnou rychlostí 2:1 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_STRING(TMZ_PROFILE_ID_MZ800EMU_CMT, block_profile(0, prof));
    assert_block_mzf(0, &mzf);
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_speed_changeable(0, CMTSPEED_3_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(0, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1, sp.speed);
    TEST_ASSERT_EQUAL_UINT16(1000, sp.pause_ms);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_SET, cmtext_container_get_block_speed(cmtext_get_container(g_cmt.ext), 0));
    double len_2_1 = block_seconds(0);
    TEST_ASSERT_TRUE_MESSAGE(len_2_1 < len_emu, "2:1 plays shorter than 1:1");
    printf("block A length: MZ800EMU-CMT 1:1 %.3f s, ROM-800-NOMINAL %.3f s, MZ800EMU-CMT 2:1 %.3f s\n", len_emu, len_rom,
           len_2_1);

    /* tentýž profil bez rychlosti: beze změny */
    st_SERIALS s = serials();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_NONE));
    assert_serials_same(s);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(0, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1, sp.speed);

    /* z jiného profilu bez rychlosti: výchozí rychlost decku, DEFAULT */
    g_cmt.mz_cmtspeed = CMTSPEED_3_1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(0, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_DEFAULT, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, sp.speed);
    TEST_ASSERT_EQUAL_INT(CMTEXT_BLOCK_SPEED_DEFAULT, cmtext_container_get_block_speed(cmtext_get_container(g_cmt.ext), 0));
    TEST_ASSERT_EQUAL_INT(1, cmttmz_block_speed_changeable(0, CMTSPEED_1_1));
    assert_block_mzf(0, &mzf);

    /* poslední blok s pauzou 0: pauza se převezme */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(1, "TC-1.21", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(1, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_1_1));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &sp, NULL));
    TEST_ASSERT_EQUAL_UINT16(0, sp.pause_ms);
}

/* Převod beze změny výsledku pásku nedotkne. */
void test_convert_same_is_noop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    /* blok z TMZ s PROFILE_ARGS speed=1:1,pause=1000 je FIXED 1:1 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_1_1));
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_is_unsaved(0));
}

/* Chyby: páska beze změny a text chyby. */
void test_convert_errors(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("No tape inserted", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_profile_convertible(0));

    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const uint8_t loop[] = {2, 0};
    add_raw_block(f, TZX_BLOCK_ID_LOOP_START, loop, 2);
    add_mzf_block(f, "B", 1000);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    open_tape(f, "tmz");
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);

    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, "NO-SUCH-PROFILE", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("Unknown profile: NO-SUCH-PROFILE", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, NULL, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(0, cmttmz_block_profile_convertible(1));
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(1, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("Block 1 has no MZ file that can be converted", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(9, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("Block 9 does not exist", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, "ROM-800-NOMINAL", CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_STRING("Profile ROM-800-NOMINAL does not accept a speed", cmttmz_get_edit_error());
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_CUSTOM));
    TEST_ASSERT_EQUAL_STRING("Invalid speed", cmttmz_get_edit_error());
    /* BASIC-1Z016 chce tělo z kusů po 258 B */
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_block_convert_profile(0, "BASIC-1Z016", CMTSPEED_NONE));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_error(), "Can't build the block with profile BASIC-1Z016"));
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());
}

/**
 * @brief Očekávaný registr profilů tmz 1.14.0: ID, zda ho převod nabízí a zda přijímá rychlost.
 *
 * Test selže, když se tabulka profilů knihovny změní:
 * nabídka a přijetí rychlosti se zjišťují zkušební stavbou, tady se jen
 * hlídá, že výsledek odpovídá známému stavu knihovny.
 */
static const struct {
    const char *id; /**< ID profilu */
    int offered;    /**< 1 = v nabídce převodu */
    int accepts;    /**< 1 = přijímá rychlost */
} s_expected_profiles[] = {
    {"ROM-800-NOMINAL", 1, 0}, {"ROM-800-EXACT", 1, 0}, {"TC-1.21", 1, 1},     {"TAPEMZ-V1-LEGACY", 1, 1},
    {"UNICMT", 1, 0},          {"IC-10.2-NORMAL", 1, 0}, {"IC-10.2-TURBO", 1, 0}, {"IC-10.2-FASTIPL", 1, 0},
    {"PEZIK-2.0A", 1, 0},      {"IC-10.2-CPM", 1, 0},   {"BASIC-1Z016", 0, 0}, {TMZ_PROFILE_ID_MZ800EMU_CMT, 1, 1},
};

/* Výčet profilů: nabídka bez BASIC-1Z016, přijetí rychlosti jen u TC-1.21, LEGACY a MZ800EMU-CMT. */
void test_profile_list(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const size_t n_exp = sizeof(s_expected_profiles) / sizeof(s_expected_profiles[0]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n_exp, (uint32_t)tmz_profile_count(), "library profile table changed");
    uint32_t offered = 0;
    for (size_t k = 0; k < n_exp; k++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(s_expected_profiles[k].id, tmz_profile_get(k)->id, "library profile table changed");
        offered += (uint32_t)s_expected_profiles[k].offered;
    }
    TEST_ASSERT_EQUAL_UINT32(offered, cmttmz_profile_count());
    uint32_t i = 0;
    for (size_t k = 0; k < n_exp; k++) {
        if (!s_expected_profiles[k].offered) continue;
        const char *id = NULL, *desc = NULL;
        int acc = -1;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_profile_get(i, &id, &desc, &acc));
        TEST_ASSERT_EQUAL_STRING(s_expected_profiles[k].id, id);
        TEST_ASSERT_EQUAL_STRING(tmz_profile_get(k)->description, desc);
        TEST_ASSERT_EQUAL_INT_MESSAGE(s_expected_profiles[k].accepts, acc, id);
        i++;
    }
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_profile_get(cmttmz_profile_count(), NULL, NULL, NULL));
}

/*
 * Převod na každý nabízený profil (bez rychlosti, profily s rychlostí i s
 * rychlostí): MZF bajtově shodné.
 */
void test_convert_every_profile(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    st_MZF_COPY mzf = block_mzf(0);
    char prof[64];
    for (uint32_t i = 0; i < cmttmz_profile_count(); i++) {
        const char *id = NULL;
        int acc = 0;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_profile_get(i, &id, NULL, &acc));
        int rc = cmttmz_block_convert_profile(0, id, CMTSPEED_NONE);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, rc, cmttmz_get_edit_error());
        TEST_ASSERT_EQUAL_STRING(id, block_profile(0, prof));
        assert_block_mzf(0, &mzf);
        TEST_ASSERT_TRUE(block_seconds(0) > 0.1);
        if (acc && strcmp(id, TMZ_PROFILE_ID_MZ800EMU_CMT)) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(EXIT_SUCCESS, cmttmz_block_convert_profile(0, id, CMTSPEED_3_1),
                                          cmttmz_get_edit_error());
            assert_block_mzf(0, &mzf);
        }
        TEST_ASSERT_EQUAL_INT(strcmp(id, TMZ_PROFILE_ID_MZ800EMU_CMT) ? 0 : 1,
                              cmttmz_block_speed_changeable(0, CMTSPEED_1_1));
    }
}

/* ================================================================
 * DALŠÍ PŘÍPADY: TEXTY, MEZE, POŠKOZENÉ BLOKY, PŘEVOD
 * ================================================================ */

/*
 * Texty 0x32 v ISO 8859-1 se zalomením řádku 0Dh: čtení na UTF-8 a '\n',
 * zápis beze změny bajtově shodný, změna jiného pole zachová 0Dh i A3h,
 * nový text s '\n' a "\r\n" zapíše 0Dh, znak z ISO 8859-1 projde, znak mimo
 * něj dá '?' a varování, mez 255 znaků platí po převodu.
 */
void test_info_latin1_multiline(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    const st_ENTRY e[] = {{0xFF, "Side A\x0D" "Load with LOAD\"\""}, {0x06, "\xA3" "1.99"}};
    add_archive(f, e, 2);
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("Side A\nLoad with LOAD\"\"", info.comment);
    TEST_ASSERT_EQUAL_STRING("\xC2\xA3" "1.99", info.price);

    /* beze změny: bajtově stejná páska */
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    int changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    assert_serials_same(s);
    assert_image_same(img, size);

    /* změna jiného pole: víceřádkový komentář a cena bajtově */
    strcpy(info.title, "Game");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    const st_ENTRY e2[] = {{0x00, "Game"}, {0xFF, "Side A\x0D" "Load with LOAD\"\""}, {0x06, "\xA3" "1.99"}};
    uint8_t buf[2048];
    uint32_t len = archive_data(e2, 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);

    /* nový víceřádkový text, znak ISO 8859-1 a znak mimo něj */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    strcpy(info.comment, "Line 1\nLine 2\r\nLine 3");
    strcpy(info.author, "Ren\xC3\xA9");          /* René */
    strcpy(info.publisher, "\xC4\x8D" "x");      /* č mimo ISO 8859-1 */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "ISO 8859-1"));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "publisher"));
    TEST_ASSERT_NULL(strstr(cmttmz_get_edit_warning(), "author"));
    TEST_ASSERT_NULL(strstr(cmttmz_get_edit_warning(), "comment"));
    const st_ENTRY e3[] = {{0x00, "Game"}, {0x01, "?x"}, {0x02, "Ren\xE9"}, {0xFF, "Line 1\x0DLine 2\x0DLine 3"},
                           {0x06, "\xA3" "1.99"}};
    len = archive_data(e3, 5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("Line 1\nLine 2\nLine 3", info.comment);
    TEST_ASSERT_EQUAL_STRING("Ren\xC3\xA9", info.author);
    TEST_ASSERT_EQUAL_STRING("?x", info.publisher);

    /* mez 255 znaků po převodu: 255 x é (510 B UTF-8) projde, 256 znaků ne */
    info.origin[0] = '\0';
    for (int i = 0; i < 255; i++) strcat(info.origin, "\xC3\xA9");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_UINT32(510, (uint32_t)strlen(info.origin));
    s = serials();
    strcat(info.origin, "x");
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_EQUAL_STRING("The origin text is longer than 255 characters", cmttmz_get_edit_error());
    assert_serials_same(s);
}

/* Formulář z doby před změnou pásky se odmítne, páska beze změny. */
void test_info_stale_form_refused(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    open_ab();
    st_CMTTMZ_TAPE_INFO stale, fresh;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&stale));
    TEST_ASSERT_EQUAL_UINT(cmttmz_get_tape_serial(), stale.tape_serial);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&fresh));
    strcpy(fresh.title, "Changed elsewhere");
    int changed = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&fresh, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);

    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    strcpy(stale.author, "Somebody");
    changed = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmttmz_tape_info_set(&stale, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_EQUAL_STRING("The tape changed since the tape information was read; read it again", cmttmz_get_edit_error());
    assert_serials_same(s);
    assert_image_same(img, size);

    /* jiná páska: formulář staré pásky se odmítne */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&fresh));
    open_ab();
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmt_tape_info_set(&fresh, &changed));
    TEST_ASSERT_EQUAL_INT(0, changed);
    ASSERT_NAMES("A,B");
}

/*
 * Blok 0x32 mimo začátek pásky a nečitelné bloky 0x32 (počet
 * položek nad data, WORD délky přes blok) formulář nevidí, get je ohlásí,
 * zápis je nechá bajtově a vloží nový blok 0x32 na začátek.
 */
void test_info_other_and_malformed_archive_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    add_mzf_block(f, "A", 1000);
    const st_ENTRY late[] = {{0x00, "Late title"}};
    add_archive(f, late, 1);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("", info.title);
    TEST_ASSERT_EQUAL_UINT32(1, info.other_archive_blocks);
    TEST_ASSERT_EQUAL_INT(1, info.other_archive_block);
    strcpy(info.title, "T");
    int changed = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    ASSERT_NAMES("#32,A,#32,B");
    uint8_t buf[256];
    uint32_t len = archive_data(late, 1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(2)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(2)->data, len);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("T", info.title);
    TEST_ASSERT_EQUAL_INT(2, info.other_archive_block);

    /* nečitelné bloky 0x32 vložené přímo do pásky v paměti */
    open_ab();
    static const uint8_t bad_count[] = {0x04, 0x00, 3, 0x00, 1, 'X'};          /* 3 položky, data na 1 */
    static const uint8_t bad_word[] = {0x20, 0x00, 1, 0x00, 2, 'A', 'B'};      /* WORD 32 přes blok */
    /* data bloků převezme páska a uvolní free(): kopie z malloc */
    st_TZX_BLOCK bl[2];
    bl[0].id = TZX_BLOCK_ID_ARCHIVE_INFO;
    bl[0].length = sizeof(bad_count);
    bl[0].data = (uint8_t *)malloc(sizeof(bad_count));
    bl[1].id = TZX_BLOCK_ID_ARCHIVE_INFO;
    bl[1].length = sizeof(bad_word);
    bl[1].data = (uint8_t *)malloc(sizeof(bad_word));
    TEST_ASSERT_NOT_NULL(bl[0].data);
    TEST_ASSERT_NOT_NULL(bl[1].data);
    memcpy(bl[0].data, bad_count, sizeof(bad_count));
    memcpy(bl[1].data, bad_word, sizeof(bad_word));
    st_CMTTMZ_BLOCK_SPEED sp[2];
    memset(sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(0, bl, sp, 2));
    ASSERT_NAMES("#32,#32,A,B");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    assert_texts_empty(&info);
    TEST_ASSERT_EQUAL_UINT32(2, info.other_archive_blocks);
    TEST_ASSERT_EQUAL_INT(0, info.other_archive_block);
    strcpy(info.title, "New");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    ASSERT_NAMES("#32,#32,#32,A,B");
    TEST_ASSERT_EQUAL_MEMORY(bad_count, tape_block(1)->data, sizeof(bad_count));
    TEST_ASSERT_EQUAL_MEMORY(bad_word, tape_block(2)->data, sizeof(bad_word));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("New", info.title);
}

/**
 * @brief Vloží do pásky A,B jeden blok s daty (kopie, malloc) na pozici 1.
 * @param id ID bloku. @param data Data (NULL jen při len 0). @param len Délka.
 */
static void insert_raw(uint8_t id, const uint8_t *data, uint32_t len)
{
    open_ab();
    st_TZX_BLOCK b;
    b.id = id;
    b.length = len;
    b.data = NULL;
    if (len) {
        b.data = (uint8_t *)malloc(len);
        TEST_ASSERT_NOT_NULL(b.data);
        memcpy(b.data, data, len);
    }
    st_CMTTMZ_BLOCK_SPEED sp;
    memset(&sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_insert_blocks(1, &b, &sp, 1));
}

/* Poškozený skok a volání, skok na sebe, návrat bez volání, vnořená smyčka, poškozený 0x40. */
void test_check_damaged_control_blocks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const uint8_t one[] = {5};
    insert_raw(TZX_BLOCK_ID_JUMP, one, 1);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Jump block is damaged");

    const uint8_t self[] = {0, 0};
    insert_raw(TZX_BLOCK_ID_JUMP, self, 2);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Jump to itself (endless loop)");

    const uint8_t call[] = {3, 0, 1, 0}; /* 3 cíle, data na 1 */
    insert_raw(TZX_BLOCK_ID_CALL_SEQUENCE, call, 4);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Call sequence block is damaged");

    insert_raw(TZX_BLOCK_ID_RETURN_FROM_SEQ, NULL, 0);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Return without a call sequence on the tape");

    const uint8_t mz[] = {0, 0, 0, 0};
    insert_raw(TMZ_BLOCK_ID_MZ_STANDARD_DATA, mz, 4);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed");

    /* blok 0x40 / 0x41 s délkou 0 = nález bez volání parseru */
    insert_raw(TMZ_BLOCK_ID_MZ_STANDARD_DATA, NULL, 0);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed");
    insert_raw(TMZ_BLOCK_ID_MZ_TURBO_DATA, NULL, 0);
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "MZ file block cannot be parsed");

    st_TZX_FILE *f = new_tmz();
    add_word_block(f, TZX_BLOCK_ID_LOOP_START, 2);
    add_word_block(f, TZX_BLOCK_ID_LOOP_START, 2);
    add_mzf_block(f, "A", 1000);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    add_raw_block(f, TZX_BLOCK_ID_LOOP_END, NULL, 0);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    assert_single_finding(1, CMTTMZ_CHECK_WARNING, "Loop inside another loop");

    /* neznámé ID podle smlouvy tmz_block_id_name() ("Unknown"); bloky, které emulátor zapisuje, známé jsou */
    static const uint8_t known[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
                                    0x26, 0x27, 0x2A, 0x2B, 0x30, 0x31, 0x32, 0x33, 0x35, 0x40, 0x41, 0x43, 0x45, 0x46, 0x5A};
    for (size_t k = 0; k < sizeof(known); k++) TEST_ASSERT_TRUE(strcmp(tmz_block_id_name(known[k]), "Unknown") != 0);
    TEST_ASSERT_EQUAL_STRING("Unknown", tmz_block_id_name(0x47));
    TEST_ASSERT_EQUAL_STRING("Unknown", tmz_block_id_name(0x99));
}

/* Počet nálezů má mez 500 a souhrnný informační nález s blokem -1. */
void test_check_findings_capped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    enum { CALLS = 600 };
    uint8_t data[2 + 2 * CALLS];
    data[0] = (uint8_t)(CALLS & 0xFF);
    data[1] = (uint8_t)(CALLS >> 8);
    for (int k = 0; k < CALLS; k++) {
        data[2 + 2 * k] = (uint8_t)((uint16_t)-30000 & 0xFF);
        data[3 + 2 * k] = (uint8_t)((uint16_t)-30000 >> 8);
    }
    insert_raw(TZX_BLOCK_ID_CALL_SEQUENCE, data, sizeof(data));
    st_CHECK c = run_check();
    TEST_ASSERT_EQUAL_UINT32(CMTTMZ_CHECK_MAX_ITEMS + 1, c.count);
    for (uint32_t k = 0; k < CMTTMZ_CHECK_MAX_ITEMS; k++) TEST_ASSERT_EQUAL_INT(1, c.items[k].block_id);
    TEST_ASSERT_EQUAL_INT(-1, c.items[CMTTMZ_CHECK_MAX_ITEMS].block_id);
    TEST_ASSERT_EQUAL_INT(CMTTMZ_CHECK_INFO, c.items[CMTTMZ_CHECK_MAX_ITEMS].severity);
    TEST_ASSERT_EQUAL_STRING("More problems were found; only 500 of them are listed.", c.items[CMTTMZ_CHECK_MAX_ITEMS].text);
    g_free(c.items);
}

/**
 * @brief Přidá do souboru blok 0x46 daného profilu s parametry.
 * @param f Soubor. @param id Profil. @param args Parametry. @param name Jméno MZF.
 */
static void add_profile_block(st_TZX_FILE *f, const char *id, const st_TMZ_PROFILE_ARGS *args, const char *name)
{
    st_MZF_HEADER hdr;
    uint8_t body[TEST_BODY_SIZE];
    make_header(&hdr, name, 0x01, TEST_BODY_SIZE);
    make_body(body, name[0]);
    en_TMZ_PROFILE_ERROR pe = TMZ_PROFILE_OK;
    st_TZX_BLOCK *b = tmz_profile_build_block(id, args, &hdr, body, TEST_BODY_SIZE, &pe);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, id);
    append_owned(f, b);
}

/**
 * @brief PROFILE_ARGS bloku 0x46 vložené pásky.
 * @param block Číslo bloku. @param[out] args Parametry.
 */
static void block_args(int block, st_TMZ_PROFILE_ARGS *args)
{
    st_TMZ_MZFILE mf;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tmz_mzfile_parse(tape_block(block), &mf));
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    int found = 0;
    tmz_tlv_iter_init(&it, mf.block_tlv, mf.block_tlv_length);
    while (tmz_tlv_iter_next(&it, &t) == 1) {
        if (t.tag != TMZ_TAG_PROFILE_ARGS) continue;
        TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, tmz_profile_args_parse((const char *)t.value, t.length, args));
        found = 1;
        break;
    }
    tmz_mzfile_free(&mf);
    TEST_ASSERT_TRUE(found);
}

/*
 * Převod na tentýž profil bez rychlosti nic nemění (TC-1.21 s loader=N
 * a 3:1, UNICMT s vlastními pulzy); s rychlostí zachová ostatní parametry;
 * převod na jiný profil přenese sdílené parametry (bd, vlastní pulzy,
 * rychlost) a ohlásí, co se nepřeneslo.
 */
void test_convert_keeps_profile_settings(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init(&a);
    a.speed = CMTSPEED_3_1;
    a.loader = 0;
    add_profile_block(f, "TC-1.21", &a, "TC");          /* 0 */
    tmz_profile_args_init(&a);
    a.cmtspeed[0] = 400;
    a.cmtspeed[1] = 420;
    a.cmtspeed[2] = 200;
    a.cmtspeed[3] = 230;
    add_profile_block(f, "UNICMT", &a, "UNI");          /* 1 */
    tmz_profile_args_init(&a);
    a.bd = 2400;
    add_profile_block(f, "IC-10.2-NORMAL", &a, "IC");   /* 2 */
    tmz_profile_args_init(&a);
    a.speed = CMTSPEED_3_1;
    a.loader = 0;
    add_profile_block(f, "TC-1.21", &a, "TC2");         /* 3 */
    tmz_profile_args_init(&a);
    a.bd = 2400;
    add_profile_block(f, "IC-10.2-NORMAL", &a, "IC2");  /* 4 */
    open_tape(f, "tmz");

    /* tentýž profil bez rychlosti: beze změny */
    st_SERIALS s = serials();
    size_t size = 0;
    uint8_t *img = tape_image(&size);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, "TC-1.21", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(1, "UNICMT", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(2, "IC-10.2-NORMAL", CMTSPEED_NONE));
    assert_serials_same(s);
    assert_image_same(img, size);
    TEST_ASSERT_EQUAL_INT(0, cmttmz_is_modified());

    /* tentýž profil s rychlostí: loader=N zůstane */
    st_MZF_COPY mzf3 = block_mzf(3);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(3, "TC-1.21", CMTSPEED_2_1));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    block_args(3, &a);
    TEST_ASSERT_EQUAL_INT(0, a.loader);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_2_1, a.speed);
    assert_block_mzf(3, &mzf3);

    /* jiný profil se sdíleným parametrem: bd 2400 se přenese, bez varování */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(2, "IC-10.2-TURBO", CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    block_args(2, &a);
    TEST_ASSERT_EQUAL_UINT16(2400, a.bd);

    /* jiný profil bez rychlosti v Bd: varování */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(4, "ROM-800-NOMINAL", CMTSPEED_NONE));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "Settings of profile IC-10.2-NORMAL not carried over"));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "baud rate"));

    /* TC-1.21 3:1 bez předzavaděče -> MZ800EMU-CMT: rychlost se přenese, předzavaděč ne */
    st_MZF_COPY mzf0 = block_mzf(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_NONE));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "Settings of profile TC-1.21 not carried over"));
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "preloader setting"));
    TEST_ASSERT_NULL(strstr(cmttmz_get_edit_warning(), "speed,"));
    st_CMTTMZ_BLOCK_SPEED sp;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(0, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_3_1, sp.speed);
    assert_block_mzf(0, &mzf0);

    /* UNICMT s vlastními pulzy -> MZ800EMU-CMT: pulzy se přenesou (CUSTOM, FIXED) */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(1, TMZ_PROFILE_ID_MZ800EMU_CMT, CMTSPEED_NONE));
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_test_get_side(1, &sp, NULL));
    TEST_ASSERT_EQUAL_INT(CMTTMZ_SPEED_ORIGIN_FIXED, sp.origin);
    TEST_ASSERT_EQUAL_INT(CMTSPEED_CUSTOM, sp.speed);
    block_args(1, &a);
    TEST_ASSERT_EQUAL_UINT16(400, a.cmtspeed[0]);
    TEST_ASSERT_EQUAL_UINT16(230, a.cmtspeed[3]);
}

/*
 * Cizí zalomení 0Ah a 0Dh 0Ah se čtou jako '\n'; bajty 80h-9Fh
 * (C1) jako '?'. Nezměněná položka zůstane bajtově (i s 0Ah a 85h); upravená
 * položka s bajtem ukázaným jako '?' se zapíše s 3Fh a s varováním.
 */
void test_info_foreign_breaks_and_c1(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_TZX_FILE *f = new_tmz();
    const st_ENTRY e[] = {{0x00, "Big\x85Game"}, {0xFF, "Line 1\x0ALine 2\x0D\x0ALine 3"}};
    add_archive(f, e, 2);
    add_mzf_block(f, "A", 1000);
    add_mzf_block(f, "B", 0);
    open_tape(f, "tmz");
    st_CMTTMZ_TAPE_INFO info;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    TEST_ASSERT_EQUAL_STRING("Big?Game", info.title);
    TEST_ASSERT_EQUAL_STRING("Line 1\nLine 2\nLine 3", info.comment);

    /* změna jiného pole: titulek i komentář bajtově, bez varování */
    strcpy(info.author, "Me");
    int changed = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_STRING("", cmttmz_get_edit_warning());
    const st_ENTRY e2[] = {{0x00, "Big\x85Game"}, {0x02, "Me"}, {0xFF, "Line 1\x0ALine 2\x0D\x0ALine 3"}};
    uint8_t buf[2048];
    uint32_t len = archive_data(e2, 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);

    /* úprava titulku s bajtem 85h: 3Fh a varování s klíčem title; úprava komentáře: zalomení 0Dh, bez varování */
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_get(&info));
    strcpy(info.title, "Big?Game!");
    strcat(info.comment, ".");
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_tape_info_set(&info, &changed));
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_NOT_NULL(strstr(cmttmz_get_edit_warning(), "title"));
    TEST_ASSERT_NULL(strstr(cmttmz_get_edit_warning(), "comment"));
    const st_ENTRY e3[] = {{0x00, "Big?Game!"}, {0x02, "Me"}, {0xFF, "Line 1\x0DLine 2\x0DLine 3."}};
    len = archive_data(e3, 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(len, tape_block(0)->length);
    TEST_ASSERT_EQUAL_MEMORY(buf, tape_block(0)->data, len);
}

/**
 * @brief Přepíše v PROFILE_ARGS bloku 0x46 každé '=' na ';' (parametry pak nejdou přečíst).
 * @param b Blok (data se mění na místě).
 */
static void spoil_profile_args(st_TZX_BLOCK *b)
{
    st_TMZ_MZFILE mf;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tmz_mzfile_parse(b, &mf));
    st_TMZ_TLV_ITER it;
    st_TMZ_TLV t;
    int found = 0;
    tmz_tlv_iter_init(&it, mf.block_tlv, mf.block_tlv_length);
    while (tmz_tlv_iter_next(&it, &t) == 1) {
        if (t.tag != TMZ_TAG_PROFILE_ARGS) continue;
        uint8_t *v = (uint8_t *)t.value; /* ukazuje do dat bloku */
        for (uint32_t i = 0; i < t.length; i++)
            if (v[i] == '=') v[i] = ';';
        found = 1;
    }
    tmz_mzfile_free(&mf);
    TEST_ASSERT_TRUE(found);
}

/*
 * Převod bloku s vnuceným součtem hlavičky (ck_header, část
 * bloku mimo PROFILE_ARGS) to ohlásí varováním; blok, který profil sám zapsal,
 * varování nemá. Nečitelné PROFILE_ARGS: jen varování o nečitelných nastaveních,
 * žádná nepřenesená skupina.
 */
void test_convert_rebuilt_parts_and_unreadable_args(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    static const uint8_t ck[2] = {0x12, 0x34};
    st_TZX_FILE *f = new_tmz();
    st_TMZ_PROFILE_ARGS a;
    tmz_profile_args_init(&a);
    a.ck_header = ck;
    add_profile_block(f, "ROM-800-NOMINAL", &a, "CK");       /* 0: vnucený součet hlavičky */
    tmz_profile_args_init(&a);
    add_profile_block(f, "ROM-800-NOMINAL", &a, "OK");       /* 1: blok profilu */
    tmz_profile_args_init(&a);
    a.legacy_format = TMZ_LEGACY_NORMAL;
    {
        st_MZF_HEADER hdr;
        uint8_t body[TEST_BODY_SIZE];
        make_header(&hdr, "BAD", 0x01, TEST_BODY_SIZE);
        make_body(body, 'B');
        en_TMZ_PROFILE_ERROR pe = TMZ_PROFILE_OK;
        st_TZX_BLOCK *b = tmz_profile_build_block("TAPEMZ-V1-LEGACY", &a, &hdr, body, TEST_BODY_SIZE, &pe);
        TEST_ASSERT_NOT_NULL(b);
        spoil_profile_args(b);
        append_owned(f, b);                                    /* 2: nečitelné PROFILE_ARGS */
    }
    open_tape(f, "tmz");

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(0, "TC-1.21", CMTSPEED_NONE));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_edit_warning(), "for example checksums"), cmttmz_get_edit_warning());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(1, "TC-1.21", CMTSPEED_NONE));
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_edit_warning(), "checksums"), cmttmz_get_edit_warning());

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmttmz_block_convert_profile(2, "TAPEMZ-V1-LEGACY", CMTSPEED_2_1));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cmttmz_get_edit_warning(), "could not be read"), cmttmz_get_edit_warning());
    TEST_ASSERT_NULL_MESSAGE(strstr(cmttmz_get_edit_warning(), "not carried over"), cmttmz_get_edit_warning());
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_info_get_without_metadata);
    RUN_TEST(test_info_set_inserts_blocks_and_roundtrip);
    RUN_TEST(test_info_foreign_entries_survive);
    RUN_TEST(test_info_empty_form_removes_blocks);
    RUN_TEST(test_info_machine_keeps_clock_and_warns);
    RUN_TEST(test_info_storable_mzf);
    RUN_TEST(test_info_wrapper_refused_outside_stop);
    RUN_TEST(test_check_balanced_tape_no_findings);
    RUN_TEST(test_check_findings);
    RUN_TEST(test_convert_emu_to_rom_and_back);
    RUN_TEST(test_convert_same_is_noop);
    RUN_TEST(test_convert_errors);
    RUN_TEST(test_profile_list);
    RUN_TEST(test_convert_every_profile);
    RUN_TEST(test_info_latin1_multiline);
    RUN_TEST(test_info_stale_form_refused);
    RUN_TEST(test_info_other_and_malformed_archive_blocks);
    RUN_TEST(test_check_damaged_control_blocks);
    RUN_TEST(test_check_findings_capped);
    RUN_TEST(test_convert_keeps_profile_settings);
    RUN_TEST(test_info_foreign_breaks_and_c1);
    RUN_TEST(test_convert_rebuilt_parts_and_unreadable_args);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
