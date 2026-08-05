/*
 * watch_window.cpp - Watch panel UI (Phase A = L0 + Phase B = L1 typy
 *                                     + Phase C = L2 expression + Save/Load
 *                                     + Phase D = UX polish).
 *
 * Layout Phase D:
 *   Sticky header (2 řádky):
 *     Řádek 1: [+ Add]  Filter:[__________] [Clear]  (vis/tot)
 *     Řádek 2: Selected: N  [Del]  |  [Save] [Save As...] [Load From...]
 *                                     [Clear All...]
 *   Tabulka 7 sloupců (Phase E):
 *     [sel] | [::] | Name | Addr/Expr | Type | Value | [x]
 *
 *   Sloupec 0:
 *     - hlavička: tristate select-all (none / partial / all visible)
 *     - tělo:     per-row selection checkbox
 *
 *   Sloupec 1 (Phase E):
 *     - drag handle "::" - DragDropSource pro reorder řádků.
 *       Drop target je celá Selectable. Backend: watch_move().
 *
 *   Sloupec 2 (Name):
 *     - inline rename: double-click NEBO context menu (Phase D)
 *     - context menu (pravý klik kdekoliv v řádku):
 *         Rename / Delete / Copy expression (do clipboardu)
 *     - tooltip: "Double-click to rename"
 *
 *   Sloupec 3 (Addr/Expr):
 *     - ADDRESS mode -> hex 0xXXXX
 *     - EXPR_* mode  -> zkrácený expr s tooltipem (mode short label + plný expr)
 *
 *   Sloupec 5 (Value):
 *     - pravý klik na buňce = Format popup (dec/hex/bin/char) - jen pro int typy
 *
 *   Sloupec 6 (Delete):
 *     - SmallButton x = single-row delete bez confirmu
 *
 * Filter:
 *   - Case-insensitive substring přes name, expr_text (EXPR módy), hex addr
 *     (ADDRESS mode), type label.
 *   - NEsahá do storage - jen UI visibility (skip v render loop).
 *
 * Selection:
 *   - std::set<int> stable runtime IDs (`st_WATCH_ROW.id`). Po delete/move
 *     selection zůstává konzistentní (IDs jsou stabilní napříč mutacemi).
 *   - Bulk Delete: confirm popup pro N > 1.
 *
 * "+ Add" popup (beze změny oproti Phase C):
 *   * Mode dropdown: Address / Expression (scalar) / Expression (pointer deref)
 *   * Pro ADDRESS: Address input + Name input
 *   * Pro EXPR_*: Expression input + Name input
 *   * Type dropdown (vždy)
 *   * Length spinner (jen pro variabilní typy)
 *   * Bit index spinner 0-7 (jen pro WATCH_TYPE_BIT)
 *
 * Polling: kazdy frame cte hodnoty pres watch_read_int / watch_read_bytes
 * (= memory_read_byte_no_se v UI vlakne). EXPR módy navíc volaji
 * bp_expr_eval (side_effect=false = nepuštíme MEM_R BP).
 *
 * Race s EMU vlaknem akceptovany (stejný kompromis jako io_window).
 *
 * Sym DB autocomplete: vstup pro adresu se nejprve zkusi parsovat jako
 * cislo, jinak se hleda v sym_db (case-sensitive). Pro V1 to staci.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <cstdio>
#include <cstring>

#include <set>
#include <string>
#include <vector>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include "watch_window.h"

extern "C" {
#include "emulator/debugger/watch.h"
#include "emulator/debugger/watch_cache.h"  /* Phase E: backend cache + snapshot */
#include "emulator/debugger/watch_emu_cache.h"  /* V1.D.2.C: dispatch-side mirror */
#include "ui-imgui/mcp_activity/owner_badge.h"  /* V1.C.3 owner badge */
#include "mzarch/mzarch.h"        /* g_mzarch_main.reset_count (Phase E.3) */
}


/* ========================================================================= */
/*  UI state                                                                 */
/* ========================================================================= */


namespace {

/**
 * @brief Persistent UI state Watch okna (per-frame).
 *
 * - addr_input / name_input: buffer pro Add Watch popup
 * - add_*: rozšířený stav popupu (typ, length, bit)
 * - add_error: validation feedback pro popup
 * - editing_id / edit_buf: inline rename state (klíčuje přes stabilní ID,
 *   ne přes index - aby přežil reorder/delete jiných řádků)
 * - selected: set stabilních ID vybraných řádků (Phase D)
 * - filter: case-insensitive substring filter (Phase D)
 * - context_menu_id: ID řádku jehož context menu se má otevřít (Phase D)
 * - pending_*: deferred akce mimo render loop
 */
struct WatchUiState {
    /* Add popup. */
    char addr_input[128] = "";
    char expr_input[256] = "";        /* Phase C: výrazový input */
    char name_input[64]  = "";
    char add_error[128]  = "";
    int  add_mode        = WATCH_MODE_ADDRESS;  /* Phase C */
    int  add_type        = WATCH_TYPE_U8;
    int  add_length      = 8;       /* default pro ascii/mzascii/bytes */
    int  add_bit_index   = 0;

    /* Inline rename (Phase D: klíčuje přes stabilní ID, ne index). */
    int  editing_id    = -1;
    char edit_buf[64]  = "";
    bool edit_focus_pending = false;

    /* Phase D: Selection set + filter. */
    std::set<int> selected;
    char filter[128] = "";

    /* Phase D: pending rename request z context menu (= aktivovat edit
     * mode pro daný ID). */
    int pending_rename_id = -1;

    /* Pending delete (single-row delete = no confirm dialog pro rychly
     * workflow). Phase D: rovnez z context menu. */
    int pending_delete_id = -1;

    /* Phase D: pending bulk delete - confirm popup */
    bool pending_bulk_delete = false;

    /* Phase C: Save/Load/Clear status hláška. */
    char status_msg[256] = "";

    /* Phase C: confirm clear all (modální popup). */
    bool pending_clear_all = false;

    /* Phase C: Load From... -> REPLACE confirm. */
    std::string pending_load_path;

    /* Phase E.1: highlight changes toggle (default ON, perzist přes cfg). */
    bool highlight_changes = true;
    /* Phase E.1: fade trvání ms (perzist přes cfg, default 500). */
    int  highlight_fade_ms = 500;

    /* V1.E.6.A: pending focus z Activity dvojkliku - hodnota > 0 = render-
     * loop má najít řádek s watch.id == pending_focus_id, vyčistit filter
     * a scrollnout. Po spotřebě je 0. */
    int  pending_focus_id = 0;
};


}  /* anonymous namespace */

static WatchUiState g_wu;


/**
 * @brief Wrapper okolo backend `watch_cache_prune_stale` - vytahne
 *        live ID array z aktualniho storage stavu.
 */
static void watch_ui_prune_stale_cache ( void )
{
    size_t n = watch_count ( );
    if ( n == 0 ) {
        watch_cache_prune_stale ( NULL, 0 );
        /* V1.D.2.C: zároveň prune dispatch-side mirror. */
        watch_emu_cache_prune_stale ( NULL, 0 );
        return;
    };
    std::vector<int> live;
    live.reserve ( n );
    for ( size_t i = 0; i < n; i++ ) {
        const st_WATCH_ROW *r = watch_get ( i );
        if ( r ) live.push_back ( r->id );
    };
    watch_cache_prune_stale ( live.data ( ), live.size ( ) );
    /* V1.D.2.C: dispatch-side mirror se synchronizuje stejným seznamem. */
    watch_emu_cache_prune_stale ( live.data ( ), live.size ( ) );
}


/* ========================================================================= */
/*  Helpery                                                                  */
/* ========================================================================= */


/**
 * @brief Tooltip pro předchozí ImGui prvek (= "(?)" markery vynechány,
 *        tooltip se objeví přímo na hover).
 *
 * @param desc Text tooltipu (anglicky, user-facing).
 */
static void watch_item_tooltip ( const char *desc )
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
 * @brief Case-insensitive substring match (Phase D filter helper).
 *
 * Prázdný needle => match (= žádný filter aktivní).
 */
static bool watch_str_contains_ci ( const char *haystack, const char *needle )
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


/* Forward decl - implementace níže (po dropdown helperech). */
static const char *watch_type_label ( en_WATCH_TYPE t );


/**
 * @brief Filter match Phase D - kombinace name + expr_text / addr / type label.
 *
 * Match přes (case-insensitive substring):
 *   - row->name (pokud non-NULL)
 *   - row->expr_text (pro EXPR módy)
 *   - hex addr "0xXXXX" + IASM "XXXXh" pro ADDRESS mode
 *   - type label ("u8", "mzascii", apod.)
 *
 * @param r  Řádek (NULL = false).
 * @return true pokud match nebo filter prázdný.
 */
