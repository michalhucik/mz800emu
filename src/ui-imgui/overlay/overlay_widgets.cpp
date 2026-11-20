/**
 * @file   overlay_widgets.cpp
 * @brief  Widgety overlay menu (karta, řádek nastavení, tlačítko, přepínač, segmentová volba, posuvník, štítky).
 *
 * Popis chování a kontraktů je v overlay_widgets.h. Widgety jsou vlastní
 * položky ImGui (ItemSize / ItemAdd / ButtonBehavior) kreslené přes
 * ImDrawList; výchozí vzhled ImGui nepoužívají.
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
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

/* ================================================================
 * Rozměry widgetů v návrhových bodech
 * ================================================================ */

/** @brief Vodorovný vnitřní okraj tlačítka a segmentu. */
#define OVL_W_PAD_X 12.0f
/** @brief Mezera mezi ikonou, popiskem a štítkem uvnitř prvku. */
#define OVL_W_GAP 8.0f
/** @brief Nejmenší svislý vnitřní okraj prvku s textem. */
#define OVL_W_PAD_Y 6.0f
/** @brief Vodorovný vnitřní okraj klávesového štítku. */
#define OVL_KEYCAP_PAD_X 6.0f
/** @brief Svislý vnitřní okraj klávesového štítku. */
#define OVL_KEYCAP_PAD_Y 3.0f
/** @brief Vodorovný vnitřní okraj stavového štítku. */
#define OVL_CHIP_PAD_X 8.0f
/** @brief Svislý vnitřní okraj stavového štítku. */
#define OVL_CHIP_PAD_Y 4.0f
/** @brief Šířka přepínače. */
#define OVL_TOGGLE_W 44.0f
/** @brief Výška přepínače. */
#define OVL_TOGGLE_H 24.0f
/** @brief Výška položky posuvníku (plocha pro obrys fokusu). */
#define OVL_SLIDER_H 28.0f
/** @brief Výška drážky posuvníku. */
#define OVL_SLIDER_TRACK_H 6.0f
/** @brief Poloměr jezdce posuvníku. */
#define OVL_SLIDER_KNOB_R 8.0f
/** @brief Nejmenší délka drážky posuvníku; pod ni se prvek nezmenší ani v užším místě (pak přesáhne danou šířku). */
#define OVL_SLIDER_TRACK_HARD_MIN 8.0f
/** @brief Průměr kontrolky. */
#define OVL_LED_SIZE 10.0f
/** @brief Velikost písma popisku řádku nastavení. */
#define OVL_FONT_ROW_LABEL 16.0f
/** @brief Podíl šířky řádku nastavení, který zabírá sloupec popisku. */
#define OVL_ROW_LABEL_FRACTION 0.36f
/** @brief Výška pásu popisku ve vyšším řádku nastavení (ovl_row_begin_stacked()). */
#define OVL_ROW_STACKED_LABEL_H 34.0f
/** @brief Výška pásu ovládacích prvků ve vyšším řádku nastavení včetně mezery pod nimi. */
#define OVL_ROW_STACKED_ITEMS_H 48.0f
/** @brief Šířka textu tooltipu, po které se zalamuje. */
#define OVL_TOOLTIP_WRAP 360.0f
/** @brief Poměr velikosti písma ikony ke straně jejího čtverce (kresba Lucide nevyplňuje celé em). */
#define OVL_ICON_FONT_SCALE 1.2f
/** @brief Délka čárky čárkovaného okraje. */
#define OVL_DASH_LEN 5.0f
/** @brief Mezera čárkovaného okraje. */
#define OVL_DASH_GAP 4.0f
/** @brief Největší počet segmentů segmentové volby. */
#define OVL_SEGMENT_MAX 12
/** @brief Výška položky rozbaleného seznamu ovl_dropdown(). */
#define OVL_DROPDOWN_ITEM_H 32.0f
/** @brief Výška oddělovače bez nadpisu v rozbaleném seznamu. */
#define OVL_DROPDOWN_SEPARATOR_H 9.0f
/** @brief Vnitřní okraj rozbaleného seznamu. */
#define OVL_DROPDOWN_PAD 4.0f
/** @brief Mezera mezi zavřeným prvkem a rozbaleným seznamem a nejmenší odstup seznamu od okraje viewportu. */
#define OVL_DROPDOWN_GAP 4.0f
/** @brief Nejmenší šířka textu zavřeného rozbalovacího výběru. */
#define OVL_DROPDOWN_TEXT_MIN 48.0f
/** @brief Největší počet položek rozbaleného seznamu, jejichž obdélník se pamatuje pro testy. */
#define OVL_DROPDOWN_TEST_MAX 48

/* ================================================================
 * Stav
 * ================================================================ */

/**
 * @brief Rozpracovaný řádek nastavení (mezi ovl_row_begin() a ovl_row_end()).
 *
 * Invariant: při active == false se ostatní členy nepoužívají.
 */
typedef struct st_OVL_ROW {
    bool active;   /**< řádek je otevřený; widgety se v něm svisle středí */
    ImVec2 start;  /**< levý horní roh řádku (souřadnice obrazovky) */
    float width;   /**< šířka řádku */
    float height;  /**< výška řádku */
    float items_y; /**< horní okraj pásu, ve kterém se widgety svisle středí (u běžného řádku start.y) */
    float items_h; /**< výška pásu widgetů (u běžného řádku height) */
} st_OVL_ROW;

/** @brief Rozpracovaný řádek nastavení. */
static st_OVL_ROW s_row;
/** @brief Počet řádků nastavení začatých v současné kartě (před druhým a dalším je oddělovací linka). */
static int s_row_index = 0;
/** @brief Barva, kterou naposledy kreslil pasivní prvek (ovl_test_last_passive_color()). */
static ImU32 s_test_last_text_col = 0;
/** @brief Příští ovládací prvek se má vykreslit s obrysem fokusu (ovl_test_force_focus_next()). */
static bool s_force_focus = false;
/** @brief Číslo snímku ImGui, ve kterém byl naposledy vykreslen rozbalený seznam ovl_dropdown(); -1 = nikdy. */
static int s_dropdown_frame = -1;
/** @brief Dolní mez rozbaleného seznamu ovl_dropdown() v souřadnicích obrazovky (ovl_dropdown_set_bottom_limit()). */
static float s_dropdown_bottom = 0.0f;
/** @brief Číslo snímku ImGui, pro který s_dropdown_bottom platí; -1 = mez není nastavená. */
static int s_dropdown_bottom_frame = -1;

/**
 * @brief Obdélník položky rozbaleného seznamu pro testy (ovl_test_dropdown_item_rect()).
 *
 * Invariant: platí jen při frame == s_dropdown_frame.
 */
typedef struct st_OVL_DROPDOWN_TEST_RECT {
    int frame;  /**< číslo snímku ImGui, ve kterém byla položka vykreslena */
    ImVec2 min; /**< levý horní roh */
    ImVec2 max; /**< pravý dolní roh */
} st_OVL_DROPDOWN_TEST_RECT;

/** @brief Obdélníky položek naposledy vykresleného rozbaleného seznamu (index = pořadí položky). */
static st_OVL_DROPDOWN_TEST_RECT s_dropdown_rects[OVL_DROPDOWN_TEST_MAX];

ImU32 ovl_test_last_passive_color(void)
{
    return s_test_last_text_col;
}

void ovl_test_force_focus_next(void)
{
    s_force_focus = true;
}

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/**
 * @brief Zásobník písma ImGui po dobu života objektu.
 */
struct OvlFontScope {
    /**
     * @brief Nastaví písmo a velikost.
     * @param font   Písmo (overlay_font_*()).
     * @param design Velikost v návrhových bodech.
     */
    OvlFontScope(ImFont *font, float design) { ImGui::PushFont(font, overlay_font_size(font, design)); }
    ~OvlFontScope() { ImGui::PopFont(); }
};

/** @brief Je současná položka zakázaná (ImGui::BeginDisabled())? */
static bool ovl_is_disabled(void)
{
    return (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}

/**
 * @brief Pozice příští položky; v řádku nastavení ji svisle vystředí.
 * @param height Výška položky v pixelech.
 * @return Levý horní roh položky (souřadnice obrazovky).
 * @post V otevřeném řádku je kurzor okna posunutý na vrácenou pozici.
 */
static ImVec2 ovl_item_pos(float height)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (s_row.active) window->DC.CursorPos.y = floorf(s_row.items_y + (s_row.items_h - height) * 0.5f);
    return window->DC.CursorPos;
}

/**
 * @brief Vyzvedne a zruší vynucený obrys fokusu a zjistí skutečný fokus navigace.
 * @param id ID položky; 0 = položka bez ID.
 * @return true = prvek má kreslit obrys fokusu.
 */
