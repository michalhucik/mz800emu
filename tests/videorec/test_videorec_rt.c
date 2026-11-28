/**
 * @file   test_videorec_rt.c
 * @brief  Standalone testy režimu "podle reality": hodiny vzorkovače, fronta snímků, převzorkování a korekce driftu.
 *
 * Simulace běží ve virtuálním čase (us, u fronty snímků ns): producent (SDL
 * callback) přidává bloky 882 vzorků 44,1 kHz (u krystalu 881 / 882, NTSC
 * 735 / 736) v termínech podle hodin "zařízení" (s volitelným driftem
 * a jitterem), spotřebitel (vzorkovač) odebírá 960 vzorků 48 kHz za tik.
 * Fronta snímků se simuluje s konci emulovaných snímků s kolísáním, mřížkou
 * zvukového zařízení, posunem o celý snímek a driftem.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emulator/videorec/videorec_rt.h"

#define IN_RATE 44100u
#define OUT_RATE 48000u
#define CHUNK 882u   /**< Blok SDL cesty (20 ms při 44,1 kHz). */
#define OUT_TICK 960u /**< Výstup jednoho ticku při 48 kHz. */
#define PERIOD 20000 /**< Perioda ticku [us]. */

static st_VIDEOREC_RT_AUDIO RA;

void setUp(void)
{
    videorec_rt_audio_init(&RA, IN_RATE, OUT_RATE, VIDEOREC_RT_AUDIO_TARGET_MS, VIDEOREC_RT_AUDIO_CAPACITY_MS);
    videorec_rt_audio_reset(&RA, true, 0);
}

void tearDown(void)
{
    videorec_rt_audio_free(&RA);
}

/* ================================================================
 * Hodiny vzorkovače
 * ================================================================ */

/* První volání založí řadu a vydá tick 0; další ticky přesně po periodě. */
static void test_clock_first_tick_and_period(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    TEST_ASSERT_EQUAL_INT64(0, videorec_rt_clock_next_deadline(&c));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 1000000));
    TEST_ASSERT_EQUAL_INT64(1000000 + PERIOD, videorec_rt_clock_next_deadline(&c));
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 1000000 + PERIOD - 1));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 1000000 + PERIOD));
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 1000000 + PERIOD + 10));
}

/* Zpoždění do max_catchup se dohání; termíny zůstávají absolutní (bez driftu). */
static void test_clock_catchup_without_drift(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    (void)videorec_rt_clock_due(&c, 0);
    TEST_ASSERT_EQUAL_UINT(3, videorec_rt_clock_due(&c, 3 * PERIOD + 5));
    /* 10 000 ticků s pozdním probuzením o 7 ms: celkem přesně tolik ticků, kolik termínů uběhlo */
    uint64_t total = 4;
    for (int k = 4; k < 10000; k++) total += videorec_rt_clock_due(&c, (int64_t)k * PERIOD + 7000);
    TEST_ASSERT_EQUAL_UINT64(10000, total);
    TEST_ASSERT_EQUAL_UINT64(0, c.rebased);
}

/* Zpoždění nad max_catchup: nová řada od aktuálního času, vydá se 1 tick. */
static void test_clock_rebase_on_large_lag(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init(&c, PERIOD, 50);
    (void)videorec_rt_clock_due(&c, 0);
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 100 * PERIOD));
    TEST_ASSERT_EQUAL_UINT64(1, c.rebased);
    TEST_ASSERT_TRUE(c.lost_ticks >= 50);
    TEST_ASSERT_EQUAL_INT64(101 * PERIOD, videorec_rt_clock_next_deadline(&c));
}

/* 60 snímků/s (MZ-1500, MZ-700 NTSC): perioda 16 666,67 us bez zaokrouhlení - za 1 s přesně
 * 60 ticků, za 1 h přesně 216 000 (celočíselná perioda 16 666 us by dala o 13 ticků víc). */
static void test_clock_fractional_period_60fps(void)
{
    st_VIDEOREC_RT_CLOCK c;
    videorec_rt_clock_init_fps(&c, 60, 1, 60);
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 0));
    TEST_ASSERT_EQUAL_INT64(16667, videorec_rt_clock_next_deadline(&c)); /* ceil(1e6 / 60) */
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 16666));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 16667));
    TEST_ASSERT_EQUAL_INT64(33334, videorec_rt_clock_next_deadline(&c)); /* ceil(2e6 / 60) */
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 33334));
    /* tick 3 přesně v 50 000 us */
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_clock_due(&c, 49999));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_clock_due(&c, 50000));
    uint64_t total = 4;
    for (int64_t t = 50000 + 1000; t <= 3600LL * 1000000; t += 1000) total += videorec_rt_clock_due(&c, t);
    TEST_ASSERT_EQUAL_UINT64(216001, total); /* ticky 0 .. 216000 včetně (t = 3600 s) */
    TEST_ASSERT_EQUAL_UINT64(0, c.rebased);
}

/* 50 snímků/s přes videorec_rt_clock_init_fps() = stejné termíny jako perioda 20 000 us. */
static void test_clock_fps_50_equals_period(void)
{
    st_VIDEOREC_RT_CLOCK a, b;
    videorec_rt_clock_init_fps(&a, 50, 1, 50);
    videorec_rt_clock_init(&b, PERIOD, 50);
    for (int64_t t = 0; t < 2000000; t += 777) {
        TEST_ASSERT_EQUAL_UINT(videorec_rt_clock_due(&b, t), videorec_rt_clock_due(&a, t));
        TEST_ASSERT_EQUAL_INT64(videorec_rt_clock_next_deadline(&b), videorec_rt_clock_next_deadline(&a));
    }
}

/* Zpoždění obrazu je dané v ms (VIDEOREC_RT_VIDEO_DELAY_MS = 40) a na ticky se
 * zaokrouhluje podle fps: 50 i 60 snímků/s = 2 ticky (60: 2,4 -> 2), nejméně 1 tick. */
static void test_video_delay_ticks_from_ms(void)
{
    TEST_ASSERT_EQUAL_UINT(40u, VIDEOREC_RT_VIDEO_DELAY_MS);
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(50, 1));
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(60, 1));
    TEST_ASSERT_EQUAL_UINT(2u, videorec_rt_video_delay_ticks(60000, 1001)); /* 59,94 */
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(25, 1));      /* 1,0 */
    TEST_ASSERT_EQUAL_UINT(4u, videorec_rt_video_delay_ticks(100, 1));
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(1, 1));       /* 0,04 -> min. 1 */
    TEST_ASSERT_EQUAL_UINT(1u, videorec_rt_video_delay_ticks(0, 0));       /* 0 se nahradí 1 (jako u hodin) -> min. 1 */
}

/*
 * Blok SDL cesty = horní celá část VIDEOREC_RT_SDL_RATE / fps: 882 při 50, 735 při 60
 * snímcích/s; podle krystalu (krystal / takty snímku) delší z obou délek bloku: 882 (PAL,
 * bloky 881 a 882) a 736 (NTSC, bloky 735 a 736).
 */
static void test_sdl_chunk_per_fps(void)
{
    TEST_ASSERT_EQUAL_UINT(882u, videorec_rt_sdl_chunk(50, 1));
    TEST_ASSERT_EQUAL_UINT(735u, videorec_rt_sdl_chunk(60, 1));
    TEST_ASSERT_EQUAL_UINT(882u, videorec_rt_sdl_chunk(17734475u, 354432u));
    TEST_ASSERT_EQUAL_UINT(736u, videorec_rt_sdl_chunk(14318180u, 238944u));
    TEST_ASSERT_EQUAL_UINT(882u, VIDEOREC_RT_SDL_CHUNK_MAX);
    TEST_ASSERT_TRUE(videorec_rt_sdl_chunk(14318180u, 238944u) <= VIDEOREC_RT_SDL_CHUNK_MAX);
}

/*
 * Rozvrh podle krystalu (vzorkovač záznamu podle reality běží stejným tempem jako
 * emulace): zlomek periody se zkrátí (PAL 14 177 280 000 / 709 379 us, NTSC
 * 11 947 200 000 / 715 909 us) a za 1000 s je přesně floor(1000 * krystal / takty
 * snímku) + 1 ticků (tick 0 v čase 0): PAL 50 037, NTSC 59 923; po 100 dnech řada
 * nepřeteče a drží přesnou délku.
 */
