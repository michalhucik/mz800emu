/*
 * sym_window.cpp - Symbol Browser panel (D.8.6 + V1.5.D rework dle
 * Variables vzoru, Michal feedback 2026-05-06).
 *
 * Layout:
 *   +-------------------------------------------------------------------+
 *   | Symbols                                                      [X]  |
 *   +-------------------------------------------------------------------+
 *   | Sticky header (2 řádky):                                          |
 *   |   [+ Add]  Search:[__________] [Clear]  (vis/tot symbols)         |
 *   |   Selected: N  [Delete]  | [Load From...] [Save .lbl As...]       |
 *   |                            [Clear All]                            |
 *   +-------------------------------------------------------------------+
 *   | (Add Label collapsible block - jen když [+ Add] je aktivní)      |
 *   |   Addr (hex):[___]  Name:[__________]                            |
 *   |   Comment:[___________________________] [OK] [Cancel]             |
 *   |   (inline error)                                                  |
 *   +-------------------------------------------------------------------+
 *   | [v] | Name      | Addr   | Bank | Source | Comment       | x |   |
 *   |-----|-----------|--------|------|--------|---------------|---|   |
 *   | [ ] | wboot     | 0xD8A2 |  0   |  NOI   | (none)        | x |   |
 *   | [ ] | print_ch  | 0x4042 |  0   |  LBL   | print one ch  | x |   |
 *   +-------------------------------------------------------------------+
 *
 * Rozdíly oproti V1 sym_window:
 *   - Sticky header (toolbar + bulk + file ops) vždy nahoře
 *   - Add Label form jako collapsible block POD sticky header
 *   - Sel sloupec s tristate select-all checkboxem
 *   - Bulk Delete (přes selection set)
 *   - Per-row Delete x sloupec (rychlý workflow bez confirm)
 *   - Inline edit Name (= rename via remove + add_user_label, auto-LBL)
 *   - Inline edit Addr (= relocation via remove + add_user_label)
 *   - Inline edit Comment zachován (auto-promote LBL)
 *   - Tooltipy přes IsItemHovered + SetTooltip (žádné "(?)" markery)
 *   - Filter match na Name + Comment + Addr (formát "0x40")
 *   - Anglické headery natvrdo (= bypass cs překlad "Název")
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include "sym_window.h"
#include "ui-imgui/mcp_activity/owner_badge.h"  /* V1.C.3 owner badge */

#include <set>
#include <string>
#include <vector>

extern "C" {
#include "emulator/debugger/symbols/sym_db.h"
}


/* ========================================================================= */
/*  Persistent UI state                                                       */
/* ========================================================================= */


namespace {

struct SymUiState {
    /* Filter (case-insensitive na name + comment + addr-hex). */
    char filter[ 128 ] = "";

    /* Add-form expand. */
    bool add_form_open = false;
    char add_addr[ 16 ]      = "";
    char add_name[ 64 ]      = "";
    char add_comment[ 256 ]  = "";
    char add_error[ 200 ]    = "";

    /* Inline edit. */
    enum class EditField { None, Name, Addr, Comment };
    std::string editing_name;          /* prázdný = no edit */
    EditField   editing_field = EditField::None;
    char        edit_buffer[ 256 ] = "";
    bool        edit_focus_pending = false;
    char        edit_error[ 200 ] = "";

    /* Selection (jména vybraných řádků). */
    std::set<std::string> selected;

    /* Pending dialogy. */
    bool show_clear_all_confirm = false;
    bool show_delete_selected_confirm = false;

    /* Status (= zpráva o load/save výsledku). */
    char status[ 256 ] = "";

    /* V1.E.6.A: pending focus request z Activity okna (= dvojklik routing).
     * pending_focus_active=true = render-loop má najít první symbol s
     * addr == pending_focus_addr a scrollnout na něj + označit selected.
     * Po spotřebě se appearance vrátí na false. */
    bool     pending_focus_active = false;
    uint32_t pending_focus_addr   = 0;
};

}  /* anonymous namespace */

static SymUiState g_su;


/* ========================================================================= */
/*  Helpery                                                                   */
/* ========================================================================= */


/**
 * @brief Tooltip pro předchozí ImGui prvek.
 */
static void sym_item_tooltip ( const char *desc )
{
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 35.0f );
        ImGui::TextUnformatted ( desc );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    };
}


/**
 * @brief Vrátí krátký textový popis source enum.
 */
static const char *sym_source_label ( en_SYM_SOURCE src )
{
    switch ( src ) {
        case SYM_SOURCE_LBL:        return "LBL";
        case SYM_SOURCE_MAP:        return "MAP";
        case SYM_SOURCE_NOI:        return "NOI";
        case SYM_SOURCE_SJASMPLUS:  return "SYM";
        default:                    return "??";
    };
}


/**
 * @brief Case-insensitive substring match.
 */
static bool sym_str_contains_ci ( const char *haystack, const char *needle )
{
    if ( !needle || !*needle ) return true;
    if ( !haystack ) return false;
    size_t hl = strlen ( haystack );
    size_t nl = strlen ( needle );
    if ( nl > hl ) return false;
    for ( size_t i = 0; i + nl <= hl; i++ ) {
        size_t j = 0;
        for ( ; j < nl; j++ ) {
            char a = haystack[ i + j ];
            char b = needle[ j ];
            if ( a >= 'A' && a <= 'Z' ) a = (char) ( a - 'A' + 'a' );
            if ( b >= 'A' && b <= 'Z' ) b = (char) ( b - 'A' + 'a' );
            if ( a != b ) break;
        };
        if ( j == nl ) return true;
    };
    return false;
}


