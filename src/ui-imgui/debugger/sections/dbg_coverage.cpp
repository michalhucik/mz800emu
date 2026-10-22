/**
 * @file   dbg_coverage.cpp
 * @brief  Záložka "Coverage" okna CPU Profiler (viz dbg_coverage.h).
 *
 * Toolbar ovládá sběr (úroveň, kapacita, Start/Stop, Reset, Mark, Export),
 * filtr vybírá řádky (neúplné větve, obě větve, podmíněné instrukce, nové
 * od značky, vše) v rozsahu adres. Tabulka ukazuje řádky plné kopie dat
 * (dbg_coverage_data.h): u úrovně branch jeden řádek na vykonanou adresu,
 * u úrovně detail jeden řádek na verzi kódu (buňka, adresa, klíč)
 * s čítači.
 *
 * Levý klik na řádek otevře adresu v Disassembly #1, pravý klik nabídne
 * breakpoint a Disassembly #2-#5 (stejně jako záložka Per PC).
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "i18n.h"

#include "dbg_coverage.h"
#include "dbg_coverage_data.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"  /* dbg_disasm_show_in_slot */
#include "ui-imgui/debugger/dbgapi_helpers.h"             /* dbg_ui_bp_add */

extern "C" {
#include "emulator/debugger/symbols/sym_db.h"
}


namespace {

/** @brief Co tabulka ukazuje (index = položka comba). */
enum CovShow {
    COV_SHOW_PARTIAL = 0, /**< Podmíněné instrukce s jedním výsledkem nebo bez výsledku. */
    COV_SHOW_BOTH,        /**< Podmíněné instrukce s oběma výsledky. */
    COV_SHOW_DECISIONS,   /**< Všechny podmíněné instrukce. */
    COV_SHOW_NEW,         /**< Kód vykonaný poprvé od poslední značky. */
    COV_SHOW_ALL,         /**< Vše. */
    COV_SHOW_COUNT        /**< Počet položek. */
};

/** @brief Sloupce tabulky (index = pořadí v BeginTable). */
enum CovCol {
    COV_COL_ADDR = 0,  /**< Adresa. */
    COV_COL_SYMBOL,    /**< Nejbližší symbol + posun. */
    COV_COL_STATE,     /**< Stav větve. */
    COV_COL_EXEC,      /**< Počet vykonání (detail). */
    COV_COL_TAKEN,     /**< Podmínka splněna (detail). */
    COV_COL_NOT_TAKEN, /**< Podmínka nesplněna (detail). */
    COV_COL_CELL,      /**< Prostor a offset buňky (detail). */
    COV_COL_KEY,       /**< Bajty klíče verze kódu (detail). */
    COV_COL_EPOCH,     /**< Epocha prvního vykonání. */
    COV_COL_FLAGS,     /**< Příznaky. */
    COV_COL_COUNT_ALL  /**< Počet sloupců. */
};

/** @brief Kapacity tabulky úrovně detail nabízené v comboboxu (k nim případně kapacita stávajících dat). */
const uint32_t k_capacities[] = { 16384u, 65536u, 262144u, 1048576u };

/**
 * @brief Stav záložky mezi snímky (jen UI vlákno).
 *
 * @invariant view obsahuje indexy do dbg_cov_data_rows() obnovy view_serial.
 * @invariant sym_addrs je seřazené vzestupně a odpovídá sym_db verze sym_version.
 */
struct CovPanelState {
    int level = 0;                         /**< Volba úrovně pro Start (0 branch, 1 detail). */
    uint32_t capacity = COVERAGE_DEFAULT_CAPACITY; /**< Kapacita pro Start (detail). */
    en_COVERAGE_KEY key = COVERAGE_KEY_OPCODE; /**< Klíč pro Start (převzatý z dat, GUI ho nemění). */
    bool reset_on_discontinuity = false;   /**< Volba pro Start (převzatá z dat, GUI ji nemění). */
    bool synced = false;                   /**< Volby už jednou převzaly nastavení dat. */
    uint32_t synced_generation = 0;        /**< Generace dat, ze které volby převzaly nastavení. */
    int show = COV_SHOW_PARTIAL;           /**< Filtr (CovShow). */
    char from_buf[ 8 ] = "0000";           /**< Rozsah adres - od (hex). */
    char to_buf[ 8 ] = "FFFF";             /**< Rozsah adres - do (hex). */

    std::vector< size_t > view;            /**< Indexy řádků, které vyhoví filtru. */
    uint32_t view_serial = 0;              /**< Obnova dat, ke které patří view. */
    bool view_dirty = true;                /**< Filtr se změnil, view přepočítat. */

