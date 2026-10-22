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
 * @brief Vyzvedne výsledky zásahů a ohlásí odmítnuté.
 *
 * Více odmítnutí za jeden snímek ohlásí jednou zprávou. Výsledky,
 * které mají vlastní oznámení (reinicializace Unicardu), předá
 * zařízení.
 *
 * @pre Volat z UI vlákna každý snímek (main_window).
 */
void imgui_device_request_results(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REQUEST_UI_H */
