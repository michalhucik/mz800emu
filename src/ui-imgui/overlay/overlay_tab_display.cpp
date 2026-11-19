/**
 * @file   overlay_tab_display.cpp
 * @brief  Tab Display overlay menu: okno a fullscreen, barevné schéma, scanlines CRT a řízení snímkové frekvence.
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW). Každý ovládací prvek je
 * akce z tabulky akcí (overlay_actions.h) a zvolení akci jen vyžádá;
 * provede ji rámec po vykreslení okna. Funkce akcí jsou tytéž, které volá
 * klasické menu Interface > Display a zkratky Alt+Ctrl+Enter, Alt+W a
 * Alt+H. Podle konceptu display-v1 a rulingů K02, K03, K04, K07, K08 a
 * K13 (gui/DECISIONS.md experimentu). Z display.h se berou jen konstanty
 * (hodnoty výčtů a meze), stav jádra se tu nečte.
 *
 * Karty shora dolů, nejdřív ty, jejichž účinek je vidět na obraze: COLORS
 * (Color schema Normal / Grayscale / Green), CRT SCANLINES (Enable
 * scanlines, Intensity), WINDOW (Fullscreen se skutečným stavem okna,
 * Window size on startup, Lock window aspect ratio, Fix aspect ratio By
 * width / By height) a FRAME RATE (Frame rate control, v režimu Custom
 * FPS řádek s hodnotou, Forced redraw every screen). Pod kartami
 * věta "Changes apply immediately.". Náhled scanlines z konceptu se
 * nekreslí: plocha overlaye má na tomto tabu krytí 55 % (K04) a pod ní je
 * vidět skutečný obraz. Windows HDR a měření FPS zůstávají v klasickém
 * menu.
 *
 * Rozvržení: všechny rozměry jsou násobky jednotky overlaye. Karty jsou v
 * jednom sloupci pod sebou; v širokém okně zabírá sloupec jen
 * DISPLAY_COLUMN_FRACTION šířky obsahu u levého okraje, aby pod průhlednější
 * plochou overlaye zůstala vidět většina obrazu stroje, a podle potřeby se
 * svisle roluje jen tento sloupec (vnořené okno "###ovl_disp_col" s
 * posuvníkem hned u pravého okraje karet). V úzkém okně
 * (overlay_is_narrow()) mají karty celou šířku a roluje se celý obsah tabu.
 * Výšky karet se počítají předem.
 *
 * Štítky zkratek: místo pro štítek se počítá ze zkratky v tabulce akcí bez
 * ohledu na to, zda je akce právě dostupná; zakázaná akce štítek nekreslí,
 * ale místo zůstává. Dvojice "By width" a "By height" má štítky obě, nebo
 * žádné (overlay_action_keycaps_fit()).
 *
 * Fokus: výchozí fokus tabu má zvolený segment řádku Color schema (první
 * prvek sloupce, aby se obsah po otevření tabu neodroloval).
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

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

#include "i18n.h"
#include "emulator/display.h"

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

#ifdef MZTEST_HEADLESS
/* testy prodlužují přeložené texty tabu o 40 % (overlay_test_set_stretch()) */
#undef _
#define _(STRING) overlay_test_tr(STRING)
#endif

/* ================================================================
 * Rozměry v návrhových bodech (overlay_px())
 * ================================================================ */

/** @brief Mezera mezi kartami. */
#define DISPLAY_CARD_GAP 12.0f
/** @brief Podíl šířky obsahu tabu, který v širokém okně zabírá sloupec karet (zbytek nechává vidět obraz stroje). */
#define DISPLAY_COLUMN_FRACTION 0.45f
/** @brief Mezera mezi prvky uvnitř karty. */
#define DISPLAY_GAP 8.0f
/** @brief Podíl šířky řádku pro popisek u řádků, jejichž prvek je přepínač u pravého okraje. */
#define DISPLAY_TOGGLE_LABEL_FRACTION 0.72f
/** @brief Nejmenší podíl šířky řádku pro popisek, když sloupec popisku ustupuje štítku zkratky. */
#define DISPLAY_TOGGLE_LABEL_MIN_FRACTION 0.40f
/** @brief Vnitřní okraj podkladu věty pod kartami. */
#define DISPLAY_NOTE_PAD 8.0f

