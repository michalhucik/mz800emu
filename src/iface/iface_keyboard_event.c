/*
 * File:   iface_keyboard_event.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 21. června 2015, 22:45
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */
#include "emulator/mzarch/mzhal.h"

#include <stdio.h>
#ifdef WINDOWS
#include <windows.h>
#endif
#include "mzarch/mzarch_platform_functions.h"
#include "iface_keyboard.h"
#include "iface_video.h"

#include "iface/iface_joy.h"

#include "hw-generic/fdc/fdc.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
#include "debugger/debugger.h"
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

iface_keyboard_state_t g_iface_kbdstate = {0};

static inline int iface_keyboard_keydown_in_development_mode(SDL_Keycode scancode)
{
    (void)scancode;
#if 0
    if ( scancode == SDL_SCANCODE_F10 ) {
        printf ( "F10 - INTERRUPT\n" );
        /* z80ex_int() jiz neni k dispozici - pouzit nove CPU API */
        printf ( "Interrupt test not available!\n" );
        return 1;
    };
#endif
#if 0
    if ( event.key.keysym.scancode == SDL_SCANCODE_F11 ) {
        if ( g_mz800_main.debug_pc == 0 ) {
            printf ( "Turn ON debug PC\n" );
        } else {
            printf ( "Turn OFF debug PC\n" );
        };
        g_mz800_main.debug_pc = ~g_mz800_main.debug_pc & 1;
        return 1;
    };
#endif
    return 0;
}

static inline void iface_keyboard_keydown_hotkeys(SDL_Keycode scancode)
{
    /*
     *
     *  Obsluha klavesovych zkratek ALT+xx
     *
     */
    if (g_iface_kbdstate.lalt || g_iface_kbdstate.ralt)
    {

        if (scancode == SDL_SCANCODE_M)
        {
            /*
             * Alt + M: switch between MAX and ( NORMAL or CUSTOM )
             * Alt + Shift + M : switch between CUSTOM and ( NORMAL or MAX )
             */
            if (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift)
            {
                // switch between CUSTOM and ( NORMAL or MAX )
                emulator_switch_to_custom_speed();
            }
            else
            {
                // switch between MAX and ( NORMAL or CUSTOM )
                emulator_max_speed(!EMULATOR_TEST_MAX_SPEED);
            };
        }
        else if (scancode == SDL_SCANCODE_P)
        {
            /*
             * Alt + N: Force normal speed (100%)
             */
            emulator_switch_to_normal_speed();
        }
        else if (scancode == SDL_SCANCODE_P)
        {
            /*
             * Alt + P: Pause/Resume emulation
             */
            emulator_pause(!EMULATOR_TEST_PAUSED);
        }
        else if (scancode == SDL_SCANCODE_W)
        {
            /*
             * Fix window aspect ratio by With: Alt + W
             */
            if (g_iface_video_callbacks->fix_window_aspect_ratio)
                g_iface_video_callbacks->fix_window_aspect_ratio('W');
        }
        else if (scancode == SDL_SCANCODE_H)
        {
            /*
             * Fix window aspect ratio by Height: Alt + H
             */
            if (g_iface_video_callbacks->fix_window_aspect_ratio)
                g_iface_video_callbacks->fix_window_aspect_ratio('H');
        }

        else if (scancode == SDL_SCANCODE_UP)
        {
            /*
             * Speed up 1%: Alt + Up
             * Speed up 10%: Alt + Shift + Up
             */
            int step = (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift) ? 10 : 1;
            customspeed_step_up_request(step);
        }
        else if (scancode == SDL_SCANCODE_DOWN)
        {
            /*
             * Speed down 1%: Alt + Down
             * Speed down 10%: Alt + Shift + Down
             */
            int step = (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift) ? 10 : 1;
            customspeed_step_down_request(step);
        }
        else if (scancode == SDL_SCANCODE_PAGEUP)
        {
            /*
             * Speed up 100%: Alt + PgUp
             */
            customspeed_step_up_request(100);
        }
        else if (scancode == SDL_SCANCODE_PAGEDOWN)
        {
            /*
             * Speed down 100%: Alt + PgDown
             */
            customspeed_step_down_request(100);
        }

        else if (g_mzhal.have_fdc && (scancode == SDL_SCANCODE_1))
        {
            /*
             * Mount/Umount FD0 DSK image: Alt + 1
             */
            int drive_id = scancode - SDL_SCANCODE_1;
            if (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift)
            {
                printf("Eject FDC0 FD%d DSK image\n", drive_id);
                fdc_umount(&g_fdc[FDC0], drive_id);
            }
            else
            {
                printf("Select FDC0 FD%d DSK image for mount\n", drive_id);
                fdc_ui_mount(&g_fdc[FDC0], drive_id);
            };
        }
        else if (g_mzhal.have_fdc && (scancode == SDL_SCANCODE_2))
        {
            /*
             * Mount/Umount FD1 DSK image: Alt + 2
             */
            int drive_id = scancode - SDL_SCANCODE_1;
            if (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift)
            {
                printf("Eject FDC0 FD%d DSK image\n", drive_id);
                fdc_umount(&g_fdc[FDC0], drive_id);
            }
            else
            {
                printf("Select FDC0 FD%d DSK image for mount\n", drive_id);
                fdc_ui_mount(&g_fdc[FDC0], drive_id);
            };
        }
        else if (g_mzhal.have_fdc && (scancode == SDL_SCANCODE_3))
        {
            /*
             * Mount/Umount FD2 DSK image: Alt + 3
             */
            int drive_id = scancode - SDL_SCANCODE_1;
            if (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift)
            {
                printf("Eject FDC0 FD%d DSK image\n", drive_id);
                fdc_umount(&g_fdc[FDC0], drive_id);
            }
            else
            {
                printf("Select FDC0 FD%d DSK image for mount\n", drive_id);
                fdc_ui_mount(&g_fdc[FDC0], drive_id);
            };
        }
        else if (g_mzhal.have_fdc && (scancode == SDL_SCANCODE_4))
        {
            /*
             * Mount/Umount FD3 DSK image: Alt + 4
             */
            int drive_id = scancode - SDL_SCANCODE_1;
            if (g_iface_kbdstate.lshift || g_iface_kbdstate.rshift)
            {
                printf("Eject FDC0 FD%d DSK image\n", drive_id);
                fdc_umount(&g_fdc[FDC0], drive_id);
            }
            else
            {
                printf("Select FDC0 FD%d DSK image for mount\n", drive_id);
                fdc_ui_mount(&g_fdc[FDC0], drive_id);
            };
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        }
        else if (scancode == SDL_SCANCODE_D)
        {
            /*
             * Debugger window: Alt + D
             */
            debugger_show_hide_main_window_request();
#endif
        };
    };
}

