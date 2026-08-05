#include "mzarch/mzarch_config.h" /* capability makra - dříve tranzitivně přes main.h (mzhal 11c-1) */
#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <glib.h>

#include "iface.h"
#include "iface_video.h"
#include "iface_joy.h"
#include "iface_audio.h"

bool iface_init(void)
{
    g_print("Initializing interface...\n");

    if(!iface_video_init())
    {
        return false;
    };

    if(!iface_joy_init())
    {
        return false;
    };

    if(!iface_audio_init())
    {
        return false;
    };

    return true;
}

void iface_exit(void)
{
    g_print("Quitting interface...\n");

    iface_audio_exit();
    iface_joy_exit();
    iface_video_exit();
}
