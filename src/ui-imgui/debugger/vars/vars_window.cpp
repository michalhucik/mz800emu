/*
 * vars_window.cpp - Live $vars panel pro user-defined proměnné smart BP.
 *
 * Layout (V1.5.C UX rework, Michal feedback 2026-05-06):
 *   +---------------------------------------------------------------------+
 *   | Variables                                                     [X]   |
 *   +---------------------------------------------------------------------+
 *   | Sticky header (2 řádky):                                            |
 *   |   [+ Add]  Filter:[__________] [Clear]  (vis/tot)                   |
 *   |   Selected: N  [Del] [Set 0] [+1] [-1] | [Save] [Save As...]        |
 *   |                            [Load From...] [Merge...] [Clear ...]   |
 *   +---------------------------------------------------------------------+
 *   | (Add Var collapsible block - jen když [+ Add] je aktivní)          |
 *   |   Name:[___] Value:[__] [v] Persist                                 |
 *   |   Comment:[__________________________] [OK] [Cancel]                |
 *   +---------------------------------------------------------------------+
 *   | [v] | Name      | Value | Hex   | Persist | Comment      | x |     |
 *   |-----|-----------|-------|-------|---------|--------------|---|     |
 *   | [ ] | $framecnt | 250   | 0xFA  | [v]     | frame counter| x |     |
 *   | [ ] | $state    | 7     | 0x07  | [ ]     | state machine| x |     |
 *   +---------------------------------------------------------------------+
 *
 * Rozdíly oproti V1.5.B:
 *   - "(?)" markery odstraněny, tooltipy přímo na hover tlačítka
 *   - Add Var formulář vyjmut ze sticky header do samostatného bloku
 *   - 7 sloupců (přidán dedikovaný delete sloupec, dříve SameLine v Comment)
 *   - "Sel" hlavička nahrazena tristate select-all checkboxem
 *   - "Persist" hlavička místo zkráceného "Per..." (širší fixed sloupec)
 *   - Sloupec "Name" anglicky (dříve "Název" v lokalizovaném buildu)
 *
 * "Clear Values" = bp_vars_clear_all (vynuluje hodnoty, záznamy zůstávají).
 * "Clear All"    = bp_vars_clear_storage (smaže záznamy).
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
#include <stdint.h>
#include <errno.h>
#include <limits.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include "vars_window.h"

#include <set>
#include <string>
#include <vector>
#include <cstring>

extern "C" {
#include "emulator/debugger/bp_vars.h"
}


/* ========================================================================= */
/*  V1.5.B - IASM value parser helper                                        */
/* ========================================================================= */

/**
 * @brief Parsuje hodnotu pro $vars input per IASM konvenci.
 *
 * Podporované formáty:
 *   - "42"        -> 42 (decimal)
 *   - "0x2A"      -> 42 (hex C-style)
 *   - "#2A"       -> 42 (hex Sharp/IASM)
 *   - "%101010"   -> 42 (binary)
 *   - "-1"        -> -1 (negative decimal)
 *
 * Vstup je whitespace-tolerant (= leading + trailing). Hex/bin přijímá
 * jen kladné hodnoty (= bitová interpretace, sign nemá smysl).
 *
 * @param input  Vstupní řetězec (NULL = false).
 * @param out    Výstup signed 32-bit (jen při úspěchu).
 * @return true při úspěchu, false pokud vstup je prázdný / nesprávný formát
 *         / nečíselné znaky / přetečení.
 */
static bool vars_value_parse ( const char *input, int32_t *out )
{
    if ( !input || !out ) return false;

    /* Skip leading whitespace */
    while ( *input && ( *input == ' ' || *input == '\t' ) ) input++;
    if ( !*input ) return false;

    /* Detekce znaménka pro decimal (hex/bin sign nedává smysl) */
    bool negative = false;
    const char *p = input;
    if ( *p == '+' ) { p++; }
    else if ( *p == '-' ) { negative = true; p++; }

    /* Detekce prefixu */
    int base = 10;
    if ( *p == '#' ) {
        if ( negative ) return false;  /* hex sign nepovolen */
        base = 16;
        p++;
    } else if ( p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) ) {
        if ( negative ) return false;
        base = 16;
        p += 2;
    } else if ( *p == '%' ) {
        if ( negative ) return false;
        base = 2;
        p++;
    };

    if ( !*p ) return false;

    /* Validace tělových znaků (= strtoll je tolerantní, my chceme strict). */
    const char *body = p;
    while ( *body ) {
        char c = *body;
        if ( c == ' ' || c == '\t' ) break;  /* trailing ws */
        bool ok = false;
        if ( base == 10 && c >= '0' && c <= '9' ) ok = true;
        else if ( base == 16 && ( ( c >= '0' && c <= '9' ) ||
                                  ( c >= 'a' && c <= 'f' ) ||
                                  ( c >= 'A' && c <= 'F' ) ) ) ok = true;
        else if ( base == 2 && ( c == '0' || c == '1' ) ) ok = true;
        if ( !ok ) return false;
        body++;
    };

    /* Trailing whitespace check */
    while ( *body == ' ' || *body == '\t' ) body++;
    if ( *body ) return false;  /* nečekaný znak za hodnotou */

    char *end = NULL;
    errno = 0;
    long long val = strtoll ( p, &end, base );
    if ( errno == ERANGE ) return false;
    if ( negative ) val = -val;

    /* Range check signed 32-bit. */
    if ( val > (long long) INT32_MAX || val < (long long) INT32_MIN ) return false;

    *out = (int32_t) val;
    return true;
}


/**
 * @brief Formátuje value do hex zobrazení pro Hex sloupec.
 *
 * Pro nezáporné hodnoty: "0x<HEX>". Pro záporné: "-0x<HEX>" (= absolutní
 * hodnota s prefixem znaménka, čitelnější než two's complement 0xFFFFFFFF).
 *
 * @param value     Vstupní hodnota.
 * @param buf       Výstupní buffer.
 * @param bufsize   Velikost bufferu (>= 12 stačí pro int32_t).
 */
static void vars_value_format_hex ( int32_t value, char *buf, size_t bufsize )
{
    if ( !buf || bufsize == 0 ) return;
    if ( value < 0 ) {
        /* Použij unsigned negaci přes uint32_t, vyhneme se UB pro INT32_MIN. */
        uint32_t abs_val = (uint32_t) ( -(int64_t) value );
        snprintf ( buf, bufsize, "-0x%X", abs_val );
    } else {
        snprintf ( buf, bufsize, "0x%X", (uint32_t) value );
    };
}


/* ========================================================================= */
/*  V1.5.B - UI panel state                                                  */
/* ========================================================================= */

/**
 * @brief Persistentní state UI okna (per-frame).
 *
 * Selection: jména vybraných řádků (= bulk ops). Mažeme/přepisujeme
 * jména po každé mutaci storage, aby se selection neukazoval na
 * smazané var.
 *
 * Inline edit: drží jméno řádku v editu + edit buffer.
 *
 * Add-form expand state.
 *
 * Pending action - zachycení akce do "po render" fáze (= aby se
 * neztratil iterator po unset/set_full v půlce render loopu).
 */
