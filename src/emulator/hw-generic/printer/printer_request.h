/**
 * @file printer_request.h
 * @brief Zásahy GUI do tiskárny přes emu vlákno (device_request.h).
 *
 * Menu Printer a zadní DIP přepínač dřív volaly pioz80_set_printer_std(),
 * printer_set_active() a printer_close_file() přímo z UI vlákna;
 * printer_close_file() zavírá soubor zachytávání, do kterého emu vlákno
 * právě zapisuje bajty z PIO. Tyto funkce zásah jen zařadí; provede ho emu
 * vlákno v device_request_poll() (při aktivním replay ho odmítne).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef PRINTER_REQUEST_H
#define PRINTER_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí změnu standardu tiskárny (pioz80_set_printer_std()).
 * @param std en_PIOZ80_PRINTER_STD (jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t printer_request_set_std(int std);

/**
 * @brief Zařadí zapnutí nebo vypnutí zachytávání tisku (printer_set_active()).
 * @param active true = zachytávat.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t printer_request_set_active(bool active);

/**
 * @brief Zařadí uzavření souboru zachytávání (printer_close_file()).
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t printer_request_close_file(void);

#ifdef __cplusplus
}
#endif

#endif /* PRINTER_REQUEST_H */
