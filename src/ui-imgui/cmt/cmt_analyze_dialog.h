/**
 * @file   cmt_analyze_dialog.h
 * @brief  Dialog "Analyze recording - block N" okna Virtual CMT: analýza surového bloku na soubory.
 *
 * Dialog vezme surový blok 0x18 (záznam, který se nedekódoval, nebo blok
 * z importu WAV), nechá jádro zkopírovat jeho pulzy (zásah fronty
 * cmt_request_analysis_pulses()), kopii rozebere v pracovním vlákně
 * (cmttmz_analyze_pulses()) a nalezené soubory ukáže v tabulce
 * se zaškrtnutím. Výsledek uplatní zásahem cmt_request_analysis_apply():
 * Add files after recording (surový blok zůstane, soubory za něj) nebo
 * Replace recording (surový blok nahradí bloky zaškrtnutých souborů
 * a surové bloky všeho ostatního). Návrh: návrh okna (SPEC), koncepty
 * cmt-analyze-v2 a cmt-analyze-empty-v2.
 *
 * Vlákna:
 * - Dialog, jeho stav a zásahy patří UI vláknu. Jádro dialog nečte přímo:
 *   údaje bloku bere z kopie stavu okna (st_CMT_VIEW), pulzy z kopie, kterou
 *   vyrobí emu vlákno.
 * - Analýza běží v jednom pracovním vlákně GLib (nejvýš jedna najednou)
 *   nad kopií pulzů; UI vlákno každý snímek čte příznak hotovo (vzor
 *   version_check). Úloha a kopie pulzů mají čítače odkazů: Cancel a zavření
 *   okna úlohu jen opustí, vlákno doběhne a uvolní všechno samo (výsledek
 *   se zahodí). Doběhlá vlákna se připojí (g_thread_join()); ukončení
 *   aplikace na běžící vlákno čeká nejvýš 2 s, pak ho odpojí. Vlákno nesahá
 *   na nic z UI. Nejdelší pulzy (osa histogramu) spočítá pracovní vlákno;
 *   UI vlákno staví histogram jen ze vzorku nejvýš
 *   CMTTMZ_ANALYZE_HIST_MAX_PULSES pulzů; histogram se počítá v kreslení
 *   dialogu, tedy pod device_state_lock() okna (změřeno nejvýš 4 ms na
 *   obřím bloku, ladicí build).
 * - Data předaná zásahu uplatnění (kopie pulzů, analýza, výběr řádků)
 *   drží modul (ne dialog, který se po Add / Replace zavře), dokud nepřijde
 *   výsledek zásahu, dokud fronta zásahů neohlásí, že ho už neprovede
 *   (prázdná fronta bez výsledku = ztracený zásah), nebo do ukončení
 *   aplikace (cmt_analyze_dialog_shutdown(), emu vlákno už připojené).
 * - Modál se otevírá jen při volnu (imgui_modal_open_when_free()); opuštění
 *   mimo modál (zavřené okno) popup v zásobníku ImGui zavře.
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

#ifndef CMT_ANALYZE_DIALOG_H
#define CMT_ANALYZE_DIALOG_H

#include <stdint.h>
#include <string>
#include <vector>

#include "cmt_view.h"
#include "device_request.h"
#include "emulator/hw-generic/cmt/cmt_request.h"


/**
 * @brief Otevře dialog analýzy surového bloku a zařadí kopii jeho pulzů.
 *
 * Zapamatuje si blok, čítač výměn pásky a údaje pro souhrn z kopie stavu,
 * zařadí cmt_request_analysis_pulses() a otevře modál ve stavu "běží"
 * (Analyzing...). Analýza začne, až přijde výsledek zásahu
 * (cmt_analyze_dialog_request_result()). Volby jsou výchozí (Leader
 * tolerance 20 %, opravy vypnuté). Otevřený dialog nejdřív opustí.
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @param block_id Blok (index do v->blocks); volající ověřil, že je to
 *        surový blok (CMT_VIEW_KIND_RAW) a úpravy pásky jdou.
 * @pre UI vlákno uvnitř snímku ImGui (z kreslení okna Virtual CMT).
 * @post cmt_analyze_dialog_is_open() == true; modál se otevře při nejbližším
 *       cmt_analyze_dialog_draw().
 */