namespace {

struct PendingClearStorage { bool active; };
struct PendingDeleteName { std::string name; };
struct PendingMergeFile { std::string path; };
struct PendingLoadFile { std::string path; };
struct PendingClearAllValues { bool active; };

struct VarsUiState {
    /* Filter (case-insensitive substring na name + comment). */
    char filter[128] = "";

    /* Add-form expand (= sticky header reveal). */
    bool add_form_open = false;
    char add_name[BP_VAR_NAME_MAX + 1] = "";
    char add_value[32] = "";
    char add_comment[BP_VAR_COMMENT_MAX + 1] = "";
    bool add_persist = true;
    char add_error[160] = "";   /* validation feedback */

    /* Inline edit: drží name + buffer + původní typ editu. */
    std::string editing_name;       /* prázdný = no edit */
    enum class EditField { None, Name, Value, Hex, Comment };
    EditField editing_field = EditField::None;
    char edit_buffer[BP_VAR_COMMENT_MAX + 1] = "";
    bool edit_focus_pending = false;
    char edit_error[160] = "";

    /* Bulk Set... popup state. */
    char bulk_set_buf[64] = "";
    char bulk_set_error[80] = "";

    /* Selection (jména vybraných řádků). */
    std::set<std::string> selected;

    /* Pending dialogy. */
    bool show_clear_all_confirm = false;
    bool show_clear_values_confirm = false;
    bool show_delete_selected_confirm = false;
    bool show_load_replace_confirm = false;
    bool show_merge_choice = false;
    std::string pending_load_path;
    std::string pending_merge_path;

    /* Status text (= zobrazený pod ovládáním, např. po save/load).
     * 512 = errbuf[256] + path + prefix; GCC -Wformat-truncation= jinak
     * hlasi pri snprintf("Load failed: %s", errbuf) truncation. */
    char status[512] = "";
};

}  /* anonymous namespace */

static VarsUiState g_vu;


/* ========================================================================= */
/*  Helpery                                                                  */
/* ========================================================================= */


/**
 * @brief Tooltip pro předchozí ImGui prvek (= "(?)" markery odstraněny per
 *        Michal 2026-05-06, tooltip se objeví přímo na hover tlačítka).
 *
 * Wrapper přes ImGui::SetItemTooltip s wrap pro delší texty.
 *
 * @param desc Text tooltipu (anglicky, user-facing).
 */
static void vars_item_tooltip ( const char *desc )
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
 * @brief Case-insensitive substring match helper.
 */
static bool vars_str_contains_ci ( const char *haystack, const char *needle )
{
    if ( !needle || !*needle ) return true;
    if ( !haystack ) return false;
    size_t hl = strlen ( haystack );
    size_t nl = strlen ( needle );
    if ( nl > hl ) return false;
    for ( size_t i = 0; i + nl <= hl; i++ ) {
        size_t j = 0;
        for ( ; j < nl; j++ ) {
            char a = haystack[i + j];
            char b = needle[j];
            if ( a >= 'A' && a <= 'Z' ) a = (char) ( a - 'A' + 'a' );
            if ( b >= 'A' && b <= 'Z' ) b = (char) ( b - 'A' + 'a' );
            if ( a != b ) break;
        };
        if ( j == nl ) return true;
    };
    return false;
}


/**
 * @brief Filter match: name + comment substring (= per Michal 2026-05-06).
 */
static bool vars_filter_match ( const bp_var_t *v )
{
    if ( !g_vu.filter[0] ) return true;
    if ( vars_str_contains_ci ( v->name, g_vu.filter ) ) return true;
    if ( v->comment && vars_str_contains_ci ( v->comment, g_vu.filter ) ) return true;
    return false;
}


/**
 * @brief Vyčistí state add-formu (po úspěšném Add nebo Cancel).
 */
static void vars_add_form_reset ( void )
{
    g_vu.add_name[0] = '\0';
    g_vu.add_value[0] = '\0';
    g_vu.add_comment[0] = '\0';
    g_vu.add_persist = true;
    g_vu.add_error[0] = '\0';
}


/**
 * @brief Pokus o přidání nové variable z add-formy.
 *
 * Validace: name (regex), value (IASM parser), duplicate check.
 * Při chybě nastaví g_vu.add_error a nedělá nic. Při úspěchu storage
 * dostane nový záznam, formulář se vyčistí.
 *
 * @return true pokud byl záznam přidán.
 */
static bool vars_add_form_submit ( void )
{
    g_vu.add_error[0] = '\0';

    if ( !bp_var_name_is_valid ( g_vu.add_name ) ) {
        snprintf ( g_vu.add_error, sizeof ( g_vu.add_error ),
                   "Invalid name (must match [a-zA-Z_][a-zA-Z0-9_]*, max %d chars)",
                   BP_VAR_NAME_MAX );
        return false;
    };

    /* Duplicate check - bp_vars_count + linear scan (storage neexposuje
     * find by name přes API, ale set/get zvládne BC). */
    size_t count = bp_vars_count ( );
    for ( size_t i = 0; i < count; i++ ) {
        const bp_var_t *e = bp_vars_get_by_index ( i );
        if ( e && e->name && strcmp ( e->name, g_vu.add_name ) == 0 ) {
            snprintf ( g_vu.add_error, sizeof ( g_vu.add_error ),
                       "Variable '$%s' already exists", g_vu.add_name );
            return false;
        };
    };

    int32_t value = 0;
    if ( g_vu.add_value[0] ) {
        if ( !vars_value_parse ( g_vu.add_value, &value ) ) {
            snprintf ( g_vu.add_error, sizeof ( g_vu.add_error ),
                       "Invalid value (use 42 / 0x2A / #2A / %%101010)" );
            return false;
        };
    };

    bp_var_set_full ( g_vu.add_name, value,
                      g_vu.add_comment[0] ? g_vu.add_comment : NULL,
                      g_vu.add_persist );
    vars_add_form_reset ( );
    return true;
}


/**
 * @brief Začíná inline edit hodnoty (Value sloupec).
 */
static void vars_begin_edit_value ( const bp_var_t *v )
{
    g_vu.editing_name = v->name;
    g_vu.editing_field = VarsUiState::EditField::Value;
    snprintf ( g_vu.edit_buffer, sizeof ( g_vu.edit_buffer ),
               "%d", (int) v->value );
    g_vu.edit_focus_pending = true;
    g_vu.edit_error[0] = '\0';
}


/**
 * @brief Začíná inline edit hodnoty z Hex sloupce.
 *
 * Buffer předvyplněn "0x%X" (= aktuální value v hex). Apply je stejný
 * jako Value (= IASM parser parse + bp_var_set).
 */
