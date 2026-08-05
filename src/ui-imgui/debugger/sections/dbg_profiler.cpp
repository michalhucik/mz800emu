/*
 * dbg_profiler.cpp - implementace CPU Profiler panelu (mutant profiler V1, F3).
 *
 * Renderuje per-funkční agregát z profileru jako sortable tabulku
 * (Function / Addr / Kind / Calls / Excl% / Excl / Incl / Avg / Min /
 * Max), nad ní toolbar (Start/Stop / Reset / filter / Kind checkboxy)
 * a pod ní status bar (Running/Stopped + counters).
 *
 * Refresh: vlastní throttle 250 ms (= 4 Hz polling) pres dbgapi
 * CMD_GET_PROFILER. Mezi refresh ticky se renderuje cache poslední
 * úspěšné odpovědi. Snapshot ownership: handler v EMU vlákně alokuje
 * pole entries (g_malloc), UI ho kopíruje do lokálního std::vector
 * a okamžitě uvolní přes profiler_snapshot_free.
 *
 * Symbol resolution: per-frame na render time přes sym_db_lookup_by_addr
 * pro každý entry.target_addr. Žádné cachování (V1 = jednoduchost).
 *
 * Sortování: ImGui table sort spec API (jeden sloupec aktivní). Default
 * = Excl% desc. Sort se přepočítá pri změně sort spec nebo po refresh.
 *
 * Filter: substring match (case-insensitive) přes name (sym_db) + hex
 * addr string. Kind checkboxy = bitmask, default vše ON.
 *
 * Threading: pouze UI vlákno.
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
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <algorithm>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "i18n.h"

#include "dbg_profiler.h"
#include "ui-imgui/debugger/profiler/profiler_window.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"  /* dbg_disasm_show_in_slot */
#include "ui-imgui/debugger/dbgapi_helpers.h"             /* dbg_ui_bp_add */
#include "ui-imgui/bootstrap/myimgui.h"                   /* g_gui->showBookmarksWindow */

extern "C" {
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/callstack.h"
#include "emulator/debugger/profiler.h"
#include "emulator/debugger/symbols/sym_db.h"
#include "emulator/debugger/bookmarks/bookmarks.h"        /* bookmarks_add */
}


/* g_dbgapi_cmdrq_queue je definovaný v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


/* ========================================================================= */
/*  Panel state                                                              */
/* ========================================================================= */

namespace {

/**
 * @brief Persistent panel state mezi framy.
 *
 * Cache poslední odpovědi dbgapi + UI preference. Single-thread access
 * (jen UI vlákno).
 *
 * @invariant valid == false na startu, true až po prvním úspěšném
 *            CMD_GET_PROFILER.
 * @invariant entries.size() == stats.entry_count po každém úspěšném
 *            refresh (= synchronizováno).
 */
struct ProfilerPanelState {
    /* Lokální kopie entries (= bezpečné držet mezi framy, callee
     * snapshot je uvolněn ihned po kopii). */
    std::vector< st_PROF_ENTRY > entries;

    /* Inline stats kopie z dbgapi GET payload. */
    uint64_t total_cycles_64   = 0;
    uint64_t total_calls       = 0;
    uint32_t irq_entries       = 0;
    uint32_t unmatched_returns = 0;
    uint32_t max_depth_reached = 0;
    uint32_t overflow_count    = 0;
    bool     active            = false;
    bool     valid             = false;

    /* UI control state. */
    char filter_buf[ 64 ]   = "";
    bool kind_show_call     = true;
    bool kind_show_rst      = true;
    bool kind_show_irq      = true;
    bool kind_show_nmi      = true;
    /* IRQ-in-parent toggle: V1 ovlivňuje jen UI viditelnost IRQ řádků.
     * Skutečné per-call přeúčtování je V1.5+ (= vyžaduje per-call
     * data, ne agregát). */
    bool irq_in_parent      = true;

    /* Refresh throttle (250 ms). */
    double last_refresh_time_s = 0.0;

    /* Pre-filtrované pole indexů do entries (= aktuálně viditelné řádky
     * po aplikaci filter_buf + kind toggles). Sort spec ho přerovnává
     * pri změně sort sloupce. */
    std::vector< int > filtered_idx;

    /* CSV export dialog state (F4). Drží otevřený file dialog mezi framy
     * dokud uživatel nepotvrdí / nezruší. Error popup zobrazí, pokud
     * fopen / fwrite selže. */
    bool export_dialog_open       = false;
    bool export_error_popup_queue = false;
    char export_error_msg[ 256 ]  = "";
};

ProfilerPanelState g_pf;

}  /* anonymous namespace */


/* ========================================================================= */
/*  Pomocné funkce                                                           */
/* ========================================================================= */

/**
 * @brief Převede en_CALLSTACK_ENTRY_KIND na čitelný short text.
 *
 * Vrací statický string (= bez ownership), bezpečný pro ImGui::Text*.
 *
 * @param kind  Hodnota z en_CALLSTACK_ENTRY_KIND (cast přes uint8_t).
 * @return Krátký textový popis (např. "CALL", "RST", "IRQ-IM2").
 */
static const char *kind_to_str ( uint8_t kind )
{
    switch ( kind ) {
        case CS_KIND_CALL:    return "CALL";
        case CS_KIND_RST:     return "RST";
        case CS_KIND_IRQ_IM0: return "IRQ-IM0";
        case CS_KIND_IRQ_IM1: return "IRQ-IM1";
        case CS_KIND_IRQ_IM2: return "IRQ-IM2";
        case CS_KIND_NMI:     return "NMI";
        default:              return "?";
    };
}


/**
 * @brief Case-insensitive substring match (ASCII).
 *
 * @param hay   Prohledávaný řetězec (NULL = no match).
 * @param needle Hledaný substring. Prázdný string = match (true).
 * @return true pokud needle je substring hay (case-insensitive).
 */
static bool str_icontains ( const char *hay, const char *needle )
{
    if ( !needle || !needle[ 0 ] ) return true;
    if ( !hay ) return false;
    size_t hlen = strlen ( hay );
    size_t nlen = strlen ( needle );
    if ( nlen > hlen ) return false;
    for ( size_t i = 0; i + nlen <= hlen; i++ ) {
        size_t k = 0;
        for ( ; k < nlen; k++ ) {
            unsigned char a = (unsigned char) hay[ i + k ];
            unsigned char b = (unsigned char) needle[ k ];
            if ( tolower ( a ) != tolower ( b ) ) break;
        };
        if ( k == nlen ) return true;
    };
    return false;
}


