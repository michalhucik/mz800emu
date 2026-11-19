#include "main.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#include "iface/iface_video.h"

#include "emulator/emulator_measuring.h"
#include "mzarch/mzarch.h"
#include "customspeed.h"
#include "emulator/mzarch/mzarch_platform.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "emulator/debugger/trace/cputrack.h"
#include "emulator/debugger/trace/iorqlog.h"
#include "emulator/debugger/trace/intlog.h"
#include "emulator/debugger/trace/hwlog.h"
#endif

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "emulator/mcp/tcp_server.h"
#endif

struct iface_video_t *g_iface_video = NULL;

#define IFACE_VIDEO_TITLE_SHOW_PERCENTAGE_SPEED 1
#define IFACE_VIDEO_TITLE_SHOW_FPS 1

bool iface_video_init(void)
{
    if (g_iface_video)
    {
        fprintf(stderr, "%s():%d - Interface already initialized!\n", __func__, __LINE__);
        return false;
    };

    g_iface_video = g_new0(iface_video_t, 1);
    if (!g_iface_video)
    {
        fprintf(stderr, "%s():%d - Could not allocate memory!\n", __func__, __LINE__);
        return false;
    };

    g_iface_video->is_initialized = false;

    APP_MUTEX_CREATE(g_iface_video->is_initialized_mutex);
    if (!g_iface_video->is_initialized_mutex)
    {
        fprintf(stderr, "%s():%d - Could not create Mutex!\n", __func__, __LINE__);
        return false;
    };

    APP_COND_CREATE(g_iface_video->is_initialized_cond);
    if (!g_iface_video->is_initialized_cond)
    {
        fprintf(stderr, "%s():%d - Could not create Condition!\n", __func__, __LINE__);
        return false;
    };

    APP_RWLOCK_CREATE(g_iface_video->redraw_full_screen_request_rwlock);
    if (!g_iface_video->redraw_full_screen_request_rwlock)
    {
        fprintf(stderr, "%s():%d - Could not create RWLock!\n", __func__, __LINE__);
        return false;
    };

    APP_MUTEX_CREATE(g_iface_video->fbsnapshot_pixels_mutex);
    if (!g_iface_video->fbsnapshot_pixels_mutex)
    {
        fprintf(stderr, "%s():%d - Could not create Mutex!\n", __func__, __LINE__);
        return false;
    };

    APP_COND_CREATE(g_iface_video->fbsnapshot_pixels_cond);
    if (!g_iface_video->fbsnapshot_pixels_cond)
    {
        fprintf(stderr, "%s():%d - Could not create Condition!\n", __func__, __LINE__);
        return false;
    };

    g_iface_video->fbsnapshot_pixels = NULL;
    iface_video_create_redraw_full_screen_request();

    if (!g_iface_video_callbacks)
    {
        fprintf(stderr, "%s():%d - No interface callbacks!\n", __func__, __LINE__);
        return false;
    };

    if (!g_iface_video_callbacks->init)
    {
        WARN("Init callback is not set!\n");
        return true;
    };

    st_EMULATOR_MEASURING_GDG *msgdg = &g_iface_video->msgdg;
    emulator_measuring_gdg_init(msgdg);
    msgdg->enabled = false;
    msgdg->console_output_enabled = false;

    return g_iface_video_callbacks->init();
}

void iface_video_exit(void)
{
    if (!g_iface_video)
        return;

    if (g_iface_video->redraw_full_screen_request_rwlock)
    {
        APP_RWLOCK_DESTROY(g_iface_video->redraw_full_screen_request_rwlock);
    };

    if (g_iface_video->fbsnapshot_pixels_mutex)
        APP_MUTEX_DESTROY(g_iface_video->fbsnapshot_pixels_mutex);

    if (g_iface_video->fbsnapshot_pixels_cond)
        APP_COND_DESTROY(g_iface_video->fbsnapshot_pixels_cond);

    if (g_iface_video->is_initialized_mutex)
        APP_MUTEX_DESTROY(g_iface_video->is_initialized_mutex);

    if (g_iface_video->is_initialized_cond)
        APP_COND_DESTROY(g_iface_video->is_initialized_cond);

    st_EMULATOR_MEASURING_GDG *msgdg = &g_iface_video->msgdg;
    emulator_measuring_gdg_exit(msgdg);

    g_free(g_iface_video);
    g_iface_video = NULL;

    if (g_iface_video_callbacks->exit)
    {
        g_iface_video_callbacks->exit();
    }
    else
    {
        WARN("Exit callback is not set!\n");
    };
}

/* Doxygen viz iface_video.h. */
char *iface_video_create_window_title_text(void)
{
    st_EMULATOR_MEASURING_GDG *msgdg = &g_iface_video->msgdg;

    emulator_measuring_gdg_event(msgdg);

    GString *status = g_string_new(NULL);
    g_string_append_printf(status, "%s - %s", g_mzarch_full_name, emulator_get_speed_status_as_text());

    if (!EMULATOR_TEST_PAUSED)
    {
        if (IFACE_VIDEO_TITLE_SHOW_PERCENTAGE_SPEED)
        {
            g_string_append_printf(status, ": %.2f %%", msgdg->current_gdg_frequency_percent);
        };

        if (IFACE_VIDEO_TITLE_SHOW_FPS)
        {
            g_string_append_printf(status, ", FB-FPS: %.2f", msgdg->current_fps);
        };
    };

    /* Ztlumený poslech: značka za stavem rychlosti, za běhu i při pauze. Jedno čtení příznaku při periodické
     * obnově titulku (IFACE_VIDEO_UPDATE_WINDOW_TITLE_INTERVAL_MS), horké cesty emulace se netýká. */
    const char *mute_text = emulator_get_mute_status_as_text();
    if (mute_text)
    {
        g_string_append_printf(status, ", %s", mute_text);
    };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* Trace-suite indikátor v titulku okna.
     * Vypisuje se pouze pokud je alespoň jeden ze 4 trace subsystémů
     * aktivně recordující (mode != OFF a podmínky aktivace splněny).
     * Při všech vypnutých subsystémech se titulek nemění (žádné
     * "[trace:]" prázdné značky). Suffix '!' = subsystém dosáhl
     * max_total_mb limitu a recording byl zastaven. */
    if ( g_cputrack_active || g_iorqlog_active || g_intlog_active || g_hwlog_active )
    {
        g_string_append ( status, " [trace:" );
        if ( g_cputrack_active ) g_string_append_printf ( status, " C%s", cputrack_is_truncated ( ) ? "!" : "" );
        if ( g_iorqlog_active )  g_string_append_printf ( status, " I%s", iorqlog_is_truncated ( ) ? "!" : "" );
        if ( g_intlog_active )   g_string_append_printf ( status, " N%s", intlog_is_truncated ( ) ? "!" : "" );
        if ( g_hwlog_active )    g_string_append_printf ( status, " H%s", hwlog_is_truncated ( ) ? "!" : "" );
        g_string_append ( status, "]" );
    }
#endif

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    /* V0.A.5: indikátor TCP listeneru v titulku. Zobrazujeme jen pokud
     * server skutečně běží - default čistý titulek se zachovává.
     * Formát: " [MCP:PORT, N clients]"
     */
    if ( g_mcp_tcp_server && mcp_tcp_server_is_running ( g_mcp_tcp_server ) )
    {
        g_string_append_printf ( status, " [MCP:%d, %d clients]",
            mcp_tcp_server_get_port ( g_mcp_tcp_server ),
            mcp_tcp_server_get_client_count ( g_mcp_tcp_server ) );
    }
#endif

    return g_string_free(status, FALSE);
}