static void vars_begin_edit_hex ( const bp_var_t *v )
{
    g_vu.editing_name = v->name;
    g_vu.editing_field = VarsUiState::EditField::Hex;
    /* Pro záporné hodnoty zobrazí jako 32-bit hex (= jako vars_value_format_hex). */
    snprintf ( g_vu.edit_buffer, sizeof ( g_vu.edit_buffer ),
               "0x%X", (unsigned) v->value );
    g_vu.edit_focus_pending = true;
    g_vu.edit_error[0] = '\0';
}


/**
 * @brief Začíná inline edit názvu (Name sloupec).
 *
 * Rename je bez refaktorizace BPs - existing BP s `$oldname` v condition
 * / action zůstanou s starým jménem (= bp_var_get vrátí 0 pokud old name
 * smazán, action `set $oldname = ...` vytvoří novou variable).
 */
static void vars_begin_edit_name ( const bp_var_t *v )
{
    g_vu.editing_name = v->name;
    g_vu.editing_field = VarsUiState::EditField::Name;
    snprintf ( g_vu.edit_buffer, sizeof ( g_vu.edit_buffer ),
               "%s", v->name );
    g_vu.edit_focus_pending = true;
    g_vu.edit_error[0] = '\0';
}


/**
 * @brief Začíná inline edit comment (Comment sloupec).
 */
static void vars_begin_edit_comment ( const bp_var_t *v )
{
    g_vu.editing_name = v->name;
    g_vu.editing_field = VarsUiState::EditField::Comment;
    snprintf ( g_vu.edit_buffer, sizeof ( g_vu.edit_buffer ),
               "%s", v->comment ? v->comment : "" );
    g_vu.edit_focus_pending = true;
    g_vu.edit_error[0] = '\0';
}


/**
 * @brief Aplikuje inline edit (apply) - podle field type.
 *
 * @return true pokud apply proběhl OK (= editing state se vyčistil).
 *         false pokud parse error (editing zůstává aktivní pro retry).
 */
static bool vars_apply_edit ( void )
{
    if ( g_vu.editing_name.empty ( ) ) return true;

    if ( g_vu.editing_field == VarsUiState::EditField::Value ||
         g_vu.editing_field == VarsUiState::EditField::Hex ) {
        int32_t parsed = 0;
        if ( !vars_value_parse ( g_vu.edit_buffer, &parsed ) ) {
            snprintf ( g_vu.edit_error, sizeof ( g_vu.edit_error ),
                       "Invalid format (use 42 / 0x2A / #2A / %%101010)" );
            return false;
        };
        bp_var_set ( g_vu.editing_name.c_str ( ), parsed );
    } else if ( g_vu.editing_field == VarsUiState::EditField::Name ) {
        const char *new_name = g_vu.edit_buffer;
        const char *old_name = g_vu.editing_name.c_str ( );
        /* Žádná změna -> tichý exit. */
        if ( strcmp ( new_name, old_name ) == 0 ) {
            /* propadne do reset níže */
        } else if ( !bp_var_name_is_valid ( new_name ) ) {
            snprintf ( g_vu.edit_error, sizeof ( g_vu.edit_error ),
                       "Invalid name (use [a-zA-Z_][a-zA-Z0-9_]*)" );
            return false;
        } else {
            /* Duplicate check: nový název nesmí existovat. */
            size_t cnt = bp_vars_count ( );
            for ( size_t i = 0; i < cnt; i++ ) {
                const bp_var_t *vi = bp_vars_get_by_index ( i );
                if ( vi && vi->name && strcmp ( vi->name, new_name ) == 0 ) {
                    snprintf ( g_vu.edit_error, sizeof ( g_vu.edit_error ),
                               "Name already exists" );
                    return false;
                };
            };
            /* Rename = snapshot fields + unset(old) + set(new). */
            int32_t old_value = bp_var_get ( old_name );
            const char *old_cmt = bp_var_get_comment ( old_name );
            char saved_cmt[BP_VAR_COMMENT_MAX + 1];
            saved_cmt[0] = '\0';
            if ( old_cmt ) {
                strncpy ( saved_cmt, old_cmt, sizeof ( saved_cmt ) - 1 );
                saved_cmt[sizeof ( saved_cmt ) - 1] = '\0';
            };
            bool old_persist = bp_var_get_persist ( old_name );
            bp_var_unset ( old_name );
            bp_var_set ( new_name, old_value );
            if ( saved_cmt[0] ) bp_var_set_comment ( new_name, saved_cmt );
            bp_var_set_persist ( new_name, old_persist );
            /* Update selection set: pokud old byl selected, nyní new je. */
            if ( g_vu.selected.erase ( old_name ) > 0 ) {
                g_vu.selected.insert ( new_name );
            };
        };
    } else if ( g_vu.editing_field == VarsUiState::EditField::Comment ) {
        /* Truncate na BP_VAR_COMMENT_MAX (storage to defenzivně přijme,
         * ale vstup z UI je limited InputText size). */
        bp_var_set_comment ( g_vu.editing_name.c_str ( ),
                             g_vu.edit_buffer[0] ? g_vu.edit_buffer : NULL );
    };

    g_vu.editing_name.clear ( );
    g_vu.editing_field = VarsUiState::EditField::None;
    g_vu.edit_buffer[0] = '\0';
    g_vu.edit_error[0] = '\0';
    return true;
}


/**
 * @brief Cancel inline edit (Esc) - žádné změny do storage.
 */
static void vars_cancel_edit ( void )
{
    g_vu.editing_name.clear ( );
    g_vu.editing_field = VarsUiState::EditField::None;
    g_vu.edit_buffer[0] = '\0';
    g_vu.edit_error[0] = '\0';
}


/* ========================================================================= */
/*  Sticky header                                                            */
/* ========================================================================= */


/**
 * @brief Render sticky header (= 2 řádky: filter+counts | bulk + file ops).
 *
 * Layout (per Michal feedback 2026-05-06):
 *   Řádek 1: [+ Add]  Filter:[__] [Clear]  (vis/tot)
 *   Řádek 2: Selected: N  [Del] [Set 0] [+1] [-1]  |  [Save] [Save As...]
 *            [Load From...] [Merge...] [Clear Values] [Clear All]
 *
 * Add form se NErenderuje uvnitř sticky header (= dříve překrýval řádek 3
 * při expandu). Místo toho po sticky header EndChild se renderuje samostatně
 * vars_render_add_form_block.
 *
 * Tooltipy se zobrazují přímo na hover tlačítka (= žádné "(?)" markery).
 *
 * @param visible_count Počet aktuálně zobrazených (po filtru) varů.
 * @param total_count   Celkový počet ve storage.
 */
