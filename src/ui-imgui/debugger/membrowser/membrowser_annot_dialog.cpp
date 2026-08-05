/**
 * @file membrowser_annot_dialog.cpp
 * @brief Annotation dialog + auto-load/save - implementace.
 *
 * Modal "Edit annotation" s text editor, color picker (3 fixed swatches +
 * custom), Save / Delete / Cancel.
 *
 * Persist file: relative path "membrowser_annotations.txt" v current
 * working directory. Auto-load při startu, auto-save při shutdown.
 *
 * Tooltip rendering: jednoduchý ImGui::SetTooltip volaný callerem.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_annot_dialog.h"
#include "membrowser_annotations.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <cstdint>


/* Pending request - z context menu se nastavuje, modal render ho spotřebuje. */
static bool      s_pending_open = false;
static int       s_pending_region_kind = 0;
static int       s_pending_region_sub_id = 0;
static uint32_t  s_pending_addr = 0;

/* Stav modálu. */
static int       s_dialog_region_kind = 0;
static int       s_dialog_region_sub_id = 0;
static uint32_t  s_dialog_addr = 0;
static char      s_dialog_text[MB_ANNOT_TEXT_MAX] = "";
static float     s_dialog_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

/* Persist filename - relative k CWD. */
static const char *s_persist_filename = "membrowser_annotations.txt";


/**
 * @brief Konverze RGBA8 (0xRRGGBBAA) -> ImGui float[4] (0..1 per channel).
 */
static void rgba_u32_to_f4 ( uint32_t rgba, float out[4] )
{
    out[0] = ( ( rgba >> 24 ) & 0xFF ) / 255.0f;
    out[1] = ( ( rgba >> 16 ) & 0xFF ) / 255.0f;
    out[2] = ( ( rgba >>  8 ) & 0xFF ) / 255.0f;
    out[3] = ( ( rgba >>  0 ) & 0xFF ) / 255.0f;
}


/**
 * @brief Konverze ImGui float[4] -> RGBA8 (0xRRGGBBAA).
 */
static uint32_t rgba_f4_to_u32 ( const float in[4] )
{
    auto clamp = [] ( float v ) -> uint32_t {
        if ( v < 0.0f ) v = 0.0f;
        if ( v > 1.0f ) v = 1.0f;
        return ( uint32_t ) ( v * 255.0f + 0.5f );
    };
    return ( clamp ( in[0] ) << 24 )
         | ( clamp ( in[1] ) << 16 )
         | ( clamp ( in[2] ) <<  8 )
         | ( clamp ( in[3] ) <<  0 );
}


extern "C" void membrowser_annot_dialog_request_open ( int region_kind,
                                                        int region_sub_id,
                                                        uint32_t addr )
{
    s_pending_open = true;
    s_pending_region_kind = region_kind;
    s_pending_region_sub_id = region_sub_id;
    s_pending_addr = addr;
}


extern "C" void membrowser_annot_dialog_render ( void )
{
    if ( s_pending_open ) {
        s_dialog_region_kind = s_pending_region_kind;
        s_dialog_region_sub_id = s_pending_region_sub_id;
        s_dialog_addr = s_pending_addr;

        /* Pokud annotace už existuje, pre-fill text + color. */
        const st_MB_ANNOTATION *ex = membrowser_annotations_find (
            s_dialog_region_kind, s_dialog_region_sub_id, s_dialog_addr );
        if ( ex ) {
            std::strncpy ( s_dialog_text, ex->text, sizeof ( s_dialog_text ) - 1 );
            s_dialog_text[sizeof ( s_dialog_text ) - 1] = '\0';
            rgba_u32_to_f4 ( ex->color_rgba, s_dialog_color );
        } else {
            s_dialog_text[0] = '\0';
            /* Default lehce žlutá - viditelné ale neagresivní. */
            s_dialog_color[0] = 1.0f;
            s_dialog_color[1] = 0.85f;
            s_dialog_color[2] = 0.30f;
            s_dialog_color[3] = 1.0f;
        }
        s_pending_open = false;
        ImGui::OpenPopup ( "###mb_annot_dialog" );
    }

    char title[96];
    std::snprintf ( title, sizeof ( title ),
                    "%s###mb_annot_dialog", _( "Edit annotation" ) );

    ImVec2 center = ImGui::GetMainViewport ( )->GetCenter ( );
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( !ImGui::BeginPopupModal ( title, NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        return;
    }

    ImGui::Text ( _( "Region kind: %d  Sub-ID: %d  Addr: 0x%08X" ),
                  s_dialog_region_kind, s_dialog_region_sub_id,
                  ( unsigned ) s_dialog_addr );
    ImGui::Separator ( );

    ImGui::InputTextMultiline ( _L( "Text" ), s_dialog_text,
                                  sizeof ( s_dialog_text ),
                                  ImVec2 ( 360.0f, 100.0f ) );

    ImGui::ColorEdit4 ( _L( "Color tag" ), s_dialog_color,
                         ImGuiColorEditFlags_NoInputs );

    ImGui::Separator ( );

    bool clicked_save = ImGui::Button ( _L( "Save" ), ImVec2 ( 100, 0 ) );
    ImGui::SameLine ( );
    bool clicked_delete = ImGui::Button ( _L( "Delete" ), ImVec2 ( 100, 0 ) );
    ImGui::SameLine ( );
    bool clicked_cancel = ImGui::Button ( _L( "Cancel" ), ImVec2 ( 100, 0 ) );

    if ( clicked_save ) {
        uint32_t rgba = rgba_f4_to_u32 ( s_dialog_color );
        membrowser_annotations_set ( s_dialog_region_kind,
                                       s_dialog_region_sub_id,
                                       s_dialog_addr,
                                       s_dialog_text, rgba );
        /* Hned auto-save aby annotation přežila crash. */
        membrowser_annotations_save_to_file ( s_persist_filename );
        ImGui::CloseCurrentPopup ( );
    } else if ( clicked_delete ) {
        membrowser_annotations_remove ( s_dialog_region_kind,
                                          s_dialog_region_sub_id,
                                          s_dialog_addr );
        membrowser_annotations_save_to_file ( s_persist_filename );
        ImGui::CloseCurrentPopup ( );
    } else if ( clicked_cancel ) {
        ImGui::CloseCurrentPopup ( );
    }

    ImGui::EndPopup ( );
}


extern "C" void membrowser_annot_dialog_render_tooltip ( int region_kind,
                                                          int region_sub_id,
                                                          uint32_t addr )
{
    const st_MB_ANNOTATION *a = membrowser_annotations_find ( region_kind,
                                                                region_sub_id,
                                                                addr );
    if ( !a ) return;

    float col[4];
    rgba_u32_to_f4 ( a->color_rgba, col );

    ImGui::BeginTooltip ( );
    /* Color swatch + addr header. */
    ImGui::ColorButton ( "##mb_annot_color_swatch",
                          ImVec4 ( col[0], col[1], col[2], col[3] ),
                          ImGuiColorEditFlags_NoTooltip,
                          ImVec2 ( 12, 12 ) );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Annotation @ 0x%08X" ), ( unsigned ) addr );
    ImGui::Separator ( );
    ImGui::TextWrapped ( "%s", a->text );
    ImGui::EndTooltip ( );
}


extern "C" void membrowser_annot_dialog_load_on_startup ( void )
{
    membrowser_annotations_load_from_file ( s_persist_filename );
}


extern "C" void membrowser_annot_dialog_save_on_shutdown ( void )
{
    membrowser_annotations_save_to_file ( s_persist_filename );
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
