/**
 * @file membrowser_regions.cpp
 * @brief V2 Regions tree - implementace levého sidebaru s hierarchií regionů.
 *
 * Iteruje per-frame snapshot z dbgapi a buduje strom skupin po druzích
 * (REGION_KIND_*). Skupiny bez položek (např. PCG na MZ-800) se nezobrazují
 * - dbgapi pro daný arch tyto regiony vůbec neenumeruje, takže náš filtr
 * vychází automaticky správně.
 *
 * Disconnected regiony (r->connected == 0) jsou stále vidět - jen se vykreslí
 * jako TextDisabled bez možnosti aktivace. To je rozdíl oproti V0 dropdown
 * který je úplně skrýval. UI tak ukazuje uživateli, že platforma podporuje
 * danou periferii, ale není připojená (= jasné info kde hledat config).
 *
 * Pořadí skupin v stromu odpovídá frekvenci použití pro debugging:
 *   1. Logical Z80 (= co Z80 reálně vidí v current banking - default)
 *   2. User RAM (= 64 KB fyzická DRAM)
 *   3. Monitor ROM (lower / upper)
 *   4. CG-ROM
 *   5. VRAM (Physical planes / 700 char / 700 attr)
 *   6. CG-RAM (700 mode)
 *   7. PCG (MZ-1500)
 *   8. Memext (RAM banks / FLASH banks)
 *   9. Ramdisk STD
 *  10. Ramdisk PEZIK 68 / E8
 *  11. PROHIBITED shadow (jen pokud aktivní)
 *
 * Klik na leaf = stejná akce jako V0 dropdown Selectable: přepiš state
 * (kind, sub_id), reset cursor + edit_nibble, persist save.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_regions.h"
#include "membrowser_state.h"
#include "membrowser_io.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
}


/* Velikost ID bufferu pro ImGui tree nodes a leafs - prefix mb_rt_ +
 * dostatečně pro kind+sub_id zápis. */
#define MB_REGIONS_ID_BUFLEN 48


/* ---------- Helpers ----------------------------------------------------- */


/**
 * @brief Zjistí, zda snapshot obsahuje alespoň jeden region daného druhu.
 *
 * Pro vykreslení skupinového TreeNode má smysl jen pokud aspoň jedna
 * položka existuje. Skupiny bez položek (= arch je nemá enumerated)
 * úplně skryjeme.
 *
 * @param snap Snapshot regionů.
 * @param kind Hledaný en_REGION_KIND.
 * @return true pokud existuje alespoň jedna položka tohoto druhu.
 */
static bool snapshot_has_kind ( const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                 int kind )
{
    for ( int i = 0; i < snap->count; i++ ) {
        if ( ( int ) snap->items[i].kind == kind ) return true;
    }
    return false;
}


/**
 * @brief Vrátí pointer na descriptor regionu (kind, sub_id) nebo NULL.
 *
 * @param snap   Snapshot.
 * @param kind   en_REGION_KIND.
 * @param sub_id Disambiguator.
 * @return Pointer na descriptor v poli snap->items, nebo NULL pokud nenalezen.
 */
static const st_REGION_DESC *find_desc ( const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                          int kind, int sub_id )
{
    for ( int i = 0; i < snap->count; i++ ) {
        if ( ( int ) snap->items[i].kind == kind
                && snap->items[i].sub_id == sub_id ) {
            return &snap->items[i];
        }
    }
    return NULL;
}


/**
 * @brief Aktivuje daný region jako current_key + provede side-effects.
 *
 * Volá se po kliknutí na leaf v tree. Side-effects:
 *   - state.current_key (kind, sub_id) = parametry
 *   - cursor_addr a scroll_top_addr reset na 0
 *   - edit_nibble reset na 0 (aby další stisk šel do high nibble)
 *   - persist save (cfgmain INI při příštím close)
 *
 * Pattern shodný s V0 Region dropdown Selectable callback.
 *
 * @param st     Memory Browser state.
 * @param kind   Cílový en_REGION_KIND.
 * @param sub_id Cílový sub_id.
 */
