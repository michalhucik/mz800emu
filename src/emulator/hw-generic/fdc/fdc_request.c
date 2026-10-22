/**
 * @file fdc_request.c
 * @brief Zásahy GUI do FDC přes emu vlákno (kontrakt v fdc_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = index řadiče, i[1] =
 * mechanika, i[2] = hodnota volby, i[3] = příznak (save_first), str =
 * cesta nebo režim úložiště.
 */

#include <string.h>

#include <glib.h>

#include "fdc.h"
#include "fdc_request.h"
#include "device_request.h"
#include "baseui/baseui.h"


/**
 * @brief Ověří index řadiče a mechaniky.
 * @param fdc_index Index řadiče.
 * @param drive_id  Mechanika.
 * @return true = obojí v rozsahu.
 */
static bool fdc_request_valid(unsigned fdc_index, unsigned drive_id)
{
    return fdc_index < FDC_INSTANCE_COUNT && drive_id < FDC_NUM_DRIVES;
}


/**
 * @brief Remount nasazeného obrazu (nová volba read-only / úložiště).
 * @param fdc      Řadič.
 * @param drive_id Mechanika.
 * @pre Emu vlákno (zásah).
 */
static void fdc_request_remount(st_FDC *fdc, unsigned drive_id)
{
    if (!FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id)) return;
    /* Kopie: remount přepisuje `filename` mechaniky. */
    char *pathdup = g_strdup(fdc_get_dsk_filepath(fdc, drive_id));
    if (pathdup) {
        fdc_mount_dskfile(fdc, drive_id, pathdup);
        g_free(pathdup);
    }
}


/** @brief Zásah: mount DSK (i[0] řadič, i[1] mechanika, str cesta). */
static int fdc_request_exec_mount(const st_DEVICE_REQ_ARGS *a)
{
    st_FDC *fdc = &g_fdc[a->i[0]];
    unsigned drive_id = (unsigned)a->i[1];

    if (fdc->connected != FDC_CONNECTED) {
        baseui_show_message(0, "Can't mount DSK image into FDC%u FD%u, because the FD controller is not connected.",
                            fdc->index, drive_id);
        return -1;
    }
    if (strlen(a->str) >= sizeof(fdc->drive[drive_id].filename)) {
        baseui_show_message(0, "Sorry, filepath is too big (%d).", (int)strlen(a->str));
        return -1;
    }
    char *path = g_strdup(a->str);
    fdc_mount_dskfile(fdc, drive_id, path);
    g_free(path);
    return 0;
}