static void test_clock_crystal_fps(void)
{
    static const struct { unsigned num, den; int64_t pnum, pden; uint64_t ticks_1000s; } tc[] = {
        { 17734475u, 354432u, 14177280000ll, 709379ll, 50037u },
        { 14318180u, 238944u, 11947200000ll, 715909ll, 59923u },
    };
    for (unsigned i = 0; i < 2; i++) {
        st_VIDEOREC_RT_CLOCK c;
        videorec_rt_clock_init_fps(&c, tc[i].num, tc[i].den, 5);
        TEST_ASSERT_EQUAL_INT64(tc[i].pnum, c.period_num);
        TEST_ASSERT_EQUAL_INT64(tc[i].pden, c.period_den);
        uint64_t total = 0;
        for (int64_t t = 0; t <= 1000ll * 1000000; t += 997) total += videorec_rt_clock_due(&c, t);
        total += videorec_rt_clock_due(&c, 1000ll * 1000000);
        TEST_ASSERT_EQUAL_UINT64(tc[i].ticks_1000s, total);
        TEST_ASSERT_EQUAL_UINT64(0, c.rebased);
        /* 100 dní: termín ticku je pořád přesný (ceil(k * num / den)) */
        const int64_t day100 = 100ll * 86400 * 1000000;
        uint64_t k = (uint64_t)((day100 * tc[i].pden) / tc[i].pnum);   /* ticky splatné do day100 - 1 */
        c.done = k;
        TEST_ASSERT_EQUAL_INT64((int64_t)(((unsigned __int128)k * (uint64_t)tc[i].pnum + (uint64_t)tc[i].pden - 1u)
                                          / (uint64_t)tc[i].pden),
                                videorec_rt_clock_next_deadline(&c) - c.base_us);
    }
}

/* ================================================================
 * Fronta emulovaných snímků
 * ================================================================ */

/** Počet položek uvolněných callbackem fronty (zahození, vyčištění). */
static unsigned s_released;

/** Callback uvolnění položky fronty: jen počítá. @param item Položka. */
static void fq_release(void *item)
{
    (void)item;
    s_released++;
}

/** Položka fronty s číslem snímku `id` (nenulový ukazatel). */
#define FQ_ITEM(id) ((void *)(uintptr_t)((id) + 1u))
/** Číslo snímku z položky fronty. */
#define FQ_ID(it) ((uint64_t)(uintptr_t)(it) - 1u)

/* Pořadí: položky vycházejí v pořadí vložení; prázdná fronta při DRAIN = poslední obraz. */
static void test_frameq_order_and_empty(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    s_released = 0;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_DRAIN, &it));
    TEST_ASSERT_NULL(it);
    for (unsigned i = 0; i < 5; i++) videorec_rt_frameq_push(&q, FQ_ITEM(i), 0);
    TEST_ASSERT_EQUAL_UINT(5, videorec_rt_frameq_len(&q));
    for (unsigned i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_DRAIN, &it));
        TEST_ASSERT_EQUAL_UINT64(i, FQ_ID(it));
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_DRAIN, &it));
    TEST_ASSERT_EQUAL_UINT(0, s_released);
    /* pop bez regulace (dopsání na konci úseku) a značka */
    videorec_rt_frameq_push(&q, FQ_ITEM(7), 1);
    uint8_t tag = 0;
    TEST_ASSERT_EQUAL_UINT64(7, FQ_ID(videorec_rt_frameq_pop(&q, &tag)));
    TEST_ASSERT_EQUAL_UINT8(1, tag);
    TEST_ASSERT_NULL(videorec_rt_frameq_pop(&q, NULL));
}

/* Přetečení kapacity: zahodí se nejstarší, zůstane posledních CAP snímků v pořadí. */
static void test_frameq_overflow_drops_oldest(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    s_released = 0;
    videorec_rt_frameq_init(&q, 50, fq_release);
    for (unsigned i = 0; i < VIDEOREC_RT_FRAMEQ_CAP + 5; i++) videorec_rt_frameq_push(&q, FQ_ITEM(i), 0);
    TEST_ASSERT_EQUAL_UINT(VIDEOREC_RT_FRAMEQ_CAP, videorec_rt_frameq_len(&q));
    TEST_ASSERT_EQUAL_UINT(5, s_released);
    TEST_ASSERT_EQUAL_UINT64(5, q.stats.overflow_drops);
    for (unsigned i = 5; i < VIDEOREC_RT_FRAMEQ_CAP + 5; i++) TEST_ASSERT_EQUAL_UINT64(i, FQ_ID(videorec_rt_frameq_pop(&q, NULL)));
    /* vyčištění uvolní zbytek a vrátí stav plnění */
    for (unsigned i = 0; i < 3; i++) videorec_rt_frameq_push(&q, FQ_ITEM(i), 0);
    q.starved = false;
    videorec_rt_frameq_clear(&q);
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_frameq_len(&q));
    TEST_ASSERT_EQUAL_UINT(8, s_released);
    TEST_ASSERT_TRUE(q.starved);
}

/* Odebrání podle značky zachová pořadí ostatních (snímky z běhu k dočasnému breakpointu). */
static void test_frameq_purge_tag(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    s_released = 0;
    videorec_rt_frameq_init(&q, 50, fq_release);
    /* kruh přes konec pole: nejdřív posunout head */
    for (unsigned i = 0; i < 6; i++) videorec_rt_frameq_push(&q, FQ_ITEM(100 + i), 0);
    for (unsigned i = 0; i < 6; i++) (void)videorec_rt_frameq_pop(&q, NULL);
    const uint8_t tags[] = { 0, 1, 1, 0, 1, 0, 0 };
    for (unsigned i = 0; i < sizeof(tags); i++) videorec_rt_frameq_push(&q, FQ_ITEM(i), tags[i]);
    TEST_ASSERT_EQUAL_UINT(3, videorec_rt_frameq_purge_tag(&q, 1));
    TEST_ASSERT_EQUAL_UINT(3, s_released);
    TEST_ASSERT_EQUAL_UINT(4, videorec_rt_frameq_len(&q));
    const uint64_t want[] = { 0, 3, 5, 6 };
    for (unsigned i = 0; i < 4; i++) TEST_ASSERT_EQUAL_UINT64(want[i], FQ_ID(videorec_rt_frameq_pop(&q, NULL)));
}

/*
 * Plnění v LIVE: do úrovně TARGET se opakuje obraz před frontou (PREV, prázdná fronta LATEST),
 * pak jeden snímek za tik. Vyprázdnění = podtečení, opakování a nové plnění.
 */
static void test_frameq_live_priming_and_underflow(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    uint64_t next = 0;
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    for (unsigned k = 1; k < VIDEOREC_RT_FRAMEQ_TARGET; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_PREV, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    }
    uint64_t expect = 0;
    for (unsigned k = 0; k < 20; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
        TEST_ASSERT_EQUAL_UINT64(expect++, FQ_ID(it));
    }
    TEST_ASSERT_FALSE(q.starved);
    /* emulace se zastaví: dobere rezervu (TARGET - 1), pak podtečení */
    for (unsigned k = 0; k + 1 < VIDEOREC_RT_FRAMEQ_TARGET; k++) {
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
        TEST_ASSERT_EQUAL_UINT64(expect++, FQ_ID(it));
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    TEST_ASSERT_EQUAL_UINT64(1, q.stats.underflows);
    TEST_ASSERT_TRUE(q.starved);
    /* emulace znovu: plnění na cíl, pak pokračuje v pořadí */
    videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
    next++;
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_PREV, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    for (unsigned k = 1; k < VIDEOREC_RT_FRAMEQ_TARGET; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    TEST_ASSERT_EQUAL_UINT64(expect, FQ_ID(it));
}

/*
 * Přesah při dokončení plnění po startu: mezi dvěma tiky přijdou dva snímky, plnění by skončilo
 * na TARGET + 1 a obraz by zůstal trvale o snímek později proti zvuku (naměřeno: posun zvuku
 * proti obrazu -39 místo -19 ms). Nejstarší přesah se zahodí (`prime_trims`), úroveň je pak
 * cíl. Po vyprázdnění (doběhnutí emulace) se přesah nezahazuje.
 */
static void test_frameq_prime_overshoot_trimmed(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    uint64_t next = 0;
    videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
    next++;
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_PREV, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    for (unsigned k = 0; k < VIDEOREC_RT_FRAMEQ_TARGET; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    TEST_ASSERT_EQUAL_UINT64(1, q.stats.prime_trims);
    TEST_ASSERT_EQUAL_UINT64(1, FQ_ID(it)); /* snímek 0 zahozen */
    TEST_ASSERT_EQUAL_UINT(VIDEOREC_RT_FRAMEQ_TARGET - 1, videorec_rt_frameq_len(&q));
    /* po vyprázdnění: doběhnutí emulace (přesah) se nezahazuje */
    while (videorec_rt_frameq_len(&q)) (void)videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it);
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    for (unsigned k = 0; k <= VIDEOREC_RT_FRAMEQ_TARGET; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
    TEST_ASSERT_EQUAL_UINT64(1, q.stats.prime_trims);
    videorec_rt_frameq_clear(&q);
}

/*
 * Pozdní probuzení vzorkovače (zaseknutí hostitele až o 1 s, max_catchup lepidla): mezitím
 * přibývají snímky a dohánějící ticky je odeberou v pořadí, nic se nezahodí ani neopakuje
 * (kapacita fronty 64 pokryje cíl + 50 ticků; dřívější kapacita 8 zahazovala od 6 ticků).
 */
static void test_frameq_late_wakeup_no_loss(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    s_released = 0;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    uint64_t next = 0, expect = 0;
    for (unsigned k = 0; k < 10; k++) { /* ustálený stav */
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
        if (videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it) == VIDEOREC_RT_TAKE_FRAME)
            TEST_ASSERT_EQUAL_UINT64(expect++, FQ_ID(it));
    }
    for (unsigned k = 0; k < 50; k++) { /* vzorkovač stojí 50 ticků */
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
    }
    for (unsigned k = 0; k < 50; k++) { /* dohánění: 50 ticků najednou */
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it));
        TEST_ASSERT_EQUAL_UINT64(expect++, FQ_ID(it));
    }
    TEST_ASSERT_EQUAL_UINT64(0, q.stats.overflow_drops);
    TEST_ASSERT_EQUAL_UINT(0, s_released);
    videorec_rt_frameq_clear(&q);
}

