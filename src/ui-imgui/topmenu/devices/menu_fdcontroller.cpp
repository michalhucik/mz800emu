#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/imgui_modal.h"

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

#if CFG_HWEXT_HAVE_FDC
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/fdc/fdc_request.h"
#endif

extern "C"
{
    void imgui_menu_fdcontroller(void);
    void imgui_fdc_storage_switch_popup(bool *p_open);
}

/* --- State pro modal "Switch storage mode with pending RAM changes" --- */
typedef struct fdc_storage_switch_pending_t
{
    int fdc_index;        /* FDC0 / FDC1 - kterého řadiče se popup týká */
    int drive_id;
    char target_mode[16]; /* "cached" | "direct" */
} fdc_storage_switch_pending_t;

static fdc_storage_switch_pending_t g_fdc_storage_switch_pending = { FDC0, -1, "" };

/**
 * @brief Aktivuj modal popup pro confirm switch storage mode s dirty RAM.
 *
 * Voláno z menu radio handleru když user přepíná z Discard mode s pending
 * RAM changes na Cached/Direct. Popup nabídne: Save & Switch / Discard &
 * Switch / Cancel.
 */
static void imgui_fdc_storage_switch_activate(int fdc_index, int drive_id, const char *target_mode)
{
    g_fdc_storage_switch_pending.fdc_index = fdc_index;
    g_fdc_storage_switch_pending.drive_id = drive_id;
    g_strlcpy(g_fdc_storage_switch_pending.target_mode, target_mode,
              sizeof(g_fdc_storage_switch_pending.target_mode));
    g_gui->showFdcStorageSwitchWindow = true;
}

/**
 * @brief Popup "FDC: Unsaved RAM changes" před přepnutím z režimu Discard.
 *
 * Tlačítka přepnutí jen zařadí zásah (fdc_request_set_storage_mode(),
 * u "Save and switch" s předchozím zápisem obrazu do souboru), provede
 * ho emu vlákno.
 *
 * @param p_open Příznak otevření popupu (nastaví se na false po volbě).
 * @pre UI vlákno; volající drží zámek stavu zařízení (main_window).
 */
