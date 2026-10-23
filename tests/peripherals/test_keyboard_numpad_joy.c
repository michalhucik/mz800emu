/*
 * test_keyboard_numpad_joy.c - sken klávesnice hostitele a joystick
 *                              emulovaný na numerické klávesnici
 *
 * Testuje iface_keyboard_full_scan(): když je port joysticku typu
 * JOY_TYPE_NUM_KEYPAD, KP_0 až KP_9 ovládají jen joystick a do matice
 * klávesnice MZ se nedostanou, bez ohledu na NumLock. Bez joysticku na
 * numerické klávesnici jsou to číslice (NumLock zapnutý) nebo kurzorové
 * klávesy a INST (NumLock vypnutý).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>

#include <SDL3/SDL.h>
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/joy/joy.h"
#include "iface/iface_keyboard.h"
#include "replay.h"

/** @brief Podvržený stav kláves SDL (jako SDL_GetKeyboardState()). */
static bool s_keys[SDL_SCANCODE_COUNT];

static en_JOY_TYPE s_saved_type[JOY_DEVID_COUNT];

void setUp(void)
{
    memset(s_keys, 0, sizeof(s_keys));
    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        s_saved_type[i] = g_joy.dev[i].type;
        g_joy.dev[i].type = JOY_TYPE_NONE;
        g_joy.dev[i].state = 0xFF;
    }
    memset(g_pio8255.keyboard_matrix, 0xFF, sizeof(g_pio8255.keyboard_matrix));
}

void tearDown(void)
{
    for (int i = 0; i < JOY_DEVID_COUNT; i++)
    {
        g_joy.dev[i].type = s_saved_type[i];
        g_joy.dev[i].state = 0xFF;
    }
    memset(g_pio8255.keyboard_matrix, 0xFF, sizeof(g_pio8255.keyboard_matrix));
}

/**
 * @brief Sken s podvrženým stavem a jeho zápis do stroje.
 *
 * Mimo záznam vstupů sken matici jen předá (replay_host_keyboard_submit());
 * zapíše ji emu vlákno, tady replay_on_paused() jako v paused smyčce.
 */
static void scan(SDL_Keymod kmod)
{
    iface_keyboard_full_scan(s_keys, kmod, SDL_SCANCODE_COUNT);
    replay_on_paused();
}

/* Bez joysticku: KP_8 je při NumLock číslice 8, bez NumLock kurzor nahoru. */
void test_numpad_without_joy_is_keyboard(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    s_keys[SDL_SCANCODE_KP_8] = true;

    scan(SDL_KMOD_NUM);
    TEST_ASSERT_EQUAL_HEX8(0xFE, g_pio8255.keyboard_matrix[5]); /* 8 */
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[7]);

    scan(SDL_KMOD_NONE);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[5]);
    TEST_ASSERT_EQUAL_HEX8(0xDF, g_pio8255.keyboard_matrix[7]); /* UP */
}

/* Joystick na numerické klávesnici: KP_0 až KP_9 nejsou klávesnice MZ při
 * zapnutém ani vypnutém NumLock, joystick je vidí. */
void test_numpad_joy_digits_not_keyboard(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_joy.dev[JOY_DEVID_1].type = JOY_TYPE_NUM_KEYPAD;
    s_keys[SDL_SCANCODE_KP_8] = true; /* nahoru */
    s_keys[SDL_SCANCODE_KP_4] = true; /* vlevo */
    s_keys[SDL_SCANCODE_KP_0] = true; /* TRIG2 */

    const SDL_Keymod mods[] = {SDL_KMOD_NUM, SDL_KMOD_NONE};
    for (size_t m = 0; m < sizeof(mods) / sizeof(mods[0]); m++)
    {
        scan(mods[m]);
        for (int col = 0; col < 10; col++)
        {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, g_pio8255.keyboard_matrix[col], "numpad v matici klávesnice");
        }
        uint8_t expect = (uint8_t)~((1 << JOY_STATEBIT_UP) | (1 << JOY_STATEBIT_LEFT) | (1 << JOY_STATEBIT_TRIG2));
        TEST_ASSERT_EQUAL_HEX8(expect, g_joy.dev[JOY_DEVID_1].state);
    }
}

/* Joystick na numerické klávesnici nebere ostatní klávesy: hlavní řada
 * číslic, šipky a KP_PERIOD (NumLock: tečka) zůstávají klávesnicí. */
void test_numpad_joy_keeps_other_keys(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_joy.dev[JOY_DEVID_0].type = JOY_TYPE_NUM_KEYPAD;
    s_keys[SDL_SCANCODE_8] = true;
    s_keys[SDL_SCANCODE_UP] = true;
    s_keys[SDL_SCANCODE_KP_PERIOD] = true;
    s_keys[SDL_SCANCODE_KP_5] = true; /* TRIG1 */

    scan(SDL_KMOD_NUM);
    TEST_ASSERT_EQUAL_HEX8(0xFE, g_pio8255.keyboard_matrix[5]); /* 8, KP_5 ne */
    TEST_ASSERT_EQUAL_HEX8(0xFE, g_pio8255.keyboard_matrix[6]); /* . */
    TEST_ASSERT_EQUAL_HEX8(0xDF, g_pio8255.keyboard_matrix[7]); /* UP */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)~(1 << JOY_STATEBIT_TRIG1), g_joy.dev[JOY_DEVID_0].state);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_numpad_without_joy_is_keyboard);
    RUN_TEST(test_numpad_joy_digits_not_keyboard);
    RUN_TEST(test_numpad_joy_keeps_other_keys);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
