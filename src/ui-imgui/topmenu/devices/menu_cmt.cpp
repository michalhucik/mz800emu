/**
 * @file   menu_cmt.cpp
 * @brief  Nabídka CMT hlavního menu (HW & Devices > CMT): CMT hack, výchozí rychlost, Fast Open, okno Virtual CMT, volby CMT.
 *
 * Stav jádra nabídka čte pod device_state_lock(), který drží
 * imgui_topmenu_body() po dobu kreslení menu zařízení. CMT hack, CPU Boost,
 * výchozí rychlost, Fix FNAME Terminator a Direct Save mění zásahy fronty zařízení
 * (cmt_request_*(), imgui_cmt_menu_set_cpu_boost()), které provede emu
 * vlákno; Fast Open CMT File & Play... jde automatem akcí okna kazety
 * (imgui_cmt_tape_action()) a podnabídka Recent (naposledy vložené pásky,
 * imgui_menu_recent()) stejným automatem přes imgui_cmt_tape_open_path(),
 * jako list výměny pásky v overlay menu. Přímo z UI vlákna mění jádro jen Enable MZF
 * Size Check (cmt_mzfsize_check_set()) a Device for Direct MZF Start
 * (bootdev_setting_set_text(), nastavení s vlastním zámkem, stroj nemění);
 * Show Virtual CMT přepíná jen příznak okna (g_gui->showVirtualCmtWindow).
 */

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/topmenu/menu_recent.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"
extern "C"
{
#include "baseui/baseui.h"
}

#include "emulator/hw-generic/cmt/cmt.h"
#include "emulator/hw-generic/cmt/cmthack.h"
#include "emulator/hw-generic/cmt/cmt_request.h"
#include "ui-imgui/cmt/imgui_cmt.h"
#include "ui-imgui/cmt/cmt_view.h"
#include "libs/cmtspeed/cmtspeed.h"

#include "emulator/mzarch/bootstrap_device.h"
#include "ui-imgui/imgui_modal.h"

#include <stdio.h>
#include <string.h>
#ifdef MZTEST_HEADLESS
#include <string>
#include <vector>
#endif

extern "C"
{
    void imgui_menu_cmt(void);
    void imgui_menu_cmt_bootdev_popup(void);
#ifdef MZTEST_HEADLESS
    bool imgui_menu_cmt_test_item_rect(const char *name, float *x0, float *y0, float *x1, float *y1);
#endif
}

/** @brief Stabilní ImGui ID modálu Custom value... (titulek se překládá). */
#define MENU_CMT_BOOTDEV_POPUP_ID "###menu_cmt_bootdev_custom"

/**
 * @brief Stav modálu Custom value... volby Device for Direct MZF Start.
 *
 * Patří jen UI vláknu. open_request nastaví položka menu (ta se kreslí
 * uvnitř nabídek, které klik zavře); modál otevře a kreslí
 * imgui_menu_cmt_bootdev_popup() z hlavního okna mimo nabídky, až je volno
 * (imgui_modal_open_when_free()). buf drží rozepsaný zápis do potvrzení
 * nebo zrušení.
 *
 * @invariant buf je ukončený nulou.
 */
static struct
{
    bool open_request;           /**< položka Custom value... žádá otevření modálu */
    bool focus;                  /**< při příštím kreslení dát fokus vstupu */
    char buf[BOOTDEV_TEXT_MAX];  /**< zápis hodnoty (desítkově nebo šestnáctkově) */
} s_bootdev_custom;

#ifdef MZTEST_HEADLESS
/** @brief Obdélník prvku nabídky zaznamenaný pro testy (jen testovací build). */
struct st_MENU_CMT_TEST_ITEM
{
    std::string name; /**< jméno prvku (stabilní ID bez ###) */
    ImVec2 min;       /**< levý horní roh */
    ImVec2 max;       /**< pravý dolní roh */
};
/** @brief Prvky nabídky z posledního kreslení (testy hledají řádky podle jména). */
static std::vector<st_MENU_CMT_TEST_ITEM> s_menu_cmt_test_items;
#endif

