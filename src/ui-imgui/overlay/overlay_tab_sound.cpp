/**
 * @file   overlay_tab_sound.cpp
 * @brief  Tab Sound overlay menu: poslech, hlavní hlasitost, ztlumení, výstupní propust a hlasitosti kanálů.
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW::sound) a akce jen vyžádá
 * (overlay_actions.h). Dvě karty: OUTPUT (Listen, Master volume, Mute,
 * Hardware output filter a dvě věty nápovědy) a CHANNELS (posuvník každého
 * kanálu a tlačítko Reset channels).
 *
 * Hlavní hlasitost a ztlumení jsou veličiny zvukového rozhraní
 * (iface_audio_set_master_gain(), iface_audio_set_mute()): násobí hotový
 * mix pro poslech a hlasitosti kanálů nemění. Hlavní hlasitost má rozsah
 * 0 až 200 % (OVERLAY_VIEW_MASTER_GAIN_MAX) se značkou a přichycením na 100 %;
 * nad 100 % zvuk zesiluje. Kanály mají počet a jména
 * jako okno Audio Mixer (audio_window.cpp; jména jsou jeho msgid) a
 * posuvník zapisuje stejnou hodnotu g_iface_audio.gain[], jen v procentech
 * místo decibelů. Emulace na tabu stojí jako na ostatních tabech; aby byla
 * změna slyšet hned, zapne uživatel přepínač Listen v prvním řádku karty
 * OUTPUT (akce OVL_ACTION_LISTEN; overlay.h, oddíl Tab Sound). Přepínač je
 * zakázaný s důvodem v tooltipu, když pauza není overlaye (emulace stála
 * před otevřením nebo ji zastavil uživatel) nebo když emulace běží z jiného
 * důvodu než kvůli poslechu (pak ukazuje zapnuto). Věta pod kartou Output
 * říká, zda emulace běží (rozhodnutí autora 2026-11-19: přepínač nahradil
 * samočinný běh emulace na tabu Sound). Ztlumení se neukládá, poslech také
 * ne.
 *
 * Fokus: výchozí fokus tabu má posuvník Master volume.
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

/** @brief Největší hodnota posuvníku Master volume v procentech. */
#define SOUND_MASTER_PERCENT_MAX (OVERLAY_VIEW_MASTER_GAIN_MAX * 100.0f)
/** @brief Polovina šířky pásma, ve kterém se posuvník Master volume při tažení myší přichytí na 100 % [procenta]. */
#define SOUND_MASTER_DETENT_PERCENT 4.0f

/** @brief Počet kanálů jednoho čipu PSG a kanálu CTC0 dohromady (jména bez čísla čipu). */
#define SOUND_MONO_CHANNELS 5

/** @brief Jména kanálů stroje s jedním čipem PSG nebo bez PSG (msgid okna Audio Mixer). */
static const char *const s_names_mono[SOUND_MONO_CHANNELS] = {N_("CTC0"), N_("PSG tone1"), N_("PSG tone2"), N_("PSG tone3"), N_("PSG noise")};
/** @brief Jména kanálů stroje se dvěma čipy PSG (msgid okna Audio Mixer). */
static const char *const s_names_stereo[OVERLAY_VIEW_AUDIO_CHANNELS_MAX] = {N_("CTC0"),       N_("PSG0 tone1"), N_("PSG0 tone2"), N_("PSG0 tone3"), N_("PSG0 noise"),
                                                                            N_("PSG1 tone1"), N_("PSG1 tone2"), N_("PSG1 tone3"), N_("PSG1 noise")};

/**
 * @brief Přeložené jméno zvukového kanálu.
 * @param v  Kopie stavu.
 * @param ch Index kanálu (menší než st_OVERLAY_VIEW_SOUND::channel_count).
 * @return Přeložené jméno; prázdný text pro kanál mimo rozsah.
 */
static const char *sound_channel_name(const st_OVERLAY_VIEW *v, unsigned ch)
{
    if (v->sound.stereo_names) return (ch < OVERLAY_VIEW_AUDIO_CHANNELS_MAX) ? _(s_names_stereo[ch]) : "";
    return (ch < SOUND_MONO_CHANNELS) ? _(s_names_mono[ch]) : "";
}

/**
 * @brief Počet kanálů, které tab kreslí.
 * @param v Kopie stavu.
 * @return Počet kanálů omezený velikostí kopie.
 */
static unsigned sound_channel_count(const st_OVERLAY_VIEW *v)
{
    return (v->sound.channel_count > OVERLAY_VIEW_AUDIO_CHANNELS_MAX) ? OVERLAY_VIEW_AUDIO_CHANNELS_MAX : v->sound.channel_count;
}

/** @brief Věta o výstupní propusti pod kartou OUTPUT. */
static const char *sound_filter_note(void)
{
    return _("The filter models the audio output stage of the real machine.");
}

