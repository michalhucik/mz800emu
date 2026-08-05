/*
 * File:   iface_keyboard.c
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
#include <stdint.h>
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "main.h"
#include "iface_keyboard.h"
#include "hw-generic/pio8255/pio8255.h"

#include "hw-generic/joy/joy.h"

static inline void iface_keyboard_scan_col0(const bool *state)
{
    /* BLANK GRAPH LIBRA ALPHA TAB ; : CR */

    if (state[SDL_SCANCODE_GRAVE])
    {
        PIO8255_MZKEYBIT_RESET(0, 7); /* BLANK */
    };
    if (state[SDL_SCANCODE_CAPSLOCK])
    {
        PIO8255_MZKEYBIT_RESET(0, 6); /* GRAPH */
    };
    if (state[SDL_SCANCODE_F9])
    {
        PIO8255_MZKEYBIT_RESET(0, 5); /* LIBRA */
    };
    /* ALPHA: obě hostitelské klávesy generující "\" - BACKSLASH (ImGui 604)
     * i NONUSBACKSLASH (ImGui Oem102 631, ISO klávesa, na ANSI klávesnicích
     * obvykle u levého Shiftu / dolního Enteru). Obě plní funkci ALPHA. */
    if ((state[SDL_SCANCODE_BACKSLASH]) || (state[SDL_SCANCODE_NONUSBACKSLASH]))
    {
        PIO8255_MZKEYBIT_RESET(0, 4); /* ALPHA */
    };
    /* Hostitelská TAB: na HW s klávesou TAB (MZ-800) bit (0, 3), jinak
     * (MZ-700/MZ-1500 TAB nemají) se aplikuje jako ALPHA (0, 4). */
    if (state[SDL_SCANCODE_TAB])
    {
        if (g_mzhal.has_key_tab)
        {
            PIO8255_MZKEYBIT_RESET(0, 3); /* TAB */
        }
        else
        {
            PIO8255_MZKEYBIT_RESET(0, 4); /* ALPHA */
        };
    };
    if ((state[SDL_SCANCODE_SEMICOLON]) || (state[SDL_SCANCODE_KP_PLUS]))
    {
        PIO8255_MZKEYBIT_RESET(0, 2); /* ; */
    };
    if ((state[SDL_SCANCODE_APOSTROPHE]) || (state[SDL_SCANCODE_KP_MULTIPLY]))
    {
        PIO8255_MZKEYBIT_RESET(0, 1); /* : */
    };
    /* samotny RETURN udajne zlobi na nejakem notebooku s winXP pri aktivnim NumLock */
    if ((state[SDL_SCANCODE_RETURN]) ||
        (state[SDL_SCANCODE_RETURN2]) ||
        (state[SDL_SCANCODE_KP_ENTER]))
    {
        PIO8255_MZKEYBIT_RESET(0, 0); /* CR */
    };
}

static inline void iface_keyboard_scan_col1(const bool *state)
{
    /* Y Z @ [ ] */

    if (state[SDL_SCANCODE_Y])
    {
        PIO8255_MZKEYBIT_RESET(1, 7); /* Y */
    };
    if (state[SDL_SCANCODE_Z])
    {
        PIO8255_MZKEYBIT_RESET(1, 6); /* Z */
    };
    if (state[SDL_SCANCODE_F6])
    {
        PIO8255_MZKEYBIT_RESET(1, 5); /* @ */
    };
    if (state[SDL_SCANCODE_LEFTBRACKET])
    {
        PIO8255_MZKEYBIT_RESET(1, 4); /* [ */
    };
    if (state[SDL_SCANCODE_RIGHTBRACKET])
    {
        PIO8255_MZKEYBIT_RESET(1, 3); /* ] */
    };
}

