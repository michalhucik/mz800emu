#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_process.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

// Lokalizace
#include "i18n.h"

#include "ui-imgui/bootstrap/images.h"
#include "emulator/cfgmain.h"
#include "build_revision/build_revision.h"
#include "emulator/mzarch/mzarch_platform.h"

extern "C"
{
    void imgui_ShowAboutWindow(bool *p_open);
};

static void CenteredText(const char *text)
{
    float window_width = ImGui::GetWindowSize().x;
    float text_width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((window_width - text_width) * 0.5f);
    ImGui::Text("%s", text);
}

static void CenteredColorText(ImVec4 txtColor, const char *text)
{
    float window_width = ImGui::GetWindowSize().x;
    float text_width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((window_width - text_width) * 0.5f);
    ImGui::TextColored(txtColor, "%s", text);
}

/* Vrátí počet UTF-8 znaků v řetězci */
static int utf8_strlen(const char *s)
{
    int count = 0;
    while (*s)
    {
        /* Přeskočíme continuation bajty (10xxxxxx) */
        if ((*s & 0xC0) != 0x80)
            count++;
        s++;
    }
    return count;
}

/* Vrátí bajtový offset pro n-tý UTF-8 znak */
static int utf8_byte_offset(const char *s, int char_count)
{
    const char *p = s;
    int count = 0;
    while (*p && count < char_count)
    {
        if ((*p & 0xC0) != 0x80)
            count++;
        if (count < char_count)
            p++;
        else
            break;
    }
    /* Přeskočíme zbývající continuation bajty aktuálního znaku */
    if (*p)
    {
        p++;
        while (*p && (*p & 0xC0) == 0x80)
            p++;
    }
    return (int)(p - s);
}

static void CenteredTextLen(const char *text, int char_len)
{
    if (!text || char_len <= 0)
        return;

    float window_width = ImGui::GetWindowSize().x;
    float text_width = ImGui::CalcTextSize(text).x;

    ImGui::SetCursorPosX((window_width - text_width) * 0.5f);

    /* Převod počtu UTF-8 znaků na bajtový offset */
    int byte_offset = utf8_byte_offset(text, char_len);

    std::string partialText = std::string(text, byte_offset);
    ImGui::Text("%s", partialText.c_str());
    /* Zbývající znaky vykreslíme barvou pozadí */
    ImVec4 bgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    ImGui::SameLine(0, 0);
    ImGui::TextColored(bgColor, "%s", text + byte_offset);
}

static void CenteredImage(ui_glimage_t *img)
{
    float window_width = ImGui::GetWindowSize().x;
    float img_width = img->width;
    ImGui::SetCursorPosX((window_width - img_width) * 0.5f);
    ImGui::Image(img->texture, ImVec2(img->width, img->height));
}

/**
 * @brief Vykreslí tlačítko zarovnané na střed okna.
 *
 * @param label Text tlačítka.
 * @return True, pokud bylo tlačítko kliknuto.
 */
static bool CenteredButton(const char *label)
{
    float window_width = ImGui::GetWindowSize().x;
    float button_width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;

    ImGui::SetCursorPosX((window_width - button_width) * 0.5f);
    return ImGui::Button(label);
}

static void CenteredTextLinkOpenURL(const char *label, const char *url)
{
    float window_width = ImGui::GetWindowSize().x;
    float text_width = ImGui::CalcTextSize(label).x;

    ImGui::SetCursorPosX((window_width - text_width) * 0.5f);
    ImGui::TextLinkOpenURL(label, url);
}

#define EPIC_TEXT_SPEED_MS 100
#define SHOW_BUTTON_TIMEOUT ((4 * 60 * 1000) / EPIC_TEXT_SPEED_MS) // 4 minuty

/**
 * @brief Detekuje absolutní cestu k aktuálně běžící binárce.
 *
 * Cesta je potřeba pro spuštění další instance téhož exe v alternativním
 * launch módu (musí běžet v samostatném procesu kvůli vlastnímu SDL/ImGui
 * kontextu).
 *
 * @return Newly-allocated UTF-8 string s absolutní cestou, nebo @c NULL
 *         při selhání detekce. Volající uvolní přes @c g_free().
 */
