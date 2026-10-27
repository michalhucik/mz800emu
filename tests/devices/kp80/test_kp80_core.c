/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_kp80_core.c
 * @brief Testy čistého jádra 80-Zeichen-Karte K&P (kp80_core.c, kp80_font_rom.c).
 *
 * Pokrytí:
 *  - font: CRC-32 a součet celého pole (shoda s 80Z.ROM), glyf "A" na 410h;
 *  - stav po zapnutí, výběr registru, šířky registrů, čitelnost registrů;
 *  - transparentní zápis postupem BKK (R18/R19, OUT (72h),31, bit 7, OUT (71h),A)
 *    s autoinkrementem, platnost R18/R19 až po R31 i "ihned";
 *  - čtení 70h s autoinkrementem i bez, peek bez vedlejších účinků;
 *  - zápis 71h do vybraného registru (volba zapnuta / vypnuta);
 *  - přístup k R31 čtením, status bity, V-blank a update ready v čase;
 *  - VRAM modulo 2 048 a 14bitové přetečení adresy;
 *  - reset, round-trip uloženého stavu a odmítnutí vadných dat;
 *  - vykreslení: "A" na pozici, inverze bitem 7, kurzor a jeho blikání,
 *    start obrazu R12/R13, ořez při větším rozměru.
 *
 * Standalone test: jádro nezávisí na emulátoru ani na MZARCH.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "emulator/mzarch/mz700/devices/kp80/kp80_core.h"


/* ===== Fixture ===== */

/** @brief Testovaná karta. */
static st_KP80 g_kp;

/** @brief Výstupní buffer snímku. */
static uint8_t g_fb[KP80_FB_SIZE];

/** @brief Výchozí registry R0 - R15 podle BKK (KuP80Z.Lib, KuP_Init6845). */
static const uint8_t s_bkk_init[16] = {
    0x7F, 0x50, 0x64, 0x0C, 0x1E, 0x02, 0x19, 0x1C,
    0x78, 0x09, 0x60, 0x09, 0x00, 0x00, 0x00, 0x00
};

/** @brief Glyf "A" v 80Z.ROM na 410h (vlastní xxd souboru). */
static const uint8_t s_glyph_A[16] = {
    0x00, 0x38, 0x44, 0x44, 0x7C, 0x44, 0x44, 0x44,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x41, 0x41, 0x41
};

void setUp(void)
{
    kp80_init(&g_kp, 0);
    memset(g_fb, 0xAA, sizeof(g_fb));
}

void tearDown(void) { }


/* ===== Pomocné funkce ===== */

/** @brief Zápis registru CRTC přes 72h/73h. */
static void crtc_set(unsigned reg, uint8_t v, uint64_t now)
{
    kp80_write(&g_kp, 0x72, (uint8_t) reg, now);
    kp80_write(&g_kp, 0x73, v, now);
}

/** @brief Čtení registru CRTC přes 72h/73h. */
static uint8_t crtc_get(unsigned reg, uint64_t now)
{
    kp80_write(&g_kp, 0x72, (uint8_t) reg, now);
    return kp80_read(&g_kp, 0x73, now, 0xFF);
}

/** @brief Inicializace R0 - R15 smyčkou jako KuP_CRTInit. */
static void bkk_crt_init(uint64_t now)
{
    for (unsigned r = 0; r < 16; r++) {
        crtc_set(r, s_bkk_init[r], now);
    }
}

/** @brief Nastavení update adresy postupem BKK (R18/R19, pak OUT (72h),31). */
static void bkk_set_addr(uint16_t addr, uint64_t now)
{
    crtc_set(18, (uint8_t) (addr >> 8), now);
    crtc_set(19, (uint8_t) addr, now);
    kp80_write(&g_kp, 0x72, 31, now);
}

/** @brief Zápis znaku postupem BKK: čekat na bit 7, pak OUT (71h),A. */
static void bkk_put(uint8_t ch, uint64_t now)
{
    TEST_ASSERT_TRUE(kp80_read(&g_kp, 0x72, now, 0xFF) & KP80_STATUS_UPDATE_READY);
    kp80_write(&g_kp, 0x71, ch, now);
}