void imgui_fdc_storage_switch_popup(bool *p_open)
{
    int fi = g_fdc_storage_switch_pending.fdc_index;
    int drv = g_fdc_storage_switch_pending.drive_id;
    if (drv < 0 || drv >= 4 || fi < 0 || fi >= FDC_INSTANCE_COUNT)
    {
        *p_open = false;
        return;
    }

    /* jen při volnu: jinak by OpenPopup() zavřel jiný otevřený modál (imgui_modal.h) */
    imgui_modal_open_when_free(_L("FDC: Unsaved RAM changes"));

    if (ImGui::BeginPopupModal(_L("FDC: Unsaved RAM changes"), NULL,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(_("Drive %d is in Discard mode and has unsaved RAM changes."), drv);
        ImGui::TextWrapped(_("Switching to '%s' mode will close the current RAM buffer."),
                           g_fdc_storage_switch_pending.target_mode);
        ImGui::Separator();
        ImGui::TextWrapped(_("What to do with the changes?"));
        ImGui::Spacing();

        /* Spočítej širku tlačítek podle text obsahu před ## (= _L macro
         * suffix který ImGui interně ořezává pro render, ale CalcTextSize
         * ho měří celý). Všechna tři tlačítka dostanou max šířku pro symetrii. */
        const char *btn_save    = _L("Save and switch");
        const char *btn_discard = _L("Switch and discard");
        const char *btn_cancel  = _L("Cancel");

        const ImGuiStyle &style = ImGui::GetStyle();
        float btn_pad = style.FramePadding.x * 2.0f + 8.0f; /* extra margin */
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
            fdc_request_set_storage_mode((unsigned)fi, (unsigned)drv,
                                         g_fdc_storage_switch_pending.target_mode, true);
            *p_open = false;
            g_fdc_storage_switch_pending.drive_id = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button(btn_discard, ImVec2(button_width, 0)))
        {
            fdc_request_set_storage_mode((unsigned)fi, (unsigned)drv,
                                         g_fdc_storage_switch_pending.target_mode, false);
            *p_open = false;
            g_fdc_storage_switch_pending.drive_id = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button(btn_cancel, ImVec2(button_width, 0)))
        {
            *p_open = false;
            g_fdc_storage_switch_pending.drive_id = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

/**
 * @brief Obsah podmenu jedné mechaniky: jméno obrazu, Mount / Recent / Umount, R/O a režim ukládání.
 *
 * Podnabídka "Recent" (imgui_menu_recent()) nabízí naposledy vložené obrazy
 * DSK; klik vloží obraz do této mechaniky stejným zásahem jako výběr
 * v souborovém dialogu (fdc_request_mount()), takže platí i pravidlo
 * sdíleného obrazu. Cesta delší než jméno obrazu mechaniky se nevkládá a
 * ohlásí se zprávou, jako v overlay menu.
 *
 * U položek Mount a Umount ukazuje klávesovou zkratku, kterou obsluhuje
 * global_shortcuts.cpp: Alt+1 až Alt+4 (vložení) a Alt+Shift+1 až Alt+Shift+4
 * (vysunutí) pro mechaniky 0 až 3 řadiče FDC0. Mechaniky řadiče FDC1 zkratku
 * nemají, popisek se u nich neukazuje.
 *
 * U obrazu, který má ochranu proti zápisu kvůli sdílení souboru s jinou
 * mechanikou (fdc.h), ukazuje pod jménem poznámku "Shared with FDD n,
 * write-protected" a položka "Read-only" je zaškrtnutá; dokud druhá
 * mechanika do souboru smí zapisovat, je položka zakázaná s důvodem
 * v tooltipu.
 *
 * Do stavu řadiče nezapisuje přímo; změny jdou přes fdc_ui_mount() a
 * fdc_request_*() do emulačního vlákna.
 *
 * @param fdc instance řadiče, které mechanika patří (nesmí být NULL).
 * @param drive_id číslo mechaniky 0 až 3.
 * @pre Volá se z UI vlákna uvnitř otevřeného menu ImGui (mezi BeginMenu a EndMenu).
 */
void SubmenuFDDriveImage(st_FDC *fdc, int drive_id)
{
#if CFG_HWEXT_HAVE_FDC

    GString *image_basename = g_string_new(NULL);

    if (FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id))
    {
        const char *basename = g_path_get_basename(fdc_get_dsk_filepath(fdc, drive_id));
        // pokud ma vice jak 20 znaku, tak zkratime a na konec pridame "..." - aby se veslo do menu
        if (strlen(basename) > 20)
        {
            g_string_printf(image_basename, "%s...", g_utf8_substring(basename, 0, 20));
        }
        else
        {
            g_string_assign(image_basename, basename);
        };
        g_free((gpointer)basename);
    } 
    else
    {
        g_string_assign(image_basename, _("No image"));
    };

    ImGui::MenuItem(image_basename->str, NULL, false, false);

    /* Tentýž soubor obrazu ve více mechanikách (fdc.h): do souboru smí
     * zapisovat jen jedna, tahle má ochranu kvůli sdílení. Mechaniky se
     * číslují přes oba řadiče (0-3 = FDC0, 4-7 = FDC1). */
    const bool shared_ro = fdc_drive_shared_readonly(fdc, drive_id) != 0;
    const int shared_with = fdc_drive_shared_with(fdc, drive_id);
    const int wp_blocker = fdc_drive_write_blocker(fdc, drive_id);
    if (shared_ro && shared_with >= 0)
    {
        char note[128];
        g_snprintf(note, sizeof(note), _("Shared with FDD %d, write-protected"), shared_with);
        ImGui::MenuItem(note, NULL, false, false);
    };

    ImGui::Separator();

    /* Popisky zkratek musí odpovídat obsluze v global_shortcuts.cpp: mechaniky
     * FDD 0 až 3 mají klávesy Alt+1 až Alt+4 (s Shiftem vysunutí) a zkratky
     * pracují jen s řadičem FDC0. U mechanik řadiče FDC1 se proto popisek
     * neukazuje (prázdný řetězec ImGui nekreslí). */
    GString *mount_accel = g_string_new(NULL);
    GString *umount_accel = g_string_new(NULL);
    if (fdc->index == FDC0)
    {
        g_string_append_printf(mount_accel, "Alt+%d", drive_id + 1);
        g_string_append_printf(umount_accel, "Alt+Shift+%d", drive_id + 1);
    };

    const char *mount_text = (!FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id)) ? _("Mount...") : _("Re-Mount...");

    if (ImGui::MenuItem(mount_text, mount_accel->str, false))
    {
        fdc_ui_mount(fdc, drive_id);
    };

    /* Naposledy vložené obrazy: vložení do této mechaniky jako po výběru v dialogu (fdc_ui_mount_cb()). */
    char recent_path[4096];
    if (imgui_menu_recent(RECENT_MEDIA_DSK, true, NULL, NULL, recent_path, sizeof(recent_path)))
    {
        if (strlen(recent_path) >= sizeof(fdc->drive[drive_id].filename))
        {
            gchar *text = g_strdup_printf(_("The file path is too long:\n%s"), recent_path);
            baseui_show_message(false, (char *)"%s", text);
            g_free(text);
        }
        else
        {
            fdc_request_mount(fdc->index, (unsigned)drive_id, recent_path);
        };
    };

    if (ImGui::MenuItem(_L("Umount"), umount_accel->str, false, FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id)))
    {
        fdc_request_umount(fdc->index, (unsigned)drive_id);
    };

    /* --- Per-drive R/O + Storage mode --- */
    bool is_mounted = FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id);
    bool fs_ro = fdc_drive_fs_readonly(fdc, drive_id) != 0;
    int user_ro_cfg = fdc_cfg_get_readonly(fdc, drive_id);

    ImGui::Separator();

    /* R/O checkbox: pokud FS read-only, zobraz disabled s pevně ON + popisek.
     * Persistent user pref se touto cestou nemění. */
    if (fs_ro)
    {
        bool forced_ro = true;
        ImGui::BeginDisabled();
        ImGui::MenuItem(_L("Read-only (FS write-protected)"), NULL, &forced_ro, false);
        ImGui::EndDisabled();
    }
    else
    {
        /* Zaškrtnuto i při ochraně kvůli sdílení; odškrtnutí ruší obojí
         * (fdc_drive_set_write_protect()). Dokud tentýž soubor drží jiná
         * mechanika se zápisem, ochranu sundat nejde: položka je zakázaná
         * a důvod ukáže tooltip. */
        bool ro_pref = (user_ro_cfg != 0) || shared_ro;
        const bool locked = ro_pref && is_mounted && (wp_blocker >= 0);
        if (ImGui::MenuItem(_L("Read-only"), NULL, &ro_pref, !locked))
        {
            /* Zápis volby a remount nasazeného obrazu provede emu vlákno. */
            fdc_request_set_readonly(fdc->index, (unsigned)drive_id, ro_pref ? 1 : 0);
        }
        if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(_("The image is also inserted in FDD %d, which may write to it."), wp_blocker);
    }

    /* Storage mode: combo přes 3 radio položky. */
    const char *mode_str = fdc_cfg_get_storage_mode(fdc, drive_id);
    bool mode_cached  = (strcmp(mode_str, "cached") == 0);
    bool mode_direct  = (strcmp(mode_str, "direct") == 0);
    bool mode_discard = (strcmp(mode_str, "discard") == 0);

    ImGui::SeparatorText(_("Storage mode"));

    /* Pomocná lambda: switch storage mode s detekcí "dirty Discard → save mode".
     * Pokud je to případ, otevře modal popup; jinak okamžitý switch. */
    auto try_switch = [&](const char *target) {
        bool needs_prompt = is_mounted && mode_discard
                            && (strcmp(target, "discard") != 0)
                            && (fdc_drive_has_ram_changes(fdc, drive_id) != 0);
        if (needs_prompt)
        {
            imgui_fdc_storage_switch_activate((int)fdc->index, drive_id, target);
        }
        else
        {
            fdc_request_set_storage_mode(fdc->index, (unsigned)drive_id, target, false);
        }
    };

    if (MenuRadioItem(_("Cached (sync at reset/umount/exit)"), mode_cached))
    {
        if (!mode_cached) try_switch("cached");
    }

    if (MenuRadioItem(_("Direct (write-through to file)"), mode_direct))
    {
        if (!mode_direct) try_switch("direct");
    }

    if (MenuRadioItem(_("Discard changes (in-RAM, no sync)"), mode_discard))
    {
        if (!mode_discard) try_switch("discard");
    }

    /* Sync now - dostupné jen v cached s pending changes. */
    bool sync_enabled = is_mounted && mode_cached
                        && (fdc_drive_has_unsaved_changes(fdc, drive_id) != 0);
    if (ImGui::MenuItem(_L("Sync now"), NULL, false, sync_enabled))
    {
        fdc_request_sync(fdc->index, (unsigned)drive_id);
    }

    g_string_free(image_basename, TRUE);
    g_string_free(mount_accel, TRUE);
    g_string_free(umount_accel, TRUE);
