/*
 * bm_window.cpp - Bookmarks panel pro pojmenované adresové záložky.
 *
 * Layout shora dolů (sjednoceno s Watch / Symbols / Variables):
 *   +---------------------------------------------------------------+
 *   | Bookmarks                                              [X]    |
 *   +---------------------------------------------------------------+
 *   | Sticky header:                                                |
 *   |   [+ Add] Filter:[__________] [Clear]  (vis/tot)              |
 *   |   Selected: N  [Delete]  |  [Save] [Save As...] [Load...] ... |
 *   +---------------------------------------------------------------+
 *   | (Add form collapsible block - jen když [+ Add] je aktivní)    |
 *   |   Bookmark: [_______________________] [Insert PC]             |
 *   |   Comment:  [_______________________] [Add]                   |
 *   +---------------------------------------------------------------+
 *   | [v]| [>][...] Label  | $address | Comment       | Del         |
 *   |----|-----------------|----------|---------------|-------------|
 *   | [v]| [>][...] MAIN_..| $E6CA    | hlavni smycka | [X]         |
 *   | [ ]| [>][...] 0x1234 | $1234    |               | [X]         |
 *   +---------------------------------------------------------------+
 *
 * Sloupec Label (= sloupec 1) layout: tlačítko šipka + tlačítko "..."
 * + plain text label. Sloupec $address (= sloupec 2) je jen plain text.
 *   - Šipka tlačítko = focus_in_disasm na slot 0 (= primary disasm).
 *   - "..." tlačítko = popup "Show in Disassembly #1..#5".
 *   - Pokud addr_valid == false: obě tlačítka disabled, label TextDisabled.
 *
 * Inline edit:
 *   - Klik na [E] -> sloupce Label a Comment se přepnou na InputText,
 *     [E] [X] se nahradí [Apply ✓] [Cancel ✗]
 *   - Enter v jakémkoli edit InputText = Apply (save changes)
 *   - Escape v edit režimu = Cancel (revert)
 *
 * Header tabulky:
 *   - Sloupec Sel má 3-state checkbox (none/some/all) pro select/deselect
 *     all visible (= prošlé filtrem) řádky.
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
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include "bm_window.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"
#include "ui-imgui/mcp_activity/owner_badge.h"  /* V1.C.3 owner badge */

#include <set>
#include <string>
#include <vector>
#include <cstring>

extern "C" {
#include "emulator/debugger/bookmarks/bookmarks.h"
#include "emulator/debugger/symbols/sym_db.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"
}


/* ========================================================================= */
/*  UI state (persistentní per-frame)                                        */
/* ========================================================================= */

namespace {

/**
 * @brief State přepravený mezi framy.
 */
struct BmState {
    /* Filter input */
    char     filter_buf[128] = {};

    /* Add form */
    bool     add_form_open = false;
    char     add_input_buf[BOOKMARK_INPUT_MAX + 1] = {};
    char     add_comment_buf[BOOKMARK_COMMENT_MAX + 1] = {};
    bool     focus_add_input_next_frame = false;

    /* Selection - set IDs */
    std::set<uint32_t> selected_ids;

    /* Inline edit - aktivní ID + buffery */
    uint32_t edit_id = 0;
    char     edit_input_buf[BOOKMARK_INPUT_MAX + 1] = {};
    char     edit_comment_buf[BOOKMARK_COMMENT_MAX + 1] = {};

    /* RMB popup target */
    uint32_t rmb_popup_id = 0;
    uint16_t rmb_popup_addr = 0;
    bool     rmb_popup_open = false;

    /* V1.E.6.A: pending focus request z Activity okna (= dvojklik routing).
     * Hodnota != 0 = budoucí frame má scrollnout na řádek s bookmark.id == X
     * a označit ho jako selected. Po spotřebě je reset na 0. */
    uint32_t pending_focus_id = 0;
};

static BmState g_bm;

}  /* anon namespace */


/* ========================================================================= */
/*  Helpers                                                                  */
/* ========================================================================= */


/**
 * @brief Case-insensitive substring search (ASCII).
 */
static bool bm_str_contains_ci ( const char *haystack, const char *needle ) {
    if ( !needle || !needle[0] ) return true;
    if ( !haystack ) return false;
    size_t hl = strlen ( haystack );
    size_t nl = strlen ( needle );
    if ( nl > hl ) return false;
    for ( size_t i = 0; i + nl <= hl; i++ ) {
        bool ok = true;
        for ( size_t j = 0; j < nl; j++ ) {
            char a = haystack[i + j];
            char b = needle[j];
            if ( a >= 'A' && a <= 'Z' ) a = (char)( a + 32 );
            if ( b >= 'A' && b <= 'Z' ) b = (char)( b + 32 );
            if ( a != b ) { ok = false; break; };
        };
        if ( ok ) return true;
    };
    return false;
}


