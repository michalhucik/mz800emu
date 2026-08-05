#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <glib.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "emulator/mzarch/mzhal.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL3/SDL_opengles2.h>
#else
#include <SDL3/SDL_opengl.h>
#endif

#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "myimgui.h"
#include "ui-imgui/filechooser/res/CustomFont.cpp"
#include "ui-imgui/filechooser/res/CustomFont.h"

#include "main.h"  /* g_sdlapp */
extern "C" void imgui_filechooser_settings_init(void);

#include "mzarch/mzcommon_config.h"
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
/* MCP Activity okno (V1.C.2 mutant mcp-server) - init/shutdown ring
 * bufferu pro real-time log MCP akcí. Bootstrap UI vrstva si vlastní
 * lifecycle, vlastní render volá main_window.cpp. */
#include "ui-imgui/mcp_activity/mcp_activity_window.h"
#include "ui-imgui/mcp_activity/action_toast.h"
#endif

static ImVec4 default_bg_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

/* Monospace font - načten v myimgui_set_fonts() jako samostatný font
 * (NE merged) - takže ho lze přepnout přes PushFont() pro hex dumpy.
 * NULL pokud TTF soubor chybí (chytí to caller přes
 * myimgui_get_monospace_font() == NULL kontrolu). */
static ImFont *s_monospace_font = nullptr;

ImFont *myimgui_get_monospace_font(void)
{
    return s_monospace_font;
}

