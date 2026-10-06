/**
 * @file memext_map_window.cpp
 * @brief Konfigurační okno pro mapování banků MemExt na adresní prostor Z80
 *
 * 16 address pointů po 4 KB (0x0000-0xFFFF).
 * PEHU: 8 editovatelných řádků (sudé address pointy, číslo 8 KB banky
 *       0x00-0x3F = hodnota OUT E7h, ne 4 KB raw banka z g_memext.map[])
 * Luftner: 16 editovatelných řádků (4 KB banky, bit 7 = FLASH)
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

// Lokalizace
#include "i18n.h"

extern "C"
{
#include "emulator/hw-generic/memory/memext.h"
}

/**
 * @brief Editované hodnoty mapování - hodnoty zápisu OUT E7h pro address
 *        pointy 0..15.
 *
 * LUFTNER: 4 KB raw banka (bit 7 = FLASH). PEHU: číslo 8 KB banky
 * (0x00-0x3F), sudý a lichý point páru drží stejnou hodnotu; při Apply se
 * použijí jen sudé pointy (memext_map_request_process()).
 */
static uint8_t s_bank_values[MEMEXT_RAW_MAP_SIZE];

/** @brief true = s_bank_values je načtené pro aktuální otevření okna. */
static bool s_initialized = false;

/**
 * @brief Načte s_bank_values z aktuální mapy MemExt.
 *
 * Převod přes memext_map_get_out_values(): u PEHU g_memext.map[] drží
 * 4 KB raw banky (2n, 2n+1), editace a OUT E7h ale pracují s číslem
 * 8 KB banky n. Dřívější přímé kopírování map[i] dávalo u PEHU dvojnásobná
 * čísla a Apply bez úprav mapu rozházelo (2,3,6,7,...,30,31 místo 0..15).
 */
static void load_map_state(void) {
    memext_map_get_out_values(s_bank_values);
}

/**
 * @brief Aplikuje upravené mapování (tlačítka Apply a OK).
 *
 * Zápis g_memext.map[] a memory_reconnect_ram() (RAM ukazatele, MZ-800
 * fast-path CPU) smí provést jen emu vlákno - souběžně s ním probíhají
 * přístupy CPU do paměti i OUT E7h. Proto okno jen předá kopii
 * s_bank_values přes memext_map_request(); memext_map_pwrite() pro
 * address pointy (LUFTNER 0..15, PEHU jen sudé) zavolá emu vlákno při
 * nejbližší emulační události (v pauze do ~20 ms). Funguje s debuggerem
 * i bez něj (nepoužívá frontu dbgapi).
 *
 * Asynchronní: Refresh hned po Apply může ještě ukázat původní hodnoty.
 */
static void apply_map(void) {
    memext_map_request(s_bank_values);
}

