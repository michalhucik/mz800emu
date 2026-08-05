/*
 * joy_setup_window.cpp - ImGui okno pro konfiguraci joysticku
 *
 * Umoznuje:
 * - Inicializovat/vypnout joystick subsystem
 * - Pro kazdy z 2 joysticku vybrat typ (None/Numpad/Real device)
 * - Konfigurovat mapovani os a tlacitek pro real zarizeni
 * - Zobrazit live stav joysticku
 */

#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

// Lokalizace
#include "i18n.h"

#include "iface/iface_joy.h"
#include "hw-generic/joy/joy.h"

extern "C"
{
    void imgui_joy_setup_window(bool *p_open);
}


/* Nazvy gamepad tlacitek pro combo box */
static const char *gamepad_button_names[] = {
    "A (South)",
    "B (East)",
    "X (West)",
    "Y (North)",
    "Back",
    "Guide",
    "Start",
    "Left Stick",
    "Right Stick",
    "Left Shoulder",
    "Right Shoulder",
};
static const int gamepad_button_values[] = {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
};
static const int gamepad_button_count = sizeof(gamepad_button_values) / sizeof(gamepad_button_values[0]);

/* Stav jednoho joysticku v UI dialogu */
struct JoyDeviceUI
{
    en_JOY_TYPE type;
    int selected_device_idx;           /* index do seznamu dostupnych zarizeni (-1=zadny) */
    st_IFACE_JOY_PARAMS params;
    st_IFACE_JOY_SYSDEVICE sysdevice;
};

/* Celkovy stav dialogu */
struct JoySetupState
{
    bool initialized;
    bool startup_joy_subsystem;
    JoyDeviceUI dev[JOY_DEVID_COUNT];
};

static JoySetupState s_state = {0};

/* Najde index gamepad tlacitka v poli gamepad_button_values */
static int find_gamepad_button_index(int button_value)
{
    for (int i = 0; i < gamepad_button_count; i++)
    {
        if (gamepad_button_values[i] == button_value)
            return i;
    }
    return 0; /* vychozi: A (South) */
}

/* Inicializace stavu z aktualniho nastaveni */
static void init_state_from_current(void)
{
    s_state.startup_joy_subsystem = g_iface_joy.startup_joy_subsystem;

    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        s_state.dev[i].type = g_joy.dev[i].type;
        s_state.dev[i].params = g_iface_joy.dev[i].params;
        s_state.dev[i].sysdevice = g_iface_joy.dev[i].sysdevice;
        s_state.dev[i].selected_device_idx = -1;

        /* Zkusime najit odpovidajici zarizeni v seznamu dostupnych */
        if (g_joy.dev[i].type == JOY_TYPE_JOYSTICK)
        {
            for (int j = 0; j < g_iface_joy.available_count; j++)
            {
                if (g_iface_joy.available[j].instance_id == g_iface_joy.dev[i].sysdevice.instance_id)
                {
                    s_state.dev[i].selected_device_idx = j;
                    break;
                }
            }
        }
    }

    s_state.initialized = true;
}

/* Aplikovani zmen z UI do emulatoroveho stavu */
static void apply_changes(void)
{
    g_iface_joy.startup_joy_subsystem = s_state.startup_joy_subsystem;

    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        en_JOY_DEVID devid = (en_JOY_DEVID)i;
        en_JOY_TYPE new_type = s_state.dev[i].type;
        en_JOY_TYPE old_type = g_joy.dev[i].type;

        /* Zmena typu */
        g_joy.dev[i].type = new_type;

        if (new_type == JOY_TYPE_JOYSTICK)
        {
            /* Otevrit zarizeni pokud se zmenilo */
            int idx = s_state.dev[i].selected_device_idx;
            if (idx >= 0 && idx < g_iface_joy.available_count)
            {
                iface_joy_open_sysdevice(devid, &g_iface_joy.available[idx]);
                iface_joy_set_params(devid, &s_state.dev[i].params);
            }
        }
        else if (old_type == JOY_TYPE_JOYSTICK)
        {
            /* Uzavrit zarizeni pokud se typ zmenil z JOYSTICK */
            iface_joy_close_device(devid);
        }
    }
}