/**
 * @brief Filter match: name + comment + addr-hex substring.
 */
static bool sym_filter_match ( const st_SYMBOL *s )
{
    if ( !g_su.filter[ 0 ] ) return true;
    if ( sym_str_contains_ci ( s->name, g_su.filter ) ) return true;
    if ( s->comment && sym_str_contains_ci ( s->comment, g_su.filter ) ) return true;
    /* Addr-hex match: "0x4042" / "4042" / "40" substring v hex stringu. */
    char addr_str[ 16 ];
    snprintf ( addr_str, sizeof ( addr_str ), "0x%04X", (unsigned) s->addr );
    if ( sym_str_contains_ci ( addr_str, g_su.filter ) ) return true;
    return false;
}


/**
 * @brief Parse hex string ("4042" / "0x4042" / "#4042") → uint32_t.
 *
 * @return true při úspěchu.
 */
static bool sym_parse_hex_addr ( const char *input, uint32_t *out )
{
    if ( !input || !out ) return false;
    while ( *input == ' ' || *input == '\t' ) input++;
    if ( !*input ) return false;

    const char *p = input;
    if ( p[ 0 ] == '#' ) p++;
    else if ( p[ 0 ] == '0' && ( p[ 1 ] == 'x' || p[ 1 ] == 'X' ) ) p += 2;

    if ( !*p ) return false;

    uint32_t val = 0;
    while ( *p ) {
        char c = *p;
        if ( c == ' ' || c == '\t' ) break;
        int digit;
        if ( c >= '0' && c <= '9' ) digit = c - '0';
        else if ( c >= 'a' && c <= 'f' ) digit = 10 + ( c - 'a' );
        else if ( c >= 'A' && c <= 'F' ) digit = 10 + ( c - 'A' );
        else return false;
        if ( val > ( 0xFFFFFFFFu - (uint32_t) digit ) / 16u ) return false;
        val = val * 16 + (uint32_t) digit;
        p++;
    };
    *out = val;
    return true;
}


/**
 * @brief Validuje symbol name (= identifier-like).
 *
 * Akceptuje: písmena, číslice, underscore, dot (.), apostrof (').
 * První znak nesmí být číslice. Max 63 znaků.
 */
static bool sym_name_is_valid ( const char *name )
{
    if ( !name || !*name ) return false;
    size_t len = strlen ( name );
    if ( len > 63 ) return false;
    char c0 = name[ 0 ];
    if ( !( ( c0 >= 'a' && c0 <= 'z' ) || ( c0 >= 'A' && c0 <= 'Z' )
            || c0 == '_' || c0 == '.' ) ) return false;
    for ( size_t i = 1; i < len; i++ ) {
        char c = name[ i ];
        bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' )
                  || ( c >= '0' && c <= '9' )
                  || c == '_' || c == '.' || c == '\'';
        if ( !ok ) return false;
    };
    return true;
}


/* ========================================================================= */
/*  Add form helpers                                                          */
/* ========================================================================= */


static void sym_add_form_reset ( void )
{
    g_su.add_addr[ 0 ] = '\0';
    g_su.add_name[ 0 ] = '\0';
    g_su.add_comment[ 0 ] = '\0';
    g_su.add_error[ 0 ] = '\0';
}


/**
 * @brief Submit Add Label formuláře.
 *
 * Validace: addr (hex), name (regex), duplicate check.
 *
 * @return true při úspěchu.
 */
static bool sym_add_form_submit ( void )
{
    g_su.add_error[ 0 ] = '\0';

    uint32_t addr = 0;
    if ( !sym_parse_hex_addr ( g_su.add_addr, &addr ) ) {
        snprintf ( g_su.add_error, sizeof ( g_su.add_error ),
                   "%s", _("Invalid address (use hex 4042 / 0x4042 / #4042)") );
        return false;
    };

    if ( !sym_name_is_valid ( g_su.add_name ) ) {
        snprintf ( g_su.add_error, sizeof ( g_su.add_error ),
                   "%s", _("Invalid name (letters/digits/_/./',  no leading digit)") );
        return false;
    };

    /* Duplicate check po jménu (sym_db_add_user_label by overwrote, ale UX
     * lepší explicit error). */
    if ( sym_db_lookup_by_name ( g_su.add_name ) ) {
        snprintf ( g_su.add_error, sizeof ( g_su.add_error ),
                   _("Symbol '%s' already exists"), g_su.add_name );
        return false;
    };

    const char *cmt = ( g_su.add_comment[ 0 ] != '\0' )
                      ? g_su.add_comment : NULL;
    if ( sym_db_add_user_label ( addr, g_su.add_name, cmt ) != 0 ) {
        snprintf ( g_su.add_error, sizeof ( g_su.add_error ),
                   "%s", _("Failed to add symbol (sym_db error)") );
        return false;
    };

    sym_add_form_reset ( );
    return true;
}


/* ========================================================================= */
/*  Inline edit helpers                                                       */
/* ========================================================================= */


static void sym_begin_edit_name ( const st_SYMBOL *s )
{
    g_su.editing_name = s->name;
    g_su.editing_field = SymUiState::EditField::Name;
    snprintf ( g_su.edit_buffer, sizeof ( g_su.edit_buffer ),
               "%s", s->name );
    g_su.edit_focus_pending = true;
    g_su.edit_error[ 0 ] = '\0';
}


static void sym_begin_edit_addr ( const st_SYMBOL *s )
{
    g_su.editing_name = s->name;
    g_su.editing_field = SymUiState::EditField::Addr;
    snprintf ( g_su.edit_buffer, sizeof ( g_su.edit_buffer ),
               "0x%04X", (unsigned) s->addr );
    g_su.edit_focus_pending = true;
    g_su.edit_error[ 0 ] = '\0';
}


