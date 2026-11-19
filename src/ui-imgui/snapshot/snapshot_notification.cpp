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
#include "libs/imgui/imgui_internal.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/overlay/overlay.h"

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


/**
 * @brief Vykreslí čekající notifikaci: toast dole uprostřed, nebo modální chybové okno.
 *
 * Toast je okno bez vstupu a bez fokusu. Při otevřeném overlay menu
 * (imgui_overlay_is_open()) se každý snímek vynáší v pořadí kreslení nad
 * ostatní okna: overlay kryje celý viewport a bere si fokus, takže toast
 * vytvořený dřív by jinak zůstal pod ním a potvrzení Quick Save nebo
 * screenshotu spuštěného z overlaye by nebylo vidět. Při zavřeném overlayi
 * a při otevřeném popupu (modální dialog nad overlayem) se pořadí oken
 * nemění (toast může být pod oknem nástroje jako dřív). Při otevřeném
 * overlayi se toast navíc umisťuje vůči hlavnímu viewportu a připíná se k
 * němu (při více viewportech by jinak mohl skončit jako samostatné okno
 * systému mimo hlavní okno); při zavřeném overlayi zůstává pozice z
 * io.DisplaySize jako dřív.
 *
 * @pre UI vlákno, mezi ImGui::NewFrame() a ImGui::Render(), mimo Begin() / End() jiného okna.
 * @post Po uplynutí doby zobrazení nebo po potvrzení chyby notifikace zaniká.
 */
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
        /* jen při volnu: jinak by OpenPopup() zavřel jiný otevřený modál (imgui_modal.h) */
        imgui_modal_open_when_free(popup_title);

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
    const bool overlay_open = imgui_overlay_is_open();
    if (overlay_open) {
        /* Při otevřeném overlay menu patří toast do hlavního okna: s více viewporty jsou pozice oken v
         * souřadnicích plochy, proto se počítá od počátku hlavního viewportu, a okno se k němu připne, aby z něj
         * nevzniklo samostatné okno systému mimo hlavní okno. Bez více viewportů je počátek (0, 0) a pozice
         * stejná jako dřív. Při zavřeném overlayi se chování nemění. */
        const ImGuiViewport *main_vp = ImGui::GetMainViewport();
        window_pos = ImVec2(main_vp->Pos.x + main_vp->Size.x * 0.5f, main_vp->Pos.y + main_vp->Size.y - 60.0f);
        ImGui::SetNextWindowViewport(main_vp->ID);
    }

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
    /* nad overlay menu: to kryje celý viewport a fokusem se dostává dopředu; pořadí otevřených popupů se nemění */
    if (overlay_open && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::End();

    ImGui::PopStyleVar();
}
