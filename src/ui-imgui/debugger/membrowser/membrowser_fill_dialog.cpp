/**
 * @file membrowser_fill_dialog.cpp
 * @brief Fill dialog ImGui modal + undo/redo dispatch - implementace.
 *
 * Pending request flow:
 *   1. RMB → context menu → "Fill at cursor" → request_open(kind, sub_id, addr)
 *   2. Při příštím membrowser_fill_dialog_render zavolá ImGui::OpenPopup
 *   3. Modal sbírá user vstup (mode combo, length, pattern, value, seed)
 *   4. OK button → membrowser_undo_push(read before) → membrowser_fill_apply
 *      → backend.write_bytes → status
 *
 * Undo/redo flow:
 *   1. Ctrl+Z → do_undo: pop undo → push redo (= current bytes) → write back
 *   2. Ctrl+Y → do_redo: symetrické (pop redo → push undo → write)
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_fill_dialog.h"
#include "membrowser_fill.h"
#include "membrowser_undo.h"
/* Edit semantics: po fill / undo / redo označit byte range jako
 * "recently edited" pro vizuální badge v hex view. */
#include "membrowser_edited.h"

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>


/* ------------------------------------------------------------------------- */
/* Pending request - z context menu se nastavuje, modal render ho spotřebuje. */

static bool      s_pending_open = false;
static int       s_pending_region_kind = 0;
static int       s_pending_region_sub_id = 0;
static uint64_t  s_pending_start_offset = 0;

/* Stav modálu (drží se mezi frame dokud uživatel modal nezavře). */
static bool      s_dialog_active = false;
static int       s_dialog_region_kind = 0;
static int       s_dialog_region_sub_id = 0;
static uint64_t  s_dialog_start_offset = 0;
static int       s_dialog_mode = MB_FILL_MODE_ZERO;
static int       s_dialog_length = 16;
static int       s_dialog_increment_start = 0;
static uint32_t  s_dialog_random_seed = 0;
static char      s_dialog_pattern_input[256] = "";

/* Poslední status hláška (lokalizovaný anglický string). */
static char      s_status_buf[160] = "";


/**
 * @brief Nastaví status hlášku z printf-style formátu.
 *
 * Bezpečné vůči přetečení; ukončuje řetězec '\0'.
 */
static void set_status ( const char *fmt, ... )
{
    va_list ap;
    va_start ( ap, fmt );
    std::vsnprintf ( s_status_buf, sizeof ( s_status_buf ), fmt, ap );
    va_end ( ap );
    s_status_buf[sizeof ( s_status_buf ) - 1] = '\0';
}


extern "C" void membrowser_fill_dialog_request_open ( int region_kind,
                                                       int region_sub_id,
                                                       uint64_t start_offset )
{
    s_pending_open = true;
    s_pending_region_kind = region_kind;
    s_pending_region_sub_id = region_sub_id;
    s_pending_start_offset = start_offset;
}


/**
 * @brief Vykoná samotnou fill operaci - undo snapshot + apply + write.
 *
 * @return 0 OK, jinak chybový kód (pro status zprávu).
 */
