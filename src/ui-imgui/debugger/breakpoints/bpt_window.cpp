/*
 * bpt_window.cpp — Hlavní okno Breakpoints (ImGui)
 *
 * PŘEHLED
 * =======
 * Samostatné okno pro správu breakpointů. Obsahuje:
 * - Top menu (File, Settings)
 * - Tree view — hierarchický seznam skupin a eventů s checkboxy
 * - Akční lišta — hex input, tlačítko Add, tlačítko Save
 *
 * OTEVŘENÍ/ZAVŘENÍ
 * ================
 * - Alt+B z hlavního okna (global_shortcuts.cpp)
 * - Menu → Debugger → Breakpoints (menu_debugger.cpp)
 * - Ikona v iconbar debuggeru (dbg_iconbar.cpp)
 * - Zavření: ESC, Alt+B, křížek, File → Hide
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
#include <ctype.h>

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "debugger/breakpoints.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"  /* V1.7+ BP CRUD via dbgapi */
#include "mzarch/mzarch.h"     /* V1.7 fix: g_mzarch_main.cpu->pc pro highlight expiry detekci */
#include "emulator/emulator.h"  /* EMULATOR_TEST_PAUSED */

#include "bpt_window.h"
#include "bpt_state.h"
#include "bpt_edit_panel.h"
#include "ui-imgui/mcp_activity/owner_badge.h"  /* V1.C.3 owner badge */

/* V1.7 C.3.5: cfgmain persistence (active_tab + per-tab filters). */
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"


BptUIState g_bpt_ui;


/* ========================================================================= */
/*  Interní funkce                                                           */
/* ========================================================================= */


/*
 * bpt_state_init — inicializace stavu UI
 */
void bpt_state_init ( void ) {
    memset ( &g_bpt_ui, 0x00, sizeof ( g_bpt_ui ) );
    g_bpt_ui.selected_id = -1;
    g_bpt_ui.last_triggered_id = -1;
    g_bpt_ui.last_triggered_at_frame = 0;
    g_bpt_ui.scroll_to_last_triggered = false;
    /* V1.7 C.3.5: default active tab = "All" (= persisted unsigned 0). */
    g_bpt_ui.active_tab = 0;
    g_bpt_ui.restore_active_tab_once = false;
    g_bpt_ui.initialized = true;
}


/*
 * bpt_hex_input_filter — ImGui callback pro filtraci znaků v hex inputu.
 * Povoluje pouze: 0-9, A-F, a-f
 */
static int bpt_hex_input_filter ( ImGuiInputTextCallbackData *data ) {
    if ( data->EventChar < 256 && strchr ( "0123456789abcdefABCDEF", (char)data->EventChar ) )
        return 0; /* povolit */
    return 1; /* zakázat */
}


/*
 * bpt_parse_hex_input — parsuje hex string z inputu. Vrátí true pokud je validní.
 */
static bool bpt_parse_hex_input ( const char *text, uint16_t *out_addr ) {
    if ( !text || !text[0] ) return false;
    unsigned long val = strtoul ( text, NULL, 16 );
    if ( val > 0xFFFF ) return false;
    *out_addr = (uint16_t) val;
    return true;
}


/*
 * ImU32 z RGB uint32_t — převede 0xRRGGBB na ImGui barvu.
 */
static ImU32 bpt_rgb_to_imu32 ( uint32_t rgb ) {
    return IM_COL32 (
        ( rgb >> 16 ) & 0xFF,
        ( rgb >> 8 ) & 0xFF,
        rgb & 0xFF,
        255
    );
}


/* ========================================================================= */
/*  Kontextové menu                                                          */
/* ========================================================================= */


/*
 * bpt_is_name_empty — vrátí true pokud je jméno NULL, prázdné, nebo jen mezery.
 */
static bool bpt_is_name_empty ( const char *name ) {
    if ( !name ) return true;
    while ( *name ) {
        if ( *name != ' ' ) return false;
        name++;
    };
    return true;
}


/*
 * bpt_selected_has_parent — vrátí true pokud vybraná položka má rodiče (parent >= 0).
 */
static bool bpt_selected_has_parent ( void ) {
    if ( g_bpt_ui.selected_id < 0 ) return false;

    if ( g_bpt_ui.selected_type == BPT_ITEM_GROUP ) {
        st_BPTGROUP *grp = breakpoints_group_find_by_id ( g_bpt_ui.selected_id );
        return ( grp && grp->parent >= 0 );
    } else {
        st_BPT *bpt = breakpoints_find_by_id ( g_bpt_ui.selected_id );
        return ( bpt && bpt->parent >= 0 );
    };
}


static void bpt_render_context_menu ( void ) {
    if ( !ImGui::BeginPopup ( "##bpt_context" ) ) return;

    if ( ImGui::MenuItem ( _L ( "Expand All##bpt" ) ) ) {
        g_bpt_ui.request_expand_all = true;
    };

    if ( ImGui::MenuItem ( _L ( "Collapse All##bpt" ) ) ) {
        g_bpt_ui.request_collapse_all = true;
    };

    ImGui::Separator ( );

    if ( ImGui::MenuItem ( _L ( "Add Breakpoint Event...##bpt" ) ) ) {
        bpt_edit_panel_open ( BPT_ITEM_EVENT, -1 );
    };

    if ( ImGui::MenuItem ( _L ( "Add Group...##bpt" ) ) ) {
        bpt_edit_panel_open ( BPT_ITEM_GROUP, -1 );
    };

    ImGui::Separator ( );

    bool has_selection = ( g_bpt_ui.selected_id >= 0 );

    if ( ImGui::MenuItem ( _L ( "Edit row...##bpt" ), NULL, false, has_selection ) ) {
        bpt_edit_panel_open ( g_bpt_ui.selected_type, g_bpt_ui.selected_id );
    };

    /* Unparent — aktivní jen u položek s rodičem */
    bool can_unparent = has_selection && bpt_selected_has_parent ( );
    if ( ImGui::MenuItem ( _L ( "Unparent##bpt" ), NULL, false, can_unparent ) ) {
        if ( g_bpt_ui.selected_type == BPT_ITEM_GROUP ) {
            st_DBGAPI_BPGRP_UPDATE_PARAM p;
            memset ( &p, 0, sizeof ( p ) );
            p.id = g_bpt_ui.selected_id;
            p.update_mask = DBGAPI_BPGRP_UM_PARENT;
            p.parent = -1;
            dbg_ui_bpgrp_update ( &p );
        } else {
            dbg_ui_bp_set_parent ( g_bpt_ui.selected_id, -1 );
        };
    };

    ImGui::Separator ( );

    if ( ImGui::MenuItem ( _L ( "Delete Row/Branch##bpt" ), NULL, false, has_selection ) ) {
        if ( g_bpt_ui.selected_type == BPT_ITEM_GROUP ) {
            dbg_ui_bpgrp_remove ( g_bpt_ui.selected_id );
        } else {
            dbg_ui_bp_remove ( g_bpt_ui.selected_id );
        };
        g_bpt_ui.selected_id = -1;
    };

    bool has_data = ( breakpoints_group_count ( ) > 0 || breakpoints_count ( ) > 0 );

    if ( ImGui::MenuItem ( _L ( "Delete All##bpt" ), NULL, false, has_data ) ) {
        breakpoints_clear_all ( );
        g_bpt_ui.selected_id = -1;
    };

    ImGui::EndPopup ( );
}


/* ========================================================================= */
/*  Vykreslení jedné položky stromu                                          */
/* ========================================================================= */


/*
 * Vykreslí jeden event v tree view.
 * parent_disabled = true pokud je rodičovská skupina disabled (pro strikethrough).
 */
/**
 * @brief V1.7 helper - vykreslí celořádkový background pro BP řádek.
 *
 * Zahrnuje:
 *   - custom bg_rgb (bpt->bg_rgb, pokud != 0x000000)
 *   - last-triggered žlutý overlay (pokud bpt->id == last_triggered_id)
 *
 * Volá se PŘED render interaktivních prvků (Checkbox / Selectable / ...) -
 * obě varianty (inline tree i table-based) potřebují vykreslit overlay
 * jako podklad za widget content.
 *
 * @param bpt              BP záznam (musí mít bg_rgb a id).
 * @param consume_scroll   Pokud true a tento BP je last_triggered se
 *                         scroll_to_last_triggered flagem, voláme
 *                         SetScrollHereY a flag resetneme. Volat z toho
 *                         view kde má scroll smysl (= active tab).
 */
static void bpt_render_event_row_background ( const st_BPT *bpt, bool consume_scroll ) {
    bool is_last_triggered = ( g_bpt_ui.last_triggered_id == bpt->id );
    if ( is_last_triggered && consume_scroll && g_bpt_ui.scroll_to_last_triggered ) {
        ImGui::SetScrollHereY ( 0.5f );
        g_bpt_ui.scroll_to_last_triggered = false;
    };

    /* Custom user bg (pokud non-black). */
    if ( bpt->bg_rgb != 0x000000 ) {
        ImU32 bg = bpt_rgb_to_imu32 ( bpt->bg_rgb );
        ImVec2 cursor = ImGui::GetCursorScreenPos ( );
        ImVec2 avail = ImGui::GetContentRegionAvail ( );
        float line_h = ImGui::GetTextLineHeightWithSpacing ( );
        ImGui::GetWindowDrawList ( )->AddRectFilled (
            cursor,
            ImVec2 ( cursor.x + avail.x, cursor.y + line_h ),
            bg
        );
    };

    /* Last-triggered yellow overlay (alpha 0x50 = custom bg prosvítá). */
    if ( is_last_triggered ) {
        ImVec2 cursor = ImGui::GetCursorScreenPos ( );
        ImVec2 avail = ImGui::GetContentRegionAvail ( );
        float line_h = ImGui::GetTextLineHeightWithSpacing ( );
        ImGui::GetWindowDrawList ( )->AddRectFilled (
            cursor,
            ImVec2 ( cursor.x + avail.x, cursor.y + line_h ),
            IM_COL32 ( 255, 220, 0, 0x50 )
        );
    };
}