/** @brief Počet voleb řízení snímkové frekvence. */
#define DISPLAY_FRAMERATE_CHOICES 5

/** @brief Režimy řízení snímkové frekvence v pořadí klasického menu (Slave, V-Sync, Adaptive, Immediate, Custom FPS). */
static const en_DISPLAY_FRAMERATE_MODE s_framerate_order[DISPLAY_FRAMERATE_CHOICES] = {
    DISPLAY_FRAMERATE_MODE_SLAVE, DISPLAY_FRAMERATE_MODE_VSYNC, DISPLAY_FRAMERATE_MODE_ADAPTIVE, DISPLAY_FRAMERATE_MODE_IMMEDIATE, DISPLAY_FRAMERATE_MODE_CUSTOM,
};

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/**
 * @brief Začne kartu tabu s přeloženým nadpisem.
 * @param title Přeložený nadpis.
 * @param id    Stabilní ID karty (bez "###").
 * @param size  Velikost karty v pixelech.
 * @return true = karta je vidět.
 * @post Musí následovat ovl_card_end().
 */
static bool display_card_begin(const char *title, const char *id, ImVec2 size)
{
    char full[256];
    snprintf(full, sizeof(full), "%s###%s", title, id);
    return ovl_card_begin(full, size);
}

/**
 * @brief Uzavře ručně rozmístěný obsah položkou, aby kurzor nezůstal za hranicí obsahu.
 * @param x Levý okraj obsahu.
 * @param y Spodní okraj obsahu.
 */