static void vars_render_sticky_header ( size_t visible_count, size_t total_count )
{
    /* Řádek 1: [+ Add] / [- Cancel] + Filter + counts. */
    ImGui::AlignTextToFramePadding ( );
    if ( ImGui::Button ( g_vu.add_form_open ? _L ( "- Cancel##varshdr_add" )
                                            : _L ( "+ Add##varshdr_add" ) ) ) {
        g_vu.add_form_open = !g_vu.add_form_open;
        if ( !g_vu.add_form_open ) vars_add_form_reset ( );
    };
    vars_item_tooltip ( _( "Add a new variable (expands form below)." ) );

    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Filter:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 200.0f );
    ImGui::InputTextWithHint ( "###vars_filter", _( "name or comment" ),
                                g_vu.filter, sizeof ( g_vu.filter ) );
    vars_item_tooltip ( _( "Filter matches variable name or comment "
                           "(case-insensitive substring)." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear##vars_clrflt" ) ) ) {
        g_vu.filter[0] = '\0';
    };
    vars_item_tooltip ( _( "Clear the filter text." ) );
    ImGui::SameLine ( );
    ImGui::Text ( "(%zu/%zu)", visible_count, total_count );
    vars_item_tooltip ( _( "Visible / total variable count." ) );

    /* Řádek 2: Selection info + bulk akce + file ops na jednom řádku. */
    size_t sel_count = g_vu.selected.size ( );
    ImGui::Text ( "%s %zu", _( "Selected:" ), sel_count );
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( sel_count == 0 );
    if ( ImGui::Button ( _L ( "Delete##vars_delsel" ) ) ) {
        g_vu.show_delete_selected_confirm = true;
    };
    vars_item_tooltip ( _( "Delete selected variables (with confirm)." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Set 0##vars_zerosel" ) ) ) {
        for ( const auto &nm : g_vu.selected ) {
            bp_var_set ( nm.c_str ( ), 0 );
        };
    };
    vars_item_tooltip ( _( "Reset selected variable values to 0." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "+1##vars_incsel" ) ) ) {
        for ( const auto &nm : g_vu.selected ) {
            int32_t cur = bp_var_get ( nm.c_str ( ) );
            bp_var_set ( nm.c_str ( ), cur + 1 );
        };
    };
    vars_item_tooltip ( _( "Increment selected variable values by 1." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "-1##vars_decsel" ) ) ) {
        for ( const auto &nm : g_vu.selected ) {
            int32_t cur = bp_var_get ( nm.c_str ( ) );
            bp_var_set ( nm.c_str ( ), cur - 1 );
        };
    };
    vars_item_tooltip ( _( "Decrement selected variable values by 1." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Set...##vars_setsel" ) ) ) {
        g_vu.bulk_set_buf[0] = '\0';
        g_vu.bulk_set_error[0] = '\0';
        ImGui::OpenPopup ( "###vars_bulk_set_popup" );
    };
    vars_item_tooltip ( _( "Set selected variables to specific value "
                           "(IASM format: 42 / 0x2A / #2A / %101010)." ) );
    ImGui::EndDisabled ( );

    /* Bulk Set value popup. */
    if ( ImGui::BeginPopup ( "###vars_bulk_set_popup" ) ) {
        ImGui::TextUnformatted ( _( "Set value for all selected:" ) );
        ImGui::SetNextItemWidth ( 200.0f );
        if ( ImGui::IsWindowAppearing ( ) ) ImGui::SetKeyboardFocusHere ( );
        bool submitted = ImGui::InputText ( "###vars_bulk_set_input",
                                              g_vu.bulk_set_buf,
                                              sizeof ( g_vu.bulk_set_buf ),
                                              ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SameLine ( );
        bool ok_clicked = ImGui::Button ( _L ( "OK##vars_bulk_set_ok" ) );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_bulk_set_cancel" ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        if ( submitted || ok_clicked ) {
            int32_t parsed = 0;
            if ( !vars_value_parse ( g_vu.bulk_set_buf, &parsed ) ) {
                snprintf ( g_vu.bulk_set_error,
                           sizeof ( g_vu.bulk_set_error ),
                           "Invalid format" );
            } else {
                for ( const auto &nm : g_vu.selected ) {
                    bp_var_set ( nm.c_str ( ), parsed );
                };
                ImGui::CloseCurrentPopup ( );
            };
        };
        if ( g_vu.bulk_set_error[0] ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                                 g_vu.bulk_set_error );
        };
        ImGui::EndPopup ( );
    };

    /* Vizuální oddělovač mezi bulk ops a file ops. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Save##vars_save" ) ) ) {
        char errbuf[256] = {0};
        if ( bp_vars_save_to_filepath ( NULL, errbuf, sizeof ( errbuf ) ) ) {
            char *def = bp_vars_default_filepath ( );
            snprintf ( g_vu.status, sizeof ( g_vu.status ),
                       "Saved to %s", def ? def : "(default)" );
            if ( def ) g_free ( def );
        } else {
            snprintf ( g_vu.status, sizeof ( g_vu.status ),
                       "Save failed: %s", errbuf );
        };
    };
    vars_item_tooltip ( _( "Save to default per-arch .vars file "
                           "(mz800.vars / mz1500.vars in cfg dir)." ) );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Save As...##vars_saveas" ) ) ) {
        IGFD::FileDialogConfig config;
        char *def = bp_vars_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles |
                       ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "VarsSaveDialog", _( "Save Variables As..." ),
            ".vars", config );
        if ( def ) g_free ( def );
    };
    vars_item_tooltip ( _( "Save variables to a chosen file." ) );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Load From...##vars_loadfrom" ) ) ) {
        IGFD::FileDialogConfig config;
        char *def = bp_vars_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "VarsLoadDialog", _( "Load Variables From..." ),
            ".vars,.*", config );
        if ( def ) g_free ( def );
    };
    vars_item_tooltip ( _( "Replace current variables with file content." ) );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Merge...##vars_merge" ) ) ) {
        IGFD::FileDialogConfig config;
        char *def = bp_vars_default_filepath ( );
        config.path = def ? def : ".";
        config.countSelectionMax = 1;
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "VarsMergeDialog", _( "Merge Variables From..." ),
            ".vars,.*", config );
        if ( def ) g_free ( def );
    };
    vars_item_tooltip ( _( "Add variables from file. On name conflict, "
                           "you choose Overwrite or Skip." ) );

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( total_count == 0 );
    if ( ImGui::Button ( _L ( "Clear Values##vars_clrval" ) ) ) {
        g_vu.show_clear_values_confirm = true;
    };
    vars_item_tooltip ( _( "Reset all variable values to 0 (records remain)." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear All##vars_clrall" ) ) ) {
        g_vu.show_clear_all_confirm = true;
    };
    vars_item_tooltip ( _( "Delete ALL variable records." ) );
    ImGui::EndDisabled ( );

    if ( g_vu.status[0] ) {
        ImGui::TextDisabled ( "%s", g_vu.status );
    };
}


/**
 * @brief Render Add Var formuláře jako collapsible block POD sticky header.
 *
 * Renderuje se jen když g_vu.add_form_open == true. Block je MIMO sticky
 * header child (= dříve překrýval řádek 3 sticky header při expandu).
 *
 * Layout: rámeček s name + value + persist + comment inputy + OK/Cancel.
 * Po úspěšném submit / klik Cancel se add_form_open vyresetuje.
 */
