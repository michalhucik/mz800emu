/**
 * @file unicard_request.c
 * @brief Zásahy GUI do Unicardu přes emu vlákno (kontrakt v unicard_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = hodnota volby, str =
 * cesta SD root.
 */

#include <stdlib.h>
#include <string.h>

#include "unicard.h"
#include "unicard_request.h"
#include "device_request.h"


/** @brief Zásah: připojení (i[0] 0/1). */
static int unicard_request_exec_set_connected(const st_DEVICE_REQ_ARGS *a)
{
    unicard_set_connected(a->i[0] ? UNICARD_CONNECTION_CONNECTED : UNICARD_CONNECTION_DISCONNECTED);
    return 0;
}


uint32_t unicard_request_set_connected(int connected)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = connected ? 1 : 0;
    return device_request_submit("unicard_set_connected", unicard_request_exec_set_connected, &a);
}


/** @brief Zásah: připojení bez kontroly runtime. */
static int unicard_request_exec_connect_bypass(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    unicard_bypass_runtime_check_once();
    unicard_set_connected(UNICARD_CONNECTION_CONNECTED);
    return 0;
}


uint32_t unicard_request_connect_bypass_check(void)
{
    return device_request_submit("unicard_connect_bypass", unicard_request_exec_connect_bypass, NULL);
}


/** @brief Zásah: firmware (i[0] en_UNICARD_FW). */
static int unicard_request_exec_set_fw(const st_DEVICE_REQ_ARGS *a)
{
    unicard_set_fw((en_UNICARD_FW)a->i[0]);
    return 0;
}


uint32_t unicard_request_set_fw(int fw)
{
    if (fw != UNICARD_FW_UC1 && fw != UNICARD_FW_UC3) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = fw;
    return device_request_submit("unicard_set_fw", unicard_request_exec_set_fw, &a);
}


/** @brief Zásah: jen čtení SD (i[0] 0/1). */
static int unicard_request_exec_set_readonly(const st_DEVICE_REQ_ARGS *a)
{
    unicard_set_readonly(a->i[0]);
    return 0;
}


uint32_t unicard_request_set_readonly(int readonly)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = readonly ? 1 : 0;
    return device_request_submit("unicard_set_readonly", unicard_request_exec_set_readonly, &a);
}


/** @brief Zásah: spravovaný FDC (i[0] en_UNICARD_MANAGED_FDC). */
static int unicard_request_exec_set_managed_fdc(const st_DEVICE_REQ_ARGS *a)
{
    unicard_set_managed_fdc((en_UNICARD_MANAGED_FDC)a->i[0]);
    return 0;
}


uint32_t unicard_request_set_managed_fdc(int managed_fdc)
{
    if (managed_fdc != UNICARD_MANAGED_FDC_NONE && managed_fdc != UNICARD_MANAGED_FDC_FDC0 &&
        managed_fdc != UNICARD_MANAGED_FDC_FDC1) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = managed_fdc;
    return device_request_submit("unicard_set_managed_fdc", unicard_request_exec_set_managed_fdc, &a);
}


/** @brief Zásah: kontrola runtime při připojení (i[0] 0/1). */
static int unicard_request_exec_set_runtime_check(const st_DEVICE_REQ_ARGS *a)
{
    unicard_set_runtime_check_on_connect(a->i[0]);
    return 0;
}


uint32_t unicard_request_set_runtime_check(int enabled)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = enabled ? 1 : 0;
    return device_request_submit("unicard_set_runtime_check", unicard_request_exec_set_runtime_check, &a);
}


/** @brief Zásah: SD root (str adresář). */
static int unicard_request_exec_set_sd_root(const st_DEVICE_REQ_ARGS *a)
{
    unicard_apply_sd_root(a->str);
    return 0;
}


uint32_t unicard_request_set_sd_root(const char *path)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    return device_request_submit("unicard_set_sd_root", unicard_request_exec_set_sd_root, &a);
}


/** @brief Zásah: reinicializace s mgr V2.4 (uc1). */
static int unicard_request_exec_reinit_uc1(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    return (EXIT_SUCCESS == unicard_reinit_sd_for_fw(UNICARD_FW_UC1)) ? 0 : -1;
}


/** @brief Zásah: reinicializace s mgr V2.11b (uc3). */
static int unicard_request_exec_reinit_uc3(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    return (EXIT_SUCCESS == unicard_reinit_sd_for_fw(UNICARD_FW_UC3)) ? 0 : -1;
}


/** @brief Zásah: reinicializace pro zvolený firmware a po úspěchu připojení. */
static int unicard_request_exec_reinit_connect(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    if (EXIT_SUCCESS != unicard_reinit_sd_for_current_fw()) return -1;
    unicard_set_connected(UNICARD_CONNECTION_CONNECTED);
    return 0;
}


uint32_t unicard_request_reinit(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, UNICARD_REQUEST_NAME_REINIT_UC1) == 0)
        return device_request_submit(UNICARD_REQUEST_NAME_REINIT_UC1, unicard_request_exec_reinit_uc1, NULL);
    if (strcmp(name, UNICARD_REQUEST_NAME_REINIT_UC3) == 0)
        return device_request_submit(UNICARD_REQUEST_NAME_REINIT_UC3, unicard_request_exec_reinit_uc3, NULL);
    if (strcmp(name, UNICARD_REQUEST_NAME_REINIT_DEFAULT) == 0)
        return device_request_submit(UNICARD_REQUEST_NAME_REINIT_DEFAULT, unicard_request_exec_reinit_uc3, NULL);
    if (strcmp(name, UNICARD_REQUEST_NAME_REINIT_CONNECT) == 0)
        return device_request_submit(UNICARD_REQUEST_NAME_REINIT_CONNECT, unicard_request_exec_reinit_connect, NULL);
    return 0;
}
