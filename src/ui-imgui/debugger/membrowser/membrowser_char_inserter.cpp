/**
 * @file membrowser_char_inserter.cpp
 * @brief Char Inserter okno - implementace.
 *
 * Layout:
 *   +--------------------------------------------------+
 *   | Target: MB #N @ 0xNNNN  Region: ...  [Edit: ON] |
 *   +--------------------------------------------------+
 *   | [ASCII EU] [ASCII JP] [KOI8-CS]                 |
 *   |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+              |
 *   |  | A | B | C | ... 16 cellů ...                  |
 *   |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+              |
 *   |  | ...                                          |
 *   |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+              |
 *   |  ... 16 řádků (256 cellů celkem) ...            |
 *   +--------------------------------------------------+
 *
 * Per tab encoding:
 *   - ASCII EU -> MB_CHARSET_SHARPMZ_EU_UTF8
 *   - ASCII JP -> MB_CHARSET_SHARPMZ_JP_UTF8
 *   - KOI8-CS  -> MB_CHARSET_KOI8CS
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_char_inserter.h"
#include "membrowser_encoding.h"
#include "membrowser_state.h"
#include "membrowser_window.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include <cstdio>
#include <cstring>

/* V4.1+: target mode - kam emitnout byte při click na cell:
 *   - CI_TARGET_MB_CURSOR (default): write na cursor pos MB instance přes
 *     membrowser_window_write_byte_at_cursor() (= existující chování)
 *   - CI_TARGET_CALLBACK: volá s_target_callback(byte, s_target_userdata)
 *     (= nový pattern build mód pro search pattern builder a další)
 *
 * Mode přepíná opening API: _open(idx) → MB_CURSOR; _open_for_callback() →
 * CALLBACK. Default při startu = MB_CURSOR (= zachování dosavadní UX). */
typedef enum en_CI_TARGET_MODE
{
    CI_TARGET_MB_CURSOR = 0,
    CI_TARGET_CALLBACK = 1
} en_CI_TARGET_MODE;

static int s_target_mode = CI_TARGET_MB_CURSOR;

/* Zdrojová MB instance pro write akce v MB_CURSOR módu. -1 = no target. */
static int s_source_instance_idx = 0;

/* Callback + userdata + label pro CALLBACK mód. */
static membrowser_char_emit_cb_t s_target_callback = NULL;
static void *s_target_userdata = NULL;
static char s_target_label[96] = { 0 };

/* Index aktivního tabu - persistuje mezi otevřeními session.
 *   0 = ASCII EU, 1 = ASCII JP, 2 = KOI8-CS. */
static int s_active_tab = 0;

/* Per-tab encoding mapping. Pole je in-order podle s_active_tab indexu.
 *
 * EU/JP taby používají CG1 variantu = MZ ASCII byte projde přes
 * mz_vcode_from_ascii_dump() do MZ vkódu, ten přes mzglyphs_to_utf8_buf()
 * na PUA codepoint U+E100-E4FF. Monospace font má merge mzglyphs.ttf pro
 * tento PUA range, takže glyf se vyrenderuje skutečnou bitmapou z MZ
 * CG-ROM (= jak by vypadal na obrazovce v 700/800 textovém režimu).
 *
 * KOI8-CS žádnou CG variantu nemá - je to CP/M Czech/Slovak encoding mimo
 * MZ CG-ROM. Renderuje se přes UTF-8 standardními glyfy default fontu. */
static const int CHAR_INSERTER_TAB_ENCODINGS[3] = {
    MB_CHARSET_SHARPMZ_EU_CG1,
    MB_CHARSET_SHARPMZ_JP_CG1,
    MB_CHARSET_KOI8CS
};

/* Stabilní anglické labely tabů. Lokalizace dělá _L() v render. */
static const char *const CHAR_INSERTER_TAB_LABELS[3] = {
    "ASCII EU###ci_tab_eu",
    "ASCII JP###ci_tab_jp",
    "KOI8-CS###ci_tab_koi"
};


extern "C" void membrowser_char_inserter_open ( int source_instance_idx )
{
    /* Reset na MB cursor mode (= existing behavior). */
    s_target_mode = CI_TARGET_MB_CURSOR;
    s_source_instance_idx = source_instance_idx;
    s_target_callback = NULL;
    s_target_userdata = NULL;
    if ( g_gui ) g_gui->showMembrowserCharInserter = true;
}