static void vars_render_add_form_block ( void )
{
    if ( !g_vu.add_form_open ) return;

    ImGui::BeginGroup ( );
    /* Rámeček pro vizuální oddělení od sticky header a tabulky. */
    ImGui::PushStyleVar ( ImGuiStyleVar_FramePadding, ImVec2 ( 6, 4 ) );
    ImGui::BeginChild ( "###vars_add_form", ImVec2 ( 0, 0 ),
                         ImGuiChildFlags_AutoResizeY |
                         ImGuiChildFlags_Borders );

    /* Layout přes ImGui::BeginTable s 3 sloupci. Šířky **dynamicky** per
     * skutečný obsah (= žádné hardcoded px - lokalizační tolerance):
     *   - sloupec "label"  = fit content (= max šířka "Name:" / "Comment:")
     *   - sloupec "input"  = stretch (= textentry roste s velikostí okna)
     *   - sloupec "rest"   = fit content (= Value: + input + Persist resp.
     *                       OK + Cancel - širší ze dvou určuje šířku) */

    /* Spočítat šířky pro right column dynamicky. */
    const ImGuiStyle &style = ImGui::GetStyle ( );
    const char *value_lbl = _( "Value:" );
    const char *persist_lbl = _L ( "Persist##vars_add_persist" );
    const char *ok_lbl = _L ( "OK##vars_add_ok" );
    const char *cancel_lbl = _L ( "Cancel##vars_add_cancel" );

    /* Šířka řádku 1 right side: "Value:" + spacing + input(80) + spacing
     * + checkbox(square) + spacing + label "Persist". */
    float value_input_w = ImGui::CalcTextSize ( "0123456" ).x
                            + style.FramePadding.x * 2.0f;
    float row1_rest_w = ImGui::CalcTextSize ( value_lbl ).x
                          + style.ItemSpacing.x
                          + value_input_w
                          + style.ItemSpacing.x
                          + ImGui::GetFrameHeight ( )      /* checkbox square */
                          + style.ItemInnerSpacing.x
                          + ImGui::CalcTextSize ( persist_lbl, NULL, true ).x;

    /* Šířka řádku 2 right side: OK + spacing + Cancel (Buttony s
     * FramePadding.x kolem labelu). */
    float ok_btn_w = ImGui::CalcTextSize ( ok_lbl, NULL, true ).x
                       + style.FramePadding.x * 2.0f;
    float cancel_btn_w = ImGui::CalcTextSize ( cancel_lbl, NULL, true ).x
                           + style.FramePadding.x * 2.0f;
    float row2_rest_w = ok_btn_w + style.ItemSpacing.x + cancel_btn_w;

    float rest_w = ( row1_rest_w > row2_rest_w ) ? row1_rest_w : row2_rest_w;
    rest_w += style.CellPadding.x * 2.0f;   /* table cell padding */

    if ( ImGui::BeginTable ( "###vars_add_form_table", 3,
                              ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn ( "label",  ImGuiTableColumnFlags_WidthFixed );
        ImGui::TableSetupColumn ( "input",  ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn ( "rest",   ImGuiTableColumnFlags_WidthFixed,
                                   rest_w );

        /* Řádek 1: Name | textentry | Value: + input + Persist */
        ImGui::TableNextRow ( );
        ImGui::TableNextColumn ( );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _( "Name:" ) );

        ImGui::TableNextColumn ( );
        ImGui::SetNextItemWidth ( -FLT_MIN );
        ImGui::InputText ( "###vars_add_name",
                            g_vu.add_name, sizeof ( g_vu.add_name ) );

        ImGui::TableNextColumn ( );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( value_lbl );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( value_input_w );
        ImGui::InputText ( "###vars_add_value",
                            g_vu.add_value, sizeof ( g_vu.add_value ) );
        ImGui::SameLine ( );
        ImGui::Checkbox ( persist_lbl, &g_vu.add_persist );
        vars_item_tooltip ( _( "If unchecked, value is reset to 0 on each "
                               "load (only name + comment persist)." ) );

        /* Řádek 2: Comment | textentry | OK + Cancel */
        ImGui::TableNextRow ( );
        ImGui::TableNextColumn ( );
        ImGui::AlignTextToFramePadding ( );
        ImGui::TextUnformatted ( _( "Comment:" ) );

        ImGui::TableNextColumn ( );
        ImGui::SetNextItemWidth ( -FLT_MIN );
        ImGui::InputText ( "###vars_add_comment",
                            g_vu.add_comment, sizeof ( g_vu.add_comment ) );

        ImGui::TableNextColumn ( );
        if ( ImGui::Button ( ok_lbl, ImVec2 ( ok_btn_w, 0 ) ) ) {
            if ( vars_add_form_submit ( ) ) {
                g_vu.add_form_open = false;
            };
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( cancel_lbl, ImVec2 ( cancel_btn_w, 0 ) ) ) {
            vars_add_form_reset ( );
            g_vu.add_form_open = false;
        };

        ImGui::EndTable ( );
    };

    if ( g_vu.add_error[0] ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                             g_vu.add_error );
    };

    ImGui::EndChild ( );
    ImGui::PopStyleVar ( );
    ImGui::EndGroup ( );
}


/* ========================================================================= */
/*  Tabulka                                                                  */
/* ========================================================================= */


/**
 * @brief Render jednoho řádku tabulky pro var s daným indexem.
 *
 * Layout (7 sloupců): [sel] | Name | Value | Hex | Persist | Comment | [x]
 *
 * Persist sloupec má samostatnou hlavičku "Persist" + jen checkbox v buňce.
 * Delete tlačítko je v posledním samostatném sloupci (= dříve bylo SameLine
 * v Comment buňce, mátlo).
 *
 * @param idx                Index v storage.
 * @param out_pending_delete Pokud user klikne na "x", jméno se zapíše sem.
 * @return true pokud řádek byl render (= prošel filtrem).
 */