/**
 * @brief Posoudí, zda IRQ filtr propustí řádek tohoto kindu.
 *
 * @param kind  uint8_t z en_CALLSTACK_ENTRY_KIND.
 * @return true pokud je IRQ_IM0/IM1/IM2.
 */
static bool kind_is_irq ( uint8_t kind )
{
    return kind == CS_KIND_IRQ_IM0
        || kind == CS_KIND_IRQ_IM1
        || kind == CS_KIND_IRQ_IM2;
}


/* ========================================================================= */
/*  Refresh - volání dbgapi CMD_GET_PROFILER                                 */
/* ========================================================================= */

/**
 * @brief Refresh snapshot agregátoru z emulátoru.
 *
 * Volá dbgapi CMD_GET_PROFILER. Handler v EMU vlákně alokuje pole
 * entries přes profiler_snapshot_get (callee-allocated, g_malloc) -
 * UI ho zkopíruje do g_pf.entries (std::vector) a okamžitě uvolní
 * přes profiler_snapshot_free.
 *
 * Self-rate-limit (skip pri plné cmdrq frontě) + 50 ms timeout konsistentní
 * se vzorem dbg_callstack.cpp.
 *
 * Side effects: aktualizuje g_pf (entries, stats, valid).
 *               Při dbgapi chybě cache zůstane (= zachová poslední
 *               platný snímek).
 */
static void profiler_refresh ( void )
{
    if ( dbgapi_ui_queue_is_full ( &g_dbgapi_cmdrq_queue ) ) {
        return;
    };

    st_DBGAPI_PROFILER_GET_PARAM p;
    memset ( &p, 0, sizeof ( p ) );

    if ( !dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue,
                                       DBGAPI_CMD_GET_PROFILER,
                                       &p, NULL, 50 ) )
    {
        return;
    };

    /* Kopie do lokální cache. Vlastnictví entries pres p.entries -
     * uvolnit po kopii přes profiler_snapshot_free (= vyžaduje
     * zrekonstruovaný st_PROF_SNAPSHOT). */
    g_pf.entries.clear ( );
    if ( p.entries && p.entry_count > 0 ) {
        const st_PROF_ENTRY *src = (const st_PROF_ENTRY *) p.entries;
        g_pf.entries.assign ( src, src + p.entry_count );
    };
    g_pf.total_cycles_64   = p.total_cycles_64;
    g_pf.total_calls       = p.total_calls;
    g_pf.irq_entries       = p.irq_entries;
    g_pf.unmatched_returns = p.unmatched_returns;
    g_pf.max_depth_reached = p.max_depth_reached;
    g_pf.overflow_count    = p.overflow_count;
    g_pf.active            = ( p.active != 0 );
    g_pf.valid             = true;

    /* Uvolnit callee-allocated buffer (snapshot ownership patří caller). */
    if ( p.entries ) {
        st_PROF_SNAPSHOT snap;
        memset ( &snap, 0, sizeof ( snap ) );
        snap.entries     = (st_PROF_ENTRY *) p.entries;
        snap.entry_count = p.entry_count;
        profiler_snapshot_free ( &snap );
    };
}


/**
 * @brief Pošle CMD_PROFILER_SET_ACTIVE do EMU vlákna.
 *
 * Synchronní (= 100 ms timeout). Po návratu refresh přepíše g_pf.active
 * při dalším refresh ticku.
 *
 * @param enable true = zapnout profiler, false = vypnout.
 */
static void profiler_send_set_active ( bool enable )
{
    st_DBGAPI_PROFILER_SET_ACTIVE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.active = enable ? 1 : 0;
    (void) dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_PROFILER_SET_ACTIVE,
                                        &p, NULL, 100 );
}


/**
 * @brief Pošle CMD_PROFILER_RESET do EMU vlákna.
 *
 * Synchronní (= 100 ms timeout). Po návratu vyčistí lokální cache,
 * aby UI nezobrazoval staré řádky do prvního refresh ticku.
 */
static void profiler_send_reset ( void )
{
    (void) dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue,
                                        DBGAPI_CMD_PROFILER_RESET,
                                        NULL, NULL, 100 );
    g_pf.entries.clear ( );
    g_pf.filtered_idx.clear ( );
    g_pf.total_calls       = 0;
    g_pf.irq_entries       = 0;
    g_pf.unmatched_returns = 0;
    g_pf.max_depth_reached = 0;
    g_pf.overflow_count    = 0;
    g_pf.total_cycles_64   = 0;
}


/* ========================================================================= */
/*  Filter + sort pipeline                                                   */
/* ========================================================================= */

/**
 * @brief Naplní g_pf.filtered_idx podle aktuálních filter_buf + Kind toggles.
 *
 * Iteruje g_pf.entries, pro každý entry rozhoduje:
 *   - Kind filter (CALL/RST/IRQ/NMI checkboxy);
 *   - IRQ-in-parent toggle - pokud OFF, skryje IRQ řádky úplně;
 *   - substring match přes sym name a hex addr (case-insensitive).
 *
 * Side effect: rewrite g_pf.filtered_idx.
 */
static void rebuild_filtered_idx ( void )
{
    g_pf.filtered_idx.clear ( );
    const int n = (int) g_pf.entries.size ( );
    for ( int i = 0; i < n; i++ ) {
        const st_PROF_ENTRY &e = g_pf.entries[ i ];

        /* Kind filter. */
        bool show = false;
        switch ( e.kind ) {
            case CS_KIND_CALL:    show = g_pf.kind_show_call; break;
            case CS_KIND_RST:     show = g_pf.kind_show_rst;  break;
            case CS_KIND_IRQ_IM0:
            case CS_KIND_IRQ_IM1:
            case CS_KIND_IRQ_IM2: show = g_pf.kind_show_irq;  break;
            case CS_KIND_NMI:     show = g_pf.kind_show_nmi;  break;
            default:              show = false;
        };
        if ( !show ) continue;

        /* IRQ-in-parent toggle OFF → schovej IRQ úplně (V1 viditelnost,
         * V1.5+ skutečný attribution rework). */
        if ( !g_pf.irq_in_parent && kind_is_irq ( e.kind ) ) continue;

        /* Substring filter (sym name + addr hex). */
        if ( g_pf.filter_buf[ 0 ] ) {
            const st_SYMBOL *sym = sym_db_lookup_by_addr (
                (uint32_t) e.target_addr, 0 );
            const char *name = ( sym && sym->name ) ? sym->name : NULL;
            char addr_str[ 8 ];
            snprintf ( addr_str, sizeof ( addr_str ), "%04X", e.target_addr );
            if ( !str_icontains ( name, g_pf.filter_buf )
                 && !str_icontains ( addr_str, g_pf.filter_buf ) ) {
                continue;
            };
        };

        g_pf.filtered_idx.push_back ( i );
    };
}


