/**
 * @file psg_window.cpp
 * @brief Debugger: PSG SN76489 chip state inspector window (per-chip-panels F5).
 *
 * Real-time snapshot vnitřního stavu PSG SN76489:
 *  - latched channel select (CS) + latched attn flag (kam půjde další DATA byte),
 *  - per kanál (0..3) typ TONE/NOISE, attenuation s dB konverzí,
 *  - TONE kanály: 10-bit tone divider + odvozená frekvence (Hz) + MIDI nota,
 *  - NOISE kanál (CH3 v noise módu): noise divider type, white/periodic feedback.
 *
 * MZ-800 má mono PSG (1 chip), MZ-1500 má stereo (2 chipy = L/R). MZ-700
 * žádný PSG nemá - TU je sdílené (mz_emucore), volající gatují okno
 * runtime přes `g_mzhal.psg_count >= 1` (menu i render loop).
 * Stereo flag se čte z `g_psg_module.stereo`.
 *
 * Data source: výhradně přes mirror API z `hw-generic/psg/psg.h` (side-effect
 * free, viz CHECKLIST 2.5). Žádné `psg_write_byte()` / `psg_step()`.
 *
 * Frekvenční vzorec (SN76489):
 *   f_out = GDGCLK_BASE / (32 * divider * GDGCLK2CPU_DIVIDER)
 *         = CPU_CLOCK / (32 * divider)
 *   - PSG_DIVIDER = 16 * GDGCLK2CPU_DIVIDER GDG ticků mezi `psg_step()`
 *     voláními (psg.h),
 *   - každý `psg_step` dekrementuje timer, při 0 toggle output_signal,
 *     plná perioda = 2 * divider * PSG_DIVIDER GDG ticků
 *     = 32 * divider * GDGCLK2CPU_DIVIDER GDG ticků,
 *   - výsledek odpovídá standardní SN76489 formuli (input clock CPU,
 *     interní /16 prescaler, 2x toggle pro full period),
 *   - divider < 2 znamená PSG drží výstup na 1 (DC, viz `psg.c` řádek 188-190).
 *
 * MIDI nota: `note = 12 * log2(f / 440) + 69`, A4 = 69 = 440 Hz.
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"

#include "i18n.h"

#include "mzarch/mzcommon_config.h"

#include "ui-imgui/debugger/chip_window_helpers.h"
#include "ui-imgui/debugger/psg_window.h"


extern "C"
{
#include "hw-generic/psg/psg.h"
#include "emulator/mzarch/mzhal.h"

    void imgui_psg_state_window(bool *p_open);
}

#include <math.h>
#include <stdio.h>

/* ===================================================================== */
/* Pomocné statické funkce / konstanty                                   */
/* ===================================================================== */

/**
 * @brief Stringové labely pro `en_PSG_CHTYPE` enum.
 *
 * Prvky obalené N_() pro extrakci do .pot. Runtime překlad provádí
 * render_enum_row() přes _() na vybraném prvku.
 */
static const char *const psg_chtype_names[] = {
    N_("TONE"),  /* PSG_CHTYPE_TONE = 0 */
    N_("NOISE"), /* PSG_CHTYPE_NOISE = 1 */
};

/**
 * @brief Stringové labely pro `en_NOISE_DIV_TYPE` enum.
 *
 * 0..2 = fixed PSG noise dividers (16/32/64), 3 = řízeno tone divider CH2.
 */
static const char *const psg_noise_div_type_names[] = {
    N_("DIV /16 (fast)"),
    N_("DIV /32 (mid)"),
    N_("DIV /64 (slow)"),
    N_("DIV from CH2 tone"),
};

/**
 * @brief Stringové labely pro `en_NOISE_TYPE` enum.
 */
static const char *const psg_noise_type_names[] = {
    N_("PERIODIC"),
    N_("WHITE"),
};

/**
 * @brief Názvy MIDI not v rámci oktávy (12-tone equal temperament).
 */
static const char *const midi_note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/**
 * @brief Převede attenuation (0..15) na text s dB hodnotou.
 *
 * SN76489 datasheet: každý krok je -2 dB, 0 = 0 dB (max volume), 15 = output
 * disabled (silent). Výsledek se zapíše do `out` (buffer min 24 znaků).
 *
 * @param attn  Hodnota 0..15 (en_ATTENUATOR).
 * @param out   Výstupní buffer.
 * @param sz    Velikost bufferu.
 */