void cmt_analyze_dialog_open ( const st_CMT_VIEW *v, int block_id );

#ifdef MZTEST_HEADLESS
/** @brief Jen testy (MZTEST_HEADLESS): je dialog analýzy otevřený (i čekající na kopii pulzů nebo analýzu)? @return true = ano. @pre UI vlákno. */
bool cmt_analyze_dialog_is_open ( void );
#endif

/**
 * @brief Nakreslí otevřený dialog analýzy (jednou za snímek, z kreslení okna Virtual CMT).
 *
 * Převezme hotovou analýzu z pracovního vlákna, spustí čekající analýzu
 * (předchozí opuštěné vlákno doběhlo) a nakreslí modál. Tlačítka Add files
 * after recording / Replace recording zařadí cmt_request_analysis_apply()
 * a dialog zavřou; Cancel, Escape a zavření modálu jinak dialog opustí.
 * Akce jsou zakázané s důvodem v tooltipu: páska se změnila (jiný čítač
 * výměn pásky; dialog zůstane), analýza běží, chyba analýzy, zastaralé
 * výsledky (jiné volby než u výsledku), důvod úprav pásky @p edit_reason,
 * nic zaškrtnutého.
 *
 * @param v Kopie stavu okna (nesmí být NULL).
 * @param edit_reason Důvod zákazu úprav pásky okna (přeložený; NULL = úpravy jdou).
 * @pre UI vlákno uvnitř okna Virtual CMT (Begin()), drží device_state_lock().
 * @post Zásah uplatnění je zařazený, nebo nic; jádro se přímo nemění.
 */
void cmt_analyze_dialog_draw ( const st_CMT_VIEW *v, const char *edit_reason );

/**
 * @brief Analýza surového bloku na pozadí, zatímco hraje (každý snímek z kreslení okna Virtual CMT).
 *
 * Když páska hraje (v každém režimu načítání; ne při záznamu ani
 * přehrávání vstupů) surový blok, který ještě analyzovaný nebyl, zařadí
 * kopii jeho pulzů (cmt_request_analysis_pulses()) a spustí nad ní analýzu
 * s výchozími volbami v pracovním vlákně (nejvýš jedna analýza najednou;
 * otevřený dialog má přednost a nic nového se pak nespouští). Jakmile blok
 * dohraje nebo se páska zastaví a výsledek má aspoň jeden soubor, který
 * jde přidat, ohlásí to jednou v řádku zpráv okna (cmt_view_post(),
 * informace). cmt_analyze_dialog_open() pro týž blok hotový výsledek nebo
 * rozběhnutou úlohu převezme. Jiná páska (čítač výměn) nebo vypnutá volba
 * rozpracovanou analýzu zahodí; selhání kopie nebo analýzy se nehlásí.
 * Pásku nemění.
 *
 * @param v Kopie stavu okna (NULL = nic).
 * @param enabled Volba "Find files while playing" (false = nic se nespouští a nedrží).
 * @pre UI vlákno uvnitř snímku ImGui.
 */
void cmt_analyze_dialog_auto ( const st_CMT_VIEW *v, bool enabled );

/**
 * @brief Opustí dialog analýzy (zavřené okno Virtual CMT): jako Cancel.
 *
 * Běžící analýza se opustí (vlákno doběhne a uvolní data samo), kopie
 * pulzů a výsledek se uvolní; čekající zásah uplatnění zůstává.
 *
 * @pre UI vlákno.
 * @post cmt_analyze_dialog_is_open() == false.
 */
void cmt_analyze_dialog_abandon ( void );

