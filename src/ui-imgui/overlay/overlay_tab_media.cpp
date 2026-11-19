/**
 * @file   overlay_tab_media.cpp
 * @brief  Tab Media overlay menu: mechaniky FDD 0 až 3, kazeta, Quick Disk, pruh nedávných médií a řada akcí.
 *
 * Kreslí jen z kopie stavu (st_OVERLAY_VIEW). Každý ovládací prvek je
 * akce z tabulky akcí (overlay_actions.h): popisek, ikona, štítek zkratky
 * a dostupnost se berou z ní a zvolení akci jen vyžádá; provede ji rámec
 * po vykreslení okna. Výjimkou je výměna média: "Change" / "Insert disk" /
 * "Change tape" otevírají výsuvný list (overlay_sheet_open()) a položky
 * pruhu nedávných médií vkládají médium z historie
 * (overlay_sheet_request_insert(), u diskety po volbě mechaniky). Podle
 * konceptů media-v2 a media-insert-v2 a rulingů K01, K02, K05 až K12 a
 * K17 (gui/DECISIONS.md experimentu).
 *
 * Rozvržení: všechny rozměry jsou násobky jednotky overlaye. V širokém
 * okně dva sloupce (mechaniky 58 % šířky, vpravo kazeta a Quick Disk se
 * stejnou celkovou výškou), pod nimi pruh nedávných médií a řada akcí;
 * v úzkém okně (overlay_is_narrow()) jsou karty pod sebou a řada akcí má
 * dvě řady. Výšky karet se počítají předem, aby sloupce končily ve
 * stejné výšce.
 *
 * Fokus: řádek mechaniky je jedna zastávka navigace (Enter, mezerník
 * nebo klik kamkoli do řádku = "Change" / "Insert disk"); tlačítka
 * uložení a vysunutí jsou další zastávky vpravo. Položky pruhu nedávných
 * médií jsou zastávky navigace. Výchozí fokus tabu má řádek FDD 0.
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
#define MEDIA_CARD_GAP 12.0f
/** @brief Podíl šířky levého sloupce (mechaniky) v dvousloupcovém rozvržení. */
#define MEDIA_LEFT_FRACTION 0.58f
/** @brief Výška řádku mechaniky. */
#define MEDIA_FDD_ROW_H 68.0f
/** @brief Vodorovný vnitřní okraj řádku mechaniky. */
#define MEDIA_ROW_PAD_X 12.0f
/** @brief Mezera mezi prvky řádku. */
#define MEDIA_GAP 8.0f
/** @brief Mezera mezi skupinami prvků řádku (kontrolka, popisek, ikona, text). */
#define MEDIA_GROUP_GAP 16.0f
/** @brief Velikost písma popisku mechaniky ("FDD 0"). */
#define MEDIA_FONT_DRIVE 16.0f
/** @brief Velikost písma jména souboru. */
#define MEDIA_FONT_NAME 14.0f
/** @brief Průměr kontrolky mechaniky. */
#define MEDIA_LED 10.0f
/** @brief Průměr tečky "neuložené změny". */
#define MEDIA_DOT 8.0f
/** @brief Strana ikony média v řádku. */
#define MEDIA_MEDIA_ICON 24.0f
/** @brief Výška ukazatele průběhu pásky. */
#define MEDIA_PROGRESS_H 8.0f
/** @brief Svislá mezera mezi řádky karty kazety. */
#define MEDIA_CMT_GAP 10.0f
/** @brief Výška řádku se jménem pásky. */
#define MEDIA_CMT_NAME_H 28.0f
/** @brief Výška tlačítka řady akcí. */
#define MEDIA_ACTION_H 40.0f
/** @brief Počet tlačítek řady akcí v jedné řadě úzkého okna. */
#define MEDIA_ACTIONS_PER_ROW_NARROW 4
/** @brief Největší podíl šířky řádku nadpisu karty, který smí zabrat důvod zákazu karty (zbytek patří nadpisu). */
#define MEDIA_REASON_FRACTION 0.62f
/** @brief Největší šířka jména souboru v položce pruhu nedávných médií. */
#define MEDIA_CHIP_NAME_MAX 180.0f
/** @brief Vodorovný vnitřní okraj položky pruhu nedávných médií. */
#define MEDIA_CHIP_PAD_X 12.0f

/* ================================================================
 * Pomocné funkce
 * ================================================================ */

/**
 * @brief Rozměry tabu v pixelech pro jedno kreslení.
 *
 * Invariant: všechny hodnoty jsou celé pixely odvozené z jednotky overlaye.
 */
typedef struct st_MEDIA_METRICS {
    float gap;        /**< mezera mezi kartami */
    float pad;        /**< vnitřní okraj karty (jako ovl_card_begin()) */
    float title_h;    /**< výška nadpisu karty včetně linky a mezer */
    float control_h;  /**< výška ovládacího prvku */
    float fdd_row_h;  /**< výška řádku mechaniky */
} st_MEDIA_METRICS;

/**
 * @brief Spočítá rozměry tabu.
 * @param[out] m Rozměry.
 */
static void media_metrics(st_MEDIA_METRICS *m)
{
    m->gap = floorf(overlay_px(MEDIA_CARD_GAP));
    m->pad = floorf(overlay_px(OVL_CARD_PAD));
    /* nadpis karty: řádek písma a dvě mezery kolem oddělovací linky (ovl_card_begin()) */
    m->title_h = overlay_font_px(OVL_FONT_CARD_TITLE) + 2.0f * floorf(overlay_px(10.0f));
    m->control_h = floorf(overlay_px(OVL_CONTROL_H));
    m->fdd_row_h = floorf(overlay_px(MEDIA_FDD_ROW_H));
}

/**
 * @brief Výška karty s nadpisem pro obsah dané výšky.
 * @param m         Rozměry.
 * @param content_h Výška obsahu pod nadpisem.
 * @return Výška karty v pixelech; důvod zákazu ji nemění (kreslí se v řádku nadpisu nebo v pruhu nad kartami).
 */
static float media_card_h(const st_MEDIA_METRICS *m, float content_h)
{
    return 2.0f * m->pad + m->title_h + content_h;
}

/**
 * @brief Důvod zákazu je společný všem kartám médií a kreslí ho pruh nad kartami (media_reason_strip()).
 *
 * Nastavuje overlay_tab_media_draw() před kreslením karet; při true karty
 * svůj důvod nekreslí.
 */
static bool s_media_common_reason = false;

/**
 * @brief Je současná položka zakázaná (uvnitř ImGui::BeginDisabled())?
 */
static bool media_disabled(void)
{
    return (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}

/** @brief Barva hlavního textu; v zakázané kartě barva zakázaného prvku. */
static ImU32 media_col_text(void)
{
    return media_disabled() ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT;
}

/** @brief Barva vedlejšího textu; v zakázané kartě barva zakázaného prvku. */
static ImU32 media_col_secondary(void)
{
    return media_disabled() ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY;
}

/**
 * @brief Šířka textu daným písmem a návrhovou velikostí.
 * @param font   Písmo.
 * @param design Velikost v návrhových bodech.
 * @param text   Text.
 * @return Šířka v pixelech.
 */
static float media_text_w(ImFont *font, float design, const char *text)
{
    ImGui::PushFont(font, overlay_font_size(font, design));
    const float w = ImGui::CalcTextSize(text, NULL, false).x;
    ImGui::PopFont();
    return w;
}

/**
 * @brief Text na jednom řádku na dané pozici, při přetečení s výpustkou a tooltipem s plným zněním.
 * @param x         Levý okraj.
 * @param cy        Svislý střed řádku textu.
 * @param font      Písmo.
 * @param design    Velikost písma v návrhových bodech.
 * @param col       Barva.
 * @param text      Text.
 * @param max_w     Největší šířka v pixelech (nejméně 1).
 * @param test_name Jméno obdélníku pro testy; NULL = nezaznamenávat.
 * @return Šířka vykresleného textu v pixelech.
 */
static float media_text(float x, float cy, ImFont *font, float design, ImU32 col, const char *text, float max_w, const char *test_name)
{
    const float fs = overlay_font_size(font, design);
    if (max_w < 1.0f) max_w = 1.0f;
    ImGui::SetCursorScreenPos(ImVec2(floorf(x), floorf(cy - fs * 0.5f)));
    ImGui::PushFont(font, fs);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ovl_text_ellipsis(text, max_w);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (test_name) overlay_test_note_item(test_name);
    return ImGui::GetItemRectSize().x;
}

/**
 * @brief Přeložený popisek akce.
 * @param id Akce.
 * @return Popisek; u akce bez popisku prázdný text.
 */
static const char *media_action_label(en_OVL_ACTION id)
{
    /* "Rewind" znamená jinde návrat záznamu vstupů na checkpoint: kontext překladu jako v okně Virtual CMT */
    if (id == OVL_ACTION_CMT_REWIND) return C_("cmt", "Rewind");
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);
    return def->label ? _(def->label) : "";
}

