#include "main.h"
#ifdef WINDOWS
#include <windows.h>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "libs/imgui/imgui.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/bootstrap/images.h"
#include "iface/iface_video.h"
#include "emulator/display.h"
#include "iface-video/sdl3/iface_video_sdl3.h"
#include "iface-video/sdl3/iface_sdl3_events.h"
#include "ui-imgui/imgui_windows.h"
#include "emulator/cfgmain.h"
#include "mzarch/mzarch_config.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "ui-imgui/debugger/eventview/event_viewer_window.h"
#include "ui-imgui/debugger/profiler/profiler_window.h"
#include "ui-imgui/debugger/membrowser/membrowser_window.h"
#endif

// #include "imgui_impl_sdl3.h"
// #include "imgui_impl_sdlrenderer3.h"
// #include "imgui_impl_opengl3.h"
// #include <stdio.h>
// #if defined(IMGUI_IMPL_OPENGL_ES2)
// #include <SDL3/SDL_opengles2.h>
// #else
// #endif

//#define IFACE_WINDOW_SCALE DISPLAY_DEFAULT_SCALE
#define IFACE_WINDOW_SCALE 1.0f

#define IFACE_WINDOW_WIDTH VIDEO_DISPLAY_WIDTH
#define IFACE_WINDOW_HEIGHT (VIDEO_DISPLAY_HEIGHT * 2)
#define IFACE_SCREEN_ASPECT_RATIO ((float)IFACE_WINDOW_WIDTH / IFACE_WINDOW_HEIGHT)

typedef struct SdlappMyImGuiVideo_t
{
    SDL_Surface *surface;
    guint update_title_timer; /**< sdlapp timer id periodické aktualizace titulku okna (0 = neběží, headless) */
    Uint64 lastPictureTime;
    int TARGET_FPS;
    int FRAME_TIME;
    int raise_pending_frames; /* počet framů do pokusu o raise okna */
} SdlappMyImGuiVideo_t;

SdlappMyImGuiVideo_t *g_sdlapp_myimgui_video = NULL;

MyImGui *g_gui = NULL;

extern "C"
{
    void iface_sdl3_video_window_resize_event_handler(SdlAppWindow *win, SDL_Event *event);
};

static inline void sdlapp_myimgui_video_reset_fps_timer(void)
{
    g_sdlapp_myimgui_video->lastPictureTime = SDL_GetTicks();
    g_sdlapp_myimgui_video->TARGET_FPS = (g_display.custom_fps) ? g_display.custom_fps : 100;
    g_sdlapp_myimgui_video->FRAME_TIME = 1000 / g_sdlapp_myimgui_video->TARGET_FPS;
}

#ifdef WINDOWS
/**
 * Vynutí foreground focus pro SDL okno na Windows.
 * Používá AttachThreadInput trik k obejití Windows omezení na SetForegroundWindow.
 */
