/*
 * iface_joy.c - Joystick interface vrstva
 *
 * Spravuje joystick zarizeni a konverzi vstupu
 * na 8-bitovy stavovy byte pro emulator.
 */

#include "main.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#include <SDL3/SDL.h>
#include "iface_joy.h"
#include "libs/sdlapp/sdl3_backend/sdl3_backend.h"


st_IFACE_JOY g_iface_joy = {0};

bool iface_joy_init(void)
{
    return iface_joy_lowlevel_init();
}

void iface_joy_exit(void)
{
    iface_joy_lowlevel_exit();
}

/* Inicializace SDL joystick subsystemu */
bool iface_joy_lowlevel_init(void)
{
    if (g_iface_joy.startup_joy_subsystem)
    {
        if (!sdl3_backend_joy_init())
        {
            g_warning("Cannot initialize SDL Joystick subsystem");
            return false;
        }
        iface_joy_rescan_devices();
    }
    return true;
}

/* Ukonceni SDL joystick subsystemu */
void iface_joy_lowlevel_exit(void)
{
    /* Zavrit vsechna otevrena zarizeni */
    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        iface_joy_close_device((en_JOY_DEVID)i);
    }
    sdl3_backend_joy_quit();
}

/* Inicializace subsystemu (volano z UI) */
void iface_joy_subsystem_init(void)
{
    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
    {
        sdl3_backend_joy_init();
    }
    g_iface_joy.startup_joy_subsystem = true;
}

/* Ukonceni subsystemu (volano z UI) */
void iface_joy_subsystem_shutdown(void)
{
    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        iface_joy_close_device((en_JOY_DEVID)i);
    }
    g_iface_joy.startup_joy_subsystem = false;
    sdl3_backend_joy_quit();
}

/* Enumerace dostupnych zarizeni */
int iface_joy_rescan_devices(void)
{
    g_iface_joy.available_count = 0;

    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
    {
        if (!sdl3_backend_joy_init())
            return 0;
    }

    int count = 0;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&count);
    if (!joysticks)
    {
        g_print("Joystick rescan: no devices found\n");
        return 0;
    }

    int result = (count < IFACE_JOY_MAX_SYSDEVICES) ? count : IFACE_JOY_MAX_SYSDEVICES;
    for (int i = 0; i < result; i++)
    {
        SDL_JoystickID id = joysticks[i];
        st_IFACE_JOY_SYSDEVICE *dev = &g_iface_joy.available[i];

        dev->instance_id = id;

        const char *name = SDL_GetJoystickNameForID(id);
        g_snprintf(dev->name, sizeof(dev->name), "%s",
                   name ? name : "Unknown Device");

        dev->is_gamepad = SDL_IsGamepad(id);
        dev->axes = 0;
        dev->buttons = 0;
    }

    SDL_free(joysticks);
    g_iface_joy.available_count = result;

    g_print("Joystick rescan: found %d device(s)\n", result);
    return result;
}

/* Pocet dostupnych zarizeni */
int iface_joy_available_realdev_count(void)
{
    return g_iface_joy.available_count;
}

/* Info o zarizeni podle indexu */
int iface_joy_get_realdev_info(int idx, st_IFACE_JOY_SYSDEVICE *out)
{
    if (idx < 0 || idx >= g_iface_joy.available_count)
        return EXIT_FAILURE;
    *out = g_iface_joy.available[idx];
    return EXIT_SUCCESS;
}

/* Nastaveni vychozich parametru mapovani */
void iface_joy_set_default_params(st_IFACE_JOY_PARAMS *params)
{
    memset(params, 0, sizeof(*params));
    params->x_axis = 0;
    params->x_invert = false;
    params->y_axis = 1;
    params->y_invert = false;
    params->trig1_button = 0;
    params->trig2_button = 1;
    params->deadzone = IFACE_JOY_DEFAULT_DEADZONE;
    params->gamepad_trig1 = SDL_GAMEPAD_BUTTON_SOUTH;
    params->gamepad_trig2 = SDL_GAMEPAD_BUTTON_EAST;
    params->use_dpad = false;
}

/* Nastaveni parametru mapovani pro dane zarizeni */
void iface_joy_set_params(en_JOY_DEVID joy_devid, st_IFACE_JOY_PARAMS *params)
{
    if (joy_devid < JOY_DEVID_COUNT)
    {
        g_iface_joy.dev[joy_devid].params = *params;
    }
}

/* Zavreni zarizeni */
void iface_joy_close_device(en_JOY_DEVID joy_devid)
{
    st_IFACE_JOY_DEVICE *dev = &g_iface_joy.dev[joy_devid];

    if (dev->gamepad)
    {
        SDL_CloseGamepad(dev->gamepad);
        dev->gamepad = NULL;
        dev->joystick = NULL; /* gamepad vlastni joystick handle */
    }
    else if (dev->joystick)
    {
        SDL_CloseJoystick(dev->joystick);
        dev->joystick = NULL;
    }
}