static inline void iface_keyboard_scan_col2(const bool *state)
{
    /* Q R S T U V W X */

    if (state[SDL_SCANCODE_Q])
    {
        PIO8255_MZKEYBIT_RESET(2, 7); /* Q */
    };
    if (state[SDL_SCANCODE_R])
    {
        PIO8255_MZKEYBIT_RESET(2, 6); /* R */
    };
    if (state[SDL_SCANCODE_S])
    {
        PIO8255_MZKEYBIT_RESET(2, 5); /* S */
    };
    if (state[SDL_SCANCODE_T])
    {
        PIO8255_MZKEYBIT_RESET(2, 4); /* T */
    };
    if (state[SDL_SCANCODE_U])
    {
        PIO8255_MZKEYBIT_RESET(2, 3); /* U */
    };
    if (state[SDL_SCANCODE_V])
    {
        PIO8255_MZKEYBIT_RESET(2, 2); /* V */
    };
    if (state[SDL_SCANCODE_W])
    {
        PIO8255_MZKEYBIT_RESET(2, 1); /* W */
    };
    if (state[SDL_SCANCODE_X])
    {
        PIO8255_MZKEYBIT_RESET(2, 0); /* X */
    };
}

static inline void iface_keyboard_scan_col3(const bool *state)
{
    /* I J K L M N O P */

    if (state[SDL_SCANCODE_I])
    {
        PIO8255_MZKEYBIT_RESET(3, 7); /* I */
    };
    if (state[SDL_SCANCODE_J])
    {
        PIO8255_MZKEYBIT_RESET(3, 6); /* J */
    };
    if (state[SDL_SCANCODE_K])
    {
        PIO8255_MZKEYBIT_RESET(3, 5); /* K */
    };
    if (state[SDL_SCANCODE_L])
    {
        PIO8255_MZKEYBIT_RESET(3, 4); /* L */
    };
    if (state[SDL_SCANCODE_M])
    {
        PIO8255_MZKEYBIT_RESET(3, 3); /* M */
    };
    if (state[SDL_SCANCODE_N])
    {
        PIO8255_MZKEYBIT_RESET(3, 2); /* N */
    };
    if (state[SDL_SCANCODE_O])
    {
        PIO8255_MZKEYBIT_RESET(3, 1); /* O */
    };
    if (state[SDL_SCANCODE_P])
    {
        PIO8255_MZKEYBIT_RESET(3, 0); /* P */
    };
}

static inline void iface_keyboard_scan_col4(const bool *state)
{
    /* A B C D E F G H  */

    if (state[SDL_SCANCODE_A])
    {
        PIO8255_MZKEYBIT_RESET(4, 7); /* A */
    };
    if (state[SDL_SCANCODE_B])
    {
        PIO8255_MZKEYBIT_RESET(4, 6); /* B */
    };
    if (state[SDL_SCANCODE_C])
    {
        PIO8255_MZKEYBIT_RESET(4, 5); /* C */
    };
    if (state[SDL_SCANCODE_D])
    {
        PIO8255_MZKEYBIT_RESET(4, 4); /* D */
    };
    if (state[SDL_SCANCODE_E])
    {
        PIO8255_MZKEYBIT_RESET(4, 3); /* E */
    };
    if (state[SDL_SCANCODE_F])
    {
        PIO8255_MZKEYBIT_RESET(4, 2); /* F */
    };
    if (state[SDL_SCANCODE_G])
    {
        PIO8255_MZKEYBIT_RESET(4, 1); /* G */
    };
    if (state[SDL_SCANCODE_H])
    {
        PIO8255_MZKEYBIT_RESET(4, 0); /* H */
    };
}

static inline void iface_keyboard_scan_col5(const bool *state, SDL_Keymod kmod)
{
    /* 1 2 3 4 5 6 7 8 */

    if ((state[SDL_SCANCODE_1]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_1])))
    {
        PIO8255_MZKEYBIT_RESET(5, 7); /* 1 */
    };
    if ((state[SDL_SCANCODE_2]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_2])))
    {
        PIO8255_MZKEYBIT_RESET(5, 6); /* 2 */
    };
    if ((state[SDL_SCANCODE_3]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_3])))
    {
        PIO8255_MZKEYBIT_RESET(5, 5); /* 3 */
    };
    if ((state[SDL_SCANCODE_4]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_4])))
    {
        PIO8255_MZKEYBIT_RESET(5, 4); /* 4 */
    };
    if ((state[SDL_SCANCODE_5]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_5])))
    {
        PIO8255_MZKEYBIT_RESET(5, 3); /* 5 */
    };
    if ((state[SDL_SCANCODE_6]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_6])))
    {
        PIO8255_MZKEYBIT_RESET(5, 2); /* 6 */
    };
    if ((state[SDL_SCANCODE_7]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_7])))
    {
        PIO8255_MZKEYBIT_RESET(5, 1); /* 7 */
    };
    if ((state[SDL_SCANCODE_8]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_8])))
    {
        PIO8255_MZKEYBIT_RESET(5, 0); /* 8 */
    };
}

