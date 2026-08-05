/**
 * @file membrowser_diff_window.cpp
 * @brief Memory Diff okno - implementace (V5).
 *
 * Layout:
 *   +- Memory Diff ###mb_diff_main ---------------------------------------+
 *   | Row 1: Source A: [Region ▼] [Mode ▼] [Snapshot Now] |               |
 *   |        Source B: [Region ▼] [Mode ▼] [Snapshot Now]                 |
 *   | Row 2: [Auto-snapshot at pause] [Refresh] [Export...]               |
 *   |        Changed: 1234 of 65536 (1.88 %)                              |
 *   +---------------------------------------------------------------------+
 *   |  addr   | A: hex bytes      | B: hex bytes      | diff col          |
 *   |  0000:  | 00 01 02 ...      | 00 99 02 ...      |  .X.             |
 *   |  ...                                                                |
 *   +---------------------------------------------------------------------+
 *
 * Rozdílné bajty: magenta background (obě A i B cell). Stejné: default
 * theme color.
 *
 * Source modes:
 *   - SRCM_LIVE: per render frame fresh read přes dbgapi_regions_read,
 *     uložené do interního live_buf (re-realloc při změně regionu).
 *   - SRCM_MANUAL: snapshot pořízený "Snapshot Now" tlačítkem; drží se
 *     dokud user nepřepořídí nebo nezmění region.
 *   - SRCM_AUTO: snapshot automaticky pořízený při emu pause (edge
 *     detect EMULATOR_TEST_PAUSED). Kompletně shodný s MANUAL kromě
 *     mechanismu trigger.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "membrowser_diff_window.h"
#include "membrowser_diff_engine.h"

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <cfloat>

extern "C" {
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/emulator.h"
}


/**
 * @brief Maximální velikost jednoho diff bufferu.
 *
 * Stejné jako MEMBROWSER_SNAPSHOT_MAX_BYTES (= 4 MB). Region větší než
 * tato hranice se ořeže (warning v status bar). Pro běžné regiony
 * (16 KB ROM .. 64 KB Logical .. 256 KB VRAM plane) bez problému.
 */
#define MB_DIFF_BUF_MAX_BYTES ( 4u * 1024u * 1024u )

/**
 * @brief Maximální počet enumerovaných regionů (per snapshot).
 */
#define MB_DIFF_MAX_REGIONS 512

/**
 * @brief Source mode (Source A i B sdílejí stejný enum).
 */
typedef enum
{
    MB_DIFF_SRCM_LIVE = 0,      /**< Live emu state, re-read každý frame. */
    MB_DIFF_SRCM_MANUAL = 1,    /**< Snapshot pořízený "Snapshot Now". */
    MB_DIFF_SRCM_AUTO = 2,      /**< Auto-snapshot at pause edge. */
    MB_DIFF_SRCM__COUNT
} mb_diff_srcm_t;


/**
 * @brief Per-source state (drží oba Source A a Source B nezávisle).
 *
 * region_kind/sub_id: (kind, sub_id) tuple stabilní napříč session.
 * region_id_cached:   resolved per frame z aktuálního regions snapshot.
 * mode:               LIVE/MANUAL/AUTO.
 * buf/buf_size:       cached obsah pro porovnání. Pro LIVE = aktuální
 *                     read (refresh per frame). Pro MANUAL/AUTO = poslední
 *                     pořízený snapshot.
 * snapshot_taken:     true = MANUAL/AUTO byl alespoň jednou pořízen
 *                     a buf je validní.
 */
struct mb_diff_source_t
{
    int             region_kind;
    int             sub_id;
    int             region_id_cached;
    int             mode;
    uint8_t        *buf;
    uint32_t        buf_size;
    bool            snapshot_taken;
};


/**
 * @brief Globální state Memory Diff okna (singleton).
 */
struct mb_diff_state_t
{
    bool            inited;

    mb_diff_source_t src_a;
    mb_diff_source_t src_b;

