/**
 * @file   menu_qdisk.cpp
 * @brief  Nabídka Quick Disk klasického menu (HW & Devices > Quick Disk): připojení, mechanika, ochrana proti zápisu, režim ukládání.
 *
 * Stav jádra nabídka čte pod device_state_lock(), který drží
 * imgui_topmenu_body() po dobu kreslení menu zařízení; změny jdou zásahy
 * fronty zařízení (qdisk_request_*()), které provede emu vlákno.
 *
 * Podnabídka QD Drive > Recent nabízí naposledy vložené obrazy nebo
 * adresáře se stejnými omezeními jako overlay menu: jen do prázdné
 * mechaniky, jen položky odpovídající typu mechaniky (soubor obrazu pro MZ
 * 1F11, adresář pro virtuální Quick Disk) a soubor, který už neexistuje,
 * se nevkládá (vložení obrazu by ho jinak založilo jako nový). Zda cesta
 * existuje a je-li to adresář, nezjišťuje UI vlákno: bere se z mezipaměti
 * nedávných médií overlay menu (overlay_view_fill_recent()), kterou plní
 * pracovní vlákno.
 */

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/imgui_modal.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/topmenu/menu_recent.h"
#include "ui-imgui/overlay/overlay_view.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"
extern "C"
{
#include "baseui/baseui.h"
}
#include "emulator/hw-generic/qdisk/qdisk.h"
#include "emulator/hw-generic/qdisk/qdisk_request.h"
#include "emulator/hw-generic/unicard/unicard.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_menu_qdisk(void);
    void imgui_qdisk_storage_switch_popup(bool *p_open);
}

/* --- Faze 3 (qdisk-rewrite) Commit 5: pending state pro switch storage mode
 * popup ---
 *
 * QD ma jen 1 drive, takze drzime pouze target_mode (zadny drive_id). Popup
 * se aktivuje, kdyz user prepina z DISCARD na CACHED/DIRECT a v memory
 * bufferu jsou neulozene zmeny - jinak by switch tise zahodil RAM. Target =
 * "" znamena, ze popup neni aktivni / byl odlozen.
 */
typedef struct qdisk_storage_switch_pending_t
{
    char target_mode[16]; /* "cached" | "direct" | "discard" */
} qdisk_storage_switch_pending_t;

static qdisk_storage_switch_pending_t g_qdisk_storage_switch_pending = { "" };

/**
 * @brief Aktivuje modal popup pro confirm switch storage mode s dirty RAM.
 *
 * Zapise target_mode do pending state a nastavi g_gui flag. Vlastni render
 * popupu probehne v dalsi frame z main_window.cpp (imgui_qdisk_storage_switch_popup).
 *
 * @param target_mode Cilovy mod ("cached"|"direct"|"discard"). Kopiruje se.
 */
static void imgui_qdisk_storage_switch_activate(const char *target_mode)
{
    g_strlcpy(g_qdisk_storage_switch_pending.target_mode, target_mode,
              sizeof(g_qdisk_storage_switch_pending.target_mode));
    g_gui->showQdiskStorageSwitchWindow = true;
}

/**
 * @brief Render popup okna "QD: Unsaved RAM changes".
 *
 * Modal popup se 3 tlacitky:
 *  - Save and switch  - flushne aktualni memory buffer do souboru, pak switch
 *  - Switch and discard - jen switch (RAM zmeny se ztrati)
 *  - Cancel - mod zustane DISCARD, zadna akce
 *
 * Volano z main_window.cpp pokud g_gui->showQdiskStorageSwitchWindow == true
 * (pod zámkem stavu zařízení). Tlacitka maji jednotnou sirku podle
 * nejsirsiho label textu (centrovano). Prepnuti jen zaradi zasah
 * (qdisk_request_set_storage_mode()), provede ho emu vlakno.
 *
 * @param p_open ImGui pattern - pointer na bool, ktery se nastavi false po
 *               zavreni popupu.
 */
