/**
 * @file membrowser_window.cpp
 * @brief Memory Browser hlavní okno (V0 + V1) - implementace.
 *
 * Layout (V1 - 3 toolbar řádky + 1 status řádek + opt. side panel):
 *   +- Memory Browser ###mb_main -----------------------------------------+
 *   | Row 1: [Load][Save] | [Edit] | [Layers ▶] | Region:                 |
 *   | Row 2: Encoding: | Bytes/row: | [x] ASCII                           |
 *   | Row 3: HEX:__ DEC:__ [Goto] | [Search] | [<][>] Page N of M|0xHHHH  |
 *   +---------------------------------------------------------------------+
 *   | (hex view scroll - core renderer)        | [Layers panel - opt.]    |
 *   |   addr: XX XX ... | ASCII                | [x] CDL X/R/W            |
 *   |                                          | [x] Heatmap              |
 *   |                                          | [x] Snapshot Δ           |
 *   |                                          | [Snapshot Now][Reset]    |
 *   |                                          | [x] Frozen Bytes         |
 *   |                                          | [x] Symbols              |
 *   +---------------------------------------------------------------------+
 *   | Status: Size N B | R/W | mapped | Z80 base 0xHHHH || Cursor: ...    |
 *   +---------------------------------------------------------------------+
 *
 * V1 features:
 *   - Layers panel (pravý sidebar, default collapsed) s 7 layer toggles
 *   - per-byte color overlay nad hex view (CDL/heatmap/snapshot Δ/frozen)
 *   - symbol overlay v ASCII column (Logical region only)
 *   - right-click context menu: Add bookmark, Freeze/Unfreeze byte
 *   - Snapshot Now/Reset tlačítka v Layers panelu (per region)
 *
 * Region/RW/mapped/Z80 base + Cursor jsou na 1 řádku status baru (per
 * stará GTK reference). V0-polish-6: Row 2 split na Row 2 (jen view
 * options: Encoding/Bytes-row/ASCII) + Row 3 (navigace: HEX/DEC/Goto/
 * Search/Page) - lepší vizuální oddělení "co zobrazujeme" vs "kam jdeme".
 * Warningy (KOI8-CS missing) případně zaberou extra řádek - výjimečné.
 *
 * Multi-instance: V0 lazy singleton "main". V3+ multiple instances
 * (#2..#5) přes membrowser_view_create("2") atd.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_window.h"
#include "membrowser_state.h"
#include "membrowser_io.h"
#include "membrowser_encoding.h"
#include "membrowser_hexview.h"
#include "membrowser_fileio.h"
#include "membrowser_layers.h"
#include "membrowser_regions.h"
#include "membrowser_search.h"
#include "membrowser_char_inserter.h"
#include "membrowser_fill_dialog.h"
#include "membrowser_annot_dialog.h"
/* Edit semantics: recently-edited storage - clear na region switch. */
#include "membrowser_edited.h"
#include "membrowser_undo.h"
#include "membrowser_annotations.h"
#include "hex_view_backend.h"

#include "libs/imgui/imgui.h"
/* V4.1+: ImGui::GetActiveID() je internal API - potřebné pro Pattern
 * Builder sync logiku (detekce kdy ASCII field má focus, abychom ho
 * nepřepisovali per-frame z search_pattern). */
#include "libs/imgui/imgui_internal.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>
#include <new>     /* std::nothrow */

extern "C" {
/* Direct include kvůli st_REGION_DESC + REGION_KIND_* konstantám pro
 * dropdown / fallback logic. Nikde v tomto souboru se NEVOLAJÍ funkce
 * dbgapi_regions_read/write/enumerate přímo - vše chodí přes
 * membrowser_io_*. */
#include "emulator/debugger/dbgapi_regions.h"

/* V3 multi-view: cfgroot_register_new_module pro 5 instancí + legacy
 * fallback sekce. Registrace probíhá uvnitř
 * membrowser_window_register_persistence_all (volaná z debugger.c). */
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"

/* PC/SP markers - direct cpu access pattern (proven by bpt_window,
 * bm_window, debugger_window). Single-thread UI read, no mutex. */
#include "mzarch/mzarch.h"

/* V0-leftovers F4: --memory-browser CLI flag - sdlapp_option_present. */
#include "libs/sdlapp/sdlapp_options.h"

/* V2: banking status row rozšířený - PROHIBITED/SCRW640/DMD detekce
 * per MZ-800 (GDG DMD + memory banking flagy). Pro MZ-700/MZ-1500 jen
 * PROHIBITED flag (700 mode + MZ-700 standalone). */
#include "emulator/hw-generic/memory/memory.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/memory/mz800_memory.h"
#elif MZARCH == 700
#include "mzarch/mz700/memory/mz700_memory.h"
#endif
}


/**
 * @brief Interní reprezentace MembrowserView instance.
 *
 * V3 multi-view: 5 nezávislých instancí (main + #2..#5). Každá drží
 * vlastní st_MEMBROWSER_STATE + per-frame snapshot regionů. Sdílí pouze
 * globální freeze subsystém (= cheat-engine freeze bytes je shared mezi
 * instancemi záměrně - hex view kteréhokoliv okna vidí stejné frozen
 * adresy).
 *
 * window_id (např. "main", "2".."5") je použito v ###StableID konstrukci -
 * statický string z @c SLOT_WINDOW_ID pole, instance ho jen drží.
 */
struct MembrowserView
{
    int instance_idx;                 /**< 0=main, 1..4=#2..#5. */
    const char *window_id;            /**< Statický string ("main", "2", ...). */
    char title_id[96];                /**< Cached "Memory Browser%s###mb_<id>". */
    st_MEMBROWSER_STATE state;

    /* Per-frame snapshot regionů. */
    st_MEMBROWSER_REGION_SNAPSHOT regions;
};

/* Stabilní window_id stringy per slot - drží lifetime po dobu programu,
 * pointer se předává do membrowser_view_create. */
static const char *const SLOT_WINDOW_ID[MB_INSTANCE_COUNT] = {
    "main", "2", "3", "4", "5"
};

/* Pole 5 instancí; index 0 = main, 1..4 = #2..#5. NULL = ještě nevytvořeno
 * (lazy create při prvním renderu pro odpovídající flag = true).
 *
 * Main: lazy v membrowser_window_render() při g_gui->showMemoryBrowserWindow.
 * #2..#5: lazy v render_one_slot() při g_gui->showMemoryBrowserWindowExtra[i]. */
static MembrowserView *s_views[MB_INSTANCE_COUNT] = {
    nullptr, nullptr, nullptr, nullptr, nullptr
};


/* ---- Pomocné rendery -------------------------------------------------- */


/* Region label - lokalizovaný short název z REGION_KIND.
 * dbgapi_regions vrací anglické ASCII name v st_REGION_DESC.name; pro
 * dropdown přidáváme i sub_id badge.
 * Buffer per-call (NESmí být thread-safe - jen UI vlákno). */
static const char *region_descriptor_label ( const st_REGION_DESC *r )
{
    static char buf[96];
    if ( r->sub_id != 0 ) {
        std::snprintf ( buf, sizeof ( buf ), "%s [#%d]", r->name, r->sub_id );
    } else {
        std::snprintf ( buf, sizeof ( buf ), "%s", r->name );
    }
    return buf;
}


/* Najde aktuální region size (helper pro toolbar a status). */
static uint64_t find_current_region_size ( const MembrowserView *v )
{
    for ( int i = 0; i < v->regions.count; i++ ) {
        if ( v->regions.items[i].id == v->state.current_region_id ) {
            return ( uint64_t ) v->regions.items[i].size;
        }
    }
    return 0;
}


/* Najde writable flag aktuálního regionu. */
static bool find_current_region_writable ( const MembrowserView *v )
{
    for ( int i = 0; i < v->regions.count; i++ ) {
        if ( v->regions.items[i].id == v->state.current_region_id ) {
            return v->regions.items[i].writable != 0;
        }
    }
    return false;
}


/* Toolbar row 1 - file ops + Edit toggle + Region.
 * Per Michalova úprava (V0-polish-5) Encoding/Bytes-per-row/ASCII přesunuty
 * na Row 2, aby Row 1 zůstal jen pro "destinaci dat" (region + file ops +
 * edit master switch):
 *   [Load BIN] [Save BIN] | [Edit: OFF/ON] | Region: ▼
 */
