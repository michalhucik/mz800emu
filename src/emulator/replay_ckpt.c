/**
 * @file   replay_ckpt.c
 * @brief  Kruhový buffer checkpointů záznamu a přehrání vstupů (viz replay_ckpt.h).
 */

#include <string.h>

#include "replay_ckpt.h"


/** @brief Položka podle pořadí (0 = nejstarší), bez kontroly rozsahu. */
static st_REPLAY_CKPT *replay_ckpt_at(const st_REPLAY_CKPT_RING *ring, guint i)
{
    return &ring->items[(ring->head + i) % ring->capacity];
}


/** @brief Uvolní snapshot položky a sníží součet velikostí. */
static void replay_ckpt_release(st_REPLAY_CKPT_RING *ring, st_REPLAY_CKPT *c)
{
    ring->bytes -= c->snap_size;
    g_free(c->snap);
    memset(c, 0, sizeof(*c));
}


void replay_ckpt_ring_init(st_REPLAY_CKPT_RING *ring, guint capacity)
{
    if (capacity < 1) capacity = 1;
    if (capacity > REPLAY_CKPT_MAX_COUNT) capacity = REPLAY_CKPT_MAX_COUNT;
    memset(ring, 0, sizeof(*ring));
    ring->items = g_new0(st_REPLAY_CKPT, capacity);
    ring->capacity = capacity;
}


void replay_ckpt_ring_clear(st_REPLAY_CKPT_RING *ring)
{
    if (!ring->capacity) return;
    for (guint i = 0; i < ring->count; i++) replay_ckpt_release(ring, replay_ckpt_at(ring, i));
    ring->head = 0;
    ring->count = 0;
    ring->bytes = 0;
}


void replay_ckpt_ring_free(st_REPLAY_CKPT_RING *ring)
{
    replay_ckpt_ring_clear(ring);
    g_free(ring->items);
    memset(ring, 0, sizeof(*ring));
}


void replay_ckpt_ring_truncate_after(st_REPLAY_CKPT_RING *ring, guint i)
{
    while (ring->count > i + 1)
    {
        replay_ckpt_release(ring, replay_ckpt_at(ring, ring->count - 1));
        ring->count--;
    };
}


void replay_ckpt_ring_push(st_REPLAY_CKPT_RING *ring, const st_REPLAY_CKPT *c)
{
    /* Pořadí podle snímku musí růst: novější checkpoint téhož nebo
     * pozdějšího snímku (po rewindu) nahradí. */
    while (ring->count && replay_ckpt_at(ring, ring->count - 1)->frame >= c->frame)
    {
        replay_ckpt_release(ring, replay_ckpt_at(ring, ring->count - 1));
        ring->count--;
    };
    if (ring->count == ring->capacity)
    {
        replay_ckpt_release(ring, replay_ckpt_at(ring, 0));
        ring->head = (ring->head + 1) % ring->capacity;
        ring->count--;
    };
    *replay_ckpt_at(ring, ring->count) = *c;
    ring->count++;
    ring->bytes += c->snap_size;
}


const st_REPLAY_CKPT *replay_ckpt_ring_get(const st_REPLAY_CKPT_RING *ring, guint i)
{
    if (i >= ring->count) return NULL;
    return replay_ckpt_at(ring, i);
}


bool replay_ckpt_ring_insert(st_REPLAY_CKPT_RING *ring, const st_REPLAY_CKPT *c)
{
    /* Pozice: první položka se snímkem >= nový. */
    guint k = 0;
    while (k < ring->count && replay_ckpt_at(ring, k)->frame < c->frame) k++;
    if (k < ring->count && replay_ckpt_at(ring, k)->frame == c->frame)
    {
        replay_ckpt_release(ring, replay_ckpt_at(ring, k));
        *replay_ckpt_at(ring, k) = *c;
        ring->bytes += c->snap_size;
        return true;
    };
    if (ring->count == ring->capacity)
    {
        if (k == 0)
        {
            g_free(c->snap);
            return false;
        };
        replay_ckpt_release(ring, replay_ckpt_at(ring, 0));
        ring->head = (ring->head + 1) % ring->capacity;
        ring->count--;
        k--;
    };
    for (guint i = ring->count; i > k; i--) *replay_ckpt_at(ring, i) = *replay_ckpt_at(ring, i - 1);
    *replay_ckpt_at(ring, k) = *c;
    ring->count++;
    ring->bytes += c->snap_size;
    return true;
}


bool replay_ckpt_ring_has_frame(const st_REPLAY_CKPT_RING *ring, uint32_t frame)
{
    for (guint k = 0; k < ring->count; k++)
    {
        if (replay_ckpt_at(ring, k)->frame == frame) return true;
    };
    return false;
}


en_REPLAY_CKPT_FIND replay_ckpt_ring_find(const st_REPLAY_CKPT_RING *ring, uint32_t max_frame,
                                          uint32_t media_writes, guint *out_index)
{
    for (guint k = ring->count; k > 0; k--)
    {
        const st_REPLAY_CKPT *c = replay_ckpt_at(ring, k - 1);
        if (c->frame > max_frame) continue;
        if (out_index) *out_index = k - 1;
        return (c->media_writes == media_writes) ? REPLAY_CKPT_FOUND : REPLAY_CKPT_MEDIA_WRITTEN;
    };
    return REPLAY_CKPT_NONE;
}