/**
 * @brief Resolve label pro display (= sym_db jméno pokud user_input
 *        je hex literál a symbol existuje, jinak user_input).
 *
 * Tj. pokud user zadá "0xE6CA" a v sym_db je MAIN_LOOP @ 0xE6CA, label
 * sloupec ukáže "MAIN_LOOP" (= příjemnější UX).
 *
 * @param user_input  Vstupní user_input.
 * @param out         Buffer pro výsledný label.
 * @param outsize     Velikost bufferu.
 */
static void bm_compute_label ( const char *user_input, char *out, size_t outsize ) {
    if ( !out || outsize == 0 ) return;
    out[0] = '\0';
    if ( !user_input || !user_input[0] ) return;

    /* Pokud je user_input hex literál, zkus najít symbol pro adresu. */
    uint16_t addr = 0;
    if ( bookmarks_resolve_addr ( user_input, &addr ) ) {
        /* Pokud user_input je čistý hex, zkus symbol lookup. */
        const st_SYMBOL *sym = sym_db_lookup_by_addr ( addr, 0 );
        if ( sym && sym->name && sym->name[0] ) {
            /* Pokud user_input se nerovná sym jménu (= je to hex zápis),
             * ukaž symbol jako label. Jinak (= user zadal samotné jméno
             * symbolu) ukaž user_input. */
            if ( strcmp ( user_input, sym->name ) != 0 ) {
                snprintf ( out, outsize, "%s", sym->name );
                return;
            };
        };
    };
    snprintf ( out, outsize, "%s", user_input );
}


/**
 * @brief Spočítá, zda bookmark splňuje aktuální filter.
 *
 * Filter substring se hledá case-insensitive ve třech zdrojích:
 *   - user_input (= raw vstup)
 *   - resolved label (= sym_db jméno pokud user_input je hex)
 *   - comment
 *
 * Při prázdném filtru (`g_bm.filter_buf[0] == '\\0'`) vrací true pro
 * jakýkoli neprázdný bookmark.
 *
 * @param bm Pointer do storage; pokud NULL nebo bm->user_input == NULL,
 *           vrací false.
 * @return true pokud bookmark má být vidět v aktuálním filtru.
 */
static bool bm_filter_match ( const bookmark_t *bm ) {
    if ( !bm || !bm->user_input ) return false;
    if ( g_bm.filter_buf[0] == '\0' ) return true;
    char label_buf[128];
    bm_compute_label ( bm->user_input, label_buf, sizeof ( label_buf ) );
    if ( bm_str_contains_ci ( bm->user_input, g_bm.filter_buf ) ) return true;
    if ( bm_str_contains_ci ( label_buf,      g_bm.filter_buf ) ) return true;
    if ( bm->comment &&
         bm_str_contains_ci ( bm->comment, g_bm.filter_buf ) ) return true;
    return false;
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */


extern "C" void bm_window_open_with_focus_to_input ( void ) {
    g_gui->showBookmarksWindow = true;
    g_bm.add_form_open = true;
    g_bm.focus_add_input_next_frame = true;
}


extern "C" void bm_window_focus_id ( uint32_t bm_id ) {
    /* V1.E.6.A: Activity dvojklik routing - otevři okno a označ pending
     * focus ID. Spotřebitelem je render loop (bm_window_render), který
     * při dalším frame najde řádek s odpovídajícím bookmark.id, scrollne
     * na něj a označí jako selected. Pokud bookmark s daným ID v storage
     * neexistuje (= mezičasem smazán), spotřeba je no-op a flag se resetne.
     *
     * Hodnota 0 je legální bookmark ID (= storage začíná od 1), takže
     * 0 jako sentinel pro "no pending" je bezpečné per bookmarks_get_by_id
     * kontraktu (= 0 vrací NULL). */
    if ( bm_id == 0 ) return;
    g_gui->showBookmarksWindow = true;
    g_bm.pending_focus_id = bm_id;
}


/* ========================================================================= */
/*  Render - jednotlivé sekce                                                */
/* ========================================================================= */


/**
 * @brief Render sticky header s filter + selection ops + file ops.
 *
 * Layout sjednocen s Watch / Symbols / Variables okny (= [+ Add] toggle,
 * Filter: label + input + Clear + (vis/tot) na řádku 1; Selected + Delete +
 * file ops na řádku 2). Inline Add form se renderuje v samostatném bloku
 * pod headerem jen pokud `g_bm.add_form_open == true`.
 */
static void bm_render_header ( size_t total_count, size_t filtered_count ) {
    /* Řádek 1: [+ Add] / [- Cancel] toggle + Filter + Clear + counts. */
    ImGui::AlignTextToFramePadding ( );
    if ( ImGui::Button ( g_bm.add_form_open ? _L ( "- Cancel##bm_add_toggle" )
                                            : _L ( "+ Add##bm_add_toggle" ) ) ) {
        g_bm.add_form_open = !g_bm.add_form_open;
        if ( !g_bm.add_form_open ) {
            g_bm.add_input_buf[0]   = '\0';
            g_bm.add_comment_buf[0] = '\0';
        } else {
            g_bm.focus_add_input_next_frame = true;
        };
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Add a new bookmark (expands form below)." ) );
    };

    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Filter:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 200.0f );
    ImGui::InputTextWithHint ( "###bm_filter",
                                _( "label, comment, address" ),
                                g_bm.filter_buf, sizeof ( g_bm.filter_buf ) );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Filter matches label, comment, or address "
               "(case-insensitive substring)." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear##bm_filter_clr" ) ) ) {
        g_bm.filter_buf[0] = '\0';
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Clear the filter text." ) );
    };
    ImGui::SameLine ( );
    ImGui::Text ( "(%zu/%zu)", filtered_count, total_count );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Visible / total bookmark count." ) );
    };

    /* Řádek 2: Selected + Delete + file ops. */
    size_t n_sel = g_bm.selected_ids.size ( );
    ImGui::Text ( "%s %zu", _( "Selected:" ), n_sel );
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( n_sel == 0 );
    if ( ImGui::Button ( _L ( "Delete##bm_bulk_del" ) ) ) {
        for ( uint32_t id : g_bm.selected_ids ) {
            bookmarks_remove ( id );
        };
        g_bm.selected_ids.clear ( );
    };
    ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) && n_sel > 0 ) {
        ImGui::SetTooltip ( "%s", _( "Delete selected bookmarks." ) );
    };

    /* Vizuální oddělovač mezi bulk ops a file ops (vzor Variables). */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* File ops */
    if ( ImGui::Button ( _L ( "Save##bm_save" ) ) ) {
        if ( !bookmarks_save ( NULL ) ) {
            fprintf ( stderr, "BOOKMARKS: save failed\n" );
        };
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Save to default per-arch .bookmarks file" ) );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Save As...##bm_saveas" ) ) ) {
        IGFD::FileDialogConfig config;
        const char *def = bookmarks_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles |
                       ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "BookmarksSaveDialog", _( "Save Bookmarks As..." ),
            ".bookmarks", config );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Load...##bm_load" ) ) ) {
        IGFD::FileDialogConfig config;
        const char *def = bookmarks_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "BookmarksLoadDialog", _( "Load Bookmarks From..." ),
            ".bookmarks,.*", config );
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Replace current bookmarks with file content" ) );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Merge...##bm_merge" ) ) ) {
        IGFD::FileDialogConfig config;
        const char *def = bookmarks_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "BookmarksMergeDialog", _( "Merge Bookmarks From..." ),
            ".bookmarks,.*", config );
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Add bookmarks from file (skip identical duplicates)" ) );
    };
}