/** @brief CRC-32 (polynom EDB88320h), jako zlib.crc32. */
static uint32_t crc32_calc(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/** @brief Bod snímku. */
static uint8_t px(unsigned x, unsigned y)
{
    return g_fb[y * KP80_FB_WIDTH + x];
}

/**
 * @brief Ověří, že buňka (col, row) při 10 řádcích na znak obsahuje glyf
 *        @p glyph, případně invertovaný.
 */
static void assert_cell(unsigned col, unsigned row, const uint8_t *glyph, bool inverted)
{
    for (unsigned ra = 0; ra < 10; ra++) {
        uint8_t bits = glyph[ra];
        if (inverted) {
            bits = (uint8_t) ~bits;
        }
        for (unsigned x = 0; x < 8; x++) {
            unsigned expect = (bits >> (7 - x)) & 1u;
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(expect, px(col * 8 + x, row * 10 + ra), "bod buňky");
        }
    }
}

/** @brief Inicializace BKK + VRAM vyplněná mezerami (KuP_ClrScr). */
static void bkk_screen(uint64_t now)
{
    bkk_crt_init(now);
    bkk_set_addr(0, now);
    for (unsigned i = 0; i < KP80_VRAM_SIZE; i++) {
        bkk_put(' ', now);
    }
}


/* ===== Font ===== */

static void test_font_checksum(void)
{
    /* CRC-32 a součet 80Z.ROM (MD5 b6bc43cd4127eff1f511123fbf98d1ed) spočtené ze souboru. */
    TEST_ASSERT_EQUAL_HEX32(0x626526F7u, crc32_calc(g_kp80_font_rom, KP80_FONT_SIZE));
    uint32_t sum = 0;
    for (unsigned i = 0; i < KP80_FONT_SIZE; i++) {
        sum += g_kp80_font_rom[i];
    }
    TEST_ASSERT_EQUAL_UINT32(87911u, sum);
}

static void test_font_glyph_A(void)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_glyph_A, &g_kp80_font_rom[0x41 * 16], 16);
    TEST_ASSERT_EQUAL_PTR(g_kp80_font_rom, g_kp.font);
}


/* ===== Registry ===== */

static void test_power_on_state(void)
{
    for (unsigned r = 0; r < KP80_NUM_REGS; r++) {
        TEST_ASSERT_EQUAL_UINT8(0, g_kp.reg[r]);
    }
    for (unsigned i = 0; i < KP80_VRAM_SIZE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, g_kp.vram[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.ar);
    TEST_ASSERT_EQUAL_UINT16(0, g_kp.upd_addr);
    TEST_ASSERT_FALSE(g_kp.upd_pending);
    TEST_ASSERT_TRUE(g_kp.opt.read_autoinc);
    TEST_ASSERT_TRUE(g_kp.opt.data_write_to_reg);
    TEST_ASSERT_TRUE(g_kp.opt.addr_on_r31);
    TEST_ASSERT_EQUAL_UINT32(0, g_kp.opt.update_delay);
    /* Registry 0: R6 = 0 zobrazených řádků -> trvalý V-blank; update ready. */
    TEST_ASSERT_EQUAL_HEX8(KP80_STATUS_UPDATE_READY | KP80_STATUS_VBLANK,
                           kp80_read(&g_kp, 0x72, 0, 0xFF));
}

static void test_register_select_5bit(void)
{
    kp80_write(&g_kp, 0x72, 0xEE, 0);
    TEST_ASSERT_EQUAL_UINT8(0x0E, g_kp.ar);
    kp80_write(&g_kp, 0x72, 0x1F, 0);
    TEST_ASSERT_EQUAL_UINT8(KP80_REG_DUMMY, g_kp.ar);
}

static void test_register_widths(void)
{
    static const uint8_t mask[KP80_NUM_REGS] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xFF, 0x1F,
        0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x00, 0x00, 0x3F, 0xFF
    };
    g_kp.opt.addr_on_r31 = false;
    for (unsigned r = 0; r < KP80_NUM_REGS; r++) {
        crtc_set(r, 0xFF, 0);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(mask[r], g_kp.reg[r], "šířka registru");
    }
    /* R16/R17 (light pen) jsou jen pro čtení. */
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.reg[16]);
}