static void sdlapp_win32_force_foreground(SDL_Window *sdl_window)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd)
        return;
    DWORD foreground_tid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD our_tid = GetCurrentThreadId();
    if (foreground_tid != our_tid)
    {
        AttachThreadInput(foreground_tid, our_tid, TRUE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        AttachThreadInput(foreground_tid, our_tid, FALSE);
    }
    else
    {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
}
#endif

static void video_sdlapp_imgui_render_cb(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    // SDL_Window *window = win->sdl_window;
    // SDL_Renderer *renderer = win->sdl_renderer;
    SDL_Surface *surface = g_sdlapp_myimgui_video->surface;

    /* Odložený raise okna po startu hlavní smyčky */
    if (g_sdlapp_myimgui_video->raise_pending_frames > 0)
    {
        g_sdlapp_myimgui_video->raise_pending_frames--;
        if (g_sdlapp_myimgui_video->raise_pending_frames == 0)
        {
            SDL_RaiseWindow(win->sdl_window);
#ifdef WINDOWS
            sdlapp_win32_force_foreground(win->sdl_window);
#endif
            ImGui::SetWindowFocus("##MainEmulatorWindow");
        }
    }

    gboolean have_new_screen = video_sdl3_update_surface_from_framebuffer(surface);

#if 0 // SDL_RENDERER_SDL
    video_sdl3_render_surface_as_background(surface, renderer, IFACE_SCREEN_ASPECT_RATIO, have_new_screen);
#endif

    static GLuint imageTexture = 0;
    if ((have_new_screen) || (DISPLAY_TEST_FORCED_FULL_SCREEN_REDRAWING))
    {
        // g_print("Redrawing screen fb: %u, ren_id: %u\n", g_iface_video->fbsnapshot_screen_id, g_iface_video->renderer_screen_id);
        if (imageTexture)
            glDeleteTextures(1, &imageTexture);
        imageTexture = SDL_SurfaceToTexture(surface);
    };

    imgui_main_window(imageTexture);

    // TODO: chtelo by to merit presneji a az po vyrenderovani obrazu
    // Pokud je nastaveno Custom FPS, tak pockame pozadovanou dobu
    if (DISPLAY_TEST_FRAMERATE_MODE_CUSTOM)
    {
        Uint64 currentTime = SDL_GetTicks();
        int deltaTime = currentTime - g_sdlapp_myimgui_video->lastPictureTime;

        if (deltaTime < g_sdlapp_myimgui_video->FRAME_TIME)
        {
            SDL_Delay(g_sdlapp_myimgui_video->FRAME_TIME - deltaTime);
        };

        sdlapp_myimgui_video_reset_fps_timer();
    };
}

static void sdlapp_myimgui_video_set_colormap(uint32_t *colormap)
{
    video_sdl3_set_surface_colormap(colormap, g_sdlapp_myimgui_video->surface);
}

static SDL_Window *getSDL_Window_by_name(const char *name);

/**
 * @brief Timer callback (hlavní vlákno) - aktualizuje titulek hlavního okna.
 *
 * Registrován přes @ref sdlapp_add_timer, takže běží v hlavním vlákně -
 * SDL timer vlákno jen pushne event do fronty hlavní smyčky.
 * @c SDL_SetWindowTitle se smí volat pouze z hlavního vlákna; na macOS
 * (Cocoa) volání z timer vlákna končí pádem na neošetřené
 * NSInternalInconsistencyException (GitHub issue #2).
 *
 * Vedlejší efekt: @ref iface_video_create_window_title_text uvnitř
 * aktualizuje měření GDG (@c emulator_measuring_gdg_event) - i tato
 * mutace tedy probíhá v hlavním vlákně.
 *
 * @param timer_id Identifikátor sdlapp timeru (nepoužito).
 * @param user_data Nepoužito - okno se resolvuje podle jména, aby callback
 *                  nedržel potenciálně dangling ukazatel.
 * @return TRUE - timer pokračuje.
 */
static gboolean sdlapp_mygui_video_update_status_line(guint timer_id, gpointer user_data)
{
    (void)timer_id;
    (void)user_data;
    SDL_Window *window = getSDL_Window_by_name("main_window");
    if (!window)
        return TRUE;
    char *title_text = iface_video_create_window_title_text();
    SDL_SetWindowTitle(window, title_text);
    g_free(title_text);
    return TRUE;
}

static gboolean sdlapp_myimgui_video_init(void)
{
    /* Headless režim: žádné SDL okno se nevytváří, žádný ImGui kontext.
     * Alokujeme:
     *  - SdlappMyImGuiVideo_t + SDL surface (= cíl @c set_colors callbacku
     *    z display.c, který běží v emu vlákně a očekává existující surface).
     *  - Stub @c MyImGui struct (g_gui) zaplněný nulami. Důvod: 400+ míst
     *    v kódu emulátoru i UI cest čte @c g_gui->showXxxWindow flagy
     *    (viz např. @c imgui_cmt_tape_update_filelist volané z @c cmt_init
     *    v emu vlákně). NULL deref by emu vlákno spadl. Stub všech polí
     *    na nulu = všechny @c show* flagy false = UI cesty no-op.
     *
     * SDL surface lze vytvořit i bez SDL_INIT_VIDEO (SDL_CreateSurface
     * alokuje jen paměťový buffer), ale @ref sdl3_backend_video_init
     * stejně potřebujeme pro hlavní smyčku @ref sdlapp_run
     * (SDL_PollEvent vyžaduje SDL_INIT_VIDEO + SDL_INIT_EVENTS). */
    if (sdlapp_option_present("--headless"))
    {
        g_print("Headless mode: skipping main window + ImGui creation (no-op)\n");

        sdl3_backend_video_init();

        SdlappMyImGuiVideo_t *video = g_new0(SdlappMyImGuiVideo_t, 1);
        if (!video)
        {
            SDLAPP_ERROR("Failed to allocate memory for SdlappMyImGuiVideo_t (headless)");
            return false;
        };

        SDL_Surface *surface = video_sdl3_create_surface();
        if (!surface)
        {
            SDLAPP_ERROR("Could not create surface (headless)! SDL_Error: %s\n", SDL_GetError());
            g_free(video);
            return false;
        };

        video->surface = surface;
        video->raise_pending_frames = 0;
        video->update_title_timer = 0;
        g_sdlapp_myimgui_video = video;

        /* Stub g_gui - all-zero MyImGui pro NULL-safety v emu init pipeline.
         * Žádné ImGui context, žádné okno, žádné callbacks. Emu kód, který
         * jen čte show* flagy nebo MainWindowHasFocus, dostane 0 = false. */
        g_gui = g_new0(MyImGui, 1);
        if (!g_gui)
        {
            SDLAPP_ERROR("Failed to allocate stub MyImGui (headless)");
            SDL_DestroySurface(surface);
            g_free(video);
            g_sdlapp_myimgui_video = NULL;
            return false;
        };

        sdlapp_myimgui_video_set_colormap(g_display_predef_colors);

        /* @ref sdlapp_myimgui_video_reset_fps_timer čte @c g_display.custom_fps,
         * což je bezpečné - cfgmain už proběhl. FRAME_TIME se v headless
         * nepoužije (žádný render callback). */
        sdlapp_myimgui_video_reset_fps_timer();

        g_iface_video->last_wsizeX = IFACE_WINDOW_WIDTH;
        g_iface_video->last_wsizeY = IFACE_WINDOW_HEIGHT;

        return true;
    };

    // sdl3_backend_video_init();
    sdl3_backend_video_opengl_init();

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    // SdlAppRendererType renderer = SDLAPP_RENDERER_SDL;
    SdlAppRendererType renderer = SDLAPP_RENDERER_OPENGL;

    SdlAppWindow *win = sdlapp_window_create(g_sdlapp->manager, "main_window", "Main window", IFACE_WINDOW_WIDTH, IFACE_WINDOW_HEIGHT, flags, renderer, myimgui_init_cb, NULL);
    if (!win)
    {
        SDLAPP_ERROR("Failed to create window: %s", SDL_GetError());
        return false;
    };

    g_iface_video->last_wsizeX = IFACE_WINDOW_WIDTH;
    g_iface_video->last_wsizeY = IFACE_WINDOW_HEIGHT;

    MyImGui *gui = (MyImGui *)sdlapp_window_get_gui_data(win);
    if (!gui)
    {
        SDLAPP_ERROR("Failed to get GUI data for window: %s", win->name);
        sdlapp_window_destroy(win->manager, win);
        return false;
    };
    g_gui = gui;
    myimgui_set_event_cb(gui, iface_sdl3_events_cb);
    myimgui_set_render_cb(gui, video_sdlapp_imgui_render_cb);

    /* Aplikuj persistované UI flagy z cfg sekcí, které vyžadují
     * existující g_gui (= byly zaregistrované v debugger_init() PŘED
     * vznikem g_gui, jejich hodnoty čekají v file-static cache). */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    event_viewer_window_apply_persisted();
    profiler_window_apply_persisted();
    /* V0-leftovers F4: propagate CLI flag --memory-browser do
     * g_gui->showMemoryBrowserWindow (po vytvoření g_gui). */
    membrowser_window_apply_persisted();
#endif

    // Pri prvnim spusteni zobrazime okno s informacemi o programu.
    // CLI flag --no-first-run-windows toto chování potlačí (= headless
    // nebo scripted launch).
    if (!g_cfgmain_ini_file_exists
        && !sdlapp_option_present("--no-first-run-windows"))
        gui->showAboutWindow = true;

#if 0 // SDL_RENDERER_SDL
    myimgui_set_bg_clean(gui, false);
#endif

    SdlappMyImGuiVideo_t *video = g_new0(SdlappMyImGuiVideo_t, 1);
    if (!video)
    {
        SDLAPP_ERROR("Failed to allocate memory for SdlappMyImGuiVideo_t");
        sdlapp_window_destroy(win->manager, win);
        return false;
    };

    // Vytvoření surface
    SDL_Surface *surface = video_sdl3_create_surface();
    if (!surface)
    {
        SDLAPP_ERROR("Could not create surface! SDL_Error: %s\n", SDL_GetError());
        sdlapp_window_destroy(win->manager, win);
        g_free(video);
        return false;
    };

    video->surface = surface;
    video->raise_pending_frames = 3;
    g_sdlapp_myimgui_video = video;
    sdlapp_myimgui_video_set_colormap(g_display_predef_colors);
    // Odložíme raise okna - provedeme po vykreslení prvních framů
    // (SDL_RaiseWindow volaný před startem hlavní smyčky na Windows nefunguje,
    // protože SetForegroundWindow selže když foreground drží konzole)

    /* Periodická aktualizace titulku okna. sdlapp timer doručí callback
     * do hlavního vlákna (viz sdlapp_mygui_video_update_status_line). */
    g_sdlapp_myimgui_video->update_title_timer = sdlapp_add_timer(g_sdlapp, IFACE_VIDEO_UPDATE_WINDOW_TITLE_INTERVAL_MS, sdlapp_mygui_video_update_status_line, NULL);

    sdlapp_myimgui_video_reset_fps_timer();

    return imgui_images_load_all();
}

static void sdlapp_myimgui_video_exit(void)
{
    /* Headless režim: nealokovali jsme ImGui obrázky ani timer titulku,
     * proto cleanup omezujeme jen na surface + stub g_gui. Vyhneme se
     * @c imgui_images_destroy_all (= hlásilo by "Images not loaded"). */
    gboolean headless = sdlapp_option_present("--headless");

    if (g_sdlapp_myimgui_video)
    {
        if (g_sdlapp_myimgui_video->update_title_timer)
        {
            sdlapp_remove_timer(g_sdlapp, g_sdlapp_myimgui_video->update_title_timer);
        };

        if (g_sdlapp_myimgui_video->surface)
        {
            SDL_DestroySurface(g_sdlapp_myimgui_video->surface);
        };

        g_free(g_sdlapp_myimgui_video);
        g_sdlapp_myimgui_video = NULL;
    };
    if (headless && g_gui)
    {
        /* Stub g_gui byl alokovaný v init headless větvi - free, ne destroy
         * (žádný ImGui kontext k destroynutí). */
        g_free(g_gui);
    };
    g_gui = NULL;
    sdl3_backend_video_quit();
    if (!headless)
    {
        imgui_images_destroy_all();
    };
}

static SDL_Window *getSDL_Window_by_name(const char *name)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_sdlapp);
    if (!wm)
    {
        SDLAPP_ERROR("Failed to get window manager");
        return NULL;
    };
    SdlAppWindow *win = sdlapp_winmanager_get_window_by_name(wm, name);
    if (!win)
    {
        SDLAPP_ERROR("Failed to get window: %s", name);
        return NULL;
    };
    if (!win->sdl_window)
    {
        SDLAPP_ERROR("Failed to get SDL window: %s", name);
        return NULL;
    };
    return win->sdl_window;
}

