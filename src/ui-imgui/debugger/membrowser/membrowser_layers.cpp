/**
 * @file membrowser_layers.cpp
 * @brief Implementace V1 Layers - sidebar panel + per-byte color resolver.
 *
 * Mapping (region_kind, offset) -> mhmap cell:
 *   - LOGICAL: jako 16-bit CPU adresa -> g_mhmap.bus[addr] přímo + případný
 *     mhmap_resolve_mem pokud chceme fyzické counters (V1: stačí bus mapa)
 *   - RAM: g_mhmap.ram[offset]
 *   - ROM_LOWER (MZ-800): g_mhmap.rom_lower[offset]
 *   - ROM_UPPER (MZ-800): g_mhmap.rom_upper[offset]
 *   - CGROM (MZ-800): g_mhmap.rom_cg[offset]
 *   - ROM (MZ-1500/700): g_mhmap.rom[offset]
 *   - CGROM (MZ-1500/700): g_mhmap.cgrom[offset]
 *   - VRAM/PCG/Memext/Ramdisk: V1 částečné - pro Memext_RAM mapuje na
 *     g_mhmap.memext[sub_id*0x1000 + offset], jinak return NULL (= layer pass)
 *
 * Counter heatmap normalizace: per-region max R+W+X+S napříč všemi cells
 * v dané mhmap pole. Pro V1 jednoduchá heuristika - max přes celou bus
 * mapu (= globální max). Pro V2 možno refinovat per-region.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_layers.h"
#include "membrowser_snapshot.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/debugger/debugger.h"
#include "emulator/debugger/freeze/freeze.h"
#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )
#include "emulator/debugger/mhmap.h"
#endif
}


/* Helper: true pokud MH/CDL recording je aktivní (mhmap_mode != OFF + případně
 * debugger window podmínka pro WITH_WINDOW mode). Tooltip pro CDL/Heatmap
 * checkboxy reflektuje tento stav. */
static bool layers_mhmap_recording_active ( void )
{
    return TEST_DEBUGGER_MHMAP_ACTIVE;
}


/* ============================================================================
 *  Color konstanty - ImU32 RGBA (= IM_COL32 little-endian).
 * ============================================================================
 */

/* Tmavě fialová - frozen byte (Mesen2 style). */
static const ImU32 c_bg_frozen        = IM_COL32 ( 90, 30, 120, 180 );

/* Magenta - snapshot Δ. */
static const ImU32 c_bg_snapshot_delta = IM_COL32 ( 200, 50, 180, 140 );

/* Světle modrá - CDL Code (X). */
static const ImU32 c_bg_cdl_x         = IM_COL32 ( 60, 110, 200, 110 );

/* Světle zelená - CDL Data Read. */
static const ImU32 c_bg_cdl_r         = IM_COL32 ( 60, 180, 90, 100 );

/* Světle červená - CDL Data Write. */
static const ImU32 c_bg_cdl_w         = IM_COL32 ( 200, 70, 70, 100 );


/* ============================================================================
 *  Helper: mapping (region_kind, offset) -> mhmap cell pointer
 * ============================================================================
 *
 * Vrací NULL pokud kind nemá v mhmap odpovídající pole (např. VRAM
 * mode-specific - V1 neimplementováno, PROHIBITED, Ramdisk).
 */