static void render_toolbar_row1 ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    const st_MEMBROWSER_REGION_SNAPSHOT *snap = &v->regions;

    bool writable = find_current_region_writable ( v );
    uint64_t total = find_current_region_size ( v );

    /* --- Load BIN. --- */
    if ( ImGui::Button ( _L ( "Load BIN###mb_load_bin" ) ) ) {
        membrowser_fileio_open_load ( st->current_region_id );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Load .bin file into current region" ) );
    }

    ImGui::SameLine ( );

    /* --- Save BIN. --- */
    bool save_dis = ( total == 0 );
    if ( save_dis ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L ( "Save BIN###mb_save_bin" ) ) ) {
        membrowser_fileio_open_save ( st->current_region_id );
    }
    if ( save_dis ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Save current region content into .bin file" ) );
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- Edit toggle (single gate, default OFF). Pro RO region zakázán.
     * V1-polish-1: zjednodušený model - jediné tlačítko ovládá editaci.
     * Dříve dvojitý gate (edit_enabled master + edit_active F2 toggle)
     * způsoboval, že stisk tlačítka Edit: ON sice přepnul label, ale
     * dispatch kláves vyžadoval ještě edit_active=true (= F2 stisk), což
     * navíc kvůli early-return v dispatch nefungovalo. Tlačítko + F2 nyní
     * obě dělají to samé: toggle edit_enabled. */
    bool edit_dis = !writable;
    if ( edit_dis ) {
        ImGui::BeginDisabled ( );
        st->edit_enabled = false;  /* RO -> force OFF */
    }
    const char *edit_label = st->edit_enabled
        ? _L ( "Edit: ON###mb_edit_toggle" )
        : _L ( "Edit: OFF###mb_edit_toggle" );

    /* V1-polish-2 Bug 2: výrazný červený highlight když edit mode ON.
     * Pattern z bookmarks/freeze "destructive" tlačítek - upozornění že
     * další klávesy budou modifikovat data emulátoru.
     *
     * Barvy: button = sytá rezedo-červená, hovered o cca 15 % světlejší,
     * active (mouse down) tmavší. OFF zůstává defaultní ImGui barva. */
    bool pushed_edit_colors = false;
    if ( st->edit_enabled && !edit_dis ) {
        ImGui::PushStyleColor ( ImGuiCol_Button,
                                 ImVec4 ( 0.78f, 0.20f, 0.20f, 1.00f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonHovered,
                                 ImVec4 ( 0.92f, 0.32f, 0.32f, 1.00f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonActive,
                                 ImVec4 ( 0.60f, 0.12f, 0.12f, 1.00f ) );
        pushed_edit_colors = true;
    }
    if ( ImGui::Button ( edit_label ) ) {
        st->edit_enabled = !st->edit_enabled;
        st->edit_nibble = 0;
    }
    if ( pushed_edit_colors ) ImGui::PopStyleColor ( 3 );
    if ( edit_dis ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            edit_dis
              ? _( "Region is read-only" )
              : _( "Toggle edit mode (off by default for safety, F2 shortcut)" ) );
    }

    /* Edit semantics: "Clear edited" tlačítko pro manuální vymazání
     * recently-edited highlight bytů. Disabled pokud žádné záznamy.
     * Po region/bank switche se clear stejně spustí automaticky. */
    int edited_n = membrowser_edited_count_for_instance ( st->instance_idx );
    {
        bool clr_dis = ( edited_n == 0 );
        if ( clr_dis ) ImGui::BeginDisabled ( );
        ImGui::SameLine ( );
        if ( ImGui::Button ( _L ( "Clear edited###mb_edit_clear" ) ) ) {
            membrowser_edited_clear_for_instance ( st->instance_idx );
        }
        if ( clr_dis ) ImGui::EndDisabled ( );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( _( "Clear recently-edited byte highlights (%d)" ),
                                 edited_n );
        }
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- V2: Regions sidebar toggle. --- */
    membrowser_regions_render_toolbar_toggle ( st );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- V1: Layers panel toggle. --- */
    membrowser_layers_render_toolbar_toggle ( st );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- Region combo (grupy + dynamické sub-controls). ---
     *
     * Dropdown listuje LOGICKÉ GRUPY (kategorie), ne per-bank items. To
     * dramaticky zkrátí seznam i s plně připojeným Luftner Memextem (256
     * banks) a 16 MB Ramdiskem (256 banks). Per-bank výběr pro Memext /
     * Ramdisk se řeší dynamicky vedle dropdown (Type combo + Bank combo).
     *
     * Per-bank Sidebar Regions tree (membrowser_regions.cpp) zůstává beze
     * změny - tam má per-bank zobrazení smysl jako navigační strom.
     *
     * Per-grupa "dostupnost":
     *   - Grupa se zobrazí v dropdown jen pokud existuje aspoň 1 region
     *     daného druhu v aktuálním snapshot (= per-arch + connected HW).
     *   - Selected grupa se odvozuje z aktuálního (kind, sub_id) páru
     *     pomocí membrowser_region_kind_to_group.
     */
    ImGui::TextUnformatted ( _( "Region:" ) );
    ImGui::SameLine ( );

    /* Zjisti aktuální grupu z (kind, sub_id). */
    en_MEMBROWSER_REGION_GROUP cur_group = membrowser_region_kind_to_group (
        st->current_key.kind, st->current_key.sub_id );

    /* Připrav set "available" grup z aktuálního snapshotu. Iterace přes
     * snap je O(n), ale n je malé (jedno volání per frame). */
    bool group_available[MB_RG__COUNT] = { false };
    for ( int i = 0; i < snap->count; i++ ) {
        const st_REGION_DESC *r = &snap->items[i];
        if ( !r->connected ) continue;
        en_MEMBROWSER_REGION_GROUP g = membrowser_region_kind_to_group (
            ( int ) r->kind, r->sub_id );
        if ( g >= 0 && g < MB_RG__COUNT ) {
            group_available[g] = true;
        }
    }

    /* Label aktuální grupy. */
    const char *cur_group_label = membrowser_group_label_en ( cur_group );

    ImGui::SetNextItemWidth ( 200 );
    if ( ImGui::BeginCombo ( "##mb_region_group_combo", cur_group_label ) ) {
        for ( int g = 0; g < MB_RG__COUNT; g++ ) {
            if ( !group_available[g] ) continue;
            const char *glabel = membrowser_group_label_en (
                ( en_MEMBROWSER_REGION_GROUP ) g );
            bool selected = ( ( int ) cur_group == g );
            ImGui::PushID ( g );
            if ( ImGui::Selectable ( glabel, selected ) ) {
                /* Změna grupy: pro grupy bez sub-controls okamžitě resolvuj
                 * konkrétní (kind, sub_id). Pro grupy se sub-controls vyber
                 * první dostupný region daného druhu jako default - dynamic
                 * sub-control se pak může přepnout. */
                en_MEMBROWSER_REGION_GROUP new_g =
                    ( en_MEMBROWSER_REGION_GROUP ) g;
                int new_kind = REGION_KIND_LOGICAL;
                int new_sub  = 0;

                if ( membrowser_group_has_subcontrols ( new_g ) ) {
                    /* Najdi první connected region patřící do této grupy. */
                    bool found = false;
                    for ( int i = 0; i < snap->count; i++ ) {
                        const st_REGION_DESC *r = &snap->items[i];
                        if ( !r->connected ) continue;
                        if ( membrowser_region_kind_to_group (
                                 ( int ) r->kind, r->sub_id ) == new_g ) {
                            new_kind = ( int ) r->kind;
                            new_sub  = r->sub_id;
                            found = true;
                            break;
                        }
                    }
                    if ( !found ) {
                        /* Fallback - kanonický (kind, sub_id) z helperu.
                         * Resolvuje se per find_region za chvíli. */
                        membrowser_group_to_region ( new_g, 0, 0,
                                                      &new_kind, &new_sub );
                    }
                } else {
                    membrowser_group_to_region ( new_g, 0, 0,
                                                  &new_kind, &new_sub );
                }

                int new_id = membrowser_io_find_region ( snap, new_kind,
                                                          new_sub );
                if ( new_id >= 0 ) {
                    st->current_region_id = new_id;
                    st->current_key.kind = new_kind;
                    st->current_key.sub_id = new_sub;
                    st->cursor_addr = 0;
                    st->scroll_top_addr = 0;
                    st->edit_nibble = 0;
                    membrowser_state_save_to_persisted ( st );
                }
            }
            if ( selected ) ImGui::SetItemDefaultFocus ( );
            ImGui::PopID ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Select memory region category" ) );
    }

    /* --- Sub-controls pro Memext (Type RAM/FLASH + Bank). --- */
    if ( cur_group == MB_RG_MEMEXT ) {
        /* Type combo: vyber dostupné typy iterací snap-em. */
        bool type_avail[2] = { false, false };  /* 0=RAM, 1=FLASH */
        for ( int i = 0; i < snap->count; i++ ) {
            const st_REGION_DESC *r = &snap->items[i];
            if ( !r->connected ) continue;
            if ( r->kind == REGION_KIND_MEMEXT_RAM )   type_avail[0] = true;
            if ( r->kind == REGION_KIND_MEMEXT_FLASH ) type_avail[1] = true;
        }
        int cur_type = ( st->current_key.kind == REGION_KIND_MEMEXT_FLASH )
                       ? 1 : 0;
        const char *type_labels[2] = { "RAM", "FLASH" };

        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 70 );
        if ( ImGui::BeginCombo ( "##mb_memext_type", type_labels[cur_type] ) ) {
            for ( int t = 0; t < 2; t++ ) {
                if ( !type_avail[t] ) continue;
                bool sel = ( t == cur_type );
                if ( ImGui::Selectable ( type_labels[t], sel ) ) {
                    /* Změna typu: vyber první bank dostupného nového typu. */
                    int wanted_kind = ( t == 1 ) ? REGION_KIND_MEMEXT_FLASH
                                                 : REGION_KIND_MEMEXT_RAM;
                    for ( int i = 0; i < snap->count; i++ ) {
                        const st_REGION_DESC *r = &snap->items[i];
                        if ( !r->connected ) continue;
                        if ( ( int ) r->kind == wanted_kind ) {
                            st->current_region_id = r->id;
                            st->current_key.kind  = wanted_kind;
                            st->current_key.sub_id = r->sub_id;
                            st->cursor_addr = 0;
                            st->scroll_top_addr = 0;
                            st->edit_nibble = 0;
                            membrowser_state_save_to_persisted ( st );
                            break;
                        }
                    }
                }
                if ( sel ) ImGui::SetItemDefaultFocus ( );
            }
            ImGui::EndCombo ( );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Memext memory type" ) );
        }

        /* Bank combo: dostupné banky pro aktuální typ. */
        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _( "Bank:" ) );
        ImGui::SameLine ( );
        char bank_label[16];
        std::snprintf ( bank_label, sizeof ( bank_label ), "0x%02X",
                        st->current_key.sub_id );
        ImGui::SetNextItemWidth ( 80 );
        if ( ImGui::BeginCombo ( "##mb_memext_bank", bank_label ) ) {
            for ( int i = 0; i < snap->count; i++ ) {
                const st_REGION_DESC *r = &snap->items[i];
                if ( !r->connected ) continue;
                if ( ( int ) r->kind != st->current_key.kind ) continue;
                char opt[16];
                std::snprintf ( opt, sizeof ( opt ), "0x%02X", r->sub_id );
                bool sel = ( r->sub_id == st->current_key.sub_id );
                ImGui::PushID ( r->sub_id );
                if ( ImGui::Selectable ( opt, sel ) ) {
                    st->current_region_id = r->id;
                    st->current_key.sub_id = r->sub_id;
                    st->cursor_addr = 0;
                    st->scroll_top_addr = 0;
                    st->edit_nibble = 0;
                    membrowser_state_save_to_persisted ( st );
                }
                if ( sel ) ImGui::SetItemDefaultFocus ( );
                ImGui::PopID ( );
            }
            ImGui::EndCombo ( );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Memext bank index" ) );
        }
    }

    /* --- Sub-control pro Ramdisk STD (Bank). --- */
    if ( cur_group == MB_RG_RAMDISK_STD ) {
        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _( "Bank:" ) );
        ImGui::SameLine ( );
        char bank_label[16];
        std::snprintf ( bank_label, sizeof ( bank_label ), "0x%02X",
                        st->current_key.sub_id );
        ImGui::SetNextItemWidth ( 80 );
        if ( ImGui::BeginCombo ( "##mb_ramdisk_bank", bank_label ) ) {
            for ( int i = 0; i < snap->count; i++ ) {
                const st_REGION_DESC *r = &snap->items[i];
                if ( !r->connected ) continue;
                if ( r->kind != REGION_KIND_RAMDISK_STD ) continue;
                char opt[16];
                std::snprintf ( opt, sizeof ( opt ), "0x%02X", r->sub_id );
                bool sel = ( r->sub_id == st->current_key.sub_id );
                ImGui::PushID ( r->sub_id );
                if ( ImGui::Selectable ( opt, sel ) ) {
                    st->current_region_id = r->id;
                    st->current_key.sub_id = r->sub_id;
                    st->cursor_addr = 0;
                    st->scroll_top_addr = 0;
                    st->edit_nibble = 0;
                    membrowser_state_save_to_persisted ( st );
                }
                if ( sel ) ImGui::SetItemDefaultFocus ( );
                ImGui::PopID ( );
            }
            ImGui::EndCombo ( );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Ramdisk STD bank index" ) );
        }
    }

    /* --- Sub-control pro Ramdisk PEZIK (port 0x68 / 0xE8) - Bank. ---
     *
     * sub_id 0..7 = port 0x68, 8..15 = port 0xE8 (per dbgapi konvence).
     * Filtrujeme jen banky patřící do aktuální PEZIK instance. */
    if ( cur_group == MB_RG_RAMDISK_PEZIK_68
         || cur_group == MB_RG_RAMDISK_PEZIK_E8 ) {
        bool is_e8 = ( cur_group == MB_RG_RAMDISK_PEZIK_E8 );
        int  sub_min = is_e8 ? 8 : 0;
        int  sub_max = is_e8 ? 15 : 7;
        int  display_bank = st->current_key.sub_id - sub_min;

        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _( "Bank:" ) );
        ImGui::SameLine ( );
        char bank_label[16];
        std::snprintf ( bank_label, sizeof ( bank_label ), "0x%02X",
                        display_bank );
        ImGui::SetNextItemWidth ( 80 );
        if ( ImGui::BeginCombo ( "##mb_pezik_bank", bank_label ) ) {
            for ( int i = 0; i < snap->count; i++ ) {
                const st_REGION_DESC *r = &snap->items[i];
                if ( !r->connected ) continue;
                if ( r->kind != REGION_KIND_RAMDISK_PEZIK ) continue;
                if ( r->sub_id < sub_min || r->sub_id > sub_max ) continue;
                int local_bank = r->sub_id - sub_min;
                char opt[16];
                std::snprintf ( opt, sizeof ( opt ), "0x%02X", local_bank );
                bool sel = ( r->sub_id == st->current_key.sub_id );
                ImGui::PushID ( r->sub_id );
                if ( ImGui::Selectable ( opt, sel ) ) {
                    st->current_region_id = r->id;
                    st->current_key.sub_id = r->sub_id;
                    st->cursor_addr = 0;
                    st->scroll_top_addr = 0;
                    st->edit_nibble = 0;
                    membrowser_state_save_to_persisted ( st );
                }
                if ( sel ) ImGui::SetItemDefaultFocus ( );
                ImGui::PopID ( );
            }
            ImGui::EndCombo ( );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                is_e8 ? _( "Ramdisk PEZIK port 0xE8 bank index" )
                      : _( "Ramdisk PEZIK port 0x68 bank index" ) );
        }
    }
}


/* Forward declarace - paging inline volaný na konci toolbar_row3. */
static void render_paging_inline ( MembrowserView *v );


/* Toolbar row 2 - view options (Encoding/Bytes-per-row/ASCII toggle).
 * Per Michalova úprava (V0-polish-6) navigace (HEX/DEC/Goto/Search/Page)
 * přesunuta na samostatný Row 3 - Row 2 zůstal jen pro "jak zobrazujeme"
 * (encoding, šířka, ASCII column).
 *
 *   Encoding: ▼ | Bytes/row: ▼ | [x] ASCII
 */