static bool ovl_take_focus(ImGuiID id)
{
    const ImGuiContext &g = *GImGui;
    const bool forced = s_force_focus;
    s_force_focus = false;
    if (forced) return true;
    return id != 0 && g.NavId == id && g.NavCursorVisible && !g.CurrentWindow->DC.NavHideHighlightOneFrame;
}

bool ovl_last_item_focused(void)
{
    return ovl_take_focus(GImGui->LastItemData.ID);
}

void ovl_draw_focus_ring(ImVec2 min, ImVec2 max, float rounding)
{
    const float w = overlay_px(OVL_FOCUS_W);
    const float d = w * 0.5f;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - d, min.y - d), ImVec2(max.x + d, max.y + d), OVL_COL_ACCENT, rounding + d, 0, w);
}

/**
 * @brief Tooltip s textem zalomeným na OVL_TOOLTIP_WRAP (bez testu ukazatele myši).
 * @param text Text.
 */
static void ovl_tooltip_show(const char *text)
{
    if (!text) return;
    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(overlay_px(OVL_TOOLTIP_WRAP));
    ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void ovl_tooltip(const char *text)
{
    if (!text || !*text) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ovl_tooltip_show(text);
}

void ovl_tooltip_disabled(const char *text)
{
    if (!text || !*text) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ovl_tooltip_show(text);
}

/**
 * @brief Vykreslí text současným písmem, při přetečení s výpustkou.
 * @param dl    Seznam kreslení.
 * @param pos   Levý horní roh textu.
 * @param max_w Největší šířka v pixelech.
 * @param text  Začátek textu.
 * @param end   Konec textu.
 * @param col   Barva.
 * @return true = text se nevešel a byl zkrácen.
 */
static bool ovl_render_text(ImDrawList *dl, ImVec2 pos, float max_w, const char *text, const char *end, ImU32 col)
{
    if (max_w <= 0.0f || end <= text) return end > text;
    const ImVec2 ts = ImGui::CalcTextSize(text, end, false);
    if (ts.x <= max_w + 0.5f) {
        /* vejde se: bez výpustky (RenderTextEllipsis() by ji při zlomkových souřadnicích přidala i při shodné šířce) */
        dl->AddText(ImVec2(floorf(pos.x), floorf(pos.y)), col, text, end);
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(pos.x + max_w, pos.y + ts.y), pos.x + max_w, text, end, &ts);
    ImGui::PopStyleColor();
    return true;
}

/**
 * @brief Nakreslí čárkovaný obrys zaobleného obdélníku.
 * @param dl        Seznam kreslení.
 * @param a         Levý horní roh.
 * @param b         Pravý dolní roh.
 * @param rounding  Zaoblení rohů v pixelech.
 * @param col       Barva.
 * @param thickness Tloušťka čáry v pixelech.
 * @param dash      Délka čárky v pixelech (větší než 0).
 * @param gap       Délka mezery v pixelech.
 */
static void ovl_draw_dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col, float thickness, float dash, float gap)
{
    if (dash <= 0.0f) return;
    dl->PathRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), rounding);
    ImVector<ImVec2> pts;
    pts = dl->_Path;
    dl->PathClear();
    const int n = pts.Size;
    if (n < 2) return;
    const float period = dash + gap;
    float phase = 0.0f; /* poloha v periodě čárka + mezera */
    for (int i = 0; i < n; i++) {
        const ImVec2 p0 = pts[i];
        const ImVec2 p1 = pts[(i + 1) % n];
        const float dx = p1.x - p0.x, dy = p1.y - p0.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        float t = 0.0f;
        while (t < len) {
            const bool in_dash = phase < dash;
            const float rest = (in_dash ? dash : period) - phase;
            const float step = (rest < len - t) ? rest : (len - t);
            if (in_dash) {
                const float t0 = t / len, t1 = (t + step) / len;
                dl->AddLine(ImVec2(p0.x + dx * t0, p0.y + dy * t0), ImVec2(p0.x + dx * t1, p0.y + dy * t1), col, thickness);
            }
            t += step;
            phase += step;
            if (phase >= period) phase -= period;
        }
    }
}

bool ovl_draw_icon(ImDrawList *dl, ImVec2 pos, float size, const char *icon, ImU32 color)
{
    if (!icon || !*icon || size <= 0.0f) return false;

    if (strcmp(icon, OVL_ICON_EJECT) == 0) {
        /* trojúhelník nad vodorovnou čarou */
        float stroke = size * 0.075f;
        if (stroke < 1.0f) stroke = 1.0f;
        const float m = size * 0.22f;
        const float x0 = pos.x + m, x1 = pos.x + size - m, cx = pos.x + size * 0.5f;
        const ImVec2 tri[3] = {ImVec2(cx, pos.y + m), ImVec2(x1, pos.y + size * 0.58f), ImVec2(x0, pos.y + size * 0.58f)};
        dl->AddPolyline(tri, 3, color, ImDrawFlags_Closed, stroke);
        const float yb = pos.y + size - m;
        dl->AddLine(ImVec2(x0, yb), ImVec2(x1, yb), color, stroke);
        return true;
    }

    if (!overlay_style_has_icons()) return false;
    ImFont *font = overlay_font_ui();
    if (!font) return false;
    unsigned int c = 0;
    ImTextCharFromUtf8(&c, icon, NULL);
    const float font_size = floorf(size * OVL_ICON_FONT_SCALE);
    ImFontBaked *baked = font->GetFontBaked(font_size);
    const ImFontGlyph *glyph = baked ? baked->FindGlyphNoFallback((ImWchar)c) : NULL;
    if (!glyph) return false;
    /* kresbu glyfu vystředit ve čtverci (řádkové metriky písma se pro ikony nehodí) */
    const float gx = pos.x + (size - (glyph->X1 - glyph->X0)) * 0.5f - glyph->X0;
    const float gy = pos.y + (size - (glyph->Y1 - glyph->Y0)) * 0.5f - glyph->Y0;
    dl->AddText(font, font_size, ImVec2(floorf(gx + 0.5f), floorf(gy + 0.5f)), color, icon);
    return true;
}

/**
 * @brief Je ikona vykreslitelná (kreslená přes ImDrawList, nebo je načtené písmo ikon)?
 * @param icon Ikona nebo NULL.
 */
static bool ovl_icon_available(const char *icon)
{
    if (!icon || !*icon) return false;
    return strcmp(icon, OVL_ICON_EJECT) == 0 || overlay_style_has_icons();
}

ImVec2 ovl_keycap_size(const char *text)
{
    if (!text || !*text) return ImVec2(0.0f, 0.0f);
    OvlFontScope font(overlay_font_ui(), OVL_FONT_HINT);
    const ImVec2 ts = ImGui::CalcTextSize(text, NULL, false);
    const float h = floorf(ts.y + 2.0f * overlay_px(OVL_KEYCAP_PAD_Y));
    float w = floorf(ts.x + 2.0f * overlay_px(OVL_KEYCAP_PAD_X) + 0.5f);
    if (w < h) w = h;
    return ImVec2(w, h);
}

void ovl_draw_keycap(ImDrawList *dl, ImVec2 pos, const char *text, bool disabled)
{
    /* u zakázaného prvku se štítek nekreslí vůbec (místo mu zůstává, rozvržení se nemění) */
    if (!dl || !text || !*text || disabled) return;
    const ImVec2 size = ovl_keycap_size(text);
    OvlFontScope font(overlay_font_ui(), OVL_FONT_HINT);
    const ImVec2 ts = ImGui::CalcTextSize(text, NULL, false);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), disabled ? OVL_COL_BORDER : OVL_COL_BORDER_STRONG, overlay_px(OVL_RADIUS_CHIP), 0,
                overlay_border_px());
    dl->AddText(ImVec2(floorf(pos.x + (size.x - ts.x) * 0.5f), floorf(pos.y + (size.y - ts.y) * 0.5f)), disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY,
                text);
}

/* ================================================================
 * Karta a řádek nastavení
 * ================================================================ */

/**
 * @brief Nakreslí nadpis karty verzálkami a prostrkaně a oddělovací linku pod ním.
 * @param title Začátek nadpisu.
 * @param end   Konec nadpisu (před "##").
 * @pre Uvnitř okna karty, kurzor na začátku obsahu.
 */