/**
 * @brief V1.7 helper - reakce na interakci nad BP widgetem (Selectable).
 *
 * Volá se HNED PO ImGui widget jehož IsItem* funkce konzumuje. Pokrývá:
 *   - levý/pravý klik = selekce do g_bpt_ui
 *   - dvojklik = otevření edit panelu
 *   - pravý klik = trigger pro context menu (= g_bpt_ui.open_context_menu)
 *   - drag-drop source pro reparenting
 *
 * Společné pro inline tree view ("All" tab) i budoucí table-based per-typ
 * tabs - z hlediska user UX je interakce stejná.
 *
 * @param bpt  BP záznam.
 */
static void bpt_event_interaction_handler ( const st_BPT *bpt ) {
    /* Selekce levým i pravým klikem. */
    if ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) || ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
        g_bpt_ui.selected_id = bpt->id;
        g_bpt_ui.selected_type = BPT_ITEM_EVENT;
    };

    /* Dvojklik = otevřít editační panel. */
    if ( ImGui::IsItemHovered ( ) && ImGui::IsMouseDoubleClicked ( 0 ) ) {
        bpt_edit_panel_open ( BPT_ITEM_EVENT, bpt->id );
    };

    /* Pravý klik = trigger context menu (odložené otevření). */
    if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
        g_bpt_ui.open_context_menu = true;
    };

    /* Drag source pro reparenting. */
    if ( ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
        BptTreeItem item = { BPT_ITEM_EVENT, bpt->id, bpt->parent };
        ImGui::SetDragDropPayload ( "BPT_ITEM", &item, sizeof ( item ) );
        ImGui::Text ( "%s", bpt->name ? bpt->name : "" );
        ImGui::EndDragDropSource ( );
    };
}


static void bpt_render_event ( st_BPT *bpt, bool parent_disabled ) {
    ImGui::PushID ( bpt->id );

    /* V1.7: background + last-triggered overlay + auto-scroll trigger
     * konsolidováno do helperu (= shared se table-based view v per-typ tabech). */
    bpt_render_event_row_background ( bpt, true /* consume scroll */ );

    /* Checkbox — enabled/disabled */
    bool enabled = bpt->enabled;
    if ( ImGui::Checkbox ( "##en", &enabled ) ) {
        dbg_ui_bp_set_enabled ( bpt->id, enabled );
    };

    ImGui::SameLine ( );

    /* V1.7: Type tag - krátký typ BP (PC_EXEC/MEM_R/...) zarovnaný do
     * pseudo-sloupce za checkboxem. Šířka = "SP_THRESHOLD" (nejdelší)
     * + padding aby všechny labely v listu začaly na stejné pozici. */
    ImU32 fg = bpt_rgb_to_imu32 ( bpt->fg_rgb );
    float type_w = ImGui::CalcTextSize ( "SP_THRESHOLD " ).x;
    float type_start_x = ImGui::GetCursorPosX ( );
    ImGui::TextColored ( ImVec4 ( 0.55f, 0.75f, 0.95f, 1.0f ), "%s",
                          bpt_type_to_string ( bpt->type ) );
    ImGui::SameLine ( type_start_x + type_w );

    /* Label — selectable. Span all columns + double-click flag, aby
     * interaction handler níže viděl správné IsItem* hits. */
    ImGui::PushStyleColor ( ImGuiCol_Text, fg );
    bool is_selected = ( g_bpt_ui.selected_id == bpt->id && g_bpt_ui.selected_type == BPT_ITEM_EVENT );
    char label[64];
    snprintf ( label, sizeof ( label ), "%s", bpt->name ? bpt->name : "" );
    ImGui::Selectable ( label, is_selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick );

    /* V1.7: interakční handler (selekce / dvojklik / context menu / drag). */
    bpt_event_interaction_handler ( bpt );

    /* Pravý sloupec — adresa (fixní pozice od pravého okraje okna). */
    float addr_width = ImGui::CalcTextSize ( "0xFFFF" ).x;
    float right_edge = ImGui::GetWindowWidth ( ) - ImGui::GetStyle ( ).WindowPadding.x;
    ImGui::SameLine ( right_edge - addr_width );
    ImGui::Text ( "0x%04X", bpt->addr );

    /* Strikethrough overlay pokud rodičovská skupina disabled. */
    if ( parent_disabled ) {
        ImVec2 p_min = ImGui::GetItemRectMin ( );
        ImVec2 p_max = ImGui::GetItemRectMax ( );
        float y_mid = ( p_min.y + p_max.y ) * 0.5f;
        ImGui::GetWindowDrawList ( )->AddLine (
            ImVec2 ( p_min.x, y_mid ),
            ImVec2 ( p_max.x, y_mid ),
            fg, 1.0f
        );
    };

    ImGui::PopStyleColor ( );
    ImGui::PopID ( );
}


/*
 * Porovnání eventů podle adresy pro qsort.
 */
static int bpt_compare_events_by_addr ( const void *a, const void *b ) {
    const st_BPT *ea = (const st_BPT *)a;
    const st_BPT *eb = (const st_BPT *)b;
    if ( ea->addr < eb->addr ) return -1;
    if ( ea->addr > eb->addr ) return 1;
    return 0;
}


/*
 * Porovnání skupin podle order, abecedy, ID.
 */
static int bpt_compare_groups ( const void *a, const void *b ) {
    const st_BPTGROUP *ga = (const st_BPTGROUP *)a;
    const st_BPTGROUP *gb = (const st_BPTGROUP *)b;

    /* Primárně podle order */
    if ( ga->order < gb->order ) return -1;
    if ( ga->order > gb->order ) return 1;

    /* Při shodě abecedně */
    int cmp = g_ascii_strcasecmp ( ga->name ? ga->name : "", gb->name ? gb->name : "" );
    if ( cmp != 0 ) return cmp;

    /* Při shodě podle ID */
    return ga->id - gb->id;
}


/*
 * Vykreslí eventy patřící danému rodiči (parent_id). Seřazeno podle adresy.
 */
static void bpt_render_events_for_parent ( int parent_id, bool parent_disabled ) {
    /* Sebrat eventy s daným parent */
    unsigned count = g_breakpoints.breakpoints->len;
    if ( count == 0 ) return;

    /* Dočasné pole pro seřazené eventy */
    st_BPT *sorted = (st_BPT *) g_alloca ( count * sizeof ( st_BPT ) );
    unsigned n = 0;

    for ( unsigned i = 0; i < count; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( bpt->parent == parent_id ) {
            sorted[n++] = *bpt;
        };
    };

    if ( n == 0 ) return;

    qsort ( sorted, n, sizeof ( st_BPT ), bpt_compare_events_by_addr );

    for ( unsigned i = 0; i < n; i++ ) {
        bpt_render_event ( &sorted[i], parent_disabled );
    };
}


/*
 * Vykreslí skupiny patřící danému rodiči (parent_id). Rekurzivní.
 */