static bool vars_render_row ( size_t idx, std::string *out_pending_delete )
{
    const bp_var_t *v = bp_vars_get_by_index ( idx );
    if ( !v || !v->name ) return false;
    if ( !vars_filter_match ( v ) ) return false;

    /* Snapshot dat pro stabilní pointry (storage realloc při set_comment). */
    std::string row_name = v->name;
    int32_t row_value = v->value;
    std::string row_comment = v->comment ? v->comment : "";
    bool row_persist = v->persist_value;

    ImGui::PushID ( (int) idx );
    ImGui::TableNextRow ( );

    /* Sloupec 0: Checkbox (selection). */
    ImGui::TableNextColumn ( );
    bool selected = g_vu.selected.count ( row_name ) > 0;
    if ( ImGui::Checkbox ( "###vars_sel", &selected ) ) {
        if ( selected ) g_vu.selected.insert ( row_name );
        else g_vu.selected.erase ( row_name );
    };

    /* Sloupec 1: Name (rename inline edit). */
    ImGui::TableNextColumn ( );
    bool is_editing_name =
        g_vu.editing_name == row_name &&
        g_vu.editing_field == VarsUiState::EditField::Name;
    if ( is_editing_name ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_vu.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_vu.edit_focus_pending = false;
        };
        if ( ImGui::InputText ( "###vars_edit_name", g_vu.edit_buffer,
                                  sizeof ( g_vu.edit_buffer ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            vars_apply_edit ( );
        };
        if ( ImGui::IsItemDeactivated ( ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_Enter ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) vars_cancel_edit ( );
            else vars_apply_edit ( );
        };
        if ( g_vu.edit_error[0] ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                                 g_vu.edit_error );
        };
    } else {
        char name_buf[BP_VAR_NAME_MAX + 4];
        snprintf ( name_buf, sizeof ( name_buf ), "$%s", row_name.c_str ( ) );
        /* Selectable s AllowDoubleClick = spolehlivý double-click detect
         * (= ImGui::Text není interaktivní item, IsMouseDoubleClicked
         * tam nemusí fungovat per scope). */
        ImGui::Selectable ( name_buf, false,
                            ImGuiSelectableFlags_AllowDoubleClick );
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            vars_begin_edit_name ( v );
        };
        vars_item_tooltip ( _( "Double-click to rename. Existing BPs "
                               "with $oldname references stay unchanged "
                               "(no refactor)." ) );
    };

    /* Sloupec 2: Value (inline edit). */
    ImGui::TableNextColumn ( );
    bool is_editing_value =
        g_vu.editing_name == row_name &&
        g_vu.editing_field == VarsUiState::EditField::Value;
    if ( is_editing_value ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_vu.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_vu.edit_focus_pending = false;
        };
        if ( ImGui::InputText ( "###vars_edit_val", g_vu.edit_buffer,
                                  sizeof ( g_vu.edit_buffer ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            vars_apply_edit ( );
        };
        if ( ImGui::IsItemDeactivated ( ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_Enter ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter ) ) {
            /* Click mimo / Esc - pokud Esc, cancel; jinak apply. */
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) vars_cancel_edit ( );
            else vars_apply_edit ( );
        };
        if ( g_vu.edit_error[0] ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                                 g_vu.edit_error );
        };
    } else {
        char val_buf[32];
        snprintf ( val_buf, sizeof ( val_buf ), "%d", (int) row_value );
        ImGui::Selectable ( val_buf, false,
                            ImGuiSelectableFlags_AllowDoubleClick );
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            vars_begin_edit_value ( v );
        };
        vars_item_tooltip ( _( "Double-click to edit value (decimal "
                               "or IASM format: 42 / 0x2A / #2A / %101010)." ) );
    };

    /* Sloupec 3: Hex (inline edit - reuses Value apply path). */
    ImGui::TableNextColumn ( );
    bool is_editing_hex =
        g_vu.editing_name == row_name &&
        g_vu.editing_field == VarsUiState::EditField::Hex;
    if ( is_editing_hex ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_vu.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_vu.edit_focus_pending = false;
        };
        if ( ImGui::InputText ( "###vars_edit_hex", g_vu.edit_buffer,
                                  sizeof ( g_vu.edit_buffer ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            vars_apply_edit ( );
        };
        if ( ImGui::IsItemDeactivated ( ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_Enter ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) vars_cancel_edit ( );
            else vars_apply_edit ( );
        };
        if ( g_vu.edit_error[0] ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                                 g_vu.edit_error );
        };
    } else {
        char hex_buf[16];
        vars_value_format_hex ( row_value, hex_buf, sizeof ( hex_buf ) );
        /* Selectable s disabled-text colorem (= hex je read-only display
         * dokud user nedvojklikne). */
        ImGui::PushStyleColor ( ImGuiCol_Text,
            ImGui::GetStyleColorVec4 ( ImGuiCol_TextDisabled ) );
        ImGui::Selectable ( hex_buf, false,
                            ImGuiSelectableFlags_AllowDoubleClick );
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            vars_begin_edit_hex ( v );
        };
        vars_item_tooltip ( _( "Double-click to edit value in hex "
                               "(prefilled as 0x{value})." ) );
    };

    /* Sloupec 4: Persist checkbox (jen checkbox, hlavička sloupce dělá label). */
    ImGui::TableNextColumn ( );
    bool persist = row_persist;
    if ( ImGui::Checkbox ( "###vars_persist", &persist ) ) {
        bp_var_set_persist ( row_name.c_str ( ), persist );
    };

    /* Sloupec 5: Comment (inline edit). */
    ImGui::TableNextColumn ( );
    bool is_editing_comment =
        g_vu.editing_name == row_name &&
        g_vu.editing_field == VarsUiState::EditField::Comment;
    if ( is_editing_comment ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_vu.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_vu.edit_focus_pending = false;
        };
        if ( ImGui::InputText ( "###vars_edit_cmt", g_vu.edit_buffer,
                                  sizeof ( g_vu.edit_buffer ),
                                  ImGuiInputTextFlags_EnterReturnsTrue ) ) {
            vars_apply_edit ( );
        };
        if ( ImGui::IsItemDeactivated ( ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_Enter ) &&
             !ImGui::IsKeyPressed ( ImGuiKey_KeypadEnter ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) vars_cancel_edit ( );
            else vars_apply_edit ( );
        };
    } else {
        const char *cmt_disp = row_comment.empty ( ) ? "(none)"
                                                     : row_comment.c_str ( );
        ImGui::Selectable ( cmt_disp, false,
                            ImGuiSelectableFlags_AllowDoubleClick );
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            vars_begin_edit_comment ( v );
        };
        vars_item_tooltip ( _( "Double-click to edit comment (free text, "
                               "max 256 chars)." ) );
    };

    /* Sloupec 6: Delete tlačítko (= samostatný sloupec, žádný confirm). */
    ImGui::TableNextColumn ( );
    if ( ImGui::SmallButton ( "x###vars_del_row" ) ) {
        if ( out_pending_delete ) *out_pending_delete = row_name;
    };
    vars_item_tooltip ( _( "Delete this variable (no confirm)." ) );

    ImGui::PopID ( );
    return true;
}


/* ========================================================================= */
/*  Confirm dialogy                                                          */
/* ========================================================================= */


