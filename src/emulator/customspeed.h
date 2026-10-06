#ifndef CUSTOMSPEED_H
#define CUSTOMSPEED_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include "mzarch/mzarch.h"

#define CUSTOMSPEED_MAX_VALUE 4000
typedef struct st_CUSTOMSPEED
{
    st_EMUEVENT speed_sync_event;
    unsigned previous_speed_in_percentage;   /**< predchozi rychlost v procentech (min:1, default 100) */
    unsigned speed_in_percentage_requested; /**< pozadovana rychlost v procentech (min:1, default 100) */
    unsigned speed_frame_width_requested;   /**< pozadovana sirka snimku v taktech */
    unsigned speed_in_percentage;           /**< aktualne nastavena rychlost v procentech (min:1, default 100) */
    unsigned speed_frame_width;             /**< aktualne nastavena sirka snimku v taktech */
} st_CUSTOMSPEED;

extern st_CUSTOMSPEED g_customspeed;

#define customspeed_on_screen_done() (g_customspeed.speed_sync_event.ticks -= VIDEO_SCREEN_TICKS)

#define customspeed_get_current_speed() (g_customspeed.speed_in_percentage)
#define customspeed_get_current_speed_ticks() (g_customspeed.speed_frame_width)
#define customspeed_get_event_pointer() (&g_customspeed.speed_sync_event)

#define CUSTOMSPEED_TEST_SPEED_100 (customspeed_get_current_speed() == 100)

#ifdef __cplusplus
extern "C"
{
#endif

    extern void customspeed_init(int speed_in_percentage);
    extern void customspeed_print(void);
    extern void customspeed_set_request(int speed_in_percentage);
    extern void customspeed_step_up_request(int step);
    extern void customspeed_step_down_request(int step);
    extern void customspeed_store_speed(void);
    extern void customspeed_restore_speed(void);
    /** Zpracuje hodnotu CLI volby --speed (max | 1..CUSTOMSPEED_MAX_VALUE); viz customspeed.c. */
    extern bool customspeed_parse_cli_value(const char *text, bool *out_max, int *out_percent);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOMSPEED_H */