static void sym_begin_edit_comment ( const st_SYMBOL *s )
{
    g_su.editing_name = s->name;
    g_su.editing_field = SymUiState::EditField::Comment;
    snprintf ( g_su.edit_buffer, sizeof ( g_su.edit_buffer ),
               "%s", s->comment ? s->comment : "" );
    g_su.edit_focus_pending = true;
    g_su.edit_error[ 0 ] = '\0';
}


static void sym_cancel_edit ( void )
{
    g_su.editing_name.clear ( );
    g_su.editing_field = SymUiState::EditField::None;
    g_su.edit_buffer[ 0 ] = '\0';
    g_su.edit_error[ 0 ] = '\0';
}


/**
 * @brief Aplikuje rename (= remove old + add_user_label new).
 *
 * Pro non-LBL source vzniká nový LBL alias na stejnou addr (= starý
 * import-driven symbol zůstane s původním jménem).
 *
 * @return true při úspěchu.
 */
static bool sym_apply_rename ( const char *old_name, const char *new_name )
{
    if ( strcmp ( old_name, new_name ) == 0 ) return true;  /* no-op */

    if ( !sym_name_is_valid ( new_name ) ) {
        snprintf ( g_su.edit_error, sizeof ( g_su.edit_error ),
                   "%s", _("Invalid name") );
        return false;
    };
    if ( sym_db_lookup_by_name ( new_name ) ) {
        snprintf ( g_su.edit_error, sizeof ( g_su.edit_error ),
                   _("Name '%s' already exists"), new_name );
        return false;
    };

    const st_SYMBOL *s = sym_db_lookup_by_name ( old_name );
    if ( !s ) return false;

    /* Snapshot. */
    uint32_t addr = s->addr;
    char *cmt_copy = s->comment ? g_strdup ( s->comment ) : NULL;
    en_SYM_SOURCE src = s->source;

    sym_db_add_user_label ( addr, new_name, cmt_copy );
    if ( src == SYM_SOURCE_LBL ) {
        sym_db_remove_user_label ( old_name );
    };
    g_free ( cmt_copy );

    /* Selection update: pokud old name byl selected, nová varianta též. */
    if ( g_su.selected.erase ( old_name ) ) {
        g_su.selected.insert ( new_name );
    };
    return true;
}


/**
 * @brief Aplikuje addr change (= remove old + add_user_label nový s novým addr).
 */
static bool sym_apply_addr_change ( const char *name, uint32_t new_addr )
{
    const st_SYMBOL *s = sym_db_lookup_by_name ( name );
    if ( !s ) return false;
    if ( s->addr == new_addr ) return true;  /* no-op */

    char *cmt_copy = s->comment ? g_strdup ( s->comment ) : NULL;
    en_SYM_SOURCE src = s->source;

    /* sym_db_add_user_label overwrite same name → ale jen pokud LBL
     * source (per docs). Pro non-LBL musíme remove first, ale
     * remove_user_label nemůže smazat non-LBL → pak add overwrites.
     * Test simply: add_user_label ho přepíše na LBL s novou addr. */
    sym_db_add_user_label ( new_addr, name, cmt_copy );
    g_free ( cmt_copy );
    (void) src;
    return true;
}


/**
 * @brief Aplikuje inline edit podle field type.
 *
 * @return true pokud apply OK (= edit state se vyčistí).
 */
static bool sym_apply_edit ( void )
{
    if ( g_su.editing_name.empty ( ) ) return true;

    std::string old_name = g_su.editing_name;
    SymUiState::EditField field = g_su.editing_field;

    if ( field == SymUiState::EditField::Name ) {
        if ( !sym_apply_rename ( old_name.c_str ( ), g_su.edit_buffer ) ) {
            return false;
        };
    } else if ( field == SymUiState::EditField::Addr ) {
        uint32_t new_addr = 0;
        if ( !sym_parse_hex_addr ( g_su.edit_buffer, &new_addr ) ) {
            snprintf ( g_su.edit_error, sizeof ( g_su.edit_error ),
                       "%s", _("Invalid address") );
            return false;
        };
        if ( !sym_apply_addr_change ( old_name.c_str ( ), new_addr ) ) {
            return false;
        };
    } else if ( field == SymUiState::EditField::Comment ) {
        sym_db_set_comment ( old_name.c_str ( ), g_su.edit_buffer );
    };

    sym_cancel_edit ( );
    return true;
}


/* ========================================================================= */
/*  Tristate select-all checkbox                                              */
/* ========================================================================= */


/**
 * @brief Spočítá kolik visible symbolů je selected.
 */
static void sym_count_selected_visible ( size_t *out_visible_total,
                                          size_t *out_visible_selected )
{
    size_t total = sym_db_count ( );
    size_t vis_total = 0;
    size_t vis_sel = 0;
    for ( size_t i = 0; i < total; i++ ) {
        const st_SYMBOL *s = sym_db_get_by_index ( i );
        if ( !s || !s->name ) continue;
        if ( !sym_filter_match ( s ) ) continue;
        vis_total++;
        if ( g_su.selected.count ( s->name ) ) vis_sel++;
    };
    if ( out_visible_total ) *out_visible_total = vis_total;
    if ( out_visible_selected ) *out_visible_selected = vis_sel;
}


