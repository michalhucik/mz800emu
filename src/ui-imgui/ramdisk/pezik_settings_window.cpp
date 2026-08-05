/**
 * @file pezik_settings_window.cpp
 * @brief Konfigurační dialog pro dva PEZIK ramdisk moduly
 *
 * Standard PEZIK (porty 0xE8-0xEF) a Experimental PEZIK (porty 0x68-0x6F).
 * Každý PEZIK má 8 banků po 64 KB (celkem 512 KB), přičemž jednotlivé
 * banky lze individuálně zapínat/vypínat přes port masku.
 *
 * Na MZ-1500 je Standard PEZIK skrytý (port 0xE9 je obsazen PSG).
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

// Lokalizace
#include "i18n.h"
#include "emulator/mzarch/mzhal.h"

extern "C"
{
#include "emulator/hw-generic/ramdisk/ramdisk.h"
}

/* Dočasný stav pro jeden PEZIK modul */
struct PezikSettingsUI {
    bool connected;
    int combo_idx;          /* 0=Full Size, 1=Upper Half, 2=Lower Half, 3=Custom */
    uint8_t portmask;
    bool backuped;
    char filepath[1024];
};

/* Stav celého dialogu */
struct PezikDialogState {
    PezikSettingsUI e8;     /* Standard PEZIK (0xE8-0xEF) */
    PezikSettingsUI p68;    /* Experimental PEZIK (0x68-0x6F) */
};

static PezikDialogState s_state;
static bool s_initialized = false;

/* ID pro file chooser dialogy */
static const char *s_fch_id_e8 = "PezikFchE8";
static const char *s_fch_id_68 = "PezikFch68";
static bool s_fch_open_e8 = false;
static bool s_fch_open_68 = false;

/* Combo popisky */
static const char *s_combo_items[] = {
    "Full Size - Default",
    "Upper Half",
    "Lower Half",
    "Custom"
};

/* Zjistí combo index z portmasky */
static int portmask_to_combo(uint8_t mask) {
    if (mask == 0xFF) return 0;
    if (mask == 0xF0) return 1;
    if (mask == 0x0F) return 2;
    return 3;
}

/* Spočítá velikost v kB podle počtu aktivních banků */
static int calc_size_kb(uint8_t portmask) {
    int count = 0;
    for (int i = 0; i < 8; i++) {
        if (portmask & (1 << i)) count++;
    }
    return count * 64;
}

/* Načte stav jednoho PEZIK modulu z globální struktury */
static void load_pezik_state(PezikSettingsUI *ui, int pezik_type) {
    st_RAMDISKPEZIK *p = &g_ramdisk.pezik[pezik_type];
    ui->connected = (p->connected == RAMDISK_CONNECTED);
    ui->portmask = (uint8_t)(p->portmask & 0xFF);
    ui->combo_idx = portmask_to_combo(ui->portmask);
    ui->backuped = (p->backuped == PEZIK_BACKUPED_YES);
    if (p->filepath) {
        snprintf(ui->filepath, sizeof(ui->filepath), "%s", p->filepath);
    } else {
        ui->filepath[0] = '\0';
    }
}

