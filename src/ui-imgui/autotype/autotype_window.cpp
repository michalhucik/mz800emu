/**
 * @file autotype_window.cpp
 * @brief Okno s textovým bufferem pro automatické psaní do emulátoru
 *
 * Uživatel napíše text, který se automaticky "napíše" do emulátoru
 * simulací stisku kláves na klávesnicové matici PIO8255.
 * Obsahuje nastavení časování (key down/key up prodlevy v ms).
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
#include "hw-generic/pio8255/pio8255.h"
#include "emulator/replay.h"
}

/* Stav autotype okna */
static char s_text_buffer[4096] = "";
static int s_kd_ms = 20;       /* Key down prodleva v ms */
static int s_ku_ms = 20;       /* Key up prodleva v ms */
static bool s_autotype_active = false;
static char s_error_msg[256] = "";
static int s_typed_bytes = 0;   /* Počet již zpracovaných bajtů */
static int s_gen_before = 0;    /* Generace autotype před aktivací (pio8255_autotype_progress()); emu vlákno ho spustí až na hranici snímku */

/* Validace textu — kontrola, zda všechny znaky mají mapování v matici */
static bool validate_text(const char *text) {
    s_error_msg[0] = '\0';

    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];

        /* Přeskočit newline — ten se mapuje jako Enter */
        if (c == '\n' || c == '\r') continue;

        uint8_t ret;
        bool ret_shift;
        if (pio8255_autotype_get_matrix(c, &ret, &ret_shift) == -1) {
            snprintf(s_error_msg, sizeof(s_error_msg),
                     "%s %d: '%c' (0x%02X)",
                     _("Invalid character at position"), i + 1, c, (uint8_t)c);
            return false;
        }
    }
    return true;
}

/* Aktivace autotypu */
static void activate_autotype(void) {
    if (s_text_buffer[0] == '\0') return;

    if (!validate_text(s_text_buffer)) return;

    /* Prodlevy a text předat do PIO8255 (autotype engine si text převezme;
     * při záznamu vstupů až na hranici snímku jako kopii, při přehrávání
     * se zahodí - viz replay_host_autotype()). Změna prodlev za běhu
     * autotype se při záznamu / přehrávání neuplatní (byla by mimo
     * záznam). */
    pio8255_autotype_progress(&s_gen_before, NULL, NULL);
    replay_host_autotype(s_text_buffer, (double)s_kd_ms, (double)s_ku_ms);
    s_autotype_active = true;
    s_typed_bytes = 0;
    s_error_msg[0] = '\0';
}

/* Deaktivace autotypu */
static void deactivate_autotype(void) {
    replay_host_autotype(NULL, (double)s_kd_ms, (double)s_ku_ms);
    s_autotype_active = false;
}

/**
 * Vykreslení textu se dvěma barvami — napsaný text normální barvou,
 * zbývající text zeleně. Řeší správné rozdělení na řádky.
 */
static void render_colored_text(const char *text, int split_offset) {
    ImVec4 green = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    int total = (int)strlen(text);
    const char *end = text + total;
    const char *split = text + split_offset;
    if (split > end) split = end;
    if (split < text) split = text;

    const char *p = text;

    /* Zpracování po řádcích */
    while (true) {
        /* Najít konec aktuálního řádku */
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;

        if (p >= split) {
            /* Celý řádek je čekající (zelený) */
            ImGui::PushStyleColor(ImGuiCol_Text, green);
            if (nl > p)
                ImGui::TextUnformatted(p, nl);
            else
                ImGui::TextUnformatted("");
            ImGui::PopStyleColor();
        } else if (split >= nl) {
            /* Celý řádek je napsaný (normální barva) */
            if (nl > p)
                ImGui::TextUnformatted(p, nl);
            else
                ImGui::TextUnformatted("");
        } else {
            /* Řádek je rozdělen — část napsaná, část čekající */
            if (split > p)
                ImGui::TextUnformatted(p, split);
            ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, green);
            if (nl > split)
                ImGui::TextUnformatted(split, nl);
            else
                ImGui::TextUnformatted("");
            ImGui::PopStyleColor();
        }

        if (nl >= end) break;
        p = nl + 1;
    }
}