/**
 * @brief Render tristate select-all checkbox v Sel column header.
 *
 * Visual (sjednoceno s Watch / Variables / Bookmarks):
 *   - none (0 vybráno)        → prázdný čtvereček
 *   - all  (vše visible)      → V-checkmark per ImGui::RenderCheckMark
 *   - some (subset)           → vyplněný menší čtvereček uprostřed
 *
 * Klik:
 *   - none / some → vybere všechny visible
 *   - all         → odznačí všechny visible
 *
 * Implementace: InvisibleButton první, pak background s hover/active
 * feedback (= shoda s ImGui::Checkbox).
 */
static void sym_render_select_all ( void )
{
    size_t vis_total = 0, vis_sel = 0;
    sym_count_selected_visible ( &vis_total, &vis_sel );

    bool all_selected  = ( vis_total > 0 && vis_sel == vis_total );
    bool none_selected = ( vis_sel == 0 );

    ImGui::PushID ( "sym_selall" );
    float h = ImGui::GetFrameHeight ( );
    ImVec2 size ( h, h );
    ImVec2 pos = ImGui::GetCursorScreenPos ( );
    ImGui::InvisibleButton ( "###sym_selall_btn", size );
    bool clicked = ImGui::IsItemClicked ( );
    bool hovered = ImGui::IsItemHovered ( );
    bool active  = ImGui::IsItemActive ( );

    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    ImU32 bg_col = ImGui::GetColorU32 ( active ? ImGuiCol_FrameBgActive
                                        : hovered ? ImGuiCol_FrameBgHovered
                                        : ImGuiCol_FrameBg );
    ImVec2 br ( pos.x + size.x, pos.y + size.y );
    dl->AddRectFilled ( pos, br, bg_col, 3.0f );

    ImU32 mark_col = ImGui::GetColorU32 ( ImGuiCol_CheckMark );
    if ( all_selected ) {
        /* Checkmark analogicky ImGui::RenderCheckMark. */
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
        /* "Some" = vyplněný menší čtvereček uprostřed. */
        float pad = h * 0.28f;
        ImVec2 inner_min ( pos.x + pad,     pos.y + pad );
        ImVec2 inner_max ( pos.x + h - pad, pos.y + h - pad );
        dl->AddRectFilled ( inner_min, inner_max, mark_col, 1.5f );
    };

    if ( clicked ) {
        if ( all_selected ) {
            /* Odznačit všechny visible. */
            size_t total = sym_db_count ( );
            for ( size_t i = 0; i < total; i++ ) {
                const st_SYMBOL *s = sym_db_get_by_index ( i );
                if ( !s || !s->name ) continue;
                if ( !sym_filter_match ( s ) ) continue;
                g_su.selected.erase ( s->name );
            };
        } else {
            /* Vybrat všechny visible. */
            size_t total = sym_db_count ( );
            for ( size_t i = 0; i < total; i++ ) {
                const st_SYMBOL *s = sym_db_get_by_index ( i );
                if ( !s || !s->name ) continue;
                if ( !sym_filter_match ( s ) ) continue;
                g_su.selected.insert ( s->name );
            };
        };
    };
    ImGui::PopID ( );
    sym_item_tooltip ( _("Select / deselect all visible symbols") );
}


/* ========================================================================= */
/*  Sticky header                                                             */
/* ========================================================================= */