static void bpt_render_groups_for_parent ( int parent_id, bool parent_disabled ) {
    unsigned count = g_breakpoints.groups->len;
    if ( count == 0 ) return;

    /* Dočasné pole pro seřazené skupiny */
    st_BPTGROUP *sorted = (st_BPTGROUP *) g_alloca ( count * sizeof ( st_BPTGROUP ) );
    unsigned n = 0;

    for ( unsigned i = 0; i < count; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        if ( grp->parent == parent_id ) {
            sorted[n++] = *grp;
        };
    };

    if ( n == 0 ) return;

    qsort ( sorted, n, sizeof ( st_BPTGROUP ), bpt_compare_groups );

    for ( unsigned i = 0; i < n; i++ ) {
        st_BPTGROUP *grp = &sorted[i];
        ImGui::PushID ( grp->id );

        /* Barvy */
        ImU32 fg = bpt_rgb_to_imu32 ( grp->fg_rgb );
        ImU32 bg = bpt_rgb_to_imu32 ( grp->bg_rgb );

        /* Pozadí */
        if ( grp->bg_rgb != 0x000000 ) {
            ImVec2 cursor = ImGui::GetCursorScreenPos ( );
            ImVec2 avail = ImGui::GetContentRegionAvail ( );
            float line_h = ImGui::GetTextLineHeightWithSpacing ( );
            ImGui::GetWindowDrawList ( )->AddRectFilled (
                cursor,
                ImVec2 ( cursor.x + avail.x, cursor.y + line_h ),
                bg
            );
        };

        /* Checkbox */
        bool enabled = grp->enabled;
        if ( ImGui::Checkbox ( "##en", &enabled ) ) {
            st_DBGAPI_BPGRP_UPDATE_PARAM p;
            memset ( &p, 0, sizeof ( p ) );
            p.id = grp->id;
            p.update_mask = DBGAPI_BPGRP_UM_ENABLED;
            p.enabled = enabled;
            dbg_ui_bpgrp_update ( &p );
        };

        ImGui::SameLine ( );

        /* Barva textu */
        ImGui::PushStyleColor ( ImGuiCol_Text, fg );

        /* TreeNode pro skupinu */
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        bool is_selected = ( g_bpt_ui.selected_id == grp->id && g_bpt_ui.selected_type == BPT_ITEM_GROUP );
        if ( is_selected ) flags |= ImGuiTreeNodeFlags_Selected;

        /* Expand/Collapse All — nastavit stav před vykreslením */
        if ( g_bpt_ui.request_expand_all ) {
            ImGui::SetNextItemOpen ( true );
        } else if ( g_bpt_ui.request_collapse_all ) {
            ImGui::SetNextItemOpen ( false );
        };

        bool is_open = ImGui::TreeNodeEx ( grp->name ? grp->name : "", flags );

        /* Selekce levým i pravým klikem */
        if ( ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) && !ImGui::IsItemToggledOpen ( ) ) ||
             ImGui::IsItemClicked ( ImGuiMouseButton_Right ) )
        {
            g_bpt_ui.selected_id = grp->id;
            g_bpt_ui.selected_type = BPT_ITEM_GROUP;
        };

        /* Dvojklik — editace přes nemodální panel */
        if ( ImGui::IsItemHovered ( ) && ImGui::IsMouseDoubleClicked ( 0 ) ) {
            bpt_edit_panel_open ( BPT_ITEM_GROUP, grp->id );
        };

        /* Kontextové menu na pravé tlačítko — odložené otevření */
        if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_bpt_ui.open_context_menu = true;
        };

        /* Strikethrough overlay pokud rodičovská skupina disabled */
        if ( parent_disabled ) {
            ImVec2 p_min = ImGui::GetItemRectMin ( );
            ImVec2 p_max = ImGui::GetItemRectMax ( );
            float y_mid = ( p_min.y + p_max.y ) * 0.5f;
            ImGui::GetWindowDrawList ( )->AddLine (
                ImVec2 ( p_min.x, y_mid ),
                ImVec2 ( p_max.x, y_mid ),
                fg, 1.0f
            );
        };

        /* Drag source */
        if ( ImGui::BeginDragDropSource ( ImGuiDragDropFlags_None ) ) {
            BptTreeItem item = { BPT_ITEM_GROUP, grp->id, grp->parent };
            ImGui::SetDragDropPayload ( "BPT_ITEM", &item, sizeof ( item ) );
            ImGui::Text ( "%s", grp->name ? grp->name : "" );
            ImGui::EndDragDropSource ( );
        };

        /* Drop target — reparenting */
        if ( ImGui::BeginDragDropTarget ( ) ) {
            if ( const ImGuiPayload *payload = ImGui::AcceptDragDropPayload ( "BPT_ITEM" ) ) {
                BptTreeItem *dropped = (BptTreeItem *)payload->Data;
                if ( dropped->type == BPT_ITEM_GROUP ) {
                    st_DBGAPI_BPGRP_UPDATE_PARAM p;
                    memset ( &p, 0, sizeof ( p ) );
                    p.id = dropped->id;
                    p.update_mask = DBGAPI_BPGRP_UM_PARENT;
                    p.parent = grp->id;
                    dbg_ui_bpgrp_update ( &p );
                } else {
                    dbg_ui_bp_set_parent ( dropped->id, grp->id );
                };
            };
            ImGui::EndDragDropTarget ( );
        };

        ImGui::PopStyleColor ( );

        if ( is_open ) {
            /* Potomci — zda je tato skupina disabled? */
            bool child_disabled = parent_disabled || !grp->enabled;

            if ( g_breakpoints.groups_first ) {
                bpt_render_groups_for_parent ( grp->id, child_disabled );
                bpt_render_events_for_parent ( grp->id, child_disabled );
            } else {
                bpt_render_events_for_parent ( grp->id, child_disabled );
                bpt_render_groups_for_parent ( grp->id, child_disabled );
            };

            ImGui::TreePop ( );
        };

        ImGui::PopID ( );
    };
}


/* ========================================================================= */
/*  Per-typ tab view (V1.7 C.3.3)                                            */
/* ========================================================================= */


/**
 * @brief Identifikátor tabu (= sada BP typů které tab zobrazuje).
 *
 * Použito pro filter logic v bpt_render_filtered_table(). Hodnoty nesmí
 * záviset na en_BPT_TYPE konkrétně - každý tab může zahrnovat 1 nebo
 * více typů, a pořadí typů v enum se nemusí krýt s pořadím tabů.
 */
typedef enum en_BPT_TAB_FILTER {
    BPT_TAB_EXEC = 0,    /**< BPT_TYPE_PC_EXEC */
    BPT_TAB_MEM,         /**< BPT_TYPE_MEM_R + MEM_W */
    BPT_TAB_IO,          /**< BPT_TYPE_IORQ_R + IORQ_W */
    BPT_TAB_IRQ,         /**< BPT_TYPE_IRQ + IRQ_SIG */
    BPT_TAB_EVENTS,      /**< BPT_TYPE_HW_EVENT */
    BPT_TAB_MISC,        /**< BPT_TYPE_SP_THRESHOLD + GLOBAL */
} en_BPT_TAB_FILTER;


/**
 * @brief Vrátí true pokud BP daného typu patří do daného tabu.
 */
static bool bpt_passes_tab_filter ( en_BPT_TYPE bp_type, en_BPT_TAB_FILTER tab ) {
    switch ( tab ) {
    case BPT_TAB_EXEC:   return bp_type == BPT_TYPE_PC_EXEC;
    case BPT_TAB_MEM:    return bp_type == BPT_TYPE_MEM_R   || bp_type == BPT_TYPE_MEM_W;
    case BPT_TAB_IO:     return bp_type == BPT_TYPE_IORQ_R  || bp_type == BPT_TYPE_IORQ_W;
    case BPT_TAB_IRQ:    return bp_type == BPT_TYPE_IRQ     || bp_type == BPT_TYPE_IRQ_SIG;
    case BPT_TAB_EVENTS: return bp_type == BPT_TYPE_HW_EVENT;
    case BPT_TAB_MISC:   return bp_type == BPT_TYPE_SP_THRESHOLD || bp_type == BPT_TYPE_GLOBAL;
    }
    return false;
}


/**
 * @brief Zformátuje sloupec "Target" pro daný BP do bufferu.
 *
 * Target = co je primární filter - addr/range pro EXEC+MEM, port pro IO,
 * vector/ISR/source pro IRQ, event_name pro HW_EVENT, threshold pro
 * SP_THRESHOLD, condition pro GLOBAL.
 */
static void bpt_format_target_text ( const st_BPT *bp, char *out, size_t out_sz ) {
    if ( !out || out_sz == 0 ) return;
    out[0] = '\0';

    switch ( bp->type ) {
    case BPT_TYPE_PC_EXEC:
    case BPT_TYPE_MEM_R:
    case BPT_TYPE_MEM_W:
        switch ( bp->addr_match_mode ) {
        case BP_MATCH_SINGLE:
            snprintf ( out, out_sz, "0x%04X", bp->addr );
            break;
        case BP_MATCH_RANGE:
            snprintf ( out, out_sz, "0x%04X..0x%04X", bp->addr, bp->addr_end );
            break;
        case BP_MATCH_MASK:
            snprintf ( out, out_sz, "0x%04X & 0x%04X", bp->addr, bp->addr_mask );
            break;
        default:
            snprintf ( out, out_sz, "0x%04X", bp->addr );
            break;
        };
        break;

    case BPT_TYPE_IORQ_R:
    case BPT_TYPE_IORQ_W:
        switch ( bp->port_match_mode ) {
        case BP_MATCH_SINGLE:
            snprintf ( out, out_sz, "port 0x%04X", bp->port );
            break;
        case BP_MATCH_RANGE:
            snprintf ( out, out_sz, "port 0x%04X..0x%04X", bp->port, bp->port_end );
            break;
        case BP_MATCH_MASK:
            snprintf ( out, out_sz, "port 0x%04X & 0x%04X", bp->port, bp->port_mask );
            break;
        default:
            snprintf ( out, out_sz, "port 0x%04X", bp->port );
            break;
        };
        break;

    case BPT_TYPE_IRQ:
        snprintf ( out, out_sz, "IM%s%s%s%s",
                   bp->im0_enabled ? "0" : "",
                   bp->im1_enabled ? "1" : "",
                   bp->im2_enabled ? "2" : "",
                   ( bp->im0_enabled || bp->im1_enabled || bp->im2_enabled ) ? "" : "(none)" );
        break;

    case BPT_TYPE_IRQ_SIG:
        snprintf ( out, out_sz, "mask 0x%02X", (unsigned) bp->irq_sig_source_mask );
        break;

    case BPT_TYPE_HW_EVENT:
        snprintf ( out, out_sz, "%s", bp->event_name ? bp->event_name : "(unnamed)" );
        break;

    case BPT_TYPE_SP_THRESHOLD:
        if ( bp->sp_mode == BP_SP_WINDOW ) {
            snprintf ( out, out_sz, "SP 0x%04X..0x%04X", bp->sp_threshold, bp->sp_upper );
        } else {
            snprintf ( out, out_sz, "SP < 0x%04X", bp->sp_threshold );
        };
        break;

    case BPT_TYPE_GLOBAL:
        snprintf ( out, out_sz, "%s", _( "(condition-based)" ) );
        break;

    default:
        snprintf ( out, out_sz, "0x%04X", bp->addr );
        break;
    };
}