/*******************************************************************************
 *
 *
 *                  Callbacks for resize window event
 *                  =================================
 *
 *
 *******************************************************************************/

void iface_sdl3_video_window_resize_event_handler(SdlAppWindow *win, SDL_Event *event)
{

    if (event->type != SDL_EVENT_WINDOW_RESIZED)
    {
        SDLAPP_ERROR("Invalid event type: %d\n", event->type);
        return;
    };

    int32_t wsizeX = event->window.data1;
    int32_t wsizeY = event->window.data2;

    SDL_Window *window = win->sdl_window;

    int32_t w, h;
    if (!SDL_GetWindowSize(window, &w, &h))
    {
        SDLAPP_ERROR("Could not get window size: %s\n", SDL_GetError());
        return;
    };

    // pokud doslo ke zmene velikosti nejakeho openGL sub okna, tak neni co resit.
    if (!((w == wsizeX) && (h == wsizeY)))
        return;

    if (!DISPLAY_TEST_LOCKED_ASPECT_RATIO)
    {
        g_iface_video->last_wsizeX = wsizeX;
        g_iface_video->last_wsizeY = wsizeY;
        return;
    };

    float aspect_ratio = (float)wsizeX / (float)wsizeY;

    if (aspect_ratio != IFACE_SCREEN_ASPECT_RATIO)
    {
        if ((wsizeX < g_iface_video->last_wsizeX) &&
            (wsizeY == g_iface_video->last_wsizeY))
        {
            wsizeY = (1.f / IFACE_SCREEN_ASPECT_RATIO) * wsizeX;
        }
        else if ((wsizeY < g_iface_video->last_wsizeY) &&
                 (wsizeX == g_iface_video->last_wsizeX))
        {
            wsizeX = IFACE_SCREEN_ASPECT_RATIO * wsizeY;
        }
        else
        {
            if (aspect_ratio > IFACE_SCREEN_ASPECT_RATIO)
            {
                wsizeY = (1.f / IFACE_SCREEN_ASPECT_RATIO) * wsizeX;
            }
            else
            {
                wsizeX = IFACE_SCREEN_ASPECT_RATIO * wsizeY;
            };
        };

        g_iface_video->last_wsizeX = wsizeX;
        g_iface_video->last_wsizeY = wsizeY;

        if (!SDL_SetWindowSize(window, wsizeX, wsizeY))
        {
            fprintf(stderr, "%s():%d - Could not set window size: %s\n", __func__, __LINE__, SDL_GetError());
            return;
        };
    };
}

