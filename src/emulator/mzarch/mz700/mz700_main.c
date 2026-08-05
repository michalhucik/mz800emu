#include "main.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "emulator.h"
#include "customspeed.h"

#include "mzarch/mzarch.h"
#include "mzarch/mzarch_platform_functions.h"
#include "libs/cpu-z80/z80.h"
#include "mz700_iorq.h"
#include "mzarch/interrupt.h"
#include "mzarch/mz700/gdg/mz700_gdg.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/cmt/cmt.h"
#include "hw-generic/pioz80/pioz80.h"
#include "hw-generic/psg/psg.h"
#include "audio.h"

#include "hw-generic/joy/joy.h"
#include "hw-generic/joy/joymz-1x03.h"

/*******************************************************************************
 *
 *
 *                  Volitelny hardware
 *                  ==================
 *
 *
 *******************************************************************************/

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#endif /* CFG_HWEXT_HAVE_FDC */

#if CFG_HWEXT_HAVE_IDE8
#include "hw-generic/ide8/ide8.h"
#endif /* CFG_HWEXT_HAVE_IDE8 */

#if CFG_HWEXT_HAVE_RAMDISK
#include "hw-generic/ramdisk/ramdisk.h"
#endif /* CFG_HWEXT_HAVE_RAMDISK */

#include "hw-generic/unicard/unicard.h"

#if CFG_HWEXT_HAVE_QDISK
#include "hw-generic/qdisk/qdisk.h"
#endif /* CFG_HWEXT_HAVE_QDISK */

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/trace/cputrack.h"
#include "debugger/trace/iorqlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/marklog.h"
#include "debugger/bptmap.h"
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

// #ifndef USE_SDL_VIDEO
// #define iface_events_pool()
// #define iface_video_update_status_line()
// #endif

// // TODO: tohle volame v mz800_main_do_emulator_paused()
// #define iface_sdl_update_window_in_beam_interval(a, b)

// nekresli okno
// #define framebuffer_screen_done()
// #define iface_sdl_pool_all_events()
// #define iface_sdl_update_window_in_beam_interval(a, b)
// nedelej framebuffer
// #define framebuffer_update_MZ700_current_screen_row()
// #define framebuffer_MZ800_current_screen_row_fill(a)
// #define framebuffer_border_current_row_fill()

void mzarch_platform_fn_exit(void)
{
    cmt_exit();

#if CFG_HWEXT_HAVE_FDC
    fdc_exit();
#endif

#if CFG_HWEXT_HAVE_QDISK
    qdisk_exit();
#endif

#if CFG_HWEXT_HAVE_RAMDISK
    ramdisc_exit();
#endif

    unicard_exit();

#if CFG_HWEXT_HAVE_IDE8
    ide8_exit();
#endif

    audio_exit();

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    debugger_exit();
#endif

#ifdef MEMORY_MAKE_STATISTICS
    memory_write_memory_statistics();
#endif

    APP_MUTEX_DESTROY(g_mzarch_main.reset_request_mutex);
}

