/**
 * @file test_cmthack_tape.c
 * @brief Unit testy CMT hacku nad vloženou páskou (cmthack_load_file(), cmthack_read_mzf_body()).
 *
 * Testy volají obě pasti ROM přímo se stejnými registry, jaké nastaví patch
 * ROM (hlavička: HL = adresa hlavičky; tělo: HL = adresa, BC = délka),
 * a čtou výsledek z AF (CY = 0 OK; CY = 1, A = 1 chyba, A = 2 Break).
 * Testovací prostředí nemá interaktivní GUI (baseui_filechooser_can_wait()
 * vrací false), takže "chování jako bez pásky" je headless Break.
 *
 * Pokrývá:
 *   - MZF z aktuálního bloku, posun pásky na další blok s MZF po těle
 *     (přes bloky bez MZF), konec pásky = Break, transport beze změny,
 *   - jen hlavička bez těla: další hlavička vezme další blok (bez dvojího
 *     posunu), posun pásky mezi hlavičkou a tělem uživatelem nebo MCP,
 *     konec pásky zrušený přesunem pásky,
 *   - blok bez MZF (tón) a blok 0x46 s fsize jiným než tělo = jako bez pásky,
 *   - blok 0x40 s fsize jiným než tělo (tělo, které blok má; po chybě posun;
 *     chyba na posledním bloku s MZF -> další LOAD je konec pásky),
 *   - hrající a pozastavená páska: první načtení transport zastaví,
 *     nahrávání (RECORD) hack pásku nepoužije (i pojistka stavu RECORD),
 *   - blok 0x41 NORMAL a blok 0x46 profilu ROM-800-NOMINAL,
 *   - oprava terminátoru jména (fix_fname_terminator),
 *   - stav hacku ve snapshotu (konec pásky, hlavička bez těla i s posunem
 *     pásky mezi pastmi), ověření hodnot, reset stroje stav vynuluje.
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
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
#include "libs/tzx/tzx.h"
#include "libs/tmz/tmz.h"
#include "libs/tmz/tmz_blocks.h"
#include "libs/tmz/tmz_profile.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "libs/mzf/mzf.h"
#include "libs/generic_driver/generic_driver.h"
#include "libs/generic_driver/memory_driver.h"
#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"

/** @brief Adresa hlavičky v RAM (HL pasti hlavičky). */
#define HDR_ADDR 0x2000
/** @brief Adresa těla v RAM (HL pasti těla). */
#define BODY_ADDR 0x3000
/** @brief Délka těla testovacích MZF. */
#define BODY_SIZE 64

/**
 * @brief Bajt paměti tak, jak ho zapisuje memory_load_block(MEMORY_LOAD_MAPED).
 *
 * Čte přes ukazatele zápisu mapy paměti (memram_write), stejné, jaké
 * používá memory_load_block(); test tak nezávisí na tom, co je namapované
 * pro čtení.
 */
#define MEM(addr) (g_memory.memram_write[((addr) & 0xFFFF) >> 12][(addr) & 0x0FFF])

/** @brief Dočasný soubor pásky (maže tearDown). */
static char *s_path = NULL;

/** @brief Uložené volby (obnovuje tearDown). */
static unsigned s_saved_fix;

/**
 * @brief Vrátí cestu dočasného souboru s danou příponou.
 * @param ext Přípona bez tečky. @return Cesta (uvolní tearDown).
 */
static const char *tmp_path(const char *ext)
{
    gchar *name = g_strdup_printf("mztest_cmthack_tape_%u.%s", (unsigned)g_random_int(), ext);
    g_free(s_path);
    s_path = g_build_filename(g_get_tmp_dir(), name, NULL);
    g_free(name);
    return s_path;
}

/**
 * @brief Bajt těla programu b na pozici i (programy se liší).
 * @param b Číslo programu. @param i Pozice. @return Bajt.
 */
static uint8_t body_byte(int b, int i)
{
    return (uint8_t)((i * 37 + 11 + 5 * b) & 0xFF);
}

/**
 * @brief Vyplní hlavičku MZF programu b (jméno "P<b>", tělo BODY_SIZE B na 1200h).
 * @param hdr Hlavička (pořadí hostitele). @param b Číslo programu.
 */
