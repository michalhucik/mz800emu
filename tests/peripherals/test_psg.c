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

#include <math.h>

#include "hw-generic/psg/psg.h"
#include "audio.h"
#include "iface/iface_audio.h"
#include "videorec/videorec_audio.h"

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

/* PSG - dělič tónu 000h se chová jako 1024, dělič 001h drží výstup v 1
 * (model čipu; zvukový log z toho dělá poloviční výchylku, viz
 * test_psg_tone_divider_one_logs_half_level) */
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

/* ================================================================
 * ZVUKOVÁ CESTA PODLE MĚŘENÍ NA HW (2026-11-18)
 * ================================================================ */

/**
 * @brief Provede jeden krok PSG přes zvukový log a vrátí zapsanou hodnotu kanálu.
 *
 * Posune čas GDG o jednu periodu PSG (PSG_DIVIDER tiků) a dokrokuje zvukový
 * log do něj. Čas se posouvá v GDG, ne jen v logu: psg_write_byte() krokuje
 * log do času GDG a ten nesmí být před last_psg_timestamp logu.
 *
 * @param ch Kanál PSG0 (0 až 3).
 * @return Hodnota zvukového logu kanálu po kroku (0 až 30).
 * @pre Log je dokrokovaný do času GDG (platí po psg_write_byte()).
 */
static int log_value_after_step(unsigned ch)
{
    g_gdg.total_elapsed.ticks += PSG_DIVIDER;
    audio_log_fill_psg(gdg_get_total_ticks());
    return g_audio.log->src[AUDIO_SOURCE_ID_SHIFT_PSG0 + ch]->last_value;
}

/* PSG - čítač tónového kanálu běží i při vypnuté hlasitosti (útlum 15) */
void test_psg_muted_tone_counter_runs(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0x80);   /* tón 0: dělič 010h, hlasitost zůstává vypnutá */
    psg_write_byte(PSG_CH_LEFT, 0x01);
    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg.channel[PSG_CHANNEL_0].attn);

    unsigned level = g_psg.channel[PSG_CHANNEL_0].output_signal;
    (void) steps_until_level(PSG_CHANNEL_0, level ^ 1u, 5000);       /* první překlopení */
    TEST_ASSERT_EQUAL_UINT(16, steps_until_level(PSG_CHANNEL_0, level, 5000));
    TEST_ASSERT_EQUAL_UINT(16, steps_until_level(PSG_CHANNEL_0, level ^ 1u, 5000));

    /* Zapnutí hlasitosti fázi nemění: další překlopení přijde po zbytku
     * půlperiody. Útlum se nastavuje přímo ve stavu - psg_write_byte() posouvá
     * čas (čekání procesoru) a mohl by PSG sám odkrokovat. */
    for (unsigned k = 0; k < 5; k++) psg_instance_step(&g_psg);
    g_psg.channel[PSG_CHANNEL_0].attn = PSG_OUT_MAX;
    TEST_ASSERT_EQUAL_UINT(11, steps_until_level(PSG_CHANNEL_0, level, 5000));
    TEST_ASSERT_EQUAL_UINT(16, steps_until_level(PSG_CHANNEL_0, level ^ 1u, 5000));
}

/* PSG - dva vypnuté kanály, které dostaly stejný dělič v různou dobu, nejsou po zapnutí ve fázi */
void test_psg_muted_channels_keep_phase(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Stav se nastavuje přímo (psg_write_byte() posouvá čas a mohl by PSG
     * sám odkrokovat). Oba kanály mají hlasitost vypnutou a čítače na nule. */
    psg_clean_init();
    g_psg.channel[PSG_CHANNEL_0].tone.divider = 0x10;
    g_psg.channel[PSG_CHANNEL_1].tone.divider = 0x07;
    for (unsigned k = 0; k < 40; k++) psg_instance_step(&g_psg);
    g_psg.channel[PSG_CHANNEL_1].tone.divider = 0x10;   /* stejný dělič o 40 kroků později */
    for (unsigned k = 0; k < 100; k++) psg_instance_step(&g_psg);
    g_psg.channel[PSG_CHANNEL_0].attn = PSG_OUT_MAX;    /* oba kanály zapnout */
    g_psg.channel[PSG_CHANNEL_1].attn = PSG_OUT_MAX;

    /* Kanál 1 začal čítat dělič 010h jindy než kanál 0; fázový rozdíl se
     * zapnutím hlasitosti nesrovná (před opravou stály oba čítače na nule
     * a po zapnutí běžely ve fázi). */
    unsigned differ = 0;
    for (unsigned k = 0; k < 64; k++) {
        psg_instance_step(&g_psg);
        if (g_psg.channel[PSG_CHANNEL_0].output_signal != g_psg.channel[PSG_CHANNEL_1].output_signal) differ++;
    }
    TEST_ASSERT_GREATER_THAN_UINT(0, differ);
}

