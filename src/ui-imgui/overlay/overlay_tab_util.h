/**
 * @file   overlay_tab_util.h
 * @brief  Společné stavební prvky tabů Sound, Input a Capture overlay menu.
 *
 * Karta s přeloženým nadpisem, řádek s přepínačem akce, tlačítko akce s
 * vlastní ikonou, vedlejší text zalomený na šířku karty a uzavření ručně
 * rozmístěného obsahu. Prvky kreslí jen z kopie stavu a akce jen vyžádají
 * (overlay_actions.h). Texty dostávají už přeložené.
 *
 * @par Vlákna
 * Jen UI vlákno.
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

#ifndef OVERLAY_TAB_UTIL_H
#define OVERLAY_TAB_UTIL_H

#include "libs/imgui/imgui.h"

#include "ui-imgui/overlay/overlay_actions.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/overlay/overlay_widgets.h"

/** @brief Mezera mezi kartami tabu v návrhových bodech (overlay_px()). */
#define OVL_TAB_CARD_GAP 12.0f
/** @brief Mezera mezi prvky uvnitř karty v návrhových bodech (overlay_px()). */
#define OVL_TAB_GAP 8.0f
/** @brief Podíl šířky řádku pro sloupec popisku u řádku s přepínačem (ovl_tab_toggle_row()). */
#define OVL_TAB_TOGGLE_LABEL_FRACTION 0.72f
/** @brief Nejmenší podíl šířky řádku pro sloupec popisku, na který ovl_tab_toggle_row() ustoupí štítku zkratky. */
#define OVL_TAB_TOGGLE_LABEL_MIN_FRACTION 0.40f

/**
 * @brief Začne kartu tabu s přeloženým nadpisem.
 * @param title Přeložený nadpis.
 * @param id    Stabilní ID karty (bez "###").
 * @param size  Velikost karty v pixelech.
 * @return true = karta je vidět.
 * @post Musí následovat ovl_card_end().
 */
bool ovl_tab_card_begin(const char *title, const char *id, ImVec2 size);

/**
 * @brief Uzavře ručně rozmístěný obsah položkou, aby kurzor nezůstal za hranicí obsahu.
 * @param x Levý okraj obsahu.
 * @param y Spodní okraj obsahu.
 */
void ovl_tab_finish_content(float x, float y);

/**
 * @brief Řádek s přepínačem u pravého okraje, který přepíná akci z tabulky akcí.
 *
 * Štítek zkratky akce se kreslí vpravo od přepínače, pokud se do řádku
 * vejde; sloupec popisku mu ustoupí až na OVL_TAB_TOGGLE_LABEL_MIN_FRACTION
 * šířky řádku (popisek se pak zkracuje výpustkou). Nevejde-li se štítek ani
 * tak, je zkratka v tooltipu. Nedostupná akce kreslí přepínač
 * zakázaný s důvodem v tooltipu.
 *
 * @param label     Přeložený popisek řádku.
 * @param id        Řetězec pro ID přepínače.
 * @param on        Současný stav z kopie.
 * @param action    Akce, kterou přepnutí vyžádá (overlay_action_request()).
 * @param v         Kopie stavu (dostupnost a štítek zkratky akce).
 * @param test_name Jméno obdélníku přepínače pro testy (štítek zkratky má jméno s příponou "_keycap").
 * @param focus     true = přepínač je výchozí prvek tabu (dostane fokus, když ho rámec žádá).
 * @pre Uvnitř karty; kurzor okna stojí na začátku řádku.
 */
void ovl_tab_toggle_row(const char *label, const char *id, bool on, en_OVL_ACTION action, const st_OVERLAY_VIEW *v, const char *test_name, bool focus);

/**
 * @brief Tlačítko akce z tabulky akcí s vlastní ikonou.
 *
 * Jako overlay_action_button(), jen ikonu určuje volající (tlačítko, jehož
 * ikona závisí na stavu). Zkratku bere z tabulky; nedostupnou akci kreslí
 * jako zakázanou a v tooltipu ukáže důvod.
 *
 * @param id        Akce.
 * @param v         Kopie stavu.
 * @param text      Přeložený popisek.
 * @param icon      Ikona (ICON_LC_*); NULL = bez ikony.
 * @param kind      Druh tlačítka.
 * @param size      Velikost jako u ovl_button_ex().
 * @param test_name Jméno obdélníku pro testy; NULL = nezaznamenávat.
 * @return true = tlačítko bylo zvoleno a akce vyžádána.
 * @pre Uvnitř okna overlaye se stylem overlay_style_push(); kurzor okna stojí na pozici tlačítka.
 */
bool ovl_tab_action_button(en_OVL_ACTION id, const st_OVERLAY_VIEW *v, const char *text, const char *icon, en_OVL_BUTTON_KIND kind, ImVec2 size, const char *test_name);

/**
 * @brief Výška vedlejšího textu (písmo nápovědy) zalomeného na danou šířku.
 * @param text   Přeložený text; NULL nebo prázdný = 0.
 * @param wrap_w Šířka, na kterou se text zalamuje, v pixelech.
 * @return Výška v celých pixelech.
 */
float ovl_tab_note_height(const char *text, float wrap_w);

/**
 * @brief Vykreslí vedlejší text (písmo nápovědy, vedlejší barva) zalomený na danou šířku.
 * @param text      Přeložený text; NULL nebo prázdný = nic.
 * @param x         Levý okraj (souřadnice obrazovky).
 * @param y         Horní okraj.
 * @param wrap_w    Šířka, na kterou se text zalamuje, v pixelech.
 * @param test_name Jméno obdélníku textu pro testy; NULL = nezaznamenávat.
 * @return Výška textu v pixelech (jako ovl_tab_note_height()).
 * @pre Uvnitř karty.
 */
float ovl_tab_note(const char *text, float x, float y, float wrap_w, const char *test_name);

#endif /* OVERLAY_TAB_UTIL_H */