/**
 * @brief Render add-form sekce ve formátu 2x2 tabulky pro zarovnání.
 *
 * Layout (3 sloupce, 2 řádky):
 *   | Bookmark: | [textentry______________] | [Insert PC] |
 *   | Comment:  | [textentry______________] | [Add]       |
 *
 * Sloupec 0 (label): fixed width = max(text("Bookmark:"), text("Comment:")).
 * Sloupec 1 (input): WidthStretch = roztáhne se na zbytek.
 * Sloupec 2 (button): fixed width = max(button("Insert PC"), button("Add")).
 *
 * Validace: prázdný bookmark input = červený border kolem InputText +
 * disabled tlačítko Add.
 *
 * "Insert PC" vloží g_mzarch_main.cpu->pc jako "#XXXX" do bookmark inputu.
 * "Add" provede bookmarks_add(input, comment) a clearne oba textové buffery.
 */
static void bm_render_add_form ( void ) {
    if ( !g_bm.add_form_open ) return;

    ImGui::Indent ( );
    ImGui::BeginGroup ( );

    /* Validace - prázdný input = červený border + disabled Add. */
    bool input_empty = ( g_bm.add_input_buf[0] == '\0' );

    /* Spočítej fixed šířky - label sloupec = max obou labelů,
     * button sloupec = max šířka obou tlačítek (s padding). */
    float label_w = ImGui::CalcTextSize ( _( "Bookmark:" ) ).x;
    float comment_label_w = ImGui::CalcTextSize ( _( "Comment:" ) ).x;
    if ( comment_label_w > label_w ) label_w = comment_label_w;
    label_w += ImGui::GetStyle ( ).ItemInnerSpacing.x;

    float btn_w = ImGui::CalcTextSize ( _( "Insert PC" ) ).x;
    float add_btn_w = ImGui::CalcTextSize ( _( "Add" ) ).x;
    if ( add_btn_w > btn_w ) btn_w = add_btn_w;
    btn_w += ImGui::GetStyle ( ).FramePadding.x * 2.0f;

    /* Spacing mezi text entry a tlačítkem - vypočítaný jako width mezery + ItemSpacing */
    float spacing_w = ImGui::GetStyle ( ).ItemSpacing.x * 2.0f;

    const ImGuiTableFlags add_flags = ImGuiTableFlags_SizingStretchProp |
                                        ImGuiTableFlags_NoPadInnerX;
    if ( ImGui::BeginTable ( "##bm_add_form", 4, add_flags ) ) {
        ImGui::TableSetupColumn ( "##lbl",   ImGuiTableColumnFlags_WidthFixed,
                                   label_w );
        ImGui::TableSetupColumn ( "##input", ImGuiTableColumnFlags_WidthStretch,
                                   1.0f );
        ImGui::TableSetupColumn ( "##spacer", ImGuiTableColumnFlags_WidthFixed,
                                   spacing_w );
        ImGui::TableSetupColumn ( "##btn",   ImGuiTableColumnFlags_WidthFixed,
                                   btn_w );

        /* Řádek 1: Bookmark: [input] [Insert PC] */
        ImGui::TableNextRow ( );
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _( "Bookmark:" ) );

        ImGui::TableSetColumnIndex ( 1 );
        if ( g_bm.focus_add_input_next_frame ) {
            ImGui::SetKeyboardFocusHere ( );
            g_bm.focus_add_input_next_frame = false;
        };
        if ( input_empty ) {
            ImGui::PushStyleColor ( ImGuiCol_Border,
                                     IM_COL32 ( 200, 50, 50, 200 ) );
            ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 1.0f );
        };
        ImGui::SetNextItemWidth ( -FLT_MIN );
        ImGui::InputTextWithHint ( "##bm_add_input",
                                    _( "address or symbol" ),
                                    g_bm.add_input_buf,
                                    sizeof ( g_bm.add_input_buf ) );
        if ( input_empty ) {
            ImGui::PopStyleVar ( );
            ImGui::PopStyleColor ( );
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::SetTooltip ( "%s",
                    _( "Address or symbol required" ) );
            };
        };

        ImGui::TableSetColumnIndex ( 3 );
        if ( ImGui::Button ( _L ( "Insert PC##bm_add_pc" ),
                             ImVec2 ( -FLT_MIN, 0.0f ) ) ) {
            if ( g_mzarch_main.cpu ) {
                uint16_t pc = g_mzarch_main.cpu->pc;
                snprintf ( g_bm.add_input_buf, sizeof ( g_bm.add_input_buf ),
                           "#%04X", pc );
            };
        };
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Insert current PC as #XXXX" ) );
        };

        /* Řádek 2: Comment: [input] [Add] */
        ImGui::TableNextRow ( );
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _( "Comment:" ) );

        ImGui::TableSetColumnIndex ( 1 );
        ImGui::SetNextItemWidth ( -FLT_MIN );
        ImGui::InputText ( "##bm_add_comment",
                            g_bm.add_comment_buf,
                            sizeof ( g_bm.add_comment_buf ) );

        ImGui::TableSetColumnIndex ( 3 );
        ImGui::BeginDisabled ( input_empty );
        if ( ImGui::Button ( _L ( "Add##bm_add_btn" ),
                             ImVec2 ( -FLT_MIN, 0.0f ) ) ) {
            bookmarks_add ( g_bm.add_input_buf, g_bm.add_comment_buf );
            g_bm.add_input_buf[0]   = '\0';
            g_bm.add_comment_buf[0] = '\0';
            /* Sjednoceno s Vars/Symbols: po úspěšném přidání zavřít form. */
            g_bm.add_form_open = false;
        };
        ImGui::EndDisabled ( );

        ImGui::EndTable ( );
    };

    ImGui::EndGroup ( );
    ImGui::Unindent ( );
}