void imgui_qdisk_storage_switch_popup(bool *p_open)
{
    const char *target = g_qdisk_storage_switch_pending.target_mode;
    if (target[0] == 0)
    {
        *p_open = false;
        return;
    };

    /* jen při volnu: jinak by OpenPopup() zavřel jiný otevřený modál (imgui_modal.h) */
    imgui_modal_open_when_free(_L("QD: Unsaved RAM changes"));

    if (ImGui::BeginPopupModal(_L("QD: Unsaved RAM changes"), NULL,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(_("Quick Disk is in Discard mode and has unsaved RAM changes."));
        ImGui::TextWrapped(_("Switching to '%s' mode will close the current RAM buffer."), target);
        ImGui::Separator();
        ImGui::TextWrapped(_("What to do with the changes?"));
        ImGui::Spacing();

        /* Vypocet sirky tlacitek - vsechna max sirka pro symetrii.
         * CalcTextSize meri vc. ##suffix; orizneme na useful prefix. */
        const char *btn_save    = _L("Save and switch");
        const char *btn_discard = _L("Switch and discard");
        const char *btn_cancel  = _L("Cancel");

        const ImGuiStyle &style = ImGui::GetStyle();
        float btn_pad = style.FramePadding.x * 2.0f + 8.0f;
        auto label_width = [&](const char *s) -> float {
            const char *id = strstr(s, "##");
            const char *end = id ? id : s + strlen(s);
            return ImGui::CalcTextSize(s, end).x + btn_pad;
        };
        float w_save = label_width(btn_save);
        float w_disc = label_width(btn_discard);
        float w_canc = label_width(btn_cancel);
        float button_width = w_save;
        if (w_disc > button_width) button_width = w_disc;
        if (w_canc > button_width) button_width = w_canc;

        float spacing = style.ItemSpacing.x;
        float total_width = button_width * 3 + spacing * 2;
        float cursor_x = (ImGui::GetWindowSize().x - total_width) * 0.5f;
        if (cursor_x < style.WindowPadding.x) cursor_x = style.WindowPadding.x;
        ImGui::SetCursorPosX(cursor_x);

        if (ImGui::Button(btn_save, ImVec2(button_width, 0)))
        {
            /* Save and switch: nejdriv ulozime memory buffer do souboru
             * pres force_save (bypass storage_mode gate - jeste jsme v
             * DISCARD), pak provedeme switch. Obojí v emu vlákně. */
            qdisk_request_set_storage_mode(target, true);
            g_qdisk_storage_switch_pending.target_mode[0] = 0;
            *p_open = false;
            ImGui::CloseCurrentPopup();
        };

        ImGui::SameLine();
        if (ImGui::Button(btn_discard, ImVec2(button_width, 0)))
        {
            qdisk_request_set_storage_mode(target, false);
            g_qdisk_storage_switch_pending.target_mode[0] = 0;
            *p_open = false;
            ImGui::CloseCurrentPopup();
        };

        ImGui::SameLine();
        if (ImGui::Button(btn_cancel, ImVec2(button_width, 0)))
        {
            g_qdisk_storage_switch_pending.target_mode[0] = 0;
            *p_open = false;
            ImGui::CloseCurrentPopup();
        };

        ImGui::EndPopup();
    };
}

/**
 * @brief Callback dialogu "Create New Image": vytvoření zařadí do emu vlákna.
 * @param fch Dialog (zničí se zde).
 */
void qdisk_create_image_cb(baseui_fchooser_t *fch)
{
    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    const char *filename = fch->selected_filePathName;

    if (filename)
    {
        qdisk_request_create_image(filename);
    };

    baseui_filechooser_destroy(fch);
}

void imgui_qdisk_create_image(void)
{
    baseui_fchooser_t *fch = NULL;

    fch = baseui_filechooser_save_file(BASEUI_FCHOOSER_CAT_QDISK, _("Create new QD image"), ".mzq", NULL, _("qdimage.mzq"), NULL, qdisk_create_image_cb, NULL);

    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

/**
 * @brief Kopie nedávných médií se stavem existence souborů pro podnabídku Recent.
 *
 * Používají se jen pole recent, recent_count a recent_now; plní je
 * overlay_view_fill_recent() nejvýš jednou za snímek při otevřené
 * podnabídce (menu_qdisk_recent_state()). Patří jen UI vláknu.
 */
static st_OVERLAY_VIEW s_qd_recent_view;
/** @brief Číslo snímku ImGui posledního plnění s_qd_recent_view (-1 = ještě nikdy). */
static int s_qd_recent_frame = -1;

/**
 * @brief Položka Quick Disku v kopii nedávných médií podle cesty.
 *
 * Při prvním volání ve snímku kopii obnoví z mezipaměti a nechá pracovní
 * vlákno zjišťovat existenci souborů (podnabídka je vidět); na disk nečeká.
 *
 * @param path Cesta z historie (UTF-8).
 * @return Položka, nebo NULL (cesta se do kopie nevešla nebo z historie mezitím zmizela).
 * @pre UI vlákno, uvnitř snímku ImGui.
 * @note Volá se pod device_state_lock() (menu zařízení): bere jen krátké
 *       zámky historie a mezipaměti, žádný z nich se nedrží přes volání do
 *       jádra zařízení.
 */
static const st_OVERLAY_VIEW_RECENT *menu_qdisk_recent_state(const char *path)
{
    const int frame = ImGui::GetFrameCount();
    if (frame != s_qd_recent_frame)
    {
        s_qd_recent_frame = frame;
        overlay_view_fill_recent(&s_qd_recent_view, true);
    };
    for (unsigned i = 0; i < s_qd_recent_view.recent_count; i++)
    {
        const st_OVERLAY_VIEW_RECENT *r = &s_qd_recent_view.recent[i];
        if (r->kind == OVERLAY_MEDIA_QD && strcmp(r->path, path) == 0) return r;
    };
    return NULL;
}

/**
 * @brief Výběr položek podnabídky Recent: vynechá cesty jiného druhu, než jaký mechanika bere.
 *
 * Jako overlay menu: položka se známým stavem, která existuje a je adresář
 * při mechanice s obrazem (nebo soubor při virtuálním Quick Disku), se
 * nenabízí. Položky s neznámým stavem a chybějící položky zůstávají.
 *
 * @param path Cesta položky. @param user Nepoužívá se.
 * @return true = nabídnout.
 */
static bool menu_qdisk_recent_filter(const char *path, void *user)
{
    (void)user;
    const st_OVERLAY_VIEW_RECENT *r = menu_qdisk_recent_state(path);
    if (r && r->checked && !r->missing && r->is_dir != (QDISK_TEST_TYPE_VIRTUAL != 0)) return false;
    return true;
}

/**
 * @brief Ohlásí zprávou text s jednou cestou.
 * @param format Přeložený formát s jedním `%s`. @param path Cesta dosazená za `%s`.
 */
static void menu_qdisk_message(const char *format, const char *path) __attribute__((format(printf, 1, 0)));
static void menu_qdisk_message(const char *format, const char *path)
{
    gchar *text = g_strdup_printf(format, path);
    baseui_show_message(false, (char *)"%s", text);
    g_free(text);
}

/**
 * @brief Vloží Quick Disk zvolený v podnabídce Recent (stejná pravidla jako list výměny média overlay menu).
 *
 * Stav souboru ještě není známý nebo je údaj "chybí" starší než
 * OVERLAY_VIEW_RECENT_FRESH_US: nic se nevkládá, vyžádá se nové zjištění a
 * uživatel dostane zprávu. Soubor neexistuje: položka se z historie
 * odstraní a ohlásí. Jinak zařadí qdisk_request_mount() s druhem cesty
 * podle zjištěného stavu.
 *
 * @param path Zvolená cesta (UTF-8).
 * @pre UI vlákno, mechanika je prázdná (podnabídka je jinak zakázaná).
 */
static void menu_qdisk_recent_insert(const char *path)
{
    const st_OVERLAY_VIEW_RECENT *r = menu_qdisk_recent_state(path);
    if (!r)
    {
        menu_qdisk_message(_("The file path is too long:\n%s"), path);
    }
    else if (!r->checked || (r->missing && g_get_monotonic_time() - (gint64)r->checked_us > OVERLAY_VIEW_RECENT_FRESH_US))
    {
        overlay_view_recent_recheck();
        menu_qdisk_message(_("Still checking whether the file exists. Try again in a moment:\n%s"), path);
    }
    else if (r->missing)
    {
        recent_media_remove_path(RECENT_MEDIA_QD, path);
        overlay_view_recent_recheck();
        menu_qdisk_message(_("The file no longer exists and was removed from the recent media:\n%s"), path);
    }
    else if (r->is_dir != (QDISK_TEST_TYPE_VIRTUAL != 0))
    {
        /* typ mechaniky se změnil mezi kreslením a kliknutím */
        baseui_show_message(false, (char *)"%s", r->is_dir ? _("The Quick Disk is set to a disk image.") : _("The Quick Disk is set to a mapped directory."));
    }
    else
    {
        qdisk_request_mount(path, r->is_dir ? 1 : 0);
    };
}

/**
 * @brief Nakreslí nabídku Quick Disk.
 *
 * Připojení (Not Connected / MZ 1F11 / Virtual - Mapped Directory / Unicard
 * Boot Loader), podnabídka QD Drive (vložené médium, Mount..., Recent,
 * Unmount), Write Protected, režim ukládání a Save now. Podnabídka Recent
 * je dostupná jen s prázdnou mechanikou, stejně jako Mount...
 *
 * @pre UI vlákno uvnitř lišty nebo popupu menu ImGui, pod device_state_lock().
 * @post Stav jádra se nemění přímo; změny provede emu vlákno zásahy.
 */
void imgui_menu_qdisk(void)
{
    if (ImGui::BeginMenu(_L("Quick Disk")))
    {
        if (MenuRadioItem(_("Not Connected"), !QDISK_TEST_CONNECTED))
        {
            qdisk_request_set_connection(QDISK_REQ_DISCONNECTED);
        };

        if (MenuRadioItem(_("MZ 1F11"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_IMAGE)))
        {
            qdisk_request_set_connection(QDISK_REQ_MZ1F11);
        };

        if (MenuRadioItem(_("Virtual - Mapped Directory"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_VIRTUAL)))
        {
            qdisk_request_set_connection(QDISK_REQ_VIRTUAL);
        };

        bool is_dissabled = (!UNICARD_TEST_IS_CONNECTED);

        if (is_dissabled)
        {
            ImGui::BeginDisabled();
        };

        if (MenuRadioItem(_("Unicard Boot Loader"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_UNICARD && UNICARD_TEST_IS_CONNECTED)))
        {
            qdisk_request_set_connection(QDISK_REQ_UNICARD);
        };

        if (is_dissabled)
        {
            ImGui::EndDisabled();
        };

        ImGui::Separator();

        const char *drive_option = NULL;
        const char *drive_empty = _("Empty");

        if (QDISK_TEST_TYPE_IMAGE)
        {
            drive_option = (QDISK_TEST_STD_PATH_IS_SET) ? _("image") : drive_empty;
        }
        else if (QDISK_TEST_TYPE_VIRTUAL)
        {
            drive_option = (QDISK_TEST_VIRT_PATH_IS_SET) ? _("directory") : drive_empty;
        }
        else if (QDISK_TEST_TYPE_UNICARD)
        {
            drive_option = _("Unicard");
        };

        GString *str = g_string_new(_("QD Drive"));
        if (drive_option)
        {
            g_string_append_printf(str, _(" (%s)"), drive_option);
        };

        if (ImGui::BeginMenu(str->str, (QDISK_TEST_CONNECTED && !QDISK_TEST_TYPE_UNICARD)))
        {
            if (drive_option == drive_empty)
            {
                ImGui::MenuItem(drive_empty, NULL, false, false);
            }
            else
            {
                const char *current_prefix = (QDISK_TEST_TYPE_IMAGE) ? _("File") : _("Dir");

                GString *image_basename = g_string_new(NULL);
                const char *basename = g_path_get_basename((QDISK_TEST_TYPE_IMAGE) ? qdisc_get_ui_std_filepath() : qdisc_get_ui_virt_filepath());
                // pokud ma vice jak 20 znaku, tak zkratime a na konec pridame "..." - aby se veslo do menu
                if (strlen(basename) > 20)
                {
                    g_string_printf(image_basename, _("%s: %s..."), current_prefix, g_utf8_substring(basename, 0, 20));
                }
                else
                {
                    g_string_printf(image_basename, _("%s: %s"), current_prefix, basename);
                };
                g_free((gpointer)basename);

                ImGui::MenuItem(image_basename->str, NULL, false, false);
                g_string_free(image_basename, TRUE);
            };

            ImGui::Separator();

            const char *mount_label = (QDISK_TEST_TYPE_IMAGE) ? _("Mount Image...") : _("Mount Directory...");

            if (ImGui::MenuItem(mount_label, NULL, false, (drive_option == drive_empty)))
            {
                qdisk_ui_mount();
            };

            /* Naposledy vložené obrazy nebo adresáře: jen do prázdné mechaniky, jako Mount... a overlay menu. */
            char recent_path[OVERLAY_VIEW_PATH_SIZE];
            if (imgui_menu_recent(RECENT_MEDIA_QD, (drive_option == drive_empty), menu_qdisk_recent_filter, NULL, recent_path, sizeof(recent_path)))
            {
                menu_qdisk_recent_insert(recent_path);
            };

            if (ImGui::MenuItem(_L("Unmount"), NULL, false, (drive_option != drive_empty)))
            {
                qdisk_request_umount();
            };

            ImGui::EndMenu();
        };

        g_string_free(str, TRUE);

        /* Faze 5 (qdisk-rewrite): UNICARD má vynucené R/O (force_user_readonly=1
         * v qdisk_open_image_internal). MenuItem zobrazujeme zaškrtnutý a
         * disabled (gray) - user vidí, že R/O platí, ale nedá se přepnout. */
        bool is_unicard = (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_UNICARD);
        bool wrprot_enabled = (QDISK_TEST_CONNECTED && !QDISK_TEST_TYPE_UNICARD);
        bool wrprot_shown = is_unicard ? true : (qdisc_get_write_protected() != 0);

        if (ImGui::MenuItem(_L("Write Protected"), NULL, wrprot_shown, wrprot_enabled))
        {
            qdisk_request_set_write_protected(qdisc_get_write_protected() ? 0 : 1);
        };
        if (is_unicard && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(_("Forced R/O by UNICARD - cannot be changed."));
        };

        /* Faze 2: vizualni indikator fs_readonly. Pokud je soubor na disku
         * sam o sobe write-protected (chmod a-w nebo Windows R/O attribute),
         * je effective R/O vynucene bez ohledu na user volbu - ukazujeme
         * to oranzovym labelem + tooltipem. */
        if (g_qdisk.fs_readonly && QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_IMAGE)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::MenuItem(_("    [FS R/O]"), NULL, false, false);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(_("File on disk is not writable - effective R/O regardless of user setting."));
            };
        };

        /* --- Faze 4: Save now ---
         *
         * Manualni flush memory bufferu zpet do souboru. Enabled pouze kdyz
         * qdisk_drive_has_unsaved_changes() vrati 1 (handler MEMORY + CACHED
         * + !R/O + memspec.updated + filename neprazdny). Pro DIRECT mod je
         * vzdy disabled (zadny buffer); pro DISCARD taky (storage_mode gate
         * - pokud user chce zapsat z DISCARD, musi pres switch popup).
         */
        bool has_unsaved = qdisk_drive_has_unsaved_changes() != 0;
        if (ImGui::MenuItem(_L("Save now"), NULL, false, has_unsaved))
        {
            qdisk_request_sync();
        };

        /* --- Faze 3: Storage mode submenu ---
         *
         * Persistentni volba CACHED / DIRECT / DISCARD pro IMAGE. VIRTUAL i
         * UNICARD rezim storage mode nepouzivaji:
         *   - VIRTUAL: pripadne nasleduje vlastni I/O cesta (FS_LAYER per .mzf),
         *     storage mode nedava smysl.
         *   - UNICARD (Faze 5): uzamceno na CACHED runtime, nezavisle na INI.
         *     Submenu je disabled (sede); tooltip vysvetli proc.
         */
        bool storage_enabled = QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_IMAGE;

        if (ImGui::BeginMenu(_L("Storage mode"), storage_enabled))
        {
            const char *mode_str = qdisk_get_storage_mode_str();
            bool mode_cached  = (strcmp(mode_str, "cached") == 0);
            bool mode_direct  = (strcmp(mode_str, "direct") == 0);
            bool mode_discard = (strcmp(mode_str, "discard") == 0);

            bool is_mounted = (g_qdisk.status & QDSTS_IMG_READY) != 0;

            /* Faze 3 Commit 5: switch s detekci "DISCARD -> jiny mod s
             * pending RAM changes". V tom pripade aktivuje popup, jinak
             * okamzity switch. */
            auto try_switch = [&](const char *target) {
                bool needs_prompt = is_mounted && mode_discard
                                    && (strcmp(target, "discard") != 0)
                                    && (qdisk_drive_has_ram_changes() != 0);
                if (needs_prompt)
                    imgui_qdisk_storage_switch_activate(target);
                else
                    qdisk_request_set_storage_mode(target, false);
            };

            if (MenuRadioItem(_("Cached (sync at reset/umount/exit)"), mode_cached))
            {
                if (!mode_cached) try_switch("cached");
            };

            if (MenuRadioItem(_("Direct (write-through to file)"), mode_direct))
            {
                if (!mode_direct) try_switch("direct");
            };

            if (MenuRadioItem(_("Discard changes (in-RAM, no sync)"), mode_discard))
            {
                if (!mode_discard) try_switch("discard");
            };

            ImGui::EndMenu();
        };
        if (is_unicard && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(_("UNICARD mode is locked to Cached storage."));
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("Create New Image..."), NULL))
        {
            imgui_qdisk_create_image();
        };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* Faze 6 (qdisk-rewrite): debugger okno se stavem chipu + drive. */
        ImGui::Separator();
        if (ImGui::MenuItem(_L("QDisk State (debugger)..."), NULL,
                            g_gui->showQdiskStateWindow, true))
        {
            g_gui->showQdiskStateWindow = !g_gui->showQdiskStateWindow;
        };
#endif

        ImGui::EndMenu();
    };
}
