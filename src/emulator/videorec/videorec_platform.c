/**
 * @file   videorec_platform.c
 * @brief  Kontrola parametrů platformy video záznamu (viz videorec_platform.h).
 *
 * @par Licence: GPLv3
 */

#include "videorec_platform.h"

#include <stdarg.h>
#include <stdio.h>

/**
 * @brief Zapíše anglický popis chyby do bufferu volajícího (printf formát, zkrácení na `size`).
 * @param err  Buffer, nebo NULL (pak nic).
 * @param size Velikost bufferu (0 = nic).
 * @param fmt  Formát.
 * @return Vždy false (pro `return vr_err(...)`).
 */
static bool vr_err(char *err, size_t size, const char *fmt, ...)
{
    if (err && size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, size, fmt, ap);
        va_end(ap);
    }
    return false;
}

bool videorec_platform_check(const st_VIDEOREC_PLATFORM *p, unsigned audio_rate, char *err, size_t err_size)
{
    if (p->fps_num == 0 || p->fps_den == 0) return vr_err(err, err_size, "Invalid frame rate %u/%u", p->fps_num, p->fps_den);
    if (p->ticks_per_frame == 0 || p->ticks_per_frame * p->fps_num != p->clk_hz * p->fps_den) {
        return vr_err(err, err_size, "Frame length %llu ticks does not match clock %llu Hz at %u/%u fps",
                      (unsigned long long)p->ticks_per_frame, (unsigned long long)p->clk_hz, p->fps_num, p->fps_den);
    }
    if (p->fb_width == 0 || p->fb_height == 0 || p->canvas_w == 0 || p->canvas_h == 0 ||
        p->canvas_x + p->canvas_w > p->fb_width || p->canvas_y + p->canvas_h > p->fb_height) {
        return vr_err(err, err_size, "Canvas %ux%u+%u+%u does not fit into framebuffer %ux%u", p->canvas_w, p->canvas_h,
                      p->canvas_x, p->canvas_y, p->fb_width, p->fb_height);
    }
    if (p->audio_channels != 1 + 4 * p->psg_count) {
        return vr_err(err, err_size, "Audio channel count %u does not match %u PSG", p->audio_channels, p->psg_count);
    }
    if (audio_rate && ((uint64_t)audio_rate * p->fps_den) % p->fps_num != 0) {
        return vr_err(err, err_size, "Audio rate %u Hz does not give a whole number of samples per frame at %u/%u fps",
                      audio_rate, p->fps_num, p->fps_den);
    }
    return true;
}

unsigned videorec_platform_samples_per_frame(const st_VIDEOREC_PLATFORM *p, unsigned audio_rate)
{
    return (unsigned)(((uint64_t)audio_rate * p->fps_den) / p->fps_num);
}