/**
 * @brief Tlačítko akce z tabulky akcí.
 *
 * Popisek, ikonu a zkratku bere z tabulky; nedostupnou akci kreslí jako
 * zakázanou a v tooltipu ukáže důvod. Tlačítko jen s ikonou má v tooltipu
 * název akce a zkratku (štítek by se do něj nevešel). Zvolení akci vyžádá.
 *
 * Tlačítko druhu OVL_BUTTON_ACTIVE ukazuje právě činnou funkci (běžící
 * páska): kreslí se jako povolené, i když akce dostupná není; zvolení pak
 * nic nevyžádá a tooltip říká proč.
 *
 * @param id         Akce.
 * @param v          Kopie stavu.
 * @param text       Přeložený popisek; NULL = popisek z tabulky.
 * @param icon_only  true = kreslit jen ikonu (zkratka jde do tooltipu).
 * @param with_icon  false = nekreslit ikonu z tabulky (tlačítko jen s popiskem).
 * @param kind       Druh tlačítka.
 * @param size       Velikost jako u ovl_button_ex().
 * @param test_name  Jméno obdélníku pro testy; NULL = nezaznamenávat. Kreslený štítek zkratky se zaznamená pod jménem s příponou "_keycap".
 * @param show_keycap true = štítek zkratky dostupné akce se kreslí, pokud se do tlačítka vejde; false = skupina
 *                    tlačítek štítky neukazuje (pravidlo "všechny, nebo žádný") a zkratka jde do tooltipu.
 * @return true = tlačítko bylo zvoleno a akce vyžádána.
 * @pre Kurzor okna stojí na pozici tlačítka.
 */
static bool media_button_ex(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, bool with_icon, en_OVL_BUTTON_KIND kind, ImVec2 size,
                            const char *test_name, bool show_keycap)
{
    const st_OVL_ACTION_DEF *def = overlay_action_def(id);
    const char *reason = overlay_action_reason(id, v);
    const char *keycap = overlay_action_keycap(id, v);
    const bool disabled = reason != NULL && kind != OVL_BUTTON_ACTIVE;
    if (!text) text = media_action_label(id);
    char label[384];
    snprintf(label, sizeof(label), "%s###ovl_action_%d", icon_only ? "" : text, (int)id);
    if (icon_only) show_keycap = false;
    const char *icon = with_icon ? def->icon : NULL;

    if (disabled) ImGui::BeginDisabled();
    bool pressed = ovl_button_ex(label, icon, show_keycap ? keycap : NULL, kind, size);
    if (disabled) ImGui::EndDisabled();
    if (test_name) overlay_test_note_item(test_name);
    if (test_name && show_keycap && keycap && !disabled) {
        /* štítek, který tlačítko kreslí u pravého okraje (ovl_button_ex() ho kreslí, jen když se vejde) */
        const ImVec2 kc = ovl_keycap_size(keycap);
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        if (ovl_button_natural_width(text, icon, keycap) <= mx.x - mn.x + 0.5f) {
            char kc_name[48];
            snprintf(kc_name, sizeof(kc_name), "%.36s_keycap", test_name);
            overlay_test_note_rect(kc_name, ImVec2(mx.x - kc.x, mn.y), mx);
        }
    }

    if (reason) {
        ovl_tooltip_disabled(reason);
        pressed = false;
    } else if (icon_only || (keycap && !show_keycap)) {
        char tip[384];
        if (keycap) snprintf(tip, sizeof(tip), "%s (%s)", text, keycap);
        else snprintf(tip, sizeof(tip), "%s", text);
        ovl_tooltip(tip);
    }
    if (pressed) overlay_action_request(id);
    return pressed;
}

/**
 * @brief Tlačítko akce z tabulky akcí, které o štítku zkratky rozhoduje samo (media_button_ex() se show_keycap = true).
 * @param id         Akce.
 * @param v          Kopie stavu.
 * @param text       Přeložený popisek; NULL = popisek z tabulky.
 * @param icon_only  true = kreslit jen ikonu.
 * @param with_icon  false = nekreslit ikonu z tabulky.
 * @param kind       Druh tlačítka.
 * @param size       Velikost jako u ovl_button_ex().
 * @param test_name  Jméno obdélníku pro testy; NULL = nezaznamenávat.
 * @return true = tlačítko bylo zvoleno a akce vyžádána.
 * @pre Kurzor okna stojí na pozici tlačítka.
 */
static bool media_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, bool icon_only, bool with_icon, en_OVL_BUTTON_KIND kind, ImVec2 size,
                         const char *test_name)
{
    return media_button_ex(id, v, text, icon_only, with_icon, kind, size, test_name, true);
}

/**
 * @brief Tlačítko, které otevírá výsuvný list výměny média ("Change tape", "Insert disk").
 *
 * Dostupnost a důvod zákazu bere z akce, která totéž médium vkládá
 * souborovým dialogem. Nic nevyžaduje; list otevře volající. Polohu
 * tlačítka zaznamená jako kotvu listu.
 *
 * @param gate      Akce, jejíž dostupnost tlačítko sdílí.
 * @param v         Kopie stavu.
 * @param label     Popisek "Text###id".
 * @param kind      Druh tlačítka.
 * @param size      Velikost jako u ovl_button_ex().
 * @param test_name Jméno obdélníku pro testy.
 * @param anchor    Druh kotvy listu.
 * @param index     Index kotvy.
 * @return true = tlačítko bylo zvoleno a je dostupné.
 * @pre Kurzor okna stojí na pozici tlačítka.
 */
static bool media_sheet_button(en_OVL_ACTION gate, const st_OVERLAY_VIEW *v, const char *label, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name,
                               en_OVL_SHEET_ANCHOR anchor, int index)
{
    const char *reason = overlay_action_reason(gate, v);
    if (reason) ImGui::BeginDisabled();
    const bool pressed = ovl_button_ex(label, NULL, NULL, kind, size);
    if (reason) ImGui::EndDisabled();
    overlay_test_note_item(test_name);
    overlay_sheet_note_anchor(anchor, index, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (reason) ovl_tooltip_disabled(reason);
    return pressed && !reason;
}

/**
 * @brief Začne kartu tabu; nedostupná karta má obsah zakázaný a důvod v řádku nadpisu vpravo.
 *
 * Důvod se kreslí běžným písmem (OVL_FONT_BODY) jantarově, zarovnaný k
 * pravému okraji karty, nejvýš přes MEDIA_REASON_FRACTION šířky (delší
 * končí výpustkou a má tooltip). Výšku karty ani polohu obsahu nemění. Je-li
 * důvod společný všem kartám (s_media_common_reason), kreslí ho jednou pruh
 * nad kartami a karta ho neopakuje.
 *
 * @param title  Nadpis "Text###id".
 * @param size   Velikost karty v pixelech.
 * @param reason Přeložený důvod, proč je karta zakázaná; NULL = povolená. Prázdný text = zakázaná bez důvodu.
 * @return true = karta je vidět.
 * @post Musí následovat media_card_end().
 */
static bool media_card_begin(const char *title, ImVec2 size, const char *reason)
{
    const bool visible = ovl_card_begin(title, size);
    if (visible && reason && *reason && !s_media_common_reason) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float pad = floorf(overlay_px(OVL_CARD_PAD));
        const float right = wp.x + ImGui::GetWindowSize().x - pad;
        const float max_w = floorf((right - p.x) * MEDIA_REASON_FRACTION);
        float tw = ceilf(media_text_w(overlay_font_ui(), OVL_FONT_BODY, reason));
        if (tw > max_w) tw = max_w;
        /* svislý střed řádku nadpisu karty (ovl_card_begin(): nadpis začíná na vnitřním okraji) */
        const float cy = wp.y + pad + overlay_font_px(OVL_FONT_CARD_TITLE) * 0.5f;
        media_text(right - tw, cy, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_AMBER, reason, tw, "card_reason");
        ImGui::SetCursorScreenPos(p);
    }
    ImGui::BeginDisabled(reason != NULL);
    return visible;
}