    std::vector< uint16_t > sym_addrs;     /**< Adresy globálních symbolů, seřazené. */
    unsigned sym_version = ~0u;            /**< sym_db verze, ke které patří sym_addrs. */

    uint16_t ctx_pc = 0;                   /**< Adresa řádku, na kterém se otevřelo kontextové menu. */

    bool export_dialog_open = false;       /**< IGFD dialog exportu je otevřený. */
    bool error_queue = false;              /**< Otevřít popup s chybou. */
    char error[ 256 ] = "";                /**< Text chyby operace nebo exportu. */
};

CovPanelState g_cv;


/**
 * @brief Přečte hex adresu z bufferu.
 * @return Adresa, nebo `def` při prázdném nebo neplatném textu.
 */
uint16_t parse_hex_addr ( const char *s, uint16_t def )
{
    if ( !s || !s[ 0 ] ) return def;
    char *end = NULL;
    unsigned long v = strtoul ( s, &end, 16 );
    if ( !end || *end != '\0' || v > 0xFFFFul ) return def;
    return (uint16_t) v;
}


/** @brief Ohlásí chybu operace popupem. */
void report_error ( const char *msg )
{
    g_strlcpy ( g_cv.error, msg && msg[ 0 ] ? msg : _( "The emulator did not respond" ), sizeof ( g_cv.error ) );
    g_cv.error_queue = true;
}


/** @brief Obnoví sym_addrs, pokud se sym_db od posledně změnila. */
void sym_cache_update ( void )
{
    unsigned ver = sym_db_get_version ( );
    if ( ver == g_cv.sym_version ) return;
    g_cv.sym_version = ver;
    g_cv.sym_addrs.clear ( );
    st_SYM_DB_ITER it;
    sym_db_iter_init ( &it );
    const st_SYMBOL *s;
    while ( ( s = sym_db_iter_next ( &it ) ) != NULL ) {
        if ( s->bank_id != 0 || s->addr > 0xFFFFu || !s->name ) continue;
        g_cv.sym_addrs.push_back ( (uint16_t) s->addr );
    };
    std::sort ( g_cv.sym_addrs.begin ( ), g_cv.sym_addrs.end ( ) );
    g_cv.sym_addrs.erase ( std::unique ( g_cv.sym_addrs.begin ( ), g_cv.sym_addrs.end ( ) ),
                           g_cv.sym_addrs.end ( ) );
}


/**
 * @brief Nejbližší globální symbol na adrese <= pc.
 * @param pc       Adresa.
 * @param[out] off pc - adresa symbolu.
 * @return Symbol, nebo NULL.
 */
const st_SYMBOL *nearest_symbol ( uint16_t pc, unsigned *off )
{
    auto it = std::upper_bound ( g_cv.sym_addrs.begin ( ), g_cv.sym_addrs.end ( ), pc );
    if ( it == g_cv.sym_addrs.begin ( ) ) return NULL;
    uint16_t sa = *( it - 1 );
    *off = (unsigned) ( pc - sa );
    return sym_db_lookup_by_addr ( sa, 0 );
}


/** @brief Sestaví filtr podle voleb záložky. */
void build_filter ( st_COVERAGE_FILTER *f )
{
    coverage_filter_init ( f );
    f->pc_from = parse_hex_addr ( g_cv.from_buf, 0x0000 );
    f->pc_to = parse_hex_addr ( g_cv.to_buf, 0xFFFF );
    if ( f->pc_to < f->pc_from ) {
        uint16_t t = f->pc_to;
        f->pc_to = f->pc_from;
        f->pc_from = t;
    };
    switch ( g_cv.show ) {
        case COV_SHOW_PARTIAL:   f->state_mask = coverage_state_bits ( "partial" ); break;
        case COV_SHOW_BOTH:      f->state_mask = COVERAGE_STATE_BOTH; break;
        case COV_SHOW_DECISIONS: f->state_mask = coverage_state_bits ( "decision" ); break;
        case COV_SHOW_NEW:
            f->has_epoch = true;
            f->epoch_from = dbg_cov_data_status ( )->epoch;
            break;
        default: break;
    };
}


/** @brief Přepočítá view, pokud se změnila data nebo filtr. */
void view_update ( void )
{
    uint32_t serial = dbg_cov_data_full_serial ( );
    if ( !g_cv.view_dirty && serial == g_cv.view_serial ) return;
    g_cv.view_dirty = false;
    g_cv.view_serial = serial;
    g_cv.view.clear ( );
    const st_COVERAGE_SNAPSHOT *s = dbg_cov_data_full ( );
    size_t n = 0;
    const st_COVERAGE_ROW *rows = dbg_cov_data_rows ( &n );
    if ( !s || !rows ) return;
    st_COVERAGE_FILTER f;
    build_filter ( &f );
    for ( size_t i = 0; i < n; i++ ) {
        if ( coverage_row_match ( s, &rows[ i ], &f ) ) g_cv.view.push_back ( i );
    };
}


/** @brief Bajty klíče jednotky jako hex text ("DD CB"). */
void key_text ( const st_COVERAGE_UNIT *u, char *buf, size_t size )
{
    buf[ 0 ] = '\0';
    size_t pos = 0;
    for ( unsigned k = 0; k < u->key_len && k < 4; k++ ) {
        int w = snprintf ( buf + pos, size - pos, k ? " %02X" : "%02X", u->key[ k ] );
        if ( w < 0 || (size_t) w >= size - pos ) break;
        pos += (size_t) w;
    };
}


/** @brief Příznaky řádku jako krátký text (oddělené mezerou). */
void flags_text ( uint16_t flags, char *buf, size_t size )
{
    static const struct { uint16_t bit; const char *name; } k_names[] = {
        { COVERAGE_F_INT_ENTRY, "int" },
        { COVERAGE_F_OPERAND_VARIED, "smc" },
        { COVERAGE_F_CMT_TRAP, "cmt" },
        { COVERAGE_F_HALT, "halt" },
        { COVERAGE_F_FETCH_FALLBACK, "fallback" },
        { COVERAGE_F_SATURATED, "saturated" },
        { COVERAGE_F_CELL_BUS, "bus" },
        { COVERAGE_F_PREFIX_SPLIT, "prefixes" },
        { COVERAGE_F_KEY_TRUNCATED, "long" },
    };
    buf[ 0 ] = '\0';
    for ( const auto &n : k_names ) {
        if ( !( flags & n.bit ) ) continue;
        if ( buf[ 0 ] ) g_strlcat ( buf, " ", size );
        g_strlcat ( buf, n.name, size );
    };
}


/** @brief Otevře IGFD dialog exportu (JSON nebo CSV podle přípony). */
void open_export_dialog ( void )
{
    g_cv.export_dialog_open = true;
    static char default_name[ 64 ];
    time_t now = time ( NULL );
    struct tm *tmv = localtime ( &now );
    if ( tmv ) {
        snprintf ( default_name, sizeof ( default_name ), "coverage_%04d%02d%02d_%02d%02d%02d.json",
                   tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                   tmv->tm_hour, tmv->tm_min, tmv->tm_sec );
    }
    else {
        snprintf ( default_name, sizeof ( default_name ), "coverage.json" );
    };
    IGFD::FileDialogConfig config;
    config.path = ".";
    config.fileName = default_name;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ShowDevicesButton
                 | ImGuiFileDialogFlags_ConfirmOverwrite;
    imgui_filechooser_prepare_config ( BASEUI_FCHOOSER_CAT_DBG_EXPORT, config );
    ImGuiFileDialog::Instance ( )->OpenDialog ( "CoverageExportDialog",
                                                _( "Export code coverage (JSON or CSV)" ),
                                                ".json,.csv,.*", config );
}


/** @brief Obslouží otevřený dialog exportu; po potvrzení zapíše soubor. */
void render_export_dialog ( void )
{
    if ( !g_cv.export_dialog_open ) return;
    ImGui::SetNextWindowSize ( ImVec2 ( 800.0f, 500.0f ), ImGuiCond_FirstUseEver );
    if ( !ImGuiFileDialog::Instance ( )->Display ( "CoverageExportDialog" ) ) return;
    if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
        std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
        bool csv = g_str_has_suffix ( path.c_str ( ), ".csv" ) || g_str_has_suffix ( path.c_str ( ), ".CSV" );
        char err[ 256 ];
        if ( !dbg_cov_data_export ( path.c_str ( ), csv, NULL, err, sizeof ( err ) ) ) report_error ( err );
    };
    ImGuiFileDialog::Instance ( )->Close ( );
    g_cv.export_dialog_open = false;
}


