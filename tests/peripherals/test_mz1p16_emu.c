/*
 * test_mz1p16_emu.c - headless testy integrace plotteru MZ-1P16 do emulátoru
 *
 * Pokrývá integraci plotteru MZ-1P16 (clock-domain handshake):
 *  - aktivace plotteru spustí fast-forward homing/selftest (rotace 4 per),
 *  - hot-path gate: neaktivní plotter se nekrokuje,
 *  - event-driven krokování: aktivní plotter se per-frame dokrokuje pevným
 *    rozpočtem a při host I/O (čtení BUSY) - ne podle uplynulého času,
 *  - handshake PŘES PIO CESTU (pioz80_read_byte/write_byte) pro MZ standard:
 *      data PB -> P2, STROBE PA7 -> /INT INVERTOVANĚ (PA7=0 -> INT=1 idle,
 *      PA7=1 -> INT=0 assert), BUSY plotter (P1.4) -> PA0, STA (P1.7) -> PA1;
 *    firmware v klidu = ready (PA0=0), po přijetí bajtu krátce busy a zpět;
 *    host poll smyčka čtoucí BUSY krokuje firmware (drive_poll) a SKONČÍ,
 *  - tlačítka okna Paper feed a self-test jdou přes frontu zásahů zařízení
 *    (mz1p16_request.h): provede je až emu vlákno v device_request_poll()
 *    a provedený zásah zvedne revizi stavu,
 *  - dohánění homingu (aktivace, reset) a self-testu se rozloží do snímků:
 *    jeden per-frame krok odkrokuje nejvýš omezený rozpočet a emulace
 *    stroje nestojí.
 *
 * Test obchází mz1p16_emu_init (ten registruje INI/CLI) a inicializuje jádro
 * + mechaniku přímo, aby byl self-contained bez cfgmain/sdlapp.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "hw-generic/mz1p16/mz1p16_emu.h"
#include "hw-generic/mz1p16/mz1p16.h"
#include "hw-generic/mz1p16/mcs48.h"
#include "hw-generic/mz1p16/mz1p16_rom.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/mz1p16/mz1p16_request.h"
#include "device_request.h"
#include "state_rev.h"

/* ----------------------------------------------------------------------------
 * Pomocné funkce
 * ------------------------------------------------------------------------- */

/** Inicializuje jádro 8050 + mechaniku plotteru do klidu (bez INI/CLI). */
static void plotter_core_init(void)
{
    mcs48_init(&g_mz1p16_cpu, mz1p16_rom_data());
    mz1p16_init(&g_mz1p16, &g_mz1p16_cpu);
    /* mz1p16_init nastaví active=0 (memset), plotter je neaktivní. */
}

/**
 * Posune monotónní časovou základnu GDG o @p ticks pixel-CLK (= simuluje
 * běžící Z80/GDG). Přetečení přes jednu obrazovku normalizuje do screens, aby
 * total_elapsed.ticks zůstalo v platném rozsahu (gdg_get_total_ticks() vrací
 * screens*VIDEO_SCREEN_TICKS + ticks, tedy spojitě roste).
 */
static void advance_gdg(uint64_t ticks)
{
    uint64_t t = (uint64_t)g_gdg.total_elapsed.ticks + ticks;
    while (t >= (uint64_t)VIDEO_SCREEN_TICKS) {
        t -= (uint64_t)VIDEO_SCREEN_TICKS;
        g_gdg.total_elapsed.screens++;
    }
    g_gdg.total_elapsed.ticks = (unsigned)t;
}

void setUp(void)
{
    /* GDG do definovaného stavu - timebase pro catch-up. */
    memset(&g_gdg, 0, sizeof(g_gdg));
    pioz80_init();
    pioz80_reset();
    plotter_core_init();
}

void tearDown(void)
{
    /* Přes set_active(false): zahodí i naplánované dohánění. */
    mz1p16_emu_set_active(false);
    g_mz1p16.active = false;
}

/**
 * @brief Dožene naplánované dohánění (homing, self-test) per-frame kroky.
 *
 * Simuluje konce snímků emulace (mz1p16_emu_on_screen_done()), dokud
 * mz1p16_emu_catchup_pending() neklesne na 0.
 *
 * @return Počet potřebných snímků (strop 1000 = selhání testu).
 */
static int catch_up(void)
{
    int frames = 0;
    while (mz1p16_emu_catchup_pending() > 0 && frames < 1000) {
        mz1p16_emu_on_screen_done();
        frames++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)mz1p16_emu_catchup_pending(), "dohánění nedoběhlo");
    return frames;
}

