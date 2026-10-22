/**
 * @file mz1p16_request.h
 * @brief Zásahy okna plotteru MZ-1P16 přes emu vlákno (device_request.h).
 *
 * Plotter má vlastní serializační zámek (mz1p16_emu_lock()), ten ale
 * nechrání tiskárnu: mz1p16_emu_set_active() přepíná standard tiskárny
 * (pioz80_set_printer_std()) a mz1p16_emu_reset() zavírá soubor
 * zachytávání (printer_close_file()). Okno plotteru proto aktivaci
 * a reset jen zařadí; provede je emu vlákno v device_request_poll() (při
 * aktivním replay je odmítne). Ostatní ovládání plotteru (papír, posun
 * papíru, self-test) jde dál přes vlastní zámek.
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()).
 */

#ifndef MZ1P16_REQUEST_H
#define MZ1P16_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí připojení nebo odpojení plotteru (mz1p16_emu_set_active()).
 * @param active true = připojit (náběh přepne standard tiskárny na MZ).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t mz1p16_request_set_active(bool active);

/**
 * @brief Zařadí reset plotteru (mz1p16_emu_reset()), volitelně se smazáním papíru.
 * @param clear_paper true = po resetu i mz1p16_emu_clear_paper() (tlačítko Clear paper).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t mz1p16_request_reset(bool clear_paper);

#ifdef __cplusplus
}
#endif

#endif /* MZ1P16_REQUEST_H */