static void sym_render_sticky_header ( size_t visible_count, size_t total_count )
{
    /* Řádek 1: [+ Add] toggle + Search + Clear + count */
    {
        bool is_add_open = g_su.add_form_open;
        if ( is_add_open ) {
            if ( ImGui::Button ( _L("[- Add]##sym_add_toggle") ) ) {
                g_su.add_form_open = false;
                sym_add_form_reset ( );
            };
        } else {
            if ( ImGui::Button ( _L("[+ Add]##sym_add_toggle") ) ) {
                g_su.add_form_open = true;
            };
        };
        sym_item_tooltip ( _("Add new user label") );

        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _("Search:") );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 220.0f );
        ImGui::InputText ( "##sym_filter", g_su.filter, sizeof ( g_su.filter ) );
        sym_item_tooltip ( _("Filter visible symbols (case-insensitive). "
                            "Matches name, comment, and address hex (e.g. \"40\" matches 0x40FF).") );

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Clear##sym_filter_clear") ) ) {
            g_su.filter[ 0 ] = '\0';
        };
        sym_item_tooltip ( _("Clear filter") );

        ImGui::SameLine ( );
        ImGui::TextDisabled ( _("(%zu/%zu symbols)"), visible_count, total_count );
    }

    /* Řádek 2: Selected: N + bulk + file ops */
    {
        size_t sel_count = g_su.selected.size ( );
        ImGui::Text ( _("Selected: %zu"), sel_count );

        ImGui::SameLine ( );
        ImGui::BeginDisabled ( sel_count == 0 );
        if ( ImGui::Button ( _L("Delete##sym_bulk_delete") ) ) {
            g_su.show_delete_selected_confirm = true;
        };
        ImGui::EndDisabled ( );
        sym_item_tooltip ( _("Delete all selected symbols (with confirmation)") );

        /* Separátor mezi bulk a file ops. */
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|" );

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Load From...##sym_load") ) ) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles;
            ImGuiFileDialog::Instance ( )->OpenDialog (
                "SymLoadDialog", _("Load Symbols From..."),
                ".noi,.map,.sym,.lbl,.*", config );
        };
        sym_item_tooltip ( _("Load symbols from .noi / .map / .sym / .lbl file") );

        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Save .lbl As...##sym_save") ) ) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ConfirmOverwrite;
            ImGuiFileDialog::Instance ( )->OpenDialog (
                "SymSaveDialog", _("Save .lbl As..."),
                ".lbl", config );
        };
        sym_item_tooltip ( _("Save current LBL labels to .lbl file") );

        ImGui::SameLine ( );
        ImGui::BeginDisabled ( total_count == 0 );
        if ( ImGui::Button ( _L("Clear All##sym_clear_all") ) ) {
            g_su.show_clear_all_confirm = true;
        };
        ImGui::EndDisabled ( );
        sym_item_tooltip ( _("Wipe all symbols (LBL + imported)") );

        /* Persist... popup - default .lbl auto-load/save (V1.7+ 3.2).
         *
         * Spravuje cfg klíče [SYMBOLS] lbl_file / auto_load / auto_save.
         * Implicitně zapnuto (= "plně automatický" mode); uživatel může
         * vypnout pro každý směr nezávisle, nebo přepsat default cestu. */
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|" );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Persist...##sym_persist_btn") ) ) {
            ImGui::OpenPopup ( "sym_persist_popup" );
        };
        sym_item_tooltip (
            _("Configure auto-load and auto-save of default .lbl file") );

        if ( ImGui::BeginPopup ( "sym_persist_popup" ) ) {
            ImGui::TextUnformatted ( _("Default .lbl persistence:") );
            ImGui::Separator ( );

            bool auto_load_b = sym_db_get_auto_load_lbl ( ) ? true : false;
            if ( ImGui::Checkbox ( _L("Auto Load on start##sym_auto_load"),
                                    &auto_load_b ) ) {
                sym_db_set_auto_load_lbl ( auto_load_b ? 1 : 0 );
            };
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::SetTooltip ( "%s",
                    _("Load default .lbl file when emulator starts.") );
            };

            bool auto_save_b = sym_db_get_auto_save_lbl ( ) ? true : false;
            if ( ImGui::Checkbox ( _L("Auto Save on exit##sym_auto_save"),
                                    &auto_save_b ) ) {
                sym_db_set_auto_save_lbl ( auto_save_b ? 1 : 0 );
            };
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::SetTooltip ( "%s",
                    _("Save LBL symbols to default .lbl on exit.") );
            };

            ImGui::Separator ( );

            const char *cur = sym_db_get_default_lbl_file ( );
            ImGui::TextDisabled ( _("Default file: %s"),
                                   ( cur && cur[ 0 ] ) ? cur : _("(unset)") );

            if ( ImGui::Button ( _L("Browse...##sym_persist_browse") ) ) {
                IGFD::FileDialogConfig config;
                config.path = ( cur && cur[ 0 ] ) ? cur : ".";
                config.countSelectionMax = 1;
                config.flags = ImGuiFileDialogFlags_Modal |
                               ImGuiFileDialogFlags_DontShowHiddenFiles;
                ImGuiFileDialog::Instance ( )->OpenDialog (
                    "SymDefaultLblDialog", _("Select Default .lbl File"),
                    ".lbl,.*", config );
            };
            ImGui::SameLine ( );
            if ( ImGui::Button ( _L("Load now##sym_persist_load_now") ) ) {
                int n = sym_db_load_default_lbl ( );
                if ( n >= 0 ) {
                    snprintf ( g_su.status, sizeof ( g_su.status ),
                               _("Loaded %d symbols from default .lbl"), n );
                } else {
                    snprintf ( g_su.status, sizeof ( g_su.status ),
                               "%s", _("Default .lbl load failed (file missing?)") );
                };
            };
            ImGui::SameLine ( );
            if ( ImGui::Button ( _L("Save now##sym_persist_save_now") ) ) {
                int n = sym_db_save_default_lbl ( );
                if ( n >= 0 ) {
                    snprintf ( g_su.status, sizeof ( g_su.status ),
                               _("Saved %d LBL symbols to default .lbl"), n );
                } else {
                    snprintf ( g_su.status, sizeof ( g_su.status ),
                               "%s", _("Default .lbl save failed") );
                };
            };

            ImGui::EndPopup ( );
        };
    }

    /* Status (= zpráva po load/save). */
    if ( g_su.status[ 0 ] ) {
        ImGui::TextDisabled ( "%s", g_su.status );
    };
}


/* ========================================================================= */
/*  Add Label collapsible block                                               */
/* ========================================================================= */


static void sym_render_add_form_block ( void )
{
    if ( !g_su.add_form_open ) return;

    ImGui::Indent ( );
    ImGui::BeginGroup ( );

    /* Použijeme BeginTable pro zarovnané label/input layout. */
    if ( ImGui::BeginTable ( "##sym_add_tbl", 2,
                              ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn ( "lbl", ImGuiTableColumnFlags_WidthFixed );
        ImGui::TableSetupColumn ( "in",  ImGuiTableColumnFlags_WidthStretch );

        /* Addr + Name na stejném řádku - rozdělené přes BeginGroup. */
        ImGui::TableNextRow ( );
        ImGui::TableNextColumn ( );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _("Addr (hex):") );
        ImGui::TableNextColumn ( );
        ImGui::SetNextItemWidth ( 100.0f );
        ImGui::InputText ( "##sym_add_addr", g_su.add_addr, sizeof ( g_su.add_addr ),
                            ImGuiInputTextFlags_CharsHexadecimal );
        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _("  Name:") );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 200.0f );
        ImGui::InputText ( "##sym_add_name", g_su.add_name, sizeof ( g_su.add_name ) );

        /* Comment + OK / Cancel. */
        ImGui::TableNextRow ( );
        ImGui::TableNextColumn ( );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _("Comment:") );
        ImGui::TableNextColumn ( );
        ImGui::SetNextItemWidth ( -180.0f );  /* zarezervovat pro OK + Cancel */
        ImGui::InputText ( "##sym_add_cmt", g_su.add_comment,
                            sizeof ( g_su.add_comment ) );
        ImGui::SameLine ( );
        bool can_add = ( g_su.add_addr[ 0 ] != '\0' && g_su.add_name[ 0 ] != '\0' );
        ImGui::BeginDisabled ( !can_add );
        if ( ImGui::Button ( _L("OK##sym_add_ok") ) ) {
            if ( sym_add_form_submit ( ) ) {
                g_su.add_form_open = false;
            };
        };
        ImGui::EndDisabled ( );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##sym_add_cancel") ) ) {
            sym_add_form_reset ( );
            g_su.add_form_open = false;
        };

        ImGui::EndTable ( );
    };

    if ( g_su.add_error[ 0 ] ) {
        ImVec4 red = ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f );
        ImGui::TextColored ( red, "%s", g_su.add_error );
    };

    ImGui::EndGroup ( );
    ImGui::Unindent ( );
}