/* FREE (rychlost != 100 %): přebytek nad cíl se zahodí, prázdná fronta = poslední obraz bez plnění. */
static void test_frameq_free_and_drain_modes(void)
{
    st_VIDEOREC_RT_FRAMEQ q;
    s_released = 0;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    /* 200 %: dva snímky za tik */
    uint64_t next = 0, last = 0;
    for (unsigned k = 0; k < 50; k++) {
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
        videorec_rt_frameq_push(&q, FQ_ITEM(next), 0);
        next++;
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_FREE, &it));
        TEST_ASSERT_TRUE(FQ_ID(it) > last || k == 0);
        last = FQ_ID(it);
        TEST_ASSERT_TRUE(videorec_rt_frameq_len(&q) < VIDEOREC_RT_FRAMEQ_TARGET);
    }
    TEST_ASSERT_TRUE(q.stats.surplus_drops > 0);
    /* 50 %: snímek každý druhý tik - střídá se snímek a poslední obraz, žádné plnění */
    videorec_rt_frameq_clear(&q);
    unsigned frames = 0, latest = 0;
    for (unsigned k = 0; k < 20; k++) {
        if (k % 2 == 0) videorec_rt_frameq_push(&q, FQ_ITEM(k), 0);
        en_VIDEOREC_RT_TAKE t = videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_FREE, &it);
        if (t == VIDEOREC_RT_TAKE_FRAME) frames++;
        if (t == VIDEOREC_RT_TAKE_LATEST) latest++;
    }
    TEST_ASSERT_EQUAL_UINT(10, frames);
    TEST_ASSERT_EQUAL_UINT(10, latest);
    TEST_ASSERT_TRUE(q.starved); /* návrat na 100 % začne plněním */
    /* DRAIN (pauza se zápisem): zbytek fronty se dopíše bez zahazování */
    for (unsigned i = 0; i < 6; i++) videorec_rt_frameq_push(&q, FQ_ITEM(i), 0);
    for (unsigned i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_FRAME, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_DRAIN, &it));
        TEST_ASSERT_EQUAL_UINT64(i, FQ_ID(it));
    }
    TEST_ASSERT_EQUAL_INT(VIDEOREC_RT_TAKE_LATEST, videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_DRAIN, &it));
}

/**
 * Simulace FREE nad 100 %: za tik přijde `per_tick` zobrazených obrazů, každý sdílí
 * `frames_per_img` emulovaných snímků (emulace zobrazí jen jeden obraz za synchronizaci);
 * tiky na hraně dodání: každý `gap_every`. tik dostane dvojnásobek a další nic
 * (`double_first`, naměřený případ: 4 snímky, pak 0), nebo naopak.
 * @param double_first Pořadí dvojnásobku a prázdného tiku.
 * @param repeats_out Tiků, které zapsaly tentýž obraz jako minulý tik.
 * @param back_out    Tiků, které zapsaly starší obraz než minulý tik.
 * @return Počet tiků.
 */
static unsigned fq_free_shared(unsigned per_tick, unsigned frames_per_img, unsigned gap_every, bool double_first,
                               unsigned *repeats_out, unsigned *back_out)
{
    st_VIDEOREC_RT_FRAMEQ q;
    videorec_rt_frameq_init(&q, 50, fq_release);
    uint64_t img = 0, last = 0;
    bool have = false;
    unsigned repeats = 0, back = 0, ticks = 0;
    for (unsigned i = 0; i < 3; i++, img++)
        for (unsigned f = 0; f < frames_per_img; f++) videorec_rt_frameq_push(&q, FQ_ITEM(img), 0);
    for (unsigned j = 1; j <= 2000; j++) {
        unsigned first = double_first ? 2u * per_tick : 0u, second = double_first ? 0u : 2u * per_tick;
        unsigned n = (j % gap_every == 0) ? first : (j % gap_every == 1 && j > 1) ? second : per_tick;
        for (unsigned i = 0; i < n; i++, img++)
            for (unsigned f = 0; f < frames_per_img; f++) videorec_rt_frameq_push(&q, FQ_ITEM(img), 0);
        void *it = NULL;
        en_VIDEOREC_RT_TAKE r = videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_FREE, &it);
        uint64_t id = (r == VIDEOREC_RT_TAKE_FRAME) ? FQ_ID(it) : last; /* LATEST/PREV = beze změny */
        if (have && id == last) repeats++;
        if (have && id < last) back++;
        last = id;
        have = true;
        ticks++;
    }
    videorec_rt_frameq_clear(&q);
    *repeats_out = repeats;
    *back_out = back;
    return ticks;
}

/*
 * Rychlost nad 100 % (FREE): naměřeno při 200 %, že emulace zobrazí jeden obraz za
 * synchronizaci, takže dva emulované snímky za sebou ve frontě sdílejí tentýž obraz.
 * Odběr po položkách pak podle fáze tiků bral tentýž obraz dvakrát za sebou (ve skutečném
 * běhu 73 až 83 opakování za 10 s, 0677e23b se slotem "poslední obraz" 7 až 16). Po
 * sloučení duplicit se při stálém přísunu (i s tiky na hraně dodání: 0 a pak 2 obrazy)
 * žádný obraz nezopakuje ani nevrátí.
 */
static void test_frameq_free_shared_images_no_repeat(void)
{
    const unsigned cases[][3] = { { 1, 2, 7 }, { 1, 2, 3 }, { 3, 2, 5 }, { 2, 4, 9 } };
    for (unsigned c = 0; c < 4; c++) {
        for (unsigned order = 0; order < 2; order++) {
            unsigned rep = 0, back = 0;
            unsigned t = fq_free_shared(cases[c][0], cases[c][1], cases[c][2], order == 0, &rep, &back);
            printf("FREE %u images/tick, %u frames/image, every %u ticks %s: %u ticks, %u repeated, %u backwards\n",
                   cases[c][0], cases[c][1], cases[c][2], order == 0 ? "double then none" : "none then double", t,
                   rep, back);
            TEST_ASSERT_EQUAL_UINT(0, rep);
            TEST_ASSERT_EQUAL_UINT(0, back);
        }
    }
    /* pozice značek ve FREE počítá různé obrazy: 2 obrazy po 2 snímcích = obraz na obrazovce
     * vezme až druhý tik */
    st_VIDEOREC_RT_FRAMEQ q;
    videorec_rt_frameq_init(&q, 50, fq_release);
    void *it = NULL;
    (void)videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_FREE, &it);
    for (unsigned i = 0; i < 2; i++)
        for (unsigned f = 0; f < 2; f++) videorec_rt_frameq_push(&q, FQ_ITEM(i), 0);
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_frameq_pos_offset(&q));
    TEST_ASSERT_EQUAL_UINT(2, videorec_rt_frameq_merge_dups(&q));
    TEST_ASSERT_EQUAL_UINT(2, videorec_rt_frameq_len(&q));
    TEST_ASSERT_EQUAL_UINT(1, videorec_rt_frameq_pos_offset(&q));
    videorec_rt_frameq_clear(&q);
}

/** Výsledek simulace fronty snímků proti vzorkovači. */
typedef struct {
    uint64_t frames;   /**< Vložených snímků. */
    uint64_t ticks;    /**< Ticků vzorkovače. */
    uint64_t taken;    /**< Snímků zapsaných tiky. */
    uint64_t repeats;  /**< Ticků, které nezapsaly další snímek (opakování po prvním snímku). */
    uint64_t skipped;  /**< Snímků, které tiky přeskočily (zahozené). */
    uint64_t max_rep_run;  /**< Nejdelší řada opakování za sebou po prvních 5 s (náběh se nepočítá). */
    uint64_t multi_skips;  /**< Kolikrát tik přeskočil víc než 1 snímek najednou. */
    uint64_t slow_ticks;   /**< Ticků v pomalém režimu (`q.slow` po odběru). */
    uint64_t high_ticks;   /**< Ticků s úrovní před odběrem aspoň TARGET + SURPLUS (obraz o 2 a víc snímků později). */
    uint64_t marks;        /**< Kontrolovaných pozic značek (každý tik po 5 s, kdy už je nějaký snímek). */
    uint64_t marks_off;    /**< Z nich pozic, které neukazují na tik, kde se nejnovější snímek poprvé zapíše. */
} st_FQSIM;

