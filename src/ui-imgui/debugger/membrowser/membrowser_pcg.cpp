/**
 * @file membrowser_pcg.cpp
 * @brief PCG glyph editor - implementace.
 *
 * Reads/writes přes membrowser_io backend nad REGION_KIND_PCG_1500.
 * Bank index = region sub_id 0..2.
 *
 * Editor layout:
 *   [Bank: combo 1/2/3]  [Char #: input 0..255]  [< >]
 *   [Inverse] [Mirror H] [Mirror V] [Rotate 90 CW]
 *   +-------------------+
 *   |  8x8 grid editor  |  (cell size ~32 px, click toggle pixel)
 *   +-------------------+
 *   Raw bytes: B0 B1 ... B7 (hex display read-only)
 *
 * Pixel mapping (per existing emu konvence - viz vram_sim PCG):
 *   row 0..7 = byte[row]
 *   bit 7 = leftmost pixel, bit 0 = rightmost
 *
 * Edit dispatch: čte 8 bajtů PCG charu z backendu, modifikuje lokální
 * kopii dle akce, zapíše zpět všech 8 bajtů. Žádné per-pixel write
 * (jednodušší a deterministické).
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_pcg.h"
#include "membrowser_io.h"
#include "hex_view_backend.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
}


/* State okna - drží se mezi frame. */
static int s_bank_idx = 0;     /* 0..2 */
static int s_char_idx = 0;     /* 0..255 */
static bool s_focus_requested = false;
static int s_pending_bank = 0;
static int s_pending_char = 0;


extern "C" void membrowser_pcg_window_focus_at ( int bank_idx, int char_idx )
{
    if ( bank_idx < 0 || bank_idx > 2 ) bank_idx = 0;
    if ( char_idx < 0 || char_idx > 255 ) char_idx = 0;
    s_pending_bank = bank_idx;
    s_pending_char = char_idx;
    s_focus_requested = true;
}


/**
 * @brief Zjisti region_id pro daný PCG bank index.
 *
 * @return region_id (>=0) nebo -1 pokud nedostupný (= nejsme na MZ-1500
 *         nebo region snapshot prázdný).
 */
static int find_pcg_region_id ( int bank_idx )
{
    st_MEMBROWSER_REGION_SNAPSHOT snap;
    std::memset ( &snap, 0, sizeof ( snap ) );
    membrowser_io_refresh_regions ( &snap );
    return membrowser_io_find_region ( &snap, REGION_KIND_PCG_1500, bank_idx );
}


/**
 * @brief Načte 8 bajtů glyfu do bufferu (0 pokud selže).
 */
static void read_glyph ( int bank_idx, int char_idx, uint8_t out[8] )
{
    std::memset ( out, 0, 8 );
    int region_id = find_pcg_region_id ( bank_idx );
    if ( region_id < 0 ) return;
    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, region_id, true );
    if ( !be.read_bytes ) return;
    be.read_bytes ( be.ctx, ( uint64_t ) ( char_idx * 8 ), out, 8 );
}


/**
 * @brief Zapíše 8 bajtů glyfu.
 *
 * @return true při úspěchu.
 */
static bool write_glyph ( int bank_idx, int char_idx, const uint8_t in[8] )
{
    int region_id = find_pcg_region_id ( bank_idx );
    if ( region_id < 0 ) return false;
    st_HEX_VIEW_BACKEND be;
    membrowser_io_make_emu_backend ( &be, region_id, true );
    if ( !be.write_bytes ) return false;
    int wr = be.write_bytes ( be.ctx, ( uint64_t ) ( char_idx * 8 ), in, 8 );
    return ( wr == 8 );
}


/**
 * @brief Operace: inverse (XOR 0xFF na všech 8 bajtech).
 */
static void op_inverse ( uint8_t g[8] )
{
    for ( int i = 0; i < 8; i++ ) g[i] = ~g[i];
}


/**
 * @brief Operace: mirror horizontal (bit reverse per řádek).
 */
static void op_mirror_h ( uint8_t g[8] )
{
    for ( int i = 0; i < 8; i++ ) {
        uint8_t b = g[i];
        uint8_t r = 0;
        for ( int k = 0; k < 8; k++ ) {
            if ( b & ( 1 << k ) ) r |= ( 1 << ( 7 - k ) );
        }
        g[i] = r;
    }
}


/**
 * @brief Operace: mirror vertical (řádek 0<->7, 1<->6, ...).
 */
static void op_mirror_v ( uint8_t g[8] )
{
    for ( int i = 0; i < 4; i++ ) {
        uint8_t t = g[i];
        g[i] = g[7 - i];
        g[7 - i] = t;
    }
}


/**
 * @brief Operace: rotate 90 CW.
 *
 * Pixel (row, col) -> (col, 7-row). Vstupní bity bereme z g[row] s
 * pozicí (7-col); výstupní zapisujeme do nového bufferu.
 */
static void op_rotate_90 ( uint8_t g[8] )
{
    uint8_t out[8] = { 0 };
    for ( int row = 0; row < 8; row++ ) {
        for ( int col = 0; col < 8; col++ ) {
            bool px = ( g[row] >> ( 7 - col ) ) & 1;
            if ( px ) {
                int new_row = col;
                int new_col = 7 - row;
                out[new_row] |= ( 1 << ( 7 - new_col ) );
            }
        }
    }
    std::memcpy ( g, out, 8 );
}