static void render_toolbar_row2 ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;

    /* --- Encoding combo. --- */
    ImGui::TextUnformatted ( _( "Encoding:" ) );
    ImGui::SameLine ( );

    ImGui::SetNextItemWidth ( 220 );
    if ( ImGui::BeginCombo ( "##mb_enc_combo",
                              membrowser_encoding_label ( st->current_encoding ) ) ) {
        for ( int i = 0; i < ( int ) MEMBROWSER_ENC__COUNT; i++ ) {
            bool sel = ( st->current_encoding == i );
            if ( ImGui::Selectable ( membrowser_encoding_label ( i ), sel ) ) {
                st->current_encoding = i;
                st->edit_nibble = 0;
                membrowser_state_save_to_persisted ( st );
                /* V0-leftovers F5: encoding change → cache flush
                 * (per brief explicit invalidation point - jistota že
                 * uživatel uvidí fresh decode i v případě podivných
                 * race conditions). */
                membrowser_io_cache_invalidate_all ( );
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "ASCII column encoding" ) );
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- Bytes/row dropdown. --- */
    ImGui::TextUnformatted ( _( "Bytes/row:" ) );
    ImGui::SameLine ( );
    char bpr_label[8];
    std::snprintf ( bpr_label, sizeof ( bpr_label ), "%d", st->bytes_per_row );
    /* Bugfix-final Bug 3b: combo width 60 -> 80 aby se hodnota "32"
     * vešla vedle arrow dropdownu. Při 60 ImGui ořezával zobrazenou
     * hodnotu na "1" (zbytek byte glyphu skryt za arrow). */
    ImGui::SetNextItemWidth ( 80 );
    if ( ImGui::BeginCombo ( "##mb_bpr_combo", bpr_label ) ) {
        const int options[3] = { 8, 16, 32 };
        for ( int i = 0; i < 3; i++ ) {
            char opt[8];
            std::snprintf ( opt, sizeof ( opt ), "%d", options[i] );
            bool sel = ( st->bytes_per_row == options[i] );
            if ( ImGui::Selectable ( opt, sel ) ) {
                st->bytes_per_row = options[i];
                st->edit_nibble = 0;
                membrowser_state_save_to_persisted ( st );
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Bytes per row (8/16/32)" ) );
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- ASCII column toggle. --- */
    if ( ImGui::Checkbox ( _L ( "ASCII###mb_ascii_toggle" ),
                            &st->ascii_column_visible ) ) {
        membrowser_state_save_to_persisted ( st );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Show or hide ASCII decoded column" ) );
    }

    /* V0-leftovers F3: PC/SP marker checkboxy - jen pro Logical region.
     * Pro fyzické/banked regiony PC/SP nemají smysl, takže checkboxy
     * skryjeme (UI clutter). */
    if ( st->current_key.kind == REGION_KIND_LOGICAL ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|" );
        ImGui::SameLine ( );
        if ( ImGui::Checkbox ( _L ( "PC###mb_pc_marker" ), &st->show_pc_marker ) ) {
            membrowser_state_save_to_persisted ( st );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Highlight row containing Z80 PC" ) );
        }
        ImGui::SameLine ( );
        if ( ImGui::Checkbox ( _L ( "SP###mb_sp_marker" ), &st->show_sp_marker ) ) {
            membrowser_state_save_to_persisted ( st );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Highlight row containing Z80 SP" ) );
        }

        /* V2: Origin labels - per-row label fyzického původu (4 KB stránka). */
        ImGui::SameLine ( );
        if ( ImGui::Checkbox ( _L ( "Origin###mb_origin_labels" ),
                                &st->show_origin_labels ) ) {
            membrowser_state_save_to_persisted ( st );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Show per-row physical origin label (banking-aware)" ) );
        }
    }
}


/* Toolbar row 3 - navigace (Goto HEX/DEC + Search + paging inline).
 * Per Michalova úprava (V0-polish-6) přesunuto z Row 2 na samostatný
 * třetí řádek pro vizuální oddělení view options (Row 2) od navigace
 * a vyhledávání (Row 3).
 *
 *   HEX: ____  DEC: ____  [Goto] | [Search] | [<] [>] Page: N of: M | 0xHHHH
 */
static void render_toolbar_row3 ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    uint64_t total = find_current_region_size ( v );

    /* --- Goto HEX input. --- */
    ImGui::TextUnformatted ( _( "HEX:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 80 );
    bool do_goto = false;
    if ( ImGui::InputText ( "##mb_goto_hex", st->goto_input,
                            sizeof ( st->goto_input ),
                            ImGuiInputTextFlags_EnterReturnsTrue
                                | ImGuiInputTextFlags_CharsHexadecimal ) ) {
        do_goto = true;
    }
    /* V1-polish-4: zaregistruj ID toolbar InputText pro hex view ASCII
     * dispatch (musí preferovat náš input pokud má focus, jinak by user
     * současně psal do Goto a do hex bytea). */
    membrowser_hexview_register_toolbar_input ( ImGui::GetItemID ( ) );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Hex address (Enter or Goto)" ) );
    }

    ImGui::SameLine ( );

    /* --- Goto DEC input (zrcadlí stejný buffer, ale jako desítkový). */
    ImGui::TextUnformatted ( _( "DEC:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 80 );
    char dec_buf[16];
    unsigned cur_hex = 0;
    std::sscanf ( st->goto_input, "%x", &cur_hex );
    std::snprintf ( dec_buf, sizeof ( dec_buf ), "%u", cur_hex );
    if ( ImGui::InputText ( "##mb_goto_dec", dec_buf, sizeof ( dec_buf ),
                            ImGuiInputTextFlags_EnterReturnsTrue
                                | ImGuiInputTextFlags_CharsDecimal ) ) {
        unsigned d = 0;
        if ( std::sscanf ( dec_buf, "%u", &d ) == 1 ) {
            std::snprintf ( st->goto_input, sizeof ( st->goto_input ),
                            "%X", d );
            do_goto = true;
        }
    }
    membrowser_hexview_register_toolbar_input ( ImGui::GetItemID ( ) );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Decimal address (Enter or Goto)" ) );
    }

    ImGui::SameLine ( );

    /* --- Goto tlačítko (alternativa k Enter). --- */
    if ( ImGui::Button ( _L ( "Goto###mb_goto_btn" ) ) ) {
        do_goto = true;
    }

    if ( do_goto && total > 0 ) {
        unsigned addr = 0;
        if ( std::sscanf ( st->goto_input, "%x", &addr ) == 1
                && ( uint64_t ) addr < total ) {
            st->cursor_addr = addr;
            st->edit_nibble = 0;
            membrowser_state_save_to_persisted ( st );
        }
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    /* --- Search panel toggle. --- */
    if ( ImGui::Button ( _L ( "Search###mb_search_btn" ) ) ) {
        st->search_panel_open = !st->search_panel_open;
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s (Ctrl+F)", _( "Toggle search panel" ) );
    }

    /* Paging na stejném řádku - per stará GTK reference paging za Search
     * v navigačním Row 3. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    render_paging_inline ( v );
}


/* Region status inline - vykreslí "Size: N B | R/W | mapped | Z80 base 0xHHHH"
 * na jeden řádek pomocí SameLine bez trailing newline (aby na něj šlo
 * navázat cursor info). Warningy (disconnect / KOI8-CS missing) se
 * vykreslí pod hlavním řádkem barevně.
 *
 * Bottom bar je per stará GTK reference jen 1 řádek (Size...|Cursor...),
 * warningy přicházejí jen výjimečně. */
static void render_region_status_inline ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;

    const st_REGION_DESC *r = nullptr;
    for ( int i = 0; i < v->regions.count; i++ ) {
        if ( v->regions.items[i].id == st->current_region_id ) {
            r = &v->regions.items[i];
            break;
        }
    }

    if ( r ) {
        const char *rw = r->writable ? _( "R/W" ) : _( "R/O" );
        const char *mapped = r->mapped_now ? _( "mapped" ) : _( "not mapped" );
        if ( r->logical_base != 0xFFFFFFFF ) {
            ImGui::Text ( _( "Size: %u B | %s | %s | Z80 base 0x%04X" ),
                          ( unsigned ) r->size, rw, mapped,
                          ( unsigned ) r->logical_base );
        } else {
            ImGui::Text ( _( "Size: %u B | %s | %s" ),
                          ( unsigned ) r->size, rw, mapped );
        }
    } else {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ), "%s",
                              _( "Region disconnected - falling back to Logical" ) );
    }
}


/* V2: banking indikátory řádek (PROHIBITED / SCRW640 / DMD / exVRAM).
 *
 * Vykreslí se jako extra řádek pod hlavním "Size: ... | R/W | mapped | Z80
 * base" řádkem, pouze pokud je co zobrazit (= aspoň jeden flag aktivní).
 * Default-default banking (např. čistý emu start) = nic se nezobrazí (=
 * jeden řádek status baru, jak bylo dosud).
 *
 * Per-arch:
 *   - MZ-800: PROHIBITED, SCRW640, DMD mode (700/800), exVRAM (info)
 *   - MZ-700: PROHIBITED
 *   - MZ-1500: žádné MZ-800 banking koncepty (PROHIBITED není MZ-1500
 *     věc, SCRW640/DMD jen MZ-800)
 *
 * Barvy:
 *   - PROHIBITED: oranžová/červená (= "neobvyklé, pozor")
 *   - SCRW640:    žlutá (= "nestandardní mód, info")
 *   - DMD 700:    tlumeně zelená (= "klasický info indikátor")
 *   - exVRAM:     muted (= jen info kde to v default emu vždy je)
 */
static bool render_banking_indicators_if_any ( void )
{
    bool printed_any = false;

#if MZARCH == 800
    /* MZ-800 PROHIBITED detection. */
    bool prh = MEMORY_MZ800_MAP_TEST_PROHIBITED ? true : false;
    bool mz700_mode = GDG_DMD_TEST_MODE700 ? true : false;
    bool scrw640 = GDG_MZ800_DMD_TEST_SCRW640 ? true : false;
    bool hicolor = GDG_MZ800_DMD_TEST_HICOLOR ? true : false;
    bool vbank = GDG_MZ800_DMD_TEST_VBANK ? true : false;

    if ( prh ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.45f, 0.30f, 1.0f ),
                              "%s", _( "[PROHIBITED]" ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Prohibited mode active (OUT E5) - 0xE000-0xFFFF reads return shadow 0x1A" ) );
        }
        printed_any = true;
    }
    if ( scrw640 ) {
        if ( printed_any ) { ImGui::SameLine ( ); ImGui::TextDisabled ( "|" ); ImGui::SameLine ( ); }
        ImGui::TextColored ( ImVec4 ( 0.95f, 0.85f, 0.35f, 1.0f ),
                              "%s", _( "[SCRW640]" ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "640px wide screen mode active (DMD bit 2) - VRAM II mapped to 0xA000-0xBFFF" ) );
        }
        printed_any = true;
    }
    if ( hicolor ) {
        if ( printed_any ) { ImGui::SameLine ( ); ImGui::TextDisabled ( "|" ); ImGui::SameLine ( ); }
        ImGui::TextColored ( ImVec4 ( 0.95f, 0.85f, 0.35f, 1.0f ),
                              "%s", _( "[HICOLOR]" ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "16-color mode active (DMD bit 1) - 4-plane VRAM addressing" ) );
        }
        printed_any = true;
    }
    if ( vbank ) {
        if ( printed_any ) { ImGui::SameLine ( ); ImGui::TextDisabled ( "|" ); ImGui::SameLine ( ); }
        ImGui::TextColored ( ImVec4 ( 0.55f, 0.85f, 0.55f, 1.0f ),
                              "%s", _( "[VBANK]" ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Display bank toggled (DMD bit 5)" ) );
        }
        printed_any = true;
    }

    /* DMD mode indikátor - vždy ukázat (= klíčový stav). */
    if ( printed_any ) { ImGui::SameLine ( ); ImGui::TextDisabled ( "|" ); ImGui::SameLine ( ); }
    const char *mode_str = mz700_mode ? _( "MZ-700 mode" ) : _( "MZ-800 mode" );
    ImGui::TextColored ( ImVec4 ( 0.65f, 0.80f, 1.0f, 1.0f ),
                          _( "DMD: %s (0x%02X)" ), mode_str,
                          ( unsigned ) ( g_gdg.regDMD & 0x0F ) );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Display Mode Descriptor - GDG register, bit 3 = MZ-700 compat mode" ) );
    }
    printed_any = true;

#elif MZARCH == 700
    bool prh = MEMORY_MZ700_MAP_TEST_PROHIBITED ? true : false;
    if ( prh ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.45f, 0.30f, 1.0f ),
                              "%s", _( "[PROHIBITED]" ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Prohibited mode active (OUT E5) - 0xE000-0xFFFF reads return shadow 0xFF" ) );
        }
        printed_any = true;
    }
#elif MZARCH == 1500
    /* MZ-1500 nemá PROHIBITED/SCRW640/DMD koncepty - jen banking přes
     * SPEC port (0xD000-0xEFFF dispatch). Žádný status indikátor v V2. */
    ( void ) 0;
#endif

    /* Pokud nic, žádný extra řádek se nevykresluje (= žádný NewLine). */
    return printed_any;
}


/* KOI8-CS warning - jen pokud aktivní KOI8-CS encoding A tabulka chybí.
 * Vykresluje se jako extra řádek pod hlavním status řádkem, aby
 * uživatele upozornil na fallback. Bez warningu zůstává header 3-řádkový.
 *
 * V1.5+: edit error toast (round-trip selhání ASCII edit / write_bytes
 * selhání) je nyní renderovaný right-aligned na řádku 2 (v render_bottom_bar
 * vedle fill_status), aby se nelaufoval na 3. řádek mimo bottom_h reservaci. */
static void render_warnings_if_any ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    /* V0-leftovers F6: KOI8-CS warning - vždy pokud tabulka chybí
     * (= broken lib link nebo runtime sanity test selhal). Předtím se
     * warning ukazoval jen pokud aktivní encoding bylo KOI8-CS; nově
     * varujeme pořád, aby uživatel věděl že KOI8-CS dropdown položka
     * je nepoužitelná i kdyby si ji vybral. */
    if ( membrowser_encoding_koi8cs_table_missing ( ) ) {
        const char *suffix = ( st->current_encoding == MB_CHARSET_KOI8CS )
                              ? _( " (current encoding affected)" )
                              : "";
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.7f, 0.3f, 1.0f ),
                              _( "KOI8-CS table not loaded - showing identity mapping%s" ),
                              suffix );
    }
}


/* Paging inline (volaný z toolbar row2 jako pokračování za Search).
 * Per stará GTK reference - 3-řádkový header celkem: Row1 toolbar,
 * Row2 goto+search+paging, status 1 řádek dole.
 *
 * Stránka má pevnou velikost 16 řádků = 16 * bytes_per_row bajtů.
 * Pro 64 KB region při 32 B/row to dává 128 stránek. Prev/Next krok =
 * 1 stránka = stejný posun jako PgUp/PgDn klávesy. */
