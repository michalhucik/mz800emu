/*
 * stack_regions_window.cpp - V8 samostatne okno s tabulkou stack regionu.
 *
 * 1-radek-per-region table layout (Name / Base / Limit / SP% / Min /
 * Push / Pop / Trend / Action). Doplňuje hlavni Stack Monitor okno
 * (stack_window.cpp / dbg_stack_panel.cpp), ktere se po V8 cleanup
 * soustredi pouze na hex dump kolem SP a SP history sparkline.
 *
 * Refresh dat (regiony + history) probiha pres
 * dbg_stack_panel_frame_refresh(), ktery je sdileny s hlavnim oknem a
 * idempotentni v ramci framu. Add/Edit modaly se vyzaduji pres
 * dbg_stack_panel_request_add_modal / _request_edit_modal a vykresluji
 * se ve sdilene render funkci dbg_stack_panel_render_modals.
 *
 * Stabilni ID: "Stack Regions###stack_regions_window".
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "stack_regions_window.h"
#include "ui-imgui/debugger/sections/dbg_stack_panel.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/bootstrap/myimgui.h"

extern "C" {
#include "emulator/debugger/dbgapi_cmdrq.h"
}


/**
 * @brief V11: Pending flag pro bring-to-front request.
 *
 * Nastaven pres `stack_regions_window_request_focus()` z cross-link
 * tlacitka "Region:" v hlavnim Stack Monitor okne (V11). Konzumovan v
 * `stack_regions_window_render`:
 *
 * 1. PRED `Begin` (pokud true): `ImGui::SetNextWindowFocus()` priradi
 *    okno do popredi v ImGui-internal z-orderu.
 * 2. PO `Begin` (V9.3 pattern): pokud je okno v separate platform viewport,
 *    explicitne zavola `Platform_SetWindowFocus(viewport)` = SDL_RaiseWindow.
 *
 * Stejny dual-step pattern jako `stack_window.cpp`/`stack_history_window.cpp`
 * - viz tam pro detail root-cause (V9.3 multi-viewport platform raise).
 */
static bool s_focus_pending = false;


extern "C" void stack_regions_window_request_focus ( void ) {
    s_focus_pending = true;
}


/**
 * @brief Vykresli sticky header row Stack Regions okna.
 *
 * Obsahuje pouze tlacitko "+ Add region from current SP" - dalsi sticky
 * info (SP, Depth, recording flag) je v hlavnim Stack Monitor okne, ne
 * v tomto.
 */
static void stack_regions_window_header(void)
{
    if (ImGui::Button(_L("+ Add region from current SP###stack_reg_win_add"))) {
        dbg_stack_panel_request_add_modal();
    };
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
            _("Define new stack region with base = current SP, "
              "limit = base - 256."));
    };
}


/**
 * @brief Vykresli tabulku regionu 1-radek-per-region.
 *
 * Sloupce:
 *   Name   - jmeno regionu (Selectable = vyber = sync s dropdownem
 *            v hlavnim Stack Monitor okne)
 *   Base   - bazni adresa hex
 *   Limit  - dolni mez hex
 *   SP%    - depth indicator (0-100%, jen kdyz SP padne do regionu)
 *   Min    - watermark hex (nejnizsi pozorovany SP)
 *   Push   - u64 counter operaci PUSH/CALL/RST
 *   Pop    - u64 counter operaci POP/RET
 *   Trend  - mini sparkline SP samplu filtrovanych na rozsah regionu
 *   Act    - [E] Edit / [R] Reset watermark / [X] Delete
 *
 * Pri prazdnem seznamu vypise hint "(no regions)".
 *
 * Akce jsou deferred (= modifikace seznamu po opusteni for loopu) aby se
 * predeslo invalidaci indexu pri compact v emu vlaknu.
 */