/* Vykresleni vizualniho numpad mapovani */
static void DrawNumpadMapping(void)
{
    ImVec4 key_color = ImVec4(0.4f, 0.7f, 0.2f, 1.0f);
    ImVec2 key_size = ImVec2(28, 28);

    ImGui::PushStyleColor(ImGuiCol_Button, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, key_color);

    /* Radek 1: sipky + 7 8 9 */
    ImGui::Text("     ");
    ImGui::SameLine();
    ImGui::Text("%s", "\xe2\x86\x96"); /* ↖ */
    ImGui::SameLine();
    ImGui::Text("   %s", "\xe2\x86\x91"); /* ↑ */
    ImGui::SameLine();
    ImGui::Text("   %s", "\xe2\x86\x97"); /* ↗ */

    ImGui::Text("     ");
    ImGui::SameLine();
    ImGui::Button("7", key_size);
    ImGui::SameLine();
    ImGui::Button("8", key_size);
    ImGui::SameLine();
    ImGui::Button("9", key_size);

    /* Radek 2: sipky + 4 _ 6 */
    ImGui::Text("  %s", "\xe2\x86\x90"); /* ← */
    ImGui::SameLine();
    ImGui::Button("4", key_size);
    ImGui::SameLine();
    ImGui::Dummy(key_size);
    ImGui::SameLine();
    ImGui::Button("6", key_size);
    ImGui::SameLine();
    ImGui::Text(" %s", "\xe2\x86\x92"); /* → */

    /* Radek 3: sipky + 1 2 3 */
    ImGui::Text("     ");
    ImGui::SameLine();
    ImGui::Button("1", key_size);
    ImGui::SameLine();
    ImGui::Button("2", key_size);
    ImGui::SameLine();
    ImGui::Button("3", key_size);

    ImGui::Text("     ");
    ImGui::SameLine();
    ImGui::Text("%s", "\xe2\x86\x99"); /* ↙ */
    ImGui::SameLine();
    ImGui::Text("   %s", "\xe2\x86\x93"); /* ↓ */
    ImGui::SameLine();
    ImGui::Text("   %s", "\xe2\x86\x98"); /* ↘ */

    ImGui::PopStyleColor(3);

    /* Triggery */
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, key_color);
    ImGui::Button("5", key_size);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::Text("- %s 1", _("Trigger"));

    ImGui::PushStyleColor(ImGuiCol_Button, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, key_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, key_color);
    ImGui::Button("0", key_size);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::Text("- %s 2", _("Trigger"));
}

/* Vykresleni konfigurace gamepadu */
static void DrawGamepadConfig(JoyDeviceUI *joyui)
{
    st_IFACE_JOY_PARAMS *p = &joyui->params;

    ImGui::Text("%s: %s", _("Device"), joyui->sysdevice.name);
    ImGui::Text("%s: %s", _("Type"), _("Gamepad (standard mapping)"));
    ImGui::Spacing();

    /* Smerovy vstup: Left Stick / D-Pad */
    const char *dir_items[] = {_("Left Stick"), _("D-Pad")};
    int dir_idx = p->use_dpad ? 1 : 0;
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo(_L("Directions"), &dir_idx, dir_items, 2))
    {
        p->use_dpad = (dir_idx == 1);
    }

    /* Deadzone slider (pouze pro stick mod) */
    if (!p->use_dpad)
    {
        int dz = (int)p->deadzone;
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderInt(_L("Deadzone"), &dz, 0, 32000))
        {
            p->deadzone = (Sint16)dz;
        }
    }

    /* Trigger 1 */
    int trig1_idx = find_gamepad_button_index(p->gamepad_trig1);
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo(_L("Trigger 1"), &trig1_idx, gamepad_button_names, gamepad_button_count))
    {
        p->gamepad_trig1 = gamepad_button_values[trig1_idx];
    }

    /* Trigger 2 */
    int trig2_idx = find_gamepad_button_index(p->gamepad_trig2);
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo(_L("Trigger 2"), &trig2_idx, gamepad_button_names, gamepad_button_count))
    {
        p->gamepad_trig2 = gamepad_button_values[trig2_idx];
    }
}