static void ovl_card_title(const char *title, const char *end)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    gchar *upper = g_utf8_strup(title, (gssize)(end - title));
    if (!upper) return;
    ImFont *font = overlay_font_ui_bold();
    const float fs = overlay_font_px(OVL_FONT_CARD_TITLE);
    const float spacing = fs * 0.08f;
    const ImVec2 p = window->DC.CursorPos;
    float x = p.x;
    const char *s = upper;
    const char *s_end = upper + strlen(upper);
    while (s < s_end) {
        unsigned int c = 0;
        const int bytes = ImTextCharFromUtf8(&c, s, s_end);
        if (bytes <= 0) break;
        window->DrawList->AddText(font, fs, ImVec2(x, p.y), OVL_COL_TEXT_SECONDARY, s, s + bytes);
        x += font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s, s + bytes).x + spacing;
        s += bytes;
    }
    g_free(upper);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(floorf(x - p.x), fs));
    ImGui::Dummy(ImVec2(0.0f, floorf(overlay_px(10.0f))));
    const float y = floorf(window->DC.CursorPos.y);
    /* linka jde přes vnitřní okraj karty až k jejímu okraji, ale ne mimo výřez rodiče (rolovaný obsah tabu) */
    window->DrawList->PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max);
    window->DrawList->AddLine(ImVec2(window->Pos.x, y), ImVec2(window->Pos.x + window->Size.x, y), OVL_COL_BORDER, overlay_border_px());
    window->DrawList->PopClipRect();
    ImGui::Dummy(ImVec2(0.0f, floorf(overlay_px(10.0f))));
    ImGui::PopStyleVar();
}

bool ovl_card_begin(const char *title, ImVec2 size)
{
    if (!title) title = "##ovl_card";
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, overlay_px(OVL_RADIUS_CARD));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, overlay_border_px());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, OVL_COL_CARD);
    ImGui::PushStyleColor(ImGuiCol_Border, OVL_COL_BORDER);
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
    if (size.y <= 0.0f) child_flags |= ImGuiChildFlags_AutoResizeY;
    const bool visible = ImGui::BeginChild(title, ImVec2(size.x > 0.0f ? size.x : 0.0f, size.y > 0.0f ? size.y : 0.0f), child_flags,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    s_row_index = 0;
    s_row.active = false;
    if (visible) {
        const char *end = ImGui::FindRenderedTextEnd(title);
        if (end > title) ovl_card_title(title, end);
    }
    return visible;
}

void ovl_card_end(void)
{
    s_row.active = false;
    s_row_index = 0;
    ImGui::EndChild();
}

float ovl_card_height(float content_h, bool with_title)
{
    /* nadpis: řádek písma a dvě mezery kolem oddělovací linky (ovl_card_title()) */
    const float title_h = with_title ? overlay_font_px(OVL_FONT_CARD_TITLE) + 2.0f * floorf(overlay_px(10.0f)) : 0.0f;
    return 2.0f * floorf(overlay_px(OVL_CARD_PAD)) + title_h + content_h;
}

float ovl_row_height(void)
{
    return floorf(overlay_px(OVL_ROW_H));
}

void ovl_row_begin(const char *label)
{
    ovl_row_begin_ex(label, OVL_ROW_LABEL_FRACTION);
}

void ovl_row_begin_ex(const char *label, float label_fraction)
{
    if (!(label_fraction > 0.0f && label_fraction < 1.0f)) label_fraction = OVL_ROW_LABEL_FRACTION;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    s_row.active = true;
    s_row.start = window->DC.CursorPos;
    s_row.width = ImGui::GetContentRegionAvail().x;
    s_row.height = floorf(overlay_px(OVL_ROW_H));
    s_row.items_y = s_row.start.y;
    s_row.items_h = s_row.height;
    if (window->SkipItems) return;

    if (s_row_index > 0)
        window->DrawList->AddLine(ImVec2(s_row.start.x, floorf(s_row.start.y)), ImVec2(s_row.start.x + s_row.width, floorf(s_row.start.y)), OVL_COL_BORDER,
                                  overlay_border_px());
    s_row_index++;

    float label_col = 0.0f;
    if (label && *label) {
        OvlFontScope font(overlay_font_ui(), OVL_FONT_ROW_LABEL);
        label_col = floorf(s_row.width * label_fraction);
        const char *end = label + strlen(label);
        const ImVec2 ts = ImGui::CalcTextSize(label, end, false);
        const float max_w = label_col - overlay_px(OVL_W_GAP);
        const ImVec2 pos(s_row.start.x, floorf(s_row.start.y + (s_row.height - ts.y) * 0.5f));
        const ImRect bb(pos, ImVec2(pos.x + (ts.x < max_w ? ts.x : max_w), pos.y + ts.y));
        if (ImGui::ItemAdd(bb, 0)) {
            const bool truncated = ovl_render_text(window->DrawList, pos, max_w, label, end, ovl_is_disabled() ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT);
            if (truncated) ovl_tooltip(label);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(s_row.start.x + label_col, s_row.start.y));
}

float ovl_row_stacked_height(void)
{
    return floorf(overlay_px(OVL_ROW_STACKED_LABEL_H)) + floorf(overlay_px(OVL_ROW_STACKED_ITEMS_H));
}

void ovl_row_begin_stacked(const char *label)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const float label_h = floorf(overlay_px(OVL_ROW_STACKED_LABEL_H));
    s_row.active = true;
    s_row.start = window->DC.CursorPos;
    s_row.width = ImGui::GetContentRegionAvail().x;
    s_row.height = ovl_row_stacked_height();
    s_row.items_y = s_row.start.y + label_h;
    s_row.items_h = floorf(overlay_px(OVL_CONTROL_H));
    if (window->SkipItems) return;

    if (s_row_index > 0)
        window->DrawList->AddLine(ImVec2(s_row.start.x, floorf(s_row.start.y)), ImVec2(s_row.start.x + s_row.width, floorf(s_row.start.y)), OVL_COL_BORDER,
                                  overlay_border_px());
    s_row_index++;

    if (label && *label) {
        OvlFontScope font(overlay_font_ui(), OVL_FONT_ROW_LABEL);
        const char *end = label + strlen(label);
        const ImVec2 ts = ImGui::CalcTextSize(label, end, false);
        /* popisek sedí u spodního okraje svého pásu, blíž k prvkům, ke kterým patří */
        const ImVec2 pos(s_row.start.x, floorf(s_row.start.y + label_h - ts.y - overlay_px(4.0f)));
        const ImRect bb(pos, ImVec2(pos.x + (ts.x < s_row.width ? ts.x : s_row.width), pos.y + ts.y));
        if (ImGui::ItemAdd(bb, 0)) {
            const bool truncated = ovl_render_text(window->DrawList, pos, s_row.width, label, end, ovl_is_disabled() ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT);
            if (truncated) ovl_tooltip(label);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(s_row.start.x, s_row.items_y));
}

void ovl_row_right(float width)
{
    if (!s_row.active) return;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const float x = s_row.start.x + s_row.width - width;
    /* nikdy před současnou pozici: prvek širší než zbytek řádku nesmí překrýt popisek ani předchozí prvek */
    if (x > window->DC.CursorPos.x) window->DC.CursorPos.x = floorf(x);
}

void ovl_row_end(void)
{
    if (!s_row.active) return;
    s_row.active = false;
    ImGui::SetCursorScreenPos(s_row.start);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(s_row.width, s_row.height));
    ImGui::PopStyleVar();
}

/* ================================================================
 * Tlačítko
 * ================================================================ */

bool ovl_button(const char *label, const char *icon, const char *keycap, en_OVL_BUTTON_KIND kind)
{
    return ovl_button_ex(label, icon, keycap, kind, ImVec2(0.0f, 0.0f));
}

float ovl_button_natural_width(const char *label, const char *icon, const char *keycap)
{
    if (!label) return 0.0f;
    /* stejný výpočet jako přirozená šířka v ovl_button_ex() */
    const char *label_end = ImGui::FindRenderedTextEnd(label);
    const bool has_label = label_end > label;
    const bool has_icon = ovl_icon_available(icon);
    const ImVec2 kc = ovl_keycap_size(keycap);
    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    const float text_w = has_label ? ImGui::CalcTextSize(label, label_end, false).x : 0.0f;
    const float gap = overlay_px(OVL_W_GAP);
    float w = 2.0f * overlay_px(OVL_W_PAD_X) + (has_icon ? overlay_px(OVL_ICON) : 0.0f) + ((has_icon && has_label) ? gap : 0.0f) + text_w;
    if (kc.x > 0.0f) w += ((has_icon || has_label) ? gap * 1.5f : 0.0f) + kc.x;
    return ceilf(w);
}

std::string ovl_button_tooltip_text(const char *label, const char *label_end, const char *keycap)
{
    std::string tip;
    if (label && label_end > label) tip.assign(label, (size_t)(label_end - label));
    if (keycap && *keycap) {
        tip += " (";
        tip += keycap;
        tip += ")";
    }
    return tip;
}