static void vars_render_confirm_dialogs ( void )
{
    /* Clear All */
    if ( g_vu.show_clear_all_confirm ) {
        ImGui::OpenPopup ( "###vars_confirm_clrall" );
        g_vu.show_clear_all_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Clear All Variables?###vars_confirm_clrall" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        size_t count = bp_vars_count ( );
        ImGui::Text ( _( "Delete all %zu variables? This cannot be undone." ), count );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Delete All##vars_confirm_clrall_yes" ),
                              ImVec2 ( 140, 0 ) ) ) {
            bp_vars_clear_storage ( );
            g_vu.selected.clear ( );
            g_vu.editing_name.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_confirm_clrall_no" ),
                              ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Clear Values (= reset hodnot, záznamy zachovat) */
    if ( g_vu.show_clear_values_confirm ) {
        ImGui::OpenPopup ( "###vars_confirm_clrval" );
        g_vu.show_clear_values_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Clear All Values?###vars_confirm_clrval" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::TextUnformatted ( _( "Reset all variable values to 0?\n"
                                    "Records (name, comment, persist flag) remain." ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Reset Values##vars_confirm_clrval_yes" ),
                              ImVec2 ( 140, 0 ) ) ) {
            bp_vars_clear_all ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_confirm_clrval_no" ),
                              ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Delete selected */
    if ( g_vu.show_delete_selected_confirm ) {
        ImGui::OpenPopup ( "###vars_confirm_delsel" );
        g_vu.show_delete_selected_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Delete Selected Variables?###vars_confirm_delsel" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _( "Delete %zu selected variable(s)?" ), g_vu.selected.size ( ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Delete##vars_confirm_delsel_yes" ),
                              ImVec2 ( 140, 0 ) ) ) {
            /* Snapshot - protože unset posune storage. */
            std::vector<std::string> to_del ( g_vu.selected.begin ( ),
                                               g_vu.selected.end ( ) );
            for ( const auto &nm : to_del ) bp_var_unset ( nm.c_str ( ) );
            g_vu.selected.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_confirm_delsel_no" ),
                              ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Load From... = REPLACE confirm */
    if ( g_vu.show_load_replace_confirm ) {
        ImGui::OpenPopup ( "###vars_confirm_load" );
        g_vu.show_load_replace_confirm = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Replace All Variables?###vars_confirm_load" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _( "Replace current %zu variables with content of:" ),
                       bp_vars_count ( ) );
        ImGui::TextDisabled ( "%s", g_vu.pending_load_path.c_str ( ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Replace##vars_confirm_load_yes" ),
                              ImVec2 ( 140, 0 ) ) ) {
            char errbuf[256] = {0};
            if ( bp_vars_load_from_filepath ( g_vu.pending_load_path.c_str ( ),
                                                BP_VARS_LOAD_REPLACE,
                                                errbuf, sizeof ( errbuf ) ) ) {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Loaded from %s", g_vu.pending_load_path.c_str ( ) );
            } else {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Load failed: %s", errbuf );
            };
            g_vu.selected.clear ( );
            g_vu.editing_name.clear ( );
            g_vu.pending_load_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_confirm_load_no" ),
                              ImVec2 ( 100, 0 ) ) ) {
            g_vu.pending_load_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Merge From... = OVERWRITE / SKIP / CANCEL */
    if ( g_vu.show_merge_choice ) {
        ImGui::OpenPopup ( "###vars_confirm_merge" );
        g_vu.show_merge_choice = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Merge Variables?###vars_confirm_merge" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::TextUnformatted ( _( "Merge from:" ) );
        ImGui::TextDisabled ( "%s", g_vu.pending_merge_path.c_str ( ) );
        ImGui::Separator ( );
        ImGui::TextUnformatted ( _( "On name conflict between existing and file:" ) );
        ImGui::Spacing ( );
        if ( ImGui::Button ( _L ( "Overwrite all##vars_merge_overwrite" ),
                              ImVec2 ( 160, 0 ) ) ) {
            char errbuf[256] = {0};
            if ( bp_vars_load_from_filepath ( g_vu.pending_merge_path.c_str ( ),
                                                BP_VARS_LOAD_MERGE_OVERWRITE,
                                                errbuf, sizeof ( errbuf ) ) ) {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Merged (overwrite) from %s",
                           g_vu.pending_merge_path.c_str ( ) );
            } else {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Merge failed: %s", errbuf );
            };
            g_vu.pending_merge_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Skip all##vars_merge_skip" ),
                              ImVec2 ( 120, 0 ) ) ) {
            char errbuf[256] = {0};
            if ( bp_vars_load_from_filepath ( g_vu.pending_merge_path.c_str ( ),
                                                BP_VARS_LOAD_MERGE_SKIP,
                                                errbuf, sizeof ( errbuf ) ) ) {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Merged (skip conflicts) from %s",
                           g_vu.pending_merge_path.c_str ( ) );
            } else {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Merge failed: %s", errbuf );
            };
            g_vu.pending_merge_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##vars_merge_cancel" ),
                              ImVec2 ( 100, 0 ) ) ) {
            g_vu.pending_merge_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };
}


/* ========================================================================= */
/*  File dialogy (= IGFD)                                                    */
/* ========================================================================= */


