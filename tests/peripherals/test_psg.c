/*
 * test_psg.c — unit testy pro PSG (Programmable Sound Generator SN76489AN)
 *
 * Testuje: inicializaci, zápis do registrů, nastavení frekvence,
 *          atenuace, noise generátor (rychlost posunu, 15bitový registr,
 *          nulování zápisem), dělič tónu 0 a 1, step processing
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "hw-generic/psg/psg.h"

void setUp(void) { }
void tearDown(void) { }

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* PSG — po inicializaci je PSG modul přístupný */
void test_psg_init_state(void)
{
    /* po mztest_init() by měl být PSG inicializován */
    /* kanály 0-2 jsou tónové, kanál 3 je noise */
    TEST_ASSERT_EQUAL_INT(PSG_CHTYPE_TONE, g_psg.channel[PSG_CHANNEL_0].type);
    TEST_ASSERT_EQUAL_INT(PSG_CHTYPE_TONE, g_psg.channel[PSG_CHANNEL_1].type);
    TEST_ASSERT_EQUAL_INT(PSG_CHTYPE_TONE, g_psg.channel[PSG_CHANNEL_2].type);
    TEST_ASSERT_EQUAL_INT(PSG_CHTYPE_NOISE, g_psg.channel[PSG_CHANNEL_3].type);
}

/* PSG — zápis do PSG nepadne */
void test_psg_write_no_crash(void)
{
    /* latch + data byte pro kanál 0, frekvence */
    psg_write_byte(PSG_CH_LEFT, 0x80); /* latch: ch0, tone, data=0x00 */
    TEST_ASSERT_TRUE(true);
}

/* PSG — step nepadne */
void test_psg_step_no_crash(void)
{
    psg_step();
    TEST_ASSERT_TRUE(true);
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* PSG — po reinicializaci jsou kanály ztišené */
void test_psg_reinit_muted(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false); /* mono reinit */

    /* všechny kanály by měly být ztišené (attn = PSG_OUT_OFF = 15) */
    for (int ch = 0; ch < PSG_CHANNELS_COUNT; ch++) {
        TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg.channel[ch].attn);
    }
}

/* PSG — nastavení frekvence kanálu 0 (10-bit divider) */
void test_psg_tone_frequency(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* SN76489 byte format:
     * Latch byte: 1 CC T DDDD
     *   CC = channel (00=ch0, 01=ch1, 10=ch2, 11=ch3)
     *   T  = 0=tone, 1=volume
     *   DDDD = dolní 4 bity frekvence
     *
     * Data byte: 0 X DDDDDD
     *   DDDDDD = horních 6 bitů frekvence
     */

    /* kanál 0, tone, dolní 4 bity = 0x05 */
    psg_write_byte(PSG_CH_LEFT, 0x85); /* 1 00 0 0101 */

    /* ověřit latch */
    TEST_ASSERT_EQUAL_INT(PSG_CHANNEL_0, g_psg.latch_cs);
    TEST_ASSERT_EQUAL_INT(0, g_psg.latch_attn);

    /* horních 6 bitů = 0x0A */
    psg_write_byte(PSG_CH_LEFT, 0x0A); /* 0 0 001010 */

    /* výsledný divider: horních 6 bitů (0x0A) << 4 | dolních 4 bitů (0x05) = 0x0A5 */
    TEST_ASSERT_EQUAL_HEX16(0x0A5, g_psg.channel[PSG_CHANNEL_0].tone.divider);
}

/* PSG — nastavení hlasitosti kanálu */
void test_psg_volume(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* kanál 0, volume, hodnota 0 = max hlasitost */
    /* 1 00 1 0000 = 0x90 */
    psg_write_byte(PSG_CH_LEFT, 0x90);

    TEST_ASSERT_EQUAL_INT(PSG_OUT_MAX, g_psg.channel[PSG_CHANNEL_0].attn);

    /* kanál 0, volume, hodnota 15 = mute */
    /* 1 00 1 1111 = 0x9F */
    psg_write_byte(PSG_CH_LEFT, 0x9F);

    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg.channel[PSG_CHANNEL_0].attn);
}

