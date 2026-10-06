/**
 * @file   videorec_pipe.c
 * @brief  Implementace párování obrazu a zvuku pro video záznam (viz videorec_pipe.h).
 *
 * @par Licence: GPLv3
 */

#include "videorec_pipe.h"

#include <glib.h>
#include <string.h>

void videorec_pipe_init(st_VIDEOREC_PIPE *p, unsigned width, unsigned height, uint64_t ticks_per_frame, uint64_t clk_hz,
                        unsigned rate, unsigned channels, const float level[][VIDEOREC_AUDIO_LEVELS],
                        const uint8_t *values, uint64_t origin_ticks, videorec_pipe_sink_cb sink, void *user)
{
    memset(p, 0, sizeof(*p));
    p->width = width;
    p->height = height;
    p->tpf = ticks_per_frame;
    p->spf = (unsigned)((uint64_t)rate * ticks_per_frame / clk_hz);
    p->next_frame_end = origin_ticks + ticks_per_frame;
    p->sink = sink;
    p->user = user;
    videorec_audio_init(&p->audio, clk_hz, rate, channels, level, values, origin_ticks, true);
}

/** @brief Uvolní pixely všech čekajících snímků, vyprázdní frontu a zahodí nevyřízené požadavky na zachycení. */
static void pipe_drop_pending(st_VIDEOREC_PIPE *p)
{
    p->ncapture = 0;
    while (p->count) {
        st_VIDEOREC_PIPE_PENDING *pd = &p->pending[p->head];
        g_free(pd->pixels);
        pd->pixels = NULL;
        p->head = (p->head + 1) % VIDEOREC_PIPE_MAX_PENDING;
        p->count--;
    }
    p->head = 0;
}

void videorec_pipe_free(st_VIDEOREC_PIPE *p)
{
    pipe_drop_pending(p);
    videorec_audio_free(&p->audio);
}

en_VIDEOREC_PIPE_RESULT videorec_pipe_frame(st_VIDEOREC_PIPE *p, const uint8_t *pixels_or_null, uint64_t frame_end_ticks)
{
    if (frame_end_ticks != p->next_frame_end) return VIDEOREC_PIPE_ERR_DISCONTINUITY;
    if (p->count >= VIDEOREC_PIPE_MAX_PENDING) return VIDEOREC_PIPE_ERR_FULL;

    st_VIDEOREC_PIPE_PENDING *pd = &p->pending[(p->head + p->count) % VIDEOREC_PIPE_MAX_PENDING];
    pd->end = frame_end_ticks;
    pd->pixels = NULL;
    if (pixels_or_null) {
        size_t n = (size_t)p->width * p->height;
        pd->pixels = g_malloc(n);
        for (size_t i = 0; i < n; i++) pd->pixels[i] = pixels_or_null[i] & 0x0F;
    }
    p->count++;
    p->next_frame_end += p->tpf;
    return VIDEOREC_PIPE_OK;
}

void videorec_pipe_audio_event(st_VIDEOREC_PIPE *p, unsigned ch, uint8_t value, uint64_t ticks)
{
    (void)videorec_audio_event(&p->audio, ch, value, ticks);
}

/** @brief Předá callbacku aktuální stav rendereru (bez callbacku nic). */
static void pipe_capture_now(st_VIDEOREC_PIPE *p, uint64_t tag)
{
    if (!p->capture_cb) return;
    st_VIDEOREC_AUDIO_STATE st;
    videorec_audio_get_state(&p->audio, &st);
    p->capture_cb(tag, &st, p->capture_user);
}

/**
 * @brief Vyřídí požadavky na zachycení, jejichž snímek končí v `end`.
 * @param p   Instance.
 * @param end Takt konce právě vyrenderovaného snímku.
 */
static void pipe_capture_due(st_VIDEOREC_PIPE *p, uint64_t end)
{
    unsigned keep = 0;
    for (unsigned i = 0; i < p->ncapture; i++) {
        if (p->capture[i].end == end) pipe_capture_now(p, p->capture[i].tag);
        else if (p->capture[i].end > end) p->capture[keep++] = p->capture[i];
        /* end < konec snímku: bod už minul (porušený invariant) - zahodit */
    }
    p->ncapture = keep;
}

/** @brief Vyrenderuje zvuk prvního čekajícího snímku, vyřídí zachycení stavu a předá snímek sinku (nebo zahodí). */
static void pipe_emit_head(st_VIDEOREC_PIPE *p)
{
    st_VIDEOREC_PIPE_PENDING *pd = &p->pending[p->head];
    int16_t *audio = g_malloc((size_t)p->spf * 2 * sizeof(int16_t));
    size_t got = videorec_audio_render(&p->audio, pd->end, audio, p->spf);
    /* got < spf nastane jen při chybném nastavení clk/tpf/rate - doplnit tichem */
    for (size_t i = got; i < p->spf; i++) audio[i * 2] = audio[i * 2 + 1] = 0;
    if (p->ncapture) pipe_capture_due(p, pd->end);
    if (pd->pixels) {
        st_VIDEOREC_PIPE_FRAME fr = { pd->pixels, audio, p->spf, p->next_index++ };
        p->sink(&fr, p->user);
    } else {
        g_free(audio);
    }
    pd->pixels = NULL;
    p->head = (p->head + 1) % VIDEOREC_PIPE_MAX_PENDING;
    p->count--;
}

void videorec_pipe_horizon(st_VIDEOREC_PIPE *p, uint64_t horizon_ticks)
{
    while (p->count && p->pending[p->head].end <= horizon_ticks) pipe_emit_head(p);
}

void videorec_pipe_flush(st_VIDEOREC_PIPE *p)
{
    while (p->count) pipe_emit_head(p);
}

void videorec_pipe_flush_until_index(st_VIDEOREC_PIPE *p, uint64_t index)
{
    while (p->count && p->next_index < index) pipe_emit_head(p);
}

void videorec_pipe_rebase(st_VIDEOREC_PIPE *p, uint64_t origin_ticks, const uint8_t *values)
{
    pipe_drop_pending(p);
    videorec_audio_rebase(&p->audio, origin_ticks, values);
    p->next_frame_end = origin_ticks + p->tpf;
}

void videorec_pipe_set_capture_cb(st_VIDEOREC_PIPE *p, videorec_pipe_capture_cb cb, void *user)
{
    p->capture_cb = cb;
    p->capture_user = user;
}

void videorec_pipe_request_capture(st_VIDEOREC_PIPE *p, uint64_t tag)
{
    if (p->count == 0) {
        /* vše vyrenderováno: renderer stojí přesně na konci posledního přijatého snímku */
        pipe_capture_now(p, tag);
        return;
    }
    if (p->ncapture == VIDEOREC_PIPE_MAX_CAPTURES) {
        memmove(&p->capture[0], &p->capture[1], (VIDEOREC_PIPE_MAX_CAPTURES - 1) * sizeof(p->capture[0]));
        p->ncapture--;
    }
    p->capture[p->ncapture].end = p->next_frame_end - p->tpf;
    p->capture[p->ncapture].tag = tag;
    p->ncapture++;
}

void videorec_pipe_set_stereo(st_VIDEOREC_PIPE *p, bool stereo)
{
    videorec_audio_set_stereo(&p->audio, stereo); /* levné (pár kanálů), volá se jednou za horizont */
}

void videorec_pipe_set_next_index(st_VIDEOREC_PIPE *p, uint64_t index)
{
    p->next_index = index;
}

uint64_t videorec_pipe_next_index(const st_VIDEOREC_PIPE *p)
{
    return p->next_index;
}