bool ovl_button_ex(const char *label, const char *icon, const char *keycap, en_OVL_BUTTON_KIND kind, ImVec2 size_arg)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || !label) {
        s_force_focus = false;
        return false;
    }
    const ImGuiID id = window->GetID(label);
    const char *label_end = ImGui::FindRenderedTextEnd(label);
    const bool has_label = label_end > label;
    const bool has_icon = ovl_icon_available(icon);
    const ImVec2 kc = ovl_keycap_size(keycap);

    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    const ImVec2 ts = has_label ? ImGui::CalcTextSize(label, label_end, false) : ImVec2(0.0f, ImGui::GetFontSize());
    const float pad = overlay_px(OVL_W_PAD_X);
    const float gap = overlay_px(OVL_W_GAP);
    const float icon_sz = overlay_px(OVL_ICON);
    const float kc_gap = gap * 1.5f;

    float h = floorf((size_arg.y > 0.0f) ? size_arg.y : overlay_px(OVL_CONTROL_H));
    const float h_min = floorf(ts.y + 2.0f * overlay_px(OVL_W_PAD_Y));
    if (h < h_min) h = h_min;

    /* šířka bez klávesového štítku a se štítkem */
    float fixed_w = 2.0f * pad + (has_icon ? icon_sz : 0.0f) + ((has_icon && has_label) ? gap : 0.0f);
    if (has_icon && !has_label && kc.x <= 0.0f) fixed_w = h; /* jen ikona: čtverec */
    const float natural_plain = fixed_w + ts.x;
    const float natural = natural_plain + (kc.x > 0.0f ? ((has_icon || has_label) ? kc_gap : 0.0f) + kc.x : 0.0f);

    const float avail = ImGui::GetContentRegionAvail().x;
    float w;
    if (size_arg.x > 0.0f) w = size_arg.x;
    else if (size_arg.x < 0.0f) w = (avail > h) ? avail : h;
    else w = (natural > avail && avail > h) ? avail : natural;
    w = ceilf(w);
    if (size_arg.x <= 0.0f && w > avail && avail > h) w = floorf(avail); /* zaokrouhlení nesmí přesáhnout zbývající šířku */

    const ImVec2 pos = ovl_item_pos(h);
    const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) {
        s_force_focus = false;
        return false;
    }
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool disabled = ovl_is_disabled();
    const bool focused = ovl_take_focus(id);

    /* co se vejde: nejdřív ustoupí štítek, potom se zkrátí popisek */
    /* zakázaný prvek štítek zkratky neukazuje: štítek znamená "funguje právě teď" (K02, posudek R05) */
    const bool show_kc = !disabled && kc.x > 0.0f && natural <= w + 0.5f;
    float label_max = w - fixed_w - (show_kc ? kc_gap + kc.x : 0.0f);
    if (label_max < 0.0f) label_max = 0.0f;
    const bool truncated = has_label && ts.x > label_max + 0.5f;
    const float label_w = truncated ? label_max : ts.x;

    ImU32 col_text = OVL_COL_TEXT, col_icon = OVL_COL_TEXT, col_border = OVL_COL_BORDER_CONTROL;
    ImU32 col_bg = held ? OVL_COL_PRESSED : (hovered ? OVL_COL_SELECTED : OVL_COL_RAISED);
    if (kind == OVL_BUTTON_PRIMARY) {
        col_text = col_icon = OVL_COL_ACCENT_TEXT;
    } else if (kind == OVL_BUTTON_DANGER) {
        col_text = OVL_COL_RED_TEXT;
        col_icon = OVL_COL_RED;
    } else if (kind == OVL_BUTTON_ACTIVE) {
        col_text = col_icon = OVL_COL_GREEN_TEXT;
        col_border = OVL_COL_GREEN;
    } else if (kind == OVL_BUTTON_TEXT) {
        col_text = col_icon = (hovered || held) ? OVL_COL_TEXT : OVL_COL_TEXT_SECONDARY;
    }
    if (disabled) {
        col_text = col_icon = OVL_COL_TEXT_DISABLED;
        col_border = OVL_COL_BORDER_STRONG;
        col_bg = OVL_COL_CARD;
    }

    ImDrawList *dl = window->DrawList;
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    if (kind == OVL_BUTTON_DASHED) {
        if (!disabled && (hovered || held)) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_RAISED, rounding);
        ovl_draw_dashed_rect(dl, bb.Min, bb.Max, rounding, col_border, overlay_border_px(), overlay_px(OVL_DASH_LEN), overlay_px(OVL_DASH_GAP));
    } else if (kind == OVL_BUTTON_TEXT) {
        /* textové tlačítko: plocha jen jako odezva na myš, nikdy okraj (nesmí vypadat jako položka seznamu) */
        if (!disabled && (hovered || held)) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_RAISED, rounding);
    } else {
        dl->AddRectFilled(bb.Min, bb.Max, col_bg, rounding);
        dl->AddRect(bb.Min, bb.Max, col_border, rounding, 0, overlay_border_px());
    }

    /* obsah: samotná ikona na střed, jinak vždy zleva se stejným odsazením a štítek vpravo (posudek R07) */
    float x;
    if (has_icon && !has_label && !show_kc) x = bb.Min.x + (w - icon_sz) * 0.5f;
    else x = bb.Min.x + pad;
    x = floorf(x);
    if (has_icon) {
        ovl_draw_icon(dl, ImVec2(x, floorf(bb.Min.y + (h - icon_sz) * 0.5f)), icon_sz, icon, col_icon);
        x += icon_sz + (has_label ? gap : 0.0f);
    }
    if (has_label) ovl_render_text(dl, ImVec2(x, floorf(bb.Min.y + (h - ts.y) * 0.5f)), label_max, label, label_end, col_text);
    if (show_kc) ovl_draw_keycap(dl, ImVec2(floorf(bb.Max.x - pad - kc.x), floorf(bb.Min.y + (h - kc.y) * 0.5f)), keycap, disabled);

    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);

    if (!disabled && (truncated || (kc.x > 0.0f && !show_kc)) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        /* std::string: popisek libovolné délky, žádné řezání uprostřed znaku UTF-8 (review V7) */
        ovl_tooltip_show(ovl_button_tooltip_text(label, label_end, (kc.x > 0.0f && !show_kc) ? keycap : NULL).c_str());
    }
    return pressed;
}

/* ================================================================
 * Přepínač
 * ================================================================ */

ImVec2 ovl_toggle_size(void)
{
    return ImVec2(floorf(overlay_px(OVL_TOGGLE_W)), floorf(overlay_px(OVL_TOGGLE_H)));
}

bool ovl_toggle(const char *id_str, bool *value)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || !id_str || !value) {
        s_force_focus = false;
        return false;
    }
    const ImGuiID id = window->GetID(id_str);
    const ImVec2 size = ovl_toggle_size();
    const ImVec2 pos = ovl_item_pos(size.y);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) {
        s_force_focus = false;
        return false;
    }
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *value = !*value;
    const bool disabled = ovl_is_disabled();
    const bool focused = ovl_take_focus(id);
    const bool on = *value;

    ImDrawList *dl = window->DrawList;
    const float r = size.y * 0.5f;
    ImU32 col_track, col_border, col_knob;
    if (disabled) {
        col_track = on ? OVL_COL_BORDER_STRONG : OVL_COL_CARD;
        col_border = OVL_COL_BORDER_STRONG;
        col_knob = OVL_COL_TEXT_DISABLED;
    } else if (on) {
        col_track = col_border = OVL_COL_ACCENT;
        col_knob = OVL_COL_KNOB;
    } else {
        col_track = (hovered || held) ? OVL_COL_SELECTED : OVL_COL_RAISED;
        col_border = OVL_COL_BORDER_CONTROL;
        col_knob = OVL_COL_TEXT_SECONDARY;
    }
    dl->AddRectFilled(bb.Min, bb.Max, col_track, r);
    dl->AddRect(bb.Min, bb.Max, col_border, r, 0, overlay_border_px());
    const float knob_r = r - overlay_px(4.0f);
    const float cx = on ? bb.Max.x - r : bb.Min.x + r;
    dl->AddCircleFilled(ImVec2(cx, bb.Min.y + r), knob_r, col_knob);

    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, r);
    return pressed;
}

/* ================================================================
 * Segmentová volba
 * ================================================================ */

bool ovl_segmented(const char *id, const char *const *labels, int count, int *selected)
{
    return ovl_segmented_ex(id, labels, NULL, count, selected, 0.0f);
}