/* ========================================================================= */
/*  Render řádku tabulky                                                      */
/* ========================================================================= */


static void sym_render_row ( size_t idx, std::string *out_pending_delete )
{
    const st_SYMBOL *s = sym_db_get_by_index ( idx );
    if ( !s || !s->name ) return;
    if ( !sym_filter_match ( s ) ) return;

    bool is_editing_this = ( g_su.editing_name == s->name );

    ImGui::PushID ( (int) idx );
    ImGui::TableNextRow ( );

    /* Sloupec 0: Sel checkbox */
    ImGui::TableNextColumn ( );
    bool sel = ( g_su.selected.count ( s->name ) > 0 );
    if ( ImGui::Checkbox ( "##sym_sel", &sel ) ) {
        if ( sel ) g_su.selected.insert ( s->name );
        else g_su.selected.erase ( s->name );
    };

    /* Sloupec 1 (V1.C.3): Owner badge */
    ImGui::TableNextColumn ( );
    owner_badge_render ( s->cmd_origin );

    /* Sloupec 2: Name (dvojklik = rename) */
    ImGui::TableNextColumn ( );
    if ( is_editing_this && g_su.editing_field == SymUiState::EditField::Name ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_su.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_su.edit_focus_pending = false;
        };
        bool enter = ImGui::InputText ( "##sym_edit_name", g_su.edit_buffer,
                                          sizeof ( g_su.edit_buffer ),
                                          ImGuiInputTextFlags_EnterReturnsTrue );
        if ( enter ) {
            sym_apply_edit ( );
        } else if ( ImGui::IsItemDeactivated ( ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
                sym_cancel_edit ( );
            } else {
                sym_apply_edit ( );
            };
        };
        if ( g_su.edit_error[ 0 ] ) {
            ImVec4 red = ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f );
            ImGui::TextColored ( red, "%s", g_su.edit_error );
        };
    } else {
        if ( ImGui::Selectable ( s->name, false,
                                  ImGuiSelectableFlags_AllowDoubleClick ) ) {
            if ( ImGui::IsMouseDoubleClicked ( 0 ) ) {
                sym_begin_edit_name ( s );
            };
        };
        sym_item_tooltip ( _("Double-click to rename. Existing references "
                            "(BPs / disasm) keep old name unless you also "
                            "remove the old symbol manually.") );
    };

    /* Sloupec 2: Addr (dvojklik = relocate) */
    ImGui::TableNextColumn ( );
    if ( is_editing_this && g_su.editing_field == SymUiState::EditField::Addr ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_su.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_su.edit_focus_pending = false;
        };
        bool enter = ImGui::InputText ( "##sym_edit_addr", g_su.edit_buffer,
                                          sizeof ( g_su.edit_buffer ),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_CharsHexadecimal );
        if ( enter ) {
            sym_apply_edit ( );
        } else if ( ImGui::IsItemDeactivated ( ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
                sym_cancel_edit ( );
            } else {
                sym_apply_edit ( );
            };
        };
        if ( g_su.edit_error[ 0 ] ) {
            ImVec4 red = ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f );
            ImGui::TextColored ( red, "%s", g_su.edit_error );
        };
    } else {
        char addr_buf[ 16 ];
        snprintf ( addr_buf, sizeof ( addr_buf ), "0x%04X", (unsigned) s->addr );
        if ( ImGui::Selectable ( addr_buf, false,
                                  ImGuiSelectableFlags_AllowDoubleClick ) ) {
            if ( ImGui::IsMouseDoubleClicked ( 0 ) ) {
                sym_begin_edit_addr ( s );
            };
        };
        sym_item_tooltip ( _("Double-click to relocate (= change address)") );
    };

    /* Sloupec 3: Bank */
    ImGui::TableNextColumn ( );
    ImGui::Text ( "%u", (unsigned) s->bank_id );

    /* Sloupec 4: Source */
    ImGui::TableNextColumn ( );
    ImGui::TextUnformatted ( sym_source_label ( s->source ) );

    /* Sloupec 5: Comment (dvojklik = inline edit, auto-promote LBL) */
    ImGui::TableNextColumn ( );
    if ( is_editing_this && g_su.editing_field == SymUiState::EditField::Comment ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_su.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_su.edit_focus_pending = false;
        };
        bool enter = ImGui::InputText ( "##sym_edit_cmt", g_su.edit_buffer,
                                          sizeof ( g_su.edit_buffer ),
                                          ImGuiInputTextFlags_EnterReturnsTrue );
        if ( enter ) {
            sym_apply_edit ( );
        } else if ( ImGui::IsItemDeactivated ( ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
                sym_cancel_edit ( );
            } else {
                sym_apply_edit ( );
            };
        };
    } else {
        const char *cmt_disp = ( s->comment && s->comment[ 0 ] )
                               ? s->comment : "";
        if ( ImGui::Selectable ( cmt_disp[ 0 ] ? cmt_disp : "##empty_cmt",
                                  false,
                                  ImGuiSelectableFlags_AllowDoubleClick |
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap ) ) {
            if ( ImGui::IsMouseDoubleClicked ( 0 ) ) {
                sym_begin_edit_comment ( s );
            };
        };
        sym_item_tooltip ( _("Double-click to edit comment "
                            "(auto-promotes symbol to LBL source)") );
    };

    /* Sloupec 6: Delete x (per-row, no confirm).
     *
     * SetNextItemAllowOverlap = button konzumuje klik i když Comment Selectable
     * v sloupci 5 má SpanAllColumns (= jeho hit rect přesahuje do dalších
     * sloupců). Bez tohoto kombo flag by klik na "x" propadl do Selectable. */
    ImGui::TableNextColumn ( );
    ImGui::SetNextItemAllowOverlap ( );
    if ( ImGui::SmallButton ( _L("x##sym_row_del") ) ) {
        if ( out_pending_delete ) *out_pending_delete = s->name;
    };
    sym_item_tooltip ( _("Remove this symbol") );

    ImGui::PopID ( );
}


