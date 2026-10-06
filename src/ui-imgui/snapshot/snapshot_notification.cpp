/**
 * @file snapshot_notification.cpp
 * @brief Notifikace pro snapshot operace — toast zprávy a chybové dialogy
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <SDL3/SDL.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

/* Stav notifikace */
static bool s_notification_active = false;
static bool s_notification_is_error = false;
static char s_notification_message[512] = "";
/* Titulek chybového okna (už přeložený); prázdný = "Snapshot Error" */
static char s_notification_error_title[128] = "";
static Uint64 s_notification_start_time = 0;

/* Doba zobrazení notifikace v ms */
#define NOTIFICATION_DURATION_MS 3000
#define NOTIFICATION_FADE_MS 500


/**
 * @brief Zobrazí notifikaci s volitelným titulkem chybového okna.
 *
 * Úspěch = toast dole uprostřed (3 s s dozníváním), chyba = modální okno
 * s tlačítkem OK. Nová notifikace nahradí předchozí.
 *
 * @param message     Text (už přeložený; zkopíruje se, zkrátí na 511 znaků).
 * @param is_error    true = modální chybové okno.
 * @param error_title Titulek chybového okna (už přeložený; zkopíruje se),
 *                    NULL = "Snapshot Error". Pro toast se ignoruje.
 * @par Vlákna Jen UI vlákno.
 */
extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title)
{
    snprintf(s_notification_message, sizeof(s_notification_message), "%s", message);
    snprintf(s_notification_error_title, sizeof(s_notification_error_title), "%s", error_title ? error_title : "");
    s_notification_is_error = is_error;
    s_notification_active = true;
    s_notification_start_time = SDL_GetTicks();
}


/**
 * @brief Zda je právě otevřené chybové (modální) okno notifikace.
 *
 * Volající, který nechce běžnou notifikací (toast) přepsat dosud nepotvrzenou
 * chybu, se podle toho rozhodne (snapshot_notification_show_ex() sama
 * předchozí notifikaci vždy nahradí).
 *
 * @return true dokud uživatel chybové okno nezavřel tlačítkem OK.
 * @par Vlákna Jen UI vlákno.
 */
extern "C" bool snapshot_notification_error_active(void)
{
    return s_notification_active && s_notification_is_error;
}


extern "C" void snapshot_notification_show(const char *message, bool is_error)
{
    snapshot_notification_show_ex(message, is_error, NULL);
}


extern "C" void imgui_snapshot_notification(void)
{
    if (!s_notification_active)
        return;

    Uint64 elapsed = SDL_GetTicks() - s_notification_start_time;

    /* Chybové hlášení — modální dialog (bez fadeout) */
    if (s_notification_is_error) {
        /* Stabilní ID ###NotificationError: titulek se mění (snapshot / video záznam). */
        char popup_title[192];
        snprintf(popup_title, sizeof(popup_title), "%s###NotificationError",
                 s_notification_error_title[0] ? s_notification_error_title : _("Snapshot Error"));
        ImGui::OpenPopup(popup_title);

        if (ImGui::BeginPopupModal(popup_title, NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", s_notification_message);
            ImGui::Spacing();

            if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
                s_notification_active = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return;
    }

    /* Toast notifikace pro úspěch — overlay s fadeout */
    if (elapsed > NOTIFICATION_DURATION_MS) {
        s_notification_active = false;
        return;
    }

    /* Vypočítat průhlednost (fadeout v posledních 500ms) */
    float alpha = 1.0f;
    if (elapsed > NOTIFICATION_DURATION_MS - NOTIFICATION_FADE_MS) {
        alpha = (float)(NOTIFICATION_DURATION_MS - elapsed) / (float)NOTIFICATION_FADE_MS;
    }

    /* Pozice: dole uprostřed */
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 window_pos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 60.0f);
    ImVec2 window_pivot = ImVec2(0.5f, 1.0f);

    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pivot);
    ImGui::SetNextWindowBgAlpha(alpha * 0.85f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    if (ImGui::Begin("##SnapshotNotification", NULL, flags)) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, alpha), "%s", s_notification_message);
    }
    ImGui::End();

    ImGui::PopStyleVar();
}
