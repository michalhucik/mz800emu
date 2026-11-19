/**
 * @file   test_audio_ctc0_div2.c
 * @brief  Zvukový kanál CTC0 při režimu 3 s děličem 2 (obdélník 554 kHz).
 *
 * Změřeno na MZ-800 (2026-11-19): CTC0 v režimu 3 s děličem 2 není jako stálý
 * signál slyšet, klíčovaný bitem PC0 na 440 Hz ale dává čistý tón o úrovni
 * 0,64 klíčované stálé jedničky (AUDIO_CTC0_DIV2_FACTOR). Emulátor proto po
 * dobu, kdy CTC0 s děličem 2 skutečně čítá, posílá do zvukového logu místo
 * hran stálou hodnotu AUDIO_LEVEL_CTC0_DIV2 maskovanou PC0. Testuje se:
 * - stálý signál: jediná hodnota v logu, po doznění střídavé vazby ticho,
 * - klíčování PC0 na 440 Hz: základní složka i efektivní hodnota 0,64
 *   klíčované stálé jedničky, dvě hrany na periodu (žádný šum),
 * - děliče 4 a 16 beze změny (úroveň 0,50, skutečné hrany OUT0),
 * - dělič platí od nahrání do čítače, ne od zápisu; jiný dělič, brána
 *   a řídicí slovo vracejí kanál ke skutečnému OUT0,
 * - echo kazety, odvození stavu po načtení snapshotu, tabulky úrovní
 *   zvukových cest.
 *
 * Čítač se krokuje rychlou cestou CTC0 (ctc8253_ctc1m1_event()) v čase GDG,
 * PC0 se zapisuje přes pio8255_write().
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <math.h>
#include <string.h>
#include <glib.h>

#include "audio.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"
#include "iface/iface_audio.h"
#include "iface/iface_audio_resampler.h"
#include "videorec/videorec_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief Půlperioda klíčování 440 Hz v ticích GDG. */
#define KEY_HALF_TICKS ((unsigned)(GDGCLK_BASE / 880u))

/** @brief Výsledek rozboru kanálu CTC0 zvukového logu v časovém okně. */
typedef struct st_MEAS
{
    double mean;       /**< Střední výchylka. */
    double fund;       /**< Amplituda složky na kmitočtu klíčování. */
    double rms;        /**< Efektivní hodnota po odečtení střední výchylky. */
    unsigned events;   /**< Počet změn hodnoty uvnitř okna. */
    uint32_t values;   /**< Bitová maska hodnot logu, které se v okně vyskytly. */
} st_MEAS;

/** @brief Krokuje CTC0 do času @p target (tiky snímku) a posune na něj čas GDG. */
static void run_to(unsigned target)
{
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    while (((int)ctc0->clk1m1_event.ticks != -1) && (ctc0->clk1m1_event.ticks <= target))
    {
        ctc8253_ctc1m1_event(ctc0->clk1m1_event.ticks);
    };
    g_gdg.total_elapsed.ticks = target;
}

/** @brief Posune čas o @p ticks tiků GDG. */
static void advance(unsigned ticks)
{
    run_to(g_gdg.total_elapsed.ticks + ticks);
}

/** @brief Posune čas o @p clocks period CLK0. */
static void advance_clk(unsigned clocks)
{
    advance(clocks * GDGCLK_CTC0_DIVIDER);
}

/** @brief Zapíše dvoubajtový dělič CTC0. */
static void ctc0_divider(unsigned n)
{
    ctc8253_write_byte(CTCADDR_CTC0, (uint8_t)(n & 0xff));
    ctc8253_write_byte(CTCADDR_CTC0, (uint8_t)(n >> 8));
}

/** @brief Řídicí slovo CTC0: režim 3, LSB + MSB, binárně. */
static void ctc0_mode3(void)
{
    ctc8253_write_byte(CTCADDR_CWREG, 0x36);
}

/** @brief Adresa řídicího registru 8255 (DEF_PIO8255_MASTER v pio8255.c). */
#define PIO8255_ADDR_CONTROL 0x03

/** @brief Nastaví bit PC0 povelem bit set/reset 8255. Na samostatném MZ-700 zvuk nehradluje. */
static void pc0(bool on)
{
    pio8255_write(PIO8255_ADDR_CONTROL, on ? 0x01 : 0x00);
}

/** @brief Hodnota kanálu CTC0 ve zvukovém logu. */
static int ctc0_log_value(void)
{
    return g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)]->last_value;
}