void myimgui_set_fonts(ImGuiIO &io)
{
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 3; /* Lepší vykreslování */
    font_cfg.OversampleV = 3;
    font_cfg.PixelSnapH = true;

    /* Rozsahy glyphů pro všechny podporované jazyky:
     * CS, SK, PL, DE, FR, ES, IT, NL — latinka + rozšíření
     * UK — cyrilice */
    static const ImWchar i18nRanges[] = {
        0x0020, 0x024F, /* Basic Latin + Latin Extended-A + Latin Extended-B */
        0x0400, 0x04FF, /* Cyrilice (ukrajinština) */
        0};

    /* Cesty k fontům resolvujeme proti home_dir. Z hlediska paměti řetězce
     * žijí dokud je ImGui nezpracuje (Build()) — drží se v lokálních proměnných
     * uvnitř bloků a uvolní se až poté. ImGui si interně cestu nepamatuje. */
    char *droid_path = sdlapp_paths_resolve_home(g_sdlapp->paths,
                                                  "ui_resources/imgui/fonts/DroidSans.ttf");
    io.Fonts->AddFontFromFileTTF(droid_path, 28.0f, &font_cfg, i18nRanges);
    g_free(droid_path);

    /* CJK font pro japonštinu — volitelný, pokud soubor existuje */
    {
        char *cjk_font_path = sdlapp_paths_resolve_home(g_sdlapp->paths,
                                                        "ui_resources/imgui/fonts/NotoSansJP-Regular.ttf");
        FILE *f = fopen(cjk_font_path, "rb");
        if (f)
        {
            fclose(f);
            ImFontConfig cjk_cfg;
            cjk_cfg.MergeMode = true;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(cjk_font_path, 28.0f, &cjk_cfg,
                                         io.Fonts->GetGlyphRangesJapanese());
        }
        g_free(cjk_font_path);
    }

    { /* Glyphs — filechooser ikony */
        static const ImWchar icons_ranges_3[] = {ICON_MIN_IGFD, ICON_MAX_IGFD, 0};
        ImFontConfig icons_config_3;
        icons_config_3.MergeMode = true;
        icons_config_3.PixelSnapH = true;
        io.Fonts->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_IGFD, 28.0f, &icons_config_3, icons_ranges_3);
    }

    { /* Glyphs - MZ specifické ikony + CG-ROM glyfy (Memory Browser).
       *
       * Rozsahy:
       *   U+E000        - kazetová páska a další UI ikony (původní use case)
       *   U+E100-U+E4FF - 4x 256 CG-ROM glyfů z mzglyphs lib:
       *                   E1xx=EU1, E2xx=EU2, E3xx=JP1, E4xx=JP2.
       *
       * Pokud font soubor chybí, ImGui pokračuje bez glyfů - na
       * relevantních místech (Memory Browser ASCII column) se zobrazí
       * replacement glyph. Žádný crash. */
        static const ImWchar icon_ranges[] = {
            0xE000, 0xE000,    /* UI ikony (kazeta apod.) */
            0xE100, 0xE4FF,    /* mzglyphs CG-ROM glyfy (4 charsety) */
            0
        };
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        char *mzglyphs_path = sdlapp_paths_resolve_home(g_sdlapp->paths,
                                                         "ui_resources/imgui/symbols/mzglyphs.ttf");
        io.Fonts->AddFontFromFileTTF(mzglyphs_path, 20.0f, &config, icon_ranges);
        g_free(mzglyphs_path);
    }

    /* Monospace font (Cousine-Regular) pro hex dumpy (Memory Browser
     * a podobné). Načten jako SAMOSTATNÝ font (NE merge) - lze ho
     * přepnout přes PushFont(). Merge mzglyphs nahoru zajišťuje, že
     * DISPLAY EU/JP glyfy v ASCII column fungují i v monospace módu.
     *
     * Velikost 28 px = stejná jako default UI font (DroidSans) aby
     * řádkování v okně zůstalo konzistentní. */
    {
        char *mono_path = sdlapp_paths_resolve_home(g_sdlapp->paths,
                                                     "ui_resources/imgui/fonts/Cousine-Regular.ttf");
        FILE *mf = fopen(mono_path, "rb");
        if (mf) {
            fclose(mf);
            ImFontConfig mono_cfg;
            mono_cfg.OversampleH = 2;
            mono_cfg.OversampleV = 2;
            mono_cfg.PixelSnapH = true;
            /* Pouze ASCII + Latin Extended - hex dump používá jen
             * 0-9 / A-F / běžné ASCII znaky. Pro národní znaky v ASCII
             * column ve Sharp encodingu by se musely přidat rozsahy.
             * V0-polish-2 záměrně omezeno - národní znaky se ukáží přes
             * fallback. */
            static const ImWchar mono_ranges[] = {
                0x0020, 0x024F,
                0
            };
            s_monospace_font = io.Fonts->AddFontFromFileTTF(mono_path, 28.0f,
                                                             &mono_cfg, mono_ranges);

            /* Merge mzglyphs nahoru aby DISPLAY EU/JP CG-ROM glyfy
             * fungovaly v ASCII column při monospace módu. */
            char *mzglyphs2_path = sdlapp_paths_resolve_home(g_sdlapp->paths,
                                                              "ui_resources/imgui/symbols/mzglyphs.ttf");
            FILE *gf = fopen(mzglyphs2_path, "rb");
            if (gf) {
                fclose(gf);
                static const ImWchar mono_icon_ranges[] = {
                    0xE000, 0xE000,
                    0xE100, 0xE4FF,
                    0
                };
                ImFontConfig mono_icon_cfg;
                mono_icon_cfg.MergeMode = true;
                mono_icon_cfg.PixelSnapH = true;
                io.Fonts->AddFontFromFileTTF(mzglyphs2_path, 20.0f,
                                              &mono_icon_cfg, mono_icon_ranges);
            }
            g_free(mzglyphs2_path);
        }
        g_free(mono_path);
    }
}

