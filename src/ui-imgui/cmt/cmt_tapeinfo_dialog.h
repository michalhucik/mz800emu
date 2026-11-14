/**
 * @file   cmt_tapeinfo_dialog.h
 * @brief  Modál "Tape information - <jméno>" okna Virtual CMT: informace o pásce (Archive Info 0x32, Machine Info 0x43).
 *
 * Formulář deseti textů bloku Archive Info (Title, Publisher, Author, Year,
 * Language, Type, Price, Protection, Origin, Comment - Comment víceřádkový)
 * a výběr Target machine (Not specified, MZ-700, MZ-800, MZ-1500, MZ-80B).
 * Hodnoty se čtou jednou při otevření (cmt_view_read_tape_info(), pod
 * zámkem stavu zařízení); OK pošle zásah cmt_request_tape_info_set() jen
 * tehdy, když se něco změnilo, jinak se chová jako Cancel. Dialog čeká na
 * výsledek zásahu: úspěch ho zavře (varování jádra, např. znaky nahrazené
 * '?', jde do řádku zpráv okna), odmítnutí kvůli změně pásky a chyba jádra
 * ho nechají otevřený s úpravami uživatele a zprávou v dialogu; po změně
 * pásky Reload přečte informace znovu a ponechá pole, která uživatel změnil.
 * Návrh: návrh okna (SPEC), část Informace o pásce.
 *
 * Vlákna: dialog i jeho stav patří UI vláknu; jádro mění jen zásah fronty.
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

#ifndef CMT_TAPEINFO_DIALOG_H
#define CMT_TAPEINFO_DIALOG_H

#include <stdint.h>
#include <string>
#include <vector>

#include "cmt_view.h"
#include "device_request.h"
#include "emulator/hw-generic/cmt/cmt_request.h"


/**
 * @brief Otevře modál Tape information nad vloženou páskou.
 *
 * Přečte informace o pásce (cmt_view_read_tape_info()) a zapamatuje si je
 * jako výchozí stav formuláře i jako srovnání pro "změnilo se něco";
 * modál se otevře při nejbližším cmt_tapeinfo_dialog_draw() (až je volno,
 * imgui_modal_open_when_free()). Otevřený dialog se nejdřív opustí.
 *
 * @param v Kopie stavu okna (nesmí být NULL; jméno pásky pro titulek).
 * @pre UI vlákno drží device_state_lock(); volající ověřil, že úpravy pásky
 *      jdou (páska vložená, STOP, mimo záznam a přehrávání vstupů).
 * @post Při vložené pásce cmt_tapeinfo_dialog_is_open() == true; bez pásky nic.
 */
void cmt_tapeinfo_dialog_open(const st_CMT_VIEW *v);

#ifdef MZTEST_HEADLESS
/** @brief Jen testy (MZTEST_HEADLESS): je modál Tape information otevřený (i čekající na výsledek zápisu)? @return true = ano. @pre UI vlákno. */
bool cmt_tapeinfo_dialog_is_open(void);
#endif

/**
 * @brief Nakreslí otevřený modál (jednou za snímek, z kreslení okna Virtual CMT).
 *
 * OK zařadí zápis jen při změně formuláře a čeká na výsledek; beze změny
 * dialog zavře (jako Cancel). Enter v jednořádkovém poli = OK, v poli
 * Comment nový řádek; Escape bez aktivního pole = Cancel (bez dotazu i při
 * neuložených úpravách; Escape v otevřeném výběru Target machine zavře jen
 * výběr). OK je zakázané s důvodem, když se páska od čtení změnila (jiný
 * čítač výměn pásky, odmítnutý zápis; úpravy zůstanou a tlačítko Reload
 * přečte pásku znovu a ponechá pole, která uživatel změnil), úpravy pásky
 * nejdou (@p edit_reason) nebo zápis čeká na výsledek. Po úspěšném zápisu se
 * v okně kazety vybere blok vybraný při otevření na svém novém čísle.
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @param edit_reason Důvod zákazu úprav pásky okna (přeložený; NULL = úpravy jdou).
 * @pre UI vlákno uvnitř okna Virtual CMT (Begin()), drží device_state_lock().
 * @post Zásah zápisu je zařazený, nebo nic; jádro se přímo nemění.
 */
void cmt_tapeinfo_dialog_draw(const st_CMT_VIEW *v, const char *edit_reason);

