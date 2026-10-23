/**
 * @file joy_request.c
 * @brief Zásahy GUI do joysticků přes emu vlákno (kontrakt v joy_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = zařízení, i[1] = typ.
 */

#include "joy.h"
#include "joy_request.h"
#include "device_request.h"


/** @brief Zásah: typ zařízení (i[0] en_JOY_DEVID, i[1] en_JOY_TYPE). */
static int joy_request_exec_set_type(const st_DEVICE_REQ_ARGS *a)
{
    g_joy.dev[a->i[0]].type = (en_JOY_TYPE)a->i[1];
    return 0;
}


uint32_t joy_request_set_type(int joy_devid, int type)
{
    if (joy_devid < 0 || joy_devid >= JOY_DEVID_COUNT) return 0;
    if (type != JOY_TYPE_NONE && type != JOY_TYPE_NUM_KEYPAD && type != JOY_TYPE_JOYSTICK) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = joy_devid;
    a.i[1] = type;
    return device_request_submit("joy_set_type", joy_request_exec_set_type, &a);
}