/** Zaseknutí emulace v simulaci: snímky dokončené v <at_s, at_s + dur_s) se dokončí až na jeho konci (dohnání). */
typedef struct {
    double at_s;  /**< Začátek [s]. */
    double dur_s; /**< Délka [s]. */
} st_FQSTALL;

/**
 * Deterministické pseudonáhodné kolísání snímku `k` (hash, xorshift32).
 * @param seed Semínko. @param k Číslo snímku.
 * @return Hodnota rovnoměrně v <-1, 1>.
 */
static double fq_noise(uint32_t seed, uint64_t k)
{
    uint32_t x = seed ^ (uint32_t)(k * 2654435761u) ^ 0x9E3779B9u;
    for (int i = 0; i < 3; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return (double)(x % 2001u) / 1000.0 - 1.0;
}

static st_FQSIM fq_simulate_ex(double seconds, double frame_ns, double tick_ns, double phase_ns, double jitter_ns,
                               double grid_ns, double step_s, uint32_t seed, const st_FQSTALL *stalls,
                               unsigned n_stalls);

/**
 * Simulace konců emulovaných snímků a ticků vzorkovače (čas v ns, `seconds` s).
 *
 * Snímek k se dokončí v čase `phase + k * frame_ns + kolísání`, kde kolísání je
 * rovnoměrné v +-`jitter_ns`; s `grid_ns` > 0 se čas zaokrouhlí nahoru na mřížku
 * (perioda zvukového zařízení, kterou emulace dokončuje snímky); s `step_s` > 0 se
 * dokončování po každých `step_s` sekundách střídavě posune o celý snímek později
 * a zpět (posun latence se zvukovým zařízením). Drift: `frame_ns` vs `tick_ns`.
 * Tik j v čase `j * tick_ns` vezme z fronty (LIVE) a zkontroluje pořadí.
 */
static st_FQSIM fq_simulate(double seconds, double frame_ns, double tick_ns, double phase_ns, double jitter_ns,
                            double grid_ns, double step_s, uint32_t seed)
{
    return fq_simulate_ex(seconds, frame_ns, tick_ns, phase_ns, jitter_ns, grid_ns, step_s, seed, NULL, 0);
}

/**
 * fq_simulate() se zaseknutími emulace (`stalls`, `n_stalls`; snímky ze zaseknutí se
 * dokončí najednou na jeho konci - emulace zaostání dohání). Navíc kontroluje pozici
 * značek: v každém tiku po 5 s se před odběrem vezme `j + videorec_rt_frameq_pos_offset()`
 * a porovná s tikem, kdy se nejnovější vložený snímek poprvé zapíše (FRAME s jeho id,
 * LATEST = nejnovější, PREV = předchozí výstup).
 */
static st_FQSIM fq_simulate_ex(double seconds, double frame_ns, double tick_ns, double phase_ns, double jitter_ns,
                               double grid_ns, double step_s, uint32_t seed, const st_FQSTALL *stalls,
                               unsigned n_stalls)
{
    st_VIDEOREC_RT_FRAMEQ q;
    videorec_rt_frameq_init(&q, 50, fq_release);
    st_FQSIM s;
    memset(&s, 0, sizeof(s));
    const double end = seconds * 1e9;
    double last_t = 0.0;
    uint64_t k = 0;
    bool have = false;
    uint64_t last_id = 0, run = 0;
    const uint64_t n_ticks = (uint64_t)(end / tick_ns) + 2u;
    uint64_t *out = calloc(n_ticks, sizeof(*out));   /* id + 1 obrazu zapsaného v tiku j */
    uint64_t *mpos = calloc(n_ticks, sizeof(*mpos)); /* předpověď pozice značky z tiku j */
    uint64_t *mlat = calloc(n_ticks, sizeof(*mlat)); /* id + 1 nejnovějšího snímku v tiku j (0 = nekontroluje se) */
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(mpos);
    TEST_ASSERT_NOT_NULL(mlat);
    uint64_t prev_out = 0;
    for (uint64_t j = 0;; j++) {
        double tick_t = (double)j * tick_ns;
        if (tick_t > end || j >= n_ticks) break;
        for (;;) {
            /* čas dokončení snímku k (deterministický, nezávislý na počtu průchodů) */
            double t = phase_ns + (double)k * frame_ns + fq_noise(seed, k) * jitter_ns;
            if (step_s > 0.0 && ((uint64_t)(t / (step_s * 1e9)) & 1u)) t += frame_ns;
            if (grid_ns > 0.0) t = ceil(t / grid_ns) * grid_ns;
            for (unsigned si = 0; si < n_stalls; si++) {
                double s0 = stalls[si].at_s * 1e9, s1 = s0 + stalls[si].dur_s * 1e9;
                if (t >= s0 && t < s1) t = s1;
            }
            if (t < last_t) t = last_t;
            if (t > tick_t) break;
            last_t = t;
            videorec_rt_frameq_push(&q, FQ_ITEM(k), 0);
            k++;
            s.frames++;
        }
        if (q.len >= VIDEOREC_RT_FRAMEQ_TARGET + VIDEOREC_RT_FRAMEQ_SURPLUS) s.high_ticks++;
        if (k > 0 && tick_t > 5e9) {
            mpos[j] = j + videorec_rt_frameq_pos_offset(&q);
            mlat[j] = k; /* id nejnovějšího + 1 */
        }
        void *it = NULL;
        en_VIDEOREC_RT_TAKE r = videorec_rt_frameq_take(&q, VIDEOREC_RT_FRAMEQ_LIVE, &it);
        s.ticks++;
        if (q.slow) s.slow_ticks++;
        uint64_t o = (r == VIDEOREC_RT_TAKE_FRAME) ? FQ_ID(it) + 1u : (r == VIDEOREC_RT_TAKE_LATEST) ? k : prev_out;
        out[j] = o;
        prev_out = o;
        if (r == VIDEOREC_RT_TAKE_FRAME) {
            uint64_t id = FQ_ID(it);
            if (have && id > last_id + 1) s.skipped += id - last_id - 1;
            if (have && id > last_id + 2) s.multi_skips++;
            last_id = id;
            have = true;
            s.taken++;
            run = 0;
        } else if (have) {
            s.repeats++;
            run++;
            if (tick_t > 5e9 && run > s.max_rep_run) s.max_rep_run = run;
        }
    }
    for (uint64_t j = 0; j < s.ticks; j++) {
        if (!mlat[j]) continue;
        uint64_t t = j;
        while (t < s.ticks && out[t] != mlat[j]) t++;
        if (t >= s.ticks) continue; /* snímek se do konce simulace nezapsal (zahozen) */
        s.marks++;
        if (mpos[j] != t) s.marks_off++;
    }
    free(out);
    free(mpos);
    free(mlat);
    videorec_rt_frameq_clear(&q);
    return s;
}

/** Délka snímku MZ-800 (PAL) podle krystalu [ns]: 354 432 / 17 734 475 s. */
#define FRAME_PAL_NS (1e9 * 354432.0 / 17734475.0)
/** Délka snímku MZ-1500 (NTSC) podle krystalu [ns]: 238 944 / 14 318 180 s. */
#define FRAME_NTSC_NS (1e9 * 238944.0 / 14318180.0)

/*
 * Každý emulovaný snímek právě jednou: vzorkovač tiká tempem emulace (krystal / takty
 * snímku) a bere další snímek z fronty. Kolísání dokončení snímku proti tiku nehraje roli
 * při žádné fázi - i když konec snímku padne přímo na tik (fáze 0) a kolísá +-2 ms
 * (naměřeno headless +-1,8 ms), nebo se snímky dokončují v periodě zvukového zařízení
 * 10 ms (kolísání plánovače +-2 ms před zaokrouhlením na periodu, výsledek kolísá +-10 ms
 * jako naměřené) a s posunem o celý snímek (naměřeno se zařízením). Dřívější
 * vzorkovač "poslední zobrazený snímek" v takové situaci opakoval a vynechával po desítkách.
 */
static void test_frameq_every_frame_once_with_jitter(void)
{
    const double phases[] = { 0.0, 0.1, 0.5, 0.9, 0.999 };
    for (unsigned p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
        for (unsigned sys = 0; sys < 2; sys++) {
            const double fns = sys ? FRAME_NTSC_NS : FRAME_PAL_NS;
            /* headless: kolísání +-2 ms */
            st_FQSIM a = fq_simulate(120.0, fns, fns, phases[p] * fns, 2e6, 0.0, 0.0, 1234u + p);
            /* se zařízením: kolísání +-2 ms, mřížka 10 ms, posun o snímek každých 7 s */
            st_FQSIM b = fq_simulate(120.0, fns, fns, phases[p] * fns, 2e6, 10e6, 7.0, 99u + p);
            printf("phase %.3f %s: headless %llu frames, %llu taken, %llu repeated, %llu skipped; device %llu frames, "
                   "%llu taken, %llu repeated, %llu skipped\n", phases[p], sys ? "NTSC" : "PAL",
                   (unsigned long long)a.frames, (unsigned long long)a.taken, (unsigned long long)a.repeats,
                   (unsigned long long)a.skipped, (unsigned long long)b.frames, (unsigned long long)b.taken,
                   (unsigned long long)b.repeats, (unsigned long long)b.skipped);
            TEST_ASSERT_TRUE(a.taken > 5900);
            TEST_ASSERT_EQUAL_UINT64(0, a.repeats);
            TEST_ASSERT_EQUAL_UINT64(0, a.skipped);
            TEST_ASSERT_TRUE(b.taken > 5900);
            TEST_ASSERT_EQUAL_UINT64(0, b.repeats);
            TEST_ASSERT_EQUAL_UINT64(0, b.skipped);
        }
    }
}

/*
 * Rozvrh musí mít tempo emulace: s jmenovitými 50 ticky za sekundu (emulace podle krystalu
 * 50,036) přibývá ve frontě snímek za 27,5 s a regulace ho musí zahodit - za 300 s
 * asi 10 vynechaných snímků. S tikem podle krystalu žádný.
 */
static void test_frameq_nominal_schedule_drops(void)
{
    st_FQSIM n = fq_simulate(300.0, FRAME_PAL_NS, 20e6, 0.5 * FRAME_PAL_NS, 1e6, 0.0, 0.0, 7u);
    st_FQSIM c = fq_simulate(300.0, FRAME_PAL_NS, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 1e6, 0.0, 0.0, 7u);
    printf("nominal 50 Hz: %llu skipped, %llu repeated; crystal: %llu skipped, %llu repeated\n",
           (unsigned long long)n.skipped, (unsigned long long)n.repeats, (unsigned long long)c.skipped,
           (unsigned long long)c.repeats);
    TEST_ASSERT_TRUE(n.skipped >= 5);
    TEST_ASSERT_EQUAL_UINT64(0, c.skipped);
    TEST_ASSERT_EQUAL_UINT64(0, c.repeats);
}

/*
 * Skutečný rozchod tempa (hodiny zvukového zařízení +-0,1 % proti systémovým): opravuje se
 * jen tím směrem, kterým se tempo rozchází (zařízení rychlejší = zahození trvalého
 * přebytku, pomalejší = opakování při vyprázdnění fronty a nové naplnění, po 2 až 3
 * snímcích), za 300 s asi 15 snímků a nikdy dvojice opakování a zahození.
 */
static void test_frameq_drift_corrections(void)
{
    st_FQSIM fast = fq_simulate(300.0, FRAME_PAL_NS / 1.001, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 5u);
    st_FQSIM slow = fq_simulate(300.0, FRAME_PAL_NS * 1.001, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 5u);
    printf("drift +0.1 %%: %llu skipped, %llu repeated; -0.1 %%: %llu skipped, %llu repeated\n",
           (unsigned long long)fast.skipped, (unsigned long long)fast.repeats, (unsigned long long)slow.skipped,
           (unsigned long long)slow.repeats);
    TEST_ASSERT_EQUAL_UINT64(0, fast.repeats);
    TEST_ASSERT_TRUE(fast.skipped >= 10 && fast.skipped <= 18);
    TEST_ASSERT_EQUAL_UINT64(0, slow.skipped);
    TEST_ASSERT_TRUE(slow.repeats >= 10 && slow.repeats <= 18);
}

/*
 * Pomalý počítač (nastaveno 100 %, emulace stíhá jen 99, 95 nebo 80 %): po druhém
 * vyprázdnění fronty do 10 s se plní jen na 1 snímek, takže se po náběhu opakuje vždy
 * jen jeden snímek a rovnoměrně (jako dřívější vzorkovač "poslední obraz"), ne po dávkách
 * 3 snímků do naplnění rezervy. Žádný snímek se nevynechá; opakování odpovídají deficitu.
 */
static void test_frameq_slow_machine_even_repeats(void)
{
    const double rates[] = { 0.99, 0.95, 0.80 };
    for (unsigned i = 0; i < 3; i++) {
        st_FQSIM s = fq_simulate(120.0, FRAME_PAL_NS / rates[i], FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 21u);
        double deficit = (double)s.ticks * (1.0 - rates[i]);
        printf("slow %.0f %%: %llu ticks, %llu taken, %llu repeated (deficit %.0f), longest run after 5 s %llu, "
               "%llu skipped, %llu slow ticks\n", rates[i] * 100.0, (unsigned long long)s.ticks,
               (unsigned long long)s.taken, (unsigned long long)s.repeats, deficit, (unsigned long long)s.max_rep_run,
               (unsigned long long)s.skipped, (unsigned long long)s.slow_ticks);
        TEST_ASSERT_EQUAL_UINT64(0, s.skipped);
        TEST_ASSERT_TRUE(s.max_rep_run <= 1);
        TEST_ASSERT_DOUBLE_WITHIN(deficit * 0.1 + 4.0, deficit, (double)s.repeats);
        TEST_ASSERT_TRUE(s.slow_ticks > s.ticks * 9 / 10);
    }
}

/*
 * Značky v pomalém režimu: pozice (videorec_rt_frameq_pos_offset()) má ukazovat na tik, ve
 * kterém se právě zobrazený (nejnovější) snímek poprvé zapíše. Pomalý režim plní jen na
 * 1 snímek, takže rezerva plnění (2) neplatí - dřív ležela většina značek při 95 a 80 %
 * o 1 až 2 snímky později. Odchylka je přípustná jen výjimečně (kolísání dokončení snímku
 * těsně kolem tiku): nejvýš 3 % značek. V ustáleném živém běhu sedí všechny.
 */
static void test_frameq_slow_machine_marker_position(void)
{
    const double rates[] = { 0.95, 0.80 };
    for (unsigned i = 0; i < 2; i++) {
        st_FQSIM s = fq_simulate(120.0, FRAME_PAL_NS / rates[i], FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 31u);
        printf("slow %.0f %% markers: %llu of %llu off\n", rates[i] * 100.0, (unsigned long long)s.marks_off,
               (unsigned long long)s.marks);
        TEST_ASSERT_TRUE(s.marks > 5000);
        TEST_ASSERT_TRUE(s.marks_off * 100 <= s.marks * 3);
    }
    st_FQSIM n = fq_simulate(120.0, FRAME_PAL_NS, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 31u);
    printf("live markers: %llu of %llu off\n", (unsigned long long)n.marks_off, (unsigned long long)n.marks);
    TEST_ASSERT_EQUAL_UINT64(0, n.marks_off);
}

/*
 * Zdravý počítač se dvěma ojedinělými zaseknutími po 100 ms 6 s po sobě (emulace pak
 * zaostání dožene): pomalý režim se nezapne (fronta se po zaseknutí přeplní, to pomalý
 * počítač nedělá) a přebytek se vybije běžnou regulací do 2 oken po každém zaseknutí.
 * Dřív obě vyprázdnění pomalý režim zapnula a přebytek zůstal ve frontě po celých 10 s
 * pomalého režimu (obraz o 2 a víc snímků později proti zvuku po stovky ticků).
 * Pomalý počítač (95 %), který jednou zasekne na 300 ms a dožene: přeplnění pomalý režim
 * hned ukončí a přebytek se vybije stejně; pak se pomalý režim zase zapne.
 */
static void test_frameq_isolated_stalls_not_slow(void)
{
    const unsigned high = VIDEOREC_RT_FRAMEQ_TARGET + VIDEOREC_RT_FRAMEQ_SURPLUS;
    const st_FQSTALL two[] = { { 20.0, 0.1 }, { 26.0, 0.1 } };
    st_FQSIM h = fq_simulate_ex(60.0, FRAME_PAL_NS, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 41u, two, 2);
    printf("healthy, 2 stalls of 100 ms: %llu repeated, %llu skipped, %llu slow ticks, %llu ticks at level >= %u\n",
           (unsigned long long)h.repeats, (unsigned long long)h.skipped, (unsigned long long)h.slow_ticks,
           (unsigned long long)h.high_ticks, high);
    TEST_ASSERT_EQUAL_UINT64(0, h.slow_ticks);
    TEST_ASSERT_TRUE(h.high_ticks > 0);
    TEST_ASSERT_TRUE(h.high_ticks <= 2u * 2u * 50u);

    const st_FQSTALL one[] = { { 30.0, 0.3 } };
    st_FQSIM sl = fq_simulate_ex(60.0, FRAME_PAL_NS / 0.95, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 42u,
                                 one, 1);
    printf("slow 95 %% + stall 300 ms: %llu repeated, %llu skipped, %llu slow ticks, %llu ticks at level >= %u\n",
           (unsigned long long)sl.repeats, (unsigned long long)sl.skipped, (unsigned long long)sl.slow_ticks,
           (unsigned long long)sl.high_ticks, high);
    TEST_ASSERT_TRUE(sl.high_ticks > 0);
    TEST_ASSERT_TRUE(sl.high_ticks <= 2u * 50u);
    TEST_ASSERT_TRUE(sl.slow_ticks > sl.ticks / 2);
}

/*
 * Zkrácené snímky (CKSW na MZ-800, snímek asi o 1/70 kratší) přicházejí rychleji než tiky:
 * přebytek se vybíjí po jednom snímku (nikdy 2 a víc najednou), bez opakování, a úroveň
 * fronty (zpoždění obrazu) zůstává omezená.
 */
static void test_frameq_short_frames_drop_singly(void)
{
    st_FQSIM s = fq_simulate(120.0, FRAME_PAL_NS * 69.0 / 70.0, FRAME_PAL_NS, 0.5 * FRAME_PAL_NS, 2e6, 0.0, 0.0, 22u);
    double surplus = (double)s.ticks / 69.0;
    printf("short frames 1/70: %llu frames, %llu taken, %llu skipped (surplus %.0f), %llu multi-skips, "
           "%llu repeated\n", (unsigned long long)s.frames, (unsigned long long)s.taken,
           (unsigned long long)s.skipped, surplus, (unsigned long long)s.multi_skips,
           (unsigned long long)s.repeats);
    TEST_ASSERT_EQUAL_UINT64(0, s.multi_skips);
    TEST_ASSERT_EQUAL_UINT64(0, s.repeats);
    TEST_ASSERT_DOUBLE_WITHIN(surplus * 0.1 + 4.0, surplus, (double)s.skipped);
}

/* ================================================================
 * Zvukový buffer - pomocné funkce simulace
 * ================================================================ */

/** @brief Výsledek simulace. */
typedef struct {
    uint64_t out_frames;   /**< Vydaných výstupních vzorků celkem. */
    size_t min_fill;       /**< Minimální naplnění po prvním naplnění. */
    size_t max_fill;       /**< Maximální naplnění po prvním naplnění. */
    double sum_fill;       /**< Součet naplnění (pro průměr). */
    unsigned n_fill;       /**< Počet měření naplnění. */
    double sum_corr;       /**< Součet korekce v poslední třetině simulace. */
    double min_corr;       /**< Minimální korekce v poslední třetině. */
    double max_corr;       /**< Maximální korekce v poslední třetině. */
    unsigned n_corr;       /**< Počet měření korekce. */
    size_t late_min_fill;  /**< Minimální naplnění v poslední třetině. */
    size_t late_max_fill;  /**< Maximální naplnění v poslední třetině. */
} st_SIM;

/** Producent: konstantní hodnota (pro testy driftu na obsahu nezáleží). */
static void push_chunk(float v)
{
    float buf[CHUNK * 2];
    for (unsigned i = 0; i < CHUNK * 2; i++) buf[i] = v;
    videorec_rt_audio_push(&RA, buf, CHUNK);
}

/**
 * Simulace: producent s hodinami zařízení posunutými o `drift` (relativně,
 * kladný = zařízení rychlejší), bloky v dávkách po `burst` (jitter), spotřebitel
 * každý snímek (20 ms při 50 fps) monotónních hodin.
 */
static st_SIM simulate(double seconds, double drift, unsigned burst)
{
    st_SIM s;
    memset(&s, 0, sizeof(s));
    s.min_fill = (size_t)-1;
    s.late_min_fill = (size_t)-1;
    s.min_corr = 1.0;
    s.max_corr = -1.0;
    int16_t out[OUT_TICK * 2];
    double prod_period = (double)PERIOD * burst / (1.0 + drift);
    double next_prod = 0.0;
    int64_t end = (int64_t)(seconds * 1e6);
    bool primed = false;
    for (int64_t t = 0; t < end; t += PERIOD) {
        /* producent: všechny dávky s termínem <= t (zařízení si říká o data po dávkách) */
        while (next_prod <= (double)t) {
            for (unsigned b = 0; b < burst; b++) push_chunk(0.5f);
            next_prod += prod_period;
        }
        /* naplnění před odběrem (to regulátor drží na cíli) */
        size_t f = videorec_rt_audio_fill(&RA);
        if (primed) {
            if (f < s.min_fill) s.min_fill = f;
            if (f > s.max_fill) s.max_fill = f;
            s.sum_fill += (double)f;
            s.n_fill++;
        }
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
        s.out_frames += OUT_TICK;
        if (!RA.starved) primed = true;
        if (t >= end * 2 / 3) {
            double c = RA.stats.corr;
            s.sum_corr += c;
            s.n_corr++;
            if (c < s.min_corr) s.min_corr = c;
            if (c > s.max_corr) s.max_corr = c;
            if (f < s.late_min_fill) s.late_min_fill = f;
            if (f > s.late_max_fill) s.late_max_fill = f;
        }
    }
    return s;
}

/* ================================================================
 * Zvukový buffer - testy
 * ================================================================ */

/* Bez driftu: po naplnění žádné podtečení ani přetečení, naplnění kolem cíle. */
static void test_audio_steady_no_drift(void)
{
    st_SIM s = simulate(60.0, 0.0, 1);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(60 * 50 * OUT_TICK, s.out_frames);
    TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
    TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
    double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
    double avg = s.sum_fill / s.n_fill;
    TEST_ASSERT_DOUBLE_WITHIN(CHUNK, target, avg);
    /* ticho jen při počátečním plnění (cíl 3 bloky = 2 ticky čekání) */
    TEST_ASSERT_TRUE(st.silence <= 3 * OUT_TICK);
}

/**
 * Drift hodin zařízení +-0,2 % (řádově víc než reálná zařízení): regulátor ho
 * dorovná, naplnění zůstane omezené kolem cíle a nedojde k podtečení ani
 * přetečení - zvuk proti obrazu nedriftuje.
 */
static void test_audio_drift_corrected(void)
{
    const double drifts[] = { +0.002, -0.002 };
    for (unsigned i = 0; i < 2; i++) {
        videorec_rt_audio_reset(&RA, true, 0);
        st_SIM s = simulate(300.0, drifts[i], 1);
        st_VIDEOREC_RT_AUDIO_STATS st;
        videorec_rt_audio_get_stats(&RA, &st);
        TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
        TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
        /* průměrná korekce v ustáleném stavu odpovídá driftu */
        double mean_corr = s.sum_corr / s.n_corr;
        printf("drift %+.4f: mean corr %+.5f (min %+.5f, max %+.5f), late fill %u..%u, max fill %u\n", drifts[i],
               mean_corr, s.min_corr, s.max_corr, (unsigned)s.late_min_fill, (unsigned)s.late_max_fill,
               (unsigned)s.max_fill);
        TEST_ASSERT_DOUBLE_WITHIN(0.0002, drifts[i], mean_corr);
        /* kolísání korekce (výška tónu) v ustáleném stavu nejvýš 0,3 % špička-špička */
        TEST_ASSERT_TRUE(s.max_corr - s.min_corr < 0.003);
        double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
        /* P regulátor má trvalou odchylku = drift * in_rate * TC (882 vzorků = 20 ms při 0,2 %);
         * naplnění zůstává omezené - zvuk proti obrazu nedriftuje */
        TEST_ASSERT_DOUBLE_WITHIN(2.0 * CHUNK, target, st.fill_ema);
        TEST_ASSERT_TRUE(s.late_max_fill < target + 3 * CHUNK);
        TEST_ASSERT_TRUE(s.late_min_fill > CHUNK + 2);
    }
}

/**
 * Jedna simulace záznamu podle reality s krystalem: producent (SDL cesta) dává
 * za každý emulovaný snímek blok `lo` nebo `lo + 1` vzorků 44,1 kHz
 * (akumulátor jako iface_audio_block_clock_next()), vzorkovač tiká stejným
 * tempem (krystal / takty snímku, fáze půl snímku) a za tik odebírá
 * `out_tick` vzorků 48 kHz (zvuk snímku AVI 50/1 nebo 60/1) s měřítkem ticku
 * `scale` (videorec_rt_audio_set_tick_scale()). Za 300 s: bez podtečení
 * a přetečení, trvalá korekce driftu `want_corr` (do 0,0002), rozkmit korekce
 * pod 0,3 %, naplnění kolem cíle (`fill_tol` vzorků). Vrací průměrné naplnění
 * v poslední třetině.
 */
static double run_crystal_blocks(uint64_t crystal, uint64_t frame_ticks, unsigned out_tick, size_t lo, double scale,
                                 double want_corr, double fill_tol)
{
    videorec_rt_audio_reset(&RA, true, 0);
    videorec_rt_audio_set_tick_scale(&RA, scale);
    uint64_t rem = 0;
    uint64_t frames = 0;
    unsigned n_lo = 0, n_hi = 0;
    static int16_t out[OUT_TICK * 2];
    static float buf[CHUNK * 2];
    for (unsigned i = 0; i < CHUNK * 2; i++) buf[i] = 0.5f;
    double sum_corr = 0.0, min_corr = 1.0, max_corr = -1.0, sum_fill = 0.0;
    unsigned n_corr = 0;
    const uint64_t ticks = 300u * crystal / frame_ticks;
    for (uint64_t k = 0; k < ticks; k++) {
        /* tik k v čase k * takty snímku / krystal; snímky končí o půl snímku dřív */
        const uint64_t t_num = k * frame_ticks * 2u;          /* čas v jednotkách 1 / (2 * krystal) s */
        while ((2u * frames + 1u) * frame_ticks <= t_num) {
            const uint64_t acc = rem + (uint64_t)IN_RATE * frame_ticks;
            rem = acc % crystal;
            const size_t n = (size_t)(acc / crystal);
            if (n == lo) n_lo++;
            else if (n == lo + 1) n_hi++;
            else TEST_FAIL_MESSAGE("block length is neither floor nor ceil of the mean");
            videorec_rt_audio_push(&RA, buf, n);
            frames++;
        }
        size_t f = videorec_rt_audio_fill(&RA);
        videorec_rt_audio_pull(&RA, out, out_tick, 1.0f);
        if (k >= ticks * 2 / 3) {
            double c = RA.stats.corr;
            sum_corr += c;
            sum_fill += (double)f;
            n_corr++;
            if (c < min_corr) min_corr = c;
            if (c > max_corr) max_corr = c;
        }
    }
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
    printf("crystal %llu Hz, tick scale %.6f: %u x %zu, %u x %zu, mean corr %+.6f (want %+.6f, min %+.5f, max %+.5f), "
           "mean fill %.0f (target %.0f), max |corr| %.5f, underruns %llu, overruns %llu\n", (unsigned long long)crystal,
           scale, n_lo, lo, n_hi, lo + 1, sum_corr / n_corr, want_corr, min_corr, max_corr, sum_fill / n_corr, target,
           st.max_abs_corr, (unsigned long long)st.underruns, (unsigned long long)st.overruns);
    TEST_ASSERT_TRUE(n_lo > 0 && n_hi > 0);
    TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
    TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
    TEST_ASSERT_DOUBLE_WITHIN(0.0002, want_corr, sum_corr / n_corr);
    TEST_ASSERT_TRUE(max_corr - min_corr < 0.003);
    TEST_ASSERT_DOUBLE_WITHIN(fill_tol, target, st.fill_ema);
    videorec_rt_audio_set_tick_scale(&RA, 1.0);
    return sum_fill / n_corr;
}

/**
 * Záznam podle reality s krystalem: vzorkovač tiká stejným tempem jako emulace
 * (krystal / takty snímku), hlavička AVI zůstává 50/1 (60/1), takže se za tik
 * odebírá 960 (800) vzorků 48 kHz.
 *
 * - Bez měřítka ticku by jitter buffer držel trvalou korekci PAL 882 * (1 + c) =
 *   881,3597 -> c = -0,0726 %, NTSC 735 * (1 + c) = 735,9476 -> c = +0,1289 %
 *   (mez VIDEOREC_RT_AUDIO_MAX_CORR je 0,5 %) a naplnění by trvale leželo o
 *   c * in_rate * VIDEOREC_RT_AUDIO_DRIFT_TC_S mimo cíl: PAL -320 vzorků (-7,3 ms),
 *   NTSC +569 vzorků (+12,9 ms) - o tolik by se posunul zvuk proti obrazu.
 * - S měřítkem ticku (skutečná / jmenovitá délka ticku, jako nastavuje lepidlo)
 *   je trvalá korekce nulová a naplnění na cíli (do 1 bloku).
 */
static void test_audio_crystal_blocks(void)
{
    const double pal = 44100.0 * 354432.0 / 17734475.0 / 882.0 - 1.0;
    const double ntsc = 44100.0 * 238944.0 / 14318180.0 / 735.0 - 1.0;
    const double target = IN_RATE * VIDEOREC_RT_AUDIO_TARGET_MS / 1000.0;
    double f_pal = run_crystal_blocks(17734475u, 354432u, 960u, 881, 1.0, pal, 2.0 * CHUNK);
    double f_ntsc = run_crystal_blocks(14318180u, 238944u, 800u, 735, 1.0, ntsc, 2.0 * CHUNK);
    /* posun naplnění bez měřítka ticku: -7,3 ms / +12,9 ms (do 1 ms) */
    TEST_ASSERT_DOUBLE_WITHIN(44.1, pal * IN_RATE * VIDEOREC_RT_AUDIO_DRIFT_TC_S, f_pal - target);
    TEST_ASSERT_DOUBLE_WITHIN(44.1, ntsc * IN_RATE * VIDEOREC_RT_AUDIO_DRIFT_TC_S, f_ntsc - target);
    const double s_pal = 50.0 * 354432.0 / 17734475.0, s_ntsc = 60.0 * 238944.0 / 14318180.0;
    f_pal = run_crystal_blocks(17734475u, 354432u, 960u, 881, s_pal, 0.0, (double)CHUNK);
    f_ntsc = run_crystal_blocks(14318180u, 238944u, 800u, 735, s_ntsc, 0.0, (double)CHUNK);
    /* průměrné naplnění (před odběrem) do půl bloku od cíle */
    TEST_ASSERT_DOUBLE_WITHIN(CHUNK / 2.0, target, f_pal);
    TEST_ASSERT_DOUBLE_WITHIN(CHUNK / 2.0, target, f_ntsc);
}

/**
 * Náběh na cíl: dva bloky 881 (1762) nestačí; tři bloky 881 (2643, o 3 vzorky pod cílem 2646)
 * stačí díky toleranci VIDEOREC_RT_AUDIO_PRIME_TOL_MS - dřív se čekalo na čtvrtý (ticho na
 * začátku o snímek delší, start s 3524 vzorky a korekce (3524 - 2646) / 441 000 = 0,199 %).
 * Blok NTSC: tři bloky 735 (2205) jsou pod tolerancí, čtvrtý dá 2940 a přesah nad cíl se
 * zahodí (`prime_drops`). Korekce na začátku je v obou případech 0 (do 0,001 %).
 */
static void test_audio_prime_exact_target(void)
{
    static float buf[CHUNK * 2];
    for (unsigned i = 0; i < CHUNK * 2; i++) buf[i] = 0.5f;
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 2; k++) videorec_rt_audio_push(&RA, buf, 881);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(RA.starved);
    TEST_ASSERT_EQUAL_INT16(0, out[0]);
    videorec_rt_audio_push(&RA, buf, 881);
    TEST_ASSERT_EQUAL_UINT(3 * 881, videorec_rt_audio_fill(&RA));
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_FALSE(RA.starved);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(0, st.prime_drops);
    TEST_ASSERT_TRUE(st.max_abs_corr < 0.00001);
    TEST_ASSERT_INT16_WITHIN(2, 16384, out[0]);
    /* po odběru jednoho ticku zbývá naplnění minus odebrané vzorky (882 při 960 / 48 kHz) */
    TEST_ASSERT_UINT_WITHIN(1u, 3 * 881 - 882u, videorec_rt_audio_fill(&RA)); /* korekce -3 / 441 000 */
    /* NTSC: bloky 735 */
    videorec_rt_audio_reset(&RA, true, 0);
    for (int k = 0; k < 3; k++) videorec_rt_audio_push(&RA, buf, 735);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(RA.starved);
    videorec_rt_audio_push(&RA, buf, 735);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_FALSE(RA.starved);
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(4 * 735 - RA.target, st.prime_drops);
    TEST_ASSERT_TRUE(st.max_abs_corr < 0.00001);
}