static void display_finish_content(float x, float y)
{
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

/**
 * @brief Řádek s přepínačem u pravého okraje, který přepíná akci z tabulky akcí.
 * @param label     Přeložený popisek řádku.
 * @param id        Řetězec pro ID přepínače.
 * @param on        Současný stav z kopie.
 * @param action    Akce, kterou přepnutí vyžádá.
 * @param v         Kopie stavu (dostupnost a štítek zkratky akce).
 * @param test_name Jméno obdélníku přepínače pro testy.
 * @param focus     true = přepínač je výchozí prvek tabu (dostane fokus, když ho rámec žádá).
 *
 * Štítek zkratky akce se kreslí vpravo od přepínače, pokud se do řádku
 * vejde; sloupec popisku mu ustoupí nejvýš na
 * DISPLAY_TOGGLE_LABEL_MIN_FRACTION šířky řádku. Místo pro štítek nezávisí
 * na dostupnosti akce: u zakázané akce se štítek nekreslí, ale přepínač
 * zůstává na stejném místě. Štítek, který se nevešel, je v tooltipu.
 */
static void display_toggle_row(const char *label, const char *id, bool on, en_OVL_ACTION action, const st_OVERLAY_VIEW *v, const char *test_name, bool focus)
{
    const float gap = floorf(overlay_px(DISPLAY_GAP));
    /* místo se počítá ze zkratky v tabulce (nezávisle na dostupnosti), kreslí se jen štítek fungující zkratky */
    const char *layout_kc = overlay_action_def(action)->keycap;
    const char *keycap = overlay_action_keycap(action, v);
    const ImVec2 ts = ovl_toggle_size();
    const float kc_w = layout_kc ? ovl_keycap_size(layout_kc).x : 0.0f;
    /* sloupec popisku ustoupí štítku zkratky, nejvýš na DISPLAY_TOGGLE_LABEL_MIN_FRACTION šířky řádku */
    float fraction = DISPLAY_TOGGLE_LABEL_FRACTION;
    const float row_w = ImGui::GetContentRegionAvail().x;
    if (layout_kc && row_w > 1.0f) {
        const float need = 1.0f - (ts.x + 2.0f * gap + kc_w) / row_w;
        if (need < fraction) fraction = (need > DISPLAY_TOGGLE_LABEL_MIN_FRACTION) ? need : DISPLAY_TOGGLE_LABEL_MIN_FRACTION;
    }
    ovl_row_begin_ex(label, fraction);
    const bool room_kc = layout_kc && ts.x + gap + kc_w <= ImGui::GetContentRegionAvail().x;
    const bool show_kc = room_kc && keycap;
    ovl_row_right(ts.x + (room_kc ? gap + kc_w : 0.0f));
    const char *reason = overlay_action_reason(action, v);
    bool value = on;
    if (reason) ImGui::BeginDisabled();
    const bool changed = ovl_toggle(id, &value);
    if (reason) ImGui::EndDisabled();
    overlay_test_note_item(test_name);
    if (focus && overlay_default_focus_wanted()) overlay_focus_last_item();
    if (reason) ovl_tooltip_disabled(reason);
    else if (keycap && !room_kc) ovl_tooltip(keycap);
    if (changed && !reason) overlay_action_request(action);
    if (show_kc) {
        ImGui::SameLine(0.0f, gap);
        ovl_keycap(keycap);
        char kc_name[32];
        snprintf(kc_name, sizeof(kc_name), "%.20s_keycap", test_name);
        overlay_test_note_item(kc_name);
    }
    ovl_row_end();
}

/* ================================================================
 * Karty
 * ================================================================ */

/**
 * @brief Výška obsahu karty WINDOW: tři běžné řádky a vyšší řádek velikosti okna.
 * @return Výška v pixelech.
 */
static float display_window_content_h(void)
{
    return 3.0f * ovl_row_height() + ovl_row_stacked_height();
}

/**
 * @brief Karta WINDOW: fullscreen, velikost okna při startu, poměr stran.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void display_card_window(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (display_card_begin(_("Window"), "ovl_card_window", size)) {
        /* přepínač ukazuje skutečný stav okna; přepnutí je požadavek jako Alt+Ctrl+Enter */
        display_toggle_row(_("Fullscreen"), "disp_fullscreen", v->display.fullscreen, OVL_ACTION_FULLSCREEN, v, "disp_fullscreen", false);

        ovl_row_begin_stacked(_("Window size on startup"));
        {
            const char *labels[DISPLAY_STARTUP_SIZES_COUNT];
            labels[DISPLAY_STARTUP_SIZE_HALF] = _("Half");
            labels[DISPLAY_STARTUP_SIZE_NATIVE] = _("Native");
            labels[DISPLAY_STARTUP_SIZE_NORMAL] = _("Normal");
            labels[DISPLAY_STARTUP_SIZE_BIGGER] = _("Bigger");
            labels[DISPLAY_STARTUP_SIZE_BIG] = _("Big");
            labels[DISPLAY_STARTUP_SIZE_FULLSCREEN] = _("Fullscreen");
            int sel = v->display.startup_size;
            if (ovl_segmented_ex("disp_startup", labels, NULL, (int)DISPLAY_STARTUP_SIZES_COUNT, &sel, -1.0f)) overlay_action_request_value(OVL_ACTION_STARTUP_SIZE, sel);
            overlay_test_note_item("disp_startup");
        }
        ovl_row_end();

        display_toggle_row(_("Lock window aspect ratio"), "disp_lock", v->display.locked_aspect, OVL_ACTION_LOCK_ASPECT, v, "disp_lock", false);

        ovl_row_begin(_("Fix aspect ratio"));
        {
            const float gap = floorf(overlay_px(DISPLAY_GAP));
            const float half = floorf((ImGui::GetContentRegionAvail().x - gap) * 0.5f);
            /* dvojice stejných tlačítek: štítky zkratek obě, nebo žádné (posudek N02) */
            const en_OVL_ACTION pair[2] = {OVL_ACTION_FIX_ASPECT_W, OVL_ACTION_FIX_ASPECT_H};
            const char *const texts[2] = {_("By width"), _("By height")};
            const float widths[2] = {half, half};
            const bool keycaps = overlay_action_keycaps_fit(pair, texts, widths, 2);
            overlay_action_button_ex(pair[0], v, texts[0], false, OVL_BUTTON_NORMAL, ImVec2(half, 0.0f), "disp_fix_w", keycaps);
            ImGui::SameLine(0.0f, gap);
            overlay_action_button_ex(pair[1], v, texts[1], false, OVL_BUTTON_NORMAL, ImVec2(half, 0.0f), "disp_fix_h", keycaps);
        }
        ovl_row_end();
    }
    ovl_card_end();
    overlay_test_note_item("card_window");
}