/**
 * @brief Sort comparator podle aktivní sort spec.
 *
 * Volaný z std::sort při zpracování ImGuiTableSortSpecs. Sloupce:
 *   0 = Function, 1 = Addr, 2 = Kind, 3 = Calls, 4 = Excl%,
 *   5 = Excl, 6 = Incl, 7 = Avg, 8 = Min, 9 = Max.
 *
 * Excl% je monotónní vůči Excl při společném total_excl, takže
 * stačí sortovat podle Excl (= identické pořadí).
 */
struct ProfSortCtx {
    int  col;
    bool ascending;
};

static bool prof_sort_cmp ( const ProfSortCtx &ctx, int ai, int bi )
{
    const st_PROF_ENTRY &a = g_pf.entries[ ai ];
    const st_PROF_ENTRY &b = g_pf.entries[ bi ];

    auto cmp64 = [ ] ( uint64_t x, uint64_t y ) -> int {
        if ( x < y ) return -1;
        if ( x > y ) return 1;
        return 0;
    };

    int r = 0;
    switch ( ctx.col ) {
        case 0: {
            const st_SYMBOL *sa = sym_db_lookup_by_addr ( (uint32_t) a.target_addr, 0 );
            const st_SYMBOL *sb = sym_db_lookup_by_addr ( (uint32_t) b.target_addr, 0 );
            const char *na = ( sa && sa->name ) ? sa->name : "";
            const char *nb = ( sb && sb->name ) ? sb->name : "";
            r = strcmp ( na, nb );
            /* Sekundární tie-break - addr (= unique). */
            if ( r == 0 ) r = (int) a.target_addr - (int) b.target_addr;
            break;
        };
        case 1: r = (int) a.target_addr - (int) b.target_addr; break;
        case 2: r = (int) a.kind        - (int) b.kind;        break;
        case 3: r = cmp64 ( a.calls, b.calls );                break;
        case 4:
        case 5: r = cmp64 ( a.cycles_excl_sum, b.cycles_excl_sum ); break;
        case 6: r = cmp64 ( a.cycles_incl_sum, b.cycles_incl_sum ); break;
        case 7: {
            uint64_t avg_a = a.calls ? a.cycles_incl_sum / a.calls : 0;
            uint64_t avg_b = b.calls ? b.cycles_incl_sum / b.calls : 0;
            r = cmp64 ( avg_a, avg_b );
            break;
        };
        case 8: r = cmp64 ( a.cycles_incl_min, b.cycles_incl_min ); break;
        case 9: r = cmp64 ( a.cycles_incl_max, b.cycles_incl_max ); break;
        default: r = 0;
    };
    return ctx.ascending ? ( r < 0 ) : ( r > 0 );
}


/**
 * @brief Aplikuje aktuální ImGui sort spec na g_pf.filtered_idx.
 *
 * Default sort (= bez user-aktivace) = Excl% desc. Specs->SpecsCount
 * == 0 nastane při fresh open okna.
 */
static void apply_sort ( ImGuiTableSortSpecs *specs )
{
    ProfSortCtx ctx;
    ctx.col       = 4;       /* Default = Excl% column. */
    ctx.ascending = false;

    if ( specs && specs->SpecsCount > 0 ) {
        const ImGuiTableColumnSortSpecs &s = specs->Specs[ 0 ];
        ctx.col       = s.ColumnIndex;
        ctx.ascending = ( s.SortDirection == ImGuiSortDirection_Ascending );
    };

    std::sort ( g_pf.filtered_idx.begin ( ), g_pf.filtered_idx.end ( ),
                [ &ctx ] ( int ai, int bi ) {
                    return prof_sort_cmp ( ctx, ai, bi );
                } );
}


/* ========================================================================= */
/*  CSV export (F4)                                                          */
/* ========================================================================= */

/**
 * @brief Minimální RFC 4180 quote helper pro CSV pole.
 *
 * Pokud vstupní řetězec obsahuje znaky vyžadující quoting (`,` `"` `\n` `\r`),
 * obalí ho dvojitými uvozovkami a všechna interní `"` zdvojí. Pokud žádný
 * z těchto znaků neobsahuje, vrací kopii beze změny.
 *
 * Výstupní buffer je vždy NUL terminated. Pokud výsledek nevejde do @p outsz,
 * je oříznut (ale stále NUL terminated). NULL vstup vyrobí prázdný string.
 *
 * @param out    Cílový buffer pro quoted výstup.
 * @param outsz  Velikost cílového bufferu v bytech (musí být >= 1).
 * @param in     Vstupní řetězec (může být NULL).
 */
static void csv_quote ( char *out, size_t outsz, const char *in )
{
    if ( !out || outsz == 0 ) return;
    out[ 0 ] = 0;
    if ( !in ) return;

    bool need_quote = false;
    for ( const char *p = in; *p; p++ ) {
        if ( *p == ',' || *p == '"' || *p == '\n' || *p == '\r' ) {
            need_quote = true;
            break;
        };
    };

    if ( !need_quote ) {
        g_strlcpy ( out, in, outsz );
        return;
    };

    /* Quoted varianta - obalit do " ... " a zdvojit každý interní ". */
    size_t o = 0;
    if ( o + 1 >= outsz ) { out[ o ] = 0; return; };
    out[ o++ ] = '"';
    for ( const char *p = in; *p; p++ ) {
        if ( *p == '"' ) {
            if ( o + 2 >= outsz ) break;
            out[ o++ ] = '"';
            out[ o++ ] = '"';
        }
        else {
            if ( o + 1 >= outsz ) break;
            out[ o++ ] = *p;
        };
    };
    if ( o + 1 < outsz ) out[ o++ ] = '"';
    out[ o ] = 0;
}