extern "C" void imgui_memext_map_window(bool *p_open) {
    if (!*p_open)
        return;

    if (!s_initialized) {
        load_map_state();
        s_initialized = true;
    }

    bool is_pehu = MEMEXT_TEST_TYPE_PEHU;
    bool is_luftner = MEMEXT_TEST_TYPE_LUFTNER;
    int max_bank = is_pehu ? MEMEXT_PEHU_MASK : 0xFF;

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *memext_title = _L("MemExt Map Settings");
    auto_layout_first_use_portrait(memext_title, 500.0f, 400.0f);
    if (ImGui::Begin(memext_title, p_open, flags)) {

        /* Refresh tlačítko vpravo nahoře */
        float avail_w = ImGui::GetContentRegionAvail().x;
        float btn_w = ImGui::CalcTextSize(_("Refresh")).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_w - btn_w);
        if (ImGui::Button(_L("Refresh"))) {
            load_map_state();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        /* Info sekce — zarovnání dvojteček do sloupce */
        {
            /* Vypočítat šířku nejdelšího labelu pro zarovnání */
            const char *lbl_type = _("Type");
            const char *lbl_banksize = _("Bank size");
            const char *lbl_ram = "RAM 512 kB";
            const char *lbl_flash = "FLASH 512 kB";

            float w_type = ImGui::CalcTextSize(lbl_type).x;
            float w_banksize = ImGui::CalcTextSize(lbl_banksize).x;
            float w_ram = ImGui::CalcTextSize(lbl_ram).x;
            float max_lbl_w = w_type;
            if (w_banksize > max_lbl_w) max_lbl_w = w_banksize;
            if (w_ram > max_lbl_w) max_lbl_w = w_ram;
            if (!is_pehu) {
                float w_flash = ImGui::CalcTextSize(lbl_flash).x;
                if (w_flash > max_lbl_w) max_lbl_w = w_flash;
            }
            /* Zarovnávací pozice pro " : hodnota" */
            float colon_x = max_lbl_w + ImGui::GetStyle().ItemSpacing.x;

            if (is_pehu) {
                ImGui::Text("%s", lbl_type); ImGui::SameLine(colon_x);
                ImGui::Text(":   %s", _("Model 'Peroutka & Hucik'"));
                ImGui::Text("%s", lbl_banksize); ImGui::SameLine(colon_x);
                ImGui::Text(":   0x2000");
                ImGui::Text("%s", lbl_ram); ImGui::SameLine(colon_x);
                ImGui::Text(":   0x00 - 0x3F");
            } else {
                ImGui::Text("%s", lbl_type); ImGui::SameLine(colon_x);
                ImGui::Text(":   %s", _("Model 'Luftner'"));
                ImGui::Text("%s", lbl_banksize); ImGui::SameLine(colon_x);
                ImGui::Text(":   0x1000");
                ImGui::Text("%s", lbl_ram); ImGui::SameLine(colon_x);
                ImGui::Text(":   0x00 - 0x7F");
                ImGui::Text("%s", lbl_flash); ImGui::SameLine(colon_x);
                ImGui::Text(":   0x80 - 0xFF");
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        /* Pozice sloupců tabulky — zvětšené rozestupy */
        float col_loc = 0;
        float col_hex = 170;
        float col_dec = 300;
        float col_type = 420;

        /* Zvýrazněné záhlaví tabulky */
        {
            ImVec2 hdr_min = ImGui::GetCursorScreenPos();
            hdr_min.x = ImGui::GetWindowPos().x;
            float hdr_h = ImGui::GetFrameHeightWithSpacing();
            ImVec2 hdr_max = ImVec2(hdr_min.x + ImGui::GetWindowWidth(), hdr_min.y + hdr_h);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 hdr_bg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg, 1.0f);
            dl->AddRectFilled(hdr_min, hdr_max, hdr_bg);
        }

        ImGui::SetCursorPosX(col_loc);
        ImGui::Text("%s", _("Location"));
        ImGui::SameLine(col_hex);
        ImGui::Text("%s", _("Bank HEX"));
        ImGui::SameLine(col_dec);
        ImGui::Text("%s", _("Bank DEC"));
        ImGui::SameLine(col_type);
        ImGui::Text("%s", _("Memory type"));

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();

        /* Barva pro střídavé pozadí řádků — výrazná */
        ImU32 row_bg_alt = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt, 1.0f);
        ImDrawList *draw_list = ImGui::GetWindowDrawList();

        /* Tabulka mapování */
        for (int i = 0; i < MEMEXT_RAW_MAP_SIZE; i++) {
            /* Na začátku každého páru (sudý řádek): mezera mezi páry */
            if (i > 0 && (i % 2) == 0) {
                ImGui::Spacing();
                ImGui::Spacing();
            }

            /* Střídavé pozadí — na začátku zvýrazněného páru nakreslit obdélník přes oba řádky */
            if ((i % 2) == 0 && (i / 2) % 2 == 1) {
                ImVec2 pair_min = ImGui::GetCursorScreenPos();
                pair_min.x = ImGui::GetWindowPos().x;
                /* Výška pro 2 řádky */
                float row_h = ImGui::GetFrameHeightWithSpacing();
                float pair_h = row_h * 2;
                ImVec2 pair_max = ImVec2(pair_min.x + ImGui::GetWindowWidth(), pair_min.y + pair_h);
                draw_list->AddRectFilled(pair_min, pair_max, row_bg_alt);
            }

            /* Adresa */
            ImGui::Text("%X000 - %XFFF", i, i);

            /* U PEHU: jen sudé řádky mají editovatelná pole */
            bool editable;
            if (is_pehu) {
                editable = ((i % 2) == 0);
            } else {
                editable = true;
            }

            if (editable) {
                ImGui::SameLine(col_hex);

                /* Hex vstup — pouze hex znaky, max 2 znaky */
                char hex_buf[4];
                snprintf(hex_buf, sizeof(hex_buf), "%02X", s_bank_values[i]);
                char id_hex[32];
                snprintf(id_hex, sizeof(id_hex), "##mhex%d", i);
                ImGui::SetNextItemWidth(50);
                if (ImGui::InputText(id_hex, hex_buf, sizeof(hex_buf),
                                     ImGuiInputTextFlags_CharsHexadecimal |
                                     ImGuiInputTextFlags_CharsUppercase)) {
                    int v = (int)strtol(hex_buf, NULL, 16);
                    if (v < 0) v = 0;
                    if (v > max_bank) v = max_bank;
                    s_bank_values[i] = (uint8_t)v;
                    /* PEHU: párování — nastavit i lichý */
                    if (is_pehu && (i + 1 < MEMEXT_RAW_MAP_SIZE)) {
                        s_bank_values[i + 1] = s_bank_values[i];
                    }
                }

                ImGui::SameLine(col_dec);

                /* Dec vstup — pouze čísla */
                int dec_val = s_bank_values[i];
                char id_dec[32];
                snprintf(id_dec, sizeof(id_dec), "##mdec%d", i);
                ImGui::SetNextItemWidth(60);
                if (ImGui::InputInt(id_dec, &dec_val, 0, 0,
                                    ImGuiInputTextFlags_CharsDecimal)) {
                    if (dec_val < 0) dec_val = 0;
                    if (dec_val > max_bank) dec_val = max_bank;
                    s_bank_values[i] = (uint8_t)dec_val;
                    if (is_pehu && (i + 1 < MEMEXT_RAW_MAP_SIZE)) {
                        s_bank_values[i + 1] = s_bank_values[i];
                    }
                }

                ImGui::SameLine(col_type);

                /* Memory type */
                if (is_luftner && (s_bank_values[i] & 0x80)) {
                    ImGui::Text("FLASH");
                } else {
                    ImGui::Text("RAM");
                }
            } else {
                /* Lichý řádek u PEHU — zobrazit RAM/FLASH bez vstupních polí */
                ImGui::SameLine(col_type);
                ImGui::Text("RAM");
            }
        }

        /* Tlačítka — větší mezery nad a pod */
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();

        float total_btn_width = 120 * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button(_L("Apply"), ImVec2(120, 0))) {
            apply_map();
        }

        ImGui::SameLine();

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            apply_map();
            s_initialized = false;
            *p_open = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_initialized = false;
            *p_open = false;
        }

        ImGui::Spacing();
    }
    ImGui::End();

    /* Zavření křížkem — zahodit dočasný stav */
    if (!*p_open) {
        s_initialized = false;
    }
}
