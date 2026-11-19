/**
 * @file   overlay_tab_machine.cpp
 * @brief  Tab Machine overlay menu: běh emulace, reset, výběr ROM, rychlost, snapshoty a odkaz na klasické menu.
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW). Každý ovládací prvek je
 * akce z tabulky akcí (overlay_actions.h): zkratka a dostupnost se berou z
 * ní a zvolení akci jen vyžádá; provede ji rámec po vykreslení okna.
 * Funkce akcí jsou tytéž, které volá klasické menu (Speed, Snapshot, ROM,
 * Pause Emulation, Reset). Podle konceptu machine-v1 a rulingů K02, K03,
 * K07, K08, K11, K13 a K17 (gui/DECISIONS.md experimentu).
 *
 * Karty: MACHINE (Emulation Running / Paused, Reset, ROM), SPEED (CPU
 * speed Normal / Custom / Max, Custom speed s tlačítky minus a plus a
 * posuvníkem), SNAPSHOTS (Quick Save, Quick Load, Save Snapshot..., Load
 * Snapshot..., Snapshot Setup...) a MORE HARDWARE (text a tlačítko "Open
 * classic menu").
 *
 * Rozvržení: všechny rozměry jsou násobky jednotky overlaye. V širokém
 * okně dva sloupce stejné šířky, karty vedle sebe mají stejnou výšku; v
 * úzkém okně (overlay_is_narrow()) jsou karty pod sebou. Výšky karet se
 * počítají předem.
 *
 * Rychlost: řádky ukazují rychlost nastavenou (požadovanou), kterou emu
 * vlákno uplatní na hranici snímku, tedy po zavření overlaye. Posuvník má
 * rozsah 1 až 400 % jako výchozí posuvník klasického menu; je-li
 * nastavená rychlost vyšší (zkratkami až CUSTOMSPEED_MAX_VALUE), rozsah se
 * po ni prodlouží a po dobu držení tlačítka myši se nezkracuje
 * (machine_speed_slider_max()).
 *
 * Fokus: výchozí fokus tabu má zvolený segment řádku Emulation (K17).
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

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_style.h"
#include "ui-imgui/overlay/overlay_tabs.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"
#include "ui-imgui/overlay/res/IconsLucide.h"

#ifdef MZTEST_HEADLESS
/* testy prodlužují přeložené texty tabu o 40 % (overlay_test_set_stretch()), i texty s kontextem */
#undef _
#define _(STRING) overlay_test_tr(STRING)
#undef C_
#define C_(CTX, STR) overlay_test_trc(CTX "\004" STR, STR)
#endif

/* ================================================================
 * Rozměry v návrhových bodech (overlay_px())
 * ================================================================ */

/** @brief Mezera mezi kartami. */
#define MACHINE_CARD_GAP 12.0f
/** @brief Mezera mezi prvky uvnitř karty. */
#define MACHINE_GAP 8.0f
/** @brief Podíl šířky sloupce prvků, který zabírá segmentová volba Emulation a tlačítko Reset (zbytek patří štítku zkratky). */
#define MACHINE_MAIN_FRACTION 0.62f
/** @brief Největší hodnota posuvníku vlastní rychlosti v procentech (výchozí rozsah posuvníku klasického menu, customspeed_setup.cpp). */
#define MACHINE_SPEED_SLIDER_MAX 400.0f
/** @brief Nejmenší hodnota posuvníku vlastní rychlosti v procentech. */
#define MACHINE_SPEED_SLIDER_MIN 1.0f

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/**
 * @brief Klávesový štítek za prvkem řádku, pokud se do zbytku řádku vejde.
 * @param keycap    Text štítku; NULL = žádný.
 * @param test_name Jméno obdélníku pro testy.
 * @pre Uvnitř řádku nastavení, za prvkem, ke kterému štítek patří.
 */