/**
 * @brief Pruh s důvodem zákazu společným všem kartám médií, nad kartami.
 *
 * Kreslí se do horního okraje obsahu tabu (nad první kartou), takže
 * rozvržení karet ani řady akcí neposouvá. Jantarový text běžným písmem
 * (OVL_FONT_BODY), při přetečení s výpustkou a tooltipem.
 *
 * @param reason Přeložený důvod (neprázdný).
 * @param origin Levý horní roh obsahu tabu (první karty).
 * @param w      Šířka obsahu.
 * @pre Uvnitř okna obsahu tabu, před první kartou.
 * @post Kurzor okna je zpět na origin.
 */
static void media_reason_strip(const char *reason, ImVec2 origin, float w)
{
    const float pad_y = ImGui::GetCurrentWindow()->WindowPadding.y;
    media_text(origin.x, origin.y - pad_y * 0.5f, overlay_font_ui(), OVL_FONT_BODY, OVL_COL_AMBER, reason, w, "card_reason");
    ImGui::SetCursorScreenPos(origin);
}

/**
 * @brief Ukončí kartu začatou media_card_begin().
 * @param test_name Jméno obdélníku karty pro testy.
 */
static void media_card_end(const char *test_name)
{
    ImGui::EndDisabled();
    ovl_card_end();
    overlay_test_note_item(test_name);
}

/**
 * @brief Uzavře ručně rozmístěný obsah karty položkou, aby kurzor nezůstal za hranicí obsahu.
 * @param x Levý okraj obsahu.
 * @param y Spodní okraj obsahu.
 */
static void media_finish_content(float x, float y)
{
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::PopStyleVar();
}

/**
 * @brief Tečka stavu (kontrolka, neuložené změny) vystředěná na bodě.
 * @param cx     Střed x.
 * @param cy     Střed y.
 * @param design Průměr v návrhových bodech.
 * @param col    Barva.
 */
static void media_dot(float cx, float cy, float design, ImU32 col)
{
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(floorf(cx), floorf(cy)), floorf(overlay_px(design)) * 0.5f, col);
}

/**
 * @brief Text času "mm:ss" (od hodiny "h:mm:ss"); neznámý čas "--:--".
 * @param seconds Čas v sekundách; záporný = neznámý.
 * @param buf     Výstup.
 * @param size    Velikost výstupu.
 */
static void media_time_text(double seconds, char *buf, size_t size)
{
    if (seconds < 0.0) {
        snprintf(buf, size, "--:--");
        return;
    }
    const unsigned long total = (unsigned long)(seconds + 0.5);
    if (total >= 3600ul) snprintf(buf, size, "%lu:%02lu:%02lu", total / 3600ul, (total / 60ul) % 60ul, total % 60ul);
    else snprintf(buf, size, "%02lu:%02lu", total / 60ul, total % 60ul);
}

/* ================================================================
 * Karta FLOPPY DRIVES
 * ================================================================ */

/**
 * @brief Řádek mechaniky: kontrolka, popisek, ikona, jméno souboru se stavem, tlačítka a štítek zkratky.
 *
 * Řádek je položka navigace: klik, Enter nebo mezerník otevře výsuvný list
 * výměny diskety (overlay_sheet_open()). Tlačítko "Change" / "Insert disk"
 * uvnitř řádku dělá totéž jen pro myš (zastávka navigace je řádek) a je
 * kotvou listu; uložení a vysunutí jsou samostatné zastávky. Zkratka ze
 * štítku (Alt+1 až Alt+4) otevírá rovnou souborový dialog.
 *
 * @param v     Kopie stavu.
 * @param drive Číslo mechaniky 0 až 3.
 * @param pos   Levý horní roh řádku.
 * @param size  Velikost řádku.
 * @pre Uvnitř karty mechanik.
 */
