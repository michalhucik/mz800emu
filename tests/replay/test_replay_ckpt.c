/**
 * @file test_replay_ckpt.c
 * @brief Unit testy kruhového bufferu checkpointů (replay_ckpt.c)
 *        a zkrácení záznamu při rewindu (replay_data_truncate()).
 *
 * Pokrývá: přidání a vyřazení nejstaršího při plném bufferu (součet
 * velikostí), náhradu checkpointu téhož nebo pozdějšího snímku, hledání
 * nejnovějšího checkpointu do snímku, blokaci zápisem na médium (i pro
 * starší checkpointy), odebrání novějších po rewindu, zkrácení událostí,
 * hashů a médií záznamu včetně příznaku událostí debuggeru.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/replay_ckpt.h"
#include "emulator/replay_format.h"

void setUp(void) {}
void tearDown(void) {}


/** @brief Checkpoint snímku @p frame se snapshotem velikosti @p size. */
static st_REPLAY_CKPT make(uint32_t frame, size_t size, uint32_t media_writes)
{
    st_REPLAY_CKPT c;
    memset(&c, 0, sizeof(c));
    c.frame = frame;
    c.snap = g_malloc0(size);
    c.snap_size = size;
    c.media_writes = media_writes;
    return c;
}


/** @test Plný buffer vyřadí nejstarší, součet velikostí sedí. */
static void test_ring_evicts_oldest(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 3);
    for (uint32_t f = 0; f < 5; f++)
    {
        st_REPLAY_CKPT c = make(f * 250, 100 + f, 0);
        replay_ckpt_ring_push(&r, &c);
    };
    TEST_ASSERT_EQUAL_UINT(3, r.count);
    TEST_ASSERT_EQUAL_UINT32(500, replay_ckpt_ring_get(&r, 0)->frame);
    TEST_ASSERT_EQUAL_UINT32(1000, replay_ckpt_ring_get(&r, 2)->frame);
    TEST_ASSERT_NULL(replay_ckpt_ring_get(&r, 3));
    TEST_ASSERT_EQUAL_UINT(102 + 103 + 104, r.bytes);
    replay_ckpt_ring_free(&r);
    TEST_ASSERT_EQUAL_UINT(0, r.capacity);
}


/** @test Checkpoint téhož nebo staršího snímku nahradí novější (po rewindu). */
static void test_ring_replaces_newer(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 10);
    for (uint32_t f = 0; f < 4; f++)
    {
        st_REPLAY_CKPT c = make(f * 250, 10, 0);
        replay_ckpt_ring_push(&r, &c);
    };
    st_REPLAY_CKPT c = make(500, 20, 0);
    replay_ckpt_ring_push(&r, &c);
    TEST_ASSERT_EQUAL_UINT(3, r.count);
    TEST_ASSERT_EQUAL_UINT32(500, replay_ckpt_ring_get(&r, 2)->frame);
    TEST_ASSERT_EQUAL_UINT(10 + 10 + 20, r.bytes);
    replay_ckpt_ring_free(&r);
}


/** @test Dotaz na checkpoint konkrétního snímku. */
static void test_ring_has_frame(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 10);
    for (uint32_t f = 1; f <= 3; f++)
    {
        st_REPLAY_CKPT c = make(f * 250, 1, 0);
        replay_ckpt_ring_push(&r, &c);
    };
    TEST_ASSERT_TRUE(replay_ckpt_ring_has_frame(&r, 250));
    TEST_ASSERT_TRUE(replay_ckpt_ring_has_frame(&r, 750));
    TEST_ASSERT_FALSE(replay_ckpt_ring_has_frame(&r, 500 + 1));
    TEST_ASSERT_FALSE(replay_ckpt_ring_has_frame(&r, 0));
    replay_ckpt_ring_clear(&r);
    TEST_ASSERT_FALSE(replay_ckpt_ring_has_frame(&r, 250));
    replay_ckpt_ring_free(&r);
}