#endif
}

void SubmenuFDDrive(st_FDC *fdc, int drive_id)
{
#if CFG_HWEXT_HAVE_FDC
    GString *drive_name = g_string_new(_("FDD "));
    g_string_append_printf(drive_name, "%d", drive_id);

    bool fdc_connected = (fdc->connected == FDC_CONNECTED);

    if (fdc_connected && (!FDC_TEST_DRIVE_ID_MOUNTED(fdc, drive_id)))
    {
        g_string_append(drive_name," ");
        g_string_append(drive_name, _("(Empty)"));
    };

    if (ImGui::BeginMenu(drive_name->str, fdc_connected))
    {
        SubmenuFDDriveImage(fdc, drive_id);
        ImGui::EndMenu();
    };
    g_string_free(drive_name, TRUE);
#endif
}

#if CFG_HWEXT_HAVE_FDC
/**
 * @brief Submenu pro HW volby FDC chipu.
 *
 * Sekce "Hardware options" v hlavním FDC menu. Obsahuje:
 *  - Checkbox HD Patch (port 0xDF EINT obvod).
 *  - Radio bus_xlate (invert = Sharp default / passthrough = experimental).
 *
 * Obě volby čte řadič za běhu (`hd_patch` přes `hd_patch_ref` ve
 * wd279x.c, `bus_xlate` při každém přístupu na port), změna tedy platí
 * hned. Zápis do `fdc->*` provede emu vlákno (fdc_request_set_hd_patch(),
 * fdc_request_set_bus_xlate()); hodnoty se uloží do INI při exitu.
 *
 * @param fdc instance FDC řadiče, jehož HW volby se konfigurují.
 */
