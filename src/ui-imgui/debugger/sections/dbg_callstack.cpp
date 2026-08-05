/*
 * dbg_callstack.cpp - implementace Callstack panelu (mutant callstack, Fáze 2A).
 *
 * Renderuje shadow stack jako sloupcovou tabulku (#/Ret/Call/Target/Sym/
 * Kind/Cyc-in), nad ní toolbar (Active toggle + Reset + filtry), pod ní
 * status footer (divergence/sp-swap/overflow counters).
 *
 * Refresh: g_dbg_ui.refresh.should_refresh - panel volá dbgapi
 * (CMD_GET_CALLSTACK) jen kdyz je tick. Snapshot pattern: handler
 * v emu vlákně alokuje pole, UI ho zkopíruje do local cache (
 * panel_state.entries Vector) a uvolní pres callstack_snapshot_free.
 *
 * Symbol resolution: per-frame na render time pres sym_db_lookup_by_addr
 * pro každý entry.target_addr. Žádné cachování (V1 = jednoduchost,
 * V1.5 case-by-case optimalizace).
 *
 * Self-rate-limit: pokud CMDRQ fronta plná (emu vlakno blokovano,
 * napr. CMT/FileBrowser), skipuje request a zachova cache.
 *
 * Threading: pouze UI vlákno. Pristup ke g_cs je single-threaded.
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
#include <string.h>
#include <stdint.h>
#include <vector>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "dbg_callstack.h"
#include "ui-imgui/debugger/debugger_state.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"

#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/bootstrap/myimgui.h"

extern "C" {
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/callstack.h"
#include "emulator/debugger/bookmarks/bookmarks.h"
#include "emulator/debugger/symbols/sym_db.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
}
#include "emulator/mzarch/mzhal.h"

/* g_dbgapi_cmdrq_queue je definovany v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/* ========================================================================= */
/*  Panel state                                                              */
/* ========================================================================= */

namespace {

/**
 * @brief Persistent panel state mezi framy.
 *
 * Cache poslední odpovědi dbgapi + UI preference. Single-thread access
 * (jen UI vlákno).
 *
 * @invariant valid == false na startu, true až po prvním úspěšném
 *            CMD_GET_CALLSTACK.
 * @invariant entries.size() == count (= synchronizováno při refresh).
 */
struct CallstackPanelState {
    /* Lokální kopie entries (= bezpečné držet mezi framy, callee
     * snapshot je uvolněn ihned po kopii). */
    std::vector< st_CALLSTACK_ENTRY > entries;

    /* Stats z posledniho refresh. */
    int      current_depth     = 0;
    int      max_depth_reached = 0;
    uint32_t divergence_count  = 0;
    uint32_t diverg_trampoline = 0;
    uint32_t diverg_longjmp    = 0;
    uint32_t diverg_mismatch   = 0;
    uint32_t sp_swap_count     = 0;
    uint32_t overflow_count    = 0;
    uint32_t stack_discard_count = 0;
    bool     active            = false;
    bool     valid             = false;

    /* CPU total_cycles v okamžiku snapshotu. Pro Cyc-in výpočet
     * (= cycles_now - entry.cycles_at_entry = reálné cycles uvnitř
     * frame od jeho push do snapshot momentu). */
    uint64_t cycles_now        = 0;

    /* Filtry (persistent přes cfgmain). */
    bool hide_irq   = false;
    bool hide_synth = false;
};

CallstackPanelState g_cs;

constexpr float K_DEFAULT_WIDTH  = 880.0f;
constexpr float K_DEFAULT_HEIGHT = 560.0f;

}  /* anonymous namespace */


/* ========================================================================= */
/*  Refresh logika - volání dbgapi CMD_GET_CALLSTACK                          */
/* ========================================================================= */

/**
 * @brief Refresh shadow stacku snapshot z emulátoru.
 *
 * Volá dbgapi CMD_GET_CALLSTACK. Handler alokuje pole entries
 * (callee-allocated, g_malloc) - UI ho zkopíruje do g_cs.entries
 * (std::vector) a okamžitě uvolní pres callstack_snapshot_free.
 *
 * Self-rate-limit + 50 ms timeout shoda se stack_panel pristupem.
 *
 * Side effects: aktualizuje g_cs (entries, stats, active, valid).
 *               Při dbgapi chybě cache zustane (= zachová poslední
 *               platný snímek).
 */