    /* Auto-snapshot at pause - edge detect přes last_paused. */
    bool            auto_snapshot_at_pause;
    bool            last_paused;

    /* Region enumerate cache (per frame). */
    st_REGION_DESC  regions[MB_DIFF_MAX_REGIONS];
    int             regions_count;

    /* Scroll state side-by-side hex view. */
    uint32_t        scroll_top_addr;

    /* Stats (per frame compute). */
    st_MB_DIFF_STATS stats;

    /* Export state. */
    bool            export_dialog_open;
    char            export_error[256];

    /* Last-applied auto-snapshot timestamp/counter (info v UI). */
    int             auto_snapshot_count;
};

static mb_diff_state_t s_diff;


/* -------------------------------------------------------------------------
 * Source buffer management
 * ------------------------------------------------------------------------- */

static void source_free_buf ( mb_diff_source_t *src )
{
    if ( src->buf ) {
        std::free ( src->buf );
        src->buf = nullptr;
    }
    src->buf_size = 0;
    src->snapshot_taken = false;
}


/**
 * @brief (Re)alokuje source buffer na požadovanou velikost.
 *
 * Pokud current size shoduje, nedělá nic. Při OOM nastaví size=0 a
 * vrátí false.
 */
static bool source_alloc_buf ( mb_diff_source_t *src, uint32_t size )
{
    if ( size == 0 ) {
        source_free_buf ( src );
        return true;
    }
    if ( src->buf && src->buf_size == size ) return true;

    std::free ( src->buf );
    src->buf = ( uint8_t * ) std::malloc ( size );
    if ( !src->buf ) {
        src->buf_size = 0;
        src->snapshot_taken = false;
        return false;
    }
    src->buf_size = size;
    return true;
}


/* Najde region v interním snapshotu dle (kind, sub_id) -> region_id. */
static int find_region_id ( int kind, int sub_id )
{
    for ( int i = 0; i < s_diff.regions_count; i++ ) {
        if ( s_diff.regions[i].kind == kind
             && s_diff.regions[i].sub_id == sub_id ) {
            return s_diff.regions[i].id;
        }
    }
    return -1;
}


/* Najde region descriptor dle id. */
static const st_REGION_DESC *find_region_desc ( int region_id )
{
    if ( region_id < 0 ) return nullptr;
    for ( int i = 0; i < s_diff.regions_count; i++ ) {
        if ( s_diff.regions[i].id == region_id ) {
            return &s_diff.regions[i];
        }
    }
    return nullptr;
}


/**
 * @brief Pořídí snapshot do source bufferu (MANUAL/AUTO trigger).
 *
 * Re-resolve region_id z (kind, sub_id), naalokuj buffer dle region size
 * (clamp na MB_DIFF_BUF_MAX_BYTES), volej dbgapi_regions_read.
 *
 * @return true = úspěch, false = chyba (disconnected / OOM / read fail).
 */
static bool source_take_snapshot ( mb_diff_source_t *src )
{
    int rid = find_region_id ( src->region_kind, src->sub_id );
    src->region_id_cached = rid;
    if ( rid < 0 ) {
        source_free_buf ( src );
        return false;
    }
    const st_REGION_DESC *rd = find_region_desc ( rid );
    if ( !rd || !rd->connected || rd->size == 0 ) {
        source_free_buf ( src );
        return false;
    }
    uint32_t take = rd->size;
    if ( take > MB_DIFF_BUF_MAX_BYTES ) take = MB_DIFF_BUF_MAX_BYTES;

    if ( !source_alloc_buf ( src, take ) ) return false;

    int got = dbgapi_regions_read ( rid, 0, src->buf, take );
    if ( got < 0 ) {
        source_free_buf ( src );
        return false;
    }
    /* Pokud read clampem vrátil méně, zbytek vyplníme nulami. */
    for ( int i = got; i < ( int ) take; i++ ) src->buf[i] = 0;

    src->snapshot_taken = true;
    return true;
}


