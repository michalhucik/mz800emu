/**
 * @file main_window_focus.h
 * @brief Pravidlo, kdy hlavní okno emulátoru předává klávesy emulované klávesnici, a příznaky hlavního okna.
 *
 * Hlavní okno (main_window.cpp) nastaví podle něj MyImGui::MainWindowHasFocus
 * a obsluha událostí SDL (iface_sdl3_events.c) pak klávesy pošle
 * emulované klávesnici jen při MainWindowHasFocus nebo VkbdWindowHasFocus.
 * Je v hlavičce, aby testy oken (např. okna Virtual CMT) ověřily totéž
 * pravidlo, které používá aplikace. Když klávesy zadržuje overlay menu
 * (overlay/overlay.h), hlavní okno je nepředává bez ohledu na fokus
 * (main_window_keyboard_enabled()). Sdílená jsou i pravidla, kterými
 * hlavní okno overlay menu otevírá: klávesa F10 (main_window_overlay_open_key())
 * a levý Alt s levým tlačítkem myši (main_window_overlay_open_click()).
 * Je tu i jediné pravidlo, kdy smí ImGui přepínat okna klávesnicí
 * (Ctrl+Tab): main_window_nav_windowing_allowed() a
 * main_window_nav_windowing_set().
 */

#ifndef MAIN_WINDOW_FOCUS_H
#define MAIN_WINDOW_FOCUS_H

#include <stdint.h>
#include <string.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "emulator/replay.h"
#include "hw-generic/joy/joy.h"

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

/**
 * @brief Má emulovaná klávesnice dostávat klávesy, s ohledem na overlay menu?
 *
 * Jako main_window_keyboard_focus(), ale když klávesy zadržuje overlay
 * menu, vždy ne: overlay bere klávesy pro sebe (F10, Esc, číslice) a do
 * matice stroje nesmí prosáknout žádná, ani ve snímku otevření, kdy fokus
 * ještě drží hlavní okno, ani když se fokus do hlavního okna vrátí po
 * zavření dialogu, ani držená klávesa, která overlay zavřela.
 *
 * @param vkbd_has_focus Fokus má okno virtuální klávesnice (MyImGui::VkbdWindowHasFocus).
 * @param overlay_blocks Overlay menu klávesy zadržuje (imgui_overlay_blocks_keyboard()).
 * @return true = předávat klávesy emulované klávesnici.
 * @pre UI vlákno, uvnitř Begin() / End() hlavního okna emulátoru.
 */
static inline bool main_window_keyboard_enabled(bool vkbd_has_focus, bool overlay_blocks)
{
    return !overlay_blocks && main_window_keyboard_focus(vkbd_has_focus);
}

/**
 * @brief Smí ImGui přepínat okna klávesnicí (Ctrl+Tab, Ctrl+Shift+Tab)?
 *
 * Ne, když klávesy patří emulovanému stroji (fokus má hlavní okno nebo
 * okno virtuální klávesnice): Ctrl i Tab jsou klávesy stroje a přepínač
 * oken by fokus odnesl do okna nástroje, takže by klávesnice stroje přestala
 * dostávat klávesy, dokud uživatel neklikne do obrazu. Ne také, dokud klávesy
 * zadržuje overlay menu (Ctrl+Tab v něm přepíná taby). Při fokusu okna
 * nástroje ano: Ctrl+Tab přepíná mezi okny nástrojů a vrátí fokus i do
 * obrazu; odtud do okna nástroje vede jen myš nebo zkratka, která okno otevře.
 *
 * @param vkbd_has_focus Fokus má okno virtuální klávesnice (MyImGui::VkbdWindowHasFocus).
 * @param overlay_blocks Overlay menu klávesy zadržuje (imgui_overlay_blocks_keyboard()).
 * @return true = přepínač oken ImGui má být zapnutý.
 * @pre UI vlákno, uvnitř Begin() / End() hlavního okna emulátoru.
 */
static inline bool main_window_nav_windowing_allowed(bool vkbd_has_focus, bool overlay_blocks)
{
    return !overlay_blocks && !main_window_keyboard_focus(vkbd_has_focus);
}

