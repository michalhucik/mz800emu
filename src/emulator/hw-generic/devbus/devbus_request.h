/**
 * @file   devbus_request.h
 * @brief  Zásahy GUI do zařízení devbus přes emu vlákno (device_request.h).
 *
 * UI nikdy nevolá devbus_connect() ani devbus_option_set() přímo: tyto
 * funkce zásah jen zařadí, provede ho emu vlákno v device_request_poll()
 * (při aktivním replay ho odmítne, STATE_REV zvedne poll).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno
 * (devbus není inicializovaný, neznámé id, neplatná volba nebo hodnota).
 * Návrat funkce zásahu (st_DEVICE_REQ_RESULT::rc) je en_DEVBUS_RESULT.
 *
 * @par Licence: GPLv3
 */

#ifndef DEVBUS_REQUEST_H
#define DEVBUS_REQUEST_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí připojení nebo odpojení zařízení.
 * @param id        Id zařízení (zkopíruje se).
 * @param connected true = devbus_connect(), false = devbus_disconnect().
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t devbus_request_connect(const char *id, bool connected);

/**
 * @brief Zařadí změnu volby (devbus_option_set()).
 * @param id    Id zařízení (zkopíruje se).
 * @param opt   Index volby v deskriptoru.
 * @param value Nová hodnota (ověří se už při zařazení).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno.
 */
uint32_t devbus_request_option(const char *id, unsigned opt, unsigned value);

#ifdef __cplusplus
}
#endif

#endif /* DEVBUS_REQUEST_H */