/**
 * @brief Refresh LIVE bufferu (per frame, jen pokud mode == LIVE).
 *
 * Vyrobí čerstvý read aktuálního regionu (re-resolve region_id).
 * Pro MANUAL/AUTO mode nedělá nic.
 */
static void source_refresh_live ( mb_diff_source_t *src )
{
    if ( src->mode != MB_DIFF_SRCM_LIVE ) return;
    int rid = find_region_id ( src->region_kind, src->sub_id );
    src->region_id_cached = rid;
    if ( rid < 0 ) {
        source_free_buf ( src );
        return;
    }
    const st_REGION_DESC *rd = find_region_desc ( rid );
    if ( !rd || !rd->connected || rd->size == 0 ) {
        source_free_buf ( src );
        return;
    }
    uint32_t take = rd->size;
    if ( take > MB_DIFF_BUF_MAX_BYTES ) take = MB_DIFF_BUF_MAX_BYTES;

    if ( !source_alloc_buf ( src, take ) ) return;
    int got = dbgapi_regions_read ( rid, 0, src->buf, take );
    if ( got < 0 ) {
        source_free_buf ( src );
        return;
    }
    for ( int i = got; i < ( int ) take; i++ ) src->buf[i] = 0;
    src->snapshot_taken = true;  /* LIVE drží buf - je vždy "platný". */
}


/* -------------------------------------------------------------------------
 * State init / cleanup
 * ------------------------------------------------------------------------- */

static void ensure_inited ( void )
{
    if ( s_diff.inited ) return;
    std::memset ( &s_diff, 0, sizeof ( s_diff ) );

    /* Default: oba sources nastavené na LOGICAL region, A = LIVE, B = MANUAL.
     * Žádný snapshot zatím nepořízen - status bar řekne "no snapshot". */
    s_diff.src_a.region_kind = REGION_KIND_LOGICAL;
    s_diff.src_a.sub_id = 0;
    s_diff.src_a.mode = MB_DIFF_SRCM_LIVE;
    s_diff.src_a.region_id_cached = -1;

    s_diff.src_b.region_kind = REGION_KIND_LOGICAL;
    s_diff.src_b.sub_id = 0;
    s_diff.src_b.mode = MB_DIFF_SRCM_MANUAL;
    s_diff.src_b.region_id_cached = -1;

    s_diff.auto_snapshot_at_pause = false;
    s_diff.last_paused = false;
    s_diff.regions_count = 0;
    s_diff.scroll_top_addr = 0;
    s_diff.export_dialog_open = false;
    s_diff.export_error[0] = '\0';
    s_diff.auto_snapshot_count = 0;

    s_diff.inited = true;
}


extern "C" void membrowser_diff_window_cleanup ( void )
{
    if ( !s_diff.inited ) return;
    source_free_buf ( &s_diff.src_a );
    source_free_buf ( &s_diff.src_b );
    s_diff.inited = false;
}


/* -------------------------------------------------------------------------
 * Region enumerate (per frame)
 * ------------------------------------------------------------------------- */

static void refresh_regions_snapshot ( void )
{
    int n = dbgapi_regions_enumerate ( s_diff.regions, MB_DIFF_MAX_REGIONS );
    if ( n < 0 ) n = 0;
    s_diff.regions_count = n;
}


/* -------------------------------------------------------------------------
 * UI helpers
 * ------------------------------------------------------------------------- */

/* Region label - "Logical Z80 [#0]" style. Buffer per-call, single thread. */
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


/* Render region dropdown pro daný source. Změny v dropdown invalidují
 * snapshot (= user musí re-take pro MANUAL/AUTO). */