/* Vykreslí sekci pro jeden PEZIK modul */
static void DrawPezikSection(const char *label, PezikSettingsUI *ui,
                              const char *port_labels[], const char *id_suffix,
                              bool *fch_open, const char *fch_id) {
    int size_kb = calc_size_kb(ui->portmask);

    /* Titulek sekce s dynamickou velikostí */
    char section_title[128];
    snprintf(section_title, sizeof(section_title), "%s %d kB", label, size_kb);
    ImGui::SeparatorText(section_title);

    /* Connected checkbox + Combo na jednom řádku */
    char id_connected[32];
    snprintf(id_connected, sizeof(id_connected), "##conn_%s", id_suffix);
    ImGui::Checkbox(id_connected, &ui->connected);
    ImGui::SameLine();
    ImGui::Text("%s", _("Connected"));

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20.0f);

    /* Combo pro výběr režimu - disabled pokud nepřipojeno */
    if (!ui->connected) ImGui::BeginDisabled();

    char id_combo[32];
    snprintf(id_combo, sizeof(id_combo), "##combo_%s", id_suffix);
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo(id_combo, _(s_combo_items[ui->combo_idx]))) {
        for (int i = 0; i < 4; i++) {
            bool selected = (ui->combo_idx == i);
            if (ImGui::Selectable(_L(s_combo_items[i]), selected)) {
                ui->combo_idx = i;
                switch (i) {
                    case 0: ui->portmask = 0xFF; break;
                    case 1: ui->portmask = 0xF0; break;
                    case 2: ui->portmask = 0x0F; break;
                    /* case 3: Custom - ponechat aktuální portmask */
                }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    /* Porty - checkboxy */
    ImGui::Text("%s", _("Ports:"));
    ImGui::SameLine();

    /* Popisky portů */
    float start_x = ImGui::GetCursorPosX() + 10.0f;
    ImGui::SetCursorPosX(start_x);
    for (int i = 0; i < 8; i++) {
        if (i > 0) ImGui::SameLine();
        ImGui::Text("%s", port_labels[i]);
    }

    /* Checkboxy portů */
    ImGui::Text("       ");
    ImGui::SameLine();
    ImGui::SetCursorPosX(start_x);

    bool custom_mode = (ui->combo_idx == 3);
    if (!custom_mode) ImGui::BeginDisabled();

    for (int i = 0; i < 8; i++) {
        if (i > 0) ImGui::SameLine();
        char id_port[32];
        snprintf(id_port, sizeof(id_port), "##port%d_%s", i, id_suffix);
        bool bit_set = (ui->portmask & (1 << i)) != 0;
        if (ImGui::Checkbox(id_port, &bit_set)) {
            if (bit_set) {
                ui->portmask |= (1 << i);
            } else {
                /* Nelze vypnout všechny — minimálně 1 port musí zůstat aktivní */
                uint8_t new_mask = ui->portmask & ~(1 << i);
                if (new_mask != 0) {
                    ui->portmask = new_mask;
                }
            }
        }
    }

    if (!custom_mode) ImGui::EndDisabled();

    /* Backuped checkbox + filepath */
    ImGui::Spacing();
    char id_backuped[32];
    snprintf(id_backuped, sizeof(id_backuped), "##bkp_%s", id_suffix);
    ImGui::Checkbox(id_backuped, &ui->backuped);
    ImGui::SameLine();
    ImGui::Text("%s", _("Backuped"));
    ImGui::SameLine();

    /* Filepath pole - disabled pokud Backuped není zaškrtnutý */
    if (!ui->backuped) ImGui::BeginDisabled();

    char id_filepath[32];
    snprintf(id_filepath, sizeof(id_filepath), "##fpath_%s", id_suffix);
    ImGui::SetNextItemWidth(200);
    ImGui::InputText(id_filepath, ui->filepath, sizeof(ui->filepath));

    ImGui::SameLine();
    char id_browse[32];
    snprintf(id_browse, sizeof(id_browse), "%s##%s", _("Browse..."), id_suffix);
    if (ImGui::Button(id_browse)) {
        IGFD::FileDialogConfig config;
        config.path = (ui->filepath[0] != '\0') ? ui->filepath : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles |
                       ImGuiFileDialogFlags_ShowDevicesButton;
        ImGuiFileDialog::Instance()->OpenDialog(fch_id, _("Select backup file"), ".dat,.*", config);
        *fch_open = true;
    }

    if (!ui->backuped) ImGui::EndDisabled();

    if (!ui->connected) ImGui::EndDisabled();

    ImGui::Spacing();
}

extern "C" void imgui_pezik_settings_window(bool *p_open) {
    if (!*p_open)
        return;

    /* Při prvním otevření zkopírovat aktuální hodnoty z g_ramdisk */
    if (!s_initialized) {
/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
    if (g_mzhal.arch != 1500) {
        load_pezik_state(&s_state.e8, RAMDISK_PEZIK_E8);
    }
        load_pezik_state(&s_state.p68, RAMDISK_PEZIK_68);
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse;

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *pezik_title = _L("Settings for PEZIK Ramdisks");
    auto_layout_first_use_portrait(pezik_title, 500.0f, 400.0f);
    if (ImGui::Begin(pezik_title, p_open, flags)) {

        /* Port labels pro oba PEZIKy */
        static const char *ports_e8[] = { "E8", "E9", "EA", "EB", "EC", "ED", "EE", "EF" };
        static const char *ports_68[] = { "68", "69", "6A", "6B", "6C", "6D", "6E", "6F" };

/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
    if (g_mzhal.arch != 1500) {
        /* Standard PEZIK (E8) — skrytý na MZ-1500 */
        DrawPezikSection(_("Standard PEZIK:"), &s_state.e8, ports_e8, "e8",
                         &s_fch_open_e8, s_fch_id_e8);
    }

        /* Experimental PEZIK (68) */
        DrawPezikSection(_("Experimental PEZIK:"), &s_state.p68, ports_68, "68",
                         &s_fch_open_68, s_fch_id_68);

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
/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
    if (g_mzhal.arch != 1500) {
            /* Při zapínání E8 PEZIK — odpojit MR1R18 (sdílí porty 0xE8-0xEF) */
            if (s_state.e8.connected && RAMDISK_TEST_STD_CONNECTED) {
                ramdisk_std_disconnect();
            }
            ramdisk_pezik_init(RAMDISK_PEZIK_E8,
                               s_state.e8.connected ? RAMDISK_CONNECTED : RAMDISK_DISCONNECTED,
                               s_state.e8.portmask,
                               s_state.e8.backuped ? PEZIK_BACKUPED_YES : PEZIK_BACKUPED_NO,
                               s_state.e8.filepath);
    }
            ramdisk_pezik_init(RAMDISK_PEZIK_68,
                               s_state.p68.connected ? RAMDISK_CONNECTED : RAMDISK_DISCONNECTED,
                               s_state.p68.portmask,
                               s_state.p68.backuped ? PEZIK_BACKUPED_YES : PEZIK_BACKUPED_NO,
                               s_state.p68.filepath);

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

    /* File chooser dialogy */
/* Pezik E8 nelze na MZ-1500 (kolize portu 0xE8-0xEF) - runtime dle arch. */
    if (g_mzhal.arch != 1500) {
    if (s_fch_open_e8) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display(s_fch_id_e8)) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                snprintf(s_state.e8.filepath, sizeof(s_state.e8.filepath), "%s", path.c_str());
            }
            ImGuiFileDialog::Instance()->Close();
            s_fch_open_e8 = false;
        }
    }
    }

    if (s_fch_open_68) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display(s_fch_id_68)) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                snprintf(s_state.p68.filepath, sizeof(s_state.p68.filepath), "%s", path.c_str());
            }
            ImGuiFileDialog::Instance()->Close();
            s_fch_open_68 = false;
        }
    }
}