#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )
static const st_MHMAP_CELL *resolve_mhmap_cell ( int region_kind, int sub_id,
                                                  uint32_t offset )
{
    switch ( region_kind ) {
        case REGION_KIND_LOGICAL:
            if ( offset < 0x10000u ) return &g_mhmap.bus[offset];
            return NULL;
        case REGION_KIND_RAM:
            if ( offset < 0x10000u ) return &g_mhmap.ram[offset];
            return NULL;
#if MZARCH == 800
        case REGION_KIND_ROM_LOWER:
            if ( offset < MHMAP_SIZE_ROM_LOWER ) return &g_mhmap.rom_lower[offset];
            return NULL;
        case REGION_KIND_ROM_UPPER:
            if ( offset < MHMAP_SIZE_ROM_UPPER ) return &g_mhmap.rom_upper[offset];
            return NULL;
        case REGION_KIND_CGROM:
            /* MZ-800 native CGROM = rom_cg pole (4 KB). */
            if ( offset < MHMAP_SIZE_ROM_CG ) return &g_mhmap.rom_cg[offset];
            return NULL;
        case REGION_KIND_CGRAM_700:
            /* MZ-800 v MZ-700 compat: vram700_cg pole. */
            if ( offset < MHMAP_SIZE_VRAM700_CG ) return &g_mhmap.vram700_cg[offset];
            return NULL;
        case REGION_KIND_VRAM_700_CHAR:
            /* MZ-800 v MZ-700 compat: vram700 pole (char + attr 4 KB).
             * UI region je 2 KB char na D000-D7FF -> offset 0..0x7FF. */
            if ( offset < 0x800u ) return &g_mhmap.vram700[offset];
            return NULL;
        case REGION_KIND_VRAM_700_ATTR:
            /* Attr je 0x800-0xFFF v fyzickém vram700 (za 2 KB char). */
            if ( offset < 0x800u ) return &g_mhmap.vram700[0x800u + offset];
            return NULL;
#elif MZARCH == 1500
        case REGION_KIND_ROM_LOWER:
            /* MZ-1500 má jednu rom[16K], offset = lower 4 KB. */
            if ( offset < 0x1000u ) return &g_mhmap.rom[offset];
            return NULL;
        case REGION_KIND_ROM_UPPER:
            /* Upper = poslední 8 KB rom[]. */
            if ( offset < 0x2000u ) return &g_mhmap.rom[0x2000u + offset];
            return NULL;
        case REGION_KIND_CGROM:
            if ( offset < MHMAP_SIZE_CGROM ) return &g_mhmap.cgrom[offset];
            return NULL;
        case REGION_KIND_VRAM_700_CHAR:
            /* Znaková 2 KB = vram[0x000-0x7FF], atributová 2 KB = vram[0x800-0xFFF]. */
            if ( offset < 0x800u ) return &g_mhmap.vram[offset];
            return NULL;
        case REGION_KIND_VRAM_700_ATTR:
            if ( offset < 0x800u ) return &g_mhmap.vram[0x800u + offset];
            return NULL;
        case REGION_KIND_PCG_1500: {
            const st_MHMAP_CELL *base = NULL;
            switch ( sub_id ) {
                case 0: base = g_mhmap.pcg_1; break;
                case 1: base = g_mhmap.pcg_2; break;
                case 2: base = g_mhmap.pcg_3; break;
                default: return NULL;
            }
            if ( offset < MHMAP_SIZE_PCG_BANK ) return &base[offset];
            return NULL;
        }
#elif MZARCH == 700
        case REGION_KIND_ROM_LOWER:
            if ( offset < 0x1000u ) return &g_mhmap.rom[offset];
            return NULL;
        case REGION_KIND_ROM_UPPER:
            if ( offset < 0x2000u ) return &g_mhmap.rom[0x2000u + offset];
            return NULL;
        case REGION_KIND_CGROM:
            if ( offset < MHMAP_SIZE_CGROM ) return &g_mhmap.cgrom[offset];
            return NULL;
        case REGION_KIND_VRAM_700_CHAR:
            /* Znaková 2 KB = vram[0x000-0x7FF], atributová 2 KB = vram[0x800-0xFFF]. */
            if ( offset < 0x800u ) return &g_mhmap.vram[offset];
            return NULL;
        case REGION_KIND_VRAM_700_ATTR:
            if ( offset < 0x800u ) return &g_mhmap.vram[0x800u + offset];
            return NULL;
#endif
        case REGION_KIND_MEMEXT_RAM:
        case REGION_KIND_MEMEXT_FLASH: {
            /* Memext g_mhmap.memext je 512 KB linear; sub_id = raw bank (0..0xFF),
             * každá bank 4 KB. Offset uvnitř banky 0..0xFFF. */
            unsigned bank_off = ( unsigned ) sub_id * 0x1000u + offset;
            if ( bank_off < MHMAP_SIZE_MEMEXT ) return &g_mhmap.memext[bank_off];
            return NULL;
        }
        /* REGION_KIND_VRAM_PHYS_PLANE, RAMDISK_*, PROHIBITED_SHADOW:
         * V1 nepokrývá - layer pass (no color). V2 doplnit. */
        default:
            return NULL;
    }
}
#endif