/**
 * @brief Zapíše CSV soubor s daty filtrovaného setu profileru.
 *
 * Formát: čistý UTF-8 (bez BOM, moderní Excel UTF-8 detekuje sám),
 * jeden hlavičkový řádek, poté jeden řádek per index v @p filtered_idx.
 * Sloupce: `Function,Address,Kind,Calls,Exclusive_cycles,Inclusive_cycles,
 * Exclusive_pct,Inclusive_pct,Avg,Min,Max`.
 *
 * - Function = sym_db název (nebo `func_XXXX` fallback), quoted dle
 *   RFC 4180 (`csv_quote`).
 * - Address = 4 hex znaky s `h` suffixem (např. `1A00h`).
 * - Kind = krátký string z `kind_to_str` (`CALL`, `RST`, `IRQ-IM2`, ...).
 * - Calls / Excl / Incl / Avg / Min / Max = uint64 decimal.
 * - Excl% / Incl% = `%.4f` přes `g_ascii_formatd` (= locale-safe,
 *   vždy `.` jako desetinný oddělovač - jinak by česká locale `cs_CZ`
 *   vypsala `0,2521` a rozbila by CSV strukturu protože `,` je
 *   zároveň separator).
 *
 * Při fopen / fwrite chybě vrátí false a naplní @p err_msg krátkým popisem
 * (caller pak otevře error popup). Při success vrací true.
 *
 * @param path           Cesta k cílovému CSV souboru (UTF-8, g_fopen mu rozumí).
 * @param filtered_idx   Indexy do @p entries vybrané aktuálním filtrem + sort.
 * @param entries        Pole st_PROF_ENTRY (pole musí být alive po dobu volání).
 * @param entry_count    Počet validních entries (= bounds check).
 * @param total_excl     Suma cycles_excl_sum přes filtered_idx (= jmenovatel %).
 * @param total_incl     Suma cycles_incl_sum přes filtered_idx (= jmenovatel %).
 * @param err_msg        Out buffer pro chybový popis (zapíše se jen pri false).
 * @param err_msg_sz     Velikost @p err_msg v bytech.
 * @return true pokud byl soubor zapsán bez chyby, jinak false.
 */
static bool dbg_profiler_export_csv ( const char *path,
                                       const std::vector< int > &filtered_idx,
                                       const st_PROF_ENTRY *entries,
                                       size_t entry_count,
                                       uint64_t total_excl,
                                       uint64_t total_incl,
                                       char *err_msg,
                                       size_t err_msg_sz )
{
    if ( err_msg && err_msg_sz ) err_msg[ 0 ] = 0;

    FILE *f = g_fopen ( path, "wb" );
    if ( !f ) {
        if ( err_msg && err_msg_sz ) {
            snprintf ( err_msg, err_msg_sz,
                       "Cannot open '%s' for writing (errno=%d).",
                       path, errno );
        };
        return false;
    };

    /* Čisté UTF-8 bez BOM. Moderní Excel UTF-8 detekuje sám; BOM
     * by se v nástrojích bez BOM-awareness zobrazoval jako patvar
     * "" před prvním sloupcem. */

    /* Hlavička. */
    if ( fputs ( "Function,Address,Kind,Calls,Exclusive_cycles,"
                 "Inclusive_cycles,Exclusive_pct,Inclusive_pct,"
                 "Avg,Min,Max\n", f ) < 0 ) goto write_err;

    for ( int idx : filtered_idx ) {
        if ( idx < 0 || (size_t) idx >= entry_count ) continue;
        const st_PROF_ENTRY &e = entries[ idx ];

        /* Function name - sym_db lookup s fallbackem na func_XXXX. */
        const st_SYMBOL *sym =
            sym_db_lookup_by_addr ( (uint32_t) e.target_addr, 0 );
        const char *name_raw = ( sym && sym->name && sym->name[ 0 ] )
                               ? sym->name : NULL;
        char fallback_name[ 32 ];
        if ( !name_raw ) {
            snprintf ( fallback_name, sizeof ( fallback_name ),
                       "func_%04X", e.target_addr );
            name_raw = fallback_name;
        };
        char quoted_name[ 512 ];
        csv_quote ( quoted_name, sizeof ( quoted_name ), name_raw );

        double excl_pct = ( total_excl > 0 )
            ? ( 100.0 * (double) e.cycles_excl_sum / (double) total_excl )
            : 0.0;
        double incl_pct = ( total_incl > 0 )
            ? ( 100.0 * (double) e.cycles_incl_sum / (double) total_incl )
            : 0.0;
        uint64_t avg = ( e.calls > 0 )
            ? ( e.cycles_incl_sum / e.calls )
            : 0;

        /* Pozn.: %llu cast pattern stejný jako v render_table (F3).
         * Procenta formátujeme přes g_ascii_formatd (= vždy '.' jako
         * desetinný oddělovač, locale-safe). Bez toho by česká locale
         * cs_CZ produkovala "0,2521" a rozbila by CSV. */
        char excl_pct_buf[ G_ASCII_DTOSTR_BUF_SIZE ];
        char incl_pct_buf[ G_ASCII_DTOSTR_BUF_SIZE ];
        g_ascii_formatd ( excl_pct_buf, sizeof ( excl_pct_buf ),
                          "%.4f", excl_pct );
        g_ascii_formatd ( incl_pct_buf, sizeof ( incl_pct_buf ),
                          "%.4f", incl_pct );
        if ( fprintf ( f,
                       "%s,%04Xh,%s,%llu,%llu,%llu,%s,%s,%llu,%llu,%llu\n",
                       quoted_name,
                       e.target_addr,
                       kind_to_str ( e.kind ),
                       (unsigned long long) e.calls,
                       (unsigned long long) e.cycles_excl_sum,
                       (unsigned long long) e.cycles_incl_sum,
                       excl_pct_buf,
                       incl_pct_buf,
                       (unsigned long long) avg,
                       (unsigned long long) e.cycles_incl_min,
                       (unsigned long long) e.cycles_incl_max ) < 0 )
        {
            goto write_err;
        };
    };

    if ( fclose ( f ) != 0 ) {
        f = NULL;
        if ( err_msg && err_msg_sz ) {
            snprintf ( err_msg, err_msg_sz,
                       "Failed to close '%s' (errno=%d).", path, errno );
        };
        return false;
    };
    return true;

write_err:
    if ( err_msg && err_msg_sz ) {
        snprintf ( err_msg, err_msg_sz,
                   "Write error on '%s' (errno=%d).", path, errno );
    };
    if ( f ) fclose ( f );
    return false;
}


/**
 * @brief Otevře IGFD file dialog pro výběr cílového CSV souboru.
 *
 * Default jméno: `profiler_YYYYMMDD_hhmmss.csv` (= unique per export
 * spuštění). Pokud uživatel dialog potvrdí, `render_export_dialog`
 * v dalším framu cestu vyzvedne a zavolá `dbg_profiler_export_csv`.
 *
 * Side effect: nastaví `g_pf.export_dialog_open = true`.
 */