static void media_fdd_row(const st_OVERLAY_VIEW *v, int drive, ImVec2 pos, ImVec2 size)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImDrawList *dl = window->DrawList;
    const st_OVERLAY_VIEW_FDD *d = &v->fdd[drive];
    const en_OVL_ACTION a_insert = (en_OVL_ACTION)(OVL_ACTION_FDD_INSERT_0 + drive);
    const en_OVL_ACTION a_eject = (en_OVL_ACTION)(OVL_ACTION_FDD_EJECT_0 + drive);
    const en_OVL_ACTION a_save = (en_OVL_ACTION)(OVL_ACTION_FDD_SAVE_0 + drive);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const float gap = floorf(overlay_px(MEDIA_GAP));
    const float group_gap = floorf(overlay_px(MEDIA_GROUP_GAP));
    const float pad_x = floorf(overlay_px(MEDIA_ROW_PAD_X));
    const float cy = floorf(bb.Min.y + size.y * 0.5f);
    char name[40];

    ImGui::PushID(drive);

    /* řádek jako položka navigace; prvky uvnitř ho smějí překrývat */
    const ImGuiID id = window->GetID("###ovl_fdd_row");
    ImGui::SetCursorScreenPos(bb.Min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::ItemSize(bb.GetSize());
    const bool added = ImGui::ItemAdd(bb, id);
    snprintf(name, sizeof(name), "fdd%d", drive);
    overlay_test_note_rect(name, bb.Min, bb.Max);
    bool hovered = false, held = false, focused = false;
    if (added) {
        if (drive == 0 && overlay_default_focus_wanted() && !media_disabled()) overlay_focus_last_item();
        if (ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_AllowOverlap) && !overlay_action_reason(a_insert, v))
            overlay_sheet_open(OVL_SHEET_FDD, drive);
        /* zakázaný řádek obrys fokusu nekreslí (fokus mu mohl zůstat z doby, kdy byl povolený) */
        focused = ovl_last_item_focused() && !media_disabled();
        if (overlay_action_reason(a_insert, v)) hovered = held = false;
    }
    if (hovered || held) dl->AddRectFilled(bb.Min, bb.Max, held ? OVL_COL_SELECTED : OVL_COL_RAISED, rounding);
    if (drive > 0) dl->AddLine(ImVec2(bb.Min.x, bb.Min.y), ImVec2(bb.Max.x, bb.Min.y), OVL_COL_BORDER, overlay_border_px());

    /* zleva: kontrolka, popisek, ikona */
    float x = bb.Min.x + pad_x;
    const float led = floorf(overlay_px(MEDIA_LED));
    media_dot(x + led * 0.5f, cy, MEDIA_LED, (d->active && !media_disabled()) ? OVL_COL_GREEN : OVL_COL_MUTED);
    x += led + group_gap;
    char drive_label[32];
    snprintf(drive_label, sizeof(drive_label), _("FDD %d"), drive);
    /* sloupec popisku má šířku popisku nejširší mechaniky, aby jména souborů začínala pod sebou */
    float label_w = 0.0f;
    for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) {
        char any_label[32];
        snprintf(any_label, sizeof(any_label), _("FDD %d"), i);
        const float lw = ceilf(media_text_w(overlay_font_ui_bold(), MEDIA_FONT_DRIVE, any_label));
        if (lw > label_w) label_w = lw;
    }
    media_text(x, cy, overlay_font_ui_bold(), MEDIA_FONT_DRIVE, media_col_text(), drive_label, label_w, NULL);
    x += label_w + group_gap;
    const float icon = floorf(overlay_px(MEDIA_MEDIA_ICON));
    if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon * 0.5f)), icon, ICON_LC_SAVE, media_col_secondary())) x += icon + group_gap;
    const float text_x = x;

    /* zprava: štítek zkratky (pevný sloupec), vysunutí, uložení, Change / Insert disk */
    float rx = bb.Max.x - pad_x;
    const float keycap_col = ovl_keycap_size("Alt+4").x;
    const char *keycap = overlay_action_keycap(a_insert, v);
    if (keycap) {
        const ImVec2 kc = ovl_keycap_size(keycap);
        const ImVec2 kp(floorf(rx - kc.x), floorf(cy - kc.y * 0.5f));
        ovl_draw_keycap(dl, kp, keycap, false);
        snprintf(name, sizeof(name), "fdd%d_keycap", drive);
        overlay_test_note_rect(name, kp, ImVec2(kp.x + kc.x, kp.y + kc.y));
    }
    rx -= keycap_col + group_gap;
    const float btn = floorf(overlay_px(OVL_CONTROL_H));
    const float btn_y = floorf(cy - btn * 0.5f);
    if (d->mounted) {
        rx -= btn;
        ImGui::SetCursorScreenPos(ImVec2(rx, btn_y));
        snprintf(name, sizeof(name), "fdd%d_eject", drive);
        media_button(a_eject, v, NULL, true, true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), name);
        rx -= gap;
        if (d->unsaved) {
            rx -= btn;
            ImGui::SetCursorScreenPos(ImVec2(rx, btn_y));
            snprintf(name, sizeof(name), "fdd%d_save", drive);
            media_button(a_save, v, NULL, true, true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), name);
            rx -= gap;
        }
    }
    /* Change / Insert disk: výsuvný list jako řádek, jen pro myš (zastávkou navigace je řádek) */
    /* "Change" je tu sloveso (vyměnit disketu); bez kontextu má katalog podstatné jméno (druh události breakpointu) */
    const char *insert_text = d->mounted ? C_("overlay media", "Change") : _("Insert disk");
    const float insert_min = 2.0f * overlay_px(12.0f) + media_text_w(overlay_font_ui(), OVL_FONT_BODY, insert_text);
    float insert_w = ceilf(insert_min);
    const float insert_limit = rx - text_x - floorf(overlay_px(64.0f));
    if (insert_w > insert_limit) insert_w = (insert_limit > btn) ? floorf(insert_limit) : btn;
    rx -= insert_w;
    ImGui::SetCursorScreenPos(ImVec2(rx, btn_y));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    snprintf(name, sizeof(name), "fdd%d_insert", drive);
    char insert_label[160];
    snprintf(insert_label, sizeof(insert_label), "%s###ovl_fdd_insert", insert_text);
    if (media_sheet_button(a_insert, v, insert_label, d->mounted ? OVL_BUTTON_NORMAL : OVL_BUTTON_DASHED, ImVec2(insert_w, btn), name, OVL_SHEET_ANCHOR_FDD, drive))
        overlay_sheet_open(OVL_SHEET_FDD, drive);
    ImGui::PopItemFlag();
    rx -= group_gap;

    /* uprostřed: jméno souboru a stav, nebo "Empty" */
    const float text_w = rx - text_x;
    if (d->mounted) {
        const float name_fs = overlay_font_size(overlay_font_mono(), MEDIA_FONT_NAME);
        const float hint_fs = overlay_font_px(OVL_FONT_HINT);
        const float line_gap = floorf(overlay_px(4.0f));
        const float top = floorf(cy - (name_fs + line_gap + hint_fs) * 0.5f);
        snprintf(name, sizeof(name), "fdd%d_name", drive);
        media_text(text_x, top + name_fs * 0.5f, overlay_font_mono(), MEDIA_FONT_NAME, media_col_text(), d->name, text_w, name);
        const float line_cy = top + name_fs + line_gap + hint_fs * 0.5f;
        snprintf(name, sizeof(name), "fdd%d_state", drive);
        if (d->unsaved) {
            const float dot = floorf(overlay_px(MEDIA_DOT));
            media_dot(text_x + dot * 0.5f, line_cy, MEDIA_DOT, media_disabled() ? OVL_COL_MUTED : OVL_COL_AMBER);
            media_text(text_x + dot + gap, line_cy, overlay_font_ui(), OVL_FONT_HINT, media_col_secondary(), _("Unsaved changes"), text_w - dot - gap, name);
        } else {
            char info[192];
            overlay_media_fdd_info_text(d, info, sizeof(info));
            if (info[0]) media_text(text_x, line_cy, overlay_font_ui(), OVL_FONT_HINT, media_col_secondary(), info, text_w, name);
        }
    } else {
        snprintf(name, sizeof(name), "fdd%d_name", drive);
        media_text(text_x, cy, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), C_("overlay drive", "Empty"), text_w, name);
    }

    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);
    ImGui::PopID();
}

void overlay_media_fdd_info_text(const st_OVERLAY_VIEW_FDD *d, char *buf, size_t size)
{
    if (!buf || size == 0) return;
    buf[0] = '\0';
    if (!d || !d->mounted) return;
    /* velikost dat obrazu; typ souborového systému jádro levně nezná */
    if (d->size_kb > 0) snprintf(buf, size, _("%u KB"), d->size_kb);
    /* obraz sdílený s jinou mechanikou: poznámka místo obecného "Read-only" (do souboru smí zapisovat jen jedna mechanika) */
    char ro[128];
    ro[0] = '\0';
    if (d->shared_readonly && d->shared_with >= 0) snprintf(ro, sizeof(ro), _("Shared with FDD %d, write-protected"), d->shared_with);
    else if (d->readonly) snprintf(ro, sizeof(ro), "%s", _("Read-only"));
    const size_t used = strlen(buf);
    if (ro[0] && used < size) snprintf(buf + used, size - used, "%s%s", used ? " \xC2\xB7 " : "", ro);
}

/**
 * @brief Karta FLOPPY DRIVES se čtyřmi řádky mechanik.
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void media_card_fdd(const st_OVERLAY_VIEW *v, const st_MEDIA_METRICS *m, ImVec2 size)
{
    char title[128];
    snprintf(title, sizeof(title), "%s###ovl_card_fdd", _("Floppy drives"));
    /* důvod zákazu celé karty = důvod, proč nejde vložit disketu do FDD 0 (replay, odpojený řadič) */
    if (media_card_begin(title, size, overlay_action_reason(OVL_ACTION_FDD_INSERT_0, v))) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < OVERLAY_VIEW_FDD_COUNT; i++) media_fdd_row(v, i, ImVec2(p.x, p.y + (float)i * m->fdd_row_h), ImVec2(w, m->fdd_row_h));
        media_finish_content(p.x, p.y + (float)OVERLAY_VIEW_FDD_COUNT * m->fdd_row_h);
    }
    media_card_end("card_fdd");
}

/* ================================================================
 * Karta CASSETTE
 * ================================================================ */

/**
 * @brief Výška obsahu karty kazety.
 * @param m Rozměry.
 * @return Výška v pixelech.
 */
static float media_cmt_content_h(const st_MEDIA_METRICS *m)
{
    const float gap = floorf(overlay_px(MEDIA_CMT_GAP));
    return floorf(overlay_px(MEDIA_CMT_NAME_H)) + gap + floorf(overlay_px(MEDIA_PROGRESS_H)) + floorf(overlay_px(6.0f)) + overlay_font_px(OVL_FONT_BODY) + gap
           + m->control_h + gap + m->control_h;
}

