/**
 * @file   dbg_coverage_data.cpp
 * @brief  Data pokrytí kódu pro okna GUI (viz dbg_coverage_data.h).
 *
 * Dvě kopie dat: lehká (bez jednotek, pro značky) a plná (s jednotkami
 * a seřazenými řádky, pro tabulku a tooltip). Plná kopie se obnoví jen
 * tehdy, když se od minula změnil počet instrukcí, generace dat nebo
 * epocha, protože tabulka jednotek může mít až 32 MiB.
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>
#include <algorithm>

#include <glib.h>

#include "i18n.h"

#include "dbg_coverage_data.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

extern "C" {
#include "emulator/debugger/dbgapi_ui.h"
}

/* g_dbgapi_cmdrq_queue je definovaný v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


namespace {

/** @brief Limit vyzvednutí periodického snímku (ms); při běhu emu vlákno vyzvedá frontu na konci snímku. */
const int k_poll_timeout_ms = 50;

/** @brief Limit vyzvednutí operace a exportu (ms). */
const int k_op_timeout_ms = 1000;

/**
 * @brief Stav kopií dat (jen UI vlákno).
 *
 * @invariant rows ukazují do full (jeho pole units); full.units vlastní
 *            tento modul (coverage_snapshot_clear()).
 * @invariant light.units == NULL.
 */
struct CovData {
    st_COVERAGE_SNAPSHOT light {};   /**< Lehká kopie: stav, bitmapy, epochy. */
    bool light_valid = false;        /**< light obsahuje platnou kopii. */
    double light_time = -1.0;        /**< Čas poslední žádosti o lehkou kopii (s, monotónní), < 0 = hned. */

    st_COVERAGE_SNAPSHOT full {};    /**< Plná kopie i s jednotkami. */
    bool full_valid = false;         /**< full obsahuje platnou kopii. */
    bool full_dirty = false;         /**< Po operaci: plnou kopii obnovit i beze změny počtu instrukcí. */
    double full_time = -1.0;         /**< Čas poslední žádosti o plnou kopii, < 0 = hned. */
    uint32_t full_serial = 0;        /**< Pořadové číslo plné kopie. */

    st_COVERAGE_ROW *rows = NULL;    /**< Řádky full (g_free). */
    size_t n_rows = 0;               /**< Počet řádků. */
};

CovData *g_cd = NULL;


/** @brief Vrátí stav modulu, při prvním volání ho založí (snímky jsou velké, ne na zásobník). */
CovData &cd ( void )
{
    if ( !g_cd ) g_cd = new CovData ( );
    return *g_cd;
}


/**
 * @brief Vyžádá snímek přes dbgapi.
 * @param snap       Cíl (předchozí jednotky musí být uvolněné).
 * @param with_units Kopírovat i jednotky úrovně detail.
 * @param timeout_ms Limit vyzvednutí.
 * @param err        Výstup chyby (smí být NULL).
 * @param err_size   Velikost err.
 * @return true = snímek je platný.
 */
bool take_snapshot ( st_COVERAGE_SNAPSHOT *snap, bool with_units, int timeout_ms,
                     char *err, size_t err_size )
{
    st_DBGAPI_COVERAGE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = DBGAPI_COVERAGE_OP_SNAPSHOT;
    p.snap = snap;
    p.snap_without_units = !with_units;
    if ( dbg_ui_coverage ( &p, timeout_ms ) ) return true;
    /* Neúspěch po vyzvednutí mohl jednotky přesto alokovat. */
    coverage_snapshot_clear ( snap );
    if ( err && err_size ) {
        g_strlcpy ( err, p.out_error[ 0 ] ? p.out_error : _( "The emulator did not respond" ), err_size );
    };
    return false;
}


/**
 * @brief Plná kopie je zastaralá vůči lehké.
 * @return true = od plné kopie přibyly instrukce, změnila se generace,
 *         epocha nebo běh sběru.
 */
bool full_outdated ( const CovData &d )
{
    if ( !d.full_valid || d.full_dirty ) return true;
    if ( !d.light_valid ) return false;
    const st_COVERAGE_STATUS &a = d.light.status;
    const st_COVERAGE_STATUS &b = d.full.status;
    return a.instructions != b.instructions || a.generation != b.generation
        || a.epoch != b.epoch || a.active != b.active || a.has_data != b.has_data;
}


/**
 * @brief Obnoví plnou kopii a řádky.
 *
 * Při neúspěchu (plná fronta, timeout, nedostatek paměti) zůstane
 * předchozí kopie i řádky.
 */
void refresh_full ( CovData &d )
{
    st_COVERAGE_SNAPSHOT *tmp = (st_COVERAGE_SNAPSHOT *) g_malloc0 ( sizeof ( *tmp ) );
    if ( take_snapshot ( tmp, true, k_poll_timeout_ms, NULL, 0 ) ) {
        g_free ( d.rows );
        coverage_snapshot_clear ( &d.full );
        memcpy ( &d.full, tmp, sizeof ( d.full ) );   /* převezme i pole units */
        d.full_valid = true;
        d.full_dirty = false;
        d.rows = coverage_rows_build ( &d.full, &d.n_rows );
        d.full_serial++;
    };
    g_free ( tmp );
}

} /* namespace */


