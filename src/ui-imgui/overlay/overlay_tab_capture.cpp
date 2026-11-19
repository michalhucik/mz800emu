/**
 * @file   overlay_tab_capture.cpp
 * @brief  Tab Capture overlay menu: screenshot, video záznam a záznam vstupů (replay).
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW::capture) a akce jen vyžádá
 * (overlay_actions.h). Tři karty vedle sebe (v úzkém okně pod sebou):
 * SCREENSHOT (Save Screenshot, Save Screenshot As..., Record Changed
 * Frames, Open Output Folder, Settings...), VIDEO RECORDING (Start nebo
 * Stop Recording podle stavu, Pause Recording, Record in Real Time, Open
 * Output Folder, Settings...) a REPLAY (Record Inputs..., Play Replay...,
 * Stop Replay, Replay Control... a řádek stavu).
 *
 * Dostupnost je jako v klasickém menu: Pause Recording jen při nahrávání,
 * Record Inputs... a Play Replay... jen při vypnutém replay, Stop Replay
 * jen při běžícím. Okna nastavení a Replay Control overlay zavřou (ruling
 * K11); Audio Capture se nenabízí.
 *
 * Fokus: výchozí fokus tabu má tlačítko Save Screenshot.
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

#include "libs/imgui/imgui.h"

#include "i18n.h"

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tab_util.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

#ifdef MZTEST_HEADLESS
/* testy prodlužují přeložené texty tabu o 40 % (overlay_test_set_stretch()) */
#undef _
#define _(STRING) overlay_test_tr(STRING)
#endif

/** @brief Velikost bufferu řádku stavu replay (text jádra a přeložený rámec). */
#define CAPTURE_STATUS_SIZE 640

/**
 * @brief Rozměry karet tabu spočtené jednou za kreslení.
 *
 * Invariant: všechny hodnoty jsou celé pixely; pair_h je control_h, když
 * jsou tlačítka dvojice vedle sebe, a dvojnásobek s mezerou, když pod sebou.
 */
typedef struct st_CAPTURE_METRICS {
    float gap;         /**< mezera mezi prvky karty */
    float control_h;   /**< výška tlačítka */
    float row_h;       /**< výška řádku s přepínačem */
    bool pair_stacked; /**< tlačítka "Open Output Folder" a "Settings..." jsou pod sebou (vedle sebe se nevejdou) */
    float pair_h;      /**< výška dvojice tlačítek */
} st_CAPTURE_METRICS;

/**
 * @brief Přirozená šířka tlačítka akce s popiskem a ikonou z tabulky akcí.
 * @param id   Akce.
 * @param text Přeložený popisek.
 * @return Šířka v pixelech.
 */
static float capture_natural_w(en_OVL_ACTION id, const char *text)
{
    char label[256];
    snprintf(label, sizeof(label), "%s###ovl_action_%d", text, (int)id);
    return ovl_button_natural_width(label, overlay_action_def(id)->icon, NULL);
}

/**
 * @brief Spočte rozměry karet pro danou vnitřní šířku karty.
 * @param inner_w Vnitřní šířka karty v pixelech.
 * @return Rozměry.
 */
static st_CAPTURE_METRICS capture_metrics(float inner_w)
{
    st_CAPTURE_METRICS m;
    m.gap = floorf(overlay_px(OVL_TAB_GAP));
    m.control_h = floorf(overlay_px(OVL_CONTROL_H));
    m.row_h = ovl_row_height();
    const float half = floorf((inner_w - m.gap) * 0.5f);
    m.pair_stacked = capture_natural_w(OVL_ACTION_SCREENSHOT_FOLDER, _("Open Output Folder")) > half || capture_natural_w(OVL_ACTION_SCREENSHOT_SETUP, _("Settings...")) > half;
    m.pair_h = m.pair_stacked ? 2.0f * m.control_h + m.gap : m.control_h;
    return m;
}

/**
 * @brief Dvojice tlačítek "Open Output Folder" a "Settings..." vedle sebe nebo pod sebou.
 * @param v           Kopie stavu.
 * @param m           Rozměry.
 * @param folder      Akce otevření výstupního adresáře.
 * @param setup       Akce otevření nastavení.
 * @param x           Levý okraj.
 * @param y           Horní okraj.
 * @param w           Šířka.
 * @param folder_name Jméno obdélníku tlačítka adresáře pro testy.
 * @param setup_name  Jméno obdélníku tlačítka nastavení pro testy.
 */