static void make_header(st_MZF_HEADER *hdr, int b)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->ftype = 0x01;
    memset(&hdr->fname, 0x0D, sizeof(hdr->fname));
    ((uint8_t *)&hdr->fname)[0] = 'P';
    ((uint8_t *)&hdr->fname)[1] = (uint8_t)('0' + b);
    hdr->fsize = BODY_SIZE;
    hdr->fstrt = 0x1200;
    hdr->fexec = 0x1200;
}

/**
 * @brief Uloží MZT s programy 0 .. count-1.
 * @param count Počet programů (nejvýš 4). @return Cesta.
 */
static const char *save_mzt(int count)
{
    uint8_t raw[4 * (128 + BODY_SIZE)];
    for (int b = 0; b < count; b++) {
        st_MZF_HEADER hdr;
        make_header(&hdr, b);
        uint8_t *m = raw + b * (128 + BODY_SIZE);
        memcpy(m, &hdr, 128);
        for (int i = 0; i < BODY_SIZE; i++)
            m[128 + i] = body_byte(b, i);
    }
    const char *path = tmp_path("mzt");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)raw, count * (128 + BODY_SIZE), NULL));
    return path;
}

/** @brief Nový prázdný TMZ. @return Soubor (uvolní save_tmz()). */
static st_TZX_FILE *new_tmz(void)
{
    st_TZX_FILE *f = (st_TZX_FILE *)calloc(1, sizeof(st_TZX_FILE));
    TEST_ASSERT_NOT_NULL(f);
    tmz_header_init(&f->header);
    f->is_tmz = true;
    return f;
}

/**
 * @brief Připojí blok do souboru (data převezme soubor).
 * @param f Soubor. @param b Blok (uvolní se, data ne).
 */
static void append_block(st_TZX_FILE *f, st_TZX_BLOCK *b)
{
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, b));
    b->data = NULL;
    tmz_block_free(b);
}

/**
 * @brief Blok 0x46 MZ800EMU-CMT s programem b.
 * @param f Soubor. @param b Číslo programu.
 */
static void add_mzf46(st_TZX_FILE *f, int b)
{
    st_MZF_HEADER hdr;
    make_header(&hdr, b);
    uint8_t body[BODY_SIZE];
    for (int i = 0; i < BODY_SIZE; i++)
        body[i] = body_byte(b, i);
    append_block(f, cmttmz_build_mzf_block(&hdr, body, BODY_SIZE, CMTSPEED_1_1, NULL, 1000));
}

/**
 * @brief Blok 0x46 s programem b, v jehož hlavičce je fsize jiný než délka těla.
 * @param f Soubor. @param b Číslo programu. @param fsize fsize v hlavičce bloku.
 */
