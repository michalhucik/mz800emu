/**
 * @file ramdisk_request.c
 * @brief Zásahy GUI do RAM disků přes emu vlákno (kontrakt v ramdisk_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS) podle zásahu, viz komentář u
 * každé funkce provedení. Cesta se do ramdisk_std_init() /
 * ramdisk_pezik_init() předává jako kopie z argumentů: obě funkce
 * realokují vlastní `filepath`, takže dřívější předání `g_ramdisk.*.filepath`
 * samotného by mohlo číst uvolněnou paměť.
 */

#include <string.h>

#include "ramdisk.h"
#include "ramdisk_request.h"
#include "device_request.h"


/** @brief Zásah: odpojit vše. */
static int ramdisk_request_exec_disconnect_all(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    RAMDISK_DISCONNECT_ALL();
    return 0;
}


uint32_t ramdisk_request_disconnect_all(void)
{
    return device_request_submit("ramdisk_disconnect_all", ramdisk_request_exec_disconnect_all, NULL);
}


/**
 * @brief Zásah: MZ-1R18 (i[0] typ, i[1] velikost, i[2] 1 = str platí,
 *        i[3] odpojit vše předem, str cesta).
 */
static int ramdisk_request_exec_std_set(const st_DEVICE_REQ_ARGS *a)
{
    if (a->i[3]) {
        RAMDISK_DISCONNECT_ALL();
    }
    en_RAMDISK_TYPE type = (a->i[0] == RAMDISK_REQUEST_KEEP) ? g_ramdisk.std.type : (en_RAMDISK_TYPE)a->i[0];
    en_RAMDISK_BANKMASK size = (a->i[1] == RAMDISK_REQUEST_KEEP) ? g_ramdisk.std.size : (en_RAMDISK_BANKMASK)a->i[1];

    char path[DEVICE_REQUEST_STR_MAX];
    if (a->i[2]) {
        memcpy(path, a->str, sizeof(path));
    } else {
        path[0] = '\0';
        if (g_ramdisk.std.filepath) {
            strncpy(path, g_ramdisk.std.filepath, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    }

    ramdisk_std_init(RAMDISK_CONNECTED, type, size, path);
    /* ramdisk_std_init() připojení odmítne, když je připojený Pezik E8. */
    return g_ramdisk.std.connected ? 0 : -1;
}


uint32_t ramdisk_request_std_set(int type, int size, const char *path, bool disconnect_all)
{
    if (type != RAMDISK_REQUEST_KEEP && type != RAMDISK_TYPE_STD &&
        type != RAMDISK_TYPE_SRAM && type != RAMDISK_TYPE_ROM) return 0;
    if (size != RAMDISK_REQUEST_KEEP && size != RAMDISK_SIZE_64 && size != RAMDISK_SIZE_256 &&
        size != RAMDISK_SIZE_512 && size != RAMDISK_SIZE_1M && size != RAMDISK_SIZE_16M) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    a.i[0] = type;
    a.i[1] = size;
    a.i[2] = path ? 1 : 0;
    a.i[3] = disconnect_all ? 1 : 0;
    return device_request_submit("ramdisk_std_set", ramdisk_request_exec_std_set, &a);
}


/** @brief Zásah: Pezik z menu (i[0] typ Pezika). */
static int ramdisk_request_exec_pezik_connect(const st_DEVICE_REQ_ARGS *a)
{
    RAMDISK_DISCONNECT_ALL();
    ramdisk_pezik_connect(a->i[0]);
    return g_ramdisk.pezik[a->i[0]].connected ? 0 : -1;
}


uint32_t ramdisk_request_pezik_connect(int pezik_type)
{
    if (pezik_type != RAMDISK_PEZIK_E8 && pezik_type != RAMDISK_PEZIK_68) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = pezik_type;
    return device_request_submit("ramdisk_pezik_connect", ramdisk_request_exec_pezik_connect, &a);
}


/**
 * @brief Zásah: Pezik z okna nastavení (i[0] typ, i[1] connect, i[2] maska
 *        portů, i[3] bit 0 backuped, bit 1 odpojit MZ-1R18, str cesta).
 */
static int ramdisk_request_exec_pezik_init(const st_DEVICE_REQ_ARGS *a)
{
    int pezik_type = a->i[0];
    if ((a->i[3] & 2) && a->i[1] && RAMDISK_TEST_STD_CONNECTED) {
        ramdisk_std_disconnect();
    }
    char path[DEVICE_REQUEST_STR_MAX];
    memcpy(path, a->str, sizeof(path));
    ramdisk_pezik_init(pezik_type, a->i[1] ? RAMDISK_CONNECTED : RAMDISK_DISCONNECTED, a->i[2],
                       (a->i[3] & 1) ? PEZIK_BACKUPED_YES : PEZIK_BACKUPED_NO, path);
    return 0;
}


uint32_t ramdisk_request_pezik_init(int pezik_type, int connect, int portmask, int backuped,
                                    const char *path, bool disconnect_std)
{
    if (pezik_type != RAMDISK_PEZIK_E8 && pezik_type != RAMDISK_PEZIK_68) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    a.i[0] = pezik_type;
    a.i[1] = connect ? 1 : 0;
    a.i[2] = portmask;
    a.i[3] = ((backuped == PEZIK_BACKUPED_YES) ? 1 : 0) | (disconnect_std ? 2 : 0);
    return device_request_submit("ramdisk_pezik_init", ramdisk_request_exec_pezik_init, &a);
}