static void activate_region ( st_MEMBROWSER_STATE *st, int kind, int sub_id )
{
    st->current_key.kind = kind;
    st->current_key.sub_id = sub_id;
    st->cursor_addr = 0;
    st->scroll_top_addr = 0;
    st->edit_nibble = 0;
    membrowser_state_save_to_persisted ( st );
}


/**
 * @brief Vykreslí leaf node pro jeden region.
 *
 * Selectable s indikací "selected" pokud (kind, sub_id) odpovídá current_key.
 * Pokud r->connected == 0, vykreslí jako TextDisabled (= gray-out, žádný
 * click handler). Label = label parametr (volaný předem ze short jména +
 * sub_id badge).
 *
 * @param st     Memory Browser state (pro current_key compare a aktivaci).
 * @param r      Descriptor regionu.
 * @param label  Lokalizovaný label pro zobrazení.
 * @param id_str ImGui ID suffix (kvůli unikátnímu PushID per leaf).
 */
static void render_leaf ( st_MEMBROWSER_STATE *st,
                           const st_REGION_DESC *r,
                           const char *label,
                           const char *id_str )
{
    bool selected = ( st->current_key.kind == ( int ) r->kind
                       && st->current_key.sub_id == r->sub_id );

    if ( !r->connected ) {
        ImGui::TextDisabled ( "%s", label );
        return;
    }

    ImGui::PushID ( id_str );
    if ( ImGui::Selectable ( label, selected ) ) {
        activate_region ( st, ( int ) r->kind, r->sub_id );
    }
    if ( selected ) ImGui::SetItemDefaultFocus ( );
    ImGui::PopID ( );
}


/**
 * @brief Vykreslí jednoduchou skupinu s jedním leaf (= single-region kind).
 *
 * Použité pro skupiny LOGICAL/RAM/CGROM/CGRAM_700 atd. kde je jen 1 položka.
 * Pokud snapshot daný kind nemá, skupinu úplně vynechá.
 *
 * @param st     Memory Browser state.
 * @param snap   Snapshot.
 * @param kind   en_REGION_KIND.
 * @param label  Lokalizovaný label leaf nodu (label samotného regionu).
 */
static void render_single_kind ( st_MEMBROWSER_STATE *st,
                                  const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                  int kind,
                                  const char *label )
{
    const st_REGION_DESC *r = find_desc ( snap, kind, 0 );
    if ( !r ) return;

    char id_buf[MB_REGIONS_ID_BUFLEN];
    std::snprintf ( id_buf, sizeof ( id_buf ), "mb_rt_k%d", kind );
    render_leaf ( st, r, label, id_buf );
}


/**
 * @brief Vykreslí ROM skupinu (lower + upper) jako TreeNode s 2 leafs.
 *
 * Skupinu zobrazí jen pokud aspoň jeden ze dvou ROM regionů existuje.
 * Default open = false (= méně visual noise při startu).
 */