static void add_mzf46_fsize(st_TZX_FILE *f, int b, uint16_t fsize)
{
    st_MZF_HEADER hdr;
    make_header(&hdr, b);
    uint8_t body[BODY_SIZE];
    for (int i = 0; i < BODY_SIZE; i++)
        body[i] = body_byte(b, i);
    st_TZX_BLOCK *blk = cmttmz_build_mzf_block(&hdr, body, BODY_SIZE, CMTSPEED_1_1, NULL, 1000);
    TEST_ASSERT_NOT_NULL(blk);
    /* hlavička MZF v datech bloku: najít podle prvních 19 bajtů (typ + jméno) */
    uint8_t *h = NULL;
    for (uint32_t o = 0; o + 128 <= blk->length; o++) {
        if (0 == memcmp(blk->data + o, &hdr, 0x12)) {
            h = blk->data + o;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(h);
    h[0x12] = (uint8_t)(fsize & 0xFF);
    h[0x13] = (uint8_t)(fsize >> 8);
    append_block(f, blk);
}

/**
 * @brief Blok 0x40 Standard Data s programem b a fsize hlavičky jiným než tělo.
 * @param f Soubor. @param b Číslo programu. @param fsize fsize v hlavičce bloku.
 */
static void add_mzf40_fsize(st_TZX_FILE *f, int b, uint16_t fsize)
{
    st_MZF mzf;
    make_header(&mzf.header, b);
    uint8_t body[BODY_SIZE];
    for (int i = 0; i < BODY_SIZE; i++)
        body[i] = body_byte(b, i);
    mzf.body = body;
    mzf.body_size = BODY_SIZE;
    st_TZX_BLOCK *blk = tmz_block_from_mzf(&mzf, TMZ_MACHINE_MZ800, TMZ_PULSESET_800, 1000);
    TEST_ASSERT_NOT_NULL(blk);
    TEST_ASSERT_EQUAL_HEX8(TMZ_BLOCK_ID_MZ_STANDARD_DATA, blk->id);
    /* data: DWORD délka, stroj, sada pulzů, pauza (2 B), hlavička MZF, body_size, tělo */
    uint8_t *h = blk->data + 4 + 4;
    h[0x12] = (uint8_t)(fsize & 0xFF);
    h[0x13] = (uint8_t)(fsize >> 8);
    append_block(f, blk);
}

/**
 * @brief Blok 0x12 Pure Tone (bez MZF).
 * @param f Soubor.
 */
static void add_tone(st_TZX_FILE *f)
{
    st_TZX_BLOCK b;
    b.id = TZX_BLOCK_ID_PURE_TONE;
    b.length = 4;
    b.data = (uint8_t *)malloc(4);
    TEST_ASSERT_NOT_NULL(b.data);
    b.data[0] = 0x78; /* 2168 T */
    b.data[1] = 0x08;
    b.data[2] = 0x2C; /* 300 pulzů */
    b.data[3] = 0x01;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &b));
}

/**
 * @brief Uloží TMZ a otevře ho ve Virtual CMT.
 * @param f Soubor (uvolní se).
 */
static void open_tmz(st_TZX_FILE *f)
{
    st_HANDLER h;
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_save(&h, f));
    const char *path = tmp_path("tmz");
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const gchar *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
    tzx_free(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)path));
}

/** @brief Číslo otevřeného bloku pásky (-1 bez bloku). @return Číslo bloku. */
static int open_block(void)
{
    return (g_cmt.ext && g_cmt.ext->block) ? cmtext_block_get_block_id(g_cmt.ext->block) : -1;
}

/**
 * @brief Past hlavičky (OUT 01h): HL = HDR_ADDR.
 * @return AF po pasti.
 */
static uint16_t trap_header(void)
{
    for (unsigned i = 0; i < 128; i++)
        MEM(HDR_ADDR + i) = 0xAA;
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, HDR_ADDR);
    cmthack_load_file();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}

/**
 * @brief Past těla (OUT 02h): HL = BODY_ADDR, BC = délka.
 * @param bc Požadovaná délka. @return AF po pasti.
 */
static uint16_t trap_body(uint16_t bc)
{
    for (unsigned i = 0; i < 256; i++)
        MEM(BODY_ADDR + i) = 0xAA;
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_AF, 0x0000);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_HL, BODY_ADDR);
    z80_set_reg(g_mzarch_main.cpu, Z80_REG_BC, bc);
    cmthack_read_mzf_body();
    return (uint16_t)z80_get_reg(g_mzarch_main.cpu, Z80_REG_AF);
}

/** @brief AF výsledku OK (CY = 0). @param af AF. */
static void assert_ok(uint16_t af)
{
    TEST_ASSERT_EQUAL_HEX16(0, af & 0x01);
}

/** @brief AF výsledku Break (CY = 1, A = 2). @param af AF. */
static void assert_break(uint16_t af)
{
    TEST_ASSERT_EQUAL_HEX16(0x01, af & 0x01);
    TEST_ASSERT_EQUAL_HEX8(2, af >> 8);
}

/**
 * @brief Ověří hlavičku programu b v RAM na HDR_ADDR.
 * @param b Číslo programu.
 */
static void assert_header(int b)
{
    TEST_ASSERT_EQUAL_HEX8(0x01, MEM(HDR_ADDR));
    TEST_ASSERT_EQUAL_HEX8('P', MEM(HDR_ADDR + 1));
    TEST_ASSERT_EQUAL_HEX8('0' + b, MEM(HDR_ADDR + 2));
    TEST_ASSERT_EQUAL_HEX8(BODY_SIZE, MEM(HDR_ADDR + 0x12));
}

