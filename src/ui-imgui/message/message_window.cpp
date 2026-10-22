/**
 * @file message_window.cpp
 * @brief Modální okno se zprávou pro uživatele (informace, chyba, varování).
 *
 * Zprávu zakládá imgui_message_create() / imgui_message_vprintf() z
 * libovolného vlákna: z UI vlákna menu a dialogy, z emu vlákna zařízení
 * při zásahu přes frontu (device_request.h) nebo příkazu MCP. Text a typ
 * čekající zprávy proto chrání s_message_lock; okno (UI vlákno) si je
 * pod zámkem zkopíruje a kreslí kopii. Najednou čeká nejvýš jedna zpráva,
 * další se do jejího potvrzení zahodí (jen výpis na stderr).
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <stdarg.h>
#include <string>

// Lokalizace
#include "i18n.h"

#include "message_window.h"

extern "C"
{
    void imgui_message_window(void);
    void imgui_message_create(ui_message_type_t type, const char *message);
    void imgui_message_vprintf(ui_message_type_t type, const char *format, va_list args);
};

static const char *message_type_txt[] = {
    "INFO",
    "ERROR",
    "WARNING",
};

/** @brief Text čekající zprávy ("" = žádná), pod s_message_lock. */
std::string g_message = "";
/** @brief Typ čekající zprávy, pod s_message_lock. */
ui_message_type_t g_message_type = MESSAGE_TYPE_INFO;
/** @brief Titulek právě zobrazené zprávy (jen UI vlákno). */
std::string g_message_title = "";
/** @brief Zámek g_message a g_message_type (statický GMutex nepotřebuje init). */
static GMutex s_message_lock;

/**
 * @brief Založí zprávu pro modální okno.
 *
 * @param type    Typ zprávy.
 * @param message Text (zkopíruje se); NULL = nic.
 * @note Libovolné vlákno. Když už jiná zpráva čeká na potvrzení, nová se
 *       zahodí a ohlásí na stderr.
 */
void imgui_message_create(ui_message_type_t type, const char *message)
{
    if (message == NULL)
    {
        return;
    };
    g_mutex_lock(&s_message_lock);
    bool busy = (g_message != "");
    if (!busy)
    {
        g_message_type = type;
        g_message = message;
    };
    g_mutex_unlock(&s_message_lock);
    if (busy)
    {
        fprintf(stderr, "imgui_message_basic: message already set\n");
    };
};

void imgui_message_vprintf(ui_message_type_t type, const char *format, va_list args)
{
    char *message = g_strdup_vprintf(format, args);

    imgui_message_create(type, message);
    g_free(message);
};

/**
 * @brief Smaže čekající zprávu (po potvrzení nebo zavření okna).
 * @note UI vlákno; g_message_title patří jen UI vláknu.
 */
static void imgui_message_clear(void)
{
    g_mutex_lock(&s_message_lock);
    g_message_type = MESSAGE_TYPE_INFO;
    g_message = "";
    g_mutex_unlock(&s_message_lock);
    g_message_title = "";
}

/**
 * @brief Nakreslí modální okno zprávy.
 * @param type    Typ zprávy (kopie pořízená pod zámkem).
 * @param message Text zprávy (kopie pořízená pod zámkem).
 * @note UI vlákno.
 */
void ShowModalMessage(ui_message_type_t type, std::string message)
{
    if (g_message_title == "")
    {
        switch (type)
        {
        case MESSAGE_TYPE_INFO:
            g_message_title = _("Information");
            break;

        case MESSAGE_TYPE_ERROR:
            g_message_title = _("Error");
            break;

        case MESSAGE_TYPE_WARNING:
            g_message_title = _("Warning");
            break;

        default:
            g_message_title = _("Information");
            break;
        };

        g_message_title += "##popup_{message_type_txt[mtype]}##";
    };

    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);

    ImGui::OpenPopup(message_type_txt[type]);

    if (ImGui::BeginPopupModal(message_type_txt[type], NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("%s", message.c_str());
        ImGui::Separator();

        if (ImGui::Button(_L("OK"), ImVec2(120, 0)))
        {
            imgui_message_clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else
    {
        imgui_message_clear();
    };
};

/**
 * @brief Nakreslí okno čekající zprávy, pokud nějaká čeká.
 * @note UI vlákno, každý snímek (main_window).
 */
void imgui_message_window(void)
{
    g_mutex_lock(&s_message_lock);
    std::string message = g_message;
    ui_message_type_t type = g_message_type;
    g_mutex_unlock(&s_message_lock);

    if (message == "")
    {
        return;
    };

    ShowModalMessage(type, message);
}