uint32_t fdc_request_mount(unsigned fdc_index, unsigned drive_id, const char *path)
{
    if (!fdc_request_valid(fdc_index, drive_id) || !path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    a.i[0] = (int)fdc_index;
    a.i[1] = (int)drive_id;
    return device_request_submit("fdc_mount", fdc_request_exec_mount, &a);
}


/** @brief Zásah: umount (i[0] řadič, i[1] mechanika). */
static int fdc_request_exec_umount(const st_DEVICE_REQ_ARGS *a)
{
    fdc_umount(&g_fdc[a->i[0]], (unsigned)a->i[1]);
    return 0;
}


uint32_t fdc_request_umount(unsigned fdc_index, unsigned drive_id)
{
    if (!fdc_request_valid(fdc_index, drive_id)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[1] = (int)drive_id;
    return device_request_submit("fdc_umount", fdc_request_exec_umount, &a);
}


/** @brief Zásah: read-only (i[0] řadič, i[1] mechanika, i[2] hodnota). */
static int fdc_request_exec_set_readonly(const st_DEVICE_REQ_ARGS *a)
{
    st_FDC *fdc = &g_fdc[a->i[0]];
    unsigned drive_id = (unsigned)a->i[1];
    fdc_cfg_set_readonly(fdc, drive_id, a->i[2] ? 1 : 0);
    fdc_request_remount(fdc, drive_id);
    return 0;
}


uint32_t fdc_request_set_readonly(unsigned fdc_index, unsigned drive_id, int readonly)
{
    if (!fdc_request_valid(fdc_index, drive_id)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[1] = (int)drive_id;
    a.i[2] = readonly ? 1 : 0;
    return device_request_submit("fdc_set_readonly", fdc_request_exec_set_readonly, &a);
}


/** @brief Zásah: režim úložiště (i[0] řadič, i[1] mechanika, i[3] save_first, str režim). */
static int fdc_request_exec_set_storage_mode(const st_DEVICE_REQ_ARGS *a)
{
    st_FDC *fdc = &g_fdc[a->i[0]];
    unsigned drive_id = (unsigned)a->i[1];
    int rc = 0;
    if (a->i[3] && !fdc_drive_force_save_to_file(fdc, drive_id)) rc = -1;
    fdc_cfg_set_storage_mode(fdc, drive_id, a->str);
    fdc_request_remount(fdc, drive_id);
    return rc;
}


uint32_t fdc_request_set_storage_mode(unsigned fdc_index, unsigned drive_id,
                                      const char *mode, bool save_first)
{
    if (!fdc_request_valid(fdc_index, drive_id) || !mode) return 0;
    if (strcmp(mode, "cached") != 0 && strcmp(mode, "direct") != 0 && strcmp(mode, "discard") != 0)
        return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, mode);
    a.i[0] = (int)fdc_index;
    a.i[1] = (int)drive_id;
    a.i[3] = save_first ? 1 : 0;
    return device_request_submit("fdc_set_storage_mode", fdc_request_exec_set_storage_mode, &a);
}


/** @brief Zásah: sync (i[0] řadič, i[1] mechanika). */
static int fdc_request_exec_sync(const st_DEVICE_REQ_ARGS *a)
{
    return fdc_sync_drive(&g_fdc[a->i[0]], (unsigned)a->i[1]) ? 0 : -1;
}


uint32_t fdc_request_sync(unsigned fdc_index, unsigned drive_id)
{
    if (!fdc_request_valid(fdc_index, drive_id)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[1] = (int)drive_id;
    return device_request_submit("fdc_sync", fdc_request_exec_sync, &a);
}


/** @brief Zásah: HD Patch (i[0] řadič, i[2] hodnota). */
static int fdc_request_exec_set_hd_patch(const st_DEVICE_REQ_ARGS *a)
{
    g_fdc[a->i[0]].hd_patch = a->i[2] ? 1 : 0;
    return 0;
}


uint32_t fdc_request_set_hd_patch(unsigned fdc_index, int enabled)
{
    if (!fdc_request_valid(fdc_index, 0)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[2] = enabled ? 1 : 0;
    return device_request_submit("fdc_set_hd_patch", fdc_request_exec_set_hd_patch, &a);
}


/** @brief Zásah: BUS translation (i[0] řadič, i[2] hodnota). */
static int fdc_request_exec_set_bus_xlate(const st_DEVICE_REQ_ARGS *a)
{
    g_fdc[a->i[0]].bus_xlate = (en_FDC_BUS_XLATE)a->i[2];
    return 0;
}


uint32_t fdc_request_set_bus_xlate(unsigned fdc_index, en_FDC_BUS_XLATE xlate)
{
    if (!fdc_request_valid(fdc_index, 0)) return 0;
    if (xlate != FDC_BUS_XLATE_INVERT && xlate != FDC_BUS_XLATE_PASSTHROUGH) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[2] = (int)xlate;
    return device_request_submit("fdc_set_bus_xlate", fdc_request_exec_set_bus_xlate, &a);
}


/** @brief Zásah: připojení řadiče (i[0] řadič, i[2] hodnota). */
static int fdc_request_exec_set_connected(const st_DEVICE_REQ_ARGS *a)
{
    g_fdc[a->i[0]].connected = a->i[2] ? FDC_CONNECTED : FDC_DISCONNECTED;
    return 0;
}


uint32_t fdc_request_set_connected(unsigned fdc_index, int connected)
{
    if (!fdc_request_valid(fdc_index, 0)) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = (int)fdc_index;
    a.i[2] = connected ? 1 : 0;
    return device_request_submit("fdc_set_connected", fdc_request_exec_set_connected, &a);
}