/**
 * @brief Ověří tělo programu b v RAM na BODY_ADDR.
 * @param b Číslo programu. @param n Počet bajtů.
 */
static void assert_body(int b, int n)
{
    for (int i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(body_byte(b, i), MEM(BODY_ADDR + i), "body byte");
    TEST_ASSERT_EQUAL_HEX8(0xAA, MEM(BODY_ADDR + n));
}

void setUp(void)
{
    s_saved_fix = g_cmthack.fix_fname_terminator;
    g_cmthack.fix_fname_terminator = 1;
    cmthack_reset();
}

void tearDown(void)
{
    g_cmthack.fix_fname_terminator = s_saved_fix;
    cmthack_reset();
    cmt_eject();
    g_cmt.state = CMT_STATE_STOP;
    g_cmt.paused = 0;
    if (s_path) {
        g_remove(s_path);
        g_free(s_path);
        s_path = NULL;
    }
}

/*
 * MZT se dvěma programy: hlavička a tělo programu 0, páska na bloku 1,
 * hlavička a tělo programu 1, páska zůstane na posledním bloku, další
 * hlavička = konec pásky (Break). Transport zůstává ve STOP.
 */
void test_hack_loads_programs_in_order(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(2)));
    TEST_ASSERT_EQUAL_INT(0, open_block());

    assert_ok(trap_header());
    assert_header(0);
    TEST_ASSERT_EQUAL_INT(0, open_block()); /* po hlavičce se páska nehýbe */
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(1, open_block());

    assert_ok(trap_header());
    assert_header(1);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(1, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(1, open_block()); /* poslední blok: páska stojí na něm */

    assert_break(trap_header());
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_break(trap_header()); /* konec pásky trvá */
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/* Posun přes bloky bez MZF (text, tón) na další blok s MZF; pauza transportu se načtením zastaví. */
void test_hack_skips_blocks_without_mzf(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf46(f, 0);
    st_TZX_BLOCK t;
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_block_create_text_description("between", &t));
    TEST_ASSERT_EQUAL_INT(TZX_OK, tzx_file_append_block(f, &t));
    add_tone(f);
    add_mzf46(f, 1);
    open_tmz(f);
    cmt_play_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);

    assert_ok(trap_header());
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    assert_ok(trap_body(BODY_SIZE));
    TEST_ASSERT_EQUAL_INT(3, open_block());
    assert_ok(trap_header());
    assert_header(1);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/**
 * @brief Posune GDG čas (páska hraje dál).
 * @param ticks Takty GDG.
 */
static void advance_ticks(unsigned ticks)
{
    g_gdg.total_elapsed.ticks += ticks;
}

/**
 * @brief Dvě načtení z pásky MZT se dvěma programy, třetí = konec pásky.
 *
 * Ověří, že první načtení zastavilo transport a že se žádný program
 * nepřeskočil.
 */
static void load_both_programs_after_stop(void)
{
    assert_ok(trap_header());
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    TEST_ASSERT_EQUAL_INT(0, g_cmt.paused);
    assert_header(0);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(1, open_block());
    advance_ticks(5 * GDGCLK_BASE); /* čas běží dál, páska stojí */
    (void)cmt_read_data();
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_ok(trap_header());
    assert_header(1);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(1, BODY_SIZE);
    assert_break(trap_header());
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
}

/*
 * Páska hraje (PLAY, uprostřed prvního bloku): první načtení hackem
 * transport zastaví, dá program 1, páska stojí na programu 2, druhé
 * načtení dá program 2; nic se nepřeskočí.
 */
void test_hack_stops_playing_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(2)));
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(GDGCLK_BASE / 2);
    (void)cmt_read_data();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    load_both_programs_after_stop();
}

/* Totéž z pauzy uprostřed bloku (PLAY + pauza). */
void test_hack_stops_paused_tape(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(2)));
    g_cmt.paused = 0;
    cmt_play();
    advance_ticks(GDGCLK_BASE / 2);
    (void)cmt_read_data();
    cmt_pause(1);
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    load_both_programs_after_stop();
}

