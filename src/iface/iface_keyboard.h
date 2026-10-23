#ifndef IFACE_KEYBOARD_H
#define IFACE_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

typedef struct iface_keyboard_state_t
{
    int lalt;
    int ralt;
    int lshift;
    int rshift;
} iface_keyboard_state_t;

extern iface_keyboard_state_t g_iface_kbdstate;

#define IFACE_KEYBOARD_PRESSED true
#define IFACE_KEYBOARD_RELEASED false

#ifdef __cplusplus
extern "C"
{
#endif
    /**
     * @brief Sken celé klávesnice hostitele do klávesnicové matice MZ.
     *
     * Spočítá celou matici (a stav joysticku z numerické klávesnice, je-li
     * tak joystick nastaven) a předá ji najednou přes
     * replay_host_keyboard_submit(): mimo záznam vstupů ji emu vlákno zapíše
     * na hranici snímku nebo v pauze, při záznamu se převezme na hranici
     * snímku, při přehrávání se zahodí.
     * Se stisknutým Alt nedělá nic (klávesové zkratky emulátoru).
     *
     * Je-li některý port joysticku typu JOY_TYPE_NUM_KEYPAD, klávesy KP_0 až
     * KP_9 ovládají jen joystick a do matice klávesnice se nedostanou (bez
     * ohledu na NumLock). Ostatní klávesy numerické klávesnice (+ - * / .
     * Enter) zůstávají klávesnicí. Bez joysticku na numerické klávesnici
     * jsou KP_0 až KP_9 při zapnutém NumLock číslice, při vypnutém kurzorové
     * klávesy a INST.
     *
     * @param state   Stav kláves SDL (SDL_GetKeyboardState()).
     * @param kmod    Modifikátory SDL.
     * @param numkeys Počet položek @p state; 0 = žádná klávesa (předá klidovou matici).
     * @pre UI vlákno.
     */
    extern void iface_keyboard_full_scan(const bool *state, SDL_Keymod kmod, int numkeys);

#ifdef __cplusplus
}
#endif
#endif // IFACE_KEYBOARD_H