static void machine_row_keycap(const char *keycap, const char *test_name)
{
    if (!keycap) return;
    const float gap = floorf(overlay_px(MACHINE_GAP));
    ImGui::SameLine(0.0f, gap);
    if (ovl_keycap_size(keycap).x > ImGui::GetContentRegionAvail().x) return;
    ovl_keycap(keycap);
    overlay_test_note_item(test_name);
}

/**
 * @brief Uzavře ručně rozmístěný obsah položkou, aby kurzor nezůstal za hranicí obsahu.
 * @param x Levý okraj obsahu.
 * @param y Spodní okraj obsahu.
 */
static void machine_finish_content(float x, float y)
{
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

/**
 * @brief Začne kartu tabu s přeloženým nadpisem.
 * @param title Přeložený nadpis.
 * @param id    Stabilní ID karty (bez "###").
 * @param size  Velikost karty v pixelech.
 * @return true = karta je vidět.
 * @post Musí následovat ovl_card_end().
 */
static bool machine_card_begin(const char *title, const char *id, ImVec2 size)
{
    char full[256];
    snprintf(full, sizeof(full), "%s###%s", title, id);
    return ovl_card_begin(full, size);
}

/**
 * @brief Text karty More hardware.
 * @return Přeložený text (dvě věty, zalamuje se na šířku karty).
 */
static const char *machine_more_text(void)
{
    return _("ROM settings, memory disks, memory extensions, IDE and DIP switches are in the classic menu.");
}

/**
 * @brief Výška zalomeného textu karty More hardware.
 * @param wrap_w Šířka, na kterou se text zalamuje, v pixelech.
 * @return Výška v pixelech (běžné písmo overlaye).
 */
static float machine_more_text_h(float wrap_w)
{
    ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_BODY));
    const float h = ImGui::CalcTextSize(machine_more_text(), NULL, false, wrap_w > 1.0f ? wrap_w : 1.0f).y;
    ImGui::PopFont();
    return ceilf(h);
}

/* ================================================================
 * Karty
 * ================================================================ */