/*
 * Nahrávání (RECORD): hack pásku nepoužije (headless Break), nahrávání
 * pokračuje. cmt_record_to_file() nahrává do vložené pásky cmt_tmz (záznam
 * se po STOP připojí na její konec), hack ji tedy odmítne kontrolou
 * CMT_TEST_RECORD v cmthack_tape_load_header() (tu samostatně ověřuje
 * i test_hack_record_state_check()).
 */
void test_hack_ignores_recording(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(2)));
    gchar *wav = g_build_filename(g_get_tmp_dir(), "mztest_cmthack_tape_rec.wav", NULL);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_record_to_file(wav));
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_TRUE(g_cmt.ext == g_cmt_tmz_extension); /* nahrává se do vložené pásky */
    assert_break(trap_header());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    cmt_stop();
    g_remove(wav);
    g_free(wav);
}

/*
 * Pojistka RECORD v cmthack_tape_load_header(): stav RECORD nad vloženou
 * páskou cmt_tmz (stav je tu nastavený přímo, bez bufferu záznamu) hack
 * pásku nepoužije - bez kontroly by transport zastavil a dal hlavičku
 * programu 0.
 */
void test_hack_record_state_check(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(2)));
    TEST_ASSERT_TRUE(g_cmt.ext == g_cmt_tmz_extension);
    g_cmt.state = CMT_STATE_RECORD;
    assert_break(trap_header());
    TEST_ASSERT_TRUE(CMT_TEST_RECORD);
    TEST_ASSERT_EQUAL_HEX8(0xAA, MEM(HDR_ADDR)); /* hlavička nezapsaná */
    g_cmt.state = CMT_STATE_STOP;
}

/*
 * Chyba těla na posledním bloku s MZF (fsize v hlavičce větší než tělo):
 * ROM dostane chybu, za blokem už žádný MZF není, takže další LOAD je
 * konec pásky (headless Break), ne znovu tentýž blok.
 */
void test_hack_error_on_last_block_then_break(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf46(f, 0);
    add_mzf40_fsize(f, 1, 100);
    open_tmz(f);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
    assert_ok(trap_header());
    uint16_t af = trap_body(100);
    TEST_ASSERT_EQUAL_HEX16(0x01, af & 0x01);
    TEST_ASSERT_EQUAL_HEX8(1, af >> 8); /* chyba */
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_break(trap_header()); /* konec pásky */
    TEST_ASSERT_EQUAL_INT(1, open_block());
}

/* Blok 0x41 ve formátu NORMAL (převod v1 na 0x46) a blok 0x46 profilu ROM-800-NOMINAL: hack čte oba. */
void test_hack_reads_block41_and_rom800(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    st_TMZ_MZ_TURBO_DATA td;
    memset(&td, 0, sizeof(td));
    td.machine = TMZ_MACHINE_MZ800;
    td.pulseset = TMZ_PULSESET_800;
    td.format = TMZ_FORMAT_NORMAL;
    td.speed = CMTSPEED_1_1;
    td.pause_ms = 1000;
    make_header(&td.mzf_header, 0);
    uint8_t body[BODY_SIZE];
    for (int i = 0; i < BODY_SIZE; i++)
        body[i] = body_byte(0, i);
    st_TZX_BLOCK *b41 = tmz_block_create_mz_turbo(&td, body, BODY_SIZE);
    TEST_ASSERT_NOT_NULL(b41);
    TEST_ASSERT_EQUAL_HEX8(TMZ_BLOCK_ID_MZ_TURBO_DATA, b41->id);
    append_block(f, b41);

    st_MZF_HEADER hdr;
    make_header(&hdr, 1);
    for (int i = 0; i < BODY_SIZE; i++)
        body[i] = body_byte(1, i);
    en_TMZ_PROFILE_ERROR err = TMZ_PROFILE_OK;
    append_block(f, tmz_profile_build_block("ROM-800-NOMINAL", NULL, &hdr, body, BODY_SIZE, &err));
    open_tmz(f);
    TEST_ASSERT_EQUAL_INT(0, open_block()); /* 0x41 NORMAL se otevře (player ho přehraje) */

    assert_ok(trap_header());
    assert_header(0);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_ok(trap_header());
    assert_header(1);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(1, BODY_SIZE);
}

/*
 * Jen hlavička bez těla (hledání programu podle jména): další hlavička
 * vezme další blok; páska se posune jen jednou.
 */
