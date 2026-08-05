/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file plotter_window.cpp
 * @brief GUI okno plotteru Sharp MZ-1P16 - implementace.
 *
 * ImGui okno se živým náhledem kresby plotteru. Náhled vykresluje stroke
 * buffer @ref g_mz1p16 přes ImGui DrawList (AddLine, tloušťka 2 px, žádné
 * tečky - čisté linie jako reálný plotter). Souřadnice v krocích motorů se
 * mapují na pixely přes uživatelsky volitelný zoom + scroll uvnitř child
 * regionu (Y roste dolů, jako papír vyjíždějící z plotteru).
 *
 * Barvy karuselu (index 0-3): vizuální pořadí self-testu je černá, modrá,
 * zelená, červená, ale spatial index sekvence je 0,3,2,1 - proto je
 * case 1=červená, 2=zelená, 3=modrá.
 *
 * Toolbar 1. řada: Save (export do PNG + file dialog), Clear paper (smaže
 * kresbu - jediná akce, která maže papír; RESET ho NEmaže).
 * 2. řada (fyzická tlačítka plotteru): Reset (mz1p16_emu_reset, papír zachová),
 * Paper feed (momentální - posunuje papír dokud drženo, T1=0) a Run drawing
 * self-test. Zoom slider (px/krok) řídí měřítko náhledu. Statusbar: X, Y,
 * aktivní barva (swatch), pero up/down, BUSY (modelovaný host-ready) a počet
 * úseček (strokes).
 *
 * Vztah okno <-> plotter: plotter je aktivní jen když je okno otevřené
 * (zavřené okno = plotter odpojený). Při přechodu zavřeno->otevřeno se volá
 * mz1p16_emu_set_active(true), při zavření false.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <glib.h>

#include "i18n.h"

#include "ui-imgui/plotter/plotter_window.h"

#include "emulator/hw-generic/mz1p16/mz1p16.h"
#include "emulator/hw-generic/mz1p16/mz1p16_emu.h"
#include "emulator/debugger/png_encode.h"
#include "baseui/baseui_filechooser.h"
#include "emulator/mzarch/mzhal.h"

extern "C"
{
    void imgui_plotter_window(bool *p_open);
}

/* --- Stav okna (UI-only) --- */

/** Přibližovací faktor náhledu (px na krok motoru). */
static float s_zoom = 1.5f;

/** true = v náhledu kreslit živý marker aktuální pozice vozíku/pera. */
static bool s_show_carriage = true;

/** true = uživatel drží toggle Paper feed (T1=0). */
static bool s_paper_feed_held = false;

/**
 * @brief Mapuje index barvy karuselu na RGB (ImU32).
 *
 * Vizuální pořadí self-testu je černá, modrá, zelená, červená, ale spatial
 * index sekvence je 0,3,2,1 - proto se index 1 kreslí červeně, 2 zeleně,
 * 3 modře.
 *
 * @param idx Index barvy 0-3 (vyšší bity ignorovány).
 * @return Barva pera jako ImU32 (RGBA, alpha 255).
 */
static ImU32 plotter_color_u32(uint8_t idx)
{
    switch (idx & 3)
    {
    case 0: return IM_COL32(20, 20, 20, 255);   /* černá (tmavě šedá pro viditelnost) */
    case 1: return IM_COL32(220, 50, 50, 255);  /* červená */
    case 2: return IM_COL32(30, 170, 60, 255);  /* zelená */
    case 3: return IM_COL32(40, 90, 220, 255);  /* modrá */
    default: return IM_COL32(0, 0, 0, 255);
    }
}

/** @brief Vrátí lidsky čitelný název aktivní barvy pera. */
static const char *plotter_color_name(uint8_t idx)
{
    switch (idx & 3)
    {
    case 0: return _("Black");
    case 1: return _("Red");
    case 2: return _("Green");
    case 3: return _("Blue");
    default: return "?";
    }
}