/** @brief Odešle operaci; chybu ukáže popupem. */
void send_op ( en_DBGAPI_COVERAGE_OP op )
{
    st_COVERAGE_CONFIG cfg;
    memset ( &cfg, 0, sizeof ( cfg ) );
    cfg.level = g_cv.level ? COVERAGE_LEVEL_DETAIL : COVERAGE_LEVEL_BRANCH;
    cfg.key = g_cv.key;
    cfg.capacity = g_cv.capacity;
    cfg.reset_on_discontinuity = g_cv.reset_on_discontinuity;
    char err[ 256 ];
    if ( !dbg_cov_data_op ( op, &cfg, false, err, sizeof ( err ) ) ) report_error ( err );
    g_cv.view_dirty = true;
}


/** @brief Toolbar: úroveň, kapacita, Start/Stop, Reset, Mark, Export. */
void render_toolbar ( void )
{
    const st_COVERAGE_STATUS *st = dbg_cov_data_status ( );
    const bool active = st->active;

    /* Volby převezmou nastavení dat za běhu sběru (i spuštěného přes MCP,
     * CLI nebo akci BP) a při každé nové generaci dat. Start po Stop tak
     * se stejnými volbami data nezaloží znovu (coverage_start porovnává
     * úroveň, klíč a kapacitu); změněná úroveň nebo kapacita založí nová. */
    if ( st->has_data && ( active || !g_cv.synced || st->generation != g_cv.synced_generation ) ) {
        g_cv.synced = true;
        g_cv.synced_generation = st->generation;
        g_cv.level = ( st->level == COVERAGE_LEVEL_DETAIL ) ? 1 : 0;
        g_cv.key = st->key;
        g_cv.reset_on_discontinuity = st->reset_on_discontinuity;
        if ( st->level == COVERAGE_LEVEL_DETAIL ) g_cv.capacity = st->capacity;
    };

    ImGui::BeginDisabled ( active );
    ImGui::TextUnformatted ( _( "Level:" ) );
    ImGui::SameLine ( );
    const char *levels[] = { "branch", "detail" };
    ImGui::SetNextItemWidth ( ImGui::CalcTextSize ( "detailWWW" ).x + ImGui::GetFrameHeight ( ) );
    ImGui::Combo ( "###cov_level", &g_cv.level, levels, 2 );
    if ( g_cv.level == 1 ) {
        ImGui::SameLine ( );
        ImGui::TextUnformatted ( _( "Capacity:" ) );
        ImGui::SameLine ( );
        std::vector< uint32_t > caps ( std::begin ( k_capacities ), std::end ( k_capacities ) );
        if ( std::find ( caps.begin ( ), caps.end ( ), g_cv.capacity ) == caps.end ( ) ) {
            caps.push_back ( g_cv.capacity );   /* kapacita stávajících dat (CLI, INI, MCP) */
            std::sort ( caps.begin ( ), caps.end ( ) );
        };
        char cur[ 16 ];
        snprintf ( cur, sizeof ( cur ), "%u", g_cv.capacity );
        ImGui::SetNextItemWidth ( ImGui::CalcTextSize ( "1048576WWW" ).x + ImGui::GetFrameHeight ( ) );
        if ( ImGui::BeginCombo ( "###cov_capacity", cur ) ) {
            for ( uint32_t c : caps ) {
                char item[ 16 ];
                snprintf ( item, sizeof ( item ), "%u", c );
                if ( ImGui::Selectable ( item, c == g_cv.capacity ) ) g_cv.capacity = c;
            };
            ImGui::EndCombo ( );
        };
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::SetTooltip ( "%s", _( "Number of code versions the detail level can hold (32 bytes each).\n"
                                         "When the table is full, new versions are dropped and the data\n"
                                         "are reported as incomplete." ) );
        };
    };
    ImGui::EndDisabled ( );

    ImGui::SameLine ( );
    if ( ImGui::Button ( active ? _L ( "Stop###cov_startstop" ) : _L ( "Start###cov_startstop" ) ) ) {
        send_op ( active ? DBGAPI_COVERAGE_OP_STOP : DBGAPI_COVERAGE_OP_START );
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s", _( "Start continues into the existing data if the level and capacity\n"
                                     "are the same, otherwise it starts with empty data.\n"
                                     "Stop keeps the data." ) );
    };
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( !st->has_data );
    if ( ImGui::Button ( _L ( "Reset###cov_reset" ) ) ) {
        send_op ( DBGAPI_COVERAGE_OP_RESET );
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s", _( "Clears the data; collecting continues if it is running." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Mark###cov_mark" ) ) ) {
        send_op ( DBGAPI_COVERAGE_OP_MARK );
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s", _( "Starts a new epoch. \"New since mark\" then lists only code\n"
                                     "executed for the first time after this point,\n"
                                     "for example what a key press adds." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Export...###cov_export" ) ) ) {
        open_export_dialog ( );
    };
    ImGui::EndDisabled ( );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "(?)" );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::TextUnformatted ( _( "Code coverage" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s", _( "Records which instructions ran and which way the conditional ones went\n"
                                     "(JR, JP, CALL and RET with a condition, DJNZ, LDIR, CPIR, INIR, OTIR\n"
                                     "and the decrementing forms)." ) );
        ImGui::BulletText ( "%s", _( "Taken = the condition was met (jump, call, return, repeat)." ) );
        ImGui::BulletText ( "%s", _( "branch: by logical address, memory banks are not told apart." ) );
        ImGui::BulletText ( "%s", _( "detail: also by physical memory cell and code version (banks,\n"
                                     "self-modifying code), with counters; slows the emulation more." ) );
        ImGui::BulletText ( "%s", _( "Marks in the Disassembly windows show the same data\n"
                                     "(debugger window: Settings -> Disassembled -> Show code coverage)." ) );
        ImGui::EndTooltip ( );
    };
}


