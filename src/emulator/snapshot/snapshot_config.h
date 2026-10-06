/**
 * @file snapshot_config.h
 * @brief Konfigurace snapshotů — INI integrace
 */

#ifndef SNAPSHOT_CONFIG_H
#define SNAPSHOT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zaregistruje sekci [SNAPSHOT], načte ji z INI a propaguje do g_snapshot_settings.
 *
 * Globální cfgroot_propagate se v emulátoru nevolá, proto modul sám volá
 * cfgmodule_parse + cfgmodule_propagate. Chybí-li sekce nebo soubor, platí
 * výchozí hodnoty elementů.
 *
 * @pre g_cfgmain existuje (volá cfgmain_init()).
 * @post g_snapshot_settings odpovídá INI (resp. výchozím hodnotám).
 * @note Volat jednou; vlákno: hlavní, před startem emulace.
 */
void snapshot_config_init(void);

/**
 * @brief Ochrání INI hodnotu nastavení, které snapshot přepsal jen pro běh.
 *
 * Volá se v loaderu komponenty po přepsání proměnné svázané s cfg
 * elementem. Najde element @p module_name / @p element_name v g_cfgmain
 * a připne mu @p ini_value (cfgelement_pin_save_value()): do INI se
 * uloží hodnota před načtením snapshotu, dokud uživatel nastavení sám
 * nezmění.
 *
 * @param module_name Název INI sekce (např. "FDC", "MEMEXT").
 * @param element_name Název klíče v sekci.
 * @param ini_value Hodnota proměnné před přepsáním snapshotem.
 *
 * @pre Proměnná už obsahuje hodnotu ze snapshotu; element je KEYWORD,
 *      BOOL nebo UNSIGNED se save handlerem na tuto proměnnou.
 * @post Neexistuje-li sekce nebo klíč, vypíše varování a nic nezmění.
 * @post Bez g_cfgmain (NULL) nedělá nic.
 * @note Vlákno: to, které načítá snapshot; save handler čte proměnnou
 *       bez zámku jako dosud.
 */
void snapshot_config_pin_ini_value(const char *module_name, const char *element_name, unsigned ini_value);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_CONFIG_H */