/**
 * @brief Vykreslí náhled kresby (stroke buffer) do child regionu přes DrawList.
 *
 * Každý zaznamenaný tah @ref st_MZ1P16_Stroke (úsečka x0,y0 -> x1,y1 v krocích
 * motorů) se vykreslí jako jedna čára (AddLine, tloušťka 2 px) v barvě tahu.
 * Žádné izolované tečky. Spojitost tahů řeší už jádro (mz1p16.c record_stroke:
 * úsečku zaznamená jen při pohybu se spuštěným perem, takže navazující kroky
 * tvoří souvislou linii; při zvednutém peru se neukládá = žádné skoky/přelety).
 * Okno tedy jen vykresluje hotové úsečky.
 *
 * Papír má PEVNOU šířku (MZ1P16_PAPER_WIDTH_STEPS, dojezd vozíku) a kreslí se
 * v absolutních souřadnicích zleva doprava od X=0 (levý doraz = levý okraj).
 * ŽÁDNÉ vystřeďování. Výška plátna roste s kresbou (papír = role, Y roste dolů).
 * Plátno větší než viditelná plocha = vodorovný/svislý scrollbar.
 */
static void plotter_render_preview()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 16.0f) avail.x = 16.0f;
    if (avail.y < 16.0f) avail.y = 16.0f;

    const float margin = 8.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    uint32_t n = g_mz1p16.n_strokes;

    /* Bounding box tahů (kroky). Prázdná kresba => jen prázdné plátno. */
    int32_t minx = 0, maxx = 0, miny = 0, maxy = 0;
    if (n > 0)
    {
        minx = maxx = g_mz1p16.strokes[0].x0;
        miny = maxy = g_mz1p16.strokes[0].y0;
        for (uint32_t i = 0; i < n; i++)
        {
            const st_MZ1P16_Stroke *s = &g_mz1p16.strokes[i];
            int32_t xs[2] = { s->x0, s->x1 };
            int32_t ys[2] = { s->y0, s->y1 };
            for (int k = 0; k < 2; k++)
            {
                if (xs[k] < minx) minx = xs[k];
                if (xs[k] > maxx) maxx = xs[k];
                if (ys[k] < miny) miny = ys[k];
                if (ys[k] > maxy) maxy = ys[k];
            }
        }
    }

    /* X/levý okraj se NEodvozuje z bboxu - papír má pevný levý okraj (X=0). */
    (void)minx; (void)maxx;

    /* PEVNÝ PAPÍR (role): pevná ŠÍŘKA (dojezd vozíku), absolutní souřadnice
     * zleva od X=0 (levý doraz), bez vystřeďování. VÝŠKA roste jako role papíru:
     * dolů s tiskem/paper feed, NAHORU když Y jde do záporu - vždy s rezervou.
     * Y rozsah bereme z tahů (miny/maxy) I z AKTUÁLNÍ pozice vozíku (sy.pos),
     * protože paper feed posouvá papír i bez kresby. Plátno větší než okno =
     * scrollbary. */
    const int32_t RESERVE = 8;   /* kroků rezerva nahoře i dole */
    int32_t cy = g_mz1p16.sy.pos;
    int32_t lo = (n > 0) ? miny : 0;
    int32_t hi = (n > 0) ? maxy : 0;
    if (cy < lo) lo = cy;
    if (cy > hi) hi = cy;
    if (lo > 0) lo = 0;          /* horní okraj papíru je minimálně Y=0 */
    if (hi < 0) hi = 0;
    int32_t top_y = lo - RESERVE;
    int32_t bot_y = hi + RESERVE;

    const float paper_w_steps = MZ1P16_PAPER_WIDTH_STEPS;
    float canvas_w = paper_w_steps * s_zoom + 2.0f * margin;
    float draw_h   = (float)(bot_y - top_y) * s_zoom + 2.0f * margin;
    float canvas_h = (draw_h > avail.y) ? draw_h : avail.y;

    /* Origin = aktuální kurzor (respektuje scroll child regionu). */
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(origin.x + canvas_w, origin.y + canvas_h);

    /* Bílý papír + rámeček (jasně ukazuje rozměr papíru). */
    dl->AddRectFilled(origin, p1, IM_COL32(255, 255, 255, 255));
    dl->AddRect(origin, p1, IM_COL32(160, 160, 160, 255));

    if (n > 0)
    {
        /* Absolutní souřadnice: X=0 vlevo (bez vystředění). Y posunuté tak, aby
         * top_y bylo na horním okraji kreslicí plochy (záporné Y se vejdou). */
        float ox = origin.x + margin;
        float oy = origin.y + margin - (float)top_y * s_zoom;

        dl->PushClipRect(origin, p1, true);
        for (uint32_t i = 0; i < n; i++)
        {
            const st_MZ1P16_Stroke *s = &g_mz1p16.strokes[i];
            ImVec2 a(ox + (float)s->x0 * s_zoom, oy + (float)s->y0 * s_zoom);
            ImVec2 b(ox + (float)s->x1 * s_zoom, oy + (float)s->y1 * s_zoom);
            dl->AddLine(a, b, plotter_color_u32(s->color), 2.0f);
        }
        dl->PopClipRect();
    }

    /* Živý marker aktuální pozice vozíku/pera (volitelný, checkbox). Křížek +
     * kolečko; pero dole = vyplněné červené, nahoře = duté modré. */
    if (s_show_carriage && g_mz1p16.active)
    {
        float mx = origin.x + margin + (float)g_mz1p16.sx.pos * s_zoom;
        float my = origin.y + margin + (float)(g_mz1p16.sy.pos - top_y) * s_zoom;
        bool down = g_mz1p16.pen_down;
        ImU32 col = down ? IM_COL32(220, 40, 40, 255) : IM_COL32(50, 110, 220, 255);
        const float r = 5.0f;
        dl->PushClipRect(origin, p1, true);
        dl->AddLine(ImVec2(mx - r - 2.0f, my), ImVec2(mx + r + 2.0f, my), col, 1.5f);
        dl->AddLine(ImVec2(mx, my - r - 2.0f), ImVec2(mx, my + r + 2.0f), col, 1.5f);
        if (down)
            dl->AddCircleFilled(ImVec2(mx, my), r, col, 12);
        else
            dl->AddCircle(ImVec2(mx, my), r, col, 12, 1.5f);
        dl->PopClipRect();
    }

    /* InvisibleButton velikosti plátna - child region pak nabídne H i V
     * scrollbar pro papír větší než viditelná plocha. */
    ImGui::InvisibleButton("##plotter_paper", ImVec2(canvas_w, canvas_h));

    /* Při Paper feed (papír vyjíždí) automaticky scrolluj dolů, ať je vidět
     * spodní okraj role, jak roste. */
    if (s_paper_feed_held)
    {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
}