/**
 * @brief Karta COLORS: barevné schéma; zvolený segment je výchozí prvek tabu.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void display_card_colors(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (display_card_begin(_("Colors"), "ovl_card_colors", size)) {
        ovl_row_begin(_("Color schema"));
        {
            const char *labels[DISPLAY_COLORS_COUNT];
            labels[DISPLAY_NORMAL] = _("Normal");
            labels[DISPLAY_GRAYSCALE] = _("Grayscale");
            labels[DISPLAY_GREEN] = _("Green");
            int sel = v->display.color_schema;
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            if (ovl_segmented_ex("disp_colors", labels, NULL, (int)DISPLAY_COLORS_COUNT, &sel, -1.0f)) overlay_action_request_value(OVL_ACTION_COLORS, sel);
            overlay_test_note_item("disp_colors");
            if (overlay_default_focus_wanted()) {
                /* výchozí prvek tabu; ID segmentu jako v ovl_segmented_reasons(): index pod ID prvku */
                const int cur = v->display.color_schema;
                ImGui::PushID("disp_colors");
                const ImGuiID seg_id = window->GetID((cur >= 0 && cur < (int)DISPLAY_COLORS_COUNT) ? cur : 0);
                ImGui::PopID();
                overlay_focus_item(seg_id);
            }
        }
        ovl_row_end();
    }
    ovl_card_end();
    overlay_test_note_item("card_colors");
}

/**
 * @brief Karta CRT SCANLINES: zapnutí a intenzita.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void display_card_scanlines(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (display_card_begin(_("CRT scanlines"), "ovl_card_scanlines", size)) {
        display_toggle_row(_("Enable scanlines"), "disp_scanlines", v->display.scanlines, OVL_ACTION_SCANLINES, v, "disp_scanlines", false);

        const char *reason = overlay_action_reason(OVL_ACTION_SCANLINES_INTENSITY, v);
        if (reason) ImGui::BeginDisabled();
        ovl_row_begin(_("Intensity"));
        {
            float value = (float)v->display.scanlines_intensity;
            if (ovl_slider_percent_ex("disp_intensity", &value, (float)DISPLAY_SCANLINES_INTENSITY_MIN, (float)DISPLAY_SCANLINES_INTENSITY_MAX, 1.0f, 0.0f) && !reason)
                overlay_action_request_value(OVL_ACTION_SCANLINES_INTENSITY, (int)(value + 0.5f));
            overlay_test_note_item("disp_intensity");
            if (reason) ovl_tooltip_disabled(reason);
        }
        ovl_row_end();
        if (reason) ImGui::EndDisabled();
    }
    ovl_card_end();
    overlay_test_note_item("card_scanlines");
}

/**
 * @brief Je zvolen režim Custom FPS (karta Frame rate má navíc řádek s hodnotou)?
 * @param v Kopie stavu.
 */
static bool display_custom_fps(const st_OVERLAY_VIEW *v)
{
    return v->display.framerate_mode == (int)DISPLAY_FRAMERATE_MODE_CUSTOM;
}