static void vars_render_file_dialogs ( void )
{
    if ( ImGuiFileDialog::Instance ( )->Display ( "VarsSaveDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            char errbuf[256] = {0};
            if ( bp_vars_save_to_filepath ( path.c_str ( ),
                                              errbuf, sizeof ( errbuf ) ) ) {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Saved to %s", path.c_str ( ) );
            } else {
                snprintf ( g_vu.status, sizeof ( g_vu.status ),
                           "Save failed: %s", errbuf );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    if ( ImGuiFileDialog::Instance ( )->Display ( "VarsLoadDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            g_vu.pending_load_path =
                ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            g_vu.show_load_replace_confirm = true;
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    if ( ImGuiFileDialog::Instance ( )->Display ( "VarsMergeDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            g_vu.pending_merge_path =
                ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            g_vu.show_merge_choice = true;
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };
}


/* ========================================================================= */
/*  Hlavní render                                                            */
/* ========================================================================= */


/**
 * @brief Render hlavičky tabulky s custom select-all checkboxem v sloupci 0.
 *
 * Místo standardního TableHeadersRow renderujeme řádek manuálně:
 *   - Sloupec 0: tristate checkbox (none/some/all visible selected)
 *   - Sloupce 1-5: standardní TableHeader s label z TableSetupColumn
 *   - Sloupec 6: prázdná hlavička (delete tlačítko nemá popis)
 *
 * Toggle select-all setuje selection na všechny aktuálně VISIBLE řádky
 * (= prošlé filtrem), nebo všechny odznačí pokud už jsou vybrané všechny.
 *
 * @param total_count Celkový počet ve storage (pro iteraci).
 */
static void vars_render_table_header ( size_t total_count )
{
    /* Spočítej vybrané + visible vars pro tristate stav. */
    size_t visible = 0;
    size_t visible_selected = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const bp_var_t *v = bp_vars_get_by_index ( i );
        if ( !v || !v->name ) continue;
        if ( !vars_filter_match ( v ) ) continue;
        visible++;
        if ( g_vu.selected.count ( v->name ) > 0 ) visible_selected++;
    };

    bool all_selected = ( visible > 0 && visible_selected == visible );
    bool none_selected = ( visible_selected == 0 );

    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );

    /* Sloupec 0: select-all tristate checkbox (custom widget přes DrawList).
     * ImGui::Checkbox nemá nativní tristate, vyrobíme ručně:
     *   - none  = prázdný čtvereček
     *   - all   = standardní checkmark
     *   - some  = vyplněný čtvereček uprostřed (= "indeterminate")
     */
    ImGui::TableSetColumnIndex ( 0 );
    ImGui::PushID ( "vars_selall" );
    {
        float h = ImGui::GetFrameHeight ( );
        ImVec2 size ( h, h );
        ImVec2 pos = ImGui::GetCursorScreenPos ( );
        ImGui::InvisibleButton ( "###vars_selall_btn", size );
        bool clicked = ImGui::IsItemClicked ( );
        bool hovered = ImGui::IsItemHovered ( );
        bool active = ImGui::IsItemActive ( );

        ImDrawList *dl = ImGui::GetWindowDrawList ( );
        ImU32 bg_col = ImGui::GetColorU32 ( active ? ImGuiCol_FrameBgActive
                                            : hovered ? ImGuiCol_FrameBgHovered
                                            : ImGuiCol_FrameBg );
        ImVec2 br ( pos.x + size.x, pos.y + size.y );
        dl->AddRectFilled ( pos, br, bg_col, 3.0f );

        ImU32 mark_col = ImGui::GetColorU32 ( ImGuiCol_CheckMark );
        if ( all_selected ) {
            /* Checkmark per ImGui::RenderCheckMark algoritmus
             * (= shoda visualu s standardním ImGui::Checkbox v řádcích).
             * Source: imgui_widgets.cpp v ImGui >= 1.88. */
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
            /* "Some" = vyplněný menší čtvereček uprostřed (indeterminate). */
            float pad = h * 0.28f;
            ImVec2 inner_min ( pos.x + pad,            pos.y + pad );
            ImVec2 inner_max ( pos.x + h - pad,        pos.y + h - pad );
            dl->AddRectFilled ( inner_min, inner_max, mark_col, 1.5f );
        };
        /* none_selected = prázdný čtvereček (= jen background, bez marku) */

        if ( clicked ) {
            if ( all_selected ) {
                /* Vše vybráno -> odznačit. */
                for ( size_t i = 0; i < total_count; i++ ) {
                    const bp_var_t *v = bp_vars_get_by_index ( i );
                    if ( !v || !v->name ) continue;
                    if ( !vars_filter_match ( v ) ) continue;
                    g_vu.selected.erase ( v->name );
                };
            } else {
                /* Some/none -> označit vše visible. */
                for ( size_t i = 0; i < total_count; i++ ) {
                    const bp_var_t *v = bp_vars_get_by_index ( i );
                    if ( !v || !v->name ) continue;
                    if ( !vars_filter_match ( v ) ) continue;
                    g_vu.selected.insert ( v->name );
                };
            };
        };
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) {
        if ( all_selected ) {
            ImGui::SetTooltip ( "%s",
                _( "Deselect all visible variables." ) );
        } else if ( none_selected ) {
            ImGui::SetTooltip ( "%s",
                _( "Select all visible variables." ) );
        } else {
            ImGui::SetTooltip ( "%s",
                _( "Some variables selected. Click to select all visible." ) );
        };
    };
    ImGui::PopID ( );

    /* Sloupce 1-5: standardní hlavičky. */
    for ( int col = 1; col <= 5; col++ ) {
        ImGui::TableSetColumnIndex ( col );
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
    };

    /* Sloupec 6: prázdná hlavička pro delete tlačítko. */
    ImGui::TableSetColumnIndex ( 6 );
    ImGui::TableHeader ( "" );
}


void vars_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) return;

    /* 1162x430 default - sticky header řádek 2 (bulk ops + file ops) má
     * cca 1120px šířky vč. mezer; min 760x300 zabraňuje overflow file ops
     * vpravo, pokud user okno zúží. */
    ImGui::SetNextWindowSize ( ImVec2 ( 1162, 430 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 760, 300 ),
                                            ImVec2 ( FLT_MAX, FLT_MAX ) );

    if ( !ImGui::Begin ( _L ( "Variables###bpt_vars_panel" ), p_open,
                          ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_HorizontalScrollbar ) ) {
        ImGui::End ( );
        return;
    };

    /* Snapshot count + visible_count před render (= konzistence). */
    size_t total_count = bp_vars_count ( );
    size_t visible_count = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const bp_var_t *v = bp_vars_get_by_index ( i );
        if ( v && v->name && vars_filter_match ( v ) ) visible_count++;
    };

    /* Sticky header + Add form render PŘÍMO do window (= ne v BeginChild),
     * aby horizontal scrollbar byl WINDOW-level a scrolloval všechno
     * dohromady (= sticky řádek + tabulka pod ním scrollují spolu).
     * Sticky behavior zachováno: tabulka v BeginChild s vyplněním
     * zbytku window height = scrolluje vertikálně uvnitř, sticky region
     * zůstává nahoře. */
    vars_render_sticky_header ( visible_count, total_count );

    /* Add Var collapsible block POD sticky header. */
    vars_render_add_form_block ( );

    ImGui::Separator ( );

    /* Tabulka v scrollable child - bez horizontal scrollbar flag
     * (= horizontal scroll je window-level, sloupce stretch zaplní). */
    ImGui::BeginChild ( "###vars_table_child", ImVec2 ( 0, 0 ), false,
                         ImGuiWindowFlags_None );
    if ( total_count == 0 ) {
        ImGui::TextDisabled ( "%s", _( "No variables defined. "
                                       "Use [+ Add] above or define via "
                                       "smart breakpoint action." ) );
    } else {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_NoSavedSettings;
        if ( ImGui::BeginTable ( "###vars_tbl", 7, flags ) ) {
            ImGui::TableSetupScrollFreeze ( 0, 1 );
            /* Sloupce - sel/persist/delete fixed, comment stretch. */
            ImGui::TableSetupColumn ( "",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 26.0f );
            ImGui::TableSetupColumn ( _( "Name" ),
                                       ImGuiTableColumnFlags_WidthFixed, 140.0f );
            ImGui::TableSetupColumn ( _( "Value" ),
                                       ImGuiTableColumnFlags_WidthFixed, 80.0f );
            ImGui::TableSetupColumn ( _( "Hex" ),
                                       ImGuiTableColumnFlags_WidthFixed, 80.0f );
            ImGui::TableSetupColumn ( _( "Persist" ),
                                       ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableSetupColumn ( _( "Comment" ),
                                       ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn ( "",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 28.0f );
            /* Custom hlavička se select-all checkboxem v sloupci 0. */
            vars_render_table_header ( total_count );

            std::string pending_delete;
            for ( size_t i = 0; i < total_count; i++ ) {
                vars_render_row ( i, &pending_delete );
                if ( !pending_delete.empty ( ) ) {
                    /* Single-row delete - no confirm dialog (per spec
                     * "rychlý workflow"). Selection cleanup. */
                    g_vu.selected.erase ( pending_delete );
                    if ( g_vu.editing_name == pending_delete ) {
                        vars_cancel_edit ( );
                    };
                    bp_var_unset ( pending_delete.c_str ( ) );
                    /* Storage se posunul - break (= další iterace by
                     * četla invalidní index). Re-render příští frame. */
                    break;
                };
            };
            ImGui::EndTable ( );
        };
    };
    ImGui::EndChild ( );

    /* Confirm dialogy + file dialogy (= ImGui popup machinery render
     * po hlavním obsahu, ale state se nastavil v sticky header). */
    vars_render_confirm_dialogs ( );
    vars_render_file_dialogs ( );

    ImGui::End ( );
}


void vars_window_show_hide ( void )
{
    g_gui->showVarsWindow = !g_gui->showVarsWindow;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
