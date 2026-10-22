/**
 * @file fdc_request.h
 * @brief Zásahy GUI do FDC přes emu vlákno (device_request.h).
 *
 * Menu FD Controller, zkratky Alt+1..4 / Alt+Shift+1..4, file chooser
 * mountu a popup přepnutí úložiště volají tyto funkce místo přímých
 * fdc_mount_dskfile(), fdc_umount() a zápisů do `g_fdc[]`. Každá funkce
 * jen zařadí zásah; provede ho emu vlákno v device_request_poll() (při
 * aktivním replay ho odmítne). Chyby provedení hlásí zásah přes
 * baseui_show_message() jako dřív callback mountu.
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno
 * (neplatný index řadiče nebo mechaniky).
 */

#ifndef FDC_REQUEST_H
#define FDC_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#include "fdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zařadí mount DSK obrazu (fdc_mount_dskfile()).
 *
 * Při provedení: odpojený řadič = hlášení a návrat -1 bez změny; jinak
 * mount (cesta do INI, při chybě otevření zůstane mechanika prázdná).
 * Cestu delší než `filename` mechaniky (1023 znaků) odmítne už zařazení;
 * stejná kontrola při provedení je jen pojistka pro případ, že by se pole
 * zmenšilo. Hlášení "filepath is too big" ukáže callback dialogu.
 *
 * @param fdc_index Index řadiče (FDC0, FDC1).
 * @param drive_id  Mechanika 0..3.
 * @param path      Cesta k DSK (zkopíruje se; NULL nebo "" = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t fdc_request_mount(unsigned fdc_index, unsigned drive_id, const char *path);

/**
 * @brief Zařadí odpojení obrazu (fdc_umount(): cached zapíše, INI se smaže).
 * @param fdc_index Index řadiče.
 * @param drive_id  Mechanika 0..3.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_umount(unsigned fdc_index, unsigned drive_id);

/**
 * @brief Zařadí změnu volby read-only mechaniky a remount nasazeného obrazu.
 * @param fdc_index Index řadiče.
 * @param drive_id  Mechanika 0..3.
 * @param readonly  0 = zápis povolen, jinak jen pro čtení.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_set_readonly(unsigned fdc_index, unsigned drive_id, int readonly);

/**
 * @brief Zařadí přepnutí režimu úložiště a remount nasazeného obrazu.
 *
 * @param fdc_index  Index řadiče.
 * @param drive_id   Mechanika 0..3.
 * @param mode       "cached", "direct" nebo "discard" (zkopíruje se;
 *                   jiná hodnota = nezařadí se).
 * @param save_first true = před přepnutím zapsat paměťový obraz do souboru
 *                   (fdc_drive_force_save_to_file(), popup "Save and switch").
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_set_storage_mode(unsigned fdc_index, unsigned drive_id,
                                      const char *mode, bool save_first);

/**
 * @brief Zařadí zápis paměťového obrazu do souboru (fdc_sync_drive()).
 * @param fdc_index Index řadiče.
 * @param drive_id  Mechanika 0..3.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_sync(unsigned fdc_index, unsigned drive_id);

/**
 * @brief Zařadí zapnutí nebo vypnutí obvodu HD Patch (`hd_patch`).
 *
 * Hodnotu čte řadič za běhu (`wd279x.c`), změna platí hned.
 *
 * @param fdc_index Index řadiče.
 * @param enabled   0 = vypnuto, jinak zapnuto.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_set_hd_patch(unsigned fdc_index, int enabled);

/**
 * @brief Zařadí změnu polarity BUS translation (`bus_xlate`).
 * @param fdc_index Index řadiče.
 * @param xlate     FDC_BUS_XLATE_INVERT nebo FDC_BUS_XLATE_PASSTHROUGH
 *                  (jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_set_bus_xlate(unsigned fdc_index, en_FDC_BUS_XLATE xlate);

/**
 * @brief Zařadí připojení nebo odpojení řadiče (`connected`).
 * @param fdc_index Index řadiče.
 * @param connected 0 = odpojit, jinak připojit.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t fdc_request_set_connected(unsigned fdc_index, int connected);

#ifdef __cplusplus
}
#endif

#endif /* FDC_REQUEST_H */