static char *get_current_executable_path(void)
{
#ifdef _WIN32
    /* Windows: GetModuleFileNameW vrací UTF-16, převedeme na UTF-8 přes glib. */
    wchar_t wbuf[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        return NULL;
    };
    return g_utf16_to_utf8((const gunichar2 *)wbuf, -1, NULL, NULL, NULL);
#else
    /* Linux: /proc/self/exe je symlink na běžící binárku. */
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
    {
        return NULL;
    };
    buf[len] = '\0';
    return g_strdup(buf);
#endif
}

/**
 * @brief Spustí modul @c spdfd jako samostatný proces.
 *
 * Volá @c SDL_CreateProcess s cestou k aktuálně běžícímu exe a parametrem.
 * Subproces žije paralelně s hlavním procesem a po jeho
 * skončení hlavní proces pokračuje bez přerušení.
 *
 * SDL_Process handle je okamžitě uvolněn - subproces nepřebíráme (nečekáme
 * na jeho exit, neřešíme jeho stdout/stderr).
 */
static void launch_spdfd_subprocess(void)
{
    char *exe_path = get_current_executable_path();
    if (!exe_path)
    {
        SDLAPP_ERROR("Cannot detect own executable path for spdfd subprocess");
        return;
    };

    /* Handshake tabulka pro --spdfd. Pořadí je významné - subproces ji
     * dekóduje a porovná s argumenty. Klíč a tabulka jsou duplikované
     * v spdfd_main.cpp; obě strany musí zůstat synchronizované. */
    static constexpr uint32_t K = 0xC0FEu;
    static constexpr uint32_t TABLE[3] = { 0xC3DEu, 0xC522u, 0xC242u };

    char p1[16], p2[16], p3[16];
    std::snprintf(p1, sizeof(p1), "%u", TABLE[0] ^ K);
    std::snprintf(p2, sizeof(p2), "%u", TABLE[1] ^ K);
    std::snprintf(p3, sizeof(p3), "%u", TABLE[2] ^ K);

    const char *args[] = { exe_path, "--spdfd", p1, p2, p3, NULL };
    SDL_Process *proc = SDL_CreateProcess(args, false);
    if (!proc)
    {
        SDLAPP_ERROR("SDL_CreateProcess failed: %s", SDL_GetError());
    }
    else
    {
        /* Handle už nepotřebujeme - subproces žije nezávisle. */
        SDL_DestroyProcess(proc);
    };

    g_free(exe_path);
}

/**
 * @brief Vrátí dekódovaný label, který nepotřebuje další komentář.
 *
 * @return Pointer na NUL-terminovaný UTF-8 řetězec. Buffer žije po celý
 *         běh programu.
 */
static const char *spdfd_get_button_label(void)
{
    static constexpr uint8_t LABEL_KEY = 0x5Au;
    static constexpr uint8_t LABEL_ENC[] = {
        0x08, 0x2F, 0x34, 0x7A, 0x1F, 0x3B, 0x29, 0x2E, 0x3F, 0x28, 0x7A,
        0x1F, 0x3D, 0x3D, 0x7A, 0x1D, 0x3B, 0x37, 0x3F, 0x7A, 0x60, 0x73,
    };
    static char label[sizeof(LABEL_ENC) + 1] = { 0 };
    if (label[0] == '\0')
    {
        for (size_t i = 0; i < sizeof(LABEL_ENC); ++i)
        {
            label[i] = static_cast<char>(LABEL_ENC[i] ^ LABEL_KEY);
        };
        label[sizeof(LABEL_ENC)] = '\0';
    };
    return label;
}

static int g_epic_char_count = 0;
static int g_epic_line_index = 0;
static int g_show_button_counter = SHOW_BUTTON_TIMEOUT;
static bool g_show_button_bypass = false;

