/**
 * @file message_window.cpp
 * @brief Modální okno se zprávou pro uživatele (informace, chyba, varování).
 *
 * Zprávu zakládá imgui_message_create() / imgui_message_vprintf() z
 * libovolného vlákna: z UI vlákna menu a dialogy, z emu vlákna zařízení
 * při zásahu přes frontu (device_request.h) nebo příkazu MCP. Zprávy čekají
 * ve frontě chráněné s_message_lock; okno (UI vlákno) si první zprávu pod
 * zámkem zkopíruje a kreslí kopii, po potvrzení ukáže další.
 *
 * Fronta má nejvýš MESSAGE_QUEUE_MAX zpráv. Chyba se zařadí před čekající
 * informace a varování (ne před zprávu, která se právě zobrazuje), takže
 * např. selhání uložení pásky se ukáže hned po potvrzení aktuální zprávy.
 * Plná fronta: nová chyba vytlačí nejstarší čekající zprávu, která chybou
 * není; jinak se nová zpráva zahodí (výpis na stderr) - chyba se neztratí,
 * dokud fronta nedrží jen chyby. Zpráva shodná (typ i text) s poslední ve
 * frontě se nepřidá, jen se jí zvýší počet ("(repeated N times)").
 *
 * Otevírání (imgui_modal.h): popup zprávy se otevře jen tehdy, když žádný
 * jiný popup otevřený není a neběží dotaz Virtual CMT
 * (imgui_cmt_tape_flow_active(): ten má přednost), a OpenPopup() se volá
 * jednou. Zavře-li popup zprávy někdo jiný, zpráva zůstává ve frontě a
 * ukáže se znovu, až bude volno; dialog, který uživatel nechá otevřený,
 * frontu jen pozdrží.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <glib.h>
#include <stdarg.h>
#include <deque>
#include <string>

// Lokalizace
#include "i18n.h"

#include "message_window.h"
#include "ui-imgui/imgui_modal.h"
#include "ui-imgui/cmt/imgui_cmt.h"

extern "C"
{
    void imgui_message_window(void);
    void imgui_message_create(ui_message_type_t type, const char *message);
    void imgui_message_vprintf(ui_message_type_t type, const char *format, va_list args);
};

/** @brief Nejvyšší počet zpráv ve frontě (včetně zobrazené). */
#define MESSAGE_QUEUE_MAX 8

/**
 * @brief Jedna zpráva ve frontě.
 * @invariant text není prázdný.
 */
struct st_MESSAGE
{
    ui_message_type_t type; /**< typ zprávy */
    std::string text;       /**< text (kopie) */
    unsigned count;         /**< kolikrát přišla po sobě (>= 1) */
};

/**
 * @brief Fronta zpráv, pod s_message_lock.
 *
 * Invariant: front() je zpráva, kterou okno zobrazuje (nebo zobrazí
 * v příštím snímku); s_shown = true, jakmile ji okno jednou nakreslilo.
 */
static std::deque<st_MESSAGE> s_queue;
/** @brief true = první zprávu fronty už okno zobrazilo (nesmí ji předběhnout chyba), pod s_message_lock. */
static bool s_shown = false;
/** @brief true = popup zprávy je otevřený (OpenPopup() proběhlo, nikdo ho nezavřel); jen UI vlákno. */
static bool s_popup_open = false;
/** @brief ImGui ID popupu zprávy (titulek se mění s typem a jazykem, ID ne). */
#define MESSAGE_POPUP_ID "###imgui_message_popup"
/** @brief Titulek právě zobrazené zprávy (jen UI vlákno). */
std::string g_message_title = "";
/** @brief Zámek s_queue a s_shown (statický GMutex nepotřebuje init). */
static GMutex s_message_lock;

/**
 * @brief Založí zprávu pro modální okno.
 *
 * @param type    Typ zprávy.
 * @param message Text (zkopíruje se); NULL nebo "" = nic.
 * @note Libovolné vlákno. Řazení a mez fronty viz popis souboru; zahozená
 *       zpráva se ohlásí na stderr.
 */
void imgui_message_create(ui_message_type_t type, const char *message)
{
    if ((message == NULL) || (message[0] == '\0'))
    {
        return;
    };
    bool dropped = false;
    bool repeated = false;
    g_mutex_lock(&s_message_lock);
    if (!s_queue.empty() && (s_queue.back().type == type) && (s_queue.back().text == message))
    {
        /* stejná zpráva po sobě (např. opakovaná chyba): jen počet */
        s_queue.back().count++;
        repeated = true;
    }
    else if (s_queue.size() >= MESSAGE_QUEUE_MAX)
    {
        /* plná fronta: chyba vytlačí nejstarší čekající ne-chybu */
        dropped = true;
        if (type == MESSAGE_TYPE_ERROR)
        {
            for (auto it = s_queue.begin() + (s_shown ? 1 : 0); it != s_queue.end(); ++it)
            {
                if (it->type != MESSAGE_TYPE_ERROR)
                {
                    s_queue.erase(it);
                    dropped = false;
                    break;
                };
            };
        };
    };
    if (!dropped && !repeated)
    {
        st_MESSAGE m = {type, message, 1u};
        if (type == MESSAGE_TYPE_ERROR)
        {
            /* chyba před čekající informace a varování, za zobrazenou zprávu a dřívější chyby */
            auto it = s_queue.begin() + (s_shown ? 1 : 0);
            while ((it != s_queue.end()) && (it->type == MESSAGE_TYPE_ERROR))
                ++it;
            s_queue.insert(it, m);
        }
        else
        {
            s_queue.push_back(m);
        };
    };
    g_mutex_unlock(&s_message_lock);
    if (dropped)
    {
        fprintf(stderr, "imgui_message: message queue is full, message dropped: %s\n", message);
    };
};

