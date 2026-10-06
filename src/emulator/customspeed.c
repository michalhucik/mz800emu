#include "main.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "hw-generic/gdg/video.h"
#include "mzarch/mzarch.h"
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
    float frame_time = ((float)VIDEO_SCREEN_TICKS / g_customspeed.speed_frame_width_requested) * (1000.0f / VIDEO_SCREENS_PER_SEC);
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
    g_customspeed.speed_frame_width_requested = VIDEO_SCREEN_TICKS * g_customspeed.speed_in_percentage_requested / 100;
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

/**
 * @brief Zpracuje hodnotu CLI volby `--speed`.
 *
 * Přijímá buď řetězec `max` (MAX SPEED, malá/velká písmena se rozlišují),
 * nebo celé číslo v rozsahu 1..CUSTOMSPEED_MAX_VALUE (rychlost v procentech,
 * 100 = normální rychlost). Na rozdíl od customspeed_set_request() hodnoty
 * mimo rozsah NEořezává, ale odmítne je.
 *
 * Při neplatné hodnotě vypíše anglickou chybu na stderr a výstupní parametry
 * nemění.
 *
 * @param text        Hodnota volby (NULL nebo prázdná = neplatná).
 * @param[out] out_max     true pro `max`, jinak false.
 * @param[out] out_percent Procenta (1..CUSTOMSPEED_MAX_VALUE); pro `max` se
 *                         nenastavuje.
 * @return true při platné hodnotě, jinak false.
 *
 * @pre out_max a out_percent jsou platné ukazatele.
 * @note Funkce nemá vedlejší efekt na stav emulace (jen čte text).
 */
bool customspeed_parse_cli_value(const char *text, bool *out_max, int *out_percent)
{
    if (text && strcmp(text, "max") == 0)
    {
        *out_max = true;
        return true;
    }

    gchar *endptr = NULL;
    gint64 value = 0;
    bool digits_only = (text && *text);
    for (const char *p = text; digits_only && *p; p++)
    {
        if (*p < '0' || *p > '9') digits_only = false;
    }
    if (digits_only)
    {
        value = g_ascii_strtoll(text, &endptr, 10);
        if (value >= 1 && value <= CUSTOMSPEED_MAX_VALUE && endptr && *endptr == '\0')
        {
            *out_max = false;
            *out_percent = (int)value;
            return true;
        }
    }

    fprintf(stderr,
            "Invalid value for --speed: '%s' (expected 'max' or an integer percentage in 1..%d, 100 = normal speed)\n",
            text ? text : "", CUSTOMSPEED_MAX_VALUE);
    return false;
}