static void render_region_combo ( mb_diff_source_t *src, const char *id_suffix )
{
    char combo_id[64];
    std::snprintf ( combo_id, sizeof ( combo_id ), "###mb_diff_region_%s",
                    id_suffix );

    /* Najdi current label. */
    const char *cur_label = "?";
    int cur_id = find_region_id ( src->region_kind, src->sub_id );
    src->region_id_cached = cur_id;
    if ( cur_id >= 0 ) {
        const st_REGION_DESC *rd = find_region_desc ( cur_id );
        if ( rd ) cur_label = region_descriptor_label ( rd );
    }

    ImGui::SetNextItemWidth ( 200 );
    if ( ImGui::BeginCombo ( combo_id, cur_label ) ) {
        for ( int i = 0; i < s_diff.regions_count; i++ ) {
            const st_REGION_DESC *r = &s_diff.regions[i];
            bool selected = ( r->id == cur_id );
            char item_id[128];
            std::snprintf ( item_id, sizeof ( item_id ),
                            "%s###mb_diff_region_item_%s_%d",
                            region_descriptor_label ( r ), id_suffix, i );
            if ( ImGui::Selectable ( item_id, selected ) ) {
                if ( r->kind != src->region_kind
                     || r->sub_id != src->sub_id ) {
                    src->region_kind = r->kind;
                    src->sub_id = r->sub_id;
                    /* Region změna - invalidate snapshot (MANUAL/AUTO musí
                     * re-take). LIVE se sám refreshne v dalším render frame. */
                    source_free_buf ( src );
                }
            }
            if ( selected ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
}


/* Render mode dropdown - LIVE / MANUAL / AUTO. */
static void render_mode_combo ( mb_diff_source_t *src, const char *id_suffix )
{
    const char *mode_labels[MB_DIFF_SRCM__COUNT];
    mode_labels[MB_DIFF_SRCM_LIVE]   = _( "Live" );
    mode_labels[MB_DIFF_SRCM_MANUAL] = _( "Snapshot" );
    mode_labels[MB_DIFF_SRCM_AUTO]   = _( "Auto @ pause" );

    char combo_id[64];
    std::snprintf ( combo_id, sizeof ( combo_id ), "###mb_diff_mode_%s",
                    id_suffix );

    int cur = src->mode;
    if ( cur < 0 || cur >= MB_DIFF_SRCM__COUNT ) cur = MB_DIFF_SRCM_LIVE;

    ImGui::SetNextItemWidth ( 140 );
    if ( ImGui::BeginCombo ( combo_id, mode_labels[cur] ) ) {
        for ( int i = 0; i < MB_DIFF_SRCM__COUNT; i++ ) {
            bool selected = ( i == cur );
            char item_id[64];
            std::snprintf ( item_id, sizeof ( item_id ),
                            "%s###mb_diff_mode_item_%s_%d",
                            mode_labels[i], id_suffix, i );
            if ( ImGui::Selectable ( item_id, selected ) ) {
                if ( i != src->mode ) {
                    src->mode = i;
                    /* Mode změna - invalidate (MANUAL/AUTO musí re-take,
                     * LIVE se refreshne příští frame). */
                    source_free_buf ( src );
                }
            }
            if ( selected ) ImGui::SetItemDefaultFocus ( );
        }
        ImGui::EndCombo ( );
    }
}


/* -------------------------------------------------------------------------
 * Auto-snapshot at pause edge detect
 * ------------------------------------------------------------------------- */

/**
 * @brief Polling per-frame pause edge detect.
 *
 * Pokud auto_snapshot_at_pause = true a emu právě přešel
 * non-paused -> paused, pořídí snapshot do source-ů které mají mode = AUTO.
 *
 * Hot path discipline: edge je trigger event (rare), polling
 * EMULATOR_TEST_PAUSED je 1 read globalu per UI frame, žádný emu hot path.
 */
static void poll_auto_snapshot_at_pause ( void )
{
    bool now_paused = EMULATOR_TEST_PAUSED;
    bool edge_to_paused = ( now_paused && !s_diff.last_paused );
    s_diff.last_paused = now_paused;

    if ( !s_diff.auto_snapshot_at_pause ) return;
    if ( !edge_to_paused ) return;

    bool took = false;
    if ( s_diff.src_a.mode == MB_DIFF_SRCM_AUTO ) {
        if ( source_take_snapshot ( &s_diff.src_a ) ) took = true;
    }
    if ( s_diff.src_b.mode == MB_DIFF_SRCM_AUTO ) {
        if ( source_take_snapshot ( &s_diff.src_b ) ) took = true;
    }
    if ( took ) s_diff.auto_snapshot_count++;
}


/* -------------------------------------------------------------------------
 * Export diff to text
 * ------------------------------------------------------------------------- */

/* Callback predaný do diff_compute - zapisuje řádek do FILE *. */
struct export_ctx_t
{
    FILE *fp;
    uint64_t emitted;
    uint64_t cap;     /**< Bezpečný limit počtu řádek (= avoid runaway). */
};

static bool export_cb ( void *user, uint64_t off, uint8_t va, uint8_t vb )
{
    export_ctx_t *c = ( export_ctx_t * ) user;
    if ( c->emitted >= c->cap ) return false;
    std::fprintf ( c->fp, "%08llX: %02X -> %02X\n",
                   ( unsigned long long ) off,
                   ( unsigned ) va, ( unsigned ) vb );
    c->emitted++;
    return true;
}


/* Otevře IGFD save dialog pro export. */
static void open_export_dialog ( void )
{
    s_diff.export_dialog_open = true;
    s_diff.export_error[0] = '\0';

    IGFD::FileDialogConfig config;
    config.path = ".";
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                   | ImGuiFileDialogFlags_DontShowHiddenFiles
                   | ImGuiFileDialogFlags_ShowDevicesButton
                   | ImGuiFileDialogFlags_ConfirmOverwrite;
    ImGuiFileDialog::Instance ( )->OpenDialog (
        "MembrowserDiffExportFch",
        _( "Export diff as text" ),
        ".txt,.diff,.*",
        config );
}


static void render_export_dialog ( void )
{
    if ( !s_diff.export_dialog_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800, 500 ), ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "MembrowserDiffExportFch" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );

            FILE *fp = std::fopen ( path.c_str ( ), "wb" );
            if ( !fp ) {
                std::snprintf ( s_diff.export_error,
                                sizeof ( s_diff.export_error ),
                                "%s", _( "Cannot open file for writing" ) );
            } else {
                /* Header. */
                std::fprintf ( fp,
                    "# Memory Diff export\n"
                    "# Source A size: %llu, Source B size: %llu\n"
                    "# Changed: %llu of %llu bytes\n"
                    "#\n",
                    ( unsigned long long ) s_diff.stats.size_a,
                    ( unsigned long long ) s_diff.stats.size_b,
                    ( unsigned long long ) s_diff.stats.changed,
                    ( unsigned long long ) s_diff.stats.compared_bytes );

                export_ctx_t ctx;
                ctx.fp = fp;
                ctx.emitted = 0;
                ctx.cap = 1000000;  /* 1 M lines max - safety. */
                st_MB_DIFF_STATS dummy;
                membrowser_diff_compute (
                    s_diff.src_a.buf, s_diff.src_a.buf_size,
                    s_diff.src_b.buf, s_diff.src_b.buf_size,
                    export_cb, &ctx, &dummy );

                if ( s_diff.stats.size_mismatch ) {
                    std::fprintf ( fp,
                        "#\n# (tail mismatch: size_a=%llu vs size_b=%llu)\n",
                        ( unsigned long long ) s_diff.stats.size_a,
                        ( unsigned long long ) s_diff.stats.size_b );
                }

                std::fclose ( fp );
            }
        }
        ImGuiFileDialog::Instance ( )->Close ( );
        s_diff.export_dialog_open = false;
    }

    if ( s_diff.export_error[0] != '\0' ) {
        if ( ImGui::Begin ( _L ( "Export error###mb_diff_export_err" ) ) ) {
            ImGui::PushStyleColor ( ImGuiCol_Text,
                                     ImVec4 ( 1.0f, 0.3f, 0.3f, 1.0f ) );
            ImGui::TextWrapped ( "%s", s_diff.export_error );
            ImGui::PopStyleColor ( );
            if ( ImGui::Button ( _L ( "OK###mb_diff_export_err_ok" ) ) ) {
                s_diff.export_error[0] = '\0';
            }
        }
        ImGui::End ( );
    }
}