static void callstack_refresh ( void )
{
    if ( dbgapi_ui_queue_is_full ( &g_dbgapi_cmdrq_queue ) ) {
        return;
    };

    st_DBGAPI_CALLSTACK_GET_PARAM p;
    memset ( &p, 0, sizeof ( p ) );

    if ( !dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue,
                                       DBGAPI_CMD_GET_CALLSTACK,
                                       &p, NULL, 50 ) )
    {
        return;
    };

    /* Kopie do lokální cache. Vlastnictví entries pres p.entries -
     * uvolnit po kopii. */
    g_cs.entries.clear ( );
    if ( p.entries && p.count > 0 ) {
        const st_CALLSTACK_ENTRY *src = (const st_CALLSTACK_ENTRY *) p.entries;
        g_cs.entries.assign ( src, src + p.count );
    };
    g_cs.current_depth     = p.current_depth;
    g_cs.max_depth_reached = p.max_depth_reached;
    g_cs.divergence_count  = p.divergence_count;
    g_cs.diverg_trampoline = p.diverg_trampoline;
    g_cs.diverg_longjmp    = p.diverg_longjmp;
    g_cs.diverg_mismatch   = p.diverg_mismatch;
    g_cs.sp_swap_count     = p.sp_swap_count;
    g_cs.overflow_count    = p.overflow_count;
    g_cs.stack_discard_count = p.stack_discard_count;
    g_cs.cycles_now        = p.cycles_now;
    g_cs.active            = ( p.active != 0 );
    g_cs.valid             = true;

    /* Uvolnit callee-allocated buffer (snapshot ownership patří caller). */
    if ( p.entries ) {
        callstack_snapshot_free ( (st_CALLSTACK_ENTRY *) p.entries );
    };
}


/* ========================================================================= */
/*  Pomocné funkce - kind/flag jako text                                     */
/* ========================================================================= */

/**
 * @brief Převede en_CALLSTACK_ENTRY_KIND na čitelný short text.
 *
 * Vrací statický string (= bez ownership), bezpečný pro ImGui::Text*.
 *
 * @param kind  Hodnota z en_CALLSTACK_ENTRY_KIND (cast přes uint8_t).
 * @return Krátký textový popis (např. "CALL", "RST", "IRQ1").
 */
static const char *kind_to_str ( uint8_t kind )
{
    switch ( kind ) {
        case CS_KIND_CALL:      return "CALL";
        case CS_KIND_RST:       return "RST";
        case CS_KIND_IRQ_IM0:   return "IRQ0";
        case CS_KIND_IRQ_IM1:   return "IRQ1";
        case CS_KIND_IRQ_IM2:   return "IRQ2";
        case CS_KIND_NMI:       return "NMI";
        case CS_KIND_SYNTHETIC: return "SYNT";
        default:                return "?";
    };
}


/**
 * @brief Posoudí, zda řádek odpovídá IRQ/NMI kategorii pro filtr.
 *
 * @param kind  uint8_t z en_CALLSTACK_ENTRY_KIND.
 * @return true pokud je to IRQ_IM0/IM1/IM2 nebo NMI.
 */
static bool kind_is_irq_or_nmi ( uint8_t kind )
{
    return kind == CS_KIND_IRQ_IM0
        || kind == CS_KIND_IRQ_IM1
        || kind == CS_KIND_IRQ_IM2
        || kind == CS_KIND_NMI;
}


/* ========================================================================= */
/*  Render - toolbar                                                         */
/* ========================================================================= */

/**
 * @brief Vykreslí horní toolbar s Active checkboxem, Reset stats, filtry
 *        a indikátorem hloubky.
 */