/** @brief Počet úseků kanálu CTC0 ve zvukovém logu. */
static unsigned ctc0_log_count(void)
{
    return g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)]->samples_count;
}

/**
 * @brief Rozbor kanálu CTC0 zvukového logu v okně [ta, tb).
 *
 * Signál je po částech stálý s výchylkou audio_level_amplitude(hodnota);
 * integrály se počítají přesně z časů změn.
 *
 * @param ta     Začátek okna (osa gdg_get_total_ticks()).
 * @param tb     Konec okna; okno má být celý počet period.
 * @param period Perioda hledané složky v ticích.
 * @return Střední výchylka, amplituda složky, efektivní hodnota, počet změn a hodnoty.
 */
static st_MEAS measure(uint64_t ta, uint64_t tb, double period)
{
    const st_AUDIO_SOURCE_LOG *src = g_audio.log->src[audio_source_id(AUDIO_SRC_CTC0)];
    const double w = 2.0 * M_PI / period;
    const double len = (double)(tb - ta);
    double sum = 0.0, sum2 = 0.0, re = 0.0, im = 0.0;
    st_MEAS m;
    memset(&m, 0, sizeof(m));

    uint64_t t = g_audio.log->first_timestamp;
    unsigned n = src->samples_count ? src->samples_count : 1;
    for (unsigned i = 0; i < n; i++)
    {
        int value = src->samples_count ? src->samples[i].value : src->last_value;
        uint64_t end = (i + 1 < n) ? (t + src->samples[i].count_ticks) : UINT64_MAX;
        if ((i > 0) && (t > ta) && (t < tb))
            m.events++;
        uint64_t s = (t > ta) ? t : ta;
        uint64_t e = (end < tb) ? end : tb;
        if (e > s)
        {
            double a = audio_level_amplitude((unsigned)value);
            double x1 = w * (double)(s - ta), x2 = w * (double)(e - ta);
            sum += a * (double)(e - s);
            sum2 += a * a * (double)(e - s);
            re += a * (sin(x2) - sin(x1)) / w;
            im += a * (cos(x1) - cos(x2)) / w;
            m.values |= 1u << value;
        };
        t = end;
    };

    m.mean = sum / len;
    m.fund = 2.0 * sqrt(re * re + im * im) / len;
    double var = sum2 / len - m.mean * m.mean;
    m.rms = (var > 0.0) ? sqrt(var) : 0.0;
    return m;
}

#if MZARCH != 700
/**
 * @brief Klíčuje PC0 kmitočtem 440 Hz a změří kanál CTC0.
 *
 * Čtyři periody náběhu, pak @p periods měřených.
 *
 * @param periods Počet měřených period.
 * @return Rozbor kanálu CTC0 v měřeném okně.
 * @post PC0 = 0.
 */
static st_MEAS key_pc0(unsigned periods)
{
    uint64_t ta = 0;
    for (unsigned k = 0; k < periods + 4; k++)
    {
        if (k == 4)
            ta = gdg_get_total_ticks();
        pc0(true);
        advance(KEY_HALF_TICKS);
        pc0(false);
        advance(KEY_HALF_TICKS);
    };
    return measure(ta, gdg_get_total_ticks(), 2.0 * KEY_HALF_TICKS);
}
#endif

void setUp(void)
{
    /* Čítače a PC0 ve výchozím stavu, čistý log sekundu za dosavadními událostmi. */
    pc0(false);
    ctc8253_init();
    g_audio.cmt_echo = 0;
    audio_ctc0_restore(false, false);
    g_gdg.total_elapsed.ticks += (unsigned)GDGCLK_BASE;
    audio_reset_log(gdg_get_total_ticks());
    ctc8253_gate(CTC_CS0, 1, g_gdg.total_elapsed.ticks);
}

void tearDown(void) { }

/* Hodnota 31: výchylka 0,64 stupně 15, není poloviční výchylkou PSG. */
void test_div2_level_value(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    TEST_ASSERT_EQUAL_INT(31, AUDIO_LEVEL_CTC0_DIV2);
    TEST_ASSERT_EQUAL_INT(32, AUDIO_LEVEL_VALUES_COUNT);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, audio_level_amplitude(AUDIO_LEVEL_CTC0_DIV2));
    TEST_ASSERT_TRUE(audio_level_is_ctc0_div2(AUDIO_LEVEL_CTC0_DIV2));
    TEST_ASSERT_FALSE(audio_level_is_half(AUDIO_LEVEL_CTC0_DIV2));
    TEST_ASSERT_TRUE(audio_level_is_half(30));
    TEST_ASSERT_FALSE(audio_level_is_ctc0_div2(30));
    TEST_ASSERT_EQUAL_UINT(AUDIO_MAXVAL_PER_CHANNEL, audio_level_step(AUDIO_LEVEL_CTC0_DIV2));
    TEST_ASSERT_EQUAL_UINT(15, audio_level_step(30));
    TEST_ASSERT_EQUAL_UINT(7, audio_level_step(7));
}