/* PSG — nastavení hlasitosti všech kanálů */
void test_psg_volume_all_channels(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* ch0 volume = 5 → 0x95 */
    psg_write_byte(PSG_CH_LEFT, 0x95);
    TEST_ASSERT_EQUAL_INT(5, g_psg.channel[PSG_CHANNEL_0].attn);

    /* ch1 volume = 10 → 0xBA */
    psg_write_byte(PSG_CH_LEFT, 0xBA);
    TEST_ASSERT_EQUAL_INT(10, g_psg.channel[PSG_CHANNEL_1].attn);

    /* ch2 volume = 3 → 0xD3 */
    psg_write_byte(PSG_CH_LEFT, 0xD3);
    TEST_ASSERT_EQUAL_INT(3, g_psg.channel[PSG_CHANNEL_2].attn);

    /* noise volume = 15 → 0xFF (mute) */
    psg_write_byte(PSG_CH_LEFT, 0xFF);
    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg.channel[PSG_CHANNEL_3].attn);
}

/* PSG — noise generátor — typ a dělička */
void test_psg_noise_setup(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* noise register: latch byte
     * 1 11 0 XNDD
     *   N = noise type (0=periodic, 1=white)
     *   DD = divider type (00, 01, 10, 11)
     */

    /* periodic noise, divider type 0 */
    /* 1 11 0 0000 = 0xE0 */
    psg_write_byte(PSG_CH_LEFT, 0xE0);

    TEST_ASSERT_EQUAL_INT(NOISE_TYPE_PERIODIC, g_psg.channel[PSG_CHANNEL_3].noise.type);
    TEST_ASSERT_EQUAL_INT(NOISE_DIV_TYPE0, g_psg.channel[PSG_CHANNEL_3].noise.div_type);

    /* white noise, divider type 2 */
    /* 1 11 0 0110 = 0xE6 */
    psg_write_byte(PSG_CH_LEFT, 0xE6);

    TEST_ASSERT_EQUAL_INT(NOISE_TYPE_WHITE, g_psg.channel[PSG_CHANNEL_3].noise.type);
    TEST_ASSERT_EQUAL_INT(NOISE_DIV_TYPE2, g_psg.channel[PSG_CHANNEL_3].noise.div_type);
}

/* PSG — noise shift register reset */
void test_psg_noise_shift_register(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* po inicializaci by shift register měl mít výchozí hodnotu */
    /* SN76489: LFSR se inicializuje na nenulovou hodnotu */
    TEST_ASSERT_NOT_EQUAL(0, g_psg.channel[PSG_CHANNEL_3].noise.shiftregister);
}

/* PSG — stereo mód (MZ-1500 má 2 PSG) */
void test_psg_mono_mode(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* MZ-800 má mono PSG */
#if MZARCH == 800
    /* ověříme, že g_psg je alias pro psg[0] */
    TEST_ASSERT_EQUAL_PTR(&g_psg_module.psg[0], &g_psg);
#endif
    TEST_ASSERT_TRUE(true);
}

/* PSG — step processing nemění atenuaci */
void test_psg_step_preserves_attenuation(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* nastavit hlasitost */
    psg_write_byte(PSG_CH_LEFT, 0x93); /* ch0 volume = 3 */

    /* provést několik stepů */
    for (int i = 0; i < 100; i++) {
        psg_step();
    }

    /* hlasitost by se neměla změnit */
    TEST_ASSERT_EQUAL_INT(3, g_psg.channel[PSG_CHANNEL_0].attn);
}

/* PSG — latch mechanismus — následný data byte jde do latched kanálu */
void test_psg_latch_mechanism(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(false);

    /* latch kanál 1, tone */
    psg_write_byte(PSG_CH_LEFT, 0xA0); /* 1 01 0 0000 */
    TEST_ASSERT_EQUAL_INT(PSG_CHANNEL_1, g_psg.latch_cs);

    /* data byte jde do kanálu 1 */
    psg_write_byte(PSG_CH_LEFT, 0x05); /* 0 0 000101 */
    TEST_ASSERT_EQUAL_HEX16(0x050, g_psg.channel[PSG_CHANNEL_1].tone.divider);
}