/* PSG - šumový kanál posouvá registr i při vypnuté hlasitosti ([neověřeno] na HW) */
void test_psg_muted_noise_counter_runs(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0xE0);   /* periodický šum, NF = 0, hlasitost vypnutá */
    TEST_ASSERT_EQUAL_INT(PSG_OUT_OFF, g_psg.channel[PSG_CHANNEL_3].attn);

    unsigned first = steps_until_level(PSG_CHANNEL_3, 1, 100000);
    TEST_ASSERT_GREATER_THAN_UINT(13 * NOISE_SHIFT_STEPS_NF0, first);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(14 * NOISE_SHIFT_STEPS_NF0, first);
    TEST_ASSERT_EQUAL_UINT(NOISE_SHIFT_STEPS_NF0, steps_until_level(PSG_CHANNEL_3, 0, 100000));
}

/* Zvukový log - vypnutý kanál nehraje, i když jeho čítač běží */
void test_psg_muted_channel_logs_silence(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0x82);   /* tón 0: dělič 002h, hlasitost vypnutá */
    psg_write_byte(PSG_CH_LEFT, 0x00);
    /* Čítač může doběhnout předchozí dělič (000h = 1024 kroků); počkat na
     * první překlopení, od něj platí dělič 002h. */
    unsigned first = g_psg.channel[PSG_CHANNEL_0].output_signal;
    for (unsigned k = 0; k < 1100 && g_psg.channel[PSG_CHANNEL_0].output_signal == first; k++) {
        TEST_ASSERT_EQUAL_INT(0, log_value_after_step(PSG_CHANNEL_0));
    }
    TEST_ASSERT_NOT_EQUAL(first, g_psg.channel[PSG_CHANNEL_0].output_signal);
    unsigned ones = 0;
    for (unsigned k = 0; k < 16; k++) {
        TEST_ASSERT_EQUAL_INT(0, log_value_after_step(PSG_CHANNEL_0));
        ones += g_psg.channel[PSG_CHANNEL_0].output_signal;
    }
    TEST_ASSERT_EQUAL_UINT(8, ones);     /* výstup čítače se přitom překlápí */

    /* po zapnutí hlasitosti log střídá 0 a stupeň 15 - útlum */
    psg_write_byte(PSG_CH_LEFT, 0x93);   /* útlum 3 */
    unsigned high = 0;
    for (unsigned k = 0; k < 16; k++) {
        int v = log_value_after_step(PSG_CHANNEL_0);
        TEST_ASSERT_TRUE(v == 0 || v == 12);
        if (v == 12) high++;
    }
    TEST_ASSERT_EQUAL_UINT(8, high);
}

/* Zvukový log - tón s děličem 001h je stálá poloviční výchylka stupně hlasitosti */
void test_psg_tone_divider_one_logs_half_level(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    psg_clean_init();
    psg_write_byte(PSG_CH_LEFT, 0x81);   /* tón 0: dělič 001h */
    psg_write_byte(PSG_CH_LEFT, 0x00);

    for (unsigned attn = 0; attn < 15; attn++) {
        psg_write_byte(PSG_CH_LEFT, (uint8_t)(0x90 | attn));
        for (unsigned k = 0; k < 4; k++) {
            int v = log_value_after_step(PSG_CHANNEL_0);
            TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_HALF_BASE + AUDIO_MAXVAL_PER_CHANNEL - (int)attn, v);
            TEST_ASSERT_TRUE(audio_level_is_half((unsigned)v));
            TEST_ASSERT_EQUAL_UINT(AUDIO_MAXVAL_PER_CHANNEL - attn, audio_level_step((unsigned)v));
        }
    }
    /* vypnutý kanál je ticho i s děličem 001h */
    psg_write_byte(PSG_CH_LEFT, 0x9F);
    TEST_ASSERT_EQUAL_INT(0, log_value_after_step(PSG_CHANNEL_0));

    /* šumový kanál poloviční výchylku nepíše */
    psg_write_byte(PSG_CH_LEFT, 0xF0);
    psg_write_byte(PSG_CH_LEFT, 0xE0);
    for (unsigned k = 0; k < 64; k++) {
        TEST_ASSERT_FALSE(audio_level_is_half((unsigned)log_value_after_step(PSG_CHANNEL_3)));
    }
}

/** @brief Poměr dvou výchylek v dB. */
static double ratio_db(double a, double b)
{
    return 20.0 * log10(a / b);
}