void test_hack_header_without_body(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    assert_header(0);
    assert_ok(trap_header());
    assert_header(1);
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_ok(trap_body(BODY_SIZE));
    assert_body(1, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(2, open_block());
    assert_ok(trap_header());
    assert_header(2);
}

/*
 * Posun pásky (MCP seek) mezi hlavičkou a tělem: tělo patří načtené
 * hlavičce, páska zůstane tam, kam ji přesunul uživatel.
 */
void test_hack_seek_between_header_and_body(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    assert_header(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(2));
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(2, open_block());
    assert_ok(trap_header());
    assert_header(2);
}

/*
 * Ověření věty dokumentace: úprava pásky mezi
 * hlavičkou a tělem (režim Direct) zapomene načtenou hlavičku. Požadavek
 * na tělo pak skončí Break (tělo se nenačte), páska zůstane na bloku
 * hlavičky a nové LOAD načte jeho hlavičku znovu.
 */
void test_final_edit_between_header_and_body_forgets_header(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    assert_header(0);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_tape_move_block(2, 1));
    assert_break(trap_body(BODY_SIZE));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xAA, MEM(BODY_ADDR), "no body is loaded");
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_ok(trap_header());
    assert_header(0);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
}

/* Konec pásky zruší přesun pásky (seek na blok 0): další hlavička je zase program 0. */
void test_hack_end_cleared_by_seek(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(1)));
    assert_ok(trap_header());
    assert_ok(trap_body(BODY_SIZE));
    assert_break(trap_header());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(0));
    assert_ok(trap_header());
    assert_header(0);
}

/* Aktuální blok bez MZF (tón): jako bez pásky (headless Break), páska se nehýbe. */
void test_hack_block_without_mzf_breaks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_tone(f);
    add_mzf46(f, 0);
    open_tmz(f);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_break(trap_header());
    TEST_ASSERT_EQUAL_INT(0, open_block());
}

/*
 * Blok 0x46 s fsize v hlavičce jiným než délka těla MZF nést neumí
 * (tmz_block_to_mzf() ho odmítne): jako blok bez MZF. Po těle
 * předchozího programu se přes něj páska posune.
 */
void test_hack_block46_fsize_mismatch_is_not_mzf(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf46_fsize(f, 0, 32);
    add_mzf46(f, 1);
    add_mzf46_fsize(f, 2, 100);
    open_tmz(f);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_break(trap_header());

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(1));
    assert_ok(trap_header());
    assert_ok(trap_body(BODY_SIZE));
    TEST_ASSERT_EQUAL_INT(1, open_block()); /* blok 2 MZF nenese: konec pásky */
    assert_break(trap_header());
}

/*
 * Blok 0x40 s fsize v hlavičce jiným než délka těla: hlavička jde do RAM
 * jak je, tělo je to, které blok má. ROM chce BC = fsize bajtů: kratší
 * než tělo -> prvních BC bajtů; delší než tělo -> chyba (A = 1), RAM se
 * nemění a páska se posune na další program (jako skutečná páska).
 */
void test_hack_block40_fsize_mismatch(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf40_fsize(f, 0, 32);
    add_mzf40_fsize(f, 1, 100);
    add_mzf46(f, 2);
    open_tmz(f);
    TEST_ASSERT_EQUAL_INT(0, open_block());

    assert_ok(trap_header());
    TEST_ASSERT_EQUAL_HEX8(32, MEM(HDR_ADDR + 0x12));
    assert_ok(trap_body(32));
    assert_body(0, 32);
    TEST_ASSERT_EQUAL_INT(1, open_block());

    assert_ok(trap_header());
    TEST_ASSERT_EQUAL_HEX8(100, MEM(HDR_ADDR + 0x12));
    uint16_t af = trap_body(100);
    TEST_ASSERT_EQUAL_HEX16(0x01, af & 0x01);
    TEST_ASSERT_EQUAL_HEX8(1, af >> 8);
    TEST_ASSERT_EQUAL_HEX8(0xAA, MEM(BODY_ADDR));
    TEST_ASSERT_EQUAL_INT(2, open_block()); /* po chybě další program */
    assert_ok(trap_header());
    assert_header(2);
}