/* PSG — stereo broadcast zápis do obou PSG */
void test_psg_stereo_broadcast(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(true); /* stereo režim */

    /* broadcast zápis: PSG_CH_LEFT | PSG_CH_RIGHT musí zapsat do obou PSG */
    /* ch0 volume = 5 → 0x95 */
    psg_write_byte(PSG_CH_LEFT | PSG_CH_RIGHT, 0x95);

    /* oba PSG by měly mít ch0 atenuaci = 5 */
    TEST_ASSERT_EQUAL_INT(5, g_psg_module.psg[0].channel[PSG_CHANNEL_0].attn);
    TEST_ASSERT_EQUAL_INT(5, g_psg_module.psg[1].channel[PSG_CHANNEL_0].attn);

    /* vrátit zpět do mono režimu pro ostatní testy */
    psg_init(false);
}

/* PSG — stereo individuální zápis jen do jednoho PSG */
void test_psg_stereo_individual(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_init(true); /* stereo režim */

    /* zápis jen do levého PSG: ch0 volume = 3 */
    psg_write_byte(PSG_CH_LEFT, 0x93);
    TEST_ASSERT_EQUAL_INT(3, g_psg_module.psg[0].channel[PSG_CHANNEL_0].attn);
    /* pravý PSG zůstane na výchozí hodnotě (mute) */
    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg_module.psg[1].channel[PSG_CHANNEL_0].attn);

    psg_init(true); /* reset obou PSG */

    /* zápis jen do pravého PSG: ch0 volume = 7 */
    psg_write_byte(PSG_CH_RIGHT, 0x97);
    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg_module.psg[0].channel[PSG_CHANNEL_0].attn);
    TEST_ASSERT_EQUAL_INT(7, g_psg_module.psg[1].channel[PSG_CHANNEL_0].attn);

    /* vrátit zpět do mono režimu */
    psg_init(false);
}

/* ================================================================
 * PSG WRITE LOG (hook v psg_write_byte)
 * ================================================================ */

/**
 * @brief Pomocná funkce - spočítá řádky v souboru (mimo komentáře #).
 *
 * Slouží pro assertion že write-log zapsal očekávaný počet dat řádků.
 */
static unsigned count_data_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0u;
    unsigned count = 0u;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp))
    {
        if (buf[0] == '#' || buf[0] == '\n' || buf[0] == '\0')
            continue;
        count++;
    }
    fclose(fp);
    return count;
}

/* PSG write log - default disabled */
void test_psg_write_log_default_disabled(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_FALSE(psg_write_log_is_enabled());
}

/* PSG write log - enable s NULL path selže */
void test_psg_write_log_enable_null(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    TEST_ASSERT_FALSE(psg_write_log_enable(NULL));
    TEST_ASSERT_FALSE(psg_write_log_is_enabled());
}

/* PSG write log - mono: enable, několik zápisů, disable, ověř obsah */
void test_psg_write_log_mono_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = "test_psg_writelog_mono.tsv";
    psg_init(false); /* mono */

    TEST_ASSERT_TRUE(psg_write_log_enable(path));
    TEST_ASSERT_TRUE(psg_write_log_is_enabled());

    /* 3 zápisy: latch ch0 attn, data attn=7, latch ch1 attn */
    psg_write_byte(PSG_CH_LEFT, 0x90);
    psg_write_byte(PSG_CH_LEFT, 0x97);
    psg_write_byte(PSG_CH_LEFT, 0xB0);

    TEST_ASSERT_EQUAL_UINT(3u, psg_write_log_row_count());

    psg_write_log_disable();
    TEST_ASSERT_FALSE(psg_write_log_is_enabled());

    /* Po disable: soubor má header + 3 datové řádky */
    TEST_ASSERT_EQUAL_UINT(3u, count_data_lines(path));

    remove(path);
}

/* PSG write log - když je disabled, nezapisuje (počet řádků se nemění) */
void test_psg_write_log_disabled_skips(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Předpokládáme disabled (= po předchozím testu nebo init).
     * row_count je perzistentní (resetuje se až při enable). Test si
     * tedy uloží baseline a ověří že se nezvýší při zápisech-bez-logu. */
    psg_write_log_disable();
    TEST_ASSERT_FALSE(psg_write_log_is_enabled());

    unsigned baseline = psg_write_log_row_count();

    psg_init(false);
    psg_write_byte(PSG_CH_LEFT, 0x80);
    psg_write_byte(PSG_CH_LEFT, 0x9F);

    /* row_count se nesmí inkrementovat když je disabled. */
    TEST_ASSERT_EQUAL_UINT(baseline, psg_write_log_row_count());
}