/* ================================================================
 * FÁZE A - aktivace + krokování
 * ================================================================ */

/* Po init je plotter neaktivní a jádro nekrokované (cycles == 0 z resetu). */
void test_inactive_after_init(void)
{
    TEST_ASSERT_FALSE(mz1p16_emu_get_active());
    /* Neaktivní per-frame krok = no-op: cycles se nezmění, i když uplyne čas. */
    uint64_t before = g_mz1p16_cpu.cycles;
    advance_gdg(100000);
    mz1p16_emu_on_screen_done();
    TEST_ASSERT_EQUAL_UINT64(before, g_mz1p16_cpu.cycles);
}

/* Aktivace plotteru spustí fast-forward homing/selftest = rotace všech 4 per.
 * Tím je prokázáno, že firmware na embeddované ROM v emu kontextu naběhne. */
void test_activation_runs_selftest(void)
{
    mz1p16_emu_set_active(true);
    TEST_ASSERT_TRUE(mz1p16_emu_get_active());
    /* Aktivace sama nekrokuje, homing se dožene po snímcích. */
    TEST_ASSERT_TRUE(mz1p16_emu_catchup_pending() > 0);
    TEST_ASSERT_EQUAL_UINT64(0, g_mz1p16_cpu.cycles);
    catch_up();

    /* Po fast-forwardu firmware odhomoval vozík k levému dorazu a opakovaným
     * vjezdem do pen-change zóny prorotoval barvy. */
    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16.color_changes >= 4,
        "fast-forward neprorotoval alespon 4 pera (selftest nenabehl)");
    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16_cpu.cycles > 1000000ull,
        "fast-forward neodkrokoval ocekavany budget cyklu");
}

/* Momentální stisk PEN CHANGE (puls T0) otočí buben na další barvu.
 * Pouhé nastavení T0=0 bez odkrokování by firmware nezpracoval; puls dělá
 * fast-forward, takže color_changes musí po stisku vzrůst. No-op u
 * neaktivního plotteru. */
void test_pulse_pen_change_rotates_color(void)
{
    /* Neaktivní = no-op (jádro se nekrokuje). */
    uint64_t cyc0 = g_mz1p16_cpu.cycles;
    mz1p16_emu_pulse_pen_change();
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(cyc0, g_mz1p16_cpu.cycles,
        "pen change u neaktivniho plotteru nesmi krokovat jadro");

    mz1p16_emu_set_active(true);
    catch_up();
    uint64_t changes_before = g_mz1p16.color_changes;

    mz1p16_emu_pulse_pen_change();

    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16.color_changes > changes_before,
        "PEN CHANGE puls neotocil buben (color_changes nevzrostlo)");
    /* T0 je po pulzu zpět v klidu (1). */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, g_mz1p16_cpu.T0,
        "T0 nezustal po pulzu v klidu (1)");
}

/* Aktivní plotter se per-frame dokrokuje pevným rozpočtem (event-driven model:
 * krokování řídí host I/O + per-frame doběh, ne uplynulý čas). Neaktivní = no-op
 * (ověřeno v test_inactive_after_init). */
void test_active_per_frame_steps(void)
{
    mz1p16_emu_set_active(true);

    uint64_t c0 = g_mz1p16_cpu.cycles;
    mz1p16_emu_on_screen_done();
    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16_cpu.cycles > c0,
        "aktivni plotter se per-frame nedokrokoval");
}

/* ================================================================
 * FÁZE B - mapování stavových signálů na PA (PIO cesta)
 * ================================================================ */

/* BUSY (P1.4) -> PA0 a STA (P1.7) -> PA1, PŘÍMO, jen když plotter aktivní.
 * Čteme přes reálné pioz80_read_byte (PIO cesta). Catch-up nesmí stav
 * přepsat - proto čas neposouváme (jádro zůstane na nastavené hodnotě P1). */
void test_status_bits_map_to_pa(void)
{
    mz1p16_emu_set_active(true);
    catch_up();

    /* Po set_active proběhl fast-forward (firmware nastavil P1). Vynutíme
     * známé úrovně P1 přímo a ověříme přímou polaritu na PA0/PA1.
     * Catch-up v pioz80 by jádro krokoval jen při uplynulém čase; čas
     * nehýbáme, takže P1 zůstává. */

    /* P1.4=1 (BUSY), P1.7=1 (STA high) -> PA0=1, PA1=1. */
    g_mz1p16_cpu.P1 = 0x90; /* bity 4 a 7 */
    uint8_t pa = pioz80_read_byte(PIOZ80_ADDR_DATA_A);
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x01) != 0, "P1.4=1 se neprojevil na PA0");
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x02) != 0, "P1.7=1 se neprojevil na PA1");

    /* P1.4=0 (ready), P1.7=0 -> PA0=0, PA1=0. */
    g_mz1p16_cpu.P1 = 0x00;
    pa = pioz80_read_byte(PIOZ80_ADDR_DATA_A);
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x01) == 0, "P1.4=0 se neprojevil na PA0");
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x02) == 0, "P1.7=0 se neprojevil na PA1");
}

