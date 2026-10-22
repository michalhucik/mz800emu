/**
 * @file memext_request.h
 * @brief Zásahy GUI do MemExt přes emu vlákno (device_request.h).
 *
 * Menu MemExt, výběr souboru FLASH a dialog "Load MemExt RAM / FLASH From
 * File" dřív přepojovaly paměť (memory_reconnect_ram()) a četly soubor
 * (fread) přímo do `g_memext.RAM` / `FLASH` z UI vlákna souběžně s
 * emulací. Tyto funkce zásah jen zařadí; provede ho emu vlákno
 * v device_request_poll() (při aktivním replay ho odmítne).
 *
 * Mapa MemExt (okno Memory Map Settings) jde dál vlastní cestou
 * memext_map_request(). Volby inicializace (init fill, Luftner auto init)
 * jsou jen konfigurace pro příští init nebo reset a zůstávají v UI.
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef MEMEXT_REQUEST_H
#define MEMEXT_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hodnota typu pro memext_request_set_connection(): odpojit. */
#define MEMEXT_REQUEST_DISCONNECT (-1)

/**
 * @brief Zařadí připojení MemExt daného typu nebo odpojení.
 * @param type en_MEMEXT_TYPE (memext_connect()), nebo MEMEXT_REQUEST_DISCONNECT
 *             (memext_disconnect()); jiná hodnota = nezařadí se.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t memext_request_set_connection(int type);

/**
 * @brief Zařadí nastavení souboru FLASH Luftner a jeho načtení
 *        (memext_luftner_set_flash_filepath()).
 * @param path Cesta (zkopíruje se; NULL nebo "" = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t memext_request_set_flash_filepath(const char *path);

/**
 * @brief Zařadí načtení úseku souboru do RAM nebo FLASH MemExt.
 *
 * Při provedení: soubor se otevře, posune na `file_offset` a přečte
 * `size` bajtů na `mem_from`. Chyba otevření nebo kratší čtení se ohlásí
 * přes baseui_show_message() a vrátí -1 (přečtená část v paměti zůstane).
 *
 * @param flash       false = RAM, true = FLASH.
 * @param path        Soubor (zkopíruje se; NULL nebo "" = nezařadí se).
 * @param file_offset Offset v souboru.
 * @param mem_from    Offset v cílové paměti.
 * @param size        Počet bajtů (> 0; `mem_from + size` nesmí přesáhnout
 *                    MEMEXT_RAM_SIZE / MEMEXT_FLASH_SIZE, jinak se nezařadí).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t memext_request_load(bool flash, const char *path, uint32_t file_offset,
                             uint32_t mem_from, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MEMEXT_REQUEST_H */