bool ovl_segmented_ex(const char *id_str, const char *const *labels, const char *const *keycaps, int count, int *selected, float width)
{
    return ovl_segmented_reasons(id_str, labels, keycaps, NULL, count, selected, width);
}

bool ovl_segmented_reasons(const char *id_str, const char *const *labels, const char *const *keycaps, const char *const *reasons, int count, int *selected, float width)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || !id_str || !labels || !selected || count < 1) {
        s_force_focus = false;
        return false;
    }
    /* víc segmentů je chyba volajícího: v ladicím buildu assert, jinak se přebytečné nekreslí */
    IM_ASSERT(count <= OVL_SEGMENT_MAX && "ovl_segmented: too many segments");
    if (count > OVL_SEGMENT_MAX) count = OVL_SEGMENT_MAX;

    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    const float pad = overlay_px(OVL_W_PAD_X);
    const float gap = overlay_px(OVL_W_GAP);
    float h = floorf(overlay_px(OVL_CONTROL_H));
    const float h_min = floorf(ImGui::GetFontSize() + 2.0f * overlay_px(OVL_W_PAD_Y));
    if (h < h_min) h = h_min;

    /* přirozené šířky segmentů se štítky a bez nich */
    float text_w[OVL_SEGMENT_MAX], kc_w[OVL_SEGMENT_MAX], seg_w[OVL_SEGMENT_MAX];
    float natural = 0.0f, natural_plain = 0.0f;
    for (int i = 0; i < count; i++) {
        const char *label = labels[i] ? labels[i] : "";
        text_w[i] = ImGui::CalcTextSize(label, NULL, false).x;
        kc_w[i] = keycaps ? ovl_keycap_size(keycaps[i]).x : 0.0f;
        natural_plain += 2.0f * pad + text_w[i];
        natural += 2.0f * pad + text_w[i] + (kc_w[i] > 0.0f ? gap + kc_w[i] : 0.0f);
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    float total;
    if (width > 0.0f) total = width;
    else if (width < 0.0f) total = avail;
    else total = (natural < avail) ? natural : avail;
    if (total < h * (float)count) total = h * (float)count;
    total = ceilf(total);
    /* nejmenší šířka (čtverec na segment) ani zaokrouhlení nesmí přesáhnout danou nebo zbývající šířku: v úzké
     * kartě se segmenty zúží a popisky zkrátí (review V10); pod jeden pixel na segment se nejde */
    const float limit = floorf((width > 0.0f) ? width : avail);
    if (total > limit) total = (limit > (float)count) ? limit : (float)count;

    const bool show_kc = natural <= total + 0.5f;
    const float base = show_kc ? natural : natural_plain;
    if (base <= total + 0.5f) {
        /* vejde se: zbytek šířky rovným dílem */
        const float extra = (total - base) / (float)count;
        for (int i = 0; i < count; i++) seg_w[i] = 2.0f * pad + text_w[i] + ((show_kc && kc_w[i] > 0.0f) ? gap + kc_w[i] : 0.0f) + extra;
    } else {
        /* nevejde se ani bez štítků: stejně široké segmenty, popisky se zkrátí */
        for (int i = 0; i < count; i++) seg_w[i] = total / (float)count;
    }

    const bool all_disabled = ovl_is_disabled();
    const bool force_focus = s_force_focus;
    s_force_focus = false;
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const float bw = overlay_border_px();
    const float line_w = overlay_px(OVL_FOCUS_W);
    ImDrawList *dl = window->DrawList;

    const ImVec2 origin = ovl_item_pos(h);
    const ImRect total_bb(origin, ImVec2(origin.x + total, origin.y + h));
    bool changed = false;
    int focus_index = force_focus ? count - 1 : -1;
    ImRect focus_bb;
    const char *tip = NULL;

    ImGui::BeginGroup();
    ImGui::PushID(id_str);
    float x = origin.x;
    for (int i = 0; i < count; i++) {
        const char *label = labels[i] ? labels[i] : "";
        const float x1 = (i == count - 1) ? total_bb.Max.x : floorf(x + seg_w[i]);
        const ImRect bb(ImVec2(x, origin.y), ImVec2(x1, origin.y + h));
        const ImGuiID id = window->GetID(i);
        if (i > 0) ImGui::SameLine(0.0f, 0.0f);
        window->DC.CursorPos = bb.Min;
        ImGui::ItemSize(bb.GetSize());
        x = x1;
        /* segment s důvodem zákazu nereaguje; zvolený segment zakázaný není (jeho zvolení nic nemění) */
        const char *reason = (reasons && reasons[i] && *selected != i) ? reasons[i] : NULL;
        const bool disabled = all_disabled || reason != NULL;
        if (reason) ImGui::BeginDisabled();
        const bool added = ImGui::ItemAdd(bb, id);
        bool hovered = false, held = false;
        if (added && ImGui::ButtonBehavior(bb, id, &hovered, &held) && *selected != i) {
            *selected = i;
            changed = true;
        }
        if (reason) ImGui::EndDisabled();
        if (!added) continue;
        if (reason && *reason && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) tip = reason;
        if (ovl_take_focus(id) && focus_index < 0) focus_index = i;
        if (focus_index == i) focus_bb = bb;

        const bool is_selected = (*selected == i);
        const ImDrawFlags corners = ((i == 0) ? ImDrawFlags_RoundCornersLeft : 0) | ((i == count - 1) ? ImDrawFlags_RoundCornersRight : 0);
        const ImDrawFlags fill_flags = corners ? corners : ImDrawFlags_RoundCornersNone;
        if (is_selected) dl->AddRectFilled(bb.Min, bb.Max, disabled ? OVL_COL_RAISED : OVL_COL_SELECTED, rounding, fill_flags);
        else if (!disabled && (hovered || held)) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_PRESSED : OVL_COL_RAISED, rounding, fill_flags);
        if (is_selected) {
            /* akcentní linka dole; u krajních segmentů uhne zaoblení */
            const float lx0 = bb.Min.x + ((i == 0) ? rounding : 0.0f);
            const float lx1 = bb.Max.x - ((i == count - 1) ? rounding : 0.0f);
            dl->AddRectFilled(ImVec2(lx0, bb.Max.y - bw - line_w), ImVec2(lx1, bb.Max.y - bw), disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_ACCENT);
        }
        if (i > 0) dl->AddLine(ImVec2(bb.Min.x, bb.Min.y + bw), ImVec2(bb.Min.x, bb.Max.y - bw), disabled ? OVL_COL_BORDER : OVL_COL_BORDER_STRONG, bw);

        const bool kc_here = show_kc && kc_w[i] > 0.0f && !disabled;
        float inner = bb.GetWidth() - 2.0f * pad - (kc_here ? gap + kc_w[i] : 0.0f);
        if (inner < 0.0f) inner = 0.0f;
        const float label_w = (text_w[i] < inner) ? text_w[i] : inner;
        const float content_w = label_w + (kc_here ? gap + kc_w[i] : 0.0f);
        const float cx = floorf(bb.Min.x + (bb.GetWidth() - content_w) * 0.5f);
        const ImU32 col_text = disabled ? OVL_COL_TEXT_DISABLED : (is_selected ? OVL_COL_TEXT : OVL_COL_TEXT_SECONDARY);
        const bool truncated = ovl_render_text(dl, ImVec2(cx, floorf(bb.Min.y + (h - ImGui::GetFontSize()) * 0.5f)), inner, label, label + strlen(label), col_text);
        if (kc_here) {
            const ImVec2 kc = ovl_keycap_size(keycaps[i]);
            ovl_draw_keycap(dl, ImVec2(cx + label_w + gap, floorf(bb.Min.y + (h - kc.y) * 0.5f)), keycaps[i], disabled);
        }
        if (truncated && !disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) tip = label;
    }
    ImGui::PopID();
    ImGui::EndGroup();

    dl->AddRect(total_bb.Min, total_bb.Max, all_disabled ? OVL_COL_BORDER_STRONG : OVL_COL_BORDER_CONTROL, rounding, 0, bw);
    if (focus_index >= 0 && focus_bb.GetWidth() > 0.0f) ovl_draw_focus_ring(focus_bb.Min, focus_bb.Max, rounding);
    if (tip) ovl_tooltip_show(tip);
    return changed;
}

/* ================================================================
 * Posuvník
 * ================================================================ */

bool ovl_slider_percent(const char *id, float *value, float min, float max)
{
    return ovl_slider_percent_ex(id, value, min, max, 1.0f, 0.0f);
}

/**
 * @brief Omezí hodnotu posuvníku na rozsah a zaokrouhlí ji na násobek kroku od min.
 * @param v    Hodnota.
 * @param min  Nejmenší hodnota.
 * @param max  Největší hodnota.
 * @param step Krok (větší než 0).
 * @return Upravená hodnota v rozsahu min až max.
 */
