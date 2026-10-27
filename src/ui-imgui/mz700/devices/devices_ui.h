/**
 * @file   devices_ui.h
 * @brief  Registrace specifického UI zařízení devbus jádra MZ-700.
 *
 * @par Licence: GPLv3
 */

#ifndef MZ700_DEVICES_UI_H
#define MZ700_DEVICES_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zaregistruje specifické UI všech zařízení MZ-700 (devbus_ui_register_extra()).
 *
 * Jediné místo, kam zařízení přidávají svou registraci UI (PCG-700, K&P 80, další
 * karty). Zařízení bez specifického UI sem nepatří, generické menu je
 * zobrazí samo.
 *
 * @pre UI vlákno, jednou při inicializaci GUI (myimgui_init_cb).
 */
void mz700_devices_ui_register(void);

#ifdef __cplusplus
}
#endif

#endif /* MZ700_DEVICES_UI_H */