/**
 * @brief Karta FRAME RATE: řízení snímkové frekvence, vlastní frekvence a vynucené překreslení.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void display_card_framerate(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (display_card_begin(_("Frame rate"), "ovl_card_framerate", size)) {
        ovl_row_begin(_("Frame rate control"));
        {
            char slave[96];
            snprintf(slave, sizeof(slave), _("Slave (%dHz)"), v->display.slave_hz);
            st_OVL_DROPDOWN_ITEM items[DISPLAY_FRAMERATE_CHOICES];
            const char *labels[DISPLAY_FRAMERATE_CHOICES] = {slave, _("Swap With V-Sync"), _("Swap With V-Sync (Adaptive)"), _("Immediate Swap"), _("Custom FPS")};
            int sel = -1;
            for (int i = 0; i < DISPLAY_FRAMERATE_CHOICES; i++) {
                items[i].label = labels[i];
                items[i].section = false;
                items[i].reason = NULL;
                if ((int)s_framerate_order[i] == v->display.framerate_mode) sel = i;
            }
            if (ovl_dropdown("disp_framerate", items, DISPLAY_FRAMERATE_CHOICES, &sel, 0.0f) && sel >= 0 && sel < DISPLAY_FRAMERATE_CHOICES)
                overlay_action_request_value(OVL_ACTION_FRAMERATE, (int)s_framerate_order[sel]);
            overlay_test_note_item("disp_framerate");
        }
        ovl_row_end();

        if (display_custom_fps(v)) {
            /* hodnota vlastní frekvence: posuvník a tlačítka po 1 Hz (posuvník má na pixel několik Hz) */
            ovl_row_begin(_("Custom FPS"));
            {
                const float gap = floorf(overlay_px(DISPLAY_GAP));
                const float btn = floorf(overlay_px(OVL_CONTROL_H));
                const float avail = ImGui::GetContentRegionAvail().x;
                const int fps = v->display.custom_fps;
                char label[160];
                snprintf(label, sizeof(label), "###disp_fps_down");
                ImGui::BeginDisabled(fps <= DISPLAY_CUSTOM_FPS_MIN);
                if (ovl_button_ex(label, ICON_LC_MINUS, NULL, OVL_BUTTON_NORMAL, ImVec2(btn, btn))) overlay_action_request_value(OVL_ACTION_CUSTOM_FPS, fps - 1);
                ImGui::EndDisabled();
                overlay_test_note_item("disp_fps_down");
                ovl_tooltip(_("Decrease by 1 Hz"));
                ImGui::SameLine(0.0f, gap);
                float value = (float)fps;
                if (ovl_slider_unit_ex("disp_fps", &value, (float)DISPLAY_CUSTOM_FPS_MIN, (float)DISPLAY_CUSTOM_FPS_MAX, 1.0f, avail - 2.0f * (btn + gap), "Hz"))
                    overlay_action_request_value(OVL_ACTION_CUSTOM_FPS, (int)(value + 0.5f));
                overlay_test_note_item("disp_fps");
                ImGui::SameLine(0.0f, gap);
                snprintf(label, sizeof(label), "###disp_fps_up");
                ImGui::BeginDisabled(fps >= DISPLAY_CUSTOM_FPS_MAX);
                if (ovl_button_ex(label, ICON_LC_PLUS, NULL, OVL_BUTTON_NORMAL, ImVec2(btn, btn))) overlay_action_request_value(OVL_ACTION_CUSTOM_FPS, fps + 1);
                ImGui::EndDisabled();
                overlay_test_note_item("disp_fps_up");
                ovl_tooltip(_("Increase by 1 Hz"));
            }
            ovl_row_end();
        }

        display_toggle_row(_("Forced redraw every screen"), "disp_redraw", v->display.forced_redraw, OVL_ACTION_FORCED_REDRAW, v, "disp_redraw", false);
    }
    ovl_card_end();
    overlay_test_note_item("card_framerate");
}

/**
 * @brief Věta "Changes apply immediately." s ikonou na neprůhledném podkladu.
 *
 * Plocha overlaye má na tomto tabu krytí jen 55 %; text přímo na ní by nad
 * světlým obrazem nebyl čitelný, proto má vlastní podklad v barvě karty.
 *
 * @param x     Levý okraj.
 * @param y     Horní okraj.
 * @param max_w Největší šířka v pixelech.
 * @return Výška prvku v pixelech.
 */
