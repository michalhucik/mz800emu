/**
 * @file mz1p16_request.h
 * @brief Zásahy okna plotteru MZ-1P16 přes emu vlákno (device_request.h).
 *
 * Plotter má vlastní serializační zámek (mz1p16_emu_lock()), ten ale
 * nechrání tiskárnu: mz1p16_emu_set_active() přepíná standard tiskárny
 * (pioz80_set_printer_std()) a mz1p16_emu_reset() zavírá soubor
 * zachytávání (printer_close_file()). Okno plotteru proto aktivaci
 * a reset jen zařadí; provede je emu vlákno v device_request_poll() (při
 * aktivním replay je odmítne). Stejně jdou tlačítka Paper feed
 * a kreslicí self-test: mění stav plotteru, který čte Z80 přes tiskárnu,
 * a provedený zásah zvedne revizi stavu (state_rev.h). Kreslení papíru
 * oknem jde dál přes vlastní zámek.
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

/**
 * @brief Zařadí stisk nebo uvolnění tlačítka Paper feed (mz1p16_emu_button_paper_feed()).
 * @param pressed true = drženo (T1=0), false = uvolněno.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t mz1p16_request_paper_feed(bool pressed);

/**
 * @brief Zařadí kreslicí self-test (mz1p16_emu_run_drawing_selftest()).
 *
 * Po provedení je Paper feed uvolněn (T1=1). Neaktivní plotter = zásah
 * nic neudělá.
 *
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t mz1p16_request_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* MZ1P16_REQUEST_H */
