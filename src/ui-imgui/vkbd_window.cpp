#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "main.h"
#include "emulator/mzarch/mzhal.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <string>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "libs/sdlapp/sdlapp.h"

// Lokalizace
#include "i18n.h"

#include "ui/vkbd/ui_vkbd.h"
#include "ui-imgui/bootstrap/images.h"
#include "iface/iface_keyboard.h"

/* g_sdlapp je deklarovaný v main.h který už je include'd výše */

extern "C"
{
    void imgui_vkbd(bool *p_open);
};

typedef struct imgui_vkbd_t
{
    const st_VKBD_KEYDEF *vkdef;
    size_t count;
    ui_glimage_t **img;
} imgui_vkbd_t;

static imgui_vkbd_t g_vkbd_main[] = {
    {vk_row1_def, 0, NULL},
    {vk_row2_def, 0, NULL},
    {vk_row3_def, 0, NULL},
    {vk_row4_def, 0, NULL},
    {vk_row5_def, 0, NULL},
    {NULL, 0, NULL}};

static imgui_vkbd_t g_vkbd_others[] = {
    {vk_row0a_def, 0, NULL},
    {vk_row0b_def, 0, NULL},
    {vk_cursorUp_def, 0, NULL},
    {vk_cursorLR_def, 0, NULL},
    {vk_cursorDown_def, 0, NULL},
    {NULL, 0, NULL}};

bool imgui_vkbd_init(imgui_vkbd_t *vkbd)
{
    if (vkbd == NULL)
        return false;

    size_t i = 0;
    while (vkbd[i].vkdef != NULL)
    {
        const st_VKBD_KEYDEF *vkdef = vkbd[i].vkdef;
        size_t j = 0;
        while (vkdef[j].scancode != 0)
        {
            char *rel = g_strconcat(VKBD_IMAGE_BASE_PATH, vkdef[j].filename, (char *)NULL);
            char *fullpath_c = sdlapp_paths_resolve_home(g_sdlapp->paths, rel);
            g_free(rel);
            ui_glimage_t *img = imgui_images_append(vkdef[j].filename, fullpath_c);
            g_free(fullpath_c);
            if (!img)
            {
                fprintf(stderr, "Failed to load image: %s\n", vkdef[j].filename);
                return false;
            };
            j++;

            if (vkbd[i].count < j)
            {
                if (vkbd[i].img == NULL)
                {
                    vkbd[i].img = (ui_glimage_t **)g_malloc(sizeof(ui_glimage_t *) * j);
                    if (vkbd[i].img == NULL)
                    {
                        fprintf(stderr, "Failed to allocate memory for vkbd[%zu].img\n", i);
                        return false;
                    };
                }
                else
                {
                    vkbd[i].img = (ui_glimage_t **)g_realloc(vkbd[i].img, sizeof(ui_glimage_t *) * j);
                    if (vkbd[i].img == NULL)
                    {
                        fprintf(stderr, "Failed to reallocate memory for vkbd[%zu].img\n", i);
                        return false;
                    };
                };

                vkbd[i].count = j;
            };

            vkbd[i].img[j - 1] = img;
        };
        i++;
    };

    return true;
};

/**
 * @brief Vykreslí řadu obrázků a přidá hover/klik detekci pomocí pozice myši.
 *
 * @param vkbd   Struktura s polem obrázků a jejich počtem.
 * @param x      Počáteční X souřadnice relativně k oknu.
 * @param y      Počáteční Y souřadnice relativně k oknu.
 * @param scale  Měřítko pro škálování obrázků.
 */
