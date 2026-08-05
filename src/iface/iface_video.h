#ifndef IFACE_VIDEO_H
#define IFACE_VIDEO_H

#include "main.h"
#include <glib.h>
#include "app/app_thread.h"
#include "hw-generic/gdg/framebuffer_state.h"
#include "emulator.h"
#include "display.h"
#include "emulator/emulator_measuring.h"

typedef struct iface_video_callbacks_t
{
    gboolean (*init)(void);
    void (*exit)(void);
    void (*set_colors)(uint32_t *colormap);
    void (*set_window_focus)(void);
    void (*fix_window_aspect_ratio)(char correction_by);
    void (*set_window_size_by_scale)(float scale);
    /* Vycentruje hlavní okno na aktuálním displeji (one-shot při startu). */
    void (*set_window_centered)(void);
    void (*set_framerate_mode)(en_DISPLAY_FRAMERATE_MODE mode, int custom_fps);
    void (*set_fullscreen)(bool enabled);
    void (*switch_fullscreen)(void);
    void (*reset_hdr)(void);
} iface_video_callbacks_t;

extern iface_video_callbacks_t *g_iface_video_callbacks;

#define IFACE_VIDEO_UPDATE_WINDOW_TITLE_INTERVAL_MS 1000

typedef struct iface_video_t
{
    // mz800_main_thread pri startu ceka na inicializaci video rozhrani
    app_mutex_t *is_initialized_mutex;
    app_cond_t *is_initialized_cond;
    bool is_initialized;

    int32_t last_wsizeX;
    int32_t last_wsizeY;

    app_rwlock_t *redraw_full_screen_request_rwlock;
    bool redraw_full_screen_request; // Priznak, ze je potreba prekreslit cely obrazek

    app_mutex_t *fbsnapshot_pixels_mutex;
    app_cond_t *fbsnapshot_pixels_cond;

    uint32_t fbsnapshot_screen_id;          // cislo snimku z framebufferu
    uint8_t *fbsnapshot_pixels;          // Pointer na framebuffer pripraveny k vykresleni
    bool fbsnapshot_force_redraw_request; // Priznak, ze je potreba prekreslit cely obrazek (snapnuty s konkretnim framebfferem)
    en_FBSTATE fbsnapshot_framebuffer_state;

    uint32_t renderer_screen_id;

    st_EMULATOR_MEASURING_GDG msgdg;
} iface_video_t;

extern iface_video_t *g_iface_video;

static inline void iface_video_set_redraw_full_screen_request(bool redraw_flag)
{
    APP_RWLOCK_WRLOCK(g_iface_video->redraw_full_screen_request_rwlock);
    g_iface_video->redraw_full_screen_request = redraw_flag;
    APP_RWLOCK_WRUNLOCK(g_iface_video->redraw_full_screen_request_rwlock);
}

#define iface_video_create_redraw_full_screen_request() iface_video_set_redraw_full_screen_request(true)
#define iface_video_remove_redraw_full_screen_request() iface_video_set_redraw_full_screen_request(false)

static inline bool iface_video_get_redraw_full_screen_request(void)
{
    bool ret;
    APP_RWLOCK_RDLOCK(g_iface_video->redraw_full_screen_request_rwlock);
    ret = g_iface_video->redraw_full_screen_request;
    APP_RWLOCK_RDUNLOCK(g_iface_video->redraw_full_screen_request_rwlock);
    return ret;
}

static inline void iface_video_framebuffer_screen_done(uint8_t *pixels)
{
#if 0
    while (1)
    {
        APP_MUTEX_LOCK(g_iface_video->fbsnapshot_pixels_mutex);
        while ((g_iface_video->fbsnapshot_pixels != NULL) && APP_COND_WAIT_TIMEOUT_MS(g_iface_video->fbsnapshot_pixels_cond, g_iface_video->fbsnapshot_pixels_mutex, 20) == 0)
            continue;
        if (g_iface_video->fbsnapshot_pixels == NULL)
        {
            g_iface_video->fbsnapshot_pixels = pixels;
            g_iface_video->fbsnapshot_full_screen_request = iface_video_get_redraw_full_screen_request();
            g_iface_video->fbsnapshot_framebuffer_state = g_framebuffer.framebuffer_state;
            iface_video_remove_redraw_full_screen_request();
            APP_COND_SIGNAL(g_iface_video->fbsnapshot_pixels_cond);
            APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);
            break;
        }
        else
        {
            APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);
            if (main_app_get_exit_request(g_main_app))
                emulator_quit(EXIT_SUCCESS);
            // fprintf(stderr, "%s():%d - framebuffer screen done, but previous framebuffer screen not processed yet\n", __FUNCTION__, __LINE__);
        };
    };
#else
    APP_MUTEX_LOCK(g_iface_video->fbsnapshot_pixels_mutex);
    g_iface_video->fbsnapshot_screen_id++;
    g_iface_video->fbsnapshot_pixels = pixels;
    g_iface_video->fbsnapshot_force_redraw_request = iface_video_get_redraw_full_screen_request();
    g_iface_video->fbsnapshot_framebuffer_state = g_framebuffer.framebuffer_state;
    iface_video_remove_redraw_full_screen_request();
    APP_COND_SIGNAL(g_iface_video->fbsnapshot_pixels_cond);
    APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);
#endif
}

#ifdef __cplusplus
extern "C"
{
#endif
    /***************************************************
     *
     * Verejne funkce obsazene v iface_video.c
     *
     */
    extern bool iface_video_init(void);
    extern void iface_video_exit(void);

    extern char *iface_video_create_window_title_text(void);

#ifdef __cplusplus
}
#endif

#endif // IFACE_VIDEO_H