/* -------------------------------------------------------------------------
 * Side-by-side hex render
 * ------------------------------------------------------------------------- */

#define MB_DIFF_BYTES_PER_ROW 16

/* Magenta highlight pro odlišný bajt - v line s "Snapshot Δ" layer
 * v hlavním Memory Browser (= konzistentní visual jazyk).
 * RGBA = (255, 64, 255, 80) jako semi-transparent overlay. */
static ImU32 col_diff_bg ( void )
{
    return IM_COL32 ( 255, 64, 255, 80 );
}


/**
 * @brief Vykreslí side-by-side hex view s magenta highlight rozdílů.
 *
 * Layout per řádek:
 *   AAAAAAAA  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX  |  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX
 *   addr      A bytes (16 / row, 8x2 groups)                       B bytes
 */
static void render_side_by_side ( float avail_h )
{
    uint64_t total = ( s_diff.stats.size_a > s_diff.stats.size_b )
                       ? s_diff.stats.size_a : s_diff.stats.size_b;
    if ( total == 0 ) {
        ImGui::TextDisabled ( "%s", _( "No data - pick region and take snapshot" ) );
        return;
    }

    /* Monospace + scroll child. */
    ImFont *mono = myimgui_get_monospace_font ( );
    if ( mono ) ImGui::PushFont ( mono );

    ImGui::BeginChild ( "###mb_diff_hex_scroll",
                        ImVec2 ( 0, avail_h ),
                        false,
                        ImGuiWindowFlags_HorizontalScrollbar );

    /* ImGuiListClipper pro velké regiony (Ramdisk apod). */
    int total_rows = ( int ) ( ( total + MB_DIFF_BYTES_PER_ROW - 1 )
                                / MB_DIFF_BYTES_PER_ROW );

    ImGuiListClipper clipper;
    clipper.Begin ( total_rows );
    while ( clipper.Step ( ) ) {
        for ( int row = clipper.DisplayStart;
              row < clipper.DisplayEnd; row++ ) {
            uint64_t row_addr = ( uint64_t ) row * MB_DIFF_BYTES_PER_ROW;

            /* Address column. */
            ImGui::Text ( "%08llX:", ( unsigned long long ) row_addr );
            ImGui::SameLine ( );

            /* Source A bytes. */
            for ( int i = 0; i < MB_DIFF_BYTES_PER_ROW; i++ ) {
                uint64_t off = row_addr + i;
                bool in_a = ( off < s_diff.src_a.buf_size );
                bool changed = membrowser_diff_is_changed (
                    s_diff.src_a.buf, s_diff.src_a.buf_size,
                    s_diff.src_b.buf, s_diff.src_b.buf_size, off );

                ImVec2 p0 = ImGui::GetCursorScreenPos ( );
                if ( in_a ) {
                    ImGui::Text ( "%02X", s_diff.src_a.buf[off] );
                } else {
                    ImGui::TextDisabled ( "--" );
                }
                if ( changed ) {
                    ImVec2 p1 = ImGui::GetItemRectMax ( );
                    ImGui::GetWindowDrawList ( )->AddRectFilled (
                        ImVec2 ( p0.x - 1, p0.y ),
                        ImVec2 ( p1.x + 1, p1.y ),
                        col_diff_bg ( ) );
                }
                ImGui::SameLine ( );
                if ( ( i + 1 ) % 8 == 0 && i + 1 < MB_DIFF_BYTES_PER_ROW ) {
                    ImGui::TextDisabled ( " " );
                    ImGui::SameLine ( );
                }
            }

            ImGui::TextDisabled ( " | " );
            ImGui::SameLine ( );

            /* Source B bytes. */
            for ( int i = 0; i < MB_DIFF_BYTES_PER_ROW; i++ ) {
                uint64_t off = row_addr + i;
                bool in_b = ( off < s_diff.src_b.buf_size );
                bool changed = membrowser_diff_is_changed (
                    s_diff.src_a.buf, s_diff.src_a.buf_size,
                    s_diff.src_b.buf, s_diff.src_b.buf_size, off );

                ImVec2 p0 = ImGui::GetCursorScreenPos ( );
                if ( in_b ) {
                    ImGui::Text ( "%02X", s_diff.src_b.buf[off] );
                } else {
                    ImGui::TextDisabled ( "--" );
                }
                if ( changed ) {
                    ImVec2 p1 = ImGui::GetItemRectMax ( );
                    ImGui::GetWindowDrawList ( )->AddRectFilled (
                        ImVec2 ( p0.x - 1, p0.y ),
                        ImVec2 ( p1.x + 1, p1.y ),
                        col_diff_bg ( ) );
                }
                if ( i + 1 < MB_DIFF_BYTES_PER_ROW ) {
                    ImGui::SameLine ( );
                    if ( ( i + 1 ) % 8 == 0 ) {
                        ImGui::TextDisabled ( " " );
                        ImGui::SameLine ( );
                    }
                }
            }
        }
    }
    clipper.End ( );

    ImGui::EndChild ( );
    if ( mono ) ImGui::PopFont ( );
}


