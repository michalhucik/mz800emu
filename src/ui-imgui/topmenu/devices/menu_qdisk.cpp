#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/mywidgets/mywidgets.h"
#include "emulator/hw-generic/qdisk/qdisk.h"
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
 * Volano z main_window.cpp pokud g_gui->showQdiskStorageSwitchWindow == true.
 * Tlacitka maji jednotnou sirku podle nejsirsiho label textu (centrovano).
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

    ImGui::OpenPopup(_L("QD: Unsaved RAM changes"));

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
             * DISCARD), pak provedeme switch. Faze 4: konsoliduje primy
             * generic_driver_save_memory na verejne API. */
            if (qdisk_drive_force_save_to_file())
            {
                qdisk_apply_storage_mode_switch(target);
                g_qdisk_storage_switch_pending.target_mode[0] = 0;
                *p_open = false;
                ImGui::CloseCurrentPopup();
            };
        };

        ImGui::SameLine();
        if (ImGui::Button(btn_discard, ImVec2(button_width, 0)))
        {
            qdisk_apply_storage_mode_switch(target);
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
        const char *filter = fch->selected_filter;
        /* The selected New Image filter is authoritative.  Do not infer a
         * .qd subtype from its shared extension and do not silently fall
         * back to another representation if the chooser returns an
        * unexpected filter name. */
        if (filter != NULL && strcmp(filter, _("MZQ image")) == 0)
            qdisk_create_mzq_image((char *)filename);
        else if (filter != NULL && strcmp(filter, _("FlashFloppy physical QD image")) == 0)
            qdisk_create_qd_image((char *)filename, QDISK_CREATE_QD_FLASHFLOPPY);
        else if (filter != NULL && strcmp(filter, _("Sharp legacy QD image")) == 0)
            qdisk_create_qd_image((char *)filename, QDISK_CREATE_QD_SHARP_LEGACY);
        else if (filter != NULL && strcmp(filter, _("HxC physical QD image")) == 0)
            qdisk_create_qd_image((char *)filename, QDISK_CREATE_QD_HXC);
        else
            fprintf(stderr, "%s(%d): can't create QD image '%s': unknown image format selected\n",
                    __FILE__, __LINE__, filename);
    };

    baseui_filechooser_destroy(fch);
}

void imgui_qdisk_create_image(void)
{
    baseui_fchooser_t *fch = NULL;
    GString *filters = g_string_new(NULL);
    /* ImGuiFileDialog preserves whitespace after a comma in the collection
     * title returned by GetCurrentFilter().  Keep separators whitespace-free
     * so selected_filter exactly matches the labels used by the callback. */
    g_string_append_printf(filters, "%s{.mzq},%s{.qd},%s{.qd},%s{.qd}",
                           _("MZQ image"),
                           _("FlashFloppy physical QD image"),
                           _("Sharp legacy QD image"),
                           _("HxC physical QD image"));

    /* Bez pevné přípony: ImGuiFileDialog doplní .mzq nebo .qd podle právě
     * zvoleného konkrétního formátu. */
    fch = baseui_filechooser_save_file(_("Create new QD image"), filters->str, NULL, _("qdimage"), NULL, qdisk_create_image_cb, NULL);
    g_string_free(filters, TRUE);

    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

void imgui_menu_qdisk(void)
{
    if (ImGui::BeginMenu(_L("Quick Disk")))
    {
        if (MenuRadioItem(_("Not Connected"), !QDISK_TEST_CONNECTED))
        {
            qdisk_close();
            QDISK_SET_DISCONNECTED();
        };

        if (MenuRadioItem(_("MZ 1F11"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_IMAGE)))
        {
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_deactivate_unicard_boot_loader();
            QDISK_SET_TYPE_IMAGE();
            qdisk_open();
        };

        if (MenuRadioItem(_("Virtual - Mapped Directory"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_VIRTUAL)))
        {
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_deactivate_unicard_boot_loader();
            QDISK_SET_TYPE_VIRTUAL();
        };

        bool is_dissabled = (!UNICARD_TEST_IS_CONNECTED);

        if (is_dissabled)
        {
            ImGui::BeginDisabled();
        };

        if (MenuRadioItem(_("Unicard Boot Loader"), (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_UNICARD && UNICARD_TEST_IS_CONNECTED)))
        {
            qdisk_close();
            QDISK_SET_CONNECTED();
            qdisk_activate_unicard_boot_loader();
            QDISK_SET_TYPE_UNICARD();
            qdisk_open();
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

            const bool is_mounted = (drive_option != drive_empty);
            const char *mount_label = is_mounted ? _("Re-Mount...") : _("Mount...");

            if (ImGui::MenuItem(mount_label, NULL, false, true))
            {
                qdisk_ui_mount();
            };

            if (ImGui::MenuItem(_L("Unmount"), NULL, false, is_mounted))
            {
                qdisk_umount();
            };

            ImGui::EndMenu();
        };

        g_string_free(str, TRUE);

        /* UNICARD má vynucené R/O (force_user_readonly=1). Externí .qd
         * kontejner je zapisovatelný přes cached převod při synchronizaci. */
        bool is_unicard = (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_UNICARD);
        bool is_qd_image = (QDISK_TEST_CONNECTED && QDISK_TEST_TYPE_IMAGE
                            && g_qdisk.external_qd);
        bool wrprot_enabled = (QDISK_TEST_CONNECTED && !is_unicard);
        bool wrprot_shown = is_unicard
                          ? true : (qdisc_get_write_protected() != 0);

        if (ImGui::MenuItem(_L("Write Protected"), NULL, wrprot_shown, wrprot_enabled))
        {
            qdisk_set_write_protected(qdisc_get_write_protected() ? 0 : 1);
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
            (void)qdisk_sync_drive();
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
                    qdisk_apply_storage_mode_switch(target);
            };

            if (MenuRadioItem(_("Cached (sync at reset/umount/exit)"), mode_cached))
            {
                if (!mode_cached) try_switch("cached");
            };

            if (is_qd_image) ImGui::BeginDisabled();
            if (MenuRadioItem(_("Direct (write-through to file)"), mode_direct))
            {
                if (!mode_direct) try_switch("direct");
            };
            if (is_qd_image && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(_("Direct mode is unavailable for .qd images because they must be converted back to their container format when saved."));
            };
            if (is_qd_image) ImGui::EndDisabled();

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