static bool watch_filter_match ( const st_WATCH_ROW *r )
{
    if ( !g_wu.filter[0] ) return true;
    if ( !r ) return false;
    if ( watch_str_contains_ci ( r->name, g_wu.filter ) ) return true;
    if ( r->expr_text && watch_str_contains_ci ( r->expr_text, g_wu.filter ) ) {
        return true;
    };
    if ( watch_str_contains_ci ( watch_type_label ( r->type ), g_wu.filter ) ) {
        return true;
    };
    if ( r->mode == WATCH_MODE_ADDRESS ) {
        char abuf[16];
        snprintf ( abuf, sizeof ( abuf ), "0x%04X", r->addr );
        if ( watch_str_contains_ci ( abuf, g_wu.filter ) ) return true;
        snprintf ( abuf, sizeof ( abuf ), "%04Xh", r->addr );
        if ( watch_str_contains_ci ( abuf, g_wu.filter ) ) return true;
        snprintf ( abuf, sizeof ( abuf ), "%04x", r->addr );  /* lowercase */
        if ( watch_str_contains_ci ( abuf, g_wu.filter ) ) return true;
    };
    return false;
}


/* ========================================================================= */
/*  Type / fmt label helpers                                                 */
/* ========================================================================= */


/**
 * @brief Vrátí display label pro `en_WATCH_TYPE`.
 *
 * Pouziva se v Type dropdown + Type sloupci tabulky.
 */
static const char *watch_type_label ( en_WATCH_TYPE t ) {
    switch ( t ) {
        case WATCH_TYPE_U8:      return "u8";
        case WATCH_TYPE_I8:      return "i8";
        case WATCH_TYPE_U16LE:   return "u16le";
        case WATCH_TYPE_U16BE:   return "u16be";
        case WATCH_TYPE_I16LE:   return "i16le";
        case WATCH_TYPE_I16BE:   return "i16be";
        case WATCH_TYPE_U32LE:   return "u32le";
        case WATCH_TYPE_U32BE:   return "u32be";
        case WATCH_TYPE_I32LE:   return "i32le";
        case WATCH_TYPE_I32BE:   return "i32be";
        case WATCH_TYPE_BIT:     return "bit";
        case WATCH_TYPE_ASCII:   return "ascii";
        case WATCH_TYPE_MZASCII: return "mzascii";
        case WATCH_TYPE_BYTES:   return "bytes";
    };
    return "?";
}


/**
 * @brief Vrátí display label pro `en_WATCH_FMT`.
 */
static const char *watch_fmt_label ( en_WATCH_FMT f ) {
    switch ( f ) {
        case WATCH_FMT_DEC:  return "dec";
        case WATCH_FMT_HEX:  return "hex";
        case WATCH_FMT_BIN:  return "bin";
        case WATCH_FMT_CHAR: return "char";
    };
    return "?";
}


/**
 * @brief Pole všech typů v UI pořadí (pro dropdown iteraci).
 */
static const en_WATCH_TYPE g_all_types[] = {
    WATCH_TYPE_U8,
    WATCH_TYPE_I8,
    WATCH_TYPE_U16LE,
    WATCH_TYPE_U16BE,
    WATCH_TYPE_I16LE,
    WATCH_TYPE_I16BE,
    WATCH_TYPE_U32LE,
    WATCH_TYPE_U32BE,
    WATCH_TYPE_I32LE,
    WATCH_TYPE_I32BE,
    WATCH_TYPE_BIT,
    WATCH_TYPE_ASCII,
    WATCH_TYPE_MZASCII,
    WATCH_TYPE_BYTES,
};

static const en_WATCH_FMT g_all_fmts[] = {
    WATCH_FMT_DEC,
    WATCH_FMT_HEX,
    WATCH_FMT_BIN,
    WATCH_FMT_CHAR,
};


/**
 * @brief Vrátí display label pro `en_WATCH_MODE` (= popisek v dropdown).
 *
 * Vrací statický řetězec bez gettext _() obal - lokalizace by ho
 * komplikovala v dropdown logice. User-facing texty v anglickém zdroji
 * jsou OK.
 */
static const char *watch_mode_label ( en_WATCH_MODE m ) {
    switch ( m ) {
        case WATCH_MODE_ADDRESS:     return "Address";
        case WATCH_MODE_EXPR_SCALAR: return "Expression (scalar)";
        case WATCH_MODE_EXPR_DEREF:  return "Expression (pointer deref)";
    };
    return "?";
}


/**
 * @brief Kratší label pro Addr/Expr sloupec u řádků v EXPR módu.
 *
 * Použito v tooltipu vedle plného expr textu.
 */
static const char *watch_mode_short_label ( en_WATCH_MODE m ) {
    switch ( m ) {
        case WATCH_MODE_ADDRESS:     return "addr";
        case WATCH_MODE_EXPR_SCALAR: return "expr scalar";
        case WATCH_MODE_EXPR_DEREF:  return "expr deref";
    };
    return "?";
}


static const en_WATCH_MODE g_all_modes[] = {
    WATCH_MODE_ADDRESS,
    WATCH_MODE_EXPR_SCALAR,
    WATCH_MODE_EXPR_DEREF,
};


/**
 * @brief Najde index řádku podle stabilního ID (Phase D helper).
 *
 * @param id  Stabilní ID (st_WATCH_ROW.id).
 * @return Index 0..count-1, nebo -1 pokud nenalezeno.
 */
static int watch_find_index_by_id ( int id )
{
    if ( id < 0 ) return -1;
    size_t n = watch_count ( );
    for ( size_t i = 0; i < n; i++ ) {
        const st_WATCH_ROW *r = watch_get ( i );
        if ( r && r->id == id ) return (int) i;
    };
    return -1;
}


/* ========================================================================= */
/*  Add popup                                                                */
/* ========================================================================= */


/**
 * @brief Render Add Watch popup (otevirany + Add buttonem).
 *
 * Parsuje address input pres watch_parse_address (hex/dec/symbol).
 * Pokud parse selze, zobrazi inline error a popup zustane otevreny.
 *
 * Po Add nastavi nove pridanemu radku typ + length + bit_index dle
 * popup state.
 */