static void open_export_dialog ( void )
{
    g_pf.export_dialog_open = true;

    /* Default jméno s timestampem. */
    static char default_name[ 64 ];
    time_t now = time ( NULL );
    struct tm *tmv = localtime ( &now );
    if ( tmv ) {
        snprintf ( default_name, sizeof ( default_name ),
                   "profiler_%04d%02d%02d_%02d%02d%02d.csv",
                   tmv->tm_year + 1900,
                   tmv->tm_mon + 1,
                   tmv->tm_mday,
                   tmv->tm_hour,
                   tmv->tm_min,
                   tmv->tm_sec );
    }
    else {
        snprintf ( default_name, sizeof ( default_name ),
                   "profiler.csv" );
    };

    IGFD::FileDialogConfig config;
    config.path             = ".";
    config.fileName         = default_name;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ShowDevicesButton
                 | ImGuiFileDialogFlags_ConfirmOverwrite;

    ImGuiFileDialog::Instance ( )->OpenDialog (
        "ProfilerExportDialog",
        _( "Export profiler data as CSV" ),
        ".csv,.*",
        config );
}


/**
 * @brief Render export dialogu - musí být volán každý frame po jeho otevření.
 *
 * Pokud uživatel potvrdí, sestaví total_excl / total_incl z filtrovaného
 * setu a zavolá `dbg_profiler_export_csv`. Pri I/O chybě nastaví error
 * popup queue flag (= popup se otevře v `dbg_profiler_render`).
 *
 * Side effects: může resetovat `g_pf.export_dialog_open` na false.
 */
static void render_export_dialog ( void )
{
    if ( !g_pf.export_dialog_open ) return;

    ImGui::SetNextWindowSize ( ImVec2 ( 800.0f, 500.0f ),
                               ImGuiCond_FirstUseEver );
    if ( ImGuiFileDialog::Instance ( )->Display ( "ProfilerExportDialog" ) ) {
        if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
            std::string file_path =
                ImGuiFileDialog::Instance ( )->GetFilePathName ( );

            /* Spočítat totals z filtrovaného setu (= identický základ
             * jako sloupec Excl% v render_table). */
            uint64_t total_excl = 0;
            uint64_t total_incl = 0;
            for ( int idx : g_pf.filtered_idx ) {
                if ( idx < 0 || (size_t) idx >= g_pf.entries.size ( ) ) continue;
                total_excl += g_pf.entries[ idx ].cycles_excl_sum;
                total_incl += g_pf.entries[ idx ].cycles_incl_sum;
            };

            char err_msg[ 256 ];
            err_msg[ 0 ] = 0;
            bool ok = dbg_profiler_export_csv (
                file_path.c_str ( ),
                g_pf.filtered_idx,
                g_pf.entries.data ( ),
                g_pf.entries.size ( ),
                total_excl,
                total_incl,
                err_msg, sizeof ( err_msg ) );

            if ( !ok ) {
                g_strlcpy ( g_pf.export_error_msg, err_msg,
                            sizeof ( g_pf.export_error_msg ) );
                g_pf.export_error_popup_queue = true;
            };
        };
        ImGuiFileDialog::Instance ( )->Close ( );
        g_pf.export_dialog_open = false;
    };
}


/* ========================================================================= */
/*  Render - toolbar                                                         */
/* ========================================================================= */

/**
 * @brief Vykreslí horní toolbar (Start/Stop, Reset, filter, Kind toggles).
 */
static void render_toolbar ( void )
{
    /* Start/Stop tlačítko - label se mění podle aktuálního stavu.
     * Stable ID přes ### aby přechod Start <-> Stop neztratil hover/
     * focus state. */
    const bool active = g_pf.active;
    const char *btn_label = active ? _L ( "Stop###pf_startstop" )
                                   : _L ( "Start###pf_startstop" );
    if ( ImGui::Button ( btn_label ) ) {
        profiler_send_set_active ( !active );
    };

    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Reset###pf_reset" ) ) ) {
        profiler_send_reset ( );
    };

    ImGui::SameLine ( );
    /* Export CSV (F4) - otevře IGFD dialog, výsledek zapíše
     * `dbg_profiler_export_csv` v render_export_dialog. Disabled pokud
     * není co exportovat (= prázdná tabulka po filtru). */
    ImGui::BeginDisabled ( g_pf.filtered_idx.empty ( ) );
    if ( ImGui::Button ( _L ( "Export CSV...###pf_export" ) ) ) {
        open_export_dialog ( );
    };
    ImGui::EndDisabled ( );

    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( 200.0f );
    ImGui::InputTextWithHint ( "###pf_filter",
                                _L ( "Filter (name or addr)" ),
                                g_pf.filter_buf, sizeof ( g_pf.filter_buf ) );

    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "CALL###pf_kind_call" ), &g_pf.kind_show_call );
    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "RST###pf_kind_rst" ),   &g_pf.kind_show_rst );
    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "IRQ###pf_kind_irq" ),   &g_pf.kind_show_irq );
    ImGui::SameLine ( );
    ImGui::Checkbox ( _L ( "NMI###pf_kind_nmi" ),   &g_pf.kind_show_nmi );

    /* Hint marker - "(?)" který na hover vysvětlí toggle významy. */
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "(?)" );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::TextUnformatted ( _( "Profiler panel controls" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s",
            _( "Start/Stop: toggle profiler subsystem (turns callstack ON if needed)" ) );
        ImGui::BulletText ( "%s",
            _( "Reset: clear aggregator + baseline cycle counter" ) );
        ImGui::BulletText ( "%s",
            _( "Filter: substring match against function name and hex address" ) );
        ImGui::BulletText ( "%s",
            _( "CALL/RST/IRQ/NMI: hide rows by entry kind" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s",
            _( "Column header click: sort by column" ) );
        ImGui::EndTooltip ( );
    };
}


/* ========================================================================= */
/*  Render - tabulka                                                         */
/* ========================================================================= */

/**
 * @brief Vykreslí sortable tabulku s filtrovanými řádky.
 *
 * Sloupce: Function / Addr / Kind / Calls / Excl% / Excl / Incl / Avg /
 * Min / Max. Sort přes ImGui table sort spec API (jeden sloupec).
 */
