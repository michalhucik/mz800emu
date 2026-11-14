/**
 * @file device_request_ui.h
 * @brief GUI strana zásahů do zařízení: oznámení odmítnutých zásahů.
 *
 * Menu a okna zařízení zásahy samy neprovádějí, zařazují je do fronty
 * emu vlákna (emulator/device_request.h). imgui_device_request_results()
 * každý snímek vyzvedne výsledky a odmítnutí kvůli běžícímu záznamu nebo
 * přehrávání vstupů ohlásí uživateli. Chyby provedených zásahů hlásí
 * zařízení sama (baseui_show_message()).
 */

#ifndef DEVICE_REQUEST_UI_H
#define DEVICE_REQUEST_UI_H

#include <stdbool.h>

#include "device_request.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Oznámí výsledek reinicializace runtime Unicardu.
 *
 * Definice v menu_unicard.cpp.
 *
 * @param res Výsledek zásahu.
 * @return true = výsledek patřil Unicardu a byl ohlášen.
 * @pre UI vlákno.
 */
bool imgui_unicard_device_request_result(const st_DEVICE_REQ_RESULT *res);

/**
 * @brief Oznámí výsledek zásahu kazety (uložení, export, otevření, rychlost, režim načítání).
 *
 * Definice v cmt/cmt_tape_actions.cpp. Zprávy (i odmítnutí kvůli replay)
 * jdou přes cmt_view_post(): při viditelném okně Virtual CMT do řádku
 * zpráv, jinak chyba modálně a informace se zahodí. Uložení z dialogu
 * neuložených změn po úspěchu pokračuje čekající akcí automatu dotazu
 * (vysunutí, otevření, nová páska, načtení snapshotu, Quick Load,
 * přehrání záznamu vstupů, ukončení; další krok stav pásky přečte znovu),
 * po neúspěchu ji zruší; neúspěch při ukončení aplikace se ohlásí vždy
 * modálně. Výsledek výpočtu délek bloků (CMT_REQUEST_LENGTHS_NAME)
 * spolkne bez hlášení, i odmítnutý. Kopii pulzů a uplatnění analýzy
 * (CMT_REQUEST_ANALYSIS_PULSES_NAME, _APPLY_NAME) předá dialogu
 * analýzy (cmt_analyze_dialog_request_result()) s výsledkem úpravy; zápis
 * informací o pásce a převod profilu (CMT_REQUEST_TAPE_INFO_SET_NAME,
 * CMT_REQUEST_CONVERT_BLOCK_NAME) dialogům informací o pásce a převodu
 * (cmt_tapeinfo_dialog_request_result(), cmt_convert_dialog_request_result()).
 *
 * @param res Výsledek zásahu.
 * @return true = výsledek patřil kazetě a byl ohlášen (nebo spolknut).
 * @pre UI vlákno; zámek stavu zařízení si vezme, nedrží-li ho.
 */
bool imgui_cmt_device_request_result(const st_DEVICE_REQ_RESULT *res);

/**
 * @brief Pod zámkem stavu zařízení přečte data výsledku úpravy pásky, která jádro drží mimo frontu.
 *
 * Definice v cmt/cmt_tape_actions.cpp. U zásahů úprav pásky
 * (CMT_REQUEST_REMOVE_BLOCKS_NAME, _MOVE_BLOCK_NAME, _IMPORT_NAME,
 * _ANALYSIS_PULSES_NAME, _ANALYSIS_APPLY_NAME, _TAPE_INFO_SET_NAME,
 * _CONVERT_BLOCK_NAME) si
 * zkopíruje výsledek úpravy (cmt_request_get_edit_result()), u exportu
 * celé pásky (CMT_REQUEST_EXPORT_TAPE_NAME) zprávu a varování exportu
 * (cmttmz_get_export_message(), cmttmz_get_export_warning()).
 * imgui_device_request_results() ji volá ve stejném držení zámku jako
 * device_request_take_result(): mezi vyzvednutím výsledku a čtením dat
 * tak emu vlákno neprovede další zásah, který by kruh výsledků úprav nebo
 * zprávu exportu přepsal. imgui_cmt_device_request_result()
 * pak použije zkopírovaná data; bez nich (test volá obsluhu přímo) si je
 * přečte sama pod zámkem.
 *
 * @param res Výsledek právě vyzvednutý z fronty.
 * @pre UI vlákno, drží device_state_lock().
 * @post Data výsledku se seq == res->seq jsou zkopírovaná (jiné zásahy nic).
 */
void imgui_cmt_device_request_capture(const st_DEVICE_REQ_RESULT *res);

/**
 * @brief Vyzvedne výsledky zásahů a ohlásí odmítnuté.
 *
 * Více odmítnutí za jeden snímek ohlásí jednou zprávou. Výsledky,
 * které mají vlastní oznámení (reinicializace Unicardu), předá
 * zařízení. Každý výsledek vyzvedne pod device_state_lock() a v témže
 * držení zámku zkopíruje data výsledku úpravy pásky
 * (imgui_cmt_device_request_capture()); obsluhy zařízení běží až po
 * uvolnění zámku.
 *
 * @pre Volat z UI vlákna každý snímek (main_window), NEdrží device_state_lock().
 */
void imgui_device_request_results(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REQUEST_UI_H */