/**
 * @brief Zaznamená obdélník naposledy nakresleného prvku pro testy (v aplikaci nic).
 * @param name Jméno prvku (stabilní ID bez ###).
 * @pre Uvnitř okna ImGui, hned za prvkem.
 */
static void menu_cmt_test_record(const char *name)
{
#ifdef MZTEST_HEADLESS
    for (st_MENU_CMT_TEST_ITEM &it : s_menu_cmt_test_items)
    {
        if (it.name == name)
        {
            it.min = ImGui::GetItemRectMin();
            it.max = ImGui::GetItemRectMax();
            return;
        };
    };
    s_menu_cmt_test_items.push_back({name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
#else
    (void)name;
#endif
}

#ifdef MZTEST_HEADLESS
/**
 * @brief Obdélník prvku nabídky CMT z posledního kreslení (jen testy).
 *
 * Zaznamenané prvky: `bootdev_menu` (položka podnabídky Device for Direct
 * MZF Start), `bootdev_cmt`, `bootdev_fd`, `bootdev_qd`, `bootdev_rd`,
 * `bootdev_custom` (řádky podnabídky), `direct_save` (položka Direct Save
 * v CMT Options), `echo` a `mzfsize_autofix` (položky Echo a Fix MZF Size
 * Automatically v CMT Options).
 *
 * @param name Jméno prvku. @param[out] x0 @param[out] y0 Levý horní roh.
 * @param[out] x1 @param[out] y1 Pravý dolní roh.
 * @return true = prvek byl někdy nakreslen (hodnoty z posledního kreslení).
 */
bool imgui_menu_cmt_test_item_rect(const char *name, float *x0, float *y0, float *x1, float *y1)
{
    for (const st_MENU_CMT_TEST_ITEM &it : s_menu_cmt_test_items)
    {
        if (it.name == name)
        {
            *x0 = it.min.x;
            *y0 = it.min.y;
            *x1 = it.max.x;
            *y1 = it.max.y;
            return true;
        };
    };
    return false;
}
#endif

/**
 * @brief Rozebere zápis vlastní hodnoty kódu zařízení (jen číslo, ne jméno zařízení).
 * @param text Zápis z modálu.
 * @param[out] value Hodnota BC při úspěchu (smí být NULL).
 * @return BOOTDEV_PARSE_OK = číslo 0..65535 podle bootdev_parse_ex();
 *         BOOTDEV_PARSE_LEADING_ZERO = desítkové číslo s úvodní nulou;
 *         jinak (i jméno zařízení) BOOTDEV_PARSE_INVALID.
 */
static en_BOOTDEV_PARSE menu_cmt_bootdev_custom_parse(const char *text, uint16_t *value)
{
    st_BOOTDEV_CHOICE c;
    en_BOOTDEV_PARSE r = bootdev_parse_ex(text, &c);
    if (r != BOOTDEV_PARSE_OK) return r;
    if (!c.custom) return BOOTDEV_PARSE_INVALID;
    if (value) *value = c.value;
    return BOOTDEV_PARSE_OK;
}

/**
 * @brief Podnabídka Device for Direct MZF Start: kód zařízení v BC při přímém spuštění MZF.
 *
 * Přepínače CMT, FD, QD a RD s hodnotou BC platformy tohoto buildu
 * (bootdev_name_bc()), označený je zvolený (nastavení se jménem; `sram` je
 * RD), a Custom value... (označená při číselné volbě, ukazuje hodnotu),
 * která požádá o modál (imgui_menu_cmt_bootdev_popup()). Nastavení mění
 * přímo bootdev_setting_set_text() (vnitřní zámek modulu, platí od
 * příštího přímého spuštění; stav stroje se nemění).
 *
 * @pre UI vlákno, uvnitř nabídky CMT Options.
 */
static void menu_cmt_bootdev_submenu(void)
{
    bool open = ImGui::BeginMenu(_L("Device for Direct MZF Start"));
    menu_cmt_test_record("bootdev_menu");
    if (open)
    {
        char cur[BOOTDEV_TEXT_MAX];
        bootdev_setting_get_text(cur, sizeof(cur));
        st_BOOTDEV_CHOICE choice;
        if (!bootdev_parse(cur, &choice))
        {
            choice.custom = false;
            choice.name = BOOTDEV_NAME_CMT;
        };

        static const struct
        {
            en_BOOTDEV_NAME name; /**< řádek tabulky */
            const char *label;    /**< text položky (N_ pro katalog) */
            const char *id;       /**< stabilní ImGui ID (jméno prvku pro testy bez ###) */
        } items[] = {
            {BOOTDEV_NAME_CMT, N_("CMT - cassette"), "###bootdev_cmt"},
            {BOOTDEV_NAME_FD, N_("FD - floppy disk"), "###bootdev_fd"},
            {BOOTDEV_NAME_QD, N_("QD - Quick Disk"), "###bootdev_qd"},
            {BOOTDEV_NAME_RD, N_("RD - SRAM"), "###bootdev_rd"},
        };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        {
            char label[128];
            snprintf(label, sizeof(label), "%s (%04Xh)%s", _(items[i].label),
                     (unsigned)bootdev_name_bc(items[i].name, (unsigned)MZARCH),
                     items[i].id);
            bool clicked = MenuRadioItem(label, !choice.custom && choice.name == items[i].name);
            menu_cmt_test_record(items[i].id + 3);
            if (clicked)
            {
                (void)bootdev_setting_set_text(bootdev_name_text(items[i].name));
            };
        };

        ImGui::Separator();
        char custom[128];
        if (choice.custom)
            snprintf(custom, sizeof(custom), "%s (%04Xh)...###bootdev_custom", _("Custom value"), (unsigned)choice.value);
        else
            snprintf(custom, sizeof(custom), "%s...###bootdev_custom", _("Custom value"));
        bool custom_clicked = MenuRadioItem(custom, choice.custom);
        menu_cmt_test_record("bootdev_custom");
        if (custom_clicked)
        {
            /* zápis předvyplnit aktuální hodnotou (číslo jak bylo zadané, jinak hex) */
            if (choice.custom)
                g_strlcpy(s_bootdev_custom.buf, cur, sizeof(s_bootdev_custom.buf));
            else
                snprintf(s_bootdev_custom.buf, sizeof(s_bootdev_custom.buf), "0x%04X",
                         (unsigned)bootdev_choice_bc_here(&choice));
            s_bootdev_custom.open_request = true;
            s_bootdev_custom.focus = true;
        };
        ImGui::EndMenu();
    };
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", _("The device code a program gets in register BC when an MZF file\n"
                                  "is started directly (command line --run-mzf, MCP emu_media_run_mzf),\n"
                                  "as if it was loaded from that device.\n"
                                  "Some programs choose their default device by it."));
    };
}