void iface_keyboard_event_keydown(SDL_Keycode scancode)
{
    if (scancode == SDL_SCANCODE_F12)
    {
        mzarch_platform_fn_reset_request();
    }
    else if (scancode == SDL_SCANCODE_F11)
    {
#ifdef WINDOWS
        if (IsDebuggerPresent())
        {
            printf("Debugger is present! F11 - is now remapped as RESET.\n");
            mzarch_platform_fn_reset_request();
        }
        else
        {
            iface_joy_get_calibration();
        };
#else
        iface_joy_get_calibration();
#endif
    }
    else if (scancode == SDL_SCANCODE_LALT)
    {
        g_iface_kbdstate.lalt = 1;
    }
    else if (scancode == SDL_SCANCODE_RALT)
    {
        g_iface_kbdstate.ralt = 1;
    }
    else if (scancode == SDL_SCANCODE_LSHIFT)
    {
        g_iface_kbdstate.lshift = 1;
    }
    else if (scancode == SDL_SCANCODE_RSHIFT)
    {
        g_iface_kbdstate.rshift = 1;
    }
    else
    {
        if ((g_emulator.development_mode) && (iface_keyboard_keydown_in_development_mode(scancode)))
        {
            return;
        }
        else
        {
            iface_keyboard_keydown_hotkeys(scancode);
        };
    };
}

void iface_keyboard_event_keyup(SDL_Keycode scancode)
{
    if (scancode == SDL_SCANCODE_LALT)
    {
        g_iface_kbdstate.lalt = 0;
    }
    else if (scancode == SDL_SCANCODE_RALT)
    {
        g_iface_kbdstate.ralt = 0;
    }
    else if (scancode == SDL_SCANCODE_LSHIFT)
    {
        g_iface_kbdstate.lshift = 0;
    }
    else if (scancode == SDL_SCANCODE_RSHIFT)
    {
        g_iface_kbdstate.rshift = 0;
    };
}