static void SubmenuFDCHardwareOptions(st_FDC *fdc)
{
    ImGui::SeparatorText(_("Hardware options"));

    bool hd_patch = (fdc->hd_patch != 0);
    if (ImGui::MenuItem(_L("HD Patch enabled"), NULL, &hd_patch, true))
    {
        fdc_request_set_hd_patch(fdc->index, hd_patch ? 1 : 0);
    };

    bool xlate_invert = (fdc->bus_xlate == FDC_BUS_XLATE_INVERT);
    if (MenuRadioItem(_("Bus translation: invert (Sharp)"), xlate_invert))
    {
        fdc_request_set_bus_xlate(fdc->index, FDC_BUS_XLATE_INVERT);
    };
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Sharp MZ default - XOR 0xFF between CPU bus and WD279x chip."));

    if (MenuRadioItem(_("Bus translation: passthrough"), !xlate_invert))
    {
        fdc_request_set_bus_xlate(fdc->index, FDC_BUS_XLATE_PASSTHROUGH);
    };
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(_("NOT compatible with Sharp MZ - no inversion between CPU bus "
                                  "and WD279x chip. Reserved for future 'true bus' ROMs / experiments."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
#endif

#if CFG_HWEXT_HAVE_FDC
/**
 * @brief Vykreslí konfiguraci jedné instance FDC.
 *
 * Sekce "Floppy Drives" (4 mechaniky) a "Hardware options" pro daný
 * řadič. Použito pro per-FDC submenu (FDC0 standard / FDC1 secondary).
 *
 * @param fdc instance FDC řadiče.
 */
static void SubmenuFDCInstance(st_FDC *fdc)
{
    ImGui::SeparatorText(_("Floppy Drives"));

    for (int i = 0; i < 4; i++)
    {
        SubmenuFDDrive(fdc, i);
    };

    /* Sekce Hardware options - HD Patch + bus translation polarity. */
    SubmenuFDCHardwareOptions(fdc);
}
#endif

void imgui_menu_fdcontroller(void)
{
    if (ImGui::BeginMenu(_L("FD Controller"), CFG_HWEXT_HAVE_FDC))
    {
#if CFG_HWEXT_HAVE_FDC
        /* Připojení řadičů - nezávislé přepínače. Umožňují všechny
         * kombinace: nic / jen FDC0 / jen FDC1 / oba současně. */
        bool fdc0_connected = (g_fdc[FDC0].connected == FDC_CONNECTED);
        bool fdc1_connected = (g_fdc[FDC1].connected == FDC_CONNECTED);

        /* Not Connected - vypne oba řadiče najednou. Zaškrtnuto, když je
         * oba odpojené (= výchozí stav bez floppy). */
        if (ImGui::MenuItem(_L("Not Connected"), NULL, (!fdc0_connected && !fdc1_connected)))
        {
            fdc_request_set_connected(FDC0, 0);
            fdc_request_set_connected(FDC1, 0);
            fdc0_connected = false;
            fdc1_connected = false;
        };

        ImGui::Separator();

        if (ImGui::MenuItem(_L("WD279x (standard)"), NULL, &fdc0_connected, true))
        {
            fdc_request_set_connected(FDC0, fdc0_connected ? 1 : 0);
        };
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("Default FDC on ports 0xD8 - 0xDF"));

        if (ImGui::MenuItem(_L("WD279x (secondary)"), NULL, &fdc1_connected, true))
        {
            fdc_request_set_connected(FDC1, fdc1_connected ? 1 : 0);
        };
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _("Unicard alternate FDC ports 0x58 - 0x5F"));

        ImGui::Separator();

        /* Per-FDC konfigurace (Floppy Drives + Hardware options). Submenu
         * je aktivní jen pokud je daný řadič připojen. */
        if (ImGui::BeginMenu(_L("FDC0 (standard)"), fdc0_connected))
        {
            SubmenuFDCInstance(&g_fdc[FDC0]);
            ImGui::EndMenu();
        };

        if (ImGui::BeginMenu(_L("FDC1 (secondary)"), fdc1_connected))
        {
            SubmenuFDCInstance(&g_fdc[FDC1]);
            ImGui::EndMenu();
        };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        ImGui::SeparatorText(_("Tools"));

        if (ImGui::MenuItem(_L("FDC State (debugger)..."), NULL, g_gui->showFdcStateWindow, true))
        {
            g_gui->showFdcStateWindow = !g_gui->showFdcStateWindow;
        };
#endif
#endif
        ImGui::EndMenu();
    };
}