static void format_attn_db(unsigned attn, char *out, size_t sz)
{
    if (attn == 0)
        snprintf(out, sz, "%s", _("0 dB (max)"));
    else if (attn >= 15)
        snprintf(out, sz, "%s", _("silent (off)"));
    else
        snprintf(out, sz, "-%u dB", attn * 2u);
}

/**
 * @brief Vypočte frekvenci square-wave výstupu TONE kanálu.
 *
 * Vzorec: f = GDGCLK_BASE / (32 * divider * GDGCLK2CPU_DIVIDER),
 * ekvivalentně CPU_CLOCK / (32 * divider). Pro divider < 2 vrací 0
 * (PSG v `psg.c` drží output_signal=1, není to oscilace).
 *
 * GDGCLK_BASE a GDGCLK2CPU_DIVIDER jsou per-arch makra. Výsledný CPU
 * clock: MZ-800 = 17 721 600 / 5 = 3.5469 MHz, MZ-700/MZ-1500 =
 * 14 336 640 / 4 = 3.5796 MHz. Standardní SN76489 formule s input
 * clock = CPU clock, interní /16 prescaler a 2x toggle pro plnou periodu.
 *
 * @param divider  10-bit tone divider (0..1023).
 * @return Frekvence v Hz, nebo 0.0 pro DC případ.
 */
static double psg_tone_frequency_hz(unsigned divider)
{
    if (divider < 2u)
        return 0.0;
    /* Runtime z g_mzhal (mzhal 10b, cold - UI render). */
    return (double)g_mzhal.gdgclk_base / (32.0 * (double)divider * (double)g_mzhal.gdgclk2cpu_divider);
}

/**
 * @brief Převede frekvenci Hz na MIDI notu + cents detuning.
 *
 * `note = 12 * log2(f / 440) + 69` (A4 = 69 = 440 Hz). Cents = zlomková
 * část * 100, znaménko ukazuje, jak daleko od nejbližší rovnoměrně
 * temperované noty.
 *
 * @param freq_hz       Vstupní frekvence (> 0).
 * @param out           Buffer pro výstupní text (formát "A4 +5c" nebo "C-1").
 * @param sz            Velikost bufferu.
 */
static void format_midi_note(double freq_hz, char *out, size_t sz)
{
    if (freq_hz <= 0.0)
    {
        snprintf(out, sz, "-");
        return;
    }

    double midi = 12.0 * log2(freq_hz / 440.0) + 69.0;
    int nearest = (int)lround(midi);
    int cents = (int)lround((midi - (double)nearest) * 100.0);

    /* MIDI 0 = C-1, MIDI 12 = C0, MIDI 60 = C4, ... */
    int octave = (nearest / 12) - 1;
    int idx = nearest % 12;
    if (idx < 0)
    {
        idx += 12;
        octave -= 1;
    }

    if (cents == 0)
        snprintf(out, sz, "%s%d", midi_note_names[idx], octave);
    else
        snprintf(out, sz, "%s%d %+dc", midi_note_names[idx], octave, cents);
}

/* ===================================================================== */
/* Waveform vizualizace (oscilloscope) per kanál                         */
/* ===================================================================== */

/**
 * @brief Velikost waveform regionu (px) - kompromis mezi čitelností a místem.
 */
static const ImVec2 PSG_WAVEFORM_SIZE = ImVec2(220.0f, 44.0f);

/**
 * @brief Počet period TONE square wave viditelných v rámci waveform regionu.
 *
 * Pevná hodnota - region vždy ukazuje 4 cykly bez ohledu na skutečnou
 * frekvenci, takže low f i high f vypadá vizuálně konzistentně (nezávislé
 * na časovém měřítku reálného sample bufferu).
 */
static const int PSG_WAVEFORM_TONE_PERIODS = 4;