/* -------------------------------------------------------------------------
 * Status bar
 * ------------------------------------------------------------------------- */

static void render_status_bar ( void )
{
    double pct = 0.0;
    if ( s_diff.stats.compared_bytes > 0 ) {
        pct = ( double ) s_diff.stats.changed
              / ( double ) s_diff.stats.compared_bytes * 100.0;
    }

    if ( s_diff.stats.size_mismatch ) {
        ImGui::Text ( _( "Changed: %llu of %llu (%.2f %%)  [size mismatch A=%llu B=%llu]" ),
                      ( unsigned long long ) s_diff.stats.changed,
                      ( unsigned long long ) s_diff.stats.compared_bytes,
                      pct,
                      ( unsigned long long ) s_diff.stats.size_a,
                      ( unsigned long long ) s_diff.stats.size_b );
    } else {
        ImGui::Text ( _( "Changed: %llu of %llu (%.2f %%)" ),
                      ( unsigned long long ) s_diff.stats.changed,
                      ( unsigned long long ) s_diff.stats.compared_bytes,
                      pct );
    }

    /* Auto-snapshot info. */
    if ( s_diff.auto_snapshot_at_pause ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|" );
        ImGui::SameLine ( );
        ImGui::TextDisabled ( _( "auto-snapshots taken: %d" ),
                              s_diff.auto_snapshot_count );
    }
}