/* Neaktivní plotter neřídí PA0/PA1 (zůstávají na klidové úrovni z LPT). */
void test_inactive_does_not_drive_status(void)
{
    g_mz1p16.active = false;
    g_mz1p16_cpu.P1 = 0x00; /* nemělo by mít vliv, plotter odpojený */
    uint8_t pa = pioz80_read_byte(PIOZ80_ADDR_DATA_A);
    /* Bez aktivního plotteru/centronics je PA0/PA1 daný raw LPT vstupem (=1). */
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x01) != 0,
        "neaktivni plotter nesmi prepisovat PA0 na 0");
    TEST_ASSERT_TRUE_MESSAGE((pa & 0x02) != 0,
        "neaktivni plotter nesmi prepisovat PA1 na 0");
}

/* /INT polarita pro MZ standard (plotter): STROBE PA7 je INVERTOVANÝ.
 * Aktivace plotteru nastaví standard MZ. PA7=0 (klid) -> INT=1 (ready);
 * PA7=1 (assert) -> INT=0 (aktivní strobe). */
void test_int_polarity_mz(void)
{
    mz1p16_emu_set_active(true);   /* aktivace vynutí standard MZ */
    catch_up();

    /* Klid: PA7=0 -> INT=1 (ready). */
    pioz80_write_byte(PIOZ80_ADDR_DATA_A, 0x00);   /* PA7=0 */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, g_mz1p16_cpu.INT,
        "PA7=0 melo nastavit INT=1 (MZ idle ready)");

    /* STROBE assert: PA7=1 -> INT=0, data z PB na P2. */
    pioz80_write_byte(PIOZ80_ADDR_DATA_B, 0x55);   /* nějaká data na PB */
    pioz80_write_byte(PIOZ80_ADDR_DATA_A, 0x80);   /* PA7=1 */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, g_mz1p16_cpu.INT,
        "PA7=1 melo nastavit INT=0 (MZ strobe assert)");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x55, g_mz1p16_cpu.P2_in,
        "data z PB se nedostala na P2");
}

/* ================================================================
 * FÁZE C - PLNÝ HANDSHAKE PŘES PIO CESTU s reálným plynutím času
 * ================================================================ */

/**
 * Pošle jeden bajt přes handshake (MZ standard) výhradně přes pioz80 API,
 * event-driven model. Krokování firmwaru řídí čtení stavu (drive_poll uvnitř
 * pioz80_read_byte) a synchronní příjem na hraně STROBE - ne uplynulý čas.
 *
 * MZ standard: STROBE PA7 invertovaný (PA7=0 -> INT=1 idle; PA7=1 -> INT=0
 * assert). Firmware v klidu po homingu = ready (P1.4=0 -> PA0=0). Po přijetí
 * bajtu (synchronní příjem na náběžné hraně STROBE = sestupné hraně INT) jde
 * krátce busy (P1.4=1 -> PA0=1) a po zpracování se vrátí do ready.
 *
 * Host strana (odpovídá ROM polling RDA):
 *  1) data na PB (DATA_B),
 *  2) STROBE assert: PA7 0->1 (INT 1->0) - firmware bajt synchronně přijme (busy),
 *  3) STROBE deassert: PA7 1->0 (INT 0->1) - firmware může bajt dozpracovat,
 *  4) poll PA0, dokud neklesne na 0 (firmware se vrátil do ready).
 *
 * @param byte       odesílaný bajt
 * @param max_iters  strop iterací poll smyčky (ochrana proti nekonečné smyčce)
 * @param saw_ready  [out] PA0 == 0 (ready) na konci zpracování?
 * @return počet iterací poll smyčky (>0), nebo -1 při vyčerpání stropu
 */