/*******************************************************************************
 *
 *
 *                  Callbacks for main window focus
 *                  ===============================
 *
 *
 *******************************************************************************/

static void sdlapp_set_main_window_focus(SdlAppWindow *win)
{
    SDL_Window *sdl_window = win->sdl_window;
    if (!sdl_window)
        return;
    if (!SDL_RaiseWindow(sdl_window))
    {
        SDLAPP_ERROR("Could not raise window: %s", SDL_GetError());
        return;
    };
    ImGui::SetWindowFocus("##MainEmulatorWindow");
}

static void sdlapp_imgui_video_set_main_window_focus_cb(SdlAppWindow *win, SDL_Event *event)
{
    (void)event;
    sdlapp_set_main_window_focus(win);
}

static void sdlapp_imgui_video_set_main_window_focus_request(void)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_set_main_window_focus_cb, NULL);
}

/*******************************************************************************
 *
 *
 *                  Callbacks for fix window aspect ratio
 *                  =====================================
 *
 *
 *******************************************************************************/

static void sdlapp_fix_window_aspect_ratio(SDL_Window *sdl_window, char correction_by)
{
    if (correction_by != 'W' && correction_by != 'H')
    {
        SDLAPP_ERROR("Invalid correction_by value: %c", correction_by);
        return;
    };

    int32_t wsizeX, wsizeY;
    if (!SDL_GetWindowSize(sdl_window, &wsizeX, &wsizeY))
    {
        SDLAPP_ERROR("Could not get window size: %s", SDL_GetError());
        return;
    };

    if (correction_by == 'W')
    {
        wsizeY = (1.f / IFACE_SCREEN_ASPECT_RATIO) * wsizeX;
    }
    else
    {
        wsizeX = IFACE_SCREEN_ASPECT_RATIO * wsizeY;
    };

    g_iface_video->last_wsizeX = wsizeX;
    g_iface_video->last_wsizeY = wsizeY;

    if (!SDL_SetWindowSize(sdl_window, wsizeX, wsizeY))
    {
        SDLAPP_ERROR("Could not set window size: %s", SDL_GetError());
        return;
    };

    iface_video_create_redraw_full_screen_request();
}

