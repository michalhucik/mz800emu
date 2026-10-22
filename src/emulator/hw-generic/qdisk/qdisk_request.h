/**
 * @file qdisk_request.h
 * @brief Zásahy GUI do Quick Disku přes emu vlákno (device_request.h).
 *
 * Menu Quick Disk, file chooser mountu a popup přepnutí úložiště volají
 * tyto funkce místo přímých qdisk_close() / qdisk_open(), mountu
 * a zápisů do `g_qdisk`. Každá funkce jen zařadí zásah; provede ho emu
 * vlákno v device_request_poll() (při aktivním replay ho odmítne).
 *
 * Všechny funkce: libovolné vlákno (typicky UI), nečekají na provedení,
 * vrací pořadové číslo zásahu (device_request_submit()), 0 = nezařazeno.
 */

#ifndef QDISK_REQUEST_H
#define QDISK_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Způsob připojení Quick Disku (volby menu Quick Disk).
 */
typedef enum en_QDISK_REQ_CONNECTION {
    QDISK_REQ_DISCONNECTED = 0,  /**< Not Connected: zavřít a odpojit. */
    QDISK_REQ_MZ1F11,            /**< MZ-1F11 s obrazem .MZQ (typ IMAGE). */
    QDISK_REQ_VIRTUAL,           /**< Virtuální QD nad adresářem (typ VIRTUAL). */
    QDISK_REQ_UNICARD,           /**< Zavaděč Unicard (typ UNICARD, jen s připojeným Unicardem). */
} en_QDISK_REQ_CONNECTION;

/**
 * @brief Zařadí změnu způsobu připojení (stejná posloupnost jako dřív menu).
 * @param conn Způsob připojení (jiná hodnota = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t qdisk_request_set_connection(en_QDISK_REQ_CONNECTION conn);

/**
 * @brief Zařadí mount image nebo adresáře (qdisk_mount_path()).
 * @param path   Cesta (zkopíruje se; NULL nebo "" = nezařadí se).
 * @param is_dir 0 = soubor image, jinak adresář virtuálního QD.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t qdisk_request_mount(const char *path, int is_dir);

/**
 * @brief Zařadí vytvoření prázdného obrazu .MZQ (qdisk_create_image()).
 *
 * Soubor se přepíše; běží v emu vlákně, protože může jít o právě
 * nasazený obraz.
 *
 * @param path Cesta (zkopíruje se; NULL nebo "" = nezařadí se).
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument,
 *         nebo cesta delší než DEVICE_REQUEST_STR_MAX - 1 bajtů:
 *         neořezává se).
 */
uint32_t qdisk_request_create_image(const char *path);

/**
 * @brief Zařadí odpojení média (qdisk_umount(): cesta v INI se smaže).
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t qdisk_request_umount(void);

/**
 * @brief Zařadí změnu ochrany proti zápisu (qdisk_set_write_protected()).
 * @param value 0 = zápis povolen, jinak chráněno.
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t qdisk_request_set_write_protected(int value);

/**
 * @brief Zařadí zápis paměťového obrazu do souboru (qdisk_sync_drive()).
 * @return Pořadové číslo zásahu (od 1).
 */
uint32_t qdisk_request_sync(void);

/**
 * @brief Zařadí přepnutí režimu úložiště (qdisk_apply_storage_mode_switch()).
 * @param mode       "cached", "direct" nebo "discard" (jiná hodnota = nezařadí se).
 * @param save_first true = nejdřív zapsat paměťový obraz do souboru
 *                   (qdisk_drive_force_save_to_file(), popup "Save and switch").
 * @return Pořadové číslo zásahu (od 1); 0 = nezařazeno (neplatný argument).
 */
uint32_t qdisk_request_set_storage_mode(const char *mode, bool save_first);

#ifdef __cplusplus
}
#endif

#endif /* QDISK_REQUEST_H */
