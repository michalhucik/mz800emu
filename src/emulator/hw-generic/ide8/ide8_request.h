/**
 * @file ide8_request.h
 * @brief Zásahy GUI do IDE8 přes emu vlákno (device_request.h).
 *
 * Menu IDE8 a file chooser obrazu dřív otevíraly a zavíraly obraz disku
 * přímo z UI vlákna, zatímco ho emu vlákno mohlo právě číst nebo zapisovat.
 * Tyto funkce zásah jen zařadí; provede ho emu vlákno v device_request_poll()
 * (při aktivním replay ho odmítne).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno
 * (neplatná mechanika nebo cesta).
 */

#ifndef IDE8_REQUEST_H
#define IDE8_REQUEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí připojení nebo odpojení disku (ide8_drive_set_connected()).
 * @param drive_id  IDE8_DRIVE_MASTER nebo IDE8_DRIVE_SLAVE.
 * @param connected 0 = odpojit, jinak připojit obraz podle `filepath`.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t ide8_request_set_connected(int drive_id, int connected);

/**
 * @brief Zařadí nasazení nového obrazu disku.
 *
 * Při provedení: připojený obraz se zavře, nový se otevře; po úspěchu se
 * cesta zapíše do `g_ide8.drive[].filepath` (původní řetězec se uvolní),
 * po chybě zůstane mechanika odpojená a cesta beze změny (návrat -1).
 *
 * @param drive_id IDE8_DRIVE_MASTER nebo IDE8_DRIVE_SLAVE.
 * @param path     Soubor obrazu (zkopíruje se; NULL nebo "" = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t ide8_request_set_image(int drive_id, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IDE8_REQUEST_H */