extern "C" void membrowser_char_inserter_open_for_callback (
    membrowser_char_emit_cb_t cb,
    void *userdata,
    const char *label )
{
    if ( !cb ) return;
    s_target_mode = CI_TARGET_CALLBACK;
    s_target_callback = cb;
    s_target_userdata = userdata;
    if ( label && label[0] ) {
        std::snprintf ( s_target_label, sizeof ( s_target_label ), "%s", label );
    } else {
        std::snprintf ( s_target_label, sizeof ( s_target_label ), "%s",
                        "Pattern buffer" );
    }
    if ( g_gui ) g_gui->showMembrowserCharInserter = true;
}


extern "C" void membrowser_char_inserter_toggle ( void )
{
    if ( !g_gui ) return;
    g_gui->showMembrowserCharInserter = !g_gui->showMembrowserCharInserter;
}


/* Vykreslí 16x16 grid bytů pro daný encoding. Klik na cell zapíše byte
 * na cursor pos zdrojové MB instance přes
 * @c membrowser_window_write_byte_at_cursor. Pokud @p write_enabled = false,
 * cells jsou vizuálně dostupné (ukazují glyfy) ale neklikatelné -
 * BeginDisabled obal.
 *
 * Layout via ImGui::Table pro stabilní spacing; každý cell má fixed width
 * (~2 monospace chars + padding). */
static void render_byte_grid ( int encoding_id, bool write_enabled )
{
    const float cell_w = 24.0f;
    const float cell_h = ImGui::GetTextLineHeightWithSpacing ( ) + 4.0f;

    /* Záměrně NEPOUŽÍVÁME monospace font (myimgui_get_monospace_font):
     * Cousine-Regular.ttf v MZdata je stripped subset bez Latin Extended-A,
     * takže Czech diakritika (č, ď, ě, ŕ, ů, ĺ, ľ, ň) v KOI8-CS tabu by
     * fallbackovala na "?". Default DroidSans má Latin Extended-A
     * (range 0x0020-0x024F definovaný v myimgui.cpp:51-54) i merge
     * mzglyphs.ttf (PUA U+E000, E100-E4FF) - tj. EU/JP CG taby renderují
     * MZ CG-ROM bitmapy + KOI8-CS tab renderuje Czech diakritiku korektně.
     * Cell alignment v gridu drží ImGui::Table fixed-width sloupce. */

    if ( !write_enabled ) ImGui::BeginDisabled ( );

    /* Tabulka 17 sloupců: 1× hex label (00, 10, 20, ...) + 16 cellů.
     * Borders aby uživatel viděl mřížku. */
    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                            | ImGuiTableFlags_SizingFixedFit
                            | ImGuiTableFlags_NoHostExtendX;
    if ( !ImGui::BeginTable ( "##ci_grid", 17, flags ) ) {
        if ( !write_enabled ) ImGui::EndDisabled ( );
        return;
    }

    /* Záhlaví sloupců: prázdné rohové + 0..F. */
    ImGui::TableSetupColumn ( "##ci_hdr_row", ImGuiTableColumnFlags_WidthFixed, cell_w );
    for ( int c = 0; c < 16; c++ ) {
        char hdr[4];
        std::snprintf ( hdr, sizeof ( hdr ), "_%X", c );
        ImGui::TableSetupColumn ( hdr, ImGuiTableColumnFlags_WidthFixed, cell_w );
    }
    ImGui::TableHeadersRow ( );

    for ( int row = 0; row < 16; row++ ) {
        ImGui::TableNextRow ( 0, cell_h );
        /* První sloupec = hex prefix řádku "0_".."F_". */
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::TextDisabled ( "%X_", row );

        for ( int col = 0; col < 16; col++ ) {
            ImGui::TableSetColumnIndex ( col + 1 );
            uint8_t b = ( uint8_t ) ( ( row << 4 ) | col );
            const char *glyph = membrowser_encoding_byte_to_utf8 ( b, encoding_id );

            /* Selectable jako klikatelný cell (pattern z mzdisk). Stable
             * ID per byte. Click → write. */
            char id_buf[32];
            std::snprintf ( id_buf, sizeof ( id_buf ), "%s##ci_%02X",
                            glyph && glyph[0] ? glyph : ".", ( unsigned ) b );

            bool clicked = ImGui::Selectable ( id_buf, false,
                                                ImGuiSelectableFlags_None,
                                                ImVec2 ( cell_w, cell_h - 2.0f ) );
            if ( clicked && write_enabled ) {
                /* Mode-dependent dispatch. */
                if ( s_target_mode == CI_TARGET_CALLBACK ) {
                    if ( s_target_callback ) {
                        s_target_callback ( b, s_target_userdata );
                    }
                } else {
                    /* CI_TARGET_MB_CURSOR (default existing behavior). */
                    membrowser_window_write_byte_at_cursor (
                        s_source_instance_idx, b );
                }
            }
            if ( ImGui::IsItemHovered ( ) ) {
                ImGui::SetTooltip ( "0x%02X - %s", ( unsigned ) b,
                                     ( glyph && glyph[0] ) ? glyph : "." );
            }
        }
    }
    ImGui::EndTable ( );

    if ( !write_enabled ) ImGui::EndDisabled ( );
}


