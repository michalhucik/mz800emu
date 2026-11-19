/**
 * @file   overlay_style.cpp
 * @brief  Vizuální systém overlay menu: jednotka u, načtení písem a ikon, styl ImGui.
 *
 * Popis tokenů a kontraktů je v overlay_style.h.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <math.h>

#include <string.h>

#include <glib.h>

#include "libs/imgui/imgui.h"

#include "ui-imgui/overlay/overlay_style.h"

/** @brief Velikost, se kterou se písma overlaye přidávají do atlasu (skutečná velikost se volí při PushFont()). */
#define OVL_FONT_LOAD_SIZE 16.0f

/** @brief Počet barev, které overlay_style_push() ukládá na zásobník ImGui. */
#define OVL_STYLE_COLOR_COUNT 10
/** @brief Počet proměnných stylu, které overlay_style_push() ukládá na zásobník ImGui. */
#define OVL_STYLE_VAR_COUNT 9

/** @brief Jednotka u plochy nastavené overlay_style_set_viewport(). */
static float s_unit = 1.0f;
/** @brief Šířka plochy v návrhových bodech (šířka v pixelech / u). */
static float s_design_width = OVL_DESIGN_WIDTH;

/** @brief Inter Regular s ikonami; NULL = nenačteno. */
static ImFont *s_font_ui = NULL;
/** @brief Inter SemiBold s ikonami; NULL = nenačteno. */
static ImFont *s_font_bold = NULL;
/** @brief JetBrains Mono; NULL = nenačteno. */
static ImFont *s_font_mono = NULL;
/** @brief Ikony Lucide jsou sloučené do obou řezů Inter. */
static bool s_has_icons = false;

/** @brief Jméno souboru s japonskými znaky slučovaného do písem overlaye (stejný soubor slučuje základní písmo UI). */
#define OVL_FONT_CJK_FILE "NotoSansJP-Regular.ttf"
/** @brief Největší počet souborů písem držených ve sdílené paměti (s_font_data). */
#define OVL_FONT_DATA_MAX 8

/**
 * @brief Obsah souboru písma načtený jednou a sdílený všemi zdroji písem, které ho používají.
 *
 * Invarianty: path == NULL <=> záznam je volný; u obsazeného záznamu jsou
 * data nenulová a size > 0. Paměť se drží do konce procesu (atlas ImGui
 * data zdroje s `FontDataOwnedByAtlas = false` nevlastní a potřebuje je po
 * celou dobu svého života).
 */
typedef struct st_OVL_FONT_DATA {
    gchar *path; /**< cesta souboru (klíč záznamu); vlastní ji záznam */
    gchar *data; /**< obsah souboru; vlastní ho záznam */
    gsize size;  /**< velikost obsahu v bajtech */
} st_OVL_FONT_DATA;

/** @brief Soubory písem ve sdílené paměti. */
static st_OVL_FONT_DATA s_font_data[OVL_FONT_DATA_MAX];
/** @brief Počet čtení souboru písma z disku od startu procesu (overlay_style_test_font_file_loads()). */
static int s_font_file_loads = 0;

float overlay_unit_for(ImVec2 size)
{
    const float ux = size.x / OVL_DESIGN_WIDTH;
    const float uy = size.y / OVL_DESIGN_HEIGHT;
    const float u = (ux < uy) ? ux : uy;
    return (u < OVL_UNIT_MIN) ? OVL_UNIT_MIN : u;
}

void overlay_style_set_viewport(ImVec2 size)
{
    s_unit = overlay_unit_for(size);
    s_design_width = size.x / s_unit;
}

float overlay_unit(void)
{
    return s_unit;
}

float overlay_px(float design)
{
    return design * s_unit;
}

float overlay_border_px(void)
{
    const float w = (float)(int)(OVL_BORDER_W * s_unit + 0.5f);
    return (w < 1.0f) ? 1.0f : w;
}

float overlay_font_size(ImFont *font, float design)
{
    float em = design * s_unit;
    if (em < OVL_FONT_MIN_PX) em = OVL_FONT_MIN_PX;
    /* ImGui bere velikost písma jako výšku řádku (ascent - descent), ne jako velikost em */
    const float line = (font != NULL && font == s_font_mono) ? OVL_FONT_LINE_PER_EM_MONO : OVL_FONT_LINE_PER_EM_UI;
    return (float)(int)(em * line + 0.5f);
}

float overlay_font_px(float design)
{
    return overlay_font_size(NULL, design);
}

bool overlay_is_narrow(void)
{
    return s_design_width < OVL_NARROW_WIDTH;
}