extern "C" void dbg_cov_data_poll ( bool need_units )
{
    CovData &d = cd ( );
    const double now = (double) g_get_monotonic_time ( ) / 1e6;
    /* g_coverage_active mění jen emu vlákno; čtení z UI vlákna je jen
     * nápověda pro interval (stejně jako příznaky čtené jinými okny). */
    const double light_interval = g_coverage_active ? DBG_COV_DATA_LIGHT_INTERVAL_S
                                                    : DBG_COV_DATA_IDLE_INTERVAL_S;
    if ( d.light_time < 0.0 || ( now - d.light_time ) >= light_interval ) {
        if ( !dbgapi_ui_queue_is_full ( &g_dbgapi_cmdrq_queue ) ) {
            d.light_time = now;
            st_COVERAGE_SNAPSHOT *tmp = (st_COVERAGE_SNAPSHOT *) g_malloc0 ( sizeof ( *tmp ) );
            if ( take_snapshot ( tmp, false, k_poll_timeout_ms, NULL, 0 ) ) {
                memcpy ( &d.light, tmp, sizeof ( d.light ) );
                d.light_valid = true;
            };
            g_free ( tmp );
        };
    };
    if ( !need_units ) return;
    if ( d.full_time >= 0.0 && ( now - d.full_time ) < DBG_COV_DATA_FULL_INTERVAL_S ) return;
    if ( !full_outdated ( d ) ) return;
    if ( dbgapi_ui_queue_is_full ( &g_dbgapi_cmdrq_queue ) ) return;
    d.full_time = now;
    refresh_full ( d );
}


extern "C" void dbg_cov_data_invalidate ( void )
{
    CovData &d = cd ( );
    d.light_time = -1.0;
    d.full_time = -1.0;
    /* Plná kopie se po operaci obnoví i bez změny počtu instrukcí
     * (například reset v pauze). */
    d.full_dirty = true;
}


extern "C" bool dbg_cov_data_valid ( void )
{
    return cd ( ).light_valid;
}


extern "C" const st_COVERAGE_STATUS *dbg_cov_data_status ( void )
{
    return &cd ( ).light.status;
}


extern "C" unsigned dbg_cov_data_pc_state ( uint16_t pc )
{
    const CovData &d = cd ( );
    if ( !d.light_valid || !d.light.status.has_data ) return 0;
    return coverage_pc_state ( &d.light, pc );
}


extern "C" const st_COVERAGE_SNAPSHOT *dbg_cov_data_full ( void )
{
    const CovData &d = cd ( );
    return d.full_valid ? &d.full : NULL;
}


extern "C" const st_COVERAGE_ROW *dbg_cov_data_rows ( size_t *n )
{
    const CovData &d = cd ( );
    if ( n ) *n = d.n_rows;
    return d.rows;
}


extern "C" uint32_t dbg_cov_data_full_serial ( void )
{
    return cd ( ).full_serial;
}


extern "C" size_t dbg_cov_data_rows_for_pc ( uint16_t pc, size_t *first )
{
    const CovData &d = cd ( );
    if ( first ) *first = 0;
    if ( !d.rows || d.n_rows == 0 ) return 0;
    const st_COVERAGE_ROW *b = d.rows;
    const st_COVERAGE_ROW *e = d.rows + d.n_rows;
    const st_COVERAGE_ROW *lo = std::lower_bound ( b, e, pc,
        [] ( const st_COVERAGE_ROW &r, uint16_t v ) { return r.pc < v; } );
    const st_COVERAGE_ROW *hi = std::upper_bound ( lo, e, pc,
        [] ( uint16_t v, const st_COVERAGE_ROW &r ) { return v < r.pc; } );
    if ( first ) *first = (size_t) ( lo - b );
    return (size_t) ( hi - lo );
}


extern "C" bool dbg_cov_data_op ( en_DBGAPI_COVERAGE_OP op, const st_COVERAGE_CONFIG *cfg, bool reset,
                                  char *err, size_t err_size )
{
    st_DBGAPI_COVERAGE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = op;
    if ( cfg ) p.cfg = *cfg;
    p.reset = reset;
    bool ok = dbg_ui_coverage ( &p, k_op_timeout_ms );
    if ( err && err_size ) {
        if ( ok ) err[ 0 ] = '\0';
        else g_strlcpy ( err, p.out_error[ 0 ] ? p.out_error : _( "The emulator did not respond" ), err_size );
    };
    dbg_cov_data_invalidate ( );
    return ok;
}


extern "C" bool dbg_cov_data_export ( const char *path, bool csv, size_t *out_rows, char *err, size_t err_size )
{
    st_COVERAGE_SNAPSHOT *s = (st_COVERAGE_SNAPSHOT *) g_malloc0 ( sizeof ( *s ) );
    bool ok = take_snapshot ( s, true, k_op_timeout_ms, err, err_size );
    if ( ok && !s->status.has_data ) {
        if ( err && err_size ) g_strlcpy ( err, _( "No coverage data" ), err_size );
        ok = false;
    };
    if ( ok ) {
        char *e = NULL;
        ok = coverage_export_write ( s, path, csv, out_rows, &e );
        if ( !ok && err && err_size ) g_strlcpy ( err, e ? e : _( "Export failed" ), err_size );
        g_free ( e );
    };
    coverage_snapshot_clear ( s );
    g_free ( s );
    return ok;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
