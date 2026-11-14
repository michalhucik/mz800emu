/**
 * @file   cmt_check_window.h
 * @brief  Nemodální okno "Tape check": nálezy kontroly vložené pásky.
 *
 * Okno otevírá menu Tape > Check tape okna Virtual CMT. Nálezy se čtou jen
 * při otevření (i opakovaném z menu, které okno přenese dopředu) a tlačítkem
 * Check again, nikdy každý snímek: cmt_view_read_tape_check() a jména bloků
 * cmt_view_read_block() (jen bloky s nálezem) pod zámkem stavu zařízení. Čtení je přímé (ne
 * zásah fronty), proto jde v každém stavu transportu i při záznamu
 * a přehrávání vstupů; pásku nemění.
 *
 * Nadpis je souhrn ("Blocks checked: %d. Warnings: %d." / "No problems
 * found. Blocks checked: %d."), řádky "Warning | Block N NAME | text"
 * (závažnost slovem, číslo bloku od 1 jako v tabulce okna, u řídicího bloku
 * jen číslo - druh bloku nese text; text jádra anglicky s čísly bloků od 1;
 * mezi sloupci 2 x 3u). Okno se otevře vedle okna kazety tak, aby nezakrylo
 * tabulku bloků (imgui_cmt_window_place_beside()). Klik na řádek vybere
 * blok v tabulce okna Virtual CMT a odroluje ho do pohledu
 * (imgui_cmt_window_select_block()); okno kazety dopředu nevynáší, okno
 * kontroly zůstane vidět. Změní-li se
 * páska (čítač výměn pásky, vysunutí), nálezy jsou zastaralé: ztlumené,
 * text "The tape changed. Run the check again.", klik nic nevybere.
 *
 * Vlákna: okno a jeho stav patří UI vláknu.
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

#ifndef CMT_CHECK_WINDOW_H
#define CMT_CHECK_WINDOW_H

#include <string>
#include <vector>


/**
 * @brief Otevře okno Tape check, nebo otevřené přenese dopředu, a vyžádá nové čtení nálezů.
 *
 * Nálezy přečte nejbližší imgui_cmt_check_window() (pod zámkem).
 *
 * @pre UI vlákno.
 * @post cmt_check_window_is_open() == true.
 */
void cmt_check_window_open(void);

#ifdef MZTEST_HEADLESS
/** @brief Jen testy (MZTEST_HEADLESS): je okno Tape check otevřené? @return true = ano. @pre UI vlákno. */
bool cmt_check_window_is_open(void);
#endif

/**
 * @brief Nakreslí okno Tape check, je-li otevřené (jednou za snímek, main_window.cpp po oknu Virtual CMT).
 *
 * Při vyžádaném čtení (otevření, Check again) přečte nálezy a jména bloků;
 * každý snímek přečte jen čítač výměn pásky a počet bloků
 * (cmt_view_read_tape_serial()) pro stav "zastaralé" (počet bloků kvůli
 * připojení záznamu, které čítač nezvedá). Křížek v titulku okno zavře.
 *
 * @pre UI vlákno uvnitř snímku ImGui, drží device_state_lock().
 * @post Nic v jádře se nemění.
 */
void imgui_cmt_check_window(void);


#ifdef MZTEST_HEADLESS
#include "cmt_window.h"

/** @brief Jen testy: prvky okna z posledního nakresleného snímku. @return Seznam. */
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_check_window_test_items(void);

/**
 * @brief Jen testy: obdélník okna a jeho vnitřek z posledního snímku.
 * @param[out] wmin Levý horní roh okna. @param[out] wmax Pravý dolní roh okna.
 * @param[out] imin Levý horní roh vnitřku. @param[out] imax Pravý dolní roh vnitřku.
 * @return true = okno se v posledním snímku kreslilo.
 */
bool cmt_check_window_test_rect(ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax);

/** @brief Jen testy: kolikrát okno přečetlo nálezy (cmt_view_read_tape_check()). @return Počet. */
int cmt_check_window_test_reads(void);

/** @brief Jen testy: okno zavřené, nálezy zapomenuté, čítač čtení na 0. */
void cmt_check_window_test_reset(void);
#endif

#endif /* CMT_CHECK_WINDOW_H */