/* Stálý signál 554 kHz: v logu jediná stálá hodnota 31, po doznění vazby ticho. */
void test_div2_constant_is_silent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pc0(true);
    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, g_audio.ctc0_ac.level);

    /* čítač běží dál (OUT0 se mění), do zvuku ale nejde žádná další událost */
    unsigned count = ctc0_log_count();
    TEST_ASSERT_TRUE(count <= 3);
    unsigned out_changes = 0, out = g_ctc8253[CTC_CS0].out;
    for (unsigned i = 0; i < 100; i++)
    {
        advance_clk(1);
        if (g_ctc8253[CTC_CS0].out != out)
            out_changes++;
        out = g_ctc8253[CTC_CS0].out;
    };
    TEST_ASSERT_EQUAL_UINT(100, out_changes);
    TEST_ASSERT_EQUAL_UINT(count, ctc0_log_count());

    /* střídavá vazba: skok 0,64 dozní (6 časových konstant = 5,1 s) */
    advance((unsigned)(6u * (uint64_t)GDGCLK_BASE * AUDIO_CTC0_AC_TAU_MS / 1000u));
    TEST_ASSERT_EQUAL_UINT(count, ctc0_log_count());
    double ac = g_audio.ctc0_ac.level - audio_ctc0_ac_mean(gdg_get_total_ticks());
    TEST_ASSERT_TRUE(fabs(ac) < 0.005);
}

#if MZARCH != 700
/* Klíčování PC0 na 440 Hz: úroveň 0,64 klíčované stálé jedničky, čistý obdélník. */
void test_div2_keyed_level_is_0_64(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* srovnávací signál: OUT0 trvale v jedničce (řídicí slovo bez děliče) */
    ctc0_mode3();
    advance_clk(10);
    TEST_ASSERT_EQUAL_UINT(1, g_ctc8253[CTC_CS0].out);
    st_MEAS ref = key_pc0(100);
    TEST_ASSERT_EQUAL_HEX32((1u << 0) | (1u << AUDIO_MAXVAL_PER_CHANNEL), ref.values);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 2.0 / M_PI, ref.fund);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.5, ref.rms);

    ctc0_divider(2);
    advance_clk(100);
    st_MEAS m = key_pc0(100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    /* jen hodnoty 0 a 31, dvě změny na periodu jako u stálé jedničky */
    TEST_ASSERT_EQUAL_HEX32((1u << 0) | (1u << AUDIO_LEVEL_CTC0_DIV2), m.values);
    TEST_ASSERT_EQUAL_UINT(ref.events, m.events);
    TEST_ASSERT_DOUBLE_WITHIN(0.005, 0.64, m.fund / ref.fund);
    /* efektivní hodnota odpovídá obdélníku téže úrovně (žádný šum) */
    TEST_ASSERT_DOUBLE_WITHIN(0.005, 0.64, m.rms / ref.rms);
    TEST_ASSERT_DOUBLE_WITHIN(0.005, M_PI / 4.0, m.rms / m.fund);
}

/* Děliče 4 a 16 se nemění: do zvuku jdou skutečné hrany, klíčovaný tón má úroveň 0,50. */
void test_div4_div16_keyed_unchanged(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const unsigned dividers[] = { 4, 16 };

    ctc0_mode3();
    advance_clk(10);
    st_MEAS ref = key_pc0(50);

    for (unsigned i = 0; i < 2; i++)
    {
        ctc0_mode3();
        ctc0_divider(dividers[i]);
        advance_clk(100);
        st_MEAS m = key_pc0(50);
        TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
        TEST_ASSERT_EQUAL_HEX32((1u << 0) | (1u << AUDIO_MAXVAL_PER_CHANNEL), m.values);
        /* skutečné hrany OUT0: desítky až stovky změn na periodu klíčování */
        TEST_ASSERT_TRUE(m.events > 20u * ref.events);
        TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.50, m.fund / ref.fund);
    };
}