/**
 * @brief Karta CASSETTE: jméno pásky, průběh, pozice, transport, režim načítání a tlačítka pásky ("Change tape" otevírá výsuvný list).
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void media_card_cmt(const st_OVERLAY_VIEW *v, const st_MEDIA_METRICS *m, ImVec2 size)
{
    char title[128];
    snprintf(title, sizeof(title), "%s###ovl_card_cmt", _("Cassette"));
    const char *card_reason = v->media_disabled_reason[0] ? v->media_disabled_reason : NULL;
    if (media_card_begin(title, size, card_reason)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const st_OVERLAY_VIEW_CMT *c = &v->cmt;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float gap = floorf(overlay_px(MEDIA_GAP));
        const float row_gap = floorf(overlay_px(MEDIA_CMT_GAP));
        const bool disabled = media_disabled();
        float y = p.y;

        /* jméno pásky */
        const float name_h = floorf(overlay_px(MEDIA_CMT_NAME_H));
        const float icon = floorf(overlay_px(MEDIA_MEDIA_ICON));
        float x = p.x;
        if (ovl_draw_icon(dl, ImVec2(x, floorf(y + (name_h - icon) * 0.5f)), icon, ICON_LC_CASSETTE_TAPE, media_col_secondary())) x += icon + floorf(overlay_px(MEDIA_GROUP_GAP));
        const float dot = floorf(overlay_px(MEDIA_DOT));
        const float name_max = p.x + w - x - (c->modified ? dot + gap : 0.0f);
        float name_w;
        if (!c->has_tape) name_w = media_text(x, y + name_h * 0.5f, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), _("No tape"), name_max, "cmt_name");
        else if (!c->name[0]) name_w = media_text(x, y + name_h * 0.5f, overlay_font_ui(), OVL_FONT_BODY, media_col_text(), _("New tape"), name_max, "cmt_name");
        else name_w = media_text(x, y + name_h * 0.5f, overlay_font_mono(), MEDIA_FONT_NAME, media_col_text(), c->name, name_max, "cmt_name");
        if (c->modified) {
            /* neuložené změny pásky: tečka za jménem, význam v tooltipu */
            const ImVec2 dp(floorf(x + name_w + gap), floorf(y + (name_h - dot) * 0.5f));
            ImGui::SetCursorScreenPos(dp);
            ImGui::Dummy(ImVec2(dot, dot));
            overlay_test_note_item("cmt_modified");
            ovl_tooltip_disabled(_("Unsaved changes"));
            media_dot(dp.x + dot * 0.5f, dp.y + dot * 0.5f, MEDIA_DOT, disabled ? OVL_COL_MUTED : OVL_COL_AMBER);
        }
        y += name_h + row_gap;

        /* průběh */
        const float bar_h = floorf(overlay_px(MEDIA_PROGRESS_H));
        const ImVec2 b0(p.x, y), b1(p.x + w, y + bar_h);
        dl->AddRectFilled(b0, b1, OVL_COL_BORDER_STRONG, bar_h * 0.5f);
        if (c->has_tape && c->progress > 0.0) {
            ImU32 col = OVL_COL_MUTED;
            if (!disabled) {
                if (c->state == OVERLAY_CMT_RECORD) col = OVL_COL_RED;
                else if (c->state == OVERLAY_CMT_PLAY) col = c->paused ? OVL_COL_AMBER : OVL_COL_GREEN;
            }
            float fill = floorf(w * (float)c->progress);
            if (fill < bar_h) fill = bar_h;
            dl->AddRectFilled(b0, ImVec2(b0.x + fill, b1.y), col, bar_h * 0.5f);
        }
        overlay_test_note_rect("cmt_progress", b0, b1);
        y += bar_h + floorf(overlay_px(6.0f));

        /* pozice: blok vlevo, čas vpravo */
        const float line_fs = overlay_font_px(OVL_FONT_BODY);
        if (c->has_tape) {
            char left[160], right[64], t0[24], t1[24];
            if (c->block_count == 0 && c->state != OVERLAY_CMT_RECORD) snprintf(left, sizeof(left), "%s", _("Empty tape"));
            else if (c->state == OVERLAY_CMT_RECORD) snprintf(left, sizeof(left), _("Recording block %d"), c->block_count + 1);
            else if (c->current_block < 0) snprintf(left, sizeof(left), _("Block - / %d"), c->block_count);
            else snprintf(left, sizeof(left), _("Block %d / %d"), c->current_block + 1, c->block_count);
            media_time_text(c->elapsed_s, t0, sizeof(t0));
            media_time_text(c->total_s, t1, sizeof(t1));
            snprintf(right, sizeof(right), "%s / %s", t0, t1);
            const float right_w = ceilf(media_text_w(overlay_font_ui(), OVL_FONT_BODY, right));
            const bool show_right = right_w + gap < w * 0.6f;
            media_text(p.x, y + line_fs * 0.5f, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), left, show_right ? w - right_w - gap : w, "cmt_block");
            if (show_right) media_text(p.x + w - right_w, y + line_fs * 0.5f, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), right, right_w, "cmt_time");
        }
        y += line_fs + row_gap;

        /* transport a režim načítání */
        const float btn = m->control_h;
        static const en_OVL_ACTION transport[] = {OVL_ACTION_CMT_REWIND, OVL_ACTION_CMT_PLAY, OVL_ACTION_CMT_PAUSE, OVL_ACTION_CMT_STOP, OVL_ACTION_CMT_RECORD};
        static const char *const transport_names[] = {"cmt_rewind", "cmt_play", "cmt_pause", "cmt_stop", "cmt_record"};
        const bool playing = c->has_tape && c->state == OVERLAY_CMT_PLAY && !c->paused;
        const bool recording = c->has_tape && c->state == OVERLAY_CMT_RECORD;
        x = p.x;
        for (int i = 0; i < 5; i++) {
            en_OVL_BUTTON_KIND kind = OVL_BUTTON_NORMAL;
            const char *text = NULL;
            if (transport[i] == OVL_ACTION_CMT_PLAY && playing) kind = OVL_BUTTON_ACTIVE;
            if (transport[i] == OVL_ACTION_CMT_RECORD) {
                kind = OVL_BUTTON_DANGER;
                if (recording) text = _("Stop recording");
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            media_button(transport[i], v, text, true, true, kind, ImVec2(btn, btn), transport_names[i]);
            if (transport[i] == OVL_ACTION_CMT_RECORD && recording && !disabled)
                dl->AddRect(ImVec2(x, y), ImVec2(x + btn, y + btn), OVL_COL_RED, overlay_px(OVL_RADIUS_CONTROL), 0, overlay_border_px());
            x += btn + gap;
        }
        {
            /* segmenty režimu: nedostupný režim je zakázaný s důvodem; zvolení akci jen vyžádá, stav přijde z jádra */
            const char *labels[OVERLAY_CMT_MODE_COUNT];
            const char *reasons[OVERLAY_CMT_MODE_COUNT];
            for (int i = 0; i < (int)OVERLAY_CMT_MODE_COUNT; i++) {
                labels[i] = media_action_label((en_OVL_ACTION)(OVL_ACTION_CMT_MODE_TAPE + i));
                reasons[i] = overlay_action_reason((en_OVL_ACTION)(OVL_ACTION_CMT_MODE_TAPE + i), v);
            }
            int mode = (int)c->mode;
            const float seg_x = x + gap;
            float seg_w = p.x + w - seg_x;
            const float seg_min = 3.0f * btn;
            float seg_y = y;
            float seg_left = seg_x;
            if (seg_w < seg_min) {
                /* nevejde se vedle transportu: vlastní řádek pod ním (jen ve velmi úzké kartě) */
                seg_left = p.x;
                seg_w = w;
                seg_y = y + btn + gap;
            }
            ImGui::SetCursorScreenPos(ImVec2(seg_left, seg_y));
            if (ovl_segmented_reasons("###ovl_cmt_mode", labels, NULL, reasons, (int)OVERLAY_CMT_MODE_COUNT, &mode, seg_w)) overlay_action_request((en_OVL_ACTION)(OVL_ACTION_CMT_MODE_TAPE + mode));
            overlay_test_note_item("cmt_mode");
            if (seg_y > y) y = seg_y;
        }
        y += btn + row_gap;

        /* páska: jiná páska, okno Virtual CMT */
        const float half = floorf((w - gap) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        {
            /* Change tape: výsuvný list výměny pásky; dostupnost jako otevření jiné pásky dialogem */
            char change_label[200];
            snprintf(change_label, sizeof(change_label), "%s###ovl_cmt_change", media_action_label(OVL_ACTION_CMT_CHANGE));
            if (media_sheet_button(OVL_ACTION_CMT_CHANGE, v, change_label, OVL_BUTTON_NORMAL, ImVec2(half, btn), "cmt_change", OVL_SHEET_ANCHOR_CMT, 0))
                overlay_sheet_open(OVL_SHEET_CMT, 0);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - half, y));
        media_button(OVL_ACTION_CMT_WINDOW, v, NULL, false, false, OVL_BUTTON_NORMAL, ImVec2(half, btn), "cmt_window");
        media_finish_content(p.x, y + btn);
    }
    media_card_end("card_cmt");
}

