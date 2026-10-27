/**
 * @file   devbus_request.c
 * @brief  Zásahy GUI do zařízení devbus přes emu vlákno (kontrakt v devbus_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): str = id zařízení,
 * i[0] = připojit (connect) / index volby (option), i[1] = hodnota volby.
 * Slot se hledá až při provedení (id je stabilní, slot jen v rámci procesu).
 *
 * @par Licence: GPLv3
 */

#include "devbus_internal.h"
#include "devbus_request.h"

#include "device_request.h"


/** @brief Zásah: připojení / odpojení (str id, i[0] 0/1). */
static int devbus_request_exec_connect(const st_DEVICE_REQ_ARGS *a)
{
    int slot = devbus_find(a->str);
    if (slot < 0) return slot;
    return a->i[0] ? devbus_connect(slot) : devbus_disconnect(slot);
}


uint32_t devbus_request_connect(const char *id, bool connected)
{
    if (!devbus_is_initialized() || devbus_find(id) < 0) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, id)) return 0;
    a.i[0] = connected ? 1 : 0;
    return device_request_submit("devbus_connect", devbus_request_exec_connect, &a);
}


/** @brief Zásah: volba (str id, i[0] index, i[1] hodnota). */
static int devbus_request_exec_option(const st_DEVICE_REQ_ARGS *a)
{
    int slot = devbus_find(a->str);
    if (slot < 0) return slot;
    return devbus_option_set(slot, (unsigned) a->i[0], (unsigned) a->i[1]);
}


uint32_t devbus_request_option(const char *id, unsigned opt, unsigned value)
{
    if (!devbus_is_initialized()) return 0;
    int slot = devbus_find(id);
    st_DEVBUS_SLOT *s = devbus_slot_get(slot);
    if (!s || opt >= s->desc->option_count) return 0;
    if (!devbus_option_value_valid(&s->desc->options[opt], value)) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, id)) return 0;
    a.i[0] = (int) opt;
    a.i[1] = (int) value;
    return device_request_submit("devbus_option", devbus_request_exec_option, &a);
}