static gboolean myimgui_init_for_sdl3_sdlrenderer(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    if (!win)
        return FALSE;

    SDL_Window *sdl_window = win->sdl_window;
    if (!sdl_window)
    {
        SDLAPP_ERROR("Can't create SDL_Window: %s", SDL_GetError());
        return FALSE;
    };

    SDL_Renderer *sdl_renderer = win->sdl_renderer;
    if (!sdl_renderer)
    {
        SDLAPP_ERROR("Can't create SDL_Renderer: %s", SDL_GetError());
        return FALSE;
    };

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGuiContext *ctx = ImGui::CreateContext();
    if (!ctx)
    {
        SDLAPP_ERROR("Failed to create ImGui context");
        return false;
    };

    ImGui::SetCurrentContext(ctx);
    imgui_filechooser_settings_init();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.IniFilename = nullptr;                             // We don't want to save settings
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
    /* Docking ZÁMĚRNĚ VYPNUTO - okna debuggeru/HW panelů nejsou
     * navržena pro docking ergonomii (= různé sizing constraints, layout
     * persistence per-window, multi-viewport mode plovoucí okna). Pokud
     * by se v budoucnu chtělo docking opt-in pro vybrané okno, dělá se
     * to per-window přes ImGuiWindowFlags_None místo NoDocking. */
    // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-Viewport - není podporováno

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(sdl_window, sdl_renderer))
    {
        SDLAPP_ERROR("ImGui_ImplSDL3_InitForSDLRenderer failed!");
        return false;
    };

    if (!ImGui_ImplSDLRenderer3_Init(sdl_renderer))
    {
        SDLAPP_ERROR("ImGui_ImplSDLRenderer3_Init failed!");
        return false;
    };

    myimgui_set_fonts(io);

    return TRUE;
}

static gboolean myimgui_init_for_sdl3_opengl(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    if (!win)
        return FALSE;

    SDL_Window *sdl_window = win->sdl_window;
    if (!sdl_window)
    {
        SDLAPP_ERROR("Can't create SDL_Window: %s", SDL_GetError());
        return FALSE;
    };

    SDL_GLContext gl_context = win->gl_context;
    if (!gl_context)
    {
        SDLAPP_ERROR("Failed to get GL context");
        return false;
    };

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGuiContext *ctx = ImGui::CreateContext();
    if (!ctx)
    {
        SDLAPP_ERROR("Failed to create ImGui context");
        return false;
    };

    ImGui::SetCurrentContext(ctx);
    imgui_filechooser_settings_init();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    // io.IniFilename = nullptr;                             // We don't want to save settings
    /* IniFilename — ImGui si pointer drží během celého life cyklu, takže ho
     * udržujeme ve static stringu. CLI option --imgui-ini přebíjí default
     * (cfg-dir/mz<arch>emu-imgui.ini). */
    static char *s_imgui_ini_path = nullptr;
    if (!s_imgui_ini_path) {
        const char *opt_path = sdlapp_option_value("--imgui-ini");
        if (opt_path) {
            s_imgui_ini_path = g_strdup(opt_path);
        } else {
            /* Runtime z g_mzhal (mzhal krok 7) - jméno identické
             * s dřívějším compile-time MZARCH_NAME "emu-imgui.ini". */
            char ini_name[48];
            snprintf(ini_name, sizeof(ini_name), "%semu-imgui.ini",
                     g_mzhal.arch_name);
            s_imgui_ini_path = sdlapp_paths_resolve_cfg(
                g_sdlapp->paths, ini_name);
        }
    }
    io.IniFilename = s_imgui_ini_path;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
                                                          //    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewport / Platform Windows
    // io.ConfigViewportsNoAutoMerge = true;
    // io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // pro filechooser je lepsi pouzit tento styl
    // ApplyOrangeBlueTheme();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle &style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplSDL3_InitForOpenGL(sdl_window, gl_context))
    {
        SDLAPP_ERROR("ImGui_ImplSDL3_InitForOpenGL failed!");
        return false;
    };

// Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    const char *glsl_version = "#version 100";
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
    const char *glsl_version = "#version 300 es";
#elif defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    const char *glsl_version = "#version 150";
#else
    // GL 3.0 + GLSL 130
    const char *glsl_version = "#version 130";
#endif

    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
        SDLAPP_ERROR("ImGui_ImplOpenGL3_Init failed!");
        return false;
    };

    myimgui_set_fonts(io);

    return TRUE;
}