/**
 * @brief Render jednoho řádku v tabulce.
 *
 * Sloupec layout:
 *   - 0: per-row select checkbox
 *   - 1: [šipka] [...] tlačítka + Label text (žádný hover/click handler
 *        na samotném textu - akce jen přes tlačítka)
 *   - 2: $address (plain Text / TextDisabled, žádné kliky)
 *   - 3: Comment (Text nebo InputText v edit režimu)
 *   - 4: Actions (Edit/Del nebo Apply/Cancel)
 *
 * Edit režim (in_edit == true):
 *   - Sloupec 1 + 3 mají InputText (s ImGuiInputTextFlags_EnterReturnsTrue)
 *   - Enter v jakémkoli InputText = Apply (= save changes)
 *   - Escape v rámci edit row = Cancel (revert + exit edit)
 *
 * @param bm             Pointer do storage (lifetime jen v rámci frame).
 * @param pending_remove Pokud po návratu *pending_remove > 0, řádek se má smazat.
 * @param pending_break  Pokud po návratu *pending_break == true, breakni
 *                       iteraci (= struktura storage byla mutována, indexy
 *                       jsou neaktuální).
 */
static void bm_render_row ( const bookmark_t *bm,
                            uint32_t *pending_remove,
                            bool *pending_break ) {
    ImGui::PushID ( (int) bm->id );

    /* Resolve adresy */
    uint16_t addr = 0;
    bool addr_valid = bookmarks_resolve_addr ( bm->user_input, &addr );

    /* Selection checkbox */
    ImGui::TableSetColumnIndex ( 0 );
    bool sel = g_bm.selected_ids.count ( bm->id ) > 0;
    if ( ImGui::Checkbox ( "###bm_sel", &sel ) ) {
        if ( sel ) g_bm.selected_ids.insert ( bm->id );
        else       g_bm.selected_ids.erase  ( bm->id );
    };

    /* V1.C.3 - Owner badge sloupec. */
    ImGui::TableSetColumnIndex ( 1 );
    owner_badge_render ( bm->cmd_origin );

    bool in_edit = ( g_bm.edit_id == bm->id );

    /* Apply / Cancel akce řízené Enter / Escape v edit režimu - shromáždíme
     * z obou InputText polí, akci provedeme až po vykreslení obou. */
    bool edit_apply  = false;
    bool edit_cancel = false;

    /* Label sloupec - tlačítka [šipka] [...] + plain text label. */
    ImGui::TableSetColumnIndex ( 2 );
    if ( in_edit ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( ImGui::InputText ( "###bm_edit_input",
                                  g_bm.edit_input_buf,
                                  sizeof ( g_bm.edit_input_buf ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            edit_apply = true;
        };
    } else {
        /* Sjednoceny tlačítko "> ..." - kombinuje primary focus (left click)
         * a popup "Show in" (right click). Pokud addr neni resolvable,
         * tlacitko je disabled. */
        ImGui::BeginDisabled ( !addr_valid );
        if ( ImGui::SmallButton ( "> ...###bm_focus_btn" ) ) {
            dbg_disasm_show_in_slot ( 0, addr );
        };
        /* RMB na stejnem tlacitku - otevre popup. Flag se nastavi tady,
         * skutecny OpenPopup probehne mimo PushID stack na konci
         * bm_window_render (= aby BeginPopup v bm_render_rmb_popup nasel
         * shodne ID). */
        if ( addr_valid && ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_bm.rmb_popup_addr = addr;
            g_bm.rmb_popup_open = true;
        };
        ImGui::EndDisabled ( );
        if ( ImGui::IsItemHovered ( ) && addr_valid ) {
            ImGui::SetTooltip ( "%s",
                _( "Focus in primary disassembly. "
                   "Right-click for additional actions." ) );
        };

        ImGui::SameLine ( );

        /* Klikatelny label - hover s podtrzenim, klik = enter edit mode. */
        char label_buf[128];
        bm_compute_label ( bm->user_input, label_buf, sizeof ( label_buf ) );
        ImVec2 lbl_pos = ImGui::GetCursorScreenPos ( );
        if ( !addr_valid ) {
            ImGui::TextDisabled ( "%s", label_buf );
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::SetTooltip ( "%s",
                    _( "Cannot resolve address (symbol not found "
                       "and not a hex literal)" ) );
            };
        } else {
            ImGui::TextUnformatted ( label_buf );
            if ( ImGui::IsItemHovered ( ) ) {
                /* Underline aby uzivatel videl ze je to klikatelne. */
                ImVec2 sz = ImGui::CalcTextSize ( label_buf );
                ImGui::GetWindowDrawList ( )->AddLine (
                    ImVec2 ( lbl_pos.x, lbl_pos.y + sz.y ),
                    ImVec2 ( lbl_pos.x + sz.x, lbl_pos.y + sz.y ),
                    ImGui::GetColorU32 ( ImGuiCol_Text ), 1.0f );
                ImGui::SetTooltip ( "%s", _( "Click to edit" ) );
                if ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) ) {
                    g_bm.edit_id = bm->id;
                    snprintf ( g_bm.edit_input_buf,
                               sizeof ( g_bm.edit_input_buf ),
                               "%s", bm->user_input ? bm->user_input : "" );
                    snprintf ( g_bm.edit_comment_buf,
                               sizeof ( g_bm.edit_comment_buf ),
                               "%s", bm->comment ? bm->comment : "" );
                };
            };
        };
    };

    /* Address sloupec - jen plain text, žádné kliky. */
    ImGui::TableSetColumnIndex ( 3 );
    if ( addr_valid ) {
        ImGui::Text ( "$%04X", addr );
    } else {
        ImGui::TextDisabled ( "%s", "----" );
    };

    /* Comment sloupec */
    ImGui::TableSetColumnIndex ( 4 );
    if ( in_edit ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( ImGui::InputText ( "###bm_edit_cmnt",
                                  g_bm.edit_comment_buf,
                                  sizeof ( g_bm.edit_comment_buf ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            edit_apply = true;
        };
    } else {
        const char *cmnt = bm->comment ? bm->comment : "";
        bool empty = !cmnt[0];
        ImVec2 c_pos = ImGui::GetCursorScreenPos ( );
        if ( empty )
            ImGui::TextDisabled ( "%s", _( "(no comment)" ) );
        else
            ImGui::TextUnformatted ( cmnt );
        if ( ImGui::IsItemHovered ( ) ) {
            /* Underline aby uzivatel videl ze je to klikatelne. */
            const char *shown = empty ? _( "(no comment)" ) : cmnt;
            ImVec2 sz = ImGui::CalcTextSize ( shown );
            ImGui::GetWindowDrawList ( )->AddLine (
                ImVec2 ( c_pos.x, c_pos.y + sz.y ),
                ImVec2 ( c_pos.x + sz.x, c_pos.y + sz.y ),
                ImGui::GetColorU32 ( ImGuiCol_Text ), 1.0f );
            ImGui::SetTooltip ( "%s", _( "Click to edit" ) );
            if ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) ) {
                g_bm.edit_id = bm->id;
                snprintf ( g_bm.edit_input_buf,
                           sizeof ( g_bm.edit_input_buf ),
                           "%s", bm->user_input ? bm->user_input : "" );
                snprintf ( g_bm.edit_comment_buf,
                           sizeof ( g_bm.edit_comment_buf ),
                           "%s", bm->comment ? bm->comment : "" );
            };
        };
    };

    /* Actions sloupec */
    ImGui::TableSetColumnIndex ( 5 );
    if ( in_edit ) {
        if ( ImGui::SmallButton ( _L ( "OK##bm_apply" ) ) ) {
            edit_apply = true;
        };
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Apply changes (Enter)." ) );
        };
        ImGui::SameLine ( 0.0f, 2.0f );
        if ( ImGui::SmallButton ( _L ( "Back##bm_cancel" ) ) ) {
            edit_cancel = true;
        };
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Cancel edit, revert to original (Escape)." ) );
        };

        /* Escape detekce - jen pokud je edit row aktivní (= ne globální
         * shortcut). IsKeyPressed bez focus testu by chytlo Escape
         * kdekoli; omezíme na případy, kdy je některý z edit InputText
         * focused. ImGui IsAnyItemFocused() po vykreslení obou inputů
         * (viz výše) detekuje, jestli edit field má focus. */
        if ( !edit_cancel && ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) ) {
            edit_cancel = true;
        };
    } else {
        /* "x" delete button - sjednoceno s Watch/Symbols/Variables.
         * Editaci řádku spouští klik na label nebo komentar. */
        if ( ImGui::SmallButton ( _L ( "x##bm_del" ) ) ) {
            *pending_remove = bm->id;
        };
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Remove this bookmark." ) );
        };
    };

    /* Aplikuj edit akce. Apply má prioritu nad Cancel pokud někdo
     * stiskl Enter a Escape ve stejném frame (= unlikely). */
    if ( edit_apply ) {
        bookmarks_set_user_input ( bm->id, g_bm.edit_input_buf );
        bookmarks_set_comment    ( bm->id, g_bm.edit_comment_buf );
        g_bm.edit_id = 0;
        *pending_break = true;
    } else if ( edit_cancel ) {
        g_bm.edit_id = 0;
    };

    ImGui::PopID ( );
}