/* PSG write log - stereo broadcast: jeden zápis s LEFT|RIGHT mask */
void test_psg_write_log_stereo_broadcast(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *path = "test_psg_writelog_stereo.tsv";
    psg_init(true); /* stereo */

    TEST_ASSERT_TRUE(psg_write_log_enable(path));

    /* Broadcast = jeden řádek se channel_mask = 3. */
    psg_write_byte(PSG_CH_LEFT | PSG_CH_RIGHT, 0x9F);
    TEST_ASSERT_EQUAL_UINT(1u, psg_write_log_row_count());

    psg_write_log_disable();
    TEST_ASSERT_EQUAL_UINT(1u, count_data_lines(path));

    /* Ověř že řádek obsahuje channel_mask 3 a hex 9F. */
    FILE *fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[512];
    bool found = false;
    while (fgets(buf, sizeof(buf), fp))
    {
        if (buf[0] == '#') continue;
        /* Tab-separated: <ticks>\t<mask>\t<hex> */
        if (strstr(buf, "\t3\t9F") != NULL)
        {
            found = true;
            break;
        }
    }
    fclose(fp);
    TEST_ASSERT_TRUE(found);

    remove(path);
    psg_init(false); /* vrátit do mono pro následující testy */
}

/* ================================================================
 * ŠUM A DĚLIČ TÓNU PODLE MĚŘENÍ NA HW (2026-11-14)
 * ================================================================ */

/** @brief Počet kroků PSG na jeden posun registru šumu při pevné rychlosti NF = 0. */
#define NOISE_SHIFT_STEPS_NF0 32

/**
 * @brief Krokuje PSG a vrátí počet kroků do první změny výstupu kanálu na `level`.
 *
 * @param ch    Kanál.
 * @param level Hledaná úroveň výstupu (0 nebo 1).
 * @param limit Nejvyšší počet kroků.
 * @return Počet provedených kroků; `limit + 1`, když změna nenastala.
 */
static unsigned steps_until_level(unsigned ch, unsigned level, unsigned limit)
{
    for (unsigned n = 1; n <= limit; n++) {
        psg_instance_step(&g_psg);
        if (g_psg.channel[ch].output_signal == level) return n;
    }
    return limit + 1;
}

/**
 * @brief Uvede PSG do čistého stavu včetně čítačů a výstupů kanálů.
 *
 * psg_init() nastavuje jen typy kanálů, hlasitost a registr šumu; čítače
 * z předchozího testu nechává být. Testy časování potřebují čítače na nule.
 */
static void psg_clean_init(void)
{
    memset(&g_psg_module, 0, sizeof(g_psg_module));
    psg_init(false);
}

/* PSG - výchozí stav registru šumu je 15bitový (jen bit 14) */
void test_psg_noise_reset_value(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    TEST_ASSERT_EQUAL_HEX16(PSG_NOISE_SR_RESET, g_psg.channel[PSG_CHANNEL_3].noise.shiftregister);
    TEST_ASSERT_EQUAL_HEX16(0x4000, PSG_NOISE_SR_RESET);
}

/* PSG - periodický šum: perioda 15 posunů, pulz jeden posun, posun 512 T (32 kroků) */
void test_psg_noise_periodic_fixed_rate(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0xF0);   /* hlasitost šumu na maximum */
    psg_write_byte(PSG_CH_LEFT, 0xE0);   /* periodický šum, NF = 0 */

    /* První jednička je na výstupu po 14 posunech. Čítač posunu se zápisem
     * nenuluje (a zápis sám může PSG dokrokovat), první posun proto přijde
     * za 1 až 32 kroků. */
    unsigned first = steps_until_level(PSG_CHANNEL_3, 1, 100000);
    TEST_ASSERT_GREATER_THAN_UINT(13 * NOISE_SHIFT_STEPS_NF0, first);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(14 * NOISE_SHIFT_STEPS_NF0, first);
    /* pulz trvá jeden posun, perioda 15 posunů */
    TEST_ASSERT_EQUAL_UINT(NOISE_SHIFT_STEPS_NF0, steps_until_level(PSG_CHANNEL_3, 0, 100000));
    TEST_ASSERT_EQUAL_UINT(14 * NOISE_SHIFT_STEPS_NF0, steps_until_level(PSG_CHANNEL_3, 1, 100000));
    TEST_ASSERT_EQUAL_UINT(NOISE_SHIFT_STEPS_NF0, steps_until_level(PSG_CHANNEL_3, 0, 100000));
    TEST_ASSERT_EQUAL_UINT(14 * NOISE_SHIFT_STEPS_NF0, steps_until_level(PSG_CHANNEL_3, 1, 100000));
}