void imgui_vkbd_draw_row(imgui_vkbd_t *vkbd, int x, int y, float scale)
{
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImVec2 base_pos = ImGui::GetCursorScreenPos(); // pozice okna na obrazovce
    ImVec2 pos = ImVec2(base_pos.x + x, base_pos.y + y);
    ImVec2 mouse = ImGui::GetIO().MousePos;

    for (size_t i = 0; i < vkbd->count; i++)
    {
        ui_glimage_t *img = vkbd->img[i];
        if (img != NULL)
        {
            bool is_active = ui_vkbd_is_active(&vkbd->vkdef[i]);

            ImVec2 size = ImVec2(img->width * scale, img->height * scale);
            ImVec2 p1 = pos;
            ImVec2 p2 = ImVec2(pos.x + size.x, pos.y + size.y);

            // Detekce hoveru
            bool hovered = mouse.x >= p1.x && mouse.x <= p2.x &&
                           mouse.y >= p1.y && mouse.y <= p2.y;

            // Posun při aktivaci (napodobení stisknutí)
            if (is_active)
            {
                p1.y += 4 * scale;
                p2.y += 4 * scale;
            };

            // Tint pro aktivní tlačítko (např. stmavení)
            ImU32 tint = is_active ? IM_COL32(200, 200, 200, 255) : IM_COL32(255, 255, 255, 255);

            // Vykreslení obrázku
            draw_list->AddImage(img->texture, p1, p2, ImVec2(0, 0), ImVec2(1, 1), tint);

            // Hover efekt
            if (hovered)
            {
                draw_list->AddRect(p1, p2, IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);

                // Hint s PC ekvivalentem pro "neintuitivně" mapované klávesy
                // (GRAPH, ALPHA, BLANK, @, ?, LIBRA, ...). Ostatní klávesy
                // vrátí NULL = bez tooltipu.
                const char *pc_hint = ui_vkbd_pc_hint(vkbd->vkdef[i].scancode);
                if (pc_hint != NULL)
                {
                    ImGui::SetTooltip("%s", _(pc_hint));
                };
            };

            // if (is_active)
            // {
            //     // g_print("Key %s is active\n", vkbd->vkdef[i].filename);
            //     // Pokud je klávesa aktivní, nastavíme barvu na zelenou
            //     draw_list->AddRectFilled(pos, ImVec2(pos.x + img->width * scale, pos.y + img->height * scale), IM_COL32(0, 255, 0, 255));
            // };

            // Detekce kliknutí
            if (hovered)
            {
                if (ImGui::IsMouseClicked(0))
                {
                    // g_print("Pressed key: %s\n", vkbd->vkdef[i].filename);
                    PIO8255_VKBDBIT_RESET(vkbd->vkdef[i].mzcolumn, vkbd->vkdef[i].mzbit);
                }
                else if (ImGui::IsMouseReleased(0))
                {
                    // g_print("Released key: %s\n", vkbd->vkdef[i].filename);
                    PIO8255_VKBDBIT_SET(vkbd->vkdef[i].mzcolumn, vkbd->vkdef[i].mzbit);
                };
            };

            // Posun pro další obrázek
            pos.x += size.x;
        };
    };
};

void imgui_vkbd(bool *p_open)
{
    if (!*p_open)
        return;

    if (g_vkbd_main[0].img == NULL)
    {
        if (!imgui_vkbd_init(g_vkbd_main))
            return;

        if (!imgui_vkbd_init(g_vkbd_others))
            return;
    };

    int key_width = g_vkbd_main[0].img[0]->width;
    int key_height = g_vkbd_main[0].img[0]->height;
    float scale = 1.5f;

    ImGuiWindowFlags flags =
        //ImGuiWindowFlags_AlwaysAutoResize | // Okno se automaticky přizpůsobí obsahu
        ImGuiWindowFlags_NoResize |         // Zakáže změnu velikosti
        ImGuiWindowFlags_NoScrollbar | // Zakáže scrollbar
        ImGuiWindowFlags_NoCollapse |  // Zakáže možnost sbalení okna
        ImGuiWindowFlags_NoDocking;    // Zakáže možnost dockování

    //ImGui::SetNextWindowSize(ImVec2(1200, 768), ImGuiCond_FirstUseEver);
    /* Auto-layout při fresh open - cache _L() do lokální proměnné.
     * Titulek je per MZARCH (klávesnice se liší mezi modely). */
    /* Runtime dle g_mzhal.arch; vsechny _L literaly zustavaji kvuli
     * gettext extrakci a stabilnim ImGui ID per arch. */
    const char *vkbd_title =
        (g_mzhal.arch == 700)  ? _L("MZ-700 Virtual Keyboard") :
        (g_mzhal.arch == 1500) ? _L("MZ-1500 Virtual Keyboard") :
                                 _L("MZ-800 Virtual Keyboard");
    auto_layout_first_use_portrait(vkbd_title, 700.0f, 300.0f);
    ImGui::Begin(vkbd_title, p_open, flags);

    int x = 0;
    int y = 0;

    int others0_x = (key_width * (15.5f + 1.5f) * scale);
    int others1_x = others0_x + (key_width * 0.75f * scale);
    int spacebar_x = (key_width * (1.5f + 1.5f) * scale);

    imgui_vkbd_draw_row(&g_vkbd_others[0], x, y, scale);
    imgui_vkbd_draw_row(&g_vkbd_others[1], others0_x, y, scale);

    x = 0;
    y += key_height * scale;
    imgui_vkbd_draw_row(&g_vkbd_main[0], x, y, scale);

    y += key_height * scale;
    imgui_vkbd_draw_row(&g_vkbd_main[1], x, y, scale);
    imgui_vkbd_draw_row(&g_vkbd_others[2], others1_x, y, scale);

    y += key_height * scale;
    imgui_vkbd_draw_row(&g_vkbd_main[2], x, y, scale);
    imgui_vkbd_draw_row(&g_vkbd_others[3], others0_x, y, scale);

    y += key_height * scale;
    imgui_vkbd_draw_row(&g_vkbd_main[3], x, y, scale);
    imgui_vkbd_draw_row(&g_vkbd_others[4], others1_x, y, scale);

    y += key_height * scale;
    imgui_vkbd_draw_row(&g_vkbd_main[4], spacebar_x, y, scale);

    ImGui::SetWindowSize(ImVec2(others1_x + (key_width * 3 * scale), y + key_height * 2.5f * scale), ImGuiCond_Always);

    g_gui->VkbdWindowHasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    // {
    //     *p_open = false;
    // };

    ImGui::End();
    return;
}