/* ============================================================================
 *  Heatmap normalizace - per-frame globální max přes g_mhmap.bus
 * ============================================================================
 *
 * Pro V1 jednoduché - bere global max R+W+X+S z bus mapy. Pro Logical
 * region je to přirozený maximum. Pro non-Logical (RAM, ROM, ...) může
 * být max v jiném poli vyšší - výsledný heatmap pak bude "slabší" než
 * by mohl být. Akceptovatelné pro V1.
 */
#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )
static uint32_t s_heatmap_max_cache = 0;
static int      s_heatmap_max_frame = -1;

static uint32_t get_heatmap_max ( void )
{
    /* Per-frame cache - recompute jen jednou per frame. ImGui frame count
     * je dostupný přes GetFrameCount. */
    int cur_frame = ImGui::GetFrameCount ( );
    if ( s_heatmap_max_frame == cur_frame ) return s_heatmap_max_cache;

    uint32_t mx = 0;
    for ( unsigned i = 0; i < MHMAP_SIZE_BUS; i++ ) {
        const st_MHMAP_CELL *c = &g_mhmap.bus[i];
        uint32_t total = c->r + c->w + c->x + c->s;
        if ( total > mx ) mx = total;
    }
    s_heatmap_max_cache = mx;
    s_heatmap_max_frame = cur_frame;
    return mx;
}
#endif


/* ============================================================================
 *  Public API
 * ============================================================================
 */

extern "C" bool membrowser_layers_any_color_active ( const st_MEMBROWSER_STATE *st )
{
    if ( !st ) return false;
    return st->layer_cdl_x || st->layer_cdl_r || st->layer_cdl_w
           || st->layer_heatmap || st->layer_snapshot_delta || st->layer_frozen;
}


extern "C" uint32_t membrowser_layers_compute_byte_bg (
        const st_MEMBROWSER_STATE *st,
        int region_id,
        int region_kind, int sub_id,
        uint32_t offset,
        uint32_t logical_base )
{
    ( void ) logical_base;

    if ( !st ) return 0;

    /* 1. Frozen - priorita nejvyšší. */
    if ( st->layer_frozen ) {
        if ( freeze_is_frozen ( region_kind, sub_id, offset, NULL ) ) {
            return c_bg_frozen;
        }
    }

    /* 2. Snapshot Δ - per region_id. */
    if ( st->layer_snapshot_delta ) {
        uint8_t snap_byte = 0;
        if ( membrowser_snapshot_byte ( region_id, offset, &snap_byte ) ) {
            uint8_t cur_byte = 0;
            int got = dbgapi_regions_read ( region_id, offset, &cur_byte, 1 );
            if ( got == 1 && cur_byte != snap_byte ) {
                return c_bg_snapshot_delta;
            }
        }
    }

#if ( MZARCH == 800 ) || ( MZARCH == 1500 ) || ( MZARCH == 700 )
    /* CDL/Heatmap - vyžaduje mhmap cell. */
    const st_MHMAP_CELL *cell = NULL;
    if ( st->layer_cdl_x || st->layer_cdl_r || st->layer_cdl_w || st->layer_heatmap ) {
        cell = resolve_mhmap_cell ( region_kind, sub_id, offset );
    }
    if ( cell ) {
        /* 3. CDL X (execute). */
        if ( st->layer_cdl_x && cell->x > 0 ) return c_bg_cdl_x;
        /* 4. CDL R (data read). */
        if ( st->layer_cdl_r && cell->r > 0 ) return c_bg_cdl_r;
        /* 5. CDL W (data write). */
        if ( st->layer_cdl_w && ( cell->w > 0 || cell->s > 0 ) ) return c_bg_cdl_w;
        /* 6. Heatmap - gradient cool (blue) -> hot (red) podle log normalizace. */
        if ( st->layer_heatmap ) {
            uint32_t total = cell->r + cell->w + cell->x + cell->s;
            if ( total > 0 ) {
                uint32_t mx = get_heatmap_max ( );
                if ( mx > 0 ) {
                    float t = std::log ( ( float ) total + 1.0f )
                              / std::log ( ( float ) mx + 1.0f );
                    if ( t < 0.0f ) t = 0.0f;
                    if ( t > 1.0f ) t = 1.0f;
                    /* Cool -> hot blend. */
                    int r = ( int ) ( 50.0f + t * 180.0f );
                    int g = ( int ) ( 80.0f + ( 1.0f - t ) * 80.0f );
                    int b = ( int ) ( 180.0f * ( 1.0f - t ) + 30.0f );
                    return IM_COL32 ( r, g, b, 110 );
                }
            }
        }
    }
#else
    ( void ) region_id;
    ( void ) region_kind;
    ( void ) sub_id;
    ( void ) offset;
#endif

    return 0;
}