static void render_table ( void )
{
    if ( !g_pf.valid ) {
        ImGui::TextDisabled ( "%s", _( "(no data)" ) );
        return;
    };
    if ( g_pf.entries.empty ( ) ) {
        ImGui::TextDisabled ( "%s",
            _( "Aggregator is empty (profiler inactive or no calls recorded)" ) );
        return;
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders
                          | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_Resizable
                          | ImGuiTableFlags_Reorderable
                          | ImGuiTableFlags_Hideable
                          | ImGuiTableFlags_Sortable
                          | ImGuiTableFlags_SortMulti       /* tie-break přes addr */
                          | ImGuiTableFlags_SizingStretchProp;

    if ( !ImGui::BeginTable ( "###pf_table", 10, flags ) ) {
        return;
    };

    /* Šířky sloupců dle očekávaného obsahu + aktuální velikost fontu.
     * Padding +16 px = 2× CellPadding (~8 px) pro pravo-zarovnaný text.
     *
     * - w_func: typický sym název (`monitor_print_string` ~ 20 znaků).
     *   Function je rezizovatelný (Resizable flag), uživatel může
     *   rozšířit pro delší jména.
     * - w_hex/w_kind: pevný 4-hex addr + nejdelší kind string.
     * - w_count: 6-místné číslo pro Calls/Avg/Min/Max (typicky stovky
     *   až desítky tisíc volání / krátkých cyklů).
     * - w_cyc: 9-místné pro Excl/Incl kumulativní cycles (~1G cycles
     *   = ~5 minut běhu při 3.5 MHz, dostatečné pro typické profiling
     *   session). Pro delší běhy uživatel sloupec rozšíří.
     * - w_pct: "100.00%" max 7 znaků. */
    const float w_func  = ImGui::CalcTextSize ( "monitor_print_string" ).x + 16.0f;
    const float w_hex   = ImGui::CalcTextSize ( "FFFFh" ).x + 16.0f;
    const float w_kind  = ImGui::CalcTextSize ( "IRQ-IM2" ).x + 16.0f;
    const float w_count = ImGui::CalcTextSize ( "999999" ).x + 16.0f;
    const float w_cyc   = ImGui::CalcTextSize ( "999999999" ).x + 16.0f;
    const float w_pct   = ImGui::CalcTextSize ( "100.00%" ).x + 16.0f;

    ImGui::TableSetupScrollFreeze ( 0, 1 );  /* Sticky header. */
    ImGui::TableSetupColumn ( "Function", ImGuiTableColumnFlags_WidthFixed,
                              w_func, 0 );
    ImGui::TableSetupColumn ( "Addr",     ImGuiTableColumnFlags_WidthFixed,
                              w_hex, 1 );
    ImGui::TableSetupColumn ( "Kind",     ImGuiTableColumnFlags_WidthFixed,
                              w_kind, 2 );
    ImGui::TableSetupColumn ( "Calls",    ImGuiTableColumnFlags_WidthFixed,
                              w_count, 3 );
    ImGui::TableSetupColumn ( "Excl%",    ImGuiTableColumnFlags_WidthFixed
                              | ImGuiTableColumnFlags_DefaultSort,
                              w_pct, 4 );
    ImGui::TableSetupColumn ( "Excl",     ImGuiTableColumnFlags_WidthFixed,
                              w_cyc, 5 );
    ImGui::TableSetupColumn ( "Incl",     ImGuiTableColumnFlags_WidthFixed,
                              w_cyc, 6 );
    ImGui::TableSetupColumn ( "Avg",      ImGuiTableColumnFlags_WidthFixed,
                              w_count, 7 );
    ImGui::TableSetupColumn ( "Min",      ImGuiTableColumnFlags_WidthFixed,
                              w_count, 8 );
    ImGui::TableSetupColumn ( "Max",      ImGuiTableColumnFlags_WidthFixed,
                              w_count, 9 );

    /* Ruční render hlaviček aby šlo přidat hover tooltipy s vysvětlením
     * (= klasické pojmy Excl / Incl / Unmatched / Buckets nejsou vždy
     * známé). Per sloupec: TableSetColumnIndex + TableHeader + hover
     * detekce. */
    static const char *const k_col_tooltips[ 10 ] = {
        /* 0 Function */
        N_( "Symbol name from sym_db (NoICE / .map / .sym / .lbl).\n"
            "Fallback 'func_XXXX' = no symbol loaded for that addr.\n"
            "Tip: load .sym BEFORE Start for named functions." ),
        /* 1 Addr */
        N_( "Entry address of the function (CALL/RST/IRQ target)." ),
        /* 2 Kind */
        N_( "Origin of the entry:\n"
            "  CALL    = CALL nn / CALL cc,nn\n"
            "  RST     = RST n*8 (1-byte opcode)\n"
            "  IRQ-IM0 = IRQ accept in IM 0\n"
            "  IRQ-IM1 = IRQ accept in IM 1 (= RST 38h)\n"
            "  IRQ-IM2 = IRQ accept in IM 2 (vector dispatch)\n"
            "  NMI     = NMI accept (= CALL 0066h)" ),
        /* 3 Calls */
        N_( "Number of paired on_enter events for this bucket\n"
            "since last Reset." ),
        /* 4 Excl% */
        N_( "Exclusive cycles as percentage of total Exclusive sum.\n"
            "Sort here to find HOT SPOTS - functions that actually\n"
            "burn CPU time (excluding nested calls)." ),
        /* 5 Excl */
        N_( "Exclusive cycles = T-states spent ONLY in this function,\n"
            "NOT counting nested calls.\n"
            "Excl = Incl - sum(Incl of children).\n"
            "Excl <= Incl always. Leaf function: Excl == Incl." ),
        /* 6 Incl */
        N_( "Inclusive cycles = T-states from entry to RET,\n"
            "INCLUDING all nested calls (CALL/RST/IRQ).\n"
            "High Incl + low Excl = dispatcher function (calls a lot,\n"
            "does little itself)." ),
        /* 7 Avg */
        N_( "Average inclusive cycles per call = Incl / Calls." ),
        /* 8 Min */
        N_( "Shortest single inclusive call.\n"
            "For raster routines: Min ~= Max = stable timing.\n"
            "Max >> Min = occasional slow path." ),
        /* 9 Max */
        N_( "Longest single inclusive call." ),
    };

    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
    for ( int col = 0; col < 10; col++ ) {
        ImGui::TableSetColumnIndex ( col );
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::SetTooltip ( "%s", _( k_col_tooltips[ col ] ) );
        };
    };

    /* Sort - aplikujeme každý frame, protože rebuild_filtered_idx
     * (volaný v dbg_profiler_render každý frame, vč. throttle else
     * větve) zahodí předchozí sort tím že přebuduje pole v default
     * insertion order. Bez per-frame apply by ImGui zobrazil šipku
     * ale data by zůstala nesortovaná (= jen frame po kliknutí by
     * bylo correct). Náklad: std::sort přes stovky entries jednotky
     * mikrosekund - zanedbatelné při 60 FPS UI. */
    ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs ( );
    if ( sort_specs ) {
        apply_sort ( sort_specs );
        sort_specs->SpecsDirty = false;
    };

    /* Total excl pro % výpočet (z filtrovaného setu). */
    uint64_t total_excl = 0;
    for ( int idx : g_pf.filtered_idx ) {
        total_excl += g_pf.entries[ idx ].cycles_excl_sum;
    };

    /* Render řádků. */
    for ( int idx : g_pf.filtered_idx ) {
        const st_PROF_ENTRY &e = g_pf.entries[ idx ];

        ImGui::TableNextRow ( );

        /* Function name jako Selectable přes celý řádek (= clickable
         * row). PushID per řádek zajistí stabilní ID pro popup. */
        ImGui::TableSetColumnIndex ( 0 );
        ImGui::PushID ( (int) e.target_addr ^ ( (int) e.kind << 16 ) );

        const st_SYMBOL *sym = sym_db_lookup_by_addr (
            (uint32_t) e.target_addr, 0 );
        char label[ 96 ];
        if ( sym && sym->name && sym->name[ 0 ] ) {
            snprintf ( label, sizeof ( label ), "%s", sym->name );
        }
        else {
            snprintf ( label, sizeof ( label ), "func_%04X", e.target_addr );
        };

        bool clicked = ImGui::Selectable (
            label, false,
            ImGuiSelectableFlags_SpanAllColumns );

        if ( clicked ) {
            /* Levý klik → Disassembly #1 na target adrese. */
            dbg_disasm_show_in_slot ( 0, e.target_addr );
        };

        /* Hover tooltip - vysvětlí click akce + detaily funkce. */
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::BeginTooltip ( );
            ImGui::TextUnformatted ( label );
            ImGui::Separator ( );
            ImGui::Text ( "%s", _( "Left click: open in Disassembly #1" ) );
            ImGui::Text ( "%s", _( "Right click: more actions (BP, Bookmark, Disasm #2-5)" ) );
            ImGui::Separator ( );
            ImGui::Text ( "Addr:    %04Xh", e.target_addr );
            ImGui::Text ( "Kind:    %s", kind_to_str ( e.kind ) );
            ImGui::Text ( "Calls:   %llu", (unsigned long long) e.calls );
            ImGui::Text ( "Excl:    %llu T-states",
                          (unsigned long long) e.cycles_excl_sum );
            ImGui::Text ( "Incl:    %llu T-states",
                          (unsigned long long) e.cycles_incl_sum );
            if ( e.calls > 0 ) {
                ImGui::Text ( "Avg:     %llu T-states",
                              (unsigned long long) ( e.cycles_incl_sum / e.calls ) );
                ImGui::Text ( "Min/Max: %llu / %llu",
                              (unsigned long long) e.cycles_incl_min,
                              (unsigned long long) e.cycles_incl_max );
            };
            ImGui::EndTooltip ( );
        };

        /* Right-click context menu - BP, Bookmark, Disassembly #2..#5. */
        if ( ImGui::BeginPopupContextItem ( "###pf_row_ctx" ) ) {
            if ( ImGui::MenuItem ( _L ( "Set Breakpoint###pf_bp" ) ) ) {
                dbg_ui_bp_add ( e.target_addr, NULL );
            };
            if ( ImGui::MenuItem ( _L ( "Add Bookmark###pf_bm" ) ) ) {
                char bm_input[ 64 ];
                if ( sym && sym->name && sym->name[ 0 ] ) {
                    snprintf ( bm_input, sizeof ( bm_input ), "%s", sym->name );
                }
                else {
                    snprintf ( bm_input, sizeof ( bm_input ), "#%04X",
                               e.target_addr );
                };
                bookmarks_add ( bm_input, "" );
                if ( g_gui ) g_gui->showBookmarksWindow = true;
            };
            ImGui::Separator ( );
            /* Disassembly #2..#5 (slot 1..4). #1 = levý klik. */
            if ( ImGui::MenuItem ( _L ( "Show in Disassembly #2###pf_d2" ) ) ) {
                dbg_disasm_show_in_slot ( 1, e.target_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Show in Disassembly #3###pf_d3" ) ) ) {
                dbg_disasm_show_in_slot ( 2, e.target_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Show in Disassembly #4###pf_d4" ) ) ) {
                dbg_disasm_show_in_slot ( 3, e.target_addr );
            };
            if ( ImGui::MenuItem ( _L ( "Show in Disassembly #5###pf_d5" ) ) ) {
                dbg_disasm_show_in_slot ( 4, e.target_addr );
            };
            ImGui::EndPopup ( );
        };

        ImGui::PopID ( );

        /* Addr. */
        ImGui::TableSetColumnIndex ( 1 );
        ImGui::Text ( "%04Xh", e.target_addr );

        /* Kind. */
        ImGui::TableSetColumnIndex ( 2 );
        ImGui::TextUnformatted ( kind_to_str ( e.kind ) );

        /* Calls. */
        ImGui::TableSetColumnIndex ( 3 );
        ImGui::Text ( "%llu", (unsigned long long) e.calls );

        /* Excl%. */
        ImGui::TableSetColumnIndex ( 4 );
        double excl_pct = ( total_excl > 0 )
            ? ( 100.0 * (double) e.cycles_excl_sum / (double) total_excl )
            : 0.0;
        ImGui::Text ( "%.2f%%", excl_pct );

        /* Excl. */
        ImGui::TableSetColumnIndex ( 5 );
        ImGui::Text ( "%llu", (unsigned long long) e.cycles_excl_sum );

        /* Incl. */
        ImGui::TableSetColumnIndex ( 6 );
        ImGui::Text ( "%llu", (unsigned long long) e.cycles_incl_sum );

        /* Avg = incl / calls. */
        ImGui::TableSetColumnIndex ( 7 );
        if ( e.calls > 0 ) {
            ImGui::Text ( "%llu",
                          (unsigned long long) ( e.cycles_incl_sum / e.calls ) );
        }
        else {
            ImGui::TextDisabled ( "-" );
        };

        /* Min. */
        ImGui::TableSetColumnIndex ( 8 );
        if ( e.calls > 0 ) {
            ImGui::Text ( "%llu", (unsigned long long) e.cycles_incl_min );
        }
        else {
            ImGui::TextDisabled ( "-" );
        };

        /* Max. */
        ImGui::TableSetColumnIndex ( 9 );
        ImGui::Text ( "%llu", (unsigned long long) e.cycles_incl_max );
    };

    ImGui::EndTable ( );
}


/* ========================================================================= */
/*  Render - status bar                                                      */
/* ========================================================================= */

/**
 * @brief Vykreslí status bar (Running/Stopped + counters).
 */
static void render_status_bar ( void )
{
    if ( !g_pf.valid ) {
        ImGui::TextDisabled ( "%s", _( "(no data)" ) );
        return;
    };

    if ( g_pf.active ) {
        /* Zelená - běží. */
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 80, 220, 120, 255 ) );
        ImGui::TextUnformatted ( _( "● RUNNING" ) );
        ImGui::PopStyleColor ( );
    }
    else {
        ImGui::TextDisabled ( "%s", _( "○ STOPPED" ) );
    };

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "Total cycles: %llu",
                  (unsigned long long) g_pf.total_cycles_64 );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Total profiled T-states since last Reset.\n"
               "= sum of Excl across all buckets." ) );
    };

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "Calls: %llu", (unsigned long long) g_pf.total_calls );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Total on_enter events (CALL + RST + IRQ + NMI accepts)\n"
               "since last Reset. = sum of Calls column." ) );
    };

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "IRQ: %u", g_pf.irq_entries );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s",
            _( "How many of 'Calls' were IRQ or NMI accept events\n"
               "(= IM 0 / IM 1 / IM 2 / NMI, not CALL or RST)." ) );
    };

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "Unmatched: %u", g_pf.unmatched_returns );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s",
            _( "RET / RETI / RETN events with no paired CALL in shadow\n"
               "stack ('divergent returns').\n"
               "\n"
               "Causes:\n"
               "  - PUSH addr + RET trampoline (BDOS dispatch tables)\n"
               "  - longjmp pattern (restore SP and RET)\n"
               "  - self-modifying return address on stack\n"
               "  - ISR ending with RET instead of RETI\n"
               "\n"
               "Profiler skips these in aggregation (no paired enter).\n"
               "\n"
               "0 or a few = OK, data trustworthy.\n"
               "Growing steadily = multi-stack OS pattern (CP/M, NIPOS)\n"
               "= V1 measurements NOT accurate, waiting on callstack\n"
               "V1.5+ scope BPs." ) );
    };

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( "Buckets: %d", (int) g_pf.entries.size ( ) );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s",
            _( "Unique (target_addr, kind) entries in the aggregator.\n"
               "Each bucket = one row in the table.\n"
               "\n"
               "A bucket is created at first entry to that\n"
               "(address, kind) combination. Cleared only by Reset.\n"
               "\n"
               "Typical Z80 game / demo: tens to hundreds.\n"
               "Thousands and growing = SMC or banked code mixing\n"
               "into the same buckets, data may be misleading.\n"
               "\n"
               "Memory cost: ~64 B per bucket." ) );
    };

    if ( g_pf.overflow_count > 0 ) {
        ImGui::SameLine ( );
        ImGui::TextDisabled ( "|" );
        ImGui::SameLine ( );
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 255, 220, 80, 255 ) );
        ImGui::Text ( "Overflow: %u", g_pf.overflow_count );
        ImGui::PopStyleColor ( );
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::SetTooltip ( "%s",
                _( "Parallel stack push beyond PROFILER_MAX_DEPTH (256).\n"
                   "Extreme nesting depth - shouldn't happen in normal Z80\n"
                   "code. Investigate or Reset." ) );
        };
    };
}