static int do_apply_fill ( const st_HEX_VIEW_BACKEND *be,
                            int region_kind, int region_sub_id,
                            uint64_t offset, uint32_t length,
                            const st_MB_FILL_PARAMS *params,
                            int instance_idx )
{
    if ( !be || !be->read_bytes ) {
        set_status ( "%s", _( "Fill failed: no backend" ) );
        return -1;
    }
    if ( !be->write_bytes ) {
        set_status ( "%s", _( "Fill failed: region is read-only or Edit toggle is OFF" ) );
        return -2;
    }
    if ( length == 0 ) {
        set_status ( "%s", _( "Fill failed: length is zero" ) );
        return -3;
    }

    uint8_t *before = ( uint8_t * ) std::malloc ( length );
    uint8_t *after = ( uint8_t * ) std::malloc ( length );
    if ( !before || !after ) {
        std::free ( before );
        std::free ( after );
        set_status ( "%s", _( "Fill failed: out of memory" ) );
        return -4;
    }

    int got = be->read_bytes ( be->ctx, offset, before, length );
    if ( got < 0 || ( uint32_t ) got != length ) {
        std::free ( before );
        std::free ( after );
        set_status ( "%s", _( "Fill failed: read before write returned short" ) );
        return -5;
    }

    int frv = membrowser_fill_apply ( params, after, length );
    if ( frv != 0 ) {
        std::free ( before );
        std::free ( after );
        set_status ( "%s", _( "Fill failed: invalid parameters" ) );
        return -6;
    }

    /* Undo push - před vlastním write. Pokud selže (too large), pokračujeme
     * ale uživatel se dozví, že undo nebude k dispozici. */
    int upr = membrowser_undo_push ( region_kind, region_sub_id,
                                      offset, before, length );

    int wrv = be->write_bytes ( be->ctx, offset, after, length );

    /* V1.5+: po úspěšném fill označit range jako recently edited s
     * originals=before (pre-fill hodnoty pro session-original tracking),
     * pak check_unmark s after (= bytes vyplněné fill pattern - pokud
     * shodou okolností odpovídají session-original, badge se hned odebere). */
    if ( wrv >= 0 && instance_idx >= 0 ) {
        membrowser_edited_mark_range ( instance_idx, region_kind, region_sub_id,
                                        ( uint32_t ) offset, length, before );
        membrowser_edited_check_unmark_range ( instance_idx, region_kind,
                                                region_sub_id,
                                                ( uint32_t ) offset, length,
                                                after );
    }

    std::free ( before );
    std::free ( after );

    if ( wrv < 0 ) {
        set_status ( "%s", _( "Fill failed: backend write returned error" ) );
        return -7;
    }

    if ( upr != 0 ) {
        set_status ( _( "Fill: %u bytes written (undo unavailable - too large)" ),
                     ( unsigned ) length );
    } else {
        set_status ( _( "Fill: %u bytes written, undo level available" ),
                     ( unsigned ) length );
    }
    return 0;
}