static inline void iface_keyboard_scan_col6(const bool *state, SDL_Keymod kmod)
{
    /* \ ~ - SPACE 0 9 , . */

    if (state[SDL_SCANCODE_F7])
    {
        PIO8255_MZKEYBIT_RESET(6, 7); /* \ */
    };
    if (state[SDL_SCANCODE_EQUALS])
    {
        PIO8255_MZKEYBIT_RESET(6, 6); /* ~ */
    };
    if ((state[SDL_SCANCODE_MINUS]) || (state[SDL_SCANCODE_KP_MINUS]))
    {
        PIO8255_MZKEYBIT_RESET(6, 5); /* - */
    };
    if (state[SDL_SCANCODE_SPACE])
    {
        PIO8255_MZKEYBIT_RESET(6, 4); /* SPACE */
    };
    if ((state[SDL_SCANCODE_0]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_0])))
    {
        PIO8255_MZKEYBIT_RESET(6, 3); /* 0 */
    };
    if ((state[SDL_SCANCODE_9]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_9])))
    {
        PIO8255_MZKEYBIT_RESET(6, 2); /* 9 */
    };
    if (state[SDL_SCANCODE_COMMA])
    {
        PIO8255_MZKEYBIT_RESET(6, 1); /* , */
    };
    if ((state[SDL_SCANCODE_PERIOD]) || ((kmod & SDL_KMOD_NUM) && (state[SDL_SCANCODE_KP_PERIOD])))
    {
        PIO8255_MZKEYBIT_RESET(6, 0); /* . */
    };
}

static inline void iface_keyboard_scan_col7(const bool *state, SDL_Keymod kmod)
{
    /* INST DEL UP DOWN RIGHT LEFT ? / */

    if ((state[SDL_SCANCODE_INSERT]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_0])))
    {
        PIO8255_MZKEYBIT_RESET(7, 7); /* INSERT */
    };
    if ((state[SDL_SCANCODE_DELETE]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_PERIOD])))
    {
        PIO8255_MZKEYBIT_RESET(7, 6); /* DELETE */
    };
    if (state[SDL_SCANCODE_BACKSPACE])
    {
        PIO8255_MZKEYBIT_RESET(7, 6); /* DELETE */
    };
    if ((state[SDL_SCANCODE_UP]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_8])))
    {
        PIO8255_MZKEYBIT_RESET(7, 5); /* UP */
    };
    if ((state[SDL_SCANCODE_DOWN]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_2])))
    {
        PIO8255_MZKEYBIT_RESET(7, 4); /* DOWN */
    };
    if ((state[SDL_SCANCODE_RIGHT]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_6])))
    {
        PIO8255_MZKEYBIT_RESET(7, 3); /* RIGHT */
    };
    if ((state[SDL_SCANCODE_LEFT]) || ((!(kmod & SDL_KMOD_NUM)) && (state[SDL_SCANCODE_KP_4])))
    {
        PIO8255_MZKEYBIT_RESET(7, 2); /* LEFT */
    };
    if (state[SDL_SCANCODE_F8])
    {
        PIO8255_MZKEYBIT_RESET(7, 1); /* ? */
    };
    if ((state[SDL_SCANCODE_SLASH]) || (state[SDL_SCANCODE_KP_DIVIDE]))
    {
        PIO8255_MZKEYBIT_RESET(7, 0); /* / */
    };
}