static void watch_render_add_popup ( void )
{
    if ( ImGui::BeginPopup ( "###watch_add_popup" ) ) {

        ImGui::TextUnformatted ( _( "Add new watch entry:" ) );
        ImGui::Separator ( );

        /* Mode dropdown (Phase C). */
        ImGui::TextUnformatted ( _( "Mode:" ) );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 240.0f );
        en_WATCH_MODE cur_mode = (en_WATCH_MODE) g_wu.add_mode;
        if ( ImGui::BeginCombo ( "###watch_add_mode",
                                   watch_mode_label ( cur_mode ) ) ) {
            for ( size_t i = 0; i < sizeof ( g_all_modes ) / sizeof ( g_all_modes[0] );
                  i++ ) {
                en_WATCH_MODE m = g_all_modes[ i ];
                bool sel = ( m == cur_mode );
                if ( ImGui::Selectable ( watch_mode_label ( m ), sel ) ) {
                    g_wu.add_mode = (int) m;
                };
                if ( sel ) ImGui::SetItemDefaultFocus ( );
            };
            ImGui::EndCombo ( );
        };

        bool submit_by_enter = false;

        if ( cur_mode == WATCH_MODE_ADDRESS ) {
            ImGui::TextUnformatted ( _( "Address:" ) );
            ImGui::SameLine ( );
            ImGui::SetNextItemWidth ( 240.0f );
            if ( ImGui::IsWindowAppearing ( ) ) ImGui::SetKeyboardFocusHere ( );
            submit_by_enter = ImGui::InputText ( "###watch_add_addr",
                                                  g_wu.addr_input,
                                                  sizeof ( g_wu.addr_input ),
                                                  ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::TextDisabled ( "%s",
                _( "(e.g. 0xC080, 0C080h, 49280, or symbol name)" ) );
        } else {
            ImGui::TextUnformatted ( _( "Expression:" ) );
            ImGui::SameLine ( );
            ImGui::SetNextItemWidth ( 240.0f );
            if ( ImGui::IsWindowAppearing ( ) ) ImGui::SetKeyboardFocusHere ( );
            submit_by_enter = ImGui::InputText ( "###watch_add_expr",
                                                  g_wu.expr_input,
                                                  sizeof ( g_wu.expr_input ),
                                                  ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::TextDisabled ( "%s",
                _( "(e.g. [HL], {0x4000}, $framecnt, 0xC080+1, port[0xCE])" ) );
        };

        ImGui::TextUnformatted ( _( "Name:" ) );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 240.0f );
        ImGui::InputText ( "###watch_add_name",
                            g_wu.name_input,
                            sizeof ( g_wu.name_input ) );
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "%s", _( "(optional)" ) );

        /* Type dropdown. */
        ImGui::TextUnformatted ( _( "Type:" ) );
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 140.0f );
        en_WATCH_TYPE cur_type = (en_WATCH_TYPE) g_wu.add_type;
        if ( ImGui::BeginCombo ( "###watch_add_type", watch_type_label ( cur_type ) ) ) {
            for ( size_t i = 0; i < sizeof ( g_all_types ) / sizeof ( g_all_types[0] );
                  i++ ) {
                en_WATCH_TYPE t = g_all_types[ i ];
                bool sel = ( t == cur_type );
                if ( ImGui::Selectable ( watch_type_label ( t ), sel ) ) {
                    g_wu.add_type = (int) t;
                    /* Auto-update length default pri prepnuti typu. */
                    if ( watch_type_has_length ( t ) && g_wu.add_length < 1 ) {
                        g_wu.add_length = 8;
                    };
                };
                if ( sel ) ImGui::SetItemDefaultFocus ( );
            };
            ImGui::EndCombo ( );
        };

        /* Length spinner pro variabilni typy. */
        if ( watch_type_has_length ( cur_type ) ) {
            ImGui::TextUnformatted ( _( "Length:" ) );
            ImGui::SameLine ( );
            ImGui::SetNextItemWidth ( 120.0f );
            int maxl = (int) watch_type_max_length ( cur_type );
            if ( g_wu.add_length < 1 ) g_wu.add_length = 1;
            if ( g_wu.add_length > maxl ) g_wu.add_length = maxl;
            ImGui::InputInt ( "###watch_add_length", &g_wu.add_length, 1, 8 );
            if ( g_wu.add_length < 1 ) g_wu.add_length = 1;
            if ( g_wu.add_length > maxl ) g_wu.add_length = maxl;
            ImGui::SameLine ( );
            ImGui::TextDisabled ( "(1..%d)", maxl );
        };

        /* Bit index spinner pro WATCH_TYPE_BIT. */
        if ( cur_type == WATCH_TYPE_BIT ) {
            ImGui::TextUnformatted ( _( "Bit:" ) );
            ImGui::SameLine ( );
            ImGui::SetNextItemWidth ( 120.0f );
            if ( g_wu.add_bit_index < 0 ) g_wu.add_bit_index = 0;
            if ( g_wu.add_bit_index > 7 ) g_wu.add_bit_index = 7;
            ImGui::InputInt ( "###watch_add_bit", &g_wu.add_bit_index, 1, 1 );
            if ( g_wu.add_bit_index < 0 ) g_wu.add_bit_index = 0;
            if ( g_wu.add_bit_index > 7 ) g_wu.add_bit_index = 7;
            ImGui::SameLine ( );
            ImGui::TextDisabled ( "(0..7)" );
        };

        bool ok_clicked = ImGui::Button ( _L ( "Add##watch_add_ok" ) );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##watch_add_cancel" ) ) ) {
            g_wu.addr_input[0] = '\0';
            g_wu.expr_input[0] = '\0';
            g_wu.name_input[0] = '\0';
            g_wu.add_error[0]  = '\0';
            ImGui::CloseCurrentPopup ( );
        };

        if ( submit_by_enter || ok_clicked ) {
            const char *name_arg = g_wu.name_input[0] ? g_wu.name_input : NULL;
            int idx = -1;

            if ( cur_mode == WATCH_MODE_ADDRESS ) {
                uint16_t parsed_addr = 0;
                if ( !watch_parse_address ( g_wu.addr_input, &parsed_addr ) ) {
                    snprintf ( g_wu.add_error, sizeof ( g_wu.add_error ),
                               "%s", _( "Cannot parse address (unknown symbol "
                                        "or invalid format)." ) );
                } else {
                    idx = watch_add ( name_arg, parsed_addr, -1 );
                    if ( idx >= 0 ) {
                        watch_set_type ( idx, (en_WATCH_TYPE) g_wu.add_type );
                    };
                };
            } else {
                /* EXPR_SCALAR / EXPR_DEREF: validate non-empty + add. Parse
                 * error sám o sobě řádek nesmaže - user uvidí !parse: ... a
                 * může opravit. */
                if ( !g_wu.expr_input[0] ) {
                    snprintf ( g_wu.add_error, sizeof ( g_wu.add_error ),
                               "%s", _( "Expression cannot be empty." ) );
                } else {
                    idx = watch_add_expr ( name_arg, g_wu.expr_input,
                                            cur_mode,
                                            (en_WATCH_TYPE) g_wu.add_type );
                };
            };

            if ( idx >= 0 ) {
                if ( watch_type_has_length ( (en_WATCH_TYPE) g_wu.add_type ) ) {
                    watch_set_length ( idx, (uint16_t) g_wu.add_length );
                };
                if ( (en_WATCH_TYPE) g_wu.add_type == WATCH_TYPE_BIT ) {
                    watch_set_bit_index ( idx, (uint8_t) g_wu.add_bit_index );
                };
                g_wu.addr_input[0] = '\0';
                g_wu.expr_input[0] = '\0';
                g_wu.name_input[0] = '\0';
                g_wu.add_error[0]  = '\0';
                ImGui::CloseCurrentPopup ( );
            };
        };

        if ( g_wu.add_error[0] ) {
            ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                                 g_wu.add_error );
        };

        ImGui::EndPopup ( );
    };
}


/* ========================================================================= */
/*  Cache update + color fade (Phase E.1) + min/max (Phase E.3)              */
/* ========================================================================= */


/**
 * @brief Cti aktualní hodnotu radku + uloz do backend cache.
 *
 * Wrapper okolo watch_cache_update - cte int nebo bytes podle typu a
 * predaje backend modulu (jednotne API pro UI + testy).
 *
 * @param r        Pointer na radek (NULL = no-op).
 * @param out_int  Vystup - precterá int hodnota (jen pro int typy).
 * @param out_buf  Vystup - precterá data (pro string/bytes typy).
 * @param out_len  Vystup - pocet bajtu.
 */
static void watch_ui_cache_update ( const st_WATCH_ROW *r,
                                     uint64_t *out_int,
                                     uint8_t *out_buf,
                                     size_t *out_len )
{
    if ( out_int ) *out_int = 0;
    if ( out_len ) *out_len = 0;
    if ( !r ) return;

    int frame_no = ImGui::GetFrameCount ( );
    bool is_int_type = ( watch_type_has_format ( r->type ) ||
                          r->type == WATCH_TYPE_BIT );

    if ( is_int_type ) {
        uint64_t v = watch_read_int ( r );
        if ( out_int ) *out_int = v;
        watch_cache_update ( r->id, r->type, r->mode, v, NULL, 0, frame_no );
    } else {
        uint8_t buf[ WATCH_LENGTH_MAX_BYTES ];
        size_t got = 0;
        watch_read_bytes ( r, buf, sizeof ( buf ), &got );
        if ( out_buf && out_len ) {
            size_t to_copy = ( got > WATCH_LENGTH_MAX_BYTES )
                              ? WATCH_LENGTH_MAX_BYTES : got;
            memcpy ( out_buf, buf, to_copy );
            *out_len = to_copy;
        };
        watch_cache_update ( r->id, r->type, r->mode, 0, buf, got, frame_no );
    };

    /* V1.D.2.C: publikuj aktuální stav do dispatch-side mirroru, aby
     * MCP Resource `emulator://watch/snapshot/{name}` mohl číst snap +
     * delta + min/max + change_count z dispatch vlákna. Publikujeme jen
     * pojmenované řádky (= anonymní nelze lookup-ovat URI parametrem).
     *
     * 1-frame stale je akceptovatelné per scope (= UI vlákno publish,
     * dispatch vlákno read). Race-free díky GMutex v zrcadle. */
    if ( r->name && r->name[0] ) {
        const st_WATCH_CACHE_VIEW *v = watch_cache_get ( r->id );
        const st_WATCH_VALUE_SNAP *snap = watch_snapshot_get ( r->id );
        watch_emu_cache_publish ( r->id, v, r->name, snap, (int) r->type );
    };
}


/**
 * @brief Vrátí highlight color (lerped to default text) pro Value buňku
 *        Phase E.1. Pokud highlight neaktivní nebo fade vypršel, vrací
 *        výchozí text color z ImGui stylu.
 *
 * Lerp:
 *   - t = (now_frame - last_change_frame) / fade_frames; clamp 0..1
 *   - color = lerp(highlight_color, default_text, t)
 *
 * Předpoklad: ~60 fps. Fade_frames = highlight_fade_ms / (1000/60).
 *
 * @param r  Aktivní řádek (NULL = default).
 * @return ImVec4 RGBA pro PushStyleColor(Text, ...).
 */
static ImVec4 watch_value_color ( const st_WATCH_ROW *r )
{
    ImVec4 def_text = ImGui::GetStyleColorVec4 ( ImGuiCol_Text );
    if ( !r || !g_wu.highlight_changes ) return def_text;
    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( r->id );
    if ( !c || !c->valid || c->delta_dir == WATCH_DELTA_NONE ) return def_text;

    int frame_no = ImGui::GetFrameCount ( );
    int frames_passed = frame_no - c->last_change_frame;
    if ( frames_passed < 0 ) frames_passed = 0;

    /* Fade frames = ms / (1000/60). Min 1 aby se nedělilo nulou.
     * Použijeme live cfg getter, kdyby user změnil hodnotu externě. */
    int fade_ms = watch_cfg_get_highlight_fade_ms ( );
    if ( fade_ms < 1 ) fade_ms = g_wu.highlight_fade_ms;
    if ( fade_ms < 1 ) fade_ms = 500;
    int fade_frames = ( fade_ms * 60 ) / 1000;
    if ( fade_frames < 1 ) fade_frames = 1;
    if ( frames_passed >= fade_frames ) return def_text;

    float t = (float) frames_passed / (float) fade_frames;
    if ( t < 0.0f ) t = 0.0f;
    if ( t > 1.0f ) t = 1.0f;

    ImVec4 hi;
    if ( c->delta_dir == WATCH_DELTA_UP ) {
        /* GREEN */
        hi = ImVec4 ( 0.25f, 1.0f, 0.25f, 1.0f );
    } else if ( c->delta_dir == WATCH_DELTA_DOWN ) {
        /* RED */
        hi = ImVec4 ( 1.0f, 0.25f, 0.25f, 1.0f );
    } else {
        /* YELLOW (string changed). */
        hi = ImVec4 ( 1.0f, 1.0f, 0.25f, 1.0f );
    };

    ImVec4 out;
    out.x = hi.x + ( def_text.x - hi.x ) * t;
    out.y = hi.y + ( def_text.y - hi.y ) * t;
    out.z = hi.z + ( def_text.z - hi.z ) * t;
    out.w = hi.w + ( def_text.w - hi.w ) * t;
    return out;
}


