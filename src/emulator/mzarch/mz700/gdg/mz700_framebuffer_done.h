#ifndef MZ700_FRAMEBUFFER_DONE_H
#define MZ700_FRAMEBUFFER_DONE_H

#include <string.h>

#include <stdint.h>
#include "hw-generic/gdg/video.h"
#include "iface/iface_video.h"

static inline void framebuffer_screen_done(void)
{
    iface_video_framebuffer_screen_done(g_framebuffer.pixels);
    g_framebuffer.pixels_id = (g_framebuffer.pixels_id + 1) % GDG_FRAMEBUFFER_PIXBUF_COUNT;
    uint8_t *pixels_old = g_framebuffer.pixels;
    g_framebuffer.pixels = g_framebuffer.pixbuff[g_framebuffer.pixels_id];
    /* Kopiruje se skutecna display plocha teto architektury, ne superset. */
    memcpy(g_framebuffer.pixels, pixels_old, VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT);
    g_framebuffer.framebuffer_state = FB_STATE_NOT_CHANGED;
}

#endif // MZ700_FRAMEBUFFER_DONE_H