gboolean myimgui_init_cb(SdlAppWindow *win, gpointer user_data)
{
    if (!win)
    {
        SDLAPP_ERROR("Window is NULL");
        return false;
    }

    gboolean result = true;

    switch (win->renderer_type)
    {
    case SDLAPP_RENDERER_SDL:
        result = myimgui_init_for_sdl3_sdlrenderer(win, user_data);
        break;
    case SDLAPP_RENDERER_OPENGL:
        result = myimgui_init_for_sdl3_opengl(win, user_data);
        break;
    default:
        SDLAPP_ERROR("Unsupported renderer type: %d", win->renderer_type);
        result = false;
        break;
    };

    if (!result)
    {
        SDLAPP_ERROR("Failed to initialize ImGui for renderer type: %d", win->renderer_type);
        return false;
    };

    MyImGui *gui = g_new0(MyImGui, 1);
    if (!gui)
    {
        SDLAPP_ERROR("Failed to allocate memory for MyImGuiData");
        return false;
    };
    gui->app = win->manager->app;
    gui->win = win;
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    gui->ctxptr = ctx;

    gpointer old_gui_data = sdlapp_window_set_gui_data(win, gui);
    if (old_gui_data)
    {
        SDLAPP_ERROR("GUI data already set for window: %s", win->name);
        sdlapp_window_set_gui_data(win, old_gui_data);
        ImGui::DestroyContext(ctx);
        g_free(gui);
        return false;
    };

    gui->use_bg_color = true;
    gui->bg.r = default_bg_color.x;
    gui->bg.g = default_bg_color.y;
    gui->bg.b = default_bg_color.z;
    gui->bg.a = default_bg_color.w;

    gui->event_cb = NULL;
    gui->render_cb = NULL;

    /*
     * Default visibility flagy. Všechna okna startují closed (= user
     * je otevírá explicitně přes menu nebo zkratku). CPU Registers
     * se otevírá přes menu "Debugger -> CPU Registers" nebo Alt+R.
     * (V3.3 změna - Michal 2026-05-11: nezobrazovat automaticky.)
     */
    gui->showCpuWindow = false;
    gui->showStackWindow = false;
    /* V8: Stack Regions samostatné okno - default closed, user otevírá přes
     * menu Debugger -> Stack Regions nebo Alt+Shift+S. Persistence přes
     * [STACK_PANEL] sekci cfgmain.ini (viz dbg_stack_panel_cfg_init). */
    gui->showStackRegionsWindow = false;
    /* V9: Stack History samostatné okno - default closed, user otevírá přes
     * menu Debugger -> Stack History nebo Alt+Shift+H. Persistence přes
     * [STACK_PANEL] show_history_window. */
    gui->showStackHistoryWindow = false;
    /* Callstack (mutant callstack Fáze 2C): samostatné plovoucí okno se
     * shadow stackem volání. Default closed, user otevírá přes menu
     * Debugger -> Callstack. */
    gui->showCallstackWindow = false;

    /* Per-chip-panels F1 scaffold: per-chip detail panely (CTC 8253,
     * PPI 8255, Z80 PIO, PSG SN76489). Default closed, user otevírá přes
     * menu Debugger -> CTC/PPI/PIO/PSG State nebo zkratky Alt+Shift+C/I/Z/G.
     * Visibilita se v F1 nepersistuje (= chování konzistentní s FDC State
     * a Callstack), persistence případně přidaná v F6 docs fázi. */
    gui->showCtcStateWindow = false;
    gui->showPpiStateWindow = false;
    gui->showPiozStateWindow = false;
    gui->showPsgStateWindow = false;

    /* gdg-panel F1 scaffold: GDG (Graphic Display Generator) custom video
     * LSI inspector. Per-arch dispatch v gdg_window.cpp (MZ-700 / MZ-800 /
     * MZ-1500 mají různý state). Default closed, user otevírá přes menu
     * Debugger -> GDG State nebo zkratku Alt+Shift+V (V = Video). */
    gui->showGdgStateWindow = false;

    /* memory-disk-state: storage state inspector okna. Memory Disk State
     * ukazuje stav vsech ramdisku (MR-1R18 STD + Pezik E8/68), IDE8 State
     * stav IDE8 radice (Master + Slave). Side-effect free read, default
     * closed, user otevira pres menu Debugger nebo Devices. Visibilita se
     * nepersistuje (= konzistentni s FDC State / QDisk State). */
    gui->showRamdiskStateWindow = false;
    gui->showIde8StateWindow = false;

    /* PSG Audio Scope (psg-audio-scope mutant F1) - samostatné okno pro
     * dynamickou audio analýzu PSG (oscilloscope, plánovaný envelope /
     * piano roll v dalších F-fázích). Default closed, user otevírá přes
     * menu Debugger -> PSG Audio Scope nebo Alt+Shift+A. Visibilita se
     * v F1 nepersistuje (= konzistentní s PSG State a per-chip-panels). */
    gui->showPsgAudioScopeWindow = false;

    /* MCP Activity okno (V1.C.2 mutant mcp-server) - default closed,
     * inicializace ring bufferu se dělá zde aby log_action() byla
     * bezpečně volatelná i pokud okno nikdy nebylo otevřené
     * (= dispatcher feeduje buffer při každém MCP_ACTION broadcastu).
     * Shutdown ring bufferu je v myimgui_destroy_cb. */
    gui->showMcpActivityWindow = false;
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    mcp_activity_window_init();
    /* V1.C.3 - action toast subsystém pro destruktivní MCP akce. */
    action_toast_init();
#endif

    sdlapp_window_set_event_cb(win, myimgui_event_cb, user_data);
    sdlapp_window_set_render_cb(win, myimgui_render_cb, user_data);
    sdlapp_window_set_destroy_cb(win, myimgui_destroy_cb, user_data);
    sdlapp_window_set_active(win, TRUE);

    return TRUE;
}