/* PC0 = 0 kanál umlčí, PC0 = 1 dá vždy hodnotu 31 bez ohledu na okamžitý stav OUT0. */
void test_div2_pc0_mask(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_INT(0, ctc0_log_value());

    unsigned out_seen = 0;
    for (unsigned i = 0; i < 64; i++)
    {
        pc0(true);
        out_seen |= 1u << g_ctc8253[CTC_CS0].out;
        TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
        advance(5);
        pc0(false);
        TEST_ASSERT_EQUAL_INT(0, ctc0_log_value());
        advance(6);
    };
    TEST_ASSERT_EQUAL_HEX32(3, out_seen); /* zápisy trefily obě úrovně OUT0 */
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
}
#endif /* MZARCH != 700 */

/* Jiný dělič vrací kanál ke skutečným hranám; dělič 2 platí až od nahrání do čítače. */
void test_div2_divider_change(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pc0(true);
    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);

    /* 2 -> 4096: nový dělič se nahraje nejbližší hranou */
    ctc0_divider(0x1000);
    advance_clk(8);
    TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_INT(g_ctc8253[CTC_CS0].out ? AUDIO_MAXVAL_PER_CHANNEL : 0, ctc0_log_value());

    /* kanál sleduje OUT0 */
    unsigned out = g_ctc8253[CTC_CS0].out;
    advance_clk(0x800);
    TEST_ASSERT_EQUAL_UINT(out ^ 1u, g_ctc8253[CTC_CS0].out);
    TEST_ASSERT_EQUAL_INT(g_ctc8253[CTC_CS0].out ? AUDIO_MAXVAL_PER_CHANNEL : 0, ctc0_log_value());

    /* 4096 -> 2: do konce rozpočítané půlperiody platí starý dělič */
    advance_clk(100);
    out = g_ctc8253[CTC_CS0].out;
    ctc0_divider(2);
    TEST_ASSERT_EQUAL_UINT(2, g_ctc8253[CTC_CS0].preset_value);
    advance_clk(0x700);
    TEST_ASSERT_EQUAL_UINT(out, g_ctc8253[CTC_CS0].out);
    TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_INT(out ? AUDIO_MAXVAL_PER_CHANNEL : 0, ctc0_log_value());
    advance_clk(0x100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
}

/* Zastavení bránou a řídicí slovo vracejí kanál k OUT0 (= 1), ať byl OUT0 v kterékoli úrovni. */
void test_div2_gate_and_control_word(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unsigned out_seen = 0;

    pc0(true);
    for (unsigned i = 0; i < 4; i++)
    {
        ctc0_mode3();
        ctc0_divider(2);
        advance_clk(100 + i);
        TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
        TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
        out_seen |= 1u << g_ctc8253[CTC_CS0].out;

        if (i & 2)
        {
            /* brána: OUT0 jde hned do jedničky a čítání stojí */
            ctc8253_gate(CTC_CS0, 0, g_gdg.total_elapsed.ticks);
            TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
            TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
            advance_clk(50);
            TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());

            /* po otevření brány se dělič 2 rozběhne znovu */
            ctc8253_gate(CTC_CS0, 1, g_gdg.total_elapsed.ticks);
            advance_clk(50);
            TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
            TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
        }
        else
        {
            /* řídicí slovo: OUT0 = 1, čítač čeká na dělič */
            ctc0_mode3();
            TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
            TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
            advance_clk(50);
            TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
        };
    };
    TEST_ASSERT_EQUAL_HEX32(3, out_seen); /* obě úrovně OUT0 v okamžiku zastavení */
}

/* Echo kazety: při děliči 2 a PC0 = 1 hodnotu nemění, při PC0 = 0 hraje samo. */
void test_div2_cmt_echo(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pc0(true);
    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());

    audio_cmt_echo_changed(true, gdg_get_total_ticks());
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
#if MZARCH != 700
    advance_clk(10);
    pc0(false);
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    audio_cmt_echo_changed(false, gdg_get_total_ticks());
    TEST_ASSERT_EQUAL_INT(0, ctc0_log_value());
    pc0(true);
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
#endif
    audio_cmt_echo_changed(false, gdg_get_total_ticks());
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
}

/* Po načtení snapshotu se stav kanálu odvodí z čítače a log začíná jeho úrovní. */
void test_div2_restore_from_counter_state(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pc0(true);
    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);

    /* stav zvuku "zapomenout" a odvodit znovu */
    audio_ctc0_restore(false, false);
    ctc8253_ctc0_audio_restore();
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_output);
    audio_reset_log(gdg_get_total_ticks());
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, g_audio.ctc0_ac.level);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, g_audio.log->ctc0_ac_mean);

    /* čítač běží dál, do logu nepřibude nic */
    advance_clk(100);
    TEST_ASSERT_EQUAL_UINT(0, ctc0_log_count());
    TEST_ASSERT_EQUAL_INT(AUDIO_LEVEL_CTC0_DIV2, ctc0_log_value());

    /* zastavený čítač s OUT0 = 1: běžná jednička */
    ctc0_mode3();
    audio_ctc0_restore(false, true);
    ctc8253_ctc0_audio_restore();
    TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_output);
    audio_reset_log(gdg_get_total_ticks());
    TEST_ASSERT_EQUAL_INT(AUDIO_MAXVAL_PER_CHANNEL, ctc0_log_value());
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, g_audio.log->ctc0_ac_mean);
}

