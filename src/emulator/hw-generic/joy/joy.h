/*
 * File:   joy.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 1. února 2018, 9:20
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

#ifndef JOY_H
#define JOY_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum en_JOY_STATEBIT
{
    JOY_STATEBIT_UP = 0,
    JOY_STATEBIT_DOWN = 1,
    JOY_STATEBIT_LEFT = 2,
    JOY_STATEBIT_RIGHT = 3,
    JOY_STATEBIT_TRIG1 = 4,
    JOY_STATEBIT_TRIG2 = 5,
} en_JOY_STATEBIT;

typedef enum en_JOY_DEVID
{
    JOY_DEVID_0 = 0,
    JOY_DEVID_1,
    JOY_DEVID_COUNT
} en_JOY_DEVID;

typedef enum en_JOY_TYPE
{
    JOY_TYPE_NONE = 0,
    JOY_TYPE_NUM_KEYPAD,
    JOY_TYPE_JOYSTICK,
} en_JOY_TYPE;

typedef struct st_JOY_DEV
{
    en_JOY_DEVID id;
    en_JOY_TYPE type;
    uint8_t state;
} st_JOY_DEV;

/**
 * @brief Čtení hostitelského joysticku (typ JOY_TYPE_JOYSTICK).
 * @param joy_devid Port.
 * @return Stav portu (bity en_JOY_STATEBIT, 0 = aktivní, 0FFh = nic).
 */
typedef uint8_t (*joy_host_read_fn)(en_JOY_DEVID joy_devid);

/**
 * @brief Stav joysticků (globální g_joy).
 *
 * `host_read` určuje, odkud joy_read_byte() bere stav portu typu
 * JOY_TYPE_JOYSTICK: výchozí je živé čtení hostitele (`host_scan`) při
 * každém IN, po dobu záznamu a přehrání vstupů (joy_host_latch()) vrací
 * `dev[].state`, který replay jednou za snímek naplní. Přepnutím ukazatele
 * se čtení IN nevětví podle režimu. `host_scan` je živý zdroj (SDL); unit
 * testy ho mohou nahradit vlastní funkcí.
 *
 * @invariant `host_read` a `host_scan` nejsou NULL (nastaví je joy_init()).
 * @note Přístup z emu vlákna (IN, bod hranice snímku replay); UI vlákno
 *       nezapisuje nic: stav joysticku z numerické klávesnice předává přes
 *       replay_host_keyboard_submit() a zapíše ho emu vlákno.
 */
typedef struct st_JOY
{
    st_JOY_DEV dev[JOY_DEVID_COUNT];
    joy_host_read_fn host_read; /**< Zdroj stavu portu typu JOYSTICK pro joy_read_byte(). */
    joy_host_read_fn host_scan; /**< Živé čtení hostitele (výchozí iface_joy_scan, v testech náhrada). */
} st_JOY;

extern st_JOY g_joy;

#define joy_reset_dev_state(joy_devid)     \
    {                                      \
        g_joy.dev[joy_devid].state = 0xff; \
    }

#define JOY_STATEBIT_RESET(state, bit) \
    {                                  \
        state &= ~(1 << bit);          \
    }

#ifdef __cplusplus
extern "C"
{
#endif
    extern void joy_init(void);
    extern uint8_t joy_read_byte(en_JOY_DEVID joy_devid);

    /**
     * @brief Živý stav hostitelského joysticku (`g_joy.host_scan`, výchozí
     *        SDL), bez ohledu na joy_host_latch().
     *
     * @param joy_devid Port.
     * @return Stav (0FFh = nic nebo build bez joysticku).
     * @note Volá emu vlákno (jako IN), stejně jako dřívější přímé čtení.
     */
    extern uint8_t joy_host_scan(en_JOY_DEVID joy_devid);

    /**
     * @brief Přepne zdroj stavu portů typu JOYSTICK.
     *
     * @param latched true = joy_read_byte() vrací `g_joy.dev[].state`
     *                (záznam a přehrání vstupů: stav plní replay jednou za
     *                snímek), false = živé čtení SDL při každém IN (výchozí).
     * @note Typ portu (INI) se nemění. Volá emu vlákno.
     */
    extern void joy_host_latch(bool latched);
#ifdef __cplusplus
}
#endif

#endif /* JOY_H */