void myimgui_event_cb(SdlAppWindow *win, SDL_Event *event, gpointer user_data)
{
    (void)user_data;

    if (!win)
    {
        SDLAPP_ERROR("Window is NULL");
        return;
    }

    MyImGui *gui = (MyImGui *)win->gui_data;
    if (!gui)
    {
        SDLAPP_ERROR("Failed to get GUI data for window: %s", win->name);
        return;
    };

    ImGuiContext *ctx = (ImGuiContext *)gui->ctxptr;
    if (!ctx)
    {
        SDLAPP_ERROR("Failed to get ImGui context");
        return;
    };

    ImGui::SetCurrentContext(ctx);
    ImGui_ImplSDL3_ProcessEvent(event);
    // ImGuiIO &io = ImGui::GetIO();
    // (void)io;

    if (gui->event_cb)
    {
        gui->event_cb(win, event, user_data);
    };
}

static void myimgui_rendering_sdl3_sdlrenderer(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    MyImGui *gui = (MyImGui *)win->gui_data;
    ImGuiContext *ctx = (ImGuiContext *)gui->ctxptr;
    SDL_Renderer *sdl_renderer = win->sdl_renderer;

    ImGui::SetCurrentContext(ctx);

    // SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    if (gui->use_bg_color)
    {
        SDL_SetRenderDrawColorFloat(sdl_renderer, gui->bg.r, gui->bg.g, gui->bg.b, gui->bg.a);
        SDL_RenderClear(sdl_renderer);
    };

    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), sdl_renderer);
    SDL_RenderPresent(sdl_renderer);
}

static void myimgui_rendering_sdl3_opengl(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    MyImGui *gui = (MyImGui *)win->gui_data;
    ImGuiContext *ctx = (ImGuiContext *)gui->ctxptr;
    SDL_Window *sdl_window = win->sdl_window;

    ImGui::SetCurrentContext(ctx);

    SDL_GL_MakeCurrent(win->sdl_window, win->gl_context);
    ImGuiIO &io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);

    if (gui->use_bg_color)
    {
        glClearColor(gui->bg.r, gui->bg.g, gui->bg.b, gui->bg.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        SDL_Window *backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }

    SDL_GL_SwapWindow(sdl_window);
}