/* Reset čítačů stav "dělič 2" ukončí. */
void test_div2_cleared_by_init(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pc0(true);
    ctc0_mode3();
    ctc0_divider(2);
    advance_clk(100);
    TEST_ASSERT_EQUAL_UINT(1, g_audio.ctc0_div2);
    pc0(false);
    ctc8253_init();
    TEST_ASSERT_EQUAL_UINT(0, g_audio.ctc0_div2);
}

/* Zvukové cesty: hodnota 31 má 0,64 úrovně hodnoty 15 v převzorkování SDL cesty i v rendereru. */
void test_div2_level_in_audio_paths(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* tabulka rendereru (video záznam, reference.wav) */
    float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
    memset(level, 0, sizeof(level));
    iface_audio_build_videorec_levels(level, VIDEOREC_AUDIO_MAX_CHANNELS);
    TEST_ASSERT_TRUE(level[0][AUDIO_MAXVAL_PER_CHANNEL] > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.64f * level[0][AUDIO_MAXVAL_PER_CHANNEL], level[0][AUDIO_LEVEL_CTC0_DIV2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, 0.0f, level[0][0]);

    /* střídavá vazba rendereru začíná ustálená na úrovni počáteční hodnoty */
    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    memset(values, 0, sizeof(values));
    values[0] = AUDIO_LEVEL_CTC0_DIV2;
    st_VIDEOREC_AUDIO *a = g_malloc0(sizeof(*a));
    videorec_audio_init(a, GDGCLK_BASE, 48000, AUDIO_SRC_CHANNELS_COUNT,
                        (const float (*)[VIDEOREC_AUDIO_LEVELS])level, values, 0, true);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.64, a->flt[0].hp_mean);
    videorec_audio_free(a);
    values[0] = AUDIO_MAXVAL_PER_CHANNEL;
    videorec_audio_init(a, GDGCLK_BASE, 48000, AUDIO_SRC_CHANNELS_COUNT,
                        (const float (*)[VIDEOREC_AUDIO_LEVELS])level, values, 0, true);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, a->flt[0].hp_mean);
    videorec_audio_free(a);
    g_free(a);

    /* převzorkování SDL cesty: první vzorek po střídavé vazbě ze stavu 0 */
    const size_t in_count = IFACE_AUDIO_CTC5253_SAMPLE_RATE / 50;
    const size_t out_count = IFACE_AUDIO_SAMPLE_RATE / 50;
    uint8_t *in = g_malloc(in_count);
    float *raw = g_malloc0(out_count * sizeof(float));
    float first[2];
    static const uint8_t v[2] = { AUDIO_MAXVAL_PER_CHANNEL, AUDIO_LEVEL_CTC0_DIV2 };
    for (unsigned i = 0; i < 2; i++)
    {
        memset(in, v[i], in_count);
        iface_audio_resampler_ctc0_ac_seed(0.0);
        float *out = iface_audio_resampler_output_stream_ctc0(in, in_count, out_count, raw);
        TEST_ASSERT_NOT_NULL(out);
        first[i] = raw[0];
        g_free(out);
    };
    iface_audio_resampler_ctc0_ac_seed(0.0);
    g_free(in);
    g_free(raw);
    TEST_ASSERT_TRUE(first[0] > 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.64f, first[1] / first[0]);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_div2_level_value);
    RUN_TEST(test_div2_constant_is_silent);
#if MZARCH != 700
    RUN_TEST(test_div2_keyed_level_is_0_64);
    RUN_TEST(test_div4_div16_keyed_unchanged);
    RUN_TEST(test_div2_pc0_mask);
#endif
    RUN_TEST(test_div2_divider_change);
    RUN_TEST(test_div2_gate_and_control_word);
    RUN_TEST(test_div2_cmt_echo);
    RUN_TEST(test_div2_restore_from_counter_state);
    RUN_TEST(test_div2_cleared_by_init);
    RUN_TEST(test_div2_level_in_audio_paths);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