/**
 * @brief Modál Custom value...: vlastní hodnota BC desítkově nebo šestnáctkově.
 *
 * Ukazuje aktuální hodnotu BC šestnáctkově, vstup (desítkově `512` bez
 * úvodní nuly, šestnáctkově `0x0200` nebo `0200h`, 0 až 65535) s průběžnou
 * kontrolou; desítkové číslo s úvodní nulou (`0200`) odmítne s radou, jak
 * psát šestnáctkově. OK je při neplatném zápisu zakázané, Enter zápis
 * odmítne a vrátí fokus do vstupu, Cancel (i Escape) zavře bez změny.
 * Platný zápis uloží bootdev_setting_set_text(). Jméno zařízení modál
 * nepřijme (jen číslo).
 *
 * Modál se kreslí každý snímek mimo nabídky (položka Custom value... je
 * uvnitř nabídek HW & Devices > CMT > CMT Options, které klik zavře; modál
 * otevřený a kreslený odtud by se neukázal). Otevře se podle
 * s_bootdev_custom.open_request, až žádný jiný popup otevřený není
 * (imgui_modal_open_when_free(), pravidlo imgui_modal.h).
 *
 * @pre UI vlákno uvnitř ImGui snímku, mimo nabídky a okna (main_window.cpp,
 *      jako ostatní modály zařízení).
 */
