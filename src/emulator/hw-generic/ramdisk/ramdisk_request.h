/**
 * @file ramdisk_request.h
 * @brief Zásahy GUI do RAM disků (MZ-1R18, Pezik) přes emu vlákno.
 *
 * Menu Memory Disk, file chooser souboru MZ-1R18 a okno Pezik Settings
 * dřív volaly ramdisk_std_init(), ramdisk_pezik_connect() a odpojení přímo
 * z UI vlákna; ty uvolňují a realokují paměť disku, kterou emu vlákno
 * právě čte přes porty. Tyto funkce zásah jen zařadí (device_request.h);
 * provede ho emu vlákno v device_request_poll() (při aktivním replay ho
 * odmítne).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef RAMDISK_REQUEST_H
#define RAMDISK_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hodnota typu nebo velikosti "beze změny" pro ramdisk_request_std_set(). */
#define RAMDISK_REQUEST_KEEP (-1)

/**
 * @brief Zařadí odpojení všech RAM disků (RAMDISK_DISCONNECT_ALL()).
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t ramdisk_request_disconnect_all(void);

/**
 * @brief Zařadí připojení nebo novou konfiguraci MZ-1R18 (ramdisk_std_init()).
 *
 * Typ, velikost i cesta, které se nemění, se při provedení vezmou
 * z aktuálního stavu `g_ramdisk.std`.
 *
 * @param type           en_RAMDISK_TYPE, nebo RAMDISK_REQUEST_KEEP.
 * @param size           en_RAMDISK_BANKMASK, nebo RAMDISK_REQUEST_KEEP.
 * @param path           Soubor disku (zkopíruje se), NULL = beze změny.
 * @param disconnect_all true = nejdřív odpojit všechny RAM disky (volba
 *                       "MZ-1R18 Compatible").
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t ramdisk_request_std_set(int type, int size, const char *path, bool disconnect_all);

/**
 * @brief Zařadí odpojení všech RAM disků a připojení Pezika (volba menu).
 * @param pezik_type RAMDISK_PEZIK_E8 nebo RAMDISK_PEZIK_68 (jiná hodnota =
 *                   nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t ramdisk_request_pezik_connect(int pezik_type);

/**
 * @brief Zařadí konfiguraci Pezika z okna Pezik Settings (ramdisk_pezik_init()).
 *
 * @param pezik_type      RAMDISK_PEZIK_E8 nebo RAMDISK_PEZIK_68.
 * @param connect         0 = odpojit, jinak připojit.
 * @param portmask        Maska portů.
 * @param backuped        PEZIK_BACKUPED_YES / PEZIK_BACKUPED_NO.
 * @param path            Záložní soubor (zkopíruje se, NULL = "").
 * @param disconnect_std  true = před připojením odpojit MZ-1R18 (Pezik E8
 *                        sdílí porty 0E8h-0EFh), jen když je MZ-1R18
 *                        při provedení připojený.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t ramdisk_request_pezik_init(int pezik_type, int connect, int portmask, int backuped,
                                    const char *path, bool disconnect_std);

#ifdef __cplusplus
}
#endif

#endif /* RAMDISK_REQUEST_H */