static float display_note(float x, float y, float max_w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const char *text = _("Changes apply immediately.");
    const float fs = overlay_font_px(OVL_FONT_HINT);
    const float pad = floorf(overlay_px(DISPLAY_NOTE_PAD));
    const float gap = floorf(overlay_px(DISPLAY_GAP));
    const float icon = overlay_style_has_icons() ? floorf(overlay_px(OVL_ICON)) : 0.0f;
    const float h = floorf((icon > fs ? icon : fs) + 2.0f * pad);
    ImGui::PushFont(overlay_font_ui(), fs);
    const float text_w = ImGui::CalcTextSize(text, NULL, false).x;
    float w = ceilf(2.0f * pad + (icon > 0.0f ? icon + gap : 0.0f) + text_w);
    if (w > max_w) w = floorf(max_w);
    const ImVec2 p(floorf(x), floorf(y));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), OVL_COL_CARD, overlay_px(OVL_RADIUS_CONTROL));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), OVL_COL_BORDER, overlay_px(OVL_RADIUS_CONTROL), 0, overlay_border_px());
    float tx = p.x + pad;
    if (icon > 0.0f) {
        ovl_draw_icon(dl, ImVec2(tx, floorf(p.y + (h - icon) * 0.5f)), icon, ICON_LC_INFO, OVL_COL_TEXT_SECONDARY);
        tx += icon + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(tx, floorf(p.y + (h - fs) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT_SECONDARY);
    ovl_text_ellipsis(text, p.x + w - pad - tx);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    overlay_test_note_rect("disp_note", p, ImVec2(p.x + w, p.y + h));
    return h;
}

/* ================================================================
 * Tab
 * ================================================================ */

/**
 * @brief Sloupec karet tabu: Colors, CRT scanlines, Window, Frame rate a věta pod nimi.
 *
 * Kreslí od současné pozice kurzoru okna a na konci nastaví výšku obsahu
 * okna (display_finish_content()), takže okno, ve kterém se volá, se podle
 * potřeby svisle roluje.
 *
 * @param v     Kopie stavu.
 * @param col_w Šířka karet v pixelech.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push().
 */
static void display_column(const st_OVERLAY_VIEW *v, float col_w)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float gap = floorf(overlay_px(DISPLAY_CARD_GAP));
    const float row_h = ovl_row_height();

    float window_h = ovl_card_height(display_window_content_h(), true);
    float colors_h = ovl_card_height(row_h, true);
    float scan_h = ovl_card_height(2.0f * row_h, true);
    float rate_h = ovl_card_height((display_custom_fps(v) ? 3.0f : 2.0f) * row_h, true);

    /* pořadí Colors, CRT scanlines, Window, Frame rate: nahoře karty, jejichž účinek je vidět na obraze */
    float y = origin.y;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    display_card_colors(v, ImVec2(col_w, colors_h));
    y += colors_h + gap;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    display_card_scanlines(v, ImVec2(col_w, scan_h));
    y += scan_h + gap;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    display_card_window(v, ImVec2(col_w, window_h));
    y += window_h + gap;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    display_card_framerate(v, ImVec2(col_w, rate_h));
    y += rate_h + gap;
    y += display_note(origin.x, y, col_w);
    display_finish_content(origin.x, y);
}

void overlay_tab_display_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    if (overlay_is_narrow()) {
        /* úzké okno: karty přes celou šířku, roluje se celý obsah tabu (posuvník je hned vedle karet) */
        display_column(v, w);
        return;
    }
    /* široké okno: sloupec karet jen u levého okraje, aby zbytek obrazu stroje zůstal nezakrytý (plocha overlaye
     * je na tomto tabu průhlednější a změna barev a scanlines má být vidět, posudek R03). Roluje se jen sloupec:
     * posuvník je hned u jeho pravého okraje a má vlastní neprůhledný podklad, aby bylo nad jakýmkoli obrazem
     * zřejmé, že sloupec pokračuje (posudek N01). */
    const float col_w = floorf(w * DISPLAY_COLUMN_FRACTION);
    const float bar_w = ceilf(ImGui::GetStyle().ScrollbarSize);
    const float h = floorf(ImGui::GetContentRegionAvail().y);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, OVL_COL_CARD);
    const bool visible = ImGui::BeginChild("###ovl_disp_col", ImVec2(col_w + bar_w, h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (visible) display_column(v, col_w);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    overlay_test_note_item("disp_column");
}