/**
 * @brief V1.7 C.3.4 - case-insensitive substring match.
 *
 * needle == "" → match always. Vyhne se chybě snprintf null check chaos.
 */
static bool bpt_substring_match_ci ( const char *haystack, const char *needle ) {
    if ( !needle || needle[0] == '\0' ) return true;
    if ( !haystack ) return false;
    /* Case-insensitive substring: enumeruj všechny posuny haystacku,
     * porovnej tolower. Pro UI filtry rozumný O(N*M) bez zbytečných
     * alokací. */
    size_t hlen = strlen ( haystack );
    size_t nlen = strlen ( needle );
    if ( nlen > hlen ) return false;
    for ( size_t i = 0; i + nlen <= hlen; i++ ) {
        size_t k;
        for ( k = 0; k < nlen; k++ ) {
            unsigned char hc = (unsigned char) haystack[i + k];
            unsigned char nc = (unsigned char) needle[k];
            if ( g_ascii_tolower ( hc ) != g_ascii_tolower ( nc ) ) break;
        };
        if ( k == nlen ) return true;
    };
    return false;
}


/**
 * @brief V1.7 C.3.4 - test BP proti per-tab filter state.
 *
 * Vrátí true pokud BP **passes** filter (= má být zobrazen).
 *
 * Filter složení:
 *   - tab_filter_enabled_only[tab]: true → jen `bpt->enabled` BP
 *   - tab_filter_text[tab]: non-empty substring match (case-insensitive)
 *     proti label + condition + target text. Prázdný = match always.
 */
static bool bpt_passes_user_filter ( const st_BPT *bpt, en_BPT_TAB_FILTER tab ) {
    int idx = (int) tab;
    if ( idx < 0 || idx >= BPT_TAB_FILTER_COUNT ) return true;

    if ( g_bpt_ui.tab_filter_enabled_only[idx] && !bpt->enabled ) return false;

    const char *needle = g_bpt_ui.tab_filter_text[idx];
    if ( needle[0] == '\0' ) return true;

    /* Match v label, condition, target text. */
    if ( bpt_substring_match_ci ( bpt->name, needle ) ) return true;
    if ( bpt_substring_match_ci ( bpt->expr, needle ) ) return true;
    char target[64];
    bpt_format_target_text ( bpt, target, sizeof ( target ) );
    if ( bpt_substring_match_ci ( target, needle ) ) return true;

    return false;
}


/**
 * @brief V1.7 C.3.4 - vykreslí filter bar pro per-typ tab.
 *
 * Layout (jeden řádek):
 *   [text input ...]  [✓] Enabled only   N of M
 *
 * @param tab     Identifikátor tabu (= index do g_bpt_ui.tab_filter_*).
 * @param shown   Po render filtered table vypsaný count BP.
 * @param total   Total BP odpovídajících tab type filtru (bez user filtru).
 */
static void bpt_render_filter_bar ( en_BPT_TAB_FILTER tab, int shown, int total ) {
    int idx = (int) tab;
    if ( idx < 0 || idx >= BPT_TAB_FILTER_COUNT ) return;

    /* Text input - šířka cca 60 % viditelné šíře, zbytek pro checkbox + count. */
    float total_w = ImGui::GetContentRegionAvail ( ).x;
    float input_w = total_w * 0.55f;
    if ( input_w < 120.0f ) input_w = 120.0f;
    ImGui::SetNextItemWidth ( input_w );
    /* Pozn.: hint je pasivni text, ne ImGui widget label - pouzit _()
     * (= jen preklad). _L() pridava ##ID suffix ktery by se v hint
     * zobrazil jako literal "##bpt_filter_hint". Label inputu drzi
     * stable ID samostatne ("##bpt_filter_text"). */
    ImGui::InputTextWithHint ( "##bpt_filter_text",
                                _( "Filter (label, cond, target)" ),
                                g_bpt_ui.tab_filter_text[idx],
                                BPT_TAB_FILTER_TEXT_LEN );

    ImGui::SameLine ( );
    if ( ImGui::SmallButton ( _L( "X##bpt_filter_clear" ) ) ) {
        g_bpt_ui.tab_filter_text[idx][0] = '\0';
    };

    ImGui::SameLine ( );
    ImGui::Checkbox ( _L( "Enabled only##bpt_filter_enbo" ),
                       &g_bpt_ui.tab_filter_enabled_only[idx] );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "%d / %d", shown, total );
}


/**
 * @brief Vykreslí filtered list BP v table-based layoutu.
 *
 * Sloupce (uniformní napříč všemi taby V1.7 - per-typ specialization
 * = V1.8+):
 *   [✓]  enabled checkbox
 *   #ID  BP identifikátor
 *   Type type-specific zkratka (= bpt_type_to_string)
 *   Label user label (auto_name fallback)
 *   Target addr/range/port/event/... (per typ)
 *   Cond  condition expression (truncated na ~24 znaků)
 *   Hits  hit counter
 *
 * Interakce: stejný handler jako tree view (selekce, dvojklik, drag,
 * pravoklik = context menu). Last-triggered overlay přes shared helper.
 *
 * @param filter  Sada BP typů zobrazená v tomto tabu.
 */
/*
 * Kontextové menu pro per-typ taby (plochá tabulka, ne strom). Stejné jako
 * tree menu v "All", ale bez Expand/Collapse/Add Group (ploché taby strom
 * nemají) a "Delete Row/Branch" zkráceno na "Delete Row" (v ploché tabulce
 * není větev). Unparent ponecháno (BP může být ve skupině). Selekce se
 * nastaví klikem/pravým klikem na řádek v bpt_render_filtered_table.
 */
static void bpt_render_filtered_context_menu ( void ) {
    if ( !ImGui::BeginPopup ( "##bpt_filtered_context" ) ) return;

    if ( ImGui::MenuItem ( _L ( "Add Breakpoint Event...##bptf" ) ) ) {
        bpt_edit_panel_open ( BPT_ITEM_EVENT, -1 );
    };

    ImGui::Separator ( );

    bool has_selection = ( g_bpt_ui.selected_id >= 0 &&
                           g_bpt_ui.selected_type == BPT_ITEM_EVENT );

    if ( ImGui::MenuItem ( _L ( "Edit row...##bptf" ), NULL, false, has_selection ) ) {
        bpt_edit_panel_open ( BPT_ITEM_EVENT, g_bpt_ui.selected_id );
    };

    bool can_unparent = has_selection && bpt_selected_has_parent ( );
    if ( ImGui::MenuItem ( _L ( "Unparent##bptf" ), NULL, false, can_unparent ) ) {
        dbg_ui_bp_set_parent ( g_bpt_ui.selected_id, -1 );
    };

    ImGui::Separator ( );

    if ( ImGui::MenuItem ( _L ( "Delete Row##bptf" ), NULL, false, has_selection ) ) {
        dbg_ui_bp_remove ( g_bpt_ui.selected_id );
        g_bpt_ui.selected_id = -1;
    };

    bool has_data = ( breakpoints_group_count ( ) > 0 || breakpoints_count ( ) > 0 );
    if ( ImGui::MenuItem ( _L ( "Delete All##bptf" ), NULL, false, has_data ) ) {
        breakpoints_clear_all ( );
        g_bpt_ui.selected_id = -1;
    };

    ImGui::EndPopup ( );
}


