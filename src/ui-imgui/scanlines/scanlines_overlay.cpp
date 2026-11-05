/**
 * @file   scanlines_overlay.cpp
 * @brief  Emulace scanlines CRT přes obraz hlavního okna (viz scanlines_overlay.h).
 *
 * @par Licence: GPLv3
 */

#include <stdint.h>
#include <glib.h>
#include <SDL3/SDL_opengl.h>

#include "libs/imgui/imgui.h"

#include "scanlines_overlay.h"
#include "emulator/display.h"
#include "emulator/hw-generic/gdg/video.h"

/**
 * @brief Počet texelů vzoru na jeden rastrový řádek.
 *
 * Horní polovina (SCANLINES_PATTERN_ROWS / 2) je průhledná (obraz), dolní
 * černá (mezera). S lineárním filtrováním je přechod mezi nimi široký jeden
 * texel = 1/8 řádku. Se 2 texely by z filtrování vznikla trojúhelníková vlna:
 * obraz by nikde neměl plný jas a mezera plnou tmavost (změřeno při
 * zvětšení 3 a 4 px na řádek).
 */
#define SCANLINES_PATTERN_ROWS 8

/**
 * @brief Stav textury vzoru scanlines.
 *
 * @invariant tex != 0 jen tehdy, když se textura podařilo vytvořit;
 *            failed == true znamená, že se o vytvoření už nepokoušíme.
 */
typedef struct st_SCANLINES_TEX
{
    GLuint tex;  /**< OpenGL textura 1 x SCANLINES_PATTERN_ROWS texelů (viz SCANLINES_PATTERN_ROWS). */
    bool failed; /**< Vytvoření textury selhalo (efekt se nekreslí). */
} st_SCANLINES_TEX;

static st_SCANLINES_TEX s_scanlines = {0, false};

/**
 * @brief Vytvoří texturu vzoru, pokud ještě neexistuje.
 *
 * Wrap GL_REPEAT ve svislém směru umožní jednou texturou pokrýt všechny
 * řádky (souřadnice v jde 0 až VIDEO_DISPLAY_HEIGHT), GL_LINEAR dá při
 * neceločíselném zvětšení plynulý přechod místo nerovnoměrně širokých čar.
 *
 * @return Identifikátor textury, nebo 0 při chybě (pak nastaví s_scanlines.failed).
 * @pre Aktivní OpenGL kontext hlavního okna.
 */
static GLuint scanlines_get_texture(void)
{
    if (s_scanlines.tex || s_scanlines.failed)
        return s_scanlines.tex;

    /* RGBA: horní polovina řádku = obraz beze změny, dolní = mezera mezi řádky. */
    uint8_t pattern[SCANLINES_PATTERN_ROWS * 4];
    for (int row = 0; row < SCANLINES_PATTERN_ROWS; row++)
    {
        pattern[row * 4 + 0] = 0;
        pattern[row * 4 + 1] = 0;
        pattern[row * 4 + 2] = 0;
        pattern[row * 4 + 3] = (row < SCANLINES_PATTERN_ROWS / 2) ? 0 : 255;
    };

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex)
    {
        s_scanlines.failed = true;
        g_printerr("Scanlines: glGenTextures() failed, effect disabled\n");
        return 0;
    };

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, SCANLINES_PATTERN_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern);
    glBindTexture(GL_TEXTURE_2D, 0);

    s_scanlines.tex = tex;
    return tex;
}

void imgui_scanlines_overlay(void)
{
    if (!DISPLAY_TEST_SCANLINES)
        return;

    GLuint tex = scanlines_get_texture();
    if (!tex)
        return;

    ImVec2 p_min = ImGui::GetItemRectMin();
    ImVec2 p_max = ImGui::GetItemRectMax();

    /* Jedna perioda vzoru na jeden řádek framebufferu (= jeden rastrový řádek). */
    const float v_max = (float)VIDEO_DISPLAY_HEIGHT;
    const int alpha = (g_display.scanlines_intensity * 255 + 50) / 100;

    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)tex, p_min, p_max,
                                         ImVec2(0.0f, 0.0f), ImVec2(1.0f, v_max),
                                         IM_COL32(255, 255, 255, alpha));
}