/**
 * @brief Ukazuje přepínač Listen zapnuto?
 * @param v Kopie stavu.
 * @return true = poslech je zapnutý nebo emulace běží z jiného důvodu (přepínač je pak zakázaný).
 */
static bool sound_listen_shown_on(const st_OVERLAY_VIEW *v)
{
    return v->sound.listen == (int)OVERLAY_LISTEN_ON || v->sound.listen == (int)OVERLAY_LISTEN_BLOCKED_RUNNING;
}

/**
 * @brief Věta o tom, kdy je změna slyšet.
 * @param v Kopie stavu.
 * @return Přepínač Listen ukazuje zapnuto a emulace běží: změny jsou slyšet
 *         hned; jinak (vypnuto, zakázáno při pauze, emulace se teprve
 *         rozbíhá) návod, že poslech zapíná přepínač Listen.
 */
static const char *sound_listen_note(const st_OVERLAY_VIEW *v)
{
    if (sound_listen_shown_on(v) && !v->paused) return _("The emulation is running so that you can hear the changes.");
    return _("Turn on Listen to hear the changes while the menu is open. The emulated computer keeps running without keyboard input.");
}

/**
 * @brief Řádek s přepínačem Listen: ikona sluchátek a přepínač u pravého okraje.
 *
 * Přepnutí vyžádá akci OVL_ACTION_LISTEN; nedostupná akce kreslí ikonu i
 * přepínač zakázané a důvod ukáže v tooltipu přepínače. Zkratku akce nemá.
 * Bez písma ikon se ikona vynechá.
 *
 * @param v Kopie stavu.
 * @pre Uvnitř karty; kurzor okna stojí na začátku řádku.
 */
static void sound_listen_row(const st_OVERLAY_VIEW *v)
{
    const float gap = floorf(overlay_px(OVL_TAB_GAP));
    const ImVec2 ts = ovl_toggle_size();
    const bool icon = overlay_style_has_icons();
    const float icon_w = icon ? floorf(overlay_px(OVL_ICON)) + gap : 0.0f;
    const char *reason = overlay_action_reason(OVL_ACTION_LISTEN, v);
    const bool listening = sound_listen_shown_on(v) && !v->paused;

    ovl_row_begin_ex(_("Listen"), OVL_TAB_TOGGLE_LABEL_FRACTION);
    ovl_row_right(icon_w + ts.x);
    if (reason) ImGui::BeginDisabled();
    if (icon) {
        ovl_icon(ICON_LC_HEADPHONES, listening ? OVL_COL_GREEN_TEXT : OVL_COL_TEXT_SECONDARY);
        overlay_test_note_item("snd_listen_icon");
        ImGui::SameLine(0.0f, gap);
    }
    bool value = sound_listen_shown_on(v);
    const bool changed = ovl_toggle("snd_listen", &value);
    if (reason) ImGui::EndDisabled();
    overlay_test_note_item("snd_listen");
    if (reason) ovl_tooltip_disabled(reason);
    if (changed && !reason) overlay_action_request(OVL_ACTION_LISTEN);
    ovl_row_end();
}

/**
 * @brief Výška obsahu karty OUTPUT.
 * @param v      Kopie stavu.
 * @param wrap_w Vnitřní šířka karty v pixelech (zalomení vět nápovědy).
 * @return Výška v pixelech.
 */
static float sound_output_content_h(const st_OVERLAY_VIEW *v, float wrap_w)
{
    const float gap = floorf(overlay_px(OVL_TAB_GAP));
    float h = (v->sound.has_output_filter ? 4.0f : 3.0f) * ovl_row_height();
    if (v->sound.has_output_filter) h += gap + ovl_tab_note_height(sound_filter_note(), wrap_w);
    h += gap + ovl_tab_note_height(sound_listen_note(v), wrap_w);
    return h;
}

