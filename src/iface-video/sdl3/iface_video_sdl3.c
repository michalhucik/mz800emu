#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <stdint.h>
#include "iface/iface_video.h"
#include "mzarch/mzhal.h"
#include "emulator/display.h"
#include "libs/sdlapp/sdlapp.h"

#define SURFACE_COLORS_COUNT 256

SDL_Surface *video_sdl3_create_surface(void)
{
    SDL_Surface *surface = SDL_CreateSurface(g_mzhal.video_display_width, g_mzhal.video_display_height, SDL_PIXELFORMAT_INDEX8);

    if (!surface)
    {
        SDLAPP_ERROR("Could not create surface! SDL_Error: %s\n", SDL_GetError());
        return NULL;
    };

    SDL_Color colors[SURFACE_COLORS_COUNT];
    for (int i = 0; i < SURFACE_COLORS_COUNT; ++i)
    {
        colors[i].r = i;
        colors[i].g = i;
        colors[i].b = i;
        colors[i].a = SDL_ALPHA_OPAQUE;
    };

    SDL_Palette *palette = SDL_CreatePalette(SURFACE_COLORS_COUNT);
    if (!palette)
    {
        SDLAPP_ERROR("Failed to create palette: %s\n", SDL_GetError());
        SDL_DestroySurface(surface);
        return NULL;
    };

    if (!SDL_SetPaletteColors(palette, colors, 0, SURFACE_COLORS_COUNT))
    {
        SDLAPP_ERROR("Failed to set palette colors: %s\n", SDL_GetError());
        SDL_DestroyPalette(palette);
        SDL_DestroySurface(surface);
        return NULL;
    };

    if (!SDL_SetSurfacePalette(surface, palette))
    {
        SDLAPP_ERROR("Failed to set surface palette: %s\n", SDL_GetError());
        SDL_DestroyPalette(palette);
        SDL_DestroySurface(surface);
        return NULL;
    };

    return surface;
}

void video_sdl3_set_surface_colormap(uint32_t *colormap, SDL_Surface *surface)
{
    SDL_Color colors[DISPLAY_MZCOLORS];

    int i;
    for (i = 0; i < DISPLAY_MZCOLORS; i++)
    {
        colors[i].r = colormap[i] >> 16;
        colors[i].g = (colormap[i] >> 8) & 0xff;
        colors[i].b = colormap[i] & 0xff;
        colors[i].a = 0;
    };

    SDL_LockSurface(surface);

    SDL_Palette *palette = SDL_GetSurfacePalette(surface);
    if (!palette)
    {
        SDLAPP_ERROR("Failed to get palette: %s\n", SDL_GetError());
        SDL_UnlockSurface(surface);
        return;
    };

    if (!SDL_SetPaletteColors(palette, colors, 0, DISPLAY_MZCOLORS))
    {
        SDLAPP_ERROR("Failed to set palette colors: %s\n", SDL_GetError());
        SDL_UnlockSurface(surface);
        return;
    };

    SDL_UnlockSurface(surface);

    iface_video_create_redraw_full_screen_request();
}

SDL_Surface *ConvertIndexedSurfaceToRGBA(SDL_Surface *surface)
{
    if (!surface || surface->format != SDL_PIXELFORMAT_INDEX8)
    {
        SDLAPP_ERROR("ConvertIndexedToRGBA: Invalid or non-indexed surface");
        return NULL;
    }

    // Vytvoříme nový Surface ve formátu RGBA32
    SDL_Surface *converted = SDL_CreateSurface(surface->w, surface->h, SDL_PIXELFORMAT_RGBA32);
    if (!converted)
    {
        SDLAPP_ERROR("ConvertIndexedToRGBA: Failed to create RGBA surface: %s", SDL_GetError());
        return NULL;
    }

    // Získáme paletu z původního Surface
    SDL_Palette *palette = SDL_GetSurfacePalette(surface);
    if (!palette)
    {
        SDLAPP_ERROR("ConvertIndexedToRGBA: Surface has no palette!");
        SDL_DestroySurface(converted);
        return NULL;
    }

    Uint8 *srcPixels = (Uint8 *)surface->pixels;
    Uint32 *dstPixels = (Uint32 *)converted->pixels;
    int pixelCount = surface->w * surface->h;

    // Kopírujeme a převádíme indexované pixely na RGBA
    for (int i = 0; i < pixelCount; i++)
    {
        Uint8 index = srcPixels[i];               // Index v paletě
        SDL_Color color = palette->colors[index]; // Skutečná barva
        dstPixels[i] = SDL_MapRGBA(SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA32), NULL, color.r, color.g, color.b, 255);
#if 0
        if (i == 0) {
            SDL_Log("First pixel index: %d (R=%d G=%d B=%d A=%d)",
                index,
                color.r, color.g, color.b, color.a);
        };
#endif
    };
    return converted;
}

/**
 * @brief Vytvoří OpenGL texturu ze zadaného SDL_Surface.
 * @param surface Ukazatel na `SDL_Surface`, který chceme převést.
 * @return Identifikátor OpenGL textury (`GLuint`) nebo 0 při neúspěchu.
 */