/** @brief Řádek filtru: co ukazovat a rozsah adres. */
void render_filter ( void )
{
    ImGui::TextUnformatted ( _( "Show:" ) );
    ImGui::SameLine ( );
    const char *items[ COV_SHOW_COUNT ] = {
        _( "Partial branches" ),
        _( "Both branches" ),
        _( "All conditional" ),
        _( "New since mark" ),
        _( "All" ),
    };
    ImGui::SetNextItemWidth ( ImGui::CalcTextSize ( items[ COV_SHOW_PARTIAL ] ).x * 1.4f + ImGui::GetFrameHeight ( ) );
    if ( ImGui::Combo ( "###cov_show", &g_cv.show, items, COV_SHOW_COUNT ) ) g_cv.view_dirty = true;
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Range:" ) );
    ImGui::SameLine ( );
    const float w = ImGui::CalcTextSize ( "FFFFW" ).x + ImGui::GetStyle ( ).FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth ( w );
    if ( ImGui::InputText ( "###cov_from", g_cv.from_buf, 5, ImGuiInputTextFlags_CharsHexadecimal ) ) g_cv.view_dirty = true;
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "-" );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( w );
    if ( ImGui::InputText ( "###cov_to", g_cv.to_buf, 5, ImGuiInputTextFlags_CharsHexadecimal ) ) g_cv.view_dirty = true;
}