extern "C" void membrowser_layers_render_toolbar_toggle ( st_MEMBROWSER_STATE *st )
{
    if ( !st ) return;
    const char *label = st->layers_panel_open
        ? _L ( "Layers \xE2\x96\xBC###mb_layers_toggle" )   /* "Layers ▼" */
        : _L ( "Layers \xE2\x96\xB6###mb_layers_toggle" );  /* "Layers ▶" */
    if ( ImGui::Button ( label ) ) {
        st->layers_panel_open = !st->layers_panel_open;
        membrowser_state_save_to_persisted ( st );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Toggle Layers side panel "
                                      "(color overlays, snapshot, freeze)" ) );
    }
}


extern "C" void membrowser_layers_render_panel ( st_MEMBROWSER_STATE *st,
                                                  int current_region_id )
{
    if ( !st || !st->layers_panel_open ) return;

    ImGui::TextUnformatted ( _( "Layers" ) );
    ImGui::Separator ( );

    /* Status badge - MH/CDL recording stav. Zelená "Recording" pokud aktivní,
     * oranžová "Recording OFF" pokud ne. Hint user kde to zapnout. */
    bool mh_active = layers_mhmap_recording_active ( );
    if ( mh_active ) {
        ImGui::TextColored ( ImVec4 ( 0.30f, 0.85f, 0.30f, 1.0f ),
                             "%s", _( "* MH/CDL recording ON" ) );
    } else {
        ImGui::TextColored ( ImVec4 ( 1.00f, 0.60f, 0.20f, 1.0f ),
                             "%s", _( "o MH/CDL recording OFF" ) );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 28.0f );
        ImGui::TextUnformatted ( _( "Memory Heatmap / CDL recording state. "
                                     "Enable in Memory Heatmap window "
                                     "(Debugger -> Memory Heatmap). "
                                     "Without recording, CDL X/R/W and Heatmap "
                                     "layers stay invisible." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    }
    ImGui::Separator ( );

    bool changed = false;

    if ( ImGui::Checkbox ( _L ( "CDL Code (X)###mb_layer_cdl_x" ),
                            &st->layer_cdl_x ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 28.0f );
        ImGui::TextUnformatted ( _( "Highlight bytes executed as instructions" ) );
        ImGui::Spacing ( );
        ImGui::TextUnformatted ( _( "Requires MH/CDL recording active "
                                    "(enable in Memory Heatmap window)." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    }

    if ( ImGui::Checkbox ( _L ( "CDL Data Read (R)###mb_layer_cdl_r" ),
                            &st->layer_cdl_r ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 28.0f );
        ImGui::TextUnformatted ( _( "Highlight bytes read as data" ) );
        ImGui::Spacing ( );
        ImGui::TextUnformatted ( _( "Requires MH/CDL recording active "
                                    "(enable in Memory Heatmap window)." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    }

    if ( ImGui::Checkbox ( _L ( "CDL Data Write (W)###mb_layer_cdl_w" ),
                            &st->layer_cdl_w ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 28.0f );
        ImGui::TextUnformatted ( _( "Highlight bytes written by CPU" ) );
        ImGui::Spacing ( );
        ImGui::TextUnformatted ( _( "Requires MH/CDL recording active "
                                    "(enable in Memory Heatmap window)." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    }

    if ( ImGui::Checkbox ( _L ( "Heatmap###mb_layer_heatmap" ),
                            &st->layer_heatmap ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::PushTextWrapPos ( ImGui::GetFontSize ( ) * 28.0f );
        ImGui::TextUnformatted ( _( "Color by access count (cool -> hot)" ) );
        ImGui::Spacing ( );
        ImGui::TextUnformatted ( _( "Requires MH/CDL recording active "
                                    "(enable in Memory Heatmap window)." ) );
        ImGui::PopTextWrapPos ( );
        ImGui::EndTooltip ( );
    }

    ImGui::Separator ( );

    if ( ImGui::Checkbox ( _L ( "Snapshot \xCE\x94###mb_layer_delta" ),    /* "Snapshot Δ" */
                            &st->layer_snapshot_delta ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Highlight bytes changed since snapshot" ) );
    }

    /* Snapshot tlačítka. */
    bool has_snap = membrowser_snapshot_has ( current_region_id );
    bool snap_dis = ( current_region_id < 0 );
    if ( snap_dis ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L ( "Snapshot Now###mb_snap_take" ) ) ) {
        membrowser_snapshot_take ( current_region_id );
    }
    if ( snap_dis ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Capture current region content as snapshot baseline" ) );
    }

    ImGui::SameLine ( );
    bool reset_dis = !has_snap;
    if ( reset_dis ) ImGui::BeginDisabled ( );
    if ( ImGui::Button ( _L ( "Reset###mb_snap_reset" ) ) ) {
        membrowser_snapshot_reset ( current_region_id );
    }
    if ( reset_dis ) ImGui::EndDisabled ( );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Discard snapshot for current region" ) );
    }

    if ( has_snap ) {
        uint32_t sz = membrowser_snapshot_size ( current_region_id );
        ImGui::TextDisabled ( _( "Snapshot: %u B" ), ( unsigned ) sz );
    } else {
        ImGui::TextDisabled ( "%s", _( "No snapshot" ) );
    }

    ImGui::Separator ( );

    if ( ImGui::Checkbox ( _L ( "Frozen Bytes###mb_layer_frozen" ),
                            &st->layer_frozen ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Highlight frozen bytes (cheat engine style)" ) );
    }
    ImGui::TextDisabled ( _( "Frozen: %u" ), ( unsigned ) freeze_count ( ) );

    ImGui::Separator ( );

    if ( ImGui::Checkbox ( _L ( "Symbols###mb_layer_symbols" ),
                            &st->layer_symbols ) ) changed = true;
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s", _( "Show symbol labels in ASCII column "
                                      "(Logical region only)" ) );
    }

    if ( changed ) {
        membrowser_state_save_to_persisted ( st );
    }
}


extern "C" int membrowser_layers_compute_optimal_panel_width ( void )
{
    /* Auto-compute optimal width = max(CalcTextSize(label)) přes všechny
     * vykreslované labely + reservation pro checkbox glyph, scrollbar
     * a border padding. Hodnoty se musí počítat za běhu kvůli lokalizaci
     * - delší jazyky (CZ/DE) potřebují širší panel.
     *
     * Vykreslované texty:
     *   - sekce header _("Layers")
     *   - 7 checkbox labels (CDL X/R/W, Heatmap, Snapshot Δ, Frozen, Symbols)
     *   - 2 tlačítka (Snapshot Now, Reset)
     *   - 2 info řádky (Snapshot: %u B, Frozen: %u)
     *
     * Reservation:
     *   - Checkbox: ~20 px glyph + 4 px gap (ItemSpacing.x default)
     *   - Border padding: 2 * WindowPadding.x (~8 px each)
     *   - Scrollbar reserve: 14 px (pro případ že obsah překročí výšku)
     *   - Bezpečnostní buffer: 8 px
     * Suma reservation ~ 60 px. */
    static const char *labels[] = {
        N_( "Layers" ),
        N_( "CDL Code (X)" ),
        N_( "CDL Data Read (R)" ),
        N_( "CDL Data Write (W)" ),
        N_( "Heatmap" ),
        N_( "Snapshot \xCE\x94" ),       /* "Snapshot Δ" */
        N_( "Snapshot Now" ),
        N_( "Reset" ),
        N_( "Frozen Bytes" ),
        N_( "Symbols" ),
        /* Format strings - aproximace expanded šířky po sprintf. */
        N_( "Snapshot: 16777216 B" ),
        N_( "Frozen: 99999" ),
        N_( "No snapshot" ),
    };

    float max_w = 0.0f;
    int n = ( int ) ( sizeof ( labels ) / sizeof ( labels[0] ) );
    for ( int i = 0; i < n; i++ ) {
        /* N_() je jen marker; pro CalcTextSize potřebujeme reálný překlad
         * skrz _() = gettext(). */
        const char *txt = _( labels[i] );
        float w = ImGui::CalcTextSize ( txt ).x;
        if ( w > max_w ) max_w = w;
    }

    /* Reservation pro checkbox + border + safety. */
    const float reservation = 60.0f;
    int width = ( int ) ( max_w + reservation );

    /* Clamp na povolený rozsah 200..600 px. */
    if ( width < 200 ) width = 200;
    if ( width > 600 ) width = 600;
    return width;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