/* Vykresleni konfigurace raw joysticku */
static void DrawRawJoystickConfig(JoyDeviceUI *joyui)
{
    st_IFACE_JOY_PARAMS *p = &joyui->params;

    ImGui::Text("%s: %s", _("Device"), joyui->sysdevice.name);
    ImGui::Text("%s: %d, %s: %d",
                _("Axes"), joyui->sysdevice.axes,
                _("Buttons"), joyui->sysdevice.buttons);
    ImGui::Spacing();

    /* X axis */
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(_L("X Axis"), &p->x_axis, 1, 1);
    if (p->x_axis < 0) p->x_axis = 0;
    if (p->x_axis >= joyui->sysdevice.axes && joyui->sysdevice.axes > 0)
        p->x_axis = joyui->sysdevice.axes - 1;
    ImGui::SameLine();
    {
        char label[256];
        snprintf(label, sizeof(label), "%s##invert_x", _("Invert"));
        ImGui::Checkbox(label, &p->x_invert);
    }

    /* Y axis */
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(_L("Y Axis"), &p->y_axis, 1, 1);
    if (p->y_axis < 0) p->y_axis = 0;
    if (p->y_axis >= joyui->sysdevice.axes && joyui->sysdevice.axes > 0)
        p->y_axis = joyui->sysdevice.axes - 1;
    ImGui::SameLine();
    {
        char label[256];
        snprintf(label, sizeof(label), "%s##invert_y", _("Invert"));
        ImGui::Checkbox(label, &p->y_invert);
    }

    /* Deadzone */
    int dz = (int)p->deadzone;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderInt(_L("Deadzone"), &dz, 0, 32000))
    {
        p->deadzone = (Sint16)dz;
    }

    /* Trigger 1 button */
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(_L("Trigger 1"), &p->trig1_button, 1, 1);
    if (p->trig1_button < 0) p->trig1_button = 0;
    if (p->trig1_button >= joyui->sysdevice.buttons && joyui->sysdevice.buttons > 0)
        p->trig1_button = joyui->sysdevice.buttons - 1;

    /* Trigger 2 button */
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(_L("Trigger 2"), &p->trig2_button, 1, 1);
    if (p->trig2_button < 0) p->trig2_button = 0;
    if (p->trig2_button >= joyui->sysdevice.buttons && joyui->sysdevice.buttons > 0)
        p->trig2_button = joyui->sysdevice.buttons - 1;
}

/* Vykresleni live statusu joysticku */
static void DrawLiveStatus(en_JOY_DEVID devid, en_JOY_TYPE type)
{
    uint8_t state = 0xff;

    if (type == JOY_TYPE_NUM_KEYPAD)
    {
        state = g_joy.dev[devid].state;
    }
    else if (type == JOY_TYPE_JOYSTICK)
    {
        state = iface_joy_scan(devid);
    }
    else
    {
        return;
    }

    ImGui::SeparatorText(_("Live Status"));

    /* Smerove indikatory */
    bool up = !(state & (1 << JOY_STATEBIT_UP));
    bool down = !(state & (1 << JOY_STATEBIT_DOWN));
    bool left = !(state & (1 << JOY_STATEBIT_LEFT));
    bool right = !(state & (1 << JOY_STATEBIT_RIGHT));
    bool trig1 = !(state & (1 << JOY_STATEBIT_TRIG1));
    bool trig2 = !(state & (1 << JOY_STATEBIT_TRIG2));

    ImVec4 on_color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    ImVec4 off_color = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);

    ImGui::Text("%s:", _("Directions"));
    ImGui::SameLine();

    /* UP */
    ImGui::TextColored(up ? on_color : off_color, "%s", "\xe2\x86\x91");
    ImGui::SameLine();
    /* DOWN */
    ImGui::TextColored(down ? on_color : off_color, "%s", "\xe2\x86\x93");
    ImGui::SameLine();
    /* LEFT */
    ImGui::TextColored(left ? on_color : off_color, "%s", "\xe2\x86\x90");
    ImGui::SameLine();
    /* RIGHT */
    ImGui::TextColored(right ? on_color : off_color, "%s", "\xe2\x86\x92");
    ImGui::SameLine();
    ImGui::Text("   ");
    ImGui::SameLine();

    /* Triggery */
    ImGui::TextColored(trig1 ? on_color : off_color, "T1");
    ImGui::SameLine();
    ImGui::TextColored(trig2 ? on_color : off_color, "T2");
}