void imgui_message_vprintf(ui_message_type_t type, const char *format, va_list args)
{
    char *message = g_strdup_vprintf(format, args);

    imgui_message_create(type, message);
    g_free(message);
};

/**
 * @brief Odebere zobrazenou zprávu (po potvrzení nebo zavření okna); další čekající přijde na řadu.
 * @note UI vlákno; g_message_title patří jen UI vláknu.
 */
static void imgui_message_clear(void)
{
    g_mutex_lock(&s_message_lock);
    if (!s_queue.empty())
        s_queue.pop_front();
    s_shown = false;
    g_mutex_unlock(&s_message_lock);
    g_message_title = "";
    s_popup_open = false;
}


/**
 * @brief Nakreslí modální okno zprávy (popup už otevřený).
 *
 * Zavře-li popup někdo jiný (BeginPopupModal() vrátí false), zpráva zůstává
 * ve frontě a otevře se znovu, až bude volno.
 *
 * @param type    Typ zprávy (kopie pořízená pod zámkem).
 * @param message Text zprávy (kopie pořízená pod zámkem).
 * @param count   Kolikrát zpráva přišla po sobě.
 * @note UI vlákno.
 */
static void ShowModalMessage(ui_message_type_t type, const std::string &message, unsigned count)
{
    if (g_message_title == "")
    {
        switch (type)
        {
        case MESSAGE_TYPE_ERROR:
            g_message_title = _("Error");
            break;

        case MESSAGE_TYPE_WARNING:
            g_message_title = _("Warning");
            break;

        case MESSAGE_TYPE_INFO:
        default:
            g_message_title = _("Information");
            break;
        };
        g_message_title += MESSAGE_POPUP_ID;
    };

    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);

    if (!ImGui::BeginPopupModal(g_message_title.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        /* zavřel ho jiný modál: zpráva zůstává první ve frontě */
        s_popup_open = false;
        return;
    };

    ImGui::Text("%s", message.c_str());
    if (count > 1)
    {
        gchar *rep = g_strdup_printf(_("(repeated %u times)"), count);
        ImGui::TextUnformatted(rep);
        g_free(rep);
    };

    ImGui::Separator();

    if (ImGui::Button(_L("OK"), ImVec2(120, 0)))
    {
        imgui_message_clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
};

/**
 * @brief Nakreslí okno první zprávy fronty, pokud nějaká čeká a smí se ukázat.
 *
 * Popup se otevře jen při volnu (imgui_modal_can_open()) a mimo dotaz
 * Virtual CMT (imgui_cmt_tape_flow_active()); OpenPopup() se volá jednou.
 *
 * @note UI vlákno, každý snímek (main_window).
 */
void imgui_message_window(void)
{
    g_mutex_lock(&s_message_lock);
    bool have = !s_queue.empty();
    std::string message;
    ui_message_type_t type = MESSAGE_TYPE_INFO;
    unsigned count = 1;
    if (have)
    {
        message = s_queue.front().text;
        type = s_queue.front().type;
        count = s_queue.front().count;
    };
    g_mutex_unlock(&s_message_lock);

    if (!have)
    {
        return;
    };

    if (!s_popup_open)
    {
        /* dotaz Virtual CMT má přednost; cizí otevřený popup by OpenPopup() zavřel */
        if (imgui_cmt_tape_flow_active() || !imgui_modal_can_open())
            return;
        g_mutex_lock(&s_message_lock);
        s_shown = true; /* zobrazenou zprávu už chyba nepředběhne */
        g_mutex_unlock(&s_message_lock);
        g_message_title = "";
        ImGui::OpenPopup(MESSAGE_POPUP_ID);
        s_popup_open = true;
    };

    ShowModalMessage(type, message, count);
}


#ifdef MZTEST_HEADLESS
bool imgui_message_test_popup_open(void)
{
    return s_popup_open;
}


unsigned imgui_message_test_queued(void)
{
    g_mutex_lock(&s_message_lock);
    unsigned n = (unsigned)s_queue.size();
    g_mutex_unlock(&s_message_lock);
    return n;
}


unsigned imgui_message_test_front_count(void)
{
    g_mutex_lock(&s_message_lock);
    unsigned n = s_queue.empty() ? 0u : s_queue.front().count;
    g_mutex_unlock(&s_message_lock);
    return n;
}


bool imgui_message_test_at(unsigned index, ui_message_type_t *type, char *text, size_t size)
{
    g_mutex_lock(&s_message_lock);
    bool ok = index < s_queue.size();
    if (ok)
    {
        if (type)
            *type = s_queue[index].type;
        if (text && size)
            g_strlcpy(text, s_queue[index].text.c_str(), size);
    };
    g_mutex_unlock(&s_message_lock);
    return ok;
}


void imgui_message_test_clear(void)
{
    g_mutex_lock(&s_message_lock);
    s_queue.clear();
    s_shown = false;
    g_mutex_unlock(&s_message_lock);
    s_popup_open = false;
    g_message_title = "";
}
#endif