static void test_register_readability(void)
{
    bkk_crt_init(0);
    crtc_set(14, 0xC5, 0);
    crtc_set(15, 0x5A, 0);
    /* Čitelné jsou jen R14 - R17 (datasheet SY6545-1, Figure 3). */
    TEST_ASSERT_EQUAL_HEX8(0x05, crtc_get(14, 0));
    TEST_ASSERT_EQUAL_HEX8(0x5A, crtc_get(15, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(16, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(17, 0));
    /* Registry jen pro zápis a neexistující R20 - R30 vrací 0 (volba modelu). */
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(0, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(1, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(8, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(12, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(18, 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, crtc_get(25, 0));
    /* Registry se přesto uložily. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_bkk_init, g_kp.reg, 14);
    /* 71h čte vybraný registr stejně jako 73h. */
    kp80_write(&g_kp, 0x72, 15, 0);
    TEST_ASSERT_EQUAL_HEX8(0x5A, kp80_read(&g_kp, 0x71, 0, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(0x5A, kp80_peek(&g_kp, 0x71, 0, 0xFF));
    /* Zápis na 70h nemá účinek. */
    kp80_write(&g_kp, 0x70, 0x99, 0);
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.vram[0]);
    TEST_ASSERT_EQUAL_UINT8(15, g_kp.ar);
}

static void test_port_decode_low_bits(void)
{
    /* Jádro bere z portu jen bity 0 - 1 (dekód 70h - 73h dělá lepidlo). */
    kp80_write(&g_kp, 0x02, 14, 0);
    kp80_write(&g_kp, 0xF3, 0x21, 0);
    TEST_ASSERT_EQUAL_HEX8(0x21, g_kp.reg[14]);
    TEST_ASSERT_FALSE(kp80_port_has_wait(0x70));
    TEST_ASSERT_TRUE(kp80_port_has_wait(0x71));
    TEST_ASSERT_TRUE(kp80_port_has_wait(0x72));
    TEST_ASSERT_TRUE(kp80_port_has_wait(0x73));
}


/* ===== Transparentní přístup do VRAM ===== */

static void test_bkk_write_autoinc(void)
{
    bkk_crt_init(0);
    crtc_set(18, 0x01, 0);
    crtc_set(19, 0x23, 0);
    TEST_ASSERT_TRUE(g_kp.upd_pending);
    TEST_ASSERT_EQUAL_UINT16(0, g_kp.upd_addr);
    kp80_write(&g_kp, 0x72, 31, 0);
    TEST_ASSERT_FALSE(g_kp.upd_pending);
    TEST_ASSERT_EQUAL_HEX16(0x0123, g_kp.upd_addr);
    bkk_put('A', 0);
    bkk_put('B', 0);
    TEST_ASSERT_EQUAL_UINT8('A', g_kp.vram[0x123]);
    TEST_ASSERT_EQUAL_UINT8('B', g_kp.vram[0x124]);
    TEST_ASSERT_EQUAL_HEX16(0x0125, g_kp.upd_addr);
    /* R18/R19 drží zapsanou hodnotu, autoinkrement mění jen aktivní adresu. */
    TEST_ASSERT_EQUAL_HEX8(0x01, g_kp.reg[18]);
    TEST_ASSERT_EQUAL_HEX8(0x23, g_kp.reg[19]);
    /* Zápis s vybraným R31 je přístup k R31, bez čekající adresy adresu nemění. */
    TEST_ASSERT_EQUAL_UINT8(KP80_REG_DUMMY, g_kp.ar);
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.vram[0x125]);
}

static void test_bkk_library_putchar(void)
{
    /* KuP_GotoXY + KuP_PutChar: po GotoXY je vybrán R15, zápis 71h ho přepíše
     * znakem, PutChar pak R14/R15 hned nastaví znovu (BKK KuP80Z.Lib). */
    bkk_crt_init(0);
    bkk_set_addr(2 * 80 + 5, 0);
    crtc_set(14, 0, 0);
    crtc_set(15, 2 * 80 + 5, 0);
    bkk_put('H', 0);
    TEST_ASSERT_EQUAL_HEX8('H', g_kp.reg[15]);
    crtc_set(14, 0, 0);
    crtc_set(15, 2 * 80 + 6, 0);
    bkk_put('i', 0);
    TEST_ASSERT_EQUAL_UINT8('H', g_kp.vram[2 * 80 + 5]);
    TEST_ASSERT_EQUAL_UINT8('i', g_kp.vram[2 * 80 + 6]);
}

static void test_addr_valid_after_r31_only(void)
{
    g_kp.opt.data_write_to_reg = false;
    bkk_set_addr(0x0010, 0);
    /* Nová adresa bez přístupu k R31 ještě neplatí. */
    crtc_set(18, 0x02, 0);
    crtc_set(19, 0x00, 0);
    kp80_write(&g_kp, 0x71, 'x', 0);
    TEST_ASSERT_EQUAL_UINT8('x', g_kp.vram[0x010]);
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.vram[0x200]);
    kp80_write(&g_kp, 0x72, 31, 0);
    kp80_write(&g_kp, 0x71, 'y', 0);
    TEST_ASSERT_EQUAL_UINT8('y', g_kp.vram[0x200]);
}

static void test_addr_immediate_option(void)
{
    g_kp.opt.addr_on_r31 = false;
    g_kp.opt.data_write_to_reg = false;
    crtc_set(18, 0x03, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0300, g_kp.upd_addr);
    crtc_set(19, 0x45, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0345, g_kp.upd_addr);
    TEST_ASSERT_FALSE(g_kp.upd_pending);
    kp80_write(&g_kp, 0x71, 'z', 0);
    TEST_ASSERT_EQUAL_UINT8('z', g_kp.vram[0x345]);
    /* Zápis jen R19 mění dolní bajt aktivní adresy, horní zůstává. */
    crtc_set(19, 0x00, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0300, g_kp.upd_addr);
}

static void test_read_autoinc_on_off(void)
{
    bkk_set_addr(0x0040, 0);
    kp80_write(&g_kp, 0x71, 'P', 0);
    kp80_write(&g_kp, 0x71, 'Q', 0);
    kp80_write(&g_kp, 0x71, 'R', 0);

    /* Autoinkrement při čtení zapnut (výchozí). */
    bkk_set_addr(0x0040, 0);
    TEST_ASSERT_EQUAL_UINT8('P', kp80_peek(&g_kp, 0x70, 0, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0x0040, g_kp.upd_addr);
    TEST_ASSERT_EQUAL_UINT8('P', kp80_read(&g_kp, 0x70, 0, 0xFF));
    TEST_ASSERT_EQUAL_UINT8('Q', kp80_read(&g_kp, 0x70, 0, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0x0042, g_kp.upd_addr);

    /* Vypnut: čtení adresu nemění (BKK emulátor, Unicard). */
    g_kp.opt.read_autoinc = false;
    bkk_set_addr(0x0042, 0);
    TEST_ASSERT_EQUAL_UINT8('R', kp80_read(&g_kp, 0x70, 0, 0xFF));
    TEST_ASSERT_EQUAL_UINT8('R', kp80_read(&g_kp, 0x70, 0, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0x0042, g_kp.upd_addr);
}

static void test_data_write_hits_selected_reg(void)
{
    bkk_crt_init(0);
    bkk_set_addr(0x0100, 0);
    kp80_write(&g_kp, 0x72, 10, 0);
    kp80_write(&g_kp, 0x71, 0x20, 0);
    /* Zapnuto: bajt jde do VRAM i do vybraného R10 (kurzor vypnut). */
    TEST_ASSERT_EQUAL_UINT8(0x20, g_kp.vram[0x100]);
    TEST_ASSERT_EQUAL_HEX8(0x20, g_kp.reg[10]);

    /* Vypnuto: registr zůstává. */
    g_kp.opt.data_write_to_reg = false;
    crtc_set(10, 0x60, 0);
    kp80_write(&g_kp, 0x71, 0x41, 0);
    TEST_ASSERT_EQUAL_UINT8(0x41, g_kp.vram[0x101]);
    TEST_ASSERT_EQUAL_HEX8(0x60, g_kp.reg[10]);
}

static void test_data_write_to_r18_moves_pending_addr(void)
{
    /* Zápis 71h s vybraným R19 přepíše latch R19; adresa platí až po R31. */
    bkk_set_addr(0x0005, 0);
    kp80_write(&g_kp, 0x72, 19, 0);
    kp80_write(&g_kp, 0x71, 0x50, 0);
    TEST_ASSERT_EQUAL_UINT8(0x50, g_kp.vram[0x005]);
    TEST_ASSERT_TRUE(g_kp.upd_pending);
    kp80_write(&g_kp, 0x72, 31, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0050, g_kp.upd_addr);
}

static void test_r31_read_access(void)
{
    crtc_set(18, 0x00, 0);
    crtc_set(19, 0x77, 0);
    kp80_write(&g_kp, 0x72, 31, 0);
    crtc_set(19, 0x88, 0);
    kp80_write(&g_kp, 0x72, 31, 0);   /* znovu vybrat R31, potvrdí 0088h */
    crtc_set(19, 0x99, 0);
    TEST_ASSERT_EQUAL_HEX16(0x0088, g_kp.upd_addr);
    /* Výběr R31 přes 72h by adresu potvrdil sám; pro test čtení R31 se AR
     * nastaví přímo ve struktuře. */
    g_kp.ar = KP80_REG_DUMMY;
    /* peek: R31 nebudí sběrnici, žádný vedlejší účinek. */
    TEST_ASSERT_EQUAL_HEX8(0x5C, kp80_peek(&g_kp, 0x73, 0, 0x5C));
    TEST_ASSERT_TRUE(g_kp.upd_pending);
    /* read: přístup k R31 potvrdí čekající adresu. */
    TEST_ASSERT_EQUAL_HEX8(0x5C, kp80_read(&g_kp, 0x73, 0, 0x5C));
    TEST_ASSERT_FALSE(g_kp.upd_pending);
    TEST_ASSERT_EQUAL_HEX16(0x0099, g_kp.upd_addr);
}


/* ===== Status a časování ===== */

static void test_frame_geometry_bkk(void)
{
    bkk_crt_init(0);
    TEST_ASSERT_EQUAL_UINT32(128, kp80_line_period(&g_kp));
    TEST_ASSERT_EQUAL_UINT32(312, kp80_frame_lines(&g_kp));
    TEST_ASSERT_EQUAL_UINT32(128u * 312u, kp80_frame_period(&g_kp));
}

static void test_vblank_in_time(void)
{
    const uint64_t t0 = 1000;
    kp80_power_on(&g_kp, 0);
    /* Zápis registrů v čase 0, snímek začíná v t0 díky resetu čítačů. */
    bkk_crt_init(0);
    kp80_reset(&g_kp, t0);
    const uint64_t line = 128, frame = 128u * 312u;
    TEST_ASSERT_FALSE(kp80_vblank(&g_kp, t0));
    TEST_ASSERT_FALSE(kp80_vblank(&g_kp, t0 + 249 * line + 127));
    TEST_ASSERT_TRUE(kp80_vblank(&g_kp, t0 + 250 * line));
    TEST_ASSERT_TRUE(kp80_vblank(&g_kp, t0 + 311 * line + 127));
    TEST_ASSERT_FALSE(kp80_vblank(&g_kp, t0 + frame));
    TEST_ASSERT_TRUE(kp80_vblank(&g_kp, t0 + 5 * frame + 260 * line));
    TEST_ASSERT_EQUAL_HEX8(KP80_STATUS_UPDATE_READY | KP80_STATUS_VBLANK,
                           kp80_read(&g_kp, 0x72, t0 + 250 * line, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(KP80_STATUS_UPDATE_READY,
                           kp80_read(&g_kp, 0x72, t0 + 10, 0xFF));
    TEST_ASSERT_EQUAL_UINT64(0, kp80_field_count(&g_kp, t0 + frame - 1));
    TEST_ASSERT_EQUAL_UINT64(3, kp80_field_count(&g_kp, t0 + 3 * frame));
    /* Čas před posledním přepočtem se bere jako bez posunu. */
    TEST_ASSERT_FALSE(kp80_vblank(&g_kp, 0));
}

static void test_timing_register_change_keeps_fields(void)
{
    bkk_crt_init(0);
    const uint64_t frame = 128u * 312u;
    /* Po 10 snímcích a kousku změnit R0: snímky se dopočítají ke staré periodě. */
    uint64_t t = 10 * frame + 100;
    crtc_set(0, 63, t);
    TEST_ASSERT_EQUAL_UINT64(10, kp80_field_count(&g_kp, t));
    TEST_ASSERT_EQUAL_UINT32(64u * 312u, kp80_frame_period(&g_kp));
    TEST_ASSERT_EQUAL_UINT64(11, kp80_field_count(&g_kp, t + 64u * 312u - 100));
}

static void test_update_ready_delay(void)
{
    g_kp.opt.update_delay = 10;
    crtc_set(18, 0, 100);
    crtc_set(19, 0, 100);
    /* Zápis R18/R19 bit 7 nemění. */
    TEST_ASSERT_TRUE(kp80_status(&g_kp, 100) & KP80_STATUS_UPDATE_READY);
    kp80_write(&g_kp, 0x72, 31, 100);
    TEST_ASSERT_FALSE(kp80_status(&g_kp, 109) & KP80_STATUS_UPDATE_READY);
    TEST_ASSERT_TRUE(kp80_status(&g_kp, 110) & KP80_STATUS_UPDATE_READY);
    kp80_write(&g_kp, 0x71, 'k', 200);
    TEST_ASSERT_FALSE(kp80_read(&g_kp, 0x72, 205, 0xFF) & KP80_STATUS_UPDATE_READY);
    TEST_ASSERT_TRUE(kp80_read(&g_kp, 0x72, 210, 0xFF) & KP80_STATUS_UPDATE_READY);
    /* LPEN full je vždy 0. */
    TEST_ASSERT_EQUAL_HEX8(0, kp80_status(&g_kp, 300) & KP80_STATUS_LPEN_FULL);
}


/* ===== Adresa modulo ===== */

static void test_vram_modulo_2048(void)
{
    bkk_set_addr(0x07FF, 0);
    kp80_write(&g_kp, 0x71, 'e', 0);
    kp80_write(&g_kp, 0x71, 'f', 0);
    TEST_ASSERT_EQUAL_UINT8('e', g_kp.vram[0x7FF]);
    TEST_ASSERT_EQUAL_UINT8('f', g_kp.vram[0x000]);
    TEST_ASSERT_EQUAL_HEX16(0x0801, g_kp.upd_addr);
    /* Adresa 1234h ukazuje na VRAM 234h. */
    bkk_set_addr(0x1234, 0);
    kp80_write(&g_kp, 0x71, 'g', 0);
    TEST_ASSERT_EQUAL_UINT8('g', g_kp.vram[0x234]);
    /* 14bitové přetečení 3FFFh -> 0. */
    bkk_set_addr(0x3FFF, 0);
    kp80_write(&g_kp, 0x71, 'h', 0);
    TEST_ASSERT_EQUAL_HEX16(0x0000, g_kp.upd_addr);
    TEST_ASSERT_EQUAL_UINT8('h', g_kp.vram[0x7FF]);
}


/* ===== Reset a power-on ===== */

static void test_reset_keeps_registers_and_vram(void)
{
    bkk_crt_init(0);
    bkk_set_addr(0x0222, 0);
    kp80_write(&g_kp, 0x71, 'r', 0);
    kp80_write(&g_kp, 0x72, 14, 0);
    const uint64_t frame = 128u * 312u;
    const uint64_t t = 7 * frame + 260 * 128;
    TEST_ASSERT_TRUE(kp80_vblank(&g_kp, t));
    kp80_reset(&g_kp, t);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_bkk_init, g_kp.reg, 16);
    TEST_ASSERT_EQUAL_UINT8('r', g_kp.vram[0x222]);
    TEST_ASSERT_EQUAL_UINT8(14, g_kp.ar);
    TEST_ASSERT_EQUAL_HEX16(0x0223, g_kp.upd_addr);
    /* Čítače rozkladu začínají znovu: začátek snímku, počítadlo 0. */
    TEST_ASSERT_FALSE(kp80_vblank(&g_kp, t));
    TEST_ASSERT_EQUAL_UINT64(0, kp80_field_count(&g_kp, t));
}

static void test_power_on_clears_everything(void)
{
    bkk_crt_init(0);
    bkk_set_addr(0x0010, 0);
    kp80_write(&g_kp, 0x71, 'q', 0);
    g_kp.opt.read_autoinc = false;
    kp80_power_on(&g_kp, 50);
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.vram[0x010]);
    TEST_ASSERT_EQUAL_UINT8(0, g_kp.reg[1]);
    TEST_ASSERT_EQUAL_UINT16(0, g_kp.upd_addr);
    /* Volby se zapnutím nemění. */
    TEST_ASSERT_FALSE(g_kp.opt.read_autoinc);
}


/* ===== Uložení stavu ===== */

static void test_state_roundtrip(void)
{
    uint8_t buf[KP80_STATE_SIZE + 16];
    bkk_crt_init(0);
    bkk_set_addr(0x0333, 0);
    kp80_write(&g_kp, 0x71, 's', 0);
    crtc_set(18, 0x01, 77);
    crtc_set(0, 0x7E, 12345);
    g_kp.upd_ready_at = 0x1122334455667788ull;
    st_KP80 orig = g_kp;

    TEST_ASSERT_EQUAL_size_t(0, kp80_state_save(&g_kp, buf, KP80_STATE_SIZE - 1));
    TEST_ASSERT_EQUAL_size_t(KP80_STATE_SIZE, kp80_state_save(&g_kp, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("KP80", buf, 4);
    TEST_ASSERT_EQUAL_HEX8(KP80_STATE_VERSION, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[5]);

    kp80_power_on(&g_kp, 999);
    TEST_ASSERT_TRUE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig.reg, g_kp.reg, KP80_NUM_REGS);
    TEST_ASSERT_EQUAL_UINT8(orig.ar, g_kp.ar);
    TEST_ASSERT_EQUAL(orig.upd_pending, g_kp.upd_pending);
    TEST_ASSERT_EQUAL_HEX16(orig.upd_addr, g_kp.upd_addr);
    TEST_ASSERT_EQUAL_HEX64(orig.upd_ready_at, g_kp.upd_ready_at);
    TEST_ASSERT_EQUAL_UINT64(orig.tm_last, g_kp.tm_last);
    TEST_ASSERT_EQUAL_UINT32(orig.tm_phase, g_kp.tm_phase);
    TEST_ASSERT_EQUAL_UINT64(orig.tm_fields, g_kp.tm_fields);
    TEST_ASSERT_EQUAL_MEMORY(orig.vram, g_kp.vram, KP80_VRAM_SIZE);

    /* Druhé uložení dá bajtově stejná data. */
    uint8_t buf2[KP80_STATE_SIZE];
    TEST_ASSERT_EQUAL_size_t(KP80_STATE_SIZE, kp80_state_save(&g_kp, buf2, sizeof(buf2)));
    TEST_ASSERT_EQUAL_MEMORY(buf, buf2, KP80_STATE_SIZE);
}

static void test_state_load_rejects_bad_data(void)
{
    uint8_t buf[KP80_STATE_SIZE];
    bkk_crt_init(0);
    TEST_ASSERT_EQUAL_size_t(KP80_STATE_SIZE, kp80_state_save(&g_kp, buf, sizeof(buf)));
    kp80_power_on(&g_kp, 0);
    st_KP80 before = g_kp;

    TEST_ASSERT_FALSE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE - 1));
    TEST_ASSERT_FALSE(kp80_state_load(&g_kp, NULL, KP80_STATE_SIZE));
    buf[0] = 'X';
    TEST_ASSERT_FALSE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE));
    buf[0] = 'K';
    buf[4] = (uint8_t) (KP80_STATE_VERSION + 1);
    TEST_ASSERT_FALSE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE));
    buf[4] = 0;
    TEST_ASSERT_FALSE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE));
    TEST_ASSERT_EQUAL_MEMORY(&before.reg, &g_kp.reg, sizeof(before.reg));
    TEST_ASSERT_EQUAL_MEMORY(&before.vram, &g_kp.vram, sizeof(before.vram));

    /* Hodnoty mimo invarianty se zamaskují. */
    buf[4] = KP80_STATE_VERSION;
    buf[8 + 4] = 0xFF;                     /* R4 */
    buf[8 + KP80_NUM_REGS] = 0xFF;         /* AR */
    buf[8 + KP80_NUM_REGS + 2] = 0xFF;     /* upd_addr low */
    buf[8 + KP80_NUM_REGS + 3] = 0xFF;     /* upd_addr high */
    TEST_ASSERT_TRUE(kp80_state_load(&g_kp, buf, KP80_STATE_SIZE));
    TEST_ASSERT_EQUAL_HEX8(0x7F, g_kp.reg[4]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, g_kp.ar);
    TEST_ASSERT_EQUAL_HEX16(0x3FFF, g_kp.upd_addr);
    TEST_ASSERT_TRUE(g_kp.tm_phase < kp80_frame_period(&g_kp));
}


/* ===== Vykreslení ===== */

static void test_render_power_on_blank(void)
{
    /* Registry 0: nic se nezobrazí, buffer je celý 0. */
    kp80_render(&g_kp, 0, g_fb);
    for (unsigned i = 0; i < KP80_FB_SIZE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, g_fb[i]);
    }
}