static void printEpicText(bool *p_open)
{
    static auto start_time = std::chrono::steady_clock::now();

    const char *epic_txt[] = {
        {"\n"},
        {_("Come closer, my friend.")},
        {_("Sit down and listen to a tale from ancient times.")},
        {_("From a time when data were real zeros and ones,")},
        {_("and computers were still real computers...")},
        {"\n"},
        {_("A tale of a computer that was born in the land of the rising sun!")},
        {NULL}};

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

    if (elapsed > EPIC_TEXT_SPEED_MS)
    {
        start_time = now;
        g_epic_char_count++;

        if (epic_txt[g_epic_line_index] && g_epic_char_count > utf8_strlen(epic_txt[g_epic_line_index]))
        {
            g_epic_char_count = 0;
            g_epic_line_index++;
        }
        else
        {
            if (g_cfgmain_ini_file_exists && (g_show_button_counter > 0))
            {
                g_show_button_counter--;
            };
        };
    }

    for (int i = 0; i < g_epic_line_index; i++)
    {
        CenteredText(epic_txt[i]);
    }

    if (epic_txt[g_epic_line_index] != NULL)
    {
        CenteredTextLen(epic_txt[g_epic_line_index], g_epic_char_count);
    }

    if (epic_txt[g_epic_line_index] != NULL)
    {
        int next_line = (g_epic_char_count == 0) ? g_epic_line_index : g_epic_line_index + 1;
        ImVec4 bgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        // ImVec4 bgColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        for (int i = next_line; epic_txt[i] != NULL; i++)
        {
            CenteredColorText(bgColor, epic_txt[i]);
        };
    };

    {
        static constexpr uint8_t S[3] = { 0x0D, 0x1B, 0x07 };
        int ok = 0;
        for (size_t i = 0; i < sizeof(S); ++i)
        {
            int o = (S[i] ^ (0x40 + (int)i)) - 'A';
            if (ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_A + o))) ok++;
        }
        if (ok == (int)sizeof(S)) g_show_button_bypass = true;
    }

    if (g_show_button_bypass || g_show_button_counter <= 0)
    {
        ImGui::Text("\n");
        if (CenteredButton(spdfd_get_button_label()))
        {
            g_show_button_counter = SHOW_BUTTON_TIMEOUT;
            g_show_button_bypass = false;
            launch_spdfd_subprocess();
            if (p_open) *p_open = false;
        };
        ImGui::Text("\n");
    };
}