/* ========================================================================= */
/*  Tabulka header s tristate select-all                                      */
/* ========================================================================= */


static void sym_render_table_header ( void )
{
    /* Custom header row - sloupec 0 nahradíme tristate checkboxem,
     * ostatní sloupce standardní header. V1.C.3 - sloupec 1 je Owner
     * badge (nová položka v headeru). */
    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
    ImGui::TableSetColumnIndex ( 0 );
    sym_render_select_all ( );

    static const char *headers[ 7 ] = { "Own", "Name", "Addr", "Bank",
                                          "Source", "Comment", "" };
    for ( int i = 1; i <= 7; i++ ) {
        ImGui::TableSetColumnIndex ( i );
        const char *name = headers[ i - 1 ];
        ImGui::PushID ( i );
        ImGui::TableHeader ( name );
        ImGui::PopID ( );
    };
}


/* ========================================================================= */
/*  Confirm dialogy                                                           */
/* ========================================================================= */


static void sym_render_confirm_dialogs ( void )
{
    /* Clear All confirm. */
    if ( g_su.show_clear_all_confirm ) {
        ImGui::OpenPopup ( _L("Clear All Symbols?") );
        g_su.show_clear_all_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L("Clear All Symbols?"), NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _("Wipe all %zu symbols (LBL + imported)?"),
                      sym_db_count ( ) );
        ImGui::Spacing ( );
        if ( ImGui::Button ( _L("Clear All##sym_clear_all_confirm_ok"), ImVec2 ( 100, 0 ) ) ) {
            sym_db_clear ( );
            g_su.selected.clear ( );
            sym_cancel_edit ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##sym_clear_all_confirm_cancel"), ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Delete Selected confirm. */
    if ( g_su.show_delete_selected_confirm ) {
        ImGui::OpenPopup ( _L("Delete Selected?") );
        g_su.show_delete_selected_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L("Delete Selected?"), NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _("Delete %zu selected symbols?"), g_su.selected.size ( ) );
        ImGui::Spacing ( );
        if ( ImGui::Button ( _L("Delete##sym_del_sel_confirm_ok"), ImVec2 ( 100, 0 ) ) ) {
            /* Snapshot jmen aby iterace přes selected set neměla
             * invalidní pointers po remove. */
            std::vector<std::string> names ( g_su.selected.begin ( ),
                                              g_su.selected.end ( ) );
            for ( const auto &n : names ) {
                sym_db_remove_user_label ( n.c_str ( ) );
            };
            g_su.selected.clear ( );
            sym_cancel_edit ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L("Cancel##sym_del_sel_confirm_cancel"), ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };
}


/* ========================================================================= */
/*  File dialogy                                                              */
/* ========================================================================= */


static void sym_render_file_dialogs ( void )
{
    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "SymLoadDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            int n = sym_db_load_auto ( path.c_str ( ) );
            if ( n >= 0 ) {
                snprintf ( g_su.status, sizeof ( g_su.status ),
                           _("Loaded %d symbols from %s"), n, path.c_str ( ) );
            } else {
                snprintf ( g_su.status, sizeof ( g_su.status ),
                           _("Load failed: %s"), path.c_str ( ) );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "SymSaveDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            int n = sym_db_save_lbl ( path.c_str ( ) );
            if ( n >= 0 ) {
                snprintf ( g_su.status, sizeof ( g_su.status ),
                           _("Saved %d LBL symbols to %s"), n, path.c_str ( ) );
            } else {
                snprintf ( g_su.status, sizeof ( g_su.status ),
                           _("Save failed: %s"), path.c_str ( ) );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    /* Default .lbl path picker (V1.7+ 3.2). User vybere soubor v Persist
     * popupu -> set cfg klíče lbl_file. Save proběhne až při exit (nebo
     * uživatel klikne "Save now"). */
    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "SymDefaultLblDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            sym_db_set_default_lbl_file ( path.c_str ( ) );
            snprintf ( g_su.status, sizeof ( g_su.status ),
                       _("Default .lbl = %s"), path.c_str ( ) );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };
}


/* ========================================================================= */
/*  Main render                                                               */
/* ========================================================================= */


void sym_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 820, 420 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 760, 300 ),
                                            ImVec2 ( FLT_MAX, FLT_MAX ) );

    /* Auto-layout při fresh open. Stabilní ID přes ###. */
    const char *sym_title = _L("Symbols###sym_main");
    auto_layout_first_use_portrait ( sym_title, 400.0f, 500.0f );
    if ( !ImGui::Begin ( sym_title, p_open,
                          ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::End ( );
        return;
    };

    /* Snapshot count + visible. */
    size_t total_count = sym_db_count ( );
    size_t visible_count = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const st_SYMBOL *s = sym_db_get_by_index ( i );
        if ( !s || !s->name ) continue;
        if ( sym_filter_match ( s ) ) visible_count++;
    };

    /* Sticky header + Add form render přímo do window (= window-level
     * horizontal scrollbar). */
    sym_render_sticky_header ( visible_count, total_count );

    if ( g_su.add_form_open ) {
        sym_render_add_form_block ( );
    };

    ImGui::Separator ( );

    /* Tabulka v scrollable child. */
    ImGui::BeginChild ( "###sym_table_child", ImVec2 ( 0, 0 ), false,
                         ImGuiWindowFlags_None );
    if ( total_count == 0 ) {
        ImGui::TextDisabled ( "%s", _("No symbols loaded. Use [+ Add] above or "
                               "[Load From...] to import .noi/.map/.sym/.lbl.") );
    } else {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_NoSavedSettings;
        if ( ImGui::BeginTable ( "###sym_tbl", 8, flags ) ) {
            ImGui::TableSetupScrollFreeze ( 0, 1 );
            ImGui::TableSetupColumn ( "",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 26.0f );
            /* V1.C.3 - Owner badge sloupec mezi Sel a Name. */
            ImGui::TableSetupColumn ( _L( "Own##sym_col_owner" ),
                                       ImGuiTableColumnFlags_WidthFixed, 36.0f );
            ImGui::TableSetupColumn ( "Name",
                                       ImGuiTableColumnFlags_WidthFixed, 160.0f );
            ImGui::TableSetupColumn ( "Addr",
                                       ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableSetupColumn ( "Bank",
                                       ImGuiTableColumnFlags_WidthFixed, 50.0f );
            ImGui::TableSetupColumn ( "Source",
                                       ImGuiTableColumnFlags_WidthFixed, 60.0f );
            ImGui::TableSetupColumn ( "Comment",
                                       ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn ( "",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 28.0f );
            sym_render_table_header ( );

            std::string pending_delete;

            /* V1.E.6.A: pending focus z sym_window_focus_addr() - před loop
             * najdi cíl, předznač selection a filter clear. Scrollnutí se
             * provede v loop po row renderu, kde IsItemHovered+SetScrollHereY
             * cílí na konkrétní řádek tabulky. */
            bool     focus_active_local = g_su.pending_focus_active;
            uint32_t focus_addr_local   = g_su.pending_focus_addr;
            g_su.pending_focus_active = false;

            if ( focus_active_local ) {
                /* Vyčisti filter, aby se cílový symbol určitě v listu objevil. */
                g_su.filter[0] = '\0';
                /* Nastav selection. Cíl v storage hledáme přes
                 * sym_db_lookup_by_addr (= bank=0 = default). Pokud nenajde,
                 * jen filter zůstane vyčištěný a žádný side effect. */
                const st_SYMBOL *target = sym_db_lookup_by_addr (
                    focus_addr_local, 0 );
                if ( target && target->name ) {
                    g_su.selected.clear ( );
                    g_su.selected.insert ( std::string ( target->name ) );
                };
            };

            for ( size_t i = 0; i < total_count; i++ ) {
                sym_render_row ( i, &pending_delete );

                /* V1.E.6.A: pokud aktuální řádek odpovídá focus targetu,
                 * scroll. sym_render_row může skipnout filtrem, ale po
                 * filter clear focusovaný symbol projde. */
                if ( focus_active_local ) {
                    const st_SYMBOL *s = sym_db_get_by_index ( i );
                    if ( s && s->name && s->addr == focus_addr_local ) {
                        ImGui::SetScrollHereY ( 0.5f );
                        focus_active_local = false;
                    };
                };

                if ( !pending_delete.empty ( ) ) {
                    g_su.selected.erase ( pending_delete );
                    if ( g_su.editing_name == pending_delete ) {
                        sym_cancel_edit ( );
                    };
                    sym_db_remove_user_label ( pending_delete.c_str ( ) );
                    break;
                };
            };
            ImGui::EndTable ( );
        };
    };
    ImGui::EndChild ( );

    sym_render_confirm_dialogs ( );
    sym_render_file_dialogs ( );

    ImGui::End ( );
}


void sym_window_show_hide ( void )
{
    g_gui->showSymbolsWindow = !g_gui->showSymbolsWindow;
}


extern "C" void sym_window_focus_addr ( uint16_t addr )
{
    /* V1.E.6.A: Activity dvojklik routing - otevři okno a označ pending
     * focus addr. Render loop při dalším frame projde sym_db, najde první
     * symbol s addr == pending_focus_addr a scrollne na něj + označí jako
     * selected. Pokud symbol s danou adresou neexistuje, spotřeba je no-op
     * (= žádný side effect kromě otevření okna).
     *
     * Použít flag pending_focus_active místo "addr != 0" sentinel: adresa
     * 0x0000 je legální RAM cíl (= ROM start) a může být cílem symbolu. */
    g_gui->showSymbolsWindow = true;
    g_su.pending_focus_active = true;
    g_su.pending_focus_addr = (uint32_t)addr;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
