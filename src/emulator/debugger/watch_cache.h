/*
 * File:   watch_cache.h
 *
 * Watch panel - per-row cache (Phase E):
 *   - prev value pro change detection + color fade (E.1)
 *   - min/max int + change_count tracking (E.3)
 *   - snapshot baseline pro delta sloupec (E.2)
 *
 * Modul je samostatny od `watch.c` aby UI nemuselo drzet static cache
 * pouze v CPP. Backend API umoznuje Unity testy nezavisle na UI.
 *
 * Threading: jen UI vlakno (= zadne thread-safe primitivy). API se vola
 * z watch_window render loop + z bulk reset hookov (emu reset detection).
 *
 * Cache je klicovana stable `st_WATCH_ROW.id` (= survives reorder/delete).
 *
 * Lifetime:
 *   - watch_cache_init() pri starte programu (debugger_init).
 *   - watch_cache_shutdown() pri exit.
 *   - watch_cache_update(...) jednou per frame per radek.
 *   - watch_cache_reset_one / reset_all pri reset triggers.
 *   - watch_cache_prune_stale(live_ids, n) jednou per frame =
 *     drop entries pro id ktere nejsou v `live_ids`.
 *
 * Snapshot baseline (E.2):
 *   - Pamatuje hodnotu (type + bytes/int) pri watch_snapshot_capture.
 *   - watch_snapshot_get(row_id) vraci ulozeny stav nebo NULL.
 *   - watch_snapshot_clear() / watch_snapshot_active() pro UI logiku.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later
 *
 * ---------------------------------------------------------------------------
 */

#ifndef WATCH_CACHE_H
#define WATCH_CACHE_H

#include "mzarch/mzcommon_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "debugger/watch.h"  /* en_WATCH_TYPE, en_WATCH_MODE, WATCH_LENGTH_MAX_BYTES */


/**
 * @brief Smer/druh poslední změny pro UI highlight.
 */
typedef enum en_WATCH_DELTA_DIR {
    WATCH_DELTA_NONE   = 0,   /**< Zadna zmena od trackovani / init */
    WATCH_DELTA_UP     = 1,   /**< Hodnota vzrostla (int signed compare) */
    WATCH_DELTA_DOWN   = -1,  /**< Hodnota klesla */
    WATCH_DELTA_STRING = 2,   /**< String/bytes se zmenily (bez +/- semantiky) */
} en_WATCH_DELTA_DIR;


/**
 * @brief Snimek hodnoty radku (pro min/max + change tracking + snapshot
 *        baseline).
 *
 * Pole `bytes` se pouziva pro string/bytes typy. Pole `value_int` pro
 * fixed-size int typy.
 *
 * @invariant `len` <= WATCH_LENGTH_MAX_BYTES.
 * @invariant `type` = en_WATCH_TYPE.
 */
typedef struct st_WATCH_VALUE_SNAP {
    en_WATCH_TYPE type;                       /**< Typ v okamziku zachyceni */
    uint64_t      value_int;                  /**< Surova hodnota pro int typy */
    uint8_t       bytes[ WATCH_LENGTH_MAX_BYTES ]; /**< Bajty pro string/bytes */
    size_t        len;                        /**< Pocet validnich bajtu */
} st_WATCH_VALUE_SNAP;


/**
 * @brief Read-only view do cache entry. Vlastni cache, caller jen cte.
 *
 * Lifetime: validni do nejblizsiho `watch_cache_update` /
 * `watch_cache_reset_*` / `watch_cache_prune_stale` / `watch_cache_shutdown`.
 */
typedef struct st_WATCH_CACHE_VIEW {
    bool      valid;             /**< false = jeste neinicializovany */
    int       type_snap;         /**< en_WATCH_TYPE pri poslednim update */
    int       mode_snap;         /**< en_WATCH_MODE pri poslednim update */
    en_WATCH_DELTA_DIR delta_dir;/**< Smer posledni zmeny */
    int       last_change_frame; /**< Frame index pri posledni zmene */
    uint64_t  prev_int;          /**< Cache int hodnoty z minuleho update */
    size_t    prev_len;          /**< Delka prev_bytes */
    int64_t   min_int;           /**< Min signed hodnota (jen int typy) */
    int64_t   max_int;           /**< Max signed hodnota (jen int typy) */
    uint64_t  change_count;      /**< Pocet zmen od poslední init/reset */
    bool      min_max_valid;     /**< false = string typy nebo neinicializovano */
} st_WATCH_CACHE_VIEW;


/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */


/**
 * @brief Inicializuje cache modul. Idempotentni.
 *
 * Volat z debugger_init po watch_init.
 */
extern void watch_cache_init ( void );


/**
 * @brief Uvolni vsechny cache entries + snapshot buffer.
 *
 * Volat z debugger_exit pred watch_shutdown.
 */
extern void watch_cache_shutdown ( void );


/* ===================================================================== */
/*  Sign-extend helper (testovany)                                       */
/* ===================================================================== */