/**
 * @brief Karta MACHINE: běh emulace, reset a výběr ROM.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void machine_card_machine(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (machine_card_begin(_("Machine"), "ovl_card_machine", size)) {
        /* Emulation: Running / Paused; zvolení druhého segmentu přepne pauzu jako Alt+P */
        ovl_row_begin(_("Emulation"));
        {
            /* "Running" bez kontextu je v katalogu sloupec běžící verze v okně kontroly aktualizací */
            const char *labels[2] = {C_("overlay emulation", "Running"), _("Paused")};
            int sel = v->paused ? 1 : 0;
            const float main_w = floorf(ImGui::GetContentRegionAvail().x * MACHINE_MAIN_FRACTION);
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            if (ovl_segmented_ex("mach_emulation", labels, NULL, 2, &sel, main_w)) overlay_action_request(OVL_ACTION_PAUSE_TOGGLE);
            overlay_test_note_item("mach_emulation");
            if (overlay_default_focus_wanted()) {
                /* ID segmentu jako v ovl_segmented_reasons(): index pod ID prvku */
                ImGui::PushID("mach_emulation");
                const ImGuiID seg_id = window->GetID(v->paused ? 1 : 0);
                ImGui::PopID();
                overlay_focus_item(seg_id);
            }
            machine_row_keycap(overlay_action_keycap(OVL_ACTION_PAUSE_TOGGLE, v), "mach_emulation_keycap");
        }
        ovl_row_end();

        /* Reset: bez potvrzení, jako F12 (K17) */
        ovl_row_begin(_("Reset"));
        {
            const float main_w = floorf(ImGui::GetContentRegionAvail().x * MACHINE_MAIN_FRACTION);
            const st_OVL_ACTION_DEF *def = overlay_action_def(OVL_ACTION_RESET);
            char label[160];
            snprintf(label, sizeof(label), "%s###mach_reset", _("Reset"));
            if (ovl_button_ex(label, def->icon, NULL, OVL_BUTTON_DANGER, ImVec2(main_w, 0.0f))) overlay_action_request(OVL_ACTION_RESET);
            overlay_test_note_item("mach_reset");
            machine_row_keycap(overlay_action_keycap(OVL_ACTION_RESET, v), "mach_reset_keycap");
        }
        ovl_row_end();

        /* ROM: nabídka jako menu ROM klasického menu */
        ovl_row_begin(_("ROM"));
        {
            st_OVL_DROPDOWN_ITEM items[OVERLAY_VIEW_ROM_MAX];
            int count = 0, sel = -1;
            for (unsigned i = 0; i < v->machine.rom_count && count < OVERLAY_VIEW_ROM_MAX; i++, count++) {
                items[count].label = v->machine.rom[i].label;
                items[count].section = v->machine.rom[i].section;
                items[count].reason = NULL;
                if (v->machine.rom[i].current) sel = count;
            }
            const char *reason = overlay_action_reason(OVL_ACTION_ROM_SELECT, v);
            const bool disabled = reason != NULL || count == 0;
            if (count == 0) {
                /* stroj bez nabídky ROM: prázdný zakázaný výběr */
                items[0].label = "";
                items[0].section = false;
                items[0].reason = NULL;
                count = 1;
            }
            if (disabled) ImGui::BeginDisabled();
            const bool changed = ovl_dropdown("mach_rom", items, count, &sel, 0.0f);
            if (disabled) ImGui::EndDisabled();
            overlay_test_note_item("mach_rom");
            if (reason) ovl_tooltip_disabled(reason);
            if (changed && !disabled && sel >= 0 && sel < (int)v->machine.rom_count) overlay_action_request_value(OVL_ACTION_ROM_SELECT, v->machine.rom[sel].row);
        }
        ovl_row_end();
    }
    ovl_card_end();
    overlay_test_note_item("card_machine");
}

/**
 * @brief Horní mez posuvníku vlastní rychlosti pro tento snímek.
 *
 * Rychlost nad výchozím rozsahem (zkratky klasického menu, až
 * CUSTOMSPEED_MAX_VALUE) rozsah prodlouží, aby se hodnota neomezila. Po dobu
 * držení levého tlačítka myši mez neklesá: jinak by se při tažení jezdce
 * rozsah každý snímek zmenšil na právě nastavenou hodnotu a ta by pod
 * stojícím ukazatelem padala dál (review Y1).
 *
 * @param value Nastavená rychlost v procentech.
 * @return Horní mez v procentech, nejméně MACHINE_SPEED_SLIDER_MAX.
 * @pre UI vlákno, uvnitř snímku ImGui; volat nejvýš jednou za snímek.
 */
static float machine_speed_slider_max(float value)
{
    /* mez z minulého snímku; platí, dokud je tlačítko myši dole */
    static float s_held_max = MACHINE_SPEED_SLIDER_MAX;
    float max = (value > MACHINE_SPEED_SLIDER_MAX) ? value : MACHINE_SPEED_SLIDER_MAX;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && s_held_max > max) max = s_held_max;
    s_held_max = max;
    return max;
}