/**
 * @brief Okno Virtual CMT v tomto snímku nekreslí obsah (Begin() vrátilo false, např. sbalené okno): dialog analýzy se nezavře.
 *
 * Označí dialog jako obsloužený v tomto snímku (cmt_analyze_dialog_frame() ho pak neopustí
 * jako Cancel); stav (kopie pulzů, výsledek, výběr řádků, volby) i popup; převezme hotovou analýzu nebo spustí čekající (jako kreslení) zůstane. Příští kreslení s obsahem okna pokračuje;
 * zavřelo-li ImGui mezitím popup, otevře ho znovu. Zavřené okno (ne bez
 * obsahu) dialog dál opouští (abandon, frame).
 *
 * @return true = dialog je otevřený (okno se pak rozbalí, aby skrytý modál neblokoval vstup); false = zavřený, nic.
 * @pre UI vlákno uvnitř snímku ImGui, místo kreslení dialogu (obsah okna se nekreslí).
 */
bool cmt_analyze_dialog_skip_frame ( void );

/**
 * @brief Údržba dialogu jednou za snímek i se zavřeným oknem (imgui_cmt_tape_dialogs()).
 *
 * Dialog, který okno Virtual CMT v tomto snímku nenakreslilo (okno se
 * zavřelo; snímek bez obsahu okna ohlásí cmt_analyze_dialog_skip_frame()
 * a dialog zůstane), opustí. Uvolní data zásahu uplatnění, jehož výsledek se
 * ztratil: fronta zásahů je prázdná (device_request_pending() == 0)
 * a výsledek nepřišel ani ve vyřizování výsledků snímku (hlídač bez čekání
 * na čas, vzor postupného importu).
 *
 * @pre UI vlákno, po kreslení okna Virtual CMT téhož snímku.
 */
void cmt_analyze_dialog_frame ( void );

/**
 * @brief Výsledek zásahu kopie pulzů nebo uplatnění analýzy (imgui_cmt_device_request_result()).
 *
 * Kopie pulzů (CMT_REQUEST_ANALYSIS_PULSES_NAME): vyzvedne kopii
 * (cmt_request_take_pulses()); patří-li otevřenému dialogu, spustí
 * analýzu, jinak kopii uvolní. Chyba kopie, jiná páska nebo odmítnutí při
 * záznamu vstupů = stav chyby dialogu s textem.
 *
 * Uplatnění (CMT_REQUEST_ANALYSIS_APPLY_NAME): uvolní data zásahu
 * a ohlásí výsledek v řádku zpráv: "Files added from the recording: %d"
 * (s varováním jádra v závorce) a výběr prvního přidaného bloku
 * (cmt_view_select_after_edit()); úspěch bez výsledku úpravy "Files added
 * from the recording." bez výběru; chyba jádra jeho textem, jiná páska
 * varováním, odmítnutí při záznamu vstupů chybou. Texty jádra (chyba kopie
 * i uplatnění) se ukazují s čísly bloků a řádků od 1 jako v okně (jádro
 * čísluje od 0).
 *
 * @param res Výsledek zásahu (nesmí být NULL).
 * @param edit Výsledek úpravy pásky téhož zásahu (cmt_request_get_edit_result()), NULL = není k dispozici.
 * @return true = zásah patřil dialogu analýzy (obsloužen); false = jiný zásah.
 * @pre UI vlákno; nedrží device_state_lock() (vyzvednutí kopie má vlastní zámek modulu).
 */
bool cmt_analyze_dialog_request_result ( const st_DEVICE_REQ_RESULT *res, const st_CMT_REQUEST_EDIT_RESULT *edit );