extern "C" void membrowser_fill_dialog_render ( const st_HEX_VIEW_BACKEND *be,
                                                  int instance_idx )
{
    /* Otevírání popupu (jednou per request). */
    if ( s_pending_open ) {
        s_dialog_active = true;
        s_dialog_region_kind = s_pending_region_kind;
        s_dialog_region_sub_id = s_pending_region_sub_id;
        s_dialog_start_offset = s_pending_start_offset;
        /* Reset rozumných defaults při každém otevření. */
        s_dialog_mode = MB_FILL_MODE_ZERO;
        s_dialog_length = 16;
        s_dialog_increment_start = 0;
        s_dialog_random_seed = ( uint32_t ) std::time ( NULL );
        s_dialog_pattern_input[0] = '\0';
        s_pending_open = false;
        ImGui::OpenPopup ( "###mb_fill_dialog" );
    }

    /* Modal popup s ### stable ID + dynamickým title. */
    char title[96];
    std::snprintf ( title, sizeof ( title ),
                    "%s###mb_fill_dialog", _( "Fill memory" ) );

    /* Center + auto size. */
    ImVec2 center = ImGui::GetMainViewport ( )->GetCenter ( );
    ImGui::SetNextWindowPos ( center, ImGuiCond_Appearing, ImVec2 ( 0.5f, 0.5f ) );

    if ( !ImGui::BeginPopupModal ( title, NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize ) ) {
        return;
    }

    /* Hlavička s region + start address (read-only info). */
    ImGui::Text ( _( "Region kind: %d  Sub-ID: %d" ),
                  s_dialog_region_kind, s_dialog_region_sub_id );
    ImGui::Text ( _( "Start offset: 0x%08X" ),
                  ( unsigned ) s_dialog_start_offset );
    ImGui::Separator ( );

    /* Mode combo. */
    const char *mode_label = _( membrowser_fill_mode_name ( s_dialog_mode ) );
    if ( ImGui::BeginCombo ( _L( "Mode" ), mode_label ) ) {
        for ( int i = 0; i < MB_FILL_MODE__COUNT; i++ ) {
            bool sel = ( i == s_dialog_mode );
            if ( ImGui::Selectable ( _( membrowser_fill_mode_name ( i ) ), sel ) ) {
                s_dialog_mode = i;
            }
            if ( sel ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }

    /* Length input - 1..0x7FFFFFFF. */
    ImGui::InputInt ( _L( "Length (bytes)" ), &s_dialog_length );
    if ( s_dialog_length < 1 ) s_dialog_length = 1;
    if ( s_dialog_length > ( int ) ( 16 * 1024 * 1024 ) ) {
        s_dialog_length = 16 * 1024 * 1024;
    }

    /* Mode-specific inputs. */
    if ( s_dialog_mode == MB_FILL_MODE_INCREMENT ) {
        ImGui::InputInt ( _L( "Start value (0-255)" ), &s_dialog_increment_start );
        if ( s_dialog_increment_start < 0 ) s_dialog_increment_start = 0;
        if ( s_dialog_increment_start > 255 ) s_dialog_increment_start = 255;
    } else if ( s_dialog_mode == MB_FILL_MODE_RANDOM ) {
        int seed_i = ( int ) s_dialog_random_seed;
        if ( ImGui::InputInt ( _L( "Random seed" ), &seed_i ) ) {
            s_dialog_random_seed = ( uint32_t ) seed_i;
        }
    } else if ( s_dialog_mode == MB_FILL_MODE_PATTERN ) {
        ImGui::InputText ( _L( "Pattern (hex)" ),
                            s_dialog_pattern_input,
                            sizeof ( s_dialog_pattern_input ) );
        ImGui::TextDisabled ( "%s",
            _( "Example: \"AA BB CC\" or \"DEADBEEF\" - max 64 bytes" ) );
    }

    ImGui::Separator ( );

    /* OK / Cancel buttons. */
    bool clicked_ok = ImGui::Button ( _L( "Apply" ), ImVec2 ( 120, 0 ) );
    ImGui::SameLine ( );
    bool clicked_cancel = ImGui::Button ( _L( "Cancel" ), ImVec2 ( 120, 0 ) );

    if ( clicked_ok ) {
        st_MB_FILL_PARAMS p;
        std::memset ( &p, 0, sizeof ( p ) );
        p.mode = s_dialog_mode;
        p.fixed_value = ( uint8_t ) ( s_dialog_increment_start & 0xFF );
        p.random_seed = s_dialog_random_seed;

        bool ok_to_apply = true;
        if ( s_dialog_mode == MB_FILL_MODE_PATTERN ) {
            int prv = membrowser_fill_parse_pattern (
                s_dialog_pattern_input,
                p.pattern,
                MB_FILL_PATTERN_MAX_BYTES,
                &p.pattern_len );
            if ( prv == -1 ) {
                set_status ( "%s", _( "Fill failed: pattern syntax error" ) );
                ok_to_apply = false;
            } else if ( prv == -2 ) {
                set_status ( "%s", _( "Fill failed: pattern too long (max 64 bytes)" ) );
                ok_to_apply = false;
            } else if ( p.pattern_len == 0 ) {
                set_status ( "%s", _( "Fill failed: pattern is empty" ) );
                ok_to_apply = false;
            }
        }

        if ( ok_to_apply ) {
            /* V1.5+: mark_range + check_unmark_range jsou nyní uvnitř
             * do_apply_fill (potřebují pre-fill `before` buffer pro originals
             * a post-fill `after` pro check_unmark). instance_idx předáváme. */
            ( void ) do_apply_fill ( be, s_dialog_region_kind,
                                      s_dialog_region_sub_id,
                                      s_dialog_start_offset,
                                      ( uint32_t ) s_dialog_length,
                                      &p, instance_idx );
        }
        s_dialog_active = false;
        ImGui::CloseCurrentPopup ( );
    } else if ( clicked_cancel ) {
        s_dialog_active = false;
        ImGui::CloseCurrentPopup ( );
    }

    ImGui::EndPopup ( );
}


extern "C" bool membrowser_fill_dialog_do_undo ( const st_HEX_VIEW_BACKEND *be,
                                                  int region_kind,
                                                  int region_sub_id,
                                                  int instance_idx )
{
    if ( !be || !be->write_bytes ) {
        set_status ( "%s", _( "Undo failed: region is read-only or Edit toggle is OFF" ) );
        return false;
    }
    st_MB_UNDO_ENTRY e;
    std::memset ( &e, 0, sizeof ( e ) );
    int pr = membrowser_undo_pop ( region_kind, region_sub_id, &e );
    if ( pr != 0 || e.bytes == NULL || e.len == 0 ) {
        set_status ( "%s", _( "Undo: nothing to undo" ) );
        return false;
    }

    /* Snapshot "current bytes" pro redo - read před write. */
    uint8_t *cur = ( uint8_t * ) std::malloc ( e.len );
    if ( cur ) {
        int rg = be->read_bytes ( be->ctx, e.offset, cur, e.len );
        if ( rg == ( int ) e.len ) {
            membrowser_redo_push ( region_kind, region_sub_id,
                                    e.offset, cur, e.len );
        }
        std::free ( cur );
    }

    /* Write undo bytes zpět. */
    int wr = be->write_bytes ( be->ctx, e.offset, e.bytes, e.len );

    if ( wr >= 0 && instance_idx >= 0 ) {
        /* V1.5+: Po úspěšném undo bytes v regionu == e.bytes (= post-undo
         * current state). Voláme check_unmark_range s těmito hodnotami -
         * pokud byte se vrátil na session-original (= stored original_byte
         * v edited entry), badge zmizí. Pokud byte stále liší od originálu
         * (= víc undo levels), badge zůstává.
         *
         * NEVOLÁME mark_range jako dříve - undo nemá důvod přidávat nové
         * marky pro byty, které nikdy nebyly editované (= entry neexistuje
         * → badge nepřibyde). Stávající entries zůstávají s jejich
         * uloženým original_byte. */
        membrowser_edited_check_unmark_range ( instance_idx, region_kind,
                                                region_sub_id,
                                                ( uint32_t ) e.offset, e.len,
                                                e.bytes );
    }

    membrowser_undo_free_entry ( &e );

    if ( wr < 0 ) {
        set_status ( "%s", _( "Undo failed: backend write returned error" ) );
        return false;
    }
    set_status ( "%s", _( "Undo: applied" ) );
    return true;
}


extern "C" bool membrowser_fill_dialog_do_redo ( const st_HEX_VIEW_BACKEND *be,
                                                  int region_kind,
                                                  int region_sub_id,
                                                  int instance_idx )
{
    if ( !be || !be->write_bytes ) {
        set_status ( "%s", _( "Redo failed: region is read-only or Edit toggle is OFF" ) );
        return false;
    }
    st_MB_UNDO_ENTRY e;
    std::memset ( &e, 0, sizeof ( e ) );
    int pr = membrowser_redo_pop ( region_kind, region_sub_id, &e );
    if ( pr != 0 || e.bytes == NULL || e.len == 0 ) {
        set_status ( "%s", _( "Redo: nothing to redo" ) );
        return false;
    }

    /* Snapshot "current" (= pre-redo) pro nový undo level + pro mark_range
     * originals (= "pre-redo" hodnoty, typicky session-original po předchozím
     * undo-to-original). */
    uint8_t *pre = ( uint8_t * ) std::malloc ( e.len );
    bool pre_ok = false;
    if ( pre ) {
        int rg = be->read_bytes ( be->ctx, e.offset, pre, e.len );
        if ( rg == ( int ) e.len ) {
            pre_ok = true;
            membrowser_undo_push ( region_kind, region_sub_id,
                                    e.offset, pre, e.len );
        }
    }

    int wr = be->write_bytes ( be->ctx, e.offset, e.bytes, e.len );

    if ( wr >= 0 && instance_idx >= 0 ) {
        /* V1.5+: po redo bytes == e.bytes (post-redo state, typicky
         * "post-edit" hodnoty z doby PŘED undo). Mark range s originals=pre
         * (= pre-redo hodnoty); pokud entry už existuje, original_byte z
         * předchozí mark v session zůstává. Pro nově přidané entries
         * (případ "undo-to-original-unmark + nový redo") se uloží pre-redo
         * jako fallback original.
         *
         * Pak check_unmark s e.bytes - kdyby post-redo náhodou odpovídal
         * uloženému originalu (rare), badge se odebere. */
        membrowser_edited_mark_range ( instance_idx, region_kind, region_sub_id,
                                        ( uint32_t ) e.offset, e.len,
                                        pre_ok ? pre : NULL );
        membrowser_edited_check_unmark_range ( instance_idx, region_kind,
                                                region_sub_id,
                                                ( uint32_t ) e.offset, e.len,
                                                e.bytes );
    }

    std::free ( pre );
    membrowser_undo_free_entry ( &e );

    if ( wr < 0 ) {
        set_status ( "%s", _( "Redo failed: backend write returned error" ) );
        return false;
    }
    set_status ( "%s", _( "Redo: applied" ) );
    return true;
}


extern "C" const char *membrowser_fill_dialog_last_status ( void )
{
    return ( s_status_buf[0] != '\0' ) ? s_status_buf : NULL;
}


extern "C" void membrowser_fill_dialog_clear_status ( void )
{
    s_status_buf[0] = '\0';
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