static void capture_pair(const st_OVERLAY_VIEW *v, const st_CAPTURE_METRICS *m, en_OVL_ACTION folder, en_OVL_ACTION setup, float x, float y, float w, const char *folder_name,
                         const char *setup_name)
{
    if (m->pair_stacked) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        overlay_action_button(folder, v, _("Open Output Folder"), false, OVL_BUTTON_NORMAL, ImVec2(w, m->control_h), folder_name);
        ImGui::SetCursorScreenPos(ImVec2(x, y + m->control_h + m->gap));
        overlay_action_button(setup, v, _("Settings..."), false, OVL_BUTTON_NORMAL, ImVec2(w, m->control_h), setup_name);
    } else {
        const float half = floorf((w - m->gap) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        overlay_action_button(folder, v, _("Open Output Folder"), false, OVL_BUTTON_NORMAL, ImVec2(half, m->control_h), folder_name);
        ImGui::SetCursorScreenPos(ImVec2(x + w - half, y));
        overlay_action_button(setup, v, _("Settings..."), false, OVL_BUTTON_NORMAL, ImVec2(half, m->control_h), setup_name);
    }
}

/**
 * @brief Výška obsahu karet SCREENSHOT a VIDEO RECORDING: dvě tlačítka, řádek s přepínačem a dvojice tlačítek.
 * @param m Rozměry.
 * @return Výška v pixelech.
 */
static float capture_tools_content_h(const st_CAPTURE_METRICS *m)
{
    return 2.0f * m->control_h + m->row_h + m->pair_h + 3.0f * m->gap;
}

/**
 * @brief Karta SCREENSHOT.
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void capture_card_screenshot(const st_OVERLAY_VIEW *v, const st_CAPTURE_METRICS *m, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Screenshot"), "ovl_card_screenshot", size)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        float y = p.y;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        overlay_action_button(OVL_ACTION_SCREENSHOT, v, _("Save Screenshot"), false, OVL_BUTTON_NORMAL, ImVec2(w, m->control_h), "cap_shot");
        if (overlay_default_focus_wanted()) overlay_focus_last_item();
        y += m->control_h + m->gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        overlay_action_button(OVL_ACTION_SCREENSHOT_AS, v, _("Save Screenshot As..."), false, OVL_BUTTON_NORMAL, ImVec2(w, m->control_h), "cap_shot_as");
        y += m->control_h + m->gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        ovl_tab_toggle_row(_("Record Changed Frames"), "cap_series", v->capture.series_active, OVL_ACTION_SERIES, v, "cap_series", false);
        y += m->row_h + m->gap;
        capture_pair(v, m, OVL_ACTION_SCREENSHOT_FOLDER, OVL_ACTION_SCREENSHOT_SETUP, p.x, y, w, "cap_shot_folder", "cap_shot_setup");
        ovl_tab_finish_content(p.x, y + m->pair_h);
    }
    ovl_card_end();
    overlay_test_note_item("card_screenshot");
}

/**
 * @brief Karta VIDEO RECORDING.
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void capture_card_video(const st_OVERLAY_VIEW *v, const st_CAPTURE_METRICS *m, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Video Recording"), "ovl_card_video", size)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        float y = p.y;
        /* popisek podle stavu jako položka klasického menu (videorec_ui_menu_model()) */
        const bool stop = v->capture.video_stop;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        ovl_tab_action_button(OVL_ACTION_VIDEO_TOGGLE, v, stop ? _("Stop Recording") : _("Start Recording"), stop ? ICON_LC_SQUARE : ICON_LC_CIRCLE, OVL_BUTTON_DANGER,
                              ImVec2(w, m->control_h), "cap_video");
        y += m->control_h + m->gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        /* pozastavené nahrávání: tlačítko zvýrazněné jako zaškrtnutá položka menu */
        overlay_action_button(OVL_ACTION_VIDEO_PAUSE, v, _("Pause Recording"), false, v->capture.video_paused ? OVL_BUTTON_PRIMARY : OVL_BUTTON_NORMAL, ImVec2(w, m->control_h),
                              "cap_video_pause");
        y += m->control_h + m->gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        ovl_tab_toggle_row(_("Record in Real Time"), "cap_realtime", v->capture.video_realtime, OVL_ACTION_VIDEO_REALTIME, v, "cap_realtime", false);
        y += m->row_h + m->gap;
        capture_pair(v, m, OVL_ACTION_VIDEO_FOLDER, OVL_ACTION_VIDEO_SETUP, p.x, y, w, "cap_video_folder", "cap_video_setup");
        ovl_tab_finish_content(p.x, y + m->pair_h);
    }
    ovl_card_end();
    overlay_test_note_item("card_video");
}

/**
 * @brief Sestaví řádek stavu replay "Status: ...".
 *
 * Texty stavu jsou msgid klasického menu Replay (menu_replay.cpp); text
 * výsledku a odchylky dodává jádro anglicky a nepřekládá se.
 *
 * @param v    Kopie stavu.
 * @param buf  Výstupní buffer.
 * @param size Velikost bufferu.
 */