static void stack_regions_window_table(void)
{
    int count = dbg_stack_panel_regions_count();
    if (count == 0) {
        ImGui::TextDisabled("%s",
            _("(no regions, click [+] above to add)"));
        return;
    };

    /* Trend cell vyska = jeden radek textu + drobny padding. PlotLines
     * v teto vysce je citelny i pri 1 row table layoutu. */
    const float trend_h = ImGui::GetTextLineHeight() + 4.0f;
    /* Sirka Trend cellu - empiricky. PlotLines self-fit do bunky podle
     * column resize. */
    const float trend_w = ImGui::CalcTextSize("FFFFFFFFFFFF").x;

    ImGuiTableFlags tflags = ImGuiTableFlags_Borders
                            | ImGuiTableFlags_Resizable
                            | ImGuiTableFlags_RowBg
                            | ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("###stack_regions_table", 9, tflags)) {
        return;
    };

    ImGui::TableSetupColumn(_L("Name###stack_reg_col_name"),
                            ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(_L("Base###stack_reg_col_base"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("FFFF").x + 16.0f);
    ImGui::TableSetupColumn(_L("Limit###stack_reg_col_limit"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("FFFF").x + 16.0f);
    ImGui::TableSetupColumn(_L("SP%###stack_reg_col_pct"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("100%").x + 16.0f);
    ImGui::TableSetupColumn(_L("Min###stack_reg_col_min"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("FFFF").x + 16.0f);
    ImGui::TableSetupColumn(_L("Push###stack_reg_col_push"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("999999").x + 16.0f);
    ImGui::TableSetupColumn(_L("Pop###stack_reg_col_pop"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("999999").x + 16.0f);
    ImGui::TableSetupColumn(_L("Trend###stack_reg_col_trend"),
                            ImGuiTableColumnFlags_WidthFixed, trend_w);
    ImGui::TableSetupColumn(_L("Act###stack_reg_col_act"),
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("EE RR XX").x + 24.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    uint16_t sp = dbg_stack_panel_sp_now();
    int selected = dbg_stack_panel_get_selected();

    /* Deferred akce - shromazdime pres for, aplikujeme po EndTable, aby
     * pripadny compact / refresh nezmenil indexy uvnitr iterace. */
    int request_edit   = -1;
    int request_reset  = -1;
    int request_remove = -1;

    for (int i = 0; i < count; i++) {
        const st_DBGAPI_STACK_REGION_INFO *r =
            (const st_DBGAPI_STACK_REGION_INFO *)dbg_stack_panel_get_region(i);
        if (!r) continue;

        bool is_sel = (selected == i);

        ImGui::TableNextRow();
        ImGui::PushID(i);

        /* Name - Selectable s SpanAllColumns + AllowOverlap pro klik a
         * hover detekci napric vsemi 9 sloupci, ALE vlastni Selectable
         * highlight je potlacen pres PushStyleColor(ImGuiCol_Header*) na
         * transparent. Misto Selectable highlight nasleduje rucni
         * AddRectFilled jen pres prvnich 8 sloupcu (Name..Trend), Act
         * sloupec zustane bez highlight.
         *
         * Duvod: V8.2 zkusila Variant C (TableSetBgColor CellBg
         * transparent v Act cellu), ale Selectable s SpanAllColumns
         * kresli highlight do table BG channel (TablePushBackgroundChannel
         * v Selectable() implementaci, viz imgui_widgets.cpp), coz je
         * NAD CellBg ale POD cell foreground content. Cell bg transparent
         * tedy highlight neprekryje.
         *
         * Reseni V8.3 (Variant A z brieffu): potlacit Selectable vlastni
         * highlight pres styl, vykreslit vlastni rect rucne pres 8
         * sloupcu z foreground channel cellu 0 (= az po TablePopBg
         * channel uvnitr Selectable(), pred RenderTextClipped labelu).
         *
         * Selectable label je prazdny "##id", aby se neprekryval s
         * nasledujicim ImGui::Text(name); ten kreslime az po
         * AddRectFilled (= text nad rect).
         *
         * AllowOverlap je POVINNY: bez nej Selectable spolkne click drive
         * nez SmallButton [E]/[R]/[X] v Act sloupci (zachovani V8.1 fix). */
        ImGui::TableSetColumnIndex(0);

        ImVec2 cell0_cursor = ImGui::GetCursorScreenPos();

        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));

        char sel_id[32];
        snprintf(sel_id, sizeof(sel_id), "##stack_reg_row_%d", i);
        bool clicked = ImGui::Selectable(sel_id, is_sel,
                                          ImGuiSelectableFlags_SpanAllColumns
                                              | ImGuiSelectableFlags_AllowOverlap);

        ImGui::PopStyleColor(3);

        bool row_hovered = ImGui::IsItemHovered();

        if (clicked) {
            dbg_stack_panel_set_selected(i);
            /* V8.2: otevri Stack Monitor pokud zavreny. */
            if (g_gui && !g_gui->showStackWindow) {
                g_gui->showStackWindow = true;
            };
        };

        /* V8.3 tooltip a highlight scope omezujeme rucne na X rozsah
         * cells 0-7. Selectable s SpanAllColumns ma hitbox pres vsech 9
         * sloupcu, takze IsItemHovered vraci true i pri hover nad Act
         * sloupcem. Misto Selectable hover testu pouzijeme manualni
         * MousePos vs (cell0_left, act_left) interval test - to oddeli
         * scope tooltipu i highlight rectu od Act sloupce zcela
         * deterministicky (bez zavislosti na SmallButton "ukradeni"
         * hover prioritou). */
        ImVec2 sel_min = ImGui::GetItemRectMin();
        ImVec2 sel_max = ImGui::GetItemRectMax();

        /* Cell 8 left edge zjistime docasnym prepnutim do Act sloupce. */
        ImGui::TableSetColumnIndex(8);
        float act_left_x = ImGui::GetCursorScreenPos().x;
        ImGui::TableSetColumnIndex(0);

        /* Highlight + tooltip jen kdyz hover/sel a kurzor je v X rozsahu
         * cells 0..7. Selectable IsItemHovered() vraci true pro cely
         * SpanAllColumns rect - omezime rucne. */
        bool in_cells_0_7 = false;
        if (row_hovered) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            in_cells_0_7 = (mp.x >= cell0_cursor.x && mp.x < act_left_x);
        };

        if (in_cells_0_7 || is_sel) {
            ImU32 col;
            if (is_sel && in_cells_0_7) {
                col = ImGui::GetColorU32(ImGuiCol_HeaderActive);
            } else if (is_sel) {
                col = ImGui::GetColorU32(ImGuiCol_Header);
            } else {
                col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
            };
            /* Y rozsah z Selectable bb (= cely radek), X od cell 0 left k
             * Act left. */
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cell0_cursor.x, sel_min.y),
                ImVec2(act_left_x, sel_max.y),
                col);
        };

        if (in_cells_0_7) {
            ImGui::SetTooltip("%s", _("Show region in Stack Monitor"));
        };

        /* Name text - kresleny po highlight rect = nad nim Z-order. */
        ImGui::SetCursorScreenPos(cell0_cursor);
        ImGui::TextUnformatted(r->name);

        /* Base. */
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%04X", r->base);

        /* Limit. */
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%04X", r->limit);

        /* SP% - jen kdyz SP padne do regionu, jinak placeholder. */
        ImGui::TableSetColumnIndex(3);
        if (sp >= r->limit && sp <= r->base && r->base > r->limit) {
            int range = (int)r->base - (int)r->limit;
            int used  = (int)r->base - (int)sp;
            int pct   = (range > 0) ? (used * 100 / range) : 0;
            ImGui::Text("%d%%", pct);
        } else {
            ImGui::TextDisabled("--");
        };

        /* Min (= watermark). */
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%04X", r->watermark);

        /* Push counter. */
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%llu", (unsigned long long)r->push_count);

        /* Pop counter. */
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%llu", (unsigned long long)r->pop_count);

        /* Trend mini sparkline - shared helper z dbg_stack_panel. */
        ImGui::TableSetColumnIndex(7);
        dbg_stack_panel_render_region_trend(i, trend_w - 8.0f, trend_h);

        /* Action [E][R][X].
         *
         * V8.3: Selectable vlastni highlight uz je potlaceny stylem
         * (transparent), takze TableSetBgColor CellBg neni potreba.
         * Highlight rect kresleny rucne v Cell 0 nedosahuje do Act
         * sloupce (= konci na act_left_x). */
        ImGui::TableSetColumnIndex(8);
        if (ImGui::SmallButton(_L("E###stack_reg_btn_edit"))) {
            request_edit = i;
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _("Edit region name/base/limit"));
        };
        ImGui::SameLine();
        if (ImGui::SmallButton(_L("R###stack_reg_btn_reset"))) {
            request_reset = i;
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _("Reset watermark + counters"));
        };
        ImGui::SameLine();
        if (ImGui::SmallButton(_L("X###stack_reg_btn_del"))) {
            request_remove = i;
        };
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _("Delete region"));
        };

        ImGui::PopID();
    };

    ImGui::EndTable();

    /* Provedeni odlozenych akci - bezpecne mimo iteraci. */
    if (request_edit >= 0) {
        dbg_stack_panel_request_edit_modal(request_edit);
    };
    if (request_reset >= 0) {
        dbg_stack_panel_request_reset_region(request_reset);
    };
    if (request_remove >= 0) {
        dbg_stack_panel_request_remove_region(request_remove);
    };
}


