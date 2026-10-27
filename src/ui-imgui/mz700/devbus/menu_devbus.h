/**
 * @file   menu_devbus.h
 * @brief  Generické menu volitelných zařízení devbus (jen MZ-700).
 *
 * Za každé registrované zařízení podmenu v HW & Devices: připojení, volby
 * (BOOL zaškrtávátko, KEYWORD výběr, UNSIGNED číselné pole), u voleb
 * DEVBUS_APPLY_CONNECT poznámka, že se projeví po novém připojení.
 * Změny jdou přes zásahy (devbus_request.h), UI stav zařízení jen čte
 * pod zámkem stavu zařízení (drží ho volající menu).
 *
 * Bez registrovaného zařízení se nic nezobrazí (v D1 tedy uživatel nic
 * nového nevidí).
 *
 * @par Licence: GPLv3
 */

#ifndef MENU_DEVBUS_H
#define MENU_DEVBUS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Specifické UI zařízení: položky v jeho podmenu.
 * @pre UI vlákno, drží zámek stavu zařízení (uvnitř podmenu zařízení).
 */
typedef void (*devbus_ui_menu_fn)(void);

/**
 * @brief Specifické UI zařízení: okna (volá se každý snímek UI).
 * @pre UI vlákno; zámek stavu zařízení drží volající (imgui_devbus_windows()).
 */
typedef void (*devbus_ui_windows_fn)(void);

/**
 * @brief Zaregistruje specifické UI zařízení (např. okno monitoru D4).
 *
 * @param id         Id zařízení (statický řetězec, jako v deskriptoru).
 * @param menu_fn    Položky přidané na konec podmenu zařízení (smí být NULL).
 * @param windows_fn Okna zařízení (smí být NULL).
 * @return 0 = OK, -1 = plno (nejvýš DEVBUS_MAX_DEVICES registrací) nebo
 *         neplatný argument.
 * @pre UI vlákno při inicializaci GUI (před prvním snímkem).
 */
int devbus_ui_register_extra(const char *id, devbus_ui_menu_fn menu_fn, devbus_ui_windows_fn windows_fn);

/**
 * @brief Podmenu zařízení v menu HW & Devices.
 *
 * Bez inicializovaného devbus nebo bez registrovaného zařízení nic
 * nezobrazí (ani oddělovač).
 *
 * @pre UI vlákno uvnitř BeginMenu("HW & Devices"); drží zámek stavu zařízení.
 */
void imgui_menu_devbus(void);

/**
 * @brief Okna zařízení (windows_fn z devbus_ui_register_extra()).
 *
 * Bere zámek stavu zařízení sám. Bez registrovaného UI nic nedělá.
 *
 * @pre UI vlákno, každý snímek.
 */
void imgui_devbus_windows(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_DEVBUS_H */