void mzarch_platform_fn_init(void)
{
    // g_print("%s()\n", __func__);
    g_emulator.paused = false;

    g_mzarch_main.cpu = z80_create(
        memory_read_cb, NULL,
        memory_write_cb, NULL,
        port_read_cb, NULL,
        port_write_cb, NULL,
        pioz80_interrupt_ack_im2_cb, NULL);

    g_mzarch_main.cpu->external_int_handling = true;

    z80_set_reti(g_mzarch_main.cpu, pioz80_interrupt_reti_cb, NULL);

    z80_set_ei(g_mzarch_main.cpu, mzarch_ei_cb, NULL);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    /* D.3 - HW event BP CPU callbacks. */
    z80_set_di(g_mzarch_main.cpu, mzarch_di_cb, NULL);
    z80_set_im_change(g_mzarch_main.cpu, mzarch_im_cb, NULL);
    z80_set_halt(g_mzarch_main.cpu, mzarch_halt_cb, NULL);
    z80_set_nmi_cb(g_mzarch_main.cpu, mzarch_nmi_cb, NULL);
    /* V1.7+ 1.5 - unified IFF change BP callback (všech 7 reasonů). */
    z80_set_iff_change(g_mzarch_main.cpu, mzarch_iff_change_cb, NULL);
    /* Event Viewer Vlna 1 Commit 5 - CPU control eventy do eventlog
     * kategorie CPU_CTRL (HALT enter/exit, RST 00..38). */
    z80_set_cpu_ctrl_event(g_mzarch_main.cpu, mzarch_cpu_ctrl_event_cb, NULL);
#endif

    g_mzarch_main.regDBUS_latch = 0;
    g_mzarch_main.interrupt = 0;

    g_mzarch_main.instruction_addr = 0x0000;
    g_mzarch_main.instruction_tstates = 0;
    g_mzarch_main.instruction_insideop_sync_ticks = 0;

    g_mzarch_main.reset_request = false;
    APP_MUTEX_CREATE(g_mzarch_main.reset_request_mutex);

    mz800_main_cursor_timer_reset();

    customspeed_init(100);
    gdg_init(); // GDG by se mel inicializovat uplne jako prvni
    memory_init();
    ctc8253_init(); // CTC by se mel inicializovat drive, nez PIO-Z80
    pio8255_init();
    pioz80_init();

    cmt_init();

#if HAVE_PSG >= 1
    psg_init(false); /* MZ-700 by tady byl mono PSG - aktualne HAVE_PSG=0, nedostupne */
#endif
    audio_init();

#if CFG_HWEXT_HAVE_FDC
    fdc_init();
#endif

#if CFG_HWEXT_HAVE_RAMDISK
    ramdisk_init();
#endif

#if CFG_HWEXT_HAVE_QDISK
    qdisk_init();
#endif

    joy_init();

    /* MZ-1X03 joystick (analogovy, MZ-700/MZ-1500 only). */
    joymz_init();

    unicard_init();

#if CFG_HWEXT_HAVE_IDE8
    ide8_init();
#endif

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    debugger_init();
#endif

    mzarch_main_init();

    printf("\nRear dip switch - ");
#if MZARCH != 700    
    printf("Mode: %s, ", (!g_mzarch_main.switch700) ? "MZ-700" : "MZ-800");
#endif /* MZARCH != 700 */
    printf("CMT polarity: %s\n", (!g_cmt.polarity) ? "Normal" : "Inverted");

    printf("\n");
}

void mzarch_platform_fn_reset_request(void)
{
    /* Před mzarch_platform_fn_init (a po fn_exit) mutex neexistuje - UI
     * vlákno (F12, menu Reset, vkbd) může běžet dřív, než emu vlákno
     * platformu inicializuje; bez guardu by g_mutex_lock(NULL) spadl.
     * Reset bez běžícího stroje nemá co resetovat - tiše zahodíme. */
    if (g_mzarch_main.reset_request_mutex == NULL)
    {
        return;
    };
    APP_MUTEX_LOCK(g_mzarch_main.reset_request_mutex);
    g_mzarch_main.reset_request = true;
    APP_MUTEX_UNLOCK(g_mzarch_main.reset_request_mutex);
}

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
void mzarch_platform_fn_debugger_state_changed(bool active)
{
    /*
     * Pomalou cestu (with_history_cb) použijeme pokud je aktivní CPU
     * Instruction History (cpuhist) NEBO Memory Heatmap (CDL).
     *
     * trace-suite cputrack hookuje až po z80_step v mzarch_main_emulator_run,
     * nepotřebuje swap memory callbacků. Iorqlog/intlog/hwlog mají vlastní
     * hook strategii (Fáze 3-5).
     */
    /* Pořadí: nejdřív update trace-suite active (může změnit
     * TEST_DEBUGGER_NEED_DEBUG_CALLBACKS via iorqlog), pak swap callbacků. */
    cputrack_recompute_active(active);
    iorqlog_recompute_active(active);
    intlog_recompute_active(active);
    hwlog_recompute_active(active);
    marklog_recompute_active(active);

    bool need_debug = TEST_DEBUGGER_NEED_DEBUG_CALLBACKS;

    if (need_debug)
    {
        z80_set_mread(g_mzarch_main.cpu, memory_read_with_logging_cb, NULL);
        z80_set_mwrite(g_mzarch_main.cpu, memory_write_with_logging_cb, NULL);
        z80_set_pread(g_mzarch_main.cpu, port_read_with_logging_cb, NULL);
        z80_set_pwrite(g_mzarch_main.cpu, port_write_with_logging_cb, NULL);
    }
    else
    {
        z80_set_mread(g_mzarch_main.cpu, memory_read_cb, NULL);
        z80_set_mwrite(g_mzarch_main.cpu, memory_write_cb, NULL);
        z80_set_pread(g_mzarch_main.cpu, port_read_cb, NULL);
        z80_set_pwrite(g_mzarch_main.cpu, port_write_cb, NULL);
    }
}
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