/**
 * @brief Vykreslí oscilloscope-style waveform jednoho PSG kanálu.
 *
 * Vstupní logika čte pouze přes mirror gettery (side-effect free, dle CHECKLIST
 * 2.5) - žádné `psg_step()` ani state-changing volání. Vykreslení je čistě
 * vizuální reprezentace AKTUÁLNÍHO stavu PSG, ne přehrávání samplu z audio
 * bufferu.
 *
 * Rozhodnutí:
 *  - TONE: square wave 50% duty, pevně `PSG_WAVEFORM_TONE_PERIODS` period
 *    napříč regionem (nepokoušíme se počítat reálné samples - mimo scope).
 *  - NOISE: deterministický pseudo-random pattern (xor shift `seed ^ x_pixel`),
 *    konzistentní vzhled mezi frame-y. Rozlišení WHITE vs PERIODIC noise zatím
 *    pouze textově ve výše uvedené noise tabulce (TODO: separate pattern).
 *  - DC případ (`divider < 2`) → vodorovná čára uprostřed + "DC" label.
 *  - Silent (`attn >= 15`) → vodorovná čára uprostřed (amplituda 0).
 *  - Amplituda: lineární mapování `(15 - attn) / 15.0`. Není to dB-correct,
 *    ale vizuálně intuitivní (attn 0 = max výška, attn 15 = nula).
 *
 * @param psg           Ukazatel na PSG instanci (NESMÍ být NULL).
 * @param ch            Index kanálu (0..3).
 * @param id_suffix     Unikátní suffix pro InvisibleButton ID.
 */
static void render_channel_waveform(const st_PSG *psg, unsigned ch,
                                    const char *id_suffix)
{
    en_PSG_CHTYPE type = psg_mirror_channel_type(psg, ch);
    unsigned attn = psg_mirror_channel_attn(psg, ch);

    /* Reservace prostoru přes InvisibleButton - vrátí rect pro draw list. */
    /* 96 = id_suffix max 63 + "##wave_" 7 + \0 + rezerva. */
    char btnid[96];
    snprintf(btnid, sizeof(btnid), "##wave_%s", id_suffix);
    ImGui::InvisibleButton(btnid, PSG_WAVEFORM_SIZE);
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Tmavé pozadí + tenký rámeček pro kontrast s waveform. */
    const ImU32 bg_col = IM_COL32(18, 22, 32, 255);
    const ImU32 border_col = IM_COL32(70, 80, 100, 255);
    const ImU32 mid_col = IM_COL32(60, 70, 90, 255);
    const ImU32 wave_col = IM_COL32(180, 220, 255, 255);
    const ImU32 silent_col = IM_COL32(120, 120, 130, 255);

    dl->AddRectFilled(p0, p1, bg_col);
    dl->AddRect(p0, p1, border_col);

    float w = p1.x - p0.x;
    float h = p1.y - p0.y;
    float mid_y = p0.y + h * 0.5f;

    /* Středová horizontála jako reference (0 V). */
    dl->AddLine(ImVec2(p0.x, mid_y), ImVec2(p1.x, mid_y), mid_col, 1.0f);

    /* Silent kanál - jen vodorovná čára a popisek. */
    if (attn >= 15u)
    {
        dl->AddLine(ImVec2(p0.x + 2.0f, mid_y), ImVec2(p1.x - 2.0f, mid_y),
                    silent_col, 1.5f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), silent_col, _("silent"));
        return;
    }

    /* Lineární amplituda - 0 attn → plný rozsah, 14 attn → 1/15 rozsahu. */
    float amp_norm = (15.0f - (float)attn) / 15.0f;
    float amp_px = (h * 0.45f) * amp_norm;

    if (type == PSG_CHTYPE_TONE)
    {
        uint16_t divider = psg_mirror_channel_tone_divider(psg, ch);

        if (divider < 2u)
        {
            /* DC - PSG drží output_signal=1, žádná oscilace. Vykreslíme horní
             * úroveň jako stálou linku a označíme "DC". */
            float dc_y = mid_y - amp_px;
            dl->AddLine(ImVec2(p0.x + 2.0f, dc_y), ImVec2(p1.x - 2.0f, dc_y),
                        wave_col, 1.5f);
            dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), wave_col, _("DC"));
            return;
        }

        /* Square wave - 50% duty cycle, PSG_WAVEFORM_TONE_PERIODS period
         * v rámci regionu. Pole bodů: pro N period potřebujeme 4*N + 1 bodů
         * (každá perioda = nahoru, dolů, dolů, nahoru = 4 segmenty). */
        const int periods = PSG_WAVEFORM_TONE_PERIODS;
        const int n_pts = periods * 4 + 1;
        ImVec2 pts[64];
        if (n_pts > (int)(sizeof(pts) / sizeof(pts[0])))
            return; /* defensivní - pro periods=4 je n_pts=17, fits */

        float period_w = (w - 4.0f) / (float)periods;
        float x = p0.x + 2.0f;
        float y_hi = mid_y - amp_px;
        float y_lo = mid_y + amp_px;
        int idx = 0;
        pts[idx++] = ImVec2(x, y_hi);
        for (int p = 0; p < periods; ++p)
        {
            float x_half = x + period_w * 0.5f;
            float x_end = x + period_w;
            pts[idx++] = ImVec2(x_half, y_hi); /* horní platformu doleva */
            pts[idx++] = ImVec2(x_half, y_lo); /* sestup */
            pts[idx++] = ImVec2(x_end, y_lo);  /* dolní platformu */
            if (p < periods - 1)
                pts[idx++] = ImVec2(x_end, y_hi); /* další perioda - výstup */
            else
                pts[idx++] = ImVec2(x_end, y_hi); /* uzavři poslední hranu */
            x = x_end;
        }
        dl->AddPolyline(pts, idx, wave_col, 0, 1.5f);
    }
    else /* PSG_CHTYPE_NOISE */
    {
        /* Pseudo-random pattern - deterministický seed dle kanálu+chipu (přes
         * id_suffix hash) aby pattern flickr-free mezi frame-y. Skutečné LFSR
         * z PSG core by vyžadovalo step volání = side-effect. */
        unsigned seed = 0x9E3779B1u;
        for (const char *c = id_suffix; *c; ++c)
            seed = seed * 31u + (unsigned)(unsigned char)*c;

        /* Vykreslíme jako vertikální čárky každé 2 px - vzorek `seed ^ x`. */
        const float step = 2.0f;
        for (float dx = 0.0f; dx < w - 4.0f; dx += step)
        {
            unsigned s = seed ^ (unsigned)(dx * 7.0f);
            s ^= s >> 13;
            s ^= s << 17;
            s ^= s >> 5;
            float sign = (s & 1u) ? 1.0f : -1.0f;
            float xs = p0.x + 2.0f + dx;
            float ys = mid_y + sign * amp_px;
            dl->AddLine(ImVec2(xs, mid_y), ImVec2(xs, ys), wave_col, 1.0f);
        }
        /* TODO: rozlišení WHITE vs PERIODIC noise - V1 jen prostý random. */
    }
}