/* ========================================================================= */
/*  Delta renderer (Phase E.2)                                               */
/* ========================================================================= */


/**
 * @brief Render Δ buňky pro řádek - rozdíl proti snapshot baseline.
 *
 * Pravidla:
 *   - snapshot.type != row.type -> "?" (= type changed)
 *   - radek pridany po snapshot (= nemá entry) -> "(new)"
 *   - int typy: signed delta s prefixem +/-, formát podle row->fmt
 *     ("+5", "-0x12", "+0b0010"); rovnost = "="
 *   - string typy: "≠" pokud bytes liší (= "neq"), "=" pokud shoda
 *
 * @param r  Řádek (NULL = no-op).
 */
static void watch_render_delta ( const st_WATCH_ROW *r )
{
    if ( !r ) return;
    const st_WATCH_VALUE_SNAP *snap = watch_snapshot_get ( r->id );
    if ( !snap ) {
        ImGui::TextDisabled ( "%s", _( "(new)" ) );
        return;
    };
    if ( snap->type != r->type ) {
        ImGui::TextDisabled ( "%s", _( "(type changed)" ) );
        return;
    };

    bool is_int = ( watch_type_has_format ( r->type ) ||
                     r->type == WATCH_TYPE_BIT );

    if ( is_int ) {
        uint64_t cur = watch_read_int ( r );
        if ( cur == snap->value_int ) {
            ImGui::TextDisabled ( "=" );
            return;
        };
        int64_t cs = watch_cache_value_to_signed ( r->type, cur );
        int64_t ss = watch_cache_value_to_signed ( r->type, snap->value_int );
        int64_t diff = cs - ss;
        char vbuf[64];
        uint64_t mag = ( diff < 0 ) ? (uint64_t) ( -diff ) : (uint64_t) diff;
        const char *sign = ( diff < 0 ) ? "-" : "+";

        /* Format magnitude podle row->fmt. Pro CHAR fallback na hex
         * (delta nedává smysl jako znak). */
        en_WATCH_FMT fmt = r->fmt;
        if ( fmt == WATCH_FMT_CHAR ) fmt = WATCH_FMT_HEX;

        switch ( fmt ) {
            case WATCH_FMT_HEX:
                snprintf ( vbuf, sizeof ( vbuf ), "%s0x%llX",
                            sign, (unsigned long long) mag );
                break;
            case WATCH_FMT_BIN: {
                /* Lehky bin formatter - max 32 bitu pro readable. */
                char bbuf[40];
                int p = 0;
                if ( mag == 0 ) {
                    bbuf[p++] = '0';
                } else {
                    uint64_t v = mag;
                    char rev[64];
                    int rp = 0;
                    while ( v > 0 ) {
                        rev[rp++] = ( v & 1 ) ? '1' : '0';
                        v >>= 1;
                    };
                    while ( rp > 0 ) bbuf[p++] = rev[--rp];
                };
                bbuf[p] = '\0';
                snprintf ( vbuf, sizeof ( vbuf ), "%s0b%s", sign, bbuf );
                break;
            }
            case WATCH_FMT_DEC:
            default:
                snprintf ( vbuf, sizeof ( vbuf ), "%s%llu",
                            sign, (unsigned long long) mag );
                break;
        };
        /* Color hint - up = zelena, down = cervena (mirne ztlumene). */
        ImVec4 col = ( diff > 0 ) ? ImVec4 ( 0.5f, 0.95f, 0.5f, 1.0f )
                                  : ImVec4 ( 0.95f, 0.5f, 0.5f, 1.0f );
        ImGui::TextColored ( col, "%s", vbuf );
    } else {
        /* String/bytes: read aktualni + memcmp. */
        uint8_t buf[ WATCH_LENGTH_MAX_BYTES ];
        size_t got = 0;
        watch_read_bytes ( r, buf, sizeof ( buf ), &got );
        bool same = ( got == snap->len ) &&
                     ( got == 0 || memcmp ( buf, snap->bytes, got ) == 0 );
        if ( same ) {
            ImGui::TextDisabled ( "=" );
        } else {
            ImGui::TextColored ( ImVec4 ( 0.95f, 0.85f, 0.4f, 1.0f ),
                                  "%s", _( "changed" ) );
        };
    };
}


/* ========================================================================= */
/*  Value renderer                                                           */
/* ========================================================================= */


/**
 * @brief Render Value bunky podle typu radku.
 *
 * Pro int typy -> watch_format_int. Pro bytes -> hex dump + tooltip pro
 * truncated. Pro ascii/mzascii -> escaped string s uvozovkami.
 *
 * Phase E.1: před render zavolá `watch_cache_update` (= update prev,
 * min/max, change_count) a wrappuje TextUnformatted s color lerp pro
 * fade highlight.
 *
 * @param r  Pointer na radek.
 */
static void watch_render_value ( const st_WATCH_ROW *r )
{
    if ( !r ) return;

    /* Phase C: parse error má přednost. */
    const char *err = watch_get_expr_error ( r );
    if ( err ) {
        char ebuf[ 320 ];
        snprintf ( ebuf, sizeof ( ebuf ), "!parse: %s", err );
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", ebuf );
        return;
    };

    /* Phase C: EXPR_SCALAR pro typy bez display fmt (= string/bytes) ->
     * placeholder, nemá smysl. */
    if ( r->mode == WATCH_MODE_EXPR_SCALAR &&
          !watch_type_has_format ( r->type ) &&
          r->type != WATCH_TYPE_BIT ) {
        ImGui::TextDisabled ( "%s", _( "(N/A for scalar expr)" ) );
        return;
    };

    /* Phase E.1: cache update (= prev/min/max/change_count) + color lerp. */
    uint64_t cached_int = 0;
    uint8_t cached_buf[ WATCH_LENGTH_MAX_BYTES ];
    size_t cached_len = 0;
    watch_ui_cache_update ( r, &cached_int, cached_buf, &cached_len );

    ImVec4 col = watch_value_color ( r );
    ImGui::PushStyleColor ( ImGuiCol_Text, col );

    char vbuf[512];

    switch ( r->type ) {

        case WATCH_TYPE_U8:
        case WATCH_TYPE_I8:
        case WATCH_TYPE_U16LE:
        case WATCH_TYPE_U16BE:
        case WATCH_TYPE_I16LE:
        case WATCH_TYPE_I16BE:
        case WATCH_TYPE_U32LE:
        case WATCH_TYPE_U32BE:
        case WATCH_TYPE_I32LE:
        case WATCH_TYPE_I32BE:
        case WATCH_TYPE_BIT: {
            watch_format_int ( r, cached_int, vbuf, sizeof ( vbuf ) );
            ImGui::TextUnformatted ( vbuf );
            break;
        }

        case WATCH_TYPE_ASCII:
        case WATCH_TYPE_MZASCII: {
            char text[ 512 ];
            watch_format_ascii ( cached_buf, cached_len, text, sizeof ( text ),
                                  r->type == WATCH_TYPE_MZASCII );
            /* Uvozovky kolem stringu pro snadnejsi vizualni separaci. */
            ImGui::Text ( "\"%s\"", text );
            break;
        }

        case WATCH_TYPE_BYTES: {
            bool truncated = watch_format_bytes ( cached_buf, cached_len, vbuf,
                                                   sizeof ( vbuf ), 16 );
            ImGui::TextUnformatted ( vbuf );
            if ( truncated && ImGui::IsItemHovered (
                    ImGuiHoveredFlags_AllowWhenOverlappedByItem ) ) {
                /* Tooltip s plnym dumpem (vsechny ctene bajty). */
                char full[ 2048 ];
                watch_format_bytes ( cached_buf, cached_len, full,
                                      sizeof ( full ), 0 );
                ImGui::BeginTooltip ( );
                ImGui::TextUnformatted ( full );
                ImGui::EndTooltip ( );
            };
            break;
        }
    };

    ImGui::PopStyleColor ( );

    /* Phase E.3: Min/Max/Changes tooltip na hover Value cell. Pro int typy
     * formátuje min/max podle row->fmt. Pro string/bytes jen change_count.
     *
     * ImGuiHoveredFlags_AllowWhenOverlappedByItem zajisti že tooltip funguje
     * i kdyz nad value cell je Selectable s SpanAllColumns (Phase D Name
     * row-wide Selectable).
     */
    const st_WATCH_CACHE_VIEW *c = watch_cache_get ( r->id );
    if ( c && c->valid &&
         ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenOverlappedByItem |
                                  ImGuiHoveredFlags_ForTooltip ) ) {
        ImGui::BeginTooltip ( );
        if ( c->min_max_valid ) {
            char mn[64], mx[64];
            uint64_t mn_raw = (uint64_t) c->min_int;
            uint64_t mx_raw = (uint64_t) c->max_int;
            watch_format_int ( r, mn_raw, mn, sizeof ( mn ) );
            watch_format_int ( r, mx_raw, mx, sizeof ( mx ) );
            ImGui::Text ( "%s %s", _( "Min:" ), mn );
            ImGui::Text ( "%s %s", _( "Max:" ), mx );
            ImGui::Text ( "%s %llu", _( "Changes:" ),
                           (unsigned long long) c->change_count );
        } else {
            ImGui::Text ( "%s %llu", _( "Changes:" ),
                           (unsigned long long) c->change_count );
            ImGui::TextDisabled ( "%s",
                _( "(Min/Max not tracked for string types)" ) );
        };
        ImGui::EndTooltip ( );
    };
}