void myimgui_render_cb(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    if (!win)
    {
        SDLAPP_ERROR("Window is NULL");
        return;
    }

    MyImGui *gui = (MyImGui *)win->gui_data;
    if (!gui)
    {
        SDLAPP_ERROR("Failed to get GUI data for window: %s", win->name);
        return;
    };

    ImGuiContext *ctx = (ImGuiContext *)gui->ctxptr;
    if (!ctx)
    {
        SDLAPP_ERROR("Failed to get ImGui context");
        return;
    };

    ImGui::SetCurrentContext(ctx);

    if (win->renderer_type == SDLAPP_RENDERER_SDL)
    {
        ImGui_ImplSDLRenderer3_NewFrame();
    }
    else if (win->renderer_type == SDLAPP_RENDERER_OPENGL)
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    else
    {
        SDLAPP_ERROR("Unsupported renderer type: %d", win->renderer_type);
        return;
    };

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // zacatek kresleni ImGui
    if (gui->render_cb)
    {
        gui->render_cb(win, user_data);
    }
    else
    {
        ImGui::ShowDemoWindow();
    };

    // Rendering
    ImGui::Render();

    // Specificky render pro kazdy backend

    if (win->renderer_type == SDLAPP_RENDERER_SDL)
    {
        myimgui_rendering_sdl3_sdlrenderer(win, user_data);
    }
    else if (win->renderer_type == SDLAPP_RENDERER_OPENGL)
    {
        myimgui_rendering_sdl3_opengl(win, user_data);
    }
}

void myimgui_destroy_cb(SdlAppWindow *win, gpointer user_data)
{
    (void)user_data;

    if (!win)
    {
        SDLAPP_ERROR("Window is NULL");
        return;
    }

    MyImGui *gui = (MyImGui *)win->gui_data;
    if (!gui)
    {
        SDLAPP_ERROR("Failed to get GUI data for window: %s", win->name);
        return;
    };

    ImGuiContext *ctx = (ImGuiContext *)gui->ctxptr;
    if (!ctx)
    {
        SDLAPP_ERROR("Failed to get ImGui context");
        return;
    };

    ImGui::SetCurrentContext(ctx);

    if (win->renderer_type == SDLAPP_RENDERER_SDL)
    {
        ImGui_ImplSDLRenderer3_Shutdown();
    }
    else if (win->renderer_type == SDLAPP_RENDERER_OPENGL)
    {
        ImGui_ImplOpenGL3_Shutdown();
    }
    else
    {
        SDLAPP_ERROR("Unsupported renderer type: %d", win->renderer_type);
        return;
    };

    ImGui_ImplSDL3_Shutdown();

    ImGui::DestroyContext(ctx);

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
    /* MCP Activity okno (V1.C.2) - uvolnit ring buffer a interní GMutex.
     * Volání musí být idempotentní (= povoleno i pokud init nebyl volán). */
    mcp_activity_window_shutdown();
    /* V1.C.3 - action toast subsystém. */
    action_toast_shutdown();
#endif

    g_free(gui);
    win->gui_data = NULL;
}

void myimgui_set_bg_clean(MyImGui *gui, bool enabled)
{
    if (!gui)
        return;
    gui->use_bg_color = enabled;
}

gboolean myimgui_get_bg_clean(MyImGui *gui)
{
    if (!gui)
        return false;
    return gui->use_bg_color;
}

void myimgui_set_bg_color(MyImGui *gui, float r, float g, float b, float a)
{
    if (!gui)
        return;
    gui->bg.r = r;
    gui->bg.g = g;
    gui->bg.b = b;
    gui->bg.a = a;
}

void myimgui_get_bg_color(MyImGui *gui, float *r, float *g, float *b, float *a)
{
    if (!gui)
        return;
    *r = gui->bg.r;
    *g = gui->bg.g;
    *b = gui->bg.b;
    *a = gui->bg.a;
}

MyImGuiEventCb myimgui_get_event_cb(MyImGui *gui)
{
    if (!gui)
        return NULL;
    return gui->event_cb;
}

void myimgui_set_event_cb(MyImGui *gui, MyImGuiEventCb event_cb)
{
    if (!gui)
        return;
    gui->event_cb = event_cb;
}

MyImGuiRenderCb myimgui_get_render_cb(MyImGui *gui)
{
    if (!gui)
        return NULL;
    return gui->render_cb;
}

void myimgui_set_render_cb(MyImGui *gui, MyImGuiRenderCb render_cb)
{
    if (!gui)
        return;
    gui->render_cb = render_cb;
}
