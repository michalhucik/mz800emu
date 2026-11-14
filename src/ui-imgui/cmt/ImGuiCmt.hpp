#ifndef IMGUI_CMT_HPP
#define IMGUI_CMT_HPP

#include <vector>
#include <string>

#include "libs/cmtspeed/cmtspeed.h"
#include "cmt/cmtext_block.h"

#include "ui/cmt/UiCmt.hpp"

/**
 * @brief Stabilní ID popupu editoru vlastních pulzů (část za "###" se nepřekládá).
 *
 * OpenPopup() dostane jen toto ID, BeginPopupModal() přeložený titulek
 * a toto ID; obě volání tak dají totéž ID v každém jazyce.
 */
#define IMGUI_CMT_PULSES_POPUP_ID "###cmt_custom_tape_pulses"

typedef struct ImGuiCmtSpeedList_t
{
    en_CMTSPEED cmtspeed;
    std::string cmtspeed_txt;
    std::string label_txt;
} ImGuiCmtSpeedList_t;

class ImGuiCmt final
{
public:
    /// Zakázání vytváření instancí.
    ImGuiCmt() = delete;

    /// Zakázání kopírování a přiřazování.
    ImGuiCmt(const ImGuiCmt &) = delete;
    ImGuiCmt &operator=(const ImGuiCmt &) = delete;

    // ziskani pole s rychlostmi pro MZ
    static std::vector<ImGuiCmtSpeedList_t> getMzSpeedList(void);
    static std::vector<ImGuiCmtSpeedList_t> getMzSpeedListDefault(void);

    // ziskani pole s rychlostmi pro ZX
    static std::vector<ImGuiCmtSpeedList_t> getZxSpeedList(void);

    /**
     * @brief Vyžádá otevření editoru vlastních délek pulzů.
     *
     * Editor je jeden pro celé UI; otevře se při nejbližším volání
     * drawPulsesEditor() se stejným scope (OpenPopup musí proběhnout ve
     * stejném ID stacku jako BeginPopupModal, proto se odkládá).
     *
     * @param scope Identifikace volajícího okna (statický řetězec).
     * @param owner Údaj volajícího vrácený po potvrzení (např. index bloku).
     * @param initial Počáteční délky pulzů (kopírují se; tlačítko Revert se
     *        k nim vrací).
     */
    static void openPulsesEditor(const char *scope, int owner, const st_MZTAPE_PULSES_LENGTH *initial);

    /**
     * @brief Vykreslí editor vlastních délek pulzů, pokud patří danému scope.
     *
     * Volá se každý snímek z okna, které editor otevírá (mimo combo a
     * tabulku). Neplatné délky (mimo rozsah mztape_pulses_set_us) nejde
     * potvrdit.
     *
     * Popup má stabilní ID IMGUI_CMT_PULSES_POPUP_ID (titulek se překládá,
     * ID ne), takže se po přepnutí jazyka nezavře ani nezmění uložený stav.
     *
     * @param scope Identifikace volajícího okna (stejný řetězec jako u open).
     * @param[out] owner Při potvrzení údaj z openPulsesEditor().
     * @param[out] result Při potvrzení nové délky pulzů.
     * @return true právě ve snímku, kdy uživatel potvrdil OK.
     */
    static bool drawPulsesEditor(const char *scope, int *owner, st_MZTAPE_PULSES_LENGTH *result);
};

#endif // IMGUI_CMT_HPP