/**
 * @brief Karta OUTPUT: poslech, hlavní hlasitost, ztlumení, výstupní propust a nápověda.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void sound_card_output(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Output"), "ovl_card_output", size)) {
        const float gap = floorf(overlay_px(OVL_TAB_GAP));
        const float left = ImGui::GetCursorScreenPos().x;
        const float w = floorf(ImGui::GetContentRegionAvail().x);

        sound_listen_row(v);

        ovl_row_begin(_("Master volume"));
        {
            /* ikona říká, zda je něco slyšet: ztlumeno nebo hlasitost 0 */
            const bool silent = v->sound.mute || v->sound.master_gain <= 0.0f;
            ovl_icon(silent ? ICON_LC_VOLUME_X : ICON_LC_VOLUME_2, silent ? OVL_COL_AMBER : OVL_COL_TEXT_SECONDARY);
            overlay_test_note_item("snd_master_icon");
            ImGui::SameLine(0.0f, gap);
            float value = floorf(v->sound.master_gain * 100.0f + 0.5f);
            if (ovl_slider_percent_mark("snd_master", &value, 0.0f, SOUND_MASTER_PERCENT_MAX, 1.0f, 0.0f, 100.0f, SOUND_MASTER_DETENT_PERCENT))
                overlay_action_request_value(OVL_ACTION_MASTER_VOLUME, (int)(value + 0.5f));
            overlay_test_note_item("snd_master");
            if (overlay_default_focus_wanted()) overlay_focus_last_item();
        }
        ovl_row_end();

        ovl_tab_toggle_row(_("Mute"), "snd_mute", v->sound.mute, OVL_ACTION_MUTE, v, "snd_mute", false);
        if (v->sound.has_output_filter)
            ovl_tab_toggle_row(_("Hardware output filter"), "snd_filter", v->sound.output_filter, OVL_ACTION_OUTPUT_FILTER, v, "snd_filter", false);

        float y = ImGui::GetCursorScreenPos().y;
        if (v->sound.has_output_filter) {
            y += gap;
            y += ovl_tab_note(sound_filter_note(), left, y, w, "snd_filter_note");
        }
        y += gap;
        y += ovl_tab_note(sound_listen_note(v), left, y, w, "snd_listen_note");
        ovl_tab_finish_content(left, y);
    }
    ovl_card_end();
    overlay_test_note_item("card_output");
}

/**
 * @brief Výška obsahu karty CHANNELS: řádek na kanál a tlačítko Reset channels.
 * @param v Kopie stavu.
 * @return Výška v pixelech.
 */
static float sound_channels_content_h(const st_OVERLAY_VIEW *v)
{
    return (float)sound_channel_count(v) * ovl_row_height() + floorf(overlay_px(OVL_TAB_GAP)) + floorf(overlay_px(OVL_CONTROL_H));
}

/**
 * @brief Karta CHANNELS: posuvník hlasitosti každého kanálu a tlačítko Reset channels.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void sound_card_channels(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (ovl_tab_card_begin(_("Channels"), "ovl_card_channels", size)) {
        const float gap = floorf(overlay_px(OVL_TAB_GAP));
        const float left = ImGui::GetCursorScreenPos().x;
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        const unsigned count = sound_channel_count(v);
        for (unsigned i = 0; i < count; i++) {
            ovl_row_begin(sound_channel_name(v, i));
            {
                char id[24], name[24];
                snprintf(id, sizeof(id), "snd_ch%u", i);
                snprintf(name, sizeof(name), "snd_ch%u", i);
                float value = floorf(v->sound.channel_gain[i] * 100.0f + 0.5f);
                if (ovl_slider_percent_ex(id, &value, 0.0f, 100.0f, 1.0f, 0.0f))
                    overlay_action_request_value(OVL_ACTION_CHANNEL_VOLUME, OVL_CHANNEL_VOLUME_VALUE((int)i, (int)(value + 0.5f)));
                overlay_test_note_item(name);
            }
            ovl_row_end();
        }
        /* Reset channels u pravého okraje pod řádky */
        const float y = ImGui::GetCursorScreenPos().y + gap;
        const float h = floorf(overlay_px(OVL_CONTROL_H));
        char label[160];
        snprintf(label, sizeof(label), "%s###ovl_action_%d", _("Reset channels"), (int)OVL_ACTION_CHANNELS_RESET);
        float bw = ovl_button_natural_width(label, NULL, NULL);
        if (bw > w) bw = w;
        ImGui::SetCursorScreenPos(ImVec2(left + w - bw, y));
        overlay_action_button(OVL_ACTION_CHANNELS_RESET, v, _("Reset channels"), false, OVL_BUTTON_NORMAL, ImVec2(bw, h), "snd_reset");
        ovl_tab_finish_content(left, y + h);
    }
    ovl_card_end();
    overlay_test_note_item("card_channels");
}

void overlay_tab_sound_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    const bool narrow = overlay_is_narrow();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    const float gap = floorf(overlay_px(OVL_TAB_CARD_GAP));
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    const float col_w = narrow ? w : floorf((w - gap) * 0.5f);

    float output_h = ovl_card_height(sound_output_content_h(v, col_w - 2.0f * pad), true);
    float channels_h = ovl_card_height(sound_channels_content_h(v), true);

    float y = origin.y;
    if (!narrow) {
        /* dva sloupce: karty vedle sebe mají výšku té vyšší */
        if (output_h < channels_h) output_h = channels_h;
        else channels_h = output_h;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        sound_card_output(v, ImVec2(col_w, output_h));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + w - col_w, y));
        sound_card_channels(v, ImVec2(col_w, channels_h));
        y += output_h;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        sound_card_output(v, ImVec2(w, output_h));
        y += output_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        sound_card_channels(v, ImVec2(w, channels_h));
        y += channels_h;
    }
    ovl_tab_finish_content(origin.x, y);
}