static float ovl_slider_snap(float v, float min, float max, float step)
{
    v = min + floorf((v - min) / step + 0.5f) * step;
    if (v < min) v = min;
    if (v > max) v = max;
    return v;
}

bool ovl_slider_percent_ex(const char *id_str, float *value, float min, float max, float step, float width)
{
    return ovl_slider_unit_ex(id_str, value, min, max, step, width, "%");
}

/**
 * @brief Společné tělo posuvníků: hodnota s jednotkou, volitelně značka význačné hodnoty s přichycením.
 *
 * Parametry jako ovl_slider_unit_ex() a ovl_slider_percent_mark().
 *
 * @param id_str   Řetězec pro ID prvku.
 * @param value    Hodnota.
 * @param min      Nejmenší hodnota.
 * @param max      Největší hodnota (větší než min).
 * @param step     Krok klávesnice a zaokrouhlení; <= 0 = 1.
 * @param width    Šířka celého prvku; <= 0 = zbývající šířka.
 * @param unit     Text jednotky; NULL nebo prázdný = jen číslo.
 * @param has_mark true = kreslit značku hodnoty `mark` a přichytávat k ní.
 * @param mark     Hodnota se značkou (min < mark < max).
 * @param detent   Polovina šířky pásma přichycení myši; <= 0 = bez přichycení.
 * @return true = hodnota se změnila.
 */
static bool ovl_slider_impl(const char *id_str, float *value, float min, float max, float step, float width, const char *unit, bool has_mark, float mark, float detent);

bool ovl_slider_unit_ex(const char *id_str, float *value, float min, float max, float step, float width, const char *unit)
{
    return ovl_slider_impl(id_str, value, min, max, step, width, unit, false, 0.0f, 0.0f);
}

bool ovl_slider_percent_mark(const char *id_str, float *value, float min, float max, float step, float width, float mark, float detent)
{
    const bool has_mark = (mark > min) && (mark < max);
    return ovl_slider_impl(id_str, value, min, max, step, width, "%", has_mark, mark, detent);
}

static bool ovl_slider_impl(const char *id_str, float *value, float min, float max, float step, float width, const char *unit, bool has_mark, float mark, float detent)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (!unit) unit = "";
    if (window->SkipItems || !id_str || !value || !(max > min)) {
        s_force_focus = false;
        return false;
    }
    if (step <= 0.0f) step = 1.0f;
    ImGuiContext &g = *GImGui;
    const ImGuiID id = window->GetID(id_str);

    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    const float knob_r = overlay_px(OVL_SLIDER_KNOB_R);
    const float gap = overlay_px(OVL_W_GAP);
    /* pole číselného údaje podle nejširší hodnoty rozsahu */
    char text[32];
    snprintf(text, sizeof(text), "%.0f%s%s", (fabsf(max) > fabsf(min)) ? max : min, *unit ? " " : "", unit);
    const float field_w = floorf(ImGui::CalcTextSize(text, NULL, false).x + gap * 2.0f);

    float h = floorf(overlay_px(OVL_SLIDER_H));
    if (h < ImGui::GetFontSize()) h = ImGui::GetFontSize();
    /* šířka: daná, jinak zbývající. Prvek ji nepřesáhne (v úzké kartě se zkrátí drážka, review V10); jen pod
     * tvrdou mez (jezdec, číselný údaj a zbytek drážky OVL_SLIDER_TRACK_HARD_MIN) se už nezmenší. */
    const float w_given = floorf((width > 0.0f) ? width : ImGui::GetContentRegionAvail().x);
    const float w_hard = ceilf(overlay_px(OVL_SLIDER_TRACK_HARD_MIN) + overlay_px(2.0f) + 2.0f * knob_r + field_w);
    const float w = (w_given > w_hard) ? w_given : w_hard;

    const ImVec2 pos = ovl_item_pos(h);
    const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) {
        s_force_focus = false;
        return false;
    }
    const float x0 = bb.Min.x + knob_r + overlay_px(2.0f);
    const float x1 = bb.Max.x - field_w - knob_r;
    /* myš bere jen pásmo drážky s jezdcem: klik na číselný údaj hodnotu nemění */
    const ImRect track_bb(bb.Min, ImVec2(x1 + knob_r, bb.Max.y));
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(track_bb, id, &hovered, &held);
    const bool disabled = ovl_is_disabled();
    const bool focused = ovl_take_focus(id);

    const float old = *value;
    float v = *value;
    if (v < min) v = min;
    if (v > max) v = max;

    if (!disabled) {
        if (held && g.ActiveIdSource == ImGuiInputSource_Mouse) {
            float t = (g.IO.MousePos.x - x0) / (x1 - x0);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            v = ovl_slider_snap(min + t * (max - min), min, max, step);
            /* značka: tažení se u ní přichytí, dvojklik na ni hodnotu vrátí */
            if (has_mark && ((detent > 0.0f && fabsf(v - mark) <= detent) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) v = mark;
        }
        if (g.NavId == id && g.NavWindow && g.NavWindow->RootWindow == window->RootWindow) {
            /* vodorovné šipky a Home / End patří při fokusu posuvníku (navigace je příští snímek nedostane) */
            ImGui::SetKeyOwner(ImGuiKey_LeftArrow, id);
            ImGui::SetKeyOwner(ImGuiKey_RightArrow, id);
            ImGui::SetKeyOwner(ImGuiKey_Home, id);
            ImGui::SetKeyOwner(ImGuiKey_End, id);
            float nv = v;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, id)) nv -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, id)) nv += step;
            if (ImGui::IsKeyPressed(ImGuiKey_Home, 0, id)) nv = min;
            if (ImGui::IsKeyPressed(ImGuiKey_End, 0, id)) nv = max;
            if (nv != v) {
                v = ovl_slider_snap(nv, min, max, step);
                ImGui::SetNavCursorVisible(true);
            }
        }
        *value = v;
    }
    /* zakázaný posuvník kreslí z omezené kopie a hodnotu volajícího nemění */

    ImDrawList *dl = window->DrawList;
    const float cy = floorf(bb.Min.y + h * 0.5f);
    const float th = overlay_px(OVL_SLIDER_TRACK_H) * 0.5f;
    const float kx = x0 + (x1 - x0) * ((v - min) / (max - min));
    dl->AddRectFilled(ImVec2(x0, cy - th), ImVec2(x1, cy + th), disabled ? OVL_COL_BORDER_STRONG : OVL_COL_MUTED, th);
    dl->AddRectFilled(ImVec2(x0, cy - th), ImVec2(kx, cy + th), disabled ? OVL_COL_MUTED_DIM : OVL_COL_ACCENT, th);
    if (has_mark) {
        /* značka význačné hodnoty: svislá čárka přes drážku */
        const float mx = floorf(x0 + (x1 - x0) * ((mark - min) / (max - min)));
        const float mh = floorf(th + overlay_px(3.0f));
        dl->AddRectFilled(ImVec2(mx, cy - mh), ImVec2(mx + overlay_px(1.0f), cy + mh), disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY);
    }
    dl->AddCircleFilled(ImVec2(kx, cy), knob_r, disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_KNOB);
    if (!disabled && (hovered || held)) dl->AddCircle(ImVec2(kx, cy), knob_r + overlay_px(1.0f), OVL_COL_ACCENT, 0, overlay_px(OVL_FOCUS_W));

    snprintf(text, sizeof(text), "%.0f%s%s", v, *unit ? " " : "", unit);
    const ImVec2 ts = ImGui::CalcTextSize(text, NULL, false);
    dl->AddText(ImVec2(floorf(bb.Max.x - ts.x - gap * 0.75f), floorf(bb.Min.y + (h - ts.y) * 0.5f)), disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT, text);

    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, overlay_px(OVL_RADIUS_CONTROL));
    return !disabled && v != old;
}

/* ================================================================
 * Rozbalovací výběr
 * ================================================================ */

/**
 * @brief Výška řádku položky rozbaleného seznamu.
 * @param item Položka.
 * @return Výška v pixelech: položka a nadpis oddílu OVL_DROPDOWN_ITEM_H, oddělovač bez nadpisu OVL_DROPDOWN_SEPARATOR_H.
 */
static float ovl_dropdown_row_h(const st_OVL_DROPDOWN_ITEM *item)
{
    if (item->section && !(item->label && *item->label)) return floorf(overlay_px(OVL_DROPDOWN_SEPARATOR_H));
    return floorf(overlay_px(OVL_DROPDOWN_ITEM_H));
}

