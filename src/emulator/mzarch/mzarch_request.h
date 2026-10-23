/**
 * @file mzarch_request.h
 * @brief Zásahy GUI do přepínačů stroje přes emu vlákno (device_request.h).
 *
 * Zadní DIP přepínač SW1 (Mode 700 Compat.) a HW experiment Allow PSG1
 * dřív měnilo menu Devices přímo z UI vlákna: mzarch_mode_sw_set() mění
 * stav, který čte GDG při každém přístupu, mz800_mz800_hwcompat_allow_psg1()
 * inicializuje druhý PSG, do kterého emu vlákno zapisuje. Tyto funkce
 * zásah jen zařadí; provede ho emu vlákno v device_request_poll() (při
 * aktivním replay ho odmítne, záznam by změnu nezopakoval).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef MZARCH_REQUEST_H
#define MZARCH_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí přepnutí SW1 (mzarch_mode_sw_set()).
 * @param mode en_MZ800_MODE_SW (MZ800_MODE_SW_MZ700 nebo MZ800_MODE_SW_MZ800;
 *        jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 * @note V buildu MZ-700 je provedení no-op (přepínač neexistuje).
 */
uint32_t mzarch_request_mode_sw_set(int mode);

#if MZARCH == 800
/**
 * @brief Zařadí zapnutí nebo vypnutí druhého PSG
 *        (mz800_mz800_hwcompat_allow_psg1()).
 * @param allow true = PSG1 povolit (stereo).
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t mzarch_request_allow_psg1(bool allow);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MZARCH_REQUEST_H */