ImU32 overlay_color_alpha(ImU32 col, float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    const ImU32 a = (ImU32)(alpha * 255.0f + 0.5f);
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

/**
 * @brief Soukromá oblast Unicode, kterou písma textu nesmějí obsadit.
 *
 * Inter má v soukromé oblasti vlastní glyfy (číslice v kroužku apod.) na
 * kódových bodech ikon Lucide; první zdroj písma má přednost, takže by je
 * bez vyloučení zakryl.
 */
static const ImWchar s_private_use_ranges[] = {0xE000, 0xF8FF, 0};

/**
 * @brief Přidá do atlasu písmo ze souboru v adresáři, pokud soubor existuje.
 * @param atlas       Atlas písem.
 * @param dir         Adresář písem.
 * @param file        Jméno souboru.
 * @param merge       true = sloučit do naposledy přidaného písma.
 * @param no_private  true = nebrat z písma glyfy soukromé oblasti Unicode (s_private_use_ranges).
 * @param shared      true = obsah souboru načíst jednou a sdílet (overlay_style_add_shared_font()); false = obsah vlastní atlas.
 * @return Přidané písmo (při sloučení cílové písmo); NULL = soubor chybí nebo ho nejde načíst.
 */
static ImFont *overlay_add_font_file(ImFontAtlas *atlas, const char *dir, const char *file, bool merge, bool no_private, bool shared)
{
    gchar *path = g_build_filename(dir, file, NULL);
    ImFont *font = NULL;
    ImFontConfig cfg;
    cfg.MergeMode = merge;
    if (no_private) cfg.GlyphExcludeRanges = s_private_use_ranges;
    if (shared) {
        font = overlay_style_add_shared_font(atlas, path, OVL_FONT_LOAD_SIZE, &cfg, NULL);
    } else if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        /* chybějící soubor by v AddFontFromFileTTF() skončil chybou uživatele ImGui (viz myimgui_set_fonts()) */
        font = atlas->AddFontFromFileTTF(path, OVL_FONT_LOAD_SIZE, &cfg);
    }
    g_free(path);
    return font;
}

/**
 * @brief Označují dvě cesty týž soubor písma?
 *
 * Porovnává texty cest: oddělovače '/' a '\\' bere jako stejné a na Windows
 * nerozlišuje velikost písmen ASCII (volající skládají cestu různě). Různé
 * zápisy téže cesty (relativní, s "..") nerozpozná; soubor se pak jen
 * načte dvakrát.
 *
 * @param a První cesta.
 * @param b Druhá cesta.
 * @return true = cesty jsou stejné.
 */
static bool overlay_font_path_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
#ifdef G_OS_WIN32
        ca = g_ascii_tolower(ca);
        cb = g_ascii_tolower(cb);
#endif
        if (ca != cb) return false;
    }
    return *a == *b;
}

const void *overlay_style_font_file_data(const char *path, int *size)
{
    if (size) *size = 0;
    if (!path || !*path) return NULL;
    int free_slot = -1;
    for (int i = 0; i < OVL_FONT_DATA_MAX; i++) {
        if (!s_font_data[i].path) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (overlay_font_path_equal(s_font_data[i].path, path)) {
            if (size) *size = (int)s_font_data[i].size;
            return s_font_data[i].data;
        }
    }
    /* chybějící soubor se neeviduje: příští volání ho zkusí znovu */
    if (free_slot < 0 || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) return NULL;
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL)) return NULL;
    s_font_file_loads++;
    if (len == 0 || len > (gsize)G_MAXINT) {
        g_free(data);
        return NULL;
    }
    s_font_data[free_slot].path = g_strdup(path);
    s_font_data[free_slot].data = data;
    s_font_data[free_slot].size = len;
    if (size) *size = (int)len;
    return data;
}

int overlay_style_test_font_file_loads(void)
{
    return s_font_file_loads;
}

ImFont *overlay_style_add_shared_font(ImFontAtlas *atlas, const char *path, float size_pixels, const ImFontConfig *cfg, const ImWchar *glyph_ranges)
{
    if (!atlas) return NULL;
    int size = 0;
    const void *data = overlay_style_font_file_data(path, &size);
    if (!data) return NULL;
    ImFontConfig font_cfg = cfg ? *cfg : ImFontConfig();
    font_cfg.FontDataOwnedByAtlas = false;
    if (font_cfg.Name[0] == '\0') {
        /* jméno zdroje jako u ImFontAtlas::AddFontFromFileTTF(): jméno souboru bez cesty */
        const char *p = path + strlen(path);
        while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
        g_strlcpy(font_cfg.Name, p, sizeof(font_cfg.Name));
    }
    /* atlas obsah jen čte; ukazatel bez const vyžaduje jeho rozhraní */
    return atlas->AddFontFromMemoryTTF((void *)data, size, size_pixels, &font_cfg, glyph_ranges);
}