bool ovl_dropdown(const char *id_str, const st_OVL_DROPDOWN_ITEM *items, int count, int *selected, float width)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || !id_str || !items || !selected || count < 1) {
        s_force_focus = false;
        return false;
    }
    ImGuiContext &g = *GImGui;
    const ImGuiID id = window->GetID(id_str);

    OvlFontScope font(overlay_font_ui(), OVL_FONT_BODY);
    const float pad = overlay_px(OVL_W_PAD_X);
    const float gap = overlay_px(OVL_W_GAP);
    const float icon_sz = overlay_px(OVL_ICON);
    float h = floorf(overlay_px(OVL_CONTROL_H));
    const float h_min = floorf(ImGui::GetFontSize() + 2.0f * overlay_px(OVL_W_PAD_Y));
    if (h < h_min) h = h_min;
    float w = (width > 0.0f) ? width : ImGui::GetContentRegionAvail().x;
    const float w_min = 2.0f * pad + icon_sz + gap + overlay_px(OVL_DROPDOWN_TEXT_MIN);
    if (w < w_min) w = w_min;
    w = floorf(w);

    const ImVec2 pos = ovl_item_pos(h);
    const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id)) {
        s_force_focus = false;
        return false;
    }
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool disabled = ovl_is_disabled();
    const bool focused = ovl_take_focus(id);

    char popup_name[32];
    snprintf(popup_name, sizeof(popup_name), "##ovl_dd_%08X", (unsigned)id);
    if (pressed && !disabled) ImGui::OpenPopup(popup_name);
    const bool open = ImGui::IsPopupOpen(popup_name);

    /* zavřený prvek: text zvolené položky a šipka */
    const bool valid = *selected >= 0 && *selected < count && !items[*selected].section;
    const char *preview = (valid && items[*selected].label) ? items[*selected].label : "";
    ImDrawList *dl = window->DrawList;
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const ImU32 col_bg = disabled ? OVL_COL_CARD : ((held || open) ? OVL_COL_PRESSED : (hovered ? OVL_COL_SELECTED : OVL_COL_RAISED));
    dl->AddRectFilled(bb.Min, bb.Max, col_bg, rounding);
    dl->AddRect(bb.Min, bb.Max, disabled ? OVL_COL_BORDER_STRONG : OVL_COL_BORDER_CONTROL, rounding, 0, overlay_border_px());
    const float text_max = w - 2.0f * pad - icon_sz - gap;
    const bool truncated = ovl_render_text(dl, ImVec2(floorf(bb.Min.x + pad), floorf(bb.Min.y + (h - ImGui::GetFontSize()) * 0.5f)), text_max, preview,
                                           preview + strlen(preview), disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT);
    ovl_draw_icon(dl, ImVec2(floorf(bb.Max.x - pad - icon_sz), floorf(bb.Min.y + (h - icon_sz) * 0.5f)), icon_sz, ICON_LC_CHEVRON_DOWN,
                  disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY);
    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);
    if (truncated && !disabled && !open && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ovl_tooltip_show(preview);
    if (!open) return false;

    /* rozbalený seznam: pod prvkem, nebo nad ním, je-li tam víc místa; uvnitř hlavního viewportu a nad dolní mezí volajícího */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float vp_bottom = vp->Pos.y + vp->Size.y;
    const bool limited = s_dropdown_bottom_frame == ImGui::GetFrameCount() && s_dropdown_bottom < vp_bottom;
    if (limited) vp_bottom = s_dropdown_bottom;
    const float edge = floorf(overlay_px(OVL_DROPDOWN_GAP));
    const float list_pad = floorf(overlay_px(OVL_DROPDOWN_PAD));
    float content_h = 0.0f, content_w = 0.0f;
    for (int i = 0; i < count; i++) {
        content_h += ovl_dropdown_row_h(&items[i]);
        const float tw = items[i].label ? ImGui::CalcTextSize(items[i].label, NULL, false).x : 0.0f;
        const float iw = 2.0f * pad + (items[i].section ? 0.0f : icon_sz + gap) + tw;
        if (iw > content_w) content_w = iw;
    }
    float list_h = content_h + 2.0f * list_pad;
    /* mez volajícího (hrana patičky) je sama okrajem; od hrany viewportu se drží odstup */
    const float below = vp_bottom - (limited ? 0.0f : edge) - (bb.Max.y + edge);
    const float above = bb.Min.y - edge - (vp->Pos.y + edge);
    const bool up = list_h > below && above > below;
    const float room = up ? above : below;
    const bool scroll = list_h > room;
    if (scroll) list_h = (room > 2.0f * list_pad) ? floorf(room) : 2.0f * list_pad + overlay_px(1.0f);
    float list_w = ceilf(content_w + 2.0f * list_pad + (scroll ? g.Style.ScrollbarSize : 0.0f));
    if (list_w < w) list_w = w;
    const float w_room = vp->Size.x - 2.0f * edge;
    if (list_w > w_room) list_w = floorf(w_room);
    float list_x = bb.Min.x;
    if (list_x + list_w > vp->Pos.x + vp->Size.x - edge) list_x = vp->Pos.x + vp->Size.x - edge - list_w;
    if (list_x < vp->Pos.x + edge) list_x = vp->Pos.x + edge;
    ImGui::SetNextWindowPos(ImVec2(floorf(list_x), floorf(up ? bb.Min.y - edge - list_h : bb.Max.y + edge)));
    ImGui::SetNextWindowSize(ImVec2(list_w, list_h));

    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(list_pad, list_pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, OVL_COL_RAISED);
    ImGui::PushStyleColor(ImGuiCol_Border, OVL_COL_BORDER_CONTROL);
    if (ImGui::BeginPopup(popup_name, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        s_dropdown_frame = ImGui::GetFrameCount();
        ImGuiWindow *list = ImGui::GetCurrentWindow();
        ImDrawList *ldl = list->DrawList;
        const float item_w = ImGui::GetContentRegionAvail().x;
        const float chip = overlay_px(OVL_RADIUS_CHIP);
        for (int i = 0; i < count; i++) {
            const st_OVL_DROPDOWN_ITEM *item = &items[i];
            const float row_h = ovl_dropdown_row_h(item);
            const ImVec2 p = list->DC.CursorPos;
            const ImRect rb(p, ImVec2(p.x + item_w, p.y + row_h));
            const char *label = item->label ? item->label : "";
            if (item->section) {
                /* nadpis oddílu vedlejší barvou, nebo jen oddělovací linka */
                ImGui::ItemSize(rb.GetSize());
                if (!ImGui::ItemAdd(rb, 0)) continue;
                if (*label) {
                    const float fs = overlay_font_px(OVL_FONT_HINT);
                    ldl->AddText(overlay_font_ui_bold(), fs, ImVec2(floorf(rb.Min.x + pad), floorf(rb.Min.y + (row_h - fs) * 0.5f)), OVL_COL_TEXT_SECONDARY, label);
                } else {
                    const float ly = floorf(rb.Min.y + row_h * 0.5f);
                    ldl->AddLine(ImVec2(rb.Min.x, ly), ImVec2(rb.Max.x, ly), OVL_COL_BORDER_STRONG, overlay_border_px());
                }
                continue;
            }
            ImGui::PushID(i);
            const ImGuiID item_id = list->GetID("##item");
            ImGui::PopID();
            ImGui::ItemSize(rb.GetSize());
            const bool item_disabled = item->reason != NULL;
            if (item_disabled) ImGui::BeginDisabled();
            const bool added = ImGui::ItemAdd(rb, item_id);
            bool item_hovered = false, item_held = false;
            const bool item_pressed = added && ImGui::ButtonBehavior(rb, item_id, &item_hovered, &item_held);
            if (item_disabled) ImGui::EndDisabled();
            if (!added) continue;
            if (i < OVL_DROPDOWN_TEST_MAX) {
                s_dropdown_rects[i].frame = s_dropdown_frame;
                s_dropdown_rects[i].min = rb.Min;
                s_dropdown_rects[i].max = rb.Max;
            }
            const bool is_selected = (i == *selected);
            /* po rozbalení klávesnicí dostane fokus zvolená položka */
            if (is_selected) ImGui::SetItemDefaultFocus();
            const bool item_focused = g.NavId == item_id && g.NavCursorVisible;
            if (!item_disabled && (item_hovered || item_held || item_focused)) ldl->AddRectFilled(rb.Min, rb.Max, item_held ? OVL_COL_PRESSED : OVL_COL_SELECTED, chip);
            if (is_selected)
                ovl_draw_icon(ldl, ImVec2(floorf(rb.Min.x + pad * 0.5f), floorf(rb.Min.y + (row_h - icon_sz) * 0.5f)), icon_sz, ICON_LC_CHECK,
                              item_disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_ACCENT_TEXT);
            const float tx = rb.Min.x + pad * 0.5f + icon_sz + gap;
            const bool item_truncated = ovl_render_text(ldl, ImVec2(floorf(tx), floorf(rb.Min.y + (row_h - ImGui::GetFontSize()) * 0.5f)), rb.Max.x - pad * 0.5f - tx, label,
                                                        label + strlen(label), item_disabled ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT);
            /* obrys posunutý dovnitř o polovinu své šířky (střed čáry leží na hraně řádku), v jednotkách overlaye */
            const float ring_in = overlay_px(OVL_FOCUS_W) * 0.5f;
            if (item_focused) ovl_draw_focus_ring(ImVec2(rb.Min.x + ring_in, rb.Min.y + ring_in), ImVec2(rb.Max.x - ring_in, rb.Max.y - ring_in), chip);
            if (item_disabled) {
                if (*item->reason && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ovl_tooltip_show(item->reason);
            } else if (item_truncated && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ovl_tooltip_show(label);
            }
            if (item_pressed && !item_disabled) {
                if (!is_selected) {
                    *selected = i;
                    changed = true;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    return changed;
}

void ovl_dropdown_set_bottom_limit(float y)
{
    if (!ImGui::GetCurrentContext()) return;
    s_dropdown_bottom = y;
    s_dropdown_bottom_frame = ImGui::GetFrameCount();
}

bool ovl_dropdown_is_open(void)
{
    if (s_dropdown_frame < 0 || !ImGui::GetCurrentContext()) return false;
    return ImGui::GetFrameCount() - s_dropdown_frame <= 1;
}

bool ovl_test_dropdown_item_rect(int index, ImVec2 *min, ImVec2 *max)
{
    if (index < 0 || index >= OVL_DROPDOWN_TEST_MAX || s_dropdown_frame < 0) return false;
    if (s_dropdown_rects[index].frame != s_dropdown_frame || !ovl_dropdown_is_open()) return false;
    if (min) *min = s_dropdown_rects[index].min;
    if (max) *max = s_dropdown_rects[index].max;
    return true;
}

/* ================================================================
 * Pasivní prvky
 * ================================================================ */

void ovl_keycap(const char *text)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || !text || !*text) return;
    const ImVec2 size = ovl_keycap_size(text);
    const ImVec2 pos = ovl_item_pos(size.y);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    ovl_draw_keycap(window->DrawList, pos, text, ovl_is_disabled());
}

void ovl_text_ellipsis(const char *text, float max_width)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    if (!text) text = "";
    if (max_width <= 0.0f) max_width = ImGui::GetContentRegionAvail().x;
    if (max_width < 1.0f) max_width = 1.0f;
    const char *end = text + strlen(text);
    const ImVec2 ts = ImGui::CalcTextSize(text, end, false);
    const bool truncated = ts.x > max_width + 0.5f;
    const ImVec2 pos = ovl_item_pos(ts.y);
    const ImRect bb(pos, ImVec2(pos.x + (truncated ? floorf(max_width) : ts.x), pos.y + ts.y));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    /* v zakázaném stavu barva zakázaného textu bez ohledu na barvu textu ImGui (review V6) */
    const ImU32 col = ovl_is_disabled() ? OVL_COL_TEXT_DISABLED : ImGui::GetColorU32(ImGuiCol_Text);
    s_test_last_text_col = col;
    if (truncated) {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::RenderTextEllipsis(window->DrawList, bb.Min, bb.Max, bb.Max.x, text, end, &ts);
        ImGui::PopStyleColor();
        ovl_tooltip(text);
    } else {
        window->DrawList->AddText(ImVec2(floorf(bb.Min.x), floorf(bb.Min.y)), col, text, end);
    }
}

/**
 * @brief Rozměry stavového štítku.
 * @param icon Ikona nebo NULL.
 * @param text Text.
 * @param[out] text_size Velikost textu (smí být NULL).
 * @param[out] icon_size Strana čtverce ikony, 0 bez ikony (smí být NULL).
 * @return Velikost štítku v pixelech.
 */
static ImVec2 ovl_status_chip_calc(const char *icon, const char *text, ImVec2 *text_size, float *icon_size)
{
    OvlFontScope font(overlay_font_ui_bold(), OVL_FONT_HINT);
    const ImVec2 ts = ImGui::CalcTextSize(text ? text : "", NULL, false);
    const float isz = ovl_icon_available(icon) ? ImGui::GetFontSize() : 0.0f;
    if (text_size) *text_size = ts;
    if (icon_size) *icon_size = isz;
    /* mezera mezi ikonou a textem jen tehdy, když text je (štítek se samotnou ikonou) */
    const float w = 2.0f * overlay_px(OVL_CHIP_PAD_X) + ts.x + (isz > 0.0f ? isz + (ts.x > 0.0f ? overlay_px(4.0f) : 0.0f) : 0.0f);
    return ImVec2(floorf(w + 0.5f), floorf(ImGui::GetFontSize() + 2.0f * overlay_px(OVL_CHIP_PAD_Y)));
}

ImVec2 ovl_status_chip_size(const char *icon, const char *text)
{
    return ovl_status_chip_calc(icon, text, NULL, NULL);
}

void ovl_status_chip(const char *text, en_OVL_STATUS status)
{
    ovl_status_chip_icon(NULL, text, status);
}

void ovl_status_chip_icon(const char *icon, const char *text, en_OVL_STATUS status)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    if (!text) text = "";
    ImVec2 ts;
    float isz = 0.0f;
    const ImVec2 size = ovl_status_chip_calc(icon, text, &ts, &isz);
    const ImVec2 pos = ovl_item_pos(size.y);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;

    ImU32 col_border = OVL_COL_BORDER_STRONG, col_text = OVL_COL_TEXT;
    bool tinted = true;
    switch (status) {
        case OVL_STATUS_ACTIVE:
            col_border = OVL_COL_GREEN;
            col_text = OVL_COL_GREEN_TEXT;
            break;
        case OVL_STATUS_WARNING:
            col_border = col_text = OVL_COL_AMBER;
            break;
        case OVL_STATUS_DANGER:
            col_border = OVL_COL_RED;
            col_text = OVL_COL_RED_TEXT;
            break;
        case OVL_STATUS_NEUTRAL:
        default:
            tinted = false;
            break;
    }
    if (ovl_is_disabled()) {
        /* zakázaný stav: bez barvy stavu (review V6) */
        col_border = OVL_COL_BORDER_STRONG;
        col_text = OVL_COL_TEXT_DISABLED;
        tinted = false;
    }
    s_test_last_text_col = col_text;
    ImDrawList *dl = window->DrawList;
    const float rounding = overlay_px(OVL_RADIUS_CHIP);
    if (tinted) dl->AddRectFilled(bb.Min, bb.Max, overlay_color_alpha(col_border, 0.12f), rounding);
    dl->AddRect(bb.Min, bb.Max, col_border, rounding, 0, overlay_border_px());

    OvlFontScope font(overlay_font_ui_bold(), OVL_FONT_HINT);
    float x = bb.Min.x + overlay_px(OVL_CHIP_PAD_X);
    if (isz > 0.0f) {
        ovl_draw_icon(dl, ImVec2(floorf(x), floorf(bb.Min.y + (size.y - isz) * 0.5f)), isz, icon, col_text);
        x += isz + overlay_px(4.0f);
    }
    dl->AddText(ImVec2(floorf(x), floorf(bb.Min.y + (size.y - ts.y) * 0.5f)), col_text, text);
}

void ovl_led(bool on)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const float size = floorf(overlay_px(OVL_LED_SIZE));
    const ImVec2 pos = ovl_item_pos(size);
    const ImRect bb(pos, ImVec2(pos.x + size, pos.y + size));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    /* v zakázaném stavu se kreslí jako zhasnutá (review V6) */
    const ImU32 col = (on && !ovl_is_disabled()) ? OVL_COL_GREEN : OVL_COL_MUTED;
    s_test_last_text_col = col;
    window->DrawList->AddCircleFilled(ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.5f, col);
}

void ovl_icon(const char *icon, ImU32 color)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const float size = floorf(overlay_px(OVL_ICON));
    const ImVec2 pos = ovl_item_pos(size);
    const ImRect bb(pos, ImVec2(pos.x + size, pos.y + size));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    /* v zakázaném stavu barva zakázaného textu bez ohledu na předanou barvu (review V6) */
    if (ovl_is_disabled()) color = OVL_COL_TEXT_DISABLED;
    s_test_last_text_col = color;
    ovl_draw_icon(window->DrawList, pos, size, icon, color);
}
