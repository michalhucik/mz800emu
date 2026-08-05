#include "main.h"
#include <SDL3/SDL.h>
#include <glib.h>
#include <stdint.h>
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "iface-video/sdlapp/sdlapp_imgui_video.h"

#include "iface/iface_joy.h"

void iface_sdl3_events_cb(SdlAppWindow *win, SDL_Event *event, gpointer user_data)
{
    (void)user_data;
    MyImGui *gui = (MyImGui *)sdlapp_window_get_gui_data(win);

    switch (event->type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (gui->MainWindowHasFocus || gui->VkbdWindowHasFocus)
        {
            gui->haveKeyboardEvents = TRUE;
        };
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        iface_sdl3_video_window_resize_event_handler(win, event);
        break;

    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        /* Aktualizovat seznam dostupnych zarizeni */
        if (g_iface_joy.startup_joy_subsystem)
        {
            iface_joy_rescan_devices();
        }
        break;

    default:
        break;
    };
}