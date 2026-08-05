#include "main.h"
#ifdef WINDOWS
#include <windows.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <glib.h>
#include <setjmp.h>

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "emulator_measuring.h"
#include "cfgmain.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "display.h"
#include "mzarch/mzarch_platform_functions.h"
#include "mzarch/mzarch.h"
#include "iface/iface_video.h"
#include "iface/iface_audio.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#include "debugger/bptmap.h"
#endif

#include "generic_driver/memory_driver.h"
#include "generic_driver/file_driver.h"

#include "version_check/version_check.h"
#include "snapshot/snapshot.h"

st_EMULATOR g_emulator;

jmp_buf jumpBuffer;
int g_emulator_exit_value = 0;

void emulator_teardown(void)
{
    /* cfgmain musi byt prvni exit funkce !!! */
    version_check_exit();
    snapshot_exit();
    cfgmain_exit();

    emulator_measuring_exit();
    mzarch_platform_fn_exit();
}

void emulator_quit(int exit_value)
{
    g_emulator_exit_value = exit_value;
    if (exit_value == 0)
    {
        /* Subsystemy zde NEuklizime - emulator_teardown() vola main
         * vlakno az po g_thread_join + MCP/dbgapi shutdownu (viz
         * kontrakt v emulator.h; jinak UAF z MCP dispatch vlakna). */
        fprintf(stderr, "Application is normaly exiting...\n");
    }
    else
    {
        fprintf(stderr, "Oops ... Apppication is abnormaly exiting...\n");
#ifdef WINDOWS
        // Pokud doslo k chybe, tak nechame chvili otevrene okno, aby uzivatel mohl videt, co se stalo
        fprintf(stderr, "Waiting 5 seconds before exit...\n");
        g_usleep(5 * 1000 * 1000);
#endif
    };

    sdlapp_quit(g_sdlapp);

    longjmp(jumpBuffer, 1); // Skok zpět do mz800_main()

    while (1)
    {
    };
}

static void emulator_print_hint(void)
{

    printf("\nTips:\n");
    printf("   - use right-click on the emulator window for the Menu.\n");
    printf("   - ImGui window sizes and positions are stored in mz800emu-imgui.ini\n");

    printf("\nSome useful shortcut keys:\n");

    printf("   Alt + C                - virtual CMT\n");
    printf("   Alt + K                - virtual keyboard\n");

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    printf("   Alt + D                - debugger\n");
    printf("   Alt + B                - breakpoints\n");
    printf("   Alt + E                - memory browser\n");
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

    printf("   Alt + M                - switch MAX / (NORMAL or CUSTOM) speed\n");
    printf("   Alt + Shift + M        - switch CUSTOM / (NORMAL or MAX) speed\n");
    printf("   Alt + N                - set NORMAL speed\n");

    printf("   Alt + [Up/Dwn]         - inc. / dec. CUSTOM speed by 1 %%\n");
    printf("   Alt + Shift + [Up/Dwn] - inc. / dec. CUSTOM speed by 10 %%\n");
    printf("   Alt + [PgUp/PgDwn]     - inc. / dec. CUSTOM speed by 100 %%\n");

    printf("   Alt + P                - switch Paused / Runnning\n");
    printf("   Alt + [1..4]           - mount FD image\n"); // Pokud je FDC odpojeno, tak se automaticky zapne WD279x
    printf("   Alt + Shift + [1..4]   - umount FD image\n");
    printf("   Alt + Ctrl + Enter     - switch fullscreen\n");

#ifdef WINDOWS
    if (IsDebuggerPresent())
    {
        printf("   F11, F12               - reset\n");
    }
    else
    {
        printf("   F12                    - reset\n");
    };
#else  /* WINDOWS */
    printf("   F12                    - reset\n");
#endif /* !WINDOWS */

    printf("\n");
}

static void emulator_init(void)
{
    memset(&g_emulator, 0, sizeof(st_EMULATOR));

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "EMULATOR");
    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "development_mode", CFGENTYPE_BOOL, false);

    cfgelement_set_handlers(elm, (void *)&g_emulator.development_mode, (void *)&g_emulator.development_mode);

    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