/* Jméno bez terminátoru 0Dh: s fix_fname_terminator se doplní (stejné místo jako u souboru). */
void test_hack_fix_fname_terminator(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    st_MZF_HEADER hdr;
    make_header(&hdr, 0);
    memset(&hdr.fname, 'A', sizeof(hdr.fname));
    uint8_t body[BODY_SIZE];
    memset(body, 0x11, sizeof(body));
    append_block(f, cmttmz_build_mzf_block(&hdr, body, BODY_SIZE, CMTSPEED_1_1, NULL, 1000));
    append_block(f, cmttmz_build_mzf_block(&hdr, body, BODY_SIZE, CMTSPEED_1_1, NULL, 1000));
    open_tmz(f);

    const unsigned off = sizeof(st_MZF_FILENAME) - 2; /* jako cmthack_load_mzf_filename() */
    g_cmthack.fix_fname_terminator = 1;
    assert_ok(trap_header());
    TEST_ASSERT_EQUAL_HEX8(0x0D, MEM(HDR_ADDR + off));
    assert_ok(trap_body(BODY_SIZE));

    g_cmthack.fix_fname_terminator = 0;
    assert_ok(trap_header());
    TEST_ASSERT_EQUAL_HEX8('A', MEM(HDR_ADDR + off));
}

/* Bez pásky beze změny: headless Break. */
void test_hack_without_tape_breaks(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    cmt_eject();
    assert_break(trap_header());
}

/**
 * @brief Uloží snapshot do paměti (jako checkpoint záznamu vstupů).
 * @param[out] buf Buffer (uvolní snap_load()). @param[out] size Velikost.
 */
static void snap_save(uint8_t **buf, size_t *size)
{
    g_emulator.paused = true;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save_to_buffer("cmthack tape", buf, size));
}

/**
 * @brief Načte snapshot z paměti a uvolní buffer.
 * @param buf Buffer. @param size Velikost.
 */
static void snap_load(uint8_t *buf, size_t size)
{
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load_from_buffer(buf, size));
    g_free(buf);
}

/*
 * Scénář (a): jediný program na pásce, LOAD hackem (konec pásky),
 * checkpoint, druhý LOAD skončí jako bez pásky (Break). Po návratu na
 * checkpoint musí druhý LOAD skončit stejně - dřív načtení snapshotu posunulo
 * čítač otevření, konec pásky se zahodil a LOAD vzal blok z pásky znovu.
 */
void test_hack_end_survives_snapshot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(1)));
    assert_ok(trap_header());
    assert_ok(trap_body(BODY_SIZE));
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    assert_break(trap_header());
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_break(trap_header()); /* jako bez checkpointu */
}

/*
 * Snapshot mezi hlavičkou a tělem: po načtení dostane tělo program, jehož
 * hlavičku ROM dostala, a páska se posune na další program.
 */
void test_hack_pending_survives_snapshot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    assert_header(0);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    assert_ok(trap_body(BODY_SIZE));
    TEST_ASSERT_EQUAL_INT(1, open_block());
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(1, open_block());
    assert_ok(trap_header());
    assert_header(1);
}

/*
 * Snapshot mezi hlavičkou a tělem načtený do stroje s vypnutým CMT hackem:
 * rozečtená hlavička se neobnoví, takže po zapnutí hacku první načtení
 * dostane blok, na kterém páska stojí (nepřeskočí ho).
 */
void test_hack_pending_not_restored_with_hack_off(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const unsigned was_installed = CMTHACK_TEST_IS_INSTALLED;
    cmthack_load_rom_patch(1);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    assert_header(0);
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);

    cmthack_load_rom_patch(0);
    snap_load(buf, size);
    st_CMTHACK_TAPE_STATE st;
    TEST_ASSERT_FALSE(cmthack_tape_state_get(&st));
    TEST_ASSERT_EQUAL_INT(0, open_block());

    cmthack_load_rom_patch(1);
    assert_ok(trap_header());
    assert_header(0);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    cmthack_load_rom_patch(was_installed);
}

/*
 * Totéž s posunem pásky mezi hlavičkou a tělem (MCP seek na blok 2):
 * po načtení dostane tělo program 0 a páska zůstane na bloku 2.
 */