/* Status řádek nad tab bar. Ukazuje aktuální target instance + info
 * o cursoru / regionu / writable. Pokud žádný validní target, "No target". */
static void render_status_row ( bool *out_write_enabled )
{
    /* V4.1+: CALLBACK mode - status řádek ukazuje custom label místo MB
     * instance info. Click cell → call s_target_callback() místo write to
     * memory. Cells jsou enabled pokud callback nastaven. */
    if ( s_target_mode == CI_TARGET_CALLBACK ) {
        if ( s_target_callback ) {
            ImGui::Text ( "%s ", _( "Target:" ) );
            ImGui::SameLine ( );
            ImGui::TextColored ( ImVec4 ( 0.40f, 0.80f, 1.0f, 1.0f ),
                                  "%s", s_target_label );
            if ( out_write_enabled ) *out_write_enabled = true;
        } else {
            ImGui::TextColored ( ImVec4 ( 0.85f, 0.55f, 0.25f, 1.0f ),
                                  "%s",
                                  _( "No target callback set" ) );
            if ( out_write_enabled ) *out_write_enabled = false;
        }
        return;
    }

    /* CI_TARGET_MB_CURSOR mode (= existing behavior). */
    uint32_t cur = 0;
    bool writable = false;
    int enc = 0;
    char region_buf[96];
    region_buf[0] = '\0';

    bool ok = false;
    if ( s_source_instance_idx >= 0 ) {
        ok = membrowser_window_get_cursor_info ( s_source_instance_idx,
                                                  &cur, &writable, &enc,
                                                  region_buf,
                                                  ( unsigned long ) sizeof ( region_buf ) );
    }

    if ( !ok ) {
        ImGui::TextColored ( ImVec4 ( 0.85f, 0.55f, 0.25f, 1.0f ),
                              "%s", _( "No target: open a Memory Browser and right-click in hex/ASCII column" ) );
        if ( out_write_enabled ) *out_write_enabled = false;
        return;
    }

    /* Slot index 0 = "main", 1..4 = "#2".."#5". */
    char target_label[16];
    if ( s_source_instance_idx == 0 ) {
        std::snprintf ( target_label, sizeof ( target_label ), "main" );
    } else {
        std::snprintf ( target_label, sizeof ( target_label ), "#%d",
                        s_source_instance_idx + 1 );
    }

    ImGui::Text ( "%s ", _( "Target:" ) );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "MB %s", target_label );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "@" );
    ImGui::SameLine ( );
    ImGui::Text ( "0x%04X", ( unsigned ) cur );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "  %s", _( "Region:" ) );
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( region_buf[0] ? region_buf : "?" );
    ImGui::SameLine ( );
    if ( writable ) {
        ImGui::TextColored ( ImVec4 ( 0.30f, 0.85f, 0.30f, 1.0f ),
                              "  [%s]", _( "Edit ON" ) );
    } else {
        ImGui::TextColored ( ImVec4 ( 0.85f, 0.55f, 0.25f, 1.0f ),
                              "  [%s]", _( "Edit OFF / RO" ) );
    }
    if ( out_write_enabled ) *out_write_enabled = writable;
}


extern "C" void membrowser_char_inserter_render ( void )
{
    if ( !g_gui || !g_gui->showMembrowserCharInserter ) return;

    /* Default size pri prvním otevření. ImGui pak zachovává přes imgui.ini. */
    ImGui::SetNextWindowSize ( ImVec2 ( 540.0f, 580.0f ),
                                ImGuiCond_FirstUseEver );

    bool *p_open = &g_gui->showMembrowserCharInserter;
    /* ### stable ID nezávisí na lokalizovaném prefixu (dynamic title). */
    if ( !ImGui::Begin ( _L ( "Char Inserter###mb_char_inserter" ),
                          p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    }

    bool write_enabled = false;
    render_status_row ( &write_enabled );

    ImGui::Separator ( );

    if ( ImGui::BeginTabBar ( "##ci_tabs", ImGuiTabBarFlags_None ) ) {
        for ( int t = 0; t < 3; t++ ) {
            if ( ImGui::BeginTabItem ( _L ( CHAR_INSERTER_TAB_LABELS[t] ) ) ) {
                s_active_tab = t;
                render_byte_grid ( CHAR_INSERTER_TAB_ENCODINGS[t],
                                    write_enabled );
                ImGui::EndTabItem ( );
            }
        }
        ImGui::EndTabBar ( );
    }

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