static void sdlapp_imgui_video_fix_window_aspect_ratio_cb(SdlAppWindow *win, SDL_Event *event)
{
    SDL_Window *sdl_window = win->sdl_window;
    char correction_by = (GPOINTER_TO_INT(event->user.data2)) & 0xFF;
    sdlapp_fix_window_aspect_ratio(sdl_window, correction_by);
}

static void sdlapp_imgui_video_fix_window_aspect_ratio_request(char correction_by)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_fix_window_aspect_ratio_cb, GUINT_TO_POINTER(correction_by));
}
/*******************************************************************************
 *
 *
 *                  Callbacks for Fullscreen mode
 *                  =============================
 *
 *
 *******************************************************************************/

#ifdef WINDOWS

#if 0
// vyzaduje LDFLAGS += -ldxgi -ldxguid
#include <dxgi1_6.h>
#include <wrl/client.h> // Pro Microsoft::WRL::ComPtr
using Microsoft::WRL::ComPtr;
 
static void sdlapp_imgui_video_DisableHDR()
{
     ComPtr<IDXGIFactory6> pFactory;
     if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory6, (void **)&pFactory)))
     {
         return;
     }
 
     ComPtr<IDXGIAdapter1> pAdapter;
     if (FAILED(pFactory->EnumAdapters1(0, &pAdapter)))
     {
         return;
     }
 
     ComPtr<IDXGIOutput> pOutput;
     if (FAILED(pAdapter->EnumOutputs(0, &pOutput)))
     {
         return;
     }
 
     DXGI_OUTPUT_DESC outputDesc;
     if (FAILED(pOutput->GetDesc(&outputDesc)))
     {
         return;
     }
 
     DEVMODEW devMode = {};
     devMode.dmSize = sizeof(DEVMODEW);
 
     if (EnumDisplaySettingsW(outputDesc.DeviceName, ENUM_CURRENT_SETTINGS, &devMode))
     {
         // Simulujeme změnu na jinou obnovovací frekvenci a pak ji vrátíme zpět
         devMode.dmDisplayFrequency = (devMode.dmDisplayFrequency == 60) ? 59 : 60;
         ChangeDisplaySettingsW(&devMode, CDS_FULLSCREEN);
 
         // Vrátíme zpět původní obnovovací frekvenci
         devMode.dmDisplayFrequency = (devMode.dmDisplayFrequency == 59) ? 60 : 59;
         ChangeDisplaySettingsW(&devMode, CDS_FULLSCREEN);
     }
 }