/**
 * @brief Vyrenderuje kresbu do RGBA bufferu (pro PNG export).
 *
 * @param out_w  OUT: šířka výsledku (px).
 * @param out_h  OUT: výška výsledku (px).
 * @return Nově alokovaný RGBA buffer (w*h*4), nebo NULL (prázdná kresba /
 *         chyba alokace). Volající uvolní přes g_free.
 */
static uint8_t *plotter_render_to_rgba(uint32_t *out_w, uint32_t *out_h)
{
    uint32_t n = g_mz1p16.n_strokes;
    if (n == 0) return NULL;

    int32_t minx = g_mz1p16.strokes[0].x0, maxx = g_mz1p16.strokes[0].x0;
    int32_t miny = g_mz1p16.strokes[0].y0, maxy = g_mz1p16.strokes[0].y0;
    for (uint32_t i = 0; i < n; i++)
    {
        const st_MZ1P16_Stroke *s = &g_mz1p16.strokes[i];
        int32_t xs[2] = { s->x0, s->x1 };
        int32_t ys[2] = { s->y0, s->y1 };
        for (int k = 0; k < 2; k++)
        {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }

    const float scale = 3.0f;   /* px na krok */
    const int margin = 12;
    int span_x = maxx - minx + 1, span_y = maxy - miny + 1;
    int W = (int)(span_x * scale) + 2 * margin;
    int H = (int)(span_y * scale) + 2 * margin;
    if (W < 32) W = 32;
    if (H < 32) H = 32;

    uint8_t *rgba = (uint8_t *)g_malloc0((size_t)W * H * 4);
    if (!rgba) return NULL;
    /* Papírové pozadí (světle šedé). */
    for (size_t i = 0; i < (size_t)W * H; i++)
    {
        rgba[i * 4 + 0] = 0xF6;
        rgba[i * 4 + 1] = 0xF6;
        rgba[i * 4 + 2] = 0xF6;
        rgba[i * 4 + 3] = 0xFF;
    }

    /* Bresenham úsečka s tloušťkou 2 px (brush 2x2), barvy dle indexu. */
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t *p = rgba + ((size_t)y * W + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 0xFF;
    };
    auto line = [&](int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;)
        {
            put(x0, y0, r, g, b);
            put(x0 + 1, y0, r, g, b);
            put(x0, y0 + 1, r, g, b);
            put(x0 + 1, y0 + 1, r, g, b);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    };

    for (uint32_t i = 0; i < n; i++)
    {
        const st_MZ1P16_Stroke *s = &g_mz1p16.strokes[i];
        ImU32 c = plotter_color_u32(s->color);
        uint8_t r = (uint8_t)(c & 0xFF);
        uint8_t g = (uint8_t)((c >> 8) & 0xFF);
        uint8_t b = (uint8_t)((c >> 16) & 0xFF);
        int ax = margin + (int)((s->x0 - minx) * scale);
        int ay = margin + (int)((s->y0 - miny) * scale);
        int bx = margin + (int)((s->x1 - minx) * scale);
        int by = margin + (int)((s->y1 - miny) * scale);
        line(ax, ay, bx, by, r, g, b);
    }

    *out_w = (uint32_t)W;
    *out_h = (uint32_t)H;
    return rgba;
}

/** @brief Callback file dialogu pro Save - zapíše PNG na zvolenou cestu. */
static void plotter_save_png_cb(baseui_fchooser_t *fch)
{
    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK || !fch->selected_filePathName)
    {
        baseui_filechooser_destroy(fch);
        return;
    }

    uint32_t w = 0, h = 0;
    uint8_t *rgba = plotter_render_to_rgba(&w, &h);
    if (rgba)
    {
        size_t png_len = 0;
        uint8_t *png = png_encode_rgba(rgba, w, h, &png_len);
        if (png && png_len > 0)
        {
            FILE *f = fopen(fch->selected_filePathName, "wb");
            if (f)
            {
                fwrite(png, 1, png_len, f);
                fclose(f);
            }
        }
        if (png) g_free(png);
        g_free(rgba);
    }

    baseui_filechooser_destroy(fch);
}