/* -------------------------------------------------------------------------
 * Toolbar
 * ------------------------------------------------------------------------- */

static void render_source_row ( mb_diff_source_t *src, const char *label_short,
                                 const char *id_suffix )
{
    ImGui::Text ( "%s:", label_short );
    ImGui::SameLine ( );

    render_region_combo ( src, id_suffix );
    ImGui::SameLine ( );
    render_mode_combo ( src, id_suffix );
    ImGui::SameLine ( );

    /* "Snapshot Now" tlačítko - aktivní jen pro MANUAL/AUTO mode. */
    bool can_snap = ( src->mode == MB_DIFF_SRCM_MANUAL
                       || src->mode == MB_DIFF_SRCM_AUTO );
    if ( !can_snap ) ImGui::BeginDisabled ( );
    char btn_id[64];
    std::snprintf ( btn_id, sizeof ( btn_id ),
                    "%s###mb_diff_snap_%s", _( "Snapshot Now" ), id_suffix );
    if ( ImGui::Button ( btn_id ) ) {
        source_take_snapshot ( src );
    }
    if ( !can_snap ) ImGui::EndDisabled ( );

    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Capture current region content as snapshot" ) );
    }

    /* Snapshot status indikator. */
    ImGui::SameLine ( );
    if ( src->mode == MB_DIFF_SRCM_LIVE ) {
        ImGui::TextDisabled ( _( "(live)" ) );
    } else if ( src->snapshot_taken && src->buf_size > 0 ) {
        ImGui::TextDisabled ( _( "(%u B)" ), src->buf_size );
    } else {
        ImGui::PushStyleColor ( ImGuiCol_Text,
                                 ImVec4 ( 0.95f, 0.55f, 0.25f, 1.0f ) );
        ImGui::Text ( "%s", _( "(no snapshot)" ) );
        ImGui::PopStyleColor ( );
    }
}