/** @brief Tabulka řádků s akcemi. */
void render_table ( void )
{
    const st_COVERAGE_SNAPSHOT *s = dbg_cov_data_full ( );
    size_t n_rows = 0;
    const st_COVERAGE_ROW *rows = dbg_cov_data_rows ( &n_rows );
    const st_COVERAGE_STATUS *st = dbg_cov_data_status ( );
    if ( !s || !rows || !st->has_data ) {
        ImGui::TextDisabled ( "%s", st->active ? _( "Collecting - the table fills while the emulation runs." )
                                               : _( "No data. Press Start and let the emulation run." ) );
        return;
    };
    if ( g_cv.view.empty ( ) ) {
        ImGui::TextDisabled ( "%s", _( "No rows match the filter." ) );
        return;
    };

    const bool detail = ( s->status.level == COVERAGE_LEVEL_DETAIL );
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable
                          | ImGuiTableFlags_SizingFixedFit;
    if ( !ImGui::BeginTable ( "###cov_table", COV_COL_COUNT_ALL, flags ) ) return;

    const float w_hex = ImGui::CalcTextSize ( "FFFFh" ).x + 16.0f;
    const float w_sym = ImGui::CalcTextSize ( "monitor_print_string+999" ).x + 16.0f;
    const float w_state = ImGui::CalcTextSize ( "not_taken_only" ).x + 16.0f;
    const float w_num = ImGui::CalcTextSize ( "9999999999" ).x + 16.0f;
    const float w_cell = ImGui::CalcTextSize ( "memext 07FFFFh" ).x + 16.0f;
    const float w_key = ImGui::CalcTextSize ( "DD CB 00 46" ).x + 16.0f;
    const float w_epoch = ImGui::CalcTextSize ( "65535" ).x + 16.0f;
    const ImGuiTableColumnFlags fd = ImGuiTableColumnFlags_WidthFixed | ( detail ? 0 : ImGuiTableColumnFlags_Disabled );
    ImGui::TableSetupScrollFreeze ( 0, 1 );
    ImGui::TableSetupColumn ( "Addr",      ImGuiTableColumnFlags_WidthFixed, w_hex );
    ImGui::TableSetupColumn ( "Symbol",    ImGuiTableColumnFlags_WidthFixed, w_sym );
    ImGui::TableSetupColumn ( "State",     ImGuiTableColumnFlags_WidthFixed, w_state );
    ImGui::TableSetupColumn ( "Exec",      fd, w_num );
    ImGui::TableSetupColumn ( "Taken",     fd, w_num );
    ImGui::TableSetupColumn ( "Not taken", fd, w_num );
    ImGui::TableSetupColumn ( "Cell",      fd, w_cell );
    ImGui::TableSetupColumn ( "Key",       fd, w_key );
    ImGui::TableSetupColumn ( "Epoch",     ImGuiTableColumnFlags_WidthFixed, w_epoch );
    ImGui::TableSetupColumn ( "Flags",     ImGuiTableColumnFlags_WidthStretch );

    static const char *const k_tips[ COV_COL_COUNT_ALL ] = {
        N_( "Address of the instruction (logical PC).\nLeft click: open in Disassembly #1." ),
        N_( "Nearest symbol at or below the address, + offset." ),
        N_( "none = not a conditional instruction; both, taken_only, not_taken_only\n"
            "= which results of the condition were seen; unobserved = none yet." ),
        N_( "How many times this code version was executed." ),
        N_( "How many times the condition was met (jump, call, return, repeat)." ),
        N_( "How many times the condition was not met." ),
        N_( "Physical memory cell of the first byte: ram, rom, vram, pcg, memext\n"
            "and the offset; bus = no physical cell (graphic VRAM, mapped ports)." ),
        N_( "Bytes that tell code versions apart (prefixes and opcode)." ),
        N_( "Epoch of the first execution (Mark starts a new epoch)." ),
        N_( "int = first instruction of an interrupt handler, smc = the operand changed,\n"
            "cmt = the CMT hack stepped in, halt = HALT, fallback = a byte was read\n"
            "from memory instead of the CPU fetch, saturated = a counter overflowed,\n"
            "bus = no physical cell, prefixes = a step of 64 prefixes only,\n"
            "long = key longer than 4 bytes (shortened)." ),
    };
    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
    for ( int col = 0; col < COV_COL_COUNT_ALL; col++ ) {
        if ( !ImGui::TableSetColumnIndex ( col ) ) continue;
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::SetTooltip ( "%s", _( k_tips[ col ] ) );
        };
    };

    ImGuiListClipper clipper;
    clipper.Begin ( (int) g_cv.view.size ( ) );
    while ( clipper.Step ( ) ) {
        for ( int vi = clipper.DisplayStart; vi < clipper.DisplayEnd; vi++ ) {
            size_t ri = g_cv.view[ (size_t) vi ];
            if ( ri >= n_rows ) continue;
            const st_COVERAGE_ROW *r = &rows[ ri ];
            const st_COVERAGE_UNIT *u = r->unit;
            ImGui::TableNextRow ( );

            ImGui::TableSetColumnIndex ( COV_COL_ADDR );
            ImGui::PushID ( (int) ri );
            char label[ 16 ];
            snprintf ( label, sizeof ( label ), "%04Xh", r->pc );
            if ( ImGui::Selectable ( label, false, ImGuiSelectableFlags_SpanAllColumns ) ) {
                dbg_disasm_show_in_slot ( 0, r->pc );
            };
            /* Adresa se uloží při otevření menu: obnova dat (nejvýš 1x za
             * sekundu) může pod otevřeným menu posunout řádky. */
            if ( ImGui::IsMouseReleased ( ImGuiMouseButton_Right ) && ImGui::IsItemHovered ( ) ) {
                g_cv.ctx_pc = r->pc;
            };
            if ( ImGui::BeginPopupContextItem ( "###cov_row_ctx" ) ) {
                const uint16_t pc = g_cv.ctx_pc;
                if ( ImGui::MenuItem ( _L ( "Set Breakpoint###cov_bp" ) ) ) {
                    dbg_ui_bp_add ( pc, NULL );
                };
                ImGui::Separator ( );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #2###cov_d2" ) ) ) dbg_disasm_show_in_slot ( 1, pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #3###cov_d3" ) ) ) dbg_disasm_show_in_slot ( 2, pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #4###cov_d4" ) ) ) dbg_disasm_show_in_slot ( 3, pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #5###cov_d5" ) ) ) dbg_disasm_show_in_slot ( 4, pc );
                ImGui::EndPopup ( );
            };
            ImGui::PopID ( );

            ImGui::TableSetColumnIndex ( COV_COL_SYMBOL );
            unsigned off = 0;
            const st_SYMBOL *sym = nearest_symbol ( r->pc, &off );
            if ( sym && sym->name ) {
                if ( off ) ImGui::Text ( "%s+%u", sym->name, off );
                else ImGui::TextUnformatted ( sym->name );
            };

            ImGui::TableSetColumnIndex ( COV_COL_STATE );
            const char *state = coverage_row_state ( s, r );
            unsigned sbits = coverage_state_bits ( state );
            if ( sbits == COVERAGE_STATE_NONE ) ImGui::TextDisabled ( "%s", state );
            else ImGui::TextColored ( ImGui::ColorConvertU32ToFloat4 ( dbg_coverage_state_color ( sbits ) ), "%s", state );

            if ( u ) {
                if ( ImGui::TableSetColumnIndex ( COV_COL_EXEC ) ) ImGui::Text ( "%u", u->exec );
                if ( u->flags & COVERAGE_F_DECISION ) {
                    if ( ImGui::TableSetColumnIndex ( COV_COL_TAKEN ) ) ImGui::Text ( "%u", u->taken );
                    if ( ImGui::TableSetColumnIndex ( COV_COL_NOT_TAKEN ) ) ImGui::Text ( "%u", u->not_taken );
                };
                if ( ImGui::TableSetColumnIndex ( COV_COL_CELL ) ) {
                    ImGui::Text ( "%s %06Xh", coverage_space_name ( COVERAGE_CELL_SPACE ( u->cell ) ),
                                  (unsigned) COVERAGE_CELL_OFFSET ( u->cell ) );
                };
                if ( ImGui::TableSetColumnIndex ( COV_COL_KEY ) ) {
                    char kb[ 16 ];
                    key_text ( u, kb, sizeof ( kb ) );
                    ImGui::TextUnformatted ( kb );
                };
            };

            ImGui::TableSetColumnIndex ( COV_COL_EPOCH );
            ImGui::Text ( "%u", (unsigned) coverage_row_epoch ( s, r ) );

            ImGui::TableSetColumnIndex ( COV_COL_FLAGS );
            char fb[ 96 ];
            flags_text ( coverage_row_flags ( s, r ), fb, sizeof ( fb ) );
            ImGui::TextUnformatted ( fb );
        };
    };
    ImGui::EndTable ( );
}


