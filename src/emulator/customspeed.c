#include "main.h"
#include <glib.h>
#include "mzarch/mzarch.h"
#include "mzarch/mzhal.h"
#include "customspeed.h"
#include "emulator.h"

/*******************************************************************************
 *
 *
 *                  Custom CPU speed synchronisation
 *                  ================================
 *
 *
 *******************************************************************************/

st_CUSTOMSPEED g_customspeed;

void customspeed_print(void)
{
    /* Runtime z g_mzhal (mzhal 10b, cold cesta - UI/konzole). */
    float frame_time = ((float)g_mzhal.video_screen_ticks / g_customspeed.speed_frame_width_requested) * (1000.0f / g_mzhal.video_screens_per_sec);
    float fps = (1 / frame_time) * 1000;
    g_print("%s speed: %d %% => frame time: %0.2f ms, FPS: %0.2f\n", (EMULATOR_TEST_CUSTOM_SPEED) ? "Custom" : "Normal", g_customspeed.speed_in_percentage_requested, frame_time, fps);
}

void customspeed_set_request(int speed_in_percentage)
{
    if (speed_in_percentage > CUSTOMSPEED_MAX_VALUE)
        speed_in_percentage = CUSTOMSPEED_MAX_VALUE;
    if (speed_in_percentage < 1)
        speed_in_percentage = 1;
    g_customspeed.speed_in_percentage_requested = speed_in_percentage;
    g_customspeed.speed_frame_width_requested = g_mzhal.video_screen_ticks * g_customspeed.speed_in_percentage_requested / 100;
    customspeed_print();
}

void customspeed_init(int speed_in_percentage)
{
    customspeed_set_request(speed_in_percentage);
    g_customspeed.previous_speed_in_percentage = g_customspeed.speed_in_percentage_requested;
    g_customspeed.speed_sync_event.event_name = MZEVENT_CUSTOM_SPEED_SYNCHRONISATION;
    g_customspeed.speed_sync_event.ticks = g_customspeed.speed_frame_width_requested;
    g_customspeed.speed_in_percentage = g_customspeed.speed_in_percentage_requested;
    g_customspeed.speed_frame_width = g_customspeed.speed_frame_width_requested;
}

void customspeed_step_up_request(int step)
{
    int speed_in_percentage = g_customspeed.speed_in_percentage_requested + step;
    customspeed_set_request(speed_in_percentage);
}

void customspeed_step_down_request(int step)
{
    int speed_in_percentage = g_customspeed.speed_in_percentage_requested - step;
    customspeed_set_request(speed_in_percentage);
}

void customspeed_store_speed(void)
{
    if (g_customspeed.speed_in_percentage != 100)
    {
        g_customspeed.previous_speed_in_percentage = g_customspeed.speed_in_percentage;
    };
}

void customspeed_restore_speed(void)
{
    if ((g_customspeed.speed_in_percentage == 100) && (g_customspeed.previous_speed_in_percentage != 100))
    {
        customspeed_set_request(g_customspeed.previous_speed_in_percentage);
    };
}