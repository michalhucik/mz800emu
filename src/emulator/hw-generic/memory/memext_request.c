/**
 * @file memext_request.c
 * @brief Zásahy GUI do MemExt přes emu vlákno (kontrakt v memext_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): připojení i[0] = typ; načtení
 * i[0] = 1 pro FLASH, i[1] = offset v souboru, i[2] = offset v paměti,
 * i[3] = velikost, str = cesta. Offsety a velikost se vejdou do `int`
 * (paměti mají 512 KB).
 */

#include <stdio.h>
#include <string.h>

#include "memext.h"
#include "memext_request.h"
#include "device_request.h"
#include "baseui/baseui.h"


/** @brief Zásah: připojení (i[0] typ nebo MEMEXT_REQUEST_DISCONNECT). */
static int memext_request_exec_set_connection(const st_DEVICE_REQ_ARGS *a)
{
    if (a->i[0] == MEMEXT_REQUEST_DISCONNECT) {
        memext_disconnect();
    } else {
        memext_connect((en_MEMEXT_TYPE)a->i[0]);
    }
    return 0;
}


uint32_t memext_request_set_connection(int type)
{
    if (type != MEMEXT_REQUEST_DISCONNECT && type != MEMEXT_TYPE_LUFTNER && type != MEMEXT_TYPE_PEHU)
        return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = type;
    return device_request_submit("memext_set_connection", memext_request_exec_set_connection, &a);
}


/** @brief Zásah: soubor FLASH (str cesta). */
static int memext_request_exec_set_flash_filepath(const st_DEVICE_REQ_ARGS *a)
{
    memext_luftner_set_flash_filepath(a->str);
    return 0;
}


uint32_t memext_request_set_flash_filepath(const char *path)
{
    if (!path || !path[0]) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0; /* delší cestu neořezávat */
    return device_request_submit("memext_set_flash_filepath", memext_request_exec_set_flash_filepath, &a);
}


/** @brief Zásah: načtení souboru (i[0] FLASH, i[1] offset souboru, i[2] offset paměti, i[3] velikost). */
static int memext_request_exec_load(const st_DEVICE_REQ_ARGS *a)
{
    FILE *fp = fopen(a->str, "rb");
    if (!fp) {
        baseui_show_message(1, "MemExt: can't open file '%s'.", a->str);
        return -1;
    }
    uint8_t *dst = a->i[0] ? g_memext.FLASH : g_memext.RAM;
    size_t read = 0;
    if (fseek(fp, (long)a->i[1], SEEK_SET) == 0) {
        read = fread(dst + a->i[2], 1, (size_t)a->i[3], fp);
    }
    fclose(fp);
    if (read != (size_t)a->i[3]) {
        baseui_show_message(1, "MemExt: read only %u of %u bytes from '%s'.",
                            (unsigned)read, (unsigned)a->i[3], a->str);
        return -1;
    }
    return 0;
}


uint32_t memext_request_load(bool flash, const char *path, uint32_t file_offset,
                             uint32_t mem_from, uint32_t size)
{
    uint32_t mem_size = flash ? MEMEXT_FLASH_SIZE : MEMEXT_RAM_SIZE;
    if (!path || !path[0] || size == 0) return 0;
    if (mem_from >= mem_size || size > mem_size - mem_from) return 0;
    if (file_offset > (uint32_t)0x7FFFFFFF) return 0;
    st_DEVICE_REQ_ARGS a;
    if (!device_request_args_set_str(&a, path)) return 0;
    a.i[0] = flash ? 1 : 0;
    a.i[1] = (int)file_offset;
    a.i[2] = (int)mem_from;
    a.i[3] = (int)size;
    return device_request_submit("memext_load", memext_request_exec_load, &a);
}