/* Otevreni konkretniho zarizeni */
int iface_joy_open_sysdevice(en_JOY_DEVID joy_devid, st_IFACE_JOY_SYSDEVICE *sysdev)
{
    if (joy_devid >= JOY_DEVID_COUNT)
        return EXIT_FAILURE;

    /* Zavrit pripadne predchozi zarizeni */
    iface_joy_close_device(joy_devid);

    st_IFACE_JOY_DEVICE *dev = &g_iface_joy.dev[joy_devid];
    dev->sysdevice = *sysdev;

    if (sysdev->is_gamepad)
    {
        /* Otevrit jako gamepad (standardni mapovani) */
        dev->gamepad = SDL_OpenGamepad(sysdev->instance_id);
        if (!dev->gamepad)
        {
            g_warning("Cannot open gamepad: %s", SDL_GetError());
            return EXIT_FAILURE;
        }
        /* Ziskame i joystick handle pro info o osach/tlacitkach */
        dev->joystick = SDL_GetGamepadJoystick(dev->gamepad);
    }
    else
    {
        /* Otevrit jako raw joystick */
        dev->joystick = SDL_OpenJoystick(sysdev->instance_id);
        if (!dev->joystick)
        {
            g_warning("Cannot open joystick: %s", SDL_GetError());
            return EXIT_FAILURE;
        }
    }

    /* Aktualizovat pocet os a tlacitek */
    if (dev->joystick)
    {
        dev->sysdevice.axes = SDL_GetNumJoystickAxes(dev->joystick);
        dev->sysdevice.buttons = SDL_GetNumJoystickButtons(dev->joystick);
    }

    g_print("Joystick JOY%d: opened '%s' (axes=%d, buttons=%d, gamepad=%d)\n",
            joy_devid, dev->sysdevice.name,
            dev->sysdevice.axes, dev->sysdevice.buttons,
            dev->sysdevice.is_gamepad);

    return EXIT_SUCCESS;
}

/* Otevreni driv nakonfigurovaneho zarizeni (volano pri startu z joy.c) */
int iface_joy_open_configured_joyid(en_JOY_DEVID joy_devid)
{
    /* Pokud neni subsystem inicializovany, inicializujeme */
    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
    {
        if (!sdl3_backend_joy_init())
            return EXIT_FAILURE;
    }

    /* Enumerace zarizeni pokud jeste nebyla provedena */
    if (g_iface_joy.available_count == 0)
    {
        iface_joy_rescan_devices();
    }

    /* Otevrit prvni dostupne zarizeni */
    if (g_iface_joy.available_count > 0)
    {
        st_IFACE_JOY_SYSDEVICE *sysdev = &g_iface_joy.available[0];
        iface_joy_set_default_params(&g_iface_joy.dev[joy_devid].params);
        return iface_joy_open_sysdevice(joy_devid, sysdev);
    }

    return EXIT_FAILURE;
}

/* Polling stavu joysticku — konverze na 8-bit (active-low) */
uint8_t iface_joy_scan(en_JOY_DEVID joy_devid)
{
    uint8_t state = 0xff; /* vychozi: nic nestisknuto */

    if (joy_devid >= JOY_DEVID_COUNT)
        return state;

    st_IFACE_JOY_DEVICE *dev = &g_iface_joy.dev[joy_devid];
    st_IFACE_JOY_PARAMS *p = &dev->params;

    if (dev->gamepad)
    {
        /* Gamepad mod — standardni mapovani */
        if (p->use_dpad)
        {
            /* DPAD digitalni smery */
            if (SDL_GetGamepadButton(dev->gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
                JOY_STATEBIT_RESET(state, JOY_STATEBIT_UP);
            if (SDL_GetGamepadButton(dev->gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
                JOY_STATEBIT_RESET(state, JOY_STATEBIT_DOWN);
            if (SDL_GetGamepadButton(dev->gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                JOY_STATEBIT_RESET(state, JOY_STATEBIT_LEFT);
            if (SDL_GetGamepadButton(dev->gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                JOY_STATEBIT_RESET(state, JOY_STATEBIT_RIGHT);
        }
        else
        {
            /* Levy analogovy stick s deadzone */
            Sint16 x = SDL_GetGamepadAxis(dev->gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            Sint16 y = SDL_GetGamepadAxis(dev->gamepad, SDL_GAMEPAD_AXIS_LEFTY);

            if (x < -p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_LEFT);
            if (x > p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_RIGHT);
            if (y < -p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_UP);
            if (y > p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_DOWN);
        }

        /* Triggery */
        if (SDL_GetGamepadButton(dev->gamepad, (SDL_GamepadButton)p->gamepad_trig1))
            JOY_STATEBIT_RESET(state, JOY_STATEBIT_TRIG1);
        if (SDL_GetGamepadButton(dev->gamepad, (SDL_GamepadButton)p->gamepad_trig2))
            JOY_STATEBIT_RESET(state, JOY_STATEBIT_TRIG2);
    }
    else if (dev->joystick)
    {
        /* Raw joystick mod — uzivatelsky konfigurovane mapovani */
        Sint16 x = SDL_GetJoystickAxis(dev->joystick, p->x_axis);
        Sint16 y = SDL_GetJoystickAxis(dev->joystick, p->y_axis);

        if (p->x_invert) x = (Sint16)(-x);
        if (p->y_invert) y = (Sint16)(-y);

        if (x < -p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_LEFT);
        if (x > p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_RIGHT);
        if (y < -p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_UP);
        if (y > p->deadzone) JOY_STATEBIT_RESET(state, JOY_STATEBIT_DOWN);

        if (SDL_GetJoystickButton(dev->joystick, p->trig1_button))
            JOY_STATEBIT_RESET(state, JOY_STATEBIT_TRIG1);
        if (SDL_GetJoystickButton(dev->joystick, p->trig2_button))
            JOY_STATEBIT_RESET(state, JOY_STATEBIT_TRIG2);
    }

    return state;
}

/* Autokalibrace — v SDL3 Gamepad API typicky neni potreba */
void iface_joy_get_calibration(void)
{
    /* SDL3 gamepad API provadi kalibraci automaticky */
    g_print("Joystick calibration: SDL3 gamepad API kalibruje automaticky\n");
}

