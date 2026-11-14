/**
 * @file imgui_modal.h
 * @brief Pravidlo otevírání modálních dialogů ImGui v celé UI vrstvě.
 *
 * ImGui::OpenPopup() s jiným ID na téže úrovni zásobníku popupů zavře
 * popup, který je na ní otevřený (OpenPopupEx() -> ClosePopupToLevel()).
 * Dva modály, které se otevírají nezávisle (okno zpráv, dotazy Virtual
 * CMT, varování zařízení), by se tak navzájem zavíraly. Pravidlo: modál
 * se otevře jen tehdy, když žádný jiný popup otevřený není; jinak počká
 * na další snímek. OpenPopup() se volá jednou (ne každý snímek), dokud je
 * vlastní popup otevřený.
 *
 * Vlákna: jen UI vlákno uvnitř ImGui snímku.
 */

#ifndef IMGUI_MODAL_H
#define IMGUI_MODAL_H

#include "libs/imgui/imgui.h"

/**
 * @brief Smí se teď otevřít modální popup (žádný popup není otevřený na žádné úrovni)?
 * @return true = žádný popup (modál, menu, kontextové menu) není otevřený.
 * @pre UI vlákno uvnitř ImGui snímku.
 */
static inline bool imgui_modal_can_open(void)
{
    return !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

/**
 * @brief Otevře modál @p id, není-li otevřený a je-li volno (imgui_modal_can_open()).
 *
 * Náhrada za ImGui::OpenPopup() volané každý snímek: vlastní otevřený
 * popup nechá být, cizí otevřený popup nezavře - modál počká na snímek,
 * kdy bude volno. Volající dál volá BeginPopupModal() každý snímek.
 *
 * @param id ID popupu (jako pro OpenPopup(); počítá se z aktuálního zásobníku ID).
 * @return true = popup je (nebo byl právě) otevřený; false = čeká.
 * @pre UI vlákno uvnitř ImGui snímku.
 */
static inline bool imgui_modal_open_when_free(const char *id)
{
    if (ImGui::IsPopupOpen(id))
        return true;
    if (!imgui_modal_can_open())
        return false;
    ImGui::OpenPopup(id);
    return true;
}

#endif /* IMGUI_MODAL_H */