/**
 * @brief Karta SPEED: rychlost Normal / Custom / Max a vlastní rychlost.
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void machine_card_speed(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (machine_card_begin(_("Speed"), "ovl_card_speed", size)) {
        ovl_row_begin(_("CPU speed"));
        {
            /* krátká víceznačná slova: kontext rychlosti procesoru ("Normal" je jinde barevné schéma a velikost okna) */
            const char *labels[3] = {C_("overlay speed", "Normal"), C_("overlay speed", "Custom"), C_("overlay speed", "Max")};
            const char *keycaps[3] = {overlay_action_keycap(OVL_ACTION_SPEED_NORMAL, v), NULL, overlay_action_keycap(OVL_ACTION_SPEED, v)};
            const char *reasons[3] = {NULL, overlay_action_reason(OVL_ACTION_SPEED_CUSTOM, v), NULL};
            int sel = (v->speed_mode == OVERLAY_SPEED_MAX) ? 2 : ((v->speed_mode == OVERLAY_SPEED_CUSTOM) ? 1 : 0);
            if (ovl_segmented_reasons("mach_speed", labels, keycaps, reasons, 3, &sel, -1.0f)) {
                /* Max je přepínač (jako Alt+M): zvolit ho jde jen z jiného režimu, kde ho zapne */
                overlay_action_request((sel == 0) ? OVL_ACTION_SPEED_NORMAL : ((sel == 1) ? OVL_ACTION_SPEED_CUSTOM : OVL_ACTION_SPEED));
            }
            overlay_test_note_item("mach_speed");
        }
        ovl_row_end();

        ovl_row_begin(_("Custom speed"));
        {
            const float gap = floorf(overlay_px(MACHINE_GAP));
            const float btn = floorf(overlay_px(OVL_CONTROL_H));
            const float avail = ImGui::GetContentRegionAvail().x;
            overlay_action_button(OVL_ACTION_SPEED_DOWN, v, _("Decrease by 10 %"), true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), "mach_speed_down");
            ImGui::SameLine(0.0f, gap);
            float value = (float)v->speed_percent;
            const float max = machine_speed_slider_max(value);
            if (ovl_slider_percent_ex("mach_speed_slider", &value, MACHINE_SPEED_SLIDER_MIN, max, 1.0f, avail - 2.0f * (btn + gap)))
                overlay_action_request_value(OVL_ACTION_SPEED_SET, (int)(value + 0.5f));
            overlay_test_note_item("mach_speed_slider");
            ImGui::SameLine(0.0f, gap);
            overlay_action_button(OVL_ACTION_SPEED_UP, v, _("Increase by 10 %"), true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), "mach_speed_up");
        }
        ovl_row_end();
    }
    ovl_card_end();
    overlay_test_note_item("card_speed");
}

/**
 * @brief Výška obsahu karty SNAPSHOTS: tři řady tlačítek.
 * @return Výška v pixelech.
 */
static float machine_snapshots_content_h(void)
{
    return 3.0f * floorf(overlay_px(OVL_CONTROL_H)) + 2.0f * floorf(overlay_px(MACHINE_GAP));
}

/**
 * @brief Karta SNAPSHOTS: Quick Save a Quick Load, Save Snapshot... a Load Snapshot..., Snapshot Setup....
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void machine_card_snapshots(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (machine_card_begin(_("Snapshots"), "ovl_card_snapshots", size)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        const float gap = floorf(overlay_px(MACHINE_GAP));
        const float h = floorf(overlay_px(OVL_CONTROL_H));
        const float half = floorf((w - gap) * 0.5f);
        const float x2 = p.x + w - half;

        ImGui::SetCursorScreenPos(p);
        overlay_action_button(OVL_ACTION_QUICK_SAVE, v, _("Quick Save"), false, OVL_BUTTON_NORMAL, ImVec2(half, h), "snap_quick_save");
        ImGui::SetCursorScreenPos(ImVec2(x2, p.y));
        overlay_action_button(OVL_ACTION_QUICK_LOAD, v, _("Quick Load"), false, OVL_BUTTON_NORMAL, ImVec2(half, h), "snap_quick_load");

        const float y2 = p.y + h + gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y2));
        overlay_action_button(OVL_ACTION_SNAPSHOT_SAVE, v, _("Save Snapshot..."), false, OVL_BUTTON_NORMAL, ImVec2(half, h), "snap_save");
        ImGui::SetCursorScreenPos(ImVec2(x2, y2));
        overlay_action_button(OVL_ACTION_SNAPSHOT_LOAD, v, _("Load Snapshot..."), false, OVL_BUTTON_NORMAL, ImVec2(half, h), "snap_load");

        const float y3 = y2 + h + gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y3));
        overlay_action_button(OVL_ACTION_SNAPSHOT_SETUP, v, _("Snapshot Setup..."), false, OVL_BUTTON_NORMAL, ImVec2(w, h), "snap_setup");
        machine_finish_content(p.x, y3 + h);
    }
    ovl_card_end();
    overlay_test_note_item("card_snapshots");
}

/**
 * @brief Karta MORE HARDWARE: text s odkazem na klasické menu a tlačítko, které ho otevře.
 * @param v      Kopie stavu.
 * @param size   Velikost karty.
 * @param text_h Výška zalomeného textu (machine_more_text_h() pro vnitřní šířku karty).
 */
