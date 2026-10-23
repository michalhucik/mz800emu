#ifndef IFACE_JOY_H
#define IFACE_JOY_H

#include "main.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "emulator/hw-generic/joy/joy.h"

/* Maximalni pocet dostupnych zarizeni v systemu */
#define IFACE_JOY_MAX_SYSDEVICES 16

/* Vychozi deadzone pro analog→digital konverzi */
#define IFACE_JOY_DEFAULT_DEADZONE 8000

typedef struct st_IFACE_JOY_SYSDEVICE
{
    SDL_JoystickID instance_id;   /* SDL3 instance ID */
    char name[128];               /* nazev zarizeni (kopie) */
    int axes;
    int buttons;
    bool is_gamepad;              /* SDL3: SDL_IsGamepad() */
} st_IFACE_JOY_SYSDEVICE;

typedef struct st_IFACE_JOY_PARAMS
{
    /* Pro raw joystick mod */
    int x_axis;
    bool x_invert;
    int y_axis;
    bool y_invert;
    int trig1_button;
    int trig2_button;

    /* Spolecne parametry */
    Sint16 deadzone;              /* deadzone pro analog→digital konverzi */

    /* Pro gamepad mod (standardni mapovani) */
    int gamepad_trig1;            /* SDL_GAMEPAD_BUTTON_* pro trigger 1 */
    int gamepad_trig2;            /* SDL_GAMEPAD_BUTTON_* pro trigger 2 */
    bool use_dpad;                /* true=DPAD, false=levy stick */
} st_IFACE_JOY_PARAMS;

typedef struct st_IFACE_JOY_DEVICE
{
    st_IFACE_JOY_SYSDEVICE sysdevice;
    SDL_Joystick *joystick;       /* raw joystick handle */
    SDL_Gamepad *gamepad;         /* gamepad handle (NULL pokud neni gamepad) */
    st_IFACE_JOY_PARAMS params;
} st_IFACE_JOY_DEVICE;

typedef struct st_IFACE_JOY
{
    bool startup_joy_subsystem;
    st_IFACE_JOY_DEVICE dev[JOY_DEVID_COUNT];

    /* Seznam dostupnych zarizeni (aktualizuje se po rescan) */
    st_IFACE_JOY_SYSDEVICE available[IFACE_JOY_MAX_SYSDEVICES];
    int available_count;
} st_IFACE_JOY;

extern st_IFACE_JOY g_iface_joy;

#ifdef __cplusplus
extern "C"
{
#endif
    /***************************************************
     *
     * Verejne funkce obsazene v iface_joy.c
     *
     */

    extern bool iface_joy_init(void);
    extern void iface_joy_exit(void);

    /***************************************************
     *
     * Verejne funkce pozadovane v lowlevel implementaci
     *
     */

    extern bool iface_joy_lowlevel_init(void);
    extern void iface_joy_lowlevel_exit(void);

    /***************************************************
     *
     * Verejne funkce, ktere mohou byt volany z jineho modulu
     *
     */
    extern void iface_joy_get_calibration(void);

    extern int iface_joy_rescan_devices(void);
    extern void iface_joy_subsystem_init(void);
    extern void iface_joy_subsystem_shutdown(void);
    extern int iface_joy_available_realdev_count(void);
    extern int iface_joy_get_realdev_info(int idx, st_IFACE_JOY_SYSDEVICE *joy_sysdevice);
    /**
     * @brief Otevře systémové zařízení pro port joysticku (předchozí zavře).
     * @param joy_devid Port joysticku.
     * @param joy_sysdevice Zařízení ze seznamu dostupných (kopíruje se).
     * @return EXIT_SUCCESS; EXIT_FAILURE (port mimo rozsah, SDL zařízení
     *         neotevřelo - port pak zůstane bez zařízení).
     * @note UI vlákno; bere zámek zařízení modulu (blokující), iface_joy_scan()
     *       mezitím vrací poslední stav.
     */
    extern int iface_joy_open_sysdevice(en_JOY_DEVID joy_devid, st_IFACE_JOY_SYSDEVICE *joy_sysdevice);

    /**
     * @brief Zavře zařízení portu joysticku (handle SDL uvolní).
     * @param joy_devid Port joysticku.
     * @note UI vlákno nebo ukončení; bere zámek zařízení modulu (blokující).
     */
    extern void iface_joy_close_device(en_JOY_DEVID joy_devid);

    /**
     * @brief Nastaví mapování os a tlačítek portu joysticku.
     * @param joy_devid Port joysticku (mimo rozsah = nic).
     * @param params Parametry (kopírují se).
     * @note UI vlákno; bere zámek zařízení modulu (blokující).
     */
    extern void iface_joy_set_params(en_JOY_DEVID joy_devid, st_IFACE_JOY_PARAMS *params);
    extern void iface_joy_set_default_params(st_IFACE_JOY_PARAMS *params);
    extern int iface_joy_open_configured_joyid(en_JOY_DEVID joy_devid);
    /**
     * @brief Přečte stav joysticku portu jako bajt (aktivní v 0, bity en_JOY_STATEBIT).
     * @param joy_devid Port joysticku.
     * @return Stav; 0FFh bez zařízení nebo mimo rozsah. Když zámek zařízení
     *         právě drží UI (otevírá nebo zavírá zařízení), poslední stav.
     * @note Emu vlákno i UI; zámek zařízení jen zkouší (nikdy nečeká).
     */
    extern uint8_t iface_joy_scan(en_JOY_DEVID joy_devid);

#ifdef __cplusplus
}
#endif
#endif // IFACE_JOY_H