/** @brief Stavový řádek: běh, úroveň, instrukce, větve, úplnost, epocha. */
void render_status ( void )
{
    const st_COVERAGE_STATUS *s = dbg_cov_data_status ( );
    if ( s->active ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 80, 220, 120, 255 ) );
        ImGui::TextUnformatted ( _( "● RUNNING" ) );
        ImGui::PopStyleColor ( );
    }
    else {
        ImGui::TextDisabled ( "%s", _( "○ STOPPED" ) );
    };
    if ( !s->has_data ) return;
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "%s", s->level == COVERAGE_LEVEL_DETAIL ? "detail" : "branch" );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Instructions: %llu" ), (unsigned long long) s->instructions );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Branches: %u, both %u, taken only %u, not taken only %u" ),
                  s->decisions, s->branches_both, s->branches_taken_only, s->branches_not_taken_only );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    if ( s->complete ) {
        ImGui::TextUnformatted ( _( "Complete" ) );
    }
    else {
        ImGui::TextColored ( ImVec4 ( 1.0f, 0.6f, 0.2f, 1.0f ), "%s", _( "Incomplete" ) );
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( _( "Units: %u of %u\nDropped steps (table full): %llu\n"
                               "Bytes read from memory instead of the CPU fetch: %llu\n"
                               "Counter overflow: %s\n"
                               "Discontinuities (reset, snapshot load, replay seek): %u\n"
                               "Host edits of PC or F: %u" ),
                            s->units, s->capacity, (unsigned long long) s->dropped,
                            (unsigned long long) s->fetch_fallback, s->saturated ? _( "yes" ) : _( "no" ),
                            s->discontinuities, s->host_state_edits );
    };
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Epoch: %u" ), s->epoch );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Rows: %u" ), (unsigned) g_cv.view.size ( ) );
}

} /* namespace */