extern "C" void membrowser_pcg_window_render ( bool *p_open )
{
    if ( !p_open || !*p_open ) return;

    /* Apply pending focus z external API. */
    if ( s_focus_requested ) {
        s_bank_idx = s_pending_bank;
        s_char_idx = s_pending_char;
        s_focus_requested = false;
        ImGui::SetNextWindowFocus ( );
    }

    ImGui::SetNextWindowSize ( ImVec2 ( 460, 480 ), ImGuiCond_FirstUseEver );

    char title[96];
    std::snprintf ( title, sizeof ( title ),
                    "%s###mb_pcg_editor", _( "PCG editor (MZ-1500)" ) );

    if ( !ImGui::Begin ( title, p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    }

    /* Bank combo - 3 banky. */
    const char *bank_names[3] = { "Bank 1", "Bank 2", "Bank 3" };
    if ( ImGui::BeginCombo ( _L( "Bank" ), bank_names[s_bank_idx] ) ) {
        for ( int i = 0; i < 3; i++ ) {
            bool sel = ( i == s_bank_idx );
            if ( ImGui::Selectable ( bank_names[i], sel ) ) {
                s_bank_idx = i;
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }

    /* Char index - InputInt + < > arrows. */
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 80.0f );
    ImGui::InputInt ( _L( "Char #" ), &s_char_idx );
    if ( s_char_idx < 0 ) s_char_idx = 0;
    if ( s_char_idx > 255 ) s_char_idx = 255;

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "<" ) ) && s_char_idx > 0 ) s_char_idx--;
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( ">" ) ) && s_char_idx < 255 ) s_char_idx++;

    /* Region availability check. */
    int region_id = find_pcg_region_id ( s_bank_idx );
    if ( region_id < 0 ) {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.5f, 0.5f, 1.0f ),
            "%s", _( "PCG region not available - emulator is not MZ-1500" ) );
        ImGui::End ( );
        return;
    }

    /* Načti glyph data per frame (= reflect external edits). */
    uint8_t g[8];
    read_glyph ( s_bank_idx, s_char_idx, g );

    /* Operations row. */
    ImGui::Separator ( );
    bool changed = false;
    if ( ImGui::Button ( _L( "Inverse" ) ) )    { op_inverse ( g ); changed = true; }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Mirror H" ) ) )   { op_mirror_h ( g ); changed = true; }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Mirror V" ) ) )   { op_mirror_v ( g ); changed = true; }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Rotate 90 CW" ) ) ) { op_rotate_90 ( g ); changed = true; }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Clear" ) ) )      { std::memset ( g, 0, 8 ); changed = true; }
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L( "Fill" ) ) )       { std::memset ( g, 0xFF, 8 ); changed = true; }

    /* 8x8 grid editor. Cell size 32 px, frame 1 px. */
    ImGui::Separator ( );
    const float cell = 32.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList ( );
    ImVec2 origin = ImGui::GetCursorScreenPos ( );

    /* Render všech 64 cells + hit-test pro click. */
    for ( int row = 0; row < 8; row++ ) {
        for ( int col = 0; col < 8; col++ ) {
            float x0 = origin.x + col * cell;
            float y0 = origin.y + row * cell;
            float x1 = x0 + cell - 1;
            float y1 = y0 + cell - 1;
            bool px = ( g[row] >> ( 7 - col ) ) & 1;
            ImU32 fill_col = px
                ? IM_COL32 ( 240, 240, 80, 255 )       /* žlutá = pixel ON */
                : IM_COL32 ( 30, 30, 30, 255 );        /* tmavé pozadí */
            ImU32 border_col = IM_COL32 ( 90, 90, 90, 255 );
            dl->AddRectFilled ( ImVec2 ( x0, y0 ), ImVec2 ( x1, y1 ), fill_col );
            dl->AddRect ( ImVec2 ( x0, y0 ), ImVec2 ( x1, y1 ), border_col );
        }
    }

    /* Hit-test - jeden invisible button přes celou grid plochu, pak compute
     * cell index z mouse pos relative k origin. */
    ImGui::InvisibleButton ( "##mb_pcg_grid",
                              ImVec2 ( cell * 8, cell * 8 ) );
    if ( ImGui::IsItemHovered ( ) && ImGui::IsMouseClicked ( 0 ) ) {
        ImVec2 mp = ImGui::GetMousePos ( );
        int col = ( int ) ( ( mp.x - origin.x ) / cell );
        int row = ( int ) ( ( mp.y - origin.y ) / cell );
        if ( col >= 0 && col < 8 && row >= 0 && row < 8 ) {
            g[row] ^= ( 1 << ( 7 - col ) );
            changed = true;
        }
    }

    /* Raw bytes display - read-only hex line. */
    ImGui::Separator ( );
    ImGui::Text ( _( "Raw: %02X %02X %02X %02X %02X %02X %02X %02X" ),
                  g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7] );

    /* Status: addr range v PCG banku. */
    ImGui::TextDisabled ( _( "Bank addr range: 0x%04X..0x%04X" ),
                          ( unsigned ) ( s_char_idx * 8 ),
                          ( unsigned ) ( s_char_idx * 8 + 7 ) );

    if ( changed ) {
        write_glyph ( s_bank_idx, s_char_idx, g );
    }

    ImGui::End ( );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