static void render_paging_inline ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;

    /* Najdi total velikost regionu pro výpočet počtu stránek. */
    uint64_t total = 0;
    for ( int i = 0; i < v->regions.count; i++ ) {
        if ( v->regions.items[i].id == st->current_region_id ) {
            total = v->regions.items[i].size;
            break;
        }
    }
    if ( total == 0 ) return;

    int bpr = st->bytes_per_row;
    if ( bpr != 8 && bpr != 16 && bpr != 32 ) bpr = 32;

    /* Stránka = 16 řádků (jako PgUp/PgDn step). */
    uint64_t page_bytes = ( uint64_t ) bpr * 16ULL;
    if ( page_bytes == 0 ) page_bytes = 512ULL;
    uint64_t total_pages = ( total + page_bytes - 1ULL ) / page_bytes;
    uint64_t cur_page = ( uint64_t ) st->cursor_addr / page_bytes;

    /* Adaptivní šířka adresy pro displej cursor offset. */
    int addr_digits = 4;
    if ( total > 0x10000ULL ) addr_digits = 6;
    if ( total > 0x1000000ULL ) addr_digits = 8;

    /* Prev tlačítko - vrátí na začátek předchozí stránky. */
    bool dis_prev = ( cur_page == 0 );
    if ( dis_prev ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L ( "<##mb_page_prev" ) ) ) {
        if ( cur_page > 0 ) {
            uint64_t new_addr = ( cur_page - 1ULL ) * page_bytes;
            if ( new_addr >= total ) new_addr = total - 1ULL;
            st->cursor_addr = ( uint32_t ) new_addr;
            membrowser_state_save_to_persisted ( st );
        }
    }
    if ( dis_prev ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Previous page (PgUp)" ) );
    }
    ImGui::SameLine ( );

    /* Next tlačítko. */
    bool dis_next = ( cur_page + 1ULL >= total_pages );
    if ( dis_next ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L ( ">##mb_page_next" ) ) ) {
        if ( cur_page + 1ULL < total_pages ) {
            uint64_t new_addr = ( cur_page + 1ULL ) * page_bytes;
            if ( new_addr >= total ) new_addr = total - 1ULL;
            st->cursor_addr = ( uint32_t ) new_addr;
            membrowser_state_save_to_persisted ( st );
        }
    }
    if ( dis_next ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Next page (PgDn)" ) );
    }
    ImGui::SameLine ( );

    /* "Page: N of: M | 0xHHHH" - displej čísel stran (1-based) + addr. */
    ImGui::Text ( _( "Page: %llu of: %llu" ),
                  ( unsigned long long ) ( cur_page + 1ULL ),
                  ( unsigned long long ) total_pages );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "0x%0*X",
                  addr_digits, ( unsigned ) st->cursor_addr );
}


/* ---- V4 Search panel - inline pod toolbar ------------------------------ */


/**
 * @brief Krátké statické labely pro search type combo.
 *
 * Pole indexované přes en_MEMBROWSER_SEARCH_TYPE; obsah musí korespondovat
 * s aktuálním enum (V4 = 7 typů). _() pro lokalizaci v UI vrstvě.
 */
static const char *search_type_label ( int t )
{
    switch ( t ) {
    case MEMBROWSER_SEARCH_BYTES:      return _( "Bytes (HEX + ASCII)" );
    case MEMBROWSER_SEARCH_BYTE_SEQ:   return _( "Byte sequence (hex, legacy)" );
    case MEMBROWSER_SEARCH_ASCII:      return _( "ASCII (current encoding, legacy)" );
    case MEMBROWSER_SEARCH_WORD_LE:    return _( "Word LE (16-bit)" );
    case MEMBROWSER_SEARCH_WORD_BE:    return _( "Word BE (16-bit)" );
    case MEMBROWSER_SEARCH_MASKED:     return _( "Masked (AA ?? BB)" );
    case MEMBROWSER_SEARCH_REGEX:      return _( "Regex (POSIX ERE)" );
    case MEMBROWSER_SEARCH_EXPRESSION: return _( "Expression (per byte)" );
    default:                           return "?";
    }
}


/**
 * @brief Labely pro scope dropdown.
 */
static const char *search_scope_label ( int s )
{
    switch ( s ) {
    case MEMBROWSER_SCOPE_CURRENT:     return _( "Current region" );
    case MEMBROWSER_SCOPE_ALL_REGIONS: return _( "All regions (current arch)" );
    default:                           return "?";
    }
}


/**
 * @brief Pomocná konverze 1 bajtu -> 2 hex znaky (pro preview).
 */
static void byte_to_hex2 ( uint8_t b, char *out )
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = digits[( b >> 4 ) & 0x0F];
    out[1] = digits[b & 0x0F];
}


/**
 * @brief Spustí new search - validuje + push do history + delegate na engine.
 */
static void start_new_search ( MembrowserView *v, int mode )
{
    st_MEMBROWSER_STATE *st = &v->state;
    /* Push do history (i pri prazdnem patternu engine sám vrátí error). */
    membrowser_search_history_push ( st, st->search_pattern, st->search_type );
    /* Engine setup + první step nedělá tady - step se volá per frame v
     * membrowser_view_render. */
    membrowser_search_start ( st, &v->regions, mode );
    membrowser_state_save_to_persisted ( st );
}


/**
 * @brief Render výsledkového panelu (mode ALL nebo zbytek po DONE).
 */
static void render_results_panel ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    if ( !st->search_results_visible || st->search_results_count == 0 ) return;

    ImGui::Spacing ( );
    if ( !ImGui::CollapsingHeader ( _L ( "Results###mb_search_results_hdr" ),
                                     ImGuiTreeNodeFlags_DefaultOpen ) ) {
        return;
    }

    ImGui::Text ( _( "Hits: %d" ), st->search_results_count );
    ImGui::SameLine ( );
    if ( ImGui::SmallButton ( _L ( "Clear###mb_search_results_clear" ) ) ) {
        st->search_results_count = 0;
        st->search_results_visible = 0;
        return;
    }

    /* Compact tabulka 3 sloupců: Addr | Region | Preview hex. */
    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                             | ImGuiTableFlags_RowBg
                             | ImGuiTableFlags_ScrollY
                             | ImGuiTableFlags_SizingFixedFit;
    /* Limit výšky tabulky. */
    float row_h = ImGui::GetTextLineHeightWithSpacing ( );
    float table_h = row_h * 8.0f + 4.0f;
    if ( ImGui::BeginTable ( "##mb_search_results_tbl", 3, flags,
                              ImVec2 ( 0.0f, table_h ) ) ) {
        ImGui::TableSetupColumn ( _( "Address" ), ImGuiTableColumnFlags_WidthFixed, 100.0f );
        ImGui::TableSetupColumn ( _( "Region" ), ImGuiTableColumnFlags_WidthFixed, 140.0f );
        ImGui::TableSetupColumn ( _( "Preview (hex)" ), ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableHeadersRow ( );

        for ( int i = 0; i < st->search_results_count; i++ ) {
            const st_MB_SEARCH_HIT *hit = &st->search_results[i];
            ImGui::TableNextRow ( );
            ImGui::TableSetColumnIndex ( 0 );
            /* 40 = "0x" + %08X (8) + "##mb_hit_" (9) + %d max 11 + \0 + rezerva. */
            char addr_buf[40];
            std::snprintf ( addr_buf, sizeof ( addr_buf ), "0x%08X##mb_hit_%d",
                            ( unsigned ) hit->addr, i );
            if ( ImGui::Selectable ( addr_buf, false,
                                      ImGuiSelectableFlags_SpanAllColumns ) ) {
                /* Click na řádek -> jump cursor + přepnout region pokud
                 * neodpovídá. */
                if ( hit->region_kind != st->current_key.kind
                     || hit->region_sub_id != st->current_key.sub_id ) {
                    st->current_key.kind = hit->region_kind;
                    st->current_key.sub_id = hit->region_sub_id;
                }
                st->cursor_addr = ( uint32_t ) hit->addr;
                st->last_match_addr = ( uint32_t ) hit->addr;
                st->last_match_valid = true;
                membrowser_state_save_to_persisted ( st );
            }

            ImGui::TableSetColumnIndex ( 1 );
            /* Lookup region name v aktuálním snapshotu. */
            const char *rn = "?";
            for ( int j = 0; j < v->regions.count; j++ ) {
                if ( v->regions.items[j].id == hit->region_id ) {
                    rn = v->regions.items[j].name;
                    break;
                }
            }
            ImGui::TextUnformatted ( rn );

            ImGui::TableSetColumnIndex ( 2 );
            /* Preview: 16 hex bajtů, oddělené mezerou. */
            char prev_buf[64];
            int n = hit->preview_len;
            if ( n > 16 ) n = 16;
            int off = 0;
            for ( int b = 0; b < n && off + 4 < ( int ) sizeof ( prev_buf ); b++ ) {
                byte_to_hex2 ( hit->preview[b], prev_buf + off );
                off += 2;
                if ( b + 1 < n ) prev_buf[off++] = ' ';
            }
            prev_buf[off] = '\0';
            ImGui::TextUnformatted ( prev_buf );
        }
        ImGui::EndTable ( );
    }
}


/**
 * @brief V4 search panel - rozšířený o type/scope/case + Find Next/Prev/All
 *        + progress bar + Cancel + history dropdown + results panel.
 *
 * Layout (2 řádky max):
 *   Row A: Search type ▼ | Pattern [_______________] [Find Next][Prev][All][Cancel][Close]
 *   Row B: Scope ▼ | [x] Case sens. | Status (progress bar / hit info / error)
 *   Row C (CollapsingHeader): Results table (jen mode ALL)
 */


/* V4.1+: Pattern Builder helpery pro dual HEX+ASCII synced fields.
 *
 * regen_ascii_from_hex: parsuje search_pattern hex string, per byte vola
 * membrowser_encoding_byte_to_utf8(b, encoding) a appende UTF-8 glyph do
 * out bufferu. Pro chars bez UTF-8 mapping (= 0 z encoding) appende ".".
 *
 * rebuild_hex_from_ascii: walkuje UTF-8 codepointy v ascii str, per codepoint
 * vola membrowser_encoding_utf8_to_byte(utf8, encoding) -> uint8_t. Skládá
 * byte buffer + format do hex stringu " %02X". Chars které utf8_to_byte
 * vrátí false silent skipuje (analogicky původnímu ASCII compile chování). */
static void render_search_pattern_regen_ascii (
        const char *hex_str, int encoding_id, char *out, size_t out_size )
{
    if ( !out || out_size == 0 ) return;
    out[0] = '\0';

    uint8_t bytes[MB_SEARCH_MAX_PATTERN_BYTES];
    int n = membrowser_search_decode_hex_bytes ( hex_str, bytes,
                                                   MB_SEARCH_MAX_PATTERN_BYTES );
    if ( n <= 0 ) return;

    size_t pos = 0;
    for ( int i = 0; i < n; i++ ) {
        const char *g = membrowser_encoding_byte_to_utf8 ( bytes[i], encoding_id );
        if ( !g || !g[0] ) g = ".";
        size_t glen = std::strlen ( g );
        if ( pos + glen + 1 > out_size ) break;
        std::memcpy ( out + pos, g, glen );
        pos += glen;
    }
    out[pos] = '\0';
}


/* Vrátí počet bajtů UTF-8 sekvence začínající @p first_byte (1..4). */
static int utf8_seq_len ( unsigned char first_byte )
{
    if ( first_byte < 0x80 ) return 1;
    if ( ( first_byte & 0xE0 ) == 0xC0 ) return 2;
    if ( ( first_byte & 0xF0 ) == 0xE0 ) return 3;
    if ( ( first_byte & 0xF8 ) == 0xF0 ) return 4;
    return 1;  /* invalid lead byte - advance 1, encoding_lib selže. */
}


static void render_search_pattern_rebuild_hex (
        const char *ascii_str, int encoding_id, char *out, size_t out_size )
{
    if ( !out || out_size == 0 ) return;
    out[0] = '\0';
    if ( !ascii_str ) return;

    uint8_t bytes[MB_SEARCH_MAX_PATTERN_BYTES];
    int n = 0;

    const char *p = ascii_str;
    while ( *p && n < MB_SEARCH_MAX_PATTERN_BYTES ) {
        int seq = utf8_seq_len ( ( unsigned char ) *p );
        char tmp[5] = { 0 };
        int copy = seq;
        if ( copy > 4 ) copy = 4;
        std::memcpy ( tmp, p, ( size_t ) copy );
        tmp[copy] = '\0';

        uint8_t b = 0;
        if ( membrowser_encoding_utf8_to_byte ( tmp, encoding_id, &b ) ) {
            bytes[n++] = b;
        }
        /* Silent skip chars co encoding nedokáže zakódovat - user vidí
         * v HEX fieldu co se reálně zachytilo. */
        p += seq;
    }

    membrowser_search_encode_hex_bytes ( bytes, n, out, out_size );
}


/* Char Inserter callback pro Pattern Builder - appende byte do hex string. */
static void render_search_pattern_byte_emit_cb ( uint8_t b, void *userdata )
{
    st_MEMBROWSER_STATE *st = ( st_MEMBROWSER_STATE * ) userdata;
    if ( !st ) return;
    size_t cur = std::strlen ( st->search_pattern );
    /* Need: " XX" (3 char) + NUL pokud cur > 0; "XX" + NUL pokud cur == 0.
     * Pokud nestačí, ignoruj (= max pattern reached). */
    size_t need = ( cur > 0 ) ? 4u : 3u;
    if ( cur + need > sizeof ( st->search_pattern ) ) return;
    const char *sep = ( cur > 0 ) ? " " : "";
    std::snprintf ( st->search_pattern + cur,
                     sizeof ( st->search_pattern ) - cur,
                     "%s%02X", sep, ( unsigned ) b );
}