/** @test Vložení podle snímku: novější zůstanou, týž snímek nahradí, plný buffer. */
static void test_ring_insert(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 4);
    st_REPLAY_CKPT a = make(0, 1, 0), b = make(250, 1, 0), c = make(500, 1, 0);
    replay_ckpt_ring_push(&r, &a);
    replay_ckpt_ring_push(&r, &b);
    replay_ckpt_ring_push(&r, &c);
    st_REPLAY_CKPT m = make(300, 2, 0);
    TEST_ASSERT_TRUE(replay_ckpt_ring_insert(&r, &m));
    TEST_ASSERT_EQUAL_UINT(4, r.count);
    TEST_ASSERT_EQUAL_UINT32(300, replay_ckpt_ring_get(&r, 2)->frame);
    TEST_ASSERT_EQUAL_UINT32(500, replay_ckpt_ring_get(&r, 3)->frame);
    TEST_ASSERT_EQUAL_UINT(1 + 1 + 2 + 1, r.bytes);
    /* Týž snímek nahradí (velikost se přepočte). */
    st_REPLAY_CKPT m2 = make(300, 5, 0);
    TEST_ASSERT_TRUE(replay_ckpt_ring_insert(&r, &m2));
    TEST_ASSERT_EQUAL_UINT(4, r.count);
    TEST_ASSERT_EQUAL_UINT(1 + 1 + 5 + 1, r.bytes);
    /* Plný buffer: nový mezi uloženými vyřadí nejstarší (0). */
    st_REPLAY_CKPT n = make(400, 1, 0);
    TEST_ASSERT_TRUE(replay_ckpt_ring_insert(&r, &n));
    TEST_ASSERT_EQUAL_UINT(4, r.count);
    TEST_ASSERT_EQUAL_UINT32(250, replay_ckpt_ring_get(&r, 0)->frame);
    TEST_ASSERT_EQUAL_UINT32(400, replay_ckpt_ring_get(&r, 2)->frame);
    TEST_ASSERT_EQUAL_UINT32(500, replay_ckpt_ring_get(&r, 3)->frame);
    /* Plný buffer a nový starší než všechny: zahozen. */
    st_REPLAY_CKPT o = make(100, 1, 0);
    TEST_ASSERT_FALSE(replay_ckpt_ring_insert(&r, &o));
    TEST_ASSERT_EQUAL_UINT32(250, replay_ckpt_ring_get(&r, 0)->frame);
    TEST_ASSERT_EQUAL_UINT(1 + 5 + 1 + 1, r.bytes);
    /* Nový za všemi = jako push. */
    st_REPLAY_CKPT z = make(900, 1, 0);
    TEST_ASSERT_TRUE(replay_ckpt_ring_insert(&r, &z));
    TEST_ASSERT_EQUAL_UINT32(900, replay_ckpt_ring_get(&r, 3)->frame);
    TEST_ASSERT_EQUAL_UINT32(300, replay_ckpt_ring_get(&r, 0)->frame);
    replay_ckpt_ring_free(&r);
}


/** @test Hledání do snímku a blokace zápisem na médium. */
static void test_ring_find(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 10);
    st_REPLAY_CKPT c0 = make(0, 1, 5);
    st_REPLAY_CKPT c1 = make(250, 1, 5);
    st_REPLAY_CKPT c2 = make(500, 1, 7);
    replay_ckpt_ring_push(&r, &c0);
    replay_ckpt_ring_push(&r, &c1);
    replay_ckpt_ring_push(&r, &c2);

    guint i = 99;
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_FOUND, replay_ckpt_ring_find(&r, 600, 7, &i));
    TEST_ASSERT_EQUAL_UINT(2, i);
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_FOUND, replay_ckpt_ring_find(&r, 499, 5, &i));
    TEST_ASSERT_EQUAL_UINT(1, i);
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_FOUND, replay_ckpt_ring_find(&r, 0, 5, &i));
    TEST_ASSERT_EQUAL_UINT(0, i);
    /* Zápis na médium po 500: nejbližší i starší nejdou použít. */
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_MEDIA_WRITTEN, replay_ckpt_ring_find(&r, 600, 8, &i));
    TEST_ASSERT_EQUAL_UINT(2, i);
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_MEDIA_WRITTEN, replay_ckpt_ring_find(&r, 300, 7, &i));
    TEST_ASSERT_EQUAL_UINT(1, i);

    replay_ckpt_ring_truncate_after(&r, 0);
    TEST_ASSERT_EQUAL_UINT(1, r.count);
    TEST_ASSERT_EQUAL_UINT(1, r.bytes);
    replay_ckpt_ring_clear(&r);
    TEST_ASSERT_EQUAL_INT(REPLAY_CKPT_NONE, replay_ckpt_ring_find(&r, 1000, 5, NULL));
    replay_ckpt_ring_free(&r);
}


