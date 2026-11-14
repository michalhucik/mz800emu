/**
 * @file main_window_focus.h
 * @brief Pravidlo, kdy hlavní okno emulátoru předává klávesy emulované klávesnici, a příznaky hlavního okna.
 *
 * Hlavní okno (main_window.cpp) nastaví podle něj MyImGui::MainWindowHasFocus
 * a obsluha událostí SDL (iface_sdl3_events.c) pak klávesy pošle
 * emulované klávesnici jen při MainWindowHasFocus nebo VkbdWindowHasFocus.
 * Je v hlavičce, aby testy oken (např. okna Virtual CMT) ověřily totéž
 * pravidlo, které používá aplikace.
 */

#ifndef MAIN_WINDOW_FOCUS_H
#define MAIN_WINDOW_FOCUS_H

#include "libs/imgui/imgui.h"

/**
 * @brief Okenní příznaky hlavního okna emulátoru (obraz přes celou plochu hlavního viewportu, vzadu).
 *
 * Jediná definice pro main_window.cpp (ShowFullScreenImageEmulatorWindow())
 * a testy pravidla fokusu, které hlavní okno nahrazují (test_cmt_window.cpp):
 * na příznacích závisí, které okno fokus po kliknutí dostane
 * (ImGuiWindowFlags_NoBringToFrontOnFocus drží obraz za okny nástrojů),
 * takže test s vlastní kopií by po změně příznaků ověřoval jiné chování
 * než aplikace.
 */
static const ImGuiWindowFlags MAIN_WINDOW_FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                                  | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar
                                                  | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

/**
 * @brief Má emulovaná klávesnice dostávat klávesy?
 *
 * Ano, když má fokus hlavní okno emulátoru nebo jeho potomek, nebo okno
 * virtuální klávesnice (VKBD). Okno nástroje (Virtual CMT, debugger, ...)
 * s fokusem klávesy emulované klávesnici nepředá.
 *
 * @param vkbd_has_focus Fokus má okno virtuální klávesnice (MyImGui::VkbdWindowHasFocus).
 * @return true = předávat klávesy emulované klávesnici.
 * @pre UI vlákno, uvnitř Begin() / End() hlavního okna emulátoru (ImGui::IsWindowFocused()
 *      se ptá na právě kreslené okno).
 */
static inline bool main_window_keyboard_focus(bool vkbd_has_focus)
{
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || vkbd_has_focus;
}

#endif /* MAIN_WINDOW_FOCUS_H */
