/**
 * @file device_request_ui.cpp
 * @brief GUI strana zásahů do zařízení (kontrakt v device_request_ui.h).
 */

#include <stdio.h>

// Lokalizace
#include "i18n.h"

#include "device_request.h"
#include "ui-imgui/devices/device_request_ui.h"
#include "ui-imgui/message/message_window.h"


void imgui_device_request_results(void)
{
    bool rejected = false;
    st_DEVICE_REQ_RESULT res;
    while (device_request_take_result(&res))
    {
        if (imgui_unicard_device_request_result(&res))
        {
            continue;
        };
        if (res.status == DEVICE_REQ_REJECTED_REPLAY)
        {
            fprintf(stderr, "Device request '%s' rejected: replay is active\n", res.name);
            rejected = true;
        };
    };

    if (rejected)
    {
        imgui_message_create(MESSAGE_TYPE_WARNING,
                             _("Machine settings cannot be changed while a replay is recorded or played. "
                               "Stop it in the Replay menu first."));
    };
}
