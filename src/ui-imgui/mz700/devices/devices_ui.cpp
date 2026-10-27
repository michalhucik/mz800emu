/**
 * @file   devices_ui.cpp
 * @brief  Registrace specifického UI zařízení devbus jádra MZ-700 (kontrakt v devices_ui.h).
 *
 * @par Licence: GPLv3
 */

#include "devices_ui.h"
#include "pcg700/pcg700_ui.h"
#include "kp80/kp80_window.h"


void mz700_devices_ui_register(void)
{
    pcg700_ui_register();
    kp80_ui_register();
}