/* ================================================================
 * Karta QUICK DISK
 * ================================================================ */

/**
 * @brief Karta QUICK DISK: jeden řádek s ikonou, jménem média nebo "Empty" a tlačítky ("Insert disk" otevírá výsuvný list).
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void media_card_qd(const st_OVERLAY_VIEW *v, const st_MEDIA_METRICS *m, ImVec2 size)
{
    char title[128];
    snprintf(title, sizeof(title), "%s###ovl_card_qd", _("Quick Disk"));
    const st_OVERLAY_VIEW_QD *q = &v->qd;
    /* důvod zákazu celé karty = důvod akce, která by jinak byla dostupná (vysunutí vloženého, vložení do prázdné) */
    const char *card_reason = overlay_action_reason(q->mounted ? OVL_ACTION_QD_EJECT : OVL_ACTION_QD_INSERT, v);
    if (media_card_begin(title, size, card_reason)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float gap = floorf(overlay_px(MEDIA_GAP));
        const float btn = m->control_h;
        const float cy = floorf(p.y + btn * 0.5f);
        const float icon = floorf(overlay_px(MEDIA_MEDIA_ICON));
        float x = p.x;
        if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon * 0.5f)), icon, q->directory ? ICON_LC_FOLDER : ICON_LC_SAVE, media_col_secondary()))
            x += icon + floorf(overlay_px(MEDIA_GROUP_GAP));

        float rx = p.x + w;
        if (q->mounted) {
            rx -= btn;
            ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
            media_button(OVL_ACTION_QD_EJECT, v, NULL, true, true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), "qd_eject");
            rx -= gap;
            if (q->unsaved) {
                rx -= btn;
                ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                media_button(OVL_ACTION_QD_SAVE, v, NULL, true, true, OVL_BUTTON_NORMAL, ImVec2(btn, btn), "qd_save");
                rx -= gap;
            }
        } else {
            const char *text = media_action_label(OVL_ACTION_QD_INSERT);
            float bw = ceilf(2.0f * overlay_px(12.0f) + media_text_w(overlay_font_ui(), OVL_FONT_BODY, text));
            const float limit = rx - x - floorf(overlay_px(64.0f));
            if (bw > limit) bw = (limit > btn) ? floorf(limit) : btn;
            rx -= bw;
            ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
            char insert_label[200];
            snprintf(insert_label, sizeof(insert_label), "%s###ovl_qd_insert", text);
            /* Insert disk: výsuvný list; dostupnost jako vložení dialogem (jen do prázdné mechaniky) */
            if (media_sheet_button(OVL_ACTION_QD_INSERT, v, insert_label, OVL_BUTTON_DASHED, ImVec2(bw, btn), "qd_insert", OVL_SHEET_ANCHOR_QD, 0))
                overlay_sheet_open(OVL_SHEET_QD, 0);
            rx -= gap;
        }
        rx -= gap;

        const float dot = floorf(overlay_px(MEDIA_DOT));
        const float text_max = rx - x - (q->unsaved ? dot + gap : 0.0f);
        if (q->mounted) {
            const float name_w = media_text(x, cy, overlay_font_mono(), MEDIA_FONT_NAME, media_col_text(), q->name, text_max, "qd_name");
            if (q->unsaved) {
                const ImVec2 dp(floorf(x + name_w + gap), floorf(cy - dot * 0.5f));
                ImGui::SetCursorScreenPos(dp);
                ImGui::Dummy(ImVec2(dot, dot));
                overlay_test_note_item("qd_unsaved");
                ovl_tooltip_disabled(_("Unsaved changes"));
                media_dot(dp.x + dot * 0.5f, dp.y + dot * 0.5f, MEDIA_DOT, media_disabled() ? OVL_COL_MUTED : OVL_COL_AMBER);
            }
        } else {
            media_text(x, cy, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), C_("overlay drive", "Empty"), text_max, "qd_name");
        }
        media_finish_content(p.x, p.y + btn);
    }
    media_card_end("card_qd");
}

/* ================================================================
 * Pruh RECENT MEDIA
 * ================================================================ */

/**
 * @brief Položka pruhu nedávných médií: ikona média a jméno souboru neproporcionálním písmem.
 *
 * Položka je zastávka navigace. Tooltip ukazuje celou cestu a u položky,
 * kterou nejde vložit, důvod; chybějící soubor se kreslí šedě. Polohu
 * zaznamená jako kotvu volby mechaniky.
 *
 * @param index  Pořadí položky v st_OVERLAY_VIEW::recent (ID, kotva a jméno obdélníku pro testy "recent_chip<index>").
 * @param r      Položka.
 * @param reason Přeložený důvod, proč položku nejde vložit; NULL = jde.
 * @param pos    Levý horní roh.
 * @param size   Velikost (šířka z media_chip_width()).
 * @return true = položka byla zvolena (klik, Enter, mezerník) a jde vložit.
 */
static bool media_chip(int index, const st_OVERLAY_VIEW_RECENT *r, const char *reason, ImVec2 pos, ImVec2 size)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImDrawList *dl = window->DrawList;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const float rounding = overlay_px(OVL_RADIUS_CONTROL);
    const float pad = floorf(overlay_px(MEDIA_CHIP_PAD_X));
    const float gap = floorf(overlay_px(MEDIA_GAP));
    const float cy = floorf(bb.Min.y + size.y * 0.5f);
    char name[40];

    ImGui::PushID(index);
    const ImGuiID id = window->GetID("###ovl_recent_chip");
    ImGui::PopID();
    ImGui::SetCursorScreenPos(bb.Min);
    ImGui::ItemSize(bb.GetSize());
    if (reason) ImGui::BeginDisabled();
    const bool added = ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false, pressed = false, focused = false;
    if (added) {
        pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        focused = ovl_last_item_focused() && !reason;
    }
    if (reason) ImGui::EndDisabled();
    snprintf(name, sizeof(name), "recent_chip%d", index);
    overlay_test_note_rect(name, bb.Min, bb.Max);
    overlay_sheet_note_anchor(OVL_SHEET_ANCHOR_CHIP, index, bb.Min, bb.Max);
    if (!added) return false;

    const bool dim = reason != NULL || media_disabled();
    if (dim) hovered = held = pressed = false;
    dl->AddRectFilled(bb.Min, bb.Max, dim ? OVL_COL_CARD : (held ? OVL_COL_PRESSED : (hovered ? OVL_COL_SELECTED : OVL_COL_RAISED)), rounding);
    dl->AddRect(bb.Min, bb.Max, dim ? OVL_COL_BORDER_STRONG : OVL_COL_BORDER_CONTROL, rounding, 0, overlay_border_px());
    float x = bb.Min.x + pad;
    const float icon = floorf(overlay_px(OVL_ICON));
    const char *glyph = (r->kind == OVERLAY_MEDIA_CMT) ? ICON_LC_CASSETTE_TAPE : (r->is_dir ? ICON_LC_FOLDER : ICON_LC_SAVE);
    if (ovl_draw_icon(dl, ImVec2(x, floorf(cy - icon * 0.5f)), icon, glyph, (dim || r->missing) ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT_SECONDARY)) x += icon + gap;
    const float fs = overlay_font_size(overlay_font_mono(), MEDIA_FONT_NAME);
    ImGui::PushFont(overlay_font_mono(), fs);
    const char *end = r->name + strlen(r->name);
    const ImVec2 ts = ImGui::CalcTextSize(r->name, end, false);
    const float max_w = bb.Max.x - pad - x;
    const ImU32 col = (dim || r->missing) ? OVL_COL_TEXT_DISABLED : OVL_COL_TEXT;
    const ImVec2 tp(floorf(x), floorf(cy - ts.y * 0.5f));
    if (ts.x <= max_w + 0.5f) {
        dl->AddText(tp, col, r->name, end);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::RenderTextEllipsis(dl, tp, ImVec2(tp.x + max_w, tp.y + ts.y), tp.x + max_w, r->name, end, &ts);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (focused) ovl_draw_focus_ring(bb.Min, bb.Max, rounding);

    /* tooltip: celá cesta a případně proč položku nejde vložit */
    char tip[OVERLAY_VIEW_PATH_SIZE + 256];
    const char *note = (reason && *reason) ? reason : (r->missing ? _("File not found") : NULL);
    if (note) snprintf(tip, sizeof(tip), "%s\n%s", r->path, note);
    else snprintf(tip, sizeof(tip), "%s", r->path);
    ovl_tooltip_disabled(tip);
    return pressed;
}

