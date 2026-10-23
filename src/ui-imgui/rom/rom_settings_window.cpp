/**
 * @file rom_settings_window.cpp
 * @brief Konfigurační dialog pro uživatelem definované ROM soubory
 *
 * Dva panely: hlavní ROM binárky a CMT Hack Patch ROM.
 * Každý panel podporuje all-in-one (16 KB) nebo 3 samostatné soubory.
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

// Lokalizace
#include "i18n.h"

extern "C"
{
#include "emulator/hw-generic/memory/rom.h"
#include "emulator/hw-generic/memory/rom_request.h"
#include "emulator/hw-generic/cmt/cmthack.h"
}

/* Dočasný stav ROM panelu */
struct RomPanelUI {
    bool allinone;
    char allinone_fp[1024];
    char mz700_fp[1024];
    char cgrom_fp[1024];
    char mz800_fp[1024];
};

/* Stav dialogu */
struct RomSettingsState {
    RomPanelUI main;
    int cmthack_idx;        /* 0=Disabled, 1=Default, 2=Custom */
    RomPanelUI cmthack;
};

static RomSettingsState s_state;
static bool s_initialized = false;

/* File chooser flagy */
static bool s_fch_open = false;
static char s_fch_id[64] = "";
static char *s_fch_target = NULL;
static int s_fch_target_size = 0;

/* CMT Hack Patch popisky */
static const char *s_cmthack_items[] = {
    "Disabled CMT Hack Patch",
    "Use Default CMT Hack Patch",
    "Use Custom ROM With CMT Hack Patch"
};

/* Načte stav z globální struktury */
static void load_state(void) {
    /* Hlavní ROM */
    s_state.main.allinone = (g_rom.user_defined_allinone == ROM_BOOL_YES);
    if (g_rom.user_defined_allinone_fp)
        snprintf(s_state.main.allinone_fp, sizeof(s_state.main.allinone_fp), "%s", g_rom.user_defined_allinone_fp);
    else
        s_state.main.allinone_fp[0] = '\0';
    if (g_rom.user_defined_mz700_fp)
        snprintf(s_state.main.mz700_fp, sizeof(s_state.main.mz700_fp), "%s", g_rom.user_defined_mz700_fp);
    else
        s_state.main.mz700_fp[0] = '\0';
    if (g_rom.user_defined_cgrom_fp)
        snprintf(s_state.main.cgrom_fp, sizeof(s_state.main.cgrom_fp), "%s", g_rom.user_defined_cgrom_fp);
    else
        s_state.main.cgrom_fp[0] = '\0';
    if (g_rom.user_defined_mz800_fp)
        snprintf(s_state.main.mz800_fp, sizeof(s_state.main.mz800_fp), "%s", g_rom.user_defined_mz800_fp);
    else
        s_state.main.mz800_fp[0] = '\0';

    /* CMT Hack */
    s_state.cmthack_idx = (int)g_rom.user_defined_cmthack_type;
    s_state.cmthack.allinone = (g_rom.user_defined_cmthack_allinone == ROM_BOOL_YES);
    if (g_rom.user_defined_cmthack_allinone_fp)
        snprintf(s_state.cmthack.allinone_fp, sizeof(s_state.cmthack.allinone_fp), "%s", g_rom.user_defined_cmthack_allinone_fp);
    else
        s_state.cmthack.allinone_fp[0] = '\0';
    if (g_rom.user_defined_cmthack_mz700_fp)
        snprintf(s_state.cmthack.mz700_fp, sizeof(s_state.cmthack.mz700_fp), "%s", g_rom.user_defined_cmthack_mz700_fp);
    else
        s_state.cmthack.mz700_fp[0] = '\0';
    if (g_rom.user_defined_cmthack_cgrom_fp)
        snprintf(s_state.cmthack.cgrom_fp, sizeof(s_state.cmthack.cgrom_fp), "%s", g_rom.user_defined_cmthack_cgrom_fp);
    else
        s_state.cmthack.cgrom_fp[0] = '\0';
    if (g_rom.user_defined_cmthack_mz800_fp)
        snprintf(s_state.cmthack.mz800_fp, sizeof(s_state.cmthack.mz800_fp), "%s", g_rom.user_defined_cmthack_mz800_fp);
    else
        s_state.cmthack.mz800_fp[0] = '\0';
}