static void render_rom_group ( st_MEMBROWSER_STATE *st,
                                const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    bool has_low = snapshot_has_kind ( snap, REGION_KIND_ROM_LOWER );
    bool has_up = snapshot_has_kind ( snap, REGION_KIND_ROM_UPPER );
    if ( !has_low && !has_up ) return;

    if ( ImGui::TreeNodeEx ( _L ( "Monitor ROM###mb_rt_rom_group" ),
                              ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
        if ( has_low ) {
            const st_REGION_DESC *r = find_desc ( snap, REGION_KIND_ROM_LOWER, 0 );
            if ( r ) render_leaf ( st, r, _( "lower (4 KB)" ), "mb_rt_rom_low" );
        }
        if ( has_up ) {
            const st_REGION_DESC *r = find_desc ( snap, REGION_KIND_ROM_UPPER, 0 );
            if ( r ) render_leaf ( st, r, _( "upper (8 KB)" ), "mb_rt_rom_up" );
        }
        ImGui::TreePop ( );
    }
}


/**
 * @brief Vykreslí VRAM skupinu (planes + 700 char/attr) jako TreeNode.
 *
 * Sub-skupina "Physical Planes" pro plane I-IV. Plane III/IV jsou
 * v dbgapi vždy enumerované (EXVRAM pole je vždy alokované) - tag
 * "[exVRAM]" v labelu je informativní per V2 brief, ale v default
 * MZ-800 emu se ošetří přes connected flag pokud bude doplněn.
 *
 * VRAM 700 char/attr se ukazuje jen pokud snapshot obsahuje (= MZ-800
 * v 700 compat módu, MZ-700 native, MZ-1500). Na MZ-800 v native módu
 * dbgapi tyto regiony neenumeruje (jsou pod planes).
 */
static void render_vram_group ( st_MEMBROWSER_STATE *st,
                                 const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    bool has_plane = snapshot_has_kind ( snap, REGION_KIND_VRAM_PHYS_PLANE );
    bool has_char = snapshot_has_kind ( snap, REGION_KIND_VRAM_700_CHAR );
    bool has_attr = snapshot_has_kind ( snap, REGION_KIND_VRAM_700_ATTR );
    if ( !has_plane && !has_char && !has_attr ) return;

    if ( ImGui::TreeNodeEx ( _L ( "VRAM###mb_rt_vram_group" ),
                              ImGuiTreeNodeFlags_SpanAvailWidth
                              | ImGuiTreeNodeFlags_DefaultOpen ) ) {

        if ( has_plane ) {
            if ( ImGui::TreeNodeEx ( _L ( "Physical Planes###mb_rt_vram_planes" ),
                                      ImGuiTreeNodeFlags_SpanAvailWidth
                                      | ImGuiTreeNodeFlags_DefaultOpen ) ) {
                static const char *plane_labels[4] = {
                    "Plane I (8 KB)",
                    "Plane II (8 KB)",
                    "Plane III (8 KB) [exVRAM]",
                    "Plane IV (8 KB) [exVRAM]"
                };
                for ( int p = 0; p < 4; p++ ) {
                    const st_REGION_DESC *r =
                        find_desc ( snap, REGION_KIND_VRAM_PHYS_PLANE, p );
                    if ( !r ) continue;
                    char id_buf[MB_REGIONS_ID_BUFLEN];
                    std::snprintf ( id_buf, sizeof ( id_buf ),
                                    "mb_rt_vram_plane_%d", p );
                    render_leaf ( st, r, _( plane_labels[p] ), id_buf );
                }
                ImGui::TreePop ( );
            }
        }

        if ( has_char ) {
            const st_REGION_DESC *r =
                find_desc ( snap, REGION_KIND_VRAM_700_CHAR, 0 );
            if ( r ) render_leaf ( st, r, _( "VRAM 700 char (1 KB)" ),
                                    "mb_rt_vram_700_char" );
        }
        if ( has_attr ) {
            const st_REGION_DESC *r =
                find_desc ( snap, REGION_KIND_VRAM_700_ATTR, 0 );
            if ( r ) render_leaf ( st, r, _( "VRAM 700 attr (1 KB)" ),
                                    "mb_rt_vram_700_attr" );
        }

        ImGui::TreePop ( );
    }
}


/**
 * @brief Vykreslí PCG skupinu (MZ-1500) - 3 banky po 8 KB.
 */
static void render_pcg_group ( st_MEMBROWSER_STATE *st,
                                const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    if ( !snapshot_has_kind ( snap, REGION_KIND_PCG_1500 ) ) return;

    if ( ImGui::TreeNodeEx ( _L ( "PCG###mb_rt_pcg_group" ),
                              ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
        for ( int b = 0; b < 3; b++ ) {
            const st_REGION_DESC *r = find_desc ( snap, REGION_KIND_PCG_1500, b );
            if ( !r ) continue;
            char label[32];
            std::snprintf ( label, sizeof ( label ),
                            _( "Bank %d (8 KB)" ), b + 1 );
            char id_buf[MB_REGIONS_ID_BUFLEN];
            std::snprintf ( id_buf, sizeof ( id_buf ), "mb_rt_pcg_%d", b );
            render_leaf ( st, r, label, id_buf );
        }
        ImGui::TreePop ( );
    }
}


/**
 * @brief Vykreslí Memext skupinu (RAM banks + FLASH banks).
 *
 * Per-bank enumerace dbgapi vrací jen banky které jsou v g_memext.map[]
 * mapované jako alokované; PEHU má menší set než Luftner. Banky řadíme
 * podle sub_id (raw bank index).
 *
 * FLASH banky jsou read-only - to už označuje r->writable. UI sub-label
 * v stromu nepřidává "[R-only]" - status bar po výběru ukáže "R/O".
 */
static void render_memext_group ( st_MEMBROWSER_STATE *st,
                                   const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    bool has_ram = snapshot_has_kind ( snap, REGION_KIND_MEMEXT_RAM );
    bool has_flash = snapshot_has_kind ( snap, REGION_KIND_MEMEXT_FLASH );
    if ( !has_ram && !has_flash ) return;

    if ( ImGui::TreeNodeEx ( _L ( "Memext###mb_rt_memext_group" ),
                              ImGuiTreeNodeFlags_SpanAvailWidth ) ) {

        if ( has_ram ) {
            if ( ImGui::TreeNodeEx ( _L ( "RAM banks###mb_rt_memext_ram" ),
                                      ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
                for ( int i = 0; i < snap->count; i++ ) {
                    const st_REGION_DESC *r = &snap->items[i];
                    if ( ( int ) r->kind != REGION_KIND_MEMEXT_RAM ) continue;
                    char label[64];
                    std::snprintf ( label, sizeof ( label ),
                                    _( "Bank 0x%02X (4 KB)" ),
                                    ( unsigned ) r->sub_id );
                    char id_buf[MB_REGIONS_ID_BUFLEN];
                    std::snprintf ( id_buf, sizeof ( id_buf ),
                                    "mb_rt_memext_ram_%d", r->sub_id );
                    render_leaf ( st, r, label, id_buf );
                }
                ImGui::TreePop ( );
            }
        }
        if ( has_flash ) {
            if ( ImGui::TreeNodeEx ( _L ( "FLASH banks###mb_rt_memext_flash" ),
                                      ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
                for ( int i = 0; i < snap->count; i++ ) {
                    const st_REGION_DESC *r = &snap->items[i];
                    if ( ( int ) r->kind != REGION_KIND_MEMEXT_FLASH ) continue;
                    char label[64];
                    std::snprintf ( label, sizeof ( label ),
                                    _( "Bank 0x%02X (4 KB) [R/O]" ),
                                    ( unsigned ) r->sub_id );
                    char id_buf[MB_REGIONS_ID_BUFLEN];
                    std::snprintf ( id_buf, sizeof ( id_buf ),
                                    "mb_rt_memext_flash_%d", r->sub_id );
                    render_leaf ( st, r, label, id_buf );
                }
                ImGui::TreePop ( );
            }
        }

        ImGui::TreePop ( );
    }
}


/**
 * @brief Vykreslí Ramdisk STD skupinu.
 *
 * Banky po 64 KB; počet závisí na g_ramdisk.std.size (64 / 256 / 512 / 1M / 16M).
 * Dbgapi enumeruje aktivní set (1..256). Pro velký set (256 banks) je
 * tree node levný (žádné per-bank decode hex view), takže bez clipperu.
 */
static void render_ramdisk_std_group ( st_MEMBROWSER_STATE *st,
                                        const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    if ( !snapshot_has_kind ( snap, REGION_KIND_RAMDISK_STD ) ) return;

    if ( ImGui::TreeNodeEx ( _L ( "Ramdisk STD###mb_rt_rd_std" ),
                              ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
        for ( int i = 0; i < snap->count; i++ ) {
            const st_REGION_DESC *r = &snap->items[i];
            if ( ( int ) r->kind != REGION_KIND_RAMDISK_STD ) continue;
            char label[64];
            std::snprintf ( label, sizeof ( label ),
                            _( "Bank 0x%02X (64 KB)" ),
                            ( unsigned ) r->sub_id );
            char id_buf[MB_REGIONS_ID_BUFLEN];
            std::snprintf ( id_buf, sizeof ( id_buf ),
                            "mb_rt_rd_std_%d", r->sub_id );
            render_leaf ( st, r, label, id_buf );
        }
        ImGui::TreePop ( );
    }
}


/**
 * @brief Vykreslí Ramdisk PEZIK skupinu (instance 68 nebo E8).
 *
 * V dbgapi je PEZIK kódovaný jako sub_id = pezik_instance * 8 + bank_idx.
 * Instance 0 = port 0x68 (sub_id 0..7), instance 1 = port 0xE8 (sub_id 8..15).
 *
 * @param st       Memory Browser state.
 * @param snap     Snapshot.
 * @param instance 0 (68) nebo 1 (E8).
 * @param label    Lokalizovaný label skupiny ("Ramdisk PEZIK 68" / "PEZIK E8").
 * @param id_str   ImGui ID suffix skupiny.
 */
static void render_ramdisk_pezik_instance ( st_MEMBROWSER_STATE *st,
                                             const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                             int instance,
                                             const char *label,
                                             const char *id_str )
{
    /* Zjisti zda instance má aspoň jednu bank v snapshotu. */
    bool has_any = false;
    for ( int i = 0; i < snap->count; i++ ) {
        const st_REGION_DESC *r = &snap->items[i];
        if ( ( int ) r->kind != REGION_KIND_RAMDISK_PEZIK ) continue;
        if ( ( r->sub_id / 8 ) == instance ) { has_any = true; break; }
    }
    if ( !has_any ) return;

    char id_buf[MB_REGIONS_ID_BUFLEN];
    std::snprintf ( id_buf, sizeof ( id_buf ), "%s###%s", label, id_str );
    if ( ImGui::TreeNodeEx ( id_buf, ImGuiTreeNodeFlags_SpanAvailWidth ) ) {
        for ( int b = 0; b < 8; b++ ) {
            int sub = instance * 8 + b;
            const st_REGION_DESC *r =
                find_desc ( snap, REGION_KIND_RAMDISK_PEZIK, sub );
            if ( !r ) continue;
            char lbl[64];
            std::snprintf ( lbl, sizeof ( lbl ),
                            _( "Bank %d (64 KB)" ), b );
            char leaf_id[MB_REGIONS_ID_BUFLEN];
            std::snprintf ( leaf_id, sizeof ( leaf_id ),
                            "mb_rt_pezik_%d", sub );
            render_leaf ( st, r, lbl, leaf_id );
        }
        ImGui::TreePop ( );
    }
}


/* ---------- Public API ------------------------------------------------- */


extern "C" void membrowser_regions_render_toolbar_toggle ( st_MEMBROWSER_STATE *st )
{
    const char *label = st->regions_panel_open
        ? _L ( "Regions: ON###mb_regions_toggle" )
        : _L ( "Regions: OFF###mb_regions_toggle" );

    bool pushed = false;
    if ( st->regions_panel_open ) {
        /* Klidná tlumeně zelená pro "panel aktivní" - méně agresivní než
         * červená Edit highlight (= edit je destruktivní akce, tady jen
         * navigation panel viditelnost). */
        ImGui::PushStyleColor ( ImGuiCol_Button,
                                 ImVec4 ( 0.20f, 0.45f, 0.25f, 1.00f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonHovered,
                                 ImVec4 ( 0.28f, 0.58f, 0.34f, 1.00f ) );
        ImGui::PushStyleColor ( ImGuiCol_ButtonActive,
                                 ImVec4 ( 0.14f, 0.36f, 0.20f, 1.00f ) );
        pushed = true;
    }
    if ( ImGui::Button ( label ) ) {
        st->regions_panel_open = !st->regions_panel_open;
        membrowser_state_save_to_persisted ( st );
    }
    if ( pushed ) ImGui::PopStyleColor ( 3 );

    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Toggle Regions sidebar (hierarchical region tree)" ) );
    }
}


extern "C" void membrowser_regions_render_tree ( st_MEMBROWSER_STATE *st,
                                                  const st_MEMBROWSER_REGION_SNAPSHOT *snap )
{
    if ( !st || !snap ) return;

    /* Skupina 1: Logical Z80 - vždy první, default selected. */
    render_single_kind ( st, snap, REGION_KIND_LOGICAL,
                          _( "Logical Z80 (current)" ) );

    /* Skupina 2: User RAM (64 KB). */
    render_single_kind ( st, snap, REGION_KIND_RAM, _( "User RAM (64 KB)" ) );

    /* Skupina 3: Monitor ROM lower/upper. */
    render_rom_group ( st, snap );

    /* Skupina 4: CG-ROM (single leaf). */
    render_single_kind ( st, snap, REGION_KIND_CGROM, _( "CG-ROM (4 KB)" ) );

    /* Skupina 5: VRAM (planes + 700 char/attr). */
    render_vram_group ( st, snap );

    /* Skupina 6: CG-RAM (jen 700 mode / MZ-700). */
    render_single_kind ( st, snap, REGION_KIND_CGRAM_700,
                          _( "CG-RAM (4 KB) [700 mode]" ) );

    /* Skupina 7: PCG (MZ-1500). */
    render_pcg_group ( st, snap );

    /* Skupina 8: Memext (RAM + FLASH banks). */
    render_memext_group ( st, snap );

    /* Skupina 9: Ramdisk STD. */
    render_ramdisk_std_group ( st, snap );

    /* Skupina 10+11: Ramdisk PEZIK 68 / E8 (oddělené skupiny pro lepší
     * UX - každá instance má vlastní port a vlastních 8 banks). */
    render_ramdisk_pezik_instance ( st, snap, 0,
                                     _( "Ramdisk PEZIK 0x68" ),
                                     "mb_rt_pezik_68" );
    render_ramdisk_pezik_instance ( st, snap, 1,
                                     _( "Ramdisk PEZIK 0xE8" ),
                                     "mb_rt_pezik_e8" );

    /* Skupina 12: PROHIBITED shadow (single leaf, jen pokud arch má). */
    render_single_kind ( st, snap, REGION_KIND_PROHIBITED_SHADOW,
                          _( "Prohibited shadow" ) );
}


extern "C" int membrowser_regions_compute_optimal_panel_width ( void )
{
    /* Spočti max(CalcTextSize) z typických (= delších po lokalizaci) labelů
     * tree skupin a leafs. Reservation: indent (~28 px per nest) + arrow
     * (~16 px) + scrollbar (~14 px) + border (4 px) = ~62 px overhead nad
     * širším labelem. */
    const char *candidates[] = {
        _( "Logical Z80 (current)" ),
        _( "User RAM (64 KB)" ),
        _( "Monitor ROM" ),
        _( "lower (4 KB)" ),
        _( "upper (8 KB)" ),
        _( "CG-ROM (4 KB)" ),
        _( "VRAM" ),
        _( "Physical Planes" ),
        _( "Plane III (8 KB) [exVRAM]" ),
        _( "Plane IV (8 KB) [exVRAM]" ),
        _( "VRAM 700 char (1 KB)" ),
        _( "VRAM 700 attr (1 KB)" ),
        _( "CG-RAM (4 KB) [700 mode]" ),
        _( "PCG" ),
        _( "Memext" ),
        _( "RAM banks" ),
        _( "FLASH banks" ),
        _( "Bank 0xFF (4 KB) [R/O]" ),
        _( "Bank 0xFF (64 KB)" ),
        _( "Ramdisk STD" ),
        _( "Ramdisk PEZIK 0x68" ),
        _( "Ramdisk PEZIK 0xE8" ),
        _( "Prohibited shadow" ),
    };
    float max_w = 0.0f;
    for ( size_t i = 0; i < sizeof ( candidates ) / sizeof ( candidates[0] ); i++ ) {
        ImVec2 sz = ImGui::CalcTextSize ( candidates[i] );
        /* Plane III/Plane IV jsou pod sub-tree (= 2 úrovně indent) - přidej
         * 28 px na simulovaný indent pro nested leaf. */
        float adjusted = sz.x;
        if ( std::strstr ( candidates[i], "Plane III" )
                || std::strstr ( candidates[i], "Plane IV" )
                || std::strstr ( candidates[i], "Bank 0x" ) ) {
            adjusted += 28.0f;  /* nested 2nd-level leaf indent */
        }
        if ( adjusted > max_w ) max_w = adjusted;
    }

    int w = ( int ) ( max_w + 62.0f );  /* overhead */
    if ( w < 180 ) w = 180;
    if ( w > 420 ) w = 420;
    return w;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
