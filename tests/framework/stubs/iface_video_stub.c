/*
 * iface_video_stub.c — stub implementace video rozhraní pro headless testy
 *
 * Nahrazuje reálný SDL3 video backend.
 * Všechny callbacky jsou no-op, init nastaví is_initialized=true.
 *
 * Licence: GPLv3
 */

#include "main.h"
#include <glib.h>
#include "iface/iface_video.h"
#include "app/app_thread.h"

/* definice globální proměnné — normálně je v iface_video_sdl3.c */
iface_video_callbacks_t *g_iface_video_callbacks = NULL;

/* stub callbacky */

static gboolean stub_video_init(void)
{
    /* nastavit is_initialized — emulátorový kód na to čeká */
    APP_MUTEX_LOCK(g_iface_video->is_initialized_mutex);
    g_iface_video->is_initialized = true;
    APP_COND_SIGNAL(g_iface_video->is_initialized_cond);
    APP_MUTEX_UNLOCK(g_iface_video->is_initialized_mutex);
    return TRUE;
}

static void stub_video_exit(void)
{
    /* nic — žádné SDL prostředky k uvolnění */
}

static void stub_video_set_colors(uint32_t *colormap)
{
    (void)colormap;
}

static void stub_video_set_window_focus(void)
{
    /* nic — žádné okno */
}

static void stub_video_fix_window_aspect_ratio(char correction_by)
{
    (void)correction_by;
}

static void stub_video_set_window_size_by_scale(float scale)
{
    (void)scale;
}

static void stub_video_set_framerate_mode(en_DISPLAY_FRAMERATE_MODE mode, int custom_fps)
{
    (void)mode;
    (void)custom_fps;
}

static void stub_video_set_fullscreen(bool enabled)
{
    (void)enabled;
}

static void stub_video_switch_fullscreen(void)
{
    /* nic */
}

/* dotaz na fullscreen: testovací okno ve fullscreenu není */
static bool stub_video_is_fullscreen(void)
{
    return false;
}

static void stub_video_reset_hdr(void)
{
    /* nic */
}

/* callback struktura */
static iface_video_callbacks_t stub_video_callbacks = {
    .init = stub_video_init,
    .exit = stub_video_exit,
    .set_colors = stub_video_set_colors,
    .set_window_focus = stub_video_set_window_focus,
    .fix_window_aspect_ratio = stub_video_fix_window_aspect_ratio,
    .set_window_size_by_scale = stub_video_set_window_size_by_scale,
    .set_framerate_mode = stub_video_set_framerate_mode,
    .set_fullscreen = stub_video_set_fullscreen,
    .switch_fullscreen = stub_video_switch_fullscreen,
    .is_fullscreen = stub_video_is_fullscreen,
    .reset_hdr = stub_video_reset_hdr,
};

/*
 * Nainstaluje stub video callbacky.
 * Musí se volat PŘED iface_video_init().
 */
void mztest_install_video_stubs(void)
{
    g_iface_video_callbacks = &stub_video_callbacks;
}