/* Otevře file chooser pro výběr ROM souboru */
static void open_file_chooser(const char *id, char *target, int target_size) {
    snprintf(s_fch_id, sizeof(s_fch_id), "%s", id);
    s_fch_target = target;
    s_fch_target_size = target_size;

    IGFD::FileDialogConfig config;
    config.path = (target[0] != '\0') ? target : ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton;
    imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_ROM, config);
    ImGuiFileDialog::Instance()->OpenDialog(s_fch_id, _("Select ROM file"), ".rom,.bin,.*", config);
    s_fch_open = true;
}

/* Šířka sloupce pro labely — musí pojmout i přeložené řetězce */
static float s_label_col_width = 0.0f;

/* Přepočítá šířku sloupce pro labely */
static void RecalcLabelWidth(void) {
    const char *labels[] = {
        _("ROM 0x0000"),
        _("CG-ROM"),
        _("ROM 0xE000")
    };
    float max_w = 0.0f;
    for (int i = 0; i < 3; i++) {
        float w = ImGui::CalcTextSize(labels[i]).x;
        if (w > max_w) max_w = w;
    }
    /* Přidáme padding */
    s_label_col_width = max_w + ImGui::GetStyle().ItemSpacing.x;
}

/* Vykreslí řádek s filepath + Browse.
 * optional == true: při najetí myší nad řádek se zobrazí tooltip "(optional)" */
static void DrawFileRow(const char *label, char *filepath, int filepath_size,
                         const char *browse_id, bool enabled, bool optional = false) {
    ImGui::BeginGroup();

    if (!enabled) ImGui::BeginDisabled();

    if (label) {
        ImGui::Text("%s", label);
        ImGui::SameLine(s_label_col_width);
    } else {
        ImGui::Text("  ");
        ImGui::SameLine();
    }

    char id_input[64];
    snprintf(id_input, sizeof(id_input), "##fp_%s", browse_id);
    ImGui::SetNextItemWidth(200);
    ImGui::InputText(id_input, filepath, filepath_size);
    ImGui::SameLine();

    char id_btn[64];
    snprintf(id_btn, sizeof(id_btn), "%s##%s", _("Browse..."), browse_id);
    if (ImGui::Button(id_btn)) {
        open_file_chooser(browse_id, filepath, filepath_size);
    }

    if (!enabled) ImGui::EndDisabled();

    ImGui::EndGroup();

    if (optional && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("optional"));
    }
}

/* Vykreslí panel pro ROM výběr (použitelný pro hlavní i CMT hack) */
static void DrawRomPanel(const char *title, RomPanelUI *panel, bool enabled, const char *id_prefix) {
    ImGui::PushID(id_prefix);
    ImGui::BeginGroup();

    if (title) {
        ImGui::SeparatorText(title);
    }

    if (!enabled) ImGui::BeginDisabled();

    /* Radio: All-In-One */
    if (ImGui::RadioButton(_L("All-In-One ROM Binary File"), panel->allinone)) {
        panel->allinone = true;
    }

    /* All-in-one file path */
    char aio_browse[64];
    snprintf(aio_browse, sizeof(aio_browse), "aio_%s", id_prefix);
    DrawFileRow(NULL, panel->allinone_fp, sizeof(panel->allinone_fp),
                aio_browse, enabled && panel->allinone);

    ImGui::Spacing();

    /* Radio: Separate Files */
    if (ImGui::RadioButton(_L("Separate ROM Binary Files"), !panel->allinone)) {
        panel->allinone = false;
    }

    bool sep_enabled = enabled && !panel->allinone;

    char mz700_id[64], cgrom_id[64], mz800_id[64];
    snprintf(mz700_id, sizeof(mz700_id), "mz700_%s", id_prefix);
    snprintf(cgrom_id, sizeof(cgrom_id), "cgrom_%s", id_prefix);
    snprintf(mz800_id, sizeof(mz800_id), "mz800_%s", id_prefix);

    DrawFileRow(_("ROM 0x0000"), panel->mz700_fp, sizeof(panel->mz700_fp), mz700_id, sep_enabled);
    DrawFileRow(_("CG-ROM"), panel->cgrom_fp, sizeof(panel->cgrom_fp), cgrom_id, sep_enabled);
    DrawFileRow(_("ROM 0xE000"), panel->mz800_fp, sizeof(panel->mz800_fp), mz800_id, sep_enabled, true);

    if (!enabled) ImGui::EndDisabled();

    ImGui::EndGroup();
    ImGui::PopID();
}