/**
 * @brief Render top-level Stack Regions okna.
 *
 * Stejny pattern jako stack_window_render / cpu_window_render. Pred
 * vlastnim renderem vola dbg_disassembled_frame_init (= per-frame guarded
 * init g_dbg_ui + dbg_refresh_tick), aby okno fungovalo i bez hlavniho
 * debug okna.
 *
 * Refresh dat pres dbg_stack_panel_frame_refresh - idempotentni v ramci
 * framu (= soucasne otevreni hlavniho Stack Monitor okna nezpusobi
 * double CMDRQ).
 *
 * @param p_open Pointer na bool s viditelnosti. NULL nebo false = no-op.
 */
extern "C" void stack_regions_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    /* Per-frame init refresh kontroleru. */
    dbg_disassembled_frame_init ( );

    /* Default size pri prvnim pouziti - dostatecne siroke aby se vesly
     * vsechny sloupce table (Name stretch + 8 fixed kolem 60-100 px kazdy).
     * User-modified velikost si ImGui zapamatuje pres ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 900.0f, 380.0f ),
                                ImGuiCond_FirstUseEver );

    /* V11: Konzumace pending focus requestu (z cross-link "Region:" tlacitka
     * v hlavnim Stack Monitor okne). Dual-step pattern - viz V9.3 popis v
     * stack_window.cpp. */
    bool was_pending = s_focus_pending;
    if ( s_focus_pending ) {
        ImGui::SetNextWindowFocus ( );
        s_focus_pending = false;
    };

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;

    /* Stabilni ID pres ###. */
    if ( !ImGui::Begin ( _L ( "Stack Regions###stack_regions_window" ),
                          p_open, wflags ) ) {
        ImGui::End ( );
        return;
    };

    /* V11: Multi-viewport platform raise (V9.3 pattern). */
    if ( was_pending ) {
        ImGuiViewport *vp = ImGui::GetWindowViewport ( );
        ImGuiViewport *main_vp = ImGui::GetMainViewport ( );
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO ( );
        if ( vp && vp != main_vp && vp->PlatformWindowCreated
             && pio.Platform_SetWindowFocus ) {
            pio.Platform_SetWindowFocus ( vp );
        };
    };

    /* Refresh dat (sdileny s hlavnim Stack Monitor oknem - idempotentni
     * per frame). */
    dbg_stack_panel_frame_refresh ( );

    /* Sticky header - + Add region. */
    stack_regions_window_header ( );
    ImGui::Separator ( );

    /* Hlavni tabulka regionu. */
    stack_regions_window_table ( );

    /* Modaly Add/Edit - delegovano na sdilenou render funkci dbg_stack_panel.
     * Volat kazdy frame, ImGui ho zobrazi jen pokud byl OpenPopup proveden. */
    dbg_stack_panel_render_modals ( );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