static void test_render_char_A(void)
{
    static const uint8_t blank[16] = { 0 };
    bkk_screen(0);
    crtc_set(10, 0x20, 0);              /* bez kurzoru */
    bkk_set_addr(3 * 80 + 7, 0);
    bkk_put('A', 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(7, 3, s_glyph_A, false);
    assert_cell(6, 3, blank, false);
    assert_cell(8, 3, blank, false);
    assert_cell(0, 0, blank, false);
    /* Levý bod = bit 7: řádek 1 "A" = 38h -> body 2, 3, 4. */
    TEST_ASSERT_EQUAL_UINT8(0, px(7 * 8 + 1, 3 * 10 + 1));
    TEST_ASSERT_EQUAL_UINT8(1, px(7 * 8 + 2, 3 * 10 + 1));
    TEST_ASSERT_EQUAL_UINT8(1, px(7 * 8 + 4, 3 * 10 + 1));
    TEST_ASSERT_EQUAL_UINT8(0, px(7 * 8 + 5, 3 * 10 + 1));
}

static void test_render_inverse_bit7(void)
{
    bkk_screen(0);
    crtc_set(10, 0x20, 0);
    bkk_set_addr(24 * 80 + 79, 0);      /* pravý dolní roh */
    bkk_put(0x80 | 'A', 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(79, 24, s_glyph_A, true);
}

static void test_render_cursor_steady_and_blink(void)
{
    static const uint8_t blank[16] = { 0 };
    bkk_screen(0);
    crtc_set(14, 0x00, 0);
    crtc_set(15, 81, 0);
    crtc_set(10, 0x00, 0);              /* svítí, start řádku 0 */
    crtc_set(11, 9, 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(1, 1, blank, true);
    assert_cell(0, 1, blank, false);

    /* Kurzor jen v řádcích 8 - 9 (podtržení). */
    crtc_set(10, 0x08, 0);
    kp80_render(&g_kp, 0, g_fb);
    TEST_ASSERT_EQUAL_UINT8(0, px(8, 10 + 7));
    TEST_ASSERT_EQUAL_UINT8(1, px(8, 10 + 8));
    TEST_ASSERT_EQUAL_UINT8(1, px(15, 10 + 9));

    /* Kurzor na inverzním znaku: XOR vrací normální podobu. */
    crtc_set(10, 0x00, 0);
    bkk_set_addr(81, 0);
    bkk_put(0x80 | 'A', 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(1, 1, s_glyph_A, false);

    /* Bez kurzoru (01). */
    crtc_set(10, 0x20, 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(1, 1, s_glyph_A, true);

    /* Blikání 1/16 (10): snímky 0 - 7 svítí, 8 - 15 ne. */
    crtc_set(10, 0x40, 0);
    const uint64_t frame = 128u * 312u;
    TEST_ASSERT_TRUE(kp80_cursor_visible(&g_kp, 7 * frame));
    TEST_ASSERT_FALSE(kp80_cursor_visible(&g_kp, 8 * frame));
    TEST_ASSERT_FALSE(kp80_cursor_visible(&g_kp, 15 * frame));
    TEST_ASSERT_TRUE(kp80_cursor_visible(&g_kp, 16 * frame));
    kp80_render(&g_kp, 9 * frame, g_fb);
    assert_cell(1, 1, s_glyph_A, true);
    kp80_render(&g_kp, 2 * frame, g_fb);
    assert_cell(1, 1, s_glyph_A, false);

    /* Blikání 1/32 (11): snímky 0 - 15 svítí, 16 - 31 ne. */
    crtc_set(10, 0x60, 0);
    TEST_ASSERT_TRUE(kp80_cursor_visible(&g_kp, 15 * frame));
    TEST_ASSERT_FALSE(kp80_cursor_visible(&g_kp, 16 * frame));
    TEST_ASSERT_TRUE(kp80_cursor_visible(&g_kp, 32 * frame));
}

static void test_render_start_address(void)
{
    static const uint8_t blank[16] = { 0 };
    bkk_screen(0);
    crtc_set(10, 0x20, 0);
    bkk_set_addr(1 * 80 + 2, 0);
    bkk_put('A', 0);
    /* Scroll o řádek (R12/R13 += 80): znak se posune na řádek 0. */
    crtc_set(12, 0, 0);
    crtc_set(13, 80, 0);
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(2, 0, s_glyph_A, false);
    assert_cell(2, 1, blank, false);
    /* Start blízko konce VRAM: adresa se zalomí modulo 2 048. */
    bkk_set_addr(0x0005, 0);
    bkk_put(0x80 | 'A', 0);
    crtc_set(12, 0x07, 0);
    crtc_set(13, 0xFE, 0);              /* 7FEh: buňka (7, 0) = 805h -> VRAM 005h */
    kp80_render(&g_kp, 0, g_fb);
    assert_cell(7, 0, s_glyph_A, true);
}

static void test_render_clip_larger_geometry(void)
{
    bkk_screen(0);
    crtc_set(10, 0x20, 0);
    crtc_set(1, 100, 0);                /* 100 znaků = 800 bodů */
    crtc_set(6, 40, 0);                 /* 40 řádků = 400 bodů */
    crtc_set(9, 15, 0);                 /* 16 řádků na znak */
    bkk_set_addr(1 * 100 + 79, 0);
    bkk_put(0x80, 0);                   /* inverzní "@" */
    kp80_render(&g_kp, 0, g_fb);
    /* Buňka (79, 1) při 16 řádcích na znak: řádek 16 - 31. Řádek 12 glyfu "@" = 40h. */
    uint8_t row0 = (uint8_t) ~g_kp80_font_rom[0x00 * 16 + 1];
    for (unsigned x = 0; x < 8; x++) {
        TEST_ASSERT_EQUAL_UINT8((row0 >> (7 - x)) & 1u, px(79 * 8 + x, 16 + 1));
    }
    TEST_ASSERT_EQUAL_UINT8(((uint8_t) ~0x40u >> 7) & 1u, px(79 * 8, 16 + 12));
    TEST_ASSERT_EQUAL_UINT8(((uint8_t) ~0x40u >> 6) & 1u, px(79 * 8 + 1, 16 + 12));
}

static void test_render_smaller_geometry_leaves_black(void)
{
    bkk_screen(0);
    crtc_set(10, 0x00, 0);              /* kurzor svítí na 0 */
    crtc_set(1, 40, 0);
    crtc_set(6, 10, 0);
    kp80_render(&g_kp, 0, g_fb);
    /* Kurzor na (0, 0) svítí, mimo 40 x 10 znaků je tma. */
    TEST_ASSERT_EQUAL_UINT8(1, px(0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, px(40 * 8, 0));
    TEST_ASSERT_EQUAL_UINT8(0, px(0, 10 * 10));
    TEST_ASSERT_EQUAL_UINT8(0, px(KP80_FB_WIDTH - 1, KP80_FB_HEIGHT - 1));
}

static void test_render_user_font(void)
{
    static uint8_t font[KP80_FONT_SIZE];
    memset(font, 0xFF, sizeof(font));
    bkk_screen(0);
    crtc_set(10, 0x20, 0);
    kp80_set_font(&g_kp, font);
    kp80_render(&g_kp, 0, g_fb);
    TEST_ASSERT_EQUAL_UINT8(1, px(100, 100));
    kp80_set_font(&g_kp, NULL);
    TEST_ASSERT_EQUAL_PTR(g_kp80_font_rom, g_kp.font);
}


/* ===================================================================== */
/* Runner                                                                */
/* ===================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_font_checksum);
    RUN_TEST(test_font_glyph_A);
    RUN_TEST(test_power_on_state);
    RUN_TEST(test_register_select_5bit);
    RUN_TEST(test_register_widths);
    RUN_TEST(test_register_readability);
    RUN_TEST(test_port_decode_low_bits);
    RUN_TEST(test_bkk_write_autoinc);
    RUN_TEST(test_bkk_library_putchar);
    RUN_TEST(test_addr_valid_after_r31_only);
    RUN_TEST(test_addr_immediate_option);
    RUN_TEST(test_read_autoinc_on_off);
    RUN_TEST(test_data_write_hits_selected_reg);
    RUN_TEST(test_data_write_to_r18_moves_pending_addr);
    RUN_TEST(test_r31_read_access);
    RUN_TEST(test_frame_geometry_bkk);
    RUN_TEST(test_vblank_in_time);
    RUN_TEST(test_timing_register_change_keeps_fields);
    RUN_TEST(test_update_ready_delay);
    RUN_TEST(test_vram_modulo_2048);
    RUN_TEST(test_reset_keeps_registers_and_vram);
    RUN_TEST(test_power_on_clears_everything);
    RUN_TEST(test_state_roundtrip);
    RUN_TEST(test_state_load_rejects_bad_data);
    RUN_TEST(test_render_power_on_blank);
    RUN_TEST(test_render_char_A);
    RUN_TEST(test_render_inverse_bit7);
    RUN_TEST(test_render_cursor_steady_and_blink);
    RUN_TEST(test_render_start_address);
    RUN_TEST(test_render_clip_larger_geometry);
    RUN_TEST(test_render_smaller_geometry_leaves_black);
    RUN_TEST(test_render_user_font);
    return UNITY_END();
}