extern "C" void imgui_rom_settings_window(bool *p_open) {
    if (!*p_open)
        return;

    if (!s_initialized) {
        load_state();
        RecalcLabelWidth();
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse;

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *rom_title = _L("User Defined ROM Settings");
    auto_layout_first_use_portrait(rom_title, 500.0f, 400.0f);
    if (ImGui::Begin(rom_title, p_open, flags)) {

        /* Horní řádek: CMT Hack Patch combo vpravo */
        ImGui::Text("%s", _("CMT Hack Patch:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280);
        if (ImGui::BeginCombo("##cmthack", _(s_cmthack_items[s_state.cmthack_idx]))) {
            for (int i = 0; i < 3; i++) {
                bool selected = (s_state.cmthack_idx == i);
                if (ImGui::Selectable(_L(s_cmthack_items[i]), selected))
                    s_state.cmthack_idx = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        /* Dva panely vedle sebe */
        bool cmthack_custom = (s_state.cmthack_idx == 2);

        /* Levý panel: hlavní ROM */
        DrawRomPanel(_("ROM Binary Files"), &s_state.main, true, "main");

        /* Pokud je CMT hack custom, pravý panel */
        if (cmthack_custom) {
            ImGui::SameLine(0.0f, 20.0f);
            DrawRomPanel(_("CMT Hack Patch ROM"), &s_state.cmthack, true, "cmth");
        }

        /* Oddělovač a tlačítka */
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tlačítka OK / Cancel — zarovnání na střed */
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            /* Nastavení použije emu vlákno (přepis cest v g_rom, nové
             * načtení souborů a přeinstalace vybrané uživatelské ROM). */
            st_ROM_USER_DEFINED_SETTINGS s;
            s.allinone = s_state.main.allinone ? ROM_BOOL_YES : ROM_BOOL_NO;
            s.allinone_fp = s_state.main.allinone_fp;
            s.mz700_fp = s_state.main.mz700_fp;
            s.cgrom_fp = s_state.main.cgrom_fp;
            s.mz800_fp = s_state.main.mz800_fp;
            s.cmthack_type = (en_ROM_CMTHACK)s_state.cmthack_idx;
            s.cmthack_allinone = s_state.cmthack.allinone ? ROM_BOOL_YES : ROM_BOOL_NO;
            s.cmthack_allinone_fp = s_state.cmthack.allinone_fp;
            s.cmthack_mz700_fp = s_state.cmthack.mz700_fp;
            s.cmthack_cgrom_fp = s_state.cmthack.cgrom_fp;
            s.cmthack_mz800_fp = s_state.cmthack.mz800_fp;
            rom_request_set_user_defined(&s);

            s_initialized = false;
            *p_open = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_initialized = false;
            *p_open = false;
        }
    }
    ImGui::End();

    /* Zavření křížkem — zahodit dočasný stav */
    if (!*p_open) {
        s_initialized = false;
    }

    /* File chooser dialog */
    if (s_fch_open && s_fch_target) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display(s_fch_id)) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                snprintf(s_fch_target, s_fch_target_size, "%s", path.c_str());
            }
            ImGuiFileDialog::Instance()->Close();
            s_fch_open = false;
            s_fch_target = NULL;
        }
    }
}