GLuint SDL_SurfaceToTexture(SDL_Surface *surface)
{
    if (!surface)
    {
        SDLAPP_ERROR("SDL_SurfaceToTexture: Invalid surface\n");
        return 0;
    }

    SDL_Surface *converted = NULL;
    if (surface->format == SDL_PIXELFORMAT_INDEX8)
    {
        converted = ConvertIndexedSurfaceToRGBA(surface);
        if (!converted)
        {
            return 0;
        }
        surface = converted; // Použijeme převedený povrch
    }
    else if (surface->format != SDL_PIXELFORMAT_RGBA32)
    {
        converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        if (!converted)
        {
            SDLAPP_ERROR("SDL_SurfaceToTexture: Unsupported surface format: %s\n", SDL_GetPixelFormatName(surface->format));
            SDLAPP_ERROR("Failed to convert surface to RGBA32: %s", SDL_GetError());
            return 0;
        }
        surface = converted;
    };

    GLenum format = GL_RGBA;

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0, format, GL_UNSIGNED_BYTE, surface->pixels);

    glBindTexture(GL_TEXTURE_2D, 0);

    if (converted)
    {
        SDL_DestroySurface(converted); // Uvolníme převedený surface
    }

    return texture;
}

gboolean video_sdl3_update_surface_from_framebuffer(SDL_Surface *surface)
{
    if (!surface)
        return FALSE;

    static uint32_t current_emulator_screen = (uint32_t)-1;

    APP_MUTEX_LOCK(g_iface_video->fbsnapshot_pixels_mutex);

    if (DISPLAY_TEST_FRAMERATE_MODE_SLAVE)
    {
        // cekame na signal od framebuferu, ale max 20ms
        APP_COND_WAIT_TIMEOUT_MS(g_iface_video->fbsnapshot_pixels_cond, g_iface_video->fbsnapshot_pixels_mutex, 20);
    }

    bool readraw_request = false;

    // Pokud je k dispozici novy framebuffer, tak ho zkopirujeme do surface
    if ((g_iface_video->fbsnapshot_pixels != NULL) && (g_iface_video->fbsnapshot_framebuffer_state != FB_STATE_NOT_CHANGED))
    {
        current_emulator_screen = g_iface_video->fbsnapshot_screen_id;
        SDL_LockSurface(surface);
        memcpy(surface->pixels, g_iface_video->fbsnapshot_pixels, g_mzhal.video_display_width * g_mzhal.video_display_height);
        SDL_UnlockSurface(surface);
        g_iface_video->fbsnapshot_pixels = NULL;
    };
    readraw_request = g_iface_video->fbsnapshot_force_redraw_request;
    g_iface_video->fbsnapshot_force_redraw_request = false;
    APP_MUTEX_UNLOCK(g_iface_video->fbsnapshot_pixels_mutex);

    gboolean result = ((readraw_request) || (g_iface_video->renderer_screen_id != current_emulator_screen));
    g_iface_video->renderer_screen_id = current_emulator_screen;
    return result;
}

void video_sdl3_render_surface_as_background(SDL_Surface *surface, SDL_Renderer *renderer, float aspect_ratio, gboolean have_new_screen)
{
    if ((!surface) || (!renderer))
        return;

    static SDL_Texture *texture = NULL;
    if ((have_new_screen) || (!texture))
    {
        if (texture)
        {
            SDL_DestroyTexture(texture);
        };
        texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (NULL == texture)
        {
            SDLAPP_ERROR("SDL_CreateTextureFromSurface(): %s\n", SDL_GetError());
            return;
        };
    };

    // emulator window as background
    int win_width, win_height;
    SDL_GetCurrentRenderOutputSize(renderer, &win_width, &win_height);

    int scaled_width = win_width;
    int scaled_height = (int)(win_width / aspect_ratio);

    if (scaled_height > win_height)
    {
        scaled_height = win_height;
        scaled_width = (int)(win_height * aspect_ratio);
    }

    SDL_FRect dest_rect = {
        ((float)win_width - scaled_width) / 2,
        ((float)win_height - scaled_height) / 2,
        (float)scaled_width,
        (float)scaled_height};

    // Vyčištění rendereru a vykreslení textury jako pozadí
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, &dest_rect);
}

void video_sdl3_set_render_vsync(bool enabled)
{
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, (enabled) ? "1" : "0"); // 0 => Vypne V-Sync
}

void video_sdl3_set_GL_swap_interval(int value)
{
    // g_print("Setting GL swap interval: %d\n", value);
    g_return_if_fail(value >= -1 && value <= 1);
    bool result = SDL_GL_SetSwapInterval(value); // 0 = immediate updates, 1 = vsync, -1 = adaptive vsync
    if (!result)
    {
        SDLAPP_ERROR("Failed to set GL swap interval: %s", SDL_GetError());
        if (value == -1)
        {
            WARN("Adaptive V-Sync is not supported! Switching to V-Sync.\n");
            SDL_GL_SetSwapInterval(1);
            display_set_framerate_mode(DISPLAY_FRAMERATE_MODE_VSYNC);
        };
    };
}