extern "C" uint32_t dbg_coverage_state_color ( unsigned state )
{
    switch ( state ) {
        case COVERAGE_STATE_NONE:
        case COVERAGE_STATE_BOTH:           return IM_COL32 ( 70, 200, 90, 255 );
        case COVERAGE_STATE_TAKEN_ONLY:     return IM_COL32 ( 235, 205, 40, 255 );
        case COVERAGE_STATE_NOT_TAKEN_ONLY: return IM_COL32 ( 245, 130, 40, 255 );
        case COVERAGE_STATE_UNOBSERVED:     return IM_COL32 ( 150, 150, 150, 255 );
        default:                            return IM_COL32 ( 0, 0, 0, 0 );
    };
}


extern "C" const char *dbg_coverage_state_label ( unsigned state )
{
    switch ( state ) {
        case COVERAGE_STATE_NONE:           return _( "Executed" );
        case COVERAGE_STATE_BOTH:           return _( "Both branches" );
        case COVERAGE_STATE_TAKEN_ONLY:     return _( "Taken only (condition always met)" );
        case COVERAGE_STATE_NOT_TAKEN_ONLY: return _( "Not taken only (condition never met)" );
        case COVERAGE_STATE_UNOBSERVED:     return _( "Condition not observed" );
        default:                            return _( "Not executed" );
    };
}