/* ========================================================================= */
/*  Tabulka                                                                  */
/* ========================================================================= */


/**
 * @brief Render jednoho radku tabulky.
 *
 * Sloupce (Phase E): [sel] | [::] | Name | Addr/Expr | Type | Value | [x].
 *
 * @param i  Index radku v storage.
 */
static void watch_render_row ( size_t i )
{
    const st_WATCH_ROW *r = watch_get ( i );
    if ( !r ) return;
    if ( !watch_filter_match ( r ) ) return;

    int row_id = r->id;

    ImGui::PushID ( row_id );
    ImGui::TableNextRow ( );

    /* Sloupec 0 (Phase D): Selection checkbox. */
    ImGui::TableNextColumn ( );
    bool selected = g_wu.selected.count ( row_id ) > 0;
    if ( ImGui::Checkbox ( "###watch_sel", &selected ) ) {
        if ( selected ) g_wu.selected.insert ( row_id );
        else g_wu.selected.erase ( row_id );
    };

    /* Sloupec 1 (Phase E): drag handle "::" - reorder zdroj + cíl.
     *
     * Selectable šířky -FLT_MIN aby celá buňka byla hit area. DragDropSource
     * nese payload "WATCH_ROW_IDX" = int (= aktuální storage index dragged
     * řádku, NE stabilní ID; index je platný v rámci téhož frame). DropTarget
     * na téže Selectable přijímá payload a volá watch_move(src_idx, i).
     *
     * Pozn.: drag source uses index místo stable ID protože watch_move
     * pracuje s indexy a v jednom frame se nezmění storage layout (drag
     * gesture je single-frame perspective).
     */
    ImGui::TableNextColumn ( );
    ImGui::Selectable ( "::###watch_drag", false,
                         ImGuiSelectableFlags_None,
                         ImVec2 ( 0.0f, 0.0f ) );
    if ( ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
        int src_idx = (int) i;
        ImGui::SetDragDropPayload ( "WATCH_ROW_IDX", &src_idx,
                                      sizeof ( src_idx ) );
        const char *label = ( r->name && r->name[0] ) ? r->name : "(anon)";
        ImGui::Text ( "%s %s", "::", label );
        ImGui::EndDragDropSource ( );
    };
    if ( ImGui::BeginDragDropTarget ( ) ) {
        const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload ( "WATCH_ROW_IDX" );
        if ( payload && payload->DataSize == (int) sizeof ( int ) ) {
            int src_idx = *(const int *) payload->Data;
            int dst_idx = (int) i;
            if ( src_idx != dst_idx && src_idx >= 0 ) {
                watch_move ( src_idx, dst_idx );
            };
        };
        ImGui::EndDragDropTarget ( );
    };
    watch_item_tooltip ( _( "Drag to reorder this watch row." ) );

    /* Sloupec 2: Name (inline rename na double-click NEBO context menu). */
    ImGui::TableNextColumn ( );
    bool is_editing = ( g_wu.editing_id == row_id );
    if ( is_editing ) {
        ImGui::SetNextItemWidth ( -FLT_MIN );
        if ( g_wu.edit_focus_pending ) {
            ImGui::SetKeyboardFocusHere ( );
            g_wu.edit_focus_pending = false;
        };
        bool done = ImGui::InputText ( "###watch_edit_name", g_wu.edit_buf,
                                         sizeof ( g_wu.edit_buf ),
                                         ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_AutoSelectAll );
        /* Loss-of-focus = potvrdit. */
        if ( done || ImGui::IsItemDeactivatedAfterEdit ( ) ) {
            watch_rename ( (int) i, g_wu.edit_buf );
            g_wu.editing_id = -1;
        } else if ( ImGui::IsItemDeactivated ( ) ) {
            /* Esc/cancel = no save. */
            g_wu.editing_id = -1;
        };
    } else {
        const char *display = ( r->name && r->name[0] ) ? r->name : "(anon)";
        /* V1.C.3: inline owner badge před jméno (= varianta integrace
         * bez přidání samostatného sloupce, watch má dynamický počet
         * sloupců závislý na snapshot mode). Tooltip + barva přes shared
         * owner_badge_render(). */
        owner_badge_render ( r->cmd_origin );
        ImGui::SameLine ( );
        /* Selectable s AllowDoubleClick + SpanAllColumns pro spolehlivý
         * double-click handle a row-wide kontext menu. AllowOverlap = umožní
         * následujícím widgetům v dalších sloupcích (např. Delete x button)
         * konzumovat klik nezávisle. */
        ImGui::Selectable ( display, false,
                            ImGuiSelectableFlags_AllowDoubleClick |
                            ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowOverlap );
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            g_wu.editing_id = row_id;
            snprintf ( g_wu.edit_buf, sizeof ( g_wu.edit_buf ),
                       "%s", r->name ? r->name : "" );
            g_wu.edit_focus_pending = true;
        };
        watch_item_tooltip ( _( "Double-click to rename. "
                                "Right-click for context menu." ) );

        /* Phase D: row-wide context menu na pravý klik (Selectable má
         * SpanAllColumns = klik fungue z libovolné buňky). */
        if ( ImGui::BeginPopupContextItem ( "###watch_row_ctx" ) ) {
            if ( ImGui::MenuItem ( _L ( "Rename##watch_ctx_rename" ) ) ) {
                g_wu.pending_rename_id = row_id;
                ImGui::CloseCurrentPopup ( );
            };
            if ( ImGui::MenuItem ( _L ( "Delete##watch_ctx_del" ) ) ) {
                g_wu.pending_delete_id = row_id;
                ImGui::CloseCurrentPopup ( );
            };
            ImGui::Separator ( );
            if ( ImGui::MenuItem ( _L ( "Copy expression##watch_ctx_copy" ) ) ) {
                if ( r->mode == WATCH_MODE_ADDRESS ) {
                    char abuf[16];
                    snprintf ( abuf, sizeof ( abuf ), "0x%04X", r->addr );
                    ImGui::SetClipboardText ( abuf );
                } else if ( r->expr_text ) {
                    ImGui::SetClipboardText ( r->expr_text );
                };
                ImGui::CloseCurrentPopup ( );
            };
            watch_item_tooltip ( _( "Copy address (hex) or expression text "
                                    "to clipboard." ) );
            ImGui::EndPopup ( );
        };
    };

    /* Sloupec 3: Addr / Expr.
     *
     * Pro mode=ADDRESS = hex adresa.
     * Pro mode=EXPR_* = zkrácený text expr (max ~22 znaků, ellipsis +
     * tooltip s plným expr + mode short label).
     */
    ImGui::TableNextColumn ( );
    if ( r->mode == WATCH_MODE_ADDRESS ) {
        ImGui::Text ( "0x%04X", r->addr );
    } else {
        const char *full = r->expr_text ? r->expr_text : "";
        const size_t cutoff = 22;
        size_t flen = strlen ( full );
        if ( flen <= cutoff ) {
            ImGui::TextUnformatted ( full );
        } else {
            char trunc[ 32 ];
            /* Vezmi prvních (cutoff-1) znaků + ellipsis. */
            size_t take = cutoff - 1;
            if ( take >= sizeof ( trunc ) ) take = sizeof ( trunc ) - 2;
            memcpy ( trunc, full, take );
            trunc[ take ] = '~';
            trunc[ take + 1 ] = '\0';
            ImGui::TextUnformatted ( trunc );
        };
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::BeginTooltip ( );
            ImGui::Text ( "[%s]", watch_mode_short_label ( r->mode ) );
            ImGui::Separator ( );
            ImGui::TextUnformatted ( full );
            if ( r->mode == WATCH_MODE_EXPR_DEREF && !watch_get_expr_error ( r ) ) {
                ImGui::Separator ( );
                ImGui::Text ( "-> 0x%04X", (unsigned) watch_eval_expr_as_addr ( r ) );
            };
            ImGui::EndTooltip ( );
        };
    };

    /* Sloupec 4: Type label (vcetne meta - bit index, length). */
    ImGui::TableNextColumn ( );
    if ( r->type == WATCH_TYPE_BIT ) {
        ImGui::Text ( "bit.%u", (unsigned) r->bit_index );
    } else if ( watch_type_has_length ( r->type ) ) {
        ImGui::Text ( "%s[%u]", watch_type_label ( r->type ),
                       (unsigned) r->length );
    } else {
        ImGui::TextUnformatted ( watch_type_label ( r->type ) );
    };

    /* Sloupec 5: Value (live polling). Pravy klik = format popup. */
    ImGui::TableNextColumn ( );
    watch_render_value ( r );

    /* Context menu na cele bunce Value.
     *
     * Pro int typy obsahuje Display format submenu (dec/hex/bin/char).
     * Vsechny typy maji Phase E.3 "Reset min/max" + "Reset changes count".
     */
    if ( ImGui::BeginPopupContextItem ( "###watch_value_ctx" ) ) {
        if ( watch_type_has_format ( r->type ) ) {
            ImGui::TextDisabled ( "%s", _( "Display format" ) );
            ImGui::Separator ( );
            for ( size_t fi = 0;
                  fi < sizeof ( g_all_fmts ) / sizeof ( g_all_fmts[0] );
                  fi++ ) {
                en_WATCH_FMT f = g_all_fmts[ fi ];
                /* CHAR ma smysl jen pro u8/i8 (multibyte fallback na hex). */
                bool sel = ( f == r->fmt );
                if ( ImGui::MenuItem ( watch_fmt_label ( f ), NULL, sel ) ) {
                    watch_set_fmt ( (int) i, f );
                };
            };
            ImGui::Separator ( );
        };
        if ( ImGui::MenuItem ( _L ( "Reset min/max##watch_ctx_resetmm" ) ) ) {
            watch_cache_reset_one ( row_id );
            ImGui::CloseCurrentPopup ( );
        };
        watch_item_tooltip ( _( "Clear min/max tracking and changes count "
                                "for this row." ) );
        ImGui::EndPopup ( );
    };

    /* Phase E.2: Δ sloupec (jen pokud snapshot aktivni). */
    if ( watch_snapshot_active ( ) ) {
        ImGui::TableNextColumn ( );
        watch_render_delta ( r );
    };

    /* Sloupec Delete (index 6 nebo 7 podle snapshot aktivity). */
    ImGui::TableNextColumn ( );
    /* SetNextItemAllowOverlap = button konzumuje klik i když Name Selectable
     * v sloupci 2 má SpanAllColumns (= jeho hit rect přesahuje do dalších
     * sloupců). Bez tohoto kombo flag by klik na "x" propadl do Selectable. */
    ImGui::SetNextItemAllowOverlap ( );
    if ( ImGui::SmallButton ( "x##watch_del" ) ) {
        g_wu.pending_delete_id = row_id;
    };
    watch_item_tooltip ( _( "Delete this watch (no confirm)." ) );

    ImGui::PopID ( );
}