#endif

#if 1
static void sdlapp_imgui_video_DisableHDR()
{
    // Simulace změny rozlišení (hard reset)
    DEVMODE dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm))
    {
        ChangeDisplaySettings(&dm, CDS_RESET);
    };
}
#endif

#else // WINDOWS
static void sdlapp_imgui_video_DisableHDR()
{
    // V Linuxu to neni potreba
}
#endif

static void sdlapp_imgui_video_set_fullscreen_cb(SdlAppWindow *win, SDL_Event *event)
{
    int value = GPOINTER_TO_INT(event->user.data2);
    SDL_Window *sdl_window = win->sdl_window;
    SDL_SetWindowFullscreen(sdl_window, (value) ? true : false);
    if (!value)
    {
        SDL_RestoreWindow(sdl_window);
#ifdef WINDOWS
        if (g_display.windows_forced_reset_hdr_on_return_from_fullscreen)
            sdlapp_imgui_video_DisableHDR();
#endif
    };
    SDL_RaiseWindow(sdl_window);
}

static void sdlapp_imgui_video_switch_fullscreen_cb(SdlAppWindow *win, SDL_Event *event)
{
    (void)event;
    SDL_Window *sdl_window = win->sdl_window;
    Uint32 flags = SDL_GetWindowFlags(sdl_window);
    if (flags & SDL_WINDOW_FULLSCREEN)
    {
        SDL_SetWindowFullscreen(sdl_window, false);
        SDL_RestoreWindow(sdl_window);
        SDL_RaiseWindow(sdl_window);

#ifdef WINDOWS
        if (g_display.windows_forced_reset_hdr_on_return_from_fullscreen)
            sdlapp_imgui_video_DisableHDR();
#endif
    }
    else
    {
        SDL_SetWindowFullscreen(sdl_window, true);
    };
}