/* Tabulky úrovní - poloviční výchylka je o 6,2 dB pod plnou pro všechny útlumy */
void test_audio_levels_half_is_6db2_below_full(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    float saved_gain[AUDIO_SRC_CHANNELS_COUNT];
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        saved_gain[i] = g_iface_audio.gain[i];
        g_iface_audio.gain[i] = 1.0f;
        iface_audio_set_src_volume(i, 100);
    }
    static float lv[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    memset(lv, 0, sizeof(lv));
    iface_audio_build_videorec_levels(lv, VIDEOREC_AUDIO_MAX_CHANNELS);

    for (int ch = 1; ch < AUDIO_SRC_CHANNELS_COUNT; ch++) {
        const float *sdl = g_iface_audio.SN76489_volume_value[ch];
        TEST_ASSERT_EQUAL_FLOAT(0.0f, sdl[0]);
        TEST_ASSERT_EQUAL_FLOAT(1.0f, sdl[15]);
        for (int step = 1; step <= 15; step++) {
            int half = AUDIO_LEVEL_HALF_BASE + step;
            /* SDL cesta (tabulka výchylek) i reference a video záznam (tabulka úrovní) */
            TEST_ASSERT_DOUBLE_WITHIN(0.3, -6.2, ratio_db(sdl[half], sdl[step]));
            TEST_ASSERT_DOUBLE_WITHIN(0.3, -6.2, ratio_db(lv[ch][half], lv[ch][step]));
            /* krok hlasitosti zůstává 2 dB */
            if (step > 1) TEST_ASSERT_DOUBLE_WITHIN(0.01, 2.0, ratio_db(sdl[step], sdl[step - 1]));
        }
        TEST_ASSERT_EQUAL_FLOAT(0.0f, lv[ch][VIDEOREC_AUDIO_LEVELS - 1]);
    }
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = saved_gain[i];
}

/* Tabulky úrovní a mix - CTC0 je 1,49 násobek jednoho kanálu PSG s útlumem 0 */
void test_audio_levels_ctc0_to_psg_ratio(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    float saved_gain[AUDIO_SRC_CHANNELS_COUNT];
    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        saved_gain[i] = g_iface_audio.gain[i];
        g_iface_audio.gain[i] = 1.0f;
        iface_audio_set_src_volume(i, 100);
    }
    static float lv[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    memset(lv, 0, sizeof(lv));
    iface_audio_build_videorec_levels(lv, VIDEOREC_AUDIO_MAX_CHANNELS);

    /* reference a video záznam: +3,5 dB */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lv[0][0]);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1.49, (double)lv[0][15] / (double)lv[1][15]);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 3.5, ratio_db(lv[0][15], lv[1][15]));

    /* SDL cesta: střední zesílení převzorkování CTC0 krát násobitel mixu */
    float k = videorec_audio_sdl_ctc0_gain(IFACE_AUDIO_CTC5253_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC,
                                           IFACE_AUDIO_SAMPLE_RATE / VIDEO_SCREENS_PER_SEC);
    double psg_channel = (double)g_iface_audio.SN76489_volume_value[1][15] / (double)PSG_CHANNELS_COUNT;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1.49, (double)(k * iface_audio_ctc0_mix_level()) / psg_channel);

    /* uživatelská hlasitost zůstává násobitelem navíc */
    g_iface_audio.gain[0] = 0.5f;
    g_iface_audio.gain[1] = 2.0f;
    iface_audio_build_videorec_levels(lv, VIDEOREC_AUDIO_MAX_CHANNELS);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1.49 * 0.5 / 2.0, (double)lv[0][15] / (double)lv[1][15]);

    for (int i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) g_iface_audio.gain[i] = saved_gain[i];
}

/* Volba věrné výstupní propusti: výchozí zapnuto, jde přepnout, tabulky úrovní se jí nemění.
 * Samostatný MZ-700 propust nemá: volba je tam trvale vypnutá a zapnout nejde. */
void test_audio_output_filter_option(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

#if !AUDIO_HAVE_OUTPUT_FILTER
    TEST_ASSERT_FALSE(iface_audio_output_filter_enabled());
    iface_audio_set_output_filter(true);
    TEST_ASSERT_FALSE(iface_audio_output_filter_enabled());
    return;
#endif
    TEST_ASSERT_TRUE(iface_audio_output_filter_enabled());   /* výchozí stav [AUDIO] output_filter */
    static float lv_on[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    static float lv_off[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    memset(lv_on, 0, sizeof(lv_on));
    memset(lv_off, 0, sizeof(lv_off));
    iface_audio_build_videorec_levels(lv_on, VIDEOREC_AUDIO_MAX_CHANNELS);
    iface_audio_set_output_filter(false);
    TEST_ASSERT_FALSE(iface_audio_output_filter_enabled());
    iface_audio_build_videorec_levels(lv_off, VIDEOREC_AUDIO_MAX_CHANNELS);
    /* žádné dorovnání hlasitosti: úrovně kanálů jsou v obou stavech stejné */
    TEST_ASSERT_EQUAL_MEMORY(lv_on, lv_off, sizeof(lv_on));
    iface_audio_set_output_filter(true);
    TEST_ASSERT_TRUE(iface_audio_output_filter_enabled());
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

    /* zvuková cesta podle měření na HW */
    RUN_TEST(test_psg_muted_tone_counter_runs);
    RUN_TEST(test_psg_muted_channels_keep_phase);
    RUN_TEST(test_psg_muted_noise_counter_runs);
    RUN_TEST(test_psg_muted_channel_logs_silence);
    RUN_TEST(test_psg_tone_divider_one_logs_half_level);
    RUN_TEST(test_audio_levels_half_is_6db2_below_full);
    RUN_TEST(test_audio_levels_ctc0_to_psg_ratio);
    RUN_TEST(test_audio_output_filter_option);

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