/* ========================================================================= */
/*  Sticky header (Phase D)                                                  */
/* ========================================================================= */


/**
 * @brief Render sticky header (= 2 řádky: filter+counts | selection+file ops).
 *
 * Layout:
 *   Řádek 1: [+ Add]  Filter:[__] [Clear]  (vis/tot)
 *   Řádek 2: Selected: N  [Del]  |  [Save] [Save As...] [Load From...]
 *                                   [Clear All...]   | status_msg
 *
 * "Selected" sekce se zobrazuje vždy (count 0 = button Del disabled).
 *
 * @param visible_count  Počet řádků po filtru.
 * @param total_count    Celkový počet ve storage.
 */
static void watch_render_sticky_header ( size_t visible_count, size_t total_count )
{
    /* Řádek 1: + Add + Filter + counts. */
    ImGui::AlignTextToFramePadding ( );
    if ( ImGui::Button ( _L ( "+ Add##watch_add_btn" ) ) ) {
        g_wu.addr_input[0] = '\0';
        g_wu.expr_input[0] = '\0';
        g_wu.name_input[0] = '\0';
        g_wu.add_error[0]  = '\0';
        g_wu.add_mode      = WATCH_MODE_ADDRESS;
        g_wu.add_type      = WATCH_TYPE_U8;
        g_wu.add_length    = 8;
        g_wu.add_bit_index = 0;
        ImGui::OpenPopup ( "###watch_add_popup" );
    };
    watch_item_tooltip ( _( "Add a new watch entry." ) );

    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Filter:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 200.0f );
    ImGui::InputTextWithHint ( "###watch_filter",
                                _( "name, expression, address, type" ),
                                g_wu.filter, sizeof ( g_wu.filter ) );
    watch_item_tooltip ( _( "Filter matches watch name, expression text, "
                            "hex address or type label "
                            "(case-insensitive substring)." ) );
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Clear##watch_clrflt" ) ) ) {
        g_wu.filter[0] = '\0';
    };
    watch_item_tooltip ( _( "Clear the filter text." ) );
    ImGui::SameLine ( );
    ImGui::Text ( "(%zu/%zu)", visible_count, total_count );
    watch_item_tooltip ( _( "Visible / total watch count." ) );

    /* Phase E.1: highlight changes toggle. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    bool hl = g_wu.highlight_changes;
    if ( ImGui::Checkbox ( _L ( "Highlight changes##watch_hl" ), &hl ) ) {
        g_wu.highlight_changes = hl;
        watch_cfg_set_highlight_changes ( hl );
    };
    watch_item_tooltip ( _( "Briefly color value cells when they change. "
                            "Green=up, Red=down, Yellow=string changed." ) );

    /* Řádek 2: Selected + Del + file ops + status. */
    size_t sel_count = g_wu.selected.size ( );
    ImGui::Text ( "%s %zu", _( "Selected:" ), sel_count );
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( sel_count == 0 );
    if ( ImGui::Button ( _L ( "Delete##watch_delsel" ) ) ) {
        if ( sel_count > 1 ) {
            g_wu.pending_bulk_delete = true;
        } else {
            /* N=1: bez confirmu. Sebrat first ID z setu. */
            int only_id = *g_wu.selected.begin ( );
            g_wu.pending_delete_id = only_id;
            g_wu.selected.clear ( );
        };
    };
    watch_item_tooltip ( _( "Delete selected watches "
                            "(confirm if more than one)." ) );
    ImGui::EndDisabled ( );

    /* Oddělovač + file ops (přesunuto z původního toolbaru 2). */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Save##watch_save" ) ) ) {
        if ( watch_save_to_filepath ( NULL ) ) {
            char *def = watch_default_filepath ( );
            snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                       "Saved to %s", def ? def : "(default)" );
            if ( def ) g_free ( def );
        } else {
            snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                       "%s", "Save failed (I/O error)" );
        };
    };
    watch_item_tooltip ( _( "Save to default per-arch .watch file." ) );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Save As...##watch_saveas" ) ) ) {
        IGFD::FileDialogConfig config;
        char *def = watch_default_filepath ( );
        config.path = def ? def : ".";
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles |
                       ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "WatchSaveDialog", _( "Save Watches As..." ),
            ".watch,.*", config );
        if ( def ) g_free ( def );
    };
    watch_item_tooltip ( _( "Save watches to a chosen file." ) );

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Load From...##watch_loadfrom" ) ) ) {
        IGFD::FileDialogConfig config;
        char *def = watch_default_filepath ( );
        config.path = def ? def : ".";
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_DontShowHiddenFiles;
        ImGuiFileDialog::Instance ( )->OpenDialog (
            "WatchLoadDialog", _( "Load Watches From..." ),
            ".watch,.*", config );
        if ( def ) g_free ( def );
    };
    watch_item_tooltip ( _( "Replace current watches with file content." ) );

    ImGui::SameLine ( );
    ImGui::BeginDisabled ( total_count == 0 );
    if ( ImGui::Button ( _L ( "Clear All...##watch_clrall" ) ) ) {
        g_wu.pending_clear_all = true;
    };
    watch_item_tooltip ( _( "Remove all watch entries (with confirmation)." ) );
    ImGui::EndDisabled ( );

    /* Phase E.2: Snapshot baseline tlačítka. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( total_count == 0 );
    if ( ImGui::Button ( _L ( "Snapshot##watch_snap" ) ) ) {
        /* Sebrat hodnoty vsech radku (i nevisible po filtru). */
        watch_snapshot_begin ( ImGui::GetFrameCount ( ) );
        for ( size_t i = 0; i < total_count; i++ ) {
            const st_WATCH_ROW *rr = watch_get ( i );
            if ( !rr ) continue;
            bool is_int = ( watch_type_has_format ( rr->type ) ||
                            rr->type == WATCH_TYPE_BIT );
            if ( is_int ) {
                uint64_t v = watch_read_int ( rr );
                watch_snapshot_put ( rr->id, rr->type, v, NULL, 0 );
            } else {
                uint8_t buf[ WATCH_LENGTH_MAX_BYTES ];
                size_t got = 0;
                watch_read_bytes ( rr, buf, sizeof ( buf ), &got );
                watch_snapshot_put ( rr->id, rr->type, 0, buf, got );
            };
        };
        /* Status_msg NEnastavujeme - status bar uz ukazuje
         * "Snapshot @ frame N (Δ M frames)" pri aktivnim snapshotu. */
        g_wu.status_msg[0] = '\0';
    };
    watch_item_tooltip ( _( "Capture current values as baseline. "
                            "Adds Delta column showing change since snapshot." ) );
    ImGui::EndDisabled ( );

    if ( watch_snapshot_active ( ) ) {
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Clear snapshot##watch_snapclr" ) ) ) {
            watch_snapshot_clear ( );
            /* Status_msg NEnastavujeme - status bar uz ukazuje "No snapshot",
             * pridanim "Snapshot cleared" by se vznikla redundance. */
            g_wu.status_msg[0] = '\0';
        };
        watch_item_tooltip ( _( "Clear snapshot baseline (Delta column hides)." ) );
    };
}