/**
 * @brief Opustí dialog (zavřené okno Virtual CMT): jako Cancel.
 *
 * Čekající zásah zápisu zůstane ve frontě; jeho výsledek ohlásí řádek
 * zpráv (cmt_tapeinfo_dialog_request_result()).
 *
 * @pre UI vlákno.
 * @post cmt_tapeinfo_dialog_is_open() == false.
 */
void cmt_tapeinfo_dialog_abandon(void);

/**
 * @brief Okno Virtual CMT v tomto snímku nekreslí obsah (Begin() vrátilo false, např. sbalené okno): modál Tape information se nezavře.
 *
 * Označí dialog jako obsloužený v tomto snímku (cmt_tapeinfo_dialog_frame() ho pak neopustí
 * jako Cancel); formulář s úpravami, čekající zápis i popup zůstane. Příští kreslení s obsahem okna pokračuje;
 * zavřelo-li ImGui mezitím popup, otevře ho znovu. Zavřené okno (ne bez
 * obsahu) dialog dál opouští (abandon, frame).
 *
 * @return true = dialog je otevřený (okno se pak rozbalí, aby skrytý modál neblokoval vstup); false = zavřený, nic.
 * @pre UI vlákno uvnitř snímku ImGui, místo kreslení dialogu (obsah okna se nekreslí).
 */
bool cmt_tapeinfo_dialog_skip_frame(void);

/**
 * @brief Údržba jednou za snímek i se zavřeným oknem (imgui_cmt_tape_dialogs()): dialog nenakreslený v tomto snímku opustí.
 *
 * Nenakreslený = okno Virtual CMT se nekreslilo vůbec (zavřené); snímek bez
 * obsahu okna ohlásí cmt_tapeinfo_dialog_skip_frame() a dialog zůstane.
 *
 * @pre UI vlákno, po kreslení okna Virtual CMT téhož snímku.
 */
void cmt_tapeinfo_dialog_frame(void);

/**
 * @brief Výsledek zásahu zápisu informací (CMT_REQUEST_TAPE_INFO_SET_NAME; imgui_cmt_device_request_result()).
 *
 * Výsledek otevřeného dialogu (stejné pořadové číslo zásahu): úspěch dialog
 * zavře a ohlásí "Tape information updated." (beze změny pásky nic),
 * varování jádra ohlásí místo toho jako varování; jiná páska nechá dialog
 * otevřený s výzvou k Reload; chyba jádra a odmítnutí při záznamu
 * vstupů nechají dialog otevřený s textem chyby. Výsledek zásahu, jehož
 * dialog se mezitím zavřel, jde do řádku zpráv okna (cmt_view_post()).
 *
 * @param res Výsledek zásahu (nesmí být NULL).
 * @param edit Výsledek úpravy pásky téhož zásahu, NULL = není k dispozici.
 * @return true = zásah patřil zápisu informací (obsloužen).
 * @pre UI vlákno.
 */
bool cmt_tapeinfo_dialog_request_result(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit);


#ifdef MZTEST_HEADLESS
#include "cmt_window.h"

/** @brief Jen testy: prvky dialogu z posledního nakresleného snímku. @return Seznam. */
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_tapeinfo_dialog_test_items(void);

/**
 * @brief Jen testy: obdélník okna modálu a jeho vnitřek z posledního snímku.
 * @param[out] wmin Levý horní roh okna. @param[out] wmax Pravý dolní roh okna.
 * @param[out] imin Levý horní roh vnitřku. @param[out] imax Pravý dolní roh vnitřku.
 * @return true = modál se v posledním snímku kreslil.
 */
bool cmt_tapeinfo_dialog_test_rect(ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax);

/** @brief Jen testy: titulek modálu z posledního snímku (včetně "###cmt_tapeinfo"). @return Titulek. */
const char *cmt_tapeinfo_dialog_test_title(void);

/**
 * @brief Jen testy: přepíše text pole formuláře, jako by ho uživatel napsal.
 * @param field Index 0 .. CMTTMZ_TAPE_INFO_FIELDS - 1 (pořadí st_CMTTMZ_TAPE_INFO). @param text Text (UTF-8).
 */
void cmt_tapeinfo_dialog_test_set_text(int field, const char *text);

/**
 * @brief Jen testy: vybere položku Target machine, jako by ji uživatel zvolil.
 * @param machine -1 = Not specified, jinak en_TMZ_MACHINE (1 .. 4).
 */
void cmt_tapeinfo_dialog_test_set_machine(int machine);

/** @brief Jen testy: dialog zpět do výchozího stavu (zavřený, bez čekajícího zásahu). */
void cmt_tapeinfo_dialog_test_reset(void);
#endif

#endif /* CMT_TAPEINFO_DIALOG_H */