/**
 * @brief Úklid při ukončení aplikace (sdlapp_myimgui_video_exit()).
 *
 * Opustí dialog. Běžící analýzu nejde zrušit: počká na ni nejvýš
 * CAD_EXIT_JOIN_MS (2 s) a vlákno připojí (g_thread_join()); po mezi
 * vlákno odpojí - doběhne nad vlastní kopií pulzů, uvolní ji, na UI nesahá.
 *
 * Data čekajících zásahů uplatnění uvolní bezpodmínečně. Pořadí ukončení
 * aplikace: emu vlákno v emulator_exit_cleanup() zavolá cmt_exit() ->
 * cmt_request_exit() (registr zásahů uplatnění vymaže, zásahy, které by ještě
 * proběhly, data nenajdou), main.c pak emu vlákno připojí (g_thread_join())
 * a teprve potom iface_exit() zavolá tuto funkci. Zrušení zásahu
 * (cmt_request_analysis_cancel()) je tu proto no-op a emu vlákno už žádný
 * zásah neprovede.
 *
 * @pre UI vlákno, před zrušením kontextu ImGui; emu vlákno je připojené
 *      a registr zásahů vymazal cmt_request_exit() (pořadí main.c).
 * @post Žádná data zásahů uplatnění; s_cad_threads prázdné.
 */
void cmt_analyze_dialog_shutdown ( void );

/**
 * @brief Zpráva řádku zpráv po přidání souborů: "Files added from the recording: %d" (přeložená).
 *
 * Bez množného čísla (projekt nemá ngettext): štítek s počtem
 * za dvojtečkou platí pro každý počet.
 *
 * @param count Počet přidaných bloků souborů; záporný = počet neznámý
 *        (chybí výsledek úpravy) - pak "Files added from the recording.".
 * @return Text.
 * @pre UI vlákno.
 */
std::string cmt_analyze_dialog_added_message ( int count );


/**
 * @brief Funkce analýzy, kterou volá pracovní vlákno v testech (cmt_analyze_dialog_test_set_analyzer(); aplikace volá přímo cmttmz_analyze_pulses()).
 *
 * Kontrakt jako cmttmz_analyze_pulses(): čte jen kopii pulzů a volby, výsledek
 * alokuje sama a smí běžet v jiném vlákně než UI a emulátor.
 */
typedef st_CMTTMZ_ANALYSIS *( *cmt_analyze_dialog_analyzer_fn ) ( const st_CMTTMZ_PULSES *p, const st_CMTTMZ_ANALYZE_OPTS *opts );


#ifdef MZTEST_HEADLESS
#include "cmt_window.h"

/**
 * @brief Jen testy: řízené "vlákno" analýzy.
 * @param manual true = úloha se nespustí ve vlákně, čeká ve frontě testu na cmt_analyze_dialog_test_run_jobs(); false = vlákno GLib.
 */
void cmt_analyze_dialog_test_set_manual_runner ( bool manual );

/**
 * @brief Jen testy: provede čekající úlohy synchronně ve vlákně testu (tělo pracovního vlákna).
 * @return Počet provedených úloh.
 */
int cmt_analyze_dialog_test_run_jobs ( void );

/** @brief Jen testy: počet úloh, které čekají ve frontě testu. @return Počet. */
int cmt_analyze_dialog_test_queued_jobs ( void );

/** @brief Jen testy: nahradí funkci analýzy (NULL = cmttmz_analyze_pulses()). @param fn Funkce. */
void cmt_analyze_dialog_test_set_analyzer ( cmt_analyze_dialog_analyzer_fn fn );

/** @brief Jen testy: počet živých úloh analýzy (alokovaných, dosud neuvolněných). @return Počet. */
int cmt_analyze_dialog_test_live_jobs ( void );

/** @brief Jen testy: počet živých kopií pulzů dialogu (držák s čítačem odkazů). @return Počet. */
int cmt_analyze_dialog_test_live_pulses ( void );

/** @brief Jen testy: počet zásahů uplatnění, jejichž data dialog drží. @return Počet. */
int cmt_analyze_dialog_test_pending_applies ( void );