/**
 * @brief Render status bar na dolním okraji okna.
 *
 * Levá strana: snapshot info (frame + delta) pokud aktivní + poslední
 * status_msg z file/snapshot operací. Pravá strana: počet řádků (visible/
 * total při aktivním filtru, jen total jinak) a počet selected (pokud > 0).
 *
 * @param total_count Celkový počet řádků ve storage.
 */
static void watch_render_statusbar ( size_t total_count )
{
    /* Visible count pokud filter aktivní. */
    size_t visible = total_count;
    bool filter_on = ( g_wu.filter[0] != '\0' );
    if ( filter_on ) {
        visible = 0;
        for ( size_t i = 0; i < total_count; i++ ) {
            const st_WATCH_ROW *v = watch_get ( i );
            if ( v && watch_filter_match ( v ) ) visible++;
        };
    };

    ImGui::Separator ( );

    /* Levá strana: snapshot info + případně status_msg. */
    if ( watch_snapshot_active ( ) ) {
        int snap_frame = watch_snapshot_frame ( );
        int cur_frame = (int) ImGui::GetFrameCount ( );
        int delta = cur_frame - snap_frame;
        if ( delta < 0 ) delta = 0;
        ImGui::TextDisabled ( _( "Snapshot @ frame %d  (\xCE\x94 %d frames)" ),
                                snap_frame, delta );
    } else {
        ImGui::TextDisabled ( "%s", _( "No snapshot" ) );
    };

    if ( g_wu.status_msg[0] ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|  %s", g_wu.status_msg );
    };

    /* Pravá strana: counts. Spočítat šířku textu a posunout cursor. */
    char counts[128];
    if ( filter_on ) {
        snprintf ( counts, sizeof ( counts ),
                   _( "%zu / %zu rows" ), visible, total_count );
    } else {
        snprintf ( counts, sizeof ( counts ),
                   _( "%zu rows" ), total_count );
    };
    char counts_full[192];
    size_t sel_n = g_wu.selected.size ( );
    if ( sel_n > 0 ) {
        snprintf ( counts_full, sizeof ( counts_full ),
                   "%s  |  %zu selected", counts, sel_n );
    } else {
        snprintf ( counts_full, sizeof ( counts_full ), "%s", counts );
    };
    float text_w = ImGui::CalcTextSize ( counts_full ).x;
    float avail_w = ImGui::GetContentRegionAvail ( ).x;
    if ( text_w < avail_w ) {
        ImGui::SameLine ( avail_w - text_w + ImGui::GetStyle ( ).ItemSpacing.x );
    } else {
        ImGui::SameLine ( );
    };
    ImGui::TextDisabled ( "%s", counts_full );
}


/**
 * @brief Render hlavičky tabulky s tristate select-all v 0. sloupci.
 *
 * Custom hlavička - tristate widget pres DrawList (none/some/all visible
 * selected). Toggle nastaví/odznačí všechny aktuálně VISIBLE řádky.
 *
 * @param total_count Celkový počet ve storage.
 */
static void watch_render_table_header ( size_t total_count )
{
    /* Spočítat visible vs visible_selected. */
    size_t visible = 0;
    size_t visible_selected = 0;
    for ( size_t i = 0; i < total_count; i++ ) {
        const st_WATCH_ROW *v = watch_get ( i );
        if ( !v ) continue;
        if ( !watch_filter_match ( v ) ) continue;
        visible++;
        if ( g_wu.selected.count ( v->id ) > 0 ) visible_selected++;
    };

    bool all_selected  = ( visible > 0 && visible_selected == visible );
    bool none_selected = ( visible_selected == 0 );

    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );

    /* Sloupec 0: tristate select-all checkbox (custom widget). */
    ImGui::TableSetColumnIndex ( 0 );
    ImGui::PushID ( "watch_selall" );
    {
        float h = ImGui::GetFrameHeight ( );
        ImVec2 size ( h, h );
        ImVec2 pos = ImGui::GetCursorScreenPos ( );
        ImGui::InvisibleButton ( "###watch_selall_btn", size );
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
            /* Checkmark analogicky ImGui::RenderCheckMark (= visual shoda
             * s běžným Checkbox). */
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
                for ( size_t i = 0; i < total_count; i++ ) {
                    const st_WATCH_ROW *v = watch_get ( i );
                    if ( !v ) continue;
                    if ( !watch_filter_match ( v ) ) continue;
                    g_wu.selected.erase ( v->id );
                };
            } else {
                /* Some/none -> označit vše visible. */
                for ( size_t i = 0; i < total_count; i++ ) {
                    const st_WATCH_ROW *v = watch_get ( i );
                    if ( !v ) continue;
                    if ( !watch_filter_match ( v ) ) continue;
                    g_wu.selected.insert ( v->id );
                };
            };
        };
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_ForTooltip ) ) {
        if ( all_selected ) {
            ImGui::SetTooltip ( "%s", _( "Deselect all visible watches." ) );
        } else if ( none_selected ) {
            ImGui::SetTooltip ( "%s", _( "Select all visible watches." ) );
        } else {
            ImGui::SetTooltip ( "%s", _( "Some selected. Click to select all visible." ) );
        };
    };
    ImGui::PopID ( );

    /* Sloupec 1 (Phase E): prázdná hlavička pro drag handle.
     * Hidden ID "###hdr_*" = vizualne prazdny, ale unikatni ID (vice
     * TableHeader s prazdnym labelem v jednom radku = konflikt). */
    ImGui::TableSetColumnIndex ( 1 );
    ImGui::TableHeader ( "###hdr_drag" );

    /* Sloupce 2-5: standardní header (Name / Addr / Type / Value). */
    for ( int col = 2; col <= 5; col++ ) {
        ImGui::TableSetColumnIndex ( col );
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
    };

    /* Phase E.2: Δ sloupec hlavička (jen pokud snapshot aktivní). */
    bool snap_active = watch_snapshot_active ( );
    int del_col = 6;
    if ( snap_active ) {
        ImGui::TableSetColumnIndex ( 6 );
        const char *dn = ImGui::TableGetColumnName ( 6 );
        ImGui::TableHeader ( dn ? dn : _( "Delta" ) );
        del_col = 7;
    };

    /* Sloupec del_col: prázdná hlavička pro delete tlačítko. */
    ImGui::TableSetColumnIndex ( del_col );
    ImGui::TableHeader ( "###hdr_del" );
}


/* ========================================================================= */
/*  Render top-level                                                         */
/* ========================================================================= */