static void render_search_panel ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    if ( !st->search_panel_open ) return;

    ImGui::Separator ( );

    /* ---- Row A: search type + pattern + buttons ---- */
    ImGui::TextUnformatted ( _( "Search:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 200 );
    if ( ImGui::BeginCombo ( "##mb_search_type",
                              search_type_label ( st->search_type ) ) ) {
        for ( int i = 0; i < ( int ) MEMBROWSER_SEARCH__COUNT; i++ ) {
            if ( ImGui::Selectable ( search_type_label ( i ),
                                      st->search_type == i ) ) {
                st->search_type = i;
                membrowser_state_save_to_persisted ( st );
            }
        }
        ImGui::EndCombo ( );
    }

    bool do_next = false;

    if ( st->search_type == MEMBROWSER_SEARCH_BYTES ) {
        /* V4.1+ Pattern Builder: dual HEX + ASCII synced fields + "[+]"
         * Insert button. Canonical storage = search_pattern[] hex string
         * "AA BB CC". ASCII field je per-frame scratch regenerovaný z
         * search_pattern přes current_encoding.
         *
         * Sync direction: jen jeden field může být focused najednou
         * (ImGui ActiveID). Když HEX focused, ASCII scratch se NEpřepisuje
         * (= user vidí to co píše). Když HEX není focused, scratch
         * regenerovaný z aktuálního search_pattern (= autoritativní zdroj).
         * Pokud user typuje v ASCII fieldu, edits se per-frame parsují
         * a aktualizují search_pattern - HEX field se pak per next frame
         * sám aktualizuje (= search_pattern změnil).
         *
         * IsItemEdited() detekuje keystroke-by-keystroke modification
         * (přesnější než IsItemDeactivatedAfterEdit který čeká na focus
         * loss). Enter v kterémkoliv fieldu triggrne do_next. */

        /* HEX field (= editujeme search_pattern přímo). */
        ImGui::SetNextItemWidth ( 200 );
        bool hex_enter = ImGui::InputText ( "##mb_search_hex",
                                              st->search_pattern,
                                              sizeof ( st->search_pattern ),
                                              ImGuiInputTextFlags_EnterReturnsTrue
                                                | ImGuiInputTextFlags_CharsHexadecimal
                                                | ImGuiInputTextFlags_CharsUppercase );
        membrowser_hexview_register_toolbar_input ( ImGui::GetItemID ( ) );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "HEX bytes: AA BB CC (spaces optional)" ) );
        }
        do_next = do_next || hex_enter;

        /* ASCII field - per-frame scratch. Regen z search_pattern jen pokud
         * sám není focused (= user nepíše do něj zrovna teď). */
        static char s_ascii_scratch[1024] = { 0 };
        static ImGuiID s_ascii_id = 0;
        bool ascii_active = ( s_ascii_id != 0
                              && ImGui::GetActiveID ( ) == s_ascii_id );
        if ( !ascii_active ) {
            render_search_pattern_regen_ascii ( st->search_pattern,
                                                  st->current_encoding,
                                                  s_ascii_scratch,
                                                  sizeof ( s_ascii_scratch ) );
        }

        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 200 );
        bool ascii_enter = ImGui::InputText ( "##mb_search_ascii",
                                                s_ascii_scratch,
                                                sizeof ( s_ascii_scratch ),
                                                ImGuiInputTextFlags_EnterReturnsTrue );
        s_ascii_id = ImGui::GetItemID ( );
        membrowser_hexview_register_toolbar_input ( s_ascii_id );
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "ASCII view (current encoding). Type to update HEX. Use [+] for special chars." ) );
        }
        if ( ImGui::IsItemEdited ( ) ) {
            render_search_pattern_rebuild_hex ( s_ascii_scratch,
                                                  st->current_encoding,
                                                  st->search_pattern,
                                                  sizeof ( st->search_pattern ) );
        }
        do_next = do_next || ascii_enter;

        /* "[+]" Insert character button - otevře Char Inserter v callback
         * mode; click cell appende byte do search_pattern přes callback. */
        ImGui::SameLine ( );
        if ( ImGui::SmallButton ( _L ( "[+]###mb_search_insert_char" ) ) ) {
            membrowser_char_inserter_open_for_callback (
                render_search_pattern_byte_emit_cb, st,
                _( "Search pattern" ) );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Insert special character via Char Inserter (palette of MZ/KOI8 glyphs)" ) );
        }
    } else {
        /* Legacy single-field input pro ostatní typy (byte_seq, ASCII,
         * word LE/BE, masked, regex, expression). Beze změny. */
        ImGui::SameLine ( );
        ImGui::SetNextItemWidth ( 240 );
        do_next = ImGui::InputText ( "##mb_search_pattern",
                                       st->search_pattern,
                                       sizeof ( st->search_pattern ),
                                       ImGuiInputTextFlags_EnterReturnsTrue );
        /* V1-polish-4: zaregistruj ID pro hex view ASCII dispatch skip check. */
        membrowser_hexview_register_toolbar_input ( ImGui::GetItemID ( ) );
    }

    /* History dropdown jako malé tlačítko vedle (in-memory recall). */
    ImGui::SameLine ( );
    if ( st->search_history_count > 0 ) {
        if ( ImGui::SmallButton ( _L ( "H###mb_search_hist_btn" ) ) ) {
            ImGui::OpenPopup ( "##mb_search_hist_popup" );
        }
        if ( ImGui::IsItemHovered ( ) ) {
            ImGui::SetTooltip ( "%s", _( "Recent patterns" ) );
        }
        if ( ImGui::BeginPopup ( "##mb_search_hist_popup" ) ) {
            for ( int i = 0; i < st->search_history_count; i++ ) {
                char item_buf[MB_SEARCH_PATTERN_SIZE + 32];
                std::snprintf ( item_buf, sizeof ( item_buf ),
                                "[%s] %s##mb_hist_%d",
                                search_type_label ( st->search_history_types[i] ),
                                st->search_history[i], i );
                if ( ImGui::Selectable ( item_buf ) ) {
                    std::strncpy ( st->search_pattern, st->search_history[i],
                                   sizeof ( st->search_pattern ) - 1 );
                    st->search_pattern[sizeof ( st->search_pattern ) - 1] = '\0';
                    st->search_type = st->search_history_types[i];
                    membrowser_state_save_to_persisted ( st );
                }
            }
            ImGui::EndPopup ( );
        }
    }

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Find Next###mb_search_next" ) ) ) do_next = true;
    ImGui::SameLine ( );
    bool do_prev = ImGui::Button ( _L ( "Prev###mb_search_prev" ) );
    ImGui::SameLine ( );
    bool do_all = ImGui::Button ( _L ( "All###mb_search_all" ) );
    ImGui::SameLine ( );

    bool engine_running =
        ( st->search_state == MEMBROWSER_SEARCH_STATE_RUNNING );
    if ( engine_running ) {
        if ( ImGui::Button ( _L ( "Cancel###mb_search_cancel" ) ) ) {
            membrowser_search_cancel ( st );
        }
        ImGui::SameLine ( );
    }
    if ( ImGui::Button ( _L ( "Close###mb_search_close" ) ) ) {
        st->search_panel_open = false;
    }

    /* ---- Row B: scope + case sens + status ---- */
    ImGui::TextUnformatted ( _( "Scope:" ) );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 220 );
    if ( ImGui::BeginCombo ( "##mb_search_scope",
                              search_scope_label ( st->search_scope ) ) ) {
        for ( int i = 0; i < ( int ) MEMBROWSER_SCOPE__COUNT; i++ ) {
            if ( ImGui::Selectable ( search_scope_label ( i ),
                                      st->search_scope == i ) ) {
                st->search_scope = i;
                membrowser_state_save_to_persisted ( st );
            }
        }
        ImGui::EndCombo ( );
    }
    ImGui::SameLine ( );
    if ( ImGui::Checkbox ( _L ( "Case sens.###mb_search_case" ),
                            &st->search_case_sensitive ) ) {
        membrowser_state_save_to_persisted ( st );
    }

    /* Status / progress bar / error label. */
    ImGui::SameLine ( );
    if ( st->search_state == MEMBROWSER_SEARCH_STATE_RUNNING ) {
        float frac = 0.0f;
        if ( st->search_total_bytes > 0 ) {
            frac = ( float ) st->search_total_scanned
                   / ( float ) st->search_total_bytes;
            if ( frac > 1.0f ) frac = 1.0f;
        }
        char overlay[48];
        std::snprintf ( overlay, sizeof ( overlay ), "%.0f%%",
                        ( double ) ( frac * 100.0f ) );
        ImGui::ProgressBar ( frac, ImVec2 ( 160.0f, 0.0f ), overlay );
    } else if ( st->search_state == MEMBROWSER_SEARCH_STATE_ERROR ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.4f, 0.4f, 1.0f ),
                              _( "Error: %s" ), st->search_error );
    } else if ( st->search_state == MEMBROWSER_SEARCH_STATE_CANCELED ) {
        ImGui::TextDisabled ( "%s", _( "Canceled" ) );
    } else if ( st->last_match_valid ) {
        ImGui::Text ( _( "Match @ 0x%08X" ), ( unsigned ) st->last_match_addr );
    }

    /* ---- Trigger actions ---- */
    if ( do_next ) start_new_search ( v, MEMBROWSER_SEARCH_MODE_NEXT );
    else if ( do_prev ) start_new_search ( v, MEMBROWSER_SEARCH_MODE_PREV );
    else if ( do_all ) start_new_search ( v, MEMBROWSER_SEARCH_MODE_ALL );

    /* ---- Results panel ---- */
    render_results_panel ( v );
}


/* Spodní status bar - 1 řádek (region info + cursor info inline).
 *
 * Per stará GTK reference 3-řádkový header total (2 toolbar + 1 status).
 * Předchozí varianta měla 2 status řádky (region zvlášť, cursor zvlášť);
 * teď sloučeno přes separator "  |  " do jediného Text bloku.
 *
 * Layout:
 *   Size: N B | R/W | mapped | Z80 base 0xHHHH  ||  Cursor: 0xAAAA = 0xBB 'c' (encoding)
 *
 * Warningy (region disconnected / KOI8-CS missing) se vykreslí jako extra
 * 2. řádek POUZE pokud nastanou - běžný stav má status 1 řádek. */
static void render_bottom_bar ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;

    /* Region část - vykreslí Size/RW/mapped/Z80 base na jeden řádek. */
    render_region_status_inline ( v );

    /* Separator + Cursor info na stejném řádku. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "  ||  " );
    ImGui::SameLine ( );

    /* Cursor info - byte pod cursorem + decode v aktivní encoding. */
    uint8_t byte_under = 0;
    bool ok = false;
    if ( st->current_region_id >= 0 ) {
        st_HEX_VIEW_BACKEND bb;
        membrowser_io_make_emu_backend ( &bb, st->current_region_id, false );
        int got = bb.read_bytes ( bb.ctx, ( uint64_t ) st->cursor_addr,
                                   &byte_under, 1 );
        ok = ( got == 1 );
    }

    if ( ok ) {
        const char *ascii = membrowser_encoding_byte_to_utf8 ( byte_under,
                                                                st->current_encoding );
        ImGui::Text ( _( "Cursor: 0x%08X = 0x%02X '%s' (%s)" ),
                      ( unsigned ) st->cursor_addr,
                      ( unsigned ) byte_under,
                      ascii,
                      membrowser_encoding_label ( st->current_encoding ) );
    } else {
        ImGui::Text ( _( "Cursor: 0x%08X (no data)" ), ( unsigned ) st->cursor_addr );
    }

    /* V1-polish-2 Bug 5: indikátor edit režimu a aktivního edit_mode
     * (HEX nibble vs ASCII char). Vykreslujeme jen pokud edit ON - v
     * browse režimu by jen plnil status bar. */
    if ( st->edit_enabled ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "  ||  " );
        ImGui::SameLine ( );
        const char *mode_str = ( st->edit_mode == MEMBROWSER_EDIT_ASCII )
                                ? _( "ASCII" ) : _( "HEX" );
        ImGui::TextColored ( ImVec4 ( 1.00f, 0.55f, 0.50f, 1.00f ),
                              _( "Mode: %s (Tab to switch)" ), mode_str );
    }

    /* V2: banking indikátory (PROHIBITED/SCRW640/HICOLOR/VBANK/DMD pro
     * MZ-800, PROHIBITED pro MZ-700). Vykresluje se jako řádek 2 pod
     * hlavním status řádkem JEN pokud arch má co zobrazit (na MZ-800
     * vždy aspoň DMD label, na MZ-700 jen pokud PROHIBITED, na MZ-1500
     * nikdy). Pomáhá při debug banking ladění. */
    bool banking_printed = render_banking_indicators_if_any ( );

    /* V1.5+: status / error toast right-aligned na řádek 2 (vedle banking).
     *
     * Dříve fill_status a edit_err padaly na řádek 3, který se nevešel do
     * bottom_h = 2*line_height (= visuální clip dolní hrany). Místo toho:
     *   - vlevo = banking indikátory (line 2 start)
     *   - separátor "  ||  " mezi nimi a pravou částí
     *   - vpravo = fill_status (priorita) NEBO edit_err, right-aligned
     *     k pravému okraji okna
     *
     * Pokud right_text overflows přes available width (banking je dlouhé +
     * error message je dlouhá), fallback: render za separator inline (bez
     * right-align), může se vizuálně rozšířit pod hex view ale aspoň se
     * neztratí. */
    const char *fill_status = membrowser_fill_dialog_last_status ( );
    const char *edit_err = membrowser_hexview_get_edit_error ( );

    char right_buf[512];
    const char *right_text = NULL;
    ImVec4 right_color;
    if ( fill_status && fill_status[0] ) {
        right_text = fill_status;
        right_color = ImVec4 ( 1.00f, 0.85f, 0.20f, 1.00f );
    } else if ( edit_err && edit_err[0] ) {
        std::snprintf ( right_buf, sizeof ( right_buf ),
                        "%s: %s", _( "Edit error" ), edit_err );
        right_text = right_buf;
        right_color = ImVec4 ( 1.0f, 0.5f, 0.3f, 1.0f );
    }

    if ( right_text ) {
        const char *separator = "  ||  ";
        float sep_w = ImGui::CalcTextSize ( separator ).x;
        float text_w = ImGui::CalcTextSize ( right_text ).x;
        float right_pad = 4.0f;
        float win_w = ImGui::GetWindowContentRegionMax ( ).x;

        if ( banking_printed ) {
            ImGui::SameLine ( );
            float cur_x = ImGui::GetCursorPosX ( );
            float target_x = win_w - text_w - right_pad;
            float min_x = cur_x + sep_w;
            if ( target_x >= min_x ) {
                /* Right-align: padding pos right + separator těsně před. */
                ImGui::SetCursorPosX ( target_x - sep_w );
                ImGui::TextDisabled ( "%s", separator );
                ImGui::SameLine ( );
                ImGui::TextColored ( right_color, "%s", right_text );
            } else {
                /* Nedostatek místa - render inline za banking + separator. */
                ImGui::TextDisabled ( "%s", separator );
                ImGui::SameLine ( );
                ImGui::TextColored ( right_color, "%s", right_text );
            }
        } else {
            /* Banking nic neprintlo - line 2 prázdné. Right-align přímo. */
            float target_x = win_w - text_w - right_pad;
            if ( target_x > 0 ) {
                ImGui::SetCursorPosX ( target_x );
            }
            ImGui::TextColored ( right_color, "%s", right_text );
        }
    }

    /* V2: PEZIK byte-order info indikátor (extra řádek, jen pokud PEZIK
     * region). Informativní display ze stávajícího g_ramdisk.pezik state. */
    if ( st->current_key.kind == REGION_KIND_RAMDISK_PEZIK ) {
        int instance = st->current_key.sub_id / 8;
        int bank = st->current_key.sub_id % 8;
        const char *port_str = ( instance == 0 ) ? "0x68" : "0xE8";
        ImGui::TextDisabled (
            _( "PEZIK port %s bank %d | byte order: native (TODO BE/LE toggle V3+)" ),
            port_str, bank );
    }

    /* V6: annotation indikátor pro byte pod kurzorem - extra řádek
     * s color swatch a textem. Pokud annotation neexistuje, no display. */
    const st_MB_ANNOTATION *cur_annot = membrowser_annotations_find (
        st->current_key.kind, st->current_key.sub_id, st->cursor_addr );
    if ( cur_annot ) {
        ImVec4 col;
        col.x = ( ( cur_annot->color_rgba >> 24 ) & 0xFF ) / 255.0f;
        col.y = ( ( cur_annot->color_rgba >> 16 ) & 0xFF ) / 255.0f;
        col.z = ( ( cur_annot->color_rgba >>  8 ) & 0xFF ) / 255.0f;
        col.w = ( ( cur_annot->color_rgba >>  0 ) & 0xFF ) / 255.0f;
        ImGui::ColorButton ( "##mb_annot_cur_swatch", col,
                              ImGuiColorEditFlags_NoTooltip,
                              ImVec2 ( 12, 12 ) );
        ImGui::SameLine ( );
        ImGui::TextWrapped ( _( "Annotation: %s" ), cur_annot->text );
    }

    /* Warningy (KOI8-CS missing) - extra řádek jen pokud aktivní. */
    render_warnings_if_any ( v );
}