/**
 * @brief Render RMB context popup "Show in Disassembly #1..#5".
 */
static void bm_render_rmb_popup ( void ) {
    if ( ImGui::BeginPopup ( "bm_show_in_popup" ) ) {
        for ( int slot = 0; slot < 5; slot++ ) {
            char label[64];
            if ( slot == 0 ) {
                snprintf ( label, sizeof ( label ),
                           "%s", _( "Show in Disassembly (main)" ) );
            } else {
                snprintf ( label, sizeof ( label ),
                           "%s #%d", _( "Show in Disassembly" ), slot + 1 );
            };
            if ( ImGui::MenuItem ( label ) ) {
                dbg_disasm_show_in_slot ( slot, g_bm.rmb_popup_addr );
            };
        };
        ImGui::EndPopup ( );
    };
}


/**
 * @brief Render file dialogů.
 */
static void bm_render_file_dialogs ( void ) {
    ImVec2 minSize ( 480, 320 );

    if ( ImGuiFileDialog::Instance ( )->Display ( "BookmarksSaveDialog",
                                                    ImGuiWindowFlags_None, minSize ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            if ( !bookmarks_save ( path.c_str ( ) ) ) {
                fprintf ( stderr, "BOOKMARKS: save to '%s' failed\n", path.c_str ( ) );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    if ( ImGuiFileDialog::Instance ( )->Display ( "BookmarksLoadDialog",
                                                    ImGuiWindowFlags_None, minSize ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            if ( !bookmarks_load ( path.c_str ( ) ) ) {
                fprintf ( stderr, "BOOKMARKS: load from '%s' failed\n", path.c_str ( ) );
            };
            g_bm.selected_ids.clear ( );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    if ( ImGuiFileDialog::Instance ( )->Display ( "BookmarksMergeDialog",
                                                    ImGuiWindowFlags_None, minSize ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            if ( !bookmarks_merge ( path.c_str ( ) ) ) {
                fprintf ( stderr, "BOOKMARKS: merge from '%s' failed\n", path.c_str ( ) );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };
}


/**
 * @brief Render table headeru s custom 3-state select-all checkboxem
 *        ve sloupci 0.
 *
 * Místo standardního TableHeadersRow renderujeme řádek manuálně:
 *   - Sloupec 0: tristate checkbox (none/some/all visible selected)
 *   - Sloupce 1-4: standardní TableHeader z TableSetupColumn
 *
 * Toggle akce na klik:
 *   - all  -> deselect all visible
 *   - none -> select all visible
 *   - some -> select all visible (= rozumnější default než deselect)
 *
 * Kde "visible" = řádky, které prošly aktuálním filtrem.
 *
 * @param total_count Celkový počet záznamů ve storage (pro iteraci).
 */
static void bm_render_table_header ( size_t total_count ) {
    /* Spočítej visible + visible_selected pro tristate stav. */
    size_t visible = 0;
    size_t visible_selected = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const bookmark_t *bm = bookmarks_get_by_index ( i );
        if ( !bm_filter_match ( bm ) ) continue;
        visible++;
        if ( g_bm.selected_ids.count ( bm->id ) > 0 ) visible_selected++;
    };

    bool all_selected  = ( visible > 0 && visible_selected == visible );
    bool none_selected = ( visible_selected == 0 );

    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );

    /* Sloupec 0: custom tristate checkbox přes InvisibleButton + DrawList.
     * ImGui nemá nativní tristate, vyrobíme ručně:
     *   - none = prázdný čtvereček (jen background)
     *   - all  = standardní checkmark (visual shoda s ImGui::Checkbox)
     *   - some = vyplněný menší čtvereček uprostřed (= "indeterminate")
     * Vzor: src/ui-imgui/debugger/vars/vars_window.cpp - vars_render_table_header.
     */
    ImGui::TableSetColumnIndex ( 0 );
    ImGui::PushID ( "bm_selall" );
    {
        float h = ImGui::GetFrameHeight ( );
        ImVec2 size ( h, h );
        ImVec2 pos = ImGui::GetCursorScreenPos ( );
        ImGui::InvisibleButton ( "###bm_selall_btn", size );
        bool clicked = ImGui::IsItemClicked ( );
        bool hovered = ImGui::IsItemHovered ( );
        bool active  = ImGui::IsItemActive ( );

        ImDrawList *dl = ImGui::GetWindowDrawList ( );
        ImU32 bg_col = ImGui::GetColorU32 (
            active ? ImGuiCol_FrameBgActive
                   : hovered ? ImGuiCol_FrameBgHovered
                             : ImGuiCol_FrameBg );
        ImVec2 br ( pos.x + size.x, pos.y + size.y );
        dl->AddRectFilled ( pos, br, bg_col, 3.0f );

        ImU32 mark_col = ImGui::GetColorU32 ( ImGuiCol_CheckMark );
        if ( all_selected ) {
            /* Checkmark per ImGui::RenderCheckMark algoritmus. */
            ImVec2 fp = ImGui::GetStyle ( ).FramePadding;
            float square_sz = h - fp.y * 2.0f;
            ImVec2 mark_pos ( pos.x + fp.y, pos.y + fp.y );
            float thickness = ( square_sz / 5.0f );
            if ( thickness < 1.0f ) thickness = 1.0f;
            float sz_inner = square_sz - thickness * 0.5f;
            ImVec2 origin ( mark_pos.x + thickness * 0.25f,
                             mark_pos.y + thickness * 0.25f );
            float third = sz_inner / 3.0f;
            float bx = origin.x + third;
            float by = origin.y + sz_inner - third * 0.5f;
            dl->PathLineTo ( ImVec2 ( bx - third, by - third ) );
            dl->PathLineTo ( ImVec2 ( bx, by ) );
            dl->PathLineTo ( ImVec2 ( bx + third * 2.0f, by - third * 2.0f ) );
            dl->PathStroke ( mark_col, ImDrawFlags_None, thickness );
        } else if ( !none_selected ) {
            /* Indeterminate = vyplněný menší čtvereček uprostřed. */
            float pad = h * 0.28f;
            ImVec2 inner_min ( pos.x + pad,     pos.y + pad );
            ImVec2 inner_max ( pos.x + h - pad, pos.y + h - pad );
            dl->AddRectFilled ( inner_min, inner_max, mark_col, 1.5f );
        };

        if ( clicked ) {
            if ( all_selected ) {
                /* Vše vybráno -> odznačit. */
                for ( size_t i = 0; i < total_count; i++ ) {
                    const bookmark_t *bm = bookmarks_get_by_index ( i );
                    if ( !bm_filter_match ( bm ) ) continue;
                    g_bm.selected_ids.erase ( bm->id );
                };
            } else {
                /* Some/none -> označit vše visible. */
                for ( size_t i = 0; i < total_count; i++ ) {
                    const bookmark_t *bm = bookmarks_get_by_index ( i );
                    if ( !bm_filter_match ( bm ) ) continue;
                    g_bm.selected_ids.insert ( bm->id );
                };
            };
        };
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) {
        if ( all_selected ) {
            ImGui::SetTooltip ( "%s",
                _( "Deselect all visible bookmarks." ) );
        } else if ( none_selected ) {
            ImGui::SetTooltip ( "%s",
                _( "Select all visible bookmarks." ) );
        } else {
            ImGui::SetTooltip ( "%s",
                _( "Some bookmarks selected. Click to select all visible." ) );
        };
    };
    ImGui::PopID ( );

    /* Sloupce 1-5: standardní hlavičky (V1.C.3 - +1 = Owner sloupec). */
    for ( int col = 1; col <= 5; col++ ) {
        ImGui::TableSetColumnIndex ( col );
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
    };
}


/* ========================================================================= */
/*  Main render                                                              */
/* ========================================================================= */


extern "C" void bm_window_render ( bool *p_open ) {
    if ( !p_open || !*p_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 705, 531 ), ImGuiCond_FirstUseEver );

    /* Stabilní ID přes ###. Auto-layout při fresh open - cache _L(). */
    const char *bm_title = _L ( "Bookmarks###bm_window" );
    auto_layout_first_use_portrait ( bm_title, 705.0f, 531.0f );
    if ( !ImGui::Begin ( bm_title, p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    };

    /* ---- 1) sticky header ---- */

    /* Spočti filtered count přes helper bm_filter_match. */
    size_t total_count = bookmarks_count ( );
    size_t filtered_count = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const bookmark_t *bm = bookmarks_get_by_index ( i );
        if ( bm_filter_match ( bm ) ) filtered_count++;
    };

    bm_render_header ( total_count, filtered_count );

    /* ---- 2) Add form ---- */
    bm_render_add_form ( );

    /* ---- 3) Tabulka ---- */

    ImGui::SeparatorText ( _( "Bookmarks" ) );

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_Resizable;

    if ( ImGui::BeginTable ( "##bm_table", 6, table_flags ) ) {
        ImGui::TableSetupScrollFreeze ( 0, 1 );
        ImGui::TableSetupColumn ( "##bm_sel",     ImGuiTableColumnFlags_WidthFixed,
                                  ImGui::GetFontSize ( ) * 1.5f );
        /* V1.C.3 - Owner badge sloupec. */
        ImGui::TableSetupColumn ( _L( "Own##bm_col_owner" ),
                                  ImGuiTableColumnFlags_WidthFixed,
                                  ImGui::GetFontSize ( ) * 2.5f );
        ImGui::TableSetupColumn ( _( "Label" ),   ImGuiTableColumnFlags_WidthStretch,
                                  3.0f );
        ImGui::TableSetupColumn ( _( "Address" ), ImGuiTableColumnFlags_WidthFixed,
                                  ImGui::GetFontSize ( ) * 5.0f );
        ImGui::TableSetupColumn ( _( "Comment" ), ImGuiTableColumnFlags_WidthStretch,
                                  4.0f );
        /* Actions sloupec - nadpis prázdný (sjednoceno s Watch/Symbols/Vars).
         * Šířka snížena, protože "x" delete tlačítko je menší než původní "Del". */
        ImGui::TableSetupColumn ( "##bm_actions", ImGuiTableColumnFlags_WidthFixed,
                                  ImGui::GetFontSize ( ) * 4.0f );

        /* Custom hlavička s tristate select-all checkboxem ve sloupci 0. */
        bm_render_table_header ( total_count );

        uint32_t pending_remove = 0;
        bool     pending_break  = false;

        /* V1.E.6.A: pending focus z bm_window_focus_id() - najdi řádek a
         * scrollni. Spotřebovat se musí jednorázově a poté reset. */
        uint32_t focus_id_local = g_bm.pending_focus_id;
        g_bm.pending_focus_id = 0;

        for ( size_t i = 0; i < total_count; i++ ) {
            const bookmark_t *bm = bookmarks_get_by_index ( i );
            if ( !bm_filter_match ( bm ) ) continue;

            ImGui::TableNextRow ( );
            bm_render_row ( bm, &pending_remove, &pending_break );

            /* V1.E.6.A: pokud řádek odpovídá focus targetu, scroll + select. */
            if ( focus_id_local != 0 && bm && bm->id == focus_id_local ) {
                ImGui::SetScrollHereY ( 0.5f );
                g_bm.selected_ids.clear ( );
                g_bm.selected_ids.insert ( focus_id_local );
                focus_id_local = 0;
            };

            if ( pending_break ) break;
        };

        ImGui::EndTable ( );

        if ( pending_remove ) {
            bookmarks_remove ( pending_remove );
            g_bm.selected_ids.erase ( pending_remove );
        };
    };

    /* ---- 4) RMB popup ----
     * OpenPopup musi byt volane MIMO PushID stack per-radek, jinak se
     * popup ID hashuje s row id a BeginPopup nasel by jine ID. Flag
     * rmb_popup_open je nastaven v render row pri RMB na "> ..." tlacitku;
     * tady ho zpracujeme v parent scope. */
    if ( g_bm.rmb_popup_open ) {
        ImGui::OpenPopup ( "bm_show_in_popup" );
        g_bm.rmb_popup_open = false;
    };
    bm_render_rmb_popup ( );

    /* ---- 5) File dialogy ---- */
    bm_render_file_dialogs ( );

    ImGui::End ( );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