/** @test Kapacita se ořízne na 1 .. REPLAY_CKPT_MAX_COUNT. */
static void test_ring_capacity_clamp(void)
{
    st_REPLAY_CKPT_RING r;
    replay_ckpt_ring_init(&r, 0);
    TEST_ASSERT_EQUAL_UINT(1, r.capacity);
    replay_ckpt_ring_free(&r);
    replay_ckpt_ring_init(&r, REPLAY_CKPT_MAX_COUNT + 5);
    TEST_ASSERT_EQUAL_UINT(REPLAY_CKPT_MAX_COUNT, r.capacity);
    replay_ckpt_ring_free(&r);
}


/** @test Zkrácení záznamu: události, hashe, média a příznak debuggeru. */
static void test_data_truncate(void)
{
    st_REPLAY_DATA *d = replay_data_new();
    st_REPLAY_EVENT ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_AUTOTYPE;
    ev.text = (char *)"LOAD";
    replay_data_add_event(d, &ev);
    memset(&ev, 0, sizeof(ev));
    ev.kind = REPLAY_EV_DBG;
    ev.frame = 300;
    ev.cmd = (char *)"mem_write";
    ev.args_json = (char *)"{}";
    replay_data_add_event(d, &ev);
    TEST_ASSERT_TRUE(d->has_debugger_events);
    st_REPLAY_HASH_ROW row;
    memset(&row, 0, sizeof(row));
    replay_data_add_hash(d, &row);
    row.frame = 300;
    replay_data_add_hash(d, &row);
    static const uint8_t data[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL_INT(0, replay_data_add_media(d, "fdc0.0", "a.dsk", data, 4, NULL, false));
    TEST_ASSERT_EQUAL_INT(1, replay_data_add_media(d, "cmthack.0", "b.mzf", data, 4, NULL, true));

    replay_data_truncate(d, 1, 1, 1);
    TEST_ASSERT_EQUAL_UINT(1, d->events->len);
    TEST_ASSERT_EQUAL_STRING("LOAD", g_array_index(d->events, st_REPLAY_EVENT, 0).text);
    TEST_ASSERT_FALSE(d->has_debugger_events);
    TEST_ASSERT_EQUAL_UINT(1, d->hashes->len);
    TEST_ASSERT_EQUAL_UINT(1, d->media->len);
    TEST_ASSERT_NULL(replay_data_find_media(d, "cmthack.0"));
    TEST_ASSERT_NOT_NULL(replay_data_find_media(d, "fdc0.0"));

    /* Větší délky než obsah = beze změny. */
    replay_data_truncate(d, 10, 10, 10);
    TEST_ASSERT_EQUAL_UINT(1, d->events->len);
    replay_data_free(d);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_ring_evicts_oldest);
    RUN_TEST(test_ring_replaces_newer);
    RUN_TEST(test_ring_has_frame);
    RUN_TEST(test_ring_insert);
    RUN_TEST(test_ring_find);
    RUN_TEST(test_ring_capacity_clamp);
    RUN_TEST(test_data_truncate);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