/**
 * @brief Sign-extend surovou hodnotu z `watch_read_int` podle typu na
 *        signed int64.
 *
 * Pro unsigned typy = vstup (nezaporna). Pro signed extenduje MSB.
 *
 * @param t  Typ radku.
 * @param v  Surova hodnota.
 * @return Signed int64.
 */
extern int64_t watch_cache_value_to_signed ( en_WATCH_TYPE t, uint64_t v );


/* ===================================================================== */
/*  Update / Get / Reset                                                 */
/* ===================================================================== */


/**
 * @brief Update cache entry pro radek `row_id`. Volat jednou per frame
 *        per radek.
 *
 * Pro int typy: `value_int` se cte, min/max/change_count se updatuje.
 * Pro string/bytes typy: `bytes` + `len` se porovnava memcmp, delta_dir
 * se nastavi na WATCH_DELTA_STRING pri zmene.
 *
 * Type nebo mode change = invalidate prev (= zadny highlight, reset
 * min/max).
 *
 * @param row_id      Stable id radku (>= 1).
 * @param type        Aktualni typ.
 * @param mode        Aktualni mode (Address / Expr_*).
 * @param value_int   Surova int hodnota (pro int typy).
 * @param bytes       Buffer s precterymi bajty (pro string/bytes typy, NULL OK).
 * @param len         Pocet validnich bajtu (0 pro int typy).
 * @param frame_no    ImGui::GetFrameCount() z UI loopu.
 */
extern void watch_cache_update ( int row_id,
                                  en_WATCH_TYPE type,
                                  en_WATCH_MODE mode,
                                  uint64_t value_int,
                                  const uint8_t *bytes,
                                  size_t len,
                                  int frame_no );


/**
 * @brief Vrati read-only view do cache entry.
 *
 * @param row_id  Stable id radku.
 * @return Pointer na view nebo NULL pokud entry neexistuje.
 *
 * Lifetime: validni do nejblizsiho update/reset/prune.
 */
extern const st_WATCH_CACHE_VIEW *watch_cache_get ( int row_id );


/**
 * @brief Reset cache pro jeden radek (= prev invalidate, min/max reset,
 *        change_count na 0).
 *
 * @param row_id  Stable id (< 0 = no-op).
 */
extern void watch_cache_reset_one ( int row_id );


/**
 * @brief Reset cache pro vsechny radky. Vola se pri emu reset detection.
 */
extern void watch_cache_reset_all ( void );


/**
 * @brief Smaze cache entries pro id ktere nejsou v `live_ids`.
 *
 * @param live_ids  Pole platnych id (= aktualne registrovane radky).
 * @param n         Pocet polozek.
 */
extern void watch_cache_prune_stale ( const int *live_ids, size_t n );


/* ===================================================================== */
/*  Snapshot baseline (Phase E.2)                                        */
/* ===================================================================== */


/**
 * @brief Ulozi soucasnou hodnotu vsech radku jako snapshot baseline.
 *
 * UI volac iteruje vsechny radky a vola `watch_snapshot_put` per radek.
 * Tato funkce nezabere - cely workflow je v `watch_snapshot_put` +
 * priznak `watch_snapshot_active` se prepne pri prvním put.
 *
 * Pred capture si volac zavola `watch_snapshot_clear` aby vyrobil cisty
 * stav.
 *
 * @param frame_no  Frame index pro status ("Snapshot @ frame N").
 */
extern void watch_snapshot_begin ( int frame_no );


/**
 * @brief Ulozi snapshot pro jeden radek.
 *
 * @param row_id     Stable id.
 * @param type       Typ v okamziku capture.
 * @param value_int  Surova int hodnota.
 * @param bytes      Buffer bajtu (pro string/bytes typy, NULL OK).
 * @param len        Pocet bajtu.
 */
extern void watch_snapshot_put ( int row_id,
                                  en_WATCH_TYPE type,
                                  uint64_t value_int,
                                  const uint8_t *bytes,
                                  size_t len );


/**
 * @brief Vraci snapshot entry pro radek nebo NULL.
 *
 * @param row_id  Stable id.
 * @return Pointer (caller jen cte, lifetime do snapshot_clear / shutdown).
 */
extern const st_WATCH_VALUE_SNAP *watch_snapshot_get ( int row_id );


/**
 * @brief Vyprazdni snapshot buffer + deaktivuje feature.
 */
extern void watch_snapshot_clear ( void );


/**
 * @brief Smaze snapshot entry pro konkretni radek (= cleanup pri
 *        watch_remove).
 *
 * @param row_id  Stable id.
 */
extern void watch_snapshot_remove ( int row_id );


/**
 * @brief Vrati true pokud je aktivni snapshot baseline (= alespon
 *        jeden put od posledniho clear).
 */
extern bool watch_snapshot_active ( void );


/**
 * @brief Vrati frame_no posledniho `watch_snapshot_begin` (= status).
 *
 * @return Frame index, 0 pokud snapshot neaktivni.
 */
extern int watch_snapshot_frame ( void );


#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* WATCH_CACHE_H */