static void bpt_render_filtered_table ( en_BPT_TAB_FILTER filter ) {
    unsigned count = g_breakpoints.breakpoints->len;

    /* Vstupní guard - žádné BP = info text. */
    if ( count == 0 ) {
        ImGui::TextDisabled ( "%s", _( "No breakpoints defined." ) );
        return;
    };

    /* Spočítáme total (= passes tab filter, bez user filtru) a shown
     * (= passes oba). Pro filter bar header X / Y. */
    int total_in_tab = 0;
    int shown_in_tab = 0;
    for ( unsigned i = 0; i < count; i++ ) {
        st_BPT *bp = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( !bpt_passes_tab_filter ( bp->type, filter ) ) continue;
        total_in_tab++;
        if ( bpt_passes_user_filter ( bp, filter ) ) shown_in_tab++;
    };

    /* Filter bar nad table - text input + enabled-only + count. */
    bpt_render_filter_bar ( filter, shown_in_tab, total_in_tab );

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

    if ( !ImGui::BeginTable ( "##bpt_table", 9, tflags ) ) {
        return;
    };

    ImGui::TableSetupScrollFreeze ( 0, 1 );
    ImGui::TableSetupColumn ( "##en",  ImGuiTableColumnFlags_WidthFixed, 24.0f );
    ImGui::TableSetupColumn ( _L( "#ID##bpt_col_id" ),     ImGuiTableColumnFlags_WidthFixed, 48.0f );
    /* V1.C.3 - Owner badge sloupec mezi ID a Type. */
    ImGui::TableSetupColumn ( _L( "Own##bpt_col_owner" ),  ImGuiTableColumnFlags_WidthFixed, 36.0f );
    ImGui::TableSetupColumn ( _L( "Type##bpt_col_type" ),   ImGuiTableColumnFlags_WidthFixed, 88.0f );
    /* Group sloupec - jméno skupiny (parent) daného BP, "-" pro root. */
    ImGui::TableSetupColumn ( _L( "Group##bpt_col_grp" ),   ImGuiTableColumnFlags_WidthFixed, 110.0f );
    ImGui::TableSetupColumn ( _L( "Label##bpt_col_label" ), ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn ( _L( "Target##bpt_col_tgt" ),  ImGuiTableColumnFlags_WidthFixed, 180.0f );
    ImGui::TableSetupColumn ( _L( "Cond##bpt_col_cond" ),   ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn ( _L( "Hits##bpt_col_hits" ),   ImGuiTableColumnFlags_WidthFixed, 60.0f );
    ImGui::TableHeadersRow ( );

    int rendered = 0;
    for ( unsigned i = 0; i < count; i++ ) {
        st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, i );
        if ( !bpt_passes_tab_filter ( bpt->type, filter ) ) continue;
        if ( !bpt_passes_user_filter ( bpt, filter ) ) continue;

        ImGui::PushID ( bpt->id );
        ImGui::TableNextRow ( );

        /* Background overlay (custom bg + last-triggered yellow + scroll). */
        ImGui::TableNextColumn ( );
        bpt_render_event_row_background ( bpt, true /* consume scroll */ );

        /* Sloupec 0: enabled checkbox. */
        bool enabled = bpt->enabled;
        if ( ImGui::Checkbox ( "##en", &enabled ) ) {
            dbg_ui_bp_set_enabled ( bpt->id, enabled );
        };

        /* Selectable přes celý řádek (SpanAllColumns) je rendered samostatně
         * v jednom z následujících columns - musí být před TableNextColumn
         * dalších sloupců, aby IsItem* odkazovaly na něj. Pro UX kliknutí
         * "na řádek mimo checkbox" = label sloupec to dělá nejlépe. */

        /* Sloupec 1: ID. */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "#%d", bpt->id );

        /* Sloupec 2 (V1.C.3): Owner badge. */
        ImGui::TableNextColumn ( );
        owner_badge_render ( bpt->cmd_origin );

        /* Sloupec 3: Type. */
        ImGui::TableNextColumn ( );
        ImU32 fg = bpt_rgb_to_imu32 ( bpt->fg_rgb );
        ImGui::PushStyleColor ( ImGuiCol_Text, fg );
        ImGui::TextUnformatted ( bpt_type_to_string ( bpt->type ) );
        ImGui::PopStyleColor ( );

        /* Sloupec Group: jméno parent skupiny ("-" pro root, "?" pokud
         * skupina nedohledána). */
        ImGui::TableNextColumn ( );
        if ( bpt->parent >= 0 ) {
            st_BPTGROUP *grp = breakpoints_group_find_by_id ( bpt->parent );
            if ( grp && grp->name && grp->name[0] ) {
                ImGui::TextUnformatted ( grp->name );
            } else {
                ImGui::TextDisabled ( "?" );
            };
        } else {
            ImGui::TextDisabled ( "-" );
        };

        /* Sloupec 3: Label (= Selectable kvůli interakci nad celým řádkem). */
        ImGui::TableNextColumn ( );
        bool is_selected = ( g_bpt_ui.selected_id == bpt->id && g_bpt_ui.selected_type == BPT_ITEM_EVENT );
        const char *lbl = ( bpt->name && bpt->name[0] ) ? bpt->name : "(unnamed)";
        ImGui::Selectable ( lbl, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick );
        bpt_event_interaction_handler ( bpt );
        /* Right-click na řádek = selekce + odložené otevření kontextového
         * menu per-typ tabu (zpracováno po EndTable). */
        if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_bpt_ui.selected_id = bpt->id;
            g_bpt_ui.selected_type = BPT_ITEM_EVENT;
            g_bpt_ui.open_filtered_context = true;
        };

        /* Sloupec 4: Target. */
        ImGui::TableNextColumn ( );
        char target[64];
        bpt_format_target_text ( bpt, target, sizeof ( target ) );
        ImGui::TextUnformatted ( target );

        /* Sloupec 5: Cond (truncated). */
        ImGui::TableNextColumn ( );
        if ( bpt->expr && bpt->expr[0] ) {
            char cond_buf[40];
            snprintf ( cond_buf, sizeof ( cond_buf ), "%s", bpt->expr );
            ImGui::TextUnformatted ( cond_buf );
            if ( ImGui::IsItemHovered ( ) && strlen ( bpt->expr ) > sizeof ( cond_buf ) - 1 ) {
                ImGui::SetTooltip ( "%s", bpt->expr );
            };
        } else {
            ImGui::TextDisabled ( "-" );
        };

        /* Sloupec 6: Hits. */
        ImGui::TableNextColumn ( );
        if ( bpt->hits > 0 ) {
            ImGui::Text ( "%llu", (unsigned long long) bpt->hits );
        } else {
            ImGui::TextDisabled ( "0" );
        };

        ImGui::PopID ( );
        rendered++;
    };

    ImGui::EndTable ( );

    /* Right-click na plochu tabulky (mimo řádek) také otevře menu - umožní
     * "Add Breakpoint Event..." i bez selekce. IsItemHovered po EndTable se
     * vztahuje na tabulku jako celek. */
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenBlockedByPopup ) &&
         ImGui::IsMouseReleased ( ImGuiMouseButton_Right ) ) {
        g_bpt_ui.open_filtered_context = true;
    };

    /* Odložené otevření kontextového menu (nastaveno pravým klikem na řádek
     * nebo na plochu tabulky výše) - OpenPopup mimo PushID(bpt->id) scope. */
    if ( g_bpt_ui.open_filtered_context ) {
        ImGui::OpenPopup ( "##bpt_filtered_context" );
        g_bpt_ui.open_filtered_context = false;
    };
    bpt_render_filtered_context_menu ( );

    if ( rendered == 0 ) {
        if ( total_in_tab == 0 ) {
            ImGui::TextDisabled ( "%s", _( "No breakpoints of this type." ) );
        } else {
            ImGui::TextDisabled ( "%s", _( "No breakpoints match the filter." ) );
        };
    };
}


/*
 * Kontextové menu pro záložku Groups (správa skupin). Add Group, Edit row,
 * Delete Branch (= skupina + podskupiny + jejich BP), Delete All. Bez
 * Add Breakpoint Event (sem patří jen operace nad skupinami).
 */
static void bpt_render_groups_context_menu ( void ) {
    if ( !ImGui::BeginPopup ( "##bpt_groups_context" ) ) return;

    if ( ImGui::MenuItem ( _L ( "Add Group...##bptg" ) ) ) {
        bpt_edit_panel_open ( BPT_ITEM_GROUP, -1 );
    };

    ImGui::Separator ( );

    bool has_selection = ( g_bpt_ui.selected_id >= 0 &&
                           g_bpt_ui.selected_type == BPT_ITEM_GROUP );

    if ( ImGui::MenuItem ( _L ( "Edit row...##bptg" ), NULL, false, has_selection ) ) {
        bpt_edit_panel_open ( BPT_ITEM_GROUP, g_bpt_ui.selected_id );
    };

    ImGui::Separator ( );

    if ( ImGui::MenuItem ( _L ( "Delete Branch##bptg" ), NULL, false, has_selection ) ) {
        dbg_ui_bpgrp_remove ( g_bpt_ui.selected_id );
        g_bpt_ui.selected_id = -1;
    };

    bool has_data = ( breakpoints_group_count ( ) > 0 || breakpoints_count ( ) > 0 );
    if ( ImGui::MenuItem ( _L ( "Delete All##bptg" ), NULL, false, has_data ) ) {
        breakpoints_clear_all ( );
        g_bpt_ui.selected_id = -1;
    };

    ImGui::EndPopup ( );
}


/*
 * Záložka Groups - plochá tabulka všech skupin. Sloupce: enabled, #ID, Name,
 * Parent (jméno rodičovské skupiny), #BP (počet BP přímo v dané skupině).
 * Pravý klik = kontextové menu pro správu skupin.
 */