void imgui_ShowAboutWindow(bool *p_open)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize | // Okno se automaticky přizpůsobí obsahu
        ImGuiWindowFlags_NoResize |         // Zakáže změnu velikosti
        ImGuiWindowFlags_NoScrollbar |      // Zakáže scrollbar
        ImGuiWindowFlags_NoCollapse |       // Zakáže možnost sbalení okna
        ImGuiWindowFlags_NoDocking;         // Zakáže možnost dockování

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20)); // Větší okraj

    const char *window_title_text;
    switch (g_mzarch_platform_numeric)
    {
    case 700:
        window_title_text = _("About MZ-700 Emulator");
        break;
    case 1500:
        window_title_text = _("About MZ-1500 Emulator");
        break;
    default:
        window_title_text = _("About MZ-800 Emulator");
        break;
    };

    std::string window_title = std::string(window_title_text) + "##about_window";

    /* Multi-viewport ImGui aktivní (ImGuiConfigFlags_ViewportsEnable):
     * About okno bylo vykreslováno jako separátní OS-level SDL window,
     * který OS dával do Z-orderu pod main / pod terminál (běžné na
     * Windows). ImGuiWindowClass.ViewportFlagsOverrideSet s TopMost
     * vynutí, že SDL platform window pro About bude vždy nahoře v OS
     * Z-orderu (= SDL_WINDOW_ALWAYS_ON_TOP). */
    {
        ImGuiWindowClass wc;
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_TopMost;
        ImGui::SetNextWindowClass(&wc);
    }

    if (!ImGui::Begin(window_title.c_str(), p_open, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        g_epic_line_index = 0;
        g_epic_char_count = 0;
        return;
    };

    if (ImGui::IsWindowAppearing())
    {
        g_show_button_bypass = false;
    };

    /* Při prvním zobrazení okna (Appearing) ho přesuneme do popředí.
     * Bez toho by se okno otevřené při first-run mohlo objevit pod
     * Version Check Setup oknem a uživatel by ho přehlédl. */
    if (ImGui::IsWindowAppearing())
        ImGui::SetWindowFocus();

    ImGui::SetWindowFontScale(1.75f);
    const char *head_title;
    switch (g_mzarch_platform_numeric)
    {
    case 700:
        head_title = _("Sharp MZ-700 - Emulator");
        break;
    case 1500:
        head_title = _("Sharp MZ-1500 - Emulator");
        break;
    default:
        head_title = _("Sharp MZ-800 - Emulator");
        break;
    };

    CenteredText(head_title);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetWindowFontScale(0.75f);

    GString *version = g_string_new(_("Version: "));
    g_string_append(version, CFGMAIN_EMULATOR_VERSION_NUM_STRING);
    g_string_append(version, " ");
    g_string_append(version, CFGMAIN_EMULATOR_VERSION_TAG);
    g_string_append(version, _(", Rev.: "));
    g_string_append_printf(version, "%d", build_revision_get_int());
    g_string_append(version, _(", Build: "));
    g_string_append(version, cfgmain_get_build_datetime());
    g_string_append(version, " (");
    g_string_append(version, SDL_GetPlatform());
    g_string_append(version, ")");
    CenteredText(version->str);
    g_string_free(version, TRUE);

    /* Build ze zdrojů repozitáře NAS1 (vývojové buildy): doplnit větev
     * a commit, aby šlo build jednoznačně dohledat. Oficiální (GitHub)
     * a ostatní buildy řádek nemají. Příznak neuložených změn se
     * připojí ke zkrácenému hashi jako "-dirty" (konvence git describe). */
    if (build_revision_get_repo_origin() == BUILD_REVISION_ORIGIN_NAS1)
    {
        const char *branch = build_revision_is_detached() ? _("detached") : build_revision_get_branch();
        GString *commit = g_string_new(build_revision_get_commit_short());
        if (build_revision_is_dirty() == 1)
        {
            g_string_append(commit, "-dirty");
        };
        GString *source = g_string_new(NULL);
        g_string_printf(source, _("Source: NAS1 repository, branch %s, commit %s"), branch, commit->str);
        CenteredText(source->str);
        g_string_free(source, TRUE);
        g_string_free(commit, TRUE);
    };

    ImGui::SetWindowFontScale(1.0f);

    ImGui::Text("\n");

    const char *photo_name;
    switch (g_mzarch_platform_numeric)
    {
    case 700:
        photo_name = "mz700-photo";
        break;
    case 1500:
        photo_name = "mz1500-photo";
        break;
    default:
        photo_name = "mz800-photo";
        break;
    };

    ui_glimage_t *img = imgui_images_get_image_by_name(photo_name);
    if (!img)
    {
        SDLAPP_ERROR("Failed to get image");
        return;
    };
    CenteredImage(img);

    printEpicText(p_open);

    ImGui::Separator();

    ImGui::Text("\n");
    CenteredTextLinkOpenURL("www.ordoz.com/mz800emu", "https://www.ordoz.com/mz800emu");
    CenteredText("\xc2\xa9 2015 - 2025 - Michal Hu\xc4\x8d\xc3\xadk - ORDOZ");
    CenteredText("Email: hucik@ordoz.com");

    ImGui::Text("\n");
    CenteredText(_("Special thanks to"));
    CenteredText("Zden\xc4\x9bk \xc3\x81" "dler, Roman Dolej\xc5\xa1\xc3\xad, Petr Odehnal, Martin Veverka");
    CenteredText(_("and the many other \"Sharp's Men\" who explored and"));
    CenteredText(_("documented the hidden secrets of the Sharp MZ-800 computer."));

    ImGui::Text("\n");
    ImGui::Separator();
    ImGui::SetWindowFontScale(0.75f);
    CenteredText(_("This program comes with absolutely no warranty."));
    CenteredText(_("See the GNU General Public License, version 3 or later for details."));
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Text("\n");
    if (p_open && CenteredButton(_("Close this window")))
    {
        *p_open = false;
    };

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        *p_open = false;
    };

    ImGui::End();
    ImGui::PopStyleVar();

    if (!*p_open)
    {
        g_epic_line_index = 0;
        g_epic_char_count = 0;
        g_show_button_counter = SHOW_BUTTON_TIMEOUT;
    };
}