/** @brief Jen testy: prvky dialogu z posledního nakresleného snímku (všechny s popup = true). @return Seznam. */
const std::vector<st_CMT_WINDOW_TEST_ITEM> &cmt_analyze_dialog_test_items ( void );

/**
 * @brief Jen testy: obdélník okna modálu a jeho vnitřek z posledního snímku.
 * @param[out] wmin Levý horní roh okna. @param[out] wmax Pravý dolní roh okna.
 * @param[out] imin Levý horní roh vnitřku (bez titulku). @param[out] imax Pravý dolní roh vnitřku.
 * @return true = modál se v posledním snímku kreslil.
 */
bool cmt_analyze_dialog_test_rect ( ImVec2 *wmin, ImVec2 *wmax, ImVec2 *imin, ImVec2 *imax );

/** @brief Jen testy: titulek modálu z posledního snímku (včetně "###cmt_analyze"). @return Titulek. */
const char *cmt_analyze_dialog_test_title ( void );

/** @brief Jen testy: vybere řádek tabulky výsledků (-1 = bez výběru), jako klik. @param row Index řádku. */
void cmt_analyze_dialog_test_select_row ( int row );

/** @brief Jen testy: rozbalí nebo sbalí oddíl Signal details. @param open true = rozbalit. */
void cmt_analyze_dialog_test_set_details ( bool open );

/** @brief Jen testy: volby, se kterými se spustila poslední úloha analýzy. @return Volby. */
st_CMTTMZ_ANALYZE_OPTS cmt_analyze_dialog_test_last_opts ( void );

/**
 * @brief Jen testy: ořez posouvaného těla dialogu z posledního snímku.
 * @param[out] min Levý horní roh. @param[out] max Pravý dolní roh. @param[out] scroll Tělo mělo svislý posuvník.
 * @return true = modál se v posledním snímku kreslil.
 */
bool cmt_analyze_dialog_test_body_rect ( ImVec2 *min, ImVec2 *max, bool *scroll );

/**
 * @brief Jen testy: tabulka výsledků z posledního snímku (stálé ID tabulky, nejvýš jeden svislý posuvník).
 * @param[out] scrollbar Tabulka měla vlastní svislý posuvník (smí být NULL).
 * @return ID tabulky ImGui (0 = tabulka se nekreslila).
 */
ImGuiID cmt_analyze_dialog_test_table ( bool *scrollbar );

/** @brief Jen testy: mez čekání na vlákno analýzy při ukončení v ms (výchozí 2000). @param ms Mez. */
void cmt_analyze_dialog_test_set_exit_join_ms ( int ms );

/** @brief Jen testy: počet průchodů celou kopií pulzů pro nejdelší pulz (pracovní vlákno; jednou za kopii). @return Počet. */
int cmt_analyze_dialog_test_max_passes ( void );

/** @brief Jen testy: počet vláken analýzy, která se ještě nepřipojila. @return Počet. */
int cmt_analyze_dialog_test_threads ( void );

/**
 * @brief Jen testy: otevře dialog nad danou kopií pulzů (bez zásahu kopie; měření obřího bloku).
 *
 * Jako cmt_analyze_dialog_open(), ale kopii převezme (uvolní ji dialog,
 * cmttmz_pulses_free()) a hned zařadí analýzu; zásah kopie zařazený
 * otevřením se nepoužije.
 *
 * @param v Kopie stavu okna. @param block_id Blok (titulek, souhrn). @param p Kopie pulzů (malloc jako jádro; převezme ji dialog).
 */
void cmt_analyze_dialog_test_open_with_pulses ( const st_CMT_VIEW *v, int block_id, st_CMTTMZ_PULSES *p );

/** @brief Jen testy: dialog zpět do výchozího stavu (opustí dialog, zahodí frontu testu, analyzátor a ruční běh vrátí). */
void cmt_analyze_dialog_test_reset ( void );
#endif

#endif /* CMT_ANALYZE_DIALOG_H */