void test_hack_pending_moved_survives_snapshot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(3)));
    assert_ok(trap_header());
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmtext_get_container(g_cmt.ext)->cb_open_block(2));
    uint8_t *buf = NULL;
    size_t size = 0;
    snap_save(&buf, &size);
    cmthack_tape_state_clear();
    snap_load(buf, size);
    TEST_ASSERT_EQUAL_INT(2, open_block());
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(2, open_block());
    assert_ok(trap_header());
    assert_header(2);
}

/* Ověření hodnot stavu hacku (jak je loader dostane ze snapshotu). */
void test_hack_state_set_validates(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_TZX_FILE *f = new_tmz();
    add_mzf46(f, 0);
    add_tone(f);
    add_mzf46(f, 1);
    open_tmz(f);
    st_CMTHACK_TAPE_STATE st;
    memset(&st, 0, sizeof(st));
    st.pending = true;
    st.end = true;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmthack_tape_state_set(&st)); /* obojí zároveň */
    st.end = false;
    st.pending_block = 3;
    st.pending_moved = true;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmthack_tape_state_set(&st)); /* mimo pásku */
    st.pending_block = -1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmthack_tape_state_set(&st));
    st.pending_block = 1;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmthack_tape_state_set(&st)); /* tón nenese MZF */
    st.pending_block = 2;
    st.pending_moved = false;
    TEST_ASSERT_EQUAL_INT(EXIT_FAILURE, cmthack_tape_state_set(&st)); /* páska stojí na bloku 0 */
    TEST_ASSERT_FALSE(cmthack_tape_state_get(&st));
    st.pending_block = 0;
    st.pending = true;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmthack_tape_state_set(&st));
    st_CMTHACK_TAPE_STATE got;
    TEST_ASSERT_TRUE(cmthack_tape_state_get(&got));
    TEST_ASSERT_TRUE(got.pending);
    TEST_ASSERT_EQUAL_INT(0, got.pending_block);
    TEST_ASSERT_FALSE(got.pending_moved);
    assert_ok(trap_body(BODY_SIZE));
    assert_body(0, BODY_SIZE);
    TEST_ASSERT_EQUAL_INT(2, open_block());
}

/* Reset stroje vynuluje konec pásky: páska stojí, další LOAD vezme tentýž program znovu. */
void test_hack_reset_clears_end(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension((char *)save_mzt(1)));
    assert_ok(trap_header());
    assert_ok(trap_body(BODY_SIZE));
    assert_break(trap_header());
    cmthack_reset();
    TEST_ASSERT_EQUAL_INT(0, open_block());
    assert_ok(trap_header());
    assert_header(0);
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_hack_loads_programs_in_order);
    RUN_TEST(test_hack_skips_blocks_without_mzf);
    RUN_TEST(test_hack_stops_playing_tape);
    RUN_TEST(test_hack_stops_paused_tape);
    RUN_TEST(test_hack_ignores_recording);
    RUN_TEST(test_hack_record_state_check);
    RUN_TEST(test_hack_error_on_last_block_then_break);
    RUN_TEST(test_hack_reads_block41_and_rom800);
    RUN_TEST(test_hack_header_without_body);
    RUN_TEST(test_hack_seek_between_header_and_body);
    RUN_TEST(test_final_edit_between_header_and_body_forgets_header);
    RUN_TEST(test_hack_end_cleared_by_seek);
    RUN_TEST(test_hack_block_without_mzf_breaks);
    RUN_TEST(test_hack_block46_fsize_mismatch_is_not_mzf);
    RUN_TEST(test_hack_block40_fsize_mismatch);
    RUN_TEST(test_hack_fix_fname_terminator);
    RUN_TEST(test_hack_without_tape_breaks);
    RUN_TEST(test_hack_end_survives_snapshot);
    RUN_TEST(test_hack_pending_survives_snapshot);
    RUN_TEST(test_hack_pending_not_restored_with_hack_off);
    RUN_TEST(test_hack_pending_moved_survives_snapshot);
    RUN_TEST(test_hack_state_set_validates);
    RUN_TEST(test_hack_reset_clears_end);
    return UNITY_END();
}