/**
 * @brief Zapne nebo vypne přepínání oken ImGui klávesnicí (Ctrl+Tab, Ctrl+Shift+Tab).
 *
 * Jediné místo, které mění ConfigNavWindowingKeyNext a
 * ConfigNavWindowingKeyPrev kontextu ImGui. Hlavní okno ho volá každý snímek
 * s výsledkem main_window_nav_windowing_allowed(); overlay menu ho při
 * otevření jen vypne (aby vypnutí platilo hned od příštího snímku, i když
 * se otevřelo při fokusu okna nástroje) a zapnutí nechává na hlavním okně.
 * Zapnutí nastaví výchozí zkratky ImGui (na macOS se Super místo Ctrl,
 * podle io.ConfigMacOSXBehaviors); jiné hodnoty aplikace nepoužívá.
 * ImGui zkratky vyhodnocuje v NewFrame(), změna tedy platí od příštího snímku.
 *
 * @param enabled true = přepínač zapnout, false = vypnout.
 * @pre UI vlákno. Bez kontextu ImGui nedělá nic.
 * @post Zkratky přepínače oken v kontextu ImGui odpovídají @p enabled.
 */
static inline void main_window_nav_windowing_set(bool enabled)
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (!ctx) return;
    if (enabled) {
        const ImGuiKeyChord mod = ctx->IO.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        ctx->ConfigNavWindowingKeyNext = mod | ImGuiKey_Tab;
        ctx->ConfigNavWindowingKeyPrev = mod | ImGuiMod_Shift | ImGuiKey_Tab;
    } else {
        ctx->ConfigNavWindowingKeyNext = 0;
        ctx->ConfigNavWindowingKeyPrev = 0;
    }
}

/**
 * @brief Byla právě stisknuta klávesa, která z hlavního okna otevírá overlay menu?
 *
 * F10 bez Shift, Ctrl, Alt a Super, bez opakování při držení. Pravidlo se
 * záměrně vyhodnocuje jen ve větvi hlavního okna (MyImGui::MainWindowHasFocus),
 * ne v imgui_global_shortcuts(): ty volá při svém fokusu i okno debuggeru,
 * kde je F10 obvyklá klávesa pro krok přes instrukci a overlay otevírat nesmí.
 * Zavření overlaye klávesou F10 obsluhuje overlay sám.
 *
 * @return true = otevřít overlay menu.
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render(). Volající ověřil,
 *      že klávesy patří hlavnímu oknu (main_window_keyboard_enabled()).
 */
static inline bool main_window_overlay_open_key(void)
{
    const ImGuiIO &io = ImGui::GetIO();
    return !io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_F10, false);
}

/**
 * @brief Bylo právě stisknuto levé tlačítko myši s levým Alt (otevření overlay menu myší)?
 *
 * Platí jen levý Alt: pravý Alt (AltGr) slouží na řadě rozložení klávesnice
 * k psaní znaků a overlay otevírat nesmí.
 *
 * @return true = otevřít overlay menu.
 * @pre UI vlákno, uvnitř Begin() / End() hlavního okna emulátoru; zda je
 *      myš nad obrazem, ověřuje volající (ImGui::IsWindowHovered()).
 */
static inline bool main_window_overlay_open_click(void)
{
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_LeftAlt);
}

/**
 * @brief Pošle emulované klávesnici klidovou matici (žádná klávesa není stisknutá).
 *
 * Volá hlavní okno při ztrátě fokusu a overlay menu při otevření, aby
 * v emulovaném stroji nezůstala "držená" klávesa. Do klidu uvede i joystick
 * emulovaný na numerické klávesnici (joy_num_keypad_dev()): jeho stav jde
 * stejným skenem a bez toho by směr držený při otevření overlaye nebo při
 * ztrátě fokusu zůstal ve stroji stisknutý. Jde přes záznam vstupů
 * (replay_host_keyboard_submit()): matici stroje zapíše emu vlákno na
 * hranici snímku nebo v paused smyčce, při záznamu se klidová matice
 * převezme na hranici snímku, při přehrávání se zahodí.
 *
 * @pre UI vlákno.
 * @post Čekající sken klávesnice hostitele je klidová matice a klidový
 *       stav joysticku z numerické klávesnice (je-li některý port toho typu).
 */
static inline void main_window_keyboard_release(void)
{
    uint8_t idle_matrix[10];
    memset(idle_matrix, 0xff, sizeof(idle_matrix));
    replay_host_keyboard_submit(idle_matrix, joy_num_keypad_dev(), 0xff);
}

#endif /* MAIN_WINDOW_FOCUS_H */