/**
 * @brief Šířka položky pruhu nedávných médií.
 * @param r Položka.
 * @return Šířka v pixelech: okraje, ikona a jméno souboru zkrácené na MEDIA_CHIP_NAME_MAX.
 */
static float media_chip_width(const st_OVERLAY_VIEW_RECENT *r)
{
    float name_w = ceilf(media_text_w(overlay_font_mono(), MEDIA_FONT_NAME, r->name));
    const float name_max = floorf(overlay_px(MEDIA_CHIP_NAME_MAX));
    if (name_w > name_max) name_w = name_max;
    float w = 2.0f * floorf(overlay_px(MEDIA_CHIP_PAD_X)) + name_w;
    if (overlay_style_has_icons()) w += floorf(overlay_px(OVL_ICON)) + floorf(overlay_px(MEDIA_GAP));
    return w;
}

/**
 * @brief Pruh RECENT MEDIA: nedávná média všech druhů od naposledy vloženého a tlačítko "Browse files...".
 *
 * Položek se kreslí tolik, kolik se vejde do jedné řady vedle tlačítka.
 * Zvolení položky: páska a Quick Disk se vloží rovnou
 * (overlay_sheet_request_insert()), u obrazu diskety se otevře volba
 * mechaniky "Insert into" (overlay_sheet_open_pick()); chybějící soubor se
 * při pokusu o vložení odstraní z historie. Bez historie ukazuje pruh text
 * prázdného stavu. Při neprázdné historii je před tlačítkem "Browse
 * files..." za svislým oddělovačem textové tlačítko "Clear history" bez
 * okraje a výplně (OVL_BUTTON_TEXT; akce "Clear recent media", vyprázdní
 * historii všech druhů), aby nevypadalo jako další položka média. Věta o přetahování se neuvádí, dokud
 * přetahování neexistuje.
 *
 * @param v    Kopie stavu.
 * @param m    Rozměry.
 * @param size Velikost karty.
 */
static void media_card_recent(const st_OVERLAY_VIEW *v, const st_MEDIA_METRICS *m, ImVec2 size)
{
    char title[128];
    snprintf(title, sizeof(title), "%s###ovl_card_recent", _("Recent media"));
    if (media_card_begin(title, size, NULL)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float btn = m->control_h;
        const float gap = floorf(overlay_px(MEDIA_GROUP_GAP));
        const char *text = media_action_label(OVL_ACTION_BROWSE_FILES);
        float bw = ceilf(2.0f * overlay_px(12.0f) + overlay_px(OVL_ICON) + overlay_px(MEDIA_GAP) + media_text_w(overlay_font_ui(), OVL_FONT_BODY, text));
        if (bw > w * 0.6f) bw = floorf(w * 0.6f);
        if (v->recent_count == 0) {
            media_text(p.x, p.y + btn * 0.5f, overlay_font_ui(), OVL_FONT_BODY, media_col_secondary(), _("No recent media yet. Use Browse files..."), w - bw - gap,
                       "recent_empty");
        } else {
            /* položky zleva, dokud se vejdou před tlačítka; pořadí navigace: položky, Clear, Browse files */
            const float chip_gap = floorf(overlay_px(MEDIA_GAP));
            const char *clear_text = _("Clear history");
            float cbw = ceilf(2.0f * overlay_px(12.0f) + media_text_w(overlay_font_ui(), OVL_FONT_BODY, clear_text));
            if (cbw > w * 0.25f) cbw = floorf(w * 0.25f);
            /* před "Clear history" svislý oddělovač: akce nad historií nepatří mezi položky médií (posudek N06) */
            const float sep_x = floorf(p.x + w - bw - gap - cbw - gap * 0.5f);
            const float limit = sep_x - gap * 0.5f - chip_gap;
            float x = p.x;
            for (unsigned i = 0; i < v->recent_count; i++) {
                const st_OVERLAY_VIEW_RECENT *r = &v->recent[i];
                const float cw = media_chip_width(r);
                if (x + cw > limit) break;
                if (media_chip((int)i, r, overlay_sheet_insert_reason(v, r), ImVec2(x, p.y), ImVec2(cw, btn))) {
                    /* disketa: do které mechaniky; páska a Quick Disk mají jedinou */
                    if (r->kind == OVERLAY_MEDIA_DSK && !r->missing) overlay_sheet_open_pick(r, (int)i);
                    else overlay_sheet_request_insert(r, 0);
                }
                x += cw + chip_gap;
            }
            ImGui::GetWindowDrawList()->AddLine(ImVec2(sep_x, p.y + floorf(overlay_px(6.0f))), ImVec2(sep_x, p.y + btn - floorf(overlay_px(6.0f))), OVL_COL_BORDER_STRONG,
                                                overlay_border_px());
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - gap - cbw, p.y));
            media_button(OVL_ACTION_RECENT_CLEAR, v, clear_text, false, false, OVL_BUTTON_TEXT, ImVec2(cbw, btn), "recent_clear");
            ovl_tooltip(_("Clear recent media"));
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw, p.y));
        media_button(OVL_ACTION_BROWSE_FILES, v, text, false, true, OVL_BUTTON_NORMAL, ImVec2(bw, btn), "recent_browse");
        media_finish_content(p.x, p.y + btn);
    }
    media_card_end("card_recent");
}

/* ================================================================
 * Řada akcí
 * ================================================================ */

/**
 * @brief Výška obsahu karty řady akcí.
 * @param narrow true = úzké okno (dvě řady).
 * @return Výška v pixelech.
 */
static float media_actions_content_h(bool narrow)
{
    const float h = floorf(overlay_px(MEDIA_ACTION_H));
    return narrow ? 2.0f * h + floorf(overlay_px(MEDIA_GAP)) : h;
}

/**
 * @brief Řada akcí: Resume, Speed, Quick Save, Quick Load, Screenshot, Fullscreen, oddělovač, Reset.
 *
 * V širokém okně mají tlačítka šířku podle obsahu a zbytek řady si dělí
 * rovným dílem. Štítek zkratky má tolik akcí, kolik se jich do řady vejde
 * (posudek N02): ubírají se v pevném pořadí priority Fullscreen,
 * Screenshot, Speed, Quick Load, Quick Save, Reset. Měří se se zkratkami z
 * tabulky akcí, jako by byly všechny akce dostupné, takže výsledek závisí
 * jen na šířce okna a jazyce, ne na tom, která akce je právě zakázaná.
 * Zakázaná akce štítek nekreslí, ale místo pro něj zůstává, takže se řada
 * při změně stavu nepřeskládá. Skryté zkratky jsou v tooltipu. Při stejných
 * šířkách tlačítek (úzké okno, nebo když se řada nevejde ani bez štítků) si
 * štítek od nejdůležitější akce nechají ty, kterým se do tlačítka vejde;
 * první, které se nevejde, ho bere i všem méně důležitým. Popisky se pak
 * podle potřeby zkracují.
 * "Resume" štítek nemá (F10 patří tlačítku Close); "Reset" je poslední,
 * za svislým oddělovačem. V úzkém okně jsou tlačítka ve dvou řadách se
 * stejnou šířkou.
 *
 * @param v    Kopie stavu.
 * @param size Velikost karty.
 */