/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

extern "C" void dbg_profiler_render ( void )
{
    /* Refresh throttle 250 ms (= 4 Hz polling). */
    double now = ImGui::GetTime ( );
    if ( ( now - g_pf.last_refresh_time_s ) > 0.25 ) {
        g_pf.last_refresh_time_s = now;
        profiler_refresh ( );
        /* Refresh změnil entries vector → invalidate filtered_idx. */
        rebuild_filtered_idx ( );
        /* Re-sort přes aktuální sort spec (table sort spec API je validní
         * jen uvnitř BeginTable/EndTable bloku → necháme apply_sort běžet
         * v render_table přes SpecsDirty trigger). Tady ho jen vynulujem
         * a render_table si všimne empty filtered_idx → re-applikuje. */
    }
    else {
        /* Filter / Kind / IRQ toggles se mohly změnit i bez refresh -
         * přepočítej viditelnou množinu každý frame (low cost - max
         * stovky entries). */
        rebuild_filtered_idx ( );
    };

    render_toolbar ( );
    ImGui::Separator ( );

    /* Tabulka v dostupné ploše (status bar nechá místo na 1 řádek). */
    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float footer_h = ImGui::GetTextLineHeightWithSpacing ( )
                   + ImGui::GetStyle ( ).ItemSpacing.y * 2.0f;
    float table_h  = avail.y - footer_h;
    if ( table_h < 60.0f ) table_h = 60.0f;

    ImGui::BeginChild ( "###pf_table_child",
                        ImVec2 ( 0.0f, table_h ),
                        false, ImGuiWindowFlags_None );
    render_table ( );
    ImGui::EndChild ( );

    ImGui::Separator ( );
    render_status_bar ( );

    /* F4: CSV export dialog + error popup (na top-level okna, ne uvnitř
     * BeginChild). Display se volá každý frame dokud uživatel dialog
     * nezavře - state drží g_pf.export_dialog_open. */
    render_export_dialog ( );

    /* Error popup queue (= fopen / fwrite selhal v render_export_dialog).
     * OpenPopup MUSÍ být volaný ve stejném ID-stacku jako BeginPopupModal,
     * proto je tady na úrovni okna profileru. */
    if ( g_pf.export_error_popup_queue ) {
        g_pf.export_error_popup_queue = false;
        ImGui::OpenPopup ( "###pf_export_error" );
    };
    if ( ImGui::BeginPopupModal ( _L ( "Export error###pf_export_error" ),
                                   NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextUnformatted ( g_pf.export_error_msg );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "OK###pf_export_error_ok" ),
                              ImVec2 ( 120.0f, 0.0f ) ) )
        {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