/* ============================================================================
 * V2: 3-panel layout helpery (Regions sidebar | Hex | Layers panel).
 * ============================================================================ */


/* Render vertical splitter handle + drag tracking.
 *
 * Pattern shodný s existující Layers splitter implementací (V1-polish-1).
 * Vrátí delta_x (signed px) drag z aktuálního framu - 0 pokud bez drag.
 * Caller aplikuje delta na svou panel_width state proměnnou (+/- podle
 * orientace splitteru: pro levý sidebar drag doprava = roste, pro pravý
 * panel drag doprava = klesá pravý panel).
 *
 * Strana se zvýrazní hover/active barvou. SetMouseCursor ResizeEW při
 * hover/active.
 *
 * @param id_str   Unique ImGui ID (např. "##mb_split_regions").
 * @param width    Šířka handle v px (4..8 typicky).
 * @param height   Výška handle v px (typicky avail content height).
 * @return Delta_x v px (signed), 0 pokud bez interakce.
 */
static float render_vsplitter ( const char *id_str, float width, float height )
{
    ImVec2 p0 = ImGui::GetCursorScreenPos ( );
    ImGui::InvisibleButton ( id_str, ImVec2 ( width, height ) );
    bool hov = ImGui::IsItemHovered ( );
    bool act = ImGui::IsItemActive ( );
    if ( hov || act ) {
        ImGui::SetMouseCursor ( ImGuiMouseCursor_ResizeEW );
    }
    float delta = 0.0f;
    if ( act ) {
        delta = ImGui::GetIO ( ).MouseDelta.x;
    }
    ImU32 col = act
        ? ImGui::GetColorU32 ( ImGuiCol_SeparatorActive )
        : ( hov
              ? ImGui::GetColorU32 ( ImGuiCol_SeparatorHovered )
              : ImGui::GetColorU32 ( ImGuiCol_Separator ) );
    ImGui::GetWindowDrawList ( )->AddRectFilled (
        p0, ImVec2 ( p0.x + width, p0.y + height ), col );
    return delta;
}


/* Render Regions sidebar (levý kolaps panel) v aktuálním child slotu.
 * Width + splitter resize logic je shodný s Layers panel patternem.
 *
 * Vrací aktualizovanou šířku panelu (px) po případném drag - caller
 * uloží do state.regions_panel_width pokud změna.
 */
static int render_regions_sidebar_child ( MembrowserView *self,
                                           float regions_w, float avail )
{
    ImGui::BeginChild ( "##mb_regions_panel",
                        ImVec2 ( regions_w, avail ),
                        ImGuiChildFlags_Borders );
    membrowser_regions_render_tree ( &self->state, &self->regions );
    ImGui::EndChild ( );
    return ( int ) regions_w;
}


/* Render hlavní 3-panel layout: [Regions | Hex | Layers].
 *
 * Každý panel je volitelný (per state.regions_panel_open /
 * state.layers_panel_open). Mezi sousedními panely je 6 px vertical
 * splitter handle s drag-resize.
 *
 * Šířky panelů jsou perzistované (state.regions_panel_width /
 * layers_panel_width). 0 = auto-compute z labelů při prvním renderu.
 *
 * Hex view dostane zbylou šířku (avail - regions - layers - splittery).
 * Minimální hex width = 240 px (= 8 bajtů/řádek se vejde). Pokud okno
 * je úzké, panely se zmenší pod jejich min, ale hex dostane svůj min.
 */
static void render_main_layout ( MembrowserView *self,
                                  st_HEX_VIEW_BACKEND *be,
                                  const st_MEMBROWSER_PCSP_INFO *pcsp,
                                  const st_MEMBROWSER_HEXVIEW_EXTRAS *extras,
                                  float avail )
{
    st_MEMBROWSER_STATE *st = &self->state;

    const float splitter_w = 6.0f;
    const float min_regions = 180.0f;
    const float max_regions = 420.0f;
    const float min_layers = 200.0f;
    const float max_layers = 600.0f;
    const float min_hex = 240.0f;

    bool show_regions = st->regions_panel_open;
    bool show_layers = st->layers_panel_open;

    /* Auto-compute panel widths při sentinel 0 (= první otevření). */
    if ( show_regions && st->regions_panel_width <= 0 ) {
        st->regions_panel_width =
            membrowser_regions_compute_optimal_panel_width ( );
        membrowser_state_save_to_persisted ( st );
    }
    if ( show_layers && st->layers_panel_width <= 0 ) {
        st->layers_panel_width =
            membrowser_layers_compute_optimal_panel_width ( );
        membrowser_state_save_to_persisted ( st );
    }

    float avail_w = ImGui::GetContentRegionAvail ( ).x;

    /* Vypočti efektivní šířky panelů s clampingem (= aby hex měl aspoň
     * min_hex). Pokud okno úzké, oba panely se mohou squeezovat až na
     * jejich min. */
    float regions_w = show_regions ? ( float ) st->regions_panel_width : 0.0f;
    float layers_w = show_layers ? ( float ) st->layers_panel_width : 0.0f;

    /* Splitter overhead: 1 splitter na každý zobrazený panel. */
    int n_splitters = ( show_regions ? 1 : 0 ) + ( show_layers ? 1 : 0 );
    float split_total = n_splitters * splitter_w;

    /* Reserved budget pro hex view a splittery. Co zbude rozdělíme mezi
     * panely poměrně k jejich nastavené šířce. */
    float reserved_for_others = min_hex + split_total;
    float panels_budget = avail_w - reserved_for_others;
    if ( panels_budget < 0.0f ) panels_budget = 0.0f;

    /* Clamp regions_w a layers_w aby suma <= panels_budget. */
    float sum_panels = regions_w + layers_w;
    if ( sum_panels > panels_budget && sum_panels > 0.0f ) {
        float scale = panels_budget / sum_panels;
        regions_w *= scale;
        layers_w *= scale;
    }
    /* Per-panel min/max clamp. */
    if ( show_regions ) {
        if ( regions_w < min_regions ) regions_w = min_regions;
        if ( regions_w > max_regions ) regions_w = max_regions;
    }
    if ( show_layers ) {
        if ( layers_w < min_layers ) layers_w = min_layers;
        if ( layers_w > max_layers ) layers_w = max_layers;
    }

    float hex_w = avail_w - regions_w - layers_w - split_total;
    if ( hex_w < min_hex ) hex_w = min_hex;

    /* === Render Regions sidebar (left) + splitter === */
    if ( show_regions ) {
        render_regions_sidebar_child ( self, regions_w, avail );

        ImGui::SameLine ( 0.0f, 0.0f );
        float dx = render_vsplitter ( "##mb_split_regions", splitter_w, avail );
        if ( dx != 0.0f ) {
            /* Drag doprava (+dx) = sidebar roste (sedí vlevo). */
            int new_w = ( int ) ( ( float ) st->regions_panel_width + dx );
            if ( new_w < ( int ) min_regions ) new_w = ( int ) min_regions;
            if ( new_w > ( int ) max_regions ) new_w = ( int ) max_regions;
            if ( new_w != st->regions_panel_width ) {
                st->regions_panel_width = new_w;
                membrowser_state_save_to_persisted ( st );
            }
        }
        ImGui::SameLine ( 0.0f, 0.0f );
    }

    /* === Render Hex view (middle) === */
    ImGui::BeginChild ( "##mb_hex_main", ImVec2 ( hex_w, avail ),
                        ImGuiChildFlags_None );
    {
        membrowser_hexview_render_ex ( st, be,
                                        ImGui::GetContentRegionAvail ( ).y,
                                        pcsp, extras );
    }
    ImGui::EndChild ( );

    /* === Render Layers panel (right) + splitter === */
    if ( show_layers ) {
        ImGui::SameLine ( 0.0f, 0.0f );
        float dx = render_vsplitter ( "##mb_split_layers", splitter_w, avail );
        if ( dx != 0.0f ) {
            /* Drag doprava (+dx) = layers panel klesá (sedí vpravo,
             * splitter je mezi hex a layers). */
            int new_w = ( int ) ( ( float ) st->layers_panel_width - dx );
            if ( new_w < ( int ) min_layers ) new_w = ( int ) min_layers;
            if ( new_w > ( int ) max_layers ) new_w = ( int ) max_layers;
            if ( new_w != st->layers_panel_width ) {
                st->layers_panel_width = new_w;
                membrowser_state_save_to_persisted ( st );
            }
        }
        ImGui::SameLine ( 0.0f, 0.0f );

        ImGui::BeginChild ( "##mb_layers_panel", ImVec2 ( layers_w, avail ),
                            ImGuiChildFlags_Borders );
        membrowser_layers_render_panel ( st, st->current_region_id );
        ImGui::EndChild ( );
    }
}


/* Klávesová navigace (volat z okna, jen pokud má focus).
 *
 * Navigace funguje vždy nezávisle na edit režimu - šipky posouvají cursor.
 * Edit klávesy (0-9, A-F, ASCII chars) jsou dispatchované v
 * hexview_handle_edit_input. Tab přepíná HEX/ASCII mode (v edit dispatch),
 * Esc vypne Edit (= shortcut na Edit: OFF). Tady NEresíme edit data klávesy. */
