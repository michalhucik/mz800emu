/**
 * @file mz1p16_request.c
 * @brief Zásahy okna plotteru MZ-1P16 přes emu vlákno (kontrakt v mz1p16_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = hodnota volby.
 */

#include <stddef.h>

#include "mz1p16_emu.h"
#include "mz1p16_request.h"
#include "device_request.h"


/** @brief Zásah: připojení plotteru (i[0] 0/1). */
static int mz1p16_request_exec_set_active(const st_DEVICE_REQ_ARGS *a)
{
    mz1p16_emu_set_active(a->i[0] ? true : false);
    return 0;
}


uint32_t mz1p16_request_set_active(bool active)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = active ? 1 : 0;
    return device_request_submit("mz1p16_set_active", mz1p16_request_exec_set_active, &a);
}


/** @brief Zásah: reset plotteru (i[0] 1 = smazat i papír). */
static int mz1p16_request_exec_reset(const st_DEVICE_REQ_ARGS *a)
{
    mz1p16_emu_reset();
    if (a->i[0]) mz1p16_emu_clear_paper();
    return 0;
}


uint32_t mz1p16_request_reset(bool clear_paper)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = clear_paper ? 1 : 0;
    return device_request_submit("mz1p16_reset", mz1p16_request_exec_reset, &a);
}


/** @brief Zásah: tlačítko Paper feed (i[0] 1 = drženo). */
static int mz1p16_request_exec_paper_feed(const st_DEVICE_REQ_ARGS *a)
{
    mz1p16_emu_button_paper_feed(a->i[0] ? true : false);
    return 0;
}


uint32_t mz1p16_request_paper_feed(bool pressed)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = pressed ? 1 : 0;
    return device_request_submit("mz1p16_paper_feed", mz1p16_request_exec_paper_feed, &a);
}


/** @brief Zásah: kreslicí self-test (bez argumentů). */
static int mz1p16_request_exec_selftest(const st_DEVICE_REQ_ARGS *a)
{
    (void)a;
    mz1p16_emu_run_drawing_selftest();
    return 0;
}


uint32_t mz1p16_request_selftest(void)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    return device_request_submit("mz1p16_selftest", mz1p16_request_exec_selftest, &a);
}