static long host_send_byte_pio(uint8_t byte, long max_iters, bool *saw_ready)
{
    bool ready_seen = false;
    long iters = 0;

    /* Klid: PA7=0 (INT=1 ready). Nech firmware vidět klid PŘED hranou. */
    pioz80_write_byte(PIOZ80_ADDR_DATA_A, 0x00);
    for (int k = 0; k < 8; k++)
        pioz80_read_byte(PIOZ80_ADDR_DATA_A);      /* drive_poll krokuje firmware */

    pioz80_write_byte(PIOZ80_ADDR_DATA_B, byte);   /* data na PB -> P2 */
    pioz80_write_byte(PIOZ80_ADDR_DATA_A, 0x80);   /* STROBE assert (PA7 0->1, busy) */
    pioz80_write_byte(PIOZ80_ADDR_DATA_A, 0x00);   /* STROBE deassert (PA7 1->0) */

    /* Poll PA0, dokud firmware bajt nedozpracuje a nevrátí se do ready (PA0=0). */
    for (; iters < max_iters; iters++) {
        if ((pioz80_read_byte(PIOZ80_ADDR_DATA_A) & 0x01) == 0) {
            ready_seen = true;
            iters++;
            break;
        }
    }

    if (saw_ready) *saw_ready = ready_seen;
    if (!ready_seen) return -1;                    /* zatuhnutí */
    return iters;
}

/**
 * Plný handshake: host pošle textový řetězec přes PIO cestu, každý znak s
 * RDA/BUSY poll smyčkou a plynoucím časem. Ověřuje, že:
 *   (1) PA0 (RDA/BUSY) během zpracování znaku klesne na 0 (ready puls),
 *   (2) poll smyčka pro každý znak SKONČÍ (žádné zatuhnutí),
 *   (3) data dorazí na P2 a plotter na řetězec mechanicky reaguje.
 */
void test_handshake_pio_path_completes(void)
{
    mz1p16_emu_set_active(true);
    catch_up();
    mz1p16_clear_drawing(&g_mz1p16);

    uint64_t x_before = g_mz1p16.x_steps_fwd + g_mz1p16.x_steps_rev;

    /* Každý poll (čtení PA0) firmware odkrokuje (drive_poll), takže se k
     * dozpracování bajtu dostane za rozumný počet iterací. Strop velký, ať
     * "nekonečno" odhalíme jako selhání. */
    const long MAX_ITERS = 2000000;

    const char *msg = "MZ1P16";
    long total_iters = 0;
    int chars = 0;
    bool any_ready = false;

    for (const char *p = msg; *p; p++, chars++) {
        bool saw_ready = false;
        long it = host_send_byte_pio((uint8_t)*p, MAX_ITERS, &saw_ready);
        fprintf(stderr,
            "[mz1p16_emu] char '%c': poll_iters=%ld saw_ready=%d P2_in=%02X "
            "P1.4=%d xpos=%ld n_strokes=%u\n",
            *p, it, (int)saw_ready, g_mz1p16_cpu.P2_in,
            (g_mz1p16_cpu.P1 & 0x10) ? 1 : 0,
            (long)g_mz1p16.sx.pos, g_mz1p16.n_strokes);

        TEST_ASSERT_TRUE_MESSAGE(it > 0,
            "poll smycka RDA zatuhla (vycerpala strop iteraci)");
        if (saw_ready) any_ready = true;
        total_iters += it;
    }

    /* (1) Alespoň u některého znaku PA0 kleslo na 0 (ready puls). */
    TEST_ASSERT_TRUE_MESSAGE(any_ready,
        "PA0 (RDA) behem handshaku nikdy nekleslo na 0 (zadny ready puls)");

    /* (2) Poslední bajt dorazil na P2. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)msg[chars - 1], g_mz1p16_cpu.P2_in,
        "posledni bajt se nedostal na P2 plotteru");

    /* (3) Řetězec vyvolal mechanickou odezvu (pohyb vozíku nebo kresba). */
    uint64_t x_after = g_mz1p16.x_steps_fwd + g_mz1p16.x_steps_rev;
    bool drew  = (g_mz1p16.n_strokes > 0);
    bool moved = (x_after > x_before);
    fprintf(stderr,
        "[mz1p16_emu] handshake summary: chars=%d total_poll_iters=%ld "
        "x_steps %llu->%llu n_strokes=%u\n",
        chars, total_iters,
        (unsigned long long)x_before, (unsigned long long)x_after,
        g_mz1p16.n_strokes);
    TEST_ASSERT_TRUE_MESSAGE(drew || moved,
        "prijate znaky nevyvolaly zadnou mechanickou odezvu (pohyb/kresba)");
}

/* ================================================================
 * Runner
 * ================================================================ */

/* Tlačítka okna Paper feed a self-test: zařadí se, provede je až
 * device_request_poll() (emu vlákno) a zvednou revizi stavu. */