/* PSG - pevné rychlosti šumu: posun po 32, 64 a 128 krocích (512, 1024, 2048 T) */
void test_psg_noise_fixed_rates(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    for (unsigned nf = 0; nf < 3; nf++) {
        unsigned shift = NOISE_SHIFT_STEPS_NF0 << nf;
        psg_clean_init();
        psg_write_byte(PSG_CH_LEFT, 0xF0);
        psg_write_byte(PSG_CH_LEFT, (uint8_t)(0xE0 | nf));
        (void) steps_until_level(PSG_CHANNEL_3, 1, 100000);
        TEST_ASSERT_EQUAL_UINT(shift, steps_until_level(PSG_CHANNEL_3, 0, 100000));
        TEST_ASSERT_EQUAL_UINT(14 * shift, steps_until_level(PSG_CHANNEL_3, 1, 100000));
    }
}

/* PSG - šum řízený tónem 2: posun jednou za celou periodu tónu (2 x dělič kroků) */
void test_psg_noise_tone2_rate(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0xC0);   /* tón 2: dolní 4 bity děliče = 0 */
    psg_write_byte(PSG_CH_LEFT, 0x01);   /* horní bity: dělič 010h */
    psg_write_byte(PSG_CH_LEFT, 0xF0);
    psg_write_byte(PSG_CH_LEFT, 0xE3);   /* periodický šum, NF = 3 */

    (void) steps_until_level(PSG_CHANNEL_3, 1, 100000);
    /* dělič 16: posun po 32 krocích (512 T), perioda 15 posunů = 7680 T */
    TEST_ASSERT_EQUAL_UINT(32, steps_until_level(PSG_CHANNEL_3, 0, 100000));
    TEST_ASSERT_EQUAL_UINT(14 * 32, steps_until_level(PSG_CHANNEL_3, 1, 100000));
}

/* PSG - bílý šum: 15bitový registr, zpětná vazba bit 0 xor bit 1, perioda 32767 posunů */
void test_psg_noise_white_sequence(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    enum { BITS = 32767 + 64 };
    static uint8_t bits[BITS];

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0xF0);
    psg_write_byte(PSG_CH_LEFT, 0xE4);   /* bílý šum, NF = 0 */

    for (unsigned i = 0; i < BITS; i++) {
        for (unsigned k = 0; k < NOISE_SHIFT_STEPS_NF0; k++) psg_instance_step(&g_psg);
        bits[i] = (uint8_t) g_psg.channel[PSG_CHANNEL_3].output_signal;
    }
    unsigned ones = 0;
    for (unsigned i = 0; i + 15 < BITS; i++) {
        TEST_ASSERT_EQUAL_UINT8(bits[i] ^ bits[i + 1], bits[i + 15]);
    }
    for (unsigned i = 0; i < 32767; i++) ones += bits[i];
    TEST_ASSERT_EQUAL_UINT(16384, ones);                 /* maximální sekvence: 2^14 jedniček */
    for (unsigned i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8(bits[i], bits[i + 32767]);
    }
}