static void media_card_actions(const st_OVERLAY_VIEW *v, ImVec2 size)
{
    if (media_card_begin("###ovl_card_actions", size, NULL)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float gap = floorf(overlay_px(MEDIA_GAP));
        const float h = floorf(overlay_px(MEDIA_ACTION_H));
        const bool narrow = overlay_is_narrow();
        static const en_OVL_ACTION actions[] = {OVL_ACTION_RESUME,     OVL_ACTION_SPEED,      OVL_ACTION_QUICK_SAVE, OVL_ACTION_QUICK_LOAD,
                                                OVL_ACTION_SCREENSHOT, OVL_ACTION_FULLSCREEN, OVL_ACTION_RESET};
        static const char *const names[] = {"act_resume", "act_speed", "act_quick_save", "act_quick_load", "act_screenshot", "act_fullscreen", "act_reset"};
        const int count = (int)(sizeof(actions) / sizeof(actions[0]));
        /* pořadí, ve kterém akce přicházejí o štítek zkratky (indexy do actions): Fullscreen (dlouhý štítek, zkratka
         * je vidět na tabu Display), Screenshot, Speed, Quick Load, Quick Save a poslední Reset */
        static const int s_drop_order[] = {5, 4, 1, 3, 2, 6};
        const int drop_count = (int)(sizeof(s_drop_order) / sizeof(s_drop_order[0]));
        const int per_row = narrow ? MEDIA_ACTIONS_PER_ROW_NARROW : count;
        /* oddělovač před Reset zabírá dvě mezery navíc v řadě, kde Reset je */
        const float sep_w = 2.0f * gap;

        char speed[128];
        if (v->speed_mode == OVERLAY_SPEED_MAX) snprintf(speed, sizeof(speed), "%s", _("Speed Max"));
        else snprintf(speed, sizeof(speed), _("Speed %u %%"), v->speed_percent);

        /* šířky a štítky: měří se se zkratkami z tabulky akcí, jako by byly všechny akce dostupné, takže řada
         * vypadá stejně ve všech stavech (posudek N02). Štítky se ubírají v pevném pořadí s_drop_order. */
        float widths[8], plain[8], full[8];
        bool show_kc[8];
        float total = (float)(count - 1) * gap + sep_w;
        const float equal_w = floorf((w - (float)(per_row - 1) * gap - (narrow ? 0.0f : sep_w)) / (float)per_row);
        for (int i = 0; i < count; i++) {
            const st_OVL_ACTION_DEF *def = overlay_action_def(actions[i]);
            const char *text = (actions[i] == OVL_ACTION_SPEED) ? speed : media_action_label(actions[i]);
            plain[i] = ovl_button_natural_width(text, def->icon, NULL);
            full[i] = ovl_button_natural_width(text, def->icon, def->keycap);
            total += full[i];
            widths[i] = equal_w;
            show_kc[i] = def->keycap != NULL;
        }
        if (!narrow) {
            /* jedna řada: ubírat štítky podle priority, dokud se řada nevejde */
            for (int k = 0; k < drop_count && total > w; k++) {
                const int i = s_drop_order[k];
                if (!show_kc[i]) continue;
                show_kc[i] = false;
                total -= full[i] - plain[i];
            }
        }
        if (!narrow && total <= w) {
            /* šířka podle obsahu, zbytek rovným dílem */
            const float extra = floorf((w - total) / (float)count);
            for (int i = 0; i < count; i++) widths[i] = (show_kc[i] ? full[i] : plain[i]) + extra;
        } else {
            /* stejné šířky (dvě řady, nebo se řada nevejde ani bez štítků): od nejdůležitější akce si štítek nechají
             * ty, kterým se do tlačítka vejde; první, které se nevejde, ho bere i všem méně důležitým */
            bool keep = true;
            for (int k = drop_count - 1; k >= 0; k--) {
                const int i = s_drop_order[k];
                if (!overlay_action_def(actions[i])->keycap) continue;
                if (full[i] > equal_w + 0.5f) keep = false;
                show_kc[i] = keep;
            }
        }

        float x = p.x, y = p.y;
        for (int i = 0; i < count; i++) {
            if (i > 0 && i % per_row == 0) {
                x = p.x;
                y += h + gap;
            }
            const en_OVL_ACTION id = actions[i];
            if (id == OVL_ACTION_RESET) {
                const float lx = floorf(x + sep_w * 0.5f - gap * 0.5f);
                dl->AddLine(ImVec2(lx, y + floorf(overlay_px(6.0f))), ImVec2(lx, y + h - floorf(overlay_px(6.0f))), OVL_COL_BORDER_STRONG, overlay_border_px());
                x += sep_w;
            }
            en_OVL_BUTTON_KIND kind = OVL_BUTTON_NORMAL;
            if (id == OVL_ACTION_RESUME) kind = OVL_BUTTON_PRIMARY;
            if (id == OVL_ACTION_RESET) kind = OVL_BUTTON_DANGER;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            /* skrytá zkratka jde do tooltipu; zakázaná akce štítek nekreslí, šířka tlačítka s místem pro něj zůstává */
            media_button_ex(id, v, (id == OVL_ACTION_SPEED) ? speed : NULL, false, true, kind, ImVec2(widths[i], h), names[i], show_kc[i]);
            x += widths[i] + gap;
        }
        media_finish_content(p.x, y + h);
    }
    media_card_end("card_actions");
}

/* ================================================================
 * Tab
 * ================================================================ */

void overlay_tab_media_draw(const st_OVERLAY_VIEW *v, float u)
{
    (void)u;
    if (!v) return;
    st_MEDIA_METRICS m;
    media_metrics(&m);
    const bool have_fdd = v->fdd[0].present;
    const bool have_qd = v->qd.present;
    const bool narrow = overlay_is_narrow() || !have_fdd;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = floorf(ImGui::GetContentRegionAvail().x);

    /* důvod zákazu: mají-li ho všechny karty médií stejný (replay), kreslí se jednou v pruhu nad kartami;
     * jinak ho má každá zakázaná karta v řádku nadpisu. Výšky karet na něm nezávisí (posudek R04). */
    const char *fdd_r = have_fdd ? overlay_action_reason(OVL_ACTION_FDD_INSERT_0, v) : NULL;
    const char *cmt_r = v->media_disabled_reason[0] ? v->media_disabled_reason : NULL;
    const char *qd_r = have_qd ? overlay_action_reason(v->qd.mounted ? OVL_ACTION_QD_EJECT : OVL_ACTION_QD_INSERT, v) : NULL;
    s_media_common_reason = cmt_r && *cmt_r && (!have_fdd || (fdd_r && strcmp(fdd_r, cmt_r) == 0)) && (!have_qd || (qd_r && strcmp(qd_r, cmt_r) == 0));
    if (s_media_common_reason) media_reason_strip(cmt_r, origin, w);
    float fdd_h = media_card_h(&m, (float)OVERLAY_VIEW_FDD_COUNT * m.fdd_row_h);
    float cmt_h = media_card_h(&m, media_cmt_content_h(&m));
    const float qd_h = media_card_h(&m, m.control_h);
    const float recent_h = media_card_h(&m, m.control_h);
    const float actions_h = 2.0f * m.pad + media_actions_content_h(overlay_is_narrow());

    float y = origin.y;
    if (!narrow) {
        /* dva sloupce se stejnou výškou: vyšší sloupec určuje výšku obou */
        const float right_h = cmt_h + (have_qd ? m.gap + qd_h : 0.0f);
        if (fdd_h < right_h) fdd_h = right_h;
        else cmt_h += fdd_h - right_h;
        const float left_w = floorf((w - m.gap) * MEDIA_LEFT_FRACTION);
        const float right_w = w - m.gap - left_w;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        media_card_fdd(v, &m, ImVec2(left_w, fdd_h));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + left_w + m.gap, y));
        media_card_cmt(v, &m, ImVec2(right_w, cmt_h));
        if (have_qd) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + left_w + m.gap, y + cmt_h + m.gap));
            media_card_qd(v, &m, ImVec2(right_w, qd_h));
        }
        y += fdd_h + m.gap;
    } else {
        if (have_fdd) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
            media_card_fdd(v, &m, ImVec2(w, fdd_h));
            y += fdd_h + m.gap;
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        media_card_cmt(v, &m, ImVec2(w, cmt_h));
        y += cmt_h + m.gap;
        if (have_qd) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
            media_card_qd(v, &m, ImVec2(w, qd_h));
            y += qd_h + m.gap;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    media_card_recent(v, &m, ImVec2(w, recent_h));
    y += recent_h + m.gap;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    media_card_actions(v, ImVec2(w, actions_h));
    media_finish_content(origin.x, y + actions_h);
}