bool overlay_style_load_fonts(ImFontAtlas *atlas, const char *font_dir)
{
    s_font_ui = NULL;
    s_font_bold = NULL;
    s_font_mono = NULL;
    s_has_icons = false;
    if (!atlas || !font_dir || !g_file_test(font_dir, G_FILE_TEST_IS_DIR)) return false;

    /* ikony a japonské znaky se slučují do více písem: obsah souboru jednou pro všechna (NotoSansJP má 16 MB) */
    bool icons_regular = false, icons_bold = false;
    s_font_ui = overlay_add_font_file(atlas, font_dir, "Inter-Regular.ttf", false, true, false);
    if (s_font_ui) {
        icons_regular = overlay_add_font_file(atlas, font_dir, "lucide.ttf", true, false, true) != NULL;
        overlay_add_font_file(atlas, font_dir, OVL_FONT_CJK_FILE, true, true, true);
    }
    s_font_bold = overlay_add_font_file(atlas, font_dir, "Inter-SemiBold.ttf", false, true, false);
    if (s_font_bold) {
        icons_bold = overlay_add_font_file(atlas, font_dir, "lucide.ttf", true, false, true) != NULL;
        overlay_add_font_file(atlas, font_dir, OVL_FONT_CJK_FILE, true, true, true);
    }
    /* jména souborů mohou nést japonské znaky (klasické menu je základním písmem ukáže) */
    s_font_mono = overlay_add_font_file(atlas, font_dir, "JetBrainsMono-Regular.ttf", false, true, false);
    if (s_font_mono) overlay_add_font_file(atlas, font_dir, OVL_FONT_CJK_FILE, true, true, true);

    /* ikony se kreslí písmem overlay_font_ui(); bez něj by kód ikony skončil v základním písmu (jiné glyfy) */
    s_has_icons = icons_regular && (icons_bold || !s_font_bold);
    return s_font_ui && s_font_bold && s_font_mono && icons_regular && icons_bold;
}

ImFont *overlay_font_ui(void)
{
    if (s_font_ui) return s_font_ui;
    if (!ImGui::GetCurrentContext()) return NULL;
    const ImGuiIO &io = ImGui::GetIO();
    if (io.FontDefault) return io.FontDefault;
    return (io.Fonts->Fonts.Size > 0) ? io.Fonts->Fonts[0] : NULL;
}

ImFont *overlay_font_ui_bold(void)
{
    return s_font_bold ? s_font_bold : overlay_font_ui();
}

ImFont *overlay_font_mono(void)
{
    return s_font_mono ? s_font_mono : overlay_font_ui();
}

bool overlay_style_has_icons(void)
{
    return s_has_icons;
}

void overlay_style_push(void)
{
    const float grid = floorf(overlay_px(OVL_GRID)); /* celé pixely: výška obsahu karet se pak nezaokrouhluje dolů */
    ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_BODY));

    /* OVL_STYLE_COLOR_COUNT barev */
    ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, OVL_COL_TEXT_DISABLED);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, OVL_COL_RAISED);
    ImGui::PushStyleColor(ImGuiCol_Border, OVL_COL_BORDER);
    ImGui::PushStyleColor(ImGuiCol_Separator, OVL_COL_BORDER);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, overlay_color_alpha(OVL_COL_PANEL, 0.0f));
    /* jezdec posuvníku barvou vedlejšího textu: musí být vidět i nad průhlednou plochou tabu Display (posudek N01) */
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, OVL_COL_TEXT_SECONDARY);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, OVL_COL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, OVL_COL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, OVL_COL_ACCENT);

    /* OVL_STYLE_VAR_COUNT proměnných */
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(grid, grid));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(grid, grid));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, overlay_px(OVL_SCROLLBAR_W));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarPadding, overlay_px(OVL_SCROLLBAR_PAD));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, overlay_px(OVL_SCROLLBAR_W * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, overlay_px(OVL_RADIUS_CONTROL));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, overlay_border_px());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, overlay_px(OVL_RADIUS_CONTROL));
}

void overlay_style_pop(void)
{
    ImGui::PopStyleVar(OVL_STYLE_VAR_COUNT);
    ImGui::PopStyleColor(OVL_STYLE_COLOR_COUNT);
    ImGui::PopFont();
}