/* Vykresleni obsahu jednoho tabu joysticku */
static void DrawJoystickTab(en_JOY_DEVID devid)
{
    JoyDeviceUI *joyui = &s_state.dev[devid];

    ImGui::Text("%s %d", _("Setup preferences for Joystick"), devid + 1);
    ImGui::Spacing();

    /* Combo box pro typ joysticku */
    /* Polozky: None, Emulated on numeric keypad, + dostupna zarizeni */
    int combo_idx = 0;
    if (joyui->type == JOY_TYPE_NUM_KEYPAD)
        combo_idx = 1;
    else if (joyui->type == JOY_TYPE_JOYSTICK)
        combo_idx = 2 + joyui->selected_device_idx;

    /* Sestavit seznam polozek */
    char combo_items[IFACE_JOY_MAX_SYSDEVICES + 2][160];
    const char *combo_ptrs[IFACE_JOY_MAX_SYSDEVICES + 2];
    int combo_count = 2;

    g_snprintf(combo_items[0], sizeof(combo_items[0]), "%s", _("None"));
    g_snprintf(combo_items[1], sizeof(combo_items[1]), "%s", _("Emulated on numeric keypad"));
    combo_ptrs[0] = combo_items[0];
    combo_ptrs[1] = combo_items[1];

    for (int i = 0; i < g_iface_joy.available_count; i++)
    {
        st_IFACE_JOY_SYSDEVICE *sd = &g_iface_joy.available[i];
        if (sd->is_gamepad)
            g_snprintf(combo_items[2 + i], sizeof(combo_items[2 + i]), "%s (Gamepad)", sd->name);
        else
            g_snprintf(combo_items[2 + i], sizeof(combo_items[2 + i]), "%s", sd->name);
        combo_ptrs[2 + i] = combo_items[2 + i];
        combo_count++;
    }

    /* Osetrit neplatny index */
    if (combo_idx < 0 || combo_idx >= combo_count)
        combo_idx = 0;

    ImGui::SetNextItemWidth(350);
    if (ImGui::Combo("##joy_type", &combo_idx, combo_ptrs, combo_count))
    {
        if (combo_idx == 0)
        {
            joyui->type = JOY_TYPE_NONE;
            joyui->selected_device_idx = -1;
        }
        else if (combo_idx == 1)
        {
            joyui->type = JOY_TYPE_NUM_KEYPAD;
            joyui->selected_device_idx = -1;
        }
        else
        {
            joyui->type = JOY_TYPE_JOYSTICK;
            joyui->selected_device_idx = combo_idx - 2;
            joyui->sysdevice = g_iface_joy.available[joyui->selected_device_idx];

            /* Nastavit vychozi parametry */
            iface_joy_set_default_params(&joyui->params);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Obsah podle vybraneho typu */
    if (joyui->type == JOY_TYPE_NUM_KEYPAD)
    {
        DrawNumpadMapping();
    }
    else if (joyui->type == JOY_TYPE_JOYSTICK && joyui->selected_device_idx >= 0)
    {
        if (joyui->sysdevice.is_gamepad)
        {
            DrawGamepadConfig(joyui);
        }
        else
        {
            DrawRawJoystickConfig(joyui);
        }
    }

    /* Live status — zobrazit i pro numpad (ukazuje aktualni stav) */
    DrawLiveStatus(devid, joyui->type);
}

void imgui_joy_setup_window(bool *p_open)
{
    if (!*p_open)
    {
        s_state.initialized = false;
        return;
    }

    /* Inicializace stavu pri prvnim otevreni */
    if (!s_state.initialized)
    {
        init_state_from_current();
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *joy_title = _L("Joystick Setup");
    auto_layout_first_use_portrait(joy_title, 500.0f, 500.0f);
    if (ImGui::Begin(joy_title, p_open, flags))
    {
        /* Checkbox: Inicializovat joystick subsystem */
        if (ImGui::Checkbox(_L("Initialize Joystick Subsystem"), &s_state.startup_joy_subsystem))
        {
            if (s_state.startup_joy_subsystem)
            {
                /* Inicializovat SDL joystick subsystem a naskenovat zarizeni */
                iface_joy_subsystem_init();
                iface_joy_rescan_devices();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", _("need only for real gaming devices"));

        /* Tlacitko Rescan */
        ImGui::SameLine();
        if (ImGui::Button(_L("Rescan Devices")))
        {
            if (!SDL_WasInit(SDL_INIT_JOYSTICK))
            {
                iface_joy_subsystem_init();
            }
            iface_joy_rescan_devices();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tab bar: Joystick 1 | Joystick 2 */
        if (ImGui::BeginTabBar("##JoystickTabs"))
        {
            for (int i = 0; i < JOY_DEVID_COUNT; i++)
            {
                char tab_label[32];
                g_snprintf(tab_label, sizeof(tab_label), "%s %d", _("Joystick"), i + 1);

                if (ImGui::BeginTabItem(tab_label))
                {
                    ImGui::Spacing();
                    ImGui::PushID(i);
                    DrawJoystickTab((en_JOY_DEVID)i);
                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tlacitka OK / Cancel */
        float avail = ImGui::GetContentRegionAvail().x;
        float btn_width = 100.0f;
        float total = btn_width * 2 + ImGui::GetStyle().ItemSpacing.x;
        float offset = (avail - total) * 0.5f;
        if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button(_L("OK"), ImVec2(btn_width, 0)))
        {
            apply_changes();
            *p_open = false;
            s_state.initialized = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(btn_width, 0)))
        {
            *p_open = false;
            s_state.initialized = false;
        }
    }
    ImGui::End();
}


