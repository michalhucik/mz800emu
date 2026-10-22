/**
 * @file ide8_request.c
 * @brief Zásahy GUI do IDE8 přes emu vlákno (kontrakt v ide8_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = mechanika, i[1] = připojit,
 * str = cesta obrazu.
 */

#include <stdlib.h>

#include <glib.h>

#include "ide8.h"
#include "ide8_request.h"
#include "device_request.h"


/** @brief Zásah: připojení (i[0] mechanika, i[1] 0/1). */
static int ide8_request_exec_set_connected(const st_DEVICE_REQ_ARGS *a)
{
    ide8_drive_set_connected((en_IDE8_DRIVE)a->i[0], a->i[1] ? IDE8_STATE_CONNECTED : IDE8_STATE_DISCONNECTED);
    return 0;
}


uint32_t ide8_request_set_connected(int drive_id, int connected)
{
    if (drive_id != IDE8_DRIVE_MASTER && drive_id != IDE8_DRIVE_SLAVE) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = drive_id;
    a.i[1] = connected ? 1 : 0;
    return device_request_submit("ide8_set_connected", ide8_request_exec_set_connected, &a);
}


/** @brief Zásah: nový obraz (i[0] mechanika, str cesta). */
static int ide8_request_exec_set_image(const st_DEVICE_REQ_ARGS *a)
{
    st_IDE8_DRIVE *drive = &g_ide8.drive[a->i[0]];
    if (drive->connected) {
        ide8_drive_close_image(drive);
    }
    char *path = g_strdup(a->str);
    if (EXIT_SUCCESS != ide8_drive_open_image(drive, path)) {
        g_free(path);
        return -1;
    }
    /* Dřívější callback cestu jen přepsal a původní řetězec ztratil.
     * `filepath` alokuje cfgfile (malloc) nebo tento zásah (g_malloc);
     * g_free() uvolní obojí (GLib >= 2.46 používá systémový alokátor). */
    g_free(drive->filepath);
    drive->filepath = path;
    return 0;
}


uint32_t ide8_request_set_image(int drive_id, const char *path)
{
    if (drive_id != IDE8_DRIVE_MASTER && drive_id != IDE8_DRIVE_SLAVE) return 0;
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    a.i[0] = drive_id;
    return device_request_submit("ide8_set_image", ide8_request_exec_set_image, &a);
}