/* Jitter: zařízení si bere data po dvou blocích každých 40 ms - stále bez podtečení. */
static void test_audio_bursty_producer(void)
{
    (void)simulate(30.0, 0.0, 2);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(0, st.underruns);
    TEST_ASSERT_EQUAL_UINT64(0, st.overruns);
}

/* Výpadek producenta (pauza zařízení): ticho, pak znovu naplnění na cíl a pokračování. */
static void test_audio_starve_and_reprime(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 10; k++) {
        push_chunk(0.5f);
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    }
    TEST_ASSERT_FALSE(RA.starved);
    /* výpadek */
    for (int k = 0; k < 5; k++) videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(RA.starved);
    TEST_ASSERT_EQUAL_INT16(0, out[0]);
    TEST_ASSERT_EQUAL_INT16(0, out[OUT_TICK * 2 - 1]);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_TRUE(st.underruns >= 1);
    /* dva bloky nestačí na cíl (3 bloky) - pořád ticho a nic se nespotřebuje */
    push_chunk(0.5f);
    push_chunk(0.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(0, out[OUT_TICK]);
    TEST_ASSERT_EQUAL_UINT(2 * CHUNK, videorec_rt_audio_fill(&RA));
    /* třetí blok = cíl: data */
    push_chunk(0.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_FALSE(RA.starved);
    TEST_ASSERT_INT16_WITHIN(2, 16384, out[OUT_TICK]);
}

/* Zesílení: 0 = ticho při zachované spotřebě, 0,25 = útlum; NULL výstup jen spotřebuje. */
static void test_audio_gain_and_discard(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 4; k++) push_chunk(0.8f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 0.25f);
    TEST_ASSERT_INT16_WITHIN(2, (int16_t)lrintf(0.2f * 32767.0f), out[100]);
    size_t f1 = videorec_rt_audio_fill(&RA);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 0.0f);
    TEST_ASSERT_EQUAL_INT16(0, out[100]);
    size_t f2 = videorec_rt_audio_fill(&RA);
    TEST_ASSERT_TRUE(f2 < f1); /* spotřebováno */
    videorec_rt_audio_pull(&RA, NULL, OUT_TICK, 1.0f);
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) < f2);
}

