/**
 * @file   pcg700_ui.h
 * @brief  UI zařízení PCG-700 (položka v podmenu HW & Devices, okno náhledu PCG RAM).
 *
 * @par Licence: GPLv3
 */

#ifndef PCG700_UI_H
#define PCG700_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zaregistruje specifické UI PCG-700 v generickém menu devbus.
 *
 * Přidá na konec podmenu zařízení položku "Show PCG RAM characters" a okno
 * s náhledem 256 glyfů PCG RAM.
 *
 * @pre UI vlákno při inicializaci GUI (myimgui_init_cb), jen jádro MZ-700.
 * @post Registrace platí do konce procesu; opakované volání přidá druhou
 *       registraci (volat jednou).
 */
void pcg700_ui_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PCG700_UI_H */