static void bpt_render_groups_table ( void ) {
    unsigned gcount = g_breakpoints.groups->len;

    if ( gcount == 0 ) {
        ImGui::TextDisabled ( "%s", _( "No groups defined." ) );
        /* I bez skupin nabídni Add Group přes pravý klik do plochy okna. */
        if ( ImGui::IsWindowHovered ( ) &&
             ImGui::IsMouseReleased ( ImGuiMouseButton_Right ) ) {
            g_bpt_ui.open_groups_context = true;
        };
        if ( g_bpt_ui.open_groups_context ) {
            ImGui::OpenPopup ( "##bpt_groups_context" );
            g_bpt_ui.open_groups_context = false;
        };
        bpt_render_groups_context_menu ( );
        return;
    };

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

    if ( !ImGui::BeginTable ( "##bpt_groups_table", 5, tflags ) ) {
        return;
    };

    ImGui::TableSetupScrollFreeze ( 0, 1 );
    ImGui::TableSetupColumn ( "##gen",  ImGuiTableColumnFlags_WidthFixed, 24.0f );
    ImGui::TableSetupColumn ( _L( "#ID##bpt_gcol_id" ),    ImGuiTableColumnFlags_WidthFixed, 48.0f );
    ImGui::TableSetupColumn ( _L( "Name##bpt_gcol_name" ),  ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn ( _L( "Parent##bpt_gcol_par" ), ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn ( _L( "#BP##bpt_gcol_cnt" ),    ImGuiTableColumnFlags_WidthFixed, 60.0f );
    ImGui::TableHeadersRow ( );

    unsigned bpcount = g_breakpoints.breakpoints->len;

    for ( unsigned i = 0; i < gcount; i++ ) {
        st_BPTGROUP *grp = &g_array_index ( g_breakpoints.groups, st_BPTGROUP, i );
        ImGui::PushID ( grp->id );
        ImGui::TableNextRow ( );

        /* Sloupec 0: enabled checkbox. */
        ImGui::TableNextColumn ( );
        bool enabled = grp->enabled;
        if ( ImGui::Checkbox ( "##gen", &enabled ) ) {
            st_DBGAPI_BPGRP_UPDATE_PARAM p;
            memset ( &p, 0, sizeof ( p ) );
            p.id = grp->id;
            p.update_mask = DBGAPI_BPGRP_UM_ENABLED;
            p.enabled = enabled;
            dbg_ui_bpgrp_update ( &p );
        };

        /* Sloupec 1: ID. */
        ImGui::TableNextColumn ( );
        ImGui::Text ( "#%d", grp->id );

        /* Sloupec 2: Name (Selectable přes celý řádek). */
        ImGui::TableNextColumn ( );
        bool is_selected = ( g_bpt_ui.selected_id == grp->id &&
                             g_bpt_ui.selected_type == BPT_ITEM_GROUP );
        const char *gname = ( grp->name && grp->name[0] ) ? grp->name : "(unnamed)";
        ImGui::Selectable ( gname, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick );
        if ( ImGui::IsItemClicked ( ImGuiMouseButton_Left ) ||
             ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_bpt_ui.selected_id = grp->id;
            g_bpt_ui.selected_type = BPT_ITEM_GROUP;
        };
        if ( ImGui::IsItemHovered ( ) &&
             ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) ) {
            bpt_edit_panel_open ( BPT_ITEM_GROUP, grp->id );
        };
        if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
            g_bpt_ui.open_groups_context = true;
        };

        /* Sloupec 3: Parent (jméno rodičovské skupiny, "-" pro root). */
        ImGui::TableNextColumn ( );
        if ( grp->parent >= 0 ) {
            st_BPTGROUP *par = breakpoints_group_find_by_id ( grp->parent );
            if ( par && par->name && par->name[0] ) {
                ImGui::TextUnformatted ( par->name );
            } else {
                ImGui::TextDisabled ( "?" );
            };
        } else {
            ImGui::TextDisabled ( "-" );
        };

        /* Sloupec 4: #BP (počet BP přímo v této skupině). */
        ImGui::TableNextColumn ( );
        unsigned nbp = 0;
        for ( unsigned j = 0; j < bpcount; j++ ) {
            st_BPT *bpt = &g_array_index ( g_breakpoints.breakpoints, st_BPT, j );
            if ( bpt->parent == grp->id ) nbp++;
        };
        if ( nbp > 0 ) {
            ImGui::Text ( "%u", nbp );
        } else {
            ImGui::TextDisabled ( "0" );
        };

        ImGui::PopID ( );
    };

    ImGui::EndTable ( );

    /* Right-click na plochu tabulky (mimo řádek) otevře menu (Add Group). */
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenBlockedByPopup ) &&
         ImGui::IsMouseReleased ( ImGuiMouseButton_Right ) ) {
        g_bpt_ui.open_groups_context = true;
    };

    if ( g_bpt_ui.open_groups_context ) {
        ImGui::OpenPopup ( "##bpt_groups_context" );
        g_bpt_ui.open_groups_context = false;
    };
    bpt_render_groups_context_menu ( );
}


/* ========================================================================= */
/*  Tree view                                                                */
/* ========================================================================= */


static void bpt_render_tree ( void ) {
    /* Vykreslení stromu — top-level (parent == -1) */
    if ( g_breakpoints.groups_first ) {
        bpt_render_groups_for_parent ( -1, false );
        bpt_render_events_for_parent ( -1, false );
    } else {
        bpt_render_events_for_parent ( -1, false );
        bpt_render_groups_for_parent ( -1, false );
    };

    /* Spotřebovat expand/collapse požadavky */
    g_bpt_ui.request_expand_all = false;
    g_bpt_ui.request_collapse_all = false;

    /* Drop target pro unparent — neviditelná plocha vyplňující zbylý prostor */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    if ( avail.y < 20.0f ) avail.y = 20.0f; /* minimální výška pro drop target */
    ImGui::InvisibleButton ( "##bpt_drop_root", avail );

    if ( ImGui::BeginDragDropTarget ( ) ) {
        if ( const ImGuiPayload *payload = ImGui::AcceptDragDropPayload ( "BPT_ITEM" ) ) {
            BptTreeItem *dropped = (BptTreeItem *)payload->Data;
            if ( dropped->type == BPT_ITEM_GROUP ) {
                st_DBGAPI_BPGRP_UPDATE_PARAM p;
                memset ( &p, 0, sizeof ( p ) );
                p.id = dropped->id;
                p.update_mask = DBGAPI_BPGRP_UM_PARENT;
                p.parent = -1;
                dbg_ui_bpgrp_update ( &p );
            } else {
                dbg_ui_bp_set_parent ( dropped->id, -1 );
            };
        };
        ImGui::EndDragDropTarget ( );
    };

    /* Kontextové menu — otevírá se pravým klikem na položku (odložené) nebo prázdnou plochu */
    if ( g_bpt_ui.open_context_menu ) {
        ImGui::OpenPopup ( "##bpt_context" );
        g_bpt_ui.open_context_menu = false;
    } else if ( ImGui::IsItemClicked ( ImGuiMouseButton_Right ) ) {
        /* Pravý klik na prázdnou plochu (InvisibleButton) */
        ImGui::OpenPopup ( "##bpt_context" );
    };

    bpt_render_context_menu ( );
}


/* ========================================================================= */
/*  Top menu                                                                 */
/* ========================================================================= */


static void bpt_render_menu ( void ) {
    if ( !ImGui::BeginMenuBar ( ) ) return;

    /* === File === */
    if ( ImGui::BeginMenu ( _L ( "File##bpt" ) ) ) {

        if ( ImGui::MenuItem ( _L ( "Load##bpt" ) ) ) {
            breakpoints_load_from_file ( );
        };

        if ( ImGui::MenuItem ( _L ( "Save##bpt" ) ) ) {
            breakpoints_save_to_file ( );
        };

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L ( "Load Breakpoints From...##bpt" ) ) ) {
            IGFD::FileDialogConfig config;
            config.path = ( g_breakpoints.default_file && g_breakpoints.default_file[0] )
                          ? g_breakpoints.default_file : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles;
            ImGuiFileDialog::Instance ( )->OpenDialog (
                "BptLoadDialog", _( "Load Breakpoints From..." ),
                ".bpt,.*", config );
        };

        if ( ImGui::MenuItem ( _L ( "Save Breakpoints As...##bpt" ) ) ) {
            IGFD::FileDialogConfig config;
            config.path = ( g_breakpoints.default_file && g_breakpoints.default_file[0] )
                          ? g_breakpoints.default_file : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ConfirmOverwrite;
            ImGuiFileDialog::Instance ( )->OpenDialog (
                "BptSaveDialog", _( "Save Breakpoints As..." ),
                ".bpt", config );
        };

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L ( "Hide##bpt" ), "Alt+B" ) ) {
            g_gui->showBreakpointsWindow = false;
        };

        ImGui::EndMenu ( );
    };

    /* === Settings === */
    if ( ImGui::BeginMenu ( _L ( "Settings##bpt" ) ) ) {

        bool auto_load = g_breakpoints.auto_load ? true : false;
        if ( ImGui::MenuItem ( _L ( "Auto Load##bpt" ), NULL, auto_load ) ) {
            g_breakpoints.auto_load = !g_breakpoints.auto_load;
        };

        bool auto_save = g_breakpoints.auto_save ? true : false;
        if ( ImGui::MenuItem ( _L ( "Auto Save##bpt" ), NULL, auto_save ) ) {
            g_breakpoints.auto_save = !g_breakpoints.auto_save;
        };

        ImGui::Separator ( );

        if ( ImGui::MenuItem ( _L ( "Default BPT file...##bpt" ) ) ) {
            IGFD::FileDialogConfig config;
            config.path = ( g_breakpoints.default_file && g_breakpoints.default_file[0] )
                          ? g_breakpoints.default_file : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles;
            ImGuiFileDialog::Instance ( )->OpenDialog (
                "BptDefaultFileDialog", _( "Select Default BPT File" ),
                ".bpt,.*", config );
        };

        /* Aktuální cesta jako disabled info pod tlačítkem */
        if ( g_breakpoints.default_file && g_breakpoints.default_file[0] ) {
            ImGui::Indent ( );
            ImGui::TextDisabled ( "%s", g_breakpoints.default_file );
            ImGui::Unindent ( );
        };

        ImGui::Separator ( );

        bool groups_first = g_breakpoints.groups_first ? true : false;
        if ( ImGui::MenuItem ( _L ( "Groups First##bpt" ), NULL, groups_first ) ) {
            g_breakpoints.groups_first = !g_breakpoints.groups_first;
        };

        ImGui::EndMenu ( );
    };

    ImGui::EndMenuBar ( );
}