static void capture_replay_status(const st_OVERLAY_VIEW *v, char *buf, size_t size)
{
    const st_OVERLAY_VIEW_CAPTURE *c = &v->capture;
    char body[CAPTURE_STATUS_SIZE - 64];
    if (c->replay_mode == (int)OVERLAY_REPLAY_RECORDING) {
        snprintf(body, sizeof(body), _("Recording: frame %u, %u events"), c->replay_frame, c->replay_events);
    } else if (c->replay_mode == (int)OVERLAY_REPLAY_PLAYING) {
        int n = snprintf(body, sizeof(body), _("Playing: frame %u / %u"), c->replay_frame, c->replay_frames);
        if (c->replay_text[0] && n > 0 && (size_t)n < sizeof(body) - 4) {
            n += snprintf(body + n, sizeof(body) - (size_t)n, " - ");
            snprintf(body + n, sizeof(body) - (size_t)n, _("Deviated: %s"), c->replay_text);
        }
    } else if (c->replay_text[0]) {
        snprintf(body, sizeof(body), _("Last: %s"), c->replay_text);
    } else {
        snprintf(body, sizeof(body), "%s", _("Off"));
    }
    snprintf(buf, size, "%s %s", _("Status:"), body);
}

/**
 * @brief Výška obsahu karty REPLAY: čtyři tlačítka a řádek stavu.
 * @param v      Kopie stavu.
 * @param m      Rozměry.
 * @param wrap_w Vnitřní šířka karty v pixelech (zalomení řádku stavu).
 * @return Výška v pixelech.
 */
static float capture_replay_content_h(const st_OVERLAY_VIEW *v, const st_CAPTURE_METRICS *m, float wrap_w)
{
    char status[CAPTURE_STATUS_SIZE];
    capture_replay_status(v, status, sizeof(status));
    return 4.0f * m->control_h + 4.0f * m->gap + ovl_tab_note_height(status, wrap_w);
}

/**
 * @brief Karta REPLAY.
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void capture_card_replay(const st_OVERLAY_VIEW *v, const st_CAPTURE_METRICS *m, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Replay"), "ovl_card_replay", size)) {
        static const struct {
            en_OVL_ACTION id;   /**< akce tlačítka */
            const char *label;  /**< anglický popisek (msgid) */
            const char *name;   /**< jméno obdélníku pro testy */
        } buttons[4] = {
            {OVL_ACTION_REPLAY_RECORD, N_("Record Inputs..."), "cap_replay_record"},
            {OVL_ACTION_REPLAY_PLAY, N_("Play Replay..."), "cap_replay_play"},
            {OVL_ACTION_REPLAY_STOP, N_("Stop Replay"), "cap_replay_stop"},
            {OVL_ACTION_REPLAY_WINDOW, N_("Replay Control..."), "cap_replay_window"},
        };
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        float y = p.y;
        for (int i = 0; i < 4; i++) {
            ImGui::SetCursorScreenPos(ImVec2(p.x, y));
            overlay_action_button(buttons[i].id, v, _(buttons[i].label), false, OVL_BUTTON_NORMAL, ImVec2(w, m->control_h), buttons[i].name);
            y += m->control_h + m->gap;
        }
        char status[CAPTURE_STATUS_SIZE];
        capture_replay_status(v, status, sizeof(status));
        y += ovl_tab_note(status, p.x, y, w, "cap_replay_status");
        ovl_tab_finish_content(p.x, y);
    }
    ovl_card_end();
    overlay_test_note_item("card_replay");
}

void overlay_tab_capture_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    const bool narrow = overlay_is_narrow();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    const float gap = floorf(overlay_px(OVL_TAB_CARD_GAP));
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    const float col_w = narrow ? w : floorf((w - 2.0f * gap) / 3.0f);
    const st_CAPTURE_METRICS m = capture_metrics(col_w - 2.0f * pad);

    float tools_h = ovl_card_height(capture_tools_content_h(&m), true);
    float replay_h = ovl_card_height(capture_replay_content_h(v, &m, col_w - 2.0f * pad), true);

    float y = origin.y;
    if (!narrow) {
        /* tři sloupce: všechny karty mají výšku té nejvyšší */
        if (tools_h < replay_h) tools_h = replay_h;
        else replay_h = tools_h;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        capture_card_screenshot(v, &m, ImVec2(col_w, tools_h));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + col_w + gap, y));
        capture_card_video(v, &m, ImVec2(col_w, tools_h));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + w - col_w, y));
        capture_card_replay(v, &m, ImVec2(col_w, replay_h));
        y += tools_h;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        capture_card_screenshot(v, &m, ImVec2(w, tools_h));
        y += tools_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        capture_card_video(v, &m, ImVec2(w, tools_h));
        y += tools_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        capture_card_replay(v, &m, ImVec2(w, replay_h));
        y += replay_h;
    }
    ovl_tab_finish_content(origin.x, y);
}