static void render_toolbar ( void )
{
    render_source_row ( &s_diff.src_a, _( "A" ), "a" );
    render_source_row ( &s_diff.src_b, _( "B" ), "b" );

    /* Row: Auto-snapshot toggle + Export. */
    ImGui::Checkbox ( _L ( "Auto-snapshot at pause###mb_diff_auto_pause" ),
                       &s_diff.auto_snapshot_at_pause );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Take fresh snapshot in AUTO sources on emulator pause" ) );
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    if ( ImGui::Button ( _L ( "Refresh A+B###mb_diff_refresh" ) ) ) {
        /* Manual refresh - re-take pro MANUAL/AUTO; LIVE se sám refreshne. */
        if ( s_diff.src_a.mode != MB_DIFF_SRCM_LIVE ) {
            source_take_snapshot ( &s_diff.src_a );
        }
        if ( s_diff.src_b.mode != MB_DIFF_SRCM_LIVE ) {
            source_take_snapshot ( &s_diff.src_b );
        }
    }

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );

    if ( ImGui::Button ( _L ( "Export...###mb_diff_export" ) ) ) {
        open_export_dialog ( );
    }
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Export diff as text file (addr: AA -> BB per line)" ) );
    }
}


/* -------------------------------------------------------------------------
 * Public entry: render
 * ------------------------------------------------------------------------- */

extern "C" void membrowser_diff_window_render ( bool *p_open )
{
    if ( !p_open ) return;
    ensure_inited ( );

    ImGui::SetNextWindowSize ( ImVec2 ( 960, 560 ), ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSizeConstraints ( ImVec2 ( 600, 320 ),
                                           ImVec2 ( FLT_MAX, FLT_MAX ) );

    /* Title bez _L() - 3 hashe StableID drží identitu nezávisle na lokalizaci
     * (label se přeloží přes _(), ID suffix stabilní). */
    char title[96];
    std::snprintf ( title, sizeof ( title ), "%s###mb_diff_main",
                    _( "Memory Diff" ) );

    if ( !ImGui::Begin ( title, p_open, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::End ( );
        return;
    }

    /* Per-frame: refresh region list, LIVE re-read, pause edge detect. */
    refresh_regions_snapshot ( );
    poll_auto_snapshot_at_pause ( );
    source_refresh_live ( &s_diff.src_a );
    source_refresh_live ( &s_diff.src_b );

    render_toolbar ( );
    render_export_dialog ( );

    ImGui::Separator ( );

    /* Recompute stats (always - cheap pro <= 4 MB buf; per frame OK). */
    membrowser_diff_compute (
        s_diff.src_a.buf, s_diff.src_a.buf_size,
        s_diff.src_b.buf, s_diff.src_b.buf_size,
        NULL, NULL, &s_diff.stats );

    /* Reserve space pro status bar. */
    float bottom_h = ImGui::GetTextLineHeightWithSpacing ( ) + 8.0f;
    float avail = ImGui::GetContentRegionAvail ( ).y - bottom_h;
    if ( avail < 80.0f ) avail = 80.0f;

    render_side_by_side ( avail );

    ImGui::Separator ( );
    render_status_bar ( );

    ImGui::End ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