void watch_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) return;

    /* Phase E.1: lazy sync UI state s cfg defaults (jen jednou per program
     * lifetime, protože cfg může být loadnut až po static init g_wu). */
    static bool s_cfg_synced = false;
    if ( !s_cfg_synced ) {
        g_wu.highlight_changes = watch_cfg_get_highlight_changes ( );
        g_wu.highlight_fade_ms = watch_cfg_get_highlight_fade_ms ( );
        if ( g_wu.highlight_fade_ms < 1 ) g_wu.highlight_fade_ms = 500;
        s_cfg_synced = true;
    };

    /* Phase E: cache stale prune (= drop entries pro id které už nejsou
     * v storage; cleanup po watch_remove). */
    watch_ui_prune_stale_cache ( );

    /* Phase E.3: emu reset detection (polling). g_mzarch_main.reset_count se
     * inkrementuje při každém dokončeném resetu CPU (= UI vlákno detekuje
     * edge). Při edge reset všech cache entries (prev + min/max + change_count)
     * a snapshot baseline (Phase E.2 = pokud existuje).
     *
     * Threading: čteme atomický unsigned bez locku - race může způsobit
     * propás (delayed detekce o 1 frame), což je akceptováno (nemá vliv
     * na korektnost dat). */
    static unsigned s_last_reset_count = 0;
    static bool s_reset_count_initialized = false;
    if ( !s_reset_count_initialized ) {
        s_last_reset_count = g_mzarch_main.reset_count;
        s_reset_count_initialized = true;
    } else if ( g_mzarch_main.reset_count != s_last_reset_count ) {
        s_last_reset_count = g_mzarch_main.reset_count;
        watch_cache_reset_all ( );
    };

    ImGui::SetNextWindowSize ( ImVec2 ( 640, 400 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 480, 240 ),
                                            ImVec2 ( FLT_MAX, FLT_MAX ) );

    /* ###watch_panel = stabilni ID i pri zmene titulu (lokalizace). */
    if ( !ImGui::Begin ( _L ( "Watch###watch_panel" ), p_open,
                          ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    };

    /* Snapshot counts. */
    size_t total = watch_count ( );
    size_t visible = 0;
    for ( size_t i = 0; i < total; i++ ) {
        const st_WATCH_ROW *v = watch_get ( i );
        if ( v && watch_filter_match ( v ) ) visible++;
    };

    /* Sticky header (2 řádky). */
    watch_render_sticky_header ( visible, total );

    /* Add popup je context-bound k buttonu Add v sticky header. */
    watch_render_add_popup ( );

    ImGui::Separator ( );

    /* Tabulka v scrollable region. */
    if ( total == 0 ) {
        ImGui::TextDisabled ( "%s",
            _( "No watches defined. Use [+ Add] above to add one." ) );
    } else {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable;
        bool snap_active = watch_snapshot_active ( );
        int n_cols = snap_active ? 8 : 7;
        /* Outer size -statusbar_h = "vezmi zbylou výšku mínus prostor pro
         * status bar dole" (separator + jeden řádek textu). */
        float statusbar_h = ImGui::GetFrameHeightWithSpacing ( );
        ImVec2 table_outer ( 0.0f, -statusbar_h );
        if ( ImGui::BeginTable ( "###watch_tbl", n_cols, flags, table_outer ) ) {
            ImGui::TableSetupScrollFreeze ( 0, 1 );
            /* Hidden ID "###col_*" = vizualne prazdny label, ale unikatni
             * interni ID (vice prazdnych labelu by zpusobilo ImGui ID
             * conflict warning). */
            ImGui::TableSetupColumn ( "###col_sel",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 26.0f );
            /* Phase E.0: drag handle sloupec. */
            ImGui::TableSetupColumn ( "###col_drag",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 22.0f );
            ImGui::TableSetupColumn ( _( "Name" ),
                                       ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn ( _( "Addr" ),
                                       ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableSetupColumn ( _( "Type" ),
                                       ImGuiTableColumnFlags_WidthFixed, 80.0f );
            ImGui::TableSetupColumn ( _( "Value" ),
                                       ImGuiTableColumnFlags_WidthStretch, 1.4f );
            /* Phase E.2: Δ sloupec (jen pokud snapshot aktivni). */
            if ( snap_active ) {
                ImGui::TableSetupColumn ( _( "Delta" ),
                                           ImGuiTableColumnFlags_WidthFixed,
                                           120.0f );
            };
            ImGui::TableSetupColumn ( "###col_del",
                                       ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoResize, 28.0f );
            /* Custom hlavička s tristate v sloupci 0 (Phase D). */
            watch_render_table_header ( total );

            /* V1.E.6.A: pending focus z watch_window_focus_id() - před loop
             * vyčisti filter, předznač selection. Scroll provede v loop po
             * render row. */
            int focus_id_local = g_wu.pending_focus_id;
            g_wu.pending_focus_id = 0;
            if ( focus_id_local > 0 ) {
                g_wu.filter[0] = '\0';
                int idx = watch_find_index_by_id ( focus_id_local );
                if ( idx >= 0 ) {
                    g_wu.selected.clear ( );
                    g_wu.selected.insert ( focus_id_local );
                };
            };

            for ( size_t i = 0; i < total; i++ ) {
                watch_render_row ( i );

                /* V1.E.6.A: scroll na cílový focus řádek. */
                if ( focus_id_local > 0 ) {
                    const st_WATCH_ROW *rr = watch_get ( i );
                    if ( rr && rr->id == focus_id_local ) {
                        ImGui::SetScrollHereY ( 0.5f );
                        focus_id_local = 0;
                    };
                };
            };

            ImGui::EndTable ( );
        };
    };

    /* Phase D: aplikace pending_rename z context menu (po render row). */
    if ( g_wu.pending_rename_id >= 0 ) {
        int idx = watch_find_index_by_id ( g_wu.pending_rename_id );
        if ( idx >= 0 ) {
            const st_WATCH_ROW *r = watch_get ( (size_t) idx );
            g_wu.editing_id = g_wu.pending_rename_id;
            snprintf ( g_wu.edit_buf, sizeof ( g_wu.edit_buf ),
                       "%s", ( r && r->name ) ? r->name : "" );
            g_wu.edit_focus_pending = true;
        };
        g_wu.pending_rename_id = -1;
    };

    /* Resolve pending single-row delete (přes ID = stabilní napříč mutacemi). */
    if ( g_wu.pending_delete_id >= 0 ) {
        int idx = watch_find_index_by_id ( g_wu.pending_delete_id );
        if ( idx >= 0 ) {
            if ( g_wu.editing_id == g_wu.pending_delete_id ) {
                g_wu.editing_id = -1;
            };
            g_wu.selected.erase ( g_wu.pending_delete_id );
            watch_remove ( idx );
        };
        g_wu.pending_delete_id = -1;
    };

    /* Phase D: bulk delete confirm popup. */
    if ( g_wu.pending_bulk_delete ) {
        ImGui::OpenPopup ( "###watch_confirm_bulkdel" );
        g_wu.pending_bulk_delete = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Delete Selected Watches?###watch_confirm_bulkdel" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _( "Delete %zu selected watch(es)?" ),
                       g_wu.selected.size ( ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Delete##watch_bulkdel_ok" ),
                              ImVec2 ( 140, 0 ) ) ) {
            /* Snapshot - remove posune indexy. Iterujeme přes ID, najdeme
             * aktuální index. Pořadí mazání nezáleží - každý lookup je
             * čerstvý. */
            std::vector<int> to_del ( g_wu.selected.begin ( ),
                                       g_wu.selected.end ( ) );
            for ( int id : to_del ) {
                int idx = watch_find_index_by_id ( id );
                if ( idx >= 0 ) {
                    if ( g_wu.editing_id == id ) g_wu.editing_id = -1;
                    watch_remove ( idx );
                };
            };
            g_wu.selected.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##watch_bulkdel_cancel" ),
                              ImVec2 ( 100, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Phase C: Clear All confirm popup. */
    if ( g_wu.pending_clear_all ) {
        ImGui::OpenPopup ( _L ( "Clear All Watches?###watch_confirm_clrall" ) );
        g_wu.pending_clear_all = false;
    };
    if ( ImGui::BeginPopupModal ( _L ( "Clear All Watches?###watch_confirm_clrall" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::TextUnformatted ( _( "Remove all watch entries? This cannot be undone." ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Clear##watch_clrall_ok" ), ImVec2 ( 120, 0 ) ) ) {
            watch_clear_storage ( );
            g_wu.editing_id = -1;
            g_wu.selected.clear ( );
            snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                       "%s", "All watches cleared" );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##watch_clrall_cancel" ),
                              ImVec2 ( 120, 0 ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Phase C: Load From... -> REPLACE confirm. */
    if ( !g_wu.pending_load_path.empty ( ) ) {
        ImGui::OpenPopup ( _L ( "Replace Watches?###watch_confirm_load" ) );
    };
    if ( ImGui::BeginPopupModal ( _L ( "Replace Watches?###watch_confirm_load" ),
                                    NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::Text ( _( "Loading from:\n%s\n\nReplace all current watches?" ),
                       g_wu.pending_load_path.c_str ( ) );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "Replace##watch_load_ok" ), ImVec2 ( 120, 0 ) ) ) {
            if ( watch_load_from_filepath ( g_wu.pending_load_path.c_str ( ) ) ) {
                snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                           "Loaded from %s", g_wu.pending_load_path.c_str ( ) );
            } else {
                snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                           "%s", "Load failed (parse/I/O error)" );
            };
            g_wu.pending_load_path.clear ( );
            g_wu.editing_id = -1;
            g_wu.selected.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Cancel##watch_load_cancel" ),
                              ImVec2 ( 120, 0 ) ) ) {
            g_wu.pending_load_path.clear ( );
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };

    /* Status bar (dolní okraj okna). */
    watch_render_statusbar ( total );

    /* Phase C: file dialogy. */
    if ( ImGuiFileDialog::Instance ( )->Display ( "WatchSaveDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            if ( watch_save_to_filepath ( path.c_str ( ) ) ) {
                snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                           "Saved to %s", path.c_str ( ) );
            } else {
                snprintf ( g_wu.status_msg, sizeof ( g_wu.status_msg ),
                           "%s", "Save failed (I/O error)" );
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };
    if ( ImGuiFileDialog::Instance ( )->Display ( "WatchLoadDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            g_wu.pending_load_path =
                ImGuiFileDialog::Instance ( )->GetFilePathName ( );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    ImGui::End ( );
}


void watch_window_show_hide ( void )
{
    g_gui->showWatchWindow = !g_gui->showWatchWindow;
}


extern "C" void watch_window_focus_id ( int watch_id )
{
    /* V1.E.6.A: Activity dvojklik routing - otevři okno, vyčisti filter
     * (aby cílový řádek byl viditelný) a přednastav selection. Scroll na
     * konkrétní řádek udělá render-loop v dalším frame. Pokud watch_id
     * neodpovídá žádnému st_WATCH_ROW.id (= mezičasem smazán), spotřeba
     * je no-op (= žádný side effect kromě otevření okna).
     *
     * Watch.id = stabilní runtime counter (>= 1), takže 0 jako sentinel
     * pro "no pending" je bezpečné. */
    if ( watch_id <= 0 ) return;
    g_gui->showWatchWindow = true;
    g_wu.pending_focus_id = watch_id;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
