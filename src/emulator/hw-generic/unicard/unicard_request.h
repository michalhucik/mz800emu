/**
 * @file unicard_request.h
 * @brief Zásahy GUI do Unicardu přes emu vlákno (device_request.h).
 *
 * Menu Unicard, výběr SD root a dialogy reinicializace runtime (nesoulad
 * runtime s firmware, volba verze manageru) dřív volaly
 * unicard_set_connected(), unicard_set_fw(), reinicializaci a další přímo
 * z UI vlákna; odpojení zavírá soubory a adresáře, se kterými emulovaný
 * Unicard právě pracuje, reinicializace přepisuje a maže soubory
 * v `{SD}/unicard/`. Tyto funkce zásah jen zařadí; provede ho emu vlákno
 * v device_request_poll() (při aktivním replay ho odmítne).
 *
 * Výsledek reinicializace ohlásí UI podle názvu zásahu
 * (UNICARD_REQUEST_NAME_*) a návratu `rc` (0 = úspěch).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef UNICARD_REQUEST_H
#define UNICARD_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Název zásahu: reinicializace pro zvolený firmware a připojení (dialog nesouladu). */
#define UNICARD_REQUEST_NAME_REINIT_CONNECT "unicard_reinit_connect"
/** @brief Název zásahu: reinicializace s manažerem mgr V2.4 (uc1). */
#define UNICARD_REQUEST_NAME_REINIT_UC1 "unicard_reinit_uc1"
/** @brief Název zásahu: reinicializace s manažerem mgr V2.11b (uc3) z dialogu volby. */
#define UNICARD_REQUEST_NAME_REINIT_UC3 "unicard_reinit_uc3"
/** @brief Název zásahu: reinicializace bez dialogu (platformy mimo MZ-800, vždy uc3). */
#define UNICARD_REQUEST_NAME_REINIT_DEFAULT "unicard_reinit_default"

/**
 * @brief Zařadí připojení nebo odpojení (unicard_set_connected()).
 * @param connected 0 = odpojit, jinak připojit (s kontrolou runtime podle
 *                  nastavení; nesoulad otevře dialog).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_set_connected(int connected);

/**
 * @brief Zařadí připojení bez kontroly runtime (dialog nesouladu, Continue).
 *
 * unicard_bypass_runtime_check_once() a unicard_set_connected().
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t unicard_request_connect_bypass_check(void);

/**
 * @brief Zařadí změnu simulovaného firmware (unicard_set_fw()).
 * @param fw en_UNICARD_FW (jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_set_fw(int fw);

/**
 * @brief Zařadí změnu režimu jen pro čtení SD (unicard_set_readonly()).
 * @param readonly 0 = zápis povolen, jinak jen čtení.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_set_readonly(int readonly);

/**
 * @brief Zařadí volbu spravovaného FDC (unicard_set_managed_fdc()).
 * @param managed_fdc en_UNICARD_MANAGED_FDC (jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_set_managed_fdc(int managed_fdc);

/**
 * @brief Zařadí volbu kontroly runtime při připojení
 *        (unicard_set_runtime_check_on_connect(), zapíše i INI).
 * @param enabled 0 = bez kontroly, jinak kontrolovat.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_set_runtime_check(int enabled);

/**
 * @brief Zařadí změnu SD root adresáře (unicard_apply_sd_root()).
 * @param path Adresář (zkopíruje se; NULL nebo "" = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t unicard_request_set_sd_root(const char *path);

/**
 * @brief Zařadí reinicializaci runtime v `{SD}/unicard/`.
 *
 * @param name Jeden z UNICARD_REQUEST_NAME_REINIT_*: určuje verzi manageru
 *             (UC1 = uc1; UC3 a DEFAULT = uc3; CONNECT = podle zvoleného
 *             firmware a po úspěchu připojení). Jiný řetězec = nezařadí se.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t unicard_request_reinit(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_REQUEST_H */