/* ========================================================================= */
/*  Akční lišta                                                              */
/* ========================================================================= */


static void bpt_render_action_bar ( void ) {
    /* === Levá skupina === */

    /* Hex input — max 6 znaků (4 platné hex + prostor) */
    ImGui::SetNextItemWidth ( ImGui::CalcTextSize ( "FFFFFFF" ).x );
    bool enter_pressed = ImGui::InputText ( "##bpt_hex", g_bpt_ui.hex_input, sizeof ( g_bpt_ui.hex_input ),
                       ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CallbackCharFilter |
                       ImGuiInputTextFlags_EnterReturnsTrue,
                       bpt_hex_input_filter );

    ImGui::SameLine ( 0.0f, 10.0f );

    /* Tlačítko Add — aktivní jen pokud je něco vyplněno */
    uint16_t addr;
    bool has_valid_input = bpt_parse_hex_input ( g_bpt_ui.hex_input, &addr );

    ImGui::BeginDisabled ( !has_valid_input );
    if ( ( ImGui::Button ( _L ( "Add##bpt" ) ) || ( enter_pressed && has_valid_input ) ) && has_valid_input ) {
        /* dbg_ui_bp_add prejde pres CMD_BP_ADD = breakpoints_add_auto
         * s NULL name = setter automaticky generuje "Addr: 0x%04X" pres
         * set_auto_name(true), takze nazev je identicky s drivejsim
         * explicit snprintf. */
        dbg_ui_bp_add ( addr, NULL );

        /* Vyčistit input */
        g_bpt_ui.hex_input[0] = '\0';
    };
    ImGui::EndDisabled ( );

    ImGui::SameLine ( 0.0f, 10.0f );
    ImGui::SeparatorEx ( ImGuiSeparatorFlags_Vertical );

    /* === Pravá skupina === */
    ImGui::SameLine ( );

    /* Expandující mezera — posunout Save doprava, +50% pravý okraj za Save */
    float save_width = ImGui::CalcTextSize ( _( "Save" ) ).x + ImGui::GetStyle ( ).FramePadding.x * 2.0f;
    float right_margin = ImGui::GetStyle ( ).WindowPadding.x * 0.5f;
    float avail = ImGui::GetContentRegionAvail ( ).x;
    float spacing = avail - save_width - 10.0f - 10.0f - right_margin;

    if ( spacing > 0.0f ) {
        ImGui::Dummy ( ImVec2 ( spacing, 0 ) );
        ImGui::SameLine ( );
    };

    ImGui::SeparatorEx ( ImGuiSeparatorFlags_Vertical );
    ImGui::SameLine ( 0.0f, 10.0f );

    /* Tlačítko Save - povoleno vždy, včetně prázdného stavu. Po smazání všech
     * BP musí jít explicitně přepsat soubor "vyčištěným" stavem (auto-save při
     * ukončení emulátoru na to nestačí). Dřív bylo disabled při 0 skupin i 0
     * BP, což bránilo uložení prázdného stavu a nekonzistentně s menu
     * File -> Save (to disabled nikdy nebylo). breakpoints_save_to_filepath()
     * prázdný stav korektně uloží (validní JSON s prázdnými poli). */
    if ( ImGui::Button ( _L ( "Save##bpt_bar" ) ) ) {
        breakpoints_save_to_file ( );
    };
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Save breakpoints to file (an empty state overwrites the file too)." ) );
    };
}




/* ========================================================================= */
/*  Veřejné API                                                              */
/* ========================================================================= */


void breakpoints_show_hide_window ( void ) {
    g_gui->showBreakpointsWindow = !g_gui->showBreakpointsWindow;
}


extern "C" void bpt_window_focus_id ( int bp_id ) {
    /* V1.E.6.A: Activity dvojklik routing - otevři okno, ověř že BP s
     * daným ID v storage existuje a pokud ano nastav selected_id +
     * pending_focus_id. Render-loop při dalším frame podle
     * pending_focus_id přesune tree-view focus a otevře editační panel.
     *
     * Pokud BP s daným ID neexistuje (= mezičasem smazán), spotřeba je
     * no-op kromě otevření okna. Sentinel <= 0 = no-op (BP id v storage
     * začíná od 1 monotonic counterem). */
    if ( bp_id <= 0 ) return;
    if ( !g_bpt_ui.initialized ) {
        bpt_state_init ( );
    };
    const st_BPT *bpt = breakpoints_find_by_id ( bp_id );
    if ( !bpt ) {
        /* Otevři okno i tak (= user pak vidí prázdný stav nebo log) */
        g_gui->showBreakpointsWindow = true;
        return;
    };
    g_gui->showBreakpointsWindow = true;
    g_bpt_ui.selected_id = bp_id;
    g_bpt_ui.selected_type = BPT_ITEM_EVENT;
    g_bpt_ui.pending_focus_id = bp_id;
    /* Přímé otevření editačního panelu = funkční ekvivalent "focus".
     * Selected_id zajistí, že tree view v render-loop bude označený. */
    bpt_edit_panel_open ( BPT_ITEM_EVENT, bp_id );
}


/* V1.7 C.3.5 helper callbacks pro TEXT filter pole.
 *
 * Cfgelement_bind pro CFGENTYPE_TEXT očekává char** (= adresu pointer
 * proměnné, alokuje string heap-em). Nemůžeme bindovat na char[N]
 * buffer v rámci structu - cfg lib by dereferencoval bytes bufferu
 * jako pointer hodnotu = corrupted state ("bordel ve filtrech").
 *
 * Místo bind tedy explicit propagate/save callbacks které manually
 * kopírují string mezi cfg elementem a fixed-size bufferem. cbdata
 * nese index 0..5 přes GPOINTER_TO_INT. */
static void bpt_filter_text_propagate_cb ( void *e, void *data ) {
    int idx = GPOINTER_TO_INT ( data );
    if ( idx < 0 || idx >= BPT_TAB_FILTER_COUNT ) return;
    const char *val = cfgelement_get_text_value ( (st_CFGELEMENT *)e );
    if ( val ) {
        g_strlcpy ( g_bpt_ui.tab_filter_text[idx], val, BPT_TAB_FILTER_TEXT_LEN );
    } else {
        g_bpt_ui.tab_filter_text[idx][0] = '\0';
    };
}

static void bpt_filter_text_save_cb ( void *e, void *data ) {
    int idx = GPOINTER_TO_INT ( data );
    if ( idx < 0 || idx >= BPT_TAB_FILTER_COUNT ) return;
    cfgelement_set_text_value ( (st_CFGELEMENT *)e, g_bpt_ui.tab_filter_text[idx] );
}


extern "C" void bpt_window_register_persistence ( void *cmod_void ) {
    if ( !cmod_void ) return;
    /* Init defaultů PŘED registrací cfgelementů (= memmap_window pattern). */
    if ( !g_bpt_ui.initialized ) {
        bpt_state_init ( );
    };

    st_CFGMODULE *cmod = (st_CFGMODULE *)cmod_void;
    st_CFGELEMENT *elm;

    /* active_tab: KEYWORD typ s string-namovou mapou.
     * Default 0 = "all". Hodnoty 1..6 = per-typ taby (= 1 + en_BPT_TAB_FILTER),
     * 7 = záložka Groups. */
    elm = cfgmodule_register_new_element ( cmod, (char *)"active_tab",
                                           CFGENTYPE_KEYWORD, 0,
                                           0,                 "all",
                                           1 + BPT_TAB_EXEC,   "exec",
                                           1 + BPT_TAB_MEM,    "mem",
                                           1 + BPT_TAB_IO,     "io",
                                           1 + BPT_TAB_IRQ,    "irq",
                                           1 + BPT_TAB_EVENTS, "events",
                                           1 + BPT_TAB_MISC,   "misc",
                                           7,                  "groups",
                                           -1 );
    cfgelement_set_handlers ( elm,
                              (void *)&g_bpt_ui.active_tab,
                              (void *)&g_bpt_ui.active_tab );

    /* Per-tab text filter + enabled-only toggle. Names odpovídají
     * indexům en_BPT_TAB_FILTER pro čitelnost v .ini souboru. */
    static const char *k_tab_names[BPT_TAB_FILTER_COUNT] = {
        "exec", "mem", "io", "irq", "events", "misc"
    };
    for ( int i = 0; i < BPT_TAB_FILTER_COUNT; i++ ) {
        char key[64];
        snprintf ( key, sizeof ( key ), "tab_%s_filter", k_tab_names[i] );
        elm = cfgmodule_register_new_element ( cmod, key, CFGENTYPE_TEXT, "" );
        /* Místo cfgelement_bind (= mutates char**) explicit callbacks
         * kopírují string mezi elementem a char[BPT_TAB_FILTER_TEXT_LEN]. */
        cfgelement_set_propagate_cb ( elm, bpt_filter_text_propagate_cb,
                                       GINT_TO_POINTER ( i ) );
        cfgelement_set_save_cb ( elm, bpt_filter_text_save_cb,
                                  GINT_TO_POINTER ( i ) );

        snprintf ( key, sizeof ( key ), "tab_%s_enabled_only", k_tab_names[i] );
        elm = cfgmodule_register_new_element ( cmod, key, CFGENTYPE_BOOL, 0 );
        cfgelement_set_handlers ( elm,
                                  (void *)&g_bpt_ui.tab_filter_enabled_only[i],
                                  (void *)&g_bpt_ui.tab_filter_enabled_only[i] );
    };

    /* Po propagaci hodnot z .ini (= cfgmodule_propagate volá caller)
     * flag říká render-loopu "při dalším frame force-select active_tab
     * přes ImGuiTabItemFlags_SetSelected, pak resetuj". */
    g_bpt_ui.restore_active_tab_once = true;
}


