/**
 * @file printer_request.c
 * @brief Zásahy GUI do tiskárny přes emu vlákno (kontrakt v printer_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = hodnota volby.
 */

#include "printer.h"
#include "printer_request.h"
#include "pioz80/pioz80.h"
#include "device_request.h"


/** @brief Zásah: standard tiskárny (i[0] en_PIOZ80_PRINTER_STD). */
static int printer_request_exec_set_std(const st_DEVICE_REQ_ARGS *a)
{
    pioz80_set_printer_std((en_PIOZ80_PRINTER_STD)a->i[0]);
    return 0;
}


uint32_t printer_request_set_std(int std)
{
    if (std != PIOZ80_PRINTER_STD_CENTRONICS && std != PIOZ80_PRINTER_STD_MZ) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = std;
    return device_request_submit("printer_set_std", printer_request_exec_set_std, &a);
}


/** @brief Zásah: zachytávání (i[0] 0/1). */
static int printer_request_exec_set_active(const st_DEVICE_REQ_ARGS *a)
{
    printer_set_active(a->i[0] ? true : false);
    return 0;
}


uint32_t printer_request_set_active(bool active)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = active ? 1 : 0;
    return device_request_submit("printer_set_active", printer_request_exec_set_active, &a);
}


/** @brief Zásah: uzavření souboru zachytávání. */
static int printer_request_exec_close_file(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    printer_close_file();
    return 0;
}


uint32_t printer_request_close_file(void)
{
    return device_request_submit("printer_close_file", printer_request_exec_close_file, NULL);
}