/** @brief Otevře Save file dialog pro export kresby do PNG. */
static void plotter_save_dialog()
{
    baseui_fchooser_t *fch = baseui_filechooser_save_file(
        _("Save plotter drawing"), ".png", NULL, _("plotter.png"), NULL,
        plotter_save_png_cb, NULL);
    if (!fch)
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
}


void imgui_plotter_window(bool *p_open)
{
    /* Plotter vyzaduje Z80 PIO - na MZ-700 je okno no-op (menu ho
     * nenabizi, runtime gate dle g_mzhal). */
    if (!g_mzhal.have_pioz80) {
        (void)p_open;
        return;
    }
    /* Okno je JEDINÁ autorita nad aktivací plotteru: kdykoliv se viditelnost
     * okna a stav aktivace jádra rozejdou, sesynchronizuj je. Pokrývá to i
     * počáteční rozjezd (INI [MZ1P16] active / CLI --mz1p16 mohou nastavit
     * g_mz1p16.active=true bez otevřeného okna - bez této synchronizace by
     * plotter zůstal "připojený" navzdory zavřenému oknu). set_active dělá
     * drahý homing jen na náběžné hraně, takže se volá pouze při skutečné
     * změně (open_now != aktuální stav). */
    bool open_now = (p_open == NULL) || *p_open;
    if (open_now != mz1p16_emu_get_active())
        mz1p16_emu_set_active(open_now);

    if (!open_now)
        return;

    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(_L("Plotter MZ-1P16"), p_open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        /* Okno collapsed (přes title bar) NEpočítáme jako zavřené - active
         * se mění jen podle *p_open (hrana výše). */
        return;
    }

    /* === Toolbar, 1. řada: Save + Clear paper === */
    if (ImGui::Button(_L("Save...")))
        plotter_save_dialog();
    ImGui::SameLine();
    /* Clear paper = vymaže kresbu A zároveň resetuje plotter (nový čistý papír
     * + vozík na home). Samotný RESET papír NEmaže. */
    if (ImGui::Button(_L("Clear paper")))
    {
        mz1p16_emu_reset();
        mz1p16_emu_clear_paper();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Clear the drawing and reset the plotter (plain RESET keeps the paper)"));

    /* === 2. řada: fyzická tlačítka plotteru (Reset, Paper feed) + self-test === */
    ImGui::TextUnformatted(_("Plotter buttons:"));
    ImGui::SameLine();

    /* RESET = reset jádra 8050 + mechaniky. Papír (kresbu) NEmaže. */
    if (ImGui::Button(_L("Reset")))
        mz1p16_emu_reset();
    ImGui::SameLine();

    /* Paper feed = momentální tlačítko: posunuje papír, dokud je drženo
     * (T1=0 aktivní LOW po dobu stisku). */
    ImGui::Button(_L("Paper feed"));
    {
        bool feed_held = ImGui::IsItemActive();
        if (feed_held != s_paper_feed_held)
        {
            s_paper_feed_held = feed_held;
            mz1p16_emu_button_paper_feed(feed_held);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Hold to feed paper"));
    ImGui::SameLine();

    if (ImGui::Button(_L("Run drawing self-test")))
    {
        /* HW postup: PAPER FEED držet + RESET -> charset 4 barvy. */
        mz1p16_emu_run_drawing_selftest();
        /* Po self-testu uvolníme PAPER FEED, ať se další reset chová normálně. */
        s_paper_feed_held = false;
        mz1p16_emu_button_paper_feed(false);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Reset with PAPER FEED held -> writes the character set in 4 colors"));

    /* Zoom slider (px na 1 krok motoru). Počet úseček (strokes) sem nepatří -
     * je to info o kresbě, ne parametr zobrazení; přesunuto do statusbaru. */
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat(_L("Zoom"), &s_zoom, 0.1f, 3.0f, "%.2f px/step");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Preview scale: pixels per motor step"));

    ImGui::SameLine();
    ImGui::Checkbox(_L("Show carriage"), &s_show_carriage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _("Live marker at the current pen/carriage position"));

    /* === Náhled papíru === (child region, vyplní zbytek nad statusbarem) */
    float status_h = ImGui::GetFrameHeightWithSpacing();
    ImVec2 child_sz = ImGui::GetContentRegionAvail();
    child_sz.y -= status_h;
    if (child_sz.y < 32.0f) child_sz.y = 32.0f;

    /* Serializace čtení plotter stavu (stroke buffer + statusbar pole) proti
     * emu vláknu, které jádro 8050 současně krokuje (per-frame catch-up,
     * pioz80 handshake). Akční tlačítka výše zamykají uvnitř - tady jen čteme,
     * takže držíme jeden render-lock kolem celého čtecího bloku. POZOR: žádné
     * vnoření - render NEvolá zamykající mz1p16_emu_* funkce. */
    mz1p16_emu_lock();

    if (ImGui::BeginChild("##plotter_canvas", child_sz, false,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        plotter_render_preview();
    }
    ImGui::EndChild();

    /* === Statusbar === */
    ImGui::Separator();
    ImGui::Text(_("X: %ld"), (long)g_mz1p16.sx.pos);
    ImGui::SameLine();
    ImGui::Text(_("Y: %ld"), (long)g_mz1p16.sy.pos);
    ImGui::SameLine();

    /* Barevný swatch + název. */
    ImGui::TextUnformatted(_("Color:"));
    ImGui::SameLine();
    ImVec2 sw_p = ImGui::GetCursorScreenPos();
    float sw = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(
        sw_p, ImVec2(sw_p.x + sw, sw_p.y + sw), plotter_color_u32(g_mz1p16.color));
    ImGui::GetWindowDrawList()->AddRect(
        sw_p, ImVec2(sw_p.x + sw, sw_p.y + sw), IM_COL32(80, 80, 80, 255));
    ImGui::Dummy(ImVec2(sw, sw));
    ImGui::SameLine();
    ImGui::TextUnformatted(plotter_color_name(g_mz1p16.color));
    ImGui::SameLine();

    ImGui::Text(_("Pen: %s"), g_mz1p16.pen_down ? _("DOWN") : _("UP"));
    ImGui::SameLine();
    /* BUSY = stavový výstup plotteru (P1.4), přímo. */
    if (g_mz1p16.busy)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        ImGui::TextUnformatted(_("BUSY"));
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("%s", _("idle"));
    }
    ImGui::SameLine();

    /* Info o kresbě: počet úseček (strokes) + případně zahozené (buffer plný). */
    ImGui::Text(_("Strokes: %u"), g_mz1p16.n_strokes);
    if (g_mz1p16.dropped > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(_("(dropped %u)"), g_mz1p16.dropped);
    }

    /* Konec čtecího bloku plotter stavu. */
    mz1p16_emu_unlock();

    ImGui::End();
}