static void render_toolbar ( void )
{
    /* Active checkbox - hot toggle subsystému.
     *
     * Volá callstack_set_active(enable) z UI vlákna, který:
     *   - při ON: zaregistruje Z80 CALL/RET callback sloty (z80_set_call/ret)
     *     a vyresetuje shadow + stats (= čistý start),
     *   - při OFF: deregistruje sloty (NULL) a ponechá shadow pro
     *     poslední zobrazení.
     *
     * Pointer assignment do z80_t.call_cb/ret_cb je 8-byte aligned write
     * (= atomic na x86_64); race s emu vláknem maximálně missne 1 frame
     * pri přechodu, ne crash. Plný safe-point CMDRQ pattern je V1.5+
     * pokud bude reálná synchronizační potřeba. */
    bool active = g_cs.active;
    if ( ImGui::Checkbox ( _L ( "Active###cs_active" ), &active ) ) {
        callstack_set_active ( active );
        g_cs.active = active;
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Reset###cs_reset" ) ) ) {
        /* Full reset: shadow stack + stats (= jako po emu reset).
         * UI cache (g_cs.entries) se vyčistí na dalším refresh ticku. */
        callstack_reset ( );
        g_cs.entries.clear ( );
        g_cs.current_depth     = 0;
        g_cs.max_depth_reached = 0;
        g_cs.divergence_count  = 0;
        g_cs.diverg_trampoline = 0;
        g_cs.diverg_longjmp    = 0;
        g_cs.diverg_mismatch   = 0;
        g_cs.sp_swap_count     = 0;
        g_cs.overflow_count    = 0;
        g_cs.stack_discard_count = 0;
    };

    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "Hide IRQ###cs_hide_irq" ), &g_cs.hide_irq );
    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "Hide Synth###cs_hide_synth" ), &g_cs.hide_synth );

    /* Diverg log - tlačítko otevřé popup s posledními N divergence events
     * pro diagnostiku (= kde se mismatch / trampoline / longjmp dějí). */
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Diverg log###cs_diverg_log_btn" ) ) ) {
        ImGui::OpenPopup ( "cs_diverg_log_popup" );
    };

    /* Hint marker - "(?)" který na hover ukáže nápovědu k ovládacím
     * gestům. Bez něho user nezná že existuje right-click menu nad
     * řádkem ani right-click menu nad hlavičkou sloupce pro show/hide. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "(?)" );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::TextUnformatted ( _( "Callstack panel controls" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s",
            _( "Row click: focus disasm at Call address" ) );
        ImGui::BulletText ( "%s",
            _( "Row Shift+click: focus disasm at Target address" ) );
        ImGui::BulletText ( "%s",
            _( "Row double-click: open Disasm #2 at Call" ) );
        ImGui::BulletText ( "%s",
            _( "Row right-click: context menu (bookmark, BP, copy, ...)" ) );
        ImGui::BulletText ( "%s",
            _( "Row hover: tooltip with frame details" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s",
            _( "Column header right-click: show/hide columns" ) );
        ImGui::BulletText ( "%s",
            _( "Column header drag: reorder columns" ) );
        ImGui::BulletText ( "%s",
            _( "Column border drag: resize" ) );
        ImGui::Separator ( );
        ImGui::TextDisabled ( "%s",
            _( "Frame and Pxclk columns are hidden by default." ) );
        ImGui::EndTooltip ( );
    };
    /* Pozn: depth indikator je v status footeru, ne tady (= nesouvisí
     * s Hide checkboxy). */
}


/* ========================================================================= */
/*  Render - tabulka entries                                                 */
/* ========================================================================= */

/**
 * @brief Vykreslí tabulku se shadow stack entries.
 *
 * Sloupce: #, Ret, Call, Target, Sym, Kind, Cyc-in.
 * Interakce: klik LMB = focus disasm na call_site, Shift+klik = focus
 * na target, double-click = open extra disasm slot na call_site.
 *
 * Pořadí řádků: TOP nejmladší frame = horní řádek (= g_cs.entries
 * sestupně od end()-1 k begin()).
 */