static void machine_card_more(const st_OVERLAY_VIEW *v, ImVec2 size, float text_h)
{
    if (machine_card_begin(_("More hardware"), "ovl_card_more", size)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = floorf(ImGui::GetContentRegionAvail().x);
        const float gap = floorf(overlay_px(MACHINE_GAP));
        const float h = floorf(overlay_px(OVL_CONTROL_H));

        ImGui::PushFont(overlay_font_ui(), overlay_font_px(OVL_FONT_BODY));
        ImGui::PushStyleColor(ImGuiCol_Text, OVL_COL_TEXT_SECONDARY);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextUnformatted(machine_more_text());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        overlay_test_note_item("more_text");

        const float y = p.y + text_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        overlay_action_button(OVL_ACTION_CLASSIC_MENU, v, _("Open classic menu"), false, OVL_BUTTON_NORMAL, ImVec2(w, h), "more_classic");
        machine_finish_content(p.x, y + h);
    }
    ovl_card_end();
    overlay_test_note_item("card_more");
}

/* ================================================================
 * Tab
 * ================================================================ */

void overlay_tab_machine_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    const bool narrow = overlay_is_narrow();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);
    const float gap = floorf(overlay_px(MACHINE_CARD_GAP));
    const float inner_gap = floorf(overlay_px(MACHINE_GAP));
    const float pad = floorf(overlay_px(OVL_CARD_PAD));
    const float row_h = ovl_row_height();
    const float col_w = narrow ? w : floorf((w - gap) * 0.5f);
    const float more_text_h = machine_more_text_h(col_w - 2.0f * pad);

    float machine_h = ovl_card_height(3.0f * row_h, true);
    float speed_h = ovl_card_height(2.0f * row_h, true);
    float snap_h = ovl_card_height(machine_snapshots_content_h(), true);
    float more_h = ovl_card_height(more_text_h + inner_gap + floorf(overlay_px(OVL_CONTROL_H)), true);

    float y = origin.y;
    if (!narrow) {
        /* dva sloupce: karty vedle sebe mají výšku té vyšší */
        if (machine_h < snap_h) machine_h = snap_h;
        else snap_h = machine_h;
        if (speed_h < more_h) speed_h = more_h;
        else more_h = speed_h;
        const float x2 = origin.x + w - col_w;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_machine(v, ImVec2(col_w, machine_h));
        ImGui::SetCursorScreenPos(ImVec2(x2, y));
        machine_card_snapshots(v, ImVec2(col_w, snap_h));
        y += machine_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_speed(v, ImVec2(col_w, speed_h));
        ImGui::SetCursorScreenPos(ImVec2(x2, y));
        machine_card_more(v, ImVec2(col_w, more_h), more_text_h);
        y += speed_h;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_machine(v, ImVec2(w, machine_h));
        y += machine_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_speed(v, ImVec2(w, speed_h));
        y += speed_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_snapshots(v, ImVec2(w, snap_h));
        y += snap_h + gap;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        machine_card_more(v, ImVec2(w, more_h), more_text_h);
        y += more_h;
    }
    machine_finish_content(origin.x, y);
}