/* ===================================================================== */
/* Renderování jednoho PSG chipu                                         */
/* ===================================================================== */

/**
 * @brief Vykreslí latch state sekci (CS + attn flag) jednoho PSG chipu.
 *
 * @param psg          Ukazatel na PSG instanci (NESMÍ být NULL).
 * @param table_id_str Unikátní ImGui ID prefix (např. "##psg0" / "##psg1").
 */
static void render_latch_state(const st_PSG *psg, const char *table_id_str)
{
    /* 96 = table_id_str max 63 + "_latch" 6 + \0 + rezerva. */
    char tid[96];
    snprintf(tid, sizeof(tid), "%s_latch", table_id_str);

    if (ImGui::BeginTable(tid, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Value"));
        ImGui::TableSetupColumn(_("Bin/Note"));
        ImGui::TableHeadersRow();

        unsigned cs = psg_mirror_latch_cs(psg);
        unsigned attn_flag = psg_mirror_latch_attn(psg);

        render_dec_row(_("Latched channel (CS)"), cs,
                       _("Index kanalu (0..3) ktery prijme dalsi DATA byte."));

        /* Render attn flag jako enum (TONE / ATTN) - kam smeruje dalsi DATA. */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("Next DATA byte targets"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                _("Bit [4] posledniho LATCH bytu. 1 = attn, 0 = tone (TONE kanal) / noise cfg (CH3)."));
        ImGui::TableSetColumnIndex(1);
        if (attn_flag)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", _("attenuation"));
        else
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", _("tone / noise cfg"));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", attn_flag ? 1u : 0u);

        ImGui::EndTable();
    }
}

/**
 * @brief Vykreslí stav jednoho PSG kanálu (TONE 0-2 nebo NOISE 3).
 *
 * @param psg           Ukazatel na PSG instanci (NESMÍ být NULL).
 * @param ch            Index kanálu (0..3).
 * @param table_id_str  Unikátní ImGui ID prefix (např. "##psg0_ch0").
 */
