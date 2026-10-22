/**
 * @file memext_load_dialog.cpp
 * @brief Dialog pro nahrání souboru do MemExt RAM nebo FLASH paměti
 *
 * Samotné čtení souboru do paměti MemExt provede emu vlákno
 * (memext_request_load()); dialog jen ověří, že soubor jde otevřít,
 * a zásah zařadí. Chybu čtení pak ohlásí okno zpráv.
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
#include "memext_content.h"

// Lokalizace
#include "i18n.h"

extern "C"
{
#include "emulator/hw-generic/memory/memext.h"
#include "emulator/hw-generic/memory/memext_request.h"
#include "baseui/baseui.h"
}

/* Stav load dialogu */
static bool s_load_open = false;
static en_MemextContentType s_load_type = MEMEXT_CONTENT_RAM;
static char s_load_filepath[1024] = "";
static uint32_t s_load_file_size = 0;
static uint32_t s_load_file_offset = 0;
static uint32_t s_load_mem_from = 0;
static uint32_t s_load_mem_size = MEMEXT_RAM_SIZE;
static bool s_load_fch_open = false;
static char s_load_error[256] = "";

/* Helper: Hex/Dec duální vstup */
static bool HexDecInput(const char *label, uint32_t *value, uint32_t max_value, const char *id_suffix) {
    bool changed = false;

    if (label) {
        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);
    }

    /* Hex vstup */
    char hex_buf[16];
    snprintf(hex_buf, sizeof(hex_buf), "%05X", *value);
    char id_hex[64];
    snprintf(id_hex, sizeof(id_hex), "##hex_%s", id_suffix);
    ImGui::Text("0x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputText(id_hex, hex_buf, sizeof(hex_buf), ImGuiInputTextFlags_CharsHexadecimal)) {
        uint32_t v = (uint32_t)strtoul(hex_buf, NULL, 16);
        if (v > max_value) v = max_value;
        *value = v;
        changed = true;
    }

    ImGui::SameLine();

    /* Dec vstup */
    int dec_val = (int)*value;
    char id_dec[64];
    snprintf(id_dec, sizeof(id_dec), "##dec_%s", id_suffix);
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt(id_dec, &dec_val, 0, 0)) {
        if (dec_val < 0) dec_val = 0;
        if ((uint32_t)dec_val > max_value) dec_val = (int)max_value;
        *value = (uint32_t)dec_val;
        changed = true;
    }

    return changed;
}

extern "C" void imgui_memext_open_load(en_MemextContentType type) {
    s_load_type = type;
    s_load_open = true;
    s_load_filepath[0] = '\0';
    s_load_file_size = 0;
    s_load_file_offset = 0;
    s_load_mem_from = 0;
    s_load_mem_size = (type == MEMEXT_CONTENT_RAM) ? MEMEXT_RAM_SIZE : MEMEXT_FLASH_SIZE;
    s_load_error[0] = '\0';
}

extern "C" void imgui_memext_load_dialog(void) {
    if (!s_load_open) return;

    uint32_t max_mem = (s_load_type == MEMEXT_CONTENT_RAM) ? MEMEXT_RAM_SIZE : MEMEXT_FLASH_SIZE;
    const char *title = (s_load_type == MEMEXT_CONTENT_RAM) ?
        _("Load MemExt RAM From File") : _("Load MemExt FLASH From File");

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

    /* Auto-layout poblíž kurzoru pro modální dialog. */
    auto_layout_first_use_near_mouse(title, 500.0f, 400.0f);
    if (ImGui::Begin(title, &s_load_open, flags)) {

        /* Sekce: Input File */
        ImGui::SeparatorText(_("Input File"));

        ImGui::Text("%s:", _("File"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##load_fp", s_load_filepath, sizeof(s_load_filepath), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(_L("Browse..."))) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_MEMEXT, config);
            ImGuiFileDialog::Instance()->OpenDialog("MemextLoadFch", _("Select file"), ".*", config);
            s_load_fch_open = true;
        }

        if (s_load_file_size > 0) {
            ImGui::Text("%s: 0x%08X (%u bytes)", _("File size"), s_load_file_size, s_load_file_size);
        }

        /* Sekce: File Offset */
        ImGui::SeparatorText(_("File Offset"));
        HexDecInput(_("Offset:"), &s_load_file_offset,
                    s_load_file_size > 0 ? s_load_file_size - 1 : 0, "foffset");

        /* Sekce: Memory Allocation */
        ImGui::SeparatorText(_("Memory Allocation"));

        /* From */
        HexDecInput(_("From:"), &s_load_mem_from, max_mem - 1, "mfrom");

        /* Size */
        ImGui::SameLine(0, 20);
        uint32_t max_size = max_mem - s_load_mem_from;
        if (s_load_file_size > 0 && (s_load_file_size - s_load_file_offset) < max_size) {
            max_size = s_load_file_size - s_load_file_offset;
        }
        HexDecInput(_("Size:"), &s_load_mem_size, max_size, "msize");

        /* Tlačítka */
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        /* Load tlačítko */
        bool can_load = (s_load_filepath[0] != '\0') && (s_load_file_size > 0) && (s_load_mem_size > 0);
        if (!can_load) ImGui::BeginDisabled();

        if (ImGui::Button(_L("Load"), ImVec2(120, 0))) {
            /* Otevření ověří dialog (chyba zůstane v dialogu), čtení do
             * paměti MemExt provede emu vlákno. */
            FILE *fp = fopen(s_load_filepath, "rb");
            if (fp) {
                fclose(fp);
                if (memext_request_load(s_load_type == MEMEXT_CONTENT_FLASH, s_load_filepath,
                                        s_load_file_offset, s_load_mem_from, s_load_mem_size)) {
                    s_load_open = false;
                } else {
                    snprintf(s_load_error, sizeof(s_load_error), "%s", _("Cannot open file"));
                }
            } else {
                snprintf(s_load_error, sizeof(s_load_error), "%s", _("Cannot open file"));
            }
        }

        if (!can_load) ImGui::EndDisabled();

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_load_open = false;
        }

        /* Chybová hláška */
        if (s_load_error[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", s_load_error);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    /* File chooser */
    if (s_load_fch_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("MemextLoadFch")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                snprintf(s_load_filepath, sizeof(s_load_filepath), "%s", path.c_str());
                /* Zjistit velikost souboru */
                FILE *fp = fopen(s_load_filepath, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    s_load_file_size = (uint32_t)ftell(fp);
                    fclose(fp);
                    /* Výchozí velikost = minimum z file_size a max_mem */
                    uint32_t max_mem_local = (s_load_type == MEMEXT_CONTENT_RAM) ? MEMEXT_RAM_SIZE : MEMEXT_FLASH_SIZE;
                    s_load_mem_size = (s_load_file_size < max_mem_local) ? s_load_file_size : max_mem_local;
                }
            }
            ImGuiFileDialog::Instance()->Close();
            s_load_fch_open = false;
        }
    }
}