static void sdlapp_imgui_video_set_fullscreen(bool enabled)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    int value = (enabled) ? 1 : 0;
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_set_fullscreen_cb, GINT_TO_POINTER(value));
}

static void sdlapp_imgui_video_switch_fullscreen(void)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_switch_fullscreen_cb, NULL);
}

/*******************************************************************************
 *
 *
 *                  Callbacks for setting window size by scale
 *                  ==========================================
 *
 *
 *******************************************************************************/

// Nastavení velikosti okna podle zadaného měřítka
static void sdlapp_set_window_size_by_scale(SDL_Window *sdl_window, float scale)
{
    Uint32 flags = SDL_GetWindowFlags(sdl_window);
    if (flags & SDL_WINDOW_FULLSCREEN)
    {
        SDL_SetWindowFullscreen(sdl_window, 0);
#ifdef WINDOWS
        if (g_display.windows_forced_reset_hdr_on_return_from_fullscreen)
            sdlapp_imgui_video_DisableHDR();
#endif
    };

    int32_t w = IFACE_WINDOW_WIDTH * scale;
    int32_t h = IFACE_WINDOW_HEIGHT * scale;

    g_print("Setting window size to %d x %d, by scale: %f\n", w, h, scale);

    g_iface_video->last_wsizeX = w;
    g_iface_video->last_wsizeY = h;
    if (!SDL_SetWindowSize(sdl_window, w, h))
    {
        SDLAPP_ERROR("Could not set window size: %s", SDL_GetError());
        return;
    };

    iface_video_create_redraw_full_screen_request();
}

static void sdlapp_imgui_video_set_window_size_by_scale_cb(SdlAppWindow *win, SDL_Event *event)
{
    float *scale = (float *)event->user.data2;
    if (!scale)
    {
        SDLAPP_ERROR("Invalid scale value");
        return;
    };
    SDL_Window *sdl_window = win->sdl_window;
    sdlapp_set_window_size_by_scale(sdl_window, *scale);
    g_free(scale);
}

static void sdlapp_imgui_video_set_window_size_by_scale_request(float scale)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    float *scale_ptr = g_new0(float, 1);
    *scale_ptr = scale;
    if (!scale_ptr)
    {
        SDLAPP_ERROR("Failed to allocate memory for scale");
        return;
    };
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_set_window_size_by_scale_cb, scale_ptr);
}

/*******************************************************************************
 *
 *
 *                  Callback pro vycentrování hlavního okna
 *                  =======================================
 *
 *
 *******************************************************************************/

/**
 * @brief Callback (hlavní vlákno) - vycentruje hlavní okno na aktuálním displeji.
 *
 * Vznik okna proběhne v base velikosti (= scale Native) a okno se vycentruje
 * podle ní; startup velikost (Normal/Bigger/...) se aplikuje až později přes
 * @c set_window_size_by_scale, který mění jen velikost, ne pozici. Bez tohoto
 * re-centrování by okno pro scale != Native skončilo mimo střed. Pozici počítá
 * @c sdlapp_winmanager_good_place_for (jediné okno -> střed).
 *
 * @param win Hlavní okno.
 * @param event Nepoužito.
 */
static void sdlapp_imgui_video_center_main_window_cb(SdlAppWindow *win, SDL_Event *event)
{
    (void)event;
    if (!win || !win->sdl_window)
        return;
    int x = 0, y = 0;
    sdlapp_winmanager_good_place_for(sdlapp_window_get_manager(win), win, &x, &y);
    SDL_SetWindowPosition(win->sdl_window, x, y);
}

/**
 * @brief Požadavek (libovolné vlákno) na vycentrování hlavního okna.
 *
 * Zařadí @c sdlapp_imgui_video_center_main_window_cb do fronty zpráv okna.
 * Díky FIFO pořadí fronty se centrování provede až po případné předchozí
 * změně velikosti (resize), takže okno skončí vycentrované ve finální velikosti.
 */