/* Ořez: |gain * vzorek| > 1 se ořízne na int16 rozsah. */
static void test_audio_clip(void)
{
    int16_t out[OUT_TICK * 2];
    for (int k = 0; k < 3; k++) push_chunk(1.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(32767, out[10]);
    for (int k = 0; k < 3; k++) push_chunk(-1.5f);
    videorec_rt_audio_reset(&RA, true, 0);
    for (int k = 0; k < 3; k++) push_chunk(-1.5f);
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    TEST_ASSERT_EQUAL_INT16(-32767, out[10]);
}

/* Zakázaný buffer zahazuje data; wants_data jen pod cílem + blok. */
static void test_audio_disabled_and_wants_data(void)
{
    videorec_rt_audio_reset(&RA, false, 0);
    TEST_ASSERT_FALSE(videorec_rt_audio_wants_data(&RA, CHUNK));
    push_chunk(0.5f);
    TEST_ASSERT_EQUAL_UINT(0, videorec_rt_audio_fill(&RA));
    videorec_rt_audio_reset(&RA, true, 0);
    unsigned pushes = 0;
    while (videorec_rt_audio_wants_data(&RA, CHUNK)) {
        push_chunk(0.5f);
        pushes++;
    }
    /* cíl 3 bloky + 1 blok rezervy */
    TEST_ASSERT_EQUAL_UINT(RA.target / CHUNK + 1, pushes);
    TEST_ASSERT_EQUAL_UINT(RA.target + CHUNK, videorec_rt_audio_fill(&RA));
}

/* Přetečení kapacity: nejstarší data se zahodí, naplnění nepřekročí kapacitu. */
static void test_audio_capacity_overflow(void)
{
    for (int k = 0; k < 100; k++) push_chunk(0.5f); /* 2 s > kapacita 1 s */
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) <= RA.cap);
    st_VIDEOREC_RT_AUDIO_STATS st;
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_TRUE(st.overflow_drops > 0);
    /* první pull zahodí přebytek nad cíl (overrun) a pokračuje plynule */
    int16_t out[OUT_TICK * 2];
    videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
    videorec_rt_audio_get_stats(&RA, &st);
    TEST_ASSERT_EQUAL_UINT64(1, st.overruns);
    TEST_ASSERT_TRUE(videorec_rt_audio_fill(&RA) <= RA.target);
}