static void handle_navigation_keys ( MembrowserView *v )
{
    st_MEMBROWSER_STATE *st = &v->state;
    if ( !ImGui::IsWindowFocused ( ImGuiFocusedFlags_RootAndChildWindows ) ) return;

    /* Najdi total size aktuálního regionu. */
    uint64_t total = 0;
    for ( int i = 0; i < v->regions.count; i++ ) {
        if ( v->regions.items[i].id == st->current_region_id ) {
            total = v->regions.items[i].size;
            break;
        }
    }
    if ( total == 0 ) return;

    int bpr = st->bytes_per_row;
    uint32_t cur = st->cursor_addr;

    if ( ImGui::IsKeyPressed ( ImGuiKey_LeftArrow ) && cur > 0 ) cur--;
    if ( ImGui::IsKeyPressed ( ImGuiKey_RightArrow ) && cur + 1 < total ) cur++;
    if ( ImGui::IsKeyPressed ( ImGuiKey_UpArrow ) ) {
        if ( cur >= ( uint32_t ) bpr ) cur -= bpr;
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_DownArrow ) ) {
        if ( cur + ( uint32_t ) bpr < total ) cur += bpr;
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_PageUp ) ) {
        uint32_t step = ( uint32_t ) bpr * 16;
        cur = ( cur > step ) ? cur - step : 0;
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_PageDown ) ) {
        uint32_t step = ( uint32_t ) bpr * 16;
        if ( ( uint64_t ) cur + step < total ) cur += step;
        else cur = ( uint32_t ) ( total - 1 );
    }
    if ( ImGui::IsKeyPressed ( ImGuiKey_Home ) ) cur = 0;
    if ( ImGui::IsKeyPressed ( ImGuiKey_End ) ) cur = ( uint32_t ) ( total - 1 );

    if ( cur != st->cursor_addr ) {
        st->cursor_addr = cur;
        /* V0-leftovers F2: reset hex edit_nibble při navigaci, jinak
         * by uživatel po šipce psal do low nibble nového bytu. */
        st->edit_nibble = 0;
        membrowser_state_save_to_persisted ( st );
    }

    /* Ctrl+F = open search panel. */
    if ( ImGui::IsKeyPressed ( ImGuiKey_F ) && ImGui::GetIO ( ).KeyCtrl ) {
        st->search_panel_open = true;
    }

    /* V4: F3 = Find Next, Shift+F3 = Find Prev. Dispatch jen pokud panel
     * je open (jinak je shortcut bez kontextu zavádějící). Engine sám
     * zvalidi prázdný pattern. */
    if ( st->search_panel_open && ImGui::IsKeyPressed ( ImGuiKey_F3 ) ) {
        int mode = ImGui::GetIO ( ).KeyShift
                       ? MEMBROWSER_SEARCH_MODE_PREV
                       : MEMBROWSER_SEARCH_MODE_NEXT;
        /* Push do history + start search. */
        membrowser_search_history_push ( st, st->search_pattern,
                                          st->search_type );
        membrowser_search_start ( st, &v->regions, mode );
        membrowser_state_save_to_persisted ( st );
    }

    /* V4: Esc = close panel (jen pokud běží idle / done, ne během RUNNING -
     * RUNNING má vlastní Cancel button + Esc by zruseni pojmenoval Cancel
     * jen pokud byl explicit click; držme jednoduché chování). */
    if ( st->search_panel_open && ImGui::IsKeyPressed ( ImGuiKey_Escape ) ) {
        if ( st->search_state == MEMBROWSER_SEARCH_STATE_RUNNING ) {
            membrowser_search_cancel ( st );
        } else {
            st->search_panel_open = false;
        }
    }
}


/* ---- Public API ------------------------------------------------------- */


/* Pomocný resolver: window_id string -> instance_idx (0..MB_INSTANCE_COUNT-1).
 * Vrací 0 (= MAIN) pro neznámé ID jako safety fallback. */
static int resolve_instance_idx_from_window_id ( const char *window_id )
{
    if ( !window_id ) return MB_INSTANCE_MAIN;
    for ( int i = 0; i < MB_INSTANCE_COUNT; i++ ) {
        if ( std::strcmp ( window_id, SLOT_WINDOW_ID[i] ) == 0 ) return i;
    }
    return MB_INSTANCE_MAIN;
}


extern "C" MembrowserView *membrowser_view_create ( const char *window_id )
{
    if ( !window_id ) window_id = "main";

    int idx = resolve_instance_idx_from_window_id ( window_id );

    MembrowserView *v = new ( std::nothrow ) MembrowserView ( );
    if ( !v ) return nullptr;

    v->instance_idx = idx;
    v->window_id = SLOT_WINDOW_ID[idx];  /* Lifetime-stable string. */

    /* ###StableID s window_id - 3 hashe per memory feedback_imgui_window_id_three_hashes.
     * Title prefix složený v render-time (kvůli _() lokalizaci), zde držíme
     * jen ###StableID suffix templátový string. Pro main: bez " #N" suffixu
     * (= backward-compat label "Memory Browser"); pro #2..#5: s " #N" suffixem. */
    if ( idx == MB_INSTANCE_MAIN ) {
        std::snprintf ( v->title_id, sizeof ( v->title_id ),
                        "Memory Browser###mb_%s", v->window_id );
    } else {
        std::snprintf ( v->title_id, sizeof ( v->title_id ),
                        "Memory Browser #%s###mb_%s", v->window_id, v->window_id );
    }

    /* Default state s konkrétním instance_idx. */
    v->state = membrowser_state_default ( idx );

    /* Aplikuj persisted hodnoty pro tuto instanci. */
    membrowser_state_apply_persisted ( &v->state );

    v->regions.count = 0;
    v->regions.items = nullptr;

    return v;
}


extern "C" void membrowser_view_destroy ( MembrowserView *self )
{
    if ( !self ) return;

    /* Save aktuální state do persist slotu této instance. */
    membrowser_state_save_to_persisted ( &self->state );

    delete self;
}


extern "C" void membrowser_view_render ( MembrowserView *self, bool *p_open )
{
    if ( !self ) return;

    /* Initial size 600x480 - preferovaná velikost podle uživatelského
     * testu (Snímek obrazovky 2026-05-25 120217.png). Stačí na 16
     * bajtů/řádek hex view + toolbar a bere v potaz typický 1080p
     * dual-window debugger layout. Aplikuje se jen ImGuiCond_FirstUseEver
     * = stávající uložené velikosti v ImGui.ini zůstávají respektovány. */
    /* Auto-layout pos + explicit SetNextWindowSize ImGuiCond_Once per instance:
     * FirstUseEver z neznámého důvodu (BeginChild parent? render path bug?)
     * neaplikuje 600x480 ani na fresh start bez ini. Once = aplikuje jen
     * první frame v session, pak ImGui drží user resize. Per-instance flag
     * v state struct (size_applied) zajistí ne-skip po close+reopen okna. */
    auto_layout_first_use_portrait ( self->title_id, 1260.0f, 900.0f );
    if ( !self->state.size_applied ) {
        ImGui::SetNextWindowSize ( ImVec2 ( 1260, 900 ), ImGuiCond_Always );
        self->state.size_applied = true;
    }
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 480, 280 ),
                                           ImVec2 ( FLT_MAX, FLT_MAX ) );

    /* Window title - title_id už má ###mb_<id> StableID suffix složený
     * v membrowser_view_create. _L() zde NESMÍ být - přidává "##Original"
     * suffix určený pro krátké gettext klíče, čímž by se ID stack zacyklil
     * (Begin pak hlásí Mismatching PushID/PopID + duplikovaný window ID).
     * Lokalizace label části "Memory Browser" je TODO V1+ - vyžaduje
     * skládat title až v render-time z přeloženého _() prefixu + ###. */
    if ( !ImGui::Begin ( self->title_id, p_open,
                          ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    }

    /* DEBUG: force size PO Begin (per-frame, ImGuiCond_Always). User
     * resize ztratí ale ověříme zda 1800x1100 vůbec dorazí. */
    if ( !self->state.size_applied ) {
        ImGui::SetWindowSize ( ImVec2 ( 1260, 900 ) );
        self->state.size_applied = true;
    }

    /* Refresh region snapshot per frame (banking změny mohou měnit ID). */
    membrowser_io_refresh_regions ( &self->regions );

    /* V1-polish-2: globální hex view shortcuts (F2/Esc/Tab) musí být
     * dispatchované na top-level mb okno scope, NE z hex view child
     * (child window může mít odlišný focus state, viz Doxygen v
     * membrowser_hexview_handle_global_shortcuts).
     *
     * find_current_region_writable potřebuje aktualizovaný current_region_id
     * (= po resolve níže). Pro F2 dispatch nás stačí vědět writable
     * AKTUÁLNÍHO regionu (= persisted current_key resolved v posledním
     * frame); volání před resolve znamená 1-frame delay po region switch,
     * což je akceptovatelné (region switch je explicit user akce, ne
     * tichý event). */
    {
        bool can_write = find_current_region_writable ( self );
        membrowser_hexview_handle_global_shortcuts ( &self->state, can_write );
    }

    /* Resolve current_region_id z persisted (kind, sub_id). */
    int new_id = membrowser_io_find_region ( &self->regions,
                                              self->state.current_key.kind,
                                              self->state.current_key.sub_id );
    if ( new_id < 0 ) {
        /* Region disconnected - fallback na LOGICAL. */
        new_id = membrowser_io_find_region ( &self->regions,
                                              REGION_KIND_LOGICAL, 0 );
        if ( new_id >= 0 ) {
            self->state.current_key.kind = REGION_KIND_LOGICAL;
            self->state.current_key.sub_id = 0;
        }
    }
    self->state.current_region_id = new_id;

    /* Edit semantics: detekce region/bank switche pro recently-edited clear.
     * Sentinel last_region_kind = -1 = ještě nebylo nic = první render
     * po create instance; jen zapamatovat bez clear.
     *
     * Bank switch (= sub_id změna při stejném kind, např. Memext bank
     * 0x05 → 0x06) také trigger clear - per schválená sémantika "nový
     * region/bank = nová session". Edit_enabled toggle zůstane zachován
     * (= user explicit volba). */
    if ( self->state.last_region_kind < 0 ) {
        /* První render - jen inicializovat last_region_key, žádný clear. */
        self->state.last_region_kind = self->state.current_key.kind;
        self->state.last_region_sub_id = self->state.current_key.sub_id;
    } else if ( self->state.last_region_kind != self->state.current_key.kind
                 || self->state.last_region_sub_id
                      != self->state.current_key.sub_id ) {
        /* Region nebo bank se změnil - clear recently-edited pro instanci.
         * Edit_enabled se nemění (uživatel ho explicit zapnul, nechceme
         * překvapit). */
        membrowser_edited_clear_for_instance ( self->state.instance_idx );
        self->state.last_region_kind = self->state.current_key.kind;
        self->state.last_region_sub_id = self->state.current_key.sub_id;
    }

    /* 3-řádkový toolbar (Row1 = file ops/edit/region, Row2 = view options
     * encoding/bytes-per-row/ASCII, Row3 = navigace goto/search/paging
     * inline) + volitelný search panel + hex view + 1-řádkový bottom status
     * bar. V0-polish-6: split z původního 2-řádkového toolbaru pro lepší
     * vizuální oddělení view options vs navigace. */
    render_toolbar_row1 ( self );
    render_toolbar_row2 ( self );
    render_toolbar_row3 ( self );

    /* V4 search engine: per-frame chunk step pokud běží hledání.
     * Budget 256 KB/frame = ~15 MB/s při 60 FPS, dostatečné pro 16 MB
     * Ramdisk během cca 1 sekundy. */
    if ( self->state.search_state == MEMBROWSER_SEARCH_STATE_RUNNING ) {
        membrowser_search_step ( &self->state, &self->regions,
                                   256u * 1024u );
    }

    render_search_panel ( self );

    /* Render file dialogy (Load BIN / Save BIN) - vždy aktivní, sami
     * si řídí viditelnost. */
    membrowser_fileio_render_dialogs ( );

    ImGui::Separator ( );

    /* Reserve space pro bottom bar (1 řádek: region status + cursor inline).
     * Per stará GTK reference 3-řádkový header total. Warningy (KOI8-CS
     * missing) případně zaberou extra řádek - akceptovatelné, jsou
     * výjimečné. */
    /* V2: bottom bar má 1 řádek (main status) + případně 1 řádek banking
     * indikátory + případně 1 řádek warningy. Pro MZ-800 ukazujeme DMD
     * vždy = 2 řádky baseline. Reserve 2.0× line + padding kompromis aby
     * hex view nezmizel přílis při výskytu warningu (warning je extra
     * 3. řádek - krátký okamžik). */
    float bottom_h = ImGui::GetTextLineHeightWithSpacing ( ) * 2.0f + 8.0f;
    float avail = ImGui::GetContentRegionAvail ( ).y - bottom_h;
    if ( avail < 80.0f ) avail = 80.0f;

    /* Hex view přes backend adapter. */
    if ( self->state.current_region_id >= 0 ) {
        /* Writable jen pokud region je R/W A user explicit zapnul Edit toggle
         * (default OFF, per stará GTK reference - safety před nechtěnou
         * modifikací paměti emulátoru). */
        bool hw_writable = find_current_region_writable ( self );
        bool effective_writable = hw_writable && self->state.edit_enabled;

        st_HEX_VIEW_BACKEND be;
        membrowser_io_make_emu_backend ( &be, self->state.current_region_id,
                                          effective_writable );

        /* PC/SP markers (V0-leftovers F3) - jen pro Logical Z80 region.
         * Ostatní regiony (RAM, ROM, VRAM, Memext, Ramdisk) PC/SP nemají
         * smysl - jsou to fyzické/banked adresy, ne Z80 logické. */
        st_MEMBROWSER_PCSP_INFO pcsp;
        pcsp.pc_sp_valid = ( self->state.current_key.kind == REGION_KIND_LOGICAL );
        pcsp.show_pc = self->state.show_pc_marker;
        pcsp.show_sp = self->state.show_sp_marker;
        pcsp.pc = g_mzarch_main.cpu ? g_mzarch_main.cpu->pc : 0;
        pcsp.sp = g_mzarch_main.cpu ? g_mzarch_main.cpu->sp : 0;

        /* V1 extras - region kontext pro layers/symbols/context menu.
         * Plněno vždy (i bez Layers panelu open), aby context menu (freeze,
         * bookmark) fungovalo z hex grid right-click i v default layoutu. */
        st_MEMBROWSER_HEXVIEW_EXTRAS extras;
        extras.region_id = self->state.current_region_id;
        extras.region_kind = self->state.current_key.kind;
        extras.sub_id = self->state.current_key.sub_id;
        extras.logical_base = 0xFFFFFFFF;
        for ( int i = 0; i < self->regions.count; i++ ) {
            if ( self->regions.items[i].id == self->state.current_region_id ) {
                extras.logical_base = self->regions.items[i].logical_base;
                break;
            }
        }

        /* V2: 3-panel layout helper - Regions sidebar (left, optional) |
         * Hex view (middle, vždy) | Layers panel (right, optional). Splittery
         * resize logic + auto-compute panel widths uvnitř helperu. */
        render_main_layout ( self, &be, &pcsp, &extras, avail );

        /* V6: Fill dialog modal + Ctrl+Z/Y shortcuts.
         *
         * Modal je renderovaný mimo hex view child scope (= popup ID na
         * hlavním mb okně). RMB context menu volá fill_dialog_request_open
         * který nastaví pending request; render uvnitř BeginPopup volá
         * OpenPopup při čerstvém requestu.
         *
         * Ctrl+Z / Ctrl+Y handler chytá globální shortcuts pouze pokud
         * mb okno je focused (= IsWindowFocused včetně child windows).
         * Test IsAnyItemActive() blokuje shortcut, pokud uživatel píše
         * v InputText (Goto / Search) - jinak by se Ctrl+Z bral jako
         * undo InputText buffer.
         */
        membrowser_fill_dialog_render ( &be, self->state.instance_idx );
        /* V6: Annotation modal - sdílí stejný request/render pattern. */
        membrowser_annot_dialog_render ( );
        if ( ImGui::IsWindowFocused ( ImGuiFocusedFlags_ChildWindows )
             && !ImGui::IsAnyItemActive ( )
             && ( ImGui::GetIO ( ).KeyCtrl ) ) {
            if ( ImGui::IsKeyPressed ( ImGuiKey_Z, false ) ) {
                membrowser_fill_dialog_do_undo ( &be,
                                                  self->state.current_key.kind,
                                                  self->state.current_key.sub_id,
                                                  self->state.instance_idx );
            } else if ( ImGui::IsKeyPressed ( ImGuiKey_Y, false ) ) {
                membrowser_fill_dialog_do_redo ( &be,
                                                  self->state.current_key.kind,
                                                  self->state.current_key.sub_id,
                                                  self->state.instance_idx );
            }
        }
    } else {
        ImGui::TextDisabled ( "%s", _( "No region available" ) );
    }

    handle_navigation_keys ( self );

    ImGui::Separator ( );
    render_bottom_bar ( self );

    ImGui::End ( );
}


