/**
 * @file qdisk_request.c
 * @brief Zásahy GUI do Quick Disku přes emu vlákno (kontrakt v qdisk_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = volba (způsob připojení,
 * hodnota ochrany, is_dir), i[3] = příznak save_first, str = cesta nebo
 * režim úložiště.
 */

#include <string.h>

#include "qdisk.h"
#include "qdisk_request.h"
#include "device_request.h"
#include "unicard/unicard.h"


/** @brief Zásah: způsob připojení (i[0] en_QDISK_REQ_CONNECTION). */
static int qdisk_request_exec_set_connection(const st_DEVICE_REQ_ARGS *a)
{
    switch ((en_QDISK_REQ_CONNECTION)a->i[0]) {
        case QDISK_REQ_DISCONNECTED:
            qdisk_close();
            QDISK_SET_DISCONNECTED();
            return 0;
        case QDISK_REQ_MZ1F11:
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_deactivate_unicard_boot_loader();
            QDISK_SET_TYPE_IMAGE();
            qdisk_open();
            return 0;
        case QDISK_REQ_VIRTUAL:
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_deactivate_unicard_boot_loader();
            QDISK_SET_TYPE_VIRTUAL();
            return 0;
        case QDISK_REQ_UNICARD:
            /* Unicard se mohl mezi kliknutím a provedením odpojit. */
            if (!UNICARD_TEST_IS_CONNECTED) return -1;
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_activate_unicard_boot_loader();
            QDISK_SET_TYPE_UNICARD();
            qdisk_open();
            return 0;
    }
    return -1;
}


uint32_t qdisk_request_set_connection(en_QDISK_REQ_CONNECTION conn)
{
    if (conn != QDISK_REQ_DISCONNECTED && conn != QDISK_REQ_MZ1F11 &&
        conn != QDISK_REQ_VIRTUAL && conn != QDISK_REQ_UNICARD) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)conn;
    return device_request_submit("qdisk_set_connection", qdisk_request_exec_set_connection, &a);
}


/** @brief Zásah: mount (str cesta, i[0] is_dir). */
static int qdisk_request_exec_mount(const st_DEVICE_REQ_ARGS *a)
{
    return qdisk_mount_path(a->str, a->i[0]);
}


uint32_t qdisk_request_mount(const char *path, int is_dir)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    a.i[0] = is_dir ? 1 : 0;
    return device_request_submit("qdisk_mount", qdisk_request_exec_mount, &a);
}


/** @brief Zásah: vytvoření obrazu (str cesta). */
static int qdisk_request_exec_create_image(const st_DEVICE_REQ_ARGS *a)
{
    char path[DEVICE_REQUEST_STR_MAX];
    memcpy(path, a->str, sizeof(path));
    qdisk_create_image(path);
    return 0;
}


uint32_t qdisk_request_create_image(const char *path)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    return device_request_submit("qdisk_create_image", qdisk_request_exec_create_image, &a);
}


/** @brief Zásah: umount. */
static int qdisk_request_exec_umount(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    qdisk_umount();
    return 0;
}


uint32_t qdisk_request_umount(void)
{
    return device_request_submit("qdisk_umount", qdisk_request_exec_umount, NULL);
}


/** @brief Zásah: ochrana proti zápisu (i[0] hodnota). */
static int qdisk_request_exec_set_write_protected(const st_DEVICE_REQ_ARGS *a)
{
    qdisk_set_write_protected(a->i[0]);
    return 0;
}


uint32_t qdisk_request_set_write_protected(int value)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = value ? 1 : 0;
    return device_request_submit("qdisk_set_write_protected", qdisk_request_exec_set_write_protected, &a);
}


/** @brief Zásah: sync. */
static int qdisk_request_exec_sync(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    return qdisk_sync_drive() ? 0 : -1;
}


uint32_t qdisk_request_sync(void)
{
    return device_request_submit("qdisk_sync", qdisk_request_exec_sync, NULL);
}


/** @brief Zásah: režim úložiště (str režim, i[3] save_first). */
static int qdisk_request_exec_set_storage_mode(const st_DEVICE_REQ_ARGS *a)
{
    int rc = 0;
    if (a->i[3] && !qdisk_drive_force_save_to_file()) rc = -1;
    qdisk_apply_storage_mode_switch(a->str);
    return rc;
}


uint32_t qdisk_request_set_storage_mode(const char *mode, bool save_first)
{
    if (!mode) return 0;
    if (strcmp(mode, "cached") != 0 && strcmp(mode, "direct") != 0 && strcmp(mode, "discard") != 0)
        return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, mode);
    a.i[3] = save_first ? 1 : 0;
    return device_request_submit("qdisk_set_storage_mode", qdisk_request_exec_set_storage_mode, &a);
}