void imgui_breakpoints_window ( bool *p_open ) {
    if ( !*p_open ) return;

    /* Inicializace při prvním otevření */
    if ( !g_bpt_ui.initialized ) {
        bpt_state_init ( );
    };

    ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse;

    const char *bpt_title = _L ( "Breakpoints##bpt_main" );
    /* Šířka 825 dá prostor 8 tabům (All/EXEC/Memory/I/O/IRQ/Events/Misc/
     * Groups) v jediné řadě + sloupcům tabulky vč. nového Group sloupce.
     * Výška 480 pokryje menu bar + tab bar + list BP + bottom row (Add
     * input + Save tlačítko). Jen initial velikost (FirstUseEver). */
    auto_layout_first_use_portrait ( bpt_title, 825.0f, 480.0f );
    if ( !ImGui::Begin ( bpt_title, p_open, flags ) ) {
        ImGui::End ( );
        return;
    };

    /* Alt+B v okně breakpointů zavírá okno */
    if ( ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) {
        /* Guard: pokud okno bylo právě otevřeno přes Alt+B, nepřijímáme
         * Alt+B na tomto framu (jinak by se ihned zavřelo). */
        if ( g_bpt_ui.opened_via_alt_b ) {
            g_bpt_ui.opened_via_alt_b = false;
        } else if ( ImGui::GetIO ( ).KeyAlt && ImGui::IsKeyPressed ( ImGuiKey_B, false ) ) {
            *p_open = false;
            ImGui::End ( );
            return;
        };
    };

    /* ESC zavírá okno (pokud není otevřen popup) */
    if ( ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) &&
         !ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId ) &&
         ImGui::IsKeyPressed ( ImGuiKey_Escape, false ) )
    {
        *p_open = false;
        ImGui::End ( );
        return;
    };

    /* V1.7 fix: detekce expiry pro last_triggered highlight.
     *
     * DBGAPI_MSG_RUNNING / STEP_COMPLETE se z emu jádra nesílá (pouze
     * BREAKPOINT_HIT je hooked), takže dispatcher reset na RUNNING
     * v praxi nikdy nefiruje. Místo toho detekujeme stav přes:
     *   1) Emu už neni paused (= user clikl Play) -> clear.
     *   2) Emu je paused ale PC se posunul (= user dělal Step) -> clear.
     *
     * Snímkujeme PC při prvním frame kdy vidíme nové last_triggered_id;
     * porovnáváme v dalších framech. Pokud se hodnota id nezměnila ale
     * PC se posunul nebo emu už neni paused, highlight expiroval. */
    {
        static int s_last_seen_trig_id = -1;
        static uint16_t s_pc_at_trig = 0;

        int cur_trig = g_bpt_ui.last_triggered_id;
        if ( cur_trig < 0 ) {
            s_last_seen_trig_id = -1;
        } else if ( cur_trig != s_last_seen_trig_id ) {
            /* Nový HIT v tomto frame - snímek PC (= kde se emu zastavil). */
            s_last_seen_trig_id = cur_trig;
            s_pc_at_trig = g_mzarch_main.cpu->pc;
        } else {
            /* Stejný triggered_id jako minulý frame - check state movement. */
            bool emu_running = !EMULATOR_TEST_PAUSED;
            bool pc_moved = ( g_mzarch_main.cpu->pc != s_pc_at_trig );
            if ( emu_running || pc_moved ) {
                g_bpt_ui.last_triggered_id = -1;
                g_bpt_ui.scroll_to_last_triggered = false;
                s_last_seen_trig_id = -1;
            };
        };
    };

    /* Top menu */
    bpt_render_menu ( );

    /* Tree view — v child regionu, aby expandoval.
     * Spodní oblast: Dummy(5) + Separator(0 layout) + Dummy(5) + action bar + 4× ItemSpacing.
     * Separator má layout výšku 0 (při thickness 1), ale ItemSpacing.y se přidává mezi každé dva widgety. */
    float frame_h = ImGui::GetFrameHeight ( );
    float spacing_y = ImGui::GetStyle ( ).ItemSpacing.y;
    float bottom_h = frame_h + 4.0f * spacing_y + 10.0f;

    ImVec2 child_size = ImVec2 ( 0, -bottom_h );
    ImGui::BeginChild ( "##bpt_tabs_container", child_size, ImGuiChildFlags_None );

    /* V1.7 C.3.2: TabBar scaffold. "All" tab obsahuje stávající tree view
     * (zachovává plnou hierarchii skupin + reparenting drag-drop). Per-typ
     * taby ("EXEC", "Memory", "I/O", "IRQ", "Events", "Misc") jsou prozatím
     * placeholder - implementace s per-typ column setem přijde v C.3.3.
     *
     * Stable ID přes ###bpt_tab_<name> (= localized title zachová ID).
     * Tab bar samotný má jen ##bpt_tabs (interní, ne user-visible). */
    if ( ImGui::BeginTabBar ( "##bpt_tabs", ImGuiTabBarFlags_None ) ) {

        /* V1.7 C.3.5: per-tab restore flag - pokud po startu app
         * restore_active_tab_once = true, force-selectneme tab dle
         * persistovaného active_tab (unsigned, 0=All, 1..6=tab+1).
         * Po prvním renderu reset. */
        const bool restore_now = g_bpt_ui.restore_active_tab_once;
#define BPT_TAB_FLAGS_FOR(persist_idx)                                         \
        ( ( restore_now && g_bpt_ui.active_tab == (persist_idx) )              \
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None )

        if ( ImGui::BeginTabItem ( _L ( "All###bpt_tab_all" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 0 ) ) ) {
            g_bpt_ui.active_tab = 0;
            bpt_render_tree ( );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "EXEC###bpt_tab_exec" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_EXEC ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_EXEC;
            bpt_render_filtered_table ( BPT_TAB_EXEC );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "Memory###bpt_tab_mem" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_MEM ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_MEM;
            bpt_render_filtered_table ( BPT_TAB_MEM );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "I/O###bpt_tab_io" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_IO ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_IO;
            bpt_render_filtered_table ( BPT_TAB_IO );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "IRQ###bpt_tab_irq" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_IRQ ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_IRQ;
            bpt_render_filtered_table ( BPT_TAB_IRQ );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "Events###bpt_tab_evt" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_EVENTS ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_EVENTS;
            bpt_render_filtered_table ( BPT_TAB_EVENTS );
            ImGui::EndTabItem ( );
        };

        if ( ImGui::BeginTabItem ( _L ( "Misc###bpt_tab_misc" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 1 + BPT_TAB_MISC ) ) ) {
            g_bpt_ui.active_tab = 1 + BPT_TAB_MISC;
            bpt_render_filtered_table ( BPT_TAB_MISC );
            ImGui::EndTabItem ( );
        };

        /* Záložka Groups (správa skupin) - na konci za per-typ taby.
         * Persist index 7 (= za 0..6). */
        if ( ImGui::BeginTabItem ( _L ( "Groups###bpt_tab_groups" ), NULL,
                                    BPT_TAB_FLAGS_FOR ( 7 ) ) ) {
            g_bpt_ui.active_tab = 7;
            bpt_render_groups_table ( );
            ImGui::EndTabItem ( );
        };

#undef BPT_TAB_FLAGS_FOR

        ImGui::EndTabBar ( );

        /* Reset restore flag - jen prvni render po startu app. */
        g_bpt_ui.restore_active_tab_once = false;
    };

    ImGui::EndChild ( );

    /* Horizontální separátor s 5px mezerou nad a pod */
    ImGui::Dummy ( ImVec2 ( 0, 5.0f ) );
    ImGui::Separator ( );
    ImGui::Dummy ( ImVec2 ( 0, 5.0f ) );

    /* Akční lišta */
    bpt_render_action_bar ( );

    ImGui::End ( );

    /* Nemodální dockable panel pro Edit BP / Edit Group (single-instance).
     * Render musí být MIMO ImGui::Begin/End mateřského Breakpoints okna,
     * jinak by se editor renderoval jako nested child window - což
     * způsobí broken click detection na Apply/Cancel tlačítkách
     * a problémy s dockingem. */
    bpt_edit_panel_render ( );

    /* === File dialogy (Load From / Save As / Default file) ===
     * Render mimo Begin/End mateřského okna ze stejných důvodů jako
     * edit panel - dialogy jsou top-level modální okna. */
    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "BptLoadDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            breakpoints_load_from_filepath ( path.c_str ( ) );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "BptSaveDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            breakpoints_save_to_filepath ( path.c_str ( ) );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "BptDefaultFileDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
            /* Realokuj default_file přes g_strdup (= konzistentní s GLib
             * memory management používaným v breakpoints.c). */
            if ( g_breakpoints.default_file ) g_free ( g_breakpoints.default_file );
            g_breakpoints.default_file = g_strdup ( path.c_str ( ) );
        };
        ImGuiFileDialog::Instance ( )->Close ( );
    };
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