/**
 * Kvalita převzorkování 44,1 -> 48 kHz: sinus 1 kHz má po převzorkování
 * frekvenci 1 kHz (průchody nulou) a amplitudu v toleranci lineární interpolace.
 */
static void test_audio_resample_sine(void)
{
    const double f = 1000.0;
    float buf[CHUNK * 2];
    uint64_t n_in = 0;
    int16_t out[OUT_TICK * 2];
    unsigned crossings = 0;
    int16_t prev = 0, maxv = 0;
    bool have_prev = false;
    uint64_t counted = 0;
    for (int tick = 0; tick < 200; tick++) {
        for (unsigned i = 0; i < CHUNK; i++, n_in++) {
            float v = (float)(0.5 * sin(2.0 * G_PI * f * (double)n_in / IN_RATE));
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        videorec_rt_audio_push(&RA, buf, CHUNK);
        videorec_rt_audio_pull(&RA, out, OUT_TICK, 1.0f);
        if (tick < 20) continue; /* ustálení */
        for (unsigned i = 0; i < OUT_TICK; i++) {
            int16_t v = out[2 * i];
            TEST_ASSERT_EQUAL_INT16(v, out[2 * i + 1]);
            if (v > maxv) maxv = v;
            if (have_prev && prev < 0 && v >= 0) crossings++;
            prev = v;
            have_prev = true;
            counted++;
        }
    }
    double secs = (double)counted / OUT_RATE;
    TEST_ASSERT_DOUBLE_WITHIN(0.005 * f, f, crossings / secs);
    /* amplituda 0,5 * 32767; lineární interpolace (< 0,3 %) a vzorkování vrcholu při 48 kHz (< 0,3 %) */
    TEST_ASSERT_INT_WITHIN(150, 16384, maxv);
}

/* Při stejné vstupní a výstupní frekvenci a bez driftu je výstup přesná kopie vstupu. */
static void test_audio_identity_44k(void)
{
    videorec_rt_audio_free(&RA);
    videorec_rt_audio_init(&RA, IN_RATE, IN_RATE, VIDEOREC_RT_AUDIO_TARGET_MS, VIDEOREC_RT_AUDIO_CAPACITY_MS);
    videorec_rt_audio_reset(&RA, true, 0);
    float buf[CHUNK * 2];
    int16_t out[CHUNK * 2];
    uint64_t n = 0;
    for (unsigned i = 0; i < CHUNK; i++) buf[2 * i] = buf[2 * i + 1] = (float)i / CHUNK;
    for (int k = 0; k < 3; k++) videorec_rt_audio_push(&RA, buf, CHUNK); /* = cíl */
    videorec_rt_audio_pull(&RA, out, CHUNK, 1.0f);
    for (unsigned i = 0; i < CHUNK; i++, n++) {
        TEST_ASSERT_INT16_WITHIN(1, (int16_t)lrintf((float)i / CHUNK * 32767.0f), out[2 * i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clock_first_tick_and_period);
    RUN_TEST(test_clock_catchup_without_drift);
    RUN_TEST(test_clock_rebase_on_large_lag);
    RUN_TEST(test_clock_fractional_period_60fps);
    RUN_TEST(test_clock_fps_50_equals_period);
    RUN_TEST(test_video_delay_ticks_from_ms);
    RUN_TEST(test_sdl_chunk_per_fps);
    RUN_TEST(test_clock_crystal_fps);
    RUN_TEST(test_frameq_order_and_empty);
    RUN_TEST(test_frameq_overflow_drops_oldest);
    RUN_TEST(test_frameq_purge_tag);
    RUN_TEST(test_frameq_live_priming_and_underflow);
    RUN_TEST(test_frameq_prime_overshoot_trimmed);
    RUN_TEST(test_frameq_late_wakeup_no_loss);
    RUN_TEST(test_frameq_free_and_drain_modes);
    RUN_TEST(test_frameq_free_shared_images_no_repeat);
    RUN_TEST(test_frameq_every_frame_once_with_jitter);
    RUN_TEST(test_frameq_nominal_schedule_drops);
    RUN_TEST(test_frameq_drift_corrections);
    RUN_TEST(test_frameq_slow_machine_even_repeats);
    RUN_TEST(test_frameq_slow_machine_marker_position);
    RUN_TEST(test_frameq_isolated_stalls_not_slow);
    RUN_TEST(test_frameq_short_frames_drop_singly);
    RUN_TEST(test_audio_steady_no_drift);
    RUN_TEST(test_audio_drift_corrected);
    RUN_TEST(test_audio_crystal_blocks);
    RUN_TEST(test_audio_prime_exact_target);
    RUN_TEST(test_audio_bursty_producer);
    RUN_TEST(test_audio_starve_and_reprime);
    RUN_TEST(test_audio_gain_and_discard);
    RUN_TEST(test_audio_clip);
    RUN_TEST(test_audio_disabled_and_wants_data);
    RUN_TEST(test_audio_capacity_overflow);
    RUN_TEST(test_audio_resample_sine);
    RUN_TEST(test_audio_identity_44k);
    return UNITY_END();
}