static void render_channel(const st_PSG *psg, unsigned ch, const char *table_id_str)
{
    en_PSG_CHTYPE type = psg_mirror_channel_type(psg, ch);
    unsigned attn = psg_mirror_channel_attn(psg, ch);

    /* 96 = table_id_str max 63 + "_main" 5 + \0 + rezerva. */
    char tid[96];
    snprintf(tid, sizeof(tid), "%s_main", table_id_str);

    if (ImGui::BeginTable(tid, 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn(_("Field"));
        ImGui::TableSetupColumn(_("Value"));
        ImGui::TableSetupColumn(_("Bin/Note"));
        ImGui::TableHeadersRow();

        /* Typ kanálu (TONE / NOISE - jediný kanál ktery muze byt NOISE je CH3). */
        render_enum_row(_("Channel type"), (unsigned)type, psg_chtype_names,
                        (unsigned)(sizeof(psg_chtype_names) / sizeof(psg_chtype_names[0])),
                        _("CH0-2 vzdy TONE, CH3 vzdy NOISE (initializace v psg_instance_init)."));

        /* Attenuation 0..15 + dB konverze v Bin sloupci. */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("Attenuation"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                _("4-bit volume: 0 = max (0 dB), krok -2 dB, 15 = silent (channel off)."));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", attn);
        ImGui::TableSetColumnIndex(2);
        char dbbuf[32];
        format_attn_db(attn, dbbuf, sizeof(dbbuf));
        if (attn == 0)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", dbbuf);
        else if (attn >= 15)
            ImGui::TextDisabled("%s", dbbuf);
        else
            ImGui::TextUnformatted(dbbuf);

        ImGui::EndTable();
    }

    /* TONE sekce - pro CH0-2 vzdy, pro CH3 jen pokud type==TONE. */
    if (type == PSG_CHTYPE_TONE)
    {
        uint16_t divider = psg_mirror_channel_tone_divider(psg, ch);
        double freq_hz = psg_tone_frequency_hz(divider);

        /* 96 = table_id_str max 63 + "_tone" 5 + \0 + rezerva. */
        char tid2[96];
        snprintf(tid2, sizeof(tid2), "%s_tone", table_id_str);

        if (ImGui::BeginTable(tid2, 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn(_("Field"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableSetupColumn(_("Bin/Note"));
            ImGui::TableHeadersRow();

            /* Tone divider 10-bit - dec hodnota + hex tooltip. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(_("Tone divider (10-bit)"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    _("10-bit value 0..1023. f_out = CPU_CLOCK / (32 * divider). <2 = DC."));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", (unsigned)divider);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%03X", (unsigned)divider);

            /* Frekvence Hz. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(_("Frequency"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    _("CPU_CLOCK / (32 * divider). MZ-800: 3.5469 MHz, MZ-700/MZ-1500: 3.5796 MHz CPU clock."));
            ImGui::TableSetColumnIndex(1);
            if (freq_hz <= 0.0)
                ImGui::TextDisabled("%s", _("DC (no oscillation)"));
            else if (freq_hz >= 1000.0)
                ImGui::Text("%.3f kHz", freq_hz / 1000.0);
            else
                ImGui::Text("%.2f Hz", freq_hz);
            ImGui::TableSetColumnIndex(2);
            if (freq_hz > 0.0)
                ImGui::Text("%.0f Hz", freq_hz);
            else
                ImGui::TextDisabled("-");

            /* MIDI nota. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(_("MIDI note"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    _("12 * log2(f / 440) + 69. A4 = 440 Hz = 69. Cents = odchylka od rovnomerne temperace."));
            ImGui::TableSetColumnIndex(1);
            char notebuf[32];
            format_midi_note(freq_hz, notebuf, sizeof(notebuf));
            if (freq_hz > 0.0)
                ImGui::TextUnformatted(notebuf);
            else
                ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(2);
            if (freq_hz > 0.0)
            {
                double midi = 12.0 * log2(freq_hz / 440.0) + 69.0;
                ImGui::Text("%.2f", midi);
            }
            else
                ImGui::TextDisabled("-");

            ImGui::EndTable();
        }
    }

    /* NOISE sekce - jen pokud type==NOISE (typicky CH3). */
    if (type == PSG_CHTYPE_NOISE)
    {
        en_NOISE_DIV_TYPE div_type = psg_mirror_channel_noise_div_type(psg, ch);
        en_NOISE_TYPE ntype = psg_mirror_channel_noise_type(psg, ch);

        /* 96 = table_id_str max 63 + "_noise" 6 + \0 + rezerva. */
        char tid3[96];
        snprintf(tid3, sizeof(tid3), "%s_noise", table_id_str);

        if (ImGui::BeginTable(tid3, 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn(_("Field"));
            ImGui::TableSetupColumn(_("Value"));
            ImGui::TableSetupColumn(_("Bin/Note"));
            ImGui::TableHeadersRow();

            render_enum_row(_("Noise divider type"), (unsigned)div_type,
                            psg_noise_div_type_names,
                            (unsigned)(sizeof(psg_noise_div_type_names) / sizeof(psg_noise_div_type_names[0])),
                            _("0..2 = fixed PSG dividery 16/32/64, 3 = CH2 tone divider rizeny shift."));

            render_enum_row(_("Noise feedback"), (unsigned)ntype,
                            psg_noise_type_names,
                            (unsigned)(sizeof(psg_noise_type_names) / sizeof(psg_noise_type_names[0])),
                            _("Periodic = LFSR top bit only, White = XOR feedback (sirsi spektrum)."));

            ImGui::EndTable();
        }

        ImGui::TextDisabled("%s", _("Tone divider: N/A (NOISE channel)"));
    }

    /* Per-kanál waveform vizualizace (square wave / random pattern dle typu). */
    ImGui::Spacing();
    ImGui::TextUnformatted(_("Waveform"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
            _("Oscilloscope view: TONE = square wave (4 periods fixed), NOISE = pseudo-random pattern. Amplitude scaled by attenuation."));
    render_channel_waveform(psg, ch, table_id_str);
}

/**
 * @brief Vykreslí kompletní stav jednoho PSG chipu (latch + 4 kanály).
 *
 * @param psg           Ukazatel na PSG instanci (NESMÍ být NULL).
 * @param id_prefix     Unikátní prefix pro ImGui ID (např. "psg0" / "psg1").
 */
static void render_psg_chip(const st_PSG *psg, const char *id_prefix)
{
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "##%s", id_prefix);

    /* Latch state subheader. */
    ImGui::TextUnformatted(_("Latch state"));
    render_latch_state(psg, idbuf);
    ImGui::Spacing();

    /* 4x kanál jako vnořené collapsible headery. */
    for (unsigned ch = 0; ch < PSG_CHANNELS_COUNT; ++ch)
    {
        char title[96];
        en_PSG_CHTYPE type = psg_mirror_channel_type(psg, ch);
        /* Skládáme z atomických klíčů ("Channel" + typ z psg_chtype_names) -
         * jednodušší pro překladatele než celý format pattern. ###suffix
         * zajišťuje stabilní ImGui ID napříč změnami typu kanálu. */
        const char *type_name = (type == PSG_CHTYPE_TONE)
                                    ? _(psg_chtype_names[PSG_CHTYPE_TONE])
                                    : _(psg_chtype_names[PSG_CHTYPE_NOISE]);
        snprintf(title, sizeof(title), "%s %u (%s)###%s_ch%u",
                 _("Channel"), ch, type_name, id_prefix, ch);
        if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen))
        {
            char chid[64];
            snprintf(chid, sizeof(chid), "%s_ch%u", id_prefix, ch);
            render_channel(psg, ch, chid);
            ImGui::Spacing();
        }
    }
}

/* ===================================================================== */
/* Hlavní entry point okna                                               */
/* ===================================================================== */

void imgui_psg_state_window(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("PSG SN76489 State###PSGState"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* Stereo / mono detekce dle runtime flagu (MZ-1500 default stereo,
     * MZ-800 stereo opt-in podle ROM presetu). MZ-700 PSG nemá, sem
     * se v tom buildu nedostaneme (celý soubor #if-out). */
    bool is_stereo = g_psg_module.stereo;

    if (is_stereo)
    {
        /* MZ-1500 / MZ-800 stereo - 2x collapsible. */
        if (ImGui::CollapsingHeader(_L("PSG Left (chip 0)###PSGLeftHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            render_psg_chip(&g_psg_module.psg[0], "psg0");
            ImGui::Spacing();
        }

        if (ImGui::CollapsingHeader(_L("PSG Right (chip 1)###PSGRightHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            render_psg_chip(&g_psg_module.psg[1], "psg1");
            ImGui::Spacing();
        }
    }
    else
    {
        /* MZ-800 mono - 1 PSG. */
        if (ImGui::CollapsingHeader(_L("PSG (chip 0)###PSGMonoHdr"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
            render_psg_chip(&g_psg_module.psg[0], "psg0");
        }
    }

    ImGui::End();
}