extern "C" void membrowser_window_render ( bool *p_open )
{
    if ( !s_views[MB_INSTANCE_MAIN] ) {
        s_views[MB_INSTANCE_MAIN] = membrowser_view_create (
            SLOT_WINDOW_ID[MB_INSTANCE_MAIN] );
        if ( !s_views[MB_INSTANCE_MAIN] ) return;
    }
    membrowser_view_render ( s_views[MB_INSTANCE_MAIN], p_open );
}


/* Render jednoho sekundárního slotu (1..4 = #2..#5). Lazy-create při
 * otevření, lazy-destroy při zavření (save state do persist před delete). */
static void render_secondary_slot ( int slot_idx )
{
    if ( slot_idx <= MB_INSTANCE_MAIN || slot_idx >= MB_INSTANCE_COUNT ) return;

    /* g_gui->showMemoryBrowserWindowExtra je indexované 0..3 pro sloty 1..4. */
    int gui_idx = slot_idx - 1;
    bool *p_open = &g_gui->showMemoryBrowserWindowExtra[gui_idx];

    if ( !*p_open ) {
        /* Okno zavřené - pokud existuje instance, uvolnit (save inside). */
        if ( s_views[slot_idx] ) {
            membrowser_view_destroy ( s_views[slot_idx] );
            s_views[slot_idx] = nullptr;
        }
        return;
    }

    /* Lazy create. */
    if ( !s_views[slot_idx] ) {
        s_views[slot_idx] = membrowser_view_create ( SLOT_WINDOW_ID[slot_idx] );
        if ( !s_views[slot_idx] ) {
            /* Alokace selhala - tichý fallback: vypni flag. */
            *p_open = false;
            return;
        }
    }

    membrowser_view_render ( s_views[slot_idx], p_open );
}


extern "C" void membrowser_window_render_all_secondary ( void )
{
    if ( !g_gui ) return;
    for ( int i = 1; i < MB_INSTANCE_COUNT; i++ ) {
        render_secondary_slot ( i );
    }
}


extern "C" void membrowser_window_show_hide ( void )
{
    g_gui->showMemoryBrowserWindow = !g_gui->showMemoryBrowserWindow;
}


/* Stabilní jména sekcí cfgmain modulu per slot. Musí žít po dobu programu,
 * cfgroot_register_new_module si pointer drží. */
static const char *const SLOT_CFG_SECTION[MB_INSTANCE_COUNT] = {
    "MEMBROWSER_WINDOW_MAIN",
    "MEMBROWSER_WINDOW_2",
    "MEMBROWSER_WINDOW_3",
    "MEMBROWSER_WINDOW_4",
    "MEMBROWSER_WINDOW_5"
};


extern "C" void membrowser_window_register_persistence_all ( void *cfgroot_void )
{
    if ( !cfgroot_void ) return;
    st_CFGROOT *cfgroot = ( st_CFGROOT * ) cfgroot_void;

    /* Detekce, jestli legacy sekce [MEMBROWSER_WINDOW] byla v INI nahlížena
     * pred registraci sekce MAIN. cfgfile API neumoznuje primy lookup na
     * "existuje sekce v souboru"; spoléháme na to, že hodnota
     * encoding v legacy slotu po cfgmodule_parse bude jiná než default 0
     * v případě, že INI obsahuje nějakou non-zero hodnotu. Pro robustnejsi
     * detekci by bylo treba rozsireni cfgfile API; pro V3 stačí scratch
     * register + apply_legacy_fallback ktery zkopiruje legacy -> MAIN
     * vždy pokud MAIN ma defaultni stav (= encoding=0 + cursor=0 + ...).
     *
     * Strategie: REGISTROVAT legacy první (= naplní scratch slot z INI),
     * pak MAIN sekci (= naplni MAIN slot z INI, pripadne defaultem).
     * apply_legacy_fallback_if_needed pak heuristikou zkontroluje, jestli
     * MAIN slot vypadá "panenský" (= encoding=0, region_kind=0, cursor=0,
     * layers vsechny 0) a pokud ano A legacy slot ma neco non-zero,
     * překopíruje legacy -> MAIN. Pokud uživatel V0/V1/V2 INI měl všechno
     * defaultní, kopírovat netřeba. */

    /* 1) Legacy sekce [MEMBROWSER_WINDOW] -> scratch slot. */
    CFGMOD *legacy_mod = cfgroot_register_new_module ( cfgroot,
                                                         ( char * ) "MEMBROWSER_WINDOW" );
    if ( legacy_mod ) {
        membrowser_state_register_persistence ( legacy_mod, MB_INSTANCE_COUNT );
        /* Note: MB_INSTANCE_COUNT = MB_INSTANCE_LEGACY uvnitř state.cpp. */
        cfgmodule_parse ( legacy_mod );
        cfgmodule_propagate ( legacy_mod );
    }

    /* 2) Per-instance sekce. */
    bool main_had_nontrivial = false;
    for ( int i = 0; i < MB_INSTANCE_COUNT; i++ ) {
        CFGMOD *mod = cfgroot_register_new_module ( cfgroot,
                                                     ( char * ) SLOT_CFG_SECTION[i] );
        if ( !mod ) continue;
        membrowser_state_register_persistence ( mod, i );
        cfgmodule_parse ( mod );
        cfgmodule_propagate ( mod );

        /* Heuristic pro detekci, ze MAIN sekce byla v INI (= mela explicitne
         * non-default hodnotu encoding, region_kind, cursor_addr nebo
         * bytes_per_row jiny nez 32). Pokud ne, je MAIN "panenský" a stojí
         * za to zkusit legacy fallback.
         *
         * NOTE: Toto je conservative heuristika. V edge case kdy uživatel
         * měl V3 INI s explicit defaults, fallback se neaktivuje ani by
         * neměl mít efekt (= legacy slot byl by stejně 0). */
        if ( i == MB_INSTANCE_MAIN ) {
            st_MEMBROWSER_STATE tmp = membrowser_state_default ( MB_INSTANCE_MAIN );
            membrowser_state_apply_persisted ( &tmp );
            if ( tmp.cursor_addr != 0 || tmp.current_encoding != 0
                    || tmp.current_key.kind != REGION_KIND_LOGICAL
                    || tmp.current_key.sub_id != 0
                    || tmp.bytes_per_row != 32
                    || tmp.layers_panel_open ) {
                main_had_nontrivial = true;
            }
        }
    }

    /* 3) Legacy fallback - jen pokud MAIN vypadá panenský. */
    membrowser_state_apply_legacy_fallback_if_needed ( main_had_nontrivial );

    /* V6: load annotations z perzistentního souboru (relative cesta v CWD).
     * Save se volá explicitně z view_destroy + při každém Save v dialogu. */
    membrowser_annot_dialog_load_on_startup ( );
}


extern "C" void membrowser_window_register_persistence ( void *cmod )
{
    /* DEPRECATED: zachováno pro backward-compat. Registruje jen MAIN slot.
     * Pro V3 multi-view použij membrowser_window_register_persistence_all. */
    membrowser_state_register_persistence ( cmod, MB_INSTANCE_MAIN );
}


extern "C" void membrowser_window_apply_persisted ( void )
{
    /* V0-leftovers F4: --memory-browser FLAG - pokud přítomen, vynutí
     * hlavní okno otevřené při startu. Sekundární okna #2..#5 se neaktivují
     * automaticky - uživatel je otevírá z menu Debugger explicit.
     * Pattern shodný s event_viewer_window_apply_persisted. */
    if ( !g_gui ) return;
    if ( sdlapp_option_present ( "--memory-browser" ) ) {
        g_gui->showMemoryBrowserWindow = true;
    }
}


/* === Char Inserter podporné API ===========================================
 *
 * Char Inserter okno (samostatný modul membrowser_char_inserter.cpp)
 * potřebuje:
 *   - per-frame informace o cursor pos / writable / encoding zdrojové
 *     MB instance (pro status řádek a tooltip badge),
 *   - cestu jak zapsat byte na cursor pos s identickou semantikou jako
 *     interní ASCII typing (undo push + edited mark + advance cursor).
 *
 * Implementace tady, protože MembrowserView struct a static s_views jsou
 * private TU-local (= v tomto souboru). Vystavujeme jen 2 stabilní C-API
 * funkce, char inserter NESmí znát internal strukturu instance. */
extern "C" bool membrowser_window_get_cursor_info ( int instance_idx,
                                                     uint32_t *cursor_addr_out,
                                                     bool *writable_out,
                                                     int *encoding_id_out,
                                                     char *region_name_buf,
                                                     unsigned long region_name_buf_sz )
{
    if ( instance_idx < 0 || instance_idx >= MB_INSTANCE_COUNT ) return false;
    MembrowserView *v = s_views[instance_idx];
    if ( !v ) return false;
    if ( v->state.current_region_id < 0 ) return false;

    if ( cursor_addr_out ) *cursor_addr_out = v->state.cursor_addr;
    if ( encoding_id_out ) *encoding_id_out = v->state.current_encoding;

    bool hw_w = find_current_region_writable ( v );
    if ( writable_out ) *writable_out = hw_w && v->state.edit_enabled;

    if ( region_name_buf && region_name_buf_sz > 0 ) {
        region_name_buf[0] = '\0';
        for ( int i = 0; i < v->regions.count; i++ ) {
            if ( v->regions.items[i].id == v->state.current_region_id ) {
                std::snprintf ( region_name_buf,
                                ( size_t ) region_name_buf_sz,
                                "%s", v->regions.items[i].name );
                break;
            }
        }
    }
    return true;
}


extern "C" bool membrowser_window_write_byte_at_cursor ( int instance_idx,
                                                          uint8_t byte )
{
    if ( instance_idx < 0 || instance_idx >= MB_INSTANCE_COUNT ) return false;
    MembrowserView *v = s_views[instance_idx];
    if ( !v ) return false;
    if ( v->state.current_region_id < 0 ) return false;
    if ( !v->state.edit_enabled ) return false;
    if ( !find_current_region_writable ( v ) ) return false;

    uint64_t total = find_current_region_size ( v );
    if ( total == 0 ) return false;
    uint64_t pos = ( uint64_t ) v->state.cursor_addr;
    if ( pos >= total ) return false;

    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, v->state.current_region_id, true );
    if ( !be.read_bytes || !be.write_bytes ) return false;

    uint8_t old_byte = 0;
    if ( be.read_bytes ( be.ctx, pos, &old_byte, 1 ) != 1 ) return false;

    if ( byte != old_byte ) {
        if ( be.write_bytes ( be.ctx, pos, &byte, 1 ) != 1 ) return false;
        /* Per-byte undo push + recently edited mark (identicky s hexview
         * ASCII / HEX typing dispatch). region_kind/sub_id z current_key.
         * V1.5+: mark s original=old_byte + check_unmark s byte (= session
         * original detection po vložení znaku z palety). */
        membrowser_undo_push ( v->state.current_key.kind,
                                v->state.current_key.sub_id,
                                pos, &old_byte, 1 );
        membrowser_edited_mark ( v->state.instance_idx,
                                  v->state.current_key.kind,
                                  v->state.current_key.sub_id,
                                  ( uint32_t ) pos, old_byte );
        membrowser_edited_check_unmark ( v->state.instance_idx,
                                          v->state.current_key.kind,
                                          v->state.current_key.sub_id,
                                          ( uint32_t ) pos, byte );
    }

    /* Posun cursor na další byte (stejně jako ASCII typing). */
    if ( pos + 1 < total ) {
        v->state.cursor_addr = ( uint32_t ) ( pos + 1 );
    }
    return true;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