static inline void iface_keyboard_scan_col8(const bool *state)
{
    /* ESC CTRL SHIFT */

    if (state[SDL_SCANCODE_ESCAPE])
    {
        PIO8255_MZKEYBIT_RESET(8, 7); /* ESC */
    };
    if (state[SDL_SCANCODE_END])
    {
        PIO8255_MZKEYBIT_RESET(8, 7); /* END */
    };
    if (state[SDL_SCANCODE_LCTRL])
    {
        PIO8255_MZKEYBIT_RESET(8, 6); /* CTRL */
    };
    if (state[SDL_SCANCODE_LSHIFT])
    {
        PIO8255_MZKEYBIT_RESET(8, 0); /* SHIFT */
    };
    if (state[SDL_SCANCODE_RSHIFT])
    {
        PIO8255_MZKEYBIT_RESET(8, 0); /* SHIFT */
    };
    if (state[SDL_SCANCODE_KP_PLUS])
    {
        PIO8255_MZKEYBIT_RESET(8, 0); /* SHIFT + ; = PLUS */
    };
    if (state[SDL_SCANCODE_KP_MULTIPLY])
    {
        PIO8255_MZKEYBIT_RESET(8, 0); /* SHIFT + : = * */
    };
}

static inline void iface_keyboard_scan_col9(const bool *state)
{
    /* F1 F2 F3 F4 F5 */

    if (state[SDL_SCANCODE_F1])
    {
        PIO8255_MZKEYBIT_RESET(9, 7); /* F1 */
    };
    if (state[SDL_SCANCODE_F2])
    {
        PIO8255_MZKEYBIT_RESET(9, 6); /* F2 */
    };
    if (state[SDL_SCANCODE_F3])
    {
        PIO8255_MZKEYBIT_RESET(9, 5); /* F3 */
    };
    if (state[SDL_SCANCODE_F4])
    {
        PIO8255_MZKEYBIT_RESET(9, 4); /* F4 */
    };
    if (state[SDL_SCANCODE_F5])
    {
        PIO8255_MZKEYBIT_RESET(9, 3); /* F5 */
    };
}

static inline void iface_keyboard_joy_num_keypad_scan(const bool *state, uint8_t *joystate)
{
    if (state[SDL_SCANCODE_KP_8])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_UP);
    };
    if (state[SDL_SCANCODE_KP_2])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_DOWN);
    };
    if (state[SDL_SCANCODE_KP_4])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_LEFT);
    };
    if (state[SDL_SCANCODE_KP_6])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_RIGHT);
    };
    if (state[SDL_SCANCODE_KP_7])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_UP);
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_LEFT);
    };
    if (state[SDL_SCANCODE_KP_9])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_UP);
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_RIGHT);
    };
    if (state[SDL_SCANCODE_KP_1])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_DOWN);
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_LEFT);
    };
    if (state[SDL_SCANCODE_KP_3])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_DOWN);
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_RIGHT);
    };
    if (state[SDL_SCANCODE_KP_5])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_TRIG1);
    };
    if (state[SDL_SCANCODE_KP_0])
    {
        JOY_STATEBIT_RESET(*joystate, JOY_STATEBIT_TRIG2);
    };
}

void iface_keyboard_full_scan(const bool *state, SDL_Keymod kmod, int numkeys)
{
    if ((state[SDL_SCANCODE_LALT]) || (state[SDL_SCANCODE_RALT]))
        return;

    pio8255_keyboard_matrix_reset();

    if (!numkeys)
        return;

    iface_keyboard_scan_col0(state);
    iface_keyboard_scan_col1(state);
    iface_keyboard_scan_col2(state);
    iface_keyboard_scan_col3(state);
    iface_keyboard_scan_col4(state);
    iface_keyboard_scan_col5(state, kmod);
    iface_keyboard_scan_col6(state, kmod);
    iface_keyboard_scan_col7(state, kmod);
    iface_keyboard_scan_col8(state);
    iface_keyboard_scan_col9(state);

    int device;
    for (device = 0; device < JOY_DEVID_COUNT; device++)
    {
        if (g_joy.dev[device].type == JOY_TYPE_NUM_KEYPAD)
        {
            joy_reset_dev_state(device);
            iface_keyboard_joy_num_keypad_scan(state, &g_joy.dev[device].state);
            break;
        };
    };
}