static void render_table ( void )
{
    if ( g_cs.entries.empty ( ) ) {
        ImGui::TextDisabled ( "%s",
            _( "Stack is empty (callstack inactive or no frames pushed)" ) );
        return;
    };

    /* Resizable + Reorderable + Hideable - user může táhnout border,
     * měnit pořadí, skrývat sloupce přes right-click na hlavičce.
     * SizingStretchProp = nepoužité místo se rozdělí proporcionálně. */
    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                          | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_Resizable
                          | ImGuiTableFlags_Reorderable
                          | ImGuiTableFlags_Hideable
                          | ImGuiTableFlags_SizingStretchProp;

    if ( !ImGui::BeginTable ( "###cs_table", 11, flags ) ) {
        return;
    };

    /* Default šířky vyladěné na default font + monospaced hex (4 chars =
     * "FFFF" ~ 36 px). Stretch = Sym, ostatní fixed s rezervou na padding.
     * Frame/Sline/Px/Pxclk + Cyc-in jsou Hideable - skryjí se přes
     * right-click na hlavičce pokud uživatel nepotřebuje raster info. */
    const float w_hex = ImGui::CalcTextSize ( "FFFF" ).x + 16.0f;  /* ~48 px */
    ImGui::TableSetupScrollFreeze ( 0, 1 );  /* Sticky header. */
    ImGui::TableSetupColumn ( "#",      ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize ( "999" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Ret",    ImGuiTableColumnFlags_WidthFixed, w_hex );
    ImGui::TableSetupColumn ( "Call",   ImGuiTableColumnFlags_WidthFixed, w_hex );
    ImGui::TableSetupColumn ( "Target", ImGuiTableColumnFlags_WidthFixed, w_hex );
    ImGui::TableSetupColumn ( "Sym",    ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn ( "Kind",   ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize ( "IRQ2" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Frame",  ImGuiTableColumnFlags_WidthFixed
                              | ImGuiTableColumnFlags_DefaultHide,
                              ImGui::CalcTextSize ( "99999" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Sline",  ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize ( "999" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Px",     ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize ( "9999" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Pxclk",  ImGuiTableColumnFlags_WidthFixed
                              | ImGuiTableColumnFlags_DefaultHide,
                              ImGui::CalcTextSize ( "999999999" ).x + 14.0f );
    ImGui::TableSetupColumn ( "Cyc-in", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize ( "9999999" ).x + 14.0f );
    ImGui::TableHeadersRow ( );

    /* Aktuální cycle pro výpočet Cyc-in (= cycles_now - cycles_at_entry).
     * cycles_now je cpu->total_cycles v okamžiku snapshotu (zapsáno
     * handlerem v dbgapi.c). Tj. Cyc-in je reálný počet T-states
     * uvnitř frame od jeho push do snapshot momentu. */
    const uint64_t cycles_now = g_cs.cycles_now;

    /* Iterace TOP-first (= last vector entry = top of stack = index 0
     * v UI). Depth index zobrazuje #0 = top, #N = nejstarší frame. */
    const int n = (int) g_cs.entries.size ( );
    for ( int ui_idx = 0; ui_idx < n; ui_idx++ ) {
        int vec_idx = ( n - 1 ) - ui_idx;  /* vector je oldest..newest. */
        const st_CALLSTACK_ENTRY &e = g_cs.entries[ vec_idx ];

        /* Filtry. */
        if ( g_cs.hide_irq && kind_is_irq_or_nmi ( e.kind ) ) continue;
        if ( g_cs.hide_synth && e.kind == CS_KIND_SYNTHETIC ) continue;

        ImGui::TableNextRow ( );

        /* # depth index. */
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::PushID ( ui_idx );

        /* Row-wide klikabilita: Selectable přes celý řádek pro reakci
         * na klik / Shift+klik / double-klik. */
        char idx_lbl[ 16 ];
        snprintf ( idx_lbl, sizeof ( idx_lbl ), "%d", ui_idx );
        bool sel_clicked = ImGui::Selectable (
            idx_lbl, false,
            ImGuiSelectableFlags_SpanAllColumns
            | ImGuiSelectableFlags_AllowDoubleClick );

        if ( sel_clicked ) {
            bool is_shift  = ImGui::GetIO ( ).KeyShift;
            bool is_double = ImGui::IsMouseDoubleClicked ( 0 );
            uint16_t addr  = is_shift ? e.target_addr : e.call_site_addr;
            if ( is_double ) {
                /* Otevři/Focus sekundární disasm slot #2 (Disassembly #2).
                 * dbg_disasm_show_in_slot interně řeší ensure-open. */
                dbg_disasm_show_in_slot ( 1, addr );
            }
            else {
                /* Focus hlavní disasm (slot 0 = main view, eager create
                 * pokud ještě neexistuje). */
                dbg_disasm_show_in_slot ( 0, addr );
            };
        };

        /* Hover tooltip - vysvětlí klik akce + ukáže detaily entry. */
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::BeginTooltip ( );
            ImGui::Text ( "%s",
                _( "Click: focus disasm at Call" ) );
            ImGui::Text ( "%s",
                _( "Shift+Click: focus disasm at Target" ) );
            ImGui::Text ( "%s",
                _( "Double-click: open in Disasm #2 at Call" ) );
            ImGui::Text ( "%s",
                _( "Right-click: more actions" ) );
            ImGui::Separator ( );
            ImGui::Text ( "Ret:    %04Xh", e.return_addr );
            ImGui::Text ( "Call:   %04Xh", e.call_site_addr );
            ImGui::Text ( "Target: %04Xh", e.target_addr );
            ImGui::Text ( "SP:     %04Xh", e.sp_at_entry );
            ImGui::Text ( "Kind:   %s", kind_to_str ( e.kind ) );
            if ( e.kind == CS_KIND_IRQ_IM2 ) {
                ImGui::Text ( "IM2 vec: %04Xh", e.im2_vector_addr );
            };
            if ( ( e.flags & CS_FLAG_DIVERGENT ) != 0 ) {
                ImGui::TextColored (
                    ImVec4 ( 1.0f, 0.86f, 0.31f, 1.0f ),
                    "%s", _( "DIVERGENT (push+ret trampoline or longjmp)" ) );
            };
            ImGui::EndTooltip ( );
        };

        /* Right-click context menu. */
        if ( ImGui::BeginPopupContextItem ( "###cs_row_ctx" ) ) {
            char buf[ 64 ];

            if ( ImGui::MenuItem ( _L ( "Focus disasm at Call###cs_focus_call" ) ) ) {
                dbg_disasm_show_in_slot ( 0, e.call_site_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Focus disasm at Target###cs_focus_target" ) ) ) {
                dbg_disasm_show_in_slot ( 0, e.target_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Open Disasm #2 at Call###cs_disasm2_call" ) ) ) {
                dbg_disasm_show_in_slot ( 1, e.call_site_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Open Disasm #2 at Target###cs_disasm2_target" ) ) ) {
                dbg_disasm_show_in_slot ( 1, e.target_addr );
            };

            ImGui::Separator ( );

            if ( ImGui::MenuItem ( _L ( "Add bookmark at Call###cs_bm_call" ) ) ) {
                const st_SYMBOL *sym = sym_db_lookup_by_addr (
                    (uint32_t) e.call_site_addr, e.bank_id );
                if ( sym && sym->name && sym->name[ 0 ] ) {
                    snprintf ( buf, sizeof ( buf ), "%s", sym->name );
                }
                else {
                    snprintf ( buf, sizeof ( buf ), "#%04X", e.call_site_addr );
                };
                bookmarks_add ( buf, "" );
                g_gui->showBookmarksWindow = true;
            };
            if ( ImGui::MenuItem ( _L ( "Add bookmark at Target###cs_bm_target" ) ) ) {
                const st_SYMBOL *sym = sym_db_lookup_by_addr (
                    (uint32_t) e.target_addr, e.bank_id );
                if ( sym && sym->name && sym->name[ 0 ] ) {
                    snprintf ( buf, sizeof ( buf ), "%s", sym->name );
                }
                else {
                    snprintf ( buf, sizeof ( buf ), "#%04X", e.target_addr );
                };
                bookmarks_add ( buf, "" );
                g_gui->showBookmarksWindow = true;
            };

            ImGui::Separator ( );

            if ( ImGui::MenuItem ( _L ( "Set BP at Call###cs_bp_call" ) ) ) {
                dbg_ui_bp_add ( e.call_site_addr, NULL );
            };
            if ( ImGui::MenuItem ( _L ( "Set BP at Target###cs_bp_target" ) ) ) {
                dbg_ui_bp_add ( e.target_addr, NULL );
            };

            ImGui::Separator ( );

            /* Copy hex helpers. */
            snprintf ( buf, sizeof ( buf ), "%04Xh", e.call_site_addr );
            if ( ImGui::MenuItem ( _L ( "Copy Call address###cs_copy_call" ) ) ) {
                ImGui::SetClipboardText ( buf );
            };
            snprintf ( buf, sizeof ( buf ), "%04Xh", e.target_addr );
            if ( ImGui::MenuItem ( _L ( "Copy Target address###cs_copy_target" ) ) ) {
                ImGui::SetClipboardText ( buf );
            };
            snprintf ( buf, sizeof ( buf ), "%04Xh", e.return_addr );
            if ( ImGui::MenuItem ( _L ( "Copy Return address###cs_copy_ret" ) ) ) {
                ImGui::SetClipboardText ( buf );
            };

            ImGui::EndPopup ( );
        };

        ImGui::TableSetColumnIndex ( 1 );
        ImGui::Text ( "%04Xh", e.return_addr );

        ImGui::TableSetColumnIndex ( 2 );
        ImGui::Text ( "%04Xh", e.call_site_addr );

        ImGui::TableSetColumnIndex ( 3 );
        ImGui::Text ( "%04Xh", e.target_addr );

        /* Sym lookup per render frame (V1 = bez cache). */
        ImGui::TableSetColumnIndex ( 4 );
        const st_SYMBOL *sym = sym_db_lookup_by_addr (
            (uint32_t) e.target_addr, e.bank_id );
        if ( sym && sym->name ) {
            ImGui::TextUnformatted ( sym->name );
        }
        else {
            ImGui::TextDisabled ( "%s", _( "(no sym)" ) );
        };

        ImGui::TableSetColumnIndex ( 5 );
        const char *kind_s = kind_to_str ( e.kind );
        if ( ( e.flags & CS_FLAG_DIVERGENT ) != 0 ) {
            /* Divergentní řádek - žlutá. */
            ImGui::PushStyleColor ( ImGuiCol_Text,
                IM_COL32 ( 255, 220, 80, 255 ) );
            ImGui::TextUnformatted ( kind_s );
            ImGui::PopStyleColor ( );
        }
        else {
            ImGui::TextUnformatted ( kind_s );
        };

        /* Raster timing snapshot v okamžiku push framu. Frame = číslo
         * snímku, Sline = scanline 0..VIDEO_SCREEN_HEIGHT-1, Px = pixel
         * column 0..VIDEO_SCREEN_WIDTH-1, Pxclk = total pxclk counter
         * (= integer rostoucí přes všechny snímky). */
        const uint32_t v_width = (uint32_t) g_mzhal.video_screen_width;
        const uint32_t sline   = e.pxclk_in_screen / v_width;
        const uint32_t px_col  = e.pxclk_in_screen % v_width;

        ImGui::TableSetColumnIndex ( 6 );
        ImGui::Text ( "%u", e.screens_total );

        ImGui::TableSetColumnIndex ( 7 );
        ImGui::Text ( "%u", sline );

        ImGui::TableSetColumnIndex ( 8 );
        ImGui::Text ( "%u", px_col );

        ImGui::TableSetColumnIndex ( 9 );
        ImGui::Text ( "%llu", (unsigned long long) e.pxclk_total );

        ImGui::TableSetColumnIndex ( 10 );
        /* Cyc-in = cycles_now - entry.cycles_at_entry = reálný počet
         * T-states strávených uvnitř frame od jeho push do snapshot
         * okamžiku. Nejmladší frame (#0) má nejmenší hodnotu, nejstarší
         * frame (#N) největší (= obsahuje cycles celé volací řetězec). */
        uint64_t cyc_in = 0;
        if ( cycles_now > e.cycles_at_entry ) {
            cyc_in = cycles_now - e.cycles_at_entry;
        };
        ImGui::Text ( "%llu", (unsigned long long) cyc_in );

        ImGui::PopID ( );
    };

    ImGui::EndTable ( );
}


/* ========================================================================= */
/*  Render - status footer                                                   */
/* ========================================================================= */

/**
 * @brief Vykreslí status footer s counters (divergence, sp-swap, overflow).
 */
static void render_footer ( void )
{
    ImGui::Separator ( );
    ImGui::Text ( "Depth: %d / max=%d",
                  g_cs.current_depth, g_cs.max_depth_reached );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    /* Diverg breakdown - trampoline (typ PUSH+RET dispatch v CP/M BDOS),
     * longjmp (RET pop přes více framů), mismatch (= konzervativní pop
     * po neúspěšném match). Hover ukáže detail. */
    ImGui::Text ( "Diverg: %u", g_cs.divergence_count );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::TextUnformatted ( _( "Divergence breakdown" ) );
        ImGui::Separator ( );
        ImGui::Text ( "Trampoline: %u",  g_cs.diverg_trampoline );
        ImGui::Text ( "Longjmp:    %u",  g_cs.diverg_longjmp );
        ImGui::Text ( "Mismatch:   %u",  g_cs.diverg_mismatch );
        ImGui::Separator ( );
        ImGui::TextDisabled ( "%s",
            _( "Trampoline = PUSH+RET (e.g. CP/M BDOS dispatch)" ) );
        ImGui::TextDisabled ( "%s",
            _( "Longjmp    = RET unwinds multiple frames at once" ) );
        ImGui::TextDisabled ( "%s",
            _( "Mismatch   = RET top mismatch, conservative pop" ) );
        ImGui::EndTooltip ( );
    };
    ImGui::SameLine ( );
    ImGui::Text ( " SP-swap: %u  Overflow: %u  Discard: %u",
                  g_cs.sp_swap_count,
                  g_cs.overflow_count,
                  g_cs.stack_discard_count );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 35.0f );
        ImGui::TextUnformatted (
            _( "Discard counter = SP jumped above deepest tracked frame "
               "(possible stack reset: warm boot, ROM reset, exception, "
               "OS scheduler). Counter only - shadow is NOT auto-cleared "
               "(OS patterns like CP/M BDOS multi-stack would false-trigger). "
               "If counter grows during analysis, press Reset for clean start." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    };
}


/* ========================================================================= */
/*  Render - Diverg log popup                                                */
/* ========================================================================= */

/**
 * @brief Vykreslí popup s ring bufferem nedávných divergence events.
 *
 * Otevírá se přes Button "Diverg log" v toolbar (= OpenPopup
 * "cs_diverg_log_popup"). Volá public API @c callstack_get_recent_diverg
 * a renderuje tabulku s detaily pro diagnostiku.
 */
static void render_diverg_log_popup ( void )
{
    /* Modal popup s explicit Close, fixed size pro stabilitu layoutu.
     * AlwaysAutoResize aby user neviděl scrollbar pro malý počet entries. */
    if ( !ImGui::BeginPopup ( "cs_diverg_log_popup",
                              ImGuiWindowFlags_AlwaysAutoResize ) ) {
        return;
    };

    ImGui::Text ( "%s", _( "Recent divergence events (ring buffer, oldest first)" ) );
    ImGui::Separator ( );

    /* Pull recent events z jádra. */
    st_CALLSTACK_DIVERG_EVENT events[ CALLSTACK_DIVERG_RING_SIZE ];
    int n = callstack_get_recent_diverg ( events );

    if ( n == 0 ) {
        ImGui::TextDisabled ( "%s", _( "No divergence events recorded yet." ) );
        if ( ImGui::Button ( _L ( "Close###cs_diverg_close" ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
        return;
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                          | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_Resizable
                          | ImGuiTableFlags_SizingFixedFit
                          | ImGuiTableFlags_ScrollY;

    /* 9 columns: #, Kind, RET pc, Pop tgt, SP before, Top return, Top sp,
     * Top kind, Shadow depth at event */
    if ( ImGui::BeginTable ( "###cs_diverg_table", 9, flags,
                              ImVec2 ( 800.0f, 280.0f ) ) ) {
        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableSetupColumn ( "#",         ImGuiTableColumnFlags_WidthFixed, 30.0f );
        ImGui::TableSetupColumn ( "Kind",      ImGuiTableColumnFlags_WidthFixed, 75.0f );
        ImGui::TableSetupColumn ( "RET pc",    ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( "Pop tgt",   ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( "SP before", ImGuiTableColumnFlags_WidthFixed, 70.0f );
        ImGui::TableSetupColumn ( "Top ret",   ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( "Top sp",    ImGuiTableColumnFlags_WidthFixed, 60.0f );
        ImGui::TableSetupColumn ( "Top kind",  ImGuiTableColumnFlags_WidthFixed, 70.0f );
        ImGui::TableSetupColumn ( "Depth",     ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableHeadersRow ( );

        for ( int i = 0; i < n; i++ ) {
            const st_CALLSTACK_DIVERG_EVENT &e = events[ i ];
            ImGui::TableNextRow ( );

            ImGui::TableSetColumnIndex ( 0 );
            ImGui::Text ( "%d", i );

            ImGui::TableSetColumnIndex ( 1 );
            const char *k = "?";
            switch ( e.kind ) {
                case CS_DIVERG_TRAMPOLINE:    k = "Trampoline"; break;
                case CS_DIVERG_LONGJMP:       k = "Longjmp";    break;
                case CS_DIVERG_MISMATCH:      k = "Mismatch";   break;
                case CS_DIVERG_STACK_DISCARD: k = "Discard";    break;
                default: break;
            }
            ImGui::TextUnformatted ( k );

            ImGui::TableSetColumnIndex ( 2 );
            if ( e.ret_pc == 0 && e.pop_target == 0 && e.sp_before == 0 ) {
                ImGui::TextDisabled ( "RETI/N" );  /* nedostupné z hooku */
            }
            else {
                ImGui::Text ( "%04Xh", e.ret_pc );
            };

            ImGui::TableSetColumnIndex ( 3 );
            ImGui::Text ( "%04Xh", e.pop_target );

            ImGui::TableSetColumnIndex ( 4 );
            ImGui::Text ( "%04Xh", e.sp_before );

            ImGui::TableSetColumnIndex ( 5 );
            if ( e.shadow_depth == 0 ) {
                ImGui::TextDisabled ( "-" );
            }
            else {
                ImGui::Text ( "%04Xh", e.top_return );
            };

            ImGui::TableSetColumnIndex ( 6 );
            if ( e.shadow_depth == 0 ) {
                ImGui::TextDisabled ( "-" );
            }
            else {
                ImGui::Text ( "%04Xh", e.top_sp );
            };

            ImGui::TableSetColumnIndex ( 7 );
            if ( e.top_kind == 0xFF ) {
                ImGui::TextDisabled ( "empty" );
            }
            else {
                ImGui::TextUnformatted ( kind_to_str ( e.top_kind ) );
            };

            ImGui::TableSetColumnIndex ( 8 );
            ImGui::Text ( "%u", (unsigned) e.shadow_depth );
        };

        ImGui::EndTable ( );
    };

    ImGui::Separator ( );
    if ( ImGui::Button ( _L ( "Close###cs_diverg_close" ) ) ) {
        ImGui::CloseCurrentPopup ( );
    };
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "%s",
        _( "Tip: RET pc shows where divergence occurred. Open it in disasm to investigate the calling pattern." ) );

    ImGui::EndPopup ( );
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

extern "C" void dbg_callstack_render ( void )
{
    /* Refresh dat pokud je refresh tick. */
    if ( g_dbg_ui.refresh.should_refresh ) {
        callstack_refresh ( );
    };

    render_toolbar ( );
    ImGui::Separator ( );

    /* Tabulka v dostupné ploše (footer nechá místo na 2-3 řádky). */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float footer_h = ImGui::GetTextLineHeightWithSpacing ( )
                   + ImGui::GetStyle ( ).ItemSpacing.y * 2.0f;
    float table_h  = avail.y - footer_h;
    if ( table_h < 60.0f ) table_h = 60.0f;

    ImGui::BeginChild ( "###cs_table_child",
                        ImVec2 ( 0.0f, table_h ),
                        false, ImGuiWindowFlags_None );
    render_table ( );
    ImGui::EndChild ( );

    render_footer ( );

    /* Diverg log popup (rendering trigger v render_toolbar). */
    render_diverg_log_popup ( );
}


/* ========================================================================= */
/*  cfgmain - [CALLSTACK_PANEL] sekce                                        */
/* ========================================================================= */

extern "C" void dbg_callstack_cfg_init ( void )
{
    static bool s_registered = false;
    if ( s_registered ) return;
    s_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, (char *) "CALLSTACK_PANEL" );
    if ( !cmod ) return;

    CFGELM *elm;

    elm = cfgmodule_register_new_element ( cmod, (char *) "hide_irq", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void *) &g_cs.hide_irq,
                                    (void *) &g_cs.hide_irq );

    elm = cfgmodule_register_new_element ( cmod, (char *) "hide_synth", CFGENTYPE_BOOL, 0 );
    cfgelement_set_handlers ( elm, (void *) &g_cs.hide_synth,
                                    (void *) &g_cs.hide_synth );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