gpointer emulator_thread(gpointer ptr)
{
    (void)ptr;

    if (setjmp(jumpBuffer) == 0)
    {
        // Tady se dostaneme pouze pri prvnim volani

        // g_print("%s() - Start\n", __func__);

        APP_MUTEX_LOCK(g_iface_video->is_initialized_mutex);
        if (!g_iface_video->is_initialized)
        {
            g_print("%s() - Waiting for video interface initialization...\n", __func__);
            /* Smyčka musí vyskočit i při vyžádaném ukončení (sdlapp_quit
             * před rozběhem sdlapp_run): bez kontroly quit_requested by se
             * čekalo na running==TRUE, které už nikdy nepřijde (running a
             * "quit požadován" jsou jinak nerozlišitelné - oba FALSE). */
            while ((!g_iface_video->is_initialized) && (!sdlapp_is_running(g_sdlapp)) && (!sdlapp_is_quit_requested(g_sdlapp)))
            {
                APP_COND_WAIT_TIMEOUT_MS(g_iface_video->is_initialized_cond, g_iface_video->is_initialized_mutex, 1000);
            };

            if (!sdlapp_is_running(g_sdlapp))
            {
                /* Ukončení vyžádáno ještě před inicializací emulátoru
                 * (emulator_init / snapshot_init / version_check_init /
                 * emulator_measuring_init / mzarch_platform_fn_init níže
                 * v této funkci zatím NEproběhly). Nesmíme volat
                 * emulator_quit() - ten deinicializuje právě tyto
                 * subsystémy, což by na neinicializovaném stavu
                 * spadlo. Stačí ukončit vlákno; subsystémy
                 * inicializované mimo toto vlákno uklidí volající
                 * (např. main_pipe.c cleanup). */
                g_print("%s() - Exit request detected before init!\n", __func__);
                APP_MUTEX_UNLOCK(g_iface_video->is_initialized_mutex);
                g_emulator_exit_value = EXIT_SUCCESS;
                return &g_emulator_exit_value;
            };

            g_print("%s() - Video interface initialized!\n", __func__);
        };
        APP_MUTEX_UNLOCK(g_iface_video->is_initialized_mutex);

        emulator_init();
        display_init();

        version_check_init();

        // inicializace callbacku pro generic driver
        file_driver_init();
        memory_driver_init();

        mzarch_platform_fn_init();
        snapshot_init();
        emulator_print_hint();
        if (g_iface_video_callbacks->set_window_focus)
        {
            g_iface_video_callbacks->set_window_focus();
        }
        else
        {
            WARN("Video: set window focus - callback is not implemented\n");
        };

        emulator_measuring_init();

        /* --maxspeed-bench: headless A/B režim - spusť v MAX SPEED a nech
         * sampling thread periodicky tisknout report na konzoli. */
        if (sdlapp_option_present("--maxspeed-bench"))
        {
            g_emulator_measuring.maxspeed.console_output_enabled = true;
            emulator_max_speed(true);
        };

        mzarch_main();
    }
    else
    {
        // Tady se dostaneme pri navratu z mz800_main_quit()
        // printf("%s() - end\n", __func__);
    }

    // printf("%s():%d - end done!\n", __func__, __LINE__);
    return &g_emulator_exit_value;
}

void emulator_switch_to_normal_speed(void)
{
    customspeed_store_speed();
    customspeed_set_request(100);
    emulator_max_speed(false);
}

void emulator_switch_to_custom_speed(void)
{
    customspeed_restore_speed();
    emulator_max_speed(false);
}

void emulator_max_speed(bool value)
{
    value = (value) ? true : false;
    if (value == g_emulator.max_speed)
        return;

    g_emulator.max_speed = value;

    if (EMULATOR_TEST_MAX_SPEED)
    {
        printf("Max emulation speed.\n");
    }
    else
    {
        emulator_measuring_frame_timing_reset();

        if (CUSTOMSPEED_TEST_SPEED_100)
        {
            printf("Normal emulation speed.\n");
        }
        else
        {
            customspeed_print();
        };
    };

    // MAX SPEED benchmark: otevři/uzavři měřený segment podle nové rychlosti
    emulator_measuring_maxspeed_update_segment();

    iface_audio_update_buffer_state();
}

void emulator_pause(bool value)
{
    value = (value) ? true : false;

    if (value == g_emulator.paused)
        return;

    if (value)
    {
        emulator_measuring_frame_timing_reset();
        MZ800_MAIN_SET_EVENT(MZEVENT_BREAK_EMULATION_PAUSED, 0);
    };

    g_emulator.paused = value;

    // MAX SPEED benchmark: pauza ukončuje měřený segment, unpauza jej (v MAX SPEED) obnoví
    emulator_measuring_maxspeed_update_segment();

    iface_audio_pause_emulation(g_emulator.paused);

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
    if (TEST_DEBUGGER_ACTIVE)
    {
        if (g_emulator.paused)
        {
            // zastavili jsme
            bptmap_reset_temporary_event();
        }
        else
        {
            if (g_iface_video_callbacks->set_window_focus)
            {
                g_iface_video_callbacks->set_window_focus();
            }
            else
            {
                WARN("Video: set window focus - callback is not implemented\n");
            };
        };
    };
    if (!g_emulator.paused)
    {
        g_debugger.run_to_temporary_breakpoint = 0;
    };
#endif
}

const char *emulator_get_speed_status_as_text(void)
{
    if (EMULATOR_TEST_PAUSED)
        return _("PAUSED");

    if (EMULATOR_TEST_MAX_SPEED)
        return _("Max speed");

    if (!CUSTOMSPEED_TEST_SPEED_100)
        return _("Custom speed");

    return _("Normal speed");
}