extern "C" void dbg_coverage_render_tooltip ( uint16_t pc )
{
    dbg_cov_data_poll ( true );
    unsigned state = dbg_cov_data_pc_state ( pc );
    ImGui::TextColored ( ImVec4 ( 0.6f, 0.9f, 0.7f, 1.0f ), "%s:", _( "Coverage" ) );
    ImGui::SameLine ( );
    if ( state ) ImGui::TextColored ( ImGui::ColorConvertU32ToFloat4 ( dbg_coverage_state_color ( state ) ), "%s",
                                      dbg_coverage_state_label ( state ) );
    else ImGui::TextDisabled ( "%s", dbg_coverage_state_label ( state ) );

    const st_COVERAGE_SNAPSHOT *s = dbg_cov_data_full ( );
    if ( !s || !state ) return;
    if ( ( s->int_entry[ pc >> 3 ] >> ( pc & 7u ) ) & 1u ) {
        ImGui::TextUnformatted ( _( "  First instruction of an interrupt handler" ) );
    };
    if ( s->status.level != COVERAGE_LEVEL_DETAIL ) {
        ImGui::Text ( _( "  First executed in epoch %u" ), (unsigned) s->first_epoch[ pc ] );
        return;
    };

    size_t first = 0;
    size_t n = dbg_cov_data_rows_for_pc ( pc, &first );
    size_t n_rows = 0;
    const st_COVERAGE_ROW *rows = dbg_cov_data_rows ( &n_rows );
    if ( n > 1 ) ImGui::Text ( _( "  Code versions at this address: %u" ), (unsigned) n );
    const size_t k_max = 8;
    for ( size_t k = 0; k < n && k < k_max; k++ ) {
        const st_COVERAGE_UNIT *u = rows[ first + k ].unit;
        if ( !u ) continue;
        char kb[ 16 ];
        key_text ( u, kb, sizeof ( kb ) );
        if ( u->flags & COVERAGE_F_DECISION ) {
            ImGui::Text ( _( "  %s %06Xh [%s]: executed %u, taken %u, not taken %u" ),
                          coverage_space_name ( COVERAGE_CELL_SPACE ( u->cell ) ),
                          (unsigned) COVERAGE_CELL_OFFSET ( u->cell ), kb, u->exec, u->taken, u->not_taken );
        }
        else {
            ImGui::Text ( _( "  %s %06Xh [%s]: executed %u" ),
                          coverage_space_name ( COVERAGE_CELL_SPACE ( u->cell ) ),
                          (unsigned) COVERAGE_CELL_OFFSET ( u->cell ), kb, u->exec );
        };
        char fb[ 96 ];
        flags_text ( (uint16_t) ( u->flags & ~COVERAGE_F_DECISION ), fb, sizeof ( fb ) );
        if ( fb[ 0 ] ) {
            ImGui::SameLine ( );
            ImGui::TextDisabled ( "(%s)", fb );
        };
    };
    if ( n > k_max ) ImGui::TextDisabled ( _( "  ... and %u more (see CPU Profiler -> Coverage)" ),
                                           (unsigned) ( n - k_max ) );
}


extern "C" void dbg_coverage_render ( void )
{
    ImGui::PushID ( "cov" );
    dbg_cov_data_poll ( true );
    sym_cache_update ( );
    view_update ( );

    render_toolbar ( );
    render_filter ( );
    ImGui::Separator ( );

    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float footer_h = ImGui::GetTextLineHeightWithSpacing ( ) + ImGui::GetStyle ( ).ItemSpacing.y * 2.0f;
    float table_h = avail.y - footer_h;
    if ( table_h < 60.0f ) table_h = 60.0f;
    ImGui::BeginChild ( "###cov_table_child", ImVec2 ( 0.0f, table_h ), false, ImGuiWindowFlags_None );
    render_table ( );
    ImGui::EndChild ( );

    ImGui::Separator ( );
    render_status ( );

    render_export_dialog ( );
    if ( g_cv.error_queue ) {
        g_cv.error_queue = false;
        ImGui::OpenPopup ( "###cov_error" );
    };
    if ( ImGui::BeginPopupModal ( _L ( "Coverage error###cov_error" ), NULL,
                                  ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::TextUnformatted ( g_cv.error );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "OK###cov_error_ok" ), ImVec2 ( 120.0f, 0.0f ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };
    ImGui::PopID ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
