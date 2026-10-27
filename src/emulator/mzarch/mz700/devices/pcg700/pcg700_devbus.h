/**
 * @file   pcg700_devbus.h
 * @brief  PCG-700 (HAL Laboratory) jako zařízení devbus jádra MZ-700.
 *
 * Lepidlo mezi čistým modelem pcg700_core a device API devbus: deskriptor
 * (volby, MMIO 0E010h-0E012h, zdroj glyfů, region PCG RAM, katalog I/O,
 * snapshot) a životní cyklus. Model a rozhodnutí: devdoc/pcg700.md.
 *
 * Soubor závisí jen na arch-neutrálním jádře devbus a na g_rom (CG-ROM),
 * takže se dá testovat i v testovacím jádru MZ-800 s falešným adaptérem
 * (tests/devices/pcg700/test_pcg700_devbus.c). V emulátoru ho volá jen
 * adaptér MZ-700 (mz700_devbus_start()).
 *
 * @par Licence: GPLv3
 */

#ifndef PCG700_DEVBUS_H
#define PCG700_DEVBUS_H

#include "pcg700_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Id zařízení (klíč snapshotu, MCP kind, neměnit). */
#define PCG700_DEVBUS_ID "pcg700"

/** @brief První adresa MMIO (registr data). */
#define PCG700_MMIO_FIRST 0xE010u
/** @brief Poslední adresa MMIO (registr řízení); 0E013h a výš se nedekódují. */
#define PCG700_MMIO_LAST  0xE012u

/**
 * @brief Indexy voleb v deskriptoru (pořadí v INI, menu a MCP).
 *
 * Hodnoty KEYWORD voleb hsw, ssw, gsw, ssw_off, strobe a poweron_ram jsou
 * přímo hodnoty odpovídajících výčtů pcg700_core (pořadí klíčových slov
 * = pořadí hodnot výčtu).
 */
typedef enum en_PCG700_DEVBUS_OPT {
    PCG700_OPT_HSW = 0,      /**< hsw: nom / pcg (en_PCG700_HSW), výchozí pcg. */
    PCG700_OPT_SSW,          /**< ssw: off / on (en_PCG700_SSW), výchozí on. */
    PCG700_OPT_GSW,          /**< gsw: off / on (en_PCG700_GSW), výchozí on. */
    PCG700_OPT_SSWOFF,       /**< ssw_off: hsw / force_off (en_PCG700_SSWOFF), výchozí hsw. */
    PCG700_OPT_STROBE,       /**< strobe: rising / level / falling (en_PCG700_STROBE), výchozí rising. */
    PCG700_OPT_DECODE_DRAM,  /**< decode_dram (BOOL): dekód zápisů i s DRAM na 0E000h, výchozí 0. */
    PCG700_OPT_POWERON_RAM,  /**< poweron_ram: zero / cgrom (en_PCG700_POWERON_RAM), uplatní se při připojení. */
    PCG700_OPT_COUNT         /**< Počet voleb. */
} en_PCG700_DEVBUS_OPT;

/**
 * @brief Zaregistruje PCG-700 v devbus.
 *
 * @return Slot (>= 0), nebo záporné en_DEVBUS_RESULT (registrace selhala,
 *         emulátor běží bez zařízení; adaptér MZ-700 vypíše varování).
 * @pre devbus_init() proběhl, devbus_start() ještě ne; emu vlákno
 *      (mz700_devbus_start()) nebo test.
 * @post Zařízení je odpojené, volby mají hodnoty z INI (jinak výchozí).
 */
int pcg700_devbus_register(void);

/**
 * @brief Slot zařízení z poslední registrace.
 * @return Slot (>= 0), nebo záporné číslo, když registrace neproběhla nebo selhala.
 */
int pcg700_devbus_slot(void);

/**
 * @brief Instance modelu (jen pro čtení: UI náhled, testy).
 *
 * @return Ukazatel na statickou instanci (platí po celou dobu procesu).
 * @note Stav mění emu vlákno; UI čte pod device_state_lock().
 */
const st_PCG700 *pcg700_devbus_instance(void);

/**
 * @brief Konfigurace modelu podle aktuálních voleb zařízení.
 * @param slot Slot zařízení.
 * @param cfg  Výstup (nesmí být NULL).
 * @pre Emu vlákno, nebo UI pod device_state_lock() (čte volby).
 */
void pcg700_devbus_config_from_options(int slot, st_PCG700_CONFIG *cfg);

#ifdef __cplusplus
}
#endif

#endif /* PCG700_DEVBUS_H */