static void sdlapp_imgui_video_center_main_window_request(void)
{
    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;
    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_center_main_window_cb, NULL);
}

/*******************************************************************************
 *
 *
 *                  Callbacks for setting framerate mode
 *                  ====================================
 *
 *
 *******************************************************************************/

static void sdlapp_imgui_video_send_gl_swapinterval_cb(SdlAppWindow *win, SDL_Event *event)
{
    (void)win;
    int value = GPOINTER_TO_INT(event->user.data2);
    video_sdl3_set_GL_swap_interval(value);
}

static void sdlapp_imgui_video_send_gl_swapinterval_request(int value)
{
    if (!(value >= -1 && value <= 1))
    {
        SDLAPP_ERROR("Invalid value for GL swap interval: %d", value);
        return;
    };

    SDL_Window *sdl_window = getSDL_Window_by_name("main_window");
    if (!sdl_window)
        return;

    sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(sdl_window), SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST, 1, (gpointer)sdlapp_imgui_video_send_gl_swapinterval_cb, GINT_TO_POINTER(value));
}

static void sdlapp_imgui_video_set_framerate_mode(en_DISPLAY_FRAMERATE_MODE mode, int custom_fps)
{
    if (mode > (DISPLAY_FRAMERATE_MODE_COUNT - 1))
    {
        mode = DISPLAY_DEFAULT_FRAMERATE_MODE;
    };
    g_display.framerate_mode = mode;

    if ((custom_fps < DISPLAY_CUSTOM_FPS_MIN) || (custom_fps > DISPLAY_CUSTOM_FPS_MAX))
    {
        if (g_display.framerate_mode != DISPLAY_FRAMERATE_MODE_CUSTOM)
        {
            WARN("Custom FPS out of range (%d - %d), setting to default value\n", DISPLAY_CUSTOM_FPS_MIN, DISPLAY_CUSTOM_FPS_MAX);
        };
        custom_fps = DISPLAY_CUSTOM_FPS_DEFAULT;
    };

    switch (mode)
    {
    case DISPLAY_FRAMERATE_MODE_IMMEDIATE:
        sdlapp_imgui_video_send_gl_swapinterval_request(0);
        break;
    case DISPLAY_FRAMERATE_MODE_VSYNC:
        sdlapp_imgui_video_send_gl_swapinterval_request(1);
        break;
    case DISPLAY_FRAMERATE_MODE_ADAPTIVE:
        sdlapp_imgui_video_send_gl_swapinterval_request(-1);
        break;
    case DISPLAY_FRAMERATE_MODE_CUSTOM:
        sdlapp_imgui_video_send_gl_swapinterval_request(0);
        break;
    case DISPLAY_FRAMERATE_MODE_SLAVE:
        sdlapp_imgui_video_send_gl_swapinterval_request(0);
        sdlapp_myimgui_video_reset_fps_timer();
        break;
    default:
        SDLAPP_ERROR("Unsupported framerate mode: %d", mode);
        break;
    };
}

/*******************************************************************************
 *
 *
 *                  Settings for Video Interface Callbacks
 *                  ======================================
 *
 *
 *******************************************************************************/

static iface_video_callbacks_t sdl3_iface_video_callbacks = {
    .init = sdlapp_myimgui_video_init,
    .exit = sdlapp_myimgui_video_exit,
    .set_colors = sdlapp_myimgui_video_set_colormap,
    .set_window_focus = sdlapp_imgui_video_set_main_window_focus_request,
    .fix_window_aspect_ratio = sdlapp_imgui_video_fix_window_aspect_ratio_request,
    .set_window_size_by_scale = sdlapp_imgui_video_set_window_size_by_scale_request,
    .set_window_centered = sdlapp_imgui_video_center_main_window_request,
    .set_framerate_mode = sdlapp_imgui_video_set_framerate_mode,
    .set_fullscreen = sdlapp_imgui_video_set_fullscreen,
    .switch_fullscreen = sdlapp_imgui_video_switch_fullscreen,
    .reset_hdr = sdlapp_imgui_video_DisableHDR,
};

iface_video_callbacks_t *g_iface_video_callbacks = &sdl3_iface_video_callbacks;
