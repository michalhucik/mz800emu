/**
 * @file joy_request.h
 * @brief Zásahy GUI do joysticků přes emu vlákno (device_request.h).
 *
 * Okno Joystick Setup dřív přepisovalo typ zařízení (g_joy.dev[].type)
 * přímo z UI vlákna, zatímco ho emu vlákno čte při každém čtení portu
 * joysticku a záznam vstupů ho ukládá na začátku. Tato funkce změnu typu
 * jen zařadí; provede ji emu vlákno v device_request_poll() (při aktivním
 * replay ji odmítne, záznam by změnu nezopakoval).
 *
 * Otevření a zavření zařízení SDL zůstává v UI vlákně pod zámkem modulu
 * iface_joy (iface_joy_scan() ho v emu vlákně jen zkouší).
 *
 * Funkce: libovolné vlákno (typicky UI), nečeká na provedení, vrací
 * pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef JOY_REQUEST_H
#define JOY_REQUEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí změnu typu zařízení joysticku.
 * @param joy_devid en_JOY_DEVID (mimo rozsah = nezařadí se).
 * @param type en_JOY_TYPE (neplatná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t joy_request_set_type(int joy_devid, int type);

#ifdef __cplusplus
}
#endif

#endif /* JOY_REQUEST_H */