/* PSG - registr šumu nuluje řídicí i datový bajt, zápis hlasitosti ne */
void test_psg_noise_reset_on_write(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_PSG_NOISE *noise = &g_psg.channel[PSG_CHANNEL_3].noise;

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0xF0);
    psg_write_byte(PSG_CH_LEFT, 0xE0);
    for (unsigned k = 0; k < 5 * NOISE_SHIFT_STEPS_NF0; k++) psg_instance_step(&g_psg);
    TEST_ASSERT_NOT_EQUAL(PSG_NOISE_SR_RESET, noise->shiftregister);

    /* zápis hlasitosti šumu registr nenuluje */
    uint16_t before = noise->shiftregister;
    psg_write_byte(PSG_CH_LEFT, 0xF0);
    TEST_ASSERT_EQUAL_HEX16(before, noise->shiftregister);

    /* řídicí bajt se stejnou hodnotou nuluje */
    psg_write_byte(PSG_CH_LEFT, 0xE0);
    TEST_ASSERT_EQUAL_HEX16(PSG_NOISE_SR_RESET, noise->shiftregister);
    TEST_ASSERT_EQUAL_UINT(0, g_psg.channel[PSG_CHANNEL_3].output_signal);

    /* datový bajt do registru šumu nuluje také */
    for (unsigned k = 0; k < 5 * NOISE_SHIFT_STEPS_NF0; k++) psg_instance_step(&g_psg);
    TEST_ASSERT_NOT_EQUAL(PSG_NOISE_SR_RESET, noise->shiftregister);
    psg_write_byte(PSG_CH_LEFT, 0x00);
    TEST_ASSERT_EQUAL_HEX16(PSG_NOISE_SR_RESET, noise->shiftregister);
    TEST_ASSERT_EQUAL_INT(NOISE_TYPE_PERIODIC, noise->type);
    TEST_ASSERT_EQUAL_INT(NOISE_DIV_TYPE0, noise->div_type);
}

/* PSG - dělič tónu 000h se chová jako 1024, dělič 001h drží výstup v 1 */
void test_psg_tone_divider_zero_and_one(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0x80);   /* tón 0: dělič 000h */
    psg_write_byte(PSG_CH_LEFT, 0x00);
    psg_write_byte(PSG_CH_LEFT, 0x90);   /* hlasitost na maximum */

    unsigned level = g_psg.channel[PSG_CHANNEL_0].output_signal;
    (void) steps_until_level(PSG_CHANNEL_0, level ^ 1u, 5000);       /* první překlopení */
    TEST_ASSERT_EQUAL_UINT(1024, steps_until_level(PSG_CHANNEL_0, level, 5000));
    TEST_ASSERT_EQUAL_UINT(1024, steps_until_level(PSG_CHANNEL_0, level ^ 1u, 5000));

    psg_write_byte(PSG_CH_LEFT, 0x81);   /* dělič 001h */
    psg_write_byte(PSG_CH_LEFT, 0x00);
    psg_instance_step(&g_psg);
    TEST_ASSERT_EQUAL_UINT(1, g_psg.channel[PSG_CHANNEL_0].output_signal);
    TEST_ASSERT_EQUAL_UINT(5001, steps_until_level(PSG_CHANNEL_0, 0, 5000));
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_psg_init_state);
    RUN_TEST(test_psg_write_no_crash);
    RUN_TEST(test_psg_step_no_crash);

    /* unit */
    RUN_TEST(test_psg_reinit_muted);
    RUN_TEST(test_psg_tone_frequency);
    RUN_TEST(test_psg_volume);
    RUN_TEST(test_psg_volume_all_channels);
    RUN_TEST(test_psg_noise_setup);
    RUN_TEST(test_psg_noise_shift_register);
    RUN_TEST(test_psg_mono_mode);
    RUN_TEST(test_psg_step_preserves_attenuation);
    RUN_TEST(test_psg_latch_mechanism);
    RUN_TEST(test_psg_stereo_broadcast);
    RUN_TEST(test_psg_stereo_individual);

    /* šum a dělič tónu podle měření na HW */
    RUN_TEST(test_psg_noise_reset_value);
    RUN_TEST(test_psg_noise_periodic_fixed_rate);
    RUN_TEST(test_psg_noise_fixed_rates);
    RUN_TEST(test_psg_noise_tone2_rate);
    RUN_TEST(test_psg_noise_white_sequence);
    RUN_TEST(test_psg_noise_reset_on_write);
    RUN_TEST(test_psg_tone_divider_zero_and_one);

    /* write log */
    RUN_TEST(test_psg_write_log_default_disabled);
    RUN_TEST(test_psg_write_log_enable_null);
    RUN_TEST(test_psg_write_log_mono_roundtrip);
    RUN_TEST(test_psg_write_log_disabled_skips);
    RUN_TEST(test_psg_write_log_stereo_broadcast);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
