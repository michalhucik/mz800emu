/**
 * @file   cmt_convert_dialog.h
 * @brief  Modál "Convert to profile - block N" okna Virtual CMT: přestavba bloku 0x46 jiným profilem zapisovače.
 *
 * Seznam profilů knihovny (ID a anglický popis z registru, nepřekládá se;
 * aktuální profil bloku označený), výběr rychlosti jen pro profily, které
 * ji přijímají (cmt_view_read_profiles(), accepts_speed), tlačítka Convert
 * a Cancel. Convert zařadí cmt_request_convert_block_on_tape() a dialog
 * zavře; výsledek ohlásí řádek zpráv okna ("Block %d converted to %s.",
 * varování jádra o nepřenesených nastaveních místo toho, "Nothing to change.").
 *
 * Vlákna: dialog a jeho stav patří UI vláknu; jádro mění jen zásah fronty.
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

#ifndef CMT_CONVERT_DIALOG_H
#define CMT_CONVERT_DIALOG_H

#include <stdint.h>
#include <string>
#include <vector>

#include "cmt_view.h"
#include "device_request.h"
#include "emulator/hw-generic/cmt/cmt_request.h"


/**
 * @brief Otevře modál převodu bloku na jiný profil zapisovače.
 *
 * Zapamatuje si blok, jeho profil (výchozí výběr; profil mimo nabídku = nic
 * vybraného) a čítač výměn pásky;
 * nabídku profilů načte cmt_view_read_profiles(). Rychlost je výchozí
 * profilu (CMTSPEED_NONE). Otevřený dialog se nejdřív opustí. Modál se
 * otevře při nejbližším cmt_convert_dialog_draw() (až je volno).
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @param block_id Blok (index do v->blocks); volající ověřil, že jde převést
 *        (profile_convertible) a úpravy pásky jdou.
 * @pre UI vlákno uvnitř snímku ImGui.
 * @post cmt_convert_dialog_is_open() == true (při platném bloku).
 */
void cmt_convert_dialog_open(const st_CMT_VIEW *v, int block_id);

#ifdef MZTEST_HEADLESS
/** @brief Jen testy (MZTEST_HEADLESS): je modál převodu otevřený? @return true = ano. @pre UI vlákno. */
bool cmt_convert_dialog_is_open(void);
#endif

/**
 * @brief Nakreslí otevřený modál (jednou za snímek, z kreslení okna Virtual CMT).
 *
 * Klik nebo šipky v seznamu vyberou profil; výběr rychlosti je jen
 * u profilu, který rychlost přijímá (jinak text "Set by the profile"
 * a rychlost se neposílá). Convert (Enter) zařadí zásah a dialog zavře;
 * Cancel a Escape dialog opustí (Escape v otevřeném výběru rychlosti zavře
 * jen výběr). Convert je zakázané s důvodem: páska se
 * změnila, úpravy pásky nejdou (@p edit_reason), vybraný je aktuální profil
 * bloku s výchozí rychlostí ("Nothing to change."; Enter pak nic neudělá),
 * nic vybraného ("Select a profile.").
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @param edit_reason Důvod zákazu úprav pásky okna (přeložený; NULL = úpravy jdou).
 * @pre UI vlákno uvnitř okna Virtual CMT (Begin()), drží device_state_lock().
 * @post Zásah převodu je zařazený, nebo nic.
 */
void cmt_convert_dialog_draw(const st_CMT_VIEW *v, const char *edit_reason);

/** @brief Opustí dialog (zavřené okno Virtual CMT): jako Cancel. @pre UI vlákno. @post cmt_convert_dialog_is_open() == false. */
void cmt_convert_dialog_abandon(void);

/**
 * @brief Okno Virtual CMT v tomto snímku nekreslí obsah (Begin() vrátilo false, např. sbalené okno): modál převodu se nezavře.
 *
 * Označí dialog jako obsloužený v tomto snímku (cmt_convert_dialog_frame() ho pak neopustí
 * jako Cancel); výběr profilu a rychlosti i popup zůstane. Příští kreslení s obsahem okna pokračuje;
 * zavřelo-li ImGui mezitím popup, otevře ho znovu. Zavřené okno (ne bez
 * obsahu) dialog dál opouští (abandon, frame).
 *
 * @return true = dialog je otevřený (okno se pak rozbalí, aby skrytý modál neblokoval vstup); false = zavřený, nic.
 * @pre UI vlákno uvnitř snímku ImGui, místo kreslení dialogu (obsah okna se nekreslí).
 */
bool cmt_convert_dialog_skip_frame(void);

/**
 * @brief Údržba jednou za snímek i se zavřeným oknem (imgui_cmt_tape_dialogs()): dialog nenakreslený v tomto snímku opustí.
 *
 * Nenakreslený = okno Virtual CMT se nekreslilo vůbec (zavřené); snímek bez
 * obsahu okna ohlásí cmt_convert_dialog_skip_frame() a dialog zůstane.
 *
 * @pre UI vlákno, po kreslení okna Virtual CMT téhož snímku.
 */
void cmt_convert_dialog_frame(void);

/**
 * @brief Výsledek zásahu převodu (CMT_REQUEST_CONVERT_BLOCK_NAME; imgui_cmt_device_request_result()).
 *
 * Úspěch se změnou: "Block %d converted to %s." (číslo bloku od 1, ID
 * profilu); vrátilo-li jádro varování, ukáže se místo toho jako varování.
 * Úspěch beze změny: "Nothing to change." (informace). Jiná páska =
 * varování "The tape changed; the action was cancelled.", chyba jádra jeho
 * textem, odmítnutí při záznamu vstupů chybou. Texty jádra s čísly bloků
 * od 1 (cmt_view_core_text()). Po převodu se vybere převedený blok
 * (cmt_view_select_after_edit()).
 *
 * @param res Výsledek zásahu (nesmí být NULL).
 * @param edit Výsledek úpravy pásky téhož zásahu, NULL = není k dispozici.
 * @return true = zásah patřil převodu (obsloužen).
 * @pre UI vlákno.
 */
bool cmt_convert_dialog_request_result(const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit);


#ifdef MZTEST_HEADLESS
#include "cmt_window.h"

/** @brief Jen testy: prvky dialogu z posledního nakresleného snímku. @return Seznam. */
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_convert_dialog_test_items(void);

/**
 * @brief Jen testy: obdélník okna modálu a jeho vnitřek z posledního snímku.
 * @param[out] wmin Levý horní roh okna. @param[out] wmax Pravý dolní roh okna.
 * @param[out] imin Levý horní roh vnitřku. @param[out] imax Pravý dolní roh vnitřku.
 * @return true = modál se v posledním snímku kreslil.
 */
bool cmt_convert_dialog_test_rect(ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax);

/** @brief Jen testy: titulek modálu z posledního snímku (včetně "###cmt_convert"). @return Titulek. */
const char *cmt_convert_dialog_test_title(void);

/** @brief Jen testy: vybere profil podle ID, jako klik v seznamu (příští kreslení ho v seznamu odroluje do pohledu). @param id ID profilu. @return true = profil v nabídce je. */
bool cmt_convert_dialog_test_select(const char *id);

/** @brief Jen testy: nastaví rychlost, jako výběr v seznamu rychlostí. @param speed Rychlost (CMTSPEED_NONE = výchozí profilu). */
void cmt_convert_dialog_test_set_speed(en_CMTSPEED speed);

/** @brief Jen testy: dialog zpět do výchozího stavu (zavřený, zapomenuté čekající převody). */
void cmt_convert_dialog_test_reset(void);
#endif

#endif /* CMT_CONVERT_DIALOG_H */
