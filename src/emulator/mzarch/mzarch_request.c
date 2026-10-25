/**
 * @file mzarch_request.c
 * @brief Zásahy GUI do přepínačů stroje přes emu vlákno (kontrakt v mzarch_request.h).
 *
 * Argumenty zásahu (st_DEVICE_REQ_ARGS): i[0] = hodnota volby.
 */

#include "mzarch_request.h"
#include "mzarch.h"
#include "device_request.h"
#include "mzarch_platform_functions.h"
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "../debugger/dbgapi_cmdrq.h"
#endif

#if MZARCH == 800
#include "mz800/mz800_main.h"
#endif


/** @brief Zásah: poloha SW1 (i[0] en_MZ800_MODE_SW). */
static int mzarch_request_exec_mode_sw_set(const st_DEVICE_REQ_ARGS *a)
{
    mzarch_mode_sw_set((en_MZ800_MODE_SW)a->i[0]);
    return 0;
}


uint32_t mzarch_request_mode_sw_set(int mode)
{
    if (mode != MZ800_MODE_SW_MZ700 && mode != MZ800_MODE_SW_MZ800) return 0;
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = mode;
    return device_request_submit("mode_sw_set", mzarch_request_exec_mode_sw_set, &a);
}


#if MZARCH == 800
/** @brief Zásah: druhý PSG (i[0] 0/1). */
static int mzarch_request_exec_allow_psg1(const st_DEVICE_REQ_ARGS *a)
{
    mz800_mz800_hwcompat_allow_psg1(a->i[0] ? 1u : 0u);
    return 0;
}


uint32_t mzarch_request_allow_psg1(bool allow)
{
    st_DEVICE_REQ_ARGS a;
    device_request_args_set_str(&a, NULL);
    a.i[0] = allow ? 1 : 0;
    return device_request_submit("allow_psg1", mzarch_request_exec_allow_psg1, &a);
}
#endif /* MZARCH == 800 */


void mzarch_request_user_reset(void)
{
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    dbgapi_note_user_action("reset");
#endif
    mzarch_platform_fn_reset_request();
}