void imgui_menu_cmt_bootdev_popup(void)
{
    if (s_bootdev_custom.open_request && imgui_modal_open_when_free(MENU_CMT_BOOTDEV_POPUP_ID))
    {
        s_bootdev_custom.open_request = false;
    };
    char title[128];
    snprintf(title, sizeof(title), "%s%s", _("Device Code for Direct MZF Start"), MENU_CMT_BOOTDEV_POPUP_ID);
    if (!ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    char cur[BOOTDEV_TEXT_MAX];
    bootdev_setting_get_text(cur, sizeof(cur));
    ImGui::Text(_("Current value: %04Xh (%s)"), (unsigned)bootdev_setting_bc(), cur);
    ImGui::TextUnformatted(_("Decimal (512) or hexadecimal (0x0200 or 0200h), 0 to 65535."));

    if (s_bootdev_custom.focus)
    {
        ImGui::SetKeyboardFocusHere();
        s_bootdev_custom.focus = false;
    };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    bool enter = ImGui::InputText("##bootdev_value", s_bootdev_custom.buf, sizeof(s_bootdev_custom.buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    uint16_t value = 0;
    en_BOOTDEV_PARSE pr = menu_cmt_bootdev_custom_parse(s_bootdev_custom.buf, &value);
    bool valid = (pr == BOOTDEV_PARSE_OK);
    ImGui::SameLine();
    if (valid)
        ImGui::Text("= %04Xh", (unsigned)value);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _("Invalid value"));
    if (pr == BOOTDEV_PARSE_LEADING_ZERO)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           _("A number with a leading zero is not accepted: write hexadecimal as 0x0200 or 0200h."));

    ImGui::BeginDisabled(!valid);
    bool ok = ImGui::Button(_L("OK"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool cancel = ImGui::Button(_L("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if ((ok || enter) && valid)
    {
        (void)bootdev_setting_set_text(s_bootdev_custom.buf);
        ImGui::CloseCurrentPopup();
    }
    else if (cancel)
    {
        ImGui::CloseCurrentPopup();
    }
    else if (enter)
    {
        /* Enter s neplatným zápisem: vstup ztratil fokus, vrátit ho (oprava zápisu) */
        s_bootdev_custom.focus = true;
    };
    ImGui::EndPopup();
}

/**
 * @brief Nakreslí nabídku CMT hlavního menu a zařadí zásahy vybraných položek.
 *
 * Use CMT Hack přepne CMT hack zásahem režimu načítání
 * (cmt_request_set_loading_hack(), jen ve STOP, výsledek ohlásí okno
 * Virtual CMT); s ROM bez podpory hacku (cmthack_rom_supported()) a vypnutým
 * hackem je položka zakázaná s tooltipem cmt_view_direct_unsupported_reason()
 * (týž důvod jako segment Direct okna); zapnutý hack s takovou ROM nastat
 * nemá (rom_reinstall() ho vypne). Ostatní položky: výchozí rychlost (jen ve
 * STOP), Fast Open CMT File & Play..., Recent (naposledy vložené pásky:
 * klik otevře pásku obvyklým postupem s dotazy na nahrávání a neuložené
 * změny, imgui_cmt_tape_open_path(); běží-li jiný dotaz automatu, páska se
 * nezmění a ohlásí se to zprávou), Show Virtual CMT, CMT Options (CPU
 * Boost, Echo, kontrola velikosti MZF a její automatická oprava, Fix FNAME
 * Terminator, Direct Save, Device for Direct MZF Start). Echo
 * (cmt_echo_set()) a Fix MZF Size Automatically (cmt_mzfsize_autofix_set(),
 * jen se zapnutou kontrolou) jen přepnou příznak jádra. Direct Save přepíná zásah
 * cmt_request_set_direct_save() (jen ve STOP, výsledek ohlásí okno
 * Virtual CMT); bez CMT hacku nebo s ROM bez standardních rutin kazety
 * (cmthack_save_rom_supported()) je položka zakázaná s tooltipem důvodu.
 * Modál Custom value... kreslí imgui_menu_cmt_bootdev_popup() z hlavního
 * okna mimo nabídky.
 *
 * @pre UI vlákno uvnitř lišty nebo popupu menu ImGui, pod device_state_lock().
 * @post Stav jádra se přímo mění jen u Enable MZF Size Check
 *       (cmt_mzfsize_check_set()), Fix MZF Size Automatically, Echo a u kódu zařízení pro přímé spuštění MZF
 *       (bootdev_setting_set_text(), vlastní zámek modulu); Show Virtual CMT
 *       přepne příznak okna, ostatní změny provede emu vlákno zásahy.
 */
void imgui_menu_cmt(void)
{
    if (ImGui::BeginMenu(_L("CMT")))
    {
        /*
         *
         * Submenu CMT
         *
         */

        /* ROM bez podpory hacku: zapnutí by zásah neprovedl (patch se
         * nenainstaluje), položka je zakázaná s důvodem jako segment Direct
         * okna kazety. Dokud je hack zapnutý, položka zůstane povolená
         * (k vypnutí); s ROM bez podpory zapnutý být nemá - výměna ROM
         * i změna volby uživatelské ROM ho vypne (rom_reinstall()). Menu se
         * kreslí pod device_state_lock() (imgui_topmenu_body()), ROM mění
         * jen emu vlákno pod ním. */
        const bool hack_unsupported = !CMTHACK_TEST_IS_INSTALLED && !cmthack_rom_supported();
        if (ImGui::MenuItem(_L("Use CMT Hack"), NULL, CMTHACK_TEST_IS_INSTALLED, !hack_unsupported))
        {
            /* zásah režimu načítání: hack jen ve STOP, výsledek ohlásí okno kazety */
            cmt_request_set_loading_hack(!CMTHACK_TEST_IS_INSTALLED);
        };
        if (hack_unsupported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", cmt_view_direct_unsupported_reason());
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("Set Default CMT MZ Speed"), CMT_TEST_STOP))
        {
            if (MenuRadioItem(_("1:1 - 1200 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_1_1)))
            {
                cmt_request_set_speed(CMTSPEED_1_1);
            };

            if (MenuRadioItem(_("2:1 - 2 400 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_2_1)))
            {
                cmt_request_set_speed(CMTSPEED_2_1);
            };

            if (MenuRadioItem(_("2:1 - 2 400 Bd (cp/m)"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_2_1_CPM)))
            {
                cmt_request_set_speed(CMTSPEED_2_1_CPM);
            };

            if (MenuRadioItem(_("7:3 - 2 800 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_7_3)))
            {
                cmt_request_set_speed(CMTSPEED_7_3);
            };

            if (MenuRadioItem(_("8:3 - 3 200 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_8_3)))
            {
                cmt_request_set_speed(CMTSPEED_8_3);
            };

            if (MenuRadioItem(_("3:1 - 3 600 Bd"), CMT_TEST_MZ_CMTSPEED(CMTSPEED_3_1)))
            {
                cmt_request_set_speed(CMTSPEED_3_1);
            };

            ImGui::EndMenu();
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Fast Open CMT File & Play..."), NULL, false))
        {
            /* páska se změnami se nejdřív zeptá na uložení */
            imgui_cmt_tape_action(IMGUI_CMT_TAPE_ACTION_OPEN_PLAY);
        };

        /* Naposledy vložené pásky: jako návrat dialogu Open... okna Virtual CMT (stejná cesta jako overlay menu). */
        char recent_path[4096];
        if (imgui_menu_recent(RECENT_MEDIA_CMT, true, NULL, NULL, recent_path, sizeof(recent_path)))
        {
            if (!imgui_cmt_tape_open_path(recent_path))
            {
                gchar *text = g_strdup_printf(_("Another tape dialog is still open. The tape was not changed:\n%s"), recent_path);
                baseui_show_message(false, (char *)"%s", text);
                g_free(text);
            };
        };

        if (ImGui::MenuItem(_L("Show Virtual CMT"), "Alt+C", g_gui->showVirtualCmtWindow))
        {
            g_gui->showVirtualCmtWindow = !g_gui->showVirtualCmtWindow;
        };

        ImGui::Separator();

        if (ImGui::BeginMenu(_L("CMT Options")))
        {
            if (ImGui::MenuItem(_L("Enable CPU Boost"), NULL, CMT_TEST_CPU_BOOST_ENABLED))
            {
                /* zásah režimu načítání: MAX SPEED mění jen emu vlákno; výsledek
                 * se ohlásí podle toho, co se stalo (s hackem režim zůstává Direct) */
                imgui_cmt_menu_set_cpu_boost(!CMT_TEST_CPU_BOOST_ENABLED);
            };

            /* echo: signál pásky do reproduktoru (jen příznak, stav stroje se nemění) */
            if (ImGui::MenuItem(_L("Echo"), NULL, g_cmt.echo != 0))
            {
                cmt_echo_set(g_cmt.echo == 0);
            };
            menu_cmt_test_record("echo");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s", _("Plays the tape signal through the speaker while the tape\n"
                                          "is playing or recording. Direct loading and saving have no signal."));
            };

            ImGui::SeparatorText(_("MZF"));

            if (ImGui::MenuItem(_L("Enable MZF Size Check"), NULL, CMT_TEST_MZF_SIZE_CHECK_ENABLED))
            {
                cmt_mzfsize_check_set((CMT_TEST_MZF_SIZE_CHECK_ENABLED) ? CMT_MZFSIZE_CHECK_DISABLED : CMT_MZFSIZE_CHECK_ENABLED);
            };

            /* oprava bez dotazu: jen se zapnutou kontrolou */
            if (ImGui::MenuItem(_L("Fix MZF Size Automatically"), NULL, g_cmt.mzfsize_autofix != 0,
                                CMT_TEST_MZF_SIZE_CHECK_ENABLED))
            {
                cmt_mzfsize_autofix_set(g_cmt.mzfsize_autofix == 0);
            };
            menu_cmt_test_record("mzfsize_autofix");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s", _("An MZF file longer than its header says is truncated\n"
                                          "to the right size without asking."));
            };

            if (ImGui::MenuItem(_L("Fix FNAME Terminator"), NULL, CMTHACK_TEST_FIX_FNAME_TERMINATOR))
            {
                cmt_request_set_fix_fname_terminator(!CMTHACK_TEST_FIX_FNAME_TERMINATOR);
            };

            /* Direct Save: s hackem a ROM se standardními rutinami kazety;
             * zásah jen ve STOP, výsledek i odmítnutí ohlásí okno kazety */
            const bool save_rom_ok = cmthack_save_rom_supported();
            const bool save_allowed = CMTHACK_TEST_IS_INSTALLED && save_rom_ok;
            if (ImGui::MenuItem(_L("Direct Save"), NULL, g_cmthack.direct_save != 0, save_allowed))
            {
                cmt_request_set_direct_save(g_cmthack.direct_save == 0);
            };
            menu_cmt_test_record("direct_save");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (!CMTHACK_TEST_IS_INSTALLED)
                    ImGui::SetTooltip("%s", _("Direct Save works only with Direct loading: turn on Use CMT Hack first."));
                else if (!save_rom_ok)
                    ImGui::SetTooltip("%s", _("The selected ROM does not support direct saving."));
                else
                    ImGui::SetTooltip("%s", _("Without Record, a ROM SAVE stores the file at once as a new block at the end of the tape.\n"
                                              "With Record pressed, the signal is recorded as usual."));
            };

            /* kód zařízení v BC při přímém spuštění MZF (--run-mzf, MCP) */
            menu_cmt_bootdev_submenu();

            ImGui::EndMenu();
        };

        ImGui::EndMenu();
    };
}