extern "C" void imgui_autotype_window(bool *p_open) {
    if (!*p_open) {
        return;
    }

    /* Průběh autotypu z atomických čítačů emulace (text vlastní emu vlákno,
     * okno ho nečte). Konec nastaví emulace sama na konci textu nebo při
     * zastavení. Dokud se generace nezměnila, autotype ještě čeká na
     * spuštění v emu vlákně. */
    if (s_autotype_active) {
        int gen, done_gen, typed;
        pio8255_autotype_progress(&gen, &done_gen, &typed);
        int total = (int)strlen(s_text_buffer);
        if (gen != s_gen_before) {
            s_typed_bytes = typed;
            if (s_typed_bytes < 0) s_typed_bytes = 0;
            if (s_typed_bytes > total) s_typed_bytes = total;
            if (done_gen == gen) s_autotype_active = false;
        }
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::SetNextWindowSize(ImVec2(780, 400), ImGuiCond_FirstUseEver);

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *autotype_title = _L("Autotype Buffer");
    auto_layout_first_use_portrait(autotype_title, 500.0f, 400.0f);
    if (ImGui::Begin(autotype_title, p_open, flags)) {

        /* Horní řádek: tlačítko Autotype + Key down/up spinbuttony */

        bool is_empty = (s_text_buffer[0] == '\0');
        bool was_active = s_autotype_active;

        /* Toggle tlačítko s barvou a labelem dle stavu */
        if (was_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        }

        /* Tlačítko nelze aktivovat s prázdným bufferem */
        if (is_empty && !was_active) {
            ImGui::BeginDisabled();
        }

        /* Pevná šířka tlačítka podle labelu "Autotype!" */
        const char *autotype_label = _("Autotype!");
        const char *btn_label = was_active ? _("Stop!") : autotype_label;
        ImVec2 btn_size = ImGui::CalcTextSize(autotype_label);
        btn_size.x += ImGui::GetStyle().FramePadding.x * 2;
        btn_size.y = 0; /* výchozí výška */
        if (ImGui::Button(btn_label, btn_size)) {
            if (!s_autotype_active) {
                activate_autotype();
            } else {
                deactivate_autotype();
            }
        }

        if (is_empty && !was_active) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", _("Text buffer is empty"));
            }
        }

        if (was_active) {
            ImGui::PopStyleColor(3);
        }

        /* Vertikální separátor za tlačítkem */
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, 10.0f);

        /* Key down spinbutton */
        ImGui::Text("%s:", _("Key down"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("##kd", &s_kd_ms, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            if (s_kd_ms < 0) s_kd_ms = 0;
            if (s_kd_ms > 100) s_kd_ms = 100;
            if (s_autotype_active) {
                replay_host_autotype_delays((double)s_kd_ms, (double)s_ku_ms);
            }
        }
        ImGui::SameLine();
        ImGui::Text("ms");

        /* Vizuální oddělení před Key up */
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, 10.0f);

        /* Key up spinbutton */
        ImGui::Text("%s:", _("Key up"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("##ku", &s_ku_ms, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            if (s_ku_ms < 0) s_ku_ms = 0;
            if (s_ku_ms > 100) s_ku_ms = 100;
            if (s_autotype_active) {
                replay_host_autotype_delays((double)s_kd_ms, (double)s_ku_ms);
            }
        }
        ImGui::SameLine();
        ImGui::Text("ms");

        ImGui::Spacing();

        /* Výška: progressbar + separátor + stavový řádek */
        float status_height = ImGui::GetFrameHeightWithSpacing() * 2
                            + ImGui::GetStyle().ItemSpacing.y * 2 + 8.0f;

        /* Textový editor / barevné zobrazení */
        ImVec2 text_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - status_height);

        if (s_autotype_active) {
            /* Během autotypu: barevné zobrazení v child window */
            if (ImGui::BeginChild("##autotype_display", text_size,
                                  ImGuiChildFlags_FrameStyle)) {
                render_colored_text(s_text_buffer, s_typed_bytes);
            }
            ImGui::EndChild();
        } else {
            /* Editovatelný text buffer */
            ImGuiInputTextFlags text_flags = ImGuiInputTextFlags_AllowTabInput;
            ImGui::InputTextMultiline("##autotype_text", s_text_buffer, sizeof(s_text_buffer),
                                      text_size, text_flags);
            /* Kliknutí do editoru — reset počítadla a progressbaru */
            if (ImGui::IsItemActive()) {
                s_typed_bytes = 0;
            }
        }

        /* Progressbar — plná šířka okna */
        int total_bytes = (int)strlen(s_text_buffer);
        float progress = (total_bytes > 0) ? (float)s_typed_bytes / (float)total_bytes : 0.0f;
        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0));

        /* Horizontální separátor */
        ImGui::Separator();

        /* Stavový řádek: Characters + Typed */
        char count_str[16];

        ImGui::Text("%s:", _("Characters"));
        ImGui::SameLine();
        snprintf(count_str, sizeof(count_str), "%d", total_bytes);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000").x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::InputText("##chars", count_str, sizeof(count_str), ImGuiInputTextFlags_ReadOnly);

        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, 10.0f);

        ImGui::Text("%s:", _("Typed"));
        ImGui::SameLine();
        snprintf(count_str, sizeof(count_str), "%d", s_typed_bytes);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000").x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::InputText("##typed", count_str, sizeof(count_str), ImGuiInputTextFlags_ReadOnly);

        /* Chybová hláška */
        if (s_error_msg[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", s_error_msg);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}