void test_buttons_go_through_device_request(void)
{
    device_request_clear();
    mz1p16_emu_set_active(true);
    catch_up();
    TEST_ASSERT_EQUAL_UINT8(1, g_mz1p16_cpu.T1);

    uint64_t rev0 = g_state_rev;
    TEST_ASSERT_NOT_EQUAL_UINT32(0, mz1p16_request_paper_feed(true));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, g_mz1p16_cpu.T1, "UI vlákno T1 samo nemění");
    device_request_poll();
    TEST_ASSERT_EQUAL_UINT8(0, g_mz1p16_cpu.T1);
    TEST_ASSERT_TRUE(g_state_rev > rev0);

    rev0 = g_state_rev;
    uint64_t cycles = g_mz1p16_cpu.cycles;
    TEST_ASSERT_NOT_EQUAL_UINT32(0, mz1p16_request_selftest());
    TEST_ASSERT_EQUAL_UINT64(cycles, g_mz1p16_cpu.cycles);
    device_request_poll();
    TEST_ASSERT_TRUE(g_state_rev > rev0);
    TEST_ASSERT_TRUE_MESSAGE(mz1p16_emu_catchup_pending() > 0, "self-test se má dohánět po snímcích");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, g_mz1p16_cpu.T1, "během self-testu je PAPER FEED držen");
    catch_up();
    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16_cpu.cycles > cycles, "self-test neodkrokoval firmware");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, g_mz1p16_cpu.T1, "self-test uvolní PAPER FEED");
    TEST_ASSERT_TRUE_MESSAGE(g_mz1p16.n_strokes > 0, "self-test nic nenakreslil");
    device_request_clear();
}


/* Dohánění self-testu je rozložené do snímků: jeden per-frame krok
 * odkrokuje nejvýš omezený rozpočet (emulace stroje nestojí), celý self-test
 * přesto doběhne a nakreslí znakovou sadu. Reset ho zruší. */
void test_selftest_catchup_bounded_per_frame(void)
{
    mz1p16_emu_set_active(true);
    catch_up();

    mz1p16_emu_run_drawing_selftest();
    long pending0 = mz1p16_emu_catchup_pending();
    TEST_ASSERT_TRUE(pending0 > 10L * 1000 * 1000);
    uint64_t c0 = g_mz1p16_cpu.cycles;
    mz1p16_emu_on_screen_done();
    uint64_t step = g_mz1p16_cpu.cycles - c0;
    /* Strop 500 tisíc cyklů + přesah poslední instrukce. */
    TEST_ASSERT_TRUE_MESSAGE(step <= 500u * 1000u + 16u, "per-frame krok přesáhl rozpočet dohánění");
    TEST_ASSERT_TRUE(step >= 40u * 1000u);
    TEST_ASSERT_TRUE(mz1p16_emu_catchup_pending() < pending0);

    int frames = catch_up();
    TEST_ASSERT_TRUE_MESSAGE(frames >= 100, "self-test se dohnal příliš rychle (rozpočet za snímek?)");
    TEST_ASSERT_EQUAL_UINT8(1, g_mz1p16_cpu.T1);
    TEST_ASSERT_TRUE(g_mz1p16.n_strokes > 0);

    /* Reset během self-testu: dohánění homingu místo self-testu, PAPER FEED
     * uvolněný, emulace nestojí (reset sám nekrokuje). */
    mz1p16_emu_run_drawing_selftest();
    mz1p16_emu_on_screen_done();
    mz1p16_emu_reset();
    TEST_ASSERT_EQUAL_UINT8(1, g_mz1p16_cpu.T1);
    TEST_ASSERT_TRUE(mz1p16_emu_catchup_pending() > 0);
    TEST_ASSERT_TRUE(mz1p16_emu_catchup_pending() <= 3L * 1000 * 1000);
    TEST_ASSERT_TRUE(catch_up() <= 7);
    TEST_ASSERT_EQUAL_UINT8(1, g_mz1p16_cpu.T1);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_inactive_after_init);
    RUN_TEST(test_activation_runs_selftest);
    RUN_TEST(test_pulse_pen_change_rotates_color);
    RUN_TEST(test_active_per_frame_steps);
    RUN_TEST(test_status_bits_map_to_pa);
    RUN_TEST(test_inactive_does_not_drive_status);
    RUN_TEST(test_int_polarity_mz);
    RUN_TEST(test_handshake_pio_path_completes);
    RUN_TEST(test_buttons_go_through_device_request);
    RUN_TEST(test_selftest_catchup_bounded_per_frame);
    return UNITY_END();
}
